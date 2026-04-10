# System Design: Cluster Resource Manager

> **Relevance to role:** A cloud infrastructure platform engineer must design and operate cluster-level resource management systems that arbitrate between competing workloads, enforce fairness, handle preemption, and scale across tens of thousands of nodes. This is the brain of any IaaS/PaaS platform — analogous to YARN in Hadoop, Mesos in two-level scheduling architectures, and kube-scheduler + controller-manager in Kubernetes. Deep understanding of DRF, gang scheduling, multi-cluster federation, and autoscaling is essential for this role.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Accept job/workload submissions with resource requests (CPU, RAM, GPU, disk, network) and scheduling constraints. |
| FR-2 | Maintain a global view of cluster resources across all nodes. |
| FR-3 | Schedule workloads using configurable policies: fair-share (DRF), priority-based, FIFO, capacity scheduling. |
| FR-4 | Support gang scheduling: all-or-nothing allocation for distributed ML training jobs (e.g., all 64 workers must be placed or none). |
| FR-5 | Implement priority-based preemption: higher-priority workloads can evict lower-priority ones. |
| FR-6 | Handle node pressure eviction: automatically evict pods when memory, disk, or PID pressure exceeds thresholds. |
| FR-7 | Support cluster autoscaling: scale out when pending workloads exceed capacity, scale in when utilization drops. |
| FR-8 | Multi-cluster federation: route workloads to the most appropriate cluster across regions. |
| FR-9 | Provide queue management with hierarchical queues (org → team → project). |
| FR-10 | Expose scheduling pipeline as extensible plugin framework (custom filters, scorers, binders). |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Scheduling throughput | 10,000 pods/sec |
| NFR-2 | Scheduling latency (p99) | < 500 ms per pod |
| NFR-3 | Gang scheduling latency (64-pod group) | < 5 s |
| NFR-4 | Cluster state convergence | < 10 s after node join/leave |
| NFR-5 | Availability | 99.99% (52 min downtime/year) |
| NFR-6 | Node scale per cluster | 10,000 nodes |
| NFR-7 | Total nodes (federated) | 100,000 across 10 clusters |
| NFR-8 | Fairness deviation | < 5% from DRF ideal allocation |

### Constraints & Assumptions

- Heterogeneous clusters: mix of CPU-only, GPU (A100/H100), high-memory, ARM, and bare-metal nodes.
- Workload types: long-running services (months), batch jobs (minutes to hours), ML training (hours to days), cron jobs.
- Network: 25 Gbps node-to-node within a cluster, 10 Gbps cross-cluster.
- Existing infrastructure: Kubernetes as the container orchestration layer, OpenStack for VM management, custom bare-metal provisioner.
- The resource manager sits above these orchestrators, providing a unified scheduling layer.

### Out of Scope

- Container runtime implementation (containerd, CRI-O).
- Network policy and service mesh (Istio, Cilium).
- Application-level load balancing.
- Cost optimization and billing (separate system).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Total nodes (single cluster) | 10,000 | Given |
| Total clusters | 10 | 5 regions x 2 AZs primary |
| Total federated nodes | 100,000 | 10,000 x 10 |
| Workloads per node (avg) | 30 | Assuming container density |
| Total active workloads (single cluster) | 300,000 | 10,000 x 30 |
| Total active workloads (federated) | 3,000,000 | 300,000 x 10 |
| New scheduling decisions/day (single cluster) | 500,000 | ~1.67 churn rate |
| Peak scheduling rate | 10,000 pods/sec | Burst during mass deployment |
| Node heartbeats per second (single cluster) | 1,000 | 10,000 nodes / 10 sec interval |
| Gang scheduling jobs per hour | 50 | ML training job submissions |

### Latency Requirements

| Operation | p50 | p99 | p999 |
|-----------|-----|-----|------|
| Single pod schedule | 50 ms | 300 ms | 1 s |
| Gang schedule (64 pods) | 500 ms | 3 s | 10 s |
| Node registration | 2 s | 5 s | 15 s |
| Cluster autoscale (scale-out decision) | 30 s | 60 s | 120 s |
| Actual node provisioning (bare-metal) | 5 min | 15 min | 30 min |
| Actual node provisioning (cloud VM) | 30 s | 90 s | 5 min |
| Preemption execution | 1 s | 5 s | 30 s |
| Federation routing decision | 100 ms | 500 ms | 2 s |

### Storage Estimates

| Data | Size per record | Count | Total |
|------|-----------------|-------|-------|
| Node state | 8 KB | 10,000 | 80 MB |
| Pod/workload state | 2 KB | 300,000 | 600 MB |
| Queue state | 1 KB | 10,000 pending | 10 MB |
| Scheduling history (30 days) | 0.5 KB | 15,000,000 | 7.5 GB |
| Cluster config + policies | 100 KB | 10 clusters | 1 MB |
| **Active state (single cluster)** | | | **~700 MB** |
| **Total with history** | | | **~8 GB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Node heartbeats inbound | 1,000/sec x 8 KB | 8 MB/s |
| Pod status updates | 5,000/sec x 1 KB | 5 MB/s |
| Scheduling decisions outbound | 10,000/sec x 2 KB | 20 MB/s |
| Cross-cluster federation sync | 10 clusters x 80 MB / 30 sec | 27 MB/s |
| **Total** | | **~60 MB/s** |

---

## 3. High Level Architecture

```
                    ┌─────────────────────────────────────────────────┐
                    │              Federation Layer                    │
                    │  ┌──────────┐  ┌──────────┐  ┌──────────┐     │
                    │  │ Cluster 1│  │ Cluster 2│  │ Cluster N│     │
                    │  │  Manager │  │  Manager │  │  Manager │     │
                    │  └────┬─────┘  └────┬─────┘  └────┬─────┘     │
                    │       └──────┬──────┘──────┬──────┘            │
                    │              │   Federation│Controller          │
                    │              │  (routing + │placement)          │
                    │              └──────┬──────┘                    │
                    └─────────────────────┼──────────────────────────┘
                                          │
         ┌────────────────────────────────┼──────────────────────────────┐
         │                    SINGLE CLUSTER                             │
         │                                                               │
         │  ┌──────────────────┐    ┌──────────────────┐                │
         │  │   API Server     │    │   Queue Manager   │                │
         │  │ (submit, cancel, │◄──►│  (hierarchical    │                │
         │  │  status, admin)  │    │   fair-share DRF) │                │
         │  └────────┬─────────┘    └────────┬──────────┘                │
         │           │                       │                           │
         │  ┌────────▼───────────────────────▼──────────┐               │
         │  │            Scheduling Engine                │               │
         │  │  ┌──────────┐ ┌──────────┐ ┌────────────┐ │               │
         │  │  │ Predicate│ │ Priority │ │   Bind     │ │               │
         │  │  │ Filters  │→│ Scorers  │→│  (commit)  │ │               │
         │  │  └──────────┘ └──────────┘ └────────────┘ │               │
         │  │  ┌──────────────────────────────────────┐  │               │
         │  │  │  Gang Scheduler (coscheduling plugin) │  │               │
         │  │  └──────────────────────────────────────┘  │               │
         │  └────────────────────┬────────────────────────┘               │
         │                       │                                        │
         │  ┌────────────────────▼────────────────────────┐              │
         │  │              Cluster State Store              │              │
         │  │  (etcd for k8s objects, MySQL for extended   │              │
         │  │   metadata, Redis for real-time metrics)     │              │
         │  └────────────────────┬─────────────────────────┘              │
         │                       │                                        │
         │  ┌────────────────────▼────────────────────────┐              │
         │  │         Node Management Layer                │              │
         │  │  ┌───────────┐ ┌────────────┐ ┌──────────┐ │              │
         │  │  │  Node     │ │ Eviction   │ │ Autoscale│ │              │
         │  │  │  Monitor  │ │ Manager    │ │ Controller│ │              │
         │  │  └───────────┘ └────────────┘ └──────────┘ │              │
         │  └────────────────────┬─────────────────────────┘              │
         │                       │                                        │
         │       ┌───────┬───────┼───────┬───────┐                       │
         │  ┌────▼──┐┌───▼──┐┌───▼──┐┌───▼──┐┌───▼──┐                  │
         │  │Node 1 ││Node 2││Node 3││ ...  ││Node N│                  │
         │  │Agent  ││Agent ││Agent ││      ││Agent │                  │
         │  └───────┘└──────┘└──────┘└──────┘└──────┘                  │
         └───────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Federation Layer** | Routes workloads to the best cluster based on capacity, locality, and policy. Maintains aggregate capacity view across all clusters. |
| **API Server** | Receives job submissions, cancellations, status queries. Validates requests, enforces quotas (via admission webhooks), and persists to state store. |
| **Queue Manager** | Manages hierarchical scheduling queues. Implements DRF for fair-share allocation, priority ordering, and queue capacity limits. |
| **Scheduling Engine** | The core scheduler. Runs the filter → score → bind pipeline. Contains plugin framework for custom scheduling logic. |
| **Gang Scheduler** | Co-scheduling plugin. Holds pods in a "waiting" state until all members of a gang can be placed simultaneously. Uses PodGroup CRD. |
| **Cluster State Store** | etcd for Kubernetes API objects, MySQL for extended metadata (scheduling history, queue definitions, autoscaler config), Redis for real-time node utilization metrics. |
| **Node Monitor** | Watches node heartbeats, detects node failures (no heartbeat for > 40s), marks nodes as NotReady. |
| **Eviction Manager** | Monitors node pressure signals (memory, disk, PID). Evicts pods when pressure exceeds thresholds, respecting PodDisruptionBudgets. |
| **Autoscale Controller** | Monitors pending pod queue. Triggers scale-out when pods are unschedulable. Triggers scale-in when node utilization < 50% for > 10 minutes. |
| **Node Agents** | Kubelet (for k8s), nova-compute (for OpenStack), or custom agent (for bare metal). Reports resource capacity, utilization, and health. |

### Data Flows

**Primary — Job Submission:**
1. User submits workload via API.
2. API Server validates, checks quotas (admission webhook), persists to etcd.
3. Queue Manager picks up the workload, assigns to appropriate queue, orders by DRF share + priority.
4. Scheduling Engine dequeues workload, runs filter-score-bind pipeline.
5. Bind result is written to etcd (pod is assigned to a node).
6. Kubelet on the target node picks up the binding and starts the workload.

**Secondary — Gang Scheduling:**
1. All pods in a PodGroup enter the gang scheduler's waiting pool.
2. When all members are present (or quorum), the gang scheduler attempts simultaneous placement.
3. If successful, all pods are bound atomically. If not, all remain pending and are retried.

**Tertiary — Autoscaling:**
1. Pending pods accumulate (unschedulable for > 30 seconds).
2. Autoscale controller calculates how many new nodes are needed (by simulating scheduling).
3. Triggers node provisioning (cloud API or bare-metal provisioner).
4. New node registers, existing pending pods are rescheduled.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Hierarchical scheduling queues
CREATE TABLE scheduling_queues (
    queue_id         CHAR(36) PRIMARY KEY,
    queue_name       VARCHAR(255) NOT NULL UNIQUE,
    parent_queue_id  CHAR(36),                     -- NULL for root queues
    -- Capacity allocation
    guaranteed_cpu   DECIMAL(12,2) NOT NULL,       -- guaranteed CPU cores for this queue
    guaranteed_ram_mb BIGINT NOT NULL,
    guaranteed_gpu   INT NOT NULL DEFAULT 0,
    max_cpu          DECIMAL(12,2),                -- elastic max (can burst to this)
    max_ram_mb       BIGINT,
    max_gpu          INT,
    -- Current usage (real-time aggregated)
    used_cpu         DECIMAL(12,2) NOT NULL DEFAULT 0,
    used_ram_mb      BIGINT NOT NULL DEFAULT 0,
    used_gpu         INT NOT NULL DEFAULT 0,
    -- Policy
    scheduling_policy ENUM('fair_share','fifo','priority','capacity') NOT NULL DEFAULT 'fair_share',
    preemption_enabled BOOLEAN NOT NULL DEFAULT TRUE,
    max_running_jobs INT,                          -- NULL = unlimited
    -- State
    state            ENUM('open','closed','draining') NOT NULL DEFAULT 'open',
    -- Ownership
    owner_org_id     CHAR(36),
    owner_team_id    CHAR(36),
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (parent_queue_id) REFERENCES scheduling_queues(queue_id),
    INDEX idx_queues_parent (parent_queue_id),
    INDEX idx_queues_org (owner_org_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Nodes (cluster members)
CREATE TABLE cluster_nodes (
    node_id          CHAR(36) PRIMARY KEY,
    cluster_id       CHAR(36) NOT NULL,
    hostname         VARCHAR(255) NOT NULL,
    node_type        ENUM('cpu_general','gpu_a100','gpu_h100','high_mem','storage','arm','bare_metal','fpga') NOT NULL,
    region           VARCHAR(32) NOT NULL,
    availability_zone VARCHAR(32) NOT NULL,
    rack_id          VARCHAR(64),
    -- Capacity
    total_cpu        DECIMAL(10,2) NOT NULL,
    total_ram_mb     BIGINT NOT NULL,
    total_gpu        INT NOT NULL DEFAULT 0,
    total_disk_gb    BIGINT NOT NULL,
    -- Allocatable (total - system reserved)
    alloc_cpu        DECIMAL(10,2) NOT NULL,
    alloc_ram_mb     BIGINT NOT NULL,
    alloc_gpu        INT NOT NULL DEFAULT 0,
    alloc_disk_gb    BIGINT NOT NULL,
    -- Current committed (sum of pod requests)
    committed_cpu    DECIMAL(10,2) NOT NULL DEFAULT 0,
    committed_ram_mb BIGINT NOT NULL DEFAULT 0,
    committed_gpu    INT NOT NULL DEFAULT 0,
    committed_disk_gb BIGINT NOT NULL DEFAULT 0,
    -- Actual utilization (from monitoring)
    actual_cpu_pct   DECIMAL(5,2) DEFAULT 0,
    actual_ram_pct   DECIMAL(5,2) DEFAULT 0,
    actual_gpu_pct   DECIMAL(5,2) DEFAULT 0,
    -- Conditions
    status           ENUM('ready','not_ready','cordoned','draining','provisioning') NOT NULL DEFAULT 'provisioning',
    conditions       JSON NOT NULL DEFAULT '{}',   -- memory_pressure, disk_pressure, pid_pressure, network_unavailable
    -- Metadata
    labels           JSON NOT NULL DEFAULT '{}',
    taints           JSON NOT NULL DEFAULT '[]',
    annotations      JSON NOT NULL DEFAULT '{}',
    -- Heartbeat
    last_heartbeat   TIMESTAMP(3),
    version          BIGINT NOT NULL DEFAULT 1,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_nodes_cluster_status (cluster_id, status),
    INDEX idx_nodes_type (node_type, status),
    INDEX idx_nodes_az (availability_zone, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Jobs / workloads submitted to the resource manager
CREATE TABLE jobs (
    job_id           CHAR(36) PRIMARY KEY,
    job_name         VARCHAR(255) NOT NULL,
    queue_id         CHAR(36) NOT NULL,
    tenant_id        CHAR(36) NOT NULL,
    -- Resource requests (total across all tasks)
    req_cpu          DECIMAL(10,2) NOT NULL,
    req_ram_mb       BIGINT NOT NULL,
    req_gpu          INT NOT NULL DEFAULT 0,
    req_disk_gb      BIGINT NOT NULL DEFAULT 0,
    -- Job configuration
    task_count       INT NOT NULL DEFAULT 1,       -- number of tasks (pods) in this job
    min_task_count   INT NOT NULL DEFAULT 1,       -- for gang scheduling: minimum tasks required
    is_gang          BOOLEAN NOT NULL DEFAULT FALSE,
    priority_class   ENUM('system_critical','high','medium','low','preemptible') NOT NULL DEFAULT 'medium',
    priority_value   INT NOT NULL DEFAULT 0,       -- numeric priority within class (higher = more important)
    preemptible      BOOLEAN NOT NULL DEFAULT FALSE,
    -- Scheduling constraints
    node_selectors   JSON NOT NULL DEFAULT '{}',
    affinity_rules   JSON NOT NULL DEFAULT '{}',
    tolerations      JSON NOT NULL DEFAULT '[]',
    -- State
    status           ENUM('pending','scheduling','running','succeeded','failed','cancelled','preempted') NOT NULL DEFAULT 'pending',
    -- Timing
    submitted_at     TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    scheduled_at     TIMESTAMP(3),
    started_at       TIMESTAMP(3),
    completed_at     TIMESTAMP(3),
    -- DRF tracking
    dominant_share   DECIMAL(10,6) DEFAULT 0,      -- current dominant resource share
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (queue_id) REFERENCES scheduling_queues(queue_id),
    INDEX idx_jobs_queue_status (queue_id, status),
    INDEX idx_jobs_tenant (tenant_id, status),
    INDEX idx_jobs_priority (priority_class, priority_value DESC, submitted_at),
    INDEX idx_jobs_drf (queue_id, dominant_share, submitted_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Individual tasks within a job (maps to pods/containers)
CREATE TABLE tasks (
    task_id          CHAR(36) PRIMARY KEY,
    job_id           CHAR(36) NOT NULL,
    task_index       INT NOT NULL,                 -- 0 to task_count-1
    node_id          CHAR(36),                     -- assigned node (NULL if pending)
    -- Per-task resources
    req_cpu          DECIMAL(10,2) NOT NULL,
    req_ram_mb       BIGINT NOT NULL,
    req_gpu          INT NOT NULL DEFAULT 0,
    -- State
    status           ENUM('pending','scheduled','running','succeeded','failed','evicted','preempted') NOT NULL DEFAULT 'pending',
    -- Eviction info
    eviction_reason  VARCHAR(255),
    evicted_at       TIMESTAMP(3),
    -- Timing
    scheduled_at     TIMESTAMP(3),
    started_at       TIMESTAMP(3),
    completed_at     TIMESTAMP(3),
    -- Retry
    retry_count      INT NOT NULL DEFAULT 0,
    max_retries      INT NOT NULL DEFAULT 3,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (job_id) REFERENCES jobs(job_id),
    FOREIGN KEY (node_id) REFERENCES cluster_nodes(node_id),
    INDEX idx_tasks_job (job_id, task_index),
    INDEX idx_tasks_node (node_id, status),
    INDEX idx_tasks_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Scheduling events for observability
CREATE TABLE scheduling_events (
    event_id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    job_id           CHAR(36) NOT NULL,
    task_id          CHAR(36),
    event_type       ENUM('submitted','queued','scheduling','scheduled','preempting','preempted',
                          'evicting','evicted','failed','retrying','completed','cancelled') NOT NULL,
    node_id          CHAR(36),
    details          JSON,                         -- filter results, scores, preemption victims, etc.
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    INDEX idx_events_job (job_id),
    INDEX idx_events_time (created_at),
    INDEX idx_events_node (node_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Autoscaler state
CREATE TABLE autoscaler_state (
    cluster_id       CHAR(36) PRIMARY KEY,
    enabled          BOOLEAN NOT NULL DEFAULT TRUE,
    min_nodes        INT NOT NULL DEFAULT 3,
    max_nodes        INT NOT NULL DEFAULT 10000,
    current_nodes    INT NOT NULL,
    target_nodes     INT NOT NULL,
    last_scale_out   TIMESTAMP(3),
    last_scale_in    TIMESTAMP(3),
    cooldown_sec     INT NOT NULL DEFAULT 300,     -- 5 min between scale events
    scale_out_pending_threshold INT NOT NULL DEFAULT 10, -- scale out when > N pods pending > 30s
    scale_in_util_threshold DECIMAL(5,2) NOT NULL DEFAULT 50.00, -- scale in when util < 50%
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Criteria | etcd | MySQL 8.0 | Redis | Apache Kafka |
|----------|------|-----------|-------|--------------|
| **Use case** | k8s API objects (pods, nodes, configmaps) | Extended metadata, scheduling queues, job history | Real-time node utilization cache | Scheduling event stream |
| **Consistency** | Strong (Raft) | Strong (ACID) | Eventual | Ordered within partition |
| **Throughput** | ~10K writes/sec | ~50K writes/sec | ~200K ops/sec | ~1M msgs/sec |
| **Data size limit** | 8 GB practical | Terabytes | ~100 GB per shard | Unlimited (retention-based) |
| **Query flexibility** | Key-prefix only | Full SQL | Key-value + data structures | Topic-based consume |

**Selected: Hybrid — etcd + MySQL 8.0 + Redis.**

**Justification:** 
- **etcd** is required for Kubernetes API compatibility (pods, nodes, configmaps, CRDs). It stores the core scheduling state that kubelets consume.
- **MySQL 8.0** stores extended metadata: hierarchical queue definitions, job history, autoscaler configuration, and scheduling events that need SQL queryability. The role specifically requires MySQL expertise.
- **Redis** serves as a real-time metrics cache for node utilization (updated by heartbeats), providing sub-millisecond reads for scheduling decisions.
- **Kafka** (optional) streams scheduling events for downstream consumers (monitoring, analytics, billing).

### Indexing Strategy

| Index | Table | Purpose |
|-------|-------|---------|
| `idx_jobs_drf` | jobs | DRF ordering: queue + dominant_share + submit_time for fair scheduling |
| `idx_jobs_priority` | jobs | Priority-based scheduling: priority_class + priority_value DESC |
| `idx_tasks_node` | tasks | Find all tasks on a node (for eviction planning) |
| `idx_nodes_cluster_status` | cluster_nodes | Filter ready nodes in a cluster |
| `idx_queues_parent` | scheduling_queues | Traverse queue hierarchy |
| `idx_events_time` | scheduling_events | Time-range queries for debugging |

---

## 5. API Design

### gRPC Endpoints (internal, high-throughput)

```protobuf
service ClusterResourceManager {
    // Job lifecycle
    rpc SubmitJob(SubmitJobRequest) returns (SubmitJobResponse);
    rpc CancelJob(CancelJobRequest) returns (CancelJobResponse);
    rpc GetJobStatus(GetJobStatusRequest) returns (JobStatusResponse);
    
    // Queue management
    rpc CreateQueue(CreateQueueRequest) returns (QueueResponse);
    rpc UpdateQueue(UpdateQueueRequest) returns (QueueResponse);
    rpc GetQueueStatus(GetQueueStatusRequest) returns (QueueStatusResponse);
    rpc ListQueues(ListQueuesRequest) returns (ListQueuesResponse);
    
    // Node management
    rpc RegisterNode(RegisterNodeRequest) returns (RegisterNodeResponse);
    rpc NodeHeartbeat(stream NodeHeartbeatRequest) returns (stream NodeHeartbeatResponse);
    rpc CordonNode(CordonNodeRequest) returns (NodeResponse);
    rpc DrainNode(DrainNodeRequest) returns (DrainResponse);
    
    // Autoscaler
    rpc GetAutoscalerStatus(AutoscalerStatusRequest) returns (AutoscalerStatusResponse);
    rpc UpdateAutoscalerConfig(UpdateAutoscalerConfigRequest) returns (AutoscalerStatusResponse);
    
    // Federation
    rpc GetClusterCapacity(ClusterCapacityRequest) returns (ClusterCapacityResponse);
    rpc RouteToCluster(RouteRequest) returns (RouteResponse);
    
    // Gang scheduling
    rpc SubmitPodGroup(SubmitPodGroupRequest) returns (SubmitPodGroupResponse);
}

message SubmitJobRequest {
    string idempotency_key = 1;
    string job_name = 2;
    string queue_name = 3;
    string tenant_id = 4;
    int32 task_count = 5;
    int32 min_task_count = 6;           // for gang scheduling, minimum viable
    ResourceRequirements per_task_resources = 7;
    SchedulingConstraints constraints = 8;
    string priority_class = 9;
    int32 priority_value = 10;
    bool preemptible = 11;
    int32 max_retries = 12;
    string auth_token = 13;
}

message SubmitJobResponse {
    string job_id = 1;
    string status = 2;                   // "pending" or "scheduling"
    int32 queue_position = 3;            // position in the queue
    float estimated_wait_seconds = 4;    // estimated time to scheduling
}

message NodeHeartbeatRequest {
    string node_id = 1;
    ResourceUsage current_usage = 2;     // actual utilization
    ResourceCommitted committed = 3;     // sum of pod requests
    NodeConditions conditions = 4;       // pressure signals
    repeated PodStatus pod_statuses = 5; // status of all pods on this node
    int64 timestamp_ms = 6;
}

message NodeConditions {
    bool memory_pressure = 1;            // memory.available < threshold
    bool disk_pressure = 2;              // nodefs.available < threshold
    bool pid_pressure = 3;              // pids in use > threshold
    bool network_unavailable = 4;
    int64 available_memory_mb = 5;
    int64 available_disk_gb = 6;
    int32 available_pids = 7;
}
```

### REST Endpoints (management plane)

| Method | Path | Description | Auth | Rate Limit |
|--------|------|-------------|------|------------|
| POST | `/v1/jobs` | Submit a job | JWT | 500/min/tenant |
| DELETE | `/v1/jobs/{id}` | Cancel a job | JWT | 500/min/tenant |
| GET | `/v1/jobs/{id}` | Get job status | JWT | 5,000/min/tenant |
| GET | `/v1/jobs?queue={name}&status={status}` | List jobs | JWT | 1,000/min/tenant |
| POST | `/v1/queues` | Create queue | JWT (admin) | 50/min |
| PUT | `/v1/queues/{id}` | Update queue config | JWT (admin) | 50/min |
| GET | `/v1/queues/{id}/status` | Queue status + usage | JWT | 1,000/min |
| GET | `/v1/cluster/capacity` | Cluster capacity summary | JWT (admin) | 100/min |
| GET | `/v1/cluster/nodes` | List nodes | JWT (admin) | 100/min |
| POST | `/v1/cluster/nodes/{id}/cordon` | Cordon node | JWT (admin) | 50/min |
| POST | `/v1/cluster/nodes/{id}/drain` | Drain node | JWT (admin) | 50/min |
| GET | `/v1/federation/capacity` | All clusters capacity | JWT (admin) | 100/min |
| GET | `/v1/autoscaler/status` | Autoscaler state | JWT (admin) | 100/min |
| PUT | `/v1/autoscaler/config` | Update autoscaler config | JWT (admin) | 10/min |

### CLI Design

```bash
# Job management
crm job submit \
    --name="ml-training-resnet" \
    --queue=ml-team \
    --tasks=64 --min-tasks=64 \
    --cpu=8 --ram=32768 --gpu=1 --gpu-type=h100 \
    --gang \
    --priority=high \
    --output=json

crm job status job-uuid-12345
crm job cancel job-uuid-12345
crm job list --queue=ml-team --status=running --output=table

# Queue management
crm queue create \
    --name=ml-team \
    --parent=engineering \
    --guaranteed-cpu=1000 --guaranteed-ram=2048000 --guaranteed-gpu=32 \
    --max-cpu=2000 --max-gpu=64 \
    --policy=fair_share

crm queue status ml-team --show-jobs
crm queue list --tree  # shows hierarchy

# Node management
crm node list --status=ready --type=gpu_h100
crm node cordon node-abc123 --reason="hardware issue"
crm node drain node-abc123 --grace-period=120s --respect-pdb
crm node taint node-abc123 key=value:NoSchedule

# Cluster operations
crm cluster capacity --by=node-type --output=table
crm cluster utilization --by=queue --output=table

# Autoscaler
crm autoscaler status
crm autoscaler config --min-nodes=100 --max-nodes=5000 --scale-out-threshold=20

# Federation
crm federation capacity --all-clusters
crm federation route --job=job-12345 --prefer=us-east-1
```

---

## 6. Core Component Deep Dives

### 6.1 Dominant Resource Fairness (DRF) Algorithm

**Why it's hard:**
Fair resource allocation in multi-resource systems is fundamentally more complex than single-resource (e.g., CPU-only) fairness. A naive approach (equal CPU to all, equal RAM to all) doesn't work because different workloads have different resource profiles. DRF — the standard algorithm from UC Berkeley — provides a max-min fairness guarantee across multiple resource types, but implementing it efficiently at scale with hierarchical queues, preemption, and dynamic workloads is non-trivial.

**Approaches Compared:**

| Approach | Fairness Guarantee | Multi-Resource | Complexity | Used By |
|----------|-------------------|----------------|------------|---------|
| FIFO | None (first-come-first-served) | N/A | Very Low | Simple batch systems |
| Max-min fairness (single resource) | Fair for one resource | No | Low | Classic OS schedulers |
| Dominant Resource Fairness (DRF) | Max-min fair across dominant resource | Yes | Medium | Mesos, YARN (partially) |
| Weighted Fair Queuing (WFQ) | Weighted fairness | Partially | Medium | Network schedulers |
| Hierarchical DRF (H-DRF) | DRF within hierarchy | Yes | High | YARN capacity scheduler |
| Karma (credit-based) | Long-term fairness | Yes | High | Academic research |

**Selected: Hierarchical DRF (H-DRF).**

**Justification:** DRF is the gold standard for multi-resource fairness and has theoretical guarantees (sharing incentive, strategy-proofness, envy-freeness, Pareto efficiency). Hierarchical extension matches our queue structure (org → team → project). YARN and Mesos both use DRF variants in production.

**DRF Algorithm:**

For each user/queue, the *dominant resource* is the resource for which the user's share is highest.

Example: Cluster has 100 CPU and 100 GB RAM. User A's jobs need <2 CPU, 8 GB> each. User B's jobs need <6 CPU, 2 GB> each.
- User A's dominant resource: RAM (8/100 = 8% per job)
- User B's dominant resource: CPU (6/100 = 6% per job)

DRF equalizes dominant shares. In equilibrium: User A gets ~5.7 jobs (45.7% RAM share), User B gets ~7.6 jobs (45.7% CPU share).

**Implementation (pseudocode):**

```python
class HierarchicalDRFScheduler:
    def __init__(self, cluster_capacity: ResourceVector):
        self.total = cluster_capacity  # e.g., {cpu: 10000, ram_mb: 20000000, gpu: 500}
        self.queues = {}               # queue_id -> QueueState
    
    def schedule_next(self) -> Optional[Job]:
        """Select the next job to schedule using H-DRF."""
        # Start from root queues, recursively pick the queue with lowest dominant share
        candidate_queue = self._pick_queue_recursive(root_queues=self._get_root_queues())
        if candidate_queue is None:
            return None
        
        # Within the selected queue, pick the job with lowest dominant share (or FIFO if same)
        job = self._pick_job(candidate_queue)
        return job
    
    def _pick_queue_recursive(self, queues: List[Queue]) -> Optional[Queue]:
        """Recursively pick the queue with the lowest dominant share."""
        if not queues:
            return None
        
        # Filter to queues that have pending jobs (directly or in children)
        active = [q for q in queues if self._has_pending_jobs(q)]
        if not active:
            return None
        
        # Sort by dominant share ascending
        active.sort(key=lambda q: self._compute_dominant_share(q))
        
        selected = active[0]
        
        # If selected queue has children, recurse into children
        children = self._get_child_queues(selected)
        if children:
            return self._pick_queue_recursive(children)
        
        return selected
    
    def _compute_dominant_share(self, queue: Queue) -> float:
        """Compute the dominant resource share for a queue."""
        shares = []
        if self.total.cpu > 0:
            shares.append(queue.used_cpu / self.total.cpu)
        if self.total.ram_mb > 0:
            shares.append(queue.used_ram_mb / self.total.ram_mb)
        if self.total.gpu > 0:
            shares.append(queue.used_gpu / self.total.gpu)
        return max(shares) if shares else 0.0
    
    def _pick_job(self, queue: Queue) -> Optional[Job]:
        """Within a leaf queue, pick the next job to schedule."""
        pending = queue.get_pending_jobs()
        if not pending:
            return None
        
        if queue.scheduling_policy == 'fair_share':
            # Sort by dominant share, then by submit time
            pending.sort(key=lambda j: (self._job_dominant_share(j), j.submitted_at))
        elif queue.scheduling_policy == 'priority':
            pending.sort(key=lambda j: (-PRIORITY_ORDER[j.priority_class], 
                                         -j.priority_value, j.submitted_at))
        elif queue.scheduling_policy == 'fifo':
            pending.sort(key=lambda j: j.submitted_at)
        
        return pending[0]
    
    def _job_dominant_share(self, job: Job) -> float:
        """Compute the dominant share for a single job's tenant/user within their queue."""
        tenant_usage = self._get_tenant_usage(job.tenant_id, job.queue_id)
        shares = []
        queue = self.queues[job.queue_id]
        if queue.guaranteed_cpu > 0:
            shares.append(tenant_usage.cpu / queue.guaranteed_cpu)
        if queue.guaranteed_ram_mb > 0:
            shares.append(tenant_usage.ram_mb / queue.guaranteed_ram_mb)
        if queue.guaranteed_gpu > 0:
            shares.append(tenant_usage.gpu / queue.guaranteed_gpu)
        return max(shares) if shares else 0.0
    
    def on_job_scheduled(self, job: Job, resources: ResourceVector):
        """Update queue usage after scheduling."""
        queue = self.queues[job.queue_id]
        queue.used_cpu += resources.cpu
        queue.used_ram_mb += resources.ram_mb
        queue.used_gpu += resources.gpu
        # Propagate to parent queues
        parent = queue.parent
        while parent:
            parent.used_cpu += resources.cpu
            parent.used_ram_mb += resources.ram_mb
            parent.used_gpu += resources.gpu
            parent = parent.parent
    
    def on_job_completed(self, job: Job, resources: ResourceVector):
        """Release resources when job completes."""
        queue = self.queues[job.queue_id]
        queue.used_cpu -= resources.cpu
        queue.used_ram_mb -= resources.ram_mb
        queue.used_gpu -= resources.gpu
        parent = queue.parent
        while parent:
            parent.used_cpu -= resources.cpu
            parent.used_ram_mb -= resources.ram_mb
            parent.used_gpu -= resources.gpu
            parent = parent.parent
```

**Failure Modes:**
1. **Starvation under DRF:** A queue with small jobs keeps getting scheduled while a queue needing large allocations (e.g., 64-GPU gang) never accumulates enough resources. Mitigation: "reservation" mechanism — if a large job has been waiting > N minutes, start reserving freed resources for it instead of giving them to other queues.
2. **Queue hierarchy depth:** Deep hierarchies (> 5 levels) increase scheduling latency. Mitigation: limit hierarchy to 4 levels (org → team → project → user).
3. **Usage accounting drift:** A node failure causes tasks to be marked as "running" when they've actually died. Mitigation: periodic reconciliation between task state and node-reported pod statuses.

**Interviewer Q&As:**

**Q1: What are the theoretical properties of DRF?**
A: DRF satisfies four key properties: (1) **Sharing incentive** — every user gets at least 1/n of every resource (where n is number of users), so no user is worse off than equal partitioning. (2) **Strategy-proofness** — users can't benefit by lying about their resource needs. (3) **Envy-freeness** — no user prefers another user's allocation. (4) **Pareto efficiency** — no reallocation can improve one user without hurting another.

**Q2: How does DRF handle users with very different resource profiles?**
A: That's exactly where DRF shines. If User A is CPU-heavy and User B is memory-heavy, DRF lets them each consume more of their dominant resource. The cluster achieves higher overall utilization than equal partitioning because complementary workloads fill each other's gaps.

**Q3: How do you handle a new queue that just joined with zero historical usage?**
A: New queues start with dominant share = 0, so they get priority under DRF until they catch up to the fair share. This provides excellent convergence — a new queue ramps up quickly.

**Q4: What's the difference between guaranteed and max capacity in a queue?**
A: Guaranteed capacity is the minimum resources a queue is always entitled to (other queues cannot consume these resources if this queue needs them). Max capacity is the elastic limit — the queue can burst up to max if other queues aren't using their guaranteed capacity. This is analogous to YARN's capacity scheduler with elastic scheduling.

**Q5: How does DRF interact with priority-based scheduling?**
A: They operate at different levels. DRF decides *which queue* to serve next. Within a queue, the queue's own scheduling policy (which could be priority-based) decides *which job* to serve. This is the hierarchical separation: inter-queue fairness (DRF) vs intra-queue ordering (policy).

**Q6: How do you prevent a single tenant from submitting millions of small jobs to game DRF?**
A: Per-tenant rate limits on job submission. Also, DRF is strategy-proof — submitting more jobs doesn't increase your dominant share faster than your actual resource consumption warrants. Max concurrent jobs per queue is enforced.

---

### 6.2 Gang Scheduling (Co-Scheduling)

**Why it's hard:**
Distributed ML training requires N workers running simultaneously on N GPUs. If only N-1 workers can be placed, the one missing worker makes all others useless (they block waiting for the missing peer). Traditional one-at-a-time scheduling can't guarantee atomic N-pod placement. Gang scheduling must: (1) hold resources tentatively for partial groups, (2) avoid deadlock when multiple gangs compete for the same resources, and (3) handle timeout and cleanup when a gang can never be fully placed.

**Approaches Compared:**

| Approach | Atomicity | Deadlock Risk | Resource Waste | Implementation |
|----------|-----------|---------------|----------------|----------------|
| Naive sequential (schedule one at a time) | None | None | High (partial gangs) | Trivial |
| Optimistic gang (schedule all, rollback if partial) | Yes | Low | Low | Medium |
| Reservation-based (reserve resources for entire gang) | Yes | Medium | Medium (reserved but unused) | High |
| Coscheduling plugin (k8s scheduler framework) | Yes | Low | Low | Medium |
| Dedicated gang scheduler (Volcano, YuniKorn) | Yes | Low | Low | Medium |

**Selected: Coscheduling with reservation and deadlock detection.**

**Justification:** We implement gang scheduling as a plugin in the scheduling framework (similar to Kubernetes Coscheduling plugin / Volcano). The scheduler groups pods by PodGroup, waits until quorum members are pending, then attempts to place them all. Reservation prevents resources from being stolen between individual placements. Deadlock detection prevents two gangs from each holding half the resources the other needs.

**Implementation (pseudocode):**

```python
class GangScheduler:
    def __init__(self, scheduler: SchedulingEngine):
        self.scheduler = scheduler
        self.waiting_groups = {}        # group_id -> PodGroup
        self.reservations = {}          # group_id -> {node_id: reserved_resources}
        self.GANG_TIMEOUT = 300         # 5 minutes to form a complete group
    
    def on_pod_arrives(self, pod: Pod, group: PodGroup):
        """Called when a pod that belongs to a gang arrives."""
        if group.id not in self.waiting_groups:
            self.waiting_groups[group.id] = group
            group.arrived_pods = []
            group.first_arrival = time.now()
        
        group.arrived_pods.append(pod)
        
        # Check if quorum met
        if len(group.arrived_pods) >= group.min_members:
            self._try_schedule_gang(group)
        else:
            # Wait for more pods
            pass
    
    def _try_schedule_gang(self, group: PodGroup):
        """Attempt to schedule all pods in the gang simultaneously."""
        pods = group.arrived_pods[:group.min_members]
        
        # Phase 1: Find feasible assignment for all pods
        assignment = self._find_gang_assignment(pods)
        
        if assignment is None:
            # Check if we should wait or give up
            if time.now() - group.first_arrival > self.GANG_TIMEOUT:
                self._fail_gang(group, "timeout: could not place all members")
            else:
                # Try to reserve resources to prevent starvation
                self._reserve_partial(group, pods)
            return
        
        # Phase 2: Commit all assignments atomically
        try:
            self._commit_gang(group, assignment)
        except CommitConflict:
            # Another scheduler grabbed some resources, retry
            self._release_reservations(group)
            self._try_schedule_gang(group)
    
    def _find_gang_assignment(self, pods: List[Pod]) -> Optional[Dict[Pod, Node]]:
        """Find a feasible node assignment for all pods in the gang."""
        # Strategy: sort pods by resource demand (largest first = FFD)
        pods_sorted = sorted(pods, key=lambda p: p.total_resource_magnitude, reverse=True)
        
        assignment = {}
        tentative_usage = {}  # node_id -> tentatively committed resources
        
        for pod in pods_sorted:
            # Filter nodes that can still accommodate this pod
            # considering both existing usage AND tentative gang assignments
            candidates = self.scheduler.filter_nodes(pod, extra_usage=tentative_usage)
            
            if not candidates:
                return None  # gang cannot be fully placed
            
            # Score and select best node
            scored = self.scheduler.score_nodes(pod, candidates)
            best_node = scored[0]
            
            assignment[pod] = best_node
            if best_node.id not in tentative_usage:
                tentative_usage[best_node.id] = ResourceVector.zero()
            tentative_usage[best_node.id] += pod.resources
        
        return assignment
    
    def _commit_gang(self, group: PodGroup, assignment: Dict[Pod, Node]):
        """Atomically commit all pod-to-node bindings."""
        with distributed_lock(f"gang_{group.id}"):
            for pod, node in assignment.items():
                # This uses the same optimistic locking as single-pod placement
                self.scheduler.bind(pod, node)
            
            group.status = 'scheduled'
            self._release_reservations(group)
            del self.waiting_groups[group.id]
    
    def _reserve_partial(self, group: PodGroup, pods: List[Pod]):
        """Reserve resources for pods we CAN place, so they don't get stolen."""
        # Only reserve if the gang has been waiting > 60 seconds
        if time.now() - group.first_arrival < 60:
            return
        
        partial_assignment = {}
        for pod in pods:
            candidates = self.scheduler.filter_nodes(pod)
            if candidates:
                best = self.scheduler.score_nodes(pod, candidates)[0]
                partial_assignment[pod] = best
            else:
                break  # can't even partially place, no point reserving
        
        if len(partial_assignment) > len(pods) * 0.5:
            # Reserve for the partial assignment to prevent starvation
            self.reservations[group.id] = {
                node.id: pod.resources 
                for pod, node in partial_assignment.items()
            }
    
    def periodic_deadlock_detection(self):
        """Detect and break deadlocks between competing gangs."""
        # Build wait-for graph: gang A waits for resources held by gang B's reservations
        wait_graph = {}
        for group_id, reservations in self.reservations.items():
            blocking_groups = set()
            for node_id, resources in reservations.items():
                # Find other gangs with reservations on the same node
                for other_id, other_res in self.reservations.items():
                    if other_id != group_id and node_id in other_res:
                        blocking_groups.add(other_id)
            if blocking_groups:
                wait_graph[group_id] = blocking_groups
        
        # Detect cycles (simple DFS)
        cycles = self._find_cycles(wait_graph)
        
        for cycle in cycles:
            # Break deadlock: release reservations for lowest-priority gang in cycle
            lowest = min(cycle, key=lambda gid: 
                         PRIORITY_ORDER[self.waiting_groups[gid].priority_class])
            self._release_reservations(self.waiting_groups[lowest])
            self.waiting_groups[lowest].retry_count += 1
```

**Failure Modes:**
1. **Gang timeout:** Not all members arrive within 5 minutes. Mitigation: fail the entire gang with a clear error message. The orchestrator can retry or alert the user.
2. **Deadlock between gangs:** Two gangs each hold half the resources needed by the other. Mitigation: periodic deadlock detection (every 10 seconds) that breaks cycles by releasing the lowest-priority gang's reservations.
3. **Partial failure during commit:** Pod 50 of 64 fails to bind. Mitigation: saga-style rollback — unbind pods 1-49 and retry the entire gang.
4. **Resource fragmentation for large gangs:** A 64-GPU gang needs 8 nodes with 8 GPUs each, but GPUs are spread across many nodes. Mitigation: the gang scheduler uses FFD to pack pods onto the fewest nodes possible.

**Interviewer Q&As:**

**Q1: How does gang scheduling interact with DRF?**
A: The DRF queue manager decides *when* a gang job gets to attempt scheduling (based on the queue's dominant share). The gang scheduler decides *how* to place it. A gang job's resource demand is the total across all members. DRF accounts for this total when computing the queue's share.

**Q2: What if a gang needs more GPUs than any single cluster has?**
A: Multi-cluster gang scheduling via the federation layer. The federation controller splits the gang into sub-groups and coordinates placement across clusters. This requires a fast inter-cluster network (InfiniBand or RDMA over converged Ethernet) for distributed training to work.

**Q3: How do you handle gang preemption?**
A: If a higher-priority gang needs resources, it can preempt lower-priority gangs. But preempting a gang means evicting ALL its members (partial eviction is useless — the remaining members will block). This makes gang preemption expensive, so we require explicit policy approval.

**Q4: What's the difference between your approach and Volcano?**
A: Volcano is a dedicated batch scheduling system for Kubernetes that includes gang scheduling, job queuing, and fair scheduling. Our approach is similar in concept but integrated into a unified resource manager that handles both long-running services (non-gang) and batch jobs (potentially gang). We use Volcano's PodGroup CRD concept but implement the scheduling logic in our own plugin framework.

**Q5: How do you handle elastic gang scheduling (jobs that can use 32-64 GPUs)?**
A: The `min_task_count` field allows partial gangs. If the job specifies min=32, max=64, the gang scheduler first tries to place 64. If it can't, it decrements and tries 63, 62, ... down to 32. The first feasible size is scheduled. The application must handle elastic scaling (e.g., PyTorch Elastic).

**Q6: What's the maximum gang size you can handle?**
A: Practically, gangs up to 512 pods (e.g., 512-GPU training jobs). Beyond that, the find_gang_assignment becomes slow (O(N * M) where N is gang size and M is candidate nodes). For very large gangs (1000+), we'd partition into sub-gangs of 64 and use hierarchical scheduling.

---

### 6.3 Node Pressure Eviction

**Why it's hard:**
When a node runs out of memory, disk, or PIDs, the kubelet must evict pods to prevent the node from crashing. But eviction decisions must balance: (1) freeing enough resources to relieve pressure, (2) minimizing disruption to important workloads, (3) respecting PodDisruptionBudgets, and (4) avoiding cascade effects where evicted pods overload other nodes.

**Approaches Compared:**

| Approach | Precision | Disruption | Speed | Complexity |
|----------|-----------|------------|-------|------------|
| Kill random pod | Very low | High | Instant | None |
| Kill by OOM score (Linux default) | Low | Medium | Instant | None |
| Priority-based eviction (k8s kubelet) | Medium | Medium | Seconds | Low |
| Smart eviction (predict pressure + preemptive) | High | Low | Proactive | High |
| Descheduler (periodic rebalancing) | Medium | Low | Minutes | Medium |

**Selected: Priority-based eviction with predictive pre-eviction.**

**Justification:** The standard Kubernetes eviction model (soft/hard thresholds) is reactive — it evicts pods only after pressure is detected. We enhance it with predictive monitoring: if memory usage trend predicts OOM in < 2 minutes, we proactively evict low-priority pods before hard pressure hits. This reduces the chance of uncontrolled OOM kills.

**Eviction thresholds (k8s-compatible):**

| Signal | Soft Threshold | Hard Threshold | Grace Period |
|--------|---------------|----------------|-------------|
| `memory.available` | < 500 MiB | < 100 MiB | 30 s |
| `nodefs.available` | < 15% | < 5% | 60 s |
| `nodefs.inodesFree` | < 10% | < 3% | 60 s |
| `pid.available` | < 1000 | < 100 | 10 s |

**Eviction priority order:**
1. Pods exceeding their resource requests (using more than they asked for).
2. Pods with `priority_class = preemptible`.
3. Pods with `priority_class = low`.
4. Pods with highest resource usage relative to their requests.
5. Pods with `priority_class = medium` (only under hard threshold).
6. Never evict `system_critical` or `high` priority pods due to pressure (they're protected).

**Implementation (pseudocode):**

```python
class EvictionManager:
    def __init__(self, node: Node):
        self.node = node
        self.soft_thresholds = {
            'memory_mb': 500, 'disk_pct': 15, 'pid': 1000
        }
        self.hard_thresholds = {
            'memory_mb': 100, 'disk_pct': 5, 'pid': 100
        }
        self.prediction_window_sec = 120  # predict 2 minutes ahead
    
    def check_and_evict(self, node_metrics: NodeMetrics):
        """Called every 10 seconds by the node agent."""
        pressure_type = None
        threshold_level = None
        
        # Check hard thresholds first
        if node_metrics.available_memory_mb < self.hard_thresholds['memory_mb']:
            pressure_type = 'memory'
            threshold_level = 'hard'
        elif node_metrics.available_disk_pct < self.hard_thresholds['disk_pct']:
            pressure_type = 'disk'
            threshold_level = 'hard'
        elif node_metrics.available_pids < self.hard_thresholds['pid']:
            pressure_type = 'pid'
            threshold_level = 'hard'
        
        # Check soft thresholds
        if pressure_type is None:
            if node_metrics.available_memory_mb < self.soft_thresholds['memory_mb']:
                pressure_type = 'memory'
                threshold_level = 'soft'
            # ... similar for disk and pid
        
        # Predictive check
        if pressure_type is None and self._predict_pressure(node_metrics):
            pressure_type = 'memory'
            threshold_level = 'predictive'
        
        if pressure_type is None:
            return  # No pressure
        
        # Select victims
        victims = self._select_victims(pressure_type, threshold_level, node_metrics)
        
        for pod in victims:
            self._evict_pod(pod, pressure_type, threshold_level)
    
    def _predict_pressure(self, metrics: NodeMetrics) -> bool:
        """Predict if memory will hit hard threshold within prediction_window."""
        # Use linear regression on last 10 data points (100 seconds of history)
        history = metrics.memory_history[-10:]  # list of (timestamp, available_mb)
        if len(history) < 5:
            return False
        
        times = [h[0] for h in history]
        values = [h[1] for h in history]
        
        # Simple linear regression
        slope = (values[-1] - values[0]) / (times[-1] - times[0])
        
        if slope >= 0:
            return False  # Memory is stable or increasing
        
        # Time to hit hard threshold
        time_to_threshold = (values[-1] - self.hard_thresholds['memory_mb']) / abs(slope)
        
        return time_to_threshold < self.prediction_window_sec
    
    def _select_victims(self, pressure: str, level: str, metrics: NodeMetrics) -> List[Pod]:
        """Select pods to evict, ordered by priority."""
        pods = self.node.running_pods[:]
        
        # Never evict system_critical
        pods = [p for p in pods if p.priority_class != 'system_critical']
        
        # If soft/predictive, only evict preemptible and low priority
        if level in ('soft', 'predictive'):
            pods = [p for p in pods if p.priority_class in ('preemptible', 'low')]
        
        # Sort: over-request first, then by priority ascending, then by usage descending
        def eviction_sort_key(pod):
            over_request = 1 if self._is_over_request(pod, pressure) else 0
            return (-over_request, PRIORITY_ORDER[pod.priority_class], -pod.resource_usage(pressure))
        
        pods.sort(key=eviction_sort_key)
        
        # Select minimum victims to relieve pressure
        target = self._calculate_relief_target(pressure, level, metrics)
        victims = []
        freed = 0
        
        for pod in pods:
            victims.append(pod)
            freed += pod.resource_usage(pressure)
            if freed >= target:
                break
        
        return victims
    
    def _evict_pod(self, pod: Pod, pressure: str, level: str):
        """Evict a pod with appropriate grace period."""
        grace_period = 30 if level != 'hard' else 0  # immediate kill on hard threshold
        
        # Check PodDisruptionBudget
        if not self._pdb_allows_eviction(pod):
            if level == 'hard':
                # Override PDB for hard threshold (node survival takes precedence)
                log.warn(f"Overriding PDB for pod {pod.id} due to hard {pressure} pressure")
            else:
                log.info(f"Skipping pod {pod.id} eviction due to PDB")
                return
        
        log.info(f"Evicting pod {pod.id}: {pressure} pressure ({level}), "
                 f"grace_period={grace_period}s")
        
        # Send SIGTERM, wait grace period, then SIGKILL
        pod.terminate(grace_period_seconds=grace_period)
        
        # Record eviction event
        self._record_event(pod, pressure, level)
```

**Failure Modes:**
1. **Eviction storm:** Evicted pods reschedule to the same node (the scheduler doesn't know about the pressure). Mitigation: node taint is set during pressure (`node.kubernetes.io/memory-pressure:NoSchedule`), preventing new pods from scheduling.
2. **PDB blocks all evictions:** All candidate victims are protected by PDBs. Mitigation: hard threshold overrides PDBs. For soft threshold, alert the operator.
3. **Prediction false positive:** Predictive eviction evicts pods unnecessarily. Mitigation: only evict `preemptible` pods on predictive signals. Tune the prediction model to minimize false positives (accept some false negatives).
4. **Cascade eviction:** Evicting pods from node A causes their replicas on node B to receive more traffic, pushing node B into pressure. Mitigation: the scheduler spreads evicted pod rescheduling across multiple nodes, not back to the same neighborhood.

**Interviewer Q&As:**

**Q1: Why not just rely on Linux OOM killer?**
A: The OOM killer makes poor decisions from an application perspective — it kills whichever process has the highest OOM score (essentially the largest process), which is often the most important one. Our eviction manager has application-level context: priority classes, PDBs, and workload importance.

**Q2: How does eviction interact with the descheduler?**
A: The eviction manager handles urgent, reactive eviction (node is in trouble NOW). The descheduler runs periodically (every 5 minutes) and proactively rebalances workloads across nodes to prevent pressure from building. They're complementary.

**Q3: What happens to stateful pods during eviction?**
A: StatefulSets have persistent volumes (PVCs). The eviction manager terminates the pod, and the StatefulSet controller reschedules it. The PVC is re-attached on the new node. For locally-attached storage (hostPath), the pod must return to the same node — eviction is avoided unless absolutely necessary.

**Q4: How do you handle eviction in a gang-scheduled job?**
A: Evicting one member of a gang effectively kills the entire gang (the other members will block). We treat gang members as a unit: if eviction is needed and a gang member is selected, we record that the entire gang may need rescheduling, and notify the gang scheduler to find a new assignment for the full group.

**Q5: What's the typical eviction rate in a well-managed cluster?**
A: In a well-tuned cluster, memory pressure evictions should be < 0.1% of pods per day. If higher, it indicates resource requests are set too low (pods requesting 256MB but using 1GB) or node capacity is insufficient.

**Q6: How do you prevent thrashing where a pod is evicted, rescheduled to the same node, and evicted again?**
A: (1) The node is tainted during pressure, preventing re-scheduling. (2) The scheduler avoids nodes with recent eviction events (scoring penalty). (3) Evicted pods get a scheduling backoff (increasing delay before rescheduling).

---

### 6.4 Cluster Autoscaler

**Why it's hard:**
The autoscaler must decide: (1) when to add nodes (scale out), (2) when to remove nodes (scale in), (3) which node type to add (heterogeneous fleet), and (4) how many nodes to add (right-sizing). Over-provisioning wastes money; under-provisioning causes scheduling delays. For bare-metal, the lead time is 15-30 minutes (PXE boot + OS install), so the autoscaler must predict demand ahead of time.

**Approaches Compared:**

| Approach | Responsiveness | Cost Efficiency | Complexity |
|----------|---------------|-----------------|------------|
| Reactive (scale on pending pods) | 30-60s (cloud) | Medium | Low |
| Predictive (ML-based demand forecast) | Proactive | High | High |
| Schedule-based (cron scale up/down) | Fixed | Medium | Very Low |
| Buffer-based (maintain N% spare) | Instant (spare used) | Low | Low |
| Hybrid (reactive + buffer + predictive) | Proactive with buffer | High | High |

**Selected: Hybrid reactive + buffer approach. Predictive as Phase 2.**

**Implementation (pseudocode):**

```python
class ClusterAutoscaler:
    def __init__(self, config: AutoscalerConfig, provisioner: NodeProvisioner):
        self.config = config
        self.provisioner = provisioner
        self.last_scale_out = None
        self.last_scale_in = None
    
    def evaluate(self, cluster_state: ClusterState) -> ScaleDecision:
        """Called every 30 seconds."""
        # Check cooldown
        if self._in_cooldown():
            return ScaleDecision.NO_CHANGE
        
        # Phase 1: Check for unschedulable pods (scale out trigger)
        pending_pods = cluster_state.get_pods_pending_longer_than(seconds=30)
        if len(pending_pods) >= self.config.scale_out_pending_threshold:
            return self._plan_scale_out(pending_pods, cluster_state)
        
        # Phase 2: Check for under-utilized nodes (scale in trigger)
        underutilized = self._find_underutilized_nodes(cluster_state)
        if underutilized:
            return self._plan_scale_in(underutilized, cluster_state)
        
        # Phase 3: Maintain buffer capacity
        buffer_gap = self._calculate_buffer_gap(cluster_state)
        if buffer_gap > 0:
            return self._plan_buffer_scale_out(buffer_gap, cluster_state)
        
        return ScaleDecision.NO_CHANGE
    
    def _plan_scale_out(self, pending_pods: List[Pod], state: ClusterState) -> ScaleDecision:
        """Determine what nodes to add to schedule pending pods."""
        # Group pending pods by node type requirement
        needs = {}
        for pod in pending_pods:
            node_type = self._infer_needed_node_type(pod)
            if node_type not in needs:
                needs[node_type] = {'pods': [], 'cpu': 0, 'ram': 0, 'gpu': 0}
            needs[node_type]['pods'].append(pod)
            needs[node_type]['cpu'] += pod.req_cpu
            needs[node_type]['ram'] += pod.req_ram_mb
            needs[node_type]['gpu'] += pod.req_gpu
        
        nodes_to_add = []
        for node_type, demand in needs.items():
            sku = self._get_node_sku(node_type)
            # Calculate how many nodes of this type we need
            nodes_needed = max(
                math.ceil(demand['cpu'] / sku.alloc_cpu),
                math.ceil(demand['ram'] / sku.alloc_ram_mb),
                math.ceil(demand['gpu'] / sku.alloc_gpu) if sku.alloc_gpu > 0 else 0
            )
            # Add buffer (20% extra)
            nodes_needed = math.ceil(nodes_needed * 1.2)
            # Respect max nodes
            nodes_needed = min(nodes_needed, 
                             self.config.max_nodes - state.total_nodes)
            nodes_to_add.append(ScaleOutOrder(node_type, nodes_needed))
        
        return ScaleDecision(action='scale_out', orders=nodes_to_add)
    
    def _find_underutilized_nodes(self, state: ClusterState) -> List[Node]:
        """Find nodes that have been under-utilized for > 10 minutes."""
        candidates = []
        for node in state.ready_nodes():
            # Skip nodes with system-critical pods (can't drain)
            if node.has_system_critical_pods():
                continue
            # Skip nodes added < 10 minutes ago (give them time to fill)
            if node.age_minutes < 10:
                continue
            
            utilization = max(
                node.committed_cpu / node.alloc_cpu,
                node.committed_ram_mb / node.alloc_ram_mb
            )
            if utilization < self.config.scale_in_util_threshold / 100:
                # Verify pods can be rescheduled to other nodes
                if self._pods_can_be_rescheduled(node, state):
                    candidates.append(node)
        
        return candidates
    
    def _plan_scale_in(self, nodes: List[Node], state: ClusterState) -> ScaleDecision:
        """Plan node removal (scale in)."""
        # Sort by utilization ascending (remove least-utilized first)
        nodes.sort(key=lambda n: n.committed_cpu / n.alloc_cpu)
        
        # Remove at most 10% of nodes per scale-in event
        max_remove = max(1, int(state.total_nodes * 0.10))
        to_remove = nodes[:max_remove]
        
        return ScaleDecision(action='scale_in', nodes_to_remove=to_remove)
    
    def _calculate_buffer_gap(self, state: ClusterState) -> int:
        """Calculate how many buffer nodes are needed."""
        total_cpu = sum(n.alloc_cpu for n in state.ready_nodes())
        used_cpu = sum(n.committed_cpu for n in state.ready_nodes())
        buffer_target = total_cpu * (self.config.buffer_pct / 100)  # e.g., 20%
        current_buffer = total_cpu - used_cpu
        
        if current_buffer < buffer_target:
            default_sku = self._get_node_sku('cpu_general')
            nodes_needed = math.ceil((buffer_target - current_buffer) / default_sku.alloc_cpu)
            return nodes_needed
        return 0
```

**Failure Modes:**
1. **Thrashing (scale out then immediately scale in):** Cooldown period (5 min after scale-out, 10 min after scale-in) prevents this. Also, scale-in waits for nodes to be under-utilized for > 10 minutes.
2. **Provisioning failure (bare metal):** PXE boot fails, OS install fails. Mitigation: retry with a different physical host from the spare pool. Alert after 3 failures.
3. **Over-provisioning (burst that subsides quickly):** Buffer capacity absorbs transient bursts. The 20% buffer means we always have headroom for short spikes.
4. **Wrong node type provisioned:** A CPU node is added but the pending pods need GPUs. Mitigation: `_infer_needed_node_type` examines pending pod requirements and maps to the correct SKU.

**Interviewer Q&As:**

**Q1: How does the autoscaler work with bare-metal hosts where provisioning takes 15-30 minutes?**
A: For bare metal, we maintain a "warm pool" of pre-provisioned but idle hosts. Scale-out draws from the warm pool (instant) while triggering background provisioning to replenish the pool. The warm pool size is based on historical demand patterns — typically 5-10% of total cluster size.

**Q2: How do you decide which node type to add?**
A: We analyze the pending pods' resource requirements and match to the cheapest SKU that satisfies them. If pods need GPUs, only GPU SKUs are candidates. If pods are memory-heavy, high-memory SKUs are preferred. We use a cost-efficiency score: (resources provided per dollar) weighted by the current demand profile.

**Q3: What prevents infinite scale-out?**
A: Hard max-nodes limit per cluster. Also, per-tenant quotas limit the total resources a tenant can consume, which indirectly limits scale-out. Budget alerts fire when provisioning costs exceed thresholds.

**Q4: How does scale-in ensure we don't remove nodes with important pods?**
A: The scale-in planner simulates rescheduling each pod on the candidate node to other nodes. If any pod can't be rescheduled (due to resource constraints, PDB violations, or anti-affinity rules), the node is skipped. PodDisruptionBudgets are respected during drain.

**Q5: How do you handle autoscaling across a federated multi-cluster setup?**
A: Each cluster has its own autoscaler. The federation controller monitors aggregate capacity. If one cluster is at max capacity and others have headroom, the federation controller routes new workloads to the available clusters. Autoscaling is a per-cluster decision; federation handles cross-cluster load balancing.

**Q6: What's the typical scale-out latency in production?**
A: Cloud VMs: 60-90 seconds (API call + boot + kubelet registration). Bare metal with warm pool: 2-5 minutes (IPMI power-on + PXE boot + OS already installed + kubelet join). Bare metal cold: 15-30 minutes.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

Three-phase pipeline identical to Kubernetes scheduler architecture:

1. **Filter (Predicates):** Remove nodes that can't run the pod. Predicates: PodFitsResources, PodFitsHost, PodFitsHostPorts, NodeSelector, TaintToleration, CheckNodeCondition, NodeAffinity, PodAffinityPredicate, VolumeBinding.

2. **Score (Priorities):** Rank remaining nodes. Priorities: LeastRequestedPriority (spread), MostRequestedPriority (pack), BalancedResourceAllocation, InterPodAffinityPriority, NodeAffinityPriority, TaintTolerationPriority, ImageLocalityPriority.

3. **Bind:** Commit the selection to etcd. Node resource counts updated.

### Conflict Detection

- **Optimistic concurrency** in etcd: resource version check during pod binding.
- **Scheduler cache** updated immediately after bind, preventing double-booking in the short term.
- **Periodic reconciliation** between scheduler cache and actual etcd state (every 30 seconds).

### Queue & Priority

See Section 6.1 (H-DRF) for inter-queue fairness.

Within a queue, jobs are ordered by:
1. Priority class (system_critical > high > medium > low > preemptible)
2. Priority value (numeric, higher first)
3. Dominant resource share (lower first, for DRF within queue)
4. Submit time (earlier first, for FIFO tiebreaker)

### Preemption Policy

Follows Kubernetes preemption model:
1. Higher-priority pod is unschedulable.
2. Scheduler identifies nodes where evicting lower-priority pods would make the pod schedulable.
3. Among candidate nodes, select the one requiring minimum evictions.
4. Evict victims with graceful termination.
5. Schedule the preemptor on the freed node.
6. Victims are rescheduled on other nodes (if resources available).

### Starvation Prevention

1. **DRF fairness** ensures no queue is starved relative to its guaranteed capacity.
2. **Priority aging:** pending jobs get a priority boost after waiting > 5 minutes.
3. **Resource reservation for large jobs:** after 2 minutes of waiting, freed resources are reserved for the large job instead of being given to other queues.
4. **Preemption as last resort:** if a job at priority `high` has been pending for > 10 minutes and preemption is enabled, lower-priority pods are evicted.
5. **Max wait time alert:** jobs pending > 30 minutes trigger a P2 alert for manual intervention.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Notes |
|-----------|-----------------|-------|
| **API Server** | Horizontal behind LB. Add instances per 1,000 req/s. | Stateless, reads/writes to etcd/MySQL. |
| **Scheduler** | Active-passive HA (one leader). Scale throughput via batch scheduling (schedule 100 pods per cycle). | Kubernetes limitation: single scheduler process per scheduling profile. Overcome with multiple schedulers partitioned by namespace/label. |
| **Queue Manager** | Single leader (reads from MySQL). Failover via leader election. | Bottleneck at ~50K queue evaluations/sec. Shard queues across managers. |
| **Node Monitor** | Partition nodes across monitor instances by hash(node_id). | Each monitor handles ~2,000 nodes. |
| **Eviction Manager** | Runs on each node (embedded in kubelet/agent). | Scales linearly with nodes. |
| **Autoscaler** | One per cluster (leader-elected). | Low throughput requirement. |
| **Federation** | One controller per region. | Lightweight routing decisions. |

### Database Scaling

| Store | Scaling Strategy |
|-------|------------------|
| **etcd** | 3-5 node Raft cluster. Max ~8 GB. Compaction every hour. If pod count exceeds etcd capacity, shard namespaces across etcd clusters. |
| **MySQL** | Primary + 2 read replicas. Schedule history archived to cold storage monthly. Connection pooling via ProxySQL (500 connections). |
| **Redis** | 6-shard cluster for real-time metrics. Each shard handles ~3,333 node heartbeats/sec. |

### Caching

| Layer | Technology | Data | TTL | Hit Ratio |
|-------|-----------|------|-----|-----------|
| **Scheduler cache (L1)** | In-memory (Go struct) | Node state snapshot | Updated on watch events | 99%+ |
| **Node metrics (L2)** | Redis | Real-time utilization | 10s (heartbeat interval) | 95% |
| **Queue state (L2)** | Redis | Queue usage aggregates | 5s | 90% |
| **MySQL read cache (L3)** | ProxySQL query cache | Job history, config | 60s | 80% |
| **etcd watch cache** | kube-apiserver | API objects | Real-time (watch stream) | N/A |

**Interviewer Q&As:**

**Q1: How does Kubernetes handle scheduler scalability with 10,000 nodes?**
A: The default k8s scheduler processes pods sequentially but uses parallelism in the scoring phase. The `percentageOfNodesToScore` parameter (default 50% for large clusters) limits how many nodes are scored, trading optimality for speed. At 10,000 nodes, scoring 5,000 nodes per pod is sufficient. Throughput: ~5,000 pods/sec with a single scheduler.

**Q2: How would you handle 100,000 nodes across the federation?**
A: Each cluster handles 10,000 nodes independently. The federation controller maintains an aggregate capacity view (updated every 30 seconds) and routes workloads to the best cluster. No single scheduler ever sees 100,000 nodes. This is similar to how Google Borg uses cells of ~10,000 machines.

**Q3: What's the performance impact of DRF computation at scale?**
A: DRF computation is O(Q * log Q) where Q is the number of queues, for each scheduling cycle. With 1,000 queues, this is < 1ms. The dominant share for each queue is maintained incrementally (updated on job schedule/complete), not recomputed from scratch.

**Q4: How do you handle scheduler crashes mid-decision?**
A: The scheduler is stateless — its cache is rebuilt from etcd on restart (takes 5-10 seconds for 300,000 pods). Pods that were in the middle of scheduling are re-processed. The bind operation is idempotent (if the pod was already bound, the bind is a no-op).

**Q5: How do you prevent a scheduling decision storm after a network partition heals?**
A: When a partition heals, many nodes may transition from NotReady to Ready simultaneously. The scheduler has a rate limiter (max 1,000 scheduling decisions per second) to prevent overwhelming etcd. Pods are dequeued at a controlled rate.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery | RTO |
|---------|--------|-----------|----------|-----|
| Scheduler process crash | New pods remain pending (not placed) | Leader health check (3s) | Standby takes over via leader election (etcd lease). Cache rebuilds from etcd. | 10-15 s |
| etcd majority loss | Entire cluster API down | etcd health endpoint, apiserver errors | Restore from etcd snapshot. Requires operator intervention. | 5-30 min |
| MySQL primary failure | Queue management and history writes fail | Replication lag spike, health check | Orchestrator promotes replica. | 30-60 s |
| Node failure (single) | Pods on that node die | No heartbeat for 40s → mark NotReady | Pods rescheduled to other nodes after 5-min grace period (to allow for transient failures). | 5 min |
| Node failure (mass, e.g., rack) | 100+ nodes lost, many pods die | Multiple NotReady events | Pods rescheduled. Autoscaler triggers if capacity insufficient. PDBs prevent cascading. | 5-15 min |
| Network partition (node ↔ control plane) | Isolated nodes continue running but can't receive updates | Heartbeat timeout | Pods on isolated nodes keep running (kubelet maintains last state). When partition heals, state reconciles. | Self-healing |
| Scheduler-etcd partition | Scheduler can't read/write state | API errors from scheduler | Scheduler retries with backoff. Standby scheduler in other AZ takes over if using multi-AZ. | 10-30 s |
| Redis cluster failure | Scheduling works but uses stale metrics | Redis health check | Fallback to etcd-based node status (slightly less real-time). | Self-healing |

### Automated Recovery

1. **Scheduler HA:** 2 scheduler instances in active-passive. Leader election via etcd lease (15s TTL, 10s renew). Failover in < 5 seconds.
2. **etcd recovery:** Automated backup every hour to S3. Restore runbook tested quarterly. etcd operator manages member replacement.
3. **Node auto-repair:** If a node is NotReady for > 15 minutes, the auto-repair controller attempts: (1) kubelet restart via SSH, (2) full node reboot via IPMI, (3) node replacement from warm pool.
4. **Pod restart policy:** Pods with `restartPolicy: Always` (default for Deployments) restart automatically on the same node if the container exits.

### Retry Strategy

| Operation | Retry Policy | Max Retries | Backoff |
|-----------|-------------|-------------|---------|
| Pod scheduling | Requeueing with exponential backoff | Unlimited (until timeout) | 1s, 2s, 4s, 8s, ..., max 60s |
| etcd write | Immediate retry on conflict, backoff on timeout | 5 | 100ms, 200ms, 400ms, 800ms, 1600ms |
| MySQL write | Retry on deadlock | 3 | 50ms, 100ms, 200ms |
| Node heartbeat | Next heartbeat cycle (10s) | Continuous | 10s fixed |
| Autoscaler provisioning | Retry with different host | 3 | 30s between attempts |
| Gang scheduling | Requeue entire group | 5 | 30s, 60s, 120s, 240s, 300s |

### Circuit Breaker

| Dependency | Failure Threshold | Open Duration | Behavior When Open |
|------------|-------------------|---------------|-------------------|
| etcd | 3 failures in 5s | 15s | Scheduler pauses (no new scheduling). Pods keep running. |
| MySQL | 5 failures in 10s | 30s | Queue management read from cache. Writes queued. |
| Redis | 10 failures in 5s | 10s | Fall back to etcd node status. |
| Node provisioner | 3 failures in 60s | 120s | Autoscaler pauses scale-out. Alert fires. |

### Consensus & Coordination

- **etcd Raft:** All Kubernetes API objects (pods, nodes, configmaps) use etcd's Raft consensus. Strong consistency within the etcd cluster.
- **Scheduler leader election:** etcd lease-based. Only one scheduler writes bindings at a time. Prevents split-brain scheduling.
- **Federation consensus:** Each cluster is independent. The federation controller is eventually consistent (30-second sync interval). No distributed transactions across clusters.
- **DRF state:** Queue usage counters are maintained in MySQL with row-level locking. No distributed consensus needed — single MySQL primary serializes all writes.

---

## 10. Observability

### Key Metrics

| Metric | Type | Labels | Alert Threshold |
|--------|------|--------|-----------------|
| `scheduler_queue_depth` | Gauge | queue, priority | > 1000 pending |
| `scheduler_scheduling_duration_ms` | Histogram | result, queue | p99 > 500ms |
| `scheduler_throughput_pods_per_sec` | Gauge | | < 1000 sustained |
| `scheduler_preemption_total` | Counter | priority_class, queue | > 100/hour |
| `scheduler_gang_wait_seconds` | Histogram | gang_size | p99 > 60s |
| `scheduler_drf_share_deviation` | Gauge | queue | > 0.1 (10% deviation from fair) |
| `node_eviction_total` | Counter | reason, priority | > 50/hour cluster-wide |
| `node_not_ready_count` | Gauge | region, az | > 5% of nodes |
| `autoscaler_pending_pods` | Gauge | | > 100 for > 2 min |
| `autoscaler_node_count` | Gauge | type, status | Approaching max |
| `autoscaler_scale_event_total` | Counter | action(out/in), type | > 5 events/hour |
| `cluster_utilization_cpu` | Gauge | cluster, az | < 40% or > 85% |
| `cluster_utilization_gpu` | Gauge | cluster, gpu_type | > 95% |
| `etcd_request_duration_ms` | Histogram | operation | p99 > 100ms |
| `federation_routing_latency_ms` | Histogram | source, dest | p99 > 500ms |

### Distributed Tracing

- **Trace per scheduling decision:** Spans for queue_dequeue → filter → score → bind → confirm.
- **Trace per gang scheduling:** Parent span for the PodGroup, child spans for each member pod's placement.
- **Trace per autoscaler decision:** Spans for evaluate → plan → provision → register.
- **Context propagation:** W3C TraceContext from API submission through scheduler to kubelet.
- **Sampling:** 1% for normal operations, 100% for errors and slow operations (> 1s).

### Logging

| Level | When | Content |
|-------|------|---------|
| INFO | Pod scheduled | pod_id, node_id, queue, latency_ms, score |
| INFO | Scale event | direction, count, node_type, trigger_reason |
| WARN | Pod pending > 5 min | pod_id, queue, filter_results |
| WARN | Preemption | preemptor_id, victim_ids, freed_resources |
| ERROR | Scheduling failure | pod_id, all_filter_results (per-node breakdown) |
| ERROR | Gang timeout | group_id, member_count, placed_count |
| ERROR | Autoscaler failure | error_type, cluster_state_summary |

Structured JSON. Shipped to Elasticsearch via Filebeat. Retention: 30 days hot, 90 days warm.

### Alerting

| Alert | Condition | Severity | Escalation |
|-------|-----------|----------|------------|
| SchedulerDown | No successful scheduling for 30s | P1 | PagerDuty immediate |
| HighPendingCount | > 500 pods pending for > 5 min | P1 | PagerDuty immediate |
| GangStarvation | Gang pending > 15 min | P2 | PagerDuty 15 min |
| DRFUnfairness | Any queue > 20% deviation from fair share for > 10 min | P2 | Slack alert |
| NodeEvictionStorm | > 100 evictions/hour | P2 | PagerDuty 15 min |
| AutoscalerStuck | Scale-out triggered but no nodes added in 10 min | P1 | PagerDuty immediate |
| EtcdLatency | p99 > 200ms for 5 min | P1 | PagerDuty immediate |
| ClusterCapacityLow | > 90% utilization any dimension for 30 min | P2 | Capacity planning team |

---

## 11. Security

### Auth & AuthZ

- **API authentication:** mTLS for service-to-service. JWT (OIDC) for human users. Service accounts with scoped tokens for automated systems.
- **RBAC model:**
  - `cluster-admin`: Full access. Manage queues, nodes, autoscaler config.
  - `queue-admin`: Manage specific queues and their sub-queues. Submit/cancel jobs in their queues.
  - `job-submitter`: Submit and manage their own jobs within assigned queues.
  - `viewer`: Read-only access to cluster/queue status and their own job status.
- **Namespace isolation:** Jobs in different namespaces cannot see each other. Queue access is granted per namespace.
- **Network policy:** Scheduler components communicate only via defined API endpoints. No direct pod-to-pod access outside declared dependencies.

### Secrets Management

- **etcd encryption at rest:** EncryptionConfiguration with AES-CBC or AES-GCM. Keys stored in KMS (AWS KMS or Vault Transit).
- **Database credentials:** HashiCorp Vault with dynamic credentials. 1-hour TTL, auto-renewed.
- **Kubelet bootstrap tokens:** Short-lived tokens for node registration. Expired after first use.
- **mTLS certificates:** cert-manager with internal CA. 90-day rotation.

### Audit Logging

All API mutations logged to a dedicated audit stream:
- **Job lifecycle events:** submit, cancel, schedule, preempt, evict, complete.
- **Queue configuration changes:** create, update, delete queues. Capacity changes.
- **Node management:** cordon, drain, taint, label changes.
- **Autoscaler actions:** scale-out, scale-in decisions with full reasoning.
- **RBAC changes:** role bindings created/modified/deleted.

Audit logs: append-only, stored in Kafka for real-time processing + S3 for long-term retention (7 years).

---

## 12. Incremental Rollout Strategy

**Phase 1 — Scheduler Plugin (Week 1-4):**
Deploy the new scheduler as a secondary scheduler alongside the existing one. Label test namespaces to use the new scheduler. Production namespaces remain on the old scheduler.

**Phase 2 — DRF Queue Manager (Week 5-8):**
Migrate one team's queue to the new DRF-based queue manager. Monitor fairness metrics. Compare with FIFO baseline.

**Phase 3 — Gang Scheduling (Week 9-12):**
Enable gang scheduling for ML training pipelines in the test cluster. Run comparison against manual synchronization approach.

**Phase 4 — Autoscaler (Week 13-16):**
Enable autoscaler in "dry run" mode (calculates decisions but doesn't execute). Validate decisions against human operator decisions for 4 weeks.

**Phase 5 — Full Production (Week 17-20):**
Gradually migrate all namespaces to the new scheduler. Enable autoscaler in active mode with conservative limits (scale out only, no scale in initially).

**Rollout Q&As:**

**Q1: How do you handle a scheduler bug that causes pod placement failures?**
A: Feature flag instantly routes all scheduling back to the old scheduler. Any pods placed by the new scheduler continue running (placement is valid). New pods go through the old scheduler.

**Q2: How do you validate DRF fairness in production?**
A: We compare each queue's actual dominant share against the ideal fair share. If any queue deviates > 10% for > 10 minutes, we alert. We also compare job wait times across queues — no queue should have systematically higher wait times.

**Q3: What if the gang scheduler causes resource deadlocks?**
A: The deadlock detector runs every 10 seconds. If it can't resolve deadlocks automatically (by releasing lowest-priority gang's reservations), an alert fires and the operator can manually cancel the offending gang jobs.

**Q4: How do you test autoscaler scale-in without risking production?**
A: Start with scale-in disabled. Enable dry-run mode that logs which nodes *would* be removed. Operators review for 2 weeks. Then enable with a cap (max 1 node per hour). Gradually increase.

**Q5: What's the rollback plan for the entire system?**
A: Each component has an independent feature flag. We can roll back any component independently without affecting others. The shared state (etcd, MySQL) is backward-compatible — new columns are ignored by the old scheduler.

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Risk |
|----------|-------------------|--------|-----------|------|
| Scheduling approach | Single-level (k8s) vs Two-level (Mesos) vs Shared-state (Omega) | Single-level with plugin framework | Simpler operational model. Plugin framework provides extensibility without two-level complexity. | Centralized scheduler becomes bottleneck at extreme scale. |
| Queue fairness algorithm | FIFO vs Priority vs DRF vs WFQ | Hierarchical DRF | Gold standard for multi-resource fairness. Theoretical guarantees. | Implementation complexity. May over-optimize for fairness at cost of throughput. |
| Gang scheduling | Bolt-on (Volcano) vs Native scheduler plugin | Native plugin | Tighter integration with DRF and preemption. Single scheduling loop. | More code to maintain vs using existing open-source. |
| State store | etcd-only vs etcd + MySQL vs CockroachDB | etcd + MySQL | etcd for k8s compatibility, MySQL for extended features (team expertise). | Two systems to operate. Data consistency between them. |
| Autoscaler timing | Reactive-only vs Predictive vs Hybrid | Hybrid (reactive + buffer) | Buffer handles transient spikes, reactive handles sustained growth. | Buffer wastes some resources. |
| Preemption scope | Pod-level vs Job-level vs Queue-level | Pod-level (k8s compatible) with job-level awareness | Compatible with k8s ecosystem. Gang-aware preemption as enhancement. | Pod-level preemption may partially kill a job without fully freeing enough resources. |
| Multi-cluster | Single mega-cluster vs Federation | Federation (10 clusters of 10K nodes) | Failure blast radius. Independent scaling. Regional data sovereignty. | Cross-cluster scheduling is eventually consistent. |
| Node eviction | Reactive-only vs Predictive | Reactive with predictive enhancement | Predictive reduces OOM kills by ~40% (based on benchmarks). | False positive predictions cause unnecessary evictions. |

---

## 14. Agentic AI Integration

### AI-Powered Resource Management

**1. Intelligent Queue Advisor:**
An LLM agent monitors queue performance and advises on configuration changes: "Queue 'ml-training' has been at 100% capacity for 72 hours. Recommend increasing guaranteed GPU from 32 to 48, taking from 'batch-analytics' which is 40% utilized." The agent generates a change proposal that requires human approval.

**2. Anomaly-Based Scheduling Policy Adjustment:**
An ML model detects scheduling anomalies: unusual preemption patterns, fairness violations, scheduling latency spikes. When anomalies are detected, an AI agent investigates root causes by correlating with node health, workload patterns, and infrastructure events. It proposes targeted fixes: "Scheduling latency spike correlates with 200 simultaneous gang submissions from the ML platform. Recommend rate-limiting gang submissions to 10/minute from this tenant."

**3. Predictive Autoscaling:**
Train a time-series model (Prophet, LSTM) on historical scheduling demand patterns. Features: time of day, day of week, recent deployment events, CI/CD pipeline status. The model predicts demand 30 minutes ahead and pre-provisions nodes. Especially valuable for bare-metal where provisioning takes 15-30 minutes.

**4. Natural Language Ops Interface:**
- "Why are pods in the ml-team queue waiting so long?" → Agent analyzes: queue at capacity, 3 large gang jobs blocking, dominant share is 0.45 (above fair share due to burst). Recommends: wait for current jobs to complete, or increase queue capacity.
- "What's the impact of losing 2 GPU nodes?" → Agent simulates: 4 running ML jobs would need rescheduling. Cluster has 6 spare GPU nodes. Impact: ~5 min interruption for affected jobs.
- "Optimize the cluster for cost" → Agent proposes: scale in 15 under-utilized CPU nodes (saves $X/month), consolidate 8 low-utilization GPU jobs to 2 nodes (saves $Y/month).

**5. DRF Weight Auto-Tuning:**
Instead of static queue guaranteed capacities, an AI agent continuously adjusts based on actual demand patterns and business priorities. It learns that "ml-team" needs more GPUs on weekdays but "batch-analytics" needs more on weekends, and adjusts quotas accordingly.

### Guard Rails
- All AI recommendations require human approval before execution.
- Confidence thresholds: only present recommendations with > 80% confidence.
- Audit trail: every AI recommendation logged with full reasoning chain.
- Kill switch: disable AI features instantly via feature flag.
- Simulation mode: AI recommendations are simulated before execution to validate safety.

---

## 15. Complete Interviewer Q&A Bank

**Q1: Compare YARN, Mesos, and Kubernetes scheduling architectures.**
A: **YARN:** Monolithic scheduler — the ResourceManager makes all decisions. Simple but bottleneck at scale (one scheduler thread). ApplicationMasters handle task-level scheduling within their allocation. **Mesos:** Two-level scheduling — Mesos offers resources to frameworks, frameworks choose whether to accept. Enables framework-specific scheduling policies but can lead to offer fragmentation. **Kubernetes:** Single scheduler with plugin framework. Filter → Score → Bind pipeline. Extensible via scheduling framework API. Single scheduler per profile, but can run multiple profiles.

**Q2: What is Dominant Resource Fairness and why is it important?**
A: DRF is a fair allocation algorithm for environments with multiple resource types. For each user, it identifies their "dominant resource" — the resource for which they have the highest share. DRF equalizes dominant shares across users. It's important because simple per-resource fairness doesn't work when workloads have different resource profiles (CPU-heavy vs memory-heavy).

**Q3: How does gang scheduling prevent deadlock?**
A: Two gangs can deadlock if each holds reservations that the other needs. We prevent this with: (1) a periodic deadlock detector that builds a wait-for graph and finds cycles, (2) breaking cycles by releasing the lowest-priority gang's reservations, (3) a global ordering — gangs are scheduled in priority order, so higher-priority gangs always proceed first.

**Q4: How does the cluster autoscaler decide what type of node to add?**
A: It examines unschedulable pods' resource requests and scheduling constraints (node selectors, tolerations). It simulates placing each pod on each available node type and selects the cheapest node type that satisfies the most pending pods. For GPU pods, it selects the specific GPU SKU. It also considers pending reservations and anticipated demand.

**Q5: What is the difference between eviction and preemption?**
A: **Eviction** is reactive — the kubelet evicts pods because the node is under resource pressure (OOM, disk full). It's a node-local decision. **Preemption** is proactive — the scheduler evicts lower-priority pods to make room for higher-priority pods that can't be scheduled. It's a cluster-level scheduling decision.

**Q6: How do you handle the "thundering herd" problem after a node failure?**
A: When a node fails, all its pods become pending simultaneously. Without throttling, the scheduler would try to place 30+ pods on the same replacement node. We handle this with: (1) spreading scoring to distribute pods across nodes, (2) scheduling rate limiting (max 1,000 bindings/sec), (3) anti-affinity rules ensuring replicas spread across nodes.

**Q7: How does two-level scheduling (Mesos-style) compare to single-level (k8s-style)?**
A: Two-level scheduling gives frameworks more control — each framework can implement its own scheduling policy (e.g., Spark has different needs than TensorFlow). But it can lead to offer fragmentation (resources offered to framework A but not accepted, while framework B could have used them). Single-level scheduling has global visibility, leading to better utilization, but requires all policies to be in one scheduler (which plugins address).

**Q8: How do you implement priority-based preemption in Kubernetes?**
A: (1) Pod has a PriorityClass with a numeric value. (2) Scheduler can't place the pod on any node. (3) Scheduler identifies "preemption candidates" — nodes where evicting lower-priority pods would make the pod schedulable. (4) Selects the node with minimum evictions. (5) Sets pods' `deletionTimestamp` (graceful termination). (6) After termination, the preempting pod is scheduled on the freed node.

**Q9: What is node pressure eviction and what are the signals?**
A: Node pressure eviction occurs when a node's resources are critically low. Signals: `memory.available` (< 100 MiB hard threshold), `nodefs.available` (< 10%), `nodefs.inodesFree` (< 5%), `pid.available` (< 100). When thresholds are breached, kubelet evicts pods in priority order: BestEffort first, then Burstable, then Guaranteed (in extreme cases only).

**Q10: How do you implement fairness in a hierarchical queue system?**
A: Hierarchical DRF: at each level of the queue tree, child queues compete using DRF based on their guaranteed capacity. A parent queue's resources are distributed among children proportionally. Elastic sharing allows a child to exceed its guarantee if siblings aren't using their share. The recursion descends from root to leaf, always picking the child with the lowest dominant share.

**Q11: How would you handle multi-cluster gang scheduling?**
A: Federation controller receives the gang request. It queries capacity across clusters and finds a cluster (or set of clusters) with enough resources. For intra-cluster gangs, it routes the entire group to one cluster. For very large gangs (> single cluster capacity), it splits into sub-groups with inter-cluster networking requirements (RDMA/GPUDirect). This is significantly harder and often avoided — instead, clusters are sized to handle the largest expected gang.

**Q12: How does the scheduler handle node taints and tolerations?**
A: Taints are set on nodes (e.g., `gpu=true:NoSchedule`). Tolerations are set on pods. A pod can only be scheduled on a tainted node if it has a matching toleration. Effects: `NoSchedule` (won't schedule new pods), `PreferNoSchedule` (soft preference), `NoExecute` (evict existing pods that don't tolerate). This mechanism is used for GPU nodes, maintenance, and dedicated tenancy.

**Q13: How do you prevent scheduler resource accounting drift?**
A: Three mechanisms: (1) The scheduler cache is updated synchronously on every bind (immediate). (2) A watch on etcd pod events updates the cache when pods are deleted/updated externally. (3) Periodic full reconciliation (every 30 seconds) re-reads all nodes and pods from etcd and rebuilds the cache from scratch. Metric: `scheduler_cache_stale_count` alerts if drift > 0.

**Q14: How would you implement a "spot" or "preemptible" tier?**
A: Preemptible workloads use the lowest PriorityClass. They can be scheduled on any node that has spare capacity (committed < allocatable). When a non-preemptible workload needs the capacity, preemptible workloads get a 30-second SIGTERM. The autoscaler doesn't count preemptible workloads when deciding to scale out (they're opportunistic). Billing is at a discount (e.g., 70% off).

**Q15: How does the descheduler complement the scheduler?**
A: The scheduler makes point-in-time optimal decisions. Over time, the cluster drifts from optimal (nodes become unbalanced, anti-affinity violations accumulate after failures). The descheduler runs periodically and identifies pods that should be moved: pods violating inter-pod anti-affinity, pods on over-utilized nodes, duplicate pods on the same node. It evicts them, and the scheduler re-places them optimally.

**Q16: How would you implement resource quotas in the scheduling pipeline?**
A: Quotas are enforced at two levels: (1) **Admission control** — when a job is submitted, check if the tenant/queue has remaining quota before accepting. This is fast (quota lookup) and prevents queue flooding. (2) **Scheduler predicate** — during scheduling, verify that placing this pod doesn't violate any quota. This catches edge cases where multiple pods are in-flight simultaneously.

**Q17: What's the Omega shared-state approach and when would you use it?**
A: Google's Omega uses a shared-state approach: all schedulers read from and write to a shared cluster state store. Each scheduler makes optimistic decisions, and conflicts are detected at commit time. Advantages: no central scheduler bottleneck, each scheduler is specialized for its workload type. Disadvantages: conflict resolution complexity, potential for wasted work. Use it when you have multiple distinct workload types (services, batch, ML) that benefit from specialized schedulers.

---

## 16. References

1. Ghodsi, A., et al. "Dominant Resource Fairness: Fair Allocation of Multiple Resource Types." *NSDI 2011*.
2. Vavilapalli, V., et al. "Apache Hadoop YARN: Yet Another Resource Negotiator." *SoCC 2013*.
3. Hindman, B., et al. "Mesos: A Platform for Fine-Grained Resource Sharing in the Data Center." *NSDI 2011*.
4. Schwarzkopf, M., et al. "Omega: Flexible, Scalable Schedulers for Large Compute Clusters." *EuroSys 2013*.
5. Verma, A., et al. "Large-scale cluster management at Google with Borg." *EuroSys 2015*.
6. Kubernetes Scheduling Framework: https://kubernetes.io/docs/concepts/scheduling-eviction/scheduling-framework/
7. Volcano — Kubernetes Native Batch Scheduling: https://volcano.sh/
8. Apache YuniKorn — Universal Resource Scheduler: https://yunikorn.apache.org/
9. Kubernetes Cluster Autoscaler: https://github.com/kubernetes/autoscaler/tree/master/cluster-autoscaler
10. Kubernetes Descheduler: https://github.com/kubernetes-sigs/descheduler
11. Kubernetes Node Pressure Eviction: https://kubernetes.io/docs/concepts/scheduling-eviction/node-pressure-eviction/
