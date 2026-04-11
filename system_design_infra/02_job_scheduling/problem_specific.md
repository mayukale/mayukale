# Problem-Specific Design — Job Scheduling (02_job_scheduling)

## Cron Service at Scale

### Unique Functional Requirements
- 3 cron expression formats: Unix 5-field, Quartz 6/7-field, AWS EventBridge rate expressions
- Timezone-aware scheduling with DST transition handling
- Exactly-once trigger across 5 scheduler instances; 10 M registered / 2 M active jobs
- Misfire handling: FIRE_NOW / SKIP_TO_NEXT / FIRE_ALL_MISSED (configurable per job)
- Calendar-aware scheduling: skip holidays, business hours, custom calendars
- 500 K "thundering minute" triggers at midnight UTC; 100 K triggers/min at peak

### Unique Components / Services
- **Cron Trigger Engine**: leader-only loop; scans `ZRANGEBYSCORE cron:next_exec 0 {nowMs} LIMIT BATCH_SIZE` every 1 s; acquires per-job distributed lock before dispatch; `O(log N + K)` per scan cycle
- **Distributed Lock per Trigger**: `SETNX lock:cron:{cron_job_id}:{scheduled_fire_ms} {instance_id} EX 60` — prevents duplicate triggers when multiple scheduler pods scan simultaneously
- **Calendar Engine**: evaluates calendar_rule rows (EXCLUDE_DATE, EXCLUDE_DOW, INCLUDE_HOURS) to determine if execution should proceed; configurable timezone-aware rules
- **Missed Trigger Detector**: periodic sweep every 5 min; detects misfires (`last_fire_time < now - expected_interval`); applies `misfire_policy` per job

### Unique Data Model
- **cron_job**: cron_job_id, tenant_id, cron_expression VARCHAR(255), expression_format ENUM(UNIX_5/QUARTZ_6/QUARTZ_7/EVENTBRIDGE_RATE), timezone VARCHAR(64), calendar_id FK, command TEXT, container_image, resource_request JSON, priority SMALLINT, concurrency_policy ENUM(ALLOW/SKIP/QUEUE), misfire_policy ENUM(FIRE_NOW/SKIP_TO_NEXT/FIRE_ALL_MISSED), max_misfire_count INT, next_fire_time TIMESTAMP(3), last_fire_time, last_fire_status, fire_count; `INDEX idx_enabled_next (enabled, paused, next_fire_time)`
- **trigger_history** (600 GB, 30-day retention, partitioned monthly): trigger_id, cron_job_id, scheduled_fire_time TIMESTAMP(3), actual_fire_time, fire_delay_ms BIGINT, trigger_type ENUM(SCHEDULED/MANUAL/MISFIRE_RECOVERY), execution_id, skipped, skip_reason TEXT, fired_by_instance
- **calendar** + **calendar_rule**: calendar_id, timezone, rule_type ENUM(EXCLUDE_DATE/EXCLUDE_DOW/INCLUDE_HOURS), recurring_yearly BOOLEAN, hour_start/hour_end SMALLINT
- **tenant_cron_quota**: max_cron_jobs INT, max_triggers_per_min INT, min_interval_seconds INT

### Algorithms

**Redis Sorted Set Trigger Scan:**
```java
Set<String> dueJobIds = redis.opsForZSet()
  .rangeByScore("cron:next_exec", 0, nowMs, 0, BATCH_SIZE);
// Score = next_execution_time_ms; O(log N + K) where K = due jobs
```
**Fire Delay Metric:** `fire_delay_ms = actual_fire_time_ms - scheduled_fire_time_ms` — recorded per trigger for SLA tracking

### Key Differentiator
Cron Service's uniqueness is its **3-format Redis ZSET trigger engine + per-job distributed lock for exactly-once semantics**: `ZRANGEBYSCORE cron:next_exec 0 {now} LIMIT BATCH_SIZE` scans only due jobs (not all 10 M) in O(log N + K); `SETNX lock:cron:{id}:{fire_ms}` per trigger prevents duplicate fires when multiple leader candidates scan simultaneously; calendar engine (EXCLUDE_DOW, INCLUDE_HOURS) enables business-hours-only jobs; misfire_policy per job handles DST transitions and scheduler downtime recovery.

---

## Deadline-Aware Scheduler

### Unique Functional Requirements
- Deadline specification: hard (must not miss) vs soft (best-effort); absolute timestamp
- Feasibility check at submission: determines FEASIBLE / AT_RISK / INFEASIBLE given current queue load
- SLA violation prediction > 10 min before deadline with 95% accuracy
- Deadline-based preemption: jobs approaching deadline preempt lower-urgency running jobs
- Resource reservation for HARD-deadline jobs; 500 K active (50 K hard + 450 K soft)

### Unique Components / Services
- **Feasibility Checker**: `slack_ms = (deadline - estimated_completion)`; if slack_ms > 0 → FEASIBLE; else if can_preempt → FEASIBLE with note; else INFEASIBLE; P80 runtime estimate used
- **Runtime Estimator**: P50/P80/P95/P99 percentiles from `runtime_estimation` history; `confidence = max(0.5, min(0.99, 1.0 - cv))` where cv = stdev/mean; cold start uses user-provided estimates
- **Resource Reservation Manager**: holds reserved capacity for HARD-deadline jobs; tracks in `resource_reservation` table; released on job completion/cancellation
- **Urgency Calculator**: `laxity_ms = deadline - now - estimated_remaining_runtime`; jobs with lowest laxity scheduled first; ZSET scored by laxity_ms
- **SLA Monitor & Predictor**: scans every 60 s; marks AT_RISK if `work_remaining > time_remaining × 0.8`; triggers preemption or alerts 10+ min before deadline

### Unique Data Model
- **job_deadline** (150 MB): job_id, deadline TIMESTAMP, deadline_type ENUM(HARD/SOFT), estimated_runtime_ms BIGINT, runtime_confidence DECIMAL(3,2), laxity_ms BIGINT, urgency_score DECIMAL(10,6), feasibility_status ENUM(FEASIBLE/AT_RISK/INFEASIBLE/VIOLATED/MET), reservation_id FK, submitted_at/accepted_at/started_at/estimated_completion/actual_completion, deadline_met BOOLEAN, slack_ms BIGINT; `INDEX idx_deadline`, `INDEX idx_type_status`, `INDEX idx_laxity`
- **resource_reservation** (25 MB/day): reservation_id, job_id, tenant_id, reserved_cpu_millis/memory_mb/gpu, pool_name, reserved_from/until TIMESTAMP(3), status ENUM(PENDING/ACTIVE/CONSUMED/RELEASED/EXPIRED); `INDEX idx_pool_time`, `INDEX idx_status`
- **sla_violation** (2.5 MB/day, 90-day): violation_id, job_id, deadline, deadline_type, violation_type ENUM(PREDICTED/ACTUAL), predicted_at TIMESTAMP, time_overdue_ms BIGINT, root_cause TEXT, escalation_level ENUM(NONE/WARN/PAGE/INCIDENT); `INDEX idx_unresolved`
- **runtime_estimation** (2 GB, 90-day, partitioned): job_type_key, actual_runtime_ms, resource_config JSON, input_size_bytes; `INDEX idx_type_time`

### Algorithms

**Laxity-Based Scheduling:**
```
laxity_ms = deadline - now - estimated_remaining_runtime
AT_RISK: work_remaining > time_remaining × 0.8
```

**P80 Runtime Estimation:**
```python
runtimes = sorted([h.actual_runtime_ms for h in history])
p80 = runtimes[int(len(runtimes) * 0.80)]
confidence = max(0.5, min(0.99, 1.0 - (stdev / mean)))
```

### Key Differentiator
Deadline-Aware Scheduler's uniqueness is its **laxity-based ZSET scheduling + feasibility check + proactive AT_RISK detection**: `laxity_ms = deadline - now - estimated_runtime` gives a continuous urgency metric (unlike binary priority bands); P80 runtime estimation with confidence score (0.5–0.99) handles both well-characterized and novel job types; `work_remaining > time_remaining × 0.8` fires AT_RISK alert 10+ min before violation with 95% accuracy; resource reservation pool prevents HARD-deadline jobs from being blocked by queued work.

---

## Distributed Job Scheduler

### Unique Functional Requirements
- DAG support: jobs declare dependencies; topological execution order via Kahn's algorithm
- Scheduling modes: one-shot, recurring (cron), event-triggered, dependency-triggered
- Full job lifecycle: create → queue → dispatch → run → succeed/fail → retry → cancel
- 1 M concurrent jobs; 100 K submissions/min; 10 K completions/sec
- At-least-once delivery; exactly-once via idempotency tokens

### Unique Components / Services
- **DAG Resolver**: Kahn's algorithm (BFS topological sort, O(V+E)); evaluates dependency conditions ENUM(ON_SUCCESS/ON_FAILURE/ON_COMPLETE/ALWAYS); unblocks downstream tasks when upstream completes
- **Schedule Evaluator**: scans due cron/time-based jobs, marks QUEUED; delegates to priority-based dispatch
- **Dispatch Engine**: matches QUEUED jobs to available workers considering resource requirements (cpu_millis, memory_mb, gpu); gRPC FetchJob RPC
- **Heartbeat Collector**: receives worker heartbeats every 30 s; > 30 s silence → SUSPECT; on confirmation → DEAD → jobs re-queued with retry_count + 1

### Unique Data Model
- **job_definition** (20 GB): job_id UUID, tenant_id, job_type ENUM(ONE_SHOT/RECURRING/EVENT_TRIGGERED/DAG_TASK), cron_expression, timezone, command TEXT, container_image, resource_request JSON, max_retries TINYINT, retry_backoff_ms INT, timeout_seconds INT, priority SMALLINT 0–1000, tags JSON, idempotency_key VARCHAR(255), version INT; `INDEX idx_type_enabled`, `INDEX idx_priority`
- **job_execution** (50 GB/day, 4.5 TB 90-day, partitioned quarterly): execution_id UUID, job_id, dag_run_id, attempt_number TINYINT, status ENUM(PENDING/QUEUED/DISPATCHED/RUNNING/SUCCEEDED/FAILED/RETRYING/CANCELLED/TIMED_OUT/DEAD_LETTERED), worker_id, scheduled_at/queued_at/dispatched_at/started_at/completed_at TIMESTAMP(3), exit_code SMALLINT, output_ref VARCHAR(512), idempotency_token VARCHAR(255), lock_version INT; `INDEX idx_status_scheduled`, `INDEX idx_dag_run`
- **dag_edge**: upstream_job_id UUID, downstream_job_id UUID, condition ENUM(ON_SUCCESS/ON_FAILURE/ON_COMPLETE/ALWAYS)
- **worker_node** (5 MB): worker_id, pool_name, total/available cpu_millis/memory_mb/gpu, status ENUM(ACTIVE/DRAINING/SUSPECT/DEAD), last_heartbeat TIMESTAMP(3), labels JSON; `INDEX idx_pool_status`

### Algorithms

**Kahn's DAG Cycle Detection + Topological Sort:**
```python
in_deg = dict(self.in_degree)
queue = deque([t for t in self.all_tasks if in_deg.get(t, 0) == 0])
while queue:
    task = queue.popleft(); sorted_tasks.append(task)
    for downstream in self.edges[task]:
        in_deg[downstream] -= 1
        if in_deg[downstream] == 0: queue.append(downstream)
if len(sorted_tasks) < len(self.all_tasks): raise CyclicDependencyError
```

### Key Differentiator
Distributed Job Scheduler's uniqueness is its **DAG with ON_SUCCESS/ON_FAILURE dependency conditions + idempotency token exactly-once**: Kahn's BFS topological sort (O(V+E)) resolves multi-step pipelines at 1 M concurrent jobs; dependency conditions (ON_SUCCESS/ON_FAILURE/ON_COMPLETE) enable branch-on-outcome workflows (e.g., run cleanup only on failure); `idempotency_key` on job_definition + `idempotency_token` on job_execution prevents duplicate execution when producers retry submission; optimistic locking (lock_version) on job_execution prevents two dispatch workers from picking the same job.

---

## Priority-Based Job Scheduler

### Unique Functional Requirements
- Multi-level priority: 0 (lowest) to 1000 (highest); 3 bands: CRITICAL 900–1000, NORMAL 500–899, BATCH 0–499
- Weighted Fair Queuing: 70% dispatch capacity to CRITICAL, 25% NORMAL, 5% BATCH
- Anti-starvation aging: every 60 s, each queued job gains +1 effective_priority (max bonus = +200)
- Preemption: preempt if Δpriority ≥ 200 AND target job < 80% complete
- Priority inversion prevention: boost low-priority dependencies of high-priority jobs

### Unique Components / Services
- **Multi-Level Priority Queue**: 3 Redis ZSETs (one per band); `score = (1000 - effective_priority) × 10^15 + queued_at_epoch_ms` — tie-break by submission time; `ZPOPMIN` per band per cycle
- **Weighted Fair Dequeue (WFQ)**: `allocations = {CRITICAL: batch×0.70, NORMAL: batch×0.25, BATCH: batch×0.05}` rounded up with floor of 1; runs per dispatch cycle
- **Anti-Starvation Engine**: runs every 60 s; `effective_priority = base_priority + aging_applied`; `aging_applied += 1` capped at `max_aging_bonus = 200`; if effective_priority crosses band boundary, job moves to higher ZSET
- **Preemption Evaluator**: `IF (Δpriority ≥ 200) AND (progress < 80%) AND (hourly_preemptions < MAX_BUDGET): SIGTERM target`; 30 s grace period (grace_period_ms = 30000)
- **Priority Inversion Detector**: detects high-priority job blocked on low-priority dependency; boosts dependent's effective_priority to unblock

### Unique Data Model
- **job_priority_config**: job_id UUID, base_priority SMALLINT 0–1000, effective_priority SMALLINT, priority_band ENUM(CRITICAL/NORMAL/BATCH), preemptible BOOLEAN, preempt_others BOOLEAN, max_aging_bonus SMALLINT DEFAULT 200, deadline TIMESTAMP, escalation_rate SMALLINT DEFAULT 50 (priority/hour), submitted_at, queued_since, aging_applied SMALLINT; `INDEX idx_band_effective`
- **tenant_priority_quota**: tenant_id, priority_band ENUM, max_concurrent INT, max_submissions_hour INT, current_running INT, current_hour_count INT
- **preemption_record**: preemption_id, preemptor_job_id, preemptor_priority, preempted_job_id, preempted_priority, worker_id, resources_freed JSON, preempted_at TIMESTAMP(3), preempted_job_requeued BOOLEAN, grace_period_ms INT DEFAULT 30000
- **priority_audit**: audit_id, job_id, old_priority, new_priority, change_reason ENUM(AGING/ESCALATION/ADMIN_OVERRIDE/INVERSION_BOOST/QUOTA_DOWNGRADE/SUBMISSION), changed_by

### Algorithms

**Weighted Fair Dequeue:**
```python
allocations = {
    'CRITICAL': max(1, int(batch_size * 70 / 100)),
    'NORMAL':   max(1, int(batch_size * 25 / 100)),
    'BATCH':    max(1, int(batch_size * 5 / 100))
}
# score = (1000 - effective_priority) * 10^15 + queued_at_epoch_ms
```

**Preemption Decision:**
```
IF (preemptor_priority - preempted_priority >= 200)
   AND (preempted_job.progress < 80%)
   AND (preemptions_this_hour < MAX_BUDGET):
   SIGTERM preempted_job (grace_period_ms = 30000)
```

### Key Differentiator
Priority-Based Scheduler's uniqueness is its **3-band WFQ (70/25/5%) + aging anti-starvation + threshold-gated preemption**: WFQ prevents BATCH jobs from starving (always 5% capacity) without compromising CRITICAL throughput; aging (+1 effective priority every 60 s, max +200) guarantees batch jobs run within ~4 hours regardless of CRITICAL load; preemption threshold (Δ ≥ 200, < 80% complete) avoids wasted preemption of nearly-done jobs; priority_audit table provides full provenance of every effective_priority change (aging, escalation, inversion boost).

---

## Task Queue System

### Unique Functional Requirements
- Reliable at-least-once delivery via RabbitMQ quorum queues (3-node Raft consensus)
- Task workflow primitives: chains (sequential), groups (parallel), chords (parallel + callback)
- Per-task-type rate limiting: token bucket with refill_rate per tenant per endpoint
- 500 K enqueues/min, 200 K completions/min; 50 K concurrent workers
- Result backend: Redis TTL 24 h; < 50 ms P99 result retrieval

### Unique Components / Services
- **RabbitMQ Cluster (3 nodes, quorum queues)**: Raft-based consensus; quorum queues confirm after majority persist; separate exchanges per priority band (HIGH/DEFAULT/BULK); publisher confirms for at-least-once
- **Exchange Router**: topic exchange routes by task_type, priority, tenant to correct queue; direct exchange for result routing
- **Delay Queue (TTL + DLX)**: tasks with `eta` published with `x-message-ttl` header to delay exchange; on TTL expiry, dead-letter routed to target queue; worker picks up when eta ≤ now
- **Chord Completion Tracker**: Redis counter `chord:{chord_id}:pending = N`; each completing task `DECR`; when counter reaches 0 → enqueue callback task with collected results
- **Rate Limiter**: token bucket per `(task_type, tenant_id)` in Redis; Lua EVALSHA atomic: `tokens = min(max_tokens, tokens + (now - last_refill) × refill_rate); if tokens >= 1: tokens -= 1; allow`

### Unique Data Model
- **task_type** (5 K types): task_type_id, task_name (e.g., `infra.provision.bare_metal`), queue_name, priority ENUM(HIGH/DEFAULT/BULK), max_retries TINYINT, retry_backoff_ms INT, timeout_seconds INT, rate_limit VARCHAR(32) (e.g., `100/m`, `1000/h`), result_ttl_seconds INT DEFAULT 86400, serialization ENUM(JSON/PROTOBUF/MSGPACK); `INDEX idx_queue`
- **task_instance** (200 M/day, partitioned monthly): task_id UUID v7 (time-ordered), task_type_id, tenant_id, status ENUM(PENDING/QUEUED/ACTIVE/SUCCEEDED/FAILED/RETRYING/CANCELLED/DEAD_LETTERED), priority TINYINT 0–9, args_ref VARCHAR(512), args_hash CHAR(64) SHA-256, eta TIMESTAMP, countdown_seconds INT, attempt_number TINYINT, worker_id, parent_task_id (chains), group_id (groups), chord_callback_id, enqueued_at/started_at/completed_at TIMESTAMP(3), result_ref VARCHAR(512), runtime_ms INT; `INDEX idx_tenant_status`, `INDEX idx_eta`, `INDEX idx_group`
- **dead_letter_task** (200 K/day, 90-day): dlq_id, task_id, total_attempts TINYINT, last_error TEXT, last_traceback TEXT, original_args MEDIUMBLOB, dead_lettered_at TIMESTAMP(3), resolved BOOLEAN, resolution ENUM(RETRIED/DISCARDED/MANUALLY_FIXED)
- **rate_limit_bucket**: bucket_key VARCHAR(255) `{task_type}:{tenant_id}`, tokens DECIMAL(10,3), max_tokens INT, refill_rate DECIMAL(10,3) tokens/sec, last_refill TIMESTAMP(3)

### Key Differentiator
Task Queue System's uniqueness is its **RabbitMQ quorum queues + chord/group workflow primitives + TTL delay queue**: quorum queues (3-node Raft) provide durable at-least-once without ZooKeeper overhead; TTL + dead-letter exchange for delayed tasks is native RabbitMQ (no separate timer service); chord completion uses Redis DECR counter — last completing task atomically triggers callback without polling; per-task-type `rate_limit` field (e.g., `100/m`) enforces upstream API quotas at queue level, not just at producer level.
