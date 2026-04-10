# System Design: Distributed Job Scheduler

---

## 1. Requirement Clarifications

### Functional Requirements
- Schedule jobs to execute at a specific time (one-time) or on a recurring schedule (cron expression)
- Support cron syntax (e.g., `0 2 * * *` = 2 AM daily) and ISO 8601 interval notation
- Job definitions include: handler/worker type, input payload, retry policy, priority, timeout
- Exactly-once execution: a scheduled job fires exactly once per trigger time, even if multiple scheduler nodes are running
- Job deduplication: prevent duplicate registration of the same job (idempotent submit)
- Priority queues: higher-priority jobs preempt or jump ahead of lower-priority jobs in the execution queue
- Job dependency DAGs: job B can declare a dependency on job A; B executes only after A succeeds
- Failure handling: configurable retry with exponential backoff, max retry count, DLQ for failed jobs
- Status tracking: PENDING → QUEUED → RUNNING → COMPLETED | FAILED | RETRYING
- Job history: queryable execution history with output/error logs
- Pause / resume / cancel: admin operations on individual jobs or job groups
- Cron job management: create, update (reschedule), disable, delete cron job definitions

### Non-Functional Requirements
- Reliability: guaranteed fire — a scheduled job must fire within 5 seconds of its scheduled time (trigger precision = 5s)
- Exactly-once execution under concurrent scheduler nodes (no duplicate fires for the same trigger)
- Scalability: support 100,000 distinct cron jobs, 10,000 job executions/minute at peak
- High availability: scheduler service survives single node failures without missed job fires
- Fault tolerance: if a worker crashes mid-execution, the job is retried per policy
- Latency: job enqueued to worker start < 1 second after scheduled time
- Durability: job definitions and execution history survive scheduler restarts
- Observability: full audit trail; job execution duration, success/failure rates per job type

### Out of Scope
- Real-time stream processing (use Flink/Spark Streaming)
- Complex multi-step workflow orchestration beyond DAG (use Airflow/Temporal for complex workflows)
- Sub-second job scheduling (use dedicated rate-limiting or event-driven triggers)
- Dynamic code upload (jobs are pre-deployed workers; scheduler only coordinates)
- Billing / metering per job execution

---

## 2. Users & Scale

### User Types
| Actor | Behavior |
|---|---|
| Engineering team | Registers cron jobs (batch processing, report generation, cleanup) |
| Business stakeholder | Monitors job health dashboard; approves/pauses jobs |
| Application service | Submits one-time jobs programmatically (e.g., send email 24h after signup) |
| Worker process | Polls/receives job tasks; executes and reports status |
| Admin | Force-retries failed jobs; cancels runaway jobs; audits history |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 100,000 distinct cron job definitions
- Average job frequency: once per hour per job (conservative; many jobs are daily or weekly)
- Job execution duration: average 30 seconds, peak 5 minutes
- Peak-to-average ratio: 5× (many jobs scheduled at midnight/top-of-hour)
- Worker pool: 500 worker processes × 4 concurrent job slots = 2,000 concurrent workers

Average jobs/min = 100,000 / 60 ≈ 1,667 jobs/min  
Peak jobs/min = 1,667 × 5 = **8,335 jobs/min ≈ 139 jobs/sec**

Note: "midnight spike" — if 10% of 100K jobs are scheduled at midnight (00:00):  
10,000 jobs must be triggered within 5 seconds → 2,000 jobs/sec for ~5 seconds  
Worker throughput needed: 2,000 × 30 sec avg duration / 2,000 workers = sustainable, but queue depth spikes.  
Queue max depth at spike: 2,000 jobs queued for 2,000 workers → ~1 min to drain at normal pace.

### Latency Requirements
| Operation | P50 | P99 | P999 |
|---|---|---|---|
| Cron job trigger → queue | 100 ms | 2 s | 5 s |
| Job queued → worker start | 100 ms | 1 s | 5 s |
| Job submit API response | 50 ms | 200 ms | 500 ms |
| Job status query | 10 ms | 50 ms | 200 ms |
| Job cancel (graceful) | 100 ms | 2 s | 10 s |

### Storage Estimates

**Job Definition:**
- Each: ~2 KB (cron expression, handler, payload template, retry config, metadata)
- 100,000 jobs × 2 KB = **200 MB** (trivial)

**Execution History:**
- Each execution record: ~1 KB (job_id, run_id, start/end time, status, worker_id, output summary)
- 139 executions/sec × 86,400 sec/day × 1 KB = **12 GB/day**
- Retain 90 days: 12 × 90 = **1.08 TB** execution history
- Index on (job_id, scheduled_time, status): ~200 GB additional index storage

| Component | Size | Notes |
|---|---|---|
| Job definitions | 200 MB | 100K jobs × 2 KB |
| Execution history (90 days) | 1.08 TB | 12 GB/day × 90 days |
| Indexes | 200 GB | Covering indexes on job_id, status, time |
| Trigger log (7 days) | 84 GB | Dedup/audit |
| Worker heartbeat state | < 1 MB | 2000 workers × 500 B |

### Bandwidth Estimates

Job dispatch: 139 jobs/sec × 5 KB avg payload = **695 KB/sec** outbound to workers  
Status updates: 139 jobs/sec × 2 KB update = **278 KB/sec** inbound from workers  
Total: < 1 MB/sec — negligible; the system is compute-bound, not bandwidth-bound.

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                   API / Control Plane                             │
│                                                                   │
│  POST /jobs         - Register one-time job                      │
│  POST /cron-jobs    - Register recurring job                     │
│  GET  /jobs/:id     - Get job status                            │
│  POST /jobs/:id/cancel                                           │
│  PUT  /cron-jobs/:id - Update schedule                           │
│  GET  /jobs?status=FAILED&after=... - Query history              │
└──────────────────────┬───────────────────────────────────────────┘
                       │
         ┌─────────────▼──────────────┐
         │     Scheduler Database      │
         │  (PostgreSQL with timescale │
         │   or partitioned tables)    │
         │                            │
         │  Tables:                   │
         │  - job_definitions         │
         │  - scheduled_triggers      │
         │  - job_executions          │
         │  - job_dependencies        │
         └─────────────┬──────────────┘
                       │
         ┌─────────────▼──────────────────────────────────┐
         │      Trigger Engine (Leader-elected)            │
         │                                                  │
         │  Scheduler Node 1 (LEADER)  ←── etcd leader    │
         │  Scheduler Node 2 (STANDBY)     election        │
         │  Scheduler Node 3 (STANDBY)                     │
         │                                                  │
         │  Leader loop:                                    │
         │  1. SELECT due triggers (now() - 5s < time ≤ now()) │
         │  2. Atomic claim (UPDATE ... SET claimed_by=me  │
         │     WHERE claimed_by IS NULL)                    │
         │  3. Enqueue to Job Queue                         │
         │  4. Mark trigger as FIRED                        │
         └─────────────┬──────────────────────────────────┘
                       │
         ┌─────────────▼──────────────┐
         │         Job Queue           │
         │   (Multi-priority Redis     │
         │    Sorted Set or Kafka)     │
         │                            │
         │  Priority levels: 0-9      │
         │  FIFO within same priority │
         └─────────────┬──────────────┘
                       │
         ┌─────────────▼──────────────────────────────────┐
         │              Worker Pool                         │
         │                                                  │
         │  ┌───────────┐  ┌───────────┐  ┌───────────┐   │
         │  │ Worker-1  │  │ Worker-2  │  │ Worker-N  │   │
         │  │ Polls queue│  │ Polls queue│  │ Polls queue│  │
         │  │ Executes  │  │ Executes  │  │ Executes  │   │
         │  │ Reports   │  │ Reports   │  │ Reports   │   │
         │  │ heartbeat │  │ heartbeat │  │ heartbeat │   │
         │  └───────────┘  └───────────┘  └───────────┘   │
         └─────────────┬──────────────────────────────────┘
                       │ Status updates
         ┌─────────────▼──────────────┐
         │   Status & Retry Manager    │
         │                            │
         │  - Updates job_executions  │
         │  - Retries per policy      │
         │  - Enqueues to DLQ on      │
         │    max retries exceeded     │
         └─────────────┬──────────────┘
                       │
         ┌─────────────▼──────────────┐
         │  DAG Dependency Engine      │
         │                            │
         │  - On job COMPLETE: check  │
         │    if any downstream jobs  │
         │    have all deps satisfied │
         │  - If yes: enqueue them    │
         │  - Topological sort check  │
         │    on job registration     │
         └────────────────────────────┘

         ┌────────────────────────────────────────────────┐
         │   Dead Letter Queue (DLQ topic)                 │
         │   - Jobs that exceeded max retries              │
         │   - Alert on DLQ entries > 0                   │
         │   - Manual requeue by admin                     │
         └────────────────────────────────────────────────┘

         ┌────────────────────────────────────────────────┐
         │   Monitoring (Prometheus + Grafana)             │
         │   - Jobs fired on time / late                  │
         │   - Worker utilization                         │
         │   - Queue depth per priority                   │
         │   - DLQ depth                                  │
         └────────────────────────────────────────────────┘
```

**Component Roles:**
- **API / Control Plane:** Stateless REST/gRPC layer for managing job definitions, querying status, and admin operations. Validates cron expressions, checks for duplicate job names, and persists to the Scheduler Database.
- **Scheduler Database:** PostgreSQL — stores job definitions, computed trigger times (next_fire_time), execution history, and dependency graph. The `scheduled_triggers` table is the heart of the system.
- **Trigger Engine (Leader-elected):** The only component that polls for due jobs and fires them. Leader election via etcd prevents duplicate firing by multiple nodes. Atomically claims due triggers and enqueues them.
- **Job Queue:** Redis sorted set (score = scheduled_time + priority) or Kafka topic per priority tier. Buffers jobs between trigger and worker execution.
- **Worker Pool:** Language-agnostic processes that pull jobs from the queue, execute handlers, report status updates, and send heartbeats during long-running jobs.
- **Status & Retry Manager:** Processes worker completion events. On success: marks execution COMPLETED, triggers dependency evaluation. On failure: evaluates retry policy, re-enqueues with backoff delay, or sends to DLQ.
- **DAG Dependency Engine:** Tracks per-job dependency completion state. Uses a counter approach (decrements on each dependency completing; enqueues job when counter reaches 0).
- **Dead Letter Queue:** Kafka topic for permanently failed jobs. Triggers alerts; supports manual admin requeue.

**Primary Use-Case Data Flow (Cron Job Execution):**
1. Admin registers cron job: `POST /cron-jobs {name: "daily_report", cron: "0 2 * * *", handler: "report_generator", payload: {...}}`
2. API validates cron expression, computes `next_fire_time = 2024-01-02 02:00:00`, persists to `job_definitions` and `scheduled_triggers`
3. At 02:00:00, Trigger Engine (leader) queries: `SELECT * FROM scheduled_triggers WHERE next_fire_time <= now() AND claimed_by IS NULL FOR UPDATE SKIP LOCKED LIMIT 1000`
4. Atomically marks as `claimed_by = node_id_1`, computes `next_next_fire_time = 2024-01-03 02:00:00`, inserts new trigger row
5. Enqueues job execution to Redis sorted set with score = priority
6. Worker polls queue, dequeues job, sends heartbeat every 10s, executes handler
7. Worker completes: sends `COMPLETE` status + output to Status Manager
8. Status Manager inserts execution record into `job_executions`, updates metrics

---

## 4. Data Model

### Entities & Schema

```sql
-- Job definition (what to run, when, how often)
CREATE TABLE job_definitions (
    job_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            VARCHAR(255) UNIQUE NOT NULL,  -- human-readable, used for dedup
    job_type        VARCHAR(50) NOT NULL,          -- 'cron' | 'one_time' | 'dependent'
    handler         VARCHAR(255) NOT NULL,          -- worker handler identifier
    payload         JSONB,                          -- input data for the handler
    cron_expression VARCHAR(100),                   -- null for one-time jobs
    timezone        VARCHAR(50) DEFAULT 'UTC',
    priority        SMALLINT DEFAULT 5,             -- 0 (highest) - 9 (lowest)
    max_retries     SMALLINT DEFAULT 3,
    retry_backoff_seconds INTEGER DEFAULT 60,       -- base for exponential backoff
    timeout_seconds INTEGER DEFAULT 300,            -- max execution time
    idempotency_key VARCHAR(255) UNIQUE,            -- prevents duplicate registration
    status          VARCHAR(20) DEFAULT 'ACTIVE',   -- 'ACTIVE' | 'PAUSED' | 'DELETED'
    created_by      VARCHAR(255),
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    updated_at      TIMESTAMPTZ DEFAULT NOW(),
    next_fire_time  TIMESTAMPTZ,                    -- cached; recomputed on each fire
    last_fired_at   TIMESTAMPTZ,
    metadata        JSONB DEFAULT '{}'::JSONB
);

CREATE INDEX idx_job_next_fire ON job_definitions (next_fire_time)
    WHERE status = 'ACTIVE';

-- Scheduled trigger instances (one row per pending trigger time)
-- Separate from job_definitions to allow multiple outstanding triggers
CREATE TABLE scheduled_triggers (
    trigger_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    job_id          UUID NOT NULL REFERENCES job_definitions(job_id),
    scheduled_time  TIMESTAMPTZ NOT NULL,           -- when this trigger should fire
    claimed_by      VARCHAR(255),                   -- scheduler node that claimed it
    claimed_at      TIMESTAMPTZ,
    status          VARCHAR(20) DEFAULT 'PENDING',  -- 'PENDING' | 'CLAIMED' | 'FIRED' | 'SKIPPED'
    fire_count      INTEGER DEFAULT 0,              -- retry count for missed fires
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_trigger_schedule ON scheduled_triggers (scheduled_time, status)
    WHERE status = 'PENDING';
CREATE INDEX idx_trigger_job ON scheduled_triggers (job_id, scheduled_time);

-- Job execution instance (one row per actual run)
CREATE TABLE job_executions (
    execution_id    UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    job_id          UUID NOT NULL REFERENCES job_definitions(job_id),
    trigger_id      UUID REFERENCES scheduled_triggers(trigger_id),
    run_number      INTEGER NOT NULL DEFAULT 1,     -- 1=first attempt, 2=first retry, etc.
    status          VARCHAR(20) NOT NULL DEFAULT 'QUEUED',
                    -- 'QUEUED' | 'RUNNING' | 'COMPLETED' | 'FAILED' | 'TIMED_OUT' | 'CANCELLED' | 'RETRYING'
    worker_id       VARCHAR(255),                   -- which worker picked it up
    queued_at       TIMESTAMPTZ DEFAULT NOW(),
    started_at      TIMESTAMPTZ,
    completed_at    TIMESTAMPTZ,
    duration_ms     BIGINT,                         -- computed on completion
    output          JSONB,                          -- result data or error details
    error_message   TEXT,
    error_type      VARCHAR(100),                   -- exception class name
    last_heartbeat  TIMESTAMPTZ,                    -- updated by worker every 10s
    fencing_token   BIGINT,                         -- monotonic; prevents zombie writes
    scheduled_time  TIMESTAMPTZ,                    -- when this was supposed to run
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Partition by month for query performance on large history
-- CREATE TABLE job_executions_2024_01 PARTITION OF job_executions
--     FOR VALUES FROM ('2024-01-01') TO ('2024-02-01');

CREATE INDEX idx_exec_job_time ON job_executions (job_id, queued_at DESC);
CREATE INDEX idx_exec_status ON job_executions (status, queued_at)
    WHERE status IN ('QUEUED', 'RUNNING');
CREATE INDEX idx_exec_heartbeat ON job_executions (last_heartbeat)
    WHERE status = 'RUNNING';

-- Job dependency graph
CREATE TABLE job_dependencies (
    job_id          UUID NOT NULL REFERENCES job_definitions(job_id),
    depends_on_job_id UUID NOT NULL REFERENCES job_definitions(job_id),
    condition       VARCHAR(20) DEFAULT 'COMPLETED',  -- 'COMPLETED' | 'SUCCEEDED' | 'FAILED'
    PRIMARY KEY (job_id, depends_on_job_id)
);

CREATE INDEX idx_dep_upstream ON job_dependencies (depends_on_job_id);

-- Dependency fulfillment tracker (per execution of a dependent job)
CREATE TABLE dependency_fulfillments (
    execution_id        UUID NOT NULL,              -- the blocked job's execution_id
    dependency_job_id   UUID NOT NULL,              -- which upstream job we're waiting for
    upstream_exec_id    UUID,                       -- which execution of upstream fulfilled it
    fulfilled_at        TIMESTAMPTZ,
    PRIMARY KEY (execution_id, dependency_job_id)
);

-- Dead letter queue (jobs that exhausted retries)
CREATE TABLE dead_letter_jobs (
    dlq_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    job_id          UUID NOT NULL,
    execution_id    UUID NOT NULL,
    final_error     TEXT,
    final_error_type VARCHAR(100),
    total_attempts  INTEGER,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    resolved_at     TIMESTAMPTZ,
    resolved_by     VARCHAR(255),
    requeue_execution_id UUID
);
```

### Database Choice

| Option | Pros | Cons | Fit |
|---|---|---|---|
| **PostgreSQL** | ACID transactions for trigger claiming; SKIP LOCKED for queue; JSONB for payload; mature partitioning; rich query for history | Write throughput ceiling (~10K TPS); trigger polling adds load | **Selected** |
| MySQL | Familiar, `SELECT FOR UPDATE SKIP LOCKED` available | Less mature JSONB; advisory locks less reliable | Acceptable alternative |
| Cassandra | Write-optimized; time-series naturally | No atomic CAS transactions for trigger claiming; complex range queries | Not suitable for trigger claiming |
| DynamoDB | Managed, scalable | Conditional writes for trigger claiming are complex; no SKIP LOCKED equivalent | Possible but complex |
| Redis (for job queue only) | Ultra-fast sorted set queue | Not for durable job history; lose data on restart without AOF | Used as queue layer only |
| TimescaleDB | Excellent time-series performance for execution history | Adds complexity; PostgreSQL extension | Add-on for execution history table if needed |

**Selected: PostgreSQL for all persistent state + Redis for the live job queue.**  
Justification: The trigger claiming pattern requires atomic `UPDATE ... WHERE claimed_by IS NULL FOR UPDATE SKIP LOCKED` — a native PostgreSQL feature that provides lock-free queue semantics without application-level concurrency control. The execution history is a time-series append workload suited to partitioned PostgreSQL tables. JSONB supports flexible payload and output storage. Redis sorted sets provide sub-millisecond priority queue operations for the active job dispatch layer.

---

## 5. API Design

**Protocol:** REST/JSON for external clients; gRPC internally between components.  
**Authentication:** JWT Bearer tokens with `sub` = service account. Scopes: `jobs:read`, `jobs:write`, `jobs:admin`.  
**Rate Limiting:** 100 job submissions/sec per service account; 1,000 status reads/sec per account.

```
-- Job Definition Management --

POST /v1/cron-jobs
  Auth: Bearer token with jobs:write scope
  Body: {
    "name": "daily_sales_report",
    "handler": "report_generator:sales_daily",
    "cron_expression": "0 2 * * *",
    "timezone": "America/New_York",
    "payload": {"report_type": "sales", "format": "pdf"},
    "priority": 3,
    "max_retries": 3,
    "retry_backoff_seconds": 300,
    "timeout_seconds": 600,
    "idempotency_key": "daily_sales_report_v1"
  }
  Response: 201 {
    "job_id": "uuid-...",
    "next_fire_time": "2024-01-02T07:00:00Z",
    "status": "ACTIVE"
  }
  Errors: 409 CONFLICT (idempotency_key already exists)
          400 BAD_REQUEST (invalid cron expression)

PUT /v1/cron-jobs/{job_id}
  Body: { "cron_expression": "0 3 * * *" }   -- reschedule
  Response: 200 { "job_id": "...", "next_fire_time": "..." }
  Notes: Updates next_fire_time; pending trigger NOT cancelled (fires once at old time)

DELETE /v1/cron-jobs/{job_id}
  Query: ?cancel_running=true  -- optionally cancel running executions
  Response: 204 No Content

POST /v1/jobs                    -- one-time job
  Body: {
    "handler": "email_sender:welcome",
    "payload": {"user_id": 12345, "template": "welcome"},
    "scheduled_time": "2024-01-02T15:30:00Z",
    "priority": 5,
    "idempotency_key": "welcome_email_user_12345"
  }
  Response: 201 { "job_id": "...", "execution_id": "..." }

-- Job Execution Control --

POST /v1/jobs/{job_id}/trigger   -- immediate manual trigger (creates extra execution)
  Auth: jobs:admin scope
  Response: 201 { "execution_id": "..." }

POST /v1/executions/{execution_id}/cancel
  Auth: jobs:write scope
  Response: 202 Accepted  -- async; worker receives cancel signal via heartbeat response

POST /v1/executions/{execution_id}/retry  -- admin force-retry from DLQ
  Auth: jobs:admin scope
  Response: 201 { "new_execution_id": "..." }

POST /v1/cron-jobs/{job_id}/pause    -- stop future fires; current run unaffected
POST /v1/cron-jobs/{job_id}/resume   -- re-enable; sets new next_fire_time from now

-- Status Queries --

GET /v1/executions/{execution_id}
  Response: 200 {
    "execution_id": "...",
    "job_id": "...",
    "status": "RUNNING",
    "worker_id": "worker-42",
    "started_at": "2024-01-02T07:00:01Z",
    "last_heartbeat": "2024-01-02T07:00:55Z",
    "scheduled_time": "2024-01-02T07:00:00Z",
    "run_number": 1
  }

GET /v1/jobs/{job_id}/executions?status=FAILED&limit=50&after=2024-01-01T00:00:00Z
  Response: 200 {
    "executions": [...],
    "total": 12,
    "next_cursor": "..."
  }

GET /v1/metrics/overview
  Response: {
    "active_jobs": 95432,
    "executions_last_1h": 8200,
    "failed_last_1h": 23,
    "dlq_depth": 5,
    "worker_utilization": 0.62,
    "queue_depth": {"priority_0": 12, "priority_5": 340, "priority_9": 2}
  }

-- Worker Internal API (called by workers) --

POST /v1/internal/executions/{execution_id}/heartbeat
  Headers: X-Worker-Id: worker-42, X-Fencing-Token: 42
  Body: { "progress_pct": 45, "status_message": "Processing batch 3/7" }
  Response: 200 { "cancel_requested": false } | 200 { "cancel_requested": true }

POST /v1/internal/executions/{execution_id}/complete
  Headers: X-Worker-Id: worker-42, X-Fencing-Token: 42
  Body: { "status": "COMPLETED", "output": {...} | "error_message": "..." }
  Response: 200 OK | 409 CONFLICT (fencing token mismatch → zombie write rejected)
```

---

## 6. Deep Dive: Core Components

### 6.1 Exactly-Once Job Firing (Trigger Engine)

**Problem it solves:** Multiple scheduler nodes running simultaneously could each detect that a job is due and each fire it, causing duplicate executions. The trigger engine must guarantee that each scheduled trigger fires exactly once, even if multiple nodes are healthy and racing.

**Approaches Comparison:**

| Approach | Mechanism | Exactly-Once? | Failure Behavior |
|---|---|---|---|
| **Single leader with hot standby (etcd election)** | Only leader fires; standby takes over on failure | Yes | Up to election timeout (15s) of missed fires; acceptable |
| Database row locking (`FOR UPDATE SKIP LOCKED`) | Each scheduler claims rows atomically; unclaimed rows taken by another | Yes (per-row) | Works with multiple active schedulers; no leader needed |
| Distributed lock per trigger | Lock per trigger_id before firing | Yes | Lock service becomes SPOF; overhead per trigger |
| Message deduplication (Kafka exactly-once) | All schedulers produce to Kafka; dedup by trigger_id | Near-exactly-once | Depends on Kafka EOS; works well |
| Epoch-based watermarks | Only one node "owns" a time range | Yes | Complex epoch management |

**Selected: Database-level atomic row claim with `FOR UPDATE SKIP LOCKED`**

This approach allows multiple scheduler nodes to operate simultaneously (no leader election needed), with PostgreSQL ensuring each trigger row is claimed by exactly one node.

**Trigger Engine Algorithm:**
```python
class TriggerEngine:
    def __init__(self, db, job_queue, node_id):
        self.db = db
        self.queue = job_queue
        self.node_id = node_id
        self.poll_interval_sec = 1  # check every second

    def run(self):
        while True:
            try:
                self.fire_due_triggers()
            except Exception as e:
                log.error(f"Trigger engine error: {e}")
            time.sleep(self.poll_interval_sec)

    def fire_due_triggers(self):
        now = datetime.utcnow()
        look_ahead = now + timedelta(seconds=1)  # claim triggers up to 1s in future

        with self.db.transaction():
            # SKIP LOCKED: skip rows locked by other scheduler nodes
            # This is the key to multi-node correctness: each row claimed exactly once
            triggers = self.db.execute("""
                SELECT t.trigger_id, t.job_id, t.scheduled_time, j.handler,
                       j.payload, j.priority, j.max_retries, j.timeout_seconds,
                       j.cron_expression, j.timezone
                FROM scheduled_triggers t
                JOIN job_definitions j ON t.job_id = j.job_id
                WHERE t.scheduled_time <= :look_ahead
                  AND t.status = 'PENDING'
                  AND j.status = 'ACTIVE'
                ORDER BY t.scheduled_time ASC, j.priority ASC
                LIMIT 1000
                FOR UPDATE OF t SKIP LOCKED
            """, {"look_ahead": look_ahead})

            if not triggers:
                return

            trigger_ids = [t.trigger_id for t in triggers]

            # Mark as claimed (atomic within transaction)
            self.db.execute("""
                UPDATE scheduled_triggers
                SET status = 'CLAIMED',
                    claimed_by = :node_id,
                    claimed_at = NOW()
                WHERE trigger_id = ANY(:trigger_ids)
            """, {"node_id": self.node_id, "trigger_ids": trigger_ids})

            # Create execution records
            execution_records = []
            queue_items = []

            for trigger in triggers:
                execution_id = uuid4()
                fencing_token = self.allocate_fencing_token(trigger.job_id)

                execution_records.append({
                    "execution_id": execution_id,
                    "job_id": trigger.job_id,
                    "trigger_id": trigger.trigger_id,
                    "status": "QUEUED",
                    "fencing_token": fencing_token,
                    "scheduled_time": trigger.scheduled_time,
                    "queued_at": now
                })

                queue_items.append({
                    "execution_id": str(execution_id),
                    "job_id": str(trigger.job_id),
                    "handler": trigger.handler,
                    "payload": trigger.payload,
                    "fencing_token": fencing_token,
                    "timeout_seconds": trigger.timeout_seconds,
                    "priority": trigger.priority
                })

            # Bulk insert execution records
            self.db.bulk_insert("job_executions", execution_records)

            # Compute and insert next trigger for recurring jobs
            new_triggers = []
            for trigger in triggers:
                if trigger.cron_expression:
                    next_time = self.compute_next_fire(
                        trigger.cron_expression, trigger.timezone, trigger.scheduled_time
                    )
                    new_triggers.append({
                        "trigger_id": uuid4(),
                        "job_id": trigger.job_id,
                        "scheduled_time": next_time,
                        "status": "PENDING"
                    })

            if new_triggers:
                self.db.bulk_insert("scheduled_triggers", new_triggers)

            # Mark fired
            self.db.execute("""
                UPDATE scheduled_triggers SET status = 'FIRED'
                WHERE trigger_id = ANY(:trigger_ids)
            """, {"trigger_ids": trigger_ids})

        # Enqueue to Redis AFTER transaction commits (transactional outbox pattern)
        for item in queue_items:
            self.queue.enqueue(item)

    def compute_next_fire(self, cron_expr, timezone, after):
        # Use croniter library (Python) or cron-parser (Node.js)
        cron = CronIterator(cron_expr, start_time=after, tz=timezone)
        return cron.get_next(datetime)
```

**Missed Fire Recovery (catch-up):**
```python
def handle_missed_fires():
    # On scheduler restart: check for PENDING triggers with scheduled_time in the past
    # These were missed while scheduler was down
    missed = db.execute("""
        SELECT * FROM scheduled_triggers
        WHERE scheduled_time < NOW() - INTERVAL '10 seconds'
          AND status = 'PENDING'
        FOR UPDATE SKIP LOCKED
        LIMIT 500
    """)

    for trigger in missed:
        job = get_job_definition(trigger.job_id)
        if job.missed_fire_policy == 'FIRE_ONCE':
            # Fire it now, once, regardless of how many were missed
            enqueue_single_execution(trigger)
        elif job.missed_fire_policy == 'FIRE_ALL':
            # Fire once per missed interval (use with caution)
            fire_all_missed(trigger, job)
        elif job.missed_fire_policy == 'SKIP':
            # Mark as SKIPPED; no execution created
            mark_trigger_skipped(trigger)
```

**Interviewer Q&As:**

Q: How does `FOR UPDATE SKIP LOCKED` prevent duplicate job firing?  
A: `FOR UPDATE` acquires a row-level write lock on each selected row. `SKIP LOCKED` makes the query skip rows that are already locked by other transactions instead of waiting. If two scheduler nodes simultaneously run the same query, node A acquires locks on rows 1–500 and node B acquires locks on rows 501–1000 (assuming the batch is large enough). They each claim disjoint sets of triggers, preventing any trigger from being claimed twice. The lock is held for the duration of the transaction (commit/rollback). This PostgreSQL feature was specifically designed for queue-like processing patterns.

Q: What happens if a scheduler node crashes after claiming triggers but before enqueueing them to Redis?  
A: The triggers are in `CLAIMED` status in the database but never reach the job queue. They are stuck until a recovery mechanism runs. Recovery: a background process (or the same trigger engine on restart) queries for triggers stuck in `CLAIMED` status for more than `claim_timeout` (e.g., 30 seconds): `SELECT * FROM scheduled_triggers WHERE status = 'CLAIMED' AND claimed_at < NOW() - INTERVAL '30 seconds' FOR UPDATE SKIP LOCKED`. It resets these to `PENDING`, allowing them to be re-claimed. This is the "outbox pattern recovery."

Q: How do you handle the scenario where the same cron job is registered multiple times (duplicate registration)?  
A: The `idempotency_key` field on `job_definitions` has a UNIQUE constraint. If a second registration request arrives with the same `idempotency_key`, the `INSERT` fails with a unique constraint violation, and the API returns `409 CONFLICT` with the existing job_id. If the `idempotency_key` is the same and the payload differs (attempted update via create), return `409 CONFLICT` with a message indicating the key is already used. For intentional updates, the caller must use `PUT /v1/cron-jobs/{job_id}`.

Q: How do you handle time zone changes (e.g., DST transitions) for cron jobs?  
A: Store `timezone` alongside the cron expression. Use the timezone-aware cron next-time calculator (e.g., `croniter` with `tzlocal`, which handles DST transitions correctly). For a job scheduled at `"0 2 * * *"` in `America/New_York`: during the "fall back" DST transition, 2 AM occurs twice. The cron library must either fire once (skip the duplicate) or fire twice (fire at both 2 AM instances) based on configuration. Standard: fire once. For the "spring forward" transition (2 AM to 3 AM), 2 AM never occurs — the cron library should fire at 3 AM (the next valid time). This is handled correctly by `croniter` and standard cron implementations.

Q: How do you prevent the scheduler from creating too many trigger rows for a high-frequency cron job?  
A: The design inserts exactly ONE new trigger row per fired trigger (the next occurrence). At any point in time, there is at most one PENDING trigger per cron job. This means there is no pre-population of future triggers. The `next_fire_time` column on `job_definitions` is a cache for informational purposes. Pre-inserting all future triggers (e.g., 1 year × once/minute = 525,600 rows) would consume storage unnecessarily and complicate management. One-trigger-ahead is the correct approach.

---

### 6.2 Job Deduplication & Exactly-Once Execution

**Problem it solves:** A job worker may crash after completing the work but before acknowledging the completion. The retry mechanism would re-execute the job, causing duplicate side effects (e.g., sending an email twice, charging a customer twice). Exactly-once execution prevents this.

**Approaches Comparison:**

| Approach | Mechanism | Exactly-Once? | Complexity |
|---|---|---|---|
| **Fencing token + idempotency key at worker** | Worker checks execution_id in a result store before executing | Yes, if worker handles it | Requires worker cooperation |
| **Transactional outbox** | Worker writes result and acks in same transaction | Yes | Requires worker DB access |
| **Distributed lock per execution_id** | Lock prevents concurrent execution of same job | Yes for concurrency; not for retries | Doesn't help with retry-after-crash |
| **Idempotency table** | Record execution_id before side effect; check before executing | Yes | Simple, widely applicable |
| **At-least-once + idempotent side effects** | Accept retries; make all side effects idempotent | Effectively yes | Requires idempotent worker design |

**Selected: Fencing token + worker-side idempotency check + exactly-once at the dequeue level.**

**Implementation:**
```python
class Worker:
    def process_job(self, job_item):
        execution_id = job_item["execution_id"]
        fencing_token = job_item["fencing_token"]
        handler = job_item["handler"]
        payload = job_item["payload"]

        # Step 1: Atomically mark as RUNNING (prevents concurrent duplicate execution)
        # Uses fencing_token to detect stale/zombie workers
        with db.transaction():
            updated = db.execute("""
                UPDATE job_executions
                SET status = 'RUNNING',
                    worker_id = :worker_id,
                    started_at = NOW(),
                    last_heartbeat = NOW()
                WHERE execution_id = :execution_id
                  AND status = 'QUEUED'                -- only claim if still queued
                  AND fencing_token = :fencing_token   -- zombie detection
            """, {"worker_id": self.worker_id,
                  "execution_id": execution_id,
                  "fencing_token": fencing_token})

            if updated == 0:
                # Either already running (duplicate dequeue) or fencing_token mismatch
                # (stale job item) — skip silently
                log.warning(f"Skipping execution {execution_id}: already claimed or stale token")
                return

        # Step 2: Check worker-side idempotency (for side effects like sending email)
        if self.idempotency_store.already_processed(execution_id):
            # This execution already succeeded; just mark complete
            self.mark_complete(execution_id, fencing_token, cached_result=True)
            return

        # Step 3: Execute handler with heartbeat
        result = None
        error = None
        try:
            with timeout(job_item["timeout_seconds"]):
                result = self.execute_handler(handler, payload, execution_id)
                # Record idempotency BEFORE reporting completion
                # (in case report fails and we retry)
                self.idempotency_store.record(execution_id, result)
        except TimeoutError:
            error = {"type": "TIMEOUT", "message": f"Exceeded {job_item['timeout_seconds']}s"}
        except Exception as e:
            error = {"type": type(e).__name__, "message": str(e)}

        # Step 4: Report completion
        self.report_completion(execution_id, fencing_token, result, error)

    def heartbeat_loop(self, execution_id, fencing_token):
        while self.is_running(execution_id):
            response = api.post(f"/v1/internal/executions/{execution_id}/heartbeat",
                                headers={"X-Fencing-Token": str(fencing_token)})
            if response.json().get("cancel_requested"):
                self.cancel_execution(execution_id)
                return
            time.sleep(10)

    def report_completion(self, execution_id, fencing_token, result, error):
        response = api.post(
            f"/v1/internal/executions/{execution_id}/complete",
            headers={"X-Fencing-Token": str(fencing_token)},
            json={"status": "COMPLETED" if not error else "FAILED",
                  "output": result, "error_message": error}
        )
        if response.status_code == 409:
            log.warning(f"Fencing token mismatch for {execution_id}: zombie write rejected")
            # Our fencing_token is stale; another worker has taken over
            # Stop immediately; do not retry
```

**Retry Policy Engine:**
```python
class RetryManager:
    def handle_failure(self, execution_id, error):
        exec_record = db.get_execution(execution_id)
        job_def = db.get_job(exec_record.job_id)

        if exec_record.run_number >= job_def.max_retries + 1:
            # Exhausted retries → send to DLQ
            self.send_to_dlq(exec_record, error)
            self.update_status(execution_id, "FAILED")
            return

        # Compute next retry time with exponential backoff + jitter
        backoff_base = job_def.retry_backoff_seconds
        retry_number = exec_record.run_number  # 1=first retry, 2=second, etc.
        # Exponential backoff: base * 2^(retry_number - 1)
        # Example: base=60s, retry 1: 60s, retry 2: 120s, retry 3: 240s
        delay = backoff_base * (2 ** (retry_number - 1))
        # Add jitter: ±25% to prevent thundering herd of retries
        jitter = random.uniform(0.75, 1.25)
        delay_with_jitter = int(delay * jitter)
        next_retry_at = datetime.utcnow() + timedelta(seconds=delay_with_jitter)

        # Create new execution record for the retry
        new_execution_id = self.create_retry_execution(
            exec_record, run_number=retry_number + 1,
            scheduled_time=next_retry_at
        )

        # Schedule via trigger engine (insert into scheduled_triggers)
        db.insert("scheduled_triggers", {
            "trigger_id": uuid4(),
            "job_id": exec_record.job_id,
            "scheduled_time": next_retry_at,
            "status": "PENDING"
        })

        log.info(f"Scheduled retry {retry_number+1} for job {exec_record.job_id} "
                 f"at {next_retry_at} (delay: {delay_with_jitter}s)")

    def handle_timeout(self, execution_id):
        # Background process checks for executions where last_heartbeat is stale
        # A worker that stopped heartbeating is considered dead
        stale_threshold = datetime.utcnow() - timedelta(seconds=60)  # 6× heartbeat interval
        
        stale_executions = db.execute("""
            SELECT execution_id, job_id, run_number, fencing_token
            FROM job_executions
            WHERE status = 'RUNNING'
              AND last_heartbeat < :threshold
              AND started_at < :timeout_threshold
        """, {"threshold": stale_threshold})
        
        for exec_rec in stale_executions:
            log.warning(f"Execution {exec_rec.execution_id} timed out (no heartbeat)")
            db.update_status(exec_rec.execution_id, "TIMED_OUT",
                             error_message="No heartbeat within 60 seconds")
            self.handle_failure(exec_rec.execution_id, {"type": "WORKER_TIMEOUT"})
```

**Interviewer Q&As:**

Q: How do you achieve exactly-once execution when the worker crashes after executing but before reporting completion?  
A: This is the classic at-least-once problem. True exactly-once requires either: (1) the side effect and the "mark complete" are in the same atomic transaction (only possible if the side effect is a DB write in the same database as the execution state), or (2) the side effect is idempotent and the worker records the execution_id before performing the side effect, so on retry it checks if the work was already done. For truly non-idempotent side effects (e.g., sending a physical letter), accept at-least-once and build in human review for duplicates.

Q: How does the fencing token prevent zombie workers from marking a job as complete?  
A: When a worker takes a job, it receives a `fencing_token` (monotonically increasing number). When reporting completion, it sends this token. The Status Manager checks `WHERE execution_id = ? AND fencing_token = ?`. If the token doesn't match (because the timeout handler already retried the job and issued a new token to a new worker), the old worker's completion report is rejected with `409 CONFLICT`. The old worker (zombie) sees this and stops processing, knowing it is stale.

Q3: How do you handle a job that is scheduled every second and takes 2 seconds to run?  
A: This is an "overrun" scenario. Options: (1) **Skip if running** (`skip_if_running=true`): if a trigger fires while the previous execution is still running, mark the new trigger as SKIPPED. (2) **Queue it anyway**: multiple executions of the same job can run in parallel. (3) **Coalesce**: if N triggers fired while the job was running, fire only once after the current execution completes. The right choice depends on the job semantics. For idempotent jobs that aggregate time-window data (e.g., aggregate last 5 minutes of metrics), coalescing is correct. For independent batch runs, queuing all is fine. The `missed_fire_policy` and `allow_concurrent_runs` settings on `job_definitions` control this.

---

### 6.3 Job Dependency DAGs

**Problem it solves:** Complex pipelines require jobs to execute in a specific order. Job B should not start until Job A completes successfully. Multiple jobs can run in parallel if their dependencies are satisfied. A cycle in the dependency graph is invalid and must be rejected.

**DAG Execution Engine:**
```python
class DagEngine:
    def on_job_completed(self, execution_id, status):
        """Called by Status Manager when a job execution completes."""
        exec_record = db.get_execution(execution_id)
        job_id = exec_record.job_id

        if status not in ["COMPLETED", "SUCCEEDED"]:
            # Downstream jobs remain blocked; optionally fail them or alert
            self.handle_upstream_failure(job_id, execution_id, status)
            return

        # Find all jobs that depend on this job
        downstream_jobs = db.execute("""
            SELECT d.job_id, d.condition
            FROM job_dependencies d
            WHERE d.depends_on_job_id = :completed_job_id
        """, {"completed_job_id": job_id})

        for downstream in downstream_jobs:
            if downstream.condition == "COMPLETED" and status in ["COMPLETED", "FAILED"]:
                self.check_and_unblock(downstream.job_id, job_id, execution_id)
            elif downstream.condition == "SUCCEEDED" and status == "COMPLETED":
                self.check_and_unblock(downstream.job_id, job_id, execution_id)
            elif downstream.condition == "FAILED" and status == "FAILED":
                self.check_and_unblock(downstream.job_id, job_id, execution_id)

    def check_and_unblock(self, blocked_job_id, fulfilled_dep_id, fulfilling_exec_id):
        """Check if all dependencies of blocked_job_id are now satisfied."""
        with db.transaction():
            # Record this dependency as fulfilled
            db.execute("""
                INSERT INTO dependency_fulfillments
                    (execution_id, dependency_job_id, upstream_exec_id, fulfilled_at)
                SELECT e.execution_id, :dep_id, :upstream_exec_id, NOW()
                FROM job_executions e
                WHERE e.job_id = :blocked_job_id
                  AND e.status = 'WAITING_FOR_DEPENDENCIES'
                ON CONFLICT DO NOTHING
            """, {"dep_id": fulfilled_dep_id,
                  "upstream_exec_id": fulfilling_exec_id,
                  "blocked_job_id": blocked_job_id})

            # Check if all dependencies are now fulfilled
            unfulfilled_count = db.scalar("""
                SELECT COUNT(*)
                FROM job_dependencies jd
                LEFT JOIN dependency_fulfillments df
                    ON df.dependency_job_id = jd.depends_on_job_id
                    AND df.execution_id = (
                        SELECT execution_id FROM job_executions
                        WHERE job_id = :blocked_job_id
                          AND status = 'WAITING_FOR_DEPENDENCIES'
                        ORDER BY queued_at DESC LIMIT 1
                    )
                WHERE jd.job_id = :blocked_job_id
                  AND df.fulfilled_at IS NULL
            """, {"blocked_job_id": blocked_job_id})

            if unfulfilled_count == 0:
                # All deps satisfied — transition to QUEUED
                execution_id = db.scalar("""
                    UPDATE job_executions
                    SET status = 'QUEUED', queued_at = NOW()
                    WHERE job_id = :job_id
                      AND status = 'WAITING_FOR_DEPENDENCIES'
                    RETURNING execution_id
                """, {"job_id": blocked_job_id})

                if execution_id:
                    job_item = build_queue_item(execution_id, blocked_job_id)
                    job_queue.enqueue(job_item)

    def validate_dag_on_registration(self, job_id, dependencies):
        """Called when registering a job with dependencies. Rejects cycles."""
        # Build adjacency list of entire graph + new edges
        all_deps = db.get_all_dependencies()
        graph = build_adjacency_list(all_deps)
        for dep in dependencies:
            graph[job_id].append(dep)

        # Topological sort (Kahn's algorithm) — fails if cycle exists
        in_degree = {node: 0 for node in graph}
        for node, neighbors in graph.items():
            for n in neighbors:
                in_degree[n] = in_degree.get(n, 0) + 1

        queue = [n for n in in_degree if in_degree[n] == 0]
        processed = 0
        while queue:
            node = queue.pop(0)
            processed += 1
            for neighbor in graph.get(node, []):
                in_degree[neighbor] -= 1
                if in_degree[neighbor] == 0:
                    queue.append(neighbor)

        if processed != len(graph):
            raise CycleDetectedError(f"Adding job {job_id} creates a dependency cycle")
```

**Interviewer Q&As:**

Q: How do you handle a partial DAG failure where one branch fails and another succeeds?  
A: Define failure propagation policy per edge: `condition = "SUCCEEDED"` means the downstream job only runs if the upstream succeeded. `condition = "COMPLETED"` means it runs regardless of upstream outcome (even failure). `condition = "FAILED"` runs the downstream only on upstream failure (useful for cleanup/compensation jobs). If a required upstream fails and the downstream requires success, the downstream remains in `WAITING_FOR_DEPENDENCIES` forever — an alert fires, an admin decides whether to force-run the downstream or abort the entire DAG run. For pipeline automation, add a DAG-level failure handler job.

Q: How do you track which "version" of an upstream job a downstream job depended on (when jobs recur)?  
A: The `dependency_fulfillments` table records the specific `upstream_exec_id` that fulfilled each dependency. When a DAG runs for the nth time (all recurring jobs), each downstream execution is linked to the specific upstream execution IDs from that same run cycle. This is managed by grouping executions by a shared `pipeline_run_id` (a UUID generated when the root trigger fires, propagated to all downstream jobs it triggers). This way, the second run of Job B waits for the second run of Job A, not the first.

Q5: How do you prevent a long-running DAG from blocking new DAG runs?  
A: Allow concurrent DAG runs with the `allow_concurrent_runs` flag. Each run gets a unique `pipeline_run_id`. The DAG engine tracks dependency fulfillments per pipeline_run_id, so run N's downstream jobs wait for run N's upstream jobs, independent of run N-1. Alert if the number of in-flight pipeline runs exceeds a threshold (e.g., > 3) for a given DAG, indicating the DAG's cycle time is longer than its schedule interval.

---

## 7. Scaling

### Horizontal Scaling

**Trigger Engine:** Multiple nodes can run simultaneously using `FOR UPDATE SKIP LOCKED` — they naturally partition the work without coordination. At 139 jobs/sec, a single trigger engine node handles easily. Scale to 3–5 nodes for HA.

**API Layer:** Stateless; add instances behind load balancer. Target: 100 API instances for 10,000 API requests/sec.

**Worker Pool:** Add workers to consume from the queue faster. Workers are stateless; container orchestration (Kubernetes with HPA on queue depth) auto-scales. Target autoscaler: when queue depth > 50 for > 30 seconds, add 10 workers.

**Queue Scaling:** Redis cluster (multiple shards by priority level) or Kafka (one topic per priority tier). For 10,000 jobs/min = 167 jobs/sec, Redis single instance handles trivially.

### DB Sharding
PostgreSQL is the bottleneck at high write rates. Strategies:
- **Partition execution history by time** (monthly partitions): old partitions become read-only and can be archived to cheaper storage or Parquet files in S3.
- **Read replicas** for status queries (dashboards, history API) — only the trigger engine and status writes go to primary.
- **Vertical scaling** for primary: r6g.4xlarge (16 vCPU, 128 GB RAM) handles 50K TPS for OLTP workloads.
- If trigger table grows large: keep only `PENDING` rows in the hot table; archive `FIRED`/`SKIPPED` to history table.

### Caching
- **Job definition cache (Redis):** Cache frequently-read job definitions in Redis with a 60s TTL. Invalidate on `PUT /v1/cron-jobs/{job_id}`. Saves DB reads for every trigger fire.
- **Next fire time cache:** In Redis, cache `next_fire_time` per job_id. Trigger engine checks Redis first; refreshes from DB on miss.

### Interviewer Q&As (Scaling)

Q: How do you handle the "midnight spike" where 10,000 jobs are scheduled at 00:00?  
A: Pre-warm the job queue: 5 minutes before midnight, the trigger engine queries for jobs scheduled at 00:00 and pre-enqueues them. This spreads the DB load across 5 minutes. For the workers: autoscale worker pool 10 minutes before midnight based on predicted load (historical data shows midnight spikes). Use a capacity headroom of 2× normal peak for the midnight window. For the queue: Redis sorted set ordered by scheduled_time handles 10,000 items instantly. For DB: batch-insert all 10,000 execution records in a single transaction (bulk insert is 100× faster than individual inserts).

Q: How would you scale the system to 10 million distinct cron jobs?  
A: The current design hits limits at ~1M jobs due to trigger table scan cost. Scale: (1) Partition `scheduled_triggers` by `scheduled_time` (daily partitions); only the current day's partition is hot. (2) Add a dedicated trigger index service (e.g., a sorted set in Redis where key = job_id, score = next_fire_time — fast range scan to find all jobs due in the next 10 seconds). (3) Pre-compute next_fire_time for all jobs and store in Redis sorted set; the trigger engine reads from Redis instead of PostgreSQL for time-based lookups. (4) Shard job_definitions by job_id range across multiple DB instances.

Q: How does worker autoscaling work with Kubernetes HPA and a Redis queue?  
A: Install KEDA (Kubernetes Event-Driven Autoscaler). Configure a `ScaledObject` with trigger type `redis` pointing to the queue list length. KEDA queries Redis `LLEN queue:priority:5` every 30 seconds. When length > `desiredReplicas × targetAverageValue` (e.g., target 10 pending jobs per worker), KEDA increases `Deployment.spec.replicas`. When queue is drained, HPA scales down (with a scale-down delay of 5 minutes to avoid thrashing). Max replicas = 500 (worker pool size limit); min replicas = 10 (always warm).

Q: How do you prevent runaway jobs (infinite loops) from consuming all workers?  
A: `timeout_seconds` on the job definition is the primary guard. The worker process has a watchdog thread that kills the job after the timeout. The heartbeat system provides a secondary check: if a job has been running for > `2 × timeout_seconds` without completing (possibly the watchdog failed), the Status Manager's timeout scanner marks it TIMED_OUT and re-queues. For hard resource limits: run workers in containers with CPU/memory limits. For job-type-level sandboxing: use separate worker pools per handler type, preventing one misbehaving job type from starving others.

Q: What is the throughput limit of a single PostgreSQL instance as the scheduler DB?  
A: A well-tuned PostgreSQL r6g.4xlarge (16 vCPU, 128 GB RAM, gp3 SSD) sustains ~50K simple TPS. The trigger engine's `FOR UPDATE SKIP LOCKED` query (with proper indexes) takes ~1–5ms per batch of 1,000 rows. At 139 jobs/sec, the scheduler performs ~0.14 batch queries/sec — negligible. The status update stream (139 writes/sec) is also trivial. Bottleneck appears at ~5,000–10,000 jobs/sec, at which point the write throughput (status updates + execution inserts + trigger updates) approaches 50K TPS. Mitigation: batch status updates, use WAL-based replication to read replicas, and partition execution history.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Recovery |
|---|---|---|---|
| Trigger engine node crash | Other nodes take over via SKIP LOCKED (no single leader needed) | LB health check | Automatic; no missed fires if multiple nodes running |
| PostgreSQL primary crash | Trigger engine and API cannot write | DB health check; connection pool errors | Failover to replica (RDS Multi-AZ: < 30s); triggers missed during outage recovered via missed-fire handler |
| Worker crash mid-execution | Heartbeat stops; execution stays RUNNING | Heartbeat timeout scanner (60s threshold) | Execution marked TIMED_OUT; retry scheduled per policy |
| Redis queue crash | Job dispatch halted; jobs stay in DB as PENDING | Redis health check | Trigger engine re-reads PENDING triggers from DB and re-enqueues; acceptable delay |
| Job execution stuck (infinite loop) | Worker busy; not processing new jobs | Timeout scanner + container resource limits | Watchdog kills container; retry scheduled |
| DAG upstream permanent failure | Downstream jobs blocked indefinitely | DLQ alert for upstream + DAG stall detector (jobs in WAITING_FOR_DEPENDENCIES > N hours) | Alert ops; admin decides to force-run downstream or abort DAG run |
| Burst of retries (many jobs failing simultaneously) | Retry storm floods worker pool | DLQ depth spike + queue depth alert | Exponential backoff + jitter spreads retries; DLQ alert triggers investigation |
| Clock skew between scheduler nodes | Two nodes may both think a trigger is due | NTP monitoring | SKIP LOCKED prevents double-claiming regardless of clock skew |
| Data center failure | Full outage | DC-level monitoring | Multi-AZ RDS + multi-AZ trigger engine nodes; recovery in < 5 min per RDS failover |

**Idempotency of Trigger Firing:**  
Even if a trigger is claimed and enqueued twice (e.g., DB transaction commit + Redis enqueue fails, then retry), the `job_executions` table's unique constraint on `(trigger_id, run_number)` prevents duplicate execution records. Worker's `UPDATE ... WHERE status = 'QUEUED' AND fencing_token = ?` prevents duplicate execution of the same job.

---

## 9. Monitoring & Observability

| Metric | Source | Alert Threshold | Meaning |
|---|---|---|---|
| `trigger_delay_p99` (scheduled vs actual fire time) | Trigger engine logs | > 5 s | Trigger engine overloaded or DB slow |
| `jobs_fired_per_minute` | Counter | Drop below baseline × 0.8 | Trigger engine may have crashed |
| `worker_queue_depth` per priority | Redis LLEN | > 500 for > 5 min | Workers can't keep up; scale up |
| `worker_utilization` | (active workers) / (total workers) | > 85% for > 5 min | Scale out workers |
| `job_failure_rate` per handler | Counter | > 5% for same handler over 1h | Bug in worker code; investigate |
| `dlq_depth` | DB count | > 0 | Jobs exhausted retries; alert immediately |
| `heartbeat_timeout_count` | Counter | > 5/hour | Workers crashing; infrastructure issue |
| `missed_fire_rate` | Counter | > 0/hour | DB downtime or trigger engine crash |
| `dag_stall_count` | DB count of WAITING_FOR_DEPS > 1h | > 0 | Upstream failure blocking downstream |
| `db_replication_lag_ms` | PostgreSQL metrics | > 1000 ms | Read replica stale; status reads may be stale |
| `retry_storm_rate` | (retries/sec) / (executions/sec) | > 0.5 (50% of jobs retrying) | Widespread failures; possible infrastructure issue |

**Distributed Tracing:**
- Inject `trace_id` into job payload on enqueue; worker propagates it in all downstream calls
- Trace spans: `trigger_engine.claim` → `queue.enqueue` → `worker.dequeue` → `worker.execute` → `status_manager.complete`
- Measure end-to-end from `scheduled_time` to `completed_at` per job type
- Alert on execution traces with `worker.execute` span > `timeout_seconds × 0.9`

**Logging:**
- Trigger Engine: log every batch fire with count, max delay, and any missed triggers
- Worker: log job start (with execution_id, fencing_token, handler) and completion (with duration, output summary)
- Status Manager: log all state transitions at INFO; log TIMED_OUT and DLQ transitions at WARN
- DAG Engine: log dependency unblocking events at DEBUG; DAG stalls at ERROR

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Alternative | Reason |
|---|---|---|---|
| Trigger claiming | PostgreSQL `FOR UPDATE SKIP LOCKED` | Leader election + single scheduler | Multi-node correctness without leader election complexity |
| Scheduling storage | PostgreSQL with JSONB payload | Cassandra / DynamoDB | ACID transactions needed for atomic trigger claiming |
| Job queue | Redis sorted set (priority queue) | Kafka per priority topic | Sub-ms enqueue; simpler priority; acceptable non-durable queue layer |
| Execution dedup | Fencing token + `WHERE status='QUEUED'` | Distributed lock per execution | Lower latency; fencing prevents zombie writes too |
| Retry backoff | Exponential + jitter | Fixed interval | Prevents thundering herd of simultaneous retries |
| DAG tracking | Dependency fulfillment counter table | Poll-based completion check | Event-driven; O(1) per completion vs O(N) poll |
| Worker timeout detection | Heartbeat staleness (60s) | Hard timeout enforced by scheduler | Works across network partitions; worker kills itself if cancel signal received |
| Execution history retention | 90 days partitioned PostgreSQL | Infinite S3 Parquet | Balance queryability and cost; old data archived to S3 |
| Missed fire recovery | `missed_fire_policy` per job | Global replay | Per-job semantics; some jobs must fire once, others can skip |
| Cron parsing | Server-side (croniter) | Client-side | Consistent; prevents client bugs |

---

## 11. Follow-up Interview Questions

Q1: How is this different from Airflow?  
A: Apache Airflow is a workflow orchestration platform with a full Python DAG definition language, rich UI, operators for every cloud service, and complex dependency expression. This design is a simpler, lower-overhead cron-style scheduler: jobs are registered via API with simple cron expressions, dependencies are expressed as job_id references, and handlers are pre-deployed workers. Airflow's overhead (Python DAG parsing, scheduler single-point bottleneck, complex state machine) is overkill for simple cron scheduling. For complex ML pipelines or data engineering workflows with branching logic, Airflow (or Prefect, Temporal) is more appropriate.

Q2: What is the difference between a job scheduler and a task queue (e.g., Celery)?  
A: A task queue (Celery, Sidekiq) executes tasks as quickly as possible — tasks are enqueued by application code and workers drain the queue immediately. There is no time-based scheduling in the task queue itself (though Celery Beat provides basic cron scheduling). A distributed job scheduler adds time-based triggering, cron expressions, dependency management, exactly-once firing guarantees, and centralized visibility. Task queues are pull-based and latency-focused; schedulers are time-based and reliability-focused.

Q3: How would you implement a job scheduler that spans multiple datacenters?  
A: Use a geo-distributed PostgreSQL (CockroachDB or AlloyDB Omni) as the scheduling database to avoid replication lag issues with trigger claiming. Run trigger engine nodes in each DC connected to the same distributed DB; `FOR UPDATE SKIP LOCKED` remains effective across DCs (serialized via distributed consensus). Workers in each DC pull from a local Redis queue; the trigger engine enqueues to the local DC's queue. For DC-affinity: add a `preferred_dc` column to job_definitions; trigger engine in DC-A skips jobs preferred for DC-B (processed by DC-B's trigger engine).

Q4: How do you handle a job that runs for longer than its scheduled interval (e.g., hourly job takes 90 minutes)?  
A: The `allow_concurrent_runs` flag controls this. If `false` (default for most jobs): new trigger fires but sees previous execution still RUNNING, marks new trigger as SKIPPED. Log and alert: "Job X: trigger at T+1h skipped because T execution still running." If the job consistently overruns, increase the interval or optimize the job. If `true`: multiple instances of the same job run in parallel — only safe if the job is idempotent and processes non-overlapping data sets.

Q5: How do you prevent a job from being scheduled more frequently than its minimum safe interval?  
A: Add a `min_interval_seconds` constraint on `job_definitions`. On trigger registration, the API validates that `next_fire_time - last_fired_at >= min_interval_seconds`. The trigger engine respects this: even if a missed-fire recovery would fire a job multiple times, it respects the minimum interval. For sub-minute cron expressions (e.g., `*/30 * * * * *` every 30 seconds), validate that `min_interval_seconds <= 30`.

Q6: How does the scheduler handle clock skew between the scheduler nodes and the database?  
A: All time comparisons use `NOW()` from the database (`SELECT ... WHERE scheduled_time <= NOW()`), not the scheduler node's clock. This eliminates clock skew between nodes as a source of duplicate firing — all comparisons are relative to the authoritative database clock. The only remaining skew risk: PostgreSQL primary vs replica clock (for read replicas). Use linearizable reads (primary only) for trigger queries; stale read-replica data could miss a trigger.

Q7: How would you implement job cancellation for a long-running job?  
A: Cancellation uses two mechanisms: (1) Heartbeat response: when a cancel is requested, the Status Manager sets `cancel_requested=true` in the execution record. The next heartbeat response to the worker includes `"cancel_requested": true`. The worker's heartbeat loop checks this and triggers graceful shutdown. (2) Hard kill: if the job doesn't stop within `cancel_grace_period_seconds`, the worker manager (Kubernetes) forcibly terminates the pod. The execution is marked `CANCELLED` regardless of the worker's response.

Q8: How do you handle a job that should only run on weekdays vs weekends?  
A: Standard cron syntax handles this: `0 9 * * 1-5` = 9 AM Monday through Friday. For more complex business calendars (holidays, custom business days), extend the job definition with a `business_calendar_id` reference. The trigger engine's `compute_next_fire` checks if the computed time falls within the business calendar; if not, advances to the next valid time. Store business calendars in the DB as a set of valid dates or exception dates.

Q9: What is the difference between a scheduled job and a cron job in this system?  
A: In this design: **cron job** = recurring trigger defined by a cron expression (fires indefinitely until paused/deleted). **Scheduled job (one-time)** = a job with a specific `scheduled_time`, fires exactly once. Internally, both use the `scheduled_triggers` table. A cron job generates new trigger rows (next occurrence) on each fire. A one-time job does not generate a new trigger row after firing. The distinction is in the `job_type` field and the `cron_expression` (null for one-time jobs).

Q10: How do you provide SLA guarantees for job execution?  
A: Define SLA tiers: tier 1 (priority 0-2) = fire within 1 second of scheduled time; tier 2 (priority 3-6) = within 5 seconds; tier 3 (priority 7-9) = within 30 seconds. Enforce via separate priority queues with dedicated worker capacity per tier (e.g., 200 workers reserved for priority 0-2 jobs). Monitor `trigger_delay` per priority tier. Alert when P99 exceeds SLA threshold. For contractual SLAs, record the `actual_fire_time - scheduled_time` in execution records and produce daily SLA compliance reports.

Q11: How would you implement idempotent job submission for one-time jobs submitted by application services?  
A: The `idempotency_key` field on job submission. If the caller submits a job with the same idempotency_key twice (e.g., due to a network retry), the second submission returns the existing job's ID with `HTTP 200` (not a new `201`). Implementation: `INSERT INTO job_definitions (idempotency_key, ...) ON CONFLICT (idempotency_key) DO NOTHING RETURNING job_id`. If DO NOTHING fires (conflict), a separate `SELECT WHERE idempotency_key = ?` fetches and returns the existing record. The caller should use a content-based idempotency key: `sha256(user_id + event_type + event_timestamp)`.

Q12: What is the difference between the `scheduled_time` and the `actual_start_time` of a job execution?  
A: `scheduled_time` is when the job was supposed to fire (the trigger time from the cron schedule). `actual_start_time` (or `started_at`) is when a worker actually started executing. The difference is: queuing delay + worker startup time. This is the "scheduling jitter" metric. For SLA enforcement, measure `started_at - scheduled_time`. Typical target: < 2 seconds for priority 1 jobs. High jitter indicates queue congestion or worker shortage.

Q13: How do you handle a scenario where you need to pause all jobs for a maintenance window?  
A: Two approaches: (1) **Pause all jobs globally:** Add a `global_pause` flag to a configuration table. The trigger engine checks this flag on each poll; if set, it skips all firing. Set a `maintenance_start` + `maintenance_end` timestamp; the trigger engine automatically resumes after `maintenance_end`. Jobs whose scheduled_time falls within the maintenance window are either SKIPPED or re-scheduled to fire immediately after maintenance ends (controlled by `missed_fire_policy`). (2) **Drain the queue:** Stop the trigger engine, let workers finish current jobs, then start the maintenance window. Resume by restarting the trigger engine. The `missed_fire_policy` handles recovery.

Q14: How would you implement rate limiting for job execution (e.g., max 10 executions/minute for a specific job)?  
A: Add `max_executions_per_minute` to job_definitions. The trigger engine checks the Redis rate limiter before enqueueing: `INCR rate:job:{job_id}:{minute_bucket}; EXPIRE rate:job:{job_id}:{minute_bucket} 60`. If count > max, mark trigger as DEFERRED (re-enqueue 60-second delay). Alternatively, use a token bucket: each job has a bucket with `max_executions_per_minute` tokens refilled per minute. Trigger engine atomically decrements the bucket before firing; if empty, defers. This is useful for jobs that call rate-limited external APIs.

Q15: How does this scheduler compare to cloud-native schedulers like AWS EventBridge Scheduler or Google Cloud Scheduler?  
A: Cloud-native schedulers (EventBridge Scheduler, GCP Cloud Scheduler) handle cron scheduling and HTTP/Lambda/SQS invocations with ≥1 million schedules, < 1-second precision, managed HA, and built-in retry. They are the right choice for event-driven serverless architectures. This custom distributed scheduler adds: arbitrary DAG dependencies, complex retry logic with backoff, job-type-specific priority queues, worker heartbeat/timeout tracking, multi-payload job variants, and full execution history queryable via API. Build custom when the off-the-shelf scheduler lacks DAG dependencies, worker lifecycle management, or custom priority semantics.

---

## 12. References & Further Reading

- **Quartz Scheduler — Java job scheduling framework:** https://www.quartz-scheduler.org/documentation/
- **Airflow Architecture Documentation:** https://airflow.apache.org/docs/apache-airflow/stable/concepts/overview.html
- **Temporal — Durable workflow execution:** https://docs.temporal.io/temporal
- **Uber Cadence — Workflow as Code:** https://cadenceworkflow.io/docs/concepts/
- **PostgreSQL — FOR UPDATE SKIP LOCKED:** https://www.postgresql.org/docs/current/sql-select.html#SQL-FOR-UPDATE-SHARE
- **KEDA — Kubernetes Event-Driven Autoscaling:** https://keda.sh/docs/
- **Martin Fowler — "Competing Consumers Pattern":** https://www.enterpriseintegrationpatterns.com/patterns/messaging/CompetingConsumers.html
- **AWS EventBridge Scheduler Documentation:** https://docs.aws.amazon.com/scheduler/latest/UserGuide/what-is-scheduler.html
- **GCP Cloud Scheduler Documentation:** https://cloud.google.com/scheduler/docs/overview
- **Croniter Python Library (cron expression parsing):** https://github.com/kiorky/croniter
- **Designing Data-Intensive Applications — Kleppmann, Chapter 11 (Stream Processing):** O'Reilly, 2017
- **LinkedIn Azkaban — Workflow scheduler:** https://azkaban.github.io/
- **Apache Oozie — Hadoop workflow scheduler:** https://oozie.apache.org/docs/
- **Sidekiq — Background job processing:** https://sidekiq.org/
- **Cloudflare Blog — Scaling Distributed Cron at Cloudflare:** https://blog.cloudflare.com/introducing-cron-triggers-for-cloudflare-workers/
