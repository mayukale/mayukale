# System Design: Service Mesh Design

> **Relevance to role:** A service mesh is the networking layer that infrastructure platform engineers provide to application teams — it gives them mTLS, observability, traffic management, and resiliency without modifying application code. Understanding how Istio/Envoy work at the data-plane and control-plane level, including the xDS API, certificate lifecycle, and ambient mesh architecture, is essential for operating and troubleshooting production service meshes at scale.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Mutual TLS (mTLS) between all services — automatic certificate issuance and rotation |
| FR-2 | Traffic management: canary deployments, A/B testing, traffic mirroring, fault injection |
| FR-3 | Resiliency: circuit breaking, retries, timeouts, rate limiting |
| FR-4 | Observability: automatic metrics, distributed tracing, access logs |
| FR-5 | Service discovery and load balancing (client-side, L7-aware) |
| FR-6 | Authorization policies (L4/L7) — which service can call which endpoint |
| FR-7 | Ingress and egress gateway management |
| FR-8 | Multi-cluster service mesh (cross-cluster service discovery and traffic) |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Data plane latency overhead (per hop) | < 1 ms p99 (sidecar), < 0.5 ms (ambient) |
| NFR-2 | Control plane configuration propagation | < 5 s from policy change to enforcement |
| NFR-3 | Certificate rotation | Automatic, < 24 h TTL default |
| NFR-4 | Data plane memory overhead | < 50 MB per sidecar proxy |
| NFR-5 | Data plane CPU overhead | < 5% per sidecar |
| NFR-6 | Control plane availability | 99.99% |
| NFR-7 | Max services in mesh | 10,000+ |
| NFR-8 | Max pods in mesh | 100,000+ |

### Constraints & Assumptions
- Kubernetes 1.28+ as the platform.
- Istio as the primary mesh implementation (most widely deployed).
- Envoy as the data plane proxy.
- Certificate authority: Istio CA (citadel) with SPIFFE identity.
- Prometheus for metrics, Jaeger/Tempo for tracing.
- Consider ambient mesh (sidecar-less) as the future direction.

### Out of Scope
- API gateway (Istio Gateway is covered; full API management is separate).
- Service mesh for non-Kubernetes workloads (VMs — mentioned briefly).
- CNI-level features (covered in container_orchestration_system.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|------------|--------|
| Services in mesh | Large platform | 5,000 |
| Pods in mesh | 5,000 services x 5 replicas avg | 25,000 |
| Sidecar proxies (sidecar mode) | 1 per pod | 25,000 |
| RPS through mesh | 25,000 pods x 100 RPS avg | 2,500,000 RPS |
| xDS config pushes (steady state) | 100 config changes/hour x 25,000 proxies | 2.5M pushes/hour |
| Certificate issuance/rotation | 25,000 certs x 1 rotation/24h | ~17 cert operations/min |
| Tracing spans | 2.5M RPS x 1% sampling x 3 spans avg | 75,000 spans/s |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| Sidecar proxy latency (p50) | < 0.5 ms | Envoy in-process forwarding |
| Sidecar proxy latency (p99) | < 1 ms | Includes connection setup, TLS handshake (session reuse) |
| Ambient mesh latency (p50) | < 0.3 ms | ztunnel in-kernel forwarding |
| mTLS handshake (first connection) | < 5 ms | ECDSA P-256 key exchange |
| mTLS handshake (session resume) | < 1 ms | TLS session ticket |
| xDS config propagation | < 5 s | Istiod → Envoy ADS push |
| Certificate issuance | < 1 s | Istiod CA signs CSR |

### Storage Estimates

| Component | Calculation | Result |
|-----------|------------|--------|
| Envoy configuration per proxy | ~500 KB (routes, clusters, listeners) | 500 KB x 25,000 = 12.5 GB total |
| Istiod memory (config cache) | 5,000 services x 10 KB config | ~50 MB |
| Istiod memory (xDS connections) | 25,000 connections x 10 KB state | ~250 MB |
| Tracing storage | 75,000 spans/s x 1 KB x 86,400s | ~6.5 TB/day (before sampling) |
| Prometheus metrics | 25,000 proxies x 200 metrics x 8 B x 86,400s / 15s | ~230 GB/day |

### Bandwidth Estimates

| Flow | Calculation | Result |
|------|------------|--------|
| xDS updates (istiod → proxies) | 100 changes/hour x 500 KB push x 25,000 proxies (incremental) | ~3.5 MB/s avg, bursty |
| mTLS overhead per connection | ~1 KB handshake + ~20 B/packet encryption overhead | < 1% bandwidth overhead |
| Metrics export (proxy → Prometheus) | 25,000 proxies x 10 KB/scrape / 15s | ~16.7 MB/s |
| Tracing export (proxy → collector) | 75,000 spans/s x 1 KB | ~75 MB/s |
| Access log shipping | 2.5M RPS x 200 B/log | ~500 MB/s (if enabled for all requests) |

---

## 3. High Level Architecture

```
                    ┌────────────────────────────────────────────┐
                    │              Control Plane                  │
                    │                                            │
                    │  ┌────────────────────────────────────┐   │
                    │  │            istiod                   │   │
                    │  │                                    │   │
                    │  │  ┌──────────┐  ┌───────────────┐  │   │
                    │  │  │  Pilot   │  │  Citadel (CA) │  │   │
                    │  │  │  (xDS    │  │  (SPIFFE cert │  │   │
                    │  │  │  server) │  │   issuance)   │  │   │
                    │  │  └──────────┘  └───────────────┘  │   │
                    │  │                                    │   │
                    │  │  ┌──────────┐  ┌───────────────┐  │   │
                    │  │  │  Galley  │  │  Sidecar      │  │   │
                    │  │  │  (config │  │  Injector     │  │   │
                    │  │  │  valid.) │  │  (webhook)    │  │   │
                    │  │  └──────────┘  └───────────────┘  │   │
                    │  └────────────────────────────────────┘   │
                    └──────────────┬─────────────────────────────┘
                                  │ xDS (gRPC/ADS)
                                  │ + SDS (certificates)
                    ┌─────────────▼─────────────────────────────┐
                    │              Data Plane                     │
                    │                                            │
                    │  ┌──────────────────────────────────────┐ │
                    │  │         Pod (Service A)               │ │
                    │  │  ┌──────────┐  ┌─────────────────┐  │ │
                    │  │  │  App     │  │  Envoy Sidecar  │  │ │
                    │  │  │ Container│  │  - mTLS         │  │ │
                    │  │  │ :8080    │←→│  - Load balance │  │ │
                    │  │  │          │  │  - Retry/CB     │  │ │
                    │  │  │          │  │  - Metrics      │  │ │
                    │  │  │          │  │  - Tracing      │  │ │
                    │  │  └──────────┘  └────────┬────────┘  │ │
                    │  └─────────────────────────┼────────────┘ │
                    │                            │ mTLS          │
                    │  ┌─────────────────────────┼────────────┐ │
                    │  │         Pod (Service B)  │            │ │
                    │  │  ┌──────────┐  ┌────────▼────────┐  │ │
                    │  │  │  App     │  │  Envoy Sidecar  │  │ │
                    │  │  │ Container│  │                  │  │ │
                    │  │  │ :9090    │←→│                  │  │ │
                    │  │  └──────────┘  └─────────────────┘  │ │
                    │  └──────────────────────────────────────┘ │
                    │                                            │
                    │  ┌──────────────────────────────────────┐ │
                    │  │      Ingress Gateway                  │ │
                    │  │  (Envoy — external traffic entry)     │ │
                    │  └──────────────────────────────────────┘ │
                    │  ┌──────────────────────────────────────┐ │
                    │  │      Egress Gateway                   │ │
                    │  │  (Envoy — external traffic exit)      │ │
                    │  └──────────────────────────────────────┘ │
                    └────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **istiod** | Unified control plane (Pilot + Citadel + Galley merged since Istio 1.5) |
| **Pilot** | Translates Istio config (VirtualService, DestinationRule) to Envoy xDS config and pushes to all proxies |
| **Citadel (CA)** | Issues SPIFFE X.509 certificates for workload identity; handles rotation |
| **Sidecar Injector** | Mutating webhook that injects Envoy sidecar + init container into pods |
| **Envoy Proxy** | Data plane proxy: handles all inbound/outbound traffic for a pod |
| **Ingress Gateway** | Envoy proxy at cluster edge; terminates external TLS, routes to internal services |
| **Egress Gateway** | Envoy proxy for outbound traffic; enforces egress policies, provides auditing |

### Data Flows

1. **Configuration flow:** User applies VirtualService → istiod validates → Pilot translates to Envoy route config → pushes via xDS to all affected Envoy proxies → Envoy hot-reloads routes without restart.

2. **Request flow (with sidecar):** Client pod → iptables REDIRECT to Envoy sidecar (outbound) → Envoy performs service discovery, load balancing, mTLS → packet sent to destination → destination Envoy sidecar (inbound, iptables REDIRECT) → mTLS termination → forward to application container → response path reverses.

3. **Certificate flow:** Pod starts → Envoy sidecar requests certificate from istiod via SDS (Secret Discovery Service) → istiod validates pod identity (k8s service account) → istiod CA signs certificate with SPIFFE ID → certificate returned to Envoy → Envoy uses cert for mTLS → rotation happens automatically before expiry.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# VirtualService — traffic routing rules
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
metadata:
  name: reviews-route
  namespace: production
spec:
  hosts:
    - reviews.production.svc.cluster.local
  http:
    - match:
        - headers:
            end-user:
              exact: "internal-tester"
      route:
        - destination:
            host: reviews
            subset: v2
          weight: 100
    - route:
        - destination:
            host: reviews
            subset: v1
          weight: 90
        - destination:
            host: reviews
            subset: v2
          weight: 10       # 10% canary to v2
      retries:
        attempts: 3
        perTryTimeout: 2s
        retryOn: "5xx,reset,connect-failure"
      timeout: 10s
      fault:
        delay:
          percentage:
            value: 0.1     # Inject 100ms delay in 0.1% of requests (chaos testing)
          fixedDelay: 100ms

---
# DestinationRule — traffic policies + subsets
apiVersion: networking.istio.io/v1beta1
kind: DestinationRule
metadata:
  name: reviews-destination
  namespace: production
spec:
  host: reviews.production.svc.cluster.local
  trafficPolicy:
    connectionPool:
      tcp:
        maxConnections: 100
      http:
        h2UpgradePolicy: DEFAULT
        http1MaxPendingRequests: 100
        http2MaxRequests: 1000
        maxRequestsPerConnection: 10
    outlierDetection:
      consecutive5xxErrors: 5
      interval: 10s
      baseEjectionTime: 30s
      maxEjectionPercent: 50
    loadBalancer:
      simple: LEAST_REQUEST    # Or ROUND_ROBIN, RANDOM, PASSTHROUGH
    tls:
      mode: ISTIO_MUTUAL       # Automatic mTLS
  subsets:
    - name: v1
      labels:
        version: v1
    - name: v2
      labels:
        version: v2
      trafficPolicy:
        connectionPool:
          http:
            http2MaxRequests: 500   # Lower limit for canary

---
# Gateway — ingress/egress entry point
apiVersion: networking.istio.io/v1beta1
kind: Gateway
metadata:
  name: production-gateway
  namespace: istio-system
spec:
  selector:
    istio: ingressgateway
  servers:
    - port:
        number: 443
        name: https
        protocol: HTTPS
      tls:
        mode: SIMPLE
        credentialName: production-tls-cert
      hosts:
        - "api.company.com"
        - "*.company.com"

---
# AuthorizationPolicy — L7 access control
apiVersion: security.istio.io/v1beta1
kind: AuthorizationPolicy
metadata:
  name: reviews-authz
  namespace: production
spec:
  selector:
    matchLabels:
      app: reviews
  action: ALLOW
  rules:
    - from:
        - source:
            principals: ["cluster.local/ns/production/sa/frontend"]
      to:
        - operation:
            methods: ["GET"]
            paths: ["/api/v1/reviews/*"]
    - from:
        - source:
            principals: ["cluster.local/ns/production/sa/admin"]
      to:
        - operation:
            methods: ["GET", "POST", "PUT", "DELETE"]
            paths: ["/api/v1/*"]

---
# PeerAuthentication — mTLS policy
apiVersion: security.istio.io/v1beta1
kind: PeerAuthentication
metadata:
  name: default
  namespace: production
spec:
  mtls:
    mode: STRICT    # All traffic must be mTLS (no plaintext)

---
# ServiceEntry — register external services in the mesh
apiVersion: networking.istio.io/v1beta1
kind: ServiceEntry
metadata:
  name: external-api
  namespace: production
spec:
  hosts:
    - api.external-provider.com
  location: MESH_EXTERNAL
  ports:
    - number: 443
      name: https
      protocol: HTTPS
  resolution: DNS
```

### Database Selection

| Data Type | Storage | Justification |
|-----------|---------|---------------|
| Istio CRDs (VirtualService, etc.) | etcd (via API server) | Native k8s CRDs; watched by istiod |
| Envoy xDS config (per proxy) | istiod memory → pushed to Envoy | Real-time streaming; no persistent storage needed |
| TLS certificates | istiod CA (in-memory) + k8s Secrets | Short-lived (24h); auto-rotated |
| Metrics | Prometheus | Standard time-series storage |
| Traces | Jaeger/Tempo | Distributed tracing backend |
| Access logs | Elasticsearch/Loki | Full-text search for debugging |

### Indexing Strategy

| Index | Purpose |
|-------|---------|
| Service hostname → VirtualService | Route lookup during xDS translation |
| Pod labels → DestinationRule subset | Traffic policy selection |
| Service account → SPIFFE identity | Certificate issuance mapping |
| Namespace → PeerAuthentication | mTLS mode lookup |

---

## 5. API Design

### REST/gRPC/kubectl Endpoints

```bash
# Istio configuration
kubectl apply -f virtual-service.yaml
kubectl get virtualservices -n production
kubectl get destinationrules -n production
kubectl get authorizationpolicies -n production
kubectl get peerauthentication -n production
kubectl get gateways -n istio-system

# Debug proxy configuration
istioctl proxy-config routes productpage-v1-abc123 -n production
istioctl proxy-config clusters productpage-v1-abc123 -n production
istioctl proxy-config endpoints productpage-v1-abc123 -n production
istioctl proxy-config listeners productpage-v1-abc123 -n production
istioctl proxy-config secret productpage-v1-abc123 -n production  # TLS certs

# Analyze configuration for issues
istioctl analyze -n production
istioctl analyze --all-namespaces

# Proxy status (sync between istiod and proxies)
istioctl proxy-status

# Debug mesh connectivity
istioctl x authz check productpage-v1-abc123 -n production
istioctl x describe pod productpage-v1-abc123 -n production

# Dashboard access
istioctl dashboard kiali         # Service mesh visualization
istioctl dashboard jaeger        # Distributed tracing
istioctl dashboard prometheus    # Metrics
istioctl dashboard grafana       # Dashboards
istioctl dashboard envoy productpage-v1-abc123  # Envoy admin UI
```

### xDS API (Envoy Discovery Services)

```
xDS Protocol (gRPC streaming between istiod and Envoy):

LDS (Listener Discovery Service):
  - Defines listening sockets and filter chains
  - Maps incoming traffic to routes
  - Example: "listen on 0.0.0.0:80, apply HTTP connection manager"

RDS (Route Discovery Service):
  - Defines routing rules (maps to VirtualService)
  - Host matching, path matching, header matching
  - Example: "if host=reviews, path=/api/v1/*, route to cluster reviews-v2"

CDS (Cluster Discovery Service):
  - Defines upstream service clusters (maps to DestinationRule)
  - Connection pool settings, load balancing, outlier detection
  - Example: "cluster reviews-v2: max_connections=100, lb=LEAST_REQUEST"

EDS (Endpoint Discovery Service):
  - Defines endpoints within clusters (maps to k8s Endpoints)
  - IP:port pairs for each service version
  - Example: "reviews-v2 endpoints: [10.244.1.5:9080, 10.244.2.7:9080]"

SDS (Secret Discovery Service):
  - Delivers TLS certificates to Envoy
  - Automatic rotation without proxy restart
  - Example: "certificate for spiffe://cluster.local/ns/production/sa/reviews"

ADS (Aggregated Discovery Service):
  - Single gRPC stream for all xDS types
  - Ensures consistent ordering (LDS before RDS before CDS before EDS)
  - Prevents configuration inconsistency during updates

Push Flow:
  1. User applies VirtualService
  2. istiod detects change (k8s watch)
  3. istiod translates to Envoy config (LDS + RDS + CDS + EDS)
  4. istiod pushes via ADS to affected proxies
  5. Envoy hot-reloads configuration (no connection drop)
  6. New routing rules take effect within seconds
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: mTLS and Certificate Management (SPIFFE/SPIRE)

**Why it's hard:**
Every service-to-service connection must be authenticated and encrypted. This requires: (1) identity for every workload (not just the node), (2) short-lived certificates to limit blast radius, (3) automatic rotation without downtime, (4) certificate authority that scales to 100K+ certificates, and (5) cross-cluster identity federation for multi-cluster meshes.

**Approaches Compared:**

| Approach | Identity Model | Auto-Rotation | Complexity | Multi-Cluster |
|----------|---------------|---------------|-----------|---------------|
| Istio CA (built-in) | SPIFFE, k8s SA-based | Yes (24h default) | Low | Shared root CA |
| SPIRE (full SPIFFE) | SPIFFE, workload attestation | Yes (configurable) | High | Native federation |
| cert-manager + mesh | X.509, configurable | Yes | Medium | Manual root CA sharing |
| Vault PKI | X.509, Vault identity | Yes | High | Vault federation |

**Selected: Istio CA (citadel) with SPIFFE identity, pluggable root CA**

**Implementation Detail:**

```
SPIFFE Identity Model:
  Every workload gets a SPIFFE ID:
    spiffe://cluster.local/ns/<namespace>/sa/<service-account>
    
  Example:
    spiffe://cluster.local/ns/production/sa/reviews
    
  This identity is encoded in the X.509 certificate's SAN (Subject Alternative Name).
  
  Trust domain: "cluster.local" (configurable per cluster)
  For multi-cluster: each cluster can have its own trust domain,
  federated via shared root CA or cross-signing.

Certificate Lifecycle:
  
  1. Pod starts → Envoy sidecar initializes
  2. Envoy sends SDS (Secret Discovery Service) request to istiod:
     "I need a certificate for spiffe://cluster.local/ns/production/sa/reviews"
  3. istiod validates the request:
     a. Verify pod exists in the specified namespace
     b. Verify pod uses the specified service account
     c. Verify via k8s TokenReview (JWT token mounted in the pod)
  4. istiod CA generates certificate:
     - Subject: spiffe://cluster.local/ns/production/sa/reviews
     - Validity: 24 hours (configurable)
     - Key type: ECDSA P-256
     - Signed by istiod's CA key
  5. Certificate returned to Envoy via SDS
  6. Envoy uses certificate for all mTLS connections
  7. Before expiry (at ~80% of TTL), Envoy requests a new certificate
  8. istiod issues new certificate → Envoy rotates without connection drop

mTLS Connection Flow:
  
  Client Envoy                                Server Envoy
       │                                            │
       │──── TLS ClientHello ──────────────────────>│
       │                                            │
       │<─── TLS ServerHello + Server Cert ─────────│
       │     (contains SPIFFE ID of server)         │
       │                                            │
       │──── Client Cert ──────────────────────────>│
       │     (contains SPIFFE ID of client)         │
       │                                            │
       │<─── TLS Finished ─────────────────────────>│
       │     (both sides verified, session key set) │
       │                                            │
       │<──── Encrypted HTTP/2 traffic ────────────>│
       │                                            │
  
  Verification:
  - Client verifies server cert against Istio root CA
  - Server verifies client cert against Istio root CA
  - Both check SPIFFE ID in SAN field
  - AuthorizationPolicy checks: is this SPIFFE ID allowed?

Root CA Options:
  
  Option A: Self-signed (default)
    istiod generates a self-signed root CA on startup
    Stored as k8s Secret (istio-ca-secret in istio-system)
    Simple but requires manual distribution for multi-cluster
    
  Option B: Pluggable CA (recommended for production)
    Use external CA (Vault, AWS ACM PCA, Google Cloud CAS)
    istiod acts as intermediate CA, signed by external root
    Better for: compliance, audit trail, HSM-backed keys
    
  Option C: SPIRE integration
    Replace istiod CA entirely with SPIRE server
    Full SPIFFE attestation (node + workload)
    Stronger identity verification but more complex
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| istiod CA down | No new certificates issued; existing certs work until expiry | HA istiod (3 replicas); certificate TTL provides buffer |
| Root CA key compromised | All mesh identity compromised | HSM-backed root CA; short-lived certificates limit damage; rotate root |
| Certificate expired (rotation failed) | mTLS connections fail; traffic blocked | Monitor cert expiry; alert at 50% TTL; istiod auto-retries |
| Clock skew between nodes | Certificate validation fails (not yet valid / expired) | NTP synchronization; certificate tolerance for clock skew |
| SPIFFE ID spoofing | Attacker impersonates a service | Pod identity verified via k8s TokenReview; RBAC on service accounts |

**Interviewer Q&As:**

**Q1: Why does Istio use SPIFFE identity instead of IP-based identity?**
A: Pod IPs are ephemeral — they change on every pod restart. IP-based identity cannot survive scaling events, deployments, or node failures. SPIFFE identity (`spiffe://<trust-domain>/ns/<namespace>/sa/<service-account>`) is tied to the workload identity (service account), not the network location. Benefits: (1) Identity survives pod restarts. (2) Works across clusters (same service account = same identity). (3) Decoupled from network topology. (4) Compatible with zero-trust networking principles.

**Q2: How do you handle mTLS migration (permissive to strict mode)?**
A: (1) Start with `PeerAuthentication.mtls.mode: PERMISSIVE` — Envoy accepts both mTLS and plaintext. (2) Monitor Kiali dashboard: which connections are already mTLS? Which are plaintext? (3) Gradually inject sidecars into services that don't have them. (4) Once all services have sidecars and all traffic is mTLS (visible in Kiali): switch to `mode: STRICT` per namespace, then globally. (5) Test in staging first. (6) Exception: external services without sidecars use ServiceEntry with `tls.mode: DISABLE`.

**Q3: How does certificate rotation happen without dropping connections?**
A: Envoy uses SDS (Secret Discovery Service) — a streaming gRPC API. When istiod issues a new certificate, it pushes via SDS to Envoy. Envoy performs a "hot swap": (1) New connections use the new certificate immediately. (2) Existing connections continue using the old certificate until they naturally close. (3) There is no connection reset or handshake renegotiation. The key insight: TLS session resumption ensures that even the new certificate doesn't cause a full handshake — session tickets from the old connection can be reused.

**Q4: How do you implement cross-cluster mTLS?**
A: Two approaches: (1) **Shared root CA**: all clusters use the same root CA (or root CA signs intermediate CAs for each cluster). Certificates from any cluster are trusted by all clusters. (2) **Trust domain federation**: each cluster has its own trust domain (`cluster-a.local`, `cluster-b.local`). istiod in each cluster is configured to trust the other cluster's root CA. Federation is configured via `MeshConfig.trustDomainAliases`. Both approaches require network connectivity between clusters (east-west gateway).

**Q5: What is the performance impact of mTLS on every request?**
A: First connection: ~3-5ms for full TLS handshake (ECDSA P-256). Subsequent connections: < 0.5ms with TLS session resumption (session tickets or session cache). Per-request overhead: ~20 bytes for TLS record layer + encryption (AES-GCM-128 is hardware-accelerated on modern CPUs). In practice: < 1ms added latency per hop for p99. The overhead is amortized over HTTP/2 connection multiplexing — one TLS connection handles many requests.

**Q6: How do you handle services that cannot participate in mTLS (legacy, third-party)?**
A: (1) Use PeerAuthentication `portLevelMtls` to disable mTLS on specific ports. (2) Use DestinationRule `tls.mode: DISABLE` for specific destinations. (3) ServiceEntry for external services with explicit TLS configuration. (4) Egress gateway: mesh traffic exits via the gateway, which handles TLS to the external service. (5) For in-cluster legacy: use `PeerAuthentication.mode: PERMISSIVE` on the legacy service's namespace.

---

### Deep Dive 2: Traffic Management

**Why it's hard:**
Fine-grained traffic control (canary deployments, A/B testing, fault injection, traffic mirroring) requires modifying routing at L7 without application changes. The routing configuration must be consistent across all proxies, handle failover gracefully, and not introduce significant latency. Misconfigured routing can cause outages.

**Approaches Compared:**

| Approach | L7 Routing | Canary | Traffic Mirror | Complexity | Overhead |
|----------|-----------|--------|---------------|-----------|---------|
| Istio VirtualService | Full (header, path, weight) | Yes | Yes | Medium | ~0.5ms |
| Kubernetes Ingress | Limited (path-based only) | Limited | No | Low | Minimal |
| Linkerd | Good (header, weight) | Yes | Yes | Low | ~0.3ms |
| Application-level | Custom | Custom | Custom | Very high | Varies |

**Selected: Istio VirtualService + DestinationRule**

**Implementation Detail — Canary Deployment:**

```yaml
# Step 1: Deploy v2 alongside v1
# Two Deployments: reviews-v1 (production), reviews-v2 (canary)
# Both have label: app=reviews
# v1 has: version=v1
# v2 has: version=v2

# Step 2: DestinationRule defines subsets
apiVersion: networking.istio.io/v1beta1
kind: DestinationRule
metadata:
  name: reviews
spec:
  host: reviews
  subsets:
    - name: v1
      labels:
        version: v1
    - name: v2
      labels:
        version: v2

# Step 3: VirtualService routes traffic
# Start: 100% to v1
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
metadata:
  name: reviews
spec:
  hosts:
    - reviews
  http:
    - route:
        - destination:
            host: reviews
            subset: v1
          weight: 100
        - destination:
            host: reviews
            subset: v2
          weight: 0

# Step 4: Gradually shift traffic
# 10% to v2 (canary)
    - route:
        - destination:
            host: reviews
            subset: v1
          weight: 90
        - destination:
            host: reviews
            subset: v2
          weight: 10

# Step 5: Monitor canary metrics
# If error rate for v2 is acceptable, increase to 50%
# If error rate spikes, revert to 0%

# Step 6: Full rollout
    - route:
        - destination:
            host: reviews
            subset: v2
          weight: 100

# Advanced: Header-based routing (dark launch)
  http:
    - match:
        - headers:
            x-canary:
              exact: "true"
      route:
        - destination:
            host: reviews
            subset: v2
    - route:
        - destination:
            host: reviews
            subset: v1
```

**Traffic Mirroring (Shadow Traffic):**

```yaml
# Mirror 100% of traffic to v2 (async, fire-and-forget)
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
metadata:
  name: reviews
spec:
  hosts:
    - reviews
  http:
    - route:
        - destination:
            host: reviews
            subset: v1
          weight: 100
      mirror:
        host: reviews
        subset: v2
      mirrorPercentage:
        value: 100.0
# v1 handles all real traffic
# v2 receives a copy of all requests (responses are discarded)
# Use case: test v2 with real production traffic without risk
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Misconfigured weight (sum != 100) | Istio rejects config | `istioctl analyze` catches errors before apply |
| Canary v2 failing but traffic still routed | Users experience errors | Automated canary analysis (Flagger/Argo Rollouts) auto-reverts on error spike |
| xDS push failure | Proxies stuck on old config | Monitor `istio_xds_pushes{type="error"}`; istiod retries |
| Circular routing | Infinite loop between services | Envoy max retries + request timeout prevents infinite loops |
| VirtualService conflict (overlapping hosts) | Unpredictable routing | `istioctl analyze` detects conflicts |

**Interviewer Q&As:**

**Q1: How do you implement automated canary analysis with Istio?**
A: Use Flagger or Argo Rollouts with Istio: (1) Define a Canary resource with progressive traffic shifting (e.g., 10% → 20% → 50% → 100%). (2) At each step, Flagger queries Prometheus for canary metrics (error rate, latency p99). (3) If metrics are within thresholds, increase traffic. (4) If metrics breach thresholds, roll back to 0% canary. (5) Flagger modifies the VirtualService weights automatically. (6) The entire process is hands-free after initial deployment.

**Q2: How does Istio handle circuit breaking and what happens when a circuit opens?**
A: DestinationRule `outlierDetection` implements circuit breaking: (1) Track error rate per endpoint (pod IP). (2) If a pod has 5 consecutive 5xx errors, eject it from the load balancer pool for 30s (`baseEjectionTime`). (3) After 30s, the pod is re-added (trial). (4) If it fails again, ejection time increases (exponential backoff). (5) `maxEjectionPercent: 50` ensures at least 50% of endpoints remain in the pool. (6) Ejected traffic is redistributed to healthy endpoints. This is per-proxy: each Envoy tracks its own view of endpoint health.

**Q3: How does traffic mirroring work without affecting the primary traffic?**
A: Envoy clones the request and sends the copy asynchronously to the mirror destination: (1) The original request proceeds normally to the primary destination. (2) The mirrored request is fire-and-forget — the response from the mirror is discarded. (3) The mirror request has a special header (`x-envoy-original-path`) indicating it's a copy. (4) Mirroring does NOT double the client-perceived latency (it's async). (5) It does increase server-side load on the mirror destination. (6) Use case: test a new version with production traffic patterns without any risk to production users.

---

### Deep Dive 3: Ambient Mesh (Sidecar-less Architecture)

**Why it's hard:**
The sidecar model adds ~50 MB RAM and ~0.5 ms latency per pod. At 25,000 pods, that's 1.25 TB of RAM just for sidecars. Ambient mesh eliminates sidecars by implementing L4 (mTLS, routing) in a per-node daemon (ztunnel) and L7 (routing, observability) in shared waypoint proxies. The challenge is maintaining Istio's full feature set without a per-pod proxy.

**Approaches Compared:**

| Architecture | L4 mTLS | L7 Routing | Per-Pod Overhead | Operational Complexity |
|-------------|---------|-----------|-----------------|----------------------|
| Sidecar model | Pod-local Envoy | Pod-local Envoy | ~50 MB RAM, ~0.5 ms | Medium (injection, upgrades) |
| Ambient mesh | ztunnel (per-node) | Waypoint proxy (per-namespace or per-service) | ~0 (shared) | Lower (no injection) |
| eBPF mesh (Cilium) | In-kernel eBPF | Limited L7 | ~0 | Low |
| No mesh | Application-managed | Application-managed | 0 | Very high (developer burden) |

**Selected: Ambient mesh (Istio 1.22+, GA path) as the future direction; sidecar model for immediate production**

**Ambient Mesh Architecture:**

```
                    ┌─────────────────────────────────────────┐
                    │                  Node                    │
                    │                                         │
                    │  ┌─────────────────────────────────┐   │
                    │  │         ztunnel (per-node)       │   │
                    │  │  DaemonSet: runs on every node  │   │
                    │  │                                  │   │
                    │  │  Responsibilities:               │   │
                    │  │  - mTLS encryption/decryption    │   │
                    │  │  - L4 authorization              │   │
                    │  │  - Transparent proxy (redirect)  │   │
                    │  │  - HBONE tunneling               │   │
                    │  │                                  │   │
                    │  │  Performance: ~0.1-0.3ms latency │   │
                    │  │  Memory: ~20 MB per node (shared)│   │
                    │  └─────────────────────────────────┘   │
                    │                                         │
                    │  ┌──────────┐  ┌──────────┐           │
                    │  │ Pod A    │  │ Pod B    │           │
                    │  │ (no      │  │ (no      │           │
                    │  │  sidecar)│  │  sidecar)│           │
                    │  └──────────┘  └──────────┘           │
                    └─────────────────────────────────────────┘

                    ┌─────────────────────────────────────────┐
                    │    Waypoint Proxy (per-namespace)       │
                    │    Deployment: Envoy-based              │
                    │                                         │
                    │    Responsibilities:                    │
                    │    - L7 traffic management              │
                    │    - L7 authorization                   │
                    │    - Observability (metrics, tracing)   │
                    │    - Retries, circuit breaking          │
                    │                                         │
                    │    Only deployed when L7 features needed│
                    │    Scales horizontally (HPA)            │
                    └─────────────────────────────────────────┘

Traffic Flow (Ambient):
  
  L4 only (default):
    Pod A → ztunnel (src node) → HBONE tunnel → ztunnel (dst node) → Pod B
    Latency: ~0.1-0.3ms overhead
    Features: mTLS, L4 authz, basic load balancing
    
  L7 (when waypoint proxy is deployed):
    Pod A → ztunnel (src) → waypoint proxy → ztunnel (dst) → Pod B
    Latency: ~0.5-1ms overhead (extra hop to waypoint)
    Features: full L7 routing, headers, retries, metrics, tracing

Opt-in per namespace:
  kubectl label namespace production istio.io/dataplane-mode=ambient
  
  This redirects all traffic in the namespace through ztunnel.
  No sidecar injection needed.
  
  For L7 features, deploy a waypoint proxy:
  istioctl x waypoint apply --namespace production
```

**Sidecar vs. Ambient Resource Comparison:**

| Metric | Sidecar (25,000 pods) | Ambient (25,000 pods, 500 nodes) |
|--------|----------------------|----------------------------------|
| Proxy instances | 25,000 Envoy sidecars | 500 ztunnel + ~50 waypoint proxies |
| RAM overhead | 25,000 x 50 MB = 1.25 TB | 500 x 20 MB + 50 x 100 MB = 15 GB |
| CPU overhead | 25,000 x 0.1 CPU = 2,500 CPU | 500 x 0.1 + 50 x 0.5 = 75 CPU |
| Latency (L4) | ~0.5ms | ~0.2ms |
| Latency (L7) | ~0.5ms | ~0.8ms (extra hop) |
| Proxy upgrades | Rolling restart of all 25K pods | Restart 500 ztunnel DaemonSet + 50 waypoints |
| Sidecar injection | Required (webhook) | Not needed |

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| ztunnel crash on a node | All pods on that node lose mTLS and mesh features | DaemonSet auto-restart; pods continue running (may fall back to plaintext) |
| Waypoint proxy overloaded | L7 features degraded for the namespace | HPA on waypoint proxy; multiple waypoints per namespace |
| ztunnel upgrade (DaemonSet rolling update) | Brief interruption per node during restart | Rolling update one node at a time; pods handle temporary plaintext gracefully in PERMISSIVE mode |

**Interviewer Q&As:**

**Q1: What is the main advantage of ambient mesh over sidecar mesh?**
A: Resource efficiency and operational simplicity. With 25,000 pods, sidecar mesh consumes 1.25 TB of RAM just for proxies. Ambient mesh reduces this to ~15 GB (98.8% reduction). Operationally: no sidecar injection webhook (eliminates injection failures), no per-pod proxy upgrades (upgrade 500 ztunnel DaemonSet pods, not 25,000 sidecar pods), no sidecar lifecycle management (startup ordering, resource allocation). Trade-off: L7 features require an extra network hop through the waypoint proxy, adding ~0.3ms latency.

**Q2: When would you still choose sidecar over ambient mesh?**
A: (1) When L7 features are needed on every request and the extra waypoint hop latency is unacceptable. (2) When per-pod security isolation is required (sidecar = per-pod proxy; ztunnel = shared per-node). (3) When running on older Istio versions without ambient mesh support. (4) When the platform has existing investment in sidecar-based tooling (logging, debugging). (5) Ambient mesh is still maturing (GA path but some features in beta) — sidecar is battle-tested.

---

### Deep Dive 4: Observability

**Why it's hard:**
The service mesh observes every request without application instrumentation. At 2.5M RPS, this generates enormous volumes of metrics, traces, and logs. The challenge is: collecting data efficiently (don't overwhelm proxies or collectors), providing useful aggregations (not just raw data), and correlating across services (distributed tracing).

**Implementation Detail:**

```
Automatic Metrics (no application changes):

  Envoy generates Prometheus metrics for every request:
  
  istio_requests_total{
    reporter="source",               # or "destination"
    source_workload="frontend",
    source_workload_namespace="production",
    destination_workload="reviews",
    destination_workload_namespace="production",
    destination_service="reviews.production.svc.cluster.local",
    response_code="200",
    request_protocol="http",
    connection_security_policy="mutual_tls"
  }
  
  istio_request_duration_milliseconds_bucket{...}  # Histogram
  istio_request_bytes_bucket{...}                  # Request size
  istio_response_bytes_bucket{...}                 # Response size
  
  TCP metrics:
  istio_tcp_connections_opened_total{...}
  istio_tcp_connections_closed_total{...}
  istio_tcp_sent_bytes_total{...}
  istio_tcp_received_bytes_total{...}

Golden Signals from Mesh Metrics:

  Latency (p50, p90, p99):
    histogram_quantile(0.99, 
      sum(rate(istio_request_duration_milliseconds_bucket{...}[5m]))
      by (le, destination_workload))
  
  Traffic (RPS):
    sum(rate(istio_requests_total{...}[5m])) by (destination_workload)
  
  Errors (error rate):
    sum(rate(istio_requests_total{response_code=~"5.*"}[5m]))
    / sum(rate(istio_requests_total[5m]))
  
  Saturation:
    max(envoy_server_total_connections) / max(envoy_server_concurrency)

Distributed Tracing:

  Envoy automatically generates trace spans:
  - Span per request (inbound + outbound)
  - Propagates trace context headers (W3C Trace Context, B3, Zipkin)
  - Sends spans to tracing collector (Jaeger, Zipkin, Tempo)
  
  Important: application must propagate trace headers
  (Envoy generates spans but cannot propagate across application logic)
  
  Headers to propagate:
  - traceparent / tracestate (W3C)
  - x-request-id (Envoy)
  - x-b3-traceid, x-b3-spanid, x-b3-parentspanid (B3)
  
  Sampling:
  - Default: 1% (configurable via MeshConfig.defaultConfig.tracing.sampling)
  - At 2.5M RPS, 1% = 25,000 traces/s — still significant storage
  - Use adaptive sampling: higher rate for errors, lower for success
  - Tail-based sampling: collect all spans, then decide which traces to keep

Access Logs:

  Envoy access logs (configurable format):
  [2026-04-10T15:30:00.000Z] "GET /api/v1/reviews/42 HTTP/2" 200 - 
  via_upstream - "-" 0 1234 15 12 
  "10.244.1.5:80" "reviews.production.svc.cluster.local"
  "frontend-abc123" "reviews-v2-xyz789"
  outbound|9080||reviews.production.svc.cluster.local
  
  Fields: timestamp, method, path, status, upstream_host, response_size,
          request_duration_ms, upstream_response_time_ms,
          source_pod, destination_pod, route
  
  Access logs can be:
  - Written to stdout (collected by Fluentd/Vector)
  - Sent to OpenTelemetry collector
  - Disabled for high-traffic services (reduce I/O)
```

**Interviewer Q&As:**

**Q1: How do you manage the storage cost of mesh observability at 2.5M RPS?**
A: (1) **Metrics**: use Istio's metric filtering — reduce cardinality by dropping unnecessary labels (source/destination IP, request ID). Use Prometheus recording rules to pre-aggregate. Retention: 15 days for raw, 1 year for aggregated. (2) **Traces**: sample at 1% (25K traces/s). Use tail-based sampling (keep 100% of error traces, 0.1% of success). Storage: Tempo with S3 backend (cheap). (3) **Logs**: disable access logs for high-volume healthy services. Enable only for debugging or for critical paths. (4) **Cost estimate**: ~300 GB/day for metrics + 10 GB/day for traces (1% sampling) + access logs as needed.

**Q2: How do you trace a request across a service mesh with 10 hops?**
A: (1) The first Envoy proxy (ingress gateway) generates a trace ID and request ID. (2) Each subsequent Envoy proxy creates a child span with the same trace ID. (3) The application at each hop must propagate the trace context headers (this is the one requirement). (4) All spans are sent to the tracing collector (Jaeger/Tempo). (5) The collector assembles spans into a trace tree based on trace ID + parent span ID. (6) Result: a waterfall view showing latency at each hop, which service is slow, where errors occur. (7) Key tool: Kiali (service mesh visualization) shows the service graph with real-time traffic flow.

---

## 7. Scheduling & Resource Management

### Service Mesh Resource Overhead

```
Sidecar Mode Resource Budget:

  Per sidecar (Envoy):
    CPU request:  100m (idle), 500m-2000m (under load)
    Memory:       50-100 MB (depends on config size)
    Disk:         ~0 (logs to stdout)
  
  Control plane (istiod):
    CPU:          500m-2000m (depends on config push rate)
    Memory:       1-4 GB (depends on number of proxies)
    Replicas:     3 (HA)
  
  Total overhead for 25,000 pod cluster:
    Sidecars:     25,000 x 100m CPU, 50 MB = 2,500 CPU cores reserved, 1.25 TB RAM
    Control:      3 x 2 CPU, 4 GB = 6 CPU, 12 GB
    Total:        ~2,506 CPU cores, ~1.26 TB RAM
    
    This is significant — typically 15-20% of cluster capacity

Ambient Mode Resource Budget:

  ztunnel (per node):
    CPU:          50-100m
    Memory:       20-50 MB
  
  Waypoint proxy (per namespace with L7):
    CPU:          200m-1000m
    Memory:       100-256 MB
  
  Total for same 25K pod cluster (500 nodes, 50 L7 namespaces):
    ztunnel:      500 x 100m, 50 MB = 50 CPU, 25 GB
    Waypoints:    50 x 500m, 200 MB = 25 CPU, 10 GB  
    Control:      3 x 2 CPU, 4 GB = 6 CPU, 12 GB
    Total:        ~81 CPU, ~47 GB
    
    ~97% reduction from sidecar model
```

---

## 8. Scaling Strategy

### Scaling the Service Mesh

| Component | Scaling Method | Limit |
|-----------|---------------|-------|
| istiod | Horizontal (add replicas) | 3-5 replicas for 100K+ proxies |
| Envoy sidecar | Scales with pod count | Per-pod resource overhead |
| ztunnel | DaemonSet (scales with nodes) | Per-node resource overhead |
| Waypoint proxy | Horizontal (HPA) | Scale based on L7 traffic |
| Ingress gateway | Horizontal (HPA) | Scale based on external traffic |
| Prometheus (mesh metrics) | Thanos/Cortex for horizontal | Cardinality management critical |

### Interviewer Q&As

**Q1: How does istiod scale to manage 100,000 proxies?**
A: (1) istiod is horizontally scalable — multiple replicas share the load. (2) xDS connections are distributed across istiod replicas. (3) Incremental xDS pushes: only send changed configuration, not full state. (4) Configuration scoping: use `Sidecar` resource to limit which services each proxy knows about (reduces config size from 5,000 services to ~50 per proxy). (5) Memory optimization: istiod deduplicates xDS configs (proxies with identical config share the same config object). (6) Google runs Istio at this scale in GKE Autopilot.

**Q2: How do you reduce Envoy sidecar memory usage?**
A: (1) **Sidecar resource**: restrict the set of services visible to each proxy. By default, every proxy gets config for every service in the mesh. With `Sidecar` resource: only egress services actually called are configured. This reduces per-proxy config from ~500 KB to ~20 KB. (2) **Disable unused features**: turn off access logging, reduce tracing sampling, disable unused filters. (3) **Tune connection pools**: reduce max connections per destination. (4) **Wasm plugin size**: minimize custom Envoy filters. (5) These optimizations can reduce per-proxy memory from 100 MB to 30-40 MB.

**Q3: How do you handle service mesh upgrades at scale?**
A: (1) **Control plane first**: upgrade istiod (rolling update of Deployment). New istiod serves both old and new proxy versions. (2) **Data plane (sidecar)**: rolling restart of all pods to pick up new sidecar version. Use canary: tag specific namespaces with new istio version first. (3) **Ambient mesh**: upgrade ztunnel DaemonSet (rolling, one node at a time). Much simpler — 500 nodes vs. 25,000 pods. (4) **Revision-based upgrades (Istio canary)**: deploy new istiod as a separate revision (`istio-1-21`). Migrate namespaces one at a time by changing the `istio.io/rev` label. Old and new revisions coexist.

**Q4: How do you limit the blast radius of a mesh configuration error?**
A: (1) **Namespace scoping**: VirtualService and DestinationRule in a namespace only affect that namespace's traffic (unless `exportTo` is set). (2) **`istioctl analyze`**: catches configuration errors before applying. (3) **Dry-run**: apply config in a staging mesh first. (4) **Canary**: apply config to one service first (e.g., 10% traffic), monitor, then roll out. (5) **Revision-based**: use Istio revisions to test new control plane config without affecting production data plane.

**Q5: How do you implement a multi-cluster service mesh?**
A: (1) **Primary-Remote**: one primary cluster runs istiod, remote clusters connect to it. Simple but single point of failure. (2) **Multi-Primary**: each cluster runs its own istiod, synced via cross-cluster service discovery. More resilient. (3) **East-West Gateway**: Envoy gateway in each cluster handles cross-cluster mTLS traffic. (4) **Trust**: shared root CA or trust domain federation. (5) **Service discovery**: istiod watches remote cluster's API server for endpoints. (6) **Traffic routing**: VirtualService can route between clusters based on locality (prefer local cluster, failover to remote).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | istiod crash (all replicas) | No config updates; no new certs; existing proxies continue with cached config | istiod pod health check | HA deployment (3 replicas); last-known-good config cached in proxies | < 30s |
| 2 | Envoy sidecar crash | Pod's traffic blackholed | Pod health check fails; readiness probe | kubelet restarts sidecar container | < 10s |
| 3 | xDS push storm (large config change) | All proxies receive config simultaneously; CPU spike | istiod CPU/memory spike | Rate-limited pushes; incremental xDS | Self-recovering |
| 4 | Certificate expiry (istiod CA down for > 24h) | mTLS connections start failing | Cert expiry metrics | Restart istiod; extend cert TTL; manual cert rotation | < 5 min |
| 5 | Misconfigured VirtualService | Traffic routed incorrectly | Error rate spike; Kiali visualization | Revert config; `istioctl analyze` | < 1 min |
| 6 | Sidecar injection webhook down | New pods deployed without sidecar | Pods running without mesh | HA webhook; `istioctl analyze` detects | < 1 min |
| 7 | Ingress gateway crash | External traffic blocked | LB health check | HPA + multiple replicas; LB failover | < 30s |
| 8 | ztunnel crash (ambient) | Node's mesh traffic disrupted | DaemonSet health; node-level monitoring | DaemonSet auto-restart | < 10s |

### Automated Recovery

| Mechanism | Implementation |
|-----------|---------------|
| istiod HA | 3 replicas; leader election for CA; all replicas serve xDS |
| Proxy retry on xDS disconnect | Envoy reconnects to istiod with exponential backoff |
| Certificate auto-rotation | SDS renews at 80% of TTL; istiod auto-signs |
| Ingress gateway HPA | Auto-scale based on connection count |
| Config validation | `istioctl analyze` in CI/CD pipeline before apply |
| Canary deployment | Istio revisions for control plane; VirtualService for data plane |

---

## 10. Observability

### Key Metrics

| Metric | Source | Alert Threshold | Meaning |
|--------|--------|----------------|---------|
| `istio_requests_total{response_code=~"5.*"}` | Envoy | Error rate > 1% | Service errors |
| `istio_request_duration_milliseconds_bucket` | Envoy | p99 > SLO | Latency breach |
| `pilot_xds_pushes{type="error"}` | istiod | > 0 | Config push failures |
| `pilot_proxy_convergence_time` | istiod | p99 > 10s | Slow config propagation |
| `citadel_server_csr_count` | istiod | Spike | Mass certificate rotation |
| `envoy_server_total_connections` | Envoy | > 10,000 per proxy | Connection pool exhaustion |
| `istio_tcp_connections_closed_total{reason="overflow"}` | Envoy | > 0 | Circuit breaker tripped |
| `pilot_xds_expired_nonce` | istiod | > 0 | Proxy has stale config |
| `envoy_cluster_upstream_rq_pending_overflow` | Envoy | > 0 | Connection pool full |
| `galley_validation_failed` | istiod | > 0 | Invalid Istio config submitted |

### Distributed Tracing

```
Tracing architecture:

  App → Envoy sidecar → [generates span] → OTel Collector → Jaeger/Tempo
  
  Trace context propagation (application responsibility):
  - Incoming request has: traceparent header
  - Application must copy these headers to all outgoing requests
  - Envoy adds its own spans automatically
  
  Example trace (3-service chain):
  
  Trace ID: abc123
  ├── Span 1: ingress-gateway (2ms)
  │   └── Span 2: frontend (outbound) (1ms)
  │       └── Span 3: reviews (inbound) (0.5ms)
  │           └── Span 4: reviews (processing) (15ms)
  │               └── Span 5: reviews (outbound to ratings) (0.5ms)
  │                   └── Span 6: ratings (inbound) (0.5ms)
  │                       └── Span 7: ratings (processing) (5ms)
  │
  Total: 24.5ms
  Envoy overhead: 2.5ms (spans 2, 3, 5, 6)
  Application: 22ms (spans 4, 7)
```

### Logging

```
Envoy access log format (configurable):

  Default format:
  [%START_TIME%] "%REQ(:METHOD)% %REQ(X-ENVOY-ORIGINAL-PATH?:PATH)% %PROTOCOL%"
  %RESPONSE_CODE% %RESPONSE_FLAGS% %BYTES_RECEIVED% %BYTES_SENT%
  %DURATION% %RESP(X-ENVOY-UPSTREAM-SERVICE-TIME)%
  "%REQ(X-FORWARDED-FOR)%" "%REQ(USER-AGENT)%"
  "%REQ(X-REQUEST-ID)%" "%REQ(:AUTHORITY)%"
  "%UPSTREAM_HOST%" %UPSTREAM_CLUSTER%
  
  Structured JSON format (recommended):
  meshConfig:
    accessLogFile: /dev/stdout
    accessLogEncoding: JSON
    accessLogFormat: |
      {
        "timestamp": "%START_TIME%",
        "method": "%REQ(:METHOD)%",
        "path": "%REQ(X-ENVOY-ORIGINAL-PATH?:PATH)%",
        "status": "%RESPONSE_CODE%",
        "duration_ms": "%DURATION%",
        "upstream_host": "%UPSTREAM_HOST%",
        "source": "%DOWNSTREAM_REMOTE_ADDRESS%",
        "request_id": "%REQ(X-REQUEST-ID)%",
        "trace_id": "%REQ(X-B3-TRACEID)%"
      }
```

---

## 11. Security

### Auth & AuthZ

**Zero-Trust Security Model:**

```
Layer 1: Transport Security (mTLS)
  - All service-to-service communication encrypted
  - Workload identity via SPIFFE certificates
  - No plaintext within the mesh (STRICT mode)

Layer 2: Authentication (PeerAuthentication + RequestAuthentication)
  - PeerAuthentication: verify service identity (mTLS cert)
  - RequestAuthentication: verify end-user identity (JWT)
  
  apiVersion: security.istio.io/v1beta1
  kind: RequestAuthentication
  metadata:
    name: jwt-auth
    namespace: production
  spec:
    selector:
      matchLabels:
        app: reviews
    jwtRules:
      - issuer: "https://auth.company.com"
        jwksUri: "https://auth.company.com/.well-known/jwks.json"
        outputPayloadToHeader: "x-jwt-payload"

Layer 3: Authorization (AuthorizationPolicy)
  - L4: source principal (SPIFFE ID), namespace, IP
  - L7: HTTP method, path, headers, JWT claims
  
  Example: Only frontend service can GET reviews; admin can do anything:
  
  apiVersion: security.istio.io/v1beta1
  kind: AuthorizationPolicy
  metadata:
    name: reviews-policy
  spec:
    selector:
      matchLabels:
        app: reviews
    action: ALLOW
    rules:
      - from:
          - source:
              principals: ["cluster.local/ns/production/sa/frontend"]
        to:
          - operation:
              methods: ["GET"]
      - from:
          - source:
              principals: ["cluster.local/ns/production/sa/admin"]

Layer 4: Egress Control
  - Default: deny all egress (outboundTrafficPolicy: REGISTRY_ONLY)
  - Explicitly allow external services via ServiceEntry
  - Route through egress gateway for audit and policy enforcement
```

### Multi-tenancy Isolation

| Mechanism | Isolation |
|-----------|-----------|
| Namespace-scoped Istio config | VirtualService/DestinationRule only affects own namespace |
| AuthorizationPolicy | Per-namespace access control |
| PeerAuthentication | Per-namespace mTLS mode |
| Sidecar resource | Limit egress visibility per namespace |
| Ingress gateway per tenant | Separate gateway deployments per team |

---

## 12. Incremental Rollout Strategy

### Phase 1: Foundation (Week 1-2)
- Install istiod (3 replicas, HA).
- Enable sidecar injection for one pilot namespace.
- Verify mTLS in PERMISSIVE mode.
- Deploy Kiali + Jaeger for visualization.

### Phase 2: Observability (Week 3-4)
- Enable metrics collection for all meshed services.
- Configure Grafana dashboards (golden signals per service).
- Enable tracing at 1% sampling.
- Configure access logging for critical services.

### Phase 3: Traffic Management (Week 5-6)
- Implement canary deployment for one service (VirtualService).
- Configure retries and timeouts (DestinationRule).
- Test circuit breaking with chaos injection.
- Deploy ingress gateway for external traffic.

### Phase 4: Security (Week 7-8)
- Enable STRICT mTLS globally.
- Deploy AuthorizationPolicies (start with AUDIT mode).
- Configure egress policies (REGISTRY_ONLY).
- Enable RequestAuthentication for user-facing services.

### Phase 5: Scale (Week 9-10)
- Mesh all production namespaces.
- Tune Envoy resources per service (Sidecar resource for config scoping).
- Configure multi-cluster mesh if needed.
- Evaluate ambient mesh for high-density namespaces.

### Rollout Interviewer Q&As

**Q1: How do you roll out a service mesh to an existing cluster without disruption?**
A: (1) Install istiod without enabling injection. (2) Enable injection for one non-critical namespace (`istio-injection=enabled`). (3) Rolling restart pods in that namespace to get sidecars. (4) Verify: traffic flows, mTLS works (PERMISSIVE mode), metrics collected. (5) Gradually enable injection for more namespaces. (6) Switch to STRICT mTLS only after all communicating services have sidecars. (7) Total rollout: 4-6 weeks for a large platform.

**Q2: How do you handle services that break when sidecars are injected?**
A: Common issues: (1) Application binds to localhost but sidecar redirects traffic — fix: bind to 0.0.0.0. (2) Application does not handle connection reset (sidecar restart during rolling update) — fix: add retry logic. (3) Init containers need network before sidecar is ready — fix: use `holdApplicationUntilProxyStarts: true`. (4) gRPC services with custom load balancing — fix: configure DestinationRule to disable Istio load balancing for these services. (5) Always test in staging before production.

**Q3: How do you handle the mesh during a Kubernetes upgrade?**
A: (1) Istio has a strict compatibility matrix with Kubernetes versions. (2) Upgrade Kubernetes first (Istio supports current and previous k8s versions). (3) Then upgrade Istio. (4) Or: upgrade Istio first if the current version supports the target k8s version. (5) Use Istio revision-based upgrades: deploy new istiod version as a separate revision, migrate namespaces gradually.

**Q4: How do you debug a 503 error in a service mesh?**
A: (1) Check Envoy access logs: look at response_flags. Common flags: `UC` (upstream connection failure), `UO` (upstream overflow — circuit breaker), `NR` (no route found), `UF` (upstream connection failure). (2) Check if mTLS is the issue: `istioctl x authz check <pod>`. (3) Check routing: `istioctl proxy-config routes <pod>`. (4) Check endpoints: `istioctl proxy-config endpoints <pod>`. (5) Check Kiali: is the service graph showing errors? (6) Check Prometheus: `istio_requests_total{response_code="503"}` by source and destination.

**Q5: How do you migrate from sidecar to ambient mesh?**
A: (1) Upgrade to Istio 1.22+ (ambient GA support). (2) Enable ambient mode for a test namespace: `kubectl label ns test istio.io/dataplane-mode=ambient`. (3) Remove sidecar injection label. (4) Rolling restart pods (sidecars will be removed). (5) ztunnel handles L4 (mTLS) automatically. (6) Deploy waypoint proxy for namespaces needing L7 features. (7) Verify functionality: mTLS, routing, observability. (8) Gradual migration: one namespace at a time.

---

## 13. Trade-offs & Decision Log

| # | Decision | Alternative Considered | Trade-off | Rationale |
|---|----------|----------------------|-----------|-----------|
| 1 | Istio over Linkerd | Linkerd (simpler, lower resource) | More complex but richer feature set | Istio has broader ecosystem, multi-cluster support, L7 auth policies |
| 2 | Sidecar model (current) with ambient migration path | Ambient mesh immediately | Battle-tested vs. newer technology | Start with sidecar for production stability; plan ambient for 2027 |
| 3 | SPIFFE identity over IP-based | IP-based ACLs | More complex identity model but survives pod restarts | Zero-trust requires workload identity, not network identity |
| 4 | Envoy over custom proxy | Custom L7 proxy | Less customizable but massive community and ecosystem | Envoy is CNCF graduated, battle-tested at Google/Lyft/Uber scale |
| 5 | 1% trace sampling | 100% sampling | Incomplete traces but manageable storage | 100% at 2.5M RPS = 6.5 TB/day; tail-based sampling catches errors at 100% |
| 6 | STRICT mTLS globally | PERMISSIVE (opt-in) | Breaks non-mesh services but strongest security | Zero-trust requires all traffic encrypted; exceptions via DestinationRule |
| 7 | Istiod CA over external CA | Vault PKI, AWS ACM PCA | Simpler but less enterprise features | Sufficient for most clusters; pluggable CA for compliance environments |
| 8 | Kiali for visualization | Custom dashboard | Dependency on Kiali project but excellent UX | Kiali is purpose-built for Istio; real-time service graph |
| 9 | Ingress gateway (Envoy) over NGINX | NGINX ingress controller | Different config model but unified mesh observability | Consistent mTLS from edge to service; same Envoy config language |

---

## 14. Agentic AI Integration

### AI-Powered Service Mesh Operations

| Use Case | AI Agent Capability | Implementation |
|----------|-------------------|---------------|
| **Automated canary analysis** | Analyze canary metrics and decide promote/rollback | Agent monitors error rate, latency p99, throughput for canary subset; auto-adjusts VirtualService weights |
| **Anomaly detection** | Detect unusual traffic patterns or latency spikes | Agent monitors `istio_requests_total` and `istio_request_duration_milliseconds`; flags anomalies in service graph |
| **Root cause analysis** | Trace error propagation across service chain | Agent follows trace data: which service first returned 5xx? What changed recently (deploy, config)? |
| **Policy generation** | Generate AuthorizationPolicies from observed traffic | Agent observes 7 days of service-to-service traffic in Kiali; generates least-privilege AuthorizationPolicies |
| **Configuration optimization** | Tune connection pools, circuit breakers, retries | Agent analyzes: connection reuse rate, circuit breaker trip frequency, retry success rate; adjusts DestinationRules |
| **Capacity planning** | Predict when mesh infrastructure needs scaling | Agent analyzes RPS growth trends; predicts when istiod/gateway/waypoint need more replicas |

### Example: AI-Driven Service Mesh Troubleshooter

```python
class MeshTroubleshooter:
    """
    Diagnoses service mesh issues using metrics, traces, and configuration.
    """
    
    def diagnose_high_error_rate(self, service: str, namespace: str) -> Diagnosis:
        # Step 1: Identify error pattern
        error_rate = self.prometheus.query(
            f'sum(rate(istio_requests_total{{destination_workload="{service}",'
            f'namespace="{namespace}",response_code=~"5.*"}}[5m]))'
            f'/ sum(rate(istio_requests_total{{destination_workload="{service}",'
            f'namespace="{namespace}"}}[5m]))'
        )
        
        # Step 2: Check if errors are from all sources or specific ones
        error_by_source = self.prometheus.query(
            f'sum by (source_workload) (rate(istio_requests_total{{'
            f'destination_workload="{service}",response_code=~"5.*"}}[5m]))'
        )
        
        # Step 3: Check response flags (Envoy-specific error reasons)
        response_flags = self.prometheus.query(
            f'sum by (response_flags) (rate(istio_requests_total{{'
            f'destination_workload="{service}",response_code=~"5.*"}}[5m]))'
        )
        
        # Step 4: Analyze root cause
        diagnosis = Diagnosis(service=service, error_rate=error_rate)
        
        for flag, rate in response_flags.items():
            if flag == 'UO':  # Upstream overflow
                diagnosis.add_finding(
                    cause='Circuit breaker triggered',
                    evidence=f'Response flag UO at {rate}/s',
                    remediation='Increase connectionPool.http.http2MaxRequests '
                               'in DestinationRule'
                )
            elif flag == 'UC':  # Upstream connection failure
                diagnosis.add_finding(
                    cause='Upstream pods unreachable',
                    evidence=f'Response flag UC at {rate}/s',
                    remediation='Check pod health, NetworkPolicy, '
                               'DNS resolution'
                )
            elif flag == 'NR':  # No route
                diagnosis.add_finding(
                    cause='Routing misconfiguration',
                    evidence=f'Response flag NR at {rate}/s',
                    remediation='Run istioctl analyze; check VirtualService '
                               'and DestinationRule for this service'
                )
        
        # Step 5: Check recent changes
        recent_deploys = self.k8s.list_recent_deployments(namespace, hours=1)
        recent_config = self.k8s.list_recent_istio_config(namespace, hours=1)
        
        if recent_deploys:
            diagnosis.add_context(f'Recent deployments: {recent_deploys}')
        if recent_config:
            diagnosis.add_context(f'Recent Istio config changes: {recent_config}')
        
        return diagnosis
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the data plane vs. control plane separation in a service mesh.**
A: The control plane (istiod) makes decisions: what routes to configure, what policies to enforce, what certificates to issue. The data plane (Envoy proxies) executes those decisions: routing traffic, enforcing mTLS, collecting metrics. This separation means: (1) Control plane can be upgraded independently. (2) Data plane continues operating if control plane is temporarily down (cached config). (3) Performance-critical path (data plane) is optimized separately from management logic (control plane). It's analogous to the SDN model: controller vs. switch.

**Q2: How does Envoy's xDS protocol work?**
A: xDS is a set of gRPC streaming APIs for dynamic configuration. Envoy connects to istiod and subscribes to configuration streams: LDS (listeners), RDS (routes), CDS (clusters), EDS (endpoints), SDS (secrets). Istiod pushes configuration changes in real-time — no Envoy restart needed. ADS (Aggregated Discovery Service) multiplexes all xDS types over a single gRPC stream, ensuring consistent ordering. Each push includes a nonce; Envoy ACKs successful applies or NACKs failures (istiod retries on NACK).

**Q3: What happens to traffic when istiod is down?**
A: Existing Envoy proxies continue operating with their last-known-good configuration. All routing, mTLS, and policies remain in effect. However: (1) No new configuration changes can be pushed. (2) No new certificates can be issued (existing ones work until expiry). (3) New pods get sidecars but with stale config (may not have routes to recently created services). (4) Certificate rotation will fail if istiod is down longer than the cert TTL (default 24h). This is why HA istiod (3 replicas) is critical.

**Q4: How do you implement rate limiting with Istio?**
A: Two approaches: (1) **Local rate limiting** (per-proxy): configure Envoy's local rate limit filter via EnvoyFilter. Each proxy independently limits requests. Simple but not globally consistent. (2) **Global rate limiting** (shared rate limiter): deploy a rate limit service (envoy-ratelimit) backed by Redis. Configure Envoy to check with the rate limiter before forwarding. Accurate but adds latency (Redis round-trip). (3) **Istio-native** (Istio 1.22+): WasmPlugin for rate limiting without EnvoyFilter. (4) Best practice: use local rate limiting for protection against burst, global for precise per-user/per-tenant limits.

**Q5: How does Istio handle a service that has both mTLS and non-mTLS clients?**
A: Use `PeerAuthentication.mtls.mode: PERMISSIVE`. The Envoy proxy detects whether the incoming connection is TLS or plaintext and handles both. This is the default mode during mesh migration. Once all clients have sidecars: switch to `STRICT` mode (reject plaintext). You can also use port-level configuration: `portLevelMtls: 8080: {mode: DISABLE}` for specific ports that serve non-mesh clients.

**Q6: How does Istio's sidecar injection work under the hood?**
A: (1) istiod registers a MutatingWebhookConfiguration targeting pods with `istio-injection=enabled` namespace label. (2) When a pod is created, the API server calls the webhook. (3) The webhook adds: (a) istio-init container (runs iptables rules to redirect all traffic through Envoy), (b) istio-proxy container (Envoy sidecar), (c) volume mounts for certificates. (4) iptables rules: REDIRECT all inbound traffic to Envoy's inbound port (15006), REDIRECT all outbound traffic to Envoy's outbound port (15001). (5) Envoy handles the traffic and forwards to the application container on localhost.

**Q7: What is the purpose of the Sidecar CRD in Istio?**
A: The Sidecar CRD limits the scope of configuration visible to a proxy. Without it, every Envoy proxy receives routes/endpoints for all 5,000 services in the mesh — even if a service only communicates with 5 others. This wastes memory and increases xDS push latency. With Sidecar CRD: specify exactly which services a proxy can reach. Example: `egress: [{hosts: ["./reviews", "istio-system/*"]}]` — this proxy only gets config for reviews service and istio-system. Memory reduction: from ~500 KB to ~20 KB per proxy.

**Q8: How do you implement mutual authentication between services (not just encryption)?**
A: mTLS provides both: (1) Encryption: TLS encrypts all traffic. (2) Authentication: both client and server present certificates during handshake. The server verifies the client's SPIFFE identity, and vice versa. (3) Authorization: after authentication, AuthorizationPolicy checks if the authenticated identity is allowed to access the requested resource. Example: reviews service only accepts requests from frontend service's SPIFFE ID. Even if an attacker compromises a pod, they cannot impersonate frontend without its private key.

**Q9: How do you handle gRPC services in a service mesh?**
A: Istio natively supports gRPC: (1) Envoy understands gRPC (HTTP/2 + protobuf). (2) Routing: VirtualService can match on gRPC service/method names. (3) Load balancing: per-request LB for gRPC (not per-connection, which is critical for long-lived gRPC connections). (4) Retries: configure retryOn for gRPC status codes. (5) Metrics: istio_requests_total includes gRPC status. (6) Tracing: automatic span generation for gRPC calls. (7) Important: gRPC uses HTTP/2 with long-lived connections. Without mesh, all requests go to one backend (connection-level LB). Envoy provides request-level LB across all backends.

**Q10: What is the performance overhead of a service mesh in production?**
A: Measured overhead (sidecar model): (1) **Latency**: ~0.5ms p50, ~1ms p99 per hop (two sidecars involved: source outbound + destination inbound). (2) **CPU**: ~100m (0.1 CPU) per proxy idle, ~500m under load. (3) **Memory**: ~50 MB per proxy (configurable; reducible with Sidecar CRD). (4) **Throughput**: < 5% reduction with mTLS (AES-GCM is hardware-accelerated). (5) **Total cluster overhead**: ~15-20% of resources for sidecar proxies. Ambient mesh reduces this to ~2-3%.

**Q11: How do you implement fault injection for chaos testing?**
A: VirtualService fault injection: (1) **Delay**: inject latency into a percentage of requests: `fault.delay: {percentage: 10, fixedDelay: 5s}`. (2) **Abort**: return error codes for a percentage: `fault.abort: {percentage: 5, httpStatus: 503}`. (3) Target specific users: match on headers (`end-user: tester`). (4) This tests: circuit breaker configuration, retry behavior, timeout settings, graceful degradation. (5) Advantage over Chaos Mesh: no pod-level disruption — fault is injected at the proxy level, affecting only specific traffic.

**Q12: How does Istio handle DNS resolution in the mesh?**
A: Envoy sidecars intercept all outbound traffic via iptables. For HTTP services: Envoy uses the Host header for routing (not DNS). For non-HTTP (TCP): Envoy needs to know the destination — it uses the original destination IP (captured by iptables before redirect). Istio's DNS proxy (enabled by default): intercepts DNS queries from the application, resolves using Kubernetes CoreDNS, and can auto-allocate VIPs for ServiceEntry (external services without a ClusterIP). This enables Istio to route traffic to ServiceEntry hosts transparently.

**Q13: What is HBONE and why does ambient mesh use it?**
A: HBONE (HTTP-Based Overlay Network Encapsulation) is a tunneling protocol used by ambient mesh. Instead of iptables-based traffic interception (sidecar model), ztunnel uses HBONE to tunnel L4 traffic over HTTP/2 CONNECT. Benefits: (1) Works with any CNI (no iptables dependency). (2) Supports mTLS natively (HTTP/2 over TLS). (3) HTTP/2 multiplexing reduces connection overhead. (4) Compatible with load balancers that understand HTTP/2. Trade-off: slight overhead from HTTP/2 framing, but lower than iptables redirect + Envoy processing.

**Q14: How do you handle service mesh observability for a 10,000-service platform?**
A: (1) **Metrics cardinality management**: drop low-value labels (source/destination IP), keep high-value labels (workload, namespace, response code). Use Prometheus recording rules for pre-aggregation. (2) **Selective tracing**: 1% base rate, 100% for errors (tail-based sampling via OTel Collector). (3) **Access log sampling**: enable only for debugging sessions, not permanently. (4) **Kiali with read replicas**: Kiali can be CPU-intensive for large service graphs; use caching. (5) **Federated Prometheus**: use Thanos/Cortex for cross-cluster metric aggregation. (6) **Budget**: plan for ~300 GB/day for metrics, ~10 GB/day for traces at 10,000 services.

**Q15: How do you implement egress control (what traffic can leave the cluster)?**
A: (1) Set `outboundTrafficPolicy.mode: REGISTRY_ONLY` in MeshConfig. By default, all outbound traffic is blocked unless explicitly registered. (2) Register external services via ServiceEntry: `hosts: [api.stripe.com], ports: [{443, HTTPS}]`. (3) Route through egress gateway for auditing: VirtualService routes external traffic through the gateway. (4) The egress gateway can enforce: TLS origination, rate limiting, logging. (5) For DNS-based egress control: use Cilium's DNS-based network policies alongside Istio.

---

## 16. References

| # | Reference | URL |
|---|-----------|-----|
| 1 | Istio Documentation | https://istio.io/latest/docs/ |
| 2 | Envoy Proxy | https://www.envoyproxy.io/ |
| 3 | SPIFFE/SPIRE | https://spiffe.io/ |
| 4 | xDS Protocol | https://www.envoyproxy.io/docs/envoy/latest/api-docs/xds_protocol |
| 5 | Istio Ambient Mesh | https://istio.io/latest/docs/ambient/ |
| 6 | Kiali (Service Mesh Visualization) | https://kiali.io/ |
| 7 | Flagger (Canary Automation) | https://flagger.app/ |
| 8 | Linkerd (Alternative Mesh) | https://linkerd.io/ |
| 9 | Cilium Service Mesh | https://docs.cilium.io/en/stable/network/servicemesh/ |
| 10 | Istio Performance Benchmarks | https://istio.io/latest/docs/ops/deployment/performance-and-scalability/ |
| 11 | Envoy Filter Documentation | https://istio.io/latest/docs/reference/config/networking/envoy-filter/ |
| 12 | HBONE Specification | https://docs.google.com/document/d/1Gl2JtE5jnFdJlOLz5GjhMvlLxJXv0K3m8FEu8v3z0xA |
