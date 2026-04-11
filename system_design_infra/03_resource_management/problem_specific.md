# Problem-Specific Design — Resource Management (03_resource_management)

## Capacity Planning System

### Unique Functional Requirements
- Forecast demand using ensemble of time-series models (linear regression, ARIMA, Prophet) with < 15% MAPE for 30-day forecasts
- Procurement lead-time awareness: bare-metal 12–24 weeks, cloud VMs minutes — generate purchase orders with correct lead time
- 20% headroom buffer target; alert when buffer drops below threshold
- Scenario simulation: what-if analysis (demand growth %, zone failure, hardware addition) with < 30 s response time per scenario
- Dashboard drill-down: global → region → AZ → cluster → host; P99 < 2 s

### Unique Components / Services
- **Metrics Aggregation Pipeline**: Prometheus remote-write → VictoriaMetrics/Thanos; 50,000 hosts × 50 metrics = 166,667 data points/sec; downsamples 15 s → 5 min → 1 hr → 1 day; raw retained 14 days, 5-min 90 days, 1-hr 2 years, 1-day 5 years
- **Forecasting Engine**: 800 models (10 AZs × 8 SKUs × 10 resource types); trains Prophet + ARIMA + linear regression + ensemble; produces 30/60/90-day forecasts with confidence intervals; target MAPE < 15% for 30-day
- **Scenario Simulator**: what-if analysis with < 30 s latency; simulates demand growth %, zone failure, hardware addition; reads from capacity_snapshots + capacity_forecasts
- **Procurement Recommender**: generates purchase orders with lead-time awareness; 12–24 weeks for bare-metal, minutes for cloud VMs; integrates vendor SKU catalog
- **Cost Modeler**: on-demand vs reserved instances vs spot vs bare-metal CapEx/OpEx TCO comparison

### Unique Data Model
- **capacity_snapshots**: snapshot_id UUID, timestamp TIMESTAMP(3), region, availability_zone, sku_type ENUM(8), resource_type ENUM(cpu,ram,gpu,disk,network), total_capacity BIGINT, used_capacity BIGINT, utilization DECIMAL(5,2), saturation DECIMAL(5,2); `INDEX idx_snap_time_az_sku (timestamp, availability_zone, sku_type)`
- **capacity_forecasts**: forecast_id UUID, run_timestamp, availability_zone, sku_type, resource_type, forecast_date DATE, forecast_value BIGINT, confidence_lower/upper BIGINT, days_until_exhaustion INT; `INDEX idx_forecast_az_sku (availability_zone, sku_type, resource_type, forecast_date)`
- **fleet_inventory**: host_id UUID, hostname UNIQUE, sku_type ENUM(8), procurement_date DATE, end_of_life_date DATE; `INDEX idx_fleet_eol (end_of_life_date)` for EOL planning
- Storage totals: 2.5 M metric streams; 168 TB raw (2 years); 10 TB active after downsampling

### Key Differentiator
Capacity Planning's uniqueness is its **bare-metal procurement integration + multi-model ensemble forecasting**: unlike cloud VM autoscaling (minutes), bare-metal procurement requires 12–24 week lead times — the system bridges demand forecasting to purchase orders with vendor lead-time constraints; 800 ensemble models (Prophet + ARIMA + linear regression) cover every AZ × SKU × resource combination; 20% headroom buffer target prevents surprise capacity exhaustion; Scenario Simulator enables what-if analysis before committing to hardware orders.

---

## Cluster Resource Manager

### Unique Functional Requirements
- DRF (Dominant Resource Fairness) scheduling with hierarchical queues (org → team → project)
- Gang scheduling: all-or-nothing placement for distributed ML jobs (all 64 workers placed simultaneously or none)
- Priority-based preemption: higher-priority workloads evict lower-priority ones
- 10,000 pods/sec scheduling throughput; < 500 ms P99 per pod; < 5 s for 64-pod gang

### Unique Components / Services
- **Queue Manager**: hierarchical queues (org → team → project) with DRF ordering; `dominant_share(queue) = max(used_cpu/total_cpu, used_ram/total_ram, used_gpu/total_gpu)`; queue with lowest dominant share scheduled next
- **Gang Scheduler Plugin**: co-scheduling via PodGroup CRD; holds pods in "waiting" state until all gang members are simultaneously placeable; atomically places all or retries all; prevents partial ML training launch
- **Node Monitor**: heartbeats every 10 s; > 40 s silence → NotReady; triggers job re-scheduling from that node
- **Eviction Manager**: monitors memory/disk/PID pressure signals; evicts pods respecting PodDisruptionBudgets; preempts based on priority_value comparison
- **Autoscale Controller**: monitors pending queue; scale-out if pods unschedulable; scale-in if utilization < 50% for > 10 minutes

### Unique Data Model
- **jobs**: job_id UUID, queue_name, tenant_id, task_count INT, min_task_count INT, priority_class VARCHAR(32), priority_value INT, dominant_share DECIMAL(10,6), submit_time TIMESTAMP(3); `INDEX idx_jobs_drf (queue_name, dominant_share, submit_time)`, `INDEX idx_jobs_priority (priority_class, priority_value DESC)`
- **scheduling_queues**: queue_id UUID, queue_name, parent_queue_id UUID, guaranteed_cpu/ram_mb/gpu INT, max_cpu/gpu INT, policy ENUM(fair_share/priority/fifo/capacity); `INDEX idx_queues_parent (parent_queue_id)`
- **tasks**: task_id UUID, job_id, node_id; `INDEX idx_tasks_node (node_id)`

### Algorithms

**Hierarchical DRF Example:**
```
Cluster: {100 CPU, 100 GB RAM}
User A needs <2 CPU, 8 GB>  → dominant = RAM (8%)
User B needs <6 CPU, 2 GB>  → dominant = CPU (6%)
DRF equalizes: User A gets ~5.7 allocations (45.7% RAM)
               User B gets ~7.6 allocations (45.7% CPU)
```

### Key Differentiator
Cluster Resource Manager's uniqueness is its **hierarchical DRF + all-or-nothing gang scheduling**: DRF equalizes fairness across multi-resource dimensions (CPU + RAM + GPU) simultaneously without requiring manual priority assignments; `dominant_share = max(used_cpu/total, used_ram/total, used_gpu/total)` — lowest dominant share queue scheduled next; PodGroup CRD gang scheduling prevents partial ML training launches that waste all allocated resources while waiting for remaining workers.

---

## Compute Resource Allocator

### Unique Functional Requirements
- Multi-dimensional bin packing: 5D (CPU, RAM, GPU, disk IOPS, network bandwidth) simultaneously
- NUMA-aware placement for latency-sensitive workloads
- GPU topology-aware placement (NVLink domain affinity)
- Defragmentation trigger when fragmentation > threshold; live migration/drain+reschedule
- 5,000 placements/sec; < 15% fragmentation ratio; < 200 ms P99 placement latency

### Unique Components / Services
- **Placement Engine**: Filter → Score → Bind pipeline; filters 50,000 hosts via predicates (CPU/RAM/GPU fit, affinity, NUMA, taint); scores using dot-product alignment; if candidates > 500, samples 500 randomly; binds with optimistic locking (version column in MySQL)
- **Dot-Product Alignment Scoring**: `A_norm = available / ||available||`, `R_norm = request / ||request||`; `alignment_score = dot(A_norm, R_norm)`; `final_score = 0.35 × alignment + 0.30 × utilization + 0.15 × affinity + 0.10 × numa + 0.10 × gpu_topo`
- **Defragmentation Engine**: async worker; identifies hosts with < 30% utilization or high fragmentation score; generates migration plans minimizing total migrations; executes during maintenance windows
- **Cluster State Cache**: Redis Cluster holding real-time host snapshots; updated every 3 s via gRPC heartbeats from 50,000 node agents; MySQL reconciled every 60 s
- **Overcommit Ratios**: CPU 5:1, RAM 1.2:1, GPU 1:1 (never overcommitted)

### Unique Data Model
- **hosts**: host_id CHAR(36), hostname UNIQUE, region, availability_zone, rack_id, sku_type ENUM(8), total_cpu_cores/ram_mb/gpu_count/gpu_mem_mb/disk_gb/net_gbps, alloc_cpu_cores/ram_mb (after system reservation); `INDEX idx_hosts_region_sku (region, sku_type)`
- **workload_placements**: placement_id, host_id, workload_id, cpu_cores/ram_mb/gpu_count, placement_timestamp TIMESTAMP(3), **version INT** (optimistic locking)

### Algorithms

**Dot-Product Placement Scoring:**
```python
A = [avail_cpu, avail_ram, avail_gpu, avail_disk, avail_net]
R = [req_cpu, req_ram, req_gpu, req_disk, req_net]
A_norm = A / norm(A)
R_norm = R / norm(R)
alignment = dot(A_norm, R_norm)   # 0..1, higher = better shape match
final_score = 0.35*alignment + 0.30*utilization + 0.15*affinity + 0.10*numa + 0.10*gpu_topo
```

### Key Differentiator
Compute Resource Allocator's uniqueness is its **5D dot-product alignment scoring + optimistic locking binding**: normalized dot-product of available-resource vector and request vector maximizes shape alignment (proven in Google Borg), minimizing fragmentation while packing; `final_score = 0.35×alignment + 0.30×utilization + 0.15×affinity + 0.10×numa + 0.10×gpu_topo` weights 5 dimensions; optimistic locking (version column) prevents double-allocation without distributed locks; overcommit CPU 5:1 / RAM 1.2:1 / GPU never reflects real-world workload burstiness.

---

## Quota and Limit Enforcement System

### Unique Functional Requirements
- Hierarchical quotas (org → team → project → user) with inheritance: effective_hard_limit = min(parent, self)
- Admission-time enforcement via Kubernetes ValidatingWebhookConfiguration — rejects pod before creation, not at runtime
- API rate limits: token bucket per tenant per endpoint (requests/sec + burst allowance)
- < 10 ms quota check latency; 20,000 quota checks/sec throughput
- Approval workflow: quota request → manager review → auto-allocation on approval

### Unique Components / Services
- **Quota Enforcement Service (QES)**: admission webhook; checks quota from Caffeine local cache (30 s TTL) → Redis → MySQL; rejects if used + requested > hard_limit; < 10 ms P99
- **API Gateway Rate Limiter**: Redis Lua EVALSHA for atomic token bucket: `if current + 1 <= limit: current++; allow; else: deny`; per-tenant per-endpoint
- **Usage Aggregator**: consumes pod/VM lifecycle events (50,000 events/sec); atomic Redis INCRBY on quota usage; MySQL usage snapshot every 30 s
- **Approval Workflow Engine**: quota request lifecycle with Slack/Teams notifications; auto-allocates on approval

### Unique Data Model
- **quota_definitions**: quota_id UUID, scope_id VARCHAR(255), scope_type ENUM(organization/team/project/user), parent_scope_id, resource_type VARCHAR(32), hard_limit/soft_limit/burst_limit BIGINT, burst_duration_seconds INT, default_request/default_limit BIGINT, max/min_per_workload BIGINT, effective_until TIMESTAMP(3); `INDEX idx_quota_scope (scope_id, resource_type)`
- **quota_usage_snapshots**: scope_id, resource_type, usage_amount BIGINT, timestamp TIMESTAMP(3); `INDEX idx_usage_scope_time (scope_id, timestamp)`
- **quota_requests**: request_id UUID, scope_id, resource_type, current_limit/requested_limit BIGINT, justification TEXT, status ENUM(pending/approved/denied), reviewer_id, request_time/decision_time TIMESTAMP(3)
- Caching: Caffeine local (30 s TTL) → Redis → MySQL; Redis Lua EVALSHA for atomic rate limit increment

### Key Differentiator
Quota System's uniqueness is its **admission-time enforcement + three-level caching + hierarchical inheritance**: ValidatingWebhookConfiguration intercepts pod CREATE before any resource is allocated — unlike runtime enforcement that stops workloads mid-execution; `effective_hard_limit = min(parent_hard_limit, this_level_hard_limit)` propagates org-wide caps down to individual users automatically; Caffeine (30 s) → Redis → MySQL three-level cache achieves < 10 ms admission checks at 20,000/sec without DB hotspot; burst_limit + burst_duration_seconds enables temporary overages with automatic reclaim.

---

## Resource Reservation System

### Unique Functional Requirements
- Zero double-booking: 100% conflict detection accuracy for time-based resource reservations
- Recurring reservations: "every Tuesday 2 AM–6 AM for maintenance" via cron expression expansion
- Overbooking: configurable ratio (e.g., 1.1 = 10% overbook) with < 5% actual displacement rate
- 1,000 concurrent reservations/sec; 100,000 active concurrent reservations at any time
- Billing integration: emit reservation lifecycle events to Kafka

### Unique Components / Services
- **Conflict Detection Engine**: augmented interval tree (BST ordered by host_id × start_time); O(log n + k) overlap detection; two intervals overlap if `s1 < e2 AND s2 < e1`; scales to 100,000 concurrent reservations
- **Activation Engine**: Redis sorted set as timer queue (`ZADD timers score=start_time epoch`); polls `ZRANGEBYSCORE timers 0 {now}` every 1 s; transitions CONFIRMED → ACTIVE within 30 s SLA
- **Recurring Reservation Expander**: parses cron expressions (e.g., `0 2 * * 2` = Tuesday 2 AM UTC); expands to concrete instances for rolling 30-day window; runs daily at 2 AM UTC
- **Billing Event Publisher**: emits to Kafka: reservation_created, reservation_activated, reservation_completed, reservation_cancelled

### Unique Data Model
- **reservations**: reservation_id UUID, project_id, status ENUM(requested/confirmed/active/completed/expired/cancelled), start_time/end_time TIMESTAMP(3), cpu_cores/ram_mb/gpu_count/gpu_type/disk_gb/net_gbps, region, availability_zone, reservation_type ENUM(guaranteed/best_effort/maintenance), priority INT; `INDEX idx_res_time_resource (start_time, end_time, region)`
- **host_resource_timeline**: host_id CHAR(36), start_time TIMESTAMP(3), end_time TIMESTAMP(3), reservation_id UUID, resource_type VARCHAR(32), amount BIGINT; PK (host_id, start_time, end_time); `INDEX idx_timeline_host (host_id, start_time, end_time)` — enables interval overlap queries
- **recurring_reservation_templates**: template_id UUID, cron_expression VARCHAR(32), duration_seconds INT, timezone VARCHAR(64), resource_requirements JSON, type ENUM(maintenance/training/backup), effective_from/until DATE

### Algorithms

**Interval Overlap Detection:**
```
Intervals [s1, e1) and [s2, e2) overlap if: s1 < e2 AND s2 < e1
Interval tree: O(log n + k) where k = overlapping intervals
SELECT FOR UPDATE on host_resource_timeline for concurrent reservation safety
```

**Timer Activation:**
```
ZADD timers {start_time_epoch} {reservation_id}
Every 1s: ZRANGEBYSCORE timers 0 {now}  → activate all due reservations
```

### Key Differentiator
Resource Reservation System's uniqueness is its **interval tree conflict detection + cron-based recurring expansion + Redis timer activation**: interval tree augmented BST gives O(log n + k) overlap detection for zero double-bookings at 100,000 concurrent reservations; `s1 < e2 AND s2 < e1` overlap check is evaluated in MySQL using the `host_resource_timeline` index; Redis sorted set (score = start_time epoch) enables sub-second activation polling without expensive scheduled job frameworks; cron expression expansion (`0 2 * * 2` = Tuesday 2 AM) provides recurring maintenance window scheduling without storing millions of individual reservation rows.
