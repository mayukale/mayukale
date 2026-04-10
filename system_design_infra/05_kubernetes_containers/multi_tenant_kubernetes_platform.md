# System Design: Multi-Tenant Kubernetes Platform

> **Relevance to role:** Most infrastructure teams serve multiple internal teams on shared Kubernetes clusters. A cloud infrastructure platform engineer must design tenancy models that balance isolation, efficiency, and operational simplicity — enforcing security boundaries, resource fairness, and cost attribution while keeping infrastructure utilization high. This is where platform engineering meets security engineering.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Isolate tenants at network, resource, security, and API access levels |
| FR-2 | Enforce resource quotas (CPU, memory, storage, object counts) per tenant |
| FR-3 | Implement RBAC with tenant-scoped roles and service accounts |
| FR-4 | Enforce network policies (default-deny inter-tenant traffic) |
| FR-5 | Apply admission policies (image registries, pod security, resource limits) per tenant |
| FR-6 | Provide self-service namespace provisioning within guardrails |
| FR-7 | Attribute infrastructure costs to tenants |
| FR-8 | Support tenant onboarding and offboarding workflows |
| FR-9 | Provide tenant-scoped observability (metrics, logs, dashboards) |
| FR-10 | Enable per-tenant upgrade/maintenance windows |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Tenant count | 100+ tenants per cluster |
| NFR-2 | Noisy neighbor impact | < 5% performance degradation for other tenants |
| NFR-3 | Tenant onboarding time | < 30 minutes (automated) |
| NFR-4 | Policy enforcement latency | < 100 ms (admission webhook) |
| NFR-5 | Cost attribution accuracy | > 95% of infrastructure costs attributed |
| NFR-6 | RBAC evaluation time | < 5 ms per authorization check |
| NFR-7 | Cluster availability | 99.99% |
| NFR-8 | Audit trail retention | 90 days minimum |

### Constraints & Assumptions
- Shared control plane (all tenants share API server, scheduler, etcd).
- Tenants are internal engineering teams (not external customers — see vCluster for that).
- Compliance requirements: SOC2, PCI-DSS for some tenants.
- Kubernetes 1.28+ with Pod Security Standards.
- CNI: Cilium (L7 network policies) or Calico.
- Policy engine: OPA/Gatekeeper or Kyverno.

### Out of Scope
- External multi-tenancy (SaaS-like, untrusted tenants) — requires stronger isolation (vCluster/cluster-per-tenant).
- Application-level multi-tenancy (tenant isolation within an application).
- Billing system integration (mentioned but not primary focus).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|------------|--------|
| Tenants | 100 internal teams | 100 |
| Namespaces | 100 tenants x 3 namespaces avg (dev, staging, prod) | 300 |
| Users | 100 teams x 10 engineers avg | 1,000 |
| Pods (total) | 300 namespaces x 100 pods avg | 30,000 |
| Services | 300 namespaces x 20 services avg | 6,000 |
| Network policies | 300 namespaces x 5 policies avg | 1,500 |
| RBAC objects | 100 tenants x (Role + RoleBinding + SA) x 3 namespaces | ~900 |
| ResourceQuota objects | 300 namespaces x 1 each | 300 |
| Admission webhook calls | ~500 create/update/s x policy evaluation | 500 QPS |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| RBAC evaluation | < 5 ms | Cached in API server |
| Admission webhook (OPA/Gatekeeper) | < 50 ms | Policy evaluation |
| Namespace creation (full provisioning) | < 60 s | Namespace + RBAC + quota + network policy |
| Cost report generation | < 5 min | Aggregate metrics over billing period |
| Audit log query | < 10 s | Elasticsearch/Loki query |

### Storage Estimates

| Component | Calculation | Result |
|-----------|------------|--------|
| RBAC objects in etcd | 900 objects x 2 KB | ~1.8 MB |
| Network policies | 1,500 x 3 KB | ~4.5 MB |
| ResourceQuotas | 300 x 1 KB | ~300 KB |
| Audit logs | 1,000 users x 50 req/day x 1 KB x 90 days | ~4.5 GB |
| Prometheus metrics (per-tenant) | 100 tenants x 1,000 time-series x 5 B/sample x 86,400 samples/day | ~43 GB/day |

### Bandwidth Estimates

| Flow | Calculation | Result |
|------|------------|--------|
| Admission webhook traffic | 500 QPS x 5 KB payload | ~2.5 MB/s |
| Audit log shipping | 50,000 events/day x 1 KB | ~0.6 KB/s |
| Metrics scraping (per-tenant) | 100 tenants x 500 KB/scrape x 1 scrape/15s | ~3.3 MB/s |
| Cost data export | 100 tenants x 10 MB/report x 1/hour | ~0.3 MB/s |

---

## 3. High Level Architecture

```
                   ┌────────────────────────────────────────────────┐
                   │                Platform Team                    │
                   │  (manages cluster, policies, tenant lifecycle)  │
                   └────────────┬───────────────────────────────────┘
                                │
                   ┌────────────▼───────────────────────────────────┐
                   │            Shared Kubernetes Cluster            │
                   │                                                │
                   │  ┌──────────────────────────────────────────┐  │
                   │  │         Control Plane (shared)            │  │
                   │  │  API Server → RBAC → Admission Webhooks  │  │
                   │  │  etcd (encrypted at rest)                 │  │
                   │  │  Scheduler → Controller Manager           │  │
                   │  └──────────────────────────────────────────┘  │
                   │                                                │
                   │  ┌──────────────────────────────────────────┐  │
                   │  │      Policy Enforcement Layer             │  │
                   │  │  ┌──────────────┐  ┌─────────────────┐  │  │
                   │  │  │OPA/Gatekeeper│  │Pod Security Adm.│  │  │
                   │  │  │(constraints) │  │(Restricted mode)│  │  │
                   │  │  └──────────────┘  └─────────────────┘  │  │
                   │  └──────────────────────────────────────────┘  │
                   │                                                │
                   │  ┌──────────┐ ┌──────────┐ ┌──────────────┐  │
                   │  │ Team A   │ │ Team B   │ │ Team C       │  │
                   │  │ (ns: a-*)│ │ (ns: b-*)│ │ (ns: c-*)   │  │
                   │  │          │ │          │ │              │  │
                   │  │ ┌──────┐ │ │ ┌──────┐ │ │ ┌──────────┐│  │
                   │  │ │ RBAC │ │ │ │ RBAC │ │ │ │ RBAC     ││  │
                   │  │ │ Quota│ │ │ │ Quota│ │ │ │ Quota    ││  │
                   │  │ │ NetPol│ │ │ │ NetPol│ │ │ │ NetPol  ││  │
                   │  │ │ LimitR│ │ │ │ LimitR│ │ │ │ LimitR  ││  │
                   │  │ └──────┘ │ │ └──────┘ │ │ └──────────┘│  │
                   │  │          │ │          │ │              │  │
                   │  │ Pods...  │ │ Pods...  │ │ Pods...     │  │
                   │  └──────────┘ └──────────┘ └──────────────┘  │
                   │                                                │
                   │  ┌──────────────────────────────────────────┐  │
                   │  │         Shared Infrastructure             │  │
                   │  │  ┌──────────┐ ┌───────────┐ ┌─────────┐│  │
                   │  │  │ Cilium   │ │ Prometheus│ │ Logging ││  │
                   │  │  │ (CNI+NP) │ │ + Thanos  │ │ (Loki)  ││  │
                   │  │  └──────────┘ └───────────┘ └─────────┘│  │
                   │  │  ┌──────────┐ ┌───────────┐            │  │
                   │  │  │ Ingress  │ │ Cost      │            │  │
                   │  │  │ (shared) │ │ Attribution│            │  │
                   │  │  └──────────┘ └───────────┘            │  │
                   │  └──────────────────────────────────────────┘  │
                   └────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Shared Control Plane** | Single API server, scheduler, etcd serving all tenants |
| **RBAC** | Namespace-scoped Roles/RoleBindings restricting tenant access |
| **ResourceQuota** | Hard limits on CPU, memory, storage, object counts per namespace |
| **LimitRange** | Default and max container resource requests/limits per namespace |
| **NetworkPolicy** | Default-deny inter-tenant traffic; allow only declared flows |
| **OPA/Gatekeeper** | Policy-as-code enforcement (image registries, labels, security) |
| **Pod Security Admission** | Enforce Restricted/Baseline security standards per namespace |
| **Tenant Controller** | Automates namespace provisioning, RBAC, quota, policy setup |
| **Cost Attribution** | Tracks resource usage per tenant for chargeback |

### Data Flows

1. **Tenant onboarding:** Platform team creates Tenant CR → Tenant controller creates namespace(s) → applies RBAC, ResourceQuota, LimitRange, NetworkPolicy, labels → team gets kubeconfig scoped to their namespace.

2. **Workload deployment:** Team member runs kubectl apply → API server authenticates (OIDC) → authorizes (RBAC: team-a can deploy to ns-team-a-prod?) → mutating admission (inject labels, set defaults) → validating admission (OPA: image from approved registry? resources within limits?) → stored in etcd → scheduled.

3. **Inter-tenant request:** Pod in team-a tries to reach pod in team-b → Cilium evaluates NetworkPolicy → if no policy allows cross-namespace traffic → packet dropped.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# Tenant Custom Resource (managed by tenant controller)
apiVersion: platform.example.com/v1
kind: Tenant
metadata:
  name: team-alpha
spec:
  displayName: "Alpha Team"
  owner: "alice@company.com"
  costCenter: "CC-1234"
  namespaces:
    - name: team-alpha-dev
      environment: development
      quotaProfile: small
    - name: team-alpha-staging
      environment: staging
      quotaProfile: medium
    - name: team-alpha-prod
      environment: production
      quotaProfile: large
  users:
    - name: alice@company.com
      role: admin       # Can manage all resources in tenant namespaces
    - name: bob@company.com
      role: developer   # Can deploy and debug
    - name: carol@company.com
      role: viewer      # Read-only access
  groups:
    - name: team-alpha-oncall
      role: admin
  policies:
    allowedImageRegistries:
      - "registry.company.com"
      - "gcr.io/company-project"
    podSecurityStandard: restricted
    networkIsolation: strict       # default-deny all cross-namespace
    allowedStorageClasses:
      - "standard"
      - "fast-ssd"
    maxPodCount: 500               # Across all namespaces
  nodePool: general                # Schedule to shared or dedicated pool
status:
  phase: Active
  namespaces:
    - name: team-alpha-dev
      status: Ready
    - name: team-alpha-staging
      status: Ready
    - name: team-alpha-prod
      status: Ready
  resourceUsage:
    cpu: "45.5"          # Total CPU used across all namespaces
    memory: "120Gi"
    pods: 234
    costThisMonth: "$12,450"

---
# Quota profiles (reusable templates)
apiVersion: platform.example.com/v1
kind: QuotaProfile
metadata:
  name: small
spec:
  resourceQuota:
    requests.cpu: "10"
    requests.memory: "20Gi"
    limits.cpu: "20"
    limits.memory: "40Gi"
    pods: "50"
    services: "20"
    persistentvolumeclaims: "10"
    services.loadbalancers: "1"
  limitRange:
    defaultRequest:
      cpu: "100m"
      memory: "128Mi"
    default:
      cpu: "500m"
      memory: "512Mi"
    max:
      cpu: "4"
      memory: "8Gi"
    min:
      cpu: "50m"
      memory: "64Mi"

---
# Generated: ResourceQuota per namespace
apiVersion: v1
kind: ResourceQuota
metadata:
  name: tenant-quota
  namespace: team-alpha-prod
  labels:
    platform.example.com/tenant: team-alpha
    platform.example.com/managed: "true"
spec:
  hard:
    requests.cpu: "50"
    requests.memory: "100Gi"
    limits.cpu: "100"
    limits.memory: "200Gi"
    pods: "200"
    services: "50"
    persistentvolumeclaims: "50"
    services.loadbalancers: "3"

---
# Generated: LimitRange per namespace
apiVersion: v1
kind: LimitRange
metadata:
  name: tenant-limits
  namespace: team-alpha-prod
  labels:
    platform.example.com/tenant: team-alpha
spec:
  limits:
    - type: Container
      default:
        cpu: "500m"
        memory: "512Mi"
      defaultRequest:
        cpu: "100m"
        memory: "128Mi"
      max:
        cpu: "8"
        memory: "16Gi"
      min:
        cpu: "50m"
        memory: "64Mi"

---
# Generated: Default-deny NetworkPolicy
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: default-deny-all
  namespace: team-alpha-prod
  labels:
    platform.example.com/tenant: team-alpha
    platform.example.com/managed: "true"
spec:
  podSelector: {}
  policyTypes:
    - Ingress
    - Egress
  ingress: []     # Deny all ingress
  egress: []      # Deny all egress

---
# Generated: Allow DNS egress
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: allow-dns
  namespace: team-alpha-prod
spec:
  podSelector: {}
  policyTypes:
    - Egress
  egress:
    - to: []
      ports:
        - protocol: UDP
          port: 53
        - protocol: TCP
          port: 53

---
# Generated: Allow intra-namespace traffic
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: allow-same-namespace
  namespace: team-alpha-prod
spec:
  podSelector: {}
  policyTypes:
    - Ingress
  ingress:
    - from:
        - podSelector: {}    # Any pod in same namespace

---
# Generated: RBAC for tenant admin
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: tenant-admin
  namespace: team-alpha-prod
rules:
- apiGroups: ["", "apps", "batch", "networking.k8s.io"]
  resources: ["*"]
  verbs: ["*"]
- apiGroups: [""]
  resources: ["resourcequotas", "limitranges"]
  verbs: ["get", "list"]    # Can view but not modify platform-managed resources
---
apiVersion: rbac.authorization.k8s.io/v1
kind: RoleBinding
metadata:
  name: tenant-admin-binding
  namespace: team-alpha-prod
subjects:
- kind: User
  name: alice@company.com
  apiGroup: rbac.authorization.k8s.io
- kind: Group
  name: team-alpha-oncall
  apiGroup: rbac.authorization.k8s.io
roleRef:
  kind: Role
  name: tenant-admin
  apiGroup: rbac.authorization.k8s.io
```

### Database Selection

| Data Type | Storage | Justification |
|-----------|---------|---------------|
| Tenant CRDs | etcd (via API server) | Native k8s CRD; watched by tenant controller |
| RBAC objects | etcd | Native k8s resources |
| Cost metrics | Prometheus + Thanos (long-term) | Time-series data; PromQL for aggregation |
| Audit logs | Elasticsearch / Loki | Full-text search; 90-day retention |
| Tenant metadata (owner, cost center) | Tenant CR labels + annotations | Queryable via kubectl |

### Indexing Strategy

| Index | Purpose |
|-------|---------|
| `platform.example.com/tenant` label on all resources | Find all resources belonging to a tenant |
| `platform.example.com/environment` label | Filter by environment (dev/staging/prod) |
| `platform.example.com/managed` label | Identify platform-managed resources (not to be modified by tenants) |
| Namespace name prefix (e.g., `team-alpha-*`) | Quick visual identification of tenant ownership |
| Audit log index by user/namespace/timestamp | Query who did what and when |

---

## 5. API Design

### REST/gRPC/kubectl Endpoints

**Tenant Lifecycle:**

```bash
# Platform team creates tenant
kubectl apply -f tenant-alpha.yaml

# List all tenants
kubectl get tenants

# Inspect tenant
kubectl describe tenant team-alpha

# Tenant self-service: view own resources
kubectl get all -n team-alpha-prod

# Tenant self-service: check quota usage
kubectl describe resourcequota tenant-quota -n team-alpha-prod

# Platform team: view cross-tenant resource usage
kubectl get resourcequotas -A -l platform.example.com/managed=true

# Platform team: check which tenants use the most resources
kubectl top pods -A --sort-by=cpu | head -50
```

**OPA/Gatekeeper Policy Management:**

```bash
# Create constraint template (platform team)
kubectl apply -f allowed-registries-template.yaml

# Create constraint (applies to all tenant namespaces)
kubectl apply -f - <<EOF
apiVersion: constraints.gatekeeper.sh/v1beta1
kind: K8sAllowedRegistries
metadata:
  name: require-approved-registries
spec:
  match:
    kinds:
      - apiGroups: [""]
        kinds: ["Pod"]
    namespaceSelector:
      matchExpressions:
        - key: platform.example.com/tenant
          operator: Exists
  parameters:
    registries:
      - "registry.company.com"
      - "gcr.io/company-project"
      - "docker.io/library"
EOF

# Check policy violations
kubectl get k8sallowedregistries require-approved-registries -o yaml
# Shows .status.violations[] with details
```

### CLI Design (Platform CLI)

```bash
# Custom platform CLI (kubectl plugin)
kubectl platform tenant create team-alpha \
  --owner=alice@company.com \
  --cost-center=CC-1234 \
  --quota-profile=medium \
  --environments=dev,staging,prod

kubectl platform tenant list
kubectl platform tenant describe team-alpha
kubectl platform tenant delete team-alpha --confirm

# Cost attribution
kubectl platform cost report --tenant=team-alpha --period=2026-03
kubectl platform cost report --all --period=2026-03 --format=csv

# Policy management
kubectl platform policy list
kubectl platform policy audit --tenant=team-alpha  # Show violations

# Tenant user management
kubectl platform tenant add-user team-alpha bob@company.com --role=developer
kubectl platform tenant remove-user team-alpha bob@company.com
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Tenancy Models

**Why it's hard:**
There is no single "right" tenancy model — each approach trades isolation strength against resource efficiency and operational complexity. The choice depends on trust level, compliance requirements, and team size. Getting this wrong means either wasting resources on over-isolation or exposing tenants to each other's failures and security breaches.

**Approaches Compared:**

| Model | Isolation Level | Resource Efficiency | Operational Complexity | Cost per Tenant | Use Case |
|-------|----------------|--------------------|-----------------------|-----------------|----------|
| **Namespace-per-tenant** | Medium (shared kernel, shared CP) | High (dense packing) | Low | Lowest | Trusted internal teams |
| **vCluster** | High (virtual CP, shared data plane) | Medium | Medium | Medium | Semi-trusted teams, dev environments |
| **Cluster-per-tenant** | Highest (full isolation) | Low (duplicated CP) | High | Highest | Untrusted tenants, regulated environments |
| **Hierarchical namespaces** | Medium (hierarchical RBAC) | High | Low-Medium | Low | Large organizations with team hierarchy |
| **Capsule** | Medium-High (namespace groups) | High | Medium | Low | Multi-tenant SaaS on k8s |

**Selected: Namespace-per-tenant (primary) + vCluster (for enhanced isolation)**

**Justification:** For internal teams, namespace-per-tenant provides sufficient isolation with RBAC, NetworkPolicy, ResourceQuota, and Pod Security Standards — without the operational overhead of managing hundreds of clusters. vCluster provides an escape hatch for teams needing stronger control plane isolation (custom CRDs, custom admission policies) without the cost of a full cluster.

**Namespace-per-Tenant Implementation Detail:**

```
Isolation Layers (defense in depth):

Layer 1: API Access (RBAC)
  ┌─────────────────────────────────────────────────┐
  │ Team A can ONLY access namespaces: team-a-*     │
  │ Team A CANNOT see/modify team-b-* resources     │
  │ Team A CANNOT create cluster-scoped resources   │
  │ Team A CANNOT modify ResourceQuota/LimitRange   │
  │ Team A CANNOT modify NetworkPolicies (managed)  │
  └─────────────────────────────────────────────────┘

Layer 2: Network (NetworkPolicy + Cilium)
  ┌─────────────────────────────────────────────────┐
  │ Default: deny all inter-namespace traffic       │
  │ Allow: intra-namespace traffic (same tenant)    │
  │ Allow: DNS (port 53 to kube-system)             │
  │ Allow: ingress controller → tenant pods         │
  │ Deny: tenant pods → control plane               │
  │ Optional: Cilium L7 policies (HTTP path-based)  │
  └─────────────────────────────────────────────────┘

Layer 3: Resources (Quota + LimitRange)
  ┌─────────────────────────────────────────────────┐
  │ ResourceQuota: max CPU, memory, pods per NS     │
  │ LimitRange: default and max per-container limits│
  │ Prevention: tenant cannot exhaust cluster        │
  └─────────────────────────────────────────────────┘

Layer 4: Security (PSA + OPA/Gatekeeper)
  ┌─────────────────────────────────────────────────┐
  │ Pod Security Standard: Restricted               │
  │   - No privileged containers                    │
  │   - No host networking                          │
  │   - runAsNonRoot required                       │
  │   - Read-only root filesystem required          │
  │   - Seccomp profile required                    │
  │ OPA policies:                                   │
  │   - Approved image registries only              │
  │   - Required labels (team, cost-center)         │
  │   - Max replica count per deployment            │
  │   - No NodePort services (use Ingress)          │
  │   - No hostPath volumes                         │
  └─────────────────────────────────────────────────┘

Layer 5: Node Isolation (optional, for sensitive tenants)
  ┌─────────────────────────────────────────────────┐
  │ Dedicated node pool with taint:                 │
  │   tenant=team-alpha:NoSchedule                  │
  │ Team A pods have matching toleration            │
  │ No other tenant's pods can schedule here        │
  │ Use case: PCI-DSS, HIPAA compliance             │
  └─────────────────────────────────────────────────┘
```

**vCluster for Enhanced Isolation:**

```
vCluster Architecture:
  Management Cluster (host)
  ├── Namespace: vcluster-team-gamma
  │   ├── StatefulSet: vcluster (runs virtual API server + virtual etcd)
  │   ├── Pod: vcluster-0 (virtual control plane)
  │   └── Syncer: syncs virtual pods ↔ host pods
  │
  └── Actual pods run in host namespace but are managed by virtual CP

Benefits:
  - Team gets "full cluster admin" within their vCluster
  - Can install custom CRDs, operators, admission webhooks
  - Isolated API server = no RBAC conflicts with other tenants
  - Host cluster network policies still apply

Trade-offs:
  - Each vCluster consumes ~1 CPU, 512 MB RAM (virtual CP)
  - Syncer adds latency (~100ms for pod operations)
  - Limited to single namespace on host (no cross-namespace resources)
  - Node-level resources (DaemonSets) require special handling
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Tenant exceeds quota | New pods rejected with quota exceeded error | Clear error messages; quota usage alerts at 80% |
| Network policy misconfiguration | Tenant traffic blocked or exposed | Default-deny base; platform-managed policies; audit regularly |
| RBAC escalation (tenant creates ClusterRoleBinding) | Cross-tenant access | Admission policy: deny cluster-scoped resource creation by tenants |
| Noisy neighbor (CPU) | Other tenants experience latency | LimitRange enforces per-container limits; dedicated node pools for critical tenants |
| Noisy neighbor (IO) | Disk-heavy workload degrades co-located pods | io.max cgroup controls (k8s 1.31+); dedicated storage classes |
| Secret leakage (tenant accesses other tenant's secrets) | Data breach | RBAC: Secrets access scoped to namespace; encryption at rest |
| Tenant controller bug | Namespaces created without proper policies | Admission webhook validates that all required policies exist before allowing pods |

**Interviewer Q&As:**

**Q1: When would you choose cluster-per-tenant over namespace-per-tenant?**
A: Cluster-per-tenant when: (1) Tenants are untrusted (external customers, third-party vendors). (2) Regulatory requirements mandate physical/logical isolation (PCI-DSS Level 1, FedRAMP High). (3) Tenants need cluster-admin access (install CRDs, operators, admission webhooks). (4) Blast radius must be zero — one tenant's failure cannot affect any other. (5) Different Kubernetes versions per tenant. Trade-off: 3-5x higher infrastructure cost (duplicated control planes) and much higher operational complexity.

**Q2: How does vCluster provide stronger isolation than namespaces without the cost of full clusters?**
A: vCluster runs a lightweight virtual control plane (k3s or k8s API server + SQLite/etcd) inside a single pod in the host cluster. Tenants interact with the virtual API server — they see an empty cluster that they fully control. Pods are actually scheduled on the host cluster via a syncer component. This provides: (1) API-level isolation (tenant's CRDs, RBAC, admission don't affect others). (2) Full admin access within the vCluster. (3) Shared worker nodes (efficient). But lacks: kernel-level isolation (still shared kernel), and requires the syncer to keep virtual and host state in sync.

**Q3: How do you prevent a tenant from consuming all resources on a node (noisy neighbor)?**
A: Multiple layers: (1) **ResourceQuota**: limits total namespace-level resource consumption. (2) **LimitRange**: sets per-container defaults and maximums — a single pod cannot request 64 GB on a 64 GB node. (3) **Priority classes**: system pods and critical tenants get higher priority; BestEffort tenant pods are evicted first. (4) **Dedicated node pools**: taint + toleration pins sensitive tenants to dedicated nodes. (5) **Topology spread**: ensure tenant pods are spread across nodes, not concentrated. (6) **Bandwidth limiting**: use Cilium bandwidth annotations (`kubernetes.io/egress-bandwidth`).

**Q4: How do you handle the case where a tenant needs to communicate with another tenant's service?**
A: Explicit cross-tenant NetworkPolicies: (1) Both tenants must agree (bilateral). (2) Platform team creates a NetworkPolicy in tenant B's namespace allowing ingress from tenant A's namespace (using namespaceSelector). (3) Service exposure: tenant B creates an ExternalName Service or the platform provides a shared service mesh. (4) Alternative: use an API gateway or ingress controller as the boundary — tenants communicate via external APIs, not internal pod IPs. This provides an audit point and rate limiting.

**Q5: How do you enforce that every tenant namespace has the required policies?**
A: (1) **Tenant controller**: automatically creates ResourceQuota, LimitRange, NetworkPolicy when namespace is created. (2) **Admission webhook**: rejects pod creation in any namespace missing required labels/policies (prevents "naked" namespaces). (3) **Gatekeeper constraint**: requires specific resources to exist in the namespace before pods can be scheduled. (4) **Drift detection**: controller periodically verifies policies exist and match the template; recreates if deleted. (5) **Hierarchical namespaces (HNC)**: parent namespace policies automatically propagate to child namespaces.

**Q6: How do you handle cluster-scoped resources in a multi-tenant environment?**
A: Cluster-scoped resources (ClusterRoles, PriorityClasses, IngressClasses, StorageClasses) are shared. Controls: (1) Tenants cannot create/modify cluster-scoped resources (RBAC). (2) Platform team manages shared resources with labels indicating which tenants can use them. (3) Admission policies enforce that tenants only reference approved StorageClasses, IngressClasses, etc. (4) PriorityClasses: platform defines a fixed set (system-critical, production, development) — tenants select but cannot create. (5) CRDs: tenants cannot install CRDs on a shared cluster — use vCluster if needed.

---

### Deep Dive 2: RBAC Design for Multi-Tenancy

**Why it's hard:**
RBAC must be fine-grained enough to prevent cross-tenant access while permissive enough for tenants to be productive. It must handle: multiple user roles per tenant, service accounts for CI/CD, automation permissions, and the boundary between platform-managed and tenant-managed resources.

**Implementation Detail:**

```yaml
# Role hierarchy per tenant:

# 1. Platform Admin (ClusterRole — manages all tenants)
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRole
metadata:
  name: platform-admin
rules:
- apiGroups: ["*"]
  resources: ["*"]
  verbs: ["*"]
# Bound to: platform-team group via ClusterRoleBinding

# 2. Tenant Admin (Role — full access within tenant namespaces)
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: tenant-admin
  namespace: team-alpha-prod
rules:
- apiGroups: ["", "apps", "batch", "autoscaling", "networking.k8s.io", "policy"]
  resources: ["*"]
  verbs: ["*"]
- apiGroups: [""]
  resources: ["resourcequotas", "limitranges"]
  verbs: ["get", "list"]  # View-only for platform-managed resources
# CANNOT: create NetworkPolicies with platform.example.com/managed label
# CANNOT: modify labels starting with platform.example.com/

# 3. Tenant Developer (Role — deploy and debug)
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: tenant-developer
  namespace: team-alpha-prod
rules:
- apiGroups: ["", "apps", "batch"]
  resources: ["pods", "deployments", "statefulsets", "jobs", "cronjobs",
              "services", "configmaps", "secrets"]
  verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]
- apiGroups: [""]
  resources: ["pods/log", "pods/exec", "pods/portforward"]
  verbs: ["get", "create"]
- apiGroups: [""]
  resources: ["events"]
  verbs: ["get", "list"]
- apiGroups: ["autoscaling"]
  resources: ["horizontalpodautoscalers"]
  verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]

# 4. Tenant Viewer (Role — read-only)
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: tenant-viewer
  namespace: team-alpha-prod
rules:
- apiGroups: ["", "apps", "batch", "autoscaling"]
  resources: ["*"]
  verbs: ["get", "list", "watch"]

# 5. CI/CD Service Account (Role — deploy only, no exec)
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: tenant-ci-deployer
  namespace: team-alpha-prod
rules:
- apiGroups: ["apps"]
  resources: ["deployments", "statefulsets"]
  verbs: ["get", "list", "watch", "update", "patch"]
- apiGroups: [""]
  resources: ["configmaps", "secrets"]
  verbs: ["get", "list", "create", "update", "patch"]
# Deliberately NO exec or port-forward permissions
```

**Authentication Integration (OIDC):**

```
API Server Configuration:
  --oidc-issuer-url=https://auth.company.com/realms/k8s
  --oidc-client-id=kubernetes
  --oidc-username-claim=email
  --oidc-groups-claim=groups

OIDC Token claims:
  {
    "email": "alice@company.com",
    "groups": ["team-alpha", "team-alpha-oncall"],
    "exp": 1712700000
  }

Mapping to RBAC:
  User "alice@company.com" + Group "team-alpha"
  → RoleBinding in team-alpha-prod binds group "team-alpha" to Role "tenant-admin"
  → Alice can manage resources in team-alpha-prod namespace

Token lifecycle:
  - Short-lived tokens (1 hour) issued by OIDC provider
  - kubectl uses kubelogin plugin for automatic token refresh
  - No long-lived static tokens or certificates for users
```

**Interviewer Q&As:**

**Q1: How do you prevent RBAC escalation where a tenant admin creates a RoleBinding that grants them cluster-admin?**
A: (1) RBAC has a built-in escalation check: a user cannot create a RoleBinding that references a ClusterRole they don't already have permissions for. (2) Specifically: to bind a Role/ClusterRole, the user must have all permissions in that Role. So a tenant-admin cannot bind `cluster-admin` because they don't have cluster-wide permissions. (3) Additional defense: OPA/Gatekeeper policy that rejects RoleBindings referencing ClusterRoles not in an approved list. (4) Audit log: alert on any RoleBinding creation referencing cluster-admin or system:* roles.

**Q2: How do you manage RBAC at scale (100 tenants x 3 namespaces x 4 roles)?**
A: (1) **Tenant controller**: automates RBAC creation from Tenant CR spec. Role templates are parameterized. (2) **Aggregated ClusterRoles**: define fine-grained ClusterRoles with aggregation labels; compose tenant roles from building blocks. (3) **Group-based bindings**: bind to OIDC groups, not individual users. User management happens in the IdP, not in k8s. (4) **RBAC audit**: periodically query all RoleBindings, flag unused bindings (no recent API activity from bound subjects). (5) **rbac-lookup** tool: quickly check what permissions a user/group has.

**Q3: How do you handle service account tokens for CI/CD pipelines?**
A: (1) Create a dedicated ServiceAccount per tenant per namespace (e.g., `team-alpha-ci`). (2) Use bound service account tokens (audience and time-scoped, k8s 1.22+). (3) Token request API: `kubectl create token team-alpha-ci --duration=1h`. (4) CI/CD system (Jenkins, GitHub Actions) stores the token as a secret. (5) Alternative: use OIDC-based authentication for CI/CD (GitHub OIDC → k8s OIDC, no token to store). (6) Never use default service account — it has no permissions in a properly configured cluster.

**Q4: How do you audit who accessed what in a multi-tenant cluster?**
A: Kubernetes API audit logging captures every request: (1) Configure audit policy: log all write operations at Request level (include request body), reads at Metadata level. (2) Include: username, groups, source IP, verb, resource, namespace, response code, timestamp. (3) Ship audit logs to Elasticsearch/Loki. (4) Dashboards: per-tenant activity, failed authorization attempts, unusual patterns (off-hours access, privilege escalation attempts). (5) Alert on: 403 spikes (tenant trying to access unauthorized resources), RoleBinding changes, Secret access to non-tenant namespaces.

---

### Deep Dive 3: Policy Enforcement (OPA/Gatekeeper)

**Why it's hard:**
Admission policies must be comprehensive (cover all attack vectors), correct (no false positives that block legitimate workloads), performant (< 100ms evaluation), and auditable (which policy rejected which resource and why). They must also evolve with the platform without breaking existing workloads.

**Approaches Compared:**

| Engine | Language | Mutation Support | Audit | Performance | k8s Integration |
|--------|---------|-----------------|-------|-------------|-----------------|
| OPA/Gatekeeper | Rego | Yes (mutation beta) | Yes (audit controller) | Good | ConstraintTemplate CRD |
| Kyverno | YAML (declarative) | Yes (native) | Yes | Good | ClusterPolicy CRD |
| Kubewarden | Multiple (Wasm) | Yes | Yes | Excellent (Wasm) | ClusterAdmissionPolicy CRD |
| Vanilla webhook | Any language | Yes | Must build | Varies | MutatingWebhookConfiguration |

**Selected: OPA/Gatekeeper (primary) with Kyverno considered for simpler policies**

**Justification:** Gatekeeper's ConstraintTemplate model separates policy definition (Rego) from policy instantiation (Constraint), enabling platform teams to write reusable policies and tenants to customize parameters. Audit mode enables dry-run before enforcement.

**Implementation Detail:**

```yaml
# ConstraintTemplate: reusable policy definition
apiVersion: templates.gatekeeper.sh/v1
kind: ConstraintTemplate
metadata:
  name: k8sallowedregistries
spec:
  crd:
    spec:
      names:
        kind: K8sAllowedRegistries
      validation:
        openAPIV3Schema:
          type: object
          properties:
            registries:
              type: array
              items:
                type: string
  targets:
    - target: admission.k8s.gatekeeper.sh
      rego: |
        package k8sallowedregistries
        
        violation[{"msg": msg}] {
          container := input.review.object.spec.containers[_]
          not startswith_any(container.image, input.parameters.registries)
          msg := sprintf("Image '%v' is not from an approved registry. Approved: %v",
                         [container.image, input.parameters.registries])
        }
        
        violation[{"msg": msg}] {
          container := input.review.object.spec.initContainers[_]
          not startswith_any(container.image, input.parameters.registries)
          msg := sprintf("Init image '%v' is not from an approved registry.",
                         [container.image])
        }
        
        startswith_any(str, prefixes) {
          prefix := prefixes[_]
          startswith(str, prefix)
        }

---
# Constraint: instantiated policy (applied to tenant namespaces)
apiVersion: constraints.gatekeeper.sh/v1beta1
kind: K8sAllowedRegistries
metadata:
  name: approved-registries
spec:
  enforcementAction: deny    # or "warn" or "dryrun"
  match:
    kinds:
      - apiGroups: [""]
        kinds: ["Pod"]
    namespaceSelector:
      matchExpressions:
        - key: platform.example.com/tenant
          operator: Exists
    excludedNamespaces:
      - kube-system
      - gatekeeper-system
      - monitoring
  parameters:
    registries:
      - "registry.company.com"
      - "gcr.io/company-project"
      - "docker.io/library"

---
# More policy examples:

# Require specific labels on all pods
apiVersion: constraints.gatekeeper.sh/v1beta1
kind: K8sRequiredLabels
metadata:
  name: require-team-label
spec:
  match:
    kinds:
      - apiGroups: [""]
        kinds: ["Pod"]
    namespaceSelector:
      matchLabels:
        platform.example.com/managed: "true"
  parameters:
    labels:
      - key: "app"
      - key: "team"

# Disallow NodePort services
apiVersion: constraints.gatekeeper.sh/v1beta1
kind: K8sBlockNodePort
metadata:
  name: no-nodeport
spec:
  match:
    kinds:
      - apiGroups: [""]
        kinds: ["Service"]
    namespaceSelector:
      matchExpressions:
        - key: platform.example.com/tenant
          operator: Exists

# Maximum replica count
apiVersion: constraints.gatekeeper.sh/v1beta1
kind: K8sMaxReplicas
metadata:
  name: max-replicas-50
spec:
  match:
    kinds:
      - apiGroups: ["apps"]
        kinds: ["Deployment", "StatefulSet"]
    namespaceSelector:
      matchLabels:
        platform.example.com/environment: development
  parameters:
    maxReplicas: 50
```

**Policy Rollout Strategy:**

```
1. dryrun → Audit violations without blocking
   enforcementAction: dryrun
   - View: kubectl get constraint <name> -o yaml → .status.violations[]
   
2. warn → Show warnings to users, don't block
   enforcementAction: warn
   - Users see: Warning: Image is not from approved registry
   
3. deny → Block non-compliant resources
   enforcementAction: deny
   - Returns: admission webhook denied the request: ...
   
4. Exemptions for migration:
   - Use Config resource to exempt specific namespaces/processes
   - Gradually remove exemptions as workloads are migrated
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Gatekeeper webhook down | If failurePolicy=Fail: all pod creation blocked | HA deployment (3 replicas); failurePolicy=Ignore as temporary fallback |
| Policy too restrictive | Legitimate workloads rejected | Always start with dryrun; gradual enforcement |
| Policy too permissive | Security gap | Audit mode catches violations; periodic policy review |
| Rego bug | Wrong policy evaluation | Unit test Rego policies (OPA test framework); integration tests |
| High webhook latency | Pod creation latency increases | Cache OPA decisions; limit policy complexity; monitor latency |

**Interviewer Q&As:**

**Q1: How do you test OPA policies before deploying them?**
A: (1) **Unit tests**: OPA has a built-in test framework (`opa test`). Write test cases with input/expected output. (2) **Conftest**: validate k8s manifests against policies locally before kubectl apply. (3) **Gatekeeper audit mode**: deploy with `enforcementAction: dryrun`, monitor `.status.violations` for false positives. (4) **CI pipeline**: every policy change goes through PR review, unit tests, and dry-run against staging cluster. (5) **Policy scoring**: track violation counts over time — sudden spikes indicate policy issues.

**Q2: How do you handle policy exceptions for specific workloads?**
A: (1) Gatekeeper Config resource: exempt specific namespaces, processes, or groups. (2) Constraint match excludeNamespaces: exclude specific namespaces from a policy. (3) Pod-level annotation: `gatekeeper.sh/exception: "reason"` — requires a mutating webhook to add. (4) Separate Constraint for excepted workloads with relaxed parameters. (5) Important: all exceptions must be documented, time-bounded, and reviewed quarterly. Track via labels on the exception resources.

**Q3: How does Kyverno differ from OPA/Gatekeeper?**
A: (1) Language: Kyverno uses YAML-native policies (no Rego) — lower learning curve. (2) Mutation: Kyverno has first-class mutation support (Gatekeeper mutation is beta). (3) Generation: Kyverno can generate resources (e.g., auto-create NetworkPolicy when namespace is created). (4) Trade-off: Rego is more powerful for complex logic; YAML policies are easier for simple rules. (5) Both support audit mode, dry-run, and exception handling. (6) Choose Kyverno if team is YAML-native; Gatekeeper if policies require complex logic.

---

### Deep Dive 4: Cost Attribution

**Why it's hard:**
Kubernetes resources are shared (nodes, network, storage) making it difficult to attribute costs to specific tenants. Over-provisioning (requests > actual usage) creates "reservation waste." Shared infrastructure (control plane, monitoring, ingress) must be distributed fairly. Chargeback models must be accurate enough for internal billing but simple enough to be understood.

**Implementation Detail:**

```
Cost Attribution Model:

1. Direct costs (attributable to tenant):
   CPU:     sum(container_resource_requests_cpu_cores{namespace=~"team-alpha-.*"})
   Memory:  sum(container_resource_requests_memory_bytes{namespace=~"team-alpha-.*"})
   Storage: sum(kubelet_volume_stats_capacity_bytes{namespace=~"team-alpha-.*"})
   Network: sum(container_network_transmit_bytes_total{namespace=~"team-alpha-.*"})

2. Attribution methods:
   a. Request-based (recommended for fairness):
      Cost = (CPU_requests / total_CPU_allocatable) * total_CPU_cost
           + (Mem_requests / total_Mem_allocatable) * total_Mem_cost
           + Storage_PVC_capacity * $/GB
      
      Why requests not limits: requests represent reserved capacity;
      limits represent burst potential. Charging on requests incentivizes
      right-sizing.

   b. Usage-based (reflects actual consumption):
      Cost = actual_CPU_hours * $/CPU-hour
           + actual_RAM_GB-hours * $/GB-hour
           + Storage_used * $/GB
      
      Problem: idle capacity is "free" — incentivizes over-provisioning
      to get first access during contention.

   c. Hybrid (recommended):
      Cost = max(request_based_cost, usage_based_cost) * 0.7
           + shared_infra_cost_share * 0.3
      
      Shared infra = control plane + monitoring + ingress + CNI overhead
      Distributed proportionally to tenant's resource footprint.

3. Tools:
   - Kubecost: open-source cost allocation for k8s
   - OpenCost: CNCF project for real-time cost monitoring
   - Custom: Prometheus queries + Grafana dashboard

4. Metrics for cost:
   
   Per-tenant CPU cost (monthly):
   sum(
     kube_pod_container_resource_requests{resource="cpu", namespace=~"team-alpha-.*"}
   ) * hours_in_month * cpu_cost_per_hour
   
   Per-tenant memory cost (monthly):
   sum(
     kube_pod_container_resource_requests{resource="memory", namespace=~"team-alpha-.*"}
   ) / 1073741824 * hours_in_month * gb_memory_cost_per_hour
   
   Per-tenant storage cost (monthly):
   sum(
     kube_persistentvolumeclaim_resource_requests_storage_bytes{namespace=~"team-alpha-.*"}
   ) / 1073741824 * gb_storage_cost_per_month
```

**Interviewer Q&As:**

**Q1: How do you handle cost attribution for shared resources (ingress, monitoring, DNS)?**
A: Three approaches: (1) **Proportional allocation**: distribute shared costs proportionally to each tenant's direct resource footprint. If team-alpha uses 10% of cluster CPU requests, they pay 10% of shared infra costs. (2) **Fixed overhead**: flat fee per tenant for shared services (e.g., $500/month for monitoring access). (3) **Usage-based for specific shared services**: if ingress controller tracks requests per namespace, charge proportionally. (4) In practice: proportional allocation for infrastructure, usage-based for metered services.

**Q2: How do you incentivize tenants to right-size their resource requests?**
A: (1) Charge based on requests, not usage — wasted requests cost money. (2) Provide VPA recommendations: show "you're requesting 4 CPU but using 0.5 CPU". (3) Efficiency dashboards: show request-to-usage ratio per tenant. (4) Anomaly alerts: notify when a tenant's utilization drops below 20%. (5) Periodic right-sizing reports sent to team leads. (6) Gamification: rank teams by efficiency, publicly acknowledge top performers.

---

## 7. Scheduling & Resource Management

### Multi-Tenant Scheduling

```
Priority Classes (platform-defined, not tenant-modifiable):

  system-critical (1000000000):
    - Control plane components
    - CNI, CSI, monitoring agents
    
  production-high (1000000):
    - Production workloads with SLA
    - Preempts lower-priority pods
    
  production-standard (100000):
    - Standard production workloads
    
  development (1000):
    - Dev/staging workloads
    - Preempted by production
    
  batch (100):
    - Batch jobs, CI/CD
    - First to be preempted

Node Pool Strategy:
  
  shared-general:    # All tenants can schedule here
    taints: none
    labels: {pool: general}
    
  shared-memory:     # Memory-optimized workloads
    taints: none
    labels: {pool: memory-optimized}
    
  dedicated-pci:     # PCI-DSS tenant only
    taints: [{key: tenant, value: pci-team, effect: NoSchedule}]
    labels: {pool: pci, tenant: pci-team}
    # Only pci-team pods with matching toleration can schedule here
```

---

## 8. Scaling Strategy

### Scaling the Multi-Tenant Platform

| Dimension | Strategy | Limit |
|-----------|---------|-------|
| Tenants per cluster | Namespace-per-tenant | ~200 tenants (etcd, RBAC overhead) |
| Pods per tenant | ResourceQuota | Configurable per tenant |
| Policies | Gatekeeper constraints | ~500 constraints (audit performance) |
| RBAC rules | Aggregated ClusterRoles | ~10,000 bindings before noticeable auth latency |
| Beyond 200 tenants | Multiple clusters with fleet management | CAPI + ArgoCD |

### Interviewer Q&As

**Q1: At what point should you split a multi-tenant cluster into multiple clusters?**
A: Split when: (1) Tenant count exceeds ~200 (RBAC and quota objects add significant etcd overhead). (2) Total pod count exceeds ~100,000 (scheduler and endpoint controller strain). (3) Compliance requires physical isolation (separate clusters for regulated tenants). (4) Upgrade windows conflict (some tenants need latest version, others need stability). (5) Blast radius is too large (single cluster failure affects too many tenants). Strategy: keep shared clusters for dev/staging, dedicated clusters for production. Use CAPI + ArgoCD for fleet management.

**Q2: How do you handle tenant isolation during cluster upgrades?**
A: (1) PodDisruptionBudgets per tenant ensure minimum availability during node drains. (2) Rolling upgrade strategy: upgrade one node at a time, draining and cordoning. (3) Priority-based eviction: development pods evicted first, production last. (4) Communication: platform team notifies tenants of maintenance windows. (5) Pre-upgrade validation: check that all PDBs are satisfiable before starting upgrade. (6) Canary: upgrade a subset of nodes first, monitor for issues, then continue.

**Q3: How do you handle a tenant that deploys a fork bomb or resource-exhausting workload?**
A: Defense in depth: (1) **LimitRange** limits per-container CPU/memory — prevents a single pod from claiming all resources. (2) **ResourceQuota** limits total namespace resources — even if every pod hits limits, total is bounded. (3) **PID limit**: containerd `--pids-limit` per container (default 4096). (4) **Pod Security Standard: Restricted**: prevents privileged containers that could escape cgroups. (5) **Rate limiting**: API Priority and Fairness prevents tenant from overwhelming API server. (6) **Detection**: alert on sudden resource usage spikes per namespace. (7) **Response**: platform team can cordon tenant's pods, reduce quota, or delete offending pods.

**Q4: How do you provide tenants with self-service capabilities while maintaining platform control?**
A: (1) **Tenant CRD**: tenants can modify their Tenant CR to request changes (add users, increase quota). (2) **Approval workflow**: quota increases require platform team approval (webhook + Slack integration). (3) **Backstage portal**: self-service UI for namespace creation, user management, quota visibility. (4) **GitOps**: tenants own their namespace configuration in a Git repo; platform team reviews PRs. (5) **Guardrails**: admission policies prevent tenants from creating resources outside their boundaries.

**Q5: How do you implement fair scheduling across tenants?**
A: Kubernetes scheduler doesn't have native tenant-aware fairness. Approaches: (1) **ResourceQuota**: hard limit prevents any tenant from consuming more than their share. (2) **Priority classes**: critical tenants get higher priority. (3) **Scheduler plugins**: custom scorer that prefers nodes with lower utilization by the requesting tenant (prevents concentration). (4) **Volcano scheduler**: supports queue-based fair-share scheduling for batch workloads. (5) **Request-to-capacity ratio**: alert when any tenant's requests exceed 80% of their quota.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | Tenant namespace accidentally deleted | All tenant workloads lost | API audit log alert | Recreate from GitOps; PVCs retained if reclaimPolicy=Retain | 5-30 min |
| 2 | RBAC misconfiguration (tenant gets cluster-admin) | Cross-tenant access possible | Periodic RBAC audit; OPA policy | Revoke binding; investigate access logs | < 5 min |
| 3 | NetworkPolicy deleted | Tenant traffic exposed | Drift detection controller | Recreate from template; tenant controller reconciles | < 1 min |
| 4 | ResourceQuota deleted | Tenant can consume unlimited resources | Drift detection controller | Recreate from template | < 1 min |
| 5 | Gatekeeper down | Policies not enforced (failurePolicy-dependent) | Gatekeeper health check | Restart; HA deployment | < 1 min |
| 6 | Noisy neighbor (memory) | Co-located pods OOM killed | OOM kill metrics per namespace | Evict offending pods; enforce limits; consider dedicated nodes | < 5 min |
| 7 | Tenant credential leak | Unauthorized access to tenant namespace | Unusual API access patterns in audit log | Revoke tokens; rotate credentials; investigate scope | < 30 min |
| 8 | Cost attribution data loss | Incorrect billing | Prometheus storage alerts | Thanos long-term storage; recompute from raw metrics | < 1 hour |
| 9 | OIDC provider outage | Users cannot authenticate | API server auth failure rate spike | Fallback: emergency break-glass service account; fix OIDC | Variable |
| 10 | Tenant controller crash | New tenants not provisioned; policy drift not corrected | Controller health metrics | Deployment auto-restart; existing tenants unaffected | < 1 min |

### Automated Recovery

| Mechanism | Implementation |
|-----------|---------------|
| Tenant controller reconciliation | Continuously ensures namespace + RBAC + quota + policies exist |
| Gatekeeper audit | Periodically scans existing resources for policy violations |
| RBAC watcher | Alerts on any cluster-scoped binding changes |
| NetworkPolicy drift detection | Controller compares deployed policies against templates |
| Cost metric redundancy | Prometheus + Thanos with multi-replica; recalculate from raw data |

---

## 10. Observability

### Key Metrics

| Metric | Source | Alert Threshold | Meaning |
|--------|--------|----------------|---------|
| `kube_resourcequota{type="used"}` / `{type="hard"}` | kube-state-metrics | > 80% | Tenant approaching quota |
| `container_cpu_usage_seconds_total` per namespace | cAdvisor | > 90% of requests | Tenant CPU-saturated |
| `container_memory_working_set_bytes` per namespace | cAdvisor | > 80% of limits | Tenant approaching OOM |
| Network policy denied flows | Cilium Hubble | Spike > 100/min | Possible misconfiguration or attack |
| Gatekeeper violations | Gatekeeper audit | > 0 in enforce mode | Policy violation in existing resources |
| API 403 errors per user/tenant | API audit log | > 10/hour | User attempting unauthorized access |
| Pod creation failures per namespace | kube-state-metrics | > 5% failure rate | Quota exhaustion or policy rejection |
| Inter-tenant traffic attempts | Cilium Hubble | Any | Cross-tenant communication (should be zero unless explicitly allowed) |

### Distributed Tracing

```
Per-tenant tracing:
1. Each tenant gets a Jaeger/Tempo tenant in the observability stack
2. Traces are labeled with namespace/tenant
3. Platform team can view all traces; tenants can only view their own
4. Trace sampling rate may differ by tenant (production: 1%, dev: 10%)
```

### Logging

```
Per-tenant log isolation:
1. Logs shipped with namespace label
2. Loki/Elasticsearch with multi-tenant mode:
   - Loki: X-Scope-OrgID header per tenant
   - Elasticsearch: index per tenant (logs-team-alpha-*)
3. Grafana with team datasources:
   - Team A can only query their indices/tenant
4. Audit logs: centralized, platform-team-only access
5. Retention: configurable per tenant (compliance tenants: 1 year)
```

---

## 11. Security

### Auth & AuthZ

**Defense-in-depth summary:**

| Layer | Mechanism | Protects Against |
|-------|-----------|-----------------|
| Authentication | OIDC with short-lived tokens (1h) | Unauthorized access |
| Authorization | Namespace-scoped RBAC | Cross-tenant API access |
| Admission | OPA/Gatekeeper + PSA | Non-compliant workloads |
| Network | Default-deny NetworkPolicy | Cross-tenant network traffic |
| Runtime | Seccomp + AppArmor + no-root | Container escape |
| Data | Encryption at rest + in-transit | Data exposure |
| Audit | Full API audit logging | Incident investigation |

**Security Hardening Checklist:**

```
Control Plane:
  [x] API server: --anonymous-auth=false
  [x] API server: --enable-admission-plugins=...,PodSecurity,...
  [x] API server: --audit-policy-file=/etc/kubernetes/audit-policy.yaml
  [x] etcd: encryption at rest for Secrets
  [x] etcd: TLS for client and peer communication
  [x] API server: --profiling=false (disable profiling endpoint)

RBAC:
  [x] No ClusterRoleBindings for tenant users
  [x] Default service account has no permissions (automountServiceAccountToken: false)
  [x] No wildcard permissions (*) in tenant Roles
  [x] Regular RBAC audit (who has access to what)

Network:
  [x] Default-deny NetworkPolicy in every tenant namespace
  [x] DNS egress allowed (port 53)
  [x] Ingress only from ingress controller namespace
  [x] No direct pod-to-pod cross-namespace unless explicitly approved

Pod Security:
  [x] PSA enforce: restricted on all tenant namespaces
  [x] No privileged containers
  [x] runAsNonRoot: true
  [x] readOnlyRootFilesystem: true
  [x] seccompProfile: RuntimeDefault
  [x] No hostPath, hostNetwork, hostPID, hostIPC

Image Security:
  [x] OPA: only approved registries
  [x] Image scanning in CI (Trivy)
  [x] Image signing verification (cosign)
  [x] No latest tag (require specific version)
```

### Multi-tenancy Isolation

Comprehensive isolation matrix:

| Attack Vector | Mitigation | Residual Risk |
|---------------|-----------|---------------|
| Tenant reads another's Secrets | RBAC: namespace-scoped | Misconfigured RoleBinding → audit |
| Tenant networks into another's pods | NetworkPolicy: default deny | Policy deletion → drift detection |
| Tenant exhausts node CPU/memory | ResourceQuota + LimitRange | Burst within limits → dedicated nodes |
| Tenant escalates to cluster-admin | RBAC escalation prevention + OPA | Zero-day k8s vulnerability → update promptly |
| Container escape (kernel exploit) | PSA Restricted + Seccomp + no-root | Kernel zero-day → use gVisor for sensitive workloads |
| Tenant installs CRDs | RBAC: deny cluster-scoped resource creation | vCluster for tenants needing CRDs |
| Side-channel attack (Spectre) | Dedicated node pools for sensitive tenants | Hardware-level mitigation |

---

## 12. Incremental Rollout Strategy

### Phase 1: Foundation (Week 1-2)
- Design tenant model (namespace-per-tenant).
- Create Tenant CRD and tenant controller.
- Set up OIDC authentication.
- Create RBAC templates (admin, developer, viewer, CI).
- Onboard 3 pilot teams.

### Phase 2: Isolation (Week 3-4)
- Deploy default-deny NetworkPolicies.
- Implement ResourceQuotas and LimitRanges.
- Enable Pod Security Admission (warn mode).
- Test cross-tenant isolation.

### Phase 3: Policy Enforcement (Week 5-6)
- Deploy OPA/Gatekeeper.
- Create core policies (registries, labels, security).
- Run in dryrun/warn mode.
- Iterate based on violation reports.

### Phase 4: Observability & Cost (Week 7-8)
- Deploy per-tenant Grafana dashboards.
- Set up per-tenant log isolation (Loki).
- Deploy Kubecost/OpenCost for cost attribution.
- Generate first cost reports.

### Phase 5: Self-Service & Scale (Week 9-10)
- Build self-service portal (Backstage or custom).
- Automate tenant onboarding (< 30 min).
- Enforce policies (switch from warn to deny).
- Onboard remaining teams.
- Document operational runbooks.

### Rollout Interviewer Q&As

**Q1: How do you onboard a new tenant to the platform?**
A: (1) Team requests access via self-service portal or Jira ticket. (2) Platform team (or automation) creates a Tenant CR in Git. (3) PR review verifies: cost center, owner, requested quota, compliance requirements. (4) Merge triggers Flux/ArgoCD → Tenant CR applied to cluster → Tenant controller provisions: namespaces, RBAC, quotas, NetworkPolicies. (5) OIDC group created in IdP for the team. (6) Team receives onboarding documentation: kubeconfig setup, allowed registries, quota limits, support channels. (7) Total time: < 30 minutes automated, < 4 hours with manual approval.

**Q2: How do you handle a tenant that consistently exceeds their quota?**
A: (1) Alert when quota usage > 80%. (2) Analyze usage: is it legitimate growth or waste? (3) If legitimate: increase quota (with cost-center approval). (4) If waste: provide VPA recommendations, help right-size. (5) If repeated: implement usage-based chargeback so the team has financial incentive to optimize. (6) Emergency: temporarily increase quota to avoid outage, then right-size.

**Q3: How do you handle tenant offboarding?**
A: (1) Verify no active workloads (or migrate them). (2) Take final backup of persistent data. (3) Delete Tenant CR → tenant controller cascading deletes: namespaces → all resources within (except PVs with Retain policy). (4) Remove OIDC group in IdP. (5) Archive audit logs and cost data. (6) Reclaim dedicated node pools if applicable. (7) Generate final cost report.

**Q4: How do you handle the transition from single-tenant clusters to a multi-tenant platform?**
A: (1) Document the target tenancy model and isolation guarantees. (2) Set up the shared cluster with all isolation layers. (3) Migrate teams one at a time (canary approach). (4) First migrate dev/staging environments (lower risk). (5) Validate isolation: can team A access team B's resources? (6) Then migrate production (with PDB, monitoring). (7) Decommission old single-tenant clusters after migration + burn-in period.

**Q5: How do you handle emergency access (break-glass) when the OIDC provider is down?**
A: (1) Pre-provision a break-glass ServiceAccount with cluster-admin in a sealed Kubernetes Secret. (2) Store the token in a physical safe or a separate secrets manager (not the OIDC-dependent one). (3) Break-glass access is logged, alerted, and requires post-incident review. (4) Token has a short TTL (or is rotated frequently). (5) Alternative: x509 client certificate stored offline — does not depend on OIDC. (6) Important: test break-glass procedure quarterly.

---

## 13. Trade-offs & Decision Log

| # | Decision | Alternative Considered | Trade-off | Rationale |
|---|----------|----------------------|-----------|-----------|
| 1 | Namespace-per-tenant | Cluster-per-tenant | Less isolation but much higher efficiency (1 cluster vs. 100) | Internal teams = trusted; namespace isolation sufficient with policy enforcement |
| 2 | OIDC authentication | x509 client certificates | Token management vs. cert rotation | OIDC integrates with corporate IdP; short-lived tokens; group claims for RBAC |
| 3 | OPA/Gatekeeper over Kyverno | Kyverno (YAML-based) | Steeper learning curve (Rego) but more powerful | Complex policies (cross-resource validation) require Rego expressiveness |
| 4 | Request-based cost attribution | Usage-based | Penalizes over-provisioning but incentivizes right-sizing | Fair: you reserve it, you pay for it. Complements VPA recommendations |
| 5 | Shared node pools (default) | Dedicated node pools per tenant | Less isolation but 2-3x better utilization | Dedicated pools only for compliance-required tenants |
| 6 | Pod Security Admission over PSP | OPA-only security policies | Less flexible but built-in (no external dependency) | PSA is the k8s standard; OPA complements with custom policies |
| 7 | Tenant controller (CRD-based) | Manual namespace provisioning | Development cost but consistent automation | Eliminates drift, enables self-service, ensures all isolation layers applied |
| 8 | Default-deny NetworkPolicy | Allow-all with selective deny | More restrictive but zero-trust | Security-first: deny all, explicitly allow what's needed |
| 9 | Short-lived OIDC tokens (1h) | Long-lived service account tokens | More frequent auth but smaller blast radius on token leak | Limits damage if token is compromised; auto-refresh via kubelogin |
| 10 | Centralized audit logging | Per-tenant audit logs | Less tenant privacy but unified security monitoring | Platform team needs cross-tenant visibility for security investigations |

---

## 14. Agentic AI Integration

### AI-Powered Multi-Tenant Platform Management

| Use Case | AI Agent Capability | Implementation |
|----------|-------------------|---------------|
| **Quota right-sizing** | Analyze resource usage patterns and recommend quota adjustments | Agent queries Prometheus for per-namespace utilization over 30 days; identifies over/under-provisioned tenants; generates quota change PRs |
| **Policy violation triage** | Prioritize and explain policy violations | Agent groups violations by severity, tenant, and pattern; generates remediation suggestions for each violation type |
| **Anomaly detection** | Detect unusual tenant behavior (resource spikes, cross-tenant probes) | Agent monitors per-tenant metrics and network flows; flags anomalies (3-sigma deviation from baseline) |
| **Cost optimization** | Identify cost-saving opportunities per tenant | Agent analyzes: idle pods, over-provisioned PVCs, unused services; generates per-tenant optimization reports |
| **Tenant onboarding automation** | Generate complete tenant configuration from requirements | Agent takes input (team name, compliance level, expected workload) and generates full Tenant CR with appropriate quotas, policies, node pool selection |
| **Security audit** | Comprehensive security posture assessment per tenant | Agent audits: RBAC bindings, NetworkPolicies, PSA compliance, image scan results; generates security scorecard |

### Example: AI-Driven Tenant Security Audit

```python
class TenantSecurityAuditor:
    """
    Performs comprehensive security audit of a tenant's namespace configuration.
    """
    
    def audit_tenant(self, tenant_name: str) -> SecurityReport:
        namespaces = self.get_tenant_namespaces(tenant_name)
        findings = []
        
        for ns in namespaces:
            # Check RBAC
            bindings = self.k8s.list_role_bindings(ns)
            for binding in bindings:
                if self.has_wildcard_permissions(binding):
                    findings.append(Finding(
                        severity='HIGH',
                        namespace=ns,
                        resource=f'RoleBinding/{binding.name}',
                        issue='Wildcard verb permissions detected',
                        remediation='Replace * verbs with explicit list'
                    ))
            
            # Check NetworkPolicies
            policies = self.k8s.list_network_policies(ns)
            if not self.has_default_deny(policies):
                findings.append(Finding(
                    severity='CRITICAL',
                    namespace=ns,
                    issue='No default-deny NetworkPolicy found',
                    remediation='Apply default-deny ingress+egress policy'
                ))
            
            # Check Pod Security
            ns_labels = self.k8s.get_namespace(ns).labels
            psa_enforce = ns_labels.get('pod-security.kubernetes.io/enforce', '')
            if psa_enforce != 'restricted':
                findings.append(Finding(
                    severity='HIGH',
                    namespace=ns,
                    issue=f'PSA enforce level is "{psa_enforce}" (should be restricted)',
                    remediation='Set pod-security.kubernetes.io/enforce: restricted'
                ))
            
            # Check for privileged pods
            pods = self.k8s.list_pods(ns)
            for pod in pods:
                if self.is_privileged(pod):
                    findings.append(Finding(
                        severity='CRITICAL',
                        namespace=ns,
                        resource=f'Pod/{pod.name}',
                        issue='Privileged container detected',
                        remediation='Remove privileged flag; use specific capabilities'
                    ))
            
            # Check image sources
            for pod in pods:
                for container in pod.spec.containers:
                    if not self.is_approved_registry(container.image):
                        findings.append(Finding(
                            severity='MEDIUM',
                            namespace=ns,
                            resource=f'Pod/{pod.name}',
                            issue=f'Image from unapproved registry: {container.image}',
                            remediation='Use images from registry.company.com'
                        ))
        
        return SecurityReport(
            tenant=tenant_name,
            findings=findings,
            score=self.calculate_score(findings),
            recommendations=self.generate_recommendations(findings)
        )
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Design a multi-tenant Kubernetes platform for 100 internal teams. What are the key components?**
A: (1) **Tenancy model**: namespace-per-tenant with Tenant CRD for lifecycle management. (2) **Authentication**: OIDC with corporate IdP (Okta/Azure AD), short-lived tokens. (3) **Authorization**: RBAC with Role/RoleBinding per namespace; roles: admin, developer, viewer, CI. (4) **Network isolation**: default-deny NetworkPolicy per namespace; Cilium for enforcement. (5) **Resource isolation**: ResourceQuota + LimitRange per namespace; quota profiles (small/medium/large). (6) **Policy enforcement**: OPA/Gatekeeper for admission control (registries, security, labels). (7) **Security**: Pod Security Admission (restricted), encryption at rest, audit logging. (8) **Cost attribution**: Kubecost/OpenCost with request-based chargeback. (9) **Observability**: per-tenant Grafana dashboards, Loki/ES for logs. (10) **Self-service**: Backstage portal + tenant controller automation.

**Q2: How do you ensure that one tenant's workload cannot crash another tenant's pods?**
A: (1) **Memory**: LimitRange enforces per-container limits; QoS-based OOM scoring evicts BestEffort first. (2) **CPU**: LimitRange enforces per-container CPU limits; CFS throttling prevents CPU monopolization. (3) **Disk**: ephemeral storage limits; kubelet evicts pods exceeding disk allocation. (4) **Network**: bandwidth annotations on pods (Cilium); network policy prevents DDoS between namespaces. (5) **API**: API Priority and Fairness prevents a tenant's controllers from overwhelming the API server. (6) **Scheduling**: topology spread and node-level limits prevent one tenant from landing all pods on one node.

**Q3: How do you handle Secrets management in a multi-tenant cluster?**
A: (1) RBAC: Secrets access scoped to namespace (tenant can only read their own Secrets). (2) Encryption at rest: `--encryption-provider-config` encrypts Secrets in etcd. (3) External Secrets Operator: sync from Vault/AWS Secrets Manager (each tenant has their own Vault path). (4) Sealed Secrets: encrypt in Git, decrypt in-cluster (per-namespace encryption key). (5) No `default` ServiceAccount auto-mount (`automountServiceAccountToken: false`). (6) Audit: log all Secret access in API audit log.

**Q4: How do you prevent a tenant from deploying a cryptocurrency miner?**
A: Multiple layers: (1) **Image registry restriction**: only approved registries allowed (OPA policy). (2) **Image scanning**: Trivy Operator scans all images; block images with critical CVEs. (3) **Resource monitoring**: alert on sustained 100% CPU usage in tenant namespace. (4) **Network monitoring**: alert on connections to known mining pools (Cilium DNS policy). (5) **Pod Security**: restricted PSA prevents privileged access (miners often need GPUs or special capabilities). (6) **Resource limits**: LimitRange caps per-container CPU; miner cannot effectively use more.

**Q5: How do you implement per-tenant Ingress without exposing other tenants' services?**
A: (1) Shared Ingress controller (NGINX/Envoy) with namespace isolation. (2) Each tenant creates Ingress resources in their namespace. (3) IngressClass per environment: `production-ingress`, `development-ingress`. (4) DNS: `<service>.<tenant>.company.com` pattern. (5) TLS: cert-manager issues per-Ingress certificates (Let's Encrypt or internal CA). (6) OPA policy: Ingress hostnames must match `*.<tenant>.company.com` pattern — prevents hostname hijacking. (7) Rate limiting: per-tenant rate limits on the ingress controller.

**Q6: How do you handle compliance requirements (PCI-DSS) for specific tenants?**
A: (1) **Dedicated node pool**: taint + toleration for PCI tenant only. (2) **Enhanced logging**: full audit logging at RequestResponse level for PCI namespace. (3) **Network isolation**: stricter NetworkPolicies; no internet egress unless explicitly approved. (4) **Encryption**: mandatory TLS for all inter-pod communication (mTLS via service mesh). (5) **Image security**: mandatory image signing + vulnerability scanning with zero-critical-CVE policy. (6) **Access control**: MFA required for PCI namespace access (OIDC with MFA step-up). (7) **Regular audits**: automated compliance checks via AI security auditor (section 14).

**Q7: What is hierarchical namespaces (HNC) and how does it help multi-tenancy?**
A: HNC allows creating parent-child namespace relationships. A parent namespace's RBAC policies, NetworkPolicies, and ResourceQuotas propagate to child namespaces automatically. Benefits: (1) Team "backend" is parent; "backend-api", "backend-worker" are children — policies inherited. (2) Reduces duplication of policies across namespaces. (3) Subtree quotas (alpha): limit total resources across all child namespaces. (4) Self-service: team leads can create child namespaces within their parent's quota. Limitation: still a Kubernetes namespace-based model; no virtual control plane isolation.

**Q8: How do you handle the scenario where two tenants need to share a database?**
A: (1) **Shared service namespace**: create a dedicated namespace for the shared database. (2) **RBAC**: both tenants get read-only access to the service endpoint (but not the pods). (3) **NetworkPolicy**: allow traffic from both tenant namespaces to the database namespace. (4) **Application-level isolation**: separate database schemas/users per tenant. (5) **Cost attribution**: split database cost proportionally based on query volume (application metrics). (6) **Alternative**: each tenant gets their own database instance — more isolation, higher cost.

**Q9: How do you prevent namespace squatting (tenant creates a namespace that conflicts with another tenant)?**
A: (1) **Naming convention enforcement**: OPA policy requires namespace names to start with tenant prefix (e.g., `team-alpha-*`). (2) **Tenant controller**: only the controller can create namespaces; direct `kubectl create namespace` is blocked by admission webhook. (3) **Namespace reservation**: platform maintains a registry of allocated namespace prefixes. (4) **RBAC**: tenants don't have `create` permission on namespaces (cluster-scoped resource).

**Q10: How do you implement network segmentation between tenant environments (dev vs. prod)?**
A: (1) Separate namespaces per environment: `team-alpha-dev`, `team-alpha-prod`. (2) NetworkPolicies: `team-alpha-dev` cannot reach `team-alpha-prod` (and vice versa). (3) Node pools: production pods on production-labeled nodes; dev pods on dev-labeled nodes (optional, for stronger isolation). (4) Priority classes: production pods have higher priority than dev pods. (5) Ingress: separate ingress controllers for production and development.

**Q11: How do you handle the case where the policy engine (Gatekeeper) rejects a legitimate workload?**
A: (1) Check the rejection message: `kubectl apply -f pod.yaml` returns the specific constraint that was violated. (2) Verify: is the workload actually violating the policy, or is the policy wrong? (3) If policy is correct: fix the workload (e.g., use an approved image). (4) If policy needs an exception: create a targeted Constraint with exclusion for this specific workload (by label or namespace). (5) If policy is wrong: fix in dryrun mode first, then promote to enforce. (6) Provide clear documentation: each policy should have a description, examples, and known exceptions.

**Q12: How do you measure the effectiveness of your multi-tenant isolation?**
A: (1) **Penetration testing**: periodically attempt cross-tenant access (API, network, storage). (2) **Compliance scanning**: CIS Kubernetes Benchmark checks for security best practices. (3) **Metrics**: track number of cross-tenant network flows (should be zero unless explicitly allowed). (4) **Audit log analysis**: flag any access to resources outside tenant's namespaces. (5) **Policy violation count**: track Gatekeeper violations over time (should trend toward zero). (6) **Chaos testing**: simulate tenant namespace deletion, verify other tenants are unaffected.

**Q13: How do you handle custom resource definitions (CRDs) in a multi-tenant cluster?**
A: CRDs are cluster-scoped — any tenant with CRD create permission could install CRDs that affect all tenants. Solutions: (1) **Deny CRD creation by tenants** (RBAC: no `create` on `customresourcedefinitions`). (2) Platform team manages all CRDs. (3) If a tenant needs custom CRDs: use vCluster (virtual control plane where they can install CRDs). (4) Operator CRDs: platform team installs operators centrally; tenants create CRs in their namespace.

**Q14: How do you implement tenant SLAs (different availability guarantees per tenant)?**
A: (1) **Priority classes**: production-critical tenants get highest priority; their pods preempt lower-priority pods. (2) **PodDisruptionBudgets**: stricter for high-SLA tenants (minAvailable: 90%). (3) **Dedicated node pools**: high-SLA tenants on dedicated nodes with more headroom. (4) **Topology spread**: require zone distribution for high-SLA services. (5) **Monitoring**: per-tenant SLO dashboards (availability, error rate, latency). (6) **Alerting**: pager-level alerts for high-SLA tenants; email for standard tenants.

**Q15: How do you debug issues in a multi-tenant cluster without accessing other tenants' data?**
A: (1) Platform team uses cluster-admin but follows strict access protocol (documented, audited). (2) Tenant-scoped debugging: `kubectl --as=alice@company.com get pods -n team-alpha-prod` (impersonation to verify RBAC). (3) Per-tenant observability: tenant's own Grafana org with namespace-scoped datasources. (4) Log access: tenants only see logs from their namespaces (Loki tenant ID = namespace). (5) Network debugging: Cilium Hubble with namespace filter — show only flows for this tenant. (6) Escalation: if platform team needs to inspect tenant data, follow break-glass procedure with audit trail.

---

## 16. References

| # | Reference | URL |
|---|-----------|-----|
| 1 | Multi-tenancy in Kubernetes | https://kubernetes.io/docs/concepts/security/multi-tenancy/ |
| 2 | Hierarchical Namespaces Controller | https://github.com/kubernetes-sigs/hierarchical-namespaces |
| 3 | vCluster | https://www.vcluster.com/ |
| 4 | OPA Gatekeeper | https://open-policy-agent.github.io/gatekeeper/ |
| 5 | Kyverno | https://kyverno.io/ |
| 6 | Capsule | https://capsule.clastix.io/ |
| 7 | Pod Security Admission | https://kubernetes.io/docs/concepts/security/pod-security-admission/ |
| 8 | Kubernetes RBAC | https://kubernetes.io/docs/reference/access-authn-authz/rbac/ |
| 9 | Kubecost | https://www.kubecost.com/ |
| 10 | OpenCost | https://www.opencost.io/ |
| 11 | CIS Kubernetes Benchmark | https://www.cisecurity.org/benchmark/kubernetes |
| 12 | Cilium Network Policies | https://docs.cilium.io/en/stable/security/policy/ |
