# System Design: A/B Testing Platform

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Experiment Design**: Create experiments with a control group and one or more treatment variants. Define the traffic allocation (e.g., 50/50, 33/33/33, 10% test + 90% holdout).
2. **Randomization (User Bucketing)**: Deterministically and consistently assign users to experiment variants using hash-based bucketing. The same user must always receive the same variant within an experiment.
3. **Targeting and Eligibility**: Define eligibility criteria for experiment participation (e.g., "US users only", "users on the Pro plan", "users who have completed onboarding").
4. **Metric Collection**: Collect experiment events (exposures, conversions, page views, custom events) and associate them with the user's assigned variant.
5. **Statistical Significance Testing**: Compute whether a treatment variant's metric performance is statistically significantly different from control, using frequentist (two-sided t-test, z-test for proportions) and/or Bayesian methods.
6. **Guardrail Metrics**: Define per-experiment safety metrics (e.g., error rate, latency) that trigger automatic experiment stopping if violated, regardless of the primary metric.
7. **Experiment Lifecycle Management**: Experiments progress through states: `draft → running → paused → concluded`. Support early stopping (by the platform when significance is reached, or manually).
8. **Segment Analysis**: Compute metric results broken down by user segments (country, plan, device type) to detect heterogeneous treatment effects.
9. **Multiple Experiments**: A user can be in multiple experiments simultaneously. The system ensures mutual exclusivity where required (via traffic layers/namespaces).
10. **Experiment Audit Trail**: Record all experiment configuration changes with actor and timestamp.
11. **Integration with Feature Flag Service**: Experiments are implemented via feature flags; the A/B platform reads exposure events emitted by the feature flag SDK.
12. **Results Dashboard**: Real-time (within 5 minutes) experiment results visualization with confidence intervals, p-values, and sample size indicators.

### Non-Functional Requirements

- **Throughput**: 1 trillion experiment exposures per day across all customers (same scale as feature flag evaluations).
- **Event Ingestion Latency**: Exposure and conversion events must be durably stored within 1 second of client emission (p99).
- **Results Freshness**: Experiment results must be updated within 5 minutes of new event ingestion.
- **Statistical Accuracy**: Results must correctly control Type I error (false positive) rate at α = 0.05. Multiple testing corrections must be applied for experiments with multiple metrics.
- **Availability**: 99.9% for event ingestion (can lose < 0.1% of events). 99.9% for results reads. Experiment assignment (bucketing) is fully client-side (no availability dependency).
- **Scalability**: Horizontal scaling for event ingestion and results computation independently.
- **Consistency**: A user assigned to a variant stays in that variant for the experiment's duration (no re-bucketing mid-experiment).

### Out of Scope

- Feature flag evaluation engine (uses the feature flag service's SDK).
- UI analytics and session replay.
- Multivariate testing (MVT) with interaction effect modeling.
- Causal inference beyond A/B testing (e.g., difference-in-differences, synthetic controls).
- Revenue attribution beyond experiment-exposed user cohorts.

---

## 2. Users & Scale

### User Types

| User Type | Description | Primary Actions |
|---|---|---|
| Product Manager | Designs and monitors experiments | Create experiments, set metrics, view results |
| Data Scientist | Interprets results, designs statistical framework | Configure significance thresholds, segment analyses, power calculations |
| Developer | Instruments experiments in code | Add feature flag evaluations, add conversion events |
| Exec/Stakeholder | Reviews business impact | View high-level results dashboard |
| Automated System | Emits exposure/conversion events | SDK calling ingest API on flag evaluation |

### Traffic Estimates

Assumptions:
- 500 organizations; typical large org: 50M DAU, 50 simultaneous experiments.
- Each user in an experiment: 1 exposure event + avg 0.1 conversion events per experiment per day.
- 50 experiments × 50M users × 10% of users enrolled per experiment = 25M enrolled users × 50 experiments across org.
- Scale-down for realistic launch: 50 orgs × 1M DAU × 10 experiments × 30% enrollment = 150M exposures/day.

| Metric | Calculation | Result |
|---|---|---|
| Exposure events/day | 150M enrolled-user-experiment-days | 150M/day |
| Conversion events/day | 150M × 0.1 | 15M/day |
| Total events/day | 150M + 15M | 165M/day |
| Events/sec (avg) | 165M / 86,400 | ~1,910/sec |
| Events/sec (peak 5x) | 1,910 × 5 | ~9,550/sec |
| Experiment assignment calls/day | Same as exposure events (in-process, no network) | 150M/day (zero network traffic) |
| Results computation jobs/day | 50 active experiments × 288 computations/day (every 5 min) | 14,400 jobs/day |
| Dashboard reads/day | ~10,000 PM/DS users × 20 reads/day | 200,000 reads/day = ~2.3 reads/sec |

### Latency Requirements

| Operation | Target (p50) | Target (p99) | Notes |
|---|---|---|---|
| Experiment assignment (bucketing) | < 0.1 ms | < 1 ms | In-process in feature flag SDK |
| Event ingestion acknowledgment | 100 ms | 1 s | API endpoint to Kafka |
| Results computation (incremental) | 2 min | 5 min | Aggregation job on new events |
| Dashboard read (results) | 100 ms | 500 ms | Pre-computed results from DB |
| Experiment creation (API) | 200 ms | 1 s | Write to experiment DB |
| Power calculation (pre-experiment) | 500 ms | 3 s | CPU-bound statistical computation |

### Storage Estimates

| Data Type | Size per Unit | Volume | Retention | Total |
|---|---|---|---|---|
| Raw exposure events | ~200 B/event | 150M/day | 180 days | 150M × 200 B × 180 = ~5.4 TB |
| Raw conversion events | ~300 B/event | 15M/day | 180 days | 15M × 300 B × 180 = ~810 GB |
| Experiment definitions | ~10 KB/experiment | 10,000 experiments total | Indefinite | ~100 MB |
| Aggregated results (per experiment, per day) | ~50 KB/experiment/day | 10,000 active × 180 days | 2 years | 10K × 50 KB × 180 = ~90 GB |
| User assignment table (if persisted) | ~100 B/assignment | 150M/day | Duration of experiment | 150M × 100 B = ~15 GB/day (not persisted — computed on the fly) |
| Metric definitions | ~2 KB/metric | 100 metrics/org × 500 orgs | Indefinite | ~100 MB |

Total data: ~6.3 TB raw events + ~100 GB aggregated results over 180 days. Manageable in a columnar store (ClickHouse, BigQuery, Redshift) for event queries and PostgreSQL for experiment metadata.

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Event ingest (SDK → API) | 9,550 events/sec × 250 B avg | ~2.4 MB/s peak |
| Kafka event bus | Same as ingest (single-hop) | ~2.4 MB/s |
| Event store writes | Same as Kafka | ~2.4 MB/s |
| Results query (computation layer → event store) | Batch queries, not real-time | Burst: ~100 MB/s during computation window |
| Dashboard reads | 2.3 reads/sec × 50 KB response | ~115 KB/s (negligible) |

---

## 3. High-Level Architecture

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                       EXPERIMENT MANAGEMENT PLANE                                      │
│                                                                                        │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐                   │
│  │  Management UI   │   │  Experiment API  │   │  Power           │                   │
│  │  (Experiment     │──▶│  (REST, RBAC)    │   │  Calculator      │                   │
│  │   designer,      │   │  CRUD + lifecycle│──▶│  (pre-experiment │                   │
│  │   results dash)  │   │  management      │   │   sample size)   │                   │
│  └──────────────────┘   └────────┬─────────┘   └──────────────────┘                   │
│                                  │                                                     │
│                                  ▼                                                     │
│  ┌──────────────────────────────────────────────────────────────────┐                  │
│  │                   Experiment Store (PostgreSQL)                  │                  │
│  │  experiments | variants | metrics | assignments |               │                  │
│  │  experiment_metrics | guardrails | audit_events                  │                  │
│  └──────────────────────────────────────────────────────────────────┘                  │
│                                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐                  │
│  │                   Feature Flag Service (external)                │                  │
│  │  - Holds flag → experiment linkage                              │                  │
│  │  - SDK emits flag_evaluated events (exposure source)            │                  │
│  └──────────────────────────────────────────────────────────────────┘                  │
└────────────────────────────────────────────────────────────────────────────────────────┘
                                  │ Experiment config changes
                                  │ (sync to feature flag service)
                                  ▼
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                         EVENT INGESTION PLANE                                          │
│                                                                                        │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐                   │
│  │  Event Ingest    │   │  Event Validator │   │  Event Bus       │                   │
│  │  API             │──▶│  (schema check,  │──▶│  (Kafka)         │                   │
│  │  POST /events    │   │   dedup, enrich) │   │  exp-events      │                   │
│  │  (batched, async)│   │                  │   │  (partitioned    │                   │
│  └──────────────────┘   └──────────────────┘   │   by user_id)    │                   │
│                                                  └────────┬─────────┘                  │
│                                                           │                            │
│                                                           ▼                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────┐   │
│  │                        Event Store (ClickHouse)                                 │   │
│  │  experiment_exposures (experiment_id, user_id, variant, timestamp, attributes)  │   │
│  │  experiment_events    (experiment_id, user_id, event_type, value, timestamp)    │   │
│  └─────────────────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                      COMPUTATION PLANE                                                 │
│                                                                                        │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐                   │
│  │  Experiment      │   │  Statistical     │   │  Results Store   │                   │
│  │  Scheduler       │──▶│  Computation     │──▶│  (PostgreSQL)    │                   │
│  │  (triggers batch │   │  Engine          │   │  experiment_     │                   │
│  │   every 5 min)   │   │  (t-tests, z-    │   │  results,        │                   │
│  │                  │   │   tests, CUPED,  │   │  metric_         │                   │
│  └──────────────────┘   │   sequential)    │   │  snapshots       │                   │
│                          └──────────────────┘   └──────────────────┘                   │
│                                                                                        │
│  ┌──────────────────┐   ┌──────────────────┐                                           │
│  │  Guardrail       │   │  Early Stopping  │                                           │
│  │  Monitor         │   │  Controller      │                                           │
│  │  (checks safety  │   │  (sequential     │                                           │
│  │   metrics)       │   │   testing /      │                                           │
│  └──────────────────┘   │   mSPRT)         │                                           │
│                          └──────────────────┘                                          │
└────────────────────────────────────────────────────────────────────────────────────────┘
              │
              ▼
┌───────────────────────────────────────────────────────┐
│              Notification Service                      │
│  (Slack/email: significance reached, guardrail        │
│   triggered, experiment auto-stopped)                 │
└───────────────────────────────────────────────────────┘
```

**Component Roles:**

- **Experiment API**: CRUD for experiments, variants, metrics, guardrails. Validates experiment configuration (e.g., variant weights sum to 100%). Syncs experiment-to-flag linkage with the Feature Flag Service. Manages experiment lifecycle state machine.
- **Power Calculator**: Given a target minimum detectable effect (MDE), baseline conversion rate, and desired statistical power (1-β) and significance level (α), computes the required sample size per variant. CPU-bound; runs synchronously on the management API server.
- **Experiment Store (PostgreSQL)**: Source of truth for experiment metadata. Never stores event data or raw metric values.
- **Feature Flag Service**: Holds the flag that controls which variant each user receives. The A/B platform creates/updates a flag rule in the feature flag service when an experiment starts. The feature flag SDK emits `flag_evaluated` events that become exposure events for the A/B platform.
- **Event Ingest API**: HTTP endpoint for receiving batched exposure and conversion events. Validates, deduplicates, and publishes to Kafka. Does not compute results.
- **Event Bus (Kafka)**: Durable buffer between event ingest and event storage. Partitioned by `user_id` to ensure all events for a user land in the same partition (important for session-level metrics).
- **Event Store (ClickHouse)**: Columnar OLAP store for raw event data. Optimized for aggregation queries over large datasets. Schema is denormalized for query performance.
- **Experiment Scheduler**: Triggers the Statistical Computation Engine every 5 minutes for each running experiment. Uses a priority queue to prioritize experiments approaching significance.
- **Statistical Computation Engine**: Reads aggregated event data from ClickHouse, computes per-metric statistics (mean, variance, sample size, p-value, confidence interval, lift), writes results to the Results Store.
- **Results Store (PostgreSQL)**: Stores pre-computed results snapshots for fast dashboard reads. Also stores historical results for each computation run (for trend analysis).
- **Guardrail Monitor**: Runs alongside the Computation Engine; computes guardrail metric values and compares against thresholds. Triggers experiment suspension if a guardrail is breached.
- **Early Stopping Controller**: Implements sequential testing (mSPRT — Mixture Sequential Probability Ratio Test) to determine if an experiment can be stopped early with valid statistical guarantees.

**Primary Use-Case Data Flow (user exposed to experiment, converts):**

1. User makes a request to the application. Application code calls `flags.evaluate("checkout-redesign-v2", user_context)`.
2. Feature flag SDK evaluates the flag in-process (< 0.1ms). Returns `{ "variant": "treatment", "reason": "percentage_rollout" }`.
3. Application renders the treatment variant for the user.
4. SDK (or application code) records an exposure event in the SDK's local buffer: `{ "experiment_id": "exp_001", "user_id": "u_abc", "variant": "treatment", "timestamp": "..." }`.
5. SDK background thread flushes batched events to the Event Ingest API every 5 seconds.
6. User completes checkout (conversion). Application code records a conversion event: `{ "experiment_id": "exp_001", "user_id": "u_abc", "event_type": "checkout_completed", "value": 49.99 }`.
7. Event Ingest API validates and publishes both events to Kafka topic `exp-events`.
8. A Kafka consumer writes events to ClickHouse in micro-batches (every 5 seconds).
9. Every 5 minutes, the Experiment Scheduler triggers the Statistical Computation Engine for `exp_001`.
10. Computation Engine queries ClickHouse: aggregate exposures and checkout events by variant, compute mean order value per variant, run a two-sample t-test.
11. Results written to the Results Store: `treatment: mean=$49.99, control: mean=$42.30, p-value=0.002, lift=+17%, CI=[+8%, +26%], significant=true`.
12. PM opens the dashboard, sees the results (pre-computed, served from Results Store in < 100ms).
13. Guardrail Monitor ran alongside step 10 and found that the error rate for the treatment group (0.8%) is below the guardrail threshold (2%), so the experiment continues.

---

## 4. Data Model

### Entities & Schema

```sql
-- Organizations
CREATE TABLE organizations (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug            VARCHAR(64) UNIQUE NOT NULL,
    name            VARCHAR(255) NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Experiments
CREATE TABLE experiments (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    key             VARCHAR(255) NOT NULL,          -- URL-safe identifier
    name            VARCHAR(512) NOT NULL,
    description     TEXT,
    hypothesis      TEXT,                           -- scientific hypothesis statement
    status          VARCHAR(32) NOT NULL DEFAULT 'draft',
                                                    -- draft,running,paused,concluded,stopped
    traffic_percent NUMERIC(5,2) NOT NULL DEFAULT 100.0, -- % of eligible users in experiment
    layer_id        UUID REFERENCES layers(id),    -- mutual exclusion group
    flag_id         UUID,                           -- linked feature flag key
    flag_key        VARCHAR(255),
    min_detectable_effect NUMERIC(8,6),             -- MDE for power calculation
    confidence_level NUMERIC(4,3) NOT NULL DEFAULT 0.95,
    statistical_power NUMERIC(4,3) NOT NULL DEFAULT 0.80,
    required_sample_size INT,                       -- from power calculation
    started_at      TIMESTAMPTZ,
    concluded_at    TIMESTAMPTZ,
    conclusion      TEXT,                           -- written conclusion by PM/DS
    created_by      UUID REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, key)
);

-- Experiment variants (control + treatments)
CREATE TABLE variants (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    experiment_id   UUID NOT NULL REFERENCES experiments(id) ON DELETE CASCADE,
    key             VARCHAR(255) NOT NULL,          -- 'control', 'treatment', 'treatment_b'
    name            VARCHAR(512),
    description     TEXT,
    traffic_weight  NUMERIC(5,2) NOT NULL,          -- e.g., 50.00 (percent of experiment traffic)
    is_control      BOOLEAN NOT NULL DEFAULT false,
    flag_variant_key VARCHAR(255),                  -- linked flag variant key
    UNIQUE(experiment_id, key)
);

-- Metrics tracked by experiments
CREATE TABLE metrics (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    name            VARCHAR(255) NOT NULL,
    key             VARCHAR(255) NOT NULL,          -- e.g., 'checkout_conversion_rate'
    description     TEXT,
    metric_type     VARCHAR(32) NOT NULL,           -- 'binary','continuous','count','ratio'
    event_type      VARCHAR(255) NOT NULL,          -- event type to count/aggregate
    aggregation     VARCHAR(32) NOT NULL DEFAULT 'count', -- 'count','sum','mean','p50','p95','p99'
    numerator_event VARCHAR(255),                   -- for ratio metrics: e.g., 'checkout_completed'
    denominator_event VARCHAR(255),                 -- for ratio metrics: e.g., 'checkout_started'
    direction       VARCHAR(8) NOT NULL DEFAULT 'up', -- 'up' or 'down' (is higher better?)
    win_horizon_days INT NOT NULL DEFAULT 14,       -- stop collecting after N days post-exposure
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, key)
);

-- Metrics attached to experiments
CREATE TABLE experiment_metrics (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    experiment_id   UUID NOT NULL REFERENCES experiments(id) ON DELETE CASCADE,
    metric_id       UUID NOT NULL REFERENCES metrics(id),
    is_primary      BOOLEAN NOT NULL DEFAULT false,  -- exactly one primary metric per experiment
    is_guardrail    BOOLEAN NOT NULL DEFAULT false,
    guardrail_direction VARCHAR(8),                  -- 'up' or 'down' (which direction violates)
    guardrail_threshold NUMERIC(12,6),               -- absolute threshold value
    guardrail_relative_threshold NUMERIC(8,6),       -- relative % change threshold
    UNIQUE(experiment_id, metric_id)
);

-- Layers for mutual exclusion
CREATE TABLE layers (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    name            VARCHAR(255) NOT NULL,
    description     TEXT,
    UNIQUE(org_id, name)
);

-- Computed results snapshots (written every 5 minutes by Computation Engine)
CREATE TABLE experiment_results (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    experiment_id   UUID NOT NULL REFERENCES experiments(id),
    metric_id       UUID NOT NULL REFERENCES metrics(id),
    computed_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    -- Per variant (denormalized; in practice one row per experiment × metric with JSONB variants)
    variants_data   JSONB NOT NULL,
    /*
    variants_data structure:
    {
      "control":   { "n": 50000, "mean": 0.042, "variance": 0.0004, "sum": 2100.0 },
      "treatment": { "n": 51200, "mean": 0.051, "variance": 0.0005, "sum": 2611.2 }
    }
    */
    -- Statistical test results
    p_value         NUMERIC(12,10),
    z_score         NUMERIC(10,6),
    t_statistic     NUMERIC(10,6),
    degrees_of_freedom INT,
    lift            NUMERIC(8,6),                   -- (treatment_mean - control_mean) / control_mean
    lift_lower_ci   NUMERIC(8,6),                   -- 95% CI lower bound on lift
    lift_upper_ci   NUMERIC(8,6),                   -- 95% CI upper bound on lift
    is_significant  BOOLEAN,
    sequential_test_statistic NUMERIC(12,6),        -- for mSPRT early stopping
    INDEX idx_results_exp_metric (experiment_id, metric_id, computed_at DESC)
);

-- Segment-level results breakdown
CREATE TABLE segment_results (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    result_id       UUID NOT NULL REFERENCES experiment_results(id),
    segment_key     VARCHAR(255) NOT NULL,          -- e.g., 'country'
    segment_value   VARCHAR(255) NOT NULL,          -- e.g., 'US'
    variants_data   JSONB NOT NULL,
    p_value         NUMERIC(12,10),
    lift            NUMERIC(8,6),
    INDEX idx_segment_results (result_id, segment_key, segment_value)
);

-- Audit trail
CREATE TABLE audit_events (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL,
    actor_id        UUID REFERENCES users(id),
    event_type      VARCHAR(128) NOT NULL,
    experiment_id   UUID REFERENCES experiments(id),
    before_state    JSONB,
    after_state     JSONB,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Users
CREATE TABLE users (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    email           VARCHAR(255) NOT NULL,
    display_name    VARCHAR(255),
    role            VARCHAR(32) NOT NULL DEFAULT 'member',
    UNIQUE(org_id, email)
);
```

**ClickHouse Event Store Schema (append-only, columnar):**

```sql
-- ClickHouse DDL (uses MergeTree engine family)
CREATE TABLE experiment_exposures (
    org_id          UUID,
    experiment_id   UUID,
    user_id         String,
    variant_key     String,
    exposed_at      DateTime64(3, 'UTC'),
    session_id      String,
    attributes      Map(String, String)  -- country, plan, device, etc.
) ENGINE = MergeTree()
PARTITION BY (toYYYYMM(exposed_at), org_id)
ORDER BY (experiment_id, variant_key, user_id, exposed_at)
TTL exposed_at + INTERVAL 180 DAY DELETE;

-- Deduplication: ReplacingMergeTree to handle duplicate exposure events
CREATE TABLE experiment_exposures_deduped (
    org_id          UUID,
    experiment_id   UUID,
    user_id         String,
    variant_key     String,
    first_exposure  DateTime64(3, 'UTC'),  -- earliest exposure time (used as join key for metrics)
    attributes      Map(String, String)
) ENGINE = ReplacingMergeTree(first_exposure)
PARTITION BY (toYYYYMM(first_exposure), org_id)
ORDER BY (experiment_id, user_id);  -- unique per experiment × user

-- Conversion/metric events
CREATE TABLE experiment_events (
    org_id          UUID,
    experiment_id   UUID,
    user_id         String,
    event_type      String,
    event_value     Float64,               -- numeric value (revenue, duration, etc.)
    occurred_at     DateTime64(3, 'UTC'),
    metadata        Map(String, String)
) ENGINE = MergeTree()
PARTITION BY (toYYYYMM(occurred_at), org_id)
ORDER BY (experiment_id, user_id, event_type, occurred_at)
TTL occurred_at + INTERVAL 180 DAY DELETE;
```

### Database Choice

| Component | Storage | Reason |
|---|---|---|
| **Experiment metadata** | PostgreSQL | ACID, complex relational queries, small data volume |
| **Raw events** | ClickHouse | Columnar, high-throughput inserts, OLAP-optimized aggregation |
| **Results store** | PostgreSQL | Pre-computed; small data; fast key-value reads by experiment_id + metric_id |
| **Event bus** | Kafka | Durable, high-throughput, ordered delivery per partition |

**Why ClickHouse over alternatives:**

| Option | Throughput | Aggregation Speed | Cost | Verdict |
|---|---|---|---|---|
| **ClickHouse** | Excellent (millions of rows/sec ingestion) | Sub-second on billions of rows | Self-hosted cost-effective | **Selected** |
| BigQuery | Excellent | Sub-second | Pay-per-query (can be expensive at 165M events/day) | Viable alternative for cloud-first orgs |
| Redshift | Good | Good (with proper distribution keys) | Fixed cluster cost | Viable; more ops overhead than BigQuery |
| PostgreSQL | Poor (row-oriented) | Very slow for large aggregations | N/A | Rejected for events — wrong engine |
| Cassandra | Excellent writes | Poor aggregations (not designed for OLAP) | Complex ops | Rejected |
| Druid | Excellent | Sub-second | High ops complexity | Over-engineered for this use case |

**ClickHouse** is selected for raw events because its columnar storage, vectorized execution engine, and MergeTree engine family (specifically `ReplacingMergeTree` for deduplication) are optimized for exactly the workload of this platform: high-cardinality group-by queries (GROUP BY experiment_id, variant_key) over hundreds of millions of rows, computing SUM, COUNT, AVG.

---

## 5. API Design

```
Base URL: https://api.abtesting.example.com/v1
Auth: Bearer JWT. SDK events use SDK API key (X-SDK-Key).
Rate limits: Management API — 200 req/min per user. Event ingest — 10,000 events/req, 100 batches/min per SDK instance.
```

### Experiment Management

```
# Create an experiment
POST /orgs/{org_id}/experiments
Body: {
  "key": "checkout-redesign-v2",
  "name": "Checkout Redesign V2",
  "hypothesis": "Simplified checkout flow will increase conversion by 15%",
  "traffic_percent": 50.0,
  "variants": [
    { "key": "control",   "traffic_weight": 50.0, "is_control": true,  "flag_variant_key": "control" },
    { "key": "treatment", "traffic_weight": 50.0, "is_control": false, "flag_variant_key": "treatment" }
  ],
  "metrics": [
    { "metric_key": "checkout_conversion_rate", "is_primary": true, "is_guardrail": false },
    { "metric_key": "order_value_mean",         "is_primary": false, "is_guardrail": false },
    { "metric_key": "checkout_error_rate",      "is_primary": false, "is_guardrail": true,
      "guardrail_direction": "up", "guardrail_threshold": 0.02 }
  ],
  "confidence_level": 0.95,
  "min_detectable_effect": 0.05
}
Response: 201
{ "experiment_id": "exp_001", "required_sample_size": 15420 }

# Start experiment
POST /orgs/{org_id}/experiments/{exp_key}/start
Response: 200
{ "status": "running", "started_at": "2026-04-09T10:00:00Z" }

# Pause experiment
POST /orgs/{org_id}/experiments/{exp_key}/pause
Body: { "reason": "Pausing for Q2 launch freeze" }
Response: 200
{ "status": "paused" }

# Conclude experiment with decision
POST /orgs/{org_id}/experiments/{exp_key}/conclude
Body: {
  "decision": "ship_treatment",  // or "keep_control", "inconclusive"
  "conclusion": "Treatment shows 17% lift in conversion at p=0.002. Shipping."
}
Response: 200
{ "status": "concluded" }

# List experiments
GET /orgs/{org_id}/experiments?status=running&limit=25&cursor=...
Response: 200
{ "experiments": [ { "id", "key", "name", "status", "started_at", "primary_metric" } ] }
```

### Results

```
# Get experiment results
GET /orgs/{org_id}/experiments/{exp_key}/results
Query params:
  metric_key = checkout_conversion_rate (optional; returns all if omitted)
  segment    = country:US (optional; returns segment-level breakdown)
Response: 200
{
  "experiment": { "id", "key", "status", "started_at", "days_running": 7 },
  "computed_at": "2026-04-09T10:45:00Z",
  "sample_size_required": 15420,
  "results": [
    {
      "metric": { "key": "checkout_conversion_rate", "name": "...", "is_primary": true },
      "variants": {
        "control": {
          "n": 51000,
          "mean": 0.042,
          "confidence_interval": [0.039, 0.045],
          "std_error": 0.0015
        },
        "treatment": {
          "n": 51200,
          "mean": 0.051,
          "confidence_interval": [0.048, 0.054],
          "std_error": 0.0016,
          "lift": 0.2143,              // +21.43%
          "lift_ci": [0.10, 0.33],
          "p_value": 0.002,
          "z_score": 3.08,
          "is_significant": true,
          "sequential_test_statistic": 4.7  // > 1 means reject H0 under mSPRT
        }
      },
      "guardrail_status": "ok"  // or "violated"
    }
  ]
}

# Get results history (time series for trend view)
GET /orgs/{org_id}/experiments/{exp_key}/results/history?metric_key=checkout_conversion_rate&limit=50
Response: 200
{
  "history": [
    { "computed_at": "...", "control_mean": 0.042, "treatment_mean": 0.048, "p_value": 0.12, "is_significant": false },
    { "computed_at": "...", "control_mean": 0.042, "treatment_mean": 0.050, "p_value": 0.03, "is_significant": true }
  ]
}

# Pre-experiment power calculation
POST /orgs/{org_id}/power-calculation
Body: {
  "metric_type": "binary",
  "baseline_rate": 0.042,
  "min_detectable_effect": 0.05,  // relative: detect a 5% lift
  "confidence_level": 0.95,
  "statistical_power": 0.80,
  "num_variants": 2
}
Response: 200
{
  "required_sample_size_per_variant": 15420,
  "total_sample_size": 30840,
  "estimated_duration_days": 12  // based on historical daily traffic
}
```

### Event Ingestion

```
# Batch event submission (from SDK)
POST /events
Headers: X-SDK-Key: sdk-xxxx
Body: {
  "events": [
    {
      "type": "exposure",
      "experiment_id": "exp_001",
      "user_id": "u_abc123",
      "variant_key": "treatment",
      "timestamp": "2026-04-09T10:00:00.000Z",
      "attributes": { "country": "US", "plan": "pro", "device": "mobile" }
    },
    {
      "type": "metric_event",
      "experiment_id": "exp_001",
      "user_id": "u_abc123",
      "event_type": "checkout_completed",
      "value": 49.99,
      "timestamp": "2026-04-09T10:01:23.000Z"
    }
  ]
}
Response: 202 Accepted
{ "accepted": 2, "rejected": 0 }
```

---

## 6. Deep Dive: Core Components

### 6.1 Randomization — Hash-Based User Bucketing

**Problem it solves**: Assign users to experiment variants deterministically and consistently so that: (a) the same user always gets the same variant, (b) the assignment is statistically independent across experiments, (c) users can be in multiple experiments without interference, and (d) assignment is computed in-process without a network call.

**Approaches Comparison:**

| Approach | Consistency | Independence | Speed | Requires Central State |
|---|---|---|---|---|
| **Random assignment (stored in DB)** | Yes — stored | Yes | Fast read, slow write | Yes — DB lookup per request |
| **Random assignment (stored in cookie)** | Yes — per device | Yes | Fast | No | Cookie reset = re-randomized |
| **Hash-based (deterministic)** | Yes — deterministic | Yes (with per-experiment salt) | Sub-microsecond | No |
| **Stratified sampling** | Yes — stored | Yes | Slow (sampling algorithm) | Yes |

**Selected: Hash-based deterministic bucketing**

The core formula: `bucket = MurmurHash3(experiment_id + "." + user_id) % 10000`
The user is assigned to variant V if `cumulative_weight(V-1) <= bucket < cumulative_weight(V)`.

Rationale: Hash-based bucketing is computed entirely in the feature flag SDK in-process. No database lookup is required. Independence across experiments is guaranteed by including `experiment_id` in the hash input — the same user hashes to different buckets in different experiments. Consistency is guaranteed because the hash function is deterministic and the user_id and experiment_id are stable.

**Implementation — bucketing engine (embedded in feature flag SDK):**

```go
// BucketUser assigns a user to a variant for an experiment.
// variant_weights: ordered list of {key, cumulative_weight} pairs, e.g.,
//   [{control, 5000}, {treatment, 10000}]  for 50/50 split
// Returns the variant key.
func BucketUser(experimentID string, userID string, variantWeights []VariantWeight) string {
    // Compute hash: include experiment ID as salt to ensure cross-experiment independence
    hashInput := experimentID + "." + userID
    hashVal := murmur3.Sum32([]byte(hashInput))

    // Map to bucket [0, 10000)
    bucket := int(hashVal % 10000)

    // Find variant by cumulative weight
    for _, vw := range variantWeights {
        if bucket < vw.CumulativeWeight {
            return vw.VariantKey
        }
    }
    // Should not reach here if weights sum to 10000
    return variantWeights[len(variantWeights)-1].VariantKey
}

// ExperimentTrafficGate determines if a user is in the experiment at all.
// traffic_percent: fraction of eligible users in the experiment (e.g., 0.5 for 50%).
// Uses a separate hash to decouple enrollment from variant assignment.
func IsUserInExperiment(experimentID string, userID string, trafficPercent float64) bool {
    hashInput := experimentID + ".traffic." + userID
    hashVal := murmur3.Sum32([]byte(hashInput))
    bucket := int(hashVal % 10000)
    threshold := int(trafficPercent * 100)  // e.g., 50.0% -> 5000
    return bucket < threshold
}

// Full assignment flow
func AssignUserToExperiment(exp Experiment, userID string) *AssignmentResult {
    // Step 1: Check experiment eligibility rules (from feature flag targeting)
    // (This is handled by the feature flag SDK's rule evaluation engine)

    // Step 2: Check if user is in the experiment's traffic allocation
    if !IsUserInExperiment(exp.ID, userID, exp.TrafficPercent) {
        return &AssignmentResult{Enrolled: false}
    }

    // Step 3: Assign to variant
    variant := BucketUser(exp.ID, userID, exp.VariantWeights)
    return &AssignmentResult{
        Enrolled:   true,
        VariantKey: variant,
        ExperimentID: exp.ID,
    }
}
```

**Mutual Exclusion with Layers:**

To prevent users from being in two experiments that could interfere with each other (e.g., two checkout experiments), experiments are assigned to "layers" (also called "namespaces"). Within a layer, traffic is partitioned: Experiment A takes 0-4999, Experiment B takes 5000-9999. A user's layer bucket determines which experiment (if any) they're in.

```go
func BucketUserInLayer(layerID string, userID string) int {
    hashInput := layerID + "." + userID
    return int(murmur3.Sum32([]byte(hashInput)) % 10000)
}

func AssignUserToLayer(layer Layer, userID string) *LayerAssignmentResult {
    bucket := BucketUserInLayer(layer.ID, userID)
    for _, exp := range layer.Experiments {
        if bucket >= exp.LayerStart && bucket < exp.LayerEnd {
            return &LayerAssignmentResult{ExperimentID: exp.ID}
        }
    }
    return nil  // User in holdout (no experiment in this layer)
}
```

**Interviewer Q&A:**

Q: What is the "bucket boundary" problem and how does MurmurHash3 address it?
A: The bucket boundary problem is when a hash function produces a non-uniform distribution, causing some buckets to receive more users than others. This introduces systematic bias — some variants get more users, violating the assumption of equal treatment. MurmurHash3 produces a uniformly distributed 32-bit output. Modulo 10000 on a uniform 32-bit distribution gives each bucket an equal probability of `1/10000`. The bias from modulo on a non-power-of-2 value is `(2^32 mod 10000) / 2^32 = 4296 / 4294967296 ≈ 0.0001%` — negligible.

Q: A user clears cookies or uses a different device. Do they get a different variant?
A: Only if the user_id changes. If you use a stable user ID (authenticated user's UUID), the assignment is device-independent. If you use a cookie-based anonymous ID, cookie deletion creates a new ID and potentially a different variant. This is a known limitation of A/B testing for unauthenticated users. Mitigation: use a device fingerprint or require login for sensitive experiments. Optionally, use CUPED (Controlled-experiment Using Pre-Experiment Data) to reduce noise from user-switching.

Q: How do you handle the case where a new experiment starts while an old experiment is still running on the same feature flag?
A: The feature flag system supports at most one experiment per flag at a time. To run a new experiment, either: (1) Conclude the old experiment and start a new one (the flag's percentage rollout rule is updated). (2) Use a separate flag for the new experiment. Running two concurrent experiments on the same flag would confound results — the same users would be split between two experiments' variant assignments, which could contradict each other. The experiment platform enforces this by rejecting an experiment start if the linked flag already has an active experiment.

Q: How would you handle a 90/10 holdout group across all experiments for long-term impact measurement?
A: Create a permanent "global holdout" layer where 10% of users are always in control (no experiment variants). All other experiments draw from the remaining 90%. Users in the global holdout receive the default behavior for all flags. The global holdout is a superlayer that pre-empts all other layer assignments. This enables measuring the aggregate lift of all experiments vs. a pure control, revealing the overall impact of the experimentation program.

Q: What happens to statistical validity if a user is in 5 experiments simultaneously?
A: If the experiments are in different layers (orthogonal layers), there is no interference by design — layer bucketing hashes are independent. If experiments are in the same layer, the mutual exclusion constraint prevents a user from being in more than one. The primary concern about multiple experiments is interaction effects (Experiment A changes the header, Experiment B changes the checkout button — their combined effect may not equal the sum of individual effects). Layer isolation handles this for known interactions. For unknown interactions, post-hoc interaction analysis (ANOVA with interaction terms) can be run on experiment results.

---

### 6.2 Statistical Significance Testing

**Problem it solves**: Given metric data collected from control and treatment groups, determine whether the observed difference is statistically significant or could be attributed to random chance.

**Approaches Comparison:**

| Method | Best For | Multiple Looks? | Bayesian Prior? | Early Stop? | Complexity |
|---|---|---|---|---|---|
| **Two-sample t-test** | Continuous metrics (mean) | No (p-hacking risk) | No | No (fixed horizon) | Low |
| **Two-proportion z-test** | Binary metrics (conversion rates) | No | No | No | Low |
| **Sequential testing (mSPRT)** | Any metric; valid with multiple looks | Yes | Empirical Bayes | Yes | Medium |
| **Bayesian with Beta-Binomial** | Binary metrics; intuitive interpretation | Yes | Yes | Yes | Medium |
| **Bootstrap methods** | Non-normal distributions; ratio metrics | No (expensive) | No | No | High (CPU) |
| **CUPED (variance reduction)** | Reducing noise with pre-experiment data | No (applies to residuals) | No | Faster (smaller N required) | Medium |

**Selected: Two-sample t-test / z-test for standard metrics (fixed-horizon experiments) + mSPRT for early stopping; CUPED as a preprocessing step for variance reduction.**

Rationale:
- The t-test and z-test are well-understood by product managers and provide interpretable p-values and confidence intervals. For most experiments with a fixed duration (determined by power analysis), these are the correct choice.
- mSPRT (Mixture Sequential Probability Ratio Test) is used for experiments where early stopping is desirable. It controls the Type I error rate even when the test is evaluated repeatedly (solving the "peeking problem" that invalidates standard t-tests with multiple looks).
- CUPED reduces variance (and therefore required sample size) by 20-40% by regressing out pre-experiment user behavior covariates.

**Implementation — Statistical Computation Engine pseudocode:**

```python
import numpy as np
from scipy import stats
from scipy.stats import norm

def compute_experiment_results(experiment: Experiment, events: ExperimentEvents) -> List[MetricResult]:
    results = []
    for em in experiment.experiment_metrics:
        metric = em.metric
        # Step 1: Get per-user metric values per variant
        user_values = compute_per_user_metric(events, metric, experiment)
        # user_values = { "control": [0, 0, 1, 0, 1, ...], "treatment": [0, 1, 1, 0, 1, ...] }

        # Step 2: Apply CUPED variance reduction (if pre-experiment covariate available)
        if metric.cuped_covariate_available:
            user_values = apply_cuped(user_values, events.pre_experiment_covariate)

        control_values = np.array(user_values["control"])
        treatment_values = np.array(user_values["treatment"])

        n_c = len(control_values)
        n_t = len(treatment_values)
        mean_c = np.mean(control_values)
        mean_t = np.mean(treatment_values)
        var_c = np.var(control_values, ddof=1)
        var_t = np.var(treatment_values, ddof=1)

        # Step 3: Compute test statistic
        if metric.metric_type == "binary":
            # Two-proportion z-test
            p_c = mean_c
            p_t = mean_t
            pooled_p = (sum(control_values) + sum(treatment_values)) / (n_c + n_t)
            se = np.sqrt(pooled_p * (1 - pooled_p) * (1/n_c + 1/n_t))
            z_score = (p_t - p_c) / se if se > 0 else 0
            p_value = 2 * (1 - norm.cdf(abs(z_score)))  # two-sided
            test_stat = z_score
        else:
            # Welch's t-test (does not assume equal variances)
            t_stat, p_value = stats.ttest_ind(treatment_values, control_values, equal_var=False)
            test_stat = t_stat

        # Step 4: Compute lift and confidence interval
        lift = (mean_t - mean_c) / mean_c if mean_c != 0 else 0
        # Delta method for CI on relative lift
        se_diff = np.sqrt(var_c / n_c + var_t / n_t)
        z_alpha = norm.ppf(1 - (1 - experiment.confidence_level) / 2)  # e.g., 1.96 for 95%
        ci_lower = (mean_t - mean_c - z_alpha * se_diff) / mean_c if mean_c != 0 else 0
        ci_upper = (mean_t - mean_c + z_alpha * se_diff) / mean_c if mean_c != 0 else 0

        # Step 5: Bonferroni correction for multiple primary metrics
        alpha = 1 - experiment.confidence_level  # e.g., 0.05
        num_primary_metrics = sum(1 for m in experiment.experiment_metrics if m.is_primary)
        adjusted_alpha = alpha / max(num_primary_metrics, 1)  # Bonferroni
        is_significant = p_value < adjusted_alpha

        # Step 6: Sequential test statistic (mSPRT) for early stopping evaluation
        sequential_stat = compute_msprt_statistic(
            n_c, mean_c, var_c, n_t, mean_t, var_t,
            rho=0.5  # mixing parameter; controls the mSPRT boundary shape
        )
        # mSPRT rejects H0 if sequential_stat > 1/alpha
        can_stop_early = sequential_stat > (1 / alpha)

        results.append(MetricResult(
            metric_id=metric.id,
            n_control=n_c, mean_control=mean_c,
            n_treatment=n_t, mean_treatment=mean_t,
            lift=lift, lift_ci=(ci_lower, ci_upper),
            p_value=p_value, test_statistic=test_stat,
            is_significant=is_significant,
            sequential_stat=sequential_stat,
            can_stop_early=can_stop_early
        ))
    return results

def compute_msprt_statistic(n_c, mu_c, var_c, n_t, mu_t, var_t, rho: float) -> float:
    """
    Mixture Sequential Probability Ratio Test (mSPRT).
    Computes the test martingale value. Values > 1/alpha allow early stopping.
    Based on: Johari et al. (2017) "Peeking at A/B Tests"
    """
    # Pooled variance
    sigma_sq = ((n_c - 1) * var_c + (n_t - 1) * var_t) / (n_c + n_t - 2)
    if sigma_sq <= 0:
        return 1.0

    # Harmonic mean of sample sizes
    n_harmonic = 2 / (1/n_c + 1/n_t)

    # mSPRT log-mixture statistic
    # rho is the mixing parameter of the normal mixing distribution
    tau_sq = rho / n_harmonic  # variance of mixing prior
    delta_hat = mu_t - mu_c
    V = sigma_sq * (1/n_c + 1/n_t)  # variance of delta_hat

    # Log-likelihood ratio under the mixture prior
    log_stat = (0.5 * np.log(V / (V + tau_sq)) +
                (tau_sq * delta_hat**2) / (2 * V * (V + tau_sq)))
    return np.exp(log_stat)

def apply_cuped(user_values: Dict[str, List[float]], pre_experiment_covariate: Dict[str, float]) -> Dict[str, List[float]]:
    """
    CUPED: Subtract the linear regression component on pre-experiment covariate.
    Y_cuped = Y - theta * X, where theta = Cov(Y, X) / Var(X)
    """
    all_users = list(pre_experiment_covariate.keys())
    X = np.array([pre_experiment_covariate.get(u, 0) for u in all_users])
    Y = np.array([/* get user's metric value */ for u in all_users])

    theta = np.cov(Y, X)[0][1] / np.var(X) if np.var(X) > 0 else 0
    mean_X = np.mean(X)

    adjusted = {}
    for variant, values in user_values.items():
        variant_users = [/* map to users */]
        X_v = np.array([pre_experiment_covariate.get(u, mean_X) for u in variant_users])
        Y_v = np.array(values)
        adjusted[variant] = (Y_v - theta * (X_v - mean_X)).tolist()
    return adjusted
```

**Interviewer Q&A:**

Q: What is the "peeking problem" and how does mSPRT solve it?
A: The peeking problem (also called "p-hacking by optional stopping"): if you run a t-test every day and stop the experiment whenever p < 0.05, the true Type I error rate is much higher than 5% (simulations show it can reach 25%+ if you check 100 times). This is because the p-value fluctuates over time; random streaks cause temporary false significance. mSPRT (and other sequential testing methods) maintain valid Type I error control for any number of looks by computing a test martingale that only exceeds the stopping boundary with probability ≤ α, regardless of when or how often you look. The mSPRT statistic is the ratio of two likelihoods (null vs. mixture prior on effect size), and by Ville's inequality, the probability that a martingale ever exceeds 1/α when the null is true is ≤ α.

Q: How do you handle the "winner's curse" (overestimating the effect size of the winning variant)?
A: The winner's curse occurs because experiments that are concluded when p < α are selected for significance, and by random chance, the observed effect size at the stopping point is larger than the true effect size. Mitigations: (1) Run experiments to their pre-specified sample size (fixed-horizon design) and don't peek. (2) Apply empirical Bayes shrinkage to effect size estimates: `shrunk_lift = lift × (n / (n + n_prior))` where `n_prior` is chosen from historical experiments. (3) Use a holdback experiment after shipping to re-measure the treatment effect without selection bias.

Q: How do you compute statistical results when metric distribution is highly skewed (e.g., revenue)?
A: Revenue distributions are typically right-skewed with heavy tails (a few large orders dominate). Options: (1) Log-transform revenue and run t-test on log(revenue). (2) Use a non-parametric test (Mann-Whitney U test) — more robust to outliers but tests a different hypothesis (distribution stochastic dominance, not mean difference). (3) Winsorize the data (cap values at the 99th percentile) before computing the t-test. (4) Bootstrap confidence intervals (resampling with replacement, 10,000 iterations) for the mean — accurate for any distribution but computationally expensive. At 150M events, bootstrap is infeasible. Recommendation: log-transform for revenue + delta method CI + Winsorizing at 99.5th percentile.

Q: What is the minimum detectable effect (MDE) and how does it determine sample size?
A: MDE is the smallest effect size that the experiment is designed to detect with the specified statistical power (probability of detecting a true effect). For a binary metric with baseline rate p₀: `MDE = δ` such that `n = (z_α/2 + z_β)² × (p₀(1-p₀) + p₁(1-p₁)) / δ²` where `p₁ = p₀ + δ`. For `p₀ = 0.042`, `δ = 0.002` (5% relative MDE = 0.042 × 0.05), `α = 0.05`, `β = 0.20`: `n ≈ (1.96 + 0.84)² × (0.042×0.958 + 0.044×0.956) / 0.002² ≈ 7.84 × 0.0846 / 0.000004 ≈ 165,000` per variant. The MDE is a trade-off: smaller MDE (detect smaller effects) requires more users.

Q: How do you prevent false positives from analyzing too many metrics (multiple comparisons problem)?
A: Apply Bonferroni correction: if there are K primary metrics, the significance threshold for each is `α/K`. For K=5 metrics at α=0.05, each metric requires p < 0.01. A more powerful alternative is the Benjamini-Hochberg procedure (FDR control), which is less conservative than Bonferroni while still controlling the expected proportion of false discoveries. The platform enforces designating exactly one metric as the primary metric; Bonferroni applies to all secondary metrics. Guardrail metrics use one-sided tests with their own α threshold.

---

### 6.3 Event Ingestion Pipeline

**Problem it solves**: Ingest 9,550 events/sec peak durably, deduplicate events (SDK may retry on network failure), associate each event with the correct experiment and variant, and make events available to the Statistical Computation Engine within 5 minutes.

**Approaches Comparison:**

| Approach | Throughput | Latency | Deduplication | Complexity |
|---|---|---|---|---|
| **Direct write to ClickHouse** | Good (ClickHouse handles high-throughput inserts natively) | ~100ms to durable | Requires ReplacingMergeTree | Low |
| **Kafka → ClickHouse (Kafka consumer)** | Excellent (Kafka decouples bursts) | ~5s to ClickHouse | Dedup at consumer or ReplacingMergeTree | Medium |
| **Kafka → Flink → ClickHouse** | Excellent | ~5-30s (Flink processing time) | Stateful dedup in Flink | High |
| **Kafka → S3 → scheduled load** | Excellent | ~5-60 min (batch) | Dedup at load time | Medium |

**Selected: Direct batched write to Kafka from Ingest API → Kafka Consumer → ClickHouse (no intermediate Flink)**

Rationale: Flink adds operational complexity (state management, checkpointing, cluster management) without being necessary at 9,550 events/sec. ClickHouse's `ReplacingMergeTree` engine handles deduplication server-side. The Kafka consumer reads micro-batches (every 5 seconds) and does a bulk insert to ClickHouse.

**Implementation — Event Ingest pipeline:**

```python
class EventIngestAPI:
    async def ingest_events(self, events: List[Event], sdk_key: str) -> IngestResponse:
        org_id = authenticate_sdk_key(sdk_key)

        # Step 1: Validate events
        valid_events = []
        for e in events:
            if not validate_event_schema(e):
                continue
            # Enrich: add org_id and server-side timestamp
            e.org_id = org_id
            e.server_timestamp = datetime.utcnow()
            valid_events.append(e)

        if not valid_events:
            return IngestResponse(accepted=0, rejected=len(events))

        # Step 2: Dedup check via Redis bloom filter (probabilistic, fast)
        # Event ID = SHA-256(experiment_id + user_id + event_type + client_timestamp_ms)
        new_events = []
        for e in valid_events:
            event_id = compute_event_id(e)
            e.event_id = event_id
            # bloom.add returns True if item was already present
            if not await redis_bloom.add(f"event_dedup:{org_id}", event_id):
                new_events.append(e)
            # else: duplicate, skip silently

        # Step 3: Publish to Kafka
        kafka_batch = [
            kafka_producer.produce(
                topic="exp-events",
                key=e.user_id.encode(),    # partition by user_id for ordering
                value=serialize_msgpack(e),
                headers={"org_id": org_id}
            )
            for e in new_events
        ]
        await asyncio.gather(*kafka_batch)

        return IngestResponse(accepted=len(new_events), rejected=len(events) - len(valid_events))

class ClickHouseConsumer:
    """Kafka consumer that batch-writes events to ClickHouse every 5 seconds."""

    BATCH_SIZE = 10000
    FLUSH_INTERVAL_SECONDS = 5

    def __init__(self):
        self.exposure_buffer = []
        self.event_buffer = []
        self.last_flush = time.time()

    async def consume(self):
        async for msg in kafka_consumer("exp-events"):
            event = deserialize_msgpack(msg.value)
            if event.type == "exposure":
                self.exposure_buffer.append(self._to_exposure_row(event))
            else:
                self.event_buffer.append(self._to_event_row(event))

            if (len(self.exposure_buffer) >= self.BATCH_SIZE or
                len(self.event_buffer) >= self.BATCH_SIZE or
                time.time() - self.last_flush >= self.FLUSH_INTERVAL_SECONDS):
                await self.flush()

    async def flush(self):
        if self.exposure_buffer:
            await clickhouse.execute(
                "INSERT INTO experiment_exposures VALUES",
                self.exposure_buffer
            )
            self.exposure_buffer.clear()
        if self.event_buffer:
            await clickhouse.execute(
                "INSERT INTO experiment_events VALUES",
                self.event_buffer
            )
            self.event_buffer.clear()
        self.last_flush = time.time()
```

**Interviewer Q&A:**

Q: How do you handle event ordering issues? A conversion event may arrive before the exposure event (due to client-side race conditions).
A: The Statistical Computation Engine enforces temporal ordering at query time. When computing per-user metric values, only conversion events with `occurred_at >= user's first_exposure_at` for that experiment are included. The `experiment_exposures_deduped` table stores `first_exposure` per user × experiment. Conversions before first exposure are ignored. This is a join condition: `JOIN experiment_exposures_deduped e ON ev.user_id = e.user_id AND ev.experiment_id = e.experiment_id AND ev.occurred_at >= e.first_exposure`. Out-of-order events that arrive in Kafka are handled because ClickHouse writes are idempotent and the query always reads the full dataset up to the computation window.

Q: How does the bloom filter prevent duplicate event counting without becoming a bottleneck?
A: Redis bloom filter (`BF.ADD` command from RedisBloom module) has O(1) time complexity regardless of the number of items. At 9,550 events/sec, each event requires one `BF.ADD` call (1-2ms latency per call, but these are issued concurrently). With `pipeline()` batching (issue all `BF.ADD` calls in a single Redis pipeline per batch), the overhead is the single round-trip for the entire batch regardless of batch size. The bloom filter is configured with error rate 0.1% and estimated capacity of 100M items/day per org. A small false positive rate (0.1%) means 0.1% of new events are incorrectly flagged as duplicates — acceptable. Bloom filter is reset daily.

Q: What happens to events if the ClickHouse consumer crashes mid-batch?
A: Kafka provides at-least-once delivery. The consumer commits offsets only after a successful ClickHouse insert (`asyncio.gather(*[flush()])`). If the consumer crashes before committing, Kafka redelivers the batch to the new consumer instance. The ClickHouse `ReplacingMergeTree` engine deduplicates based on the `ORDER BY` key — duplicate exposure rows are merged during background merges. For the `experiment_events` table (non-deduplicated), duplicate events mean a user's metric value is counted twice. Mitigation: the bloom filter in the Ingest API already filtered known duplicates; only retry-caused duplicates reach ClickHouse. For the computation query, use `countDistinct(user_id)` for exposure counts and deduplicate metric events by event_id if event_id is included in the schema.

---

## 7. Scaling

### Horizontal Scaling

- **Event Ingest API**: Stateless; scale based on peak throughput. At 9,550 events/sec, 10ms average handling time, need ~96 concurrent threads = ~5 server instances (20 threads each). Deploy 10 for headroom.
- **Kafka**: 6 brokers, 12 partitions on `exp-events`. Partitioned by `user_id` ensures all events for a user are in the same partition (important for ordering and session-level metrics). At 9,550 events/sec × 250 B = ~2.4 MB/s — easily handled by a single Kafka broker; 6 brokers provide fault tolerance.
- **ClickHouse Consumer**: One consumer instance per Kafka partition = 12 consumers. Each consumer inserts into ClickHouse independently; ClickHouse handles concurrent inserts via its insert queue. Scale consumers by adding partitions.
- **ClickHouse Cluster**: 3 shards × 2 replicas = 6 nodes. Sharded by `org_id` (hash). Each node handles ~55M events/day. ClickHouse can handle 1B+ rows/day per node; this is well within capacity.
- **Statistical Computation Engine**: CPU-bound. Each computation job for one experiment takes ~30 seconds (ClickHouse query: 5s + t-test computation: 25s). With 50 running experiments computed every 5 minutes, need 50 × 30s / 300s = 5 parallel workers. Scale to 20 workers for headroom and to handle computation bursts at experiment start.

### DB Sharding

- **PostgreSQL (experiment metadata)**: Shard by `org_id`. Experiment metadata is small (< 1 GB per org); sharding is for write isolation, not data volume. A single Postgres instance can handle all orgs at this scale.
- **ClickHouse (events)**: Sharded by `org_id`. Cross-shard queries needed for platform-wide analytics are handled by a distributed table on top of the sharded tables.

### Caching

| Data | Cache | TTL | Note |
|---|---|---|---|
| Pre-computed experiment results | PostgreSQL (already indexed) | N/A | Results are written every 5 min; reads are fast (< 100ms) |
| Experiment configuration (for ingest validation) | In-memory LRU (per ingest server) | 5 min | Avoids DB lookup on every event ingest |
| SDK key → org_id mapping | Redis | 10 min | Auth lookup on every ingest request |
| Power calculation results | Redis | 24 hours | Expensive computation; same inputs → same result |
| Active experiment list per org | Redis | 60 s | Used by ingest to validate experiment_ids |

**Interviewer Q&As:**

Q: ClickHouse background merges on `ReplacingMergeTree` are eventually consistent. How does this affect real-time results?
A: ClickHouse merges happen asynchronously. A query on `experiment_exposures_deduped` may see duplicate rows before merges complete. Solution: use `FINAL` modifier in queries: `SELECT * FROM experiment_exposures_deduped FINAL`. This applies deduplication at query time, making the results consistent but adding query latency (~2x). For the 5-minute computation window, this overhead is acceptable. Alternatively, use `SELECT ... GROUP BY ... HAVING count() = 1` to manually enforce deduplication — faster but more complex SQL.

Q: How do you scale the Statistical Computation Engine to handle 1,000 simultaneous experiments?
A: The scheduler dispatches computation jobs to a worker pool via a Kafka topic `computation-jobs`. Each computation job is independent. With 1,000 experiments × 30s per computation = 30,000 CPU-seconds per computation cycle. A 5-minute cycle has 300 seconds. Need 100 workers running at 100% utilization. Deploy 150 worker instances (Go, Python, or Java processes). Jobs are dispatched by a scheduler that tracks which experiments are due for computation. Experiments with more enrolled users (higher variance in p-value) are prioritized.

Q: How do you handle a ClickHouse shard failure during a computation query?
A: The Statistical Computation Engine's ClickHouse query uses a Distributed table that queries all shards. If a shard is unavailable, the query fails. For fault tolerance: (1) Each shard has a replica (ReplicatedMergeTree). The Distributed table automatically routes to the replica if the primary is unavailable. (2) For non-critical computations, allow the query to proceed with the available shards and note the missing shard in the results (partial results). (3) For critical p-value computation, fail the job and retry after 60 seconds.

Q: How do you handle the skew problem where one experiment has 50% of all traffic (one very large experiment)?
A: Large experiments mean one ClickHouse partition (by org_id) has disproportionate data volume if the large experiment's org is on one shard. Mitigate with sub-sharding: for experiments with > 10M enrollments, create a dedicated ClickHouse shard for that experiment (keyed by experiment_id). The Distributed table routes queries to the correct shard. This requires detecting large experiments at creation time (via power calculation: n > 10M is a signal) and pre-allocating dedicated infrastructure.

Q: What is the data retention policy and how do you enforce it?
A: ClickHouse `TTL` clause on the event tables: `TTL exposed_at + INTERVAL 180 DAY DELETE`. ClickHouse automatically deletes rows older than 180 days during background merges. For compliance, certain experiments may need longer retention (7 years for some regulated industries). In that case, use a separate ClickHouse cluster (or S3-backed cold storage via ClickHouse's `S3` table engine) for long-retention orgs. Postgres experiment metadata is retained indefinitely (small). Computed results are retained for 2 years.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Recovery |
|---|---|---|---|
| Event Ingest API | Instance crash | ALB health check | Other instances serve; Kafka acts as buffer |
| Kafka broker | Crash | Controller leader election | Partition rebalance; < 10s; no event loss if RF ≥ 3 |
| ClickHouse consumer | Crash | Kubernetes liveness probe | Restarts; re-reads uncommitted Kafka offset; ClickHouse deduplication handles duplicate inserts |
| ClickHouse node | Crash | ClickHouse built-in monitoring | Replica serves reads; shard rebuilds from replica |
| Statistical Computation Engine | Crash mid-computation | Kubernetes restart | Job re-runs from scratch (idempotent: overwrites results in Postgres) |
| Postgres (results store) | Primary crash | Patroni failover | < 30s failover; computation retries write |
| Redis (dedup bloom filter) | Node failure | Redis Sentinel | Duplicate events may get through during failover; counted once in ClickHouse via ReplacingMergeTree |
| Guardrail Monitor | False trigger | Alert with manual override | Experiment paused; human reviews and resumes; false trigger rate < 0.1% with correct threshold setting |

### Idempotency

- **Event Ingest**: Events carry a client-generated `event_id` (UUID or deterministic hash). The bloom filter deduplicates at ingest. ClickHouse `ReplacingMergeTree` deduplicates at storage. Duplicate events in ClickHouse before merge are handled by `FINAL` queries.
- **Computation**: Results are written with `INSERT ... ON CONFLICT (experiment_id, metric_id, computed_at) DO UPDATE SET ...` in Postgres. Re-running a computation for the same 5-minute window overwrites the previous result — idempotent.
- **Experiment Lifecycle**: Experiment state transitions are protected by Postgres row-level locks. `UPDATE experiments SET status='running' WHERE id=? AND status='draft'` — the `WHERE status='draft'` condition prevents double-starting.

### Circuit Breaker

- **ClickHouse write circuit breaker**: If ClickHouse insert latency > 10s or error rate > 10% over 30 seconds, the ClickHouse consumer trips the circuit breaker. Events are buffered in Kafka (Kafka has a 7-day retention). The consumer retries every 30 seconds. During the outage, no new events reach ClickHouse, but all events are durably stored in Kafka.
- **Guardrail auto-stop**: The Guardrail Monitor circuit breaker: if a guardrail metric exceeds its threshold in 2 consecutive 5-minute computation windows, the experiment is automatically paused. A single-window violation could be a transient anomaly; two consecutive violations indicate a real issue.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `event.ingest.throughput` | Counter rate | < 1,000/s (unexpected drop) | Event pipeline health |
| `event.ingest.error_rate` | Counter rate | > 0.5% | Ingest API errors |
| `event.kafka.consumer_lag` | Gauge | > 500,000 | ClickHouse consumer falling behind |
| `event.clickhouse.insert_latency_p99` | Histogram | > 10s | ClickHouse write performance |
| `computation.job.duration_p99` | Histogram | > 5 min | Computation SLA |
| `computation.job.failure_rate` | Counter rate | > 5% | Statistical computation errors |
| `guardrail.triggered.count` | Counter | > 0 | Any guardrail trigger is an alert |
| `experiment.novelty_effect_detected` | Gauge | — | Day-1 vs. day-7 conversion rate comparison |
| `event.dedup.bloom_false_positive_rate` | Gauge | > 1% | Bloom filter saturation |
| `clickhouse.replication.delay_seconds` | Gauge | > 60s | Replica health |
| `results.freshness_seconds` | Gauge | > 300s | Time since last successful computation |

### Distributed Tracing

Every event batch submission generates a trace: Ingest API → Kafka publish → Consumer Kafka read → ClickHouse insert. The `computed_at` timestamp in experiment results is compared against the `ingestion_completed_at` timestamp (stored in the Kafka message header) to compute end-to-end data freshness lag.

### Logging

- **Event Ingest API**: Log per-batch metrics (batch size, validation failures, dedup rate, publish latency) at INFO. Log individual validation failures at DEBUG. Log Kafka publish failures at ERROR with retry context.
- **Computation Engine**: Log per-experiment computation start/end, metric results summary (p-value, is_significant), computation duration. Log ClickHouse query plan on slow queries (> 10s).
- **Guardrail Monitor**: Log every guardrail check result (experiment_id, metric, measured_value, threshold, status). This creates an audit trail of why an experiment was stopped.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Event storage | ClickHouse (OLAP columnar) | PostgreSQL | Postgres row-oriented storage is 100x slower for aggregation queries over 165M events/day; ClickHouse vectorized execution handles this in seconds |
| Statistical test | Welch's t-test + mSPRT | Fixed-sample z-test only | mSPRT allows valid early stopping; Welch's t-test is more robust (doesn't assume equal variances) |
| Bucketing | Hash-based (in-process) | DB-stored assignments | Hash-based is zero-latency, zero-dependency; DB assignments require a lookup on every request (at 1B assignments/day, impractical) |
| Event deduplication | Bloom filter (probabilistic) + ReplacingMergeTree | Exact dedup (Redis SET) | Redis SET for 100M events/day would require hundreds of GB of memory; bloom filter is 99.9% accurate with < 1 MB memory |
| Computation frequency | Every 5 minutes | Real-time streaming (Flink) | Real-time requires Flink cluster (high operational overhead); 5-minute batch gives results within the dashboard SLA without the operational complexity |
| Variance reduction | CUPED | No variance reduction | CUPED reduces required sample size by 20-40%, shortening experiment duration significantly |
| Multiple comparisons | Bonferroni (conservative) | No correction | Without correction, 5 metrics × α=0.05 = expected 0.25 false discoveries per experiment — unacceptable |
| Cross-experiment isolation | Layers (mutual exclusion) | No isolation | Without layers, two experiments on the same surface can interact, confounding both results |
| Guardrail stopping | Two consecutive violations | Single violation | Single violations have a high false positive rate for transient metric dips; two consecutive windows is more reliable |

---

## 11. Follow-up Interview Questions

**Q1: How do you detect and handle novelty effects in A/B tests?**
A: Novelty effect: users interact with a new variant more simply because it's different (novelty), not because it's better. This inflates treatment metrics in the first few days. Detection: compare the treatment lift in the first 48 hours vs. days 7-14. If day-1 lift >> day-7 lift, novelty effect is likely. Handling: (1) Run the experiment until the novelty effect washes out (day-14 results are the reliable signal). (2) Use a "new users only" analysis: if novelty effect is artificial inflation, users who are new to the feature shouldn't show it (they have no prior baseline to contrast with). (3) Report both the initial and steady-state lift in the results dashboard.

**Q2: How would you implement Bayesian A/B testing?**
A: Use a Beta-Binomial model for binary metrics. Prior: `Beta(α=1, β=1)` (uninformative). Posterior for control after `n_c` exposures and `k_c` conversions: `Beta(α + k_c, β + n_c - k_c)`. Similarly for treatment. Probability that treatment > control: `P(p_t > p_c) = ∫₀¹ P(p_t > p_c | p_t) dp_t` — computed via Monte Carlo sampling (10,000 samples from each posterior) or analytically. The "probability to be best" (PBB) is intuitive for product managers ("treatment is 93% likely to be better than control"). Bayesian tests also provide a natural "expected loss" framework for decision-making under uncertainty.

**Q3: How do you run an A/B test when you have very little traffic (e.g., a new product with 10,000 DAU)?**
A: With 10,000 DAU and a conversion rate of 5%, detecting a 5% relative lift requires `n ≈ 165,000` per variant — over 16 days at 50% traffic each. Options: (1) Accept a larger MDE (detect only large effects). (2) Increase traffic allocation to 100% (no holdout). (3) Use CUPED to reduce variance and required sample size. (4) Use a longer measurement window (capture 30-day retention instead of single-session conversion). (5) Use Bayesian testing — Bayesian methods can provide useful information with smaller samples than frequentist tests. (6) Consider multivariate testing with careful power analysis if changes are multiple independent factors.

**Q4: How do you handle experiments that affect each other (SUTVA violation — Stable Unit Treatment Value Assumption)?**
A: SUTVA violation occurs when one user's treatment affects another user's experience (e.g., social network experiments: treating user A with a new feed algorithm causes them to post more, which changes user B's experience even if B is in the control group). Mitigation: (1) Cluster randomization: randomize at the group level (e.g., friend groups, geographic areas) rather than the individual level. (2) Ego network analysis: ensure treated and control users have minimal network overlap. (3) Use "global holdout" style analysis to measure the spillover effect.

**Q5: How would you design the results dashboard to prevent premature experiment conclusions?**
A: The dashboard implements "mandatory blindness" until the required sample size is reached: during the sample accumulation phase, show only the sample size progress (e.g., "7,200 / 15,420 users enrolled") but not the p-value or lift. Only after reaching the required sample size (or the experiment end date) are full statistical results shown. This prevents teams from concluding experiments based on "it looks significant" before achieving adequate power. For organizations that prefer peeking (with valid sequential testing), the mSPRT statistic and its boundary are shown, making the sequential test boundary explicit.

**Q6: How do you handle metric definitions that change mid-experiment?**
A: Metric definitions (event type, aggregation method) are frozen at experiment start. The experiment references a specific `metric_id` with a specific definition. If the metric definition needs to change, a new metric version is created; the experiment continues with the original metric. Metric definition changes mid-experiment would make the before and after data incomparable, invalidating the statistical test. If the change is critical, the experiment must be concluded and restarted with the new metric.

**Q7: What is CUPED and why does it help?**
A: CUPED (Controlled-experiment Using Pre-Experiment Data) is a variance reduction technique. It subtracts a linear regression component on a pre-experiment covariate (typically the user's baseline metric value before the experiment) from the observed metric. `Y_cuped = Y - θ × X` where `X` is the pre-experiment covariate and `θ = Cov(Y, X) / Var(X)`. This reduces the variance of Y_cuped vs. Y, because X explains part of Y's variance. Lower variance → tighter confidence intervals → smaller required sample size. Typical variance reduction: 20-40%, meaning experiments run 20-40% faster. Requirement: you need historical baseline data for users before the experiment. Best covariate: the same metric measured in the pre-experiment window (e.g., if the metric is weekly revenue, the covariate is the previous week's revenue for the same user).

**Q8: How do you handle an experiment where some users switch variants mid-experiment (contamination)?**
A: Hash-based bucketing makes variant switching impossible as long as the user_id and experiment_id are stable. However, contamination can occur if: (1) A user has multiple user_ids (logged in vs. anonymous). Solution: use a single stable ID (authenticated user ID for logged-in users). (2) A developer changes the flag's rollout seed mid-experiment. Solution: the platform prevents rollout seed changes on active experiments. (3) A user is forcibly reassigned via the override feature. Solution: track override assignments and exclude those users from statistical analysis (intent-to-treat vs. per-protocol analysis).

**Q9: How do you compute experiment results for ratio metrics (e.g., revenue per user who started checkout)?**
A: Ratio metrics have two components: numerator (e.g., users who completed checkout) and denominator (e.g., users who started checkout). Simple division of group totals is biased (ratio of averages ≠ average of ratios). Use the delta method to compute the variance of the ratio: `Var(R) ≈ (1/μ_D)² × Var(N) + (μ_N/μ_D²)² × Var(D) - 2 × (μ_N/μ_D³) × Cov(N, D)` where N is the numerator, D is the denominator, and μ denotes the mean. This gives a valid confidence interval for the ratio difference between variants.

**Q10: How would you design a self-serve experiment platform that non-data-scientists can use?**
A: Simplify the UX: (1) Auto-suggest an MDE based on historical metric variance and current traffic (power calculator pre-filled with defaults). (2) Show a plain-English interpretation: "With 15,420 users per variant (est. 12 days), you can detect a 5% or larger improvement in checkout conversion." (3) Replace p-value with a "confidence level" percentage bar and "probability of being the best" (Bayesian). (4) Provide a guardrail auto-stop with sensible defaults (error rate guardrail at 2x baseline). (5) Use a 3-step wizard (Name → Define variants → Pick a metric) rather than a single complex form. (6) Post-experiment summary: auto-generate a plain-English paragraph: "The treatment increased checkout conversion by 17% (p=0.002). We are 99.8% confident the treatment is better. Recommended action: ship the treatment."

**Q11: How do you detect Sample Ratio Mismatch (SRM)?**
A: SRM occurs when the actual traffic split between variants differs significantly from the intended split (e.g., 50/50 intended but 53/47 observed). This indicates a bug in the assignment or logging pipeline and invalidates statistical results. Detection: run a chi-squared goodness-of-fit test on the observed vs. expected exposure counts. `χ² = Σ (observed_i - expected_i)² / expected_i`. If p < 0.01, flag the experiment as having an SRM. Common causes: (1) Bot traffic filtered in one variant but not the other. (2) Cache serving old variant to some users. (3) Logging bug losing events for one variant. The Computation Engine checks for SRM on every computation and adds a warning to results if detected.

**Q12: How would you design the system to handle holdback experiments (a permanent 1-10% control group)?**
A: A holdback experiment is a long-running experiment where the treatment is the "shipped" product and the control is the old product (held back for a cohort of users). Implementation: create a permanent "holdback" layer (or use an existing layer slot). Assign 5% of users to this layer's holdback slot. For those users, the feature flag service always returns the control variant for every experiment in the holdback layer. This enables measuring the cumulative effect of all shipped features over time. The holdback experiment runs continuously; results are computed weekly to show the overall lift of the product vs. the holdback baseline.

**Q13: How do you ensure that your experimentation platform itself doesn't introduce bias?**
A: A/A testing: run a "null experiment" where both variants receive identical treatment. Statistical results should show no significant differences across metrics (p-values uniformly distributed over [0,1]). If A/A tests consistently show significant results for some metrics, the platform has a bug (implementation bias, misconfigured hash function, logging error). Run A/A tests quarterly on the platform itself as a calibration check. Also: validate the hash function produces equal bucket sizes across 1M simulated users (chi-squared test on bucket distribution).

**Q14: How would you handle timezone-dependent metrics (e.g., experiments that measure "next-day retention")?**
A: Next-day retention requires attributing a day-2 event to the user's day-1 exposure, accounting for the user's local timezone. Implementation: store the user's timezone in the exposure event attributes. The metric definition specifies a `win_horizon_hours: 24` in the user's timezone. The computation query converts timestamps: `CONVERT_TZ(occurred_at, 'UTC', user_timezone) BETWEEN day1_local AND day2_local`. For users without a known timezone, use UTC (safe default). Timezone-aware metrics are computed with a larger query complexity but the same SQL framework.

**Q15: What are the biggest threats to experiment validity and how does the platform mitigate each?**
A: (1) **Peeking / multiple testing**: mSPRT for valid early stopping; Bonferroni for multiple metrics. (2) **SRM (Sample Ratio Mismatch)**: automatic chi-squared SRM check on every computation window. (3) **Novelty effect**: detect via day-1 vs. day-14 lift comparison; warn when detected. (4) **SUTVA violation**: layer isolation for experiments that could interfere; alert when two experiments with overlapping user cohorts are in the same layer. (5) **Carryover effects** (experiment ends but variant still affects behavior): use a washout period after experiment conclusion before starting a new experiment on the same surface. (6) **Selection bias from feature gating** (experiment only eligible for users who triggered a specific event): adjust for eligibility bias using propensity score matching or ITT analysis. (7) **Instrument pollution** (bugs in event logging): A/A testing, SRM checks, event count anomaly detection.

---

## 12. References & Further Reading

- **Johari, R., Koomen, P., Pekelis, L., & Walsh, D. (2017). Peeking at A/B Tests: Why It Matters, and What to Do About It.** *Proceedings of the 23rd ACM SIGKDD International Conference on Knowledge Discovery and Data Mining*. — Foundational paper on mSPRT and valid sequential testing.
- **Deng, A., Xu, Y., Kohavi, R., & Walker, T. (2013). Improving the Sensitivity of Online Controlled Experiments by Utilizing Pre-Experiment Data.** *Proceedings of the 6th ACM WSDM Conference*. — Original CUPED paper.
- **Kohavi, R., Tang, D., & Xu, Y. (2020). *Trustworthy Online Controlled Experiments: A Practical Guide to A/B Testing*. Cambridge University Press.** — Comprehensive reference for all aspects of experimentation at scale.
- **Bakshy, E., Eckles, D., & Bernstein, M. S. (2014). Designing and Deploying Online Field Experiments.** *Proceedings of WWW 2014*. — Facebook's large-scale experimentation platform design.
- **Tang, D., Agarwal, A., O'Brien, D., & Meyer, M. (2010). Overlapping Experiment Infrastructure: More, Better, Faster Experimentation.** *Proceedings of the 16th ACM SIGKDD International Conference*. — Google's paper on layers/namespaces for running thousands of simultaneous experiments.
- **Zhao, Z., Chen, W., & Jiang, C. (2019). How to Measure Ad Load Cannibalization with Holdback Experiments.** *Proceedings of KDD 2019*. — Holdback experiment design.
- **MurmurHash3 — Austin Appleby**: https://github.com/aappleby/smhasher — Hash function used for bucketing.
- **ClickHouse Documentation — MergeTree Engine Family**: https://clickhouse.com/docs/en/engines/table-engines/mergetree-family/
- **ClickHouse Documentation — ReplacingMergeTree**: https://clickhouse.com/docs/en/engines/table-engines/mergetree-family/replacingmergetree
- **Netflix Tech Blog — Experimentation Platform**: https://netflixtechblog.com/experimentation-is-a-major-focus-of-data-science-across-netflix-f67923f8e985
- **Airbnb Engineering — Experiment Reporting Framework**: https://medium.com/airbnb-engineering/experiment-reporting-framework-4e3fcd29e6c0
- **Booking.com — The Anatomy of a Large-Scale Experimentation Platform**: https://booking.ai/the-anatomy-of-a-large-scale-experimentation-platform-at-booking-com-7e05d5f4b2d6
- **Evan Miller — How Not To Run an A/B Test** (peeking problem): https://www.evanmiller.org/how-not-to-run-an-ab-test.html
- **Statsig Engineering Blog — CUPED in Practice**: https://www.statsig.com/blog/cuped
- **Apache Kafka Documentation**: https://kafka.apache.org/documentation/
- **Benjamini, Y., & Hochberg, Y. (1995). Controlling the False Discovery Rate: A Practical and Powerful Approach to Multiple Testing.** *Journal of the Royal Statistical Society: Series B (Methodological)*, 57(1), 289–300.
