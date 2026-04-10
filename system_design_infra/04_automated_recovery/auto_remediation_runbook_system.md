# System Design: Auto-Remediation Runbook System

> **Relevance to role:** Runbooks are the bridge between human operational knowledge and automated remediation. A cloud infrastructure platform engineer must design systems that encode diagnostic steps and remediation actions as executable code — not wiki pages. This system turns tribal knowledge into repeatable, auditable, version-controlled automation. It directly supports the self-healing and health check systems by providing the "what to do when something is wrong" logic, with preconditions, post-conditions, blast radius limits, human approval gates, dry-run mode, and rollback support.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Runbook as Code:** Define runbooks in YAML/JSON with structured steps: diagnostic checks, remediation actions, decision branches, and verification steps.
2. **Decision Tree Execution Engine:** Execute runbooks as decision trees — each step's outcome determines the next step.
3. **Preconditions and Postconditions:** Each step has pre-conditions (must be true before executing) and post-conditions (must be true after executing for the step to be considered successful).
4. **Dry-Run Mode:** Execute a runbook without actually performing any actions — log what would happen, verify preconditions, simulate outcomes.
5. **Blast Radius Limits:** Per-runbook limits on how many entities can be affected simultaneously (e.g., "max 5 nodes" or "max 2% of cluster").
6. **Human Approval Gates:** Define steps that require human approval before proceeding (for high-impact actions like PXE reimaging or database failover).
7. **Rollback Support:** Each remediation step defines a rollback action. If a later step fails, the system can undo previous steps in reverse order.
8. **Audit Trail:** Every action, decision, and observation is logged with executor identity, timestamp, target, and outcome.
9. **Runbook Versioning:** Runbooks are version-controlled. Executions are pinned to a specific version. Old versions can be viewed for audit.
10. **ML-Based Runbook Selection:** Given a set of symptoms (health check failures, logs, metrics), automatically select the most appropriate runbook.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Runbook execution start latency | < 5 seconds from trigger |
| Step execution timeout | Configurable per step (default 60 seconds) |
| Approval response time | < 15 minutes (escalate if exceeded) |
| Concurrent runbook executions | Up to 100 |
| Runbook repository size | 5,000+ runbooks |
| Execution history retention | 2 years |
| Availability | 99.99% |

### Constraints & Assumptions
- Runbooks execute actions via existing infrastructure APIs: Kubernetes API, IPMI, MySQL admin, Kafka admin, SSH.
- The system integrates with the health check service (trigger source), the self-healing platform (escalation target), and the failover system (for database failover runbooks).
- Actions have side effects on production infrastructure — safety is paramount.
- Teams author runbooks; the platform team reviews and approves for production use.
- Assumption: existing monitoring (Prometheus, Elasticsearch) provides the observability data that runbooks query.

### Out of Scope
- The monitoring/alerting system itself (assumed to exist).
- Health check definitions (covered in health_check_and_remediation_service.md).
- The self-healing reconciliation loop (covered in self_healing_infrastructure.md).
- Application-level business logic runbooks (e.g., database migration scripts).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|---|---|---|
| Total runbooks | ~5,000 across all teams and infrastructure types | 5,000 |
| Auto-triggered executions/day | ~5,000 remediations/day (from health check system) × 80% have runbooks | ~4,000/day |
| Manual executions/day | ~100 operator-triggered | ~100/day |
| Total executions/day | 4,000 + 100 | ~4,100/day |
| Average steps per execution | ~8 steps (diagnose → decide → act → verify) | 8 |
| Total step executions/day | 4,100 × 8 | ~32,800/day |
| API calls/sec (CRUD + status queries) | ~200 (humans + automated triggers) | ~200/sec |
| Approval requests/day | ~200 (5% of executions require approval) | ~200/day |

### Latency Requirements

| Operation | Target | P99 |
|---|---|---|
| Runbook trigger (API call) | < 200ms | < 500ms |
| Execution start (trigger → first step) | < 5s | < 10s |
| Diagnostic step execution | < 30s | < 60s |
| Remediation step execution | < 5min | < 15min (varies by action) |
| Approval notification delivery | < 10s | < 30s |
| Approval timeout (escalation) | 15 minutes | N/A |
| Runbook search (by symptoms) | < 500ms | < 2s |
| Audit log query | < 1s | < 5s |

### Storage Estimates

| Data Type | Calculation | Result |
|---|---|---|
| Runbook definitions | 5,000 runbooks × 20 KB avg (YAML + metadata) | ~100 MB |
| Runbook versions (10 versions avg) | 100 MB × 10 | ~1 GB |
| Execution records | 4,100/day × 10 KB × 365 days × 2 years | ~30 GB |
| Step execution logs | 32,800/day × 2 KB × 365 × 2 | ~48 GB |
| Audit trail | 4,100/day × 5 KB × 365 × 2 | ~15 GB |
| ML model artifacts | ~500 MB (classification model + embeddings) | ~500 MB |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Runbook execution commands | 4,100/day × 10 KB | ~40 MB/day (negligible) |
| Monitoring queries (during execution) | 32,800 steps/day × 2 Prometheus queries × 1 KB | ~66 MB/day |
| Approval notifications | 200/day × 5 KB | ~1 MB/day |
| API traffic | 200/sec × 5 KB response | ~1 MB/sec |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    AUTO-REMEDIATION RUNBOOK SYSTEM                          │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    AUTHORING LAYER                                    │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │  │
│  │  │ Runbook       │  │ Version      │  │ Runbook      │              │  │
│  │  │ Editor        │  │ Control      │  │ Validator    │              │  │
│  │  │ (YAML/UI)     │  │ (Git-backed) │  │ (schema +   │              │  │
│  │  │               │  │              │  │  sandbox)    │              │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘              │  │
│  └──────────────────────────────┬────────────────────────────────────────┘  │
│                                 │                                           │
│  ┌──────────────────────────────▼────────────────────────────────────────┐  │
│  │                    EXECUTION LAYER                                    │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │  │
│  │  │ Execution     │  │ Decision     │  │ Step         │              │  │
│  │  │ Orchestrator  │  │ Tree Engine  │  │ Executor     │              │  │
│  │  │ (state        │  │ (branch      │  │ (plugin-     │              │  │
│  │  │  machine)     │  │  evaluation) │  │  based)      │              │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘              │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │  │
│  │  │ Approval      │  │ Blast Radius │  │ Rollback     │              │  │
│  │  │ Gate Manager  │  │ Controller   │  │ Engine       │              │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘              │  │
│  └──────────────────────────────┬────────────────────────────────────────┘  │
│                                 │                                           │
│  ┌──────────────────────────────▼────────────────────────────────────────┐  │
│  │                    INTELLIGENCE LAYER                                 │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐              │  │
│  │  │ Symptom       │  │ Runbook      │  │ Outcome      │              │  │
│  │  │ Classifier    │  │ Recommender  │  │ Learner      │              │  │
│  │  │ (ML model)    │  │ (RAG)        │  │ (feedback    │              │  │
│  │  │               │  │              │  │  loop)       │              │  │
│  │  └──────────────┘  └──────────────┘  └──────────────┘              │  │
│  └──────────────────────────────┬────────────────────────────────────────┘  │
│                                 │                                           │
│  ┌──────────────────────────────▼────────────────────────────────────────┐  │
│  │                    ACTION PLUGINS                                     │  │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌──────────────┐  │  │
│  │  │ K8s     │ │ MySQL   │ │ IPMI    │ │ SSH     │ │ HTTP/API     │  │  │
│  │  │ Plugin  │ │ Plugin  │ │ Plugin  │ │ Plugin  │ │ Plugin       │  │  │
│  │  └─────────┘ └─────────┘ └─────────┘ └─────────┘ └──────────────┘  │  │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐                   │  │
│  │  │ Kafka   │ │ ES      │ │ DNS     │ │ Prome-  │                   │  │
│  │  │ Plugin  │ │ Plugin  │ │ Plugin  │ │ theus   │                   │  │
│  │  │         │ │         │ │         │ │ Plugin  │                   │  │
│  │  └─────────┘ └─────────┘ └─────────┘ └─────────┘                   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  DATA LAYER                                                          │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐           │  │
│  │  │ MySQL    │  │ Git Repo │  │ Elastic  │  │ Redis    │           │  │
│  │  │ (exec    │  │ (runbook │  │ Search   │  │ (exec    │           │  │
│  │  │  state,  │  │  source) │  │ (logs,   │  │  locks,  │           │  │
│  │  │  audit)  │  │          │  │  search) │  │  state)  │           │  │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘           │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Runbook Editor** | Web-based editor and CLI for authoring runbooks in YAML with live validation and preview. |
| **Version Control** | Git-backed storage for runbooks. Every change is a commit. Branches for development, main for production. |
| **Runbook Validator** | Schema validation (YAML structure), safety checks (blast radius defined, rollback defined for each action), and sandbox execution (dry-run against test environment). |
| **Execution Orchestrator** | State machine that drives runbook execution. Persists state to MySQL/Redis for crash recovery. |
| **Decision Tree Engine** | Evaluates conditional branches in the runbook based on diagnostic step outputs. |
| **Step Executor** | Dispatches individual steps to the appropriate action plugin. Handles timeouts, retries, and error capture. |
| **Approval Gate Manager** | Sends approval requests via Slack/PagerDuty, tracks responses, enforces timeouts. |
| **Blast Radius Controller** | Enforces per-runbook and global limits on concurrent affected entities. |
| **Rollback Engine** | On failure, executes rollback steps in reverse order. Tracks which steps need rollback. |
| **Symptom Classifier** | ML model that classifies failure symptoms into categories for runbook selection. |
| **Runbook Recommender** | RAG-based system that retrieves the most relevant runbook given a classified failure. |
| **Outcome Learner** | Feedback loop that tracks runbook success/failure rates and uses outcomes to improve the classifier and recommender. |
| **Action Plugins** | Plugin-based executors for each infrastructure action type (K8s API, MySQL admin, IPMI, SSH, etc.). |

### Data Flows

**Automated Trigger Flow:**
1. Health check system detects failure → publishes to Kafka topic `remediation-triggers`.
2. Runbook system consumes trigger, extracts symptoms.
3. Symptom Classifier categorizes the failure.
4. Runbook Recommender selects the best runbook (confidence > 0.8) or escalates to human.
5. Execution Orchestrator starts the runbook execution.
6. Decision Tree Engine processes diagnostic steps → determines remediation path.
7. Step Executor dispatches actions via plugins.
8. Approval Gate Manager pauses for human approval (if required).
9. Post-conditions verified after each action step.
10. On success: log and close. On failure: Rollback Engine undoes completed steps.

**Manual Trigger Flow:**
1. Operator selects runbook and target via CLI/UI.
2. System validates preconditions and blast radius.
3. Operator can run in dry-run mode first.
4. On confirmation, execution proceeds as above.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Runbook definition (metadata — actual YAML is in Git)
CREATE TABLE runbook (
    runbook_id          VARCHAR(128) PRIMARY KEY,
    name                VARCHAR(256) NOT NULL,
    description         TEXT,
    category            ENUM('host_recovery', 'pod_recovery', 'database', 'network',
                             'storage', 'kafka', 'elasticsearch', 'kubernetes',
                             'bare_metal', 'security', 'capacity') NOT NULL,
    target_type         ENUM('host', 'pod', 'service', 'cluster', 'bare_metal',
                             'mysql_cluster', 'kafka_cluster', 'es_cluster') NOT NULL,
    severity            ENUM('critical', 'high', 'medium', 'low') NOT NULL,
    owner_team          VARCHAR(128) NOT NULL,
    current_version     INT NOT NULL DEFAULT 1,
    git_path            VARCHAR(512) NOT NULL,         -- path in git repo
    auto_execute        BOOLEAN NOT NULL DEFAULT FALSE, -- can be auto-triggered
    requires_approval   BOOLEAN NOT NULL DEFAULT TRUE,
    blast_radius_limit  JSON NOT NULL,                  -- {"max_targets": 5, "max_pct": 2}
    tags                JSON,                           -- for search
    enabled             BOOLEAN NOT NULL DEFAULT TRUE,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_category (category),
    INDEX idx_target_type (target_type),
    INDEX idx_owner (owner_team),
    INDEX idx_auto (auto_execute, enabled)
) ENGINE=InnoDB;

-- Runbook version history
CREATE TABLE runbook_version (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    runbook_id          VARCHAR(128) NOT NULL,
    version             INT NOT NULL,
    git_commit_sha      VARCHAR(40) NOT NULL,
    change_description  TEXT,
    changed_by          VARCHAR(128) NOT NULL,
    reviewed_by         VARCHAR(128),
    review_status       ENUM('pending', 'approved', 'rejected') NOT NULL DEFAULT 'pending',
    yaml_content_hash   VARCHAR(64) NOT NULL,           -- SHA-256 of YAML content
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (runbook_id) REFERENCES runbook(runbook_id),
    UNIQUE INDEX idx_runbook_version (runbook_id, version)
) ENGINE=InnoDB;

-- Execution record
CREATE TABLE execution (
    execution_id        VARCHAR(128) PRIMARY KEY,
    runbook_id          VARCHAR(128) NOT NULL,
    runbook_version     INT NOT NULL,
    trigger_type        ENUM('automated', 'manual', 'scheduled') NOT NULL,
    trigger_source      VARCHAR(256),                   -- e.g., "health-check:chk-123" or "operator:jane"
    target_entities     JSON NOT NULL,                  -- list of entity IDs being remediated
    status              ENUM('pending', 'running', 'waiting_approval', 'rolling_back',
                             'succeeded', 'failed', 'aborted', 'dry_run_completed') NOT NULL DEFAULT 'pending',
    dry_run             BOOLEAN NOT NULL DEFAULT FALSE,
    current_step_index  INT NOT NULL DEFAULT 0,
    total_steps         INT NOT NULL,
    started_at          TIMESTAMP NULL,
    completed_at        TIMESTAMP NULL,
    duration_ms         INT,
    result_summary      TEXT,
    rollback_executed   BOOLEAN NOT NULL DEFAULT FALSE,
    executor            VARCHAR(128) NOT NULL,           -- service account or human username
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (runbook_id) REFERENCES runbook(runbook_id),
    INDEX idx_runbook (runbook_id, created_at),
    INDEX idx_status (status),
    INDEX idx_trigger (trigger_type, created_at),
    INDEX idx_target (target_entities(255))
) ENGINE=InnoDB;

-- Step execution record
CREATE TABLE step_execution (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    execution_id        VARCHAR(128) NOT NULL,
    step_index          INT NOT NULL,
    step_name           VARCHAR(256) NOT NULL,
    step_type           ENUM('diagnostic', 'condition', 'action', 'verification',
                             'approval', 'notification', 'wait', 'rollback') NOT NULL,
    plugin              VARCHAR(64),                     -- which action plugin was used
    command             TEXT,                             -- the actual command/query executed
    precondition_result ENUM('passed', 'failed', 'skipped') NULL,
    execution_result    ENUM('success', 'failure', 'timeout', 'skipped', 'dry_run') NOT NULL,
    postcondition_result ENUM('passed', 'failed', 'skipped') NULL,
    output              TEXT,                             -- step output (truncated if large)
    error_message       TEXT,
    duration_ms         INT,
    rollback_status     ENUM('not_needed', 'pending', 'completed', 'failed') NOT NULL DEFAULT 'not_needed',
    started_at          TIMESTAMP(3) NULL,
    completed_at        TIMESTAMP(3) NULL,
    FOREIGN KEY (execution_id) REFERENCES execution(execution_id),
    INDEX idx_execution (execution_id, step_index)
) ENGINE=InnoDB;

-- Approval record
CREATE TABLE approval_request (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    execution_id        VARCHAR(128) NOT NULL,
    step_index          INT NOT NULL,
    requested_at        TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    timeout_at          TIMESTAMP NOT NULL,
    approver            VARCHAR(128),
    decision            ENUM('pending', 'approved', 'rejected', 'timeout') NOT NULL DEFAULT 'pending',
    reason              TEXT,
    decided_at          TIMESTAMP NULL,
    notification_channel ENUM('slack', 'pagerduty', 'email') NOT NULL,
    notification_id     VARCHAR(256),                    -- Slack message ID or PD incident ID
    FOREIGN KEY (execution_id) REFERENCES execution(execution_id),
    INDEX idx_execution_step (execution_id, step_index),
    INDEX idx_pending (decision, timeout_at)
) ENGINE=InnoDB;

-- Symptom-to-runbook mapping (for ML training)
CREATE TABLE symptom_runbook_mapping (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    symptoms_json       JSON NOT NULL,                   -- {check_type, failure_type, entity_type, error_pattern, ...}
    runbook_id          VARCHAR(128) NOT NULL,
    confidence          DECIMAL(3,2) NOT NULL,
    outcome             ENUM('success', 'failure', 'partial') NULL,  -- filled after execution
    feedback_by         VARCHAR(128),
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (runbook_id) REFERENCES runbook(runbook_id),
    INDEX idx_runbook (runbook_id),
    INDEX idx_outcome (outcome)
) ENGINE=InnoDB;

-- Execution lock (prevent concurrent execution on same target)
CREATE TABLE execution_lock (
    target_entity       VARCHAR(256) PRIMARY KEY,
    execution_id        VARCHAR(128) NOT NULL,
    locked_at           TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at          TIMESTAMP NOT NULL,
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB;
```

### Database Selection

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **MySQL 8.0** | ACID for execution state and audit; JSON for flexible configs | N/A — good fit | **Selected for execution state, audit, metadata** |
| **Git (e.g., Gitea/GitLab)** | Version control, code review, branch management, diff/blame | Not queryable | **Selected for runbook YAML storage** |
| **Elasticsearch** | Search across runbooks and execution logs | Eventually consistent | **Selected for search and log exploration** |
| **Redis** | Fast execution state, distributed locks | Ephemeral | **Selected for execution locks and caching** |

**Justification:** Four-store architecture aligned with each store's strengths. Git for runbook source code (version control is essential). MySQL for authoritative execution state and audit (ACID guarantees). Elasticsearch for full-text search across runbook content and execution logs. Redis for fast operational state (execution locks, step status polling).

### Indexing Strategy

**MySQL:**
- `runbook(category, target_type)` — find runbooks for a failure category.
- `execution(runbook_id, created_at)` — execution history for a runbook.
- `execution(status)` — find all running executions.
- `step_execution(execution_id, step_index)` — ordered step history for an execution.
- `approval_request(decision, timeout_at)` — find pending approvals about to timeout.

**Elasticsearch:**
- Index: `runbooks` — full-text search on name, description, tags, YAML content.
- Index: `execution-logs-YYYY-MM` — searchable execution details and step outputs.

**Git:**
- Directory structure: `runbooks/{category}/{target_type}/{runbook_id}.yaml`.
- Tags for promoted versions: `v{version}-{runbook_id}`.

---

## 5. API Design

### REST Endpoints

```
Base URL: https://runbook.infra.internal/api/v1

# Runbook management
POST   /runbooks
  Body: {
    "runbook_id": "rb-host-disk-failure",
    "name": "Host Disk Failure Remediation",
    "category": "host_recovery",
    "target_type": "bare_metal",
    "severity": "high",
    "owner_team": "infra-sre",
    "auto_execute": true,
    "requires_approval": false,
    "blast_radius_limit": {"max_targets": 3, "max_pct": 1},
    "tags": ["disk", "bare-metal", "hardware"],
    "yaml_content": "... (runbook YAML, see format below) ..."
  }
  Response: 201 { "runbook_id": "rb-host-disk-failure", "version": 1 }

GET    /runbooks?category=host_recovery&target_type=bare_metal&enabled=true
  Response: 200 [ list of runbooks with metadata ]

GET    /runbooks/{runbook_id}
  Query: ?version=3  (optional — default: current)
  Response: 200 { full runbook definition including YAML steps }

PUT    /runbooks/{runbook_id}
  Body: { updated fields, new yaml_content }
  Response: 200 { "version": 4 }

POST   /runbooks/{runbook_id}/review
  Body: { "reviewer": "senior-sre", "decision": "approved", "comments": "LGTM" }
  Response: 200

# Search
GET    /runbooks/search?q=disk+failure+bare+metal&limit=10
  Response: 200 [ ranked list of matching runbooks ]

POST   /runbooks/recommend
  Body: {
    "symptoms": {
      "entity_type": "bare_metal",
      "check_types_failing": ["hardware", "kernel"],
      "error_patterns": ["I/O error", "SMART threshold exceeded"],
      "entity_labels": {"role": "compute", "gpu": "false"}
    }
  }
  Response: 200 [
    {"runbook_id": "rb-host-disk-failure", "confidence": 0.92, "reason": "Matches disk I/O error pattern"},
    {"runbook_id": "rb-host-reimage", "confidence": 0.71, "reason": "Fallback for persistent hardware issues"}
  ]

# Execution
POST   /executions
  Body: {
    "runbook_id": "rb-host-disk-failure",
    "target_entities": ["host-prod-0042"],
    "trigger_type": "automated",
    "trigger_source": "health-check:chk-hw-disk-0042",
    "dry_run": false,
    "parameters": {
      "disk_device": "/dev/sda",
      "replacement_strategy": "reimage_if_degraded"
    }
  }
  Response: 202 { "execution_id": "exec-abc123", "status": "pending" }

GET    /executions/{execution_id}
  Response: 200 {
    "execution_id": "exec-abc123",
    "status": "running",
    "current_step": 3,
    "total_steps": 8,
    "steps": [
      {"index": 0, "name": "Check SMART status", "type": "diagnostic", "result": "success",
       "output": "Reallocated_Sector_Ct: 150 (threshold: 100)"},
      {"index": 1, "name": "Check data replication", "type": "diagnostic", "result": "success",
       "output": "All PVs replicated: yes"},
      {"index": 2, "name": "Drain node", "type": "action", "result": "success",
       "output": "23 pods evicted, 0 PDB violations"},
      {"index": 3, "name": "IPMI power cycle", "type": "action", "result": "running"}
    ]
  }

POST   /executions/{execution_id}/abort
  Body: { "reason": "Operator decision — investigating root cause manually" }
  Response: 200 { "status": "rolling_back" }

POST   /executions/{execution_id}/approve
  Body: { "approver": "sre-lead", "decision": "approved", "reason": "Confirmed bad disk" }
  Response: 200

# Execution history
GET    /executions?runbook_id=rb-host-disk-failure&status=failed&since=2026-04-01
  Response: 200 [ list of failed executions for analysis ]

# Rollback
POST   /executions/{execution_id}/rollback
  Body: { "reason": "Post-condition failed after step 5" }
  Response: 202 { "status": "rolling_back" }
```

### Runbook YAML Format

```yaml
# rb-host-disk-failure.yaml
apiVersion: runbook.infra.internal/v1
kind: Runbook
metadata:
  id: rb-host-disk-failure
  name: Host Disk Failure Remediation
  description: >
    Handles bare-metal hosts with disk failures detected via SMART or kernel I/O errors.
    Diagnoses severity, drains workloads, and either reimages or decommissions the host.
  category: host_recovery
  target_type: bare_metal
  severity: high
  owner: infra-sre
  tags: [disk, bare-metal, hardware, smart]

parameters:
  - name: disk_device
    type: string
    default: "/dev/sda"
    description: "The failing disk device"
  - name: replacement_strategy
    type: enum
    values: [reimage_if_degraded, decommission_immediately]
    default: reimage_if_degraded

blast_radius:
  max_targets: 3
  max_percentage: 1     # max 1% of cluster
  max_per_rack: 1

steps:
  - name: check_smart_status
    type: diagnostic
    plugin: ssh
    command: "smartctl -A {{ disk_device }} | grep -E 'Reallocated_Sector|Current_Pending|Offline_Uncorrectable'"
    timeout_sec: 30
    output_var: smart_output
    
  - name: assess_severity
    type: condition
    conditions:
      - if: "{{ smart_output.Reallocated_Sector_Ct | int > 500 }}"
        goto: decommission_path
        reason: "Severe disk degradation: >500 reallocated sectors"
      - if: "{{ smart_output.Reallocated_Sector_Ct | int > 100 }}"
        goto: reimage_path
        reason: "Moderate disk degradation: >100 reallocated sectors"
      - else:
        goto: monitor_path
        reason: "Disk degradation below threshold"
    
  - name: check_data_replication
    type: diagnostic
    id: reimage_path
    plugin: kubernetes
    command: "check_pv_replication"
    args:
      node: "{{ target_entity }}"
    precondition:
      check: "node_exists"
      args: { node: "{{ target_entity }}" }
    timeout_sec: 30
    output_var: replication_status
    
  - name: verify_data_safe
    type: condition
    conditions:
      - if: "{{ replication_status.all_replicated == false }}"
        goto: escalate_data_risk
        reason: "Not all PVs are replicated — cannot safely drain"
    
  - name: drain_node
    type: action
    plugin: kubernetes
    command: "drain_node"
    args:
      node: "{{ target_entity }}"
      grace_period_sec: 60
      respect_pdb: true
      timeout_sec: 300
    precondition:
      check: "node_is_schedulable"
      args: { node: "{{ target_entity }}" }
    postcondition:
      check: "node_has_no_pods"
      args: { node: "{{ target_entity }}" }
    rollback:
      command: "uncordon_node"
      args: { node: "{{ target_entity }}" }
    timeout_sec: 300
    
  - name: ipmi_power_cycle
    type: action
    plugin: ipmi
    command: "power_cycle"
    args:
      host: "{{ target_entity }}"
    postcondition:
      check: "host_is_reachable"
      args: { host: "{{ target_entity }}", timeout_sec: 300 }
    rollback:
      command: "power_on"
      args: { host: "{{ target_entity }}" }
    timeout_sec: 600
    
  - name: verify_disk_health_after_reboot
    type: verification
    plugin: ssh
    command: "smartctl -H {{ disk_device }}"
    timeout_sec: 30
    conditions:
      - if: "{{ output contains 'PASSED' }}"
        goto: uncordon_and_finish
      - else:
        goto: reimage_host
        
  - name: reimage_host
    type: action
    requires_approval: true
    approval_message: >
      Host {{ target_entity }} disk {{ disk_device }} failed health check after reboot.
      SMART status: {{ smart_output }}.
      Reimage will destroy local data (PVs already replicated: {{ replication_status }}).
      Approve PXE reimage?
    plugin: bare_metal
    command: "pxe_reimage"
    args:
      host: "{{ target_entity }}"
      image: "ubuntu-22.04-base-latest"
    postcondition:
      check: "host_passes_burn_in"
      args: { host: "{{ target_entity }}", timeout_sec: 900 }
    rollback:
      command: "decommission_host"
      args: { host: "{{ target_entity }}" }
      reason: "Reimage failed — host needs hardware investigation"
    timeout_sec: 1800
    
  - name: uncordon_and_finish
    type: action
    id: uncordon_and_finish
    plugin: kubernetes
    command: "uncordon_node"
    args:
      node: "{{ target_entity }}"
    postcondition:
      check: "node_is_schedulable"
      args: { node: "{{ target_entity }}" }

  - name: escalate_data_risk
    type: notification
    id: escalate_data_risk
    plugin: pagerduty
    command: "page"
    args:
      service: "data-team"
      severity: "high"
      message: >
        Host {{ target_entity }} has disk failure but unreplicated PVs.
        Cannot auto-remediate. Manual intervention required.
        SMART: {{ smart_output }}
        Unreplicated PVs: {{ replication_status.unreplicated_pvs }}

  - name: decommission_path
    type: action
    id: decommission_path
    plugin: bare_metal
    command: "decommission_host"
    args:
      host: "{{ target_entity }}"
      reason: "Severe disk degradation"
      file_hardware_ticket: true

  - name: monitor_path
    type: notification
    id: monitor_path
    plugin: slack
    command: "send_message"
    args:
      channel: "#infra-alerts"
      message: >
        Host {{ target_entity }} has minor disk degradation ({{ smart_output }}).
        Monitoring — no immediate action required.
```

### CLI Design

```bash
# Runbook management
runbook list --category=host_recovery --enabled
runbook show rb-host-disk-failure --version=latest
runbook show rb-host-disk-failure --version=3 --diff-with=2   # diff versions
runbook create --file=./rb-host-disk-failure.yaml
runbook update rb-host-disk-failure --file=./rb-host-disk-failure.yaml
runbook validate ./rb-host-disk-failure.yaml                   # schema + safety validation
runbook review rb-host-disk-failure --approve --comment="LGTM"

# Execution
runbook execute rb-host-disk-failure --target=host-prod-0042 --dry-run
runbook execute rb-host-disk-failure --target=host-prod-0042
runbook execute rb-host-disk-failure --target=host-prod-0042 --param disk_device=/dev/sdb
runbook status exec-abc123                                     # execution status
runbook status exec-abc123 --watch                             # live updates
runbook abort exec-abc123 --reason="manual investigation"
runbook rollback exec-abc123 --reason="postcondition failed"
runbook approve exec-abc123 --step=6 --reason="confirmed bad disk"

# Search and recommend
runbook search "disk failure bare metal"
runbook recommend --symptoms='{"entity_type":"bare_metal","error":"I/O error on /dev/sda"}'

# History and analytics
runbook history rb-host-disk-failure --last=90d --format=table
runbook analytics --category=host_recovery --last=30d  # success rates, avg duration, etc.
```

---

## 6. Core Component Deep Dives

### 6.1 Decision Tree Execution Engine

**Why it's hard:** Runbooks are not simple scripts — they are decision trees with conditional branches, loops (retry), and parallel paths. The engine must handle: (a) arbitrary branching based on diagnostic output, (b) variable interpolation across steps, (c) timeouts per step and per execution, (d) crash recovery (resume from the last completed step), and (e) concurrent execution of independent diagnostic steps.

**Approaches Compared:**

| Approach | Flexibility | Crash Recovery | Complexity | Performance |
|---|---|---|---|---|
| **Sequential script execution** | Low — linear only | Poor | Low | Fast |
| **Directed Acyclic Graph (DAG)** | High — branching, parallel | Medium | Medium | Good |
| **State machine per execution** | High — any flow pattern | Excellent | High | Good |
| **Workflow engine (Temporal)** | Very high — built-in retry, timers | Excellent | High (external dependency) | Good |

**Selected Approach: DAG-based execution with persistent state (inspired by Argo Workflows).**

**Justification:** A DAG captures the natural structure of runbooks: diagnostic steps can run in parallel, condition nodes branch to different paths, and action nodes run sequentially within a path. State is persisted to MySQL after each step completion, enabling crash recovery. We avoid a full workflow engine (Temporal) to reduce dependencies — the runbook execution engine is simpler than a general-purpose workflow engine.

**Implementation:**

```python
class RunbookExecutor:
    def execute(self, execution_id: str):
        execution = load_execution(execution_id)
        runbook = load_runbook(execution.runbook_id, execution.runbook_version)
        dag = build_dag(runbook.steps)
        context = ExecutionContext(
            variables=execution.parameters,
            target_entities=execution.target_entities,
            dry_run=execution.dry_run
        )
        
        # Resume from last completed step (crash recovery)
        completed_steps = load_completed_steps(execution_id)
        for step in completed_steps:
            context.set_output(step.step_name, step.output)
        
        while dag.has_next(completed_steps):
            next_steps = dag.get_ready_steps(completed_steps)
            
            for step in next_steps:
                # Check precondition
                if step.precondition:
                    pre_result = evaluate_precondition(step.precondition, context)
                    if not pre_result.passed:
                        record_step(execution_id, step, "skipped", pre_result.reason)
                        continue
                
                # Handle different step types
                if step.type == "condition":
                    branch = evaluate_conditions(step.conditions, context)
                    record_step(execution_id, step, "success", f"Branch: {branch}")
                    dag.follow_branch(step, branch)
                
                elif step.type == "approval":
                    request_approval(execution_id, step)
                    # Execution pauses here; resumes on approval callback
                    return  # will be re-invoked when approval arrives
                
                elif step.type == "diagnostic":
                    output = execute_step(step, context)
                    context.set_output(step.name, output)
                    record_step(execution_id, step, "success", output)
                
                elif step.type == "action":
                    if execution.dry_run:
                        record_step(execution_id, step, "dry_run",
                                    f"Would execute: {step.plugin}.{step.command}")
                        continue
                    
                    # Execute with timeout
                    try:
                        output = execute_step_with_timeout(step, context, step.timeout_sec)
                    except TimeoutError:
                        record_step(execution_id, step, "timeout")
                        trigger_rollback(execution_id, completed_steps)
                        return
                    except Exception as e:
                        record_step(execution_id, step, "failure", str(e))
                        trigger_rollback(execution_id, completed_steps)
                        return
                    
                    # Check postcondition
                    if step.postcondition:
                        post_result = evaluate_postcondition(step.postcondition, context)
                        if not post_result.passed:
                            record_step(execution_id, step, "failure",
                                        f"Postcondition failed: {post_result.reason}")
                            trigger_rollback(execution_id, completed_steps)
                            return
                    
                    record_step(execution_id, step, "success", output)
                
                elif step.type == "verification":
                    output = execute_step(step, context)
                    context.set_output(step.name, output)
                    record_step(execution_id, step, "success", output)
                
                completed_steps.add(step.name)
        
        # All steps completed
        mark_execution_complete(execution_id, "succeeded")
```

**Variable Interpolation:**
```python
class ExecutionContext:
    """Jinja2-based template engine for variable interpolation in runbook steps."""
    
    def resolve(self, template: str) -> str:
        """Resolve {{ variable }} references in step commands and arguments."""
        return jinja2.Template(template).render(**self.variables)
    
    # Example:
    # template: "smartctl -A {{ disk_device }}"
    # variables: {"disk_device": "/dev/sda"}
    # result: "smartctl -A /dev/sda"
```

**Crash Recovery:**
```
On engine restart:
1. Query: SELECT * FROM execution WHERE status = 'running'
2. For each running execution:
   a. Load completed steps from step_execution table
   b. Rebuild execution context from step outputs
   c. Resume from the next uncompleted step
   d. If a step was "running" when the engine crashed:
      - Check if the action was actually executed (idempotent check)
      - If yes: record as complete and continue
      - If no: re-execute the step
```

**Failure Modes:**
- **Engine crash during step execution:** Crash recovery resumes from last checkpoint. Steps must be idempotent.
- **Step timeout:** Recorded as failure; triggers rollback.
- **Variable resolution failure:** Treated as step failure; abort execution with clear error message.
- **Circular dependency in DAG:** Detected at validation time (YAML upload). Cycle detection using topological sort.

**Interviewer Q&As:**

**Q1: How do you ensure idempotency of runbook steps?**
A: Each action plugin implements an idempotency check. Example: "drain node" — before draining, check if the node is already cordoned and has no running pods. If yes, the drain is a no-op. "Reimage host" — check if the host is already running the target OS image. Step outputs include a "was_noop" flag for audit clarity.

**Q2: How do you handle parallel steps in the DAG?**
A: Steps with no dependency between them can run in parallel. The DAG engine identifies steps with all dependencies satisfied and dispatches them concurrently. A thread pool (configurable, default 5 threads) executes parallel steps. Results are collected and dependencies resolved before advancing to the next DAG layer.

**Q3: What if a step produces unexpected output that breaks downstream steps?**
A: Output schemas are validated against expected types. If a diagnostic step is expected to produce a JSON object with a specific field, and it doesn't, the step fails with "output schema mismatch." This prevents garbage data from propagating through the decision tree.

**Q4: How do you handle runbooks that need to wait for an external process (e.g., wait for a node to reboot)?**
A: The `wait` step type: the engine records that it's waiting, sets a check interval (e.g., every 30s), and periodically evaluates a condition (e.g., "is the host reachable?"). The execution state is persisted, so if the engine restarts during the wait, it resumes checking. Max wait time is configurable (default: 15 minutes for reboot).

**Q5: Can runbook steps call other runbooks (composition)?**
A: Yes. A step can reference another runbook as a sub-execution: `plugin: runbook, command: execute, args: {runbook_id: rb-drain-node}`. The sub-execution runs as a child of the parent execution, with its own step tracking. The parent waits for the child to complete before advancing.

**Q6: How do you prevent two concurrent executions from conflicting on the same target?**
A: Execution locks. Before starting, the engine acquires a lock: `INSERT INTO execution_lock (target_entity, execution_id, expires_at)`. If the lock already exists (another execution is running on that target), the new execution is queued or rejected. Locks expire automatically (max execution duration + buffer) to handle abandoned locks.

---

### 6.2 Rollback Engine

**Why it's hard:** Rollback must undo actions in reverse order, but not all actions are reversible, and some rollback actions have their own preconditions (e.g., can't uncordon a node if it's currently being reimaged). The engine must handle partial rollback (some steps rolled back successfully, others can't be rolled back) and clearly report the final state.

**Approaches Compared:**

| Approach | Completeness | Complexity | Risk |
|---|---|---|---|
| **No rollback (manual only)** | N/A | None | High (human must fix everything) |
| **Per-step rollback actions** | High | Medium | Medium (rollback can also fail) |
| **Compensating transactions** | Very high | High | Low |
| **Checkpoint + restore** | Highest (VM snapshots) | Very high | Low (but slow) |

**Selected Approach: Per-step rollback actions with failure handling.**

**Justification:** Each action step defines its own rollback command. This is practical for infrastructure operations (drain → uncordon, power-off → power-on, reimage → decommission as last resort). Compensating transactions are overkill for infrastructure operations that are naturally reversible.

**Implementation:**

```python
class RollbackEngine:
    def rollback(self, execution_id: str, failed_step_index: int):
        execution = load_execution(execution_id)
        completed_steps = load_completed_action_steps(execution_id)
        
        # Sort steps in reverse order (undo most recent first)
        steps_to_rollback = sorted(
            [s for s in completed_steps if s.step_index < failed_step_index and s.step_type == 'action'],
            key=lambda s: s.step_index,
            reverse=True
        )
        
        rollback_results = []
        for step in steps_to_rollback:
            runbook_step = get_runbook_step(execution.runbook_id, step.step_index)
            
            if not runbook_step.rollback:
                rollback_results.append(RollbackResult(
                    step=step.step_name,
                    result="no_rollback_defined",
                    detail="Step does not define a rollback action"
                ))
                continue
            
            try:
                context = rebuild_context(execution_id, step.step_index)
                
                # Check if rollback is safe (preconditions)
                if runbook_step.rollback.precondition:
                    pre = evaluate_precondition(runbook_step.rollback.precondition, context)
                    if not pre.passed:
                        rollback_results.append(RollbackResult(
                            step=step.step_name,
                            result="rollback_precondition_failed",
                            detail=pre.reason
                        ))
                        continue
                
                # Execute rollback
                output = execute_plugin(
                    runbook_step.rollback.plugin or runbook_step.plugin,
                    runbook_step.rollback.command,
                    runbook_step.rollback.args,
                    context,
                    timeout_sec=runbook_step.rollback.timeout_sec or 300
                )
                
                rollback_results.append(RollbackResult(
                    step=step.step_name,
                    result="success",
                    detail=output
                ))
                
                update_step_rollback_status(execution_id, step.step_index, "completed")
                
            except Exception as e:
                rollback_results.append(RollbackResult(
                    step=step.step_name,
                    result="failed",
                    detail=str(e)
                ))
                update_step_rollback_status(execution_id, step.step_index, "failed")
                # Continue with other rollbacks — don't stop on single failure
        
        # Report final state
        all_succeeded = all(r.result == "success" for r in rollback_results)
        if all_succeeded:
            mark_execution_complete(execution_id, "rolled_back")
        else:
            mark_execution_complete(execution_id, "partial_rollback")
            alert_operator(execution_id, rollback_results)
```

**Rollback Examples:**

| Action Step | Rollback Step | Caveat |
|---|---|---|
| Cordon node | Uncordon node | Safe — idempotent |
| Drain node | Uncordon node (pods reschedule) | Pods don't return to original node |
| IPMI power off | IPMI power on | Host state after power-on may differ |
| PXE reimage | Decommission host | Can't un-reimage; best effort: file ticket |
| Change MySQL master | Reverse failover | High risk — may require manual intervention |
| Delete pod | No rollback (K8s recreates) | Natural — ReplicaSet handles recreation |

**Failure Modes:**
- **Rollback step fails:** Log the failure, continue with remaining rollbacks, alert operator. Never halt rollback.
- **Rollback causes additional damage:** Each rollback step has its own postcondition. If the postcondition fails, it's logged and the operator is alerted.
- **Rollback of a step that was never fully completed:** Before rolling back, check if the original action actually completed. If the action timed out (might have partially executed), rollback attempts best-effort reversal.

**Interviewer Q&As:**

**Q1: What about actions that are inherently irreversible?**
A: Document them clearly in the runbook. The YAML includes `rollback: null` with a comment explaining why. The execution engine knows to skip rollback for these steps and instead alerts the operator: "Step 'PXE reimage' is irreversible. Manual intervention may be required."

**Q2: How do you handle rollback when the target is unreachable?**
A: If the target is unreachable (network partition, host down), rollback actions that require target access (SSH, K8s API) will fail. The engine retries 3 times with 30s delay, then records the failure and alerts. For IPMI-based rollback (power on), the out-of-band path may still work even if in-band is down.

**Q3: How do you test rollback?**
A: Every runbook must pass a "rollback drill" during validation: execute the runbook in a test environment, then trigger rollback, and verify the environment returns to its pre-execution state. This is part of the CI pipeline for runbook changes.

**Q4: What if rollback takes longer than the original action?**
A: Each rollback step has its own timeout (default: same as the original step, configurable). If rollback times out, it's recorded as failed and the operator is alerted. Total rollback time is bounded by the sum of individual rollback timeouts.

**Q5: Can an operator partially rollback (undo some steps but not others)?**
A: Yes. `runbook rollback exec-abc123 --steps=5,4,3` rolls back only the specified steps in reverse order. This is useful when the operator knows which steps need undoing and which should remain.

**Q6: How do you handle the case where rollback creates a worse state than doing nothing?**
A: The rollback engine evaluates a "rollback safety score" before executing. If the rollback is more disruptive than the failed state (e.g., rolling back a node drain by uncordoning brings traffic back to a broken node), the engine recommends "skip rollback, escalate to human" and logs the reasoning.

---

### 6.3 ML-Based Runbook Selection

**Why it's hard:** With 5,000+ runbooks, selecting the right one for a given failure requires understanding the failure's symptoms and matching them to the runbook's purpose. Simple keyword matching fails because symptoms are described differently across teams. The classifier must handle ambiguity (multiple runbooks could match) and novelty (failure patterns not seen before).

**Approaches Compared:**

| Approach | Accuracy | Cold-Start | Maintenance | Explainability |
|---|---|---|---|---|
| **Manual symptom → runbook mapping table** | High (if maintained) | N/A | Very high | Perfect |
| **Keyword search (Elasticsearch)** | Medium | Good | Low | Good |
| **Classification model (XGBoost)** | High | Requires training data | Medium | Medium |
| **RAG (Retrieval-Augmented Generation)** | Very high | Good (embeds runbook content) | Low | Good |

**Selected Approach: RAG-based recommendation with classification model for initial filtering.**

**Justification:** RAG combines the strengths of semantic search (finds relevant runbooks even with novel phrasing) with LLM reasoning (ranks candidates considering full context). The classification model provides fast initial filtering (reduce 5,000 candidates to 50), and RAG provides precise ranking.

**Implementation:**

```python
class RunbookRecommender:
    def __init__(self):
        self.classifier = XGBoostClassifier()  # Trained on historical symptom → category mapping
        self.vector_store = ChromaDB("runbooks")  # Embedded runbook descriptions + YAML
        self.llm = LLMClient()
    
    def recommend(self, symptoms: dict) -> List[Recommendation]:
        # Stage 1: Classify failure category (fast, narrow the search)
        category = self.classifier.predict(self.extract_features(symptoms))
        # e.g., category = "disk_failure" with confidence 0.85
        
        # Stage 2: Semantic search for relevant runbooks in that category
        query = self.build_query(symptoms)
        candidates = self.vector_store.similarity_search(
            query,
            filter={"category": category.name} if category.confidence > 0.7 else None,
            k=10
        )
        
        # Stage 3: LLM-based ranking (considering full context)
        prompt = f"""
        Given these failure symptoms:
        {json.dumps(symptoms, indent=2)}
        
        And these candidate runbooks:
        {self.format_candidates(candidates)}
        
        Rank the runbooks by relevance. For each, provide:
        1. Rank (1 = most relevant)
        2. Confidence (0-1)
        3. Why this runbook matches (1 sentence)
        4. Any caveats or preconditions the operator should check
        
        If no runbook is a good match (confidence < 0.5 for all), say so explicitly.
        """
        
        ranking = self.llm.invoke(prompt, temperature=0.1)
        return self.parse_ranking(ranking)
    
    def extract_features(self, symptoms: dict) -> np.ndarray:
        """Extract numerical features for the classification model."""
        features = {
            'entity_type_encoded': encode_enum(symptoms.get('entity_type')),
            'check_types_failing': len(symptoms.get('check_types_failing', [])),
            'has_hardware_check': 'hardware' in symptoms.get('check_types_failing', []),
            'has_kernel_check': 'kernel' in symptoms.get('check_types_failing', []),
            'has_network_check': 'network' in symptoms.get('check_types_failing', []),
            'error_pattern_tfidf': self.tfidf.transform(symptoms.get('error_patterns', [])),
            'entity_labels_encoded': self.label_encoder.transform(symptoms.get('entity_labels', {})),
        }
        return self.feature_vectorizer.transform(features)
    
    def build_query(self, symptoms: dict) -> str:
        """Build a natural language query from symptoms for semantic search."""
        parts = []
        if symptoms.get('entity_type'):
            parts.append(f"Entity type: {symptoms['entity_type']}")
        if symptoms.get('check_types_failing'):
            parts.append(f"Failing checks: {', '.join(symptoms['check_types_failing'])}")
        if symptoms.get('error_patterns'):
            parts.append(f"Error patterns: {', '.join(symptoms['error_patterns'])}")
        return " | ".join(parts)
```

**Training Pipeline:**
```
Historical data:
  symptom_runbook_mapping table (50K+ rows from 2 years of operations)
    → Label: runbook_id
    → Features: entity_type, check_types_failing, error_patterns, entity_labels

Training:
  1. TF-IDF on error_patterns (vocabulary: 10K terms)
  2. One-hot encode entity_type, check_types
  3. Label encode entity_labels
  4. Train XGBoost multi-class classifier (5,000 classes = runbooks)
  5. Evaluate: precision@1 > 0.7, precision@5 > 0.9

Embedding pipeline (for RAG):
  1. For each runbook: concatenate name + description + tags + first 500 chars of YAML
  2. Embed using sentence-transformers (all-MiniLM-L6-v2)
  3. Store in ChromaDB with metadata (category, target_type, severity)
  4. Update on runbook change (webhook from Git)
```

**Feedback Loop:**
```python
class OutcomeLearner:
    def record_outcome(self, execution_id: str, outcome: str):
        """Record whether the selected runbook succeeded, for model retraining."""
        execution = load_execution(execution_id)
        mapping = SymptomRunbookMapping(
            symptoms=execution.trigger_symptoms,
            runbook_id=execution.runbook_id,
            confidence=execution.recommendation_confidence,
            outcome=outcome  # 'success', 'failure', 'partial'
        )
        save_mapping(mapping)
        
        # Retrain model weekly on updated dataset
        # If failure rate for a specific runbook > 30%, flag for review
        if get_failure_rate(execution.runbook_id, last_30d=True) > 0.3:
            alert_runbook_owner(execution.runbook_id,
                "Runbook failure rate exceeds 30% — review and update required")
```

**Failure Modes:**
- **ML model recommends wrong runbook:** The execution's precondition checks catch this early (step 1 of the runbook fails because the symptoms don't match). The execution aborts cleanly.
- **No runbook matches (novel failure):** The recommender returns confidence < 0.5. The system escalates to a human operator rather than executing a bad match.
- **Embedding drift (runbooks updated but embeddings stale):** Webhook triggers re-embedding when a runbook is updated. Nightly full re-embedding as a safety net.

**Interviewer Q&As:**

**Q1: Why RAG instead of a pure classification model?**
A: The classification model handles known symptom patterns well but fails on novel patterns. RAG excels at finding semantically similar runbooks even for never-before-seen symptom combinations, because it matches on meaning rather than exact feature values. RAG also naturally handles runbook additions without retraining.

**Q2: How do you handle runbook proliferation (too many runbooks)?**
A: (1) Regular review: runbooks with 0 executions in 90 days are flagged for deprecation. (2) Deduplication: the recommendation engine identifies runbooks with > 80% cosine similarity and suggests merging them. (3) Hierarchy: categories → sub-categories → specific runbooks, reducing search space.

**Q3: What's the cold-start problem for a new runbook?**
A: A new runbook has no execution history, so the classification model doesn't recommend it. RAG handles this: the new runbook's description and YAML are embedded immediately, and semantic search can find it for matching symptoms. The classification model is retrained weekly to include the new runbook.

**Q4: How do you handle a runbook that was correct historically but the infrastructure changed?**
A: Runbook versioning with review. When infrastructure changes (e.g., new OS version, new disk controller), the runbook owner must review and update. If a runbook starts failing after an infrastructure change, the feedback loop catches it (failure rate spike) and alerts the owner.

**Q5: Can the AI generate entirely new runbooks?**
A: Experimentally, yes. Given a new failure pattern and similar existing runbooks, an LLM can draft a new runbook. But generated runbooks are always placed in "draft" status and require human review before activation. We never auto-execute an AI-generated runbook without human validation.

**Q6: How do you evaluate the recommendation system's accuracy?**
A: (1) Precision@1: does the top recommendation match what the human would have selected? Target: > 70%. (2) Precision@5: is the correct runbook in the top 5? Target: > 90%. (3) Execution success rate: do recommended runbooks actually fix the problem? Target: > 85%. We evaluate monthly by replaying recent failure events through the recommender and comparing with actual human selections.

---

## 7. Scheduling & Resource Management

### Execution Scheduling

- Max 100 concurrent runbook executions (global limit).
- Per-cluster limit: max 10 concurrent executions.
- Priority queue: critical severity runbooks execute before medium/low.
- If the queue is full, low-priority executions are deferred; critical are never deferred.

### Resource Consumption

| Component | Resources | Scaling |
|---|---|---|
| Execution Orchestrator | 2 CPU, 4 GB memory per instance | 3 instances (1 leader, 2 standby) |
| Step Executors | 0.5 CPU, 512 MB per executor thread | Thread pool of 50 per instance |
| ML Inference (classifier + RAG) | 4 CPU, 8 GB memory (model loading) | 2 instances behind LB |
| API Server | 1 CPU, 2 GB memory | 3 instances behind LB |

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Strategy | Scale Unit |
|---|---|---|
| Execution Orchestrator | Leader-elected; scale by sharding clusters | 1 leader per cluster shard |
| Step Executors | Stateless workers from a shared queue | Scale with concurrent executions |
| API Server | Stateless behind LB | Scale with API traffic |
| ML Inference | Stateless; scale with recommendation requests | Add replicas as needed |

### Database Scaling

- MySQL: single primary + 2 read replicas. Write volume is low (~5 writes/sec for executions).
- Git: single Gitea/GitLab instance. 5,000 runbooks is trivial for Git.
- Elasticsearch: small cluster (3 nodes). Index volume is modest.
- Redis: single instance with replica (execution locks are lightweight).

### Caching

| Layer | Technology | Data | TTL |
|---|---|---|---|
| Runbook definitions | In-process | Parsed YAML (avoid re-parsing per execution) | 60s |
| ML model | In-process | Loaded model weights | Until new version deployed |
| Vector embeddings | ChromaDB (persistent) | Runbook embeddings | Permanent (updated on change) |
| Execution state | Redis | Current step, variables | TTL = max execution duration |
| Approval status | Redis | Pending/approved/rejected | 24h |

### Interviewer Q&As

**Q1: What if you need to handle 10,000 concurrent executions?**
A: Scale the step executor pool (more workers), shard the execution queue by cluster, and use more Kafka partitions for the trigger topic. The bottleneck is likely the action plugins (K8s API, IPMI, SSH), not the orchestrator. Rate limiting on action plugins prevents overwhelming the target infrastructure.

**Q2: How do you handle runbook execution that spans multiple hours (e.g., fleet-wide OS upgrade)?**
A: Long-running executions use the `wait` step type with periodic checkpoints. State is persisted, so engine restarts don't lose progress. We also support "batch" runbooks that iterate over a target list with rate limiting (e.g., "reimage 1 node per rack, wait 10 minutes, proceed to next").

**Q3: What's the latency of the ML recommendation?**
A: Stage 1 (classification): ~10ms (XGBoost inference). Stage 2 (vector search): ~50ms (ChromaDB). Stage 3 (LLM ranking): ~2-5s (API call to LLM). Total: ~3-6s. This is acceptable because recommendation happens once per trigger, not per step.

**Q4: How do you handle Git outages (runbook source unavailable)?**
A: Runbook YAML is cached locally after first fetch. If Git is unavailable, the system uses the cached version. Cache is refreshed on every runbook update (webhook) and periodically (every 5 minutes). A Git outage doesn't block execution.

**Q5: Can you scale the approval workflow?**
A: Approvals are async (Slack message, PagerDuty alert). The system polls for approval responses every 10 seconds (lightweight). With 200 approval requests/day, this is trivial. The bottleneck is human response time, not system capacity.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery |
|---|---|---|---|
| Execution Orchestrator crash | Running executions pause | Leader election lost | Standby takes over; resumes from last checkpoint |
| Step Executor timeout | Single step fails | Timeout timer | Trigger rollback for the execution |
| MySQL unavailable | Can't start new executions or record steps | Health check | Retry writes; buffer to Redis temporarily |
| Git unavailable | Can't fetch runbook YAML | Git health check | Use locally cached YAML |
| Redis unavailable | Can't acquire execution locks | Redis health check | Fall back to MySQL advisory locks |
| Approval timeout (human didn't respond) | Execution blocked | Timer in approval_request table | Escalate to next approver or abort |
| Action plugin failure (K8s API down) | Can't execute K8s steps | Plugin health check | Retry step; if persistent, abort and rollback |
| ML model returns bad recommendation | Wrong runbook selected | Precondition check fails early | Abort execution; escalate to human |
| Concurrent executions on same target | Conflicting actions | Execution lock check | Second execution queued or rejected |

### Automated Recovery

- Execution Orchestrator: 3 replicas with leader election. Crash recovery via persisted state.
- Step Executors: stateless workers. K8s Deployment ensures crashed pods are replaced.
- API Servers: stateless, behind LB. K8s Deployment handles restarts.
- Orphaned execution cleanup: every 10 minutes, scan for executions with status="running" and no recent step progress. If stalled > 30 minutes, mark as failed and trigger rollback.

### Retry Strategy

| Operation | Retries | Backoff | Notes |
|---|---|---|---|
| Step execution | 2 retries (configurable per step) | Exponential: 10s, 30s | Only for transient failures |
| Rollback step | 3 retries | Exponential: 5s, 15s, 45s | Rollback must try harder |
| Approval notification | 5 retries | Linear: 60s each | Must deliver notification |
| MySQL write | 3 retries on deadlock | 100ms, 200ms, 400ms | Standard MySQL retry |
| Plugin connection | 3 retries | Exponential: 1s, 3s, 9s | K8s API, IPMI, SSH |

### Circuit Breaker

- Per-plugin circuit breaker: if a plugin (e.g., K8s API) fails 5 consecutive calls, open circuit for 60 seconds. Affects all executions using that plugin.
- Per-runbook circuit breaker: if a specific runbook fails 5 consecutive executions, disable auto-execution and alert the runbook owner.

### Consensus & Coordination

- Execution Orchestrator leader election: Kubernetes Lease (15s TTL).
- Execution locks: Redis `SET NX EX` (set-if-not-exists with expiry). Fall back to MySQL `GET_LOCK` if Redis unavailable.
- Approval coordination: MySQL table with atomic status updates (no distributed consensus needed for approvals).

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `runbook_executions_total` | Counter per runbook_id, status | N/A (track rate) |
| `runbook_execution_duration_sec` | Histogram per runbook_id | P99 > 2x expected |
| `runbook_success_rate` | Gauge per runbook_id | < 80% |
| `runbook_auto_trigger_rate` | Counter per runbook_id | Spike > 3x baseline |
| `step_execution_duration_sec` | Histogram per step_type, plugin | P99 > step timeout |
| `rollback_triggered_total` | Counter | > 10/day |
| `rollback_failure_total` | Counter | > 0 (critical) |
| `approval_pending_count` | Gauge | > 20 (backlog) |
| `approval_timeout_total` | Counter | > 5/day |
| `recommendation_confidence` | Histogram | Mean < 0.6 (model degradation) |
| `recommendation_accuracy` | Gauge | < 70% precision@1 |
| `execution_queue_depth` | Gauge | > 50 |
| `concurrent_executions` | Gauge | > 80 (near limit) |

### Distributed Tracing

- Each execution generates a trace: trigger → recommendation → orchestrator → step 1 → step 2 → ... → completion.
- Plugin calls include the trace ID for correlation with target system logs.
- 100% sampling for executions (they're infrequent and important).

### Logging

```json
{
  "timestamp": "2026-04-09T14:30:15Z",
  "level": "INFO",
  "service": "runbook-executor",
  "execution_id": "exec-abc123",
  "runbook_id": "rb-host-disk-failure",
  "step_index": 4,
  "step_name": "drain_node",
  "event": "step_completed",
  "result": "success",
  "duration_ms": 45000,
  "output_summary": "23 pods evicted, 0 PDB violations",
  "target": "host-prod-0042",
  "trace_id": "trace-xyz789"
}
```

### Alerting

| Alert | Severity | Routing |
|---|---|---|
| Rollback failure | P1 (page) | On-call SRE — manual cleanup needed |
| Runbook success rate < 80% | P2 (warn) | Runbook owner team |
| Approval backlog > 20 | P2 (warn) | SRE leads |
| Execution queue depth > 50 | P2 (warn) | Platform team |
| ML recommendation accuracy drop | P3 (ticket) | ML team |
| Concurrent execution near limit | P3 (info) | Platform team |

---

## 11. Security

### Auth & AuthZ

| Role | Permissions |
|---|---|
| `runbook-viewer` | Read runbooks, view execution history |
| `runbook-author` | Create/edit runbooks for their team, request review |
| `runbook-reviewer` | Approve runbook changes for their team |
| `runbook-operator` | Trigger manual executions, approve execution gates |
| `runbook-admin` | Full access, configure global settings, override locks |

**Runbook execution permissions:**
- The execution engine runs with a service account that has specific, scoped permissions per plugin:
  - K8s Plugin: can drain/cordon/uncordon nodes, delete pods, scale deployments. CANNOT delete namespaces, modify RBAC, access secrets.
  - IPMI Plugin: can power-cycle, power-off, power-on, read sensors. Credentials from Vault.
  - SSH Plugin: can execute pre-approved commands only (allowlist). CANNOT run arbitrary shell commands.
  - MySQL Plugin: can run `SHOW STATUS`, `SHOW SLAVE STATUS`, `STOP SLAVE`, `CHANGE MASTER`. CANNOT drop databases or modify user permissions.

**Runbook sandboxing:**
- Runbook YAML is not arbitrary code — it's declarative actions dispatched to pre-built plugins.
- Custom steps (type: custom) are restricted to a pre-approved list of scripts stored in the Git repo.
- No arbitrary shell execution from runbook YAML (prevents injection attacks).

### Audit Logging

Every execution is fully audited:
```json
{
  "execution_id": "exec-abc123",
  "runbook_id": "rb-host-disk-failure",
  "runbook_version": 3,
  "trigger": "automated",
  "trigger_source": "health-check:chk-hw-disk-0042",
  "executor": "system:runbook-engine",
  "target_entities": ["host-prod-0042"],
  "dry_run": false,
  "steps": [
    {
      "step": "check_smart_status",
      "type": "diagnostic",
      "result": "success",
      "output": "Reallocated_Sector_Ct: 150",
      "timestamp": "2026-04-09T14:30:15Z"
    },
    {
      "step": "drain_node",
      "type": "action",
      "result": "success",
      "command": "kubectl drain host-prod-0042 --grace-period=60",
      "output": "23 pods evicted",
      "timestamp": "2026-04-09T14:31:05Z"
    }
  ],
  "approvals": [
    {
      "step": "reimage_host",
      "approver": "sre-lead@company.com",
      "decision": "approved",
      "reason": "Confirmed bad disk",
      "timestamp": "2026-04-09T14:35:22Z"
    }
  ],
  "final_status": "succeeded",
  "total_duration_ms": 842000
}
```

Audit logs are immutable, retained for 2 years in MySQL and replicated to compliance storage (S3 with object lock).

---

## 12. Incremental Rollout Strategy

**Phase 1 (Week 1-4): Runbook Authoring Infrastructure**
- Deploy Git repo, YAML schema validator, runbook editor.
- Teams migrate existing wiki runbooks to YAML format.
- No execution yet — just authoring and review.

**Phase 2 (Week 5-8): Dry-Run Execution**
- Deploy execution engine in dry-run mode.
- Teams trigger runbooks in dry-run against staging.
- Validate: preconditions check correctly, decision trees branch correctly, outputs are meaningful.

**Phase 3 (Week 9-12): Manual Execution in Staging**
- Enable real execution in staging (not dry-run).
- Operators trigger runbooks manually via CLI.
- Validate: actions execute correctly, rollback works, postconditions verify.

**Phase 4 (Week 13-16): Manual Execution in Production**
- Enable real execution in production.
- All executions require human approval (no auto-trigger).
- Start with low-risk runbooks (restart process, restart pod).

**Phase 5 (Week 17-20): Auto-Triggered Execution (Tier 3-4)**
- Enable auto-triggering from the health check system for tier-3/4 services.
- ML recommendation system active with human confirmation.

**Phase 6 (Week 21-30): Full Automation**
- Enable auto-triggering for tier-1/2 with approval gates for high-impact actions.
- ML recommendation without human confirmation for high-confidence (> 0.9) matches.
- Remove approval gates for routine runbooks after 60 days of 100% success rate.

### Rollout Q&As

**Q1: How do you migrate from wiki runbooks to executable runbooks?**
A: Incremental. Each team migrates their top-10 most-frequently-executed runbooks first (80/20 rule). The platform team provides migration guides, templates, and 1:1 support. Wiki runbooks remain as documentation; YAML runbooks become the executable version.

**Q2: What if a team's runbook has a bug that causes an incident?**
A: Runbook review process catches most bugs before production. For bugs that slip through: (1) rollback support limits damage. (2) Blast radius limits cap the impact. (3) Post-incident review identifies the bug, and the runbook is updated. (4) The runbook's auto-execute flag is disabled until the fix is reviewed.

**Q3: How do you handle runbook sprawl (too many, poorly maintained)?**
A: (1) Ownership enforcement: every runbook has an owner team. (2) Staleness detection: runbooks not executed in 90 days are flagged. (3) Quality gates: runbooks must pass validation (preconditions, postconditions, rollback defined) to be production-eligible. (4) Annual audit: teams review their runbooks during a "runbook hygiene week."

**Q4: How do you measure the value of the runbook system?**
A: (1) MTTR reduction: time from failure detection to resolution (target: 10x improvement over manual). (2) Toil reduction: hours saved per month (count manual runbook executions replaced by automated). (3) Consistency: variance in remediation outcomes (automated should be more consistent than manual). (4) Coverage: % of common failure modes with an auto-executable runbook.

**Q5: What's the training plan for operators?**
A: (1) Runbook authoring workshop (2 hours). (2) YAML reference documentation with examples. (3) Sandbox environment for testing runbooks. (4) Office hours with the platform team (weekly). (5) "Runbook of the Week" Slack channel — showcase well-written runbooks as examples.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Choice | Rationale | Risk | Mitigation |
|---|---|---|---|---|---|
| Runbook format | Custom DSL vs YAML vs Python | YAML | Declarative, readable, no code execution risk | Less flexible than code | Plugin system handles complex actions |
| Execution engine | Custom vs Temporal vs Argo | Custom DAG-based | Simpler, fewer dependencies, tailored to our needs | Must handle crash recovery ourselves | Persistent state + crash recovery logic |
| Runbook storage | MySQL vs Git | Git (source) + MySQL (metadata) | Version control is essential for runbooks | Two stores to keep in sync | Webhook on Git push updates MySQL |
| Rollback approach | Per-step vs checkpoint | Per-step | Practical for infrastructure operations | Not all actions are reversible | Document irreversible actions; alert on rollback failure |
| ML recommendation | Classification only vs RAG | RAG with classification pre-filter | Handles novel patterns and runbook additions | LLM latency (~3-5s) | Acceptable for trigger → recommendation flow |
| Approval workflow | In-system only vs Slack/PD integration | Slack + PagerDuty integration | Meet operators where they are (Slack) | Slack/PD outage blocks approvals | Fallback to web UI approval |
| Action execution | Direct API calls vs agent-based | Plugin-based direct calls | Simpler architecture, fewer components | Plugin must handle all protocols | Plugin abstraction layer |
| Blast radius | Per-execution vs global | Both (per-execution + global limits) | Defense in depth | Complex configuration | Sensible defaults with override capability |

---

## 14. Agentic AI Integration

### AI Agent as Runbook Author

**Problem:** Writing good runbooks is hard. It requires deep operational knowledge about failure modes, diagnostic steps, and remediation actions. Most teams write minimal runbooks that miss edge cases.

**Solution:** An AI agent that analyzes incident history and generates draft runbooks.

```python
class AIRunbookAuthor:
    def draft_runbook(self, incident: Incident) -> str:
        """Generate a draft runbook from a past incident's resolution."""
        prompt = f"""
        You are an infrastructure SRE writing an auto-remediation runbook.
        
        Given this past incident:
        - Summary: {incident.summary}
        - Root cause: {incident.root_cause}
        - Resolution steps: {incident.resolution_steps}
        - Time to resolve: {incident.ttr_minutes} minutes
        - Services affected: {incident.affected_services}
        
        And existing runbooks in this category:
        {self.get_similar_runbooks(incident.category)}
        
        Generate a YAML runbook that automates this resolution. Include:
        1. Diagnostic steps (what to check before acting)
        2. Decision tree (different paths based on diagnosis)
        3. Remediation actions with preconditions and postconditions
        4. Rollback actions for each step
        5. Blast radius limits
        6. Approval gates for high-impact steps
        
        Use the standard runbook YAML format with plugins: kubernetes, ssh, ipmi, mysql, prometheus.
        Be conservative: prefer safer actions and require approval for destructive ones.
        """
        yaml_content = self.llm.invoke(prompt, temperature=0.2)
        return yaml_content
```

### AI Agent as Runbook Executor Assistant

**Problem:** During execution, unexpected situations arise that the runbook didn't anticipate. An operator needs to decide how to proceed.

**Solution:** An AI assistant that provides contextual advice during execution.

```python
class AIExecutionAssistant:
    def advise_on_failure(self, execution: Execution, failed_step: Step) -> str:
        """When a step fails, provide advice on how to proceed."""
        context = {
            "runbook": execution.runbook,
            "target": execution.target_entities,
            "completed_steps": execution.completed_steps,
            "failed_step": {
                "name": failed_step.name,
                "error": failed_step.error_message,
                "output": failed_step.output
            },
            "current_cluster_state": get_cluster_state(execution.cluster_id),
            "similar_past_executions": self.find_similar_failures(failed_step)
        }
        
        prompt = f"""
        A runbook execution has encountered a failure at step "{failed_step.name}".
        
        Context: {json.dumps(context, indent=2)}
        
        Similar past failures and their resolutions:
        {self.format_similar_failures(context['similar_past_executions'])}
        
        Options:
        1. Retry the step (maybe with different parameters)
        2. Skip the step and continue
        3. Rollback and abort
        4. Take an alternative action
        
        Analyze the failure and recommend the best option with reasoning.
        Consider safety (will retrying cause harm?), the cluster state,
        and what worked in similar past situations.
        """
        return self.llm.invoke(prompt, temperature=0.2)
```

### AI-Driven Runbook Optimization

**Problem:** Runbooks accumulate technical debt — they may have unnecessary steps, overly conservative timeouts, or missing edge cases.

**Solution:** Periodic AI review of runbook performance.

```python
class AIRunbookOptimizer:
    def review(self, runbook_id: str) -> OptimizationReport:
        """Analyze a runbook's execution history and suggest improvements."""
        executions = get_executions(runbook_id, last_90d=True)
        
        analysis = {
            "total_executions": len(executions),
            "success_rate": sum(1 for e in executions if e.status == 'succeeded') / len(executions),
            "avg_duration_sec": avg(e.duration_ms / 1000 for e in executions),
            "common_failure_steps": find_common_failure_steps(executions),
            "timeout_rates_per_step": calculate_timeout_rates(executions),
            "unnecessary_steps": find_steps_always_succeed_fast(executions),
            "missing_edge_cases": find_unexpected_failures(executions)
        }
        
        prompt = f"""
        Review this runbook's performance and suggest improvements:
        
        Runbook: {runbook_id}
        Execution statistics: {json.dumps(analysis, indent=2)}
        
        Current runbook YAML: {get_runbook_yaml(runbook_id)}
        
        Provide:
        1. Steps that can be removed (always succeed, add no value)
        2. Steps that need longer timeouts (frequent timeouts)
        3. Missing diagnostic steps (failures that could be caught earlier)
        4. Missing edge cases (unexpected failures not handled)
        5. Rollback improvements
        6. Overall efficiency recommendations
        """
        return self.llm.invoke(prompt, temperature=0.3)
```

### Guard Rails for AI in Runbook System

| Guard Rail | Implementation |
|---|---|
| AI-generated runbooks require human review | Status: "draft" until approved by team lead |
| AI execution advice is advisory only | Displayed to operator; not automatically applied |
| AI optimization suggestions require review | Generated as PR to Git repo for review |
| AI recommendation confidence threshold | Auto-execute only if confidence > 0.9 |
| AI model monitoring | Track recommendation accuracy weekly |
| Fallback to manual selection | If AI recommendation confidence < 0.5 or AI service unavailable |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Why runbooks as code instead of scripts or wiki pages?**
A: Scripts are executable but unstructured — no preconditions, no postconditions, no decision trees, no blast radius control. Wiki pages are structured but not executable — a human must read and manually execute each step. Runbooks as code combine structure (YAML schema with conditions, gates, rollback) with executability (automated engine) and auditability (version control, execution logging).

**Q2: How do you handle a runbook that needs to interact with multiple infrastructure layers (K8s + bare-metal + MySQL)?**
A: The plugin architecture handles this. A single runbook can use multiple plugins: diagnostic steps use the Prometheus plugin (query metrics), action steps use the Kubernetes plugin (drain node), then the IPMI plugin (power cycle), then the MySQL plugin (check replication). Each plugin has its own auth and connection management.

**Q3: What's the testing strategy for runbooks?**
A: Four levels: (1) Schema validation (YAML is well-formed, required fields present, plugins exist). (2) Dry-run against staging (all preconditions/postconditions evaluate, no actions execute). (3) Full execution in staging (actions execute against a non-production environment). (4) Production canary (execute against 1 target in production before fleet-wide activation). Automated via CI: every runbook change triggers levels 1-3.

**Q4: How do you handle a runbook step that takes much longer than expected?**
A: Per-step timeouts. If a step exceeds its timeout, the engine: (1) Logs the timeout. (2) Checks if the action was partially completed (idempotency check). (3) Decides: retry (if the action is safe to retry) or fail (trigger rollback). The timeout is configurable per step because different actions have different expected durations (pod restart: 30s, PXE reimage: 30 minutes).

**Q5: How do you prevent a runbook from being used maliciously (e.g., draining all nodes)?**
A: Multiple layers: (1) Blast radius limits are enforced by the engine, not the runbook (runbook authors can't override). (2) RBAC controls who can create, execute, and approve runbooks. (3) Plugin permissions are scoped (K8s plugin can't delete namespaces). (4) All executions are audited. (5) High-impact actions require approval. (6) Runbook review process catches dangerous patterns before they reach production.

**Q6: How do you handle runbook versioning and backward compatibility?**
A: Each execution is pinned to a specific runbook version. If a runbook is updated from v3 to v4 while v3 is executing, the execution continues with v3. New executions use v4. Both versions are available in Git for audit. The version history includes a change description and reviewer, so auditors can trace exactly what changed and why.

**Q7: What's the difference between a runbook and a Kubernetes Operator?**
A: A Kubernetes Operator is a long-running controller that continuously reconciles state for a specific application (e.g., MySQL Operator manages MySQL cluster lifecycle). A runbook is a one-shot remediation that runs in response to a specific failure. Operators handle application-level lifecycle; runbooks handle incident-level remediation. They're complementary: an Operator might detect a MySQL primary failure, and a runbook might handle the node-level recovery underneath.

**Q8: How do you handle runbook dependencies (runbook A must run before runbook B)?**
A: The sub-execution feature: runbook A can include a step that triggers runbook B as a sub-execution. The parent waits for the child to complete. For more complex dependencies, the GameDay orchestrator sequences multiple runbooks with checkpoints between them.

**Q9: How do you ensure runbooks stay up-to-date as infrastructure evolves?**
A: (1) Runbook CI: a nightly job executes all production runbooks in dry-run mode against staging. If any fail (schema change, API deprecation, removed endpoint), the owning team is alerted. (2) Infrastructure change process: PRs that change APIs, endpoints, or behavior must include runbook updates. (3) Quarterly review: the outcome learner flags runbooks with degrading success rates.

**Q10: How do you handle a situation where the runbook system itself is down during an incident?**
A: Runbooks are also published as static documentation (generated from YAML) in the wiki. If the automated system is down, operators follow the manual version. The manual version includes the same diagnostic steps and actions — just executed by hand. This is the offline fallback. The automated system is designed for 99.99% availability to minimize this scenario.

**Q11: How would you implement conditional loops in a runbook (retry until success)?**
A: The DAG supports "retry" edges: a step can reference itself with a condition (e.g., retry up to 3 times with 30s delay). The engine tracks the retry count and prevents infinite loops (max 10 retries hardcoded as a safety limit). More complex loops (iterate over a list of nodes) are handled by the "batch" step type that iterates with configurable concurrency.

**Q12: How do you handle runbook execution across multiple clusters?**
A: The runbook defines a target scope (e.g., `target_type: bare_metal, cluster: prod-*`). The execution engine resolves this to specific entities across clusters. Each cluster's actions are dispatched to the appropriate cluster's API. Blast radius limits apply per-cluster and globally.

**Q13: What about runbook templates and inheritance?**
A: A runbook can extend a template: `extends: template-host-recovery-base`. The template defines common diagnostic steps and rollback patterns. The child runbook overrides or adds specific steps. This reduces duplication — 50 host-recovery runbooks might share 80% of their steps from a base template.

**Q14: How do you handle secrets in runbook parameters (e.g., database passwords)?**
A: Runbook parameters NEVER contain secrets. The plugins retrieve secrets at execution time from Vault. For example, the MySQL plugin has a Vault path for each MySQL cluster's credentials. The runbook just specifies `target: mysql-orders-prod`, and the plugin fetches the credentials transparently.

**Q15: How do you measure the quality of ML-based runbook selection?**
A: Offline: replay historical failures through the recommender and compare with actual human selections (precision@1, precision@5). Online: track (1) how often the operator accepts vs overrides the recommendation, (2) success rate of auto-selected runbooks vs human-selected, (3) time-to-resolution with vs without AI recommendation.

**Q16: What's the most impactful improvement you would make to this system?**
A: Proactive runbook execution: instead of waiting for failures to trigger runbooks, use predictive models to detect pre-failure conditions and execute preventive runbooks. Example: "SMART data shows disk will fail within 48 hours" → auto-execute the pre-failure drain-and-replace runbook before the disk actually fails. This eliminates downtime entirely for predictable failure modes.

---

## 16. References

1. Google SRE Book — Chapter 14: Managing Incidents, Chapter 15: Postmortem Culture: https://sre.google/sre-book/table-of-contents/
2. PagerDuty Incident Response Documentation: https://response.pagerduty.com/
3. Rundeck (Runbook Automation): https://www.rundeck.com/
4. StackStorm (Event-Driven Automation): https://stackstorm.com/
5. Argo Workflows (Kubernetes-native workflow engine): https://argoproj.github.io/argo-workflows/
6. Temporal (Distributed workflow engine): https://temporal.io/
7. Ansible (Infrastructure automation): https://docs.ansible.com/
8. Netflix Dispatch (Crisis management): https://github.com/Netflix/dispatch
9. ChromaDB (Vector database for RAG): https://www.trychroma.com/
10. XGBoost (Gradient boosting for classification): https://xgboost.readthedocs.io/
11. Jinja2 Templating (for variable interpolation): https://jinja.palletsprojects.com/
12. HashiCorp Vault (Secret management): https://www.vaultproject.io/
