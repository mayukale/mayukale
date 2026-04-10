# System Design: DNS at Scale

> **Relevance to role:** DNS is the foundation of service discovery in every cloud infrastructure platform. For a bare-metal IaaS / Kubernetes role, you must understand CoreDNS internals (pod DNS resolution flow, ndots, search domains), DNS-based service discovery limitations (TTL caching, staleness), headless services, ExternalDNS for dynamic record management, split-horizon DNS, and scaling DNS to handle millions of queries per second across thousands of services. DNS misconfigurations are one of the most common causes of production outages.

---

## 1. Requirement Clarifications

### Functional Requirements

| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Internal service DNS | Resolve `<service>.<namespace>.svc.cluster.local` to ClusterIP or pod IPs |
| FR-2 | External DNS resolution | Resolve external hostnames (e.g., `api.stripe.com`) via upstream recursive resolvers |
| FR-3 | Headless service DNS | Return all pod IPs directly (no ClusterIP) for client-side load balancing |
| FR-4 | SRV records | Return port information for services with named ports |
| FR-5 | ExternalName services | CNAME aliasing from Kubernetes service names to external DNS names |
| FR-6 | Dynamic DNS record management | Automatically sync Kubernetes Service/Ingress records to external DNS providers (Route53, Cloud DNS) |
| FR-7 | Split-horizon DNS | Return different answers for internal vs. external queries |
| FR-8 | DNS-based failover | Health-checked DNS records with automatic failover |
| FR-9 | Negative caching | Cache NXDOMAIN responses to reduce load |
| FR-10 | Custom DNS entries | Support custom DNS records for non-Kubernetes services |

### Non-Functional Requirements

| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Query latency (cache hit) | < 1 ms p99 |
| NFR-2 | Query latency (cache miss, internal) | < 5 ms p99 |
| NFR-3 | Query latency (cache miss, external) | < 50 ms p99 |
| NFR-4 | Query throughput per CoreDNS pod | 30,000+ QPS |
| NFR-5 | Availability | 99.999% (DNS failure = total service failure) |
| NFR-6 | Staleness (internal records) | < 5 seconds after endpoint change |
| NFR-7 | Record update propagation (external) | < 60 seconds to authoritative nameserver |

### Constraints & Assumptions

- Kubernetes 1.28+ with CoreDNS as the cluster DNS.
- 2,000 services, 20,000 pods, across 200+ nodes.
- All pods use the cluster DNS (configured via kubelet `--cluster-dns`).
- External DNS managed via Route53 (AWS) or Cloud DNS (GCP).
- Bare-metal infrastructure without cloud-native DNS resolvers.

### Out of Scope

- DNS protocol development (RFC compliance for exotic record types).
- DNSSEC implementation (can be added as a CoreDNS plugin).
- Domain registrar management.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|------------|-------|
| Total pods (DNS clients) | Given | 20,000 pods |
| DNS queries per pod per second | Typical for microservices (HTTP keep-alive reduces DNS) | 5-10 QPS avg, 50 QPS peak |
| Total cluster DNS QPS (avg) | 20,000 x 7.5 | 150,000 QPS |
| Total cluster DNS QPS (peak) | 20,000 x 50 | 1,000,000 QPS |
| External DNS queries (% of total) | ~20% of queries go to external domains | 30,000-200,000 QPS |
| Internal DNS queries | ~80% resolved by CoreDNS from K8s data | 120,000-800,000 QPS |
| Unique DNS names (internal) | 2,000 services + 20,000 pods (headless) | 22,000 records |
| Unique DNS names (external) | External dependencies | ~500 unique external domains |
| Search domain expansions | Default ndots=5 → 4 extra queries per name | 4-5x actual lookups (with dots < 5) |

### Latency Requirements

| Query Type | Target |
|------------|--------|
| Cache hit (CoreDNS in-memory) | < 0.5 ms p50, < 1 ms p99 |
| Internal miss (K8s plugin lookup) | < 2 ms p50, < 5 ms p99 |
| External miss (upstream resolver) | < 20 ms p50, < 50 ms p99 |
| NodeLocal DNSCache hit | < 0.2 ms p50, < 0.5 ms p99 |
| NXDOMAIN (negative cache hit) | < 0.5 ms p50, < 1 ms p99 |

### Storage Estimates

| Data | Calculation | Size |
|------|------------|------|
| CoreDNS in-memory cache | 22,000 internal + 500 external records x 256 bytes | ~6 MB |
| NodeLocal DNSCache per node | 10,000 cached records x 256 bytes | ~2.5 MB |
| DNS query logs (1 hour) | 150K QPS x 3600s x 100 bytes | ~54 GB/hour |
| External DNS zone files | 500 records x 256 bytes | negligible |

### Bandwidth Estimates

| Data | Calculation | Value |
|------|------------|-------|
| DNS query traffic (average) | 150K QPS x 512 bytes (UDP) | ~77 MB/s (615 Mbps) |
| DNS query traffic (peak) | 1M QPS x 512 bytes | ~512 MB/s (4 Gbps) |
| CoreDNS to K8s API (watch) | Endpoint changes, ~10/s x 1 KB | ~10 KB/s |
| External DNS sync | Every 60s, full zone reconciliation | negligible |

---

## 3. High Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                    KUBERNETES CLUSTER                              │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐ │
│  │                          NODE                                 │ │
│  │                                                                │ │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐                       │ │
│  │  │  Pod A   │  │  Pod B   │  │  Pod C   │                       │ │
│  │  │          │  │          │  │          │                       │ │
│  │  │ /etc/    │  │ /etc/    │  │ /etc/    │                       │ │
│  │  │ resolv.  │  │ resolv.  │  │ resolv.  │                       │ │
│  │  │ conf:    │  │ conf:    │  │ conf:    │                       │ │
│  │  │ nameserv │  │ nameserv │  │ nameserv │                       │ │
│  │  │ 169.254. │  │ 169.254. │  │ 169.254. │                       │ │
│  │  │ 25.10    │  │ 25.10    │  │ 25.10    │                       │ │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘                       │ │
│  │       │              │              │                             │ │
│  │       └──────────────┼──────────────┘                             │ │
│  │                      │ DNS queries (UDP/TCP :53)                  │ │
│  │                      ▼                                            │ │
│  │              ┌──────────────────┐                                  │ │
│  │              │ NodeLocal DNS    │ ← DaemonSet, link-local IP      │ │
│  │              │ Cache            │   169.254.25.10                  │ │
│  │              │ (CoreDNS)        │                                  │ │
│  │              │                  │                                  │ │
│  │              │ Cache: 10K       │                                  │ │
│  │              │ records, 30s TTL │                                  │ │
│  │              └────────┬─────────┘                                  │ │
│  │                       │ Cache miss                                 │ │
│  └───────────────────────┼───────────────────────────────────────────┘ │
│                          │                                              │
│                          ▼                                              │
│              ┌─────────────────────────────────────┐                   │
│              │       COREDNS DEPLOYMENT             │                   │
│              │       (kube-dns Service)              │                   │
│              │       ClusterIP: 10.96.0.10          │                   │
│              │                                       │                   │
│              │  ┌──────────┐  ┌──────────┐          │                   │
│              │  │ CoreDNS  │  │ CoreDNS  │  ...     │                   │
│              │  │ Pod 1    │  │ Pod 2    │          │                   │
│              │  │          │  │          │          │                   │
│              │  │ Plugins: │  │ Plugins: │          │                   │
│              │  │ -kubernetes│ │ -kubernetes│         │                   │
│              │  │ -forward  │  │ -forward  │          │                   │
│              │  │ -cache    │  │ -cache    │          │                   │
│              │  │ -log      │  │ -log      │          │                   │
│              │  │ -errors   │  │ -errors   │          │                   │
│              │  └─────┬────┘  └─────┬────┘          │                   │
│              │        │             │                │                   │
│              └────────┼─────────────┼────────────────┘                   │
│                       │             │                                    │
│         ┌─────────────┼─────────────┘                                    │
│         │             │                                                   │
│         ▼             ▼                                                   │
│  ┌──────────┐  ┌──────────────────────┐                                  │
│  │ K8s API  │  │ Upstream Recursive    │                                  │
│  │ Server   │  │ DNS Resolvers         │                                  │
│  │          │  │ (8.8.8.8, 1.1.1.1)   │                                  │
│  │ Services │  │ or internal resolvers │                                  │
│  │ Endpoints│  └──────────┬───────────┘                                  │
│  │ Pods     │              │                                              │
│  └──────────┘              ▼                                              │
│                    ┌──────────────────┐                                   │
│                    │  Authoritative   │                                   │
│                    │  Nameservers     │                                   │
│                    │  (Route53, etc.) │                                   │
│                    └──────────────────┘                                   │
│                                                                           │
│  ┌────────────────────────────────────────────────────────────────┐      │
│  │                  EXTERNAL DNS CONTROLLER                        │      │
│  │                                                                  │      │
│  │  Watches: Services (type=LoadBalancer), Ingress, Gateway API    │      │
│  │  Syncs to: Route53 / Cloud DNS / PowerDNS                       │      │
│  │  Creates: A/AAAA/CNAME records for exposed services             │      │
│  │                                                                  │      │
│  └────────────────────────────────────────────────────────────────┘      │
└──────────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| Pod resolv.conf | Points all DNS queries to the local node cache (169.254.25.10) or CoreDNS ClusterIP |
| NodeLocal DNSCache | Per-node DNS cache (DaemonSet) that reduces cross-network DNS queries and CoreDNS load |
| CoreDNS Deployment | Cluster DNS server; resolves internal names from K8s API, forwards external to upstream |
| Kubernetes API Server | Source of truth for Service, Endpoints, Pods used by CoreDNS `kubernetes` plugin |
| Upstream Resolvers | Recursive DNS resolvers for external domain resolution |
| ExternalDNS Controller | Synchronizes Kubernetes resources to external DNS providers |
| Authoritative Nameservers | Host external DNS zones (e.g., Route53 for `example.com`) |

### Data Flows

**Internal DNS resolution (Pod A calls svc-B.default.svc.cluster.local):**
1. Application in Pod A calls `getaddrinfo("svc-B")`.
2. glibc/musl reads `/etc/resolv.conf`: `search default.svc.cluster.local svc.cluster.local cluster.local`, `ndots:5`.
3. Since "svc-B" has 0 dots (< ndots=5), search domains are appended first.
4. Query: `svc-B.default.svc.cluster.local.` → NodeLocal DNSCache (169.254.25.10).
5. NodeLocal cache: cache miss → forward to CoreDNS ClusterIP (10.96.0.10).
6. CoreDNS `kubernetes` plugin: lookup Service `svc-B` in namespace `default`.
7. Returns A record: `svc-B.default.svc.cluster.local. 5 IN A 10.96.15.200` (ClusterIP).
8. NodeLocal cache stores result (TTL 30s).
9. Pod A receives the IP and connects.

**External DNS resolution (Pod A calls api.stripe.com):**
1. Application calls `getaddrinfo("api.stripe.com")`.
2. `ndots:5`: "api.stripe.com" has 2 dots (< 5), so search domains are tried first.
3. Queries: `api.stripe.com.default.svc.cluster.local.` → NXDOMAIN.
4. Then: `api.stripe.com.svc.cluster.local.` → NXDOMAIN.
5. Then: `api.stripe.com.cluster.local.` → NXDOMAIN.
6. Then: `api.stripe.com.` (FQDN) → CoreDNS `forward` plugin → upstream resolver → answer.
7. Total: 4 NXDOMAIN queries + 1 successful query = 5 queries for one resolution.
8. This is why `ndots` tuning matters at scale.

---

## 4. Data Model

### Core Entities & Schema

```
DNS Record Types in Kubernetes:

┌─────────────────────────────────────────────────────────────────────┐
│ Service Type          │ DNS Record                                   │
├───────────────────────┼──────────────────────────────────────────────┤
│ ClusterIP Service     │ svc.ns.svc.cluster.local. → ClusterIP (A)   │
│                       │                                              │
│ Headless Service      │ svc.ns.svc.cluster.local. → Pod IPs (A)     │
│ (clusterIP: None)     │ pod-ip.svc.ns.svc.cluster.local → Pod IP    │
│                       │ _port._proto.svc.ns... → SRV records         │
│                       │                                              │
│ ExternalName Service  │ svc.ns.svc.cluster.local. → CNAME to        │
│                       │   external.example.com                       │
│                       │                                              │
│ StatefulSet Pods      │ pod-0.svc.ns.svc.cluster.local. → Pod IP    │
│ (with headless svc)   │ pod-1.svc.ns.svc.cluster.local. → Pod IP    │
│                       │                                              │
│ Pod DNS (spec.hostname│ hostname.subdomain.ns.svc.cluster.local.     │
│  + spec.subdomain)    │   → Pod IP                                   │
└───────────────────────┴──────────────────────────────────────────────┘

CoreDNS Kubernetes Plugin Internal Data Structures:

  ServiceIndex: map[namespace/name] → {
      ClusterIP:  "10.96.15.200"
      Type:       "ClusterIP" | "Headless" | "ExternalName"
      Ports:      [{name: "http", port: 80, protocol: "TCP"}]
      ExternalName: "ext.example.com"  // for ExternalName type
  }

  EndpointIndex: map[namespace/name] → [{
      IP:         "10.244.1.15"
      NodeName:   "node-03"
      TargetRef:  {Kind: "Pod", Name: "svc-B-7f8d4-abc12"}
      Ports:      [{name: "http", port: 8080, protocol: "TCP"}]
      Ready:      true
      Hostname:   "pod-0"  // for StatefulSets
  }]

  PodIndex: map[podIP] → {
      Namespace: "default"
      Name:      "svc-B-7f8d4-abc12"
  }
```

```
External DNS Record Model (Route53):

  Zone: example.com.
  ┌──────────────────────────────────────────────────────────────┐
  │ Name                    │ Type  │ TTL   │ Value               │
  ├─────────────────────────┼───────┼───────┼─────────────────────┤
  │ api.example.com         │ A     │ 60    │ 203.0.113.10        │
  │                         │       │       │ 203.0.113.11        │
  │ api.example.com         │ AAAA  │ 60    │ 2001:db8::1         │
  │ *.staging.example.com   │ CNAME │ 300   │ staging-lb.example. │
  │ db.internal.example.com │ A     │ 30    │ 10.0.5.100          │
  │                         │       │       │ (split-horizon: internal only) │
  └──────────────────────────────────────────────────────────────┘
```

### Database Selection

| Store | Technology | Rationale |
|-------|-----------|-----------|
| Internal DNS records | Kubernetes API (in-memory informer cache in CoreDNS) | Real-time watch updates; no external DB needed |
| DNS cache (cluster) | CoreDNS in-process cache plugin | Low-latency, configurable TTL |
| DNS cache (node) | NodeLocal DNSCache (separate CoreDNS instance) | Reduces cross-network queries; survives CoreDNS pod restarts |
| External DNS zones | Route53 / Cloud DNS / PowerDNS | Managed authoritative DNS with API access |
| DNS query logs | Elasticsearch or ClickHouse | Full-text search, time-series analysis for debugging |

### Indexing Strategy

| Data | Index | Purpose |
|------|-------|---------|
| Services | namespace + name | O(1) service lookup by CoreDNS |
| Endpoints | service name + namespace | O(1) endpoint lookup for headless services |
| Cache entries | DNS name + record type | O(1) cache hit/miss |
| Zone records (external) | DNS name (trie/radix tree) | Efficient prefix matching for wildcards |

---

## 5. API Design

### Management APIs

```
# CoreDNS Configuration (Corefile)
# Configured via ConfigMap: kube-system/coredns

.:53 {
    errors                           # Log errors
    health {                         # Health check endpoint
        lameduck 5s                  # Wait 5s before shutdown
    }
    ready                            # Readiness endpoint (:8181/ready)
    
    kubernetes cluster.local in-addr.arpa ip6.arpa {
        pods insecure                # Enable pod DNS records
        fallthrough in-addr.arpa ip6.arpa
        ttl 30                       # TTL for internal records
    }
    
    prometheus :9153                  # Metrics endpoint
    
    forward . /etc/resolv.conf {     # Forward external to upstream
        max_concurrent 1000          # Max concurrent queries
        policy sequential            # Try resolvers in order
    }
    
    cache 30 {                       # Cache with 30s TTL
        success 9984 30              # Cache successful (max 9984 entries, 30s)
        denial 9984 5               # Cache NXDOMAIN (max 9984, 5s)
        prefetch 10 60s 10%         # Prefetch popular entries before expiry
    }
    
    loop                             # Detect forwarding loops
    reload                           # Auto-reload Corefile changes
    loadbalance                      # Round-robin A/AAAA records
}

# Split-horizon: internal zone
internal.example.com:53 {
    file /etc/coredns/internal.zone
    cache 60
}

# Stub domain: forward specific domain to custom resolver
consul.local:53 {
    forward . 10.0.5.50:8600        # Consul DNS interface
    cache 10
}

# ExternalDNS Controller Configuration (Kubernetes Deployment)
apiVersion: apps/v1
kind: Deployment
metadata:
  name: external-dns
spec:
  template:
    spec:
      containers:
        - name: external-dns
          image: registry.k8s.io/external-dns/external-dns:v0.14.0
          args:
            - --source=service                # Watch Services
            - --source=ingress                # Watch Ingresses
            - --provider=aws                  # Route53
            - --domain-filter=example.com     # Only manage this domain
            - --aws-zone-type=public          # Public hosted zone
            - --policy=upsert-only            # Create/update, never delete
            - --registry=txt                  # Use TXT records for ownership
            - --txt-owner-id=k8s-cluster-1    # Ownership identifier
            - --interval=60s                  # Sync interval
```

### Data Plane Behavior

```
CoreDNS Request Processing Pipeline:

  DNS Query arrives (UDP/TCP :53)
       │
       ▼
  ┌─────────────────────────────────────────────────────┐
  │  CoreDNS Plugin Chain (executed in Corefile order)   │
  │                                                       │
  │  1. errors plugin                                     │
  │     └─ Log errors from downstream plugins             │
  │                                                       │
  │  2. cache plugin                                      │
  │     ├─ Cache HIT → return cached response             │
  │     └─ Cache MISS → continue to next plugin           │
  │                                                       │
  │  3. kubernetes plugin                                 │
  │     ├─ Zone match? (cluster.local)                    │
  │     │   ├─ YES → lookup in K8s informer cache         │
  │     │   │   ├─ Service found → return A/SRV/CNAME     │
  │     │   │   └─ Not found → NXDOMAIN or fallthrough    │
  │     │   └─ NO → continue to next plugin               │
  │     └─ fallthrough → allow next plugin to handle      │
  │                                                       │
  │  4. forward plugin                                    │
  │     └─ Forward query to upstream resolver              │
  │        (8.8.8.8, 1.1.1.1, or /etc/resolv.conf)       │
  │                                                       │
  │  Response flows back through the chain:               │
  │  forward → kubernetes → cache (store) → errors → client│
  └─────────────────────────────────────────────────────┘

Pod DNS Resolution Detail (/etc/resolv.conf):

  # Generated by kubelet
  nameserver 169.254.25.10           # NodeLocal DNSCache (or 10.96.0.10 without it)
  search default.svc.cluster.local svc.cluster.local cluster.local
  options ndots:5

  Resolution of "svc-B":
    ndots=5: "svc-B" has 0 dots (< 5) → try search domains first
    
    Query 1: svc-B.default.svc.cluster.local. → A 10.96.15.200 ✓ (FOUND)
    
    Done! Only 1 query because the first search domain matched.

  Resolution of "api.stripe.com":
    ndots=5: "api.stripe.com" has 2 dots (< 5) → try search domains first
    
    Query 1: api.stripe.com.default.svc.cluster.local. → NXDOMAIN ✗
    Query 2: api.stripe.com.svc.cluster.local. → NXDOMAIN ✗
    Query 3: api.stripe.com.cluster.local. → NXDOMAIN ✗
    Query 4: api.stripe.com. → A 54.187.xxx.xxx ✓ (FOUND)
    
    4 queries! 3 wasted NXDOMAIN queries.
    
  Optimization: set ndots=2 or use FQDN "api.stripe.com." (trailing dot)
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: CoreDNS Architecture and Kubernetes Plugin

**Why it's hard:**
CoreDNS must serve 100K-1M QPS with sub-millisecond latency while staying synchronized with Kubernetes API state that changes continuously (pods scaling, endpoints updating, services being created). The Kubernetes plugin must efficiently translate DNS queries into lookups against an in-memory cache of the Kubernetes API, handling all the edge cases: headless services, StatefulSets, pod DNS, reverse DNS, SRV records.

**Approaches:**

| Approach | Performance | Consistency | Complexity | Maturity |
|----------|-------------|-------------|------------|----------|
| Direct K8s API query per DNS request | Very poor (1+ ms per query) | Perfect (real-time) | Low | N/A |
| Informer-based cache (watch + list) | Excellent (in-memory) | Eventual (seconds lag) | Medium | CoreDNS default |
| Pre-generated zone files | Excellent | Poor (batch update) | Low | Older systems |
| External database (etcd/Redis) | Good | Good | High | SkyDNS (deprecated) |

**Selected approach: Informer-based in-memory cache (CoreDNS kubernetes plugin)**

**Justification:** The informer pattern is the standard Kubernetes approach: initial LIST populates the cache, then WATCH provides real-time updates. This gives sub-millisecond query performance with seconds-level consistency.

**Implementation detail:**

```
CoreDNS Kubernetes Plugin Architecture:

  ┌─────────────────────────────────────────────────────┐
  │  CoreDNS Process                                     │
  │                                                       │
  │  ┌─────────────────────────────────────────────────┐ │
  │  │  Kubernetes Plugin                               │ │
  │  │                                                   │ │
  │  │  ┌─────────────────┐     ┌──────────────────┐   │ │
  │  │  │  API Watcher     │     │  DNS Handler     │   │ │
  │  │  │                  │     │                  │   │ │
  │  │  │  Informer:       │     │  ServeDNS():     │   │ │
  │  │  │  - Services      │────►│  1. Parse query  │   │ │
  │  │  │  - Endpoints     │     │  2. Match zone   │   │ │
  │  │  │  - EndpointSlices│     │  3. Lookup cache │   │ │
  │  │  │  - Pods (if pod  │     │  4. Build resp   │   │ │
  │  │  │    DNS enabled)  │     │  5. Return       │   │ │
  │  │  │                  │     │                  │   │ │
  │  │  └─────────────────┘     └──────────────────┘   │ │
  │  │          ▲                                        │ │
  │  │          │ Watch events                           │ │
  │  │          │                                        │ │
  │  └──────────┼────────────────────────────────────────┘ │
  │             │                                           │
  │  ┌──────────┼────────────────────────────────────────┐ │
  │  │          │   In-Memory Store                       │ │
  │  │          │                                         │ │
  │  │  ServiceMap:                                       │ │
  │  │    key: "default/svc-B"                            │ │
  │  │    val: {ClusterIP: "10.96.15.200",                │ │
  │  │          Type: ClusterIP,                          │ │
  │  │          Ports: [{http: 80}]}                      │ │
  │  │                                                    │ │
  │  │  EndpointMap:                                      │ │
  │  │    key: "default/svc-B"                            │ │
  │  │    val: [{IP: "10.244.1.15", Port: 8080,           │ │
  │  │           Ready: true, Hostname: ""},              │ │
  │  │          {IP: "10.244.2.23", Port: 8080,           │ │
  │  │           Ready: true, Hostname: ""}]              │ │
  │  │                                                    │ │
  │  │  PodMap (optional):                                │ │
  │  │    key: "10.244.1.15"                              │ │
  │  │    val: {Namespace: "default", Name: "..."}        │ │
  │  └────────────────────────────────────────────────────┘ │
  └─────────────────────────────────────────────────────────┘

Query Processing Example:

  Query: svc-B.default.svc.cluster.local. IN A
  
  1. Zone match: "cluster.local" matches kubernetes zone → handle
  2. Parse: service="svc-B", namespace="default"
  3. Lookup: ServiceMap["default/svc-B"] → found, ClusterIP
  4. Return: A record with ClusterIP 10.96.15.200, TTL=30

  Query: svc-B.default.svc.cluster.local. IN SRV
  
  1-2. Same as above
  3. Lookup: ServiceMap → found, Ports=[{http:80}]
  4. Lookup: EndpointMap["default/svc-B"] → 2 endpoints
  5. Return: SRV records:
       _http._tcp.svc-B.default.svc.cluster.local. 30 IN SRV 0 50 8080 10-244-1-15.svc-B.default.svc.cluster.local.
       _http._tcp.svc-B.default.svc.cluster.local. 30 IN SRV 0 50 8080 10-244-2-23.svc-B.default.svc.cluster.local.

  Query: svc-B.default.svc.cluster.local. IN A (headless service)
  
  1-2. Same as above
  3. Lookup: ServiceMap → found, Type=Headless (ClusterIP=None)
  4. Lookup: EndpointMap["default/svc-B"] → 2 ready endpoints
  5. Return: A records with pod IPs:
       svc-B.default.svc.cluster.local. 30 IN A 10.244.1.15
       svc-B.default.svc.cluster.local. 30 IN A 10.244.2.23
     Records returned in random order (loadbalance plugin)
```

**Failure modes:**
- **K8s API server unreachable:** CoreDNS informer cache retains last-known state. Queries continue to be served from stale cache. New/deleted services will not be reflected. Detection: CoreDNS health check reports unhealthy if API connection lost for > 30 seconds.
- **CoreDNS pod OOM:** With very large clusters (100K+ endpoints), the informer cache can consume significant memory. Solution: increase memory limits; use EndpointSlices (1000x fewer API objects than Endpoints for large services). CoreDNS 1.9+ uses EndpointSlices by default.
- **Stale endpoint after pod termination:** Pod terminates, but CoreDNS cache still has old endpoint. TTL (30s) ensures staleness is bounded. For headless services, DNS returns the dead pod IP until endpoint removal propagates (typically < 5 seconds via informer watch).

**Interviewer Q&As:**

> **Q1: How does CoreDNS learn about new services and endpoints?**
> A: CoreDNS uses the Kubernetes informer pattern. On startup, it LISTs all Services and EndpointSlices. Then it WATCHes for changes via long-poll HTTP connections to the API server. When a Service is created or an endpoint is added/removed, the informer callback updates the in-memory maps. DNS queries read from these maps. There is no polling — updates are push-based with typically sub-second latency.

> **Q2: Why is EndpointSlice preferred over Endpoints?**
> A: The legacy Endpoints object contains ALL endpoints for a service in a single object. For a service with 5,000 pods, the Endpoints object is ~1 MB. Any pod change (scale event, rolling update) triggers a full replace of this 1 MB object. EndpointSlices split endpoints into slices of 100 endpoints each. A pod change only updates the affected slice (~10 KB). This reduces API server load by 100x for large services.

> **Q3: What happens if CoreDNS is down?**
> A: NodeLocal DNSCache serves cached entries. For cache misses, queries fail (SERVFAIL). If NodeLocal DNSCache is also down or not deployed, pods cannot resolve any DNS names. This is a total outage for all network communication. CoreDNS must be highly available: 3+ replicas, PodDisruptionBudget, pod anti-affinity, and autoscaling.

> **Q4: How does CoreDNS handle zone transfers?**
> A: CoreDNS does not support traditional DNS zone transfers (AXFR/IXFR) for the Kubernetes zone. The authoritative data comes from the Kubernetes API, not from a zone file. For external zones, CoreDNS can serve static zone files (`file` plugin) which do support zone transfers. For cross-cluster DNS, use multi-cluster service discovery (MCS API) or DNS federation.

> **Q5: How does the `loadbalance` plugin work?**
> A: For DNS responses with multiple A records (headless services, external services with multiple IPs), the `loadbalance` plugin shuffles the record order randomly for each query. This provides round-robin behavior since most DNS clients use the first record. Without this plugin, records are returned in a deterministic order, causing all clients to hit the same pod.

> **Q6: How does CoreDNS handle reverse DNS (PTR) queries?**
> A: Pods with `dnsPolicy: ClusterFirst` may do reverse lookups. CoreDNS's kubernetes plugin handles `in-addr.arpa` and `ip6.arpa` zones. It looks up the IP in the PodMap and EndpointMap. For ClusterIPs, it returns `svc-name.ns.svc.cluster.local`. For pod IPs, it returns the pod's DNS name (if pod DNS is enabled). This is important for logging and security audit.

---

### Deep Dive 2: ndots Optimization and DNS Query Amplification

**Why it's hard:**
The default `ndots:5` setting in Kubernetes means any hostname with fewer than 5 dots triggers search domain expansion. Since most external hostnames have 1-3 dots (e.g., `api.stripe.com` has 2), every external DNS lookup generates 3-4 wasted NXDOMAIN queries before the actual resolution. At 200K external QPS, this becomes 800K wasted queries. This is the single largest source of unnecessary DNS load in Kubernetes clusters.

**Approaches:**

| Approach | Query Reduction | Compatibility | Complexity |
|----------|----------------|---------------|------------|
| Keep ndots=5 (default) | None (4x amplification for external) | Full K8s compatibility | None |
| Set ndots=2 | ~60% reduction for external names | May break short internal names | Low |
| Set ndots=1 | ~80% reduction | Breaks bare service names (`svc-B`) | Medium |
| Use FQDN with trailing dot | 100% (single query) | Requires application changes | High |
| autopath plugin (CoreDNS) | ~75% reduction | Transparent | Medium |
| Pod dnsConfig per deployment | Per-service optimization | Flexible | Medium |

**Selected approach: ndots=2 for most workloads + autopath plugin + FQDN for external calls**

**Implementation detail:**

```yaml
# Pod-level ndots optimization
apiVersion: v1
kind: Pod
metadata:
  name: svc-A
spec:
  dnsConfig:
    options:
      - name: ndots
        value: "2"
  containers:
    - name: app
      # Application code: use FQDN for external calls
      # "api.stripe.com."  (trailing dot = FQDN, no search expansion)
      # "svc-B"            (0 dots < 2 = search expansion → correct)
      # "svc-B.default"    (1 dot < 2 = search expansion → correct)

# Impact analysis:
#
# With ndots=5 (default):
#   "svc-B"           → 1 query  (svc-B.default.svc.cluster.local → hit)
#   "api.stripe.com"  → 4 queries (3 NXDOMAIN + 1 success)
#   "a.b.c.d.e"       → 1 query  (5 dots >= 5 → direct lookup)
#
# With ndots=2:
#   "svc-B"           → 1 query  (0 dots < 2 → search first → hit)
#   "svc-B.default"   → 1 query  (1 dot < 2 → search first → hit)
#   "api.stripe.com"  → 1 query  (2 dots >= 2 → direct lookup first → hit)
#   "svc-B.default.svc" → 1 query (2 dots >= 2 → direct first → hit)
#
# Savings: ~75% fewer queries for external names
```

```
CoreDNS autopath plugin:

  Without autopath:
    Client sends 4 separate queries for "api.stripe.com":
      Q1: api.stripe.com.default.svc.cluster.local. → NXDOMAIN
      Q2: api.stripe.com.svc.cluster.local.          → NXDOMAIN
      Q3: api.stripe.com.cluster.local.              → NXDOMAIN
      Q4: api.stripe.com.                            → A 54.xxx.xxx.xxx

    4 round-trips between pod and CoreDNS.

  With autopath:
    Client sends Q1: api.stripe.com.default.svc.cluster.local.
    CoreDNS autopath plugin:
      - Detects the query is a search domain expansion
      - Internally tries all search domains + FQDN in one pass
      - Returns the successful result directly

    1 round-trip! CoreDNS does the search expansion server-side.

  Configuration:
    .:53 {
        kubernetes cluster.local in-addr.arpa ip6.arpa {
            pods verified
            fallthrough in-addr.arpa ip6.arpa
        }
        autopath @kubernetes    # Use kubernetes plugin's pod data
        forward . /etc/resolv.conf
        cache 30
    }

  Trade-off: autopath requires the `pods verified` option (looks up
  source IP to determine namespace/search domains), which adds memory
  usage (~50 bytes per pod for pod IP mapping).
```

**Failure modes:**
- **ndots too low (ndots=1):** Bare service names like `svc-B` have 0 dots (< 1), so search expansion works. But names like `svc-B.default` have 1 dot (>= 1), so they are treated as FQDN first, and `svc-B.default.` fails (not a valid domain). Must use `svc-B.default.svc` (3 dots) or `svc-B.default.svc.cluster.local` (4 dots) for cross-namespace calls.
- **autopath incorrect namespace:** If pod IP changes before CoreDNS's pod map updates, autopath may use the wrong namespace for search domain expansion. Impact: one extra query (falls back to normal resolution). Not a correctness issue.
- **Negative cache poisoning:** An NXDOMAIN for `svc-B.default.svc.cluster.local` is cached. If the service is created shortly after, the negative cache entry prevents resolution until TTL expires. Solution: short negative cache TTL (5 seconds in CoreDNS default config).

**Interviewer Q&As:**

> **Q1: Why is the default ndots=5 in Kubernetes?**
> A: To ensure that short service names (e.g., `svc-B`, `svc-B.namespace`, `svc-B.namespace.svc`) are resolved by trying search domains first. The fully qualified internal name `svc-B.default.svc.cluster.local` has 4 dots. With ndots=5, any name with fewer than 5 dots tries search domains first, ensuring all short forms work. The cost is query amplification for external names.

> **Q2: How does negative caching affect service creation?**
> A: If a pod queries `svc-new.default.svc.cluster.local` before the service exists, CoreDNS returns NXDOMAIN. This is cached (negative cache, default 5 seconds). If the service is created within those 5 seconds, queries continue to get NXDOMAIN until the cache expires. With NodeLocal DNSCache, there is an additional 30-second cache layer. Total staleness: up to 35 seconds. Solution: reduce negative cache TTL or ensure services are created before dependent pods.

> **Q3: How do you handle DNS for short-lived pods (Jobs, CronJobs)?**
> A: Short-lived pods create and destroy endpoints rapidly. CoreDNS informer handles this via watch events (sub-second propagation). The issue is DNS caching: clients may cache the pod IP for the TTL duration after the pod terminates. For headless services, this is mitigated by short TTL (5-10 seconds). For ClusterIP services, it does not matter (ClusterIP is stable; kube-proxy handles backend changes).

> **Q4: What is the DNS query cost difference between ndots=5 and ndots=2 at scale?**
> A: With 200,000 external QPS and ndots=5: 200K x 4 = 800K total queries (3 wasted NXDOMAIN per). With ndots=2: 200K x 1 = 200K queries (direct lookup first). Savings: 600K QPS, which is significant. At ~100 bytes per UDP query, that is 60 MB/s of saved network traffic and ~600K fewer NXDOMAIN responses CoreDNS must process.

> **Q5: How do you handle DNS resolution for pods that use host networking?**
> A: Host-network pods (`hostNetwork: true`) use the node's `/etc/resolv.conf`, not the pod-injected one. They do not use the cluster DNS by default. To force cluster DNS: set `dnsPolicy: ClusterFirstWithHostNet` in the pod spec. This injects the cluster DNS nameserver (10.96.0.10 or 169.254.25.10) into the pod's resolv.conf even in host-network mode.

> **Q6: How does autopath interact with NodeLocal DNSCache?**
> A: autopath runs in CoreDNS (not NodeLocal DNSCache). With NodeLocal, the query path is: pod → NodeLocal → CoreDNS (autopath). NodeLocal forwards cache misses to CoreDNS, which applies autopath. The optimization works correctly because autopath acts on the initial query from the pod. NodeLocal caches the final result, so subsequent queries for the same name hit NodeLocal cache and skip CoreDNS entirely.

---

### Deep Dive 3: NodeLocal DNSCache

**Why it's hard:**
CoreDNS runs as a Deployment with a ClusterIP. Every DNS query from every pod traverses the kube-proxy/IPVS/eBPF service mesh to reach a CoreDNS pod — potentially on a different node. At 150K QPS, this creates significant cross-node traffic, iptables conntrack entries, and latency. NodeLocal DNSCache runs as a DaemonSet on every node, providing a local caching layer that eliminates cross-node DNS traffic for cache hits.

**Approaches:**

| Approach | Cache Hit Latency | Cross-Node Traffic | Memory Overhead | Complexity |
|----------|------------------|-------------------|-----------------|------------|
| No local cache (CoreDNS only) | 2-5 ms (cross-node) | 100% | None | Low |
| NodeLocal DNSCache | 0.2-0.5 ms (local) | Cache miss only (~20%) | ~50 MB per node | Medium |
| Per-pod DNS cache (dnsmasq sidecar) | 0.1 ms (in-pod) | Cache miss only | ~20 MB per pod (20K pods = 400 GB!) | Very high |
| Systemd-resolved on host | 0.2 ms | Cache miss only | Minimal | Low (but K8s integration poor) |

**Selected approach: NodeLocal DNSCache (DaemonSet)**

**Implementation detail:**

```
NodeLocal DNSCache Architecture:

  ┌──────────────────────────────────────────────┐
  │  Node                                         │
  │                                                │
  │  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
  │  │  Pod A    │  │  Pod B    │  │  Pod C    │    │
  │  │ resolv:  │  │ resolv:  │  │ resolv:  │    │
  │  │ 169.254. │  │ 169.254. │  │ 169.254. │    │
  │  │ 25.10    │  │ 25.10    │  │ 25.10    │    │
  │  └────┬─────┘  └────┬─────┘  └────┬─────┘    │
  │       │              │              │          │
  │       └──────────────┼──────────────┘          │
  │                      │  Loopback/link-local    │
  │                      │  (never leaves node)    │
  │                      ▼                         │
  │  ┌───────────────────────────────────────┐    │
  │  │  NodeLocal DNSCache (DaemonSet pod)    │    │
  │  │  IP: 169.254.25.10 (link-local)       │    │
  │  │                                         │    │
  │  │  CoreDNS instance with:                 │    │
  │  │  ┌─────────┐                            │    │
  │  │  │ cache   │ 10K entries, 30s TTL       │    │
  │  │  │ plugin  │                            │    │
  │  │  └────┬────┘                            │    │
  │  │       │ cache miss                      │    │
  │  │       ▼                                 │    │
  │  │  ┌─────────────────┐                    │    │
  │  │  │ forward plugin  │                    │    │
  │  │  │                 │                    │    │
  │  │  │ cluster.local → │─── TCP ──► CoreDNS│    │
  │  │  │   __PILLAR__    │         ClusterIP  │    │
  │  │  │   CLUSTER__DNS  │         10.96.0.10 │    │
  │  │  │                 │                    │    │
  │  │  │ . (external) →  │─── TCP ──► Upstream│    │
  │  │  │   __PILLAR__    │         resolvers  │    │
  │  │  │   UPSTREAM__    │         8.8.8.8    │    │
  │  │  │   SERVERS       │                    │    │
  │  │  └─────────────────┘                    │    │
  │  │                                         │    │
  │  │  Key: Uses TCP to upstream (not UDP)     │    │
  │  │  Reason: Avoids conntrack for UDP → DNS  │    │
  │  │  UDP conntrack entries can race/leak     │    │
  │  └───────────────────────────────────────┘    │
  │                                                │
  └────────────────────────────────────────────────┘

NodeLocal DNSCache Corefile:

  cluster.local:53 {
      errors
      cache {
          success 9984 30
          denial 9984 5
      }
      reload
      loop
      bind 169.254.25.10
      forward . __PILLAR__CLUSTER__DNS__IP__ {
          force_tcp       # Use TCP to avoid conntrack race
      }
      prometheus :9253
  }
  
  in-addr.arpa:53 {
      errors
      cache 30
      reload
      loop
      bind 169.254.25.10
      forward . __PILLAR__CLUSTER__DNS__IP__ {
          force_tcp
      }
  }
  
  .:53 {
      errors
      cache 30
      reload
      loop
      bind 169.254.25.10
      forward . __PILLAR__UPSTREAM__SERVERS__ {
          force_tcp
      }
      prometheus :9253
  }

Performance Impact:
  Without NodeLocal:
    DNS query: pod → iptables/IPVS → CoreDNS (potentially cross-node)
    Latency: 2-5 ms p99
    Network: 150K QPS × 512 bytes = 77 MB/s cross-node

  With NodeLocal:
    Cache hit (80%): pod → local CoreDNS → response (< 0.5 ms)
    Cache miss (20%): pod → local CoreDNS → CoreDNS → response (3-5 ms)
    Network savings: 80% reduction in cross-node DNS traffic
    Latency improvement: 4x reduction in p50 latency
```

**Failure modes:**
- **NodeLocal pod crash:** The link-local IP (169.254.25.10) becomes unreachable. All pods on that node cannot resolve DNS. Mitigation: iptables rule on the node that falls back to CoreDNS ClusterIP if the local address is unreachable. Or: kubelet detects NodeLocal is down and rewrites pod resolv.conf to use ClusterIP directly.
- **Cache poisoning:** A malicious response could be cached. Mitigation: DNSSEC validation (if enabled), very short cache TTLs for external records, and trust-only the upstream forwarder (not arbitrary responses).
- **Stale cache during service migration:** Service IP changes but NodeLocal cache has old IP for 30 seconds. Impact: bounded staleness (30s). Acceptable for most use cases. For instant failover, reduce TTL or invalidate cache on watch events.

**Interviewer Q&As:**

> **Q1: Why does NodeLocal DNSCache use TCP to forward to upstream?**
> A: UDP DNS queries through conntrack have a known race condition in the Linux kernel: the conntrack entry for the DNS response can be created before the entry for the query, causing the response to be dropped. This manifests as intermittent DNS failures (5-second timeout before retry). Using TCP avoids this conntrack race entirely because TCP connections are tracked reliably. This is a well-known Kubernetes issue (kubernetes/kubernetes#56903).

> **Q2: Why use a link-local IP (169.254.25.10) instead of the ClusterIP?**
> A: The link-local IP is node-specific: it is only reachable from pods on that node, and traffic never crosses the network. Using the ClusterIP would route through kube-proxy/IPVS, potentially sending the query to a CoreDNS pod on another node. The link-local IP short-circuits the service mesh entirely. It is also faster: direct local delivery vs. DNAT + network traversal.

> **Q3: How do you monitor NodeLocal DNSCache health across 200+ nodes?**
> A: Each NodeLocal pod exposes Prometheus metrics on port 9253. Key metrics: `coredns_dns_requests_total` (QPS per node), `coredns_cache_hits_total` / `coredns_cache_misses_total` (cache hit ratio), `coredns_dns_request_duration_seconds` (latency histogram), `coredns_forward_responses_total` (upstream query rate). Dashboard: aggregated fleet-wide cache hit ratio, per-node latency heatmap.

> **Q4: What happens if NodeLocal and CoreDNS are both down?**
> A: Total DNS failure. No service can resolve any hostname. All HTTP connections fail after TCP timeout. gRPC connections using name resolution fail. This is the highest-severity outage. Prevention: CoreDNS with 5+ replicas, PDB (min 3 available), pod anti-affinity. NodeLocal as DaemonSet with node-level health monitoring. Monitor CoreDNS latency and error rate; alert on any degradation.

> **Q5: How does NodeLocal handle cache invalidation for internal records?**
> A: NodeLocal does not watch the Kubernetes API — it is purely a cache. Cache invalidation is TTL-based (30 seconds for internal records). When an internal service endpoint changes, NodeLocal serves stale data for up to 30 seconds. For most service-to-service calls behind a ClusterIP (where kube-proxy handles endpoint changes at L4), this is fine because the DNS-resolved ClusterIP does not change. For headless services, the 30-second staleness matters more.

> **Q6: How do you size NodeLocal DNSCache for each node?**
> A: Memory: 50 MB base + ~256 bytes per cached entry. With 10,000 cached entries: ~52.5 MB. CPU: < 0.1 cores for 10,000 QPS (per-node cache hit serving is very cheap). Set resource requests at 50m CPU, 64Mi memory. Set limits at 100m CPU, 128Mi memory. For nodes with many pods (> 100), increase cache size and resources proportionally.

---

### Deep Dive 4: ExternalDNS and Dynamic Record Management

**Why it's hard:**
When Kubernetes Services (type LoadBalancer) or Ingresses are created, external DNS records must be automatically created in Route53/Cloud DNS. The ExternalDNS controller must handle: ownership tracking (which controller manages which records), conflict resolution (multiple controllers targeting the same zone), safe record deletion (only delete records it owns), and synchronization with eventual consistency of DNS APIs.

**Approaches:**

| Approach | Automation | Safety | Multi-Controller | Maturity |
|----------|-----------|--------|------------------|----------|
| Manual DNS management | None | High (human review) | N/A | N/A |
| ExternalDNS (K8s controller) | Full | Good (TXT ownership records) | Supported | Mature |
| Terraform DNS provider | Semi (IaC) | High (plan/apply) | Supported (state locking) | Mature |
| Custom controller | Full | Varies | Custom | Custom |
| AWS Cloud Map (service discovery) | Full (AWS only) | Good | N/A | Mature |

**Selected approach: ExternalDNS controller with TXT ownership records**

**Implementation detail:**

```
ExternalDNS Controller Loop:

  Every {sync-interval} (60 seconds):
  
  1. Source collection:
     ┌──────────────────────────────────────────────────────┐
     │ Watch Kubernetes resources:                           │
     │                                                       │
     │ Services (type=LoadBalancer):                         │
     │   svc "api-gateway" → external IP 203.0.113.10       │
     │   annotation: external-dns.alpha.kubernetes.io/       │
     │     hostname: api.example.com                         │
     │                                                       │
     │ Ingresses:                                            │
     │   ingress "web" → host: www.example.com               │
     │   → LB IP from ingress status                         │
     │                                                       │
     │ Gateway API HTTPRoutes:                               │
     │   route "api" → host: api.example.com                 │
     └──────────────────────────────────────────────────────┘
     
     Desired records:
       api.example.com.     A     203.0.113.10
       www.example.com.     A     198.51.100.20
  
  2. Registry lookup (current state):
     ┌──────────────────────────────────────────────────────┐
     │ Read DNS zone from provider (Route53):                │
     │                                                       │
     │ Current records:                                      │
     │   api.example.com.     A     203.0.113.10             │
     │   api.example.com.     TXT   "heritage=external-dns,  │
     │                               external-dns/owner=     │
     │                               k8s-cluster-1,          │
     │                               external-dns/resource=  │
     │                               service/default/        │
     │                               api-gateway"            │
     │   old.example.com.     A     203.0.113.99             │
     │   old.example.com.     TXT   "heritage=external-dns,  │
     │                               external-dns/owner=     │
     │                               k8s-cluster-1, ..."     │
     │   manual.example.com.  A     10.0.5.100               │
     │   (no TXT ownership → NOT managed by ExternalDNS)     │
     └──────────────────────────────────────────────────────┘
  
  3. Plan (diff desired vs. current):
     ┌──────────────────────────────────────────────────────┐
     │ Create:  www.example.com. A 198.51.100.20            │
     │ Update:  (none — api.example.com already correct)     │
     │ Delete:  old.example.com. A 203.0.113.99             │
     │          (owned by this controller, no longer desired) │
     │ Skip:    manual.example.com (not owned)               │
     └──────────────────────────────────────────────────────┘
  
  4. Apply:
     Route53 API calls:
       ChangeResourceRecordSets: CREATE www.example.com A
       ChangeResourceRecordSets: CREATE www.example.com TXT (ownership)
       ChangeResourceRecordSets: DELETE old.example.com A
       ChangeResourceRecordSets: DELETE old.example.com TXT (ownership)

  5. Wait for next sync interval (60s)

Safety mechanisms:
  --policy=upsert-only     : Create/update only, never delete
  --policy=sync            : Full sync (create, update, delete)
  --domain-filter          : Only manage specific domains
  --txt-owner-id           : Unique per controller instance
  --dry-run                : Log changes without applying
```

```
Multi-Cluster ExternalDNS with Weighted Records:

  Cluster 1 (us-east):
    Service api-gateway → LB IP 203.0.113.10
    ExternalDNS annotation:
      external-dns.alpha.kubernetes.io/aws-weight: "70"
      external-dns.alpha.kubernetes.io/set-identifier: "us-east"

  Cluster 2 (eu-west):
    Service api-gateway → LB IP 198.51.100.20
    ExternalDNS annotation:
      external-dns.alpha.kubernetes.io/aws-weight: "30"
      external-dns.alpha.kubernetes.io/set-identifier: "eu-west"

  Route53 result:
    api.example.com.  A  203.0.113.10  (weight 70, set-id: us-east)
    api.example.com.  A  198.51.100.20 (weight 30, set-id: eu-west)

  DNS resolvers return IPs proportionally: 70% us-east, 30% eu-west.
```

**Failure modes:**
- **ExternalDNS controller down:** DNS records become stale. No new records created, no old records deleted. Existing records continue working. This is a degradation, not an outage. Solution: multiple replicas with leader election; alert on controller health.
- **Route53 API rate limiting:** Route53 API has rate limits (5 TPS for ChangeResourceRecordSets). Large clusters with frequent changes can hit this. Solution: batch changes into fewer API calls; increase sync interval.
- **Ownership record deleted manually:** If someone deletes the TXT ownership record, ExternalDNS loses track and will not update or delete the corresponding A record. Solution: reconciliation loop re-creates ownership records; or detect orphaned records.
- **Two controllers claiming the same record:** If two clusters run ExternalDNS with the same `txt-owner-id`, they fight over records. Solution: unique `txt-owner-id` per cluster; use weighted records with unique `set-identifier` for multi-cluster.

**Interviewer Q&As:**

> **Q1: How does ExternalDNS avoid deleting records it doesn't own?**
> A: ExternalDNS creates a companion TXT record for every A/CNAME record it manages. The TXT record contains the `heritage=external-dns` marker and the `owner-id`. Before deleting a record, ExternalDNS checks for the corresponding TXT record. If the TXT record is missing or has a different owner, ExternalDNS will not touch the record. This prevents deleting manually-created or externally-managed records.

> **Q2: How do you handle DNS-based failover with ExternalDNS?**
> A: Use Route53 health checks. ExternalDNS creates health-checked records that automatically failover. Alternatively, each cluster's ExternalDNS manages weighted records. If a cluster is unhealthy, its ExternalDNS stops updating (or removes) its weighted record, and DNS naturally fails over to the remaining cluster's records. TTL determines failover speed.

> **Q3: What is the TTL trade-off for external DNS records?**
> A: Short TTL (30-60s): fast failover (clients re-resolve quickly), but higher DNS query volume. Long TTL (300-3600s): lower query volume, but slow failover (clients use stale cached records). Recommendation: 60s for production API endpoints (fast failover), 300s for static content (CDN handles failover). During planned migrations, temporarily lower TTL before the change, then raise it after.

> **Q4: How do you handle DNS propagation delays during a migration?**
> A: DNS changes propagate through resolver caches, which honor TTL. Full propagation can take up to the current TTL. Strategy: (1) Lower TTL to 60s well in advance (24+ hours before migration). (2) Make the DNS change. (3) Wait for TTL to expire. (4) Verify via `dig` from multiple global locations. (5) Restore TTL to normal. During the TTL window, both old and new endpoints must be operational.

> **Q5: How does ExternalDNS work with split-horizon DNS?**
> A: Use separate hosted zones for internal and external. ExternalDNS manages the external zone (`--aws-zone-type=public`). A separate ExternalDNS instance (or CoreDNS file plugin) manages the internal zone. The `--domain-filter` and `--zone-id-filter` flags ensure each instance only touches its assigned zone.

> **Q6: Can ExternalDNS create records for non-HTTP services (e.g., MQTT, databases)?**
> A: Yes. ExternalDNS watches any Kubernetes Service with the appropriate annotations. A TCP LoadBalancer Service for MQTT can have `external-dns.alpha.kubernetes.io/hostname: mqtt.example.com`. ExternalDNS creates an A record pointing to the LoadBalancer IP. It works with any Service type that has an external IP or hostname in its status.

---

## 7. Scaling Strategy

**CoreDNS horizontal scaling:**
- Start with 3 replicas. Use HPA based on DNS QPS (custom Prometheus metric `coredns_dns_requests_total`).
- Scale formula: 1 CoreDNS pod per 10,000 QPS.
- At 150K QPS steady state: 15 CoreDNS pods.
- At 1M QPS peak: 100 CoreDNS pods (with NodeLocal absorbing 80% → 20 CoreDNS pods actually needed).

**NodeLocal scaling:**
- DaemonSet automatically scales with nodes.
- No additional configuration needed per node.
- Memory scales with cache size (configurable).

**External DNS scaling:**
- ExternalDNS is lightweight (watches K8s API, makes periodic DNS API calls).
- Single replica is sufficient for most clusters.
- For large clusters (10,000+ services): increase sync interval to reduce API load.

**DNS infrastructure scaling:**
- Upstream recursive resolvers: run 2+ instances with Anycast.
- Authoritative nameservers: Route53/Cloud DNS handles scaling automatically.
- For bare-metal: PowerDNS Recursor with 4+ instances behind IPVS.

**Interviewer Q&As:**

> **Q1: How do you scale CoreDNS for a 100,000-pod cluster?**
> A: With 100K pods at 10 QPS avg = 1M QPS total. NodeLocal absorbs 80% (cache hit) = 200K QPS reaching CoreDNS. At 30K QPS per CoreDNS pod, need ~7 CoreDNS pods. For peak (5M QPS total), 1M QPS to CoreDNS, need ~33 pods. Use HPA to scale between 7-33 based on load. Also: scope CoreDNS to only watch EndpointSlices (not legacy Endpoints) to reduce API server load.

> **Q2: How do you scale DNS for a multi-cluster setup?**
> A: Each cluster runs independent CoreDNS. Cross-cluster resolution options: (1) DNS peering: CoreDNS forwards `*.cluster-2.local` to cluster-2's CoreDNS. (2) Multi-cluster Service API: services exported from one cluster are visible in another via DNS. (3) Global DNS (Route53): both clusters register services with ExternalDNS; clients resolve via global DNS. Option 3 is simplest; option 2 is most Kubernetes-native.

> **Q3: What happens when CoreDNS becomes a bottleneck?**
> A: Symptoms: increasing DNS latency, timeout errors in applications, CoreDNS pod CPU saturation. Immediate mitigation: increase CoreDNS replicas. Long-term: (1) Deploy NodeLocal DNSCache if not already. (2) Reduce ndots to lower query amplification. (3) Enable autopath plugin. (4) Increase cache TTLs. (5) For extremely large clusters: shard CoreDNS by namespace (forward `*.payments.svc.cluster.local` to a dedicated CoreDNS fleet).

> **Q4: How do you handle DNS during cluster upgrades?**
> A: CoreDNS runs as a Deployment in `kube-system`. During cluster upgrade: (1) CoreDNS pods may restart (but PDB ensures minimum available). (2) NodeLocal DNSCache continues serving cached entries during CoreDNS restart. (3) etcd downtime (during control plane upgrade) means CoreDNS informer cache becomes stale — but stale data is acceptable for the upgrade duration (minutes). (4) Monitor DNS latency during upgrades.

> **Q5: How do you prevent DNS from being a single point of failure?**
> A: Multi-layer defense: (1) NodeLocal DNSCache (per-node) survives CoreDNS outage for cached entries. (2) CoreDNS with 5+ replicas, PDB, pod anti-affinity across nodes and AZs. (3) Pod-level DNS caching (application-level, if supported — e.g., JVM DNS cache). (4) External DNS has independent authoritative nameservers (Route53 = global, anycast, SLA 100%). (5) Monitor at every layer: NodeLocal, CoreDNS, upstream resolvers.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Single CoreDNS pod crash | Kubernetes readiness probe fails; endpoint removed | Slightly increased latency (remaining pods handle load) | HPA + PDB ensure min replicas; auto-restart | < 30s |
| All CoreDNS pods down | NodeLocal forward failures; pod DNS resolution fails for cache misses | New DNS resolutions fail; cached entries still work | PDB prevents simultaneous termination; 5+ replicas across nodes | < 60s (pod restart) |
| NodeLocal DNSCache crash | Link-local IP unreachable; pod DNS queries fail | Pods on that node lose DNS | iptables fallback to CoreDNS ClusterIP; DaemonSet auto-restart | < 15s |
| Kubernetes API server down | CoreDNS informer disconnects; logs errors | CoreDNS serves stale data from informer cache | HA API server (3+ replicas); stale cache is acceptable short-term | Stale until API recovers |
| Upstream recursive resolver down | DNS forward timeouts; external resolution fails | External domains unresolvable | Multiple upstream resolvers (8.8.8.8, 1.1.1.1); CoreDNS `forward` plugin tries next | Automatic failover (< 5s) |
| DNS cache poisoning | Unexpected DNS responses; security monitoring | Applications connect to wrong IP | DNSSEC validation; trusted forwarders only; short cache TTL | Immediate (if detected) |
| ExternalDNS controller down | Controller health check; no DNS sync | External DNS records become stale; new services not published | Leader election HA; alert on staleness | Records stale until controller recovers |
| Route53 API outage | ExternalDNS sync failures | Cannot create/update/delete external DNS records | Records still served (authoritative NS has data); ExternalDNS retries | Depends on AWS recovery |
| DNS query flood (DDoS) | CoreDNS QPS spike; latency increase | DNS resolution slow for all pods | NodeLocal absorbs most load; CoreDNS HPA scales; rate limiting per source | Automatic scaling (< 60s) |
| Negative cache TTL too long | Service created but DNS returns NXDOMAIN | New service unreachable for cache TTL duration | Short negative cache TTL (5s); manual cache flush if needed | Up to negative cache TTL |
| conntrack table full (UDP DNS) | DNS query drops; timeout errors | DNS resolution fails intermittently | NodeLocal uses TCP to upstream; increase conntrack max; migrate to eBPF | Immediate (once conntrack freed) |

---

## 9. Security

**DNS Security:**
- DNSSEC validation for external domains (CoreDNS `dnssec` plugin or upstream resolver validation).
- DoT (DNS over TLS) or DoH (DNS over HTTPS) for external queries to prevent eavesdropping.
- CoreDNS bind to ClusterIP only (not exposed externally).
- NodeLocal DNSCache bind to link-local IP (169.254.25.10) — not routable externally.

**Access Control:**
- Kubernetes RBAC: CoreDNS ServiceAccount has read-only access to Services, Endpoints, EndpointSlices, Pods, Namespaces.
- ExternalDNS ServiceAccount has minimal IAM permissions (Route53 RecordSet CRUD on specific zones only).
- DNS query logging for audit (who queried what, when).

**Anti-Abuse:**
- Rate limiting per source IP (CoreDNS `ratelimit` plugin).
- Block DNS rebinding attacks (validate response IPs are not in private ranges for external queries).
- Prevent DNS tunneling by monitoring for unusually large TXT record queries or high query volume to uncommon domains.

**Zone Protection:**
- External DNS zones have change locks (Route53 `EnableHostedZoneDNSSEC`, IAM policies restricting zone changes).
- Split-horizon DNS ensures internal records are not leaked to external resolvers.
- ExternalDNS `--policy=upsert-only` in production to prevent accidental deletions.

---

## 10. Incremental Rollout

**Phase 1 (Weeks 1-2): CoreDNS Optimization**
- Audit current CoreDNS configuration and resource usage.
- Enable Prometheus metrics (`prometheus :9153`).
- Measure baseline: QPS, latency, cache hit ratio.
- Optimize: enable `cache` plugin with appropriate TTLs.

**Phase 2 (Weeks 3-4): NodeLocal DNSCache**
- Deploy NodeLocal DNSCache DaemonSet in a test cluster.
- Update kubelet `--cluster-dns` to 169.254.25.10.
- Measure improvement: latency reduction, CoreDNS QPS reduction.
- Roll out to production node-by-node.

**Phase 3 (Weeks 5-6): ndots Optimization**
- Audit top DNS queries (identify external domain query amplification).
- Deploy autopath plugin to CoreDNS.
- Set `ndots:2` for new deployments via admission webhook default.
- Measure: 60-75% reduction in total DNS QPS.

**Phase 4 (Weeks 7-8): ExternalDNS**
- Deploy ExternalDNS in `--dry-run` mode.
- Verify planned DNS changes match expectations.
- Switch to `--policy=upsert-only` (create/update only).
- Migrate manually-managed DNS records to ExternalDNS-managed.

**Phase 5 (Weeks 9-10): Advanced Features**
- Enable split-horizon DNS for internal services.
- Configure stub domains for non-Kubernetes services (Consul, etc.).
- Enable DNS query logging for security monitoring.

**Rollout Q&As:**

> **Q1: How do you validate NodeLocal DNSCache is working correctly before production?**
> A: Deploy to a canary node. Run DNS benchmark tools (dnsperf, queryperf) from pods on that node. Compare latency and QPS with non-NodeLocal nodes. Verify cache hit ratio > 70%. Check that internal service resolution and external domain resolution both work. Monitor for DNS failures in application logs.

> **Q2: How do you roll back ndots changes if they break something?**
> A: ndots is set per-pod via dnsConfig. To roll back: revert the deployment spec to remove the dnsConfig override. Pods restart with default ndots=5. For namespace-wide changes via admission webhook: disable the webhook and restart affected pods. Monitor for DNS resolution failures (SERVFAIL, NXDOMAIN increase) during rollout.

> **Q3: How do you migrate from kube-dns to CoreDNS?**
> A: Kubernetes 1.13+ uses CoreDNS by default. For legacy clusters: (1) Deploy CoreDNS alongside kube-dns. (2) Update the `kube-dns` Service selector to point to CoreDNS pods. (3) Verify resolution works. (4) Remove kube-dns deployment. The Service stays the same (ClusterIP 10.96.0.10), so pods do not need to be restarted.

> **Q4: How do you test ExternalDNS changes safely?**
> A: (1) `--dry-run` mode: log planned changes without applying. (2) `--policy=upsert-only`: only create/update (never delete). (3) `--domain-filter=staging.example.com`: start with staging domain only. (4) Use a separate hosted zone for testing. (5) After validation, expand to production domain with `--policy=sync` (full management).

> **Q5: How do you handle the transition period for NodeLocal DNSCache deployment?**
> A: NodeLocal uses a DaemonSet. As it rolls out node-by-node: (1) Nodes WITH NodeLocal: pods use 169.254.25.10 (local cache). (2) Nodes WITHOUT NodeLocal: pods use 10.96.0.10 (CoreDNS ClusterIP, unchanged). kubelet `--cluster-dns` change requires node restart or kubelet restart. To avoid disruption: cordon node, drain pods, update kubelet config, uncordon. Or: use node-local-dns admission controller that automatically sets resolv.conf based on NodeLocal availability.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Trade-off |
|----------|-------------------|--------|-----------|-----------|
| Cluster DNS | CoreDNS, kube-dns, external (Consul) | CoreDNS | K8s default, plugin-based, performant, CNCF | Plugin configuration complexity |
| Node-level caching | NodeLocal DNSCache, no caching, per-pod dnsmasq | NodeLocal DNSCache | Per-node efficiency, no per-pod overhead, proven | Additional DaemonSet to manage; staleness for cached entries |
| ndots setting | 5 (default), 2, 1 | 2 (for most workloads) | Reduces external query amplification by 75% | Requires `svc.ns` or FQDN for cross-namespace internal calls |
| External DNS management | Manual, ExternalDNS, Terraform | ExternalDNS | Automated, K8s-native, ownership tracking | Requires careful IAM; eventual consistency |
| Upstream resolvers | Cloud (8.8.8.8, 1.1.1.1), self-hosted (unbound, PowerDNS) | Both (self-hosted primary, cloud fallback) | Self-hosted gives more control; cloud as reliable fallback | Must operate DNS infrastructure |
| DNS query logging | Always on, sampling, off | Sampling (1% steady, 100% on-demand) | Sufficient for debugging without massive storage | May miss rare DNS issues |
| Split-horizon | Separate zones, view-based (BIND), CoreDNS file plugin | Separate zones (CoreDNS server blocks) | Clear separation, simple config | Must manage two zones |
| Cache TTL (internal) | 5s, 30s, 60s | 30s | Balance between staleness (30s max) and query reduction (high cache hit) | 30-second window of stale data after endpoint change |

---

## 12. Agentic AI Integration

### AI-Powered DNS Operations

**DNS Anomaly Detection Agent:**
- Monitors DNS query patterns per service, per namespace, per pod.
- Detects: (1) Sudden query volume spikes (potential DNS amplification attack or misconfiguration). (2) Unusual NXDOMAIN rates (broken service name, typo, or DNS tunneling). (3) External domain resolution to unexpected IPs (DNS hijacking). (4) Query latency degradation (upstream resolver issues).
- Automatically alerts and can take action: rate limit abusive source, block suspicious external domains.

**DNS Configuration Optimizer Agent:**
- Analyzes actual DNS query patterns cluster-wide.
- Identifies: (1) Services with high external query amplification (ndots issue). (2) Pods querying services they never successfully connect to (broken dependencies). (3) Unnecessary headless services (all queries go to ClusterIP anyway). (4) Services with suboptimal TTL settings.
- Recommends: ndots changes per deployment, CoreDNS cache tuning, search domain optimizations.
- Example: "Deployment `analytics-worker` generates 50K NXDOMAIN queries/hour due to ndots=5. Recommend: set ndots=2 in dnsConfig. Estimated savings: 37.5K queries/hour."

**Capacity Planning Agent:**
- Tracks DNS QPS growth trends.
- Predicts when CoreDNS scaling thresholds will be reached.
- Correlates DNS growth with service/pod count growth.
- Recommends: CoreDNS replica count adjustments, NodeLocal cache size changes, upstream resolver capacity.

**Incident Response Agent:**
- When DNS latency spikes or resolution failures occur:
  - Automatically checks: CoreDNS pod health, NodeLocal health, upstream resolver reachability, K8s API server health.
  - Identifies root cause from metrics (e.g., "CoreDNS CPU at 98% due to 5x traffic spike from namespace `batch-jobs`").
  - Suggests immediate action: scale CoreDNS, rate-limit the offending namespace, increase cache TTL.
  - Executes approved actions via K8s API.

### Integration Architecture

```
┌────────────────────────────────────────────────────┐
│             DNS AI Agent                            │
│                                                      │
│  Data sources:                                       │
│  ├── CoreDNS metrics (Prometheus)                    │
│  ├── NodeLocal metrics (Prometheus)                  │
│  ├── DNS query logs (Elasticsearch)                  │
│  ├── K8s events (service/endpoint changes)           │
│  └── ExternalDNS sync status                         │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │ Anomaly  │  │ Config   │  │ Capacity         │  │
│  │ Detector │  │ Optimizer│  │ Planner          │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────────────┘  │
│       └──────────────┼──────────────┘                │
│                      ▼                               │
│              ┌──────────────┐                        │
│              │ Action Queue │                        │
│              └──────┬───────┘                        │
│                     ▼                                │
│              ┌──────────────┐                        │
│              │ Execute via  │                        │
│              │ K8s API /    │                        │
│              │ CoreDNS      │                        │
│              │ ConfigMap    │                        │
│              └──────────────┘                        │
│                                                      │
│  Safety: never reduce CoreDNS replicas below 3       │
│  Safety: cache TTL changes capped at 2x current      │
│  Safety: auto-rollback on NXDOMAIN rate increase      │
└────────────────────────────────────────────────────┘
```

---

## 13. Complete Interviewer Q&A Bank

> **Q1: Walk me through the full DNS resolution path when a pod in Kubernetes calls an external domain.**
> A: (1) App calls `getaddrinfo("api.stripe.com")`. (2) glibc reads `/etc/resolv.conf`: ndots=5, so search domains are tried first. (3) Query `api.stripe.com.default.svc.cluster.local` → NodeLocal (cache miss) → CoreDNS → kubernetes plugin → NXDOMAIN. (4) Repeat for `.svc.cluster.local` and `.cluster.local` → NXDOMAIN. (5) Query `api.stripe.com.` → NodeLocal (cache miss) → CoreDNS → forward plugin → upstream resolver (8.8.8.8) → authoritative NS → A record. (6) Response cached at CoreDNS and NodeLocal. (7) IP returned to app. Total: 4 wasted queries + 1 successful.

> **Q2: What is the difference between a headless service and a regular ClusterIP service in DNS?**
> A: ClusterIP service: DNS returns a single A record with the virtual ClusterIP (e.g., 10.96.15.200). Traffic is load-balanced by kube-proxy/IPVS at L4. Headless service (clusterIP: None): DNS returns multiple A records, one per ready pod IP. The client receives all pod IPs and must choose one (client-side load balancing). Headless also supports individual pod DNS for StatefulSets: `pod-0.svc.ns.svc.cluster.local`.

> **Q3: Why is ndots=5 the default and what are the risks of changing it?**
> A: ndots=5 ensures all short Kubernetes names work with search domains. Risk of lowering to ndots=2: names like `svc.namespace.svc` (3 dots) are treated as FQDN first (fails), then search domains are tried (slower). Risk of ndots=1: bare service names (`svc-B`) work, but `svc-B.namespace` (1 dot) tries FQDN first. Most applications use either bare names or fully-qualified names, so ndots=2 is safe in practice.

> **Q4: How do you handle DNS for a service that spans multiple namespaces?**
> A: Kubernetes DNS is namespace-scoped. A service `svc-B` in namespace `payments` is resolved as `svc-B.payments.svc.cluster.local`. Pods in other namespaces must use the full name (or at least `svc-B.payments`). For services that should be globally addressable without namespace qualification: create an ExternalName service in each consumer namespace that CNAMEs to the target service's full name.

> **Q5: How does DNS-based service discovery compare to registry-based (Consul, etcd)?**
> A: DNS advantages: universal (every language/framework supports DNS), no client library needed, transparent to applications. DNS disadvantages: limited metadata (only IP:port), TTL-based staleness (seconds), no health status in response, query amplification (ndots). Registry advantages: rich metadata, instant updates (push-based), health status, key-value store. Registry disadvantages: requires client library or sidecar, not transparent.

> **Q6: What is the "5-second DNS timeout" problem in Kubernetes?**
> A: A known Linux kernel bug causes UDP DNS queries through conntrack to intermittently timeout for 5 seconds. The bug occurs when the kernel creates a conntrack entry for the DNS response before the query entry, causing the response to be dropped. The client retries after its 5-second timeout. Solutions: (1) NodeLocal DNSCache with TCP forwarding. (2) `single-request-reopen` in resolv.conf options. (3) Use `tc-bpf` to bypass conntrack for DNS traffic.

> **Q7: How do you implement DNS-based failover for a multi-region service?**
> A: Two approaches: (1) Route53 health-checked records: create A records for each region with health checks. Route53 automatically removes unhealthy records. Failover TTL should be 60s. (2) Anycast DNS: announce the same IP from multiple regions via BGP. When a region fails, BGP withdraws the route and traffic shifts. Anycast is faster (BGP convergence < 30s) but harder to debug.

> **Q8: How does CoreDNS handle DNS-over-TCP vs DNS-over-UDP?**
> A: CoreDNS supports both. UDP is default for queries < 512 bytes. TCP is used for: (1) Responses that exceed 512 bytes (DNS truncation flag triggers TCP retry). (2) Zone transfers (AXFR). (3) When NodeLocal forces TCP to avoid conntrack races. Performance: TCP adds ~0.5ms latency (handshake overhead). CoreDNS handles TCP efficiently with connection reuse and pooling.

> **Q9: How do you monitor DNS health proactively?**
> A: Synthetic monitoring: a CronJob sends known DNS queries every 10 seconds and checks results. CoreDNS metrics: `coredns_dns_requests_total` (QPS), `coredns_dns_response_rcode_count_total` (NXDOMAIN, SERVFAIL rates), `coredns_dns_request_duration_seconds` (latency), `coredns_cache_hits_total`/`misses_total` (cache efficiency). Alert on: latency > 10ms p99, SERVFAIL rate > 0.1%, cache hit ratio < 50%.

> **Q10: How does Kubernetes DNS handle service IP changes?**
> A: ClusterIPs are immutable for the lifetime of a Service. If you delete and recreate a service, a new ClusterIP is assigned. DNS records update within seconds (informer watch). However, clients that cached the old ClusterIP (TTL-based) will still use it. The old ClusterIP is invalid and connections fail. Solution: avoid deleting and recreating services; instead, update in-place. For migration: use ExternalName service as a stable CNAME.

> **Q11: What is split-horizon DNS and when do you need it?**
> A: Split-horizon returns different DNS answers for the same name depending on the query source. Internal clients get internal IPs (10.x); external clients get public IPs (203.x). Use cases: (1) Internal clients should use the internal load balancer (lower latency, no hairpin NAT). (2) Some services should only be resolvable internally. Implementation: separate DNS zones with different records; CoreDNS serves internal zone, Route53 serves external zone.

> **Q12: How do you debug DNS resolution issues in a Kubernetes pod?**
> A: (1) `kubectl exec -it pod -- nslookup svc-B.default.svc.cluster.local` to test resolution. (2) `kubectl exec -it pod -- cat /etc/resolv.conf` to verify nameserver and search domains. (3) Check CoreDNS logs: `kubectl logs -n kube-system -l k8s-app=kube-dns`. (4) Check CoreDNS metrics: `/metrics` endpoint for error counts. (5) Verify the service exists: `kubectl get svc svc-B -n default`. (6) Check endpoints exist: `kubectl get endpoints svc-B -n default`.

> **Q13: How does DNS work with StatefulSets?**
> A: StatefulSets with a headless Service get stable, individual DNS names: `pod-0.svc.ns.svc.cluster.local`, `pod-1.svc.ns.svc.cluster.local`, etc. These resolve to individual pod IPs and update when pods are rescheduled. This is critical for stateful workloads (databases, ZooKeeper) where peers need to address each other by name. The headless service itself (`svc.ns.svc.cluster.local`) returns all pod IPs.

> **Q14: What is the performance difference between CoreDNS and BIND9?**
> A: CoreDNS: ~30,000 QPS per core for Kubernetes queries (informer cache lookup). BIND9: ~100,000+ QPS per core for static zones (highly optimized C code). But BIND9 does not integrate with Kubernetes natively. PowerDNS Recursor: ~50,000 QPS per core. For Kubernetes internal DNS, CoreDNS is the only practical choice due to the kubernetes plugin. For authoritative external DNS, BIND9/PowerDNS outperform CoreDNS.

> **Q15: How do you implement DNS-based blue-green deployment?**
> A: Use ExternalDNS with weighted records or Route53 routing policies. Blue environment: `api.example.com` → blue LB IP (weight 100). Green environment deployed; create weighted record → green LB IP (weight 0). Gradually shift: blue 90/green 10, blue 50/green 50, blue 0/green 100. Verify health at each step. Rollback: shift weight back to blue. TTL should be 60s for fast switching.

> **Q16: How does DNS caching at the application level interact with CoreDNS caching?**
> A: Three cache layers: (1) Application-level (JVM DNS cache: default 30s for positive, 10s for negative. Go: no caching by default. Python: glibc nscd or systemd-resolved). (2) NodeLocal DNSCache (30s TTL). (3) CoreDNS cache (30s TTL). Total staleness: up to 90 seconds in worst case (all caches have stale entry). For time-sensitive resolution: reduce all cache TTLs and set JVM `networkaddress.cache.ttl=5`.

---

## 14. References

- **CoreDNS Documentation:** https://coredns.io/manual/toc/
- **CoreDNS Kubernetes Plugin:** https://coredns.io/plugins/kubernetes/
- **NodeLocal DNSCache:** https://kubernetes.io/docs/tasks/administer-cluster/nodelocaldns/
- **ExternalDNS:** https://github.com/kubernetes-sigs/external-dns
- **Kubernetes DNS Specification:** https://github.com/kubernetes/dns/blob/master/docs/specification.md
- **DNS for Services and Pods:** https://kubernetes.io/docs/concepts/services-networking/dns-pod-service/
- **5-Second DNS Timeout Bug:** https://github.com/kubernetes/kubernetes/issues/56903
- **CoreDNS autopath Plugin:** https://coredns.io/plugins/autopath/
- **Route53 DNS Failover:** https://docs.aws.amazon.com/Route53/latest/DeveloperGuide/dns-failover.html
- **SPIFFE Identity via DNS:** https://spiffe.io/
