# System Design: Self-Service Developer Portal

> **Relevance to role:** A cloud infrastructure platform engineer builds the self-service layer that thousands of engineers interact with daily to provision infrastructure, manage resources, track costs, and execute approval workflows -- this portal is the primary user-facing surface of the entire IaaS platform.

---

## 1. Requirement Clarifications

### Functional Requirements
- **Web portal** for visual infrastructure management: browse machine catalog, create reservations, view dashboards, manage projects.
- **CLI tool** (`infra-cli`) for scriptable infrastructure operations, CI/CD integration, and power-user workflows.
- **Template-based provisioning** (Infrastructure-as-Code): users define infrastructure in declarative YAML/HCL templates (Terraform-compatible), stored in Git, versioned, peer-reviewed.
- **Workflow engine** for multi-step provisioning: approval chains, dependency resolution, parallel step execution, rollback on failure.
- **Approval workflows** for expensive resources: GPU cluster requests (>$10K/month) require manager approval, >$50K require VP approval.
- **Cost visibility**: real-time cost dashboard per team/project, budget alerts, cost forecasts based on current usage trends.
- **Budget enforcement**: hard and soft limits per team/project. Soft limit triggers warning; hard limit blocks new provisioning.
- **SSO integration**: SAML/OIDC via corporate identity provider. No separate credentials.
- **RBAC**: roles mapped from corporate groups. Viewer, Developer, Operator, Admin.
- **Resource catalog**: browsable catalog of machine types, OS images, network configs, pre-built templates.
- **Environment management**: dev/staging/prod environments with different approval thresholds and cost limits.
- **Notification engine**: Slack, email, PagerDuty integration for workflow status, approvals, cost alerts, expiring reservations.
- **API key management**: create/rotate/revoke API keys for service accounts.
- **Audit dashboard**: searchable history of all provisioning actions, approvals, cost events.

### Non-Functional Requirements
- **Availability target**: Portal: 99.9% (8.7 hours downtime/year -- acceptable for UI). API/CLI: 99.99%.
- **Consistency model**: Eventual consistency for dashboard data (< 5s lag). Strong consistency for approval workflow state and budget enforcement.
- **Latency target**: Portal page load: p50 < 1s, p99 < 3s. CLI command execution: p50 < 500ms, p99 < 2s. Template apply: depends on size (minutes for large templates).
- **Durability**: Workflow state and approval records: RPO = 0. Dashboard data: reconstructable from source systems.
- **Scalability**: 5,000 concurrent portal users, 10,000 CLI sessions, 1,000 template applies/day.

### Constraints & Assumptions
- The portal is a frontend to the IaaS platform (iaas_platform_design.md) and Bare-Metal Reservation Platform (bare_metal_reservation_platform.md). It does not own resource state -- it delegates to those systems.
- Corporate SSO is Okta (OIDC); group membership drives RBAC.
- Terraform is the primary IaC tool; we provide a custom Terraform provider.
- Cost data flows from the billing/metering pipeline (Kafka events).
- Engineers are familiar with Git-based workflows; templates stored in Git repos.

### Out of Scope
- IaaS resource lifecycle management (owned by IaaS platform)
- Bare-metal reservation logic (owned by reservation platform)
- Billing calculation and invoicing (owned by finance systems)
- CI/CD pipeline infrastructure (Jenkins/GitHub Actions -- separate system)

---

## 2. Scale & Capacity Estimates

### Users & Traffic
| Metric | Value | Calculation |
|--------|-------|-------------|
| Total registered users | 5,000 | All infra engineers |
| Concurrent portal sessions | 500 | ~10% of users active at any time |
| CLI sessions (active) | 1,000 | Service accounts + interactive users |
| Portal page views/sec | 200 | 500 users x ~0.4 page/sec |
| API calls/sec (portal + CLI) | 500 | Read-heavy: dashboard data, status checks |
| Template applies/day | 1,000 | ~100 teams x ~10 applies/day |
| Approval requests/day | 200 | ~20% of applies require approval |

### Latency Requirements
| Operation | Target | Justification |
|-----------|--------|---------------|
| Dashboard page render (server-side) | p50 < 200ms, p99 < 800ms | Aggregate from multiple backend services |
| Resource catalog browse | p50 < 100ms, p99 < 500ms | Cached, read-heavy |
| Template validate (dry-run) | p50 < 2s, p99 < 10s | Depends on template complexity |
| Template apply (submit) | p50 < 500ms response (async execution) | Synchronous acceptance, async workflow |
| Approval submit | p50 < 100ms | Simple DB write |
| Cost dashboard load | p50 < 500ms, p99 < 2s | Aggregation query against pre-computed data |

### Storage Estimates
| Data type | Size/record | Volume/day | Retention | Total |
|-----------|-------------|------------|-----------|-------|
| Workflow instances | ~10 KB | 1,000/day | Forever | ~3.6 GB/year |
| Approval records | ~2 KB | 200/day | Forever | ~146 MB/year |
| Template versions | ~5 KB | 5,000/day (including Git-synced) | Forever | ~9.1 GB/year |
| Cost aggregates | ~200 B | 50K/day (per-team per-hour) | 2 years | ~7.3 GB/year |
| Portal session data | ~1 KB | 5,000/day | 24 hours | ~5 MB (in Redis) |
| Notification records | ~500 B | 10,000/day | 90 days | ~450 MB/year |

### Bandwidth Estimates
| Direction | Calculation | Result |
|-----------|-------------|--------|
| Portal API inbound | 200 req/sec x 2 KB = 400 KB/s | ~3 Mbps |
| Portal API outbound | 200 req/sec x 10 KB avg page data = 2 MB/s | ~16 Mbps |
| CLI API | 300 req/sec x 1 KB avg = 300 KB/s | ~2 Mbps |
| Template storage | 5,000 versions/day x 5 KB = 25 MB/day | Negligible |
| Cost data pipeline | 50K events/day x 200 B = 10 MB/day | Negligible |

---

## 3. High Level Architecture

```
     +---------------------+         +---------------------+
     |   Web Portal        |         |   CLI (infra-cli)   |
     |   (React SPA)       |         |   (Go binary)       |
     +----------+----------+         +----------+----------+
                |                               |
                v                               v
     +----------+-------------------------------+----------+
     |              Portal API Gateway                     |
     |  (Kong / Envoy: Auth, Rate Limit, SSO redirect)    |
     +---+-------+-------+-------+-------+-------+--------+
         |       |       |       |       |       |
         v       v       v       v       v       v
     +---+--+ +--+--+ +-+---+ +-+---+ +-+---+ +-+--------+
     |Portal| |Work | |Cost | |Cata | |Notif| |Template  |
     |BFF   | |flow | |Svc  | |log  | |Svc  | |Engine    |
     |Svc   | |Eng. | |     | |Svc  | |     | |(Terraform|
     |      | |     | |     | |     | |     | | wrapper) |
     +--+---+ +--+--+ +--+--+ +--+--+ +--+--+ +--+------+
        |        |        |       |       |        |
        +--------+--------+------+--------+--------+
                 |               |
      +----------v----------+   +v---------+
      |  IaaS Platform      |   | Billing  |
      |  APIs (Compute,     |   | Pipeline |
      |  Network, Storage,  |   | (Kafka)  |
      |  Reservation)       |   +----------+
      +---------------------+
                                                    
     +---------------------------------------------------------------+
     |                  Data Stores                                   |
     | +--------+  +--------+  +--------+  +---------+  +--------+  |
     | | MySQL  |  | Redis  |  | Kafka  |  | Elastic |  | S3/GCS |  |
     | |(workflow| |(session,| |(events,|  | search  |  |(template|  |
     | | state, |  | cache) |  | cost)  |  | (audit) |  | store) |  |
     | | approvals|        |  |        |  |         |  |        |  |
     | +--------+  +--------+  +--------+  +---------+  +--------+  |
     +---------------------------------------------------------------+
```

### Component Roles

**Web Portal (React SPA):** Single-page application with responsive UI. Authenticates via OIDC redirect to corporate SSO. Renders dashboards, resource catalog, workflow status, cost charts, approval forms. Hosted on CDN for static assets.

**CLI (infra-cli, Go binary):** Compiled Go binary distributed via internal package manager. Authenticates via device authorization flow or API key. Mirrors all portal functionality as commands. Outputs in table, JSON, or YAML format. Supports `--dry-run` for template validation without execution.

**Portal BFF (Backend-for-Frontend, Java/Spring Boot):** Aggregates data from multiple backend services into portal-optimized payloads. Handles SSO callback, session management, WebSocket connections for real-time updates. Reduces portal-to-backend chattiness.

**Workflow Engine (Java/Spring Boot):** Executes multi-step provisioning workflows defined by templates. Manages workflow state (DAG of steps with dependencies), approval gates, parallel execution, rollback. Backed by MySQL for durability.

**Cost Service (Java/Spring Boot):** Consumes metering events from Kafka, aggregates into per-team/per-project/per-resource cost records. Provides budget tracking, cost forecasts, and cost anomaly detection. Writes pre-computed aggregates to MySQL for fast dashboard queries.

**Catalog Service (Java):** Manages the resource catalog: machine types, OS images, network configurations, pre-built templates. Provides search and filtering. Backed by MySQL + Elasticsearch for full-text search.

**Notification Service (Python):** Consumes workflow events, approval requests, cost alerts, and expiring reservation notices. Delivers via Slack (webhook), email (SMTP/SES), PagerDuty (API). Tracks delivery status.

**Template Engine (Python, wraps Terraform):** Parses infrastructure templates (YAML/HCL), validates against the resource catalog and quotas, executes Terraform plan/apply against the IaaS platform's APIs via a custom Terraform provider. Manages state files in S3/GCS.

### Primary Data Flow: Template-Based Provisioning

1. User writes infrastructure template (HCL/YAML):
   ```hcl
   resource "infra_reservation" "training" {
     machine_type = "gpu_h100_8x"
     count        = 8
     region       = "us-east-1"
     start_time   = "2026-04-10T09:00:00Z"
     duration     = "8h"
     os_image     = "ubuntu-22.04-cuda"
     network      = infra_network.training_net.id
   }
   
   resource "infra_network" "training_net" {
     cidr = "10.0.0.0/24"
     name = "ml-training-network"
   }
   ```

2. User commits template to Git, opens PR for peer review.

3. CI pipeline runs `infra-cli template validate --file main.tf` (dry-run: checks syntax, schema, quota, cost estimate).

4. After PR merge, user runs `infra-cli template apply --file main.tf` or clicks "Apply" in the portal.

5. Template Engine parses the template, resolves dependencies (network must be created before reservation).

6. Workflow Engine creates a workflow instance with steps:
   - Step 1: Create network (calls Network Service API)
   - Step 2: Create reservation (calls Reservation Service API, depends on Step 1)
   - Approval gate: If estimated cost > $10K, block workflow and request manager approval.

7. If approval required, Notification Service sends Slack message to manager with approval link.

8. Manager clicks "Approve" in portal or Slack. Approval recorded in MySQL.

9. Workflow Engine resumes: executes Step 2 (create reservation).

10. On success, workflow state = `completed`. User notified via Slack.

11. On failure at any step, Workflow Engine executes rollback steps in reverse (delete network if reservation failed). User notified with error details.

### Secondary Data Flow: Cost Dashboard

1. IaaS platform emits metering events to Kafka: `instance.active`, `instance.deleted`, `volume.created`, etc.
2. Cost Service consumes events, joins with pricing data (cost per machine-type per hour).
3. Cost Service aggregates into hourly buckets: `{team, project, resource_type, hour, cost}`.
4. Aggregates written to MySQL `cost_aggregates` table.
5. Portal BFF queries Cost Service API: "Team ML's cost for April 2026."
6. Cost Service returns pre-computed aggregate: fast query against indexed table.
7. Portal renders cost chart.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Workflow instance
CREATE TABLE workflows (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    workflow_uuid       CHAR(36) NOT NULL,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    created_by          BIGINT UNSIGNED NOT NULL,
    template_id         BIGINT UNSIGNED NULL,           -- NULL for ad-hoc
    name                VARCHAR(255) NOT NULL,
    status              ENUM('pending_approval','approved','running','paused',
                             'completed','failed','cancelled','rolling_back')
                        NOT NULL DEFAULT 'pending_approval',
    total_steps         SMALLINT UNSIGNED NOT NULL,
    completed_steps     SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    estimated_cost_usd  DECIMAL(12,2) NULL,
    environment         ENUM('dev','staging','prod') NOT NULL DEFAULT 'dev',
    version             INT UNSIGNED NOT NULL DEFAULT 1,
    started_at          DATETIME NULL,
    completed_at        DATETIME NULL,
    error_message       TEXT NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_uuid (workflow_uuid),
    INDEX idx_tenant_status (tenant_id, status),
    INDEX idx_created_by (created_by, status),
    INDEX idx_status_created (status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Workflow step
CREATE TABLE workflow_steps (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    workflow_id         BIGINT UNSIGNED NOT NULL,
    step_order          SMALLINT UNSIGNED NOT NULL,
    name                VARCHAR(255) NOT NULL,
    step_type           ENUM('provision','approval','notification','wait',
                             'script','terraform_apply') NOT NULL,
    status              ENUM('pending','waiting_dependency','running','completed',
                             'failed','skipped','rolled_back') NOT NULL DEFAULT 'pending',
    config              JSON NOT NULL,                  -- Step-specific configuration
    depends_on          JSON NULL,                      -- Array of step IDs
    resource_id         VARCHAR(255) NULL,              -- Created resource ID (after completion)
    rollback_config     JSON NULL,                      -- How to undo this step
    started_at          DATETIME NULL,
    completed_at        DATETIME NULL,
    error_message       TEXT NULL,
    
    INDEX idx_workflow_order (workflow_id, step_order),
    INDEX idx_status (status),
    FOREIGN KEY (workflow_id) REFERENCES workflows(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Approval records
CREATE TABLE approvals (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    approval_uuid       CHAR(36) NOT NULL,
    workflow_id         BIGINT UNSIGNED NOT NULL,
    step_id             BIGINT UNSIGNED NULL,           -- NULL if workflow-level approval
    approver_id         BIGINT UNSIGNED NULL,           -- NULL until acted on
    required_role       VARCHAR(64) NOT NULL,            -- e.g., 'manager', 'vp'
    required_group      VARCHAR(128) NULL,               -- e.g., 'ml-team-leads'
    status              ENUM('pending','approved','rejected','expired','auto_approved')
                        NOT NULL DEFAULT 'pending',
    reason              TEXT NULL,
    estimated_cost_usd  DECIMAL(12,2) NULL,
    expires_at          DATETIME NOT NULL,               -- Auto-expire after N days
    decided_at          DATETIME NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_uuid (approval_uuid),
    INDEX idx_workflow (workflow_id),
    INDEX idx_approver (approver_id, status),
    INDEX idx_pending_group (required_group, status, expires_at),
    FOREIGN KEY (workflow_id) REFERENCES workflows(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Templates
CREATE TABLE templates (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    template_uuid       CHAR(36) NOT NULL,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    name                VARCHAR(255) NOT NULL,
    description         TEXT NULL,
    template_type       ENUM('terraform','yaml','blueprint') NOT NULL,
    version             VARCHAR(32) NOT NULL,            -- Semver or Git SHA
    content_hash        CHAR(64) NOT NULL,               -- SHA-256 of template content
    content_url         VARCHAR(512) NOT NULL,            -- S3/GCS URL to template file
    parameters_schema   JSON NULL,                        -- JSON Schema for input params
    estimated_cost_usd  DECIMAL(12,2) NULL,
    is_public           BOOLEAN NOT NULL DEFAULT FALSE,
    created_by          BIGINT UNSIGNED NOT NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_uuid (template_uuid),
    INDEX idx_tenant_name (tenant_id, name),
    INDEX idx_public (is_public, name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Cost aggregates (pre-computed for dashboard performance)
CREATE TABLE cost_aggregates (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    team_id             BIGINT UNSIGNED NULL,
    project_id          BIGINT UNSIGNED NULL,
    resource_type       VARCHAR(32) NOT NULL,
    machine_type        VARCHAR(64) NULL,
    hour_bucket         DATETIME NOT NULL,               -- Truncated to hour
    cost_usd            DECIMAL(12,4) NOT NULL,
    quantity            INT UNSIGNED NOT NULL,
    
    UNIQUE KEY uk_bucket (tenant_id, team_id, project_id, resource_type, hour_bucket),
    INDEX idx_tenant_time (tenant_id, hour_bucket),
    INDEX idx_team_time (team_id, hour_bucket)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (TO_DAYS(hour_bucket)) (
    PARTITION p202604 VALUES LESS THAN (TO_DAYS('2026-05-01')),
    PARTITION p202605 VALUES LESS THAN (TO_DAYS('2026-06-01')),
    PARTITION p_future VALUES LESS THAN MAXVALUE
);

-- Budget configurations
CREATE TABLE budgets (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    team_id             BIGINT UNSIGNED NULL,
    project_id          BIGINT UNSIGNED NULL,
    budget_type         ENUM('monthly','quarterly','annual') NOT NULL,
    soft_limit_usd      DECIMAL(12,2) NOT NULL,          -- Warning threshold
    hard_limit_usd      DECIMAL(12,2) NOT NULL,          -- Block threshold
    current_spend_usd   DECIMAL(12,2) NOT NULL DEFAULT 0,
    period_start        DATE NOT NULL,
    period_end          DATE NOT NULL,
    alert_sent          BOOLEAN NOT NULL DEFAULT FALSE,
    
    INDEX idx_tenant_period (tenant_id, period_start, period_end),
    INDEX idx_team_period (team_id, period_start, period_end)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Notification records
CREATE TABLE notifications (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    recipient_id        BIGINT UNSIGNED NOT NULL,
    channel             ENUM('slack','email','pagerduty','webhook') NOT NULL,
    event_type          VARCHAR(64) NOT NULL,
    subject             VARCHAR(255) NOT NULL,
    body                TEXT NOT NULL,
    status              ENUM('pending','sent','delivered','failed') NOT NULL DEFAULT 'pending',
    sent_at             DATETIME NULL,
    error_message       TEXT NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    INDEX idx_recipient (recipient_id, created_at),
    INDEX idx_status (status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- API keys for service accounts
CREATE TABLE api_keys (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    key_id              VARCHAR(32) NOT NULL,             -- Public identifier (e.g., 'ik_abc123')
    key_hash            CHAR(64) NOT NULL,                -- SHA-256 hash of the secret key
    tenant_id           BIGINT UNSIGNED NOT NULL,
    created_by          BIGINT UNSIGNED NOT NULL,
    name                VARCHAR(255) NOT NULL,
    scopes              JSON NOT NULL,                    -- ["read:reservations","write:reservations"]
    expires_at          DATETIME NULL,
    last_used_at        DATETIME NULL,
    is_active           BOOLEAN NOT NULL DEFAULT TRUE,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_key_id (key_id),
    INDEX idx_tenant (tenant_id, is_active)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

**Selected: MySQL 8.0** for workflow state, approvals, templates, budgets (transactional, ACID needed for approval state). **Redis** for session data, cached dashboard aggregates, rate limiting. **Elasticsearch** for full-text search over templates, audit logs, and resource catalog. **S3/GCS** for template file storage (versioned, durable). **Kafka** for cost event ingestion and notification delivery queue.

### Indexing Strategy

- **`idx_pending_group`** on approvals: Powers the "pending approvals for my team" query (the approver's inbox). Composite index on (required_group, status, expires_at) enables efficient filtering.
- **`idx_tenant_time`** on cost_aggregates: Powers the cost dashboard. Partitioned by month for efficient range queries.
- **`idx_workflow_order`** on workflow_steps: Powers the workflow execution engine's "next step to run" query.
- **`idx_status_created`** on workflows: Powers the admin dashboard "all running workflows" view.

---

## 5. API Design

```
# === Workflows ===

POST   /api/v1/workflows
  Description: Create and start a provisioning workflow
  Request: {
    "name": "ML Training Infrastructure",
    "template_id": "tmpl-abc123",
    "parameters": {"machine_type": "gpu_h100_8x", "count": 8, ...},
    "environment": "prod",
    "dry_run": false
  }
  Response: 202 Accepted { "workflow_id": "wf-xyz789", "status": "pending_approval" }

GET    /api/v1/workflows/{workflow_id}
  Response: 200 OK { workflow details + step statuses }

GET    /api/v1/workflows
  Query: ?status=running&created_by=me&page=1&per_page=20
  Response: 200 OK { paginated list }

DELETE /api/v1/workflows/{workflow_id}
  Description: Cancel a running workflow (triggers rollback)
  Response: 200 OK { "status": "rolling_back" }

GET    /api/v1/workflows/{workflow_id}/steps
  Response: 200 OK { list of steps with status, timing, error details }

POST   /api/v1/workflows/{workflow_id}/steps/{step_id}/retry
  Description: Retry a failed step
  Response: 200 OK

# === Templates ===

POST   /api/v1/templates
  Description: Upload a template
  Request: multipart/form-data { name, description, type, file }
  Response: 201 Created

GET    /api/v1/templates
  Query: ?search=gpu+training&type=terraform&is_public=true
  Response: 200 OK { paginated list }

POST   /api/v1/templates/{template_id}/validate
  Description: Validate a template (dry-run)
  Request: { "parameters": {...} }
  Response: 200 OK { "valid": true, "estimated_cost": 1250.00, "resources_planned": [...] }

# === Approvals ===

GET    /api/v1/approvals
  Query: ?status=pending&approver_group=ml-team-leads
  Response: 200 OK { list of pending approvals }

POST   /api/v1/approvals/{approval_id}/approve
  Request: { "reason": "Approved for Q2 training budget" }
  Response: 200 OK

POST   /api/v1/approvals/{approval_id}/reject
  Request: { "reason": "Exceeds quarterly budget. Reduce count to 4." }
  Response: 200 OK

# === Cost ===

GET    /api/v1/cost/summary
  Query: ?team_id=42&from=2026-04-01&to=2026-04-30&granularity=daily
  Response: 200 OK {
    "total_cost": 45000.00,
    "breakdown": [
      {"date": "2026-04-01", "cost": 1500.00, "resources": {...}},
      ...
    ],
    "budget": {"limit": 50000.00, "used_pct": 90.0},
    "forecast": {"end_of_month": 52000.00, "over_budget": true}
  }

GET    /api/v1/cost/by-resource
  Query: ?team_id=42&from=2026-04-01&to=2026-04-30
  Response: 200 OK { breakdown by resource type: compute, storage, network, gpu }

POST   /api/v1/budgets
  Description: Create or update budget for a team/project
  Request: { "team_id": 42, "budget_type": "monthly", "soft_limit": 40000, "hard_limit": 50000 }

# === Catalog ===

GET    /api/v1/catalog/machine-types
  Query: ?gpu_type=H100&min_gpu_count=8
  Response: 200 OK { list of machine types with specs and pricing }

GET    /api/v1/catalog/images
  Query: ?os=ubuntu&gpu_support=true
  Response: 200 OK { list of OS images }

GET    /api/v1/catalog/templates
  Query: ?category=ml-training&search=distributed
  Response: 200 OK { list of published templates }

# === API Keys ===

POST   /api/v1/api-keys
  Request: { "name": "CI/CD pipeline", "scopes": ["read:*","write:reservations"], "expires_in_days": 90 }
  Response: 201 Created { "key_id": "ik_abc123", "secret": "sk_..." }  // Secret shown ONCE

DELETE /api/v1/api-keys/{key_id}

GET    /api/v1/api-keys
  Response: 200 OK { list of keys (no secrets) with last_used_at }

# === Notifications ===

GET    /api/v1/notifications
  Query: ?unread=true
  Response: 200 OK { list of notifications }

PUT    /api/v1/notifications/preferences
  Request: { "slack": {"enabled": true, "channel": "#my-infra"},
             "email": {"enabled": true},
             "events": ["approval_required","workflow_completed","cost_alert"] }
```

### CLI Design

```bash
# Template-based provisioning
infra-cli template validate --file main.tf --var machine_count=8
infra-cli template apply --file main.tf --var machine_count=8 --env prod
infra-cli template destroy --file main.tf

# Workflow management
infra-cli workflow list --status running
infra-cli workflow show wf-xyz789
infra-cli workflow cancel wf-xyz789
infra-cli workflow retry wf-xyz789 --step 3

# Approvals
infra-cli approvals list --pending
infra-cli approvals approve apr-abc123 --reason "Approved for Q2 budget"
infra-cli approvals reject apr-abc123 --reason "Reduce GPU count"

# Cost
infra-cli cost summary --team ml-training --month 2026-04
infra-cli cost forecast --team ml-training
infra-cli cost by-resource --team ml-training --month 2026-04 --output table

# Catalog
infra-cli catalog machines --gpu-type H100
infra-cli catalog images --os ubuntu --gpu-support
infra-cli catalog templates --category ml-training

# API keys
infra-cli api-keys create --name "CI pipeline" --scopes "read:*,write:reservations" --expires 90d
infra-cli api-keys list
infra-cli api-keys revoke ik_abc123

# Quick reservation (bypasses template for ad-hoc use)
infra-cli reserve --type gpu_h100_8x --count 4 --region us-east-1 --duration 8h

# Real-time status
infra-cli workflow watch wf-xyz789  # Streaming status updates
```

---

## 6. Core Component Deep Dives

### Component: Workflow Engine

**Why it's hard:** A workflow represents a DAG of provisioning steps with dependencies, approval gates, conditional branches, and rollback on failure. The engine must execute steps in parallel when possible, pause for approvals, resume on approval, and correctly roll back partially completed workflows -- all while being durable (survive service restarts) and observable (show exact state in the portal).

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Sequential script** | Run steps in order | Simple | No parallelism, no persistence, no approval gates | Prototypes |
| **Airflow** | DAG-based workflow scheduler | Mature, good UI | Designed for data pipelines, not interactive workflows; no built-in approval concept | Batch ETL |
| **Temporal.io** | Durable workflow engine | Durable execution, replay, visibility | Requires Temporal cluster operation | Complex long-running workflows |
| **Custom engine (DB-backed state machine)** | Store workflow DAG and step states in MySQL, engine polls and executes | Full control, simple infra, exactly fits our model | Must build visibility, retry, rollback manually | **Our use case: moderate complexity, approval-centric** |
| **Step Functions (AWS)** | Managed workflow service | Zero operational overhead | Vendor lock-in, limited customization | AWS-native architectures |

**Selected Approach:** Custom workflow engine backed by MySQL, with Kafka for async step execution. Temporal considered for v2 if workflow complexity grows significantly.

**Justification:** Our workflow complexity is moderate: typically 3-10 steps with 0-2 approval gates. The critical requirement is approval workflow integration (pause, notify, resume on approval), which is trivial with a DB-backed state machine but requires custom Temporal activities. At 1,000 workflows/day, MySQL easily handles the state management. The custom engine gives us full control over the approval UX (Slack buttons, portal approve/reject, CLI) without adapting to a framework's model.

**Implementation Detail:**

```java
@Service
public class WorkflowEngine {
    
    @Scheduled(fixedRate = 1000)  // Poll every second
    public void processWorkflows() {
        // Find workflows that have runnable steps
        List<Workflow> active = workflowRepo.findByStatusIn(
            List.of(WorkflowStatus.RUNNING, WorkflowStatus.APPROVED));
        
        for (Workflow workflow : active) {
            processWorkflow(workflow);
        }
    }
    
    private void processWorkflow(Workflow workflow) {
        List<WorkflowStep> steps = stepRepo.findByWorkflowId(workflow.getId());
        
        for (WorkflowStep step : steps) {
            if (step.getStatus() == StepStatus.PENDING && areDependenciesMet(step, steps)) {
                // Check if this is an approval step
                if (step.getStepType() == StepType.APPROVAL) {
                    createApprovalRequest(workflow, step);
                    step.setStatus(StepStatus.WAITING_DEPENDENCY);
                    stepRepo.save(step);
                    continue;
                }
                
                // Execute step asynchronously via Kafka
                step.setStatus(StepStatus.RUNNING);
                step.setStartedAt(Instant.now());
                stepRepo.save(step);
                
                kafkaTemplate.send("workflow.step.execute", 
                    StepExecutionRequest.of(workflow, step));
            }
        }
        
        // Check if all steps completed
        if (steps.stream().allMatch(s -> s.getStatus() == StepStatus.COMPLETED)) {
            workflow.setStatus(WorkflowStatus.COMPLETED);
            workflow.setCompletedAt(Instant.now());
            workflowRepo.save(workflow);
            notifyWorkflowComplete(workflow);
        }
        
        // Check if any step failed
        if (steps.stream().anyMatch(s -> s.getStatus() == StepStatus.FAILED)) {
            initiateRollback(workflow, steps);
        }
    }
    
    private boolean areDependenciesMet(WorkflowStep step, List<WorkflowStep> allSteps) {
        if (step.getDependsOn() == null) return true;
        List<Long> depIds = step.getDependsOnIds();
        return allSteps.stream()
            .filter(s -> depIds.contains(s.getId()))
            .allMatch(s -> s.getStatus() == StepStatus.COMPLETED);
    }
    
    private void initiateRollback(Workflow workflow, List<WorkflowStep> steps) {
        workflow.setStatus(WorkflowStatus.ROLLING_BACK);
        workflowRepo.save(workflow);
        
        // Rollback completed steps in reverse order
        List<WorkflowStep> completedSteps = steps.stream()
            .filter(s -> s.getStatus() == StepStatus.COMPLETED)
            .sorted(Comparator.comparingInt(WorkflowStep::getStepOrder).reversed())
            .collect(toList());
        
        for (WorkflowStep step : completedSteps) {
            if (step.getRollbackConfig() != null) {
                kafkaTemplate.send("workflow.step.rollback",
                    StepRollbackRequest.of(workflow, step));
            }
        }
    }
}

// Step executor (Kafka consumer)
@KafkaListener(topics = "workflow.step.execute")
public void executeStep(StepExecutionRequest request) {
    WorkflowStep step = stepRepo.findById(request.getStepId());
    
    try {
        switch (step.getStepType()) {
            case PROVISION:
                // Call IaaS API to create resource
                String resourceId = iaasClient.createResource(step.getConfig());
                step.setResourceId(resourceId);
                step.setRollbackConfig(Map.of(
                    "action", "delete",
                    "resource_type", step.getConfig().get("resource_type"),
                    "resource_id", resourceId
                ));
                break;
            case TERRAFORM_APPLY:
                // Run terraform apply
                TerraformResult result = terraformRunner.apply(
                    step.getConfig().get("template_url"),
                    step.getConfig().get("variables"),
                    step.getConfig().get("state_key"));
                step.setResourceId(result.getOutputs().toString());
                break;
            case SCRIPT:
                // Run custom script
                scriptRunner.execute(step.getConfig());
                break;
        }
        
        step.setStatus(StepStatus.COMPLETED);
        step.setCompletedAt(Instant.now());
    } catch (Exception e) {
        step.setStatus(StepStatus.FAILED);
        step.setErrorMessage(e.getMessage());
    }
    
    stepRepo.save(step);
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Engine crashes mid-workflow | Workflow stuck in RUNNING | Engine recovers on restart: re-scans active workflows, re-processes from current step states. Steps already COMPLETED are skipped. |
| Step execution fails | Workflow fails | Retry logic: configurable max retries per step (default: 3). After max retries, workflow enters FAILED state and triggers rollback. |
| Approval timeout | Workflow stuck waiting | Approval expires after configurable period (default: 72 hours). Expired approvals auto-reject; workflow is cancelled with notification. |
| Rollback step fails | Resources leaked | Rollback failures are logged and alerted. Resource leak detector runs daily, identifies orphaned resources, notifies owners. |
| Kafka unavailable | Step execution commands lost | Transactional outbox pattern: step commands written to MySQL outbox table, relay publishes to Kafka. If Kafka is down, commands queue in outbox. |

**Interviewer Deep-Dive Q&A:**

Q: How do you handle workflows that take days (waiting for approval)?
A: Workflow state is persisted in MySQL, not held in memory. The engine polls for workflows with runnable steps every second. A workflow waiting for approval has no runnable steps (the approval step is in `WAITING_DEPENDENCY` status). When the approver clicks "Approve," the approval record updates to `approved`, the approval step transitions to `COMPLETED`, and the next poll cycle detects new runnable steps. There is no long-lived thread or connection for waiting workflows. The engine can handle 10,000 paused workflows without any resource consumption beyond the DB rows.

Q: How do you prevent two engine instances from processing the same workflow?
A: Optimistic locking: the engine reads workflow.version, processes steps, and updates `WHERE version = ?`. If another instance already processed the same workflow, the version mismatch causes the update to affect 0 rows, and the second instance's work is discarded. For step execution commands on Kafka, the consumer group ensures each message is processed by exactly one consumer instance.

Q: How do you handle partial rollback when only some steps have rollback configs?
A: Not all steps have rollback actions (e.g., a notification step has no rollback). The rollback process only executes `rollbackConfig` for steps that have one. Steps without rollback config are skipped in the rollback sequence. The final workflow status after rollback includes a list of "could not roll back" steps, which are flagged for manual cleanup.

Q: How does the approval workflow integrate with Slack?
A: When an approval is created, the Notification Service sends a Slack message with interactive buttons ("Approve" / "Reject") using Slack Block Kit. The button payload includes the approval UUID and a one-time-use token. When the approver clicks a button, Slack sends a webhook to our callback endpoint. The endpoint validates the token, verifies the user's identity against SSO (Slack user ID mapped to corporate ID), checks they have the required role, and records the decision. The Slack message is updated to show the decision and timestamp. This is zero-context-switch approval: the manager does not need to open the portal.

Q: How do you estimate cost before a workflow runs?
A: The `validate` endpoint (dry-run) calls the IaaS platform's pricing API with the requested resources. The pricing API returns hourly cost per resource type. The template engine multiplies by duration and sums across all resources. For open-ended reservations (no end time), we use the budget period's remaining days as the estimate. The estimate is attached to the approval request so the approver sees "This workflow will cost approximately $12,500 for the month." Accuracy: within 5% for compute (deterministic pricing), within 20% for network/storage (usage-dependent).

Q: How do you handle the case where a user's template references a resource that already exists (e.g., `terraform import`)?
A: The Template Engine delegates to Terraform, which has native state management. If a resource already exists in the Terraform state, `terraform apply` computes the diff and only applies changes. For new resources referenced in the template that exist in the IaaS platform but not in Terraform state, the user runs `infra-cli template import` which calls `terraform import` under the hood. State files are stored in S3/GCS with locking (DynamoDB or GCS atomic operations) to prevent concurrent state corruption.

---

### Component: Cost Service and Budget Enforcement

**Why it's hard:** Cost data comes from multiple sources (compute, storage, network, GPU reservations), each with different metering granularity and billing models (per-hour, per-GB, per-request). We need to provide real-time cost visibility (< 5 minute lag), accurate forecasting, and hard budget enforcement (blocking new provisioning when over budget) without creating a bottleneck.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Real-time per-request billing** | Check cost on every API call | Always accurate | High latency for every write, single point of failure | Small scale |
| **Periodic batch aggregation** | Nightly cron aggregates metering data | Simple, batch-efficient | Up to 24-hour lag in cost visibility | When real-time is not needed |
| **Stream processing** | Kafka Streams / Flink aggregates events in real-time | Low lag, scalable, continuous | Operational complexity of stream processing cluster | **Our use case: near-real-time cost with budget enforcement** |
| **Pre-computed materialized views** | DB triggers maintain aggregate tables | Zero lag for DB-local events | Complex triggers, not all events are DB-local | Simple single-DB systems |

**Selected Approach:** Kafka Streams for real-time cost aggregation, with pre-computed aggregates in MySQL for dashboard queries. Budget checks use Redis counters for sub-millisecond enforcement.

**Implementation Detail:**

```java
// Kafka Streams topology for cost aggregation
@Bean
public KStream<String, MeteringEvent> costAggregationStream(StreamsBuilder builder) {
    KStream<String, MeteringEvent> events = builder.stream("metering.events");
    
    // Join metering events with pricing data (GlobalKTable)
    GlobalKTable<String, PricingRule> pricing = builder.globalTable("pricing.rules");
    
    KStream<String, CostEvent> costedEvents = events.leftJoin(
        pricing,
        (key, event) -> event.getResourceType() + ":" + event.getMachineType(),
        (event, price) -> CostEvent.builder()
            .tenantId(event.getTenantId())
            .teamId(event.getTeamId())
            .projectId(event.getProjectId())
            .resourceType(event.getResourceType())
            .costUsd(price.getHourlyRate().multiply(event.getDurationHours()))
            .timestamp(event.getTimestamp())
            .build()
    );
    
    // Aggregate by tenant + team + project + hour
    KTable<Windowed<String>, BigDecimal> hourlyAggregates = costedEvents
        .groupBy((key, event) -> event.getAggregateKey())
        .windowedBy(TimeWindows.ofSizeWithNoGrace(Duration.ofHours(1)))
        .aggregate(
            () -> BigDecimal.ZERO,
            (key, event, total) -> total.add(event.getCostUsd()),
            Materialized.with(Serdes.String(), bigDecimalSerde)
        );
    
    // Write to MySQL sink + update Redis budget counters
    hourlyAggregates.toStream()
        .foreach((windowedKey, cost) -> {
            costAggregateRepo.upsert(windowedKey, cost);
            redisBudgetCounter.incrementBy(
                windowedKey.key(), cost.doubleValue());
        });
    
    return events;
}

// Budget enforcement (called on every resource creation)
@Service
public class BudgetEnforcer {
    
    public BudgetCheckResult checkBudget(String tenantId, String teamId, 
                                           BigDecimal estimatedCost) {
        // Fast path: Redis counter
        String budgetKey = "budget:" + teamId + ":" + currentPeriod();
        double currentSpend = redisTemplate.opsForValue()
            .increment(budgetKey, 0);  // Read without increment
        
        Budget budget = budgetCache.get(teamId);
        if (budget == null) {
            return BudgetCheckResult.noBudgetConfigured();
        }
        
        double projectedSpend = currentSpend + estimatedCost.doubleValue();
        
        if (projectedSpend > budget.getHardLimitUsd()) {
            return BudgetCheckResult.blocked(
                "Budget exceeded. Current: $" + currentSpend + 
                ", Limit: $" + budget.getHardLimitUsd());
        }
        
        if (projectedSpend > budget.getSoftLimitUsd() && !budget.isAlertSent()) {
            notificationService.sendBudgetWarning(teamId, currentSpend, budget);
            budget.setAlertSent(true);
            budgetRepo.save(budget);
        }
        
        return BudgetCheckResult.allowed();
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Kafka Streams lag | Cost dashboard shows stale data | Monitor consumer lag. If lag > 5 minutes, alert. Lag does not affect budget enforcement (Redis counter is updated in near-real-time by a separate lightweight consumer). |
| Redis budget counter drift | Budget enforcement inaccurate | Reconciliation job runs every 15 minutes: recomputes Redis counters from MySQL aggregates. Maximum drift: 15 minutes of cost data. |
| Metering event lost | Under-billing | Kafka replication factor 3 + exactly-once semantics. Daily reconciliation: compare active resources in DB against metering events. Missing events are reconstructed. |
| Pricing data stale | Incorrect cost calculation | Pricing rules are versioned and cached (Kafka GlobalKTable). Changes propagate in < 1 second. Pricing changes are rare (weekly at most). |

**Interviewer Deep-Dive Q&A:**

Q: How do you handle cost forecasting?
A: The Cost Service maintains a per-team daily spend time series. For short-term forecasting (end of month), we use a simple linear extrapolation: `forecast = (current_spend / days_elapsed) * total_days_in_month`. For longer-term forecasting (next quarter), we use exponential smoothing with seasonal decomposition (teams tend to spend more at end of quarter, less at beginning). The forecast is displayed on the cost dashboard and used to trigger proactive budget warnings ("At your current spending rate, you will exceed your budget by April 22nd").

Q: How do you handle chargebacks for shared resources (e.g., shared network, shared storage)?
A: Shared resources are prorated by usage. Network egress is metered per instance. Shared storage (Ceph pool) is metered per volume. If a network or storage resource is shared across teams, the cost is split proportionally by each team's usage (e.g., team A has 60% of the traffic, pays 60% of the network cost). For simplicity, v1 uses a flat infrastructure fee per tenant that covers shared costs. V2 introduces usage-based allocation.

Q: What happens if the budget service is down?
A: Budget enforcement has a fail-open policy for non-critical operations and fail-closed for expensive ones. If Redis is unavailable: (1) Requests for resources costing < $100 are allowed (fail-open, with warning logged). (2) Requests for resources costing > $100 are rejected (fail-closed) until budget can be verified. This prevents a Redis outage from enabling massive over-spend while minimizing impact on small requests. The threshold is configurable per tenant tier.

Q: How accurate is the cost data?
A: Compute cost (per-hour reservation/VM): exact. Metering events are emitted at start/stop with timestamps. Storage cost (per-GB-month): exact for block volumes (size is known). For object storage: usage polled hourly (< 1% error). Network cost (per-GB egress): sampled at flow level (sFlow/IPFIX), with ~95% accuracy for per-tenant attribution. Total accuracy: > 98% for compute-dominant workloads (which is most of our usage). We reconcile monthly and credit/debit differences > $10.

Q: How do you prevent a malicious user from gaming the budget system (e.g., creating resources just before the budget period resets)?
A: (1) Budget periods overlap: monthly budgets are checked against a rolling 30-day window, not calendar month. This prevents end-of-month gaming. (2) Suspicious spending spikes trigger alerts: if a team's hourly spend exceeds 3x their 7-day average, an alert is sent. (3) Resource limits complement budget limits: even within budget, per-tenant quotas cap the number of instances/GPUs.

Q: How do you handle multi-currency billing for global tenants?
A: All internal cost tracking is in USD. Currency conversion happens at the billing/invoicing layer (outside our scope). We store cost_usd consistently and the finance system applies the appropriate exchange rate at invoice generation time. This avoids the complexity of real-time FX rate fluctuations in the infrastructure cost service.

---

### Component: Approval Workflow

**Why it's hard:** Approval workflows must be flexible (different thresholds per team, per environment, per resource type), fast (approvers should not wait), secure (cannot bypass approvals), and integrated with multiple UIs (portal, Slack, email, CLI).

**Selected Approach:** Rule-based approval engine with configurable policies stored in MySQL. Policies are evaluated at workflow creation time to determine if approval is needed and from whom.

**Implementation Detail:**

```java
@Service
public class ApprovalPolicyEngine {
    
    // Approval policies (loaded from DB, cached in memory)
    private static final List<ApprovalPolicy> POLICIES = List.of(
        // GPU requests > 8 machines: manager approval
        ApprovalPolicy.builder()
            .condition("resource_type == 'gpu' && count > 8")
            .requiredRole("manager")
            .requiredGroup("${team}_leads")
            .build(),
        
        // Cost > $10K: manager approval
        ApprovalPolicy.builder()
            .condition("estimated_cost_usd > 10000")
            .requiredRole("manager")
            .requiredGroup("${team}_leads")
            .build(),
        
        // Cost > $50K: VP approval
        ApprovalPolicy.builder()
            .condition("estimated_cost_usd > 50000")
            .requiredRole("vp")
            .requiredGroup("engineering_leadership")
            .build(),
        
        // Production environment: tech lead approval
        ApprovalPolicy.builder()
            .condition("environment == 'prod'")
            .requiredRole("tech_lead")
            .requiredGroup("${team}_leads")
            .build(),
        
        // Dev environment: no approval needed (auto-approve)
        ApprovalPolicy.builder()
            .condition("environment == 'dev' && estimated_cost_usd < 1000")
            .autoApprove(true)
            .build()
    );
    
    public List<ApprovalRequirement> evaluatePolicies(WorkflowContext context) {
        List<ApprovalRequirement> requirements = new ArrayList<>();
        
        for (ApprovalPolicy policy : POLICIES) {
            if (policy.evaluate(context)) {
                if (policy.isAutoApprove()) {
                    // No approval needed, auto-approve
                    continue;
                }
                
                String group = policy.getRequiredGroup()
                    .replace("${team}", context.getTeamName());
                
                requirements.add(ApprovalRequirement.builder()
                    .role(policy.getRequiredRole())
                    .group(group)
                    .expiresIn(Duration.ofHours(72))
                    .build());
            }
        }
        
        return requirements;  // May have multiple (e.g., manager + VP for $60K GPU request)
    }
}
```

**Interviewer Deep-Dive Q&A:**

Q: How do you prevent approval fatigue (rubber-stamping)?
A: (1) Auto-approve low-risk requests (dev environment, < $1K). Managers only see high-value approvals. (2) Context-rich approval messages: include estimated cost, resource details, historical usage ("Team X's GPU usage has increased 50% this month"), and cost impact. (3) Monthly approval metrics: track approval-to-rejection ratio per approver. A 100% approval rate may indicate rubber-stamping -- flag for review. (4) Time-limited approvals: expire after 72 hours. No "approved 3 months ago" stale approvals.

Q: How do you handle the case where the designated approver is on vacation?
A: (1) Delegation: approvers can set a delegate who inherits their approval authority for a date range. (2) Group-based approval: the approval is assigned to a group (e.g., `ml-team-leads`), not an individual. Any member of the group can approve. (3) Escalation: if no one approves within 24 hours, the Notification Service escalates to the next level (manager -> VP -> admin). (4) The CLI supports `--auto-expire 72h` so the requester is not blocked indefinitely.

Q: How do you ensure approvals cannot be bypassed via the API?
A: The Workflow Engine enforces approval gates in code. A workflow with a pending approval cannot have its steps executed -- the engine checks the approval status before executing any step that depends on the approval step. Even if someone tries to call the IaaS API directly (bypassing the workflow), the IaaS API itself enforces budget limits and quotas. For production environments, there is a secondary check: the IaaS API verifies that a valid workflow (with approved approval records) exists for any resource creation in `prod` environment. This prevents bypassing the portal entirely.

Q: How do you audit approval decisions?
A: Every approval action (approve, reject, auto-approve, expire, delegate) is recorded in the `approvals` table with: decision, approver ID, timestamp, reason text, and the full workflow context at decision time (JSON snapshot). These records are immutable (no UPDATE allowed by application service accounts). The audit dashboard shows: all approvals for a team/time period, approval latency (time from request to decision), auto-approve rate, rejection reasons. Compliance can query: "Show me all approved GPU requests > $50K in Q1."

---

## 7. Scheduling & Resource Management

### Workflow Scheduling

The workflow engine processes workflows in priority order:
1. **Production environment** workflows run first (lowest latency for critical deployments).
2. **Staging** workflows run second.
3. **Dev** workflows run last.

Within the same priority, FIFO ordering applies.

The engine limits concurrent workflow execution per tenant (default: 10) to prevent one team from monopolizing provisioning capacity.

### Template Dependency Resolution

Templates declare resource dependencies. The engine resolves them into a DAG:

```
Network -> Subnet -> Security Group -> Instance -> Volume Attach
                                        |
                           (parallel) -> Floating IP Assign
```

Steps without dependencies run in parallel. The DAG is computed at workflow creation time (static analysis of the template) and stored in the `workflow_steps` table.

### Cost-Based Resource Management

The Cost Service provides real-time budget data to the Workflow Engine. Before executing a provisioning step, the engine calls `BudgetEnforcer.checkBudget()`. If the budget is exceeded:
- **Hard limit**: Workflow pauses with status `BUDGET_EXCEEDED`. User must either increase budget (requires finance approval) or reduce the template.
- **Soft limit**: Workflow proceeds but a warning notification is sent.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Instances |
|-----------|-----------------|-----------|
| Portal BFF | Stateless; scale on request rate | 4-8 replicas, HPA on CPU |
| Workflow Engine | Partitioned by tenant_id hash; each instance handles a subset of tenants | 4-8 replicas |
| Cost Service | Kafka Streams instances (one per partition) | 6-12 instances (matching Kafka partitions) |
| Catalog Service | Stateless; scale on query rate | 2-4 replicas |
| Notification Service | Scale on Kafka consumer lag | 2-4 replicas |
| Template Engine | Scale on apply queue depth | 4-8 replicas (each runs Terraform in isolation) |

### Database Scaling

- MySQL: single primary + 2 replicas. Portal reads go to replicas. Workflow writes go to primary.
- For cost_aggregates (highest volume table): partitioned by month. Old partitions archived.

### Caching

| Layer | What to cache | Strategy | Tool | Eviction | TTL | Invalidation |
|-------|---------------|----------|------|----------|-----|--------------|
| Portal BFF | Dashboard aggregates | Cache-aside | Redis | LRU | 30s | On cost event |
| Portal BFF | User session | Write-through | Redis | TTL | 8 hours | Logout |
| Catalog Service | Machine types, images | Cache-aside | Local (Caffeine) | LRU | 5 min | TTL-based |
| Cost Service | Budget configs | Cache-aside | Local (Caffeine) | LRU | 1 min | On budget change |
| Cost Service | Budget counters | Write-through | Redis | N/A | N/A | Reconciled every 15 min |
| Template Engine | Template content | Cache-aside | Local disk | LRU | 10 min | On template update |

### Kubernetes-Specific

- Template Engine pods run with `securityContext.readOnlyRootFilesystem: false` and ephemeral storage limits (Terraform needs a writable workspace). Isolated via network policies: cannot reach anything except the IaaS API and S3/GCS.
- Workflow Engine uses leader election (etcd lease) for the singleton poll loop. Redundant replicas are hot standbys.

**Interviewer Deep-Dive Q&A:**

Q: How do you scale the Terraform execution to handle 1,000 applies/day?
A: Each Template Engine pod runs one `terraform apply` at a time in an isolated workspace directory. With 8 replicas, we handle 8 concurrent applies. Average apply takes 5 minutes; 8 x 12 applies/hour = 96/hour x 10 hours = 960/day, covering peak. For burst: HPA scales pods up to 20 based on queue depth in Kafka. Each pod pulls the next apply from the Kafka topic, runs Terraform in an ephemeral directory, and reports results. Terraform state is stored in S3/GCS with locking, so concurrent applies to different workspaces are safe.

Q: How do you prevent Terraform state corruption?
A: Terraform state is stored in a remote backend (S3 + DynamoDB for locking, or GCS with atomic operations). Each template has a unique state key (derived from tenant_id + template_id + environment). Before `terraform apply`, the engine acquires the state lock. If the lock is held (another apply in progress for the same template), the engine queues the request and retries. The lock has a 30-minute TTL (force-unlock if Terraform crashes). We never run concurrent applies against the same state file.

Q: How does the portal handle 500 concurrent users without becoming slow?
A: (1) The portal is a React SPA; static assets served from CDN (CloudFront/Akamai). The BFF only serves API data, not HTML. (2) Dashboard data is pre-aggregated in MySQL (cost_aggregates table) and cached in Redis (30s TTL). Rendering a cost chart is a single Redis read, not a complex aggregation query. (3) WebSocket connections for real-time workflow updates (instead of polling). Each BFF instance holds ~100 WebSocket connections; 8 instances handle 500+ users. (4) API Gateway rate limits per user: 50 req/sec. This prevents a broken script from overwhelming the BFF.

Q: How do you ensure the portal works during a partial outage (e.g., Cost Service is down)?
A: The Portal BFF implements circuit breakers for each backend service. If the Cost Service is down: (1) The cost dashboard shows cached data with a "Data may be stale (last updated X minutes ago)" banner. (2) Budget enforcement degrades to fail-open for small requests (< $100). (3) All other portal functions (resource management, workflows, approvals) continue working. The portal never shows a blank page or error screen for a single backend service failure -- it gracefully degrades the affected section.

Q: How do you handle API key security? What prevents key leakage?
A: (1) The secret key is shown exactly once at creation time. After that, only the key_id (public identifier) is stored. The key_hash (SHA-256 of the secret) is stored for authentication. (2) Keys have mandatory expiration (max 365 days, default 90 days). (3) Keys have scoped permissions (least privilege). (4) Key usage is logged (last_used_at, source IP). (5) Secret scanning: we integrate with GitHub/GitLab secret detection to flag leaked API keys in source code. Flagged keys are automatically revoked and the owner notified. (6) Key rotation: the CLI supports `infra-cli api-keys rotate ik_abc123` which creates a new key and revokes the old one atomically.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios & Mitigations

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| Portal BFF crash | Affected users see errors | K8s liveness probe | Pod restart; other replicas handle traffic | 15s | 0 (stateless) |
| Workflow Engine crash | Workflows pause | K8s liveness probe + etcd leader loss detection | New leader elected in 10s; workflows resume from persisted state | 10-30s | 0 (state in MySQL) |
| MySQL primary fails | Workflows cannot progress, approvals stuck | MySQL orchestrator | Semi-sync replica promotion | 30s | 0 |
| Kafka unavailable | Cost events queue, step execution commands buffer | Kafka health check | Outbox pattern for commands; cost data catches up on recovery | 1-30 min | 0 (outbox) |
| Cost Service lag | Stale cost dashboard | Consumer lag metric | Alert at > 5 min lag; users see staleness indicator | N/A (degraded) | N/A |
| SSO provider down | Users cannot log in | Health check | Cached JWT tokens continue to work for existing sessions (up to 15 min). CLI users with API keys are unaffected. | Depends on SSO provider | N/A |
| Slack API down | Approval notifications not delivered | Slack API error rate | Fallback to email. Retry Slack delivery with backoff. | Minutes | 0 (approvals still in DB) |

### Automated Recovery

- Workflow stuck detection: hourly scan for workflows in `RUNNING` state with no step progress in > 2 hours. Alert on-call.
- Expired approval cleanup: hourly scan for approvals past `expires_at`. Auto-reject, cancel workflow, notify requester.
- Orphan resource detection: daily scan compares resources created by workflows against resources in IaaS platform. Flag discrepancies.

---

## 10. Observability

### Key Metrics

| Metric | Type | Tool | Alert Threshold | Business Impact |
|--------|------|------|-----------------|-----------------|
| `portal.page_load.latency_p99` | Histogram | Prometheus | > 3s | User experience |
| `workflow.completion_rate` | Counter | Prometheus | < 90% over 1h | Provisioning broken |
| `workflow.duration_p99` | Histogram | Prometheus | > 30 min | Slow provisioning |
| `approval.pending.count` | Gauge | Prometheus | > 50 | Approval bottleneck |
| `approval.latency_p50` | Histogram | Prometheus | > 4 hours | Slow approvals blocking teams |
| `cost.pipeline.lag` | Gauge | Prometheus | > 5 min | Stale cost data |
| `budget.enforcement.blocked` | Counter | Prometheus | > 10/day | Teams hitting budget limits frequently |
| `template.apply.failure_rate` | Counter | Prometheus | > 10% | Template or IaaS issue |
| `notification.delivery.failure_rate` | Counter | Prometheus | > 5% | Users not getting notifications |

### Distributed Tracing
Traces span: Portal click -> BFF -> Workflow Engine -> IaaS API -> Provisioning. Trace ID displayed in the portal workflow detail page for debugging.

### Logging
Structured JSON with user_id, tenant_id, workflow_id, trace_id. Aggregated in ELK. Portal access logs track: page views, feature usage, error rates (for UX improvement).

---

## 11. Security

### Authentication & Authorization

- **SSO**: OIDC with Okta. JWT tokens with 15-minute access token, 8-hour refresh token.
- **API keys**: Scoped service accounts for CI/CD. SHA-256 hashed storage. Rate limited.
- **RBAC roles**: Mapped from SSO groups.
  - `viewer`: Read-only access to own team's resources and costs.
  - `developer`: Create/manage resources within quota and budget. Submit workflows.
  - `operator`: Approve workflows. Manage team budgets. View all team members' resources.
  - `admin`: Cross-team access. Manage quotas, policies, catalog.

### Secrets Management
- API key secrets never stored in plaintext (only hash).
- Terraform state files encrypted at rest (S3 SSE-KMS).
- SSO client secret in Vault.
- Database credentials via Vault dynamic secrets.

### Network Security
- Portal served over HTTPS (TLS 1.3).
- BFF to backend services: mTLS (Istio).
- Template Engine pods isolated via Kubernetes NetworkPolicy: can only reach IaaS API and S3/GCS.

### Audit Logging
- Every portal action logged: who, what, when, from where (IP, user agent).
- Every approval decision logged with full context.
- Every template apply logged with template version, parameters, and outcome.
- Audit logs in Elasticsearch, retained 3 years.

---

## 12. Incremental Rollout Strategy

### Portal Rollout

1. **Feature flags**: New portal features behind LaunchDarkly flags. Enabled for internal dogfood team (50 users) for 1 week.
2. **Canary**: 5% of users see the new feature (by user_id hash). Monitor error rates and user engagement.
3. **Gradual rollout**: 25% -> 50% -> 100% over 2 weeks.

### API/CLI Rollout

- API versioning (`/api/v1/`, `/api/v2/`). New versions additive, old versions supported for 6 months.
- CLI auto-update: `infra-cli` checks for updates on every invocation (background check, non-blocking). Users prompted to upgrade. Old CLI versions continue to work against old API versions.

### Workflow Engine Rollout

- New workflow step types (e.g., adding a security scan step) are feature-flagged per tenant.
- Existing running workflows are not affected by new code (they use the step definitions captured at creation time).

**Rollout Q&A:**

Q: How do you A/B test a new portal feature (e.g., a new cost dashboard)?
A: Feature flag with user_id bucketing. Group A sees the old dashboard, Group B sees the new one. We measure: (1) Page load latency (new dashboard may have more data). (2) User engagement (time on page, click-through to details). (3) Support tickets (does the new dashboard cause confusion?). After 2 weeks of data, we analyze results and decide to ship, iterate, or revert. The feature flag ensures instant rollback.

Q: How do you handle breaking changes to the CLI?
A: We version the CLI separately from the API. Major version bumps (e.g., v1 -> v2) require explicit user action (`infra-cli upgrade --major`). Within a major version, all changes are backward-compatible (new flags, new commands, but no removed flags). The CLI checks its version against the server's minimum supported version and warns if outdated. We maintain backward compatibility for at least 6 months after a major version bump.

Q: How do you migrate approval policies (e.g., changing the threshold from $10K to $15K)?
A: Approval policies are stored in the DB and cached in memory. Changing a policy: (1) Update the DB record. (2) Cache invalidation propagates to all engine instances within 1 minute. (3) Running workflows that already have pending approvals at the old threshold are not affected (their approval requirements were captured at creation time). (4) New workflows use the new threshold. This ensures no workflow is retroactively affected by a policy change.

Q: How do you handle a scenario where the portal is under DDoS attack?
A: (1) CDN (CloudFront) absorbs static asset requests. (2) API Gateway rate limits per IP and per authenticated user. (3) WAF rules block known attack patterns (SQL injection, XSS, volumetric attacks). (4) If the attack overwhelms the gateway, we enable Cloudflare/AWS Shield Advanced DDoS protection. (5) The CLI and API keys are unaffected (different endpoint, authenticated traffic only). The portal may be temporarily unavailable, but core infrastructure operations via CLI continue.

Q: How do you ensure the template engine (running Terraform) is secure?
A: (1) **Network isolation**: Template Engine pods can only reach the IaaS API and S3/GCS (Kubernetes NetworkPolicy). No internet access. (2) **Process isolation**: Each `terraform apply` runs in a separate ephemeral directory, cleaned after completion. No shared state between applies. (3) **Credential scoping**: Terraform uses a service account with minimal IaaS API permissions (create/delete resources for the requesting tenant only). (4) **Template validation**: Before apply, we scan for known anti-patterns (e.g., requesting excessive resources, using deprecated providers). (5) **Audit**: Full Terraform plan output is logged and attached to the workflow record.

---

## 13. Trade-offs & Decision Log

| Decision | Option A | Option B | Option C | Chosen | Specific Reason |
|----------|----------|----------|----------|--------|-----------------|
| Portal framework | Server-rendered (Next.js) | SPA (React) | Mobile app | **React SPA** | Engineers use desktop browsers. SPA provides snappy UX with CDN-served static assets. Server-rendering adds complexity without benefit for our user base. |
| CLI language | Python | Go | Rust | **Go** | Single binary, no runtime dependency. Fast startup (< 100ms). Cross-platform compilation. Python CLI requires managing virtualenvs. Rust is overkill for a CLI. |
| Workflow engine | Temporal | Airflow | Custom (DB-backed) | **Custom** | Moderate complexity (3-10 steps). Critical requirement is approval integration, which is trivial with DB state machine. Temporal adds infra dependency for limited benefit at this scale. |
| Cost aggregation | Batch (nightly cron) | Stream (Kafka Streams) | Real-time DB triggers | **Kafka Streams** | Near-real-time cost visibility (< 5 min lag). Nightly batch has 24-hour lag. DB triggers cannot aggregate cross-service events. |
| Budget enforcement | Synchronous DB check | Redis counter | API gateway plugin | **Redis counter** | Sub-millisecond check latency. Cannot afford DB round-trip on every resource creation. Redis INCR is atomic and fast. Reconciled with DB periodically. |
| Approval integration | Portal only | Slack only | Portal + Slack + email | **Portal + Slack + email** | Approvers are in different contexts. Slack approval reduces context-switch time. Email is the fallback. Portal provides full context when needed. |
| Template storage | MySQL BLOB | S3/GCS | Git repository | **S3/GCS** | Templates can be large (hundreds of KB). S3/GCS provides versioned, durable storage. Git is used for development but S3/GCS is the operational store. |
| IaC tool | Terraform | Pulumi | Custom YAML DSL | **Terraform** | Established ecosystem, HCL is widely known, provider model fits our API. Pulumi is code-based (good but higher barrier). Custom DSL requires building a language. |

---

## 14. Agentic AI Integration

### Where AI Adds Value

1. **Natural language provisioning**: "I need 4 H100 GPUs for a training run next Tuesday, 9am to 5pm." The AI agent parses this, generates a template, estimates cost, and presents for confirmation. Reduces barrier to entry for non-expert users.

2. **Template generation assistant**: "I need infrastructure for a distributed PyTorch training job with 16 nodes, NVLink topology, shared NFS storage, and a monitoring stack." The agent generates a complete Terraform template based on best practices.

3. **Cost optimization recommendations**: Agent analyzes usage patterns and recommends: "Your team's GPU utilization averages 12% on weekends. Consider converting weekend reservations to shorter windows, saving ~$8,000/month."

4. **Approval summarization**: For complex approval requests, the agent generates a one-paragraph summary: "This request provisions 32 H100 GPUs for the LLM-v5 project, estimated cost $45,000/month, 3x Team ML's current usage. Historical precedent: similar requests approved 3 times in Q1 for the same project."

### Guard Rails
- Template generation: always presented as a draft for human review. Never auto-applied.
- Cost recommendations: notification only, no automatic changes.
- Natural language provisioning: shows dry-run result and estimated cost, requires explicit confirmation.

---

## 15. Complete Interviewer Q&A Bank

**Q: How do you handle a workflow that requires approvals from multiple people (e.g., both manager and VP)?**
A: The approval policy engine can return multiple approval requirements. The workflow creates multiple approval records, all as dependencies for the approval step. The approval step is only considered `COMPLETED` when all required approvals are received. The workflow engine checks: `approvals.stream().allMatch(a -> a.getStatus() == APPROVED)`. If any approval is rejected, the entire workflow is cancelled. Approvals are independent: the VP can approve before the manager. The order does not matter -- only that all required approvals are present before proceeding.

**Q: How do you prevent infrastructure sprawl (users provisioning resources and forgetting about them)?**
A: Five mechanisms: (1) **Resource TTL**: Configurable per environment (dev: 7 days default, staging: 30 days, prod: no TTL). Resources approaching TTL get 24-hour and 1-hour warning notifications. After TTL, auto-terminated unless renewed. (2) **Weekly digest email**: Each team receives a summary of their active resources, cost, and utilization. Low-utilization resources highlighted. (3) **Cost dashboards**: Real-time visibility makes forgotten resources obvious. (4) **Orphan detection**: Daily scan for resources not attached to any active project or workflow. Flagged for owner review. (5) **Budget pressure**: Hard budget limits naturally force teams to clean up unused resources.

**Q: How does the portal handle slow backend services?**
A: The Portal BFF sets aggressive timeouts per backend call (2 seconds for most services). If a backend service is slow or down, the BFF returns cached data (if available) or a graceful degradation response (section shows "Loading..." with a retry button, not a full page error). The circuit breaker pattern prevents cascading failures: if the Cost Service has 50% failures over a 10-second window, the circuit opens and all Cost Service calls return cached data for 30 seconds before re-testing. Each section of the dashboard is independently loaded (micro-frontend pattern), so one failing section does not block others.

**Q: How do you handle template versioning and rollback?**
A: Templates are versioned in S3/GCS (each version is a separate object with a version suffix). The `templates` table tracks all versions. When a user runs `infra-cli template apply`, the engine uses the latest version by default but can target a specific version (`--version 1.2.3`). Rollback: `infra-cli template apply --version 1.1.0` applies the previous version, which executes a `terraform plan` showing the diff and then applies the changes to bring infrastructure to the old spec. Terraform state tracks the current deployed state, so the rollback is just an apply of an older template version against the current state.

**Q: How do you ensure consistency between the portal view and the actual infrastructure state?**
A: The portal always reads from the IaaS platform's APIs (via the BFF), not from its own database. Workflow state is the only portal-owned data. Resource state (instances, networks, volumes) is always fetched from the IaaS platform, which is the source of truth. The BFF caches IaaS responses for 30 seconds to reduce load, but users can force-refresh. The dashboard shows a "Last updated X seconds ago" timestamp. For critical operations (create, delete), the portal polls the IaaS API directly (not cache) for up-to-date status.

**Q: How do you handle the scenario where an approved workflow cannot complete because the infrastructure is no longer available (e.g., GPU capacity filled between approval and execution)?**
A: The workflow engine re-checks capacity at execution time, not at approval time. If the approved workflow's provisioning step fails with "insufficient capacity," the workflow pauses with status `WAITING_CAPACITY` and enters the waitlist queue (delegated to the Bare-Metal Reservation Platform's waitlist). The user is notified: "Your approved workflow is waiting for GPU capacity. Estimated wait: 2 hours." The approval remains valid for 72 hours. If capacity is not available within that window, the workflow is auto-cancelled and the user must re-request.

**Q: How do you onboard a new team to the portal?**
A: (1) Team lead requests a new tenant/project via the admin portal or CLI. (2) Admin creates the tenant with default quotas and budget (auto-approved for standard tier). (3) Team members are added by SSO group mapping (no manual user creation). (4) The team lead configures: budget (soft/hard limits), notification preferences (Slack channel), default environment (dev), approval policies (can override defaults). (5) The team browses the resource catalog and template library, picks a template, customizes, and deploys. First deployment guides them through the approval process. (6) Total onboarding time: < 1 hour for a team already familiar with IaC.

**Q: How do you measure the portal's effectiveness?**
A: Key metrics: (1) **Time-to-provision**: From request to active resource. Target: < 30 min including approvals for dev, < 2 hours for prod. (2) **Self-service rate**: Percentage of provisioning requests completed without ops team involvement. Target: > 95%. (3) **Portal adoption**: Percentage of infrastructure provisioned via portal/CLI vs. ad-hoc tickets. Target: > 90%. (4) **Approval latency**: p50 time from approval request to decision. Target: < 2 hours. (5) **User satisfaction**: Quarterly NPS survey. Target: > 40. (6) **Cost visibility impact**: Reduction in over-provisioning after cost dashboard launch (measured by average utilization improvement).

**Q: How do you support disaster recovery for the portal itself?**
A: The portal is stateless (React SPA on CDN + BFF pods on K8s). Workflow state is in MySQL (replicated). (1) CDN failure: DNS failover to direct BFF serving of static assets (pre-packaged in the BFF container). (2) BFF failure: K8s restarts pods. Multiple replicas across AZs. (3) MySQL failure: semi-sync replica promotion (30s). (4) Redis failure: graceful degradation (slower but functional). (5) Full region failure: the portal is deployed in 2 regions with active-active routing. Users in the affected region are routed to the surviving region. Workflow state is replicated via MySQL cross-region async replication (< 1 min lag). Users may see slightly stale workflow status but can continue operating.

**Q: How do you handle audit requirements for SOC 2 / ISO 27001?**
A: (1) Every API call is logged with actor, action, resource, timestamp, source IP. (2) Every approval decision is logged with approver, decision, reason, workflow context. (3) Every template apply is logged with template version, parameters, resources created. (4) Audit logs are immutable (append-only, no DELETE/UPDATE by app service accounts). (5) Retained for 3 years (SOC 2 requirement). (6) Access to audit logs is restricted to compliance team and admins. (7) Quarterly access review: verify all admin role holders are current employees with business justification. (8) Penetration testing: annual, with remediation tracking.

**Q: How do you handle the case where a Terraform provider has a bug that creates resources but does not return the ID?**
A: The Template Engine wraps Terraform execution with pre- and post-execution resource counting. (1) Before apply: query IaaS API for current resource count for the tenant. (2) Run `terraform apply`. (3) After apply: query IaaS API again. (4) If resource count increased but Terraform state does not reflect the new resource, a reconciliation alert fires. The Terraform state is updated via `terraform import` for the missing resource. (5) We maintain a custom Terraform provider (`terraform-provider-iaas`) with comprehensive integration tests. Provider bugs are caught in CI before release. (6) For critical bugs: the Template Engine supports a `--terraform-binary` flag to pin a specific Terraform version, allowing users to roll back to a known-good version.

**Q: How do you manage configuration for different environments (dev/staging/prod)?**
A: (1) Template variables file per environment: `dev.tfvars`, `staging.tfvars`, `prod.tfvars`. Different machine counts, network configs, budget limits. (2) Environment-specific approval policies: dev auto-approves < $1K; prod requires tech lead + manager for any change. (3) Environment-specific quotas: dev has lower limits (prevents accidental large-scale provisioning). (4) Environment-specific Terraform backends: separate state files per environment (prevents dev changes from affecting prod). (5) The CLI supports `--env dev|staging|prod` to select the configuration. (6) CI/CD pipelines: PR merge to `main` triggers staging apply; manual promotion triggers prod apply.

---

## 16. References

- "Terraform: Up and Running" by Yevgeniy Brikman -- Terraform patterns, state management, module design.
- "Building Micro-Frontends" by Luca Mezzalira (O'Reilly) -- Independent dashboard sections, BFF pattern.
- "Kafka Streams in Action" by Bill Bejeck (Manning) -- Stream processing for real-time aggregation.
- "The Design of Everyday Things" by Don Norman -- UX principles for self-service tooling (reducing cognitive load, progressive disclosure).
- "HashiCorp Terraform Provider Development" -- registry.terraform.io. Custom provider patterns, schema design, CRUD lifecycle.
- "Slack Block Kit Builder" -- api.slack.com/block-kit. Interactive message design for approval workflows.
- "OIDC (OpenID Connect) Specification" -- openid.net. Authentication flow for SSO integration.
- "LaunchDarkly Feature Flag Best Practices" -- launchdarkly.com. Feature flagging patterns, percentage rollouts, user targeting.
- "React Query / TanStack Query" -- tanstack.com. Server-state caching for React SPAs, stale-while-revalidate pattern.
- "FinOps Framework" -- finops.org. Cloud cost management practices, showback/chargeback models, optimization workflows.
