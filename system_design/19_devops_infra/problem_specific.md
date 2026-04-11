# Problem-Specific Design — DevOps Infra (19_devops_infra)

## CI/CD Pipeline

### Unique Functional Requirements
- 500 K pipeline triggers/day; 6 M jobs/day; 37,500 concurrent jobs at peak
- Webhook ingestion from GitHub/GitLab/Bitbucket with HMAC signature validation
- DAG-based stage execution with parallel job dispatch; matrix builds (e.g., test against Python 3.9/3.10/3.11)
- Blue-green and canary deployment strategies; approval gates with CODEOWNERS/policy enforcement
- Log streaming from agents (Fluent Bit → S3); artifact registry with SHA-256 content-addressable storage

### Unique Components / Services
- **Webhook Gateway**: terminates TLS; validates HMAC-SHA256 signatures on incoming Git webhooks; rate-limits per org; publishes to Kafka `webhook-events` partitioned by org_id
- **Lua Atomic Job Dequeue**: jobs stored in Redis `ZSET queue:jobs:org:{org_id}` by priority score; agent pull uses Lua script to atomically `ZREVRANGE` candidates → match labels → `ZREM` from queue + `HSET` in assigned_jobs_hash; prevents double-dispatch without distributed locks
  - Priority score formula: `urgency_level × 1000 - queued_duration_seconds` (aging prevents starvation)
  - Reaper process every 10 s: scans assigned_jobs_hash for `deadline < now()` → re-enqueues (increments retry_count) → alerts if `retry_count > max_retries`
- **Canary Deployment Controller**: traffic ramp 10% → 25% → 50% → 100%; health check loop every 30 s via Datadog/Prometheus API; automatic rollback if HTTP 5xx rate or P99 latency exceeds thresholds; blue-green = atomic K8s Service selector swap
- **Agent Pull with Long-Poll**: agents call `PUT /agents/{agent_id}/heartbeat` returning assigned job or null; heartbeat timeout 30 s → reaper marks agent offline and re-enqueues jobs
- **Test Parallelization (LPT Bin-Packing)**: Longest Processing Time algorithm assigns test files to agent bins; historical runtime data from PostgreSQL used to estimate test duration; minimizes makespan for parallel test execution

### Unique Data Model
- **pipeline_runs**: project_id, trigger_type (push/pull_request/tag/manual/schedule), trigger_sha VARCHAR(40), status (pending/running/success/failed/cancelled/waiting_approval)
- **jobs**: stage_id, agent_id, runner_labels TEXT[] (capability requirements), exit_code, log_path VARCHAR(1024) (S3 key), retry_count, max_retries, timeout_seconds INT DEFAULT 3600
- **agents**: hostname, labels TEXT[], status (idle/busy/offline/draining), last_heartbeat TIMESTAMPTZ, version; `idx_agents_status_labels ON agents(status, labels)` for O(1) capability matching
- **deployments**: pipeline_run_id, artifact_id, environment, strategy (blue_green/canary/rolling), status, manifest JSONB (K8s manifest snapshot for rollback), canary_weight SMALLINT
- **artifacts**: job_id, artifact_type (docker_image/binary/archive/report), storage_key, sha256 VARCHAR(64), expires_at

### Algorithms

**Lua Atomic Job Dequeue (key excerpt):**
```lua
local candidates = redis.call('ZREVRANGE', jobs_key, 0, 99, 'WITHSCORES')
for i = 1, #candidates, 2 do
    local job_id = candidates[i]
    -- check label compatibility
    if match then
        redis.call('ZREM', jobs_key, job_id)
        redis.call('HSET', assigned_key, job_id, encode({agent_id, assigned_at, deadline}))
        return job_id
    end
end
return nil
```

### Key Differentiator
CI/CD Pipeline's uniqueness is its **Redis Lua atomic job dequeue with label-based matching + canary 30 s health-check feedback loop**: Lua atomicity (single Redis command sequence, no distributed lock needed) prevents double-dispatch at 37,500 concurrent jobs; LPT bin-packing minimizes test suite makespan using historical runtime data; blue-green = atomic K8s Service selector swap with no traffic split complexity; canary = Istio/ALB weight manipulation with automatic rollback on health check failure; HMAC-validated webhook gateway is the trust boundary between public Git providers and internal systems.

---

## Feature Flag Service

### Unique Functional Requirements
- In-process SDK evaluation in < 0.1 ms (no network call per evaluate())
- Kill switch: global enable/disable per flag per environment — highest priority override
- SSE delta propagation: < 5 s from management write to all connected server-side SDKs
- Prerequisite flags: flag B only evaluates if flag A resolved to a specific variant
- Client-side SDKs must not receive business rule details (use server-side evaluation endpoint)
- 50 K persistent SSE connections sustained

### Unique Components / Services
- **Evaluation Engine (In-Process SDK)**: downloads full flag ruleset at startup (MessagePack-encoded, ~50 MB compressed); evaluates in-process using `threading.RWLock` for safe concurrent reads; rule priority: enabled check → prerequisites → targeting rules (ordered by priority) → percentage_rollout → default_variant
- **MurmurHash3 Bucketing**: `hash_input = f"{seed}.{flag_key}.{user_key}"`; `bucket = mmh3.hash(hash_input, signed=False) % 10_000`; `threshold = int(rollout_pct × 100)`; user in rollout if `bucket < threshold`; deterministic (same user always same bucket), independent per flag via seed
- **Kill Switch**: `flag_environments.enabled = FALSE` → SDK returns `default_variant` with reason `"flag_disabled"`; checked first in evaluation loop before any rule evaluation; propagated within 5 s via SSE
- **SSE Delta Push**: SSE Server maintains connections keyed by org × environment; on `FlagChangedEvent` from Kafka → push `{updated_flags: ["checkout-redesign"], snapshot_version: "v42"}`; SDK re-fetches updated snapshot or applies full flag diff embedded in delta
- **Client-Side Flag API**: `POST /v1/sdk/evaluate` accepts user context; evaluates server-side; returns only variant values — business rules (condition logic) not exposed to browser/mobile

### Unique Data Model
- **flags**: org_id, key VARCHAR(255) UNIQUE, flag_type (boolean/string/number/json), archived BOOLEAN
- **flag_environments**: (flag_id, environment_id) UNIQUE; enabled BOOLEAN (kill switch); default_variant; version BIGINT (incremented per change)
- **flag_rules**: flag_env_id, priority INT, rule_type (targeting/percentage_rollout/prerequisite); rollout_percent NUMERIC(5,2), rollout_seed VARCHAR(128); prerequisite_flag_id + prerequisite_variant
- **rule_conditions**: rule_id, attribute VARCHAR(255), operator (equals/not_equals/contains/in/not_in/starts_with/regex/gt/lt/gte/lte/between/semver_gte), value JSONB, negate BOOLEAN
- **segments** + **segment_conditions**: reusable user segment definitions referenced by rules
- **Redis**: `snapshot:{org_id}:{env_id}` → full MessagePack snapshot; versioned by ETag = SHA-256 of content

### Key Differentiator
Feature Flag Service's uniqueness is its **in-process evaluation engine + MurmurHash3 bucketing + SSE delta propagation**: in-process evaluation (< 0.1 ms, no network) scales to millions of evaluations/s without any flag service load; MurmurHash3 `mmh3.hash(f"{seed}.{flag_key}.{user_key}") % 10000` gives deterministic independent bucketing per flag per user; SSE delta pushes only changed flags (not full snapshot) to 50 K connections in < 5 s; prerequisite flags enable dependency-ordered rollout (e.g., enable advanced feature only if basic feature is on); client-side evaluation endpoint prevents business rule leakage to browser.

---

## Config Management Service

### Unique Functional Requirements
- Hot reload: config change reaches all 100 K running application instances within 5 seconds without restart
- Namespace hierarchy: `payments/gateway`, `payments/fraud`, `infra/cache` — hierarchical grouping with RBAC per namespace prefix
- vault:// URI scheme: config values containing `vault://secret/path/key` are resolved by the SDK via Vault Connector at startup and cached; never persisted to logs
- Rollback = new immutable version record (not revert): rollback creates a new config_version pointing to old values, preserving full history
- JSON Schema validation on every write: schema violation returns detailed error before commit

### Unique Components / Services
- **Schema Validator**: validates incoming config values against `config_schemas.json_schema JSONB` before write is committed; enforces `type: integer, minimum: 100, maximum: 30000` etc.; detailed error messages on violation
- **Vault Connector**: resolves `vault://` references in config values; uses application's Vault token scoped to specific namespaces; caches resolved secrets in-memory (never in Redis); handles Vault lease renewal and secret rotation notifications
- **Hot Reload SDK**: 4-step: (1) load full snapshot on init; (2) resolve vault refs; (3) persist snapshot to local disk cache for startup resilience; (4) open SSE connection; on SSE delta → invoke registered `HotReloadCallback(key, oldValue, newValue)` with RWLock
- **ETag Polling**: `GET /sdk/namespace/{ns}?env=prod` with `If-None-Match: v137` → 304 if unchanged; 200 + new snapshot if changed; mobile/embedded SDKs poll every 60 s; server-side SDKs prefer SSE
- **Sidecar Process**: for legacy apps that read config from files; sidecar writes config to mounted volume, uses `inotify` to notify app of changes; no SDK code changes required
- **Kubernetes ConfigMap Sync Operator**: syncs config service values to K8s ConfigMaps and Secrets on each `ConfigChangedEvent`; kubelet automatically updates mounted ConfigMap volumes (1–60 s latency)
- **Immutable Version History**: `config_versions` table stores every value change (config_key_id, environment_id, version, value, changed_by, change_reason); rollback = INSERT new config_values row pointing to desired historical value, then trigger new snapshot

### Unique Data Model
- **namespaces**: org_id, path VARCHAR(1024) (e.g., `payments/gateway`), schema_id FK; UNIQUE(org_id, path)
- **config_keys**: namespace_id, key VARCHAR(1024), full_path VARCHAR(2048) (e.g., `payments/gateway/timeout_ms`), value_type (string/integer/float/boolean/json/vault_ref), is_sensitive BOOLEAN
- **config_values**: (config_key_id, environment_id) UNIQUE; value TEXT; encrypted_value BYTEA (AES-256-GCM for is_sensitive); version BIGINT
- **config_versions**: append-only version history; config_key_id, environment_id, version, value, changed_by, change_reason — rollback reads here
- **namespace_snapshots**: (namespace_id, environment_id) UNIQUE; version BIGINT, etag VARCHAR(64) SHA-256, storage_key (S3 key for large namespaces)
- **access_policies**: namespace_path VARCHAR(1024) (supports prefix `payments/*`); principal_type (user/group/service_account); permission (read/write/admin)
- **service_accounts**: name, api_key_hash, allowed_namespaces TEXT[] — for SDK authentication

### Key Differentiator
Config Management Service's uniqueness is its **vault:// URI scheme + SSE hot reload + immutable version history**: vault:// references embed Vault secret paths directly in config values — the SDK resolves them at startup using app-scoped tokens without persisting secrets; SSE delta (key-value pair, not full snapshot) reaches 100 K connections in < 5 s; hot reload callbacks registered in SDK code enable zero-restart updates to circuit breakers, timeouts, and feature tuning; rollback = new config_version INSERT preserves complete audit trail; JSON Schema validation enforces type safety before commit; sidecar + K8s operator provide three deployment models (in-process SDK, file-based, ConfigMap) for different app architectures.

---

## A/B Testing Platform

### Unique Functional Requirements
- Statistical validity with sequential testing (no peeking problem): mSPRT (Mixture Sequential Probability Ratio Test) for valid early stopping
- Variance reduction via CUPED (Controlled-experiment Using Pre-Experiment Data): 20–40% variance reduction
- Multiple comparison correction: Bonferroni α/n_metrics correction when tracking multiple metrics
- Sample Ratio Mismatch (SRM) detection: chi-squared test on assignment counts vs. expected weights
- Mutual exclusion via Layers: experiments in the same layer partition traffic to prevent interference

### Unique Components / Services
- **Randomization (delegated to Feature Flag SDK)**: `bucket = MurmurHash3(experiment_id + "." + user_id) % 10000`; traffic gate separate from variant assignment: `IsUserInExperiment` uses `experiment_id + ".traffic." + user_id` as separate hash to decouple enrollment from variant; Layer bucketing: `BucketUserInLayer(layer_id, user_id)` determines which experiment in a layer the user lands in
- **mSPRT Early Stopping**: `log_stat = 0.5 × log(V/(V+τ²)) + (τ² × δ̂²) / (2V(V+τ²))`; `can_stop = log_stat > log(1/α)`; rho parameter ρ=0.5 controls the mixture weight; stored as `sequential_test_statistic` in experiment_results; prevents inflated false positives from continuous monitoring (unlike naive p-value checks)
- **CUPED Variance Reduction**: `Y_cuped = Y - θ × (X - mean_X)` where `θ = Cov(Y, X) / Var(X)`, X = pre-experiment covariate (e.g., prior 7-day conversion rate); reduces standard error by 20–40%, effectively increasing statistical power without additional sample size
- **Bonferroni Correction**: when experiment has n_metrics, significance threshold adjusted to `α / n_metrics` for each metric; prevents false discovery inflation when tracking CTR + revenue + retention simultaneously
- **SRM Detection**: chi-squared test on actual exposure counts vs. expected (traffic_weight × total); `χ² = Σ (observed - expected)² / expected`; SRM flags invalid randomization (e.g., bot traffic, SDK bug) before results are trusted
- **Statistical Computation Engine**: queries ClickHouse every 5 min for `experiment_exposures` and `experiment_events`; computes per-metric: n, mean, variance, p-value, z-score, lift, lift_CI lower/upper; writes pre-computed JSON to `experiment_results.variants_data JSONB`; results served from PostgreSQL (< 100 ms dashboard reads)
- **Guardrail Monitor**: runs alongside Computation Engine; checks guardrail metrics (e.g., error_rate < 2%) against configured thresholds; triggers experiment suspension (`status = stopped`) on breach

### Unique Data Model
- **experiments**: hypothesis TEXT, status (draft/running/paused/concluded/stopped), traffic_percent NUMERIC(5,2), layer_id FK, flag_id/flag_key (linked Feature Flag Service flag), min_detectable_effect, confidence_level DEFAULT 0.95, statistical_power DEFAULT 0.80, required_sample_size
- **metrics**: metric_type (binary/continuous/count/ratio), aggregation (count/sum/mean/p50/p95/p99), direction (up/down — is higher better?), win_horizon_days DEFAULT 14; ratio metrics have numerator_event + denominator_event fields
- **experiment_metrics**: is_primary BOOLEAN (exactly one per experiment), is_guardrail BOOLEAN, guardrail_threshold NUMERIC(12,6), guardrail_relative_threshold
- **experiment_results**: computed_at, variants_data JSONB `{"control": {n, mean, variance, sum}, "treatment": {...}}`, p_value, z_score, lift, lift_lower_ci, lift_upper_ci, is_significant, sequential_test_statistic; refreshed every 5 min
- **layers**: org_id, name — mutual exclusion group; experiments in same layer partition the 0–9999 bucket space
- **ClickHouse event_store**: `experiment_exposures (experiment_id, user_id, variant, timestamp, attributes)` and `experiment_events (experiment_id, user_id, event_type, value, timestamp)`; partitioned by experiment_id for fast per-experiment aggregations

### Algorithms

**mSPRT formula:**
```
V = sample_variance_of_metric_difference
τ = prior_standard_deviation (hyperparameter)
δ̂ = estimated_effect (treatment_mean - control_mean)
log_stat = 0.5 × log(V / (V + τ²)) + (τ² × δ̂²) / (2V × (V + τ²))
can_stop = log_stat > log(1/α)   # e.g. log(1/0.05) = 2.996
```

**CUPED formula:**
```
θ = Cov(Y_post, X_pre) / Var(X_pre)   # computed from control group history
Y_cuped = Y_post - θ × (X_pre - mean(X_pre))
# Y_cuped has same mean as Y_post but reduced variance
```

### Key Differentiator
A/B Testing Platform's uniqueness is its **mSPRT sequential testing + CUPED variance reduction + Layers mutual exclusion**: mSPRT log-statistic threshold replaces fixed-horizon t-test, enabling valid early stopping without peeking inflation; CUPED `Y_cuped = Y - θ(X - mean_X)` achieves 20–40% variance reduction using pre-experiment covariates, equivalent to 25–70% sample size reduction; Layers partition the 0–9999 bucket space via `MurmurHash3(layer_id + "." + user_id)` ensuring users can't be in two interfering experiments simultaneously; SRM chi-squared detection prevents invalid results from assignment bugs before conclusions are drawn.
