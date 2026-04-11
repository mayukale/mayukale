# Common Patterns — Job Scheduling (02_job_scheduling)

## Common Components

### MySQL 8.0 as Source of Truth for Job Definitions and Execution History
- All 5 systems store job/task definitions and execution records in MySQL; time-partitioned for efficient purging
- cron_service: `cron_job`, `trigger_history` (partitioned monthly, 30-day retention), `calendar`, `tenant_cron_quota`
- deadline_aware_scheduler: `job_deadline`, `resource_reservation`, `sla_violation` (90-day retention), `runtime_estimation`
- distributed_job_scheduler: `job_definition`, `job_execution` (50 GB/day, partitioned quarterly, 4.5 TB 90-day), `dag_definition`, `dag_edge`, `worker_node`
- priority_based_job_scheduler: `job_priority_config`, `tenant_priority_quota`, `preemption_record`, `priority_audit`
- task_queue_system: `task_type`, `task_instance` (200 M/day, partitioned monthly), `dead_letter_task`, `worker_instance`

### Redis Sorted Sets for Priority/Deadline/Time Queuing
- All 5 use Redis sorted sets (ZADD/ZRANGEBYSCORE/ZPOPMIN) for O(log N) insert and O(K + log N) range query
- cron_service: `cron:next_exec` ZSET (score = next_execution_time_ms); `ZRANGEBYSCORE cron:next_exec 0 {nowMs}` every 1 s
- deadline_aware_scheduler: ZSET scored by laxity_ms (deadline - now - estimated_remaining_runtime)
- distributed_job_scheduler: priority queue ZSET scored by (priority DESC, scheduled_at ASC)
- priority_based_job_scheduler: 3 Redis ZSETs — CRITICAL (900–1000), NORMAL (500–899), BATCH (0–499); WFQ 70/25/5%
- task_queue_system: per-queue priority sorted sets (priority 0–9); ZPOPMIN for highest-priority task

### Distributed Lock for Exactly-Once Dispatch
- All 5 use Redis SETNX with TTL to prevent duplicate job triggering/dispatch across scheduler instances
- cron_service: `lock:cron:{cron_job_id}:{scheduled_fire_ms}` — acquired before dispatch; 60 s TTL
- distributed_job_scheduler: `lock:dispatch:{job_id}` — prevents two scheduler pods dispatching same job
- priority_based_job_scheduler: `lock:preempt:{job_id}` — prevents concurrent preemption decisions
- deadline_aware_scheduler: `lock:reserve:{job_id}` — serializes resource reservation
- task_queue_system: RabbitMQ quorum queues (Raft-based) provide at-least-once at broker level; consumer ack for exactly-once semantics

### Retry with Exponential Backoff + Dead-Letter Queue
- All 5 implement configurable retry with exponential backoff; permanently failed jobs routed to DLQ
- cron_service: `max_misfire_count`; misfire_policy ENUM(FIRE_NOW/SKIP_TO_NEXT/FIRE_ALL_MISSED)
- deadline_aware_scheduler: retry with urgency-aware scheduling; preemption as last resort before violation
- distributed_job_scheduler: `max_retries` + `retry_backoff_ms`; DEAD_LETTERED status in job_execution
- priority_based_job_scheduler: retry with same base_priority; dead_letter threshold configurable
- task_queue_system: `max_retries` + `retry_backoff_ms` per task_type; `dead_letter_task` table; 90-day retention

### Tenant Isolation with Per-Tenant Quotas
- All 5 enforce tenant-level resource isolation and throughput limits
- cron_service: `tenant_cron_quota (max_cron_jobs, max_triggers_per_min, min_interval_seconds)`
- distributed_job_scheduler: per-tenant `max_concurrent_jobs`, rate limiting on job submission API
- priority_based_job_scheduler: `tenant_priority_quota (priority_band, max_concurrent, max_submissions_hour)`
- task_queue_system: `rate_limit_bucket (bucket_key=task_type:tenant_id, tokens, max_tokens, refill_rate)`
- deadline_aware_scheduler: per-tenant resource reservation pool limits

### Worker Heartbeat and Failure Detection
- 4 of 5 use heartbeat-based worker liveness tracking; missed heartbeat → jobs re-queued
- distributed_job_scheduler: `worker_node.last_heartbeat`; > 30 s silence → SUSPECT → DEAD; jobs re-dispatched
- priority_based_job_scheduler: heartbeat timeout → re-queue with original priority
- task_queue_system: `worker_instance.last_heartbeat`; ACTIVE → DRAINING → OFFLINE; RabbitMQ consumer cancel on disconnect
- deadline_aware_scheduler: worker failures trigger urgency recalculation + preemption of replacement

## Common Databases

### MySQL 8.0
- All 5; job/task definitions, execution history, audit trails, dead-letter records; RANGE partitioning on created_at for efficient purging; optimistic locking (version/lock_version INT column)

### Redis
- All 5; sorted sets for priority/deadline/time queues; SETNX distributed locks; result backend (TTL 24 h); in-memory counters for rate limiting; chord/group tracking

### etcd
- 4 of 5 (cron, distributed_job, priority_based, deadline_aware); leader election for scheduler singleton; distributed key-value config

### Elasticsearch
- 4 of 5 (cron, distributed_job, priority_based, task_queue); job execution search, audit log search, dashboards with filter/facet queries

### RabbitMQ
- task_queue_system: 3-node cluster with quorum queues (Raft consensus); separate queues per priority band; TTL + dead-letter exchange for delayed tasks

## Common Communication Patterns

### gRPC for Worker ↔ Scheduler Data Plane
- 3 of 5 (distributed_job, priority_based, task_queue): FetchJob/Heartbeat/ReportCompletion RPC calls; streaming for heartbeat; low latency vs REST

### REST API + CLI for Management Plane
- All 5: `POST /api/v1/jobs`, `GET /api/v1/jobs/{id}`, `PATCH`, `DELETE`; CLI tools (cronctl, jobctl, taskq) for operator use

### Async Dispatch (Produce and Forget)
- All 5: producers submit job (INSERT + return job_id immediately); workers consume asynchronously; decouples submission latency from execution

## Common Scalability Techniques

### Time-Range Partitioning for Execution History
- All 5: MySQL RANGE partitioning on created_at (monthly or quarterly); old partitions archived/dropped; keeps hot set small; enables sub-second purge of 90-day-old records

### Batch Dequeue with LIMIT
- All 5: scheduler scans Redis sorted set with LIMIT (100–1000 jobs per cycle); avoids single-instance overload; distributes work across dispatch cycles

### Leader Election for Singleton Scheduler
- 4 of 5: etcd or Redis-based leader election ensures only one scheduler instance runs the trigger loop, preventing duplicate dispatches; followers stand by for failover

## Common Deep Dive Questions

### How do you guarantee exactly-once job execution when multiple scheduler pods are running?
Answer: Two-layer approach: (1) Redis SETNX `lock:job:{job_id}:{fire_time}` — only the instance that acquires this lock dispatches the job; TTL prevents permanent lock if the scheduler crashes; (2) Optimistic locking on the MySQL execution record — the dispatch UPDATE uses `WHERE status = 'QUEUED' AND version = ?`; if another instance already dispatched it, 0 rows update and the lock holder skips. Together, Redis SETNX provides fast rejection at the distributed layer; MySQL CAS provides the authoritative exactly-once guarantee.
Present in: cron_service, distributed_job_scheduler, priority_based_job_scheduler

### How do you handle thundering herd at midnight UTC when millions of cron jobs are due simultaneously?
Answer: Three mitigations: (1) Redis ZRANGEBYSCORE with BATCH_SIZE limit — process K jobs per 1 s scan cycle, not all N simultaneously; (2) Distributed lock per job — only one scheduler instance processes each job, so horizontally scaling scanners doesn't increase duplicate dispatches; (3) Jitter injection on job submission — spread next_fire_time by ± jitter_seconds to prevent synchronization; tenant_cron_quota.max_triggers_per_min enforces per-tenant rate limits to prevent any single tenant saturating the dispatch engine.
Present in: cron_service

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.99% for scheduler control plane across all 5 systems |
| **P99 Dispatch Latency** | < 200 ms (priority), < 500 ms (deadline, distributed), < 5 s (cron trigger → execution start), < 100 ms (task queue enqueue → worker pickup) |
| **Throughput** | 100 K triggers/min (cron), 10 K completions/s (distributed), 50 K decisions/min (priority), 500 K enqueues/min (task queue) |
| **Scale** | 10 M registered jobs (cron), 2 M active jobs (cron), 1 M concurrent jobs (distributed), 50 K concurrent workers (task queue) |
| **Retention** | 30–90 days execution history; time-partitioned for efficient purging |
| **Durability** | Zero job loss after acknowledgement across all 5 systems |
