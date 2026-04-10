# System Design: Network Policy Enforcement System

> **Relevance to role:** Network policy enforcement is critical for securing multi-tenant Kubernetes clusters and bare-metal infrastructure. For this role, you must understand Kubernetes NetworkPolicy semantics (ingress/egress, pod/namespace selectors), CNI enforcement mechanisms (Calico iptables/eBPF, Cilium eBPF, Antrea OVS), eBPF kernel-level enforcement, default deny patterns, L7 policy enforcement, egress control, and policy-as-code via GitOps. This is a core security and compliance requirement for any production infrastructure platform.

---

## 1. Requirement Clarifications

### Functional Requirements

| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | L3/L4 network policy enforcement | Allow/deny traffic based on IP, port, protocol, pod labels, namespace labels |
| FR-2 | Ingress policies | Control which sources can send traffic to a workload |
| FR-3 | Egress policies | Control which destinations a workload can reach |
| FR-4 | Namespace isolation | Default deny between namespaces; explicit allow for cross-namespace |
| FR-5 | L7 policy enforcement | HTTP method/path/header filtering; gRPC service/method filtering |
| FR-6 | DNS-based egress policies | Allow egress to specific FQDNs (e.g., allow `api.stripe.com` but block all other internet) |
| FR-7 | Policy audit mode | Log policy violations without blocking traffic (dry-run) |
| FR-8 | Network flow visibility | Observe all network flows with source/destination identity, verdict (allow/deny) |
| FR-9 | Policy-as-code | Manage policies via Git with CI/CD validation and deployment |
| FR-10 | Cluster-wide default policies | Global default deny with namespace-level and workload-level overrides |

### Non-Functional Requirements

| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Policy enforcement latency | < 100 microseconds per packet (eBPF) |
| NFR-2 | Policy propagation time | < 5 seconds from policy creation to enforcement on all nodes |
| NFR-3 | Scale | 20,000+ pods, 1,000+ network policies |
| NFR-4 | Throughput impact | < 5% throughput reduction vs. no policy enforcement |
| NFR-5 | Availability | Policy engine failure must not block traffic (fail-open optional) |
| NFR-6 | Observability | Full flow logs for every allow/deny decision |

### Constraints & Assumptions

- Kubernetes 1.28+ with a CNI that supports NetworkPolicy.
- Bare-metal infrastructure with either Calico or Cilium as CNI.
- Multi-tenant cluster: multiple teams share the same cluster with namespace isolation.
- Some workloads require L7 policy enforcement (HTTP/gRPC).
- Compliance requirement: all inter-namespace traffic must be explicitly authorized.

### Out of Scope

- Host-level firewall management (iptables rules on the node itself outside Kubernetes).
- Cloud-native security groups (AWS SGs, GCP firewall rules).
- Service mesh authorization policies (covered in service_proxy_and_sidecar.md).
- Intrusion detection/prevention systems (IDS/IPS).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|------------|-------|
| Total pods | Given | 20,000 pods |
| Total nodes | Given | 200 nodes |
| Pods per node | 20,000 / 200 | 100 pods/node |
| Total namespaces | Given | 100 namespaces (teams/environments) |
| Network policies (L3/L4) | 5 per namespace avg | 500 policies |
| Cilium Network Policies (L7) | 2 per namespace avg | 200 policies |
| Total unique pod labels | 20,000 pods x 5 labels avg | 100,000 label entries |
| Packets per second (cluster-wide) | From LB design: 4.5M pps | 4,500,000 pps |
| Packets per second (per node) | 4.5M / 200 nodes | 22,500 pps/node |
| New connections per second (cluster) | 450,000 conn/s | 450,000 conn/s |
| New connections per second (per node) | 450K / 200 | 2,250 conn/s/node |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Per-packet policy evaluation (eBPF) | < 50 us (cached), < 200 us (first packet of flow) |
| Per-packet policy evaluation (iptables) | < 100 us (cached), < 500 us (first packet) |
| Policy propagation to datapath | < 5 seconds (policy creation to enforcement) |
| Flow log emission | < 100 ms (async, non-blocking) |

### Storage Estimates

| Data | Calculation | Size |
|------|------------|------|
| iptables rules per node (Calico) | 100 pods x 5 rules per pod x 200 bytes | 100 KB |
| eBPF maps per node (Cilium) | Policy map + identity map + conntrack | 50 MB |
| Flow logs (1 hour) | 4.5M pps x 3600s x 100 bytes (aggregated per flow, not per packet) | ~10 GB/hour (at flow granularity) |
| Policy objects in etcd | 700 policies x 2 KB avg | 1.4 MB |
| Hubble flow storage (Cilium) | Ring buffer per node, 1M flows | 200 MB per node |

### Bandwidth Estimates

| Data | Calculation | Value |
|------|------------|-------|
| Policy distribution (per node) | 100 KB policy + 1 MB identity map | 1.1 MB on change |
| Flow log export | 10 GB/hour / 200 nodes = 50 MB/hour/node | ~14 KB/s per node |
| Policy update propagation | Watch events, ~10/s x 2 KB | 20 KB/s cluster-wide |

---

## 3. High Level Architecture

```
┌───────────────────────────────────────────────────────────────────────┐
│                        CONTROL PLANE                                   │
│                                                                         │
│  ┌──────────────────┐  ┌────────────────────┐  ┌───────────────────┐  │
│  │  Kubernetes       │  │  CNI Controller     │  │  Policy Validator │  │
│  │  API Server       │  │  (Calico/Cilium     │  │  (OPA/Kyverno    │  │
│  │                   │  │   Operator)          │  │   Admission      │  │
│  │  NetworkPolicy    │  │                      │  │   Webhook)       │  │
│  │  CiliumNetworkPol │  │  - Watch policies    │  │                  │  │
│  │  GlobalNetworkPol │  │  - Compile to        │  │  - Validate      │  │
│  │                   │  │    eBPF/iptables     │  │    policy syntax │  │
│  │                   │  │  - Distribute to     │  │  - Check for     │  │
│  │                   │  │    node agents       │  │    conflicts     │  │
│  │                   │  │                      │  │  - Enforce naming│  │
│  └───────┬───────────┘  └────────┬─────────────┘  └────────┬────────┘  │
│          │                       │                          │           │
│          │   Watch               │  Compile & push          │           │
│          │   policies            │  to node agents          │           │
└──────────┼───────────────────────┼──────────────────────────┼───────────┘
           │                       │                          │
           ▼                       ▼                          │
┌───────────────────────────────────────────────────────────────────────┐
│                        DATA PLANE (per Node)                           │
│                                                                         │
│  ┌────────────────────────────────────────────────────────────────┐    │
│  │  CNI Agent (Calico Felix / Cilium Agent)                        │    │
│  │                                                                  │    │
│  │  ┌─────────────────┐  ┌──────────────────┐  ┌───────────────┐  │    │
│  │  │  Policy Engine  │  │  Identity Manager │  │  Datapath     │  │    │
│  │  │                  │  │                    │  │  Programmer   │  │    │
│  │  │  - Parse rules   │  │  - Map pod labels  │  │               │  │    │
│  │  │  - Compile to    │  │    to identities   │  │  - iptables   │  │    │
│  │  │    datapath       │  │  - Track pod       │  │    rules OR   │  │    │
│  │  │    rules          │  │    lifecycle        │  │  - eBPF       │  │    │
│  │  │  - Handle updates│  │  - Endpoint CRDs   │  │    programs   │  │    │
│  │  └─────────────────┘  └──────────────────┘  └───────┬───────┘  │    │
│  │                                                       │          │    │
│  └───────────────────────────────────────────────────────┼──────────┘    │
│                                                           │              │
│  ┌───────────────────────────────────────────────────────┼──────────┐   │
│  │  KERNEL / DATAPATH                                     │          │   │
│  │                                                         ▼          │   │
│  │  ┌───────────────────────────────────────────────────────────┐    │   │
│  │  │                    eBPF Programs                            │    │   │
│  │  │                                                             │    │   │
│  │  │  TC Ingress Hook ──────► Policy Map Lookup ──► ALLOW/DROP  │    │   │
│  │  │  TC Egress Hook  ──────► Policy Map Lookup ──► ALLOW/DROP  │    │   │
│  │  │                                                             │    │   │
│  │  │  OR (Calico iptables mode):                                 │    │   │
│  │  │  iptables FORWARD ──► calico chains ──► ACCEPT/DROP        │    │   │
│  │  │                                                             │    │   │
│  │  │  Flow log: eBPF perf event / iptables LOG                  │    │   │
│  │  └───────────────────────────────────────────────────────────┘    │   │
│  │                                                                    │   │
│  │  ┌───────────────────────────────────────────────────────────┐    │   │
│  │  │  Hubble (Cilium) / Flow Logs                                │    │   │
│  │  │                                                             │    │   │
│  │  │  eBPF perf ring buffer → Hubble agent → Hubble Relay       │    │   │
│  │  │  → Hubble UI / Prometheus metrics / Elasticsearch           │    │   │
│  │  └───────────────────────────────────────────────────────────┘    │   │
│  └────────────────────────────────────────────────────────────────────┘   │
│                                                                           │
│  Pods:  [pod-A: identity=12345] [pod-B: identity=67890] ...             │
│  Each pod's veth has eBPF programs attached at TC ingress/egress         │
└───────────────────────────────────────────────────────────────────────────┘

  ┌───────────────────────────────────────────────────────────────────────┐
  │                    POLICY-AS-CODE PIPELINE                             │
  │                                                                         │
  │  Git Repo ──► CI Validation ──► PR Review ──► Merge ──► ArgoCD/Flux  │
  │  (policies     (OPA conftest,    (team        (main     (apply to     │
  │   as YAML)      syntax check,     approval)    branch)   cluster)     │
  │                 conflict detect)                                        │
  └───────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| Kubernetes API Server | Stores NetworkPolicy and CiliumNetworkPolicy CRDs; notifies controllers via watch |
| CNI Controller (Operator) | Watches policy objects; compiles into node-level enforcement rules |
| Policy Validator (Admission Webhook) | Validates policy syntax, checks for conflicts, enforces naming conventions |
| CNI Agent (per-node) | Receives compiled policies; programs iptables or eBPF datapath |
| Policy Engine | Translates policy selectors (labels) into concrete allow/deny rules |
| Identity Manager | Assigns numeric identities to pod label sets; used for efficient eBPF map lookup |
| Datapath Programmer | Installs iptables rules or eBPF programs on pod veth interfaces |
| Hubble / Flow Logs | Captures network flow data for observability and audit |
| GitOps Pipeline | Manages policies as code with version control and automated deployment |

### Data Flows

**Policy enforcement flow (eBPF/Cilium):**
1. Admin creates CiliumNetworkPolicy in Git; ArgoCD applies to cluster.
2. Cilium Operator watches the policy; distributes to Cilium agents on relevant nodes.
3. Cilium agent compiles policy into eBPF policy maps.
4. eBPF program attached to pod's veth TC hook evaluates every packet.
5. Packet arrives → extract source identity (from eBPF identity map) → lookup policy map (identity + port + protocol) → ALLOW or DROP.
6. Flow log emitted via eBPF perf event ring buffer → Hubble agent.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# ============================================
# Kubernetes NetworkPolicy (Standard API)
# ============================================
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: allow-frontend-to-backend
  namespace: production
spec:
  podSelector:
    matchLabels:
      app: backend           # Target: pods with label app=backend
  policyTypes:
    - Ingress
    - Egress
  ingress:
    - from:
        - podSelector:
            matchLabels:
              app: frontend   # Allow from: pods with app=frontend
          namespaceSelector:
            matchLabels:
              env: production  # In namespace with env=production
      ports:
        - protocol: TCP
          port: 8080           # On port 8080 only
  egress:
    - to:
        - podSelector:
            matchLabels:
              app: database
      ports:
        - protocol: TCP
          port: 5432
    - to:                        # Allow DNS
        - namespaceSelector: {}
          podSelector:
            matchLabels:
              k8s-app: kube-dns
      ports:
        - protocol: UDP
          port: 53
        - protocol: TCP
          port: 53

# ============================================
# CiliumNetworkPolicy (Cilium-specific, L7 capable)
# ============================================
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: l7-api-policy
  namespace: production
spec:
  endpointSelector:
    matchLabels:
      app: api-server
  ingress:
    - fromEndpoints:
        - matchLabels:
            app: frontend
      toPorts:
        - ports:
            - port: "8080"
              protocol: TCP
          rules:
            http:                  # L7 HTTP filtering
              - method: GET
                path: "/api/v1/users.*"
              - method: POST
                path: "/api/v1/orders"
                headers:
                  - "Content-Type: application/json"
  egress:
    - toEndpoints:
        - matchLabels:
            app: database
      toPorts:
        - ports:
            - port: "5432"
              protocol: TCP
    - toFQDNs:                     # DNS-based egress
        - matchName: "api.stripe.com"
        - matchPattern: "*.amazonaws.com"
      toPorts:
        - ports:
            - port: "443"
              protocol: TCP

# ============================================
# CiliumClusterwideNetworkPolicy (Global default deny)
# ============================================
apiVersion: cilium.io/v2
kind: CiliumClusterwideNetworkPolicy
metadata:
  name: default-deny-all
spec:
  endpointSelector: {}            # All pods
  ingress:
    - fromEntities:
        - cluster                  # Allow only from within cluster
  egress:
    - toEntities:
        - cluster                  # Allow only to within cluster
    - toEndpoints:
        - matchLabels:
            k8s-app: kube-dns     # Always allow DNS
      toPorts:
        - ports:
            - port: "53"
              protocol: UDP
            - port: "53"
              protocol: TCP

# ============================================
# Calico GlobalNetworkPolicy (Calico-specific)
# ============================================
apiVersion: projectcalico.org/v3
kind: GlobalNetworkPolicy
metadata:
  name: default-deny-non-system
spec:
  selector: projectcalico.org/namespace != "kube-system"
  types:
    - Ingress
    - Egress
  ingress:
    - action: Deny
  egress:
    - action: Deny
  order: 1000                      # Lower order = higher priority
```

```
Cilium Identity System:

  Pod labels → Identity (numeric ID)
  
  Pod: {app: frontend, version: v1, env: prod}
    → Identity: 12345
  
  Pod: {app: backend, version: v2, env: prod}
    → Identity: 67890
  
  All pods with identical label sets share the same identity.
  Identity is used as the key in eBPF policy maps.
  
  eBPF Policy Map Structure:
  ┌────────────────────────────────────────────────┐
  │  Map: cilium_policy_<endpoint_id>               │
  │  Type: BPF_MAP_TYPE_HASH                        │
  │                                                  │
  │  Key:   {identity: u32, dport: u16, proto: u8}  │
  │  Value: {flags: u8}                              │
  │           flags: ALLOW=1, DENY=2, REDIRECT=4     │
  │                  (REDIRECT = proxy to L7 Envoy)   │
  │                                                  │
  │  Example entries:                                │
  │  {identity=12345, dport=8080, proto=TCP} → ALLOW │
  │  {identity=0,     dport=0,    proto=0}   → DENY  │ (default)
  │  {identity=12345, dport=8080, proto=TCP} →        │
  │     REDIRECT (to L7 proxy for HTTP inspection)    │
  └────────────────────────────────────────────────┘
  
  Cilium Identity Map (shared across all eBPF programs):
  ┌────────────────────────────────────────────────┐
  │  Map: cilium_ipcache                            │
  │  Type: BPF_MAP_TYPE_LPM_TRIE                    │
  │                                                  │
  │  Key:   {prefix_len: u32, ip: u32}              │
  │  Value: {identity: u32, tunnel_endpoint: u32}   │
  │                                                  │
  │  Example:                                        │
  │  {/32, 10.244.1.15} → {identity: 12345}         │
  │  {/32, 10.244.2.23} → {identity: 67890}         │
  │  {/16, 10.96.0.0}   → {identity: WORLD}         │ (CIDR-based)
  └────────────────────────────────────────────────┘
```

### Database Selection

| Store | Technology | Rationale |
|-------|-----------|-----------|
| Policy objects | Kubernetes API (etcd) | Native CRD storage; watch-based distribution |
| Identity mapping | eBPF maps (cilium_ipcache) + kvstore (etcd) | In-kernel for fast lookup; etcd for cross-node sync |
| Policy compilation | In-memory (Cilium agent) | Per-node computation from policy + identity |
| Flow logs | Hubble ring buffer → Elasticsearch or ClickHouse | Time-series flow data for observability and audit |
| Policy-as-code | Git repository | Version control, PR review, audit trail |

### Indexing Strategy

| Data | Index | Purpose |
|------|-------|---------|
| NetworkPolicy | namespace + podSelector labels | Find policies applicable to a pod |
| Cilium identity | label set hash | O(1) identity lookup for label set |
| eBPF policy map | identity + dport + protocol | O(1) per-packet policy decision |
| eBPF ipcache | IP address (LPM trie) | O(log n) source identity resolution |
| Flow logs | timestamp + src_identity + dst_identity | Flow query and audit |

---

## 5. API Design

### Management APIs

```yaml
# Policy management via Kubernetes API (kubectl / GitOps)

# Create/apply network policy
kubectl apply -f network-policy.yaml

# List policies in a namespace
kubectl get networkpolicies -n production
kubectl get ciliumnetworkpolicies -n production
kubectl get ciliumclusterwidenetworkpolicies

# Describe policy (shows selectors, rules)
kubectl describe networkpolicy allow-frontend-to-backend -n production

# Cilium-specific: policy status and realization
cilium policy get                     # Dump all realized policies
cilium policy trace --src-identity 12345 --dst-identity 67890 --dport 8080
                                      # Trace policy decision for specific flow

# Cilium endpoint status (per-pod policy state)
cilium endpoint list                  # List all endpoints with policy status
cilium endpoint get 12345             # Get detailed endpoint info
cilium endpoint log 12345             # Get policy change log for endpoint

# Hubble CLI (flow observability)
hubble observe --namespace production --verdict DROPPED
                                      # Show dropped flows in production namespace
hubble observe --from-label app=frontend --to-label app=backend
                                      # Show flows between frontend and backend
hubble observe --protocol http --http-method GET
                                      # Show HTTP GET flows (L7)

# Hubble API (gRPC)
hubble.observer.Observer/GetFlows     # Stream flows matching filter
hubble.observer.Observer/GetNodes     # List Hubble observer nodes
hubble.peer.Peer/Notify              # Node join/leave notifications
```

### Data Plane Behavior

```
eBPF Policy Enforcement (Cilium):

  Packet arrives at pod's veth interface
       │
       ▼
  TC Ingress Hook (eBPF program: bpf_lxc)
       │
       ├── 1. Parse packet headers (L3: IP, L4: TCP/UDP)
       │
       ├── 2. Lookup source identity:
       │      cilium_ipcache[src_ip] → src_identity
       │      (If unknown: WORLD identity = 2)
       │
       ├── 3. Lookup policy:
       │      cilium_policy_<endpoint_id>[src_identity, dport, proto]
       │      │
       │      ├── ALLOW → pass packet
       │      ├── DENY  → drop packet, emit flow log
       │      └── REDIRECT → send to L7 proxy (Envoy)
       │                     for HTTP/gRPC inspection
       │
       ├── 4. Conntrack: if established flow, skip policy lookup
       │      cilium_ct[5-tuple] → {flags, identity, ...}
       │      (First packet of flow: full policy eval + create CT entry)
       │      (Subsequent packets: CT hit → fast path)
       │
       └── 5. Emit flow log via perf event ring buffer
              {src_ip, dst_ip, src_id, dst_id, port, proto,
               verdict: ALLOW/DENY, policy_id, timestamp}


iptables Policy Enforcement (Calico):

  Packet enters FORWARD chain
       │
       ▼
  cali-FORWARD
       │
       ├── cali-from-wl-dispatch (workload egress)
       │   └── cali-fw-<endpoint_id>
       │       ├── Conntrack: -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
       │       ├── Policy chains:
       │       │   cali-po-<policy_name>
       │       │     -s 10.244.1.15 -p tcp --dport 8080 -j MARK --set-mark 0x10000
       │       │     -j RETURN (on match)
       │       └── Default: -j DROP (if no policy matches)
       │
       ├── cali-to-wl-dispatch (workload ingress)
       │   └── cali-tw-<endpoint_id>
       │       ├── Similar structure to egress
       │       └── Default: -j DROP
       │
  Performance comparison:
    iptables: O(n) rule traversal per packet (n = number of rules)
    eBPF:     O(1) hash map lookup per packet
    
    At 500 iptables rules: ~200us per first packet
    At 5000 iptables rules: ~2ms per first packet (unacceptable)
    eBPF with 5000 policies: still ~50us (hash map)
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: eBPF-Based Policy Enforcement (Cilium)

**Why it's hard:**
eBPF programs run in the kernel with strict constraints (bounded execution, 512-byte stack, limited helpers). Implementing a full L3/L4/L7 policy engine in eBPF requires careful data structure design, efficient identity resolution, conntrack integration, and L7 proxying. The policy must be updated atomically when policies or endpoints change, without disrupting in-flight traffic.

**Approaches:**

| Approach | Performance | Scalability | L7 Support | Complexity |
|----------|-------------|-------------|-----------|------------|
| iptables chains (Calico default) | O(n) per packet | Poor at >5K rules | No (L3/L4 only) | Medium |
| ipsets + iptables (Calico optimized) | O(1) for IP matching | Good | No | Medium |
| eBPF TC hook (Cilium) | O(1) hash lookup | Excellent | Via proxy redirect | High |
| OVS (Open vSwitch) flows (Antrea) | O(1) flow table | Good | Via proxy | High |
| nftables (next-gen iptables) | Better than iptables | Good | No | Medium |

**Selected approach: eBPF TC hook with identity-based policy maps (Cilium)**

**Justification:** O(1) per-packet policy evaluation regardless of policy count. Identity-based model scales better than IP-based (1 identity per label set vs. 1 rule per IP). L7 enforcement via transparent redirect to Envoy proxy. eBPF maps can be updated atomically without disrupting datapath.

**Implementation detail:**

```c
// Simplified Cilium eBPF policy enforcement
// Attached to TC ingress hook on pod's veth (host side)

SEC("tc")
int policy_ingress(struct __sk_buff *skb) {
    // 1. Parse packet
    void *data = (void *)(long)skb->data;
    void *data_end = (void *)(long)skb->data_end;
    struct ethhdr *eth = data;
    struct iphdr *ip = data + sizeof(*eth);
    struct tcphdr *tcp = data + sizeof(*eth) + sizeof(*ip);
    
    // Bounds check (required by eBPF verifier)
    if (data + sizeof(*eth) + sizeof(*ip) + sizeof(*tcp) > data_end)
        return TC_ACT_OK;  // Not a TCP packet we care about
    
    // 2. Lookup source identity from ipcache
    struct ipcache_key src_key = {
        .lpm_key = {.prefixlen = 32},
        .ip = ip->saddr
    };
    struct ipcache_value *src_id = bpf_map_lookup_elem(&cilium_ipcache, &src_key);
    __u32 identity = src_id ? src_id->identity : WORLD_IDENTITY;
    
    // 3. Check conntrack (fast path for established flows)
    struct ct_key ct = {
        .src_ip = ip->saddr,
        .dst_ip = ip->daddr,
        .src_port = tcp->source,
        .dst_port = tcp->dest,
        .proto = ip->protocol
    };
    struct ct_entry *ct_entry = bpf_map_lookup_elem(&cilium_ct, &ct);
    if (ct_entry && ct_entry->state == CT_ESTABLISHED) {
        // Established flow: already policy-approved
        update_metrics(ALLOW, identity);
        return TC_ACT_OK;
    }
    
    // 4. New flow: full policy evaluation
    struct policy_key pol_key = {
        .identity = identity,
        .dport = bpf_ntohs(tcp->dest),
        .protocol = ip->protocol
    };
    struct policy_entry *pol = bpf_map_lookup_elem(
        &cilium_policy_ENDPOINT_ID, &pol_key);
    
    if (!pol) {
        // No explicit policy entry: check default
        struct policy_key default_key = {
            .identity = 0,  // wildcard
            .dport = 0,
            .protocol = 0
        };
        pol = bpf_map_lookup_elem(&cilium_policy_ENDPOINT_ID, &default_key);
    }
    
    if (!pol || pol->flags == POLICY_DENY) {
        // DROP: emit flow log and drop packet
        emit_flow_log(skb, identity, VERDICT_DROPPED);
        update_metrics(DROP, identity);
        return TC_ACT_SHOT;  // Drop packet
    }
    
    if (pol->flags == POLICY_REDIRECT) {
        // L7 enforcement: redirect to Envoy proxy
        // Envoy will inspect HTTP/gRPC content
        skb->mark = PROXY_MARK;
        return TC_ACT_REDIRECT;  // Redirect to proxy
    }
    
    // ALLOW: create conntrack entry for fast-path
    struct ct_entry new_ct = {
        .state = CT_ESTABLISHED,
        .identity = identity,
        .timestamp = bpf_ktime_get_ns()
    };
    bpf_map_update_elem(&cilium_ct, &ct, &new_ct, BPF_ANY);
    
    emit_flow_log(skb, identity, VERDICT_ALLOWED);
    update_metrics(ALLOW, identity);
    return TC_ACT_OK;  // Allow packet
}
```

```
Identity Resolution System:

  When a pod starts:
  1. Cilium agent detects new pod (via K8s informer)
  2. Extract pod labels: {app: frontend, version: v1, env: prod}
  3. Hash labels → lookup identity:
     If existing identity with same labels → reuse (e.g., 12345)
     If new label set → allocate new identity (e.g., 99999)
  4. Store in cilium_ipcache BPF map:
     {10.244.1.15/32} → {identity: 12345}
  5. Distribute identity to all nodes via kvstore (etcd)
     Other nodes update their local cilium_ipcache

  Benefits of identity-based model:
  - 100 pods with same labels = 1 identity = 1 policy map entry
  - Pod IP changes (reschedule) → identity stays the same
  - Policy evaluation: O(1) regardless of pod count
  
  vs. IP-based model:
  - 100 pods = 100 IP entries in policy rules
  - Pod reschedule = update all policies referencing that pod
  - Policy evaluation: O(n) where n = number of IP rules

Policy Compilation Pipeline:

  NetworkPolicy YAML
       │
       ▼
  ┌──────────────────┐
  │ Policy Parser     │ Parse selectors, rules, ports
  └──────┬───────────┘
         │
         ▼
  ┌──────────────────┐
  │ Selector Resolver │ Resolve label selectors to identities
  │                    │ {app: frontend} → identities [12345, 12346]
  └──────┬───────────┘
         │
         ▼
  ┌──────────────────┐
  │ Policy Compiler   │ Generate BPF map entries
  │                    │ {identity=12345, dport=8080, proto=TCP} → ALLOW
  └──────┬───────────┘
         │
         ▼
  ┌──────────────────┐
  │ Map Updater       │ Atomically update BPF policy maps
  │                    │ bpf_map_update_elem() for each entry
  └──────────────────┘

  Atomicity: Updates are per-entry (not per-map). During update,
  some entries may be new and some old. This is acceptable because:
  - New ALLOW entries: traffic that should be allowed is allowed slightly early
  - New DENY entries: traffic that should be denied may be allowed briefly
  - Solution: for deny-critical policies, add deny rules first, then allow
```

**Failure modes:**
- **eBPF verifier rejects program:** Program is not loaded; old program continues running. No traffic disruption. Cilium agent logs error. Fix: debug with `cilium bpf prog list` and verifier output.
- **eBPF map full:** If policy map exceeds max entries, new entries silently fail. Default deny kicks in (drops unmatched traffic). Alert on map utilization. Increase map size (`bpf-policy-map-max`).
- **Identity allocation race:** Two pods with new label sets start simultaneously on different nodes. Both allocate identities via kvstore CAS (compare-and-swap). If collision, one retries. Brief window where identity is not yet distributed to all nodes → packets from unknown identity treated as WORLD. Mitigated by kvstore eventual consistency (seconds).
- **Cilium agent crash:** eBPF programs remain loaded and active (kernel-space). Policy enforcement continues with last-known rules. No new policy updates until agent restarts. This is a key advantage of eBPF over iptables: the datapath survives agent restarts.

**Interviewer Q&As:**

> **Q1: Why is eBPF-based enforcement superior to iptables at scale?**
> A: iptables evaluates rules linearly (O(n) per packet). With 5,000 rules, first-packet latency reaches ~2ms. eBPF uses hash map lookups (O(1) per packet). At 50,000 policies, eBPF latency remains ~50us. Additionally, eBPF programs persist in the kernel independently of the user-space agent, so agent restarts do not disrupt enforcement. iptables rules are user-space-managed and can be lost on agent crash.

> **Q2: How does Cilium handle L7 policy enforcement?**
> A: When a policy has L7 rules (HTTP method/path), the eBPF program marks the packet with POLICY_REDIRECT. The TC hook redirects the packet to a per-pod Envoy proxy instance. Envoy inspects the HTTP request (method, path, headers) against the L7 rules. If allowed, Envoy forwards to the original destination. If denied, Envoy returns 403. This adds ~1-2ms latency for L7-inspected flows.

> **Q3: How does policy enforcement work for pods that are not yet assigned an identity?**
> A: During pod startup, there is a brief window before the Cilium agent assigns an identity and programs the eBPF maps. During this window, the pod cannot send or receive traffic (eBPF default deny). Once the identity is assigned and maps are programmed, traffic flows. This takes typically 1-3 seconds. The pod's readiness probe should account for this delay.

> **Q4: How do you handle DNS-based egress policies (toFQDNs)?**
> A: Cilium intercepts DNS responses from the pod. When the pod resolves `api.stripe.com`, Cilium captures the returned IPs (e.g., 54.187.x.x). These IPs are dynamically added to the egress policy's allow list. The eBPF map is updated with: `{dst_ip=54.187.x.x, dport=443, proto=TCP} → ALLOW`. When DNS TTL expires, old IPs are removed. This provides FQDN-based egress control without static IP lists.

> **Q5: What happens if the Cilium agent is down for an extended period?**
> A: eBPF programs continue enforcing the last-known policy (kernel-space). But: (1) New pods cannot get identities (no policy applied). (2) Pod IP changes (rescheduling) are not reflected in ipcache (stale identity mapping). (3) Policy changes are not applied. (4) DNS-based policies stop updating (FQDN IPs become stale). Agent should restart automatically (DaemonSet); if it does not, node should be cordoned.

> **Q6: How do you debug a packet being unexpectedly dropped by policy?**
> A: (1) `cilium policy trace --src-identity 12345 --dst-identity 67890 --dport 8080`: trace the policy decision for a specific flow. (2) `hubble observe --verdict DROPPED --to-label app=backend`: show all dropped flows to the backend. (3) `cilium endpoint get <id>`: check the endpoint's policy status and realized rules. (4) `cilium bpf policy get <endpoint_id>`: dump the eBPF policy map to see all allowed identities/ports. (5) `cilium monitor --type drop`: real-time drop event monitoring.

---

### Deep Dive 2: Default Deny and Multi-Tenancy Isolation

**Why it's hard:**
In a multi-tenant cluster, namespaces must be isolated by default. Any pod should only communicate with explicitly authorized peers. But "default deny" has subtle semantics in Kubernetes: a NetworkPolicy that selects a pod activates policy enforcement for that pod (default deny for unmatched traffic). Pods without ANY NetworkPolicy selecting them have NO enforcement (default allow). This implicit behavior is confusing and dangerous.

**Approaches:**

| Approach | Security | Usability | Maintenance | Compatibility |
|----------|----------|-----------|-------------|---------------|
| No default policies (Kubernetes default) | None (all traffic allowed) | High | None | Universal |
| Default deny per namespace (NetworkPolicy) | Good | Medium | 1 policy per NS | Universal |
| Cluster-wide default deny (CiliumClusterwideNetworkPolicy) | Best | Medium | 1 global policy | Cilium only |
| Calico GlobalNetworkPolicy (default deny) | Best | Medium | 1 global policy | Calico only |
| Admission webhook enforcing default deny exists | Best | Medium | 1 webhook rule | Universal |

**Selected approach: Cluster-wide default deny (Cilium) + namespace-level allow policies + admission webhook enforcement**

**Implementation detail:**

```yaml
# Step 1: Cluster-wide default deny
# Blocks ALL inter-pod traffic except explicitly allowed
apiVersion: cilium.io/v2
kind: CiliumClusterwideNetworkPolicy
metadata:
  name: default-deny-all-namespaces
spec:
  description: "Default deny all inter-pod traffic cluster-wide"
  endpointSelector: {}            # Matches ALL pods
  ingress:
    - fromEntities:
        - host                     # Allow from node (health checks, kubelet)
  egress:
    - toEntities:
        - host                     # Allow to node (DNS on NodeLocal, etc.)
    - toEndpoints:
        - matchLabels:
            "k8s:io.kubernetes.pod.namespace": kube-system
            k8s-app: kube-dns      # Always allow DNS
      toPorts:
        - ports:
            - port: "53"
              protocol: UDP
            - port: "53"
              protocol: TCP

# Step 2: Namespace-level policies (per team)
# Team "payments" namespace:
---
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: allow-intra-namespace
  namespace: payments
spec:
  description: "Allow all traffic within payments namespace"
  endpointSelector: {}
  ingress:
    - fromEndpoints:
        - {}                       # All pods in same namespace
  egress:
    - toEndpoints:
        - {}                       # All pods in same namespace

---
# Cross-namespace: allow payments-api to call users-service in users namespace
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: allow-payments-to-users
  namespace: users
spec:
  endpointSelector:
    matchLabels:
      app: users-service
  ingress:
    - fromEndpoints:
        - matchLabels:
            app: payments-api
            "k8s:io.kubernetes.pod.namespace": payments
      toPorts:
        - ports:
            - port: "8080"
              protocol: TCP

# Step 3: System namespace exemption
# kube-system, monitoring, logging namespaces need broader access
---
apiVersion: cilium.io/v2
kind: CiliumClusterwideNetworkPolicy
metadata:
  name: allow-monitoring-scrape
spec:
  description: "Allow Prometheus to scrape all pods"
  endpointSelector:
    matchLabels:
      "k8s:io.kubernetes.pod.namespace": monitoring
      app: prometheus
  egress:
    - toEntities:
        - cluster                  # Prometheus can reach any pod in cluster
      toPorts:
        - ports:
            - port: "9090"
              protocol: TCP
            - port: "9153"
              protocol: TCP
            - port: "8080"
              protocol: TCP
```

```
Multi-Tenancy Isolation Model:

  Namespace: payments (Team A)
  ┌─────────────────────────────────────────┐
  │  Policies:                               │
  │  1. Intra-namespace: ALLOW (all pods)    │
  │  2. Egress to users-svc: ALLOW (port 80) │
  │  3. Egress to stripe API: ALLOW (FQDN)   │
  │  4. Everything else: DENY (global default)│
  │                                           │
  │  [payment-api] ←→ [payment-worker]        │  ✓ Intra-NS
  │       │                    │               │
  │       │ port 8080          │ port 443      │
  │       ▼                    ▼               │
  └───────┼────────────────────┼───────────────┘
          │                    │
          ▼                    ▼
  Namespace: users (Team B)   External: api.stripe.com
  ┌───────────────────┐       ✓ FQDN egress
  │  [users-service]   │
  │  Ingress policy:   │
  │  ALLOW from        │
  │  payments/          │
  │  payment-api:8080  │
  └───────────────────┘

  Namespace: analytics (Team C)
  ┌───────────────────────────────────────────┐
  │  [analytics-worker]                        │
  │                                             │
  │  Cannot reach:                              │
  │  - payments namespace (DENIED by global)    │
  │  - users namespace (DENIED by global)       │
  │  - External internet (DENIED by global)     │
  │                                             │
  │  Can reach:                                 │
  │  - Other pods in analytics namespace        │
  │  - DNS (kube-system/kube-dns)              │
  └───────────────────────────────────────────┘
```

**Failure modes:**
- **Missing DNS egress rule in default deny:** All pods lose DNS resolution → all service-to-service calls fail. This is the most common mistake. Always include DNS allow in default deny policies.
- **Missing health check ingress:** kubelet health probes blocked → pods marked unhealthy → cascade failure. Include `fromEntities: host` in default deny.
- **Policy ordering conflict:** Two policies select the same pod with conflicting rules. Kubernetes NetworkPolicy is additive (ALLOW-only); CiliumNetworkPolicy supports explicit DENY with priority ordering. Always use explicit deny at the global level and allow at the namespace/workload level.
- **Admission webhook rejects valid policy:** Over-strict validation blocks legitimate policy changes. Solution: webhook has `failurePolicy: Fail` for security, but allow emergency bypass via a dedicated admin role.

**Interviewer Q&As:**

> **Q1: How does Kubernetes NetworkPolicy's "default deny" actually work?**
> A: It is NOT an explicit deny. When at least one NetworkPolicy selects a pod (via podSelector), the pod enters "isolated" mode. In isolated mode, only traffic explicitly allowed by some NetworkPolicy is permitted; everything else is implicitly denied. Pods with no NetworkPolicy selecting them are NOT isolated — all traffic is allowed. This means: you must create at least one policy per pod/namespace for any enforcement to occur.

> **Q2: Why is CiliumClusterwideNetworkPolicy better than per-namespace NetworkPolicy for default deny?**
> A: With Kubernetes NetworkPolicy, you must create a default-deny policy in EVERY namespace. If a new namespace is created without a default-deny policy, it is wide open. CiliumClusterwideNetworkPolicy applies to ALL namespaces automatically, including future namespaces. No manual step needed. This eliminates the "forgotten namespace" vulnerability.

> **Q3: How do you handle system namespaces (kube-system, monitoring) that need broad access?**
> A: Create specific CiliumClusterwideNetworkPolicy for system namespaces with broader egress rules. For example, Prometheus needs egress to all pods on metrics ports. CoreDNS needs ingress from all pods on port 53. These are explicitly defined and audited. The key: use the narrowest possible rules even for system namespaces (specify ports, not `toEntities: all`).

> **Q4: How do you prevent one team from creating a policy that allows traffic from another team's namespace?**
> A: Kubernetes RBAC: restrict NetworkPolicy creation to the team's own namespace. Teams cannot create policies in other namespaces. For cross-namespace policies (CiliumClusterwideNetworkPolicy), restrict creation to platform admins only. OPA/Kyverno admission policy: validate that NetworkPolicy `from` selectors do not reference labels outside the team's own namespace.

> **Q5: How do you test network policies before enforcing them?**
> A: (1) Cilium policy audit mode: `cilium_policy_audit_mode=true` — log violations without dropping traffic. (2) `cilium policy trace`: dry-run a specific flow against policies. (3) Deploy in a staging namespace first. (4) Hubble: observe actual traffic flows, verify all expected flows are allowed. (5) Integration tests: run end-to-end service tests with policies applied.

> **Q6: What is the performance impact of default deny with 500+ policies?**
> A: With eBPF (Cilium): negligible. The eBPF hash map lookup is O(1) regardless of policy count. A cluster with 500 policies compiles to a few thousand map entries, all O(1) lookups. With iptables (Calico): significant. 500 policies may generate 5,000+ iptables rules, each traversed linearly. At this scale, iptables mode should be migrated to eBPF (Calico supports eBPF dataplane since v3.13).

---

### Deep Dive 3: L7 Network Policy Enforcement

**Why it's hard:**
L3/L4 policies operate on packet headers (IP, port, protocol) and can be evaluated in the kernel. L7 policies (HTTP method/path, gRPC service/method) require parsing application-layer data, which is impossible in a stateless per-packet eBPF program. L7 enforcement requires reassembling TCP streams, parsing HTTP, and making policy decisions on the request level. This means proxying the traffic through a user-space process (Envoy), adding latency and complexity.

**Approaches:**

| Approach | L7 Capability | Performance | Complexity | Transparency |
|----------|--------------|-------------|------------|-------------|
| eBPF only (no L7) | None | Best | Low | Full |
| eBPF + Envoy redirect (Cilium) | HTTP, gRPC, Kafka | Good (+1-2ms) | Medium | Full |
| Service mesh sidecar (Istio) | HTTP, gRPC, TCP | Good (+1-2ms) | High | Full |
| Application-level middleware | Any protocol | Variable | Low | None (code change) |
| Kernel TLS + eBPF (experimental) | Limited | Best | Very high | Full |

**Selected approach: eBPF for L3/L4 + Envoy proxy redirect for L7 (Cilium's L7 proxy)**

**Implementation detail:**

```
L7 Policy Enforcement Architecture:

  Pod A                                                Pod B
  ┌──────┐                                           ┌──────┐
  │ App  │ HTTP GET /api/v1/users                     │ App  │
  │      │───────────────────────────────────────────►│      │
  └──────┘                                           └──────┘
      │                                                  ▲
      │ TC egress hook                                   │
      ▼                                                  │
  eBPF program (Pod A egress):                           │
      │                                                  │
      ├── L3/L4 check: dst identity=67890,              │
      │   port=8080, proto=TCP                           │
      │   → Policy entry has REDIRECT flag               │
      │                                                  │
      ▼                                                  │
  ┌──────────────────────────┐                          │
  │  Cilium Envoy Proxy      │                          │
  │  (per-node, shared)      │                          │
  │                           │                          │
  │  L7 policy evaluation:    │                          │
  │  ┌───────────────────┐   │                          │
  │  │ HTTP Parser        │   │                          │
  │  │ Method: GET        │   │                          │
  │  │ Path: /api/v1/users│   │                          │
  │  │ Headers: ...       │   │                          │
  │  └────────┬──────────┘   │                          │
  │           │               │                          │
  │  ┌────────▼──────────┐   │                          │
  │  │ L7 Policy Check   │   │                          │
  │  │                    │   │                          │
  │  │ Rule 1: ALLOW      │   │                          │
  │  │   method=GET       │   │                          │
  │  │   path=/api/v1/*   │   │                          │
  │  │   → MATCH ✓        │   │                          │
  │  │                    │   │                          │
  │  │ Rule 2: ALLOW      │   │                          │
  │  │   method=POST      │   │                          │
  │  │   path=/api/v1/    │   │                          │
  │  │     orders         │   │                          │
  │  │                    │   │                          │
  │  │ Default: DENY      │   │                          │
  │  └────────┬──────────┘   │                          │
  │           │               │                          │
  │  ALLOWED → forward to    ─┼──────────────────────────┘
  │  original destination     │
  │                           │
  │  DENIED → return HTTP 403 │
  │  Access denied             │
  └───────────────────────────┘

L7 Protocols Supported by Cilium:
  - HTTP/1.1: method, path, headers
  - HTTP/2 (gRPC): service, method
  - Kafka: topic, API key (produce, fetch, metadata)
  - DNS: query name, query type (A, AAAA, CNAME)
```

```yaml
# L7 CiliumNetworkPolicy Example
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: api-l7-policy
  namespace: production
spec:
  endpointSelector:
    matchLabels:
      app: api-server
  ingress:
    - fromEndpoints:
        - matchLabels:
            app: frontend
      toPorts:
        - ports:
            - port: "8080"
              protocol: TCP
          rules:
            http:
              # Allow GET on /api/v1/users and sub-paths
              - method: GET
                path: "/api/v1/users(/[a-zA-Z0-9-]+)?"
              # Allow POST on /api/v1/orders with JSON body
              - method: POST
                path: "/api/v1/orders"
                headers:
                  - "Content-Type: application/json"
              # Allow health check
              - method: GET
                path: "/healthz"
              # All other requests: DENIED (implicit)
    
    - fromEndpoints:
        - matchLabels:
            app: admin-dashboard
      toPorts:
        - ports:
            - port: "8080"
              protocol: TCP
          rules:
            http:
              # Admin can access all endpoints
              - method: ".*"
                path: "/.*"

# gRPC L7 Policy
---
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: grpc-l7-policy
  namespace: production
spec:
  endpointSelector:
    matchLabels:
      app: grpc-service
  ingress:
    - fromEndpoints:
        - matchLabels:
            app: grpc-client
      toPorts:
        - ports:
            - port: "50051"
              protocol: TCP
          rules:
            http:                   # gRPC uses HTTP/2
              - method: POST        # All gRPC calls are POST
                path: "/mypackage.MyService/GetUser"
              - method: POST
                path: "/mypackage.MyService/ListUsers"
              # Block: /mypackage.MyService/DeleteUser (not listed = denied)
```

**Failure modes:**
- **Envoy proxy crash:** L7-enforced flows are dropped (eBPF redirect target is gone). Cilium detects proxy health and can fall back to L3/L4-only enforcement. Alert on proxy restarts.
- **Envoy resource exhaustion:** Many L7-inspected flows = high proxy CPU/memory. Each L7 flow adds ~1-2ms latency and ~10 KB memory. At 10K L7-inspected flows/second, proxy needs ~1 CPU core and 512 MB memory. Solution: be selective about which flows get L7 inspection.
- **L7 policy regex backtracking:** Complex path regex can cause catastrophic backtracking. Solution: use RE2 (no backtracking) for path matching. Cilium uses Go's regexp package which is RE2-compatible.
- **HTTP/2 multiplexing through proxy:** Multiple gRPC RPCs on one TCP connection. Proxy must demultiplex, evaluate policy per-RPC, and re-multiplex. This is handled by Envoy's HTTP/2 codec but adds per-stream overhead.

**Interviewer Q&As:**

> **Q1: When should you use L7 policies vs L3/L4 policies?**
> A: Use L3/L4 for most traffic (faster, simpler, no proxy overhead). Use L7 only when you need fine-grained control: (1) Multiple API endpoints on the same port requiring different access controls. (2) Compliance requirements mandating method-level authorization. (3) Preventing data exfiltration via specific API paths. The rule of thumb: if port-level isolation is sufficient, do not use L7.

> **Q2: How does L7 enforcement interact with mTLS (Istio)?**
> A: If both Cilium L7 policy and Istio sidecar are present, there are TWO proxies in the path (Cilium Envoy + Istio Envoy). This is unnecessary overhead. Solution: use one or the other. If using Istio for mTLS and L7 routing, use Istio's AuthorizationPolicy for L7 enforcement. If using Cilium for networking, use Cilium's L7 policies. Some deployments use Cilium for L3/L4/L7 and skip Istio entirely.

> **Q3: Can L7 policies inspect encrypted traffic?**
> A: No. L7 inspection requires parsing plaintext HTTP. If traffic is mTLS-encrypted between pods, the Cilium proxy cannot inspect it without terminating TLS. Options: (1) Cilium terminates mTLS (acts as TLS endpoint). (2) Use Istio sidecar for mTLS + L7 (sidecar sees plaintext). (3) Apply L7 policies before mTLS encryption (in the source pod's sidecar).

> **Q4: What is the overhead of L7 enforcement per request?**
> A: Per-request overhead: ~1-2ms latency (proxy processing + TCP redirection). Throughput: Cilium's Envoy proxy can handle ~50,000 L7-inspected requests/second per node. Memory: ~10 KB per active L7 connection. CPU: ~0.1 cores per 10,000 L7 requests/second. These costs are only incurred for flows matching L7 policies; L3/L4-only flows bypass the proxy.

> **Q5: How do you handle WebSocket or streaming protocols with L7 policies?**
> A: WebSocket starts as an HTTP Upgrade request. The L7 proxy can inspect the initial HTTP request (method, path, headers) and make a policy decision. Once upgraded, the connection becomes raw TCP and is no longer L7-inspectable. For streaming gRPC (server-streaming, bidi), the L7 proxy inspects each message independently. Long-lived streams do not benefit from per-message policy (the initial call is authorized once).

> **Q6: How do you monitor L7 policy enforcement effectiveness?**
> A: Hubble provides L7-aware flow logs: `hubble observe --protocol http --http-status-code 403` shows L7 denials. Metrics: `hubble_drop_total{reason="policy_denied"}` for L3/L4 drops, `hubble_flows_processed_total{verdict="DROPPED",type="L7"}` for L7 drops. Cilium proxy stats: `cilium_proxy_requests_total`, `cilium_proxy_upstream_reply_seconds`. Dashboard: L7 allow/deny ratio per service, L7 latency overhead.

---

### Deep Dive 4: Policy-as-Code and GitOps

**Why it's hard:**
Network policies are security-critical infrastructure. Changes must be reviewed, validated, tested, and audited. In a multi-team environment, policy ownership, naming conventions, and conflict resolution must be managed. Applying policies directly via `kubectl apply` is error-prone and lacks an audit trail. GitOps provides version control, review workflows, and automated deployment, but requires tooling for policy validation and conflict detection.

**Approaches:**

| Approach | Version Control | Review | Validation | Automation | Audit |
|----------|----------------|--------|-----------|-----------|-------|
| kubectl apply (manual) | None | None | None | None | kubectl audit log |
| Kustomize + Git | Git | PR review | Manual | CI/CD | Git history |
| Helm charts + Git | Git | PR review | Helm lint | CI/CD | Git history |
| ArgoCD + OPA conftest | Git | PR review | Automated (conftest) | ArgoCD sync | Git + Argo history |
| Crossplane + Git | Git | PR review | Schema validation | Crossplane | Git + Crossplane |

**Selected approach: ArgoCD + OPA conftest + Kyverno admission webhook**

**Implementation detail:**

```
Policy-as-Code Pipeline:

  ┌─────────────────────────────────────────────────────────────────┐
  │                    GIT REPOSITORY                                │
  │                                                                   │
  │  policies/                                                        │
  │  ├── global/                                                      │
  │  │   ├── default-deny.yaml                                        │
  │  │   ├── allow-dns.yaml                                           │
  │  │   └── allow-monitoring.yaml                                    │
  │  ├── namespaces/                                                  │
  │  │   ├── payments/                                                │
  │  │   │   ├── intra-namespace.yaml                                 │
  │  │   │   ├── egress-stripe.yaml                                   │
  │  │   │   └── allow-from-frontend.yaml                             │
  │  │   ├── users/                                                   │
  │  │   │   ├── intra-namespace.yaml                                 │
  │  │   │   └── allow-from-payments.yaml                             │
  │  │   └── ...                                                      │
  │  └── tests/                                                       │
  │      ├── conftest/                                                │
  │      │   ├── policy_test.rego                                     │
  │      │   └── data.json                                            │
  │      └── connectivity-tests/                                      │
  │          └── test-payments-to-users.yaml                          │
  └─────────────────────────────────────────────────────────────────┘
        │
        ▼ Pull Request
  ┌─────────────────────────────────────────────────────────────────┐
  │                    CI PIPELINE                                    │
  │                                                                   │
  │  1. YAML Lint: yamllint + kubeval (schema validation)            │
  │                                                                   │
  │  2. OPA Conftest: policy-level validation                        │
  │     ┌──────────────────────────────────────────────────────┐     │
  │     │ conftest test policies/ --policy tests/conftest/      │     │
  │     │                                                       │     │
  │     │ Rules checked:                                        │     │
  │     │ - Every namespace has a default-deny policy            │     │
  │     │ - No policy allows ingress from 0.0.0.0/0             │     │
  │     │ - Egress to internet requires explicit FQDN list      │     │
  │     │ - L7 policies have path validation (no .* wildcards)  │     │
  │     │ - Policy names follow convention: <ns>-<direction>-*  │     │
  │     │ - No policy exceeds 50 rules (complexity limit)       │     │
  │     └──────────────────────────────────────────────────────┘     │
  │                                                                   │
  │  3. Conflict Detection:                                          │
  │     - Check for overlapping selectors with conflicting rules      │
  │     - Verify no two policies grant contradictory access           │
  │                                                                   │
  │  4. Dry-Run Apply:                                               │
  │     kubectl apply --dry-run=server -f policies/                  │
  │     (validates against K8s API schema)                           │
  │                                                                   │
  │  5. Connectivity Test (staging cluster):                         │
  │     Apply policies to staging → run cilium connectivity test     │
  │     → verify expected allow/deny verdicts                        │
  └─────────────────────────────────────────────────────────────────┘
        │
        ▼ PR Merged
  ┌─────────────────────────────────────────────────────────────────┐
  │                    ARGOCD SYNC                                    │
  │                                                                   │
  │  1. ArgoCD detects Git change                                    │
  │  2. Sync: apply policies to production cluster                   │
  │  3. Kyverno admission webhook validates each policy              │
  │  4. Cilium agent picks up new policies via K8s watch             │
  │  5. eBPF maps updated within 5 seconds                          │
  │  6. ArgoCD reports sync status (healthy/degraded)                │
  │  7. Hubble: monitor for unexpected DROPPED flows                 │
  └─────────────────────────────────────────────────────────────────┘
```

```rego
# OPA Conftest Policy: tests/conftest/policy_test.rego

package network_policy

# Every policy must have a description
deny[msg] {
    input.kind == "CiliumNetworkPolicy"
    not input.spec.description
    msg := sprintf("CiliumNetworkPolicy %s/%s must have a description",
                   [input.metadata.namespace, input.metadata.name])
}

# No policy should allow ingress from all IPs (0.0.0.0/0)
deny[msg] {
    input.kind == "NetworkPolicy"
    ingress := input.spec.ingress[_]
    from := ingress.from[_]
    from.ipBlock.cidr == "0.0.0.0/0"
    msg := sprintf("NetworkPolicy %s/%s allows ingress from 0.0.0.0/0",
                   [input.metadata.namespace, input.metadata.name])
}

# Egress to external internet must use toFQDNs (not toIPBlock 0.0.0.0/0)
deny[msg] {
    input.kind == "CiliumNetworkPolicy"
    egress := input.spec.egress[_]
    to := egress.toCIDR[_]
    to == "0.0.0.0/0"
    msg := sprintf("CiliumNetworkPolicy %s/%s uses toCIDR 0.0.0.0/0. Use toFQDNs instead.",
                   [input.metadata.namespace, input.metadata.name])
}

# L7 policies must not use .* wildcard paths in production
deny[msg] {
    input.kind == "CiliumNetworkPolicy"
    input.metadata.namespace != "staging"
    rule := input.spec.ingress[_].toPorts[_].rules.http[_]
    rule.path == "/.*"
    msg := sprintf("CiliumNetworkPolicy %s/%s has wildcard path '/.*' in production",
                   [input.metadata.namespace, input.metadata.name])
}

# Policy naming convention
deny[msg] {
    input.kind == "CiliumNetworkPolicy"
    not re_match("^[a-z0-9]+-(?:ingress|egress|intra|allow|deny)-", input.metadata.name)
    msg := sprintf("CiliumNetworkPolicy name '%s' does not match naming convention: <ns>-<direction>-<description>",
                   [input.metadata.name])
}
```

**Failure modes:**
- **ArgoCD sync failure:** Policies not applied. Existing policies remain (stale but safe). ArgoCD retries. Alert on sync failure.
- **OPA conftest false positive:** Valid policy rejected by overly strict rule. Developer blocked. Solution: conftest rules should have escape hatches (annotations to bypass specific checks with justification).
- **Kyverno webhook down:** Pod creation blocked (if `failurePolicy: Fail`). Solution: HA Kyverno with PDB; use `failurePolicy: Ignore` for non-critical validation (with logging).
- **Git repository compromise:** Attacker pushes malicious policy (allow all traffic). Mitigation: branch protection, required code review, signed commits, and ArgoCD RBAC (limit which repos/paths can sync to which namespaces).

**Interviewer Q&As:**

> **Q1: Why manage network policies in Git instead of directly via kubectl?**
> A: (1) Version control: every change is tracked with commit history, author, and timestamp. (2) Review workflow: PR-based review ensures four-eyes principle for security changes. (3) Validation: CI pipeline catches errors before production. (4) Rollback: `git revert` to undo a bad policy. (5) Audit: Git history satisfies compliance audit requirements. (6) Consistency: same policy set across staging and production.

> **Q2: How do you handle emergency policy changes that need immediate deployment?**
> A: Emergency path: (1) Apply policy directly via kubectl (bypassing Git). (2) Create a post-hoc PR documenting the emergency change. (3) ArgoCD detects drift (running state != Git state) and alerts. (4) Merge the PR to reconcile Git with cluster state. For faster GitOps: use a "fast-track" branch with auto-merge and minimal review requirements for security-critical changes.

> **Q3: How do you ensure every namespace has a default-deny policy?**
> A: Three layers: (1) CiliumClusterwideNetworkPolicy provides cluster-wide default deny (covers all namespaces automatically). (2) Kyverno admission policy: require a default-deny NetworkPolicy exists in every namespace before pods can be created. (3) CI conftest: validate that every namespace directory in Git contains a default-deny policy file.

> **Q4: How do you handle policy conflicts between teams?**
> A: Team A creates a policy in their namespace that allows traffic from Team B's namespace. Team B creates a policy in their namespace that denies egress to Team A. Conflict resolution: (1) Ingress policies are additive in Kubernetes NetworkPolicy (no conflict — more permissive wins at the target). (2) For Cilium with explicit deny: order field determines priority (lower order = higher priority). (3) Organizational: cross-team policies require both teams' approval in the PR.

> **Q5: How do you visualize the effective policy for a given pod?**
> A: (1) `cilium endpoint get <id>`: shows all policies applied to this endpoint with realized rules. (2) `cilium policy trace --src <id> --dst <id>`: trace specific flow. (3) Hubble UI: visual service map showing allow/deny flows between services. (4) Network policy visualization tools (Kiali, Cilium's policy map). (5) Compute "effective policy" by merging all applicable policies (namespace + cluster-wide) in a CI step and rendering as a readable table.

> **Q6: How do you roll back a network policy change that is causing an outage?**
> A: (1) ArgoCD: sync to previous Git commit (`argocd app sync <app> --revision <prev-commit>`). (2) kubectl: re-apply the previous version of the policy (`git show HEAD~1:policies/path/file.yaml | kubectl apply -f -`). (3) Emergency: delete the offending policy (`kubectl delete cnp <name> -n <ns>`). Deletion removes enforcement, reverting to the next applicable policy (global default). (4) Post-incident: root-cause analysis, improve CI validation to catch the issue.

---

## 7. Scaling Strategy

**Policy enforcement scaling:**
- eBPF: O(1) per packet regardless of policy count. No scaling concern.
- iptables: consider migration to eBPF at > 2,000 policies.
- Identity system: Cilium identities scale with unique label sets (typically << pod count).

**Hubble/flow log scaling:**
- Per-node ring buffer limits memory usage (configurable, default 4096 flows).
- Hubble Relay aggregates flows from all nodes.
- For large clusters: sample flow logs (1 in 10) or aggregate per-minute.
- Long-term storage: export to Elasticsearch with retention policy (7 days detailed, 90 days aggregated).

**Policy management scaling:**
- GitOps: separate repos per team or per namespace for large organizations.
- ArgoCD: ApplicationSets for managing policies across multiple namespaces.
- Kyverno/OPA: namespace-scoped policies for team autonomy.

**Cross-cluster scaling:**
- Cilium ClusterMesh: extend network policies across multiple clusters.
- Global policies replicated via federated Git repo.

**Interviewer Q&As:**

> **Q1: How does Cilium handle policy enforcement at 100,000 pods?**
> A: The eBPF datapath scales linearly with pod count (one eBPF program per pod veth, O(1) per packet). The control plane bottleneck is identity allocation and policy compilation. At 100K pods with 1,000 unique label sets = 1,000 identities. Policy compilation is per-identity, not per-pod. Cilium Operator distributes identities via kvstore (etcd). Ensure etcd has sufficient capacity (3+ nodes, SSDs).

> **Q2: What happens when you have 10,000 network policies?**
> A: With eBPF: each pod's policy map contains only the rules applicable to that pod (not all 10,000 policies). A pod that matches 5 policies might have 50 map entries — O(1) lookup. With iptables: 10,000 policies might generate 100,000 rules in a single chain — O(n) per packet. This is where iptables fundamentally cannot scale, and eBPF is essential.

> **Q3: How do you handle the identity map at scale?**
> A: Cilium's identity map (cilium_ipcache) is an LPM (Longest Prefix Match) trie in eBPF. At 100,000 pods, the map has 100,000 /32 entries. LPM trie lookup is O(32) for IPv4 (32 prefix levels). Memory: ~100K x 32 bytes = 3.2 MB per node. This is well within eBPF map limits. Cross-node identity sync via etcd adds ~1s latency for new identities.

> **Q4: How do you scale flow log storage?**
> A: At 4.5M pps cluster-wide, logging every packet is impossible (~450 TB/day). Instead: (1) Aggregate per-flow (source/dest/port/verdict → 1 log entry per flow, not per packet). (2) Sample: log 1 in 100 flows at steady state. (3) Log 100% of DROPPED flows (security events). (4) Use ClickHouse for time-series flow storage (columnar, compressed, fast queries). (5) Retention: 7 days full fidelity, 90 days aggregated.

> **Q5: How do you handle policy enforcement during node scaling?**
> A: When a new node joins: (1) Cilium agent DaemonSet pod starts. (2) Agent syncs identity map from etcd. (3) Agent compiles policies for local endpoints. (4) eBPF programs are loaded. This takes 10-30 seconds. During this window, pods on the new node have no policy enforcement. Mitigation: use Kubernetes node NotReady taint until Cilium is ready (Cilium supports this via `--set agent.waitForKubeProxy=true`).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Cilium agent crash (per-node) | DaemonSet restart, Kubernetes node event | eBPF programs persist (enforcement continues with stale policy) | Auto-restart; eBPF survives agent restart | Policy stale until agent restarts (< 30s) |
| Cilium Operator crash | Deployment restart, identity allocation fails | No new identity allocation; existing policies unaffected | HA Operator (2+ replicas, leader election) | < 30s |
| etcd (Cilium kvstore) down | etcd health check, Cilium agent logs | No cross-node identity sync; new pods on other nodes get WORLD identity | HA etcd (3+ nodes); Cilium caches identities locally | Degraded until etcd recovers |
| eBPF program rejected by verifier | Cilium agent logs error | Old eBPF program continues (no enforcement update) | Fix program; unlikely in production (programs are pre-verified) | Immediate (old program active) |
| eBPF map full | Cilium metrics (map utilization) | New policy entries cannot be added; default deny drops unmatched traffic | Increase map size; reduce policy complexity | Immediate once map resized |
| Kyverno webhook down | Pod creation timeout or bypassed | New pods created without policy validation | HA Kyverno; failurePolicy: Ignore (log-only) | < 30s (pod restart) |
| ArgoCD sync failure | ArgoCD status, Slack alert | Policies not updated from Git | Retry; manual sync; existing policies unaffected | Manual intervention |
| Cilium Envoy proxy crash (L7) | Proxy health check, L7 flow drops | L7-inspected traffic dropped | Proxy auto-restart; fall back to L3/L4 enforcement | < 10s |
| Node network partition | Node NotReady, Cilium health | Node's identity cache stale; remote pods may see stale identities | Cilium uses local cache; reconnects on heal | Stale until partition heals |
| Git repo compromise | Signed commits, branch protection alerts | Malicious policies deployed | Mandatory code review; signed commits; ArgoCD RBAC | Manual rollback |
| DNS-based egress policy stale | DNS TTL expiry monitoring | Pod connecting to IP that is no longer valid for allowed FQDN | Re-resolve DNS on TTL expiry; short DNS TTL | DNS TTL duration |

---

## 9. Security

**Least Privilege:**
- Default deny cluster-wide: no inter-namespace traffic without explicit policy.
- Egress to internet: blocked by default; requires explicit FQDN-based policy.
- Pods cannot bypass policy: eBPF programs are loaded by a privileged agent; pods cannot unload them.
- No `allowAll` policies in production (enforced by admission webhook).

**Policy Integrity:**
- Network policies are Kubernetes CRDs stored in etcd with encryption at rest.
- GitOps pipeline: all changes require PR review and CI validation.
- Signed Git commits for non-repudiation.
- Kyverno validates policy syntax and business rules at admission time.

**Runtime Security:**
- eBPF programs are verified by the kernel verifier (memory safety, termination).
- Cilium agent runs with minimal capabilities (`NET_ADMIN`, `SYS_MODULE`).
- Agent binary is signed and verified.
- Hubble flow logs capture security events (dropped traffic) for SIEM integration.

**Compliance:**
- PCI DSS: network segmentation between cardholder data environment (CDE) and other namespaces.
- SOC 2: all network flow logs retained for 90 days.
- HIPAA: PHI workloads in dedicated namespace with strict ingress/egress policies.
- Audit trail: Git history for policy changes, Hubble for enforcement evidence.

**Attack Surface:**
- eBPF programs in the kernel: verified but still kernel-level code. Monitor CVEs in kernel BPF subsystem.
- Cilium agent API (local socket): restricted to localhost, no external access.
- Hubble Relay gRPC API: authenticated with mTLS, RBAC for who can observe flows.

---

## 10. Incremental Rollout

**Phase 1 (Weeks 1-2): Observability First**
- Deploy Cilium with default-allow policy (observe-only).
- Enable Hubble for network flow visibility.
- Build flow dashboards: who talks to whom, which ports, which namespaces.
- Identify existing traffic patterns before enforcing policies.

**Phase 2 (Weeks 3-4): Audit Mode**
- Deploy default-deny policy in AUDIT mode (log violations, do not block).
- Generate allow policies from observed traffic (Hubble flow data).
- Review audit logs: identify expected traffic that needs explicit allow policies.

**Phase 3 (Weeks 5-8): Namespace Isolation**
- Enable default-deny enforcement for non-critical namespaces (staging, dev).
- Create intra-namespace allow policies for each namespace.
- Create cross-namespace allow policies based on audit data.
- Monitor for dropped traffic; adjust policies as needed.

**Phase 4 (Weeks 9-12): Production Enforcement**
- Enable default-deny for production namespaces (one at a time).
- Start with the least-connected namespaces (fewer dependencies).
- Each namespace activation: deploy allow policies, monitor Hubble for drops, iterate.
- Complete: all namespaces under policy enforcement.

**Phase 5 (Weeks 13-16): L7 and Advanced Policies**
- Add L7 policies for sensitive API endpoints (payment processing, user data).
- Add FQDN-based egress policies for external service access.
- Enable policy-as-code pipeline (GitOps).
- Enable Kyverno admission webhook for policy validation.

**Rollout Q&As:**

> **Q1: How do you generate initial network policies from existing traffic?**
> A: (1) Deploy Hubble and collect flow data for 2-4 weeks (capture all traffic patterns). (2) Use `hubble observe --output json` to export flows. (3) Analyze flows to identify: source namespace/label → destination namespace/label → port. (4) Generate CiliumNetworkPolicy YAML from the flow data (automated scripts or tools like "Cilium Network Policy Editor"). (5) Review generated policies with service owners before applying.

> **Q2: How do you handle false positives (legitimate traffic blocked by new policies)?**
> A: (1) Start with audit mode: log drops without blocking. Review logs for 1-2 weeks. (2) Gradual rollout: one namespace at a time. (3) Hubble alerts on new DROPPED flows: investigate each one. (4) "Break glass" emergency procedure: delete the offending policy to immediately restore traffic. (5) Post-incident: update the policy to include the missing allow rule.

> **Q3: How do you handle services that are called by many other services (e.g., authentication service)?**
> A: Create a label-based policy that allows ingress from all pods with a specific label (e.g., `needs-auth: "true"`). Add this label to pods that should call the auth service. This is more maintainable than listing every caller individually. For truly "public" internal services, create an allow-all-cluster-internal ingress rule for that service.

> **Q4: How do you test network policies in a staging environment?**
> A: (1) Apply policies to staging cluster (separate from production). (2) Run Cilium connectivity tests (`cilium connectivity test`) — automated test suite that validates connectivity. (3) Run application integration tests with policies applied. (4) Compare staging and production Hubble flows to verify policy coverage. (5) Use `cilium policy trace` to verify specific expected-allow and expected-deny flows.

> **Q5: How do you communicate policy changes to application teams?**
> A: (1) Policy changes are PRs in Git — teams are tagged as reviewers for policies affecting their namespace. (2) Slack notifications on policy changes to affected namespaces. (3) Dashboard showing "Policy Coverage" per namespace (% of services with explicit policies). (4) Regular security review meetings to discuss policy changes and traffic pattern changes. (5) Self-service policy generator tool for teams to create policies from templates.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Trade-off |
|----------|-------------------|--------|-----------|-----------|
| CNI | Calico, Cilium, Antrea, Flannel | Cilium | eBPF for O(1) policy evaluation, L7 support, Hubble observability, identity model | More complex than Calico; newer project |
| Datapath | iptables, eBPF, OVS | eBPF (Cilium) | O(1) per-packet, survives agent restart, L7 redirect | Requires kernel 5.4+; eBPF expertise needed |
| Default deny scope | Per-namespace (K8s NetworkPolicy), cluster-wide (Cilium) | Cluster-wide (CiliumClusterwideNetworkPolicy) | Covers all namespaces automatically including future ones | Cilium-specific (not portable) |
| L7 enforcement | Cilium L7, Istio AuthorizationPolicy, custom proxy | Cilium L7 | Single CNI for L3/L4/L7; no separate service mesh needed | Envoy proxy overhead for L7 flows |
| Policy management | kubectl, Helm, ArgoCD + conftest | ArgoCD + conftest + Kyverno | GitOps for audit and review; conftest for validation; Kyverno for admission | Three tools to operate |
| Flow logging | iptables LOG, eBPF perf events, Hubble | Hubble (eBPF perf events) | Identity-aware flows, Prometheus metrics, UI visualization | Cilium-specific; must adapt if changing CNI |
| Policy validation | Manual review only, OPA conftest, Kyverno | Both (conftest in CI, Kyverno at admission) | Defense in depth: CI catches errors early, admission catches runtime violations | Two validation systems to maintain |
| FQDN egress | IP-based CIDR, FQDN-based | FQDN-based (Cilium toFQDNs) | More precise than IP ranges (cloud IPs change); easier to understand | Depends on DNS interception; slight latency |

---

## 12. Agentic AI Integration

### AI-Powered Network Policy Management

**Policy Generation Agent:**
- Analyzes Hubble flow data to learn actual communication patterns.
- Generates least-privilege network policies automatically.
- Identifies: which services call which, on which ports, with which frequency.
- Outputs: CiliumNetworkPolicy YAML with human-readable comments.
- Example: "Observed 10,000 flows from payments/payment-api to users/users-service on port 8080 over 7 days. Generating ingress allow policy for users-service."

**Anomaly Detection Agent:**
- Monitors network flows in real-time for unusual patterns.
- Detects: (1) New communication paths not seen in baseline (lateral movement?). (2) Sudden traffic volume changes between services. (3) Connections to unusual external IPs. (4) Port scanning patterns.
- Alerts security team with context: "Pod analytics-worker-7 in namespace analytics is attempting to connect to payments/database-pod on port 5432. This communication path was never observed in the 30-day baseline. This matches a potential data exfiltration pattern."

**Policy Compliance Agent:**
- Continuously verifies that deployed policies meet compliance requirements.
- Checks: (1) PCI DSS: CDE namespace has strict isolation. (2) All inter-namespace traffic is encrypted (mTLS). (3) No egress to unapproved external endpoints. (4) All production namespaces have default-deny.
- Generates compliance reports automatically.
- Alerts on drift: "Namespace `payments` is missing a default-deny ingress policy. PCI DSS requirement 1.3.4 violated."

**Incident Response Agent:**
- When a security incident is detected (compromised pod):
  - Automatically creates a quarantine policy (deny all ingress/egress for the compromised pod).
  - Preserves flow logs for forensic analysis.
  - Notifies security team with timeline of the pod's network activity.
  - Recommends: "Pod `web-server-5` in namespace `frontend` connected to unusual external IP 185.x.x.x on port 4444 (reverse shell pattern). Quarantine policy applied. Review last 24 hours of flow logs."

**Policy Optimization Agent:**
- Identifies overly permissive policies and suggests tightening.
- Analyzes: actual traffic vs. policy allows. If a policy allows port 8080 and 9090, but only 8080 is ever used, suggest removing 9090.
- Identifies unused policies (policies that have not matched any traffic in 30 days).
- Recommends: "CiliumNetworkPolicy `allow-legacy-api` in namespace `users` has not matched any traffic in 45 days. Consider removing."

### Integration Architecture

```
┌────────────────────────────────────────────────────┐
│           Network Policy AI Agent                    │
│                                                      │
│  Data sources:                                       │
│  ├── Hubble flows (real-time gRPC stream)            │
│  ├── Prometheus metrics (flow aggregates)            │
│  ├── Kubernetes API (policies, pods, services)       │
│  ├── Git repo (policy definitions, PR history)       │
│  └── Security feeds (threat intelligence)            │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │ Policy   │  │ Anomaly  │  │ Compliance       │  │
│  │ Generator│  │ Detector │  │ Checker          │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────────────┘  │
│       └──────────────┼──────────────┘                │
│                      ▼                               │
│              ┌──────────────┐                        │
│              │ Action Queue │                        │
│              │              │                        │
│              │ - Generate   │                        │
│              │   policy PR  │                        │
│              │ - Quarantine │                        │
│              │   pod        │                        │
│              │ - Alert      │                        │
│              │   security   │                        │
│              └──────────────┘                        │
│                                                      │
│  Safety:                                             │
│  - Policy generation creates PR (not auto-apply)     │
│  - Quarantine requires human confirmation             │
│    (except for known-bad patterns)                   │
│  - Never removes allow rules automatically            │
│  - All agent actions logged with reasoning            │
└────────────────────────────────────────────────────┘
```

---

## 13. Complete Interviewer Q&A Bank

> **Q1: Explain how Kubernetes NetworkPolicy works. What are its limitations?**
> A: NetworkPolicy selects pods via podSelector and defines allowed ingress/egress rules. It is additive (ALLOW-only; no explicit DENY). Limitations: (1) No cluster-wide default deny (must create per-namespace). (2) No L7 support (HTTP method/path). (3) No FQDN-based egress. (4) No audit mode (dry-run). (5) No policy ordering/priority. (6) Depends on CNI for enforcement (Flannel does not support NetworkPolicy at all). Cilium/Calico CRDs address all these limitations.

> **Q2: What is the difference between Calico iptables mode and eBPF mode?**
> A: iptables mode: Calico Felix agent programs iptables rules on each node. Rules are in the FORWARD chain with per-endpoint chains. Performance: O(n) per packet where n = number of rules. eBPF mode (Calico 3.13+): Felix programs eBPF maps attached to TC hooks. Performance: O(1) per packet (hash map lookup). eBPF mode also provides connection tracking and flow logging. Upgrade path: switch via Calico configuration; no pod restart needed.

> **Q3: How does Cilium's identity-based model differ from IP-based models?**
> A: IP-based (iptables): rules reference specific IPs (`-s 10.244.1.15 -j ACCEPT`). When pods reschedule (new IP), rules must be updated. 100 pods = 100 rules. Cilium identity-based: pods with the same labels share an identity (e.g., 12345). Policy maps reference identities (`{identity=12345, port=8080} → ALLOW`). 100 identical pods = 1 map entry. Pod reschedule only updates the ipcache (IP → identity mapping), not the policy map. Dramatically better scaling.

> **Q4: How do you implement namespace isolation for multi-tenancy?**
> A: (1) Cluster-wide default deny (CiliumClusterwideNetworkPolicy with empty endpointSelector). (2) Per-namespace intra-namespace allow (allow pods within same namespace to communicate). (3) Explicit cross-namespace policies for authorized inter-team traffic. (4) Kubernetes RBAC: teams can only create policies in their own namespace. (5) OPA admission: validate that cross-namespace references are authorized by both teams.

> **Q5: What is the "default deny" gotcha in Kubernetes NetworkPolicy?**
> A: In Kubernetes, pods are NOT isolated until at least one NetworkPolicy selects them. Creating a policy that says "allow from frontend" implicitly denies all other ingress to the selected pods. But pods not selected by ANY policy allow ALL traffic. This means: you must explicitly create a default-deny policy that selects all pods in the namespace. Without it, pods have no enforcement. This is the most common misconfiguration.

> **Q6: How do you handle egress control for pods that need internet access?**
> A: Three levels: (1) Block all internet egress by default (CiliumClusterwideNetworkPolicy with no toEntities: world). (2) For pods that need specific external APIs: use `toFQDNs` to allow by hostname (`api.stripe.com`). Cilium intercepts DNS and dynamically creates IP-based rules. (3) For pods that need broad internet access (e.g., a web scraper): create explicit CIDR-based egress with logging. All egress to internet must be justified and reviewed.

> **Q7: How does eBPF policy enforcement survive a Cilium agent restart?**
> A: eBPF programs are loaded into the kernel and run independently of the user-space agent. When Cilium agent restarts, the eBPF programs (attached to TC hooks on veth interfaces) continue executing with the last-programmed policy maps. No traffic disruption. When the agent comes back, it reconciles the maps with current desired state. This is a fundamental advantage over iptables, where the rules are user-space-managed and can be lost.

> **Q8: How do you handle network policies for StatefulSets with stable identities?**
> A: StatefulSet pods have stable names (pod-0, pod-1) but may have different IPs across rescheduling. With Cilium's identity model, all pods with the same labels share an identity. For policies targeting specific StatefulSet pods: use additional labels (e.g., `statefulset.kubernetes.io/pod-name: pod-0`). This gives a unique identity per StatefulSet pod, allowing fine-grained policies (e.g., only pod-0 can be the leader that accepts write traffic).

> **Q9: What is the performance overhead of network policy enforcement?**
> A: eBPF (Cilium): < 50us per first packet of a flow (policy map lookup + conntrack creation). < 10us per subsequent packet (conntrack hit, skip policy). Throughput impact: < 2-3% vs. no enforcement. iptables (Calico): 50-500us per first packet depending on rule count. 10-50us per subsequent packet (conntrack). Throughput impact: 5-15% at 1000+ rules. L7 enforcement (Envoy proxy): 1-2ms per request. Throughput: ~50K requests/second per node.

> **Q10: How do you handle network policies for pods with multiple containers?**
> A: Network policies apply to the pod (not individual containers). All containers in a pod share the same network namespace and IP. If container A listens on port 8080 and container B listens on port 9090, the policy can allow/deny each port independently. But you cannot have different policies for different containers within the same pod. For container-level isolation, use separate pods.

> **Q11: How do you handle network policies for hostNetwork pods?**
> A: hostNetwork pods bypass the pod network namespace (they share the node's network). Standard NetworkPolicy does not apply because there is no veth interface to attach policy to. Cilium supports host-level policies (`fromEntities: host` / `toEntities: host`), but fine-grained per-pod policy is not possible for hostNetwork pods. For strict isolation: avoid hostNetwork; use NodePort or HostPort instead.

> **Q12: What is the relationship between NetworkPolicy and security groups (AWS/GCP)?**
> A: They operate at different layers. Security groups (cloud): L3/L4 rules at the VPC/subnet level, applied to ENIs (network interfaces). They do not understand Kubernetes pods or labels. NetworkPolicy (Kubernetes): L3/L4/L7 rules at the pod level, applied by the CNI within the cluster. They complement each other: security groups protect the cluster perimeter; NetworkPolicy protects pod-to-pod traffic within the cluster. Both are needed for defense-in-depth.

> **Q13: How do you validate that network policies are actually being enforced?**
> A: (1) `cilium connectivity test`: automated test suite that creates test pods and verifies expected allow/deny. (2) Hubble: observe actual DROPPED flows and verify they match expected denials. (3) Penetration testing: attempt connections that should be denied and verify they are blocked. (4) `cilium policy trace`: trace specific flows against policies. (5) Metrics: `cilium_drop_count_total{reason="POLICY_DENIED"}` should be > 0 (indicating enforcement is active).

> **Q14: How do you handle the "pod startup race" where policy is not yet enforced?**
> A: During pod startup, there is a brief window before Cilium programs the eBPF maps. During this window: Cilium's default behavior is to DROP all traffic (secure default). The pod cannot communicate until its identity is assigned and policies are programmed. This typically takes 1-3 seconds. Kubernetes readiness probes should have `initialDelaySeconds` sufficient to account for this. Cilium's `--set policyEnforcementMode=always` ensures strict enforcement from the start.

> **Q15: How do you handle network policies across cluster boundaries (multi-cluster)?**
> A: Cilium ClusterMesh connects multiple clusters and extends identities across them. Pods in cluster-A can reference identities from cluster-B in policies. Example: allow traffic from `app=frontend` in cluster-B to `app=api` in cluster-A. Implementation: (1) etcd-based identity sharing across clusters. (2) Tunnel or native routing for cross-cluster pod traffic. (3) Cluster-aware network policies (CiliumNetworkPolicy with cluster-scoped identities).

> **Q16: What is the difference between Cilium's CiliumNetworkPolicy and CiliumClusterwideNetworkPolicy?**
> A: CiliumNetworkPolicy is namespace-scoped: it can only select pods within its own namespace. CiliumClusterwideNetworkPolicy is cluster-scoped: it can select pods across all namespaces. Use cluster-wide for: default deny, monitoring/logging access, DNS access. Use namespace-scoped for: intra-namespace rules, cross-namespace ingress (applied in the destination namespace). Cluster-wide policies have higher priority by default.

---

## 14. References

- **Kubernetes NetworkPolicy:** https://kubernetes.io/docs/concepts/services-networking/network-policies/
- **Cilium Network Policy:** https://docs.cilium.io/en/stable/security/policy/
- **Cilium eBPF Datapath:** https://docs.cilium.io/en/stable/concepts/ebpf/
- **Cilium Identity Model:** https://docs.cilium.io/en/stable/concepts/networking/identity/
- **Hubble Observability:** https://docs.cilium.io/en/stable/gettingstarted/hubble/
- **Calico Network Policy:** https://docs.tigera.io/calico/latest/network-policy/
- **Calico eBPF Datapath:** https://docs.tigera.io/calico/latest/operations/ebpf/
- **Antrea Network Policy:** https://antrea.io/docs/main/docs/antrea-network-policy/
- **OPA Conftest:** https://www.conftest.dev/
- **Kyverno Policy Engine:** https://kyverno.io/
- **eBPF Documentation:** https://ebpf.io/
- **Linux TC (Traffic Control):** https://man7.org/linux/man-pages/man8/tc.8.html
