# Problem-Specific Notes — CI/CD & Deployment (11_cicd_and_deployment)

## artifact_registry

### Unique Purpose
Multi-format artifact storage (Docker/OCI images, Maven JARs, Python wheels, Helm charts). Content-addressable, layer-deduplicated, vulnerability-scanned, geo-replicated. 500 TB+ capacity, 10K concurrent pulls, < 5 s pull for 200 MB within region.

### Unique Architecture
- **Content-addressable blobs**: S3 key = `sha256/<first-2-chars>/<full-digest>`; blobs never duplicated; `reference_count` column for GC
- **Pull-through cache**: proxies Docker Hub, Maven Central, PyPI → local S3; transparent to clients
- **Vulnerability scanner**: Trivy triggered by push webhook; results in `scan_results` table; < 5 min SLA
- **Geo-replication**: Replication Controller watches new blobs; S3 cross-region copy to secondary regions; < 5 min

### Key Schema
```sql
CREATE TABLE repositories (
  id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
  name                VARCHAR(255) NOT NULL,
  description         TEXT,
  storage_location    VARCHAR(512) NOT NULL,
  retention_policy_id BIGINT,
  created_at          DATETIME DEFAULT CURRENT_TIMESTAMP,
  updated_at          DATETIME ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uk_name (name),
  INDEX idx_created (created_at)
);
CREATE TABLE blobs (
  id               BIGINT PRIMARY KEY AUTO_INCREMENT,
  digest           CHAR(71) NOT NULL,        -- "sha256:<64-hex>"
  size_bytes       BIGINT NOT NULL,
  storage_path     VARCHAR(512) NOT NULL,    -- "sha256/ab/abcdef1234..."
  reference_count  INT NOT NULL DEFAULT 0,
  last_accessed_at DATETIME,
  UNIQUE KEY uk_digest (digest)
);
CREATE TABLE manifests (
  id             BIGINT PRIMARY KEY AUTO_INCREMENT,
  digest         CHAR(71) NOT NULL,
  repository_id  BIGINT NOT NULL,
  content_type   VARCHAR(256) NOT NULL,
  config_blob_id BIGINT,
  pushed_at      DATETIME NOT NULL,
  UNIQUE KEY uk_digest_repo (digest, repository_id),
  INDEX idx_pushed (pushed_at)
);
CREATE TABLE tags (
  id            BIGINT PRIMARY KEY AUTO_INCREMENT,
  tag_name      VARCHAR(255) NOT NULL,
  manifest_id   BIGINT NOT NULL,
  repository_id BIGINT NOT NULL,
  created_at    DATETIME DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_tag_repo (tag_name, repository_id)
);
CREATE TABLE scan_results (
  id                 BIGINT PRIMARY KEY AUTO_INCREMENT,
  manifest_digest    CHAR(71) NOT NULL,
  scan_time          DATETIME NOT NULL,
  severity           ENUM('NONE','LOW','MEDIUM','HIGH','CRITICAL') NOT NULL,
  vulnerability_count INT NOT NULL DEFAULT 0,
  findings           JSON
);
```

### Deduplication Algorithm
```python
# Blob upload with SHA-256 deduplication
actual_digest = "sha256:" + hashlib.sha256(data).hexdigest()
s3_key = f"sha256/{actual_digest[7:9]}/{actual_digest[7:]}"  # two-char dir prefix
# INSERT INTO blobs (digest, size_bytes, storage_path, reference_count)
# ON DUPLICATE KEY UPDATE reference_count = reference_count + 1
# → same blob uploaded by 1,500 services: stored once, reference_count = 1500
```

### Deduplication Savings Example
```
1,500 Java services sharing same base image layers:
  JDK base layer (200 MB): stored once
  Spring Boot layer (50 MB): stored once per 3-service group
  App layer (80 MB): stored once per 30-service group
  Config layer (30 MB): stored once per service (unique)

Without dedup: 1,500 × (200+50+80+30) = 540 GB per version
With dedup:    200 + 50×3 + 80×30 + 30×1,500 = ~47.8 GB
Savings: ~91% per version; ~65% across 20 tags
```

### Unique NFRs
- Pull latency (within region): < 5 s for 200 MB; push: < 30 s for 200 MB
- Vulnerability scan: < 5 min after push
- Geo-replication: < 5 min to secondary regions
- Durability: 11 nines (S3-backed); Availability: 99.99%
- Concurrent pulls: 10,000+

---

## blue_green_deployment_system

### Unique Purpose
Zero-downtime releases via identical Blue (live) and Green (new) environments. Atomic traffic switch < 1 s. Instant rollback (no redeployment). Shared database with expand-contract migrations.

### Unique Architecture
- **Kubernetes Service selector patch**: single API call changes `spec.selector.slot` from `blue` to `green`; all new connections go to green; in-flight requests to blue drain (30 s default)
- **Expand-contract DB migrations**: 4-phase schema changes ensuring old (blue) and new (green) versions can both read/write simultaneously
- **Pre-switch validation**: smoke tests + health check against green before any traffic switch; suite runtime < 2 min

### Key Schema
```sql
CREATE TABLE environments (
  id             BIGINT PRIMARY KEY AUTO_INCREMENT,
  service_name   VARCHAR(255) NOT NULL,
  slot           ENUM('blue','green') NOT NULL,
  version        VARCHAR(128) NOT NULL,
  image_digest   CHAR(71) NOT NULL,
  status         ENUM('provisioning','ready','live','draining','idle','terminated') NOT NULL,
  instance_count INT NOT NULL,
  health_status  ENUM('healthy','degraded','unhealthy','unknown') NOT NULL DEFAULT 'unknown',
  endpoint       VARCHAR(512) NOT NULL,
  created_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
  updated_at     DATETIME ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uk_service_slot (service_name, slot),
  INDEX idx_status (status)
);
CREATE TABLE deployments (
  id            BIGINT PRIMARY KEY AUTO_INCREMENT,
  service_name  VARCHAR(255) NOT NULL,
  from_version  VARCHAR(128),
  to_version    VARCHAR(128) NOT NULL,
  triggered_at  DATETIME NOT NULL,
  status        ENUM('pending','deploying','testing','switching','monitoring',
                     'success','rolled_back','failed') NOT NULL,
  switched_at   DATETIME,
  rolled_back_at DATETIME,
  triggered_by  VARCHAR(255) NOT NULL
);
```

### Traffic Switch Algorithm
```python
def switch_traffic(service_name: str, target_slot: str, namespace: str = "production"):
    deployment = apps_v1.read_namespaced_deployment(
        f"{service_name}-{target_slot}", namespace)
    if deployment.status.ready_replicas != deployment.spec.replicas:
        raise Exception(f"Not ready: {deployment.status.ready_replicas}/{deployment.spec.replicas}")
    # Single atomic patch — changes load balancing in < 1 s
    v1.patch_namespaced_service(service_name, namespace, {
        "spec": {"selector": {"app": service_name, "slot": target_slot}}
    })
```

### Expand-Contract Migration (4 Phases)
```
Phase 1 EXPAND: ALTER TABLE users ADD COLUMN display_name VARCHAR(255)
  → v1.2.4 writes both user_name AND display_name
  → v1.2.3 (Blue) reads/writes user_name only (unaffected)

Phase 2 BACKFILL: UPDATE users SET display_name = user_name WHERE display_name IS NULL
  → Background job, batched, throttled off-peak

Phase 3 MIGRATE READS: Deploy v1.2.5 (Blue-Green switch)
  → reads display_name, writes both columns
  → Green (v1.2.5) becomes live

Phase 4 CONTRACT: Deploy v1.2.6 after > 48 h stable
  → ALTER TABLE users DROP COLUMN user_name
```

### Unique NFRs
- Zero dropped requests during switch
- Traffic switch: < 1 s (LB/k8s selector); DNS switch: < 5 min
- Rollback: < 5 s (selector patch back to blue)
- Smoke test suite: < 2 min
- Max infrastructure overhead: 2× during deploy, 1× after teardown

---

## canary_deployment_system

### Unique Purpose
Progressive traffic shifting with automated metric analysis. 1% → 5% → 25% → 50% → 100% steps. Auto-promotion if metrics pass; auto-rollback if fail. Manual approval gates. < 60 min end-to-end full rollout.

### Unique Architecture
- **Istio VirtualService**: defines traffic weights between `stable` and `canary` subsets; xDS push < 5 s to all sidecars
- **Analysis Engine**: queries Prometheus; baseline comparison (canary vs stable + tolerance); 5-min windows every 60 s
- **Argo Rollouts** (or Flagger/custom): manages state machine and traffic weight updates

### Key Schema
```sql
CREATE TABLE canary_rollouts (
  id             BIGINT PRIMARY KEY AUTO_INCREMENT,
  service_name   VARCHAR(255) NOT NULL,
  from_version   VARCHAR(128) NOT NULL,
  to_version     VARCHAR(128) NOT NULL,
  image_digest   CHAR(71) NOT NULL,
  status         ENUM('pending','progressing','paused','promoted','rolled_back','failed','cancelled') NOT NULL,
  current_step   INT NOT NULL DEFAULT 0,
  current_weight INT NOT NULL DEFAULT 0,
  total_steps    INT NOT NULL,
  triggered_by   VARCHAR(255) NOT NULL,
  pipeline_run_id BIGINT,
  started_at     DATETIME,
  promoted_at    DATETIME,
  rolled_back_at DATETIME,
  finished_at    DATETIME,
  created_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_service_status (service_name, status),
  INDEX idx_created (created_at)
);
CREATE TABLE canary_steps (
  id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
  rollout_id          BIGINT NOT NULL REFERENCES canary_rollouts(id),
  step_index          INT NOT NULL,
  step_type           ENUM('set_weight','analysis','pause','manual_approval') NOT NULL,
  weight_value        INT,                 -- for set_weight steps
  analysis_duration_s INT,                 -- for analysis steps
  status              ENUM('pending','running','passed','failed','skipped') NOT NULL DEFAULT 'pending',
  started_at          DATETIME,
  finished_at         DATETIME,
  INDEX idx_rollout_step (rollout_id, step_index)
);
```

### Analysis Engine Algorithm
```python
# Baseline comparison: relative to stable + tolerance
def analyze_metric(canary_query: str, stable_query: str,
                   tolerance: float, absolute_threshold: float) -> str:
    canary_value = prometheus.query(canary_query)   # e.g., error rate
    stable_value = prometheus.query(stable_query)
    # Relative check: canary must not be materially worse than stable
    if canary_value > stable_value + tolerance:
        return "FAIL"
    # Absolute safety net: always fail if above absolute threshold
    if canary_value > absolute_threshold:           # e.g., 5% error rate
        return "FAIL"
    return "PASS"

# PromQL for canary error rate:
# sum(rate(http_requests_total{service="svc",version="canary",status=~"5.."}[5m]))
# / sum(rate(http_requests_total{service="svc",version="canary"}[5m]))

# PromQL for canary p99 latency:
# histogram_quantile(0.99, sum(rate(http_request_duration_seconds_bucket
#   {service="svc",version="canary"}[5m])) by (le))
```

### Argo Rollouts Config
```yaml
canary:
  steps:
    - setWeight: 1
    - pause: {duration: 5m}
    - analysis: {templates: [success-rate, latency-check]}
    - setWeight: 5
    - setWeight: 25
    - setWeight: 50
    - pause: {}        # manual approval gate
    - setWeight: 100
```

### Statistical Significance
```
At 1% canary, 10K RPS service:
  Canary RPS = 100; 5-min window = 30,000 requests → statistically significant
  Analysis interval: every 60 s
  Rollback on: error_rate > 5% (absolute) OR canary > stable + tolerance
```

### Unique NFRs
- Canary pod scheduling: < 30 s
- Rollback: < 10 s
- Analysis interval: every 60 s per step
- Traffic accuracy: within ±1% of configured weight
- Full 1%→100% cycle: < 60 min
- Availability: 99.95%

---

## cicd_pipeline_system

### Unique Purpose
Git-push → build → test (parallel shards) → security scan → Docker build → promote through dev/staging/prod with approval gates. 5,000 concurrent pipelines, < 10 s webhook to first task, 3,000+ engineers, 1,500 services.

### Unique Architecture
- **Tekton** (k8s-native): pipeline-as-code YAML; DAG of TaskRuns; Kubernetes PodSpec per task
- **Webhook Gateway**: validates HMAC-SHA256 on `X-Hub-Signature-256`; dedup via Redis `SETNX event_id TTL=300s`; publishes to Kafka `pipeline.trigger`
- **Build cache**: Maven deps cached in S3 keyed by `sha256(pom.xml + gradle-wrapper.properties)`; restored per shard; only saved on main branch
- **ArgoCD (GitOps)**: watches Helm/Kustomize config repo; syncs Kubernetes Deployment on image tag bump

### Key Schema
```sql
CREATE TABLE pipeline_definitions (
  id              BIGINT PRIMARY KEY AUTO_INCREMENT,
  repo_slug       VARCHAR(255) NOT NULL,
  branch_pattern  VARCHAR(255) NOT NULL,
  yaml_path       VARCHAR(512) NOT NULL,
  yaml_hash       CHAR(64) NOT NULL,    -- SHA-256 of pipeline YAML
  created_at      DATETIME DEFAULT CURRENT_TIMESTAMP,
  updated_at      DATETIME ON UPDATE CURRENT_TIMESTAMP
);
CREATE TABLE pipeline_runs (
  id           BIGINT PRIMARY KEY AUTO_INCREMENT,
  pipeline_id  BIGINT NOT NULL REFERENCES pipeline_definitions(id),
  repo_slug    VARCHAR(255) NOT NULL,
  branch       VARCHAR(255) NOT NULL,
  commit_sha   CHAR(40) NOT NULL,
  triggered_at DATETIME NOT NULL,
  status       ENUM('triggered','scheduled','running','passed','failed') NOT NULL,
  finished_at  DATETIME,
  triggered_by VARCHAR(255) NOT NULL,
  INDEX idx_commit (commit_sha),
  INDEX idx_status (status)
);
CREATE TABLE task_runs (
  id              BIGINT PRIMARY KEY AUTO_INCREMENT,
  pipeline_run_id BIGINT NOT NULL REFERENCES pipeline_runs(id),
  task_name       VARCHAR(255) NOT NULL,
  started_at      DATETIME,
  finished_at     DATETIME,
  status          ENUM('pending','running','passed','failed','skipped') NOT NULL DEFAULT 'pending',
  log_url         VARCHAR(512),
  INDEX idx_pipeline_run (pipeline_run_id)
);
```

### Build Cache Strategy
```bash
# Cache key: SHA-256 of dependency manifests
CACHE_KEY=$(sha256sum pom.xml */pom.xml gradle/wrapper/gradle-wrapper.properties \
            | sha256sum | cut -d' ' -f1)
CACHE_PATH="s3://build-cache/${REPO_SLUG}/maven/${CACHE_KEY}.tar.zst"

# Restore: extract cached .m2 repository
aws s3 cp "${CACHE_PATH}" - | zstd -d | tar xf - -C ~/.m2/repository

# Save (main branch only, to avoid polluting with feature branch deps):
tar cf - ~/.m2/repository | zstd -3 | aws s3 cp - "${CACHE_PATH}"
```

### Tekton Pipeline (Java Spring Boot)
```yaml
tasks:
  - name: maven-build
    params: [{name: GOALS, value: ["clean","package","-DskipTests"]}]
  - name: unit-tests
    matrix:
      params: [{name: shard-index, value: ["0","1","2","3"]}]
      include: [{name: total-shards, value: "4"}]
  - name: docker-build
    params: [{name: BUILD_ARGS, value: ["JAR_FILE=target/*.jar"]}]
```

### Priority Scheduling
```java
// Hotfix branches get Kubernetes high-priority preemption
if (labels.get("pipeline.ci/hotfix").equals("true")) {
    priorityClass = "pipeline-critical";   // priority 1000
} else if (labels.get("pipeline.ci/branch").equals("main")) {
    priorityClass = "pipeline-high";       // priority 500
} else {
    priorityClass = "pipeline-normal";     // priority 100
}
```

### Unique NFRs
- Webhook to first task: < 10 s
- Build queue wait P95: < 30 s
- Concurrent pipelines: 5,000+
- Artifact durability: 11 nines (S3-backed)
- Secret zero-knowledge: secrets never written to logs (100%)
- Audit retention: 2 years

---

## feature_flag_service

### Unique Purpose
Decouple deployment from release. In-process SDK evaluation (< 1 ms). Real-time flag updates via SSE (< 5 s). Emergency kill switch (< 2 s). Deterministic percentage rollout with murmurHash3. 10K+ flags, 12K SDK instances, 10M+ evals/sec (all in-process, zero server load).

### Unique Architecture
- **In-process SDK**: `ConcurrentHashMap<String, FlagDefinition>` per service instance; evaluation never leaves process
- **SSE Gateway**: backed by Redis Pub/Sub; pushes flag mutations to all connected SDK instances; kill switch bypasses batching for < 2 s propagation
- **Multi-environment**: `flag_environments` table with separate enabled/default_value per `dev/staging/production`

### Key Schema
```sql
CREATE TABLE feature_flags (
  id               BIGINT PRIMARY KEY AUTO_INCREMENT,
  flag_key         VARCHAR(255) NOT NULL,
  name             VARCHAR(255) NOT NULL,
  flag_type        ENUM('boolean','string','number','json') NOT NULL,
  owner_team       VARCHAR(255) NOT NULL,
  owner_email      VARCHAR(255) NOT NULL,
  jira_ticket      VARCHAR(50),
  tags             JSON,
  lifecycle_status ENUM('active','stale','permanent','deprecated') NOT NULL DEFAULT 'active',
  stale_after      DATETIME,
  created_at       DATETIME DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_flag_key (flag_key),
  INDEX idx_lifecycle (lifecycle_status),
  INDEX idx_stale (stale_after)
);
CREATE TABLE flag_environments (
  id            BIGINT PRIMARY KEY AUTO_INCREMENT,
  flag_id       BIGINT NOT NULL REFERENCES feature_flags(id) ON DELETE CASCADE,
  environment   ENUM('dev','staging','production') NOT NULL,
  enabled       BOOLEAN NOT NULL DEFAULT FALSE,
  default_value TEXT NOT NULL,
  version       INT NOT NULL DEFAULT 1,
  updated_at    DATETIME ON UPDATE CURRENT_TIMESTAMP,
  updated_by    VARCHAR(255),
  UNIQUE KEY uk_flag_env (flag_id, environment)
);
CREATE TABLE targeting_rules (
  id          BIGINT PRIMARY KEY AUTO_INCREMENT,
  flag_id     BIGINT NOT NULL REFERENCES feature_flags(id),
  rule_order  INT NOT NULL,
  conditions  JSON NOT NULL,       -- [{attribute, operator, values}]
  serve_value TEXT NOT NULL,
  enabled     BOOLEAN NOT NULL DEFAULT TRUE
);
```

### Unique Algorithm: Deterministic Percentage Rollout
```java
// Same user always gets same result for same flag — no flip-flopping
private boolean evaluatePercentageRollout(Clause clause, EvaluationContext ctx) {
    int percentage = Integer.parseInt(clause.getValues().get(0));
    String keyValue = ctx.getAttribute(clause.getRolloutKey());  // e.g., user_id
    String hashInput = clause.getAttribute() + ":" + keyValue;
    int hash = murmurHash3(hashInput);
    int bucket = Math.abs(hash % 100);          // bucket 0–99
    return bucket < percentage;                  // deterministic, stable
}

// In-process evaluation (ConcurrentHashMap, < 0.1 ms)
public boolean isEnabled(String flagKey, EvaluationContext ctx) {
    FlagDefinition flag = flagCache.get(flagKey);  // O(1)
    if (flag == null || !flag.isEnabled()) return false;
    for (TargetingRule rule : flag.getRules()) {
        if (evaluateConditions(rule.getConditions(), ctx))
            return Boolean.parseBoolean(rule.getServeValue());
    }
    return Boolean.parseBoolean(flag.getDefaultValue());
}
```

### SSE Propagation Flow
```
Flag change → MySQL UPDATE → Redis PUBLISH "flag_updated:{flag_key}"
→ SSE Gateway subscribers receive message
→ SSE Gateway pushes to all 12,000 SDK instances via SSE stream
→ SDK updates ConcurrentHashMap entry
→ Total: < 5 s end-to-end
Kill switch: bypasses batching → < 2 s
```

### Unique NFRs
- Flag evaluation: < 0.1 ms in-process (< 1 ms guaranteed)
- Flag update propagation: < 5 s (SSE); < 2 s for kill switch
- Evaluations/sec: 10M+ (all in-process, no server calls)
- SDK initialization: < 2 s (bootstrap fetch + cache warm)
- Graceful degradation: return cached defaults when service unavailable
- Availability: 99.99%

---

## rollout_controller

### Unique Purpose
Wave-based rolling updates across 50,000+ bare-metal hosts/VMs. Topology-aware batching (AZ + rack constraints). Two-phase health gate (agent health + Prometheus). Idempotent — skips already-updated hosts. 5% per wave, < 30 s auto-pause on error detection.

### Unique Architecture
- **Agent Manager**: persistent gRPC connections to all host agents; fan-out deploy commands; streaming health reports
- **Wave Planner**: topology-aware round-robin across AZs (max 33% per AZ per wave) + rack distribution (max 2 hosts per rack per wave)
- **Canary wave**: 1–2 pre-selected hosts; 10-min soak vs 2-min for main waves
- **Health Gate**: phase 1 (all agents healthy within 5 min) + phase 2 (Prometheus error_rate + p99_latency below threshold)

### Key Schema
```sql
CREATE TABLE hosts (
  id                BIGINT PRIMARY KEY AUTO_INCREMENT,
  hostname          VARCHAR(255) NOT NULL,
  ip_address        VARCHAR(45) NOT NULL,
  availability_zone VARCHAR(20) NOT NULL,
  rack_id           VARCHAR(50) NOT NULL,
  role              VARCHAR(100) NOT NULL,
  current_version   VARCHAR(128) NOT NULL,
  target_version    VARCHAR(128),
  health_status     ENUM('healthy','degraded','unhealthy','unknown','draining') NOT NULL,
  last_heartbeat    DATETIME,
  tags              JSON,
  UNIQUE KEY uk_hostname (hostname),
  INDEX idx_role_version (role, current_version),
  INDEX idx_az_rack (availability_zone, rack_id)
);
CREATE TABLE rollouts (
  id                        BIGINT PRIMARY KEY AUTO_INCREMENT,
  name                      VARCHAR(255) NOT NULL,
  service_role              VARCHAR(100) NOT NULL,
  from_version              VARCHAR(128) NOT NULL,
  to_version                VARCHAR(128) NOT NULL,
  status                    ENUM('planning','executing','paused','rolling_back','succeeded','failed') NOT NULL,
  wave_size_percent         INT NOT NULL DEFAULT 5,
  max_failed_hosts_per_wave INT NOT NULL DEFAULT 1,
  error_rate_threshold      DECIMAL(5,2) NOT NULL DEFAULT 1.00,
  p99_latency_threshold_ms  INT NOT NULL,
  created_at                DATETIME DEFAULT CURRENT_TIMESTAMP,
  started_at                DATETIME,
  paused_at                 DATETIME,
  finished_at               DATETIME,
  INDEX idx_service_role_status (service_role, status)
);
CREATE TABLE waves (
  id                   BIGINT PRIMARY KEY AUTO_INCREMENT,
  rollout_id           BIGINT NOT NULL REFERENCES rollouts(id),
  wave_number          INT NOT NULL,
  status               ENUM('pending','executing','passed','failed') NOT NULL DEFAULT 'pending',
  host_count           INT NOT NULL,
  failed_count         INT NOT NULL DEFAULT 0,
  started_at           DATETIME,
  finished_at          DATETIME,
  health_check_result  TEXT,
  INDEX idx_rollout_wave (rollout_id, wave_number)
);
CREATE TABLE host_rollout_status (
  id                      BIGINT PRIMARY KEY AUTO_INCREMENT,
  host_id                 BIGINT NOT NULL REFERENCES hosts(id),
  rollout_id              BIGINT NOT NULL REFERENCES rollouts(id),
  status                  ENUM('pending','deploying','passed','failed','skipped') NOT NULL,
  deployment_started_at   DATETIME,
  deployment_finished_at  DATETIME,
  deployment_log          LONGTEXT
);
```

### Wave Planning Algorithm
```go
func PlanWaves(hosts []Host, config WaveConfig) WavePlan {
    pendingHosts := filterPending(hosts)  // skip current_version == target_version
    totalHosts := len(pendingHosts)
    waveSize := min(
        int(math.Ceil(float64(totalHosts) * float64(config.WaveSizePercent) / 100.0)),
        config.WaveSizeMax,
    )
    // Wave 0: canary (1-2 pre-selected hosts, 10-min soak)
    canaryWave := buildCanaryWave(pendingHosts, config.CanaryHosts)
    // Main waves: topology-aware round-robin
    // MaxHostsPerAZ = 0.33 (max 33% of AZ's hosts per wave)
    // MaxHostsPerRack = 2 (absolute, prevents correlated rack failures)
    for len(remaining) > 0 {
        wave := buildTopologyAwareWave(remaining, waveSize, config)
        // ensures: sum(az_count) per AZ <= total_in_AZ * MaxHostsPerAZ
        // ensures: count(rack_id) per rack <= MaxHostsPerRack
    }
}
// Example: 500 hosts, 3 AZs, 5% waves
// Wave 0 [canary]: 2 hosts (AZ1:1, AZ2:1)
// Wave 1–19: 25 hosts each (AZ1:9, AZ2:8, AZ3:8); max 2 per rack
```

### Two-Phase Health Gate
```go
// Phase 1: Agent health (timeout 5 min)
// All hosts in wave must report healthy via gRPC heartbeat
// Phase 2: Prometheus metrics soak
// error_rate = sum(rate(http_5xx[5m])) / sum(rate(http_total[5m]))
// Must be < rollout.error_rate_threshold for 2 min sustained
// Both phases must pass; either failure → auto-pause rollout
```

### Unique NFRs
- Rollout start: < 30 s
- Wave transition: < 5 min
- Auto-pause (error detection → halt): < 30 s
- Rollback: < 10 min (wave-by-wave reverse)
- Blast radius: 5% per wave (configurable)
- Fleet scale: 50,000+ hosts
- Availability: 99.9%
