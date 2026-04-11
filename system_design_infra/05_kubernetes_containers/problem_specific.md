# Problem-Specific Design — Kubernetes & Containers (05_kubernetes_containers)

## Container Orchestration System

### Unique Functional Requirements
- Container lifecycle with Linux kernel primitives: cgroups v2, namespaces, overlayfs
- Pod-to-pod flat networking (every pod unique IP); < 0.1 ms same-node, < 1 ms cross-node
- Node density: 110 pods/node default, 250 with tuning; container restart exponential backoff 10 s → 5 min max
- OOM eviction priority by QoS class; CoreDNS resolution < 5 ms

### Unique Components / Services
- **kubelet**: node agent managing pod lifecycle, liveness/readiness/startup probes, volume mount, status reporting to API server
- **containerd + runc**: CRI-compliant runtime (containerd) → OCI runtime (runc) creates namespaces + cgroups + starts process; overlayfs for image layers
- **CNI Plugin (Calico/Cilium eBPF)**: pod IP allocation, veth pairs, network policies enforced in eBPF kernel space; < 0.1 ms same-node latency
- **CSI Node Plugin**: volume attach (< 30 s), mount, filesystem configuration; PVC → PV binding
- **kube-proxy**: ClusterIP → pod IP translation via iptables/IPVS/eBPF; ClusterIP → pod latency < 1 ms
- **CoreDNS**: in-cluster DNS; `svc.cluster.local` resolution < 5 ms

### Unique Data Model
- **cgroup v2 per container**: `cpu.max: "500000 100000"` (500m cores); `memory.max` (hard limit); `memory.min` (guaranteed); `cpu.weight` (proportional to requests); `pids.max: 4096`
- **QoS Classes**: Guaranteed (requests == limits), Burstable (requests < limits), BestEffort (no requests/limits)
- **OOM eviction scores**: BestEffort = 1000; Burstable = (used - requested) / limit × 999; Guaranteed = -997
- **EndpointSlices**: max 100 endpoints per slice; sharded for services > 100 pods
- **PVC accessModes**: ReadWriteOnce, ReadOnlyMany, ReadWriteMany
- **Pod topology spread**: violation if `(pods_in_domain + 1) - pods_in_min_domain > maxSkew`

### Algorithms

**OOM Eviction Priority:**
```
BestEffort: OOM score = 1000 (evicted first)
Burstable:  OOM score = (memory_usage - memory_requests) / memory_limit × 999
Guaranteed: OOM score = -997 (evicted last)
```

**CFS CPU Throttling:**
```
cpu.max quota = requests_in_millicores × period_us / 1000
Example: 500m → 500000 / 1000000 = 0.5 cores/s
```

### Key Differentiator
Container Orchestration's uniqueness is its **cgroups v2 QoS OOM eviction + eBPF networking**: OOM score formula `(used - requested) / limit` evicts the most over-budget Burstable container first, protecting Guaranteed pods; cgroups v2 memory.min enables kernel-enforced memory reservation; Cilium eBPF enforces NetworkPolicy in kernel space (< 0.1 ms intra-node vs. > 1 ms iptables); 250 pods/node density achieved via overlayfs image sharing and pids.max cgroup limit.

---

## Kubernetes Cluster API Platform

### Unique Functional Requirements
- Declarative cluster provisioning across 4+ providers: AWS (CAPA), GCP (CAPG), vSphere (CAPV), bare-metal (CAPM3/Metal3)
- Cluster provisioning: < 15 min cloud, < 30 min bare-metal; 500+ workload clusters per management cluster
- MachineHealthCheck auto-remediation with circuit breaker (maxUnhealthy threshold)
- ClusterClass templates with variables for fleet standardization

### Unique Components / Services
- **Infrastructure Providers**: CAPA (AWS EC2), CAPG (GCP GCE), CAPV (vSphere VM), CAPM3 (Metal3/Ironic bare-metal); each translates Machine spec to provider-specific API calls
- **MachineHealthCheck**: watches nodes matching selector; if `Ready=Unknown > 300 s` → IPMI reboot → wait `nodeStartupTimeout`; if still unhealthy → delete Machine → MachineSet creates replacement; `maxUnhealthy: 40%` prevents cascading deletion
- **KubeadmControlPlane**: manages etcd cluster, API server, scheduler, controller-manager; handles rolling upgrades; rollout strategy respects `maxSurge`
- **Bootstrap Provider**: generates cloud-init/ignition data for node initial configuration; stored as Secret referenced by Machine.spec.bootstrap.dataSecretName
- **BareMetalHost (Metal3)**: state machine: Registering → Inspecting → Available → Provisioning → Provisioned; `spec.bmc.address` IPMI/Redfish; `spec.hardwareProfile`

### Unique Data Model
- **Cluster**: `spec.controlPlaneRef → KubeadmControlPlane`; `spec.infrastructureRef → AWSCluster/VSpherCluster`; `status.phase: Provisioned/Provisioning`; `conditions[]: Ready, ControlPlaneReady, InfrastructureReady`
- **Machine**: `spec.providerID` (e.g., `i-1234abcd`); `spec.bootstrap.dataSecretName`; `status.phase: Pending/Provisioning/Provisioned`; `status.nodeRef → Node`
- **MachineHealthCheck**: `spec.unhealthyConditions: [{type, status, timeout}]`; `spec.maxUnhealthy: "40%"`; `spec.nodeStartupTimeout: 600s`; `spec.remediationTemplate: provider CR`

### Key Differentiator
Cluster API's uniqueness is its **pluggable infrastructure provider abstraction + MachineHealthCheck circuit breaker**: provider interface (CAPA/CAPG/CAPV/CAPM3) standardizes cluster CRUD across cloud and bare-metal without changing management logic; `maxUnhealthy: 40%` stops automated remediation when > 40% of machines are unhealthy (prevents deleting an entire cluster during widespread failure); BareMetalHost state machine (Registering → Inspecting → Available) tracks hardware lifecycle through IPMI/Redfish before it's fit for workloads.

---

## Kubernetes Control Plane

### Unique Functional Requirements
- 5,000 nodes, 150,000 pods, 10,000 services per cluster
- API server P99: < 100 ms GET, < 200 ms write, < 1 s LIST of 5 K objects
- Scheduling throughput: ≥ 100 pods/s; etcd write P99 < 25 ms
- ~30 reconciliation control loops running continuously

### Unique Components / Services
- **kube-apiserver**: stateless; auth (X.509 + OIDC + webhook), admission (mutating → validating), optimistic concurrency (resourceVersion = etcd modRevision), Watch API (HTTP/2 streaming)
- **etcd (3/5-node Raft)**: `/registry/<resource>/<ns>/<name>` key structure; single source of truth; Bookmark events in watch stream prevent re-LIST storms
- **kube-scheduler**: Filter → Score → Bind pipeline; `percentageOfNodesToScore` (50% default); preemption: delete lower-priority victims to fit high-priority pod
- **kube-controller-manager**: ~30 reconciliation loops; each uses informer cache; leader election prevents split-brain

### Unique Data Model
- **Every k8s object**: `metadata.resourceVersion` (etcd modRevision for optimistic concurrency); `metadata.ownerReferences` (GC chain); `metadata.finalizers` (deletion protection)
- **etcd key**: `/registry/<resource-type>/<namespace>/<name>`
- **Scheduling Filter plugins**: NodeResourcesFit, NodePorts, TaintToleration, NodeAffinity, PodTopologySpread, VolumeBinding, InterPodAffinity
- **Scheduling Score plugins** (0–100): LeastAllocated, BalancedResourceAllocation, InterPodAffinity, ImageLocality, PodTopologySpread

### Algorithms

**Scheduling Pipeline:**
```
Filter → reduces N nodes to feasible set
  (stops early once enough feasible found)
Score  → ranks by weighted plugin scores (0-100)
Bind   → Reserve → Permit → PreBind → Bind → PostBind

Preemption (when no node feasible):
  1. Identify lower-priority victims per node
  2. Simulate removal + re-run Filter
  3. Select node with minimum victims
  4. Delete victims (grace period) → schedule pod
```

### Key Differentiator
Control Plane's uniqueness is its **optimistic concurrency (resourceVersion) + Watch HTTP/2 streaming + percentageOfNodesToScore**: `resourceVersion = etcd modRevision` provides lock-free concurrent updates (update fails if resourceVersion changed → client retry); Watch over HTTP/2 multiplexes thousands of controller watchers on single TCP connection (critical for 5,000 node clusters); scheduling throughput ≥ 100 pods/s achieved by evaluating only 50% of nodes and stopping filter early.

---

## Kubernetes Operator Framework

### Unique Functional Requirements
- CRD-based desired state + reconciliation driving actual → desired
- Full stateful workload lifecycle: create, update, upgrade, scale, backup, restore, delete
- < 5 s reconciliation latency; ≥ 50 objects/s throughput; < 256 MB for 1,000 CRs
- Finalizer-based safe cleanup; owner references for GC; admission webhooks for defaulting/validation

### Unique Components / Services
- **CRD + Schema**: OpenAPI v3 schema with `x-kubernetes-validations` (CEL); defaults, min/max, enum
- **Reconciler (controller-runtime)**: `Reconcile(ctx, req) (Result, error)` pattern; idempotent: `build desired → compare → Create if missing / Update if different / no-op if same`; `Result{RequeueAfter: 60s}` for periodic health check
- **Work Queue**: rate-limited (1 s, 2 s, 4 s, ... 5 min backoff); deduplicates rapid changes; prevents thundering herd
- **Finalizer Pattern**: adds finalizer on creation; blocks deletion until cleanup completes; `RemoveFinalizer` only after external resources cleaned up
- **Leader Election Lease**: active replica holds Lease; standby replicas watch; < 15 s takeover

### Unique Data Model
- **CRD status conditions**: `type: Ready`, `status: True/False/Unknown`, `reason: string`, `observedGeneration: int` (tracks spec changes); `phase: Pending/Creating/Running/Upgrading/Failed`
- **Reconciliation sub-resource order**: ConfigMap → Secrets → Services → StatefulSet → application-specific config → CronJob (backup) → ServiceMonitor (monitoring)
- **Operator resource limits**: < 256 MB managing 1,000 CRs; < 100 QPS to API server (informer cache absorbs reads)

### Key Differentiator
Operator Framework's uniqueness is its **idempotent sub-resource diff reconciliation + finalizer-controlled deletion**: `build desired → compare → create/update/no-op` per sub-resource makes each reconcile safe to re-run after crash; finalizer blocks K8s deletion until operator cleans up external resources (e.g., cloud snapshots, DNS records) — without finalizer, parent object is deleted before cleanup completes; `observedGeneration` on status conditions distinguishes stale status from current-generation status.

---

## Multi-Tenant Kubernetes Platform

### Unique Functional Requirements
- 100+ tenants per cluster; noisy neighbor impact < 5%
- 5-layer isolation: API access + network + resources + security + optional node isolation
- Tenant onboarding < 30 min (automated); policy admission latency < 100 ms
- Cost attribution accuracy > 95%; audit retention 90 days

### Unique Components / Services
- **Tenant Controller**: on Tenant CR create → provisions namespace, RBAC (Role + RoleBinding), ResourceQuota, LimitRange, NetworkPolicy, service accounts atomically
- **OPA/Gatekeeper**: policy-as-code constraints: image registry allowlist, required labels (costCenter, team), no privileged containers; admission latency < 100 ms
- **5-Layer Defense**: (1) RBAC: team accesses own namespaces only; (2) NetworkPolicy: default-deny inter-tenant + Cilium L7; (3) ResourceQuota + LimitRange: prevents over-consumption; (4) Pod Security Standard Restricted + OPA; (5) optional: dedicated node pool with taint per sensitive tenant
- **vCluster (Enhanced Isolation)**: virtual k3s control plane per tenant in a pod; tenant sees empty cluster; pods synced to host cluster via Syncer; ~100 ms additional latency; provides API-level isolation

### Unique Data Model
- **ResourceQuota (large profile)**: `requests.cpu: "100"`, `requests.memory: "100Gi"`, `limits.cpu: "200"`, `limits.memory: "200Gi"`, `requests.storage: "500Gi"`, `pods: "500"`, `services: "50"`, `persistentvolumeclaims: "10"`
- **LimitRange per namespace**: `Container: min {cpu: 100m, memory: 128Mi}`, `max {cpu: 4, memory: 4Gi}`, `default {cpu: 500m, memory: 512Mi}`, `defaultRequest {cpu: 250m, memory: 256Mi}`; `Pod: max {cpu: 64, memory: 64Gi}`
- **Tenant CR**: `spec.namespaces[].quotaProfile: small/medium/large`; `spec.costCenter`; `spec.users[]`; `status.phase`
- **NetworkPolicy (default-deny)**: `ingress.from: namespaceSelector {matchLabels: {platform/tenant: team-alpha}}` — allows only same-tenant traffic by default

### Key Differentiator
Multi-Tenant Platform's uniqueness is its **5-layer defense-in-depth + vCluster option + Tenant Controller automation**: each layer independently blocks different attack vectors (network bypass → stopped by NetworkPolicy; resource exhaustion → stopped by ResourceQuota; privilege escalation → stopped by PSS Restricted + OPA); vCluster gives teams their own `kubectl get nodes` view without dedicated infrastructure cost; Tenant Controller provisions all 5 layers atomically in < 30 min via single Tenant CR.

---

## Pod Autoscaler System

### Unique Functional Requirements
- HPA (horizontal), VPA (vertical), KEDA (event-driven) working without conflicts
- Scale-out < 2 min; scale-in stabilization 5 min (prevent flapping); scale-to-zero < 30 s cold start
- VPA accuracy within 20% of optimal; based on 8-day usage histogram
- Cluster Autoscaler/Karpenter node provisioning < 5 min (cloud), < 20 min (bare-metal)

### Unique Components / Services
- **HPA Controller**: evaluates every 15 s; `desiredReplicas = ceil(currentReplicas × (currentValue / targetValue))`; multiple metrics → take maximum; tolerance band ±10% (no scaling within band)
- **VPA Recommender**: 8-day weighted histogram; `target ≈ P90`, `lowerBound ≈ P50`, `upperBound ≈ P95 + 15% headroom`; < 24 h = low confidence; ≥ 7 days = high confidence; OOM → immediately raise memory
- **VPA Updater**: optional; evicts pods with outdated requests to force re-creation with new VPA values; recommendation-only mode avoids disruption
- **KEDA**: event-driven scaler; polls external sources (Kafka lag, queue depth, cron); supports scale-to-zero; per-ScaledObject polling interval
- **Cluster Autoscaler / Karpenter**: provisions nodes for Pending unschedulable pods; scale-in when utilization < threshold for > 10 min; aware of pod startup delay (25–90 s)

### Unique Data Model
- **HPA spec**: `metrics: [{type: Resource, name: cpu, target: {utilization: 70}}]`; `behavior.scaleDown.stabilizationWindowSeconds: 300`; `behavior.scaleUp: {policies: [{type: Percent, value: 100}, {type: Pods, value: 4}], selectPolicy: Max}`
- **VPA recommendation**: `target: {cpu: "250m", memory: "512Mi"}`, `lowerBound`, `upperBound`, `uncappedTarget`; updated based on 8-day histogram
- **KEDA ScaledObject**: `spec.scaleTargetRef`, `spec.triggers: [{type: kafka, metadata: {topic, consumerGroup, lagThreshold}}]`, `spec.minReplicaCount: 0` (scale-to-zero)

### Algorithms

**HPA Formula:**
```
desiredReplicas = ceil(currentReplicas × (currentMetricValue / targetMetricValue))
Multiple metrics: take MAXIMUM across all metrics
Tolerance: no scaling if within ±10% of target
Scale-down: take MAXIMUM desired count over stabilizationWindow (5 min) → most conservative
```

**VPA Histogram:**
```
target = P90 of 8-day weighted usage
lowerBound = P50
upperBound = P95 + 15% headroom
After OOM: immediately bump memory upperBound
```

### Key Differentiator
Pod Autoscaler's uniqueness is its **HPA stabilization window + VPA 8-day P90 histogram + KEDA scale-to-zero**: stabilization window (300 s scale-down) takes maximum desired replicas over the window — scales down only when sustained; VPA uses 8-day weighted histogram (P90 target, P95+15% upper bound) tuned to avoid OOM while minimizing waste; KEDA enables scale-to-zero for event-driven workloads (Kafka consumer = 0 replicas when queue empty, scales up in < 30 s on lag detection); three scalers are non-conflicting when VPA is in recommendation-only mode.

---

## Service Mesh Design

### Unique Functional Requirements
- Automatic mTLS between all services via SPIFFE identity (no app code changes)
- Data plane latency per hop: < 1 ms P99 sidecar; xDS config propagation < 5 s
- Certificate TTL 24 h with auto-rotation at 80% TTL; 10,000+ services, 100,000+ pods
- Sidecar overhead: < 50 MB memory, < 5% CPU

### Unique Components / Services
- **istiod**: unified control plane (Pilot + Citadel CA + Galley); watches k8s objects → translates to xDS; issues SPIFFE X.509 certs via SDS
- **xDS (Aggregated Discovery Service)**: LDS (listeners), RDS (routes), CDS (clusters), EDS (endpoints), SDS (secrets) pushed over single ADS gRPC stream; hot-reload with no connection drop
- **SPIFFE Identity**: `spiffe://cluster.local/ns/<namespace>/sa/<service-account>`; ECDSA P-256; 24 h TTL; istiod validates pod + SA + JWT before issuing
- **Envoy Sidecar**: handles all inbound/outbound traffic; mTLS first connection 3–5 ms, subsequent < 0.5 ms (TLS session resumption); AES-GCM-128 HW-accelerated encryption adds ~20 bytes/request
- **Ingress/Egress Gateway**: Envoy at cluster edge; external TLS termination, routing, AuthorizationPolicy enforcement

### Unique Data Model
- **VirtualService**: `spec.http[].match`, `spec.http[].route[].destination.host + subset + weight`; canary: `[{destination: v1, weight: 90}, {destination: v2, weight: 10}]`
- **DestinationRule**: `spec.subsets[].labels` (v1/v2 label selectors); `spec.trafficPolicy.connectionPool`, `outlierDetection.consecutiveErrors: 5, baseEjectionTime: 30s`
- **AuthorizationPolicy**: `spec.selector`, `spec.rules[].from[].source.principals` (SPIFFE IDs), `spec.rules[].to[].operation.paths`

### Algorithms

**xDS Push Flow:**
```
1. User applies VirtualService
2. istiod detects via k8s watch
3. Translates to Envoy config (LDS+RDS+CDS+EDS)
4. Pushes via ADS to affected proxies
5. Envoy hot-reloads (no connection drop)
6. Takes effect within < 5 s
```

**mTLS Cert Lifecycle:**
```
1. Pod starts → Envoy requests cert via SDS
2. istiod validates: pod exists + SA matches + JWT token
3. istiod CA: ECDSA P-256, 24h TTL, SPIFFE SAN
4. At 80% TTL: Envoy requests new cert (no connection drop)
```

### Key Differentiator
Service Mesh's uniqueness is its **SPIFFE identity + ADS hot-reload + automated 80%-TTL cert rotation**: SPIFFE identity `spiffe://cluster.local/ns/<ns>/sa/<sa>` ties certificate to k8s ServiceAccount — identity is cryptographically bound to workload identity without application code changes; ADS single gRPC stream ensures LDS+RDS+CDS+EDS+SDS updates arrive in consistent order (prevents partial config where route points to non-existent cluster); 80%-TTL cert rotation happens without any connection drop because Envoy holds both old and new certs during transition.
