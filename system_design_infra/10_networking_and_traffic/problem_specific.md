# Problem-Specific Details — Networking & Traffic (10_networking_and_traffic)

---

## api_gateway_design

### Unique Stack
- Data plane: Envoy proxy (or compatible); plugins compiled as shared libs or executed as ext_authz/ext_proc gRPC services
- Control plane: custom Route Compiler service; outputs Envoy xDS snapshots; serves via gRPC ADS (Aggregated Discovery Service)
- Rate limit store: Redis Cluster; 320 MB for 50K consumers × 100 routes
- Config store: PostgreSQL for routes/upstreams/consumers/api_keys/certificates
- Plugin pipeline (ordered): TLS Termination → Auth → RateLimit → Transform → CircuitBreak → Router → Upstream

### Key Algorithms / Design Decisions

**Route Matching Engine (Two-Phase Host Hash + Radix Trie):**
```
Phase 1: Host Resolution
  HashMap<Host, Trie> — O(1) lookup; "*" wildcard fallback
Phase 2: Path Matching (Radix Trie)
  /api/v1/users → prefix match; {id} → parameterized segment
Phase 3: Header + Method Refinement
  Filter by method + headers; select highest priority route
Phase 4: Regex Fallback (only if no trie match)
  Iterate regex routes in priority order; must pass RE2 safety check
Overall: O(1) + O(k) where k = path depth
```

**Distributed Rate Limiting (Sliding Window + Local Pre-allocation):**
```lua
current_window = floor(now / window_size)
previous_window = current_window - 1
elapsed = now - current_window * window_size
overlap = 1 - (elapsed / window_size)

effective_count = redis.GET(prev_window) * overlap + redis.INCR(curr_window)
if effective_count > limit:
    return RATE_LIMITED

-- Hybrid optimization:
-- Local token bucket per node with pre_allocated = global_limit / num_nodes
-- Refill rate = global_rate / num_nodes
-- Redis check only when local bucket exhausted (~10% of requests)
```

**JWT Validation Pipeline:**
```
1. Decode header → extract alg (RS256/ES256) + kid
2. Fetch JWKS from cache (refresh if kid unknown, background rotation)
3. Verify signature using public key
4. Check claims: exp, nbf, iss, aud
5. Check revocation bloom filter (in-memory, refreshed every 60s)
6. Extract claims → set X-User-Id, X-Roles headers downstream
```

**API Key Lookup:**
- key format: `<8-char prefix>.<remaining body>`; index on `key_prefix` (B-tree); SHA-256 hash stored, never plaintext
- Lookup: `WHERE key_prefix = ? AND key_hash = SHA256(?)` — two-column filter; local LRU cache before Redis before DB

### Key Tables
```sql
routes (
  id UUID PRIMARY KEY,
  name VARCHAR(255) UNIQUE,
  host VARCHAR(255),
  path_prefix VARCHAR(512),
  path_regex VARCHAR(512),
  methods VARCHAR[],
  headers JSONB,
  upstream_id UUID REFERENCES upstreams(id),
  strip_prefix BOOLEAN,
  priority INT,
  enabled BOOLEAN,
  created_at TIMESTAMP,
  updated_at TIMESTAMP,
  version BIGINT,
  INDEX idx_routes_host_path (host, path_prefix)
);

upstreams (
  id UUID PRIMARY KEY,
  name VARCHAR(255) UNIQUE,
  algorithm ENUM('round_robin','least_conn','consistent_hash'),
  hash_on ENUM('header','cookie','ip'),
  health_check JSONB,
  connect_timeout INT,   -- ms
  read_timeout INT,
  write_timeout INT,
  retries INT,
  circuit_breaker JSONB
);

consumers (
  id UUID PRIMARY KEY,
  username VARCHAR(255) UNIQUE,
  custom_id VARCHAR(255) UNIQUE,
  tags VARCHAR[],
  rate_limit JSONB   -- {requests_per_second, burst}
);

api_keys (
  id UUID PRIMARY KEY,
  consumer_id UUID REFERENCES consumers(id),
  key_hash VARCHAR(64),   -- SHA-256, never plaintext
  key_prefix VARCHAR(8),  -- indexed for fast lookup
  scopes VARCHAR[],
  expires_at TIMESTAMP,
  enabled BOOLEAN,
  created_at TIMESTAMP,
  INDEX idx_api_keys_prefix (key_prefix)
);

certificates (
  id UUID PRIMARY KEY,
  sni VARCHAR[],          -- GIN index for multi-domain certs
  cert_pem TEXT,
  key_pem_encrypted TEXT,
  expires_at TIMESTAMP,
  created_at TIMESTAMP,
  INDEX idx_certs_sni USING GIN (sni)
);
```

### NFRs
- 900,000 RPS peak (external 500K + internal 400K)
- 50,000 registered API keys; 2,000 microservices routed
- 100K+ concurrent HTTP/2 connections per node
- Latency p50: < 1 ms passthrough (no auth); p99: < 2 ms passthrough; < 10 ms with Redis rate limit check
- Availability: 99.999%
- Config propagation: < 1 s (xDS snapshot push)

---

## dns_at_scale

### Unique Stack
- CoreDNS with Kubernetes plugin (LIST+WATCH informer against K8s API)
- NodeLocal DNSCache DaemonSet on every node at 169.254.25.10; 10K records, 30 s TTL
- autopath plugin: server-side search domain expansion (1 round-trip instead of 4)
- ExternalDNS Controller: syncs K8s Services/Ingress → Route53/Cloud DNS
- Upstream resolvers: 8.8.8.8, 1.1.1.1, or internal resolvers for external domains
- DNSSEC optional; split-horizon DNS for internal-only records

### Key Algorithms / Design Decisions

**ndots Query Resolution Algorithm:**
```
Default ndots=5 (Kubernetes default):
  "svc-B" (0 dots) → 1 query: svc-B.default.svc.cluster.local ✓ (< 5 dots → try search first)
  "api.stripe.com" (2 dots) → 4 queries:
    api.stripe.com.default.svc.cluster.local  → NXDOMAIN
    api.stripe.com.svc.cluster.local          → NXDOMAIN
    api.stripe.com.cluster.local              → NXDOMAIN
    api.stripe.com.                           → SUCCESS (4th attempt)
  Result: 4× amplification for external names

Optimized ndots=2:
  "svc-B" (0 dots) → search first → 1 query ✓
  "api.stripe.com" (2 dots) → direct lookup (≥ ndots) → 1 query ✓
  Result: ~75% reduction in external query amplification
```

**CoreDNS Informer Pattern:**
```
Startup: LIST all Services/EndpointSlices from K8s API → build ServiceMap/EndpointMap
Runtime: WATCH via long-poll HTTP — receive Add/Update/Delete events
Event handling: update in-memory maps (sub-ms, lock-protected)
Query: read from maps directly — < 1 ms p99
Staleness bound: < 5 seconds after endpoint change
```

**autopath Plugin (Server-Side Search Expansion):**
```
Without autopath: client sends 4 sequential requests for "api.stripe.com"
With autopath (@kubernetes):
  CoreDNS tries all search domains internally on first client request
  Returns successful result in single response
  Client sees 1 round-trip instead of 4
Configured in Corefile: autopath @kubernetes
```

### Key Data Structures
```go
// ServiceIndex (CoreDNS in-process)
map[namespace/name] → ServiceEntry{
    ClusterIP:    "10.96.15.200",
    Type:         "ClusterIP" | "Headless" | "ExternalName",
    Ports:        [{name: "http", port: 80, protocol: "TCP"}],
    ExternalName: "ext.example.com",   // ExternalName type only
}

// EndpointIndex (CoreDNS in-process)
map[namespace/name] → []Endpoint{
    {IP: "10.244.1.15", NodeName: "node-03",
     Ports: [{name: "http", port: 8080}],
     Ready: true, Hostname: "pod-0"},  // StatefulSet pods have stable hostname
}

// PodIndex (reverse lookup)
map[podIP] → {Namespace, Name}

// NodeLocal DNSCache record
{Name: "svc-B.default.svc.cluster.local", Type: A, TTL: 30s,
 CachedAt: time.Now(), Records: ["10.96.15.200"]}
```

### NFRs
- 150,000 QPS average (20,000 pods × 7.5 QPS); 1,000,000 QPS peak (20,000 pods × 50 QPS peak)
- Internal breakdown: 30,000 QPS internal; 200,000 QPS external
- 4–5× query amplification from ndots=5 (mitigated by ndots=2 + autopath)
- 22,000 unique internal DNS names (2K services + 20K pod headless entries)
- Latency p99: < 0.5 ms NodeLocal cache hit; < 1 ms CoreDNS in-memory; < 5 ms internal cache miss; < 50 ms external upstream
- Availability: 99.999%

---

## load_balancer_design

### Unique Stack
- L4 tier: eBPF/XDP (eXpress Data Path) OR IPVS (IP Virtual Server — kernel netfilter)
- L7 tier: Envoy or HAProxy for content-aware routing
- GeoDNS / Anycast for global traffic steering to nearest datacenter
- Active health checker daemon (TCP/HTTP/gRPC probes)
- Connection tracking: per-flow state via Linux conntrack or eBPF LRU hash map

### Key Algorithms / Design Decisions

**Maglev Consistent Hashing:**
```
Table size M = 65537 (large prime)
Backends = [B0, B1, ..., Bn-1]

For each backend Bi:
  offset_i = hash1(Bi) % M
  skip_i   = hash2(Bi) % (M - 1) + 1    // skip must be > 0

  preference[i][j] = (offset_i + j * skip_i) % M   // permutation

Fill lookup table entry[0..M-1]:
  Initialize all to -1; next[i] = 0
  repeat until full:
    for each backend i:
      c = preference[i][next[i]]
      while entry[c] != -1:
        next[i]++; c = preference[i][next[i]]
      entry[c] = i; next[i]++

Lookup: backend = entry[hash(key) % M]   // O(1) array access
Backend removal: only entries owned by removed backend are remapped = K/N disruption
```

**Ring-Based Consistent Hashing with Virtual Nodes:**
```
Hash ring: 0 ─── 2^32

Backend B0 (weight 200) → 300 virtual nodes:
  hash("B0-0"), hash("B0-1"), ..., hash("B0-299")
Backend B1 (weight 100) → 150 virtual nodes

For request key K:
  position = hash(K)
  clockwise walk → first virtual node → maps to real backend
  O(log N) with sorted array + binary search
```

**IPVS Scheduling Algorithms:**
```
rr:    Round Robin
wrr:   Weighted Round Robin
lc:    Least Connections
wlc:   Weighted Least Connections (kube-proxy default)
sh:    Source Hash (session affinity)
dh:    Destination Hash
sed:   Shortest Expected Delay
nq:    Never Queue

Example setup:
  ipvsadm -A -t 10.0.0.1:80 -s wlc
  ipvsadm -a -t 10.0.0.1:80 -r 192.168.1.10:8080 -m -w 100  # NAT mode
  ipvsadm -a -t 10.0.0.1:80 -r 192.168.1.11:8080 -g -w 100  # DR mode (DSR)
```

**Direct Server Return (DSR) via IPVS DR mode:** backend responds directly to client (bypasses LB on return path); LB only processes inbound packets; 10× throughput improvement vs. full proxy for high-bandwidth flows.

### Key Tables
```sql
frontends (
  id UUID PRIMARY KEY,
  name VARCHAR(255) UNIQUE,
  vip INET,
  port INT,
  protocol ENUM('TCP','UDP','HTTP','HTTPS','gRPC'),
  lb_tier ENUM('L4','L7'),
  tls_cert_id UUID,
  created_at TIMESTAMP,
  UNIQUE (vip, port)
);

backend_pools (
  id UUID PRIMARY KEY,
  name VARCHAR(255) UNIQUE,
  algorithm ENUM('round_robin','weighted_round_robin','least_connections',
                 'consistent_hash','random','maglev'),
  hash_key ENUM('src_ip','header','cookie'),
  health_check JSONB,
  connection_limit INT,
  created_at TIMESTAMP
);

backends (
  id UUID PRIMARY KEY,
  pool_id UUID REFERENCES backend_pools(id),
  address INET,
  port INT,
  weight INT DEFAULT 100,
  health_status ENUM('healthy','unhealthy','draining','unknown'),
  last_health_check TIMESTAMP,
  metadata JSONB,   -- {zone, rack, node}
  created_at TIMESTAMP,
  INDEX idx_backends_pool_health (pool_id, health_status)
);

routing_rules (  -- L7 only
  id UUID PRIMARY KEY,
  frontend_id UUID REFERENCES frontends(id),
  priority INT,
  match_host VARCHAR(255),
  match_path VARCHAR(512),
  match_headers JSONB,
  backend_pool_id UUID REFERENCES backend_pools(id),
  weight INT,
  created_at TIMESTAMP,
  INDEX idx_rules_frontend_priority (frontend_id, priority DESC)
);

health_check_log (  -- time-series, partition by week
  id BIGSERIAL,
  backend_id UUID,
  check_time TIMESTAMP,
  result ENUM('pass','fail','timeout'),
  latency_ms INT,
  status_code INT,
  error_message TEXT
);

connection_track (  -- in-memory eBPF LRU hash map
  src_ip INET, src_port INT,
  dst_ip INET, dst_port INT,
  protocol INT,    -- 6=TCP, 17=UDP
  backend_ip INET, backend_port INT,
  state ENUM('SYN_RECV','ESTABLISHED','FIN_WAIT'),
  created_at TIMESTAMP,
  expires_at TIMESTAMP
);
```

### NFRs
- 1,500,000 RPS total (external 500K + internal 1M); 4,500,000 packets/sec (L4)
- 450,000 new TCP connections/sec; 90,000 peak concurrent connections
- 20,000 total backends (2K services × 10 replicas)
- Latency p99: < 20 µs eBPF/XDP L4; < 50 µs IPVS L4; < 1 ms L7
- Throughput: 10M+ packets/sec per node (eBPF/XDP); 500K+ RPS per node (L7 Envoy)
- Availability: 99.999%
- Backend add/remove: < 1 s propagation to data plane

---

## network_policy_enforcement_system

### Unique Stack
- Cilium CNI with eBPF TC (traffic control) hooks at pod veth ingress/egress
- Identity Manager: assigns numeric ID to each unique pod label set
- Policy Engine: translates `NetworkPolicy` + `CiliumNetworkPolicy` CRDs → eBPF maps
- Cilium ipcache: LPM trie mapping IP addresses to numeric identities
- Hubble: eBPF perf event ring buffer for flow log capture and observability
- L7 enforcement: REDIRECT to per-node Envoy sidecar for HTTP method/path/header and gRPC service/method filtering

### Key Algorithms / Design Decisions

**eBPF TC Policy Enforcement (per-packet pipeline):**
```c
SEC("tc")
int policy_ingress(struct __sk_buff *skb) {
    // 1. Parse L3/L4 headers (bounds-checked)

    // 2. Source identity: LPM trie lookup O(log n)
    src_identity = cilium_ipcache[src_ip]   // or WORLD(2) if external

    // 3. Conntrack fast path O(1)
    ct_entry = cilium_ct[5-tuple]
    if CT_ESTABLISHED: return TC_ACT_OK

    // 4. New flow: policy map lookup O(1)
    policy_key = {identity: src_identity, dport: dport, proto: proto}
    result = cilium_policy_<endpoint_id>[policy_key]
    // result: ALLOW=1, DENY=2, REDIRECT=4

    // 5. If REDIRECT: enqueue to local Envoy for L7 inspection

    // 6. Create conntrack entry for fast-path on subsequent packets

    // 7. Emit flow log via perf event ring buffer
    return TC_ACT_OK or TC_ACT_SHOT
}
```

**Identity-Based Scaling Model:**
```
100 pods with identical labels → 1 identity → 1 policy map entry
1,000 pods across 20 unique label sets → 20 identities → 20 entries

Pod rescheduled (IP changes) → same labels → same identity
  → no policy map update needed
  → ipcache updated with new IP → old entry TTL expires

vs. iptables/Calico (IP-based):
  O(n) rule traversal: 500 rules ~200 µs; 5,000 rules ~2 ms/packet
  eBPF hash map: 5,000 policies → still ~50 µs (O(1) hash)
```

**eBPF Map Structures:**
```c
// Policy map per endpoint
Map: cilium_policy_<endpoint_id>
Type: BPF_MAP_TYPE_HASH
Key:   {identity: u32, dport: u16, proto: u8}
Value: {flags: u8}  // ALLOW=1, DENY=2, REDIRECT=4

// Identity map (ipcache)
Map: cilium_ipcache
Type: BPF_MAP_TYPE_LPM_TRIE
Key:   {prefix_len: u32, ip: u32}
Value: {identity: u32, tunnel_endpoint: u32}
Examples:
  {/32, 10.244.1.15} → {identity: 12345}
  {/16, 10.96.0.0}   → {identity: WORLD}
```

**CiliumNetworkPolicy L7 Example:**
```yaml
endpointSelector: {matchLabels: {app: api-server}}
ingress:
  - fromEndpoints: [{matchLabels: {app: frontend}}]
    toPorts:
      - ports: [{port: "8080", protocol: TCP}]
        rules:
          http:
            - method: GET
              path: "/api/v1/users.*"
            - method: POST
              path: "/api/v1/orders"
              headers: ["Content-Type: application/json"]
egress:
  - toFQDNs:
      - matchName: "api.stripe.com"
      - matchPattern: "*.amazonaws.com"
    toPorts: [{ports: [{port: "443", protocol: TCP}]}]
```

### NFRs
- 20,000 pods / 200 nodes (100 pods/node); 100 namespaces
- 500 L3/L4 NetworkPolicies; 200 L7 CiliumNetworkPolicies
- 4,500,000 packets/sec cluster-wide; 22,500 pps per node; 2,250 new connections/sec per node
- Latency: < 50 µs per-packet (eBPF cached conntrack); < 200 µs per-packet (first packet of new flow)
- Policy propagation: < 5 seconds from CRD update to all eBPF maps
- Memory: ~50 MB eBPF maps per node
- Availability: enforcement continues even if Cilium agent restarts (eBPF maps persist in kernel)

---

## service_proxy_and_sidecar

### Unique Stack
- Istiod control plane: translates K8s resources (Service, VirtualService, DestinationRule, AuthorizationPolicy) → xDS snapshots
- Envoy sidecar per pod: transparent L7 proxy; ports 15001 (outbound), 15006 (inbound), 15090 (Prometheus), 15021 (health)
- istio-init container: sets up iptables REDIRECT rules in pod network namespace before app starts
- Istiod CA: issues SPIFFE X.509 SVIDs; root key stored in HSM
- xDS APIs served: LDS (Listeners), RDS (Routes), CDS (Clusters), EDS (Endpoints), SDS (Secrets)

### Key Algorithms / Design Decisions

**iptables REDIRECT Traffic Interception:**
```bash
# INBOUND: all TCP → sidecar port 15006
iptables -t nat -A PREROUTING -p tcp -j ISTIO_INBOUND
iptables -t nat -A ISTIO_INBOUND -p tcp --dport 15008 -j RETURN   # exclude HBONE
iptables -t nat -A ISTIO_INBOUND -p tcp --dport 15090 -j RETURN   # exclude Prometheus
iptables -t nat -A ISTIO_INBOUND -p tcp --dport 15021 -j RETURN   # exclude health
iptables -t nat -A ISTIO_INBOUND -p tcp -j ISTIO_IN_REDIRECT
iptables -t nat -A ISTIO_IN_REDIRECT -p tcp -j REDIRECT --to-port 15006

# OUTBOUND: all TCP → sidecar port 15001
iptables -t nat -A OUTPUT -p tcp -j ISTIO_OUTPUT
iptables -t nat -A ISTIO_OUTPUT -m owner --uid-owner 1337 -j RETURN   # skip Envoy itself
iptables -t nat -A ISTIO_OUTPUT -o lo -d 127.0.0.1/32 -j RETURN       # skip localhost
iptables -t nat -A ISTIO_OUTPUT -d 10.96.0.1/32 -j RETURN             # skip K8s API server
iptables -t nat -A ISTIO_OUTPUT -p tcp -j ISTIO_REDIRECT
iptables -t nat -A ISTIO_REDIRECT -p tcp -j REDIRECT --to-port 15001
```

**SPIFFE Identity Issuance:**
```
Format: spiffe://<trust-domain>/ns/<namespace>/sa/<service-account>
Example: spiffe://cluster.local/ns/default/sa/svc-A

Flow:
1. Pod starts → Envoy sidecar starts
2. Envoy sends CSR to Istiod via SDS gRPC stream
3. Istiod validates: extract workload identity from pod metadata
   → verify SA matches pod's service account
4. Issue X.509 SVID cert (24h TTL, configurable)
5. Cert rotated at 2/3 TTL (~16h); new CSR sent; old cert gracefully retired
   → no connection drops during rotation
6. CA root key stored in HSM; never on disk

Scale: 20K sidecars / 24h rotation = ~14 certs/minute average
```

**Outlier Detection (Circuit Breaker):**
```yaml
outlier_detection:
  consecutive_5xx: 5           # eject after 5 consecutive 5xx from this endpoint
  interval: 10s                # check interval
  base_ejection_time: 30s      # minimum ejection duration
  max_ejection_percent: 50     # at most 50% of pool ejected at once
  enforcing_consecutive_5xx: 100  # enforcement level (%)
```

**VirtualService (Traffic Splitting) + DestinationRule:**
```yaml
# VirtualService: 90/10 split with canary header override
spec:
  http:
    - match:
        - headers: {x-canary: {exact: "true"}}
      route:
        - destination: {host: svc-B, subset: v2}
    - route:
        - destination: {host: svc-B, subset: v1}
          weight: 90
        - destination: {host: svc-B, subset: v2}
          weight: 10

# DestinationRule: LB + circuit breaker + subsets
spec:
  trafficPolicy:
    connectionPool:
      tcp: {maxConnections: 1024, connectTimeout: 5s}
      http: {h2UpgradePolicy: DEFAULT, maxRequestsPerConnection: 100}
    loadBalancer: {simple: LEAST_REQUEST}
    outlierDetection:
      consecutive5xxErrors: 5
      interval: 10s
      baseEjectionTime: 30s
      maxEjectionPercent: 50
  subsets:
    - name: v1; labels: {version: v1}
    - name: v2; labels: {version: v2}
```

**Retry Policy (RDS route config):**
```yaml
retry_policy:
  retry_on: "5xx,reset,connect-failure"
  num_retries: 2
  per_try_timeout: 10s
  retry_back_off:
    base_interval: 0.1s
    max_interval: 1s
```

### NFRs
- 20,000 sidecar proxies (2K services × 10 avg replicas); 1,000,000 RPS east-west
- 200,000 concurrent mTLS connections; 50 RPS avg per sidecar; 500 RPS peak per sidecar
- ~14 certificates/minute (20K sidecars / 24h rotation period)
- ~10 xDS updates/second cluster-wide; ~500 KB config size per sidecar
- Latency overhead: < 1 ms p99 per sidecar; < 5 ms p99 mTLS handshake
- Memory: < 50 MB base per sidecar; < 150 MB under load
- CPU: < 0.1 cores per sidecar at 1,000 RPS
- Config propagation: < 5 seconds from Istiod to all sidecars
- Availability: 99.999% (sidecar crash isolates only its pod)
