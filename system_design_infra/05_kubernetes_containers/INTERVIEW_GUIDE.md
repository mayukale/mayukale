# Infra Pattern 5: Kubernetes & Containers — Interview Study Guide

Reading Infra Pattern 5: Kubernetes & Containers — 7 problems, 9 shared components

---

## STEP 1 — ORIENTATION

This pattern covers the full Kubernetes stack, from Linux kernel primitives up through fleet-scale cluster management. The 7 problems are:

1. **Container Orchestration System** — kubelet, containerd, runc, CNI/CSI/kube-proxy; the data plane where containers actually run
2. **Kubernetes Cluster API Platform** — declarative provisioning and lifecycle management of hundreds of clusters across cloud and bare-metal (CAPI)
3. **Kubernetes Control Plane** — API server, etcd, scheduler, controller-manager; the brain of every cluster
4. **Kubernetes Operator Framework** — custom controllers that encode operational knowledge for stateful systems (databases, message brokers)
5. **Multi-Tenant Kubernetes Platform** — serving 100+ internal teams on a shared cluster with isolation, quotas, and cost attribution
6. **Pod Autoscaler System** — HPA, VPA, KEDA, and Cluster Autoscaler working together to right-size pods and nodes
7. **Service Mesh Design** — Istio/Envoy-based mTLS, traffic management, and observability without changing application code

The 9 shared components that appear across most or all of these problems are: **etcd as distributed state store**, **informer/watch cache pattern**, **level-triggered reconciliation loops**, **admission control (mutating + validating webhooks)**, **RBAC for authorization**, **leader election for HA controllers**, **health checks and probe-triggered remediation**, **Prometheus for observability**, and **Loki/Elasticsearch for log aggregation**.

---

## STEP 2 — MENTAL MODEL

### The Core Idea

Kubernetes is a **declarative desired-state reconciliation system**. You write down what you want (a YAML spec), and a distributed set of control loops continuously works to make reality match that spec. No single component does everything — each controller owns a slice of the state and keeps reconciling its slice forever.

### Real-World Analogy

Think of a large hotel chain's central operations center. The booking system (etcd) stores the authoritative record of every room reservation (desired state). Department managers (controllers) each watch their own slice: housekeeping watches for checked-in rooms that are not yet cleaned, maintenance watches for reported repairs, front desk watches for arrivals that need room assignments (scheduling). Each manager doesn't need to know what the others are doing — they each just reconcile their own pile. When the building itself (a node) has a fire alarm (node failure), that triggers a cascade: housekeeping marks those rooms unavailable, reservations get re-assigned to other rooms, and maintenance dispatches a crew. The system never reaches perfect steady state because reality keeps changing — and that's fine, because the loops keep running.

### Why This Is Hard

Three things make Kubernetes genuinely difficult to design:

**Distributed consistency without a global lock.** Thousands of controllers and kubelets simultaneously reading and writing the same state. The system uses optimistic concurrency (every object has a `resourceVersion` that equals the etcd revision; a write fails if the revision changed since you read it) and level-triggered reconciliation (re-run from scratch, don't assume events are delivered exactly once) to stay correct without a central lock.

**Multi-layer indirection.** A single `kubectl apply` touches the API server admission pipeline, etcd, the scheduler, kubelet, containerd, runc, the CNI plugin, the CSI plugin, kube-proxy, and CoreDNS — all in sequence, each with its own failure modes. Debugging a stuck pod means knowing which layer failed.

**Scale that breaks naive approaches.** At 5,000 nodes and 150,000 pods, even simple things become hard: listing all pods takes special pagination, watching all pods multiplexes 5,700+ watchers on a few TCP connections, and scheduling 100 pods per second requires evaluating only a fraction of nodes instead of all of them.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these first. Each answer changes your design significantly.

**1. What is the primary concern — running workloads, managing clusters, or enabling tenants?**
- Running workloads → focus on container orchestration, data plane, pod scheduling
- Managing a fleet of clusters → focus on Cluster API (CAPI) with multi-provider support
- Shared platform for teams → focus on multi-tenancy, RBAC, quotas, policy enforcement
- Red flag: candidate picks one area and ignores the others exist

**2. What scale are we targeting — nodes, pods, tenants, clusters?**
- Single cluster: 5,000 nodes, 150,000 pods, 10,000 services
- Fleet: 500+ workload clusters, 25,000 machines
- Multi-tenant: 100+ teams, 300+ namespaces
- This drives whether you need etcd tuning, EndpointSlice sharding, scheduler `percentageOfNodesToScore`, or management cluster HA

**3. Are tenants internal (trusted) or external (untrusted)?**
- Internal teams → namespace-per-tenant with RBAC + NetworkPolicy + ResourceQuota is sufficient
- External customers → requires vCluster (virtual control plane per tenant) or cluster-per-tenant; namespace isolation is insufficient for untrusted workloads
- Red flag: proposing namespace isolation for untrusted multi-tenancy

**4. What infrastructure providers do we need — cloud only, bare-metal, hybrid?**
- Cloud only → Cluster API with CAPA/CAPG, Cluster Autoscaler or Karpenter for node scaling
- Bare-metal → Metal3/CAPM3 with IPMI/Redfish; much longer provisioning times (10-20 min vs. 3-5 min); finite hardware pool changes capacity planning entirely
- Hybrid → CAPI pluggable provider model; but multi-provider clusters within one cluster are not natively supported

**5. Do we need a service mesh, or is plain NetworkPolicy sufficient?**
- NetworkPolicy (L3/L4) → sufficient for "pod A can reach pod B on port 8080"
- Service mesh (Istio) → needed for mTLS identity, canary deployments, circuit breaking, L7 authorization, observability with zero app code changes
- What changes: adding a mesh means sidecar injection, xDS config management, certificate lifecycle, and ~1ms p99 latency overhead per hop

**Red flags that should prompt follow-up questions:**
- Proposing `kubectl apply` as the cluster provisioning workflow at scale (no GitOps, no CAPI)
- Treating etcd as a general-purpose database ("we can store app data in etcd")
- Assuming kube-proxy handles service mesh features
- Conflating HPA (horizontal replica scaling) with VPA (vertical resource right-sizing)

---

### 3b. Functional Requirements

**Core scope (what every K8s system must do):**
- Manage container lifecycle: create, start, stop, restart, delete
- Provide resource isolation per container via cgroups v2 (CPU, memory, disk I/O)
- Schedule pods onto nodes respecting resource availability, affinity, taints, and topology constraints
- Provide pod-to-pod networking with a flat model (every pod gets a unique IP)
- Expose service discovery and load balancing (ClusterIP → pod IP via iptables/IPVS/eBPF)
- Authenticate and authorize every API request (RBAC, OIDC)
- Continuously reconcile desired state with actual state

**Scope that varies by problem:**
- Persistent storage lifecycle (CSI: provision, attach, mount, snapshot) — container orchestration
- Fleet-scale cluster provisioning and upgrades across multiple infrastructure providers — CAPI
- CRD-based custom resource management with full lifecycle (create, upgrade, backup, restore, delete) — Operator Framework
- Tenant onboarding automation and cost attribution — Multi-tenancy
- Automatic pod and node scaling without service disruption — Autoscaling
- Automatic mTLS, traffic routing rules, and observability without app changes — Service Mesh

**Clear requirement statement (use this in interviews):**

"We need a Kubernetes platform that can manage [X nodes / X clusters / X tenants], provide strong [isolation / autoscaling / observability], with [target SLA], and support [cloud-only / hybrid / bare-metal] infrastructure. Core NFRs are 99.99% control plane availability, sub-200ms API write latency, and scheduling throughput of 100+ pods per second."

---

### 3c. Non-Functional Requirements

**Derive these from the problem, don't just recite them.**

| NFR | Target | How to derive |
|-----|--------|--------------|
| Availability | 99.99% (all 7 systems) | Control plane down = no new pods scheduled, no machine replacements, no autoscaling. Requires HA etcd (3-5 node Raft) + API server replicas behind LB + leader election for scheduler/controller |
| API latency | GET < 100ms, write < 200ms, LIST < 1s | Comes from kubelet heartbeat frequency (10s) and controller reconcile loops; slow writes stall scheduling |
| Scheduling throughput | ≥ 100 pods/s | Platform SLA; drives `percentageOfNodesToScore` tuning, informer cache requirement |
| Network latency | < 0.1ms intra-node, < 1ms cross-node same-AZ | Pod startup time and service response SLAs; drives CNI plugin choice (Cilium eBPF vs. iptables) |
| Container startup | < 30s cached image, < 120s cold pull | Deployment rollout time and scale-out responsiveness |
| etcd write latency | < 25ms p99 | Every API write goes through etcd; non-SSD etcd is a known cluster killer |
| Scale | 5,000 nodes, 150,000 pods, 10,000 services | Sets etcd memory (~8GB), API server cache (~4-8GB), and EndpointSlice sharding requirement |

**Trade-offs to volunteer in an interview:**

- ✅ Namespace-per-tenant is operationally simple and has near-zero overhead
- ❌ Namespace isolation does not prevent a compromised pod from exploiting a kernel vulnerability affecting other namespaces; use vCluster or cluster-per-tenant for untrusted workloads
- ✅ Sidecar mesh (Istio) gives full L7 observability and mTLS with no app changes
- ❌ Every pod gets an extra container (Envoy), adding ~50MB RAM and ~5% CPU overhead; ambient mesh (Istio's newer sidecarless model) reduces this but is less mature
- ✅ HPA scale-down stabilization window (5 min default) prevents flapping
- ❌ It means you stay over-provisioned for 5 minutes after a traffic drop; tune for your use case
- ✅ CAPI treats clusters as cattle via declarative desired-state management; immutable machine replacement is safer than in-place upgrades
- ❌ Management cluster is a single point of control; its failure stops new provisioning and machine remediation (though running workloads are unaffected)

---

### 3d. Capacity Estimation

**Use this formula framework for a single large cluster:**

```
Nodes:             5,000 nodes
Pods:              5,000 × 30 avg = 150,000 pods
Containers:        150,000 × 1.5 (main + sidecars) = 225,000 containers
API server QPS:    500 (kubelet heartbeats) + 1,000 (controller reconciles) + 17 (users) = ~2,000–3,000 QPS
etcd size:         500,000 objects × 5KB avg = ~2.5GB data, ~7.5GB with WAL; use 50GB SSD
etcd memory:       ~8GB recommended
API server cache:  ~4–8GB RAM (full object set in-memory)
Watch connections: 5,000 kubelets + 500 controllers + 200 users = ~5,700 concurrent watches
Kubelet heartbeat bandwidth: 5,000 × 1KB / 10s = ~500 KB/s to API server
```

**For fleet management (CAPI):**

```
Clusters:          500 workload clusters
Machines:          500 × 50 avg = 25,000 machines
CAPI objects:      500 Cluster + 2,500 MachineSet + 25,000 Machine = ~28,000 objects
Management cluster etcd: 28,000 × ~5 objects each × 5KB = ~137MB (well within limits)
Provisioning burst: 10 clusters × 50 machines = 500 machines in parallel
Cloud API rate limits → key constraint; need exponential backoff in providers
```

**For multi-tenancy:**

```
Tenants:            100 teams
Namespaces:         100 × 3 (dev/staging/prod) = 300 namespaces
Users:              100 × 10 = 1,000 users
RBAC objects:       ~900 (Role + RoleBinding + ServiceAccount per namespace)
Admission webhook QPS: ~500 policy evaluations/s at peak → must be fast (<100ms) and HA
Audit logs:         1,000 users × 50 req/day × 1KB × 90 days = ~4.5GB retention
```

**Architecture implications of these numbers:**

- 5,700 concurrent watch connections → API server needs HTTP/2 multiplexing (not HTTP/1.1)
- 2.5GB etcd with 500K objects → etcd needs dedicated SSD, compaction every 5 min, defrag maintenance
- 500 webhook evaluations/s → webhook server must have 2+ replicas, circuit breaker (failurePolicy: Ignore or Fail), and p99 < 100ms
- 28,000 CAPI objects → management cluster etcd stays well under 8GB limit even at 500 clusters
- 500 machines in parallel provisioning burst → cloud API rate limits are the bottleneck, not the management cluster

**Time to work through in interview:** 3-5 minutes. Do the node/pod/API server QPS math quickly, then slow down on the single number that most constrains your design (usually etcd storage + latency, or API server watch scalability).

---

### 3e. High-Level Design

**The 4-6 components that belong on every K8s whiteboard, and why:**

**1. etcd cluster (3 or 5 nodes, Raft)**
- Single source of truth for all cluster state
- Key layout: `/registry/<resource-type>/<namespace>/<name>`
- Every component reads and writes through the API server, never directly to etcd
- Critical config: SSD-backed, 50GB disk, 8GB RAM, compaction every 5 min
- Without it: the entire cluster loses its brain; all reads and writes fail

**2. API server (stateless, 2+ replicas behind a load balancer)**
- The only entry point into the system; all other components communicate through it
- Pipeline: TLS → Auth → AuthZ → Mutating webhook → Schema validation → Validating webhook → etcd write → Watch broadcast
- Serves the watch API that all controllers and kubelets use to react to changes
- Without it: no new pods, no scaling decisions, no configuration changes; running containers continue unaffected

**3. kube-scheduler + kube-controller-manager (active-standby with leader election)**
- Scheduler: watches for pods with empty `spec.nodeName` → runs Filter → Score → Bind
- Controller-manager: ~30 reconciliation loops (Deployment, ReplicaSet, Node, ServiceAccount, EndpointSlice, etc.)
- Both use Lease objects for leader election; standby instance takes over in < 15s
- Without them: pods stay Pending forever; Deployments never scale; nodes are never cleaned up

**4. kubelet + containerd + runc (one per node)**
- kubelet: the node agent; watches the API server for pod specs assigned to its node
- containerd: CRI-compliant runtime; pulls images, creates container snapshots via overlayfs
- runc: low-level OCI runtime; creates Linux namespaces and cgroups, starts the process
- CRI gRPC connects kubelet to containerd; OCI spec connects containerd to runc
- Without it: containers never actually start on nodes

**5. CNI plugin + kube-proxy + CoreDNS**
- CNI (Calico/Cilium): allocates pod IPs, creates veth pairs, enforces NetworkPolicy; Cilium uses eBPF for < 0.1ms intra-node latency
- kube-proxy: translates ClusterIP → pod IP via iptables/IPVS/eBPF rules; updated within 5s of endpoint changes
- CoreDNS: resolves `my-svc.my-ns.svc.cluster.local` → ClusterIP in < 5ms
- Without them: pods cannot reach each other or services by name

**6. (Problem-specific) Pick one of: CAPI controllers + infrastructure providers / Operator Framework controllers / istiod + Envoy sidecar / HPA + Cluster Autoscaler / Tenant controller + OPA/Gatekeeper**

**Data flow to narrate on whiteboard (pod startup):**
API server assigns pod to node → kubelet watches and receives pod spec → CNI allocates IP + creates veth pair → CSI attaches and mounts volumes → containerd pulls image layers (overlayfs snapshot) → runc creates namespaces + cgroups + starts process → startup probe → readiness probe → endpoint controller adds pod IP to EndpointSlice → kube-proxy/Cilium updates forwarding rules → pod receives traffic.

**Whiteboard order:**
Start with etcd at center. Draw API server connecting to it. Draw load balancer in front of API server. Add scheduler + controller-manager hanging off API server. Then draw a node box with kubelet → containerd → runc. Add CNI and CSI boxes on the side of the node. Finally add the problem-specific overlay (CAPI, mesh, autoscaler, etc.) as a second layer connecting to the API server.

**Key decisions to call out explicitly:**
- etcd: 3 nodes for 1-failure tolerance, 5 nodes for 2-failure tolerance (odd number, Raft quorum)
- CNI choice: Cilium eBPF for better performance and L7 NetworkPolicy; Calico for simpler operations
- kube-proxy mode: iptables (default, works everywhere), IPVS (better for 10K+ services), or Cilium eBPF (replace kube-proxy entirely for lowest latency)
- API server replicas: 2 active (stateless, LB distributes load); more replicas needed for very large watch fan-out

---

### 3f. Deep Dive Areas

#### Deep Dive 1: etcd — Why It Is the Cluster's Achilles Heel

**The problem:** etcd must provide linearizable consistency, efficient watch semantics, and bounded storage for 500K+ objects while maintaining sub-25ms write latency. A single etcd performance issue — slow disk, quorum loss, compaction not running — cascades to the entire cluster becoming unresponsive because every API write blocks waiting for etcd.

**The solution:**

The write path: API server sends gRPC `Put(key, value)` to the etcd leader → leader appends to WAL (fsync to SSD, this is why SSD is mandatory) → replicates to followers via AppendEntries → commits after quorum (2/3 or 3/5) acknowledges → responds to API server with new revision → watch subscribers notified.

The watch path: clients subscribe to a key prefix with a `resourceVersion`. etcd streams all changes from that revision forward. If the revision was compacted (older than the compaction window, default 5 min), etcd returns `ErrCompacted`. The Kubernetes informer handles this by re-listing the resource, getting a fresh baseline, and restarting the watch. This is the key reason informers do full re-lists on reconnect — not a bug, but by design.

Compaction: without it, etcd grows unbounded as every mutation creates a new revision. With it, old revisions are pruned but disk space is not freed until defragmentation. Defrag must be done one node at a time to avoid losing quorum.

**Trade-offs to volunteer unprompted:**

- ✅ etcd's native revision-based watch maps directly to Kubernetes' `resourceVersion` semantics; no translation layer needed
- ❌ etcd is not a general-purpose database; recommended limit is 8GB; beyond that, write latency degrades and the risk of OOM increases; application data should never go in etcd
- ✅ 3-node etcd tolerates 1 failure; 5-node tolerates 2 failures
- ❌ Even-numbered clusters (2 or 4) are a trap: 2 nodes loses quorum on any single failure (need 2/2); 4 nodes still only tolerates 1 failure (needs 3/4) — same fault tolerance as 3 nodes, more overhead
- ✅ Linearizable reads guarantee you see the latest committed write
- ❌ Linearizable reads require the leader to check with followers before responding; for the informer cache, Kubernetes uses serializable reads (potentially stale but faster) which is acceptable since controllers re-reconcile periodically anyway

---

#### Deep Dive 2: kube-scheduler — How It Achieves 100+ Pods/Second

**The problem:** Scheduling must be fast (100+ pods/s), correct (respect all resource, affinity, topology, and taint constraints), and fair (high-priority pods get resources, low-priority pods don't starve indefinitely). Evaluating all 5,000 nodes for every pod is O(n) per pod — way too slow at scale.

**The solution:**

Three-phase pipeline: **Filter** eliminates infeasible nodes (NodeResourcesFit, TaintToleration, NodeAffinity, PodTopologySpread, VolumeBinding, InterPodAffinity) — reduces 5,000 nodes to hundreds of feasible candidates. **Score** ranks feasible nodes 0-100 per plugin (LeastAllocated, BalancedResourceAllocation, ImageLocality, TopologySpread) and sums weighted scores. **Bind** reserves resources, attaches volumes, and patches `pod.spec.nodeName` on the API server.

Key optimization: `percentageOfNodesToScore` (default 50%) — the scheduler stops evaluating more nodes once it has found enough feasible candidates. In a 5,000-node cluster this cuts scoring from 5,000 to ~2,500 nodes per pod. The informer cache means all node/pod state is in-memory; no API calls per scheduling decision.

Preemption flow when no node is feasible: identify lower-priority victim pods on each node → simulate removing them and re-run Filter → pick node with minimum victims → delete victims (with grace period) → reschedule the high-priority pod in next cycle.

Three scheduling queues: `ActiveQ` (ready to schedule, sorted by priority), `BackoffQ` (failed-to-schedule with exponential backoff), `UnschedulableQ` (blocked on unmet conditions, re-evaluated when cluster state changes).

**Trade-offs to volunteer unprompted:**

- ✅ `percentageOfNodesToScore` achieves 100+ pods/s throughput
- ❌ It means the scheduler may not always pick the globally optimal node; for workloads with complex topology requirements, increase this value
- ✅ Gang scheduling (all-or-nothing for ML training jobs) is achievable via the Volcano scheduler or coscheduling plugin
- ❌ Native kube-scheduler has no gang scheduling; if your ML platform needs it, you are running a separate scheduler with its own operational burden
- ✅ `topologySpreadConstraints` distributes pods across AZs for HA
- ❌ With tight `maxSkew: 1`, a single AZ going down can cause new pods to remain Pending because scheduling in the remaining AZs would violate the spread constraint; set `whenUnsatisfiable: ScheduleAnyway` for graceful degradation

---

#### Deep Dive 3: HPA + Cluster Autoscaler — The Two-Loop Scaling System

**The problem:** HPA scales pods horizontally, but if the cluster is already full, new pods sit in Pending state forever. Cluster Autoscaler scales nodes, but it can take 3-5 minutes to provision a new node. Together they form a two-loop feedback system that, if not tuned correctly, either over-provisions constantly or drops requests during spike absorption.

**The solution:**

HPA runs every 15 seconds. It calculates: `desiredReplicas = ceil(currentReplicas × (currentMetricValue / targetMetricValue))`. With multiple metrics, it takes the maximum desired replicas across all metrics to ensure every target is satisfied simultaneously. A ±10% tolerance band prevents micro-adjustments.

Scale-down stabilization window (default 300 seconds): HPA stores every desired-replica calculation for the past 5 minutes and uses the maximum — meaning it only scales down when replicas have been consistently unnecessary, not just during a brief dip. Scale-up has no stabilization (immediate by default).

Cluster Autoscaler checks every 10 seconds for unschedulable pods. If a pod has been Pending for 10+ seconds and a new node would allow it to schedule, CA provisions a node from the appropriate node group (or Karpenter selects an optimally-sized instance type). Scale-in requires a node to be under-utilized for 10+ minutes with all its pods movable (no local storage, no host ports, no PodDisruptionBudget violations).

**Trade-offs to volunteer unprompted:**

- ✅ HPA + CA together handle most dynamic workload scenarios automatically
- ❌ There is an inherent lag: metric breach → HPA decision → new pod → Pending → CA detects → node provision → node ready → pod scheduled → pod startup. Total: 3-7 minutes. For burst-sensitive workloads, pre-warm pods or use Karpenter (faster provisioning with spot instances)
- ✅ VPA in recommendation-only mode gives you right-sizing data without disruption
- ❌ Never run HPA and VPA on the same Deployment in Auto mode simultaneously — they fight each other. VPA in Off or Initial mode alongside HPA is safe
- ✅ KEDA enables scale-to-zero for event-driven workloads (Kafka consumers, job queues) that should not run when idle
- ❌ Scale-to-zero means the first request after idle takes the full pod startup time (< 30s cold start target); if your SLA cannot tolerate this, keep `minReplicaCount: 1`

---

### 3g. Failure Scenarios

These are the failure modes interviewers ask about. Think through them like a senior engineer: identify the blast radius first, then the detection mechanism, then the remediation.

**etcd quorum loss (2 of 3 nodes down):**
Blast radius: entire cluster read-only for writes; scheduling stops; controller reconciliations stop; running containers continue unaffected.
Detection: `etcdctl endpoint health --cluster` fails; API server returns 503 on writes; alert on `etcd_server_has_leader = 0`.
Remediation: restore quorum by restarting the failed members from their data directories; if data is corrupt, restore from etcd snapshot backup. This is why etcd snapshots must be taken to S3/GCS regularly.

**API server certificate expiry:**
Blast radius: all API clients get TLS handshake failures; entire cluster becomes inaccessible (kubectl, controllers, kubelets).
Detection: certificate expiry monitoring (`kubeadm certs check-expiration`), TLS error alerts.
Remediation: rotate certificates with `kubeadm certs renew all`; restart API server components; kubelets also have rotating certificates that need attention.

**Node goes NotReady (kubelet stops heartbeating):**
Detection: node controller waits `--node-monitor-grace-period` (40s default) before marking node NotReady, then `--pod-eviction-timeout` (5min) before adding NoExecute taint and evicting pods.
Remediation: pods evicted and rescheduled by their controllers (Deployment/ReplicaSet); for CAPI-managed nodes, MachineHealthCheck triggers machine replacement within 10min.
Senior framing: discuss PodDisruptionBudgets — if the cluster loses too many nodes simultaneously, evictions may not proceed because PDB says "you can only have 1 disruption at a time." This is correct behavior but requires operational awareness.

**Admission webhook outage (validating webhook unreachable):**
Blast radius: all create/update operations for the matching resource type fail with 503 if `failurePolicy: Fail`; cluster grinds to a halt if it's a broad webhook.
Detection: API server metrics on webhook call failures; sudden spike in admission errors.
Remediation: if `failurePolicy: Ignore`, operations pass and the webhook is silently bypassed; if `failurePolicy: Fail`, fix the webhook urgently. Best practice: use `failurePolicy: Fail` only for security-critical policies; use `failurePolicy: Ignore` for non-security mutations.

**OPA/Gatekeeper overloaded (multi-tenant platform):**
Blast radius: admission latency spikes above 100ms, potentially causing timeouts; API server may reject requests that take too long.
Detection: `admission_webhook_admission_duration_seconds` in API server metrics.
Remediation: scale Gatekeeper controller pods; reduce policy complexity; set resource `limits` appropriately; use `namespaceSelector` to scope policies to relevant namespaces only.

**Management cluster failure (CAPI context):**
Blast radius: cannot provision new clusters, cannot replace unhealthy machines, cannot upgrade clusters. Running workload clusters are completely unaffected — they have their own control planes.
Detection: management cluster health checks fail.
Remediation: restore management cluster from etcd backup; re-run `clusterctl restore`; as prevention, run management cluster as HA (3 control plane nodes) and take regular etcd snapshots to object storage.

---

## STEP 4 — COMMON COMPONENTS

Every component below appears in at least 5 of the 7 systems. Know why each is used, how it is configured, and what fails without it.

---

### 1. etcd as Distributed State Store

**Why used:** etcd is the single source of truth for all Kubernetes state. Its revision-based watch API maps directly to `resourceVersion` for optimistic concurrency. Its Lease primitive powers leader election. Its linearizable consistency ensures all control plane components see the same state.

**Key config:**
- 3 nodes (tolerate 1 failure) or 5 nodes (tolerate 2 failures); always odd for Raft quorum
- Dedicated SSD; `etcd_disk_wal_fsync_duration_seconds p99 < 25ms` is the key health metric
- `--quota-backend-bytes 8G` maximum recommended; monitor `etcd_mvcc_db_total_size_in_bytes`
- Compaction every 5 minutes (`--etcd-compaction-interval`); defrag one node at a time during maintenance
- Regular snapshots to object storage (S3/GCS) for disaster recovery

**What breaks without it:** Every API write fails. The entire cluster becomes read-only. Running containers continue for a while (kubelet cached the pod spec), but any new scheduling, scaling, or configuration change is impossible.

**Appears in:** All 7 systems, directly or as the storage backend for CRDs.

---

### 2. Informer / Watch Cache Pattern

**Why used:** Without an informer cache, every reconciliation loop would make direct API calls — at 50 reconciliations/second across 20 controllers, that is 1,000 QPS just from the controller manager. The informer cache seeds from a full LIST, then maintains state via a streaming watch. Controllers read from the in-memory cache (zero API latency), dramatically reducing API server load.

**Key config:**
- Each controller gets a shared informer for the resource types it cares about
- Cache sync must complete before the controller starts processing events (`WaitForCacheSync`)
- On reconnect (watch disconnect), the informer does a full re-LIST to re-establish baseline — not optional, not a bug
- Bookmark events in the watch stream prevent the client from having to re-LIST on every reconnect

**What breaks without it:** Controller reconciliation loops hammer the API server with direct reads; at scale, API server becomes the bottleneck; watch storms can cause cache invalidation loops (list-watch-list-watch) that degrade the entire cluster.

**Appears in:** Control plane (kube-controller-manager), Operator Framework (controller-runtime), Pod Autoscaler (HPA, VPA), Service Mesh (istiod watches VirtualServices/pods).

---

### 3. Level-Triggered Reconciliation Loops

**Why used:** In an edge-triggered system, missing one event means missing the action. In a level-triggered system, the reconciler re-runs periodically and on any change, comparing desired vs. actual state — so missed events are caught on the next reconciliation cycle. This is why Kubernetes controllers are idempotent by design: the reconciler asks "what do I need to do to make reality match the spec right now?" not "what event just happened?"

**Key config:**
- Reconciliation must be idempotent: `if desired == actual: no-op; if desired != actual: create/update/delete`
- Periodic resync interval (typically 1-10 minutes) ensures drift is caught even if a watch event is missed
- Work queue deduplicates rapid changes to the same object (only one reconciliation runs for 100 rapid updates to the same pod)
- Error handling: return `(Result{RequeueAfter: 5s}, nil)` for recoverable errors; `(ctrl.Result{}, err)` for permanent errors that trigger exponential backoff

**What breaks without it:** You get an edge-triggered system where missing one event (due to crash, network blip, or restart) causes the system to drift from desired state permanently until manual intervention.

**Appears in:** kube-controller-manager (~30 loops), Cluster API controllers, Operator Framework reconcilers, Tenant Controller, Pilot in Istio.

---

### 4. Admission Control (Mutating + Validating Webhooks)

**Why used:** Admission webhooks are the policy enforcement point for every API write. Mutating webhooks run first and can modify objects (inject sidecars, set defaults, add labels). Validating webhooks run after and can reject objects (enforce image registry policies, require resource limits, block privileged containers). This is the last line of defense before objects are persisted to etcd.

**Key config:**
- Order matters: mutating runs before validating
- `failurePolicy: Fail` — reject the request if webhook is unreachable (use for security-critical policies)
- `failurePolicy: Ignore` — allow the request if webhook is unreachable (use for non-security enrichment)
- Timeout: default 10s, recommended 5s; webhooks that take too long block the API pipeline
- `namespaceSelector` and `objectSelector` — scope webhooks to relevant objects only to minimize blast radius and latency
- Webhook servers must be HA (2+ replicas) and run as pods in the cluster (not external dependencies)

**What breaks without it:** No policy enforcement; any pod can use any image, request unlimited resources, run as root, or disable security features. For service mesh, no sidecar injection means no mTLS or observability.

**Appears in:** All 7 systems. Key implementations: OPA/Gatekeeper (multi-tenant), VPA admission controller (autoscaling), Istio sidecar injector (service mesh), CRD validation webhooks (operator framework), Pod Security Admission (container orchestration).

---

### 5. RBAC for Authorization

**Why used:** Every API request that passes authentication goes through RBAC. RBAC uses Role (namespace-scoped) and ClusterRole (cluster-wide) objects that define allowed verbs on resource types, bound to users or service accounts via RoleBindings. RBAC evaluation is cached in the API server and runs in < 5ms.

**Key config:**
- **Least privilege**: operators and controllers should have only the verbs they actually need
- `Role` + `RoleBinding` for namespace-scoped access (tenants, application teams)
- `ClusterRole` + `ClusterRoleBinding` for cluster-wide access (platform operators, cluster-admin)
- Service accounts for pod identity: pods authenticate as their `spec.serviceAccountName`
- For multi-tenancy: deny access between tenant namespaces by using only namespace-scoped Roles; never give tenants ClusterRole
- Audit RBAC regularly: `kubectl auth can-i --list --namespace=team-alpha --as=system:serviceaccount:team-alpha:default`

**What breaks without it:** Any authenticated user can read secrets, modify deployments across all namespaces, or create cluster-admin role bindings. Service accounts would run with default permissions that are often too broad.

**Appears in:** All 7 systems. Extended by Istio's AuthorizationPolicy to L7 path/method/SPIFFE-identity level.

---

### 6. Leader Election for HA Controllers

**Why used:** Kubernetes runs multiple replicas of controllers (scheduler, controller-manager, istiod, KEDA operator, VPA recommender) for high availability. But having multiple active instances of the same controller creates race conditions — two instances could both try to create the same pod. Leader election ensures only one active instance at a time. Standby instances watch the Lease object and take over within < 15 seconds when the leader fails to renew.

**Key config:**
- Lease object in `kube-system` namespace: `leaseObject.spec.leaseDurationSeconds` (default 15s), `renewDeadline` (default 10s), `retryPeriod` (default 2s)
- Active instance: renews Lease every `renewDeadline` period
- Standby instances: watch for Lease to expire (not renewed within `leaseDurationSeconds`)
- Takeover time: < 15s after leader failure under default config
- Not all controllers need leader election: read-only or idempotent controllers are safe to run multiple active instances (e.g., informer-only components)

**What breaks without it:** Multiple active instances cause split-brain: duplicate objects, conflicting reconciliations, unnecessary evictions. Alternatively, running a single instance means a crash leaves the system uncontrolled until manual restart.

**Appears in:** kube-scheduler, kube-controller-manager, cloud-controller-manager, CAPI core controllers, Operator Framework manager, VPA recommender, KEDA operator, istiod.

---

### 7. Health Checks and Probe-Triggered Remediation

**Why used:** Containers can be running (process alive) but unhealthy (application in a bad state, deadlocked, unable to serve traffic). Kubernetes probes provide three distinct signals: **liveness** (is the application alive? if not, restart it), **readiness** (is the application ready to receive traffic? if not, remove from service endpoints), and **startup** (is the application still initializing? if so, don't run liveness checks yet).

**Key config:**
- `startupProbe`: use for slow-starting applications; `failureThreshold × periodSeconds` gives the maximum startup time (e.g., 30 × 10s = 300s); protects liveness from killing a still-booting app
- `livenessProbe`: restart container after `failureThreshold` consecutive failures; restart uses exponential backoff: 10s → 20s → 40s → 5min max
- `readinessProbe`: removes pod from service endpoints on failure; critical for zero-downtime deployments and graceful degradation
- `initialDelaySeconds`: do not set this to work around a slow startup; use `startupProbe` instead
- For CAPI: `MachineHealthCheck` mirrors this pattern at the node level — `unhealthyConditions` triggers machine reboot then replacement; `maxUnhealthy: 40%` circuit breaker prevents cascading deletion of an entire cluster
- For service mesh: Envoy's outlier detection (5 consecutive 5xx → eject from upstream for 30s base time) is the application-layer equivalent

**What breaks without it:** Unhealthy pods keep receiving traffic (bad readiness) or keep running in a broken state without restart (bad liveness). For stateful systems, silent unhealthy states accumulate until a full outage.

**Appears in:** All 7 systems. CAPI has MachineHealthCheck; service mesh has Envoy outlier detection.

---

### 8. Prometheus for Observability Metrics

**Why used:** Prometheus is the native metrics system for Kubernetes. Every component exposes `/metrics` in Prometheus format. The HPA consumes CPU/memory via the Resource Metrics API (backed by metrics-server) and custom metrics via the Custom Metrics API (backed by Prometheus Adapter). Cost attribution, autoscaler decisions, and operational health all depend on time-series metrics.

**Key config:**
- metrics-server: scrapes kubelets every 15s for CPU/memory; keeps only the most recent sample (not historical); lightweight for HPA use
- Prometheus: full time-series retention (15 days default); PromQL for aggregation; per-tenant label cardinality is a common anti-pattern that causes memory blow-up
- Thanos or Cortex for long-term storage and multi-cluster federation
- Recording rules to pre-aggregate expensive queries (cost attribution, per-tenant resource usage)
- Alert on: `etcd_disk_wal_fsync_duration_seconds`, `apiserver_request_duration_seconds`, `scheduler_pending_pods`, `kube_node_status_condition{condition="Ready", status="false"}`

**What breaks without it:** HPA has no metrics to evaluate; VPA cannot build recommendations; cluster operators are flying blind; cost attribution is impossible.

**Appears in:** 6 of 7 systems (all except purely the data plane container orchestration which uses it for host metrics).

---

### 9. Loki / Elasticsearch for Log Aggregation

**Why used:** Container logs are written to node-local files by the container runtime. When a pod is deleted or a node is replaced, those logs are gone. Centralized log aggregation (Fluent Bit → Loki or Elasticsearch) ships logs off-node before they disappear. For multi-tenancy, per-tenant log access control is implemented by indexing on namespace labels. For compliance (SOC2, PCI-DSS), audit logs must be retained for 90 days minimum.

**Key config:**
- Fluent Bit (preferred over Fluentd for lower memory footprint) as DaemonSet on each node
- Loki for Kubernetes-native log storage (label-based queries match Kubernetes' namespace/pod/container labels naturally)
- Elasticsearch for full-text search across audit logs and Envoy access logs
- Log retention: container logs typically 7 days on-node; centralized retention 30-90 days
- Audit policy: configure `--audit-policy-file` on API server; log all writes at RequestResponse level; reads at Metadata level to manage volume

**What breaks without it:** No visibility into why containers crashed, what requests a service processed, or who changed a resource. Compliance audit trails are impossible.

**Appears in:** 5 of 7 systems (most prominently multi-tenant platform and service mesh for access logs).

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

For each problem, here is what makes it different from the others, and what unique decisions you face.

---

### Container Orchestration System

**What is unique here:**
The focus is on what happens on the node — the actual Linux primitives that make containers work. The key differentiator is the **cgroup v2 QoS + eBPF networking** stack: OOM eviction priority is computed as `(memory_usage - memory_requests) / memory_limit × 999` for Burstable containers, meaning the most over-budget container is evicted first, protecting Guaranteed pods. cgroup v2 `memory.min` provides kernel-enforced memory reservation that cgroup v1 couldn't do cleanly. Cilium enforces NetworkPolicy in eBPF kernel space, cutting intra-node latency below 0.1ms versus iptables at >1ms.

**Decisions unique to this problem:**
- Container runtime choice: containerd (standard, Docker removed in k8s 1.24) vs. gVisor/Kata (VM-level isolation for untrusted workloads, 5-15% CPU overhead)
- CNI choice: Cilium eBPF (better performance, L7 NetworkPolicy) vs. Calico (simpler, widely supported) vs. Flannel (simplest, no NetworkPolicy)
- kube-proxy mode: iptables vs. IPVS (better for 10K+ services) vs. Cilium replacing kube-proxy entirely
- Pod density tuning: 110 pods/node default → 250 with kernel tuning (`max-pods`, `pids.max`, `inotify` limits)

**Two-sentence differentiator:** Container orchestration is the only problem focused on the node-level Linux stack — cgroups, namespaces, overlayfs, veth pairs, and eBPF programs running in the kernel. While other problems assume containers "just work," this problem is about why they work and how to tune and debug the 7 layers between a pod spec and a running process.

---

### Kubernetes Cluster API Platform

**What is unique here:**
The focus shifts from running workloads to provisioning the infrastructure that runs workloads — treating Kubernetes clusters themselves as declaratively managed resources. The key differentiator is the **pluggable infrastructure provider abstraction + MachineHealthCheck circuit breaker**: the CAPI provider interface standardizes cluster CRUD across AWS (CAPA), GCP (CAPG), vSphere (CAPV), and bare-metal (Metal3/CAPM3) without changing management logic. `maxUnhealthy: 40%` stops automated remediation when too many machines are already unhealthy, preventing a cascade that deletes an entire cluster.

**Decisions unique to this problem:**
- Management cluster bootstrap: kind cluster → `clusterctl init` → pivot (`clusterctl move`) to real management cluster
- Bare-metal vs. cloud provisioning: cloud takes 3-5 min per machine; bare-metal PXE takes 10-20 min; this changes cluster SLA and capacity planning
- ClusterClass topology vs. hand-crafted manifests: ClusterClass enables self-service with guardrails; hand-crafted gives more flexibility but no standardization
- GitOps integration: Flux or ArgoCD watches Git for cluster manifests; provides audit trail and PR-based approval workflow for cluster changes

**Two-sentence differentiator:** Cluster API is the only problem where the Kubernetes API is used to manage other Kubernetes clusters, not applications — the management cluster runs controllers that provision, upgrade, and remediate workload cluster machines. Unlike the other problems, the primary concern is provisioning latency (minutes not milliseconds), cloud API rate limits as a throughput bottleneck, and the finite pool of pre-registered hardware in bare-metal environments.

---

### Kubernetes Control Plane

**What is unique here:**
The focus is on the brain of a single cluster — how the API server processes requests, how etcd stores and watches state, and how the scheduler makes placement decisions at high throughput. The key differentiator is the **optimistic concurrency (resourceVersion) + Watch HTTP/2 multiplexing + percentageOfNodesToScore** combination: `resourceVersion = etcd modRevision` provides lock-free concurrent updates (a write fails if the object was modified since you read it, forcing a retry with fresh state); Watch over HTTP/2 multiplexes thousands of controller watchers on a few TCP connections; scheduling 100+ pods/s is achieved by evaluating only 50% of nodes and stopping the filter phase early.

**Decisions unique to this problem:**
- etcd cluster size: 3 (tolerate 1 failure) vs. 5 (tolerate 2 failures, more replication overhead)
- API server sharding: for very large clusters (>5,000 nodes), consider API server federation or watch cache sharding
- Scheduler extension points: default scheduler with scheduling framework plugins vs. Volcano scheduler for batch/ML workloads requiring gang scheduling
- Controller throughput tuning: `--concurrent-*-syncs` flags control parallelism per controller type

**Two-sentence differentiator:** The control plane is the only problem where you must reason about the CAP properties of the distributed system itself — linearizable vs. serializable reads, Raft quorum calculations, and watch stream semantics. Every other problem sits on top of the control plane and assumes it works; the control plane problem asks how to make it work correctly and efficiently at scale.

---

### Kubernetes Operator Framework

**What is unique here:**
The focus is on encoding operational knowledge into software so complex stateful systems (databases, message brokers, monitoring stacks) manage themselves through their full lifecycle: create, update, upgrade, backup, restore, delete. The key differentiator is the **idempotent sub-resource diff reconciliation + finalizer-controlled deletion**: each reconcile iteration builds the entire desired state from scratch (`build desired → compare → create/update/no-op` per sub-resource), making it safe to re-run after a crash without duplication; finalizers block Kubernetes' deletion of the parent object until the operator has cleaned up all external resources (cloud snapshots, DNS records, database users) — without this, the parent is deleted before cleanup completes.

**Decisions unique to this problem:**
- SDK choice: controller-runtime (Go, most performant), Java Operator SDK, or kopf (Python) — Go is preferred for production operators
- Reconciliation sub-resource ordering: ConfigMap → Secrets → Services → StatefulSet → application config → CronJob (backup) — order matters for dependencies
- CRD versioning strategy: v1alpha1 → v1beta1 → v1 with conversion webhooks for schema migration
- Status conditions vs. phases: prefer typed conditions (`type: Ready, status: True/False/Unknown, reason: string`) over opaque phase strings; `observedGeneration` distinguishes stale status from current

**Two-sentence differentiator:** The Operator Framework is the only problem where you are designing the programming model and conventions for Kubernetes extension, not just using Kubernetes — you must decide how to model a complex system as a CRD hierarchy, how to handle partial failures during multi-step reconciliation, and how to safely delete resources that span Kubernetes and external cloud infrastructure. The key insight is that all correctness comes from idempotent comparison in every reconcile cycle, not from tracking which operations have already been done.

---

### Multi-Tenant Kubernetes Platform

**What is unique here:**
The focus is on serving multiple teams safely and fairly on shared infrastructure, which requires a defense-in-depth approach where each layer independently blocks different attack vectors. The key differentiator is the **5-layer defense stack + vCluster option + Tenant Controller automation**: (1) RBAC constrains API access, (2) NetworkPolicy provides default-deny isolation, (3) ResourceQuota + LimitRange prevents resource exhaustion, (4) Pod Security Standards Restricted + OPA/Gatekeeper block privilege escalation, (5) optional dedicated node pool with taints provides physical separation for sensitive tenants. vCluster provides virtual Kubernetes control planes (one per tenant, running as pods in the host cluster) for teams that need API-level isolation without dedicated hardware cost.

**Decisions unique to this problem:**
- Tenancy model: namespace-per-tenant (internal, trusted teams) vs. vCluster (semi-trusted, need API isolation) vs. cluster-per-tenant (untrusted, external customers, compliance-mandated)
- Policy engine: OPA/Gatekeeper (ConstraintTemplate + Constraint CRD model, mature) vs. Kyverno (YAML-native policies, simpler for K8s admins)
- Cost attribution: Kubecost or OpenCost for per-namespace/per-pod cost attribution; requires accurate resource request data (why LimitRange matters)
- Tenant onboarding: manual vs. Tenant Controller operator (GitOps-triggered namespace + RBAC + quota provisioning in < 30 min)

**Two-sentence differentiator:** Multi-tenant platform is the only problem that is primarily about security and fairness rather than functionality — the question is not "how do we make containers run" but "how do we ensure one team can't disrupt, access, or exhaust resources belonging to another team while sharing the same Kubernetes cluster." The fundamental tension is between isolation (stronger = more overhead and complexity) and efficiency (sharing resources means some blast radius is unavoidable without vCluster or cluster-per-tenant).

---

### Pod Autoscaler System

**What is unique here:**
The focus is on designing the feedback loop that connects workload demand to resource allocation at both the pod and node level, without oscillation or service disruption. The key differentiator is the **HPA stabilization window + VPA 8-day P90 histogram + KEDA scale-to-zero**: the stabilization window takes the maximum desired replicas over a 5-minute window before scaling down — preventing flapping; VPA builds a weighted histogram of 8 days of CPU/memory usage with P90 as the target (P50 lower bound, P95+15% upper bound) to handle variability without over-provisioning; KEDA enables event-driven workloads to run at zero replicas when idle and scale up in < 30s on lag detection.

**Decisions unique to this problem:**
- HPA vs. VPA vs. both: do not run both in Auto mode simultaneously on the same Deployment; use VPA in recommendation-only mode to gather right-sizing data, then apply recommendations manually or with CI/CD
- Karpenter vs. Cluster Autoscaler: Karpenter provisions right-sized nodes on-demand (faster, spot-instance aware, better bin-packing) vs. CA which manages pre-defined node groups (more predictable, wider cloud support)
- Scale-to-zero trade-off: KEDA can reduce cost to zero during off-hours but introduces cold-start latency; keep `minReplicaCount: 1` for latency-sensitive services
- Predictive scaling: for known traffic patterns (daily business hours spike), use KEDA's cron scaler to pre-warm before the spike hits

**Two-sentence differentiator:** Pod autoscaling is the only problem where you must reason about control theory — feedback loops, stabilization windows, and the risk of oscillation — and the only one where three separate controllers (HPA, VPA, KEDA) must coexist without conflicting. The core complexity is not any individual component but their interaction: HPA and VPA fight over resource requests, HPA and Cluster Autoscaler form a two-loop system with a 3-7 minute end-to-end latency that must be acceptable for your workload's burst tolerance.

---

### Service Mesh Design

**What is unique here:**
The focus is on adding networking intelligence (mTLS identity, traffic routing, circuit breaking, observability) as infrastructure rather than application code. The key differentiator is the **SPIFFE identity + ADS ordered hot-reload + 80%-TTL certificate rotation**: SPIFFE identity `spiffe://cluster.local/ns/<ns>/sa/<sa>` cryptographically binds a certificate to a Kubernetes ServiceAccount — workload identity requires no application code changes; the ADS (Aggregated Discovery Service) single gRPC stream delivers LDS+RDS+CDS+EDS+SDS in consistent order, preventing partial config states where a route points to a cluster that hasn't been created yet; 80%-TTL rotation means Envoy starts getting a new certificate at 80% of its 24-hour TTL, holding both old and new during the transition — no connection drops.

**Decisions unique to this problem:**
- Sidecar mode vs. ambient mode: sidecar (Envoy per pod) gives per-pod L7 policy with ~50MB/pod overhead; ambient mesh (ztunnel L4 + waypoint proxy L7) eliminates per-pod sidecars but is newer and less battle-tested
- Certificate authority: built-in Istio CA (simplest), external Vault PKI (audit, compliance), or SPIRE (multi-cluster federation with workload attestation)
- Multi-cluster mesh: flat network model (pods can reach each other directly, shared trust domain) vs. gateway model (all cross-cluster traffic through ingress/egress gateways, separate trust domains)
- Observability granularity: tracing at 100% sampling burns storage fast (6.5TB/day for 75K spans/s); use 1% sampling by default with adaptive sampling for error traces

**Two-sentence differentiator:** Service mesh is the only problem where the networking layer itself becomes programmable — VirtualService, DestinationRule, and AuthorizationPolicy are not Kubernetes networking primitives but a full traffic management API that happens to be stored in etcd. Unlike NetworkPolicy which operates at L3/L4 with binary allow/deny semantics, the service mesh operates at L7 and supports weighted routing (canary deployments), fault injection (chaos engineering), circuit breaking, and cryptographic identity-based authorization.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (Expect These Early in the Interview)

**Q1: What is the difference between a liveness probe, readiness probe, and startup probe?**

**KEY PHRASE: liveness = restart; readiness = remove from endpoints; startup = protect liveness during boot.** A liveness probe failure causes the kubelet to restart the container — use it when your app can get into a state it cannot recover from (deadlock, out of memory, hung process) but the process itself is still running. A readiness probe failure removes the pod's IP from the service's EndpointSlice — use it when your app is temporarily overloaded or initializing, and you don't want new traffic until it recovers. A startup probe delays liveness evaluation entirely — use it for apps with slow startup (> 30s) so liveness doesn't restart a still-booting container. Never rely on `initialDelaySeconds` alone for slow-starting apps; use `startupProbe` with `failureThreshold × periodSeconds` equal to your maximum expected startup time.

**Q2: What happens when a pod is deleted? Walk me through the lifecycle.**

**KEY PHRASE: deletionTimestamp → finalizer drain → SIGTERM → grace period → SIGKILL → kubelet cleanup.** `kubectl delete pod` sets `metadata.deletionTimestamp` on the pod object in etcd. The pod is not immediately gone. If the pod has finalizers, each finalizer must be removed by its owner before the API server permanently deletes the object. Simultaneously, kubelet sends SIGTERM to the container's main process and starts the grace period (default 30s). kube-proxy/Cilium removes the pod's IP from service endpoints immediately (so no new traffic reaches it). After grace period, if the process hasn't exited, kubelet sends SIGKILL. Then kubelet calls the CNI plugin to tear down networking, calls the CSI plugin to unmount volumes, and removes the pod from the node.

**Q3: How does Kubernetes service discovery work? What does CoreDNS do?**

**KEY PHRASE: ClusterIP is the stable DNS-resolvable IP; CoreDNS maps service name to ClusterIP; kube-proxy maps ClusterIP to pod IP.** Every Service gets a stable ClusterIP allocated from the service CIDR. CoreDNS is running as pods in `kube-system` and handles DNS lookups within the cluster. When pod A queries `my-svc.my-ns.svc.cluster.local`, CoreDNS resolves it to the service's ClusterIP (e.g., `10.96.0.42`). When the TCP packet hits that IP, kube-proxy's iptables/IPVS/eBPF rules DNAT-rewrite the destination to one of the backing pod IPs. The key insight is that DNS resolution and load balancing are separate: CoreDNS only resolves to ClusterIP, and the actual load balancing to a specific pod happens at the kernel networking level via kube-proxy rules.

**Q4: What is the difference between requests and limits for CPU and memory?**

**KEY PHRASE: requests drive scheduling and cgroup weight; limits drive throttling (CPU) and OOM kill (memory).** Resource requests are the scheduler's input: the scheduler only places a pod on a node with enough unallocated CPU and memory to satisfy all requests. Requests also set the cgroup `cpu.weight` (proportional shares during contention) and `memory.min` (kernel-guaranteed reservation). Limits set the hard ceiling: CPU limits are enforced via CFS bandwidth throttling (the container is throttled — not killed — if it exceeds its CPU limit); memory limits trigger OOM kill if the container exceeds them. A common mistake is setting limits without requests, which causes the scheduler to place pods as if they need zero resources, leading to node overload.

**Q5: What is the difference between a Deployment, StatefulSet, and DaemonSet?**

**KEY PHRASE: Deployment = stateless replicas; StatefulSet = stable identity and storage; DaemonSet = one pod per node.** A Deployment manages a ReplicaSet of identical, interchangeable pods — pods get random names, and when a pod dies it is replaced with a new pod (possibly on a different node). A StatefulSet gives each pod a stable identity (ordered names like `db-0`, `db-1`), a stable network hostname, and its own PersistentVolumeClaim that follows the pod if it is rescheduled — essential for databases, Kafka brokers, and anything with local state. A DaemonSet ensures exactly one pod runs on every matching node — used for infrastructure agents like Fluent Bit (log collection), Cilium (CNI), and metrics-server components.

**Q6: What is RBAC and how does it work?**

**KEY PHRASE: Role defines allowed verbs on resources; RoleBinding binds Role to subjects; evaluated in < 5ms from cache.** RBAC (Role-Based Access Control) uses two object pairs: `Role` (namespaced) or `ClusterRole` (cluster-wide) defines what actions (`verbs: get, list, watch, create, update, delete, patch`) are allowed on which `resources` in which `apiGroups`. `RoleBinding` or `ClusterRoleBinding` grants that role to `subjects` (Users, Groups, ServiceAccounts). Every API request is checked against RBAC rules after authentication. Rules are cached in the API server and evaluated in < 5ms. For multi-tenancy, tenants should only have `Role` + `RoleBinding` scoped to their own namespaces — never `ClusterRole` bindings.

**Q7: How does the Kubernetes scheduler decide where to place a pod?**

**KEY PHRASE: Filter eliminates infeasible nodes; Score ranks feasible nodes; Bind assigns the pod.** The scheduler runs a three-phase pipeline. Filter applies constraint plugins to eliminate nodes that cannot run the pod: NodeResourcesFit checks CPU/memory availability, TaintToleration checks taints and tolerations, NodeAffinity checks label requirements, VolumeBinding checks if required PVs can be bound. Score ranks the remaining feasible nodes 0-100 using heuristics: LeastAllocated prefers less-loaded nodes, ImageLocality prefers nodes that already have the container image. Bind patches `pod.spec.nodeName` on the API server, which kubelet picks up via watch and begins container creation.

---

### Tier 2 — Deep Dive Questions (Expect These After HLD)

**Q1: Why does Kubernetes use etcd instead of a regular database like PostgreSQL? What would you lose with PostgreSQL?**

**KEY PHRASE: revision-based watch is the fundamental primitive that maps to resourceVersion.** etcd was co-designed with Kubernetes around three primitives that PostgreSQL does not provide natively. First, revision-based watch: every write to etcd increments a global monotonic revision, and clients can subscribe to key prefixes and receive all changes from a specific revision forward — this maps exactly to `resourceVersion` for optimistic concurrency and watch resumption. PostgreSQL would require polling or a `LISTEN/NOTIFY` shim that cannot natively resume from a specific offset. Second, Lease objects for leader election are built into etcd's protocol. Third, linearizable reads are the default for etcd; PostgreSQL is serializable by default which admits a different set of anomalies. k3s does use PostgreSQL via the `kine` shim, and it works well at < 500 nodes, but the translation layer adds latency and the lack of native watch semantics means the shim must poll internally — acceptable for small clusters but not for thousands of nodes.

✅ etcd is purpose-built for Kubernetes, native watch, native lease, linearizable by default  
❌ etcd is not a general-purpose database; 8GB limit, no SQL querying, tight operational requirements (SSD, compaction schedule)

**Q2: How does the HPA stabilization window work, and why does it use the maximum desired replicas rather than the latest?**

**KEY PHRASE: stabilization window takes the maximum to be conservative about scale-down, preventing flapping.** The HPA runs its evaluation loop every 15 seconds and calculates `desiredReplicas = ceil(currentReplicas × (currentValue / target))`. For scale-up, it acts immediately on the latest calculation. For scale-down, it maintains a rolling window of all desired-replica calculations over the last `stabilizationWindowSeconds` (default 300s) and takes the maximum value across that window. Taking the maximum means: "I am only willing to scale down to a level that would have been correct for the entire past 5 minutes." A brief traffic dip that lasted 30 seconds does not trigger a scale-down if traffic was high for the other 270 seconds. This prevents the oscillation pattern of: spike → scale up → dip → scale down → spike → scale up... Taking the minimum (most aggressive) would maximize cost savings but cause constant pod churn, each scale event causing brief service degradation during pod startup.

**Q3: Explain the Cluster API machine replacement lifecycle. What is maxUnhealthy and why does it matter?**

**KEY PHRASE: MachineHealthCheck circuit breaker stops automated deletion when the cluster is already sick.** When a MachineHealthCheck controller detects a machine that has had `NodeReady=Unknown` for > `nodeStartupTimeout` (e.g., 300s), it marks the Machine as needing remediation. For a machine that has a remediation template configured, it first attempts an IPMI reboot (for bare-metal). If the machine is still unhealthy after the boot timeout, the Machine object is deleted. The owning MachineSet then creates a replacement Machine, which triggers the infrastructure provider to provision a new VM or re-provision the bare-metal host. The `maxUnhealthy` field (e.g., `40%`) is a circuit breaker: if more than 40% of machines in the MachineHealthCheck scope are already unhealthy, the controller stops performing any automated remediation. Without this, a cluster-wide network partition that makes all nodes appear unhealthy would cause the MachineHealthCheck controller to delete every machine in the cluster simultaneously — turning a network issue into a full cluster destruction event.

**Q4: How does the Kubernetes admission webhook pipeline work? What happens if a mutating webhook and a validating webhook disagree?**

**KEY PHRASE: mutating runs first and can change the object; validating runs after mutation and cannot change anything, only allow or reject.** The pipeline: after authentication and authorization, the API server calls all mutating admission webhooks in order. Each webhook receives the object and can return a modified version via JSON patch. After all mutating webhooks have run, the API server applies the merged object. Then it calls all validating admission webhooks, which receive the final mutated object and return only allow/deny decisions. A conflict scenario: a mutating webhook sets `image: approved-registry/app:latest`; a validating webhook checks `if image not in approved_registries: reject`. Because the mutating webhook ran first, the validating webhook sees the already-modified image and allows it. The key gotcha is that mutating webhooks run in an undefined order relative to each other, and each one receives the object as modified by the previous — side effects between mutating webhooks require careful ordering via `reinvocationPolicy: IfNeeded`.

**Q5: How does Istio's xDS config propagation work, and what is the ADS ordering guarantee?**

**KEY PHRASE: ADS ensures LDS-before-RDS-before-CDS-before-EDS ordering to prevent partial config states.** istiod watches Kubernetes objects (VirtualService, DestinationRule, pods, Services) via informers. When a change is detected, Pilot translates it into Envoy xDS configuration objects: LDS (listeners — what ports to listen on), RDS (routes — URL paths to clusters), CDS (clusters — upstream service definitions with load balancing policies), EDS (endpoints — the actual IP:port pairs for each cluster). These are pushed to affected Envoy proxies via the ADS (Aggregated Discovery Service) — a single bidirectional gRPC stream. ADS guarantees that LDS is pushed before RDS (routes exist before listeners that reference them), CDS before EDS (cluster definitions exist before their endpoints). Without this ordering, Envoy could receive a routing rule pointing to a cluster name that hasn't been defined yet, causing traffic errors. Envoy hot-reloads the new config without dropping existing connections.

**Q6: What is the difference between vCluster and namespace isolation for multi-tenancy?**

**KEY PHRASE: namespaces share a control plane; vCluster gives each tenant a virtual API server they fully control.** With namespace isolation, all tenants share the same API server, etcd, scheduler, and controller-manager. A ResourceQuota prevents over-consumption, RBAC prevents cross-namespace access, and NetworkPolicy prevents network communication — but all tenants can still see cluster-scoped resources (nodes, PersistentVolumes, ClusterRoles) and a misconfigured RBAC or admission policy could grant unintended access. With vCluster, each tenant gets a virtual Kubernetes cluster running as a pod inside the host cluster — their own API server (k3s or k8s), their own etcd, their own controller-manager. Tenants `kubectl apply` to their virtual cluster; a Syncer component translates their resources into real workloads on the host cluster. Tenants see a fully empty cluster and cannot enumerate host cluster resources at all. The tradeoff: vCluster adds ~100ms per API request latency, consumes additional resources per tenant (the virtual control plane), and requires operating the vCluster operator — but provides genuine API-level isolation without dedicated physical hardware.

**Q7: What are the three autoscalers and how do you prevent them from conflicting?**

**KEY PHRASE: HPA scales replicas, VPA scales resource requests — never run both in Auto mode simultaneously on the same workload.** HPA (Horizontal Pod Autoscaler) adjusts `spec.replicas` on a Deployment or StatefulSet based on metric thresholds. VPA (Vertical Pod Autoscaler) adjusts `spec.containers[].resources.requests` based on historical usage histograms. KEDA scales replicas based on external triggers (Kafka lag, queue depth, cron schedules). HPA and VPA conflict because HPA reads average per-pod CPU (which is total CPU divided by replicas) — if VPA increases the CPU request, HPA sees lower utilization and scales down replicas, which increases per-pod utilization, which triggers HPA to scale up... an oscillation loop. The safe configuration is: VPA in `Off` mode (recommendation only — read and apply manually or via CI/CD), alongside HPA. For event-driven workloads (KEDA + Cluster Autoscaler): KEDA scales from 0 to N replicas; Cluster Autoscaler handles node provisioning when N replicas can't fit. These two coexist naturally because they operate at different layers.

---

### Tier 3 — Staff+ Stress Test Questions (Reason Aloud)

**Q1: You have a 5,000-node cluster where the scheduler is taking 30+ seconds to place pods. API server GET latency is under 100ms and etcd write latency is under 25ms. Diagnose and resolve.**

*Reason aloud like this:* First, rule out the obvious bottlenecks. etcd and API server are healthy, so the write path is not the issue. Scheduler delay means the Filter → Score → Bind cycle is slow. Profile the scheduler: what is `scheduler_scheduling_algorithm_duration_seconds` showing? Sub-problems: (1) Filter phase with complex `topologySpreadConstraints` or `InterPodAffinity` rules causes O(n×m) evaluation where n is candidates and m is existing pods — check whether affinity rules reference many pods. (2) `percentageOfNodesToScore` set too high (100%) for a 5,000-node cluster — at 100 pods/s, scoring all 5,000 nodes for every pod is 500K scoring operations per second. Reduce to 5-10%. (3) VolumeBinding filter doing external calls for volume provisioner — check if CSI provisioner is slow. (4) Webhook timeout — a slow mutating webhook called for every pod can add 5-10s per pod. Fix: (a) reduce `percentageOfNodesToScore` to 10%, (b) profile and simplify affinity rules or use `topologySpreadConstraints` only for AZ-level spread, (c) audit webhooks for slow response times, (d) if ML/batch workloads need gang scheduling, migrate them to a separate scheduler (Volcano) rather than blocking the default scheduler.

**Q2: Your multi-tenant Kubernetes platform has 100 tenants. A single tenant's workload is causing OOMKilled events on a node that also runs workloads for other tenants. Explain the full blast radius and how your platform design prevents and contains this.**

*Reason aloud:* The OOMKilled event means a container exceeded its memory limit. The kernel OOM killer is invoked to free memory. Without proper design, the OOM killer could kill containers belonging to other tenants on the same node. Kubernetes' OOM score algorithm prioritizes eviction in order: BestEffort (score 1000) → Burstable (score 0-999 based on how far over their request they are) → Guaranteed (score -997, last to be killed). Prevention layer 1: LimitRange ensures every tenant container has a memory limit — without limits, containers could use unlimited node memory. Prevention layer 2: ResourceQuota caps total memory requests and limits across the namespace — even if one container uses its full limit, total tenant memory is bounded. Prevention layer 3: if the tenant's node pool is shared, ensure their pods are `Restricted` security standard (no hostPath volumes that could DoS the node's disk). Prevention layer 4: for sensitive tenants (PCI scope), use a dedicated node pool with taints and tolerations — their pods never land on shared nodes. Investigation: check `kubectl describe node` for memory pressure conditions and `kubectl top pods -A --sort-by=memory` to see which tenant is consuming most memory. If it's a ResourceQuota miss, check `kubectl describe resourcequota -n <tenant-ns>`.

**Q3: Design a zero-downtime upgrade strategy for a 500-cluster fleet managed by Cluster API. Clusters have different Kubernetes versions, different infrastructure providers, and different compliance tiers (some require upgrade approval, some can auto-upgrade).**

*Reason aloud:* This is a fleet upgrade problem, not a single-cluster problem. Key design decisions: (1) **Classification**: tag clusters by compliance tier (auto-upgrade OK, approval required, frozen) and by provider (AWS, vSphere, bare-metal — different upgrade latencies). (2) **GitOps as the control plane**: store desired Kubernetes version per cluster in Git. Upgrades are PR-based: platform automation opens a PR to bump version, which triggers review → approval → merge → Flux syncs to management cluster. For auto-upgrade clusters, the PR is auto-merged after validation gates pass. (3) **Wave-based rollout**: upgrade clusters in waves — first 1 canary cluster per provider → observe for 24h → next 5% of clusters → observe → 25% → 50% → 100%. Use CAPI `MachineDeployment` rolling update strategy with `maxSurge: 1, maxUnavailable: 0` for zero-downtime worker upgrades. (4) **PodDisruptionBudgets**: mandate all tenant Deployments have PDBs so CAPI drain does not evict too many pods at once. (5) **Control plane upgrade first**: KubeadmControlPlane rolling-replaces control plane nodes one at a time — new node up and Ready before old node removed. (6) **Failure gate**: if any cluster upgrade stalls (node not Ready within 30 min on cloud, 60 min on bare-metal) or if cluster health checks fail post-upgrade, halt the wave and alert. (7) **Version skew policy**: Kubernetes allows workers to be 2 minor versions behind the control plane; upgrade control plane first, then workers, never the other way.

**Q4: A developer says "my pod shows Running but the application is not responding to requests." Walk me through your systematic diagnosis using only kubectl and standard Kubernetes tooling.**

*Reason aloud:* "Running" means the process is alive, but Running ≠ healthy or ready. Systematic approach: (1) **Check readiness**: `kubectl get pod <pod> -o wide` — is `READY` column showing `1/1` or `0/1`? If `0/1`, the readiness probe is failing. `kubectl describe pod <pod>` shows which probe is failing and the last failure message. (2) **Check endpoints**: `kubectl get endpoints <service> -n <ns>` — is the pod's IP in the endpoints list? If not, readiness probe is failing or the pod's labels don't match the service selector. (3) **Check logs**: `kubectl logs <pod> --previous` (if it restarted) and `kubectl logs <pod>` — look for startup errors, panic, or exception logs. (4) **Check probes**: `kubectl describe pod` shows `liveness/readiness probe` history and failure messages. (5) **Exec into pod**: `kubectl exec -it <pod> -- curl localhost:8080/healthz` — can the process serve its own health endpoint? If yes but readiness probe still fails, check probe configuration (wrong path, wrong port). (6) **Check network policy**: `kubectl get networkpolicies -n <ns>` — is there a default-deny policy blocking the health check path? (7) **Check service mesh**: if Istio is enabled, `istioctl proxy-config routes <pod>` and `istioctl x authz check <pod>` — is there an AuthorizationPolicy blocking the health check? (8) **Check resource pressure**: `kubectl top pod <pod>` — is the container CPU-throttled (hitting its CPU limit) causing slow responses?

**Q5: Your Istio service mesh is propagating config changes (VirtualService edits) in 30+ seconds instead of the < 5s target. The cluster has 1,000 pods in the mesh. Diagnose.**

*Reason aloud:* xDS config propagation delay can be caused by multiple bottlenecks in the istiod → Envoy pipeline. Start with `istioctl proxy-status` to see which proxies are out of sync. (1) **istiod CPU/memory saturation**: with 1,000 pods, istiod holds 1,000 xDS connections. Each config change triggers a push to all affected proxies. `kubectl top pod -n istio-system -l app=istiod` — is istiod CPU-throttled? If so, increase CPU limits or add more istiod replicas. (2) **xDS push size**: `pilot_xds_pushes` and `pilot_xds_push_time` metrics in Prometheus. If a single VirtualService change is triggering full pushes to all 1,000 proxies instead of only affected ones, istiod may not be doing selective pushes correctly — check Istio version (selective push optimization improved in 1.13+). (3) **Envoy processing time**: Envoy must receive, validate, and apply the new config. `envoy_cluster_manager_cds_update_time` metric on the proxy — is Envoy slow to process? Check for very large Envoy configurations (many clusters, many routes) that slow down parsing. (4) **ADS stream backpressure**: if the network between istiod and Envoy is congested (< 1ms expected), pushes can queue. Check `pilot_xds_push_context_errors` and `pilot_xds_write_timeout`. (5) **istiod leader re-election**: if istiod is flapping between leader/follower, config distribution stalls during election. Check `pilot_xds_pushes_total` rate for sudden drops.

---

## STEP 7 — MNEMONICS AND OPENING LINES

### Mnemonic 1: The K8s Data Plane Stack (top to bottom) — "Every Container Really Needs CNI CSI"

- **E**very — Endpoints (Service → EndpointSlice → pod IP via kube-proxy)
- **C**ontainer — Container (containerd manages images and container lifecycle)
- **R**eally — Runtime (runc creates namespaces and cgroups)
- **N**eeds — Networking (CNI plugin: IP allocation, veth pair, NetworkPolicy)
- **C**NI — CoreDNS (in-cluster DNS, `svc.cluster.local`)
- **C**SI — CSI (persistent volume attach, mount, provision)
- **S**torage — StorageClass + PVC + PV (the storage hierarchy)
- **I**nfra — Informer (kubelet watches API server for its pod specs)

### Mnemonic 2: The CAPI Object Hierarchy mirrors the Deployment → ReplicaSet → Pod pattern

- **Cluster** ↔ Deployment (top-level desired state)
- **MachineDeployment** ↔ Deployment (manages rolling updates)
- **MachineSet** ↔ ReplicaSet (maintains N machines of same template)
- **Machine** ↔ Pod (one machine = one node)
- **AWSMachine / BareMetalHost** ↔ Container (the actual infrastructure resource)

If you know how `kubectl rollout restart deployment/web` triggers a ReplicaSet cascade, you already understand how `spec.version` bump on KubeadmControlPlane triggers immutable machine replacement in CAPI.

### Mnemonic 3: The Five Kubernetes NFR Numbers to Know Cold

- **99.99%** availability target for all 7 systems — the baseline
- **5,000** nodes / **150,000** pods / **10,000** services — scale ceiling for a single cluster
- **25ms** p99 etcd write latency — if this is exceeded, everything degrades
- **100 pods/second** scheduling throughput — the target that drives `percentageOfNodesToScore`
- **300 seconds** HPA scale-down stabilization window — the number behind "why doesn't HPA scale down immediately"

---

### Opening One-Liners (pick one based on the question framing)

**If asked to "design Kubernetes" broadly:**
"Kubernetes is fundamentally a desired-state reconciliation engine — you write down what you want, and a distributed set of control loops continuously makes reality match that spec. Let me start by clarifying whether the key challenge here is the data plane — how containers actually run — the control plane, or the fleet management layer, because each of those requires a different design focus."

**If asked to design a platform for multiple teams:**
"A multi-tenant Kubernetes platform is really a security and fairness problem disguised as an infrastructure problem — the question is how to give 100 teams independence without letting any one of them disrupt the others, and the answer is a 5-layer defense stack: RBAC, NetworkPolicy, ResourceQuota, Pod Security Standards, and optional dedicated node pools for sensitive tenants."

**If asked about autoscaling:**
"Kubernetes autoscaling is a two-loop control system — HPA adjusts pod replicas based on metrics, and Cluster Autoscaler adjusts node count based on unschedulable pods — with a key design tension: the system must react quickly enough to absorb traffic spikes but not oscillate. The stabilization window and VPA's P90 histogram are the two mechanisms that solve the oscillation problem."

---

## STEP 8 — CRITIQUE

### What is Well-Covered in These Source Files

The source files are genuinely comprehensive for a staff-level interview. Specifically strong:

- **etcd internals** are covered to source-code depth: WAL fsync semantics, compaction/defrag distinction, `ErrCompacted` handling, linearizable vs. serializable reads, even-numbered cluster anti-pattern
- **Scheduler pipeline** is precise: Filter/Score/Bind phases, `percentageOfNodesToScore`, preemption algorithm including victim selection, gang scheduling options (Volcano, coscheduling plugin)
- **CAPI specifics** are production-grade: bootstrap chicken-and-egg problem and `clusterctl move` pivot solution, `maxUnhealthy` circuit breaker behavior, bare-metal BareMetalHost state machine
- **HPA algorithm** is exact: the formula, tolerance band, multiple-metrics maximum rule, and the stabilization window maximum-over-window logic
- **Operator Framework** covers the nuances: `observedGeneration` on status conditions, finalizer-controlled deletion, idempotent build-compare-apply pattern, `Result{RequeueAfter}` for periodic re-reconciliation
- **Service Mesh SPIFFE** is detailed: full cert lifecycle with 80%-TTL rotation, ADS ordering guarantee across LDS/RDS/CDS/EDS, xDS hot-reload without connection drops

### What is Missing, Shallow, or Could Mislead

**Karpenter is mentioned but not deeply covered.** Karpenter is increasingly the preferred node autoscaler for AWS/Azure over Cluster Autoscaler. The key differences — Karpenter provisions right-sized nodes on-demand without pre-defined node groups, integrates with spot instance interruption handling, and achieves faster provisioning — deserve deeper treatment. In a 2025+ interview, expecting knowledge of Karpenter's `NodePool` and `EC2NodeClass` CRDs is reasonable.

**VPA in-place update is missing.** The source files describe VPA as requiring pod eviction to apply recommendations (Recreate mode), which was true historically but the `InPlaceOrRecreate` policy (Kubernetes 1.27+, alpha → beta) allows updating resource requests without evicting the pod. This is significant for stateful workloads and high-priority services where pod restart is expensive.

**Ambient mesh coverage is thin.** Istio's ambient mesh (ztunnel + waypoint proxy architecture) is mentioned as "the future direction" but not explained mechanically. In 2025, ambient mesh is in production at many organizations, and interviewers at companies invested in Istio will ask about ztunnel's role, the HBONE tunneling protocol, and when to use waypoint proxies versus ztunnel-only mode.

**Cluster API pivot operation is not fully explained.** The `clusterctl move` command pivots CAPI objects from the bootstrap kind cluster to the real management cluster. A follow-up interviewer question "what happens to workload clusters during a pivot?" has a non-obvious answer: running workload clusters are unaffected (independent control planes), but any machines being provisioned at the time of pivot will have their reconciliation interrupted and may need manual re-application of the Machine objects.

**etcd backup/restore is mentioned but not operationalized.** The files discuss the need for etcd snapshots but don't walk through the actual recovery procedure (`etcdctl snapshot restore`, adjusting `--initial-cluster-state=existing`, and the manual steps to restore a dead member). This is a common Staff+ interview question: "your etcd cluster is down and you have a snapshot — walk me through recovery."

**Network policy enforcement gap.** The files correctly note that Kubernetes NetworkPolicy requires a CNI that enforces it (not all CNIs do). But they don't mention the critical gap: NetworkPolicy has no egress-to-external-IP blocking by default. If a tenant's pod is compromised, it can reach any external IP unless explicit egress-to-external-IP deny policies are configured, which is not part of standard NetworkPolicy. This is a real security gap in multi-tenant platforms.

### Senior Probes That Could Catch You Off Guard

**"How does etcd leader election work for the API server?"** The API server does NOT use leader election — it is stateless and all replicas are active simultaneously. Only kube-scheduler and kube-controller-manager use leader election. This is a common trap; the LB in front of the API server handles distribution, not a leader election lease.

**"What is the difference between a ResourceQuota and a LimitRange?"** ResourceQuota sets aggregate hard limits across a namespace (total CPU across all pods = 100 cores). LimitRange sets per-container defaults and bounds (no single container can request more than 8 CPUs). They work together: LimitRange ensures pods without explicit requests still count against the ResourceQuota (via default requests it injects).

**"How do you safely drain a node that has pods with PodDisruptionBudgets?"** `kubectl drain` evicts pods one at a time, checking PDB constraints before each eviction. If evicting a pod would violate the PDB (e.g., `minAvailable: 2` but only 2 replicas exist), the drain blocks until the PDB allows eviction. This can cause `kubectl drain` to hang indefinitely if the service's other replicas are also down. In CAPI upgrades, this is why machine replacement can take longer than expected — the MachineSet cannot delete the old machine until the new one is Ready and the drain completes.

**"What happens to the cluster if the API server goes down for 5 minutes, then comes back?"** Running pods continue unaffected for those 5 minutes (kubelet cached the pod spec and continues running containers, running its own probes). When the API server comes back, kubelets reconnect and re-report pod status. The controller-manager re-reconciles anything that drifted (e.g., a node that went NotReady during the outage will have its pods evicted per the normal eviction timeline). Watch streams reconnect with `resourceVersion` from last bookmark. If the API server was down longer than the etcd compaction window (5 minutes), watches may need to re-LIST.

**"Can you run a Kubernetes cluster without kube-proxy?"** Yes, if you use Cilium as your CNI. Cilium's eBPF programs implement all service load balancing in the kernel, replacing iptables/IPVS kube-proxy rules. This is the recommended configuration for high-performance clusters because eBPF has lower per-packet overhead and faster update propagation than iptables (O(n) for iptables vs. O(1) for eBPF maps). The flag is `--kube-proxy-replacement=true` in Cilium's configuration.

### Common Traps to Avoid

**Trap 1: Saying "Kubernetes uses Docker"** — Docker was removed as the container runtime in Kubernetes 1.24. The runtime is containerd (or cri-o). Docker Desktop still uses containerd internally; Kubernetes bypasses Docker entirely.

**Trap 2: Proposing etcd as the application database** — "We'll store our application's configuration in etcd since it's already there." etcd is a shared infrastructure component; putting application data there increases storage size toward the 8GB limit, degrades API server performance (more keys to list/watch), and mixes concern layers.

**Trap 3: Recommending HPA + VPA in Auto mode simultaneously** — This is explicitly documented as conflicting. Always pair HPA with VPA in recommendation-only (`Off` or `Initial`) mode.

**Trap 4: Conflating cluster-admin RBAC with security** — Giving a team a ClusterRole that appears scoped (e.g., read-only ClusterRole bound only to their namespace via RoleBinding) does not prevent them from reading cluster-scoped resources like Nodes and PersistentVolumes. Genuine cross-team isolation requires namespace-scoped Roles and explicit deny patterns or vCluster.

**Trap 5: Forgetting DNS in pod startup** — A pod that starts up before CoreDNS is ready (during cluster bootstrap or CoreDNS pod restart) will fail DNS lookups. The solution is `readinessProbe` on the application (so it only joins service endpoints when its DNS dependencies are resolvable) and CoreDNS `PodDisruptionBudget` to ensure at least one CoreDNS pod is always running.

---

*End of Infra Pattern 5 Interview Guide — Kubernetes & Containers*
