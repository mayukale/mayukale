# Common Patterns — Kubernetes & Containers (05_kubernetes_containers)

## Common Components

### etcd as Distributed State Store
- 6 of 7 systems rely on etcd (3–5 node Raft cluster) for consistent state; the foundation all Kubernetes components build on
- kubernetes_control_plane: all API objects stored at `/registry/<resource-type>/<namespace>/<name>`; P99 write < 25 ms; SSD-backed; 50–100 GB
- kubernetes_cluster_api: cluster, machine, and provider state; MachineHealthCheck conditions; control plane readiness
- kubernetes_operator_framework: CRD instances stored in etcd via API server; operator reads via informer cache
- multi_tenant_kubernetes: tenant namespaces, ResourceQuota, RBAC bindings, NetworkPolicy all persisted in etcd
- pod_autoscaler_system: HPA, VPA, KEDA ScaledObject CRDs stored in etcd; autoscaler leader election via Lease object
- service_mesh_design: Istio CRDs (VirtualService, DestinationRule, AuthorizationPolicy) stored in etcd

### Informer / Watch Cache Pattern
- All 7 systems use in-memory informer caches populated from etcd watches; decouples controllers from direct API server calls
- control_plane: kube-controller-manager maintains informer per resource type; reduces API server load from thousands to hundreds QPS
- operator_framework: controller-runtime Manager initializes shared informers; reconciler reads from cache (no per-reconcile API call)
- pod_autoscaler_system: HPA controller caches deployments and pods; VPA recommender caches pod resource usage history
- service_mesh_design: istiod watches VirtualServices, DestinationRules, pods via informers; translates to xDS pushed to Envoy

### Level-Triggered Reconciliation Loops
- 5 of 7 systems implement level-triggered (not edge-triggered) reconciliation: observe desired vs actual → take corrective action; idempotent by design
- kubernetes_control_plane: ~30 controller loops (Deployment, ReplicaSet, Node, ServiceAccount, EndpointSlice, etc.)
- kubernetes_cluster_api: Machine, MachineSet, MachineDeployment, MachineHealthCheck controllers
- kubernetes_operator_framework: `Reconcile(ctx, req)` pattern; build desired → compare → create/update/no-op for each sub-resource
- multi_tenant_kubernetes: Tenant Controller reconciles namespace, RBAC, quota, NetworkPolicy on every change
- service_mesh_design: Pilot reconciles Istio config changes → xDS push within < 5 s

### Admission Control (Mutating + Validating Webhooks)
- All 7 systems use admission webhooks: mutating (inject/default) runs first, then validating (reject invalid)
- container_orchestration: Pod Security Admission (Restricted/Baseline/Privileged enforcement)
- kubernetes_cluster_api: defaulting webhook (cluster network CIDR defaults), validating (version compatibility)
- kubernetes_control_plane: API server built-in admission (ResourceQuota, LimitRange); external webhooks
- kubernetes_operator_framework: CRD validation webhook; mutating webhook for defaulting spec fields
- multi_tenant_kubernetes: OPA/Gatekeeper policies (image registry allowlist, required labels); Pod Security Standards; admission latency < 100 ms
- pod_autoscaler_system: VPA admission webhook mutates pod resource requests on creation
- service_mesh_design: Sidecar Injector mutating webhook injects Envoy sidecar + init container

### RBAC for Authorization
- All 7 systems use Kubernetes RBAC (Role + ClusterRole + RoleBinding + ClusterRoleBinding); evaluation < 5 ms
- multi_tenant_kubernetes: namespace-scoped Roles limit tenant access to own namespaces only
- kubernetes_cluster_api: management cluster service accounts for provider credentials
- service_mesh_design: AuthorizationPolicy extends RBAC to L7 (path, method, SPIFFE identity)
- kubernetes_operator_framework: operator ServiceAccount with minimal permissions (least privilege)

### Leader Election for HA Controllers
- 6 of 7 systems use Lease-based leader election to ensure single active instance; < 15 s recovery on failover
- control_plane: kube-scheduler, kube-controller-manager, cloud-controller-manager each use leader election
- cluster_api: CAPI core controllers run in active-standby mode
- operator_framework: Manager configures leader election; standby replicas wait on Lease renewal
- pod_autoscaler_system: VPA recommender, cluster autoscaler, KEDA operator all use leader election
- service_mesh_design: istiod replicas elect leader for cert rotation coordination

### Health Checks and Probe-Triggered Remediation
- All 7 systems define health checks that trigger automated remediation
- container_orchestration: liveness probe (restart) + readiness probe (remove from service endpoints) + startup probe; exponential backoff 10 s → 20 s → 40 s → 5 min max
- kubernetes_cluster_api: MachineHealthCheck `unhealthyConditions`; reboot → replace if still unhealthy; `maxUnhealthy: 40%` circuit breaker
- service_mesh_design: Envoy outlier detection (circuit breaking): consecutive 5xx → eject from upstream for `baseEjectionTime`

## Common Databases

### etcd (Raft)
- 6 of 7; single source of truth for all Kubernetes objects; 3 or 5-node cluster; P99 write < 25 ms; zero data loss guarantee

### Prometheus
- 6 of 7; time-series metrics: resource utilization (HPA/VPA), autoscaler decisions, mesh observability, per-tenant cost attribution; PromQL for aggregation

### Loki / Elasticsearch
- 5 of 7; centralized logging: kubelet logs, operator logs, audit logs, Envoy access logs; 90-day retention

## Common Communication Patterns

### gRPC for Data Plane
- container_orchestration: kubelet → containerd via CRI gRPC
- service_mesh_design: istiod → Envoy via ADS (Aggregated Discovery Service) gRPC streaming; pushes LDS, RDS, CDS, EDS, SDS over single stream
- operator_framework: webhook server uses HTTPS (TLS over gRPC-compatible transport)

### HTTP/2 Watch Streaming
- control_plane: `GET /api/v1/pods?watch=true` over HTTP/2; multiplexes thousands of watchers on one TCP connection; Bookmark events prevent re-LIST storms
- service_mesh_design: xDS ADS stream (HTTP/2 gRPC); consistent ordering via ADS prevents partial config states

## Common Scalability Techniques

### percentageOfNodesToScore (Scheduler Sampling)
- kubernetes_control_plane: scheduler evaluates ~50% of nodes (default) for clusters > 100 nodes; stops once enough feasible candidates found; achieves 100+ pods/s throughput
- kubernetes_cluster_api: MachineSet scales to target count; doesn't evaluate every machine for each new machine placement

### EndpointSlice Sharding
- container_orchestration + control_plane: each EndpointSlice holds max 100 endpoints; large services sharded across multiple slices; prevents single object from becoming a hotspot at 10,000 pods/service

### Work Queue with Rate Limiting and Deduplication
- operator_framework: controller-runtime work queue deduplicates multiple rapid changes to same object into single reconcile; exponential backoff on error (1 s, 2 s, 4 s, ... 5 min max); prevents thundering herd

## Common Deep Dive Questions

### How does the Kubernetes scheduler achieve 100+ pods/s throughput at 5,000 nodes?
Answer: Three optimizations: (1) Filter phase only evaluates a subset of nodes — `percentageOfNodesToScore` (default 50%) means 5,000 nodes → evaluate 2,500; stops early once sufficient feasible nodes found; (2) Informer cache: all node/pod state is in-memory, no API calls per scheduling decision; (3) Parallel scheduling: scheduler runs multiple goroutines processing pending pod queue concurrently. The Filter → Score → Bind pipeline is pipelined across goroutines.
Present in: kubernetes_control_plane, pod_autoscaler_system

### Why does the HPA use a stabilization window instead of scaling immediately on metric changes?
Answer: Without stabilization, a brief CPU spike → scale-up → spike ends → scale-down creates oscillation. Scale-down window (default 300 s) takes the maximum desired replica count across the past 5 minutes — scales down only when replicas have been consistently unnecessary. Scale-up window is 0 s by default (responsive). Multiple metrics: take the maximum desired replicas across all metrics to ensure ALL targets are satisfied simultaneously.
Present in: pod_autoscaler_system

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.99% for all 7 systems; < 30 s recovery via leader election |
| **API Response** | < 100 ms GET, < 200 ms write, < 1 s large LIST |
| **Network Latency** | < 0.1 ms intra-node, < 1 ms cross-node same-AZ, < 1 ms service mesh sidecar overhead |
| **Reconciliation** | < 5 s operators, < 5 min cluster-scoped operations |
| **Scale** | 5,000 nodes, 150,000 pods, 10,000 services, 100+ tenants, 500+ clusters |
| **Component Overhead** | < 50 MB / < 5% CPU per sidecar; < 256 MB per operator; < 3% CPU for metrics-server |
| **Throughput** | 100+ pods/s scheduling, 50+ objects/s operators, 100+ QPS API server per client |
