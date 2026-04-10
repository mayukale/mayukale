# System Design: Chaos Engineering Platform

> **Relevance to role:** A cloud infrastructure platform engineer must ensure the platform can withstand real-world failures — not just handle them in theory. Chaos engineering systematically injects failures (VM termination, network latency, disk fill, pod kill) into production and pre-production environments to validate resilience. This is the practice Netflix pioneered with Chaos Monkey and evolved through Chaos Kong (AZ failure) and ChAP (Chaos Automation Platform). For a role managing bare-metal IaaS, Kubernetes, and critical databases, chaos engineering validates that self-healing, failover, and health check systems actually work under pressure.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Experiment Definition:** Define chaos experiments as code (YAML/JSON) with hypothesis, injection type, target selection, stop conditions, and success criteria.
2. **Injection Types:** Support: process kill, pod kill, node reboot, network latency injection, packet loss, CPU stress, memory stress, disk fill, DNS failure, AZ simulation failure, Kafka broker kill, MySQL replica kill.
3. **Blast Radius Control:** Limit experiments by percentage of targets, specific AZs, specific services, specific tiers, and absolute maximum affected instances.
4. **Steady-State Hypothesis:** Define measurable steady-state (e.g., "p99 latency < 200ms, error rate < 0.1%") that is validated before, during, and after the experiment.
5. **Automated Abort:** If steady-state deviation exceeds safety thresholds, automatically halt the experiment and roll back injections.
6. **GameDay Orchestration:** Schedule and manage multi-step chaos exercises simulating complex failure scenarios (e.g., simultaneous network partition + disk failure).
7. **Results Analysis:** Compare pre/during/post metrics; determine if the system degraded gracefully or failed catastrophically.
8. **Integration with Feature Flags:** Disable chaos experiments for production tier-1 services via feature flags. Enable/disable at granular levels.
9. **Experiment History:** Store all experiment results for trend analysis: "Are we getting more resilient over time?"
10. **Approval Workflow:** Require team lead approval for production experiments. Auto-approve for staging.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Experiment start latency | < 5 seconds from trigger to first injection |
| Abort latency | < 3 seconds from abort signal to injection rollback |
| Maximum blast radius | Configurable; default: 10% of target pool |
| Platform availability | 99.99% (chaos platform itself must be highly available) |
| Concurrent experiments | Up to 50 simultaneously across the fleet |
| Experiment duration | 1 minute to 24 hours |
| Scale | Support 100K+ targetable entities (hosts, pods, services) |

### Constraints & Assumptions
- Kubernetes clusters with RBAC allow the chaos platform to create/delete pods, modify network policies.
- Bare-metal hosts are accessible via SSH for in-band injection and IPMI for out-of-band injection.
- Prometheus/Grafana are the monitoring stack; metrics are queryable via PromQL.
- Feature flag system (e.g., LaunchDarkly, Unleash, or custom) is available.
- Kafka and MySQL are present as critical infrastructure; chaos experiments target them specifically.
- Assumption: staging environments are topologically similar to production (otherwise chaos results in staging are not meaningful).

### Out of Scope
- Chaos injection into third-party SaaS services.
- Security chaos (penetration testing, fuzzing) — different discipline.
- Business logic error injection (bad data, invalid requests) — application team responsibility.
- Building the monitoring/alerting stack (assumed to exist).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|---|---|---|
| Targetable entities | 100K hosts + 2M pods + 500 DB clusters | ~2.1M targets |
| Experiments per week | 50 (staging) + 10 (production) = 60 | 60/week |
| Experiments per day | ~9/day (average), peaks at 30/day during GameDays | 9-30/day |
| Injections per experiment | Average 50 targets per experiment | ~450 injections/day |
| API calls/sec | 50 users × 5 requests/min average | ~4/sec (bursty) |
| Metric queries per experiment | 10 metrics × every 10s × 30min avg experiment | 1,800 queries/experiment |

### Latency Requirements

| Operation | Target | P99 |
|---|---|---|
| Experiment creation (API) | < 200ms | < 500ms |
| Experiment start (trigger → first injection) | < 5s | < 10s |
| Injection execution (per target) | < 2s | < 5s |
| Steady-state check | < 5s | < 10s |
| Abort (signal → rollback complete) | < 3s | < 5s |
| Results computation | < 30s | < 60s |
| Dashboard load | < 2s | < 5s |

### Storage Estimates

| Data Type | Calculation | Result |
|---|---|---|
| Experiment definitions | 60/week × 52 weeks × 10 KB | ~30 MB/year |
| Experiment results (detailed) | 60/week × 52 × 100 KB (metrics snapshots) | ~300 MB/year |
| Injection logs | 450 injections/day × 365 × 2 KB | ~330 MB/year |
| Metric snapshots (during experiments) | 9 experiments/day × 1,800 queries × 1 KB | ~6 GB/year |
| Total | | ~7 GB/year (very modest) |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Injection commands (control plane → agents) | 450/day × 1 KB | Negligible |
| Metric polling (chaos platform → Prometheus) | 180 queries/experiment × 10/sec × 1 KB response | ~1.8 KB/sec per experiment |
| Agent heartbeats | 100K agents × 100 bytes × every 30s | ~333 KB/sec |

---

## 3. High Level Architecture

```
┌────────────────────────────────────────────────────────────────────────────┐
│                       CHAOS ENGINEERING PLATFORM                           │
│                                                                            │
│  ┌─────────────────┐  ┌──────────────────┐  ┌──────────────────────────┐  │
│  │  Experiment      │  │  Experiment       │  │  GameDay Orchestrator   │  │
│  │  API & UI        │  │  Engine           │  │  (multi-step scenarios) │  │
│  │  (define, list,  │  │  (execute, abort, │  │                         │  │
│  │   approve, view) │  │   monitor)        │  │                         │  │
│  └────────┬─────────┘  └────────┬──────────┘  └───────────┬─────────────┘  │
│           │                     │                          │                │
│  ┌────────▼─────────────────────▼──────────────────────────▼─────────────┐  │
│  │                     Orchestration Layer                               │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐               │  │
│  │  │ Target       │  │ Steady-State │  │ Safety       │               │  │
│  │  │ Selector     │  │ Validator    │  │ Controller   │               │  │
│  │  │ (filter,     │  │ (PromQL      │  │ (blast       │               │  │
│  │  │  percentage, │  │  evaluation) │  │  radius,     │               │  │
│  │  │  exclude)    │  │              │  │  abort logic)│               │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘               │  │
│  └──────────────────────────────┬────────────────────────────────────────┘  │
│                                 │                                           │
│  ┌──────────────────────────────▼────────────────────────────────────────┐  │
│  │                        Injection Drivers                              │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐  │  │
│  │  │ K8s Pod  │ │ Network  │ │ Resource │ │ Bare-    │ │ Service   │  │  │
│  │  │ Kill     │ │ Chaos    │ │ Stress   │ │ Metal    │ │ Kill      │  │  │
│  │  │ (delete  │ │ (tc,     │ │ (stress- │ │ (IPMI    │ │ (systemctl│  │  │
│  │  │  pod/    │ │  iptables│ │  ng, dd) │ │  reset,  │ │  stop,    │  │  │
│  │  │  evict)  │ │  netem)  │ │          │ │  power   │ │  kill -9) │  │  │
│  │  │          │ │          │ │          │ │  off)    │ │           │  │  │
│  │  └──────────┘ └──────────┘ └──────────┘ └──────────┘ └───────────┘  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │  Data & Integration Layer                                            │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐    │   │
│  │  │ MySQL    │  │ Elastic  │  │ Feature  │  │ Notification     │    │   │
│  │  │ (exp     │  │ Search   │  │ Flag     │  │ (Slack, PD,      │    │   │
│  │  │  defs,   │  │ (results,│  │ Service  │  │  email)          │    │   │
│  │  │  results)│  │  logs)   │  │          │  │                  │    │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────────────┘    │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────────┘

             ┌──────────────────────────────────────────────┐
             │          TARGET ENVIRONMENT                   │
             │                                               │
             │  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
             │  │ Chaos    │  │ Chaos    │  │ Chaos    │   │
             │  │ Agent    │  │ Agent    │  │ Agent    │   │
             │  │ (node 1) │  │ (node 2) │  │ (node N) │   │
             │  └──────────┘  └──────────┘  └──────────┘   │
             │                                               │
             │  ┌────────────────────────────────────────┐   │
             │  │ Monitoring (Prometheus + Grafana)       │   │
             │  └────────────────────────────────────────┘   │
             └──────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Experiment API & UI** | Web interface and REST API for creating, viewing, approving, and managing experiments. |
| **Experiment Engine** | Core state machine that drives experiment lifecycle: initialize → inject → monitor → abort/complete → analyze. |
| **GameDay Orchestrator** | Sequences multiple experiments into a scripted GameDay scenario with pauses, checkpoints, and manual gates. |
| **Target Selector** | Selects targets based on filters (labels, AZ, tier, percentage), applies exclusions (feature flags, critical services), enforces blast radius. |
| **Steady-State Validator** | Evaluates PromQL queries to determine if the system is in steady state. Compares pre-injection baseline with current values. |
| **Safety Controller** | Enforces blast radius limits, manages abort conditions, prevents concurrent conflicting experiments, and integrates with feature flags. |
| **Injection Drivers** | Plugin-based executors for each injection type. Each driver knows how to inject and rollback its specific fault type. |
| **Chaos Agent** | DaemonSet running on each node. Receives injection commands from the control plane and executes them locally. Provides local rollback capability. |
| **MySQL** | Stores experiment definitions, results, approval records, and configuration. |
| **Elasticsearch** | Indexes detailed injection logs, metric snapshots, and results for search and analysis. |
| **Feature Flag Service** | Controls which services/tiers/environments are eligible for chaos experiments. |
| **Notification Service** | Sends notifications (Slack, PagerDuty, email) for experiment start, abort, completion, and findings. |

### Data Flows

**Experiment Execution Flow:**
1. User defines experiment via API/UI → stored in MySQL.
2. Approval workflow (if production): team lead approves via UI/Slack.
3. Experiment Engine initializes: selects targets, validates pre-conditions.
4. Steady-State Validator records baseline metrics (5 minutes of pre-injection data).
5. Engine sends injection commands to Chaos Agents via gRPC.
6. Agents execute injections and report status.
7. Steady-State Validator continuously evaluates metrics during injection.
8. If deviation exceeds abort threshold → Safety Controller triggers abort.
9. After experiment duration, Engine sends rollback commands.
10. Post-experiment: Steady-State Validator records recovery metrics.
11. Results analysis: compare baseline → injection → recovery.
12. Results stored in MySQL + ES; notifications sent.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Experiment definition
CREATE TABLE experiment (
    experiment_id       VARCHAR(128) PRIMARY KEY,
    name                VARCHAR(256) NOT NULL,
    description         TEXT,
    owner               VARCHAR(128) NOT NULL,       -- team or user
    environment         ENUM('staging', 'production', 'dev') NOT NULL,
    tier_restriction    ENUM('all', 'tier2_and_below', 'tier3_and_below', 'staging_only') NOT NULL DEFAULT 'staging_only',
    status              ENUM('draft', 'pending_approval', 'approved', 'running',
                             'aborting', 'aborted', 'completed', 'failed') NOT NULL DEFAULT 'draft',
    hypothesis          TEXT NOT NULL,               -- "The system should maintain p99 < 200ms when 10% of pods are killed"
    injection_config    JSON NOT NULL,               -- injection type, parameters
    target_config       JSON NOT NULL,               -- target selection criteria
    steady_state_config JSON NOT NULL,               -- PromQL queries and thresholds
    abort_config        JSON NOT NULL,               -- abort conditions
    blast_radius_config JSON NOT NULL,               -- max percentage, max count, exclusions
    duration_sec        INT NOT NULL,
    scheduled_at        TIMESTAMP NULL,              -- for scheduled experiments
    started_at          TIMESTAMP NULL,
    completed_at        TIMESTAMP NULL,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_status (status),
    INDEX idx_owner (owner),
    INDEX idx_env_status (environment, status)
) ENGINE=InnoDB;

-- Experiment approval
CREATE TABLE experiment_approval (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    experiment_id       VARCHAR(128) NOT NULL,
    approver            VARCHAR(128) NOT NULL,
    decision            ENUM('approved', 'rejected') NOT NULL,
    reason              TEXT,
    decided_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (experiment_id) REFERENCES experiment(experiment_id),
    INDEX idx_experiment (experiment_id)
) ENGINE=InnoDB;

-- Injection record (per-target injection)
CREATE TABLE injection (
    injection_id        VARCHAR(128) PRIMARY KEY,
    experiment_id       VARCHAR(128) NOT NULL,
    injection_type      ENUM('pod_kill', 'node_reboot', 'network_latency', 'network_loss',
                             'cpu_stress', 'memory_stress', 'disk_fill', 'dns_failure',
                             'process_kill', 'az_failure_sim', 'kafka_broker_kill',
                             'mysql_replica_kill', 'ipmi_power_off') NOT NULL,
    target_id           VARCHAR(256) NOT NULL,
    target_type         ENUM('pod', 'node', 'service', 'bare_metal', 'cluster') NOT NULL,
    parameters_json     JSON NOT NULL,
    agent_id            VARCHAR(128),                -- which chaos agent executed this
    status              ENUM('pending', 'injecting', 'active', 'rolling_back',
                             'rolled_back', 'completed', 'failed') NOT NULL DEFAULT 'pending',
    injected_at         TIMESTAMP NULL,
    rolled_back_at      TIMESTAMP NULL,
    error_message       TEXT,
    FOREIGN KEY (experiment_id) REFERENCES experiment(experiment_id),
    INDEX idx_experiment (experiment_id),
    INDEX idx_target (target_type, target_id),
    INDEX idx_status (status)
) ENGINE=InnoDB;

-- Steady-state measurements (before, during, after)
CREATE TABLE steady_state_measurement (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    experiment_id       VARCHAR(128) NOT NULL,
    phase               ENUM('pre_injection', 'during_injection', 'post_injection') NOT NULL,
    metric_name         VARCHAR(256) NOT NULL,
    metric_query        TEXT NOT NULL,                -- PromQL
    value               DOUBLE NOT NULL,
    threshold           DOUBLE NOT NULL,
    within_threshold    BOOLEAN NOT NULL,
    measured_at         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (experiment_id) REFERENCES experiment(experiment_id),
    INDEX idx_experiment_phase (experiment_id, phase)
) ENGINE=InnoDB;

-- Experiment result
CREATE TABLE experiment_result (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    experiment_id       VARCHAR(128) NOT NULL UNIQUE,
    outcome             ENUM('passed', 'failed', 'aborted', 'inconclusive') NOT NULL,
    hypothesis_validated BOOLEAN NOT NULL,
    summary             TEXT NOT NULL,
    findings_json       JSON,
    recommendations     TEXT,
    steady_state_maintained BOOLEAN NOT NULL,
    max_deviation_pct   DOUBLE,                       -- max deviation from steady-state
    recovery_time_sec   INT,                           -- time to return to steady-state after rollback
    targets_affected    INT NOT NULL,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (experiment_id) REFERENCES experiment(experiment_id)
) ENGINE=InnoDB;

-- GameDay definition
CREATE TABLE gameday (
    gameday_id          VARCHAR(128) PRIMARY KEY,
    name                VARCHAR(256) NOT NULL,
    description         TEXT,
    owner               VARCHAR(128) NOT NULL,
    status              ENUM('planned', 'in_progress', 'completed', 'cancelled') NOT NULL DEFAULT 'planned',
    steps_json          JSON NOT NULL,                -- ordered list of experiment IDs + manual checkpoints
    scheduled_date      DATE NOT NULL,
    started_at          TIMESTAMP NULL,
    completed_at        TIMESTAMP NULL,
    post_mortem_url     VARCHAR(512),
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_status (status),
    INDEX idx_date (scheduled_date)
) ENGINE=InnoDB;
```

### Database Selection

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **MySQL 8.0** | ACID, familiar, JSON support for flexible configs | Overkill for this data volume but consistent with stack | **Selected for primary store** |
| **Elasticsearch** | Good for searching experiment logs, time-series metrics | Not ACID, eventual consistency | **Selected for logs and metric snapshots** |
| **SQLite** | Simplest possible, embedded | Doesn't support concurrent access well | Not selected |
| **PostgreSQL** | Excellent JSON support, array types | Team uses MySQL; unnecessary migration | Not selected |

**Justification:** The data volume is tiny (~7 GB/year). MySQL handles everything with room to spare. Elasticsearch provides searchability for injection logs and metric data. The data model is straightforward relational.

### Indexing Strategy

- `experiment(status)` — quickly find running experiments (important for concurrent experiment checks).
- `experiment(environment, status)` — find all running production experiments.
- `injection(experiment_id)` — list injections for an experiment.
- `injection(target_type, target_id)` — check if a target already has an active injection (prevent double-injection).
- `steady_state_measurement(experiment_id, phase)` — retrieve measurements by phase for results analysis.

---

## 5. API Design

### REST Endpoints

```
Base URL: https://chaos.infra.internal/api/v1

# Experiment CRUD
POST   /experiments
  Body: {
    "name": "Pod kill resilience - orders service",
    "description": "Validate that orders service maintains SLO when 10% of pods are killed",
    "environment": "production",
    "tier_restriction": "tier2_and_below",
    "hypothesis": "Orders service maintains p99 latency < 200ms and error rate < 0.5% when 10% of pods are terminated",
    "injection": {
      "type": "pod_kill",
      "parameters": {
        "signal": "SIGKILL",
        "interval_sec": 60,
        "mode": "one_at_a_time"
      }
    },
    "targets": {
      "type": "pod",
      "selector": {
        "namespace": "orders",
        "labels": {"app": "orders-service"},
        "percentage": 10
      },
      "exclusions": {
        "labels": {"chaos-exempt": "true"}
      }
    },
    "steady_state": {
      "metrics": [
        {
          "name": "p99_latency",
          "query": "histogram_quantile(0.99, rate(http_request_duration_seconds_bucket{service='orders'}[5m]))",
          "threshold": 0.200,
          "comparison": "less_than"
        },
        {
          "name": "error_rate",
          "query": "rate(http_requests_total{service='orders',status=~'5..'}[5m]) / rate(http_requests_total{service='orders'}[5m])",
          "threshold": 0.005,
          "comparison": "less_than"
        }
      ]
    },
    "abort_conditions": {
      "steady_state_deviation_pct": 50,
      "error_rate_absolute": 0.05,
      "manual_abort_enabled": true
    },
    "blast_radius": {
      "max_percentage": 10,
      "max_absolute_count": 5,
      "max_per_az": 2
    },
    "duration_sec": 1800
  }
  Response: 201 { "experiment_id": "exp-abc123", "status": "pending_approval" }

GET    /experiments?status=running&environment=production
  Response: 200 [...]

GET    /experiments/{experiment_id}
  Response: 200 { full experiment with injections, measurements, result }

# Experiment lifecycle
POST   /experiments/{experiment_id}/approve
  Body: { "approver": "sre-lead", "reason": "Reviewed blast radius and hypothesis" }
  Response: 200 { "status": "approved" }

POST   /experiments/{experiment_id}/start
  Response: 202 { "status": "running" }

POST   /experiments/{experiment_id}/abort
  Body: { "reason": "Observed unexpected behavior in downstream service" }
  Response: 200 { "status": "aborting" }

GET    /experiments/{experiment_id}/status
  Response: 200 {
    "status": "running",
    "elapsed_sec": 450,
    "remaining_sec": 1350,
    "injections_active": 3,
    "steady_state": {
      "p99_latency": {"current": 0.145, "threshold": 0.200, "within": true},
      "error_rate": {"current": 0.001, "threshold": 0.005, "within": true}
    }
  }

GET    /experiments/{experiment_id}/results
  Response: 200 {
    "outcome": "passed",
    "hypothesis_validated": true,
    "summary": "Orders service maintained p99 < 200ms and error rate < 0.5% throughout the experiment",
    "targets_affected": 3,
    "max_deviation": {"p99_latency": "+15%", "error_rate": "+0.05%"},
    "recovery_time_sec": 45,
    "recommendations": "System is resilient to 10% pod kill. Consider testing 25% next."
  }

# Injection types catalog
GET    /injection-types
  Response: 200 [
    {
      "type": "pod_kill",
      "target_types": ["pod"],
      "parameters_schema": { "signal": "enum(SIGKILL,SIGTERM)", "interval_sec": "int", ... },
      "rollback_method": "Kubernetes recreates pod via ReplicaSet"
    },
    {
      "type": "network_latency",
      "target_types": ["pod", "node"],
      "parameters_schema": { "latency_ms": "int", "jitter_ms": "int", "interface": "string" },
      "rollback_method": "Remove tc qdisc rules"
    },
    ...
  ]

# GameDay management
POST   /gamedays
  Body: {
    "name": "Q2 2026 Resilience GameDay",
    "description": "Test AZ failure + database failover + network partition",
    "scheduled_date": "2026-05-15",
    "steps": [
      {"type": "experiment", "experiment_id": "exp-network-az1", "wait_after_sec": 300},
      {"type": "checkpoint", "message": "Verify AZ-1 traffic shifted to AZ-2/3", "requires_confirmation": true},
      {"type": "experiment", "experiment_id": "exp-mysql-primary-kill"},
      {"type": "checkpoint", "message": "Verify MySQL failover completed", "requires_confirmation": true},
      {"type": "experiment", "experiment_id": "exp-kafka-broker-kill"},
      {"type": "cooldown", "duration_sec": 600, "message": "Observe system recovery"}
    ]
  }
  Response: 201

POST   /gamedays/{gameday_id}/start
POST   /gamedays/{gameday_id}/advance   -- move to next step after checkpoint
POST   /gamedays/{gameday_id}/abort
```

### CLI Design

```bash
# Experiment management
chaos experiment create --file=./pod-kill-orders.yaml
chaos experiment list --status=running --env=production
chaos experiment show exp-abc123
chaos experiment approve exp-abc123 --reason="reviewed"
chaos experiment start exp-abc123
chaos experiment abort exp-abc123 --reason="unexpected behavior"
chaos experiment status exp-abc123 --watch   # live status updates
chaos experiment results exp-abc123

# Quick injection (for ad-hoc testing in staging)
chaos inject pod-kill --namespace=orders --labels="app=orders-service" --percentage=10 --duration=5m
chaos inject network-latency --target=node-42 --latency=100ms --jitter=20ms --duration=10m
chaos inject cpu-stress --target=node-42 --cores=4 --load=80 --duration=5m
chaos inject disk-fill --target=node-42 --path=/var --fill-pct=90 --duration=15m

# GameDay
chaos gameday create --file=./q2-gameday.yaml
chaos gameday start gd-q2-2026
chaos gameday advance gd-q2-2026   # pass checkpoint
chaos gameday abort gd-q2-2026
chaos gameday status gd-q2-2026

# History and analysis
chaos history --last=90d --env=production --outcome=failed
chaos trends --metric=recovery_time --last=12m   # trend over 12 months
```

---

## 6. Core Component Deep Dives

### 6.1 Experiment Engine (State Machine)

**Why it's hard:** The experiment must be safe above all else. An experiment that causes an outage defeats the purpose. The engine must ensure: pre-conditions are met before injecting, steady-state is monitored continuously, abort triggers work reliably, and rollback is complete. Additionally, the engine must handle its own failures — if the engine crashes during an experiment, injections must be rolled back.

**Approaches Compared:**

| Approach | Safety | Flexibility | Complexity | Recovery |
|---|---|---|---|---|
| **Simple script** | Low — no monitoring loop | Low | Low | Poor — crash = orphaned injections |
| **State machine with checkpoints** | High — state persisted | Medium | Medium | Good — resume from checkpoint |
| **Workflow engine (Temporal/Argo)** | High — built-in retry/resume | High | High | Excellent — durable execution |
| **Custom state machine + dead-man's switch** | Very high | Medium | Medium-High | Very good — auto-rollback on failure |

**Selected Approach: Custom state machine with dead-man's switch and persistent state.**

**Justification:** A workflow engine like Temporal would work but adds operational complexity for a relatively simple workflow. Our custom state machine persists state to MySQL at each transition, and a dead-man's switch (separate watchdog process) monitors running experiments. If the engine crashes and doesn't heartbeat the watchdog within 30 seconds, the watchdog triggers rollback of all active injections.

**State Machine:**

```
                    ┌─────────┐
                    │  DRAFT  │
                    └────┬────┘
                         │ submit
                    ┌────▼────┐
                    │ PENDING │
                    │ APPROVAL│
                    └────┬────┘
                    approve │ reject → CANCELLED
                    ┌────▼────┐
                    │APPROVED │
                    └────┬────┘
                         │ start
                    ┌────▼────┐
                    │  PRE-   │──── pre-check fails ──► FAILED
                    │ INJECT  │
                    │ VALIDATE│
                    └────┬────┘
                         │ baseline captured
                    ┌────▼────┐
                    │INJECTING│──── injection fails ──► ROLLING_BACK
                    └────┬────┘
                         │ all injections active
                    ┌────▼─────────┐
                    │  MONITORING  │◄─── steady-state check loop
                    │  (steady-    │          │
                    │   state      │    deviation > threshold
                    │   checks)    │          │
                    └──────┬───────┘    ┌─────▼─────┐
                           │            │  ABORTING  │
                    duration expired    └─────┬─────┘
                           │                  │
                    ┌──────▼───────┐    ┌─────▼──────┐
                    │  ROLLING     │    │  ROLLING   │
                    │  BACK        │    │  BACK      │
                    └──────┬───────┘    └─────┬──────┘
                           │                  │
                    ┌──────▼───────┐    ┌─────▼──────┐
                    │  POST-       │    │  ABORTED   │
                    │  VALIDATE    │    └────────────┘
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
                    │  ANALYZING   │
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
                    │  COMPLETED   │
                    └──────────────┘
```

**Pre-Injection Validation:**
```python
def pre_inject_validate(experiment):
    # 1. Check feature flag — is chaos allowed for this service?
    if not feature_flags.is_enabled("chaos_allowed", experiment.target_service):
        return FAIL, "Chaos disabled for this service via feature flag"
    
    # 2. Check for conflicting experiments
    active = db.query("SELECT * FROM injection WHERE target_id IN (%s) AND status='active'",
                      experiment.target_ids)
    if active:
        return FAIL, f"Conflicting active injection on {active[0].target_id}"
    
    # 3. Verify steady state before injecting
    for metric in experiment.steady_state_config.metrics:
        value = prometheus.query(metric.query)
        if not metric.check(value):
            return FAIL, f"Steady state not met before injection: {metric.name}={value}"
    
    # 4. Verify blast radius
    total_targets = count_targets(experiment.target_config)
    selected = experiment.blast_radius_config.max_percentage / 100 * total_targets
    if selected > experiment.blast_radius_config.max_absolute_count:
        selected = experiment.blast_radius_config.max_absolute_count
    
    # 5. Check business hours (if configured)
    if experiment.environment == "production" and not is_business_hours():
        return FAIL, "Production experiments only during business hours"
    
    return PASS, "Pre-injection validation passed"
```

**Dead-Man's Switch:**
```python
class DeadMansSwitch:
    """Watchdog that rolls back experiments if the engine dies."""
    
    def __init__(self):
        self.heartbeat_interval_sec = 10
        self.timeout_sec = 30
    
    def monitor(self):
        while True:
            running_experiments = db.query(
                "SELECT * FROM experiment WHERE status IN ('running', 'injecting', 'monitoring')"
            )
            for exp in running_experiments:
                engine_heartbeat = redis.get(f"chaos:engine:heartbeat:{exp.experiment_id}")
                if engine_heartbeat is None or (now() - engine_heartbeat > self.timeout_sec):
                    log.error(f"Engine heartbeat missed for {exp.experiment_id}, initiating emergency rollback")
                    emergency_rollback(exp.experiment_id)
                    alert_oncall(f"Chaos engine crash detected, experiment {exp.experiment_id} auto-rolled-back")
            time.sleep(self.heartbeat_interval_sec)
```

**Failure Modes:**
- **Engine crashes during injection:** Dead-man's switch rolls back within 30 seconds. Agents have local rollback timers (if no heartbeat from engine in 60s, agent rolls back its injections autonomously).
- **Agent crashes during injection:** If the agent injected via kernel settings (tc, iptables), the injection persists until the agent restarts and rolls back. Node reboot also clears kernel-level injections (they're not persistent).
- **Prometheus unavailable:** Steady-state checks fail-safe: if we can't validate steady state, we abort the experiment. Never assume steady state is maintained without measurement.

**Interviewer Q&As:**

**Q1: How do you ensure rollback is complete and no injections "leak"?**
A: Three layers: (1) The engine tracks every injection and sends explicit rollback commands. (2) Each chaos agent has a local timer — if it doesn't hear from the engine within 60 seconds, it rolls back autonomously. (3) A periodic cleanup job (every 5 minutes) scans for stale injections (status = 'active' but experiment is completed/aborted) and force-rolls them back. We also verify rollback: after rollback, re-run the injection-specific check (e.g., verify tc qdisc is removed, verify pod count is back to normal).

**Q2: What if the rollback itself fails?**
A: Rollback failure is treated as a P1 incident. The system alerts the on-call team with full context: what was injected, on which target, and what the rollback attempt returned. For kernel-level injections (tc, iptables), the fallback is to reboot the node — this clears all ephemeral network rules. For pod kills, rollback is natural (Kubernetes recreates the pod).

**Q3: How do you handle experiments that need to inject gradually (ramp up)?**
A: The injection config supports a `ramp` mode: start with 1 target, wait 60 seconds, check steady state, add 2 more targets, wait, check, and so on up to the maximum. If steady state degrades at any ramp step, we stop ramping and can either hold at the current level or abort.

**Q4: Can multiple experiments run on the same targets simultaneously?**
A: No, by default. The pre-injection validation checks for conflicting active injections on the same targets. However, this can be overridden for advanced scenarios (e.g., testing behavior under combined CPU stress + network latency) with explicit opt-in and additional safety review.

**Q5: How do you prevent chaos experiments from affecting real customer traffic?**
A: Multiple layers: (1) Feature flags control which services are chaos-eligible. (2) Blast radius limits cap the impact. (3) Steady-state monitoring aborts if customer-facing metrics degrade. (4) For production tier-1 services, we use "shadow injection" — inject faults only on shadow/dark traffic, not real traffic. (5) Time-boxing: experiments have maximum durations.

**Q6: How does the engine handle timezone and scheduling across global teams?**
A: Experiments can be scheduled with a specific start time and timezone. The scheduler enforces "experiment windows" — configurable time ranges when experiments are allowed per environment. Production experiments default to business hours (9am-5pm local time). Urgent experiments outside windows require an override from the SRE lead.

---

### 6.2 Injection Drivers (Network Chaos Deep Dive)

**Why it's hard:** Network chaos (latency injection, packet loss, partition simulation) operates at the Linux kernel level using `tc` (traffic control) and `iptables`. Getting the rules right is critical — a wrong iptables rule can lock you out of the machine. Rollback must be precise (remove exactly the rules we added, not others). And the injection must target specific traffic flows (e.g., only traffic from service A to service B) without affecting unrelated traffic.

**Approaches Compared:**

| Approach | Precision | Rollback Safety | Bare-Metal Support | Complexity |
|---|---|---|---|---|
| **tc + netem (Linux Traffic Control)** | High — per-interface, per-flow | Medium — must save and restore qdiscs | Yes | Medium |
| **iptables + tc** | Very high — can match src/dst IP | Medium — must track added rules | Yes | High |
| **Cilium/eBPF** | Very high — per-pod, per-connection | High — eBPF programs are easily removed | K8s only | High |
| **Istio fault injection** | High — per-service, per-route | High — just update VirtualService | K8s + Istio only | Low |

**Selected Approach: tc + netem for bare-metal/node-level; Istio fault injection for pod-level in K8s; iptables as fallback.**

**Justification:** Different targets need different tools. For bare-metal nodes without a service mesh, tc + netem is the standard Linux approach. For Kubernetes pods with Istio, the VirtualService fault injection is cleaner and safer (no kernel-level changes). iptables is used for specific cases like DNS failure simulation.

**Implementation Detail — Network Latency Injection:**

```python
class NetworkLatencyDriver:
    """Injects network latency using tc + netem on bare-metal / node-level."""
    
    def inject(self, target: Target, params: dict):
        latency_ms = params['latency_ms']
        jitter_ms = params.get('jitter_ms', 0)
        interface = params.get('interface', 'eth0')
        correlation_pct = params.get('correlation_pct', 25)
        target_ip = params.get('target_ip')  # optional: only affect traffic to this IP
        
        if target_ip:
            # Use tc + filter to only affect traffic to specific IP
            commands = [
                f"tc qdisc add dev {interface} root handle 1: prio",
                f"tc qdisc add dev {interface} parent 1:3 handle 30: netem delay {latency_ms}ms {jitter_ms}ms {correlation_pct}%",
                f"tc filter add dev {interface} parent 1: protocol ip prio 3 u32 match ip dst {target_ip}/32 flowid 1:3"
            ]
        else:
            # Affect all traffic on the interface
            commands = [
                f"tc qdisc add dev {interface} root netem delay {latency_ms}ms {jitter_ms}ms {correlation_pct}%"
            ]
        
        # Save current state for rollback
        current_qdiscs = execute(f"tc qdisc show dev {interface}")
        self.save_rollback_state(target, current_qdiscs)
        
        for cmd in commands:
            result = execute_on_target(target, cmd)
            if result.exit_code != 0:
                self.rollback(target)
                raise InjectionError(f"Failed to inject: {result.stderr}")
    
    def rollback(self, target: Target):
        state = self.get_rollback_state(target)
        interface = state['interface']
        # Remove all qdiscs we added
        execute_on_target(target, f"tc qdisc del dev {interface} root")
        # Restore original qdiscs
        for qdisc_cmd in state['original_qdiscs']:
            execute_on_target(target, qdisc_cmd)
    
    def verify_rollback(self, target: Target) -> bool:
        """Verify that latency injection was fully removed."""
        current = execute_on_target(target, f"tc qdisc show dev {target.interface}")
        return "netem" not in current.stdout
```

**Implementation Detail — Istio Fault Injection (Pod-Level):**

```yaml
# Istio VirtualService for fault injection
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
metadata:
  name: chaos-orders-service
  namespace: orders
  labels:
    chaos-experiment: "exp-abc123"
spec:
  hosts:
  - orders-service
  http:
  - fault:
      delay:
        percentage:
          value: 10.0          # 10% of requests
        fixedDelay: 200ms
    route:
    - destination:
        host: orders-service
---
# Rollback: simply delete this VirtualService
# kubectl delete virtualservice chaos-orders-service -n orders
```

**Injection Type Catalog:**

| Injection Type | Tool | Rollback Method | Risk Level |
|---|---|---|---|
| Pod kill | `kubectl delete pod` | K8s recreates via ReplicaSet | Low |
| Node reboot | SSH: `reboot` or IPMI: `power cycle` | Node auto-recovers on boot | Medium |
| Network latency | `tc netem` / Istio VirtualService | Remove tc rules / delete VirtualService | Medium |
| Packet loss | `tc netem loss X%` | Remove tc rules | Medium |
| CPU stress | `stress-ng --cpu N --timeout Xs` | Process ends at timeout | Low |
| Memory stress | `stress-ng --vm N --vm-bytes Xm --timeout Xs` | Process ends at timeout | Medium (OOM risk) |
| Disk fill | `dd if=/dev/zero of=/tmp/chaos_fill bs=1M count=N` | `rm /tmp/chaos_fill` | Medium |
| DNS failure | `iptables -A OUTPUT -p udp --dport 53 -j DROP` | `iptables -D` (remove rule) | High |
| Process kill | `kill -9 <pid>` / `systemctl stop <svc>` | Systemd restarts / manual | Low-Medium |
| AZ failure sim | Network partition AZ + drain nodes | Remove partition + uncordon | High |
| Kafka broker kill | `kill -9 kafka_pid` on broker | Kafka auto-recovers, partitions rebalance | Medium |
| MySQL replica kill | `kill -9 mysqld` on replica | MySQL restarts, replication resumes | Medium |
| Bare-metal IPMI off | `ipmitool power off` | `ipmitool power on` | High |

**Failure Modes:**
- **tc rule conflicts with existing rules:** We always save the current qdisc state before modifying. Rollback restores the exact original state.
- **Istio VirtualService merge conflict:** We create a separate VirtualService with a unique name (prefixed with `chaos-`) rather than modifying the existing one. Deletion is clean.
- **stress-ng causes OOM kill of critical processes:** Mitigation: run stress-ng in a cgroup with a memory limit. Never stress to 100% — cap at 90%.

**Interviewer Q&As:**

**Q1: How do you inject network latency that only affects traffic between two specific services?**
A: In a Kubernetes environment with Istio, use VirtualService fault injection which is service-aware. Without Istio, use tc + u32 filters to match source/destination IP pairs. For pod-to-pod, we need to know the pod IPs, which we resolve from Kubernetes endpoints.

**Q2: How do you simulate an AZ failure without actually bringing down an AZ?**
A: We create a network partition: iptables rules that drop all traffic from AZ-1 nodes to AZ-2/3 nodes and vice versa. We also cordon all nodes in AZ-1 (prevent new scheduling). This simulates AZ-1 being unreachable without actually affecting the hardware. Rollback: remove iptables rules, uncordon nodes.

**Q3: What's the risk of injecting chaos into bare-metal hosts?**
A: Higher than K8s pods because recovery is slower. A bare-metal IPMI power-off takes 2-5 minutes to recover (reboot). A killed MySQL process on bare-metal might take 5-10 minutes to replay InnoDB redo logs. We require stricter approval for bare-metal chaos and lower blast radius limits (max 1 node per rack).

**Q4: How do you handle chaos injection in a service mesh vs non-mesh environment?**
A: In a mesh (Istio), we prefer application-level fault injection (VirtualService) because it's cleaner, more targeted, and easier to rollback. In a non-mesh environment, we fall back to OS-level tools (tc, iptables, kill). The injection driver is selected automatically based on the target's environment.

**Q5: How do you prevent a chaos experiment from triggering the self-healing system?**
A: We notify the self-healing system before starting an experiment: "For the next 30 minutes, expect failures on these targets — do not remediate." The self-healing system adds these targets to a temporary exclusion list. After the experiment, the exclusion is removed. If the experiment is aborted, the exclusion is removed immediately.

**Q6: What about data-plane chaos (corrupting packets, reordering)?**
A: tc netem supports packet reordering (`tc qdisc add dev eth0 root netem reorder 25%`) and packet corruption (`netem corrupt 5%`). These are useful for testing TCP retransmission and application-level checksumming. We use them sparingly and only in staging, because corrupt packets can cause unpredictable application behavior.

---

### 6.3 Steady-State Hypothesis Validation

**Why it's hard:** The steady-state hypothesis is the scientific heart of chaos engineering. You need to: (1) Define measurable system properties that represent "normal." (2) Measure them accurately before, during, and after the experiment. (3) Determine if deviations are caused by the experiment or by background noise. (4) Make an abort decision in real-time when things go wrong.

**Approaches Compared:**

| Approach | Accuracy | Real-Time Capable | Complexity |
|---|---|---|---|
| **Simple threshold** | Low — doesn't account for baseline variance | Yes | Low |
| **Percentage deviation from baseline** | Medium — adapts to baseline level | Yes | Medium |
| **Statistical hypothesis testing (t-test)** | High — accounts for variance | No (needs samples) | High |
| **Hybrid: threshold + deviation + abort threshold** | High for real-time | Yes | Medium |

**Selected Approach: Hybrid with three evaluation levels.**

**Implementation:**

```python
class SteadyStateValidator:
    def __init__(self, experiment):
        self.experiment = experiment
        self.baseline = {}  # metric_name → StatisticalBaseline
    
    def capture_baseline(self, duration_sec=300):
        """Capture 5 minutes of pre-injection metrics."""
        for metric in self.experiment.steady_state_config.metrics:
            values = prometheus.range_query(
                metric.query,
                start=now() - timedelta(seconds=duration_sec),
                end=now(),
                step="15s"
            )
            self.baseline[metric.name] = StatisticalBaseline(
                mean=np.mean(values),
                std=np.std(values),
                p50=np.percentile(values, 50),
                p95=np.percentile(values, 95),
                p99=np.percentile(values, 99)
            )
    
    def evaluate(self, metric_name: str) -> EvaluationResult:
        """Real-time evaluation during experiment."""
        metric = self.get_metric_config(metric_name)
        current_value = prometheus.instant_query(metric.query)
        baseline = self.baseline[metric_name]
        
        # Level 1: Absolute threshold (configured by user)
        if not metric.check(current_value, metric.threshold):
            return EvaluationResult.THRESHOLD_VIOLATED
        
        # Level 2: Deviation from baseline (>50% deviation is concerning)
        if baseline.mean > 0:
            deviation_pct = abs(current_value - baseline.mean) / baseline.mean * 100
            if deviation_pct > 50:
                return EvaluationResult.SIGNIFICANT_DEVIATION
        
        # Level 3: Abort threshold (>100% deviation or absolute abort threshold)
        abort_config = self.experiment.abort_config
        if deviation_pct > abort_config.steady_state_deviation_pct:
            return EvaluationResult.ABORT_THRESHOLD
        
        return EvaluationResult.WITHIN_STEADY_STATE
    
    def continuous_monitor(self, check_interval_sec=10):
        """Monitor loop during experiment."""
        while self.experiment.status == "monitoring":
            for metric in self.experiment.steady_state_config.metrics:
                result = self.evaluate(metric.name)
                self.record_measurement(metric.name, result)
                
                if result == EvaluationResult.ABORT_THRESHOLD:
                    log.error(f"Abort threshold reached for {metric.name}")
                    return ABORT
                elif result == EvaluationResult.THRESHOLD_VIOLATED:
                    log.warn(f"Threshold violated for {metric.name}")
                    # Don't abort immediately — wait for 3 consecutive violations
                    self.violation_count[metric.name] += 1
                    if self.violation_count[metric.name] >= 3:
                        return ABORT
                else:
                    self.violation_count[metric.name] = 0
            
            time.sleep(check_interval_sec)
        
        return CONTINUE
```

**Failure Modes:**
- **Prometheus is slow/unavailable:** Abort the experiment. We can't maintain safety without observability.
- **Metrics are naturally noisy:** Baseline capture handles this — if the metric has high variance normally, the deviation threshold adapts. We also use 3-consecutive-violation rule to avoid aborting on single spikes.
- **Wrong metric selected:** This is a human error in experiment design. We require experiment review (peer review of the hypothesis and metrics) before production execution.

**Interviewer Q&As:**

**Q1: How do you distinguish between degradation caused by the experiment and coincidental degradation?**
A: Temporal correlation: if the metric degraded exactly when we injected, it's likely caused by our injection. If it degraded before we injected, it's coincidental. We also use control groups when possible: if we're killing pods in AZ-1, we compare metrics from AZ-1 with AZ-2/AZ-3 (unaffected control).

**Q2: What if the steady-state hypothesis is wrong (too tight or too loose)?**
A: If too tight: the experiment aborts frequently for minor, acceptable deviations. Fix: widen thresholds based on baseline variance. If too loose: the experiment misses real degradation. Fix: analyze post-experiment to see if customer-impacting events occurred that the hypothesis didn't catch. We track "escaped degradations" — cases where the hypothesis said "pass" but user-facing metrics degraded.

**Q3: How many metrics should a steady-state hypothesis include?**
A: Typically 3-5 metrics: (1) Latency (p99 or p95). (2) Error rate (5xx). (3) Throughput (requests/sec — should stay same). (4) Saturation (CPU, memory). (5) Business metric (orders/sec, signups/sec). More than 5 makes the experiment hard to interpret. Fewer than 2 risks missing degradation.

**Q4: Can the steady-state hypothesis be auto-generated?**
A: Partially. We can auto-generate reasonable defaults based on the service's SLOs: if the SLO is p99 < 500ms, the hypothesis threshold is p99 < 500ms. But meaningful hypotheses require human thought about what "degraded but acceptable" looks like.

**Q5: How do you handle experiments where the expected behavior IS degradation?**
A: The hypothesis should reflect this. Example: "When 50% of pods are killed, p99 latency should increase by at most 2x and error rate should stay below 1%." The hypothesis doesn't say "no impact" — it says "acceptable impact." The abort thresholds are set higher: abort only if latency increases 5x or error rate exceeds 5%.

**Q6: What about experiments where recovery time is the key metric?**
A: After the injection is rolled back, we measure "time to return to steady state" — the duration until all metrics return within their baseline range. This is the recovery time and is recorded in the experiment result. Some experiments are specifically designed to measure recovery (inject → remove → measure recovery).

---

## 7. Scheduling & Resource Management

### Experiment Scheduling

**Scheduling constraints:**
- No more than 3 experiments running simultaneously in the same cluster.
- No experiments during planned maintenance windows.
- Production experiments only during business hours (configurable per team/region).
- Critical infrastructure experiments (Kafka, MySQL, networking) have exclusive locks — only 1 at a time globally.

**Resource allocation for chaos agents:**
- Chaos agent DaemonSet: 50m CPU, 64 MB memory per node (minimal footprint).
- Stress-ng injections: use cgroups to limit resource consumption (never exceed 90% of node capacity).
- Disk fill: never fill beyond 95% (leave room for OS operations and recovery).

### Blast Radius Management

```python
class BlastRadiusController:
    def select_targets(self, experiment) -> List[Target]:
        # Get all candidates matching selector
        all_targets = self.discover_targets(experiment.target_config)
        
        # Apply percentage
        max_count = min(
            int(len(all_targets) * experiment.blast_radius_config.max_percentage / 100),
            experiment.blast_radius_config.max_absolute_count
        )
        
        # Apply per-AZ limit
        selected = []
        per_az_count = {}
        random.shuffle(all_targets)  # random selection
        
        for target in all_targets:
            if len(selected) >= max_count:
                break
            az = target.availability_zone
            per_az_count.setdefault(az, 0)
            if per_az_count[az] >= experiment.blast_radius_config.max_per_az:
                continue
            
            # Check exclusions
            if self.is_excluded(target, experiment.target_config.exclusions):
                continue
            
            # Check for conflicting active injections
            if self.has_active_injection(target):
                continue
            
            selected.append(target)
            per_az_count[az] += 1
        
        return selected
```

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Strategy | Scale Unit |
|---|---|---|
| Experiment API | Stateless; scale behind LB | Add pods as user count grows |
| Experiment Engine | 1 active + 2 standby (leader-elected) | Not a throughput bottleneck |
| Chaos Agents | DaemonSet — 1 per node | Scales with fleet |
| Steady-State Validator | 1 per running experiment | Max 50 concurrent (bounded) |
| GameDay Orchestrator | Singleton (infrequent use) | Not a scaling concern |

### Database Scaling

- MySQL: single instance with read replica. Data volume is ~7 GB/year — no scaling challenges.
- Elasticsearch: single-node cluster for experiment logs. Growth is negligible.

### Caching

| Layer | Technology | What's Cached | TTL |
|---|---|---|---|
| Target discovery | In-process cache | K8s pod/node list | 30s |
| Feature flag state | In-process cache | Chaos eligibility per service | 60s |
| Prometheus query results | Not cached | Metrics must be real-time | N/A |

### Interviewer Q&As

**Q1: What if you need to run chaos experiments against 10,000 targets simultaneously?**
A: The chaos agents are distributed (DaemonSet), so injections are parallel. The bottleneck is the engine orchestrating commands to 10K agents. Solution: batch commands — send injection commands via Kafka topic rather than individual gRPC calls. Agents subscribe to the topic and execute.

**Q2: How do you handle chaos agent updates across 100K nodes?**
A: DaemonSet rolling update with maxUnavailable=10%. Each agent update takes ~30 seconds. Full fleet update takes ~5 minutes (parallel across 10% of nodes at a time).

**Q3: What happens when a chaos agent version has a bug?**
A: Canary deployment: update 1% of agents, run a smoke test experiment (pod-kill on staging), verify rollback works, then continue rolling out. If the bug causes injection leaks (injections that don't roll back), the central cleanup job catches them.

**Q4: How do you prevent chaos experiments from consuming too many Prometheus resources?**
A: Each experiment is limited to 10 metric queries, checked every 10 seconds. With 50 concurrent experiments, that's 50 × 10 / 10 = 50 queries/sec to Prometheus — well within capacity. We also use recording rules for expensive queries.

**Q5: How do you archive old experiment data?**
A: Experiments older than 2 years are archived from MySQL to S3 (as JSON exports). Summary statistics are retained in MySQL indefinitely for trend analysis.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery |
|---|---|---|---|
| Experiment engine crash | Active experiments lose monitoring | Dead-man's switch (30s) | Watchdog triggers rollback of all active injections |
| Chaos agent crash on a node | Active injection on that node persists | Engine heartbeat timeout | Agent restart (systemd) rolls back on init; node reboot as fallback |
| Prometheus unavailable | Can't validate steady state | Prometheus health check | Abort all running experiments (fail-safe) |
| MySQL unavailable | Can't create experiments or record results | MySQL health check | Experiments continue based on engine cache; results buffered |
| Network partition (engine ↔ agents) | Can't send commands or rollback | Heartbeat timeout | Agents auto-rollback after 60s without engine heartbeat |
| Feature flag service down | Can't check chaos eligibility | Feature flag service health check | Fail-closed: deny all new experiments until flag service recovers |
| Accidental experiment on wrong target | Unintended service impact | Steady-state validation | Abort + rollback; post-incident review of experiment definition |

### Automated Recovery

- **Engine:** 3 replicas with leader election. If active crashes, standby takes over within 15 seconds.
- **Agents:** Systemd auto-restart. On restart, agent checks for stale injections and rolls them back.
- **Orphaned injections:** Cleanup job runs every 5 minutes, scanning for injections where experiment is completed/aborted but injection status is still 'active'.

### Retry Strategy

| Operation | Retry Strategy |
|---|---|
| Injection command to agent | 3 retries, 2s delay |
| Rollback command to agent | 5 retries, 1s delay (rollback must succeed) |
| Prometheus query | 3 retries, 5s delay |
| MySQL write | 3 retries, 1s delay |
| Feature flag check | 1 retry, fail-closed |

### Circuit Breaker

- **Global circuit breaker:** If 3 experiments in the last hour were aborted due to unexpected system degradation, halt all new experiments and alert the platform team. Something may be wrong with the environment, not the targets.

### Consensus & Coordination

- **Leader election:** Kubernetes Lease for experiment engine. Only the leader starts new experiments.
- **Distributed coordination:** None needed — the engine is a singleton that orchestrates all experiments sequentially or in bounded parallelism.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `chaos_experiments_active` | Gauge | > 50 (capacity limit) |
| `chaos_experiments_aborted_total` | Counter | > 3 in 1 hour |
| `chaos_injection_active` | Gauge per experiment | > expected blast radius |
| `chaos_rollback_failures` | Counter | > 0 (critical) |
| `chaos_steady_state_violation_rate` | Gauge per experiment | > 0 sustained |
| `chaos_agent_heartbeat_missing` | Gauge | > 10 agents |
| `chaos_engine_heartbeat_age_sec` | Gauge | > 30 (dead man's switch) |
| `chaos_experiment_duration_sec` | Histogram | > 2x configured duration |
| `chaos_orphaned_injections` | Gauge | > 0 |

### Distributed Tracing

- Each experiment generates a trace: API call → Engine → Target Selection → Injection → Monitoring → Rollback → Analysis.
- Injection commands to agents include the experiment's trace ID for correlation.

### Logging

```json
{
  "timestamp": "2026-04-09T14:30:15Z",
  "level": "INFO",
  "service": "chaos-engine",
  "experiment_id": "exp-abc123",
  "event": "injection_started",
  "injection_type": "pod_kill",
  "target": "orders-service-pod-xyz",
  "agent": "chaos-agent-node-42",
  "trace_id": "chaos-trace-789"
}
```

### Alerting

| Alert | Severity | Action |
|---|---|---|
| Rollback failure | P1 (page) | On-call SRE investigates stale injection |
| Multiple experiment aborts | P2 (warn) | Platform team reviews environment health |
| Orphaned injection detected | P2 (warn) | Cleanup job should resolve; page if persistent |
| Chaos agent fleet health < 95% | P3 (ticket) | Investigate agent deployment issues |
| Experiment exceeded duration | P3 (ticket) | Engine may be hung; check dead-man's switch |

---

## 11. Security

### Auth & AuthZ

| Role | Permissions |
|---|---|
| `chaos-viewer` | View experiments, results, and dashboards |
| `chaos-engineer` | Create experiments in staging, request approval for production |
| `chaos-approver` | Approve production experiments |
| `chaos-admin` | Configure blast radius limits, manage injection drivers, override safety controls |

**Principle of least privilege for chaos agents:**
- Chaos agents run with specific Linux capabilities: `NET_ADMIN` (for tc/iptables), `SYS_PTRACE` (for process injection), `SYS_RESOURCE` (for stress).
- Agents do NOT have root SSH access — they have a dedicated user with sudoers limited to specific commands.
- Kubernetes RBAC: chaos service account can delete pods but not deployments, secrets, or configmaps.

### Audit Logging

Every chaos action is logged:
```json
{
  "timestamp": "2026-04-09T14:30:15Z",
  "actor": "chaos-engineer@company.com",
  "action": "experiment_started",
  "experiment_id": "exp-abc123",
  "environment": "production",
  "targets": ["orders-service-pod-1", "orders-service-pod-2"],
  "injection_type": "pod_kill",
  "blast_radius": "10% of orders-service pods (2 of 20)",
  "approval": {"approver": "sre-lead@company.com", "approved_at": "2026-04-09T14:25:00Z"},
  "hypothesis": "Orders service maintains p99 < 200ms"
}
```

**Audit logs are immutable** and retained for 2 years. They answer: "Who ran what chaos experiment, when, against what, and what happened?"

---

## 12. Incremental Rollout Strategy

**Phase 1 (Week 1-4): Staging Only**
- Deploy chaos platform with pod-kill and CPU-stress drivers only.
- Run experiments against staging services.
- Validate: injection works, rollback works, steady-state monitoring works, abort works.
- Build team confidence and documentation.

**Phase 2 (Week 5-8): Full Driver Suite in Staging**
- Add network latency, packet loss, disk fill, memory stress, DNS failure drivers.
- Run increasingly complex experiments.
- Validate blast radius control and concurrent experiment handling.

**Phase 3 (Week 9-12): Production Tier 3**
- Enable chaos for production tier-3 services (batch, analytics, internal tools).
- Require team lead approval.
- Run 2-3 experiments per week. Analyze results.
- Fix resilience gaps discovered.

**Phase 4 (Week 13-20): Production Tier 2**
- Enable for tier-2 services.
- Introduce GameDay orchestration.
- First GameDay: simulate single-node failure affecting a tier-2 service.

**Phase 5 (Week 21-30): Production Tier 1 + Infrastructure**
- Enable for tier-1 services with strict blast radius (max 2% of pods).
- Infrastructure chaos: Kafka broker kill, MySQL replica kill, network partition.
- Quarterly GameDays simulating AZ-level failures.

### Rollout Q&As

**Q1: What's the biggest risk during the rollout?**
A: An injection that doesn't roll back cleanly, leaving a production service in a degraded state (e.g., iptables rule blocking DNS permanently). Mitigation: every driver has automated rollback verification. The dead-man's switch ensures engine crashes don't leave orphaned injections.

**Q2: How do you convince teams to allow chaos experiments on their services?**
A: Start with value: show them the results from staging chaos that revealed real resilience gaps. Offer "opt-in with veto": teams can exclude specific services via feature flags. Share post-experiment reports that highlight both strengths ("your service handled 30% pod loss gracefully") and areas for improvement.

**Q3: How do you handle a GameDay that discovers a critical vulnerability?**
A: If the experiment reveals that a service can't handle the failure scenario (e.g., losing 10% of pods causes cascading failure), we: (1) Document the finding. (2) Create a ticket for the owning team with severity based on likelihood and impact. (3) Re-test after the fix. (4) Track the "chaos debt" — known vulnerabilities discovered but not yet fixed.

**Q4: How often should you run chaos experiments?**
A: Continuously for automated experiments (pod-kill, CPU stress on tier-3). Weekly for manual experiments. Quarterly for GameDays. The goal is to make chaos routine, not exceptional. Netflix runs Chaos Monkey continuously during business hours.

**Q5: How do you prevent "chaos fatigue" where teams ignore findings?**
A: Gamification: track resilience scores per team (% of chaos experiments passed). Publish a leaderboard. Tie resilience scores to service tier requirements: "To be classified as tier-1, your service must pass these 5 chaos scenarios." SRE team reviews findings monthly and escalates unaddressed vulnerabilities.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Choice | Rationale | Risk | Mitigation |
|---|---|---|---|---|---|
| Experiment engine | Temporal vs custom state machine | Custom state machine + dead-man's switch | Simpler, fewer dependencies | Must handle crash recovery ourselves | Dead-man's switch + persistent state |
| Network chaos tool | tc only vs eBPF vs Istio | tc (bare-metal) + Istio (K8s) | Best tool for each environment | Two implementations to maintain | Shared interface, per-driver testing |
| Steady-state validation | Simple threshold vs statistical testing | Hybrid (threshold + deviation + abort) | Real-time capable with reasonable accuracy | May miss subtle degradation | Post-experiment analysis catches missed degradation |
| Blast radius | Static limits vs dynamic | Static with per-AZ limits | Predictable, easy to reason about | May be too conservative | Tunable per experiment |
| Approval workflow | No approval vs team lead vs multi-approval | Team lead approval for production | Balances safety with agility | Approval bottleneck | Auto-approve after 30-day track record |
| Agent communication | SSH vs gRPC vs Kafka | gRPC for commands, Kafka for bulk operations | Low latency for single-target, scalable for bulk | gRPC requires direct connectivity | Fallback to SSH for bare-metal |
| Injection persistence | Ephemeral (kernel-level) vs persistent | Ephemeral (tc, iptables — cleared on reboot) | Safer — node reboot is guaranteed rollback | Can't test persistent failures | Persistent injection mode available with extra approval |

---

## 14. Agentic AI Integration

### AI-Powered Experiment Generation

**Problem:** Most teams run the same basic chaos experiments (pod kill, CPU stress). They miss important failure modes specific to their architecture.

**Solution:** An AI agent analyzes the service's architecture and generates tailored chaos experiments.

```python
class AIExperimentGenerator:
    def generate_experiments(self, service: Service) -> List[ExperimentSuggestion]:
        # Gather context
        context = {
            "service_name": service.name,
            "dependencies": service.get_dependencies(),  # from service mesh telemetry
            "deployment_config": service.get_deployment(),  # replicas, resources, probes
            "recent_incidents": service.get_incidents(last_90d=True),
            "current_chaos_coverage": service.get_experiment_history(),
            "SLOs": service.get_slos()
        }
        
        prompt = f"""
        You are a chaos engineering expert. Given this service's architecture,
        suggest 5 chaos experiments that would be most valuable. Prioritize
        experiments that test failure modes not yet covered.
        
        Service: {context['service_name']}
        Dependencies: {context['dependencies']}
        Deployment: {context['deployment_config']}
        Recent incidents: {context['recent_incidents']}
        Previous chaos experiments: {context['current_chaos_coverage']}
        SLOs: {context['SLOs']}
        
        For each experiment, provide:
        1. Injection type and parameters
        2. Hypothesis (what should happen)
        3. Steady-state metrics to monitor
        4. Expected blast radius
        5. Why this experiment is important for this service
        """
        
        return self.llm.invoke(prompt, temperature=0.3)
```

**Example AI-generated experiment:**
```
Service: orders-service
Dependency: payment-gateway (external, p99=800ms, timeout=2s)

AI Suggestion: "Test payment gateway timeout behavior"
- Injection: Network latency to payment-gateway endpoint (3s delay → exceeds 2s timeout)
- Hypothesis: Orders service returns graceful error to user within 3s, does not retry indefinitely
- Steady-state: Error rate stays < 5%, no cascading timeouts, no thread pool exhaustion
- Blast radius: 100% of payment-gateway traffic (but this is a dependency timeout test)
- Why: Recent incident showed thread pool exhaustion when payment gateway was slow. 
  This tests whether the fix (circuit breaker + timeout) works.
```

### AI-Powered Results Analysis

After each experiment, an LLM analyzes the results and generates insights.

```python
class AIResultsAnalyzer:
    def analyze(self, experiment: Experiment, measurements: List[Measurement]) -> Analysis:
        prompt = f"""
        Analyze this chaos engineering experiment result:
        
        Experiment: {experiment.name}
        Hypothesis: {experiment.hypothesis}
        Injection: {experiment.injection_config}
        Duration: {experiment.duration_sec}s
        
        Pre-injection baseline:
        {format_measurements(measurements, phase='pre_injection')}
        
        During injection:
        {format_measurements(measurements, phase='during_injection')}
        
        Post-injection (recovery):
        {format_measurements(measurements, phase='post_injection')}
        
        Provide:
        1. Was the hypothesis validated? (yes/no with evidence)
        2. Key observations (what degraded, what held steady)
        3. Recovery analysis (how quickly did the system return to normal)
        4. Risk assessment (what would happen if the blast radius were larger)
        5. Recommendations for improving resilience
        6. Suggested follow-up experiments
        """
        return self.llm.invoke(prompt, temperature=0.2)
```

### AI-Driven Adaptive Chaos

**Concept:** Instead of running pre-defined experiments, an AI agent continuously adjusts chaos intensity based on system behavior.

```
Adaptive Chaos Loop:
1. Start with minimal injection (1% pod kill)
2. Observe: is steady state maintained?
3. If yes: increase intensity (5%, 10%, 20%, ...)
4. If no: record the breaking point, roll back
5. Report: "Service X can tolerate up to 15% pod loss before SLOs degrade"
```

**Guard Rails:**
- Maximum intensity is always bounded by blast radius limits.
- Each intensity step requires 2 minutes of steady-state observation before escalating.
- Abort threshold is still enforced independently.
- Adaptive chaos is only available in staging initially.

---

## 15. Complete Interviewer Q&A Bank

**Q1: What is chaos engineering and how does it differ from testing?**
A: Chaos engineering is the discipline of experimenting on a system in order to build confidence in its ability to withstand turbulent conditions in production. Unlike testing (which verifies known behaviors), chaos engineering discovers unknown failure modes. Tests are "does it work?" Chaos is "what happens when things go wrong?"

**Q2: Explain the Netflix Chaos Monkey and its evolution.**
A: Chaos Monkey (2011) randomly kills VM instances during business hours to ensure services are resilient to instance failure. It evolved into: Chaos Kong (simulates entire region failure), Latency Monkey (injects network delays), Conformity Monkey (shuts down non-conforming instances), and ChAP (Chaos Automation Platform — hypothesis-driven, automated, integrated with CI/CD). The key evolution was from random destruction to structured experimentation with hypotheses and metrics.

**Q3: How do you define a "steady-state hypothesis"?**
A: A steady-state hypothesis is a measurable property of the system that defines "normal behavior." Example: "The system processes 10,000 requests/second with p99 latency < 200ms and error rate < 0.1%." You validate this before the experiment (baseline), monitor it during injection (does it hold?), and verify it after (does the system recover?). A good hypothesis is specific, measurable, and tied to SLOs.

**Q4: What's the difference between chaos engineering in staging vs production?**
A: Staging chaos validates that the system CAN handle failures. Production chaos validates that it DOES handle failures under real conditions (real traffic patterns, real data volumes, real dependency behavior). Many failures only manifest under production conditions (e.g., cold cache, slow downstream services, real user traffic patterns). The goal is to do chaos in production — staging is the training ground.

**Q5: How do you control blast radius in a chaos experiment?**
A: Multiple layers: (1) Percentage cap (never affect more than X% of targets). (2) Absolute count cap (never affect more than N targets). (3) Per-AZ cap (don't concentrate failures in one AZ). (4) Exclusion lists (protect critical services via feature flags). (5) Time boxing (experiments have maximum durations). (6) Automated abort (if steady state degrades beyond threshold).

**Q6: What happens if a chaos experiment causes a real outage?**
A: First: abort the experiment (automated or manual). Second: roll back all injections. Third: treat it as a real incident — follow the incident response process. Fourth: post-incident review. The review asks: was the blast radius too large? Were the abort conditions too lenient? Was the steady-state hypothesis wrong? The finding is actually valuable — we discovered a resilience gap. But we failed to discover it safely.

**Q7: How do you simulate an AZ failure without actually bringing down an AZ?**
A: Network partition simulation: iptables rules blocking all traffic between the target AZ's nodes and the rest of the cluster. Node cordoning: prevent new pod scheduling in the AZ. DNS manipulation: return errors for service discovery queries within the AZ. This creates the effect of an AZ failure (workloads in that AZ are unreachable) without actually powering off machines.

**Q8: How would you integrate chaos engineering with CI/CD?**
A: Run chaos experiments as part of the deployment pipeline. After deploying to a canary environment, automatically run a standard chaos suite (pod kill, network latency, dependency timeout). If any experiment fails (hypothesis not validated), block the deployment. This ensures every release is tested for resilience before reaching production.

**Q9: What's a "GameDay" and how do you plan one?**
A: A GameDay is a planned chaos exercise with the full team participating. Planning: (1) Define the scenario (e.g., "AZ-1 failure during peak traffic"). (2) Pre-define experiments and their order. (3) Identify participants (SREs, developers, management). (4) Set checkpoints (pause between experiments to assess). (5) Define abort criteria. (6) Run the GameDay. (7) Post-mortem. GameDays build institutional knowledge of failure modes and response procedures.

**Q10: How do you handle chaos experiments for stateful services (databases, message queues)?**
A: Very carefully. For MySQL: kill a replica (safe), kill the primary (dangerous — test failover). For Kafka: kill a broker (tests partition leader reelection), fill broker disk (tests log retention). For Elasticsearch: kill a data node (tests shard reallocation). Key differences from stateless: (1) Longer recovery times. (2) Potential data loss. (3) Require pre-experiment verification of replication health. (4) Lower blast radius limits (1 node at a time).

**Q11: How do you measure the ROI of chaos engineering?**
A: (1) Resilience gap closure: number of vulnerabilities found and fixed. (2) Incident reduction: fewer production incidents caused by failure modes we tested. (3) MTTR improvement: faster recovery because teams practiced response. (4) Confidence metric: team confidence in deploying changes (survey). (5) SLO impact: fewer SLO violations.

**Q12: What's the relationship between chaos engineering and the self-healing system?**
A: Chaos engineering validates the self-healing system. We inject failures that the self-healing system should detect and remediate. If self-healing works, the chaos experiment should be transparent: inject failure → self-healing detects and remediates → steady state maintained → experiment passes. If it doesn't pass, we've found a gap in self-healing.

**Q13: How do you handle a team that refuses to allow chaos on their service?**
A: Education first: explain the value, show results from other teams. Start small: offer to run the most basic experiment (kill 1 pod) in staging. Show the report. Gradually build trust. If a team still refuses, escalate to management with data: "This service has never been chaos-tested and handles X% of revenue." Ultimately, organizational mandate may be needed for tier-1 services.

**Q14: What's the most interesting failure you've found with chaos engineering?**
A: A common class: retry amplification. Service A calls Service B with 3 retries. Service B calls Service C with 3 retries. Inject 1 second of latency into Service C. Expected: 1 second of latency for Service A. Actual: Service C times out, Service B retries 3x (now 4 requests to C), Service A retries 3x (now 12 requests to C), causing a 12x amplification that overwhelms Service C. Discovered via network latency injection. Fix: exponential backoff with jitter and retry budgets.

**Q15: How do you run chaos experiments in a multi-tenant environment?**
A: Tenant isolation is critical. Never affect one tenant's resources as part of another tenant's experiment. Inject chaos only into the experimenting tenant's resources (pods, connections). Use network policies to ensure injected latency/loss only affects the target tenant's traffic. Verify via tenant-specific metrics that uninvolved tenants are unaffected.

**Q16: How would you design a "chaos as a service" platform for multiple teams?**
A: Self-service model: (1) Teams define experiments via YAML/API. (2) Approval workflow per team (team leads approve their own experiments). (3) Shared injection drivers but per-team blast radius limits. (4) Per-team dashboards and results. (5) Central safety controller prevents team A's experiment from affecting team B. (6) Global rate limiting (max N experiments across all teams simultaneously). This is essentially what Netflix's ChAP provides internally.

---

## 16. References

1. Rosenthal, C., Jones, N. "Chaos Engineering: System Resiliency in Practice" — O'Reilly, 2020
2. Netflix Chaos Monkey: https://github.com/Netflix/chaosmonkey
3. Netflix ChAP (Chaos Automation Platform) — Netflix Tech Blog
4. Principles of Chaos Engineering: https://principlesofchaos.org/
5. LitmusChaos (CNCF project): https://litmuschaos.io/
6. Chaos Mesh (CNCF project): https://chaos-mesh.org/
7. AWS Fault Injection Simulator: https://aws.amazon.com/fis/
8. Gremlin (Chaos Engineering Platform): https://www.gremlin.com/
9. tc-netem (Linux network emulation): https://man7.org/linux/man-pages/man8/tc-netem.8.html
10. Istio Fault Injection: https://istio.io/latest/docs/tasks/traffic-management/fault-injection/
11. Basiri, A., et al. "Automating Chaos Experiments in Production" — ICSE-SEIP, 2019 (Netflix)
