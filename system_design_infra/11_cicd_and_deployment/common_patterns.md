# Common Patterns — CI/CD & Deployment (11_cicd_and_deployment)

## Common Components

### MySQL as Deployment Metadata Store
- All 6 systems use MySQL for authoritative state: manifests, deployment records, flag definitions, rollout history, host inventory, pipeline definitions
- artifact_registry: `repositories`, `manifests`, `tags`, `blobs`, `scan_results` tables
- blue_green_deployment: `environments`, `deployments` tables with status ENUMs
- canary_deployment: `canary_rollouts`, `canary_steps` tables
- cicd_pipeline: `pipeline_definitions`, `pipeline_runs`, `task_runs` tables
- feature_flag_service: `feature_flags`, `flag_environments`, `targeting_rules`, `audit_log` tables
- rollout_controller: `hosts`, `rollouts`, `waves`, `host_rollout_status` tables

### Content-Addressable Artifacts (SHA-256 Digest as Key)
- All 6 systems reference artifacts by SHA-256 digest; same content → same hash → stored once; S3 key pattern: `sha256/<first-2-chars>/<full-digest>`
- artifact_registry: blobs stored at `s3://registry-blobs/sha256/ab/abcdef1234...`; `reference_count` column for GC; Docker Distribution standard
- cicd_pipeline: Docker images tagged with `git_sha + semver`; BuildKit inline cache; Maven cache keyed by `sha256(pom.xml + gradle-wrapper.properties)`
- blue_green/canary/rollout: all reference `image_digest CHAR(71)` in deployment tables

### Prometheus as Post-Deploy Health Gate
- 4 of 6 systems query Prometheus after deployment to auto-promote or auto-rollback
- blue_green_deployment: monitors error_rate + latency after traffic switch; auto-rollback if thresholds breached
- canary_deployment: analysis engine queries `sum(rate(http_requests_total{...}[5m]))` and `histogram_quantile(0.99, ...)` per step; baseline comparison (canary vs stable + tolerance)
- rollout_controller: phase 2 of health gate queries Prometheus for global error_rate + p99_latency; both agent health AND Prometheus must pass to promote wave
- cicd_pipeline: pipeline smoke-test step hits Prometheus/health endpoints after deploy

### Automated Rollback on Threshold Breach
- 4 of 6 systems implement automatic rollback triggered by metric thresholds; no human needed
- blue_green_deployment: monitoring checker triggers `switch_traffic(from=green, to=blue)` if error rate or latency exceeds threshold after switch
- canary_deployment: error_rate > 5% (absolute) OR canary > stable + tolerance → auto-rollback; `status = rolled_back`
- rollout_controller: `error_rate_threshold (DECIMAL 5,2)` per rollout; >1% default → auto-pause → operator initiates rollback
- cicd_pipeline: failed smoke test → pipeline status = failed; no promotion to next environment

### Topology-Aware / Wave-Based Progressive Deployment
- 3 of 6 systems progressively roll out to bounded fractions of infrastructure with health gates between steps
- canary_deployment: 1% → 5% → 25% → 50% → 100%; analysis after each step; `canary_steps.weight_value`
- rollout_controller: 5% of fleet per wave; topology-aware (max 33% per AZ, max 2 hosts per rack per wave); canary wave (1–2 hosts, 10 min soak); `wave_size_percent = 5` default
- blue_green_deployment: atomic 100% switch (not gradual) but with pre-switch testing and instant rollback

### Audit Log for All Changes (Who/What/When)
- All 6 systems maintain an immutable audit trail
- cicd_pipeline: `pipeline_runs` + `task_runs`; Kafka `pipeline.status` events → Elasticsearch; 2-year retention
- feature_flag_service: `audit_log` table + Kafka → Elasticsearch; flag changes logged with `old_value`, `new_value`, `changed_by`
- rollout_controller: `host_rollout_status.deployment_log LONGTEXT`; `rollouts.paused_at`, `started_at`, `finished_at`
- blue_green/canary: `deployments`/`canary_rollouts` tables record `triggered_by`, timestamps for all state transitions
- artifact_registry: `scan_results` with timestamp; push events via webhook

### Idempotent Operations
- All 6 systems are safe to re-apply; duplicate artifact upload → increment `reference_count`; re-running rollout → skip hosts already at target version; webhook dedup → Redis SETNX
- artifact_registry: `INSERT … ON DUPLICATE KEY UPDATE reference_count = reference_count + 1`
- rollout_controller: `filterPending(hosts)` skips hosts where `current_version == target_version`
- cicd_pipeline: webhook dedup via Redis `SETNX event_id TTL=300s`; pipeline not triggered twice for same commit

## Common Databases

### MySQL
- All 6; authoritative state with ENUMs for lifecycle status; `UNIQUE KEY` constraints for idempotency; `INDEX idx_status` for queue polling

### S3 / Object Storage
- 4 of 6 (artifact_registry, cicd_pipeline, rollout_controller pre-staged artifacts, blue_green build outputs); 11-nines durability; content-addressable keys; build cache tarballs (zstd-compressed)

### Redis
- 3 of 6 (artifact_registry pull-through cache, cicd_pipeline webhook dedup + build cache index, feature_flag_service SSE pub/sub); `SETNX` for distributed locking; TTL-based dedup; pub/sub for SSE fanout

### Kafka
- 2 of 6 (cicd_pipeline `pipeline.trigger`/`pipeline.status`/`artifact.published`, feature_flag_service flag change events → Elasticsearch); async event streaming; exactly-once with idempotent producer

### Prometheus
- 4 of 6 (blue_green, canary, rollout_controller, cicd_pipeline smoke tests); PromQL for error_rate + p99_latency; query at each health gate

### Elasticsearch
- 2 of 6 (feature_flag_service audit search, cicd_pipeline build logs + pipeline run search); Kafka consumer writes to Elasticsearch index; 2-year retention

## Common Communication Patterns

### SSE / Streaming Push for Real-Time Updates
- feature_flag_service: SSE gateway maintains persistent connections to 12,000 SDK instances; Redis Pub/Sub → SSE gateway → all SDKs; < 5 s propagation
- canary_deployment: Istio xDS push propagates traffic weight changes to all Envoy sidecars within < 5 s

### gRPC for Control-Plane → Data-Plane
- rollout_controller: Agent Manager maintains persistent gRPC connections to 20,000+ host agents; fan-out deploy commands; streaming health reports
- cicd_pipeline: Tekton pipeline controller → task pods via Kubernetes API (gRPC-based)

### Webhook + HMAC Validation for Git Triggers
- cicd_pipeline: GitHub webhooks → Webhook Gateway; validates `X-Hub-Signature-256` (HMAC-SHA256); publishes to Kafka `pipeline.trigger`; dedup via Redis `SETNX event_id`

## Common Scalability Techniques

### In-Process Caching + Server-Side Cache
- feature_flag_service: `ConcurrentHashMap<String, FlagDefinition>` per SDK instance; < 0.1 ms local evaluation; SSE invalidation in < 5 s
- artifact_registry: pull-through cache (proxies Docker Hub → local S3); Redis index for fast manifest lookup

### Layer Deduplication (60–80% Storage Savings)
- artifact_registry: Docker layers stored once per digest regardless of how many images reference them; `reference_count` for GC safety
- Example: 1,500 Java services → from 540 GB (no dedup) to ~47.8 GB (with dedup) per version tag = 91% savings

### Parallel Execution via Job Matrix
- cicd_pipeline: Tekton task matrix splits test suite into N shards running in parallel pods; `shard-index: [0,1,2,3]` for 4× speedup; build cache restored from S3 per shard

### Priority Classes for Queue Management
- cicd_pipeline: `pipeline-critical` (hotfix, priority 1000) > `pipeline-high` (main branch, 500) > `pipeline-normal` (feature, 100); Kubernetes PriorityClass; hotfixes preempt normal builds

## Common Deep Dive Questions

### How do you achieve zero dropped requests during a production traffic switch?
Answer: Blue-green traffic switch via Kubernetes Service selector patch — atomically updates the `spec.selector` to point from blue pods to green pods in a single API call. Connection draining ensures in-flight requests to blue complete before pods terminate (default drain: 30 s). Pre-switch requirements: green pods must have `ready_replicas == spec.replicas`; smoke tests must pass. Switch itself < 1 s. Rollback: same selector patch in reverse — no redeployment, < 5 s.
Present in: blue_green_deployment_system

### How do you determine if a canary is safe to promote without false positives/negatives?
Answer: Baseline comparison, not absolute thresholds: `canary_error_rate <= stable_error_rate + tolerance`. This accounts for natural traffic variance (if stable is at 0.5%, canary at 0.8% may be acceptable). Absolute safety net: `if canary_error_rate > 5%: always fail`. Statistical significance: at 1% canary with 10K RPS service → 100 RPS = 30,000 requests per 5-min analysis window. Analysis every 60 s; `inconclusive` result (too few samples) counts as pass to avoid premature rollbacks during ramp-up.
Present in: canary_deployment_system

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.9% (rollout), 99.95% (CI/CD, canary), 99.99% (feature flags, artifact registry, blue-green) |
| **Durability** | 11 nines (artifact S3), 99.999% (MySQL semi-sync for deployment records) |
| **Traffic Switch Time** | < 1 s (blue-green LB), < 10 s (canary rollback), < 30 s (wave pause) |
| **Propagation** | < 5 s (flag updates SSE, Istio traffic weights, canary analysis), < 5 min (artifact replication, vuln scan) |
| **Throughput** | 10M flag evals/sec (in-process), 10K concurrent pulls (registry), 5,000 concurrent pipelines |
| **Blast Radius** | 5% per wave (rollout), 1% initial (canary), 0% during smoke tests (blue-green) |
| **Retention** | 2 years audit log (pipeline), 30 days build artifacts, 11 nines artifact blobs (S3) |
