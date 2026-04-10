# System Design: Distributed Job Scheduler

> **Relevance to role:** A distributed job scheduler is the backbone of any bare-metal IaaS or cloud platform -- it orchestrates provisioning workflows, manages DAG-based infrastructure pipelines, and underpins services like Kubernetes CronJobs, Airflow DAGs, and Quartz-clustered schedulers that this role will own and operate at scale.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement | Detail |
|----|------------|--------|
| FR-1 | Job submission | Users submit jobs via API/CLI with execution spec (command, image, resources, schedule, dependencies) |
| FR-2 | DAG support | Jobs may declare dependencies forming a Directed Acyclic Graph; execution respects topological order |
| FR-3 | Scheduling modes | One-shot, recurring (cron), event-triggered, and dependency-triggered |
| FR-4 | Job lifecycle management | Full state machine: create, queue, dispatch, run, succeed, fail, retry, cancel, pause, resume |
| FR-5 | Retry & dead-letter | Configurable retry with exponential backoff; permanently failed jobs route to dead-letter queue |
| FR-6 | Distributed execution | Workers on multiple nodes; scheduler assigns jobs to available workers |
| FR-7 | Job observability | Real-time status, logs, execution history, DAG visualization |
| FR-8 | Multi-tenancy | Namespace isolation, per-tenant quotas, fair resource sharing |

### Non-Functional Requirements
| NFR | Target | Rationale |
|-----|--------|-----------|
| Availability | 99.99% for scheduler control plane | Missed schedules cause SLA violations |
| Consistency | At-least-once execution guarantee by default; exactly-once via idempotency tokens | Duplicate execution is safer than missed execution for most infra jobs |
| Scheduling latency | < 500ms from trigger time to dispatch | Jobs must start within their schedule window |
| Dispatch latency | < 2s from QUEUED to worker pickup | Workers should be saturated, not idle |
| Durability | Zero job loss | All accepted jobs must persist before acknowledgement |
| Scalability | 1M concurrent jobs, 100K submissions/min | Enterprise-scale infrastructure platform |
| Throughput | 10K job completions/sec across cluster | Sustained throughput for batch workloads |

### Constraints & Assumptions
- Deployment on bare-metal Kubernetes clusters (no cloud-managed services assumed)
- MySQL 8.0 as primary persistent store (existing organizational standard)
- Elasticsearch for job execution search and analytics
- Java 17+ for scheduler service; Python workers also supported
- etcd available for leader election and distributed coordination
- Internal network with < 1ms RTT between scheduler and workers within a datacenter
- Workers run on heterogeneous hardware (varying CPU, memory, GPU counts)

### Out of Scope
- Workflow UI designer (assume programmatic DAG definition)
- Data pipeline-specific semantics (e.g., Spark job management)
- Multi-region active-active scheduling (single-region with DR failover)
- Cost optimization / spot instance integration

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Total registered jobs | 10M | Platform serves ~500 teams, ~20K jobs/team |
| Active (enabled) jobs | 2M | ~20% active at any time |
| Job submissions/day | 50M | 2M recurring + 10M one-shot + 38M dependency-triggered |
| Peak submissions/min | 100K | 3x average during business-hour batch windows |
| Concurrent running jobs | 1M | Peak capacity across all worker pools |
| DAGs in system | 200K | Average 10 tasks/DAG |
| Workers | 10K | Mix of bare-metal nodes, each running 100 concurrent tasks |
| Scheduler instances | 5 | 1 active leader + 4 standby (2 hot, 2 warm) |

### Latency Requirements

| Operation | P50 | P99 | P99.9 |
|-----------|-----|-----|-------|
| Job submission API | 20ms | 100ms | 500ms |
| Schedule evaluation | 5ms | 50ms | 200ms |
| Job dispatch to worker | 50ms | 500ms | 2s |
| Job status query | 10ms | 50ms | 200ms |
| DAG status query | 50ms | 200ms | 1s |
| Job cancellation | 100ms | 500ms | 2s |

### Storage Estimates

| Data | Size per record | Count | Total | Retention |
|------|----------------|-------|-------|-----------|
| Job definitions | 2 KB | 10M | 20 GB | Indefinite |
| Job execution records | 1 KB | 50M/day | 50 GB/day | 90 days = 4.5 TB |
| Job logs (pointers) | 200 B | 50M/day | 10 GB/day | 90 days = 900 GB |
| DAG definitions | 5 KB | 200K | 1 GB | Indefinite |
| Worker state | 500 B | 10K | 5 MB | Real-time only |
| **Total MySQL** | | | **~25 GB active** | Partitioned |
| **Total ES (search)** | | | **~5.4 TB** | 90-day rollover |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Job submissions (peak) | 100K/min x 2 KB = 200 MB/min | 3.3 MB/s |
| Heartbeats | 10K workers x 1 KB x 1/10s = 1 MB/s | 1 MB/s |
| Status updates | 1M running x 1 KB x 1/30s = 33 MB/s | 33 MB/s |
| Log ingestion pointers | 50M/day x 200B / 86400 = 115 KB/s | 0.1 MB/s |
| **Total inbound** | | **~37 MB/s** |

---

## 3. High Level Architecture

```
                                    ┌─────────────────────┐
                                    │   API Gateway /      │
                                    │   Load Balancer      │
                                    └──────────┬──────────┘
                                               │
                        ┌──────────────────────┼──────────────────────┐
                        │                      │                      │
                  ┌─────▼─────┐         ┌──────▼──────┐        ┌─────▼─────┐
                  │ Scheduler │         │  Scheduler  │        │ Scheduler │
                  │ Instance 1│         │  Instance 2 │        │ Instance 3│
                  │ (LEADER)  │         │  (STANDBY)  │        │ (STANDBY) │
                  └─────┬─────┘         └─────────────┘        └───────────┘
                        │
           ┌────────────┼────────────┐
           │            │            │
     ┌─────▼────┐ ┌────▼─────┐ ┌───▼──────┐
     │ Schedule  │ │   DAG    │ │ Dispatch │
     │ Evaluator│ │ Resolver │ │  Engine  │
     └─────┬────┘ └────┬─────┘ └───┬──────┘
           │            │            │
           └────────────┼────────────┘
                        │
              ┌─────────▼─────────┐
              │   Job Queue       │
              │ (MySQL + Redis)   │
              └─────────┬─────────┘
                        │
         ┌──────────────┼──────────────┐
         │              │              │
   ┌─────▼─────┐ ┌─────▼─────┐ ┌─────▼─────┐
   │  Worker    │ │  Worker   │ │  Worker   │
   │  Pool A   │ │  Pool B   │ │  Pool C   │
   │ (General) │ │  (GPU)    │ │ (Hi-Mem)  │
   └─────┬─────┘ └─────┬─────┘ └─────┬─────┘
         │              │              │
         └──────────────┼──────────────┘
                        │
              ┌─────────▼─────────┐
              │  Heartbeat &      │
              │  Status Collector │
              └─────────┬─────────┘
                        │
         ┌──────────────┼──────────────┐
         │              │              │
   ┌─────▼─────┐ ┌─────▼─────┐ ┌─────▼──────┐
   │  MySQL    │ │  Redis    │ │Elasticsearch│
   │ (Primary  │ │ (Locks,  │ │ (Search,   │
   │  Store)   │ │  Cache)  │ │  Analytics)│
   └───────────┘ └──────────┘ └────────────┘
         │
   ┌─────▼──────┐
   │   etcd     │
   │ (Leader    │
   │  Election) │
   └────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **API Gateway** | Authenticates requests, rate limits, routes to scheduler leader |
| **Scheduler Instance (Leader)** | Evaluates schedules, resolves DAG dependencies, dispatches jobs; exactly one active leader |
| **Scheduler Instance (Standby)** | Hot-standby; participates in leader election; takes over on leader failure |
| **Schedule Evaluator** | Scans due jobs (cron triggers, time-based), marks them QUEUED |
| **DAG Resolver** | Evaluates dependency graphs; unblocks tasks whose predecessors completed |
| **Dispatch Engine** | Matches QUEUED jobs to available workers considering resource requirements |
| **Job Queue (MySQL + Redis)** | MySQL for durable job state; Redis sorted set for efficient next-to-run ordering |
| **Worker Pools** | Heterogeneous pools labeled by capability (general, GPU, high-memory) |
| **Heartbeat & Status Collector** | Receives worker heartbeats, detects timeouts, updates job status |
| **MySQL** | Primary source of truth for job definitions, execution records, DAG structure |
| **Redis** | Distributed locks, schedule sorted sets, dispatch queue, caching |
| **Elasticsearch** | Execution log search, job analytics, SLA dashboards |
| **etcd** | Leader election, configuration distribution, service discovery |

### Data Flows

**Primary: Job Submission and Execution**
1. Client submits job via API Gateway
2. Leader scheduler validates, persists to MySQL, returns job ID
3. Schedule Evaluator detects job is due (or immediate), marks QUEUED
4. DAG Resolver checks all dependencies are met
5. Dispatch Engine selects worker from matching pool, sends job spec
6. Worker executes, streams status updates back
7. On completion, Status Collector updates MySQL and triggers downstream DAG tasks

**Secondary: Failure Detection and Recovery**
1. Heartbeat Collector detects missing worker heartbeat (> 30s)
2. Marks worker as SUSPECT, waits one more interval
3. On confirmation, marks affected jobs as FAILED or RETRYING
4. Dispatch Engine re-queues retryable jobs
5. Dead-letter queue receives permanently failed jobs

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Job definition (the template)
CREATE TABLE job_definition (
    job_id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id           VARCHAR(64)     NOT NULL,
    job_name            VARCHAR(255)    NOT NULL,
    job_type            ENUM('ONE_SHOT','RECURRING','EVENT_TRIGGERED','DAG_TASK') NOT NULL,
    cron_expression     VARCHAR(128)    NULL,
    timezone            VARCHAR(64)     DEFAULT 'UTC',
    command             TEXT            NOT NULL,
    container_image     VARCHAR(512)    NULL,
    resource_request    JSON            NOT NULL COMMENT '{"cpu_millis":1000,"memory_mb":2048,"gpu":0}',
    max_retries         INT UNSIGNED    DEFAULT 3,
    retry_backoff_ms    INT UNSIGNED    DEFAULT 1000,
    timeout_seconds     INT UNSIGNED    DEFAULT 3600,
    priority            SMALLINT        DEFAULT 500 COMMENT '0=lowest, 1000=highest',
    tags                JSON            NULL,
    enabled             BOOLEAN         DEFAULT TRUE,
    idempotency_key     VARCHAR(128)    NULL,
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    updated_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    version             INT UNSIGNED    DEFAULT 1 COMMENT 'Optimistic locking',
    UNIQUE KEY uk_tenant_name (tenant_id, job_name),
    INDEX idx_type_enabled (job_type, enabled),
    INDEX idx_tenant (tenant_id),
    INDEX idx_priority (priority DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- DAG definition
CREATE TABLE dag_definition (
    dag_id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id           VARCHAR(64)     NOT NULL,
    dag_name            VARCHAR(255)    NOT NULL,
    description         TEXT            NULL,
    schedule            VARCHAR(128)    NULL COMMENT 'Cron expression for DAG trigger',
    timezone            VARCHAR(64)     DEFAULT 'UTC',
    max_concurrent_runs INT UNSIGNED    DEFAULT 1,
    enabled             BOOLEAN         DEFAULT TRUE,
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    updated_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    version             INT UNSIGNED    DEFAULT 1,
    UNIQUE KEY uk_tenant_dag (tenant_id, dag_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- DAG edges (dependency graph)
CREATE TABLE dag_edge (
    dag_id              BIGINT UNSIGNED NOT NULL,
    upstream_job_id     BIGINT UNSIGNED NOT NULL,
    downstream_job_id   BIGINT UNSIGNED NOT NULL,
    condition           ENUM('ON_SUCCESS','ON_FAILURE','ON_COMPLETE','ALWAYS') DEFAULT 'ON_SUCCESS',
    PRIMARY KEY (dag_id, upstream_job_id, downstream_job_id),
    INDEX idx_downstream (dag_id, downstream_job_id),
    FOREIGN KEY (dag_id) REFERENCES dag_definition(dag_id),
    FOREIGN KEY (upstream_job_id) REFERENCES job_definition(job_id),
    FOREIGN KEY (downstream_job_id) REFERENCES job_definition(job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Job execution record (one per attempt)
CREATE TABLE job_execution (
    execution_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    job_id              BIGINT UNSIGNED NOT NULL,
    dag_run_id          BIGINT UNSIGNED NULL,
    attempt_number      INT UNSIGNED    DEFAULT 1,
    status              ENUM('PENDING','QUEUED','DISPATCHED','RUNNING',
                             'SUCCEEDED','FAILED','RETRYING','CANCELLED',
                             'TIMED_OUT','DEAD_LETTERED') NOT NULL DEFAULT 'PENDING',
    worker_id           VARCHAR(128)    NULL,
    scheduled_at        TIMESTAMP(6)    NOT NULL,
    queued_at           TIMESTAMP(6)    NULL,
    dispatched_at       TIMESTAMP(6)    NULL,
    started_at          TIMESTAMP(6)    NULL,
    completed_at        TIMESTAMP(6)    NULL,
    exit_code           INT             NULL,
    error_message       TEXT            NULL,
    output_ref          VARCHAR(512)    NULL COMMENT 'S3/object-store path for output',
    idempotency_token   VARCHAR(128)    NULL,
    lock_version        INT UNSIGNED    DEFAULT 0 COMMENT 'Optimistic concurrency control',
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_job_status (job_id, status),
    INDEX idx_status_scheduled (status, scheduled_at),
    INDEX idx_worker (worker_id, status),
    INDEX idx_dag_run (dag_run_id, status),
    INDEX idx_queued_priority (status, scheduled_at) COMMENT 'For dispatch ordering',
    FOREIGN KEY (job_id) REFERENCES job_definition(job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(created_at)) (
    PARTITION p_2026_q1 VALUES LESS THAN (UNIX_TIMESTAMP('2026-04-01')),
    PARTITION p_2026_q2 VALUES LESS THAN (UNIX_TIMESTAMP('2026-07-01')),
    PARTITION p_2026_q3 VALUES LESS THAN (UNIX_TIMESTAMP('2026-10-01')),
    PARTITION p_2026_q4 VALUES LESS THAN (UNIX_TIMESTAMP('2027-01-01')),
    PARTITION p_future  VALUES LESS THAN MAXVALUE
);

-- DAG run instance
CREATE TABLE dag_run (
    dag_run_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    dag_id              BIGINT UNSIGNED NOT NULL,
    status              ENUM('RUNNING','SUCCEEDED','FAILED','CANCELLED') DEFAULT 'RUNNING',
    triggered_at        TIMESTAMP(6)    NOT NULL,
    completed_at        TIMESTAMP(6)    NULL,
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_dag_status (dag_id, status),
    FOREIGN KEY (dag_id) REFERENCES dag_definition(dag_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Worker registry
CREATE TABLE worker_node (
    worker_id           VARCHAR(128)    PRIMARY KEY,
    pool_name           VARCHAR(64)     NOT NULL,
    hostname            VARCHAR(255)    NOT NULL,
    ip_address          VARCHAR(45)     NOT NULL,
    total_cpu_millis    INT UNSIGNED    NOT NULL,
    total_memory_mb     INT UNSIGNED    NOT NULL,
    total_gpu           INT UNSIGNED    DEFAULT 0,
    available_cpu_millis INT UNSIGNED   NOT NULL,
    available_memory_mb INT UNSIGNED    NOT NULL,
    available_gpu       INT UNSIGNED    DEFAULT 0,
    status              ENUM('ACTIVE','DRAINING','SUSPECT','DEAD') DEFAULT 'ACTIVE',
    last_heartbeat      TIMESTAMP(6)    NOT NULL,
    registered_at       TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    labels              JSON            NULL,
    INDEX idx_pool_status (pool_name, status),
    INDEX idx_heartbeat (last_heartbeat)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Dead letter queue
CREATE TABLE dead_letter_job (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    execution_id        BIGINT UNSIGNED NOT NULL,
    job_id              BIGINT UNSIGNED NOT NULL,
    failure_reason      TEXT            NOT NULL,
    last_error          TEXT            NULL,
    total_attempts      INT UNSIGNED    NOT NULL,
    dead_lettered_at    TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    resolved            BOOLEAN         DEFAULT FALSE,
    resolved_at         TIMESTAMP(6)    NULL,
    resolved_by         VARCHAR(128)    NULL,
    INDEX idx_unresolved (resolved, dead_lettered_at),
    FOREIGN KEY (execution_id) REFERENCES job_execution(execution_id),
    FOREIGN KEY (job_id) REFERENCES job_definition(job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Criterion | MySQL 8.0 | PostgreSQL 15 | CockroachDB | TiDB |
|-----------|-----------|--------------|-------------|------|
| Operational expertise | Strong (org standard) | Moderate | Low | Low |
| ACID compliance | Full | Full | Full | Full |
| Partitioning | RANGE, LIST, HASH | Declarative | Automatic | Automatic |
| JSON support | Good (8.0+) | Excellent | Good | Good |
| Optimistic locking | `VERSION` column | Same | Same | Same |
| Replication | Semi-sync, Group Repl | Streaming | Raft built-in | Raft built-in |
| Connection pooling | ProxySQL / HikariCP | PgBouncer | Built-in | Built-in |
| Max connections (practical) | ~5K with ProxySQL | ~5K with PgBouncer | ~50K | ~50K |
| Operational cost | Low (existing) | Medium (new stack) | High | Medium |

**Selection: MySQL 8.0** -- Already the organizational standard, team has deep operational expertise, and with partitioning + read replicas handles the write throughput (50M executions/day = ~580 writes/sec average, ~5K/sec peak, well within MySQL capacity). Use semi-synchronous replication for durability.

### Indexing Strategy

| Index | Table | Purpose | Type |
|-------|-------|---------|------|
| `idx_status_scheduled` | job_execution | Dispatch queue scan: find QUEUED jobs ordered by scheduled_at | Composite B-tree |
| `idx_job_status` | job_execution | Job history lookup by status | Composite B-tree |
| `idx_worker` | job_execution | Find all jobs on a worker (for failure recovery) | Composite B-tree |
| `idx_dag_run` | job_execution | DAG completion check | Composite B-tree |
| `idx_type_enabled` | job_definition | Schedule evaluator scan | Composite B-tree |
| `idx_pool_status` | worker_node | Dispatch engine: find available workers | Composite B-tree |
| `idx_heartbeat` | worker_node | Heartbeat timeout detection | B-tree |
| `idx_unresolved` | dead_letter_job | Operator dashboard: pending DLQ items | Composite B-tree |

Covering indexes avoided on high-write tables to minimize write amplification. Partition pruning on `job_execution.created_at` keeps active working set small.

---

## 5. API Design

### REST API Endpoints

#### Job Management

```
POST /api/v1/jobs
Authorization: Bearer <JWT>
X-Tenant-Id: <tenant_id>
X-Idempotency-Key: <uuid>
Content-Type: application/json
Rate-Limit: 1000 req/min per tenant

Request:
{
  "job_name": "nightly-backup-db-primary",
  "job_type": "RECURRING",
  "cron_expression": "0 2 * * *",
  "timezone": "America/New_York",
  "command": "/opt/scripts/backup.sh --full",
  "container_image": "internal-registry/backup-agent:3.2.1",
  "resource_request": {
    "cpu_millis": 2000,
    "memory_mb": 4096,
    "gpu": 0
  },
  "max_retries": 3,
  "retry_backoff_ms": 5000,
  "timeout_seconds": 7200,
  "priority": 700,
  "tags": {"team": "dba", "env": "production"}
}

Response (201 Created):
{
  "job_id": 12345678,
  "job_name": "nightly-backup-db-primary",
  "status": "CREATED",
  "next_execution": "2026-04-10T06:00:00Z",
  "created_at": "2026-04-09T14:30:00.123456Z",
  "_links": {
    "self": "/api/v1/jobs/12345678",
    "executions": "/api/v1/jobs/12345678/executions",
    "cancel": "/api/v1/jobs/12345678/cancel"
  }
}
```

```
GET /api/v1/jobs/{job_id}
GET /api/v1/jobs?tenant_id=X&status=ENABLED&page=1&size=50
PUT /api/v1/jobs/{job_id}  (full update, version required for optimistic lock)
PATCH /api/v1/jobs/{job_id}  (partial update)
DELETE /api/v1/jobs/{job_id}
POST /api/v1/jobs/{job_id}/trigger  (immediate execution)
POST /api/v1/jobs/{job_id}/cancel
POST /api/v1/jobs/{job_id}/pause
POST /api/v1/jobs/{job_id}/resume
```

#### DAG Management

```
POST /api/v1/dags
Authorization: Bearer <JWT>
X-Tenant-Id: <tenant_id>
Content-Type: application/json

Request:
{
  "dag_name": "etl-pipeline-daily",
  "schedule": "0 3 * * *",
  "timezone": "UTC",
  "max_concurrent_runs": 1,
  "tasks": [
    {"job_id": 100, "depends_on": []},
    {"job_id": 101, "depends_on": [{"job_id": 100, "condition": "ON_SUCCESS"}]},
    {"job_id": 102, "depends_on": [{"job_id": 100, "condition": "ON_SUCCESS"}]},
    {"job_id": 103, "depends_on": [
      {"job_id": 101, "condition": "ON_SUCCESS"},
      {"job_id": 102, "condition": "ON_SUCCESS"}
    ]}
  ]
}

Response (201):
{
  "dag_id": 5001,
  "dag_name": "etl-pipeline-daily",
  "task_count": 4,
  "validated": true,
  "cycle_detected": false,
  "critical_path_length": 3
}
```

```
GET /api/v1/dags/{dag_id}
GET /api/v1/dags/{dag_id}/runs
GET /api/v1/dags/{dag_id}/runs/{run_id}
POST /api/v1/dags/{dag_id}/trigger
DELETE /api/v1/dags/{dag_id}
```

#### Execution & Status

```
GET /api/v1/jobs/{job_id}/executions?status=FAILED&limit=20

Response:
{
  "executions": [
    {
      "execution_id": 99887766,
      "attempt_number": 3,
      "status": "FAILED",
      "worker_id": "worker-pool-a-node-17",
      "scheduled_at": "2026-04-09T06:00:00Z",
      "started_at": "2026-04-09T06:00:01.234Z",
      "completed_at": "2026-04-09T06:05:32.891Z",
      "exit_code": 137,
      "error_message": "OOMKilled",
      "duration_ms": 331657
    }
  ],
  "pagination": {"page": 1, "size": 20, "total": 1}
}
```

#### gRPC: Worker-Scheduler Communication

```protobuf
service SchedulerWorkerService {
  // Worker pulls next job assignment
  rpc FetchJob(FetchJobRequest) returns (FetchJobResponse);
  
  // Worker reports heartbeat + status
  rpc Heartbeat(stream HeartbeatRequest) returns (stream HeartbeatResponse);
  
  // Worker reports job completion
  rpc ReportCompletion(CompletionRequest) returns (CompletionResponse);
}

message FetchJobRequest {
  string worker_id = 1;
  ResourceCapacity available = 2;
  repeated string labels = 3;
}

message HeartbeatRequest {
  string worker_id = 1;
  repeated RunningJobStatus running_jobs = 2;
  ResourceCapacity available = 3;
  int64 timestamp_ms = 4;
}
```

### CLI Design

```bash
# Job management
jobctl create -f job-spec.yaml                    # Create from YAML
jobctl create --name "my-job" --cmd "echo hello" --cron "*/5 * * * *"
jobctl list --tenant myteam --status ENABLED --limit 50
jobctl get <job_id> --output json|yaml|table
jobctl update <job_id> --priority 800 --max-retries 5
jobctl delete <job_id> [--force]
jobctl trigger <job_id>                           # Immediate execution
jobctl pause <job_id>
jobctl resume <job_id>
jobctl cancel <job_id> [--execution <exec_id>]    # Cancel specific or latest

# DAG management
jobctl dag create -f dag-spec.yaml
jobctl dag list --tenant myteam
jobctl dag get <dag_id> --show-graph               # ASCII DAG visualization
jobctl dag trigger <dag_id>
jobctl dag runs <dag_id> --limit 10

# Execution history
jobctl history <job_id> --limit 20 --status FAILED
jobctl logs <execution_id> [--follow] [--tail 100]
jobctl status <execution_id>

# Dead letter queue
jobctl dlq list --unresolved --limit 50
jobctl dlq retry <dlq_id>                         # Re-enqueue
jobctl dlq resolve <dlq_id> --reason "manual fix applied"

# Worker management (admin)
jobctl worker list --pool general --status ACTIVE
jobctl worker drain <worker_id>                   # Graceful drain
jobctl worker cordon <worker_id>                  # Stop new assignments

# Admin
jobctl admin stats                                # Cluster-wide statistics
jobctl admin leader                               # Show current leader
jobctl admin rebalance --pool general             # Redistribute jobs

# Output:
# jobctl list --tenant myteam
# JOB_ID     NAME                  TYPE       SCHEDULE       STATUS   PRIORITY  LAST_RUN
# 12345678   nightly-backup-db     RECURRING  0 2 * * *      ENABLED  700       2026-04-09T02:00Z (OK)
# 12345679   health-check          RECURRING  */5 * * * *    ENABLED  500       2026-04-09T14:25Z (OK)
# 12345680   quarterly-report      ONE_SHOT   -              PAUSED   300       2026-03-31T00:00Z (FAIL)
```

---

## 6. Core Component Deep Dives

### 6.1 DAG Resolution Engine

**Why it's hard:** DAGs can have hundreds of tasks with complex dependency patterns. The engine must detect cycles, compute execution order, handle partial failures (skip downstream vs. fail-fast), and support dynamic DAG modification at runtime. Concurrent DAG runs of the same definition must be isolated.

**Approaches Compared:**

| Approach | Cycle Detection | Dynamic Modification | Complexity | Memory | Fault Tolerance |
|----------|----------------|---------------------|------------|--------|-----------------|
| Kahn's Algorithm (BFS topological sort) | O(V+E) at submission | Requires re-validation | Low | O(V) in-degree array | Stateless, re-computable |
| DFS topological sort | O(V+E) | Same | Low | O(V) recursion stack | Risk of stack overflow on deep DAGs |
| Adjacency matrix | O(V^3) transitive closure | Easy column ops | High | O(V^2) | Memory-heavy |
| Event-driven reactive | Implicit via event propagation | Natural | Medium | O(E) subscriptions | Harder to debug |

**Selected: Kahn's Algorithm (BFS-based topological sort)** -- O(V+E) time and O(V) space. Naturally detects cycles (if sorted count < V, cycle exists). Produces a valid execution order. Easy to parallelize: all nodes with in-degree 0 at any step can execute concurrently.

**Implementation:**

```python
# DAG Resolution with Kahn's Algorithm
from collections import deque, defaultdict
from typing import List, Set, Dict, Optional
from enum import Enum

class TaskStatus(Enum):
    PENDING = "PENDING"
    READY = "READY"         # All dependencies met
    RUNNING = "RUNNING"
    SUCCEEDED = "SUCCEEDED"
    FAILED = "FAILED"
    SKIPPED = "SKIPPED"

class DAGResolver:
    def __init__(self, dag_id: int, edges: List[tuple], conditions: Dict):
        """
        edges: [(upstream_job_id, downstream_job_id), ...]
        conditions: {(upstream, downstream): "ON_SUCCESS"|"ON_FAILURE"|...}
        """
        self.dag_id = dag_id
        self.adj = defaultdict(set)        # upstream -> {downstream}
        self.reverse_adj = defaultdict(set) # downstream -> {upstream}
        self.in_degree = defaultdict(int)
        self.conditions = conditions
        self.all_tasks: Set[int] = set()
        
        for u, d in edges:
            self.adj[u].add(d)
            self.reverse_adj[d].add(u)
            self.in_degree[d] += 1
            self.all_tasks.add(u)
            self.all_tasks.add(d)
        
        # Tasks with no incoming edges start with in_degree 0
        for task in self.all_tasks:
            if task not in self.in_degree:
                self.in_degree[task] = 0

    def validate_no_cycles(self) -> bool:
        """Returns True if DAG is valid (no cycles)."""
        in_deg = dict(self.in_degree)
        queue = deque([t for t in self.all_tasks if in_deg.get(t, 0) == 0])
        count = 0
        while queue:
            node = queue.popleft()
            count += 1
            for neighbor in self.adj[node]:
                in_deg[neighbor] -= 1
                if in_deg[neighbor] == 0:
                    queue.append(neighbor)
        return count == len(self.all_tasks)

    def get_ready_tasks(self, task_statuses: Dict[int, TaskStatus]) -> List[int]:
        """Given current task statuses, return tasks ready to execute."""
        ready = []
        for task in self.all_tasks:
            if task_statuses.get(task) != TaskStatus.PENDING:
                continue  # Already running, done, or skipped
            
            # Check all upstream dependencies
            all_deps_met = True
            for upstream in self.reverse_adj[task]:
                upstream_status = task_statuses.get(upstream, TaskStatus.PENDING)
                condition = self.conditions.get((upstream, task), "ON_SUCCESS")
                
                if condition == "ON_SUCCESS" and upstream_status != TaskStatus.SUCCEEDED:
                    all_deps_met = False
                    break
                elif condition == "ON_FAILURE" and upstream_status != TaskStatus.FAILED:
                    all_deps_met = False
                    break
                elif condition == "ON_COMPLETE" and upstream_status not in (
                    TaskStatus.SUCCEEDED, TaskStatus.FAILED
                ):
                    all_deps_met = False
                    break
                elif condition == "ALWAYS" and upstream_status in (
                    TaskStatus.PENDING, TaskStatus.READY, TaskStatus.RUNNING
                ):
                    all_deps_met = False
                    break
            
            # Also check: no upstream is PENDING or RUNNING for this task's deps
            if all_deps_met and not self.reverse_adj[task]:
                # Root task, no dependencies
                ready.append(task)
            elif all_deps_met:
                ready.append(task)
        
        return ready

    def should_skip_downstream(self, failed_task: int, 
                                task_statuses: Dict[int, TaskStatus]) -> Set[int]:
        """Determine which tasks to SKIP when a task fails."""
        to_skip = set()
        queue = deque()
        
        for downstream in self.adj[failed_task]:
            condition = self.conditions.get((failed_task, downstream), "ON_SUCCESS")
            if condition == "ON_SUCCESS":
                to_skip.add(downstream)
                queue.append(downstream)
        
        # Cascade: skip transitive dependents
        while queue:
            node = queue.popleft()
            for downstream in self.adj[node]:
                if downstream not in to_skip:
                    to_skip.add(downstream)
                    queue.append(downstream)
        
        return to_skip
```

```java
// Java equivalent: DAG resolution in Spring Boot scheduler service
@Service
public class DAGResolverService {
    
    public List<Long> getReadyTasks(long dagRunId) {
        DagRun run = dagRunRepository.findById(dagRunId)
            .orElseThrow(() -> new NotFoundException("DAG run not found"));
        
        List<DagEdge> edges = dagEdgeRepository.findByDagId(run.getDagId());
        Map<Long, TaskStatus> statuses = jobExecutionRepository
            .findByDagRunId(dagRunId)
            .stream()
            .collect(Collectors.toMap(
                JobExecution::getJobId,
                JobExecution::getStatus,
                (a, b) -> b  // latest attempt wins
            ));
        
        // Build in-degree map
        Map<Long, Set<Long>> reverseAdj = new HashMap<>();
        Set<Long> allTasks = new HashSet<>();
        
        for (DagEdge edge : edges) {
            reverseAdj.computeIfAbsent(edge.getDownstreamJobId(), k -> new HashSet<>())
                       .add(edge.getUpstreamJobId());
            allTasks.add(edge.getUpstreamJobId());
            allTasks.add(edge.getDownstreamJobId());
        }
        
        return allTasks.stream()
            .filter(taskId -> statuses.getOrDefault(taskId, TaskStatus.PENDING) == TaskStatus.PENDING)
            .filter(taskId -> {
                Set<Long> deps = reverseAdj.getOrDefault(taskId, Collections.emptySet());
                return deps.stream().allMatch(depId -> 
                    statuses.getOrDefault(depId, TaskStatus.PENDING) == TaskStatus.SUCCEEDED
                );
            })
            .collect(Collectors.toList());
    }
}
```

**Failure Modes:**
- **Cycle introduced via concurrent modification:** Validate DAG on every mutation; reject if cycle detected.
- **Stuck DAG (task never completes):** Timeout watchdog marks timed-out tasks as FAILED, triggering skip/retry cascade.
- **Dependency on deleted job:** DAG validation on each trigger; orphaned edges detected and reported.
- **Concurrent DAG runs conflicting:** Each run gets isolated `dag_run_id`; task statuses are per-run.

**Interviewer Q&A:**

**Q1: How do you handle dynamic DAGs where tasks are added at runtime (like Airflow's dynamic task mapping)?**
A: We support a `POST /api/v1/dags/{dag_id}/runs/{run_id}/tasks` endpoint that adds tasks mid-run. The DAG Resolver re-validates acyclicity on each addition. New tasks are added with `PENDING` status and the resolver recalculates ready tasks. We enforce that dynamic tasks can only be added as downstream of existing completed tasks to prevent reordering already-dispatched work.

**Q2: What happens if the scheduler crashes mid-DAG-resolution?**
A: DAG state is fully derived from the `job_execution` table. When the new leader takes over, it re-queries all active DAG runs, recomputes ready tasks from current statuses, and resumes. No in-memory state is authoritative -- MySQL is the source of truth. This makes recovery idempotent.

**Q3: How do you optimize DAG resolution for very large DAGs (1000+ tasks)?**
A: We cache the adjacency structure in Redis (keyed by dag_id, invalidated on mutation). The ready-task computation is O(V+E) but we prune by only examining tasks in PENDING state and their direct upstream. For DAGs with many completed tasks, the working set shrinks rapidly.

**Q4: How do you handle conditional branches (run task B only if task A fails)?**
A: The `dag_edge.condition` field supports ON_SUCCESS, ON_FAILURE, ON_COMPLETE, and ALWAYS. The resolver evaluates each edge's condition against the upstream task's terminal status. This enables error-handling branches, cleanup tasks, and notification flows.

**Q5: Can a task appear in multiple DAGs?**
A: Yes, job_definition is independent of dag_definition. A task can be referenced in multiple DAGs. Each DAG run creates its own job_execution records, so there's no conflict. The job_definition is a template; executions are per-run instances.

**Q6: How do you visualize the critical path of a DAG?**
A: We compute the longest path using a modified topological sort that tracks cumulative estimated duration. The critical path determines the minimum DAG completion time. We expose this via `GET /api/v1/dags/{dag_id}/critical-path` and display it in the CLI with `jobctl dag get --show-graph`.

---

### 6.2 Job State Machine & Execution Lifecycle

**Why it's hard:** In a distributed system, state transitions must be atomic and consistent despite network partitions, worker crashes, and scheduler failovers. Lost or duplicate transitions can cause jobs to run twice or never complete.

**Approaches Compared:**

| Approach | Consistency | Performance | Complexity | Observability |
|----------|------------|-------------|------------|---------------|
| In-memory FSM + async DB persist | Weak (state can be lost) | Excellent | Low | Poor (gap between memory and DB) |
| Database-backed FSM with optimistic locking | Strong | Good | Medium | Excellent |
| Event-sourced state machine | Strong + auditable | Good (append-only) | High | Excellent |
| Distributed state machine (Raft consensus) | Strongest | Lower (consensus overhead) | Very High | Good |

**Selected: Database-backed FSM with optimistic locking** -- Every state transition is an UPDATE with a WHERE clause on `lock_version`. If two processes try to transition the same execution concurrently, only one succeeds. The other gets 0 rows affected and must re-read and retry or abort. This leverages MySQL's row-level locking and is simple to reason about.

**State Machine:**

```
                    ┌──────────┐
                    │ PENDING  │ (Created, waiting for schedule)
                    └────┬─────┘
                         │ schedule_evaluator: cron fires / immediate
                    ┌────▼─────┐
                    │ QUEUED   │ (Ready for dispatch)
                    └────┬─────┘
                         │ dispatch_engine: worker selected
                    ┌────▼──────┐
                    │DISPATCHED │ (Sent to worker, awaiting ACK)
                    └────┬──────┘
                         │ worker: ACK received
                    ┌────▼─────┐
                    │ RUNNING  │ (Worker executing)
                    └──┬───┬───┘
                       │   │
           ┌───────────┘   └────────────┐
           │                            │
     ┌─────▼─────┐              ┌──────▼──────┐
     │ SUCCEEDED │              │   FAILED    │
     └───────────┘              └──────┬──────┘
                                       │ retries remaining?
                                  ┌────▼─────┐     ┌──────────────┐
                                  │ RETRYING │────►│ DEAD_LETTERED│
                                  └────┬─────┘     └──────────────┘
                                       │               (no retries left)
                                       │ back to queue
                                  ┌────▼─────┐
                                  │ QUEUED   │
                                  └──────────┘

  CANCELLED ← can be reached from PENDING, QUEUED, DISPATCHED, RUNNING
  TIMED_OUT ← from RUNNING (timeout_seconds exceeded)
```

**Implementation:**

```java
@Service
@Transactional
public class JobStateMachine {

    private static final Map<JobStatus, Set<JobStatus>> VALID_TRANSITIONS = Map.of(
        JobStatus.PENDING,     Set.of(JobStatus.QUEUED, JobStatus.CANCELLED),
        JobStatus.QUEUED,      Set.of(JobStatus.DISPATCHED, JobStatus.CANCELLED),
        JobStatus.DISPATCHED,  Set.of(JobStatus.RUNNING, JobStatus.QUEUED, JobStatus.CANCELLED),
        JobStatus.RUNNING,     Set.of(JobStatus.SUCCEEDED, JobStatus.FAILED, 
                                       JobStatus.TIMED_OUT, JobStatus.CANCELLED),
        JobStatus.FAILED,      Set.of(JobStatus.RETRYING, JobStatus.DEAD_LETTERED),
        JobStatus.RETRYING,    Set.of(JobStatus.QUEUED)
    );

    @Autowired
    private JobExecutionRepository executionRepo;
    
    @Autowired
    private DeadLetterRepository dlqRepo;
    
    @Autowired
    private MeterRegistry metrics;

    public boolean transition(long executionId, JobStatus targetStatus, 
                               String workerId, String errorMessage) {
        
        JobExecution exec = executionRepo.findById(executionId)
            .orElseThrow(() -> new NotFoundException("Execution not found: " + executionId));
        
        // Validate transition
        Set<JobStatus> allowed = VALID_TRANSITIONS.getOrDefault(exec.getStatus(), Set.of());
        if (!allowed.contains(targetStatus)) {
            throw new InvalidStateTransitionException(
                "Cannot transition from " + exec.getStatus() + " to " + targetStatus);
        }
        
        // Optimistic locking: UPDATE ... WHERE lock_version = :expected
        int currentVersion = exec.getLockVersion();
        
        // Set timestamps based on target status
        switch (targetStatus) {
            case QUEUED:
                exec.setQueuedAt(Instant.now());
                break;
            case DISPATCHED:
                exec.setDispatchedAt(Instant.now());
                exec.setWorkerId(workerId);
                break;
            case RUNNING:
                exec.setStartedAt(Instant.now());
                break;
            case SUCCEEDED:
            case FAILED:
            case TIMED_OUT:
            case CANCELLED:
                exec.setCompletedAt(Instant.now());
                exec.setErrorMessage(errorMessage);
                break;
        }
        
        exec.setStatus(targetStatus);
        exec.setLockVersion(currentVersion + 1);
        
        int updated = executionRepo.updateWithOptimisticLock(
            exec.getExecutionId(), targetStatus, exec.getQueuedAt(),
            exec.getDispatchedAt(), exec.getStartedAt(), exec.getCompletedAt(),
            exec.getWorkerId(), exec.getErrorMessage(), 
            currentVersion + 1, currentVersion  // new version, expected version
        );
        
        if (updated == 0) {
            metrics.counter("job.state.transition.conflict").increment();
            throw new OptimisticLockException("Concurrent modification on execution " + executionId);
        }
        
        metrics.counter("job.state.transition", 
            "from", exec.getStatus().name(), "to", targetStatus.name()).increment();
        
        // Handle terminal states
        if (targetStatus == JobStatus.FAILED) {
            handleFailure(exec);
        }
        
        return true;
    }
    
    private void handleFailure(JobExecution exec) {
        JobDefinition job = jobDefRepo.findById(exec.getJobId()).orElseThrow();
        
        if (exec.getAttemptNumber() < job.getMaxRetries()) {
            // Schedule retry with exponential backoff
            long backoffMs = job.getRetryBackoffMs() * (long) Math.pow(2, exec.getAttemptNumber() - 1);
            backoffMs = Math.min(backoffMs, 300_000); // Cap at 5 minutes
            
            transition(exec.getExecutionId(), JobStatus.RETRYING, null, null);
            
            // Create new execution for retry
            JobExecution retry = new JobExecution();
            retry.setJobId(exec.getJobId());
            retry.setDagRunId(exec.getDagRunId());
            retry.setAttemptNumber(exec.getAttemptNumber() + 1);
            retry.setStatus(JobStatus.PENDING);
            retry.setScheduledAt(Instant.now().plusMillis(backoffMs));
            executionRepo.save(retry);
        } else {
            // Dead letter
            transition(exec.getExecutionId(), JobStatus.DEAD_LETTERED, null, "Max retries exhausted");
            
            DeadLetterJob dlj = new DeadLetterJob();
            dlj.setExecutionId(exec.getExecutionId());
            dlj.setJobId(exec.getJobId());
            dlj.setFailureReason("Exhausted " + job.getMaxRetries() + " retries");
            dlj.setLastError(exec.getErrorMessage());
            dlj.setTotalAttempts(exec.getAttemptNumber());
            dlqRepo.save(dlj);
        }
    }
}
```

**Failure Modes:**
- **State stuck in DISPATCHED (worker never ACKs):** Dispatch timeout (30s); if no ACK, revert to QUEUED.
- **State stuck in RUNNING (worker dies):** Heartbeat timeout triggers TIMED_OUT or FAILED.
- **Duplicate state transition (network retry):** Optimistic lock rejects second attempt; idempotency token on worker responses prevents double-completion.
- **Scheduler crash during transition:** MySQL transaction ensures atomicity; incomplete transition rolls back.

**Interviewer Q&A:**

**Q1: How do you guarantee exactly-once execution?**
A: We guarantee at-least-once by default. For exactly-once, jobs must be idempotent. We provide an `idempotency_token` (execution_id + attempt_number hash) that workers can use to deduplicate side effects. For critical jobs, we support a two-phase protocol: the worker writes a completion record to a shared store before reporting success; the scheduler checks this record before allowing re-dispatch.

**Q2: What if a worker completes a job but the completion message is lost?**
A: The worker retries the completion RPC with the same idempotency token. The scheduler checks if the execution is already in a terminal state; if so, it ACKs without re-processing. If the worker crashes after completion but before reporting, the heartbeat timeout will mark the job as failed, leading to a retry. The idempotent job will detect the prior completion and short-circuit.

**Q3: How do you handle the DISPATCHED -> RUNNING gap?**
A: We set a `dispatch_timeout` (default 30s). If the worker doesn't send a RUNNING heartbeat within this window, the scheduler reverts to QUEUED and redispatches. This handles cases where the dispatch message was lost or the worker crashed between receiving the job and starting it.

**Q4: Can you roll back a state transition?**
A: We never roll back state transitions -- the FSM is forward-only. Instead, we create compensating transitions. For example, if we need to re-queue a DISPATCHED job, we transition DISPATCHED -> QUEUED (an explicit allowed transition). This maintains a clean audit trail.

**Q5: How do you audit state transitions?**
A: Every transition is logged to an append-only `job_state_log` table with the old status, new status, actor (scheduler/worker/user), timestamp, and reason. This table is also streamed to Elasticsearch for searchable audit trails.

**Q6: What happens if the database is temporarily unavailable during a state transition?**
A: The transition fails and the caller receives an error. The worker keeps the job running and retries the status report. The scheduler's heartbeat mechanism serves as a backup: even if individual status reports are lost, the heartbeat includes the list of running jobs, allowing the scheduler to reconcile state when the database recovers.

---

### 6.3 Distributed Coordination & Leader Election

**Why it's hard:** Multiple scheduler instances must agree on exactly one leader to prevent duplicate job dispatching. Leader election must be fast, reliable, and handle network partitions gracefully. A split-brain scenario where two nodes believe they're leader would cause duplicate executions.

**Approaches Compared:**

| Approach | Failover Time | Split-Brain Risk | Complexity | External Dependency |
|----------|--------------|------------------|------------|-------------------|
| etcd lease-based election | 5-15s | Very Low (Raft consensus) | Low | etcd cluster |
| ZooKeeper ephemeral nodes | 5-30s | Very Low (ZAB protocol) | Medium | ZK ensemble |
| MySQL GET_LOCK() | 10-60s | Medium (single point) | Low | MySQL (already have) |
| Raft embedded (e.g., JRaft) | 1-5s | None (built-in) | High | None |

**Selected: etcd lease-based election** -- Already present in the Kubernetes environment for kube-apiserver. Provides strong consistency via Raft. Lease-based approach means leadership is automatically revoked if the leader stops renewing its lease (heartbeat failure). Failover is bounded by the lease TTL.

**Implementation:**

```java
@Component
public class EtcdLeaderElector implements SmartLifecycle {
    
    private static final String ELECTION_KEY = "/scheduler/leader";
    private static final long LEASE_TTL_SECONDS = 15;
    private static final long RENEWAL_INTERVAL_MS = 5000;
    
    private final Client etcdClient;
    private final String instanceId;
    private final AtomicBoolean isLeader = new AtomicBoolean(false);
    private final ScheduledExecutorService renewalExecutor = 
        Executors.newSingleThreadScheduledExecutor(
            new ThreadFactoryBuilder().setNameFormat("leader-renewal-%d").setDaemon(true).build()
        );
    private volatile long leaseId;
    private volatile boolean running = false;
    
    @Autowired
    private LeadershipCallback callback;
    
    @Autowired
    private MeterRegistry metrics;
    
    public EtcdLeaderElector(Client etcdClient, 
                              @Value("${scheduler.instance-id}") String instanceId) {
        this.etcdClient = etcdClient;
        this.instanceId = instanceId;
    }
    
    @Override
    public void start() {
        running = true;
        campaign();
    }
    
    private void campaign() {
        CompletableFuture.runAsync(() -> {
            while (running) {
                try {
                    // 1. Create lease
                    LeaseGrantResponse lease = etcdClient.getLeaseClient()
                        .grant(LEASE_TTL_SECONDS).get();
                    this.leaseId = lease.getID();
                    
                    // 2. Try to acquire leadership (put-if-absent)
                    ByteSequence key = ByteSequence.from(ELECTION_KEY, StandardCharsets.UTF_8);
                    ByteSequence value = ByteSequence.from(instanceId, StandardCharsets.UTF_8);
                    
                    TxnResponse txn = etcdClient.getKVClient().txn()
                        .If(new Cmp(key, Cmp.Op.EQUAL, CmpTarget.version(0))) // key doesn't exist
                        .Then(Op.put(key, value, PutOption.newBuilder().withLeaseId(leaseId).build()))
                        .Else(Op.get(key, GetOption.DEFAULT))
                        .commit().get();
                    
                    if (txn.isSucceeded()) {
                        // Won the election
                        becomeLeader();
                    } else {
                        // Someone else is leader -- watch for changes
                        String currentLeader = txn.getGetResponses().get(0)
                            .getKvs().get(0).getValue().toString(StandardCharsets.UTF_8);
                        log.info("Lost election. Current leader: {}", currentLeader);
                        metrics.gauge("scheduler.is_leader", 0);
                        
                        // Watch for leader key deletion (leader failure)
                        watchForLeaderChange(key);
                    }
                } catch (Exception e) {
                    log.error("Leader election error, retrying in 5s", e);
                    sleepSafe(5000);
                }
            }
        });
    }
    
    private void becomeLeader() throws Exception {
        isLeader.set(true);
        metrics.gauge("scheduler.is_leader", 1);
        log.info("This instance ({}) is now the LEADER", instanceId);
        
        // Start lease keep-alive
        renewalExecutor.scheduleAtFixedRate(() -> {
            try {
                etcdClient.getLeaseClient().keepAliveOnce(leaseId).get();
            } catch (Exception e) {
                log.error("Lease renewal failed -- stepping down", e);
                stepDown();
            }
        }, RENEWAL_INTERVAL_MS, RENEWAL_INTERVAL_MS, TimeUnit.MILLISECONDS);
        
        // Notify callback to start scheduling
        callback.onBecomeLeader();
        
        // Block while leader (lease will expire if we crash)
        while (isLeader.get() && running) {
            sleepSafe(1000);
        }
    }
    
    private void stepDown() {
        if (isLeader.compareAndSet(true, false)) {
            log.warn("Stepping down from leadership");
            metrics.gauge("scheduler.is_leader", 0);
            callback.onLoseLeadership();
            // Re-enter campaign loop
            campaign();
        }
    }
    
    public boolean isLeader() {
        return isLeader.get();
    }
}
```

**Failure Modes:**
- **etcd cluster unavailable:** Leader cannot renew lease; after TTL expires, all schedulers lose leadership. System pauses scheduling (safe). Jobs already dispatched continue running.
- **Network partition (scheduler isolated from etcd but can reach workers):** Lease expires, scheduler steps down. Fencing token (lease ID included in dispatch messages) prevents stale leader from dispatching.
- **Split-brain:** Prevented by etcd's Raft consensus. The put-if-absent transaction ensures only one writer at a time. Even if a stale leader tries to dispatch, workers validate the fencing token against etcd.

**Interviewer Q&A:**

**Q1: Why not use MySQL's GET_LOCK for leader election since you already have MySQL?**
A: MySQL GET_LOCK is tied to a single connection. If the connection drops (network blip), the lock is released immediately, causing unnecessary failovers. etcd leases have a configurable TTL providing a grace period. Also, GET_LOCK doesn't support watching for changes; standby nodes would have to poll. Finally, MySQL is a single point of failure for leader election, whereas etcd is a Raft-based cluster.

**Q2: What is a fencing token and why do you need one?**
A: A fencing token is a monotonically increasing value (we use the etcd lease ID or a revision number) included in every dispatch message. Workers reject dispatches with a fencing token lower than the highest they've seen. This prevents a stale leader (whose lease expired but who hasn't realized it yet) from issuing conflicting dispatches. Without fencing, a slow leader could dispatch a job that the new leader also dispatches.

**Q3: What's your lease TTL trade-off?**
A: Shorter TTL (5s) means faster failover but more frequent renewals and higher risk of false failover during GC pauses or network jitter. Longer TTL (30s) means fewer false failovers but longer recovery time. We chose 15s as a balance: 3 renewal attempts (every 5s) before expiry, and failover within 15-20s which is acceptable for our 500ms scheduling latency SLA.

**Q4: How do standby schedulers stay warm?**
A: Standby schedulers maintain their connection pools (HikariCP to MySQL, gRPC channels to workers), cache DAG structures, and subscribe to the job_execution change stream. They do everything except dispatch. When they become leader, they can start dispatching immediately without cold-start overhead.

**Q5: Can you run multiple active schedulers instead of leader/standby?**
A: Yes, with partitioned scheduling: each scheduler owns a subset of tenants or job ID ranges. This is the multi-leader approach. It increases throughput but adds complexity: partition assignment, rebalancing on node join/leave, and cross-partition DAGs. We start with single-leader for simplicity and switch to partitioned when we exceed 100K dispatches/min.

**Q6: How do you handle JVM GC pauses that might cause lease renewal failure?**
A: We tune G1GC for low-pause-time (MaxGCPauseMillis=200ms), use large heap (16GB+) with ample headroom, and set the renewal interval (5s) well above worst-case GC pauses. The lease TTL (15s) provides 3 renewal windows. We also run the renewal thread at high priority and monitor GC pause duration as a metric with alerting at > 2s.

---

### 6.4 Worker Heartbeat & Timeout Detection

**Why it's hard:** Workers may fail silently (OOM kill, hardware failure, network partition). The scheduler must detect these failures promptly without false positives (a busy worker is not a dead worker). Heartbeat storms from 10K workers must not overwhelm the scheduler.

**Approaches Compared:**

| Approach | Detection Latency | False Positive Rate | Scalability | Complexity |
|----------|------------------|--------------------|----|------------|
| Pull-based polling (scheduler queries workers) | High (proportional to worker count) | Low | Poor (N queries per cycle) | Low |
| Push-based heartbeat (workers push to scheduler) | Medium (bounded by heartbeat interval) | Medium | Good (workers self-report) | Low |
| Streaming gRPC bidirectional | Low (real-time) | Low | Excellent | Medium |
| Gossip protocol (SWIM) | Medium | Low | Excellent | High |

**Selected: Streaming gRPC bidirectional** -- Each worker maintains a persistent bidirectional gRPC stream to the scheduler. Heartbeats flow on the stream with negligible overhead. Connection drop is immediately detected. For 10K workers, this is 10K long-lived gRPC streams, well within modern server capacity (each stream is ~100 bytes/10s).

**Implementation:**

```java
@Service
public class HeartbeatManager {
    
    private static final Duration HEARTBEAT_INTERVAL = Duration.ofSeconds(10);
    private static final Duration SUSPECT_THRESHOLD = Duration.ofSeconds(30);
    private static final Duration DEAD_THRESHOLD = Duration.ofSeconds(60);
    
    // worker_id -> last heartbeat info
    private final ConcurrentHashMap<String, WorkerHeartbeatState> workerStates = 
        new ConcurrentHashMap<>();
    
    @Autowired
    private WorkerNodeRepository workerRepo;
    
    @Autowired
    private JobExecutionRepository executionRepo;
    
    @Autowired
    private JobStateMachine stateMachine;
    
    @Autowired
    private MeterRegistry metrics;
    
    /**
     * Called by gRPC stream handler when heartbeat arrives.
     */
    public void processHeartbeat(HeartbeatRequest request) {
        String workerId = request.getWorkerId();
        Instant now = Instant.now();
        
        WorkerHeartbeatState state = workerStates.compute(workerId, (k, existing) -> {
            if (existing == null) {
                return new WorkerHeartbeatState(workerId, now, WorkerStatus.ACTIVE, 
                                                  request.getAvailable());
            }
            existing.setLastHeartbeat(now);
            existing.setAvailable(request.getAvailable());
            if (existing.getStatus() == WorkerStatus.SUSPECT) {
                existing.setStatus(WorkerStatus.ACTIVE);
                log.info("Worker {} recovered from SUSPECT state", workerId);
            }
            return existing;
        });
        
        // Reconcile running jobs reported by worker vs our records
        reconcileRunningJobs(workerId, request.getRunningJobsList());
        
        // Update available resources in DB (batched, not every heartbeat)
        if (state.shouldUpdateDb(now)) {
            workerRepo.updateResources(workerId, request.getAvailable().getCpuMillis(),
                request.getAvailable().getMemoryMb(), request.getAvailable().getGpu(), now);
            state.setLastDbUpdate(now);
        }
        
        metrics.gauge("worker.heartbeat.lag_ms", 
            Duration.between(Instant.ofEpochMilli(request.getTimestampMs()), now).toMillis());
    }
    
    /**
     * Periodic scanner: detect suspect and dead workers.
     * Runs every 10 seconds on the leader scheduler.
     */
    @Scheduled(fixedRate = 10_000)
    public void detectFailedWorkers() {
        if (!leaderElector.isLeader()) return;
        
        Instant now = Instant.now();
        
        workerStates.forEach((workerId, state) -> {
            Duration sinceLastHeartbeat = Duration.between(state.getLastHeartbeat(), now);
            
            if (sinceLastHeartbeat.compareTo(DEAD_THRESHOLD) > 0 
                    && state.getStatus() != WorkerStatus.DEAD) {
                // Confirmed dead
                markWorkerDead(workerId);
                
            } else if (sinceLastHeartbeat.compareTo(SUSPECT_THRESHOLD) > 0 
                    && state.getStatus() == WorkerStatus.ACTIVE) {
                // Suspect -- first warning
                state.setStatus(WorkerStatus.SUSPECT);
                workerRepo.updateStatus(workerId, WorkerStatus.SUSPECT);
                log.warn("Worker {} is SUSPECT (no heartbeat for {}s)", 
                    workerId, sinceLastHeartbeat.getSeconds());
                metrics.counter("worker.suspect").increment();
            }
        });
    }
    
    private void markWorkerDead(String workerId) {
        WorkerHeartbeatState state = workerStates.get(workerId);
        state.setStatus(WorkerStatus.DEAD);
        workerRepo.updateStatus(workerId, WorkerStatus.DEAD);
        
        log.error("Worker {} declared DEAD. Recovering jobs.", workerId);
        metrics.counter("worker.dead").increment();
        
        // Find all jobs assigned to this worker that are DISPATCHED or RUNNING
        List<JobExecution> orphanedJobs = executionRepo
            .findByWorkerIdAndStatusIn(workerId, 
                List.of(JobStatus.DISPATCHED, JobStatus.RUNNING));
        
        for (JobExecution exec : orphanedJobs) {
            try {
                stateMachine.transition(exec.getExecutionId(), JobStatus.FAILED, 
                    null, "Worker " + workerId + " declared dead");
            } catch (Exception e) {
                log.error("Failed to recover execution {}: {}", 
                    exec.getExecutionId(), e.getMessage());
            }
        }
        
        metrics.counter("jobs.recovered.worker_death").increment(orphanedJobs.size());
    }
    
    private void reconcileRunningJobs(String workerId, List<RunningJobStatus> reported) {
        // Detect jobs the worker reports but scheduler thinks are not RUNNING
        // (stale state) and jobs scheduler thinks are running but worker doesn't
        // report (silently completed or crashed)
        Set<Long> reportedExecIds = reported.stream()
            .map(RunningJobStatus::getExecutionId)
            .collect(Collectors.toSet());
        
        List<JobExecution> schedulerView = executionRepo
            .findByWorkerIdAndStatusIn(workerId, List.of(JobStatus.RUNNING));
        
        for (JobExecution exec : schedulerView) {
            if (!reportedExecIds.contains(exec.getExecutionId())) {
                log.warn("Execution {} running on {} per scheduler but not reported by worker",
                    exec.getExecutionId(), workerId);
                // Mark as failed -- worker doesn't know about it
                stateMachine.transition(exec.getExecutionId(), JobStatus.FAILED, 
                    null, "Job not reported in worker heartbeat");
            }
        }
    }
}
```

**Failure Modes:**
- **Heartbeat storm after network recovery:** Workers reconnect simultaneously; use jitter on reconnection (0-5s random delay).
- **False positive (GC pause on worker):** Two-phase detection (SUSPECT then DEAD) with 60s total window prevents false positives from brief pauses.
- **Scheduler leader change during detection:** New leader rebuilds worker state from DB + fresh heartbeats within one heartbeat interval.

**Interviewer Q&A:**

**Q1: How do you handle 10K concurrent gRPC streams without exhausting file descriptors?**
A: Linux default is 1024 FDs; we set `ulimit -n 65536`. Each gRPC stream uses one FD. With 10K workers plus MySQL/Redis connections, we need ~12K FDs, well within limits. gRPC multiplexes streams over HTTP/2 connections, so workers from the same node share a single TCP connection.

**Q2: Why not use a message broker for heartbeats instead of direct gRPC?**
A: A broker adds latency (extra hop) and introduces a dependency -- if the broker fails, heartbeat detection stops. Direct gRPC streams give immediate connection-drop detection (TCP RST), which is faster than broker-based polling. The scheduler needs real-time worker state, not durable messaging.

**Q3: What if the scheduler's heartbeat processing falls behind?**
A: We monitor heartbeat processing lag as a metric. If the scheduler can't keep up, it's a sign we need to shard workers across multiple scheduler instances (partitioned scheduling). As a short-term mitigation, we batch heartbeat DB updates (only write to MySQL every 3rd heartbeat) and keep the hot path in-memory.

**Q4: How do you distinguish "worker died" from "worker is overloaded and slow to heartbeat"?**
A: The two-phase SUSPECT/DEAD detection handles this. At 30s (3 missed heartbeats), we mark SUSPECT and stop dispatching new jobs to that worker. At 60s, we mark DEAD and recover jobs. An overloaded worker that's still alive will send a heartbeat before the 60s threshold, automatically recovering from SUSPECT. We also monitor heartbeat jitter: if a worker's heartbeat interval is consistently > 15s, we flag it for investigation.

**Q5: How do you prevent thundering herd when the scheduler fails over?**
A: Workers maintain their gRPC stream to the scheduler's load-balanced VIP. When the leader changes, the new leader inherits the existing connections (the VIP routes to all instances; only the leader processes heartbeats). Workers don't need to reconnect. If the old leader process crashes, TCP connection drops trigger reconnection with jittered backoff (random delay 0-10s).

**Q6: What's the memory overhead of tracking 10K workers?**
A: Each WorkerHeartbeatState is ~200 bytes (worker ID, timestamps, resource capacity). 10K workers = 2MB. The ConcurrentHashMap overhead adds another ~1MB. Total < 5MB, negligible on a 16GB scheduler JVM heap.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

**Options:**
| Algorithm | Optimality | Latency | Complexity |
|-----------|-----------|---------|------------|
| First-Fit | Poor | Excellent (O(1) amortized) | Low |
| Best-Fit | Good | Good (O(N) workers) | Low |
| Worst-Fit | Moderate (reduces fragmentation) | Good (O(N)) | Low |
| Bin-Packing (FFD) | Near-optimal | Moderate (O(N log N)) | Medium |
| Score-based (k8s-style) | Configurable | Moderate | High |

**Selected: Score-based placement (Kubernetes scheduler-style):**
1. **Filter phase:** Eliminate workers that don't meet requirements (insufficient CPU/memory/GPU, wrong labels, cordoned/draining).
2. **Score phase:** Score remaining workers on: resource utilization balance, data locality, spreading, affinity/anti-affinity.
3. **Pick highest score.** Ties broken by least-loaded.

```python
def select_worker(job: JobDefinition, workers: List[WorkerNode]) -> Optional[WorkerNode]:
    # Phase 1: Filter
    candidates = [
        w for w in workers
        if w.status == 'ACTIVE'
        and w.available_cpu_millis >= job.resource_request['cpu_millis']
        and w.available_memory_mb >= job.resource_request['memory_mb']
        and w.available_gpu >= job.resource_request.get('gpu', 0)
        and matches_labels(w.labels, job.required_labels)
    ]
    
    if not candidates:
        return None  # No suitable worker; job stays QUEUED
    
    # Phase 2: Score (0-100 each, weighted)
    scored = []
    for w in candidates:
        score = 0
        # Balance: prefer workers with more headroom (spread load)
        cpu_ratio = w.available_cpu_millis / w.total_cpu_millis
        mem_ratio = w.available_memory_mb / w.total_memory_mb
        score += 40 * ((cpu_ratio + mem_ratio) / 2)  # 0-40 points
        
        # Affinity: prefer workers already running tasks from same DAG
        if has_dag_affinity(w, job):
            score += 30  # Data locality benefit
        
        # Spread: prefer workers with fewer jobs (anti-affinity for resilience)
        job_count = get_running_job_count(w)
        score += 20 * (1 - min(job_count / 100, 1.0))  # 0-20 points
        
        # Freshness: prefer workers with recent heartbeat
        heartbeat_age_s = (now() - w.last_heartbeat).total_seconds()
        score += 10 * max(0, 1 - heartbeat_age_s / 30)  # 0-10 points
        
        scored.append((w, score))
    
    scored.sort(key=lambda x: -x[1])
    return scored[0][0]
```

### Conflict Detection

- **Optimistic resource reservation:** When dispatching, atomically decrement worker's available resources via `UPDATE worker_node SET available_cpu_millis = available_cpu_millis - ? WHERE worker_id = ? AND available_cpu_millis >= ?`. If 0 rows updated, another dispatch consumed the resources; retry with next candidate.
- **Double-booking detection:** Heartbeat reconciliation detects if a worker reports more running jobs than its capacity allows (indicates a bug); alert and drain the worker.

### Queue & Priority Implementation

```
Redis Sorted Set: scheduler:dispatch_queue
  Score = (1000 - priority) * 1e12 + scheduled_at_epoch_ms
  Member = execution_id

Higher priority → lower score → dequeued first.
Equal priority → earlier scheduled_at wins (FIFO within priority).
```

```python
# Enqueue
def enqueue_job(execution_id: int, priority: int, scheduled_at_ms: int):
    score = (1000 - priority) * 1_000_000_000_000 + scheduled_at_ms
    redis.zadd("scheduler:dispatch_queue", {str(execution_id): score})

# Dequeue batch
def dequeue_batch(count: int) -> List[int]:
    # Atomic pop of lowest-scored members
    results = redis.zpopmin("scheduler:dispatch_queue", count)
    return [int(member) for member, score in results]
```

### Preemption Policy

- **When:** A high-priority job (priority >= 900) cannot be placed due to resource exhaustion.
- **What:** Identify lowest-priority running jobs on candidate workers whose resources would satisfy the high-priority job.
- **How:** Send SIGTERM to preempted job's process (via worker RPC), wait 30s grace period, then SIGKILL. Preempted job is re-queued with its original priority.
- **Guard rails:** Only preempt if priority difference >= 200. Never preempt jobs that have been running > 80% of their estimated duration (almost done). Max 3 preemptions per job per hour.

### Starvation Prevention

- **Priority aging:** Every 60 seconds, increment the effective priority of QUEUED jobs by 1 (capped at original priority + 200). After ~3 hours of waiting, a priority-0 job has effective priority 200, competing with medium-priority jobs.
- **Minimum throughput guarantee:** Each priority band is guaranteed at least 5% of total dispatch capacity. If low-priority jobs have been queued for > 10 minutes, reserve 5% of next dispatch batch for them regardless of higher-priority queue depth.
- **Tenant fairness:** Within the same priority level, round-robin across tenants to prevent one tenant from monopolizing the queue.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Trigger |
|-----------|-----------------|---------|
| Scheduler (leader) | Not horizontally scalable as single leader; scale to partitioned scheduling at 100K dispatches/min | Dispatch latency P99 > 2s |
| Scheduler (partitioned) | Partition by tenant_id hash; each partition has its own leader | Exceeding single-leader throughput |
| Workers | Add worker nodes to pools; auto-scaling based on queue depth | Queue depth > 1000 for > 5 minutes |
| API Gateway | Stateless; add instances behind LB | CPU > 70% |
| Heartbeat Collector | Shard by worker_id range | Stream count > 5K per instance |

### Database Scaling

| Strategy | Implementation | When |
|----------|---------------|------|
| Read replicas | 2 read replicas for status queries, history lookups | Read QPS > 10K |
| Partitioning | job_execution partitioned by created_at (quarterly) | Table > 100M rows |
| Connection pooling | HikariCP (per-service, max 50 connections); ProxySQL for cross-service multiplexing | Always |
| Archive | Move completed executions > 90 days to archive tables/cold storage | Monthly batch job |
| Sharding | Shard by tenant_id when single MySQL instance saturates (> 50K writes/sec) | Scale inflection point |

**HikariCP Configuration:**
```yaml
spring:
  datasource:
    hikari:
      maximum-pool-size: 50          # Per scheduler instance
      minimum-idle: 10
      idle-timeout: 300000           # 5 minutes
      max-lifetime: 1800000          # 30 minutes
      connection-timeout: 5000       # 5 seconds
      leak-detection-threshold: 60000 # 1 minute
      validation-timeout: 3000
```

### Caching

| Layer | Technology | Data Cached | TTL | Invalidation |
|-------|-----------|-------------|-----|-------------|
| L1: In-process | Caffeine (JVM) | Job definitions, DAG structures | 60s | Event-driven (DB change notification) |
| L2: Distributed | Redis | Worker available resources, dispatch queue | 10s | Heartbeat updates |
| L3: Query cache | MySQL query cache (disabled) / application-level | Execution history aggregations | 5 min | Write-through |
| L4: Search cache | Elasticsearch | Execution search results | 30s | Index refresh interval |

### Kubernetes-Specific Scaling

| Mechanism | Used For | Configuration |
|-----------|----------|--------------|
| HPA (Horizontal Pod Autoscaler) | API Gateway pods | Target CPU 70%, min 3, max 20 |
| VPA (Vertical Pod Autoscaler) | Scheduler pods (memory tuning) | Update mode: "Off" (recommendations only) |
| KEDA (Event-Driven Autoscaler) | Worker pools | Scale on Redis queue depth: threshold 100 jobs/worker |
| Cluster Autoscaler | Bare-metal node provisioning | Trigger: unschedulable pods > 0 for 5 min |

```yaml
# KEDA ScaledObject for worker pool
apiVersion: keda.sh/v1alpha1
kind: ScaledObject
metadata:
  name: worker-pool-general
spec:
  scaleTargetRef:
    name: worker-general
  minReplicaCount: 10
  maxReplicaCount: 500
  pollingInterval: 15
  triggers:
  - type: redis
    metadata:
      address: redis-master:6379
      listName: scheduler:dispatch_queue
      listLength: "100"    # Scale up when queue > 100 * current replicas
```

### Interviewer Q&A

**Q1: How do you handle database connection exhaustion when scaling scheduler instances?**
A: Each scheduler instance uses HikariCP with max 50 connections. With 5 instances, that's 250 connections. MySQL 8.0 handles 5K+ connections. If we scale to partitioned scheduling with 20+ instances, we front MySQL with ProxySQL which multiplexes 1000+ application connections into 200 MySQL connections via connection pooling. ProxySQL also provides read/write splitting.

**Q2: How do you handle Redis as a single point of failure for the dispatch queue?**
A: Redis Sentinel for automatic failover (3 sentinels, 1 master, 2 replicas). We also maintain the queue in MySQL (`job_execution` table with status=QUEUED) as the source of truth. Redis is a performance optimization; if Redis fails, we fall back to MySQL-based dispatch with slightly higher latency. On Redis recovery, we rebuild the sorted set from MySQL.

**Q3: How does KEDA differ from HPA for worker scaling?**
A: HPA scales on resource metrics (CPU, memory) which are lagging indicators -- by the time CPU is high, the queue is already deep. KEDA scales on external metrics (Redis queue depth) which is a leading indicator. When queue depth rises, KEDA adds workers before they're overloaded. KEDA also scales to zero (cost saving) when the queue is empty.

**Q4: How do you handle partition rebalancing when a scheduler instance joins or leaves?**
A: We use consistent hashing (with virtual nodes) for tenant-to-partition assignment. When an instance joins, it takes ownership of the hash ring segment, claims leases in etcd for its tenants, and starts scheduling for them. In-flight dispatches from the old owner complete normally (workers don't care which scheduler dispatched them). We use a 30-second overlap period where both old and new owners can dispatch, with optimistic locking preventing duplicates.

**Q5: What's the cold start time for a new scheduler instance?**
A: JVM startup: ~5s. Spring Boot initialization: ~10s. HikariCP pool warming: ~5s. etcd leader election (if needed): up to 15s. Cache warming (load active job definitions): ~10s. Total cold start: ~30-45s. For hot standby, we keep the JVM running and caches warm; failover is just the leader election time: 5-15s.

**Q6: How do you prevent job_execution table from becoming too large?**
A: Quarterly RANGE partitioning on `created_at`. Partitions older than 90 days are detached and archived to object storage (using MySQL `ALTER TABLE ... EXCHANGE PARTITION`). This is an O(1) metadata operation, not a data copy. Archived data remains queryable via Elasticsearch (execution records are indexed on completion).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| Scheduler leader crash | All scheduling paused | etcd lease expiry (15s) | Standby takes over via leader election | 15-20s | 0 (MySQL durable) |
| Worker node failure | Jobs on that node (10-100 jobs) | Heartbeat timeout (60s) | Re-queue affected jobs | 60-90s | 0 (jobs re-executed) |
| MySQL primary failure | All writes blocked | MySQL semi-sync failover | Promote replica; ProxySQL reroutes | 30-60s | 0 (semi-sync) |
| Redis failure | Dispatch queue degraded | Sentinel detection (10s) | Sentinel promotes replica; fall back to MySQL queue | 10-15s | Minimal (Redis is cache) |
| etcd cluster failure | Leader election impossible; scheduling pauses | Health check failure | Restore etcd from snapshot; existing leader continues if already elected | 5-15 min | 0 (etcd is coordination, not data) |
| Network partition (scheduler ↔ workers) | Affected partition's jobs | Heartbeat timeout on both sides | Scheduler re-queues; workers abort and reconnect | 60-120s | 0 |
| Disk full on MySQL | Writes fail | Disk usage alert at 80% | Purge old partitions; expand volume | 5-15 min | 0 |
| API Gateway overload | Job submissions rejected | 5xx rate spike | HPA scales pods; shed load via rate limiting | 30-60s | 0 (clients retry) |

### Automated Recovery

```java
@Component
public class AutoRecoveryService {
    
    @Scheduled(fixedRate = 30_000) // Every 30 seconds
    public void recoverStuckJobs() {
        if (!leaderElector.isLeader()) return;
        
        // 1. DISPATCHED but no RUNNING after 60s → re-queue
        List<JobExecution> stuckDispatched = executionRepo
            .findByStatusAndDispatchedBefore(JobStatus.DISPATCHED, 
                Instant.now().minus(60, ChronoUnit.SECONDS));
        
        for (JobExecution exec : stuckDispatched) {
            log.warn("Recovering stuck DISPATCHED execution {}", exec.getExecutionId());
            stateMachine.transition(exec.getExecutionId(), JobStatus.QUEUED, null,
                "Auto-recovery: dispatch timeout");
            metrics.counter("recovery.stuck_dispatched").increment();
        }
        
        // 2. RUNNING past timeout → TIMED_OUT
        List<JobExecution> timedOut = executionRepo.findTimedOutExecutions();
        for (JobExecution exec : timedOut) {
            log.warn("Timing out execution {} (running for {}s, timeout={}s)",
                exec.getExecutionId(), 
                Duration.between(exec.getStartedAt(), Instant.now()).getSeconds(),
                jobDefRepo.findById(exec.getJobId()).get().getTimeoutSeconds());
            stateMachine.transition(exec.getExecutionId(), JobStatus.TIMED_OUT, null,
                "Execution exceeded timeout");
            metrics.counter("recovery.timed_out").increment();
        }
        
        // 3. Orphaned: RUNNING on DEAD worker → FAILED
        List<JobExecution> orphaned = executionRepo
            .findRunningOnDeadWorkers();
        for (JobExecution exec : orphaned) {
            stateMachine.transition(exec.getExecutionId(), JobStatus.FAILED, null,
                "Worker declared dead");
            metrics.counter("recovery.orphaned").increment();
        }
    }
}
```

### Retry Strategy

| Parameter | Default | Configurable | Range |
|-----------|---------|-------------|-------|
| Max retries | 3 | Per-job | 0-10 |
| Initial backoff | 1s | Per-job | 100ms-60s |
| Backoff multiplier | 2x (exponential) | Global | Fixed |
| Max backoff | 5 min | Global | Fixed |
| Jitter | +/- 20% | Global | Fixed |
| Retry condition | Any non-zero exit code | Per-job (can specify retryable exit codes) | Customizable |

### Circuit Breaker

Applied at the worker-pool level: if > 50% of jobs dispatched to a pool fail within a 5-minute window, the circuit opens. During open state, jobs queue but are not dispatched to that pool. After 60s, the circuit enters half-open: dispatch 10% of jobs to test. If success rate > 80%, close the circuit.

```java
@Component
public class WorkerPoolCircuitBreaker {
    
    private final Map<String, CircuitBreakerState> poolStates = new ConcurrentHashMap<>();
    
    private static final double FAILURE_THRESHOLD = 0.50;
    private static final Duration WINDOW = Duration.ofMinutes(5);
    private static final Duration OPEN_DURATION = Duration.ofSeconds(60);
    private static final double HALF_OPEN_RATIO = 0.10;
    
    public boolean canDispatchTo(String poolName) {
        CircuitBreakerState state = poolStates.getOrDefault(poolName, 
            CircuitBreakerState.CLOSED);
        
        return switch (state.getState()) {
            case CLOSED -> true;
            case OPEN -> {
                if (state.getOpenedAt().plus(OPEN_DURATION).isBefore(Instant.now())) {
                    state.setState(State.HALF_OPEN);
                    yield true; // Allow test traffic
                }
                yield false;
            }
            case HALF_OPEN -> ThreadLocalRandom.current().nextDouble() < HALF_OPEN_RATIO;
        };
    }
}
```

### Consensus & Coordination

| Resource | Mechanism | Tool |
|----------|-----------|------|
| Scheduler leadership | Lease-based election | etcd |
| Job dispatch (prevent double-dispatch) | Optimistic locking on job_execution | MySQL |
| Cron trigger firing | Distributed lock per cron job | Redis SETNX with TTL |
| Worker resource reservation | Atomic UPDATE with WHERE condition | MySQL |
| Configuration changes | Watch-based notification | etcd |

---

## 10. Observability

### Key Metrics

| Metric | Type | Threshold (Warning) | Threshold (Critical) | Dashboard |
|--------|------|---------------------|---------------------|-----------|
| `scheduler.dispatch_queue.depth` | Gauge | > 10K | > 50K | Queue Health |
| `scheduler.dispatch.latency_ms` | Histogram | P99 > 1s | P99 > 5s | Dispatch SLA |
| `scheduler.dispatch.rate` | Counter | < 100/s (low) | N/A | Throughput |
| `job.execution.duration_ms` | Histogram | P99 > 2x avg | P99 > 5x avg | Job Performance |
| `job.failure.rate` | Rate | > 5% | > 15% | Reliability |
| `job.dlq.depth` | Gauge | > 100 | > 500 | Error Health |
| `worker.active.count` | Gauge | < 80% capacity | < 50% capacity | Fleet Health |
| `worker.heartbeat.lag_ms` | Histogram | P99 > 15s | P99 > 30s | Worker Health |
| `scheduler.leader.is_leader` | Gauge | 0 (no leader) | 0 for > 30s | HA Status |
| `mysql.connection.active` | Gauge | > 80% pool | > 95% pool | DB Health |
| `mysql.replication.lag_s` | Gauge | > 1s | > 5s | DB Health |
| `redis.memory.used_bytes` | Gauge | > 80% maxmem | > 95% maxmem | Cache Health |
| `dag.run.duration_ms` | Histogram | > 2x SLA | > SLA | DAG Performance |
| `dag.stuck.count` | Gauge | > 0 | > 5 | DAG Health |

### Distributed Tracing

Every job execution carries a trace context:
```
trace_id: generated at job submission (or inherited from parent DAG run)
span: submission → evaluation → dispatch → worker_start → worker_end → completion_ack
```

Integration with OpenTelemetry:
- Scheduler propagates trace context in gRPC metadata to workers
- Workers create child spans for job execution phases
- DAG runs create parent spans with child spans for each task
- Cross-service: API Gateway → Scheduler → Worker → External Services

### Logging

| Log Level | Content | Destination | Retention |
|-----------|---------|-------------|-----------|
| ERROR | Unhandled exceptions, state transition failures, data corruption | Elasticsearch + PagerDuty | 1 year |
| WARN | Retry events, worker suspects, circuit breaker opens | Elasticsearch | 90 days |
| INFO | Job lifecycle events (submitted, started, completed), leader changes | Elasticsearch | 30 days |
| DEBUG | Dispatch decisions, score calculations, heartbeat details | Local file only | 7 days |

Structured JSON logging format:
```json
{
  "timestamp": "2026-04-09T14:30:00.123Z",
  "level": "INFO",
  "service": "job-scheduler",
  "instance_id": "scheduler-01",
  "is_leader": true,
  "trace_id": "abc123",
  "span_id": "def456",
  "event": "job.dispatched",
  "job_id": 12345678,
  "execution_id": 99887766,
  "worker_id": "worker-pool-a-node-17",
  "dispatch_latency_ms": 42,
  "queue_depth_at_dispatch": 1523
}
```

### Alerting

| Alert | Condition | Severity | Escalation |
|-------|-----------|----------|------------|
| No active leader | `scheduler.leader.is_leader` sum == 0 for > 30s | P1 | Page on-call SRE |
| Dispatch queue growing | Queue depth increasing > 1K/min for 10 min | P2 | Slack + ticket |
| High job failure rate | > 15% failures in 5-min window | P2 | Slack + page if > 30% |
| DLQ growing | DLQ depth > 500 | P3 | Slack + daily review |
| Worker fleet degraded | Active workers < 50% of registered | P1 | Page on-call |
| MySQL replication lag | > 5s for > 2 min | P2 | Page DBA on-call |
| Scheduler OOM risk | JVM heap > 90% after full GC | P2 | Page on-call SRE |

---

## 11. Security

### Authentication & Authorization

| Layer | Mechanism | Detail |
|-------|-----------|--------|
| External API | JWT (RS256) via API Gateway | Tokens issued by corporate IdP; 15-min expiry |
| Service-to-service | mTLS (mutual TLS) | Certificate rotation via cert-manager; 24h cert lifetime |
| Scheduler ↔ Worker | gRPC with mTLS + fencing token | Bidirectional authentication |
| CLI | API key + short-lived JWT exchange | `jobctl login` triggers OAuth2 device flow |

**RBAC Model:**
```
Roles:
  - job-admin:    CRUD on all jobs in tenant, trigger, cancel, DLQ management
  - job-operator: trigger, cancel, view executions, view DLQ
  - job-viewer:   read-only access to job definitions and execution history
  - platform-admin: cross-tenant access, worker management, cluster operations

Resources: job, dag, execution, worker, dlq
Actions: create, read, update, delete, trigger, cancel, drain
```

```yaml
# Example RBAC policy
apiVersion: scheduler.infra/v1
kind: RoleBinding
metadata:
  tenant: team-dba
spec:
  role: job-admin
  subjects:
  - kind: Group
    name: team-dba-engineers
  - kind: ServiceAccount
    name: ci-pipeline-dba
```

### Secrets Management

- Job environment variables: stored encrypted at rest in MySQL (AES-256-GCM, key in HashiCorp Vault)
- Worker credentials: mounted via Kubernetes Secrets (backed by Vault CSI driver)
- Database passwords: rotated every 30 days via Vault dynamic secrets
- API keys: stored in Vault; never logged or exposed in API responses

### Network Security

- Scheduler ↔ MySQL: private subnet, security group allows only scheduler IPs on port 3306
- Scheduler ↔ Redis: same private subnet, port 6379
- Worker ↔ Scheduler: gRPC on port 9090, mTLS required
- API Gateway: public-facing on port 443, TLS termination at LB
- etcd: port 2379/2380, restricted to scheduler instances only
- Network policies (Kubernetes): deny-all default, explicit allow rules per service

### Audit Logging

All mutations logged to immutable audit log:
```json
{
  "timestamp": "2026-04-09T14:30:00Z",
  "actor": "user:john.doe@company.com",
  "action": "job.create",
  "resource": "job/12345678",
  "tenant": "team-dba",
  "source_ip": "10.0.1.42",
  "user_agent": "jobctl/1.2.3",
  "request_id": "req-abc-123",
  "changes": {"job_name": "nightly-backup-db", "priority": 700}
}
```
Audit logs shipped to a separate Elasticsearch cluster with 1-year retention and write-once policy (no deletes allowed).

---

## 12. Incremental Rollout Strategy

### Phase 1: Shadow Mode (Week 1-2)
- Deploy new scheduler alongside existing system
- Mirror all job submissions to new scheduler (write-only, no execution)
- Compare scheduling decisions: new vs. old (diff report)
- Validate: DAG resolution correctness, dispatch ordering, timing accuracy
- **Exit criteria:** 100% decision match for 48 hours

### Phase 2: Canary (Week 3-4)
- Route 5% of new job submissions to new scheduler (actual execution)
- Select low-risk tenants (internal, non-production)
- Monitor: failure rate, dispatch latency, worker utilization delta
- Gradually increase to 10%, 25%
- **Exit criteria:** No regressions in failure rate, latency within 10% of baseline

### Phase 3: Blue-Green (Week 5-6)
- Route 50% of traffic to new scheduler
- Old scheduler handles remaining 50%
- Both share MySQL and Redis (compatible schema)
- Instant rollback: flip traffic routing back to old
- **Exit criteria:** P99 latency meets SLA, error rate < 1%, DLQ depth stable

### Phase 4: Full Rollout (Week 7-8)
- Route 100% to new scheduler
- Keep old scheduler instances running (standby) for 2 weeks
- Decommission old scheduler after 2-week bake period
- **Exit criteria:** 2 weeks of stable operation

### Rollback Strategy
- **Instant rollback:** Change API Gateway routing to point to old scheduler (< 30s)
- **Data compatibility:** Both schedulers read/write same MySQL schema; no migration needed
- **In-flight jobs:** Jobs dispatched by new scheduler continue to completion; workers are scheduler-agnostic
- **State reconciliation:** On rollback, old scheduler rebuilds its in-memory state from MySQL

### Rollout Interviewer Q&A

**Q1: What if the new scheduler has a subtle bug that only manifests at scale?**
A: Shadow mode catches logic bugs (wrong decisions). Canary at 5% catches performance issues early with limited blast radius. We also run chaos engineering tests during canary: kill the new scheduler leader, simulate worker failures, inject MySQL latency. If any anomaly is detected, we halt rollout and investigate.

**Q2: How do you handle schema changes between old and new scheduler?**
A: We use expand-contract migrations. Phase 1: add new columns (nullable). Phase 2: deploy new code that writes both old and new columns. Phase 3: migrate old data. Phase 4: remove old columns. The old scheduler ignores new columns it doesn't know about (MySQL ignores extra columns in SELECT *). We never remove columns during rollout.

**Q3: What if rollback is needed during blue-green when both schedulers are active?**
A: Both schedulers use optimistic locking on job_execution, so concurrent operations on the same job are safely serialized. Rollback simply stops routing to the new scheduler. Any in-flight dispatches from the new scheduler complete normally. The old scheduler picks up any QUEUED jobs that weren't dispatched.

**Q4: How do you test the rollback procedure itself?**
A: We perform rollback drills during the canary phase. On day 3 of canary, we intentionally roll back and verify: all jobs continue, no duplicates, no lost jobs, latency returns to baseline within 60s. We also test rollback under load (run a load test, trigger rollback mid-test).

**Q5: How do you communicate rollout progress to stakeholders?**
A: Automated daily rollout report: traffic split, error rates (old vs. new), latency comparison, DLQ depth delta, resource utilization. Posted to #scheduler-rollout Slack channel. Anomalies trigger automatic pause and notification.

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Selected | Trade-off | Rationale |
|----------|-------------------|----------|-----------|-----------|
| Single leader vs. multi-leader scheduling | Single leader, partitioned multi-leader, peer-to-peer | Single leader (initially) | Lower throughput ceiling vs. simplicity | 100K dispatches/min is sufficient for initial scale; upgrade path to partitioned is clear |
| MySQL vs. purpose-built job store | MySQL, Redis-only, DynamoDB, custom B-tree | MySQL | Higher write latency vs. operational simplicity | Team expertise, ACID guarantees, existing infrastructure |
| Push vs. pull job dispatch | Workers pull (polling), scheduler pushes, streaming | Workers pull via gRPC | Slightly higher latency vs. simpler flow control | Workers control their own concurrency; no need for scheduler to track worker capacity in real-time |
| Redis sorted set vs. MySQL-only queue | Redis sorted set, MySQL SELECT FOR UPDATE SKIP LOCKED, Kafka | Redis sorted set + MySQL fallback | Additional dependency vs. performance | Redis ZPOPMIN is O(log N) and atomic; MySQL polling requires row locking |
| At-least-once vs. exactly-once | At-least-once default, exactly-once via 2PC | At-least-once with idempotency support | Possible duplicate execution vs. lower complexity | Exactly-once requires 2PC or saga pattern, adding significant complexity; idempotent jobs are simpler |
| gRPC vs. REST for worker communication | gRPC, REST, message broker | gRPC bidirectional streaming | Requires gRPC client on workers vs. performance | Streaming heartbeats, binary protocol efficiency, built-in load balancing |
| Optimistic vs. pessimistic locking | Optimistic (version column), pessimistic (SELECT FOR UPDATE) | Optimistic | Retry on conflict vs. holding locks | Conflicts are rare (< 1%); optimistic avoids lock contention and deadlocks |
| Quarterly vs. daily partitioning | Daily, weekly, monthly, quarterly | Quarterly | Larger partitions vs. fewer partition operations | 50M rows/quarter is manageable; daily would create 365 partitions/year |

---

## 14. Agentic AI Integration

### Where AI Adds Value

| Use Case | AI Capability | Business Impact |
|----------|--------------|----------------|
| **Predictive job runtime estimation** | LLM analyzes historical execution data + job parameters to predict runtime | Better scheduling decisions, earlier deadline warnings |
| **Auto-tuning resource requests** | ML model recommends CPU/memory based on execution history | Reduce over-provisioning by 30-40% |
| **Anomaly detection in job failures** | LLM correlates failures across jobs, workers, time windows | Faster root cause identification (minutes vs. hours) |
| **Natural language job definition** | "Run database backup every night at 2am EST" → job spec YAML | Lower barrier to entry for non-engineers |
| **Intelligent retry strategy** | LLM classifies failure type (transient vs. permanent) and adjusts retry | Fewer wasted retries, faster DLQ routing |
| **Capacity planning** | Predict future resource needs from job submission trends | Proactive infrastructure scaling |

### LLM Agent Loop: Observe-Reason-Act-Verify

```
┌─────────────────────────────────────────────────────┐
│                 AI Agent Loop                        │
│                                                     │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐        │
│  │ OBSERVE  │──►│  REASON  │──►│   ACT    │        │
│  │          │   │          │   │          │        │
│  │ Metrics  │   │ Analyze  │   │ Execute  │        │
│  │ Logs     │   │ patterns │   │ tool     │        │
│  │ Alerts   │   │ Propose  │   │ calls    │        │
│  └──────────┘   │ actions  │   └────┬─────┘        │
│       ▲         └──────────┘        │              │
│       │                             ▼              │
│       │         ┌──────────┐                       │
│       └─────────│  VERIFY  │                       │
│                 │          │                       │
│                 │ Check    │                       │
│                 │ outcome  │                       │
│                 │ Rollback │                       │
│                 │ if bad   │                       │
│                 └──────────┘                       │
└─────────────────────────────────────────────────────┘
```

**Example: Auto-tuning Resource Requests**

```python
class ResourceTuningAgent:
    def __init__(self, llm_client, scheduler_api, metrics_api):
        self.llm = llm_client
        self.scheduler = scheduler_api
        self.metrics = metrics_api
    
    def observe(self, job_id: int) -> dict:
        """Gather execution history and resource usage."""
        executions = self.scheduler.get_executions(job_id, limit=100)
        resource_metrics = self.metrics.query(
            f'container_memory_usage_bytes{{job_id="{job_id}"}}',
            range='7d', step='1h'
        )
        return {
            'executions': executions,
            'cpu_usage_p95': self.metrics.percentile(resource_metrics, 'cpu', 0.95),
            'memory_usage_p95': self.metrics.percentile(resource_metrics, 'memory', 0.95),
            'current_request': self.scheduler.get_job(job_id).resource_request,
            'oom_kills': sum(1 for e in executions if e.error_message == 'OOMKilled'),
        }
    
    def reason(self, observation: dict) -> dict:
        """Use LLM to analyze and recommend."""
        prompt = f"""
        Analyze this job's resource usage and recommend new resource requests.
        
        Current request: {observation['current_request']}
        CPU P95 usage: {observation['cpu_usage_p95']} milliCPU
        Memory P95 usage: {observation['memory_usage_p95']} MB
        OOM kills in last 100 runs: {observation['oom_kills']}
        
        Rules:
        - Recommend CPU = P95 usage * 1.3 (30% headroom), rounded to nearest 250m
        - Recommend Memory = P95 usage * 1.5 (50% headroom for GC spikes), rounded to nearest 256MB
        - If OOM kills > 0, recommend Memory = max(current * 2, P95 * 2)
        - Never reduce below 250m CPU or 512MB memory
        - Output JSON: {{"cpu_millis": int, "memory_mb": int, "reasoning": str}}
        """
        response = self.llm.generate(prompt, temperature=0.1)
        return json.loads(response)
    
    def act(self, job_id: int, recommendation: dict) -> dict:
        """Apply the recommendation (with guard rails)."""
        current = self.scheduler.get_job(job_id).resource_request
        
        # Guard rails
        max_cpu_change = current['cpu_millis'] * 3  # Never more than 3x current
        max_mem_change = current['memory_mb'] * 3
        
        new_cpu = min(recommendation['cpu_millis'], max_cpu_change)
        new_mem = min(recommendation['memory_mb'], max_mem_change)
        
        # Dry-run first
        dry_run_result = self.scheduler.update_job(job_id, 
            resource_request={'cpu_millis': new_cpu, 'memory_mb': new_mem},
            dry_run=True)
        
        if dry_run_result['valid']:
            # Apply with audit trail
            self.scheduler.update_job(job_id,
                resource_request={'cpu_millis': new_cpu, 'memory_mb': new_mem},
                reason=f"AI auto-tune: {recommendation['reasoning']}",
                actor='ai-agent/resource-tuner')
            return {'applied': True, 'new_request': {'cpu_millis': new_cpu, 'memory_mb': new_mem}}
        else:
            return {'applied': False, 'reason': dry_run_result['errors']}
    
    def verify(self, job_id: int, previous_request: dict) -> bool:
        """Verify the change didn't cause regressions."""
        # Wait for 3 executions after the change
        # Check: no increase in failure rate, no OOM kills, runtime within 20% of baseline
        executions = self.scheduler.get_executions(job_id, limit=3, 
                                                     since=self.change_timestamp)
        if len(executions) < 3:
            return None  # Not enough data yet
        
        failures = sum(1 for e in executions if e.status == 'FAILED')
        if failures > 0:
            # Rollback
            self.scheduler.update_job(job_id, resource_request=previous_request,
                reason='AI auto-tune rollback: post-change failures detected',
                actor='ai-agent/resource-tuner')
            return False
        
        return True
```

### Tool Calls & Guard Rails

| Tool | Purpose | Guard Rail |
|------|---------|-----------|
| `scheduler.update_job` | Modify resource requests, priority | Max 3x change from current; dry-run required |
| `scheduler.pause_job` | Pause failing jobs | Only for jobs with > 80% failure rate in last hour |
| `scheduler.scale_workers` | Adjust worker pool size | Max 2x scale-up, 50% scale-down per action |
| `metrics.query` | Read observability data | Read-only, no modification |
| `alertmanager.silence` | Suppress known-issue alerts | Max 4-hour silence; requires human approval for > 1 hour |

### Human-in-the-Loop

- **Approval required for:** Resource changes > 2x, priority changes for production jobs, worker pool scaling > 50%, any action on tenant-critical (tier-1) jobs.
- **Notification (no approval):** Resource tuning within bounds, retry strategy adjustment, non-production job pausing.
- **Audit trail:** Every AI action logged with: observation data, reasoning, action taken, verification result, human approver (if applicable).

### Reliability: Dry-Run, Audit, Rollback

1. **Dry-run:** Every write action is first executed in dry-run mode. The scheduler validates the change and returns what would happen without applying it.
2. **Audit trail:** AI actions are logged to the same audit system as human actions, with `actor=ai-agent/<agent-name>` to distinguish them.
3. **Rollback:** Every AI action records the pre-change state. If verification fails (post-change regression), the agent automatically rolls back. If the rollback also fails, the agent pages a human.
4. **Rate limiting:** AI agent limited to 10 actions per hour per job, 100 actions per hour globally. Exceeding this triggers human review.
5. **Kill switch:** `jobctl admin ai-agent disable` immediately stops all AI agent actions cluster-wide.

---

## 15. Complete Interviewer Q&A Bank

**Q1: How do you ensure a cron job fires exactly once across multiple scheduler instances?**
A: Only the leader scheduler evaluates cron schedules. If using partitioned scheduling, each cron job is owned by exactly one partition. As a defense-in-depth, we acquire a Redis distributed lock (SETNX with TTL = cron interval) keyed by `cron:{job_id}:{scheduled_time}` before creating the execution record. If the lock already exists, we skip (another instance already fired it).

**Q2: What happens if the scheduler crashes mid-dispatch (after creating execution record but before sending to worker)?**
A: The execution record is in QUEUED or DISPATCHED state in MySQL. The recovery service (running every 30s) detects executions stuck in DISPATCHED for > 60s and re-queues them. The new leader picks them up on the next dispatch cycle. No jobs are lost because MySQL is the source of truth, not in-memory state.

**Q3: How would you implement exactly-once execution for a financial reconciliation job?**
A: The job itself must be idempotent. We provide an `idempotency_token` (hash of job_id + scheduled_time + attempt_number). The job writes a completion record to a transactional outbox table: `INSERT INTO reconciliation_log (idempotency_token, ...) ON DUPLICATE KEY UPDATE ...`. If the job is executed twice (due to scheduler retry), the second execution detects the existing record and short-circuits. The scheduler also marks the execution as SUCCEEDED on the second attempt with a note "idempotent duplicate."

**Q4: Your system uses MySQL for job storage. What happens at 10x scale (500M executions/day)?**
A: At 500M/day (~5.8K writes/sec sustained, ~50K/sec peak), single MySQL won't suffice. We shard by tenant_id using Vitess or ProxySQL. Each shard handles a subset of tenants. Cross-shard queries (admin dashboards) go to Elasticsearch. The dispatch queue moves to Kafka (partitioned by priority) for higher throughput. Worker heartbeats move to a dedicated low-latency store (Redis Streams or in-memory on the scheduler with periodic MySQL sync).

**Q5: How do you handle a poison job that keeps failing and consuming retry capacity?**
A: Three defenses. (1) Max retries (default 3) with exponential backoff. After exhaustion, the job goes to DLQ. (2) Per-job circuit breaker: if a job fails 5 consecutive times, it's automatically paused with an alert. (3) Anomaly detection: if a job's failure rate exceeds 3 standard deviations from its historical mean, we alert the owning team and suggest pausing.

**Q6: How do you test the DAG resolver for correctness?**
A: Property-based testing with randomly generated DAGs (using a DAG generator that ensures acyclicity). We verify: (1) every node is scheduled exactly once, (2) no node is scheduled before its dependencies complete, (3) cycle detection catches all cycles (inject cycles and verify rejection), (4) conditional branches (ON_FAILURE) are exercised. We also replay production DAG runs in a test environment and compare execution order.

**Q7: What's your approach to multi-datacenter DR for the scheduler?**
A: Active-passive with MySQL semi-synchronous replication to the DR datacenter. RPO = 0 (semi-sync ensures every committed transaction is replicated). RTO = 5-15 minutes (DNS failover + scheduler startup in DR). During failover, we accept that some in-flight jobs may be re-executed (at-least-once guarantee). We do not attempt active-active scheduling across datacenters due to consensus latency.

**Q8: How do you handle timezone and DST changes for cron jobs?**
A: Cron jobs store their timezone explicitly (e.g., "America/New_York"). We use the IANA timezone database (java.time.ZoneId in Java, pytz in Python). When DST changes, a 2 AM cron job in EST might fire at 3 AM EDT or not fire at all during the spring-forward gap. Our policy: if the scheduled time falls in the DST gap, fire immediately at the transition point. If the scheduled time occurs twice (fall-back), fire only on the first occurrence. This matches the behavior of Quartz Scheduler's `withMisfireHandlingInstructionFireAndProceed`.

**Q9: How do you prevent a single tenant from monopolizing the dispatch queue?**
A: Per-tenant dispatch quotas: max N jobs dispatched per minute. Additionally, within the same priority level, the dispatch engine round-robins across tenants. If tenant A has 10K queued jobs and tenant B has 100, they get equal dispatch slots at the same priority. Separate "reserved capacity" pools can be created for critical tenants who need guaranteed throughput.

**Q10: What's the memory model for the scheduler JVM?**
A: We use a 16GB heap with G1GC. Key data structures in memory: worker state map (~5MB for 10K workers), DAG cache (~500MB for 200K cached DAGs), dispatch queue local copy (~100MB). Total steady-state heap usage: ~2-3GB. The remaining headroom handles request processing bursts. We set `-XX:MaxGCPauseMillis=200` and monitor GC pause times. If full GC frequency exceeds 1/hour, we increase heap or investigate memory leaks.

**Q11: How would you add support for spot/preemptible instances for cost-sensitive jobs?**
A: Add a `scheduling_class` field to job_definition: GUARANTEED (never preempted), BURSTABLE (can be preempted with 2-min warning), SPOT (can be preempted immediately). Spot jobs only dispatch to spot worker pools. The scheduler subscribes to preemption notifications from the infrastructure layer. On notification, the scheduler sends SIGTERM to affected jobs, re-queues them on non-spot workers if priority warrants, or lets them wait for new spot capacity.

**Q12: How do you handle job output/artifact management?**
A: Job outputs are stored in object storage (MinIO on bare-metal). The `output_ref` field in job_execution stores the object path. Workers upload outputs to `s3://job-outputs/{tenant_id}/{job_id}/{execution_id}/`. The scheduler never handles output data directly. Output metadata (size, checksum) is stored in job_execution for quick lookup. Outputs follow the same retention policy as execution records (90 days default).

**Q13: How would you implement job-level resource quotas per tenant?**
A: Two levels. (1) **Submission quota:** Max active (enabled) jobs per tenant, enforced at the API layer. (2) **Execution quota:** Max concurrent running jobs per tenant, enforced at dispatch time. The dispatch engine checks `SELECT COUNT(*) FROM job_execution WHERE tenant_id = ? AND status = 'RUNNING'` before dispatching. If at quota, the job remains QUEUED. We cache the count in Redis (updated on every state transition) to avoid per-dispatch DB queries.

**Q14: What if Elasticsearch is down? Do you lose job search capability?**
A: ES is a secondary store; MySQL is the source of truth. If ES is down, execution search degrades to MySQL queries (slower for full-text search but functional for ID/status/time-range lookups). We maintain a "lag" metric: the difference between the latest MySQL execution and the latest ES-indexed execution. On ES recovery, the indexer catches up from the MySQL change stream (binlog).

**Q15: How do you handle job dependency cycles that span multiple DAGs?**
A: Cross-DAG dependencies are modeled via event triggers: DAG A's completion event triggers DAG B. We don't allow direct cross-DAG task-level dependencies (this would create a meta-DAG that's harder to reason about). For cycle detection across event triggers, we maintain a "trigger graph" and run cycle detection on it during DAG creation. If adding a trigger would create a cycle, we reject it.

**Q16: How would you benchmark the scheduler to find its throughput ceiling?**
A: We build a load test harness that: (1) generates synthetic jobs at increasing rates (1K, 5K, 10K, 50K, 100K per minute), (2) measures dispatch latency at each level, (3) monitors MySQL write latency, Redis ZPOPMIN latency, and GC pause times. The throughput ceiling is the point where P99 dispatch latency exceeds 5s or the dispatch queue depth grows unboundedly. We run this in a dedicated performance environment with production-equivalent hardware.

---

## 16. References

- [Apache Airflow Architecture](https://airflow.apache.org/docs/apache-airflow/stable/concepts/overview.html)
- [Quartz Scheduler Clustering](http://www.quartz-scheduler.org/documentation/quartz-2.3.0/configuration/ConfigJDBCJobStoreClustering.html)
- [Kubernetes CronJob Controller Source](https://github.com/kubernetes/kubernetes/tree/master/pkg/controller/cronjob)
- [etcd Leader Election Recipe](https://etcd.io/docs/v3.5/dev-guide/api_concurrency_reference_v3/)
- [Google Borg: Large-Scale Cluster Management](https://research.google/pubs/pub43438/) (Verma et al., EuroSys 2015)
- [Kahn's Algorithm - Topological Sort](https://en.wikipedia.org/wiki/Topological_sorting#Kahn's_algorithm)
- [HikariCP Configuration Guide](https://github.com/brettwooldridge/HikariCP#configuration-knobs-baby)
- [KEDA - Kubernetes Event-Driven Autoscaling](https://keda.sh/docs/2.12/)
- [OpenTelemetry Distributed Tracing](https://opentelemetry.io/docs/concepts/signals/traces/)
- [Celery Architecture](https://docs.celeryq.dev/en/stable/getting-started/introduction.html)
- [MySQL 8.0 Partitioning](https://dev.mysql.com/doc/refman/8.0/en/partitioning.html)
- [Redis Sorted Sets](https://redis.io/docs/data-types/sorted-sets/)
