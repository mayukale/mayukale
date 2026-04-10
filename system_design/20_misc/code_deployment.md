# System Design: Code Deployment System (Spinnaker / Argo CD-style)

---

## 1. Requirement Clarifications

### Functional Requirements

1. Engineers trigger deployments from a CI/CD pipeline or manually via a UI/API.
2. The system supports multiple rollout strategies: blue-green, canary, rolling update, and recreate.
3. Artifact versioning: each build produces a versioned artifact (Docker image tag, JAR, binary) stored in an artifact registry. Deployments reference specific artifact versions.
4. Pre-deployment and post-deployment health checks: the system runs configurable checks (HTTP readiness probes, synthetic transactions, custom scripts) before promoting traffic.
5. Automatic rollback: if post-deployment health checks fail or error rate exceeds a configurable threshold, the system automatically rolls back to the previous stable version.
6. Multi-region deployment: support deploying to multiple AWS/GCP regions with configurable promotion gates (e.g., deploy to us-east-1, wait 10 minutes, then promote to eu-west-1).
7. Deployment locks: prevent concurrent deployments to the same service/environment to avoid race conditions.
8. Audit log: every deployment action (trigger, promote, rollback, cancel) is recorded with actor identity, timestamp, and reason.
9. Notifications: notify the deploying engineer (Slack, email) on deployment success, failure, and rollback events.
10. Environment management: support dev, staging, and production environments per service with independent deployment pipelines.
11. Feature flags integration: coordinate deployments with feature flag systems for gradual enablement.
12. Rollback history: retain previous N versions (configurable, default 5) to enable one-click rollback.

### Non-Functional Requirements

1. Deployment system availability: 99.9% (brief unavailability doesn't prevent services from running; it only blocks new deployments).
2. Deployment trigger to first pod running: < 2 minutes for a rolling update.
3. Canary traffic ramp from 0% to 100%: supports 1-hour gradual ramps with automated analysis.
4. Support for 10,000+ services across hundreds of teams.
5. Deployment event throughput: 1,000 simultaneous deployments across all services.
6. Deployment locks must be strongly consistent — two simultaneous deploys for the same service/environment must never both acquire the lock.
7. Audit log retention: 2 years, immutable.
8. API response time for deployment status queries: p99 < 200 ms.
9. Rollback must complete within 5 minutes for any deployment strategy.
10. The deployment system itself must be independently deployable and upgradeable without affecting running services.

### Out of Scope

- Building the container orchestration layer (assume Kubernetes).
- CI pipeline (building, testing, pushing artifacts) — we receive a built artifact and deploy it.
- Service mesh configuration (Istio, Linkerd) — though we integrate with them for traffic splitting.
- Infrastructure provisioning (Terraform, Pulumi).
- Cost optimization / rightsizing.
- Log aggregation and APM setup (assume Datadog/New Relic).
- Secret management (assume Vault/AWS Secrets Manager).

---

## 2. Users & Scale

### User Types

| Role             | Description                                                                          |
|------------------|--------------------------------------------------------------------------------------|
| Engineer         | Triggers deployments, monitors progress, approves gates, initiates manual rollbacks  |
| Release Manager  | Creates and manages deployment pipelines; sets promotion policies                    |
| On-Call SRE      | Emergency rollbacks, deployment locks, override controls                             |
| CI Bot           | Automated service account that triggers deployments from CI pipeline                 |
| Auditor          | Read-only access to deployment history and audit logs                                |

### Traffic Estimates

**Assumptions:**
- 10,000 services across all teams.
- Average service deploys 4 times/day (high-velocity org: multiple times daily).
- 1,000 deployments/hour at peak (post-standup deploy surge: 9am Monday).
- Each deployment generates ~50 status poll requests from the CI bot + engineer UI.
- Deployment events (state changes published to Kafka): ~20 events per deployment.
- Health check pings: each deployment runs 5 health checks * 12 checks/minute for 5 minutes = 300 check results per deployment.

| Metric                              | Calculation                                        | Result           |
|-------------------------------------|----------------------------------------------------|------------------|
| Deployments/day                     | 10,000 services * 4 deploys                        | 40,000/day       |
| Deployments RPS (normal)            | 40,000 / 86,400                                    | ~0.46 RPS        |
| Peak deployments/hour               | 1,000/hour                                         | ~0.28 RPS        |
| Deployment status polls/day         | 40,000 * 50 polls                                  | 2,000,000/day    |
| Status poll RPS (normal)            | 2M / 86,400                                        | ~23 RPS          |
| Peak status poll RPS                | 1,000/hour * 50 / 3,600 (all 1,000 deploying sim.) | ~14 RPS          |
| Health check results/day            | 40,000 * 300                                       | 12,000,000/day   |
| Health check write RPS              | 12M / 86,400                                       | ~139 RPS         |
| Audit log writes/day                | 40,000 * 20 events                                 | 800,000/day      |
| Notification events/day             | 40,000 * 3 (start, end, rollback)                  | ~120,000/day     |

### Latency Requirements

| Operation                            | Target p50 | Target p99 |
|--------------------------------------|------------|------------|
| Deployment trigger (API)             | 50 ms      | 200 ms     |
| Deployment status read               | 20 ms      | 100 ms     |
| Lock acquisition                     | 10 ms      | 50 ms      |
| Health check execution               | 200 ms     | 2,000 ms   |
| Automatic rollback trigger (from failure detection) | < 30 s | < 60 s |
| First pod running after trigger      | < 2 min    | < 5 min    |
| Audit log write                      | 50 ms      | 200 ms     |

### Storage Estimates

**Assumptions:**
- Deployment record: 5 KB (metadata, config, timeline).
- Deployment event (state change): 500 bytes.
- Health check result: 500 bytes.
- Artifact metadata: 2 KB.
- Pipeline config: 10 KB.
- Audit log entry: 1 KB.
- Retention: 2 years.

| Data Type                  | Calculation                                          | Size        |
|----------------------------|------------------------------------------------------|-------------|
| Deployment records         | 40,000/day * 365 * 2 years * 5 KB                    | ~146 GB     |
| Deployment events          | 40,000 * 20 * 365 * 2 years * 500 B                  | ~292 GB     |
| Health check results       | 12M/day * 365 * 2 years * 500 B                      | ~4.38 TB    |
| Audit log                  | 800,000/day * 365 * 2 years * 1 KB                   | ~584 GB     |
| Artifact metadata          | 10,000 services * 100 versions * 2 KB                | ~2 GB       |
| Pipeline configs           | 10,000 services * 3 envs * 10 KB                     | ~300 MB     |
| Total                      | ~5.5 TB                                              | ~5.5 TB     |

### Bandwidth Estimates

| Traffic Type                  | Calculation                                           | Bandwidth     |
|-------------------------------|-------------------------------------------------------|---------------|
| Deployment status reads       | 23 RPS * 5 KB                                         | ~115 KB/s     |
| Artifact pulls (Kubernetes)   | 1,000/hour * 500 MB image / 3,600 (avg spread)        | ~139 MB/s     |
| Health check writes           | 139 RPS * 500 B                                       | ~70 KB/s      |
| Notification events           | Negligible                                            | < 10 KB/s     |
| Total API bandwidth           | ~5 MB/s normal                                        | ~200 MB/s peak|

---

## 3. High-Level Architecture

```
                      ┌─────────────────────────────────────────────────────────────┐
                      │                      CLIENT LAYER                            │
                      │    Web UI  /  CLI (spinnaker-cli)  /  CI Bot (GitHub Actions)│
                      └──────────────────────────┬──────────────────────────────────┘
                                                 │ HTTPS / gRPC
                      ┌──────────────────────────▼──────────────────────────────────┐
                      │                API Gateway + Auth (mTLS for CI bots)         │
                      └──┬─────────────────────┬──────────────────────┬─────────────┘
                         │                     │                      │
          ┌──────────────▼──┐  ┌───────────────▼──────────┐  ┌───────▼──────────────┐
          │  Deployment      │  │  Pipeline Config Service  │  │  Artifact Registry   │
          │  Orchestrator    │  │  (manage pipelines,       │  │  Proxy (image meta,  │
          │  (core deploy    │  │  environments, policies)  │  │  version catalog,    │
          │  lifecycle)      │  └──────────────────────────┘  │  promotion rules)    │
          └──────────┬───────┘                                └──────────────────────┘
                     │
          ┌──────────▼──────────────────────────────────────────────────────────────┐
          │                       Deployment Engine                                  │
          │  ┌──────────────┐  ┌──────────────┐  ┌────────────┐  ┌───────────────┐ │
          │  │ Blue-Green   │  │  Canary      │  │  Rolling   │  │  Deployment   │ │
          │  │ Strategy     │  │  Strategy    │  │  Strategy  │  │  Lock Manager │ │
          │  └──────────────┘  └──────────────┘  └────────────┘  └───────────────┘ │
          └──────────┬──────────────────────────────────────────────────────────────┘
                     │
          ┌──────────▼──────────────────────────────────────────────────────────────┐
          │                      Integration Layer                                   │
          │  ┌──────────────┐  ┌──────────────────┐  ┌────────────────────────────┐ │
          │  │  Kubernetes  │  │  Health Check     │  │  Traffic Controller        │ │
          │  │  Client      │  │  Engine           │  │  (Istio / ALB weight       │ │
          │  │  (kubectl    │  │  (HTTP probes,    │  │  rules, feature flags)     │ │
          │  │  API)        │  │  synthetic txns)  │  └────────────────────────────┘ │
          │  └──────────────┘  └──────────────────┘                                 │
          └──────────┬──────────────────────────────────────────────────────────────┘
                     │
          ┌──────────▼──────────────────────────────────────────────────────────────┐
          │                          Data Layer                                      │
          │  ┌──────────────┐  ┌──────────────┐  ┌────────────┐  ┌───────────────┐ │
          │  │  PostgreSQL  │  │  Redis       │  │  Kafka     │  │  S3           │ │
          │  │  (deploy     │  │  (locks,     │  │  (deploy   │  │  (audit logs, │ │
          │  │  records,    │  │  status      │  │  events,   │  │  pipeline     │ │
          │  │  pipelines,  │  │  cache,      │  │  notifs)   │  │  configs,     │ │
          │  │  audit log)  │  │  rate limits)│  │            │  │  artifacts)   │ │
          │  └──────────────┘  └──────────────┘  └────────────┘  └───────────────┘ │
          └─────────────────────────────────────────────────────────────────────────┘
                     │
          ┌──────────▼───────────────────────────────────────────────────────────────┐
          │                     Target Environments                                   │
          │   K8s Cluster: us-east-1  │  K8s Cluster: eu-west-1  │  K8s: ap-south-1 │
          └──────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

- **API Gateway**: Entry point for all deployment requests. Validates JWT/mTLS, applies per-service rate limits, routes to Deployment Orchestrator.
- **Deployment Orchestrator**: The central coordinator. Receives deployment requests, acquires deployment locks, selects the rollout strategy, coordinates the Deployment Engine, and manages the deployment state machine (PENDING → IN_PROGRESS → HEALTH_CHECK → SUCCEEDED/FAILED/ROLLED_BACK).
- **Pipeline Config Service**: CRUD for deployment pipelines and their stages. Manages environment configurations, promotion gates, approvers, and rollback policies. Stores config in PostgreSQL; caches in Redis.
- **Artifact Registry Proxy**: Validates artifact existence and version, stores artifact metadata (image digest, build timestamp, CI run ID, Git commit SHA), and enforces promotion rules (e.g., "only artifacts that passed staging can deploy to production").
- **Deployment Engine**: Strategy-specific deployment logic. Implements blue-green (swap K8s Services), canary (traffic weight manipulation via Istio VirtualService), rolling update (K8s RollingUpdate), and recreate (delete then create) strategies.
- **Deployment Lock Manager**: Distributed lock per (service, environment) using Redis SETNX. Prevents concurrent deployments.
- **Kubernetes Client**: Wrapper around the K8s API. Applies manifests, reads pod status, scales deployments, manages Services and Ingress rules.
- **Health Check Engine**: Executes configurable health checks: HTTP readiness probes, synthetic transaction scripts, database migration status checks. Reports pass/fail to the Orchestrator.
- **Traffic Controller**: Manages traffic weight rules. Integrates with Istio VirtualService (canary traffic splits), AWS ALB target group weights, or feature flag APIs.
- **PostgreSQL**: Stores deployment records, pipeline configurations, audit logs, and approval records.
- **Redis**: Distributed deployment locks (SETNX + TTL), deployment status cache, rate limit counters.
- **Kafka**: Publishes deployment lifecycle events (triggered, promoted, rolled-back, succeeded) for consumption by notification services, metrics systems, and audit pipelines.
- **S3**: Stores pipeline configuration versions (GitOps), deployment manifest snapshots (for rollback), and audit log archives.

**Primary Data Flow (Canary Deployment):**

1. CI bot calls `POST /deployments` with service=payments-api, environment=production, artifact=v1.2.3.
2. Orchestrator validates request → acquires deployment lock for (payments-api, production) → creates deployment record (PENDING).
3. Orchestrator instantiates Canary Strategy. Calls K8s Client to deploy new version pods with `deployment: payments-api-canary` (10% of replica count).
4. Traffic Controller sets Istio VirtualService weights: 90% → stable pods, 10% → canary pods.
5. Health Check Engine runs checks every 30 seconds for 10 minutes. Monitors error rate via Datadog API.
6. If checks pass: Orchestrator progresses through traffic ramp (10% → 25% → 50% → 100% over configured intervals).
7. At 100%: K8s Client scales down old (stable) pods. Deployment status → SUCCEEDED.
8. On any health check failure: Rollback procedure — set canary traffic to 0%, delete canary pods, deployment status → ROLLED_BACK. Notification sent.
9. Throughout: all state transitions published to Kafka → Notification Service → Slack/email.

---

## 4. Data Model

### Entities & Schema

```sql
-- Services (the unit of deployment)
CREATE TABLE services (
    service_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            VARCHAR(200) UNIQUE NOT NULL,    -- 'payments-api', 'user-service'
    team_id         UUID NOT NULL,
    description     TEXT,
    repo_url        VARCHAR(500),
    default_strategy VARCHAR(20) DEFAULT 'ROLLING',
                    -- ROLLING, BLUE_GREEN, CANARY, RECREATE
    k8s_namespace   VARCHAR(100) NOT NULL,
    k8s_deployment  VARCHAR(200) NOT NULL,           -- K8s Deployment name
    is_active       BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Environments (dev, staging, production per service)
CREATE TABLE environments (
    env_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    service_id      UUID NOT NULL REFERENCES services(service_id),
    name            VARCHAR(50) NOT NULL,             -- dev, staging, production
    k8s_cluster     VARCHAR(200) NOT NULL,            -- cluster name or API endpoint
    k8s_namespace   VARCHAR(100) NOT NULL,
    region          VARCHAR(50) NOT NULL,
    is_production   BOOLEAN NOT NULL DEFAULT FALSE,
    require_approval BOOLEAN NOT NULL DEFAULT FALSE,  -- manual gate
    auto_rollback   BOOLEAN NOT NULL DEFAULT TRUE,
    rollback_versions INT NOT NULL DEFAULT 5,         -- keep last N stable versions
    UNIQUE (service_id, name),
    INDEX idx_env_service (service_id)
);

-- Pipeline Stages Configuration
CREATE TABLE pipeline_stages (
    stage_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    env_id          UUID NOT NULL REFERENCES environments(env_id),
    stage_type      VARCHAR(50) NOT NULL,
                    -- HEALTH_CHECK, MANUAL_APPROVAL, WAIT, METRIC_CHECK, SCRIPT
    stage_order     SMALLINT NOT NULL,
    config          JSONB NOT NULL,
                    -- {"url": "http://svc/health", "expected_status": 200, "timeout_s": 30}
                    -- {"metric": "error_rate", "threshold": 0.01, "window_s": 300}
                    -- {"wait_seconds": 600}
    INDEX idx_stages_env (env_id, stage_order)
);

-- Artifacts
CREATE TABLE artifacts (
    artifact_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    service_id      UUID NOT NULL REFERENCES services(service_id),
    version         VARCHAR(100) NOT NULL,           -- 'v1.2.3' or '1.2.3-abc1234'
    image_uri       VARCHAR(500) NOT NULL,           -- 'registry.company.com/payments:v1.2.3'
    image_digest    VARCHAR(100) NOT NULL,           -- SHA256 digest for immutability
    git_commit_sha  VARCHAR(40) NOT NULL,
    git_branch      VARCHAR(200),
    ci_run_id       VARCHAR(200),
    build_at        TIMESTAMPTZ NOT NULL,
    promoted_to     VARCHAR(50)[],                   -- environments this artifact has been promoted to
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (service_id, version),
    INDEX idx_artifacts_service (service_id, created_at DESC)
);

-- Deployments (the central record)
CREATE TABLE deployments (
    deployment_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    service_id      UUID NOT NULL REFERENCES services(service_id),
    env_id          UUID NOT NULL REFERENCES environments(env_id),
    artifact_id     UUID NOT NULL REFERENCES artifacts(artifact_id),
    strategy        VARCHAR(20) NOT NULL,            -- ROLLING, BLUE_GREEN, CANARY, RECREATE
    status          VARCHAR(20) NOT NULL DEFAULT 'PENDING',
                    -- PENDING, IN_PROGRESS, HEALTH_CHECK, AWAITING_APPROVAL,
                    -- SUCCEEDED, FAILED, ROLLED_BACK, CANCELLED
    triggered_by    UUID NOT NULL,                   -- user_id or service account ID
    trigger_source  VARCHAR(50) NOT NULL,            -- CI_BOT, MANUAL_UI, API, ROLLBACK
    previous_artifact_id UUID REFERENCES artifacts(artifact_id),  -- for rollback
    started_at      TIMESTAMPTZ,
    completed_at    TIMESTAMPTZ,
    current_stage   INT NOT NULL DEFAULT 0,
    canary_weight   SMALLINT,                        -- current canary traffic % (null if not canary)
    rollback_reason TEXT,
    manifest_snapshot JSONB,                         -- K8s manifest at time of deploy (for exact rollback)
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_deploy_service_env (service_id, env_id, created_at DESC),
    INDEX idx_deploy_status (status),
    INDEX idx_deploy_triggered_by (triggered_by)
);

-- Deployment Stage Results (outcomes of each pipeline stage)
CREATE TABLE deployment_stage_results (
    id              BIGSERIAL PRIMARY KEY,
    deployment_id   UUID NOT NULL REFERENCES deployments(deployment_id),
    stage_id        UUID NOT NULL REFERENCES pipeline_stages(stage_id),
    stage_order     SMALLINT NOT NULL,
    status          VARCHAR(20) NOT NULL,            -- RUNNING, PASSED, FAILED, SKIPPED
    started_at      TIMESTAMPTZ NOT NULL,
    completed_at    TIMESTAMPTZ,
    output          JSONB,                           -- health check response, metric values, etc.
    INDEX idx_stage_results_deploy (deployment_id, stage_order)
);

-- Deployment Locks (also managed in Redis; DB is authoritative record)
CREATE TABLE deployment_locks (
    lock_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    service_id      UUID NOT NULL REFERENCES services(service_id),
    env_id          UUID NOT NULL REFERENCES environments(env_id),
    deployment_id   UUID NOT NULL REFERENCES deployments(deployment_id),
    acquired_by     UUID NOT NULL,
    acquired_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at      TIMESTAMPTZ NOT NULL,            -- max deployment TTL (1 hour default)
    UNIQUE (service_id, env_id)                      -- only one lock per service+env
);

-- Audit Log (immutable append-only)
CREATE TABLE audit_log (
    log_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    deployment_id   UUID REFERENCES deployments(deployment_id),
    actor_id        UUID NOT NULL,
    actor_type      VARCHAR(20) NOT NULL,            -- USER, CI_BOT, SYSTEM
    action          VARCHAR(50) NOT NULL,
                    -- DEPLOYMENT_TRIGGERED, STAGE_PASSED, STAGE_FAILED,
                    -- APPROVAL_GRANTED, APPROVAL_DENIED, ROLLBACK_INITIATED,
                    -- ROLLBACK_COMPLETED, LOCK_ACQUIRED, LOCK_RELEASED,
                    -- DEPLOYMENT_CANCELLED
    details         JSONB,
    ip_address      INET,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
    -- No UPDATE or DELETE ever on this table
);

-- Approval Requests (for manual gates)
CREATE TABLE approval_requests (
    approval_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    deployment_id   UUID NOT NULL REFERENCES deployments(deployment_id),
    stage_id        UUID NOT NULL REFERENCES pipeline_stages(stage_id),
    status          VARCHAR(20) NOT NULL DEFAULT 'PENDING',
                    -- PENDING, APPROVED, DENIED, TIMED_OUT
    requested_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    responded_at    TIMESTAMPTZ,
    responder_id    UUID,
    responder_note  TEXT,
    expires_at      TIMESTAMPTZ NOT NULL,            -- auto-reject after deadline
    INDEX idx_approvals_deploy (deployment_id)
);
```

### Database Choice

**Options Considered:**

| Database     | Pros                                                                   | Cons                                                              |
|--------------|------------------------------------------------------------------------|-------------------------------------------------------------------|
| PostgreSQL   | ACID, complex queries, JSON support, audit-log immutability            | Single-node write ceiling (not a concern at our write volume)     |
| MySQL        | ACID, row-level locking                                                | Less powerful query planner; no native array types                |
| MongoDB      | Flexible schema for JSONB pipeline configs                             | Eventual consistency; overkill for deployment metadata            |
| CockroachDB  | Distributed ACID, horizontal scale                                     | Operational overhead not needed at this write volume              |

**Selected: PostgreSQL**

Justification:
1. Deployment records and audit logs demand ACID guarantees. The audit log table must be append-only with no updates — PostgreSQL table-level `GRANT INSERT` only (no UPDATE/DELETE) enforces this at the database permission level.
2. Write volume is tiny (< 200 RPS even at peak). Single PostgreSQL primary easily handles this.
3. Pipeline configurations stored as JSONB allow flexible schema evolution (new stage types, new config fields) without migrations.
4. The `deployment_locks` table uses a `UNIQUE (service_id, env_id)` constraint — a database-level uniqueness guarantee in addition to the Redis lock, providing defense in depth.
5. Complex audit queries (e.g., "all deployments by user X to production in the last 30 days, with rollbacks") are efficient with PostgreSQL's query planner and partial indexes.
6. Redis is used alongside PostgreSQL specifically for lock acquisition speed and deployment status caching — not as a replacement.

---

## 5. API Design

```
BASE URL: https://api.deploy.company.internal/v1
Auth: JWT (human users), mTLS client certificates (CI bots)
```

### Services & Environments

```
GET /services
  Auth: Required
  Query: team_id, page, limit
  Response 200: { "items": [{ "service_id", "name", "team_id", "default_strategy" }] }

GET /services/{service_id}
  Response 200: full service + environments + recent deployments (last 5)

POST /services
  Auth: Required (platform admin or team lead)
  Body: { "name", "team_id", "k8s_namespace", "k8s_deployment", "default_strategy" }
  Response 201: { "service_id", ... }

GET /services/{service_id}/environments
  Response 200: { "items": [{ "env_id", "name", "region", "is_production" }] }
```

### Deployments

```
POST /deployments
  Auth: Required
  Rate limit: 10 req/min/service (prevents runaway CI loops)
  Body:
  {
    "service_id": "uuid",
    "environment": "production",
    "artifact_version": "v1.2.3",
    "strategy": "CANARY",               -- optional; overrides service default
    "force": false,                     -- override deployment lock (SRE only)
    "canary_config": {                  -- optional; canary-specific overrides
      "initial_weight": 10,
      "ramp_steps": [10, 25, 50, 100],
      "ramp_interval_seconds": 600
    }
  }
  Response 202:
  {
    "deployment_id": "uuid",
    "status": "PENDING",
    "artifact": { "version": "v1.2.3", "image_uri": "..." },
    "pipeline_url": "https://deploy.company.internal/ui/deploys/uuid"
  }
  Error 409: { "error": "DEPLOYMENT_LOCK_HELD", "held_by_deployment": "uuid", "acquired_at": "ISO8601" }
  Error 404: { "error": "ARTIFACT_NOT_FOUND", "version": "v1.2.3" }
  Error 400: { "error": "ARTIFACT_NOT_PROMOTED", "message": "v1.2.3 has not been deployed to staging" }

GET /deployments/{deployment_id}
  Response 200:
  {
    "deployment_id", "service_id", "environment", "artifact": { "version", "image_uri", "git_commit_sha" },
    "strategy", "status", "current_stage",
    "canary_weight": 25,               -- null if not canary
    "stages": [{
      "stage_type", "status", "started_at", "completed_at",
      "output": { "http_status": 200, "response_time_ms": 45 }
    }],
    "triggered_by", "trigger_source", "started_at", "completed_at",
    "timeline": [{ "timestamp", "status", "message" }]
  }

GET /deployments
  Query: service_id, environment, status, triggered_by, from, to, page, limit
  Response 200: { "items": [...deployments], "pagination": {...} }

POST /deployments/{deployment_id}/promote
  Auth: Required (for manual approval gates)
  Body: { "note": "Looks good, metrics nominal" }
  Response 200: { "deployment_id", "status": "IN_PROGRESS" }
  Error 400: { "error": "NOT_AWAITING_APPROVAL" }

POST /deployments/{deployment_id}/rollback
  Auth: Required
  Body: { "reason": "Elevated error rate detected", "target_version": "v1.2.2" }
  Response 202: { "deployment_id": "new_rollback_deployment_id", "status": "PENDING" }

POST /deployments/{deployment_id}/cancel
  Auth: Required (must be triggering user or SRE)
  Response 200: { "deployment_id", "status": "CANCELLED" }

GET /deployments/{deployment_id}/logs
  Response 200:
  {
    "deployment_id",
    "logs": [{
      "timestamp", "level", "stage", "message"
    }]
  }
```

### Locks

```
GET /locks
  Auth: Required (SRE role)
  Query: service_id, environment
  Response 200: { "items": [{ "lock_id", "service_id", "environment", "deployment_id", "acquired_by", "acquired_at", "expires_at" }] }

DELETE /locks/{lock_id}
  Auth: Required (SRE role only — emergency use)
  Body: { "reason": "Stale lock after pod crash" }
  Response 204
  Note: Triggers audit log entry; notifies the owner of the cancelled deployment
```

### Artifacts

```
GET /artifacts
  Query: service_id, branch, limit (default 20)
  Response 200: { "items": [{ "artifact_id", "version", "image_uri", "git_commit_sha", "build_at", "promoted_to" }] }

POST /artifacts
  Auth: Required (CI bot only — mTLS)
  Body:
  {
    "service_id": "uuid",
    "version": "v1.2.3",
    "image_uri": "registry.company.com/payments:v1.2.3",
    "image_digest": "sha256:abc123...",
    "git_commit_sha": "abc1234",
    "git_branch": "main",
    "ci_run_id": "github_run_12345"
  }
  Response 201: { "artifact_id", "version" }
```

### Webhooks & Notifications

```
POST /webhooks
  Auth: Required
  Body: { "service_id", "url", "events": ["DEPLOYMENT_SUCCEEDED", "ROLLBACK_INITIATED"] }
  Response 201: { "webhook_id", "url", "events" }

-- Outbound webhook payload (delivered to subscriber URL):
{
  "event": "ROLLBACK_INITIATED",
  "deployment_id": "uuid",
  "service": "payments-api",
  "environment": "production",
  "artifact_version": "v1.2.3",
  "rolled_back_to": "v1.2.2",
  "reason": "Error rate exceeded 1% threshold",
  "timestamp": "ISO8601"
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Deployment Lock Manager

**Problem it solves:**
If two CI pipeline runs trigger simultaneously for the same service and environment (e.g., two feature branches merged to main within seconds), both could start deploying concurrently. This would corrupt Kubernetes state — both deployments would modify the same Deployment object, creating an unpredictable mix of pod versions. The lock manager must guarantee mutual exclusion with a timeout (no deployment runs forever).

**Approaches Comparison:**

| Approach                         | Mechanism                                          | Pros                                    | Cons                                                          |
|----------------------------------|----------------------------------------------------|-----------------------------------------|---------------------------------------------------------------|
| Redis SETNX with TTL             | Atomic SET if not exists; auto-expires             | Millisecond acquisition; simple         | Lock lost on Redis restart without persistence                |
| PostgreSQL advisory lock         | `pg_try_advisory_lock(hash(service+env))`          | Transactional; no separate infra        | Lock held by DB session; session death = lock release (good!) |
| ZooKeeper ephemeral nodes        | Create ephemeral znode; deleted on client disconnect | Strong consistency; auto-release on crash | Heavy infra; ZooKeeper is complex to operate                |
| Etcd distributed lock            | Etcd lease with TTL                                | Strong consistency; battle-tested in K8s | External infra; slightly higher latency than Redis            |
| DB unique constraint             | INSERT into deployment_locks table                  | ACID; audit trail                       | Lock check = DB round trip; no auto-expiry (need cleanup job) |

**Selected: Redis SETNX (primary, fast path) + PostgreSQL unique constraint (durability + audit)**

Two-layer design:
- **Redis SETNX** provides the fast lock for the deployment hot path.
- **PostgreSQL unique constraint** on `deployment_locks(service_id, env_id)` provides a durable record and is the source of truth if Redis is unavailable.

```python
LOCK_TTL_SECONDS = 3600  # 1 hour max deployment duration

def acquire_deployment_lock(service_id: str, env_id: str, deployment_id: str,
                            acquired_by: str) -> bool:
    lock_key = f"deploy_lock:{service_id}:{env_id}"
    lock_value = str(deployment_id)

    # Step 1: Try Redis SETNX
    acquired = redis.set(lock_key, lock_value, nx=True, ex=LOCK_TTL_SECONDS)
    if not acquired:
        existing = redis.get(lock_key)
        raise LockHeldError(deployment_id=existing)

    # Step 2: Write durable lock record to PostgreSQL
    # If this fails (another deployment acquired the DB lock between steps),
    # release the Redis lock and raise an error
    try:
        with db.transaction():
            db.execute(
                """INSERT INTO deployment_locks
                   (service_id, env_id, deployment_id, acquired_by, acquired_at, expires_at)
                   VALUES ($1, $2, $3, $4, now(), now() + interval '1 hour')""",
                [service_id, env_id, deployment_id, acquired_by]
            )
    except UniqueConstraintViolation:
        redis.delete(lock_key)  # Release Redis lock
        raise LockHeldError("Lock exists in database")

    # Step 3: Write audit log
    write_audit_log(deployment_id=deployment_id, action='LOCK_ACQUIRED',
                    actor_id=acquired_by, details={"lock_key": lock_key})
    return True

def release_deployment_lock(service_id: str, env_id: str, deployment_id: str):
    lock_key = f"deploy_lock:{service_id}:{env_id}"
    current_holder = redis.get(lock_key)

    # Verify we own the lock before releasing (prevent releasing another deploy's lock)
    if current_holder != deployment_id:
        log.warn(f"Lock release mismatch: expected {deployment_id}, found {current_holder}")
        # Still attempt DB cleanup
    else:
        redis.delete(lock_key)

    with db.transaction():
        db.execute(
            "DELETE FROM deployment_locks WHERE service_id = $1 AND env_id = $2 AND deployment_id = $3",
            [service_id, env_id, deployment_id]
        )

    write_audit_log(deployment_id=deployment_id, action='LOCK_RELEASED',
                    actor_id='SYSTEM', details={})

def check_and_recover_stale_locks():
    """Runs every 5 minutes as a background job."""
    stale_locks = db.execute(
        "SELECT * FROM deployment_locks WHERE expires_at < now()"
    )
    for lock in stale_locks:
        # Mark the deployment as FAILED (timed out)
        db.execute(
            """UPDATE deployments SET status = 'FAILED',
               rollback_reason = 'Deployment lock expired (timeout)',
               completed_at = now()
               WHERE deployment_id = $1""",
            [lock.deployment_id]
        )
        # Release lock
        release_deployment_lock(lock.service_id, lock.env_id, lock.deployment_id)
        # Trigger rollback if the deployment got partially through
        trigger_automatic_rollback(lock.deployment_id, reason="Lock timeout")
        # Notify engineer
        publish_to_kafka("deployment.timed_out", {"deployment_id": str(lock.deployment_id)})
```

**Interviewer Q&A:**

Q1: What happens if the Deployment Orchestrator pod crashes while holding the lock?
A: The Redis lock has a 1-hour TTL — it auto-expires. The PostgreSQL `deployment_locks` record's `expires_at` column is also set to 1 hour from acquisition. The stale lock recovery job runs every 5 minutes and will detect the expired lock (via `expires_at < now()`), mark the deployment as FAILED, attempt rollback if needed, and release the lock. Mean time to lock recovery after pod crash: ≤ 5 minutes.

Q2: Why store the lock in both Redis and PostgreSQL instead of just one?
A: Redis provides speed (millisecond lock acquisition), TTL (auto-expiry without a cleanup job in the critical path), and low latency for the hot path. PostgreSQL provides durability and auditability — if Redis loses data on restart, the PostgreSQL record is authoritative. The stale lock recovery job queries PostgreSQL. The UI showing "what's currently locked" queries PostgreSQL (always consistent). The deployment pipeline queries Redis (always fast). Defense in depth: if either system has a bug, the other provides a safety net.

Q3: How do you handle the scenario where an SRE needs to forcefully acquire a lock held by a stuck deployment?
A: The `DELETE /locks/{lock_id}` API (SRE-only) performs: (1) Mark the holding deployment as CANCELLED in PostgreSQL. (2) Delete the PostgreSQL lock record. (3) Delete the Redis lock key. (4) Publish `deployment.lock_force_released` Kafka event. (5) Write audit log with the SRE's identity and reason. The SRE must provide a reason. An alert is sent to the deployment owner. The force-release creates a full audit trail for post-incident review.

Q4: How does the lock interact with a rollback? Does the rollback deployment need to acquire a new lock?
A: Yes, a rollback creates a new deployment record with `trigger_source = ROLLBACK`. The rollback flow first releases the failed deployment's lock, then acquires a new lock for the rollback deployment. The acquisition is given higher priority (SRE can force if needed). In practice, the Orchestrator performs this atomically: release old lock + acquire new lock in a single `with db.transaction()` block to prevent a window where both locks are released.

Q5: How would you implement a "queue next deployment" feature instead of rejecting when a lock is held?
A: Add a `deployment_queue` table: `(service_id, env_id, deployment_id, queued_at, position)`. When a lock is held, new deployments enter the queue instead of returning 409. When the current deployment completes and releases the lock, a Kafka `deployment.lock_released` event triggers the Deployment Orchestrator to dequeue the next entry and start it. Queue depth is monitored — if a queue exceeds 3 deployments, an alert fires (engineers should investigate why deployments are piling up).

---

### 6.2 Rollout Strategies

**Problem it solves:**
Different services have different risk profiles and traffic patterns. A zero-traffic internal tool can use Recreate (downtime acceptable). A user-facing API needs Blue-Green (instant switchover). A payment service needs Canary (gradual ramp with real-traffic validation). The system must implement all strategies with a uniform interface, health-check integration, and rollback capability.

**Approaches Comparison:**

| Strategy       | Mechanism                                           | Zero Downtime | Rollback Speed | Resource Cost     | Best For                         |
|----------------|-----------------------------------------------------|---------------|----------------|-------------------|----------------------------------|
| Recreate       | Delete all old pods, create new pods                | No            | ~2 min (redeploy old version) | Normal | Dev/batch jobs; downtime OK |
| Rolling Update | Gradually replace pods; configurable max_surge      | Yes           | ~2-5 min       | +max_surge % resources | Most services; simple rollback |
| Blue-Green     | Full new stack deployed; traffic switch via Service | Yes           | Instant (< 1s) | 2x resources temporarily | User-facing APIs; instant rollback |
| Canary         | Small % of traffic to new version; gradual ramp     | Yes           | Instant (weight to 0%) | +canary_replica_count | High-stakes services; real traffic validation |

**Selected: All four strategies implemented; service owners configure their preference**

**Canary Strategy — detailed implementation:**

```python
class CanaryDeploymentStrategy:
    def __init__(self, k8s_client, traffic_controller, health_check_engine):
        self.k8s = k8s_client
        self.traffic = traffic_controller
        self.health = health_check_engine

    def execute(self, deployment: Deployment, config: CanaryConfig) -> DeploymentResult:
        service_name = deployment.service.k8s_deployment
        canary_name = f"{service_name}-canary"
        stable_name = service_name

        try:
            # Phase 1: Deploy canary replicas (10% of total pod count)
            total_replicas = self.k8s.get_deployment_replicas(stable_name)
            canary_replicas = max(1, int(total_replicas * 0.1))

            self.k8s.create_or_update_deployment(
                name=canary_name,
                image=deployment.artifact.image_uri,
                replicas=canary_replicas,
                labels={
                    "app": service_name,
                    "version": deployment.artifact.version,
                    "track": "canary"
                }
            )

            # Wait for canary pods to be ready
            self.k8s.wait_for_rollout(canary_name, timeout_seconds=300)
            self.emit_event(deployment, f"Canary pods ready ({canary_replicas} replicas)")

            # Phase 2: Route initial traffic to canary
            initial_weight = config.initial_weight  # e.g., 10
            self.traffic.set_weights(
                service=service_name,
                stable_weight=100 - initial_weight,
                canary_weight=initial_weight
            )
            self.update_deployment_status(deployment, canary_weight=initial_weight)
            self.emit_event(deployment, f"Traffic: {initial_weight}% canary")

            # Phase 3: Health check and progressive ramp
            for target_weight in config.ramp_steps:  # e.g., [10, 25, 50, 100]
                if target_weight == initial_weight:
                    # Initial weight already set, just run health checks
                    pass
                else:
                    # Wait for ramp interval
                    self.wait_or_abort(config.ramp_interval_seconds, deployment)

                    # Check health at current weight
                    check_result = self.health.run_checks(
                        deployment=deployment,
                        duration_seconds=config.ramp_interval_seconds,
                        error_rate_threshold=config.error_rate_threshold  # e.g., 0.01 = 1%
                    )

                    if not check_result.passed:
                        raise HealthCheckFailure(check_result.failure_reason)

                    # Advance to next traffic weight
                    self.traffic.set_weights(
                        service=service_name,
                        stable_weight=100 - target_weight,
                        canary_weight=target_weight
                    )
                    self.update_deployment_status(deployment, canary_weight=target_weight)
                    self.emit_event(deployment, f"Traffic: {target_weight}% canary")

            # Phase 4: Promote canary to stable
            # Update the main deployment to the new image
            self.k8s.update_deployment_image(stable_name, deployment.artifact.image_uri)
            self.k8s.wait_for_rollout(stable_name, timeout_seconds=600)

            # Remove canary traffic split (stable now has new version)
            self.traffic.set_weights(service=service_name, stable_weight=100, canary_weight=0)

            # Delete canary deployment
            self.k8s.delete_deployment(canary_name)

            return DeploymentResult(status='SUCCEEDED')

        except (HealthCheckFailure, K8sError, TimeoutError) as e:
            return self.rollback(deployment, canary_name, stable_name, str(e))

    def rollback(self, deployment, canary_name, stable_name, reason: str) -> DeploymentResult:
        self.emit_event(deployment, f"ROLLING BACK: {reason}")

        # Immediately route all traffic to stable (old) version
        self.traffic.set_weights(service=stable_name, stable_weight=100, canary_weight=0)
        self.emit_event(deployment, "Traffic: 100% stable (old version)")

        # Delete canary pods
        try:
            self.k8s.delete_deployment(canary_name)
        except K8sError as e:
            log.error(f"Failed to delete canary deployment: {e}")

        # Mark deployment as rolled back
        db.execute(
            "UPDATE deployments SET status = 'ROLLED_BACK', rollback_reason = $1, completed_at = now() WHERE deployment_id = $2",
            [reason, deployment.deployment_id]
        )
        return DeploymentResult(status='ROLLED_BACK', reason=reason)
```

**Blue-Green Strategy:**
```python
class BlueGreenDeploymentStrategy:
    def execute(self, deployment: Deployment) -> DeploymentResult:
        service_name = deployment.service.k8s_deployment
        current_color = self.get_active_color(service_name)  # 'blue' or 'green'
        new_color = 'green' if current_color == 'blue' else 'blue'
        new_deployment_name = f"{service_name}-{new_color}"

        # Deploy full new version (same replica count as current)
        replicas = self.k8s.get_deployment_replicas(f"{service_name}-{current_color}")
        self.k8s.create_or_update_deployment(
            name=new_deployment_name,
            image=deployment.artifact.image_uri,
            replicas=replicas,
            labels={"app": service_name, "color": new_color}
        )

        # Wait for all new pods ready
        self.k8s.wait_for_rollout(new_deployment_name, timeout_seconds=600)

        # Run pre-traffic health checks against new pods (direct pod IP, no service)
        check_result = self.health.run_checks(deployment, target="new_pods")
        if not check_result.passed:
            self.k8s.delete_deployment(new_deployment_name)
            return DeploymentResult(status='FAILED', reason=check_result.failure_reason)

        # Atomic traffic switch: update K8s Service selector
        # This is atomic from Kubernetes' perspective — iptables rules update within ~1s
        self.k8s.update_service_selector(
            service=service_name,
            selector={"app": service_name, "color": new_color}
        )

        # Post-traffic health checks
        time.sleep(30)  # wait for traffic to fully shift
        post_check = self.health.run_checks(deployment, duration_seconds=120)
        if not post_check.passed:
            # Instant rollback: swap selector back to old color
            self.k8s.update_service_selector(
                service=service_name,
                selector={"app": service_name, "color": current_color}
            )
            self.k8s.delete_deployment(new_deployment_name)
            return DeploymentResult(status='ROLLED_BACK', reason=post_check.failure_reason)

        # Cleanup old deployment (keep for 10 minutes for instant rollback window)
        schedule_cleanup(f"{service_name}-{current_color}", delay_minutes=10)
        return DeploymentResult(status='SUCCEEDED', active_color=new_color)
```

**Interviewer Q&A:**

Q1: In blue-green deployment, during the 10-minute window before old pods are deleted, the system uses 2x resources. How do you justify this cost?
A: 10-minute 2x resource overhead for a production deployment is the correct trade-off for zero-downtime instant rollback capability. If a post-deploy issue is detected (error spike at 5 minutes), the selector flip is instantaneous — no pods to spin up. After 10 minutes, the risk of needing to roll back decreases significantly. Cost analysis: if a production issue costs $100K/minute in lost revenue, spending extra on compute for 10 minutes per deployment is trivially justified. The old pods can also be suspended (scaled to 0) rather than deleted if cost is a concern, maintaining instant scale-up-and-rollback capability at near-zero compute cost.

Q2: How does the Traffic Controller integrate with both Istio (service mesh) and AWS ALB (no mesh)?
A: The Traffic Controller is an abstraction layer with pluggable backends. The `set_weights(service, stable_weight, canary_weight)` interface is implemented by: (1) `IstioBackend`: patches the Istio VirtualService YAML to update `weight` fields under `spec.http[].route`. (2) `ALBBackend`: updates target group weights via the AWS ELBv2 API. (3) `NginxIngressBackend`: updates Nginx Ingress annotations (`nginx.ingress.kubernetes.io/canary-weight`). The deployment pipeline config specifies which backend to use per environment. New backends can be added without changing the Deployment Engine.

Q3: How do you implement the automatic rollback trigger based on error rate?
A: The Health Check Engine polls a metrics API (Datadog/Prometheus) every 30 seconds during the post-traffic monitoring window. It executes a query like: `sum(rate(http_requests_total{service="payments-api",status=~"5.."}[5m])) / sum(rate(http_requests_total{service="payments-api"}[5m]))`. If the result exceeds the configured threshold (e.g., 1%) for 2 consecutive checks (to avoid flapping on transient spikes), it sets `check_result.passed = False` with reason "Error rate 2.3% exceeds threshold 1.0%". The Canary strategy catches this `HealthCheckFailure` exception and calls `rollback()`.

Q4: What is the rollback behavior for a rolling update strategy mid-deployment (50% pods updated, 50% old)?
A: Kubernetes' native rollback (`kubectl rollout undo`) reverts the Deployment's pod template to the previous version. Kubernetes then performs a rolling update in reverse — replacing new-version pods with old-version pods using the same `maxSurge/maxUnavailable` settings. The Deployment Orchestrator calls `self.k8s.rollback_deployment(name)` which issues `kubectl rollout undo`. K8s handles the mechanics. The rollback is complete when `kubectl rollout status` reports success. For Rolling strategy, the Orchestrator polls rollout status every 10 seconds.

Q5: How do you handle database migrations that accompany a code deployment?
A: Database migrations require the "expand-contract" (backward-compatible migration) pattern: (1) **Expand phase** (in v1.2.3): add a new column with a default value. Both old and new code must work with both old and new schema. The migration runs before pods are deployed. (2) **Contract phase** (in v1.3.0, a later deployment): remove the old column after all instances run the new code. The pipeline for a migration deployment has an additional stage type: `MIGRATION` that runs `flyway migrate` or `liquibase update` as a K8s Job before the new pods start. The health check stage verifies migration success before routing any traffic.

---

### 6.3 Health Check Engine

**Problem it solves:**
A deployment may succeed at the Kubernetes level (all pods running, readiness probes green) but fail at the application level (silent configuration error, downstream dependency unavailable, elevated latency). The Health Check Engine validates the new version under production traffic before committing to full rollout, and triggers rollback if validation fails.

**Health Check Types and Implementation:**

```python
class HealthCheckEngine:

    def run_checks(self, deployment: Deployment, duration_seconds: int = 300,
                   error_rate_threshold: float = 0.01, target: str = "service") -> CheckResult:
        """Run all configured health checks for a deployment pipeline stage."""
        pipeline = get_pipeline_stages(deployment.env_id)
        health_stages = [s for s in pipeline if s.stage_type == 'HEALTH_CHECK']

        results = []
        for stage in health_stages:
            result = self.execute_stage(stage, deployment)
            results.append(result)

            # Write result to DB
            db.execute(
                """INSERT INTO deployment_stage_results
                   (deployment_id, stage_id, stage_order, status, started_at, completed_at, output)
                   VALUES ($1,$2,$3,$4,$5,$6,$7)""",
                [deployment.deployment_id, stage.stage_id, stage.stage_order,
                 'PASSED' if result.passed else 'FAILED',
                 result.started_at, result.completed_at, json.dumps(result.output)]
            )

            if not result.passed:
                return CheckResult(passed=False, failure_reason=result.failure_reason,
                                   failed_stage=stage)

        return CheckResult(passed=True)

    def execute_stage(self, stage: PipelineStage, deployment: Deployment) -> StageResult:
        started_at = now()
        config = stage.config

        match stage.stage_type:
            case 'HEALTH_CHECK':
                return self._http_health_check(config, started_at)

            case 'METRIC_CHECK':
                return self._metric_check(config, started_at)

            case 'SCRIPT':
                return self._script_check(config, deployment, started_at)

    def _http_health_check(self, config: dict, started_at: datetime) -> StageResult:
        """
        config: { "url": "http://payments-api/health", "expected_status": 200,
                  "timeout_s": 10, "retries": 3, "retry_interval_s": 5 }
        """
        for attempt in range(config.get('retries', 3)):
            try:
                resp = requests.get(
                    config['url'],
                    timeout=config.get('timeout_s', 10)
                )
                if resp.status_code == config.get('expected_status', 200):
                    return StageResult(
                        passed=True,
                        output={"http_status": resp.status_code,
                                "response_time_ms": resp.elapsed.total_seconds() * 1000},
                        completed_at=now()
                    )
                else:
                    failure_reason = f"HTTP {resp.status_code}, expected {config['expected_status']}"
            except requests.Timeout:
                failure_reason = f"Request timed out after {config['timeout_s']}s"
            except requests.ConnectionError as e:
                failure_reason = f"Connection failed: {e}"

            if attempt < config.get('retries', 3) - 1:
                time.sleep(config.get('retry_interval_s', 5))

        return StageResult(passed=False, failure_reason=failure_reason, completed_at=now())

    def _metric_check(self, config: dict, started_at: datetime) -> StageResult:
        """
        config: { "metric": "error_rate", "query": "sum(rate(errors[5m]))/sum(rate(requests[5m]))",
                  "threshold": 0.01, "operator": "lt", "window_s": 300 }
        """
        # Poll metrics for the configured window
        time.sleep(config['window_s'])

        value = self.metrics_client.query_scalar(config['query'])
        passed = (value < config['threshold']) if config['operator'] == 'lt' else (value > config['threshold'])

        return StageResult(
            passed=passed,
            output={"metric_value": value, "threshold": config['threshold'],
                    "query": config['query']},
            failure_reason=None if passed else f"Metric {value:.4f} {'≥' if config['operator']=='lt' else '≤'} threshold {config['threshold']}",
            completed_at=now()
        )

    def _script_check(self, config: dict, deployment: Deployment, started_at: datetime) -> StageResult:
        """
        config: { "script": "s3://deploy-scripts/smoke-test.sh", "timeout_s": 120 }
        Runs the script as a K8s Job in the target namespace.
        """
        job_name = f"health-check-{deployment.deployment_id[:8]}-{uuid4().hex[:6]}"
        script_content = s3.get_object(config['script'])

        job = self.k8s.create_job(
            name=job_name,
            namespace=deployment.env.k8s_namespace,
            image="company.registry/smoke-test-runner:latest",
            command=["bash", "-c", script_content],
            env_vars={
                "DEPLOYMENT_ID": str(deployment.deployment_id),
                "SERVICE_URL": deployment.service.internal_url,
                "ARTIFACT_VERSION": deployment.artifact.version
            },
            timeout_seconds=config.get('timeout_s', 120)
        )

        exit_code = self.k8s.wait_for_job(job_name, timeout=config.get('timeout_s', 120))
        logs = self.k8s.get_job_logs(job_name)
        self.k8s.delete_job(job_name)

        return StageResult(
            passed=(exit_code == 0),
            output={"exit_code": exit_code, "logs": logs[:1000]},  # truncate logs
            failure_reason=None if exit_code == 0 else f"Script exited with code {exit_code}",
            completed_at=now()
        )
```

**Interviewer Q&A:**

Q1: The metric check polls after waiting `window_s` seconds. This means the deployment is paused for 5 minutes. How do you avoid blocking the entire system?
A: Each deployment runs in its own goroutine/async task within the Deployment Orchestrator's worker pool. A deployment's 5-minute metric wait is a `time.sleep` only within that specific deployment's goroutine. Other deployments for other services proceed concurrently. With 1,000 simultaneous deployments, we need 1,000 goroutines — well within Go/async Python capabilities.

Q2: What is the health check strategy if the new version serves 10% of canary traffic — are you checking the right pods?
A: HTTP health checks target the canary service's endpoint specifically (not the stable service). For canary deployments, the health check URL uses the internal pod IP or a separate canary-specific K8s Service. Metric checks query Prometheus with label selectors filtering for `version="canary"` (set in the pod's labels): `sum(rate(errors{version="canary"}[5m])) / sum(rate(requests{version="canary"}[5m]))`. This ensures metrics reflect the canary version's behavior, not the stable version's.

Q3: How do you handle a health check that falsely fails due to the metrics window not having enough data yet (newly deployed canary has < 5 minutes of metrics)?
A: The metric check's `window_s` parameter must be set longer than the Prometheus scrape interval * sufficient data points. A minimum window of 5 minutes for error rate metrics provides enough data points (typically 10 scrapes at 30s interval) to avoid statistical noise. Additionally, the check uses a "burn rate" approach: instead of absolute error rate, compare to the baseline stable version's error rate: `canary_error_rate / stable_error_rate > 2.0` (2x worse than baseline). This normalizes for low-traffic periods.

Q4: How does the system handle a health check that passes, but then the service degrades 30 minutes later (after the deployment is marked SUCCEEDED)?
A: Post-deployment monitoring is a separate concern from the deployment system. The deployment system's responsibility ends at SUCCEEDED. Continuous monitoring (Datadog alerts, PagerDuty) handles post-deployment degradation. However, the deployment system retains rollback capability for 2 hours after SUCCEEDED: `POST /deployments/{id}/rollback`. The on-call SRE can trigger this. Automatic post-deployment rollback (beyond the immediate health check window) would require integrating the alerting system with the deployment API — a valid extension but complex to build correctly (risk of spurious rollbacks from unrelated incidents).

Q5: What are the failure modes of the script-based health check running as a K8s Job?
A: (1) Job pending (no resources): the orchestrator waits up to `timeout_s`; if still pending, fails the check. (2) Job crashes (OOMKilled): K8s exits the container; the job status shows `Failed`; the health check returns failure with reason "OOMKilled". (3) Script times out: K8s Job's `activeDeadlineSeconds` matches `timeout_s`; K8s terminates and marks job Failed. (4) Registry pull failure for the runner image: Job stays in `ImagePullBackOff`; orchestrator detects via job events and fails the check with reason "Image pull failed". All failure modes result in the health check returning `passed=False`, which triggers rollback.

---

## 7. Scaling

### Horizontal Scaling

- **Deployment Orchestrator**: Stateful per-deployment goroutine/task. Can run multiple instances with consistent hashing of `deployment_id` to a specific instance (using a Redis hash ring). Minimum 3 replicas. Each instance handles up to 500 concurrent deployment goroutines.
- **Health Check Engine**: Stateless workers. Scales based on active deployment count * avg check duration. Auto-scales via KEDA (K8s Event Driven Autoscaling) based on `active_deployment_count` metric.
- **API Service**: Stateless, scales to 20 pods during peak traffic.

### Database Scaling

- PostgreSQL handles the workload easily at < 200 write RPS. Single primary + 2 read replicas.
- Read replicas serve status queries and audit log reads.
- Audit log partitioned by month (`audit_log_2026_01`, `audit_log_2026_02`) to keep old partitions archivable to S3 (Parquet files) after 90 days.
- `health_check_results` data (4.38 TB over 2 years): partitioned by month. Older than 6 months migrated to S3 (detailed data not needed in DB); only aggregated metrics retained in the DB.

### Caching

| Cache Key                                     | TTL     | Contents                                    |
|-----------------------------------------------|---------|---------------------------------------------|
| `deployment:{deployment_id}:status`           | 10 s    | Current deployment status (for poll caching)|
| `service:{service_id}:pipeline_config`        | 5 min   | Pipeline stage config for the service       |
| `deploy_lock:{service_id}:{env_id}`           | Lock TTL (1h) | Lock value (deployment_id)           |
| `artifact:{service_id}:latest`                | 60 s    | Latest artifact version per service         |
| `env:{env_id}:config`                         | 5 min   | Environment configuration                   |

Deployment status is the hottest read (CI bot polls every 10 seconds during a deployment). Redis cache with 10-second TTL means the DB is only queried every 10 seconds regardless of how many CI bots are polling. For 1,000 concurrent deployments * 1 poll/10s = 100 DB reads/second (manageable).

### Multi-Region Considerations

The Deployment System itself is deployed in a single primary region (company's control plane region). It deploys to remote K8s clusters using long-lived kubeconfig credentials per region stored in Vault. The Kubernetes Client maintains connection pools to each remote cluster. Multi-region deployment pipelines use sequential stages: `[Deploy to us-east-1] → [Wait 10min + health check] → [Deploy to eu-west-1] → [Wait 10min + health check] → [Deploy to ap-south-1]`. Each stage is a separate pipeline stage of type `DEPLOY_TO_REGION`.

**Interviewer Q&A:**

Q1: With 1,000 concurrent deployments, each running health checks for 5-10 minutes, how do you prevent goroutine/thread exhaustion?
A: Use an async event loop (asyncio in Python, goroutines in Go, virtual threads in Java 21). Each deployment is a lightweight coroutine that suspends during the `await asyncio.sleep(window_s)` phase, consuming essentially zero CPU and minimal memory. 1,000 asyncio coroutines sleeping is trivially manageable. The actual HTTP health check calls use async HTTP clients (aiohttp) — they don't block threads. Goroutine overhead in Go is ~4 KB per goroutine; 1,000 goroutines = ~4 MB — negligible.

Q2: How would you scale the Deployment Orchestrator to handle 10,000 concurrent deployments (10x growth)?
A: (1) Increase Orchestrator replica count to 10; use consistent hashing by `deployment_id` for routing. (2) Use a distributed queue (Kafka topic `deployment.tasks` with 50 partitions) instead of direct goroutines — Orchestrator workers consume from the queue. (3) Extract the Health Check Engine as a separate microservice pool (100 worker pods). (4) Each `deployment.tasks` message is a deployment state machine event; the Orchestrator processes events rather than holding state in memory. This makes the Orchestrator stateless and horizontally scalable.

Q3: How do you prevent a faulty deployment pipeline configuration from taking down all 10,000 services?
A: Configuration validation at `PUT /pipelines/{id}` time: the Pipeline Config Service validates stage configs against JSON Schema, tests health check URLs for DNS resolution, and checks K8s cluster connectivity. A dry-run mode deploys to a `ci-sandbox` namespace and runs health checks without routing real traffic. Pipeline configs are versioned (stored in S3 as YAML with git-style history). Rollback to previous pipeline config is possible. A configuration change requires a second reviewer (GitOps approval workflow) for production pipelines.

Q4: How does the multi-region deployment handle a region failure mid-deployment?
A: The pipeline is sequential with a gate after each region. If us-east-1 deployment fails health checks, the pipeline stops before proceeding to eu-west-1 — preventing a bad version from propagating globally. If eu-west-1 deployment is already in progress and the region becomes unreachable, the health check times out → the orchestrator marks that stage FAILED → triggers rollback for eu-west-1 (using the stored manifest snapshot) → does NOT rollback us-east-1 (which may have succeeded). The operator receives an alert and can manually decide whether to roll back us-east-1.

Q5: How do you ensure the deployment system itself can be updated without downtime?
A: The Deployment Orchestrator is deployed using a Rolling Update strategy (not canary — ironic but practical). Before updating the Orchestrator, an operational pause is applied: `POST /admin/pause-new-deployments`. In-flight deployments complete; new triggers return 503. The Orchestrator is updated via K8s rolling update (always 1 old pod running until the new pod is healthy). After the update, the pause is lifted. For zero-downtime Orchestrator updates, each deployment's state machine state is persisted in PostgreSQL and Redis — on restart, the Orchestrator re-reads active deployments and resumes them from their last known state.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component              | Failure Mode                                  | Impact                                          | Mitigation                                                                   |
|------------------------|-----------------------------------------------|-------------------------------------------------|------------------------------------------------------------------------------|
| Deployment Orchestrator | Pod crash mid-deployment                     | In-flight deployment paused                     | K8s restarts pod; state recovered from DB; deployment resumes               |
| Redis (lock store)     | All nodes down                               | Lock acquisition fails; new deploys blocked     | Fall back to PostgreSQL advisory lock; alert; operator decision              |
| PostgreSQL             | Primary crash                                | Lock records, audit log writes fail             | Patroni failover in < 30s; deployment acquires lock on recovery             |
| K8s API server         | K8s cluster unreachable                      | Deployment cannot modify K8s resources          | Retry with exponential backoff; deployment times out; rollback if started   |
| Kafka                  | Broker failure                               | Notifications delayed; audit events buffered    | RF=3; ISR; no data loss; processing resumes on recovery                     |
| Health Check HTTP target | Target service temporarily down (cold start) | Health check fails; spurious rollback          | Multiple retries with interval; 2-of-3 consecutive failures required        |
| Deployment lock timeout | Lock TTL expires during slow deployment      | Lock released; stale deployment continues alone | Lock TTL recovery job detects and terminates stale deployment               |
| Target K8s cluster     | Remote region K8s cluster down              | Regional deployment fails; global deploy stops  | Pipeline stops before next region; existing running pods unaffected         |

### Retries & Idempotency

- K8s API calls: idempotent (apply manifest = `kubectl apply --server-side`; K8s handles "already exists" as no-op).
- Deployment trigger: POST /deployments is idempotent via idempotency key from CI bot header.
- Health check execution: retry up to 3 times with 5-second interval before marking failed.
- Audit log writes: Kafka at-least-once; consumer deduplicates via log_id (already inserted → skip).

### Circuit Breaker

- Orchestrator → K8s API: circuit opens after 5 consecutive K8s API errors. Open circuit = all new deployments to that cluster are queued with PENDING status. Operator is paged. Half-open probe every 30 seconds.
- Orchestrator → Metrics API (Datadog): circuit opens on 3 consecutive timeout. Fallback: skip metric checks and rely solely on HTTP health checks (log warning).

---

## 9. Monitoring & Observability

### Metrics

| Metric                                    | Type      | Alert Threshold                           |
|-------------------------------------------|-----------|-------------------------------------------|
| `deployment.duration_seconds_p99`         | Histogram | > 15 minutes                              |
| `deployment.success_rate`                 | Gauge     | < 95% over 1 hour                         |
| `deployment.rollback_rate`                | Gauge     | > 10% over 1 hour                         |
| `deployment.lock.wait_time_seconds_p99`   | Histogram | > 300 s (someone waiting for lock)        |
| `health_check.failure_rate`               | Gauge     | > 5% → investigate health check quality  |
| `deployment.active_count`                 | Gauge     | > 800 (capacity warning)                  |
| `deployment.queue_depth`                  | Gauge     | > 50 (deploy queue backed up)             |
| `artifact.promotion.missing_stage`        | Counter   | > 0 (production deploy with no staging)  |
| `k8s.api.error_rate`                      | Gauge     | > 1% → K8s cluster health issue          |
| `rollback.time_to_complete_seconds_p99`   | Histogram | > 300 s                                   |

### Distributed Tracing

OpenTelemetry traces for every deployment. A deployment trace spans the entire lifecycle: API Gateway → Deployment Orchestrator (deployment creation) → Lock Manager (Redis SETNX) → K8s Client (apply manifest) → Health Check Engine (HTTP probe * N) → Traffic Controller (weight update) → K8s Client (stable promotion) → Lock Manager (release). Total trace duration: 10-60 minutes for canary. Sampled at 100% (every deployment is traced — low volume, high value). Exported to Jaeger.

### Logging

Structured JSON per deployment event. Fields: `trace_id`, `deployment_id`, `service_id`, `env`, `artifact_version`, `strategy`, `stage`, `status`, `actor_id`, `duration_ms`, `error`. Deployment logs are streamed in real-time via Server-Sent Events to the deployment UI page. Log levels: INFO for normal progress, WARN for retries, ERROR for failures, CRITICAL for data integrity issues (lock race conditions). All logs shipped to Splunk / ELK; deployment-specific logs also stored in PostgreSQL `deployment_stage_results.output` for inline display in UI.

**Audit Log Immutability**: The PostgreSQL `audit_log` table has no UPDATE or DELETE grants for any application role. Only INSERT is granted. Periodic exports to S3 (Parquet, encrypted) with WORM (Write Once Read Many) bucket policy provide a tamper-evident archive. Compliance teams can read from S3 without any access to the live database.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                          | Chosen Approach                                  | Alternative                          | Trade-off                                                                |
|-----------------------------------|--------------------------------------------------|--------------------------------------|--------------------------------------------------------------------------|
| Lock mechanism                    | Redis SETNX + PostgreSQL uniqueness constraint   | ZooKeeper / etcd                     | Redis is simpler to operate; ZooKeeper would add strong consistency      |
| Deployment state persistence      | PostgreSQL state machine + Redis status cache    | In-memory only                       | DB persistence enables crash recovery; adds latency for state updates    |
| Health check as K8s Job vs Lambda | K8s Job (script checks)                         | Lambda / Cloud Functions             | K8s Job has access to service namespace; Lambda needs VPC config        |
| Multi-region sequencing           | Sequential with gates                           | Parallel regional deploys            | Sequential is safer (bad version stopped before global spread); slower  |
| Canary traffic splitting          | Istio VirtualService weight                     | Feature flags only                   | Istio provides real traffic split at network level; feature flags are per-user |
| Audit log storage                 | PostgreSQL + S3 WORM archive                    | Immutable Kafka topic (infinite retention) | PostgreSQL is queryable; S3 WORM is compliant; Kafka for infinite retention is costly |
| Rollback target                   | Previous artifact (re-deploy)                   | Snapshot restore                     | Re-deploy is cleaner (same code path); snapshot restore is faster but bypasses health checks |
| Deployment pipeline config        | Database (JSONB) + versioned in S3              | Pure GitOps (only in git)            | DB enables runtime API changes; S3 version history enables config rollback |

---

## 11. Follow-up Interview Questions

Q1: How would you implement feature flag integration so a deployment gradually enables a feature as the canary traffic increases?
A: The Traffic Controller has an optional feature flag backend (LaunchDarkly / Unleash). When canary weight progresses (10% → 25% → 50% → 100%), the Traffic Controller also calls the feature flag API to update the rollout percentage for `feature.{service_name}.{version}` to match. Users hit the canary pod and experience the feature in the same proportion. On rollback: set feature flag rollout to 0% and canary weight to 0% simultaneously.

Q2: How do you handle a deployment to a service that has active database migrations running?
A: Add a `MIGRATION` pipeline stage type that runs before the `DEPLOY` stage. The migration stage creates a K8s Job that runs `flyway migrate` or `alembic upgrade head` against the database. The health check for the migration stage monitors the K8s Job until it exits 0. If it fails (e.g., conflicting migration), the deployment fails before any new pods are started — the database is unchanged and the old version continues running. The deployment system does not manage the migration content; it only orchestrates when it runs.

Q3: How would you design deployment approval workflows for regulated industries (SOX, HIPAA)?
A: Add an `APPROVAL` pipeline stage between staging-to-production promotion. The approval stage: (1) creates an `approval_requests` record with the deploying artifact's details, change description, and required approvers (configurable: 2 of N SREs, 1 of N release managers); (2) sends Slack/email to approvers with a one-click approve/deny link; (3) records the approver's identity, timestamp, and reason in the audit log (non-repudiable); (4) expires after 24 hours if not approved (auto-deny). Approval responses are authenticated via OAuth2. The audit log entry includes the full deployment manifest snapshot, satisfying change management evidence requirements.

Q4: How do you prevent a developer from bypassing the deployment system and applying kubectl directly?
A: Kubernetes RBAC: engineers' K8s credentials have only `GET` and `LIST` verbs. Only the deployment system's service account has `PATCH`, `UPDATE`, and `DELETE` on Deployment resources. The service account credentials are stored in Vault with short-lived tokens (1-hour TTL, auto-rotated). K8s admission webhooks (OPA Gatekeeper policies) block changes to the deployment's `spec.template` unless the request comes from the deployment system's service account. All changes are logged in the K8s audit log.

Q5: How would you implement rollback for a canary deployment that has already reached 100% but needs to revert?
A: Post-promotion rollback creates a new deployment targeting the previous artifact version. The deployment orchestrator uses the `previous_artifact_id` from the current deployment record. A blue-green or rolling strategy is used for the rollback (not canary — rollback should be fast, not gradual). The `manifest_snapshot` JSONB field stored on the original deployment provides the exact K8s manifests to re-apply, ensuring the rollback is byte-for-byte identical to the previous stable state.

Q6: How do you handle dependency ordering when Service A must deploy before Service B?
A: Add a `WAIT_FOR_DEPLOYMENT` pipeline stage type: `{ "service": "payments-api", "min_version": "v1.2.0", "environment": "production", "timeout_s": 3600 }`. This stage polls the Deployment System's API until the specified service's current running version in the target environment meets the condition. Complex dependency graphs are represented as ordered pipeline stages. For mutual dependencies (A needs B ≥ v1 and B needs A ≥ v1), this creates a deadlock — detected by analyzing the dependency graph at pipeline creation time (circular dependency check).

Q7: Describe the deployment system's behavior during a major incident when you need to freeze deployments across all services.
A: An SRE calls `POST /admin/freeze-deployments` with a reason. This sets `freeze_active = true` in a global configuration in Redis and PostgreSQL. The Deployment Orchestrator checks this flag before acquiring any new locks — new deployment requests return 503 with `{ "error": "DEPLOYMENT_FREEZE_ACTIVE", "reason": "Major incident in progress" }`. In-flight deployments continue (stopping them mid-deploy could be worse than completing). The freeze also triggers a Kafka event → Notification Service → broadcasts to all engineering Slack channels. `DELETE /admin/freeze-deployments` lifts the freeze.

Q8: How would you implement a "deployment budget" (SRE concept) that limits how many simultaneous production deployments can happen to prevent cascading failures?
A: A global production deployment semaphore in Redis: `SETNX deploy_semaphore:{region}:production:slot_{N}` where N ranges from 0 to `max_concurrent_production_deploys` (configurable, e.g., 10). Before acquiring a service-specific lock, the Orchestrator acquires one semaphore slot. If no slots available, the deployment queues. This limits blast radius: at most 10 services can deploy to production simultaneously, regardless of how many engineers trigger deploys at once.

Q9: How do you handle deployment of stateful services (databases, Kafka, Zookeeper)?
A: Stateful services require special handling: (1) PodDisruptionBudgets to ensure quorum is maintained during rolling updates. (2) StatefulSet rolling update strategy (ordered pod updates, not parallel). (3) A `MIN_READY_SECONDS` gate to ensure each pod is fully ready (joined the cluster, replicated data) before proceeding. (4) Kafka: rolling restart with broker ID ordering; leader rebalancing monitored via health checks against Kafka Admin API. The deployment system has a `STATEFUL` deployment type that invokes StatefulSet-specific logic. Storage migrations (schema changes) run as separate maintenance operations, not inline with code deployments.

Q10: How would you detect and prevent "configuration drift" — where the running pods don't match the expected deployment manifest?
A: A drift detection job runs every 15 minutes per service per environment. It fetches the current K8s Deployment's `spec.template.spec.containers[*].image` and compares it to the last SUCCEEDED deployment record's `artifact.image_uri`. If they don't match, it writes a `DRIFT_DETECTED` entry to the audit log and fires an alert. Drift can occur from: (1) direct `kubectl set image` override; (2) manual K8s edits; (3) ArgoCD sync divergence. The alert goes to the service team and the SRE platform team.

Q11: How does the system handle a Docker registry outage when Kubernetes can't pull the new image?
A: The health check stage `WAIT_FOR_ROLLOUT` will detect pods stuck in `ImagePullBackOff` state via `kubectl get pods` status check. If pods don't reach `Running` within 5 minutes, the deployment fails with reason "Image pull failed." Rollback is triggered — since the old pods are still running (rolling update only kills them after new ones are ready), traffic is unaffected. The Kubernetes Image Pull Secret and Registry credentials are managed by Vault integration. For critical services, a registry mirror (Harbor/JFrog Artifactory) in the same VPC caches images, eliminating the external registry as a single point of failure.

Q12: How would you implement "progressive delivery" combining both a canary deployment and automated metric-based promotion?
A: Configure an automated canary analysis stage: `{ "type": "CANARY_ANALYSIS", "provider": "kayenta", "baseline": "stable", "canary": "canary", "metrics": [{"name": "error_rate", "threshold": 1.01}, {"name": "p99_latency_ms", "threshold": 1.1}], "duration_minutes": 10 }`. The stage uses Kayenta (Netflix's automated canary analysis tool) or a custom statistical comparison: collect metrics for both stable and canary pods, compute a Mann-Whitney U-test for statistical significance. If canary metrics are worse than baseline by more than the threshold, fail the stage. This replaces human judgment with statistical rigor for go/no-go decisions.

Q13: How do you handle secrets that are version-coupled with the application (e.g., a new API key added in v1.2.0)?
A: Secrets are managed in Vault with version tags. The deployment manifest (in `manifest_snapshot`) references a Vault secret version: `vault.company.com/payments-api:v1.2.0`. The deployment pipeline has a `VAULT_SYNC` stage that ensures the required secret version is available before deploying. If the secret doesn't exist yet (developer forgot to add it), the deployment fails with a clear error: "Secret payments-api:v1.2.0 not found in Vault." This avoids deploying code that references missing secrets, which would cause runtime panics.

Q14: Describe your approach to canary deployments in a microservices environment where the canary might call downstream services that haven't been updated.
A: Backward compatibility is a deployment contract: new versions must be backward-compatible with N-1 versions of all dependencies. The deployment pipeline configuration includes a `compatibility_check` stage that reads the service's dependency manifest (from ServiceMesh telemetry or a service catalog) and validates the new version's API compatibility against the currently deployed version of all dependencies. For database schema changes, backward compatibility is enforced via the expand-contract pattern. If backward compatibility cannot be guaranteed, sequential deployment is required (update dependencies first, then update the caller).

Q15: How would you design the deployment system to support GitOps (where desired state is defined in a git repository and automatically applied)?
A: Add a GitOps operator component: a controller that watches a git repository (via Git webhooks or periodic polling). On each push to `main`, it reads the `desired-state.yaml` per service (containing artifact version and environment config). It diffs the desired state against the current state (from the Deployment System API's `GET /services/{id}/environments/{env}/current-version`). If there is a divergence, it automatically calls `POST /deployments` to bring the actual state in sync. The deployment system itself remains strategy-agnostic — GitOps is just another trigger source (`trigger_source = GITOPS`). Approval gates still apply: a GitOps-triggered production deployment still requires human approval if configured.

---

## 12. References & Further Reading

1. Spinnaker Documentation — Deployment Strategies: https://spinnaker.io/docs/guides/user/pipeline/pipeline-expressions/
2. Argo Rollouts — Progressive Delivery on Kubernetes: https://argoproj.github.io/rollouts/
3. Netflix Engineering — "Kayenta: Open Source Automated Canary Analysis": https://netflixtechblog.com/automated-canary-analysis-at-netflix-with-kayenta-3260bc7acc69
4. Google SRE Book — "Release Engineering": https://sre.google/sre-book/release-engineering/
5. Kubernetes Documentation — Deployment Strategies: https://kubernetes.io/docs/concepts/workloads/controllers/deployment/
6. Istio Documentation — Traffic Management (VirtualService weights): https://istio.io/latest/docs/concepts/traffic-management/
7. Martin Fowler — "Blue-Green Deployment": https://martinfowler.com/bliki/BlueGreenDeployment.html
8. Jez Humble, David Farley — "Continuous Delivery" (Addison-Wesley)
9. Redis Documentation — "Distributed Locks with Redis": https://redis.io/docs/manual/patterns/distributed-locks/
10. DORA (DevOps Research and Assessment) — "Accelerate: State of DevOps" Report: https://dora.dev/research/
11. OPA Gatekeeper — Kubernetes Policy Controller: https://open-policy-agent.github.io/gatekeeper/
12. Flagger — Progressive Delivery Operator for Kubernetes: https://flagger.app/
