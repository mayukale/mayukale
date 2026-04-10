# System Design: CI/CD Pipeline

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Source Control Integration**: Ingest webhook events from Git providers (GitHub, GitLab, Bitbucket) on push, pull request open/update/merge, and tag creation.
2. **Build Orchestration**: Schedule, dispatch, and execute build jobs across a pool of build agents; support parallel job execution within a pipeline.
3. **Test Parallelization**: Split test suites across multiple agents, collect results, merge coverage reports, and surface failures with per-test metadata.
4. **Artifact Storage**: Store versioned build artifacts (binaries, Docker images, JAR files, npm tarbundles) with content-addressable identifiers and retention policies.
5. **Deployment Strategies**: Support blue-green, canary (traffic-weighted), and rolling deployments to multiple target environments (dev, staging, production).
6. **Environment Promotion**: Enforce ordered promotion gates (dev → staging → production) with approval workflows for production deploys.
7. **Rollback Mechanisms**: One-click and automatic rollback to any previously deployed artifact version within the retention window.
8. **Audit Trail**: Record every pipeline event (trigger, stage start/end, approval, deploy, rollback) with actor, timestamp, and outcome.
9. **Notifications**: Emit pipeline status events to Slack, PagerDuty, email, and webhooks.
10. **Secret Injection**: At build/deploy time, inject secrets from a vault into the job environment without persisting them in logs or artifacts.

### Non-Functional Requirements

- **Availability**: 99.9% uptime for pipeline trigger ingestion and scheduling; build agents may tolerate brief restarts.
- **Throughput**: Handle 10,000 pipeline trigger events per day across all organizations; burst to 3x during business hours.
- **Build Start Latency**: A triggered pipeline must begin executing its first job within 30 seconds of the webhook event (p99).
- **Scalability**: Agent pool scales from 10 to 10,000 agents; platform itself scales horizontally without re-architecture.
- **Security**: All secrets encrypted at rest (AES-256) and in transit (TLS 1.3); no plaintext secrets in logs.
- **Isolation**: Each job runs in an ephemeral, isolated environment (container or VM); no shared state between jobs of different pipelines unless explicitly configured.
- **Data Retention**: Artifacts retained for 90 days by default; logs retained for 1 year.
- **Auditability**: All pipeline events immutably stored and queryable for compliance purposes.

### Out of Scope

- Source control hosting (GitHub/GitLab itself).
- Container registry implementation (assumed external: ECR, GCR, Docker Hub).
- Infrastructure provisioning (Terraform/Pulumi — the CI/CD system calls out to these tools but does not implement them).
- Issue tracking integration beyond status comments on pull requests.
- Cost allocation and chargeback per team.

---

## 2. Users & Scale

### User Types

| User Type | Description | Primary Actions |
|---|---|---|
| Developer | Engineers pushing code | Trigger pipelines, view build logs, re-run failed jobs |
| Release Engineer | Owns deployment process | Approve promotion gates, configure deployment strategies |
| Platform Engineer | Administers CI/CD platform | Manage agent pools, configure integrations, set retention policies |
| Security Auditor | Reviews compliance evidence | Query audit trail, export logs |
| Automated System | Scheduled triggers, other services | API-triggered pipeline runs, status webhooks |

### Traffic Estimates

Assumptions:
- 500 engineering organizations, average 200 developers each = 100,000 developers total.
- Each developer triggers ~5 pipeline runs per day (commits + PR updates).
- Average pipeline has 4 stages (build, test, staging deploy, prod deploy), each stage has 3 jobs.
- Each job emits logs at ~50 KB/s average for 3 minutes average duration.

| Metric | Calculation | Result |
|---|---|---|
| Pipeline triggers/day | 100,000 devs × 5 triggers | 500,000 triggers/day |
| Pipeline triggers/sec (avg) | 500,000 / 86,400 | ~5.8 triggers/sec |
| Pipeline triggers/sec (peak 3x) | 5.8 × 3 | ~17.4 triggers/sec |
| Jobs/day | 500,000 pipelines × 4 stages × 3 jobs | 6,000,000 jobs/day |
| Concurrent jobs (avg) | 6,000,000 × (3 min / 1440 min) | ~12,500 concurrent jobs |
| Concurrent jobs (peak) | 12,500 × 3 (business-hours burst) | ~37,500 concurrent jobs |
| Log bytes/day | 6,000,000 jobs × 50 KB/s × 180 s | ~54 TB/day |
| Webhook ingest events/sec | 17.4 triggers × 1.2 (retries/duplicates) | ~21 events/sec |

### Latency Requirements

| Operation | Target (p50) | Target (p99) | Notes |
|---|---|---|---|
| Webhook receipt to job scheduled | 5 s | 30 s | SLA commitment |
| Log line to visible in UI | 2 s | 5 s | Tail-log streaming |
| Artifact upload (100 MB) | 10 s | 30 s | Depends on agent network |
| Deployment status update | 3 s | 10 s | Kubernetes/ECS reconciliation time included |
| Rollback initiation to traffic shift | 60 s | 120 s | Blue-green swap is fast; canary drain is slower |
| Dashboard pipeline list load | 200 ms | 800 ms | Paginated read from DB |

### Storage Estimates

| Data Type | Size per Unit | Volume/Day | Retention | Total Storage |
|---|---|---|---|---|
| Build logs | 50 KB/s × 180 s = 9 MB/job | 6M jobs × 9 MB = 54 TB | 365 days | 54 TB × 365 = ~19.7 PB (with compression ~4 PB) |
| Artifacts (binaries) | ~200 MB avg per pipeline | 500K × 200 MB = 100 TB | 90 days | 100 TB × 90 = ~9 PB |
| Pipeline metadata (DB) | ~5 KB/pipeline record | 500K × 5 KB = 2.5 GB | Indefinite | ~2.5 GB/day → ~900 GB/year |
| Audit events | ~500 B/event, ~20 events/pipeline | 500K × 20 × 500 B = 5 GB | 7 years (compliance) | ~12.8 TB |
| Agent heartbeats/metrics | ~1 KB/10 s per agent, 37,500 agents | 37,500 × 8,640 × 1 KB = 324 GB | 30 days | ~9.7 TB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Log ingest (agents → log store) | 6M jobs × 9 MB / 86,400 s | ~625 MB/s avg, ~1.9 GB/s peak |
| Artifact upload (agents → artifact store) | 500K × 200 MB / 86,400 s | ~1.16 GB/s avg |
| Artifact download (deploy agents) | ~same as upload | ~1.16 GB/s avg |
| Webhook ingest | 21 events/s × ~2 KB | ~42 KB/s (negligible) |
| Log streaming (UI reads) | Assume 5,000 concurrent users × 50 KB/s | ~250 MB/s |

---

## 3. High-Level Architecture

```
                          ┌──────────────────────────────────────────────────────────────────┐
                          │                        Source Control                            │
                          │         (GitHub / GitLab / Bitbucket)                           │
                          └───────────────────────┬──────────────────────────────────────────┘
                                                  │ Webhook (push/PR/tag)
                                                  ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    INGESTION LAYER                                          │
│                                                                                             │
│   ┌─────────────────┐    ┌──────────────────┐    ┌──────────────────┐                      │
│   │  Webhook Gateway│───▶│  Event Validator │───▶│  Message Queue   │                      │
│   │  (API Gateway / │    │  (HMAC sig check,│    │  (Kafka / SQS)   │                      │
│   │   Load Balancer)│    │   schema parse)  │    │                  │                      │
│   └─────────────────┘    └──────────────────┘    └────────┬─────────┘                      │
└────────────────────────────────────────────────────────────┼────────────────────────────────┘
                                                             │
                                                             ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│                                   ORCHESTRATION LAYER                                       │
│                                                                                             │
│   ┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐                     │
│   │  Pipeline        │───▶│  Job Scheduler   │───▶│  Agent Pool      │                     │
│   │  Controller      │    │  (DAG executor)  │    │  Manager         │                     │
│   │  (YAML parser,   │    │  (priority queue,│    │  (auto-scale,    │                     │
│   │   stage graph)   │    │   dependency res)│    │   health checks) │                     │
│   └──────────┬───────┘    └──────────────────┘    └────────┬─────────┘                     │
│              │                                             │                                │
│              ▼                                             ▼                                │
│   ┌──────────────────┐                        ┌──────────────────────┐                     │
│   │  Config / Secret │                        │  Build Agents        │                     │
│   │  Service         │                        │  (ephemeral          │                     │
│   │  (Vault-backed)  │                        │   containers/VMs)    │                     │
│   └──────────────────┘                        └──────────┬───────────┘                     │
└────────────────────────────────────────────────────────────┼────────────────────────────────┘
                                                             │
                           ┌─────────────────────────────────┼───────────────────────────┐
                           │                                 │                           │
                           ▼                                 ▼                           ▼
              ┌────────────────────┐           ┌─────────────────────┐     ┌─────────────────────┐
              │  Log Aggregation   │           │  Artifact Registry  │     │  Test Result        │
              │  (S3 + CloudWatch  │           │  (S3-backed, OCI    │     │  Aggregator         │
              │   / ELK)           │           │   manifest index)   │     │  (JUnit XML merge,  │
              └────────────────────┘           └──────────┬──────────┘     │   coverage reports) │
                                                          │                └─────────────────────┘
                                                          ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│                                  DEPLOYMENT LAYER                                           │
│                                                                                             │
│   ┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐                     │
│   │  Deployment      │───▶│  Traffic Manager │───▶│  Target Clusters │                     │
│   │  Controller      │    │  (blue-green /   │    │  (Kubernetes /   │                     │
│   │  (strategy exec) │    │   canary weights)│    │   ECS / VMs)     │                     │
│   └──────────┬───────┘    └──────────────────┘    └──────────────────┘                     │
│              │                                                                              │
│              ▼                                                                              │
│   ┌──────────────────┐    ┌──────────────────┐                                             │
│   │  Approval Gate   │    │  Rollback Engine │                                             │
│   │  (human review   │    │  (manifest store,│                                             │
│   │   + policy check)│    │   atomic swap)   │                                             │
│   └──────────────────┘    └──────────────────┘                                             │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│                                 OBSERVABILITY & AUDIT                                       │
│   ┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐                     │
│   │  Audit Log Store │    │  Metrics (Prom / │    │  Notification    │                     │
│   │  (immutable,     │    │   DataDog)       │    │  Dispatcher      │                     │
│   │   append-only)   │    │                  │    │  (Slack/PD/email)│                     │
│   └──────────────────┘    └──────────────────┘    └──────────────────┘                     │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

- **Webhook Gateway**: Terminates TLS, validates HMAC signatures on incoming Git provider webhooks, rate-limits per-org, publishes raw events to Kafka. Acts as the trust boundary between the public internet and internal systems.
- **Event Validator**: Parses provider-specific payloads (GitHub push event, GitLab merge request event), normalizes them to an internal canonical schema, enriches with org/project metadata from the Config DB.
- **Message Queue (Kafka)**: Decouples webhook reception from pipeline processing. Provides durability (replay on controller crash), ordering per-branch, and back-pressure. Partitioned by org_id for isolation.
- **Pipeline Controller**: Reads pipeline definition YAML from the repository (`.cicd.yaml`), constructs a DAG of stages and jobs, persists the pipeline run record to the metadata DB, and emits job dispatch messages.
- **Job Scheduler**: Maintains a priority queue of pending jobs, matches jobs to available agents by capability labels (e.g., `docker`, `gpu`, `macos`), and enforces concurrency limits per org.
- **Agent Pool Manager**: Tracks agent registration, health, and load. Triggers auto-scaling (Kubernetes HPA or EC2 Auto Scaling) based on queue depth. Evicts stale agents.
- **Build Agents**: Short-lived containers or VMs that poll for assigned jobs, execute the job steps, stream logs, upload artifacts, and report exit codes back to the scheduler.
- **Config/Secret Service**: Provides build-time secret injection via a short-lived token mechanism. Agents receive a one-time-use Vault token scoped to the specific job.
- **Log Aggregation**: Receives log streams from agents via a log forwarder (Fluent Bit). Stores compressed logs in S3. Exposes a streaming read API for the UI.
- **Artifact Registry**: Stores build outputs (Docker images, binaries) with content-addressable keys (SHA-256). Supports retention policies and cross-region replication.
- **Deployment Controller**: Reads the deployment strategy from the pipeline config, coordinates with the target cluster's control plane (Kubernetes API server, ECS API) to execute the deploy.
- **Traffic Manager**: For blue-green, atomically swaps the active environment. For canary, adjusts traffic weights over time while monitoring error rates.
- **Approval Gate**: Blocks pipeline promotion between environments until required approvers (by role or named user) grant approval, or a policy check (CODEOWNERS, required status checks) passes automatically.
- **Rollback Engine**: Stores the last N deployment manifests/artifact references. On rollback trigger, replays the previous deployment through the Deployment Controller.
- **Audit Log Store**: Receives every pipeline lifecycle event as an append-only record. Backed by an immutable object store (S3 Object Lock) or an append-only DB.

**Primary Use-Case Data Flow (Developer pushes a commit):**

1. Developer pushes to a feature branch on GitHub.
2. GitHub fires a `push` webhook to Webhook Gateway (HMAC-signed).
3. Gateway validates signature, rate-checks the org, publishes to Kafka topic `webhook-events`.
4. Pipeline Controller consumes the event, fetches `.cicd.yaml` from the repository at the pushed SHA, resolves the triggering branch rule, creates a `PipelineRun` record (status: `pending`), and emits `JobQueued` events for Stage 1 jobs.
5. Job Scheduler dequeues jobs, matches to available agents, updates job status to `assigned`, and sends a dispatch message to the agent.
6. Agent pulls the source code (shallow clone at the pushed SHA), executes job steps (e.g., `docker build`), streams logs to the Log Aggregation service via Fluent Bit.
7. On job success, agent uploads the Docker image to the Artifact Registry and reports success. Scheduler marks the job complete, evaluates if all jobs in Stage 1 are done.
8. Pipeline Controller advances to Stage 2 (tests). Test jobs are dispatched in parallel; results are sent to the Test Result Aggregator.
9. On all tests passing, Controller evaluates the deployment stage. For dev: auto-deploys. For production: emits an `ApprovalRequired` event.
10. Release Engineer approves in the UI; Approval Gate records the approval in the audit trail and unblocks the pipeline.
11. Deployment Controller executes a canary deployment to production: shifts 5% traffic to the new version, monitors error rates for 10 minutes, then proceeds to 100%.
12. Notification Dispatcher sends a Slack message with the deployment status and link.

---

## 4. Data Model

### Entities & Schema

```sql
-- Organizations and projects
CREATE TABLE organizations (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug            VARCHAR(64) UNIQUE NOT NULL,
    name            VARCHAR(255) NOT NULL,
    plan_tier       VARCHAR(32) NOT NULL DEFAULT 'free', -- 'free','team','enterprise'
    max_concurrency INT NOT NULL DEFAULT 5,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE projects (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    slug            VARCHAR(128) NOT NULL,
    name            VARCHAR(255) NOT NULL,
    default_branch  VARCHAR(255) NOT NULL DEFAULT 'main',
    repo_url        VARCHAR(1024) NOT NULL,
    repo_provider   VARCHAR(32) NOT NULL, -- 'github','gitlab','bitbucket'
    webhook_secret  VARCHAR(128) NOT NULL, -- stored encrypted
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, slug)
);

-- Pipeline definitions (cached from YAML, for history and UI)
CREATE TABLE pipeline_definitions (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id      UUID NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    sha             VARCHAR(40) NOT NULL,   -- git commit SHA of the YAML
    definition      JSONB NOT NULL,         -- parsed pipeline YAML as JSON
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(project_id, sha)
);

-- Pipeline runs
CREATE TABLE pipeline_runs (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    project_id      UUID NOT NULL REFERENCES projects(id),
    definition_id   UUID REFERENCES pipeline_definitions(id),
    trigger_type    VARCHAR(32) NOT NULL,   -- 'push','pull_request','tag','manual','schedule'
    trigger_ref     VARCHAR(512) NOT NULL,  -- branch name, tag, PR number
    trigger_sha     VARCHAR(40) NOT NULL,   -- git commit SHA that triggered this run
    triggered_by    UUID REFERENCES users(id), -- NULL for automated triggers
    status          VARCHAR(32) NOT NULL DEFAULT 'pending',
                                            -- pending,running,success,failed,cancelled,waiting_approval
    started_at      TIMESTAMPTZ,
    finished_at     TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_pipeline_runs_project (project_id, created_at DESC),
    INDEX idx_pipeline_runs_status  (status, created_at DESC)
);

-- Stages within a pipeline run
CREATE TABLE stages (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    pipeline_run_id UUID NOT NULL REFERENCES pipeline_runs(id) ON DELETE CASCADE,
    name            VARCHAR(255) NOT NULL,
    sequence        INT NOT NULL,           -- execution order
    status          VARCHAR(32) NOT NULL DEFAULT 'pending',
    started_at      TIMESTAMPTZ,
    finished_at     TIMESTAMPTZ,
    INDEX idx_stages_run (pipeline_run_id, sequence)
);

-- Individual jobs
CREATE TABLE jobs (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    stage_id        UUID NOT NULL REFERENCES stages(id) ON DELETE CASCADE,
    pipeline_run_id UUID NOT NULL REFERENCES pipeline_runs(id), -- denormalized for fast lookup
    name            VARCHAR(255) NOT NULL,
    agent_id        UUID REFERENCES agents(id),
    status          VARCHAR(32) NOT NULL DEFAULT 'queued',
                                            -- queued,assigned,running,success,failed,cancelled,timed_out
    runner_labels   TEXT[] NOT NULL DEFAULT '{}', -- capability requirements
    exit_code       INT,
    log_path        VARCHAR(1024),          -- S3 key for log file
    started_at      TIMESTAMPTZ,
    finished_at     TIMESTAMPTZ,
    queued_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    retry_count     INT NOT NULL DEFAULT 0,
    max_retries     INT NOT NULL DEFAULT 0,
    timeout_seconds INT NOT NULL DEFAULT 3600,
    INDEX idx_jobs_stage      (stage_id),
    INDEX idx_jobs_status     (status, queued_at),
    INDEX idx_jobs_pipeline   (pipeline_run_id)
);

-- Build agents
CREATE TABLE agents (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID REFERENCES organizations(id), -- NULL = shared pool
    hostname        VARCHAR(255) NOT NULL,
    ip_address      INET,
    labels          TEXT[] NOT NULL DEFAULT '{}',
    status          VARCHAR(32) NOT NULL DEFAULT 'idle',
                                            -- idle,busy,offline,draining
    version         VARCHAR(64),
    last_heartbeat  TIMESTAMPTZ,
    registered_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_agents_status_labels (status, labels)
);

-- Artifacts
CREATE TABLE artifacts (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    job_id          UUID NOT NULL REFERENCES jobs(id),
    pipeline_run_id UUID NOT NULL REFERENCES pipeline_runs(id), -- denormalized
    name            VARCHAR(255) NOT NULL,
    artifact_type   VARCHAR(64) NOT NULL,   -- 'docker_image','binary','archive','report'
    storage_key     VARCHAR(1024) NOT NULL, -- S3 path or OCI digest
    size_bytes      BIGINT NOT NULL,
    sha256          VARCHAR(64) NOT NULL,
    expires_at      TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_artifacts_run (pipeline_run_id),
    INDEX idx_artifacts_expiry (expires_at)
);

-- Deployment records
CREATE TABLE deployments (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    pipeline_run_id UUID NOT NULL REFERENCES pipeline_runs(id),
    artifact_id     UUID NOT NULL REFERENCES artifacts(id),
    environment     VARCHAR(64) NOT NULL,   -- 'dev','staging','production'
    strategy        VARCHAR(32) NOT NULL,   -- 'blue_green','canary','rolling'
    status          VARCHAR(32) NOT NULL DEFAULT 'pending',
                                            -- pending,in_progress,success,failed,rolled_back
    deployed_by     UUID REFERENCES users(id),
    manifest        JSONB,                  -- Kubernetes manifests or ECS task def snapshot
    previous_deployment_id UUID REFERENCES deployments(id), -- for rollback chain
    started_at      TIMESTAMPTZ,
    finished_at     TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_deployments_env   (environment, created_at DESC),
    INDEX idx_deployments_run   (pipeline_run_id)
);

-- Approval gates
CREATE TABLE approval_requests (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    pipeline_run_id UUID NOT NULL REFERENCES pipeline_runs(id),
    environment     VARCHAR(64) NOT NULL,
    requested_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    approved_at     TIMESTAMPTZ,
    approved_by     UUID REFERENCES users(id),
    rejected_at     TIMESTAMPTZ,
    rejected_by     UUID REFERENCES users(id),
    rejection_reason TEXT,
    expires_at      TIMESTAMPTZ NOT NULL    -- auto-cancel if not acted upon
);

-- Audit log (append-only; no UPDATE/DELETE permitted by application)
CREATE TABLE audit_events (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL,
    actor_id        UUID,                   -- user or service account
    actor_type      VARCHAR(32) NOT NULL,   -- 'user','service','system'
    event_type      VARCHAR(128) NOT NULL,  -- e.g. 'pipeline.run.created'
    entity_type     VARCHAR(64) NOT NULL,   -- 'pipeline_run','deployment','approval'
    entity_id       UUID NOT NULL,
    payload         JSONB NOT NULL,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_audit_org_time  (org_id, occurred_at DESC),
    INDEX idx_audit_entity    (entity_type, entity_id, occurred_at DESC)
);

-- Users (minimal; full auth delegated to IdP)
CREATE TABLE users (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    email           VARCHAR(255) NOT NULL,
    display_name    VARCHAR(255),
    role            VARCHAR(32) NOT NULL DEFAULT 'developer',
                                            -- 'developer','release_engineer','admin'
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, email)
);
```

### Database Choice

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **PostgreSQL** | ACID transactions for pipeline state transitions; JSONB for flexible definition/manifest storage; mature ecosystem; strong index types; row-level security | Vertical scaling ceiling; requires careful sharding at >10 TB; operational complexity of read replicas | **Selected** |
| MySQL / Aurora MySQL | Widely understood; AWS Aurora MySQL offers auto-scaling storage | Less expressive query planner than Postgres; JSON support less mature; fewer index types | Viable but inferior for JSONB use cases |
| CockroachDB | Distributed SQL; no manual sharding; geo-distribution | Significantly higher latency for simple queries (Raft consensus overhead); operational complexity | Over-engineered for this scale initially |
| MongoDB | Schema flexibility; horizontal scaling native | No multi-document ACID in older versions; harder to enforce referential integrity across pipeline state machine | Rejecteddue to strong consistency requirements on job status transitions |
| DynamoDB | Predictable latency; serverless scaling; no ops | No complex queries; expensive for high-read workloads; limited secondary indexes | Suitable for audit log only, not general pipeline metadata |

**Selected: PostgreSQL** with the following rationale:
- Pipeline state transitions (job status: `queued → assigned → running → success/failed`) require serializable or at minimum repeatable-read transactions to prevent double-assignment of a job to two agents. PostgreSQL's advisory locks and `SELECT ... FOR UPDATE SKIP LOCKED` are purpose-built for job queue patterns.
- `JSONB` columns store pipeline definition YAML (parsed) and Kubernetes manifests without forcing a rigid schema that changes with every new feature, while still allowing GIN index queries.
- The audit log table is append-only; at high volume it can be migrated to a time-series store (TimescaleDB as a Postgres extension) without changing the application schema.
- Sharding strategy: shard by `org_id` once a single node approaches 2 TB. Use Citus (Postgres extension) for horizontal sharding while maintaining SQL semantics.

**Supporting stores:**
- **Redis**: Job queue (sorted set by priority), distributed locks for agent job assignment, pipeline run status cache to reduce DB reads from the UI.
- **S3**: Log files, artifacts. Cheaper per-GB than any database; lifecycle policies handle retention automatically.
- **Kafka**: Webhook event queue, intra-service event bus. Provides durability and replay for crash recovery.
- **Elasticsearch**: Full-text search over build logs for the UI's log search feature.

---

## 5. API Design

All APIs use REST over HTTPS with JSON bodies. Authentication via short-lived JWT (15-min expiry) issued by the auth service, refreshed via OAuth 2.0 refresh tokens. Service-to-service calls use mTLS with service account JWTs.

Rate limits enforced per-org via a token bucket in Redis. Default: 1,000 requests/min per org.

```
Base URL: https://api.cicd.example.com/v1
Auth header: Authorization: Bearer <jwt>
```

### Webhook Ingestion (public, no JWT — HMAC-signed)

```
POST /webhooks/{provider}/{project_id}
Headers:
  X-Hub-Signature-256: sha256=<hmac>   (GitHub format)
  X-Gitlab-Token: <secret>              (GitLab format)
Body: provider-specific webhook payload (JSON)

Response: 202 Accepted
{ "event_id": "evt_01HXZ..." }

Errors:
  401 if HMAC validation fails
  429 if rate limit exceeded (100 req/min per project)
  400 if payload cannot be parsed
```

### Pipeline Runs

```
# List pipeline runs for a project
GET /projects/{project_id}/runs
Query params:
  status    = pending|running|success|failed|cancelled (filter)
  branch    = <branch name> (filter)
  limit     = 1-100 (default 25)
  cursor    = <opaque cursor for keyset pagination>
Response: 200
{
  "runs": [ { "id", "status", "trigger_ref", "trigger_sha", "started_at", "finished_at", ... } ],
  "next_cursor": "<cursor>"
}

# Get single run with stages and jobs
GET /projects/{project_id}/runs/{run_id}
Response: 200
{
  "run": { ... },
  "stages": [
    {
      "id", "name", "sequence", "status",
      "jobs": [ { "id", "name", "status", "started_at", "finished_at", "log_url" } ]
    }
  ]
}

# Trigger a manual pipeline run
POST /projects/{project_id}/runs
Body: { "ref": "main", "sha": "<optional>", "variables": { "KEY": "VALUE" } }
Response: 201
{ "run_id": "...", "status": "pending" }

# Cancel a run
POST /projects/{project_id}/runs/{run_id}/cancel
Response: 200
{ "status": "cancelled" }

# Re-run failed jobs only
POST /projects/{project_id}/runs/{run_id}/retry
Body: { "scope": "failed" }  // or "all"
Response: 201
{ "new_run_id": "..." }
```

### Jobs

```
# Get job details + log URL
GET /projects/{project_id}/runs/{run_id}/jobs/{job_id}
Response: 200
{
  "job": { "id", "name", "status", "agent_id", "exit_code", "started_at", "finished_at" },
  "log_url": "https://logs.cicd.example.com/v1/jobs/{job_id}/stream"
}

# Stream job logs (Server-Sent Events)
GET /v1/jobs/{job_id}/stream
Headers: Accept: text/event-stream
Response: 200 (streaming)
data: {"line": 1, "text": "+ docker build ...", "timestamp": "2026-04-09T..."}
data: {"line": 2, ...}
...
event: done
data: {"exit_code": 0}
```

### Artifacts

```
# List artifacts for a run
GET /projects/{project_id}/runs/{run_id}/artifacts
Response: 200
{ "artifacts": [ { "id", "name", "artifact_type", "size_bytes", "sha256", "download_url", "expires_at" } ] }

# Generate pre-signed download URL
GET /artifacts/{artifact_id}/download
Response: 302 (redirect to pre-signed S3 URL, 15-min expiry)
```

### Deployments

```
# List deployments for an environment
GET /projects/{project_id}/deployments?environment=production&limit=25&cursor=...
Response: 200
{ "deployments": [ { "id", "status", "strategy", "artifact_id", "deployed_by", "started_at", ... } ] }

# Trigger a deployment (from a specific artifact)
POST /projects/{project_id}/deployments
Body: {
  "artifact_id": "...",
  "environment": "production",
  "strategy": "canary",
  "canary_steps": [5, 25, 100],
  "canary_step_duration_seconds": 600
}
Response: 201
{ "deployment_id": "..." }

# Rollback to a previous deployment
POST /projects/{project_id}/deployments/{deployment_id}/rollback
Response: 201
{ "rollback_deployment_id": "..." }
```

### Approval Gates

```
# List pending approvals (for release engineer UI)
GET /approvals?status=pending
Response: 200
{ "approvals": [ { "id", "pipeline_run_id", "environment", "requested_at", "expires_at" } ] }

# Approve or reject
POST /approvals/{approval_id}/decision
Body: { "decision": "approved" | "rejected", "reason": "<optional>" }
Response: 200
{ "approval_id": "...", "decision": "approved", "decided_by": "...", "decided_at": "..." }
```

### Agents (Platform Admin only — requires `admin` role)

```
# List agents
GET /agents?status=idle&label=docker&limit=50
Response: 200
{ "agents": [ { "id", "hostname", "status", "labels", "last_heartbeat" } ] }

# Agent self-registration (called by agent on startup)
POST /agents/register
Body: { "hostname": "...", "labels": ["docker","linux"], "version": "1.4.2" }
Response: 201
{ "agent_id": "...", "token": "<agent-specific JWT>" }

# Agent heartbeat + job poll
PUT /agents/{agent_id}/heartbeat
Body: { "status": "idle", "current_job_id": null }
Response: 200
{ "assigned_job": { "job_id": "...", "steps": [...], "secrets_token": "..." } | null }
```

---

## 6. Deep Dive: Core Components

### 6.1 Job Scheduling and Agent Assignment

**Problem it solves**: 37,500 concurrent jobs must be dispatched to the right agents with the right capabilities (e.g., a macOS build can only run on a macOS agent), without double-assigning a job or losing it during a controller crash.

**Approaches Comparison:**

| Approach | Mechanism | Pros | Cons |
|---|---|---|---|
| **Push model (controller → agent)** | Controller picks agent, sends job via gRPC/HTTP | Low latency; controller has full visibility | Controller must track agent availability in real-time; agent may be busy by the time message arrives |
| **Pull model (agent polls controller)** | Agents call `PUT /heartbeat` and receive job assignment | Agents self-report readiness; no stale routing | Polling interval introduces latency (min ~1-2s with long-polling) |
| **Queue-based (shared queue)** | Jobs enqueued in Redis ZSET; agents BLPOP or use Lua atomic dequeue | Simple; Redis handles concurrency natively; agents scale freely | Extra infrastructure; ordering guarantees weaker |
| **Hybrid: agent pull with long-poll** | Agent opens long-poll HTTP request (60s timeout); controller responds when a matching job exists | Near-push latency with pull simplicity; handles agent disappearance gracefully | Long-lived HTTP connections at scale (37,500 connections) |

**Selected Approach: Queue-based with Redis + Lua atomic assignment**

Rationale: Redis `ZSET` with priority scores and label-based job routing solves the core problem. The key operation — atomically pop a job matching a set of agent labels and mark it as assigned — is implemented as a Lua script executed atomically in Redis, preventing race conditions without distributed locks.

**Implementation — Lua atomic job dequeue:**

```lua
-- Keys: [jobs_zset_key, assigned_jobs_hash_key]
-- Args: [agent_id, agent_labels_json, current_time, job_visibility_timeout]
-- Returns: job_id or nil

local jobs_key = KEYS[1]           -- e.g., "queue:jobs:org:{org_id}"
local assigned_key = KEYS[2]       -- e.g., "assigned:jobs"
local agent_id = ARGV[1]
local agent_labels = cjson.decode(ARGV[2])
local now = tonumber(ARGV[3])
local visibility_timeout = tonumber(ARGV[4])  -- e.g., 30 seconds

-- Scan top N jobs by priority score (higher = higher priority)
local candidates = redis.call('ZREVRANGE', jobs_key, 0, 99, 'WITHSCORES')

for i = 1, #candidates, 2 do
    local job_id = candidates[i]
    local score  = tonumber(candidates[i+1])

    -- Fetch job metadata
    local meta = redis.call('HGETALL', 'job:meta:' .. job_id)
    local job = {}
    for j = 1, #meta, 2 do job[meta[j]] = meta[j+1] end

    -- Check label compatibility
    local required = cjson.decode(job['runner_labels'] or '[]')
    local match = true
    for _, label in ipairs(required) do
        local found = false
        for _, al in ipairs(agent_labels) do
            if al == label then found = true; break end
        end
        if not found then match = false; break end
    end

    if match then
        -- Atomic: remove from queue, record assignment
        redis.call('ZREM', jobs_key, job_id)
        redis.call('HSET', assigned_key, job_id, cjson.encode({
            agent_id = agent_id,
            assigned_at = now,
            deadline = now + visibility_timeout
        }))
        return job_id
    end
end

return nil
```

A reaper process runs every 10 seconds, scanning `assigned_jobs_hash_key` for entries where `deadline < now`, indicating the agent failed to acknowledge the job. The reaper re-enqueues such jobs (incrementing `retry_count`) and emits an alert if `retry_count > max_retries`.

**Interviewer Q&A:**

Q: How do you prevent a job from being double-dispatched if the Redis primary fails mid-Lua-script?
A: Lua scripts in Redis are atomic at the Redis-server level — they either complete fully or not at all (no partial execution). If the primary crashes before sending the response to the controller, the job may be re-enqueued by the reaper (because the assignment was never acknowledged). The controller and agent are designed to be idempotent: if a job is re-dispatched, the agent checks the Postgres job status before executing; if already `running` or `success`, it skips.

Q: How do you handle priority starvation of low-priority jobs when high-priority jobs keep arriving?
A: Jobs start with a base priority score computed as `(urgency_level × 1000) - (queued_duration_seconds)`. As time passes, the negative term decreases the score less negative — effectively aging up the job's priority. This ensures a job queued long enough will eventually reach the priority of newly arriving high-priority jobs.

Q: What happens if an agent machine is terminated (spot instance preemption) while running a job?
A: The agent sends a heartbeat every 10 seconds. The reaper detects a missed heartbeat (>30s threshold), marks the agent `offline` in Postgres, and re-enqueues all jobs assigned to that agent. The re-enqueued jobs have `retry_count` incremented; if the job is not idempotent (flagged in its definition), only `retry_count == 0` re-enqueue happens and an alert fires.

Q: How do you ensure a canary deployment stops automatically if the error rate spikes?
A: The Traffic Manager runs a continuous feedback loop: every 30 seconds during canary steps, it queries the metrics backend (Datadog/Prometheus) for the canary's HTTP 5xx rate and p99 latency. If either exceeds configured thresholds, the Traffic Manager calls the Rollback Engine to revert traffic to the previous deployment and marks the deployment `failed`. This is a synchronous polling loop — not event-driven — because it needs to make a definitive binary decision on each evaluation.

Q: How would you support matrix builds (e.g., test against Python 3.9, 3.10, 3.11 simultaneously)?
A: The Pipeline Controller expands matrix variables when constructing the job DAG. A stage with `matrix: {python: [3.9, 3.10, 3.11]}` produces 3 independent job records, each with `PYTHON_VERSION` set. These are enqueued separately, run in parallel, and all must succeed for the stage to pass. The matrix expansion is stored in the `pipeline_definitions` JSONB column after parsing.

---

### 6.2 Deployment Strategies (Blue-Green and Canary)

**Problem it solves**: Deploying new code to production without downtime, while enabling safe rollback if the new version misbehaves.

**Approaches Comparison:**

| Strategy | Mechanism | Downtime | Rollback Speed | Resource Cost | Risk |
|---|---|---|---|---|---|
| **Recreate** | Stop all old, start all new | Yes (minutes) | Redeploy old version (~minutes) | 1x | High — all users affected if failure |
| **Rolling** | Replace instances one by one | No | Slow (must roll all back) | 1x (sequential) | Medium — partial fleet runs new code |
| **Blue-Green** | Two identical environments; swap load balancer target | No | Instant (swap LB back) | 2x (both envs live) | Low — switch is atomic |
| **Canary** | Route small % to new version; increase if healthy | No | Fast (set weight to 0) | 1.1-1.5x | Very low — blast radius limited to canary % |
| **Shadow / Traffic Mirroring** | Mirror prod traffic to new version; don't serve responses | No | N/A (not serving) | 1.5x | None to users — but not a true deployment |

**Selected Approach: Blue-Green for stateless services; Canary for high-traffic critical paths**

Rationale:
- Blue-green gives instant, deterministic rollback by pointing the load balancer rule back to the blue environment. Ideal for services where even a brief canary period of bad behavior is unacceptable (auth services, payment APIs).
- Canary is optimal for high-traffic services where validating with real traffic on a small cohort is more reliable than staging test coverage. The traffic-weighted routing (via Kubernetes Ingress annotations or Envoy weighted clusters) allows fine-grained control.

**Implementation — Blue-Green swap pseudocode:**

```python
def execute_blue_green_deploy(deployment: Deployment) -> DeploymentResult:
    env = deployment.environment          # "production"
    project = get_project(deployment.project_id)
    artifact = get_artifact(deployment.artifact_id)

    # Step 1: Determine current active slot
    active_slot = get_active_slot(project.id, env)   # returns "blue" or "green"
    inactive_slot = "green" if active_slot == "blue" else "blue"

    # Step 2: Deploy new version to inactive slot
    k8s_deploy(
        namespace=f"{project.slug}-{env}-{inactive_slot}",
        image=artifact.storage_key,    # OCI image reference
        manifest=render_manifest(deployment.manifest_template, inactive_slot)
    )

    # Step 3: Wait for inactive slot to be healthy
    wait_for_rollout(
        namespace=f"{project.slug}-{env}-{inactive_slot}",
        timeout_seconds=300,
        health_check_url=project.health_check_url
    )

    # Step 4: Run smoke tests against inactive slot directly
    smoke_result = run_smoke_tests(
        target_url=f"https://{inactive_slot}.internal.{project.slug}.{env}/",
        test_suite=deployment.smoke_test_suite
    )
    if not smoke_result.passed:
        emit_audit_event("deployment.smoke_test_failed", deployment.id)
        return DeploymentResult.FAILED

    # Step 5: Atomic load balancer swap
    update_lb_rule(
        listener_arn=project.lb_listener_arn,
        target_group=f"{project.slug}-{env}-{inactive_slot}-tg"
    )

    # Step 6: Record new active slot
    set_active_slot(project.id, env, inactive_slot)
    record_deployment_success(deployment.id, previous_slot=active_slot)

    # Step 7: Keep old slot warm for fast rollback (for rollback_window_seconds)
    schedule_slot_teardown(
        project_id=project.id, slot=active_slot,
        after_seconds=project.rollback_window_seconds  # default: 3600
    )

    return DeploymentResult.SUCCESS

def rollback(deployment_id: str) -> DeploymentResult:
    deployment = get_deployment(deployment_id)
    previous_deployment = get_deployment(deployment.previous_deployment_id)
    active_slot = get_active_slot(deployment.project_id, deployment.environment)
    previous_slot = "blue" if active_slot == "green" else "green"

    # Verify the previous slot is still warm (not torn down)
    if not is_slot_warm(deployment.project_id, previous_slot):
        # Cold rollback: re-deploy old artifact
        return execute_blue_green_deploy(
            Deployment(artifact_id=previous_deployment.artifact_id, ...)
        )

    # Hot rollback: just swap the LB back
    update_lb_rule(
        listener_arn=get_project(deployment.project_id).lb_listener_arn,
        target_group=f"{get_project(deployment.project_id).slug}-{deployment.environment}-{previous_slot}-tg"
    )
    set_active_slot(deployment.project_id, deployment.environment, previous_slot)
    emit_audit_event("deployment.rolled_back", deployment.id)
    return DeploymentResult.SUCCESS
```

**Interviewer Q&A:**

Q: Blue-green requires 2x infrastructure. How do you justify this cost?
A: The inactive slot only needs to run at production scale during the deployment window (typically 30-60 minutes). For most of the day it can be scaled to near-zero (1 replica minimum for fast scale-up). The cost premium is roughly 15-20% extra on monthly infra, not 2x, because the inactive slot is idle most of the time. For high-stakes services (payments, auth), this cost is justified by the instant rollback capability.

Q: What about database schema changes? Blue-green doesn't help if the migration is destructive.
A: Correct. Database migrations must follow the expand/contract pattern. Phase 1 (expand): add new columns as nullable, deploy new code that writes to both old and new columns. Phase 2: run a background migration to fill new column from old data. Phase 3 (contract): deploy code that reads from the new column only, then drop the old column. Blue-green does not protect against migrations that break backward compatibility — that's an application-level discipline enforced via migration linting tools (e.g., `squawk` for Postgres migrations).

Q: How do canary weights translate to Kubernetes?
A: Using Kubernetes with Nginx Ingress or Istio. With Nginx Ingress: two Ingress resources for the same host — one for the stable deployment, one for the canary — with annotations `nginx.ingress.kubernetes.io/canary: "true"` and `nginx.ingress.kubernetes.io/canary-weight: "5"`. The Nginx Ingress controller distributes traffic accordingly. With Istio: a VirtualService with weighted routing between two Subsets (stable, canary) of a Service.

Q: How do you handle in-flight requests during the blue-green swap?
A: The load balancer rule update (e.g., AWS ALB target group swap) is close-to-instantaneous from the load balancer's perspective, but the load balancer drains existing connections to the old target group before fully routing new connections to the new target group. AWS ALB's deregistration delay (default 300s, can be reduced to 30s) ensures in-flight requests complete. New connections go to the new target group immediately.

Q: Can you support feature-flag-gated canary (only canary for users with a specific flag)?
A: Yes, but it requires coordinating with the Feature Flag Service. The Traffic Manager sets the canary traffic weight to 0% in the load balancer and instead uses a per-request header injection approach: the Feature Flag Service SDK, running in the application, routes flag-enabled users to an internal canary service directly (via service mesh header-based routing). This is header-based canary, not traffic-weight canary, and allows testing with specific user segments before any general canary rollout.

---

### 6.3 Test Parallelization

**Problem it solves**: A large test suite (e.g., 10,000 unit tests + 500 integration tests) taking 30 minutes sequentially must be split across N agents and completed in total wall-clock time of under 5 minutes.

**Approaches Comparison:**

| Approach | Split Strategy | Load Balancing | Overhead |
|---|---|---|---|
| **File-based split** | Divide test files evenly by count across agents | Poor — files vary wildly in test count and duration | Low |
| **Test-count-based split** | Split by number of test functions | Better than file-based | Medium (requires test discovery pass) |
| **Historical-duration-based split** | Use previous run timings to assign tests so each agent gets equal wall-clock time | Best — minimizes total run time | High (requires timing DB) |
| **Dynamic queue (work stealing)** | All tests in a shared queue; agents pull until empty | Optimal for heterogeneous agents | Highest — requires queue infrastructure |

**Selected: Historical-duration-based split with dynamic queue fallback**

For the first run (no history), fall back to file-count-based split. After the first run, store per-test timings and use a bin-packing algorithm for subsequent runs.

**Implementation — bin-packing splitter pseudocode:**

```python
def split_tests_for_parallelism(
    test_files: List[str],
    num_agents: int,
    timing_data: Dict[str, float],   # test_id -> avg_duration_seconds from history
    default_duration: float = 1.0    # assumed duration for tests with no history
) -> List[List[str]]:
    """
    Returns a list of num_agents buckets, each containing test file paths.
    Uses a greedy longest-processing-time (LPT) bin-packing heuristic.
    """
    # Assign timing estimates to each test file
    weighted_files = []
    for f in test_files:
        duration = timing_data.get(f, default_duration)
        weighted_files.append((f, duration))

    # Sort descending by estimated duration (LPT heuristic)
    weighted_files.sort(key=lambda x: x[1], reverse=True)

    # Initialize N buckets with (total_time, [files]) 
    buckets = [(0.0, []) for _ in range(num_agents)]

    for file_path, duration in weighted_files:
        # Assign to the bucket with the least current total time
        buckets.sort(key=lambda b: b[0])
        lightest_bucket_time, lightest_bucket_files = buckets[0]
        buckets[0] = (lightest_bucket_time + duration, lightest_bucket_files + [file_path])

    return [files for (_, files) in buckets]

def store_test_timings(run_id: str, job_id: str, junit_xml: str):
    """Parse JUnit XML from test run and persist per-test timings."""
    root = ET.fromstring(junit_xml)
    records = []
    for testcase in root.iter('testcase'):
        records.append({
            'test_id': f"{testcase.attrib['classname']}.{testcase.attrib['name']}",
            'duration_seconds': float(testcase.attrib.get('time', 0)),
            'run_id': run_id,
            'recorded_at': datetime.utcnow()
        })
    # Upsert with exponential moving average (alpha=0.3) to smooth outliers
    for r in records:
        upsert_timing_ema(r['test_id'], r['duration_seconds'], alpha=0.3)
```

After each run, the Test Result Aggregator merges JUnit XML files from all agents, deduplicates (same test should not run on two agents), and writes the merged results back to the pipeline run record.

**Interviewer Q&A:**

Q: What if a slow test ends up on every agent's bucket due to an error in timing data?
A: The LPT heuristic guarantees that each test file appears in exactly one bucket — no duplication. The `weighted_files` list is iterated exactly once; each item is assigned to exactly one bucket.

Q: How do you detect flaky tests that only fail intermittently?
A: The Test Result Aggregator tracks pass/fail per test across the last 100 runs using a time-windowed counter in Redis (or a `test_flakiness` table in Postgres). A test with a pass rate between 50% and 99% is flagged as flaky. Flaky tests are annotated in the UI and optionally quarantined (skipped from the blocking test suite) while filed as a separate non-blocking check.

Q: How do you handle tests that have setup/teardown dependencies and can't be split arbitrarily?
A: The pipeline YAML allows test files to specify `group` labels. Files with the same group are always co-located on the same agent. The splitter treats a group as an atomic unit. This is declared in the test config as `test_groups: [{name: "db_integration", files: ["tests/db/**"]}]`.

---

## 7. Scaling

### Horizontal Scaling

- **Webhook Gateway / API Servers**: Stateless; scaled horizontally behind an ALB. Auto-scaled based on CPU utilization > 60% or connection count. Minimum 3 instances across 3 AZs.
- **Pipeline Controllers**: Consume from Kafka partitions. Scale by adding instances; each instance owns a subset of Kafka partitions. Kafka consumer group rebalancing handles instance addition/removal. At 17 pipeline triggers/sec with each pipeline taking ~200ms to parse and schedule, 3 controller instances (each handling ~6 triggers/sec) is sufficient. Scale to 20+ for sustained bursts.
- **Job Schedulers**: Run as a single leader with hot standby (leader election via Redis lock). The critical path (job assignment) is in Redis, so the scheduler is lightweight. At scale, partition by org_id: each scheduler instance owns a hash ring segment of orgs.
- **Build Agents**: Scale out (new container/VM spun up) when queue depth exceeds threshold. Scale-in policy: terminate idle agents after 10 minutes. AWS EC2 Auto Scaling groups or Kubernetes cluster autoscaler manage this. Target: 37,500 concurrent agents at peak.
- **Log Aggregation**: Fluent Bit on each agent forwards to a Kafka topic `build-logs`. Consumers write to S3 in batches. Scale consumers based on Kafka consumer lag.

### DB Sharding

- **Primary shard key: `org_id`**. All tables with `org_id` can be co-located on the same shard. Pipeline runs, jobs, artifacts, audit events — all join naturally within an org.
- **Sharding implementation**: Citus (Postgres extension) distributes tables by `org_id`. The `organizations` table is a reference table (replicated to all shards). Cross-shard queries (platform-wide analytics) run against a read replica with a full copy.
- **Shard count**: Start with 8 shards, each handling ~62 orgs at launch. Shard count can grow by splitting shards (Citus supports this online).
- **Hot org problem**: A large enterprise org generating 10% of all traffic lands on one shard. Mitigate by dedicating a shard to the largest orgs (one org per shard if necessary).

### Replication

- **Postgres**: Primary + 2 read replicas per shard (synchronous replication to 1 replica for durability; asynchronous to the second for read scaling). Dashboard queries and log searches use read replicas. Writes (job status updates, pipeline creation) go to the primary.
- **Redis**: Redis Cluster with 3 primary shards and 1 replica each. Sharded by `{org_id}:{job_id}` key. Redis Sentinel manages failover.
- **S3**: Cross-region replication enabled for artifacts (us-east-1 → us-west-2). Artifacts are replicated asynchronously; a 5-minute replication lag is acceptable for artifact downloads.

### Caching

| Cache Target | Cache Layer | TTL | Invalidation Strategy |
|---|---|---|---|
| Pipeline run status | Redis | 30 s | Write-through on status update |
| Agent list + labels | Redis | 60 s | Write-through on heartbeat |
| Org rate limit counters | Redis | Per token bucket window | Sliding window |
| Project metadata (org_id, webhook_secret) | In-process LRU (per controller instance) | 5 min | TTL expiry; webhook secret change triggers cache eviction via Kafka event |
| Artifact download pre-signed URL | Not cached | N/A | Generated fresh per request (15-min S3 expiry) |

**Interviewer Q&As:**

Q: Kafka consumer lag on the log aggregation topic spikes during a CI burst. How do you handle it?
A: Increase the number of Kafka partitions on the `build-logs` topic and add more consumer instances proportionally. Kafka consumers are stateless (they write directly to S3 via batch multipart upload), so horizontal scaling is linear. Also, configure Fluent Bit on agents to buffer locally and batch-send, smoothing the upload burst.

Q: How do you handle Postgres failover with minimal pipeline disruption?
A: With synchronous replication to one replica, failover (via Patroni or AWS RDS Multi-AZ) completes in under 30 seconds. During failover, the application retries write operations with exponential backoff (max 60 seconds). Jobs in `running` state are not re-enqueued during failover — they continue running on agents (which don't require DB connectivity during execution). The DB is only needed at job start (assignment) and end (status update).

Q: The agent pool has 37,500 agents, each polling every 10 seconds. That's 3,750 requests/sec just from heartbeats. How do you handle this?
A: Long-polling: agents hold an HTTP connection open for up to 60 seconds. The server responds immediately if a job is available, or after 60 seconds with an empty response. This reduces heartbeat frequency by 6x and shifts load from the scheduler service to the connection layer. With 37,500 agents and 60s long-poll, the scheduler handles ~625 new connections/sec rather than 3,750 req/sec. Use a non-blocking I/O server (Node.js, Go's net/http, or Nginx with upstream hold) for the long-poll endpoint.

Q: How do you limit one noisy org from consuming all build capacity?
A: Each org has a `max_concurrency` field in the `organizations` table. The Job Scheduler tracks concurrent running jobs per org in a Redis counter (`running_jobs:{org_id}`). Before assigning a job, the scheduler checks if the counter is below `max_concurrency`; if not, the job stays queued. For enterprise orgs, dedicated agent pools (tagged with `org_id`) prevent any cross-org starvation.

Q: As artifact storage grows to 9 PB, S3 costs become significant. How do you optimize?
A: Multi-tier lifecycle policy: artifacts transition to S3 Intelligent-Tiering after 7 days (handles variable access patterns automatically), then to S3 Glacier Instant Retrieval after 30 days for infrequently accessed artifacts. Only the most recent 3 deployments' artifacts per environment are kept in S3 Standard. Additionally, deduplicate artifacts using content-addressable storage (SHA-256 hash): if two pipeline runs produce identical binary output (e.g., a no-op commit), the second run's artifact record points to the same S3 object.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Recovery |
|---|---|---|---|
| Webhook Gateway | Instance crash | ALB health check (10s interval) | ALB routes to other instances; no events lost (event already in flight or not yet received) |
| Kafka broker | Broker crash | Kafka controller detects leader failure | Partition leader election (<10s); producers retry with backoff; no message loss if replication factor ≥ 3 |
| Pipeline Controller | Process crash mid-pipeline | Controller watchdog (Kubernetes liveness probe) | Container restarts; on startup, controller scans `pipeline_runs` for `running` status with no recent heartbeat and resumes from last completed stage |
| Job Scheduler | Leader crash | Redis lock TTL expires (30s) | Standby acquires lock and resumes; in-flight assignments re-evaluated via reaper |
| Build Agent | Mid-job crash (preempted VM) | Heartbeat timeout (30s) | Reaper re-enqueues job; new agent picks it up |
| Postgres primary | Instance failure | Patroni/RDS Multi-AZ detects | Failover to replica in <30s; app retries writes with backoff |
| Redis primary | Instance failure | Redis Sentinel | Sentinel promotes replica; 10-15s outage; job queue in-flight items reprocessed by reaper |
| S3 (log writes) | S3 outage / throttling | Agent upload error | Agent buffers logs locally (up to 512MB), retries with exponential backoff; logs may be delayed but not lost |
| Deployment Controller | Crash mid-deploy | Kubernetes watches the Deployment resource | On restart, controller reads `deployments` table for `in_progress` records and re-reconciles with the actual cluster state |
| Target cluster (Kubernetes) | etcd failure / API server crash | Deployment Controller gets 5xx from kube-apiserver | Deployment Controller marks deployment `failed` after 3 retries over 90s; alerts fire; rollback must be triggered manually |

### Retries and Idempotency

- **Webhook deduplication**: The Gateway computes a fingerprint of the webhook payload (SHA-256 of provider + delivery ID). Before publishing to Kafka, it checks a Redis SET `seen_webhooks` (TTL: 24 hours). Duplicate deliveries (GitHub retries webhooks on non-2xx) are discarded.
- **Job execution idempotency**: Each job has a unique `id`. Agents check job status in Postgres before executing. If status is already `running` or `success` (from a previously dispatched copy), the agent skips execution and ACKs. This handles the reaper re-enqueue edge case.
- **Deployment idempotency**: Deployments write their target manifest to the `deployments` table before calling the Kubernetes API. The Kubernetes `apply --server-side` operation is idempotent by design — applying the same manifest twice is a no-op.
- **Retry policies**: Jobs support `max_retries` (default 0). The scheduler increments `retry_count` on failure and re-enqueues. Retry delay uses exponential backoff: `min(300, 5 × 2^retry_count)` seconds.

### Circuit Breaker

The Deployment Controller wraps calls to the Kubernetes API server in a circuit breaker (Hystrix or Resilience4j pattern):
- **Closed**: Normal operation. On 5+ consecutive failures within 30 seconds, trip to Open.
- **Open**: Immediately return error without calling the API. After 60 seconds, move to Half-Open.
- **Half-Open**: Allow one request. If it succeeds, move to Closed. If it fails, move back to Open.

The circuit breaker prevents the Deployment Controller from hammering a degraded cluster with retry storms, which could worsen its recovery. When Open, deployments are queued and operators are alerted.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `pipeline.trigger.latency_p99` | Histogram | > 30s | Webhook-to-job-scheduled SLA |
| `pipeline.queue.depth` | Gauge | > 10,000 | Indicates under-provisioned agent pool |
| `pipeline.job.failure_rate` | Counter (rate) | > 15% over 5min | Signals systemic build failures (bad deploy or infra issue) |
| `agent.heartbeat.missed_rate` | Counter (rate) | > 5% | Agent instability (spot instance reclaims, OOM) |
| `deployment.rollback_rate` | Counter (rate) | > 1/hour per env | Signals bad artifact quality reaching production |
| `kafka.consumer.lag.webhook_events` | Gauge | > 5,000 | Indicates controller processing backlog |
| `kafka.consumer.lag.build_logs` | Gauge | > 100,000 | Log aggregation falling behind |
| `postgres.replication.lag_seconds` | Gauge | > 10s | Replica falling behind primary |
| `redis.memory.used_percent` | Gauge | > 80% | Risk of eviction of job queue data |
| `s3.artifact.upload.error_rate` | Counter (rate) | > 1% | Storage issues affecting artifact persistence |
| `api.request.error_rate_5xx` | Counter (rate) | > 0.1% | API layer health |

### Distributed Tracing

Every pipeline trigger generates a root trace span with `trace_id = pipeline_run_id`. Child spans are created for:
- Webhook validation and Kafka publish
- Pipeline YAML fetch and parse
- Each job's lifecycle (queued → assigned → running → complete)
- Artifact upload
- Deployment steps (health check, LB swap)

Traces are exported to Jaeger or Datadog APM via OpenTelemetry SDK. Agents propagate `traceparent` headers in all outbound HTTP calls. This allows tracing a full pipeline run end-to-end, identifying which stage introduced latency.

### Logging

- **Structured logging**: All services emit JSON logs with fields: `timestamp`, `level`, `service`, `trace_id`, `span_id`, `org_id`, `pipeline_run_id`, `job_id`, `message`, `error` (if applicable).
- **Log levels**: `DEBUG` disabled in production. `INFO` for lifecycle events. `WARN` for retried operations. `ERROR` for failures with stack traces.
- **Build logs**: Streamed from agents via Fluent Bit → Kafka → S3. Indexed in Elasticsearch for the log search UI.
- **Sensitive data scrubbing**: Log forwarder applies regex scrubbing rules to remove patterns matching API keys, tokens, passwords before writing to S3 or Elasticsearch.
- **Audit logs**: Written synchronously to the `audit_events` table within the same DB transaction as the state change they record.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Webhook ingest decoupling | Kafka message queue | Direct controller invocation | Kafka provides durability and replay on controller crash; direct call loses events if controller is down |
| Job assignment mechanism | Redis ZSET + Lua atomic dequeue | Postgres-based job queue (`SELECT FOR UPDATE SKIP LOCKED`) | Redis provides sub-millisecond lock contention resolution at 37,500 concurrent agents; Postgres advisory locks would become a bottleneck |
| Agent model | Pull (long-poll) | Push (controller → agent gRPC) | Pull model handles agent churn (spot instance terminations) gracefully; push requires the controller to maintain accurate agent liveness state |
| Log storage | S3 + Elasticsearch index | Postgres large objects / MongoDB | S3 is the cheapest durable store at 54 TB/day; Elasticsearch provides full-text search without loading logs into memory |
| Database | PostgreSQL + Citus sharding | DynamoDB | Postgres `SELECT FOR UPDATE SKIP LOCKED` is purpose-built for job queues; DynamoDB cannot express this pattern without application-level locking |
| Deployment strategy | Blue-green + canary | Rolling only | Blue-green provides instant rollback (atomic LB swap); canary limits blast radius; rolling is a poor choice for stateful services |
| Sharding key | `org_id` | `pipeline_run_id` | Sharding by `org_id` co-locates all data for a single org on one shard, enabling single-shard queries for 99% of operations |
| Secret injection | Vault short-lived token per job | Environment variables stored in DB | Vault tokens expire after job completion; no secret at rest in the CI/CD database; minimizes secret leakage surface |

---

## 11. Follow-up Interview Questions

**Q1: How would you support monorepo builds where only changed packages should be rebuilt?**
A: Implement a change detection layer in the Pipeline Controller. After fetching the pushed commit, diff the changed file paths against a monorepo package map (defined in the repo's `workspace.json`). Only packages with changed files (or that depend on changed packages via a dependency graph traversal — affected packages analysis) have their jobs enqueued. Tools like Nx, Turborepo, and Bazel implement this natively; the CI/CD system can integrate with their query interfaces.

**Q2: How do you handle secrets rotation without rebuilding running pipelines?**
A: Secrets are injected at job start via a short-lived Vault token. Rotation updates the Vault value. Running jobs have already read the secret at start and hold it in memory for the job's duration (~3 min average). Post-rotation, all new jobs get the new secret. For jobs running during rotation, the application code using the secret should handle `401` responses by requesting a fresh token — but this is application-level concern, not CI/CD concern.

**Q3: How would you implement pipeline caching (e.g., caching `node_modules`) to speed up builds?**
A: Implement a content-addressable cache store: the cache key is the SHA-256 hash of the cache key expression (e.g., hash of `package-lock.json` + OS + Node version). Agents check S3 for a matching cache object before the install step. If present, download and restore. On job completion, if the cache key was a miss, upload the directory as a compressed tarball. Cache entries have a 7-day TTL. The cache store is separate from the artifact registry (different S3 bucket, different lifecycle policy).

**Q4: How do you ensure a pipeline YAML that introduces an infinite loop or extremely expensive build doesn't DoS the system?**
A: Multiple limits: (1) Pipeline YAML is validated against a schema with a maximum stage count (50), maximum job count per stage (100), and maximum step count per job (200). (2) Each job has a `timeout_seconds` limit (default 3600, max 7200). The agent enforces the timeout and terminates the process group. (3) Org-level concurrency limits prevent a single org from consuming the entire agent pool. (4) YAML size limit (256 KB) prevents unbounded pipeline definitions.

**Q5: How would you implement compliance requirements where certain deployments need two-person approval?**
A: The Approval Gate supports a `required_approvers` policy per environment: `{ "min_approvers": 2, "required_roles": ["release_engineer"] }`. The gate blocks until the count of `approved_by` entries in `approval_requests` (extended to a one-to-many `approval_decisions` table) reaches the minimum. The gate also enforces that the deploying engineer cannot be one of the approvers (self-approval prevention), checked at decision time.

**Q6: How do you design the CI/CD system for multi-cloud deployments where the same artifact must be deployed to AWS, Azure, and GCP?**
A: The deployment stage in the pipeline YAML specifies multiple `targets`, each with a `provider` (aws/azure/gcp) and `strategy`. The Deployment Controller dispatches a sub-deployment for each target in parallel. Each target uses a provider-specific adapter (implementing a `DeploymentAdapter` interface). Results from all targets must succeed for the pipeline stage to pass. Credentials for each cloud are stored separately in Vault with cloud-specific auth methods (AWS IAM role, Azure Managed Identity, GCP Workload Identity).

**Q7: How would you implement automated rollback based on business metrics (not just error rates)?**
A: Extend the Traffic Manager's canary health check to query a business metrics API (e.g., Stripe's conversion rate via a metrics aggregator). Operators define SLO thresholds: `{ "metric": "checkout_conversion_rate", "threshold_pct": -5, "evaluation_window_minutes": 15 }`. The Traffic Manager evaluates: `(canary_metric - baseline_metric) / baseline_metric < threshold_pct`. If true, automatic rollback triggers. This requires a metrics comparison API that can isolate canary traffic from baseline traffic, which is achievable with Istio telemetry or feature-flag-based segment tracking.

**Q8: How do you handle a Git provider (GitHub) being unavailable when a webhook fires?**
A: The webhook event carries the commit SHA and branch name. The Pipeline Controller can proceed with build steps that don't require fetching repository content (e.g., if the repository was pre-cached). For steps requiring a code checkout, agents attempt `git clone` directly from the provider with a retry policy (3 retries, 30s apart). If GitHub is down for >5 minutes, the job fails with a retryable error. The pipeline run enters `waiting_retry` state and retries automatically when the next heartbeat from the agent indicates success.

**Q9: How do you prevent a developer from bypassing required checks by force-pushing to the main branch?**
A: This is a source control provider concern (GitHub branch protection rules, GitLab protected branches). The CI/CD system enforces it at the pipeline level by checking the `required_status_checks` configuration for the target branch. If a pipeline run for the main branch shows `trigger_type = push` but no corresponding `pull_request` stage run has passed, the Deployment Controller rejects the deployment to staging/production. Additionally, the Webhook Gateway records the push event; if it's a force push (`before` SHA is not an ancestor of `after` SHA), a high-severity audit event fires.

**Q10: How would you implement build cost attribution per team or service?**
A: Tag each job with `team_id` and `service_id` (from project metadata). Track: `job_duration_seconds`, `agent_instance_type` (with associated cost per second from a pricing table), `artifact_size_bytes` (with S3 storage cost). A nightly batch job aggregates these into a `cost_attribution` table. Costs = `(duration × instance_cost_per_second) + (artifact_size × s3_cost_per_gb)`. Expose a cost dashboard API and allow setting budget alerts per team.

**Q11: What's the maximum pipeline trigger throughput this design can handle, and what's the bottleneck?**
A: Current design: Webhook Gateway (stateless, N instances) → Kafka (partitioned, high throughput, essentially unlimited) → Pipeline Controllers (CPU-bound on YAML parsing, ~200ms/pipeline). With 3 controller instances handling 5 pipelines/sec each = 15 pipelines/sec = 1.3M pipelines/day. To scale beyond: increase Kafka partitions, add more controller instances (scale linearly). Bottleneck: Pipeline Controller CPU for YAML parsing. Mitigation: cache parsed definitions by SHA (most pushes are to the same pipeline definition; only the SHA differs — but the YAML is the same between commits if the YAML file wasn't changed). This cache hit rate would be ~80%+, raising effective throughput by 5x.

**Q12: How do you design the agent for secure secret handling?**
A: Agent requests a scoped Vault token at job start (using the agent's mTLS client certificate as auth). The token grants read access to only the secrets paths configured for this specific project × environment. Secrets are loaded into environment variables in-memory; they are never written to disk or logged. After job completion, the agent calls `vault token revoke` to immediately invalidate the token. The agent process runs as a non-root user in a container with `no-new-privileges` and a seccomp profile.

**Q13: How do you design the system to support both SaaS (cloud-hosted agents) and self-hosted (bring your own agents)?**
A: The agent is a standalone binary that registers with the Controller API over HTTPS. Self-hosted agents run in the customer's environment; they establish outbound connections only (no inbound firewall rules needed). The Controller treats self-hosted agents identically to cloud agents. Orgs configure their agents as a dedicated pool via the `org_id` field on the agent record. The Job Scheduler prefers org-specific agents for a project's jobs; only if the org pool is exhausted does it fall back to the shared cloud pool (configurable per org).

**Q14: How would you detect and prevent supply chain attacks (e.g., a compromised build step that exfiltrates secrets)?**
A: Multiple mitigations: (1) Pin action/step versions to full SHAs rather than mutable tags (enforced by pipeline YAML linter). (2) Run agents in network-isolated environments with outbound allowlists (only specific registry and source control hostnames allowed). (3) Log all outbound network connections from agent containers (eBPF-based network monitoring). (4) Scan artifact SBOMs (Software Bill of Materials) against vulnerability databases post-build. (5) Require code review (CODEOWNERS) for changes to pipeline YAML files in the repository.

**Q15: How do you handle a pipeline that takes 45 minutes end-to-end? What's your optimization strategy?**
A: Analyze the pipeline DAG for parallelization opportunities: (1) Move independent jobs to parallel stages. (2) Apply test parallelization (split 45-min test suite across 15 agents = 3-min wall time). (3) Implement layer caching for Docker builds (cache from previous build's layers). (4) Apply dependency caching (node_modules, Maven, Gradle). (5) Move slow integration tests to a separate non-blocking pipeline triggered post-merge rather than pre-merge. (6) Use incremental builds (monorepo affected analysis). With these optimizations, a 45-minute pipeline typically reduces to 5-8 minutes.

---

## 12. References & Further Reading

- **Humble, J. & Farley, D. (2010). *Continuous Delivery: Reliable Software Releases through Build, Test, and Deployment Automation*. Addison-Wesley.** — Foundational text on pipeline design, deployment strategies, and environment promotion.
- **Kim, G., Behr, K., & Spafford, G. (2013). *The Phoenix Project*. IT Revolution Press.** — DevOps cultural and process context.
- **Kubernetes Documentation — Deployments and Rolling Updates**: https://kubernetes.io/docs/concepts/workloads/controllers/deployment/
- **Istio Traffic Management (Canary Deployments)**: https://istio.io/latest/docs/tasks/traffic-management/traffic-shifting/
- **GitHub Actions Architecture (official docs)**: https://docs.github.com/en/actions/learn-github-actions/understanding-github-actions
- **Redis Documentation — EVAL and Lua scripting**: https://redis.io/docs/manual/programmability/eval-intro/
- **PostgreSQL Documentation — SELECT FOR UPDATE SKIP LOCKED**: https://www.postgresql.org/docs/current/sql-select.html
- **HashiCorp Vault — Dynamic Secrets**: https://developer.hashicorp.com/vault/docs/secrets
- **AWS ALB — Target Group Weighted Routing (canary)**: https://docs.aws.amazon.com/elasticloadbalancing/latest/application/load-balancer-listeners.html
- **Netflix Tech Blog — Spinnaker deployment system**: https://netflixtechblog.com/the-evolution-of-continuous-delivery-at-netflix-d4db3aeab5ab
- **Greenberg, A. et al. (LPT algorithm)**: Graham, R. L. (1969). Bounds on Multiprocessing Timing Anomalies. *SIAM Journal on Applied Mathematics*, 17(2), 416–429. — Theoretical basis for bin-packing test parallelization.
- **OpenTelemetry Specification**: https://opentelemetry.io/docs/specs/otel/
- **Citus (distributed Postgres)**: https://www.citusdata.com/product/community
- **AWS S3 Intelligent-Tiering**: https://aws.amazon.com/s3/storage-classes/intelligent-tiering/
- **Squawk (Postgres migration linter)**: https://squawkhq.com/
