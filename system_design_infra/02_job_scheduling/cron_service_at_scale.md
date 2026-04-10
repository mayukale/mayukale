# System Design: Cron Service at Scale

> **Relevance to role:** Every infrastructure platform depends on scheduled recurring tasks -- certificate rotation, health checks, log rotation, capacity reports, database backups, DNS cache invalidation. Building a cron service that handles 10M+ scheduled jobs with sub-second trigger accuracy, proper timezone/DST handling, and distributed duplicate prevention is a core platform engineering challenge.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement | Detail |
|----|------------|--------|
| FR-1 | Cron expression support | Unix 5-field, Quartz 6/7-field (with seconds), and AWS EventBridge rate expressions |
| FR-2 | Timezone-aware scheduling | Per-job timezone; correct DST transition handling |
| FR-3 | Exactly-once trigger | Each schedule fires exactly once despite multiple scheduler instances |
| FR-4 | Missed execution handling | Configurable: run-immediately, skip-to-next, or run-all-missed |
| FR-5 | Job lifecycle | Create, update, delete, pause, resume cron jobs |
| FR-6 | Calendar-aware scheduling | Skip holidays, business hours only, custom calendars |
| FR-7 | Execution concurrency control | Configurable: allow overlap, skip if previous still running, queue |
| FR-8 | Multi-tenancy | Per-tenant cron job isolation, quotas, and namespacing |
| FR-9 | Audit trail | Full history of triggers, outcomes, and configuration changes |
| FR-10 | Alerting on missed triggers | Detect and alert when a cron job doesn't fire within its expected window |

### Non-Functional Requirements
| NFR | Target | Rationale |
|-----|--------|-----------|
| Trigger accuracy | Within 1 second of scheduled time | Infrastructure tasks (cert rotation) require precision |
| Availability | 99.99% | Missed cron fires can cause outages (expired certs, full disks) |
| Scale | 10M registered cron jobs, 2M active | Enterprise platform with hundreds of teams |
| Throughput | 100K triggers/min at peak | Many jobs fire at the same minute (e.g., */5 * * * *) |
| Trigger latency | < 5s from scheduled time to job execution start | Combined trigger + dispatch + worker start |
| Consistency | No duplicate triggers | At-most-once trigger semantics (exactly-once with distributed lock) |
| Durability | Zero loss of cron job definitions | All definitions persisted to MySQL |

### Constraints & Assumptions
- Builds on the distributed job scheduler for actual job execution
- MySQL 8.0 for cron job definitions and trigger history
- Redis for next-execution sorted sets and distributed locks
- etcd for leader election
- Java 17+ for cron service; Quartz Scheduler library available
- IANA timezone database (java.time.ZoneId) for timezone handling
- Maximum cron expression frequency: once per second (Quartz format)
- Internal network; < 1ms RTT between cron service and job scheduler

### Out of Scope
- User-facing scheduling UI (programmatic/CLI only)
- Event-driven triggers (webhooks, message-based) -- handled by event system
- Complex workflow scheduling (DAGs) -- handled by job scheduler
- Multi-region cron (single region with DR failover)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Registered cron jobs | 10M | 500 teams x 20K jobs/team |
| Active (enabled) cron jobs | 2M | 20% active at any time |
| Triggers/day | 100M | Avg 50 triggers/day per active job (many fire every 5 min) |
| Peak triggers/min | 100K | :00 and :30 minutes of each hour (common cron patterns) |
| "Thundering minute" triggers | 500K | Midnight UTC: many daily jobs fire simultaneously |
| Distinct timezones | ~50 | Major timezones used by teams globally |
| Calendar rules | 5K | Custom holiday calendars, business hour definitions |

### Latency Requirements

| Operation | P50 | P99 | P99.9 |
|-----------|-----|-----|-------|
| Cron expression evaluation | 0.1ms | 1ms | 5ms |
| Next-execution-time calculation | 0.5ms | 5ms | 20ms |
| Trigger detection (sorted set scan) | 1ms | 10ms | 50ms |
| Trigger-to-dispatch (total) | 100ms | 1s | 5s |
| Cron job CRUD API | 10ms | 50ms | 200ms |

### Storage Estimates

| Data | Size per record | Count | Total | Retention |
|------|----------------|-------|-------|-----------|
| Cron job definitions | 1 KB | 10M | 10 GB | Indefinite |
| Next-execution index (Redis) | 50 B | 2M active | 100 MB | Real-time |
| Trigger history | 200 B | 100M/day | 20 GB/day | 30 days = 600 GB |
| Calendar definitions | 5 KB | 5K | 25 MB | Indefinite |
| Missed trigger alerts | 500 B | 10K/day | 5 MB/day | 90 days |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Trigger evaluations (peak) | 100K/min x 50 B = 5 MB/min | 83 KB/s |
| Next-execution updates | 100K/min x 50 B = 5 MB/min | 83 KB/s |
| Trigger history writes | 100K/min x 200 B = 20 MB/min | 333 KB/s |
| CRUD operations | 1K/min x 1 KB = 1 MB/min | 17 KB/s |
| **Total** | | **~0.5 MB/s** |

---

## 3. High Level Architecture

```
    ┌──────────────────────────────────────────────────┐
    │               API Gateway                         │
    │  (CRUD: create/update/delete/pause/resume)        │
    └────────────────────┬─────────────────────────────┘
                         │
    ┌────────────────────▼─────────────────────────────┐
    │          Cron Service Cluster                      │
    │                                                   │
    │  ┌──────────────────────────────────────────┐    │
    │  │         Cron Trigger Engine               │    │
    │  │         (Leader only)                     │    │
    │  │                                           │    │
    │  │  ┌─────────────┐  ┌─────────────────┐   │    │
    │  │  │  Tick Loop   │  │  Sorted Set     │   │    │
    │  │  │  (every 1s)  │  │  Scanner        │   │    │
    │  │  └──────┬───────┘  └────────┬────────┘   │    │
    │  │         │                   │             │    │
    │  │         └────────┬──────────┘             │    │
    │  │                  ▼                        │    │
    │  │  ┌──────────────────────────┐            │    │
    │  │  │  Trigger Dispatcher      │            │    │
    │  │  │  (batch trigger + lock)  │            │    │
    │  │  └──────────┬───────────────┘            │    │
    │  └─────────────┼────────────────────────────┘    │
    │                │                                  │
    │  ┌─────────────▼────────────────────────────┐    │
    │  │  Cron Expression Evaluator                │    │
    │  │  ┌─────────┐ ┌──────────┐ ┌───────────┐ │    │
    │  │  │  Unix   │ │  Quartz  │ │ EventBridge│ │    │
    │  │  │  Parser │ │  Parser  │ │  Parser    │ │    │
    │  │  └─────────┘ └──────────┘ └───────────┘ │    │
    │  └──────────────────────────────────────────┘    │
    │                                                   │
    │  ┌──────────────────────────────────────────┐    │
    │  │  Calendar Engine                          │    │
    │  │  (holiday detection, business hours)       │    │
    │  └──────────────────────────────────────────┘    │
    │                                                   │
    │  ┌──────────────────────────────────────────┐    │
    │  │  Missed Trigger Detector                  │    │
    │  │  (runs every 5 min)                       │    │
    │  └──────────────────────────────────────────┘    │
    └───────┬──────────────────────────────────────────┘
            │
            │ Submit one-shot job for each trigger
            ▼
    ┌───────────────────────┐
    │  Distributed Job      │
    │  Scheduler            │
    │  (from DJS design)    │
    └───────┬───────────────┘
            │
    ┌───────▼───────────────┐  ┌────────┐  ┌──────────────┐
    │  MySQL                │  │ Redis  │  │ etcd         │
    │  - cron_job           │  │ - next │  │ - leader     │
    │  - trigger_history    │  │   exec │  │   election   │
    │  - calendar           │  │   sorted│  │              │
    │                       │  │   set  │  │              │
    │                       │  │ - locks│  │              │
    └───────────────────────┘  └────────┘  └──────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Cron Trigger Engine** | Core loop: every second, scans for due triggers and fires them |
| **Tick Loop** | 1-second timer driving the trigger evaluation cycle |
| **Sorted Set Scanner** | Redis ZRANGEBYSCORE to find all cron jobs with next_execution <= now |
| **Trigger Dispatcher** | Acquires distributed lock per cron job, submits one-shot job to the job scheduler |
| **Cron Expression Evaluator** | Parses cron expressions (Unix, Quartz, EventBridge) and calculates next fire time |
| **Calendar Engine** | Evaluates calendar rules to skip holidays and non-business-hours |
| **Missed Trigger Detector** | Periodic sweep to find cron jobs that should have fired but didn't |
| **Distributed Job Scheduler** | Receives one-shot job submissions from the cron service and executes them |

### Data Flows

**Primary: Cron Trigger Fire**
1. Tick Loop fires every 1 second
2. Scanner queries Redis sorted set: `ZRANGEBYSCORE next_exec_set 0 {current_time_ms}`
3. For each due cron job:
   a. Acquire Redis distributed lock: `SET lock:cron:{job_id}:{fire_time} NX EX 60`
   b. If lock acquired: submit one-shot job to the job scheduler
   c. Calculate next execution time (cron expression + timezone + calendar)
   d. Update Redis sorted set with new next_execution_time
   e. Record trigger in MySQL trigger_history
4. If lock not acquired: skip (another instance already fired it)

**Secondary: Cron Job Creation**
1. User submits cron job via API
2. Validate cron expression, timezone, calendar reference
3. Calculate next execution time from now
4. Persist to MySQL (cron_job table)
5. Add to Redis sorted set (score = next_execution_time_ms)
6. Return cron job ID and next 5 fire times (preview)

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Cron job definition
CREATE TABLE cron_job (
    cron_job_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id           VARCHAR(64)     NOT NULL,
    job_name            VARCHAR(255)    NOT NULL,
    cron_expression     VARCHAR(128)    NOT NULL COMMENT 'Unix/Quartz/EventBridge format',
    expression_format   ENUM('UNIX_5','QUARTZ_6','QUARTZ_7','EVENTBRIDGE_RATE') NOT NULL DEFAULT 'UNIX_5',
    timezone            VARCHAR(64)     NOT NULL DEFAULT 'UTC',
    calendar_id         BIGINT UNSIGNED NULL COMMENT 'Optional: skip holidays/non-business hours',
    command             TEXT            NOT NULL,
    container_image     VARCHAR(512)    NULL,
    resource_request    JSON            NOT NULL DEFAULT '{"cpu_millis":500,"memory_mb":512}',
    priority            SMALLINT        DEFAULT 500,
    max_retries         INT UNSIGNED    DEFAULT 3,
    timeout_seconds     INT UNSIGNED    DEFAULT 3600,
    concurrency_policy  ENUM('ALLOW','SKIP','QUEUE') NOT NULL DEFAULT 'SKIP'
        COMMENT 'ALLOW=overlap OK, SKIP=skip if prev running, QUEUE=wait for prev',
    misfire_policy      ENUM('FIRE_NOW','SKIP_TO_NEXT','FIRE_ALL_MISSED') NOT NULL DEFAULT 'FIRE_NOW',
    max_misfire_count   INT UNSIGNED    DEFAULT 10 COMMENT 'Max missed fires to recover',
    enabled             BOOLEAN         DEFAULT TRUE,
    paused              BOOLEAN         DEFAULT FALSE,
    next_fire_time      TIMESTAMP(6)    NULL COMMENT 'Denormalized for query convenience',
    last_fire_time      TIMESTAMP(6)    NULL,
    last_fire_status    ENUM('SUCCEEDED','FAILED','RUNNING','SKIPPED') NULL,
    fire_count          BIGINT UNSIGNED DEFAULT 0,
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    updated_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    version             INT UNSIGNED    DEFAULT 1,
    UNIQUE KEY uk_tenant_name (tenant_id, job_name),
    INDEX idx_enabled_next (enabled, paused, next_fire_time),
    INDEX idx_tenant (tenant_id),
    INDEX idx_next_fire (next_fire_time) COMMENT 'For missed trigger detection'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Trigger history
CREATE TABLE trigger_history (
    trigger_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    cron_job_id         BIGINT UNSIGNED NOT NULL,
    scheduled_fire_time TIMESTAMP(6)    NOT NULL COMMENT 'When the cron should have fired',
    actual_fire_time    TIMESTAMP(6)    NOT NULL COMMENT 'When we actually fired it',
    fire_delay_ms       INT             NOT NULL COMMENT 'actual - scheduled, may be negative for early',
    trigger_type        ENUM('SCHEDULED','MANUAL','MISFIRE_RECOVERY') NOT NULL DEFAULT 'SCHEDULED',
    execution_id        BIGINT UNSIGNED NULL COMMENT 'Reference to job_execution in DJS',
    skipped             BOOLEAN         DEFAULT FALSE COMMENT 'Skipped due to concurrency policy',
    skip_reason         VARCHAR(255)    NULL,
    fired_by_instance   VARCHAR(128)    NOT NULL COMMENT 'Which scheduler instance fired this',
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_cron_job (cron_job_id, scheduled_fire_time DESC),
    INDEX idx_scheduled (scheduled_fire_time),
    INDEX idx_delay (fire_delay_ms) COMMENT 'For SLA monitoring',
    FOREIGN KEY (cron_job_id) REFERENCES cron_job(cron_job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (TO_DAYS(created_at)) (
    PARTITION p_2026_04 VALUES LESS THAN (TO_DAYS('2026-05-01')),
    PARTITION p_2026_05 VALUES LESS THAN (TO_DAYS('2026-06-01')),
    PARTITION p_2026_06 VALUES LESS THAN (TO_DAYS('2026-07-01')),
    PARTITION p_future  VALUES LESS THAN MAXVALUE
);

-- Calendar definitions
CREATE TABLE calendar (
    calendar_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id           VARCHAR(64)     NOT NULL,
    calendar_name       VARCHAR(128)    NOT NULL,
    description         TEXT            NULL,
    timezone            VARCHAR(64)     NOT NULL DEFAULT 'UTC',
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    updated_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    UNIQUE KEY uk_tenant_cal (tenant_id, calendar_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Calendar rules (holidays, business hours)
CREATE TABLE calendar_rule (
    rule_id             BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    calendar_id         BIGINT UNSIGNED NOT NULL,
    rule_type           ENUM('EXCLUDE_DATE','EXCLUDE_DATE_RANGE','EXCLUDE_DOW',
                             'INCLUDE_HOURS','EXCLUDE_HOURS') NOT NULL,
    -- For EXCLUDE_DATE: specific date to skip
    exclude_date        DATE            NULL,
    -- For EXCLUDE_DATE_RANGE: range to skip
    range_start         DATE            NULL,
    range_end           DATE            NULL,
    -- For EXCLUDE_DOW: day of week to skip (1=Mon, 7=Sun)
    day_of_week         TINYINT         NULL,
    -- For INCLUDE/EXCLUDE_HOURS: hour range
    hour_start          TINYINT         NULL COMMENT '0-23',
    hour_end            TINYINT         NULL COMMENT '0-23',
    -- Recurrence
    recurring_yearly    BOOLEAN         DEFAULT FALSE COMMENT 'For holidays that repeat annually',
    description         VARCHAR(255)    NULL COMMENT 'e.g., Christmas Day, Company Holiday',
    FOREIGN KEY (calendar_id) REFERENCES calendar(calendar_id),
    INDEX idx_calendar (calendar_id, rule_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Tenant cron quotas
CREATE TABLE tenant_cron_quota (
    tenant_id           VARCHAR(64)     PRIMARY KEY,
    max_cron_jobs       INT UNSIGNED    NOT NULL DEFAULT 10000,
    max_triggers_per_min INT UNSIGNED   NOT NULL DEFAULT 1000,
    min_interval_seconds INT UNSIGNED   NOT NULL DEFAULT 60 COMMENT 'Minimum cron interval allowed',
    current_job_count   INT UNSIGNED    DEFAULT 0,
    INDEX idx_tenant (tenant_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Component | Store | Rationale |
|-----------|-------|-----------|
| Cron job definitions | MySQL | Durable, ACID, joins with tenant quotas |
| Next-execution sorted set | Redis | O(log N) ZADD/ZRANGEBYSCORE; real-time trigger scanning |
| Distributed trigger locks | Redis | SETNX with TTL for exactly-once trigger |
| Trigger history | MySQL (partitioned monthly) | Audit trail, SLA reporting |
| Calendar rules | MySQL + JVM cache | Infrequently updated, frequently read |

### Indexing Strategy

| Index | Table | Purpose |
|-------|-------|---------|
| `idx_enabled_next` | cron_job | Active job scan, missed trigger detection |
| `idx_cron_job` | trigger_history | Per-job trigger history |
| `idx_delay` | trigger_history | SLA monitoring: find late triggers |
| `idx_calendar` | calendar_rule | Calendar evaluation during next-fire-time calculation |

---

## 5. API Design

### REST API Endpoints

```
POST /api/v1/cron-jobs
Authorization: Bearer <JWT>
X-Tenant-Id: team-dba
Content-Type: application/json

Request:
{
  "job_name": "nightly-backup-primary",
  "cron_expression": "0 2 * * *",
  "expression_format": "UNIX_5",
  "timezone": "America/New_York",
  "calendar_id": null,
  "command": "/opt/scripts/mysql-backup.sh --full --compress",
  "container_image": "internal-registry/backup-agent:3.2.1",
  "resource_request": {"cpu_millis": 2000, "memory_mb": 4096},
  "priority": 700,
  "max_retries": 3,
  "timeout_seconds": 7200,
  "concurrency_policy": "SKIP",
  "misfire_policy": "FIRE_NOW"
}

Response (201):
{
  "cron_job_id": 12345678,
  "job_name": "nightly-backup-primary",
  "status": "ENABLED",
  "next_fire_time": "2026-04-10T06:00:00Z",
  "next_5_fire_times": [
    "2026-04-10T06:00:00Z",
    "2026-04-11T06:00:00Z",
    "2026-04-12T06:00:00Z",
    "2026-04-13T06:00:00Z",
    "2026-04-14T06:00:00Z"
  ],
  "created_at": "2026-04-09T14:30:00.123Z"
}
```

```
GET /api/v1/cron-jobs/{cron_job_id}
GET /api/v1/cron-jobs?tenant_id=X&enabled=true&page=1&size=50
PUT /api/v1/cron-jobs/{cron_job_id}           # Full update
PATCH /api/v1/cron-jobs/{cron_job_id}         # Partial update (e.g., change schedule)
DELETE /api/v1/cron-jobs/{cron_job_id}
POST /api/v1/cron-jobs/{cron_job_id}/pause
POST /api/v1/cron-jobs/{cron_job_id}/resume
POST /api/v1/cron-jobs/{cron_job_id}/trigger  # Manual trigger (immediate)
GET /api/v1/cron-jobs/{cron_job_id}/triggers?limit=20  # Trigger history
GET /api/v1/cron-jobs/{cron_job_id}/next-fires?count=10 # Preview next N fires
```

```
POST /api/v1/cron-expression/validate
Request: {"expression": "0/15 * * * * ?", "format": "QUARTZ_6", "timezone": "US/Eastern"}
Response: {
  "valid": true,
  "human_readable": "Every 15 seconds",
  "next_5_fires": ["2026-04-09T14:30:15Z", ...]
}
```

```
POST /api/v1/calendars
Request: {
  "calendar_name": "us-business-hours",
  "timezone": "America/New_York",
  "rules": [
    {"rule_type": "EXCLUDE_DOW", "day_of_week": 6, "description": "Saturday"},
    {"rule_type": "EXCLUDE_DOW", "day_of_week": 7, "description": "Sunday"},
    {"rule_type": "INCLUDE_HOURS", "hour_start": 9, "hour_end": 17, "description": "Business hours only"},
    {"rule_type": "EXCLUDE_DATE", "exclude_date": "2026-12-25", "recurring_yearly": true, "description": "Christmas"},
    {"rule_type": "EXCLUDE_DATE", "exclude_date": "2026-07-04", "recurring_yearly": true, "description": "Independence Day"}
  ]
}
```

### CLI Design

```bash
# Cron job management
cronctl create -f cron-job.yaml
cronctl create --name "db-backup" --cron "0 2 * * *" --tz "America/New_York" \
  --cmd "/opt/scripts/backup.sh" --image backup-agent:3.2.1

cronctl list --tenant team-dba [--enabled] [--paused]
cronctl get <cron_job_id> --show-next 10
cronctl update <cron_job_id> --cron "0 3 * * *"    # Change schedule
cronctl delete <cron_job_id>
cronctl pause <cron_job_id>
cronctl resume <cron_job_id>
cronctl trigger <cron_job_id>                       # Manual fire

# Cron expression tools
cronctl validate "0 2 * * *"
# Output: Valid UNIX 5-field expression
#   Human: "At 02:00 AM every day"
#   Next 5 fires (UTC): 2026-04-10 02:00, 2026-04-11 02:00, ...

cronctl validate "0 0/15 * * * ?" --format quartz
# Output: Valid Quartz 6-field expression
#   Human: "Every 15 minutes"

# Trigger history
cronctl triggers <cron_job_id> --last 20
# Output:
# TRIGGER_ID  SCHEDULED_TIME        ACTUAL_TIME           DELAY   TYPE       STATUS
# 99001       2026-04-09T06:00:00Z  2026-04-09T06:00:00Z  0ms    SCHEDULED  SUCCEEDED
# 99000       2026-04-08T06:00:00Z  2026-04-08T06:00:01Z  1ms    SCHEDULED  SUCCEEDED
# 98999       2026-04-07T06:00:00Z  2026-04-07T06:00:00Z  0ms    SCHEDULED  FAILED
# 98998       2026-04-06T06:00:00Z  2026-04-06T06:05:30Z  330s   MISFIRE    SUCCEEDED

# Calendar management
cronctl calendar create -f us-business-hours.yaml
cronctl calendar list
cronctl calendar test us-business-hours --date "2026-12-25"
# Output: 2026-12-25 (Friday) is EXCLUDED (Christmas Day)

cronctl calendar test us-business-hours --date "2026-04-09" --time "14:30"
# Output: 2026-04-09 14:30 (Wednesday) is INCLUDED (business hours)

# Monitoring
cronctl stats
# Output:
# METRIC                  VALUE
# Active cron jobs        2,012,345
# Triggers last hour      1,234,567
# Avg trigger delay       12ms
# P99 trigger delay       450ms
# Missed triggers (24h)   3
# Upcoming triggers (1m)  8,432

cronctl missed --last 24h
# Output:
# CRON_JOB_ID  NAME                 MISSED_TIME           RECOVERED  REASON
# 55001        cert-rotation-proxy  2026-04-09T03:00:00Z  YES        Scheduler failover
# 55002        log-cleanup-node-7   2026-04-09T04:15:00Z  YES        Redis lock contention
```

---

## 6. Core Component Deep Dives

### 6.1 Cron Trigger Engine

**Why it's hard:** At 2M active cron jobs, we can't re-evaluate every cron expression every second. We need an efficient data structure to find which jobs are due. The sorted set approach (score = next_execution_time_ms) gives O(log N) insertion and O(K + log N) range query where K is the number of due jobs. But we must handle: clock skew, missed windows, thundering herd at popular times (:00, :30), and exactly-once trigger semantics across multiple scheduler instances.

**Approaches Compared:**

| Approach | Evaluation Cost | Missed Fire Handling | Scalability | Clock Sensitivity |
|----------|----------------|---------------------|-------------|-------------------|
| Poll all jobs every second | O(N) per second | Implicit (re-check) | Poor at 2M jobs | Low |
| Timer wheel (Hashed) | O(1) insert, O(1) tick | Must reconstruct on restart | Good | Medium |
| Sorted set (next_fire_time as score) | O(log N) insert, O(K+log N) query | Must detect gaps | Excellent | Medium |
| Quartz JDBC JobStore | O(K) per fire cycle | Built-in misfire handling | Good (battle-tested) | Low |

**Selected: Redis sorted set** -- At 2M active jobs, a sorted set query `ZRANGEBYSCORE 0 {now_ms}` returns only the due jobs (typically 1K-10K in a 1-second window). O(K + log 2M) = O(K + 21), effectively O(K). Far more efficient than polling all 2M jobs.

**Implementation:**

```java
@Service
public class CronTriggerEngine {

    private static final Duration TICK_INTERVAL = Duration.ofSeconds(1);
    private static final int BATCH_SIZE = 1000;
    private static final Duration LOCK_TTL = Duration.ofSeconds(60);
    
    @Autowired private StringRedisTemplate redis;
    @Autowired private CronJobRepository cronJobRepo;
    @Autowired private TriggerHistoryRepository triggerRepo;
    @Autowired private CronExpressionEvaluator evaluator;
    @Autowired private CalendarEngine calendarEngine;
    @Autowired private JobSchedulerClient jobScheduler;
    @Autowired private LeaderElector leaderElector;
    @Autowired private MeterRegistry metrics;
    
    private final ScheduledExecutorService tickExecutor = 
        Executors.newSingleThreadScheduledExecutor(
            new ThreadFactoryBuilder().setNameFormat("cron-tick-%d").build());
    
    // Thread pool for parallel trigger processing
    private final ExecutorService triggerPool = 
        new ThreadPoolExecutor(16, 16, 60, TimeUnit.SECONDS,
            new LinkedBlockingQueue<>(10000),
            new ThreadFactoryBuilder().setNameFormat("cron-trigger-%d").build());
    
    @PostConstruct
    public void start() {
        tickExecutor.scheduleAtFixedRate(
            this::tick, 0, TICK_INTERVAL.toMillis(), TimeUnit.MILLISECONDS);
    }
    
    /**
     * Main tick: fires every second. Scans for due cron jobs and triggers them.
     */
    private void tick() {
        if (!leaderElector.isLeader()) return;
        
        Timer.Sample sample = Timer.start(metrics);
        long nowMs = Instant.now().toEpochMilli();
        
        try {
            // 1. Query Redis sorted set for all jobs due at or before now
            Set<String> dueJobIds = redis.opsForZSet()
                .rangeByScore("cron:next_exec", 0, nowMs, 0, BATCH_SIZE);
            
            if (dueJobIds == null || dueJobIds.isEmpty()) {
                return;
            }
            
            metrics.gauge("cron.tick.due_jobs", dueJobIds.size());
            
            // 2. Process each due job in parallel
            List<CompletableFuture<Void>> futures = new ArrayList<>();
            for (String jobIdStr : dueJobIds) {
                long cronJobId = Long.parseLong(jobIdStr);
                futures.add(CompletableFuture.runAsync(
                    () -> processTrigger(cronJobId, nowMs), triggerPool));
            }
            
            // Wait for all triggers to complete (with timeout)
            CompletableFuture.allOf(futures.toArray(new CompletableFuture[0]))
                .get(5, TimeUnit.SECONDS);
            
        } catch (Exception e) {
            log.error("Cron tick failed", e);
            metrics.counter("cron.tick.error").increment();
        } finally {
            sample.stop(metrics.timer("cron.tick.duration"));
        }
    }
    
    /**
     * Process a single cron job trigger.
     */
    private void processTrigger(long cronJobId, long nowMs) {
        try {
            // 1. Acquire distributed lock (prevent duplicate firing)
            Double score = redis.opsForZSet().score("cron:next_exec", String.valueOf(cronJobId));
            if (score == null) return; // Already processed by another tick
            
            long scheduledFireMs = score.longValue();
            String lockKey = String.format("lock:cron:%d:%d", cronJobId, scheduledFireMs);
            
            Boolean acquired = redis.opsForValue()
                .setIfAbsent(lockKey, leaderElector.getInstanceId(), LOCK_TTL);
            
            if (!Boolean.TRUE.equals(acquired)) {
                return; // Another instance already processing this trigger
            }
            
            // 2. Load cron job definition
            CronJob cronJob = cronJobRepo.findById(cronJobId).orElse(null);
            if (cronJob == null || !cronJob.isEnabled() || cronJob.isPaused()) {
                // Job was deleted/disabled/paused between sorted set scan and now
                redis.opsForZSet().remove("cron:next_exec", String.valueOf(cronJobId));
                return;
            }
            
            // 3. Check concurrency policy
            if (cronJob.getConcurrencyPolicy() == ConcurrencyPolicy.SKIP) {
                boolean previousRunning = jobScheduler.isJobRunning(cronJobId);
                if (previousRunning) {
                    log.info("Skipping cron job {} (previous execution still running)", cronJobId);
                    recordSkippedTrigger(cronJob, scheduledFireMs, "Previous execution still running");
                    advanceNextFireTime(cronJob);
                    return;
                }
            }
            
            // 4. Check calendar
            if (cronJob.getCalendarId() != null) {
                ZonedDateTime fireTime = Instant.ofEpochMilli(scheduledFireMs)
                    .atZone(ZoneId.of(cronJob.getTimezone()));
                if (calendarEngine.isExcluded(cronJob.getCalendarId(), fireTime)) {
                    log.info("Skipping cron job {} (calendar exclusion)", cronJobId);
                    recordSkippedTrigger(cronJob, scheduledFireMs, "Calendar exclusion");
                    advanceNextFireTime(cronJob);
                    return;
                }
            }
            
            // 5. Submit one-shot job to the job scheduler
            long executionId = jobScheduler.submitOneShot(
                cronJob.getCommand(),
                cronJob.getContainerImage(),
                cronJob.getResourceRequest(),
                cronJob.getPriority(),
                cronJob.getMaxRetries(),
                cronJob.getTimeoutSeconds(),
                cronJob.getTenantId(),
                Map.of("cron_job_id", String.valueOf(cronJobId),
                       "scheduled_fire_time", String.valueOf(scheduledFireMs))
            );
            
            // 6. Record trigger
            long fireDelayMs = nowMs - scheduledFireMs;
            TriggerHistory trigger = new TriggerHistory();
            trigger.setCronJobId(cronJobId);
            trigger.setScheduledFireTime(Instant.ofEpochMilli(scheduledFireMs));
            trigger.setActualFireTime(Instant.now());
            trigger.setFireDelayMs((int) fireDelayMs);
            trigger.setTriggerType(TriggerType.SCHEDULED);
            trigger.setExecutionId(executionId);
            trigger.setFiredByInstance(leaderElector.getInstanceId());
            triggerRepo.save(trigger);
            
            // 7. Update cron job state
            cronJobRepo.updateLastFire(cronJobId, Instant.now());
            
            // 8. Calculate and set next fire time
            advanceNextFireTime(cronJob);
            
            metrics.counter("cron.triggered", "tenant", cronJob.getTenantId()).increment();
            metrics.timer("cron.trigger.delay").record(Duration.ofMillis(Math.max(0, fireDelayMs)));
            
        } catch (Exception e) {
            log.error("Failed to process cron trigger for job {}", cronJobId, e);
            metrics.counter("cron.trigger.error").increment();
            // Don't remove from sorted set on error -- it will be retried on next tick
        }
    }
    
    /**
     * Calculate next fire time and update the sorted set.
     */
    private void advanceNextFireTime(CronJob cronJob) {
        ZonedDateTime now = ZonedDateTime.now(ZoneId.of(cronJob.getTimezone()));
        ZonedDateTime nextFire = evaluator.getNextFireTime(
            cronJob.getCronExpression(), 
            cronJob.getExpressionFormat(),
            now,
            cronJob.getTimezone()
        );
        
        // Apply calendar: skip excluded times
        if (cronJob.getCalendarId() != null) {
            int maxSkips = 1000; // Safety: don't loop forever
            int skips = 0;
            while (calendarEngine.isExcluded(cronJob.getCalendarId(), nextFire) 
                    && skips < maxSkips) {
                nextFire = evaluator.getNextFireTimeAfter(
                    cronJob.getCronExpression(),
                    cronJob.getExpressionFormat(),
                    nextFire,
                    cronJob.getTimezone()
                );
                skips++;
            }
        }
        
        long nextFireMs = nextFire.toInstant().toEpochMilli();
        
        // Update Redis sorted set
        redis.opsForZSet().add("cron:next_exec", 
            String.valueOf(cronJob.getCronJobId()), nextFireMs);
        
        // Update MySQL (denormalized)
        cronJobRepo.updateNextFireTime(cronJob.getCronJobId(), nextFire.toInstant());
    }
}
```

**Failure Modes:**
- **Redis sorted set lost (Redis restart):** Rebuild from MySQL: `SELECT cron_job_id, next_fire_time FROM cron_job WHERE enabled = TRUE AND paused = FALSE`. Insert all into sorted set. Takes ~30s for 2M jobs.
- **Trigger lock contention (two instances try to fire same job):** SETNX guarantees only one acquires the lock. The loser skips. The winner's lock auto-expires after 60s, so even if it crashes, the lock is eventually released.
- **Tick takes longer than 1 second:** Next tick is scheduled at fixed rate, so it fires immediately when the previous tick completes. We monitor tick duration and alert if P99 > 900ms (risk of drifting).
- **Thundering herd at :00 minute:** Many cron jobs fire at the top of the hour. The batch size limit (1000 per tick) and 16-thread trigger pool handle this. At 100K triggers/min peak, that's ~1700/second spread across 16 threads = ~106 triggers/thread/second. Each trigger takes ~5ms (Redis lock + API call), so 16 threads can handle 16 * 200 = 3200 triggers/second -- sufficient headroom.

**Interviewer Q&A:**

**Q1: Why a sorted set instead of Quartz Scheduler's JDBC JobStore?**
A: Quartz JDBC JobStore queries MySQL every trigger cycle: `SELECT * FROM qrtz_triggers WHERE next_fire_time <= ? ORDER BY next_fire_time LIMIT ?`. At 2M active jobs, even with an index this is slower than Redis ZRANGEBYSCORE. Redis sorted set is entirely in-memory, sub-millisecond. We use Quartz-style cron expression parsing but not Quartz's scheduling infrastructure.

**Q2: How do you handle the case where ZRANGEBYSCORE returns more jobs than you can process in 1 second?**
A: The `BATCH_SIZE = 1000` limit ensures we never try to process more than 1000 triggers per tick. If there are 5000 due jobs, we process 1000 in the first tick, and the remaining 4000 remain in the sorted set (their scores are still <= now) and are picked up by subsequent ticks. The fire delay for later batches is 1-5 seconds, which is acceptable.

**Q3: What's the distributed lock's failure mode if the lock-holder crashes before releasing?**
A: The lock has a 60-second TTL (EX 60). If the holder crashes, the lock auto-expires after 60 seconds. The cron job's next_execution_time is NOT advanced (since the holder crashed before updating it). On the next tick (after lock expiry), the job is still due and re-processed. The misfire policy determines how the late trigger is handled.

**Q4: How do you handle "fire_count" for audit without MySQL write contention?**
A: We batch MySQL updates using a local counter + periodic flush. Each trigger increments an in-memory counter. Every 10 seconds, we flush the counters to MySQL: `UPDATE cron_job SET fire_count = fire_count + ? WHERE cron_job_id = ?` for each dirty counter. This converts 100K individual writes into 100K batched writes every 10 seconds. MySQL handles this via batch update + InnoDB buffer pool.

**Q5: How do you guarantee the sorted set stays in sync with MySQL?**
A: MySQL is the source of truth. The sorted set is a derived index. On any cron job mutation (create, update, delete, pause, resume), we update MySQL first, then Redis. If the Redis update fails, a reconciliation process (runs every 60s) detects discrepancies by comparing `cron_job.next_fire_time` with the Redis sorted set score. Reconciliation is idempotent: it re-adds or removes entries as needed.

**Q6: How would you shard the trigger engine for 100M active cron jobs?**
A: Partition the sorted set by tenant_id hash into N shards: `cron:next_exec:shard-0` through `cron:next_exec:shard-15`. Each shard is on a different Redis node (Redis Cluster). The trigger engine is also sharded: each instance owns a subset of shards. Leader election is per-shard. This scales linearly: 16 shards handle 16x the throughput.

---

### 6.2 Timezone and DST Handling

**Why it's hard:** Daylight Saving Time creates two edge cases. (1) Spring forward: a time gap occurs (e.g., 2:00 AM becomes 3:00 AM in US Eastern). Cron jobs scheduled at 2:30 AM don't have a valid fire time. (2) Fall back: a time ambiguity occurs (e.g., 1:30 AM happens twice). Cron jobs scheduled at 1:30 AM could fire twice. Additionally, different countries change DST on different dates, and some change the rules year-to-year.

**Approaches Compared:**

| Approach | Spring Forward | Fall Back | Complexity | Standard |
|----------|---------------|-----------|------------|----------|
| Store all times in UTC, convert on display | Fire at UTC equivalent (may be "wrong" local time) | Fire at UTC equivalent | Low | Common but incorrect for "every day at 2 AM local" |
| Evaluate in local timezone, handle gaps/overlaps | Skip gap, fire at transition | Fire only first occurrence | Medium | Quartz default |
| Evaluate in local timezone, fire at transition for gap, fire both for overlap | Fire at 3:00 AM (transition) | Fire twice | Medium | cron daemon behavior |
| Let user choose per-job policy | Flexible | Flexible | High | AWS EventBridge approach |

**Selected: Evaluate in local timezone with configurable policy** -- Default: skip gaps (fire at next valid time), fire only first occurrence of overlaps. This matches Quartz Scheduler's `withMisfireHandlingInstructionFireAndProceed` behavior. Users can override per-job.

**Implementation:**

```java
@Service
public class CronExpressionEvaluator {
    
    /**
     * Calculate the next fire time after a given reference time,
     * respecting timezone and DST transitions.
     */
    public ZonedDateTime getNextFireTime(String expression, ExpressionFormat format,
                                          ZonedDateTime after, String timezone) {
        ZoneId zone = ZoneId.of(timezone);
        CronExpression cron = parseCronExpression(expression, format);
        
        ZonedDateTime candidate = cron.nextAfter(after);
        
        // Handle DST transitions
        candidate = handleDSTTransition(candidate, zone, cron, after);
        
        return candidate;
    }
    
    private ZonedDateTime handleDSTTransition(ZonedDateTime candidate, ZoneId zone,
                                                CronExpression cron, ZonedDateTime after) {
        
        ZoneRules rules = zone.getRules();
        
        // Check if candidate falls in a DST gap (spring forward)
        if (!rules.isValidOffset(candidate.toLocalDateTime(), candidate.getOffset())) {
            ZoneOffsetTransition transition = rules.getTransition(candidate.toLocalDateTime());
            
            if (transition != null && transition.isGap()) {
                // The scheduled time doesn't exist (e.g., 2:30 AM during spring forward)
                // Policy: fire at the transition time (e.g., 3:00 AM)
                log.info("Cron fire time {} falls in DST gap in {}. Adjusting to transition time {}.",
                    candidate.toLocalDateTime(), zone, transition.getDateTimeAfter());
                
                // Reconstruct the ZonedDateTime at the transition time
                candidate = transition.getDateTimeAfter().atZone(zone);
                
                // But we need to verify this adjusted time still matches the cron pattern
                // If it doesn't, skip to the next valid fire time
                if (!cron.matches(candidate)) {
                    candidate = cron.nextAfter(candidate);
                }
            }
        }
        
        // Check if candidate falls in a DST overlap (fall back)
        ZoneOffsetTransition overlapTransition = rules.getTransition(candidate.toLocalDateTime());
        if (overlapTransition != null && overlapTransition.isOverlap()) {
            // The scheduled time occurs twice (e.g., 1:30 AM during fall back)
            // Policy: fire only on the FIRST occurrence (before the transition)
            ZoneOffset earlierOffset = overlapTransition.getOffsetBefore();
            candidate = candidate.toLocalDateTime().atOffset(earlierOffset).toZonedDateTime();
            
            log.info("Cron fire time {} falls in DST overlap in {}. Using first occurrence (offset {}).",
                candidate.toLocalDateTime(), zone, earlierOffset);
        }
        
        return candidate;
    }
    
    /**
     * Parse different cron expression formats.
     */
    private CronExpression parseCronExpression(String expression, ExpressionFormat format) {
        return switch (format) {
            case UNIX_5 -> CronExpression.parseUnix(expression);
                // minute hour day-of-month month day-of-week
                // "0 2 * * *" = daily at 2:00 AM
                
            case QUARTZ_6 -> CronExpression.parseQuartz6(expression);
                // second minute hour day-of-month month day-of-week
                // "0 0 2 * * ?" = daily at 2:00:00 AM
                
            case QUARTZ_7 -> CronExpression.parseQuartz7(expression);
                // second minute hour day-of-month month day-of-week year
                // "0 0 2 * * ? 2026" = daily at 2:00 AM in 2026 only
                
            case EVENTBRIDGE_RATE -> parseEventBridgeRate(expression);
                // "rate(5 minutes)" = every 5 minutes
                // Converted to fixed-interval schedule
        };
    }
}
```

**DST Edge Cases Cheat Sheet:**

| Scenario | Cron | Timezone | DST Event | Behavior |
|----------|------|----------|-----------|----------|
| Gap: 2:30 AM doesn't exist | `30 2 * * *` | US/Eastern | Spring forward (2AM→3AM) | Fire at 3:00 AM (transition) |
| Gap: every 15 min through gap | `*/15 * * * *` | US/Eastern | Spring forward | 1:45, 3:00, 3:15 (2:00 and 2:15 skipped) |
| Overlap: 1:30 AM happens twice | `30 1 * * *` | US/Eastern | Fall back (2AM→1AM) | Fire at first 1:30 AM only |
| Overlap: every 15 min | `*/15 * * * *` | US/Eastern | Fall back | ...1:30(1st), 1:45(1st), 1:00(2nd), 1:15(2nd)... normal from 2:00 |
| No DST: UTC | `0 2 * * *` | UTC | None | Always 2:00 AM UTC |
| Country changes DST rules | `0 2 * * *` | Asia/Gaza | Political change | IANA tzdata update; service restart picks up new rules |

**Interviewer Q&A:**

**Q1: How do you keep timezone data up to date?**
A: We use the IANA timezone database (tzdata), which is included in the JDK's `java.time` library. When countries change DST rules (happens several times a year), we update the JDK/tzdata package and redeploy. For zero-downtime updates, we support hot-reloading tzdata via a JVM agent or by using the `java.time.zone.DefaultZoneRulesProvider` SPI.

**Q2: What if a user specifies an invalid timezone?**
A: We validate the timezone at cron job creation time: `ZoneId.of(timezone)` throws `ZoneRulesException` for invalid zones. We also validate against a curated allowlist of standard IANA zone IDs (e.g., `America/New_York`, not `EST` which is ambiguous). The API returns 400 Bad Request with a suggestion of valid zones.

**Q3: How do you test DST handling?**
A: We use parameterized tests with known DST transition dates across multiple timezones (US, EU, Australia, Brazil). Each test case specifies: cron expression, timezone, reference time (before transition), expected next fire time (after transition). We also test edge cases: midnight transitions, sub-hour offset changes (e.g., Lord Howe Island, +10:30/+11:00), and countries that abolished DST.

**Q4: A user wants their job to run "every day at 2 AM local time." Is this the same as running at 6 AM UTC (for US Eastern)?**
A: No! "Every day at 2 AM local time" means the UTC equivalent changes twice a year: 7 AM UTC (EST) and 6 AM UTC (EDT). If you store the schedule as "0 6 * * * UTC", the job runs at 2 AM EDT but 1 AM EST. We always evaluate in the specified timezone to get the correct behavior.

**Q5: How do you handle cron expressions like "last day of the month" in different timezones?**
A: Quartz format supports `L` for last day: `0 0 2 L * ?` = 2 AM on the last day of every month. The "last day" is determined in the job's timezone. If a job is in `Pacific/Auckland` (UTC+12/+13), the last day in NZDT may differ from UTC by one day. We evaluate the cron expression in the local timezone, then convert to UTC for storage in the sorted set.

**Q6: What about leap seconds?**
A: Java's `Instant.now()` does not account for leap seconds (it uses UTC with leap second smearing). Our 1-second tick granularity means a leap second causes at most a 1-second trigger delay, which is within SLA. We do not attempt leap-second-precise scheduling -- this is a cron service, not a real-time system.

---

### 6.3 Missed Execution Detection & Recovery

**Why it's hard:** If the cron service is down for 5 minutes (scheduler failover, Redis outage), cron jobs that should have fired during the outage are "missed." The recovery must: detect all missed fires, decide what to do (fire now vs. skip), and execute recovery without duplicates or storm effects.

**Approaches Compared:**

| Approach | Detection | Recovery Speed | Duplicate Risk |
|----------|----------|---------------|----------------|
| Compare last_fire_time vs. expected schedule | Exact | Requires recalculating all schedules | Low |
| Compare next_fire_time vs. now (it's in the past) | Fast | Immediate (sorted set scan) | Low |
| Event sourcing (expected triggers vs. actual triggers) | Exact | Requires join | None |

**Selected: sorted set scan (next_fire_time < now after recovery)** -- When the service recovers, the tick loop naturally finds all overdue entries in the sorted set. No special recovery process needed -- the normal tick handles it. For jobs with `misfire_policy = FIRE_ALL_MISSED`, we need additional logic.

**Implementation:**

```java
@Service
public class MissedTriggerDetector {
    
    /**
     * Runs every 5 minutes. Detects cron jobs whose next_fire_time is in the past
     * but haven't been triggered. This catches cases where:
     * - The sorted set was corrupted/rebuilt
     * - A trigger lock expired without the job being fired
     * - The job was re-enabled after the fire time passed
     */
    @Scheduled(fixedRate = 300_000) // Every 5 minutes
    public void detectMissedTriggers() {
        if (!leaderElector.isLeader()) return;
        
        Instant now = Instant.now();
        // Threshold: if next_fire_time is more than 2 minutes in the past, it's missed
        Instant threshold = now.minus(2, ChronoUnit.MINUTES);
        
        List<CronJob> missedJobs = cronJobRepo.findEnabledWithNextFireTimeBefore(threshold);
        
        for (CronJob job : missedJobs) {
            handleMissedTrigger(job, now);
        }
        
        metrics.gauge("cron.missed.detected", missedJobs.size());
    }
    
    private void handleMissedTrigger(CronJob job, Instant now) {
        // Calculate how many fires were missed
        List<ZonedDateTime> missedFires = calculateMissedFires(job, now);
        
        if (missedFires.isEmpty()) return;
        
        log.warn("Cron job {} missed {} fire(s). Policy: {}. Earliest missed: {}", 
            job.getCronJobId(), missedFires.size(), 
            job.getMisfirePolicy(), missedFires.get(0));
        
        switch (job.getMisfirePolicy()) {
            case FIRE_NOW:
                // Fire once immediately (catch-up)
                triggerMisfireRecovery(job, missedFires.get(missedFires.size() - 1));
                // Advance to next regular fire time
                advanceNextFireTime(job);
                break;
                
            case SKIP_TO_NEXT:
                // Don't fire missed ones; just advance to the next scheduled time
                log.info("Skipping {} missed fires for cron job {} (policy: SKIP_TO_NEXT)",
                    missedFires.size(), job.getCronJobId());
                advanceNextFireTime(job);
                // Record skipped triggers for audit
                for (ZonedDateTime missed : missedFires) {
                    recordSkippedTrigger(job, missed.toInstant().toEpochMilli(),
                        "Missed (policy: SKIP_TO_NEXT)");
                }
                break;
                
            case FIRE_ALL_MISSED:
                // Fire all missed executions (up to max_misfire_count)
                int count = 0;
                for (ZonedDateTime missed : missedFires) {
                    if (count >= job.getMaxMisfireCount()) {
                        log.warn("Reached max misfire count ({}) for cron job {}",
                            job.getMaxMisfireCount(), job.getCronJobId());
                        break;
                    }
                    triggerMisfireRecovery(job, missed);
                    count++;
                }
                advanceNextFireTime(job);
                break;
        }
        
        // Alert: missed trigger detected
        alertService.sendMissedTriggerAlert(job, missedFires.size());
    }
    
    private List<ZonedDateTime> calculateMissedFires(CronJob job, Instant now) {
        List<ZonedDateTime> missed = new ArrayList<>();
        ZoneId zone = ZoneId.of(job.getTimezone());
        
        // Start from last_fire_time (or created_at if never fired)
        ZonedDateTime start = (job.getLastFireTime() != null)
            ? job.getLastFireTime().atZone(zone)
            : job.getCreatedAt().atZone(zone);
        
        CronExpression cron = evaluator.parse(job.getCronExpression(), job.getExpressionFormat());
        ZonedDateTime candidate = cron.nextAfter(start);
        
        while (candidate.toInstant().isBefore(now) && missed.size() < 1000) {
            // Check calendar exclusion
            if (job.getCalendarId() == null 
                    || !calendarEngine.isExcluded(job.getCalendarId(), candidate)) {
                missed.add(candidate);
            }
            candidate = cron.nextAfter(candidate);
        }
        
        return missed;
    }
    
    private void triggerMisfireRecovery(CronJob job, ZonedDateTime missedFireTime) {
        // Submit one-shot job with MISFIRE_RECOVERY type
        long executionId = jobScheduler.submitOneShot(
            job.getCommand(), job.getContainerImage(),
            job.getResourceRequest(), job.getPriority(),
            job.getMaxRetries(), job.getTimeoutSeconds(),
            job.getTenantId(),
            Map.of("cron_job_id", String.valueOf(job.getCronJobId()),
                   "scheduled_fire_time", String.valueOf(missedFireTime.toInstant().toEpochMilli()),
                   "trigger_type", "MISFIRE_RECOVERY")
        );
        
        TriggerHistory trigger = new TriggerHistory();
        trigger.setCronJobId(job.getCronJobId());
        trigger.setScheduledFireTime(missedFireTime.toInstant());
        trigger.setActualFireTime(Instant.now());
        trigger.setFireDelayMs((int) Duration.between(missedFireTime.toInstant(), Instant.now()).toMillis());
        trigger.setTriggerType(TriggerType.MISFIRE_RECOVERY);
        trigger.setExecutionId(executionId);
        trigger.setFiredByInstance(leaderElector.getInstanceId());
        triggerRepo.save(trigger);
        
        metrics.counter("cron.misfire.recovered").increment();
    }
}
```

**Failure Modes:**
- **FIRE_ALL_MISSED storm:** If the service was down for 24 hours and a job fires every minute, FIRE_ALL_MISSED would try to submit 1440 jobs. The `max_misfire_count` cap (default 10) prevents this. Jobs exceeding the cap are logged and alerted.
- **Missed trigger detection false positive:** A trigger that's 2 minutes "late" is detected as missed even if it's just queued and about to fire. We check the Redis sorted set: if the job is still in the set with the expected score, it's not missed (just delayed). We also check trigger_history: if a record exists for the expected fire time, it's not missed.
- **Recovery triggers overlap with regular triggers:** The distributed lock (`lock:cron:{job_id}:{fire_time}`) prevents both the regular trigger engine and the misfire recovery from firing the same trigger. The lock key includes the fire_time, so each fire slot has its own lock.

**Interviewer Q&A:**

**Q1: How long can the cron service be down before missing triggers is unacceptable?**
A: For FIRE_NOW policy: any downtime > tick interval (1 second) causes a missed trigger, but it's recovered within 2 minutes (detection threshold). For SKIP_TO_NEXT: any downtime is tolerable (jobs just skip to the next scheduled time). For FIRE_ALL_MISSED: downtime is tolerable up to `max_misfire_count * cron_interval`. Beyond that, older missed fires are dropped.

**Q2: What if the MySQL database has an outage -- can you still detect missed triggers?**
A: The primary trigger mechanism (Redis sorted set + lock) doesn't require MySQL. If MySQL is down, triggers still fire via Redis. Trigger history logging and misfire detection are degraded (no MySQL writes). On MySQL recovery, the misfire detector catches any gap by comparing `last_fire_time` with expected schedule.

**Q3: How do you handle the case where FIRE_ALL_MISSED submits many jobs that all compete for the same resources?**
A: Misfire recovery jobs are submitted at BATCH priority (lowest band) by default, to avoid starving regular triggers. Additionally, they're rate-limited: max 10 misfire recovery submissions per second per tenant. This spreads the recovery load over time.

**Q4: Can a user manually trigger a specific missed fire time?**
A: Yes. `POST /api/v1/cron-jobs/{id}/trigger?fire_time=2026-04-09T02:00:00Z` triggers the job as if it were the missed fire at that time. The trigger history records `trigger_type=MANUAL` with the specified fire time. This is useful for audit: "the 2 AM backup didn't run; manually trigger it for that time slot."

**Q5: How do you alert on missed triggers without spamming during planned maintenance?**
A: Maintenance windows are declared in advance: `POST /api/v1/maintenance-windows {"start": "...", "end": "...", "suppress_misfire_alerts": true}`. During the window, misfire detection still runs (for FIRE_ALL_MISSED recovery), but alerts are suppressed. After the window, a summary alert is sent: "X triggers missed during maintenance; Y recovered, Z skipped."

**Q6: How do you test misfire policies?**
A: Integration tests that: (1) Start the cron service with a 1-second-interval cron job. (2) Pause the trigger engine for 30 seconds. (3) Resume and verify: FIRE_NOW fires once, SKIP_TO_NEXT skips 30 fires, FIRE_ALL_MISSED fires 10 (capped). (4) Verify trigger_history records match expected behavior. We also test with timezone boundaries: miss a trigger that falls in a DST gap and verify correct recovery.

---

### 6.4 Concurrency Policy Engine

**Why it's hard:** When a cron job fires but the previous execution is still running, the system must decide: overlap (allow concurrent runs), skip (discard this trigger), or queue (wait for previous to complete). The "queue" option is particularly tricky: we need to track the dependency without creating a full DAG, and avoid unbounded queue growth if the job consistently runs longer than its interval.

**Implementation:**

```java
@Service
public class ConcurrencyPolicyEngine {
    
    @Autowired private JobSchedulerClient jobScheduler;
    @Autowired private TriggerHistoryRepository triggerRepo;
    
    /**
     * Evaluate concurrency policy before triggering.
     * Returns true if the trigger should proceed, false to skip.
     */
    public ConcurrencyDecision evaluate(CronJob cronJob) {
        switch (cronJob.getConcurrencyPolicy()) {
            case ALLOW:
                // Always proceed (overlap OK)
                return ConcurrencyDecision.proceed();
                
            case SKIP:
                // Check if previous execution is still running
                boolean running = jobScheduler.isJobRunningForCronJob(cronJob.getCronJobId());
                if (running) {
                    return ConcurrencyDecision.skip("Previous execution still running");
                }
                return ConcurrencyDecision.proceed();
                
            case QUEUE:
                // Check if there are already queued triggers waiting
                int queuedCount = triggerRepo.countQueuedTriggers(cronJob.getCronJobId());
                if (queuedCount >= 3) { // Max 3 queued triggers
                    return ConcurrencyDecision.skip("Max queued triggers reached (3)");
                }
                
                boolean prevRunning = jobScheduler.isJobRunningForCronJob(cronJob.getCronJobId());
                if (prevRunning) {
                    // Queue this trigger to fire when previous completes
                    return ConcurrencyDecision.queue();
                }
                return ConcurrencyDecision.proceed();
                
            default:
                return ConcurrencyDecision.proceed();
        }
    }
}
```

**Interviewer Q&A:**

**Q1: What's the default concurrency policy and why?**
A: SKIP. Most infrastructure cron jobs (backups, health checks) should not overlap. Running two backups simultaneously is wasteful and may cause contention. SKIP is safe: if the previous run is slow, we skip this trigger and catch up on the next schedule. ALLOW is for jobs where concurrency is safe (e.g., independent health checks on different targets). QUEUE is rare (sequential batch processing).

**Q2: How do you detect that a previous execution is "still running" efficiently?**
A: We cache the running status in Redis: `SET cron_running:{cron_job_id} {execution_id} EX {timeout_seconds}`. When a cron-triggered job starts, we set this key. When it completes, we delete it. The concurrency check is a single Redis GET. The TTL ensures the key is cleaned up if the completion callback is lost.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

The cron service doesn't place jobs on workers directly. It submits one-shot jobs to the distributed job scheduler, which handles placement. The cron service's scheduling concerns are temporal (when to fire), not spatial (where to run).

### Conflict Detection

- **Duplicate trigger prevention:** Redis SETNX lock per (cron_job_id, fire_time_ms)
- **Concurrent execution prevention:** Concurrency policy engine (see 6.4)

### Queue & Priority Implementation

Cron-triggered jobs inherit the priority of their cron_job definition. The distributed job scheduler's priority queue handles dispatch ordering.

### Temporal Scheduling

```
Redis Sorted Set: cron:next_exec
  Score = next_fire_time_ms (epoch milliseconds)
  Member = cron_job_id (as string)

ZRANGEBYSCORE returns all jobs due at or before now.
After firing, new score = next_fire_time_ms (recalculated).
```

This is the core scheduling data structure. It replaces the need for polling all 2M cron jobs every second.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling | Trigger |
|-----------|---------|---------|
| Cron Trigger Engine | Shard by cron_job_id hash into N partitions | Tick duration P99 > 800ms |
| API Gateway | HPA on CPU | CPU > 70% |
| Missed Trigger Detector | Single instance (leader) | N/A (lightweight) |

### Database Scaling

| Strategy | Implementation | When |
|----------|---------------|------|
| Read replicas | 2 replicas for trigger history queries | Read QPS > 10K |
| Partitioning | trigger_history by month | Table > 100M rows |
| Archiving | Move trigger_history > 30 days to cold storage | Monthly |
| Cron job table | No partitioning needed (10M rows, 10 GB) | Fits in buffer pool |

### Caching

| Layer | Data | TTL | Invalidation |
|-------|------|-----|-------------|
| L1: JVM Caffeine | Calendar rules | 5 min | Event-driven on calendar update |
| L1: JVM Caffeine | Cron job definitions | 30s | Event-driven on job update |
| L2: Redis | Next-execution sorted set | N/A (primary store) | Updated on each trigger |
| L2: Redis | Concurrency running status | Job timeout | Cleared on completion |

### Kubernetes-Specific

```yaml
# Cron service deployment
apiVersion: apps/v1
kind: Deployment
metadata:
  name: cron-service
spec:
  replicas: 3    # 1 leader + 2 standby
  strategy:
    type: RollingUpdate
    rollingUpdate:
      maxUnavailable: 1
      maxSurge: 1
  template:
    spec:
      containers:
      - name: cron-service
        resources:
          requests:
            cpu: 4000m
            memory: 8Gi
          limits:
            cpu: 8000m
            memory: 16Gi
        livenessProbe:
          httpGet:
            path: /health/live
            port: 8080
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /health/ready
            port: 8080
          periodSeconds: 5
      topologySpreadConstraints:
      - maxSkew: 1
        topologyKey: topology.kubernetes.io/zone
        whenUnsatisfiable: DoNotSchedule
```

### Interviewer Q&A

**Q1: How do you handle 500K triggers at midnight UTC?**
A: This is the "thundering minute" problem. With batch processing (1000 triggers per tick, 16 parallel threads, 1-second ticks), we can process 16,000 triggers/second. 500K triggers would take ~31 seconds to process. The first batch fires within 1 second; the last batch fires ~31 seconds late. To improve: (1) Increase trigger pool threads to 64 (processes 64K/s). (2) Shard the sorted set across multiple Redis nodes and trigger engines. (3) Pre-compute: identify the "thundering minute" jobs and pre-generate their trigger locks seconds before the minute.

**Q2: Why 3 replicas instead of using k8s CronJob for the cron service itself?**
A: The cron service IS the replacement for k8s CronJob (which doesn't scale to 10M jobs). We run 3 replicas for HA: 1 leader does the work, 2 standbys take over on failure. This gives us sub-20-second failover (etcd lease TTL). k8s CronJob controller has a single controller instance and limited scalability.

**Q3: What's the memory usage for 2M entries in the Redis sorted set?**
A: Each sorted set entry: ~70 bytes (member string ~10 bytes + skiplist overhead ~60 bytes). 2M entries = 140 MB. With Redis overhead: ~200 MB. Well within a single Redis node's capacity (typical 8-16 GB). At 10M entries, we'd need ~1 GB, still manageable.

**Q4: How do you handle the sorted set rebuild after a Redis failure?**
A: Full rebuild: `SELECT cron_job_id, UNIX_TIMESTAMP(next_fire_time) * 1000 FROM cron_job WHERE enabled = TRUE AND paused = FALSE`. Insert into sorted set with ZADD pipeline. For 2M rows: MySQL query takes ~5s, Redis pipeline takes ~10s. Total rebuild: ~15s. During rebuild, triggers are delayed but not lost (they'll be picked up once the set is populated).

**Q5: Can the cron service run on Kubernetes ephemeral pods?**
A: Yes, but with caveats. The cron service is stateless (all state in MySQL/Redis). Pods can be killed and restarted without data loss. However, pod disruption causes a leader election (15s failover). We use PodDisruptionBudget to ensure at most 1 pod is disrupted at a time, and topology spread to ensure pods are on different nodes/zones.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| Scheduler leader crash | All triggers paused | etcd lease (15s) | Standby takes over | 15-20s | 0 (sorted set in Redis) |
| Redis sorted set lost | No triggers fire | Connection error | Rebuild from MySQL (15s) | 15-30s | Missed triggers recovered |
| Redis lock service failure | Risk of duplicate triggers | Connection error | Fall back to MySQL-based lock | 10s | Possible 1-2 duplicates |
| MySQL primary failure | CRUD blocked; trigger history lost temporarily | Failover detection | Semi-sync replica promotion | 30-60s | 0 |
| Network partition (cron ↔ job scheduler) | Triggers fire but jobs not submitted | Submit failure | Retry with backoff; queue locally | 10-30s | 0 (retry from sorted set) |
| Thundering minute overload | Late triggers (> 30s) | Tick duration metric | Pre-scaling; parallel processing | Self-recovering | Late triggers < 60s |
| tzdata update needed | Wrong fire times after DST change | Calendar validation | Deploy tzdata update; restart | Hours (manual) | Potential wrong fire times |

### Automated Recovery

```java
@Component
public class CronServiceRecovery {
    
    @Scheduled(fixedRate = 60_000) // Every minute
    public void reconcileSortedSet() {
        if (!leaderElector.isLeader()) return;
        
        // Compare MySQL next_fire_time with Redis sorted set
        long redisCount = redis.opsForZSet().size("cron:next_exec");
        long mysqlCount = cronJobRepo.countEnabledAndNotPaused();
        
        if (Math.abs(redisCount - mysqlCount) > mysqlCount * 0.05) { // > 5% discrepancy
            log.warn("Sorted set discrepancy: Redis={}, MySQL={}. Triggering rebuild.",
                redisCount, mysqlCount);
            rebuildSortedSet();
            metrics.counter("cron.recovery.sorted_set_rebuild").increment();
        }
    }
    
    private void rebuildSortedSet() {
        // Full rebuild from MySQL
        redis.delete("cron:next_exec");
        
        int offset = 0;
        int batchSize = 10000;
        
        while (true) {
            List<CronJob> batch = cronJobRepo.findEnabledWithNextFireTime(offset, batchSize);
            if (batch.isEmpty()) break;
            
            Map<String, Double> entries = new HashMap<>();
            for (CronJob job : batch) {
                if (job.getNextFireTime() != null) {
                    entries.put(String.valueOf(job.getCronJobId()),
                        (double) job.getNextFireTime().toEpochMilli());
                }
            }
            
            redis.opsForZSet().add("cron:next_exec", 
                entries.entrySet().stream()
                    .map(e -> ZSetOperations.TypedTuple.of(e.getKey(), e.getValue()))
                    .collect(Collectors.toSet()));
            
            offset += batchSize;
        }
        
        log.info("Sorted set rebuild complete. {} entries.", 
            redis.opsForZSet().size("cron:next_exec"));
    }
}
```

### Retry Strategy

| Component | Retry | Backoff | Max Retries |
|-----------|-------|---------|-------------|
| Redis sorted set operations | Immediate | 100ms exponential | 3 |
| Job scheduler submission | Immediate | 1s exponential | 5 |
| MySQL trigger history write | Async | 5s | 3 (then log to file) |
| Distributed lock acquisition | No retry (skip trigger) | N/A | 0 |

### Circuit Breaker

- **Job scheduler circuit breaker:** If > 50% of job submissions fail in a 5-minute window, stop triggering cron jobs (circuit open). Triggers accumulate as "missed" and are recovered when the circuit closes (misfire policy applies).
- **Redis circuit breaker:** If Redis is unreachable, fall back to MySQL-based trigger scanning (slower but functional).

---

## 10. Observability

### Key Metrics

| Metric | Type | Warning | Critical |
|--------|------|---------|----------|
| `cron.tick.duration_ms` | Histogram | P99 > 800ms | P99 > 2s |
| `cron.trigger.delay_ms` | Histogram | P99 > 1s | P99 > 5s |
| `cron.trigger.rate` | Counter | < 50% baseline | < 20% baseline |
| `cron.missed.count` | Counter | > 0 | > 10 in 1h |
| `cron.sorted_set.size` | Gauge | Diverges > 5% from MySQL | Diverges > 20% |
| `cron.lock.contention_rate` | Rate | > 5% | > 20% |
| `cron.active_jobs` | Gauge | Informational | N/A |
| `cron.upcoming.1min` | Gauge | > 10K (thundering minute) | > 50K |
| `cron.concurrency.skipped` | Counter | > 100/hr | > 1000/hr |

### Distributed Tracing

Each cron trigger creates a trace that spans: tick → lock acquisition → job submission → worker execution → completion.

### Alerting

| Alert | Condition | Severity |
|-------|-----------|----------|
| Missed trigger | Any cron job missed its fire time by > 2 min | P2 |
| Critical missed trigger (tagged jobs) | Cert-rotation or backup missed | P1 |
| Trigger delay P99 > 5s | Sustained for 10 min | P2 |
| Sorted set divergence > 20% | Reconciliation failed | P1 |
| Tick duration > 2s | Thundering minute or performance issue | P2 |

---

## 11. Security

### Authentication & Authorization

```
Roles:
  cron-admin:     CRUD cron jobs, manage calendars, manual trigger
  cron-operator:  pause/resume, manual trigger, view trigger history
  cron-viewer:    read-only access to definitions and history
  platform-admin: cross-tenant access, quota management
```

- Cron expressions are validated and sanitized (no arbitrary code injection)
- `command` field is validated against an allowlist of executable paths (no arbitrary shell commands)
- Container images must be from approved registries

### Secrets Management

- Cron job environment variables: encrypted at rest, decrypted only at worker execution time
- Redis credentials: Vault dynamic secrets
- MySQL credentials: Vault dynamic secrets with 24h rotation

### Audit Logging

Every cron job mutation and trigger event is logged to the immutable audit trail.

---

## 12. Incremental Rollout Strategy

### Phases

1. **Shadow mode (Week 1-2):** New cron service calculates next fire times for all existing cron jobs. Compare with existing system's fire times. Validate timezone/DST correctness.
2. **Internal cron jobs (Week 3-4):** Migrate platform team's own cron jobs (health checks, cleanup scripts). Monitor trigger accuracy.
3. **Low-risk tenants (Week 5-6):** Migrate 10% of tenants (selected for non-critical workloads). Monitor for missed triggers, delay, duplicates.
4. **All tenants (Week 7-8):** Migrate remaining tenants. Keep old system running as backup for 2 weeks.
5. **Decommission old system (Week 10).**

### Rollback Strategy

- Per-tenant rollback: route individual tenants back to old cron system via routing table
- Emergency: disable new cron service leader election; old system's leader takes over all scheduling
- Cron job definitions are in MySQL (shared between old and new systems)

### Rollout Interviewer Q&A

**Q1: How do you migrate 10M cron jobs without downtime?**
A: Both old and new systems read from the same MySQL cron_job table. We add a `scheduler_assignment` column: `OLD`, `NEW`, or `MIGRATING`. The new system only triggers jobs assigned to it. Migration is per-tenant: change all cron jobs for a tenant from `OLD` to `NEW` in a single transaction. Both systems use distributed locks, so there's no risk of double-firing during migration.

**Q2: What if the new system fires a job 1 second earlier than expected?**
A: Clock precision matters. We use NTP with < 10ms accuracy across all cron service instances. A 1-second early fire is within our 1-second tick granularity and acceptable. If sub-second precision is needed, we use Quartz 7-field expressions with seconds.

**Q3: How do you validate that DST handling is correct in production?**
A: We create "canary cron jobs" in every timezone that has DST transitions. These jobs fire at times near the transition (e.g., 1:30 AM, 2:30 AM, 3:30 AM local time). We verify: correct fire count (2 fires around spring-forward, not 3), correct local time of fire, and correct handling of the gap/overlap.

**Q4: What if a tenant has cron jobs that the new system can't parse?**
A: We run a validation sweep before migration: parse every cron job's expression with the new parser and flag any that fail. These are reported to the tenant for correction before migration. Common issues: non-standard Quartz extensions, legacy 3-field expressions, or timezone aliases (EST vs. America/New_York).

**Q5: How do you ensure zero missed triggers during the migration?**
A: The migration protocol: (1) Assign job to `MIGRATING` status. (2) Both systems acquire locks but neither fires. (3) New system calculates next fire time and adds to its sorted set. (4) Assign job to `NEW`. (5) New system fires on next schedule. The `MIGRATING` window is < 1 second (single MySQL transaction). At worst, one trigger is delayed by 1 second.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Selected | Trade-off | Rationale |
|----------|---------|----------|-----------|-----------|
| Trigger detection | Poll all jobs vs. sorted set | Sorted set | Redis dependency vs. O(K) instead of O(N) | At 2M jobs, O(N) per second is infeasible |
| Trigger lock | Redis SETNX vs. MySQL lock vs. etcd | Redis SETNX | Non-durable lock vs. speed | Lock is short-lived (60s); durability not required (idempotent re-trigger) |
| Timezone handling | UTC-only vs. per-job timezone | Per-job timezone | Complexity vs. correctness | "2 AM local time" must mean 2 AM in the user's timezone, not UTC |
| Misfire policy | Single global policy vs. per-job | Per-job configurable | Complexity vs. flexibility | Different jobs have different requirements (backup: FIRE_NOW, health check: SKIP_TO_NEXT) |
| Cron expression format | Unix only vs. multiple formats | Multiple (Unix, Quartz, EventBridge) | Parser complexity vs. user convenience | Teams come from different backgrounds; support their preferred format |
| Calendar integration | Built-in vs. external calendar service | Built-in | Maintenance burden vs. simplicity | Calendar rules are simple; external service adds latency and dependency |
| Concurrency policy | SKIP-only vs. configurable | Configurable (ALLOW/SKIP/QUEUE) | Complexity vs. flexibility | Different use cases need different policies |

---

## 14. Agentic AI Integration

### Where AI Adds Value

| Use Case | AI Capability | Impact |
|----------|--------------|--------|
| **Cron expression generator** | Natural language to cron expression: "every weekday at 2 AM EST" → "0 2 * * 1-5" | Eliminates cron syntax errors |
| **Schedule optimization** | Analyze trigger patterns and suggest off-peak schedules to reduce thundering minute | Flatten peak load by 40% |
| **Missed trigger root cause analysis** | Correlate missed triggers with system events (deploys, outages, Redis latency) | Faster RCA |
| **Calendar suggestion** | Analyze failure patterns (jobs failing on holidays) and suggest calendar rules | Reduce holiday-related failures |
| **Capacity forecasting** | Predict trigger volume growth and recommend infrastructure scaling | Proactive scaling |

### Agent Loop Example

```python
class CronScheduleOptimizer:
    """Suggests optimal scheduling to reduce thundering minute effect."""
    
    def observe(self):
        # Get trigger volume per minute for last 7 days
        return {
            'trigger_heatmap': metrics.query('cron_trigger_count', range='7d', step='1m'),
            'top_minute_load': metrics.query('topk(10, cron_trigger_count)'),
            'jobs_at_midnight': cron_repo.count_by_expression_pattern('0 0 * * *'),
        }
    
    def reason(self, observation):
        prompt = f"""
        Analyze this cron trigger heatmap and suggest schedule optimizations.
        
        Peak minutes: {observation['top_minute_load']}
        Jobs at exactly midnight: {observation['jobs_at_midnight']}
        
        Rules:
        - Suggest spreading midnight jobs across :00 to :04 (jitter)
        - Suggest spreading top-of-hour jobs across :00 to :09
        - Don't change jobs with strict timing requirements (e.g., cert rotation)
        - Output: list of (cron_job_id, current_expression, suggested_expression, savings_estimate)
        """
        return self.llm.generate(prompt)
    
    def act(self, suggestions):
        # Send suggestions to job owners (don't auto-apply)
        for suggestion in suggestions:
            notify_api.send(
                tenant=suggestion['tenant_id'],
                message=f"Schedule optimization suggestion for {suggestion['job_name']}: "
                        f"change from '{suggestion['current']}' to '{suggestion['suggested']}'. "
                        f"Expected impact: reduce peak load by {suggestion['savings_estimate']}%."
            )
```

### Guard Rails

- AI never auto-changes cron schedules (suggestions only)
- AI cron expression generation includes validation: output is parsed and next 10 fire times displayed for human confirmation
- All AI-generated calendar rules require human approval

---

## 15. Complete Interviewer Q&A Bank

**Q1: Why not use Kubernetes CronJob controller for this?**
A: Kubernetes CronJob has several limitations at scale: (1) All cron jobs are stored in etcd (limited to ~1M objects total, shared with all other k8s resources). (2) The CronJob controller is a single controller instance with limited throughput (~1000 evaluations/sec). (3) No timezone support in earlier versions (added in k8s 1.27). (4) No built-in missed execution handling beyond a 100-second window. (5) No calendar-aware scheduling. Our custom cron service handles 10M jobs, has full timezone/DST support, calendar integration, and configurable misfire policies.

**Q2: How do you handle a cron expression that fires every second?**
A: Quartz 7-field format supports seconds: `* * * * * ? *` fires every second. This creates 86,400 triggers/day for a single cron job. We enforce a minimum interval per tenant quota (`min_interval_seconds`, default 60). If a tenant needs sub-minute scheduling, they must request a quota increase with justification. The sorted set handles high-frequency jobs efficiently, but we monitor per-job trigger rate to prevent abuse.

**Q3: How do you ensure the sorted set's accuracy over time?**
A: Three mechanisms: (1) After every trigger, the next fire time is recalculated from the cron expression (not incremented by interval), preventing drift. (2) The reconciliation process (every 60s) compares Redis and MySQL. (3) The missed trigger detector (every 5 min) catches any jobs that fell through the cracks. Combined, these ensure eventual consistency within 5 minutes.

**Q4: What's the operational cost of running this cron service?**
A: 3 JVM instances (4 CPU, 8 GB each) + 1 Redis instance (8 GB) + MySQL (shared with job scheduler). Total: ~20 CPU, 40 GB RAM. This handles 2M active cron jobs and 100K triggers/min. Cost-effective compared to running a separate Quartz cluster or deploying 2M individual k8s CronJob objects.

**Q5: How do you handle the case where a cron job's command changes but the next fire time doesn't?**
A: When a cron job is updated, the sorted set score (next fire time) is unchanged if the schedule didn't change. The trigger engine loads the latest cron_job definition from MySQL (or cache) at trigger time, so the updated command is used on the next fire. If the schedule changes, we recalculate next fire time and update the sorted set.

**Q6: What happens if a cron job is paused and then resumed days later?**
A: On resume, we recalculate the next fire time from now (not from the pause time). The misfire policy does NOT apply to paused periods -- pausing is an explicit user action, not a system failure. If the user wants to catch up on missed fires during the pause, they can use `cronctl trigger` to manually fire the job.

**Q7: How do you handle cron expression changes for frequently-firing jobs (e.g., every minute)?**
A: When the cron expression is updated: (1) Recalculate next fire time based on new expression. (2) Update Redis sorted set. (3) If the old expression would have fired before the new one, no issue (the old fire time passes and is re-evaluated). If the new expression fires sooner than the old, the new fire time takes effect immediately. There's no gap or overlap.

**Q8: Can two cron jobs have the same name in different tenants?**
A: Yes. The unique key is (tenant_id, job_name). This allows tenants to use their own naming conventions without collision. Cross-tenant operations require platform-admin role.

**Q9: How do you handle daylight saving time for cron jobs in Arizona (no DST)?**
A: Arizona uses `America/Phoenix` (MST year-round, no DST). The IANA database correctly reports no transitions for this zone. Our DST handling code simply finds no transitions and proceeds normally. Jobs in `America/Phoenix` always fire at the same UTC offset (-7:00).

**Q10: How do you migrate from a Quartz Scheduler cluster to this system?**
A: Quartz stores jobs in tables like `qrtz_cron_triggers`, `qrtz_triggers`, `qrtz_job_details`. We write a migration script that: (1) reads Quartz tables, (2) maps to our cron_job schema, (3) converts Quartz-specific settings to our format (misfire instructions → misfire_policy). The migration runs during a maintenance window. We verify by comparing next fire times between Quartz and our system for all migrated jobs.

**Q11: How do you handle a cron job that needs to run "the first business day of every month"?**
A: This requires calendar integration. The cron expression `0 9 1-3 * *` fires on the 1st, 2nd, and 3rd of every month. The calendar excludes weekends. Combined, the job fires on the first weekday (1st, or 2nd if 1st is Saturday, or 3rd if 1st is Sunday). For more complex business-day logic, we support custom calendar rules: `FIRST_BUSINESS_DAY_OF_MONTH` as a special rule type.

**Q12: What's the maximum number of cron jobs a single Redis sorted set can handle?**
A: Redis sorted sets can hold 2^32 - 1 members (~4.3 billion). At 100 bytes per entry, 10M jobs = 1 GB. Memory is the practical limit: a 16 GB Redis instance can hold ~160M entries. At 10M active jobs, we're at ~6% capacity. Beyond 50M, we'd shard across multiple sorted sets.

**Q13: How do you handle rate(1 minute) vs. */1 * * * * -- are they the same?**
A: `rate(1 minute)` (EventBridge format) and `*/1 * * * *` (Unix cron) produce the same result: fire every minute. However, they differ in start time. `rate(1 minute)` starts from the job creation time (relative), while `*/1 * * * *` fires at every minute boundary (:00, :01, :02...). We document this difference and recommend cron expressions for absolute scheduling and rate expressions for relative intervals.

**Q14: How do you handle the "noisy neighbor" problem (one tenant creates 1M cron jobs)?**
A: Per-tenant quota: `max_cron_jobs` (default 10K) and `max_triggers_per_min` (default 1K). These are enforced at the API layer. If a tenant legitimately needs more, they request a quota increase. The sorted set processes all tenants' jobs together, but the trigger engine rate-limits submissions to the job scheduler per-tenant.

**Q15: How do you benchmark trigger accuracy?**
A: We run synthetic cron jobs at 1-second intervals with known expected fire times. We measure `fire_delay_ms = actual_fire_time - scheduled_fire_time` for every trigger. Our SLA: P50 < 100ms, P99 < 1s, P99.9 < 5s. We run this benchmark continuously in production (dedicated canary cron jobs) and alert on SLA violations.

---

## 16. References

- [Quartz Scheduler Documentation](http://www.quartz-scheduler.org/documentation/)
- [Quartz CronExpression Format](http://www.quartz-scheduler.org/documentation/quartz-2.3.0/tutorials/crontrigger.html)
- [Kubernetes CronJob](https://kubernetes.io/docs/concepts/workloads/controllers/cron-jobs/)
- [IANA Time Zone Database](https://www.iana.org/time-zones)
- [java.time.ZoneRules DST Transitions](https://docs.oracle.com/en/java/javase/17/docs/api/java.base/java/time/zone/ZoneRules.html)
- [Redis Sorted Sets - ZRANGEBYSCORE](https://redis.io/commands/zrangebyscore/)
- [Redis Distributed Locks (SETNX pattern)](https://redis.io/docs/manual/patterns/distributed-locks/)
- [AWS EventBridge Schedule Expressions](https://docs.aws.amazon.com/eventbridge/latest/userguide/eb-create-rule-schedule.html)
- [Google Cloud Scheduler](https://cloud.google.com/scheduler/docs)
- [cron(5) - Unix manual page](https://man7.org/linux/man-pages/man5/crontab.5.html)
- [DST and Timezone Handling Best Practices](https://www.creativedeletion.com/2015/01/28/falsehoods-programmers-believe-about-time.html)
