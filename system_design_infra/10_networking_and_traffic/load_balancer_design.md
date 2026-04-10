# System Design: Load Balancer

> **Relevance to role:** Load balancing is foundational to every layer of a cloud infrastructure platform. For bare-metal IaaS and Kubernetes environments, you must understand L4 vs L7 trade-offs, kernel-space load balancing (IPVS, eBPF/XDP), consistent hashing for stateful backends, kube-proxy internals (iptables vs IPVS mode), and global server load balancing. This is a topic that surfaces in every infrastructure system design discussion.

---

## 1. Requirement Clarifications

### Functional Requirements

| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | L4 (TCP/UDP) load balancing | Distribute connections by 5-tuple, no content inspection |
| FR-2 | L7 (HTTP/HTTPS/gRPC) load balancing | Content-aware routing: host, path, headers, cookies |
| FR-3 | Multiple LB algorithms | Round robin, weighted round robin, least connections, consistent hashing, random |
| FR-4 | Health checking | Active probes (TCP, HTTP, gRPC) and passive (error rate tracking) |
| FR-5 | Session persistence | Cookie-based, IP-based, consistent hashing for sticky sessions |
| FR-6 | SSL/TLS termination | At L7 LB; passthrough at L4 |
| FR-7 | Connection draining | Graceful removal of backends without dropping in-flight requests |
| FR-8 | Dynamic backend discovery | Integrate with Kubernetes endpoints, Consul, DNS for real-time backend updates |
| FR-9 | Global server load balancing | Cross-datacenter traffic steering via GeoDNS / Anycast |
| FR-10 | Observability | Per-backend metrics, connection counts, latency histograms, error rates |

### Non-Functional Requirements

| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | L4 latency overhead | < 50 microseconds (kernel-space) |
| NFR-2 | L7 latency overhead | < 1 ms p99 (proxy mode) |
| NFR-3 | Throughput (L4) | 10M+ packets/second per node (eBPF/XDP) |
| NFR-4 | Throughput (L7) | 500K+ RPS per node (Envoy/HAProxy) |
| NFR-5 | Availability | 99.999% (redundant pair, sub-second failover) |
| NFR-6 | Connection capacity | 1M+ concurrent connections per node |
| NFR-7 | Zero-downtime config reload | Add/remove backends without dropping connections |

### Constraints & Assumptions

- Infrastructure is bare-metal with Kubernetes orchestration.
- Network fabric is leaf-spine with BGP routing (no spanning tree).
- Servers have 25/100 Gbps NICs with hardware timestamping.
- Kernel version 5.15+ (full eBPF/XDP support).
- Both north-south (external to internal) and east-west (service to service) traffic must be balanced.

### Out of Scope

- CDN edge caching and content delivery.
- Application-level load balancing (client-side with gRPC).
- WAF / DDoS mitigation (separate system, though LB is the first defense).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|------------|-------|
| Total services in cluster | Given | 2,000 microservices |
| Average replicas per service | Given | 10 pods |
| Total backends | 2,000 x 10 | 20,000 endpoints |
| External peak RPS | Given | 500,000 RPS |
| Internal (east-west) peak RPS | 2,000 services x 500 avg outbound RPS | 1,000,000 RPS |
| Total peak RPS | External + internal | 1,500,000 RPS |
| Peak new TCP connections/sec | 30% of RPS (connection reuse) | 450,000 conn/s |
| Peak concurrent connections | Avg connection duration 200ms x 450K/s | 90,000 |
| Peak packets/sec (L4) | 1.5M RPS x 3 packets avg per req/resp | 4,500,000 pps |
| Average request size | Typical API JSON | 2 KB |
| Average response size | Typical API JSON | 5 KB |

### Latency Requirements

| Layer | Target |
|-------|--------|
| L4 LB (kernel-space, IPVS) | < 100 us p50, < 200 us p99 |
| L4 LB (eBPF/XDP) | < 20 us p50, < 50 us p99 |
| L7 LB (Envoy/HAProxy) | < 500 us p50, < 1 ms p99 |
| Health check interval | 3-10 seconds (configurable) |
| Backend failover detection | < 10 seconds (3 consecutive failures) |
| Config reload (backend add/remove) | < 1 second |

### Storage Estimates

| Data | Calculation | Size |
|------|------------|------|
| Backend endpoint table | 20,000 endpoints x 256 bytes | 5 MB |
| Connection tracking table (conntrack) | 1M entries x 320 bytes | 320 MB |
| IPVS rules | 2,000 services x 10 backends x 128 bytes | 2.5 MB |
| eBPF maps | Hash maps for backends + conntrack | 500 MB |
| Access logs (1 hour) | 1.5M RPS x 3600s x 200 bytes | ~1 TB/hour |

### Bandwidth Estimates

| Direction | Calculation | Value |
|-----------|------------|-------|
| Client to LB (ingress) | 500K RPS x 2 KB | 1 GB/s (8 Gbps) |
| LB to backends | 1.5M RPS x 2 KB | 3 GB/s (24 Gbps) |
| Backends to LB | 1.5M RPS x 5 KB | 7.5 GB/s (60 Gbps) |
| LB to client (egress) | 500K RPS x 5 KB | 2.5 GB/s (20 Gbps) |
| Total LB throughput | Ingress + egress + internal | ~112 Gbps (requires 2x 100G NICs) |

---

## 3. High Level Architecture

```
                         External Clients
                               │
                               ▼
                    ┌─────────────────────┐
                    │   GeoDNS / Anycast  │  ← GSLB: direct to nearest DC
                    └──────────┬──────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
         DC-West          DC-Central        DC-East
              │                │                │
              ▼                ▼                ▼
    ┌──────────────────────────────────────────────────┐
    │              L4 LOAD BALANCER TIER                │
    │                                                    │
    │  ┌─────────────┐  ┌─────────────┐                 │
    │  │   LB Node 1 │  │   LB Node 2 │  (ECMP pair)   │
    │  │  eBPF/XDP   │  │  eBPF/XDP   │                 │
    │  │  or IPVS    │  │  or IPVS    │                 │
    │  └──────┬──────┘  └──────┬──────┘                 │
    │         │    BGP ECMP    │                         │
    │         └────────┬───────┘                         │
    │                  │                                  │
    │                  ▼                                  │
    │  ┌──────────────────────────────────────┐          │
    │  │       L7 LOAD BALANCER TIER          │          │
    │  │                                       │          │
    │  │  ┌────────┐ ┌────────┐ ┌────────┐   │          │
    │  │  │ Envoy  │ │ Envoy  │ │ Envoy  │   │          │
    │  │  │  Node  │ │  Node  │ │  Node  │   │          │
    │  │  │  (L7)  │ │  (L7)  │ │  (L7)  │   │          │
    │  │  └───┬────┘ └───┬────┘ └───┬────┘   │          │
    │  │      │          │          │          │          │
    │  └──────┼──────────┼──────────┼─────────┘          │
    │         │          │          │                      │
    │         ▼          ▼          ▼                      │
    │  ┌──────────────────────────────────────┐           │
    │  │         BACKEND SERVICES              │           │
    │  │                                       │           │
    │  │  [svc-A: pod1,pod2,...] [svc-B: ...]  │           │
    │  │  [svc-C: pod1,pod2,...] [svc-D: ...]  │           │
    │  └──────────────────────────────────────┘           │
    │                                                      │
    │  ┌──────────────────────────────────────┐           │
    │  │      EAST-WEST (Service Mesh)         │           │
    │  │                                       │           │
    │  │  kube-proxy (IPVS mode) OR            │           │
    │  │  Cilium eBPF (replacing kube-proxy)   │           │
    │  │  OR Envoy sidecar (Istio)             │           │
    │  └──────────────────────────────────────┘           │
    └──────────────────────────────────────────────────────┘

    ┌──────────────────────────────────────────────────────┐
    │              CONTROL PLANE                            │
    │                                                       │
    │  ┌────────────┐  ┌────────────┐  ┌───────────────┐  │
    │  │  LB Config │  │   Health   │  │   Kubernetes  │  │
    │  │  Manager   │  │   Checker  │  │   API Server  │  │
    │  └─────┬──────┘  └─────┬──────┘  └───────┬───────┘  │
    │        │               │                  │           │
    │        └───────────────┼──────────────────┘           │
    │                        │                              │
    │                ┌───────▼────────┐                     │
    │                │   etcd / xDS   │                     │
    │                │   Config Store │                     │
    │                └────────────────┘                     │
    └──────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| GeoDNS / Anycast | Global traffic steering to nearest datacenter |
| L4 LB Tier (eBPF/XDP or IPVS) | High-throughput packet-level load balancing; no content inspection |
| L7 LB Tier (Envoy/HAProxy) | Content-aware routing, TLS termination, HTTP/gRPC load balancing |
| Backend Services | Application pods running in Kubernetes |
| East-West LB (kube-proxy/Cilium) | Internal service-to-service load balancing within the cluster |
| LB Config Manager | Translates Kubernetes Service/Ingress objects into LB configuration |
| Health Checker | Active + passive health monitoring of all backends |
| etcd / xDS | Configuration distribution to LB nodes |

### Data Flows

**North-South (external client to service):**
1. Client DNS resolves `api.example.com` → Anycast VIP (same IP announced from multiple DCs).
2. BGP routing delivers packet to nearest DC.
3. Within DC, L4 LB (eBPF/XDP) receives packet on VIP.
4. L4 LB hashes 5-tuple → selects L7 LB node (consistent hashing for connection affinity).
5. L7 LB terminates TLS, inspects HTTP headers.
6. L7 LB routes to backend pod using configured algorithm (least connections for API, consistent hash for cache).
7. Response returns via reverse path.

**East-West (service to service):**
1. Pod A calls `svc-B.namespace.svc.cluster.local:8080`.
2. CoreDNS resolves to ClusterIP (virtual IP).
3. kube-proxy (IPVS mode) or Cilium eBPF intercepts the packet destined for ClusterIP.
4. IPVS/eBPF selects a backend pod IP using configured algorithm.
5. Packet is forwarded directly to backend pod (DNAT).

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Virtual IP / Frontend definition
CREATE TABLE frontends (
    id              UUID PRIMARY KEY,
    name            VARCHAR(255) NOT NULL UNIQUE,
    vip             INET NOT NULL,                 -- Virtual IP address
    port            INT NOT NULL,
    protocol        VARCHAR(10) NOT NULL,           -- TCP, UDP, HTTP, HTTPS, gRPC
    lb_tier         VARCHAR(10) NOT NULL,           -- L4 or L7
    tls_cert_id     UUID REFERENCES certificates(id),
    created_at      TIMESTAMPTZ DEFAULT NOW()
);
CREATE UNIQUE INDEX idx_frontends_vip_port ON frontends(vip, port);

-- Backend pool (upstream cluster)
CREATE TABLE backend_pools (
    id              UUID PRIMARY KEY,
    name            VARCHAR(255) NOT NULL UNIQUE,
    algorithm       VARCHAR(50) NOT NULL DEFAULT 'round_robin',
    -- round_robin, weighted_round_robin, least_connections,
    -- consistent_hash, random, maglev
    hash_key        VARCHAR(50),                   -- For consistent_hash: src_ip, header, cookie
    health_check    JSONB NOT NULL,                -- {type, path, interval_ms, timeout_ms, 
                                                   --  healthy_threshold, unhealthy_threshold}
    connection_limit INT DEFAULT 0,                -- 0 = unlimited
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Individual backend endpoints
CREATE TABLE backends (
    id              UUID PRIMARY KEY,
    pool_id         UUID NOT NULL REFERENCES backend_pools(id),
    address         INET NOT NULL,
    port            INT NOT NULL,
    weight          INT DEFAULT 100,               -- For weighted algorithms
    health_status   VARCHAR(20) DEFAULT 'unknown', -- healthy, unhealthy, draining, unknown
    last_health_check TIMESTAMPTZ,
    metadata        JSONB,                         -- {zone, rack, node}
    created_at      TIMESTAMPTZ DEFAULT NOW()
);
CREATE INDEX idx_backends_pool_health ON backends(pool_id, health_status);

-- Routing rules (L7 only)
CREATE TABLE routing_rules (
    id              UUID PRIMARY KEY,
    frontend_id     UUID NOT NULL REFERENCES frontends(id),
    priority        INT NOT NULL DEFAULT 0,
    match_host      VARCHAR(255),
    match_path      VARCHAR(1024),
    match_headers   JSONB,
    backend_pool_id UUID NOT NULL REFERENCES backend_pools(id),
    weight          INT DEFAULT 100,               -- For traffic splitting
    created_at      TIMESTAMPTZ DEFAULT NOW()
);
CREATE INDEX idx_routing_frontend_prio ON routing_rules(frontend_id, priority DESC);

-- Health check results (time-series, can use Prometheus instead)
CREATE TABLE health_check_log (
    id              BIGSERIAL PRIMARY KEY,
    backend_id      UUID NOT NULL REFERENCES backends(id),
    check_time      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    result          VARCHAR(20) NOT NULL,          -- pass, fail, timeout
    latency_ms      INT,
    status_code     INT,                           -- For HTTP checks
    error_message   TEXT
);
CREATE INDEX idx_hc_log_backend_time ON health_check_log(backend_id, check_time DESC);

-- Connection tracking (in-memory, shown for reference)
-- In practice this is in IPVS conntrack or eBPF map
CREATE TABLE connection_track (
    src_ip          INET NOT NULL,
    src_port        INT NOT NULL,
    dst_ip          INET NOT NULL,
    dst_port        INT NOT NULL,
    protocol        INT NOT NULL,                  -- 6=TCP, 17=UDP
    backend_ip      INET NOT NULL,
    backend_port    INT NOT NULL,
    state           VARCHAR(20),                   -- SYN_RECV, ESTABLISHED, FIN_WAIT, etc.
    created_at      TIMESTAMPTZ,
    expires_at      TIMESTAMPTZ,
    PRIMARY KEY (src_ip, src_port, dst_ip, dst_port, protocol)
);
```

### Database Selection

| Store | Technology | Rationale |
|-------|-----------|-----------|
| LB configuration | etcd / Kubernetes API | Already exists in K8s; watch-based updates |
| Connection tracking | Kernel conntrack (IPVS) or eBPF hash map | Must be in kernel/eBPF for performance |
| Health check state | In-memory + Prometheus | Real-time; metrics for alerting |
| Access logs | Elasticsearch (via Fluentd) or ClickHouse | High-write throughput, time-series queries |
| GSLB state | Consul / etcd (cross-DC) | Cross-datacenter health aggregation |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| frontends | `(vip, port)` unique | VIP lookup |
| backends | `(pool_id, health_status)` | Healthy backend selection |
| routing_rules | `(frontend_id, priority DESC)` | Priority-ordered rule evaluation |
| health_check_log | `(backend_id, check_time DESC)` | Recent health history |

---

## 5. API Design

### Management APIs

```
# Frontends (VIPs)
POST   /api/v1/frontends                    → Create frontend (VIP:port binding)
GET    /api/v1/frontends                    → List all frontends
GET    /api/v1/frontends/{id}               → Get frontend details
PUT    /api/v1/frontends/{id}               → Update frontend config
DELETE /api/v1/frontends/{id}               → Remove frontend

# Backend Pools
POST   /api/v1/pools                        → Create backend pool
GET    /api/v1/pools/{id}                   → Get pool with all backends
PUT    /api/v1/pools/{id}                   → Update pool config (algorithm, health check)
DELETE /api/v1/pools/{id}                   → Remove pool

# Backends
POST   /api/v1/pools/{id}/backends          → Add backend to pool
PUT    /api/v1/backends/{id}                → Update backend (weight, metadata)
DELETE /api/v1/backends/{id}                → Remove backend (triggers drain)
PUT    /api/v1/backends/{id}/drain          → Start connection draining
GET    /api/v1/backends/{id}/health         → Get backend health history

# Routing Rules (L7)
POST   /api/v1/frontends/{id}/rules         → Add routing rule
PUT    /api/v1/rules/{id}                   → Update routing rule
DELETE /api/v1/rules/{id}                   → Remove routing rule

# Status
GET    /api/v1/status                       → Overall LB cluster health
GET    /api/v1/stats                        → Real-time connection/RPS stats
GET    /api/v1/pools/{id}/stats             → Per-pool backend stats
```

### Data Plane Behavior

```
L4 Data Plane (eBPF/XDP):

  Packet arrives on NIC
       │
       ▼
  ┌─────────────────────┐
  │  XDP Program         │ ← Runs before kernel network stack
  │  (eBPF bytecode)     │
  │                       │
  │  1. Parse headers     │ ← Ethernet + IP + TCP/UDP
  │  2. Lookup VIP table  │ ← BPF_MAP_TYPE_HASH {vip:port → pool_id}
  │  3. If no match →     │
  │     XDP_PASS (normal) │
  │  4. Lookup conntrack   │ ← BPF_MAP_TYPE_LRU_HASH {5-tuple → backend}
  │  5. If existing conn → │
  │     Use cached backend │
  │  6. If new conn →      │
  │     Select backend     │ ← Algorithm: Maglev hash / round robin
  │  7. DNAT: rewrite      │
  │     dst_ip/port        │
  │  8. Update conntrack   │
  │  9. XDP_TX or          │
  │     XDP_REDIRECT       │ ← Forward to backend's NIC
  └─────────────────────┘

L7 Data Plane (Envoy):

  TCP connection arrives (from L4 tier)
       │
       ▼
  ┌─────────────────────┐
  │  Listener            │ ← Binds to port, filter chain match
  │  (TLS if HTTPS)      │
  └──────┬──────────────┘
         │
         ▼
  ┌─────────────────────┐
  │  HTTP Connection     │ ← HTTP/1.1 or HTTP/2 codec
  │  Manager             │
  └──────┬──────────────┘
         │
         ▼
  ┌─────────────────────┐
  │  Route Table         │ ← Match host + path + headers
  │  Lookup              │
  └──────┬──────────────┘
         │
         ▼
  ┌─────────────────────┐
  │  Cluster Selection   │ ← Upstream cluster (= backend pool)
  │  + Load Balancing    │ ← Algorithm applied here
  └──────┬──────────────┘
         │
         ▼
  ┌─────────────────────┐
  │  Connection Pool     │ ← Reuse upstream connections
  │  + Circuit Breaker   │ ← Check: max connections, max pending, max retries
  └──────┬──────────────┘
         │
         ▼
  ┌─────────────────────┐
  │  Upstream Request    │ ← Forward request, collect response
  │  + Retry Logic       │
  └─────────────────────┘
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: L4 vs L7 Load Balancing — When and Why

**Why it's hard:**
Choosing between L4 and L7 affects latency, throughput, feature set, and operational complexity. L4 is fast but blind to content. L7 is feature-rich but adds latency and requires more resources. Most production systems need both tiers, and the interaction between them is subtle.

**Approaches:**

| Aspect | L4 (Transport) | L7 (Application) |
|--------|----------------|-------------------|
| OSI Layer | Layer 4 (TCP/UDP) | Layer 7 (HTTP/HTTPS/gRPC) |
| Decision basis | 5-tuple: src_ip, src_port, dst_ip, dst_port, protocol | URL path, host header, cookies, headers |
| Latency overhead | 10-100 us (kernel-space) | 0.5-2 ms (user-space proxy) |
| Throughput | 10M+ pps per node | 100K-500K RPS per node |
| TLS handling | Passthrough (no termination) | Terminates TLS, inspects content |
| Connection multiplexing | No (1:1 client-to-backend) | Yes (N client conns → M backend conns) |
| gRPC balancing | Per-connection only (poor) | Per-RPC (good) |
| Sticky sessions | Source IP hash | Cookie, header, consistent hash |
| Health checks | TCP SYN, UDP probe | HTTP GET, gRPC health check protocol |
| Implementations | IPVS, eBPF/XDP, Katran, Maglev | Envoy, HAProxy, Nginx, Traefik |
| Visibility | IP/port only | Full request/response inspection |
| Use case | First tier (high throughput, low latency) | Second tier (content routing, TLS, features) |

**Selected approach: Two-tier architecture — L4 (eBPF/XDP) fronting L7 (Envoy)**

**Justification:** L4 tier handles raw packet switching at line rate with minimal latency. It distributes traffic across L7 nodes. L7 tier handles content routing, TLS, and advanced features. This separation allows each tier to be scaled independently and provides defense in depth.

**Implementation detail — IPVS:**

```
IPVS operates in three modes:

1. NAT Mode (Network Address Translation):
   Client → LB (DNAT to backend) → Backend → LB (SNAT to client)
   - LB rewrites both request AND response packets
   - LB is in the return path (bottleneck)
   - Works with any backend (no special config)

2. DR Mode (Direct Return / Direct Server Return):
   Client → LB (rewrite MAC, not IP) → Backend → Client (directly)
   - LB only handles inbound packets
   - Backend responds directly to client (skips LB)
   - Requires backends to accept traffic for VIP (ARP suppression)
   - 10x throughput vs NAT (LB only sees half the traffic)
   - L2 adjacency required (same subnet)

3. TUN Mode (IP Tunneling / IP-in-IP):
   Client → LB (encapsulate in IP-in-IP) → Backend (decapsulate) → Client
   - Like DR but works across L3 (different subnets/DCs)
   - Backend needs ipip tunnel interface
   - Slightly more overhead than DR (IP-in-IP header)

IPVS Scheduling Algorithms:
  rr     - Round Robin
  wrr    - Weighted Round Robin
  lc     - Least Connections
  wlc    - Weighted Least Connections (default in kube-proxy IPVS mode)
  sh     - Source Hash (session affinity)
  dh     - Destination Hash
  sed    - Shortest Expected Delay
  nq     - Never Queue

Setup example (ipvsadm):
  # Create virtual service on VIP
  ipvsadm -A -t 10.0.0.1:80 -s wlc
  
  # Add real servers
  ipvsadm -a -t 10.0.0.1:80 -r 192.168.1.10:8080 -m -w 100  # NAT mode
  ipvsadm -a -t 10.0.0.1:80 -r 192.168.1.11:8080 -g -w 100  # DR mode
  ipvsadm -a -t 10.0.0.1:80 -r 192.168.1.12:8080 -i -w 100  # TUN mode
```

**Failure modes:**
- **LB node failure:** BGP ECMP detects via BFD (Bidirectional Forwarding Detection, < 1s). Traffic re-hashes to surviving nodes. Some connections reset (mitigated by consistent hashing).
- **Conntrack table full:** New connections dropped. Monitor `nf_conntrack_count` vs `nf_conntrack_max`. Solution: increase max, reduce timeouts, or use eBPF (no conntrack dependency).
- **IPVS DR mode ARP issue:** Backend must not respond to ARP for the VIP. Use `arp_ignore=1` and `arp_announce=2` sysctl settings.

**Interviewer Q&As:**

> **Q1: When would you choose L4 over L7 load balancing?**
> A: Choose L4 when: (1) You need maximum throughput with minimum latency (e.g., first-tier load balancing). (2) The protocol is not HTTP (e.g., MySQL, Redis, MQTT). (3) You need TLS passthrough (end-to-end encryption without LB seeing plaintext). (4) Backend selection does not depend on request content. Choose L7 when: you need content-based routing, TLS termination, gRPC per-RPC balancing, HTTP/2 multiplexing, or request-level observability.

> **Q2: Why is IPVS preferred over iptables for kube-proxy?**
> A: iptables rules are O(n) per packet (linear chain traversal). With 10,000 services x 10 endpoints = 100,000 rules, this adds significant latency. IPVS uses hash tables for O(1) lookup. IPVS also supports more scheduling algorithms (8 vs iptables' random/round-robin). At 5,000+ services, IPVS outperforms iptables by 10x in throughput.

> **Q3: What is DSR (Direct Server Return) and when would you use it?**
> A: DSR (IPVS DR mode) lets backends respond directly to clients, bypassing the load balancer on the return path. This is ideal for asymmetric traffic (small requests, large responses like video streaming) because the LB only processes inbound packets, increasing throughput by up to 10x. The trade-off is: LB cannot modify responses, L2 adjacency is required, and backends must be configured to accept traffic for the VIP.

> **Q4: How does kube-proxy IPVS mode work internally?**
> A: kube-proxy watches the Kubernetes API for Service and Endpoint changes. For each Service, it creates an IPVS virtual server on the ClusterIP. For each endpoint (pod), it adds a real server. It uses dummy network interfaces (`kube-ipvs0`) to bind ClusterIPs. The default scheduling algorithm is `wlc` (weighted least connections). Connection draining uses `--ipvs-graceful-period`.

> **Q5: Why would you replace kube-proxy entirely with Cilium eBPF?**
> A: Cilium replaces kube-proxy by implementing service load balancing directly in eBPF programs attached to the TC (traffic control) hook or XDP. Benefits: (1) No iptables or IPVS rules to manage. (2) O(1) lookup in eBPF hash maps. (3) Socket-level load balancing (`connect()`-time redirection) avoids DNAT overhead entirely. (4) Better observability via Hubble. (5) Supports Maglev consistent hashing natively.

> **Q6: What is the difference between IPVS NAT, DR, and TUN modes in terms of return path?**
> A: NAT: return traffic goes through the LB (LB does SNAT on response). DR: return traffic goes directly from backend to client (LB only rewrites L2 MAC, not L3 IP; backend sends from VIP directly). TUN: return traffic goes directly from backend to client (backend decapsulates IP-in-IP tunnel, responds from VIP). NAT is simplest but makes LB a bottleneck. DR/TUN offload return traffic but require backend configuration.

---

### Deep Dive 2: Consistent Hashing and Maglev

**Why it's hard:**
When backends are added or removed (scaling events, failures, deployments), traditional hashing (hash(key) % N) remaps nearly all keys, destroying session affinity and cache locality. Consistent hashing minimizes remapping but introduces variance in load distribution. Maglev hashing (Google) provides both minimal disruption and uniform distribution, but is complex to implement.

**Approaches:**

| Algorithm | Disruption on Backend Change | Load Uniformity | Lookup Speed | Memory |
|-----------|------------------------------|-----------------|-------------|--------|
| Modulo hash (hash % N) | ~100% remapping | Perfect | O(1) | O(1) |
| Ring-based consistent hash (Karger) | K/N keys remapped (K=keys, N=backends) | Poor without virtual nodes | O(log N) | O(V*N) |
| Ring + virtual nodes (150-200 per backend) | K/N keys remapped | Good (< 10% variance) | O(log V*N) | O(V*N) |
| Jump consistent hash | K/N keys remapped | Perfect | O(ln N) | O(1) |
| Maglev hash (Google) | ~K/N keys remapped, bounded | Near-perfect | O(1) table lookup | O(M) where M is table size |
| Rendezvous (HRW) hash | K/N keys remapped | Good | O(N) per lookup | O(1) |

**Selected approach: Maglev hashing for L4 (eBPF), ring-based with virtual nodes for L7 (Envoy)**

**Justification:** Maglev's O(1) lookup fits eBPF's computational constraints (no loops, bounded execution). Ring-based consistent hashing is used by Envoy natively and is well-tested.

**Implementation detail — Maglev:**

```
Maglev Hash Table Construction:

  Table size M = large prime (e.g., 65537)
  Backends = [B0, B1, B2, ..., Bn-1]

  For each backend Bi:
    offset_i = hash1(Bi) % M
    skip_i   = hash2(Bi) % (M - 1) + 1   // skip > 0

    preference list for Bi:
      permutation[i][j] = (offset_i + j * skip_i) % M
      // This generates a permutation of [0, M-1]

  Fill lookup table entry[0..M-1]:
    Initialize all entries to -1
    next[i] = 0 for all i  // pointer into each backend's preference list

    repeat until table is full:
      for each backend i:
        // Find next empty slot in Bi's preference list
        c = permutation[i][next[i]]
        while entry[c] != -1:
          next[i]++
          c = permutation[i][next[i]]
        entry[c] = i
        next[i]++

  Lookup:
    backend = entry[hash(key) % M]
    // O(1) — single array lookup

Example with 3 backends, M=7:
  Preference lists (simplified):
    B0: [3, 0, 4, 1, 5, 2, 6]
    B1: [0, 2, 4, 6, 1, 3, 5]
    B2: [3, 4, 5, 6, 0, 1, 2]

  Round 1: B0→entry[3]=0, B1→entry[0]=1, B2→entry[4]=2 (3 taken by B0)
  Round 2: B0→entry[1]=0 (0 taken), B1→entry[2]=1, B2→entry[5]=2
  Round 3: B0→entry[5]=taken→entry[6]=0, B1→entry[6]=taken→done?, ...
  
  Final table: [B1, B0, B1, B0, B2, B2, B0]
  
  Backend removal (B1 dies):
    Rebuild table with B0, B2.
    Only entries previously assigned to B1 are remapped.
    B0 and B2's entries stay the same → minimal disruption.
```

**Ring-based consistent hashing with virtual nodes:**

```
  Hash ring: 0 ─────────────────────────── 2^32

  Backend B0 → 150 virtual nodes at hash positions:
    hash("B0-0"), hash("B0-1"), ..., hash("B0-149")
  
  Backend B1 → 150 virtual nodes:
    hash("B1-0"), hash("B1-1"), ..., hash("B1-149")

  For request with key K:
    position = hash(K)
    Walk clockwise on ring → first virtual node → maps to real backend

  Backend addition (B2 added):
    B2's 150 virtual nodes inserted on ring.
    Only keys between B2's new positions and the previous node are remapped.
    Approximately K/(N+1) keys move to B2 (optimal).

  Weighted backends:
    More virtual nodes = more ring coverage.
    B0 (weight 200) → 300 virtual nodes
    B1 (weight 100) → 150 virtual nodes
    B0 gets ~2x the traffic.
```

**Failure modes:**
- **Hash collision in Maglev:** M is prime and much larger than N, making collisions in the preference list rare. If two backends want the same slot, one yields (this is the Maglev algorithm's core mechanism).
- **Uneven load with few backends:** With 2-3 backends, even Maglev has some imbalance. Virtual nodes help: use 100+ per backend for < 5% variance.
- **Cascading failure:** When one backend fails, its traffic redistributes to others. If a second backend then fails under increased load, its traffic shifts again. This cascade can overload surviving backends. Mitigation: circuit breakers + admission control + bounded load (reject overflow).

**Interviewer Q&As:**

> **Q1: Why not just use modulo hashing?**
> A: Modulo hashing (`hash(key) % N`) remaps nearly all keys when N changes. If you go from 10 to 11 backends, ~90% of keys move. This destroys cache locality (memcached) and session affinity. Consistent hashing remaps only K/N keys (optimal minimum).

> **Q2: What is the "bounded load" variant of consistent hashing?**
> A: Google's bounded-load consistent hashing caps each backend at (1 + epsilon) * average_load. If a backend is at capacity, the request is forwarded to the next node on the ring. This prevents hot-spotting while preserving most of the consistent hashing locality. Epsilon is typically 0.25 (25% over-average).

> **Q3: How does Envoy implement consistent hashing?**
> A: Envoy supports `ring_hash` and `maglev` LB policies. For `ring_hash`, it creates a ring with configurable `minimum_ring_size` (default 1024) and `maximum_ring_size` (default 8M). The hash key is configurable: source IP, HTTP header, cookie, or query parameter. Envoy recomputes the ring when endpoints change (via EDS updates from the control plane).

> **Q4: How do you handle consistent hashing when using HTTP/2 multiplexing?**
> A: HTTP/2 multiplexes many requests over one TCP connection. For L4 consistent hashing, all requests on the same connection go to the same backend (connection-level affinity). For per-request consistent hashing, you need L7: the LB inspects each HTTP/2 frame independently and routes based on request-level attributes (e.g., a header value).

> **Q5: What happens to consistent hashing during a rolling deployment?**
> A: Kubernetes rolling update creates new pods and terminates old ones. Each endpoint change triggers a hash ring update. If updating 10-pod service one at a time, 10 successive ring updates occur, each moving ~1/10 of traffic. To minimize disruption: use maxSurge=1, maxUnavailable=0, and ensure the new pod's IP is added before the old pod's IP is removed (overlapping).

> **Q6: How would you implement consistent hashing in eBPF?**
> A: Use a pre-computed Maglev lookup table stored in a `BPF_MAP_TYPE_ARRAY` (fixed-size array, O(1) lookup). Table size = 65537 (prime). The eBPF program computes `hash(5-tuple) % 65537` and indexes into the array to get the backend ID. Table regeneration happens in user-space when backends change and is atomically swapped (BPF map-in-map or array update).

---

### Deep Dive 3: Health Checking — Active and Passive

**Why it's hard:**
Health checking must quickly detect backend failures (seconds, not minutes) without overwhelming backends with probe traffic. False positives remove healthy backends, reducing capacity. False negatives route traffic to dead backends, increasing errors. The optimal balance depends on failure characteristics (crash vs. gradual degradation).

**Approaches:**

| Approach | Detection Speed | Overhead | False Positives | False Negatives |
|----------|----------------|----------|-----------------|-----------------|
| Active TCP SYN | 3-10s | Low (1 SYN per interval) | Low | Medium (process crash ≠ TCP close) |
| Active HTTP GET /healthz | 3-10s | Medium | Low | Low (app-level check) |
| Active gRPC health protocol | 3-10s | Medium | Low | Low |
| Passive (error rate tracking) | Real-time | Zero (piggyback on real traffic) | Medium (transient errors) | Low under traffic |
| Passive (latency degradation) | Real-time | Zero | Medium | Low |
| Combined active + passive | Best of both | Medium | Lowest (corroborate) | Lowest |

**Selected approach: Combined active + passive with configurable thresholds**

**Implementation detail:**

```
Active Health Check:
  ┌────────────────────────────────────────────────────┐
  │  Health Check Worker (runs on LB control plane)     │
  │                                                      │
  │  For each backend in each pool:                      │
  │    Every {interval} seconds (default: 5s):           │
  │      Send probe:                                     │
  │        TCP:  SYN → wait for SYN-ACK (timeout: 3s)   │
  │        HTTP: GET /healthz → expect 200 (timeout: 3s) │
  │        gRPC: grpc.health.v1.Health/Check             │
  │                                                      │
  │    Track consecutive results:                        │
  │      consecutive_pass++  or  consecutive_fail++       │
  │                                                      │
  │    State transitions:                                │
  │      healthy → unhealthy:                            │
  │        consecutive_fail >= unhealthy_threshold (3)    │
  │      unhealthy → healthy:                            │
  │        consecutive_pass >= healthy_threshold (5)      │
  │                                                      │
  │    Jitter: ±20% on interval to avoid probe storms    │
  └────────────────────────────────────────────────────┘

Passive Health Check (Outlier Detection):
  ┌────────────────────────────────────────────────────┐
  │  Per-backend sliding window (30 seconds):            │
  │                                                      │
  │  Track:                                              │
  │    - Total requests                                  │
  │    - 5xx responses                                   │
  │    - Timeouts                                        │
  │    - Connection errors                               │
  │                                                      │
  │  Eject if:                                           │
  │    5xx_rate > 50% AND total_requests > 100            │
  │    (minimum volume to avoid false positives)          │
  │                                                      │
  │  Ejection:                                           │
  │    Remove from LB pool for {base_ejection_time}       │
  │    Each consecutive ejection doubles the time         │
  │    Max ejection time: 300 seconds                    │
  │    Max ejected: 50% of pool (prevent total drain)    │
  │                                                      │
  │  Re-admit:                                           │
  │    After ejection time expires, re-admit              │
  │    First request is a "canary" — if it succeeds,     │
  │    backend stays healthy; if it fails, re-eject      │
  └────────────────────────────────────────────────────┘
```

**Failure modes:**
- **Probe flood:** With 20,000 backends and 5s interval, that is 4,000 probes/second. Manageable, but use jitter and distributed probing (each LB node probes a subset).
- **Health check endpoint slow but service functional:** Some backends have slow /healthz (e.g., dependency check). Solution: separate liveness and readiness checks; LB uses readiness only.
- **Network partition between LB and backend:** Backend is healthy but LB cannot reach it. Active check marks it unhealthy. If passive check sees success (traffic from other paths), keep it healthy. Cross-validate active and passive.
- **Thundering herd on recovery:** When a backend recovers from an outage, all LBs simultaneously re-admit it, causing a traffic spike. Solution: gradual re-admission via slow-start (linearly increase weight over 30 seconds).

**Interviewer Q&As:**

> **Q1: Why do you need both active and passive health checks?**
> A: Active checks detect failures even when there is no traffic to a backend (e.g., a backend that just started and has not yet received requests). Passive checks detect degradation in real-time without additional probe traffic. Combined, they provide the fastest and most accurate detection. Active catches crash failures; passive catches performance degradation.

> **Q2: How do you prevent the health checker from marking all backends unhealthy during a network blip?**
> A: Two safeguards: (1) `max_ejection_percent` (default 50%) ensures at least half the pool stays active even if checks fail. (2) The `panic_threshold`: if healthy backends drop below 50%, the LB routes to ALL backends (including unhealthy) to avoid total outage. It is better to send some traffic to degraded backends than to have zero capacity.

> **Q3: What is slow-start and why is it important?**
> A: When a new or recovered backend joins the pool, immediately routing full traffic to it can overwhelm it (cold caches, connection pool warm-up). Slow-start linearly ramps the backend's effective weight from 0 to its configured weight over a configurable period (default 30 seconds). Envoy supports this natively via `slow_start_config`.

> **Q4: How do Kubernetes readiness probes interact with LB health checks?**
> A: Kubernetes readiness probes control whether a pod's IP appears in the Endpoints object. If readiness fails, the pod is removed from Endpoints, and kube-proxy/IPVS removes it from the LB pool. External LB health checks are an additional layer: they verify the pod is reachable from the LB, not just from kubelet. Both should be configured with consistent thresholds.

> **Q5: How do you handle health checks for UDP services?**
> A: UDP is connectionless, so there is no SYN-ACK to verify. Options: (1) Send an application-level probe and expect a response (e.g., DNS query for DNS backends). (2) Send a UDP packet and check for ICMP Port Unreachable (indicates the port is closed). (3) Use a sidecar HTTP health endpoint that reflects the UDP service's health.

> **Q6: How do you monitor the health checker itself?**
> A: The health checker exposes its own metrics: probe latency, probe success rate, number of backends per state. If the health checker itself is down, LB nodes use their last-known health state (stale but safe). A meta-health-check (watchdog timer) restarts the health checker if it stops emitting heartbeats.

---

### Deep Dive 4: eBPF/XDP-Based Load Balancing

**Why it's hard:**
eBPF programs run in the Linux kernel with strict constraints: bounded execution time, no unbounded loops, limited stack size (512 bytes), restricted helper functions. Writing a high-performance load balancer in eBPF requires careful data structure design, efficient hash computation, and graceful handling of edge cases — all within these constraints.

**Approaches:**

| Approach | Throughput | Latency | Programmability | Maturity |
|----------|-----------|---------|-----------------|----------|
| iptables (user-space rules, kernel-space exec) | 1-2M pps | 10-50 us per rule chain | Low | Very mature |
| IPVS (kernel module) | 5-10M pps | 5-20 us | Medium (8 algorithms) | Mature |
| eBPF/TC (traffic control hook) | 10-20M pps | 5-15 us | High | Mature (5.x kernels) |
| eBPF/XDP (eXpress Data Path) | 20-40M pps | 1-5 us | High | Mature (5.x kernels) |
| DPDK (user-space networking) | 40M+ pps | < 1 us | Very high | Mature but complex |
| Hardware offload (SmartNIC) | 100M+ pps | < 1 us | Low (vendor-specific) | Emerging |

**Selected approach: eBPF/XDP for L4 load balancing with TC fallback**

**Justification:** XDP runs before the kernel network stack, giving maximum performance. TC hook handles cases where XDP cannot (e.g., VXLAN decapsulation). No user-space kernel bypass (DPDK) avoids operational complexity of dedicated cores and hugepages.

**Implementation detail:**

```
XDP Program Structure:

  SEC("xdp")
  int lb_xdp(struct xdp_md *ctx) {
      // 1. Parse Ethernet header
      struct ethhdr *eth = data;
      if (eth->h_proto != ETH_P_IP) return XDP_PASS;
      
      // 2. Parse IP header
      struct iphdr *ip = data + sizeof(*eth);
      if (ip->protocol != IPPROTO_TCP && ip->protocol != IPPROTO_UDP)
          return XDP_PASS;
      
      // 3. Parse TCP/UDP header
      struct tcphdr *tcp = data + sizeof(*eth) + ip->ihl * 4;
      
      // 4. Build 5-tuple key
      struct flow_key key = {
          .src_ip = ip->saddr,
          .dst_ip = ip->daddr,
          .src_port = tcp->source,
          .dst_port = tcp->dest,
          .protocol = ip->protocol
      };
      
      // 5. Check if dst_ip:dst_port is a VIP
      struct vip_info *vip = bpf_map_lookup_elem(&vip_map, &key.dst_ip_port);
      if (!vip) return XDP_PASS;  // Not a VIP, pass to kernel
      
      // 6. Check connection tracking
      struct ct_entry *ct = bpf_map_lookup_elem(&conntrack_map, &key);
      if (ct) {
          // Existing connection: use cached backend
          backend = ct->backend;
      } else {
          // New connection: select backend via Maglev
          __u32 hash = jhash_3words(key.src_ip, key.dst_ip,
                                     (key.src_port << 16) | key.dst_port, 0);
          __u32 idx = hash % MAGLEV_TABLE_SIZE;
          struct backend_info *backend_entry = 
              bpf_map_lookup_elem(&maglev_table, &idx);
          if (!backend_entry) return XDP_DROP;
          
          // Create conntrack entry
          struct ct_entry new_ct = { .backend = *backend_entry };
          bpf_map_update_elem(&conntrack_map, &key, &new_ct, BPF_ANY);
          backend = *backend_entry;
      }
      
      // 7. DNAT: rewrite destination IP and port
      ip->daddr = backend.ip;
      tcp->dest = backend.port;
      
      // 8. Recalculate checksums (incremental)
      // ... (IP checksum + TCP pseudo-header checksum)
      
      // 9. Rewrite L2 (MAC) for next hop
      __builtin_memcpy(eth->h_dest, backend.mac, ETH_ALEN);
      __builtin_memcpy(eth->h_source, local_mac, ETH_ALEN);
      
      // 10. Redirect to backend's interface (or XDP_TX if same NIC)
      return bpf_redirect(backend.ifindex, 0);
  }

BPF Maps:
  vip_map:       BPF_MAP_TYPE_HASH       {vip_ip:port → pool_id}
  maglev_table:  BPF_MAP_TYPE_ARRAY      {index → backend_info}
  conntrack_map: BPF_MAP_TYPE_LRU_HASH   {5-tuple → backend_info}  // LRU eviction
  backend_map:   BPF_MAP_TYPE_HASH       {backend_id → backend_info}
  stats_map:     BPF_MAP_TYPE_PERCPU_ARRAY {backend_id → packets, bytes}

Performance (bare-metal, 100G NIC):
  - XDP native mode: 24M packets/second single core
  - XDP generic mode: 5M packets/second (fallback, no driver support)
  - With conntrack lookup: ~15M packets/second
  - Maglev table lookup: ~10ns (single array index)
```

**Real-world implementations:**

```
Facebook Katran:
  - Open-source XDP-based L4 LB
  - Handles all Facebook edge traffic
  - Maglev hashing + GUE (Generic UDP Encapsulation)
  - Stateless design: no conntrack for UDP, minimal for TCP
  - Kernel 4.18+ required
  
Cilium Service Load Balancing:
  - Replaces kube-proxy entirely
  - eBPF at TC hook (not XDP, for compatibility)
  - Socket-level LB: hooks connect() syscall
    → Rewrites destination at socket creation time
    → No DNAT, no conntrack, no per-packet overhead
  - Supports Maglev, random, weighted round robin
  
Google Maglev:
  - Hardware-based L4 LB (not eBPF)
  - Custom consistent hashing algorithm (Maglev hash)
  - Handles Google's entire frontend traffic
  - Connection tracking via GRE encapsulation
```

**Failure modes:**
- **eBPF verifier rejects program:** Programs must pass the kernel verifier (bounded loops, no invalid memory access). Solution: careful coding, use bounded loop helpers (bpf_loop), keep programs simple.
- **Conntrack table exhaustion (LRU):** LRU eviction drops oldest entries. Under heavy load, short-lived connections may not have conntrack entries when response arrives. Solution: use DSR mode (response does not traverse LB) or increase map size.
- **XDP not supported by NIC driver:** Fall back to XDP generic mode (software, slower) or TC hook. Check `ethtool -i <iface>` for XDP support.
- **BPF map atomic update races:** Use per-CPU maps for counters. For shared state (conntrack), use `bpf_spin_lock` or accept benign races.

**Interviewer Q&As:**

> **Q1: What is the difference between XDP and TC hooks in eBPF?**
> A: XDP runs at the earliest point in the network stack (before sk_buff allocation), giving maximum performance. TC hook runs after sk_buff is created but before routing decision, giving access to more metadata (e.g., VXLAN inner headers). XDP can only see raw packet data; TC can access socket context. Use XDP for pure L4 LB; use TC when you need tunneling or socket-level features.

> **Q2: How does Cilium's socket-level LB avoid DNAT?**
> A: Cilium hooks the `connect()` system call via a BPF_PROG_TYPE_CGROUP_SOCK_ADDR program. When a pod calls `connect(ClusterIP:port)`, the eBPF program rewrites the destination to a backend pod IP before the TCP SYN is sent. The kernel creates the connection directly to the backend. No DNAT, no conntrack, no iptables rules. This is the most efficient form of load balancing possible.

> **Q3: How do you handle eBPF program updates without dropping packets?**
> A: eBPF programs are atomically replaced. The kernel swaps the function pointer, so there is no window where packets are dropped. BPF maps are independent of programs, so state (conntrack, Maglev table) persists across program updates. For map updates, use `bpf_map_update_elem` which is atomic per entry.

> **Q4: What are the limitations of XDP-based load balancing?**
> A: (1) No connection state for the first packet of new flows if using stateless mode. (2) Cannot inspect application-layer content (no L7 features). (3) Requires NIC driver support for native mode (otherwise falls back to generic mode with lower performance). (4) 512-byte stack limit restricts local variable usage. (5) Cannot call arbitrary kernel functions — only BPF helper functions.

> **Q5: How does Katran handle TCP connection tracking without state?**
> A: For the initial SYN packet, Katran selects a backend via Maglev hash (deterministic, no state needed). For subsequent packets of the same flow, the 5-tuple hash gives the same result (Maglev is deterministic for the same input). If a backend is removed, only its flows are affected (Maglev minimizes disruption). For long-lived connections that outlast backend changes, Katran uses a small LRU conntrack table for active flows.

> **Q6: How do you debug eBPF load balancer issues in production?**
> A: (1) `bpftool prog show` to list loaded programs. (2) `bpftool map dump` to inspect map contents (conntrack, Maglev table). (3) `bpf_trace_printk()` for printf-style debugging (visible in `/sys/kernel/debug/tracing/trace_pipe`). (4) BPF perf events for structured logging. (5) Per-CPU stats maps for counters (packets, bytes, drops per reason). (6) `tcpdump` on a different interface to capture redirected packets.

---

## 7. Scaling Strategy

**L4 tier scaling:**
- ECMP (Equal Cost Multi-Path) via BGP: multiple L4 LB nodes announce the same VIP. The upstream router distributes flows across them.
- Scale by adding LB nodes. Each node handles 10M+ pps with eBPF/XDP.
- Consistent hashing (Maglev) across ECMP paths minimizes connection disruption when nodes are added/removed.

**L7 tier scaling:**
- L4 tier distributes across L7 nodes. Add L7 nodes linearly.
- Each L7 node (Envoy) handles 100K-500K RPS depending on feature usage.
- Autoscale based on CPU (target 60%) or active connection count.

**Backend scaling:**
- Kubernetes HPA/VPA for pod autoscaling.
- LB dynamically discovers new endpoints via Kubernetes API watches or xDS updates.
- Slow-start for new backends prevents overload.

**Cross-DC scaling:**
- GeoDNS or Anycast directs clients to nearest DC.
- Each DC has independent L4+L7 tiers.
- Failover: withdraw BGP routes for unhealthy DC.

**Interviewer Q&As:**

> **Q1: How do you handle a 100x traffic spike (e.g., flash sale)?**
> A: L4 tier is pre-provisioned for peak (eBPF nodes are cheap — no per-connection state). L7 tier autoscales. Backend services have HPA with custom metrics. During extreme spikes: (1) Enable connection queue (accept but delay processing). (2) Activate load shedding (return 503 for low-priority requests). (3) Serve cached responses for idempotent endpoints. Pre-scaling before known events is preferred.

> **Q2: What is the bottleneck as you scale the L4 tier?**
> A: At extreme scale (100M+ pps), the bottleneck shifts to: (1) NIC interrupt processing (use RSS — Receive Side Scaling — to distribute across CPU cores). (2) Memory bandwidth for conntrack lookups (use per-CPU maps). (3) BGP ECMP path limits (most routers support 64-128 ECMP paths). Beyond this: SmartNIC offload or DPDK.

> **Q3: How do you handle asymmetric traffic patterns (small requests, huge responses)?**
> A: Use DSR (Direct Server Return). The LB only processes inbound packets; backends respond directly to clients. This is critical for streaming services where responses are 1000x larger than requests. The LB's egress bandwidth is no longer a bottleneck.

> **Q4: How do you scale connection tracking for millions of concurrent connections?**
> A: eBPF LRU hash map auto-evicts old entries. Size the map for expected concurrent connections (1M entries = ~320 MB). For stateless protocols (UDP, short-lived TCP), minimize conntrack: Maglev hashing gives deterministic backend selection without stored state. For long-lived connections (WebSocket), ensure conntrack entries have appropriate timeouts (hours, not seconds).

> **Q5: How do you handle LB scaling across multiple availability zones?**
> A: Each AZ has its own L4 LB pair. BGP Anycast routes traffic to the nearest AZ. Within AZ, ECMP distributes across LB nodes. AZ-aware routing: prefer same-AZ backends to minimize cross-AZ latency (Envoy's zone-aware routing achieves this). If an AZ's LB tier fails, Anycast BGP routes withdraw and traffic shifts to other AZs.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Single L4 LB node crash | BGP/BFD detects route withdrawal (< 1s) | Flows on that node reset | ECMP re-hashes to surviving nodes; Maglev minimizes disruption | < 3s |
| All L4 nodes in one ECMP group | BGP route withdrawal for VIP | VIP unreachable from that path | Multiple ECMP groups across ToR switches; Anycast to other DC | < 10s |
| Single L7 LB node crash | L4 active health check fails (TCP, 3 consecutive) | Requests on that node fail | L4 removes from pool; HTTP clients retry | < 15s |
| Backend pod crash | Active health check + passive (5xx rate) | Requests to that pod fail | LB removes pod; Kubernetes restarts pod | < 10s (health check) |
| Backend service full outage (all pods) | All backends in pool marked unhealthy | Service completely down | Circuit breaker returns 503; panic mode routes to all backends | Immediate (degraded) |
| Network partition between L4 and L7 | Health check timeout | L4 cannot reach L7 nodes | L4 marks L7 nodes unhealthy; traffic shifts to reachable L7 nodes | < 15s |
| Conntrack table full (eBPF) | LRU eviction metrics spike | New flows may hit wrong backend (re-hashed) | Monitor conntrack utilization; scale map size; use DSR to reduce state | Immediate (degraded) |
| BGP session flap | BGP keepalive timeout (default 90s, BFD < 1s) | Traffic blackholed during flap | BFD (Bidirectional Forwarding Detection) for sub-second detection | < 3s with BFD |
| Config push failure (bad xDS update) | Version mismatch detection | LB nodes stuck on old config | xDS NACK mechanism; control plane rolls back | Config frozen (safe) |
| TLS certificate expiry (L7) | cert-manager renewal monitoring | TLS handshake failures | Auto-renew 30 days before; fallback to wildcard cert | Immediate if renewed |
| Redis failure (for session persistence) | Redis Sentinel/Cluster health | Sticky sessions degraded to IP hash | Fall back to source IP hash; sessions may shift | Degraded immediately |
| Kernel panic on LB node | BFD / ECMP detects no response | Node completely down | Automatic failover via ECMP; node auto-restarts | < 3s (BFD) to < 90s (BGP) |

---

## 9. Security

**Network Security:**
- L4 LB nodes are in a dedicated management VLAN with strict ACLs.
- Only BGP peers (ToR switches) can send traffic to LB VIPs.
- eBPF programs drop malformed packets (invalid IP headers, impossible flag combinations).
- SYN flood protection via SYN cookies (kernel-level) and XDP SYN proxy.

**DDoS Mitigation at L4:**
- Rate limit new connections per source IP (configurable per VIP).
- XDP program drops packets matching known attack patterns (amplification attacks, malformed packets).
- Integration with upstream DDoS scrubbing (Cloudflare, Akamai) for volumetric attacks beyond NIC capacity.
- Automatic blocklist: if source IP exceeds error threshold, add to XDP drop map (BPF_MAP_TYPE_LPM_TRIE for CIDR blocks).

**TLS Security (L7):**
- TLS 1.3 preferred; TLS 1.2 with strong ciphers as fallback.
- OCSP stapling for certificate revocation checking.
- Client certificate verification for mTLS endpoints.
- No SSLv3 or TLS 1.0/1.1.

**Access Control:**
- LB management API requires mTLS + RBAC.
- eBPF program loading requires `CAP_BPF` + `CAP_NET_ADMIN` capabilities.
- Configuration changes audited (who, what, when).

**Supply Chain:**
- eBPF programs compiled from audited source, signed, and verified before loading.
- Envoy binary verified via cosign/sigstore.

---

## 10. Incremental Rollout

**Phase 1 (Weeks 1-3): L4 Tier (eBPF/XDP)**
- Deploy eBPF LB on 2 bare-metal nodes in a test cluster.
- Mirror production traffic via port mirroring (tap); compare LB decisions with existing system.
- Validate Maglev hashing distribution and conntrack correctness.

**Phase 2 (Weeks 4-6): L4 Production (Canary)**
- Add eBPF LB nodes to production ECMP group with weight 1 (receives 10% of flows).
- Monitor packet drop rate, conntrack utilization, backend health check accuracy.
- Gradually increase ECMP weight to 50%.

**Phase 3 (Weeks 7-9): L7 Tier (Envoy)**
- Deploy Envoy L7 nodes behind the L4 tier.
- Route 1 low-traffic service through the new L7 tier.
- Enable TLS termination, health checking, and least-connections algorithm.

**Phase 4 (Weeks 10-14): Full L7 Migration**
- Migrate services one by one to the new L7 tier.
- Enable consistent hashing for stateful services (session store, cache).
- Decommission old LB infrastructure.

**Phase 5 (Weeks 15-18): GSLB & Advanced Features**
- Deploy Anycast BGP for cross-DC GSLB.
- Enable zone-aware routing.
- Implement connection draining and slow-start.

**Rollout Q&As:**

> **Q1: How do you validate eBPF LB correctness before production?**
> A: (1) Unit tests using BPF test infrastructure (`bpf_prog_test_run`). (2) Integration tests with synthetic traffic (iperf3, wrk). (3) Traffic mirroring: replay production pcap files through the eBPF program and compare decisions with the existing LB. (4) Formal verification tools (e.g., Prevail verifier) for correctness properties.

> **Q2: How do you roll back from eBPF LB to the old system?**
> A: Withdraw the eBPF LB nodes from the ECMP group (BGP route withdrawal). Traffic immediately shifts to remaining nodes running the old LB. eBPF programs are unloaded. Total rollback time: < 30 seconds.

> **Q3: How do you handle the transition period where both old and new LBs are active?**
> A: ECMP distributes flows across both. Each LB independently selects backends. Connection affinity is maintained per-LB (different flows may be on different LBs). Session persistence may be affected during transition. Solution: use consistent hashing on both LBs with the same algorithm and table, so they make the same backend selection for the same client.

> **Q4: How do you measure the performance improvement of eBPF vs. iptables?**
> A: Before/after benchmarks using: (1) `perf stat` for CPU cycles per packet. (2) `bpftool prog profile` for eBPF-specific metrics. (3) End-to-end latency measurement using hardware timestamping. (4) Throughput test: saturate the NIC and measure max pps. Expected improvement: 5-10x throughput, 10x latency reduction.

> **Q5: What is the risk of running eBPF in the kernel?**
> A: eBPF programs are verified by the kernel verifier before loading, preventing: out-of-bounds access, infinite loops, invalid memory access. The verifier guarantees termination and memory safety. Risk is mitigated but not zero: kernel bugs in the verifier itself (rare), or unexpected behavior from complex eBPF logic. We mitigate with extensive testing and canary deployment.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Trade-off |
|----------|-------------------|--------|-----------|-----------|
| L4 technology | iptables, IPVS, eBPF/XDP, DPDK | eBPF/XDP | Best throughput without user-space networking complexity; kernel-integrated | Requires modern kernel (5.15+); eBPF expertise needed |
| L7 technology | Nginx, HAProxy, Envoy, Traefik | Envoy | xDS dynamic config, gRPC native, consistent hashing, extensive observability | Higher memory usage than HAProxy; steeper learning curve |
| Consistent hashing | Ring (Karger), Maglev, Jump, Rendezvous | Maglev (L4) + Ring (L7) | Maglev: O(1) lookup for eBPF; Ring: Envoy-native | Maglev table rebuild on backend change; Ring: O(log N) lookup |
| Health checking | Active only, passive only, combined | Combined active + passive | Fastest detection (passive) + works without traffic (active) | More complexity; must handle conflicting signals |
| GSLB approach | GeoDNS only, Anycast only, both | Anycast (primary) + GeoDNS (fallback) | Anycast: automatic failover via BGP; GeoDNS: latency-optimal | Anycast: debugging harder (same IP, different DC); GeoDNS: TTL-dependent failover |
| East-west LB | kube-proxy (iptables), kube-proxy (IPVS), Cilium eBPF | Cilium eBPF | Replaces kube-proxy; socket-level LB eliminates DNAT; Hubble observability | Lock-in to Cilium CNI; newer technology |
| DSR mode | NAT, DR, TUN | DSR (DR mode) for L4 tier | 10x throughput (LB only handles ingress) | Backend must accept VIP traffic; LB cannot modify responses |
| Conntrack implementation | Kernel conntrack, eBPF LRU hash, stateless | eBPF LRU hash + stateless fallback | Avoids kernel conntrack overhead; LRU auto-evicts | LRU eviction can cause flow disruption for long-lived connections |

---

## 12. Agentic AI Integration

### AI-Powered Load Balancing

**Predictive Autoscaling Agent:**
- Analyzes historical traffic patterns (daily, weekly, seasonal).
- Predicts traffic demand 30-60 minutes ahead.
- Pre-scales L7 LB tier and backend services before demand arrives.
- Learns from past prediction errors to improve accuracy.
- Constraint: never scale below minimum replica count; never scale above budget limit.

**Intelligent Backend Selection Agent:**
- Monitors per-backend latency, error rate, and resource utilization in real-time.
- Dynamically adjusts backend weights beyond what static algorithms provide.
- Detects "gray failures" (backend responds but slowly) that health checks miss.
- Example: backend B3 has 2x latency of others (CPU throttled) — agent reduces B3's weight by 50% while alerting ops.

**Anomaly Detection Agent:**
- Monitors traffic patterns per VIP/service.
- Detects: (1) Unusual traffic sources (new IP ranges). (2) Protocol anomalies (unusual header patterns). (3) Traffic volume anomalies (sudden 10x spike from one client).
- Automatically applies temporary rate limits or blocklists.
- Escalates to human for sustained anomalies.

**Capacity Planning Agent:**
- Analyzes long-term traffic growth trends.
- Predicts when current LB capacity will be exhausted.
- Generates capacity planning reports with hardware procurement timelines.
- Recommends: "Add 2 L4 LB nodes and 5 L7 nodes by Q3 to handle projected 30% traffic growth."

**Configuration Optimization Agent:**
- Analyzes current load balancing algorithm effectiveness per service.
- Recommends algorithm changes: "Service X has 40% latency variance — switch from round_robin to least_connections."
- Simulates configuration changes against historical traffic before recommending.
- Validates that proposed changes do not violate SLOs.

### Integration Architecture

```
┌──────────────────────────────────────────────────────┐
│                AI Agent Platform                       │
│                                                        │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐ │
│  │  Metrics     │  │  Traffic     │  │  Config      │ │
│  │  Collector   │  │  Analyzer    │  │  Recommender │ │
│  │  (Prometheus │  │  (ML models) │  │  (Rule +     │ │
│  │   queries)   │  │              │  │   LLM-based) │ │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘ │
│         │                 │                  │          │
│         └─────────────────┼──────────────────┘          │
│                           │                             │
│                    ┌──────▼───────┐                     │
│                    │  Decision    │                     │
│                    │  Engine      │                     │
│                    │  (approve/   │                     │
│                    │   reject/    │                     │
│                    │   escalate)  │                     │
│                    └──────┬───────┘                     │
│                           │                             │
│                    ┌──────▼───────┐                     │
│                    │  Action      │                     │
│                    │  Executor    │                     │
│                    │  (config     │                     │
│                    │   push, API  │                     │
│                    │   calls)     │                     │
│                    └──────────────┘                     │
│                                                        │
│  Safety guardrails:                                    │
│  - Max 10% weight change per decision                  │
│  - Minimum 30s between consecutive changes             │
│  - Auto-rollback on error rate spike (> 2x baseline)   │
│  - All decisions logged with full reasoning chain       │
│  - Human approval required for: algorithm changes,      │
│    backend removal, GSLB policy changes                 │
└──────────────────────────────────────────────────────┘
```

---

## 13. Complete Interviewer Q&A Bank

> **Q1: Explain the difference between L4 and L7 load balancing with concrete examples.**
> A: L4 operates on TCP/UDP packets using the 5-tuple (src/dst IP, src/dst port, protocol). It cannot inspect HTTP content. Example: distribute MySQL connections across 3 replicas. L7 operates on HTTP/gRPC request content. It can route based on URL path, host header, cookies. Example: route `/api/v1/*` to service-A and `/api/v2/*` to service-B. L4 is 100x faster but L7 is 100x more flexible.

> **Q2: How does IPVS work internally and why is it faster than iptables?**
> A: IPVS uses hash tables for O(1) service lookup, while iptables traverses rule chains linearly O(n). IPVS is implemented as a kernel module that intercepts packets at the LOCAL_IN netfilter hook. It maintains a connection table (hash of 5-tuple to backend) and supports 8 scheduling algorithms. At 5,000+ services, IPVS handles 10x more throughput than iptables.

> **Q3: What is Maglev hashing and how does it differ from ring-based consistent hashing?**
> A: Maglev uses a pre-computed lookup table (size M, prime) where each entry maps to a backend. Lookup is O(1) — single array index. Ring-based hashing uses a sorted ring with virtual nodes; lookup is O(log N) binary search. Maglev provides better load uniformity (each backend gets exactly M/N entries). Both achieve minimal disruption (~K/N) on backend changes.

> **Q4: How does DSR (Direct Server Return) work and when would you use it?**
> A: The LB rewrites only the L2 destination MAC (not L3 IP) on inbound packets. The backend receives the packet with the VIP as the destination IP and responds directly to the client from the VIP. The LB never sees the response. Use case: video streaming, large file downloads — where response bandwidth is 100x request bandwidth.

> **Q5: Explain how kube-proxy works in IPVS mode.**
> A: kube-proxy watches the Kubernetes API for Service and EndpointSlice objects. For each ClusterIP Service, it creates an IPVS virtual server. For each ready endpoint, it adds a real server. It binds ClusterIPs to a dummy interface (`kube-ipvs0`) so the kernel accepts packets for those IPs. Default algorithm: weighted least connections. It also manages iptables rules for SNAT (masquerade) when traffic leaves the node.

> **Q6: How does Cilium replace kube-proxy with eBPF?**
> A: Cilium attaches eBPF programs at the TC (traffic control) hook and, for socket-level LB, at the cgroup socket hook. The socket-level program intercepts `connect()` calls and rewrites the destination from ClusterIP to a backend pod IP before the TCP handshake begins. This eliminates DNAT and conntrack entirely. Cilium also supports XDP for north-south L4 load balancing.

> **Q7: What is the difference between Anycast and GeoDNS for GSLB?**
> A: Anycast: the same IP address is announced via BGP from multiple data centers. Routers naturally deliver packets to the nearest announcement (shortest BGP path). Failover is automatic (BGP route withdrawal). GeoDNS: different DNS answers based on client location (IP geolocation). Failover depends on DNS TTL (slower). Anycast is better for failover; GeoDNS is better for latency optimization.

> **Q8: How do you handle the "thundering herd" problem when a backend recovers?**
> A: Slow-start: when a backend is re-admitted to the pool, its effective weight starts at 1 and linearly increases to its configured weight over 30-60 seconds. This prevents the recovered backend from being overwhelmed. Additionally, the LB's least-connections algorithm naturally avoids overloading a backend that is still warming up (fewer existing connections ≠ more capacity).

> **Q9: How do you implement session persistence without consistent hashing?**
> A: Cookie-based: the L7 LB inserts a cookie (e.g., `SERVERID=backend-3`) in the response. Subsequent requests include this cookie, and the LB routes to the specified backend. If that backend is down, the LB selects a new one and updates the cookie. This is more precise than IP-based affinity (which fails with NAT/proxy) and does not require consistent hashing.

> **Q10: What is the "small-world" problem in load balancing?**
> A: When multiple layers of load balancers each independently make decisions, traffic may become unevenly distributed. Example: 2 L4 LBs each send to 4 L7 LBs. If both L4 LBs happen to hash a popular client to the same L7 LB, that L7 gets 2x traffic. Solution: use different hash functions at each layer, or share load information between layers (power-of-two-choices).

> **Q11: How do you load balance gRPC traffic (which uses HTTP/2)?**
> A: gRPC multiplexes RPCs over a single HTTP/2 connection. L4 LB balances at the connection level (one connection = one backend), causing hot-spotting for long-lived gRPC streams. Solution: use L7 LB that terminates HTTP/2 and balances per-RPC. Envoy supports this natively. Alternative: client-side load balancing using gRPC's built-in LB (round_robin, pick_first) with a name resolver.

> **Q12: How do you handle LB for UDP services?**
> A: UDP is connectionless, so there is no SYN/FIN to track connection state. Options: (1) Use source IP hash for session affinity (deterministic, no state). (2) Use short-TTL conntrack entries (5 seconds for UDP). (3) For QUIC (UDP-based HTTP/3): use connection ID hash for affinity (QUIC connection IDs survive IP changes). (4) Health checking is harder — use application-level probes.

> **Q13: What is the power-of-two-choices algorithm?**
> A: Instead of always selecting the backend with least connections (which requires global state), randomly pick 2 backends and send to the one with fewer connections. This gives near-optimal load distribution with only local information. It avoids the herd behavior of least-connections (all LBs simultaneously choosing the same backend). Used in Envoy as `LEAST_REQUEST` with `choice_count=2`.

> **Q14: How do you handle connection draining during a rolling deployment?**
> A: When a pod is terminating: (1) Kubernetes removes the pod from Endpoints. (2) kube-proxy/LB removes it from the backend pool (no new connections). (3) Existing connections continue until they complete or `terminationGracePeriodSeconds` expires. (4) The pod's `preStop` hook can delay SIGTERM to allow in-flight requests to finish. The LB supports configurable drain timeout.

> **Q15: How do you implement zone-aware routing?**
> A: Prefer same-zone backends to minimize cross-zone latency and data transfer costs. Envoy implements this: if the local zone has sufficient healthy backends (> 70%), route 100% locally. If local zone health drops below threshold, proportionally spill to other zones. Backends are tagged with zone metadata (from Kubernetes node labels). This can save 30-50% on cross-AZ data transfer costs.

> **Q16: What is the difference between connection-level and request-level load balancing?**
> A: Connection-level: one LB decision per TCP connection. All requests on that connection go to the same backend. Works for HTTP/1.1 (one request per connection at a time) but causes hot-spotting with HTTP/2 (many concurrent requests per connection). Request-level: LB decides per HTTP request, even on the same connection. Requires L7 LB that terminates the connection and manages independent upstream connections.

> **Q17: How do you load balance across heterogeneous backends (different CPU/memory)?**
> A: Weighted round robin or weighted least connections. Weights proportional to backend capacity (e.g., 8-core machine gets weight 200, 4-core gets weight 100). Weights can be statically configured or dynamically adjusted based on resource utilization metrics (CPU, memory from Prometheus). The AI agent can automatically adjust weights based on observed latency.

---

## 14. References

- **IPVS documentation:** http://www.linuxvirtualserver.org/software/ipvs.html
- **Maglev: A Fast and Reliable Software Network Load Balancer (Google):** https://research.google/pubs/pub44824/
- **Katran (Facebook XDP LB):** https://github.com/facebookincubator/katran
- **Cilium eBPF-based Networking:** https://docs.cilium.io/en/stable/network/ebpf/
- **Envoy Load Balancing:** https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/load_balancing/
- **Consistent Hashing and Random Trees (Karger et al.):** https://dl.acm.org/doi/10.1145/258533.258660
- **BPF and XDP Reference Guide:** https://docs.cilium.io/en/latest/bpf/
- **HAProxy Architecture:** https://www.haproxy.org/download/2.9/doc/architecture.txt
- **Kubernetes kube-proxy IPVS mode:** https://kubernetes.io/blog/2018/07/09/ipvs-based-in-cluster-load-balancing-deep-dive/
- **Google SRE Book — Load Balancing at the Frontend:** https://sre.google/sre-book/load-balancing-frontend/
