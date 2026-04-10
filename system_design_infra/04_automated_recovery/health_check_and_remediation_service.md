# System Design: Health Check and Remediation Service

> **Relevance to role:** Health checks are the sensory system of a cloud infrastructure platform — without accurate, multi-layered health checks, no automated remediation, failover, or self-healing is possible. This design covers the full pipeline from raw health signals to actionable remediation: multi-layer checks (process, port, HTTP, deep health), composite health scoring, flap detection, escalation policies, and SLO-aware remediation decisions. This is foundational for anyone operating Kubernetes at scale, managing bare-metal fleets, or building reliable MySQL and Elasticsearch clusters.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Multi-Layer Health Checks:** Support health checks at process level, port level, HTTP endpoint level, and deep health level (DB connectivity, cache availability, downstream service reachability).
2. **Health Check Registry:** Centralized registry of what checks are configured for which services, with versioning and ownership metadata.
3. **Composite Health Score:** Aggregate multiple leaf health checks into a single composite health score per entity, with configurable weights.
4. **Flap Detection:** Detect entities that oscillate between healthy and unhealthy states, and suppress remediation for flapping entities.
5. **Remediation Actions:** Execute configurable remediation actions: restart process, drain node, replace instance, escalate to human.
6. **Cooldown Periods:** Enforce minimum time between consecutive remediation actions on the same target to prevent remediation loops.
7. **Escalation Policy:** Define multi-tier escalation: auto-fix → human alert → page on-call, with configurable timeouts between tiers.
8. **SLO-Aware Remediation:** Ensure remediation actions do not violate SLOs of downstream services (e.g., don't drain a node if it would reduce a service below its minimum replica count).
9. **Health Check Templating:** Provide reusable templates for common check patterns (liveness HTTP, readiness gRPC, deep-health MySQL).
10. **Historical Health Analytics:** Store health check history for trend analysis, anomaly detection, and SLO reporting.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Health check execution frequency | 5-30 seconds (configurable per check) |
| Check-to-detection latency | < 30 seconds (3 consecutive failures at 10s interval) |
| Detection-to-remediation latency | < 15 seconds |
| False positive rate | < 0.1% |
| Health check coverage | > 99% of production entities have registered checks |
| Service availability (of this service) | 99.99% |
| Scale | 2M health checks/sec (100K hosts × 20 checks/host at 10s intervals) |
| Composite score calculation latency | < 100ms per entity |

### Constraints & Assumptions
- Kubernetes is the primary orchestration platform; kubelet executes pod-level probes natively.
- This service extends beyond Kubernetes to cover bare-metal hosts, VMs, and non-K8s services.
- Prometheus is the metrics backend; PromQL is used for metric-based health checks.
- MySQL stores check definitions and remediation state; Elasticsearch for health event search.
- Kafka is the event bus for health check results.
- Teams own their health check definitions; the platform provides templates and enforcement.

### Out of Scope
- The actual execution of Kubernetes liveness/readiness/startup probes (kubelet does this).
- Self-healing reconciliation loop (covered in self_healing_infrastructure.md).
- Failover orchestration (covered in automated_failover_system.md).
- Chaos engineering validation (covered in chaos_engineering_platform.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|---|---|---|
| Entities monitored | 100K hosts + 2M pods + 500 DB clusters + 10K services | ~2.1M entities |
| Checks per entity | Average 5 checks/entity (liveness, readiness, deep health, hardware, network) | ~10.5M check instances |
| Check executions/sec | 10.5M checks ÷ avg 10s interval | ~1.05M checks/sec |
| Health events/sec (to Kafka) | 1.05M checks × ~10% report state (rest are steady-state no-ops) | ~105K events/sec |
| Composite score calculations/sec | 2.1M entities ÷ 10s recalculation interval | ~210K/sec |
| Remediation decisions/sec | ~0.1% entities unhealthy = ~2,100 decisions/10s | ~210/sec |
| API calls/sec (human + automated) | 500 (dashboard queries, check management, remediation triggers) | ~500/sec |

### Latency Requirements

| Operation | Target | P99 |
|---|---|---|
| Single health check execution | < 5s | < 10s |
| Health event ingestion (to Kafka) | < 100ms | < 500ms |
| Composite score calculation | < 100ms | < 500ms |
| Flap detection evaluation | < 50ms | < 200ms |
| Remediation decision | < 1s | < 5s |
| Remediation execution (restart) | < 10s | < 30s |
| Remediation execution (drain) | < 2min | < 5min |
| API response | < 100ms | < 500ms |
| Health dashboard query | < 2s | < 5s |

### Storage Estimates

| Data Type | Calculation | Result |
|---|---|---|
| Health check definitions | 10.5M check instances × 500 bytes | ~5 GB |
| Health check results (raw, 7-day hot) | 105K events/sec × 200 bytes × 86,400s × 7d | ~12.6 TB (ES hot tier) |
| Composite health scores (current) | 2.1M entities × 200 bytes | ~420 MB (fits in Redis) |
| Remediation state | 210 decisions/sec × 1 KB × 86,400s × 30d retention | ~545 GB |
| Flap tracking state | 2.1M entities × 100 bytes | ~210 MB (fits in Redis) |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Health events → Kafka | 105K events/sec × 200 bytes | ~21 MB/sec |
| Kafka → composite score calculator | ~21 MB/sec consumed | ~21 MB/sec |
| Composite scores → Redis | 210K writes/sec × 200 bytes | ~42 MB/sec |
| API responses | 500/sec × 5 KB | ~2.5 MB/sec |

---

## 3. High Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                   HEALTH CHECK AND REMEDIATION SERVICE                       │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐    │
│  │                    HEALTH CHECK LAYER                                │    │
│  │                                                                      │    │
│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌────────────┐  │    │
│  │  │  Process      │ │  Port        │ │  HTTP/gRPC   │ │  Deep      │  │    │
│  │  │  Check        │ │  Check       │ │  Check       │ │  Health    │  │    │
│  │  │  (pid exists, │ │  (TCP        │ │  (GET /health│ │  Check     │  │    │
│  │  │   not zombie) │ │   connect)   │ │   status 200)│ │  (DB, cache│  │    │
│  │  └──────────────┘ └──────────────┘ └──────────────┘ │   downstream)│  │    │
│  │                                                      └────────────┘  │    │
│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐                  │    │
│  │  │  Hardware     │ │  Kernel      │ │  Custom      │                  │    │
│  │  │  (SMART, ECC, │ │  (OOM, panic │ │  (user-      │                  │    │
│  │  │   temp, fan)  │ │   fs errors) │ │   defined)   │                  │    │
│  │  └──────────────┘ └──────────────┘ └──────────────┘                  │    │
│  └───────────────────────────┬──────────────────────────────────────────┘    │
│                              │ health events                                 │
│                              ▼                                               │
│  ┌──────────────────────────────────────────────────────────────────────┐    │
│  │                    EVENT PROCESSING LAYER                            │    │
│  │                                                                      │    │
│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐                 │    │
│  │  │  Event        │ │  Flap        │ │  Composite   │                 │    │
│  │  │  Normalizer   │ │  Detector    │ │  Score       │                 │    │
│  │  │  (unified     │ │  (suppress   │ │  Calculator  │                 │    │
│  │  │   schema)     │ │   oscillation)│ │  (weighted   │                 │    │
│  │  └──────────────┘ └──────────────┘ │   aggregate)  │                 │    │
│  │                                     └──────────────┘                 │    │
│  └───────────────────────────┬──────────────────────────────────────────┘    │
│                              │ health state changes                          │
│                              ▼                                               │
│  ┌──────────────────────────────────────────────────────────────────────┐    │
│  │                    REMEDIATION LAYER                                  │    │
│  │                                                                      │    │
│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌────────────┐  │    │
│  │  │  Remediation  │ │  Cooldown    │ │  SLO-Aware   │ │ Escalation │  │    │
│  │  │  Decision     │ │  Manager     │ │  Gate        │ │ Manager    │  │    │
│  │  │  Engine       │ │  (rate       │ │  (check      │ │ (auto →   │  │    │
│  │  │  (rules +     │ │   limit)     │ │   downstream │ │  alert →  │  │    │
│  │  │   priority)   │ │              │ │   SLOs)      │ │  page)    │  │    │
│  │  └──────────────┘ └──────────────┘ └──────────────┘ └────────────┘  │    │
│  └───────────────────────────┬──────────────────────────────────────────┘    │
│                              │ remediation commands                          │
│                              ▼                                               │
│  ┌──────────────────────────────────────────────────────────────────────┐    │
│  │                    EXECUTION LAYER                                    │    │
│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌────────────┐  │    │
│  │  │ K8s Actions  │ │ Host Actions │ │ DB Actions   │ │ Notify     │  │    │
│  │  │ (restart pod,│ │ (restart     │ │ (failover    │ │ (Slack,    │  │    │
│  │  │  drain node, │ │  process,    │ │  replica,    │ │  PagerDuty,│  │    │
│  │  │  scale)      │ │  reboot)     │ │  add replica)│ │  email)    │  │    │
│  │  └──────────────┘ └──────────────┘ └──────────────┘ └────────────┘  │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐    │
│  │  DATA LAYER                                                          │    │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐            │    │
│  │  │  MySQL   │  │  Redis   │  │  Elastic  │  │  Kafka   │            │    │
│  │  │  (defs,  │  │  (scores,│  │  Search   │  │  (events │            │    │
│  │  │  state,  │  │  flap,   │  │  (history,│  │  bus)    │            │    │
│  │  │  audit)  │  │  cache)  │  │  search)  │  │          │            │    │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘            │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Process Check** | Verifies process is running (PID exists, not zombie). Lightest check. |
| **Port Check** | TCP connect to port. Confirms process is listening. |
| **HTTP/gRPC Check** | Sends request to health endpoint, validates response code and optionally response body. |
| **Deep Health Check** | Tests full dependency chain: DB connectivity (SELECT 1), cache hit/miss, downstream service reachability, business logic sanity. |
| **Hardware Check** | Reads SMART disk stats, ECC memory counters, CPU/GPU temperature, fan speeds via BMC/IPMI or sysfs. |
| **Kernel Check** | Monitors dmesg/journald for kernel panics, OOM kills, filesystem errors, NIC driver errors. |
| **Custom Check** | User-defined script that returns exit code 0 (healthy) or non-zero (unhealthy) with optional structured output. |
| **Event Normalizer** | Converts heterogeneous health signals into a unified schema: {entity, check_type, status, latency, timestamp, details}. |
| **Flap Detector** | Counts state transitions per entity per time window. If transitions exceed threshold, marks entity as flapping and suppresses remediation. |
| **Composite Score Calculator** | Computes weighted average of all checks for an entity. Produces a 0-100 score. Stores in Redis for fast access. |
| **Remediation Decision Engine** | Given an unhealthy entity, selects the appropriate remediation action based on rules, check type, and entity type. |
| **Cooldown Manager** | Tracks when each entity was last remediated. Prevents re-remediation within the cooldown window. |
| **SLO-Aware Gate** | Before executing remediation, verifies that the action won't violate downstream SLOs (e.g., won't reduce a service below minimum replicas). |
| **Escalation Manager** | Manages the escalation timeline: auto-fix (Tier 1) → alert team (Tier 2) → page on-call (Tier 3) → page senior on-call (Tier 4). |

### Data Flows

**Check Execution → Score Calculation:**
1. Health check agents execute checks per their schedule.
2. Results published to Kafka topic `health-check-results`.
3. Event Normalizer consumes, normalizes, publishes to `normalized-health-events`.
4. Flap Detector consumes, updates flap state in Redis.
5. Composite Score Calculator consumes, recalculates entity scores, writes to Redis + MySQL.

**Score Change → Remediation:**
1. Score drops below threshold → `health-state-changes` Kafka topic.
2. Remediation Decision Engine consumes, checks cooldown, checks flap status.
3. If not flapping and not in cooldown → SLO-Aware Gate verifies downstream impact.
4. If safe → execute remediation via appropriate executor.
5. If escalation needed → Escalation Manager starts timer chain.
6. All actions logged to MySQL audit + Elasticsearch.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Health check template (reusable check patterns)
CREATE TABLE health_check_template (
    template_id         VARCHAR(128) PRIMARY KEY,
    template_name       VARCHAR(256) NOT NULL,
    check_type          ENUM('process', 'port', 'http', 'grpc', 'deep_health',
                             'hardware', 'kernel', 'custom') NOT NULL,
    config_template     JSON NOT NULL,
    description         TEXT,
    default_interval_sec INT NOT NULL DEFAULT 10,
    default_timeout_sec INT NOT NULL DEFAULT 5,
    default_failure_threshold INT NOT NULL DEFAULT 3,
    default_success_threshold INT NOT NULL DEFAULT 1,
    version             INT NOT NULL DEFAULT 1,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- Health check instance (a specific check bound to a specific entity)
CREATE TABLE health_check_instance (
    instance_id         BIGINT PRIMARY KEY AUTO_INCREMENT,
    check_id            VARCHAR(128) NOT NULL UNIQUE,
    template_id         VARCHAR(128),
    entity_type         ENUM('host', 'pod', 'vm', 'service', 'cluster', 'bare_metal') NOT NULL,
    entity_id           VARCHAR(256) NOT NULL,
    check_type          ENUM('process', 'port', 'http', 'grpc', 'deep_health',
                             'hardware', 'kernel', 'custom') NOT NULL,
    config_json         JSON NOT NULL,
    interval_sec        INT NOT NULL DEFAULT 10,
    timeout_sec         INT NOT NULL DEFAULT 5,
    failure_threshold   INT NOT NULL DEFAULT 3,
    success_threshold   INT NOT NULL DEFAULT 1,
    weight              DECIMAL(3,2) NOT NULL DEFAULT 1.00,  -- weight in composite score
    enabled             BOOLEAN NOT NULL DEFAULT TRUE,
    owner_team          VARCHAR(128) NOT NULL,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (template_id) REFERENCES health_check_template(template_id),
    INDEX idx_entity (entity_type, entity_id),
    INDEX idx_check_type (check_type),
    INDEX idx_owner (owner_team),
    INDEX idx_enabled (enabled)
) ENGINE=InnoDB;

-- Entity health state (current state)
CREATE TABLE entity_health_state (
    entity_id           VARCHAR(256) PRIMARY KEY,
    entity_type         ENUM('host', 'pod', 'vm', 'service', 'cluster', 'bare_metal') NOT NULL,
    composite_score     DECIMAL(5,2) NOT NULL DEFAULT 100.00,
    health_status       ENUM('healthy', 'degraded', 'unhealthy', 'critical', 'unknown') NOT NULL DEFAULT 'unknown',
    check_count         INT NOT NULL DEFAULT 0,
    checks_passing      INT NOT NULL DEFAULT 0,
    checks_failing      INT NOT NULL DEFAULT 0,
    checks_unknown      INT NOT NULL DEFAULT 0,
    is_flapping         BOOLEAN NOT NULL DEFAULT FALSE,
    flap_suppressed_until TIMESTAMP NULL,
    last_state_change   TIMESTAMP NULL,
    last_remediation    TIMESTAMP NULL,
    cooldown_until      TIMESTAMP NULL,
    escalation_tier     INT NOT NULL DEFAULT 0,       -- 0=none, 1=auto-fix, 2=alert, 3=page, 4=senior
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_status (health_status),
    INDEX idx_flapping (is_flapping),
    INDEX idx_entity_type (entity_type, health_status)
) ENGINE=InnoDB;

-- Per-check state (for each check-instance running against an entity)
CREATE TABLE check_state (
    check_id            VARCHAR(128) NOT NULL,
    entity_id           VARCHAR(256) NOT NULL,
    current_status      ENUM('passing', 'failing', 'unknown', 'disabled') NOT NULL DEFAULT 'unknown',
    consecutive_failures INT NOT NULL DEFAULT 0,
    consecutive_successes INT NOT NULL DEFAULT 0,
    last_check_time     TIMESTAMP(3) NULL,
    last_success_time   TIMESTAMP(3) NULL,
    last_failure_time   TIMESTAMP(3) NULL,
    last_response_ms    INT,
    last_error_message  TEXT,
    PRIMARY KEY (check_id, entity_id),
    INDEX idx_entity (entity_id),
    INDEX idx_status (current_status)
) ENGINE=InnoDB;

-- Flap history (for analysis)
CREATE TABLE flap_event (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    entity_id           VARCHAR(256) NOT NULL,
    flap_started        TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    flap_ended          TIMESTAMP NULL,
    transitions_count   INT NOT NULL,
    duration_sec        INT,
    root_cause          TEXT,                         -- manually filled during review
    INDEX idx_entity (entity_id),
    INDEX idx_started (flap_started)
) ENGINE=InnoDB;

-- Remediation record
CREATE TABLE remediation_record (
    remediation_id      BIGINT PRIMARY KEY AUTO_INCREMENT,
    entity_id           VARCHAR(256) NOT NULL,
    entity_type         ENUM('host', 'pod', 'vm', 'service', 'cluster', 'bare_metal') NOT NULL,
    trigger_check_id    VARCHAR(128),
    trigger_score       DECIMAL(5,2),
    action_type         ENUM('restart_process', 'restart_pod', 'drain_node', 'cordon_node',
                             'replace_instance', 'reboot_host', 'alert_team', 'page_oncall',
                             'page_senior', 'noop') NOT NULL,
    escalation_tier     INT NOT NULL DEFAULT 1,
    status              ENUM('pending', 'executing', 'succeeded', 'failed', 'skipped_cooldown',
                             'skipped_flapping', 'skipped_slo_risk', 'escalated') NOT NULL DEFAULT 'pending',
    slo_check_result    JSON,                         -- which SLOs were checked and their status
    executor            VARCHAR(128) NOT NULL DEFAULT 'system',
    started_at          TIMESTAMP(3) NULL,
    completed_at        TIMESTAMP(3) NULL,
    duration_ms         INT,
    result_detail       TEXT,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_entity (entity_id, created_at),
    INDEX idx_action_status (action_type, status),
    INDEX idx_escalation (escalation_tier, status)
) ENGINE=InnoDB;

-- Escalation policy
CREATE TABLE escalation_policy (
    policy_id           VARCHAR(128) PRIMARY KEY,
    entity_type         ENUM('host', 'pod', 'vm', 'service', 'cluster', 'bare_metal') NOT NULL,
    service_tier        ENUM('tier1', 'tier2', 'tier3', 'tier4') NOT NULL,
    tiers_json          JSON NOT NULL,
    /* Example tiers_json:
       [
         {"tier": 1, "action": "auto_remediate", "timeout_sec": 300},
         {"tier": 2, "action": "alert_team_slack", "timeout_sec": 900},
         {"tier": 3, "action": "page_oncall", "timeout_sec": 1800},
         {"tier": 4, "action": "page_senior_oncall", "timeout_sec": null}
       ]
    */
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE INDEX idx_type_tier (entity_type, service_tier)
) ENGINE=InnoDB;

-- Health check coverage report (materialized, updated hourly)
CREATE TABLE health_check_coverage (
    entity_type         ENUM('host', 'pod', 'vm', 'service', 'cluster', 'bare_metal') NOT NULL,
    service_tier        ENUM('tier1', 'tier2', 'tier3', 'tier4') NOT NULL,
    total_entities      INT NOT NULL,
    entities_with_checks INT NOT NULL,
    coverage_pct        DECIMAL(5,2) NOT NULL,
    missing_checks_json JSON,                         -- list of entities without checks
    calculated_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (entity_type, service_tier)
) ENGINE=InnoDB;
```

### Database Selection

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **MySQL 8.0** | ACID for check definitions and remediation state; team expertise | Not ideal for time-series health events at this volume | **Selected for metadata, state, and audit** |
| **Redis Cluster** | Sub-millisecond reads/writes; perfect for composite scores and flap state | Not durable (AOF persistence acceptable for cache) | **Selected for real-time state (scores, flap, cooldown)** |
| **Elasticsearch 8.x** | Excellent for time-series health events; full-text search on error messages | Eventually consistent | **Selected for health event history and search** |
| **InfluxDB / TimescaleDB** | Purpose-built for time-series | Another database to operate; team has less experience | Not selected |

**Justification:** Three-store architecture: MySQL for authoritative metadata (check definitions, escalation policies, remediation records), Redis for real-time state that must be fast (composite scores, flap tracking, cooldown timers), and Elasticsearch for the massive volume of health check events that need time-range queries and full-text search.

### Indexing Strategy

**MySQL:**
- `health_check_instance(entity_type, entity_id)` — find all checks for a specific entity.
- `entity_health_state(health_status)` — find all unhealthy entities.
- `remediation_record(entity_id, created_at)` — remediation history for an entity.
- `check_state(entity_id)` — all check states for composite score calculation.

**Redis:**
- Key pattern: `health:score:{entity_id}` → composite score (DECIMAL).
- Key pattern: `health:flap:{entity_id}` → flap state (HASH: is_flapping, transitions_1h, suppressed_until).
- Key pattern: `health:cooldown:{entity_id}:{action_type}` → cooldown expiry (TTL-based key).
- Key pattern: `health:check:{check_id}:{entity_id}` → current check state (HASH).

**Elasticsearch:**
- Index pattern: `health-events-YYYY-MM-DD`.
- Mappings: entity_id (keyword), check_type (keyword), status (keyword), response_ms (integer), error_message (text + keyword), timestamp (date).
- ILM: hot (7d), warm (30d), cold (90d), delete (365d).

---

## 5. API Design

### REST Endpoints

```
Base URL: https://healthcheck.infra.internal/api/v1

# Template management
POST   /templates
  Body: {
    "template_id": "http-liveness-standard",
    "template_name": "Standard HTTP Liveness Check",
    "check_type": "http",
    "config_template": {
      "method": "GET",
      "path": "/healthz",
      "expected_status_codes": [200],
      "timeout_sec": 5
    },
    "default_interval_sec": 10,
    "default_failure_threshold": 3,
    "description": "Standard liveness check - HTTP GET /healthz expecting 200"
  }
  Response: 201 Created

GET    /templates
  Response: 200 [ list of templates ]

# Check instance management
POST   /checks
  Body: {
    "template_id": "http-liveness-standard",  // optional, can define inline
    "entity_type": "pod",
    "entity_id": "orders-service-abc123",
    "check_type": "http",
    "config": {
      "method": "GET",
      "path": "/healthz",
      "port": 8080,
      "expected_status_codes": [200],
      "headers": {"X-Health-Check": "true"}
    },
    "interval_sec": 10,
    "timeout_sec": 5,
    "failure_threshold": 3,
    "weight": 0.30,
    "owner_team": "orders-team"
  }
  Response: 201 { "check_id": "chk-orders-liveness-001" }

GET    /checks?entity_type=pod&entity_id=orders-service-abc123
  Response: 200 [ all checks for this entity ]

PUT    /checks/{check_id}
  Body: { updated fields }
  Response: 200

DELETE /checks/{check_id}
  Response: 204

# Deep health check definition
POST   /checks
  Body: {
    "entity_type": "service",
    "entity_id": "orders-service",
    "check_type": "deep_health",
    "config": {
      "method": "GET",
      "path": "/health/deep",
      "port": 8080,
      "expected_response": {
        "checks": {
          "database": "ok",
          "cache": "ok",
          "payment_gateway": "ok|degraded"  // regex — "degraded" is acceptable
        }
      },
      "timeout_sec": 10
    },
    "interval_sec": 30,
    "failure_threshold": 2,
    "weight": 0.40,
    "owner_team": "orders-team"
  }
  Response: 201

# Entity health
GET    /entities/{entity_id}/health
  Response: 200 {
    "entity_id": "orders-service-abc123",
    "composite_score": 85.5,
    "health_status": "degraded",
    "checks": [
      {"check_id": "chk-001", "check_type": "http", "status": "passing", "last_check": "...", "response_ms": 12},
      {"check_id": "chk-002", "check_type": "deep_health", "status": "failing", "last_check": "...",
       "error": "database: timeout after 10s"}
    ],
    "is_flapping": false,
    "last_remediation": "2026-04-08T15:30:00Z",
    "cooldown_until": null,
    "escalation_tier": 0
  }

GET    /entities?health_status=unhealthy&entity_type=host&limit=100
  Response: 200 { entities with scores, sorted by score ascending (worst first) }

GET    /entities/{entity_id}/health/history?since=2026-04-08T00:00:00Z
  Response: 200 { time-series of composite scores and check results }

# Remediation
POST   /entities/{entity_id}/remediate
  Body: {
    "action_type": "restart_pod",
    "reason": "deep health check failing for 5 minutes",
    "dry_run": false
  }
  Response: 202 { "remediation_id": 12345, "status": "executing" }

GET    /remediations?entity_id=orders-service-abc123&status=failed
  Response: 200 [ remediation history ]

# Escalation policy
POST   /escalation-policies
  Body: {
    "policy_id": "host-tier1-policy",
    "entity_type": "host",
    "service_tier": "tier1",
    "tiers": [
      {"tier": 1, "action": "auto_remediate", "timeout_sec": 300,
       "remediation_type": "restart_process"},
      {"tier": 2, "action": "auto_remediate", "timeout_sec": 600,
       "remediation_type": "reboot_host"},
      {"tier": 3, "action": "alert_team_slack", "timeout_sec": 900},
      {"tier": 4, "action": "page_oncall", "timeout_sec": 1800},
      {"tier": 5, "action": "page_senior_oncall", "timeout_sec": null}
    ]
  }
  Response: 201

# Coverage report
GET    /coverage?entity_type=host&service_tier=tier1
  Response: 200 {
    "coverage_pct": 98.5,
    "total_entities": 10000,
    "entities_with_checks": 9850,
    "missing": ["host-x42", "host-x43", ...]
  }

# Flap management
GET    /flapping?entity_type=host
  Response: 200 [ list of currently flapping entities ]

POST   /entities/{entity_id}/suppress-flap
  Body: { "duration_sec": 7200, "reason": "known maintenance" }
  Response: 200
```

### CLI Design

```bash
# Check management
healthctl checks list --entity=orders-service-abc123
healthctl checks create --from-template=http-liveness-standard --entity=orders-pod-xyz --port=8080
healthctl checks create --config=./deep-health-check.yaml
healthctl checks disable chk-001 --reason="false positive investigation"
healthctl checks enable chk-001

# Entity health
healthctl health orders-service-abc123                    # summary
healthctl health orders-service-abc123 --detail           # all checks
healthctl health orders-service-abc123 --history --last=24h
healthctl health --unhealthy --type=host --format=table   # all unhealthy hosts

# Remediation
healthctl remediate orders-service-abc123 --action=restart --dry-run
healthctl remediate orders-service-abc123 --action=restart
healthctl remediation-history orders-service-abc123 --last=7d

# Flap management
healthctl flapping list
healthctl flapping suppress orders-service-abc123 --duration=2h --reason="investigating"

# Coverage
healthctl coverage --type=host --tier=tier1
healthctl coverage --gaps --type=service --tier=tier1     # show entities missing checks

# Escalation
healthctl escalation-policy list
healthctl escalation-policy show host-tier1-policy
```

---

## 6. Core Component Deep Dives

### 6.1 Multi-Layer Health Check Architecture

**Why it's hard:** Different check types have different reliability, latency, and information content. A process check (is the PID alive?) tells you almost nothing about service health — a process can be alive but deadlocked. An HTTP check (/healthz returns 200) is better but might be lying — the endpoint returns 200 even when the database is down. A deep health check (test actual DB connectivity) is most informative but has its own failure modes (the check itself can timeout, the dependency might be transiently unavailable). Combining these layers into an accurate health picture is the challenge.

**Approaches Compared:**

| Approach | Accuracy | Performance Impact | Complexity | False Positives |
|---|---|---|---|---|
| **Single layer (HTTP only)** | Low — misses deep issues | Low | Low | Medium |
| **All layers, equal weight** | Medium — noisy | Medium | Medium | Medium |
| **Layered with weights + dependency awareness** | High | Medium | High | Low |
| **ML-based anomaly detection** | Very high (when trained) | Low (inference is fast) | Very high | Low after training |

**Selected Approach: Layered with configurable weights and dependency awareness.**

**Health Check Layer Definitions:**

```
Layer 1: Process Check (lightest, fastest)
  What: Is the process running? (check PID file, /proc/{pid}/status)
  When: Every 5 seconds
  Weight: 0.10
  Failure means: Process crashed — almost certainly a real problem
  False positives: Very rare (zombie process detection handles edge cases)

Layer 2: Port Check (light)
  What: Can we TCP-connect to the service port?
  When: Every 10 seconds
  Weight: 0.15
  Failure means: Process isn't listening — startup issue, or process alive but not accepting connections
  False positives: Rare — connection refused is definitive

Layer 3: HTTP/gRPC Health Endpoint (standard)
  What: Does /healthz (or gRPC Health.Check) return 200/SERVING?
  When: Every 10 seconds
  Weight: 0.25
  Failure means: Service is running but not healthy — internal error, resource exhaustion
  False positives: Possible if the health endpoint has a bug
  Best practice: /healthz should be lightweight, check only local state

Layer 4: Deep Health Check (heaviest, most informative)
  What: Does /health/deep return OK for all dependencies?
    Checks:
    - Database: SHOW STATUS LIKE 'Uptime' (or SELECT 1)
    - Cache: SET chaos_healthcheck:probe 1 EX 10; GET chaos_healthcheck:probe
    - Downstream services: HTTP/gRPC health check to each dependency
    - Message broker: Kafka metadata request, RabbitMQ connection check
  When: Every 30-60 seconds (more expensive)
  Weight: 0.35
  Failure means: Service can't serve requests due to dependency issues
  False positives: Higher — transient dependency hiccup causes deep health failure

Layer 5: Hardware Check (bare-metal only)
  What: SMART disk stats, ECC memory errors, CPU temperature, fan speed, NIC errors
  When: Every 60 seconds
  Weight: 0.10
  Failure means: Hardware degradation that may lead to failure
  False positives: Low — hardware metrics are objective

Layer 6: Kernel/OS Check (Node Problem Detector)
  What: dmesg errors, filesystem mount status, NTP sync, container runtime health
  When: Continuous monitoring with 30-second summary
  Weight: 0.05
  Failure means: OS-level issue affecting all workloads on the node
  False positives: Low — kernel messages are authoritative
```

**Deep Health Check Implementation:**

```python
class DeepHealthCheck:
    """Example deep health check for a Java/Python web service."""
    
    def execute(self, entity: Entity) -> CheckResult:
        overall_status = "healthy"
        details = {}
        
        # Check database connectivity
        try:
            db_start = time.time()
            db_conn = mysql.connect(host=entity.config['db_host'], port=3306, timeout=5)
            db_conn.execute("SELECT 1")
            db_latency = time.time() - db_start
            details['database'] = {
                'status': 'ok',
                'latency_ms': int(db_latency * 1000)
            }
            if db_latency > 1.0:  # > 1s is degraded
                details['database']['status'] = 'degraded'
                overall_status = 'degraded'
        except Exception as e:
            details['database'] = {'status': 'error', 'error': str(e)}
            overall_status = 'unhealthy'
        
        # Check cache connectivity
        try:
            cache_start = time.time()
            redis_conn = redis.connect(host=entity.config['cache_host'], timeout=2)
            redis_conn.set("healthcheck:probe", "1", ex=10)
            val = redis_conn.get("healthcheck:probe")
            cache_latency = time.time() - cache_start
            if val == "1":
                details['cache'] = {'status': 'ok', 'latency_ms': int(cache_latency * 1000)}
            else:
                details['cache'] = {'status': 'degraded', 'error': 'write-read mismatch'}
                overall_status = 'degraded'
        except Exception as e:
            details['cache'] = {'status': 'error', 'error': str(e)}
            # Cache failure is degradation, not complete failure
            if overall_status == 'healthy':
                overall_status = 'degraded'
        
        # Check downstream services
        for dep in entity.config.get('dependencies', []):
            try:
                resp = http.get(f"http://{dep['host']}:{dep['port']}/healthz", timeout=3)
                if resp.status_code == 200:
                    details[dep['name']] = {'status': 'ok'}
                else:
                    details[dep['name']] = {'status': 'degraded', 'http_status': resp.status_code}
                    overall_status = 'degraded'
            except Exception as e:
                details[dep['name']] = {'status': 'error', 'error': str(e)}
                if dep.get('critical', False):
                    overall_status = 'unhealthy'
                else:
                    if overall_status == 'healthy':
                        overall_status = 'degraded'
        
        return CheckResult(
            status=overall_status,
            details=details,
            latency_ms=int((time.time() - overall_start) * 1000)
        )
```

**Failure Modes:**
- **Deep health check takes too long:** Timeout at 10 seconds. If the check itself is slow, it counts as a failure. Track check latency as a separate metric.
- **Dependency check causes cascade:** If every service checks every downstream service, and a downstream is slow, all upstream health checks slow down simultaneously. Mitigation: use separate thread pool for health checks; never block the main request path.
- **Health check endpoint lies:** The endpoint returns 200 but the service is actually broken. Mitigation: deep health checks test actual operations, not just status codes. Also correlate with business metrics.

**Interviewer Q&As:**

**Q1: Why not just use Kubernetes liveness and readiness probes for everything?**
A: K8s probes have limitations: (1) They're per-pod, not per-service or per-host. (2) They don't support composite scoring (multiple checks combined). (3) They don't do flap detection. (4) They don't cover bare-metal hosts or non-K8s services. (5) A bad liveness probe can restart a healthy pod (e.g., if it checks DB connectivity and the DB has a transient hiccup). Our system sits above K8s probes and adds these capabilities.

**Q2: Should liveness probes check dependencies?**
A: No. This is a common and dangerous antipattern. If a liveness probe checks DB connectivity, and the DB goes down, ALL pods report unhealthy, ALL get restarted, and you now have a DB outage + an application outage. Liveness should check: "Is my process able to handle requests?" Readiness should check: "Should I receive traffic right now?" Only readiness (or deep health) should check dependencies. And deep health should have a separate remediation path (remove from LB, not restart).

**Q3: How do you handle a health check endpoint that's computationally expensive?**
A: (1) Cache the result for N seconds (health check interval aligned). (2) Run the expensive computation in a background thread; the endpoint returns the cached result. (3) Set a separate, lower frequency for expensive checks (60s vs 10s). (4) If the check is so expensive it impacts the service, it's a bad check — refactor it to be cheaper or use a metric-based check (query Prometheus instead of the service directly).

**Q4: How do you handle partial health (some checks pass, some fail)?**
A: This is exactly what the composite score handles. If 3 of 5 checks pass with weights totaling 0.65, the composite score might be 65 — "degraded" but not "unhealthy." The remediation decision considers the composite score: degraded might trigger an alert but not a restart. Only "unhealthy" (score < 50) triggers automated remediation.

**Q5: How do you handle service discovery for deep health checks (finding dependency endpoints)?**
A: Three approaches: (1) Static configuration — endpoints are in the check definition. (2) Service mesh integration — query Istio/Envoy for upstream/downstream endpoints. (3) Kubernetes service DNS — check `service-name.namespace.svc.cluster.local`. We prefer Kubernetes service DNS because it automatically tracks pod changes.

**Q6: What's the latency impact of running health checks on every pod every 10 seconds?**
A: Each health check is a single HTTP GET returning < 1 KB. At 10s intervals, that's 0.1 requests/sec per pod for health checking. For a node running 20 pods, that's 2 additional req/sec — negligible. The deep health check (every 30s) is slightly more expensive but still trivial compared to production traffic. The bigger concern is the check destination: if deep health checks query the database, ensure they use a separate connection pool and lightweight queries.

---

### 6.2 Flap Detection

**Why it's hard:** Flapping (rapid oscillation between healthy and unhealthy) causes two problems: (1) Remediation storms — the system keeps restarting/draining based on transient failures. (2) Alert fatigue — operators get barraged with healthy/unhealthy notifications. But suppressing flapping entities means we might miss a real failure hiding inside the oscillation. The detection must be accurate and the suppression must be safe.

**Approaches Compared:**

| Approach | Detection Accuracy | Complexity | Risk of Missing Real Failures |
|---|---|---|---|
| **Simple counter** (N transitions in T time) | Medium | Low | Medium — any N transitions triggers |
| **Nagios-style flap detection** (% state changes in recent history) | High | Medium | Low — tunable sensitivity |
| **Hysteresis** (require N consecutive healthy to mark healthy) | Medium | Low | Medium — delays detection of real recovery |
| **Combined: counter + hysteresis + exponential suppression** | Very high | Medium-High | Low |

**Selected Approach: Combined approach with counter-based detection + hysteresis for recovery + exponential suppression.**

**Implementation:**

```python
class FlapDetector:
    def __init__(self):
        self.window_sec = 600           # 10-minute evaluation window
        self.max_transitions = 5        # 5 transitions in 10 min = flapping
        self.stable_period_sec = 300    # require 5 min stable to clear flapping
        self.base_suppression_sec = 300 # initial suppression: 5 minutes
        self.max_suppression_sec = 3600 # max suppression: 1 hour
    
    def evaluate(self, entity_id: str, new_state: str) -> FlapResult:
        # Get recent state history from Redis
        history = redis.lrange(f"health:transitions:{entity_id}", 0, -1)
        # history is a list of (timestamp, state) tuples
        
        # Count transitions in the evaluation window
        cutoff = now() - self.window_sec
        recent_transitions = [t for t in history if t.timestamp > cutoff]
        transition_count = len(recent_transitions)
        
        current_flap_state = redis.hget(f"health:flap:{entity_id}", "is_flapping")
        
        if not current_flap_state and transition_count >= self.max_transitions:
            # START FLAPPING
            flap_count = redis.hincrby(f"health:flap:{entity_id}", "flap_count", 1)
            # Exponential suppression: longer suppression for repeat flappers
            suppression_sec = min(
                self.base_suppression_sec * (2 ** (flap_count - 1)),
                self.max_suppression_sec
            )
            redis.hset(f"health:flap:{entity_id}", mapping={
                "is_flapping": "true",
                "started_at": str(now()),
                "suppressed_until": str(now() + suppression_sec),
                "transition_count": transition_count
            })
            return FlapResult(
                is_flapping=True,
                action="SUPPRESS_REMEDIATION",
                suppression_sec=suppression_sec
            )
        
        elif current_flap_state:
            # Currently flapping — check if it stabilized
            last_transition = recent_transitions[-1] if recent_transitions else None
            if last_transition and (now() - last_transition.timestamp) > self.stable_period_sec:
                # STOP FLAPPING — entity has been stable for 5 minutes
                redis.hset(f"health:flap:{entity_id}", "is_flapping", "false")
                return FlapResult(is_flapping=False, action="CLEAR_FLAPPING")
            else:
                # Still flapping — maintain suppression
                suppressed_until = redis.hget(f"health:flap:{entity_id}", "suppressed_until")
                return FlapResult(
                    is_flapping=True,
                    action="MAINTAIN_SUPPRESSION",
                    suppressed_until=suppressed_until
                )
        
        return FlapResult(is_flapping=False, action="NONE")
```

**Hysteresis for Recovery:**
- After an entity recovers from unhealthy to healthy, we require N consecutive successful checks (default: 3) before marking it fully healthy.
- This prevents a single successful check (amidst failures) from clearing the unhealthy state and triggering a "recovered" → "failed" → "recovered" oscillation.

**Failure Modes:**
- **Redis unavailable:** Fall back to MySQL for flap state (slower but durable). During Redis outage, flap detection is degraded but not absent.
- **Flap detector itself oscillates:** The exponential suppression prevents this — each flap episode triggers progressively longer suppression, up to 1 hour.
- **Real failure masked by flap suppression:** Safety valve: if an entity has been in flap suppression for > 1 hour, force-escalate to human regardless. A human can determine if it's a real failure or a systemic issue.

**Interviewer Q&As:**

**Q1: How do you differentiate between "flapping due to transient issues" and "flapping due to a real problem that's intermittent"?**
A: Both look the same from the health check perspective. The key differentiator is duration and pattern. Transient flapping usually resolves within minutes (deployment rollout, brief network congestion). Persistent flapping (hours) indicates an underlying issue — bad hardware, intermittent software bug, resource contention. We escalate to humans after 1 hour of flapping for investigation.

**Q2: How do you prevent flap detection from delaying detection of a real failure?**
A: Flap detection only affects entities that have recently been oscillating. A new, sudden failure (entity was stable for days, then drops to unhealthy) is not flagged as flapping — it triggers normal remediation immediately. Flap detection kicks in only after the configured number of transitions within the window.

**Q3: How do you handle flapping caused by rolling deployments?**
A: We integrate with the deployment controller. When a Deployment is rolling out (K8s Deployment strategy), we temporarily increase the flap threshold for pods in that Deployment. We also tag the transitions as "deployment-related" in the history, so they don't count toward the normal flap threshold.

**Q4: What's the relationship between flap detection and Kubernetes CrashLoopBackOff?**
A: CrashLoopBackOff is Kubernetes's built-in flap prevention for containers that keep crashing. It exponentially backs off restart attempts. Our flap detection operates at a higher level — it prevents our remediation system from taking additional actions (like draining the node) while Kubernetes is already handling the pod restarts. We observe CrashLoopBackOff as a health check signal and may escalate to the team.

**Q5: Can operators manually override flap suppression?**
A: Yes. `healthctl flapping suppress entity-123 --clear` removes the flap suppression, allowing remediation to proceed. This is useful when an operator has identified the root cause and wants the system to remediate. The override is logged.

**Q6: How do you tune the flap detection parameters?**
A: Start with conservative defaults (5 transitions in 10 minutes). Then analyze historical data: replay past health check events through the flap detector and measure: (1) How many legitimate failures would have been suppressed? (2) How many remediation storms would have been prevented? Adjust parameters until false suppression rate < 1% and storm prevention rate > 95%.

---

### 6.3 SLO-Aware Remediation Gate

**Why it's hard:** Remediation actions have side effects. Draining a node evicts pods, which reduces the available replicas for services running on that node. If a service is already at its minimum replica count (due to another failure), draining the node would violate its SLO. The gate must check downstream impact before approving remediation.

**Approaches Compared:**

| Approach | Accuracy | Performance | Complexity |
|---|---|---|---|
| **No gate (remediate blindly)** | N/A | Fast | Low |
| **Check PDB only** | Medium — PDBs exist for most services | Fast (K8s API query) | Low |
| **Full SLO impact analysis** | High — considers capacity, traffic, dependencies | Slower (multiple queries) | High |
| **PDB check + capacity headroom** | Good — covers most cases | Medium | Medium |

**Selected Approach: PDB check + capacity headroom verification.**

**Justification:** PDBs (PodDisruptionBudgets) are the standard Kubernetes mechanism for controlling disruption. Checking PDBs is fast and covers the most common case. Adding a capacity headroom check catches cases where PDBs don't exist or are misconfigured.

**Implementation:**

```python
class SLOAwareGate:
    def check(self, entity: Entity, action: RemediationAction) -> GateResult:
        if action.type in ('restart_pod', 'restart_process'):
            # Pod restart doesn't displace other pods — safe
            return GateResult(approved=True, reason="Restart doesn't affect other services")
        
        if action.type in ('drain_node', 'cordon_node', 'reboot_host', 'replace_instance'):
            # Node-level action — check impact on all services on this node
            node = get_node(entity.entity_id)
            pods_on_node = get_pods_on_node(node.name)
            
            for pod in pods_on_node:
                # Check PDB
                pdb = get_pdb_for_pod(pod)
                if pdb:
                    # Calculate: if we evict this pod, will PDB be violated?
                    available = pdb.status.currentHealthy - pdb.status.desiredHealthy
                    if available <= 0:
                        return GateResult(
                            approved=False,
                            reason=f"PDB violation: {pdb.metadata.name} has 0 disruptions allowed "
                                   f"(currentHealthy={pdb.status.currentHealthy}, "
                                   f"desiredHealthy={pdb.status.desiredHealthy})",
                            mitigation="Wait for another pod to become healthy, or manual override"
                        )
                
                # Check service minimum replicas
                rs = get_replicaset_for_pod(pod)
                if rs:
                    deployment = get_deployment_for_rs(rs)
                    if deployment:
                        current_ready = deployment.status.readyReplicas or 0
                        desired = deployment.spec.replicas
                        if current_ready <= 1:
                            return GateResult(
                                approved=False,
                                reason=f"Service {deployment.metadata.name} has only "
                                       f"{current_ready} ready replica(s); draining would "
                                       f"cause complete outage",
                                mitigation="Scale up the service first, or manual override"
                            )
            
            # Check cluster capacity headroom
            cluster_utilization = get_cluster_utilization(node.cluster)
            if cluster_utilization.cpu_pct > 85:
                return GateResult(
                    approved=False,
                    reason=f"Cluster CPU utilization at {cluster_utilization.cpu_pct}%; "
                           f"draining would push above safe threshold",
                    mitigation="Wait for capacity or add nodes"
                )
            
            return GateResult(approved=True, reason="All SLO checks passed")
        
        return GateResult(approved=True, reason="Action type doesn't require SLO check")
```

**Failure Modes:**
- **K8s API unavailable:** Can't check PDBs. Default: deny remediation (conservative). Alert operator.
- **PDB not configured for a service:** Skip PDB check for that service; rely on capacity headroom check. Also flag the service as "missing PDB" in the coverage report.
- **SLO data stale:** If deployment status is cached and stale (e.g., a pod just crashed but the cache still shows it as ready), the gate might approve incorrectly. Mitigation: cache TTL of 10 seconds; force-refresh before gate check.

**Interviewer Q&As:**

**Q1: What if the SLO gate blocks all remediations and the node gets worse?**
A: The escalation timer continues even when remediation is blocked. If auto-remediation is blocked for more than the Tier 2 timeout (e.g., 15 minutes), the system escalates to alerting the team, then paging on-call. The human can then decide: override the SLO gate (accepting the risk), scale up the service first and then approve remediation, or take a different action.

**Q2: How do you handle services without PDBs?**
A: We enforce PDB requirements through policy: tier-1 and tier-2 services must have PDBs (enforced by admission webhook). For services without PDBs, the SLO gate uses a heuristic: if the service has > 3 replicas, allow draining 1 at a time; if <= 3 replicas, require human approval.

**Q3: What about stateful services where draining impacts data availability?**
A: For StatefulSets with persistent volumes (MySQL, Elasticsearch), the SLO gate performs additional checks: (1) Replication status (MySQL: is the slave caught up?). (2) Shard allocation (ES: do all shards have replicas?). (3) Quorum (etcd: will we still have quorum after draining?). If any of these would be violated, remediation is blocked.

**Q4: How fast is the SLO gate? Does it add significant latency to remediation?**
A: The gate performs 3-5 K8s API calls (list pods on node, get PDBs, get deployments) + 1 cluster utilization query. Each call takes 10-50ms. Total gate latency: 50-250ms. Negligible compared to the remediation action itself (seconds to minutes).

**Q5: Can the SLO gate be bypassed in an emergency?**
A: Yes. `healthctl remediate entity-123 --action=drain --bypass-slo-gate --reason="emergency per INC-456"`. The bypass is logged in the audit trail. Only operators with the `sre-oncall` or `platform-admin` role can bypass.

**Q6: How does the SLO gate interact with multiple simultaneous remediations?**
A: The gate checks current state, which includes the impact of other in-progress remediations. If node A is being drained and 3 pods from service X are evicted, when node B (which also has pods from service X) is evaluated, the gate sees the reduced replica count from the node A drain and may block node B's drain. This prevents cascading disruption.

---

### 6.4 Escalation Policy Engine

**Why it's hard:** Automated remediation resolves ~80% of known failure modes. For the remaining 20%, the system must escalate to humans in a timely and non-annoying way. Too aggressive (page on-call immediately): alert fatigue. Too conservative (wait 2 hours): prolonged outage. The escalation policy must be configurable per service tier, per failure type, and per time of day.

**Implementation:**

```python
class EscalationEngine:
    def evaluate(self, entity: Entity, failure: DetectedFailure):
        policy = get_escalation_policy(entity.entity_type, entity.service_tier)
        current_tier = entity.escalation_tier
        
        if current_tier >= len(policy.tiers):
            # Already at the highest escalation tier — nothing more to do
            return
        
        current_tier_config = policy.tiers[current_tier]
        tier_start = entity.last_state_change  # when this tier started
        
        if current_tier_config.timeout_sec is not None:
            elapsed = (now() - tier_start).total_seconds()
            if elapsed > current_tier_config.timeout_sec:
                # Time to escalate to next tier
                next_tier = current_tier + 1
                next_tier_config = policy.tiers[next_tier]
                
                self.execute_escalation(entity, failure, next_tier_config)
                entity.escalation_tier = next_tier
                entity.last_state_change = now()
                save_entity_state(entity)
    
    def execute_escalation(self, entity, failure, tier_config):
        action = tier_config.action
        
        if action == "auto_remediate":
            trigger_remediation(entity, tier_config.remediation_type)
        
        elif action == "alert_team_slack":
            slack.send(
                channel=entity.owner_team_channel,
                message=f":warning: Health check failure for {entity.entity_id}\n"
                        f"Score: {entity.composite_score}\n"
                        f"Failing checks: {entity.checks_failing}\n"
                        f"Duration: {entity.failure_duration}\n"
                        f"Auto-remediation: {entity.last_remediation_result}\n"
                        f"Escalation tier: {entity.escalation_tier}"
            )
        
        elif action == "page_oncall":
            pagerduty.trigger(
                service=entity.owner_team_pd_service,
                severity="high",
                summary=f"Health check failure: {entity.entity_id} (score: {entity.composite_score})",
                details={
                    "entity": entity.entity_id,
                    "score": entity.composite_score,
                    "failing_checks": entity.failing_check_details,
                    "remediation_history": entity.recent_remediations,
                    "runbook_url": entity.runbook_url
                }
            )
        
        elif action == "page_senior_oncall":
            pagerduty.trigger(
                service=entity.owner_team_pd_service,
                severity="critical",
                escalation_policy="senior_oncall",
                summary=f"ESCALATED: {entity.entity_id} unhealthy for {entity.failure_duration}"
            )
```

**Example Escalation Timeline (Tier-1 Host):**

```
T+0min:   Host health check fails
T+0.5min: Confirmed failure (3 consecutive failures)
T+1min:   Tier 1: Auto-remediate (restart process)
T+6min:   Process restart didn't fix it
T+6min:   Tier 2: Auto-remediate (reboot host)
T+16min:  Reboot didn't fix it
T+16min:  Tier 3: Alert team on Slack
T+31min:  No human response
T+31min:  Tier 4: Page on-call
T+61min:  On-call hasn't resolved
T+61min:  Tier 5: Page senior on-call
```

**Interviewer Q&As:**

**Q1: How do you prevent escalation fatigue?**
A: (1) The first tier is always auto-remediation — resolve it automatically if possible. (2) Escalation messages include context (failing checks, remediation history, runbook links) so the operator doesn't waste time diagnosing. (3) We track escalation rate per team and review monthly: if a team is getting > 10 escalations/week, something systemic is wrong.

**Q2: How do you handle escalation during off-hours?**
A: Escalation policies can have time-of-day variants. During business hours: alert Slack first. During off-hours: skip Slack, go directly to PagerDuty (people aren't watching Slack at 3 AM). Some teams define a "night policy" with higher thresholds before paging (to avoid waking people for minor issues).

**Q3: What if the on-call person acknowledges but doesn't resolve?**
A: After PagerDuty acknowledgment, a resolution timer starts (configurable, default 30 minutes). If the entity is still unhealthy after the resolution timer, escalation continues to the next tier (senior on-call). The PagerDuty incident remains open.

**Q4: How do you handle escalation for infrastructure issues that affect multiple entities?**
A: We de-duplicate escalations. If 50 pods in the same service are unhealthy, we send one escalation (not 50): "50 of 100 orders-service pods are unhealthy." The escalation message includes the count and affected entity list. Root cause correlation (same node, same AZ, same deployment) is included.

**Q5: Can teams customize their escalation policies?**
A: Yes. Each team defines their escalation policy per service tier. The platform provides sensible defaults that teams can override. Policy changes are audited and require team lead approval.

**Q6: How do you handle escalation when the PagerDuty/Slack integration is down?**
A: Fallback to email as a last resort. The escalation engine sends to all configured channels. If all channels fail, it logs the escalation locally and retries every 60 seconds. A separate monitor checks that the escalation engine itself is healthy.

---

## 7. Scheduling & Resource Management

### Health Check Scheduling

Health checks are distributed across agents (DaemonSets on nodes, sidecars in pods). Scheduling must ensure:
- **Jitter:** Don't run all checks at exactly the same time (10s interval → each check starts at 10s + random(0, 2s) offset).
- **Staggering:** For a node with 20 pods, don't run all 20 HTTP health checks simultaneously — stagger them across the 10-second window.
- **Resource budgets:** Health check agents have CPU/memory limits (100m CPU, 128 MB). If there are too many checks to fit in the budget, checks are prioritized by weight.

### Remediation Resource Requirements

| Action | Resource Impact | Duration | Concurrency Limit |
|---|---|---|---|
| Restart pod | Minimal — K8s handles it | < 30s | No limit (K8s handles) |
| Restart process | Minimal — systemd handles it | < 10s | 10 per host |
| Drain node | Evicts all pods — requires spare capacity | 1-5 min | 1 per rack, 5% of cluster |
| Reboot host | Host offline during reboot | 2-5 min | 1 per rack |
| Replace instance | New host must be provisioned | 5-30 min | 2% of cluster |

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Strategy | Scale Unit |
|---|---|---|
| Health Check Agents | DaemonSet — 1 per node | Scales with fleet |
| Event Normalizer | Kafka consumer group — scale with partitions | 1 consumer per 10 partitions |
| Flap Detector | Stateless, scales with event volume | 1 instance per 50K entities |
| Composite Score Calculator | Stateless, scales with entity count | 1 instance per 100K entities |
| Remediation Decision Engine | Leader-elected, sharded by cluster | 1 per cluster |
| SLO-Aware Gate | Stateless, called per remediation | 3 instances (HA) |
| Escalation Manager | Singleton per region | 1 active + 2 standby |
| REST API | Stateless behind LB | 3-20 instances based on traffic |

### Database Scaling

**MySQL:** Single primary + 2 read replicas. Write volume is manageable (check definitions change infrequently; remediation records are ~210/sec). Read replicas serve API queries.

**Redis Cluster:** 6 nodes (3 masters, 3 replicas). Sharding by entity_id (hash slot). At 210K writes/sec for composite scores, each master handles ~70K ops/sec — well within Redis capacity.

**Elasticsearch:** 10-node cluster for health events. Index-per-day, 5 primary shards per index. ILM handles retention. Hot tier on SSD, warm/cold on HDD/S3.

### Caching

| Layer | Technology | Data | TTL | Purpose |
|---|---|---|---|---|
| L1: In-process | HashMap | Check definitions (for agents) | 60s | Avoid querying MySQL on every check execution |
| L2: Redis | Redis Cluster | Composite scores, flap state, cooldowns | N/A (primary store for these) | Real-time state access |
| L3: API cache | Redis | API query results | 10s | Reduce API backend load for dashboards |

### Interviewer Q&As

**Q1: How do you handle a Redis Cluster node failure?**
A: Redis Cluster with 3 masters and 3 replicas tolerates 1 master failure. The replica auto-promotes. If we lose the composite scores temporarily, the calculator recomputes from the latest check states in < 10 seconds. Composite scores are a derived computation, not source-of-truth data.

**Q2: How do you handle Kafka consumer lag in the health event pipeline?**
A: Monitor consumer lag with Burrow. If lag exceeds 30 seconds, scale up consumers (add instances to the consumer group). If lag exceeds 5 minutes, old events are dropped (health events are ephemeral — a 5-minute-old check result is stale). The composite score calculator always uses the latest event, not replaying old ones.

**Q3: What's the bottleneck at 10M check instances?**
A: The bottleneck is Kafka throughput: 1.05M events/sec × 200 bytes = ~210 MB/sec. A 30-broker Kafka cluster with 256 partitions handles this comfortably. The second bottleneck is Redis writes for composite scores: 210K/sec × 200 bytes = 42 MB/sec, well within Redis Cluster capacity.

**Q4: How do you handle a new region with no historical data?**
A: New regions start with health checks immediately — no cold start for basic checks (process, port, HTTP). The composite score starts at 50 (unknown) and adjusts as checks report. Flap detection thresholds are inherited from similar existing regions. ML models (if used) fall back to fleet-wide models until region-specific data is available.

**Q5: Can the system handle 10x traffic spike (e.g., a global event causing widespread failures)?**
A: Kafka absorbs the spike (it's designed for burst traffic). Consumers may fall behind temporarily but catch up. The remediation pipeline is rate-limited by design (blast radius controls), so even during a spike, remediations are throttled. The API layer (dashboard queries) may slow down; we shed non-critical queries under load.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery |
|---|---|---|---|
| Health check agent crash on a node | No check results for that node | Missing heartbeat from agent | Systemd auto-restart; K8s DaemonSet recreates |
| Kafka broker failure | Partial event ingestion loss | Under-replicated partitions | Kafka auto-reassigns partitions |
| Redis node failure | Composite scores briefly stale | Redis Cluster health check | Auto-promote replica in < 15s |
| MySQL primary failure | Can't write new check defs or remediation records | ProxySQL detects | MySQL Orchestrator promotes replica |
| Elasticsearch node failure | Health history search degraded | ES cluster health | Shard replication promotes replicas |
| Remediation engine crash | In-progress remediations stall | Leader election lost | Standby acquires lease; in-progress actions are idempotent |
| Escalation service crash | Escalations not sent | Liveness probe | K8s restarts pod; missed escalations are retried from state |
| All health checks for an entity fail simultaneously | Entity incorrectly marked healthy | Meta-health-check (check coverage monitoring) | Alert: "Entity X has 0 reporting checks" |

### Automated Recovery

- All components are deployed as Kubernetes Deployments with health probes.
- State is in external stores (MySQL, Redis, ES, Kafka) — services are stateless.
- Leader-elected components (Remediation Engine, Escalation Manager) have automatic failover.

### Retry Strategy

| Operation | Strategy |
|---|---|
| Health check execution | No retry — next check runs on schedule |
| Kafka produce (health event) | Idempotent producer, 5 retries with backoff |
| Redis write (composite score) | 3 retries, 100ms delay |
| MySQL write (remediation record) | 3 retries on deadlock, fail on other errors |
| Remediation execution (restart) | 2 retries, 30s delay, then escalate |
| PagerDuty notification | 5 retries, 60s delay |

### Circuit Breaker

- Remediation circuit breaker per cluster: trips if > 30% of remediations fail in 1 hour. Resets after 10 minutes.
- API circuit breaker for downstream services (Redis, MySQL): trips after 5 consecutive timeouts. Resets after 30 seconds.

### Consensus & Coordination

- Remediation Engine uses etcd lease for leader election (15s TTL).
- Redis distributed locks for per-entity remediation (prevent concurrent remediations on the same entity).

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `healthcheck_execution_rate` | Counter per check_type | Drop > 50% from baseline |
| `healthcheck_execution_latency_ms` | Histogram per check_type | P99 > 10s |
| `healthcheck_failure_rate` | Gauge per entity_type | > 5% of checks |
| `composite_score_distribution` | Histogram | > 10% entities below 50 |
| `flapping_entity_count` | Gauge per entity_type | > 100 |
| `remediation_rate` | Counter per action_type | Spike > 3x baseline |
| `remediation_success_rate` | Gauge per action_type | < 80% |
| `remediation_latency_sec` | Histogram per action_type | P99 > 5min (drain) |
| `escalation_count` | Counter per tier | Tier 4+ > 0 |
| `slo_gate_rejection_rate` | Gauge | > 20% (too many blocked remediations) |
| `health_check_coverage_pct` | Gauge per tier | < 95% for tier-1 |
| `kafka_consumer_lag_sec` | Gauge per consumer group | > 30s |
| `redis_operation_latency_ms` | Histogram | P99 > 10ms |

### Distributed Tracing

- Each health check event carries a trace ID.
- Remediation traces span: health event → score calculation → remediation decision → SLO check → execution → verification.
- Traces exported to Jaeger.

### Logging

Structured JSON. Key events:
- `health_check_failed`: entity_id, check_id, error, consecutive_failures.
- `composite_score_changed`: entity_id, old_score, new_score, new_status.
- `flap_detected`: entity_id, transition_count, suppression_duration.
- `remediation_triggered`: entity_id, action_type, trigger_check, trigger_score.
- `remediation_completed`: entity_id, action_type, result, duration_ms.
- `escalation_triggered`: entity_id, from_tier, to_tier, notification_channel.
- `slo_gate_blocked`: entity_id, action_type, reason.

### Alerting

| Alert | Severity | Routing |
|---|---|---|
| Health check coverage < 95% for tier-1 | P2 | Platform team |
| > 10% entities unhealthy in a cluster | P1 | On-call SRE |
| Flap storm (> 100 entities flapping) | P2 | Platform team |
| Remediation success rate < 80% | P2 | Platform team |
| Escalation to tier-4 (senior page) | P1 | Senior on-call |
| SLO gate blocking all remediations for > 30 min | P2 | On-call SRE |
| Kafka consumer lag > 5 min | P2 | Platform team |

---

## 11. Security

### Auth & AuthZ

| Actor | Auth Method | Permissions |
|---|---|---|
| Health check agents | mTLS (service cert) | Write health events to Kafka |
| Composite score calculator | mTLS | Read Kafka, write Redis |
| Remediation engine | mTLS + K8s ServiceAccount | Execute K8s actions (drain, delete pod) |
| API users (humans) | OIDC + RBAC | Check management, remediation trigger |
| Platform admins | OIDC + RBAC (admin role) | Modify escalation policies, override gates |

**RBAC Roles:**
- `healthcheck-viewer`: Read health state, check definitions.
- `healthcheck-editor`: Create/modify/delete check instances for their team's services.
- `healthcheck-operator`: Trigger remediation, override flap suppression, bypass SLO gate (with logging).
- `healthcheck-admin`: Modify templates, escalation policies, system configuration.

### Audit Logging

All state-modifying operations are audited:
- Check creation/modification/deletion: who, when, what changed.
- Remediation trigger: automated or manual, who triggered, what was the trigger condition.
- Escalation policy changes: who modified, old vs new policy.
- Override actions: flap suppression override, SLO gate bypass, manual health score override.

Audit logs are written to MySQL and replicated to an immutable compliance store (S3 with object lock).

---

## 12. Incremental Rollout Strategy

**Phase 1 (Week 1-4): Check Registry + HTTP Checks**
- Deploy health check registry and HTTP check templates.
- Teams register /healthz checks for their services.
- Composite scores computed but not used for remediation (observe mode).

**Phase 2 (Week 5-8): Deep Health Checks + Composite Scoring**
- Add deep health check templates.
- Enable composite scoring with configurable weights.
- Dashboard for health visibility.

**Phase 3 (Week 9-12): Flap Detection + Alerting**
- Enable flap detection.
- Enable escalation for unhealthy entities (alert only, no auto-remediation).
- Tune thresholds based on alert quality feedback.

**Phase 4 (Week 13-16): Automated Remediation (Tier 3-4)**
- Enable auto-remediation for tier-3/4 services.
- SLO-aware gate active.
- Cooldown periods configured.

**Phase 5 (Week 17-24): Full Remediation (All Tiers)**
- Enable auto-remediation for tier-1/2 with human approval gate.
- Remove approval gate after 30 days of success.
- Coverage enforcement: tier-1 services must have >= 95% check coverage.

### Rollout Q&As

**Q1: How do you get teams to register health checks?**
A: Carrot: provide templates that make registration trivial (one YAML file). Stick: tier-1/2 services must have registered health checks (enforced via CI/CD admission). Visibility: publish coverage leaderboard.

**Q2: How do you handle teams with poor health check implementations?**
A: Review process: health check definitions are reviewed by the platform team during onboarding. Common anti-patterns (liveness checking DB, no deep health, no readiness probe) are flagged. Training materials and examples provided.

**Q3: What's the rollback plan?**
A: Disable auto-remediation globally: `healthctl remediation disable --scope=global`. The system continues monitoring and alerting but takes no automated actions.

**Q4: How do you measure the quality of health checks?**
A: (1) False positive rate: how often does a check fail when the service is actually serving traffic fine (correlate with business metrics). (2) Detection time: how quickly does the check detect a real failure (correlate with incident timeline). (3) Coverage gaps: use chaos experiments to inject known failures and verify the checks detect them.

**Q5: What's the expected toil reduction from this system?**
A: Conservative estimate: 60-80% reduction in time spent on "known failure" remediations (restart process, drain bad node). SRE team shifts from fighting fires to preventing them.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Choice | Rationale | Risk | Mitigation |
|---|---|---|---|---|---|
| Health event transport | HTTP push vs Kafka | Kafka | Decoupled, durable, handles backpressure | Kafka operational overhead | Managed Kafka or dedicated team |
| Real-time state store | MySQL vs Redis | Redis | Sub-millisecond reads for composite scores, TTL-based cooldowns | Redis is ephemeral | Scores are derived — recalculate on loss |
| Composite score algorithm | Binary (up/down) vs weighted average | Weighted average | Richer signal, fewer false actions | Weight tuning complexity | Calibrate with historical data |
| Flap detection | Simple counter vs Nagios-style | Combined (counter + hysteresis + exponential suppression) | Most accurate, prevents remediation storms | Complexity | Thorough testing with replayed data |
| SLO gate | No gate vs PDB-only vs full analysis | PDB + headroom | Good accuracy with acceptable complexity | PDB may not exist for all services | Enforce PDB via admission webhook |
| Escalation | Fixed policy vs configurable | Configurable per entity_type + service_tier | Different tiers need different response times | Configuration complexity | Sensible defaults + team override |
| Check execution | Centralized pull vs distributed push | Distributed push (agents) | Scales with fleet, fault-isolated | Agent failure = blind spot | DaemonSet auto-restart + heartbeat monitoring |
| Storage for health events | MySQL vs Elasticsearch vs InfluxDB | Elasticsearch | Best for time-series search and aggregation | Another system to maintain | Existing ES cluster, shared with logging |

---

## 14. Agentic AI Integration

### AI-Powered Health Check Generation

**Problem:** Most teams copy-paste basic health check configurations. They miss important checks specific to their service's architecture and failure modes.

**Solution:** An AI agent analyzes service architecture and generates tailored health check configurations.

```python
class AIHealthCheckGenerator:
    def generate(self, service: Service) -> List[HealthCheckConfig]:
        context = {
            "service_name": service.name,
            "language": service.language,  # Java, Python, Go
            "framework": service.framework,  # Spring Boot, Django, etc.
            "dependencies": service.get_dependencies(),
            "ports": service.get_exposed_ports(),
            "existing_checks": service.get_health_checks(),
            "incident_history": service.get_incidents(last_180d=True)
        }
        
        prompt = f"""
        You are a health check design expert. Given this service's architecture,
        generate comprehensive health check configurations.
        
        Service: {context}
        
        For each check, provide:
        1. Check type (process, port, http, deep_health, custom)
        2. Configuration (endpoint, expected response, timeout)
        3. Interval and threshold settings
        4. Weight in composite score
        5. Why this check is important
        
        Requirements:
        - Do NOT make liveness checks dependent on external services
        - Deep health checks should test actual dependency connectivity
        - Include both fast checks (process, port) and thorough checks (deep health)
        - Consider the service's incident history — what past failures would these checks detect?
        """
        return self.llm.invoke(prompt)
```

### AI-Powered Root Cause Correlation

**Problem:** When multiple entities are unhealthy simultaneously, finding the root cause requires correlating many signals.

```python
class AIRootCauseAnalyzer:
    def analyze(self, unhealthy_entities: List[Entity]) -> RootCauseAnalysis:
        context = {
            "entities": [
                {
                    "id": e.entity_id,
                    "type": e.entity_type,
                    "score": e.composite_score,
                    "failing_checks": e.failing_checks_detail,
                    "location": {"rack": e.rack, "az": e.az},
                    "recent_changes": e.recent_changes
                }
                for e in unhealthy_entities
            ],
            "topology": get_network_topology(unhealthy_entities),
            "recent_deployments": get_recent_deployments(),
            "infrastructure_events": get_recent_infra_events()
        }
        
        prompt = f"""
        Multiple infrastructure entities are unhealthy simultaneously. 
        Analyze the pattern and identify the most likely root cause.
        
        Unhealthy entities: {context['entities']}
        Network topology: {context['topology']}
        Recent deployments: {context['recent_deployments']}
        Infrastructure events: {context['infrastructure_events']}
        
        Provide:
        1. Most likely root cause (with confidence)
        2. Affected blast radius
        3. Recommended remediation (entity-level or root-cause-level)
        4. What additional information would help confirm the diagnosis
        """
        return self.llm.invoke(prompt)
```

### AI-Powered Threshold Tuning

**Problem:** Health check thresholds (failure_threshold, interval, weight) are typically set by intuition. Suboptimal thresholds cause false positives or delayed detection.

**Solution:** ML model trained on historical health check data to recommend optimal thresholds.

```python
class ThresholdOptimizer:
    def optimize(self, check_id: str) -> OptimalThresholds:
        # Retrieve historical data for this check
        history = elasticsearch.query(f"check_id:{check_id}", last_180d=True)
        
        # Features: check results, concurrent incidents, remediation outcomes
        # Label: was the check result correct (compared to actual incidents)?
        
        # Optimize for: minimize detection delay while keeping false positive rate < 0.1%
        
        best_failure_threshold = binary_search(
            min_val=1, max_val=10,
            objective=lambda ft: false_positive_rate(history, ft) < 0.001
        )
        
        best_interval = find_optimal_interval(
            history, target_detection_delay_sec=30
        )
        
        return OptimalThresholds(
            failure_threshold=best_failure_threshold,
            interval_sec=best_interval,
            confidence=compute_confidence(history)
        )
```

### Guard Rails

- AI-generated health checks are created in "observe-only" mode for 7 days before activation.
- AI threshold recommendations are presented to the team for approval; not applied automatically.
- AI root cause analysis is advisory; it doesn't trigger automated remediation.
- All AI outputs are logged with model version, prompt, and confidence score for audit.

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through the lifecycle of a health check from creation to remediation.**
A: A team creates a health check definition (via API/CLI/YAML) specifying the target entity, check type (HTTP), endpoint (/healthz), interval (10s), timeout (5s), failure threshold (3), and weight in the composite score. The check definition is stored in MySQL and distributed to the health check agent on the target node. The agent executes the check every 10 seconds, publishing results to Kafka. The composite score calculator ingests results, updates the entity's composite score in Redis. If the score drops below the "unhealthy" threshold (< 50), the remediation decision engine evaluates: checks cooldown (was this entity recently remediated?), checks flap status (is it oscillating?), checks the SLO-aware gate (would remediation harm downstream?). If all checks pass, it triggers remediation (e.g., restart the pod). The action is logged in MySQL with full context. If remediation fails, the escalation engine advances to the next tier (alert team → page on-call).

**Q2: How do you handle a health check that's consuming too many resources?**
A: Three interventions: (1) Increase the check interval (10s → 30s reduces resource consumption by 3x). (2) Reduce the check timeout (5s → 2s frees the check thread faster). (3) Switch to a lighter check type (deep health → HTTP). We also enforce resource budgets on the health check agent (100m CPU, 128 MB). If the agent hits its limits, checks are prioritized by weight — high-weight checks run first, low-weight checks may be deferred.

**Q3: What's the difference between a composite health score and a simple up/down status?**
A: A composite score captures nuance. A service might have 3 of 5 checks passing (HTTP works, but DB and downstream are degraded) — the composite score of 65 tells you it's degraded but partially functional. A simple up/down would say "down" (since some checks fail), which might trigger a full remediation (restart) when the service is actually serving 80% of requests fine. The composite score enables proportional response: degraded → investigate. Unhealthy → remediate.

**Q4: How do you handle services where the deep health check always fails because a dependency is flaky?**
A: Two approaches: (1) Reduce the weight of the flaky dependency in the deep health response (e.g., payment-gateway: weight 0.05 in composite). (2) Implement a circuit breaker for the dependency check — if it fails N times, mark it as "degraded" rather than "failed" and don't count it in the composite score until it's investigated. (3) File a ticket for the dependency team and track it as "tech debt."

**Q5: How do you ensure health checks don't become a single point of failure?**
A: Multiple layers of redundancy: (1) K8s kubelet still runs its own probes independently of our system. (2) Health check agents run as a DaemonSet — if one crashes, only that node loses coverage. (3) The composite score calculator has multiple instances. (4) If the entire health check system goes down, the self-healing system falls back to basic K8s health (kubelet probes, node conditions).

**Q6: How would you design a health check for a Kafka consumer?**
A: Kafka consumers are tricky because they might be alive (process running) but not consuming (stuck on a rebalance, or the consumer group is lagged). Check layers: (1) Process check: is the consumer process running? (2) Consumer lag check: query Kafka consumer group offsets (`kafka-consumer-groups.sh --describe`). If lag is growing, the consumer is stuck. (3) Custom check: the consumer application exposes a `/healthz` that reports its last-consumed offset timestamp. If the timestamp is more than 5 minutes old, the consumer is unhealthy.

**Q7: How do you handle health check for a MySQL replica?**
A: Layers: (1) Port check (TCP 3306). (2) SQL check (`SELECT 1`). (3) Replication check (`SHOW SLAVE STATUS` — check Seconds_Behind_Master, Slave_IO_Running, Slave_SQL_Running). (4) Query performance check (run a representative SELECT and measure latency). If replication lag exceeds 60 seconds, mark as degraded. If replication stops (IO thread not running), mark as unhealthy.

**Q8: What about health checks for Elasticsearch nodes?**
A: (1) Port check (TCP 9200). (2) HTTP check (`GET /_cluster/health`). (3) Node-level check (`GET /_nodes/{node_id}/stats` — check JVM heap, disk usage). (4) Shard check: verify the node holds its expected shard count. If a data node has 0 shards, it might have been excluded. Composite score weights: cluster health (0.4), node stats (0.3), shard count (0.2), port (0.1).

**Q9: How do you handle a health check during a graceful shutdown?**
A: During graceful shutdown, the readiness probe should fail (service removes itself from the LB) but the liveness probe should still pass (don't restart a gracefully shutting down process). The deep health check should also be suspended (the service is intentionally terminating connections). We detect graceful shutdown by watching the SIGTERM signal handler or the Kubernetes pod termination state.

**Q10: How do you prevent a cascade where all health checks fail simultaneously?**
A: If > 50% of health checks for a given check type fail simultaneously, it's likely a problem with the check infrastructure (monitoring network down, Prometheus unreachable, agent bug), not with the targets. We meta-monitor: track the percentage of passing checks globally. If it drops suddenly, alert the platform team and suppress remediation (don't restart half the fleet because the health check system is broken).

**Q11: How do you handle health checks in a blue-green deployment?**
A: During blue-green, both the blue (old) and green (new) environments exist. Health checks run on both. The green environment's checks start in "observe-only" mode during validation. Once traffic is switched to green, health checks become active. Blue's checks are deactivated after blue is torn down. The composite score for the green deployment starts at 50 (unknown) and adjusts as checks report.

**Q12: How do you expose health check results to service owners?**
A: A web dashboard showing: (1) Per-service composite score timeline. (2) Per-check pass/fail history. (3) Current flapping entities. (4) Remediation history. (5) Coverage report. All data comes from Elasticsearch (historical) and Redis (current state). Teams can also subscribe to webhook notifications for their services.

**Q13: How would you add support for gRPC health checks?**
A: gRPC has a standard health checking protocol (`grpc.health.v1.Health/Check`). The health check agent supports this natively: send a gRPC health check request, expect a `SERVING` response. Configuration is similar to HTTP: specify the gRPC service name, port, and timeout. The response maps to our standard status: `SERVING` → passing, `NOT_SERVING` → failing, `UNKNOWN` → unknown.

**Q14: How do you handle false negatives (service is broken but health checks pass)?**
A: Multiple strategies: (1) Deep health checks test actual functionality, not just process liveness. (2) Canary health checks: periodically send a real (but synthetic) request through the full service stack and verify the response. (3) Correlate health checks with business metrics: if the order creation rate drops 50% but all health checks pass, something is wrong with the checks. (4) Post-incident review: for every incident, verify whether health checks detected it and how quickly.

**Q15: What's the cost of running this health check system?**
A: Resource overhead: ~100m CPU + 128 MB per node (DaemonSet) × 100K nodes = 10K CPU cores + 12.8 TB memory. That's about 0.5% of a 2M-core fleet — very modest. Infrastructure cost: Kafka (existing), Redis (small cluster), Elasticsearch (shared with logging), MySQL (small instance). Total: < 0.1% of fleet compute cost for comprehensive health monitoring.

**Q16: How do you handle environment-specific health check configurations (dev vs staging vs prod)?**
A: Health check definitions include an `environment` field. Templates have per-environment overrides: dev uses longer intervals (30s) and lower thresholds (1 failure), staging matches prod settings, prod uses the strictest settings (10s interval, 3 failures). The deployment pipeline ensures that health checks are deployed alongside the service they monitor.

---

## 16. References

1. Kubernetes Liveness, Readiness, and Startup Probes: https://kubernetes.io/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/
2. gRPC Health Checking Protocol: https://github.com/grpc/grpc/blob/master/doc/health-checking.md
3. Nagios Flap Detection Algorithm: https://assets.nagios.com/downloads/nagioscore/docs/nagioscore/3/en/flapping.html
4. Kubernetes PodDisruptionBudget: https://kubernetes.io/docs/tasks/run-application/configure-pdb/
5. Microsoft Health Endpoint Monitoring Pattern: https://learn.microsoft.com/en-us/azure/architecture/patterns/health-endpoint-monitoring
6. Google SRE Book — Chapter 6: Monitoring Distributed Systems: https://sre.google/sre-book/monitoring-distributed-systems/
7. Kubernetes Node Problem Detector: https://github.com/kubernetes/node-problem-detector
8. Netflix Zuul — Health Check Integration: https://netflixtechblog.com/
9. "Designing Distributed Systems" by Brendan Burns — Health Checking patterns
10. PagerDuty API: https://developer.pagerduty.com/api-reference/
