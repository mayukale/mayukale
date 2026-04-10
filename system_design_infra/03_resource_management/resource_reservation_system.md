# System Design: Resource Reservation System

> **Relevance to role:** A cloud infrastructure platform engineer must manage time-based resource reservations for scheduled workloads, maintenance windows, capacity guarantees for ML training jobs, and reserved instances. This system handles temporal resource allocation — reserving future capacity with guaranteed availability, detecting scheduling conflicts, managing overbooking, and integrating with billing. It's the backbone of reserved instances in AWS/GCP, capacity reservations in Azure, and scheduled scaling in any large-scale infrastructure platform.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Create reservations with start time, end time, and resource requirements (CPU, RAM, GPU, disk, network). |
| FR-2 | Detect and prevent scheduling conflicts — no double-booking of the same physical resources. |
| FR-3 | Support reservation states: REQUESTED → CONFIRMED → ACTIVE → COMPLETED/EXPIRED/CANCELLED. |
| FR-4 | Support rolling reservations (e.g., "every Tuesday 2AM-6AM for maintenance") and recurring reservations. |
| FR-5 | Implement overbooking strategy with configurable overbooking ratio and compensation policy. |
| FR-6 | Priority-based preemption of lower-priority reservations when capacity is insufficient. |
| FR-7 | Integration with billing: calculate reservation cost based on resource type, duration, and commitment level. |
| FR-8 | Support partial fulfillment: if full reservation can't be met, offer best available alternative. |
| FR-9 | Provide dry-run API to check reservation feasibility without committing. |
| FR-10 | Automatic activation and deactivation at reservation boundaries (workloads started/stopped on schedule). |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Reservation creation latency (p99) | < 200 ms |
| NFR-2 | Conflict detection latency (p99) | < 50 ms |
| NFR-3 | Concurrent reservation handling | 1,000 reservations/sec |
| NFR-4 | System availability | 99.99% |
| NFR-5 | Reservation activation accuracy | Workload started within 30 seconds of start_time |
| NFR-6 | Conflict detection accuracy | 100% — zero double-bookings for hard reservations |
| NFR-7 | Overbooking displacement rate | < 5% of overbooked reservations actually displaced |

### Constraints & Assumptions

- Fleet: 50,000 hosts across 5 regions and 10 availability zones.
- Average concurrent active reservations: 100,000.
- Reservation duration: minutes (maintenance) to years (reserved instances).
- Time granularity: 1 minute (reservations snap to minute boundaries).
- Some reservations are "hard" (guaranteed, no overbooking) and others are "soft" (best-effort, may be displaced).
- Billing integration is event-based (emit events, billing system consumes).
- The system must handle time zones correctly for recurring reservations.

### Out of Scope

- Workload execution management (handled by job scheduler).
- Billing calculation and invoicing (separate billing service).
- User-facing marketplace for selling/buying reserved capacity (future phase).
- Spot pricing / auction system.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Total hosts | 50,000 | Given |
| Active reservations (concurrent) | 100,000 | Mix of long-term (RI) and short-term (maintenance, burst) |
| New reservations per day | 10,000 | Mix of manual and automated |
| Reservation modifications per day | 5,000 | Extensions, early terminations |
| Reservation lookups per second | 5,000 | Scheduler queries, dashboard, billing |
| Conflict checks per second | 2,000 | New reservation attempts + modifications |
| Recurring reservation instances | 20,000 | ~2,000 recurring templates x 10 upcoming instances each |

### Latency Requirements

| Operation | p50 | p99 | p999 |
|-----------|-----|-----|------|
| Create reservation (with conflict check) | 50 ms | 150 ms | 500 ms |
| Conflict detection query | 10 ms | 40 ms | 100 ms |
| Reservation activation (workload start) | 5 s | 20 s | 60 s |
| List reservations (filtered) | 20 ms | 100 ms | 300 ms |
| Dry-run feasibility check | 30 ms | 100 ms | 300 ms |
| Recurring reservation expansion | 100 ms | 500 ms | 2 s |

### Storage Estimates

| Data | Size per record | Count | Total |
|------|-----------------|-------|-------|
| Active reservations | 2 KB | 100,000 | 200 MB |
| Reservation history (1 year) | 1 KB | 3,650,000 | 3.65 GB |
| Recurring reservation templates | 1 KB | 2,000 | 2 MB |
| Reservation events (1 year) | 0.5 KB | 36,500,000 | 18.25 GB |
| Host capacity timeline (interval tree index) | 200 bytes | 500,000 intervals | 100 MB |
| **Total active** | | | **~300 MB** |
| **Total with history** | | | **~22 GB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Reservation API requests | 2,000/sec x 2 KB | 4 MB/s |
| Reservation API responses | 2,000/sec x 1 KB | 2 MB/s |
| Scheduler queries | 5,000/sec x 0.5 KB | 2.5 MB/s |
| Billing events | 500/sec x 0.5 KB | 0.25 MB/s |
| **Total** | | **~9 MB/s** |

---

## 3. High Level Architecture

```
                    ┌──────────────────────────────┐
                    │        API Gateway            │
                    │  (auth, rate limit, routing)  │
                    └───────────┬──────────────────┘
                                │
                   ┌────────────┼────────────────┐
                   │            │                │
          ┌────────▼──────┐  ┌─▼────────────┐  ┌▼──────────────┐
          │ Reservation   │  │ Scheduler     │  │ Admin/Dashboard│
          │ API Service   │  │ Integration   │  │ Service        │
          │ (CRUD, query) │  │ Service       │  │ (reporting)    │
          └───────┬───────┘  └──┬────────────┘  └───────┬───────┘
                  │             │                        │
                  └─────────────┼────────────────────────┘
                                │
              ┌─────────────────┼─────────────────────┐
              │                 │                      │
     ┌────────▼────────┐ ┌─────▼──────────┐  ┌───────▼───────────┐
     │  Conflict        │ │ Reservation     │  │ Recurring         │
     │  Detection       │ │ Lifecycle       │  │ Reservation       │
     │  Engine          │ │ Manager         │  │ Expander          │
     │ (interval tree)  │ │ (state machine) │  │ (cron + template) │
     └────────┬─────────┘ └─────┬───────────┘  └───────┬──────────┘
              │                 │                       │
              └─────────────────┼───────────────────────┘
                                │
              ┌─────────────────┼─────────────────────┐
              │                 │                      │
     ┌────────▼────────┐ ┌─────▼──────────┐  ┌───────▼───────────┐
     │  Reservation     │ │  Capacity       │  │  Billing Event    │
     │  Store           │ │  Timeline       │  │  Publisher        │
     │  (MySQL 8.0)     │ │  Cache          │  │  (Kafka)          │
     │                  │ │  (Redis)        │  │                   │
     └────────┬─────────┘ └────────────────┘  └───────────────────┘
              │
     ┌────────▼────────────────────────────────┐
     │        Activation Engine                 │
     │  (watches clock, triggers workloads      │
     │   at reservation start_time)             │
     │  ┌────────────┐  ┌────────────────────┐ │
     │  │ Timer Queue │  │ Workload Launcher  │ │
     │  │ (Redis      │  │ (calls scheduler   │ │
     │  │  sorted set)│  │  or job manager)   │ │
     │  └────────────┘  └────────────────────┘ │
     └─────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Reservation API Service** | Handles CRUD operations for reservations. Validates input, calls conflict detection, persists to MySQL, publishes events. |
| **Conflict Detection Engine** | Core algorithm for detecting time-interval overlaps across resource dimensions. Uses interval tree data structure for O(log n + k) overlap queries. |
| **Reservation Lifecycle Manager** | State machine managing reservation transitions: REQUESTED → CONFIRMED → ACTIVE → COMPLETED. Handles cancellations, modifications, and preemptions. |
| **Recurring Reservation Expander** | Expands recurring reservation templates (cron expressions) into concrete future reservation instances. Runs daily to maintain a rolling 30-day window of expanded instances. |
| **Reservation Store (MySQL)** | Source of truth for all reservation data. ACID transactions for concurrent reservation creation. Optimistic locking for modifications. |
| **Capacity Timeline Cache (Redis)** | Cached view of resource availability over time per host/cluster. Enables fast feasibility queries. Updated on reservation create/modify/cancel. |
| **Billing Event Publisher** | Publishes reservation lifecycle events to Kafka for the billing system: reservation_created, reservation_activated, reservation_completed, reservation_cancelled. |
| **Activation Engine** | Time-triggered workflow that activates reservations at their start time. Uses Redis sorted sets as a timer queue. Triggers workload scheduling at the right moment. |

### Data Flows

**Primary — Create Reservation:**
1. Client submits reservation request (resource requirements, time window, priority).
2. API service validates input (time in future, valid resource types, valid scope).
3. Conflict Detection Engine queries interval tree for overlapping reservations on target resources.
4. If conflict: return error with conflicting reservation details and alternative time slots.
5. If no conflict: persist reservation with status CONFIRMED in MySQL (within a transaction using SELECT FOR UPDATE on affected host resources).
6. Update Redis capacity timeline cache.
7. Add activation timer to Redis sorted set (score = start_time epoch).
8. Publish `reservation_created` event to Kafka.
9. Return reservation ID and confirmation to client.

**Secondary — Reservation Activation:**
1. Activation Engine polls Redis sorted set every second: `ZRANGEBYSCORE timers 0 {now}`.
2. For each due reservation: transition state from CONFIRMED to ACTIVE in MySQL.
3. Call Scheduler Integration Service to launch the reserved workloads.
4. Publish `reservation_activated` event.
5. On failure: retry 3 times with 10-second intervals. Alert if still failing.

**Tertiary — Recurring Expansion:**
1. Daily job (2 AM UTC) reads all recurring reservation templates.
2. For each template, generates concrete instances for the next 30 days.
3. Each instance goes through the same conflict detection and confirmation flow.
4. Pre-existing instances (already expanded) are skipped.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Core reservation records
CREATE TABLE reservations (
    reservation_id   CHAR(36) PRIMARY KEY,
    -- Scope
    tenant_id        CHAR(36) NOT NULL,
    project_id       CHAR(36) NOT NULL,
    -- Time window
    start_time       TIMESTAMP(3) NOT NULL,
    end_time         TIMESTAMP(3) NOT NULL,
    timezone         VARCHAR(64) NOT NULL DEFAULT 'UTC',
    -- Resources requested
    req_cpu_cores    DECIMAL(10,2) NOT NULL DEFAULT 0,
    req_ram_mb       BIGINT NOT NULL DEFAULT 0,
    req_gpu_count    INT NOT NULL DEFAULT 0,
    req_gpu_type     VARCHAR(32),                   -- 'a100', 'h100', NULL for any
    req_disk_gb      BIGINT NOT NULL DEFAULT 0,
    req_net_gbps     DECIMAL(10,2) NOT NULL DEFAULT 0,
    -- Placement constraints
    region           VARCHAR(32),
    availability_zone VARCHAR(32),
    host_ids         JSON,                          -- specific hosts (for maintenance), NULL for any
    node_selector    JSON,                          -- label-based selection
    -- Reservation type and priority
    reservation_type ENUM('guaranteed','best_effort','maintenance','capacity_block') NOT NULL,
    priority         INT NOT NULL DEFAULT 100,      -- higher = more important
    preemptible      BOOLEAN NOT NULL DEFAULT FALSE,
    -- Overbooking
    allow_overbooking BOOLEAN NOT NULL DEFAULT FALSE,
    overbooking_group VARCHAR(64),                  -- group reservations that share overbooking pool
    -- State
    status           ENUM('requested','confirmed','active','completed','expired','cancelled',
                          'displaced','failed') NOT NULL DEFAULT 'requested',
    activated_at     TIMESTAMP(3),
    completed_at     TIMESTAMP(3),
    -- Recurring link
    recurring_template_id CHAR(36),                -- NULL if not from recurring template
    recurring_instance_index INT,                   -- which instance in the series
    -- Assigned resources (filled after confirmation)
    assigned_host_ids JSON,                        -- actual hosts assigned
    -- Modification tracking
    version          BIGINT NOT NULL DEFAULT 1,
    modified_count   INT NOT NULL DEFAULT 0,
    -- Metadata
    name             VARCHAR(255),
    description      TEXT,
    labels           JSON NOT NULL DEFAULT '{}',
    created_by       CHAR(36) NOT NULL,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    -- Constraints
    CHECK (end_time > start_time),
    CHECK (priority >= 0 AND priority <= 1000),
    INDEX idx_res_time (start_time, end_time),
    INDEX idx_res_status_time (status, start_time),
    INDEX idx_res_tenant (tenant_id, status),
    INDEX idx_res_project (project_id, status),
    INDEX idx_res_region_az (region, availability_zone, status),
    INDEX idx_res_type_status (reservation_type, status),
    INDEX idx_res_recurring (recurring_template_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Per-host resource timeline (for conflict detection)
-- Each row represents a "resource commitment" on a specific host for a time interval
CREATE TABLE host_resource_timeline (
    timeline_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
    host_id          CHAR(36) NOT NULL,
    reservation_id   CHAR(36) NOT NULL,
    start_time       TIMESTAMP(3) NOT NULL,
    end_time         TIMESTAMP(3) NOT NULL,
    -- Resources committed on this host for this reservation
    cpu_cores        DECIMAL(10,2) NOT NULL DEFAULT 0,
    ram_mb           BIGINT NOT NULL DEFAULT 0,
    gpu_count        INT NOT NULL DEFAULT 0,
    disk_gb          BIGINT NOT NULL DEFAULT 0,
    net_gbps         DECIMAL(10,2) NOT NULL DEFAULT 0,
    -- State
    status           ENUM('tentative','committed','active','released') NOT NULL DEFAULT 'tentative',
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (reservation_id) REFERENCES reservations(reservation_id),
    INDEX idx_timeline_host_time (host_id, start_time, end_time),
    INDEX idx_timeline_reservation (reservation_id),
    INDEX idx_timeline_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Recurring reservation templates
CREATE TABLE recurring_templates (
    template_id      CHAR(36) PRIMARY KEY,
    tenant_id        CHAR(36) NOT NULL,
    project_id       CHAR(36) NOT NULL,
    name             VARCHAR(255) NOT NULL,
    -- Schedule (cron expression)
    cron_expression  VARCHAR(255) NOT NULL,         -- e.g., "0 2 * * 2" (every Tuesday at 2 AM)
    timezone         VARCHAR(64) NOT NULL DEFAULT 'UTC',
    duration_minutes INT NOT NULL,                  -- duration of each instance
    -- Resources (same as reservation)
    req_cpu_cores    DECIMAL(10,2) NOT NULL DEFAULT 0,
    req_ram_mb       BIGINT NOT NULL DEFAULT 0,
    req_gpu_count    INT NOT NULL DEFAULT 0,
    req_gpu_type     VARCHAR(32),
    req_disk_gb      BIGINT NOT NULL DEFAULT 0,
    req_net_gbps     DECIMAL(10,2) NOT NULL DEFAULT 0,
    -- Placement constraints
    region           VARCHAR(32),
    availability_zone VARCHAR(32),
    node_selector    JSON,
    -- Reservation properties
    reservation_type ENUM('guaranteed','best_effort','maintenance','capacity_block') NOT NULL,
    priority         INT NOT NULL DEFAULT 100,
    -- Template lifecycle
    effective_from   DATE NOT NULL,
    effective_until  DATE,                          -- NULL = no end
    status           ENUM('active','paused','cancelled') NOT NULL DEFAULT 'active',
    -- Expansion tracking
    last_expanded_until DATE,                       -- last date for which instances were created
    created_by       CHAR(36) NOT NULL,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_recur_tenant (tenant_id),
    INDEX idx_recur_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Reservation events (audit trail + billing source)
CREATE TABLE reservation_events (
    event_id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    reservation_id   CHAR(36) NOT NULL,
    event_type       ENUM('created','confirmed','activated','completed','expired','cancelled',
                          'displaced','modified','extended','failed','billing_checkpoint') NOT NULL,
    -- State transition
    old_status       VARCHAR(32),
    new_status       VARCHAR(32),
    -- Details
    details          JSON,                          -- e.g., displacement reason, modification diff
    actor_id         CHAR(36),                      -- who triggered the event
    actor_type       ENUM('user','system','scheduler','billing') NOT NULL DEFAULT 'system',
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (reservation_id) REFERENCES reservations(reservation_id),
    INDEX idx_events_reservation (reservation_id),
    INDEX idx_events_type_time (event_type, created_at),
    INDEX idx_events_time (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Overbooking groups (for statistical overbooking management)
CREATE TABLE overbooking_groups (
    group_id         CHAR(36) PRIMARY KEY,
    group_name       VARCHAR(255) NOT NULL UNIQUE,
    overbooking_ratio DECIMAL(4,2) NOT NULL DEFAULT 1.20,  -- 1.20 = 20% overbooking
    max_displacement_pct DECIMAL(5,2) NOT NULL DEFAULT 5.00, -- max 5% displaced
    compensation_policy ENUM('refund','credit','reschedule','none') NOT NULL DEFAULT 'credit',
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Criteria | MySQL 8.0 | PostgreSQL 15 | Redis | Cassandra |
|----------|-----------|---------------|-------|-----------|
| **ACID for reservation creation** | Yes | Yes | No (single-key only) | No (eventual) |
| **Range queries on time intervals** | Good (B-tree index) | Excellent (GiST index on ranges) | Manual (sorted sets) | Limited |
| **Optimistic locking** | Version column | MVCC | WATCH/MULTI | LWT (slow) |
| **Temporal overlap queries** | Manual (SQL with range conditions) | Native (range types + GiST) | Not suitable | Not suitable |
| **Team expertise** | High (per role) | Medium | High | Low |
| **Operational maturity** | Excellent | Excellent | Excellent | Good |

**Selected: MySQL 8.0 (primary) + Redis (timeline cache + timer queue).**

**Justification:** MySQL provides ACID transactions needed for conflict-free reservation creation. While PostgreSQL's range types would simplify overlap queries, the team's MySQL expertise (per role requirements) is the deciding factor. We implement interval overlap detection in application code using SQL range queries on MySQL's B-tree indexes, which perform well at our scale (100,000 active reservations). Redis provides the fast timeline cache for feasibility checks and the sorted set timer queue for activation scheduling.

### Indexing Strategy

| Index | Table | Purpose |
|-------|-------|---------|
| `idx_res_time` | reservations | Range query for reservations overlapping a time window |
| `idx_res_status_time` | reservations | Find confirmed reservations due for activation |
| `idx_timeline_host_time` | host_resource_timeline | Conflict detection: find all commitments on a host overlapping a time range |
| `idx_res_region_az` | reservations | Filter by region/AZ when searching for available capacity |
| `idx_events_type_time` | reservation_events | Billing queries: find all activation events in a time range |

**Overlap detection query using B-tree indexes:**
```sql
-- Find all reservations on host_id that overlap with [new_start, new_end)
SELECT * FROM host_resource_timeline
WHERE host_id = ?
  AND status IN ('tentative', 'committed', 'active')
  AND start_time < ?  -- new_end
  AND end_time > ?    -- new_start
```
This uses the `idx_timeline_host_time` composite index efficiently.

---

## 5. API Design

### REST Endpoints

| Method | Path | Description | Auth | Rate Limit |
|--------|------|-------------|------|------------|
| POST | `/v1/reservations` | Create reservation | JWT (service/user) | 100/min/tenant |
| GET | `/v1/reservations/{id}` | Get reservation details | JWT | 1,000/min/tenant |
| GET | `/v1/reservations` | List reservations (filtered) | JWT | 500/min/tenant |
| PUT | `/v1/reservations/{id}` | Modify reservation (extend, change resources) | JWT | 50/min/tenant |
| DELETE | `/v1/reservations/{id}` | Cancel reservation | JWT | 100/min/tenant |
| POST | `/v1/reservations/dry-run` | Check feasibility | JWT | 500/min/tenant |
| POST | `/v1/reservations/{id}/extend` | Extend reservation end time | JWT | 100/min/tenant |
| GET | `/v1/availability` | Query available capacity over time | JWT | 200/min/tenant |
| POST | `/v1/recurring-templates` | Create recurring reservation template | JWT | 50/min/tenant |
| GET | `/v1/recurring-templates` | List recurring templates | JWT | 200/min/tenant |
| PUT | `/v1/recurring-templates/{id}` | Modify recurring template | JWT | 50/min/tenant |
| DELETE | `/v1/recurring-templates/{id}` | Cancel recurring template | JWT | 50/min/tenant |

#### Full Schema Examples

**POST /v1/reservations:**
```json
// Request
{
    "idempotency_key": "uuid-abc-123",
    "name": "ML Training - GPT Fine-tune",
    "project_id": "proj-ml-research",
    "start_time": "2026-04-15T02:00:00Z",
    "end_time": "2026-04-15T14:00:00Z",
    "timezone": "UTC",
    "resources": {
        "cpu_cores": 512,
        "ram_mb": 2097152,
        "gpu_count": 64,
        "gpu_type": "h100",
        "disk_gb": 10000,
        "net_gbps": 100
    },
    "constraints": {
        "region": "us-east-1",
        "availability_zone": "us-east-1a",
        "node_selector": {"gpu.nvidia.com/gpu-product": "H100"}
    },
    "reservation_type": "guaranteed",
    "priority": 500,
    "description": "Fine-tuning run for next release",
    "labels": {"team": "ml-research", "experiment": "gpt-finetune-v3"}
}

// Response (success)
{
    "reservation_id": "rsv-uuid-456",
    "status": "confirmed",
    "start_time": "2026-04-15T02:00:00Z",
    "end_time": "2026-04-15T14:00:00Z",
    "assigned_hosts": ["host-001", "host-002", ..., "host-008"],
    "total_cost_estimate": {
        "amount": 4608.00,
        "currency": "USD",
        "breakdown": {
            "gpu_h100_hours": "64 GPUs x 12 hours x $6.00/hr = $4,608.00"
        }
    },
    "warnings": [],
    "created_at": "2026-04-09T15:30:00Z"
}

// Response (conflict)
{
    "error": "CONFLICT",
    "message": "Cannot reserve 64 H100 GPUs in us-east-1a from 02:00 to 14:00",
    "conflicts": [
        {
            "reservation_id": "rsv-existing-789",
            "name": "Training job Alpha",
            "time_range": "2026-04-15T00:00:00Z - 2026-04-15T08:00:00Z",
            "resources": {"gpu_count": 48, "gpu_type": "h100"},
            "overlap": "02:00 - 08:00 (6 hours)"
        }
    ],
    "alternatives": [
        {"start_time": "2026-04-15T08:00:00Z", "end_time": "2026-04-15T20:00:00Z", "available_gpus": 64},
        {"start_time": "2026-04-16T02:00:00Z", "end_time": "2026-04-16T14:00:00Z", "available_gpus": 64}
    ]
}
```

**GET /v1/availability:**
```json
// Request: GET /v1/availability?region=us-east-1&gpu_type=h100&from=2026-04-15T00:00:00Z&to=2026-04-16T00:00:00Z&granularity=1h

// Response
{
    "region": "us-east-1",
    "gpu_type": "h100",
    "total_capacity": 128,
    "timeline": [
        {"time": "2026-04-15T00:00:00Z", "available": 80, "reserved": 48},
        {"time": "2026-04-15T01:00:00Z", "available": 80, "reserved": 48},
        {"time": "2026-04-15T02:00:00Z", "available": 32, "reserved": 96},
        {"time": "2026-04-15T08:00:00Z", "available": 80, "reserved": 48},
        // ...
    ]
}
```

### gRPC Service (internal — scheduler integration)

```protobuf
service ReservationService {
    rpc CreateReservation(CreateReservationRequest) returns (ReservationResponse);
    rpc CheckConflicts(ConflictCheckRequest) returns (ConflictCheckResponse);
    rpc ActivateReservation(ActivateRequest) returns (ActivateResponse);
    rpc ReleaseReservation(ReleaseRequest) returns (ReleaseResponse);
    rpc GetAvailableCapacity(CapacityRequest) returns (CapacityTimeline);
    rpc StreamReservationEvents(EventStreamRequest) returns (stream ReservationEvent);
}
```

### CLI Design

```bash
# Create reservation
reserve create \
    --name="ML Training Run" \
    --project=ml-research \
    --start="2026-04-15T02:00:00Z" \
    --end="2026-04-15T14:00:00Z" \
    --gpu=64 --gpu-type=h100 \
    --cpu=512 --ram=2048Gi \
    --region=us-east-1 --az=us-east-1a \
    --type=guaranteed \
    --output=json

# Check feasibility (dry run)
reserve check \
    --gpu=64 --gpu-type=h100 \
    --start="2026-04-15T02:00:00Z" \
    --duration=12h \
    --region=us-east-1

# View availability timeline
reserve availability \
    --region=us-east-1 --gpu-type=h100 \
    --from=2026-04-15 --to=2026-04-22 \
    --granularity=1h --output=chart

# List reservations
reserve list --project=ml-research --status=confirmed --output=table

# Modify reservation
reserve extend rsv-uuid-456 --new-end="2026-04-15T18:00:00Z"
reserve modify rsv-uuid-456 --gpu=32  # reduce GPU count

# Cancel
reserve cancel rsv-uuid-456 --reason="Experiment postponed"

# Recurring reservations
reserve recurring create \
    --name="Weekly Maintenance" \
    --cron="0 2 * * 2" \
    --duration=4h \
    --timezone="America/New_York" \
    --type=maintenance \
    --host-ids=host-001,host-002 \
    --effective-from=2026-04-01

reserve recurring list --output=table
reserve recurring pause tmpl-uuid-789
```

---

## 6. Core Component Deep Dives

### 6.1 Conflict Detection Engine (Interval Tree)

**Why it's hard:**
Detecting time-interval overlaps efficiently is the core challenge. A naive approach (check every existing reservation for overlap) is O(n) per query, which is 100,000 comparisons per reservation creation — too slow at 2,000 creations/sec. We need a data structure that answers "which existing reservations overlap with interval [s, e)?" in O(log n + k) time, where k is the number of overlapping results. Additionally, the problem is multi-dimensional: we must check overlaps *per resource* (a time overlap is only a conflict if the same resource type on the same host is double-booked).

**Approaches Compared:**

| Approach | Query Time | Insert Time | Memory | Multi-dim Support | Concurrent Access |
|----------|-----------|-------------|--------|-------------------|-------------------|
| Brute force scan | O(n) | O(1) | O(n) | Easy | Easy (DB lock) |
| Sorted interval list | O(n) worst case | O(log n) | O(n) | Medium | Hard |
| Interval tree (augmented BST) | O(log n + k) | O(log n) | O(n) | Medium | Hard |
| Segment tree | O(log n + k) | O(log n) | O(n) | Medium | Hard |
| R-tree (multi-dim spatial index) | O(log n + k) | O(log n) | O(n) | Native | Medium |
| MySQL range query with B-tree | O(log n + k) | O(log n) | DB | Easy (SQL) | Easy (DB lock) |

**Selected: MySQL range queries + in-memory interval tree cache.**

**Justification:** For the source of truth, MySQL range queries on the `host_resource_timeline` table are sufficient and simple. The composite B-tree index on `(host_id, start_time, end_time)` enables efficient overlap detection. For the hot path (feasibility checks, availability queries), we maintain an in-memory interval tree in each API service instance, backed by Redis. This gives us O(log n + k) lookup speed with the simplicity and ACID guarantees of MySQL for writes.

**Interval overlap condition:**
Two intervals [s1, e1) and [s2, e2) overlap if and only if: `s1 < e2 AND s2 < e1`.

**Implementation (pseudocode):**

```python
from sortedcontainers import SortedList
from dataclasses import dataclass
from typing import List, Optional
import threading

@dataclass
class Interval:
    start: int          # epoch seconds
    end: int            # epoch seconds
    reservation_id: str
    host_id: str
    resources: dict     # {cpu: X, ram: Y, gpu: Z, ...}

class IntervalTree:
    """Augmented BST for efficient interval overlap queries.
    
    Uses a sorted list of intervals ordered by start time, with each node
    augmented with the maximum end time in its subtree.
    """
    
    def __init__(self):
        self.intervals = SortedList(key=lambda iv: (iv.host_id, iv.start))
        self.lock = threading.RLock()
    
    def insert(self, interval: Interval):
        with self.lock:
            self.intervals.add(interval)
    
    def remove(self, reservation_id: str):
        with self.lock:
            to_remove = [iv for iv in self.intervals if iv.reservation_id == reservation_id]
            for iv in to_remove:
                self.intervals.remove(iv)
    
    def find_overlapping(self, host_id: str, start: int, end: int) -> List[Interval]:
        """Find all intervals on host_id that overlap with [start, end)."""
        with self.lock:
            overlapping = []
            # Binary search for the host's intervals
            # Use the sorted property: all intervals for a host are contiguous
            left = self.intervals.bisect_left(Interval(start=0, end=0, 
                                              reservation_id='', host_id=host_id, resources={}))
            
            for i in range(left, len(self.intervals)):
                iv = self.intervals[i]
                if iv.host_id != host_id:
                    break  # past this host's intervals
                if iv.start >= end:
                    break  # past the query window
                if iv.start < end and iv.end > start:
                    overlapping.append(iv)
            
            return overlapping
    
    def get_available_capacity(self, host_id: str, host_capacity: dict,
                                start: int, end: int) -> dict:
        """Calculate available capacity on a host during [start, end)."""
        overlapping = self.find_overlapping(host_id, start, end)
        
        # Find maximum resource commitment at any point in the interval
        # Build a timeline of events (starts and ends)
        events = []
        for iv in overlapping:
            clip_start = max(iv.start, start)
            clip_end = min(iv.end, end)
            events.append((clip_start, 'add', iv.resources))
            events.append((clip_end, 'remove', iv.resources))
        
        events.sort(key=lambda e: (e[0], 0 if e[1] == 'add' else 1))
        
        max_committed = {k: 0 for k in host_capacity}
        current = {k: 0 for k in host_capacity}
        
        for _, action, resources in events:
            for resource, amount in resources.items():
                if action == 'add':
                    current[resource] += amount
                else:
                    current[resource] -= amount
                max_committed[resource] = max(max_committed[resource], current[resource])
        
        available = {k: host_capacity[k] - max_committed.get(k, 0) for k in host_capacity}
        return available


class ConflictDetectionEngine:
    def __init__(self, db: MySQLClient, interval_tree: IntervalTree):
        self.db = db
        self.tree = interval_tree
    
    def check_conflicts(self, reservation: ReservationRequest) -> ConflictResult:
        """Check if a reservation request conflicts with existing reservations."""
        
        if reservation.host_ids:
            # Specific hosts requested (e.g., maintenance)
            target_hosts = reservation.host_ids
        else:
            # Find candidate hosts with enough total capacity
            target_hosts = self._find_candidate_hosts(reservation)
        
        feasible_hosts = []
        conflicts = []
        
        for host_id in target_hosts:
            host_capacity = self._get_host_capacity(host_id)
            available = self.tree.get_available_capacity(
                host_id, host_capacity,
                reservation.start_epoch, reservation.end_epoch)
            
            # Check if this host can satisfy the request
            if self._can_satisfy(available, reservation.resources):
                feasible_hosts.append(host_id)
            else:
                overlapping = self.tree.find_overlapping(
                    host_id, reservation.start_epoch, reservation.end_epoch)
                if overlapping:
                    conflicts.extend(overlapping)
        
        if len(feasible_hosts) >= self._hosts_needed(reservation):
            return ConflictResult(
                feasible=True,
                assigned_hosts=feasible_hosts[:self._hosts_needed(reservation)],
                conflicts=[])
        
        # Not enough hosts — find alternatives
        alternatives = self._find_alternative_slots(reservation)
        return ConflictResult(
            feasible=False,
            assigned_hosts=[],
            conflicts=conflicts,
            alternatives=alternatives)
    
    def _find_alternative_slots(self, reservation: ReservationRequest) -> List[TimeSlot]:
        """Find alternative time slots when the requested slot is unavailable."""
        duration = reservation.end_epoch - reservation.start_epoch
        alternatives = []
        
        # Search window: next 7 days from requested start
        search_start = reservation.start_epoch
        search_end = reservation.start_epoch + 7 * 86400
        
        # Slide a window of 'duration' length across the search range
        current = search_start
        while current < search_end and len(alternatives) < 5:
            candidate_end = current + duration
            
            # Quick feasibility check using interval tree
            hosts_available = 0
            for host_id in self._find_candidate_hosts(reservation):
                available = self.tree.get_available_capacity(
                    host_id, self._get_host_capacity(host_id),
                    current, candidate_end)
                if self._can_satisfy(available, reservation.resources):
                    hosts_available += 1
                    if hosts_available >= self._hosts_needed(reservation):
                        alternatives.append(TimeSlot(
                            start=current, end=candidate_end,
                            available_resources=self._aggregate_available(current, candidate_end)))
                        current = candidate_end  # skip to end of this slot
                        break
            
            current += 3600  # advance by 1 hour
        
        return alternatives
    
    def create_reservation_atomic(self, reservation: ReservationRequest) -> Reservation:
        """Create a reservation with atomic conflict check + commit."""
        with self.db.transaction():
            # Lock the affected host rows to prevent concurrent conflicting reservations
            hosts_needed = self._hosts_needed(reservation)
            
            if reservation.host_ids:
                candidate_hosts = reservation.host_ids
            else:
                candidate_hosts = self._find_candidate_hosts(reservation)
            
            assigned = []
            for host_id in candidate_hosts:
                if len(assigned) >= hosts_needed:
                    break
                
                # SELECT FOR UPDATE on the host's timeline entries
                existing = self.db.query("""
                    SELECT SUM(cpu_cores), SUM(ram_mb), SUM(gpu_count)
                    FROM host_resource_timeline
                    WHERE host_id = %s
                      AND status IN ('tentative', 'committed', 'active')
                      AND start_time < %s
                      AND end_time > %s
                    FOR UPDATE
                """, host_id, reservation.end_time, reservation.start_time)
                
                host_cap = self._get_host_capacity(host_id)
                remaining_cpu = host_cap['cpu'] - (existing.cpu or 0)
                remaining_gpu = host_cap['gpu'] - (existing.gpu or 0)
                
                if (remaining_cpu >= reservation.per_host_cpu and
                    remaining_gpu >= reservation.per_host_gpu):
                    assigned.append(host_id)
            
            if len(assigned) < hosts_needed:
                raise InsufficientCapacityError(
                    f"Need {hosts_needed} hosts, only {len(assigned)} available")
            
            # Commit reservation
            rsv = self.db.insert('reservations', {
                'reservation_id': uuid4(),
                'status': 'confirmed',
                'assigned_host_ids': json.dumps(assigned),
                **reservation.to_dict()
            })
            
            # Insert timeline entries for each assigned host
            for host_id in assigned:
                self.db.insert('host_resource_timeline', {
                    'host_id': host_id,
                    'reservation_id': rsv.reservation_id,
                    'start_time': reservation.start_time,
                    'end_time': reservation.end_time,
                    'cpu_cores': reservation.per_host_cpu,
                    'ram_mb': reservation.per_host_ram,
                    'gpu_count': reservation.per_host_gpu,
                    'status': 'committed'
                })
            
            # Update in-memory interval tree
            for host_id in assigned:
                self.tree.insert(Interval(
                    start=reservation.start_epoch,
                    end=reservation.end_epoch,
                    reservation_id=str(rsv.reservation_id),
                    host_id=host_id,
                    resources={'cpu': reservation.per_host_cpu,
                             'ram': reservation.per_host_ram,
                             'gpu': reservation.per_host_gpu}))
            
            # Publish event
            self.events.publish('reservation_created', rsv)
            
            return rsv
```

**Failure Modes:**
1. **In-memory tree stale after restart:** On service restart, the interval tree is empty. Mitigation: rebuilt from MySQL on startup (100,000 active reservations x 2 KB = 200 MB, takes ~5 seconds). During rebuild, use MySQL directly for conflict checks (slower but correct).
2. **Concurrent creation race:** Two requests for the same GPU slot arrive simultaneously. Mitigation: `SELECT FOR UPDATE` in MySQL serializes the commits. One succeeds, the other gets InsufficientCapacityError and can retry.
3. **Interval tree diverges from MySQL:** A MySQL write succeeds but the tree update fails (e.g., OOM). Mitigation: periodic reconciliation (every 60 seconds) rebuilds the tree from MySQL. The tree is always considered a cache, never the source of truth.
4. **Large alternative search:** Finding alternatives across 7 days with 50,000 hosts is expensive. Mitigation: pre-computed capacity summary per region/AZ (aggregated every 5 minutes). Use the summary for fast approximate search, then verify with precise check.

**Interviewer Q&As:**

**Q1: Why not use PostgreSQL's native range types for overlap detection?**
A: PostgreSQL's `tstzrange` with GiST index would be ideal for this use case — `WHERE reservation_range && '[start, end)'` is both elegant and efficient. However, the role specifies MySQL expertise. Our MySQL solution using `start_time < end AND end_time > start` with composite B-tree indexes achieves similar performance for our scale (100K reservations).

**Q2: How does the interval tree handle multi-dimensional resource conflicts?**
A: The interval tree detects *temporal* overlaps. For each temporal overlap, we sum the committed resources and compare against host capacity. A time overlap is only a conflict if the sum of committed resources (existing + new) exceeds host capacity for *any* resource dimension.

**Q3: What's the time complexity of the alternative slot search?**
A: Worst case: O(7 * 24 * H * K) where H is candidate hosts and K is overlapping intervals per host per slot. With pre-computed capacity summaries, this reduces to O(7 * 24) lookups in the summary table + one precise verification per candidate slot. In practice: < 100ms.

**Q4: How do you handle time zone complexities for reservations?**
A: All internal storage uses UTC. The `timezone` field records the user's timezone for display purposes. Recurring reservations use the specified timezone for cron evaluation (important for DST transitions — "every Tuesday at 2 AM EST" should be 2 AM local time even when the UTC offset changes).

**Q5: How would you implement partial fulfillment?**
A: If the full request (64 GPUs) can't be met, we search for the largest feasible subset: try 64, then 56, 48, 32, 16. For each size, run the conflict check. Return the largest feasible option along with the alternatives. The client can accept the partial fulfillment or request a different time.

**Q6: How do you prevent reservation sprawl (users reserving and never using)?**
A: Three mechanisms: (1) Reservations have a billing cost — even if the workload never runs, the reservation is charged. (2) "No-show" detection: if a reservation transitions from CONFIRMED to ACTIVE but no workload is launched within 15 minutes, it's flagged for review. (3) Maximum concurrent reservations per project (configurable, default 50).

---

### 6.2 Reservation Lifecycle Manager (State Machine)

**Why it's hard:**
A reservation's lifecycle involves multiple state transitions, each with side effects (billing events, resource locks, workload triggers). Concurrent operations (modify while activating, cancel while extending) create complex race conditions. The state machine must be deterministic, handle all edge cases, and recover from partial failures.

**State Machine:**

```
                    ┌───────────┐
                    │ REQUESTED │──── (validation failed) ───► FAILED
                    └─────┬─────┘
                          │ (conflict check passed)
                          ▼
                    ┌───────────┐
        ┌───────── │ CONFIRMED │ ──── (user cancels) ──────► CANCELLED
        │          └─────┬─────┘
        │                │ (start_time reached)
        │                ▼
        │          ┌───────────┐
        │ (higher  │  ACTIVE   │ ──── (user cancels) ──────► CANCELLED
        │ priority │           │ ──── (end_time reached) ──► COMPLETED
        │ preempts)└─────┬─────┘
        │                │
        ▼                ▼
  ┌───────────┐    ┌───────────┐
  │ DISPLACED │    │  EXPIRED  │ (end_time + grace period, no explicit completion)
  └───────────┘    └───────────┘
```

**Valid Transitions:**

| From | To | Trigger | Side Effects |
|------|-----|---------|-------------|
| REQUESTED | CONFIRMED | Conflict check passes | Lock resources in timeline, emit billing event |
| REQUESTED | FAILED | Validation fails, no capacity | Emit failure event |
| CONFIRMED | ACTIVE | Clock reaches start_time | Launch workloads, emit activation event |
| CONFIRMED | CANCELLED | User cancellation | Release timeline entries, emit cancellation + refund event |
| CONFIRMED | DISPLACED | Higher priority reservation needs resources | Release timeline, emit displacement + compensation event |
| ACTIVE | COMPLETED | Clock reaches end_time | Terminate workloads, release timeline, emit completion event |
| ACTIVE | CANCELLED | User cancellation | Terminate workloads, release timeline, emit cancellation event |
| ACTIVE | DISPLACED | Higher priority preemption | Terminate workloads, release timeline, emit displacement event |
| ACTIVE | EXPIRED | end_time + 15 min grace, workloads still running | Force-terminate, release timeline, emit expiry event |

**Implementation (pseudocode):**

```python
class ReservationLifecycleManager:
    VALID_TRANSITIONS = {
        'requested':  {'confirmed', 'failed'},
        'confirmed':  {'active', 'cancelled', 'displaced'},
        'active':     {'completed', 'cancelled', 'displaced', 'expired'},
        'completed':  set(),  # terminal
        'cancelled':  set(),  # terminal
        'displaced':  set(),  # terminal
        'failed':     set(),  # terminal
        'expired':    set(),  # terminal
    }
    
    def transition(self, reservation_id: str, new_status: str, 
                   actor: str = 'system', details: dict = None) -> Reservation:
        with self.db.transaction():
            # Lock the reservation row
            rsv = self.db.query(
                "SELECT * FROM reservations WHERE reservation_id = %s FOR UPDATE",
                reservation_id)
            
            if not rsv:
                raise ReservationNotFoundError(reservation_id)
            
            # Validate transition
            if new_status not in self.VALID_TRANSITIONS.get(rsv.status, set()):
                raise InvalidTransitionError(
                    f"Cannot transition from {rsv.status} to {new_status}")
            
            old_status = rsv.status
            
            # Execute side effects
            self._execute_side_effects(rsv, old_status, new_status, details)
            
            # Update reservation
            self.db.execute(
                "UPDATE reservations SET status = %s, version = version + 1, "
                "updated_at = NOW(3) WHERE reservation_id = %s AND version = %s",
                new_status, reservation_id, rsv.version)
            
            # Record event
            self.db.insert('reservation_events', {
                'reservation_id': reservation_id,
                'event_type': new_status,
                'old_status': old_status,
                'new_status': new_status,
                'details': json.dumps(details or {}),
                'actor_id': actor,
                'actor_type': 'user' if actor != 'system' else 'system'
            })
            
            # Publish to Kafka
            self.events.publish(f'reservation_{new_status}', {
                'reservation_id': reservation_id,
                'old_status': old_status,
                'new_status': new_status,
                'resources': rsv.resources_dict(),
                'start_time': rsv.start_time.isoformat(),
                'end_time': rsv.end_time.isoformat(),
                'details': details
            })
            
            return self.db.query(
                "SELECT * FROM reservations WHERE reservation_id = %s",
                reservation_id)
    
    def _execute_side_effects(self, rsv: Reservation, old: str, new: str, details: dict):
        """Execute side effects for a state transition."""
        
        if old == 'requested' and new == 'confirmed':
            # Resources already locked during creation (in ConflictDetectionEngine)
            pass
        
        elif old == 'confirmed' and new == 'active':
            # Update timeline entries to 'active'
            self.db.execute(
                "UPDATE host_resource_timeline SET status = 'active' "
                "WHERE reservation_id = %s", rsv.reservation_id)
            
            rsv.activated_at = datetime.utcnow()
            
            # Trigger workload launch
            self.scheduler.launch_reserved_workloads(rsv)
        
        elif new in ('cancelled', 'displaced', 'completed', 'expired'):
            # Release timeline entries
            self.db.execute(
                "UPDATE host_resource_timeline SET status = 'released' "
                "WHERE reservation_id = %s", rsv.reservation_id)
            
            # Update interval tree cache
            self.interval_tree.remove(rsv.reservation_id)
            
            rsv.completed_at = datetime.utcnow()
            
            if old == 'active':
                # Terminate running workloads
                grace_period = 30 if new != 'expired' else 0
                self.scheduler.terminate_reserved_workloads(rsv, grace_period)
            
            if new == 'displaced':
                # Apply compensation
                self._apply_compensation(rsv, details)
            
            if new == 'cancelled' and old == 'confirmed':
                # Refund (reservation was never used)
                self._emit_refund_event(rsv)
    
    def _apply_compensation(self, rsv: Reservation, details: dict):
        """Apply compensation when a reservation is displaced."""
        group = self.db.query(
            "SELECT * FROM overbooking_groups WHERE group_name = %s",
            rsv.overbooking_group) if rsv.overbooking_group else None
        
        if group:
            policy = group.compensation_policy
        else:
            policy = 'credit'  # default
        
        if policy == 'refund':
            self.billing.emit_refund(rsv)
        elif policy == 'credit':
            # Credit for future reservation at 1.5x the lost time
            lost_hours = (rsv.end_time - datetime.utcnow()).total_seconds() / 3600
            credit_hours = lost_hours * 1.5
            self.billing.emit_credit(rsv, credit_hours)
        elif policy == 'reschedule':
            # Automatically find next available slot and reschedule
            alternative = self.conflict_engine.find_alternative_slots(rsv)[0]
            self._create_replacement_reservation(rsv, alternative)
```

**Failure Modes:**
1. **Workload launch failure during activation:** Reservation transitions to ACTIVE but workloads fail to start. Mitigation: activation engine retries 3 times. If still failing, reservation stays ACTIVE (resources reserved) but a FAILED_ACTIVATION event is emitted and operator is alerted. Resources are not released — the user has a guaranteed slot.
2. **Crash during side effects:** Transaction commits state change but side effects (Kafka publish, interval tree update) fail. Mitigation: outbox pattern — side effects are recorded in a `pending_side_effects` table within the same transaction. A background worker processes pending side effects and retries until success.
3. **Concurrent modification:** User extends reservation while system tries to complete it. Mitigation: optimistic locking (version column) ensures only one writer succeeds. The loser retries and sees the updated state.

**Interviewer Q&As:**

**Q1: Why use optimistic locking instead of pessimistic locking for the state machine?**
A: Pessimistic locking (`SELECT FOR UPDATE`) is used within a single transaction (e.g., during creation). Optimistic locking (version column) is used for concurrent modifications from different sources (user modification vs system activation). This avoids holding locks across network calls (which could cause deadlocks or long waits).

**Q2: How do you handle the outbox pattern for reliable event publishing?**
A: Within the MySQL transaction that changes reservation state, we also insert a row into `pending_events`. A background worker polls this table every 100ms, publishes events to Kafka, and marks them as published. If Kafka is down, events accumulate in MySQL and are published when Kafka recovers. This guarantees at-least-once delivery.

**Q3: What happens if the Activation Engine crashes at exactly the reservation start_time?**
A: The Activation Engine runs multiple replicas with leader election. If the leader crashes, a standby takes over within 5 seconds. On startup, the new leader checks for overdue activations (reservations with status=CONFIRMED and start_time < now) and activates them. Worst case: workloads start 5-10 seconds late.

**Q4: How do you handle reservation extensions that conflict with subsequent reservations?**
A: An extension is treated as a new reservation for the extended period. The conflict engine checks the extension interval against existing reservations. If it conflicts, the extension is denied with alternative options (e.g., a shorter extension that doesn't conflict).

**Q5: How do you handle reservations that span DST transitions?**
A: If a user reserves "2 AM to 6 AM in America/New_York", the system converts to UTC at creation time. During the spring-forward DST transition, 2 AM becomes 3 AM, so the reservation is 3 hours instead of 4. We document this behavior and recommend users specify UTC for precision or use explicit duration-based reservations.

**Q6: What's the compensation model for displaced reservations?**
A: Three tiers: (1) Full refund for reservations displaced before activation. (2) 1.5x credit for reservations displaced during active use (compensates for disruption). (3) Automatic rescheduling to the next available slot (with no additional cost). The compensation policy is configurable per overbooking group.

---

### 6.3 Overbooking Strategy

**Why it's hard:**
Strict "no overbooking" leads to low utilization — reserved capacity sits idle when the tenant doesn't use it. Airlines, hotels, and cloud providers all overbook because a fraction of reservations are never fully utilized. The challenge is: overbook enough to maximize utilization but not so much that displacements are frequent or unpredictable. When displacement is necessary, the system must choose victims fairly and compensate them.

**Approaches Compared:**

| Approach | Utilization | Displacement Risk | Predictability | Complexity |
|----------|------------|-------------------|----------------|------------|
| No overbooking | Low (60-70%) | 0% | Perfect | None |
| Fixed ratio (e.g., 1.2x) | Medium (80-85%) | 3-5% | Medium | Low |
| Historical model (predict no-shows) | High (85-90%) | 2-3% | Medium | High |
| Per-resource adaptive | High (85-90%) | 1-2% | High | High |
| Guaranteed + best-effort tiers | High (90%+) | 0% for guaranteed, 5-10% for best-effort | High | Medium |

**Selected: Guaranteed + best-effort tiers with historical-based adaptive overbooking for best-effort.**

**Justification:** Two-tier approach: "guaranteed" reservations are never overbooked (100% reliable). "Best-effort" reservations are overbooked based on historical utilization patterns. This gives predictability for critical workloads (ML training, production maintenance) while maximizing utilization for less critical needs. The adaptive model uses historical no-show rates per tenant per resource type.

**Implementation (pseudocode):**

```python
class OverbookingManager:
    def __init__(self):
        self.base_ratio = 1.20  # default 20% overbooking for best-effort
        self.max_ratio = 1.50   # never overbook more than 50%
        self.target_displacement_rate = 0.03  # target < 3% displacement
    
    def get_effective_capacity(self, host_id: str, resource_type: str,
                               start: int, end: int) -> float:
        """Get the effective bookable capacity, considering overbooking."""
        physical_capacity = self._get_host_capacity(host_id, resource_type)
        
        # Guaranteed reservations: count at face value
        guaranteed_committed = self._sum_committed(
            host_id, resource_type, start, end, reservation_type='guaranteed')
        
        # Best-effort reservations: apply overbooking ratio
        overbooking_ratio = self._get_adaptive_ratio(host_id, resource_type)
        best_effort_capacity = physical_capacity * overbooking_ratio - guaranteed_committed
        
        return max(0, best_effort_capacity)
    
    def _get_adaptive_ratio(self, host_id: str, resource_type: str) -> float:
        """Calculate adaptive overbooking ratio based on historical data."""
        # Historical no-show rate for this resource type in this region
        region = self._get_host_region(host_id)
        
        # Query last 90 days of reservation data
        stats = self.db.query("""
            SELECT 
                COUNT(*) as total_reservations,
                SUM(CASE WHEN actual_peak_usage < req_amount * 0.5 THEN 1 ELSE 0 END) as underutilized,
                AVG(actual_peak_usage / req_amount) as avg_utilization_ratio
            FROM reservation_usage_history
            WHERE region = %s AND resource_type = %s
              AND reservation_type = 'best_effort'
              AND created_at > DATE_SUB(NOW(), INTERVAL 90 DAY)
        """, region, resource_type)
        
        if stats.total_reservations < 100:
            return self.base_ratio  # not enough data, use default
        
        # Adaptive ratio: inverse of average utilization
        # If tenants typically use 80% of reserved capacity, we can overbook by 1/0.8 = 1.25x
        avg_util = stats.avg_utilization_ratio
        adaptive_ratio = 1.0 / max(avg_util, 0.5)  # cap at 2x
        
        # Adjust for recent displacement rate
        recent_displacement_rate = self._get_recent_displacement_rate(region, resource_type)
        if recent_displacement_rate > self.target_displacement_rate:
            # Reduce overbooking
            adaptive_ratio *= 0.95
        elif recent_displacement_rate < self.target_displacement_rate * 0.5:
            # Can increase overbooking
            adaptive_ratio *= 1.05
        
        # Clamp to [1.0, max_ratio]
        return min(max(adaptive_ratio, 1.0), self.max_ratio)
    
    def select_displacement_victims(self, host_id: str, resource_type: str,
                                     amount_needed: float) -> List[Reservation]:
        """Select best-effort reservations to displace when capacity is needed."""
        # Only best-effort reservations can be displaced
        candidates = self.db.query("""
            SELECT r.* FROM reservations r
            JOIN host_resource_timeline hrt ON r.reservation_id = hrt.reservation_id
            WHERE hrt.host_id = %s
              AND r.reservation_type = 'best_effort'
              AND r.status IN ('confirmed', 'active')
              AND hrt.status IN ('committed', 'active')
            ORDER BY r.priority ASC, r.created_at DESC
        """, host_id)
        
        victims = []
        freed = 0.0
        
        for candidate in candidates:
            victims.append(candidate)
            freed += getattr(candidate, f'req_{resource_type}')
            if freed >= amount_needed:
                return victims
        
        return []  # can't free enough even displacing all best-effort
```

**Failure Modes:**
1. **Model underpredicts utilization:** Overbooking ratio too high, displacement rate exceeds 5%. Mitigation: safety cap at 1.5x. Automatic ratio reduction when displacement rate exceeds threshold. Daily monitoring alert.
2. **Correlated demand spike:** All overbooked reservations are actually used simultaneously (e.g., end-of-quarter crunch). Mitigation: guaranteed tier is never displaced. Best-effort tenants are warned about displacement risk. Emergency capacity can be provisioned (autoscaler).
3. **Unfair displacement:** Same tenant always gets displaced. Mitigation: round-robin displacement across tenants. Track displacement history per tenant and avoid displacing the same tenant more than once per week.

**Interviewer Q&As:**

**Q1: How does this compare to airline overbooking?**
A: Very similar principles. Airlines overbook by ~15% because ~10-15% of passengers are no-shows. We overbook by ~20% because ~20-30% of reserved cloud capacity goes unused. The key difference: airlines can bump passengers to the next flight; we can reschedule workloads but the disruption cost varies greatly (a 5-minute batch job vs a 12-hour ML training run).

**Q2: How do you decide the overbooking ratio for GPU resources?**
A: GPU overbooking is more conservative (typically 1.05-1.10x) because GPUs are expensive and displacement of ML training is costly (hours of lost progress). The adaptive model uses GPU-specific historical data. Some customers request guaranteed GPU reservations with zero overbooking.

**Q3: What legal/contractual implications does overbooking have?**
A: The reservation terms of service clearly state the difference between "guaranteed" (100% SLA, refund if breached) and "best-effort" (may be displaced, compensated per policy). Best-effort reservations are priced 15-30% lower than guaranteed, incentivizing tenants to choose the appropriate tier.

**Q4: How do you prevent gaming — tenants always choosing guaranteed to avoid displacement?**
A: Price differentiation. Guaranteed reservations cost 30% more. For tenants who don't need absolute guarantees (dev/test, batch jobs), best-effort is significantly cheaper. Also, guaranteed capacity is limited — not all capacity can be guaranteed.

**Q5: How would you implement a "standby" list for displaced reservations?**
A: Displaced reservations are automatically added to a priority standby queue. When capacity becomes available (another reservation ends or is cancelled), the standby queue is checked. The highest-priority standby reservation gets the newly freed capacity. This is similar to airline standby lists.

**Q6: What metrics indicate the overbooking model needs tuning?**
A: (1) Displacement rate (target < 3%). (2) Wasted capacity (target < 10% — if too much waste, increase overbooking ratio). (3) Utilization ratio (actual usage / reserved capacity — lower means more room to overbook). (4) Customer satisfaction score for displaced tenants.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

The reservation system acts as a constraint for the real-time scheduler:
1. When a reservation becomes ACTIVE, the scheduler receives a "place these workloads on these specific hosts" directive.
2. The hosts were pre-selected during reservation confirmation, so placement is deterministic (no scoring needed).
3. If a reserved host has failed since confirmation, the activation engine finds an equivalent replacement host (with immediate conflict check) and updates the reservation.

### Conflict Detection

See Section 6.1 (Interval Tree) for the full implementation.

Summary: Two-phase conflict detection:
1. **Fast path (cache):** In-memory interval tree detects temporal overlaps in O(log n + k). Checks resource sums against host capacity.
2. **Definitive path (DB):** MySQL `SELECT FOR UPDATE` during commit prevents race conditions between concurrent reservation creators.

### Queue & Priority

Reservation priority tiers (highest to lowest):
| Priority Range | Name | Use Case | Preemption Power |
|---------------|------|----------|-----------------|
| 900-1000 | System Maintenance | Critical infra maintenance | Can preempt all |
| 700-899 | Production Guaranteed | Production workload reservations | Can preempt best-effort |
| 400-699 | Standard Guaranteed | Normal guaranteed reservations | Can preempt best-effort |
| 100-399 | Best-effort | Overbooked capacity | Cannot preempt |
| 0-99 | Preemptible | Spot-like usage | Cannot preempt |

### Preemption Policy

When a higher-priority reservation needs resources occupied by lower-priority reservations:
1. Identify minimum set of lower-priority reservations to displace.
2. Notify displaced tenants immediately (webhook + Slack/email).
3. Give 15-minute grace period for active reservations (allow workload checkpointing).
4. After grace period, force-terminate and release resources.
5. Apply compensation per overbooking group policy.
6. Attempt automatic rescheduling of displaced reservations.

### Starvation Prevention

1. Maximum displacement frequency: a reservation can only be displaced once. After displacement + rescheduling, the new reservation is automatically upgraded to "guaranteed."
2. Priority aging: best-effort reservations gain +10 priority per day of advance booking. A best-effort reservation booked 30 days in advance has +300 priority.
3. Tenant fairness: no tenant can have more than 5% of their active reservations displaced in any 30-day period.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling | Notes |
|-----------|---------|-------|
| **Reservation API** | Stateless, behind LB | 10 instances for 2,000 req/sec |
| **Conflict Detection** | Each API instance has local interval tree copy | All instances read from same Redis/MySQL |
| **Lifecycle Manager** | Stateless (state in MySQL) | Scales with API |
| **Activation Engine** | Leader-elected (1 active per AZ) | Low volume, doesn't need horizontal scale |
| **Recurring Expander** | Single instance (daily cron) | Scales by parallelizing per-template |

### Database Scaling

| Strategy | Implementation |
|----------|---------------|
| **Read replicas** | 2 MySQL replicas for analytics and dashboard queries |
| **Partitioning** | `host_resource_timeline` partitioned by `start_time` (monthly). Old partitions archived to S3. |
| **Connection pooling** | ProxySQL, 200 connections shared across API instances |
| **Index optimization** | Covering indexes for conflict detection queries (avoid table lookup) |

### Caching

| Layer | Technology | Data | TTL | Hit Ratio |
|-------|-----------|------|-----|-----------|
| **L1 — In-process interval tree** | Custom data structure | All active + confirmed reservation intervals | Rebuilt on startup + real-time updates | 99% |
| **L2 — Redis** | Redis Cluster | Capacity timeline summaries, activation timer queue | Real-time updates | 95% |
| **L3 — MySQL** | InnoDB buffer pool | Hot reservation data | Source of truth | N/A |

**Interviewer Q&As:**

**Q1: How do you rebuild the interval tree after a service restart?**
A: Query all active and confirmed reservations from MySQL with their host timeline entries. Insert each into the tree. At 100,000 reservations with ~3 host entries each, this is 300,000 insertions. With sorted insertion: ~2 seconds. During rebuild, requests fall back to MySQL for conflict checks.

**Q2: How does the interval tree scale with millions of reservations?**
A: Active + confirmed reservations are typically 100,000. Historical (completed/cancelled) are not in the tree. Memory: ~100 MB for 300,000 intervals. If scale exceeds this, shard the tree by region/AZ — each API instance handles one region.

**Q3: What's the MySQL bottleneck for conflict detection?**
A: The `SELECT FOR UPDATE` during reservation commit serializes per-host. If two reservations target the same host, one waits. At 2,000 reservations/sec spread across 50,000 hosts, lock contention is extremely rare (< 0.1%). If contention increases, batch host locking with a consistent ordering to prevent deadlocks.

**Q4: How do you handle capacity timeline queries for large time ranges (e.g., 1 year)?**
A: Aggregated capacity summaries are precomputed at 1-hour granularity and stored in Redis. For a 1-year query, we return 8,760 data points from the summary. The summary is updated whenever a reservation is created/modified/cancelled.

**Q5: What's the disaster recovery strategy?**
A: MySQL replication to a standby region. Reservations are the source of truth. If the primary region fails, the standby is promoted. In-flight activations are detected and retried by the standby Activation Engine. Reservations that were "ACTIVE" in the primary are re-activated in the standby (workloads restarted on standby-region hosts).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery | RTO |
|---------|--------|-----------|----------|-----|
| API service crash | Reduced throughput | Health check (5s) | Auto-restart, LB removes | 5s |
| Activation Engine crash | Reservations activate late | Leader health check | Standby takes over. Catches up overdue activations. | 10s |
| MySQL primary failure | No new reservations | Replication lag, health check | Promote replica. In-memory trees continue serving reads. | 30-60s |
| Redis failure | Stale capacity timeline, timer queue lost | Redis health check | Rebuild from MySQL. Re-create timer queue from confirmed reservations. | 30s |
| Interval tree corruption | False negatives in conflict detection | Periodic reconciliation detects mismatch | Rebuild tree from MySQL | 60s |
| Network partition | Split-brain reservation creation | Duplicate detection via idempotency keys | Reconciliation detects and resolves duplicates | Manual |

### Automated Recovery

1. **Timer queue reconstruction:** On Activation Engine restart, scan MySQL for all CONFIRMED reservations with `start_time <= now + 24h` and re-add to Redis sorted set.
2. **Orphaned reservation cleanup:** Daily job finds reservations in ACTIVE state past their `end_time + grace_period`. Transitions them to EXPIRED.
3. **Stale timeline entry cleanup:** Daily job finds timeline entries in 'committed' state for reservations that are CANCELLED/COMPLETED. Updates them to 'released'.

### Retry Strategy

| Operation | Strategy | Retries | Backoff |
|-----------|----------|---------|---------|
| Reservation creation (lock conflict) | Retry with fresh conflict check | 3 | 100ms, 200ms, 400ms |
| Activation (workload launch) | Retry | 3 | 10s, 20s, 40s |
| Kafka event publish | Outbox pattern + retry | Unlimited | 100ms, capped at 5s |
| Interval tree update | Retry, reconcile on failure | 2 | Immediate |

### Circuit Breaker

| Dependency | Threshold | Open Duration | Fallback |
|------------|-----------|---------------|----------|
| MySQL | 3 failures/10s | 30s | Serve reads from cache. Queue writes. |
| Redis | 5 failures/5s | 15s | Use MySQL directly for conflict checks + timer queries |
| Kafka | 10 failures/30s | 60s | Buffer events in MySQL outbox table |
| Scheduler API | 3 failures/10s | 60s | Activation retries. Alert on-call. |

### Consensus & Coordination

- **Activation Engine leader election:** etcd lease (15s TTL, 10s renew). Single leader per AZ.
- **No distributed consensus for reservation creation:** MySQL transactions provide serialization. Each API instance talks directly to MySQL.
- **Cross-AZ reservation:** Each AZ manages its own hosts. Cross-AZ reservations are decomposed into per-AZ sub-reservations, each managed independently.

---

## 10. Observability

### Key Metrics

| Metric | Type | Labels | Alert Threshold |
|--------|------|--------|-----------------|
| `reservation_create_latency_ms` | Histogram | type, region | p99 > 200ms |
| `reservation_create_total` | Counter | type, result(success/conflict/error) | Conflict rate > 20% |
| `reservation_active_count` | Gauge | type, region | Approaching cluster capacity |
| `reservation_activation_delay_sec` | Histogram | | p99 > 60s |
| `reservation_displacement_total` | Counter | reason, region | > 10/day |
| `overbooking_ratio` | Gauge | region, resource_type | > 1.40 |
| `capacity_utilization_reserved_pct` | Gauge | region, resource_type | > 90% |
| `conflict_check_latency_ms` | Histogram | method(tree/mysql) | p99 > 50ms |
| `interval_tree_size` | Gauge | | > 500,000 |
| `timer_queue_depth` | Gauge | | > 10,000 |
| `recurring_expansion_errors` | Counter | | > 0 |

### Distributed Tracing

Trace per reservation creation: `api_receive → validate → conflict_check → mysql_commit → cache_update → event_publish → respond`.
Key spans annotated with: hosts_checked, conflicts_found, assigned_hosts, total_latency.

### Logging

| Level | When | Content |
|-------|------|---------|
| INFO | Reservation created/activated/completed | reservation_id, type, resources, hosts |
| WARN | Conflict detected | reservation_id, conflicting_reservations, alternatives_offered |
| WARN | Displacement triggered | displaced_id, preempting_id, compensation |
| ERROR | Activation failure | reservation_id, error, retry_count |
| ERROR | Timer queue missed activation | reservation_id, expected_time, actual_time, delay |

### Alerting

| Alert | Condition | Severity |
|-------|-----------|----------|
| ActivationDelayed | Any reservation activated > 5 min late | P2 |
| HighDisplacementRate | > 5% displacement rate in 24h | P2 |
| ConflictRateHigh | > 30% of reservation attempts conflict | P3 |
| TimerQueueStuck | Queue depth growing for > 10 min | P1 |
| OverbookingRatioHigh | Ratio > 1.4 for any resource type | P3 |

---

## 11. Security

### Auth & AuthZ

- **Reservation creation:** Requires `reservation:create` permission for the target project. JWT with project-scoped claims.
- **Reservation modification/cancellation:** Only the creator or project admin can modify/cancel.
- **Maintenance reservations:** Require `infra:maintenance` role (platform team only).
- **Guaranteed reservations:** May require additional approval if the cost exceeds a threshold.
- **Overbooking configuration:** Admin-only access.

### Secrets Management

- Database credentials via Vault with 1-hour lease.
- Kafka credentials rotated weekly.
- API authentication via short-lived JWTs (1 hour).
- Inter-service communication via mTLS.

### Audit Logging

All reservation lifecycle events are immutably logged:
- Who created, modified, cancelled, or displaced each reservation.
- Full resource details and time windows.
- Approval chain for guaranteed reservations exceeding cost threshold.
- Displacement events with compensation details.
- Audit log retained for 7 years (compliance).

---

## 12. Incremental Rollout Strategy

**Phase 1 — Read-Only (Week 1-2):**
Deploy reservation system in read-only mode. Import existing scheduled jobs as reservations. Display availability dashboard without enforcement.

**Phase 2 — Soft Enforcement (Week 3-4):**
Enable reservation creation but without blocking non-reserved workloads. Log when a non-reserved workload would have been blocked by a reservation.

**Phase 3 — Maintenance Reservations (Week 5-6):**
Enable full enforcement for maintenance windows only. These are low-risk (short duration, planned) and high-value (prevent accidental scheduling during maintenance).

**Phase 4 — Guaranteed Reservations (Week 7-10):**
Enable guaranteed reservations for ML training jobs (the primary use case). These are high-value and well-understood by the ML team.

**Phase 5 — Best-Effort + Overbooking (Week 11-14):**
Enable best-effort reservations with conservative overbooking (1.10x initially). Monitor displacement rate. Gradually increase to target ratio.

**Rollout Q&As:**

**Q1: How do you migrate existing scheduled jobs to the reservation system?**
A: Export scheduled jobs from the current scheduler (cron-based or manual). For each, create a corresponding reservation. The reservation system inherits the existing schedule without changing the user's workflow. They can later modify or cancel via the new system.

**Q2: What if the overbooking model is wrong initially?**
A: Start with 1.10x (very conservative). Monitor for 2 weeks. If displacement rate is 0%, increase to 1.15x. Repeat until displacement rate approaches 3% target. The adaptive model takes over once we have 90 days of data.

**Q3: How do you handle the transition period where some workloads use reservations and others don't?**
A: Non-reserved workloads use the existing placement system. Reserved workloads have their capacity pre-allocated and are guaranteed their slot. The two systems coexist — reserved capacity is "invisible" to the non-reserved scheduler.

**Q4: What's the rollback plan?**
A: Feature flags control reservation enforcement. Rollback = disable enforcement. Existing reservations remain in the system but are ignored by the scheduler. Workloads continue to be placed by the existing system.

**Q5: How do you validate the timer queue reliability before production?**
A: Create 10,000 test reservations with start times spread over the next 24 hours. Monitor activation accuracy. All must activate within 30 seconds of their start_time. If any are late, investigate and fix before enabling for production workloads.

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Risk |
|----------|-------------------|--------|-----------|------|
| Conflict detection data structure | Brute force vs R-tree vs Interval tree vs MySQL range query | MySQL + in-memory interval tree | MySQL for ACID, interval tree for speed. Best of both. | Memory usage for large interval trees. Tree-DB divergence. |
| Database | MySQL vs PostgreSQL (range types) | MySQL 8.0 | Team expertise per role requirement. SQL range queries are sufficient. | Missing PostgreSQL's native range/overlap operators. |
| Overbooking model | None vs Fixed ratio vs Adaptive | Guaranteed + best-effort tiers with adaptive | Predictability for critical workloads, efficiency for others. | Model may be inaccurate initially. |
| Timer queue | MySQL polling vs Redis sorted set vs Message queue delayed messages | Redis sorted set | Sub-second precision. Simple. Redis ZRANGEBYSCORE is O(log n + k). | Redis persistence risk. Mitigated by outbox pattern. |
| Recurring reservations | Cron + expand vs Dynamic evaluation at activation time | Cron + expand (30-day lookahead) | Conflict detection works on concrete instances. Avoids runtime expansion risk. | Stale expansions if template changes. |
| Compensation policy | Fixed refund vs Credit vs Reschedule | Configurable per overbooking group | Different tenants have different needs. | Complexity of multiple policies. |
| State machine implementation | Database-driven vs Event-sourced | Database-driven (MySQL status column) | Simple, debuggable. Event sourcing adds complexity without clear benefit at this scale. | Harder to replay history. |

---

## 14. Agentic AI Integration

### AI-Powered Reservation Management

**1. Intelligent Scheduling Assistant:**
Natural language reservation creation: "Reserve 64 H100 GPUs in us-east-1 for 12 hours starting tonight, preferably after 10 PM." The AI agent interprets the request, checks availability, suggests optimal time slots (cheapest, least conflict risk), and creates the reservation with user confirmation.

**2. Demand Prediction for Capacity Planning:**
An ML model (Prophet, LSTM) trained on historical reservation patterns predicts future demand by resource type, region, and time. Output: "Next Tuesday 2-6 AM, we expect 90% GPU reservation utilization in us-east-1. Consider provisioning 16 additional H100 nodes by Monday." This feeds into the capacity planning system.

**3. Conflict Resolution Advisor:**
When a reservation conflicts, the AI agent suggests creative solutions beyond simple time-shifting: "Your 64-GPU job could be split into two 32-GPU runs (with model parallelism adjusted), fitting into available slots at 2 AM and 8 AM. Estimated wall-time increase: 15%."

**4. Overbooking Model Tuning:**
An AI agent monitors overbooking metrics and automatically adjusts the model: "GPU overbooking ratio reduced from 1.15 to 1.08 for us-east-1 due to 4.2% displacement rate last week (above 3% target). Estimated utilization impact: -2%."

**5. Anomaly Detection:**
Detect unusual reservation patterns: a tenant suddenly reserving 10x their normal capacity (possible runaway automation), reservations being created and immediately cancelled (possible system bug), or reservation utilization dropping significantly (tenant may no longer need the resources).

### Guard Rails
- AI recommendations require human confirmation for create/modify/cancel actions.
- Overbooking ratio adjustments bounded by safety limits (1.0 to 1.5x).
- All AI decisions logged with full reasoning.
- Kill switch for all AI features.

---

## 15. Complete Interviewer Q&A Bank

**Q1: How do you detect reservation conflicts in O(log n) time?**
A: We use an interval tree (augmented BST). Each node stores an interval [start, end) and is augmented with the maximum end time in its subtree. To find overlaps with query interval [s, e): traverse the tree, pruning branches where the subtree's max end < s (no possible overlap). This gives O(log n + k) where k is the number of overlapping intervals.

**Q2: How do you handle concurrent reservation creation for the same resources?**
A: MySQL `SELECT FOR UPDATE` on the host_resource_timeline entries during commit serializes concurrent writers for the same host. The first transaction commits; the second retries with updated state. The in-memory interval tree is updated after commit, so it always reflects committed state.

**Q3: What's the difference between a reservation and a quota?**
A: A quota limits the *total* resources a tenant can use at any time (e.g., max 100 GPUs). A reservation guarantees *specific* resources during a *specific* time window (e.g., 64 GPUs from 2 AM to 2 PM on April 15). A tenant can have a quota of 100 GPUs and use reservations to guarantee 64 of those are available when their training job runs.

**Q4: How do you handle reservation fragmentation (small gaps between reservations)?**
A: Similar to disk fragmentation. Over time, cancellations and modifications create small gaps that are too short for new reservations. Mitigation: the alternative-slot finder considers merging adjacent gaps. Defragmentation: periodically offer to shift existing reservations slightly (with tenant consent) to consolidate gaps.

**Q5: How does the recurring reservation template handle exceptions (e.g., skip holidays)?**
A: The template supports an `exclusion_dates` list. When expanding instances, any date in the exclusion list is skipped. Also, individual instances can be cancelled without affecting the template. Future enhancement: integrate with a holiday calendar API.

**Q6: What's the maximum reservation duration you support?**
A: Up to 3 years (for reserved instances). The interval tree handles this efficiently — a 3-year reservation is just one interval. Billing emits monthly checkpoint events for long-term reservations.

**Q7: How do you handle the case where a host fails during an active reservation?**
A: The Activation Engine detects the host failure (via health monitoring). It finds a replacement host with equivalent resources, checks for conflicts, and migrates the workload (live migration for VMs, reschedule for containers). The reservation is updated with the new host assignment. If no equivalent host is available, the tenant is notified and compensation is applied.

**Q8: How do you price guaranteed vs best-effort reservations?**
A: Guaranteed: full price (e.g., $6/hr per H100 GPU). Best-effort: 70% of full price (e.g., $4.20/hr per H100 GPU). The 30% discount reflects the displacement risk. On-demand (no reservation): 120% of full price (e.g., $7.20/hr). This pricing structure encourages reservations (better capacity planning for us) while giving flexibility.

**Q9: How do you handle time zone correctness for global teams?**
A: All internal storage is UTC. API accepts any IANA timezone string. Recurring reservations evaluate cron expressions in the specified timezone (including DST handling). Display always shows the user's preferred timezone. For conflicts across timezones, comparison is always in UTC.

**Q10: How would you implement a reservation marketplace (buying/selling reserved capacity)?**
A: Two approaches: (1) **Transfer**: one tenant transfers their reservation to another (with platform taking a transaction fee). Requires atomic state transfer in MySQL. (2) **Exchange**: a marketplace where tenants list unused reservations. Other tenants can purchase them at a market-determined price. The platform manages the lifecycle transition.

**Q11: How do you handle the billing edge case where a reservation is activated late?**
A: Billing starts at the reservation's `start_time`, not the actual activation time (the capacity was reserved regardless). If activation is > 5 minutes late due to system fault, we credit the tenant for the delayed period.

**Q12: What's the impact of overbooking on the real-time scheduler?**
A: The scheduler sees reserved capacity as "unavailable" for non-reserved workloads. With overbooking, the total "reserved" capacity can exceed physical capacity. The scheduler must check: (1) the real-time resource availability (subtracting only *active* reservations), not the *committed* total. A committed-but-not-yet-active reservation doesn't consume real resources.

**Q13: How do you validate that a reservation will actually have resources when it activates (days/weeks later)?**
A: The resources are "locked" in the timeline at confirmation time. No other reservation can overlap. The only risks are: (1) host hardware failure (mitigated by automatic replacement), (2) displacement by higher-priority reservation (only for best-effort). Guaranteed reservations have a 100% SLA.

**Q14: How do you handle mass cancellations (e.g., project cancelled, 500 reservations need cancellation)?**
A: Batch cancellation API: `POST /v1/reservations/batch-cancel` with a filter (project_id). Cancellations are processed asynchronously. Timeline entries are released in bulk (single MySQL transaction per batch of 100). Events are published in batch to Kafka.

**Q15: How would you extend this system to support spot-instance-like bidding?**
A: Add a `bid_price` field to reservations. Best-effort reservations with bids are prioritized by bid price during overbooking resolution (higher bid = less likely to be displaced). When displacement is needed, the lowest-bidding reservations are displaced first. The clearing price is the lowest surviving bid.

---

## 16. References

1. Interval Tree: Cormen, T., et al. "Introduction to Algorithms." Chapter 14.3 - Interval Trees.
2. AWS EC2 Reserved Instances: https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-reserved-instances.html
3. Google Cloud Committed Use Discounts: https://cloud.google.com/compute/docs/instances/committed-use-discounts-overview
4. Azure Reservations: https://docs.microsoft.com/en-us/azure/cost-management-billing/reservations/
5. Airline Revenue Management and Overbooking: Talluri, K., van Ryzin, G. "The Theory and Practice of Revenue Management." *Springer, 2004*.
6. Redis Sorted Sets for Timer Queues: https://redis.io/docs/data-types/sorted-sets/
7. Transactional Outbox Pattern: https://microservices.io/patterns/data/transactional-outbox.html
8. PostgreSQL Range Types: https://www.postgresql.org/docs/15/rangetypes.html (for comparison)
9. Kubernetes Resource Reservations (KEP): https://github.com/kubernetes/enhancements/issues/3521
10. Saga Pattern for Distributed Transactions: https://microservices.io/patterns/data/saga.html
