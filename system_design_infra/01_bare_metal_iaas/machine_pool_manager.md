# System Design: Machine Pool Manager

> **Relevance to role:** A cloud infrastructure platform engineer must manage the physical fleet lifecycle -- abstracting heterogeneous hardware into pools, automating health monitoring and machine ejection, predicting capacity needs, managing firmware across thousands of machines, and coordinating vendor RMA processes -- all while maintaining high fleet utilization and reliability.

---

## 1. Requirement Clarifications

### Functional Requirements
- **Pool abstraction**: Group machines into logical pools by type (gpu_h100_8x, cpu_general, cpu_highmem), region, and purpose (production, staging, reserved).
- **Pool lifecycle management**: Create, resize, split, merge, drain, and retire pools.
- **Warm pool / cold pool**: Maintain a warm pool of pre-provisioned machines (OS installed, network configured, ready for assignment in < 2 minutes). Cold pool: machines in standby (powered off) for overflow capacity.
- **Machine health scoring**: Continuous health assessment (0-100 score) based on hardware metrics (CPU errors, memory ECC, GPU temperature, disk health, network errors, IPMI responsiveness).
- **Automatic ejection**: Machines below health threshold are automatically removed from the available pool, marked for maintenance, and replacement capacity is sourced.
- **Capacity prediction**: Forecast capacity demand by pool type and region, 2-8 weeks ahead, based on historical reservation patterns and growth trends.
- **Pool sizing**: Recommend optimal pool sizes based on demand forecast, utilization targets (70-85%), and procurement lead times.
- **Firmware/BIOS management**: Track firmware versions across the fleet, plan and execute rolling firmware upgrades, validate compatibility.
- **Vendor RMA integration**: Track hardware failures, create RMA tickets with vendors, track replacement delivery and installation.
- **Pool drain**: Gracefully drain a pool (wait for reservations to end, block new reservations) for hardware refresh or rack maintenance.
- **Pool metrics**: Real-time dashboard showing utilization, health distribution, capacity trends, failure rates per pool.

### Non-Functional Requirements
- **Availability target**: Pool management API: 99.99%. Health monitoring pipeline: 99.95% (brief gaps acceptable if caught by the reconciliation sweep).
- **Consistency model**: Strong consistency for pool membership and machine state transitions. Eventual consistency for health scores (< 30s lag).
- **Latency target**: Health check per machine: < 5s. Pool membership query: < 50ms. Capacity forecast generation: < 30s.
- **Durability**: All pool configuration, health history, and RMA records: RPO = 0.
- **Scalability**: 100,000 machines, 500 pools, 100 health checks/sec (all machines checked every ~15 minutes).

### Constraints & Assumptions
- Health data is collected from: IPMI/BMC telemetry, in-band agent running on the OS, network switch counters, and Ceph/storage health endpoints.
- Firmware updates are vendor-specific: NVIDIA (GPU firmware), Intel/AMD (CPU microcode, BIOS), Broadcom/Mellanox (NIC firmware), vendor-specific (BMC firmware).
- RMA processes vary by vendor: Dell, HPE, Supermicro each have different portals, SLAs, and part tracking systems.
- Procurement lead times: CPU servers 4-6 weeks, GPU servers (H100) 8-16 weeks, storage nodes 4-6 weeks.
- Health check agents are pre-installed in the base OS image.

### Out of Scope
- Reservation management (covered in bare_metal_reservation_platform.md)
- OS image build and distribution pipeline
- Physical data center operations (racking, cabling, power management)
- Procurement approval and purchasing workflows (finance system)

---

## 2. Scale & Capacity Estimates

### Users & Traffic
| Metric | Value | Calculation |
|--------|-------|-------------|
| Total managed machines | 100,000 | Across all regions |
| Total pools | 500 | ~10 machine types x 5 regions x ~10 purpose pools |
| Health checks/sec | 110 | 100,000 machines / 900 seconds (15-min interval) |
| Health data points/sec | 1,100 | 110 machines x ~10 metrics each |
| Pool API calls/sec | 50 | Pool management operations (low volume) |
| Firmware operations/day | ~200 | Rolling firmware upgrades (batch of 200/day during maintenance windows) |
| RMA tickets created/week | ~50 | ~0.1% failure rate per week on 50K active machines |

### Latency Requirements
| Operation | Target | Justification |
|-----------|--------|---------------|
| Health check (single machine) | < 5s | IPMI query + agent metrics + network stats |
| Health score computation | < 100ms | Weighted formula on collected metrics |
| Pool membership query | < 50ms | Redis cache of pool -> machine mappings |
| Capacity forecast (one pool) | < 30s | Statistical computation on historical data |
| Machine ejection (auto) | < 5 min | From health score drop to machine removed from available pool |
| Firmware upgrade (per machine) | 15-45 min | Reboot + BIOS/firmware flash + validation |

### Storage Estimates
| Data type | Size/record | Volume/day | Retention | Total |
|-----------|-------------|------------|-----------|-------|
| Health check records | ~500 B | 9.5M/day (100K x ~95/day) | 90 days (hot), 1 year (cold) | ~430 GB/90 days hot |
| Health score time series | ~100 B/point | 9.5M/day | 1 year | ~350 GB/year |
| Pool configuration | ~2 KB/pool | ~500 updates/day | Forever | Negligible |
| Firmware inventory | ~500 B/machine | 100K records | Forever | ~50 MB |
| RMA records | ~5 KB/ticket | ~50/week | 5 years | ~65 MB/year |
| Capacity forecast history | ~1 KB/forecast | ~500/day (per pool) | 1 year | ~180 MB/year |

### Bandwidth Estimates
| Direction | Calculation | Result |
|-----------|-------------|--------|
| Health data inbound | 1,100 data points/sec x 500 B = 550 KB/s | ~4 Mbps |
| IPMI queries | 110/sec x 1 KB = 110 KB/s | ~1 Mbps |
| Firmware images (during upgrade window) | 200 machines/day x 500 MB image / 8 hours = 3.5 MB/s | ~28 Mbps |
| Time series writes (Prometheus) | 1,100 points/sec x 100 B = 110 KB/s | ~1 Mbps |

---

## 3. High Level Architecture

```
     +-------------------------------------------------------------------+
     |                   Machine Health Data Sources                      |
     | +----------+ +----------+ +----------+ +----------+ +-----------+ |
     | |   IPMI   | | In-band  | | Network  | | Storage  | | GPU       | |
     | |   /BMC   | | Agent    | | Switch   | | Health   | | Telemetry | |
     | | Telemetry| | (node_   | | Counters | | (Ceph)   | | (nvidia-  | |
     | |          | |  exporter| | (SNMP)   | |          | |  smi)     | |
     | +----+-----+ +----+-----+ +----+-----+ +----+-----+ +----+------+ |
     +------|------------|------------|------------|------------|----------+
            |            |            |            |            |
            v            v            v            v            v
     +------+------------+------------+------------+------------+------+
     |                  Health Data Collector (Python)                  |
     |  - Polls IPMI/BMC every 15 min                                 |
     |  - Scrapes Prometheus endpoints (node_exporter, nvidia_exporter)|
     |  - Queries SNMP on switches                                     |
     |  - Aggregates per-machine health bundle                         |
     +----------------------------+------------------------------------+
                                  |
                                  v
     +----------------------------+------------------------------------+
     |                   Health Scoring Engine (Python)                 |
     |  - Weighted health score computation (0-100)                    |
     |  - Anomaly detection (EWMA, Z-score)                           |
     |  - Trend analysis (score declining over time)                   |
     +--------+---------------------------+----------------------------+
              |                           |
              v                           v
     +--------+--------+       +---------+--------+
     |  Time Series DB  |       | Health Events    |
     |  (Prometheus /   |       | (Kafka topic:    |
     |   VictoriaMetrics)|      | machine.health)  |
     +------------------+       +--------+---------+
                                         |
                              +----------+-----------+
                              |                      |
                              v                      v
                    +---------+--------+   +---------+--------+
                    | Pool Manager     |   | Alert Manager    |
                    | Service (Java)   |   | (PagerDuty)      |
                    |                  |   |                  |
                    | - Pool CRUD      |   | - Health alerts  |
                    | - Auto-ejection  |   | - Capacity alerts|
                    | - Warm/cold pool |   | - Firmware alerts|
                    | - Pool drain     |   |                  |
                    | - Pool sizing    |   |                  |
                    +--------+---------+   +------------------+
                             |
              +--------------+------------------+
              |              |                  |
              v              v                  v
     +--------+---+  +------+------+  +---------+--------+
     | Capacity   |  | Firmware   |  | RMA Service      |
     | Forecast   |  | Manager    |  | (Python)         |
     | Engine     |  | (Python)   |  |                  |
     | (Python)   |  |            |  | - Vendor API     |
     |            |  | - Version  |  |   integration    |
     | - Demand   |  |   tracking |  | - Ticket mgmt   |
     |   predict  |  | - Rolling  |  | - Part tracking  |
     | - Pool     |  |   upgrade  |  |                  |
     |   sizing   |  |   planner  |  |                  |
     +------------+  +------------+  +------------------+
              |              |                  |
              +-------+------+------------------+
                      |
                      v
     +----------------+-----------------+
     |          Data Stores             |
     | +--------+  +--------+  +------+ |
     | | MySQL  |  | Redis  |  | S3   | |
     | | (state)|  | (cache |  |(firm | |
     | |        |  |  pool  |  | ware | |
     | |        |  |  index)|  | imgs)| |
     | +--------+  +--------+  +------+ |
     +----------------------------------+
```

### Component Roles

**Health Data Collector (Python):** Runs per-AZ (deployed on management nodes). Polls IPMI/BMC for hardware telemetry (CPU temperature, fan speed, power draw, SEL events). Scrapes Prometheus endpoints on each machine (node_exporter for CPU/memory/disk, nvidia_gpu_exporter for GPU metrics). Queries SNMP on ToR switches for per-port error counters. Assembles a per-machine health bundle every 15 minutes.

**Health Scoring Engine (Python):** Computes a weighted health score (0-100) for each machine based on the collected metrics. Uses configurable thresholds and weights per machine type. Detects anomalies (sudden drops) and trends (gradual degradation). Publishes health events to Kafka and writes time series to Prometheus/VictoriaMetrics.

**Pool Manager Service (Java/Spring Boot):** Core service for pool lifecycle operations. Manages pool membership (which machines belong to which pool). Implements auto-ejection logic (ejects machines below health threshold). Manages warm pool and cold pool states. Provides API for pool drain, resize, and capacity queries.

**Capacity Forecast Engine (Python):** Analyzes historical reservation data and health trends to predict capacity demand per pool. Uses time series forecasting (ARIMA, exponential smoothing) to project demand 2-8 weeks ahead. Generates pool sizing recommendations factoring in procurement lead times and utilization targets.

**Firmware Manager (Python):** Tracks firmware versions across the entire fleet. Plans rolling firmware upgrades (selects machines, schedules maintenance windows, controls blast radius). Executes upgrades via IPMI firmware flash commands. Validates post-upgrade firmware version.

**RMA Service (Python):** Integrates with vendor RMA portals (Dell TechDirect, HPE RMA, Supermicro RMA). Creates tickets for failed hardware, tracks replacement part delivery, manages installation scheduling, and returns defective parts.

### Primary Data Flow: Health Check and Auto-Ejection

```
1. Health Data Collector polls machine m-1234 via IPMI:
   - CPU temperature: 72°C (normal)
   - Memory ECC errors: 47 correctable in last hour (elevated!)
   - GPU temperature: 68°C (normal)
   - Fan speed: 4200 RPM (normal)
   - Power draw: 1250W (normal)
   
2. Health Data Collector scrapes Prometheus on m-1234:
   - node_cpu_seconds_total: nominal
   - node_memory_MemAvailable_bytes: nominal
   - nvme_smart_critical_warning: 0 (healthy)
   - nvidia_gpu_memory_ecc_errors: 0 (healthy)
   
3. Health Scoring Engine computes:
   score = (
     cpu_temp_score * 0.10 +        # 95/100 * 0.10 = 9.5
     memory_ecc_score * 0.25 +      # 40/100 * 0.25 = 10.0  (degraded!)
     gpu_health_score * 0.25 +      # 100/100 * 0.25 = 25.0
     disk_health_score * 0.15 +     # 100/100 * 0.15 = 15.0
     network_health_score * 0.10 +  # 100/100 * 0.10 = 10.0
     bmc_responsive_score * 0.10 +  # 100/100 * 0.10 = 10.0
     power_score * 0.05             # 100/100 * 0.05 = 5.0
   ) = 84.5
   
   Previous score: 92.0 → Score dropped by 7.5 points.
   
4. Health Scoring Engine publishes to Kafka:
   {
     "machine_id": "m-1234",
     "health_score": 84.5,
     "previous_score": 92.0,
     "delta": -7.5,
     "degraded_components": ["memory_ecc"],
     "details": {"memory_ecc_correctable_1h": 47}
   }
   
5. Pool Manager Service consumes the event:
   - Score 84.5 is above WARNING_THRESHOLD (60) but below WATCH_THRESHOLD (90).
   - Machine is added to the "watch list" (checked every 5 min instead of 15 min).
   - If score continues to degrade below 60: schedule maintenance.
   - If score drops below 30 (CRITICAL): immediate ejection.
   
6. After 3 more checks, score drops to 55 (memory ECC errors increasing):
   - Pool Manager ejects m-1234 from the available pool:
     a. Update machine state: available → maintenance
     b. Remove from Redis pool index
     c. If machine is reserved/in_use: notify tenant, offer replacement
     d. Create maintenance task: "Investigate memory ECC errors on m-1234"
     e. Publish machine.ejected event to Kafka
   - Warm pool sizing: check if warm pool for this machine type is below
     target. If so, promote a machine from cold pool to warm pool.
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Pool: logical grouping of machines
CREATE TABLE pools (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    pool_uuid           CHAR(36) NOT NULL,
    name                VARCHAR(128) NOT NULL,           -- e.g., 'gpu_h100_8x_prod_use1'
    machine_type        VARCHAR(64) NOT NULL,
    region              VARCHAR(32) NOT NULL,
    availability_zone   VARCHAR(32) NULL,                -- NULL = spans entire region
    purpose             ENUM('production','staging','reserved','overflow','decommission')
                        NOT NULL DEFAULT 'production',
    state               ENUM('active','draining','frozen','retired') NOT NULL DEFAULT 'active',
    
    -- Sizing targets
    target_size         INT UNSIGNED NOT NULL,            -- Target number of machines
    min_size            INT UNSIGNED NOT NULL,            -- Minimum before alert
    max_size            INT UNSIGNED NOT NULL,            -- Maximum (cap)
    warm_pool_target    INT UNSIGNED NOT NULL DEFAULT 0,  -- Pre-provisioned ready machines
    cold_pool_target    INT UNSIGNED NOT NULL DEFAULT 0,  -- Standby (powered off) machines
    
    -- Utilization targets
    target_utilization  DECIMAL(4,1) NOT NULL DEFAULT 75.0,  -- Target: 75%
    min_utilization     DECIMAL(4,1) NOT NULL DEFAULT 50.0,  -- Alert below this
    max_utilization     DECIMAL(4,1) NOT NULL DEFAULT 90.0,  -- Capacity crunch above this
    
    -- Health thresholds (pool-level override of global defaults)
    health_eject_threshold   TINYINT UNSIGNED NOT NULL DEFAULT 30,
    health_watch_threshold   TINYINT UNSIGNED NOT NULL DEFAULT 60,
    health_warning_threshold TINYINT UNSIGNED NOT NULL DEFAULT 90,
    
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_uuid (pool_uuid),
    UNIQUE KEY uk_name (name),
    INDEX idx_type_region (machine_type, region),
    INDEX idx_state (state)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Pool membership (which machines are in which pool)
CREATE TABLE pool_memberships (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    pool_id             BIGINT UNSIGNED NOT NULL,
    machine_id          BIGINT UNSIGNED NOT NULL,
    pool_state          ENUM('warm','cold','active','draining','ejected') NOT NULL DEFAULT 'active',
    joined_at           DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    left_at             DATETIME NULL,
    
    UNIQUE KEY uk_machine_pool (machine_id, pool_id, left_at),
    INDEX idx_pool_state (pool_id, pool_state),
    FOREIGN KEY (pool_id) REFERENCES pools(id),
    FOREIGN KEY (machine_id) REFERENCES machines(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Health check records
CREATE TABLE health_checks (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    machine_id          BIGINT UNSIGNED NOT NULL,
    health_score        TINYINT UNSIGNED NOT NULL,        -- 0-100
    cpu_temp_c          SMALLINT UNSIGNED NULL,
    memory_ecc_correctable SMALLINT UNSIGNED NULL,
    memory_ecc_uncorrectable SMALLINT UNSIGNED NULL,
    gpu_temp_c          SMALLINT UNSIGNED NULL,
    gpu_ecc_errors      SMALLINT UNSIGNED NULL,
    disk_smart_status    TINYINT UNSIGNED NULL,            -- 0=fail, 1=pass
    disk_wear_pct       TINYINT UNSIGNED NULL,             -- 0-100
    network_errors_1h   INT UNSIGNED NULL,
    bmc_responsive      BOOLEAN NULL,
    power_draw_watts    SMALLINT UNSIGNED NULL,
    fan_speed_rpm       SMALLINT UNSIGNED NULL,
    details_json        JSON NULL,                         -- Additional vendor-specific metrics
    checked_at          DATETIME NOT NULL,
    
    INDEX idx_machine_time (machine_id, checked_at),
    INDEX idx_score_time (health_score, checked_at),
    FOREIGN KEY (machine_id) REFERENCES machines(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (TO_DAYS(checked_at)) (
    PARTITION p_current VALUES LESS THAN (TO_DAYS('2026-05-01')),
    PARTITION p_next VALUES LESS THAN (TO_DAYS('2026-06-01')),
    PARTITION p_future VALUES LESS THAN MAXVALUE
);

-- Firmware inventory
CREATE TABLE firmware_inventory (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    machine_id          BIGINT UNSIGNED NOT NULL,
    component           ENUM('bios','bmc','gpu','nic','nvme','cpu_microcode','system_board')
                        NOT NULL,
    current_version     VARCHAR(64) NOT NULL,
    target_version      VARCHAR(64) NULL,                -- NULL = no upgrade pending
    last_updated        DATETIME NOT NULL,
    update_status       ENUM('current','pending','in_progress','failed','rollback')
                        NOT NULL DEFAULT 'current',
    
    UNIQUE KEY uk_machine_component (machine_id, component),
    INDEX idx_component_version (component, current_version),
    INDEX idx_update_status (update_status),
    FOREIGN KEY (machine_id) REFERENCES machines(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Firmware releases (target versions per machine type)
CREATE TABLE firmware_releases (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    release_uuid        CHAR(36) NOT NULL,
    machine_type        VARCHAR(64) NOT NULL,
    component           ENUM('bios','bmc','gpu','nic','nvme','cpu_microcode','system_board')
                        NOT NULL,
    version             VARCHAR(64) NOT NULL,
    release_date        DATE NOT NULL,
    severity            ENUM('critical','recommended','optional') NOT NULL,
    release_notes_url   VARCHAR(512) NULL,
    firmware_image_url  VARCHAR(512) NOT NULL,            -- S3/GCS URL
    firmware_image_hash VARCHAR(128) NOT NULL,             -- SHA-256 for integrity
    compatible_bios_versions JSON NULL,                    -- Required BIOS versions for compatibility
    rollback_version    VARCHAR(64) NULL,                  -- Version to roll back to on failure
    
    UNIQUE KEY uk_uuid (release_uuid),
    INDEX idx_type_component (machine_type, component, release_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Firmware upgrade plans (rolling upgrade batches)
CREATE TABLE firmware_upgrade_plans (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    plan_uuid           CHAR(36) NOT NULL,
    release_id          BIGINT UNSIGNED NOT NULL,
    status              ENUM('planning','approved','in_progress','paused',
                             'completed','aborted') NOT NULL DEFAULT 'planning',
    total_machines      INT UNSIGNED NOT NULL,
    completed_machines  INT UNSIGNED NOT NULL DEFAULT 0,
    failed_machines     INT UNSIGNED NOT NULL DEFAULT 0,
    batch_size          SMALLINT UNSIGNED NOT NULL DEFAULT 10,
    max_failure_pct     DECIMAL(4,1) NOT NULL DEFAULT 5.0, -- Abort if > 5% fail
    maintenance_window  VARCHAR(64) NOT NULL,              -- e.g., 'tue_wed_02:00-06:00_utc'
    created_by          BIGINT UNSIGNED NOT NULL,
    approved_by         BIGINT UNSIGNED NULL,
    started_at          DATETIME NULL,
    completed_at        DATETIME NULL,
    
    UNIQUE KEY uk_uuid (plan_uuid),
    FOREIGN KEY (release_id) REFERENCES firmware_releases(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- RMA tickets
CREATE TABLE rma_tickets (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    ticket_uuid         CHAR(36) NOT NULL,
    machine_id          BIGINT UNSIGNED NOT NULL,
    vendor              ENUM('dell','hpe','supermicro','nvidia','other') NOT NULL,
    vendor_ticket_id    VARCHAR(128) NULL,                 -- Vendor's RMA number
    component_type      VARCHAR(64) NOT NULL,              -- e.g., 'dimm','gpu','nvme','motherboard'
    component_serial    VARCHAR(128) NULL,
    failure_description TEXT NOT NULL,
    status              ENUM('created','submitted_to_vendor','part_shipped',
                             'part_received','installed','verified','closed')
                        NOT NULL DEFAULT 'created',
    severity            ENUM('critical','high','normal') NOT NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    resolved_at         DATETIME NULL,
    
    UNIQUE KEY uk_uuid (ticket_uuid),
    INDEX idx_machine (machine_id),
    INDEX idx_vendor_status (vendor, status),
    INDEX idx_status (status),
    FOREIGN KEY (machine_id) REFERENCES machines(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Capacity forecasts
CREATE TABLE capacity_forecasts (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    pool_id             BIGINT UNSIGNED NOT NULL,
    forecast_date       DATE NOT NULL,                     -- Date being forecast
    generated_at        DATETIME NOT NULL,                 -- When forecast was generated
    predicted_demand    INT UNSIGNED NOT NULL,              -- Machines needed
    confidence_lower    INT UNSIGNED NOT NULL,              -- 90% CI lower bound
    confidence_upper    INT UNSIGNED NOT NULL,              -- 90% CI upper bound
    current_supply      INT UNSIGNED NOT NULL,              -- Machines available
    gap                 INT NOT NULL,                       -- demand - supply (negative = surplus)
    model_type          VARCHAR(32) NOT NULL,               -- 'arima','exponential_smoothing','prophet'
    
    INDEX idx_pool_date (pool_id, forecast_date),
    FOREIGN KEY (pool_id) REFERENCES pools(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

**MySQL 8.0** for all relational data (pool configuration, firmware inventory, RMA tickets, capacity forecasts). Low write volume, strong consistency needed.

**Prometheus / VictoriaMetrics** for time series health metrics. Purpose-built for high-cardinality metrics at 1,100 data points/sec.

**Redis** for real-time pool index (pool_id -> set of machine_ids in each state: warm, cold, active). Powers fast pool membership queries (< 1ms).

**Kafka** for health events (machine.health topic) consumed by Pool Manager and Alert Manager.

**S3/GCS** for firmware images (large binaries, versioned, durable).

### Indexing Strategy

- **`idx_machine_time`** on health_checks: Powers health history queries. Partitioned by month for efficient pruning of old data.
- **`idx_pool_state`** on pool_memberships: Powers the pool capacity dashboard ("how many warm machines in pool X?").
- **`idx_component_version`** on firmware_inventory: Powers the firmware compliance dashboard ("how many machines have GPU firmware < 2.3.0?").
- **`idx_vendor_status`** on rma_tickets: Powers the RMA dashboard ("how many open tickets with Dell?").

---

## 5. API Design

```
# === Pools ===

POST   /api/v1/pools
  Description: Create a new machine pool
  Request: {
    "name": "gpu_h100_8x_prod_use1",
    "machine_type": "gpu_h100_8x",
    "region": "us-east-1",
    "purpose": "production",
    "target_size": 500,
    "warm_pool_target": 20,
    "cold_pool_target": 50,
    "target_utilization": 75.0
  }
  Response: 201 Created

GET    /api/v1/pools
  Query: ?machine_type=gpu_h100_8x&region=us-east-1&state=active
  Response: 200 OK { list of pools with summary stats }

GET    /api/v1/pools/{pool_id}
  Response: 200 OK { pool config + current stats: size, utilization, health distribution }

PATCH  /api/v1/pools/{pool_id}
  Description: Update pool configuration (resize, change thresholds)
  Request: { "target_size": 600, "warm_pool_target": 30 }

POST   /api/v1/pools/{pool_id}/drain
  Description: Begin draining a pool (block new reservations, wait for existing to end)
  Request: { "reason": "rack_maintenance", "drain_deadline": "2026-04-15T00:00:00Z" }

POST   /api/v1/pools/{pool_id}/freeze
  Description: Freeze pool (no changes allowed)
  Request: { "reason": "investigation" }

# === Pool Machines ===

GET    /api/v1/pools/{pool_id}/machines
  Query: ?pool_state=warm&health_score_min=80
  Response: 200 OK { list of machines in pool with health scores }

POST   /api/v1/pools/{pool_id}/machines
  Description: Add machine to pool
  Request: { "machine_id": "m-1234", "pool_state": "cold" }

DELETE /api/v1/pools/{pool_id}/machines/{machine_id}
  Description: Remove machine from pool

POST   /api/v1/pools/{pool_id}/machines/{machine_id}/promote
  Description: Promote from cold to warm (pre-provision)

POST   /api/v1/pools/{pool_id}/machines/{machine_id}/demote
  Description: Demote from warm to cold (power down)

# === Health ===

GET    /api/v1/machines/{machine_id}/health
  Description: Current health score and component breakdown
  Response: 200 OK {
    "health_score": 84.5,
    "components": {
      "cpu_temp": {"score": 95, "value": "72°C"},
      "memory_ecc": {"score": 40, "value": "47 correctable/hr", "status": "degraded"},
      "gpu": {"score": 100, "value": "68°C"},
      ...
    },
    "trend": "declining",
    "watch_list": true
  }

GET    /api/v1/machines/{machine_id}/health/history
  Query: ?from=2026-04-01&to=2026-04-09&resolution=1h
  Response: 200 OK { time series of health scores }

GET    /api/v1/pools/{pool_id}/health/distribution
  Response: 200 OK {
    "distribution": {
      "90-100": 380,
      "60-89": 95,
      "30-59": 18,
      "0-29": 7
    },
    "avg_score": 91.2,
    "machines_on_watch": 18,
    "machines_ejected_30d": 12
  }

# === Firmware ===

GET    /api/v1/firmware/compliance
  Description: Fleet-wide firmware compliance report
  Query: ?machine_type=gpu_h100_8x&component=gpu
  Response: 200 OK {
    "target_version": "535.104.12",
    "compliant": 4850,
    "non_compliant": 150,
    "in_progress": 20,
    "machines_by_version": {
      "535.104.12": 4850,
      "535.86.10": 130,
      "530.30.02": 20
    }
  }

POST   /api/v1/firmware/upgrade-plans
  Description: Create a rolling firmware upgrade plan
  Request: {
    "release_id": "rel-abc123",
    "pool_ids": ["pool-1", "pool-2"],
    "batch_size": 10,
    "max_failure_pct": 5.0,
    "maintenance_window": "tue_wed_02:00-06:00_utc"
  }
  Response: 201 Created { plan details with machine list }

POST   /api/v1/firmware/upgrade-plans/{plan_id}/approve
POST   /api/v1/firmware/upgrade-plans/{plan_id}/start
POST   /api/v1/firmware/upgrade-plans/{plan_id}/pause
POST   /api/v1/firmware/upgrade-plans/{plan_id}/abort

GET    /api/v1/firmware/upgrade-plans/{plan_id}
  Response: 200 OK { plan status, batch progress, failure details }

# === RMA ===

POST   /api/v1/rma/tickets
  Description: Create RMA ticket
  Request: {
    "machine_id": "m-1234",
    "component_type": "dimm",
    "failure_description": "Excessive ECC correctable errors (47/hr), slot B2"
  }
  Response: 201 Created

GET    /api/v1/rma/tickets
  Query: ?vendor=dell&status=submitted_to_vendor
  Response: 200 OK { list of tickets }

PATCH  /api/v1/rma/tickets/{ticket_id}
  Description: Update ticket status (e.g., part received)
  Request: { "status": "part_received" }

# === Capacity ===

GET    /api/v1/capacity/forecast
  Query: ?pool_id=pool-1&weeks_ahead=4
  Response: 200 OK {
    "pool": "gpu_h100_8x_prod_use1",
    "forecasts": [
      {"week": "2026-04-14", "demand": 520, "supply": 500, "gap": 20,
       "confidence": [510, 530]},
      ...
    ],
    "recommendation": "Procure 30 additional gpu_h100_8x machines. Lead time: 12 weeks."
  }

GET    /api/v1/capacity/utilization
  Query: ?pool_id=pool-1&from=2026-03-01&to=2026-04-09
  Response: 200 OK { daily utilization time series }
```

### CLI Design

```bash
# Pool management
infra-cli pool list --machine-type gpu_h100_8x --region us-east-1
infra-cli pool show pool-1
infra-cli pool create --name gpu_h100_8x_prod_use1 --type gpu_h100_8x --region us-east-1 --target-size 500
infra-cli pool drain pool-1 --reason "rack maintenance" --deadline 2026-04-15
infra-cli pool resize pool-1 --target-size 600

# Health
infra-cli health show m-1234
infra-cli health history m-1234 --days 7 --output chart
infra-cli health distribution pool-1

# Firmware
infra-cli firmware compliance --type gpu_h100_8x --component gpu
infra-cli firmware plan-upgrade --release rel-abc123 --pools pool-1,pool-2 --batch-size 10
infra-cli firmware start-upgrade plan-xyz
infra-cli firmware status plan-xyz

# RMA
infra-cli rma create --machine m-1234 --component dimm --description "ECC errors slot B2"
infra-cli rma list --vendor dell --status open
infra-cli rma update ticket-123 --status part_received

# Capacity
infra-cli capacity forecast --pool pool-1 --weeks 4
infra-cli capacity utilization --pool pool-1 --days 30
```

---

## 6. Core Component Deep Dives

### Component: Health Scoring Engine

**Why it's hard:** Different hardware components have different failure modes, different criticality levels, and different degradation patterns. A GPU ECC error is far more concerning than a slightly elevated CPU temperature. The scoring system must be configurable per machine type (GPU servers have different thresholds than CPU-only servers), detect both sudden failures and gradual degradation, and produce actionable scores that drive automated decisions.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Simple threshold** | Binary healthy/unhealthy per metric | Simple | No gradation; miss subtle degradation | Very simple systems |
| **Weighted linear score** | Each metric contributes a weighted score to a total | Intuitive, configurable, transparent | Static thresholds may not fit all scenarios | **Our use case: configurable, auditable, production-proven** |
| **ML-based anomaly detection** | Train model on healthy machine behavior, flag deviations | Catches unknown failure modes | Black box, requires training data, false positives | Supplementary anomaly detection layer |
| **Bayesian network** | Model dependencies between failure modes | Captures causal relationships | Complex to build and maintain | Research / advanced analytics |

**Selected Approach:** Weighted linear score as the primary method, with EWMA-based anomaly detection as a supplementary layer.

**Implementation Detail:**

```python
from dataclasses import dataclass
from typing import Dict, Optional
import math

@dataclass
class HealthMetrics:
    cpu_temp_c: Optional[float] = None
    memory_ecc_correctable_1h: Optional[int] = None
    memory_ecc_uncorrectable: Optional[int] = None
    gpu_temp_c: Optional[float] = None
    gpu_ecc_errors: Optional[int] = None
    disk_smart_pass: Optional[bool] = None
    disk_wear_pct: Optional[float] = None
    nvme_media_errors: Optional[int] = None
    network_errors_1h: Optional[int] = None
    bmc_responsive: Optional[bool] = None
    power_draw_watts: Optional[float] = None
    fan_speed_rpm: Optional[int] = None

# Scoring configuration per machine type
SCORING_CONFIGS = {
    "gpu_h100_8x": {
        "weights": {
            "cpu_temp": 0.05,
            "memory_ecc": 0.20,
            "gpu_health": 0.30,      # GPUs are the most valuable component
            "disk_health": 0.15,
            "network": 0.10,
            "bmc": 0.10,
            "power_thermal": 0.10,
        },
        "thresholds": {
            "cpu_temp_warning": 80,       # °C
            "cpu_temp_critical": 95,
            "memory_ecc_correctable_warning": 10,    # per hour
            "memory_ecc_correctable_critical": 100,
            "memory_ecc_uncorrectable_any": True,     # Any = critical
            "gpu_temp_warning": 80,
            "gpu_temp_critical": 90,
            "gpu_ecc_critical": 1,        # Any GPU ECC = investigate
            "disk_wear_warning": 80,      # percentage
            "disk_wear_critical": 95,
            "network_errors_warning": 100,  # per hour
            "network_errors_critical": 1000,
        }
    },
    "cpu_general": {
        "weights": {
            "cpu_temp": 0.10,
            "memory_ecc": 0.25,
            "gpu_health": 0.0,         # No GPU
            "disk_health": 0.25,
            "network": 0.15,
            "bmc": 0.15,
            "power_thermal": 0.10,
        },
        "thresholds": {
            # Similar but no GPU thresholds
        }
    }
}


class HealthScoringEngine:
    
    def compute_health_score(self, machine_type: str, 
                              metrics: HealthMetrics) -> dict:
        config = SCORING_CONFIGS[machine_type]
        weights = config["weights"]
        thresholds = config["thresholds"]
        
        component_scores = {}
        degraded_components = []
        
        # CPU temperature score (100 = perfect, 0 = critical)
        if metrics.cpu_temp_c is not None:
            cpu_score = self._range_score(
                metrics.cpu_temp_c,
                perfect=40,                                # Below 40°C: perfect
                warning=thresholds["cpu_temp_warning"],
                critical=thresholds["cpu_temp_critical"])
            component_scores["cpu_temp"] = cpu_score
            if cpu_score < 60:
                degraded_components.append("cpu_temp")
        
        # Memory ECC score
        ecc_score = 100
        if metrics.memory_ecc_uncorrectable and metrics.memory_ecc_uncorrectable > 0:
            ecc_score = 0  # Uncorrectable ECC = critical (data corruption risk)
            degraded_components.append("memory_ecc_uncorrectable")
        elif metrics.memory_ecc_correctable_1h is not None:
            ecc_score = self._inverse_range_score(
                metrics.memory_ecc_correctable_1h,
                perfect=0,
                warning=thresholds["memory_ecc_correctable_warning"],
                critical=thresholds["memory_ecc_correctable_critical"])
            if ecc_score < 60:
                degraded_components.append("memory_ecc")
        component_scores["memory_ecc"] = ecc_score
        
        # GPU health score
        if weights["gpu_health"] > 0:
            gpu_score = 100
            if metrics.gpu_ecc_errors and metrics.gpu_ecc_errors > 0:
                gpu_score = max(0, 100 - metrics.gpu_ecc_errors * 50)
                degraded_components.append("gpu_ecc")
            if metrics.gpu_temp_c is not None:
                temp_score = self._range_score(
                    metrics.gpu_temp_c,
                    perfect=50,
                    warning=thresholds["gpu_temp_warning"],
                    critical=thresholds["gpu_temp_critical"])
                gpu_score = min(gpu_score, temp_score)
            component_scores["gpu_health"] = gpu_score
        
        # Disk health score
        disk_score = 100
        if metrics.disk_smart_pass is not None and not metrics.disk_smart_pass:
            disk_score = 0  # SMART failure = critical
            degraded_components.append("disk_smart")
        elif metrics.disk_wear_pct is not None:
            disk_score = self._inverse_range_score(
                metrics.disk_wear_pct,
                perfect=0,
                warning=thresholds["disk_wear_warning"],
                critical=thresholds["disk_wear_critical"])
        if metrics.nvme_media_errors and metrics.nvme_media_errors > 0:
            disk_score = min(disk_score, 20)  # Media errors are very bad
            degraded_components.append("nvme_media_errors")
        component_scores["disk_health"] = disk_score
        
        # Network health score
        network_score = 100
        if metrics.network_errors_1h is not None:
            network_score = self._inverse_range_score(
                metrics.network_errors_1h,
                perfect=0,
                warning=thresholds.get("network_errors_warning", 100),
                critical=thresholds.get("network_errors_critical", 1000))
            if network_score < 60:
                degraded_components.append("network")
        component_scores["network"] = network_score
        
        # BMC responsive score
        bmc_score = 100 if metrics.bmc_responsive else 0
        if not metrics.bmc_responsive:
            degraded_components.append("bmc")
        component_scores["bmc"] = bmc_score
        
        # Power/thermal score
        power_score = 100  # Simplified; could be more detailed
        component_scores["power_thermal"] = power_score
        
        # Weighted total
        total_score = sum(
            component_scores.get(comp, 100) * weight
            for comp, weight in weights.items()
        )
        
        return {
            "health_score": round(total_score, 1),
            "component_scores": component_scores,
            "degraded_components": degraded_components,
        }
    
    def _range_score(self, value, perfect, warning, critical):
        """Score where lower values are better (e.g., temperature)."""
        if value <= perfect:
            return 100
        if value >= critical:
            return 0
        if value <= warning:
            # Linear from 100 to 60 between perfect and warning
            return 100 - (value - perfect) / (warning - perfect) * 40
        # Linear from 60 to 0 between warning and critical
        return 60 - (value - warning) / (critical - warning) * 60
    
    def _inverse_range_score(self, value, perfect, warning, critical):
        """Score where higher values are worse (e.g., error counts)."""
        return self._range_score(value, perfect, warning, critical)


class AnomalyDetector:
    """EWMA-based anomaly detection for health score trends."""
    
    def __init__(self, alpha=0.3, z_threshold=3.0):
        self.alpha = alpha
        self.z_threshold = z_threshold
        self.ewma = {}   # machine_id -> ewma_score
        self.ewmv = {}   # machine_id -> ewma_variance
    
    def detect(self, machine_id: str, score: float) -> Optional[str]:
        if machine_id not in self.ewma:
            self.ewma[machine_id] = score
            self.ewmv[machine_id] = 0
            return None
        
        # Update EWMA
        prev_ewma = self.ewma[machine_id]
        new_ewma = self.alpha * score + (1 - self.alpha) * prev_ewma
        
        # Update EWMV (variance)
        diff = score - prev_ewma
        new_ewmv = (1 - self.alpha) * (self.ewmv[machine_id] + self.alpha * diff * diff)
        
        self.ewma[machine_id] = new_ewma
        self.ewmv[machine_id] = new_ewmv
        
        # Z-score for current observation
        std = math.sqrt(new_ewmv) if new_ewmv > 0 else 1
        z = (score - new_ewma) / std
        
        if z < -self.z_threshold:
            return f"ANOMALY: Score {score} is {abs(z):.1f} std below EWMA ({new_ewma:.1f})"
        
        return None
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| IPMI unreachable for a machine | Missing health data for that machine | Mark BMC as non-responsive (score 0 for BMC component). If BMC is unreachable for 3 consecutive checks, flag for manual investigation. |
| Health collector crashes | All machines in that AZ miss health checks | Multiple collector instances per AZ (active-active). K8s restarts failed pods. Reconciliation sweep (hourly) catches any machines that missed checks. |
| Scoring threshold misconfigured | False positives (too aggressive) or false negatives (too lax) | Thresholds are version-controlled and reviewed. New thresholds deployed via canary (applied to 5% of fleet first). Monitor ejection rate after threshold changes. |
| EWMA anomaly detector triggers false positive | Machine unnecessarily ejected | EWMA anomalies only trigger "watch list" escalation, not immediate ejection. Manual review required for anomaly-triggered maintenance. |

**Interviewer Deep-Dive Q&A:**

Q: How do you calibrate the weights and thresholds?
A: Initial weights are set based on component replacement cost and failure impact. GPU ECC errors get the highest weight (0.30) because a GPU failure makes an H100 server unusable for its primary purpose. Thresholds are calibrated by analyzing historical failure data: we correlate health metric values at time T with machine failures at time T+N. The goal is to set thresholds that catch 95% of failures with < 5% false positive rate. We review and update quarterly based on new failure data. Changes are deployed via canary (5% of fleet) and monitored for 1 week before fleet-wide rollout.

Q: How do you handle machines where some health data is unavailable (e.g., IPMI is down but the machine is functioning)?
A: Missing data is handled conservatively: if a metric is unavailable, we use the last known value (if < 1 hour old) or a neutral score of 50 (if stale). The BMC component gets a score of 0, which drags the total score down. This is intentional: an unreachable BMC means we cannot diagnose or remediate the machine remotely, which is a significant operational risk. The machine is not immediately ejected (score 50 is above the ejection threshold of 30), but it goes on the watch list for manual investigation.

Q: How do you prevent a batch firmware bug from ejecting a large number of machines simultaneously?
A: The auto-ejection system has a **rate limiter**: maximum 5 ejections per pool per hour. If the rate is exceeded, the system pauses ejections and alerts on-call: "Abnormal ejection rate in pool gpu_h100_8x_prod_use1 -- 12 machines flagged in last hour. Possible systemic issue." On-call investigates: if it is a firmware bug, they pause the firmware upgrade and rollback. The rate limiter prevents a runaway automation from draining an entire pool. Additionally, the auto-ejection logic excludes machines that were recently firmware-upgraded (within 24 hours) from automatic ejection -- they are flagged for manual review instead.

Q: How does the health score interact with the reservation platform's machine selection?
A: The reservation platform queries the Pool Manager for available machines. The Pool Manager returns only machines with `pool_state = 'active'` or `'warm'` and `health_score >= pool.health_warning_threshold` (default 90). Lower-scored machines are not offered for new reservations but may continue serving existing reservations (we do not preempt based on health unless the score drops below the eject threshold of 30). This creates a natural quality filter: users always get machines scored 90+, while borderline machines (60-89) are on the watch list and not assigned to new work.

Q: What happens if a machine's health score oscillates (e.g., 85 -> 55 -> 80 -> 50)?
A: The EWMA anomaly detector catches this: the high variance indicates instability. Additionally, the Pool Manager tracks a "stability score": the standard deviation of health scores over the last 24 hours. If the stability score exceeds a threshold (e.g., std > 15), the machine is flagged as "unstable" and moved to the watch list regardless of its current score. Unstable machines are not assigned to new reservations until they demonstrate 48 hours of stable scores.

Q: How do you handle GPU-specific failure modes (e.g., XID errors, NVLink errors)?
A: The health collector scrapes the `nvidia-smi` output and NVIDIA DCGM (Data Center GPU Manager) metrics from the nvidia_exporter running on each GPU machine. Specific failure modes: (1) XID errors (GPU hardware error): any XID error maps to a GPU health score of 20 (critical). Common XID codes (e.g., XID 79: GPU fallen off the bus) map to score 0. (2) NVLink errors: elevated NVLink replay counts indicate interconnect degradation. Score decreases proportionally. (3) GPU memory utilization stuck at 100% (possible hang): anomaly detected by the EWMA layer. (4) ECC errors: even correctable ECC errors on GPU memory are concerning and reduce score. Each GPU-specific metric has thresholds calibrated from NVIDIA's reliability guidelines and our own failure history.

Q: How do you handle the health check pipeline's own failure (e.g., Prometheus is down)?
A: The health collector has fallback data sources: (1) Primary: Prometheus scrape of node_exporter/nvidia_exporter. (2) Fallback 1: Direct IPMI query for hardware-level metrics (temperature, fan, power). (3) Fallback 2: SNMP query to the ToR switch for network port counters. If all sources fail for a machine, that machine gets a "stale health" flag. After 30 minutes of stale data, the machine goes on the watch list. After 1 hour, it is ejected from the available pool (conservatively: we cannot assess its health, so we do not risk assigning it to a user).

---

### Component: Capacity Forecast Engine

**Why it's hard:** Demand for infrastructure is not uniform -- it varies by machine type, time of day, day of week, and is influenced by external factors (project timelines, seasonal events, team growth). Over-provisioning wastes millions of dollars; under-provisioning blocks engineering teams. The forecast must be accurate enough to drive procurement decisions with 8-16 week lead times.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Static rules** | "Maintain 20% headroom at all times" | Simple | Ignores trends, wasteful during low demand | Small, stable fleets |
| **Linear regression** | Fit a line to utilization trend | Simple, explainable | Misses seasonality, nonlinear growth | Steady-growth scenarios |
| **ARIMA/SARIMA** | Time series model with auto-regressive and seasonal components | Captures trends and seasonality | Requires stationary data, sensitive to outliers | **Seasonal demand patterns** |
| **Prophet (Meta)** | Additive model: trend + seasonality + holidays | Easy to use, handles missing data, changepoints | Less accurate for rapidly changing demand | General purpose forecasting |
| **LSTM neural network** | Deep learning for sequence prediction | Can capture complex patterns | Requires large training data, black box | Very large datasets with complex patterns |

**Selected Approach:** SARIMA as the primary model, with Prophet as a secondary model for validation. Ensemble: average the two forecasts.

**Implementation Detail:**

```python
import pandas as pd
import numpy as np
from statsmodels.tsa.statespace.sarimax import SARIMAX
from prophet import Prophet
from datetime import timedelta

class CapacityForecastEngine:
    
    def forecast_demand(self, pool_id: str, weeks_ahead: int = 4) -> list:
        # Step 1: Load historical reservation data for this pool
        # Daily peak concurrent reservations over last 180 days
        history = self.load_reservation_history(pool_id, days=180)
        
        if len(history) < 30:
            # Insufficient data; fall back to simple growth rate
            return self._simple_growth_forecast(pool_id, weeks_ahead)
        
        # Step 2: SARIMA forecast
        sarima_forecast = self._sarima_forecast(history, weeks_ahead)
        
        # Step 3: Prophet forecast
        prophet_forecast = self._prophet_forecast(history, weeks_ahead)
        
        # Step 4: Ensemble (weighted average)
        ensemble = []
        for i in range(weeks_ahead * 7):
            sarima_val = sarima_forecast[i]
            prophet_val = prophet_forecast[i]
            # Weighted: 60% SARIMA (better for seasonal), 40% Prophet (better for trends)
            avg = 0.6 * sarima_val + 0.4 * prophet_val
            ensemble.append({
                "date": (pd.Timestamp.now() + timedelta(days=i+1)).strftime("%Y-%m-%d"),
                "predicted_demand": round(avg),
                "sarima": round(sarima_val),
                "prophet": round(prophet_val),
            })
        
        # Step 5: Compute confidence intervals
        for entry in ensemble:
            # Use SARIMA's confidence intervals as they are more statistically grounded
            entry["confidence_lower"] = max(0, entry["predicted_demand"] - 
                                             self._sarima_ci_width(history))
            entry["confidence_upper"] = entry["predicted_demand"] + \
                                         self._sarima_ci_width(history)
        
        return ensemble
    
    def _sarima_forecast(self, history: pd.Series, weeks: int) -> list:
        # SARIMA(1,1,1)(1,1,1,7) -- weekly seasonality
        model = SARIMAX(history, order=(1,1,1), seasonal_order=(1,1,1,7))
        fit = model.fit(disp=False)
        forecast = fit.forecast(steps=weeks * 7)
        return forecast.tolist()
    
    def _prophet_forecast(self, history: pd.Series, weeks: int) -> list:
        df = pd.DataFrame({"ds": history.index, "y": history.values})
        model = Prophet(
            yearly_seasonality=True,
            weekly_seasonality=True,
            daily_seasonality=False,
            changepoint_prior_scale=0.05
        )
        model.fit(df)
        future = model.make_future_dataframe(periods=weeks * 7)
        forecast = model.predict(future)
        return forecast.tail(weeks * 7)["yhat"].tolist()
    
    def generate_sizing_recommendation(self, pool_id: str) -> dict:
        pool = self.get_pool(pool_id)
        forecast = self.forecast_demand(pool_id, weeks_ahead=8)
        
        # Peak demand in the forecast period
        peak_demand = max(f["confidence_upper"] for f in forecast)
        
        # Current supply
        current_supply = self.get_pool_supply(pool_id)
        
        # Target: peak_demand / target_utilization
        # If target_utilization is 75%, we need peak_demand / 0.75 machines
        target_supply = math.ceil(peak_demand / (pool.target_utilization / 100))
        
        gap = target_supply - current_supply
        
        # Factor in expected attrition (machines failing and going to RMA)
        # Historical attrition rate: ~0.5% per month
        attrition_8w = math.ceil(current_supply * 0.005 * 2)  # 2 months
        
        total_needed = gap + attrition_8w
        
        # Procurement lead time check
        lead_time_weeks = self.get_lead_time(pool.machine_type)
        if total_needed > 0 and lead_time_weeks > 4:
            urgency = "URGENT" if gap > 0 else "PLANNED"
        else:
            urgency = "MONITOR"
        
        return {
            "pool_id": pool_id,
            "current_supply": current_supply,
            "peak_forecast_demand": peak_demand,
            "target_supply": target_supply,
            "gap": gap,
            "attrition_estimate": attrition_8w,
            "total_procurement_needed": max(0, total_needed),
            "lead_time_weeks": lead_time_weeks,
            "urgency": urgency,
            "recommendation": (
                f"Procure {max(0, total_needed)} additional {pool.machine_type} machines. "
                f"Lead time: {lead_time_weeks} weeks. "
                f"Order by {(pd.Timestamp.now() + timedelta(weeks=8 - lead_time_weeks)).strftime('%Y-%m-%d')} "
                f"to avoid capacity gap."
            ) if total_needed > 0 else "No procurement needed. Current supply meets forecast demand."
        }
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Forecast model produces unreasonable output (negative demand, 10x spike) | Bad procurement recommendation | Sanity checks: demand must be > 0 and < 2x historical peak. Outlier detection flags anomalous forecasts for human review. |
| Insufficient historical data for new pool | No forecast available | Fall back to simple growth model: current demand + 10% buffer. Alert capacity team to manually assess. |
| External event (e.g., large new project) not captured by historical data | Under-forecast | Integration with project planning tools: when a new project is approved that requires significant GPU capacity, the forecast is augmented with the known step-function increase. Capacity planning team reviews forecasts weekly. |

**Interviewer Deep-Dive Q&A:**

Q: How do you handle the 8-16 week lead time for GPU servers?
A: The forecast engine generates 8-week-ahead forecasts every week. The capacity planning dashboard shows: (1) Current utilization trend. (2) Forecasted demand with confidence intervals. (3) "Order by" date: the date by which procurement must start to receive machines before the projected capacity gap. For H100 GPUs with 12-week lead time, if the forecast shows a gap at week 8, the order-by date is "now - 4 weeks ago" -- meaning we are already late. This is why we run forecasts 8 weeks ahead: to give maximum lead time for procurement. For urgent shortfalls, we explore alternatives: (a) reallocate from lower-priority pools, (b) lease from cloud providers temporarily, (c) negotiate expedited delivery with vendors (at premium cost).

Q: How accurate are the forecasts?
A: We measure forecast accuracy as MAPE (Mean Absolute Percentage Error). Historical MAPE: ~15% for 1-week-ahead, ~25% for 4-week-ahead, ~35% for 8-week-ahead. These are typical for infrastructure demand forecasting. The confidence intervals capture the uncertainty. For procurement decisions, we use the upper bound of the 90% confidence interval (conservative: better to over-provision slightly than to be short). We backtest: every week, we compare the forecast from 4 weeks ago against actual demand and track accuracy over time. If accuracy degrades (MAPE > 40%), we retrain the model or investigate what changed.

Q: How do you handle seasonal demand (e.g., quarter-end ML training pushes)?
A: SARIMA captures weekly seasonality natively (the seasonal component with period 7). For quarterly seasonality, Prophet's yearly seasonality component captures it if we have > 1 year of data. For known events (quarter-end, all-hands hackathon, model training deadlines), we add "event markers" to the forecast as external regressors. These are provided by the capacity planning team based on the engineering calendar. The model learns the magnitude of demand increase associated with each event type.

Q: How do you balance over-provisioning (cost waste) vs. under-provisioning (blocked engineers)?
A: The `target_utilization` parameter controls this trade-off. At 75% target utilization, we maintain 25% headroom. This means: (1) On average, 25% of machines are idle (available for sudden demand). (2) This headroom absorbs forecast errors up to 33% (we have 25% extra, which covers 25/75 = 33% under-forecast). (3) The cost of 25% idle machines is significant (~$12.5M/year for 500 GPU machines at $100K each), but the cost of blocking a high-priority ML training run for 2 weeks is higher (engineer time, project delay). The target utilization is set per pool based on the business criticality: production GPU pools: 70% (more headroom), dev CPU pools: 85% (less critical).

Q: What happens if a forecast predicts a massive demand increase that turns out to be wrong?
A: Over-procurement is a risk. Mitigation: (1) Procurement happens in batches, not all at once. If forecast says "need 200 machines," we order 100 immediately and reassess before ordering the rest. (2) Machines can be reallocated between pools (a production GPU machine can be moved to staging if production demand does not materialize). (3) For leased machines (from cloud providers as temporary capacity), we can simply not renew the lease. (4) For purchased machines, the worst case is lower utilization, which is a cost issue but not an operational one. The capacity team reviews forecasts and procurement recommendations weekly, so human judgment is always in the loop.

---

### Component: Firmware Manager

**Why it's hard:** Firmware upgrades touch the lowest level of the hardware stack. A bad firmware flash can brick a machine (making it unrecoverable remotely). Different components (GPU, BIOS, BMC, NIC, NVMe) have different upgrade procedures, compatibility dependencies, and risk levels. Rolling upgrades across 100,000 machines must be controlled, observable, and rollback-capable.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Manual per-machine** | SSH in, run firmware flash tool | Full control | Does not scale beyond 100 machines | Emergency single-machine updates |
| **Ansible/Salt push** | Config management tool pushes firmware update | Scalable, idempotent | No built-in rollout control (batch, pause, abort) | Medium fleets with ops team |
| **Custom rolling upgrade engine** | Purpose-built system with batch control, health validation, auto-rollback | Full control over blast radius, integrated with health scoring | Must build and maintain | **Our use case: large fleet with heterogeneous firmware** |
| **Vendor-provided tools** | Dell OMSA, HPE OneView, NVIDIA NGC | Vendor-supported | Different tool per vendor, limited customization, may not integrate with our pool manager | Homogeneous vendor fleets |

**Selected Approach:** Custom rolling upgrade engine integrated with the Pool Manager's health scoring and the Reservation Platform's scheduling.

**Implementation Detail:**

```python
class FirmwareUpgradeEngine:
    
    def execute_upgrade_plan(self, plan_id: str):
        plan = self.db.get_upgrade_plan(plan_id)
        release = self.db.get_firmware_release(plan.release_id)
        
        # Get all machines that need this upgrade
        machines = self.db.get_machines_needing_upgrade(
            machine_type=release.machine_type,
            component=release.component,
            current_version_lt=release.version)
        
        # Validate compatibility
        for machine in machines:
            if not self._check_compatibility(machine, release):
                machine.upgrade_status = "incompatible"
                self.db.update_firmware_inventory(machine)
                plan.skipped_machines += 1
        
        eligible = [m for m in machines if m.upgrade_status != "incompatible"]
        
        # Sort: prioritize machines not currently in use
        eligible.sort(key=lambda m: (
            0 if m.state == "available" else 1,  # Available first
            0 if m.state == "cold" else 1,        # Cold pool second
        ))
        
        # Execute in batches
        batch_num = 0
        while eligible:
            batch = eligible[:plan.batch_size]
            eligible = eligible[plan.batch_size:]
            batch_num += 1
            
            # Check abort conditions
            if plan.failed_machines / max(plan.total_machines, 1) * 100 > plan.max_failure_pct:
                self._abort_plan(plan, f"Failure rate exceeded {plan.max_failure_pct}%")
                return
            
            # Wait for maintenance window
            if not self._in_maintenance_window(plan.maintenance_window):
                self._wait_for_maintenance_window(plan)
            
            # Process batch
            logger.info(f"Firmware upgrade plan {plan_id}: batch {batch_num}, "
                       f"machines: {[m.machine_id for m in batch]}")
            
            results = []
            for machine in batch:
                result = self._upgrade_single_machine(machine, release, plan)
                results.append(result)
                
                if not result.success:
                    plan.failed_machines += 1
                    # If this failure looks systemic (same error as previous failures),
                    # pause for human review
                    if self._is_systemic_failure(plan, result):
                        self._pause_plan(plan, 
                            f"Systemic failure pattern detected: {result.error}")
                        return
                else:
                    plan.completed_machines += 1
            
            self.db.update_upgrade_plan(plan)
            
            # Post-batch health validation
            # Wait 5 minutes, then check health scores of upgraded machines
            time.sleep(300)
            for result in results:
                if result.success:
                    health = self.health_engine.get_health_score(result.machine_id)
                    if health < 60:
                        logger.warning(
                            f"Machine {result.machine_id} health degraded to "
                            f"{health} after firmware upgrade. Rolling back.")
                        self._rollback_single_machine(result.machine_id, release)
                        plan.failed_machines += 1
                        plan.completed_machines -= 1
        
        plan.status = "completed"
        self.db.update_upgrade_plan(plan)
    
    def _upgrade_single_machine(self, machine, release, plan) -> UpgradeResult:
        try:
            # Step 1: Drain machine if in use (wait for reservation to end or preempt)
            if machine.state in ("reserved", "in_use"):
                # Do NOT preempt -- wait for reservation to end
                # (firmware upgrades are not urgent enough to preempt)
                self._schedule_upgrade_after_reservation(machine, release, plan)
                return UpgradeResult(machine.machine_id, success=False, 
                                     error="deferred_reservation_active")
            
            # Step 2: Remove from pool (mark maintenance)
            self.pool_manager.eject_for_maintenance(machine.machine_id, 
                reason=f"firmware_upgrade_{release.component}_{release.version}")
            
            # Step 3: Verify firmware image integrity
            image = self._download_firmware_image(release.firmware_image_url)
            if hashlib.sha256(image).hexdigest() != release.firmware_image_hash:
                raise IntegrityError("Firmware image hash mismatch")
            
            # Step 4: Upload and flash firmware via IPMI/BMC
            bmc = IpmiClient(machine.bmc_ip)
            
            if release.component == "bmc":
                bmc.update_firmware(image, component="bmc")
                # BMC update requires BMC reset (60s)
                time.sleep(60)
                bmc.reset_bmc()
                time.sleep(30)
            elif release.component == "bios":
                bmc.update_firmware(image, component="bios")
                # BIOS update requires full reboot
                bmc.power_cycle()
                self._wait_for_boot(machine, timeout_minutes=10)
            elif release.component == "gpu":
                # GPU firmware updated via in-band tool (nvidia-smi)
                self._ssh_execute(machine, 
                    f"nvidia-smi --gpu-firmware-update {release.version}")
                bmc.power_cycle()
                self._wait_for_boot(machine, timeout_minutes=10)
            elif release.component == "nic":
                # NIC firmware via vendor tool (mlxfwmanager for Mellanox)
                self._ssh_execute(machine,
                    f"mlxfwmanager --apply --fw-image /tmp/nic_fw.bin")
                bmc.power_cycle()
                self._wait_for_boot(machine, timeout_minutes=10)
            
            # Step 5: Validate new firmware version
            actual_version = self._get_firmware_version(machine, release.component)
            if actual_version != release.version:
                raise ValidationError(
                    f"Expected {release.version}, got {actual_version}")
            
            # Step 6: Update inventory
            self.db.update_firmware_inventory(
                machine.machine_id, release.component, release.version, "current")
            
            # Step 7: Return to pool
            self.pool_manager.return_from_maintenance(machine.machine_id)
            
            return UpgradeResult(machine.machine_id, success=True)
            
        except Exception as e:
            logger.error(f"Firmware upgrade failed for {machine.machine_id}: {e}")
            
            # Attempt rollback
            if release.rollback_version:
                try:
                    self._rollback_single_machine(machine.machine_id, release)
                except Exception as rollback_error:
                    logger.critical(
                        f"ROLLBACK FAILED for {machine.machine_id}: {rollback_error}")
                    # Machine stays in maintenance state, manual intervention needed
            
            return UpgradeResult(machine.machine_id, success=False, error=str(e))
    
    def _is_systemic_failure(self, plan, result) -> bool:
        """Detect if failures have a common pattern (same error, same rack, etc.)"""
        recent_failures = self.db.get_recent_failures(plan.id, limit=5)
        if len(recent_failures) < 3:
            return False
        
        # Check if all recent failures have the same error message
        error_messages = [f.error for f in recent_failures]
        if len(set(error_messages)) == 1:
            return True  # All same error = likely systemic
        
        # Check if all failures are in the same rack
        racks = [self.db.get_machine_rack(f.machine_id) for f in recent_failures]
        if len(set(racks)) == 1:
            return True  # All same rack = likely rack-level issue
        
        return False
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Firmware flash bricks machine (unresponsive BMC) | Machine completely unrecoverable remotely | Batch size limits blast radius (max 10 at a time). For critical firmware (BIOS, BMC), test on 3 machines manually before automated rollout. Bricked machines require physical RMA. |
| Firmware version incompatibility | Machine boots but GPU/NIC not recognized | Pre-upgrade compatibility check validates BIOS version against firmware release requirements. Post-upgrade health check catches issues. Auto-rollback to previous version. |
| Power failure during firmware flash | Corrupted firmware image | Most modern BMC/BIOS have dual-bank firmware: if one bank is corrupted, the other bank boots. If dual-bank is not available, machine is bricked (RMA). Risk mitigated by performing upgrades during stable power windows and using UPS-backed power. |
| Rolling upgrade takes too long (weeks) | Security vulnerability exposure | Prioritize security-critical firmware. For critical CVEs, increase batch size (with approval) and extend maintenance windows. Accept higher risk of batch failure for urgent updates. |

**Interviewer Deep-Dive Q&A:**

Q: How do you handle firmware upgrades for machines that are currently in use by tenants?
A: We never interrupt a tenant's workload for firmware upgrades (unlike preemption for P0 reservations, firmware upgrades are not urgent enough). The firmware manager checks the reservation schedule: if the machine has a reservation ending within the maintenance window, it is queued for upgrade after the reservation ends. If the reservation extends beyond the maintenance window, the machine is skipped and scheduled for the next window. For critical security patches (CVEs), we communicate with affected tenants to request early release, but never force it. Machines without active reservations are upgraded first.

Q: How do you roll back a firmware upgrade that caused problems?
A: Each firmware release record includes a `rollback_version`. If post-upgrade health check fails (health score drops below 60 within 5 minutes of upgrade), the engine automatically flashes the rollback version. If the rollback also fails (rare), the machine is marked `failed` and enters the RMA/manual intervention pipeline. For fleet-wide rollbacks (systemic issue detected), the engine pauses the upgrade plan and sends an alert. The operator can initiate a fleet rollback: the engine re-flashes the rollback version on all machines that were upgraded as part of the current plan.

Q: How do you validate firmware compatibility before upgrading?
A: The `firmware_releases` table has a `compatible_bios_versions` field (JSON array of BIOS versions that the new firmware is compatible with). Before flashing, the engine checks: (1) Current BIOS version is in the compatible list. (2) If the release requires a BIOS update first, the BIOS update is planned as a prerequisite. (3) NIC firmware compatibility with the SDN controller version is checked (Mellanox NIC firmware must match OVS/OVN requirements). (4) GPU firmware compatibility with the NVIDIA driver version on the OS image. These checks are automated: the engine queries the firmware inventory for each machine's current versions and validates against the release's compatibility matrix.

Q: How do you track firmware compliance across the fleet?
A: The `firmware_inventory` table stores the current version of every component on every machine. The `firmware_releases` table stores the target version for each machine type. The compliance report (`GET /api/v1/firmware/compliance`) computes: compliant (current >= target), non-compliant (current < target), in_progress (update running). This is displayed on the fleet management dashboard as a compliance percentage. The compliance metric is tracked over time: "GPU firmware compliance: 97% (150 machines pending upgrade, 12-week upgrade plan in progress)." Compliance drops below 95% trigger an alert to the fleet ops team.

Q: How do you handle vendor-specific firmware management?
A: Each vendor has a firmware adapter module: (1) **Dell**: Uses Redfish API for BMC/BIOS updates, Dell Update Package (DUP) for component firmware. (2) **HPE**: Uses iLO REST API for BMC, Smart Update Manager (SUM) for component firmware. (3) **Supermicro**: Uses IPMI-based firmware upload. (4) **NVIDIA**: Uses `nvidia-smi` or NVIDIA Firmware Update Utility (nvflash) for GPU firmware. (5) **Mellanox/Broadcom**: Uses `mlxfwmanager` or `ethtool` for NIC firmware. The Firmware Manager's adapter pattern abstracts vendor-specific commands behind a common interface: `upload_firmware(image, component)`, `get_firmware_version(component)`, `reset_component(component)`.

Q: What is the blast radius if a firmware upgrade goes wrong?
A: Controlled by batch_size (default 10, max 50). In the absolute worst case (batch_size 10, all 10 machines bricked), we lose 10 machines from a pool of 500 (2%). The auto-pause mechanism detects the failure pattern after 3 consecutive failures and stops the upgrade. The blast radius is: min(batch_size, 3 + batch_size * max_failure_pct). With default settings (batch_size=10, max_failure_pct=5%), the worst case before auto-pause is ~15 machines. This is an acceptable risk for a 100K-machine fleet.

---

## 7. Scheduling & Resource Management

### Pool Sizing Algorithm

```python
def compute_target_pool_size(pool, forecast, attrition_rate=0.005):
    """
    Target = peak_demand / target_utilization + warm_buffer + attrition_buffer
    
    Example for gpu_h100_8x_prod_use1:
      peak_demand = 420 (from 4-week forecast, 90% CI upper bound)
      target_utilization = 0.75
      warm_buffer = 20 (pre-provisioned for instant assignment)
      attrition_rate = 0.5% per month
      current_size = 500
      
      base_target = 420 / 0.75 = 560
      attrition_buffer = 560 * 0.005 * 2 = 5.6 ≈ 6 (2 months ahead)
      target = 560 + 6 = 566
      
      gap = 566 - 500 = 66 machines to procure
    """
    peak_demand = max(f["confidence_upper"] for f in forecast)
    base_target = math.ceil(peak_demand / (pool.target_utilization / 100))
    attrition_buffer = math.ceil(base_target * attrition_rate * 2)  # 2-month horizon
    return base_target + attrition_buffer
```

### Warm Pool Management

The warm pool contains pre-provisioned machines: OS installed, network configured, health-checked, ready for assignment in under 2 minutes (just needs VLAN switch and SSH key injection).

```
Warm pool lifecycle:
1. Machine enters pool as 'cold' (powered off).
2. Pool Manager checks warm pool level vs target.
3. If warm pool < target: promote cold -> warm.
   a. Power on via IPMI.
   b. PXE boot with pre-configured image.
   c. Run health check.
   d. If healthy: mark 'warm'. If not: mark 'failed'.
4. When a reservation is confirmed, a warm machine is assigned instantly.
5. Warm pool replenished from cold pool.
6. If cold pool is empty, new machines are procured.
```

### Drain Management

When a pool needs to be drained (rack maintenance, hardware refresh):

```python
def drain_pool(pool_id, deadline):
    pool = get_pool(pool_id)
    pool.state = "draining"
    save_pool(pool)
    
    # Block new reservations for machines in this pool
    reservation_service.block_pool(pool_id)
    
    # Check active reservations
    active_reservations = reservation_service.get_active_for_pool(pool_id)
    
    for reservation in active_reservations:
        if reservation.end_time <= deadline:
            # Will end naturally before deadline -- no action needed
            continue
        
        # Reservation extends beyond deadline -- negotiate with tenant
        notification_service.send(reservation.tenant_id,
            f"Pool {pool.name} is being drained for maintenance by {deadline}. "
            f"Your reservation {reservation.id} ends after this deadline. "
            f"Please release early or we will migrate your workload to another pool.")
        
        # If tenant does not respond within 48 hours of deadline:
        # Offer replacement in another pool. If no replacement available,
        # extend deadline or escalate to management.
```

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Instances |
|-----------|-----------------|-----------|
| Health Data Collector | Per-AZ deployment; each collector handles ~7K machines | 15 instances (5 regions x 3 AZs) |
| Health Scoring Engine | Consumes from Kafka; scale with partition count | 6-12 instances |
| Pool Manager Service | Stateless Java service | 4-8 replicas |
| Capacity Forecast Engine | Batch computation; single leader per region | 5 instances (1 per region) |
| Firmware Manager | Single leader (etcd election); handles upgrades sequentially per batch | 2-3 instances (active-standby) |
| RMA Service | Low volume; single instance per region sufficient | 5 instances |

### Database Scaling

- MySQL: single primary + 2 replicas. Health check data is the highest volume table; partitioned by month, old partitions dropped after 90 days (archived to S3 first).
- Prometheus/VictoriaMetrics: 2-week retention for full resolution, then downsampled to 1-hour resolution for 1 year. VictoriaMetrics cluster mode for HA.

### Caching

| Layer | What to cache | Strategy | Tool | Eviction | TTL | Invalidation |
|-------|---------------|----------|------|----------|-----|--------------|
| Pool membership | pool_id -> {warm: [m1,m2], cold: [m3,m4], active: [...]} | Write-through | Redis | N/A | N/A | Updated on every pool change |
| Health scores | machine_id -> latest health score | Write-through | Redis | N/A | 15 min TTL | Updated on every health check |
| Firmware inventory | machine_id -> {component: version} | Cache-aside | Redis | LRU | 1 hour | Invalidated on firmware update |
| Machine metadata | machine_id -> specs | Cache-aside | Redis | LRU | 5 min | Invalidated on CMDB sync |

**Interviewer Deep-Dive Q&A:**

Q: How does the Health Data Collector handle 100,000 machines without falling behind?
A: Each collector handles ~7,000 machines (100K / 15 AZs). At a 15-minute check interval, that is ~8 checks/sec per collector. Each check involves: 1 IPMI query (~200ms), 1 Prometheus scrape (~50ms), 1 SNMP query (~100ms). With 8 concurrent goroutines per collector, we process 8 machines simultaneously, completing all 7,000 in ~875 seconds (~14.6 minutes). This fits within the 15-minute window with a small buffer. If we add more machines per AZ, we add more collector instances (horizontal scaling via Kafka partitioning: health check tasks are distributed across collector instances).

Q: How do you handle the storage requirements for health check history (430 GB for 90 days)?
A: Partitioned MySQL tables with monthly partitions. Old partitions (> 90 days) are archived to S3 in Parquet format (compressed from 430 GB to ~50 GB) for long-term analysis. Current partitions (~150 GB for 30 days) fit on NVMe storage with the MySQL primary. For time-series queries (health score over time), we use VictoriaMetrics (Prometheus-compatible, designed for high cardinality), which stores 100K machines x 10 metrics x 96 checks/day = ~96M data points/day at ~10 bytes/point = ~1 GB/day uncompressed, but VictoriaMetrics compresses ~10x, so ~100 MB/day or ~36 GB/year. Very manageable.

Q: What happens if the firmware manager crashes mid-upgrade?
A: The firmware manager persists upgrade plan state to MySQL after each machine upgrade. On restart (or leader re-election), it loads the plan state and resumes from the last incomplete machine. Machines that were mid-flash when the crash occurred are in an uncertain state: the manager re-checks their firmware version via IPMI. If the version matches the target, the upgrade succeeded (mark complete). If it matches the old version, the flash was interrupted but the old firmware is intact (retry). If the version is unknown or the BMC is unresponsive, the machine is marked `failed` and requires manual investigation.

Q: How do you prioritize which pools to upgrade first?
A: Priority order: (1) Security-critical firmware (CVEs with known exploits): all pools, starting with production. (2) Stability-critical firmware (fixes for known crash/hang issues): production pools first, then staging, then dev. (3) Performance firmware (optimization improvements): staging first (for testing), then production. (4) Optional firmware (minor improvements): during scheduled maintenance windows only. The firmware manager's `severity` field on the release drives the priority. Critical releases bypass the normal maintenance window and can be executed during any approved window.

Q: How do you integrate with vendor RMA portals that have different APIs?
A: The RMA Service has adapter modules per vendor: (1) **Dell TechDirect**: REST API for case creation, part dispatch tracking, return label generation. (2) **HPE**: SOAP/REST hybrid API for warranty check, case management. (3) **Supermicro**: Email-based RMA process (the adapter sends structured emails and parses responses). (4) **NVIDIA**: Web portal with limited API (the adapter uses browser automation for case creation, then switches to API for tracking). Each adapter implements a common interface: `create_ticket(machine_info, failure_info) -> ticket_id`, `get_status(ticket_id) -> status`, `get_tracking(ticket_id) -> shipping_info`. The RMA dashboard aggregates across all vendors.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios & Mitigations

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| Health collector down in one AZ | ~7K machines miss health checks | K8s liveness probe | Pod restart. Reconciliation sweep catches missed machines within 1 hour. | 15s (pod restart) | 15 min (missed check) |
| Health scoring produces incorrect scores (bug) | Machines incorrectly ejected or kept | Alert on ejection rate anomaly | Rate limiter caps ejections at 5/pool/hour. Canary scoring: new logic runs in shadow mode for 1 week before becoming authoritative. | 0 (shadow mode catches before production) | 0 |
| Pool Manager MySQL down | Pool operations blocked | MySQL orchestrator | Semi-sync replica promotion. Redis cache continues serving pool membership reads. | 30s | 0 |
| Firmware upgrade bricks machines | Machines unrecoverable | Systemic failure detection (3 consecutive fails) | Auto-pause upgrade plan. Alert on-call. Maximum blast radius: batch_size + 2 (before auto-pause triggers). | Minutes (auto-pause) | N/A (hardware RMA) |
| RMA vendor portal down | Cannot create/track RMA tickets | HTTP error rate monitoring | Queue RMA requests, retry when vendor portal recovers. Non-blocking for operations (machine stays in maintenance, just the ticket is pending). | Hours (depends on vendor) | 0 (queued in DB) |
| Capacity forecast model diverges from reality | Bad procurement recommendations | Weekly backtest: compare forecast vs actual | Alert if MAPE > 40%. Fall back to simple growth model. Human review of all procurement recommendations. | N/A (advisory, not automated) | N/A |

### Automated Recovery

- **Self-healing warm pool**: If a warm machine fails health check, it is automatically replaced from the cold pool. If the cold pool is empty, an alert fires for capacity replenishment.
- **Health check reconciliation**: Hourly sweep identifies machines that missed their last 3 health checks. These machines are flagged for investigation (possible network issue, possible BMC failure).
- **Firmware compliance drift detection**: Daily scan identifies machines whose firmware has regressed (e.g., after BMC reset to factory defaults). These machines are queued for re-upgrade.

---

## 10. Observability

### Key Metrics

| Metric | Type | Tool | Alert Threshold | Business Impact |
|--------|------|------|-----------------|-----------------|
| `pool.utilization` | Gauge (per pool) | Prometheus | > 90% for 1h or < 50% for 24h | Capacity crunch or waste |
| `pool.warm_pool.size` | Gauge (per pool) | Prometheus | < 50% of target | Slow provisioning for new reservations |
| `machine.health_score.avg` | Gauge (per pool) | Prometheus | < 85 | Fleet health degrading |
| `machine.ejection_rate` | Counter (per pool) | Prometheus | > 5/pool/hour | Systemic hardware issue |
| `firmware.compliance_pct` | Gauge (per machine type) | Prometheus | < 90% | Security/stability risk |
| `firmware.upgrade.failure_rate` | Counter (per plan) | Prometheus | > 5% | Upgrade plan issue |
| `rma.open_tickets` | Gauge (per vendor) | Prometheus | > 20 for any vendor | RMA backlog growing |
| `rma.resolution_time_days` | Histogram | Prometheus | p50 > 14 days | Vendor SLA breach |
| `capacity.forecast.gap` | Gauge (per pool) | Prometheus | > 0 for any pool at 8-week horizon | Procurement needed |
| `health_check.latency` | Histogram | Prometheus | p99 > 10s | Health pipeline slow |

### Distributed Tracing
Health check pipeline: Collector -> Scoring Engine -> Kafka -> Pool Manager. Trace ID per machine per check cycle.

### Logging
Structured JSON with machine_id, pool_id, component, score, action. Health scoring decisions logged with full breakdown for auditability.

---

## 11. Security

### Access Control
- Pool management APIs: restricted to `fleet_operator` and `admin` roles.
- Health data: read access for `engineer` role (own team's machines). Write access for health collector service account only.
- Firmware management: `firmware_operator` role (specialized). Upgrade plan approval requires `fleet_admin`.
- RMA: `fleet_operator` role.

### BMC/IPMI Security
- IPMI credentials per-machine stored in Vault. Rotated quarterly.
- BMC network is on a dedicated management VLAN, not routable from tenant or general corporate network.
- All IPMI commands logged in the audit trail.
- Firmware images verified via SHA-256 hash before flashing.

### Firmware Supply Chain
- Firmware images downloaded from vendor-authenticated HTTPS endpoints.
- Images stored in S3/GCS with integrity hashes.
- Before flashing, the hash is re-verified.
- Firmware signing: where vendors provide signed images (NVIDIA, Intel), the signature is verified before flashing.

---

## 12. Incremental Rollout Strategy

### Health Scoring Changes

1. **Shadow mode (2 weeks)**: New scoring logic runs alongside old logic. Both scores computed, old score used for decisions. Log discrepancies.
2. **Canary (1 week)**: Apply new scoring to 5% of machines (one pool). Monitor ejection rate, false positive rate.
3. **Gradual rollout**: 25% -> 50% -> 100% over 2 weeks.

### Firmware Upgrade Rollout

1. **Lab validation (1 week)**: Upgrade 3 machines in the lab environment. Validate all components, run stress tests for 48 hours.
2. **Staging pool (1 week)**: Upgrade 20 machines in staging. Monitor health scores for 1 week.
3. **Production canary (1 batch)**: Upgrade 10 production machines. Monitor for 48 hours.
4. **Rolling production**: Automated batches during maintenance windows. Auto-pause on failure.

### Pool Configuration Changes

- Pool threshold changes (health thresholds, utilization targets) are versioned and reviewed.
- Changes applied to one pool first, monitored for 48 hours, then rolled to similar pools.

**Rollout Q&A:**

Q: How do you roll out a new health scoring algorithm without disrupting pool management?
A: Shadow mode: the new scoring engine publishes to a separate Kafka topic (`machine.health.v2`). The Pool Manager continues consuming `machine.health.v1`. A comparison service reads both topics and logs discrepancies (machines that would be ejected under v2 but not v1, and vice versa). After 2 weeks of analysis, we review the discrepancies with the fleet ops team. If the new algorithm catches real issues that the old one missed (and does not have excessive false positives), we switch the Pool Manager to consume from `machine.health.v2`. The switch is a single Kafka consumer group configuration change, instantly rollback-able.

Q: How do you handle firmware upgrades across multiple data centers with different maintenance windows?
A: Each region has its own firmware upgrade plan instance. Plans are created per-region with region-specific maintenance windows (aligned with local off-peak hours). A global firmware compliance dashboard shows cross-region progress. The firmware manager's `maintenance_window` field supports flexible expressions (e.g., "tue_wed_02:00-06:00_utc" for US regions, "sat_sun_22:00-06:00_jst" for Japan). Plans in different regions execute independently and concurrently.

Q: How do you test a new vendor RMA integration without creating real tickets?
A: Each vendor adapter has a `dry_run` mode that validates the request payload against the vendor's API schema without actually creating a ticket. For Dell TechDirect, we use their sandbox environment. For vendors without sandboxes (Supermicro), we use a mock server that simulates the vendor's API responses. Integration tests run against these sandboxes/mocks in CI. Before going live, we create 1 real ticket manually via the adapter and verify end-to-end.

Q: How do you handle a capacity forecast that requires procuring more machines than the budget allows?
A: The capacity forecast recommendation is advisory. It goes to the fleet ops team, who evaluate: (1) Can we reallocate from lower-priority pools? (2) Can we improve utilization in existing pools (reduce warm pool buffer, increase target utilization)? (3) Can we use cloud burst (temporary lease from AWS/GCP)? (4) Can we defer non-critical workloads? (5) If procurement is truly needed, the recommendation goes to finance with a cost-benefit analysis. The forecast engine does not make purchasing decisions -- it provides data for human decision-makers.

Q: How do you handle a situation where health check data is being spoofed by a compromised machine?
A: Defense in depth: (1) **IPMI data is out-of-band**: The BMC reports hardware telemetry independently of the OS. A compromised OS cannot spoof IPMI data (the BMC is a separate embedded system). (2) **Cross-validation**: Health scores combine IPMI data (trusted) with in-band agent data (less trusted). If the in-band agent reports "everything is fine" but IPMI shows critical temperatures, the IPMI data wins. (3) **Network switch data**: Port error counters from the ToR switch are independent of the machine. (4) **Anomaly detection**: If a machine's in-band metrics suddenly diverge from its IPMI metrics, it is flagged for investigation. (5) **Agent integrity**: The health agent binary is signed and verified at boot. If tampered with, the agent fails attestation and the machine is flagged.

---

## 13. Trade-offs & Decision Log

| Decision | Option A | Option B | Option C | Chosen | Specific Reason |
|----------|----------|----------|----------|--------|-----------------|
| Health scoring | Simple threshold | Weighted linear score | ML anomaly detection | **Weighted linear + EWMA anomaly** | Weighted score is transparent and configurable. EWMA catches gradual degradation. ML is a future enhancement. |
| Health data storage | MySQL only | Prometheus only | MySQL + Prometheus | **MySQL + Prometheus** | MySQL for relational queries (machine X's health at time T with component breakdown). Prometheus for time-series dashboards and alerting (fleet-wide health trends). |
| Capacity forecasting | Static rules (20% buffer) | SARIMA | Prophet | **SARIMA + Prophet ensemble** | Ensemble reduces model-specific errors. SARIMA captures seasonality. Prophet captures trend changepoints. Simple rules used as fallback when insufficient data. |
| Firmware upgrade orchestration | Ansible | Custom engine | Vendor tools | **Custom engine** | Integrated with health scoring (auto-rollback on health degradation). Batch control, blast radius limiting, systemic failure detection. Ansible lacks these features out of the box. |
| Warm pool strategy | No warm pool (provision on demand) | Fixed warm pool size | Dynamic warm pool (based on forecast) | **Fixed warm pool target with dynamic adjustment** | Fixed target provides baseline fast-provisioning capacity. Target adjusted weekly based on demand forecast. Fully dynamic is over-engineered for our scale. |
| Health check interval | 1 min | 5 min | 15 min | **15 min (default), 5 min (watch list)** | 15 min is sufficient for gradual degradation detection. More frequent checks for watched machines. 1 min would overwhelm BMC/IPMI at scale. |
| RMA integration | Manual (email/portal) | API integration | Full automation | **API integration where available, manual fallback** | Dell and HPE have APIs. Supermicro is manual. Full automation requires vendor cooperation we do not control. |

---

## 14. Agentic AI Integration

### Where AI Adds Value

1. **Root cause analysis for fleet-wide failures**: "12 machines ejected in the last 2 hours, all in rack R47, all with GPU ECC errors. Root cause: power supply voltage fluctuation on PDU #3 in rack R47. Recommendation: Replace PDU, move affected machines to adjacent racks."

2. **Predictive maintenance**: ML model trained on historical health score trajectories predicts which machines will fail within 7 days (before they reach the ejection threshold). These machines are proactively scheduled for maintenance during off-peak hours, avoiding disruption.

3. **Firmware impact analysis**: Before approving a firmware upgrade plan, the AI agent analyzes: (a) Known issues with this firmware version (CVE databases, vendor release notes). (b) Community reports (Reddit, vendor forums). (c) Our own staging test results. Produces a risk assessment: "This GPU firmware version has 3 reports of NVLink instability on systems with BIOS version < 2.1.5. 15% of our fleet has BIOS < 2.1.5. Recommendation: Upgrade BIOS first for those machines."

4. **Capacity optimization**: AI agent identifies pools with consistently low utilization and recommends consolidation. "Pool cpu_general_staging_use1 has averaged 35% utilization for 90 days. Recommendation: Merge with cpu_general_staging_use1b (42% utilization) and decommission 30 machines, saving $150K/year."

### Guard Rails
- Root cause analysis: recommendation only; human confirms before action.
- Predictive maintenance: machine goes on watch list, not immediately ejected.
- Firmware risk assessment: advisory; firmware operator makes the approval decision.
- Pool consolidation: recommendation only; requires fleet admin approval.

---

## 15. Complete Interviewer Q&A Bank

**Q: How do you handle a situation where 5% of your GPU fleet suddenly shows degraded health scores?**
A: (1) The auto-ejection rate limiter (5/pool/hour) prevents mass ejection. (2) The alert fires: "Abnormal ejection rate in pool gpu_h100_8x_prod_use1." (3) On-call investigates: correlate degraded machines by rack, firmware version, power domain, procurement batch. (4) If correlation found (e.g., all from the same firmware batch), pause the auto-ejection and investigate the root cause. (5) If firmware bug: roll back firmware for affected machines. (6) If hardware batch issue: engage vendor for batch RMA. (7) During investigation, affected machines are marked "under_investigation" (not available for new reservations but existing reservations continue). (8) Post-incident: review whether health scoring thresholds need adjustment.

**Q: How do you decide when to RMA a machine vs. repair it in-house?**
A: Decision tree: (1) **Component replaceable in data center** (DIMM, disk, fan, power supply): Replace in-house. Our DC ops team stocks common parts. Mean time to repair: 4 hours. (2) **Component requires vendor** (motherboard, GPU, NIC): RMA with vendor. We do not repair motherboards or GPUs. (3) **Repeated failures** (same machine, 3+ failures in 6 months): Decommission. The machine is unreliable regardless of which component is replaced. (4) **Under warranty**: Always RMA (vendor covers cost). (5) **Out of warranty**: Cost analysis. If repair cost > 30% of machine value, decommission. The RMA service tracks the decision criteria and outcome for each ticket, building a knowledge base for future decisions.

**Q: How do you manage pool fragmentation (many small pools with low utilization)?**
A: The capacity forecast engine flags fragmented pools: pools with < 50% utilization for > 30 days. Recommendations: (1) **Merge small pools**: If two pools of the same machine type in the same region have low utilization, merge them. (2) **Resize**: Reduce pool target size and reassign excess machines to higher-demand pools. (3) **Decommission**: If demand has permanently decreased for a machine type (e.g., A100 replaced by H100), drain and decommission the pool. We run a quarterly "pool rationalization" review where the fleet ops team evaluates all pools against demand forecasts.

**Q: How do you handle a vendor that has unacceptable RMA turnaround times?**
A: (1) Track RMA resolution time per vendor as a key metric. (2) If p50 resolution time exceeds SLA (e.g., 14 days for critical, 30 days for normal), escalate with vendor's account team. (3) Maintain a buffer of spare machines per machine type (5% of pool) to absorb RMA delays. (4) For chronic slow vendors, increase the spare buffer and factor vendor reliability into procurement decisions (prefer vendors with better RMA track records). (5) For critical production machines, maintain advance-replacement contracts: vendor ships replacement part before receiving the defective part.

**Q: How do you prevent unauthorized firmware changes?**
A: (1) Firmware upgrade plans require explicit approval from `fleet_admin` role. (2) The firmware manager only flashes images from the approved `firmware_releases` table (cannot upload arbitrary firmware). (3) All firmware images are hash-verified before flashing. (4) BMC/IPMI credentials are in Vault, not accessible to general users. (5) The BMC management network is isolated from tenant and corporate networks. (6) All firmware operations are logged in the audit trail with actor, timestamp, target machine, old version, new version. (7) Post-upgrade, health check validates the machine is functioning correctly.

**Q: How do you handle different hardware generations in the same pool?**
A: Within a pool, all machines have the same `machine_type` (which defines the capability class: e.g., gpu_h100_8x). Within that type, there may be hardware revisions (Rev A vs Rev B motherboards, different NVMe vendors). The pool manager treats them identically for scheduling purposes (same compute capability). However: (1) Firmware management tracks per-revision firmware compatibility. (2) Health scoring thresholds may differ by revision (Rev A may have different normal temperature ranges). (3) If a specific revision has a known issue, it can be tagged and filtered in the pool manager. The key principle: pools are organized by capability (what the machine can do), not by hardware SKU (what the machine is made of).

**Q: How does the warm pool interact with the reservation platform?**
A: When the reservation platform confirms a reservation, it requests machines from the Pool Manager. The Pool Manager first checks the warm pool (machines already OS-installed and network-configured). If warm machines are available, they are assigned immediately -- provisioning time drops from 15 minutes to < 2 minutes (just VLAN switch and SSH key injection). If the warm pool is empty, the reservation platform falls back to standard provisioning (PXE boot from cold pool). The warm pool is transparent to the reservation platform -- it simply gets machines faster when warm ones are available. The warm pool target is set based on the expected reservation rate: for a pool that creates ~10 new reservations/day, a warm pool of 5 machines provides 12-hour buffer.

**Q: How do you handle the discovery of a zero-day vulnerability in BMC firmware that affects your entire fleet?**
A: (1) Immediately: Verify the vulnerability is exploitable in our environment (BMC network is isolated, reducing attack surface). (2) If exploitable: escalate to P1 incident. (3) Contact vendor for emergency firmware patch. (4) While waiting for patch: apply network-level mitigations (additional firewall rules on BMC VLAN, disable vulnerable BMC features if possible). (5) When patch available: fast-track firmware upgrade with larger batch sizes (50 instead of 10) and extended maintenance windows. Accept higher risk of upgrade failure vs. security risk. (6) If no patch available within 48 hours: evaluate disabling BMC network access entirely (lose remote management capability but eliminate vulnerability). (7) Post-incident: review BMC network isolation to ensure it truly is air-gapped from tenant and external networks.

**Q: How do you measure the effectiveness of your health scoring system?**
A: Two key metrics: (1) **Precision**: Of machines ejected by the scoring system, what percentage actually had hardware issues? Target: > 90%. Measured by post-ejection diagnosis (was the hardware issue confirmed?). Low precision means too many false positives. (2) **Recall**: Of machines that experienced hardware failures, what percentage were flagged by the scoring system before failure? Target: > 80%. Measured by analyzing machines that failed in production (user-reported or catastrophic failure) and checking if their health score was declining before the failure. Low recall means the scoring is missing real issues. We track both metrics monthly and tune thresholds to optimize the precision-recall trade-off.

**Q: How do you handle capacity planning across multiple machine types with different procurement lead times?**
A: Each machine type has its own forecast model and procurement lead time in the capacity forecast engine. The capacity dashboard shows all pools on a single view, sorted by urgency (closest to capacity gap considering lead time). Example: H100 GPUs (12-week lead time) need 8+ weeks of forecast horizon. CPU servers (4-week lead time) need only 4 weeks. The "order by" date is computed as: `forecast_gap_date - lead_time_weeks`. Pools where the order-by date is in the past are marked "URGENT." The fleet ops team reviews the dashboard weekly and initiates procurement for any urgent or upcoming shortfalls. For new machine types with no history (e.g., H200 GPUs), we use demand signals from project planning tools and engineering leadership input, supplemented with the simple growth model as the forecast engine accumulates data.

**Q: How do you ensure the Pool Manager does not accidentally drain an entire production pool?**
A: Safety mechanisms: (1) **Minimum pool size enforcement**: The drain operation refuses to proceed if draining would reduce the pool below `min_size`. (2) **Rate-limited ejection**: Maximum 5 ejections per pool per hour. (3) **Approval required for drain**: Draining a production pool requires `fleet_admin` approval with a reason and deadline. (4) **Gradual drain**: Machines are removed one at a time as their reservations end. New reservations are blocked but existing ones are honored. (5) **Abort capability**: Drain can be cancelled at any time, re-enabling new reservations. (6) **Alert on low pool size**: Alert fires if any production pool drops below 80% of target size.

**Q: How do you integrate the Pool Manager with Kubernetes for the control plane?**
A: The Pool Manager itself runs on Kubernetes (management cluster). The managed machines are bare-metal -- they do not run Kubernetes (they run tenant workloads directly). The integration points are: (1) Pool Manager pods have HPA based on health event processing rate. (2) Health Data Collectors run as DaemonSet-like deployments (one per AZ, not one per node). (3) Firmware Manager uses Kubernetes CronJobs for scheduled maintenance window checks. (4) All services use Kubernetes secrets (via Vault CSI driver) for BMC credentials. (5) Network policies isolate Pool Manager pods: they can reach MySQL, Redis, Kafka, and the BMC management network, but nothing else.

**Q: How do you handle the transition when decommissioning an old machine type (e.g., retiring all A100 machines)?**
A: A phased approach: (1) **Announce deprecation**: 6 months before, communicate to all tenants that A100 pools will be retired. (2) **Block new long-term reservations**: Stop accepting A100 reservations with end dates beyond the retirement date. (3) **Migrate workloads**: Help tenants migrate to H100 pools (may require application changes). Provide migration guides and support. (4) **Drain pools**: As reservations end, machines are not re-assigned. Warm pool target reduced to 0. (5) **Decommission**: Once all reservations are complete, power off machines, reset to factory defaults, prepare for resale or recycling. (6) **Final accounting**: Close all pools, archive machine records, reconcile RMA tickets, update firmware compliance dashboards to exclude retired machines.

---

## 16. References

- "Site Reliability Engineering" by Google (O'Reilly) -- Chapter 26 (Data Processing Pipelines), Chapter 29 (Dealing with Interrupts). Health monitoring and fleet management patterns.
- "Predictive Maintenance with Machine Learning" -- AWS Machine Learning Blog. Health scoring and failure prediction models.
- "IPMI 2.0 Specification" -- Intel/HP/NEC/Dell. BMC management interface, sensor data records (SDR), system event log (SEL).
- "NVIDIA Data Center GPU Manager (DCGM)" -- NVIDIA Developer Documentation. GPU telemetry, health monitoring, ECC error tracking.
- "Forecasting: Principles and Practice" by Hyndman & Athanasopoulos -- ARIMA/SARIMA models, seasonal decomposition, forecast accuracy metrics.
- "Prophet: Forecasting at Scale" -- Meta Research Blog. Time series forecasting with trend changepoints and holidays.
- "VictoriaMetrics Architecture" -- victoriametrics.com. High-cardinality time series storage, downsampling.
- "SMART Monitoring Tools (smartmontools)" -- smartmontools.org. Disk health monitoring via S.M.A.R.T. attributes.
- "Network Equipment Monitoring via SNMP" -- RFC 1213 (MIB-II), RFC 2863 (Interfaces MIB). Network port error counter collection.
- "Managing Hardware at Scale: Lessons from Google, Facebook, and Microsoft" -- IEEE/ACM conference papers on fleet management.
- "Dell TechDirect API Documentation" -- dell.com/support/techdirect. Programmatic RMA and dispatch management.
- "HPE iLO REST API Reference" -- HPE developer documentation. Remote server management and firmware updates.
