# System Design: Kubernetes Operator Framework

> **Relevance to role:** Operators encode operational knowledge into software — they are how infrastructure teams automate day-2 operations (upgrades, backups, failover, scaling) for complex stateful systems like databases, message brokers, and monitoring stacks. A cloud infrastructure platform engineer must understand the operator pattern deeply: how to design, build, and operate custom operators, and how to evaluate third-party operators for production readiness.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Define custom resources (CRDs) that represent the desired state of a managed system |
| FR-2 | Implement reconciliation loops that continuously drive actual state toward desired state |
| FR-3 | Handle the full lifecycle: create, update, upgrade, scale, backup, restore, delete |
| FR-4 | Support admission webhooks (defaulting and validation) for CRDs |
| FR-5 | Implement finalizers for safe cleanup on deletion |
| FR-6 | Report status via conditions and events |
| FR-7 | Support leader election for high availability |
| FR-8 | Provide metrics and logging for observability |
| FR-9 | Manage owned resources via owner references for garbage collection |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Reconciliation latency (event → action) | < 5 s |
| NFR-2 | Reconciliation throughput | ≥ 50 objects/s |
| NFR-3 | Operator availability | 99.99% (leader election with standby) |
| NFR-4 | Memory footprint | < 256 MB for managing 1,000 CRs |
| NFR-5 | Startup time (cache sync) | < 30 s |
| NFR-6 | Error recovery (crash → resume) | < 15 s via leader election |
| NFR-7 | API server impact | < 100 QPS per operator (well-behaved) |

### Constraints & Assumptions
- Built using controller-runtime (Go) or kopf (Python) or Java Operator SDK.
- CRDs use structural schemas (required since k8s 1.16).
- Operator runs as a Deployment (2 replicas for HA with leader election).
- Follows the Kubernetes controller conventions (level-triggered, idempotent reconciliation).
- CRD versions follow Kubernetes API versioning conventions (v1alpha1 → v1beta1 → v1).

### Out of Scope
- Specific operator implementations (etcd operator, MySQL operator) — these are referenced as examples.
- Helm chart management (operator is an alternative to Helm for complex lifecycle).
- Multi-cluster operator distribution (mentioned briefly).
- Control plane internals (covered in kubernetes_control_plane.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|------------|--------|
| Custom Resources (CRs) managed | 500 per operator instance (database operator example) | 500 |
| Owned sub-resources per CR | ~10 (StatefulSet, Services, ConfigMaps, Secrets, PVCs, etc.) | 5,000 |
| Total watched objects | 500 CRs + 5,000 owned + related (Pods, Nodes) | ~10,000 |
| Reconciliation rate (steady state) | 500 CRs x 1 reconcile/min (periodic resync) | ~8/s |
| Reconciliation rate (burst) | 50 CRs updated simultaneously | 50/s |
| API server QPS (reads) | 8 reconcile/s x 5 GET calls per reconcile | ~40 QPS |
| API server QPS (writes) | 8 reconcile/s x 1 UPDATE per reconcile | ~8 QPS |
| Webhook invocations | 10 CR create/update per hour | ~0.003 QPS |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| CR creation → first reconciliation | < 2 s | Watch event triggers immediate reconciliation |
| Reconciliation loop execution | < 30 s per CR | Includes API calls, health checks, status update |
| Status condition update | < 5 s after state change | Status subresource update |
| Webhook response | < 5 s | Admission webhook must respond within timeout |
| Operator restart (leader election) | < 15 s | Leader lease timeout + cache resync |

### Storage Estimates

| Component | Calculation | Result |
|-----------|------------|--------|
| CRDs in etcd | 500 CRs x 5 KB avg | ~2.5 MB |
| Owned resources in etcd | 5,000 objects x 3 KB avg | ~15 MB |
| Operator memory (informer cache) | 10,000 objects x 5 KB | ~50 MB |
| Operator logs | 100 MB/day | 100 MB/day |

### Bandwidth Estimates

| Flow | Calculation | Result |
|------|------------|--------|
| Watch events (API → operator) | 100 events/s x 2 KB | ~200 KB/s |
| API calls (operator → API) | 48 QPS x 3 KB | ~144 KB/s |
| Webhook calls (API → operator) | Negligible | < 1 KB/s |

---

## 3. High Level Architecture

```
                    ┌──────────────────────────────────────┐
                    │          Kubernetes API Server        │
                    │                                      │
                    │  ┌──────────────┐ ┌───────────────┐  │
                    │  │ CRD:         │ │ Built-in:     │  │
                    │  │ MySQLCluster │ │ StatefulSet   │  │
                    │  │ MySQLBackup  │ │ Service       │  │
                    │  │ MySQLRestore │ │ ConfigMap     │  │
                    │  └──────────────┘ │ Secret        │  │
                    │                   │ PVC           │  │
                    │                   └───────────────┘  │
                    └─────────┬──────────────┬─────────────┘
                              │              │
                    Watch/CRUD│              │ Webhook calls
                              │              │
                    ┌─────────▼──────────────▼─────────────┐
                    │        Operator Deployment            │
                    │                                       │
                    │  ┌─────────────────────────────────┐ │
                    │  │         Manager                  │ │
                    │  │                                  │ │
                    │  │  ┌──────────────────────────┐   │ │
                    │  │  │    Informer Cache         │   │ │
                    │  │  │ (MySQLCluster, SS, Svc...)│   │ │
                    │  │  └────────────┬─────────────┘   │ │
                    │  │               │                  │ │
                    │  │  ┌────────────▼─────────────┐   │ │
                    │  │  │    Event Handlers         │   │ │
                    │  │  │ → Work Queue              │   │ │
                    │  │  └────────────┬─────────────┘   │ │
                    │  │               │                  │ │
                    │  │  ┌────────────▼─────────────┐   │ │
                    │  │  │   Reconciler              │   │ │
                    │  │  │   (business logic)        │   │ │
                    │  │  │                           │   │ │
                    │  │  │   MySQLClusterReconciler  │   │ │
                    │  │  │   MySQLBackupReconciler   │   │ │
                    │  │  │   MySQLRestoreReconciler  │   │ │
                    │  │  └──────────────────────────┘   │ │
                    │  │                                  │ │
                    │  │  ┌──────────────────────────┐   │ │
                    │  │  │  Webhook Server           │   │ │
                    │  │  │  - Defaulting             │   │ │
                    │  │  │  - Validation             │   │ │
                    │  │  └──────────────────────────┘   │ │
                    │  │                                  │ │
                    │  │  ┌──────────────────────────┐   │ │
                    │  │  │  Leader Election          │   │ │
                    │  │  │  (Lease in kube-system)   │   │ │
                    │  │  └──────────────────────────┘   │ │
                    │  └─────────────────────────────────┘ │
                    └──────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **CRD** | Schema definition for the custom resource; registered in API server |
| **Manager** | Entry point; starts informers, controllers, webhook server, leader election |
| **Informer Cache** | In-memory cache of watched resources; reduces API server load |
| **Work Queue** | Rate-limited, deduplicating queue of reconciliation requests |
| **Reconciler** | Core business logic; compares desired vs. actual state; takes corrective action |
| **Webhook Server** | HTTPS server for mutating (defaulting) and validating admission webhooks |
| **Leader Election** | Ensures only one active reconciler instance via Lease object |

### Data Flows

1. **CR Creation:** User applies MySQLCluster CR → API server validates (webhook) → stores in etcd → watch event → operator reconciler → creates StatefulSet, Services, ConfigMaps, Secrets → monitors pods → updates CR status.

2. **Reconciliation Loop:** Informer detects change → enqueues CR key → reconciler dequeues → reads CR from cache → reads owned resources → compares desired vs. actual → creates/updates/deletes resources → updates CR status → returns (requeue if needed).

3. **Deletion with Finalizers:** User deletes CR → API server sets deletionTimestamp (but does not delete) → reconciler detects deletion → runs cleanup (delete backups, release resources) → removes finalizer → API server completes deletion.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# Custom Resource Definition
apiVersion: apiextensions.k8s.io/v1
kind: CustomResourceDefinition
metadata:
  name: mysqlclusters.database.example.com
spec:
  group: database.example.com
  versions:
    - name: v1
      served: true
      storage: true
      schema:
        openAPIV3Schema:
          type: object
          properties:
            spec:
              type: object
              required: ["replicas", "version"]
              properties:
                replicas:
                  type: integer
                  minimum: 1
                  maximum: 7
                  description: "Number of MySQL instances (odd number for quorum)"
                version:
                  type: string
                  enum: ["8.0", "8.4"]
                storage:
                  type: object
                  properties:
                    size:
                      type: string
                      pattern: "^[0-9]+Gi$"
                    storageClassName:
                      type: string
                resources:
                  type: object
                  properties:
                    cpu:
                      type: string
                    memory:
                      type: string
                backup:
                  type: object
                  properties:
                    schedule:
                      type: string  # Cron expression
                    retention:
                      type: integer
                    destination:
                      type: string  # s3://bucket/path
            status:
              type: object
              properties:
                phase:
                  type: string
                  enum: ["Creating", "Running", "Upgrading", "Failed", "Deleting"]
                readyReplicas:
                  type: integer
                currentVersion:
                  type: string
                conditions:
                  type: array
                  items:
                    type: object
                    properties:
                      type:
                        type: string
                      status:
                        type: string
                        enum: ["True", "False", "Unknown"]
                      lastTransitionTime:
                        type: string
                        format: date-time
                      reason:
                        type: string
                      message:
                        type: string
      subresources:
        status: {}     # Enable status subresource (separate RBAC for status updates)
        scale:
          specReplicasPath: .spec.replicas
          statusReplicasPath: .status.readyReplicas
      additionalPrinterColumns:
        - name: Replicas
          type: integer
          jsonPath: .spec.replicas
        - name: Ready
          type: integer
          jsonPath: .status.readyReplicas
        - name: Version
          type: string
          jsonPath: .status.currentVersion
        - name: Phase
          type: string
          jsonPath: .status.phase
        - name: Age
          type: date
          jsonPath: .metadata.creationTimestamp
  scope: Namespaced
  names:
    plural: mysqlclusters
    singular: mysqlcluster
    kind: MySQLCluster
    shortNames:
      - mysql

---
# Custom Resource instance
apiVersion: database.example.com/v1
kind: MySQLCluster
metadata:
  name: production-db
  namespace: production
  finalizers:
    - database.example.com/cleanup
spec:
  replicas: 3
  version: "8.4"
  storage:
    size: "100Gi"
    storageClassName: fast-ssd
  resources:
    cpu: "4"
    memory: "16Gi"
  backup:
    schedule: "0 2 * * *"    # Daily at 2 AM
    retention: 30
    destination: "s3://backups/mysql/production-db/"
status:
  phase: Running
  readyReplicas: 3
  currentVersion: "8.4"
  conditions:
    - type: Ready
      status: "True"
      lastTransitionTime: "2026-04-10T10:00:00Z"
      reason: AllReplicasReady
      message: "All 3 replicas are running and healthy"
    - type: BackupReady
      status: "True"
      lastTransitionTime: "2026-04-10T02:15:00Z"
      reason: BackupCompleted
      message: "Last backup completed successfully"
```

**Owned Resources (created by the operator):**

```
MySQLCluster: production-db
  ├── StatefulSet: production-db
  │   └── Pod: production-db-0, production-db-1, production-db-2
  │       └── PVC: data-production-db-0, data-production-db-1, data-production-db-2
  ├── Service: production-db (headless, for StatefulSet)
  ├── Service: production-db-primary (writable endpoint)
  ├── Service: production-db-readonly (read replicas)
  ├── ConfigMap: production-db-config (MySQL configuration)
  ├── Secret: production-db-credentials (root password, replication user)
  ├── Secret: production-db-tls (TLS certificates)
  ├── CronJob: production-db-backup (automated backups)
  └── ServiceMonitor: production-db (Prometheus scraping)
```

### Database Selection

| Data Type | Storage | Justification |
|-----------|---------|---------------|
| CRDs and CRs | etcd (via API server) | Native k8s storage; watched by controller |
| Operator state | Implicit in CR status + owned resources | No external DB needed; state reconstructed from cluster |
| Backups | Object storage (S3, GCS, MinIO) | Durable, cheap, cross-region |
| Metrics | Prometheus | Standard k8s observability |

### Indexing Strategy

| Index | Purpose |
|-------|---------|
| `ownerReferences` on all created resources | Garbage collection; operator knows which resources it owns |
| `database.example.com/cluster-name` label | Cross-resource lookup (find all resources for a cluster) |
| Field indexer on `.spec.version` | Quickly find all clusters running a specific version |
| Cache indexer by namespace | Namespace-scoped reconciliation |

---

## 5. API Design

### REST/gRPC/kubectl Endpoints

**CRD automatically provides full REST API:**

```bash
# CRUD operations
kubectl apply -f mysqlcluster.yaml
kubectl get mysql -n production
kubectl get mysql production-db -n production -o yaml
kubectl describe mysql production-db -n production
kubectl delete mysql production-db -n production

# Scale (via scale subresource)
kubectl scale mysql production-db --replicas=5 -n production

# Watch
kubectl get mysql -n production -w

# Patch
kubectl patch mysql production-db -n production \
  --type merge -p '{"spec":{"version":"8.4"}}'

# Status (separate subresource — operator updates this)
# GET /apis/database.example.com/v1/namespaces/production/mysqlclusters/production-db/status
```

### CLI Design (Optional custom CLI)

```bash
# Custom kubectl plugin (kubectl-mysql)
kubectl mysql status production-db -n production
kubectl mysql backup create production-db -n production
kubectl mysql backup list production-db -n production
kubectl mysql restore production-db --from-backup=daily-2026-04-09 -n production
kubectl mysql failover production-db -n production   # Manual failover
kubectl mysql users list production-db -n production
kubectl mysql users create production-db --username=app --database=mydb -n production
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Reconciliation Loop Design

**Why it's hard:**
The reconciler must handle: (1) any possible cluster state (partially created, mid-upgrade, failed, being deleted), (2) concurrent changes from users and controllers, (3) external system failures (database crashes, network issues), (4) its own restarts (must be resumable), and (5) must be idempotent — running the same reconciliation twice must produce the same result. Getting this right is the core challenge of operator development.

**Approaches Compared:**

| Approach | Idempotency | Testability | Complexity | Recovery |
|----------|-----------|------------|-----------|---------|
| State machine (explicit phases) | Medium | High (each phase testable) | Medium | Good (resume from phase) |
| Declarative diff (desired vs. actual) | High | Medium | Low-Medium | Excellent (always converges) |
| Sequential steps with checkpoints | Low | High | High | Medium (checkpoint-dependent) |
| Event-driven (react to specific events) | Low | Medium | High | Poor (events can be missed) |

**Selected: Declarative diff (desired vs. actual) with status conditions**

**Justification:** Level-triggered reconciliation that compares desired state (CR spec) with actual state (owned resources) is the most resilient pattern. No events to miss, no checkpoints to corrupt, no state machine transitions to get wrong. The reconciler is a pure function: f(desired, actual) → actions.

**Implementation Detail (Go with controller-runtime):**

```go
// Reconciler implementation
type MySQLClusterReconciler struct {
    client.Client
    Scheme *runtime.Scheme
    Recorder record.EventRecorder
}

func (r *MySQLClusterReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
    log := log.FromContext(ctx)
    
    // 1. Fetch the CR
    cluster := &databasev1.MySQLCluster{}
    if err := r.Get(ctx, req.NamespacedName, cluster); err != nil {
        if apierrors.IsNotFound(err) {
            // CR deleted — nothing to do (finalizer handles cleanup)
            return ctrl.Result{}, nil
        }
        return ctrl.Result{}, err
    }
    
    // 2. Handle deletion (finalizer pattern)
    if !cluster.DeletionTimestamp.IsZero() {
        return r.reconcileDelete(ctx, cluster)
    }
    
    // 3. Ensure finalizer is set
    if !controllerutil.ContainsFinalizer(cluster, finalizerName) {
        controllerutil.AddFinalizer(cluster, finalizerName)
        if err := r.Update(ctx, cluster); err != nil {
            return ctrl.Result{}, err
        }
    }
    
    // 4. Reconcile each sub-resource (order matters)
    
    // 4a. ConfigMap (MySQL configuration)
    if err := r.reconcileConfigMap(ctx, cluster); err != nil {
        return r.setConditionAndRequeue(ctx, cluster, "ConfigReady", err)
    }
    
    // 4b. Secrets (credentials, TLS)
    if err := r.reconcileSecrets(ctx, cluster); err != nil {
        return r.setConditionAndRequeue(ctx, cluster, "SecretsReady", err)
    }
    
    // 4c. Services (headless, primary, readonly)
    if err := r.reconcileServices(ctx, cluster); err != nil {
        return r.setConditionAndRequeue(ctx, cluster, "ServicesReady", err)
    }
    
    // 4d. StatefulSet (the main workload)
    if err := r.reconcileStatefulSet(ctx, cluster); err != nil {
        return r.setConditionAndRequeue(ctx, cluster, "StatefulSetReady", err)
    }
    
    // 4e. MySQL-specific: configure replication
    if err := r.reconcileReplication(ctx, cluster); err != nil {
        return r.setConditionAndRequeue(ctx, cluster, "ReplicationReady", err)
    }
    
    // 4f. Backup CronJob
    if err := r.reconcileBackup(ctx, cluster); err != nil {
        return r.setConditionAndRequeue(ctx, cluster, "BackupReady", err)
    }
    
    // 4g. ServiceMonitor (Prometheus)
    if err := r.reconcileMonitoring(ctx, cluster); err != nil {
        return r.setConditionAndRequeue(ctx, cluster, "MonitoringReady", err)
    }
    
    // 5. Update overall status
    cluster.Status.Phase = "Running"
    cluster.Status.ReadyReplicas = r.countReadyReplicas(ctx, cluster)
    meta.SetStatusCondition(&cluster.Status.Conditions, metav1.Condition{
        Type:   "Ready",
        Status: metav1.ConditionTrue,
        Reason: "AllReplicasReady",
    })
    if err := r.Status().Update(ctx, cluster); err != nil {
        return ctrl.Result{}, err
    }
    
    // 6. Requeue after 60s for periodic health check
    return ctrl.Result{RequeueAfter: 60 * time.Second}, nil
}

// Sub-resource reconciliation (idempotent create-or-update)
func (r *MySQLClusterReconciler) reconcileStatefulSet(
    ctx context.Context, cluster *databasev1.MySQLCluster,
) error {
    desired := r.buildStatefulSet(cluster)  // Construct desired state from CR spec
    
    existing := &appsv1.StatefulSet{}
    err := r.Get(ctx, client.ObjectKeyFromObject(desired), existing)
    
    if apierrors.IsNotFound(err) {
        // Create
        controllerutil.SetControllerReference(cluster, desired, r.Scheme)
        return r.Create(ctx, desired)
    }
    if err != nil {
        return err
    }
    
    // Update if spec changed
    if !equality.Semantic.DeepEqual(existing.Spec, desired.Spec) {
        existing.Spec = desired.Spec
        return r.Update(ctx, existing)
    }
    
    return nil
}

// Setup controller with watches
func (r *MySQLClusterReconciler) SetupWithManager(mgr ctrl.Manager) error {
    return ctrl.NewControllerManagedBy(mgr).
        For(&databasev1.MySQLCluster{}).            // Watch CRs
        Owns(&appsv1.StatefulSet{}).                 // Watch owned StatefulSets
        Owns(&corev1.Service{}).                     // Watch owned Services
        Owns(&corev1.ConfigMap{}).                   // Watch owned ConfigMaps
        Owns(&batchv1.CronJob{}).                    // Watch owned CronJobs
        Watches(                                      // Watch external events
            &corev1.Pod{},
            handler.EnqueueRequestsFromMapFunc(r.podToCluster),
            builder.WithPredicates(predicate.ResourceVersionChangedPredicate{}),
        ).
        WithOptions(controller.Options{
            MaxConcurrentReconciles: 5,               // Process 5 CRs in parallel
        }).
        Complete(r)
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Reconcile returns error | Work queue requeues with exponential backoff | Log error; update condition; backoff prevents API storm |
| Operator crash mid-reconcile | Partial state (e.g., StatefulSet created but Service not) | Level-triggered: next reconcile detects and fixes |
| Stale cache | Operator reads outdated state; makes wrong decision | Optimistic concurrency (resourceVersion conflict → retry) |
| Rate-limited by API server | Reconciliations delayed | Work queue rate limiter; batch reads via informer cache |
| Infinite reconcile loop | CPU spike; API server overwhelmed | Detect loop (status unchanged check); max retry count |
| Webhook unavailable | CR creation/update blocked | `failurePolicy: Fail` ensures no invalid CRs; HA webhook deployment |

**Interviewer Q&As:**

**Q1: Why should controllers be level-triggered instead of edge-triggered?**
A: Level-triggered means the controller compares desired state vs. current state on every reconciliation, regardless of what event triggered it. Edge-triggered reacts to specific events ("a pod was deleted"). Level-triggered is superior because: (1) If an event is missed (network glitch, controller restart), the next reconciliation still detects and corrects the drift. (2) Multiple events between reconciliations are collapsed — you process the final state, not each intermediate state. (3) Idempotent by design — reconciling an already-correct state is a no-op.

**Q2: How do you handle the case where two reconciliations of the same CR run concurrently?**
A: controller-runtime's work queue deduplicates by key (namespace/name) — there is only one item per CR in the queue. However, the `MaxConcurrentReconciles` setting allows multiple different CRs to reconcile in parallel. For the same CR: if a reconciliation is in progress and a new event arrives, it's enqueued and processed after the current one completes. If you use multiple operator replicas, leader election ensures only one is active.

**Q3: When should you requeue vs. return an error?**
A: Return an error when the failure is unexpected and likely transient (API server unavailable, network timeout) — the work queue will requeue with exponential backoff. Use `RequeueAfter` when you're waiting for an expected condition (e.g., StatefulSet pods to become Ready — check again in 30s). Use `Requeue: true` (immediate requeue) when you made a change and need to verify the result right away. Return `Result{}` (no requeue) when the state is fully reconciled — the next watch event will trigger reconciliation.

**Q4: How do you test operator reconciliation logic?**
A: Three levels: (1) **Unit tests**: Mock the Kubernetes client; test reconciliation logic in isolation. controller-runtime provides a `fake.NewClientBuilder()` for this. (2) **Integration tests**: Use `envtest` (embedded etcd + API server) to test against a real API server without a full cluster. (3) **E2e tests**: Deploy operator to a real cluster (kind, k3d) and create actual CRs. Test all phases: create, update, scale, upgrade, delete. Use Chainsaw or Kuttl for declarative e2e testing.

**Q5: How do you handle operator upgrades that change the CRD schema?**
A: CRD versioning: (1) Add a new version (v1beta1 → v1) with the new schema. (2) Implement a conversion webhook that translates between versions. (3) Mark the new version as `storage: true`. (4) Serve both versions simultaneously during migration. (5) Once all CRs are converted, remove the old version. Alternative for simple changes: use the same version with backward-compatible changes (new optional fields).

**Q6: What is the difference between ownerReference and labels for tracking owned resources?**
A: `ownerReference` is the primary mechanism: (1) Enables garbage collection — when the owner is deleted, owned resources are automatically cleaned up. (2) The controller-runtime `Owns()` method uses it to trigger reconciliation when owned resources change. Labels are supplementary: (1) Useful for cross-namespace lookup (ownerReferences are namespace-scoped). (2) Useful for human readability (`kubectl get pods -l cluster-name=production-db`). Best practice: use both — ownerReference for GC and watch, labels for querying.

---

### Deep Dive 2: Webhooks (Admission Control for CRDs)

**Why it's hard:**
Webhooks are the enforcement mechanism for CRD schema validation beyond what OpenAPI can express. They must be highly available (if the webhook is down and `failurePolicy=Fail`, all CR operations are blocked), fast (API server waits for the webhook response), and correct (bugs in validation can corrupt cluster state).

**Approaches Compared:**

| Approach | Validation Power | Defaulting | Availability Risk | Complexity |
|----------|-----------------|-----------|-------------------|-----------|
| Operator webhook (controller-runtime) | Full (Go code) | Yes | Operator must be running | Medium |
| CRD validation rules (CEL, k8s 1.25+) | Good (CEL expressions) | No | None (in-API-server) | Low |
| OPA/Gatekeeper | Full (Rego) | No (needs mutation addon) | Gatekeeper must be running | Medium |
| OpenAPI schema only | Basic (types, enums, patterns) | No | None (in-API-server) | Low |

**Selected: CRD validation rules (CEL) for simple rules + operator webhook for complex validation and defaulting**

**Implementation Detail:**

```yaml
# CEL validation rules (no webhook needed, runs in API server)
apiVersion: apiextensions.k8s.io/v1
kind: CustomResourceDefinition
metadata:
  name: mysqlclusters.database.example.com
spec:
  versions:
    - name: v1
      schema:
        openAPIV3Schema:
          type: object
          properties:
            spec:
              type: object
              properties:
                replicas:
                  type: integer
                version:
                  type: string
              x-kubernetes-validations:
                - rule: "self.replicas % 2 == 1"
                  message: "Replicas must be an odd number for quorum"
                - rule: "self.replicas >= 1 && self.replicas <= 7"
                  message: "Replicas must be between 1 and 7"
              # Transition rules (compare old vs new)
              x-kubernetes-validations:
                - rule: "self.replicas >= oldSelf.replicas || self.replicas >= oldSelf.replicas - 2"
                  message: "Cannot scale down by more than 2 replicas at a time"
```

```go
// Webhook implementation (for complex validation + defaulting)
// Implements admission.CustomDefaulter and admission.CustomValidator

// Defaulting webhook (mutating)
func (w *MySQLClusterWebhook) Default(ctx context.Context, obj runtime.Object) error {
    cluster := obj.(*databasev1.MySQLCluster)
    
    // Set defaults
    if cluster.Spec.Storage.StorageClassName == "" {
        cluster.Spec.Storage.StorageClassName = "standard"
    }
    if cluster.Spec.Resources.CPU == "" {
        cluster.Spec.Resources.CPU = "2"
    }
    if cluster.Spec.Resources.Memory == "" {
        cluster.Spec.Resources.Memory = "4Gi"
    }
    if cluster.Spec.Backup.Retention == 0 {
        cluster.Spec.Backup.Retention = 7
    }
    
    // Inject sidecar configuration
    if cluster.Annotations == nil {
        cluster.Annotations = map[string]string{}
    }
    cluster.Annotations["database.example.com/config-hash"] = computeConfigHash(cluster)
    
    return nil
}

// Validation webhook (validating)
func (w *MySQLClusterWebhook) ValidateCreate(ctx context.Context, obj runtime.Object) (admission.Warnings, error) {
    cluster := obj.(*databasev1.MySQLCluster)
    
    var allErrs field.ErrorList
    
    // Cross-field validation (not possible with OpenAPI)
    if cluster.Spec.Replicas > 1 && cluster.Spec.Storage.StorageClassName == "local-path" {
        allErrs = append(allErrs, field.Invalid(
            field.NewPath("spec", "storage", "storageClassName"),
            cluster.Spec.Storage.StorageClassName,
            "Local storage not supported for multi-replica clusters (no cross-node access)",
        ))
    }
    
    // External validation (check if version is supported)
    if !isSupportedVersion(cluster.Spec.Version) {
        allErrs = append(allErrs, field.Invalid(
            field.NewPath("spec", "version"),
            cluster.Spec.Version,
            fmt.Sprintf("Unsupported MySQL version. Supported: %v", supportedVersions),
        ))
    }
    
    // Resource validation
    cpuRequest := resource.MustParse(cluster.Spec.Resources.CPU)
    if cpuRequest.Cmp(resource.MustParse("1")) < 0 {
        allErrs = append(allErrs, field.Invalid(
            field.NewPath("spec", "resources", "cpu"),
            cluster.Spec.Resources.CPU,
            "Minimum 1 CPU required for MySQL",
        ))
    }
    
    if len(allErrs) > 0 {
        return nil, apierrors.NewInvalid(
            schema.GroupKind{Group: "database.example.com", Kind: "MySQLCluster"},
            cluster.Name, allErrs)
    }
    
    return nil, nil
}

func (w *MySQLClusterWebhook) ValidateUpdate(ctx context.Context, oldObj, newObj runtime.Object) (admission.Warnings, error) {
    old := oldObj.(*databasev1.MySQLCluster)
    new := newObj.(*databasev1.MySQLCluster)
    
    var allErrs field.ErrorList
    
    // Immutable fields
    if old.Spec.Storage.StorageClassName != new.Spec.Storage.StorageClassName {
        allErrs = append(allErrs, field.Forbidden(
            field.NewPath("spec", "storage", "storageClassName"),
            "StorageClass cannot be changed after creation",
        ))
    }
    
    // Version downgrade check
    if semver.Compare(new.Spec.Version, old.Spec.Version) < 0 {
        allErrs = append(allErrs, field.Invalid(
            field.NewPath("spec", "version"),
            new.Spec.Version,
            "Version downgrade is not supported",
        ))
    }
    
    if len(allErrs) > 0 {
        return nil, apierrors.NewInvalid(
            schema.GroupKind{Group: "database.example.com", Kind: "MySQLCluster"},
            new.Name, allErrs)
    }
    
    return nil, nil
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Webhook server down (failurePolicy=Fail) | All CR create/update blocked | HA webhook deployment (2+ replicas); health checks |
| Webhook server down (failurePolicy=Ignore) | Invalid CRs admitted | Use CEL rules as baseline (always available) |
| Webhook timeout | API request delayed by timeout duration | Set `timeoutSeconds: 5`; optimize webhook logic |
| Webhook rejects valid CR | Users frustrated; operations blocked | Comprehensive testing; warn mode before enforce |
| TLS cert expired | Webhook unreachable | cert-manager auto-rotation |

**Interviewer Q&As:**

**Q1: How do you handle the chicken-and-egg problem where the operator webhook must be running before CRs can be created, but the operator itself might need CRs to be configured?**
A: (1) Separate the webhook and controller into independent deployments if needed. (2) Use `failurePolicy: Ignore` during initial bootstrap. (3) Use `objectSelector` or `namespaceSelector` to exclude the operator's own namespace. (4) CEL validation rules run in the API server — no dependency on the operator being up. (5) Best practice: start operator with minimal CRD validation (OpenAPI + CEL), then add webhook for advanced validation after the operator is fully running.

**Q2: When should you use CEL validation rules vs. a webhook?**
A: CEL validation rules (in-CRD) for: simple field validation, regex patterns, cross-field comparisons, immutable field enforcement, transition rules (old vs. new). They have zero availability risk since they run in the API server. Webhook for: external validation (checking against a database of supported versions), complex business logic, defaulting (CEL cannot mutate), validation that requires reading other resources. Rule of thumb: CEL for 80% of validation, webhook for the remaining 20%.

**Q3: How do you handle webhook certificate management?**
A: Three approaches: (1) **cert-manager** — creates and rotates webhook TLS certificates automatically. The Certificate resource references the webhook service. (2) **Self-signed with controller-runtime** — controller-runtime can generate self-signed certs and inject the CA bundle into the webhook configuration. (3) **Kubernetes CA API** — create CertificateSigningRequest, have it approved, use the signed cert. cert-manager is the production standard — it handles rotation before expiry.

**Q4: How do you version a webhook when adding new validation rules?**
A: (1) New validation rules only apply to new CRD version (v1beta1 → v1). Existing CRs remain valid under the old version. (2) Conversion webhook translates between versions. (3) For same-version changes: make rules additive (new rules → only new CRs must comply; existing CRs are grandfathered). (4) Use `warnings` return value to notify users of upcoming validation changes without blocking.

---

### Deep Dive 3: Finalizers and Cleanup

**Why it's hard:**
When a CR is deleted, the operator may need to perform cleanup: delete external resources (cloud databases, DNS records, backups), remove stale data, or notify external systems. If the operator simply watches for delete events, it might miss them (crash, restart). Finalizers ensure cleanup runs before the object is permanently removed, but they introduce complexity around stuck deletions and orphaned finalizers.

**Implementation Detail:**

```go
func (r *MySQLClusterReconciler) reconcileDelete(
    ctx context.Context, cluster *databasev1.MySQLCluster,
) (ctrl.Result, error) {
    log := log.FromContext(ctx)
    
    if !controllerutil.ContainsFinalizer(cluster, finalizerName) {
        return ctrl.Result{}, nil  // Nothing to do
    }
    
    log.Info("Running finalizer cleanup", "cluster", cluster.Name)
    
    // Step 1: Take a final backup (best effort)
    if err := r.takeFinalBackup(ctx, cluster); err != nil {
        log.Error(err, "Final backup failed — continuing with deletion")
        r.Recorder.Event(cluster, corev1.EventTypeWarning, "BackupFailed",
            "Final backup failed, proceeding with deletion")
    }
    
    // Step 2: Delete external resources (DNS records, monitoring)
    if err := r.cleanupExternalResources(ctx, cluster); err != nil {
        log.Error(err, "External cleanup failed — retrying")
        return ctrl.Result{RequeueAfter: 30 * time.Second}, nil
        // Don't remove finalizer — retry cleanup
    }
    
    // Step 3: Remove owned resources (optional — GC handles this via ownerReference)
    // But we might want to delete PVCs explicitly (StatefulSet does not delete PVCs)
    if err := r.deletePVCs(ctx, cluster); err != nil {
        log.Error(err, "PVC cleanup failed — retrying")
        return ctrl.Result{RequeueAfter: 10 * time.Second}, nil
    }
    
    // Step 4: Remove finalizer (allows k8s to complete deletion)
    controllerutil.RemoveFinalizer(cluster, finalizerName)
    if err := r.Update(ctx, cluster); err != nil {
        return ctrl.Result{}, err
    }
    
    log.Info("Finalizer cleanup complete", "cluster", cluster.Name)
    return ctrl.Result{}, nil
}
```

**Finalizer Lifecycle:**

```
1. CR created → reconciler adds finalizer to metadata.finalizers[]
2. CR in use → finalizer remains; deletion is blocked if attempted
3. User deletes CR:
   a. API server sets metadata.deletionTimestamp (but does NOT delete)
   b. Object is visible but immutable (spec changes rejected)
   c. Reconciler detects deletionTimestamp
   d. Reconciler runs cleanup logic
   e. Reconciler removes finalizer from list
   f. API server sees no finalizers → permanently deletes object

Stuck deletion (finalizer not removed):
  - CR stuck with deletionTimestamp forever
  - Cause: operator crash, bug, external resource unreachable
  - Recovery: fix the operator, or manually remove the finalizer:
    kubectl patch mysql production-db -p '{"metadata":{"finalizers":null}}' --type merge
  - Warning: manual removal skips cleanup — may leave orphaned resources
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Operator down during deletion | CR stuck with deletionTimestamp | Leader election; standby takes over |
| External cleanup fails repeatedly | CR stuck; cannot be fully deleted | Timeout with manual override; event logging; alert |
| Finalizer on deleted operator | CRs stuck forever (no controller to remove finalizer) | Before uninstalling operator, remove all CRs first; or clean up finalizers manually |
| Multiple finalizers conflict | Both must complete; ordering unclear | Each operator uses its own unique finalizer name; independent cleanup |

**Interviewer Q&As:**

**Q1: What happens if you forget to add a finalizer and the CR is deleted?**
A: The CR is immediately removed from etcd. The operator receives a delete event, but by the time it processes it, the object is gone — it cannot read the CR's spec to know what to clean up. Owned resources are cleaned up via garbage collection (ownerReference), but external resources (cloud databases, DNS records) are orphaned. This is why finalizers are critical for any operator that manages external resources.

**Q2: How do you handle the scenario where the finalizer cleanup takes hours?**
A: (1) Use `RequeueAfter` to periodically check if cleanup is complete (e.g., deleting a large database). (2) Update status conditions to reflect cleanup progress. (3) Set a maximum timeout — if cleanup exceeds the timeout, log a warning, emit an event, and remove the finalizer anyway (with an alert for manual cleanup). (4) The CR remains in Terminating state during this time — visible to users via `kubectl get`.

---

### Deep Dive 4: Status Conditions and Observability

**Why it's hard:**
Operators must communicate complex state (is the cluster healthy? is it upgrading? is the backup running?) through a standardized interface that is both machine-readable (for other controllers and automation) and human-readable (for operators and dashboards).

**Implementation Detail:**

```go
// Standard condition types for a database operator
const (
    ConditionReady            = "Ready"
    ConditionStatefulSetReady = "StatefulSetReady"
    ConditionReplicationReady = "ReplicationReady"
    ConditionBackupReady      = "BackupReady"
    ConditionConfigApplied    = "ConfigurationApplied"
    ConditionUpgrading        = "Upgrading"
)

// Condition update helper
func setCondition(conditions *[]metav1.Condition, condType string, status bool, reason, message string) {
    condStatus := metav1.ConditionFalse
    if status {
        condStatus = metav1.ConditionTrue
    }
    meta.SetStatusCondition(conditions, metav1.Condition{
        Type:               condType,
        Status:             condStatus,
        Reason:             reason,
        Message:            message,
        LastTransitionTime: metav1.Now(),
        ObservedGeneration: 0, // Set to CR's generation
    })
}

// Example conditions progression:
//
// Creating:
//   Ready=False, Reason=Provisioning, Message="Creating StatefulSet"
//   StatefulSetReady=False, Reason=WaitingForReplicas
//
// Running:
//   Ready=True, Reason=AllReplicasReady
//   StatefulSetReady=True
//   ReplicationReady=True
//   BackupReady=True
//
// Upgrading:
//   Ready=False, Reason=Upgrading, Message="Rolling upgrade: 2/3 completed"
//   Upgrading=True, Reason=InProgress
//
// Degraded:
//   Ready=False, Reason=ReplicaUnavailable, Message="Replica 2 is not ready"
//   StatefulSetReady=False, Reason=ReplicaNotReady
//   ReplicationReady=False, Reason=ReplicationLag, Message="Lag: 300s"
```

**Interviewer Q&As:**

**Q1: How do you design status conditions for maximum usefulness?**
A: Follow the Kubernetes API conventions: (1) Use positive polarity (Ready=True means healthy). (2) Include a Reason (CamelCase, machine-readable) and Message (human-readable). (3) Track `LastTransitionTime` (when the status last changed). (4) Use `ObservedGeneration` to detect if the status reflects the latest spec. (5) Have a top-level `Ready` condition that aggregates sub-conditions. (6) Sub-conditions for each major component (StatefulSetReady, ReplicationReady, BackupReady).

**Q2: How do you expose operator metrics to Prometheus?**
A: controller-runtime automatically exposes: `controller_runtime_reconcile_total` (count by result), `controller_runtime_reconcile_time_seconds` (histogram), `workqueue_depth`, `workqueue_adds_total`. Add custom metrics: `mysql_cluster_replicas{cluster, namespace}` (gauge), `mysql_cluster_replication_lag_seconds` (gauge), `mysql_backup_last_success_timestamp` (gauge). Register metrics in the reconciler and update them during each reconciliation. ServiceMonitor CRD tells Prometheus where to scrape.

---

## 7. Scheduling & Resource Management

### Operator Resource Consumption

```
Operator resource requests (for the operator pod itself):
  cpu: 100m (idle) - 500m (burst during reconciliation)
  memory: 128Mi (base) + ~50MB per 1000 watched objects

Sizing formula:
  Memory = 128Mi + (watched_objects * 5KB) + (concurrent_reconciles * 10MB)
  CPU = 100m + (reconciliations_per_second * 50m)

Example for 500 MySQLCluster CRs:
  Watched objects: 500 CRs + 5,000 owned resources = 5,500
  Memory: 128Mi + (5,500 * 5KB) + (5 * 10MB) = 128 + 27.5 + 50 ≈ 210 Mi
  CPU: 100m + (8/s * 50m) = 100m + 400m = 500m
  
  Set: requests: 256Mi, 500m | limits: 512Mi, 1000m
```

---

## 8. Scaling Strategy

### Operator Scaling Approaches

| Approach | Concurrency | Scalability | Complexity |
|----------|-----------|------------|-----------|
| Single instance with MaxConcurrentReconciles | 1-20 workers | ~1,000 CRs | Low |
| Namespace-scoped operator (one per namespace) | N instances | ~10,000 CRs total | Medium |
| Sharded operator (by label/hash) | N instances | Unlimited | High |
| Multi-cluster operator | One per cluster | Unlimited clusters | High |

### Interviewer Q&As

**Q1: How do you scale an operator that manages 10,000 CRs?**
A: (1) Increase `MaxConcurrentReconciles` (e.g., 20) — limits are API server QPS and operator CPU. (2) Optimize reconciliation: cache frequently needed data, batch API calls, skip no-op reconciliations. (3) Shard by namespace: deploy one operator instance per namespace (or group of namespaces), using `--namespace` flag. (4) Shard by label: partition CRs by label (e.g., `shard=0..9`), deploy 10 operator instances each watching their shard. (5) Reduce informer scope: only watch resources the operator needs (avoid cluster-wide watches if possible).

**Q2: How do you prevent an operator from overwhelming the API server?**
A: (1) Use informer cache for reads (never call `client.Get()` directly to API server — use `cache.Get()`). (2) Rate limit reconciliations via work queue settings: `BaseDelay: 5ms, MaxDelay: 1000s`. (3) Batch status updates (update once per reconciliation, not after each sub-step). (4) Use field selectors in watches to reduce event volume. (5) Implement exponential backoff for failed reconciliations. (6) Monitor `controller_runtime_reconcile_total{result="error"}` and `workqueue_retries_total`.

**Q3: How do you handle operator upgrades without disrupting managed resources?**
A: (1) Operator upgrade is a Deployment rolling update — new pod starts, old pod terminates. (2) Leader election ensures only one instance is active — brief gap during handover. (3) Managed resources (StatefulSets, Services) continue running unaffected. (4) New operator version must be backward-compatible with existing CRs. (5) If CRD schema changes, apply CRD update before operator upgrade (CRDs are cluster-scoped). (6) Test upgrade path: old operator → new operator on existing CRs.

**Q4: How do you handle the case where the operator needs to manage resources in a different cluster?**
A: (1) Multi-cluster operator: runs in a management cluster, creates a `client.Client` for each target cluster using kubeconfigs stored as Secrets. (2) Watches CRs in management cluster, reconciles by creating resources in target clusters. (3) Challenge: informer cache per target cluster consumes memory. (4) Alternative: deploy a lightweight agent in each target cluster that takes instructions from a central controller. (5) Example: ArgoCD application controller follows this pattern.

**Q5: What is the Operator Lifecycle Manager (OLM) and when would you use it?**
A: OLM manages the lifecycle of operators themselves: installation, upgrades, dependency resolution, and catalog management. It uses CRDs to model operators (ClusterServiceVersion, InstallPlan, Subscription, CatalogSource). Use when: (1) You're building an operator marketplace (internal or external). (2) You need automated operator upgrades with approval workflows. (3) You want dependency management (operator A requires operator B). (4) You're distributing operators to many clusters. Skip when: you have a small number of operators managed via GitOps (Flux/ArgoCD handles deployment well enough).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | Operator pod crash | Reconciliation stops for all CRs | Pod status CrashLoopBackOff | Deployment restarts; leader election transfers in < 15s | < 15s |
| 2 | Operator OOM | Same as crash | OOMKilled event | Increase memory limits; optimize cache usage | < 15s |
| 3 | API server unreachable | Operator cannot read/write objects | client-go logs errors | Informer reconnects automatically; in-memory cache serves reads | Auto-recovery |
| 4 | Managed resource deleted by user | Desired state drift | Reconciler detects missing resource on next loop | Reconciler recreates the resource | < reconcile interval |
| 5 | CRD accidentally deleted | All CRs vanish; operator watches break | Informer error logs | Reinstall CRD; CRs may need to be recreated from backup | Variable (data loss risk) |
| 6 | Webhook cert expired | CR creation/update blocked | TLS error in API server audit log | Rotate cert (cert-manager auto-rotation) | < 5 min |
| 7 | Reconcile loop bug (infinite requeue) | CPU spike; API server load | workqueue_depth increasing; reconcile_time increasing | Fix bug; deploy new version | Bug-fix turnaround |
| 8 | External dependency down | Operator cannot complete reconciliation | Condition shows degraded state | Requeue with backoff; external system recovery | External-dependent |

### Automated Recovery

| Mechanism | Implementation |
|-----------|---------------|
| Leader election | 2 replicas; standby promotes in < 15s |
| Informer resync | Full re-list after watch disconnection; periodic resync every 10h |
| Exponential backoff | Failed reconciliations retry: 5ms, 10ms, 20ms, ..., 1000s max |
| Optimistic concurrency | resourceVersion conflict → re-read and retry |
| Garbage collection | ownerReferences ensure orphaned resources are cleaned up |
| PodDisruptionBudget | Prevent both operator replicas from being evicted simultaneously |

---

## 10. Observability

### Key Metrics

| Metric | Source | Alert Threshold | Meaning |
|--------|--------|----------------|---------|
| `controller_runtime_reconcile_total{result="error"}` | controller-runtime | Rate > 10/min | Reconciliation failures |
| `controller_runtime_reconcile_time_seconds` | controller-runtime | p99 > 60s | Slow reconciliation |
| `workqueue_depth` | controller-runtime | > 100 sustained | Queue backup |
| `workqueue_retries_total` | controller-runtime | Rate > 50/min | Excessive retries |
| `controller_runtime_webhook_requests_total{code="500"}` | controller-runtime | Any | Webhook errors |
| Custom: `mysql_cluster_ready{cluster}` | Operator | == 0 for > 5 min | Cluster unhealthy |
| Custom: `mysql_backup_last_success_timestamp` | Operator | > 25h ago | Backup missed |
| Custom: `mysql_replication_lag_seconds` | Operator | > 60s | Replication lag |

### Distributed Tracing

```
Operator tracing:
1. Reconciliation → span per reconcile loop
2. Sub-operations → child spans (create StatefulSet, update config, etc.)
3. API server calls → child spans (with trace context propagation)
4. External calls → child spans (backup to S3, health check to MySQL)

Implementation: Use OpenTelemetry SDK in Go
  - controller-runtime supports tracing hooks
  - Export to Jaeger/Tempo
```

### Logging

```
Structured logging (logr interface in controller-runtime):

log.Info("Reconciling MySQLCluster",
    "cluster", cluster.Name,
    "namespace", cluster.Namespace,
    "generation", cluster.Generation,
    "phase", cluster.Status.Phase,
)

log.Error(err, "Failed to create StatefulSet",
    "cluster", cluster.Name,
    "statefulset", ssName,
)

Key log fields:
  - controller: controller name
  - reconcileID: unique per reconciliation
  - namespace/name: CR identifier
  - operation: create/update/delete
  - duration: operation duration
```

---

## 11. Security

### Auth & AuthZ

**Operator RBAC (principle of least privilege):**

```yaml
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRole
metadata:
  name: mysql-operator
rules:
# Own CRDs
- apiGroups: ["database.example.com"]
  resources: ["mysqlclusters", "mysqlclusters/status", "mysqlclusters/finalizers"]
  verbs: ["get", "list", "watch", "update", "patch"]
- apiGroups: ["database.example.com"]
  resources: ["mysqlbackups", "mysqlbackups/status"]
  verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]
# Managed resources
- apiGroups: ["apps"]
  resources: ["statefulsets"]
  verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]
- apiGroups: [""]
  resources: ["services", "configmaps", "secrets"]
  verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]
- apiGroups: [""]
  resources: ["pods"]
  verbs: ["get", "list", "watch"]  # Read-only for pods
- apiGroups: [""]
  resources: ["persistentvolumeclaims"]
  verbs: ["get", "list", "watch", "create", "delete"]
# Events
- apiGroups: [""]
  resources: ["events"]
  verbs: ["create", "patch"]
# Leader election
- apiGroups: ["coordination.k8s.io"]
  resources: ["leases"]
  verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]
```

### Multi-tenancy Isolation

| Concern | Mechanism |
|---------|-----------|
| Operator service account | Dedicated SA with scoped RBAC |
| Namespace isolation | Operator watches only assigned namespaces |
| Secret access | Operator only accesses secrets it creates (ownerReference) |
| Webhook scope | `namespaceSelector` limits which namespaces are intercepted |
| CRD access | Tenant RBAC controls who can create/modify CRs |

---

## 12. Incremental Rollout Strategy

### Phase 1: CRD + Basic Operator (Week 1-2)
- Design CRD schema (start with v1alpha1).
- Implement basic reconciler: create StatefulSet + Services.
- Unit tests and envtest integration tests.
- Deploy to dev cluster.

### Phase 2: Webhooks + Status (Week 3-4)
- Implement defaulting and validation webhooks.
- Add CEL validation rules for simple constraints.
- Implement comprehensive status conditions.
- Add Prometheus metrics.

### Phase 3: Day-2 Operations (Week 5-6)
- Implement rolling upgrades.
- Implement backup/restore.
- Implement scaling (scale subresource).
- E2e tests with Chainsaw.

### Phase 4: Production Hardening (Week 7-8)
- Leader election with 2 replicas.
- Finalizers for cleanup.
- PodDisruptionBudget.
- Comprehensive error handling and retry logic.
- Load testing (500 CRs).

### Phase 5: GA Release (Week 9-10)
- Promote CRD to v1beta1 or v1.
- Conversion webhook if schema changed.
- OLM packaging (optional).
- Documentation and runbooks.

### Rollout Interviewer Q&As

**Q1: How do you safely upgrade a CRD without losing data?**
A: (1) Add new fields as optional (backward compatible). (2) Never remove fields from the stored version. (3) If schema change is breaking, add a new version (v1beta1 → v1) with a conversion webhook. (4) Mark new version as `storage: true`. (5) Migrate existing CRs: `kubectl get mysqlclusters -o json | ... | kubectl apply -f -` (triggers conversion). (6) Once all CRs are on new version, deprecate old version.

**Q2: How do you handle the case where the operator's RBAC is insufficient?**
A: (1) The operator will log "forbidden" errors during reconciliation. (2) Reconciliation fails; CRs get stuck. (3) Detection: `controller_runtime_reconcile_total{result="error"}` spike, audit logs show 403 errors. (4) Fix: update the ClusterRole with missing permissions and restart the operator. (5) Prevention: test RBAC in staging; use dry-run mode; document required permissions.

**Q3: How do you roll back a failed operator upgrade?**
A: (1) Deployment rollback: `kubectl rollout undo deployment mysql-operator`. (2) If CRD was updated: roll back CRD only if backward-compatible (adding fields is safe; removing fields is not). (3) If conversion webhook was added: keep old webhook version running. (4) Best practice: use Helm or GitOps for operator deployment — rollback is `git revert` or `helm rollback`.

**Q4: How do you test an operator against multiple Kubernetes versions?**
A: (1) CI matrix: test against k8s 1.28, 1.29, 1.30 using kind clusters with specific versions. (2) envtest supports specifying k8s API server version. (3) Test CRD feature gates: CEL validation (1.25+), admission webhooks, status subresource. (4) Test deprecation: ensure no deprecated API usage. (5) Use `kubeconform` to validate manifests against specific k8s versions.

**Q5: How do you distribute an operator to 500 clusters?**
A: (1) Package as Helm chart or OLM bundle. (2) Store in a centralized chart repository (Harbor, ChartMuseum). (3) Deploy via GitOps: Flux HelmRelease or ArgoCD Application per cluster. (4) Fleet management: Rancher Fleet, ArgoCD ApplicationSet, or Flux Kustomization with cluster selectors. (5) Version pinning: pin operator version per environment (dev gets latest, prod gets stable). (6) Monitoring: aggregate operator metrics from all clusters into a central Prometheus/Thanos.

---

## 13. Trade-offs & Decision Log

| # | Decision | Alternative Considered | Trade-off | Rationale |
|---|----------|----------------------|-----------|-----------|
| 1 | Go with controller-runtime | Python (kopf), Java (JOSDK) | Less accessible to non-Go teams but best ecosystem support | controller-runtime is the standard; best performance; largest community |
| 2 | Level-triggered reconciliation | Event-driven (edge-triggered) | May do unnecessary work but never misses state drift | Resilient to missed events; idempotent by design |
| 3 | CEL + webhook validation | Webhook-only | CEL is less powerful but always available | CEL covers 80% of validation with zero availability risk |
| 4 | Finalizers for cleanup | Pre-delete hooks (not native to k8s) | Adds deletion complexity but guarantees cleanup | Only reliable mechanism for external resource cleanup |
| 5 | Status conditions (not custom status fields) | Custom status structure | Less flexible but standardized | Standard tooling (kubectl, dashboards) understands conditions |
| 6 | Leader election (2 replicas) | Single replica | Extra resource cost but HA | < 15s recovery vs. Deployment restart time |
| 7 | ownerReferences for GC | Manual cleanup | Less control over deletion order but automatic | Prevents resource leaks; standard k8s mechanism |
| 8 | Informer cache | Direct API calls | Memory cost but dramatically reduces API server load | Mandatory for any operator managing > 10 resources |
| 9 | CRD version evolution | Single version forever | Schema migration complexity but enables breaking changes | Required for production operators with evolving APIs |

---

## 14. Agentic AI Integration

### AI-Powered Operator Operations

| Use Case | AI Agent Capability | Implementation |
|----------|-------------------|---------------|
| **Auto-tuning** | Analyze managed system metrics and adjust CR spec | Agent monitors MySQL query latency, buffer pool hit rate, replication lag; recommends config changes via CR patch |
| **Predictive failure detection** | Predict managed system failures before they occur | Agent analyzes disk usage growth rate, error log patterns, replication lag trends; creates alerts before SLO breach |
| **Automated incident response** | Diagnose and fix common operational issues | Agent receives alert → reads CR status conditions → runs diagnostic queries → applies remediation (e.g., trigger failover, increase resources) |
| **Operator code generation** | Generate operator scaffolding from CRD spec | Agent reads CRD schema → generates reconciler code, webhook handlers, tests, RBAC |
| **Configuration drift detection** | Compare managed resources against desired state | Agent periodically checks if StatefulSets, ConfigMaps, etc. match what the operator should have created; flags manual modifications |

### Example: AI-Driven Database Operator Tuning

```python
class MySQLTuningAgent:
    """
    Monitors MySQL performance metrics and recommends operator configuration changes.
    """
    
    def analyze_and_recommend(self, cluster_name: str) -> list:
        recommendations = []
        
        # Query Prometheus for MySQL metrics
        buffer_pool_hit_rate = self.prometheus.query(
            f'mysql_global_status_innodb_buffer_pool_read_requests'
            f'/ (mysql_global_status_innodb_buffer_pool_read_requests'
            f'+ mysql_global_status_innodb_buffer_pool_reads)'
            f'{{cluster="{cluster_name}"}}'
        )
        
        if buffer_pool_hit_rate < 0.99:
            current_memory = self.get_cr_spec(cluster_name, 'resources.memory')
            recommended = int(current_memory.rstrip('Gi')) * 1.5
            recommendations.append(Recommendation(
                type='PERFORMANCE',
                field='spec.resources.memory',
                current=current_memory,
                recommended=f'{int(recommended)}Gi',
                reason=f'Buffer pool hit rate is {buffer_pool_hit_rate:.2%} (target: 99%+). '
                       f'Increasing memory will improve hit rate.',
                impact='Reduces disk reads; improves query latency',
                risk='LOW'
            ))
        
        # Check replication lag
        replication_lag = self.prometheus.query(
            f'mysql_slave_status_seconds_behind_master{{cluster="{cluster_name}"}}'
        )
        
        if replication_lag > 30:
            recommendations.append(Recommendation(
                type='RELIABILITY',
                field='spec.resources.cpu',
                reason=f'Replication lag is {replication_lag}s. '
                       f'Replica may not keep up with write load.',
                action='Increase replica CPU or consider read-write splitting',
                risk='MEDIUM'
            ))
        
        return recommendations
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: What is the operator pattern and how does it differ from a Helm chart?**
A: The operator pattern = CRD + custom controller. It encodes operational knowledge into software — the controller continuously reconciles desired state (CR) with actual state (managed resources). Helm is a one-shot templating and deployment tool — it renders templates and applies them, but doesn't continuously monitor or manage. Key differences: (1) Operator handles day-2 operations (upgrades, backup, failover) automatically. (2) Operator reacts to changes (pod crash → recreate). (3) Operator can manage external resources. (4) Helm is simpler for stateless applications; operators are needed for stateful systems.

**Q2: Walk through the complete lifecycle of a custom resource from creation to deletion.**
A: (1) User applies CR YAML. (2) API server runs mutating admission webhook (operator sets defaults). (3) API server validates schema (OpenAPI + CEL). (4) API server runs validating admission webhook (operator validates business rules). (5) CR stored in etcd. (6) Informer watch event triggers reconciler. (7) Reconciler adds finalizer. (8) Reconciler creates owned resources (StatefulSet, Services, etc.) with ownerReferences. (9) Reconciler monitors health; updates status conditions. (10) User updates CR spec → watch event → reconciler detects diff → applies changes. (11) User deletes CR → API server sets deletionTimestamp → reconciler runs cleanup → removes finalizer → API server deletes CR → garbage collector deletes owned resources.

**Q3: How does the informer pattern work and why is it critical for operator performance?**
A: The informer does a LIST (full download) on startup, then switches to WATCH (incremental events). It stores all objects in an in-memory cache (thread-safe store). Benefits: (1) Reads are O(1) from cache — no API server round-trip. (2) SharedInformerFactory ensures one watch per resource type (even if multiple controllers watch the same type). (3) Event handlers are called when objects change, allowing efficient work queue enqueuing. Without informers, an operator managing 500 CRs with 5,000 owned resources would need 5,500 GET calls per reconciliation cycle — hundreds of QPS to the API server.

**Q4: How do you implement rolling upgrades in an operator?**
A: The operator updates the StatefulSet's pod template with the new version. StatefulSet controller handles rolling update (one pod at a time, waiting for Ready between each). The operator adds orchestration: (1) Update ConfigMap with new configuration. (2) Perform pre-upgrade checks (backup, health). (3) Update StatefulSet spec.template. (4) Monitor pod-by-pod rollout via status.updatedReplicas. (5) After each pod upgrade, verify application-level health (e.g., MySQL replication caught up). (6) If a pod fails to become healthy, pause the rollout and set condition Upgrading=False, reason=UpgradeFailed. (7) On success, update CR status.currentVersion.

**Q5: What are the most common mistakes in operator development?**
A: (1) **Not being idempotent**: creating duplicate resources because the reconciler doesn't check if the resource already exists. (2) **Not using informer cache**: calling the API server directly for reads, causing excessive load. (3) **Not handling finalizers**: external resources leaked on deletion. (4) **Over-reconciling**: requeuing on every event without checking if actual state already matches desired. (5) **Not setting ownerReferences**: orphaned resources after CR deletion. (6) **Updating status in the main reconcile path**: status update triggers a new event → reconciliation loop → infinite loop. Solution: use status subresource and check if status actually changed before updating.

**Q6: How do you handle the case where the operator needs to execute commands inside a managed pod?**
A: (1) Use the Kubernetes exec API (equivalent to `kubectl exec`): create a `remotecommand.Executor` and execute commands inside the pod. (2) Use cases: run MySQL backup command, check replication status, run health checks. (3) Alternative: use a sidecar container that exposes an HTTP health API — cleaner than exec. (4) Important: exec requires RBAC permission on `pods/exec` — limit this carefully. (5) Timeout all exec calls (network issues can cause hangs).

**Q7: How do you handle CRD conflicts between operators?**
A: (1) CRDs are cluster-scoped — two operators cannot use the same CRD group/kind. (2) Prevention: use organization-specific API groups (e.g., `database.company.com` not just `database`). (3) If two versions of the same operator are deployed: CRD is the same, but controllers may conflict. (4) Use OLM to manage operator dependencies and prevent conflicts. (5) For shared CRDs: one operator owns the CRD, others consume it read-only.

**Q8: How do you implement a backup/restore operator?**
A: Design: (1) Separate CRDs: MySQLBackup (triggers a backup), MySQLRestore (triggers a restore). (2) MySQLBackup reconciler: creates a Job that runs the backup tool (mysqldump, xtrabackup), uploads to S3, updates MySQLBackup status with location and timestamp. (3) MySQLRestore reconciler: creates a Job that downloads backup from S3, applies to target cluster, updates status. (4) Schedule: the main MySQLCluster operator creates a CronJob that creates MySQLBackup CRs on schedule. (5) Retention: reconciler deletes old MySQLBackup CRs (and their S3 objects) based on retention policy.

**Q9: What is the controller-runtime Manager and what does it provide?**
A: The Manager is the central coordinator for the operator: (1) Starts all controllers (registers reconcilers). (2) Manages the informer cache (shared across controllers). (3) Runs the webhook server (HTTPS). (4) Handles leader election. (5) Provides health/readiness endpoints. (6) Manages graceful shutdown. (7) Creates the Kubernetes client (with cache-backed reads). You configure one Manager in main.go, register all controllers, and call `mgr.Start()`.

**Q10: How do you handle the scenario where the operator's CRD schema needs to change in a backward-incompatible way?**
A: (1) Create a new CRD version (e.g., v1alpha1 → v1beta1) with the new schema. (2) Implement a conversion webhook that translates between old and new versions. (3) Both versions are served simultaneously — existing clients using old version still work. (4) Set new version as `storage: true` — new CRs stored in new format. (5) Migrate existing CRs: list all CRs (triggers conversion), or use a migration job that reads and rewrites each CR. (6) Once all CRs and clients are on the new version, deprecate and eventually remove the old version. (7) Example: renaming a field from `size` to `storageSize` — conversion webhook maps between them.

**Q11: How do you implement a multi-cluster operator?**
A: Architecture: (1) Operator runs in a management cluster. (2) CRs in management cluster define desired state for resources in target clusters. (3) Operator creates a client.Client for each target cluster using kubeconfigs stored as Secrets. (4) Reconciler reads CR from management cluster cache, reconciles in target cluster. (5) Reports status back to CR in management cluster. (6) Challenge: informer cache per target cluster. Mitigation: use controller-runtime's cluster-aware manager or lazy client initialization.

**Q12: How do you test an operator's error handling paths?**
A: (1) **Fault injection in unit tests**: mock API client to return errors for specific calls. (2) **envtest with webhook failures**: configure webhooks to fail intermittently. (3) **Chaos testing**: use Chaos Mesh or Litmus to inject pod failures, network partitions, API server delays. (4) **Specific scenarios**: delete owned resources while operator is running (should recreate); delete CRD (should not crash operator); apply invalid CR (webhook should reject). (5) **Fuzz testing**: apply random CR mutations and verify the operator doesn't crash.

**Q13: What is the difference between a controller and an operator?**
A: Every operator is a controller, but not every controller is an operator. A controller reconciles any Kubernetes resource — the built-in ReplicaSet controller is a controller but not an operator. An operator specifically: (1) Manages a complex application's lifecycle. (2) Uses CRDs to model the application's desired state. (3) Encodes operational expertise (backup, upgrade, failover) that would otherwise require a human operator. The term "operator" was coined by CoreOS to describe this pattern for managing stateful applications like etcd and Prometheus.

**Q14: How do you handle rate limiting in an operator that manages external APIs?**
A: (1) Use a rate-limited work queue with appropriate settings (higher delays for external API calls). (2) Implement a semaphore or token bucket for external API calls within the reconciler. (3) Cache external API responses where possible (with TTL). (4) Use `RequeueAfter` with appropriate delay when rate-limited (respect Retry-After headers). (5) Track external API call metrics: `external_api_calls_total`, `external_api_latency_seconds`, `external_api_rate_limited_total`.

**Q15: How do you ensure an operator is production-ready?**
A: Production readiness checklist: (1) Leader election enabled. (2) RBAC follows least privilege. (3) Finalizers for all external resources. (4) Status conditions for all significant states. (5) Prometheus metrics exposed. (6) Structured logging with consistent fields. (7) Health and readiness endpoints. (8) PodDisruptionBudget. (9) Resource requests and limits set. (10) Unit tests > 80% coverage. (11) Integration tests with envtest. (12) E2e tests in CI. (13) Graceful shutdown handling. (14) Documentation: CRD reference, operational runbook. (15) Chaos testing for failure scenarios.

---

## 16. References

| # | Reference | URL |
|---|-----------|-----|
| 1 | Kubernetes Operator Pattern | https://kubernetes.io/docs/concepts/extend-kubernetes/operator/ |
| 2 | controller-runtime | https://github.com/kubernetes-sigs/controller-runtime |
| 3 | Kubebuilder Book | https://book.kubebuilder.io/ |
| 4 | Operator SDK | https://sdk.operatorframework.io/ |
| 5 | Operator Lifecycle Manager | https://olm.operatorframework.io/ |
| 6 | CRD Validation with CEL | https://kubernetes.io/docs/tasks/extend-kubernetes/custom-resources/custom-resource-definitions/#validation-rules |
| 7 | API Conventions | https://github.com/kubernetes/community/blob/master/contributors/devel/sig-architecture/api-conventions.md |
| 8 | kopf (Python Operator Framework) | https://kopf.readthedocs.io/ |
| 9 | Java Operator SDK | https://javaoperatorsdk.io/ |
| 10 | Chainsaw (Operator Testing) | https://kyverno.github.io/chainsaw/ |
| 11 | OperatorHub.io | https://operatorhub.io/ |
