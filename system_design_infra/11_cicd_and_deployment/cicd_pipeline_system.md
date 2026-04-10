# System Design: CI/CD Pipeline System

> **Relevance to role:** A cloud infrastructure platform engineer owns the CI/CD backbone that ships code to bare-metal hosts, VMs, and Kubernetes clusters. You must understand how builds are scheduled across shared compute, how artifacts flow through promotion gates, and how pipelines integrate with job schedulers, container registries, and GitOps controllers. Interviewers expect you to discuss build isolation, test parallelism, artifact integrity, and deployment orchestration at the scale of thousands of microservices.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|-------------|
| FR-1 | Developers push code to Git; the system automatically triggers a pipeline (build, test, package, publish, deploy). |
| FR-2 | Support pipeline-as-code: Jenkinsfile, `.github/workflows/*.yml`, Tekton PipelineRun YAML. |
| FR-3 | Parallel test execution with test sharding and job matrices. |
| FR-4 | Multi-stage artifact promotion: dev -> staging -> prod with manual/automatic approval gates. |
| FR-5 | Docker image build with BuildKit, layer caching, and multi-stage builds. |
| FR-6 | Java build support: Maven/Gradle with dependency caching, unit + integration + contract tests. |
| FR-7 | Security scanning at build time: SAST (SonarQube), DAST, container image scanning (Trivy). |
| FR-8 | Deployment to Kubernetes via Helm/Kustomize/ArgoCD (GitOps). |
| FR-9 | Deployment to bare-metal/VM fleets via rolling update orchestrator. |
| FR-10 | Notifications on build/deploy status: Slack, email, PagerDuty. |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Pipeline trigger latency (webhook to first task start) | < 10 s |
| NFR-2 | Build queue wait time (p95) | < 30 s |
| NFR-3 | System availability | 99.95% |
| NFR-4 | Concurrent pipeline runs | 5,000+ |
| NFR-5 | Artifact publish durability | 99.999999999% (11 nines, S3-backed) |
| NFR-6 | Audit log retention | 2 years |
| NFR-7 | Secret zero-knowledge: secrets never written to build logs | 100% |

### Constraints & Assumptions
- Organization has ~3,000 engineers and ~1,500 microservices.
- Builds run in ephemeral containers on Kubernetes (no persistent build hosts).
- Source of truth is GitHub Enterprise; webhooks trigger pipelines.
- Artifacts are Docker images and Maven/PyPI packages.
- Kubernetes clusters span 3 regions.

### Out of Scope
- IDE integrations and local developer tooling.
- Monorepo build graph optimization (e.g., Bazel remote execution).
- ChatOps beyond Slack notifications.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Engineers | Given | 3,000 |
| Repos | ~1,500 services + libs | ~2,000 |
| Commits/day | 3,000 eng x 4 commits/day | 12,000 |
| Pipeline triggers/day | 12,000 commits + 3,000 PR events + 500 scheduled | ~15,500 |
| Peak triggers/hour | 3x average (post-standup burst) | ~3,900 |
| Peak triggers/second | 3,900 / 3,600 | ~1.1 |
| Avg tasks per pipeline | Build + 8 test shards + scan + package + publish + deploy | ~13 |
| Peak concurrent tasks | 1.1 pipelines/s x 13 tasks x avg 180s duration | ~2,574 |
| Build pods needed (peak) | 2,574 x 1.3 headroom | ~3,350 pods |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Webhook receipt to pipeline scheduled | < 2 s |
| Pipeline scheduled to first task running | < 10 s |
| Docker build (with cache hit) | < 60 s |
| Maven build (with cache) | < 120 s |
| Full pipeline (build + test + scan + publish + deploy) | < 15 min (p95) |
| Artifact pull from registry | < 5 s |

### Storage Estimates

| Item | Calculation | Value |
|------|-------------|-------|
| Docker images/day | 15,500 pipelines x 0.6 produce image | ~9,300 images |
| Avg image size (compressed) | Multi-stage Java Spring Boot | ~150 MB |
| Daily image storage | 9,300 x 150 MB | ~1.4 TB/day |
| Monthly (before dedup + retention) | 1.4 TB x 30 | ~42 TB/month |
| After layer dedup (~60% savings) | 42 x 0.4 | ~17 TB/month |
| Build logs/day | 15,500 x 500 KB avg | ~7.75 GB/day |
| Log retention (90 days) | 7.75 x 90 | ~700 GB |
| Audit events/day | 15,500 x 13 tasks x 3 events | ~604K events |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Maven dependency download (cache miss) | 500 builds/day x 200 MB | 100 GB/day |
| Docker push to registry | 9,300 x 150 MB | ~1.4 TB/day |
| Docker pull (deploys, 3 regions) | 9,300 x 3 x 150 MB x 0.3 (cache) | ~1.25 TB/day |
| Build log ingestion | ~7.75 GB/day | |

---

## 3. High Level Architecture

```
                        +-------------------+
                        |   GitHub Webhooks  |
                        +--------+----------+
                                 |
                                 v
                     +-----------+-----------+
                     |   Webhook Gateway     |  (validates HMAC, deduplicates)
                     |   (Java / Spring)     |
                     +-----------+-----------+
                                 |
                          +------v------+
                          |  Kafka      |  pipeline.trigger topic
                          |  (Events)   |
                          +------+------+
                                 |
                +----------------v-----------------+
                |       Pipeline Controller        |
                |   (Tekton / Argo Workflows)      |
                |   Reads pipeline-as-code YAML    |
                |   Creates DAG of tasks           |
                +-------+--------+--------+--------+
                        |        |        |
              +---------+   +----+---+   ++----------+
              |             |        |               |
         +----v----+  +----v----+  +----v----+  +----v----+
         |  Build  |  |  Test   |  |  Scan   |  | Package |
         |  Task   |  |  Tasks  |  |  Tasks  |  |  Task   |
         | (Pod)   |  | (Pods)  |  | (Pods)  |  |  (Pod)  |
         +---------+  +---------+  +---------+  +----+----+
              |                                       |
              v                                       v
     +--------+--------+                  +-----------+---------+
     | Build Cache     |                  |  Artifact Registry  |
     | (S3 / Redis)    |                  |  (Docker + Maven)   |
     +-----------------+                  +-----------+---------+
                                                      |
                                          +-----------v-----------+
                                          |   Promotion Gates     |
                                          |  dev -> staging ->    |
                                          |       prod            |
                                          +-----------+-----------+
                                                      |
                             +------------------------+------------------------+
                             |                        |                        |
                     +-------v-------+       +--------v--------+      +--------v-------+
                     |  ArgoCD       |       |  Bare-Metal     |      |  VM Deploy     |
                     |  (GitOps k8s) |       |  Rollout Ctrl   |      |  (Ansible)     |
                     +---------------+       +-----------------+      +----------------+
                                                      |
                                          +-----------v-----------+
                                          |   Notification Svc    |
                                          |  Slack / Email / PD   |
                                          +-----------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| Webhook Gateway | Receives GitHub webhooks, validates HMAC-SHA256 signature, deduplicates events, publishes to Kafka. Java Spring Boot service behind an ALB. |
| Kafka (Event Bus) | Decouples webhook ingestion from pipeline scheduling. Topics: `pipeline.trigger`, `pipeline.status`, `artifact.published`. Provides replay on failure. |
| Pipeline Controller | Reads pipeline definition from repo (Tekton PipelineRun or Argo Workflow YAML), creates a DAG of tasks, schedules pods on Kubernetes. Manages retries, timeouts, and conditional steps. |
| Build Task Pod | Ephemeral pod that checks out source, runs Maven/Gradle build, produces a JAR/WAR. Mounts build cache from S3 or Redis. |
| Test Task Pods | Parallel pods for test sharding. Job matrix splits tests by module/class. Each shard reports JUnit XML. |
| Scan Tasks | SonarQube SAST, Trivy container scan, OWASP dependency check. Results posted as GitHub PR comments. |
| Package Task | Builds Docker image via BuildKit (multi-stage), tags with Git SHA + semver. |
| Artifact Registry | Stores Docker images and Maven packages. Content-addressable, layer-deduplicated. See `artifact_registry.md`. |
| Promotion Gates | Policy engine: requires scan pass, test pass, manual approval (for prod). Modeled as Tekton ApprovalTask or Argo gate. |
| ArgoCD (GitOps) | Watches a config repo; when artifact version is bumped, syncs the Kubernetes Deployment. |
| Bare-Metal Rollout Controller | Orchestrates rolling update across bare-metal fleet in waves. See `rollout_controller.md`. |
| Notification Service | Consumes `pipeline.status` from Kafka, sends Slack/email/PagerDuty alerts on failure or completion. |

### Data Flows
1. **Trigger**: Developer pushes commit -> GitHub webhook -> Gateway validates HMAC -> Kafka `pipeline.trigger`.
2. **Schedule**: Pipeline Controller consumes event, clones repo, parses pipeline YAML, creates task DAG.
3. **Build**: Build pod checks out code, restores Maven/Gradle cache from S3, compiles, uploads cache.
4. **Test**: N test shard pods run in parallel; results aggregated; if any fail, pipeline halts.
5. **Scan**: SAST + container scan run in parallel with later test stages. Findings block promotion if severity >= HIGH.
6. **Package**: Docker image built with BuildKit, pushed to Artifact Registry with SHA tag.
7. **Promote**: Promotion gate checks: all tests green, scans clean, optional manual approval. Tags image as `staging` then `prod`.
8. **Deploy**: ArgoCD detects new image tag in config repo (auto-committed by pipeline), rolls out to k8s. Or rollout controller pushes to bare-metal fleet.
9. **Notify**: Status events flow to Notification Service; Slack message sent to channel.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Pipeline definition (parsed from YAML, stored for audit)
CREATE TABLE pipeline_definitions (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    repo_slug       VARCHAR(255) NOT NULL,          -- e.g., "org/service-foo"
    branch_pattern  VARCHAR(255) NOT NULL,          -- e.g., "main", "release/*"
    yaml_path       VARCHAR(512) NOT NULL,          -- e.g., ".tekton/pipeline.yaml"
    yaml_hash       CHAR(64) NOT NULL,              -- SHA-256 of YAML content
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_repo_branch (repo_slug, branch_pattern)
) ENGINE=InnoDB;

-- Pipeline run (one per trigger)
CREATE TABLE pipeline_runs (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    pipeline_def_id BIGINT NOT NULL,
    trigger_type    ENUM('push','pull_request','schedule','manual') NOT NULL,
    trigger_ref     VARCHAR(255) NOT NULL,          -- branch or tag
    commit_sha      CHAR(40) NOT NULL,
    status          ENUM('pending','running','succeeded','failed','cancelled') NOT NULL DEFAULT 'pending',
    started_at      DATETIME,
    finished_at     DATETIME,
    duration_ms     INT GENERATED ALWAYS AS (TIMESTAMPDIFF(SECOND, started_at, finished_at) * 1000) STORED,
    triggered_by    VARCHAR(255) NOT NULL,          -- username or "webhook"
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (pipeline_def_id) REFERENCES pipeline_definitions(id),
    INDEX idx_repo_status (pipeline_def_id, status),
    INDEX idx_commit (commit_sha),
    INDEX idx_created (created_at)
) ENGINE=InnoDB;

-- Individual task within a pipeline run
CREATE TABLE pipeline_tasks (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    pipeline_run_id BIGINT NOT NULL,
    task_name       VARCHAR(255) NOT NULL,          -- e.g., "build", "test-shard-3"
    task_type       ENUM('build','test','scan','package','publish','deploy','approval','notify') NOT NULL,
    status          ENUM('pending','running','succeeded','failed','skipped','cancelled') NOT NULL DEFAULT 'pending',
    pod_name        VARCHAR(255),                   -- k8s pod name
    node_name       VARCHAR(255),                   -- k8s node
    started_at      DATETIME,
    finished_at     DATETIME,
    exit_code       INT,
    retry_count     INT NOT NULL DEFAULT 0,
    log_url         VARCHAR(1024),                  -- S3 pre-signed URL
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (pipeline_run_id) REFERENCES pipeline_runs(id),
    INDEX idx_run_task (pipeline_run_id, task_name),
    INDEX idx_status (status)
) ENGINE=InnoDB;

-- Artifact produced by a pipeline
CREATE TABLE artifacts (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    pipeline_run_id BIGINT NOT NULL,
    artifact_type   ENUM('docker_image','maven_jar','pypi_package','npm_package','binary') NOT NULL,
    name            VARCHAR(512) NOT NULL,          -- e.g., "registry.internal/org/svc:abc123"
    digest          CHAR(71) NOT NULL,              -- sha256:hex
    size_bytes      BIGINT NOT NULL,
    promotion_stage ENUM('dev','staging','prod') NOT NULL DEFAULT 'dev',
    scan_status     ENUM('pending','clean','warning','critical') NOT NULL DEFAULT 'pending',
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (pipeline_run_id) REFERENCES pipeline_runs(id),
    INDEX idx_name_digest (name, digest),
    INDEX idx_promotion (promotion_stage)
) ENGINE=InnoDB;

-- Approval gate records
CREATE TABLE approval_gates (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    pipeline_run_id BIGINT NOT NULL,
    gate_name       VARCHAR(255) NOT NULL,          -- e.g., "prod-deploy-approval"
    required_approvers INT NOT NULL DEFAULT 1,
    status          ENUM('pending','approved','rejected','timed_out') NOT NULL DEFAULT 'pending',
    decided_at      DATETIME,
    decided_by      VARCHAR(255),
    expires_at      DATETIME NOT NULL,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (pipeline_run_id) REFERENCES pipeline_runs(id),
    INDEX idx_run_gate (pipeline_run_id, gate_name)
) ENGINE=InnoDB;

-- Build cache metadata
CREATE TABLE build_cache_entries (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    repo_slug       VARCHAR(255) NOT NULL,
    cache_key       VARCHAR(512) NOT NULL,          -- hash of lockfile + build tool version
    storage_path    VARCHAR(1024) NOT NULL,          -- S3 path
    size_bytes      BIGINT NOT NULL,
    last_used_at    DATETIME NOT NULL,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_repo_key (repo_slug, cache_key),
    INDEX idx_last_used (last_used_at)
) ENGINE=InnoDB;
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| MySQL 8.0 | Pipeline runs, tasks, artifacts, approvals | Transactional consistency for state machines; familiar to ops teams; InnoDB row-level locking for concurrent updates. |
| Elasticsearch | Build log search, pipeline run analytics | Full-text search over logs; aggregation for dashboards (avg build time by repo, flaky test detection). |
| Redis | Build cache index, distributed locks, rate limiting | Sub-ms latency for cache key lookups; `SETNX` for distributed locking on pipeline dedup. |
| S3 | Build logs, build caches, large artifacts | Durable, cheap, content-addressable. 11-nines durability. |
| Kafka | Event bus (triggers, status updates) | Decouples components, enables replay, handles burst traffic. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| pipeline_runs | (pipeline_def_id, status) | Dashboard: "show me all running pipelines for repo X" |
| pipeline_runs | (commit_sha) | "What pipeline ran for this commit?" |
| pipeline_runs | (created_at) | Time-range queries for analytics |
| pipeline_tasks | (pipeline_run_id, task_name) | Task lookup within a run |
| artifacts | (name, digest) | Artifact lookup by image name + digest |
| artifacts | (promotion_stage) | "Show all artifacts promoted to prod today" |

**Elasticsearch indices:**
- `build-logs-YYYY.MM.DD`: Daily index for build logs, 30-day hot, 90-day warm, then delete.
- `pipeline-metrics`: Pipeline duration, test counts, flaky test tracking.

---

## 5. API Design

### REST Endpoints

```
# Pipeline management
POST   /api/v1/pipelines                          # Register a pipeline definition
GET    /api/v1/pipelines?repo={slug}               # List pipelines for a repo
GET    /api/v1/pipelines/{id}                      # Get pipeline definition

# Pipeline runs
POST   /api/v1/pipelines/{id}/runs                 # Trigger a manual run
GET    /api/v1/pipelines/{id}/runs?status={s}       # List runs
GET    /api/v1/runs/{runId}                         # Get run details + tasks
GET    /api/v1/runs/{runId}/tasks/{taskId}/logs      # Stream task logs (SSE)
POST   /api/v1/runs/{runId}/cancel                  # Cancel a running pipeline
POST   /api/v1/runs/{runId}/retry                   # Retry failed tasks

# Approval gates
POST   /api/v1/runs/{runId}/gates/{gateId}/approve  # Approve a gate
POST   /api/v1/runs/{runId}/gates/{gateId}/reject   # Reject a gate

# Artifacts
GET    /api/v1/artifacts?pipeline_run_id={id}       # List artifacts for a run
POST   /api/v1/artifacts/{id}/promote               # Promote artifact to next stage
GET    /api/v1/artifacts/{id}/scan-results           # Get vulnerability scan results

# Webhooks (internal)
POST   /api/v1/webhooks/github                      # GitHub webhook receiver
```

**Example: Trigger a manual run**
```json
POST /api/v1/pipelines/42/runs
{
  "trigger_ref": "main",
  "commit_sha": "abc123def456...",
  "parameters": {
    "skip_integration_tests": false,
    "deploy_target": "staging"
  }
}

Response 201:
{
  "id": 98765,
  "pipeline_def_id": 42,
  "status": "pending",
  "trigger_type": "manual",
  "created_at": "2026-04-09T10:30:00Z",
  "dashboard_url": "https://ci.internal/runs/98765"
}
```

### CLI

```bash
# Trigger pipeline
cictl run trigger --repo org/service-foo --branch main

# Watch pipeline in real-time
cictl run watch 98765

# Get run status
cictl run status 98765

# Stream logs for a task
cictl run logs 98765 --task build

# Retry failed tasks
cictl run retry 98765

# Cancel a run
cictl run cancel 98765

# Approve a gate
cictl gate approve 98765 --gate prod-deploy-approval

# Promote artifact
cictl artifact promote --run 98765 --stage prod

# List recent runs for a repo
cictl run list --repo org/service-foo --limit 20

# Show build cache stats
cictl cache stats --repo org/service-foo
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Pipeline Scheduler & Task Execution Engine

**Why it's hard:**
With 5,000+ concurrent pipelines each containing ~13 tasks, the scheduler must efficiently bin-pack ephemeral pods onto Kubernetes nodes, respect resource quotas per team, handle priority queuing (production hotfix > feature branch), and recover from node failures mid-task.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Jenkins with static agents | Mature, huge plugin ecosystem | Agents waste resources when idle; scaling is slow; single controller bottleneck |
| GitHub Actions (hosted) | Zero infra management, tight GitHub integration | Limited customization, cost at scale, data leaves network |
| Tekton (k8s-native) | Each task is a pod, k8s-native scheduling, CRD-based | Younger ecosystem, steeper learning curve |
| Argo Workflows (k8s-native) | DAG support, artifact passing, mature UI | Heavier CRDs, controller can become bottleneck |
| Custom controller on k8s | Full control, optimized for our workload | High engineering cost, maintenance burden |

**Selected approach: Tekton + custom priority scheduler**

**Justification:** Tekton is k8s-native (tasks = pods), so we inherit Kubernetes scheduling, RBAC, and resource management. We add a custom priority admission webhook that assigns `PriorityClass` based on pipeline metadata (production hotfix > main branch > feature branch). Tekton's Pipeline/Task CRDs map cleanly to our pipeline-as-code YAML.

**Implementation detail:**

```yaml
# Tekton Pipeline example for a Java Spring Boot service
apiVersion: tekton.dev/v1
kind: Pipeline
metadata:
  name: java-service-pipeline
spec:
  params:
    - name: repo-url
    - name: commit-sha
    - name: image-tag
  workspaces:
    - name: source
    - name: maven-cache
  tasks:
    - name: git-clone
      taskRef:
        name: git-clone
      params:
        - name: url
          value: $(params.repo-url)
        - name: revision
          value: $(params.commit-sha)
      workspaces:
        - name: output
          workspace: source

    - name: maven-build
      taskRef:
        name: maven-build
      runAfter: [git-clone]
      params:
        - name: GOALS
          value: ["clean", "package", "-DskipTests"]
      workspaces:
        - name: source
          workspace: source
        - name: maven-cache
          workspace: maven-cache

    - name: unit-tests
      taskRef:
        name: maven-test-shard
      runAfter: [maven-build]
      # Matrix for parallel test shards
      matrix:
        params:
          - name: shard-index
            value: ["0", "1", "2", "3"]
        include:
          - name: total-shards
            value: "4"

    - name: sonarqube-scan
      taskRef:
        name: sonarqube-scan
      runAfter: [maven-build]

    - name: docker-build
      taskRef:
        name: buildkit-build
      runAfter: [maven-build]
      params:
        - name: IMAGE
          value: "registry.internal/org/svc:$(params.image-tag)"
        - name: DOCKERFILE
          value: "./Dockerfile"
        - name: BUILD_ARGS
          value: ["JAR_FILE=target/*.jar"]

    - name: trivy-scan
      taskRef:
        name: trivy-image-scan
      runAfter: [docker-build]
      params:
        - name: IMAGE
          value: "registry.internal/org/svc:$(params.image-tag)"
        - name: SEVERITY
          value: "HIGH,CRITICAL"

    - name: push-image
      taskRef:
        name: docker-push
      runAfter: [unit-tests, trivy-scan, sonarqube-scan]

    - name: deploy-dev
      taskRef:
        name: argocd-sync
      runAfter: [push-image]
      params:
        - name: app-name
          value: "svc-dev"
        - name: image-tag
          value: "$(params.image-tag)"
```

**Priority scheduling implementation:**

```java
// Priority admission webhook (Spring Boot)
@RestController
public class PipelinePriorityWebhook {
    
    @PostMapping("/mutate")
    public AdmissionReview mutate(@RequestBody AdmissionReview review) {
        Pod pod = extractPod(review);
        Map<String, String> labels = pod.getMetadata().getLabels();
        
        String priorityClass;
        if ("true".equals(labels.get("pipeline.ci/hotfix"))) {
            priorityClass = "pipeline-critical";      // priority 1000
        } else if ("main".equals(labels.get("pipeline.ci/branch"))) {
            priorityClass = "pipeline-high";           // priority 500
        } else if ("release".equals(labels.getOrDefault("pipeline.ci/branch", "").split("/")[0])) {
            priorityClass = "pipeline-high";           // priority 500
        } else {
            priorityClass = "pipeline-normal";         // priority 100
        }
        
        JsonPatch patch = JsonPatch.builder()
            .add("/spec/priorityClassName", priorityClass)
            .build();
        
        return AdmissionReview.allow(review, patch);
    }
}
```

**Failure modes:**
- **Controller OOM:** Tekton controller watches all PipelineRun CRDs. With 5,000 concurrent runs, memory can spike. Mitigation: namespace-scoped controllers with sharding.
- **Node failure mid-task:** Pod is evicted. Tekton retries the task (configurable `retries` field). Build cache ensures work is not fully lost.
- **Webhook storm:** Developer pushes 50 commits rapidly. Dedup in webhook gateway: only trigger on latest commit per branch (debounce 5s window via Redis).
- **Resource exhaustion:** All build nodes full. Queue grows. Priority preemption evicts low-priority pods.

**Interviewer Q&As:**

**Q1: How do you prevent a single team from monopolizing build resources?**
A: Kubernetes ResourceQuotas per namespace (one namespace per team). Each team gets a guaranteed minimum (e.g., 50 CPU cores, 100 Gi memory) with burst capacity up to 2x. A custom scheduler plugin tracks per-team usage and deprioritizes teams exceeding their fair share. This is analogous to Kubernetes' `ResourceQuota` + a custom `QueueSort` plugin in the scheduling framework.

**Q2: A pipeline has 200 test shards. How do you handle this at scale?**
A: Tekton Matrix creates 200 TaskRun CRDs. To avoid overwhelming the API server, we batch creation (50 at a time) and use a `maxConcurrency` setting on the matrix. Kubernetes Cluster Autoscaler provisions nodes as needed (with a warm pool for fast scale-up). Test shards share a workspace via a PVC (ReadWriteMany NFS) or download artifacts from S3.

**Q3: How do you handle flaky tests?**
A: Track test results in Elasticsearch. A "flakiness score" is computed per test (failures in last 100 runs / 100). Tests with flakiness > 10% are automatically quarantined: they run but don't block the pipeline. A weekly report surfaces the top 20 flakiest tests to the owning team. The quarantine is a label in the test metadata checked by the test runner.

**Q4: How do you secure the build environment?**
A: Each build pod runs as a non-root user in a separate namespace with a restrictive PodSecurityStandard (baseline or restricted). Secrets are injected via Kubernetes Secrets mounted as volumes (never environment variables logged by `env`). Build logs are post-processed to redact patterns matching secrets. Docker builds use `--secret` mounts (BuildKit) so secrets never appear in image layers. Network policies restrict build pods to internal registries and source control only.

**Q5: What happens if the Tekton controller itself goes down?**
A: Tekton controller is deployed as a Deployment with 2+ replicas using leader election. If the leader crashes, the standby takes over in ~15 seconds. In-flight TaskRuns are not lost because state is in CRDs (etcd). The new leader reconciles all pending/running CRDs and resumes. Tasks running as pods continue independently; only orchestration pauses briefly.

**Q6: How do you implement pipeline-as-code versioning?**
A: The pipeline YAML is read from the commit that triggered the build (not from main). This means each PR can modify its own pipeline definition and test it. We hash the YAML content and store it in `pipeline_definitions.yaml_hash` for audit. If a team wants shared pipeline templates, we use Tekton Bundles (OCI artifacts containing Task/Pipeline definitions) versioned in the artifact registry.

---

### Deep Dive 2: Build Caching & Artifact Promotion

**Why it's hard:**
Without caching, every Maven build downloads 500+ MB of dependencies and recompiles all modules. Docker builds re-execute every layer. At 15,500 builds/day, this wastes hundreds of terabytes of bandwidth and thousands of compute-hours. Artifact promotion must be atomic and auditable, with cryptographic integrity guarantees.

**Approaches for build caching:**

| Approach | Pros | Cons |
|----------|------|------|
| Local node cache (hostPath) | Fastest I/O | Cache lost on pod reschedule; no sharing across nodes; security risk |
| NFS/EFS shared cache | Shared across all pods | High latency for small files; NFS server becomes bottleneck |
| S3 + local overlay | Durable, shared, cheap | Download time on cache miss; complexity |
| Redis for index + S3 for blobs | Fast index lookup, durable storage | Two systems to manage |
| BuildKit inline cache (Docker) | Native Docker layer cache | Only works for Docker builds, not Maven/Gradle |

**Selected approach: S3 for blobs + Redis for cache index, BuildKit cache export for Docker**

**Justification:** S3 provides durable, scalable blob storage. Redis index allows sub-ms lookups of cache keys (derived from lockfile hash + tool version). For Docker, BuildKit's `--cache-from` and `--cache-to` with a registry-backed cache is native and efficient.

**Implementation detail:**

Maven cache strategy:
```bash
#!/bin/bash
# Build task entrypoint script

REPO_SLUG="${REPO_SLUG}"
CACHE_KEY=$(sha256sum pom.xml */pom.xml gradle/wrapper/gradle-wrapper.properties 2>/dev/null | sha256sum | cut -d' ' -f1)
CACHE_PATH="s3://build-cache/${REPO_SLUG}/maven/${CACHE_KEY}.tar.zst"

# Restore cache
if aws s3 ls "${CACHE_PATH}" > /dev/null 2>&1; then
    echo "Cache hit: ${CACHE_KEY}"
    aws s3 cp "${CACHE_PATH}" - | zstd -d | tar xf - -C ~/.m2/repository
else
    echo "Cache miss: ${CACHE_KEY}"
fi

# Build
mvn clean package -DskipTests -T 1C  # parallel build, 1 thread per core

# Save cache (only on main branch to avoid cache pollution)
if [ "${BRANCH}" = "main" ]; then
    tar cf - -C ~/.m2/repository . | zstd -3 | aws s3 cp - "${CACHE_PATH}"
fi
```

Docker build with BuildKit cache:
```dockerfile
# Multi-stage build for Spring Boot
FROM eclipse-temurin:21-jdk AS builder
WORKDIR /app
COPY pom.xml .
COPY src ./src
# Layer cache: dependencies downloaded separately
RUN --mount=type=cache,target=/root/.m2 \
    mvn dependency:go-offline -B
RUN --mount=type=cache,target=/root/.m2 \
    mvn package -DskipTests -B

FROM eclipse-temurin:21-jre
COPY --from=builder /app/target/*.jar /app/app.jar
EXPOSE 8080
ENTRYPOINT ["java", "-jar", "/app/app.jar"]
```

```bash
# BuildKit build command with registry-backed cache
docker buildx build \
    --cache-from type=registry,ref=registry.internal/cache/svc:buildcache \
    --cache-to type=registry,ref=registry.internal/cache/svc:buildcache,mode=max \
    --tag registry.internal/org/svc:${GIT_SHA} \
    --push \
    .
```

Artifact promotion flow:
```
                     Build produces image
                            |
                            v
                  +-------------------+
                  | registry/svc:sha1 |  (dev)
                  +-------------------+
                            |
                     All tests pass
                     Scans clean
                            |
                            v
                  +-------------------+
                  | registry/svc:sha1 |  tagged "staging"
                  +-------------------+
                            |
                  Integration tests pass
                  Manual approval (for prod)
                            |
                            v
                  +-------------------+
                  | registry/svc:sha1 |  tagged "prod"
                  +-------------------+
                            |
                  Signed with cosign
                  Deploy to prod k8s
```

Promotion is a **tag operation**, not a copy. The image digest remains identical. We use `cosign` to sign the image at each promotion stage:

```bash
# Sign artifact at promotion
cosign sign --key cosign.key \
    --annotations "stage=prod,approver=alice,pipeline_run=98765" \
    registry.internal/org/svc@sha256:abc123...

# Verify before deploy
cosign verify --key cosign.pub \
    --annotations "stage=prod" \
    registry.internal/org/svc@sha256:abc123...
```

**Failure modes:**
- **Cache poisoning:** A malicious build uploads a tampered cache. Mitigation: cache key includes hash of all dependency files; cache is per-repo; only main branch builds write cache.
- **S3 outage:** Build proceeds without cache (slower but not broken). Circuit breaker in cache restore script falls through after 5s timeout.
- **Promotion race:** Two pipelines try to promote different SHAs to staging simultaneously. Mitigation: optimistic locking on `artifacts.promotion_stage` with a version column.
- **Signature verification failure:** Deployment is blocked. Alert fires. Likely indicates a supply chain attack or key rotation issue.

**Interviewer Q&As:**

**Q1: How do you prevent cache poisoning in a multi-tenant CI system?**
A: Cache keys are scoped to `(repo_slug, lockfile_hash, tool_version)`. Only builds on protected branches (main, release/*) can write to cache; feature branch builds can only read. Cache entries are checksummed (SHA-256 of the tar) and verified on restore. A background job audits cache entries older than 30 days and deletes them.

**Q2: How does promotion differ from rebuilding for each environment?**
A: Promotion tags the same immutable image digest. Rebuilding would produce a different binary (non-reproducible builds due to timestamps, dependency resolution order). Promotion guarantees "what was tested is what gets deployed." The image digest (sha256) is the canonical identifier across all environments.

**Q3: How do you handle a Maven cache that grows unboundedly?**
A: We use a generational cache strategy. Cache key = hash of pom.xml files. When dependencies change, a new cache entry is created. A TTL-based cleanup job in S3 lifecycle rules deletes cache entries not accessed in 14 days. We also cap cache size per repo to 2 GB; if exceeded, the oldest entries are evicted.

**Q4: What if BuildKit cache becomes stale and actually slows builds?**
A: We monitor Docker build times. If a cached build takes longer than a clean build (indicating stale layers that need to be invalidated anyway), we add a `--no-cache` flag for that repo's next build and bust the registry cache. A metric `docker_build_cache_hit_rate` in Prometheus tracks effectiveness.

**Q5: How do you ensure an artifact in prod was never tampered with after build?**
A: Supply chain security uses `cosign` (Sigstore) for image signing and `in-toto` for build provenance attestation. Each build produces a SLSA provenance document recording the builder identity, source commit, and build commands. Kubernetes admission controller (Kyverno) verifies signature and provenance before allowing pod creation.

**Q6: What is the impact of layer deduplication on build cache?**
A: Docker images share base layers (e.g., `eclipse-temurin:21-jre` is ~200 MB shared across all Java services). With 1,500 Java services, this single base layer is stored once. Layer deduplication typically saves 60-70% of storage. BuildKit's `--cache-to mode=max` exports all layers (including intermediate build stages), maximizing reuse across builds of the same repo.

---

### Deep Dive 3: Test Parallelism & Sharding

**Why it's hard:**
A large Java service may have 5,000+ tests. Running sequentially takes 45 minutes. Sharding across N pods requires: even distribution of test runtime (not just count), shared test infrastructure (databases, message brokers), aggregated results, and handling of test interdependencies.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Split by test count (N tests / K shards) | Simple | Uneven runtimes; one shard takes 10x longer |
| Split by historical runtime | Even wall-clock distribution | Requires runtime history; new tests have no data |
| Split by module/package | Natural grouping, easy to reason about | Some modules have far more tests |
| Dynamic sharding (work-stealing) | Optimal distribution | Complex coordination; requires central dispatcher |

**Selected approach: Historical runtime-based sharding with dynamic rebalancing**

**Implementation detail:**

```python
# Test shard allocator (runs before test tasks are created)
import json
import hashlib
from typing import List, Dict

def allocate_shards(
    test_classes: List[str],
    runtime_history: Dict[str, float],  # class -> avg_seconds
    num_shards: int,
    default_runtime: float = 10.0       # for new tests
) -> List[List[str]]:
    """
    Greedy bin-packing: assign each test class to the shard
    with the lowest total runtime so far.
    """
    # Get runtime for each test class
    tests_with_runtime = []
    for tc in test_classes:
        runtime = runtime_history.get(tc, default_runtime)
        tests_with_runtime.append((tc, runtime))
    
    # Sort descending by runtime (largest first for better packing)
    tests_with_runtime.sort(key=lambda x: -x[1])
    
    # Initialize shards
    shards = [[] for _ in range(num_shards)]
    shard_runtimes = [0.0] * num_shards
    
    # Greedy assignment
    for test_class, runtime in tests_with_runtime:
        min_shard = min(range(num_shards), key=lambda i: shard_runtimes[i])
        shards[min_shard].append(test_class)
        shard_runtimes[min_shard] += runtime
    
    # Log shard balance
    max_rt = max(shard_runtimes)
    min_rt = min(shard_runtimes)
    balance_ratio = min_rt / max_rt if max_rt > 0 else 1.0
    print(f"Shard balance ratio: {balance_ratio:.2f} "
          f"(target runtime per shard: {max_rt:.1f}s)")
    
    return shards
```

**Failure modes:**
- **One shard has all the slow tests:** Historical data prevents this. For new repos without history, we fall back to package-based splitting and collect runtime data over 5 builds.
- **Shared test database contention:** Each shard gets its own database schema (or uses TestContainers for isolated Postgres/MySQL instances per pod). Spring Boot test profile configures unique schema names.
- **Flaky test causes shard failure:** Shard is retried once. If it fails again, only the specific failing tests are reported; passing tests in that shard are counted. The pipeline can be configured to allow N flaky failures without blocking.

**Interviewer Q&As:**

**Q1: How do you determine the optimal number of shards?**
A: Target shard runtime of 3-5 minutes. If total test runtime is 45 minutes, that's 9-15 shards. We set a floor of 4 shards (parallelism benefit) and cap at 50 (overhead of pod scheduling). The shard count is auto-computed: `ceil(total_estimated_runtime / target_shard_runtime)`.

**Q2: How do you handle integration tests that need real databases?**
A: Each test shard pod has a sidecar container running the required database (via TestContainers pattern). For shared services (like a staging Kafka cluster), we use namespace isolation and unique consumer groups per shard. Contract tests (using Pact) verify service interactions without real dependencies.

**Q3: How do you aggregate test results from N shards?**
A: Each shard uploads JUnit XML to S3. A finalize task downloads all XML files, merges them, computes totals (pass/fail/skip), and publishes to the pipeline status API. GitHub check run is updated with the merged results. Elasticsearch ingests individual test results for trend analysis.

**Q4: What if historical runtime data is lost?**
A: Graceful degradation to equal-count splitting. Runtime data is collected every build and stored in Elasticsearch (30-day retention). Even one build populates enough data for reasonable sharding. If ES is down, we use Redis as a fallback (last known runtimes cached for 7 days).

**Q5: How do you handle test ordering dependencies?**
A: Tests should be independent (no ordering dependency). We enforce this by running tests in random order within each shard (JUnit `@TestMethodOrder(Random.class)`). If a test fails only when run after another test, that's a bug in the test, not the infrastructure. We surface these as "order-dependent failures" in our flaky test report.

**Q6: How do you handle tests that take 10+ minutes individually?**
A: Long-running tests are tagged `@SlowTest` and placed in a dedicated shard that runs on a higher-resource pod (more CPU/memory). They never share a shard with fast tests, as that would create imbalance. If a single test exceeds 15 minutes, it's flagged for refactoring.

---

## 7. Scheduling & Resource Management

### Build Pod Scheduling

| Aspect | Strategy |
|--------|----------|
| Pod resource requests | Build pods: 2 CPU, 4 Gi memory. Test pods: 1 CPU, 2 Gi. Docker build pods: 4 CPU, 8 Gi (BuildKit is CPU-intensive). |
| PriorityClasses | `pipeline-critical` (1000): production hotfixes. `pipeline-high` (500): main/release branches. `pipeline-normal` (100): feature branches. `pipeline-low` (10): scheduled/nightly builds. |
| Preemption | Low-priority feature branch builds are preempted when cluster is full and a hotfix arrives. Preempted tasks are re-queued automatically. |
| Node pools | Dedicated `ci-build` node pool with taints (`ci-build=true:NoSchedule`). Build pods have matching tolerations. Prevents build pods from competing with production workloads. |
| Autoscaling | Cluster Autoscaler scales the `ci-build` node pool from 10 to 200 nodes based on pending pod count. Scale-up latency: ~90s (warm pool of 5 pre-provisioned nodes for instant availability). |
| Resource quotas | Per-team namespace: 100 CPU cores, 200 Gi memory. Prevents one team from starving others. |

### Build Queue Management

```
Incoming pipeline triggers
        |
        v
  +-----+------+
  | Priority    |
  | Queue       |  (Redis sorted set, score = priority * -1)
  | (per team)  |
  +-----+------+
        |
        v
  +-----+------+
  | Fair-share  |  Weighted round-robin across teams
  | Scheduler   |  (team weight = quota / current_usage)
  +-----+------+
        |
        v
  +-----+------+
  | k8s Pod     |  Create TaskRun -> Pod
  | Scheduling  |
  +-------------+
```

The fair-share scheduler ensures that if Team A has used 80% of their quota and Team B has used 20%, Team B's builds are scheduled first. This prevents starvation during peak hours.

---

## 8. Scaling Strategy

### Horizontal Scaling Points

| Component | Scaling Mechanism | Trigger |
|-----------|-------------------|---------|
| Webhook Gateway | HPA on CPU (target 60%) | Webhook burst from GitHub |
| Kafka | Add partitions to `pipeline.trigger` topic | Consumer lag > 1,000 |
| Tekton Controller | Namespace sharding (1 controller per 500 namespaces) | CRD count > 10,000 |
| Build node pool | Cluster Autoscaler | Pending pods > 0 for 30s |
| Artifact Registry | Replicate to regions; CDN for pulls | Pull latency > 2s |
| MySQL | Read replicas for dashboard queries; vertical scale for writes | Query latency p95 > 100ms |
| Elasticsearch | Add data nodes; increase shard count | Indexing latency > 5s |

### Interviewer Q&As

**Q1: The Tekton controller watches all CRDs across the cluster. What happens at 50,000 concurrent runs?**
A: We shard the controller by namespace. Each controller instance is responsible for a subset of namespaces (assigned via a ConfigMap or a custom sharding CRD). The namespace-to-controller mapping uses consistent hashing so adding a controller only redistributes ~1/N of namespaces. We also use Tekton's garbage collection to delete completed PipelineRuns after 24 hours, keeping active CRD count low.

**Q2: How do you handle a 10x traffic spike (company-wide "merge day" before release)?**
A: Pre-scale the build node pool the day before (scheduled scaling). During the spike, Cluster Autoscaler handles additional demand. Low-priority builds are queued with longer wait times. We also implement build coalescing: if 5 pushes to the same branch arrive within 30 seconds, only the latest commit triggers a build.

**Q3: How do you scale the artifact registry for 10,000 image pulls during a company-wide deployment?**
A: Registry is backed by S3 (unlimited storage throughput). For pull throughput, we run multiple registry replicas behind a load balancer with connection pooling. Layer blobs are served directly from S3 (or CloudFront for cross-region). Kubernetes nodes use `crictl` pull with `--concurrent` flag for parallel layer download.

**Q4: How do you scale test infrastructure (databases, message brokers used by integration tests)?**
A: Each test shard uses ephemeral containers (TestContainers pattern) for databases, so scaling is tied to pod count. For shared services (e.g., a test Kafka cluster), we partition by namespace and auto-scale. Peak hours may have 200 simultaneous Postgres containers; the node pool accounts for this overhead.

**Q5: What's the bottleneck at 100K builds/day?**
A: At 100K builds/day (~1.15/s sustained, ~4/s peak), the bottleneck shifts to: (1) Kubernetes API server handling CRD creation/deletion rate. Mitigation: use API priority and fairness (APF) settings, batch CRD operations. (2) S3 request rate for cache and artifacts. Mitigation: use S3 request rate partitioning (prefix-based). (3) etcd size from CRD accumulation. Mitigation: aggressive garbage collection of completed runs.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | Recovery Time |
|---------|--------|-----------|------------|---------------|
| GitHub webhook delivery failure | Pipeline not triggered | Missing pipeline runs for recent commits | GitHub retries webhooks 3 times; periodic reconciler polls GitHub for recent commits and backfills missed triggers | < 5 min |
| Kafka broker down (1 of 3) | No immediate impact (replication factor 3) | Kafka monitoring alerts on under-replicated partitions | Automatic leader election; replace broker | < 30 s (failover), hours (replacement) |
| Tekton controller crash | New pipelines not scheduled; running pods continue | Pod restart alert; pending PipelineRun count grows | Leader election promotes standby; reconciliation loop picks up all pending CRDs | < 15 s |
| Build node failure | Running tasks on that node fail | Node NotReady; pod eviction | Tekton retries task on healthy node; autoscaler replaces node | < 2 min (retry), ~90 s (new node) |
| S3 outage | Build cache unavailable; artifacts not published | S3 error rate metric | Builds proceed without cache (slower); artifact publish retried with exponential backoff | Variable (AWS SLA: 99.99%) |
| MySQL primary down | Pipeline state writes fail | MySQL health check; replication lag spike | Automated failover to replica (orchestrator or RDS Multi-AZ); ~30s downtime | < 30 s |
| Elasticsearch down | No log search; analytics degraded | ES cluster health red | Logs buffered in Kafka; ES can catch up when restored. Pipeline execution unaffected. | Minutes to hours |
| Artifact registry down | Images cannot be pushed or pulled | Health check failure; push/pull error rate | Registry is stateless (S3-backed); restart pods. Multiple replicas ensure availability. | < 1 min |
| Network partition between CI cluster and production k8s | Deploys fail | ArgoCD sync failure alerts | ArgoCD retries automatically; manual approval gate pauses deploy if repeated failures | Auto-recovery on partition heal |
| Secret store (Vault) unavailable | Builds that need secrets fail | Vault health check | Cache secrets in k8s Secrets (short TTL, 1 hour); builds that don't need secrets unaffected | < 1 min (cache), variable (Vault) |

### Redundancy Architecture
- **Multi-AZ deployment:** Tekton controller, webhook gateway, Kafka, MySQL all span 3 AZs.
- **Build node pools:** Spread across 3 AZs. If one AZ is lost, capacity drops by 33%; autoscaler compensates.
- **Cross-region DR:** Pipeline definitions are in Git (distributed). State is in MySQL with cross-region replication. Artifacts in S3 with cross-region replication. Recovery: point webhooks to DR region's gateway.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Source |
|--------|------|-----------------|--------|
| `pipeline_trigger_latency_seconds` | Histogram | p95 > 10 s | Webhook Gateway |
| `pipeline_queue_wait_seconds` | Histogram | p95 > 60 s | Pipeline Controller |
| `pipeline_duration_seconds` | Histogram | p95 > 900 s (15 min) | Pipeline Controller |
| `pipeline_success_rate` | Gauge | < 85% (1h window) | Pipeline Controller |
| `build_cache_hit_rate` | Gauge | < 50% (degradation) | Build Task |
| `test_shard_balance_ratio` | Gauge | < 0.6 (poor balance) | Test Allocator |
| `artifact_push_duration_seconds` | Histogram | p95 > 30 s | Package Task |
| `artifact_scan_critical_count` | Counter | > 0 (blocks promotion) | Trivy Scan |
| `build_node_pool_utilization` | Gauge | > 85% (scale up) | Cluster Autoscaler |
| `build_pod_pending_count` | Gauge | > 50 for > 2 min | Kubernetes |
| `kafka_consumer_lag` | Gauge | > 5,000 messages | Kafka |
| `tekton_controller_reconcile_errors` | Counter | > 10/min | Tekton Controller |
| `approval_gate_pending_count` | Gauge | > 20 (bottleneck) | Promotion Service |
| `flaky_test_rate` | Gauge | > 5% (per repo) | Test Analytics |

### Dashboards
1. **Pipeline Health:** Success rate, duration trends, queue depth, cache hit rate.
2. **Build Capacity:** Node pool utilization, pending pods, autoscaler events, per-team quotas.
3. **Artifact Flow:** Images built/day, promotion rate, scan findings, registry storage growth.
4. **Test Analytics:** Flaky test leaderboard, shard balance, test runtime trends.

### Logging & Tracing
- **Structured logging:** JSON format with `pipeline_run_id`, `task_name`, `repo_slug` in every log line.
- **Distributed tracing:** OpenTelemetry traces span the full pipeline: webhook -> scheduler -> build -> test -> package -> deploy. Each task's trace is linked to the parent pipeline trace.
- **Build logs:** Streamed from pods to S3 via a sidecar container. Available in real-time via SSE endpoint and archived for 90 days.

---

## 11. Security

### Build Environment Security

| Layer | Control |
|-------|---------|
| Pod isolation | Each build runs in an ephemeral pod with `securityContext.runAsNonRoot: true`, `readOnlyRootFilesystem: true` (except workspace mount). `PodSecurityStandard: restricted`. |
| Network isolation | NetworkPolicy allows build pods to reach: internal Git, artifact registry, build cache (S3). All other egress blocked. No ingress allowed. |
| Secret management | HashiCorp Vault with short-lived credentials (15-min TTL). Secrets injected as files (never env vars). Build logs scrubbed for secret patterns. |
| Image signing | `cosign` signs images at build time. Kubernetes admission controller (Kyverno/OPA) rejects unsigned images. |
| Supply chain | SLSA Level 3 provenance. Build runs on a hardened, ephemeral builder with no persistent state. |
| Dependency scanning | OWASP Dependency-Check for Java (Maven). `pip-audit` for Python. `npm audit` for Node. Results block pipeline if CRITICAL CVE found. |
| RBAC | Pipeline definitions are in Git (PR review required). Manual approval gates for prod deployment. Audit log of all promotions with user identity. |
| Container scanning | Trivy scans every image. HIGH/CRITICAL findings block promotion. Allowlisting is possible for known false positives (stored in a policy repo, requires security team approval). |

### Secret Zero-Knowledge Pipeline

```
Developer -> Git Push (no secrets in code)
                |
                v
Build Pod -> Vault (authenticate via k8s ServiceAccount JWT)
                |
                v
          Short-lived credentials (DB password, API key)
                |
          Mounted as /run/secrets/... (tmpfs, never on disk)
                |
          Used during build/test, then pod is destroyed
```

---

## 12. Incremental Rollout Strategy

### Pipeline Rollout (Rolling Out CI/CD Changes Themselves)

The CI/CD system is infrastructure that must be rolled out carefully. Changes to the pipeline system itself follow a staged rollout:

1. **Canary (1% of repos):** Select 10-15 low-risk repos. New pipeline controller version serves only these repos (routing by repo label). Monitor for 24 hours.
2. **Early adopters (10%):** Expand to volunteering teams. 48 hours of observation.
3. **General availability (100%):** Full fleet rollout with automated rollback if pipeline success rate drops > 5%.

### Deployment Rollout via Pipeline

When a pipeline deploys application code, it uses the rollout strategy appropriate for the target:
- **Kubernetes:** ArgoCD progressive delivery with Argo Rollouts. Canary 5% -> analysis (5 min) -> 25% -> analysis -> 100%.
- **Bare-metal:** Rollout controller with 5% waves. Health check gate between waves. See `rollout_controller.md`.
- **Database migrations:** Run as a separate pipeline step before deployment. Must be backward-compatible (expand-then-contract pattern).

### Rollout Q&As

**Q1: A pipeline change breaks all builds. How do you limit blast radius?**
A: Pipeline controller changes are deployed behind a feature flag. Only repos labeled `pipeline-beta: true` use the new controller. If builds start failing, the flag is flipped, and all repos revert to the stable controller within seconds (SDK-based evaluation in the controller's reconcile loop).

**Q2: How do you roll out a new Tekton version across 200 namespaces?**
A: Namespace-by-namespace rollout. Controller instances are deployed per namespace group. Upgrade group 1 (10 namespaces), monitor reconcile errors and pipeline success rate for 2 hours, then proceed to group 2. An operator CRD (`TektonUpgrade`) tracks rollout progress and supports pause/resume.

**Q3: How do you handle a bad deployment reaching production through the pipeline?**
A: Multiple gates prevent this: (1) tests must pass, (2) security scans must be clean, (3) canary deployment in staging must pass health checks, (4) manual approval for production, (5) production canary with automated rollback on error rate spike. If a bad deployment reaches production despite these, ArgoCD rollback reverts the Kubernetes deployment to the previous image tag in < 30 seconds.

**Q4: How do you deploy database schema changes safely through CI/CD?**
A: Schema changes use the expand-contract pattern: (1) Pipeline runs "expand" migration (add new column, keep old). (2) Deploy new application code that writes to both old and new columns. (3) Backfill data. (4) Next release: deploy code that reads only from new column. (5) "Contract" migration removes old column. Each step is a separate pipeline run with its own approval gate.

**Q5: What happens if a rollout is in progress and a new commit is pushed?**
A: The new commit triggers a new pipeline run. The in-progress rollout for the previous commit is automatically superseded: ArgoCD detects a newer desired state and begins rolling out the newer version. If the older rollout was mid-canary, it's replaced by the newer canary. This is safe because each version is independently validated.

---

## 13. Trade-offs & Decision Log

| Decision | Option Chosen | Alternative Considered | Rationale |
|----------|---------------|----------------------|-----------|
| Pipeline engine | Tekton (k8s-native) | Jenkins, GitHub Actions | Tasks as pods = native k8s scheduling, resource management, isolation. Jenkins has single-controller bottleneck. GitHub Actions lacks customization at our scale. |
| Build cache storage | S3 + Redis index | NFS, hostPath | S3 is durable and scalable. NFS has IOPS limits. hostPath loses cache on node change. Redis provides fast key lookup. |
| Event bus | Kafka | RabbitMQ, Redis Streams | Kafka's durability, replay capability, and high throughput match our requirements. Ordering within partitions ensures pipeline events are processed sequentially per repo. |
| Artifact signing | cosign (Sigstore) | Notary v2, GPG | cosign is the industry standard for OCI artifact signing. Keyless mode integrates with OIDC identity. Simpler than Notary v2. |
| Test sharding | Runtime-based greedy bin packing | Count-based, module-based | Runtime-based gives the most balanced shards. Count-based leads to 5x imbalance between slow and fast tests. Module-based creates uneven modules. |
| Deployment to k8s | ArgoCD (GitOps) | Tekton deploy task, Helm CLI | GitOps provides audit trail (Git history), drift detection, and self-healing. Tekton deploy task is push-based and doesn't detect drift. |
| Secret management | Vault with k8s auth | k8s Secrets, AWS Secrets Manager | Vault provides dynamic secret generation, TTL, and audit logging. k8s Secrets are base64-encoded (not encrypted at rest by default). |
| Build isolation | Ephemeral pods (restricted PSS) | Docker-in-Docker, VM-based builds | Pod-per-build provides isolation without Docker socket exposure. DinD has security concerns. VMs are too slow to provision. |

---

## 14. Agentic AI Integration

### AI-Powered Pipeline Optimization

| Use Case | AI Agent | Implementation |
|----------|----------|----------------|
| **Auto-shard tuning** | Shard Optimizer Agent | Analyzes test runtime distributions across last 50 builds. Automatically adjusts shard count and allocation. Uses reinforcement learning to minimize wall-clock time per pipeline. |
| **Flaky test detection & quarantine** | Test Health Agent | Classifies tests as stable/flaky/broken using failure pattern analysis (random failures = flaky, consistent failures = broken). Auto-quarantines flaky tests and opens Jira tickets for the owning team. |
| **Build failure triage** | Failure Triage Agent | When a build fails, analyzes the error log, identifies root cause (dependency issue, code error, infra problem), and suggests a fix. Posts a GitHub comment with diagnosis and recommended action. Uses RAG over historical build failures for similar patterns. |
| **Pipeline optimization** | Pipeline Advisor Agent | Reviews pipeline YAML for inefficiencies: tasks that could run in parallel, missing cache steps, oversized resource requests. Suggests optimizations via PR comments. |
| **Security remediation** | Vuln Fix Agent | When Trivy finds a vulnerability, the agent checks if a patch version exists, creates a PR bumping the dependency, and runs the pipeline. If tests pass, it auto-merges (for non-breaking patch versions). |
| **Capacity planning** | Capacity Agent | Forecasts build demand using historical patterns (day-of-week, sprint cycles, release dates). Pre-scales node pools 30 minutes before predicted spikes. |

### Implementation Architecture

```
Build Failure Event (Kafka)
        |
        v
  +-----+------+
  | AI Triage   |  Consumes pipeline.status events where status=failed
  | Agent       |
  +-----+------+
        |
        v
  +-----+------+
  | LLM Service |  Prompt: "Analyze this build log and identify root cause..."
  | (Claude)    |  Context: build log (truncated to 4K tokens), error messages,
  +-----+------+  recent changes to repo, historical similar failures
        |
        v
  +-----+------+
  | Action      |  Post GitHub comment with diagnosis
  | Executor    |  Open Jira ticket if infra issue
  +-------------+  Trigger retry if transient failure
```

### Guardrails
- AI agents cannot merge code to production branches without human approval.
- Dependency bumps by the vuln fix agent require passing all tests + a human review within 24 hours.
- Capacity pre-scaling has a cap (no more than 2x current capacity) to prevent cost explosions.
- All AI agent actions are logged to an audit trail with the rationale.

---

## 15. Complete Interviewer Q&A Bank

**Pipeline Architecture:**

**Q1: Why not just use GitHub Actions for everything?**
A: GitHub Actions is excellent for small-to-medium teams but has limitations at our scale: (1) Hosted runners mean data leaves our network (compliance concern). (2) Self-hosted runners require our own infrastructure anyway, negating the "managed" benefit. (3) Limited customization of scheduling, priority, and resource management. (4) No native support for bare-metal deployments. Tekton gives us full control while remaining k8s-native.

**Q2: How would you migrate from Jenkins to Tekton?**
A: Phase 1: Build a Jenkinsfile-to-Tekton translator that converts common patterns (checkout, build, test, deploy) to Tekton YAML. Phase 2: Run both systems in parallel (shadow mode) for 2 weeks, comparing results. Phase 3: Migrate repo-by-repo, starting with stateless services. Phase 4: Decommission Jenkins. The migration takes 3-6 months for 2,000 repos with a dedicated platform team of 4 engineers.

**Q3: How do you handle monorepo pipelines?**
A: Change detection determines which services changed (using `git diff` against base branch + a dependency graph). Only affected services' pipelines run. The dependency graph is defined in a top-level `OWNERS` or `BUILD` file. Bazel or Nx can manage the build graph for true monorepos, but most orgs at this scale use a multi-repo approach.

**Q4: How do you handle long-running pipelines (> 1 hour)?**
A: Set a pipeline-level timeout (default 30 min, max 2 hours). Long pipelines indicate: (1) too many sequential steps (parallelize them), (2) slow tests (shard more aggressively), (3) large Docker images (optimize Dockerfile). We publish a "pipeline efficiency score" per repo and work with teams exceeding 15 minutes.

**Q5: How do you prevent developers from modifying the pipeline to skip security scans?**
A: Pipeline definitions in repos are validated against an organization policy. A Tekton admission webhook rejects PipelineRuns that don't include required tasks (e.g., `trivy-scan`, `sonarqube-scan`). The policy is defined centrally and enforced at admission time, not at YAML parse time (so it can't be bypassed).

**Build & Test:**

**Q6: How do you handle a build that needs access to a private Git submodule?**
A: The build pod has a Git credential helper configured via a k8s Secret (SSH key or GitHub App installation token). The credential has read-only access to the specific repos needed. The Secret is rotated daily by Vault.

**Q7: How do you ensure reproducible builds?**
A: (1) Pin all dependency versions (Maven: `dependencyManagement`; Docker: `FROM image@sha256:...`). (2) Use a hermetic build environment (same base image, same tool versions). (3) Build from a specific Git commit SHA, not a branch. (4) For Java, use `maven-enforcer-plugin` to ensure no SNAPSHOT dependencies in release builds.

**Q8: What's your strategy for contract testing in a microservices architecture?**
A: We use Pact for consumer-driven contract tests. Consumers define their expectations, publish them to a Pact Broker. Provider builds verify against consumer contracts. If a provider breaks a contract, its pipeline fails. This is a pipeline step after unit tests but before deployment.

**Deployment & Rollout:**

**Q9: How does GitOps with ArgoCD work in practice?**
A: Application source and configuration are in separate repos. The pipeline builds the image, pushes it to the registry, then auto-commits the new image tag to the config repo (via a Git write-back step). ArgoCD watches the config repo and syncs the k8s cluster to match. This decouples "what to build" from "where to deploy."

**Q10: How do you handle rollbacks?**
A: For k8s: ArgoCD reverts the config repo to the previous commit (or `kubectl rollout undo`). For bare-metal: the rollout controller re-deploys the previous artifact version. Database schema rollbacks require a compensating migration (the contract step of expand-contract). All rollback actions are audited.

**Q11: How do you deploy to multiple regions?**
A: Sequential region deployment: region 1 (canary) -> verify 30 min -> region 2 -> verify -> region 3. Each region has its own ArgoCD instance watching the same config repo but with region-specific overlays (Kustomize). The pipeline triggers region 1, and a promotion gate waits for metrics before triggering region 2.

**Scaling & Performance:**

**Q12: How do you handle a cold start problem when the build cache is empty?**
A: Pre-warm the cache during off-peak hours (nightly builds on main branch populate the cache). New repos get a "bootstrap build" that populates the cache without deploying. For Docker, we publish base images with pre-installed dependencies as a starting point.

**Q13: What's the most expensive part of the pipeline to operate?**
A: Build pods (compute). A 4-CPU build pod running for 5 minutes costs ~$0.01 (on-demand). At 15,500 builds/day x 5 min avg, that's ~$155/day in compute. Test shards multiply this: 15,500 x 8 shards x 3 min = ~$186/day for tests. Total CI compute cost: ~$10K/month. Optimization: spot instances for non-critical builds (feature branches), reserved instances for the warm pool.

**Security:**

**Q14: A developer's build pushes a malicious image to the registry. How do you detect and prevent this?**
A: (1) Image signing (cosign) ensures only the CI system can push signed images. Developer laptops don't have the signing key. (2) Kubernetes admission controller rejects unsigned images. (3) Trivy scans every image for known malware signatures. (4) Registry RBAC: only the CI service account has push permissions. Developers have pull-only access.

**Q15: How do you handle secret rotation in running pipelines?**
A: Secrets from Vault have a 15-minute TTL. If a pipeline runs longer than 15 minutes, the build task requests a new lease before expiry (a sidecar or init script handles renewal). Vault's response wrapping ensures the secret is never exposed in transit. If Vault is unreachable during renewal, the task fails and is retried (the secret was only valid for that task anyway).

**Q16: How do you audit who deployed what and when?**
A: Every promotion and deployment writes an audit event to Kafka (`deployment.audit` topic), which is consumed into an immutable audit log (S3 + Elasticsearch). The event contains: who triggered, what commit SHA, what artifact digest, which environment, approval chain, timestamp. Git history of the config repo (ArgoCD) provides a secondary audit trail.

---

## 16. References

- Tekton Documentation: https://tekton.dev/docs/
- ArgoCD Documentation: https://argo-cd.readthedocs.io/
- Argo Workflows: https://argoproj.github.io/argo-workflows/
- BuildKit: https://github.com/moby/buildkit
- Sigstore / cosign: https://docs.sigstore.dev/
- Trivy: https://aquasecurity.github.io/trivy/
- SonarQube: https://docs.sonarqube.org/
- SLSA Framework: https://slsa.dev/
- Pact Contract Testing: https://docs.pact.io/
- Kubernetes Scheduling Framework: https://kubernetes.io/docs/concepts/scheduling-eviction/scheduling-framework/
- TestContainers: https://www.testcontainers.org/
- Maven Build Cache Extension: https://maven.apache.org/extensions/maven-build-cache-extension/
