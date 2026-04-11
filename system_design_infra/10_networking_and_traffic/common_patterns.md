# Common Patterns — Networking & Traffic (10_networking_and_traffic)

## Common Components

### Two-Tier Architecture (High-Throughput Tier + Feature-Rich Tier)
- All 5 systems use a fast path + smart path split
- api_gateway: TLS termination (fast) → plugin chain auth+ratelimit+transform (smart)
- load_balancer: L4 eBPF/IPVS packet forwarding (fast) → L7 Envoy content routing (smart)
- dns_at_scale: NodeLocal DaemonSet 169.254.25.10 (fast) → CoreDNS cluster (smart)
- network_policy: eBPF ipcache identity lookup (fast) → policy map enforcement (smart)
- service_proxy: iptables REDIRECT (fast) → Envoy sidecar (smart)

### Watch-Based Incremental Config Distribution
- All 5 systems use Kubernetes informer pattern or gRPC streaming for real-time propagation (not polling)
- api_gateway + service_proxy: gRPC xDS streaming (LDS/RDS/CDS/EDS/SDS); control plane pushes delta snapshots with version numbers; atomic swap at data plane
- load_balancer: Kubernetes informer watches Services/EndpointSlices; in-memory backend pool rebuilt on delta
- dns_at_scale: CoreDNS Kubernetes plugin uses LIST+WATCH against K8s API; ServiceMap/EndpointMap updated sub-second
- network_policy: Cilium agent watches NetworkPolicy + CiliumNetworkPolicy CRDs; eBPF maps recompiled on delta
- Target propagation latency: < 5 seconds from config change to all data plane nodes

### Circuit Breaker / Outlier Detection
- 4 of 5 systems implement failure detection with automatic rerouting
- api_gateway: error rate tracking per upstream; circuit opens on threshold; half-open probe to test recovery
- load_balancer: active health probes (TCP/HTTP/gRPC) + passive error rate; unhealthy backends marked and removed from pool
- dns_at_scale: upstream resolver failover; CoreDNS retries alternate resolvers on timeout
- service_proxy (Envoy): `consecutive_5xx: 5`, `interval: 10s`, `base_ejection_time: 30s`, `max_ejection_percent: 50`; ejected endpoints re-tested periodically

### In-Memory Data Structures for O(1) / O(log n) Lookup
- All 5 systems avoid database round-trips on the hot path
- api_gateway: HashMap<Host, Trie> for route matching — O(1) host hash + O(k) trie path walk
- load_balancer: Maglev lookup table (size M = 65537 prime) for O(1) consistent hashing; connection tracking HashMap
- dns_at_scale: ServiceIndex and EndpointIndex as `map[namespace/name]` — O(1) lookup; NodeLocal cache 10K records
- network_policy: eBPF hash map `cilium_policy_<endpoint_id>` (O(1) key=identity+dport+proto) + LPM trie `cilium_ipcache` (O(log n) IP→identity)
- service_proxy: xDS config compiled into Envoy in-memory listener/route/cluster/endpoint structures; O(1) prefix tree lookup

### TLS / mTLS Termination
- api_gateway: SNI-based certificate selection (GIN index on `sni` field); certificates table stores `cert_pem` + encrypted `key_pem`; Secret Discovery Service (SDS) for rotation
- load_balancer: L7 tier terminates TLS; re-encrypts to upstream optionally (mTLS)
- service_proxy: mandatory mTLS between all sidecars; SPIFFE X.509 SVIDs issued by Istiod CA; transparent to application

### eBPF / Kernel-Space Enforcement
- load_balancer: XDP (eXpress Data Path) for L4 packet forwarding at < 20 µs p99; 10M+ packets/sec per node
- network_policy: TC (traffic control) eBPF hooks at ingress/egress per pod veth; O(1) hash map policy lookup regardless of rule count
- service_proxy: iptables REDIRECT (current); migration to eBPF TPROXY for lower overhead planned

### Distributed Rate Limiting with Local Pre-allocation
- api_gateway: hybrid approach — local token bucket pre-allocated (`global_limit / num_nodes` tokens per node) + Redis sliding window counter for cross-node accuracy; refill rate = `global_rate / num_nodes`; Redis Lua script: `effective_count = prev_window_count × overlap + INCR(curr_window)`
- load_balancer: per-connection limits via circuit breaker (`max_connections`, `max_pending_requests`, `max_requests`) per CDS cluster config
- service_proxy: per-sidecar connection pool limits (`maxConnections: 1024`, `maxRequestsPerConnection: 100`)

## Common Databases

### Redis (Rate Limiting + Caching)
- api_gateway: Redis Cluster for distributed rate limit counters — 320 MB for 50K consumers × 100 routes; sliding window with 1-minute TTL; also LRU cache for API key lookup
- No other systems in this folder use Redis as primary store; all favor in-memory kernel/process-level state

### PostgreSQL / MySQL (Route & Policy Configuration)
- api_gateway: PostgreSQL for routes, upstreams, consumers, api_keys, certificates — authoritative config store; data-plane reads compiled into xDS snapshots (not queried per request)
- network_policy: Kubernetes etcd as backing store for NetworkPolicy/CiliumNetworkPolicy CRDs
- load_balancer: etcd or Kubernetes API server stores backend pool configs; frontends/backend_pools/backends tables

### In-Process / eBPF Map Storage (No External DB on Hot Path)
- dns_at_scale: CoreDNS holds ServiceMap/EndpointMap entirely in-process; no DB reads per DNS query
- network_policy: all per-packet enforcement via eBPF maps loaded into Linux kernel; zero userspace DB calls
- service_proxy: Envoy config loaded from xDS; no DB calls per request

## Common Communication Patterns

### gRPC xDS Streaming (Control Plane → Data Plane)
- api_gateway: Route Compiler translates route configs → Envoy xDS snapshots; pushed to gateway instances
- service_proxy: Istiod serves LDS/RDS/CDS/EDS/SDS via gRPC streaming; ~10 xDS updates/sec cluster-wide; ~500 KB config per sidecar; < 5 s propagation

### Kubernetes Informer Pattern (LIST + WATCH)
- load_balancer: watches Services/EndpointSlices for backend pool membership changes
- dns_at_scale: CoreDNS Kubernetes plugin LIST all resources at startup; WATCH for incremental updates; staleness < 5 seconds
- network_policy: Cilium agent watches NetworkPolicy CRDs; compiles delta into eBPF map updates

### Health Check (Active TCP/HTTP + Passive Error Rate)
- load_balancer: TCP probe (establish connection), HTTP probe (GET with expected status), gRPC probe (Health check protocol); passive tracking of 5xx rate
- api_gateway: upstream health tracked via circuit breaker error rate counters
- service_proxy: Envoy outlier detection is passive (tracks 5xx responses per endpoint)

## Common Scalability Techniques

### Consistent Hashing for Session Affinity + Minimal Disruption
- load_balancer: Maglev hashing — fill lookup table of size M=65537; each backend gets `M/N` entries via deterministic permutation; backend removal remaps only `K/N` entries (not entire table); O(1) lookup
- load_balancer: Ring hash — 150 virtual nodes per backend on 2^32 ring; weighted backends get proportionally more virtual nodes; O(log N) clockwise walk
- api_gateway: per-route consistent hash on `header|cookie|ip`; same hash_key always reaches same upstream
- service_proxy: Envoy supports `ring_hash` and `maglev` lb_policy in DestinationRule

### Node-Local Caching (Eliminate Cross-Node Calls)
- dns_at_scale: NodeLocal DNSCache DaemonSet on every node at 169.254.25.10; 10K records, 30 s TTL; eliminates CoreDNS cluster load for hot queries; < 0.5 ms p99 cache hit
- api_gateway: local LRU cache for API key → consumer mapping; avoids Redis on cache hit; synchronous Redis fallback on miss
- network_policy: eBPF maps compiled per-node; policy lookup never leaves kernel; ~50 µs cached per-packet

### ndots / Query Amplification Reduction
- dns_at_scale: default `ndots=5` causes 4 NXDOMAIN queries before resolving external names; tuning to `ndots=2` reduces to 1 query for external (2+ dots = direct lookup); `autopath` plugin does server-side search expansion so client makes 1 round-trip instead of 4

## Common Deep Dive Questions

### How do you enforce network policy at 20,000+ pods without per-packet performance degradation?
Answer: Cilium eBPF identity model: each unique pod label set maps to a numeric identity (murmur hash of sorted labels). eBPF TC hook does: (1) LPM trie lookup `cilium_ipcache[src_ip]` → identity (O(log n)); (2) conntrack check for established flows (fast path O(1)); (3) new flow: hash map lookup `cilium_policy_<endpoint_id>[{identity, dport, proto}]` → ALLOW/DENY/REDIRECT (O(1)); (4) if REDIRECT, enqueue to Envoy for L7 inspection. Total: < 50 µs for cached flows. vs. iptables (Calico): O(n) rule traversal — 5,000 rules = ~2 ms/packet (unacceptable). Identity model: 100 pods with same labels = 1 identity = 1 policy map entry. Pod reschedule: no policy update needed. Scale: 20K pods collapses to ~1K unique identities.
Present in: network_policy_enforcement_system

### How do you achieve O(1) consistent hashing with minimal disruption when backends are added/removed?
Answer: Maglev hashing: (1) Table size M = 65537 (prime); (2) for each backend Bi, compute deterministic preference list using two independent hashes: `offset_i = hash1(Bi) % M`, `skip_i = hash2(Bi) % (M-1) + 1`; (3) fill table greedily — each backend claims preferred slots not yet taken; (4) lookup = `entry[hash(key) % M]` — O(1) array index; (5) on backend removal: only entries previously assigned to removed backend are remapped (K/N average disruption, not full rehash). Ring hash alternative: 150 virtual nodes per backend on 2^32 ring; O(log N) clockwise walk to find owner.
Present in: load_balancer_design, service_proxy_and_sidecar

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **API / Data-plane Latency p50** | < 0.5 ms DNS cache hit; < 1 ms L7 LB; < 50 µs L4 LB; < 1 ms sidecar overhead |
| **API / Data-plane Latency p99** | < 2 ms API gateway passthrough; < 50 µs L4 eBPF; < 1 ms L7; < 200 µs eBPF first-packet; < 1 ms sidecar |
| **Throughput** | 900K RPS gateway; 1.5M RPS LB; 1M QPS DNS peak; 4.5M pps L4; 1M RPS east-west sidecar |
| **Availability** | 99.999% (52 min/year) across all 5 systems |
| **Config Propagation** | < 5 seconds from change to all data plane nodes |
| **Scale** | 20K pods; 2K services; 20K sidecar proxies; 50K API keys; 200 nodes |
| **Memory per node** | 320 MB rate limit counters (GW); 500 MB eBPF maps (net-policy); 50–150 MB per sidecar; 6 MB DNS in-process |
| **TLS Cert Rotation** | 24h cert TTL; ~14 certs/min at 20K sidecars; no connection drops during rotation |
