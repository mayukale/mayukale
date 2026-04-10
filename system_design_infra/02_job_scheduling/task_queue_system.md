# System Design: Task Queue System

> **Relevance to role:** Task queues underpin every asynchronous workload on a cloud infrastructure platform -- from bare-metal provisioning workflows to Kubernetes pod lifecycle events to Elasticsearch re-indexing jobs. Designing and operating a production-grade task queue at scale (like Celery, Sidekiq, or a Java equivalent) is a core competency for this role.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement | Detail |
|----|------------|--------|
| FR-1 | Task submission | Producers submit tasks (function + arguments) to named queues via API or SDK |
| FR-2 | Reliable delivery | Tasks are delivered to exactly one worker; acknowledged on completion |
| FR-3 | Task routing | Route tasks to specific worker pools by queue name, labels, or task type |
| FR-4 | Priority queues | Support multiple priority levels within a queue |
| FR-5 | Delay/scheduled tasks | Tasks can specify a future execution time (ETA) |
| FR-6 | Task workflows | Support chains (sequential), groups (parallel), chords (parallel + callback) |
| FR-7 | Result storage | Optional result backend for task return values with configurable TTL |
| FR-8 | Rate limiting | Per-task-type and per-tenant rate limiting |
| FR-9 | Retry with backoff | Configurable retry policy per task type |
| FR-10 | Dead letter queue | Permanently failed tasks routed to DLQ for manual inspection |
| FR-11 | Task cancellation | Cancel pending or running tasks |
| FR-12 | Multi-tenancy | Queue isolation per tenant with quota enforcement |

### Non-Functional Requirements
| NFR | Target | Rationale |
|-----|--------|-----------|
| Availability | 99.99% for message broker and control plane | Task loss is unacceptable for infrastructure operations |
| Delivery guarantee | At-least-once (default); at-most-once configurable | Infra tasks (provisioning, DNS updates) must not be lost |
| Enqueue latency | < 10ms P99 | Producers must not be blocked waiting for task submission |
| Dequeue-to-start latency | < 100ms P99 (non-delayed tasks) | Workers should pick up tasks almost immediately |
| Task throughput | 500K tasks/min enqueued, 200K tasks/min completed | Enterprise-scale platform with burst capacity |
| Result retrieval | < 50ms P99 | Synchronous callers waiting for results |
| Durability | Zero task loss after acknowledgement | Enqueued tasks survive broker restart |
| Scalability | 50K concurrent workers across all pools | Large heterogeneous worker fleet |

### Constraints & Assumptions
- Broker: RabbitMQ primary (existing deployment); Redis as lightweight alternative for non-critical queues
- Workers: Java (Spring Boot) and Python (Celery-compatible) runtimes
- MySQL for task metadata and audit; Redis for result backend
- Kubernetes deployment for worker pods; bare-metal for broker cluster
- Internal network: < 1ms RTT between producers, broker, and workers within a datacenter
- Tasks are typically short-lived (median 5s, P99 5 min, max 1 hour)

### Out of Scope
- Stream processing (Kafka Streams, Flink) -- this is a task/job queue, not a data pipeline
- Long-running batch jobs (> 1 hour) -- handled by the distributed job scheduler
- Multi-region task distribution
- Task queue as a managed service (PaaS offering)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Producer services | 500 | Microservices, CI/CD pipelines, operators |
| Distinct task types | 5K | 500 services x 10 task types/service avg |
| Tasks enqueued/day | 200M | Avg ~2.3K/sec, peak ~8.3K/sec (500K/min for 10 min bursts) |
| Tasks completed/day | 200M | Steady-state: queue depth stable |
| Concurrent workers | 50K | Across all pools and tenants |
| Active queues | 2K | Named queues (tenant x task-type granularity) |
| Peak queue depth (all queues) | 5M | During burst periods before workers scale up |
| Result storage | 100M results/day | 50% of tasks store results |

### Latency Requirements

| Operation | P50 | P99 | P99.9 |
|-----------|-----|-----|-------|
| Task enqueue | 2ms | 10ms | 50ms |
| Task dequeue (broker to worker) | 5ms | 50ms | 200ms |
| Task ACK (worker to broker) | 1ms | 5ms | 20ms |
| Result store | 2ms | 10ms | 50ms |
| Result fetch | 1ms | 5ms | 20ms |
| Task cancel (pending) | 10ms | 50ms | 200ms |
| Task cancel (running) | 100ms | 500ms | 2s |

### Storage Estimates

| Data | Size per record | Count/day | Daily volume | Retention |
|------|----------------|-----------|-------------|-----------|
| Task message (in broker) | 2 KB avg | 200M | 400 GB (transient) | Until ACK'd |
| Task metadata (MySQL) | 500 B | 200M | 100 GB | 30 days = 3 TB |
| Task results (Redis) | 1 KB avg | 100M | 100 GB | 24h TTL default |
| DLQ entries | 5 KB (includes error) | 200K (0.1% failure) | 1 GB | 90 days |
| Audit log | 200 B | 200M | 40 GB | 90 days |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Enqueue (producer → broker) | 8.3K/sec peak x 2 KB = 16.6 MB/s | 17 MB/s |
| Dequeue (broker → worker) | 8.3K/sec x 2 KB = 16.6 MB/s | 17 MB/s |
| ACKs (worker → broker) | 8.3K/sec x 100 B = 0.83 MB/s | 1 MB/s |
| Result writes | 4.2K/sec x 1 KB = 4.2 MB/s | 4 MB/s |
| Result reads | 4.2K/sec x 1 KB = 4.2 MB/s | 4 MB/s |
| **Total** | | **~43 MB/s** |

---

## 3. High Level Architecture

```
   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
   │  Producer A  │  │  Producer B  │  │  Producer C  │
   │ (Java/Spring)│  │  (Python)    │  │  (Go CLI)    │
   └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
          │                 │                  │
          └────────────┬────┴──────────────────┘
                       │
                ┌──────▼───────┐
                │  Task Queue  │
                │  API Gateway │
                │ (Rate Limit, │
                │  Auth, Route)│
                └──────┬───────┘
                       │
          ┌────────────┼────────────┐
          │            │            │
    ┌─────▼─────┐┌────▼─────┐┌────▼─────┐
    │ RabbitMQ  ││ RabbitMQ ││ RabbitMQ │
    │  Node 1   ││  Node 2  ││  Node 3  │
    │ (Quorum Q)││(Quorum Q)││(Quorum Q)│
    └─────┬─────┘└────┬─────┘└────┬─────┘
          │           │           │
          └─────┬─────┘           │
                │                 │
     ┌──────────┼─────────────────┘
     │          │
     │   ┌──────▼───────┐
     │   │  Exchange     │
     │   │  Router       │
     │   │ (topic/direct)│
     │   └──────┬────────┘
     │          │
     │   ┌──────┼────────────────────────┐
     │   │      │                        │
     │┌──▼───┐┌─▼──────┐ ┌──────┐ ┌─────▼────┐
     ││Queue ││Queue   │ │Queue │ │  Delay   │
     ││high  ││default │ │bulk  │ │  Queue   │
     ││prio  ││        │ │      │ │ (TTL+DLX)│
     │└──┬───┘└──┬─────┘ └──┬───┘ └─────┬────┘
     │   │       │          │            │
     │   └───┬───┘          │      (message expires)
     │       │              │            │
     │  ┌────▼──────────────▼────────────▼──┐
     │  │        Worker Pools               │
     │  │                                   │
     │  │  ┌────────┐ ┌────────┐ ┌────────┐│
     │  │  │Worker  │ │Worker  │ │Worker  ││
     │  │  │Pool A  │ │Pool B  │ │Pool C  ││
     │  │  │(Prefork│ │(Async) │ │(GPU)   ││
     │  │  │ 8 proc)│ │100 coro│ │4 proc) ││
     │  │  └───┬────┘ └───┬────┘ └───┬────┘│
     │  └──────┼──────────┼──────────┼─────┘
     │         │          │          │
     │         └────┬─────┘          │
     │              │                │
     │  ┌───────────▼────────────────▼──┐
     │  │       Result Backend          │
     │  │  ┌────────┐  ┌────────────┐   │
     │  │  │ Redis  │  │  MySQL     │   │
     │  │  │(hot/TTL│  │(persistent │   │
     │  │  │results)│  │ results)   │   │
     │  │  └────────┘  └────────────┘   │
     │  └───────────────────────────────┘
     │
     │  ┌────────────────────────────────┐
     └──►  Monitoring & Management       │
        │  ┌─────────┐ ┌──────────────┐  │
        │  │ Metrics  │ │ Elasticsearch│  │
        │  │(Prometheus│ │ (Task search)│  │
        │  └─────────┘ └──────────────┘  │
        └────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Producer** | Submits task messages to the broker via AMQP client or REST API |
| **API Gateway** | Authenticates, rate-limits, and routes task submissions; provides REST interface for non-AMQP producers |
| **RabbitMQ Cluster (3 nodes)** | Durable message broker; quorum queues for consistency; handles routing, persistence, delivery |
| **Exchange Router** | RabbitMQ exchanges route messages to queues based on routing key (task type, priority, tenant) |
| **Priority Queues** | Separate queues per priority level (high/default/bulk) consumed with weighted round-robin |
| **Delay Queue** | Messages with TTL; on expiry, dead-letter exchange routes to target queue (delayed execution pattern) |
| **Worker Pools** | Groups of worker processes consuming from specific queues; different concurrency models per pool |
| **Result Backend (Redis)** | Stores task return values with TTL for callers awaiting results |
| **Result Backend (MySQL)** | Persistent result storage for audit and long-term access |
| **Elasticsearch** | Task execution search, analytics, dashboards |

### Data Flows

**Primary: Task Submission and Execution**
1. Producer serializes task (function name + args) and publishes to RabbitMQ exchange
2. Exchange routes message to appropriate queue based on routing key
3. Worker consumes message, deserializes, executes the task function
4. On success: worker ACKs the message (broker removes from queue), stores result in Redis
5. On failure: worker NACKs with requeue (for retry) or sends to DLQ (permanent failure)

**Secondary: Delayed Task**
1. Producer publishes to delay exchange with `x-message-ttl` header
2. Message sits in delay queue until TTL expires
3. On expiry, RabbitMQ dead-letter exchange routes to the target execution queue
4. Worker picks up and executes as normal

**Tertiary: Chord (parallel + callback)**
1. Producer submits group of N tasks + callback task
2. N tasks execute in parallel across workers
3. Each task completion decrements a Redis counter (chord_unlock key)
4. When counter reaches 0, the callback task is enqueued with collected results

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Task type registry (for rate limiting, routing, configuration)
CREATE TABLE task_type (
    task_type_id        INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    task_name           VARCHAR(255)    NOT NULL UNIQUE COMMENT 'e.g., infra.provision.bare_metal',
    queue_name          VARCHAR(128)    NOT NULL DEFAULT 'default',
    priority            ENUM('HIGH','DEFAULT','BULK') DEFAULT 'DEFAULT',
    max_retries         INT UNSIGNED    DEFAULT 3,
    retry_backoff_ms    INT UNSIGNED    DEFAULT 1000,
    timeout_seconds     INT UNSIGNED    DEFAULT 300,
    rate_limit          VARCHAR(64)     NULL COMMENT 'e.g., 100/m, 1000/h',
    result_ttl_seconds  INT UNSIGNED    DEFAULT 86400 COMMENT '24h default',
    serialization       ENUM('JSON','PROTOBUF','MSGPACK') DEFAULT 'JSON',
    enabled             BOOLEAN         DEFAULT TRUE,
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    updated_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    INDEX idx_queue (queue_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Task instance (one per submission)
CREATE TABLE task_instance (
    task_id             CHAR(36)        PRIMARY KEY COMMENT 'UUID v7 (time-ordered)',
    task_type_id        INT UNSIGNED    NOT NULL,
    tenant_id           VARCHAR(64)     NOT NULL,
    status              ENUM('PENDING','QUEUED','ACTIVE','SUCCEEDED','FAILED',
                             'RETRYING','CANCELLED','DEAD_LETTERED') NOT NULL DEFAULT 'PENDING',
    priority            TINYINT UNSIGNED DEFAULT 5 COMMENT '0=highest, 9=lowest',
    args_ref            VARCHAR(512)    NULL COMMENT 'Object store ref for large payloads',
    args_hash           CHAR(64)        NULL COMMENT 'SHA-256 for dedup',
    eta                 TIMESTAMP(6)    NULL COMMENT 'Earliest execution time',
    countdown_seconds   INT UNSIGNED    NULL COMMENT 'Delay from submission',
    attempt_number      INT UNSIGNED    DEFAULT 0,
    worker_id           VARCHAR(128)    NULL,
    parent_task_id      CHAR(36)        NULL COMMENT 'For chain/chord parent',
    group_id            CHAR(36)        NULL COMMENT 'For group/chord membership',
    chord_callback_id   CHAR(36)        NULL COMMENT 'Task to invoke when group completes',
    enqueued_at         TIMESTAMP(6)    NULL,
    started_at          TIMESTAMP(6)    NULL,
    completed_at        TIMESTAMP(6)    NULL,
    error_message       TEXT            NULL,
    result_ref          VARCHAR(512)    NULL COMMENT 'Redis key or object store ref',
    runtime_ms          INT UNSIGNED    NULL,
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_tenant_status (tenant_id, status),
    INDEX idx_type_status (task_type_id, status),
    INDEX idx_group (group_id),
    INDEX idx_parent (parent_task_id),
    INDEX idx_status_created (status, created_at),
    INDEX idx_eta (eta) COMMENT 'For delayed task polling',
    FOREIGN KEY (task_type_id) REFERENCES task_type(task_type_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (TO_DAYS(created_at)) (
    PARTITION p_2026_04 VALUES LESS THAN (TO_DAYS('2026-05-01')),
    PARTITION p_2026_05 VALUES LESS THAN (TO_DAYS('2026-06-01')),
    PARTITION p_2026_06 VALUES LESS THAN (TO_DAYS('2026-07-01')),
    PARTITION p_future  VALUES LESS THAN MAXVALUE
);

-- Dead letter queue
CREATE TABLE dead_letter_task (
    dlq_id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    task_id             CHAR(36)        NOT NULL,
    task_type_id        INT UNSIGNED    NOT NULL,
    tenant_id           VARCHAR(64)     NOT NULL,
    total_attempts      INT UNSIGNED    NOT NULL,
    last_error          TEXT            NOT NULL,
    last_traceback      TEXT            NULL,
    original_args       MEDIUMTEXT      NULL COMMENT 'Serialized original arguments',
    dead_lettered_at    TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    resolved            BOOLEAN         DEFAULT FALSE,
    resolved_at         TIMESTAMP(6)    NULL,
    resolved_by         VARCHAR(128)    NULL,
    resolution          ENUM('RETRIED','DISCARDED','MANUALLY_FIXED') NULL,
    INDEX idx_unresolved (resolved, dead_lettered_at),
    INDEX idx_tenant (tenant_id, resolved),
    INDEX idx_type (task_type_id, resolved),
    FOREIGN KEY (task_id) REFERENCES task_instance(task_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Rate limiting state (can also be in Redis)
CREATE TABLE rate_limit_bucket (
    bucket_key          VARCHAR(255)    PRIMARY KEY COMMENT 'task_type:tenant_id',
    tokens              INT UNSIGNED    NOT NULL,
    max_tokens          INT UNSIGNED    NOT NULL,
    refill_rate         INT UNSIGNED    NOT NULL COMMENT 'tokens/second',
    last_refill         TIMESTAMP(6)    NOT NULL,
    INDEX idx_refill (last_refill)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Worker registry
CREATE TABLE worker_instance (
    worker_id           VARCHAR(128)    PRIMARY KEY,
    pool_name           VARCHAR(64)     NOT NULL,
    hostname            VARCHAR(255)    NOT NULL,
    pid                 INT UNSIGNED    NOT NULL,
    concurrency         INT UNSIGNED    NOT NULL COMMENT 'Max concurrent tasks',
    concurrency_model   ENUM('PREFORK','THREADING','ASYNC') NOT NULL,
    active_tasks        INT UNSIGNED    DEFAULT 0,
    queues_consumed     JSON            NOT NULL COMMENT '["default","high"]',
    status              ENUM('ACTIVE','DRAINING','OFFLINE') DEFAULT 'ACTIVE',
    last_heartbeat      TIMESTAMP(6)    NOT NULL,
    registered_at       TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_pool_status (pool_name, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Workflow: group tracking
CREATE TABLE task_group (
    group_id            CHAR(36)        PRIMARY KEY,
    tenant_id           VARCHAR(64)     NOT NULL,
    total_tasks         INT UNSIGNED    NOT NULL,
    completed_tasks     INT UNSIGNED    DEFAULT 0,
    failed_tasks        INT UNSIGNED    DEFAULT 0,
    chord_callback_id   CHAR(36)        NULL,
    status              ENUM('RUNNING','SUCCEEDED','FAILED','CANCELLED') DEFAULT 'RUNNING',
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    completed_at        TIMESTAMP(6)    NULL,
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Criterion | RabbitMQ (Broker) | Redis (Results) | MySQL (Metadata) | Kafka (Alternative Broker) |
|-----------|------------------|----------------|-----------------|--------------------------|
| Delivery guarantee | At-least-once (quorum queues) | At-most-once (no persistence in default) | N/A | At-least-once (offsets) |
| Ordering | Per-queue FIFO | N/A | N/A | Per-partition FIFO |
| Priority support | Native priority queues | Sorted sets | N/A | No (requires workaround) |
| Delayed messages | Via TTL + DLX | Via sorted set (ZRANGEBYSCORE) | N/A | No native support |
| Throughput | ~50K msg/s per node | ~100K ops/s | ~20K writes/s | ~1M msg/s per broker |
| Durability | Quorum queues (Raft) | AOF with fsync | InnoDB WAL | Replicated log |
| Operational cost | Medium (cluster management) | Low | Low (existing) | Medium |

**Selection:**
- **Broker: RabbitMQ 3.13+ with quorum queues** -- Native priority support, dead-letter exchanges, per-message TTL for delays, mature AMQP ecosystem. Quorum queues (Raft-based) provide durability and consistency. Already deployed in the organization.
- **Result Backend: Redis 7+** -- Sub-millisecond result retrieval, native TTL for automatic cleanup, pub/sub for result notification. Results are ephemeral (24h default TTL), so Redis's in-memory model is appropriate.
- **Metadata Store: MySQL 8.0** -- Durable task audit trail, searchable history, quota tracking. Existing organizational standard.
- **Why not Kafka:** Kafka excels at high-throughput streams but lacks native priority queues, per-message TTL, and individual message acknowledgement. It requires consumer-side complexity for task queue semantics.

### Indexing Strategy

| Index | Table | Purpose | Notes |
|-------|-------|---------|-------|
| `idx_tenant_status` | task_instance | Per-tenant task listing by status | Covers quota enforcement queries |
| `idx_type_status` | task_instance | Per-type monitoring and rate limiting | |
| `idx_status_created` | task_instance | Global task listing, dashboard queries | Partition-pruned by created_at |
| `idx_eta` | task_instance | Delay queue polling (find tasks where eta <= now) | Sparse: only delayed tasks have non-NULL eta |
| `idx_group` | task_instance | Chord/group completion tracking | |
| `idx_unresolved` | dead_letter_task | DLQ dashboard | |

---

## 5. API Design

### REST API Endpoints

#### Task Submission

```
POST /api/v1/tasks
Authorization: Bearer <JWT>
X-Tenant-Id: team-platform
X-Idempotency-Key: <uuid>
Content-Type: application/json
Rate-Limit: 10000 req/min per tenant

Request:
{
  "task_name": "infra.provision.bare_metal",
  "args": {
    "server_id": "srv-12345",
    "datacenter": "us-east-1",
    "config": {"cpu": 64, "memory_gb": 256, "disk_tb": 4}
  },
  "kwargs": {},
  "priority": 3,
  "eta": null,
  "countdown_seconds": null,
  "queue": "provisioning",
  "headers": {
    "trace_id": "abc123",
    "source_service": "control-plane"
  }
}

Response (202 Accepted):
{
  "task_id": "01903f5b-a7c2-7def-9123-456789abcdef",
  "status": "QUEUED",
  "queue": "provisioning",
  "enqueued_at": "2026-04-09T14:30:00.123456Z",
  "_links": {
    "self": "/api/v1/tasks/01903f5b-a7c2-7def-9123-456789abcdef",
    "result": "/api/v1/tasks/01903f5b-a7c2-7def-9123-456789abcdef/result",
    "cancel": "/api/v1/tasks/01903f5b-a7c2-7def-9123-456789abcdef/cancel"
  }
}
```

#### Task Workflows

```
POST /api/v1/workflows/chain
Request:
{
  "tasks": [
    {"task_name": "infra.provision.bare_metal", "args": {"server_id": "srv-1"}},
    {"task_name": "infra.configure.network", "args": {"server_id": "srv-1"}},
    {"task_name": "infra.configure.monitoring", "args": {"server_id": "srv-1"}}
  ]
}

Response (202):
{
  "chain_id": "chain-uuid-123",
  "task_ids": ["task-1", "task-2", "task-3"],
  "status": "RUNNING"
}
```

```
POST /api/v1/workflows/chord
Request:
{
  "group": [
    {"task_name": "infra.health_check", "args": {"server_id": "srv-1"}},
    {"task_name": "infra.health_check", "args": {"server_id": "srv-2"}},
    {"task_name": "infra.health_check", "args": {"server_id": "srv-3"}}
  ],
  "callback": {
    "task_name": "infra.aggregate_health_report",
    "args": {}
  }
}

Response (202):
{
  "chord_id": "chord-uuid-456",
  "group_id": "group-uuid-789",
  "group_task_ids": ["task-a", "task-b", "task-c"],
  "callback_task_id": "task-callback",
  "status": "RUNNING"
}
```

#### Task Status & Results

```
GET /api/v1/tasks/{task_id}

Response:
{
  "task_id": "01903f5b-a7c2-7def-9123-456789abcdef",
  "task_name": "infra.provision.bare_metal",
  "status": "SUCCEEDED",
  "attempt_number": 1,
  "enqueued_at": "2026-04-09T14:30:00.123456Z",
  "started_at": "2026-04-09T14:30:00.200000Z",
  "completed_at": "2026-04-09T14:30:45.678000Z",
  "runtime_ms": 45478,
  "worker_id": "worker-pool-prov-node-03-pid-1234"
}

GET /api/v1/tasks/{task_id}/result

Response:
{
  "task_id": "01903f5b-a7c2-7def-9123-456789abcdef",
  "status": "SUCCEEDED",
  "result": {
    "server_id": "srv-12345",
    "ip_address": "10.0.5.42",
    "provisioned_at": "2026-04-09T14:30:45Z"
  },
  "result_expires_at": "2026-04-10T14:30:45Z"
}
```

#### Task Management

```
POST /api/v1/tasks/{task_id}/cancel
POST /api/v1/tasks/{task_id}/retry              # Re-enqueue a failed task
GET  /api/v1/tasks?tenant_id=X&status=FAILED&task_name=infra.*&limit=50
GET  /api/v1/queues                              # List all queues with depth
GET  /api/v1/queues/{queue_name}/stats           # Queue statistics
POST /api/v1/queues/{queue_name}/purge           # Emergency purge

GET  /api/v1/dlq?tenant_id=X&resolved=false&limit=50
POST /api/v1/dlq/{dlq_id}/retry
POST /api/v1/dlq/{dlq_id}/resolve               # Mark as manually resolved
```

#### gRPC: High-Throughput Internal Interface

```protobuf
service TaskQueueService {
  // High-throughput task submission
  rpc SubmitTask(SubmitTaskRequest) returns (SubmitTaskResponse);
  
  // Batch submission
  rpc SubmitBatch(SubmitBatchRequest) returns (SubmitBatchResponse);
  
  // Result streaming
  rpc WaitForResult(WaitForResultRequest) returns (stream TaskResult);
  
  // Queue stats streaming
  rpc StreamQueueStats(StreamQueueStatsRequest) returns (stream QueueStats);
}

message SubmitTaskRequest {
  string task_name = 1;
  bytes args_serialized = 2;
  int32 priority = 3;                // 0-9
  int64 eta_epoch_ms = 4;            // 0 = immediate
  string queue = 5;
  string idempotency_key = 6;
  map<string, string> headers = 7;
}
```

### CLI Design

```bash
# Task submission
taskq submit infra.provision.bare_metal \
  --args '{"server_id":"srv-1"}' \
  --queue provisioning \
  --priority 3 \
  --countdown 60              # Delay 60 seconds

taskq submit-batch -f tasks.jsonl     # Bulk submit from file

# Task status
taskq status <task_id>
taskq result <task_id> [--wait] [--timeout 120]
taskq cancel <task_id>
taskq retry <task_id>

# Workflows
taskq chain \
  "infra.provision.bare_metal:srv-1" \
  "infra.configure.network:srv-1" \
  "infra.configure.monitoring:srv-1"

taskq chord \
  --group "infra.health_check:srv-1" "infra.health_check:srv-2" \
  --callback "infra.aggregate_report"

# Queue management
taskq queues list                     # List all queues with depth
taskq queues stats provisioning       # Detailed queue stats
taskq queues purge provisioning [--confirm]  # Dangerous: purge all messages

# Worker management
taskq workers list [--pool provisioning]
taskq workers inspect <worker_id>     # Active tasks, resource usage
taskq workers drain <worker_id>       # Finish current, no new tasks

# Dead letter queue
taskq dlq list [--tenant team-platform] [--limit 20]
taskq dlq inspect <dlq_id>           # Full details + traceback
taskq dlq retry <dlq_id>
taskq dlq retry-all --task-name "infra.provision.*"  # Batch retry
taskq dlq resolve <dlq_id> --reason "underlying issue fixed"

# Rate limiting
taskq ratelimit show infra.provision.bare_metal
taskq ratelimit set infra.provision.bare_metal --rate "100/m"

# Monitoring
taskq stats                           # Global throughput, latency, failure rate
taskq top                             # Real-time task throughput (like htop)

# Example output:
# taskq queues list
# QUEUE          CONSUMERS  DEPTH   ENQUEUE/s  DEQUEUE/s  ACK/s  UNACK
# provisioning   24         142     45         42         41     3
# default        100        2,891   1,200      1,150      1,140  10
# bulk           50         45,231  500        480        475    5
# high           20         12      200        200        200    0
```

---

## 6. Core Component Deep Dives

### 6.1 Message Broker Architecture & Reliability

**Why it's hard:** The broker must guarantee message delivery under node failures, network partitions, and high load. It must support multiple queue types (FIFO, priority, delay), handle backpressure, and scale to 500K messages/min without message loss.

**Approaches Compared:**

| Approach | Durability | Ordering | Priority | Throughput | Operational Complexity |
|----------|-----------|----------|----------|-----------|----------------------|
| RabbitMQ classic queues | Mirrored (async) | FIFO | Native | ~30K/s | Low |
| RabbitMQ quorum queues | Raft (strong) | FIFO | Via separate queues | ~20K/s | Low |
| Redis Streams + XREADGROUP | AOF (configurable) | FIFO | Manual | ~100K/s | Low |
| Kafka topics | Replicated log | Per-partition | No native | ~500K/s | Medium |
| Amazon SQS-style (custom) | Replicated | Best-effort | Native | Variable | Very High |

**Selected: RabbitMQ 3.13+ quorum queues** -- Quorum queues use Raft consensus for replication: a message is confirmed to the producer only after a majority of nodes have persisted it. This guarantees zero message loss even if a minority of nodes fail. Combined with publisher confirms and consumer manual ACKs, we get at-least-once delivery guarantee.

**Implementation: Publisher with Confirms**

```java
@Component
public class TaskPublisher {
    
    private final RabbitTemplate rabbitTemplate;
    private final TaskInstanceRepository taskRepo;
    private final MeterRegistry metrics;
    
    @Autowired
    public TaskPublisher(ConnectionFactory connectionFactory,
                         TaskInstanceRepository taskRepo,
                         MeterRegistry metrics) {
        this.rabbitTemplate = new RabbitTemplate(connectionFactory);
        this.taskRepo = taskRepo;
        this.metrics = metrics;
        
        // Enable publisher confirms (async)
        this.rabbitTemplate.setConfirmCallback((correlationData, ack, cause) -> {
            if (ack) {
                metrics.counter("task.publish.confirmed").increment();
            } else {
                metrics.counter("task.publish.nacked").increment();
                // Retry publication
                if (correlationData != null) {
                    retryPublish(correlationData.getId());
                }
            }
        });
        
        // Enable mandatory flag + return callback for unroutable messages
        this.rabbitTemplate.setReturnsCallback(returned -> {
            log.error("Message returned (unroutable): exchange={}, routingKey={}, replyCode={}",
                returned.getExchange(), returned.getRoutingKey(), returned.getReplyCode());
            metrics.counter("task.publish.returned").increment();
        });
    }
    
    public String publishTask(TaskSubmission submission) {
        String taskId = UuidCreator.getTimeOrderedEpoch().toString(); // UUID v7
        
        // 1. Persist task metadata to MySQL (before publishing)
        TaskInstance task = new TaskInstance();
        task.setTaskId(taskId);
        task.setTaskTypeId(submission.getTaskTypeId());
        task.setTenantId(submission.getTenantId());
        task.setStatus(TaskStatus.PENDING);
        task.setPriority(submission.getPriority());
        task.setEnqueuedAt(Instant.now());
        taskRepo.save(task);
        
        // 2. Serialize task message
        TaskMessage message = TaskMessage.builder()
            .taskId(taskId)
            .taskName(submission.getTaskName())
            .args(submission.getArgs())
            .headers(submission.getHeaders())
            .attemptNumber(1)
            .build();
        
        byte[] body = serializer.serialize(message, submission.getSerialization());
        
        // 3. Publish to RabbitMQ with confirm
        MessageProperties props = new MessageProperties();
        props.setDeliveryMode(MessageDeliveryMode.PERSISTENT);
        props.setPriority(submission.getPriority());
        props.setMessageId(taskId);
        props.setContentType(submission.getSerialization().getContentType());
        
        if (submission.getEta() != null) {
            long delayMs = Duration.between(Instant.now(), submission.getEta()).toMillis();
            if (delayMs > 0) {
                props.setExpiration(String.valueOf(delayMs));
                // Publish to delay exchange
                rabbitTemplate.send("task.delay", submission.getQueue(),
                    new Message(body, props), new CorrelationData(taskId));
                
                // Update status
                taskRepo.updateStatus(taskId, TaskStatus.QUEUED);
                metrics.counter("task.published.delayed").increment();
                return taskId;
            }
        }
        
        // Immediate: publish to main exchange
        rabbitTemplate.send("task.direct", submission.getQueue(),
            new Message(body, props), new CorrelationData(taskId));
        
        taskRepo.updateStatus(taskId, TaskStatus.QUEUED);
        metrics.counter("task.published").increment();
        metrics.timer("task.publish.latency").record(
            Duration.between(task.getCreatedAt(), Instant.now()));
        
        return taskId;
    }
}
```

**RabbitMQ Queue Configuration:**

```java
@Configuration
public class RabbitMQConfig {
    
    @Bean
    public Queue highPriorityQueue() {
        return QueueBuilder.durable("task.high")
            .quorum()                              // Quorum queue (Raft replication)
            .deliveryLimit(5)                      // Max redeliveries before DLQ
            .deadLetterExchange("task.dlx")
            .deadLetterRoutingKey("task.dead")
            .overflow("reject-publish")            // Backpressure: reject when full
            .maxLength(1_000_000L)                 // 1M messages max
            .build();
    }
    
    @Bean
    public Queue defaultQueue() {
        return QueueBuilder.durable("task.default")
            .quorum()
            .deliveryLimit(5)
            .deadLetterExchange("task.dlx")
            .deadLetterRoutingKey("task.dead")
            .maxLength(10_000_000L)                // 10M messages max
            .build();
    }
    
    @Bean
    public Queue delayQueue() {
        return QueueBuilder.durable("task.delay")
            .quorum()
            .deadLetterExchange("task.direct")     // On TTL expiry → direct exchange
            .build();
    }
    
    @Bean
    public Queue deadLetterQueue() {
        return QueueBuilder.durable("task.dead")
            .quorum()
            .build();
    }
    
    @Bean
    public DirectExchange directExchange() {
        return new DirectExchange("task.direct", true, false);
    }
    
    @Bean
    public DirectExchange delayExchange() {
        return new DirectExchange("task.delay", true, false);
    }
    
    @Bean
    public DirectExchange dlxExchange() {
        return new DirectExchange("task.dlx", true, false);
    }
}
```

**Failure Modes:**
- **Broker node crash:** Quorum queue leader fails over to another node. Messages replicated to majority are safe. In-flight unACKed messages are redelivered to other consumers.
- **Publisher disconnects before confirm:** Message may or may not be persisted. Publisher retries with idempotency key; broker deduplicates (or consumer deduplicates via task_id).
- **Consumer crashes mid-task:** Message is not ACKed; broker redelivers to another consumer after `consumer_timeout` (default 30 min in RabbitMQ 3.12+).
- **Network partition between broker nodes:** Quorum queues require majority availability. If only 1 of 3 nodes is reachable, the queue is unavailable for writes. This is the correct behavior -- prevents split-brain at the cost of availability.

**Interviewer Q&A:**

**Q1: Why quorum queues instead of classic mirrored queues?**
A: Classic mirrored queues use asynchronous replication -- a confirmed message can be lost if the primary fails before replication. Quorum queues use Raft consensus: a message is confirmed only after majority replication. This provides stronger durability guarantees. Additionally, RabbitMQ 3.13 deprecated classic mirrored queues in favor of quorum queues.

**Q2: How do you handle the "dual write" problem (MySQL + RabbitMQ)?**
A: We write to MySQL first (task status = PENDING), then publish to RabbitMQ. If RabbitMQ publish fails, the task stays PENDING in MySQL. A recovery process scans for PENDING tasks older than 30 seconds and retries the publish. If MySQL write fails, we don't publish at all. The downside: brief window where task is in MySQL but not yet in RabbitMQ. Alternative: transactional outbox pattern (write to MySQL, a separate process reads the outbox and publishes to RabbitMQ).

**Q3: What is the message throughput ceiling for a 3-node RabbitMQ quorum queue cluster?**
A: Roughly 20-30K messages/sec per quorum queue (Raft consensus overhead). For our peak of 500K/min (~8.3K/sec), a single quorum queue suffices. At 10x scale, we'd shard across multiple queues (by tenant or task type) and use consistent hashing exchange for routing. Each queue can sustain 20K/sec, so 5 queues handle 100K/sec.

**Q4: How do delayed messages work without a native RabbitMQ delay plugin?**
A: We use the dead-letter exchange (DLX) pattern. Messages published to the delay queue have a per-message TTL (`x-message-ttl` or `expiration` property). When the TTL expires, RabbitMQ routes the message to the configured dead-letter exchange (which is our main task exchange). This effectively implements delayed delivery. Caveat: messages are processed in order of arrival, not TTL expiry -- a message with a 1-hour TTL ahead of a 1-second TTL blocks the second. We mitigate by using multiple delay queues bucketed by delay duration (< 1m, < 10m, < 1h, < 24h).

**Q5: How do you handle backpressure when the queue is full?**
A: Three-tier approach. (1) Queue level: `overflow: reject-publish` causes the broker to reject messages when queue depth exceeds `max-length`. The publisher receives a NACK and can retry or buffer locally. (2) Rate limiting: the API gateway enforces per-tenant rate limits. (3) Producer-side: the SDK includes a local buffer with configurable max size; when the buffer is full, `submit()` blocks or throws, allowing the producer to apply its own backpressure.

**Q6: How would you migrate from RabbitMQ to Kafka for higher throughput?**
A: We'd use the strangler fig pattern. (1) Add a Kafka cluster alongside RabbitMQ. (2) Dual-write new tasks to both brokers. (3) Gradually move consumers from RabbitMQ to Kafka, queue by queue. (4) For priority support on Kafka, use separate topics per priority level (topic-high, topic-default, topic-bulk) and have consumers poll them with weighted round-robin. (5) For delayed messages, use a sidecar service that reads from a delay topic, holds messages in a timer wheel, and republishes them to the execution topic when the delay expires.

---

### 6.2 Worker Process Model & Concurrency

**Why it's hard:** Workers must maximize throughput while providing isolation between tasks. Different task types have different resource profiles: CPU-bound tasks need separate processes (avoid GIL in Python), I/O-bound tasks benefit from async/threading, and long-running tasks must not block the worker from heartbeating.

**Approaches Compared:**

| Model | Isolation | Throughput (CPU-bound) | Throughput (I/O-bound) | Memory | Complexity |
|-------|-----------|----------------------|----------------------|--------|------------|
| Prefork (multiprocess) | Strong (OS process) | Excellent (true parallelism) | Moderate | High (per-process overhead) | Medium |
| Threading (thread pool) | Weak (shared memory) | Poor (Python GIL) / Good (Java) | Good | Low | Low |
| Async (asyncio/event loop) | None (cooperative) | Poor (single-threaded) | Excellent | Very Low | Medium |
| Hybrid: prefork + async per process | Strong + efficient I/O | Good | Excellent | Medium | High |

**Selected: Prefork as default (Celery model for Python workers); Thread pool for Java workers (Spring @Async + CompletableFuture)**

**Python Worker (Celery-style):**

```python
# Worker configuration
from celery import Celery, Task
from celery.signals import task_prerun, task_postrun, task_failure

app = Celery('infra-tasks')
app.config_from_object({
    'broker_url': 'amqp://user:pass@rabbitmq-cluster:5672/infra',
    'result_backend': 'redis://redis-cluster:6379/0',
    'task_serializer': 'json',
    'result_serializer': 'json',
    'accept_content': ['json'],
    'task_acks_late': True,          # ACK after completion (at-least-once)
    'task_reject_on_worker_lost': True,  # Requeue if worker dies
    'worker_prefetch_multiplier': 4,     # Prefetch 4 tasks per process
    'worker_concurrency': 8,             # 8 prefork processes
    'worker_max_tasks_per_child': 1000,  # Restart process after 1000 tasks (leak prevention)
    'worker_max_memory_per_child': 512_000,  # 512MB per process limit
    'task_time_limit': 3600,             # Hard kill after 1 hour
    'task_soft_time_limit': 3300,        # SoftTimeLimitExceeded after 55 min
    'task_default_rate_limit': '100/m',
})

class BaseTask(Task):
    """Base task with retry, metrics, and error handling."""
    
    autoretry_for = (ConnectionError, TimeoutError)
    retry_backoff = True
    retry_backoff_max = 300  # 5 min max backoff
    retry_jitter = True
    max_retries = 3
    
    def on_failure(self, exc, task_id, args, kwargs, einfo):
        """Called when task permanently fails (exhausted retries)."""
        metrics.counter('task.failed.permanent', 
                        tags={'task_name': self.name}).increment()
        # DLQ entry created by RabbitMQ dead-letter exchange
    
    def on_retry(self, exc, task_id, args, kwargs, einfo):
        metrics.counter('task.retried', tags={'task_name': self.name}).increment()
    
    def on_success(self, retval, task_id, args, kwargs):
        metrics.counter('task.succeeded', tags={'task_name': self.name}).increment()


@app.task(base=BaseTask, bind=True, name='infra.provision.bare_metal',
          queue='provisioning', priority=3,
          rate_limit='50/m')  # Max 50 provisions per minute
def provision_bare_metal(self, server_id: str, datacenter: str, config: dict):
    """Provision a bare-metal server."""
    try:
        # 1. Allocate hardware
        hardware = hardware_inventory.allocate(datacenter, config)
        
        # 2. Configure BIOS/firmware
        bios_manager.configure(hardware.bmc_ip, config)
        
        # 3. PXE boot and OS install
        pxe_server.boot(hardware.mac_address, os_image=config.get('os', 'ubuntu-22.04'))
        
        # 4. Wait for OS ready (with soft timeout awareness)
        try:
            wait_for_ready(hardware.ip, timeout=3000)
        except SoftTimeLimitExceeded:
            # Clean up and re-raise to trigger retry
            hardware_inventory.release(hardware.hardware_id)
            raise self.retry(countdown=60)
        
        # 5. Register in CMDB
        cmdb.register_server(server_id, hardware)
        
        return {
            'server_id': server_id,
            'ip_address': hardware.ip,
            'hardware_id': hardware.hardware_id,
            'provisioned_at': datetime.utcnow().isoformat()
        }
    
    except HardwareUnavailableError as e:
        # Retryable: wait for hardware to become available
        raise self.retry(exc=e, countdown=120, max_retries=10)
    
    except ConfigurationError as e:
        # Non-retryable: configuration is wrong, fix the input
        raise  # Goes to DLQ
```

**Java Worker (Spring Boot + CompletableFuture):**

```java
@Service
public class TaskWorkerService {
    
    // Thread pool for task execution (separate from RabbitMQ listener threads)
    private final ExecutorService taskExecutor;
    
    // Connection pool for database operations within tasks
    @Autowired
    private DataSource dataSource; // HikariCP
    
    @Autowired
    private MeterRegistry metrics;
    
    public TaskWorkerService(
            @Value("${worker.concurrency:16}") int concurrency,
            @Value("${worker.pool.name:default}") String poolName) {
        
        this.taskExecutor = new ThreadPoolExecutor(
            concurrency,                          // Core pool size
            concurrency,                          // Max pool size (no elasticity)
            60L, TimeUnit.SECONDS,                // Idle thread timeout
            new LinkedBlockingQueue<>(concurrency * 2),  // Bounded queue (backpressure)
            new ThreadFactoryBuilder()
                .setNameFormat("task-worker-" + poolName + "-%d")
                .setUncaughtExceptionHandler((t, e) -> 
                    log.error("Uncaught exception in worker thread {}", t.getName(), e))
                .build(),
            new ThreadPoolExecutor.CallerRunsPolicy()  // Backpressure: caller thread executes
        );
    }
    
    @RabbitListener(queues = "${worker.queues}", 
                    concurrency = "${worker.listener.concurrency:4}",
                    ackMode = "MANUAL")
    public void onMessage(Message message, Channel channel, 
                          @Header(AmqpHeaders.DELIVERY_TAG) long deliveryTag) {
        
        String taskId = message.getMessageProperties().getMessageId();
        String taskName = message.getMessageProperties().getHeader("task_name");
        
        Timer.Sample sample = Timer.start(metrics);
        
        CompletableFuture.supplyAsync(() -> {
            // Execute the task
            MDC.put("task_id", taskId);
            MDC.put("task_name", taskName);
            
            try {
                TaskMessage taskMessage = deserialize(message);
                TaskHandler handler = taskHandlerRegistry.getHandler(taskName);
                
                if (handler == null) {
                    throw new UnknownTaskException("No handler for task: " + taskName);
                }
                
                // Execute with timeout
                return CompletableFuture.supplyAsync(
                    () -> handler.execute(taskMessage.getArgs()),
                    taskExecutor
                ).get(handler.getTimeoutSeconds(), TimeUnit.SECONDS);
                
            } catch (TimeoutException e) {
                throw new TaskTimeoutException("Task " + taskId + " timed out", e);
            } finally {
                MDC.clear();
            }
        }, taskExecutor)
        .thenAccept(result -> {
            // Success: ACK and store result
            try {
                channel.basicAck(deliveryTag, false);
                resultBackend.store(taskId, result, getResultTTL(taskName));
                taskRepo.updateStatus(taskId, TaskStatus.SUCCEEDED);
                sample.stop(metrics.timer("task.execution.duration", "task", taskName, "status", "success"));
            } catch (Exception e) {
                log.error("Failed to ACK task {}", taskId, e);
            }
        })
        .exceptionally(throwable -> {
            // Failure: NACK (requeue for retry or send to DLQ)
            try {
                Throwable cause = throwable.getCause();
                boolean requeue = isRetryable(cause) && getAttemptNumber(message) < getMaxRetries(taskName);
                
                channel.basicNack(deliveryTag, false, requeue);
                
                if (!requeue) {
                    taskRepo.updateStatus(taskId, TaskStatus.DEAD_LETTERED);
                }
                
                sample.stop(metrics.timer("task.execution.duration", "task", taskName, "status", "failure"));
            } catch (Exception e) {
                log.error("Failed to NACK task {}", taskId, e);
            }
            return null;
        });
    }
}
```

**HikariCP Configuration for Workers:**

```yaml
# Each worker process gets its own connection pool
spring:
  datasource:
    hikari:
      pool-name: worker-db-pool
      maximum-pool-size: 20         # Per worker pod; 16 task threads + 4 overhead
      minimum-idle: 5
      idle-timeout: 300000
      max-lifetime: 1800000
      connection-timeout: 5000
      leak-detection-threshold: 30000  # 30s (tasks can be long)
      
# Rule of thumb: pool_size = worker_concurrency * 1.25
# 16 concurrent tasks × 1.25 = 20 connections
# At 50K workers with 20 connections each = 1M total connections → need ProxySQL
```

**Failure Modes:**
- **Worker OOM kill:** OS kills worker process. Prefork parent detects child death, spawns replacement. Unacked messages requeued by broker.
- **Task infinite loop:** `task_time_limit` (hard) triggers SIGKILL after 1 hour. `task_soft_time_limit` (soft) raises SoftTimeLimitExceeded after 55 minutes, allowing graceful cleanup.
- **Worker process leak (memory creep):** `worker_max_tasks_per_child=1000` recycles processes after N tasks. `worker_max_memory_per_child=512MB` kills processes exceeding memory limit.
- **Thread pool exhaustion (Java):** CallerRunsPolicy applies backpressure -- the RabbitMQ listener thread itself executes the task, which naturally slows down message consumption. This prevents unbounded queue growth.

**Interviewer Q&A:**

**Q1: Why prefork instead of threading for Python workers?**
A: CPython's Global Interpreter Lock (GIL) prevents true parallel execution in threads. For CPU-bound tasks (data processing, crypto), threading provides zero parallelism. Prefork spawns separate OS processes, each with its own GIL, achieving true parallelism. For I/O-bound tasks, we offer an async worker pool that uses asyncio event loops within prefork processes -- best of both worlds.

**Q2: How do you size the worker thread pool in Java?**
A: For CPU-bound tasks: threads = number of CPU cores (e.g., 8 on an 8-core machine). For I/O-bound tasks: threads = CPU cores * (1 + wait_time/compute_time). If tasks spend 80% of time waiting on I/O, threads = 8 * (1 + 4) = 40. We default to 16 and tune based on profiling. The HikariCP pool must be >= thread count to avoid connection starvation.

**Q3: What is `worker_prefetch_multiplier` and how do you tune it?**
A: Prefetch multiplier controls how many unacknowledged messages the broker sends to a worker process. Set to 4: each process has 4 tasks buffered locally. Higher values reduce broker round-trips (better throughput) but risk task starvation (if one process holds many tasks while others are idle). For long-running tasks, set to 1 (one task at a time per process). For sub-second tasks, set to 16-32.

**Q4: How do you handle a task that holds a database connection for 30 minutes?**
A: HikariCP's `leak-detection-threshold` will log a warning at 30s. We set `max-lifetime` to 30 minutes to prevent stale connections. For genuinely long tasks, we recommend using short-lived connections (open, query, close) rather than holding connections for the task duration. The task should use a connection pool, acquire connections only when needed, and release them immediately after use.

**Q5: How do you prevent a slow consumer from falling behind?**
A: Three mechanisms. (1) Prefetch limit: the broker only sends N unacknowledged messages per consumer, preventing unbounded buffering. (2) Auto-scaling: KEDA scales worker pods based on queue depth. (3) Monitoring: alert when queue depth increases monotonically for > 5 minutes. Root cause could be: slow task code, insufficient worker count, or downstream dependency failure.

**Q6: How do you handle task serialization security (Pickle deserialization vulnerabilities)?**
A: We never use Pickle for untrusted input. Default serializer is JSON (safe). For internal tasks with complex objects, we use Protobuf (schema-enforced, no arbitrary code execution). Celery's `accept_content` setting is restricted to `['json']` -- any Pickle-serialized message is rejected. For performance-sensitive tasks, we use MessagePack (binary JSON, safe).

---

### 6.3 Task Workflows: Chains, Groups, Chords

**Why it's hard:** Workflows compose individual tasks into complex execution patterns. Chains must handle intermediate failures. Groups must track parallel completion atomically. Chords (group + callback) must trigger the callback exactly once when all group tasks complete -- even under worker failures and retries.

**Approaches Compared:**

| Approach | Atomicity | Complexity | Performance | Failure Handling |
|----------|----------|------------|-------------|-----------------|
| Celery Canvas (in-broker) | Message-level | Medium | Good | Limited (chain breaks on failure) |
| Database-backed workflow engine | Strong (transactions) | High | Moderate | Excellent |
| Redis-backed counters | Weak (no transactions) | Low | Excellent | Moderate |
| Event-driven (saga pattern) | Eventual | High | Good | Excellent (compensating actions) |

**Selected: Hybrid -- Redis atomic counters for chord completion tracking + MySQL for workflow state persistence.** Redis provides the speed needed for high-frequency chord counter decrements. MySQL provides the durability needed for workflow state recovery.

**Implementation: Chord (Parallel + Callback)**

```python
import redis
from celery import group, chord

class ChordManager:
    """Manages chord execution: parallel group + callback on completion."""
    
    def __init__(self, redis_client: redis.Redis, task_repo, result_backend):
        self.redis = redis_client
        self.task_repo = task_repo
        self.result_backend = result_backend
    
    def create_chord(self, group_tasks: list, callback_task: dict, 
                      tenant_id: str) -> dict:
        """
        Create a chord: run all group_tasks in parallel, then run 
        callback_task with collected results.
        """
        group_id = str(uuid7())
        callback_id = str(uuid7())
        
        # Persist group metadata
        self.task_repo.create_group(
            group_id=group_id,
            tenant_id=tenant_id,
            total_tasks=len(group_tasks),
            chord_callback_id=callback_id
        )
        
        # Initialize Redis counter
        # Key: chord:{group_id}:remaining
        # Value: number of tasks remaining
        chord_key = f"chord:{group_id}:remaining"
        self.redis.set(chord_key, len(group_tasks))
        # Expire after 2x expected max duration (safety net)
        self.redis.expire(chord_key, 7200)
        
        # Results collection key
        results_key = f"chord:{group_id}:results"
        
        # Submit all group tasks
        task_ids = []
        for task_spec in group_tasks:
            task_id = self.submit_task(
                task_name=task_spec['task_name'],
                args=task_spec['args'],
                group_id=group_id,
                chord_callback_id=callback_id,
                tenant_id=tenant_id
            )
            task_ids.append(task_id)
        
        # Persist callback task (PENDING, will be triggered by chord unlock)
        self.task_repo.create_task(
            task_id=callback_id,
            task_name=callback_task['task_name'],
            args=callback_task.get('args', {}),
            status='PENDING',
            group_id=group_id,
            tenant_id=tenant_id
        )
        
        return {
            'group_id': group_id,
            'callback_id': callback_id,
            'task_ids': task_ids,
            'total': len(group_tasks)
        }
    
    def on_group_task_complete(self, task_id: str, group_id: str, 
                                result: any, status: str):
        """Called when a group member task completes (success or failure)."""
        
        chord_key = f"chord:{group_id}:remaining"
        results_key = f"chord:{group_id}:results"
        
        # Store result (atomic)
        self.redis.hset(results_key, task_id, json.dumps({
            'status': status,
            'result': result
        }))
        
        # Decrement counter (atomic)
        remaining = self.redis.decr(chord_key)
        
        if remaining == 0:
            # All tasks complete -- trigger callback
            self._trigger_callback(group_id)
        elif remaining < 0:
            # Race condition or duplicate completion -- log and ignore
            log.warn(f"Chord counter went negative for group {group_id}")
    
    def _trigger_callback(self, group_id: str):
        """Trigger the chord callback with all group results."""
        results_key = f"chord:{group_id}:results"
        
        # Collect all results
        all_results = self.redis.hgetall(results_key)
        parsed_results = {
            task_id.decode(): json.loads(result.decode())
            for task_id, result in all_results.items()
        }
        
        # Get callback task
        group_meta = self.task_repo.get_group(group_id)
        callback_id = group_meta.chord_callback_id
        callback_task = self.task_repo.get_task(callback_id)
        
        # Determine group status
        any_failed = any(r['status'] == 'FAILED' for r in parsed_results.values())
        
        if any_failed and not callback_task.run_on_group_failure:
            # Skip callback, mark as CANCELLED
            self.task_repo.update_status(callback_id, 'CANCELLED')
            self.task_repo.update_group_status(group_id, 'FAILED')
            return
        
        # Enqueue callback with group results as argument
        enriched_args = {
            **callback_task.args,
            'group_results': parsed_results
        }
        
        self.submit_task(
            task_id=callback_id,
            task_name=callback_task.task_name,
            args=enriched_args,
            tenant_id=group_meta.tenant_id
        )
        
        self.task_repo.update_group_status(group_id, 'SUCCEEDED')
        
        # Cleanup Redis keys
        self.redis.delete(f"chord:{group_id}:remaining", results_key)
```

**Chain Implementation:**

```python
class ChainManager:
    """Manages sequential task chains."""
    
    def create_chain(self, tasks: list, tenant_id: str) -> dict:
        chain_id = str(uuid7())
        task_ids = []
        
        # Create all tasks in PENDING state
        for i, task_spec in enumerate(tasks):
            task_id = str(uuid7())
            parent_id = task_ids[-1] if task_ids else None
            
            self.task_repo.create_task(
                task_id=task_id,
                task_name=task_spec['task_name'],
                args=task_spec['args'],
                status='PENDING' if i > 0 else 'QUEUED',
                parent_task_id=parent_id,
                chain_position=i,
                tenant_id=tenant_id
            )
            task_ids.append(task_id)
        
        # Only submit the first task
        if task_ids:
            self.submit_task(task_ids[0], tasks[0]['task_name'], tasks[0]['args'])
        
        return {'chain_id': chain_id, 'task_ids': task_ids}
    
    def on_chain_task_complete(self, task_id: str, result: any, status: str):
        """Called when a chain task completes."""
        # Find next task in chain
        next_task = self.task_repo.find_by_parent(task_id)
        
        if next_task is None:
            return  # End of chain
        
        if status == 'SUCCEEDED':
            # Enrich next task's args with previous result (pipe pattern)
            enriched_args = {**next_task.args, 'previous_result': result}
            self.submit_task(next_task.task_id, next_task.task_name, enriched_args)
        else:
            # Chain broken -- mark remaining tasks as CANCELLED
            self._cancel_remaining(next_task.task_id)
```

**Failure Modes:**
- **Chord counter lost (Redis crash):** On Redis recovery, we reconstruct the counter from MySQL: `SELECT COUNT(*) FROM task_instance WHERE group_id = ? AND status IN ('SUCCEEDED','FAILED')`. If this equals `total_tasks`, trigger callback.
- **Chord callback triggered twice (race condition):** The callback task ID is pre-generated. Publishing is idempotent on task_id (broker deduplication or consumer idempotency check).
- **Chain task fails mid-chain:** Remaining tasks are marked CANCELLED. The user can retry the failed task, which resumes the chain from that point.
- **Group task retried after chord already unlocked:** The Redis counter goes negative. We detect this (negative value) and log a warning. The callback already executed; the retry result is stored but ignored.

**Interviewer Q&A:**

**Q1: How does Celery implement chords differently from your design?**
A: Celery uses a similar Redis-counter approach but stores chord state in the result backend. Our design separates the fast path (Redis counter for chord unlock) from the persistent path (MySQL for workflow state). This gives us both speed and durability. Celery's chord can lose state if Redis restarts because it only stores chord metadata in Redis.

**Q2: What if one task in a group hangs forever?**
A: The group has a deadline derived from the max timeout of its member tasks. A background sweep checks groups where `created_at + max_timeout < now()` and at least one task is still ACTIVE. It force-fails the hung task (TIMED_OUT), which triggers the chord completion check. If the callback should run despite failures, it proceeds; otherwise, the group is marked FAILED.

**Q3: Can chains and chords be nested (e.g., a chord within a chain)?**
A: Yes. A chain step can be a chord (parallel fanout, then callback continues the chain). The callback task is set as the parent of the next chain step. We limit nesting depth to 5 levels to prevent complexity explosion. Deeply nested workflows should use the DAG scheduler instead.

**Q4: How do you handle partial group failure (3 of 10 tasks fail)?**
A: Configurable per chord. Options: (1) `fail_fast`: cancel remaining tasks on first failure, skip callback. (2) `continue`: run all tasks, pass results (including failures) to callback. (3) `threshold`: proceed with callback if >= N% succeed (e.g., 70%). Default is `continue` -- the callback receives both successful and failed results and decides what to do.

**Q5: How do you ensure the chain pipe pattern doesn't create unbounded message sizes?**
A: Each chain step only passes `previous_result` (the immediate predecessor's result), not the entire chain history. If the result is large (> 64KB), we store it in object storage and pass a reference. The chain step must explicitly declare which fields from the previous result it needs (`@chain_input(fields=['server_id', 'ip_address'])`), limiting payload growth.

**Q6: How do you debug a failed chord in production?**
A: `taskq chord-status <group_id>` shows: total tasks, completed count, failed count, pending count, each task's status and error (if any), callback status. The MySQL `task_group` + `task_instance` tables provide full history. Elasticsearch allows searching for all tasks in a group with their execution traces.

---

### 6.4 Rate Limiting Engine

**Why it's hard:** Rate limiting must be enforced across distributed workers with minimal coordination overhead. It must support multiple granularities (per task type, per tenant, global) and be fair without adding significant latency to the task execution path.

**Approaches Compared:**

| Approach | Accuracy | Distributed | Latency Overhead | Burst Handling |
|----------|---------|-------------|-----------------|----------------|
| Fixed window counter | Moderate (boundary burst) | Easy (Redis INCR) | < 1ms | Poor |
| Sliding window log | Exact | Hard (sorted set) | 2-5ms | Exact |
| Sliding window counter | Good approximation | Easy (two counters) | < 1ms | Good |
| Token bucket | Good | Medium (atomic ops) | < 1ms | Excellent (allows bursts) |
| Leaky bucket | Good | Medium | < 1ms | None (strict rate) |

**Selected: Token bucket (Redis-based)** -- Allows brief bursts while maintaining average rate. Redis atomic operations (Lua script) ensure consistency across distributed workers.

**Implementation:**

```python
# Redis Lua script for atomic token bucket
TOKEN_BUCKET_LUA = """
local key = KEYS[1]
local max_tokens = tonumber(ARGV[1])
local refill_rate = tonumber(ARGV[2])  -- tokens per second
local now = tonumber(ARGV[3])          -- current time in milliseconds
local requested = tonumber(ARGV[4])    -- tokens requested (usually 1)

-- Get current state
local state = redis.call('HMGET', key, 'tokens', 'last_refill')
local tokens = tonumber(state[1])
local last_refill = tonumber(state[2])

-- Initialize if new
if tokens == nil then
    tokens = max_tokens
    last_refill = now
end

-- Calculate tokens to add (time elapsed * refill rate)
local elapsed_ms = now - last_refill
local new_tokens = elapsed_ms * refill_rate / 1000.0
tokens = math.min(max_tokens, tokens + new_tokens)

-- Try to consume
if tokens >= requested then
    tokens = tokens - requested
    redis.call('HMSET', key, 'tokens', tokens, 'last_refill', now)
    redis.call('EXPIRE', key, 3600)  -- Auto-cleanup after 1 hour of inactivity
    return 1  -- Allowed
else
    -- Update refill time even if rejected (to prevent token accumulation on next call)
    redis.call('HMSET', key, 'tokens', tokens, 'last_refill', now)
    redis.call('EXPIRE', key, 3600)
    return 0  -- Rate limited
end
"""

class DistributedRateLimiter:
    def __init__(self, redis_client):
        self.redis = redis_client
        self.script = self.redis.register_script(TOKEN_BUCKET_LUA)
    
    def allow(self, key: str, max_tokens: int, refill_rate: float, 
              requested: int = 1) -> bool:
        """
        Check if the request is allowed under rate limit.
        
        key: Rate limit bucket key (e.g., "rl:task:infra.provision:tenant-a")
        max_tokens: Bucket capacity (burst size)
        refill_rate: Tokens per second (sustained rate)
        requested: Tokens to consume (1 for single task)
        """
        now_ms = int(time.time() * 1000)
        result = self.script(
            keys=[key],
            args=[max_tokens, refill_rate, now_ms, requested]
        )
        return result == 1
    
    def check_and_enqueue(self, task_message: TaskMessage) -> bool:
        """Multi-level rate limit check before enqueuing a task."""
        task_type = task_message.task_name
        tenant_id = task_message.tenant_id
        
        # Level 1: Global rate limit (platform-wide)
        if not self.allow(
            key="rl:global",
            max_tokens=10000,     # Allow burst of 10K
            refill_rate=8333      # 500K/min = 8333/s sustained
        ):
            raise RateLimitExceeded("Global rate limit reached")
        
        # Level 2: Per-tenant rate limit
        tenant_config = self.get_tenant_config(tenant_id)
        if not self.allow(
            key=f"rl:tenant:{tenant_id}",
            max_tokens=tenant_config.burst_limit,    # e.g., 1000
            refill_rate=tenant_config.rate_per_second  # e.g., 100/s
        ):
            raise RateLimitExceeded(f"Tenant {tenant_id} rate limit reached")
        
        # Level 3: Per-task-type rate limit
        type_config = self.get_task_type_config(task_type)
        if type_config.rate_limit:
            if not self.allow(
                key=f"rl:task:{task_type}:{tenant_id}",
                max_tokens=type_config.burst_limit,
                refill_rate=type_config.rate_per_second
            ):
                raise RateLimitExceeded(f"Task type {task_type} rate limit reached for {tenant_id}")
        
        return True
```

**Failure Modes:**
- **Redis unavailable:** Fall back to local in-process rate limiter (per-worker, not global). Accept slightly inaccurate rate limiting rather than blocking all tasks.
- **Clock skew between workers:** Use Redis server time (`TIME` command) rather than local clock. The Lua script runs atomically on Redis server, so clock skew is irrelevant for token calculation.
- **Rate limit config change:** New config propagated via etcd watch. Workers pick up changes within 5 seconds. During transition, some workers may use old limits -- acceptable temporary inconsistency.

**Interviewer Q&A:**

**Q1: Why token bucket over sliding window?**
A: Token bucket naturally handles bursts -- a task type with a 100/minute limit can burst 100 tasks in the first second if the bucket is full, then sustain 1.67/sec. Sliding window would reject the burst. For infrastructure tasks, burst handling is important (e.g., 50 servers need provisioning simultaneously after a capacity event).

**Q2: What happens to rate-limited tasks?**
A: Two options (configurable per task type). (1) `reject`: return 429 to the producer; producer retries with backoff. (2) `delay`: enqueue the task with an ETA set to when tokens will be available. For interactive tasks (API-triggered), we use `reject`. For background tasks (system-generated), we use `delay`.

**Q3: How accurate is the distributed rate limiter?**
A: With the Redis Lua script approach, accuracy is near-perfect for a single key: atomic read-modify-write ensures no race conditions. Across multiple workers, the accuracy depends on Redis latency -- with < 1ms RTT, even bursty workloads stay within 5% of the configured limit. For extremely high-precision requirements, we'd use a centralized rate limiter service, but the added latency is usually not worth it.

**Q4: How do you handle rate limit fairness across tenants?**
A: Each tenant has an independent token bucket. A tenant consuming its full rate limit does not affect other tenants. If the global rate limit is reached (all tenants combined), we enforce a fair sharing policy: each tenant gets min(their_limit, global_limit / active_tenant_count). This prevents a single tenant from consuming the entire platform capacity.

**Q5: How do you communicate rate limits to producers?**
A: The API response includes rate limit headers: `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`. The 429 response body includes `retry-after` in seconds. The SDK automatically handles retry-after delays. The CLI displays rate limit status in `taskq ratelimit show`.

**Q6: How do you rate limit across multiple RabbitMQ queues?**
A: Rate limiting is enforced at two points. (1) Enqueue time: the API gateway checks rate limits before publishing to RabbitMQ. (2) Worker side: Celery's `rate_limit` decorator controls consumption rate. The enqueue-time check prevents queue depth from growing under rate limiting. The worker-side check provides defense-in-depth and handles tasks that bypass the API (e.g., chain/chord internal submissions).

---

## 7. Scheduling & Resource Management

### Placement Algorithm

Task queue systems don't use a traditional placement algorithm like job schedulers. Instead, routing is determined by queue-to-worker binding:

```
Producer → Exchange (routes by task_name/routing_key) → Queue → Worker
```

**Queue-to-Worker Binding:**
- Each worker pool declares which queues it consumes: `worker.queues = ["provisioning", "default"]`
- Routing key patterns: `infra.provision.*` → `provisioning` queue, `infra.monitor.*` → `monitoring` queue
- Workers consume with round-robin (default) or priority-based (consume from high-priority queue first)

### Conflict Detection

- **Deduplication by idempotency key:** If two producers submit the same task (same idempotency key), the second submission is rejected at the API gateway (Redis SETNX with TTL).
- **Concurrent task conflicts:** For tasks operating on the same resource (e.g., two provisioning tasks for the same server_id), we use a distributed lock: `Redis SETNX task_lock:{resource_id}` with TTL = task timeout. Second task is delayed until lock expires.

### Queue & Priority Implementation

RabbitMQ native priority queues (max 10 levels, 0-9):
```java
// Queue declaration with priority
Map<String, Object> args = new HashMap<>();
args.put("x-max-priority", 10);
channel.queueDeclare("task.default", true, false, false, args);
```

Consumer-side weighted priority: workers consume from `high` queue at 3x the rate of `default`:
```python
# Celery worker consuming multiple queues with weights
app.conf.worker_consumer = 'celery.worker.consumer:Consumer'
# Custom consumer that polls high:default:bulk in 3:2:1 ratio
```

### Preemption Policy

Task queues generally don't support preemption (can't interrupt a running task to run a higher-priority one). Instead:
- High-priority tasks go to a separate queue with dedicated workers (reserved capacity)
- If high-priority queue depth exceeds threshold, KEDA scales up high-priority workers from the general pool
- Running low-priority tasks are not interrupted (too complex and error-prone for short-lived tasks)

### Starvation Prevention

- Workers consume from multiple queues with weighted round-robin: e.g., for every 3 high-priority tasks, consume 2 default and 1 bulk
- Monitoring: alert if any queue's oldest message age exceeds 10 minutes
- Emergency: admin can temporarily increase bulk worker count or reduce high-priority weight

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Trigger |
|-----------|-----------------|---------|
| RabbitMQ broker | Add nodes (up to 7-node quorum cluster); shard queues | Queue throughput > 20K/s per queue |
| Workers (per pool) | HPA/KEDA based on queue depth | Queue depth > N * current_workers |
| API Gateway | HPA based on CPU/request rate | CPU > 70% or RPS > 10K per pod |
| Result Backend (Redis) | Redis Cluster (hash slots) | Memory > 80% or latency P99 > 5ms |
| MySQL | Read replicas for status queries; partition for history | Write QPS > 15K |

### Database Scaling

| Strategy | Implementation | When |
|----------|---------------|------|
| Read replicas | 2 replicas for task status queries | Read QPS > 10K |
| Partitioning | Monthly partitions on task_instance.created_at | Table > 100M rows |
| Connection pooling | HikariCP per service (max 20-50); ProxySQL for multiplexing | Always |
| Archive | Move completed tasks > 30 days to archive | Monthly |
| Sharding | Shard by tenant_id if single instance saturates | > 50K writes/sec |

### Caching

| Layer | Technology | Data | TTL | Invalidation |
|-------|-----------|------|-----|-------------|
| L1: In-process | Caffeine | Task type configs, rate limit configs | 60s | etcd watch |
| L2: Distributed | Redis | Rate limit buckets, task results | Varies | TTL-based |
| L3: Broker | RabbitMQ message cache | In-flight messages | Until ACK | Consumer ACK |
| L4: Search | Elasticsearch | Task search results | 30s | Index refresh |

### Kubernetes-Specific

| Mechanism | Component | Configuration |
|-----------|----------|--------------|
| HPA | API Gateway | CPU target 70%, min 3, max 30 |
| KEDA | Worker pools | Redis queue depth trigger, threshold 50/worker |
| VPA | RabbitMQ pods | Recommendations only (UpdateMode: Off) |
| PDB | RabbitMQ | maxUnavailable: 1 (maintain quorum) |
| Topology spread | Workers | Spread across zones for availability |

```yaml
# KEDA ScaledObject for provisioning workers
apiVersion: keda.sh/v1alpha1
kind: ScaledObject
metadata:
  name: worker-provisioning
spec:
  scaleTargetRef:
    name: worker-provisioning
  minReplicaCount: 5
  maxReplicaCount: 200
  cooldownPeriod: 60
  triggers:
  - type: rabbitmq
    metadata:
      host: amqp://user:pass@rabbitmq:5672/
      queueName: provisioning
      queueLength: "50"        # 50 messages per worker
      activationQueueLength: "1" # Scale from 0 when first message arrives
```

### Interviewer Q&A

**Q1: How do you handle RabbitMQ queue sharding for throughput?**
A: RabbitMQ quorum queues are single-leader per queue. For throughput beyond 20K/s, we create N sharded queues (e.g., `task.default.shard-0` through `task.default.shard-7`) and use a consistent hashing exchange to distribute messages. Workers consume from all shards. This gives us N x 20K/s throughput. Alternatively, the RabbitMQ Sharding plugin automates this.

**Q2: How do you scale workers to zero for idle queues?**
A: KEDA supports scale-to-zero with an `activationQueueLength` threshold. When the queue has 0 messages, KEDA scales workers to 0. When the first message arrives, KEDA scales to `minReplicaCount`. The cold-start latency (~10-30s for pod startup) means the first message in an idle queue experiences higher latency. For latency-sensitive queues, we set `minReplicaCount: 1` to keep at least one warm worker.

**Q3: What's the operational burden of a 7-node RabbitMQ cluster?**
A: Quorum queues require majority for writes: 7 nodes means we tolerate 3 failures (vs. 1 for 3-node). But Raft consensus overhead increases with cluster size. The sweet spot is 3 or 5 nodes. We use 3 for most deployments and 5 for the highest-traffic queues. 7 is only for extreme availability requirements.

**Q4: How do you handle connection storms when many workers restart simultaneously (e.g., rolling deployment)?**
A: Workers use exponential backoff with jitter on connection failure. The RabbitMQ connection factory is configured with `automaticRecoveryEnabled=true` and `networkRecoveryInterval=5000`. Rolling deployments restart workers in batches (25% at a time) with `maxSurge: 25%` and `maxUnavailable: 25%`. This limits concurrent reconnections.

**Q5: How does ProxySQL help with 50K workers all needing MySQL connections?**
A: 50K workers x 20 connections each = 1M connections. MySQL can't handle this directly. ProxySQL sits between workers and MySQL, multiplexing many application connections into a smaller set of MySQL connections (e.g., 500). It also provides: read/write splitting, connection pooling, and query routing. Workers connect to ProxySQL as if it were MySQL.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| Single RabbitMQ node crash | Quorum queue leadership shifts; brief latency spike | Raft follower timeout | Automatic leader election in Raft; no message loss | 5-10s | 0 |
| RabbitMQ quorum loss (2 of 3 nodes) | All quorum queues unavailable for writes | Connection failures | Manual intervention; restore 2nd node | 5-30 min | 0 (Raft log persisted) |
| Redis failure (result backend) | Result retrieval fails; chord unlocks fail | Sentinel detection | Sentinel failover; chord recovery from MySQL | 10-15s | Minimal (results re-queryable) |
| Worker pod crash | Tasks on that pod unACKed; redelivered to other workers | RabbitMQ consumer timeout | Automatic redelivery by broker | 30s (consumer timeout) | 0 |
| Worker pool exhaustion | Queue depth grows; new tasks delayed | Queue depth monitoring | KEDA scales up; alert on-call | 30-60s (KEDA) | 0 |
| MySQL primary failure | Task metadata writes fail | MySQL failover detection | Semi-sync replica promotion | 30-60s | 0 |
| Network partition (workers ↔ broker) | Affected workers lose connection; tasks timeout | Connection drop | Workers reconnect with backoff; broker redelivers unACKed messages | 10-30s | 0 |
| Task infinite loop | Worker thread stuck; reduced concurrency | Task timeout | Soft timeout (exception) → hard timeout (SIGKILL) | Configurable (default 5 min) | 0 |
| Poison message (causes worker crash) | Worker pod restarts in loop | CrashLoopBackOff detection | Delivery limit on quorum queue; message goes to DLQ after N redeliveries | Automatic | 0 |

### Automated Recovery

```java
@Component
public class TaskQueueRecovery {
    
    /**
     * Recover chord state after Redis failure.
     * Runs on scheduler instance, not workers.
     */
    @Scheduled(fixedRate = 60_000)
    public void recoverOrphanedChords() {
        // Find groups where all tasks are complete but callback hasn't fired
        List<TaskGroup> orphaned = groupRepo.findOrphanedGroups(
            Duration.ofMinutes(10));  // Groups > 10 min old with all tasks done
        
        for (TaskGroup group : orphaned) {
            long completed = taskRepo.countByGroupAndStatusIn(
                group.getGroupId(), 
                List.of(TaskStatus.SUCCEEDED, TaskStatus.FAILED));
            
            if (completed >= group.getTotalTasks()) {
                log.warn("Recovering orphaned chord: group={}", group.getGroupId());
                chordManager.triggerCallback(group.getGroupId());
                metrics.counter("recovery.orphaned_chord").increment();
            }
        }
    }
    
    /**
     * Recover tasks stuck in ACTIVE state (worker died without NACKing).
     */
    @Scheduled(fixedRate = 120_000)
    public void recoverStuckTasks() {
        // Tasks in ACTIVE state for longer than their timeout
        List<TaskInstance> stuck = taskRepo.findStuckActiveTasks();
        
        for (TaskInstance task : stuck) {
            log.warn("Recovering stuck task: id={}, active for {}s",
                task.getTaskId(), 
                Duration.between(task.getStartedAt(), Instant.now()).getSeconds());
            
            // RabbitMQ should have already redelivered (consumer timeout).
            // This catches cases where the MySQL status update was lost.
            taskRepo.updateStatus(task.getTaskId(), TaskStatus.FAILED,
                "Auto-recovery: task stuck in ACTIVE past timeout");
            metrics.counter("recovery.stuck_task").increment();
        }
    }
}
```

### Retry Strategy

| Parameter | Default | Configurable | Notes |
|-----------|---------|-------------|-------|
| Max retries | 3 | Per task type | 0 = no retry |
| Initial backoff | 1s | Per task type | |
| Backoff multiplier | 2x | Global | Exponential |
| Max backoff | 5 min | Global | Cap |
| Jitter | +/- 25% | Global | Prevents thundering herd |
| Retry on | All exceptions | Per task type (retryable exception list) | |
| DLQ after | Max retries exhausted | Automatic | RabbitMQ delivery-limit + DLX |

### Circuit Breaker

Per-task-type circuit breaker:

```python
class TaskCircuitBreaker:
    def __init__(self, task_name: str, failure_threshold: float = 0.5,
                 window_seconds: int = 300, open_duration_seconds: int = 60):
        self.task_name = task_name
        self.failure_threshold = failure_threshold
        self.window = window_seconds
        self.open_duration = open_duration_seconds
        self.state = 'CLOSED'
        self.opened_at = None
    
    def record_success(self):
        self.redis.hincrby(f"cb:{self.task_name}:success", self._current_bucket(), 1)
    
    def record_failure(self):
        self.redis.hincrby(f"cb:{self.task_name}:failure", self._current_bucket(), 1)
        self._check_threshold()
    
    def allow_execution(self) -> bool:
        if self.state == 'CLOSED':
            return True
        elif self.state == 'OPEN':
            if time.time() - self.opened_at > self.open_duration:
                self.state = 'HALF_OPEN'
                return True  # Allow test request
            return False
        elif self.state == 'HALF_OPEN':
            return random.random() < 0.1  # 10% test traffic
```

### Consensus & Coordination

| Resource | Mechanism | Tool |
|----------|-----------|------|
| Message ordering | Per-queue FIFO | RabbitMQ |
| Message durability | Raft consensus (quorum queues) | RabbitMQ internal |
| Chord completion | Atomic DECR | Redis |
| Task deduplication | SETNX with TTL | Redis |
| Rate limiting | Lua script (atomic) | Redis |
| Worker registry | Heartbeat + MySQL | Custom |
| Configuration | Watch-based distribution | etcd |

---

## 10. Observability

### Key Metrics

| Metric | Type | Warning | Critical | Dashboard |
|--------|------|---------|----------|-----------|
| `queue.depth` (per queue) | Gauge | > 10K | > 100K | Queue Health |
| `queue.message_age_max_s` | Gauge | > 60s | > 300s | Queue Health |
| `task.enqueue.rate` | Counter | < 50% baseline | < 20% baseline | Throughput |
| `task.completion.rate` | Counter | < 50% baseline | < 20% baseline | Throughput |
| `task.failure.rate` | Rate | > 5% | > 15% | Reliability |
| `task.execution.duration_ms` | Histogram | P99 > 2x baseline | P99 > 5x baseline | Performance |
| `task.dlq.depth` | Gauge | > 100 | > 1000 | Error Health |
| `worker.active.count` | Gauge | < 80% desired | < 50% desired | Fleet Health |
| `worker.utilization_pct` | Gauge | > 90% (overloaded) | > 95% | Fleet Health |
| `broker.connection.count` | Gauge | > 80% max | > 95% max | Broker Health |
| `broker.memory_used_bytes` | Gauge | > 80% limit | > 95% limit | Broker Health |
| `redis.memory_used_bytes` | Gauge | > 80% maxmem | > 95% maxmem | Result Backend |
| `chord.completion.latency_ms` | Histogram | P99 > 30s | P99 > 120s | Workflow Health |
| `ratelimit.rejected.rate` | Counter | > 100/s | > 1000/s | Rate Limiting |

### Distributed Tracing

```
Trace: task submission → broker enqueue → worker dequeue → task execution → result storage
Spans:
  [producer] submit_task (2ms)
    └── [broker] enqueue (1ms)
  [worker] dequeue (50ms wait in queue)
    └── [worker] execute_task (5000ms)
        ├── [worker] db_query (20ms)
        ├── [worker] external_api_call (3000ms)
        └── [worker] store_result (2ms)
```

Headers propagated via AMQP message properties:
```python
props.setHeader('traceparent', f'00-{trace_id}-{span_id}-01')
props.setHeader('tracestate', f'task_id={task_id}')
```

### Logging

Structured JSON logs with task context:
```json
{
  "timestamp": "2026-04-09T14:30:00.123Z",
  "level": "INFO",
  "service": "task-worker",
  "worker_id": "worker-prov-03-pid-1234",
  "pool": "provisioning",
  "task_id": "01903f5b-a7c2-7def-9123-456789abcdef",
  "task_name": "infra.provision.bare_metal",
  "attempt": 1,
  "event": "task.started",
  "args_summary": {"server_id": "srv-12345"},
  "trace_id": "abc123"
}
```

### Alerting

| Alert | Condition | Severity | Action |
|-------|-----------|----------|--------|
| Queue depth growing | depth increasing > 1K/min for 10 min | P2 | Check worker health; KEDA should scale |
| High failure rate | > 15% failure in 5-min window per task type | P2 | Investigate task code; check downstream deps |
| DLQ growing | > 1000 unresolved DLQ items | P3 | Daily triage meeting |
| Broker memory high | > 90% of memory limit | P1 | Queue may become unavailable; purge or scale |
| Worker crash loop | > 3 restarts in 5 min | P2 | Investigate poison message or resource issue |
| Chord stuck | Group running > 2x max task timeout | P3 | Investigate hung task |

---

## 11. Security

### Authentication & Authorization

| Layer | Mechanism | Detail |
|-------|-----------|--------|
| External API | JWT (RS256) | 15-min expiry, issued by IdP |
| Service-to-producer (SDK) | mTLS | Certificate per service identity |
| RabbitMQ | AMQP SASL (EXTERNAL over TLS) | Per-service credentials |
| Redis | AUTH + TLS | Password + TLS in transit |
| Worker-to-broker | mTLS + AMQP credentials | Worker identity verified |

**RBAC:**
```
Roles:
  task-producer:  submit tasks to allowed queues, check status, get results
  task-admin:     CRUD task types, manage rate limits, purge queues, manage DLQ
  task-viewer:    read-only access to task status, metrics
  platform-admin: all operations including worker management
```

### Secrets Management

- RabbitMQ credentials: HashiCorp Vault dynamic secrets (rotated every 24h)
- Redis password: Vault KV v2 (rotated every 30 days)
- Task arguments containing secrets: encrypted at rest in broker (RabbitMQ Shovel encryption); never logged
- Worker environment secrets: Kubernetes Secrets backed by Vault CSI

### Network Security

- RabbitMQ cluster: dedicated subnet, ports 5671 (AMQPS), 25672 (cluster), 15692 (Prometheus)
- Redis: private subnet, port 6380 (TLS)
- API Gateway: public port 443, TLS termination
- Workers: can reach broker and result backend; cannot reach each other (network policy)
- All inter-service communication over mTLS

### Audit Logging

```json
{
  "timestamp": "2026-04-09T14:30:00Z",
  "actor": "service:provisioning-controller",
  "action": "task.submit",
  "resource": "task/01903f5b-a7c2-7def-9123-456789abcdef",
  "task_name": "infra.provision.bare_metal",
  "tenant": "team-platform",
  "queue": "provisioning",
  "source_ip": "10.0.3.15",
  "request_id": "req-xyz-789"
}
```

---

## 12. Incremental Rollout Strategy

### Phase 1: Shadow Mode (Week 1-2)
- Deploy new task queue system alongside existing (e.g., legacy Celery cluster)
- Dual-write: all task submissions go to both old and new systems
- New system processes tasks but discards results (shadow execution)
- Compare: task routing correctness, execution time, failure rate
- **Exit criteria:** < 1% discrepancy in routing decisions for 48h

### Phase 2: Canary (Week 3-4)
- Route 5% of traffic from selected low-risk task types to new system
- Old system handles remaining 95%
- Monitor: task latency, failure rate, result correctness
- Gradually increase: 10%, 25%
- **Exit criteria:** Metrics within 10% of old system baseline

### Phase 3: Blue-Green (Week 5-6)
- 50/50 traffic split
- Both systems share result backend (Redis) and metadata store (MySQL)
- Instant rollback: route all traffic back to old system
- **Exit criteria:** P99 latency meets SLA, error rate < 1%

### Phase 4: Full Rollout (Week 7-8)
- 100% traffic to new system
- Old system in standby for 2 weeks
- Decommission after bake period

### Rollback Strategy
- API Gateway routing change (< 30s to take effect)
- Tasks in new system's broker are not lost (workers continue processing)
- New submissions route to old system immediately
- DLQ items from new system are migrated manually if needed

### Rollout Interviewer Q&A

**Q1: How do you handle task type compatibility between old and new systems?**
A: Both systems use the same task serialization format (JSON) and task name conventions. Worker code is shared (same Python modules/Java classes). The difference is the broker and orchestration layer. Tasks are agnostic to which queue system dispatched them.

**Q2: What if shadow mode reveals a performance regression?**
A: Shadow execution runs at 100% volume but with results discarded. If the new system is slower, we profile: is it broker throughput (RabbitMQ vs. old Redis broker), worker startup time, or serialization overhead? We fix the root cause before progressing to canary.

**Q3: How do you handle in-flight tasks during rollback?**
A: Tasks already dequeued by workers in the new system will complete normally (workers are system-agnostic). Tasks still in the new system's broker remain there; we drain the broker over 24h before full decommission. No task is lost.

**Q4: What's the risk of running both systems simultaneously?**
A: Main risk: resource contention. Both systems consume from RabbitMQ (old might use Redis). Workers are dedicated per system (no sharing). MySQL and Redis connections double during blue-green. We pre-provision additional connection pool capacity.

**Q5: How do you verify result correctness in shadow mode?**
A: For deterministic tasks, we compare results byte-for-byte. For non-deterministic tasks (involving timestamps, random values), we verify structural correctness (same schema, same keys, values within expected ranges). We also verify side effects: did the task make the same API calls, write the same records?

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Selected | Trade-off | Rationale |
|----------|-------------------|----------|-----------|-----------|
| Broker technology | RabbitMQ, Redis Streams, Kafka, Amazon SQS | RabbitMQ quorum queues | Lower throughput than Kafka vs. native priority/TTL/DLX support | Task queue semantics (priority, delay, individual ACK) are native in RabbitMQ; Kafka requires workarounds |
| Result backend | Redis, MySQL, S3 | Redis (hot) + MySQL (cold) | Redis volatility vs. speed | Results are ephemeral (24h TTL); Redis provides sub-ms retrieval; MySQL for audit |
| Serialization | JSON, Protobuf, Pickle, MessagePack | JSON default, Protobuf for perf-critical | Larger payloads vs. debuggability | JSON is human-readable and safe; Protobuf for high-throughput internal tasks |
| Worker process model | Prefork, threading, async | Prefork (Python), threading (Java) | Higher memory vs. isolation | True parallelism for CPU-bound; leak isolation via process recycling |
| Task ACK timing | ACK before execution, ACK after execution | ACK after (acks_late) | Risk of redelivery on crash vs. at-least-once guarantee | Infra tasks must not be lost; redelivery (duplicate) is safer than loss |
| Rate limiting algorithm | Fixed window, sliding window, token bucket | Token bucket | Allows bursts vs. strict rate | Infrastructure workloads are bursty; token bucket handles this naturally |
| Delay implementation | RabbitMQ delayed-message plugin, TTL+DLX, external scheduler | TTL + DLX | Coarse ordering (FIFO not deadline-ordered) vs. no plugin dependency | Plugin adds operational risk; TTL+DLX is native and well-understood |
| Chord tracking | Redis counter, database counter, message-based | Redis counter + MySQL backup | Redis SPOF for chords vs. speed | Redis DECR is atomic and fast; MySQL recovery covers Redis failure |

---

## 14. Agentic AI Integration

### Where AI Adds Value

| Use Case | AI Capability | Business Impact |
|----------|--------------|----------------|
| **Intelligent retry strategy** | LLM classifies error messages to determine retryability | Reduce wasted retries by 60% (permanent errors routed to DLQ immediately) |
| **Queue depth prediction** | ML model predicts queue depth based on historical patterns + calendar | Proactive scaling: workers ready before the surge |
| **DLQ auto-triage** | LLM reads error messages, groups by root cause, suggests fix | Reduce DLQ resolution time from hours to minutes |
| **Anomaly detection** | Statistical model detects unusual failure patterns | Early warning for downstream dependency failures |
| **Task execution time prediction** | ML model estimates task runtime from arguments | Better scheduling and SLA prediction |
| **Natural language task submission** | "Provision 10 servers in us-east-1 with 64 cores each" → API calls | Lower barrier for ops engineers |

### LLM Agent Loop

```python
class DLQTriageAgent:
    """AI agent that triages dead-letter queue items."""
    
    def observe(self) -> list:
        """Gather unresolved DLQ items."""
        return dlq_api.list(resolved=False, limit=50)
    
    def reason(self, dlq_items: list) -> dict:
        """Group by root cause and suggest actions."""
        prompt = f"""
        Analyze these dead-letter queue items and:
        1. Group them by root cause (cluster similar errors)
        2. For each group, classify as: TRANSIENT (retry will fix), 
           PERMANENT (needs code fix), DEPENDENCY (external system down),
           CONFIGURATION (bad input/config)
        3. Suggest action for each group
        
        Items: {json.dumps([{
            'dlq_id': item.dlq_id,
            'task_name': item.task_name,
            'error': item.last_error[:500],
            'attempts': item.total_attempts,
            'dead_lettered_at': item.dead_lettered_at
        } for item in dlq_items])}
        """
        return self.llm.generate(prompt, response_format='json')
    
    def act(self, analysis: dict):
        """Execute suggested actions with guard rails."""
        for group in analysis['groups']:
            if group['classification'] == 'TRANSIENT':
                # Auto-retry (safe: at-least-once already tolerated)
                for dlq_id in group['dlq_ids']:
                    dlq_api.retry(dlq_id, reason=f"AI triage: {group['root_cause']}")
                    
            elif group['classification'] == 'DEPENDENCY':
                # Check if dependency is back
                dep_healthy = self.check_dependency(group['dependency_name'])
                if dep_healthy:
                    for dlq_id in group['dlq_ids']:
                        dlq_api.retry(dlq_id, reason=f"AI triage: dependency recovered")
                else:
                    # Alert the owning team
                    alert_api.notify(group['owner_team'], 
                        f"DLQ pile-up: {len(group['dlq_ids'])} items due to {group['dependency_name']} failure")
            
            elif group['classification'] in ('PERMANENT', 'CONFIGURATION'):
                # Don't auto-retry; create ticket
                ticket_api.create(
                    team=group['owner_team'],
                    title=f"DLQ: {group['root_cause']}",
                    body=f"{len(group['dlq_ids'])} tasks affected. Suggested fix: {group['suggestion']}",
                    priority='P3'
                )
    
    def verify(self, actions: list):
        """Check that retried tasks succeeded."""
        for action in actions:
            if action['type'] == 'retry':
                task = task_api.get(action['task_id'])
                if task.status == 'FAILED':
                    # Retry failed again -- escalate
                    alert_api.notify('on-call', 
                        f"AI DLQ retry failed again: {action['task_id']}")
```

### Guard Rails

| Guard Rail | Implementation |
|-----------|---------------|
| Max auto-retries per DLQ item | 1 (if AI retry fails, human must investigate) |
| Approval for bulk retry (> 50 items) | Slack approval from team lead |
| Never auto-resolve PERMANENT items | AI can only retry TRANSIENT or DEPENDENCY |
| Rate limit on AI actions | 50 actions/hour |
| Audit trail | All AI actions logged with reasoning and classification |
| Kill switch | `taskq admin ai-agent disable` |
| Dry-run mode | `AI_AGENT_DRY_RUN=true` logs actions without executing |

---

## 15. Complete Interviewer Q&A Bank

**Q1: How do you guarantee at-least-once delivery in your task queue?**
A: Three mechanisms working together. (1) Publisher confirms: the broker ACKs only after the message is persisted to a quorum of nodes. If no ACK received, the publisher retries. (2) Consumer manual ACK: the worker ACKs only after task execution completes successfully. If the worker crashes before ACKing, the broker redelivers to another worker. (3) MySQL write-ahead: we write the task record to MySQL before publishing to the broker. A recovery process catches any tasks that were persisted but never published.

**Q2: What is the difference between your task queue and a distributed job scheduler?**
A: Task queues are for short-lived, stateless tasks (median 5s, max 1 hour) submitted programmatically by services. They emphasize throughput, low latency, and simple producer/consumer semantics. Job schedulers are for longer-running, scheduled, stateful jobs with dependencies (DAGs), recurring schedules (cron), and complex lifecycle management. In our platform, the job scheduler submits tasks to the task queue for actual execution.

**Q3: How would you handle a 10x traffic spike in 5 minutes?**
A: (1) KEDA auto-scales workers based on queue depth (kicks in within 30s). (2) RabbitMQ buffers messages in quorum queues (durable to disk). Queue depth grows but messages are not lost. (3) API gateway rate limiting prevents broker overload. (4) Producers receive backpressure (429 responses or NACK from broker) and buffer locally. (5) During sustained spikes, Cluster Autoscaler provisions additional bare-metal nodes for new worker pods.

**Q4: How do you prevent a single tenant from consuming all queue capacity?**
A: (1) Per-tenant rate limiting at the API gateway (token bucket in Redis). (2) Separate queues per tenant for critical tenants (`task.provisioning.tenant-A`). (3) Worker pool isolation: high-priority tenants get dedicated worker pools. (4) Fair scheduling: within shared queues, messages are interleaved across tenants (not strictly FIFO when one tenant floods).

**Q5: What happens if your Redis result backend loses data?**
A: Task results in Redis have a 24h TTL. If Redis fails and data is lost: (1) Pending result fetches return "result not available" -- the caller must retry the task. (2) Chord unlocks are recovered from MySQL (see automated recovery). (3) For critical results (e.g., provisioning outputs), we write to both Redis and MySQL; Redis loss doesn't affect MySQL copy. (4) Redis Sentinel provides automatic failover with < 15s downtime and minimal data loss (AOF with 1s fsync).

**Q6: How do you handle task versioning when deploying new worker code?**
A: Tasks include a `version` header. Workers check version compatibility before execution. During rolling deployments, both old and new worker versions are active. Strategy: (1) New task submissions include the new version. (2) Old workers skip tasks with unknown versions (re-queue with delay). (3) New workers handle both old and new versions. (4) Once all workers are updated, old-version tasks are retired.

**Q7: How do you debug a task that fails intermittently?**
A: (1) Distributed tracing: follow the trace_id across producer → broker → worker → downstream services. (2) Structured logs: search Elasticsearch for `task_id=X` to see all log entries across retries. (3) Execution history: `taskq history <task_id>` shows all attempts with timing, worker, error. (4) Replay: `taskq replay <task_id>` resubmits the exact same arguments to a debug worker with verbose logging enabled.

**Q8: How do you handle message ordering guarantees?**
A: RabbitMQ provides per-queue FIFO ordering for messages at the same priority level. However, with multiple consumers, processing order is not guaranteed (consumer A may complete message 2 before consumer B completes message 1). For strict ordering, we use a single consumer per ordering key (e.g., per-resource). Alternatively, tasks that require ordering are submitted as a chain (sequential execution guaranteed).

**Q9: How do you handle large task payloads (> 1MB)?**
A: RabbitMQ's default message size limit is 128MB, but large messages reduce throughput. For payloads > 64KB, we use the "claim check" pattern: store the payload in object storage (MinIO), include only the reference in the message. Workers download the payload from object storage before execution. This keeps broker throughput high.

**Q10: How would you implement task priorities without RabbitMQ's native priority queue?**
A: Three options: (1) Separate queues per priority level (queue-high, queue-default, queue-low) with workers consuming from high first (weighted round-robin). (2) Redis sorted set (score = priority) with ZPOPMIN. (3) Kafka: separate topics per priority with weighted consumer assignment. Option 1 is simplest and what we'd use if we migrated to Kafka.

**Q11: How do you monitor and alert on "silent failures" (tasks that complete but produce wrong results)?**
A: (1) Result validation: task types can register a validator function that checks the result for correctness (e.g., expected keys present, values in range). (2) End-to-end health checks: synthetic tasks submitted every minute that verify the full path (enqueue → execute → result). (3) Business-level monitors: downstream systems alert if expected side effects don't occur within SLA.

**Q12: What's the impact of a RabbitMQ partition (network split between nodes)?**
A: Quorum queues handle partitions gracefully: the partition with a majority of nodes continues operating normally. The minority partition cannot accept writes (queues are unavailable on those nodes). Clients connected to minority nodes get errors and should reconnect to majority nodes. Once the partition heals, the minority nodes rejoin and catch up from the Raft log. No messages are lost.

**Q13: How do you handle task idempotency for non-idempotent tasks?**
A: For tasks that cannot be made idempotent (e.g., sending an email, charging a credit card), we use the "outbox + deduplication" pattern: (1) Before executing the side effect, write an intent record to MySQL with the idempotency token. (2) Execute the side effect. (3) Mark the intent as completed. On retry, check if the intent is already completed; if so, skip. If the intent exists but is not completed (crash mid-execution), we need domain-specific logic (check if the email was sent, check if the charge went through).

**Q14: How do you handle back-of-queue starvation for low-priority tasks?**
A: Workers consume from priority queues with weighted round-robin: for every 5 high-priority tasks, consume 3 default and 2 bulk. This guarantees minimum throughput for each priority level. Additionally, we monitor oldest message age per queue and alert if low-priority messages wait > 10 minutes. In extremis, we temporarily add workers to the low-priority pool.

**Q15: What is the cold start latency for a new worker pod and how do you minimize it?**
A: Container image pull: 5-10s (mitigated by pre-pulling images on nodes). JVM startup: 3-5s (mitigated by CDS/AppCDS class data sharing). Spring Boot init: 5-8s (mitigated by lazy initialization). Connection establishment (RabbitMQ, MySQL, Redis): 1-2s. Total: ~15-25s. For Python workers (Celery), cold start is faster (~5-10s). We keep `minReplicaCount` > 0 for latency-sensitive queues to ensure at least one warm worker.

---

## 16. References

- [Celery Documentation: Architecture](https://docs.celeryq.dev/en/stable/getting-started/introduction.html)
- [RabbitMQ Quorum Queues](https://www.rabbitmq.com/quorum-queues.html)
- [RabbitMQ Dead Letter Exchanges](https://www.rabbitmq.com/dlx.html)
- [RabbitMQ Publisher Confirms](https://www.rabbitmq.com/confirms.html)
- [Sidekiq Best Practices](https://github.com/mperham/sidekiq/wiki/Best-Practices)
- [Spring AMQP Reference](https://docs.spring.io/spring-amqp/reference/html/)
- [HikariCP: About Pool Sizing](https://github.com/brettwooldridge/HikariCP/wiki/About-Pool-Sizing)
- [Token Bucket Algorithm](https://en.wikipedia.org/wiki/Token_bucket)
- [KEDA RabbitMQ Scaler](https://keda.sh/docs/2.12/scalers/rabbitmq-queue/)
- [OpenTelemetry AMQP Semantic Conventions](https://opentelemetry.io/docs/specs/semconv/messaging/rabbitmq/)
- [Redis Lua Scripting](https://redis.io/docs/interact/programmability/eval-intro/)
- [Spring Boot ThreadPoolTaskExecutor](https://docs.spring.io/spring-framework/reference/integration/scheduling.html)
