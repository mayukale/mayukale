# System Design: Bare-Metal Reservation Platform

> **Relevance to role:** This is the core system a cloud infrastructure platform engineer builds and operates daily -- managing the lifecycle of physical servers from reservation through provisioning to decommission, handling tens of thousands of machines across multiple data centers with strict SLAs for availability and provisioning latency.

---

## 1. Requirement Clarifications

### Functional Requirements
- Engineers can **reserve** bare-metal machines by specifying hardware specs (GPU type, CPU cores, RAM, NVMe capacity, network bandwidth), time window (start/end), and count.
- The system **detects conflicts** and prevents double-booking of the same physical machine.
- Reservations follow a lifecycle: `requested` -> `pending_approval` -> `confirmed` -> `provisioning` -> `active` -> `draining` -> `released`.
- **Machine state machine**: `available` -> `reserved` -> `provisioning` -> `in_use` -> `draining` -> `maintenance` -> `failed` -> `decommissioned`.
- **Waitlist/queue** for when no machines matching criteria are available -- notify and auto-assign when capacity frees.
- **Preemption** for high-priority jobs: lower-priority reservations can be preempted with configurable grace periods.
- **Provisioning integration**: trigger IPMI/BMC power-on, PXE boot, OS image selection, network VLAN configuration, firmware validation.
- **Multi-tenant isolation**: VLAN segmentation, per-tenant RBAC, billing/chargeback integration.
- **Extend/shrink** existing reservations if capacity allows.
- **Bulk reservation** for large training jobs (e.g., 256 H100 GPUs in a single topology-aware allocation).
- **Reservation templates** for recurring workloads (e.g., nightly CI runs).
- **Audit trail** for every state transition with actor, timestamp, reason.

### Non-Functional Requirements
- **Availability target**: 99.99% for the reservation API (52.6 min downtime/year). Provisioning subsystem: 99.95%.
- **Consistency model**: Strong consistency for reservation state (cannot double-book). Eventual consistency acceptable for dashboard/reporting views (< 5s lag).
- **Latency target**: Reservation API p50 < 50ms, p99 < 200ms. Conflict check p99 < 20ms. Full provisioning end-to-end < 15 min.
- **Durability**: Zero tolerance for lost reservation records. RPO = 0 for reservation state.
- **Scalability**: 50,000 managed machines, 10,000 concurrent active reservations, 500 reservation requests/sec peak.

### Constraints & Assumptions
- Machines are located across 5 regions, 3 availability zones per region, ~50 racks per AZ.
- Average machine cost: $50K-$200K (GPU servers); reservation errors have direct financial impact.
- Provisioning uses existing IPMI/BMC infrastructure -- we do not redesign the BMC firmware layer.
- Network fabric is pre-configured per rack; VLAN assignment is software-controlled via SDN controller APIs.
- Authentication via corporate SSO (SAML/OIDC); authorization via centralized RBAC service.
- Billing integration is async -- we emit reservation events, billing system consumes them.
- Machine inventory is sourced from an existing CMDB (Configuration Management Database); we sync, not own, hardware discovery.
- Java (Spring Boot) for core reservation service; Python for provisioning orchestration scripts.

### Out of Scope
- VM/container orchestration (covered in iaas_platform_design.md)
- Network fabric design and BGP configuration
- Physical data center operations (racking, cabling, power)
- Cost optimization / spot-instance-style pricing (separate system)
- End-user application deployment

---

## 2. Scale & Capacity Estimates

### Users & Traffic
| Metric | Value | Calculation |
|--------|-------|-------------|
| Total managed machines | 50,000 | 5 regions x 3 AZs x ~3,333 machines/AZ |
| Active reservations | 10,000 | ~20% of fleet reserved at any time |
| Engineers/users | 5,000 | Platform serves ~5,000 infra engineers |
| API calls/sec (QPS read) | 2,000 | 5,000 users x ~0.4 req/sec avg (dashboard, status checks) |
| API calls/sec (QPS write) | 200 | ~10% of reads are mutations (reserve, extend, cancel) |
| Peak multiplier | 3x | Monday morning, end-of-sprint pushes: 600 writes/sec, 6,000 reads/sec |
| Provisioning jobs/hour | 500 | Avg reservation triggers ~1 provisioning workflow |

### Latency Requirements
| Operation | Target | Justification |
|-----------|--------|---------------|
| Create reservation (API response) | p50 < 50ms, p99 < 200ms | Conflict check + DB write; must feel instant in CLI |
| List available machines | p50 < 30ms, p99 < 100ms | Redis cache hit path; users expect instant filtering |
| Conflict check (single machine) | p99 < 5ms | In-memory interval tree lookup |
| Full provisioning pipeline | p99 < 15 min | IPMI boot + PXE + OS install + network config |
| Reservation cancel | p50 < 30ms, p99 < 100ms | DB update + async cleanup trigger |
| Waitlist notification | < 60 sec | From machine release to waitlisted user notification |

### Storage Estimates
| Data type | Size/record | Volume/day | Retention | Total |
|-----------|-------------|------------|-----------|-------|
| Reservation records | ~2 KB | 5,000 new/day | Forever (archival after 1yr) | ~3.6 GB/year active |
| Machine state records | ~1 KB | 50,000 updates/day | Forever | ~18 GB/year |
| Audit log entries | ~500 B | 200,000/day | 3 years | ~110 GB/year |
| Provisioning job logs | ~50 KB | 500/day | 1 year | ~9 GB/year |
| Availability cache (Redis) | ~200 B/machine | 50,000 machines | In-memory, no retention | ~10 MB |

### Bandwidth Estimates
| Direction | Calculation | Result |
|-----------|-------------|--------|
| Inbound API | 6,600 req/sec peak x 1 KB avg = 6.6 MB/s | ~53 Mbps |
| Outbound API | 6,600 req/sec peak x 2 KB avg = 13.2 MB/s | ~106 Mbps |
| DB replication | 600 writes/sec x 2 KB x 3 replicas = 3.6 MB/s | ~29 Mbps |
| Event bus | 600 events/sec x 1 KB = 0.6 MB/s | ~5 Mbps |
| Provisioning (IPMI/PXE) | 500 jobs/hr x 2 GB OS image / 3600 = 278 MB/s | Served from local image cache per AZ |

---

## 3. High Level Architecture

```
                                    +------------------+
                                    |  Web Portal /    |
                                    |  CLI (infra-cli) |
                                    +--------+---------+
                                             |
                                             v
                                    +------------------+
                                    |   API Gateway    |
                                    | (Kong / Envoy)   |
                                    | Rate limit, Auth |
                                    +--------+---------+
                                             |
                              +--------------+--------------+
                              |              |              |
                              v              v              v
                     +--------+--+  +--------+--+  +-------+--------+
                     | Reservation|  | Machine   |  | Provisioning   |
                     | Service    |  | Inventory |  | Orchestrator   |
                     | (Java/     |  | Service   |  | (Python)       |
                     |  Spring)   |  | (Java)    |  |                |
                     +-----+------+  +-----+-----+  +-------+-------+
                           |               |                 |
                    +------+------+        |          +------+------+
                    |             |        |          |             |
                    v             v        v          v             v
              +-----+---+  +----+----+ +--+---+ +----+----+ +-----+------+
              | MySQL    |  | Redis   | | MySQL| | Kafka   | | Provision  |
              | Primary  |  | Cluster | | (inv)| | (Events)| | Workers    |
              | (Reserv.)|  | (Avail  | |      | |         | | (per-AZ)   |
              |          |  |  Cache) | |      | |         | |            |
              +-----+----+  +---------+ +------+ +----+----+ +-----+------+
                    |                                   |            |
                    v                                   v            v
              +-----+----+                        +----+----+ +-----+------+
              | MySQL    |                        |Notif.   | | IPMI/BMC   |
              | Replicas |                        |Service  | | PXE Boot   |
              | (Read)   |                        |(Email/  | | SDN Ctrl   |
              |          |                        | Slack)  | | (Network)  |
              +----------+                        +---------+ +------------+

              +------------------------------------------------------------------+
              |                    Observability Layer                            |
              |  Prometheus + Grafana | Jaeger (Tracing) | ELK (Logs) | PagerDuty|
              +------------------------------------------------------------------+
```

### Component Roles

**API Gateway (Kong/Envoy):** TLS termination, JWT validation, rate limiting (per-tenant and global), request routing, API versioning. Deployed as a cluster with health checks -- no single point of failure.

**Reservation Service (Java/Spring Boot):** The core business logic service. Handles reservation CRUD, conflict detection via interval tree, state machine transitions, waitlist management, preemption logic. Stateless -- all state in MySQL + Redis. Deployed as 6-12 replicas behind the gateway.

**Machine Inventory Service (Java):** Source of truth for machine metadata (hardware specs, location, firmware version, health status). Syncs from CMDB. Exposes query APIs for filtering machines by capability. Feeds the Redis availability cache.

**Provisioning Orchestrator (Python):** Workflow engine that executes multi-step provisioning: IPMI power cycle -> PXE boot -> OS install -> network VLAN config -> health check -> mark active. Uses a DAG-based task engine (Temporal/Airflow-style). Idempotent steps with retry.

**MySQL Primary (Reservations):** ACID-compliant store for reservation records, state transitions, audit log. InnoDB with `SERIALIZABLE` isolation for conflict-critical transactions, `READ COMMITTED` for general reads. Primary-replica topology with semi-synchronous replication.

**Redis Cluster (Availability Cache):** In-memory view of machine availability for fast filtering. Sorted sets keyed by machine type, indexed by availability windows. Cache-aside pattern -- reads check Redis first, misses query MySQL. Invalidated on every reservation state change via Kafka consumer.

**Kafka (Event Bus):** Reservation lifecycle events published to topics: `reservation.created`, `reservation.confirmed`, `reservation.active`, `reservation.released`, `machine.state_changed`. Consumed by billing, notification, analytics, and cache invalidation services. Exactly-once semantics via idempotent producers + transactional consumers.

**Provisioning Workers (per-AZ):** Run in each availability zone close to the BMC network. Execute IPMI commands, monitor PXE boot progress, configure VLANs via SDN controller API. Report status back to orchestrator via Kafka.

### Primary Data Flow: Create Reservation

1. User submits `POST /api/v1/reservations` with machine specs, time window, count.
2. API Gateway validates JWT, checks rate limit, routes to Reservation Service.
3. Reservation Service generates idempotency key (or uses client-provided one), checks for duplicate.
4. Service queries Redis for candidate machines matching specs and availability window.
5. For each candidate, service performs **conflict check** using in-memory interval tree (loaded from MySQL on startup, kept in sync via Kafka).
6. If enough machines found, service begins MySQL transaction:
   - `SELECT ... FROM machines WHERE id IN (...) FOR UPDATE` -- pessimistic lock on candidate rows.
   - Re-verify no conflicting reservation exists (double-check after lock acquisition).
   - `INSERT INTO reservations (...)` with status = `confirmed`.
   - `UPDATE machines SET state = 'reserved' WHERE id IN (...)`.
   - `INSERT INTO audit_log (...)`.
   - Commit.
7. Publish `reservation.confirmed` event to Kafka.
8. Return reservation ID and details to user with HTTP 201.
9. Kafka consumer in Provisioning Orchestrator picks up event, starts provisioning workflow.
10. Redis cache invalidated asynchronously by cache-invalidation consumer.

### Secondary Data Flow: Waitlist

1. If step 5 finds insufficient machines, service creates reservation with status = `waitlisted`.
2. Waitlist entry stored in MySQL with priority score, desired specs, expiry.
3. When a machine is released (reservation ends or cancelled), `machine.released` event fires.
4. Waitlist consumer evaluates pending requests by priority, matches specs.
5. If match found, triggers reservation confirmation flow (steps 6-10 above).
6. User notified via Slack/email.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Reservation: the central entity
CREATE TABLE reservations (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    reservation_uuid    CHAR(36) NOT NULL,           -- External-facing UUID
    idempotency_key     VARCHAR(128) NOT NULL,        -- Client-provided for exactly-once
    tenant_id           BIGINT UNSIGNED NOT NULL,
    requester_id        BIGINT UNSIGNED NOT NULL,     -- User who created
    status              ENUM('requested','pending_approval','confirmed',
                             'provisioning','active','draining',
                             'released','cancelled','preempted','failed')
                        NOT NULL DEFAULT 'requested',
    priority            TINYINT UNSIGNED NOT NULL DEFAULT 5, -- 1=highest, 10=lowest
    machine_type        VARCHAR(64) NOT NULL,          -- e.g., 'gpu_h100_8x'
    requested_count     INT UNSIGNED NOT NULL,
    start_time          DATETIME NOT NULL,
    end_time            DATETIME NOT NULL,
    actual_start_time   DATETIME NULL,
    actual_end_time     DATETIME NULL,
    version             INT UNSIGNED NOT NULL DEFAULT 1, -- Optimistic locking
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

    UNIQUE KEY uk_idempotency (idempotency_key),
    UNIQUE KEY uk_uuid (reservation_uuid),
    INDEX idx_tenant_status (tenant_id, status),
    INDEX idx_machine_type_time (machine_type, start_time, end_time),
    INDEX idx_status_priority (status, priority),
    INDEX idx_end_time (end_time)                       -- For expiry scanning
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Junction table: which machines are assigned to which reservation
CREATE TABLE reservation_machines (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    reservation_id      BIGINT UNSIGNED NOT NULL,
    machine_id          BIGINT UNSIGNED NOT NULL,
    assigned_at         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    released_at         DATETIME NULL,

    UNIQUE KEY uk_reservation_machine (reservation_id, machine_id),
    INDEX idx_machine_time (machine_id, assigned_at, released_at),
    FOREIGN KEY (reservation_id) REFERENCES reservations(id),
    FOREIGN KEY (machine_id) REFERENCES machines(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Machine: physical server inventory
CREATE TABLE machines (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    machine_uuid        CHAR(36) NOT NULL,
    hostname            VARCHAR(255) NOT NULL,
    state               ENUM('available','reserved','provisioning','in_use',
                             'draining','maintenance','failed','decommissioned')
                        NOT NULL DEFAULT 'available',
    machine_type        VARCHAR(64) NOT NULL,          -- e.g., 'gpu_h100_8x'
    region              VARCHAR(32) NOT NULL,
    availability_zone   VARCHAR(32) NOT NULL,
    rack_id             VARCHAR(32) NOT NULL,
    rack_position       SMALLINT UNSIGNED NOT NULL,

    -- Hardware specs (denormalized for fast filtering)
    gpu_type            VARCHAR(32) NULL,              -- 'H100','A100','H200',NULL
    gpu_count           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    cpu_model           VARCHAR(64) NOT NULL,
    cpu_cores           SMALLINT UNSIGNED NOT NULL,
    ram_gb              SMALLINT UNSIGNED NOT NULL,
    nvme_tb             DECIMAL(5,2) NOT NULL DEFAULT 0,
    network_gbps        SMALLINT UNSIGNED NOT NULL,
    gpu_interconnect    VARCHAR(32) NULL,              -- 'NVLink','PCIe'

    -- BMC/IPMI details
    bmc_ip              VARCHAR(45) NOT NULL,
    bmc_mac             CHAR(17) NOT NULL,
    firmware_version    VARCHAR(32) NOT NULL,

    -- Health
    health_score        TINYINT UNSIGNED NOT NULL DEFAULT 100, -- 0-100
    last_health_check   DATETIME NULL,
    failure_count_30d   SMALLINT UNSIGNED NOT NULL DEFAULT 0,

    version             INT UNSIGNED NOT NULL DEFAULT 1,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

    UNIQUE KEY uk_uuid (machine_uuid),
    UNIQUE KEY uk_hostname (hostname),
    INDEX idx_type_state_az (machine_type, state, availability_zone),
    INDEX idx_state_health (state, health_score),
    INDEX idx_region_az_rack (region, availability_zone, rack_id),
    INDEX idx_gpu_type_state (gpu_type, state)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Reservation time slots: interval-based conflict detection helper
CREATE TABLE reservation_slots (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    machine_id          BIGINT UNSIGNED NOT NULL,
    reservation_id      BIGINT UNSIGNED NOT NULL,
    start_time          DATETIME NOT NULL,
    end_time            DATETIME NOT NULL,

    INDEX idx_machine_interval (machine_id, start_time, end_time),
    FOREIGN KEY (machine_id) REFERENCES machines(id),
    FOREIGN KEY (reservation_id) REFERENCES reservations(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Waitlist for pending requests
CREATE TABLE waitlist (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    reservation_id      BIGINT UNSIGNED NOT NULL,
    machine_type        VARCHAR(64) NOT NULL,
    requested_count     INT UNSIGNED NOT NULL,
    priority            TINYINT UNSIGNED NOT NULL,
    desired_start       DATETIME NOT NULL,
    desired_end         DATETIME NOT NULL,
    expires_at          DATETIME NOT NULL,             -- Auto-expire stale waitlist entries
    status              ENUM('waiting','matched','expired','cancelled')
                        NOT NULL DEFAULT 'waiting',
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    INDEX idx_type_priority (machine_type, priority, created_at),
    INDEX idx_expires (expires_at),
    FOREIGN KEY (reservation_id) REFERENCES reservations(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Audit log: every state transition
CREATE TABLE audit_log (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    entity_type         ENUM('reservation','machine','waitlist') NOT NULL,
    entity_id           BIGINT UNSIGNED NOT NULL,
    action              VARCHAR(64) NOT NULL,          -- e.g., 'status_changed'
    old_value           JSON NULL,
    new_value           JSON NULL,
    actor_id            BIGINT UNSIGNED NOT NULL,
    actor_type          ENUM('user','system','agent') NOT NULL,
    reason              VARCHAR(512) NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    INDEX idx_entity (entity_type, entity_id, created_at),
    INDEX idx_actor (actor_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Provisioning jobs
CREATE TABLE provisioning_jobs (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    job_uuid            CHAR(36) NOT NULL,
    reservation_id      BIGINT UNSIGNED NOT NULL,
    machine_id          BIGINT UNSIGNED NOT NULL,
    status              ENUM('pending','ipmi_power','pxe_boot','os_install',
                             'network_config','health_check','completed','failed')
                        NOT NULL DEFAULT 'pending',
    attempt             TINYINT UNSIGNED NOT NULL DEFAULT 1,
    max_attempts        TINYINT UNSIGNED NOT NULL DEFAULT 3,
    started_at          DATETIME NULL,
    completed_at        DATETIME NULL,
    error_message       TEXT NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

    UNIQUE KEY uk_uuid (job_uuid),
    INDEX idx_reservation (reservation_id),
    INDEX idx_status (status),
    FOREIGN KEY (reservation_id) REFERENCES reservations(id),
    FOREIGN KEY (machine_id) REFERENCES machines(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Tenants
CREATE TABLE tenants (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_uuid         CHAR(36) NOT NULL,
    name                VARCHAR(255) NOT NULL,
    quota_machines      INT UNSIGNED NOT NULL DEFAULT 100,
    quota_gpu_hours     INT UNSIGNED NOT NULL DEFAULT 10000,
    priority_tier       TINYINT UNSIGNED NOT NULL DEFAULT 5,
    billing_account_id  VARCHAR(128) NOT NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    UNIQUE KEY uk_uuid (tenant_uuid),
    UNIQUE KEY uk_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Option | Pros | Cons | Best For |
|--------|------|------|----------|
| **MySQL (InnoDB)** | ACID transactions, `SELECT FOR UPDATE`, mature replication (semi-sync, group replication), well-understood by ops teams, strong tooling (Vitess for sharding) | Single-writer primary limits write throughput, schema migrations on large tables are slow | Reservation state where correctness > throughput |
| **PostgreSQL** | ACID, advisory locks, SKIP LOCKED for queue patterns, better JSON support, range types for intervals | Vacuum overhead at high write rates, smaller operational knowledge base at some orgs | Similar to MySQL; advisory locks are nice but not essential |
| **Cassandra/ScyllaDB** | Linear write scalability, multi-DC replication | No transactions, no joins, eventual consistency makes conflict detection unreliable | Audit logs, time-series metrics (not reservation state) |
| **etcd** | Strong consistency (Raft), watch API for change notification | 8 GB data limit, not designed for high cardinality data, limited query capability | Scheduler leader election, small config data |

**Selected: MySQL 8.0 (InnoDB) with semi-synchronous replication** for reservation state, machine inventory, and audit logs.

**Justification:** Reservation correctness is the #1 requirement. Double-booking a $200K GPU server has real financial and operational impact. MySQL InnoDB provides:
- `SELECT ... FOR UPDATE` for pessimistic locking during reservation conflict check (exactly what we need for the critical section).
- `SERIALIZABLE` isolation available for the tightest conflict-check transactions.
- Semi-synchronous replication ensures at least one replica acknowledges before commit returns, giving RPO=0 for reservation data.
- At 600 writes/sec peak, a single MySQL primary with NVMe storage handles this comfortably (MySQL can sustain 10,000+ simple writes/sec on modern hardware).
- Vitess available if we need to shard later (shard key = `tenant_id`).

**Redis** for the availability cache (fast filtering, sorted sets for time-based queries, ~10 MB working set fits in memory trivially).

**Kafka** for the event bus (durable, exactly-once with idempotent producers, partitioned by machine_type for ordered processing per type).

**Elasticsearch** for full-text search over audit logs and reservation history (search by hostname, user, time range, action).

### Indexing Strategy

- **`idx_machine_interval (machine_id, start_time, end_time)`** on `reservation_slots`: The critical index for conflict detection. Query: `SELECT 1 FROM reservation_slots WHERE machine_id = ? AND start_time < ? AND end_time > ? LIMIT 1` -- O(log n) via B-tree.
- **`idx_type_state_az`** on `machines`: Powers the "find available machines" query. Composite index allows efficient filtering by machine type + state + AZ.
- **`idx_tenant_status`** on `reservations`: Per-tenant dashboard queries ("show me my active reservations").
- **`idx_status_priority`** on `reservations`: Waitlist processing ("find highest-priority waiting reservation for this machine type").
- **`idx_end_time`** on `reservations`: Cron-based scanning for reservations about to expire (draining trigger).
- All foreign keys indexed by default in InnoDB for join performance.

---

## 5. API Design

### REST API

```
# === Reservations ===

POST   /api/v1/reservations
  Description: Create a new reservation
  Auth: Bearer JWT (tenant-scoped)
  Headers:
    Idempotency-Key: <client-generated UUID>
  Request Body:
    {
      "machine_type": "gpu_h100_8x",
      "count": 4,
      "start_time": "2026-04-10T09:00:00Z",
      "end_time": "2026-04-10T17:00:00Z",
      "priority": 3,
      "region": "us-east-1",
      "availability_zone": "us-east-1a",          // optional
      "topology_aware": true,                      // prefer same rack
      "os_image": "ubuntu-22.04-gpu",
      "network_config": {
        "vlan_id": 2048,
        "ip_range": "10.100.0.0/24"               // optional, auto-assign if omitted
      },
      "tags": {"team": "ml-training", "project": "llm-v3"},
      "auto_provision": true
    }
  Response: 201 Created
    {
      "reservation_id": "res-a1b2c3d4-...",
      "status": "confirmed",                       // or "waitlisted"
      "machines": [
        {"machine_id": "m-xxx", "hostname": "gpu-h100-use1a-r12-s04", ...},
        ...
      ],
      "provisioning_eta": "2026-04-10T08:50:00Z",
      "waitlist_position": null                     // non-null if waitlisted
    }
  Rate Limit: 50 req/min per tenant
  Errors: 409 Conflict (insufficient capacity), 422 Validation, 429 Rate Limited

GET    /api/v1/reservations/{reservation_id}
  Description: Get reservation details
  Response: 200 OK with full reservation object including assigned machines

GET    /api/v1/reservations
  Description: List reservations for the authenticated tenant
  Query Params:
    status=active,confirmed    (comma-separated filter)
    machine_type=gpu_h100_8x
    start_after=2026-04-01
    start_before=2026-04-30
    page=1&page_size=50
    sort=start_time:desc
  Response: 200 OK with paginated list

PATCH  /api/v1/reservations/{reservation_id}
  Description: Modify reservation (extend, shrink, change priority)
  Request Body:
    {
      "end_time": "2026-04-10T21:00:00Z",          // extend by 4 hours
      "version": 3                                   // optimistic lock
    }
  Response: 200 OK with updated reservation
  Errors: 409 Conflict (version mismatch or no capacity for extension)

DELETE /api/v1/reservations/{reservation_id}
  Description: Cancel reservation
  Query Params: ?reason=no_longer_needed&force=false
  Response: 200 OK (initiates draining if active)

POST   /api/v1/reservations/{reservation_id}/extend
  Description: Extend reservation end time
  Request Body:
    {"additional_hours": 4, "version": 3}
  Response: 200 OK or 409 Conflict

# === Machines ===

GET    /api/v1/machines
  Description: List machines with filtering
  Query Params:
    state=available
    machine_type=gpu_h100_8x
    region=us-east-1
    availability_zone=us-east-1a
    gpu_type=H100
    min_gpu_count=8
    min_ram_gb=512
    available_from=2026-04-10T09:00:00Z
    available_until=2026-04-10T17:00:00Z
    page=1&page_size=100
  Response: 200 OK with paginated machine list + availability windows

GET    /api/v1/machines/{machine_id}
  Description: Get machine details including current state, reservations, health

GET    /api/v1/machines/{machine_id}/availability
  Description: Get availability windows for a specific machine
  Query Params: ?from=2026-04-10&to=2026-04-17
  Response: 200 OK with list of free/occupied time slots

POST   /api/v1/machines/{machine_id}/maintenance
  Description: Put machine in maintenance mode (admin only)
  Request Body:
    {"reason": "firmware_update", "estimated_duration_hours": 2}

# === Provisioning ===

GET    /api/v1/provisioning/jobs/{job_id}
  Description: Get provisioning job status
  Response: 200 OK with step-by-step progress

POST   /api/v1/provisioning/jobs/{job_id}/retry
  Description: Retry a failed provisioning job

# === Waitlist ===

GET    /api/v1/waitlist
  Description: List waitlisted requests for the authenticated tenant
  Response: 200 OK with position, ETA estimates

DELETE /api/v1/waitlist/{waitlist_id}
  Description: Remove from waitlist

# === Admin ===

POST   /api/v1/admin/preempt
  Description: Preempt lower-priority reservations (requires admin role)
  Request Body:
    {"reservation_id": "res-xyz", "reason": "emergency_capacity", "grace_period_minutes": 30}

GET    /api/v1/admin/capacity
  Description: Capacity overview by region/AZ/machine_type
```

### CLI Design

```bash
# Reserve machines
infra-cli reserve \
  --machine-type gpu_h100_8x \
  --count 4 \
  --start 2026-04-10T09:00 \
  --duration 8h \
  --region us-east-1 \
  --topology-aware \
  --os-image ubuntu-22.04-gpu \
  --tag team=ml-training \
  --tag project=llm-v3 \
  --output json

# Output:
# {
#   "reservation_id": "res-a1b2c3d4-...",
#   "status": "confirmed",
#   "machines": [...],
#   "provisioning_eta": "2026-04-10T08:50:00Z"
# }

# List my reservations
infra-cli reservations list --status active,confirmed --output table

# Check machine availability
infra-cli machines list \
  --type gpu_h100_8x \
  --state available \
  --region us-east-1 \
  --from 2026-04-10T09:00 \
  --to 2026-04-10T17:00

# Extend a reservation
infra-cli reserve extend --id res-a1b2c3d4 --additional-hours 4

# Cancel
infra-cli reserve cancel --id res-a1b2c3d4 --reason "job completed early"

# Check provisioning status
infra-cli provision status --reservation-id res-a1b2c3d4

# Admin: capacity overview
infra-cli admin capacity --region us-east-1 --machine-type gpu_h100_8x

# Admin: preempt
infra-cli admin preempt --reservation-id res-lowpri --reason "urgent training" --grace 30m

# Watch reservation status (streaming)
infra-cli reserve watch --id res-a1b2c3d4
```

---

## 6. Core Component Deep Dives

### Component: Conflict Detection Engine

**Why it's hard:** We must guarantee that no two reservations overlap on the same physical machine, even under high concurrency (hundreds of concurrent reservation requests). A naive approach (scan all existing reservations for each candidate machine) is O(n) per machine per request, which degrades as reservation history grows. We also need to handle concurrent requests that target the same machines -- the classic double-booking race condition.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Naive SQL scan** | `SELECT * FROM reservations WHERE machine_id=? AND start < ? AND end > ?` | Simple, correct with locks | O(n) scan per machine, slow as history grows | Very small scale (< 100 machines) |
| **In-memory interval tree** | Augmented BST where each node is an interval; overlap query in O(log n + k) | Fast overlap detection, well-studied | Memory overhead, must keep in sync with DB | Systems with many reservations per machine |
| **Sorted set in Redis** | Store reservation intervals as scored members, use ZRANGEBYSCORE | Fast, shared across service instances | Redis is not ACID, race conditions without Lua scripting | Read-heavy availability queries |
| **Database interval index (PostgreSQL range types)** | Native `tsrange` with `EXCLUDE USING gist` constraint | DB enforces non-overlap as a constraint | MySQL does not support this natively | PostgreSQL-based systems |
| **Hybrid: Redis filter + MySQL lock** | Redis for fast candidate filtering, MySQL `SELECT FOR UPDATE` for correctness | Fast path avoids DB for clearly-available machines, correct path uses ACID | Two data stores to keep in sync | **Our scale: high read, moderate write, correctness critical** |

**Selected Approach:** Hybrid -- Redis sorted sets for fast availability filtering + in-memory interval tree in the Reservation Service for O(log n) conflict checks + MySQL `SELECT FOR UPDATE` as the final correctness gate.

**Justification:** At 50,000 machines with an average of 20 active reservations per machine, scanning all reservations is 1M rows. The interval tree reduces conflict check to O(log 20) = ~4 comparisons per machine. Redis handles the "show me available machines" query (90% of traffic) without touching MySQL. MySQL pessimistic locking handles the 10% write path where correctness is non-negotiable.

**Implementation Detail:**

```java
// Interval Tree node for a single machine's reservations
public class IntervalTree {
    private TreeNode root;

    // Each node stores: [start, end, reservationId, maxEnd]
    // maxEnd = max(end, left.maxEnd, right.maxEnd) -- the augmentation
    
    public List<Reservation> findOverlapping(Instant queryStart, Instant queryEnd) {
        List<Reservation> result = new ArrayList<>();
        searchOverlap(root, queryStart, queryEnd, result);
        return result;
    }

    private void searchOverlap(TreeNode node, Instant qStart, Instant qEnd, 
                                List<Reservation> result) {
        if (node == null) return;
        
        // Overlap condition: node.start < qEnd AND node.end > qStart
        if (node.start.isBefore(qEnd) && node.end.isAfter(qStart)) {
            result.add(node.reservation);
        }
        
        // Prune: if left subtree's maxEnd <= qStart, no overlap possible in left
        if (node.left != null && node.left.maxEnd.isAfter(qStart)) {
            searchOverlap(node.left, qStart, qEnd, result);
        }
        
        // Always check right (could have overlaps with later start times)
        searchOverlap(node.right, qStart, qEnd, result);
    }
}

// Reservation Service -- the critical section
@Transactional(isolation = Isolation.SERIALIZABLE)
public ReservationResult createReservation(ReservationRequest req) {
    // Step 1: Idempotency check
    Optional<Reservation> existing = reservationRepo.findByIdempotencyKey(req.getIdempotencyKey());
    if (existing.isPresent()) return ReservationResult.of(existing.get());
    
    // Step 2: Fast path -- check interval trees (in-memory, no DB hit)
    List<Machine> candidates = machineCache.findAvailable(
        req.getMachineType(), req.getRegion(), req.getAz());
    
    List<Machine> eligible = candidates.stream()
        .filter(m -> intervalTreeMap.get(m.getId())
            .findOverlapping(req.getStartTime(), req.getEndTime()).isEmpty())
        .limit(req.getCount() * 2)  // Over-select for lock contention buffer
        .collect(toList());
    
    if (eligible.size() < req.getCount()) {
        return handleWaitlist(req);
    }
    
    // Step 3: Slow path -- acquire DB locks and re-verify
    // SELECT FOR UPDATE to prevent concurrent modifications
    List<Machine> locked = machineRepo.lockMachinesForUpdate(
        eligible.stream().map(Machine::getId).collect(toList()));
    
    // Re-verify conflicts under lock (another request may have committed between
    // our in-memory check and lock acquisition)
    List<Machine> verified = locked.stream()
        .filter(m -> reservationSlotRepo.countOverlapping(
            m.getId(), req.getStartTime(), req.getEndTime()) == 0)
        .limit(req.getCount())
        .collect(toList());
    
    if (verified.size() < req.getCount()) {
        // Release locks (transaction rollback), try waitlist
        throw new InsufficientCapacityException(req);
    }
    
    // Step 4: Commit reservation
    Reservation reservation = Reservation.builder()
        .reservationUuid(UUID.randomUUID().toString())
        .idempotencyKey(req.getIdempotencyKey())
        .tenantId(req.getTenantId())
        .status(ReservationStatus.CONFIRMED)
        .machineType(req.getMachineType())
        .startTime(req.getStartTime())
        .endTime(req.getEndTime())
        .build();
    reservationRepo.save(reservation);
    
    for (Machine m : verified) {
        reservationMachineRepo.save(new ReservationMachine(reservation.getId(), m.getId()));
        reservationSlotRepo.save(new ReservationSlot(
            m.getId(), reservation.getId(), req.getStartTime(), req.getEndTime()));
        m.setState(MachineState.RESERVED);
        machineRepo.save(m);
    }
    
    auditLogService.log(reservation, "created", null, req.getRequesterId());
    
    // Step 5: Publish event (after commit, via transactional outbox)
    outboxRepo.save(new OutboxEvent("reservation.confirmed", reservation));
    
    // Step 6: Update in-memory interval trees
    for (Machine m : verified) {
        intervalTreeMap.get(m.getId()).insert(
            req.getStartTime(), req.getEndTime(), reservation.getId());
    }
    
    return ReservationResult.of(reservation, verified);
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Interval tree out of sync with DB | False negatives (allow double-book) or false positives (reject valid reservation) | DB is always the source of truth; interval tree is a fast filter, not the authority. `SELECT FOR UPDATE` + re-verify catches any staleness. Periodic full reconciliation job runs every 5 min. |
| Redis cache stale | Users see machines as available that are actually reserved | Cache invalidation via Kafka consumer runs < 1s after reservation committed. Cache entries have 30s TTL as safety net. |
| MySQL primary down during transaction | In-flight reservations fail | Semi-sync replication ensures replica is promotable with zero data loss. Service retries with exponential backoff. Circuit breaker opens after 5 consecutive failures. |
| Lock contention on popular machine types | High p99 latency, transaction timeouts | Over-select candidates (2x requested count) so if some are locked, others are available. Lock timeout set to 5s; on timeout, retry with different candidates. |

**Interviewer Deep-Dive Q&A:**

Q: Why not just use `SELECT FOR UPDATE` without the interval tree? It is correct on its own.
A: Correctness is not the problem -- performance is. At 600 writes/sec peak, each hitting MySQL with `SELECT FOR UPDATE`, we would create significant lock contention on popular machine types. The interval tree serves as a **pessimistic filter**: it eliminates 95%+ of candidates cheaply in memory, so only truly viable candidates reach the expensive DB lock path. This reduces DB lock hold time from ~50ms (scan + lock + insert) to ~10ms (lock + insert on pre-verified rows). At our scale, that is the difference between manageable lock wait times and cascading timeouts.

Q: How do you keep the interval tree consistent across multiple Reservation Service replicas?
A: Each replica maintains its own in-memory interval tree, populated at startup from MySQL (`SELECT * FROM reservation_slots WHERE end_time > NOW()`), then kept in sync via a Kafka consumer that processes `reservation.confirmed` and `reservation.released` events. The trees may be briefly stale (by at most the Kafka consumer lag, typically < 100ms), but this is acceptable because the tree is **only a filter** -- the MySQL `SELECT FOR UPDATE` step is the authoritative correctness gate. If the tree is stale and lets through a conflicting request, the DB check catches it and the transaction rolls back.

Q: What happens if two requests for the last available H100 machine arrive simultaneously?
A: Both pass the in-memory interval tree check (the tree shows the machine as available). Both reach the MySQL `SELECT FOR UPDATE` step. MySQL serializes them: the first acquirer gets the row lock, inserts the reservation, commits, and releases the lock. The second request's `SELECT FOR UPDATE` blocks until the first commits, then re-queries and finds the conflict. Its transaction rolls back, and the service returns a 409 Conflict or routes to the waitlist. The idempotency key ensures the first request's response is stable even if the client retries.

Q: Could you use optimistic locking instead of `SELECT FOR UPDATE`?
A: Yes, and it is a valid alternative. The pattern: read the machine's `version` column, attempt `UPDATE machines SET state='reserved', version=version+1 WHERE id=? AND version=? AND state='available'`, check affected rows. If 0 rows affected, retry with a different machine. Pros: no lock waiting, better throughput under low contention. Cons: under high contention for popular machine types, optimistic retries cascade and burn CPU -- worst case is a retry storm. At our contention levels (~5-10 concurrent requests per machine type), pessimistic locking has more predictable latency. We use optimistic locking (`version` column) for the *reservation modification* path (extend, cancel) where contention is low, and pessimistic locking for the *creation* path where correctness of the conflict check is paramount.

Q: Why not use a distributed lock manager (Redis Redlock, ZooKeeper) instead of MySQL locks?
A: Redis Redlock has known correctness issues under clock skew and network partitions (see Martin Kleppmann's analysis). ZooKeeper locks are correct but add operational complexity and another failure domain. Since we already need MySQL for ACID storage of reservation state, leveraging its native row-level locking is simpler, correct, and does not introduce an additional distributed system dependency. The principle is: use the fewest distributed systems that satisfy your requirements.

Q: How does the interval tree handle machine replacement? (same rack slot, new machine ID)
A: When a machine is decommissioned and replaced, it gets a new `machine_id` in the database. The old machine's interval tree is garbage-collected (all its reservations have ended since the machine is decommissioned). A new tree is created for the new machine ID, starting empty. The hostname or rack_position is reused, but the reservation system operates on `machine_id`, not hostname, to avoid aliasing.

Q: What is the time complexity of your interval tree operations?
A: Insert: O(log n) where n is the number of active reservations for that machine. Delete: O(log n). Overlap query: O(log n + k) where k is the number of overlapping intervals. For our use case, n is ~20 (active reservations per machine at any time), so all operations are effectively O(1). The interval tree is more about algorithmic correctness (provably complete overlap detection) than performance at this scale. If n grew to thousands (e.g., highly fragmented short reservations), the O(log n) guarantee would become important.

---

### Component: Provisioning Orchestrator

**Why it's hard:** Provisioning a bare-metal machine involves a multi-step, potentially 15-minute workflow touching heterogeneous systems (IPMI/BMC, PXE servers, OS image servers, SDN controllers, health check endpoints) where any step can fail, each step has different failure semantics, and we must ensure idempotent retries without leaving machines in a half-configured ghost state.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Sequential script** | Python script that runs steps in order with try/catch | Simple to write, easy to debug | No persistence across failures, no visibility, must re-run from scratch on crash | Prototypes, < 100 machines |
| **State machine in DB** | Store current step in DB, cron retries from last checkpoint | Persistent, simple | Polling-based (latency), no built-in timeout/retry | Medium scale, simple workflows |
| **Temporal.io** | Durable workflow engine with automatic retry, timeouts, visibility | Battle-tested, durable execution, replay on failure | Operational overhead of running Temporal cluster | **Our scale: complex multi-step workflows with heterogeneous failure modes** |
| **Airflow** | DAG-based scheduler | Good for scheduled pipelines | Not designed for event-driven workflows, no durable execution | ETL pipelines, not real-time provisioning |
| **Custom saga orchestrator** | Build our own saga pattern with compensation steps | Full control | Significant engineering investment, correctness is hard to get right | Only if off-the-shelf solutions do not fit |

**Selected Approach:** Temporal.io for workflow orchestration, with Python activity workers deployed per-AZ for proximity to BMC networks.

**Justification:** Temporal provides durable execution (workflow state survives process crashes), automatic retry with configurable policies per activity, built-in timeouts, visibility UI for debugging stuck workflows, and saga-style compensation for rolling back partial provisioning. At our scale (500 provisioning jobs/hour), a 3-node Temporal cluster handles this easily. The alternative (building our own durable workflow engine) would take months and likely have correctness bugs that Temporal has already fixed.

**Implementation Detail:**

```python
# Temporal workflow definition
from temporalio import workflow, activity
from datetime import timedelta

@workflow.defn
class ProvisionMachineWorkflow:
    @workflow.run
    async def run(self, request: ProvisionRequest) -> ProvisionResult:
        # Step 1: Validate machine is in correct state
        machine = await workflow.execute_activity(
            validate_machine_state,
            args=[request.machine_id, "reserved"],
            start_to_close_timeout=timedelta(seconds=30),
            retry_policy=RetryPolicy(maximum_attempts=3)
        )
        
        # Step 2: IPMI power cycle
        try:
            await workflow.execute_activity(
                ipmi_power_cycle,
                args=[machine.bmc_ip, "on"],
                start_to_close_timeout=timedelta(minutes=2),
                heartbeat_timeout=timedelta(seconds=30),
                retry_policy=RetryPolicy(
                    maximum_attempts=3,
                    initial_interval=timedelta(seconds=10),
                    backoff_coefficient=2.0,
                    non_retryable_error_types=["BmcUnreachableError"]
                )
            )
        except Exception as e:
            await self._compensate_power(machine)
            raise

        # Step 3: PXE boot and OS installation
        try:
            await workflow.execute_activity(
                configure_pxe_boot,
                args=[machine.mac_address, request.os_image],
                start_to_close_timeout=timedelta(minutes=1),
                retry_policy=RetryPolicy(maximum_attempts=3)
            )
            
            await workflow.execute_activity(
                wait_for_os_install,
                args=[machine.machine_id],
                start_to_close_timeout=timedelta(minutes=10),
                heartbeat_timeout=timedelta(minutes=1),
                # No retry -- if OS install fails, we need to re-PXE
                retry_policy=RetryPolicy(maximum_attempts=1)
            )
        except Exception as e:
            await self._compensate_pxe(machine)
            await self._compensate_power(machine)
            raise

        # Step 4: Network VLAN configuration
        try:
            await workflow.execute_activity(
                configure_network_vlan,
                args=[machine.machine_id, request.vlan_id, request.ip_config],
                start_to_close_timeout=timedelta(minutes=2),
                retry_policy=RetryPolicy(maximum_attempts=3)
            )
        except Exception as e:
            await self._compensate_network(machine)
            await self._compensate_pxe(machine)
            await self._compensate_power(machine)
            raise

        # Step 5: Health check
        health_ok = await workflow.execute_activity(
            run_health_check,
            args=[machine.machine_id, request.expected_specs],
            start_to_close_timeout=timedelta(minutes=3),
            retry_policy=RetryPolicy(maximum_attempts=2)
        )
        
        if not health_ok:
            # Machine failed health check -- mark as failed, do not give to user
            await self._compensate_all(machine)
            await workflow.execute_activity(
                mark_machine_failed,
                args=[machine.machine_id, "health_check_failed"],
                start_to_close_timeout=timedelta(seconds=30)
            )
            raise HealthCheckFailedError(machine.machine_id)

        # Step 6: Mark active
        await workflow.execute_activity(
            update_machine_state,
            args=[machine.machine_id, "in_use"],
            start_to_close_timeout=timedelta(seconds=30)
        )
        
        await workflow.execute_activity(
            update_reservation_status,
            args=[request.reservation_id, "active"],
            start_to_close_timeout=timedelta(seconds=30)
        )
        
        return ProvisionResult(
            machine_id=machine.machine_id,
            hostname=machine.hostname,
            ip_address=machine.ip_address,
            status="active"
        )

    async def _compensate_power(self, machine):
        await workflow.execute_activity(
            ipmi_power_cycle, args=[machine.bmc_ip, "off"],
            start_to_close_timeout=timedelta(minutes=1),
            retry_policy=RetryPolicy(maximum_attempts=5)  # Try harder for cleanup
        )

    async def _compensate_pxe(self, machine):
        await workflow.execute_activity(
            clear_pxe_config, args=[machine.mac_address],
            start_to_close_timeout=timedelta(seconds=30)
        )

    async def _compensate_network(self, machine):
        await workflow.execute_activity(
            remove_vlan_config, args=[machine.machine_id],
            start_to_close_timeout=timedelta(minutes=1)
        )

    async def _compensate_all(self, machine):
        await self._compensate_network(machine)
        await self._compensate_pxe(machine)
        await self._compensate_power(machine)


# Activity implementations (run in per-AZ workers)
@activity.defn
async def ipmi_power_cycle(bmc_ip: str, action: str) -> None:
    """Send IPMI command to BMC. Idempotent: power-on when already on is a no-op."""
    client = IpmiClient(bmc_ip)
    current_state = await client.get_power_state()
    
    if action == "on" and current_state == "on":
        activity.heartbeat("already powered on")
        return
    
    await client.set_power_state(action)
    
    # Wait for state to settle
    for i in range(12):  # 2 minutes max
        await asyncio.sleep(10)
        activity.heartbeat(f"waiting for power {action}, attempt {i+1}")
        state = await client.get_power_state()
        if state == action or (action == "on" and state == "on"):
            return
    
    raise TimeoutError(f"Machine {bmc_ip} did not reach power state '{action}' in 2 minutes")
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| IPMI command fails (BMC unreachable) | Machine cannot be powered on | Retry 3x with backoff. If BMC unreachable after retries, mark machine `failed`, allocate a different machine from the over-selected pool, page on-call. |
| PXE boot hangs | OS never installs | 10-minute timeout on OS install activity. On timeout, power-cycle and retry PXE. After 3 attempts, mark machine `failed`. |
| SDN controller down | Cannot configure VLAN | Retry with backoff. SDN controller is itself HA (3-node cluster). If all retries fail, compensation rolls back prior steps, machine returns to pool. |
| Temporal worker crashes mid-activity | Activity appears stuck | Temporal detects missed heartbeat, retries activity on a different worker. Activity is idempotent, so retry is safe. |
| Health check reveals bad GPU | Machine has hardware defect | Mark machine `failed`, trigger RMA workflow, compensate all steps, allocate replacement machine. |

**Interviewer Deep-Dive Q&A:**

Q: What if the compensation (rollback) itself fails?
A: Compensation activities have aggressive retry policies (5 attempts with short intervals). If compensation still fails, we enter a "stuck" state: the machine is in an inconsistent state (partially provisioned). The workflow transitions to a `requires_manual_intervention` terminal state, an alert fires, and on-call manually resolves. We also run a periodic "zombie machine" detector that scans for machines stuck in `provisioning` state for > 30 minutes and escalates them. The key insight is that compensation failure is a lower-probability event (we are undoing a change, which is usually simpler than doing it), so accepting a manual fallback for this rare case is pragmatic.

Q: Why Temporal over a simple state machine in the DB with a cron poller?
A: Three reasons: (1) **Durability** -- Temporal persists workflow state automatically; with a cron poller, we must manually checkpoint every step and handle partial failures. (2) **Timers and heartbeats** -- Temporal has native support for "wait 10 minutes for OS install, checking every 30 seconds via heartbeat"; building this on cron requires polling intervals that trade off latency vs. DB load. (3) **Visibility** -- Temporal provides a UI showing all in-flight workflows, their current step, retry count, and history. With a cron poller, we must build all of this ourselves. The operational overhead of a 3-node Temporal cluster is minimal compared to the engineering effort of building a correct durable workflow engine.

Q: How do you handle provisioning 256 machines for a single large reservation?
A: The reservation creates 256 individual `ProvisionMachineWorkflow` instances, one per machine. Temporal handles the concurrency -- it can execute thousands of concurrent workflows. We configure per-AZ worker capacity to limit concurrent IPMI operations (e.g., max 50 concurrent per AZ) to avoid overwhelming the BMC network. The reservation transitions to `active` only when all 256 machines report healthy. If 3 out of 256 fail health checks, we attempt to substitute from the over-selected pool. If substitution is not possible, we report partial provisioning and let the user decide: accept 253 machines or cancel and re-request.

Q: How do you make individual provisioning steps idempotent?
A: Each activity is designed to be idempotent: (1) IPMI power-on: calling power-on when already powered on is a BMC no-op. (2) PXE config: we write a config file keyed by MAC address; re-writing the same file is idempotent. (3) VLAN config: SDN API calls are keyed by `(machine_id, vlan_id)` and are upserts. (4) Health check: pure read, naturally idempotent. (5) State updates: use `UPDATE ... WHERE state = 'expected_state'` to prevent duplicate transitions. The activity ID in Temporal is deterministic (derived from workflow ID + step), ensuring exactly-once delivery at the Temporal level as well.

Q: What metrics do you monitor for provisioning health?
A: (1) Provisioning success rate (target > 99%). (2) Provisioning p50/p99 duration (target < 10min p50, < 15min p99). (3) Per-step failure rate (identifies systemic issues: e.g., if PXE failure rate spikes, the PXE server may be down). (4) Compensation trigger rate (should be < 1%). (5) Stuck workflow count (should be 0). (6) Per-AZ provisioning throughput (detect AZ-specific issues). We alert if success rate drops below 95% over a 15-minute window, or if any machine is stuck in `provisioning` for > 30 minutes.

Q: How do you handle firmware version mismatches discovered during provisioning?
A: The health check step validates firmware version against the expected version in the machine inventory. If mismatched: (1) If the firmware is older but functional, we log a warning and proceed -- the machine works, just needs an update. A background job schedules firmware update for the next maintenance window. (2) If the firmware is incompatible (e.g., too old for the requested GPU driver), we fail the health check, mark the machine for maintenance, and attempt to substitute. The firmware management system (covered in machine_pool_manager.md) tracks target firmware versions per machine type and schedules rolling updates.

---

### Component: Machine State Machine

**Why it's hard:** Machine state transitions must be strictly ordered, observable, and reversible. Multiple systems (reservation service, provisioning orchestrator, health checker, maintenance scheduler, admin portal) can trigger transitions, creating race conditions. An invalid state transition (e.g., `available` -> `in_use` skipping `provisioning`) indicates a bug and must be caught.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Application-level enum check** | Validate transitions in code before DB update | Simple | Bypassed by direct DB access, inconsistent across services | Single-service systems |
| **DB triggers** | MySQL BEFORE UPDATE trigger validates transition | Cannot be bypassed | MySQL triggers are hard to test, debug, and version-control | Systems needing DB-level enforcement |
| **Event-sourced state** | Store events, derive state | Full audit trail, replay | Complexity, eventual consistency | Complex domain with frequent schema changes |
| **Explicit state machine library** | e.g., Spring Statemachine, squirrel-foundation | Validated transitions, hooks | Library coupling | **Our use case: multi-service access to machine state** |

**Selected Approach:** Explicit state machine in the Reservation Service (Spring Statemachine) backed by MySQL, with a DB-level CHECK constraint as a safety net.

```
                    +-----------+
                    | available |<---------------------------------+
                    +-----+-----+                                  |
                          |                                        |
                    reserve (by reservation service)         release (reservation
                          |                                   ends or cancelled)
                          v                                        |
                    +-----+-----+                                  |
                    | reserved  +----------------------------------+
                    +-----+-----+      (cancel before provision)
                          |
                    provision (by provisioning orchestrator)
                          |
                          v
                    +-----+-------+
                    | provisioning|-----+
                    +-----+-------+     |
                          |             | (provision fails -> mark failed)
                    provision complete   |
                          |             v
                          v        +----+---+
                    +-----+--+     | failed |----> (repair) ----> available
                    | in_use |     +--------+
                    +-----+--+
                          |
                    reservation ending (grace period)
                          |
                          v
                    +-----+-----+
                    | draining  |
                    +-----+-----+
                          |
                    drain complete / cleanup done
                          |
                          v
                    +-----+-------+
                    | maintenance |----> (maintenance done) ----> available
                    +-------------+
                          |
                    (decommission decision)
                          |
                          v
                    +-----+----------+
                    | decommissioned |  (terminal)
                    +----------------+
```

**Valid transitions enforced:**

```java
@Configuration
public class MachineStateMachineConfig 
    extends StateMachineConfigurerAdapter<MachineState, MachineEvent> {

    @Override
    public void configure(StateMachineTransitionConfigurer<MachineState, MachineEvent> transitions)
        throws Exception {
        transitions
            .withExternal().source(AVAILABLE).target(RESERVED).event(RESERVE)
            .and()
            .withExternal().source(RESERVED).target(PROVISIONING).event(PROVISION)
            .and()
            .withExternal().source(RESERVED).target(AVAILABLE).event(CANCEL)
            .and()
            .withExternal().source(PROVISIONING).target(IN_USE).event(PROVISION_COMPLETE)
            .and()
            .withExternal().source(PROVISIONING).target(FAILED).event(PROVISION_FAIL)
            .and()
            .withExternal().source(IN_USE).target(DRAINING).event(DRAIN)
            .and()
            .withExternal().source(DRAINING).target(MAINTENANCE).event(DRAIN_COMPLETE)
            .and()
            .withExternal().source(DRAINING).target(AVAILABLE).event(RELEASE)
            .and()
            .withExternal().source(MAINTENANCE).target(AVAILABLE).event(MAINTENANCE_COMPLETE)
            .and()
            .withExternal().source(MAINTENANCE).target(DECOMMISSIONED).event(DECOMMISSION)
            .and()
            .withExternal().source(FAILED).target(AVAILABLE).event(REPAIR)
            .and()
            .withExternal().source(FAILED).target(DECOMMISSIONED).event(DECOMMISSION);
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Invalid transition attempted | Service throws `InvalidStateTransitionException` | State machine library rejects it. Log, alert, investigate calling service. |
| Concurrent transitions | Race condition: two services try to transition same machine | `SELECT FOR UPDATE` + `version` column. Only one succeeds. |
| Machine stuck in transient state | `provisioning` or `draining` for too long | Watchdog job scans for machines in transient states > threshold (30 min for provisioning, 1 hr for draining), escalates or auto-retries. |

**Interviewer Deep-Dive Q&A:**

Q: How do you handle the drain -> release path vs. drain -> maintenance path?
A: When a reservation's `end_time` approaches, the system triggers the `DRAIN` event 15 minutes before expiry (configurable grace period). During draining, the provisioning orchestrator sends a SIGTERM to the user's workload, waits for graceful shutdown, then wipes the disk and resets network config. If the machine passes a quick health check after cleanup, it goes directly to `available` (the `RELEASE` event). If it needs firmware update, disk check, or other maintenance, it transitions to `maintenance`. The decision is made by the drain-completion activity based on the machine's health score and pending maintenance tasks.

Q: What prevents a machine from being stuck in `draining` forever if the user's workload does not shut down?
A: The draining workflow has a hard deadline: grace_period + 15 minutes. After that, it forcibly powers off the machine via IPMI (the nuclear option). We also set a Linux kernel watchdog timeout of grace_period + 10 minutes on the machine itself, so even if the BMC is slow, the OS will force-halt. The state machine watchdog job catches anything that slips through these mechanisms.

Q: How do you handle the `failed` state -- how does a machine get back to `available`?
A: `failed` is a parking state. A machine enters it when provisioning fails, health check fails, or the health monitoring system detects a hardware error. An on-call engineer or automated repair workflow diagnoses the issue: (1) If it is a software issue (bad OS install, misconfigured network), the repair workflow re-provisions and transitions to `available` via the `REPAIR` event. (2) If it is a hardware issue (bad DIMM, failing disk), it triggers an RMA workflow and stays in `failed` until the part is replaced. (3) If the machine is too old or too many failures, it is decommissioned. All transitions out of `failed` require either an explicit admin action or a successful automated repair with health check.

Q: Why not use event sourcing for machine state instead of a mutable `state` column?
A: Event sourcing is powerful but adds significant complexity: we would need an event store, projection rebuilding, snapshot management, and every consumer must derive current state from the event log. For machine state -- which has a small, well-defined state space (8 states, ~12 transitions) -- a mutable state column with an audit log achieves the same observability with far less complexity. The audit_log table gives us the full history of transitions (who, when, why), which is the primary benefit of event sourcing, without the operational overhead of a full CQRS/ES architecture. If the domain model were more complex (e.g., financial transactions with complex rollback semantics), event sourcing would be worth the cost.

Q: How do you handle "split-brain" scenarios where two services disagree on a machine's state?
A: MySQL is the single source of truth. The `version` column on the machines table enforces optimistic concurrency: any state update includes `WHERE version = ?`, and the update fails if the version has changed. If Service A reads state=`available` and Service B concurrently transitions it to `reserved`, Service A's subsequent `UPDATE ... WHERE version = old_version` returns 0 affected rows, and Service A knows the state changed. There is no distributed state to get out of sync -- all state is in one place (MySQL). The in-memory interval tree and Redis cache are derived views and are explicitly not authoritative.

Q: How do you test state machine correctness?
A: Three levels: (1) **Unit tests:** Every valid transition is tested, every invalid transition is tested (expected rejection). Property-based testing: generate random sequences of events and verify the state machine never reaches an invalid state. (2) **Integration tests:** End-to-end provisioning workflow against a staging environment with mock IPMI/PXE. (3) **Production monitoring:** A reconciliation job runs every 5 minutes, comparing each machine's DB state against its actual state (IPMI power state, network reachability, running workload). Discrepancies are logged and alerted. This catches bugs in the state machine logic that unit tests miss.

---

### Component: Preemption Engine

**Why it's hard:** Preemption must balance fairness (lower-priority jobs should not starve), speed (high-priority jobs need capacity quickly), and safety (preempted workloads need graceful shutdown time). The decision of *which* reservations to preempt is a variant of the weighted job scheduling problem.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Simple priority queue** | Always preempt lowest-priority reservation first | Simple, predictable | May preempt many small jobs instead of one large one, starvation of low-priority | Systems with few priority levels |
| **Weighted scoring** | Score = f(priority, remaining_time, cost, tenant_tier) | Better optimization | More complex, harder to explain to users | **Our use case: multi-factor preemption decisions** |
| **Market-based (auction)** | Reservations bid for capacity; highest bidder wins | Economically optimal | Complex UX, unpredictable costs | Public cloud spot instances |

**Selected Approach:** Weighted scoring with configurable weights, subject to constraints (minimum grace period, maximum preemptions per tenant per day).

**Implementation Detail:**

```java
public class PreemptionEngine {
    
    // Lower score = more likely to be preempted
    public double computePreemptionScore(Reservation reservation) {
        double score = 0.0;
        
        // Priority weight (40%): higher priority = higher score = less likely to be preempted
        // Priority 1 (highest) -> score 10, Priority 10 (lowest) -> score 1
        score += (11 - reservation.getPriority()) * 4.0;
        
        // Remaining time weight (25%): jobs close to completion are protected
        double fractionComplete = reservation.getFractionElapsed();
        score += fractionComplete * 25.0;
        
        // Tenant tier weight (20%): enterprise tenants protected over trial
        score += reservation.getTenant().getPriorityTier() * 2.0;
        
        // Recency weight (15%): recently started jobs get slight protection
        // (avoid churn from repeated preemption)
        long minutesSinceStart = Duration.between(
            reservation.getActualStartTime(), Instant.now()).toMinutes();
        if (minutesSinceStart < 30) {
            score += 15.0; // Just started, protected
        }
        
        return score;
    }
    
    public PreemptionPlan computePreemptionPlan(
            ReservationRequest highPriorityRequest,
            List<Reservation> activeReservations) {
        
        // Filter: only consider reservations with lower priority
        List<Reservation> candidates = activeReservations.stream()
            .filter(r -> r.getPriority() > highPriorityRequest.getPriority())
            .filter(r -> r.getMachineType().equals(highPriorityRequest.getMachineType()))
            .filter(r -> !isPreemptionProtected(r)) // Min grace period, daily limit
            .sorted(Comparator.comparingDouble(this::computePreemptionScore))
            .collect(toList());
        
        // Greedy: preempt lowest-scored reservations until we have enough machines
        List<Reservation> toPreempt = new ArrayList<>();
        int machinesFreed = 0;
        
        for (Reservation candidate : candidates) {
            if (machinesFreed >= highPriorityRequest.getCount()) break;
            toPreempt.add(candidate);
            machinesFreed += candidate.getAssignedMachineCount();
        }
        
        if (machinesFreed < highPriorityRequest.getCount()) {
            return PreemptionPlan.insufficient(machinesFreed, highPriorityRequest.getCount());
        }
        
        return PreemptionPlan.of(toPreempt, highPriorityRequest);
    }
    
    private boolean isPreemptionProtected(Reservation reservation) {
        // Cannot preempt if:
        // 1. Reservation has preemption_protected flag (explicitly set by admin)
        if (reservation.isPreemptionProtected()) return true;
        
        // 2. Reservation started less than minimum_active_time ago (default 15 min)
        if (reservation.getMinutesSinceStart() < MIN_ACTIVE_MINUTES) return true;
        
        // 3. Tenant has already been preempted more than daily limit
        int preemptionsToday = preemptionRepo.countTodayByTenant(reservation.getTenantId());
        if (preemptionsToday >= MAX_PREEMPTIONS_PER_TENANT_PER_DAY) return true;
        
        return false;
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Preempted workload ignores SIGTERM | Machine not freed within grace period | Escalation: SIGTERM -> wait grace period -> SIGKILL -> wait 60s -> IPMI force power off |
| Preemption cascade (high-priority preempts medium, which re-requests and preempts low) | Cascading disruption | Limit preemption depth: a preempted reservation cannot itself trigger preemption. It goes to the waitlist. |
| Scoring function creates unfair patterns | Certain tenants always preempted | Monitor preemption distribution per tenant. Alert if any tenant exceeds 3x average preemption rate. Tunable weights. |

**Interviewer Deep-Dive Q&A:**

Q: How do you prevent starvation of low-priority jobs?
A: Three mechanisms: (1) **Minimum active time**: A reservation that has been running for less than 15 minutes cannot be preempted, giving every job a chance to make progress. (2) **Daily preemption cap per tenant**: No tenant can be preempted more than 5 times per day (configurable). After the cap, their reservations are protected regardless of priority. (3) **Age-based scoring**: Reservations that are close to completion (> 80% elapsed) get a high protection score, since the cost of preempting them is high relative to the remaining resource usage.

Q: What if the high-priority requester's demand exceeds all preemptable capacity?
A: The preemption engine returns `PreemptionPlan.insufficient()`. The system offers three options to the requester: (1) Accept partial fulfillment (fewer machines than requested). (2) Waitlist for full capacity. (3) Escalate to admin for manual intervention (e.g., preempting protected reservations with executive approval). The API response includes the gap: "need 256, can free 180, shortfall 76."

Q: How do you communicate preemption to affected users?
A: The preemption triggers three notifications: (1) Immediate Slack/email: "Your reservation res-xyz on 4 machines will be preempted in 30 minutes. Reason: higher-priority training job. Please save your work." (2) At T-10 minutes: "Preemption in 10 minutes. SIGTERM will be sent." (3) At T-0: "SIGTERM sent. Grace period: 5 minutes." The reservation's status changes to `draining` with `preemption_reason` populated. All of this is logged in the audit trail.

Q: How do preemption decisions interact with topology-aware placement?
A: When a high-priority job requests topology-aware placement (e.g., 8 GPUs on the same rack for NVLink interconnect), the preemption engine tries to free machines from the same rack. The scoring function gets a topology bonus: preempting 8 machines from the same rack scores better than preempting 8 scattered machines, because the requester's job will perform better. If same-rack preemption is not possible, the engine falls back to any-rack preemption and the high-priority job runs without topology optimization (with a warning to the requester).

Q: Could an attacker abuse preemption by always requesting priority 1?
A: Priority is not self-assigned -- it is derived from the tenant's priority tier (set by admins/contracts) and the job's classification (set by the requesting team's manager). Priority 1-2 requires explicit admin approval or an automated classification system that validates the job type (e.g., production serving jobs get priority 1, training jobs get priority 3-5, experiments get priority 7-10). The approval workflow is mandatory for priorities 1-3.

Q: How do you test preemption logic?
A: (1) **Unit tests** with property-based testing: generate random sets of reservations and requests, verify the preemption plan always satisfies invariants (never preempts higher priority, never exceeds daily cap, always frees enough machines if possible). (2) **Simulation**: We have a discrete-event simulator that replays a month of production traffic with the preemption engine, measuring starvation rates, average job completion time by priority, and preemption fairness. We tune scoring weights using this simulator. (3) **Staged rollout**: Preemption policy changes are deployed in shadow mode first (compute preemption plans but do not execute), comparing shadow decisions against the current policy.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

**Options considered:**

| Algorithm | Description | Pros | Cons | Time Complexity |
|-----------|-------------|------|------|-----------------|
| **First-fit** | Assign first machine that matches specs and has no conflict | Fast, O(n) | Fragmentation -- leaves scattered holes in availability | O(n) |
| **Best-fit** | Assign machine with smallest available window that still fits | Minimizes wasted capacity | Slower, O(n log n) with sorting | O(n log n) |
| **Worst-fit** | Assign machine with largest available window | Keeps large blocks free for big jobs | Fragments large blocks | O(n log n) |
| **Bin packing (FFD)** | First-fit decreasing -- sort requests by size, assign largest first | Good utilization for batch scheduling | Requires batch mode, not real-time | O(n log n) |
| **Topology-aware best-fit** | Best-fit with rack/switch affinity scoring | Better network performance for multi-GPU jobs | Complex, requires topology graph | O(n log n + E) where E = topology edges |

**Selected: Topology-aware best-fit** for multi-machine reservations, **first-fit** for single-machine reservations.

**Reasoning:** Single-machine reservations are latency-sensitive (user is waiting) and first-fit is adequate. Multi-machine reservations benefit significantly from topology awareness: 8 H100 GPUs on the same rack with NVLink interconnect deliver 3-5x better collective communication performance than scattered machines connected over the spine network.

**Implementation:**

```java
public class TopologyAwarePlacementEngine {
    
    // Topology hierarchy: Region -> AZ -> Rack -> Machine
    private final TopologyGraph topology;
    private final IntervalTreeIndex reservationIndex;
    
    public PlacementResult place(ReservationRequest request) {
        if (request.getCount() == 1) {
            return firstFitSingle(request);
        }
        return topologyAwareBestFit(request);
    }
    
    private PlacementResult topologyAwareBestFit(ReservationRequest request) {
        // Step 1: Find all candidate machines matching specs
        List<Machine> candidates = machineIndex.findByTypeAndState(
            request.getMachineType(), MachineState.AVAILABLE, request.getRegion());
        
        // Step 2: Filter by availability window (interval tree check)
        List<Machine> available = candidates.stream()
            .filter(m -> reservationIndex.isAvailable(
                m.getId(), request.getStartTime(), request.getEndTime()))
            .collect(toList());
        
        // Step 3: Group by rack
        Map<String, List<Machine>> byRack = available.stream()
            .collect(Collectors.groupingBy(Machine::getRackId));
        
        // Step 4: Try same-rack placement first
        for (Map.Entry<String, List<Machine>> entry : byRack.entrySet()) {
            if (entry.getValue().size() >= request.getCount()) {
                // Full rack placement possible -- best case
                List<Machine> selected = entry.getValue()
                    .stream()
                    .sorted(Comparator.comparingInt(Machine::getHealthScore).reversed())
                    .limit(request.getCount())
                    .collect(toList());
                return PlacementResult.of(selected, PlacementQuality.SAME_RACK);
            }
        }
        
        // Step 5: Try same-AZ placement (machines across racks in same AZ)
        Map<String, List<Machine>> byAz = available.stream()
            .collect(Collectors.groupingBy(Machine::getAvailabilityZone));
        
        for (Map.Entry<String, List<Machine>> entry : byAz.entrySet()) {
            if (entry.getValue().size() >= request.getCount()) {
                // Prefer racks with most machines (pack tightly)
                List<Machine> selected = selectFromAz(entry.getValue(), request.getCount());
                return PlacementResult.of(selected, PlacementQuality.SAME_AZ);
            }
        }
        
        // Step 6: Cross-AZ placement (last resort)
        if (available.size() >= request.getCount()) {
            List<Machine> selected = available.stream()
                .sorted(Comparator.comparingInt(Machine::getHealthScore).reversed())
                .limit(request.getCount())
                .collect(toList());
            return PlacementResult.of(selected, PlacementQuality.CROSS_AZ);
        }
        
        // Step 7: Insufficient capacity
        return PlacementResult.insufficient(available.size(), request.getCount());
    }
}
```

### Conflict Detection: Interval Trees

Each machine maintains an interval tree of its reservation time slots. The conflict check for a proposed `[start, end)` interval is:

```
findOverlapping(root, start, end):
    if root is null: return empty
    results = []
    if root.start < end AND root.end > start:
        results.add(root)
    if root.left != null AND root.left.maxEnd > start:
        results += findOverlapping(root.left, start, end)
    results += findOverlapping(root.right, start, end)
    return results
```

Time complexity: O(log n + k) where n = number of reservations for that machine, k = number of overlapping results. For our use case (n ~ 20 per machine), this is effectively O(1).

The interval tree is backed by a self-balancing BST (Red-Black tree). Each node stores `[start, end, reservationId]` and an augmented `maxEnd` field (the maximum `end` value in the subtree). The `maxEnd` field enables pruning: if a subtree's `maxEnd` is before our query's `start`, no overlaps exist in that subtree.

### Concurrent Reservation Handling

**Pessimistic locking (used for reservation creation):**
```sql
BEGIN;
SELECT id, state, version FROM machines 
  WHERE id IN (?, ?, ?, ?) 
  FOR UPDATE;
-- Re-verify no conflicts in reservation_slots
-- Insert reservation, reservation_machines, reservation_slots
COMMIT;
```

Lock ordering: always acquire machine locks in ascending `machine_id` order to prevent deadlocks.

**Optimistic locking (used for reservation modification):**
```sql
UPDATE reservations 
SET end_time = ?, version = version + 1, updated_at = NOW()
WHERE id = ? AND version = ?;
-- Check affected rows: if 0, concurrent modification detected -> retry
```

### Priority Queue & Preemption

The waitlist is implemented as a priority queue backed by MySQL:

```sql
-- Fetch highest-priority waitlisted request matching freed machine type
SELECT w.* FROM waitlist w
JOIN reservations r ON w.reservation_id = r.id
WHERE w.machine_type = ?
  AND w.status = 'waiting'
  AND w.expires_at > NOW()
ORDER BY w.priority ASC, w.created_at ASC  -- Lower number = higher priority, FIFO within tier
LIMIT 1
FOR UPDATE SKIP LOCKED;  -- Non-blocking for concurrent waitlist consumers
```

`SKIP LOCKED` (MySQL 8.0+) enables multiple consumers to process waitlist entries concurrently without blocking each other.

### Starvation Prevention

1. **Aging**: Waitlisted requests that have been waiting for more than 4 hours get their effective priority boosted by 1 level per additional hour (capped at priority 2).
2. **Reservation of capacity**: 10% of each machine type is reserved for waitlisted requests -- new direct reservations cannot consume the last 10% unless the requester has priority <= 2.
3. **Fair-share scheduling**: Each tenant's active machine usage is tracked. If a tenant uses more than 2x their fair share (total machines / number of active tenants), their new reservations are deprioritized.

---

## 8. Scaling Strategy

### Horizontal Scaling

**Reservation Service:** Stateless Java/Spring Boot instances behind a load balancer. Scale from 6 to 24 replicas based on CPU utilization (target 60%) and request latency (p99 < 200ms). Each replica maintains an in-memory interval tree, which is ~10 MB per replica for 50,000 machines x 20 reservations each. No sticky sessions needed.

**Provisioning Workers:** Scaled per-AZ based on provisioning queue depth. Each worker handles ~50 concurrent provisioning activities. Auto-scale from 2 to 10 workers per AZ based on Kafka consumer lag.

**API Gateway:** Kong/Envoy cluster scales with overall request volume. Typically 4-8 instances per region.

### Database Scaling

**MySQL Topology:**
```
Primary (writes) --semi-sync--> Replica 1 (reads, same AZ)
                 --async------> Replica 2 (reads, different AZ)
                 --async------> Replica 3 (analytics, delayed)
```

- **Primary**: Handles all writes (~600/sec peak). Modern NVMe-backed MySQL handles 10K+ writes/sec, so we have 15x headroom.
- **Replica 1** (semi-sync): Ensures RPO=0. Used for read queries that need near-real-time data (reservation lookups).
- **Replica 2** (async): Cross-AZ disaster recovery. ~100ms replication lag acceptable.
- **Replica 3** (delayed by 1 hour): For analytics queries and "oops" recovery.

**Connection pooling:** HikariCP in the Java services. Pool size = 20 connections per replica instance. With 12 service replicas, that is 240 connections to primary, 240 to read replicas. MySQL `max_connections` set to 500.

**Future sharding (if needed):** Vitess with shard key = `tenant_id`. Each shard handles a subset of tenants. Cross-shard queries (admin dashboard, global capacity view) go through Vitess's scatter-gather. Not needed at current scale but the schema is designed to be shardable (tenant_id is on every major table).

### Caching

| Layer | What to cache | Strategy | Tool | Eviction | TTL | Invalidation |
|-------|---------------|----------|------|----------|-----|--------------|
| **L1: In-process** | Interval trees (per-machine reservation slots) | Preloaded at startup, updated via Kafka | ConcurrentHashMap | Remove on reservation cancel/complete | N/A (persistent) | Kafka event `reservation.released` / `reservation.confirmed` |
| **L2: Redis** | Machine availability index (sorted sets by type+AZ) | Cache-aside | Redis Cluster (6 nodes) | LRU | 30s TTL (safety net) | Kafka consumer updates on state change, < 1s lag |
| **L3: Redis** | Machine metadata (specs, location) | Cache-aside | Redis Cluster | LRU | 5 min TTL | Invalidated on CMDB sync |
| **L4: Redis** | Tenant quota counters | Write-through | Redis Cluster | N/A | N/A | Updated atomically with reservation creation |
| **L5: CDN** | API docs, portal static assets | Push on deploy | CloudFront/Akamai | Versioned URLs | 24h | Deploy invalidation |

**Cache hit rate targets:** L2 availability cache: > 95% (most queries are "show available machines" which hits Redis). L3 metadata cache: > 99% (machine specs rarely change).

### Kubernetes-Specific

The Reservation Service and Machine Inventory Service are deployed on Kubernetes:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: reservation-service
spec:
  replicas: 6
  strategy:
    type: RollingUpdate
    rollingUpdate:
      maxSurge: 2
      maxUnavailable: 0  # Zero-downtime deploy
  template:
    spec:
      containers:
      - name: reservation-service
        image: registry.internal/reservation-service:v2.3.1
        resources:
          requests:
            cpu: "2"
            memory: "4Gi"
          limits:
            cpu: "4"
            memory: "8Gi"  # Headroom for interval trees
        readinessProbe:
          httpGet:
            path: /health/ready
            port: 8080
          initialDelaySeconds: 30  # Time to load interval trees
          periodSeconds: 10
        livenessProbe:
          httpGet:
            path: /health/live
            port: 8080
          periodSeconds: 15
          failureThreshold: 3
        env:
        - name: JAVA_OPTS
          value: "-Xmx6g -Xms6g -XX:+UseG1GC -XX:MaxGCPauseMillis=100"
      topologySpreadConstraints:
      - maxSkew: 1
        topologyKey: topology.kubernetes.io/zone
        whenUnsatisfiable: DoNotSchedule
---
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: reservation-service-hpa
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: reservation-service
  minReplicas: 6
  maxReplicas: 24
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 60
  - type: Pods
    pods:
      metric:
        name: http_request_duration_seconds_p99
      target:
        type: AverageValue
        averageValue: "200m"  # 200ms p99
  behavior:
    scaleUp:
      stabilizationWindowSeconds: 60
      policies:
      - type: Pods
        value: 2
        periodSeconds: 60
    scaleDown:
      stabilizationWindowSeconds: 300  # Wait 5 min before scaling down
```

**JVM Tuning:** G1GC with 100ms max pause target. 6 GB heap for interval tree storage. `-XX:+UseStringDeduplication` to reduce memory for repeated machine type strings.

**Interviewer Deep-Dive Q&A:**

Q: At what point do you shard MySQL, and how?
A: We shard when write throughput exceeds single-primary capacity (~10K writes/sec sustained) or when the reservations table exceeds ~1 billion rows and index maintenance degrades query latency. At 5,000 new reservations/day and 3-year retention, we reach ~5.5M rows -- well within single-instance capacity. If we reach the sharding threshold, we use Vitess with `tenant_id` as the shard key. This keeps all of a tenant's reservations on the same shard, enabling transactional consistency for per-tenant operations. Cross-tenant queries (admin dashboards) use Vitess scatter-gather. Migration is online: Vitess resharding uses VReplication to copy data while the system is live.

Q: How do you handle Redis failure? What if the entire Redis cluster goes down?
A: Redis is a cache, not the source of truth. If Redis is down: (1) Availability queries fall back to MySQL read replicas. Latency increases from ~5ms to ~30ms -- acceptable. (2) Quota counters fall back to MySQL `SELECT COUNT(*)` -- slower but correct. (3) The circuit breaker in the Redis client (Resilience4j) detects failures within 5 seconds and routes all traffic to MySQL. When Redis recovers, the circuit breaker half-opens, tests with a few requests, and gradually routes traffic back. We cache-warm Redis from MySQL on recovery. The key design principle: the system degrades gracefully (slower, not broken) when any cache layer fails.

Q: How does the readiness probe interact with interval tree loading?
A: The readiness probe returns `503 Service Unavailable` until the interval tree is fully loaded from MySQL. At startup, the service queries `SELECT * FROM reservation_slots WHERE end_time > NOW()` (roughly 10K active slots per service partition), builds the interval trees, and then flips the readiness flag. This takes ~10-15 seconds for 50K machines. During this window, Kubernetes does not route traffic to the pod. The `initialDelaySeconds: 30` gives generous time for JVM warmup + tree loading. Once ready, the pod starts receiving traffic.

Q: How do you handle the thundering herd problem when many reservation requests arrive simultaneously (e.g., Monday morning)?
A: Multiple layers: (1) **API Gateway rate limiting**: Per-tenant rate limits smooth the burst. (2) **Request queuing**: If the Reservation Service's thread pool (Tomcat: 200 threads) is saturated, excess requests queue with a bounded queue (1000). Beyond that, 503 with Retry-After header. (3) **Lock contention mitigation**: Over-selecting candidate machines (2x requested count) reduces the probability that two concurrent requests contend for the same machine. (4) **HPA scales up**: The autoscaler adds pods within 60 seconds when CPU exceeds 60%. (5) **Admission control**: If queue depth exceeds threshold, lower-priority requests (priority > 7) are shed with 429 and directed to an async submission endpoint.

Q: What is the memory footprint of the interval trees across all service replicas?
A: Each reservation slot in the tree is ~48 bytes (two 8-byte timestamps + 8-byte reservation ID + 8-byte maxEnd + 16 bytes for tree pointers). With 50,000 machines x 20 active slots = 1M entries x 48 bytes = ~48 MB. Plus Java object overhead (~2x), so ~100 MB per replica. With 12 replicas, total cluster memory for interval trees is ~1.2 GB. This is well within the 6 GB heap allocation and leaves ample room for growth. If we grew to 500K machines, it would be ~1 GB per replica, still manageable within the 6 GB heap.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios & Mitigations

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| **MySQL primary fails** | All writes blocked; reads continue from replicas | MySQL orchestrator detects within 5s | Semi-sync replica auto-promoted to primary by MySQL orchestrator (Orchestrator/MHA). Application reconnects via DNS/ProxySQL. | 30s | 0 (semi-sync) |
| **Scheduler/Reservation Service crash** | Affected pod's in-flight requests fail | K8s liveness probe (15s) | K8s restarts pod. Stateless design means other replicas handle traffic. Failed in-flight requests retried by client (idempotency key ensures no duplicates). | 15s (single pod) | 0 (state in DB) |
| **Provisioning worker crashes mid-job** | In-flight provisioning activities stall | Temporal heartbeat timeout (30s) | Temporal detects missed heartbeat, retries the activity on a different worker. Activity is idempotent. Machine may need power-cycle if crashed mid-IPMI. | 30-60s | 0 (Temporal durable state) |
| **Network partition (AZ isolation)** | AZ's machines unreachable for provisioning | Health check failures spike in one AZ | Provisioning routed to other AZs. Existing in-use machines in the isolated AZ continue running (data plane independent of control plane). Reservations for isolated AZ's machines paused until partition heals. | 0 for existing, minutes for new | 0 |
| **Kafka broker unavailable** | Events not published; cache invalidation stalls | Kafka consumer lag alert, producer retry failures | Kafka is a 3-broker cluster with replication factor 3. Losing 1 broker: no impact. Losing 2: partition leader re-election. If Kafka is completely down: transactional outbox pattern -- events written to MySQL outbox table and published when Kafka recovers. | 0 (outbox) to 5m (recovery) | 0 (outbox in DB) |
| **Redis cluster down** | Availability queries slow, no cache | Redis Sentinel/Cluster health checks | Circuit breaker routes to MySQL. System operates at degraded performance, not broken. | 0 (degraded) | N/A (cache) |
| **Control plane overload (thundering herd)** | API latency spikes, timeouts | p99 latency breach, CPU saturation | HPA auto-scales. Admission control sheds low-priority requests. Rate limiting at gateway. Pre-warm capacity before known events (Monday morning). | 1-2m (scale-up) | 0 |
| **IPMI/BMC firmware bug** | Machines cannot be power-cycled | Provisioning failure rate spike for specific firmware version | Quarantine affected machines (move to maintenance state). Roll back firmware. Notify vendor. | Hours (firmware rollback) | 0 |
| **Full disk on MySQL primary** | Writes fail | Disk utilization alert at 80% | Auto-expand storage (if cloud-hosted). Emergency: `OPTIMIZE TABLE` to reclaim space, purge old audit logs. | Minutes | 0 (no data lost, writes just blocked) |

### Automated Recovery

**Machine health monitoring loop (runs every 60 seconds per machine):**

```python
# Health checker -- runs as a Kubernetes CronJob per AZ
async def health_check_loop():
    machines = await get_machines_in_az(state=["in_use", "available", "reserved"])
    
    for machine in machines:
        health = await check_machine_health(machine)
        # health includes: ping reachable, BMC responsive, GPU temp, disk health, RAM ECC errors
        
        old_score = machine.health_score
        new_score = compute_health_score(health)
        
        if new_score < CRITICAL_THRESHOLD:  # e.g., 30
            # Machine is in trouble
            if machine.state == "in_use":
                # Alert user, start graceful drain
                await notify_tenant(machine, "health_degraded", health)
                await trigger_drain(machine, grace_period_minutes=15)
            elif machine.state in ("available", "reserved"):
                # Pull from pool immediately
                await transition_state(machine, "failed", reason=health.summary)
                await trigger_replacement_search(machine)
        
        elif new_score < WARNING_THRESHOLD:  # e.g., 60
            # Degraded but functional -- schedule maintenance
            await schedule_maintenance(machine, priority="normal")
            await update_health_score(machine, new_score)
        
        else:
            await update_health_score(machine, new_score)
```

**Reservation expiry handler (runs every minute):**

```python
# Find reservations that should be draining but are not yet
async def expiry_watchdog():
    expiring = await db.query("""
        SELECT * FROM reservations 
        WHERE status = 'active' 
        AND end_time <= NOW() + INTERVAL 15 MINUTE
        AND end_time > NOW()
    """)
    
    for reservation in expiring:
        if reservation.status != 'draining':
            await transition_reservation(reservation, 'draining')
            await notify_tenant(reservation, "reservation_expiring_soon")
    
    # Find reservations past end_time that are still active (should never happen)
    overdue = await db.query("""
        SELECT * FROM reservations 
        WHERE status IN ('active', 'draining') 
        AND end_time < NOW() - INTERVAL 5 MINUTE
    """)
    
    for reservation in overdue:
        await force_release(reservation, reason="overdue_by_5_min")
        await alert_oncall("overdue_reservation", reservation)
```

### Retry Strategy

All external calls use exponential backoff with jitter:

```java
RetryPolicy retryPolicy = RetryPolicy.builder()
    .maxAttempts(5)
    .initialInterval(Duration.ofMillis(100))
    .backoffCoefficient(2.0)
    .maximumInterval(Duration.ofSeconds(30))
    .jitter(0.2)  // +/- 20% randomization to prevent thundering herd
    .retryableExceptions(TransientException.class, TimeoutException.class)
    .nonRetryableExceptions(
        AuthenticationException.class,
        ValidationException.class,
        NotFoundException.class)
    .build();

// Retry schedule: 100ms, 200ms, 400ms, 800ms, 1600ms (with jitter)
// After 5 attempts, circuit breaker opens
```

### Circuit Breaker

```java
CircuitBreakerConfig config = CircuitBreakerConfig.custom()
    .failureRateThreshold(50)           // Open after 50% failure rate
    .slowCallRateThreshold(80)          // Open if 80% of calls are slow
    .slowCallDurationThreshold(Duration.ofSeconds(2))
    .waitDurationInOpenState(Duration.ofSeconds(30))  // Wait before half-open
    .permittedNumberOfCallsInHalfOpenState(5)
    .slidingWindowType(SlidingWindowType.TIME_BASED)
    .slidingWindowSize(10)               // 10-second window
    .build();

// Applied to: MySQL connections, Redis connections, IPMI calls, SDN API calls
// Each external dependency has its own circuit breaker instance
```

### Consensus & Coordination

**Leader election** for singleton jobs (expiry watchdog, waitlist processor, health reconciler):

We use **etcd** with lease-based leader election. Only the leader instance runs the singleton job. If the leader loses its lease (process crash, network partition), another instance acquires the lease within the TTL (10 seconds).

```java
// Using jetcd (Java etcd client)
LeaseClient leaseClient = etcdClient.getLeaseClient();
long leaseId = leaseClient.grant(10).get().getID();  // 10-second TTL

// Campaign for leadership
electionClient.campaign(
    ByteSequence.from("/reservation-service/leader", UTF_8),
    leaseId,
    ByteSequence.from(instanceId, UTF_8)
).get();

// Keep-alive to maintain lease
leaseClient.keepAlive(leaseId, observer);

// On leadership acquired:
startSingletonJobs();

// On leadership lost (keep-alive fails):
stopSingletonJobs();
```

Why etcd over ZooKeeper: etcd has a simpler operational model (single binary, no JVM tuning), gRPC API, and is the standard for Kubernetes-native environments. Our Kubernetes cluster already runs etcd, so we can use a dedicated etcd namespace rather than deploying a separate ZK cluster.

---

## 10. Observability

### Key Metrics

| Metric | Type | Tool | Alert Threshold | Business Impact |
|--------|------|------|-----------------|-----------------|
| `reservation.create.latency` | Histogram | Prometheus | p99 > 500ms | User experience; CLI feels slow |
| `reservation.create.success_rate` | Counter (success/total) | Prometheus | < 95% over 5 min | Users cannot reserve machines |
| `reservation.conflict.rate` | Counter | Prometheus | > 20% of attempts | Capacity shortage; need to add machines or adjust quotas |
| `machine.state.gauge` | Gauge (per state) | Prometheus | `failed` > 5% of fleet | Hardware reliability issue |
| `provisioning.duration` | Histogram | Prometheus | p99 > 20 min | Slow provisioning degrades user experience |
| `provisioning.failure_rate` | Counter | Prometheus | > 5% over 15 min | Systemic provisioning issue (PXE server down?) |
| `waitlist.queue_depth` | Gauge (per machine_type) | Prometheus | > 50 for any type | Capacity shortage for that machine type |
| `waitlist.wait_time` | Histogram | Prometheus | p50 > 4 hours | Users waiting too long; consider capacity expansion |
| `mysql.replication_lag` | Gauge | Prometheus | > 5 sec | Read replicas serving stale data |
| `mysql.connection_pool.active` | Gauge | Prometheus | > 80% of pool size | Connection exhaustion imminent |
| `redis.hit_rate` | Counter (hit/miss) | Prometheus | < 90% | Cache ineffective; check invalidation logic |
| `kafka.consumer_lag` | Gauge (per consumer group) | Prometheus | > 10,000 messages | Cache invalidation delayed; potential stale data |
| `preemption.count` | Counter (per tenant) | Prometheus | > 5/day for any tenant | Unfair preemption; review scoring weights |
| `machine.health_score.avg` | Gauge (per machine_type) | Prometheus | < 70 | Fleet degradation; review hardware health |

### Distributed Tracing

**Jaeger** with OpenTelemetry instrumentation:

Every reservation request generates a trace spanning:
1. API Gateway (entry span)
2. Reservation Service (business logic span)
   - Interval tree check (child span)
   - MySQL transaction (child span, includes lock wait time)
   - Kafka publish (child span)
3. Cache invalidation consumer (linked span)
4. Provisioning orchestrator (linked span)
   - IPMI command (child span)
   - PXE boot (child span)
   - Health check (child span)

Trace context propagated via `traceparent` header (W3C Trace Context). Sampling: 100% for errors, 10% for successful requests, 100% for requests > p99 latency (tail sampling via OpenTelemetry Collector).

### Logging

**Structured JSON logging** with correlation:

```json
{
  "timestamp": "2026-04-10T09:15:23.456Z",
  "level": "INFO",
  "service": "reservation-service",
  "instance": "reservation-service-7b8c9-abc12",
  "trace_id": "a1b2c3d4e5f6...",
  "span_id": "1234abcd",
  "tenant_id": "tenant-42",
  "user_id": "user-123",
  "reservation_id": "res-a1b2c3d4",
  "action": "reservation_created",
  "machine_count": 4,
  "machine_type": "gpu_h100_8x",
  "duration_ms": 47,
  "message": "Reservation confirmed: 4 x gpu_h100_8x in us-east-1a"
}
```

**Aggregation pipeline:** Application -> Filebeat -> Kafka (log topic) -> Logstash -> Elasticsearch -> Kibana.

**Elasticsearch index strategy:** Daily indices with ILM (Index Lifecycle Management). Hot: 7 days (SSD). Warm: 30 days (HDD). Cold: 90 days (frozen). Delete: > 1 year (audit logs exempted -- retained 3 years for compliance).

### Alerting

**PagerDuty** integration with escalation policies:

| Severity | Condition | Response Time | Example |
|----------|-----------|---------------|---------|
| P1 (Critical) | System-wide outage, all reservations failing | 5 min | MySQL primary down, all service replicas crashing |
| P2 (High) | Significant degradation | 15 min | Provisioning failure rate > 20%, Redis cluster down |
| P3 (Medium) | Partial degradation | 1 hour | Single AZ provisioning slow, high waitlist depth |
| P4 (Low) | Informational | Next business day | Health score trending down, capacity approaching limits |

**Alert deduplication:** Group related alerts (e.g., "MySQL primary down" + "reservation write failures" + "Redis invalidation lag") into a single incident via PagerDuty's event intelligence.

---

## 11. Security

### Authentication & Authorization

**Service-to-service:** mTLS enforced via Istio service mesh. Every service has a SPIFFE identity (`spiffe://cluster.local/ns/infra/sa/reservation-service`). No plaintext traffic within the mesh.

**User-to-API:** OIDC/SAML via corporate SSO (Okta/Azure AD). JWT access tokens with 15-minute expiry, refresh tokens with 8-hour expiry. JWT claims include `tenant_id`, `roles`, `groups`.

**RBAC model:**

| Role | Permissions |
|------|------------|
| `viewer` | Read reservations, list machines, view capacity |
| `engineer` | Create/modify/cancel own reservations |
| `team-lead` | Create reservations for team, approve expensive requests |
| `admin` | Preempt, maintenance mode, modify any reservation, capacity management |
| `system` | Service accounts for provisioning, health checks, billing |

**Authorization enforcement:** API Gateway validates JWT and extracts roles. Reservation Service checks `tenant_id` from JWT matches the request's tenant. Admin endpoints require `admin` role. Implemented via Spring Security with custom `@PreAuthorize` annotations.

### Secrets Management

- **HashiCorp Vault** for all secrets: database credentials, BMC passwords, API keys.
- Dynamic database credentials: Vault generates short-lived MySQL credentials (TTL: 1 hour) per service instance. No static passwords in config files.
- BMC/IPMI credentials stored in Vault's KV store, rotated quarterly.
- Kubernetes integration: Vault Agent sidecar injects secrets as mounted files, not environment variables (avoids exposure in process listings).

### Network Security

- **VLANs per tenant:** Each tenant's reserved machines are on an isolated VLAN. Cross-tenant traffic is blocked at the ToR switch level.
- **BMC network:** Physically or logically separated from tenant data plane. Only provisioning workers (in the management VLAN) can reach BMC IPs. No tenant can access another machine's BMC.
- **API Gateway:** WAF rules for SQL injection, XSS. Rate limiting per tenant. DDoS protection via cloud provider.
- **Network policies (Kubernetes):** Reservation Service can only reach MySQL, Redis, Kafka, etcd. Provisioning workers can only reach Kafka and the BMC network. Principle of least privilege.

### Audit Logging

Every mutation is logged to the `audit_log` table (source-of-truth) and forwarded to Elasticsearch (searchable). Audit logs include:
- Who (user ID, service account, or AI agent)
- What (action, old value, new value)
- When (timestamp, millisecond precision)
- Why (reason field, mandatory for admin actions)
- Where (source IP, service instance)

Audit logs are immutable (append-only table, no DELETE or UPDATE permissions granted). Retained for 3 years per compliance requirements.

---

## 12. Incremental Rollout Strategy

### Phased Rollout

**Phase 1: Shadow mode (1 week)**
- New version deployed alongside old version.
- All traffic goes to old version.
- New version receives a copy of traffic (mirrored via Istio), processes requests, but responses are discarded.
- Compare: new version's decisions vs. old version's decisions. Log any discrepancies.
- Validates: no regressions in conflict detection, no new errors, latency comparable.

**Phase 2: Canary (2 weeks)**
- 1% of traffic -> new version. Monitor for 48 hours.
- 5% of traffic -> new version. Monitor for 48 hours.
- 25% of traffic -> new version. Monitor for 48 hours.
- 50% of traffic -> new version. Monitor for 72 hours.
- 100% of traffic -> new version.

Traffic splitting via Istio VirtualService:
```yaml
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
metadata:
  name: reservation-service
spec:
  hosts:
  - reservation-service
  http:
  - route:
    - destination:
        host: reservation-service
        subset: stable
      weight: 95
    - destination:
        host: reservation-service
        subset: canary
      weight: 5
```

**Automated rollback triggers:**
- Error rate (5xx) > 1% (baseline: 0.05%)
- p99 latency > 500ms (baseline: 150ms)
- Reservation conflict rate anomaly (> 2 standard deviations from 7-day average)
- Any data integrity alert (e.g., double-booking detected by reconciliation)

Rollback is automatic: Argo Rollouts (or Flagger) detects the breach, shifts 100% traffic back to stable, and pages on-call.

**Phase 3: Database migration (if schema changes)**
- Use `pt-online-schema-change` (Percona) or `gh-ost` (GitHub) for online ALTER TABLE.
- No downtime: creates shadow table, copies data, swaps atomically.
- Rollback: keep the old columns for 2 weeks after migration, drop only after confirming no issues.
- Application code handles both old and new schema during the migration window (versioned readers).

**Feature flags (LaunchDarkly / internal):**
- New preemption scoring algorithm: behind flag, enabled per-tenant for testing.
- New provisioning workflow steps: behind flag, enabled per-AZ.
- Flags evaluated at runtime, no redeploy needed.

### Rollout Q&A

Q: How do you roll out a change to the conflict detection algorithm affecting 50,000 machines without downtime?
A: Shadow mode first: the new algorithm runs in parallel with the old one on 100% of traffic. For every reservation request, both algorithms compute their result, but only the old algorithm's result is used. We log discrepancies and analyze them. If the new algorithm would have rejected a valid reservation (false positive) or approved a conflicting one (false negative), we investigate and fix. After 1 week of zero discrepancies, we canary to 1% of traffic (by tenant hash), monitor for 48 hours, then ramp. The interval tree data structure change is backward-compatible: the new code can read old tree format, and vice versa.

Q: How do you handle a database schema migration that adds a NOT NULL column to the reservations table (500M rows)?
A: We use `gh-ost` (GitHub Online Schema Change). It creates a ghost table with the new schema, uses binlog streaming to copy rows and track changes, then atomically renames the tables. The migration runs at a controlled rate (e.g., 500 rows/sec) to avoid overloading the primary. During migration (which may take hours for 500M rows), both old and new columns are accessible. The application code adds the new column to INSERT statements before the migration completes (the column has a DEFAULT value). After the table swap, the code starts reading the new column. Rollback: `gh-ost` supports cut-over postponement, so if we detect issues, we can abort before the swap.

Q: What if a canary deployment introduces a subtle bug that only manifests under high load (not caught at 5% traffic)?
A: We have two safeguards: (1) **Load testing in staging**: Before any production rollout, the new version is load-tested in a staging environment at 2x production peak traffic. (2) **Extended bake time at 25%**: We hold at 25% for 48 hours, which includes at least one peak period (Monday morning). The automated rollback triggers catch latency and error rate regressions. For subtle correctness bugs (e.g., off-by-one in time zone handling), the **reconciliation job** runs every 5 minutes comparing DB state against expected state. Any discrepancy triggers an alert and automatic rollback.

Q: How do you roll out a new provisioning step (e.g., GPU firmware validation) without disrupting in-flight provisioning jobs?
A: Temporal workflows are versioned. The new step is added to a new workflow version. In-flight workflows continue executing the old version (Temporal replays use the original workflow definition). Only new provisioning jobs use the new version with the GPU firmware validation step. We maintain the old workflow version for at least 24 hours (max provisioning time is 30 minutes, but we are generous). Feature flag controls whether new reservations trigger the new or old workflow version.

Q: How do you coordinate rollout across multiple dependent services (e.g., Reservation Service v2 requires Machine Inventory Service v2)?
A: We use API versioning. Machine Inventory Service v2 is deployed first, supporting both v1 and v2 API endpoints. Once v2 is stable and all consumers are verified, Reservation Service v2 is deployed to call the v2 endpoint. During the transition, Reservation Service v1 continues calling the v1 endpoint. After Reservation Service v2 is fully rolled out, we deprecate (but do not remove) Machine Inventory v1 endpoint for 2 weeks, then remove it. This avoids a coordinated deploy ("big bang") and allows independent rollback of each service.

---

## 13. Trade-offs & Decision Log

| Decision | Option A | Option B | Option C | Chosen | Specific Reason |
|----------|----------|----------|----------|--------|-----------------|
| Conflict detection | Pure DB (SELECT FOR UPDATE) | Pure in-memory interval tree | Hybrid (interval tree + DB lock) | **Hybrid** | Interval tree handles 90% of cases in O(log n) without DB hit; DB lock is the correctness gate for the 10% write path. Pure DB is too slow at scale; pure in-memory is not ACID. |
| Consistency for reservations | Eventual (faster, simpler) | Strong (ACID transactions) | N/A | **Strong** | Double-booking a $200K GPU server is unacceptable. The latency cost of ACID (50ms vs. 10ms) is negligible for this use case. |
| Provisioning orchestration | Custom state machine | Temporal.io | Airflow | **Temporal** | Durable execution, automatic retry, saga compensation, visibility UI. Custom is months of engineering with correctness bugs. Airflow is for batch ETL, not event-driven provisioning. |
| Message broker | RabbitMQ | Kafka | Redis Streams | **Kafka** | Durable, exactly-once, partitioned, high throughput. RabbitMQ's queue model does not support fan-out well. Redis Streams lack durability guarantees we need for reservation events. |
| Cache | Memcached | Redis | Hazelcast (in-process) | **Redis** | Sorted sets for time-range queries (availability windows). Memcached is key-value only. Hazelcast adds JVM heap pressure and consistency complexity. |
| Leader election | ZooKeeper | etcd | MySQL GET_LOCK() | **etcd** | Already in our Kubernetes stack, simpler ops than ZK, gRPC API. MySQL GET_LOCK is adequate but has session-lifetime semantics that make failover tricky. |
| API style | REST | gRPC | GraphQL | **REST** | CLI and web portal are the primary consumers. REST is simpler for these clients. gRPC would add complexity without benefit (no streaming needed for this API). Internal service-to-service: gRPC for performance. |
| Locking strategy (creation) | Optimistic (version column) | Pessimistic (SELECT FOR UPDATE) | Distributed lock (Redis) | **Pessimistic** | Under contention for popular machine types, optimistic retries cascade. Pessimistic locking has predictable latency. Redis distributed locks have correctness issues. |
| Locking strategy (modification) | Optimistic | Pessimistic | N/A | **Optimistic** | Modifications (extend, cancel) have low contention. Optimistic avoids lock hold time for the common non-contended case. |
| Sharding strategy | Shard by machine_id | Shard by tenant_id | No sharding | **No sharding (for now)** | Current scale (600 writes/sec) is well within single-primary MySQL capacity. Premature sharding adds operational complexity. Schema designed to be shardable by tenant_id when needed. |

---

## 14. Agentic AI Integration

### Where AI Adds Value

1. **Intelligent capacity forecasting**: An LLM agent analyzes historical reservation patterns, team schedules, and project timelines to predict capacity demand 1-4 weeks ahead. This enables proactive machine procurement and pool sizing.

2. **Automated incident remediation**: When a machine fails health checks, an AI agent diagnoses the root cause by analyzing logs, metrics, and hardware telemetry, then executes the appropriate remediation (restart service, power-cycle, schedule RMA) without human intervention for known failure patterns.

3. **Reservation optimization assistant**: An AI agent suggests optimal reservation parameters (machine type, count, time window) based on the user's workload description and historical performance data. "You are running distributed training on 8 H100s. Based on similar jobs, 6 H100s with NVLink topology on rack R12 would give you 15% better throughput."

4. **Anomaly detection in fleet health**: AI agent monitors fleet-wide health metrics and detects anomalies that rules-based alerts miss (e.g., subtle GPU memory degradation trend across a specific hardware batch).

### LLM Agent Loop

```
OBSERVE: Collect current system state
  - Machine health metrics (Prometheus)
  - Reservation patterns (MySQL)
  - Provisioning failure logs (ELK)
  - Capacity utilization (real-time dashboard data)

REASON: Analyze and decide
  - "GPU memory ECC error rate on machines m-1234 through m-1240 is 3x 
     above baseline. These are all from Batch B2024-Q3, NVIDIA H100 SXM5. 
     Likely a hardware batch issue."
  - "Recommendation: Move these 7 machines to maintenance, contact NVIDIA 
     for batch-level RMA, reallocate affected reservations."

ACT: Execute via tool calls
  - tool: transition_machine_state(machine_ids=[...], target_state="maintenance", reason="...")
  - tool: notify_affected_tenants(machine_ids=[...], message="...")
  - tool: create_rma_ticket(machine_ids=[...], vendor="nvidia", reason="...")
  - tool: search_replacement_capacity(machine_type="gpu_h100_8x", count=7, region="us-east-1")

VERIFY: Confirm actions succeeded
  - Check machine states transitioned correctly
  - Confirm tenant notifications sent
  - Verify RMA ticket created
  - Confirm replacement machines allocated
```

### Tool Calls the Agent Can Make

```python
@tool("transition_machine_state")
def transition_machine_state(machine_ids: list[str], target_state: str, reason: str) -> dict:
    """Move machines to a new state. Validates state transition is legal."""

@tool("search_available_capacity")
def search_available_capacity(machine_type: str, count: int, region: str, 
                                time_window: dict) -> list[dict]:
    """Find available machines matching criteria."""

@tool("create_reservation")
def create_reservation(machine_type: str, count: int, tenant_id: str, 
                        start: str, end: str) -> dict:
    """Create a reservation (requires approval for count > 10)."""

@tool("query_metrics")
def query_metrics(query: str, start: str, end: str) -> list[dict]:
    """Execute a PromQL query and return time series data."""

@tool("search_logs")
def search_logs(query: str, index: str, time_range: str) -> list[dict]:
    """Search Elasticsearch for log entries."""

@tool("create_rma_ticket")
def create_rma_ticket(machine_ids: list[str], vendor: str, reason: str) -> dict:
    """Create an RMA ticket with the hardware vendor."""

@tool("notify_users")
def notify_users(user_ids: list[str], message: str, channel: str = "slack") -> dict:
    """Send notification to users via Slack or email."""

@tool("run_health_check")
def run_health_check(machine_id: str) -> dict:
    """Run comprehensive health check on a machine."""
```

### Guard Rails and Human-in-the-Loop

| Action | Risk Level | Guard Rail |
|--------|-----------|------------|
| Read-only queries (metrics, logs, capacity) | Low | No restriction; agent can freely query |
| Single machine state transition (to maintenance/failed) | Medium | Allowed if health_score < 30. Otherwise, requires human approval. |
| Bulk machine state transitions (> 5 machines) | High | Always requires human approval via Slack confirmation |
| Preemption of active reservations | High | Always requires human approval |
| Create/modify reservations on behalf of users | Medium | Allowed within tenant's existing quota. Above quota: human approval. |
| RMA ticket creation | Medium | Auto-approved for single machines. Bulk (> 3): human approval. |
| Firmware rollback | High | Always dry-run first, show plan, require human approval. |

### Reliability: Dry-Run Mode

```python
class AgentExecutor:
    def __init__(self, dry_run: bool = True):
        self.dry_run = dry_run
        self.action_log = []
    
    async def execute_action(self, tool_name: str, params: dict) -> dict:
        # Always log the proposed action
        action = {
            "tool": tool_name,
            "params": params,
            "timestamp": datetime.utcnow().isoformat(),
            "dry_run": self.dry_run
        }
        self.action_log.append(action)
        
        if self.dry_run:
            # Simulate the action, return expected result
            return {"status": "dry_run", "would_execute": action}
        
        # Check guard rails before execution
        risk = assess_risk(tool_name, params)
        if risk == "high":
            approval = await request_human_approval(action)
            if not approval.approved:
                return {"status": "blocked", "reason": "human_rejected"}
        
        # Execute and log result
        result = await tool_registry.execute(tool_name, params)
        action["result"] = result
        
        # Publish to audit trail
        await audit_log.publish("agent_action", action)
        
        return result
```

**Rollback capability:** Every agent action is recorded with enough context to reverse it. If the agent transitions a machine to `maintenance`, the rollback is `transition_machine_state(machine_id, "available")`. The action log serves as both an audit trail and a rollback plan.

---

## 15. Complete Interviewer Q&A Bank

**Q: How do you prevent two engineers from reserving the same bare-metal machine simultaneously?**
A: Three-layer defense: (1) **Fast filter**: In-memory interval tree per machine detects overlapping time windows in O(log n). If the tree says no overlap, we proceed to the DB. (2) **DB correctness gate**: `SELECT ... FROM machines WHERE id IN (...) FOR UPDATE` acquires row-level exclusive locks on the candidate machines. This serializes concurrent requests targeting the same machines. (3) **Re-verification under lock**: After acquiring locks, we re-check the `reservation_slots` table for conflicts. If a conflict exists (inserted by a concurrent transaction that committed between our interval tree check and our lock acquisition), we rollback and return 409 Conflict. The idempotency key ensures that retried requests return the same result without creating duplicates.

**Q: What happens if your MySQL primary goes down mid-transaction during a reservation creation?**
A: The in-flight transaction is lost (not committed). The client receives a timeout or connection error. The client retries with the same idempotency key. Since the original transaction did not commit, there is no reservation record for that idempotency key. The retry creates the reservation from scratch on the new primary (promoted from semi-sync replica within ~30 seconds by MySQL Orchestrator). The user experiences a ~30-second delay, not data loss. Semi-synchronous replication ensures the promoted replica has all committed transactions (RPO = 0).

**Q: How do you handle a machine that fails during an active reservation?**
A: The health monitoring system detects the failure (ping timeout, BMC reports error, GPU ECC error threshold exceeded). (1) Machine state transitions to `failed`. (2) Affected tenant is notified immediately via Slack with a description of the failure. (3) The system searches for a replacement machine matching the same specs and availability window. (4) If found, a replacement reservation is created automatically and provisioning starts. The new machine is offered to the tenant. (5) If no replacement is available, the tenant is notified of the shortfall and given options: wait for a replacement (waitlist), reduce their reservation size, or cancel. (6) The failed machine enters the RMA workflow. (7) The entire incident is recorded in the audit log and a post-incident report is generated for hardware reliability tracking.

**Q: How do you scale conflict detection if you grow to 1 million machines?**
A: At 1M machines, each with ~20 active reservations, the interval trees consume ~2 GB per service replica (48 bytes x 20M entries x 2 Java overhead). This is still manageable with 8 GB heaps. However, startup time (loading 20M slots from MySQL) would increase to ~2 minutes, which slows deployments. Mitigation: (1) **Partition interval trees by region/AZ**: Each service replica only loads trees for its assigned partition. With 15 AZs, each replica handles ~67K machines = ~130 MB of trees. (2) **Shard MySQL by tenant_id**: Vitess distributes the load. (3) **Redis as primary availability index**: Move the availability query (the 90% read path) entirely to Redis sorted sets, which scale horizontally. The interval tree becomes a secondary check for the 10% write path.

**Q: How do you handle time zones and DST for reservation start/end times?**
A: All times are stored and processed in **UTC**. The API accepts ISO 8601 timestamps with explicit timezone offsets (e.g., `2026-04-10T09:00:00-04:00`). The Reservation Service converts to UTC on ingestion and stores only UTC. The CLI and web portal display times in the user's local timezone (derived from their profile settings). DST transitions are handled by the timezone library (Java `java.time.ZonedDateTime`), never by manual offset arithmetic. This eliminates the "1:30 AM during fall-back" ambiguity because we never store local times.

**Q: What is your strategy for handling quota enforcement?**
A: Quotas are enforced at two levels: (1) **Pre-check (fast)**: Before conflict detection, the Reservation Service checks the tenant's current usage against their quota (stored in Redis as an atomic counter: `INCRBY tenant:{id}:machines {count}`). If the reservation would exceed the quota, reject immediately with 403. (2) **Commit-time (authoritative)**: During the MySQL transaction, we re-check with a `SELECT SUM(requested_count) FROM reservations WHERE tenant_id = ? AND status IN ('confirmed','active')` and compare against `tenants.quota_machines`. This prevents a race where two concurrent requests both pass the Redis check but together exceed the quota. The Redis counter is reconciled with MySQL every 5 minutes to correct any drift from failed transactions.

**Q: How do you handle the "noisy neighbor" problem in a multi-tenant platform?**
A: Multiple layers: (1) **Network isolation**: Each tenant's machines are on dedicated VLANs. No cross-tenant traffic at L2. (2) **Rate limiting**: Per-tenant API rate limits prevent one tenant from overwhelming the control plane. (3) **Quota enforcement**: Prevents one tenant from consuming all capacity. (4) **Fair-share scheduling**: The waitlist uses fair-share scoring to prevent one tenant from monopolizing the queue. (5) **Dedicated machine pools**: High-value tenants can have machines pre-reserved in dedicated pools that other tenants cannot access. (6) **Bare metal advantage**: Unlike VMs, bare-metal eliminates CPU/memory/IO contention by definition. The noisy-neighbor problem is primarily a control-plane concern, not a data-plane concern.

**Q: How does your system handle a "flash crowd" -- everyone requests GPUs at 9 AM Monday?**
A: (1) **Rate limiting at the gateway**: Absorbs the initial burst. (2) **Admission control**: If the Reservation Service's request queue exceeds 500, low-priority requests (priority > 7) are shed with 429 Retry-After. (3) **HPA auto-scaling**: Adds Reservation Service replicas within 60 seconds. (4) **Request batching**: Multiple reservation requests for the same machine type and time window can be batched by the service (process 50 requests together, allocate machines in bulk, reducing per-request lock contention). (5) **Predictive pre-warming**: Historical analysis shows Monday 9 AM is a peak. We pre-warm additional service replicas at 8:50 AM via a scheduled HPA override. (6) **Client-side exponential backoff**: CLI implements retry with jitter, spreading retries over time.

**Q: How do you ensure provisioning is idempotent when the same machine might be provisioned multiple times due to retries?**
A: Each provisioning activity is designed to be idempotent: (1) **IPMI power-on**: Calling power-on on an already-powered-on machine is a BMC no-op. We verify state before acting. (2) **PXE config**: Config file is keyed by MAC address; writing the same file twice is idempotent. (3) **OS install**: We check if the expected OS image is already installed (by reading a marker file) before triggering a fresh install. (4) **VLAN config**: SDN API upserts VLAN assignments; re-applying the same config is idempotent. (5) **State transitions**: `UPDATE machines SET state = 'in_use' WHERE id = ? AND state = 'provisioning'` -- the WHERE clause ensures idempotency; a repeated call on an already-transitioned machine affects 0 rows. Temporal's activity deduplication adds a further layer: activities are identified by a deterministic ID, and Temporal skips re-execution if the result is already recorded.

**Q: What is your disaster recovery plan if an entire availability zone goes down?**
A: (1) **Active reservations in the affected AZ continue running if the data plane is intact** (machines are powered and networked; they do not depend on the control plane). (2) **New reservations for the affected AZ are rejected** until the AZ recovers. The API returns `503` with a message indicating AZ unavailability. (3) **Cross-AZ failover for the control plane**: The Reservation Service and MySQL are deployed across AZs. MySQL's async replica in another AZ is promoted. Service instances in the surviving AZs handle all traffic. (4) **Affected waitlist entries are re-evaluated**: Waitlisted requests that specified the failed AZ are offered alternative AZs if the tenant allows it. (5) **RTO**: Control plane: 5 minutes (automatic failover). Data plane (machines in the affected AZ): depends on the AZ outage duration -- we cannot recover machines in a physically destroyed AZ, but we can re-allocate capacity in surviving AZs.

**Q: How do you bill tenants accurately? What if a provisioning takes 30 minutes -- does the tenant pay for that time?**
A: Billing starts when the machine state reaches `in_use` (i.e., `actual_start_time` is set), not when the reservation is created or provisioning begins. If provisioning takes 30 minutes, the tenant does not pay for those 30 minutes. Similarly, billing ends at `actual_end_time` (when the machine is fully released), not `end_time` (the reserved window end). This means if a tenant releases early, they pay only for actual usage. The Reservation Service publishes `reservation.active` and `reservation.released` events to Kafka. The billing service consumes these events and calculates charges based on `actual_start_time` to `actual_end_time`, rounded up to the nearest hour. We reconcile billing events against the reservations DB nightly to catch any missed events.

**Q: How do you handle topology-aware placement for large-scale distributed training jobs?**
A: The placement engine maintains a topology graph: Region -> AZ -> Rack -> Switch -> Machine. For a 256-GPU training job: (1) **Same rack (best)**: NVLink/NVSwitch interconnect, lowest latency. We check if any rack has 32 machines (256 GPUs / 8 GPUs per machine) available. (2) **Same leaf switch**: Machines on racks connected to the same leaf switch share a 400G path. (3) **Same AZ, across racks**: Machines traverse the spine network. (4) **Cross-AZ (worst, avoid)**: Network latency > 1ms degrades collective operations. The placement engine tries each level in order, returning the highest-quality placement available. The reservation response includes `placement_quality` so the user can decide whether to proceed or wait for better placement.

**Q: How do you test the entire reservation flow end-to-end?**
A: (1) **Integration tests in CI**: A full stack (MySQL, Redis, Kafka, mock BMC) runs in Docker Compose. Tests create reservations, verify conflict detection, run provisioning against mock IPMI, and validate state transitions. (2) **Staging environment**: A dedicated staging cluster with ~100 real machines (mix of GPU and CPU). Monthly chaos testing (random machine failures, network partitions, service crashes). (3) **Load testing**: k6/Gatling scripts simulate 2x peak traffic against staging. Validates auto-scaling, rate limiting, and queue behavior. (4) **Reconciliation testing**: After each test run, a reconciliation script verifies DB state matches expected state (no phantom reservations, no orphaned machines). (5) **Canary testing in production**: Every deploy goes through the canary pipeline (Section 12).

**Q: How do you handle the cold start problem -- a brand new Reservation Service instance has no interval trees loaded?**
A: The readiness probe returns 503 until interval trees are fully loaded. At startup, the service queries: `SELECT machine_id, start_time, end_time, reservation_id FROM reservation_slots WHERE end_time > NOW()`. For 50K machines x 20 active slots = 1M rows, this query takes ~5 seconds on a read replica. Building the interval trees from the result set takes ~3 seconds. Total cold start: ~10-15 seconds including JVM warmup. The `initialDelaySeconds: 30` on the readiness probe gives generous time. During this window, other replicas handle traffic. For faster cold starts: we cache a serialized snapshot of the interval trees in Redis every 5 minutes. A new instance loads from the Redis snapshot (~1 second) and then applies the delta from Kafka (events since the snapshot timestamp).

**Q: What if a malicious insider with admin access tries to create reservations for free or preempt production workloads?**
A: (1) **Separation of duties**: Admin actions (preemption, maintenance mode) require a different authentication factor (e.g., breakglass approval from two admins). (2) **Immutable audit log**: Every action is logged with the actor's identity. Audit logs are stored in an append-only store that admins cannot modify. (3) **Billing reconciliation**: All reservations generate billing events. Free reservations would be caught by the nightly billing reconciliation. (4) **Preemption alerts**: Every preemption triggers a notification to the affected tenant AND to a security monitoring channel. Unusual preemption patterns (e.g., admin preempting production workloads to allocate to a personal project) would be flagged. (5) **Access reviews**: Admin role membership is reviewed quarterly. Principle of least privilege -- most engineers have `engineer` role, not `admin`.

**Q: How do you decide between adding more machines to the fleet vs. optimizing utilization?**
A: Both, with data-driven decisions. Metrics tracked: (1) **Fleet utilization** (active machines / total machines). Target: 70-85%. Below 70%: over-provisioned, consider decommissioning. Above 85%: capacity is tight, add machines. (2) **Waitlist depth and wait time**. If p50 wait time > 2 hours for any machine type, that is a capacity signal. (3) **Reservation fragmentation**: If machines are available but time slots are too fragmented for typical request durations, we need to defragment (compact reservations, add a consolidation pass). (4) **Forecast**: The AI capacity forecasting agent predicts demand 4 weeks out based on team growth, project schedules, and seasonal patterns. This gives procurement lead time (ordering H100 servers takes 8-12 weeks). Decision thresholds are codified in a capacity planning runbook.

---

## 16. References

- "Designing Data-Intensive Applications" by Martin Kleppmann -- Chapter 7 (Transactions), Chapter 9 (Consistency and Consensus). Foundational for understanding isolation levels, locking strategies, and distributed coordination.
- "Introduction to Algorithms" (CLRS) -- Chapter 14.3 (Interval Trees). Augmented BST for interval overlap detection.
- "Temporal.io Documentation" -- Workflow and activity patterns, retry policies, saga pattern implementation. Source: docs.temporal.io.
- "Vitess: Scaling MySQL" -- Online resharding, VReplication, connection pooling. Source: vitess.io/docs.
- "MySQL 8.0 Reference Manual: InnoDB Locking" -- SELECT FOR UPDATE semantics, gap locks, deadlock detection. Source: dev.mysql.com.
- "gh-ost: GitHub's Online Schema Migration" -- Triggerless online DDL for MySQL. Source: github.com/github/gh-ost.
- "How We Manage Bare Metal at Scale" -- Meta Engineering Blog. Fleet management, health scoring, automated remediation.
- "Borg, Omega, and Kubernetes" -- Google research paper on cluster management, resource allocation, preemption. Source: ACM Queue, 2016.
- "Is Redlock Safe?" -- Martin Kleppmann's analysis of distributed lock correctness. Source: martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html.
- "The OpenStack Bare Metal (Ironic) Project" -- Provisioning architecture, IPMI/PXE workflows. Source: docs.openstack.org/ironic.
- "NVIDIA DGX SuperPOD Architecture" -- GPU server topology, NVLink/NVSwitch interconnect design. Source: NVIDIA technical documentation.
- "Resilience4j Documentation" -- Circuit breaker, retry, rate limiter patterns for Java. Source: resilience4j.readme.io.
- "Spring Statemachine Reference" -- State machine configuration, transitions, guards. Source: docs.spring.io/spring-statemachine.
