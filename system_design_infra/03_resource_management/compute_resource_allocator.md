# System Design: Compute Resource Allocator

> **Relevance to role:** A cloud infrastructure platform engineer must place workloads onto bare-metal and virtualized hosts while maximizing cluster utilization, respecting hardware topology (NUMA, GPU NVLink), and maintaining SLA guarantees. This system is the core of every IaaS platform, from OpenStack Nova scheduler to Kubernetes kube-scheduler to proprietary bare-metal provisioning engines.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Accept a workload placement request specifying CPU, RAM, GPU, disk, and network bandwidth demands. |
| FR-2 | Return an optimal host assignment within the latency SLA. |
| FR-3 | Support multi-dimensional bin packing across CPU, RAM, GPU, disk IOPS, and network bandwidth simultaneously. |
| FR-4 | Enforce affinity and anti-affinity rules (rack-aware, zone-aware, pod affinity/anti-affinity). |
| FR-5 | Support NUMA-aware placement for latency-sensitive workloads. |
| FR-6 | Support GPU topology-aware placement (NVLink domain affinity). |
| FR-7 | Provide spread vs pack scheduling policies (reliability vs efficiency). |
| FR-8 | Trigger defragmentation workflows (live migration, drain + reschedule) when fragmentation exceeds thresholds. |
| FR-9 | Support resource overcommit with configurable ratios per resource type. |
| FR-10 | Expose placement dry-run API for capacity analysis without actual allocation. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Placement latency (p50) | < 50 ms |
| NFR-2 | Placement latency (p99) | < 200 ms |
| NFR-3 | Throughput | 5,000 placements/sec cluster-wide |
| NFR-4 | Availability | 99.99% |
| NFR-5 | Consistency | Strong consistency for resource accounting |
| NFR-6 | Fragmentation ratio | < 15% wasted capacity at any point |
| NFR-7 | Cluster utilization target | > 75% average |

### Constraints & Assumptions

- Fleet size: 50,000 physical hosts across 5 regions, 10 availability zones.
- Host heterogeneity: at least 8 SKUs (CPU-only, GPU A100, GPU H100, high-memory, storage-optimized, ARM, bare-metal HPC, FPGA).
- Average VM/container density: 20 workloads per physical host.
- Overcommit is allowed for CPU (up to 5:1) and RAM (up to 1.2:1). GPU is never overcommitted.
- Live migration is available for VMs; containers are rescheduled.
- Resource state is cached locally and reconciled periodically with ground truth from node agents.

### Out of Scope

- Network topology design and SDN configuration.
- Image management and boot process.
- Billing and cost attribution (handled by separate system).
- User-facing portal and VM lifecycle management beyond placement.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Total physical hosts | 50,000 | Given |
| Average workloads per host | 20 | Given |
| Total active workloads | 1,000,000 | 50,000 x 20 |
| New placements per day | 200,000 | ~20% churn rate on 1M workloads |
| Peak placements per second | 5,000 | Burst during major deployments (25x average of ~2.3/sec) |
| Host heartbeats per second | 16,667 | 50,000 hosts x 1 heartbeat / 3 sec |
| Defragmentation events per day | 500 | ~1% of hosts require defrag daily |

### Latency Requirements

| Operation | p50 | p99 | p999 |
|-----------|-----|-----|------|
| Single placement (filter + score + bind) | 30 ms | 150 ms | 500 ms |
| Batch placement (gang scheduling, 64 pods) | 200 ms | 800 ms | 2 s |
| Dry-run placement | 20 ms | 100 ms | 300 ms |
| Defragmentation plan generation | 5 s | 30 s | 60 s |
| Host state refresh (from cache) | 1 ms | 5 ms | 10 ms |

### Storage Estimates

| Data | Size per record | Count | Total |
|------|-----------------|-------|-------|
| Host state (resources, labels, taints) | 4 KB | 50,000 | 200 MB |
| Workload placement records | 1 KB | 1,000,000 active | 1 GB |
| Placement history (90 days) | 0.5 KB | 18,000,000 | 9 GB |
| Defragmentation plans | 10 KB | 500/day x 90 days | 450 MB |
| **Total active state** | | | **~1.2 GB** |
| **Total with history** | | | **~11 GB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Heartbeats inbound | 16,667/sec x 2 KB | 33 MB/s |
| Placement requests | 5,000/sec x 2 KB | 10 MB/s |
| Placement responses | 5,000/sec x 1 KB | 5 MB/s |
| State sync (full) | 200 MB every 60 s | 3.3 MB/s |
| **Total** | | **~51 MB/s** |

---

## 3. High Level Architecture

```
                           ┌──────────────────────────┐
                           │     API Gateway / LB      │
                           │  (auth, rate limit, TLS)  │
                           └─────────┬────────────────┘
                                     │
                    ┌────────────────┼────────────────┐
                    │                │                │
              ┌─────▼─────┐   ┌─────▼─────┐   ┌─────▼─────┐
              │ Allocator  │   │ Allocator  │   │ Allocator  │
              │ Instance 1 │   │ Instance 2 │   │ Instance N │
              │ (Stateless)│   │ (Stateless)│   │ (Stateless)│
              └─────┬──────┘   └─────┬──────┘   └─────┬──────┘
                    │                │                │
                    └────────┬───────┴────────┬───────┘
                             │                │
                    ┌────────▼────────┐  ┌────▼──────────────┐
                    │  Cluster State  │  │  Placement Engine  │
                    │     Cache       │  │  (Filter → Score   │
                    │ (Redis Cluster) │  │   → Bind pipeline) │
                    └────────┬────────┘  └────────┬──────────┘
                             │                    │
              ┌──────────────┼────────────────────┘
              │              │
     ┌────────▼──────┐  ┌───▼──────────────┐
     │ Resource State │  │  Defragmentation  │
     │   Database     │  │     Engine        │
     │  (MySQL 8.0)   │  │  (async worker)   │
     │  + Read Replicas│  └───┬──────────────┘
     └────────┬───────┘      │
              │              │
     ┌────────▼──────────────▼───────────┐
     │        Node Agent Fleet           │
     │  (50,000 agents reporting state)  │
     │  heartbeat → gRPC → state cache   │
     └──────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **API Gateway** | TLS termination, authentication, rate limiting (token bucket), request routing. |
| **Allocator Instances** | Stateless services accepting placement requests. Horizontally scaled. Each runs the full filter → score → bind pipeline. |
| **Cluster State Cache** | Redis Cluster holding real-time host resource snapshots. Updated by node heartbeats. Read by allocators for fast filtering. |
| **Placement Engine** | Core algorithm library: multi-dimensional bin packing, affinity evaluation, NUMA/GPU topology scoring. Embedded in each allocator instance. |
| **Resource State Database** | MySQL 8.0 as the source of truth for committed allocations. Read replicas for analytics. Optimistic locking for concurrent placement conflict resolution. |
| **Defragmentation Engine** | Async worker that analyzes cluster fragmentation, generates migration plans, and executes them during maintenance windows. |
| **Node Agent Fleet** | Lightweight agents on every host reporting CPU/RAM/GPU/disk/network utilization, hardware health, and topology info via gRPC streaming. |

### Data Flows

**Primary flow (placement request):**
1. Client submits placement request to API Gateway.
2. Gateway authenticates, applies rate limit, routes to an Allocator instance.
3. Allocator reads candidate hosts from Cluster State Cache.
4. Placement Engine filters candidates (predicates), scores survivors, selects best host.
5. Allocator writes allocation to Resource State Database with optimistic locking.
6. On conflict (another allocator placed on same host and resources are now insufficient), retry from step 3 with updated state.
7. On success, return host assignment to client.

**Secondary flow (state synchronization):**
1. Node agents send heartbeats every 3 seconds via gRPC.
2. Heartbeat aggregator batches updates and writes to Redis Cluster.
3. Periodic reconciliation (every 60s) reads MySQL and corrects any drift in Redis.

**Tertiary flow (defragmentation):**
1. Defrag engine runs every 15 minutes, reads cluster state.
2. Identifies hosts with < 30% utilization or high fragmentation score.
3. Generates migration plan (minimize total migrations, respect anti-affinity).
4. Executes migrations during approved windows, updates both Redis and MySQL.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Physical host inventory and current capacity
CREATE TABLE hosts (
    host_id          CHAR(36) PRIMARY KEY,         -- UUID
    hostname         VARCHAR(255) NOT NULL UNIQUE,
    region           VARCHAR(32) NOT NULL,
    availability_zone VARCHAR(32) NOT NULL,
    rack_id          VARCHAR(64) NOT NULL,
    sku_type         ENUM('cpu_general','cpu_high_mem','gpu_a100','gpu_h100',
                          'storage_opt','arm_general','hpc_bare_metal','fpga') NOT NULL,
    -- Total physical capacity
    total_cpu_cores  INT NOT NULL,                 -- physical cores
    total_ram_mb     BIGINT NOT NULL,
    total_gpu_count  INT NOT NULL DEFAULT 0,
    total_gpu_mem_mb BIGINT NOT NULL DEFAULT 0,
    total_disk_gb    BIGINT NOT NULL,
    total_net_gbps   INT NOT NULL,
    -- Allocatable capacity (after system reservation)
    alloc_cpu_cores  INT NOT NULL,
    alloc_ram_mb     BIGINT NOT NULL,
    alloc_gpu_count  INT NOT NULL DEFAULT 0,
    alloc_disk_gb    BIGINT NOT NULL,
    alloc_net_gbps   INT NOT NULL,
    -- Current committed usage (sum of all placements)
    used_cpu_cores   DECIMAL(10,2) NOT NULL DEFAULT 0,
    used_ram_mb      BIGINT NOT NULL DEFAULT 0,
    used_gpu_count   INT NOT NULL DEFAULT 0,
    used_disk_gb     BIGINT NOT NULL DEFAULT 0,
    used_net_gbps    DECIMAL(10,2) NOT NULL DEFAULT 0,
    -- Overcommit ratios
    cpu_overcommit   DECIMAL(4,2) NOT NULL DEFAULT 5.00,
    ram_overcommit   DECIMAL(4,2) NOT NULL DEFAULT 1.20,
    -- NUMA topology (JSON: array of NUMA nodes with their resources)
    numa_topology    JSON,
    -- GPU topology (JSON: NVLink domains, PCIe topology)
    gpu_topology     JSON,
    -- State
    status           ENUM('available','cordoned','draining','maintenance','decommissioned') NOT NULL DEFAULT 'available',
    -- Labels and taints (k8s-style)
    labels           JSON NOT NULL DEFAULT '{}',
    taints           JSON NOT NULL DEFAULT '[]',
    -- Versioning for optimistic locking
    version          BIGINT NOT NULL DEFAULT 1,
    last_heartbeat   TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_hosts_az_status (availability_zone, status),
    INDEX idx_hosts_sku_status (sku_type, status),
    INDEX idx_hosts_region (region),
    INDEX idx_hosts_rack (rack_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Individual workload placements
CREATE TABLE placements (
    placement_id     CHAR(36) PRIMARY KEY,
    workload_id      CHAR(36) NOT NULL,
    workload_type    ENUM('vm','container','bare_metal','batch_job') NOT NULL,
    host_id          CHAR(36) NOT NULL,
    -- Requested resources
    req_cpu_cores    DECIMAL(10,2) NOT NULL,
    req_ram_mb       BIGINT NOT NULL,
    req_gpu_count    INT NOT NULL DEFAULT 0,
    req_disk_gb      BIGINT NOT NULL DEFAULT 0,
    req_net_gbps     DECIMAL(10,2) NOT NULL DEFAULT 0,
    -- Limits (for overcommit accounting)
    limit_cpu_cores  DECIMAL(10,2),
    limit_ram_mb     BIGINT,
    -- Scheduling constraints used
    affinity_rules   JSON,
    anti_affinity_rules JSON,
    tolerations      JSON,
    numa_pinning     JSON,                         -- specific NUMA node assignment
    gpu_assignment   JSON,                         -- specific GPU device IDs
    -- Priority
    priority_class   ENUM('system_critical','high','medium','low','preemptible') NOT NULL DEFAULT 'medium',
    preemptible      BOOLEAN NOT NULL DEFAULT FALSE,
    -- State
    status           ENUM('pending','scheduled','running','evicting','terminated') NOT NULL DEFAULT 'pending',
    scheduled_at     TIMESTAMP(3),
    terminated_at    TIMESTAMP(3),
    termination_reason VARCHAR(255),
    -- Metadata
    tenant_id        CHAR(36) NOT NULL,
    namespace        VARCHAR(255),
    labels           JSON NOT NULL DEFAULT '{}',
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (host_id) REFERENCES hosts(host_id),
    INDEX idx_placements_host_status (host_id, status),
    INDEX idx_placements_workload (workload_id),
    INDEX idx_placements_tenant (tenant_id, status),
    INDEX idx_placements_priority (priority_class, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Placement history for analytics and auditing
CREATE TABLE placement_history (
    id               BIGINT AUTO_INCREMENT PRIMARY KEY,
    placement_id     CHAR(36) NOT NULL,
    host_id          CHAR(36) NOT NULL,
    action           ENUM('placed','migrated','evicted','terminated','defrag_moved') NOT NULL,
    reason           VARCHAR(512),
    prev_host_id     CHAR(36),
    duration_ms      INT,                          -- time the placement decision took
    algorithm_used   VARCHAR(64),
    score            DECIMAL(10,4),
    candidates_evaluated INT,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    INDEX idx_history_placement (placement_id),
    INDEX idx_history_host (host_id),
    INDEX idx_history_time (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Defragmentation plans
CREATE TABLE defrag_plans (
    plan_id          CHAR(36) PRIMARY KEY,
    status           ENUM('proposed','approved','executing','completed','failed','cancelled') NOT NULL DEFAULT 'proposed',
    migrations       JSON NOT NULL,                -- array of {placement_id, from_host, to_host}
    migration_count  INT NOT NULL,
    estimated_duration_sec INT NOT NULL,
    fragmentation_before DECIMAL(5,2) NOT NULL,
    fragmentation_after  DECIMAL(5,2) NOT NULL,    -- projected
    fragmentation_actual DECIMAL(5,2),             -- measured after completion
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    started_at       TIMESTAMP(3),
    completed_at     TIMESTAMP(3),
    INDEX idx_defrag_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Criteria | MySQL 8.0 | PostgreSQL 15 | CockroachDB | etcd |
|----------|-----------|---------------|-------------|------|
| **ACID transactions** | Yes | Yes | Yes (distributed) | Limited (key-value) |
| **Read performance at scale** | Excellent with read replicas | Excellent | Good (distributed reads) | Poor for range queries |
| **Operational maturity** | 25+ years, well-understood | Very mature | Newer, growing | Mature for k8s use |
| **JSON support** | Good (8.0+) | Excellent | Good | N/A |
| **Horizontal write scaling** | Limited (sharding manual) | Limited | Native | Raft-based, limited throughput |
| **Optimistic locking** | Simple (version column) | Simple | Simple | Revision-based |
| **Team familiarity** | High (per role requirements) | Medium | Low | Medium |
| **Tooling ecosystem** | Extensive | Extensive | Growing | Limited |

**Selected: MySQL 8.0** with read replicas.

**Justification:** The role specifically calls out MySQL expertise. The active dataset is small (~1.2 GB) and fits comfortably in a single MySQL primary with read replicas. Write throughput of 5,000 placements/sec is achievable with connection pooling, batched commits, and appropriate indexing. The version column pattern provides clean optimistic locking. For the hot path, we read from Redis; MySQL serves as the source of truth and handles conflict resolution.

### Indexing Strategy

| Index | Table | Purpose | Type |
|-------|-------|---------|------|
| `idx_hosts_az_status` | hosts | Filter by AZ during placement (most common filter) | Composite B-tree |
| `idx_hosts_sku_status` | hosts | Filter by SKU type for GPU/specialized workloads | Composite B-tree |
| `idx_hosts_rack` | hosts | Anti-affinity: avoid placing on same rack | B-tree |
| `idx_placements_host_status` | placements | Count active placements per host | Composite B-tree |
| `idx_placements_tenant` | placements | Tenant-level quota enforcement queries | Composite B-tree |
| `idx_placements_priority` | placements | Find preemptible workloads during eviction | Composite B-tree |
| `idx_history_time` | placement_history | Time-range queries for analytics | B-tree |

---

## 5. API Design

### REST/gRPC Endpoints

#### gRPC Service Definition (primary — low latency path)

```protobuf
service ComputeResourceAllocator {
    // Synchronous single placement
    rpc PlaceWorkload(PlaceWorkloadRequest) returns (PlaceWorkloadResponse);
    
    // Batch placement (gang scheduling)
    rpc PlaceWorkloadBatch(PlaceWorkloadBatchRequest) returns (PlaceWorkloadBatchResponse);
    
    // Dry run — returns scored candidates without committing
    rpc DryRunPlacement(PlaceWorkloadRequest) returns (DryRunResponse);
    
    // Release a placement
    rpc ReleasePlacement(ReleasePlacementRequest) returns (ReleasePlacementResponse);
    
    // Trigger defragmentation
    rpc TriggerDefragmentation(DefragRequest) returns (DefragResponse);
    
    // Stream host state updates (for monitoring)
    rpc StreamHostState(StreamHostStateRequest) returns (stream HostStateUpdate);
}

message PlaceWorkloadRequest {
    string idempotency_key = 1;       // Client-generated UUID for idempotent retries
    string workload_id = 2;
    WorkloadType workload_type = 3;
    ResourceRequirements resources = 4;
    SchedulingConstraints constraints = 5;
    string priority_class = 6;
    bool preemptible = 7;
    string tenant_id = 8;
    map<string, string> labels = 9;
    string auth_token = 10;           // JWT or service account token
}

message ResourceRequirements {
    double cpu_cores = 1;
    int64 ram_mb = 2;
    int32 gpu_count = 3;
    string gpu_type = 4;              // "a100", "h100"
    int64 disk_gb = 5;
    double net_gbps = 6;
    double cpu_limit = 7;             // for overcommit accounting
    int64 ram_limit_mb = 8;
    bool numa_required = 9;           // request NUMA-pinned placement
    int32 numa_nodes_needed = 10;
}

message SchedulingConstraints {
    repeated AffinityRule affinity_rules = 1;
    repeated AntiAffinityRule anti_affinity_rules = 2;
    repeated Toleration tolerations = 3;
    repeated string preferred_zones = 4;
    repeated string required_zones = 5;
    SchedulingPolicy policy = 6;       // PACK or SPREAD
    repeated string node_selectors = 7; // label selectors
}

message PlaceWorkloadResponse {
    string placement_id = 1;
    string host_id = 2;
    string hostname = 3;
    string availability_zone = 4;
    NUMAAssignment numa_assignment = 5;
    GPUAssignment gpu_assignment = 6;
    double placement_score = 7;
    int32 candidates_evaluated = 8;
    int64 latency_ms = 9;
}
```

#### REST Endpoints (management plane)

| Method | Path | Description | Auth | Rate Limit |
|--------|------|-------------|------|------------|
| POST | `/v1/placements` | Create a new placement | JWT (service) | 1,000/min/tenant |
| POST | `/v1/placements/batch` | Gang placement | JWT (service) | 100/min/tenant |
| POST | `/v1/placements/dry-run` | Dry-run placement | JWT (service) | 5,000/min/tenant |
| DELETE | `/v1/placements/{id}` | Release placement | JWT (service) | 1,000/min/tenant |
| GET | `/v1/placements/{id}` | Get placement details | JWT (service/user) | 10,000/min/tenant |
| GET | `/v1/hosts` | List hosts (filtered) | JWT (admin) | 100/min |
| GET | `/v1/hosts/{id}` | Host details + allocations | JWT (admin) | 1,000/min |
| POST | `/v1/hosts/{id}/cordon` | Cordon host | JWT (admin) | 50/min |
| POST | `/v1/hosts/{id}/drain` | Drain host | JWT (admin) | 50/min |
| POST | `/v1/defrag/trigger` | Trigger defrag analysis | JWT (admin) | 10/min |
| GET | `/v1/defrag/plans/{id}` | Get defrag plan | JWT (admin) | 100/min |
| POST | `/v1/defrag/plans/{id}/approve` | Approve defrag plan | JWT (admin) | 10/min |

**Idempotency:** All mutating endpoints require an `Idempotency-Key` header (UUID). The allocator stores the key and result in a short-lived cache (Redis, 24h TTL). Duplicate requests return the cached result.

**Auth:** JWT tokens validated against the central IAM service. Service-to-service calls use mTLS + service account tokens.

### CLI Design

```bash
# Place a workload
infra-alloc place \
    --workload-id=job-12345 \
    --type=container \
    --cpu=4 --ram=8192 --gpu=1 --gpu-type=a100 \
    --zone=us-east-1a \
    --anti-affinity="app=web,topology=rack" \
    --policy=spread \
    --priority=high \
    --output=json

# Dry run
infra-alloc place --dry-run \
    --cpu=32 --ram=65536 --gpu=8 --gpu-type=h100 \
    --numa-required \
    --output=table

# Release
infra-alloc release --placement-id=<uuid>

# List placements on a host
infra-alloc list --host=host-abc123 --status=running

# Host operations
infra-alloc host cordon host-abc123 --reason="hardware maintenance"
infra-alloc host drain host-abc123 --grace-period=300s --respect-pdb

# Defragmentation
infra-alloc defrag analyze --region=us-east-1 --min-improvement=5
infra-alloc defrag approve --plan-id=<uuid>
infra-alloc defrag status --plan-id=<uuid>

# Cluster overview
infra-alloc cluster stats --region=us-east-1 --by=sku
infra-alloc cluster fragmentation --threshold=20
```

---

## 6. Core Component Deep Dives

### 6.1 Multi-Dimensional Bin Packing Engine

**Why it's hard:**
Bin packing is NP-hard even in one dimension. We pack across 5+ dimensions simultaneously (CPU, RAM, GPU, disk, network). The optimal solution is computationally infeasible at scale. We need approximation algorithms that run in < 50 ms for a single placement across 50,000 hosts, while achieving > 85% of optimal utilization.

**Approaches Compared:**

| Approach | Approximation Ratio | Time Complexity | Multi-dim Support | Implementation Complexity |
|----------|--------------------:|-----------------|-------------------|---------------------------|
| First Fit (FF) | ~1.7x optimal | O(n) | Poor — unbalanced | Low |
| First Fit Decreasing (FFD) | ~1.22x optimal | O(n log n) | Moderate | Low |
| Best Fit Decreasing (BFD) | ~1.15x optimal | O(n log n) | Good — minimizes residual | Medium |
| Score-based (k8s style) | Tunable | O(n) per placement | Excellent — weighted multi-dim | Medium |
| Dot-product scoring | Near-optimal alignment | O(n) per placement | Excellent — dimension correlation | Medium |
| Constraint programming (CP-SAT) | Optimal (given time) | Exponential (bounded) | Excellent | High |
| Reinforcement Learning | Learned heuristic | O(1) inference | Excellent | Very High |

**Selected: Score-based with Dot-product alignment (hybrid).**

**Justification:** The Kubernetes-style filter-then-score pipeline is proven at scale (10,000+ nodes in production k8s clusters). We enhance the scoring phase with dot-product scoring: we compute the dot product of the normalized resource request vector and the normalized available resource vector on each host. This favors hosts where the available resource "shape" matches the request shape, reducing fragmentation. Google's Borg uses a similar vector-based scoring approach.

**Scoring calculation:**
```
For host h with available resources A = [cpu_avail, ram_avail, gpu_avail, disk_avail, net_avail]
For request R = [cpu_req, ram_req, gpu_req, disk_req, net_req]

Normalize: A_norm = A / ||A||, R_norm = R / ||R||
alignment_score = dot(A_norm, R_norm)   // 0 to 1, higher = better shape match
utilization_score = ||R|| / ||A||       // higher = tighter fit (pack policy)
spread_score = 1 - utilization_score    // higher = more headroom (spread policy)

final_score = w1 * alignment_score 
            + w2 * (pack ? utilization_score : spread_score) 
            + w3 * affinity_score 
            + w4 * numa_score 
            + w5 * gpu_topology_score
```

Typical weights: w1=0.35, w2=0.30, w3=0.15, w4=0.10, w5=0.10 (tunable per cluster).

**Implementation (pseudocode):**

```python
class PlacementEngine:
    def place(self, request: PlaceRequest, cluster_state: ClusterState) -> PlacementResult:
        # Phase 1: FILTER (predicates)
        candidates = []
        for host in cluster_state.available_hosts():
            if not self._passes_predicates(host, request):
                continue
            candidates.append(host)
        
        if not candidates:
            raise NoFeasibleHostError(request.workload_id)
        
        # Optimization: if > 500 candidates, sample 500 randomly
        # (k8s uses percentageOfNodesToScore, default ~50%)
        if len(candidates) > 500:
            candidates = random.sample(candidates, 500)
        
        # Phase 2: SCORE (priorities)
        scored = []
        for host in candidates:
            score = self._compute_score(host, request)
            scored.append((host, score))
        
        scored.sort(key=lambda x: x[1], reverse=True)
        
        # Phase 3: BIND (commit with optimistic locking)
        for host, score in scored[:3]:  # try top 3 candidates
            try:
                placement = self._try_bind(host, request, score)
                return placement
            except OptimisticLockConflict:
                continue  # host was claimed by another allocator
        
        raise PlacementConflictError("All top candidates had conflicts, retry")
    
    def _passes_predicates(self, host: Host, req: PlaceRequest) -> bool:
        # 1. Resource sufficiency (with overcommit)
        effective_cpu = host.alloc_cpu_cores * host.cpu_overcommit
        if host.used_cpu_cores + req.cpu_cores > effective_cpu:
            return False
        effective_ram = host.alloc_ram_mb * host.ram_overcommit
        if host.used_ram_mb + req.ram_mb > effective_ram:
            return False
        if host.used_gpu_count + req.gpu_count > host.alloc_gpu_count:
            return False  # GPU never overcommitted
        
        # 2. Taint/toleration matching
        for taint in host.taints:
            if not req.tolerations_match(taint):
                return False
        
        # 3. Node selector / label matching
        if not host.labels_match(req.node_selectors):
            return False
        
        # 4. Zone constraint
        if req.required_zones and host.availability_zone not in req.required_zones:
            return False
        
        # 5. Anti-affinity check
        for rule in req.anti_affinity_rules:
            if self._violates_anti_affinity(host, rule):
                return False
        
        # 6. NUMA feasibility (if required)
        if req.numa_required:
            if not self._numa_feasible(host, req):
                return False
        
        # 7. GPU type match
        if req.gpu_count > 0 and host.gpu_type != req.gpu_type:
            return False
        
        return True
    
    def _compute_score(self, host: Host, req: PlaceRequest) -> float:
        avail = np.array([
            host.alloc_cpu_cores * host.cpu_overcommit - host.used_cpu_cores,
            (host.alloc_ram_mb * host.ram_overcommit - host.used_ram_mb) / 1024,
            host.alloc_gpu_count - host.used_gpu_count,
            host.alloc_disk_gb - host.used_disk_gb,
            host.alloc_net_gbps - host.used_net_gbps
        ], dtype=float)
        
        demand = np.array([
            req.cpu_cores,
            req.ram_mb / 1024,
            req.gpu_count,
            req.disk_gb,
            req.net_gbps
        ], dtype=float)
        
        # Dot-product alignment score
        a_norm = avail / (np.linalg.norm(avail) + 1e-9)
        d_norm = demand / (np.linalg.norm(demand) + 1e-9)
        alignment = np.dot(a_norm, d_norm)
        
        # Utilization score
        util = np.linalg.norm(demand) / (np.linalg.norm(avail) + 1e-9)
        util = min(util, 1.0)
        
        if req.policy == PACK:
            fit_score = util
        else:
            fit_score = 1.0 - util
        
        # Affinity bonus
        affinity = self._affinity_score(host, req)
        
        # NUMA score
        numa = self._numa_score(host, req) if req.numa_required else 0.5
        
        # GPU topology score
        gpu_topo = self._gpu_topology_score(host, req) if req.gpu_count > 0 else 0.5
        
        return (0.35 * alignment + 0.30 * fit_score + 
                0.15 * affinity + 0.10 * numa + 0.10 * gpu_topo)
    
    def _try_bind(self, host: Host, req: PlaceRequest, score: float) -> Placement:
        # Optimistic lock: read current version, attempt update
        with db.transaction():
            current = db.query(
                "SELECT version, used_cpu_cores, used_ram_mb, used_gpu_count "
                "FROM hosts WHERE host_id = %s FOR UPDATE", host.host_id)
            
            # Re-verify resources under lock
            if current.used_cpu_cores + req.cpu_cores > host.effective_cpu:
                raise OptimisticLockConflict()
            
            db.execute(
                "UPDATE hosts SET used_cpu_cores = used_cpu_cores + %s, "
                "used_ram_mb = used_ram_mb + %s, used_gpu_count = used_gpu_count + %s, "
                "version = version + 1 WHERE host_id = %s AND version = %s",
                req.cpu_cores, req.ram_mb, req.gpu_count, host.host_id, current.version)
            
            placement = Placement(
                placement_id=uuid4(), workload_id=req.workload_id,
                host_id=host.host_id, score=score)
            db.insert("placements", placement)
            
            # Update Redis cache asynchronously
            cache.update_host_usage(host.host_id, req)
            
            return placement
```

**Failure Modes:**
1. **Stale cache reads:** Allocator reads Redis, host appears to have capacity, but MySQL shows it's full. Mitigation: optimistic locking in bind phase catches this. Retry with next candidate.
2. **Thundering herd:** Burst of placement requests all target the same "best" host. Mitigation: add jitter to scoring (small random perturbation) and try top-3 candidates.
3. **No feasible host:** All hosts filtered out. Mitigation: return clear error with breakdown of which predicate eliminated each host (for debugging). Optionally trigger cluster autoscaler.
4. **NUMA/GPU topology mismatch:** Cache doesn't reflect recent NUMA assignments. Mitigation: NUMA state is always read from MySQL during bind, not from cache.

**Interviewer Q&As:**

**Q1: Why not use an optimal solver like CP-SAT for placement?**
A: For online placement (one-at-a-time), CP-SAT is overkill — our heuristic runs in <50ms vs seconds for CP-SAT. However, we do use CP-SAT in the defragmentation engine where we batch-optimize offline and can tolerate 30-second solve times.

**Q2: How does the dot-product scoring reduce fragmentation?**
A: If a host has lots of free CPU but little free RAM, its available-resource vector "points" in the CPU direction. A CPU-heavy workload's demand vector also points in the CPU direction, so the dot product is high. This naturally steers CPU-heavy workloads to CPU-rich hosts, preventing the situation where every host runs out of RAM while having idle CPU.

**Q3: What happens if the Redis cache is completely down?**
A: We fall back to reading directly from MySQL read replicas. Latency increases from ~30ms to ~100ms for placement, but correctness is maintained. The bind phase always goes through MySQL anyway.

**Q4: How do you handle GPU placement when workloads need multiple GPUs connected via NVLink?**
A: We read the GPU topology from the host's `gpu_topology` JSON field, which encodes NVLink domains (e.g., GPUs 0-3 on NVSwitch A, GPUs 4-7 on NVSwitch B). The predicate checks that enough GPUs are free within a single NVLink domain. The scorer prefers assignments that keep the workload within the fewest NVLink hops.

**Q5: How do you prevent one tenant from monopolizing the best hosts?**
A: Three mechanisms: (1) per-tenant rate limiting at the API layer, (2) quota enforcement checked during the filter phase, (3) fair-share scoring that penalizes hosts where the requesting tenant already has many placements.

**Q6: What's the impact of the 500-candidate sampling on placement quality?**
A: Kubernetes benchmarks show that scoring 50% of nodes (down to a minimum of 100) produces placements within 5% of scoring all nodes, while cutting latency by 50%+. Our 500-sample threshold is calibrated for 50,000 hosts. We periodically A/B test different sample sizes to validate.

---

### 6.2 NUMA-Aware Placement

**Why it's hard:**
Modern servers have 2-8 NUMA nodes. Memory access latency varies 2-3x between local NUMA access (~80ns) and remote NUMA access (~150-200ns). For latency-sensitive workloads (databases, trading systems), incorrect NUMA placement can degrade throughput by 30-40%. The allocator must track per-NUMA-node resource availability and ensure workloads are pinned to the correct NUMA topology.

**Approaches Compared:**

| Approach | Performance Gain | Complexity | Flexibility |
|----------|----------------:|------------|-------------|
| No NUMA awareness | Baseline | None | Maximum |
| NUMA-preferred (best-effort) | 10-20% | Low | High |
| NUMA-strict (hard pinning) | 25-40% | High | Low |
| Automatic NUMA balancing (kernel) | 5-15% | None (kernel does it) | Medium |
| Topology Manager (k8s) | 20-35% | Medium | Medium |

**Selected: NUMA-preferred with strict mode for flagged workloads.**

**Justification:** Most workloads benefit from NUMA locality but don't require strict pinning. We use NUMA-preferred by default (scheduler tries to place on a single NUMA node but allows spilling). For workloads with `numa_required=true`, we enforce strict pinning — the placement fails if a single NUMA node can't satisfy the request.

**Implementation detail:**

```python
def _numa_feasible(self, host: Host, req: PlaceRequest) -> bool:
    """Check if any NUMA node on the host can satisfy the request."""
    topology = host.numa_topology  # e.g., [{"node": 0, "cpu": 32, "ram_mb": 65536, "used_cpu": 10, ...}, ...]
    
    for node in topology:
        avail_cpu = node["cpu"] - node["used_cpu"]
        avail_ram = node["ram_mb"] - node["used_ram_mb"]
        if avail_cpu >= req.cpu_cores and avail_ram >= req.ram_mb:
            return True
    
    # If multi-NUMA is allowed and needed:
    if req.numa_nodes_needed > 1:
        # Find best combination of N NUMA nodes
        nodes_sorted = sorted(topology, 
                              key=lambda n: n["cpu"] - n["used_cpu"], reverse=True)
        selected = nodes_sorted[:req.numa_nodes_needed]
        total_cpu = sum(n["cpu"] - n["used_cpu"] for n in selected)
        total_ram = sum(n["ram_mb"] - n["used_ram_mb"] for n in selected)
        return total_cpu >= req.cpu_cores and total_ram >= req.ram_mb
    
    return False

def _numa_score(self, host: Host, req: PlaceRequest) -> float:
    """Score based on how well the workload fits in a single NUMA node."""
    best_fit = 0.0
    for node in host.numa_topology:
        avail_cpu = node["cpu"] - node["used_cpu"]
        avail_ram = node["ram_mb"] - node["used_ram_mb"]
        if avail_cpu >= req.cpu_cores and avail_ram >= req.ram_mb:
            # Tighter fit = higher score (pack within NUMA node)
            fit = (req.cpu_cores / avail_cpu + req.ram_mb / avail_ram) / 2
            best_fit = max(best_fit, fit)
    return best_fit
```

**Failure Modes:**
1. **Stale NUMA state:** NUMA utilization changes between cache read and bind. Mitigation: re-read NUMA state from MySQL during bind, under row lock.
2. **NUMA fragmentation:** Each NUMA node has small fragments that individually can't satisfy requests. Mitigation: defragmentation engine specifically considers NUMA-level fragmentation.
3. **Kernel NUMA rebalancing conflicts:** Linux kernel may migrate pages between NUMA nodes, conflicting with our pinning. Mitigation: for strict mode, set `numactl --membind` and disable `numa_balancing`.

**Interviewer Q&As:**

**Q1: How do you discover NUMA topology on a new host?**
A: The node agent runs `lscpu`, `numactl --hardware`, and parses `/sys/devices/system/node/` at startup. The topology is reported in the first heartbeat and stored in the `numa_topology` JSON column.

**Q2: What if a workload needs 48 cores but each NUMA node only has 32?**
A: The workload must span 2 NUMA nodes. We select the two nodes with the best combined fit and the lowest inter-node distance (fewest hops). The `numa_nodes_needed` field handles this.

**Q3: How does NUMA awareness interact with CPU overcommit?**
A: NUMA-pinned workloads are never overcommitted — they need guaranteed physical cores on a specific NUMA node. Overcommit only applies to non-NUMA workloads where the kernel scheduler can time-share cores.

**Q4: How do you handle NUMA on ARM servers where the topology may differ?**
A: The topology discovery is hardware-agnostic — we read from sysfs regardless of architecture. ARM (e.g., Graviton 3) often has more NUMA nodes with fewer cores each, so the allocator naturally adapts by allowing more multi-NUMA placements.

**Q5: What's the performance impact of getting NUMA wrong?**
A: In benchmarks, a MySQL instance placed on a remote NUMA node shows 30% lower QPS and 40% higher p99 latency compared to local NUMA placement. For Redis, the impact is even larger (up to 50% throughput loss) because it's more memory-bandwidth sensitive.

**Q6: How do you handle live migration with NUMA pinning?**
A: During live migration, the target host must have a NUMA node with sufficient resources. The migration engine uses the same NUMA feasibility check. Post-migration, the workload is re-pinned to the destination NUMA node. If no suitable NUMA node exists on any target host, migration is deferred.

---

### 6.3 Defragmentation Engine

**Why it's hard:**
Over time, heterogeneous placements and terminations leave "holes" in host resource allocation — a host may have 8 free CPU cores but only 2 GB free RAM, making it useless for most workloads. Defragmentation requires moving running workloads (live migration for VMs, reschedule for containers), which has cost (downtime risk, network bandwidth, performance degradation during migration). The engine must find the minimum set of migrations that maximizes the freed capacity, while respecting all scheduling constraints (anti-affinity, NUMA, etc.).

**Approaches Compared:**

| Approach | Optimality | Latency | Disruption | Complexity |
|----------|-----------|---------|------------|------------|
| Greedy (move from least-utilized hosts) | Low | Fast (seconds) | Medium | Low |
| First Fit Decreasing repack | Medium | Moderate (seconds) | High | Medium |
| Integer Linear Programming (ILP) | Optimal | Slow (minutes) | Minimum | High |
| Constraint Programming (CP-SAT) | Near-optimal | Moderate (10-30s) | Low | High |
| Simulated Annealing | Good | Moderate (10-60s) | Medium | Medium |

**Selected: CP-SAT with greedy fallback.**

**Justification:** Google's OR-Tools CP-SAT solver can handle 50,000 hosts with a 30-second time limit, producing near-optimal migration plans. For urgent defrag (e.g., host hardware failure imminent), we fall back to the greedy approach for sub-second plan generation.

**Implementation detail:**

```python
from ortools.sat.python import cp_model

class DefragEngine:
    def generate_plan(self, cluster_state: ClusterState, 
                      target_hosts: List[Host] = None) -> DefragPlan:
        model = cp_model.CpModel()
        
        # Identify candidate workloads to move (on fragmented hosts)
        fragmented_hosts = self._find_fragmented_hosts(cluster_state)
        movable_workloads = []
        for host in fragmented_hosts:
            for placement in host.active_placements:
                if placement.priority_class != 'system_critical':
                    movable_workloads.append(placement)
        
        # Target hosts: all available hosts
        targets = [h for h in cluster_state.available_hosts() 
                   if h.status == 'available']
        
        # Decision variables: x[w][h] = 1 if workload w is placed on host h
        x = {}
        for w in movable_workloads:
            for h in targets:
                x[w.id, h.host_id] = model.NewBoolVar(f'x_{w.id}_{h.host_id}')
        
        # Constraint: each workload placed on exactly one host
        for w in movable_workloads:
            model.Add(sum(x[w.id, h.host_id] for h in targets) == 1)
        
        # Constraint: host capacity not exceeded
        for h in targets:
            model.Add(
                sum(x[w.id, h.host_id] * int(w.req_cpu_cores * 100) 
                    for w in movable_workloads)
                <= int(h.effective_cpu * 100))
            model.Add(
                sum(x[w.id, h.host_id] * w.req_ram_mb 
                    for w in movable_workloads)
                <= h.effective_ram)
        
        # Constraint: anti-affinity respected
        # ... (omitted for brevity, adds pairwise exclusion constraints)
        
        # Objective: minimize number of migrations
        migrations = []
        for w in movable_workloads:
            current_host = w.host_id
            moved = model.NewBoolVar(f'moved_{w.id}')
            model.Add(x[w.id, current_host] == 0).OnlyEnforceIf(moved)
            model.Add(x[w.id, current_host] == 1).OnlyEnforceIf(moved.Not())
            migrations.append(moved)
        
        # Secondary objective: maximize hosts that become empty (can be powered off)
        empty_hosts = []
        for h in fragmented_hosts:
            is_empty = model.NewBoolVar(f'empty_{h.host_id}')
            model.Add(
                sum(x[w.id, h.host_id] for w in movable_workloads 
                    if (w.id, h.host_id) in x) == 0
            ).OnlyEnforceIf(is_empty)
            empty_hosts.append(is_empty)
        
        # Multi-objective: minimize migrations, maximize freed hosts
        model.Minimize(
            sum(migrations) * 100 - sum(empty_hosts) * 1000)
        
        solver = cp_model.CpSolver()
        solver.parameters.max_time_in_seconds = 30.0
        status = solver.Solve(model)
        
        if status in (cp_model.OPTIMAL, cp_model.FEASIBLE):
            return self._extract_plan(solver, x, movable_workloads, targets)
        else:
            # Fallback to greedy
            return self._greedy_defrag(fragmented_hosts, targets)
    
    def _find_fragmented_hosts(self, state: ClusterState) -> List[Host]:
        """Hosts where resources are wasted due to imbalanced usage."""
        fragmented = []
        for host in state.available_hosts():
            cpu_util = host.used_cpu_cores / (host.alloc_cpu_cores * host.cpu_overcommit)
            ram_util = host.used_ram_mb / (host.alloc_ram_mb * host.ram_overcommit)
            # Fragmentation = high variance across resource dimensions
            utils = [cpu_util, ram_util]
            if host.alloc_gpu_count > 0:
                utils.append(host.used_gpu_count / host.alloc_gpu_count)
            variance = np.var(utils)
            # Or: host is severely under-utilized
            max_util = max(utils)
            if variance > 0.15 or max_util < 0.30:
                fragmented.append(host)
        return fragmented
```

**Failure Modes:**
1. **Migration failure mid-plan:** VM live migration fails (e.g., target host OOM). Mitigation: the plan is executed as a saga — each migration is a step. Failed step triggers rollback (re-place on original host). Remaining plan steps are re-evaluated.
2. **Solver timeout:** CP-SAT doesn't find a feasible solution in 30 seconds. Mitigation: fall back to greedy. Reduce problem size by partitioning into per-rack subproblems.
3. **Cascading defrag:** Moving workloads triggers new fragmentation on target hosts. Mitigation: the solver considers the full cluster state, not just fragmented hosts.
4. **Business hours conflict:** Defrag migrations during peak traffic. Mitigation: configurable maintenance windows. Automatic backoff if cluster error rate exceeds threshold.

**Interviewer Q&As:**

**Q1: How often do you run defragmentation?**
A: We run analysis every 15 minutes but only execute plans during maintenance windows (typically 2 AM - 6 AM local time). Critical defrag (e.g., host failing) runs immediately regardless of window.

**Q2: What's the typical fragmentation ratio in a production cluster?**
A: In our model, 8-15% of capacity is typically wasted due to fragmentation. After defrag, this drops to 3-5%. The remaining 3-5% is irreducible due to granularity constraints (e.g., can't split a GPU).

**Q3: How do you handle stateful workloads during defragmentation?**
A: Stateful workloads (VMs with local disk, databases) require data migration in addition to compute migration. These are deprioritized in defrag plans unless the host is being decommissioned. When they must move, we use live storage migration (QEMU block live migration) or application-level replication failover.

**Q4: How do you measure the ROI of defragmentation?**
A: We track: (1) number of hosts freed (can be powered off = direct cost savings), (2) number of previously-unfulfillable placements now feasible, (3) cluster utilization change, (4) total migration time and any SLA impact.

**Q5: Can you defragment without live migration?**
A: Yes, for containers: we drain the host (cordon + evict pods, respecting PodDisruptionBudgets), let the scheduler re-place them. This is simpler than live migration but causes brief downtime for each workload.

**Q6: How do you prevent defrag from conflicting with ongoing placements?**
A: During defrag execution, the hosts being drained are cordoned (marked unavailable for new placements). The defrag engine holds a distributed lock (via etcd or ZooKeeper) on the target hosts to prevent concurrent allocators from placing workloads on them.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

The three-phase pipeline: **Filter → Score → Bind** (adapted from Kubernetes scheduler).

**Phase 1 — Filter (Predicates):**
1. **ResourceSufficiency:** `available >= requested` for all dimensions, accounting for overcommit.
2. **TaintToleration:** Host taints must be tolerated by the workload.
3. **NodeSelector:** Host labels must match workload's node selectors.
4. **ZoneConstraint:** Host must be in a required zone (if specified).
5. **AntiAffinity:** No co-located workloads that violate anti-affinity rules.
6. **NUMAFeasibility:** (if required) At least one NUMA node can satisfy the request.
7. **GPUType:** Host GPU type must match requested type.
8. **MaxPods:** Host hasn't reached maximum workload count.
9. **DiskPressure:** Host disk utilization < 90%.
10. **HealthCheck:** Host passed last health check within 30 seconds.

**Phase 2 — Score (Priorities):**

| Priority Function | Weight | Description |
|-------------------|--------|-------------|
| AlignmentScore | 0.35 | Dot-product of available and requested resource vectors |
| PackOrSpreadScore | 0.30 | Pack: favor tighter fit. Spread: favor more headroom |
| AffinityScore | 0.15 | Bonus for affinity matches (same zone as preferred pods) |
| NUMAScore | 0.10 | Fit within single NUMA node quality |
| GPUTopologyScore | 0.10 | NVLink domain locality for multi-GPU requests |

**Phase 3 — Bind:**
Optimistic locking write to MySQL. On conflict, retry with next-best candidate. Maximum 3 retries before returning error.

### Conflict Detection

- **Optimistic locking:** Each host row has a `version` column. Bind increments version atomically. If two allocators try to bind to the same host simultaneously, one succeeds, the other gets a version mismatch and retries.
- **Double-booking prevention:** The bind phase re-checks resource availability under `SELECT FOR UPDATE` within a transaction, ensuring no phantom reads.
- **Idempotency:** Each placement request carries an `idempotency_key`. Duplicate requests return the existing placement without re-executing.

### Queue & Priority

Priority classes (highest to lowest):

| Class | Preemption Power | Use Case |
|-------|-----------------|----------|
| `system_critical` | Cannot be preempted | kube-system components, monitoring agents |
| `high` | Can preempt `low` and `preemptible` | Production services |
| `medium` | Can preempt `preemptible` | Staging, development |
| `low` | Can preempt `preemptible` | Batch jobs |
| `preemptible` | Cannot preempt anything | Spot instances, opportunistic workloads |

When a placement request fails due to insufficient resources, and the request priority is higher than existing placements:
1. Identify preemptible victims on candidate hosts.
2. Select the minimum set of victims whose eviction frees enough resources.
3. Evict victims (graceful termination with 30-second grace period).
4. Place the higher-priority workload.

### Preemption Policy

```python
def find_preemption_victims(self, host: Host, req: PlaceRequest) -> List[Placement]:
    """Find minimum set of lower-priority placements to evict."""
    if req.priority_class in ('low', 'preemptible'):
        return []  # Cannot preempt
    
    evictable = [p for p in host.active_placements 
                 if PRIORITY_ORDER[p.priority_class] < PRIORITY_ORDER[req.priority_class]]
    
    # Sort by priority ascending (evict lowest first), then by age descending (evict newest first)
    evictable.sort(key=lambda p: (PRIORITY_ORDER[p.priority_class], -p.created_at.timestamp()))
    
    freed_cpu = 0
    freed_ram = 0
    freed_gpu = 0
    victims = []
    
    for p in evictable:
        victims.append(p)
        freed_cpu += p.req_cpu_cores
        freed_ram += p.req_ram_mb
        freed_gpu += p.req_gpu_count
        
        avail_cpu = (host.alloc_cpu_cores * host.cpu_overcommit - host.used_cpu_cores + freed_cpu)
        avail_ram = (host.alloc_ram_mb * host.ram_overcommit - host.used_ram_mb + freed_ram)
        avail_gpu = (host.alloc_gpu_count - host.used_gpu_count + freed_gpu)
        
        if (avail_cpu >= req.cpu_cores and avail_ram >= req.ram_mb 
                and avail_gpu >= req.gpu_count):
            return victims
    
    return []  # Cannot free enough even by evicting all lower-priority
```

### Starvation Prevention

1. **Age-based priority boost:** Workloads waiting in the queue for > 5 minutes get a priority boost (+1 level) every 5 minutes, up to `high`.
2. **Fair-share per tenant:** Each tenant is allocated a proportional share of cluster resources based on their quota. Tenants below their fair share get a scoring bonus.
3. **Preemption cooldown:** A host that just had a preemption cannot be preempted again for 5 minutes, preventing thrashing.
4. **Max preemption rate:** No more than 1% of cluster workloads can be in preemption state simultaneously.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Max Scale |
|-----------|-----------------|-----------|
| **Allocator instances** | Stateless, behind load balancer. Add instances linearly with request rate. | 50 instances for 5,000 req/s |
| **Heartbeat aggregator** | Partition hosts by hash(host_id) across aggregator instances. | 20 instances for 50,000 hosts |
| **Defrag engine** | Shard by region/AZ. One active worker per AZ (leader election). | 10 workers (1 per AZ) |
| **API gateway** | Standard horizontal scaling. | Auto-scaled |

### Database Scaling

| Strategy | Implementation |
|----------|---------------|
| **Read replicas** | 3 MySQL read replicas for analytics queries, placement history, and dry-run reads. |
| **Connection pooling** | ProxySQL in front of MySQL. Pool of 500 connections shared across allocator instances. |
| **Sharding (future)** | If > 200K hosts: shard `hosts` table by `region` (geographic affinity — allocator in region X only queries region X shard). Not needed at 50K hosts. |
| **Write batching** | Heartbeat updates are batched (100 updates per transaction) to reduce write amplification. |
| **Archival** | Placement history older than 90 days moved to cold storage (S3 + Athena for analytics). |

### Caching

| Layer | Technology | Data | TTL | Hit Ratio |
|-------|-----------|------|-----|-----------|
| **L1 — Allocator in-process** | Caffeine (Java) / lru_cache (Python) | Host labels, taints, SKU config | 30 s | 95% |
| **L2 — Redis Cluster** | Redis 7.0, 6 shards | Host resource state, NUMA topology | Refreshed by heartbeats (3s) | 98% |
| **L3 — MySQL read replica** | MySQL 8.0 | Full host + placement data | Source of truth | N/A |
| **Idempotency cache** | Redis | Idempotency keys → results | 24 h | 99.9% |

**Interviewer Q&As:**

**Q1: What's the bottleneck at 100,000 hosts?**
A: Redis Cluster becomes the bottleneck for heartbeat writes (33,333/sec). Solution: partition heartbeat processing by host ID range across multiple Redis clusters, or move to a purpose-built time-series store for heartbeats (e.g., InfluxDB) and use Redis only for latest-state snapshots.

**Q2: How do you handle a Redis Cluster failure?**
A: Allocators fall back to MySQL read replicas with a degraded-mode flag. Placement latency increases by ~70ms. An alert fires and the on-call engineer is paged. Redis Cluster's built-in failover (Sentinel) handles single-node failures automatically.

**Q3: Why not use etcd like Kubernetes does?**
A: etcd has a practical limit of ~8 GB data and is designed for small configuration data, not for 50,000 host records with frequent heartbeat updates. MySQL + Redis gives us better separation of concerns: Redis for hot reads, MySQL for ACID writes.

**Q4: How do you test scaling before deploying?**
A: We run load tests with synthetic hosts and placement requests. Our simulator can model 200,000 hosts and 10,000 placements/sec. We also use shadow mode: production traffic is replayed against a staging cluster.

**Q5: How do you handle multi-region placement requests?**
A: Each region has its own allocator fleet and Redis cluster. Cross-region placement is handled by a global routing layer that fans out the request to the appropriate regional allocator. Cross-region state is eventually consistent (synced every 30 seconds for capacity dashboard purposes only).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery | RTO |
|---------|--------|-----------|----------|-----|
| Single allocator instance crash | Minor — others handle load | Health check fails (5s) | LB removes instance, replacement auto-scales | 5 s |
| All allocators down | All placements fail | Zero successful placements for > 10s | PagerDuty alert, restart fleet. Placements queue in gateway. | 30 s |
| Redis Cluster partition | Stale reads, some placement retries | Redis CLUSTER INFO shows unhealthy | Sentinel failover (automatic). Allocators fall back to MySQL. | 10-30 s |
| MySQL primary failure | No new placements can commit | MySQL replication lag spikes, health check fails | Promote read replica to primary (MHA/Orchestrator). | 30-60 s |
| Node agent fleet failure (mass) | Stale host state across cluster | Heartbeat age exceeds 30s for > 10% of hosts | Alert fires. Allocators switch to "last known state" mode, increase bind-phase re-verification. | Manual investigation |
| Network partition (region) | Regional placements isolated | Cross-region health checks fail | Each region operates independently. No cross-region dependency for placement. | 0 s (degraded) |
| Heartbeat storm (50K simultaneous) | Redis write saturation | Redis latency spikes | Rate-limit heartbeats, stagger with jitter (already built in) | Self-healing |

### Automated Recovery

1. **Allocator auto-restart:** Kubernetes deployment with `restartPolicy: Always`, `minReadySeconds: 10`, `maxUnavailable: 25%`.
2. **MySQL automatic failover:** Orchestrator monitors replication topology. On primary failure, promotes the most up-to-date replica within 30 seconds.
3. **Redis Cluster auto-failover:** Redis Sentinel promotes replicas. Allocators use the Sentinel protocol for auto-discovery.
4. **Stale host eviction:** Hosts with no heartbeat for > 60 seconds are automatically cordoned. Workloads are not immediately evicted (the host may come back). After 5 minutes, workloads are marked for rescheduling.

### Retry Strategy

| Operation | Retry Policy | Max Retries | Backoff |
|-----------|-------------|-------------|---------|
| Placement (bind conflict) | Immediate retry with next candidate | 3 | None (immediate) |
| Placement (all candidates exhausted) | Return error to client | 0 | Client retries with exponential backoff (1s, 2s, 4s) |
| Heartbeat write to Redis | Retry with jitter | 2 | 100ms + random(0-100ms) |
| MySQL write | Retry on deadlock/timeout | 3 | 50ms exponential (50, 100, 200ms) |
| Defrag migration | Retry individual migration step | 2 | 5s between retries |

### Circuit Breaker

| Service | Failure Threshold | Open Duration | Half-Open Probes |
|---------|-------------------|---------------|------------------|
| MySQL writes | 5 failures in 10s | 30s | 1 probe every 5s |
| Redis reads | 10 failures in 5s | 15s | 3 probes every 3s |
| Node agent gRPC | 3 timeouts in 10s | 60s | 1 probe every 10s |
| Defrag migration API | 2 failures in 30s | 120s | 1 probe every 30s |

When MySQL circuit breaker opens: placements are queued (up to 1000, then rejected) until the circuit closes.
When Redis circuit breaker opens: allocators read from MySQL read replicas directly.

### Consensus & Coordination

- **Leader election for defrag engine:** etcd-based lease. Only one defrag engine per AZ is active at a time. Lease TTL = 30 seconds, renewed every 10 seconds.
- **Distributed locking for host drain:** etcd lock on `drain/{host_id}` prevents multiple components from draining the same host simultaneously.
- **No consensus needed for placement:** Allocators are stateless and use optimistic locking against MySQL, avoiding distributed consensus overhead on the hot path.

---

## 10. Observability

### Key Metrics

| Metric | Type | Labels | Alert Threshold |
|--------|------|--------|-----------------|
| `placement_latency_ms` | Histogram | region, az, policy, result | p99 > 200ms |
| `placement_total` | Counter | region, result(success/fail/conflict) | Error rate > 1% |
| `placement_conflict_rate` | Gauge | region | > 5% |
| `cluster_utilization_cpu` | Gauge | region, az, sku | < 50% or > 90% |
| `cluster_utilization_ram` | Gauge | region, az, sku | < 50% or > 90% |
| `cluster_utilization_gpu` | Gauge | region, az, gpu_type | > 95% |
| `cluster_fragmentation_ratio` | Gauge | region, az | > 15% |
| `hosts_available` | Gauge | region, az, sku, status | < 10% of total |
| `heartbeat_age_seconds` | Histogram | region | p99 > 10s |
| `defrag_migrations_total` | Counter | region, result | Failure > 5% |
| `redis_cache_hit_ratio` | Gauge | operation | < 90% |
| `mysql_replication_lag_ms` | Gauge | replica | > 1000ms |
| `preemption_total` | Counter | region, priority_class | Spike > 2x baseline |
| `queue_depth` | Gauge | priority_class | > 500 |

### Distributed Tracing

- **OpenTelemetry SDK** in all allocator instances.
- Trace spans: `api_receive → auth → filter → score → bind → respond`.
- Each span annotated with: `host_count_filtered`, `host_count_scored`, `selected_host`, `placement_score`, `retries`.
- Trace propagation: W3C TraceContext headers through gRPC and HTTP.
- Backend: Jaeger or Tempo, with 1% sampling for normal traffic, 100% sampling for errors.

### Logging

| Level | When | Content |
|-------|------|---------|
| INFO | Every placement | workload_id, host_selected, latency_ms, candidates_count |
| WARN | Placement retry (conflict) | workload_id, conflicting_host, retry_count |
| WARN | High fragmentation detected | region, az, fragmentation_ratio |
| ERROR | Placement failure (no host) | workload_id, filter_breakdown (why each host was rejected) |
| ERROR | MySQL/Redis connection failure | error_type, circuit_breaker_state |
| DEBUG | Scoring details | host_id, alignment_score, fit_score, affinity_score, total_score |

Structured JSON logging. Shipped to Elasticsearch via Filebeat. 30-day retention.

### Alerting

| Alert | Condition | Severity | Runbook |
|-------|-----------|----------|---------|
| HighPlacementLatency | p99 > 200ms for 5 min | P2 | Check Redis latency, MySQL slow queries |
| PlacementFailureSpike | Error rate > 5% for 2 min | P1 | Check cluster capacity, stuck hosts |
| ClusterNearCapacity | CPU or RAM utilization > 85% for any AZ | P2 | Trigger capacity planning review |
| HeartbeatStale | > 5% hosts stale > 60s | P1 | Check node agent fleet, network issues |
| MySQLReplicationLag | Lag > 5s for 2 min | P2 | Check replica health, kill long queries |
| DefragFailure | > 3 failed migrations in 1 hour | P3 | Review defrag plan, check target hosts |
| FragmentationHigh | > 20% for 1 hour | P3 | Trigger manual defrag review |

---

## 11. Security

### Auth & AuthZ

- **Authentication:** mTLS for service-to-service communication. JWT (OAuth2) for user-facing APIs. Service accounts for automated systems (CI/CD, autoscaler).
- **Authorization:** RBAC with three roles:
  - `allocator-admin`: Full access (host management, defrag, configuration).
  - `allocator-service`: Can create/release placements for their tenant. Cannot manage hosts.
  - `allocator-readonly`: Can query placements and host state. No mutations.
- **Tenant isolation:** Placement API enforces `tenant_id` from JWT claims. Tenants cannot see or affect other tenants' placements.
- **Admission control:** Placement requests are validated against tenant quotas before entering the placement pipeline.

### Secrets Management

- **Database credentials:** Rotated every 24 hours via HashiCorp Vault. Allocator instances fetch credentials at startup and refresh on rotation signal.
- **Redis auth:** ACL-based authentication (Redis 6+). Password rotated weekly.
- **mTLS certificates:** Issued by internal CA, 90-day validity, auto-renewed by cert-manager.
- **No secrets in environment variables:** All secrets fetched from Vault at runtime.

### Audit Logging

Every mutation is logged to an immutable audit trail:
- **Placement created/released:** who, when, what resources, which host.
- **Host cordoned/drained:** who initiated, reason.
- **Defrag plan approved/executed:** approver, migration details.
- **Configuration changes:** overcommit ratio changes, priority weight changes.

Audit logs stored in a separate MySQL database with append-only access. Replicated to S3 for long-term retention (7 years for compliance).

---

## 12. Incremental Rollout Strategy

**Phase 1 — Shadow Mode (Week 1-2):**
Deploy allocator alongside existing system. Both receive placement requests; only the existing system's decisions are enacted. Compare decisions for quality analysis.

**Phase 2 — Canary (Week 3-4):**
Route 5% of placement requests to new allocator. Monitor placement latency, utilization, and fragmentation metrics. Compare against the 95% handled by the existing system.

**Phase 3 — Progressive Rollout (Week 5-8):**
Increase to 25%, 50%, 75%, 100% over 4 weeks. Each step requires:
- No increase in placement failure rate.
- p99 latency within 20% of baseline.
- Cluster utilization improvement or parity.

**Phase 4 — Defrag Engine (Week 9-12):**
Enable defrag in analysis-only mode first, then execute plans on one AZ, then all AZs.

**Rollout Q&As:**

**Q1: What if the new allocator makes worse placement decisions?**
A: Shadow mode comparison catches this before any traffic is routed. Metrics compared: utilization efficiency, placement latency, fragmentation ratio, preemption rate. If any metric degrades > 5%, the rollout pauses.

**Q2: How do you handle rollback?**
A: Feature flag controls routing between old and new allocator. Rollback is instant (flip the flag). Placements already made by the new allocator remain in place — they're valid placements.

**Q3: How do you validate defragmentation before production?**
A: Run defrag in dry-run mode for 2 weeks. Compare proposed plans against manual expert review. Measure: would the plan have improved fragmentation? Would any migration have violated constraints?

**Q4: What metrics gate each rollout phase?**
A: (1) Placement success rate >= 99.5%, (2) p99 latency < 200ms, (3) Zero data inconsistencies between MySQL and Redis, (4) No unplanned preemptions, (5) Fragmentation ratio not increasing.

**Q5: How do you handle database schema migrations during rollout?**
A: Blue-green schema approach. New columns added as nullable first (backward-compatible). Old allocator ignores new columns. Once 100% traffic is on the new allocator, old columns are deprecated and eventually dropped.

**Q6: How do you test the GPU topology scoring before production?**
A: We build a simulation with realistic GPU topology data from our fleet (DGX A100 and H100 configurations). Run 10,000 synthetic placement requests and validate that multi-GPU workloads are placed within the same NVLink domain > 95% of the time.

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Risk |
|----------|-------------------|--------|-----------|------|
| State store for hot path | Redis vs Memcached vs in-process | Redis Cluster | Persistence, pub/sub for updates, cluster mode for scaling | Redis complexity, operational overhead |
| Source of truth | MySQL vs PostgreSQL vs CockroachDB | MySQL 8.0 | Role requirement, team expertise, sufficient for 50K hosts | Manual sharding if we exceed 200K hosts |
| Placement algorithm | Pure bin packing vs Score-based vs ML | Score-based with dot-product | Proven at scale (k8s), tunable, interpretable | Suboptimal vs ILP solver (~5-10% gap) |
| Defrag solver | Greedy vs ILP vs CP-SAT | CP-SAT with greedy fallback | Near-optimal with bounded time. Greedy for urgent cases | Solver library dependency (OR-Tools) |
| Overcommit | No overcommit vs Fixed ratio vs Adaptive | Fixed ratio (CPU 5:1, RAM 1.2:1) | Simple, predictable. Adaptive is future work | Noisy neighbor issues at high overcommit |
| NUMA handling | Ignore vs Preferred vs Strict | Preferred + opt-in strict | Balances utilization with performance | Fragmentation at NUMA level |
| Concurrency control | Pessimistic locking vs Optimistic locking | Optimistic with retry | Better throughput under low contention. Our conflict rate is ~2%. | Retry storms under high contention |
| GPU scheduling | Share GPUs (MIG/MPS) vs Whole GPU only | Whole GPU only (initially) | Simpler. MIG support planned for Phase 2. | Under-utilization of GPU capacity |
| Cache consistency | Strong vs Eventual | Eventual (Redis) + Strong (MySQL bind) | Hot path is fast (eventual), commit path is correct (strong) | Occasionally score a host that's actually full → retry |

---

## 14. Agentic AI Integration

### AI-Driven Placement Optimization

**1. Learned Scoring Function:**
Train a neural network on historical placement data to predict workload-host affinity. Features: workload resource profile, host current utilization, time of day, tenant, workload type. Target: minimize probability of eviction/migration within 24 hours. Deploy as a scoring plugin that replaces or augments the dot-product scoring. Use online A/B testing to validate improvement.

**2. Intelligent Defragmentation Scheduling:**
An LLM-based agent monitors cluster state and decides *when* to trigger defragmentation, *which* hosts to include, and *how aggressive* to be. It considers: upcoming maintenance windows, predicted traffic patterns (from historical data), pending capacity additions, and current SLA status. This replaces the fixed 15-minute cron schedule with an adaptive, context-aware trigger.

**3. Anomaly-Driven Placement Adjustment:**
An agent monitors placement outcomes and detects anomalies: unusual placement failures, unexpected resource utilization patterns, hosts that consistently cause workload failures. When detected, the agent can automatically cordon suspicious hosts, adjust scoring weights, or trigger investigation workflows.

**4. Natural Language Capacity Queries:**
Operations engineers can query the allocator in natural language:
- "Why did job X fail to place?" → Agent traces through filter predicates and identifies the bottleneck.
- "What happens if we lose rack 42?" → Agent simulates the failure and reports which workloads would need rescheduling and whether capacity exists.
- "Find me 100 GPU-hours of A100 capacity in us-east-1 this week" → Agent queries reservations and current allocations to find availability windows.

**5. Predictive Overcommit Ratios:**
Instead of fixed overcommit ratios (CPU 5:1), an ML model predicts per-host safe overcommit ratios based on the actual workload mix. A host running mostly idle development environments can safely overcommit CPU at 8:1, while a host running latency-sensitive production services should use 2:1.

### Implementation Guard Rails

- AI recommendations are always validated against hard constraints (resource limits, anti-affinity) before execution.
- Human-in-the-loop for defragmentation plans affecting > 100 workloads.
- ML model predictions have confidence scores; below threshold, fall back to heuristic scoring.
- All AI decisions are logged with full reasoning chain for auditability.

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through what happens when a placement request arrives.**
A: The request hits the API gateway (TLS termination, auth, rate limit), routes to a stateless allocator instance. The allocator reads candidate hosts from Redis cache (L2), runs the filter phase (10 predicates), scores surviving candidates (dot-product alignment + policy + affinity + NUMA + GPU topology), selects the top candidate, and attempts to bind by writing to MySQL with optimistic locking. On success, Redis is updated asynchronously. On conflict, the next candidate is tried (up to 3 retries).

**Q2: How do you handle the case where the cluster is at 95% capacity?**
A: Several mechanisms: (1) Preemption of lower-priority workloads to make room for higher-priority ones. (2) Automatic defragmentation to recover stranded capacity. (3) Cluster autoscaler triggers to provision new hosts (if cloud-backed) or alert capacity planning team (if bare-metal). (4) Admission control rejects low-priority requests early, with clear error messages.

**Q3: What's the difference between affinity and anti-affinity in practice?**
A: Affinity says "place me near X" (e.g., web frontend pods prefer to be in the same zone as their API backend for low latency). Anti-affinity says "place me away from X" (e.g., database replicas must be on different racks for fault tolerance). In our system, anti-affinity is a hard predicate (filter phase), while affinity is a soft priority (scoring phase) unless marked as required.

**Q4: How does resource overcommit work and what are the risks?**
A: CPU overcommit at 5:1 means we promise 5x more CPU than physically exists, betting that not all workloads peak simultaneously. This is safe for CPU (the kernel time-shares cores with minimal penalty). RAM overcommit at 1.2:1 is much more conservative because RAM exhaustion causes OOM kills. Risks: noisy neighbors, cascading OOM if correlated workloads peak together. Mitigation: monitor actual utilization, adjust ratios per host based on workload mix.

**Q5: How would you extend this to support GPU sharing (NVIDIA MIG)?**
A: MIG partitions an A100 into up to 7 instances with isolated compute and memory. We'd extend the data model: `gpu_topology` would include MIG partition information. The filter predicate would check for available MIG slices matching the requested GPU memory profile. The scoring would prefer placing on a GPU that already has compatible MIG partitions (to avoid reconfiguring). The bind phase would commit a specific MIG slice assignment.

**Q6: How do you prevent a single tenant from filling an entire rack?**
A: Three layers: (1) Quota system limits how many resources a tenant can consume (separate system). (2) Anti-affinity rules can require spread across racks. (3) Fair-share scoring penalizes concentration — if tenant A already has 50% of a rack's workloads, the score for placing another tenant A workload on that rack is reduced.

**Q7: What happens during a rolling update of the allocator service itself?**
A: Rolling update with `maxUnavailable=25%`. In-flight placement requests complete before the pod terminates (graceful shutdown with 30-second drain period). New pods are added to the load balancer only after passing readiness checks. The Redis cache and MySQL state are external, so no state is lost.

**Q8: How do you handle bare-metal provisioning differently from VM/container placement?**
A: Bare-metal placement is the same algorithm but with additional constraints: (1) entire host is allocated (no sharing), (2) provisioning time is minutes (PXE boot, OS install) vs seconds for containers, (3) IPMI/BMC credentials must be provisioned, (4) network switch port configuration may be needed. The placement is synchronous but the provisioning is async with a callback.

**Q9: How would you implement gang scheduling for distributed ML training?**
A: Gang scheduling requires all N pods to be placed simultaneously or none. We'd use the batch placement API: the allocator tries to place all N pods atomically. If any pod can't be placed, all are rejected. This requires either holding locks on N hosts simultaneously (expensive) or using a two-phase commit: tentatively reserve resources on N hosts, then commit all at once. We use the CP-SAT solver to find a feasible assignment for all N pods simultaneously.

**Q10: How do you handle the "standing on each other's toes" problem with multiple allocator instances?**
A: Our conflict rate is ~2% under normal load. Optimistic locking means conflicts are detected and retried transparently. To reduce conflicts: (1) Each allocator adds small random noise to scores, naturally spreading placements across different hosts. (2) We can partition the host space — allocator instance 1 primarily scores hosts 1-10000, instance 2 scores 10001-20000, etc. — with fallback to the full set if the partition is full.

**Q11: What's the fragmentation problem and how do you measure it?**
A: Fragmentation occurs when total cluster resources are sufficient for a workload but no single host can satisfy it. Example: 100 hosts each have 4 free CPU cores and 1 GB free RAM. A workload needing 2 CPU + 8 GB RAM can't be placed despite 400 free cores and 100 GB free RAM. We measure fragmentation as: `1 - (actual_placeable_workloads / theoretical_placeable_workloads)`. "Theoretical" assumes resources can be freely redistributed across hosts.

**Q12: How would you handle spot/preemptible instance pricing?**
A: Preemptible workloads fill otherwise-idle capacity at a discount. When a non-preemptible workload needs the capacity, preemptible workloads get a 30-second termination notice. We track preemptible vs reserved utilization separately. The allocator preferentially places preemptible workloads on hosts that are likely to remain under-utilized (based on historical patterns), reducing churn.

**Q13: How do you ensure the Redis cache doesn't serve dangerously stale data?**
A: Three safeguards: (1) Heartbeats refresh host state every 3 seconds — a host's cache entry is rarely > 3 seconds stale. (2) Every host cache entry has a timestamp; allocators skip hosts with stale-time > 30 seconds. (3) The bind phase always verifies against MySQL (source of truth), catching any staleness-induced bad decisions.

**Q14: How would you implement resource overcommit monitoring and automatic adjustment?**
A: Instrument every host with actual utilization metrics (cgroups for containers, hypervisor metrics for VMs). Compare actual peak utilization to committed resources. If actual peak / committed > 0.8, the overcommit ratio is too aggressive — automatically reduce it for that host. If actual peak / committed < 0.3, the ratio can be increased. Adjustments happen weekly, with human review for changes > 20%.

**Q15: Why not use a distributed database like CockroachDB to avoid the single MySQL primary bottleneck?**
A: At 50,000 hosts and 5,000 placements/sec, a single MySQL primary with connection pooling handles the write load comfortably (each write is ~0.2ms). CockroachDB's distributed transactions add latency (~5-10ms per write due to Raft consensus). The simplicity and predictability of MySQL outweighs the scaling benefits of CockroachDB at our current scale. If we grow to 500,000 hosts, we'd revisit this decision.

**Q16: How do you handle the "noisy neighbor" problem?**
A: Prevention: NUMA pinning for sensitive workloads isolates memory bandwidth. CPU pinning (cgroups cpuset) prevents CPU contention. Network bandwidth limits via tc (traffic control). Detection: monitor per-workload performance metrics; if a workload's latency degrades while its own CPU usage hasn't increased, a noisy neighbor is likely. Remediation: identify the offending workload via host-level metrics, live-migrate the victim or the offender to a different host.

---

## 16. References

1. Verma, A., et al. "Large-scale cluster management at Google with Borg." *EuroSys 2015*.
2. Kubernetes Scheduler documentation: https://kubernetes.io/docs/concepts/scheduling-eviction/kube-scheduler/
3. Grandl, R., et al. "Multi-resource packing for cluster schedulers." *SIGCOMM 2014* (Tetris paper).
4. Ghodsi, A., et al. "Dominant Resource Fairness: Fair Allocation of Multiple Resource Types." *NSDI 2011*.
5. Google OR-Tools CP-SAT Solver: https://developers.google.com/optimization
6. Coffman, E.G., et al. "Bin Packing Approximation Algorithms: Survey and Classification." *Handbook of Combinatorial Optimization, 2013*.
7. NUMA-aware scheduling in Linux: https://www.kernel.org/doc/html/latest/admin-guide/mm/numa_memory_policy.html
8. NVIDIA NVLink and NVSwitch documentation: https://www.nvidia.com/en-us/data-center/nvlink/
9. Kubernetes Topology Manager: https://kubernetes.io/docs/tasks/administer-cluster/topology-manager/
10. Burns, B., et al. "Design Patterns for Container-based Distributed Systems." *USENIX HotCloud 2016*.
