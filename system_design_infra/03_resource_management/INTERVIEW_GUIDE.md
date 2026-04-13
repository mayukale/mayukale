# Infra Pattern 3: Resource Management — Interview Guide

Reading Infra Pattern 3: Resource Management — 5 problems, 8 shared components

---

## STEP 1 — ORIENTATION

**What this pattern covers:** Five distinct but deeply interconnected resource management problems that appear across FAANG/NVIDIA-scale infrastructure:

1. **Capacity Planning System** — Forecast future demand, recommend hardware procurement with 12–24 week bare-metal lead times
2. **Cluster Resource Manager** — Schedule workloads across clusters using Hierarchical DRF, gang scheduling, and autoscaling
3. **Compute Resource Allocator** — Place individual workloads onto physical hosts via multi-dimensional bin packing
4. **Quota and Limit Enforcement System** — Enforce per-tenant resource boundaries at admission time, not runtime
5. **Resource Reservation System** — Reserve future capacity with conflict-free time-interval guarantees

**Why interviewers love this area:** Resource management is where hardware costs, software complexity, and multi-tenant fairness all collide. Getting it wrong means either wasted hardware (expensive) or cascading failures from overprovisioning (catastrophic). Every FAANG team runs this at scale, and the edge cases — gang scheduling deadlocks, DRF in hierarchical queues, overbooking displacement, bare-metal lead times — separate seniors from staff engineers.

**The 8 shared components that bind all 5 problems together:**
- MySQL 8.0 as the source-of-truth store for structured resource state
- Redis for real-time sub-millisecond cache on the hot path
- Multi-dimensional resource tracking (CPU, RAM, GPU, Disk, Network)
- Hierarchical org model (org → team → project → user)
- State machines for resource lifecycle management
- L1 local (Caffeine) + L2 Redis + L3 MySQL three-level caching
- REST management plane + gRPC data plane communication split
- Optimistic locking (version column) for concurrent resource writes

---

## STEP 2 — MENTAL MODEL

**The core idea:** Resource management is fundamentally the problem of **mapping demand onto supply across time, space, and priority dimensions simultaneously** — and doing it fast, fairly, and without double-booking.

Think of it like running a large hotel chain:
- **Capacity Planning** is your revenue management team predicting occupancy 6 months out and deciding how many new hotels to build (with a 6-month construction lead time — you cannot auto-scale a physical building in minutes)
- **Cluster Resource Manager** is the hotel booking platform deciding which property gets your reservation based on availability, your loyalty tier, and fairness across all guests
- **Compute Resource Allocator** is the specific room assignment system — which exact room on which floor, considering your preferences, the room's amenities, and maximizing hotel occupancy
- **Quota Enforcement** is the policy layer preventing one guest from booking 500 rooms and blocking everyone else
- **Resource Reservation** is the advance booking system — you lock in a specific room for a future date, the system guarantees it won't be given to anyone else, and it automatically checks you in at your arrival time

**Why it's hard — three compounding difficulties:**

First, **multi-dimensional NP-hardness**: bin packing is NP-hard in one dimension. You're doing it in 5 dimensions (CPU, RAM, GPU, disk, network) simultaneously, at 5,000 placements/second, with hard latency SLAs. You cannot solve it optimally — you need approximation algorithms (dot-product alignment scoring, candidate sampling) that get you to 85%+ of optimal in under 200ms.

Second, **the fairness paradox**: simple FIFO starves small jobs when big jobs dominate. Priority queuing requires operators to manually tune priorities and causes low-priority jobs to never run. DRF (Dominant Resource Fairness) solves multi-resource fairness mathematically, but implementing hierarchical DRF with preemption and gang scheduling without deadlocking the cluster is genuinely hard.

Third, **the consistency-latency tension**: you need strong consistency to prevent double-allocations (two schedulers placing on the same host simultaneously) but you also need sub-10ms admission checks at 20,000/sec. The solution — Redis for fast-path reads, MySQL with optimistic locking for binding commits — is elegant but requires careful thinking about what "stale reads are acceptable" actually means in each context.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these before touching the whiteboard. Each answer changes the architecture significantly.

**Question 1: What types of resources are we managing — just CPU and RAM, or also GPU, disk, network?**
*What changes:* CPU-only systems can use simple bin packing. Adding GPU introduces topology constraints (NVLink domains, NVMe bandwidth), never-overcommit rules, and 10x more expensive hardware. Network bandwidth adds another dimension. The answer determines whether you need 5D scoring or can get away with 2D.

**Question 2: Is this a cloud environment where you can provision new capacity in minutes, or bare-metal with 12–24 week procurement lead times?**
*What changes:* Cloud = reactive autoscaling is sufficient. Bare-metal = you need proactive forecasting 6 months ahead, procurement recommendation workflows, and buffer targets (20% headroom). This is the single most important differentiator for capacity planning specifically.

**Question 3: Multi-tenant or single-tenant? If multi-tenant, how many tenants, and do we need hard isolation (quotas) or best-effort fairness?**
*What changes:* Single-tenant: no quota system needed, scheduling policy can be simpler. Multi-tenant: you need the full quota enforcement layer, hierarchical org model, rate limiting, and audit trails. 500 orgs / 5,000 teams / 50,000 projects is a materially different problem than 5 internal teams.

**Question 4: What are the dominant workload types — latency-sensitive services, batch jobs, or ML training?**
*What changes:* Services need anti-affinity for reliability. Batch needs fair-share scheduling so long-running jobs don't block others. ML training is the hardest: it needs gang scheduling (all 64 GPU workers placed simultaneously or none), GPU topology awareness (NVLink domains), and high-priority preemption rights. Each type changes the scheduling algorithm and data model substantially.

**Question 5: Do we need time-based reservations, or is it purely on-demand?**
*What changes:* On-demand: the allocator just does instant placement. Reservations add a time dimension — you need conflict detection across overlapping time intervals, an activation engine that triggers workloads at reservation start time, and recurring reservation expansion. This is an entirely separate subsystem.

**Red flags to watch for:**
- Candidate immediately jumps to "just use Kubernetes" without asking about bare-metal — Kubernetes doesn't solve procurement lead times
- Candidate suggests a single database for both real-time counters and historical analytics — that's a performance disaster
- No mention of fairness — pure priority queuing starves lower-priority tenants and creates organizational conflict
- Ignoring the consistency question — saying "Redis is the source of truth" for resource allocation is wrong; Redis is a cache, MySQL is authoritative

---

### 3b. Functional Requirements

**Core requirements for a generic resource management system:**
- Accept workload submissions specifying multi-dimensional resource requirements (CPU, RAM, GPU, disk, network)
- Maintain an accurate real-time view of cluster resource availability across all nodes
- Schedule workloads using a configurable fairness policy (DRF, priority, FIFO)
- Enforce per-tenant resource quotas at admission time — prevent over-allocation before it happens
- Support preemption: higher-priority workloads can evict lower-priority ones
- Track resource lifecycle with explicit state machines and full audit trails

**Scope clarifications for the interview:**
- "Real-time autoscaling" typically means cloud VM provisioning (minutes) — keep this separate from capacity planning
- Billing and cost attribution consumes the data from this system; it's out of scope to build billing here
- Network policy and service mesh are out of scope; you're allocating network bandwidth, not configuring SDN
- Hardware procurement execution (talking to vendors) is out of scope; this system generates purchase recommendations

**Clear statement:** Design a resource management system that can accept workloads from thousands of tenants, fairly allocate compute resources using DRF across a hierarchical org structure, enforce hard quotas at admission time, and either plan future capacity needs or manage time-based reservations — depending on which of the 5 variants is asked.

---

### 3c. Non-Functional Requirements

**Availability — derive from blast radius:** The scheduler and quota enforcement are in the critical path of every workload launch. If the scheduler goes down, nobody can start new jobs. **99.99% availability** (52 minutes/year) is the right target. Capacity planning is advisory — humans read dashboards — so **99.9%** is acceptable there.

**Latency — derive from user experience:**
- Quota admission check must complete inside the Kubernetes admission webhook timeout, which is typically 10 seconds but should be much faster in practice: **< 10ms p99** at 20,000 checks/second
- Single workload placement: **< 200ms p99** (Kubernetes kube-scheduler targets < 100ms; this is a reasonable goal for a custom system with more complexity)
- Gang scheduling (64 pods): **< 5 seconds** — the ML team is waiting for their training run to start
- Dashboard queries (pre-aggregated): **< 2 seconds p99** — this is an engineer reviewing capacity, not a transaction

**✅ Trade-offs in the NFR tier:**
- ✅ Strong consistency for allocation binding (MySQL optimistic locking) ensures no double-allocations
- ❌ Trade-off: adds 10–50ms latency on the commit path compared to eventual consistency
- ✅ Eventual consistency for monitoring/dashboards (Redis + VictoriaMetrics) enables high read throughput at low cost
- ❌ Trade-off: dashboards can show slightly stale data (30–60 seconds lag), which is fine for capacity planning but unacceptable for admission checks
- ✅ Redis fast-path caching for quota checks keeps admission under 10ms at 20K/sec
- ❌ Trade-off: requires a reconciliation loop to catch Redis divergence from MySQL; a Redis failure without proper fallback could cause admission service outage

**Throughput targets (anchor numbers):**
- Capacity planning: ingest 166,667 metric data points/sec (50,000 hosts × 50 metrics per 15-second scrape)
- Cluster scheduling: 10,000 pod scheduling decisions/sec at peak
- Compute placement: 5,000 placements/sec cluster-wide
- Quota checks: 20,000/sec
- Reservation creation: 1,000/sec

---

### 3d. Capacity Estimation

**The formula and anchor numbers to memorize:**

For the fleet baseline (50,000 hosts):
```
Metric streams = hosts × metrics_per_host = 50,000 × 50 = 2,500,000
Data points/sec = 2,500,000 / 15s scrape interval = 166,667 dps/sec
Storage (raw, 14-day) = 166,667 × 86,400 sec × 16 bytes × 14 days ≈ 3.2 TB
After downsampling (15s→5min→1hr→1day): ~10 TB active
```

For the scheduling plane (single 10,000-node cluster):
```
Active workloads = 10,000 nodes × 30 containers/node = 300,000
Node heartbeats = 10,000 / 10s = 1,000/sec
Heartbeat bandwidth = 1,000 × 8KB = 8 MB/s
Scheduling history (30 days) = 500,000 decisions/day × 0.5KB × 30 = 7.5 GB
Active state (nodes + pods) fits in memory: ~700 MB
```

For quota enforcement:
```
Quota definitions = 500 orgs + 5,000 teams + 50,000 projects = 55,500 scopes
Definition storage = 55,500 × 2KB = 111 MB — fits entirely in Redis
Usage counters = 55,500 scopes × ~10 resource types × 64 bytes = ~35 MB in Redis
Rate limit buckets = 100,000 users × 64 bytes = 6.4 MB in Redis
```

**Architecture implications of these numbers:**
- 166K dps/sec for metrics cannot go to MySQL directly — you need a time-series database (VictoriaMetrics/Thanos) with downsampling pipelines
- 700 MB active scheduler state fits in a single Redis instance — no sharding needed at single-cluster scale
- 111 MB of quota definitions fits entirely in Redis AND in a local in-process cache (Caffeine) — this is why < 10ms admission checks are achievable
- 10 TB active capacity data means you need object storage (S3/MinIO) for archives, not MySQL

**Time to estimate in interview:** State the formulas first, plug in the numbers second. For a 45-minute interview, spend 4–5 minutes on estimates maximum. Hit the "fits in RAM" vs "needs partitioning" thresholds — that drives architecture decisions.

---

### 3e. High-Level Design

**The 6 core components (whiteboard order):**

**1. API Gateway (draw first):** Authentication, rate limiting (token bucket per tenant), TLS termination. In the Quota system, the gateway itself runs the rate limiter using Redis Lua EVALSHA for atomic token bucket logic. This is the first line of defense before any backend service.

**2. Core Service Layer (draw second):** Stateless service instances that handle the core logic — the Allocator Instances (Compute), the Queue Manager + Scheduling Engine (Cluster RM), the Quota Enforcement Service (Quota), or the Reservation API Service (Reservation). Stateless means horizontally scalable. Multiple instances run simultaneously and coordinate through the shared state stores.

**3. Fast-Path Cache — Redis (draw third):** Redis Cluster holding real-time state that must be read on every hot-path operation: host resource snapshots (updated every 3 seconds via node agent heartbeats), quota usage counters (atomic INCRBY on every pod create), rate limit token buckets, reservation timeline (sorted set of upcoming activations). Redis is never the source of truth — it's the performance layer in front of MySQL.

**4. Source-of-Truth Store — MySQL 8.0 (draw fourth):** All structured resource state: host inventory, workload placements, queue definitions, quota definitions, reservations, procurement recommendations. ACID transactions prevent double-allocations. Optimistic locking (version column) handles concurrent writes without distributed locks. Read replicas serve analytics queries.

**5. Node Agent Fleet (draw fifth):** Lightweight agents on every physical host (or kubelet for containers) reporting resource capacity, current utilization, and health via gRPC streaming. The heartbeat pipeline is what keeps Redis in sync with reality. For 50,000 hosts at 3-second heartbeats: 16,667 heartbeats/sec, ~33 MB/s inbound bandwidth.

**6. Async Workers (draw sixth):** Background services that do expensive or time-triggered work: Forecasting Engine (daily batch model training), Defragmentation Engine (15-minute fragmentation analysis), Recurring Reservation Expander (daily cron expansion), Autoscale Controller (event-driven scale-out trigger), Report Generator (monthly PDF reports).

**Data flow for the critical path (placement/admission):**
Request → API Gateway (auth, rate limit) → Stateless Service (read Redis for fast candidate filter) → MySQL optimistic lock commit → on conflict, retry with refreshed Redis state → return placement result

**Key architectural decisions to call out explicitly:**
- Why split REST and gRPC? REST for human-driven management (100 concurrent users), gRPC for machine-to-machine data plane (10,000+ req/sec, streaming heartbeats)
- Why not just use etcd for everything? etcd has an 8GB practical size limit and poor throughput for range queries. MySQL handles complex queries, hierarchical data, and larger datasets. Use etcd only for Kubernetes-native objects.
- Why optimistic locking over distributed locks? Distributed locks (Zookeeper, Redis SETNX) add latency and create single points of failure. Optimistic locking on the MySQL `version` column adds < 1ms and scales with MySQL read replicas.

---

### 3f. Deep Dive Areas

**Deep Dive 1: The DRF Algorithm and Why It's the Right Choice**

The problem: you have a multi-tenant cluster. Team A runs ML training jobs that need lots of GPU but modest CPU. Team B runs web services that need CPU but no GPU. Simple CPU-based fair share gives Team A too much CPU they don't need and leaves Team B starved. Simple GPU-based fair share ignores Team B entirely since they use no GPU.

DRF's insight: for each user/queue, compute the **dominant resource** — the resource type for which their share of the total cluster is highest. Then equalize dominant shares across all users.

The math: cluster has 100 CPU, 100GB RAM. User A's jobs need <2 CPU, 8GB>. User A's CPU share = 2%, RAM share = 8% → dominant resource is RAM (8%). User B's jobs need <6 CPU, 2GB>. CPU share = 6%, RAM share = 2% → dominant resource is CPU (6%). DRF equalizes at ~45.7% dominant share each: User A gets ~5.7 job slots, User B gets ~7.6 job slots.

**Implementation on a whiteboard:**
```
dominant_share(queue) = max(used_cpu/total_cpu, used_ram/total_ram, used_gpu/total_gpu)
Schedule next: pick queue with lowest dominant_share that has pending jobs
Recurse into hierarchical queues (org → team → project) picking lowest-share child at each level
```

**Trade-offs to state unprompted:**
- ✅ DRF provides four mathematical guarantees: sharing incentive (no team is better off not sharing), strategy-proofness (no benefit from lying about resource needs), envy-freeness (no team prefers another's allocation), Pareto efficiency (no resources wasted)
- ❌ DRF can temporarily starve teams when a large gang job holds the dominant resource — mitigated by preemption policies
- ✅ Hierarchical DRF lets you set guaranteed quotas per team (guaranteed_cpu in the scheduling_queues table) as a floor, with elastic burst up to max_cpu
- ❌ Computing dominant shares in real time requires keeping running sums of used resources per queue — adds database contention at high scheduling rates; mitigate with in-process caching of queue state updated every scheduling cycle

---

**Deep Dive 2: Optimistic Locking for Concurrent Placement (the Double-Allocation Problem)**

The problem: two allocator instances read Redis and both see Host X has 8 free CPUs. Job A needs 5 CPUs, Job B needs 5 CPUs. Both allocators pick Host X simultaneously. Both try to commit. Without coordination, both succeed and Host X is overcommitted.

The naive solution — distributed locks — creates a bottleneck and a single point of failure. If the lock service (Redis, ZooKeeper) goes down, all placements stop.

The right solution — **optimistic locking with a version column**:
```sql
-- Every host record has: version BIGINT NOT NULL DEFAULT 1
-- Read phase (from Redis, fast):
SELECT host_id, free_cpu, version FROM hosts WHERE host_id = ?

-- Write phase (to MySQL, authoritative):
UPDATE hosts
SET used_cpu = used_cpu + ?, version = version + 1
WHERE host_id = ? AND version = ?  -- the version we read
-- If 0 rows updated: concurrent modification detected, retry
```

If two allocators race, exactly one wins (increments version), the other gets 0 rows updated and retries against the refreshed state. Retry rate at 5,000 placements/sec with 50,000 hosts is very low (each host gets ~0.1 placements/sec on average), so retries are rare.

**Trade-offs to state unprompted:**
- ✅ No distributed lock service dependency — MySQL alone coordinates consistency
- ✅ Linear retry rate: even at 10x burst, expected retries are < 0.01% of requests
- ❌ If cluster is severely fragmented (only 1–2 viable hosts for many requests), collision rate spikes and you see retry storms — the defragmentation engine exists partially to prevent this
- ✅ Redis can be "wrong" by 3–10 seconds (heartbeat lag) without causing double-allocations, because MySQL is the binding authority — this is intentional design, not a bug

---

**Deep Dive 3: Gang Scheduling and the Partial Placement Problem**

The problem: a distributed ML training job needs 64 GPU workers placed simultaneously. If you place 32 workers and the other 32 can't be placed, you've wasted 32 GPUs (they're sitting idle waiting for the rest of the gang) AND blocked progress.

The naive solution — place pods one by one as resources free up — fails because in a shared cluster, other jobs fill in around your partial gang, fragmenting it indefinitely.

The right solution — **PodGroup CRD with the co-scheduling plugin**:
1. All 64 pods are submitted as members of a PodGroup
2. The gang scheduler holds all 64 in "waiting" state
3. The scheduler checks if all 64 can be placed simultaneously (runs the filter-score pipeline for all 64 against current cluster state)
4. If yes: atomically binds all 64 in a single transaction
5. If no: all 64 remain pending, cluster is not partially reserved, other jobs proceed
6. Retried every scheduling cycle until capacity appears (via natural churn or preemption)

**Trade-offs to state unprompted:**
- ✅ Prevents GPU waste from partial placements
- ❌ A gang waiting for full placement can block the cluster's scheduling queue if it's large enough — mitigated by setting a `min_task_count` (e.g., 48 of 64 is enough to start training with data parallelism)
- ❌ Long-waiting gangs can starvation-deadlock if multiple gangs each need more resources than available — mitigated by priority-based preemption and configurable gang timeout with graceful degradation
- ✅ Kubernetes Volcano and YARN's gang scheduler both implement this pattern in production; it's battle-tested

---

### 3g. Failure Scenarios

**Failure 1: The Scheduling Service Goes Down**

*How it fails:* The cluster resource manager crashes or becomes unavailable. All new job submissions fail. Existing running jobs continue (they don't need the scheduler to keep running), but any workload that crashes or is evicted cannot be rescheduled.

*Senior framing:* "The scheduler is a critical path dependency for launch, not for run. Existing workloads survive an outage. The blast radius is new job launches and pod restarts — not the live fleet. We run the scheduler in active-active with leader election (via etcd lease) to minimize MTTR. During a scheduler outage, the autoscaler is also impaired — if a node fails and needs replacement, new nodes can't be commissioned. We should alert aggressively on scheduler unavailability with a 1-minute SLO breach threshold."

**Failure 2: Redis Cache Diverges from MySQL (Stale Cache Scenario)**

*How it fails:* A node agent dies. Its resource state in Redis stops updating. The Redis cache says the node has 8 free CPUs. MySQL shows the node is down. The scheduler keeps trying to place workloads on the dead node, each attempt failing at commit time (MySQL optimistic lock fails because the node status has been updated to NotReady).

*Senior framing:* "This is why the Node Monitor with heartbeat tracking matters. Any node silent for > 40 seconds gets marked NotReady in MySQL. The Cluster State Cache reconciliation loop (runs every 60 seconds) pushes this status to Redis. In the worst case, there's a 60-second window where the scheduler tries and fails to place on the dead node, burning retry cycles. The fix: when MySQL rejects a placement due to node status, immediately invalidate the Redis cache entry for that node. Don't wait 60 seconds. This is a targeted cache invalidation triggered by optimistic lock failure — it converts a 60-second drift window into a single retry."

**Failure 3: Quota Enforcement Service Unavailable**

*How it fails:* The Kubernetes admission webhook (QES) becomes unreachable. Kubernetes has two failure modes: `failurePolicy: Fail` (all pod creations fail if the webhook is down) or `failurePolicy: Ignore` (all pod creations succeed, bypassing quota).

*Senior framing:* "This is a policy decision with serious implications. `Fail` policy makes QES a hard dependency — high availability (99.99%) is not optional. `Ignore` policy means a QES outage creates a window where tenants can exceed their quotas. We use `Fail` with a very tight webhook timeout (3 seconds) and a 99.99% availability SLO for QES backed by 3-replica active-active deployment. We also implement local fallback: if QES is down but the local cache is warm (< 5 minutes old), the cache serves as an emergency admission gate. This is not a substitute for a healthy QES but prevents a complete platform outage during a brief QES restart."

**Failure 4: Reservation Activation Misfire**

*How it fails:* A reservation for 64 H100 GPUs starting at 2:00 AM misses its activation. The research team's ML training window is 2–8 AM. If activation fires at 2:30 AM, they lose 30 minutes of their 6-hour window.

*Senior framing:* "The Activation Engine's SLA is `workload started within 30 seconds of start_time`. The Redis sorted set timer queue polls every second. The bottleneck is not the timer check — it's the actual workload launch (scheduler integration, node agent response, container pull). We pre-warm the 30-second window: at `start_time - 30s`, begin staging the workload resources (pre-pull images, pre-allocate network interfaces). The actual workload starts at `start_time`. If staging fails, alert at `start_time - 5 minutes` so operators can intervene before the window is lost."

---

## STEP 4 — COMMON COMPONENTS

Every component below appears across multiple problems. Understand each deeply.

---

### MySQL 8.0 — Source of Truth for Resource State

**Why used:** MySQL provides ACID transactions critical for preventing double-allocations. The `version` column pattern (optimistic locking) coordinates concurrent writes from multiple stateless service instances without distributed locks. MySQL's SQL query capabilities handle hierarchical quota lookups (CTEs for `org → team → project` traversal), time-range queries for reservations, and analytics joins across fleet inventory and utilization history. The role specifically requires MySQL expertise.

**Key configuration:**
- `InnoDB` storage engine for row-level locking (not table-level)
- `TIMESTAMP(3)` for millisecond precision on all time columns (scheduling is millisecond-sensitive)
- Read replicas for analytics queries (forecasting engine reads 90 days of hourly data — don't run this on the primary)
- `version BIGINT NOT NULL DEFAULT 1` on every mutable resource record — this is the optimistic locking anchor
- Index strategy: composite indexes matching the query patterns (`(availability_zone, sku_type, status)` for capacity queries; `(queue_id, dominant_share, submit_time)` for DRF scheduling; `(host_id, start_time, end_time)` for reservation conflict detection)

**What happens without it:** You either use a single-instance Redis as source of truth (loses ACID guarantees, can double-allocate during network partition) or use etcd (8GB size limit, no complex queries, poor throughput for range scans). At scale, neither works. MySQL's combination of ACID, SQL flexibility, and operational maturity is the right choice for structured resource metadata.

---

### Redis — Real-Time Resource State Cache

**Why used:** Every hot-path operation in resource management must complete in under 10ms (quota checks) to 200ms (placements). MySQL at 5,000 writes/sec with optimistic lock contention is too slow for read-heavy hot paths. Redis provides sub-millisecond reads for host state snapshots (updated by heartbeats every 3 seconds), atomic operations for quota counters (`INCRBY`/`DECRBY` for usage tracking), Lua scripts for rate limiting (atomic token bucket: check + increment in one round trip), and sorted sets for the reservation timer queue.

**Key configuration:**
- Redis Cluster (sharding) for quota counters — distribute load across shards
- `EVALSHA` for Lua script execution (precompile rate-limit scripts, avoid network round trips for check-then-set logic)
- TTLs on everything: host state snapshots (~10 seconds, refreshed by heartbeats), quota definition cache (30 seconds, refreshed on quota changes), rate limit windows (1-60 second sliding windows)
- Sorted sets for timer queues: `ZADD timers {epoch_timestamp} {reservation_id}`, poll with `ZRANGEBYSCORE timers 0 {now}` — O(log n + k) poll, perfectly suited for the activation engine

**What happens without it:** MySQL becomes the hot path for every quota check (20,000/sec) and every node heartbeat acknowledgment (16,667/sec for 50,000 nodes). MySQL can handle ~50,000 simple writes/sec but under contention with analytics queries and reporting, you'll see lock waits and latency spikes. The quota check latency balloons from < 10ms to 50–200ms, breaking the Kubernetes webhook timeout.

---

### Multi-Dimensional Resource Tracking (CPU, RAM, GPU, Disk, Network)

**Why used:** Every problem in this pattern tracks the same 5 resource dimensions. Tenants specify requirements across all 5. The allocation decision must verify fit across all 5 simultaneously (you can have enough CPU but not enough GPU). Ignoring any dimension creates a resource leak — workloads will overcommit the untracked resource and cause OOM kills, disk pressure evictions, or network saturation.

**Key configuration:**
- **CPU overcommit: 5:1** — most workloads burst to their CPU limit only occasionally. A 5x overcommit ratio matches empirical workload behavior from Google Borg data.
- **RAM overcommit: 1.2:1** — memory is more dangerous to overcommit (OOM kills are hard to diagnose). 1.2x is conservative but safe.
- **GPU: never overcommit** — GPU memory is partitioned and a workload that exceeds its GPU memory will crash the entire GPU process. Hard partitioning with no overcommit.
- Network bandwidth is tracked as a soft guarantee, not a hard one (SDN enforcement is separate).

**What happens without it:** If you only track CPU and RAM (the most common simplification), you fill GPU nodes with CPU-heavy workloads that starve GPU jobs. You fill high-IOPS storage nodes with network-heavy workloads that saturate disk. Real-world utilization drops to 40–50% because of single-dimension bottlenecks on undertracked resources.

---

### Hierarchical Org Model (org → team → project → user)

**Why used:** Resource allocation in a large organization is a political problem as much as a technical one. If you give each team an independent flat quota, large teams bully the platform into allocating them more resources. The hierarchy allows: org-level caps that can never be exceeded even if teams conspire to accumulate quotas; team-level guaranteed minimums that can't be taken away by org-level reallocation; project-level granularity for chargeback and cost attribution; user-level rate limiting to prevent a single engineer's runaway job from exhausting a team's quota.

**Key configuration:**
- `effective_hard_limit = min(parent_hard_limit, this_level_hard_limit)` — a child can never exceed its parent's limit, regardless of what the child's own quota says
- `parent_scope_id` foreign key with `NOT NULL` allowed only at root (org level) — enforces hierarchy in schema
- DRF scheduling uses the same hierarchy: `dominant_share` is computed per queue, and H-DRF recursively picks the lowest-share queue at each level
- Quota changes propagate downward: when you reduce an org's GPU quota, all teams under that org automatically see reduced effective limits

**What happens without it:** Flat per-team quotas with no inheritance create administrative chaos. A team can create 1,000 projects and distribute their quota to circumvent org-level controls. A single compromised service account can exhaust the entire namespace's quota. Chargeback becomes impossible because there's no hierarchy to aggregate costs up.

---

### State Machine for Resource Lifecycle

**Why used:** Resource management has multiple actors (user, scheduler, autoscaler, billing system) modifying the same records concurrently. Without explicit state machines, you get illegal transitions: a CANCELLED reservation gets activated, a COMPLETED job gets preempted, an ORDERED procurement recommendation gets re-proposed. Each state machine defines: the set of valid states, the set of valid transitions, and who is allowed to trigger each transition.

**Key configurations across problems:**
- **Procurement:** `proposed → approved → ordered → delivered → (cancelled at any step)`
- **Scheduling jobs:** `pending → scheduling → running → (succeeded | failed | cancelled | preempted)`
- **Quota requests:** `pending → (approved | denied | auto_approved | expired | cancelled)`
- **Reservations:** `requested → confirmed → active → (completed | expired | cancelled | displaced)`
- **Hosts:** `provisioning → available → cordoned → draining → maintenance → decommissioned`

**What happens without it:** You get corruption. A reservation in `expired` state gets billed. A job in `preempted` state gets rescheduled to the same node that evicted it. The billing system receives `reservation_completed` for a reservation that was `cancelled`. State machines are one of those "obvious in retrospect" requirements that junior engineers skip and then spend weeks debugging.

---

### L1 Local Cache + L2 Redis + L3 MySQL Caching Strategy

**Why used:** The latency requirements (< 10ms for quota admission) with the throughput requirements (20,000 checks/sec across stateless service instances) cannot be met with a single cache layer. Redis adds ~1–2ms network round trip from a co-located service, which already takes 20–40% of a 10ms budget. For static, infrequently-changed data (quota definitions change at most a few times per day per scope), an in-process cache eliminates the network hop entirely.

**Key configuration:**
- **L1 (Caffeine/HashMap in-process):** Quota definitions, computed effective limits (30-second TTL). Serves ~80% of reads without any network I/O. On cache miss, falls through to Redis.
- **L2 (Redis):** Real-time usage counters (updated every pod create/delete, never cached locally), rate limit state, hot quota data for scopes not in L1. Serves ~18% of reads.
- **L3 (MySQL):** Source of truth, serves ~2% of reads (cold paths, audit queries, analytics). Never on the hot path for admission checks.

**Invalidation:** Quota definition updates write to MySQL first, then publish an invalidation event (via Kafka or Redis pub/sub) that all service instances consume to evict their L1 entry. This prevents stale quota definitions from being used after an admin increases a team's limit.

**What happens without it:** Without L1, quota checks require a Redis round trip for every admission — at 20,000/sec, that's 40MB/s of Redis network traffic just for quota reads. Redis becomes the bottleneck. Without L2, every quota check hits MySQL — the 20,000/sec write path for usage counters (INCRBY) would overwhelm MySQL's row-lock throughput.

---

### REST Management Plane + gRPC Data Plane

**Why used:** REST (HTTP/1.1 JSON) is appropriate for human-driven, infrequent operations: creating a queue, submitting a quota increase request, approving a procurement recommendation. These have < 100 concurrent users, and the extra overhead of HTTP headers and JSON parsing is invisible. gRPC (HTTP/2 protobuf) is required for machine-to-machine high-throughput operations: 50,000 node agents sending heartbeats every 3 seconds (16,667 heartbeats/sec), scheduling decisions at 10,000/sec, admission checks at 20,000/sec. gRPC's binary encoding and HTTP/2 multiplexing cut bandwidth by 3–5x compared to JSON.

**Key configuration:**
- gRPC streaming (bidirectional) for node heartbeats — one long-lived connection per node agent instead of 16,667 connection establishments per second
- gRPC unary for placement requests — low latency, simple request-response
- REST with standard HTTP status codes (429 Too Many Requests for rate limiting, 507 Insufficient Storage for quota exceeded) for operability — operators and scripts can understand REST error responses without gRPC tooling
- `Idempotency-Key` header (UUID) on all REST mutating endpoints — stored in Redis (24h TTL), duplicate requests return cached result

**What happens without it:** Using only REST at data plane throughput (10,000 scheduling decisions/sec) creates 80MB/s of JSON serialization overhead and 10,000 connection establishments/sec. Using only gRPC for the management plane means operators need gRPC client tooling (grpcurl, BloomRPC) instead of curl — poor operability.

---

### Optimistic Locking (Version Column)

**Why used:** Multiple stateless allocator instances run simultaneously and all try to claim the same scarce resources. Without coordination, two instances can both decide "Host X has space" and both place workloads there, overcommitting the host. Optimistic locking solves this without a distributed lock service: each mutable resource record has a `version BIGINT` column. The update succeeds only if the version matches what was read. If a concurrent write happened in between, the update returns 0 rows affected, and the caller retries with fresh state.

**Key configuration:**
```sql
-- Always on host/placement records:
version BIGINT NOT NULL DEFAULT 1

-- The atomic commit pattern:
UPDATE hosts
SET used_cpu = used_cpu + ?, version = version + 1
WHERE host_id = ? AND version = ?  -- fail if someone else modified
-- If rows_affected = 0: retry from Redis read
```

**What happens without it:** Either you use distributed locks (Redis SETNX or Zookeeper ephemeral nodes) — which add 2–5ms of lock acquisition latency and create a single point of failure — or you get double-allocations. At 5,000 placements/sec across 50,000 hosts, the average host sees ~0.1 placements/sec, so collisions are rare but not zero. In a GPU cluster where 8-GPU hosts are the scarce resource and hundreds of jobs compete for them, collisions happen constantly without coordination.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Capacity Planning System

**What makes it unique:** This is the only problem that is **advisory, not real-time**. Everything else in this pattern is in the critical path of workload execution. Capacity planning is a decision-support system — it tells humans what to buy and when, but it doesn't block anything. This changes the availability requirement (99.9% not 99.99%), the latency requirement (seconds for dashboard queries, minutes for forecast generation), and the architecture (batch forecasting pipelines instead of synchronous request handlers).

**Unique decisions:**
- **VictoriaMetrics/Thanos instead of MySQL for the primary store** — 166,667 metric data points per second must go to a time-series database, not a relational one. The downsampling pipeline (15s → 5min → 1hr → 1day) is essential to keep storage tractable (168 TB raw → 10 TB active).
- **Forecasting ensemble (Prophet + ARIMA + Linear Regression)** — no single model works for all workload patterns. Prophet handles seasonality (weekly cycles, annual peaks). ARIMA handles stationary time series. Linear regression handles steady organic growth. The ensemble output is more robust than any individual model.
- **Procurement lead-time awareness** — this is the single most important concept the interviewer wants you to demonstrate. Bare-metal procurement takes 12–24 weeks. If your forecast shows GPU capacity exhaustion in 10 weeks, it's already too late to order. The system must fire procurement alerts at `exhaustion_date - lead_time` to give procurement enough runway. Cloud VMs have minutes of lead time and are handled differently.
- **20% headroom buffer target** — you never want to run a production fleet at 100% utilization. The buffer absorbs traffic spikes, hardware failures, and maintenance windows. 20% is the standard industry target for bare-metal infrastructure.

**Two-sentence differentiator:** Capacity Planning is the only system in this pattern that operates on a planning horizon (months) rather than a scheduling horizon (milliseconds to seconds). Its core uniqueness is bare-metal procurement lead-time awareness combined with multi-model ensemble forecasting — the system bridges demand predictions to purchase orders knowing that physical hardware has a 12–24 week delivery window that cloud VM autoscaling never has to worry about.

---

### Cluster Resource Manager

**What makes it unique:** This is the **policy and fairness layer** — it decides *which* workloads run and in *what order*, while delegating the *where* question to the Compute Allocator beneath it. Its uniqueness is **Hierarchical DRF** (mathematical multi-resource fairness) combined with **gang scheduling** (all-or-nothing placement for distributed ML).

**Unique decisions:**
- **DRF over FIFO or priority** — FIFO starves small jobs when large jobs dominate. Pure priority requires manual tuning and causes starvation. DRF is self-balancing: it automatically computes which team is using the most resources relative to their entitlement and schedules the most deprived team next. No manual priority assignment required.
- **Gang scheduling with PodGroup CRD** — a 64-GPU ML training job that places 32 workers and waits for the other 32 wastes 32 GPUs indefinitely. The PodGroup primitive holds all members in "waiting" until all can be placed simultaneously, then binds atomically. This is the right abstraction; implementing it as "just retry individual pod scheduling" leads to deadlocks.
- **Autoscale Controller with 10-minute scale-in cooldown** — scale-out is easy (new pending pods trigger node provisioning). Scale-in is dangerous (evicting workloads). The 50% utilization threshold with 10-minute dwell time prevents oscillation: you don't scale in immediately when utilization dips, because a burst may be imminent.

**Two-sentence differentiator:** The Cluster Resource Manager's uniqueness is Hierarchical DRF scheduling and all-or-nothing gang scheduling — it is the fairness and policy brain of the platform, operating above the physical placement layer. DRF equalizes dominant resource shares across the org hierarchy without manual priority tuning, while gang scheduling prevents distributed ML jobs from holding partial GPU allocations indefinitely while waiting for remaining workers.

---

### Compute Resource Allocator

**What makes it unique:** This is the **physical placement engine** — the lowest-level system that says "Workload X goes on physical Host Y." Its uniqueness is **5-dimensional dot-product alignment scoring** and **hardware topology awareness** (NUMA, NVLink GPU domains). While the Cluster RM decides policy, the Compute Allocator decides the exact physical mapping.

**Unique decisions:**
- **Dot-product alignment scoring over First Fit or Best Fit** — traditional bin packing algorithms (First Fit, Best Fit) optimize one dimension. The dot-product approach treats available resources and workload requirements as vectors and maximizes their alignment: a workload that needs mostly GPU aligns best with a host that has mostly available GPU. This minimizes the "leftover resource shape" problem where hosts are left with resource combinations no workload can use.
- **NUMA and GPU topology awareness** — for latency-sensitive workloads (HPC, real-time inference), memory access that crosses NUMA node boundaries has 2–3x higher latency. For ML training, GPUs on the same NVLink domain can communicate at 600 GB/s; GPUs across PCIe switches are bottlenecked at 16 GB/s. These topology constraints must be encoded in the placement scoring, not ignored.
- **Candidate sampling at 500** — filtering 50,000 hosts through predicates but scoring only a random sample of 500 survivors (same as Kubernetes' `percentageOfNodesToScore` feature) keeps placement latency under 200ms. Scoring all candidates would take too long. The randomness ensures long-term fairness across the candidate pool.
- **Defragmentation Engine** — over time, workload churn leaves "Swiss cheese" clusters: hosts with small pockets of free resources that no workload fits. The defrag engine runs async (every 15 minutes), identifies fragmented hosts, plans migrations that consolidate free space, and executes during approved windows. Target: < 15% fragmentation ratio.

**Two-sentence differentiator:** The Compute Resource Allocator's uniqueness is multi-dimensional dot-product alignment scoring across 5 hardware dimensions (CPU, RAM, GPU, disk, network) combined with NUMA and GPU topology awareness. Where other systems in this pattern manage policy and accounting, this one makes the physical bit-level decision of which exact host a workload runs on, using a scoring function that maximizes geometric alignment between available host resources and workload requirements.

---

### Quota and Limit Enforcement System

**What makes it unique:** This is the **admission gate** — it prevents over-allocation before it happens, not after. Its uniqueness is **admission-time enforcement via Kubernetes webhooks** (block before the pod exists, not after it's running), **hierarchical quota inheritance** with the min-of-parent semantics, and the **three-level cache** that makes < 10ms admission checks possible at 20,000/sec.

**Unique decisions:**
- **ValidatingWebhookConfiguration (not a runtime controller)** — runtime enforcement (e.g., killing workloads that exceed quota) is disruptive and hard to reason about. Admission-time enforcement rejects the workload before any resources are allocated. The webhook integrates directly into the Kubernetes API server flow: pod CREATE → webhook call → QES check → allow/deny. This is the production-grade pattern used in all large Kubernetes deployments.
- **`effective_hard_limit = min(parent_hard_limit, this_level_hard_limit)`** — a team's project cannot exceed the team's quota even if the project's own quota definition says it can. This prevents quota laundering (creating many sub-projects to accumulate resources that exceed the parent's entitlement). The inheritance logic must walk the hierarchy at check time.
- **Soft limits vs. hard limits** — hard limits block the request. Soft limits allow it but emit a warning event and notify the team. This matters operationally: you want teams to know they're approaching their limit before they hit it, not just get a rejection at 100%.
- **Burst quota** — some workloads legitimately need short-term overage (end-of-quarter ML experiments, holiday traffic prep). The `burst_limit` allows temporary overage for `burst_duration_seconds`, after which automatic reclamation begins. This beats the alternative (permanently high quotas that are only needed occasionally).

**Two-sentence differentiator:** The Quota System's uniqueness is admission-time enforcement via Kubernetes ValidatingWebhookConfiguration combined with three-level caching (in-process Caffeine → Redis → MySQL) that achieves < 10ms quota checks at 20,000/sec. Unlike the other systems which manage actual resource allocation, this system is purely about enforcement boundaries — preventing over-allocation before it happens using hierarchical inheritance semantics where a child scope can never exceed its parent's effective limit.

---

### Resource Reservation System

**What makes it unique:** This is the **temporal allocation system** — it deals with *future* resources, not current ones. A reservation says "I want 64 H100 GPUs from Tuesday 2 AM to 8 AM." The system must guarantee that those resources will be available at that future time, detect conflicts with other reservations for the same time window, and automatically activate the workload at the right moment. No other system in this pattern manages time as a first-class resource dimension.

**Unique decisions:**
- **Interval tree conflict detection** — the core algorithmic challenge. Two reservations conflict if their time intervals overlap AND they claim the same physical resources. The overlap condition is: `s1 < e2 AND s2 < e1` (the intervals do NOT fail to overlap). An interval tree (augmented BST ordered by start time, with max_end_time tracking at each node) gives O(log n + k) overlap queries, where k is the number of overlapping intervals. At 100,000 active reservations, this is essential; a naive linear scan would be O(n) per check.
- **Redis sorted set timer queue** — reservations need to activate at a specific future time. A cron job checking MySQL every minute would give up to 60 seconds of delay. Using Redis sorted sets with epoch timestamps as scores gives second-level precision: `ZADD timers {start_epoch} {reservation_id}`, then poll `ZRANGEBYSCORE timers 0 {now}` every second. The SLA is activation within 30 seconds of start_time.
- **Overbooking with configurable ratio** — airlines overbook flights because no-show rates are predictable. Similarly, if historical data shows 10% of reserved GPU capacity is cancelled before use, an overbooking ratio of 1.10x safely books 10% more than physical capacity. The `max_displacement_pct` (< 5%) and `compensation_policy` (refund/credit/reschedule) define what happens when overbooked reservations actually conflict at activation time.
- **Cron-based recurring expansion** — recurring maintenance windows ("every Tuesday 2–6 AM") are stored as templates with cron expressions. A daily background job expands each template into concrete reservation instances for the next 30 days. This rolling window approach means you never store thousands of future instances at once, but you always have 30 days of expanded reservations for conflict detection.

**Two-sentence differentiator:** The Resource Reservation System's uniqueness is time as a first-class allocation dimension — it uses interval tree conflict detection (O(log n + k)) for zero double-bookings and a Redis sorted set timer queue for activation within 30 seconds of start time. Unlike every other system in this pattern which manages the current state of resources, this system manages the future state — guaranteeing that resources will exist at a specified future time and automating the transition from reserved to active.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (Warm-Up)

**Q: What is DRF and why is it better than a simple priority queue for multi-tenant scheduling?**

**DRF (Dominant Resource Fairness)** is a multi-resource scheduling algorithm that equalizes each tenant's "dominant resource share" — the share of the cluster's most-used resource type for that tenant. A team using mostly GPU has GPU as their dominant resource; a team using mostly CPU has CPU as their dominant. DRF schedules the tenant with the lowest dominant share next. Priority queues require manual assignment and cause starvation; DRF is self-balancing and mathematically guarantees sharing incentive, envy-freeness, and Pareto efficiency. In a multi-resource environment (CPU + RAM + GPU), pure priority-based scheduling lets a high-priority team over-consume GPU while other teams wait indefinitely — DRF prevents this.

---

**Q: Why do you put quota checks in Redis instead of MySQL?**

The quota admission check must complete in under 10ms and handle 20,000 requests per second. MySQL with row-level locking under 20,000 concurrent usage-counter updates would experience severe lock contention — a single `UPDATE quota_usage SET current = current + 1` requires a row lock, and 20,000 such operations per second against the same row would queue into the hundreds of milliseconds. **Redis `INCRBY`** is an O(1) atomic operation that completes in under 1ms. MySQL remains the source of truth and receives usage snapshots every 30 seconds asynchronously, but it's never in the hot path for admission decisions.

---

**Q: What is gang scheduling and when do you need it?**

**Gang scheduling** (or co-scheduling) is the technique of placing all tasks in a distributed job simultaneously — all or nothing. You need it for distributed ML training jobs where all workers (say, 64 GPU workers) must communicate with each other during training. If worker 1 through 32 are placed and worker 33 through 64 cannot be placed, the first 32 sit idle waiting indefinitely, wasting 32 GPUs. With gang scheduling, the entire group of 64 stays pending until all can be placed atomically. The mechanism in Kubernetes is the PodGroup CRD (from the Volcano scheduler): all pods declare membership in a PodGroup, and the co-scheduling plugin holds them until the full group can be placed.

---

**Q: How does optimistic locking prevent double-allocation without using distributed locks?**

Every host record in MySQL has a `version BIGINT` column. An allocator reads a host's available capacity and version from Redis (fast path). When committing the allocation to MySQL, it runs: `UPDATE hosts SET used_cpu = used_cpu + ? WHERE host_id = ? AND version = ?`. If a concurrent allocator already modified the same host (incremented the version), this UPDATE returns 0 rows affected, signaling a conflict. The allocator then fetches fresh state from Redis and retries. No lock service is needed — MySQL's row-level locking handles the atomic check-and-update, and the version column detects when someone else won the race between read and write.

---

**Q: Why does the Capacity Planning system need to care about lead times?**

Because **physical hardware cannot be provisioned in minutes like cloud VMs**. A bare-metal GPU server (e.g., H100) has a procurement lead time of 12–24 weeks — you order it today, it arrives 3–6 months from now. If your forecasting model shows that GPU capacity will be exhausted in 10 weeks, you already missed the ordering window to fix it. The system must generate procurement alerts when `days_until_exhaustion ≤ lead_time_weeks × 7`, not when capacity is already gone. This is the fundamental reason capacity planning for bare-metal requires a forecasting system with months of horizon — cloud VM autoscaling (reacting in minutes) is a completely different architecture.

---

**Q: What is the difference between a hard limit and a soft limit in quota enforcement?**

A **hard limit** is an absolute ceiling — any workload creation that would cause `current_usage + requested_amount > hard_limit` is rejected at admission time with an error. No exceptions. A **soft limit** is a warning threshold — workload creation is allowed even if it pushes usage past the soft limit, but the system emits a warning event and notifies the team that they're approaching their hard ceiling. Soft limits give teams visibility into growing usage before they hit the wall, allowing proactive quota increase requests. In practice, soft limits are set at 80–90% of the hard limit so teams get early warning.

---

**Q: Why use an interval tree for reservation conflict detection instead of a simple SQL range query?**

Both work, but with different performance characteristics. A SQL query `WHERE start_time < ? AND end_time > ? AND host_id = ?` uses MySQL's B-tree index and runs in O(log n) for point queries but O(log n + k) with potential full-range scans when k (overlapping reservations) is large. An in-memory **augmented interval tree** always gives O(log n + k) where k is the number of actual overlaps — it never degrades. For 100,000 active reservations with potentially hundreds of overlaps during busy periods (shared maintenance windows), the interval tree avoids query plan instability. In practice, for the scale described (100,000 reservations), both approaches work; the SQL approach is simpler to implement and the interview answer shows you know both options.

---

### Tier 2 — Deep Dive Questions

**Q: How do you handle the case where multiple allocator instances race to place a workload on the same host — walk through the exact failure path and recovery.**

This is the double-allocation problem. Two allocator instances both read Redis at time T and see Host X with 8 free CPUs. Job A (5 CPU) and Job B (5 CPU) both select Host X as the best candidate. Instance 1 executes `UPDATE hosts SET used_cpu = used_cpu + 5, version = version + 1 WHERE host_id = X AND version = 7` — succeeds, version becomes 8. Instance 2 executes the same UPDATE with `version = 7` — returns 0 rows affected (version is now 8, not 7). Instance 2 detects the conflict: re-reads Host X from Redis (which will now show only 3 free CPUs after Instance 1's update propagates), and either finds a different host or queues Job B as pending. The key insight is that **Redis can be stale** (up to ~3 seconds behind) and that's acceptable for the *filter* phase, but MySQL with the version column is always authoritative for the *commit* phase. No distributed lock required.

**Trade-offs:** In highly contended clusters (few viable hosts for specialized workloads), retry rates spike. The fix is defragmentation — consolidate free space so more hosts are viable candidates, reducing collision probability.

---

**Q: Walk through the complete DRF scheduling decision for a hierarchical queue — how does the algorithm pick the next job?**

Starting from the root of the queue tree, at each level we pick the child queue with the **lowest dominant share**. Dominant share for a queue = `max(used_cpu/total_cluster_cpu, used_ram/total_cluster_ram, used_gpu/total_cluster_gpu)`. We recurse into the selected child. When we reach a leaf queue, we pick the job within that queue that has the lowest dominant share (ties broken by submit time — older jobs first). The recursive selection ensures fairness at every level: within Engineering (org), ML Team and Backend Team get fair shares. Within ML Team, Project Alpha and Project Beta get fair shares. The guarantee is: no entity at any level can monopolize resources indefinitely, because the moment their dominant share exceeds other entities at the same level, they stop being selected. **Preemption** extends this: if a new high-priority job joins a queue that's over its guaranteed allocation, the scheduler can evict lower-priority jobs from other queues to restore fairness.

---

**Q: How does the three-level cache in the Quota Enforcement Service work, and what are the failure modes at each level?**

**L1 (Caffeine in-process, 30-second TTL):** Stores quota definitions (the static limits) and computed effective limits (min of parent and self). Hit rate ~80% for frequently-checked namespaces. Failure mode: if a quota definition changes (admin increases a team's GPU limit), the L1 cache serves the old limit for up to 30 seconds. This is acceptable for increases (the team just can't use their new quota immediately) but potentially dangerous for decreases (the team might launch workloads above their new lower limit for 30 seconds). Mitigation: cache invalidation events via Redis pub/sub push immediate eviction on limit decreases.

**L2 (Redis Cluster):** Stores real-time usage counters (updated atomically on every pod create/delete) and quota definitions for scopes not in L1. Hit rate ~18%. Failure mode: Redis partition or failure. Fallback: if Redis is unavailable, fall through to MySQL. Under Redis failure, quota check latency increases from 2ms to 20–50ms but remains functional. Rate limiting is completely unavailable during Redis outage (token buckets live in Redis), which requires the API Gateway to fail open or close depending on security policy.

**L3 (MySQL):** The authoritative source, handles ~2% of reads. Hit rate 100% (no miss — MySQL always has an answer). Failure mode: MySQL unavailability. This is a critical failure — the quota enforcement service cannot make admission decisions. The mitigation is MySQL replication with automatic failover (orchestrator/ProxySQL) and emergency mode where L1 cache is extended to 10-minute TTL.

---

**Q: How do you prevent gang scheduling deadlocks in a cluster where multiple large gangs are all waiting for resources that never free up?**

This is a real cluster deadlock scenario: Gang A needs 64 GPUs, Gang B needs 64 GPUs, and the cluster has 128 GPUs but they're fragmented across 4 gang attempts — each holding 32 GPUs in "waiting" state while waiting for the other 32. Classic deadlock.

Solutions: First, **gang timeout with graceful degradation** — if a gang hasn't placed within N minutes, reduce its `min_task_count` (e.g., from 64 to 48 workers, which can still run with data parallelism) and retry. Second, **preemption with gang awareness** — the scheduler can preempt lower-priority gangs to free enough resources for a higher-priority gang to place completely. Third, **cluster-level gang quorum** — when N large gangs are all waiting and together they exceed cluster capacity, the scheduler selects the highest-priority gang and attempts to preempt everything below it in one atomic operation. Fourth, **time-limited holding** — pods in gang-waiting state occupy no real resources but do occupy a "slot" in the pending queue; after a timeout, they're returned to pending state and re-queued at lower priority to prevent queue starvation.

---

**Q: How does interval tree conflict detection work for reservations, and how does it scale?**

The data structure is an **augmented BST** where each node represents a reservation interval `[start_time, end_time]` on a specific host, nodes are ordered by `start_time`, and each node stores `max_end_time` — the maximum end time in its subtree. This augmentation enables efficient overlap queries.

For a new reservation `[s_new, e_new]` on a specific host, the overlap query is: two intervals `[s1, e1]` and `[s2, e2]` overlap if and only if `s1 < e2 AND s2 < e1`. The tree traversal starts at the root: if `root.max_end_time ≤ s_new`, no interval in this subtree overlaps (prune entire subtree). Otherwise, check root for overlap and recurse into both children selectively. This gives O(log n + k) time where k is the number of actual overlaps.

In MySQL, we implement the same logic using the `idx_timeline_host_time (host_id, start_time, end_time)` composite index with the query `WHERE host_id = ? AND start_time < e_new AND end_time > s_new`. At 100,000 active reservations, this query hits the B-tree index and runs in 10–40ms — within our 50ms p99 SLA. For scales beyond 1M reservations, you'd move the interval tree fully in-memory in Redis as a sorted set and maintain it as reservations are created/cancelled.

---

**Q: Why does the Compute Resource Allocator use dot-product scoring rather than a simpler scoring function like "minimize unused resources"?**

The "minimize unused resources" heuristic (Best Fit) tries to find the host where the workload fits most tightly — leaving the smallest residual space. In a single-dimension problem (only CPU), this works. In 5 dimensions (CPU, RAM, GPU, disk, network), it creates a fragmentation problem: a workload that needs mostly CPU gets placed on a host with a lot of available GPU, leaving a host with partially used GPU that nothing else can use.

**Dot-product alignment** solves this by maximizing the geometric alignment between the *shape* of available resources and the *shape* of the workload request. If a host has `[8 CPU, 4 RAM, 2 GPU]` available (mostly CPU, some RAM, a little GPU) and a workload needs `[6 CPU, 3 RAM, 0 GPU]` (CPU and RAM heavy, no GPU), the dot product of their normalized vectors is high — they're well-aligned shapes. Placing this workload on a GPU-heavy host wastes the GPU shape. The formula: `alignment = dot(A_norm, R_norm)` where A_norm and R_norm are the unit vectors of available resources and request, respectively. Combined with utilization, affinity, NUMA, and GPU topology scores (weighted 0.35/0.30/0.15/0.10/0.10), this captures the full placement objective. Google Borg uses a similar approach and reports 85–90% of theoretical optimal utilization.

---

### Tier 3 — Staff+ Stress Test Questions

**Q: Your ML training cluster is at 95% GPU utilization. A researcher submits a 128-GPU gang job with priority "critical". There are 20 currently running "high" priority jobs each using 8 GPUs. Walk through exactly what happens — including preemption, ordering, state transitions, and what you'd do differently if this were a production serving cluster instead of a training cluster.**

**In the training cluster:** The gang job enters the pending queue. The scheduler evaluates: 128 GPUs needed, only ~6 GPUs free (5% of 128 GPUs). The gang cannot place without preemption. Priority "critical" > "high" means preemption is authorized. The scheduler identifies the minimum set of high-priority jobs to preempt: preempting 16 high-priority jobs frees 128 GPUs. Selection: DRF identifies the 16 jobs with the highest dominant share (they've been running longest and consumed the most). Preemption sequence: set job status to `preempting`, call eviction manager, node agents receive SIGTERM + 30-second grace period (checkpoint state to shared storage if ML training), pods evicted. MySQL updated: 16 jobs transition to `preempted`, GPUs freed. Gang scheduler polls and detects 128 GPUs now available, atomically places all 128 pods. The 16 preempted jobs re-enter the pending queue with their checkpoint paths stored, they'll resume from the last checkpoint when GPUs free up.

**What changes for a production serving cluster:** You'd never use preemption-by-default on production serving workloads. Preempting a production inference service causes real user-facing latency spikes or errors — there's no checkpoint to resume from, it's just a dropped request. In production, you'd: (1) set all serving workloads as non-preemptible, (2) route the gang job to a separate training cluster or wait for organic capacity, (3) use the Cluster Autoscaler to provision new GPU nodes (if cloud), or (4) trigger a capacity alert for the on-call if bare-metal. The critical priority designation would still apply within the training-workload priority tier but would not cross the serving/training isolation boundary.

---

**Q: Design the reconciliation mechanism that keeps Redis in sync with MySQL for the Compute Resource Allocator. What are all the ways they can diverge, and how do you detect and fix each one?**

Redis and MySQL diverge in several distinct scenarios, each requiring a different repair strategy.

**Scenario 1: Node agent heartbeat drops** (agent crashes, network partition). Redis entry for the host stops being updated. MySQL has the authoritative node status that the Node Monitor updates after 40 seconds of silence. Fix: the reconciliation loop (every 60 seconds) reads all node statuses from MySQL and pushes them to Redis. Interim: when a placement attempt fails at the MySQL bind step because the node is NotReady in MySQL but Ready in Redis, immediately invalidate the Redis entry (targeted invalidation on write failure).

**Scenario 2: Optimistic lock retry with partial Redis update**. Allocator A reads Host X state, wins the MySQL race, increments `used_cpu`. Redis is updated 3 seconds later via heartbeat. During that 3 seconds, Allocator B reads the old Redis state and might attempt the same host. Fix: this is intentional — the optimistic lock on MySQL prevents double-allocation even when Redis is stale. No divergence repair needed; the version mismatch is the repair signal.

**Scenario 3: MySQL write succeeds but Redis update fails**. An allocator commits to MySQL, then tries to update Redis but Redis times out. Now MySQL says the host has less free CPU than Redis claims. Fix: every placement request proactively reads the allocation from MySQL if the Redis hit rate for that host drops below expected (cache miss signals potential inconsistency). The 60-second reconciliation loop is the backstop — it reads MySQL as authoritative and overwrites Redis.

**Scenario 4: Redis node failure with data loss (if persistence is disabled)**. Fix: don't disable Redis persistence. Use AOF (append-only file) with `fsync` every second. On Redis restart, replay AOF to recover last-second state. Full reconciliation from MySQL immediately after Redis comes back online.

**The invariant that makes this all safe:** Redis can show *more* free resources than MySQL (stale, before MySQL was updated), but it can never show *fewer* (MySQL updates reduce Redis via targeted invalidation). This asymmetry means the worst-case Redis divergence causes placement *attempts* to fail at the MySQL commit phase — a retry — never actual double-allocation.

---

**Q: You're running a 50,000-host mixed CPU/GPU fleet and a new workload type appears: very short-lived jobs (< 30 seconds) that need 4 CPUs and 8 GPUs each, submitted at 500 jobs/second. Your current system is designed for workloads that run for hours. Walk through all the ways this breaks your existing design and how you'd adapt.**

**Problem 1: Scheduling overhead dominates job duration.** At 500 jobs/second × 200ms p99 placement latency, you'd need 100 concurrent allocator threads just to keep up. For a 30-second job, 200ms scheduling overhead is 0.7% of job duration — acceptable. But MySQL write throughput for 500 placements/sec (allocation) + 500 terminations/sec (deallocation) = 1,000 writes/sec is fine. Problem: MySQL also stores placement history; at 500 jobs/sec over 30 days, that's 1.3 billion records. Purge aggressively.

**Problem 2: Redis hot-key contention on popular hosts.** 500 jobs/sec competing for a small set of GPU hosts creates Redis hot keys (each host's `used_gpu` counter). 500 atomic INCRBYs/sec to the same key is within Redis single-threaded throughput but creates latency spikes. Fix: **shard the host state across multiple Redis key slots** using consistent hashing, or use Redis Cluster with host IDs as natural shards.

**Problem 3: Node agent heartbeat becomes stale relative to job duration.** A 3-second heartbeat interval means a host's state can be 3 seconds old. For a 30-second job, this means the scheduler might see a "stale" view of 3 jobs (3s × 1 job/sec/host = 3 jobs) that have already finished. The host appears more full than it is, causing unnecessary "no placement found" responses. Fix: **event-driven cache invalidation** instead of heartbeat-only updates. When a job terminates, the node agent immediately pushes a capacity update to Redis (don't wait for the next heartbeat). This converts heartbeat-driven state from 3-second staleness to sub-second.

**Problem 4: Gang scheduling becomes irrelevant but queue fairness is critical.** 500 single-task jobs/sec don't need gang scheduling, but DRF fairness becomes critically important — without it, one tenant submitting 500 short jobs/sec would dominate the GPU cluster. The DRF algorithm naturally handles this: the tenant's dominant share grows rapidly with each placement, deprioritizing them until other tenants' backlog clears.

**Problem 5: MySQL connection pool exhaustion.** 500 placements + 500 terminations/sec = 1,000 concurrent write transactions. With 10ms average transaction time, you need at least 10 concurrent connections to sustain this. MySQL connection pools (ProxySQL) size to 50–100 connections easily. Not a problem at this scale.

**Architectural adaptation:** Add event-driven capacity update from node agents (supplement heartbeats), aggressive placement history purging (keep 7 days instead of 30), and per-host Redis sharding for hot GPU hosts.

---

**Q: The interval tree for reservation conflict detection gives O(log n + k) complexity. Describe a scenario at scale where this becomes a bottleneck and what you'd do about it.**

The O(log n + k) complexity hides a critical variable: k, the number of conflicting intervals returned. In normal operation, k is small (0–5 conflicts per query). The bottleneck emerges in two scenarios:

**Scenario A: Maintenance window clustering.** Many organizations schedule maintenance windows for the same time (Sunday 2–4 AM UTC is an industry cliché). If 10,000 recurring reservations all expand to overlapping Sunday-morning windows, a query for a new Sunday morning reservation returns k = 10,000 conflicts. The O(log n + k) traversal is now O(log 100,000 + 10,000) ≈ O(10,000) — effectively linear for this query. Fix: **time-bucket index**. Partition reservations into hourly time buckets. A new Sunday-2AM reservation only queries the Sunday-2AM bucket (subset of all reservations), reducing n and k dramatically.

**Scenario B: High-cardinality host × time queries.** Conflict detection queries are per-host (`WHERE host_id = ?`). At 50,000 hosts × 100,000 active reservations, some hosts may have thousands of reservations (shared maintenance hosts). For these "hot hosts," even the B-tree index yields k in the hundreds. Fix: **per-host in-memory interval trees** maintained as a Redis sorted set per host ID, with the conflict detection service loading the relevant host's tree into memory on first query and caching it with a short TTL. This turns a disk-bound B-tree query into an in-memory interval tree traversal.

**The deeper answer:** At scale, the right solution is to separate the conflict detection data structure from MySQL entirely. Keep MySQL as the authoritative store of reservation records, but maintain a dedicated in-memory conflict detection service that loads all active reservation intervals at startup (100,000 × 200 bytes = 20MB) and serves all conflict checks from memory, with incremental updates on reservation create/cancel events via Kafka. This gives sub-millisecond conflict detection at any k value.

---

## STEP 7 — MNEMONICS

### The CREAM Stack (what every resource management system needs):

**C** — **Cache layering** (L1 in-process → L2 Redis → L3 MySQL)
**R** — **Resource dimensions** (CPU, RAM, GPU, Disk, Network — always 5D)
**E** — **Enforcement at admission** (reject before allocation, not after)
**A** — **Audit trail** (state machine transitions, full history, every change logged)
**M** — **MySQL as truth** (Redis is fast, MySQL is right — never confuse the two)

If you forget anything in the interview, ask yourself "does my design have CREAM?" — it covers 80% of the common missed requirements.

---

### The Five Systems as a Value Chain:

**Plan → Manage → Place → Enforce → Reserve**

- **Plan** (Capacity Planning): How much hardware do we need to buy in 6 months?
- **Manage** (Cluster RM): Which team's workloads run, in what order, with what fairness?
- **Place** (Compute Allocator): Which exact physical host does each workload go on?
- **Enforce** (Quota System): Does any tenant try to take more than their share?
- **Reserve** (Reservation System): Can I guarantee specific resources at a future time?

Each system in the chain answers one question. When an interviewer asks about "resource management," identify which question in the chain they're asking before designing anything.

---

### Opening one-liner for any resource management question:

"The core challenge in resource management is that you need strong consistency to prevent double-allocations — two requestors claiming the same resource — but you need sub-10ms read latency to keep admission fast. The pattern that solves this is: read from Redis on the fast path for filtering, commit to MySQL with optimistic locking for the binding decision, and reconcile the two stores periodically. Everything else in the design flows from this fundamental tension."

---

## STEP 8 — CRITIQUE

### What the source material covers well:

- The optimistic locking pattern is explained precisely with the exact SQL and the failure/retry path
- DRF algorithm is covered with the math, the code skeleton, and the hierarchy extension
- The three-level caching architecture (Caffeine → Redis → MySQL) is detailed with TTLs and invalidation strategies
- Capacity estimation numbers are internally consistent and can be reproduced from first principles
- The NFRs are differentiated by system (99.99% for real-time schedulers, 99.9% for advisory planning)
- Gang scheduling is explained with the practical failure mode (partial placement waste) and the solution (PodGroup CRD)
- The interval tree conflict detection algorithm includes both the big-O analysis and the MySQL fallback implementation

### What is shallow or underexplored:

**Multi-cluster federation** is mentioned in the Cluster RM but the routing algorithm (how does the federation controller decide which cluster gets a workload?) is hand-waved. In a real interview, you'd need to discuss: least-loaded-first vs affinity-preferred routing, handling cross-cluster network bandwidth as a constraint, what happens when no cluster has capacity (queue at federation layer vs fail fast), and consistency of federation state across cluster managers.

**Kubernetes Vertical Pod Autoscaler (VPA)** is absent. At FAANG scale, workloads regularly right-size their resource requests based on observed usage. VPA interacts with quota enforcement (does a VPA-triggered resize need to be admitted through the quota webhook?) and with the allocator (a resize requires potentially migrating the pod if the current host can't accommodate larger requests). This gap would be probed at senior/staff level.

**The autoscaler's interaction with bare-metal procurement** is not connected. The Autoscale Controller triggers node provisioning; for cloud this takes 30 seconds, but for bare-metal nodes that take 5–30 minutes to provision (and don't exist at all if capacity planning said not to buy them), the autoscaler's behavior needs to be fundamentally different. The source material treats these as the same pathway, which is incorrect.

**Quota enforcement for ephemeral storage** (disk space used by running containers, not PVCs) is mentioned in the schema but not designed in detail. Ephemeral storage is hard to track in real-time and eviction policies are node-local — the global quota system can't easily enforce it at sub-30-second granularity.

### Senior probes that would expose gaps:

- "How does a quota increase for a team propagate to all their running workloads? Does a previously rejected pod become schedulable automatically?" (Answer: no, the user must re-submit; the quota system is admission-time only, not retroactive)
- "What happens to a gang job that's waiting in the pending queue when the cluster autoscaler provisions new nodes — how does the gang scheduler get notified that it should retry?" (Watch for: polling vs event-driven wake-up; Kubernetes uses watch on node objects)
- "If a node fails mid-defragmentation (while a live migration is in flight), what is the recovery path?" (Watch for: the migration destination takes over, source is now effectively crashed; must handle the case where neither old nor new placement is the authoritative running location)
- "How do you prevent the approval workflow for quota requests from becoming a bottleneck when a manager has 200 pending requests?" (Auto-approval rules for small increases below a threshold, delegation chains, escalation on SLA breach)

### Traps to avoid:

**Trap 1: "Redis is the source of truth"** — Redis is a cache. The moment you treat it as authoritative, you can lose data on node failure, get incorrect quota counts during Redis partitions, and have no transactional guarantees for concurrent writes. MySQL is authoritative. Redis is fast. Never swap these roles.

**Trap 2: "Just use Kubernetes for everything"** — Kubernetes doesn't solve capacity planning with bare-metal lead times, doesn't natively handle DRF hierarchical fairness (you need Volcano or a custom scheduler), and doesn't manage reservations with time-based conflict detection. Kubernetes is the workload execution layer; the systems in this pattern sit above it.

**Trap 3: "Quota enforcement at runtime is equivalent to admission-time enforcement"** — It is not. Runtime enforcement (killing a workload that exceeds quota) is disruptive: production services go down, training jobs lose progress, engineers lose trust in the platform. Admission-time enforcement via webhooks is the right approach — it rejects the workload before any resources are touched.

**Trap 4: "Gang scheduling is just batch submission"** — Batch submission places pods one at a time and allows partial placement. Gang scheduling requires atomic all-or-nothing placement. The distinction is critical for ML training: partial placement wastes GPU and blocks progress. Saying "just submit the pods and let the scheduler figure it out" shows you haven't thought through the partial-placement deadlock scenario.

**Trap 5: "DRF and priority are alternatives"** — They're not mutually exclusive. Priority classes (system-critical, high, medium, low, preemptible) define preemption tiers. DRF operates within a priority class to provide fairness among equal-priority tenants. A correct design uses both: preemption to reclaim resources for critical jobs, DRF to fairly distribute the remaining capacity among everyone else.

---

*End of INTERVIEW_GUIDE.md — Infra Pattern 3: Resource Management*
