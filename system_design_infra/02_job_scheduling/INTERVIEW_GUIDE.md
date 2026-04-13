# Infra Pattern 2: Job Scheduling — Interview Guide

Reading Infra Pattern 2: Job Scheduling — 5 problems, 7 shared components

---

## STEP 1 — OVERVIEW

This pattern covers five closely related but distinct system design problems: **Cron Service at Scale**, **Deadline-Aware Scheduler**, **Distributed Job Scheduler**, **Priority-Based Job Scheduler**, and **Task Queue System**. They all belong to the "job scheduling" domain but represent meaningfully different engineering challenges. A distributed job scheduler is the foundation that the others build on. The cron service sits on top of it and handles time-based triggering. The priority-based scheduler adds urgency-aware dispatch to the same foundation. The deadline-aware scheduler adds feasibility guarantees and preemption based on SLA constraints. The task queue system is the lightest-weight variant, designed for short-lived async tasks with workflow primitives.

What they all share: **MySQL as the durable source of truth**, **Redis sorted sets for efficient O(log N) queue operations**, **distributed locks for exactly-once dispatch**, **exponential backoff and dead-letter queues for failure handling**, **tenant quotas for multi-tenancy**, and **heartbeat-based worker liveness detection**. The shared substrate is around 7 components. The differentiators are what will separate a good answer from a great one.

Why this domain appears at FAANG/NVIDIA: every infrastructure platform runs scheduled work — certificate rotation, database backups, log rotation, capacity reports, provisioning pipelines, health checks. Getting these wrong means missed SLAs, expired certs, and outages. Getting them right at 10M jobs and 100K/min triggers is genuinely hard.

---

## STEP 2 — MENTAL MODEL

**One core idea: a scheduler is a priority queue that spans time and machines.**

Every scheduler reduces to the same abstraction: you have a set of work items, each with a readiness condition (time, dependency, priority, deadline), and a pool of resources. The scheduler's job is to match work items to resources in the right order at the right time, exactly once, and recover gracefully when things fail.

**Real-world analogy: an air traffic controller.**

An air traffic controller manages a finite number of runways (resources) against an incoming queue of planes (jobs) that each have a scheduled landing window (time trigger), a priority (emergency vs. commercial vs. private), a hard deadline (fuel remaining), and dependencies (sequenced landings based on weather and spacing). They predict which planes will miss their window, preempt lower-urgency traffic for emergencies, and ensure the same plane never gets cleared to land twice (exactly-once). Scale that to 10 million planes per day and you have a job scheduler.

**Why it is hard in distributed systems:**

The naive single-machine scheduler (Linux cron, Windows Task Scheduler) has a single clock, a single thread, and no concurrency hazards. The distributed version must solve: **duplicate execution** (two scheduler instances fire the same job simultaneously), **missed triggers** (scheduler restarts between trigger time and dispatch), **clock skew** (machines disagree on "now" by tens of milliseconds), **thundering herd** (half a million jobs fire at midnight UTC simultaneously), **worker failures** (the machine running your job dies mid-execution), and **head-of-line blocking** (one massive slow job blocks thousands of fast jobs). Each of these has a specific, non-obvious solution, and interviewers know which ones candidates skip.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these before touching the whiteboard. The answers fundamentally change the architecture.

1. **"What is the expected scale — number of registered jobs, peak trigger rate, and number of concurrent executing jobs?"**
   What changes: 100 jobs/day on one machine versus 10M jobs at 100K/min require completely different trigger detection strategies. The Redis sorted set approach only pays off at scale. Small-scale can use database polling.

2. **"What are the execution semantics you need — at-least-once, at-most-once, or exactly-once? And does the system need to handle DAG dependencies or just flat one-shot / recurring jobs?"**
   What changes: at-least-once is much easier (no distributed lock coordination); DAG support requires topological sort infrastructure and dependency tracking, which is a separate system.

3. **"Are there deadline constraints — do some jobs have hard SLA boundaries they cannot miss? And do you need preemption of lower-priority work to meet them?"**
   What changes: if yes, you need a feasibility checker at submission time, a resource reservation system, and a laxity-based urgency recalculation loop. If no, you can use a simpler priority queue.

4. **"Are jobs short-lived tasks (seconds to minutes) or long-running batch jobs (hours)? And do jobs carry large payloads or just a reference?"**
   What changes: short tasks favor a lightweight broker like RabbitMQ with in-memory message routing. Long-running jobs need persistent execution records, timeout detection, and mid-job checkpointing consideration. Large payloads must be stored externally and referenced by pointer.

5. **"Is this multi-tenant — do different teams or customers share the same scheduler infrastructure? Do you need quota enforcement?"**
   What changes: yes means you need per-tenant quota tables, rate limiting on submission APIs, and tenant-scoped isolation to prevent a noisy tenant from starving others.

**Red flags in answers:**
- "We can handle duplicates in the application layer" — this usually means the jobs are not idempotent and the interviewer is testing whether you'll push back.
- "We'll just have one scheduler instance" — this kills availability. Ask what 99.99% uptime means to them.
- "Real-time SLAs" — if they say "real-time" in the RT/OS sense, clarify immediately. This design pattern handles soft and firm real-time, not hard real-time with mathematical proofs.

---

### 3b. Functional Requirements

**Core requirements to state explicitly:**

- Job submission API: accept job definition (command/image, resource request, schedule, priority, dependencies)
- At-least-once execution guarantee by default; exactly-once via idempotency key
- Full job lifecycle management: create, queue, dispatch, run, succeed, fail, retry, cancel, pause, resume
- Configurable retry with exponential backoff; dead-letter queue for permanently failed jobs
- Real-time job status, execution history, and log access
- Multi-tenant isolation with per-tenant quotas
- Scheduler high availability: 99.99% uptime with fast leader failover

**Scope decisions to make explicit:**

- Are you building the cron trigger layer (time-based), the dispatch layer (worker assignment), or both?
- Do you need DAG dependency tracking or flat job scheduling?
- Do you need deadline-aware prioritization or simple FIFO/priority?
- Are tasks short-lived (< 1 hour) or long-running batch jobs?

**Clear problem statement:** "We are designing a distributed job scheduler that accepts job submissions from 500 teams, maintains durable state, dispatches jobs to a heterogeneous worker fleet, handles failures with retries, and guarantees at-least-once execution with sub-second scheduling latency at 100K submissions per minute."

---

### 3c. Non-Functional Requirements

**How to derive them:** do not memorize numbers. Derive them from the problem statement.

| NFR | Derivation | Target |
|-----|-----------|--------|
| Availability | Missed schedules directly cause SLA violations. Certificate rotation not running → cert expires → outage. | 99.99% (< 52 min/year downtime) |
| Scheduling latency | A cron job that fires 30 seconds late is a problem for health checks and cert rotation. A batch job can tolerate more. | < 500ms P99 from trigger time to dispatch for high-priority; < 5s end-to-end for cron |
| Throughput | Many cron jobs fire at :00 of every minute; midnight UTC is worst-case. | 100K triggers/min peak; 500K/min task queue enqueue |
| Durability | An accepted job that gets lost before execution is a contract breach. | Zero job loss after acknowledgement |
| Consistency | Duplicate execution of provisioning jobs can leave infrastructure in a bad state. | No duplicate execution (exactly-once via distributed lock + optimistic lock) |

**Key trade-off to surface unprompted:** "Availability and exactly-once are in tension. To guarantee 99.99% availability, we run multiple scheduler instances. Multiple instances create duplicate dispatch risk. We resolve this with a two-layer lock: Redis SETNX for fast rejection and MySQL CAS update for authoritative exactly-once. The trade-off is that we accept the cost of a Redis round-trip on every dispatch in exchange for the safety of not duplicating jobs."

---

### 3d. Capacity Estimation

**Anchor numbers and derivation:**

```
Scale input:
  500 teams × 20K jobs/team = 10M registered cron jobs
  20% active at any time = 2M active
  Average 50 triggers/day per active job → 100M triggers/day
  = ~1,157 triggers/second average
  Peak at midnight UTC: 500K triggers in under 1 minute → ~8,333/second peak

Storage:
  Job definitions: 2 KB × 10M = 20 GB (warm forever)
  Execution records: 1 KB × 50M/day = 50 GB/day → 4.5 TB at 90-day retention
  Trigger history: 200 B × 100M/day = 20 GB/day → 600 GB at 30 days
  Redis sorted set (next-fire-time): 50 B × 2M active = 100 MB (fits in memory trivially)

Bandwidth:
  Peak dispatch traffic: 100K/min × 2 KB = 200 MB/min ≈ 3.3 MB/s
  Worker heartbeats: 10K workers × 1 KB × 1/10s = 1 MB/s
  Status updates: 1M running × 1 KB × 1/30s = 33 MB/s
  Total: ~37 MB/s inbound — no exotic networking needed
```

**Architecture implications:**
- 4.5 TB of execution history requires MySQL RANGE partitioning on `created_at` (quarterly partitions). Without partitioning, purging old records via DELETE is catastrophically slow.
- 100 MB Redis sorted set fits in a single Redis instance with room to spare. No Redis Cluster needed for the queue itself.
- 37 MB/s inbound bandwidth is trivially handled by any modern 10 GbE NIC. This is a latency problem, not a bandwidth problem.
- 3.3 MB/s peak dispatch means the scheduler control plane runs fine on a few instances. The bottleneck is database write throughput at peak.

**Time estimate in interview:** spend 3-4 minutes on this. Do not spend more — it signals insecurity. The point is to anchor the architecture on real numbers, not to be precise.

---

### 3e. High-Level Design

**Four to six components and their roles:**

1. **API Gateway / Load Balancer** — authenticates JWT tokens, rate-limits per-tenant (1000 req/min default), routes management-plane requests. Does not participate in dispatch.

2. **Scheduler Cluster (Leader + Standby)** — exactly one leader runs the trigger evaluation loop and dispatch engine. 2-4 standby instances participate in leader election via etcd. On leader failure, etcd lease expiry triggers a new election and a standby takes over in seconds. Standby instances serve read traffic (status queries) while not leading.

3. **Job Queue (MySQL + Redis)** — MySQL is the durable source of truth for job definitions and execution records. Redis sorted set (score = next_fire_time_ms or effective_priority) enables O(log N) dequeue. Redis is the hot path; MySQL is the recovery path after crashes.

4. **Dispatch Engine** — consumes from the Redis queue, matches jobs to available workers by resource requirements (CPU, memory, GPU), acquires a distributed lock, persists a job_execution record to MySQL, and sends the job to the worker via gRPC. Releases the lock only after the worker acknowledges receipt.

5. **Worker Fleet** — heterogeneous pools (general, GPU, high-memory). Workers register on startup, send heartbeats every 10 seconds, and report job completion. Workers do not share state with each other.

6. **Heartbeat and Status Collector** — tracks worker liveness. Workers silent for > 30 seconds are marked SUSPECT; after a second timeout, they are marked DEAD and their in-progress jobs are re-queued for retry.

**Data flow on the happy path:**
Client → API Gateway → Scheduler writes job_definition to MySQL → Schedule Evaluator marks job QUEUED → Dispatch Engine acquires lock, inserts job_execution, sends to worker via gRPC → Worker executes, sends completion event → Status Collector updates job_execution to SUCCEEDED → (if DAG) DAG Resolver unblocks downstream tasks.

**Whiteboard order:** draw the client and API gateway first, then the scheduler cluster box, then split it into evaluator + dispatch engine, then add the job queue in the middle, then add the worker fleet at the bottom, then add MySQL + Redis + etcd at the bottom. Add Elasticsearch off to the side for observability. Draw the data flows as arrows after the boxes are in place.

**Key decisions to call out explicitly:**
- "Only the leader runs the trigger loop to prevent duplicate dispatches."
- "Redis sorted set is O(log N + K) per scan versus O(N) for polling all jobs."
- "MySQL is the source of truth; Redis is rebuilt from MySQL on scheduler restart."
- "gRPC between dispatcher and workers for low-latency job delivery and streaming heartbeats."

---

### 3f. Deep Dive Areas

**Area 1: Exactly-Once Dispatch (most-probed topic)**

**Problem:** If two scheduler instances scan the Redis sorted set simultaneously, they will both see the same due job and both attempt to dispatch it, resulting in duplicate execution. Worker failure between dispatch and acknowledgement must also not cause the job to be silently dropped.

**Solution — two-layer locking:**

First layer: Redis SETNX with TTL. Before dispatching job J that is due at time T, the scheduler acquires `SETNX lock:dispatch:{job_id}:{scheduled_time_ms} {instance_id} EX 60`. Only the instance that wins this atomic set-if-not-exists proceeds. All others see the key already exists and skip. The 60-second TTL ensures the lock auto-expires if the winning scheduler crashes before completing dispatch.

Second layer: MySQL optimistic lock. The winning instance executes:
```sql
UPDATE job_execution
SET status = 'DISPATCHED', dispatched_at = NOW(), worker_id = ?, version = version + 1
WHERE execution_id = ? AND status = 'QUEUED' AND version = ?
```
If this UPDATE affects 0 rows, another instance already advanced the status. The current instance aborts and releases its Redis lock. This provides the authoritative exactly-once guarantee even if the Redis lock somehow has a race window.

**Trade-offs to state unprompted:**
- ✅ Two-layer approach gives strong exactly-once guarantees with sub-millisecond Redis fast-path rejection.
- ❌ Every dispatch requires two round-trips (Redis + MySQL), adding ~2-5ms latency.
- ✅ The TTL on the Redis lock prevents permanent lockout if the scheduler crashes.
- ❌ If the Redis lock TTL (60s) expires before MySQL is updated, a second instance could acquire the lock and create a brief window of duplicate dispatch. This is mitigated by the MySQL CAS being the authoritative check.

---

**Area 2: Thundering Herd at Midnight UTC (cron-specific but generalizable)**

**Problem:** At midnight UTC, daily cron jobs across all tenants fire simultaneously. With 2M active cron jobs and many using `0 0 * * *`, this can mean 500K triggers wanting to fire in under 60 seconds. A naive single-threaded scan will back up and cause massive delays.

**Solution — three-part mitigation:**

(1) **Batch-limited ZRANGEBYSCORE:** The trigger loop scans `ZRANGEBYSCORE cron:next_exec 0 {now_ms} LIMIT 1000` every second. This bounds the number of jobs processed per cycle to 1000 and distributes the remaining jobs to subsequent seconds. The sort order ensures oldest-due jobs are processed first within each batch.

(2) **Parallel trigger processing:** Each batch of 1000 due jobs is processed in parallel by a thread pool (16-thread pool works well). Lock acquisition and job submission happen concurrently, so the actual serial bottleneck is just the Redis ZRANGEBYSCORE call itself.

(3) **Per-tenant rate limiting:** Each tenant has a `max_triggers_per_min` quota (e.g., 1000). The trigger engine checks this quota before dispatching. A single misconfigured tenant with 100K `*/1 * * * *` jobs cannot saturate the dispatch engine.

**Bonus technique — submission-time jitter:** When a new cron job is created, you can add ± N seconds of jitter to its next_fire_time so that 10,000 jobs all scheduled for midnight don't all have exactly the same score in the sorted set. This naturally distributes them across a window. Trade-off: you lose precise-to-the-second triggers, which is usually acceptable.

---

**Area 3: Worker Failure and Job Recovery**

**Problem:** A worker picks up a job and starts executing it. Halfway through, the worker's host crashes. The job is left in DISPATCHED or RUNNING status with no heartbeat. How do you detect this and recover the job without losing it or running it twice?

**Solution — heartbeat-based state machine:**

Workers send heartbeats every 10 seconds. The Heartbeat Collector maintains a `last_heartbeat` timestamp per worker. A background job scans every 15 seconds for workers where `last_heartbeat < NOW() - 30s`. These are marked SUSPECT. After a second scan period (another 30s of silence), they are marked DEAD.

When a worker is marked DEAD, all its jobs in DISPATCHED or RUNNING status are automatically re-queued:
```sql
UPDATE job_execution
SET status = 'RETRYING', attempt_number = attempt_number + 1
WHERE worker_id = ? AND status IN ('DISPATCHED', 'RUNNING') AND attempt_number < max_retries
```
Jobs that have exhausted retries are moved to DEAD_LETTERED.

**Trade-offs:**
- ✅ The 30-second detection window bounds the maximum time a dead job sits unrecovered.
- ❌ Legitimate long-running jobs may have heartbeat gaps if the worker is CPU-saturated. Differentiate heartbeat timeout from job timeout by having workers send heartbeats on a separate thread, not the execution thread.
- ✅ Two-stage SUSPECT → DEAD prevents false positives from brief network hiccups.
- ❌ If a worker is zombie (process alive but hung, not sending heartbeats), you may have both a recovery attempt and the original execution still running. Idempotency on the job execution is your last line of defense.

---

### 3g. Failure Scenarios

**Failure Mode 1: Scheduler leader crashes during dispatch**

The leader acquires a Redis lock for job J, starts writing to MySQL, then crashes before completing. The Redis lock expires after 60 seconds. The newly elected leader scans MySQL and finds job J in QUEUED status with a stale Redis lock. The new leader re-acquires the lock and dispatches normally. The MySQL version column ensures the first partially-written record is detected and overwritten atomically. No job is lost.

**Senior framing:** "This is why we keep MySQL as the source of truth and treat Redis as a performance layer, not a durability layer. On leader restart, we rebuild the Redis sorted set from MySQL in O(N) time — at 2M active jobs, this takes under 10 seconds. We accept this recovery window in exchange for never trusting Redis alone for job state."

---

**Failure Mode 2: MySQL primary failure during peak load**

The MySQL primary fails during midnight UTC peak. The semi-synchronous replica has all writes up to the last acknowledged transaction. Promotion takes 30-60 seconds. During this window, the scheduler pauses job submissions (write path) but continues serving status reads from the replica.

**Senior framing:** "We use ProxySQL in front of MySQL to abstract the primary election. When we promote a replica to primary, ProxySQL routing updates within seconds. In-flight writes that were not yet on the replica are replayed from the binary log on the application side using idempotency keys. The 30-60 second window of write unavailability is acceptable given that the alternative — losing job records — is not."

---

**Failure Mode 3: Redis SETNX lock lost mid-dispatch**

If the Redis primary fails between when the scheduler acquires a dispatch lock and when it writes the MySQL record, the new Redis primary has no knowledge of the lock. A standby scheduler that was waiting to acquire the same lock will now succeed on the new primary. Both instances believe they own the dispatch.

**Senior framing:** "This is the classic distributed lock safety problem — Redis single-node locks are not safe under primary failure. For cron triggers where the cost of rare duplicate execution is acceptable, we rely on the MySQL optimistic lock as the tiebreaker. For truly critical once-and-only-once semantics, we would use etcd or a Raft-based distributed lock (Redlock is widely considered unsafe). The pragmatic answer at 100K triggers/min is: make execution idempotent, and use the MySQL CAS as the authoritative exactly-once check."

---

## STEP 4 — COMMON COMPONENTS

These seven components appear across all five problems. Understand each one deeply.

---

### Component 1: MySQL 8.0 as Source of Truth

**Why used:** Durable, ACID-compliant, supports the complex relational queries needed for job definitions, execution history, DAG edges, tenant quotas, and audit trails. All five systems need to survive scheduler restarts with zero job loss, which requires a write-ahead log (InnoDB WAL) and synchronous replication.

**Key configuration:**
- Semi-synchronous replication: at least one replica must acknowledge writes before MySQL returns success. This ensures no committed job record is lost on primary failure.
- RANGE partitioning on `created_at`: execution history grows at 50 GB/day. Monthly partitions let you drop an entire old partition in milliseconds rather than running a slow DELETE over billions of rows.
- Optimistic locking via `version INT`: on dispatch, `UPDATE ... WHERE version = ?` atomically advances the state. Zero rows updated means another instance already took this job. This is cheaper than a pessimistic lock and survives crash recovery.
- ProxySQL in front: connection pooling (handles the burst of 10K worker connections) and transparent primary failover.

**What breaks without it:** If you lose MySQL and only have Redis, you lose the authoritative job state on Redis failure. Duplicate jobs, phantom jobs, and lost jobs all become possible. Redis is the fast path; MySQL is the safety net.

---

### Component 2: Redis Sorted Sets for Priority/Time Queuing

**Why used:** O(log N) insert (ZADD) and O(K + log N) range query (ZRANGEBYSCORE). At 2M active cron jobs, this means each 1-second scan takes ~21 comparisons to find the range boundary, then fetches K due jobs. Compare this to polling all 2M records from MySQL every second, which is obviously infeasible.

**Key configuration:**
- Score encoding: for time-based queues, score = `next_execution_time_ms`. For priority queues, score = `(1000 - effective_priority) × 10^15 + queued_at_epoch_ms`. The multiplier separates priority bands so higher-priority jobs always sort before any lower-priority job, regardless of submission time. The queued_at component provides FIFO tie-breaking within the same priority.
- BATCH_SIZE on ZRANGEBYSCORE: always use LIMIT (e.g., 1000) to prevent a single scan from returning 500K jobs and overwhelming the trigger loop.
- ZPOPMIN for exclusive dequeue: atomically remove and return the minimum-score element. Useful when multiple workers compete to consume from the same queue.

**What breaks without it:** Without Redis, you would poll MySQL's `job_execution` table with `SELECT ... WHERE status = 'QUEUED' ORDER BY priority DESC, scheduled_at ASC LIMIT 1000`. At high write throughput, this creates index contention and table-scan pressure on a write-heavy table. Latency degrades from milliseconds to seconds.

---

### Component 3: Distributed Lock (Redis SETNX with TTL)

**Why used:** Multiple scheduler instances must share dispatch work, but each job must be dispatched exactly once. `SETNX lock:dispatch:{job_id}:{fire_time} {instance_id} EX 60` atomically sets the lock only if it does not exist. The TTL prevents the lock from persisting forever if the holding instance crashes.

**Key configuration:**
- Lock key includes `{fire_time}` (for cron) or `{execution_id}` (for dispatch): this scopes the lock to a specific trigger event, not the entire job. If a job fires at 02:00 and again at 03:00, these are separate locks.
- TTL tuning: TTL must be longer than the expected dispatch latency (typically 60s). If TTL is too short, the lock expires during a slow MySQL write and a second instance tries to dispatch the same job.
- Always check MySQL CAS after acquiring the Redis lock: the Redis lock is a fast-path optimization; MySQL is the source of truth.

**What breaks without it:** At 5 scheduler instances scanning simultaneously, the same job would be dispatched 5 times. Even with MySQL optimistic locking as a fallback, you would have 4 wasted dispatch attempts and 4 failed MySQL writes per job — serious overhead at 100K/min.

---

### Component 4: Retry with Exponential Backoff + Dead-Letter Queue

**Why used:** Transient failures (network blip, OOMKill, dependency temporarily unavailable) should be automatically retried. Permanent failures (code bug, invalid input) should not spin forever — they should be moved to a DLQ where an operator can inspect and resolve them.

**Key configuration:**
- `max_retries` (typically 3-5) with `retry_backoff_ms` (initial delay, doubled each attempt): attempt 1 retries after 1 second, attempt 2 after 2 seconds, attempt 3 after 4 seconds. Add ± 20% jitter to prevent retry storms when many jobs fail simultaneously.
- Dead-letter threshold: after `max_retries` exhausted, set status to DEAD_LETTERED, write a record to `dead_letter_job` table with last error and full traceback. Alert on-call engineers.
- Misfire policy (cron-specific): if the scheduler was down for 3 hours and missed 3 trigger windows, what happens? FIRE_NOW runs the latest missed trigger immediately. SKIP_TO_NEXT skips all missed triggers and schedules for the next future time. FIRE_ALL_MISSED runs all missed triggers in sequence (dangerous for hourly jobs with many missed windows). Each job declares its own policy.

**What breaks without it:** Without retry logic, a single OOMKill would permanently fail an important job. Without a DLQ, failed jobs with bugs spin indefinitely, consuming resources and cluttering logs. Without misfire policy, a 4-hour scheduler downtime followed by restart would either silently drop all missed triggers or flood the dispatch engine with backed-up work.

---

### Component 5: Tenant Isolation with Per-Tenant Quotas

**Why used:** On a shared platform, a misconfigured tenant (e.g., team accidentally creates 1M `*/1 * * * *` jobs) can starve all other tenants. Quotas prevent this without requiring platform-level intervention.

**Key configuration:**
- `max_cron_jobs` (cron), `max_concurrent_jobs` (distributed), `max_submissions_hour` (priority), `rate_limit` per task type (task queue): each system has slightly different quota axes.
- Quota enforcement happens at submission time (reject new job if quota exceeded) and at dispatch time (don't dispatch if tenant is at their concurrent limit). Both checks needed: submission-time rejects new definitions, dispatch-time blocks runaway burst from existing definitions.
- Tenant namespace isolation: all queries are scoped with `WHERE tenant_id = ?`. Jobs from tenant A are never visible or impactable by tenant B in the hot path.

**What breaks without it:** A single tenant can saturate the dispatch engine's throughput, delay all other tenants' jobs, and cause cascading SLA violations across unrelated teams. In a bare-metal IaaS context, this can translate to other teams' servers not being provisioned on time.

---

### Component 6: Worker Heartbeat and Failure Detection

**Why used:** Workers are processes on potentially unreliable hardware. A worker that crashes mid-job must be detected promptly so its jobs can be re-queued. A worker that never crashes but gets stuck (infinite loop, blocking I/O) must also be detected via timeout.

**Key configuration:**
- Heartbeat interval: 10 seconds. Detection timeout: 30 seconds (3 missed heartbeats). Two-stage: SUSPECT after first timeout, DEAD after second. This prevents false positives from brief network hiccups.
- On transition to DEAD: all DISPATCHED and RUNNING jobs on that worker get `status = RETRYING, attempt_number + 1`. If `attempt_number >= max_retries`, move to DEAD_LETTERED.
- Worker DRAINING state: graceful shutdown. Workers set themselves to DRAINING, finish current jobs, and stop accepting new work. Prevents in-progress job loss during planned maintenance.

**What breaks without it:** Jobs assigned to dead workers would remain in RUNNING state forever. Without timeout detection, these zombie jobs block downstream DAG tasks, occupy resource quota, and cause SLA violations with no alerting.

---

### Component 7: Leader Election via etcd

**Why used:** 4 of 5 systems run a singleton trigger loop (only one instance should scan and fire triggers at a time). etcd provides a distributed lease mechanism: only the instance holding the lease runs the loop. On leader failure, etcd detects the expired lease and other instances compete for the new lease.

**Key configuration:**
- etcd lease TTL: typically 15-30 seconds. After the leader stops sending keepalives, the lease expires in at most TTL seconds. Another instance then acquires the lease and starts the loop.
- Lease renewal: leader sends `lease.keepalive()` every TTL/3 seconds. If the leader's own network to etcd is disrupted, it voluntarily steps down rather than running without a valid lease.
- Standby instances continue serving read traffic (status queries, history) regardless of leader state. Only the trigger loop and dispatch engine require leader exclusivity.

**What breaks without it:** Without leader election, all 5 scheduler instances scan Redis simultaneously. Each sees the same due jobs. All 5 try to dispatch each job. The Redis SETNX lock limits actual duplicates to 1, but you have wasted lock-acquisition attempts at 5× normal rate, and any Redis lock gap (TTL expiry during slow MySQL write) means actual duplicate jobs.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Cron Service at Scale

**Unique things:**
- Three cron expression formats: Unix 5-field (`0 2 * * *`), Quartz 6/7-field with seconds (`0 0/15 * * * ?`), and AWS EventBridge rate expressions (`rate(5 minutes)`). Parsing all three correctly — including DST transitions in arbitrary IANA timezones — is itself a non-trivial engineering task.
- Calendar engine: jobs can reference a calendar definition that specifies excluded dates (holidays), excluded days of week (weekends), and business hours. A job scheduled for `0 9 * * *` with a US business hours calendar only fires Monday-Friday between 9 AM and 5 PM, skipping Christmas and July 4th.
- Misfire policy per job: when the scheduler was down and missed triggers, each job independently declares whether to FIRE_NOW (run once immediately), SKIP_TO_NEXT (pretend it never missed), or FIRE_ALL_MISSED (re-run every missed window up to `max_misfire_count`).
- The Cron Service does not execute jobs itself — it submits one-shot jobs to the Distributed Job Scheduler for each trigger. This clean separation means the cron layer is purely about "when to run" and the distributed scheduler layer handles "how to run."

**Different key decisions:**
- The trigger loop runs every 1 second (not 1 minute like classic Linux cron) because Quartz-format jobs can have second-level granularity.
- The distributed lock key includes both `cron_job_id` AND `scheduled_fire_ms`: `lock:cron:{id}:{fire_ms}`. Without the fire time in the key, a cron job that fires twice in quick succession (due to misfire recovery) would block the second trigger because the first trigger's lock is still held.

**Two-sentence differentiator:** The Cron Service is about converting time-based expressions (in multiple formats, across timezones, with calendar rules) into exactly-once job submissions on a precise second-level schedule. Its core challenge is not dispatch but trigger semantics: DST-safe next-fire-time calculation, thundering herd suppression at midnight UTC, and policy-driven recovery from missed triggers when the scheduler was down.

---

### Deadline-Aware Scheduler

**Unique things:**
- Hard vs. soft deadlines: hard deadlines trigger resource reservation at submission time and can cause preemption of lower-urgency running jobs. Soft deadlines generate alerts but do not preempt.
- Feasibility check at submission: before accepting a job, the system estimates whether it can complete by the deadline given current cluster load. If not feasible, it returns 409 with an explanation and a suggestion (e.g., "extend deadline by 45 minutes or increase resource allocation"). This is a contract with the submitter, not just a hint.
- Laxity-based urgency: `laxity_ms = deadline - now - estimated_remaining_runtime`. Jobs with the lowest laxity (least slack) are scheduled first. This is different from static priority — a job with a far-future deadline might have high base priority but low urgency right now.
- SLA Monitor runs every 60 seconds and scans all running/queued deadline jobs. If `work_remaining > time_remaining × 0.8`, the job is marked AT_RISK and either triggering preemption or escalation alerts more than 10 minutes before the deadline.
- P80 runtime estimation: the scheduler builds a historical database of actual runtimes per job type and resource configuration. It uses the 80th percentile (not median) for feasibility checks because optimistic estimates cause SLA violations.

**Different key decisions:**
- Dedicated reserved capacity pool for hard-deadline jobs: these workers are never used for batch work. This wastes some utilization but guarantees that a burst of batch work cannot block an emergency hard-deadline job.
- Resource reservations are tracked in a `resource_reservation` table and released on job completion. Reservations are time-bounded (reserved_from, reserved_until) so a cancelled job doesn't permanently hold capacity.

**Two-sentence differentiator:** The Deadline-Aware Scheduler adds a temporal contract at submission time — it tells the caller whether their deadline is achievable and refuses infeasible jobs rather than silently missing SLAs later. Its core engineering challenge is accurate feasibility prediction (P80 runtime estimation, queue wait modeling) and proactive AT_RISK detection that fires corrective action with enough lead time to actually help.

---

### Distributed Job Scheduler

**Unique things:**
- DAG support: jobs can declare dependencies via `dag_edge` rows with condition types (ON_SUCCESS, ON_FAILURE, ON_COMPLETE, ALWAYS). The DAG Resolver runs Kahn's BFS topological sort algorithm to determine execution order. Cycle detection happens at DAG submission time.
- Scheduling modes: one-shot (run once now), recurring (cron-driven, delegated to the cron service), event-triggered (external webhook or message fires the job), and dependency-triggered (upstream DAG task completion unblocks this task).
- This is the foundational system that the other four build on top of. The cron service submits one-shot jobs here. The priority scheduler adds priority queuing on top of this. The deadline scheduler adds deadline tracking as metadata extensions.
- Idempotency tokens: producers include an `X-Idempotency-Key` header. The scheduler deduplicates by checking for existing job definitions with the same key. This lets producers safely retry submission on network failures without creating duplicate jobs.

**Different key decisions:**
- Elasticsearch is used for job execution search and analytics. At 50M executions/day, operators need full-text search on error messages, filtering by status across many dimensions, and time-series analytics on job duration distributions. MySQL is too slow for these ad-hoc queries.
- DAG visualization: critical path length is computed at DAG submission time and returned in the API response. This helps operators understand the minimum serial execution time of a pipeline.

**Two-sentence differentiator:** The Distributed Job Scheduler is the general-purpose execution backbone — it handles arbitrary job types, multi-stage DAG pipelines, and any execution trigger, making it the foundation all other schedulers in this pattern build upon. Its defining challenge is DAG dependency management at 1M concurrent jobs: topological ordering with ON_FAILURE branch conditions, fan-out/fan-in synchronization, and safe cycle detection at submission time.

---

### Priority-Based Job Scheduler

**Unique things:**
- Three priority bands (CRITICAL 900-1000, NORMAL 500-899, BATCH 0-499) with Weighted Fair Queuing at 70/25/5% dispatch capacity allocation. This prevents starvation while still strongly favoring critical work.
- Anti-starvation aging: every 60 seconds, each queued job's effective priority increments by +1, capped at base_priority + 200. A BATCH job waiting 4 hours at priority 100 gradually reaches effective priority 300, entering competition with NORMAL traffic. No job waits indefinitely.
- Preemption rules: a preemptor must have priority at least 200 higher than the preempted job, the preempted job must be less than 80% complete (wasted work), and the hourly preemption budget must not be exceeded. These rules prevent over-aggressive preemption that would churn the cluster.
- Priority inversion prevention: if a high-priority job depends on a low-priority job (which it should not, but operators do this accidentally), the dependency's effective priority is automatically boosted to unblock the higher-priority job.
- Full priority audit trail: every priority change (submission, aging, escalation, admin override, inversion boost, quota downgrade) is logged to `priority_audit` with timestamp, reason, and actor. Required for compliance and debugging.

**Different key decisions:**
- Redis ZPOPMIN score encoding: `score = (1000 - effective_priority) × 10^15 + queued_at_epoch_ms`. The large multiplier ensures priority ordering is strict — any effective_priority difference of 1 is more significant than waiting 30 years in the queue (10^15 ms ≈ 31 years). The queued_at component breaks ties by FIFO within the same priority.
- Preemption is implemented via SIGTERM with a 30-second grace period (not SIGKILL). The preempted job re-queues with its original base priority — it does not get penalized for being preempted.

**Two-sentence differentiator:** The Priority-Based Scheduler solves the resource arbitration problem on a shared platform where different workloads have different urgency — it ensures emergency provisioning runs before routine health checks, but batch jobs still make forward progress via aging rather than starving forever. Its defining complexity is the interaction between WFQ band weights, aging increments, preemption thresholds, and priority inversion detection, all of which must compose correctly to produce predictable, fair behavior.

---

### Task Queue System

**Unique things:**
- RabbitMQ quorum queues (3-node Raft cluster) as the broker, rather than Redis sorted sets as the queue. This choice provides broker-level durability and ordering guarantees without requiring the application to poll a database.
- Workflow primitives: chains (task A → B → C sequentially), groups (tasks A, B, C in parallel), and chords (group + callback — when all parallel tasks complete, fire a callback task with their results). The chord completion is tracked via a Redis DECR counter: each completing task decrements the counter; when it hits zero, the callback is enqueued.
- TTL + Dead-Letter Exchange for delayed tasks: a task with a future `eta` is published to a delay exchange with `x-message-ttl` set to the delay in milliseconds. When the TTL expires, RabbitMQ's dead-letter routing sends it to the target execution queue. No separate timer service needed.
- Per-task-type rate limiting: each task type has a `rate_limit` field (e.g., `100/m`, `1000/h`). A token bucket per `(task_type, tenant_id)` key in Redis, implemented via a Lua EVALSHA script for atomicity, enforces upstream API quotas at the queue level. This prevents overwhelming downstream services even when workers are fast.
- Result backend: task return values are stored in Redis with a 24-hour TTL. Callers awaiting results poll or subscribe via pub/sub. For callers that need results beyond 24 hours, they are persisted to MySQL.

**Different key decisions:**
- Why RabbitMQ over Kafka: Kafka excels at high-throughput log streaming but lacks native priority queues, per-message TTL, and individual message acknowledgement. Task queues need all three. RabbitMQ's AMQP model (one consumer ACKs one message) maps directly to task semantics.
- UUID v7 for task IDs: v7 UUIDs are time-ordered (monotonically increasing), which gives good index locality in MySQL's B-tree index, unlike random v4 UUIDs that cause index fragmentation at 200M inserts/day.

**Two-sentence differentiator:** The Task Queue System is optimized for short-lived, high-throughput async tasks (median 5 seconds) with native workflow composition primitives — chains, groups, and chords — which eliminate the need for a separate orchestration layer for common patterns. Its defining differentiator is RabbitMQ quorum queues providing broker-level at-least-once durability with Raft consensus, combined with a TTL + DLX mechanism for delayed tasks that requires zero additional infrastructure.

---

## STEP 6 — Q&A BANK

### Tier 1: Surface Questions (2-4 sentences each)

**Q1: "How does your scheduler guarantee that a cron job only fires once even when multiple scheduler instances are running?"**

**KEY PHRASE: Two-layer distributed lock — Redis SETNX for fast rejection, MySQL optimistic lock for authoritative exactly-once.**

The Redis layer acquires `SETNX lock:cron:{job_id}:{fire_time_ms}` with a 60-second TTL. Only one instance wins this atomic compare-and-set. That winner then executes `UPDATE job_execution SET status='DISPATCHED' WHERE status='QUEUED' AND version=?`; if 0 rows update, MySQL already has a newer version and the instance aborts. The two layers compose: Redis prevents the 99.99% case of duplicate dispatch efficiently; MySQL provides the authoritative guarantee even if Redis has a brief window of inconsistency.

---

**Q2: "Why use a Redis sorted set instead of polling MySQL for jobs to dispatch?"**

**KEY PHRASE: O(log N + K) versus O(N) — at 2M active jobs, the difference is 21 operations versus 2,000,000.**

`ZRANGEBYSCORE cron:next_exec 0 {now_ms} LIMIT 1000` is O(log 2M + K) where K is the number of due jobs (typically a few hundred to a few thousand per second). MySQL polling requires a full index scan or a range scan that still reads many irrelevant rows under write load. Redis also keeps the hot data entirely in memory, eliminating disk I/O. On failure, we rebuild Redis from MySQL in under 10 seconds, so durability is not sacrificed.

---

**Q3: "What happens if a worker picks up a job and then the worker's machine crashes? How do you avoid losing the job?"**

**KEY PHRASE: Heartbeat-based failure detection — SUSPECT after 30s silence, DEAD after 60s, automatic re-queue.**

Workers send heartbeats every 10 seconds. After 30 seconds of silence, the worker is marked SUSPECT. After 60 seconds total silence, it is marked DEAD, and all its DISPATCHED or RUNNING jobs are transitioned to RETRYING with attempt_number incremented. If retries are exhausted, the job moves to DEAD_LETTERED. The two-stage detection prevents transient network hiccups from causing unnecessary re-queues.

---

**Q4: "How do you handle a MySQL partition that's filling up quickly due to high execution volume?"**

**KEY PHRASE: RANGE partitioning on created_at — drop old partitions in milliseconds instead of running slow DELETEs.**

Each quarter's execution records live in a separate MySQL partition (`PARTITION p_2026_q2 VALUES LESS THAN (UNIX_TIMESTAMP('2026-07-01'))`). At 90-day retention, we have at most 4 active partitions. Dropping an expired partition is `ALTER TABLE job_execution DROP PARTITION p_2026_q1` — an O(1) metadata operation that takes milliseconds regardless of partition size. Without partitioning, `DELETE WHERE created_at < 90_days_ago` on a 4.5 TB table would lock the table for hours.

---

**Q5: "How do you prevent a single misbehaving tenant from starving all other tenants on a shared scheduler?"**

**KEY PHRASE: Per-tenant quotas enforced at both submission time and dispatch time, with rate limiting on the API.**

At submission time, we check `tenant_cron_quota.max_cron_jobs` and `max_triggers_per_min`. If the tenant would exceed either, the API returns 429 Too Many Requests. At dispatch time, we check `current_running < max_concurrent_jobs` before dispatching from the tenant's queue. Rate limiting on the submission API (1000 req/min per tenant) prevents burst from flooding the job table. These three layers ensure a single tenant cannot saturate the system even if their jobs all become due simultaneously.

---

**Q6: "What is the role of etcd in your architecture?"**

**KEY PHRASE: etcd provides leader election — exactly one scheduler instance runs the trigger loop at any time.**

etcd's lease mechanism gives one scheduler instance a time-bounded exclusive lease. That instance runs the cron trigger engine; all others stand by. When the leader crashes or becomes unreachable, etcd detects the expired lease and the standby instances compete for a new lease. The new leader inherits the shared state (MySQL + Redis) and continues without manual intervention. We use etcd rather than Redis-based leader election because etcd's Raft consensus provides stronger safety guarantees — a Redis primary failure can momentarily give two instances the lease.

---

**Q7: "Why use RabbitMQ for the task queue system instead of Redis directly?"**

**KEY PHRASE: RabbitMQ quorum queues give broker-level Raft consensus durability — Redis pub/sub has no persistence.**

RabbitMQ quorum queues replicate messages across 3 nodes using Raft. A message is only acknowledged to the producer after a majority of nodes have persisted it. This means even if one broker node fails, the message is not lost. Redis pub/sub has no persistence — if the subscriber is disconnected when a message is published, the message is gone. For infrastructure operations like bare-metal provisioning where losing a task means a server is never provisioned, broker-level durability is non-negotiable.

---

### Tier 2: Deep Dive Questions

**Q8: "Walk me through your approach to avoiding the thundering herd problem at midnight UTC when hundreds of thousands of daily cron jobs all fire simultaneously."**

**KEY PHRASE: Batch LIMIT on ZRANGEBYSCORE + parallel trigger processing + per-tenant rate limits + submission-time jitter.**

First, the trigger loop's `ZRANGEBYSCORE ... LIMIT 1000` bounds work per second — at 500K jobs due in one minute, we process 1000/second and spread the remaining 499K across the next 499 scan cycles. Second, the batch of 1000 is processed in parallel by a 16-thread pool, so lock acquisition and dispatch happen concurrently. Third, each tenant has a `max_triggers_per_min` quota so no single tenant can consume all 1000 slots. Fourth, at job creation time, we can add ± N seconds of jitter to the first `next_fire_time`, naturally spreading the thundering herd across a window — trade-off is losing strict millisecond precision, which is acceptable for most daily jobs.

The trade-off: jitter reduces precision, which is not appropriate for jobs with second-level granularity (Quartz format, cert rotation that must fire at exactly 02:00:00). For those jobs, we skip jitter and rely on the batch limit + parallelism to absorb the load within the allowed tolerance window.

---

**Q9: "Explain laxity-based scheduling and why it's better than static priority for deadline-aware workloads."**

**KEY PHRASE: Laxity = deadline - now - estimated_remaining_runtime. It's a continuous urgency metric that updates in real time.**

Static priority is fixed at submission. A job submitted with priority 800 keeps that priority forever, even if it has only 5 minutes left before a hard deadline. Another job submitted with priority 900 might have 48 hours before its deadline — it should yield the machine to the more urgently-constrained job.

Laxity solves this by computing urgency dynamically: `laxity_ms = deadline - now - estimated_remaining_runtime`. Jobs with negative or near-zero laxity are about to miss their deadline. They get highest urgency regardless of their base priority. Jobs with large positive laxity have lots of slack and can wait. We score the EDF queue by laxity, recalculate it in the SLA Monitor every 60 seconds, and also recalculate immediately when a job transitions from QUEUED to RUNNING (changing estimated remaining runtime).

The complexity is in runtime estimation. We use P80 of historical runtimes for the same job type, not median, because underestimating runtime leads to AT_RISK detection failures and actual SLA violations. The confidence score (0.5-0.99) from historical variance tells us how reliable the estimate is.

---

**Q10: "How does your preemption mechanism work, and what safeguards prevent excessive preemption?"**

**KEY PHRASE: Preempt only if Δpriority ≥ 200, job is < 80% complete, and hourly preemption budget is not exceeded.**

When a high-priority job cannot find a free worker, the Preemption Evaluator scans running jobs for candidates. It selects the running job with the lowest effective priority on a worker that has sufficient resources for the new job. It only preempts if three conditions are met: the priority difference is at least 200 (prevents minor priority differences from causing churn), the preempted job is less than 80% complete (avoids wasting nearly-done work), and the hourly preemption budget is not exceeded (prevents cascading preemptions under heavy load).

The preemption is delivered as SIGTERM with a 30-second grace period, not SIGKILL. This gives the preempted job time to checkpoint state if it supports that. The preempted job is re-queued with its original base priority — it does not get penalized. A preemption record is written to the audit table. If the preempted job's operator objects to being preempted, they can set `preemptible = false` at job submission time.

---

**Q11: "How do you handle the case where a job has been running for a long time and you suspect the worker is stuck (not crashed, just hung)?"**

**KEY PHRASE: Per-job timeout enforced by the scheduler, separate from worker heartbeat — distinction between dead worker and hung job.**

Worker heartbeat and job timeout are separate mechanisms. A worker can be perfectly healthy (sending heartbeats every 10 seconds) while running a job that has been stuck for hours. Each job definition has a `timeout_seconds` field. The Status Collector runs a periodic sweep: for each RUNNING job, if `now - started_at > timeout_seconds`, it sends a timeout signal to the worker to kill the job. The worker reports the kill, and the job transitions to TIMED_OUT status, then retries if under `max_retries`.

The differentiation matters: a dead worker means all its jobs need recovery. A timed-out job on a live worker means only that specific job needs to be killed and retried — the worker is fine and can pick up new work immediately. Conflating the two would either miss stuck jobs (if only checking heartbeat) or cause unnecessary re-queuing of all jobs when a single job times out.

---

**Q12: "When multiple instances of your scheduler are running, how do you prevent them from conflicting on DAG resolution?"**

**KEY PHRASE: DAG resolution runs on the leader only; downstream task unblocking is idempotent via status CAS.**

The leader election via etcd ensures only one instance runs the Schedule Evaluator and DAG Resolver. When an upstream task completes, the Status Collector (which also runs on the leader) invokes the DAG Resolver to check if all predecessors of each downstream task are now satisfied. The Resolver performs `UPDATE job_execution SET status='QUEUED' WHERE job_id = {downstream_id} AND status = 'PENDING' AND (all predecessor conditions met)`. This is an idempotent CAS operation — if two concurrent DAG resolution attempts race (e.g., during leader transition), only one UPDATE succeeds. The second sees 0 rows updated and exits silently.

The trade-off is that during a leader election (typically 5-30 seconds), DAG unblocking is paused. Tasks already in QUEUED or RUNNING state continue executing normally. Only the progression of newly-completing tasks is briefly delayed, which is acceptable for infrastructure workflows that are measured in minutes to hours.

---

**Q13: "How does the chord completion mechanism work and what failure modes does it have?"**

**KEY PHRASE: Redis DECR counter — last task to decrement atomically enqueues the callback. Failure: group task failure and counter drift.**

When a chord is submitted, a Redis counter `chord:{chord_id}:pending` is set to N (the group size). Each task in the group, upon successful completion, calls `DECR chord:{chord_id}:pending`. When the return value of DECR is 0, that task's worker enqueues the callback task with the collected results.

Failure modes: (1) If a group task fails and the chord requires all tasks to succeed, the DECR still happens but the callback should not be triggered. Handle this with a separate `chord:{chord_id}:failed` counter: if failed > 0 when pending reaches 0, enqueue a failure callback instead. (2) Counter drift: if a worker crashes after completing the task but before calling DECR, the counter never reaches 0 and the callback never fires. Mitigate by having the Heartbeat Collector call DECR when it marks a crashed task as RETRYING. (3) Redis crash: if Redis loses the counter, you lose track of chord progress. Persist the `task_group.completed_tasks` count to MySQL as a fallback, reconciled on Redis recovery.

---

### Tier 3: Staff+ Stress Tests

**Q14: "Your cron service experiences a 10-minute outage due to a datacenter network partition. When it comes back online, 2 million active cron jobs have accumulated misfires. How do you handle the recovery without causing a thundering herd that overwhelms the job dispatcher?"**

**KEY PHRASE: Misfire policy per job + bounded recovery batch size + back-pressure from downstream dispatcher.**

First, not all 2M jobs fire at every minute — many have hourly, daily, or weekly intervals and have only missed 1 trigger. The recovery scope is narrower than it appears: at peak, only ~100K jobs were due per minute over a 10-minute window = at most 1M missed triggers across all jobs.

Second, the misfire_policy per job governs what recovery looks like: FIRE_NOW triggers run once immediately for the most recent missed window. SKIP_TO_NEXT does nothing for the past and schedules for the next future time. FIRE_ALL_MISSED triggers all missed windows (dangerous for `*/5 * * * *` jobs that missed 120 windows).

Third, the Missed Trigger Detector runs in batches with the same BATCH_SIZE limit as the normal trigger loop. Rather than flooding the dispatcher with 1M jobs simultaneously, it processes at the normal rate (1000/second) and works through the backlog over ~17 minutes. The job dispatcher's acceptance of incoming work naturally throttles recovery because workers have finite capacity.

The deeper answer: if the downstream job dispatcher is also recovering from the same partition, we need back-pressure. The cron service should check the dispatcher's queue depth before submitting misfire recoveries. If the dispatcher's queue is already at capacity (e.g., > 80% of max concurrent jobs running), pause misfire recovery and retry in 60 seconds. This coordination between layers prevents the recovery from making the outage worse.

---

**Q15: "Walk me through exactly what happens — end to end, including all failure recovery paths — when a hard-deadline job misses its deadline despite your reservation system. What went wrong, and how do you prevent it next time?"**

**KEY PHRASE: Root cause categorization in sla_violation table + post-mortem feedback into runtime estimator and reservation buffer.**

For a hard-deadline job with a resource reservation to miss its deadline, one of several things must have happened: (1) The runtime estimate was too optimistic — the job took longer than P80 of historical runtimes. (2) The reserved worker failed during execution, and the replacement worker couldn't be provisioned fast enough. (3) The reservation was made but the reserved capacity was consumed by a higher-priority emergency job that was created after the reservation. (4) The job was admitted as FEASIBLE but the admission controller's queue wait estimate was wrong.

In the moment: the SLA Monitor detects the violation when `now > deadline` and `status != SUCCEEDED`. It writes a record to `sla_violation` with `violation_type = ACTUAL` and triggers a P1 alert with `escalation_level = PAGE`. The root cause is automatically categorized based on which timestamp sequence was anomalous (long queue wait → admission model error; long execution → runtime estimate error; no worker → reservation failure).

For prevention: each actual violation feeds back into the Runtime Estimator's training data. If the P80 estimate was 5 minutes and the job actually took 20 minutes, that outlier raises the P80 estimate for similar jobs in future. We also increase the reservation buffer for this job type: instead of reserving from P50 queue wait estimate, reserve from P80. The reservation pool's minimum size is reviewed to ensure it can absorb the typical number of simultaneous hard-deadline jobs.

The honest answer: a 99.9% hard-deadline adherence target means we will miss 1 in 1000 hard-deadline jobs at scale. The design goal is not perfection but detecting violations early enough to mitigate, learning from each one to prevent recurrence, and having a clear escalation path when mitigation fails.

---

**Q16: "Your priority-based scheduler is running fine, but you discover that BATCH jobs submitted 6+ hours ago are still not running. Anti-starvation aging should have handled this. What are the failure scenarios, and how do you diagnose them?"**

**KEY PHRASE: Aging loop failure, priority band saturation, quota lockout, and aging state drift between Redis and MySQL.**

There are at least four independent failure modes to check:

(1) **Aging loop failure:** The Anti-Starvation Engine runs every 60 seconds. If the leader scheduler instance crashed and the new leader did not restart the aging loop, jobs stop accumulating aging bonus. Check: `priority_audit` table — is there a recent AGING entry for any job? If the last AGING entry is 30+ minutes ago, the loop is dead. Fix: restart the aging thread and verify it logs every 60 seconds.

(2) **CRITICAL band saturation:** If CRITICAL jobs are consuming all 70% of dispatch capacity and there is a constant stream of new CRITICAL jobs, the 25% for NORMAL and 5% for BATCH bands may be genuinely exhausted — not by starvation, but by absolute resource scarcity. Even aged BATCH jobs at effective priority 300 compete in the NORMAL band, but if NORMAL band dispatch is also backed up, they still wait. Diagnosis: check `BATCH queue depth over time` and `total worker capacity versus total job queue depth`. If total jobs > total worker capacity for hours, you have a capacity problem, not a scheduler bug.

(3) **Tenant quota lockout:** The BATCH jobs might belong to a tenant that hit `max_concurrent_jobs` for the BATCH band. Even with high effective priority, quota enforcement prevents dispatch. Check: `tenant_priority_quota.current_running` versus `max_concurrent` for the affected tenant.

(4) **Redis-MySQL aging drift:** The aging loop increments `aging_applied` in Redis. If Redis was flushed or crashed and rebuilt from MySQL, `aging_applied` reset to 0 for all jobs. Those jobs dropped back to their base priority and their position in the Redis ZSET score reset. They are now at the back of the queue again. Diagnosis: compare `job_priority_config.aging_applied` in MySQL versus Redis hash values. Fix: on Redis rebuild, restore aging values from MySQL before rebuilding the sorted sets.

---

**Q17: "You need to design an upgrade path that takes your single-region job scheduler to multi-region active-active operation. What are the fundamental problems and how do you approach each?"**

**KEY PHRASE: You cannot have multi-region exactly-once execution without either a global distributed lock (high latency) or partition-by-ownership (complexity).**

Three fundamental problems:

(1) **Exactly-once dispatch across regions:** Your current Redis SETNX lock is region-local. A job dispatched by Region A's scheduler and a concurrent dispatch by Region B's scheduler are invisible to each other. You need either a cross-region distributed lock (Raft-based, adding 50-100ms RTT to every dispatch — unacceptable) or **partition-by-ownership** (each job is assigned to a home region at creation time; only that region's scheduler dispatches it). Partition-by-ownership is the standard approach: low-latency dispatch within region, no cross-region coordination on the hot path.

(2) **Failover without duplicate dispatch:** If Region A fails, Region B needs to take over Region A's jobs. But Region B cannot verify which jobs Region A already dispatched (Region A is down, Redis is region-local). You need a **standby-promote protocol**: Region B monitors Region A's health via a global heartbeat (e.g., a record in a globally-replicated database like CockroachDB or TiDB). When Region A is declared down, Region B claims ownership of Region A's jobs by atomically updating the home_region column: `UPDATE job_definition SET home_region = 'us-west' WHERE home_region = 'us-east' AND ...`. It then replays any jobs from us-east that were QUEUED or DISPATCHED but not SUCCEEDED.

(3) **Cross-region data model:** MySQL is not a global database. You need either a multi-region MySQL setup (expensive, complex replication topology) or a purpose-built globally-distributed store (CockroachDB, Spanner) for job definitions. Execution records can remain region-local. SLA reporting needs to aggregate across regions.

The honest answer for a Staff+ interview: multi-region active-active exactly-once scheduling is hard enough that most large schedulers use active-passive instead, with automatic failover rather than simultaneous active regions. Unless the RPO requirement is "zero jobs lost during a regional outage" and RTO is "under 30 seconds," active-passive is almost always the right trade-off.

---

## STEP 7 — MNEMONICS

### Mnemonic 1: QUEST — the five problem categories in job scheduling

**Q** — Queue (Task Queue System: broker-based, short-lived tasks, workflow primitives)
**U** — Urgency (Deadline-Aware Scheduler: laxity, feasibility, preemption for SLAs)
**E** — Engine (Distributed Job Scheduler: the foundational DAG execution backbone)
**S** — Schedule (Cron Service: time-based triggers, cron expressions, DST, calendars)
**T** — Tiers (Priority-Based Scheduler: bands, WFQ, aging, preemption by urgency)

When you sit down to answer "design a job scheduler," immediately ask yourself which QUEST problem they are describing. Each one has a different core component and a different key challenge.

---

### Mnemonic 2: DREAM — the five components all five systems share

**D** — Distributed lock (Redis SETNX, exactly-once)
**R** — Retry + Dead-letter (exponential backoff, DLQ)
**E** — etcd leader election (singleton trigger loop)
**A** — Auditable state in MySQL (partitioned, optimistic lock, zero-loss)
**M** — Multi-tenant quotas (submission-time + dispatch-time enforcement)

If you can explain DREAM fluently for any job scheduling problem, you will pass the "shared components" part of any interview in this domain.

---

### Opening one-liner

"A distributed job scheduler is a priority queue that spans time and machines — I'll start by clarifying whether you need time-based triggers, dependency-based ordering, deadline SLAs, or some combination, because those three dimensions drive completely different component choices."

This one-liner does three things: demonstrates systems thinking immediately, signals that you know there are multiple flavors of this problem, and invites the interviewer to guide the scope — which is exactly the right behavior in the first 60 seconds.

---

## STEP 8 — CRITIQUE

### Well-Covered Areas

The source files are excellent in several dimensions. The data models are production-grade: every table has an appropriate primary key strategy, the right indexes called out explicitly, optimistic locking via version columns, and RANGE partitioning explained with actual SQL. The algorithm code (Kahn's BFS, WFQ weighted dequeue, Redis sorted set scan, token bucket rate limiter) is concrete and correct. The capacity estimates use realistic numbers and derive architecture implications rather than just stating numbers. The API design includes both REST endpoints and CLI tools, which matters for infrastructure platforms. The common_patterns.md cross-referencing across all five systems is genuinely useful for an interview context.

### Missing or Shallow Areas

**Cross-cutting concern: observability and debuggability** — The files mention Elasticsearch for search and Prometheus for metrics, but do not detail the critical observability story: what metrics do you alert on? What does a P99 latency spike in dispatch latency look like in your dashboards? What query do you run to find out why job 12345 is stuck in QUEUED status for 20 minutes? Senior interviewers at FAANG expect you to describe the operational story, not just the happy-path architecture. Prepare: "Key metrics are: trigger_delay_p99, dispatch_to_start_latency_p99, dead_letter_queue_depth per tenant, worker_pool_utilization, and aging_jobs_at_risk_count. Alerts fire when trigger_delay_p99 > 5s or dlq_depth > 100."

**Scheduler scalability ceiling and sharding** — All five designs assume a single leader handles all dispatch decisions. At 1M concurrent jobs and 100K/min dispatch rate, what is the actual throughput ceiling of a single leader? The source files do not discuss horizontal scaling of the dispatch engine itself (e.g., sharding jobs by tenant_id mod N and having N parallel dispatch instances). This is a legitimate Staff+ follow-up question: "What happens at 10x your current scale?" Prepare: "We would shard the job namespace across multiple leader pairs — each pair owns 1/N of tenants and runs its own trigger loop and dispatch engine independently. etcd coordinates ownership assignment, not dispatch."

**Clock synchronization and its actual impact** — DST handling and timezone correctness in the cron service are mentioned but the deeper issue is NTP synchronization. If two scheduler instances have clocks that differ by 500ms, the one with the earlier clock will try to acquire the lock 500ms before the other — not a problem if SETNX atomicity works correctly, but it means trigger accuracy is bounded by NTP precision. Prepare a one-sentence acknowledgement: "We depend on NTP to keep scheduler clocks within ~100ms. Larger skews mean trigger accuracy degrades, but never causes duplicates because the lock key includes the scheduled fire time."

**Result storage and large output artifacts** — The source files mention `output_ref` pointing to S3/object store but do not discuss the design of large output artifact handling. A job that produces 50 GB of output cannot store that in MySQL. Prepare: "Workers stream output to object storage (S3/MinIO) and record only the reference URL in job_execution.output_ref. For result_ref in the task queue, results > 1 KB are stored in object storage and Redis only holds the reference, not the data itself."

### Senior Probes to Expect

- "You said Redis SETNX prevents duplicates. What if the Redis primary fails at exactly the moment the lock is acquired? Walk me through the exact failure scenario and what state the system ends up in."
- "Your laxity calculation depends on runtime estimation. What happens for a completely new job type with zero historical data?"
- "How does your WFQ dispatch engine handle the case where the CRITICAL band has zero jobs but there are 100K BATCH jobs? Does the 5% allocation for BATCH increase?"
- "Your heartbeat detector marks workers DEAD after 60 seconds. What if a worker has a 2-minute GC pause? You'd kill all its jobs unnecessarily. How do you tune this?"
- "You use optimistic locking on job_execution with a version column. At 100K dispatches per second, how many optimistic lock conflicts do you expect, and what's the retry strategy?"

### Traps to Avoid

**Trap 1: Claiming exactly-once without explaining the mechanism.** Say "Redis SETNX for distributed lock plus MySQL CAS for authoritative exactly-once." Do not just say "we use distributed locks." Interviewers will push on this and you need to know the mechanism.

**Trap 2: Using Kafka as the job queue without acknowledging its limitations.** Kafka is high-throughput but lacks per-message TTL, native priority queues, and individual message acknowledgement. Mentioning Kafka without these caveats signals pattern-matching rather than understanding.

**Trap 3: Describing a single-instance scheduler.** This kills 99.99% availability. Always start with "N instances with leader election, exactly one leader runs the trigger loop."

**Trap 4: Ignoring misfire handling for cron.** If the scheduler is down for 3 hours, what happens when it comes back? "It catches up" is not an answer. You need FIRE_NOW, SKIP_TO_NEXT, and FIRE_ALL_MISSED as distinct policies with trade-offs.

**Trap 5: Confusing task queue with job scheduler.** A task queue is for short-lived (<1 hour) function calls with at-least-once semantics. A job scheduler is for long-running compute with full lifecycle management, DAG dependencies, and exactly-once. Celery/Sidekiq are task queues. Quartz/Airflow are job schedulers. Know the difference and state it early if the problem is ambiguous.

**Trap 6: Not mentioning partition purging strategy.** Execution history at 50 GB/day fills disks fast. "We'll archive to S3" is incomplete. The correct answer is MySQL RANGE partitioning: drop old partitions in milliseconds. Missing this at Staff+ level is a red flag.

---
