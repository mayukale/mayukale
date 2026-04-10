# System Design: Kubernetes Control Plane

> **Relevance to role:** The control plane is the brain of every Kubernetes cluster. A cloud infrastructure platform engineer must understand API server request processing, etcd consensus, scheduler internals, and controller reconciliation loops at source-code depth to debug production incidents, tune performance, and design HA deployments across bare-metal and cloud environments.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Provide a declarative API for managing workloads (Pods, Deployments, Services, etc.) |
| FR-2 | Persist all cluster state reliably with strong consistency guarantees |
| FR-3 | Schedule pods onto nodes based on resource availability, constraints, and policies |
| FR-4 | Continuously reconcile desired state with actual state via controllers |
| FR-5 | Support admission control (mutating + validating webhooks) on every API request |
| FR-6 | Authenticate and authorize every request (RBAC, ABAC, Webhook) |
| FR-7 | Expose a watch API for efficient change notification to all components |
| FR-8 | Manage cloud-provider-specific resources (LBs, routes, node lifecycle) |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | API server p99 latency (read) | < 1 s for LIST of 5k objects, < 100 ms for GET |
| NFR-2 | API server p99 latency (write) | < 200 ms for a single object mutation |
| NFR-3 | Scheduling throughput | ≥ 100 pods/second |
| NFR-4 | etcd write latency p99 | < 25 ms (SSD-backed) |
| NFR-5 | Control plane availability | 99.99 % (four-nines) |
| NFR-6 | etcd data durability | Zero data loss on any single-node failure |
| NFR-7 | Cluster scale | Up to 5,000 nodes, 150,000 pods, 10,000 services |
| NFR-8 | Recovery time (single component crash) | < 30 seconds |

### Constraints & Assumptions
- etcd cluster of 3 or 5 nodes (odd quorum requirement from Raft).
- Kubernetes version 1.28+ (latest stable scheduler framework).
- SSD-backed etcd storage for consistent write latency.
- Control plane nodes are dedicated (no user workloads scheduled on them).
- Network latency between control plane components < 2 ms (co-located or same AZ).

### Out of Scope
- Kubelet internals and container runtime (covered in container_orchestration_system.md).
- Service mesh control plane (covered in service_mesh_design.md).
- Multi-cluster management (covered in kubernetes_cluster_api_platform.md).
- Application-level autoscaling (covered in pod_autoscaler_system.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|------------|--------|
| Nodes | Large production cluster | 5,000 |
| Pods | 30 pods/node average | 150,000 |
| API objects (total) | Pods + Services + ConfigMaps + Secrets + ... | ~500,000 |
| Kubelet heartbeats | 5,000 nodes x 1 heartbeat/10s | 500 req/s |
| Controller reconciliations | ~20 controllers x ~50 reconcile/s average | 1,000 req/s |
| User kubectl/CI operations | 200 engineers x 5 req/min average | ~17 req/s |
| Watch connections | 5,000 kubelets + 500 controllers + 200 users | ~5,700 |
| **Total API server QPS** | 500 + 1,000 + 17 + watch updates | **~2,000-3,000 QPS** |

### Latency Requirements

| Operation | Target | Justification |
|-----------|--------|--------------|
| GET single object | < 10 ms | Serializable read from API server cache |
| LIST 5,000 objects | < 1 s | Paged with continue token |
| CREATE / UPDATE | < 200 ms | Includes etcd round-trip + admission webhooks |
| DELETE with finalizers | < 500 ms | Two-phase: set deletionTimestamp, then finalizer removal |
| Pod scheduling (e2e) | < 5 s | From pod creation to binding |
| Watch event delivery | < 100 ms | From etcd commit to client notification |

### Storage Estimates

| Component | Calculation | Result |
|-----------|------------|--------|
| etcd total data | 500k objects x 5 KB avg | ~2.5 GB |
| etcd WAL | 3x data for write-ahead log + snapshots | ~7.5 GB |
| etcd recommended disk | Headroom for compaction + snapshots | 50 GB SSD |
| API server cache (RAM) | Full object set in memory | ~4-8 GB |
| etcd memory | Working set + page cache | ~8 GB |

### Bandwidth Estimates

| Flow | Calculation | Result |
|------|------------|--------|
| Kubelet → API server | 5,000 x 1 KB heartbeat / 10s | ~500 KB/s |
| Watch stream (all clients) | 5,700 watchers x 0.5 KB/event x 50 events/s | ~142 MB/s peak |
| etcd replication | 3,000 writes/s x 5 KB x 2 replicas | ~30 MB/s |
| API server ↔ webhook | 500 req/s x 2 KB payload | ~1 MB/s |

---

## 3. High Level Architecture

```
                              +-----------+
                              |  kubectl  |
                              |  CI/CD    |
                              +-----+-----+
                                    |
                                    | HTTPS (TLS 1.3)
                                    v
                         +--------------------+
                         |   Load Balancer    |
                         | (HAProxy / cloud)  |
                         +----+----------+----+
                              |          |
                    +---------+--+  +----+---------+
                    | API Server |  | API Server   |
                    |  (active)  |  |  (active)    |
                    +--+---------+  +--------+-----+
                       |     |               |
          +------------+     |    +----------+
          |                  |    |
          v                  v    v
   +------+------+    +-----+----+-----+
   | Admission   |    |     etcd       |
   | Webhooks    |    | (3-node Raft)  |
   | (mutating + |    |                |
   |  validating)|    | Leader ←→ F F  |
   +-------------+    +-------+--------+
                              |
                    +---------+---------+
                    |                   |
          +---------+------+  +--------+---------+
          | kube-scheduler |  | kube-controller  |
          | (leader-elect) |  |    manager       |
          +-------+--------+  | (leader-elect)   |
                  |           +--------+---------+
                  |                    |
                  v                    v
          +-------+--------------------+-------+
          |        Kubelet on each Node        |
          |  (watches API server for pod spec) |
          +------------------------------------+
                              |
                              v
                   +----------+---------+
                   | cloud-controller   |
                   | manager            |
                   | (leader-elect)     |
                   +--------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **API Server** | Stateless REST/gRPC gateway; authenticates, authorizes, runs admission, persists to etcd, serves watches |
| **etcd** | Distributed KV store; single source of truth; Raft consensus for leader election and replication |
| **kube-scheduler** | Selects a node for every unscheduled pod via filter → score → bind pipeline |
| **kube-controller-manager** | Runs ~30 reconciliation loops (Deployment, ReplicaSet, Node, ServiceAccount, etc.) |
| **cloud-controller-manager** | Manages cloud-specific resources: node lifecycle (auto-delete terminated VMs), service LBs, routes |
| **Admission Webhooks** | External processes that mutate or validate API objects before persistence |
| **Load Balancer** | Distributes client requests across multiple API server replicas |

### Data Flows

1. **Write path:** Client → LB → API server → authentication → authorization → mutating admission → validation → validating admission → etcd write → response to client → watch notification to subscribers.
2. **Read path (GET):** Client → LB → API server → serve from cache (if resourceVersion allows) or read from etcd → response.
3. **Read path (LIST):** Client → LB → API server → serve from informer cache (serializable) or etcd (linearizable if explicitly requested) → response with resourceVersion.
4. **Watch path:** Client opens long-lived HTTP/2 stream → API server sends change events from its internal watch cache (populated from etcd watch).
5. **Scheduling path:** Scheduler watches for pods with `spec.nodeName=""` → runs filter/score → PATCHes pod with binding → kubelet picks it up via watch.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# Every Kubernetes object follows this structure
apiVersion: v1
kind: Pod
metadata:
  name: my-pod
  namespace: default
  uid: "a1b2c3d4-..."            # Unique ID (etcd key prefix)
  resourceVersion: "12345"        # etcd modRevision — used for optimistic concurrency
  creationTimestamp: "2026-04-09T10:00:00Z"
  labels: { app: web }
  annotations: {}
  ownerReferences:                # Garbage collection chain
    - apiVersion: apps/v1
      kind: ReplicaSet
      name: web-abc123
      uid: "x9y8z7..."
  finalizers: ["kubernetes.io/pvc-protection"]
spec:
  # Desired state — written by user / controller
  containers:
    - name: web
      image: nginx:1.27
      resources:
        requests: { cpu: "250m", memory: "256Mi" }
        limits:   { cpu: "500m", memory: "512Mi" }
  nodeName: ""                    # Empty until scheduler binds
status:
  # Actual state — written by kubelet / controllers
  phase: Pending
  conditions:
    - type: PodScheduled
      status: "False"
```

**etcd Key Layout:**
```
/registry/pods/default/my-pod           → Pod proto-encoded bytes
/registry/deployments/default/my-deploy → Deployment proto-encoded bytes
/registry/services/specs/default/my-svc → Service proto-encoded bytes
/registry/secrets/default/my-secret     → Encrypted Secret bytes
/registry/events/default/event-abc123   → Event proto bytes (TTL'd)
```

### Database Selection

| Aspect | etcd | Why Not Alternatives |
|--------|------|---------------------|
| Consistency | Linearizable (Raft) | MySQL: could work but etcd is purpose-built for k8s watch + lease semantics |
| Watch primitive | Native watch with revision-based streaming | Postgres: LISTEN/NOTIFY lacks revision-based resumption |
| Scale | ~500K keys, ~8 GB recommended limit | Sufficient for cluster metadata; not for application data |
| Latency | < 10 ms reads, < 25 ms writes on SSD | Collocated with API server for minimal RTT |
| Alternatives explored | **k3s uses SQLite/Postgres via kine**; works for < 500 nodes but loses native watch efficiency |

**etcd internals critical to understand:**

1. **Raft consensus:** Leader accepts all writes → appends to WAL → replicates to followers → commits after quorum (n/2 + 1) ack. Followers serve linearizable reads only by confirming with leader (ReadIndex); serializable reads served locally.
2. **Watch API:** Clients subscribe to key prefix with a revision number. etcd streams all changes from that revision. If the revision is compacted, the watch fails with `ErrCompacted` and client must re-list.
3. **Compaction:** Old revisions are pruned periodically (default: 5 min). API server sets `--etcd-compaction-interval`. Without compaction, etcd database grows unbounded.
4. **Defragmentation:** After compaction, freed space is not returned to the filesystem until defrag. Must be done one node at a time to avoid quorum loss.
5. **Lease mechanism:** Used for leader election (scheduler, controller-manager). A lease has a TTL; the holder must renew before expiry or lose leadership.

### Indexing Strategy

| Index | Purpose | Implementation |
|-------|---------|---------------|
| etcd key prefix `/registry/<resource>/<namespace>/` | Fast lookup by resource type + namespace | B+tree in etcd's bbolt |
| resourceVersion (etcd modRevision) | Optimistic concurrency control; watch resumption | Monotonically increasing per etcd transaction |
| Label selector indexes | API server in-memory label index for LIST filtering | Built by informer cache (client-go) |
| Field selectors | `spec.nodeName`, `status.phase` | API server-side index for specific fields |

---

## 5. API Design

### REST/gRPC/kubectl Endpoints

**Core API patterns (all objects follow these):**

```
# CRUD
GET    /api/v1/namespaces/{ns}/pods/{name}            # Get single pod
GET    /api/v1/namespaces/{ns}/pods                    # List pods in namespace
GET    /api/v1/pods                                     # List all pods (cluster-wide)
POST   /api/v1/namespaces/{ns}/pods                    # Create pod
PUT    /api/v1/namespaces/{ns}/pods/{name}             # Full update
PATCH  /api/v1/namespaces/{ns}/pods/{name}             # Partial update (JSON merge/strategic)
DELETE /api/v1/namespaces/{ns}/pods/{name}             # Delete

# Watch (long-lived HTTP/2 stream)
GET    /api/v1/namespaces/{ns}/pods?watch=true&resourceVersion=12345

# Subresources
GET    /api/v1/namespaces/{ns}/pods/{name}/status      # Status subresource
PUT    /api/v1/namespaces/{ns}/pods/{name}/status      # Update status only
POST   /api/v1/namespaces/{ns}/pods/{name}/binding     # Scheduler uses this
GET    /api/v1/namespaces/{ns}/pods/{name}/log          # Stream container logs

# Pagination
GET    /api/v1/pods?limit=500&continue=<token>
```

**API Server Request Processing Pipeline:**
```
Request → TLS termination
        → Authentication (x509 cert | Bearer token | OIDC | ServiceAccount)
        → Authorization  (RBAC → ABAC → Webhook — first allow wins, deny if none allow)
        → Mutating Admission Webhooks (ordered, can modify object)
        → Object Schema Validation
        → Validating Admission Webhooks (ordered, cannot modify)
        → etcd write (with optimistic concurrency via resourceVersion)
        → Response to client
        → Watch event broadcast
```

### CLI Design (kubectl internals)

```bash
# kubectl uses client-go under the hood
kubectl get pods -n production -o wide              # LIST with table output
kubectl describe pod my-pod -n production           # GET + events + conditions
kubectl apply -f deployment.yaml                    # Strategic merge patch (3-way diff)
kubectl delete pod my-pod --grace-period=30         # Sets deletionTimestamp, waits for grace period
kubectl logs my-pod -c sidecar --follow             # Streams logs via kubelet proxy
kubectl exec -it my-pod -- /bin/sh                  # WebSocket upgrade to kubelet
kubectl port-forward svc/my-svc 8080:80             # Local tunnel via API server

# Debugging control plane
kubectl get --raw /readyz                           # API server health
kubectl get --raw /metrics                          # Prometheus metrics
kubectl get componentstatuses                       # Deprecated but still used
kubectl get lease -n kube-system                    # Leader election leases
etcdctl endpoint status --cluster                   # etcd cluster health
etcdctl endpoint health --cluster                   # etcd endpoint liveness
etcdctl get /registry/pods/default/ --prefix --keys-only  # Inspect etcd keys
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: etcd — The Source of Truth

**Why it's hard:**
etcd must provide linearizable consistency, efficient watch semantics, and bounded storage for hundreds of thousands of objects while maintaining sub-25ms write latency. A single etcd failure or performance degradation cascades to the entire cluster becoming unresponsive.

**Approaches Compared:**

| Approach | Consistency | Watch Support | Operational Complexity | k8s Ecosystem Fit |
|----------|------------|---------------|----------------------|-------------------|
| etcd (Raft) | Linearizable | Native revision-based | Medium (3/5 node quorum) | Native — designed for k8s |
| PostgreSQL (via kine) | Serializable (default) | Polled via kine shim | Low (managed DB available) | Used by k3s; shim adds latency |
| CockroachDB | Serializable | No native watch | High | Not compatible without major changes |
| ZooKeeper | Linearizable | Native watch but single-fire | Medium | Legacy; single-fire watch is problematic |
| DynamoDB (via kine) | Eventually consistent (default) | No native watch | Low (serverless) | High latency; used for edge k3s |

**Selected: etcd (Raft)**

**Justification:** etcd was co-designed with Kubernetes. Its revision-based watch API maps directly to resourceVersion semantics. The bbolt B+tree backend provides consistent read performance. Raft quorum guarantees zero data loss on single-node failure.

**Implementation Detail:**

```
Write Path:
1. API server sends gRPC Put(key, value, lease) to etcd leader
2. Leader appends entry to WAL (fsync to SSD)
3. Leader replicates to followers via AppendEntries RPC
4. Once quorum (2/3 or 3/5) confirms, leader commits
5. Leader responds to API server with new revision
6. Committed entry applied to bbolt B+tree (state machine)
7. Watch subscribers notified with (key, value, modRevision, type=PUT)

Compaction:
- API server runs compaction every 5 minutes (--etcd-compaction-interval)
- etcd marks old revisions as tombstoned
- Defrag needed to reclaim disk space (offline per-node operation)
- Without compaction: etcd grows unbounded → OOM or disk full → cluster down

Snapshot:
- etcd takes automatic snapshots every 10,000 applied entries (--snapshot-count)
- Snapshot = full bbolt database dump
- Used for disaster recovery and new member bootstrap
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Leader crash | 1-2s election timeout; cluster read-only during election | Tuned election timeout (1s); API server retries |
| Quorum loss (2/3 nodes down) | Cluster completely unavailable for writes | Backup/restore from snapshot; run 5-node for tolerance of 2 failures |
| Disk full | etcd enters alarm mode, rejects all writes | Monitor disk usage; set `--quota-backend-bytes` (default 2 GB, max recommended 8 GB) |
| Slow disk (non-SSD) | Write latency spikes → scheduler timeouts → pod scheduling delays | Dedicated SSD; monitor `etcd_disk_wal_fsync_duration_seconds` |
| Split brain (network partition) | Minority partition becomes read-only; majority continues | Raft guarantees safety; clients may see stale reads from minority |
| Compaction not running | Database grows → slow queries → OOM | Alert on `etcd_mvcc_db_total_size_in_bytes` |

**Interviewer Q&As:**

**Q1: Why does Kubernetes use etcd instead of a traditional database like MySQL?**
A: Three key reasons: (1) etcd provides native revision-based watch semantics — every object mutation generates a monotonically increasing revision that clients can resume from, which maps directly to Kubernetes' `resourceVersion` for optimistic concurrency and watch resumption. (2) Linearizable consistency by default ensures all control plane components see the same state. (3) Lease primitives power leader election for scheduler and controller-manager without external dependencies. MySQL could work (and k3s proves it via kine shim), but it requires polling for change detection and adds a translation layer.

**Q2: What happens if etcd runs out of disk space?**
A: etcd enters alarm mode (`NOSPACE`). All write requests are rejected with error code. The cluster becomes effectively read-only. To recover: (1) Free disk space, (2) Clear the alarm with `etcdctl alarm disarm`, (3) Run defragmentation on each member. Prevention: set `--quota-backend-bytes` appropriately, monitor `etcd_server_quota_backend_bytes`, and ensure compaction is running.

**Q3: How does etcd's watch API handle a client that disconnects and reconnects?**
A: The client reconnects with the last observed `resourceVersion` (etcd revision). etcd streams all events from that revision forward. If the revision has been compacted (older than the compaction window), etcd returns `ErrCompacted`. The Kubernetes informer handles this by triggering a full re-list of the resource, establishing a new baseline resourceVersion, and restarting the watch from there.

**Q4: Can you run etcd with 2 or 4 nodes?**
A: Technically yes, but it's inadvisable. Raft requires a strict majority for quorum. With 2 nodes, losing 1 means no quorum (need 2/2). With 4 nodes, you still only tolerate 1 failure (need 3/4) — same as 3 nodes but with more replication overhead. The standard deployments are 3 nodes (tolerate 1 failure) or 5 nodes (tolerate 2 failures).

**Q5: How do you perform a zero-downtime etcd upgrade?**
A: Rolling upgrade one member at a time: (1) Stop the member, (2) Replace the binary, (3) Start with same data directory, (4) Wait for it to rejoin and catch up, (5) Verify cluster health with `etcdctl endpoint health --cluster`, (6) Proceed to next member. The leader should be upgraded last to minimize unnecessary elections. etcd supports online migration between minor versions.

**Q6: What is the difference between linearizable and serializable reads in etcd?**
A: Linearizable reads (default) require the leader to confirm it is still the leader via a round of Raft heartbeats before responding — this guarantees the read reflects all committed writes up to that point. Serializable reads can be served by any member from their local state — faster (no leader round-trip) but potentially stale. Kubernetes API server uses serializable reads for its informer cache (acceptable staleness) and linearizable reads for critical operations like leader election.

---

### Deep Dive 2: kube-scheduler — Pod Placement Engine

**Why it's hard:**
The scheduler must make optimal placement decisions for hundreds of pods per second across thousands of nodes, respecting resource constraints, affinity/anti-affinity rules, topology spread, taints/tolerations, and priority/preemption — all while maintaining consistency with a cluster state that is continuously changing.

**Approaches Compared:**

| Approach | Throughput | Optimality | Complexity | Use Case |
|----------|-----------|-----------|-----------|----------|
| Default kube-scheduler (filter→score→bind) | ~100 pods/s | Good (heuristic scoring) | Medium | General purpose |
| Gang scheduling (Volcano/Coscheduling) | Lower (batch-based) | High for ML jobs | High | ML training, batch workloads |
| Bin-packing (custom scorer) | ~100 pods/s | High resource efficiency | Medium | Cost optimization |
| Spread scheduling | ~100 pods/s | High availability | Medium | Stateless web services |
| Multi-scheduler | Varies | Domain-specific | High | Heterogeneous workloads |

**Selected: Default kube-scheduler with scheduling framework (extensible)**

**Justification:** The scheduling framework (introduced in v1.15, stable in v1.25) provides plugin extension points at every phase, allowing custom logic without forking the scheduler. It supports > 100 pods/s throughput and covers most production use cases.

**Implementation Detail — The Scheduling Cycle:**

```
Phase 1: FILTER (Predicates) — eliminate infeasible nodes
  ┌─────────────────────────────────────────────────────────┐
  │ NodeResourcesFit: Does node have enough CPU/RAM?        │
  │ NodePorts:        Is the requested hostPort available?   │
  │ TaintToleration:  Does pod tolerate node taints?        │
  │ NodeAffinity:     Does pod match node selector/affinity?│
  │ PodTopologySpread: Would scheduling violate maxSkew?    │
  │ VolumeBinding:    Can required PVs be bound on node?    │
  │ InterPodAffinity: Does pod's anti-affinity conflict?    │
  └─────────────────────────────────────────────────────────┘
  Input: 5,000 nodes → Output: ~500 feasible nodes (example)

  Optimization: percentageOfNodesToScore (default 50% for 5k nodes)
  - Once enough feasible nodes are found, stop filtering remaining
  - Configurable via KubeSchedulerConfiguration

Phase 2: SCORE (Priorities) — rank feasible nodes 0-100
  ┌─────────────────────────────────────────────────────────┐
  │ LeastAllocated:             Prefer less-loaded nodes    │
  │ BalancedResourceAllocation: Prefer balanced CPU:RAM     │
  │ InterPodAffinity:           Prefer co-located affinity  │
  │ NodePreferAvoidPods:        Respect controller hints    │
  │ TaintToleration:            Prefer fewer untolerated    │
  │ ImageLocality:              Prefer nodes with image     │
  │ PodTopologySpread:          Prefer better spread        │
  └─────────────────────────────────────────────────────────┘
  Each plugin returns 0-100 per node, weighted and summed.
  Highest score wins. Ties broken randomly.

Phase 3: BIND — assign pod to selected node
  1. Reserve: plugins reserve resources (volume binding, etc.)
  2. Permit:  plugins can approve, deny, or wait (for gang scheduling)
  3. PreBind: attach volumes, other pre-work
  4. Bind:    API server PATCH pod.spec.nodeName = selectedNode
  5. PostBind: cleanup, metrics

Scheduling Queue:
  - ActiveQ:        Pods ready to schedule (sorted by priority)
  - BackoffQ:       Failed-to-schedule pods (exponential backoff)
  - UnschedulableQ: Pods blocked on unmet conditions (re-evaluated on cluster changes)
```

**Preemption:**
```
If no node passes Filter for a high-priority pod:
1. Scheduler identifies lower-priority victim pods on each node
2. Simulates removing victims and re-running Filter
3. Selects node with minimum victims / lowest priority victims
4. Sets pod.status.nominatedNodeName
5. Deletes victim pods (with grace period)
6. Reschedules the high-priority pod in next cycle
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Scheduler crash | Pending pods not scheduled until restart | Leader election enables standby to take over in < 15s |
| Stale cache | Pod scheduled to node that is already full | Optimistic concurrency — bind fails, pod re-queued |
| All nodes infeasible | Pod stuck in Pending forever | Cluster Autoscaler provisions new nodes; alert on long-pending pods |
| Scheduler too slow | Pod scheduling latency > 5s | Tune percentageOfNodesToScore; profile scheduler plugins |
| Preemption cascade | High-priority pods evict many low-priority pods | Use PriorityClass with care; set preemptionPolicy: Never for batch |
| Webhook timeout | Scheduling cycle blocked by slow mutating webhook | Set failurePolicy: Ignore or timeout: 5s on webhooks |

**Interviewer Q&As:**

**Q1: How does the scheduler handle a node that becomes NotReady after a pod is scheduled but before the kubelet starts the pod?**
A: The node controller (in controller-manager) detects the NotReady condition via missed heartbeats. After `--pod-eviction-timeout` (default 5 min), it adds a NoExecute taint. The taint-based eviction controller then evicts all pods without a matching toleration. The evicted pods are recreated by their parent controller (Deployment/ReplicaSet) and re-enter the scheduling queue.

**Q2: What is percentageOfNodesToScore and when would you tune it?**
A: It controls how many feasible nodes the scheduler evaluates in the scoring phase. Default is `max(5%, 100 nodes)` for clusters > 100 nodes. In a 5,000-node cluster, scoring all nodes is O(n) per pod — expensive at 100 pods/s. Reducing to 5% (250 nodes) is usually sufficient because scoring is a heuristic anyway. You increase it when workloads have complex affinity rules that need more candidate nodes for optimal placement.

**Q3: How do you implement gang scheduling (all-or-nothing) in Kubernetes?**
A: Native kube-scheduler doesn't support gang scheduling. Solutions: (1) **Coscheduling plugin** (scheduling framework plugin) — uses the Permit extension point to hold pods until all members of a PodGroup are schedulable, then releases them together. (2) **Volcano scheduler** — a separate scheduler designed for batch/ML workloads with native gang, queue, and fair-share support. Both require CRDs (PodGroup/Queue) and a custom scheduler deployment.

**Q4: Explain the difference between node affinity and node selector.**
A: `nodeSelector` is a simple key-value label match (hard constraint, AND logic). `nodeAffinity` is the expressive successor: it supports `requiredDuringSchedulingIgnoredDuringExecution` (hard) and `preferredDuringSchedulingIgnoredDuringExecution` (soft), with operators like In, NotIn, Exists, DoesNotExist, Gt, Lt. Node selectors are being deprecated in favor of node affinity. Both are evaluated in the Filter phase.

**Q5: What happens when two pods have the same priority and compete for the last available slot?**
A: The scheduler processes pods from the ActiveQ in priority order. Same-priority pods are ordered by timestamp (FIFO). The first pod to reach the scheduling cycle gets the slot. The second pod enters BackoffQ or UnschedulableQ. If Cluster Autoscaler is enabled, it detects the unschedulable pod and provisions a new node.

**Q6: How does PodTopologySpread work internally?**
A: The `topologySpreadConstraints` field specifies: (1) `topologyKey` (e.g., `topology.kubernetes.io/zone`), (2) `maxSkew` (maximum allowed difference in pod count between topology domains), (3) `whenUnsatisfiable` (DoNotSchedule or ScheduleAnyway). The Filter plugin calculates current pod distribution across domains and rejects nodes where scheduling would violate maxSkew. The Score plugin prefers nodes in domains with fewer matching pods. This is critical for zone-aware HA deployments.

---

### Deep Dive 3: API Server — The Gateway

**Why it's hard:**
The API server is the single entry point for all cluster operations. It must handle thousands of concurrent connections (including long-lived watches), process admission webhooks with timeout guarantees, maintain an in-memory cache for efficient reads, and never lose a write — all while supporting rolling upgrades.

**Approaches Compared:**

| Aspect | Kubernetes API Server | Custom REST API | GraphQL |
|--------|----------------------|----------------|---------|
| Declarative semantics | Native (spec/status split) | Must build | Possible but unnatural |
| Watch support | Native (HTTP/2 streaming) | WebSocket/SSE (build yourself) | Subscriptions (complex) |
| Schema validation | OpenAPI + CRD validation | Must build | Intrinsic |
| Admission control | Plugin pipeline | Middleware (build yourself) | Resolvers |
| Scalability | Horizontal (stateless + etcd) | Varies | Varies |

**Selected: Kubernetes API Server (kube-apiserver)**

**Implementation Detail — Authentication Chain:**

```
Request arrives → evaluated against chain (first success wins):

1. x509 Client Certificate
   - CN = username, O = group
   - Used by: kubelets, kube-proxy, controller-manager
   - Configured: --client-ca-file

2. Bearer Token (Static File)
   - Deprecated but still supported
   - --token-auth-file

3. Bootstrap Token
   - Used for kubeadm node join (limited TTL)
   - Stored as Secrets in kube-system

4. Service Account Token (JWT)
   - Mounted into every pod at /var/run/secrets/kubernetes.io/serviceaccount/token
   - Bound service account tokens (v1.22+): audience-bound, time-bound
   - Validated by API server's token authenticator

5. OIDC Token
   - Integration with corporate IdP (Okta, Azure AD, etc.)
   - --oidc-issuer-url, --oidc-client-id
   - Groups claim → k8s groups

6. Webhook Token
   - External authentication service
   - --authentication-token-webhook-config-file
```

**Authorization Chain (evaluated in order, first decision wins):**

```
1. RBAC (Role-Based Access Control) — most common
   - Role/ClusterRole: defines permissions (verbs on resources)
   - RoleBinding/ClusterRoleBinding: binds role to subject (user, group, SA)
   - Example: "user alice can GET/LIST pods in namespace production"

2. ABAC (Attribute-Based Access Control)
   - Static policy file (--authorization-policy-file)
   - Rarely used in production; difficult to manage

3. Webhook
   - External authorization service (e.g., OPA)
   - SubjectAccessReview sent to webhook
   - Used for complex policies (e.g., time-based access)

4. Node
   - Restricts kubelets to only access their own node's objects
   - Enabled by default (--authorization-mode=Node,RBAC)

Decision: Allow → proceed | Deny → 403 | NoOpinion → next authorizer
If all say NoOpinion → deny (fail-closed)
```

**Admission Webhook Processing:**

```
Mutating Admission (ordered by webhook configuration):
1. PodPreset injection (deprecated in favor of mutating webhooks)
2. Istio sidecar injection
3. Resource defaults (e.g., set default resource requests)
4. Label injection

→ Object may be modified by each webhook in sequence

Validating Admission (ordered, called in parallel within same order):
1. OPA/Gatekeeper policy validation
2. Custom business rules (e.g., "images must be from approved registry")
3. Resource quota validation

→ Any rejection = entire request rejected with reason

Timeout: default 10s per webhook (configurable, max 30s)
Failure Policy: Fail (reject if webhook unreachable) or Ignore (allow if unreachable)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| API server OOM | All cluster operations fail | Set resource limits; tune `--max-requests-inflight` and `--max-mutating-requests-inflight` |
| Webhook timeout | Request latency spikes; potential cascading failure | Set `timeoutSeconds: 5`; use `failurePolicy: Ignore` for non-critical webhooks |
| etcd connectivity loss | All writes fail; reads served from cache (stale) | Multiple etcd endpoints; circuit breaker in client |
| Certificate expiry | All TLS connections fail | Automate cert rotation; monitor expiry with Prometheus |
| Watch bookmark failure | Clients re-list unnecessarily, causing API server load spike | Enable WatchBookmarks feature gate (default since v1.24) |
| Admission webhook crash | If failurePolicy=Fail, all writes blocked | Run webhook as HA deployment; use Ignore for non-critical |

**Interviewer Q&As:**

**Q1: How does the API server handle 5,000+ watch connections efficiently?**
A: The API server maintains a single watch cache per resource type, populated from a single etcd watch. When an object changes, the event is fanned out to all matching watchers from memory — not from etcd. This means 5,000 kubelet watches for Pod events are served from one in-memory cache. The cache uses a ring buffer of recent events, so new watchers can catch up without hitting etcd. HTTP/2 multiplexing allows many watches over a single TCP connection.

**Q2: What are watch bookmarks and why do they matter?**
A: Without bookmarks, if a client's watch has no events for a long period, its `resourceVersion` becomes stale. When it reconnects, it may be behind the compaction window and forced to re-list (expensive). Watch bookmarks are synthetic events (type=BOOKMARK) sent periodically by the API server, carrying an updated resourceVersion without any object change. This keeps the client's position current and prevents unnecessary re-lists. Critical for large clusters.

**Q3: How does the API server implement optimistic concurrency control?**
A: Every object has a `resourceVersion` field mapped to etcd's `modRevision`. When a client does an UPDATE, the API server sends the write to etcd with a transaction: `if(modRevision == client's resourceVersion) then put(new value)`. If another write modified the object in between, the revision won't match and etcd returns a conflict. The API server returns HTTP 409 Conflict, and the client must re-read and retry. This eliminates the need for distributed locks.

**Q4: How do you debug a slow API server?**
A: (1) Check `apiserver_request_duration_seconds` histogram by verb and resource — identify which operations are slow. (2) Check `apiserver_current_inflight_requests` — if at max, requests are being queued/rejected. (3) Check etcd latency (`etcd_request_duration_seconds`) — slow etcd propagates to API server. (4) Check admission webhook latency (`apiserver_admission_webhook_admission_duration_seconds`). (5) Profile with `kubectl get --raw /debug/pprof/profile` (requires enabling profiling). (6) Check for expensive LIST calls without pagination or label selectors.

**Q5: What happens during a rolling API server upgrade?**
A: Multiple API servers run behind a load balancer. During rolling upgrade: (1) Drain one instance (stop accepting new connections), (2) Wait for existing requests to complete (graceful termination, `--shutdown-delay-duration`), (3) Replace with new version, (4) New instance joins the LB pool, (5) Repeat for next instance. Kubernetes API has strict version skew policy: API servers can differ by at most 1 minor version during upgrade. Storage encoding is versioned, so old and new API servers can read the same etcd data.

**Q6: How does API Priority and Fairness (APF) work?**
A: APF (stable since v1.29) replaces `--max-requests-inflight` with a more granular system. Requests are classified into FlowSchemas (matching rules based on user, group, verb, resource). Each FlowSchema maps to a PriorityLevelConfiguration with concurrency shares. Critical system requests (leader election, node heartbeats) get guaranteed bandwidth even when the server is overloaded. This prevents a rogue controller from starving kubelets.

---

### Deep Dive 4: Controller Manager — Reconciliation Engine

**Why it's hard:**
The controller manager runs ~30 independent reconciliation loops that must converge desired state to actual state without conflicting with each other, handle partial failures gracefully, and avoid thundering herd problems during recovery — all while processing thousands of events per second.

**Implementation Detail — Informer/Lister Pattern:**

```
┌──────────────────────────────────────────────────────┐
│                  Controller Manager                   │
│                                                      │
│  ┌─────────────┐     ┌──────────────────────────┐   │
│  │  Reflector   │────→│    Informer Store        │   │
│  │  (LIST+WATCH)│     │    (in-memory cache)     │   │
│  └─────────────┘     └──────────┬───────────────┘   │
│                                  │                    │
│                     ┌───────────┴────────────┐       │
│                     │   Event Handlers       │       │
│                     │ OnAdd / OnUpdate / OnDel│       │
│                     └───────────┬────────────┘       │
│                                 │                     │
│                     ┌──────────┴──────────┐          │
│                     │    Work Queue        │          │
│                     │  (rate-limited,      │          │
│                     │   deduplicating)     │          │
│                     └──────────┬──────────┘          │
│                                │                     │
│                     ┌─────────┴──────────┐           │
│                     │   Reconcile Loop    │           │
│                     │   (N workers)       │           │
│                     └────────────────────┘           │
└──────────────────────────────────────────────────────┘

Flow:
1. Reflector does initial LIST to populate cache, then WATCH for changes
2. Events trigger handlers that enqueue object keys (namespace/name)
3. Work queue deduplicates (same key only processed once even if multiple events)
4. Rate limiter prevents thundering herd (exponential backoff: 5ms → 10ms → ... → 1000s)
5. Worker goroutines dequeue and call Reconcile(key)
6. Reconcile reads desired state from cache, compares to actual, takes action
7. On error → requeue with backoff; on success → done
```

**Key Controllers:**

| Controller | Watches | Reconciles |
|-----------|---------|-----------|
| Deployment | Deployments, ReplicaSets | Creates/updates ReplicaSets for rolling updates |
| ReplicaSet | ReplicaSets, Pods | Ensures N replicas of a pod template are running |
| StatefulSet | StatefulSets, Pods, PVCs | Ordered creation/deletion with stable identity |
| DaemonSet | DaemonSets, Nodes, Pods | One pod per node (respects taints/tolerations) |
| Job | Jobs, Pods | Run-to-completion with parallelism and retry |
| CronJob | CronJobs | Creates Jobs on a schedule |
| Node lifecycle | Nodes | Adds taints for NotReady nodes, triggers eviction |
| Service | Services, Endpoints, Pods | Maintains Endpoints for service discovery |
| Namespace | Namespaces | Garbage collects resources in terminating namespaces |
| ServiceAccount | Namespaces | Creates default SA in every new namespace |
| GarbageCollector | All objects | Deletes objects whose ownerReference target is gone |

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Controller crash | Reconciliation stops for all controllers | Leader election; standby takes over in < 15s |
| Work queue backup | Events processed with delay; pods may be stale | Monitor `workqueue_depth`; increase workers |
| Infinite reconcile loop | CPU spike; API server overwhelmed | Rate limiter with exponential backoff; max retries |
| Informer cache stale | Controller acts on outdated state | Re-list interval (default 0 = rely on watch); optimistic concurrency prevents stale writes |
| Conflicting controllers | Two controllers modify same object | Use ownerReferences and field managers (server-side apply) |

**Interviewer Q&As:**

**Q1: Why does the controller use a work queue instead of reconciling directly in the event handler?**
A: Three reasons: (1) **Deduplication** — if an object changes 10 times in 1 second, we only reconcile once with the final state. (2) **Rate limiting** — prevents a controller from overwhelming the API server after a mass event (e.g., node failure evicts 100 pods). (3) **Decoupling** — event handlers must be fast (they block the informer); reconciliation can be slow (API calls, retries).

**Q2: How does the Deployment controller implement a rolling update?**
A: (1) Creates a new ReplicaSet with the updated pod template. (2) Scales up new RS by `maxSurge` (default 25%). (3) Scales down old RS by `maxUnavailable` (default 25%). (4) Repeats until new RS is at desired count and old RS is at 0. (5) Each step is one reconciliation cycle — if the controller crashes, it resumes from current state. The `.status.conditions` track progress (Progressing, Available, ReplicaFailure).

**Q3: What is the difference between level-triggered and edge-triggered reconciliation?**
A: Kubernetes controllers are level-triggered — they compare desired state vs. current state and take action regardless of what event triggered the reconciliation. Edge-triggered systems react to specific events ("pod was deleted"). Level-triggered is more resilient: if an event is missed, the next reconciliation still detects and corrects the drift. This is why controllers should be idempotent — running the same reconciliation twice should produce the same result.

**Q4: How does garbage collection work in Kubernetes?**
A: Objects have `ownerReferences` pointing to their parent. When the parent is deleted, the GC controller detects orphaned children. Two modes: (1) **Background** (default) — parent deleted immediately; GC controller asynchronously deletes children. (2) **Foreground** — parent's `deletionTimestamp` is set but it's not removed until all children with `blockOwnerDeletion=true` are gone. (3) **Orphan** — children are detached (ownerReference removed) and left running. Example: Deployment → ReplicaSet → Pod; deleting Deployment cascades to RS → Pods.

**Q5: How do you prevent a thundering herd when the controller manager restarts?**
A: On startup, informers perform an initial LIST of all objects and the event handlers enqueue everything. Without mitigation, this would create a spike of thousands of reconciliations. The work queue's rate limiter caps the initial burst (e.g., 10 items/s, then exponential backoff for retries). Additionally, the initial reconciliation for most objects is a no-op (desired == actual), which completes quickly.

**Q6: What is server-side apply and why was it introduced?**
A: Server-side apply (SSA, stable v1.22) tracks which fields of an object are managed by which controller (field managers). When two controllers try to modify the same field, the API server detects a conflict and rejects the second write unless `force: true`. This prevents the "last writer wins" problem with client-side `kubectl apply`, where two controllers could fight over the same annotation or label, causing an infinite update loop.

---

## 7. Scheduling & Resource Management

### Kubernetes Scheduler Internals

**Scheduling Framework Extension Points:**

```
           ┌─────────────┐
           │  PreEnqueue  │  ← Gate pods before they enter scheduling queue
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │   PreFilter  │  ← Compute pod-level info used by Filter (e.g., resource sum)
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │    Filter    │  ← Eliminate infeasible nodes (predicates)
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │  PostFilter  │  ← If no node passes Filter, attempt preemption
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │   PreScore   │  ← Compute info used by Score plugins
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │    Score     │  ← Rank feasible nodes 0-100
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │  NormalizeScore│ ← Normalize scores to 0-100 range
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │   Reserve    │  ← Reserve resources (volumes, etc.)
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │    Permit    │  ← Allow, deny, or wait (gang scheduling)
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │   PreBind    │  ← Pre-binding operations (attach volumes)
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │     Bind     │  ← Update pod.spec.nodeName in etcd
           └──────┬──────┘
                  │
           ┌──────▼──────┐
           │   PostBind   │  ← Cleanup, metrics, notifications
           └──────┘──────┘
```

**Resource Model:**

| Concept | Description |
|---------|------------|
| `requests` | Guaranteed minimum resources; used by scheduler for placement and by kubelet for cgroup reservation |
| `limits` | Hard ceiling; enforced by kubelet via cgroups (CPU = throttled, memory = OOM killed) |
| Allocatable | Node capacity minus system reserved (`--system-reserved`, `--kube-reserved`) |
| QoS Guaranteed | requests == limits for all containers → highest OOM priority |
| QoS Burstable | requests < limits for at least one container → medium OOM priority |
| QoS BestEffort | No requests or limits set → lowest OOM priority (first to be evicted) |

**Scheduler Performance Tuning:**

| Parameter | Default | Tuning |
|-----------|---------|--------|
| `percentageOfNodesToScore` | 50% (for 100+ nodes) | Lower for faster scheduling at cost of optimality |
| `parallelism` | 16 workers | Increase for higher throughput |
| `pod-max-in-unschedulable-pods-duration` | 5 min | Lower to re-evaluate sooner |
| Scheduling plugins | Default profile | Disable unused plugins (e.g., VolumeBinding if no PVs) |

**Priority and Preemption:**

```yaml
apiVersion: scheduling.k8s.io/v1
kind: PriorityClass
metadata:
  name: critical-production
value: 1000000
globalDefault: false
preemptionPolicy: PreemptLowerPriority  # or Never
description: "For production-critical workloads"
```

Preemption algorithm:
1. For each node, find minimal set of lower-priority pods to evict.
2. Prefer nodes where victims have lowest total priority.
3. Prefer nodes where victims are fewest.
4. Prefer nodes where victims have shortest remaining grace period.
5. Record `nominatedNodeName` on the preemptor pod.
6. Delete victims with graceful termination.
7. Next scheduling cycle places preemptor on nominated node.

---

## 8. Scaling Strategy

### Scaling Kubernetes Control Plane

| Component | Scaling Method | Limits |
|-----------|---------------|--------|
| API Server | Horizontal — add replicas behind LB | Tested to 7+ replicas by Google for GKE |
| etcd | Vertical (SSD, RAM) + careful horizontal (3→5 nodes max) | > 5 nodes adds latency; DB size < 8 GB |
| Scheduler | Leader-elected standby; custom schedulers for different workload classes | Single active instance handles ~5,000 nodes |
| Controller Manager | Leader-elected standby; shard by namespace (experimental) | Single active instance handles ~5,000 nodes |

**API Server Scaling:**

```
Client Request Rate:
  - 5,000 nodes × 1 heartbeat/10s = 500 QPS (node updates)
  - 150,000 pods × watch events = ~1,000 QPS
  - Controller reconciliations = ~1,000 QPS
  - User traffic = ~100 QPS
  Total: ~2,600 QPS

API Server Sizing:
  - Each API server instance: 8 vCPU, 32 GB RAM
  - Handles ~1,500 QPS with p99 < 200ms
  - 3 instances behind LB for HA + headroom = 4,500 QPS capacity
  
  --max-requests-inflight=400 (non-mutating)
  --max-mutating-requests-inflight=200
  
  With APF enabled: FlowSchema + PriorityLevel replaces these flags.
```

**etcd Scaling:**

```
etcd is the bottleneck in most large clusters.

Vertical scaling:
  - Dedicated SSD (NVMe preferred): fsync < 5ms
  - 8 GB RAM minimum, 16 GB recommended for 5k-node cluster
  - Dedicated CPU (no sharing with other processes)
  
Horizontal scaling:
  - 3 nodes = tolerate 1 failure (standard)
  - 5 nodes = tolerate 2 failures (large production)
  - > 5 nodes: NOT recommended (more replicas = slower consensus)
  
Data isolation:
  - Separate etcd cluster for Events (high volume, low criticality)
  - --etcd-servers-overrides=/events#https://events-etcd:2379
  - Reduces write load on main etcd by ~30%
```

### Interviewer Q&As

**Q1: At what scale does a single Kubernetes cluster start to break down, and what are the symptoms?**
A: Kubernetes is tested to 5,000 nodes, 150,000 pods, 10,000 services. Beyond this: (1) etcd write latency increases (Raft consensus across more data). (2) API server LIST operations become expensive (large payloads). (3) Service CIDR exhaustion (default /12 = 1M IPs but iptables rules scale O(n)). (4) Endpoint objects exceed 1MB etcd limit (EndpointSlices mitigate this). (5) Scheduler throughput drops with complex topology spread constraints. Symptoms: increasing `etcd_request_duration_seconds`, growing `apiserver_request_duration_seconds` for LIST, pods stuck in Pending.

**Q2: How do you scale Kubernetes for 50,000 nodes?**
A: You don't use a single cluster. Solutions: (1) **Federation / multi-cluster** — split workloads across multiple clusters with a fleet management layer (Cluster API, ArgoCD, Fleet). (2) **Virtual kubelet** — register serverless backends (Fargate, ACI) as virtual nodes. (3) **Hierarchical namespaces** — reduce blast radius within a single cluster. (4) **Cell-based architecture** — each "cell" is a k8s cluster serving a partition of users/regions. Google uses this approach for GKE internally.

**Q3: How do you handle etcd performance degradation under load?**
A: (1) Separate Events to a dedicated etcd cluster. (2) Ensure SSD-backed storage with fsync < 5ms. (3) Run compaction more frequently to keep DB size small. (4) Enable gRPC proxy/load balancing for read-heavy workloads. (5) Increase `--quota-backend-bytes` if hitting limits. (6) Monitor `etcd_mvcc_db_total_size_in_bytes`, `etcd_disk_wal_fsync_duration_seconds`, `etcd_network_peer_round_trip_time_seconds`. (7) Defragment during maintenance windows.

**Q4: How does the API server cache reduce load on etcd?**
A: The API server maintains an in-memory watch cache for every resource type. On startup, it does a LIST from etcd and then maintains the cache via watch. All client GETs and LISTs with `resourceVersion != ""` are served from cache (serializable reads). Only writes and explicit linearizable reads hit etcd. This means that in a 5,000-node cluster, only ~3,000 QPS hit etcd (writes + leader election), while ~10,000+ QPS of reads are served from cache.

**Q5: How do you handle control plane scaling in a bare-metal environment without cloud LBs?**
A: (1) Run HAProxy or keepalived with a VIP (Virtual IP) in front of API servers. (2) Use BGP anycast for API server load balancing (with kube-vip or MetalLB). (3) Configure kubelets with multiple `--apiserver` endpoints for failover. (4) etcd runs on dedicated bare-metal nodes with local NVMe SSDs. (5) Use Talos Linux or Flatcar for immutable OS on control plane nodes to reduce operational overhead.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | Single API server crash | Load shifts to remaining instances | LB health check (HTTP /readyz) | Automatic via LB; restart by systemd/kubelet | < 10s |
| 2 | All API servers down | Cluster unreachable; running pods continue but no new scheduling | External monitoring (blackbox prober) | Restart API servers; pods keep running (kubelet is autonomous) | 1-5 min |
| 3 | etcd leader failure | 1-2s election timeout; writes blocked during election | `etcd_server_has_leader` metric drops to 0 | Automatic leader re-election | < 5s |
| 4 | etcd quorum loss | All writes blocked; cluster effectively frozen | `etcd_server_proposals_failed_total` spikes | Restore from backup; etcdctl snapshot restore | 5-30 min |
| 5 | etcd data corruption | Cluster state inconsistent or lost | etcd consistency check, checksum mismatch | Restore from snapshot backup | 15-60 min |
| 6 | Scheduler crash | New pods not scheduled; running pods unaffected | Leader lease expires; `scheduler_pending_pods` increases | Standby takes leadership in < 15s | < 15s |
| 7 | Controller manager crash | No reconciliation; desired state drift accumulates | Leader lease expires; `workqueue_depth` for all controllers drops to 0 | Standby takes leadership in < 15s | < 15s |
| 8 | Network partition (CP ↔ nodes) | Nodes marked NotReady after timeout; pods evicted after 5 min | `node_collector_evictions_total` | Heal network; pods rescheduled to healthy nodes | 5-10 min |
| 9 | Webhook server crash (failurePolicy=Fail) | All relevant writes rejected | `apiserver_admission_webhook_rejection_count` spikes | Restart webhook; consider failurePolicy=Ignore for non-critical | < 1 min |
| 10 | Certificate expiry | TLS failures between all components | cert-manager alerts, x509 error logs | Rotate certificates; restart affected components | 5-30 min |

### Automated Recovery (k8s controllers)

**Self-Healing Mechanisms:**

| Mechanism | Implementation | What It Recovers |
|-----------|---------------|-----------------|
| ReplicaSet controller | Watches pod count vs desired; creates/deletes pods | Container crashes, node failures |
| DaemonSet controller | Ensures one pod per eligible node; recreates on new nodes | Missing system daemons |
| StatefulSet controller | Ordered recreation with stable identity and PVCs | Stateful workload failures |
| Node lifecycle controller | Taints NotReady nodes; evicts pods after timeout | Node failures |
| Endpoint controller | Updates Endpoints/EndpointSlices when pods change | Service discovery staleness |
| PV controller | Rebinds PVs when PVCs are recreated | Storage attachment failures |
| Leader election | Lease-based; standby promotes in < 15s | Scheduler/controller-manager crashes |

**etcd Backup Strategy:**

```
Automated backup (every 30 minutes):
  etcdctl snapshot save /backup/etcd-$(date +%Y%m%d-%H%M%S).db \
    --endpoints=https://etcd-1:2379 \
    --cacert=/etc/etcd/ca.crt \
    --cert=/etc/etcd/client.crt \
    --key=/etc/etcd/client.key

Verify backup:
  etcdctl snapshot status /backup/etcd-latest.db --write-out=table

Restore procedure:
  1. Stop all etcd members
  2. etcdctl snapshot restore /backup/etcd-latest.db \
       --data-dir=/var/lib/etcd-restored \
       --name=etcd-1 \
       --initial-cluster=etcd-1=https://...,etcd-2=https://...,etcd-3=https://...
  3. Start each member with --data-dir=/var/lib/etcd-restored
  4. Verify cluster health
  5. Restart API servers
```

---

## 10. Observability

### Key Metrics

| Metric | Source | Alert Threshold | Meaning |
|--------|--------|----------------|---------|
| `etcd_server_has_leader` | etcd | == 0 for > 10s | etcd member lost contact with leader |
| `etcd_disk_wal_fsync_duration_seconds` | etcd | p99 > 25ms | Disk too slow for etcd |
| `etcd_mvcc_db_total_size_in_bytes` | etcd | > 6 GB (of 8 GB quota) | Database nearing quota |
| `etcd_network_peer_round_trip_time_seconds` | etcd | p99 > 50ms | Network latency between etcd peers |
| `apiserver_request_duration_seconds` | API server | p99 > 1s for reads, > 5s for writes | API server overloaded or etcd slow |
| `apiserver_current_inflight_requests` | API server | > 80% of limit | Approaching request throttling |
| `apiserver_admission_webhook_admission_duration_seconds` | API server | p99 > 5s | Webhook latency dragging down API server |
| `scheduler_pending_pods` | Scheduler | > 0 for > 5 min | Pods cannot be scheduled |
| `scheduler_e2e_scheduling_duration_seconds` | Scheduler | p99 > 5s | Scheduling too slow |
| `workqueue_depth` | Controller manager | > 1,000 for any controller | Controller falling behind |
| `workqueue_retries_total` | Controller manager | Rate > 100/s | High error rate in reconciliation |
| `rest_client_request_duration_seconds` | Any k8s component | p99 > 1s | Client-side API latency |
| `etcd_server_proposals_failed_total` | etcd | Rate > 0 | Raft proposal failures (quorum issues) |

### Distributed Tracing

```
Tracing in the control plane:

1. API server supports OpenTelemetry tracing (--tracing-config-file, GA in v1.32)
   - Traces request lifecycle: auth → admission → etcd → response
   - Propagates trace context to webhooks via HTTP headers
   
2. etcd supports distributed tracing (experimental)
   - Traces Raft operations, bbolt transactions

3. Trace correlation:
   Client → API Server → Webhook → etcd
   All share same trace ID via W3C Trace Context headers

Configuration:
  apiVersion: apiserver.config.k8s.io/v1beta1
  kind: TracingConfiguration
  endpoint: otel-collector.monitoring:4317
  samplingRatePerMillion: 1000  # 0.1% sampling
```

### Logging

```
Structured logging (klog v2, JSON format):

API server logs to watch:
  - "rejected" — admission webhook rejections
  - "etcd" — etcd connectivity issues
  - "timeout" — request timeouts
  - "429" — rate limiting (APF)

etcd logs:
  - "raft" — leader election, term changes
  - "slow" — slow apply, slow fdatasync
  - "compact" — compaction progress
  - "overloaded" — too many pending proposals

Controller manager:
  - "requeue" — failed reconciliation with backoff
  - "error syncing" — persistent reconciliation failures

Log aggregation:
  - Ship to Elasticsearch via Fluentd/Vector
  - Structured JSON enables field-based queries
  - Retain control plane logs for 30 days minimum
```

---

## 11. Security

### Auth & AuthZ

**Authentication:**

| Method | Use Case | Strength |
|--------|----------|----------|
| x509 client certs | Component-to-component (kubelet, scheduler) | Strong; no token to steal. Weakness: cert rotation complexity |
| OIDC tokens | Human users via corporate IdP | SSO integration; short-lived tokens; group claims |
| Service account tokens | Pod-to-API-server | Bound tokens (audience + time scoped); auto-mounted |
| Webhook token | Custom authentication backends | Flexible; enables MFA, conditional access |

**RBAC Deep Dive:**

```yaml
# Principle of least privilege
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  namespace: production
  name: pod-reader
rules:
- apiGroups: [""]
  resources: ["pods"]
  verbs: ["get", "list", "watch"]
- apiGroups: [""]
  resources: ["pods/log"]
  verbs: ["get"]
---
apiVersion: rbac.authorization.k8s.io/v1
kind: RoleBinding
metadata:
  namespace: production
  name: read-pods
subjects:
- kind: Group
  name: "frontend-team"       # From OIDC groups claim
  apiGroup: rbac.authorization.k8s.io
roleRef:
  kind: Role
  name: pod-reader
  apiGroup: rbac.authorization.k8s.io
```

**RBAC aggregation for extensibility:**

```yaml
# Automatically aggregate permissions from CRDs
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRole
metadata:
  name: monitoring-view
  labels:
    rbac.authorization.k8s.io/aggregate-to-view: "true"
rules:
- apiGroups: ["monitoring.coreos.com"]
  resources: ["prometheuses", "alertmanagers"]
  verbs: ["get", "list", "watch"]
# This auto-aggregates into the built-in "view" ClusterRole
```

### Multi-tenancy Isolation

| Layer | Mechanism | What It Isolates |
|-------|-----------|-----------------|
| API | RBAC Roles/RoleBindings per namespace | Resource access |
| Network | NetworkPolicy (default deny ingress/egress per namespace) | Pod-to-pod traffic |
| Resource | ResourceQuota + LimitRange per namespace | CPU, memory, storage, object counts |
| Security | Pod Security Standards (Restricted) per namespace | Container capabilities, privilege escalation |
| Admission | OPA/Gatekeeper policies | Image registries, labels, resource limits |
| Secrets | Encryption at rest (`--encryption-provider-config`) | Secret data in etcd |

**Encryption at rest configuration:**

```yaml
apiVersion: apiserver.config.k8s.io/v1
kind: EncryptionConfiguration
resources:
  - resources:
      - secrets
      - configmaps
    providers:
      - aescbc:
          keys:
            - name: key1
              secret: <base64-encoded-32-byte-key>
      - identity: {}   # Fallback for reading unencrypted data during migration
```

---

## 12. Incremental Rollout Strategy

### Phase 1: Single Control Plane (Week 1-2)
- Deploy 3-node etcd cluster on dedicated SSD nodes.
- Deploy single API server, scheduler, controller-manager.
- Validate with synthetic workloads (10 nodes, 500 pods).
- Establish baseline metrics.

### Phase 2: HA Control Plane (Week 3-4)
- Add 2 additional API servers behind LB.
- Enable leader election for scheduler and controller-manager.
- Test failover: kill each component and measure recovery time.
- Implement etcd backup automation (every 30 min to object storage).

### Phase 3: Security Hardening (Week 5-6)
- Enable RBAC with deny-by-default.
- Configure OIDC authentication with corporate IdP.
- Enable encryption at rest for Secrets.
- Deploy OPA/Gatekeeper with baseline policies.
- Enable audit logging.

### Phase 4: Scale Testing (Week 7-8)
- Load test to 1,000 nodes with kubemark (simulated kubelets).
- Tune API server parameters (max-requests-inflight, APF config).
- Separate Events etcd if needed.
- Establish SLOs and alerting.

### Phase 5: Production Promotion (Week 9-10)
- Migrate workloads from staging.
- Enable PodDisruptionBudgets for critical workloads.
- Establish upgrade runbook and test control plane upgrade procedure.

### Rollout Interviewer Q&As

**Q1: How do you perform a zero-downtime upgrade of the Kubernetes control plane?**
A: (1) Upgrade etcd first (rolling, one member at a time). (2) Upgrade API servers (rolling behind LB; new version serves both old and new API versions). (3) Upgrade controller-manager and scheduler (leader election ensures seamless handover). (4) Important: verify version skew policy — API server must be the first component upgraded and kubelet can be at most 2 minor versions behind API server. (5) Run `kubectl get nodes` to verify all nodes report correct version.

**Q2: How do you handle a failed control plane upgrade mid-way?**
A: Kubernetes API server supports serving multiple API versions simultaneously. If the upgrade fails mid-way: (1) Stop the upgrade. (2) Roll back the failed component to the previous version. (3) Verify etcd is healthy (data format is backward-compatible within 1 minor version). (4) Investigate the failure in a staging environment. (5) API server storage encoding is versioned — newer stored objects may need `--storage-media-type` adjustment if rolling back.

**Q3: How do you handle etcd upgrades?**
A: etcd has strict upgrade rules: (1) Only upgrade one minor version at a time (e.g., 3.5 → 3.6, not 3.4 → 3.6). (2) Take a snapshot backup before starting. (3) Rolling upgrade: stop member, replace binary, restart. (4) Upgrade the leader last to minimize elections. (5) Verify cluster health after each member: `etcdctl endpoint health --cluster`. (6) Monitor `etcd_server_version` to confirm all members upgraded.

**Q4: How do you roll out a new admission webhook without breaking the cluster?**
A: (1) Deploy the webhook with `failurePolicy: Ignore` initially. (2) Monitor `apiserver_admission_webhook_rejection_count` — see what would be rejected. (3) Analyze rejection reasons in webhook logs. (4) Fix false positives in webhook logic. (5) Switch to `failurePolicy: Fail` once confident. (6) Use `namespaceSelector` to exclude `kube-system` and critical namespaces. (7) Set `timeoutSeconds: 5` to prevent webhook latency from blocking API server.

**Q5: How do you migrate from a single-control-plane cluster to HA?**
A: (1) Add new etcd members one at a time: `etcdctl member add`. (2) Wait for each to sync before adding the next. (3) Deploy additional API server instances pointed at the same etcd cluster. (4) Put all API servers behind a load balancer. (5) Update kubeconfig endpoint to the LB address. (6) Update kubelet configurations to point to LB. (7) Scheduler and controller-manager already support leader election — just deploy standby instances.

---

## 13. Trade-offs & Decision Log

| # | Decision | Alternative Considered | Trade-off | Rationale |
|---|----------|----------------------|-----------|-----------|
| 1 | etcd as datastore | PostgreSQL via kine | Lower throughput but native watch | k8s watch semantics require revision-based streaming; etcd provides this natively |
| 2 | 3-node etcd (not 5) | 5-node etcd | Less fault tolerance but lower write latency | 3-node sufficient for most clusters; 5-node only for > 3,000 nodes |
| 3 | Leader-elected scheduler | Multi-active scheduler | Lower throughput but simpler consistency | Single scheduler avoids double-booking; throughput sufficient for 5,000 nodes |
| 4 | In-memory API server cache | Direct etcd reads | Stale reads possible but 100x lower latency | Informer cache with watch ensures near-real-time consistency; acceptable for most reads |
| 5 | RBAC over ABAC | ABAC with policy files | Less flexible but auditable and dynamic | RBAC is Kubernetes-native, supports dynamic policy updates, integrates with OIDC groups |
| 6 | Mutating + validating webhooks | Built-in admission plugins only | External dependency but extensible | Webhooks enable custom policy (OPA, Kyverno) without modifying API server |
| 7 | etcd encryption at rest | No encryption | Performance overhead (~5%) but data protection | Required for compliance (PCI-DSS, SOC2); secrets must be encrypted |
| 8 | API Priority and Fairness | Simple max-inflight | More complex but fair resource allocation | Prevents rogue clients from starving critical system operations |
| 9 | Separate Events etcd | Shared etcd | Operational complexity but protects main datastore | Events are high-volume, low-criticality; isolation prevents them from impacting scheduling |
| 10 | Server-side apply | Client-side apply | Migration effort but prevents field conflicts | Eliminates last-writer-wins bugs between controllers |

---

## 14. Agentic AI Integration

### AI-Powered Control Plane Operations

| Use Case | AI Agent Capability | Implementation |
|----------|-------------------|---------------|
| **Predictive scaling** | Analyze historical scheduling patterns to pre-provision nodes before demand spikes | Agent monitors `scheduler_pending_pods` trends + time-series forecasting model; triggers Cluster Autoscaler pre-emptively |
| **Anomaly detection** | Detect unusual etcd latency, API server error rates, or scheduling failures | Agent watches Prometheus metrics; uses statistical anomaly detection to alert before SLO breach |
| **Automated root cause analysis** | Correlate symptoms (pod pending, node NotReady, etcd slow) to root cause | Agent traverses dependency graph: pod → scheduler → API server → etcd → disk → hardware; generates diagnosis |
| **Intelligent admission** | AI-powered admission webhook that evaluates resource requests for optimization | Mutating webhook suggests right-sized resource requests based on historical usage (VPA-like but inference-time) |
| **Configuration tuning** | Auto-tune scheduler parameters, etcd compaction intervals, API server concurrency | Agent runs experiments with different configs in staging; applies best-performing config to production |
| **Capacity planning** | Predict cluster growth and recommend when to scale etcd, add API servers, or split clusters | Agent analyzes growth trends in object count, QPS, node count; recommends scaling actions with cost estimates |

### Architecture for AI Agent:

```
┌─────────────────────────────────────────────────────────┐
│                   AI Operations Agent                    │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐  │
│  │ Metrics   │  │ Log      │  │ Knowledge Base       │  │
│  │ Collector │  │ Analyzer │  │ (runbooks, k8s docs, │  │
│  │ (Prom API)│  │ (ES API) │  │  past incidents)     │  │
│  └─────┬────┘  └────┬─────┘  └──────────┬───────────┘  │
│        │             │                   │               │
│        └─────────────┼───────────────────┘               │
│                      │                                   │
│              ┌───────▼─────────┐                         │
│              │  LLM Reasoning  │                         │
│              │  Engine         │                         │
│              └───────┬─────────┘                         │
│                      │                                   │
│        ┌─────────────┼──────────────┐                    │
│        │             │              │                    │
│  ┌─────▼─────┐ ┌────▼─────┐ ┌─────▼──────┐            │
│  │ Alert     │ │ Auto-    │ │ Recommend  │            │
│  │ Generator │ │ Remediate│ │ (human     │            │
│  │           │ │ (kubectl)│ │  approval) │            │
│  └───────────┘ └──────────┘ └────────────┘            │
└─────────────────────────────────────────────────────────┘

Safety guardrails:
- Auto-remediation limited to pre-approved actions (restart pod, cordon node)
- Configuration changes require human approval via PR review
- All agent actions logged and auditable
- Kill switch to disable agent in emergencies
- Agent cannot modify its own RBAC permissions
```

### Example: AI-Driven etcd Health Management

```python
class EtcdHealthAgent:
    """
    Monitors etcd cluster health and takes proactive actions.
    """
    
    def analyze(self):
        metrics = self.prometheus.query_range(
            'etcd_mvcc_db_total_size_in_bytes',
            start='-24h', step='5m'
        )
        
        growth_rate = self.calculate_growth_rate(metrics)
        current_size = metrics[-1].value
        quota = self.get_etcd_quota()
        
        time_to_full = (quota - current_size) / growth_rate
        
        if time_to_full < timedelta(hours=24):
            return Action(
                type='URGENT',
                description=f'etcd will hit quota in {time_to_full}',
                remediation=[
                    'Run etcd compaction immediately',
                    'Run defragmentation on each member',
                    'Investigate object growth (kubectl get all --all-namespaces | wc -l)',
                ]
            )
        elif time_to_full < timedelta(days=7):
            return Action(
                type='WARNING',
                description=f'etcd approaching quota in {time_to_full}',
                remediation=[
                    'Increase compaction frequency',
                    'Review Event retention policy',
                    'Consider increasing --quota-backend-bytes',
                ]
            )
        
        # Check for slow fsync
        fsync_p99 = self.prometheus.query(
            'histogram_quantile(0.99, etcd_disk_wal_fsync_duration_seconds_bucket)'
        )
        if fsync_p99 > 0.025:  # 25ms threshold
            return Action(
                type='WARNING',
                description=f'etcd fsync p99 = {fsync_p99*1000:.1f}ms (threshold 25ms)',
                remediation=[
                    'Check for noisy neighbors on the disk',
                    'Consider migrating to NVMe SSD',
                    'Verify no other process is writing to the same disk',
                ]
            )
```

---

## 15. Complete Interviewer Q&A Bank

### Architecture & Design (Q1-Q5)

**Q1: Walk me through what happens when you run `kubectl create deployment nginx --image=nginx --replicas=3`.**
A: (1) kubectl sends a POST to `/apis/apps/v1/namespaces/default/deployments` with the Deployment spec. (2) API server authenticates (kubeconfig cert/token), authorizes (RBAC check: can this user create Deployments?), runs mutating admission (e.g., inject labels), validates the schema, runs validating admission. (3) Writes to etcd; returns Deployment with resourceVersion. (4) Deployment controller (watching Deployments) creates a ReplicaSet. (5) ReplicaSet controller (watching ReplicaSets and Pods) creates 3 Pods. (6) Scheduler (watching Pods with empty nodeName) selects a node for each pod via filter/score/bind. (7) Kubelet on each selected node (watching Pods assigned to it) pulls the image, creates the container, starts it. (8) Kubelet updates Pod status. (9) Endpoints controller updates the Service endpoints.

**Q2: Why is the API server stateless, and what are the implications?**
A: The API server stores no state locally — all state is in etcd. Implications: (1) Horizontal scaling is trivial — add more instances behind a load balancer. (2) Any instance can serve any request — no session affinity needed. (3) A crash loses zero data. (4) Watch cache must be rebuilt on restart (LIST from etcd), causing a brief increase in etcd read load. (5) In-flight requests are lost on crash — clients must retry.

**Q3: How does Kubernetes ensure that two controllers don't conflict when updating the same object?**
A: (1) **Optimistic concurrency**: every update includes `resourceVersion`. If another write occurred since the client read, etcd transaction fails → API server returns 409 Conflict → client re-reads and retries. (2) **Server-side apply**: field managers track which controller owns which fields. Conflicts are detected and rejected unless force-applied. (3) **Owner references**: hierarchical ownership prevents two controllers from claiming the same object.

**Q4: Explain the lifecycle of a watch connection from kubectl to etcd.**
A: (1) kubectl opens HTTP/2 GET to `/api/v1/pods?watch=true&resourceVersion=12345`. (2) API server registers the watch in its watch cache for the Pods resource. (3) The watch cache is populated by a single reflector that watches etcd key prefix `/registry/pods/`. (4) When a pod changes in etcd, the etcd watch stream sends the event to the API server's reflector. (5) The reflector updates the cache and fans out the event to all matching watch subscribers. (6) The API server serializes the event (JSON/protobuf) and sends it over the HTTP/2 stream to kubectl. (7) If the connection drops, kubectl reconnects with the last resourceVersion it received.

**Q5: What is the purpose of the cloud controller manager, and why was it split from the main controller manager?**
A: The cloud controller manager (CCM) runs controllers that interact with cloud provider APIs: (1) Node controller — initializes nodes with cloud-specific labels (zone, instance type), deletes k8s Node objects when the VM is terminated. (2) Route controller — configures cloud VPC routes for pod CIDR. (3) Service controller — provisions cloud load balancers for Service type=LoadBalancer. It was split to decouple cloud-specific code from the core Kubernetes codebase, allowing cloud providers to develop and release their CCM independently. In bare-metal environments, you either don't run a CCM or use a bare-metal CCM like MetalLB (for Services) + custom node lifecycle.

### Scaling & Performance (Q6-Q10)

**Q6: How do you benchmark Kubernetes control plane performance?**
A: (1) Use `perf-tests/clusterloader2` (official k8s scalability tool) — it creates workloads and measures API latency, scheduling throughput, and watch delivery time. (2) Key SLIs from the k8s scalability SIG: API call latency (p99 < 1s for single-object, p99 < 30s for LIST), pod startup latency (p99 < 5s), watch delivery latency (p99 < 5s). (3) Use kubemark for synthetic node simulation — 1 kubemark pod simulates 1 kubelet. (4) Monitor etcd via `etcd_debugging_mvcc_slow_watcher_total` and `etcd_disk_backend_commit_duration_seconds`.

**Q7: How do EndpointSlices improve scalability over Endpoints?**
A: Traditional Endpoints store all pod IPs for a service in a single object. For a service with 5,000 pods, the Endpoints object exceeds 100 KB — every pod change triggers a full object update sent to all watchers (kube-proxy on every node). EndpointSlices split this into chunks of 100 endpoints each, so a pod change only updates one ~5 KB slice. This reduces watch traffic by ~98% for large services and stays under etcd's 1.5 MB object size limit.

**Q8: How does API server request throttling work?**
A: Two mechanisms: (1) **Legacy**: `--max-requests-inflight` (default 400) and `--max-mutating-requests-inflight` (default 200). Excess requests get HTTP 429 with Retry-After header. (2) **APF (API Priority and Fairness)**: Requests are classified by FlowSchema (matching user/group/verb/resource) into PriorityLevels with assigned concurrency shares. System-critical requests (leader election, node heartbeats) get guaranteed shares. Fair queuing within each priority level prevents any single client from monopolizing bandwidth.

**Q9: How do you handle the "big cluster" problem where LIST calls return millions of objects?**
A: (1) **Pagination**: Use `limit` and `continue` parameters. API server returns `limit` objects + a `continue` token for the next page. (2) **Resource version semantics**: `resourceVersion=""` serves from cache (fast, consistent within server). (3) **Selective field projection**: `fieldSelector` and `labelSelector` reduce returned objects. (4) **Watch instead of poll**: Use informers that LIST once on startup then WATCH for incremental changes. (5) **Streaming LIST** (alpha in v1.32): Streams objects instead of buffering the entire response in memory.

**Q10: What are the memory implications of running many CRDs?**
A: Each CRD adds: (1) API server: new REST handler, validation logic, watch cache (~memory proportional to object count). (2) etcd: new key prefix. (3) Discovery cache: every client (kubectl, controllers) caches API discovery info for all registered resources. With 500+ CRDs (common in large platforms), discovery refresh can be slow. Mitigation: aggregated discovery (v1.30+), which batches discovery into fewer API calls. Also consider: each CRD controller adds its own informer cache, multiplying memory usage.

### Reliability & Operations (Q11-Q15)

**Q11: How do you restore a Kubernetes cluster from a complete etcd failure?**
A: (1) Stop all API servers. (2) Identify latest healthy etcd snapshot (from automated backups). (3) For each etcd member: `etcdctl snapshot restore <snapshot> --data-dir=/var/lib/etcd-new --name=<member-name> --initial-cluster=<members>`. (4) Start all etcd members simultaneously with new data directory. (5) Verify cluster health: `etcdctl endpoint health --cluster`. (6) Start API servers. (7) Verify all objects are present: `kubectl get all --all-namespaces`. Note: any changes between the last backup and the failure are lost. This is why frequent backups and short RPO targets matter.

**Q12: How does leader election work for the scheduler and controller-manager?**
A: Uses the Lease object in `kube-system` namespace. The leader periodically renews the lease (default: every 2s, timeout: 15s). Process: (1) Each instance tries to create/acquire the Lease with its identity. (2) The winner becomes leader and starts processing. (3) Standbys poll the Lease; if the leader fails to renew before timeout, a standby takes over. (4) The new leader re-builds its informer cache (LIST + WATCH) before processing, causing a brief delay. Implementation uses `client-go/tools/leaderelection` with Lease-based lock.

**Q13: What happens to running pods when the control plane is completely down?**
A: Running pods continue to run. The kubelet on each node is autonomous — it manages container lifecycle locally using its cached pod specs. However: (1) No new pods can be scheduled. (2) No failed pods can be restarted by controllers. (3) Horizontal scaling stops. (4) Service endpoints are not updated (but existing endpoints still work via kube-proxy). (5) Nodes continue to try to heartbeat but failures are not acted upon. This is the "autonomous kubelet" design — intentional decoupling for resilience.

**Q14: How do you audit who did what in a Kubernetes cluster?**
A: Enable API audit logging via `--audit-policy-file`. The policy defines what to log at what level: (1) None — don't log. (2) Metadata — log request metadata (user, verb, resource, timestamp) but not body. (3) Request — log metadata + request body. (4) RequestResponse — log metadata + request + response body. Best practices: log all write operations at Request level, read operations at Metadata level, exclude high-volume resources (Events, leases) at None level. Send audit logs to a dedicated Elasticsearch index for retention and analysis. The audit log captures: who (user, groups, service account), what (verb, resource, name), when (timestamp), and where (source IP, user-agent).

**Q15: How do you handle the scenario where a mutating admission webhook is creating a circular dependency (e.g., the webhook server runs as pods in the same cluster)?**
A: This is a common bootstrapping problem. Solutions: (1) Exclude the webhook's own namespace using `namespaceSelector` in the webhook configuration (the webhook should not mutate its own pods). (2) Use `objectSelector` to skip pods with a specific label. (3) Set `failurePolicy: Ignore` for the webhook so that if the webhook pods are down, other pods can still be created (including new webhook pods). (4) Use `reinvocationPolicy: IfNeeded` carefully — it can cause infinite loops if two mutating webhooks modify each other's changes. (5) Consider running critical webhooks outside the cluster (e.g., as a serverless function) to break the dependency.

### Advanced Topics (Q16-Q20)

**Q16: Explain the informer/SharedInformerFactory pattern and why it matters.**
A: SharedInformerFactory ensures that all controllers in the same process share a single watch connection and cache per resource type. Without it, 30 controllers each watching Pods would create 30 separate watch connections to the API server — wasting bandwidth and API server resources. The SharedInformer: (1) Runs one LIST + WATCH per resource type. (2) Maintains one in-memory cache (thread-safe store). (3) Fans out events to multiple registered handlers. (4) Each handler enqueues work into its own work queue. This reduces API server load from O(controllers x resources) to O(resources).

**Q17: How does Kubernetes handle split-brain scenarios in a multi-AZ deployment?**
A: Kubernetes relies on etcd's Raft consensus, which inherently prevents split-brain: only the partition with a majority of etcd members can continue to serve writes. The minority partition becomes read-only (or unavailable). However, nodes in the minority partition's AZ will lose connectivity to the API server: (1) After `--node-monitor-grace-period` (40s default), nodes are marked Unknown. (2) After `--pod-eviction-timeout` (5 min default), pods on those nodes are evicted. (3) To prevent premature eviction during AZ failures, configure `--unhealthy-zone-threshold` so the node controller reduces eviction rate when > 55% of nodes in a zone are unhealthy.

**Q18: What is the difference between strategic merge patch and JSON merge patch in Kubernetes?**
A: JSON merge patch replaces arrays entirely — if you have 3 containers and patch with 1 container spec, you end up with 1 container. Strategic merge patch (Kubernetes-specific) understands array merge keys (e.g., `name` for containers) and merges array elements by key. Example: patching `containers[name=sidecar]` only updates that container, leaving others untouched. Server-side apply goes further with field managers, tracking which fields are owned by which client. This is critical for controllers that need to modify specific fields without clobbering other controllers' changes.

**Q19: How do you implement a custom scheduler in Kubernetes?**
A: Three approaches: (1) **Scheduler extender** (deprecated) — HTTP webhook called by the default scheduler for additional filtering/scoring. Simple but adds latency. (2) **Scheduling framework plugin** (preferred) — implement the framework.Plugin interface (Filter, Score, Reserve, Permit, etc.), compile into the scheduler binary, and register via KubeSchedulerConfiguration. (3) **Multiple schedulers** — deploy a separate scheduler process; set `schedulerName` in the pod spec to route pods to the custom scheduler. Approach 2 is preferred because it runs in-process (no RPC overhead), has access to the scheduler's cache, and supports all extension points.

**Q20: How does Kubernetes handle clock skew between control plane components?**
A: Clock skew can cause: (1) Certificate validation failures (cert not yet valid or expired). (2) Lease expiration miscalculations (leader election). (3) Token validation failures (JWT `nbf` and `exp` claims). Mitigation: (1) Run NTP on all control plane nodes with tight synchronization (< 1s skew). (2) etcd uses logical clocks (Raft term + index) rather than wall clocks for consensus, making it resilient to clock skew. (3) API server token validation has a configurable clock skew tolerance. (4) Certificate libraries typically allow small clock skew (configurable). Production best practice: monitor NTP offset and alert if > 500ms.

---

## 16. References

| # | Reference | URL |
|---|-----------|-----|
| 1 | Kubernetes Architecture | https://kubernetes.io/docs/concepts/architecture/ |
| 2 | etcd documentation | https://etcd.io/docs/ |
| 3 | Kubernetes Scheduling Framework | https://kubernetes.io/docs/concepts/scheduling-eviction/scheduling-framework/ |
| 4 | API Priority and Fairness | https://kubernetes.io/docs/concepts/cluster-administration/flow-control/ |
| 5 | Kubernetes Scalability SIG | https://github.com/kubernetes/community/tree/master/sig-scalability |
| 6 | ClusterLoader2 perf tests | https://github.com/kubernetes/perf-tests/tree/master/clusterloader2 |
| 7 | etcd Raft implementation | https://github.com/etcd-io/raft |
| 8 | Kubernetes RBAC | https://kubernetes.io/docs/reference/access-authn-authz/rbac/ |
| 9 | API Server Admission Control | https://kubernetes.io/docs/reference/access-authn-authz/admission-controllers/ |
| 10 | Server-Side Apply | https://kubernetes.io/docs/reference/using-api/server-side-apply/ |
| 11 | Watch Bookmarks KEP | https://github.com/kubernetes/enhancements/tree/master/keps/sig-api-machinery/956-watch-bookmark |
| 12 | Kubernetes the Hard Way | https://github.com/kelseyhightower/kubernetes-the-hard-way |
