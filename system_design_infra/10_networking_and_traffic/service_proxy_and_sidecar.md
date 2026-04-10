# System Design: Service Proxy and Sidecar

> **Relevance to role:** Service meshes and sidecar proxies are the backbone of modern microservice communication in Kubernetes. For a cloud infrastructure platform engineer, you must understand Envoy internals (listener, filter chain, cluster, endpoint), xDS APIs (LDS, RDS, CDS, EDS, SDS), traffic interception via iptables, mTLS with SPIFFE identities, circuit breaking, retry policies, and how all of this integrates with Kubernetes admission controllers. This directly applies to operating and troubleshooting service mesh infrastructure at scale.

---

## 1. Requirement Clarifications

### Functional Requirements

| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Transparent traffic interception | Intercept all inbound/outbound traffic from application pods without app changes |
| FR-2 | mTLS for service-to-service | Automatic mutual TLS with SPIFFE identity, transparent to applications |
| FR-3 | Load balancing | Per-request L7 load balancing across upstream endpoints |
| FR-4 | Circuit breaking | Outlier detection, automatic ejection of unhealthy upstreams |
| FR-5 | Retry policy | Configurable retries with per-try timeout, retry budgets |
| FR-6 | Timeout management | Configurable timeouts with propagation across service hops |
| FR-7 | Traffic shifting | Canary deployments, A/B testing via weighted routing |
| FR-8 | Observability | L7 metrics (RPS, latency, error rate), distributed tracing, access logs |
| FR-9 | Rate limiting (local) | Per-connection and per-request rate limiting within the sidecar |
| FR-10 | Protocol support | HTTP/1.1, HTTP/2, gRPC, TCP, WebSocket |

### Non-Functional Requirements

| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Sidecar latency overhead | < 1 ms p99 added per hop (inbound + outbound) |
| NFR-2 | Memory per sidecar | < 50 MB base, < 150 MB under load |
| NFR-3 | CPU per sidecar | < 0.1 CPU cores at 1,000 RPS |
| NFR-4 | mTLS handshake time | < 5 ms p99 (with session resumption) |
| NFR-5 | Certificate rotation | Zero-downtime, automatic, < 24 hours rotation period |
| NFR-6 | Configuration propagation | < 5 seconds from control plane change to data plane effect |
| NFR-7 | Mesh availability | Sidecar crash must not kill the application pod |

### Constraints & Assumptions

- Kubernetes 1.28+ with Istio-compatible service mesh or standalone Envoy.
- 2,000 microservices, each with 5-20 replicas → 10,000-40,000 sidecar proxies.
- Services are Java (Spring Boot), Python (FastAPI), and gRPC (Go, Java).
- Bare-metal nodes with Calico or Cilium CNI.
- Certificate authority: Istiod or SPIRE for SPIFFE identity issuance.

### Out of Scope

- Ingress/egress gateway (covered in API gateway design).
- Multi-cluster mesh federation.
- Service mesh data plane alternatives (Linkerd proxy-rs, gRPC proxyless mesh).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|------------|-------|
| Total sidecar proxies | 2,000 services x 10 avg replicas | 20,000 sidecars |
| Total mesh RPS (east-west) | 1,000,000 RPS (from LB design) | 1,000,000 RPS |
| Average RPS per sidecar | 1M / 20,000 | 50 RPS avg, 500 RPS peak |
| mTLS connections (concurrent) | 20,000 sidecars x 10 upstream connections avg | 200,000 mTLS connections |
| Certificate issuance rate | 20,000 sidecars / 24h rotation | ~14 certs/minute |
| xDS updates per second | Service deployments + scaling events | ~10 updates/second cluster-wide |
| Config size per sidecar | Routes + clusters + endpoints | ~500 KB avg |

### Latency Requirements

| Component | Target |
|-----------|--------|
| Sidecar outbound proxy (client side) | < 0.5 ms p50, < 1 ms p99 |
| Sidecar inbound proxy (server side) | < 0.3 ms p50, < 0.5 ms p99 |
| Total sidecar overhead (both hops) | < 1 ms p50, < 2 ms p99 |
| mTLS handshake (new connection) | < 3 ms p50, < 5 ms p99 |
| mTLS handshake (session resumption) | < 0.5 ms p50, < 1 ms p99 |
| xDS config push to data plane | < 2 seconds p50, < 5 seconds p99 |

### Storage Estimates

| Data | Calculation | Size |
|------|------------|------|
| Sidecar config per pod | Routes + clusters + endpoints + secrets | 500 KB |
| Total config across mesh | 20,000 x 500 KB | 10 GB |
| Sidecar memory (base) | Envoy baseline | 40 MB per sidecar |
| Sidecar memory (under load) | +100 connection buffers, TLS state | 100 MB per sidecar |
| Total mesh sidecar memory | 20,000 x 100 MB | 2 TB cluster-wide |
| Access logs (1 hour) | 1M RPS x 3600s x 400 bytes | ~1.4 TB/hour |
| Metrics (Prometheus) | 20,000 sidecars x 200 series x 8 bytes x 240 scrapes/hour | ~7.7 GB/hour |

### Bandwidth Estimates

| Data | Calculation | Value |
|------|------------|-------|
| xDS control plane to sidecars | 10 updates/s x 50 KB avg delta x 1000 affected sidecars | 500 MB/s burst |
| xDS steady state | Heartbeat + incremental | ~10 MB/s |
| Certificate distribution | 14 certs/min x 4 KB | negligible |
| Sidecar data plane (per sidecar) | 500 RPS peak x 7 KB (req+resp) | 3.5 MB/s per sidecar |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        CONTROL PLANE                             │
│                                                                   │
│  ┌───────────────┐  ┌──────────────┐  ┌───────────────────────┐ │
│  │    Istiod /    │  │  Kubernetes  │  │   Certificate         │ │
│  │  Control Plane │  │  API Server  │  │   Authority (CA)      │ │
│  │                │  │              │  │   (Istiod CA / SPIRE) │ │
│  │  - xDS Server  │  │  - Service   │  │                       │ │
│  │  - Config      │  │  - Endpoints │  │  - SPIFFE ID issuance │ │
│  │    translation │  │  - Pods      │  │  - Cert signing       │ │
│  │  - Policy      │  │  - ConfigMap │  │  - Rotation policy    │ │
│  │    evaluation  │  │              │  │                       │ │
│  └───────┬───────┘  └──────┬───────┘  └───────────┬───────────┘ │
│          │                 │                       │              │
│          │   Watches K8s   │                       │              │
│          │◄────────────────┘                       │              │
│          │                                         │              │
│          │    xDS gRPC streams                     │  SDS (cert)  │
│          │    (LDS, RDS, CDS, EDS)                 │              │
└──────────┼─────────────────────────────────────────┼──────────────┘
           │                                         │
           ▼                                         ▼
┌──────────────────────────────────────────────────────────────────┐
│                        DATA PLANE (per Pod)                       │
│                                                                    │
│  ┌────────────────────────────────────────────────────────────┐   │
│  │  Pod Network Namespace                                      │   │
│  │                                                              │   │
│  │  ┌──────────────┐    iptables     ┌────────────────────┐   │   │
│  │  │  Application │    REDIRECT     │   Envoy Sidecar    │   │   │
│  │  │  Container   │───────────────►│                    │   │   │
│  │  │              │                 │  Inbound (15006)   │   │   │
│  │  │  Listens on  │                 │    ├─ mTLS decrypt │   │   │
│  │  │  :8080       │◄────────────────│    ├─ AuthZ check  │   │   │
│  │  │              │   localhost     │    ├─ Metrics      │   │   │
│  │  │              │   :8080        │    └─ Forward      │   │   │
│  │  │              │                 │                    │   │   │
│  │  │  Calls out   │  iptables      │  Outbound (15001)  │   │   │
│  │  │  to svc-B    │──REDIRECT────►│    ├─ Route match   │   │   │
│  │  │  :80         │                 │    ├─ LB select    │   │   │
│  │  │              │                 │    ├─ mTLS encrypt │   │   │
│  │  │              │                 │    ├─ Retry/TO     │   │   │
│  │  │              │                 │    └─ Send to pod  │   │   │
│  │  └──────────────┘                 └────────────────────┘   │   │
│  │                                                              │   │
│  │  iptables rules (injected by istio-init):                    │   │
│  │    -A PREROUTING -p tcp -j ISTIO_INBOUND                     │   │
│  │    -A ISTIO_INBOUND -p tcp --dport 8080 -j REDIRECT          │   │
│  │      --to-port 15006                                         │   │
│  │    -A OUTPUT -p tcp -j ISTIO_OUTPUT                           │   │
│  │    -A ISTIO_OUTPUT -p tcp -j REDIRECT --to-port 15001        │   │
│  │    (uid/gid exceptions for envoy's own traffic)               │   │
│  └────────────────────────────────────────────────────────────┘   │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘

   Pod A (client)                          Pod B (server)
  ┌──────────┐                           ┌──────────┐
  │ App      │                           │ App      │
  │ → Envoy  │──── mTLS over network ───►│ Envoy ←  │
  │ outbound │                           │ inbound  │
  └──────────┘                           └──────────┘
   Port 15001                             Port 15006
```

### Component Roles

| Component | Role |
|-----------|------|
| Istiod / Control Plane | Translates Kubernetes resources (Service, VirtualService, DestinationRule) into Envoy xDS configuration. Serves as xDS server for all sidecar proxies. |
| Kubernetes API Server | Source of truth for services, endpoints, pods, and mesh configuration CRDs |
| Certificate Authority | Issues SPIFFE-compliant X.509 certificates (SVIDs) for each sidecar's workload identity |
| Envoy Sidecar (per pod) | Transparent L7 proxy handling mTLS, routing, load balancing, circuit breaking, retries, and observability |
| iptables rules | Redirect all pod traffic through the sidecar without application changes |
| istio-init container | Init container that sets up iptables rules before the application starts |

### Data Flows

**Service-to-service call (Pod A calls Pod B):**
1. Pod A's application sends HTTP request to `svc-B.namespace:80`.
2. iptables OUTPUT chain redirects the packet to Envoy on port 15001 (outbound).
3. Envoy resolves the original destination, matches to a route (VirtualService).
4. Envoy selects a Pod B endpoint via load balancing (EDS provides pod IPs).
5. Envoy initiates mTLS connection to Pod B's sidecar (SPIFFE identity verification).
6. Pod B's iptables PREROUTING chain redirects inbound to Envoy on port 15006.
7. Pod B's Envoy decrypts mTLS, verifies Pod A's SPIFFE identity against authorization policy.
8. Envoy forwards to Pod B's application on localhost:8080.
9. Response traverses the reverse path.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# Envoy configuration model (xDS resources)

# Listener Discovery Service (LDS)
# Defines what ports Envoy listens on and how to process connections
listener:
  name: "inbound|8080||"
  address:
    socket_address:
      address: 0.0.0.0
      port_value: 15006           # Inbound capture port
  filter_chains:
    - filter_chain_match:
        destination_port: 8080
        transport_protocol: "tls"  # Only match mTLS connections
      transport_socket:
        name: envoy.transport_sockets.tls
        typed_config:
          require_client_certificate: true
          common_tls_context:
            tls_certificate_sds_config:
              name: "default"     # SDS certificate name
            validation_context_sds_config:
              name: "ROOTCA"      # Trust bundle
      filters:
        - name: envoy.filters.network.http_connection_manager
          typed_config:
            route_config_name: "inbound|8080|http|svc-A.default"
            http_filters:
              - name: envoy.filters.http.rbac        # Authorization
              - name: envoy.filters.http.fault       # Fault injection
              - name: envoy.filters.http.router      # Routing

# Route Discovery Service (RDS)
route_configuration:
  name: "outbound|80|svc-B.default"
  virtual_hosts:
    - name: "svc-B.default.svc.cluster.local:80"
      domains: ["svc-B.default.svc.cluster.local"]
      routes:
        - match:
            prefix: "/"
          route:
            cluster: "outbound|80|v1|svc-B.default.svc.cluster.local"
            timeout: 30s
            retry_policy:
              retry_on: "5xx,reset,connect-failure"
              num_retries: 2
              per_try_timeout: 10s
              retry_back_off:
                base_interval: 0.1s
                max_interval: 1s

# Cluster Discovery Service (CDS)
cluster:
  name: "outbound|80|v1|svc-B.default.svc.cluster.local"
  type: EDS
  eds_cluster_config:
    service_name: "outbound|80|v1|svc-B.default.svc.cluster.local"
  connect_timeout: 5s
  lb_policy: LEAST_REQUEST
  circuit_breakers:
    thresholds:
      - max_connections: 1024
        max_pending_requests: 100
        max_requests: 1024
        max_retries: 3
  outlier_detection:
    consecutive_5xx: 5
    interval: 10s
    base_ejection_time: 30s
    max_ejection_percent: 50
    enforcing_consecutive_5xx: 100
  transport_socket:
    name: envoy.transport_sockets.tls
    typed_config:
      sni: "outbound_.80_.v1_.svc-B.default.svc.cluster.local"
      common_tls_context:
        tls_certificate_sds_config:
          name: "default"
        validation_context_sds_config:
          name: "ROOTCA"

# Endpoint Discovery Service (EDS)
cluster_load_assignment:
  cluster_name: "outbound|80|v1|svc-B.default.svc.cluster.local"
  endpoints:
    - locality:
        region: "us-east-1"
        zone: "us-east-1a"
      lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: 10.244.1.15
                port_value: 8080
          health_status: HEALTHY
          load_balancing_weight: 100
        - endpoint:
            address:
              socket_address:
                address: 10.244.2.23
                port_value: 8080
          health_status: HEALTHY
          load_balancing_weight: 100

# Secret Discovery Service (SDS)
secret:
  name: "default"
  tls_certificate:
    certificate_chain:
      inline_bytes: "<PEM encoded cert chain>"   # Workload SVID
    private_key:
      inline_bytes: "<PEM encoded private key>"
```

### Database Selection

| Store | Technology | Rationale |
|-------|-----------|-----------|
| Mesh configuration | Kubernetes CRDs (VirtualService, DestinationRule, AuthorizationPolicy) | Native K8s integration, kubectl management, GitOps-friendly |
| Service/Endpoint state | Kubernetes API (informer cache) | Real-time watch-based updates |
| xDS serving | In-memory (Istiod) | Low latency; rebuilt from K8s state on restart |
| Certificate store | In-memory (SDS) + Vault (CA root key) | Short-lived certs never persisted; root key in HSM |
| Metrics | Prometheus | Standard K8s metrics pipeline |
| Access logs | Elasticsearch via Fluentd/OTel | Structured search, correlation |

### Indexing Strategy

| Resource | Index/Lookup | Purpose |
|----------|-------------|---------|
| Services | namespace + name | Route resolution |
| Endpoints | service + port | Backend selection |
| Certificates | SPIFFE ID (spiffe://cluster/ns/default/sa/svc-A) | Identity-based cert lookup |
| AuthorizationPolicy | namespace + workload selector | Policy evaluation |
| VirtualService | host + gateway | Route matching |

---

## 5. API Design

### Management APIs

```yaml
# Kubernetes CRDs (declarative configuration)

# VirtualService: L7 traffic routing rules
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
metadata:
  name: svc-B-routing
  namespace: default
spec:
  hosts:
    - svc-B.default.svc.cluster.local
  http:
    - match:
        - headers:
            x-canary:
              exact: "true"
      route:
        - destination:
            host: svc-B.default.svc.cluster.local
            subset: v2
    - route:
        - destination:
            host: svc-B.default.svc.cluster.local
            subset: v1
          weight: 90
        - destination:
            host: svc-B.default.svc.cluster.local
            subset: v2
          weight: 10

# DestinationRule: LB policy, circuit breaking, connection pool
apiVersion: networking.istio.io/v1beta1
kind: DestinationRule
metadata:
  name: svc-B-destination
  namespace: default
spec:
  host: svc-B.default.svc.cluster.local
  trafficPolicy:
    connectionPool:
      tcp:
        maxConnections: 1024
        connectTimeout: 5s
      http:
        h2UpgradePolicy: DEFAULT
        maxRequestsPerConnection: 100
    loadBalancer:
      simple: LEAST_REQUEST
    outlierDetection:
      consecutive5xxErrors: 5
      interval: 10s
      baseEjectionTime: 30s
      maxEjectionPercent: 50
  subsets:
    - name: v1
      labels:
        version: v1
    - name: v2
      labels:
        version: v2

# AuthorizationPolicy: workload-level access control
apiVersion: security.istio.io/v1beta1
kind: AuthorizationPolicy
metadata:
  name: svc-B-authz
  namespace: default
spec:
  selector:
    matchLabels:
      app: svc-B
  action: ALLOW
  rules:
    - from:
        - source:
            principals: ["cluster.local/ns/default/sa/svc-A"]
      to:
        - operation:
            methods: ["GET", "POST"]
            paths: ["/api/*"]

# PeerAuthentication: mTLS mode
apiVersion: security.istio.io/v1beta1
kind: PeerAuthentication
metadata:
  name: default
  namespace: default
spec:
  mtls:
    mode: STRICT   # STRICT, PERMISSIVE, DISABLE
```

### Data Plane Behavior

```
xDS Protocol Flow:

  Sidecar                          Control Plane (Istiod)
     │                                    │
     │── DiscoveryRequest (LDS) ────────►│
     │                                    │ Compute listeners for this workload
     │◄── DiscoveryResponse (LDS) ───────│ (based on namespace, labels, services)
     │── ACK ───────────────────────────►│
     │                                    │
     │── DiscoveryRequest (RDS) ────────►│
     │◄── DiscoveryResponse (RDS) ───────│ Route configs for matched listeners
     │── ACK ───────────────────────────►│
     │                                    │
     │── DiscoveryRequest (CDS) ────────►│
     │◄── DiscoveryResponse (CDS) ───────│ Upstream clusters
     │── ACK ───────────────────────────►│
     │                                    │
     │── DiscoveryRequest (EDS) ────────►│
     │◄── DiscoveryResponse (EDS) ───────│ Endpoints (pod IPs)
     │── ACK ───────────────────────────►│
     │                                    │
     │── SDS Request (certificate) ─────►│
     │◄── SDS Response (SVID + key) ─────│ X.509 SVID from CA
     │                                    │
     │    ... streaming, push on change   │
     │                                    │
     │◄── Push (EDS update) ─────────────│ Pod scaled, endpoint changed
     │── ACK ───────────────────────────►│

  Delta xDS (incremental):
    Instead of sending full resource sets, only changed/removed resources.
    Reduces bandwidth from O(total_resources) to O(changed_resources).
    Critical at scale: 20,000 sidecars x 500 KB = 10 GB per full push.
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Traffic Interception (iptables REDIRECT)

**Why it's hard:**
The sidecar must intercept all application traffic transparently — the application should not know it exists. This requires modifying the pod's network namespace to redirect traffic without application code changes. The interception must handle: inbound to the app, outbound from the app, traffic from the sidecar itself (must not create loops), and traffic that should bypass the sidecar (health checks, Prometheus scrape).

**Approaches:**

| Approach | Transparency | Performance | Complexity | Maturity |
|----------|-------------|-------------|------------|----------|
| iptables REDIRECT | Full | Good (kernel DNAT) | Medium | Very mature (Istio default) |
| iptables TPROXY | Full | Better (no DNAT, preserves src IP) | High | Mature |
| eBPF (Cilium) | Full | Best (no iptables) | High | Maturing |
| Application-level (SDK) | None (requires code change) | Best (no interception) | Low | Varies |
| CNI plugin chaining | Full | Good | Medium | Maturing |

**Selected approach: iptables REDIRECT with eBPF migration path**

**Justification:** iptables REDIRECT is the proven, well-understood approach used by Istio. It works with any CNI. We plan to migrate to eBPF-based interception (Cilium) for better performance once the cluster-wide CNI migration is complete.

**Implementation detail:**

```bash
# iptables rules injected by istio-init container (or istio-cni plugin)
# These run inside the pod's network namespace

# === INBOUND TRAFFIC ===
# Create custom chains
iptables -t nat -N ISTIO_INBOUND
iptables -t nat -N ISTIO_IN_REDIRECT

# Redirect inbound to Envoy's inbound port (15006)
iptables -t nat -A ISTIO_IN_REDIRECT -p tcp -j REDIRECT --to-port 15006

# Hook into PREROUTING
iptables -t nat -A PREROUTING -p tcp -j ISTIO_INBOUND

# Skip interception for:
#   - Port 15008 (HBONE/tunnel)
#   - Port 15090 (Prometheus stats)
#   - Port 15021 (health check)
iptables -t nat -A ISTIO_INBOUND -p tcp --dport 15008 -j RETURN
iptables -t nat -A ISTIO_INBOUND -p tcp --dport 15090 -j RETURN
iptables -t nat -A ISTIO_INBOUND -p tcp --dport 15021 -j RETURN

# Redirect all other inbound TCP to Envoy
iptables -t nat -A ISTIO_INBOUND -p tcp -j ISTIO_IN_REDIRECT

# === OUTBOUND TRAFFIC ===
iptables -t nat -N ISTIO_OUTPUT
iptables -t nat -N ISTIO_REDIRECT

# Redirect outbound to Envoy's outbound port (15001)
iptables -t nat -A ISTIO_REDIRECT -p tcp -j REDIRECT --to-port 15001

# Hook into OUTPUT
iptables -t nat -A OUTPUT -p tcp -j ISTIO_OUTPUT

# CRITICAL: Skip traffic FROM Envoy itself (prevent loops)
# Envoy runs as UID 1337
iptables -t nat -A ISTIO_OUTPUT -m owner --uid-owner 1337 -j RETURN

# Skip localhost traffic (app → app within same pod)
iptables -t nat -A ISTIO_OUTPUT -o lo -d 127.0.0.1/32 -j RETURN

# Skip traffic to specific CIDRs (e.g., Kubernetes API server)
iptables -t nat -A ISTIO_OUTPUT -d 10.96.0.1/32 -j RETURN

# Redirect all other outbound TCP to Envoy
iptables -t nat -A ISTIO_OUTPUT -p tcp -j ISTIO_REDIRECT
```

```
Traffic flow with iptables:

  Outbound (App → external service):
  
  App calls connect(svc-B:80)
       │
       ▼
  Kernel: original dst = svc-B-clusterIP:80
       │
       ▼
  iptables OUTPUT → ISTIO_OUTPUT → ISTIO_REDIRECT
       │
       ▼
  REDIRECT --to-port 15001 (DNAT: dst becomes 127.0.0.1:15001)
       │
       ▼
  Envoy receives on port 15001
  Envoy reads SO_ORIGINAL_DST socket option → gets svc-B-clusterIP:80
  Envoy matches route for svc-B → selects backend pod IP
  Envoy connects to backend pod (as UID 1337 → iptables skips)
  
  
  Inbound (external → App):
  
  Packet arrives at pod IP:8080
       │
       ▼
  iptables PREROUTING → ISTIO_INBOUND → ISTIO_IN_REDIRECT
       │
       ▼
  REDIRECT --to-port 15006 (DNAT: dst becomes 127.0.0.1:15006)
       │
       ▼
  Envoy receives on port 15006
  Envoy reads SO_ORIGINAL_DST → gets pod-ip:8080
  Envoy applies inbound filters (mTLS decrypt, authZ, metrics)
  Envoy forwards to localhost:8080 (app's actual port)
```

**Failure modes:**
- **iptables rules not installed:** If istio-init fails, traffic bypasses the sidecar. Application works but without mTLS/observability. Detection: check for iptables rules in pod; Istio pilot-agent monitors this.
- **Envoy crash (port 15001/15006 not listening):** All traffic redirected to a closed port → connection refused. Application is down even though it is healthy. Mitigation: Kubernetes readiness probe on the sidecar; if sidecar is not ready, pod is removed from endpoints.
- **iptables rule conflict with CNI:** Some CNIs (Calico, Cilium) also manipulate iptables. Conflicting rules can cause routing loops or dropped traffic. Solution: use istio-cni plugin (installs rules via CNI chain) instead of istio-init container.
- **High connection rate overloads conntrack:** Each redirected connection creates a conntrack entry. At 10,000+ connections/second, conntrack table fills up. Solution: increase `nf_conntrack_max`; migrate to eBPF (Cilium eliminates iptables entirely).

**Interviewer Q&As:**

> **Q1: How does Envoy know the original destination after iptables REDIRECT?**
> A: The kernel stores the original destination in the connection's socket options. Envoy reads it via `getsockopt(SO_ORIGINAL_DST)`. This returns the original IP:port before DNAT was applied. Envoy uses this to match the request to the correct route and upstream cluster.

> **Q2: Why does Envoy run as UID 1337?**
> A: iptables rules exclude traffic from UID 1337 (`-m owner --uid-owner 1337 -j RETURN`). This prevents Envoy's own outbound connections (to upstream backends) from being redirected back to itself, which would create an infinite loop. UID 1337 is the conventional Envoy sidecar user ID in Istio.

> **Q3: How does eBPF-based interception differ from iptables?**
> A: eBPF (Cilium) hooks at the socket layer (`connect()` syscall) and rewrites the destination to the sidecar address before any packet is sent. This avoids DNAT, conntrack entries, and iptables chain traversal. Benefits: lower latency (~50% reduction), no conntrack overhead, no iptables rule conflicts. The eBPF program is attached to the pod's cgroup.

> **Q4: How do you handle traffic that should bypass the sidecar?**
> A: Three mechanisms: (1) Exclude specific ports via annotation (`traffic.sidecar.istio.io/excludeOutboundPorts: "3306,6379"`). (2) Exclude specific CIDRs (`traffic.sidecar.istio.io/excludeOutboundIPRanges: "10.0.0.0/8"`). (3) The istio-init container adds iptables RETURN rules for excluded ports/CIDRs before the REDIRECT rules.

> **Q5: What happens if the application starts before the sidecar is ready?**
> A: iptables rules are installed by the init container, so they are active before both the app and sidecar start. If the app starts first and makes requests, they are redirected to port 15001 where nothing is listening yet → connection refused. Solution: `holdApplicationUntilProxyStarts: true` in Istio config, which delays app container start until Envoy is ready (Kubernetes 1.28+ sidecar containers feature).

> **Q6: How does TPROXY differ from REDIRECT and when would you use it?**
> A: REDIRECT uses DNAT (changes the destination address to 127.0.0.1:15001). TPROXY intercepts without DNAT; the socket sees the original destination address without needing `SO_ORIGINAL_DST`. TPROXY preserves the source IP on the server side (REDIRECT shows 127.0.0.1 as source). Use TPROXY when you need accurate source IP logging on the server side, or when `SO_ORIGINAL_DST` is not available (e.g., UDP).

---

### Deep Dive 2: mTLS and SPIFFE Identity

**Why it's hard:**
Every pod-to-pod connection must be encrypted and authenticated. With 20,000 sidecars and 200,000 concurrent connections, the certificate authority must issue certificates rapidly, rotate them frequently (for security), and handle revocation. The SPIFFE identity framework provides a standard way to identify workloads, but integrating it with Kubernetes service accounts, automating rotation, and handling certificate propagation across a large cluster is complex.

**Approaches:**

| Approach | Identity Granularity | Rotation | Complexity | Standards |
|----------|---------------------|----------|------------|-----------|
| Shared cluster certificate | Cluster-level (no identity) | Rare | Low | No |
| Per-namespace certificate | Namespace-level | Weekly | Low | No |
| Per-service-account (SPIFFE/Istio) | Service account level | Every 24h (configurable) | Medium | SPIFFE |
| Per-pod certificate (SPIRE) | Pod-level (unique per instance) | Every 1h | High | SPIFFE |
| Application-managed (Vault PKI) | App-level | Varies | High | Varies |

**Selected approach: Per-service-account SPIFFE identity via Istiod CA (with SPIRE upgrade path)**

**Justification:** Service-account-level identity balances security (workload identity) with scalability (20,000 sidecars, but only ~2,000 unique identities). SPIFFE standard ensures interoperability. Istiod built-in CA is simpler than standalone SPIRE for most use cases.

**Implementation detail:**

```
SPIFFE Identity Format:
  spiffe://<trust-domain>/ns/<namespace>/sa/<service-account>
  Example: spiffe://cluster.local/ns/default/sa/svc-A

Certificate Issuance Flow:

  1. Pod starts → Envoy sidecar starts
  2. Envoy sends CSR (Certificate Signing Request) to Istiod via SDS
  
     Envoy → pilot-agent (SDS proxy in sidecar)
             → Istiod SDS server (gRPC)
  
  3. Istiod validates the request:
     a. Verify the caller's Kubernetes service account token (JWT)
        - Token is projected into the pod at
          /var/run/secrets/tokens/istio-token
        - Istiod validates with Kubernetes TokenReview API
     b. Extract namespace and service account from the token
     c. Construct SPIFFE ID:
        spiffe://cluster.local/ns/{namespace}/sa/{service-account}
  
  4. Istiod signs the certificate:
     - Subject: SPIFFE ID in SAN (Subject Alternative Name) URI
     - Validity: 24 hours (default, configurable)
     - Key: ECDSA P-256 (generated by Envoy, private key never leaves pod)
     - Issuer: Istiod's intermediate CA
     - Chain: workload cert → Istiod intermediate → Root CA
  
  5. Istiod returns signed certificate + CA trust bundle via SDS
  
  6. Envoy loads the certificate for all mTLS connections
  
  7. Rotation: pilot-agent requests new cert before expiry (at 80% of TTL)
     - New cert is hot-swapped into Envoy via SDS push
     - No connection disruption (existing connections use old cert until close)


mTLS Handshake:

  Client Envoy                          Server Envoy
       │                                      │
       │── ClientHello ─────────────────────►│
       │                                      │
       │◄── ServerHello ─────────────────────│
       │    + Server Certificate               │
       │      (SPIFFE ID: spiffe://...svc-B)  │
       │    + CertificateRequest              │
       │                                      │
       │── Client Certificate ──────────────►│
       │    (SPIFFE ID: spiffe://...svc-A)    │
       │── CertificateVerify ──────────────►│
       │── Finished ──────────────────────►│
       │                                      │
       │◄── Finished ────────────────────────│
       │                                      │
       │    Both sides verified:              │
       │    1. Certificate chain → Root CA    │
       │    2. Certificate not expired        │
       │    3. SPIFFE ID matches expected     │
       │       identity (AuthorizationPolicy)  │
       │                                      │
       │◄═══ Encrypted data channel ════════►│
```

```
Trust Bundle Distribution:

  Root CA (offline, HSM)
       │
       ▼
  Istiod Intermediate CA
       │
       ├── Signs workload certs
       │
       ▼
  Trust Bundle = [Root CA cert, Intermediate CA cert]
  Distributed to all sidecars via SDS (name: "ROOTCA")
  
  For cross-cluster mTLS:
    Both clusters' Root CAs must be in each other's trust bundles.
    Or: use a shared Root CA (simpler) with per-cluster intermediates.
```

**Failure modes:**
- **Istiod CA down:** Existing certificates continue working until expiry. New certificate requests fail. If outage > 80% of cert TTL (19.2 hours for 24h certs), certs start expiring and mTLS fails. Mitigation: HA Istiod (3+ replicas), cert TTL of 24h+ gives long buffer.
- **Clock skew:** If pod's clock is ahead of Istiod, the issued cert appears "not yet valid." If behind, it appears expired. Solution: NTP synchronization mandatory on all nodes; cert NotBefore has 5-minute grace window.
- **Private key compromise:** Key is in Envoy's memory; if pod is compromised, attacker has the key. Short cert TTL (24h) limits exposure. No online revocation (CRL/OCSP) is used — Istio relies on short-lived certs as the revocation mechanism.
- **Trust bundle propagation delay:** When root CA is rotated, new trust bundle must reach all sidecars before old certs expire. Solution: overlap period where both old and new CAs are in the trust bundle.

**Interviewer Q&As:**

> **Q1: Why use SPIFFE identities instead of Kubernetes service names?**
> A: SPIFFE provides a cryptographically verifiable identity (X.509 certificate with SPIFFE ID in SAN). Service names are DNS-based and can be spoofed if DNS is compromised. SPIFFE identities work across clusters and even across non-Kubernetes platforms. They are standardized (RFC-like specification), enabling interoperability between mesh implementations.

> **Q2: How does certificate rotation work without dropping connections?**
> A: The pilot-agent requests a new certificate at 80% of TTL (after 19.2 hours for a 24h cert). The new cert is pushed to Envoy via SDS (Secret Discovery Service). Envoy hot-swaps the certificate: new connections use the new cert; existing connections continue using the old cert until they close naturally. There is no reconnection or connection drop during rotation.

> **Q3: How do you handle mTLS with non-mesh services (e.g., external databases)?**
> A: Two patterns: (1) PERMISSIVE mTLS mode: the sidecar accepts both plaintext and mTLS on the same port. Non-mesh clients connect without TLS. (2) Egress gateway: mesh services call the egress gateway, which terminates mTLS and opens a plaintext (or different TLS) connection to the external service. (3) ServiceEntry + DestinationRule: configure mTLS or simple TLS for external hosts.

> **Q4: What is the performance impact of mTLS on every connection?**
> A: TLS 1.3 handshake adds ~3-5ms for new connections (ECDHE key exchange + certificate verification). With session resumption (TLS session tickets), subsequent connections add < 1ms. At steady state, most connections are long-lived (HTTP/2 multiplexing), so the amortized cost is negligible. CPU cost: ~0.1 CPU core per 10,000 new TLS connections/second (ECDSA P-256).

> **Q5: How do you rotate the root CA certificate?**
> A: Root CA rotation is a multi-step process: (1) Generate new root CA. (2) Add new root to trust bundle (both old and new are trusted). (3) Push updated trust bundle to all sidecars via SDS. Wait for propagation. (4) Switch Istiod to sign with new intermediate (chained to new root). (5) Wait for all old workload certs to expire (24h). (6) Remove old root from trust bundle. Total process: ~48 hours minimum for a 24h cert TTL.

> **Q6: How do you handle mTLS between services in different namespaces?**
> A: SPIFFE identity includes namespace: `spiffe://cluster.local/ns/payments/sa/payment-svc`. AuthorizationPolicy in the target namespace allows or denies based on the source's SPIFFE ID. mTLS is established regardless of namespace — the trust bundle is cluster-wide. Access control is policy-based, not network-based.

---

### Deep Dive 3: Circuit Breaking and Outlier Detection

**Why it's hard:**
A single slow or failing upstream can cascade failure across the entire service mesh. Circuit breaking must detect degradation quickly, eject bad endpoints, and recover automatically when the upstream heals. The challenge is calibrating thresholds: too aggressive causes false positives (ejecting healthy backends); too lenient allows cascading failures. Different failure modes (crash, latency, partial failure) require different detection strategies.

**Approaches:**

| Approach | Detection Speed | Accuracy | Recovery | Complexity |
|----------|----------------|----------|----------|------------|
| Connection pool limits only | Instantaneous (on pool full) | Low (not failure-aware) | Manual | Low |
| Consecutive error threshold | Fast (5 errors → eject) | Medium | Auto (timer) | Medium |
| Success rate anomaly detection | Moderate (needs baseline) | High | Auto | High |
| Latency percentile outlier | Moderate | High | Auto | High |
| Combined (Envoy outlier detection) | Fast + accurate | High | Auto (exponential) | Medium |

**Selected approach: Envoy's combined outlier detection with tuned thresholds**

**Implementation detail:**

```yaml
# Envoy Outlier Detection Configuration
outlier_detection:
  # Consecutive 5xx errors (crash/error detection)
  consecutive_5xx: 5                    # 5 consecutive 5xx → eject
  enforcing_consecutive_5xx: 100        # 100% enforcement

  # Consecutive gateway errors (502, 503, 504)
  consecutive_gateway_failure: 5
  enforcing_consecutive_gateway_failure: 100

  # Success rate anomaly detection
  # If a backend's success rate is > 1 stdev below the mean:
  success_rate_minimum_hosts: 5         # Need at least 5 backends to compute mean
  success_rate_request_volume: 100      # Need 100+ requests in interval to evaluate
  success_rate_stdev_factor: 1900       # 1.9 standard deviations (19/10)
  enforcing_success_rate: 100

  # Failure percentage (alternative to success rate)
  failure_percentage_threshold: 50      # > 50% failure rate → eject
  failure_percentage_minimum_hosts: 5
  failure_percentage_request_volume: 50
  enforcing_failure_percentage: 100

  # Ejection parameters
  interval: 10s                         # Evaluate every 10 seconds
  base_ejection_time: 30s               # First ejection: 30s
  max_ejection_percent: 50              # Never eject more than 50% of backends
  max_ejection_time: 300s               # Cap at 5 minutes

  # Ejection time = base_ejection_time * num_consecutive_ejections
  # 1st: 30s, 2nd: 60s, 3rd: 90s, ..., max: 300s
```

```
Circuit Breaker State Machine:

  ┌─────────┐      consecutive errors >= threshold      ┌──────────┐
  │ CLOSED  │ ──────────────────────────────────────────► │  OPEN    │
  │(healthy)│                                             │(rejecting│
  │         │ ◄────────────── probe succeeds ──────────── │ requests)│
  └─────────┘                                             └────┬─────┘
       ▲                                                       │
       │                                                       │
       │              ┌────────────┐                           │
       │              │ HALF-OPEN  │ ◄── ejection_time expires │
       │              │ (probing)  │                           │
       │              └─────┬──────┘                           │
       │                    │                                  │
       └── probe succeeds ──┘                                  │
                            │                                  │
                            └── probe fails ──────────────────►┘

  Connection-level Circuit Breakers (resource protection):
  ┌────────────────────────────────────────────────────────┐
  │  max_connections: 1024                                  │
  │    If > 1024 TCP connections to this cluster → reject   │
  │    new connections with TCP RST                         │
  │                                                         │
  │  max_pending_requests: 100                              │
  │    If > 100 requests queued waiting for connection      │
  │    → reject with 503 (HTTP) or UNAVAILABLE (gRPC)      │
  │                                                         │
  │  max_requests: 1024                                     │
  │    If > 1024 active requests to this cluster → reject   │
  │    (relevant for HTTP/2 where requests > connections)   │
  │                                                         │
  │  max_retries: 3                                         │
  │    If > 3 retries in flight to this cluster → no more   │
  │    retries (prevents retry storm)                       │
  └────────────────────────────────────────────────────────┘
```

```
Outlier Detection Flow:

  Every {interval} seconds:
    For each backend in the cluster:
      │
      ├── Check consecutive 5xx counter
      │   If consecutive_5xx >= 5:
      │     EJECT this backend
      │
      ├── Compute success rate over the interval
      │   If num_backends >= success_rate_minimum_hosts:
      │     mean_sr = average success rate of all backends
      │     stdev_sr = standard deviation
      │     threshold = mean_sr - (stdev_factor/1000) * stdev_sr
      │     If this backend's success_rate < threshold:
      │       EJECT this backend
      │
      └── Check failure percentage
          If failure_rate > failure_percentage_threshold:
            EJECT this backend

  EJECT action:
    Mark backend as "ejected" in the LB pool
    Set re-admit timer = base_ejection_time * num_ejections
    If total ejected > max_ejection_percent * pool_size:
      Do NOT eject (prevent total drain)
    Emit metric: envoy_cluster_outlier_detection_ejections_active
```

**Failure modes:**
- **Entire upstream down (all backends ejected):** The `max_ejection_percent` cap (50%) prevents total ejection. If all backends are truly down, circuit breaker returns 503 immediately (fail-fast) rather than waiting for timeout. This is the desired behavior: fast failure enables clients to retry with backoff.
- **False ejection (transient error spike):** A brief deployment glitch causes 5 consecutive 5xx from one backend. It is ejected for 30 seconds. If the deployment completes in 10 seconds, the backend is ejected unnecessarily for 20 seconds. Trade-off: some unnecessary ejection vs. risk of routing to degraded backend. Tunable via `consecutive_5xx` threshold and `base_ejection_time`.
- **Cascading ejection:** Backend A is ejected; its traffic shifts to B and C. If B cannot handle the extra load, it starts failing too and gets ejected. Now C gets all traffic. Mitigation: (1) Retry budgets (max 20% of requests can be retries). (2) Load-based circuit breaking (max_connections). (3) Admission control at the application layer.

**Interviewer Q&As:**

> **Q1: What is the difference between circuit breaking and outlier detection in Envoy?**
> A: Circuit breaking is connection/request-level resource protection: limits on max connections, pending requests, active requests, and retries. It does not track backend health. Outlier detection is endpoint-level health tracking: it monitors error rates and ejects unhealthy backends from the load balancer pool. They work together: circuit breakers protect the caller from overload; outlier detection protects callers from bad backends.

> **Q2: How do you prevent retry storms in a service mesh?**
> A: Multiple layers: (1) `max_retries` circuit breaker (max 3 concurrent retries per cluster). (2) Retry budget: total retries across all requests should not exceed 20% of the total request rate. (3) Idempotency awareness: only retry safe methods (GET) or requests marked idempotent. (4) Exponential backoff on retries. (5) Hedged requests (send to 2 backends simultaneously, use first response) instead of sequential retries for latency-sensitive paths.

> **Q3: How does timeout propagation work across multiple service hops?**
> A: Use a "deadline" pattern: the original caller sets a deadline (e.g., "this request must complete by timestamp T"). Each hop subtracts its processing time and propagates the remaining deadline via the `grpc-timeout` header (gRPC) or a custom `X-Request-Deadline` header (HTTP). If a sidecar receives a request with a deadline that has already passed, it immediately returns 504 without forwarding. This prevents wasted work in deep call chains.

> **Q4: How do you handle partial failures (backend responds but with garbage data)?**
> A: This is the hardest failure mode. The sidecar cannot inspect application-level data correctness. Options: (1) Application returns a structured error code that the sidecar can detect (e.g., gRPC status codes). (2) Response validation plugin that checks for expected content-type, body structure, or specific headers. (3) End-to-end health checks that validate business logic (separate from infra health checks).

> **Q5: What metrics should you monitor to detect circuit breaker issues?**
> A: Key metrics: `envoy_cluster_outlier_detection_ejections_active` (currently ejected backends), `envoy_cluster_outlier_detection_ejections_total` (cumulative ejections), `envoy_cluster_upstream_cx_overflow` (connections rejected by circuit breaker), `envoy_cluster_upstream_rq_pending_overflow` (requests rejected by pending queue limit), `envoy_cluster_upstream_rq_retry_overflow` (retries rejected by retry budget). Alert on: ejections_active > 30% of pool for > 5 minutes.

> **Q6: How do you test circuit breaker configuration before production?**
> A: (1) Chaos engineering: inject faults using Istio's fault injection (VirtualService fault block) — add delay or abort to specific percentages of requests. Verify that circuit breakers trip correctly. (2) Load testing: use tools like Fortio or Locust to generate traffic and verify circuit breaker thresholds. (3) Shadow testing: apply new DestinationRule in shadow mode (log ejections without actually ejecting).

---

### Deep Dive 4: Envoy Threading Model and Performance

**Why it's hard:**
Envoy must handle 10,000+ concurrent connections per sidecar with < 1ms latency overhead. The threading model directly determines performance characteristics, connection handling, and failure isolation. Understanding it is essential for capacity planning and debugging.

**Approaches:**

| Model | Throughput | Tail Latency | Complexity | Example |
|-------|-----------|-------------|------------|---------|
| Single-threaded event loop | Limited by 1 core | Low | Low | Nginx (single worker) |
| Multi-process (fork) | Good | Low | Medium | Nginx (multiple workers, no shared state) |
| Thread-per-connection | Poor (thread overhead) | High (context switching) | Low | Apache (prefork) |
| Multi-threaded with connection ownership | Excellent | Low | High | Envoy |
| Shared-nothing worker pool | Excellent | Lowest | Highest | Seastar/ScyllaDB |

**Selected approach: Envoy's multi-threaded model (main thread + N worker threads)**

**Implementation detail:**

```
Envoy Threading Architecture:

  ┌─────────────────────────────────────────────────────────┐
  │                    ENVOY PROCESS                         │
  │                                                           │
  │  ┌─────────────────┐                                     │
  │  │   Main Thread   │                                     │
  │  │                  │                                     │
  │  │  - xDS handling  │ ← Receives config from control plane│
  │  │  - Stats flushing│ ← Aggregates stats from workers    │
  │  │  - Admin API     │ ← /config_dump, /clusters, /stats  │
  │  │  - Hot restart   │ ← Drains old instance, starts new  │
  │  │  - Signal handle │                                     │
  │  │  - Access log    │ ← Centralized log writing           │
  │  │    flushing      │                                     │
  │  └────────┬────────┘                                     │
  │           │ Config update (TLS post to worker)            │
  │           ▼                                               │
  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐   │
  │  │ Worker 0 │ │ Worker 1 │ │ Worker 2 │ │ Worker 3 │   │
  │  │          │ │          │ │          │ │          │   │
  │  │ Event    │ │ Event    │ │ Event    │ │ Event    │   │
  │  │ Loop     │ │ Loop     │ │ Loop     │ │ Loop     │   │
  │  │(libevent)│ │(libevent)│ │(libevent)│ │(libevent)│   │
  │  │          │ │          │ │          │ │          │   │
  │  │ Owns:    │ │ Owns:    │ │ Owns:    │ │ Owns:    │   │
  │  │ - Conns  │ │ - Conns  │ │ - Conns  │ │ - Conns  │   │
  │  │ - TLS    │ │ - TLS    │ │ - TLS    │ │ - TLS    │   │
  │  │   state  │ │   state  │ │   state  │ │   state  │   │
  │  │ - Conn   │ │ - Conn   │ │ - Conn   │ │ - Conn   │   │
  │  │   pools  │ │   pools  │ │   pools  │ │   pools  │   │
  │  │ - Timers │ │ - Timers │ │ - Timers │ │ - Timers │   │
  │  └──────────┘ └──────────┘ └──────────┘ └──────────┘   │
  │       ▲             ▲            ▲            ▲          │
  │       │             │            │            │          │
  │       └─────────────┼────────────┼────────────┘          │
  │                     │            │                        │
  │          Listener accepts connection                      │
  │          Kernel assigns to worker (SO_REUSEPORT)         │
  │          Connection stays on that worker forever          │
  └─────────────────────────────────────────────────────────┘

Key Design Principles:

  1. Connection ownership: A connection is assigned to a worker thread
     when accepted and NEVER moves. All I/O for that connection happens
     on the owning thread. This eliminates synchronization overhead.

  2. SO_REUSEPORT: The listener socket is shared across workers using
     the SO_REUSEPORT kernel option. The kernel distributes new
     connections across workers (approximately equal distribution).

  3. Thread-local storage (TLS): Each worker has its own:
     - Connection pools to upstream services
     - TLS session caches
     - Stats counters (aggregated by main thread)
     - Timer wheels
     This means: no locks on the hot path.

  4. Config updates: Main thread receives xDS update, creates a new
     config snapshot, and posts it to each worker thread via a
     thread-safe queue. Workers atomically swap to the new config.
     In-flight requests complete on the old config.

  5. Upstream connection pools: Each worker maintains its own pool
     of connections to each upstream cluster. With 4 workers and
     100 upstreams, there are 400 connection pools. This avoids
     cross-thread connection sharing but uses more connections.

Performance characteristics (sidecar, 2 workers):
  - Idle memory: ~40 MB
  - Per connection: ~50 KB (TLS state + buffers)
  - 1000 concurrent connections: ~90 MB
  - Throughput: ~50,000 RPS per worker (simple proxy)
  - Latency overhead: ~0.3-0.5 ms p50, ~0.8-1.5 ms p99
```

**Failure modes:**
- **One worker thread blocked:** If a filter has a bug causing a blocking operation, only connections on that worker are affected. Other workers continue serving. Detection: per-worker latency metrics diverge.
- **Memory leak in Envoy:** Sidecar memory grows unbounded. Kubernetes OOMKills the Envoy container, disrupting the application. Mitigation: set resource limits; Envoy has overload manager that sheds load before OOM.
- **Hot restart failure:** During hot restart (binary upgrade), old and new Envoy instances run simultaneously. If the new instance crashes during startup, connections are lost. Mitigation: precheck new config before hot restart.

**Interviewer Q&As:**

> **Q1: Why does Envoy use per-worker connection pools instead of shared pools?**
> A: Shared pools require locks for every connection checkout/checkin, which kills performance at high RPS. Per-worker pools are lock-free (thread-local). The trade-off is more total connections to upstreams (N workers x connections), but this is acceptable for the latency benefit. In a sidecar (2 workers), the connection overhead is minimal.

> **Q2: How does Envoy handle config updates without dropping requests?**
> A: The main thread creates a new immutable config snapshot. It posts a reference-counted pointer to each worker's event queue. Workers pick up the new config on their next event loop iteration. The old config is reference-counted and freed only when all workers have moved to the new config and all in-flight requests using it have completed. This is fully lock-free on the hot path.

> **Q3: How many worker threads should a sidecar Envoy have?**
> A: Typically 2 for sidecars. One thread handles the majority of traffic; two provides headroom. More than 2 wastes CPU (sidecar should use < 0.1 cores). For standalone Envoy (L7 LB), use N = number of CPU cores. Configured via `--concurrency` flag.

> **Q4: How does Envoy's hot restart work?**
> A: The old Envoy process forks a new process. The new process inherits the listening sockets via Unix domain socket passing. The old process enters drain mode (stops accepting new connections). Existing connections on the old process are drained (configurable drain period, default 600 seconds). Once all connections are drained or the drain period expires, the old process exits. During the overlap, both processes serve traffic.

> **Q5: How does SO_REUSEPORT distribute connections across workers?**
> A: With `SO_REUSEPORT`, each worker thread creates its own socket bound to the same address:port. The kernel distributes incoming connections across these sockets using a hash (typically 4-tuple hash). This gives roughly equal distribution. Without it, only one thread can `accept()` at a time (thundering herd problem). Since kernel 4.5, eBPF programs can customize the SO_REUSEPORT selection logic.

> **Q6: What is Envoy's overload manager?**
> A: The overload manager monitors resource usage (heap size, active connections, file descriptors). When thresholds are exceeded, it triggers overload actions: reduce HTTP/2 concurrent streams, stop accepting new connections, disable keepalives, or reset least-recently-used connections. This is a graceful degradation mechanism that prevents OOMKill.

---

## 7. Scaling Strategy

**Sidecar resource scaling:**
- Base sidecar resources: 100m CPU request, 128 MiB memory request.
- Sidecar resource limits scale with application traffic (auto-tuned based on observed usage).
- For high-traffic services (> 10K RPS per pod): increase to 500m CPU, 512 MiB memory.

**Control plane scaling:**
- Istiod horizontally scaled: 3-5 replicas for HA and throughput.
- xDS serving is sharded: each Istiod replica handles a subset of sidecars.
- At 20,000 sidecars, each Istiod handles ~4,000-7,000 xDS streams.
- Configuration computation is the bottleneck (translating K8s resources to Envoy config for each workload).

**Config push optimization:**
- Delta xDS (incremental): only push changed resources. Reduces bandwidth from O(all_routes * all_sidecars) to O(changed_routes * affected_sidecars).
- Sidecar resource scoping: `Sidecar` CRD limits which services each workload can see. A pod in namespace A does not need routes for services in namespace B unless explicitly configured. Reduces per-sidecar config from 500 KB to 50 KB.
- Debouncing: batch rapid changes (e.g., during rolling deployment) into a single push (100ms debounce window).

**Observability scaling:**
- Prometheus federation: per-node Prometheus scrapes sidecars; federated Prometheus aggregates.
- Access log sampling: log 1% of requests at steady state; 100% during incidents.
- Tracing sampling: 0.1% of requests in production; adjustable via control plane.

**Interviewer Q&As:**

> **Q1: How do you handle the "N-squared" problem in large meshes?**
> A: In a naive mesh, each sidecar needs config for every service (N services → N clusters, N * endpoints). With 2,000 services and 10 replicas each, that is 2,000 CDS entries and 20,000 EDS entries per sidecar. Solution: Sidecar resource scoping. The `Sidecar` CRD limits egress to only the services a workload actually calls (typically 5-20, not 2,000). This reduces config size by 100x.

> **Q2: What happens to the mesh when Istiod is down?**
> A: Sidecars continue operating on their last-known configuration. New pods cannot get initial config (sidecar starts but cannot connect to xDS). Existing pods cannot get config updates. Certificate rotation eventually fails (but certs are valid for 24 hours). Istiod should have 3+ replicas with pod anti-affinity across nodes for HA.

> **Q3: How do you scale certificate issuance?**
> A: Each Istiod replica can sign certificates independently (they all have access to the CA key). At 20,000 sidecars with 24h cert rotation, that is ~14 certs/minute — trivially handled. For SPIRE (per-pod identity), the load is higher but still manageable. The bottleneck is Kubernetes TokenReview API calls for validating service account tokens — cache validated tokens for 10 minutes.

> **Q4: How do you reduce sidecar memory overhead across the cluster?**
> A: (1) Sidecar scoping reduces config (and memory) per sidecar. (2) Proxyless gRPC mesh: for gRPC services, use gRPC's built-in xDS support (no sidecar needed). (3) Ambient mesh (Istio): replace sidecars with per-node ztunnel proxies for L4 (mTLS) and optional waypoint proxies for L7. Reduces from 20,000 sidecars to ~200 node-level proxies.

> **Q5: How do you monitor sidecar proxy health across 20,000 instances?**
> A: Prometheus scrapes each sidecar's `:15090/stats/prometheus` endpoint. Key metrics: `envoy_server_live` (is Envoy running), `envoy_server_concurrency` (worker threads), `envoy_server_memory_allocated` (memory usage), `envoy_server_uptime` (seconds since start). Aggregate dashboards show fleet-wide health. Alert on: sidecar restart rate > 1/hour, memory > 80% of limit, latency p99 > 5ms.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Sidecar Envoy crash | Kubernetes container restart (CrashLoopBackOff) | Pod traffic fails (iptables redirect to dead port) | Automatic restart; readiness probe removes pod from endpoints | 5-30s (restart) |
| Sidecar OOM killed | Kubernetes OOMKill event | Same as crash | Set appropriate memory limits; Envoy overload manager | 5-30s (restart) |
| Istiod down (all replicas) | Kubernetes deployment health, xDS stream disconnect | No config updates, no new certs; existing sidecars operate on stale config | 3+ replicas, pod anti-affinity, PDB | Stale config until Istiod recovers |
| xDS stream disconnect (one sidecar) | Envoy reconnect timer (1s, exponential backoff) | Sidecar config frozen; stale endpoints may cause errors | Envoy auto-reconnects; uses last-known-good config | < 5s typically |
| Certificate expiry (Istiod CA down > 24h) | Certificate expiry monitoring, mTLS handshake failures | mTLS connections fail; plaintext fallback if PERMISSIVE | HA Istiod; extend cert TTL; emergency manual cert issuance | Depends on detection |
| iptables rules corrupted | Connectivity check (envoy → app, app → external) | Traffic bypass or blackhole | istio-cni reconciliation loop; pod restart | < 60s (reconcile) |
| Bad config push (invalid route) | Envoy NACK (rejects bad config) | Sidecar stays on old config; new config not applied | xDS NACK mechanism; control plane validation before push | Immediate (old config preserved) |
| Network partition (pod ↔ Istiod) | xDS heartbeat timeout | Config and cert updates stall; sidecar operates on stale state | Sidecar operates independently; long cert TTL | Stale until healed |
| Upstream service degradation | Outlier detection (5xx rate, latency) | Requests to degraded service fail or slow | Circuit breaker ejects backend; retry to healthy backends | < 10s (ejection interval) |
| MutatingAdmissionWebhook down | Pod creation hangs (webhook timeout) | New pods cannot start (sidecar not injected) | `failurePolicy: Ignore` (pod starts without sidecar) or HA webhook | < 30s (webhook timeout) |
| Certificate authority key compromise | Security audit, anomalous cert issuance | All mesh identities potentially compromised | Revoke CA, rotate root, reissue all workload certs | Hours (full rotation) |

---

## 9. Security

**mTLS and Identity:**
- All inter-service traffic encrypted with mTLS (TLS 1.3 preferred).
- SPIFFE identity per service account: `spiffe://cluster.local/ns/<ns>/sa/<sa>`.
- Workload certificates are short-lived (24h default) — no CRL/OCSP needed.
- Private keys generated in-process (Envoy); never leave the pod.

**Authorization:**
- AuthorizationPolicy CRDs: allow/deny rules based on source identity, destination operation (method, path), and request attributes (headers, JWT claims).
- Default deny: when any AuthorizationPolicy exists for a workload, implicit deny for non-matching requests.
- Audit mode: `AUDIT` action logs matching requests without blocking (dry-run policy changes).

**Defense in Depth:**
- Network policies (Calico/Cilium) enforce L3/L4 segmentation independent of the mesh.
- Sidecar injection is mandatory for sensitive namespaces (enforced via OPA/Gatekeeper admission policy).
- Envoy binary is distroless (no shell, minimal attack surface).
- Admin API (port 15000) is bound to localhost only; not accessible from outside the pod.

**Secret Protection:**
- CA root key stored in Vault (HSM-backed) or Kubernetes Secret with encryption at rest.
- Workload private keys are in-memory only (not written to disk).
- SDS transport is authenticated (Kubernetes service account token).

**Audit Trail:**
- All AuthorizationPolicy decisions logged (source identity, destination, action, result).
- Certificate issuance events logged (SPIFFE ID, timestamp, requestor).
- Config changes tracked via Kubernetes audit log.

---

## 10. Incremental Rollout

**Phase 1 (Weeks 1-3): Control Plane Installation**
- Deploy Istiod in a test cluster with `PERMISSIVE` mTLS mode.
- Inject sidecars into 2-3 non-critical services.
- Validate: connectivity works, metrics are emitted, traces propagate.
- No traffic policy changes; sidecar is transparent.

**Phase 2 (Weeks 4-6): Observability Wins**
- Expand sidecar injection to all services in the test cluster.
- Enable access logging and tracing.
- Build Grafana dashboards for mesh-wide latency, error rates, traffic topology.
- Demonstrate value: "we can now see all inter-service traffic."

**Phase 3 (Weeks 7-10): mTLS Strict Mode**
- Switch namespaces from PERMISSIVE to STRICT mTLS one at a time.
- Validate no plaintext connections remain (check Envoy stats: `ssl.handshake` vs. total connections).
- Handle edge cases: non-mesh services that call mesh services.

**Phase 4 (Weeks 11-14): Traffic Management**
- Enable canary deployments for 1-2 services using VirtualService weight splitting.
- Configure circuit breakers and retry policies for critical paths.
- Run chaos engineering tests (fault injection via VirtualService).

**Phase 5 (Weeks 15-18): Authorization Policies**
- Deploy AuthorizationPolicy in AUDIT mode (log-only) for all services.
- Review audit logs for unexpected traffic patterns.
- Switch to ALLOW/DENY enforcement for sensitive services.
- Default deny for production namespaces.

**Rollout Q&As:**

> **Q1: How do you handle services that cannot have sidecars (e.g., DaemonSets, host-network pods)?**
> A: Annotate with `sidecar.istio.io/inject: "false"`. These services communicate in plaintext. Mesh services calling non-mesh services use `PERMISSIVE` mode on the destination or use a dedicated egress gateway. For host-network pods, iptables interception does not work (pod does not have its own network namespace).

> **Q2: How do you validate that sidecar injection does not break existing services?**
> A: (1) Start with PERMISSIVE mTLS (accepts both plaintext and mTLS). (2) Monitor metrics: request success rate, latency (before and after sidecar injection). (3) Run integration tests with sidecar enabled. (4) Check for applications that use raw TCP (not HTTP) — they may need protocol detection tuning or explicit TCP routes.

> **Q3: How do you roll back sidecar injection if it causes issues?**
> A: Remove the `istio-injection: enabled` label from the namespace and restart all pods (rolling restart). Pods restart without sidecars. iptables rules are not installed (no init container). Instant rollback for the application. Slower rollback for the namespace (pod-by-pod restart).

> **Q4: How do you handle sidecar version upgrades across 20,000 pods?**
> A: Sidecar version is controlled by the injection webhook (points to specific Envoy image tag). To upgrade: update the webhook config with the new image tag, then rolling-restart pods namespace by namespace. Use a canary namespace first. Monitor for: increased error rate, latency regression, sidecar crash loops. Istio supports revision-based canary upgrades (two Istiod versions running simultaneously).

> **Q5: How do you measure the latency overhead introduced by sidecars?**
> A: Compare p50/p99 latency before and after sidecar injection using: (1) Application-level metrics (same request path, with/without sidecar). (2) Envoy's `envoy_http_downstream_rq_time` histogram (total request time including upstream). (3) Subtract `envoy_cluster_upstream_rq_time` (upstream service time) from total to get sidecar overhead. Expected: < 1ms p50, < 2ms p99 for both hops.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Trade-off |
|----------|-------------------|--------|-----------|-----------|
| Proxy technology | Envoy, Linkerd2-proxy, Nginx | Envoy | xDS standard, extensive feature set, CNCF ecosystem | Higher memory (40 MB base) vs. Linkerd2-proxy (10 MB); more complex configuration |
| Traffic interception | iptables REDIRECT, iptables TPROXY, eBPF, application SDK | iptables REDIRECT | Proven, works with any CNI, well-documented | iptables overhead; conntrack table usage; plan eBPF migration |
| Identity framework | SPIFFE (Istiod CA), SPIRE, Vault PKI | SPIFFE via Istiod CA | Integrated with Istio; simpler ops than standalone SPIRE | Less granular than SPIRE (per-SA vs. per-pod identity) |
| mTLS mode | STRICT, PERMISSIVE, DISABLE | PERMISSIVE → STRICT (phased) | Start permissive for compatibility; migrate to strict for security | PERMISSIVE allows plaintext bypass during migration |
| Control plane | Istiod (Istio), Linkerd control plane, standalone Envoy | Istiod | Mature, large community, enterprise support | Complexity; control plane resource usage |
| Config scope | Full mesh visibility, scoped per-namespace | Scoped (Sidecar CRD) | Reduces config size by 100x at scale | Requires explicit service dependency declaration |
| Cert TTL | 1 hour, 24 hours, 7 days | 24 hours | Balance between security (short TTL) and CA load (long TTL) | 24h window where compromised cert is valid |
| Sidecar injection | MutatingWebhook (auto), manual YAML, CNI plugin | MutatingWebhook (auto) | Transparent to developers; namespace-level control | Webhook availability is critical; adds pod startup latency |

---

## 12. Agentic AI Integration

### AI-Powered Service Mesh Operations

**Traffic Anomaly Detection Agent:**
- Monitors per-service request rates, latency distributions, and error rates.
- Detects anomalies: traffic spikes, latency shifts, unusual error patterns, new traffic flows between previously unconnected services.
- Automatically suggests or applies mitigation: tighten rate limits, adjust circuit breaker thresholds, create authorization policies for unexpected flows.
- Example: "Service svc-payment is receiving 10x normal traffic from svc-frontend-v2 (canary). This correlates with the v2 deployment 30 minutes ago. Recommend: add rate limit or roll back canary."

**Canary Analysis Agent:**
- Monitors canary deployments in real-time.
- Compares canary (v2) metrics against baseline (v1): latency p50/p99, error rate, success rate.
- Uses statistical analysis (Bayesian inference) to determine if canary is safe to promote.
- Automatically adjusts canary weight (10% → 25% → 50% → 100%) or rolls back.
- Example: "Canary v2 has 2% higher error rate than v1 after 500 requests (p-value 0.03). Recommending rollback."

**Authorization Policy Recommendation Agent:**
- Analyzes actual traffic patterns (who calls whom, which methods/paths).
- Generates least-privilege AuthorizationPolicy recommendations.
- Identifies over-permissive policies (policy allows paths that are never called).
- Identifies missing policies (traffic flowing without explicit allow).
- Example: "svc-A only calls svc-B on GET /api/users and POST /api/orders. Current policy allows all methods on all paths. Recommend: restrict to observed patterns."

**Configuration Optimization Agent:**
- Analyzes retry and timeout configurations against actual traffic patterns.
- Identifies: retries that never succeed (waste resources), timeouts that are too short (cause unnecessary failures), circuit breaker thresholds that are too loose (allow cascading failures).
- Recommends optimized configurations with impact analysis.
- Example: "svc-B timeout is 30s but p99 response time is 200ms. Recommend: reduce timeout to 5s to fail-fast on genuine outages."

**Capacity Planning Agent:**
- Tracks sidecar resource utilization trends.
- Predicts when sidecar resource requests need adjustment.
- Estimates control plane capacity needs based on mesh growth.
- Example: "At current growth rate, Istiod will exhaust memory in 6 weeks (20,000 → 30,000 sidecars). Recommend: add 2 Istiod replicas and increase memory limit to 8 GB."

### Integration Architecture

```
┌─────────────────────────────────────────────────────┐
│              AI Agent Mesh Controller                 │
│                                                       │
│  Data Sources:                                        │
│  ├── Prometheus (mesh metrics)                        │
│  ├── Envoy access logs (request patterns)             │
│  ├── Kiali (service graph)                            │
│  └── Kubernetes events (deployments, scaling)         │
│                                                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐   │
│  │ Anomaly  │  │ Canary   │  │ AuthZ Policy     │   │
│  │ Detector │  │ Analyzer │  │ Recommender      │   │
│  └────┬─────┘  └────┬─────┘  └────┬─────────────┘   │
│       │              │              │                  │
│       └──────────────┼──────────────┘                  │
│                      ▼                                 │
│              ┌──────────────┐                          │
│              │ Action Queue │                          │
│              │  + Approval  │                          │
│              └──────┬───────┘                          │
│                     │                                  │
│                     ▼                                  │
│              ┌──────────────┐                          │
│              │  K8s API     │                          │
│              │  (apply CRDs)│                          │
│              └──────────────┘                          │
│                                                       │
│  Safety: max 1 config change per minute                │
│  Safety: auto-rollback if error rate > 2x after change │
│  Safety: human approval for AuthZ policy changes       │
└─────────────────────────────────────────────────────┘
```

---

## 13. Complete Interviewer Q&A Bank

> **Q1: What is the xDS protocol and what are its resource types?**
> A: xDS is Envoy's discovery service protocol. Resource types: LDS (Listener) defines what ports to listen on and filter chains; RDS (Route) defines how to match requests to clusters; CDS (Cluster) defines upstream service groups with LB and health check config; EDS (Endpoint) provides the actual pod IPs for each cluster; SDS (Secret) distributes TLS certificates and keys. All use gRPC streaming for push-based updates.

> **Q2: How does sidecar injection work in Kubernetes?**
> A: A MutatingAdmissionWebhook intercepts pod creation requests. If the namespace has the `istio-injection: enabled` label, the webhook modifies the pod spec to: (1) Add an init container (`istio-init`) that sets up iptables rules. (2) Add the Envoy sidecar container with appropriate environment variables. (3) Add volume mounts for certificates and config. The application container is unmodified.

> **Q3: How does Envoy handle HTTP/2 and gRPC differently from HTTP/1.1?**
> A: HTTP/2 multiplexes multiple streams (requests) over a single TCP connection. Envoy's L7 load balancing works per-stream (per-RPC for gRPC), not per-connection. This enables fair load distribution even with persistent connections. HTTP/1.1 requires a new connection (or pipelining, rarely used) per request. Envoy manages separate connection pools for HTTP/1.1 (one request at a time per connection) and HTTP/2 (multiple concurrent streams per connection).

> **Q4: What is the difference between VirtualService and DestinationRule?**
> A: VirtualService defines routing rules: how to match requests (host, path, headers) and where to route them (which subset, weight). DestinationRule defines policies applied after routing: load balancing algorithm, connection pool settings, circuit breaker thresholds, mTLS settings, and defines subsets (e.g., v1, v2 based on pod labels). They work together: VirtualService routes to a subset defined in DestinationRule.

> **Q5: How do you debug a failing request in the service mesh?**
> A: (1) Check Envoy access logs on both client and server sidecars (correlate by request ID). (2) Check Envoy's admin endpoint `/config_dump` for current route/cluster config. (3) Use `istioctl proxy-status` to check if sidecar config is in sync with control plane. (4) Use `istioctl analyze` to detect configuration issues. (5) Check distributed trace to see where in the call chain the failure occurs. (6) Check Envoy stats for circuit breaker trips, retry exhaustion, or connection pool exhaustion.

> **Q6: What is Istio ambient mesh and how does it differ from sidecar mode?**
> A: Ambient mesh replaces per-pod sidecars with: (1) Per-node `ztunnel` proxy for L4 (mTLS, identity). (2) Optional per-service `waypoint proxy` for L7 (routing, policies). Benefits: no sidecar resource overhead per pod, simplified pod lifecycle, no iptables interception. Trade-off: L7 features require an additional hop (to the waypoint proxy), and it is newer (less battle-tested).

> **Q7: How do you handle gRPC health checking in the service mesh?**
> A: gRPC has a standard health check protocol (`grpc.health.v1.Health.Check`). Envoy supports this natively as an active health check type. The sidecar periodically calls the gRPC health endpoint on the upstream. For Kubernetes readiness probes, use `grpc` probe type (K8s 1.24+). The sidecar's own health is checked via `/healthz/ready` on port 15021.

> **Q8: How does fault injection work in the mesh?**
> A: VirtualService supports `fault` block with `delay` (add artificial latency) and `abort` (return error without calling upstream). Example: inject 500ms delay into 10% of requests to svc-B. The client-side Envoy applies the fault before forwarding. This is used for chaos engineering: verify that circuit breakers, retries, and timeouts work correctly under degraded conditions.

> **Q9: How do you handle TCP (non-HTTP) traffic in the mesh?**
> A: Envoy supports TCP proxying without L7 parsing. mTLS still works (TLS is L4). But L7 features (path-based routing, retry, fault injection) are not available. For protocols like MySQL or Redis, Envoy has protocol-specific filters that can parse the protocol for observability (e.g., MySQL command type, Redis command). Traffic is matched by port number and routed to the appropriate cluster.

> **Q10: How do you handle WebSocket connections through the mesh?**
> A: WebSocket starts as an HTTP Upgrade request. Envoy handles the upgrade and then switches to raw TCP proxying for the WebSocket frames. mTLS encrypts the WebSocket traffic. Special considerations: WebSocket connections are long-lived, so connection-level circuit breakers (max_connections) must account for them. Timeouts should be set appropriately (idle timeout for WebSocket, not request timeout).

> **Q11: What is the blast radius if Envoy has a CVE?**
> A: Every pod in the mesh runs Envoy, so a CVE affects the entire cluster. Mitigation: (1) Pin to specific Envoy versions (do not use `latest`). (2) Subscribe to Envoy security advisories. (3) Rapid patching: update sidecar injection webhook with patched image, then rolling restart pods. (4) Use Istio's revision-based upgrades to canary the patched version. (5) Network policies provide defense-in-depth even if Envoy is compromised.

> **Q12: How do you handle egress traffic (mesh to external services)?**
> A: By default, Istio allows all egress (ALLOW_ANY mode) or denies all egress (REGISTRY_ONLY mode). For controlled egress: (1) Create ServiceEntry resources for each allowed external service. (2) Apply DestinationRules for TLS origination (sidecar initiates TLS to external). (3) Use an egress gateway for centralized egress control, logging, and policy. This is critical for compliance (e.g., preventing data exfiltration).

> **Q13: What are the alternatives to Envoy for the sidecar proxy?**
> A: Linkerd2-proxy (Rust, purpose-built for service mesh, 10 MB memory, simpler but fewer features), Nginx (mature but lacks xDS dynamic config), MOSN (Go-based, used in Ant Financial), proxy-wasm-compatible proxies. Envoy dominates because of the xDS standard, community size, and feature breadth. For specific use cases (low memory, simple needs), Linkerd2-proxy is a strong alternative.

> **Q14: How does request mirroring/shadowing work in the mesh?**
> A: VirtualService's `mirror` directive sends a copy of each request to a shadow service. The primary request is forwarded normally; the mirrored request is fire-and-forget (response is discarded). Use cases: testing a new service version with production traffic without affecting users. The mirror's response time does not affect the primary path's latency.

> **Q15: How do you implement request-level authorization (e.g., user X can only access their own resources)?**
> A: The sidecar handles identity-level authorization (which service can call which). Request-level authorization (user X accessing resource Y) is application-layer logic. However, the mesh can help: (1) JWT claim extraction in the sidecar (RequestAuthentication CRD). (2) AuthorizationPolicy can match on JWT claims (e.g., `request.auth.claims[role] == "admin"`). (3) For complex ABAC, use ext_authz filter to call an external authorization service (OPA/Cedar).

> **Q16: How do you handle service mesh multi-cluster communication?**
> A: Two models: (1) Shared control plane: one Istiod manages multiple clusters (connected via API server access). Services in all clusters are in the same mesh. (2) Federated: each cluster has its own Istiod; cross-cluster traffic goes through ingress/egress gateways with mTLS. Trust is established by sharing root CA across clusters. DNS resolution: `svc-B.ns.svc.cluster.local` resolves to remote cluster endpoints via Istio's DNS proxy.

---

## 14. References

- **Envoy Proxy Architecture:** https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/
- **Envoy Threading Model:** https://blog.envoyproxy.io/envoy-threading-model-a8d44b922310
- **xDS Protocol Specification:** https://www.envoyproxy.io/docs/envoy/latest/api-docs/xds_protocol
- **SPIFFE Specification:** https://spiffe.io/docs/latest/spiffe-about/overview/
- **Istio Architecture:** https://istio.io/latest/docs/ops/deployment/architecture/
- **Istio Security (mTLS, AuthZ):** https://istio.io/latest/docs/concepts/security/
- **Envoy Circuit Breaking:** https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/circuit_breaking
- **Envoy Outlier Detection:** https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/outlier
- **Istio Ambient Mesh:** https://istio.io/latest/docs/ambient/overview/
- **Cilium eBPF Service Mesh:** https://docs.cilium.io/en/stable/network/servicemesh/
