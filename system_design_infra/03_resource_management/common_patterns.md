# Common Patterns — Resource Management (03_resource_management)

## Common Components

### MySQL as Source of Truth for Resource State
- All 5 systems use MySQL 8.0 as the authoritative store for structured resource data; Redis holds the fast-path cache
- capacity_planning_system: `capacity_snapshots`, `capacity_forecasts`, `fleet_inventory`, `procurement_orders`
- cluster_resource_manager: `jobs`, `scheduling_queues`, `tasks`, `nodes`
- compute_resource_allocator: `hosts`, `workload_placements`
- quota_and_limit_enforcement: `quota_definitions`, `quota_usage_snapshots`, `quota_requests`
- resource_reservation_system: `reservations`, `host_resource_timeline`, `recurring_reservation_templates`

### Redis for Real-Time Resource State Cache
- 4 of 5 systems use Redis for sub-millisecond reads on the hot path; MySQL is reconciled periodically
- capacity_planning_system: capacity snapshot cache, forecast cache
- cluster_resource_manager: node state cache, queue definitions (in-process), pending job counts
- compute_resource_allocator: `host:{host_id}` real-time resource snapshot (updated every 3 s via gRPC heartbeat)
- quota_and_limit_enforcement: quota usage counters (atomic INCRBY), rate limit token buckets (Lua EVALSHA)

### Multi-Dimensional Resource Tracking (CPU, RAM, GPU, Disk, Network)
- All 5 systems track the same 5 resource dimensions across the same organizational hierarchy
- capacity_planning_system: forecasts per (AZ, SKU, resource_type) = 800 forecast models (10 AZs × 8 SKUs × 10 resource types)
- cluster_resource_manager: per-queue guaranteed/max per CPU + RAM + GPU; DRF dominant resource across all 3
- compute_resource_allocator: 5D bin-packing with overcommit ratios (CPU 5:1, RAM 1.2:1, GPU never)
- quota_and_limit_enforcement: hard_limit + soft_limit + burst_limit per resource_type (cpu_cores, ram_mb, gpu_count, gpu_mem_mb, disk_gb)
- resource_reservation_system: per-reservation (cpu_cores, ram_mb, gpu_count, gpu_type, disk_gb, net_gbps)

### Hierarchical Org Model (org → team → project → user)
- All 5 systems structure resource ownership through the same 4-level hierarchy
- cluster_resource_manager: hierarchical queues (org → team → project) with DRF per level
- quota_and_limit_enforcement: quota_definitions.scope_type ENUM(organization, team, project, user); effective_hard_limit = min(parent_hard_limit, this_level_hard_limit)
- resource_reservation_system: reservations owned at project level
- capacity_planning_system: capacity tracked region → AZ → cluster; forecasts scoped to AZ × SKU
- compute_resource_allocator: workload placement tagged by tenant_id; rate limits enforced per tenant

### State Machine for Resource Lifecycle
- All 5 systems track resource/job/reservation lifecycle through explicit states with valid transitions only
- capacity_planning_system: procurement recommendations: proposed → approved → ordered → delivered
- cluster_resource_manager: jobs: pending → scheduling → running → completed/failed; PodGroup: waiting → ready
- quota_and_limit_enforcement: quota_requests: pending → approved/denied
- resource_reservation_system: reservations: REQUESTED → CONFIRMED → ACTIVE → COMPLETED/EXPIRED/CANCELLED

### L1 Local Cache + L2 Redis + L3 MySQL Caching Strategy
- 4 of 5 systems use a three-level cache: in-process (Caffeine or HashMap) → Redis → MySQL
- cluster_resource_manager: node state in Redis (updated on heartbeat), queue definitions in-process
- compute_resource_allocator: host state in Redis (3 s TTL), MySQL reconciled every 60 s
- quota_and_limit_enforcement: Caffeine local cache (30 s TTL) → Redis → MySQL; enables < 10 ms quota checks without DB round-trip
- resource_reservation_system: active reservation set in Redis sorted set (score = start_time); MySQL is authoritative

## Common Databases

### MySQL 8.0
- All 5; source of truth for resource definitions, utilization history, job metadata, quota definitions, reservation records; ACID transactions; optimistic locking (version column) for concurrent writes

### Redis
- 4 of 5 (all except capacity_planning uses VictoriaMetrics); real-time resource counters; atomic Lua scripts for rate limiting; timer queues (sorted sets); heartbeat tracking

### Kafka
- 2 of 5 (cluster_resource_manager, resource_reservation_system); scheduling lifecycle events; reservation billing events (reservation_created, reservation_activated, reservation_completed)

### VictoriaMetrics / Thanos (Time-Series)
- capacity_planning_system: ingests 166,667 data points/sec (50 K hosts × 50 metrics, 15 s scrape); multi-resolution downsampling 15 s → 5 min → 1 hr → 1 day

## Common Communication Patterns

### REST for Management Plane + gRPC for Data Plane
- All 5: REST APIs for human-driven management operations (< 100 concurrent users); gRPC for high-throughput service-to-service data plane
- cluster_resource_manager: gRPC for job submission + node heartbeats (streaming)
- compute_resource_allocator: gRPC for node agent heartbeats (50,000 agents streaming every 3 s)
- resource_reservation_system: gRPC for scheduler integration; REST for reservation CRUD

### Kubernetes Admission Webhook Pattern
- quota_and_limit_enforcement: ValidatingWebhookConfiguration intercepts pod CREATE/UPDATE; QES checks quota in < 10 ms; rejects if over hard_limit
- cluster_resource_manager: admission webhook validates gang scheduling PodGroup membership

## Common Scalability Techniques

### Candidate Sampling for Large Fleets
- compute_resource_allocator: filter 50,000 hosts down to candidates via predicates; if candidates > 500, sample 500 randomly for scoring (same approach as Kubernetes percentageOfNodesToScore ~50%)
- cluster_resource_manager: percentageOfNodesToScore ~50% for large clusters

### Optimistic Locking for Concurrent Placement
- compute_resource_allocator: `UPDATE hosts SET alloc_cpu = alloc_cpu + req_cpu, version = version + 1 WHERE host_id = ? AND version = ?`; retry on conflict
- resource_reservation_system: SELECT FOR UPDATE on host_resource_timeline rows during reservation creation

### Eventual Consistency with Periodic Reconciliation
- compute_resource_allocator: Redis cache updated every 3 s via heartbeat; MySQL reconciled every 60 s; temporary divergence acceptable because optimistic locking prevents double-allocation
- quota_and_limit_enforcement: Redis usage counters atomically incremented on pod create; MySQL usage snapshot written every 30 s; divergence bounded at 30 s

## Common Deep Dive Questions

### How do you prevent double-allocation when multiple schedulers race to place workloads on the same host?
Answer: The pattern is optimistic locking on the host record: read the current `version`, attempt `UPDATE hosts SET alloc_cpu = alloc_cpu + req WHERE version = read_version`. If 0 rows updated, a concurrent placement won the race — retry against the refreshed state. This avoids distributed locking overhead while providing strong consistency at the MySQL level. Redis is the fast-path filter (stale is OK for filtering), but MySQL is always the binding authority.
Present in: compute_resource_allocator, resource_reservation_system

### Why use DRF instead of simple FIFO or priority queuing?
Answer: FIFO starves long-running small jobs when large jobs dominate. Simple priority-based scheduling requires operators to manually assign priorities and leads to low-priority jobs never running. DRF (Dominant Resource Fairness) automatically computes each queue's dominant resource share (highest utilization %) and schedules the queue with the lowest dominant share next — ensuring every team gets a fair slice of whichever resource they care about most, without requiring manual priority tuning. It generalizes Max-Min fairness to multiple resources simultaneously.
Present in: cluster_resource_manager

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.99% for scheduling/placement/quota enforcement; 99.9% for capacity planning (advisory) |
| **p99 Latency** | < 10 ms (quota admission), < 200 ms (placement), < 500 ms (pod scheduling), < 2 s (dashboard) |
| **Throughput** | 166 K metrics/s (capacity), 10 K pods/s (cluster), 5 K placements/s (compute), 20 K checks/s (quota), 1 K reservations/s |
| **Consistency** | Strong at admission time (MySQL); eventual for monitoring/dashboards (Redis cache) |
| **Data Retention** | 5 years aggregated (capacity planning), 1 year history (quota, reservations), 30 days (scheduling history) |
