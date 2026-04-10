# System Design: Recommendation Engine

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Personalized recommendations**: Return a ranked list of items (products, videos, articles, users) tailored to the requesting user based on their interaction history.
2. **Real-time and near-real-time updates**: Incorporate recent interactions (clicks, purchases, watches, likes) into recommendation signals within minutes.
3. **Multi-surface serving**: Serve recommendations to homepage, search sidebars, email digests, push notifications, and in-app feed widgets via a unified API.
4. **Cold-start handling**: Return meaningful recommendations for brand-new users (no history) and brand-new items (no interactions).
5. **Diversity controls**: Expose knobs for operators to control the fraction of recommendations from underexplored categories to prevent filter bubbles.
6. **A/B experimentation**: Route a configurable percentage of traffic to experimental ranking models or feature sets without code deployments.
7. **Feedback ingestion**: Accept explicit feedback (thumbs up/down, ratings) and implicit feedback (click, dwell time, scroll depth, share, purchase) from clients.
8. **Filtering and business rules**: Honor hard constraints: age-restricted content filtering, already-purchased item suppression, blocked users, geo-restrictions.

### Non-Functional Requirements

1. **Latency**: P99 recommendation response < 150 ms at the API gateway; model inference < 30 ms; candidate retrieval < 20 ms.
2. **Throughput**: Sustain 500,000 recommendation requests per second (RPS) at peak (e.g., prime-time for a streaming platform).
3. **Availability**: 99.99% uptime (< 52.6 minutes downtime/year). Recommendations fall back to popularity-based ranking if ML models are unavailable.
4. **Consistency**: Eventual consistency is acceptable. A user's last action may take up to 5 minutes to influence their recommendations.
5. **Scalability**: Horizontal scale-out without re-architecture from 10 M to 1 B users, and from 1 M to 1 B items.
6. **Freshness**: Model retrain cycle at most every 24 hours for batch models; streaming feature updates within 5 minutes.
7. **Privacy**: No PII stored in recommendation feature stores; user IDs are pseudonymized. GDPR "right to erasure" handled within 30 days.
8. **Auditability**: Every served recommendation is logged with the model version, feature snapshot, and experiment bucket for offline analysis.

### Out of Scope

- Search ranking (separate IR system).
- Ad recommendation (separate auction/bidding system).
- Social graph construction (assumed pre-built and queryable).
- Content moderation pipeline (a separate trust & safety system feeds filtered content IDs).
- Long-form model training infrastructure (MLflow / SageMaker pipelines are assumed; focus is serving and feature engineering).

---

## 2. Users & Scale

### User Types

| User Type | Description | Interaction Pattern |
|---|---|---|
| **Active users** | Log in daily, rich interaction history | High-signal; personalized recs dominant |
| **Casual users** | 1–4 sessions/week | Medium history; hybrid CF + content-based |
| **New users (cold-start)** | 0–10 interactions | Popularity + onboarding quiz signals |
| **Anonymous users** | No login | Session-based signals + geo/device defaults |
| **Content creators** | Upload/publish items | Receive creator-specific feed recs |
| **Operators/admins** | Configure experiment buckets and business rules | Write-path only; read recommendations as QA |

### Traffic Estimates

Assumption: Platform similar in scale to Netflix or YouTube — 500 M registered users, 100 M daily active users (DAU).

| Metric | Calculation | Result |
|---|---|---|
| DAU | Assumption | 100 M |
| Recommendation requests per user per day | Homepage load + 3 widget loads + 2 scroll refreshes = 6 | 6 |
| Daily recommendation API calls | 100 M × 6 | 600 M/day |
| Avg RPS (spread over 24 h) | 600 M / 86,400 | ~6,944 RPS |
| Peak-hour multiplier | Prime-time = 5× average | ~34,722 RPS |
| Safety headroom (2×) | 34,722 × 2 | **~70,000 RPS** |
| Items per recommendation response | 20 items per request | — |
| Total item scores computed per day | 600 M × 20 | 12 B score computations |
| Feedback events (clicks, watches, skips) | 100 M DAU × 40 events/user/day | 4 B events/day |
| Feedback event RPS | 4 B / 86,400 | ~46,296 RPS |

### Latency Requirements

| Component | Target (P50) | Target (P99) | Notes |
|---|---|---|---|
| End-to-end API response | 40 ms | 150 ms | Measured at load balancer |
| Candidate retrieval (ANN) | 5 ms | 20 ms | From in-memory vector index |
| Feature lookup (feature store) | 2 ms | 10 ms | Redis / DynamoDB |
| Model scoring (ranking model) | 8 ms | 30 ms | Single CPU or GPU batch |
| Business rules + filtering | 1 ms | 5 ms | In-process, O(N) over 500 candidates |
| Response serialization + network | 2 ms | 10 ms | JSON/Protobuf over HTTP/2 |

### Storage Estimates

| Data Type | Size per Record | Records | Total |
|---|---|---|---|
| User embedding (256-dim float32) | 256 × 4 B = 1 KB | 500 M users | 500 GB |
| Item embedding (256-dim float32) | 1 KB | 100 M items | 100 GB |
| User interaction log (raw events) | ~200 B/event | 4 B events/day | 800 GB/day → ~290 TB/year |
| User feature vector (50 dense features) | 200 B | 500 M users | 100 GB |
| Item feature vector (100 features) | 400 B | 100 M items | 40 GB |
| Served recommendation log | ~500 B/call | 600 M calls/day | 300 GB/day |
| Model artifacts (embedding tables) | — | — | ~10 GB per model version |
| A/B experiment assignments | 8 B user_id + 4 B bucket | 500 M | 6 GB |

### Bandwidth Estimates

| Flow | Rate | Bandwidth |
|---|---|---|
| Inbound feedback events | 46,296 RPS × 200 B | ~9.3 MB/s |
| Outbound recommendation responses | 70,000 RPS × 20 items × 200 B | ~280 MB/s |
| Feature store reads | 70,000 RPS × (1 KB user + 500 B item×500 candidates) | ~17.5 GB/s (internal) |
| Embedding index queries | 70,000 RPS × 1 KB query vector | ~70 MB/s |
| Model training data pipeline | 800 GB/day | ~9.3 MB/s sustained |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                           CLIENT LAYER                                   │
│   Mobile App / Web Browser / Smart TV / Third-Party API Consumer         │
└──────────────────────┬───────────────────────────────┬───────────────────┘
                       │  GET /recommendations          │  POST /feedback
                       ▼                               ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                        API GATEWAY / LOAD BALANCER                       │
│   Auth (JWT/OAuth2), Rate Limiting, TLS Termination, A/B Bucket Routing │
└──────────────┬───────────────────────────────────────────────────────────┘
               │
       ┌───────┴──────────────────────────────────┐
       ▼                                          ▼
┌─────────────────────┐               ┌─────────────────────────┐
│  RECOMMENDATION     │               │  FEEDBACK INGESTION     │
│  SERVING SERVICE    │               │  SERVICE                │
│                     │               │                         │
│  1. Candidate Gen   │               │  Kafka Producer         │
│  2. Feature Fetch   │               │  Event validation       │
│  3. Ranking Model   │               │  Schema enforcement     │
│  4. Business Rules  │               └────────────┬────────────┘
│  5. Diversity       │                            │
└──────┬──────────────┘                            ▼
       │                               ┌─────────────────────────┐
       │                               │  EVENT STREAM (Kafka)   │
       │                               │  Topics:                │
       │                               │  - raw.feedback         │
       │                               │  - raw.impressions      │
       └──────────────────────────────▶│  - raw.conversions      │
               (log impressions)       └──────┬──────────────────┘
                                              │
              ┌───────────────────────────────┼──────────────────────────┐
              ▼                               ▼                          ▼
┌─────────────────────┐       ┌──────────────────────┐   ┌──────────────────────┐
│  FEATURE PIPELINE   │       │  STREAM PROCESSOR    │   │  BATCH TRAINING      │
│  (Apache Spark)     │       │  (Flink / Spark      │   │  PIPELINE            │
│                     │       │   Streaming)         │   │  (Spark + Airflow)   │
│  - Daily user feats │       │                      │   │                      │
│  - Item statistics  │       │  - Real-time user    │   │  - Matrix factorize  │
│  - Co-occurrence    │       │    feature updates   │   │  - Train DNN ranker  │
│    matrices         │       │  - Session features  │   │  - Embed new items   │
│  - Embedding tables │       │  - Trending signals  │   │  - Cross-validation  │
└──────┬──────────────┘       └──────────┬───────────┘   └──────────┬───────────┘
       │                                 │                           │
       ▼                                 ▼                           ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         FEATURE STORE                                   │
│  Online: Redis Cluster (low-latency serving)                            │
│  Offline: S3 + Hive/Delta Lake (training data)                          │
│                                                                         │
│  Namespaces:  user_features  |  item_features  |  cross_features        │
└──────────────────────────────────┬──────────────────────────────────────┘
                                   │
              ┌────────────────────┼───────────────────────┐
              ▼                    ▼                        ▼
┌──────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
│  EMBEDDING       │   │  RANKING MODEL      │   │  A/B EXPERIMENT     │
│  INDEX           │   │  REGISTRY           │   │  PLATFORM           │
│  (FAISS / ScaNN) │   │  (TensorFlow        │   │                     │
│                  │   │   Serving /         │   │  - Bucket assign.   │
│  ANN search for  │   │   Triton)           │   │  - Config flags     │
│  candidate gen   │   │                     │   │  - Metric tracking  │
└──────────────────┘   └─────────────────────┘   └─────────────────────┘
```

**Component Roles:**

- **API Gateway**: Authenticates requests, enforces per-user rate limits (1,000 rec requests/hour), routes traffic to A/B experiment buckets, terminates TLS.
- **Recommendation Serving Service**: Orchestrates the multi-stage ranking pipeline. Stateless; horizontally scalable. Calls downstream services and returns ranked list.
- **Feedback Ingestion Service**: Validates, deduplicates, and publishes user interaction events to Kafka. Handles idempotency via event_id deduplication window.
- **Kafka Event Stream**: Durable, ordered event log. Decouples real-time and batch consumers. Topics partitioned by user_id for ordering guarantees within a user's timeline.
- **Stream Processor (Flink)**: Maintains rolling user feature windows (last-30-min clicks, session context). Updates Redis feature store within seconds of event arrival.
- **Batch Training Pipeline**: Nightly Spark jobs rebuild interaction matrices, retrain embedding models and DNN ranker, export model artifacts to model registry.
- **Feature Store (Redis online / S3 offline)**: Provides sub-10ms feature retrieval for serving; provides consistent snapshots for offline training to avoid training-serving skew.
- **Embedding Index (FAISS)**: In-memory approximate nearest neighbor index over item embeddings. Returns 500 candidate items from a user embedding in < 5 ms.
- **Ranking Model Registry (TF Serving)**: Versioned model deployment. Supports shadow mode (log predictions without serving), canary (5% traffic), and full deployment.
- **A/B Experiment Platform**: Deterministic bucket assignment by user_id hash. Tracks per-bucket CTR, watch time, purchase rate for statistical significance tests.

**Primary Use-Case Data Flow (Homepage Recommendation):**

```
1. User opens app → client sends GET /v1/recommendations?user_id=U&surface=homepage&n=20
2. API Gateway validates JWT, checks rate limit, assigns A/B bucket (e.g., model_v12)
3. Recommendation Serving Service starts pipeline:
   a. Fetch user embedding from Redis (key: emb:user:{user_id})      [~2 ms]
   b. ANN query FAISS index → 500 candidate item IDs                 [~5 ms]
   c. Batch-fetch item features for 500 candidates from Redis        [~8 ms]
   d. Batch-fetch user features (demographics, recent history)       [~2 ms]
   e. Assemble feature matrix [500 × 150 features]
   f. Call TF Serving with feature matrix → 500 raw scores           [~15 ms]
   g. Apply business rules: filter age-restricted, already-watched   [~2 ms]
   h. Apply diversity: max 3 items per category in top-20            [~1 ms]
   i. Sort descending by score, take top 20
4. Log impression event (user_id, item_ids, model_version, timestamp) → Kafka
5. Return JSON response with 20 ranked items, rec_token for feedback correlation
Total wall clock: ~40 ms P50
```

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────────────────────
-- USERS (sourced from user service; replicated subset for recs)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE users (
    user_id         BIGINT PRIMARY KEY,
    account_age_days INT,
    country_code    CHAR(2),
    platform        VARCHAR(20),      -- 'ios', 'android', 'web', 'tv'
    subscription_tier VARCHAR(20),    -- 'free', 'basic', 'premium'
    created_at      TIMESTAMP,
    updated_at      TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- ITEMS (products, videos, articles — polymorphic)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE items (
    item_id         BIGINT PRIMARY KEY,
    item_type       VARCHAR(30),      -- 'video', 'article', 'product'
    creator_id      BIGINT,
    title           VARCHAR(500),
    language        CHAR(5),
    category_l1     VARCHAR(100),     -- top-level category
    category_l2     VARCHAR(100),     -- sub-category
    content_tags    TEXT[],           -- e.g., ['sci-fi', 'action', '4K']
    duration_sec    INT,              -- null for non-video
    publish_date    TIMESTAMP,
    is_active       BOOLEAN DEFAULT TRUE,
    age_restricted  BOOLEAN DEFAULT FALSE,
    geo_blocked_in  CHAR(2)[],        -- countries where unavailable
    created_at      TIMESTAMP,
    updated_at      TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- USER INTERACTIONS (raw event log — append-only, partitioned)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_interactions (
    event_id        UUID            NOT NULL,
    user_id         BIGINT          NOT NULL,
    item_id         BIGINT          NOT NULL,
    event_type      VARCHAR(30)     NOT NULL,  -- 'click','watch','like','dislike',
                                               -- 'share','purchase','skip','impression'
    event_value     FLOAT,          -- watch fraction for 'watch'; rating for 'rate'
    session_id      VARCHAR(64),
    rec_token       VARCHAR(64),    -- links back to the rec that drove this event
    surface         VARCHAR(30),    -- 'homepage', 'search', 'email'
    platform        VARCHAR(20),
    event_ts        TIMESTAMP       NOT NULL,
    -- Partitioned by event_ts (monthly) for scan efficiency
    PRIMARY KEY (event_id, event_ts)
) PARTITION BY RANGE (event_ts);

CREATE INDEX idx_interactions_user_ts ON user_interactions (user_id, event_ts DESC);
CREATE INDEX idx_interactions_item_ts ON user_interactions (item_id, event_ts DESC);

-- ─────────────────────────────────────────────────────────────
-- USER EMBEDDINGS (written by batch training pipeline)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_embeddings (
    user_id         BIGINT PRIMARY KEY,
    embedding       FLOAT4[256],     -- 256-dimensional; stored as binary in practice
    model_version   VARCHAR(30),
    computed_at     TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- ITEM EMBEDDINGS
-- ─────────────────────────────────────────────────────────────
CREATE TABLE item_embeddings (
    item_id         BIGINT PRIMARY KEY,
    embedding       FLOAT4[256],
    model_version   VARCHAR(30),
    computed_at     TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- USER FEATURES (materialized by feature pipeline; online store)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_features (
    user_id             BIGINT PRIMARY KEY,
    -- Engagement history (windowed)
    clicks_7d           INT,
    clicks_30d          INT,
    watches_7d          INT,
    avg_watch_fraction  FLOAT,       -- last 30 interactions
    -- Category affinities (top-5)
    top_cat_1           VARCHAR(100),
    top_cat_1_score     FLOAT,
    top_cat_2           VARCHAR(100),
    top_cat_2_score     FLOAT,
    -- Session context (updated by stream processor)
    last_session_cats   TEXT[],      -- categories browsed in last session
    last_active_ts      TIMESTAMP,
    -- Cold-start signals
    onboarding_topics   TEXT[],      -- from sign-up quiz
    feature_version     INT,
    updated_at          TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- ITEM FEATURES (materialized by feature pipeline)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE item_features (
    item_id             BIGINT PRIMARY KEY,
    -- Popularity signals
    impressions_7d      BIGINT,
    clicks_7d           BIGINT,
    ctr_7d              FLOAT,       -- clicks_7d / impressions_7d
    avg_watch_fraction  FLOAT,       -- for videos
    likes_7d            INT,
    shares_7d           INT,
    purchases_7d        INT,
    -- Quality signals
    avg_rating          FLOAT,
    rating_count        INT,
    spam_score          FLOAT,       -- 0.0–1.0 from classifier
    -- Freshness
    age_hours           FLOAT,       -- computed at serving time
    freshness_score     FLOAT,       -- precomputed decay factor
    feature_version     INT,
    updated_at          TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- RECOMMENDATION LOG (audit + offline training)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE recommendation_log (
    rec_token       VARCHAR(64)  PRIMARY KEY,
    user_id         BIGINT,
    surface         VARCHAR(30),
    model_version   VARCHAR(30),
    experiment_bucket VARCHAR(50),
    ranked_items    BIGINT[],    -- ordered list of returned item_ids
    feature_snapshot JSONB,     -- snapshot of key user features at serving time
    served_at       TIMESTAMP
) PARTITION BY RANGE (served_at);

-- ─────────────────────────────────────────────────────────────
-- EXPERIMENT BUCKETS
-- ─────────────────────────────────────────────────────────────
CREATE TABLE experiment_assignments (
    experiment_id   VARCHAR(50)  NOT NULL,
    user_id         BIGINT       NOT NULL,
    bucket_name     VARCHAR(50)  NOT NULL,  -- 'control', 'treatment_a', 'treatment_b'
    assigned_at     TIMESTAMP,
    PRIMARY KEY (experiment_id, user_id)
);

-- ─────────────────────────────────────────────────────────────
-- BLOCKED / FILTERED ITEMS PER USER
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_item_filters (
    user_id         BIGINT,
    item_id         BIGINT,
    reason          VARCHAR(30),   -- 'not_interested', 'already_purchased', 'blocked'
    created_at      TIMESTAMP,
    PRIMARY KEY (user_id, item_id)
);
```

**Redis Key Patterns (Online Feature Store):**

```
emb:user:{user_id}         → HSET  {vec: <binary 1KB>, model_ver: "v12", ts: epoch}
emb:item:{item_id}         → HSET  {vec: <binary 1KB>, model_ver: "v12"}
feat:user:{user_id}        → HSET  {clicks_7d: 45, avg_watch_frac: 0.72, ...}
feat:item:{item_id}        → HSET  {ctr_7d: 0.08, spam_score: 0.01, ...}
filter:user:{user_id}      → SET   (Bloom filter of seen/blocked item_ids, ~10 KB)
trending:global            → ZSET  item_id → score (TTL 1h, refreshed by Flink)
trending:cat:{cat_id}      → ZSET  item_id → score (TTL 1h)
```

### Database Choice

| Concern | Options Considered | Selected | Justification |
|---|---|---|---|
| **Interaction log (write-heavy, append-only)** | PostgreSQL (partitioned), Cassandra, ClickHouse | **ClickHouse** | Column-oriented storage gives 10–100× compression for time-series event data; vectorized aggregations for feature computation; LSM-tree writes sustain 1 M+ events/s per node |
| **Feature store (online, low-latency reads)** | Redis, DynamoDB, Aerospike | **Redis Cluster** | Sub-millisecond hash reads; supports binary data for embeddings; Cluster mode shards by key hash for horizontal scaling; built-in TTL for stale feature eviction |
| **Item / User catalog (relational queries)** | PostgreSQL, MySQL, DynamoDB | **PostgreSQL** | ACID guarantees for catalog mutations; rich query planner for ad-hoc operator queries; array types for tags; JSON for flexible metadata |
| **Embedding index (ANN search)** | FAISS, ScaNN, Pinecone, Weaviate | **FAISS (in-process, IVF_PQ)** | Runs in-memory within serving pods; IVF_PQ gives ~32× compression (256-dim float32 → 8-byte PQ code); recall@100 > 98% with nprobe=64; avoids network hop for candidate retrieval |
| **Offline training data / feature snapshots** | S3 + Parquet, Snowflake, BigQuery | **S3 + Delta Lake** | ACID transactions on Parquet files; time-travel for reproducible training runs; integrates natively with Spark; cost-effective at 290 TB/year vs warehouse pricing |
| **Model serving** | TF Serving, Triton, custom gRPC | **TF Serving** | Versioned model loading with zero-downtime canary; batching inference for GPU efficiency; gRPC proto interface with low serialization overhead |

---

## 5. API Design

### Authentication
All endpoints require a valid JWT Bearer token issued by the auth service. Anonymous users receive a short-lived anonymous session token. Rate limiting enforced at API Gateway by `user_id` (authenticated) or `session_id` (anonymous).

### Endpoints

```
GET /v1/recommendations
```
**Purpose:** Fetch personalized ranked items for a surface.

**Auth:** Bearer JWT (required)

**Rate limit:** 1,000 requests/hour per user_id; 100/hour per anonymous session.

**Query parameters:**

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| surface | string | yes | — | `homepage`, `sidebar`, `email`, `notification` |
| n | int | no | 20 | Number of items to return (max 100) |
| cursor | string | no | null | Opaque pagination cursor (base64-encoded state) |
| filter_seen | bool | no | true | Suppress items user already interacted with |
| experiment | string | no | null | Override experiment bucket (internal/admin only) |
| lat / lon | float | no | null | Geo context for location-aware content |

**Response:**
```json
{
  "rec_token": "rtk_7f3b9c2a...",
  "surface": "homepage",
  "model_version": "v12.3",
  "experiment_bucket": "control",
  "items": [
    {
      "item_id": 8823901,
      "item_type": "video",
      "title": "...",
      "score": 0.942,
      "reason": "Because you watched Inception",
      "category_l1": "movies",
      "thumbnail_url": "https://cdn.example.com/...",
      "duration_sec": 7200
    }
  ],
  "next_cursor": "eyJ1c2..."
}
```

**Error responses:** 401 (invalid token), 429 (rate limited, Retry-After header), 503 (model unavailable → fallback served with `"fallback": true`).

---

```
POST /v1/feedback
```
**Purpose:** Ingest user interaction events.

**Auth:** Bearer JWT (required)

**Rate limit:** 10,000 events/hour per user_id.

**Request body:**
```json
{
  "events": [
    {
      "event_id": "evt_a1b2c3d4...",
      "rec_token": "rtk_7f3b9c2a...",
      "item_id": 8823901,
      "event_type": "watch",
      "event_value": 0.85,
      "session_id": "sess_xyz",
      "event_ts": "2026-04-09T18:34:22Z"
    }
  ]
}
```

**Response:** `202 Accepted` with `{"accepted": 1, "rejected": 0}`. Events are enqueued to Kafka asynchronously; no synchronous write to DB. Idempotent: duplicate `event_id` within 24-hour window is silently dropped.

---

```
POST /v1/feedback/explicit
```
**Purpose:** Record explicit signals (thumbs up/down, ratings, "not interested").

**Auth:** Bearer JWT

**Request body:**
```json
{
  "item_id": 8823901,
  "signal_type": "not_interested",
  "reason_code": "already_seen"
}
```

**Response:** `200 OK`. Synchronously updates `user_item_filters` and publishes to Kafka for near-real-time feature store update (item added to Bloom filter within 30 seconds).

---

```
GET /v1/recommendations/similar
```
**Purpose:** "More like this" — items similar to a given item_id.

**Auth:** Bearer JWT (optional; unauthenticated returns popularity-weighted results)

**Query params:** `item_id` (required), `n` (default 10), `surface`

**Response:** Same structure as `/v1/recommendations` with `reason: "Similar to <item_title>"`.

---

```
GET /v1/experiments/{experiment_id}/assignment
```
**Purpose:** Returns the A/B bucket assigned to the calling user for the given experiment.

**Auth:** Bearer JWT

**Response:** `{"experiment_id": "ranking_v13_test", "bucket": "treatment_a", "assigned_at": "..."}`

---

```
POST /v1/admin/experiments
```
**Purpose:** Create or update an experiment configuration.

**Auth:** Bearer JWT with `role: admin`

**Rate limit:** 100/hour per admin

**Request body:**
```json
{
  "experiment_id": "ranking_v13_test",
  "description": "Test two-tower DNN ranker v13",
  "buckets": [
    {"name": "control", "weight": 0.50, "model_version": "v12.3"},
    {"name": "treatment_a", "weight": 0.50, "model_version": "v13.0"}
  ],
  "start_ts": "2026-04-10T00:00:00Z",
  "end_ts": "2026-04-24T00:00:00Z",
  "metrics": ["ctr", "watch_time_per_session", "7d_retention"]
}
```

**Response:** `201 Created`.

---

## 6. Deep Dive: Core Components

### 6.1 Candidate Generation

**Problem it solves:** The item catalog contains 100 M+ items. Scoring every item with the ranking model at serving time is computationally infeasible (would take hundreds of seconds). Candidate generation narrows this to a tractable set (500–2,000 items) in under 20 ms using approximate nearest-neighbor search over learned embeddings.

#### Approaches Comparison

| Approach | Recall | Latency | Scalability | Cold-Start | Notes |
|---|---|---|---|---|---|
| **Popularity-based (baseline)** | Low (no personalization) | < 1 ms | Trivially scales | Handles cold-start | Good fallback |
| **Item-item CF (pre-computed neighbors)** | Medium | < 5 ms | Precompute offline | Poor for new items | Works for known users |
| **User-item matrix cosine retrieval** | High for known users | O(N) — too slow for 100 M items at serve time | Doesn't scale | Poor | Not viable at scale |
| **ANN over learned user/item embeddings (Two-Tower)** | High | < 10 ms with FAISS IVF_PQ | Very scalable (shard index) | Handles with content embeddings | **Selected** |
| **Graph-based retrieval (random walk, PIN2VEC)** | High | 20–50 ms | Complex infra | Medium | Pinterest-style; adds latency |
| **Session-based (transformer over recent actions)** | Very high for session | 30–80 ms | GPU-intensive | Good | Better as re-ranker than retriever |

**Selected Approach: Two-Tower Embedding Model + FAISS IVF_PQ**

**Architecture:**
```
User Tower                     Item Tower
──────────────                 ──────────────
user_id embedding              item_id embedding
 + age_days                    + category embedding
 + category affinities         + content tags embedding
 + recent interaction seq      + freshness decay
        │                              │
   [Dense 512]                   [Dense 512]
   [ReLU]                         [ReLU]
   [Dense 256]                    [Dense 256]
   [L2 Normalize]                 [L2 Normalize]
        │                              │
   user_vec (256-dim)          item_vec (256-dim)
```

**Training objective:** In-batch softmax with sampled negatives. For a batch of B (user, item) positive pairs, the model maximizes:

```
L = -1/B × Σᵢ log( exp(uᵢ · vᵢ / τ) / Σⱼ exp(uᵢ · vⱼ / τ) )
```

where τ = 0.07 (temperature hyperparameter), u is user embedding, v is item embedding.

**FAISS Index Configuration:**
- Index type: `IndexIVF_PQ` with `nlist=4096` Voronoi cells, `M=32` PQ sub-spaces, `nbits=8`
- Memory: 100 M items × 32 bytes/PQ code = 3.2 GB (fits in one serving pod's memory)
- Build time: ~4 hours on 32 CPU cores (nightly rebuild)
- Search: `nprobe=64` cells scanned → recall@100 = 98.3%, latency P99 < 8 ms
- Sharding: partition FAISS index by item category; query only relevant shards to cut nprobe latency

**Pseudocode for candidate retrieval:**
```python
def retrieve_candidates(user_id: int, surface: str, n_candidates: int = 500):
    # 1. Fetch user embedding (Redis, ~2ms)
    user_vec = redis.hget(f"emb:user:{user_id}", "vec")  # 1 KB binary
    if user_vec is None:
        # Cold-start: use onboarding category centroids
        user_vec = get_cold_start_vector(user_id)

    # 2. Fetch active session categories for re-weighting (Redis, <1ms)
    session_cats = redis.hget(f"feat:user:{user_id}", "last_session_cats")

    # 3. ANN search in FAISS (in-process, ~5ms)
    query = np.frombuffer(user_vec, dtype=np.float32)
    distances, item_ids = faiss_index.search(query.reshape(1, -1), n_candidates)
    # distances are inner products (after L2 norm = cosine similarities)

    # 4. Boost trending items (blend in top trending not already in results)
    trending_ids = redis.zrevrange("trending:global", 0, 50)
    item_ids = merge_with_dedup(item_ids[0], trending_ids, max_total=n_candidates)

    # 5. Filter globally (age restriction, geo block) — O(N) fast path
    user_ctx = get_user_context(user_id)
    item_ids = [i for i in item_ids
                if not is_blocked(i, user_ctx)]

    return item_ids  # ~500 candidates
```

#### Interviewer Q&A

**Q1: How do you handle a brand new item that was just published 10 minutes ago with no interaction data?**

A: New items are "cold" on the collaborative signal but can still receive content embeddings immediately. The item tower of the two-tower model is designed to operate entirely on content features (title text embedding via a fine-tuned MiniLM, category one-hot, tags, creator embedding). When an item is published, a streaming job calls the item tower inference to compute `item_vec` and inserts it into both Redis and FAISS. The item then participates in ANN retrieval within minutes. Over time, as it accumulates clicks, the interaction-based components of the item tower gain weight. For the first 24–48 hours we also apply an "exploration boost" multiplier to give new items guaranteed impressions in some fraction of recommendation slates (similar to explore-exploit balancing in a bandit).

**Q2: What recall does 98% recall@100 mean for the business, and is it good enough?**

A: Recall@100 means that 98% of the time, the true "best" item for a user (as judged by the final ranker) is present somewhere in the 500 ANN candidates. The remaining 2% of cases return a set that doesn't include the theoretically ideal item. In practice, the ranking model's job is to find the best item among the 500 — if the candidate set contains 499 good items and misses 1 perfect item, the quality loss is marginal. We validate this empirically by running offline experiments: we score all items in a held-out test set with the ranker and compare the ranking quality (NDCG@10) between the full-catalog ranker and the ANN-filtered ranker. If the gap in NDCG@10 is < 0.5%, we accept the approximation. This is the standard industry tradeoff: the full-scan ranker is unachievable at 500 K RPS, while the ANN ranker is tractable.

**Q3: How do you update the FAISS index in real-time without taking it offline?**

A: We do not attempt fully real-time FAISS index updates; FAISS IVF_PQ does not support efficient incremental inserts without retraining the IVF quantizer. Instead, we maintain a small "delta index" — a brute-force flat index containing items published in the last 24 hours (typically < 100 K items, ~100 MB). At serving time, candidates are drawn from both the main IVF_PQ index and the delta flat index, then merged. Nightly, the delta index items are folded into a rebuilt main index. This dual-index pattern is used by Pinterest's Pixie system and LinkedIn's Galene.

**Q4: What happens if FAISS is unavailable (pod crash)?**

A: We run FAISS on a sidecar container co-located with each Recommendation Serving pod. If the sidecar is unhealthy (fails readiness probe), the serving pod itself is marked unready and removed from the load balancer pool. Traffic shifts to remaining healthy pods. Simultaneously, Kubernetes reschedules the crashed pod. Since the FAISS index is loaded from S3 on startup (< 2 minutes for 3.2 GB over 10 Gbps internal network), recovery is fast. Additionally, the item-item CF pre-computed neighbor table in Redis provides a 100% in-memory fallback candidate source that doesn't require FAISS.

**Q5: How do you ensure diversity in candidates before ranking?**

A: Candidate diversification operates at two levels. First, during ANN retrieval, we query per-category FAISS shards and set a maximum of 100 candidates per category_l1, preventing a single dominant category from flooding the candidate set. Second, after ranking, we apply Maximal Marginal Relevance (MMR) to the top-50 re-ranked items:

```
MMR_score(i) = λ × relevance(i) - (1-λ) × max_{j∈selected} sim(i, j)
```

We tune λ per surface (λ=0.8 for search sidebar where relevance dominates; λ=0.6 for homepage where discovery is valued). The A/B platform tracks category entropy of recommendation slates as a diversity metric alongside CTR.

---

### 6.2 Multi-Stage Ranking Model

**Problem it solves:** Given 500 candidates from retrieval, we must compute a relevance score for each using rich cross-features (user × item interactions that the embedding model couldn't capture) within 30 ms total inference budget.

#### Approaches Comparison

| Approach | Expressiveness | Latency | Training Complexity | Interpretability |
|---|---|---|---|---|
| **Logistic regression (LR) on manual features** | Low | < 1 ms | Low | High | 
| **Gradient Boosted Trees (GBDT: XGBoost/LightGBM)** | Medium-High | 2–5 ms | Medium | Medium |
| **Wide & Deep (LR + DNN)** | High | 10–20 ms | Medium | Low |
| **Deep & Cross Network (DCN-v2)** | Very High | 15–30 ms | High | Low |
| **Transformer-based sequential ranker** | Very High | 50–200 ms | Very High | Very Low |
| **Mixture of Experts (MoE)** | Very High | 20–50 ms | Very High | Very Low |

**Selected: Deep & Cross Network v2 (DCN-v2)**

DCN-v2 explicitly models feature interactions up to arbitrary order via a Cross Network stacked with a Deep Network:

```
Input: [user_features ‖ item_features ‖ cross_features]  (150 features total)
         └─────────────────┬─────────────────────────────┘
                     Embedding Layer
                (sparse ID features → dense)
                           │
          ┌────────────────┴────────────────────┐
          │  Cross Network (K=6 layers)          │  Deep Network (4 layers)
          │  xₖ₊₁ = x₀ × W xₖᵀ + bₖ + xₖ      │  [512 → 256 → 128 → 64]
          │  Explicitly models feature crosses    │  ReLU activations, BatchNorm
          └────────────────┬────────────────────┘
                           │ (concatenate)
                    [Dense 64 → sigmoid]
                           │
                    P(engagement | user, item)
```

**Feature categories (150 total):**

```
User features (50):
  - user_id embedding (32-dim, looked up from table)
  - clicks_7d, clicks_30d, watches_7d (normalized)
  - avg_watch_fraction (last 30)
  - top_cat_1/2/3 embedding (16-dim each)
  - subscription_tier one-hot (3)
  - account_age_days (log-normalized)
  - last_session_cats set-encoding (20 binary features)

Item features (60):
  - item_id embedding (32-dim)
  - category_l1/l2 embedding (16-dim each)
  - ctr_7d, avg_watch_fraction (item-side)
  - spam_score, avg_rating, rating_count
  - age_hours (freshness: 1 / (1 + age_hours/48))
  - creator_id embedding (16-dim)
  - content tags: tf-idf reduced to 20-dim via SVD

Cross features (40):
  - user_top_cat × item_category_l1 match (binary)
  - user_avg_watch_fraction × item_avg_watch_fraction (product)
  - creator_id in user_followed_creators (binary)
  - similarity(user_embedding, item_embedding) (scalar, from retrieval step)
  - item_age_hours × user_last_active_hours_ago
```

**Training:**

- Dataset: 90 days of (user, item, label) triples. Label = 1 if watch_fraction > 0.5 or explicit like; 0 otherwise. 4 B positive + 40 B sampled negatives per training epoch.
- Loss: Binary cross-entropy with frequency-based negative sampling correction.
- Optimizer: Adam, lr=1e-4, batch size=4096.
- Training compute: 8× A100 80GB, ~6 hours per epoch, nightly retrain.
- Offline metric: AUC-ROC (target > 0.78), NDCG@10 on held-out week.

**Inference pseudocode:**
```python
def rank_candidates(user_id: int, candidate_ids: List[int]) -> List[Tuple[int, float]]:
    # 1. Batch fetch all features (single Redis pipeline call)
    user_feat = redis.hgetall(f"feat:user:{user_id}")           # ~2ms
    item_feats = redis.mget([f"feat:item:{i}" for i in candidate_ids])  # ~8ms

    # 2. Assemble feature matrix [500 × 150]
    X = assemble_feature_matrix(user_feat, item_feats, candidate_ids)

    # 3. Call TF Serving (batched gRPC)
    request = PredictRequest()
    request.model_spec.name = "dcn_ranker"
    request.model_spec.version.value = CURRENT_MODEL_VERSION
    request.inputs["features"].CopyFrom(tf.make_tensor_proto(X, dtype=tf.float32))
    response = stub.Predict(request, timeout=0.025)  # 25ms deadline
    scores = response.outputs["scores"].float_val     # 500 scores

    # 4. Zip and sort
    ranked = sorted(zip(candidate_ids, scores), key=lambda x: -x[1])
    return ranked
```

#### Interviewer Q&A

**Q1: How do you avoid training-serving skew?**

A: This is one of the most common production ML bugs. We enforce skew prevention at three levels. First, features are computed by a shared library called the feature transformer, which is the same Python code deployed both in the Spark feature pipeline and inside the serving container. This code is version-pinned: the model artifact's metadata records the exact `feature_transformer==2.3.1` version it was trained with. Second, we use a feature store with point-in-time correctness: training jobs use the Feast SDK's `get_historical_features` which retrieves each training example's features as they existed at the event timestamp, not as of batch creation time. Third, we monitor serving vs training feature distributions in real-time using a KL-divergence check: if the KL divergence between serving feature distribution and training distribution exceeds a threshold (0.1 for critical features), an alert fires before model quality degrades.

**Q2: What objective are you actually optimizing, and does it match the business goal?**

A: We optimize for watch fraction > 0.5 (completion rate) rather than raw click-through rate, because CTR optimization is well-known to create clickbait feedback loops — thumbnails get manipulated to maximize clicks on unwatchable content. Watch fraction aligns better with user satisfaction. However, it still doesn't capture long-term retention. The complete objective is a composite: 0.5 × completion_rate + 0.3 × explicit_positive_signal + 0.2 × 7-day_return_rate. The 7-day return rate component is computed as a delayed label: we join the recommendation log with user session data 7 days later to label whether a recommendation session led to the user returning. This delayed label requires a more complex training pipeline (Kafka + time-join in Spark) but prevents optimizing purely for short-term engagement at the cost of user satisfaction.

**Q3: How do you handle position bias in training data?**

A: Position bias is a systematic artifact where items shown at the top of a list receive more clicks not because they are more relevant but because users see them first. If we train on raw click data, the model learns to score items higher that were historically ranked higher — a self-reinforcing feedback loop. We correct for this using Inverse Propensity Scoring (IPS): each positive training example is weighted by 1/P(displayed at position k), where the propensity P(k) is estimated from randomization experiments (we periodically shuffle recommendation positions for a small holdout traffic slice to measure true position-click curves). At position 1: P≈0.42; at position 10: P≈0.08. A training example from position 10 gets weight 12.5× vs position 1's 2.4× — restoring the underlying relevance signal.

**Q4: How do you deploy a new model version without downtime?**

A: We use a three-phase deployment via TF Serving's native model versioning. Phase 1 (Shadow): the new model version is loaded alongside the current model. All requests are scored by both models, but only the current model's scores are served. We compare offline metrics (AUC, feature distributions) for 24 hours. Phase 2 (Canary): 5% of production traffic is routed to the new model via A/B experiment bucket assignment. We monitor online metrics (CTR, watch time, p99 latency) for 48 hours. If metrics are within ±2% of control and latency is acceptable, we proceed. Phase 3 (Full rollout): traffic is gradually shifted 5% → 25% → 50% → 100% over 6 hours, with automated rollback triggered if CTR drops > 3% or p99 latency exceeds 50 ms.

**Q5: DCN-v2 has 150 input features. How do you identify which features are actually contributing?**

A: We use three techniques. First, permutation feature importance during offline evaluation: shuffle each feature column independently in the held-out test set and measure AUC drop; features whose shuffling causes > 0.01 AUC drop are deemed important. Second, we log the magnitude of cross-network weight matrices — high-magnitude cross-feature interactions reveal which feature pairs the model learned to combine. Third, we use SHAP values on a 1% sample of predictions during shadow mode, averaging SHAP contributions across users and items to produce a global feature importance ranking, which we review quarterly for feature pruning. Features contributing < 0.001 mean absolute SHAP value are candidates for removal to reduce inference cost.

---

### 6.3 Cold-Start Problem

**Problem it solves:** New users have no interaction history (user cold-start); new items have no engagement data (item cold-start). Serving random or purely popularity-based recommendations at these moments causes poor first-session experiences, which directly impacts Day-1 and Day-7 retention rates.

#### Approaches Comparison

| Approach | Coverage | Quality | Complexity | Notes |
|---|---|---|---|---|
| **Popularity (global/geo)** | 100% | Low (not personalized) | Very low | Baseline; always fallback |
| **Onboarding quiz mapping** | ~60% of users complete quiz | Medium | Low | Works but requires user effort |
| **Demographic-based (age, country, device)** | 100% | Low-medium | Low | Privacy concerns; limited signal |
| **Session-based signals (device, time of day, referral)** | 100% | Medium | Medium | No history needed |
| **Transfer from content features** | 100% | Medium-High | High | Content embedding inference |
| **Multi-armed bandit (explore-exploit)** | 100% | Improves over time | Medium | Handles item cold-start well |
| **Cross-domain transfer (social profile, linked accounts)** | ~40% | High | High | Privacy-sensitive |

**Selected: Hybrid — Content Embeddings + Bandit Exploration + Onboarding Quiz**

**User Cold-Start Flow:**

```python
def get_cold_start_recs(user_id: int, surface: str, n: int = 20) -> List[int]:
    signals = {}

    # Signal 1: Onboarding quiz (if available)
    quiz_topics = get_onboarding_topics(user_id)  # e.g., ['sci-fi', 'cooking', 'tech']
    if quiz_topics:
        # Map topics to category_l1 centroid embeddings
        topic_vecs = [TOPIC_CENTROID_MAP[t] for t in quiz_topics if t in TOPIC_CENTROID_MAP]
        user_vec = np.mean(topic_vecs, axis=0)  # average centroid as initial user vec
        signals['quiz_vec'] = user_vec

    # Signal 2: Session context (device, referral URL, geo, time of day)
    session_ctx = get_session_context(user_id)
    trending_ids = redis.zrevrange(f"trending:country:{session_ctx.country}", 0, 100)

    # Signal 3: Bandit arms (30% explore budget for new users)
    bandit_items = epsilon_greedy_explore(
        user_id=user_id,
        epsilon=0.3,  # 30% exploration for new users (vs 5% for veteran users)
        n=n
    )

    # Combine: 50% quiz/embedding-based, 30% trending, 20% bandit
    if 'quiz_vec' in signals:
        emb_items = faiss_index.search(signals['quiz_vec'], n * 3)[1][0]
        result = merge_ranked([emb_items, trending_ids, bandit_items],
                               weights=[0.5, 0.3, 0.2], n=n)
    else:
        result = merge_ranked([trending_ids, bandit_items],
                               weights=[0.6, 0.4], n=n)

    return result
```

**Item Cold-Start — Bandit Exploration Budget:**

New items receive a guaranteed "exploration slot" in every Nth recommendation slate. Specifically, for each serving request, we reserve 2 of the 20 recommendation slots for exploration candidates. Items younger than 48 hours are entered into the exploration pool with a Thompson Sampling bandit:

```
For each new item i:
  α_i = 1 + successful_engagements  (Beta distribution parameter)
  β_i = 1 + non_engagements
  
  At serving time, sample:
  θ_i ~ Beta(α_i, β_i)
  
Select the 2 items with highest θ_i samples among items < 48h old
```

This ensures new high-quality items surface quickly (high θ samples) while low-quality ones are naturally deprioritized without requiring manual curation.

#### Interviewer Q&A

**Q1: How do you measure whether your cold-start strategy is working?**

A: We track two key metrics: (1) Day-1 engagement rate (did the new user click or watch anything in their first session?), segmented by whether they went through the cold-start path vs had existing history. Ideally the gap is < 10%. (2) Day-7 retention rate — did the cold-start user return within 7 days? This is the true north star because a single good first session can retain a user long-term. We A/B test different cold-start strategies (quiz vs no quiz, different quiz lengths, different exploration rates) and use these two metrics as primary success criteria. Secondary metrics include the speed at which users "graduate" from cold-start (i.e., how many interactions are needed before their recommendations match veteran users' quality).

**Q2: What's the minimum number of interactions before you switch from cold-start to the full personalized model?**

A: Industry research (collaborative filtering literature) suggests that 10–20 interactions provide sufficient signal for meaningful personalization. In our system, we define three regimes: Regime 1 (0–5 interactions): pure cold-start as described. Regime 2 (6–20 interactions): hybrid — we use the user's partial history to form a biased user vector (average of interacted item embeddings) and blend it 50/50 with the demographic centroid. Regime 3 (21+ interactions): full two-tower model with learned user embedding. The transition thresholds are configurable and A/B tested; the 20-interaction threshold was empirically determined to be where our offline NDCG@10 metric converges to within 5% of the full-history model performance.

**Q3: How do you prevent new item cold-start from degrading the experience for veteran users?**

A: The 2-slot exploration budget is separate from the 18 slots served by the personalized ranker. We also apply a quality gate: items are only entered into the exploration pool if they pass automated quality checks (no spam_score > 0.3, creator in good standing, no NSFW tags unless user opted in). Furthermore, the bandit's Thompson Sampling naturally self-corrects: an item that receives poor engagement in its first 50 impressions will have a very low θ distribution and effectively be removed from the exploration pool without manual intervention. Veteran users also have their exploration rate reduced (ε = 0.05 vs 0.30 for new users), so they see more exploration-slot items only if those items have already proven their quality in the general population.

---

## 7. Scaling

### Horizontal Scaling

**Recommendation Serving Service:** Stateless pods deployed on Kubernetes. Auto-scales by CPU utilization (target 60%) and custom metric (RPS per pod). Minimum 50 replicas; maximum 500. Each pod holds FAISS sidecar in-memory (~4 GB RAM) and maintains a persistent gRPC connection pool to TF Serving. Pod startup time: ~90 seconds (FAISS index load from S3).

**Feature Store (Redis Cluster):** 64-shard cluster. Key distribution by consistent hashing (CRC16 mod 16384 hash slots). Each shard: master + 1 read replica. 64 × 2 = 128 Redis nodes. Each node: 32 GB RAM → total 64 × 32 GB = 2 TB usable. User+item features estimate: 100 GB user feats + 40 GB item feats + 500 GB embeddings = 640 GB — fits with headroom.

**Kafka:** 128 partitions per topic, partitioned by `user_id % 128`. 6 brokers × 4 TB NVMe SSD = 24 TB storage. Replication factor 3. At 46,296 feedback events/second × 200 B/event = 9.3 MB/s ingest — well within Kafka's multi-GB/s throughput per broker.

**ClickHouse:** 8 shards × 3 replicas = 24 nodes. Data distributed by `sipHash64(user_id) % 8`. Each shard handles 800 GB/day / 8 = 100 GB/day insertion. ClickHouse's columnar engine compresses this ~10× → ~10 GB/day per shard effective disk usage.

### DB Sharding

| Data Store | Shard Key | Strategy | Rationale |
|---|---|---|---|
| ClickHouse interactions | `sipHash64(user_id)` | Hash sharding | Even distribution; user queries co-located |
| Redis embeddings | `CRC16(key) mod 16384` | Hash slots (Redis native) | Automatic rebalancing on cluster resize |
| FAISS index | Category shard | Range/category partitioning | Query only relevant category shards |
| PostgreSQL items | `item_id % N` | Hash sharding | Read-heavy; sharding enables parallel reads |
| Recommendation log | `served_at` month | Range (time) partitioning | Efficient time-range scans for offline analysis |

### Replication

- **Redis:** Master-replica pairs per shard. Read replicas serve feature store reads (70% of traffic) to offload master. Sentinel quorum (3 sentinels) for automatic failover in < 30 seconds.
- **ClickHouse:** ReplicatedMergeTree with ZooKeeper-managed replication. Writes go to one shard replica; replication async to others within seconds. Reads load-balanced across replicas.
- **PostgreSQL:** Primary-standby streaming replication (synchronous for item catalog writes to prevent any data loss). Read replicas for analytical queries from the feature pipeline.
- **FAISS Index:** Not traditionally "replicated" — each serving pod maintains its own in-memory copy loaded from the same S3 source. Immutable snapshot semantics; a new version is uploaded to S3 and pods reload it on a rolling schedule.

### Caching Strategy

| Cache Layer | Technology | TTL | Hit Rate Target | Notes |
|---|---|---|---|---|
| User embeddings | Redis | 24h (refreshed on model retrain) | > 95% | 100 M users × 1 KB = 100 GB |
| Item features | Redis | 1h (refreshed by Flink stream job) | > 98% | 100 M items × 400 B = 40 GB |
| Trending items | Redis ZSET | 5 min | 100% | Global + per-category |
| Recommendation responses | Application-level (in-pod LRU) | 30 seconds | ~10% | Only for burst scenarios |
| Item detail (for response enrichment) | CDN (Cloudflare) | 1h | > 90% | Thumbnail URLs, titles |
| A/B bucket assignments | In-pod hashmap | App lifetime (preloaded) | 100% | Deterministic hash; no lookup needed |

### CDN

Item metadata (titles, thumbnails, descriptions) served through CDN (Cloudflare or Fastly) with 1-hour TTL. Reduces latency for API responses that must enrich item_ids with display metadata. CDN POP selection: 25 global POPs. Cache-key: `item_id + display_language`. Purge on item metadata update (webhook to CDN purge API).

### Interviewer Q&A

**Q1: Redis is storing 640 GB of embeddings and features. What happens when memory is exhausted?**

A: We address this on three fronts. First, memory capacity planning: we provision Redis at 50% utilization ceiling, so 640 GB of data implies at least 1.28 TB allocated (64 × 32 GB = 2 TB, giving headroom). Second, we apply TTL-based eviction: user embeddings that haven't been accessed in 24 hours are eligible for eviction (using Redis's `volatile-lru` policy). This naturally evicts cold/inactive users and leaves hot users in memory. For users evicted from Redis, a cache miss triggers a read from the embedding table in ClickHouse (< 20 ms) with a Redis write-through to repopulate. Third, we compress embeddings: instead of storing 256 × 4-byte float32 = 1 KB per user, we store quantized int8 embeddings (256 bytes) with a ~0.5% recall penalty, cutting the embedding namespace to 128 GB.

**Q2: How do you shard the recommendation serving layer itself?**

A: The serving layer is stateless (state lives in Redis and FAISS), so sharding is not strictly necessary — we just add pods. However, to avoid FAISS index cross-node traffic (each pod loads 3.2 GB FAISS index), we optimize by running FAISS shards per category: a pod handling "movies" loads only the movies sub-index (30 M items, ~1 GB). Requests are routed by a lightweight request router that reads the user's top category from Redis and routes to the appropriate shard. This reduces per-pod memory from 3.2 GB to ~500 MB while improving cache locality. Trade-off: requests for users with multi-category interests need fan-out to 3–5 pods and result merging; we cap fan-out at 5 pods to bound tail latency.

**Q3: How do you scale the batch training pipeline to retrain on 90 days of data nightly?**

A: The interaction log is 290 TB/year → 90 days = ~72 TB. Delta training (retraining on the last 24 hours of new data as fine-tuning rather than full retraining from scratch) reduces this: we fine-tune for 1 epoch on yesterday's 800 GB of interactions, which takes ~45 minutes on 8× A100s rather than 6 hours for full training. We do a full retrain weekly to prevent catastrophic forgetting from purely incremental updates. The 800 GB nightly delta is read from ClickHouse via the Spark-ClickHouse connector, processed in memory on a 100-node Spark cluster (EMR on AWS), and shuffled to the training cluster's HDFS staging area before training begins.

**Q4: At 70,000 peak RPS, how many Recommendation Serving pods do you need?**

A: Each pod can handle approximately 500 RPS (benchmark: 150 ms P99 target, single-threaded serving → 1000/150 = ~6.7 requests/s per thread; 75 async coroutines per pod → ~500 RPS/pod). At 70,000 RPS: 70,000 / 500 = 140 pods minimum. With 2× safety factor: 280 pods. Each pod: 8 GB RAM (4 GB FAISS + 4 GB app) + 4 vCPU. Cluster: 280 × 4 = 1,120 vCPUs, 280 × 8 GB = 2.24 TB RAM. On AWS c5.2xlarge (8 vCPU, 16 GB, ~$0.34/hr): 140 instances = $47.6/hr, or ~$35 K/month. Significant but proportionate for 70,000 RPS.

**Q5: How do you protect downstream systems (TF Serving, Redis) from traffic spikes?**

A: Three mechanisms. First, adaptive rate limiting: if RPS to a pod exceeds 600 (20% buffer), the API Gateway's rate limiter sheds traffic with 429 responses rather than overwhelming the pod. Second, circuit breakers (Hystrix/Resilience4j pattern) around each downstream call: if TF Serving response time exceeds 40 ms on 50% of calls in a 10-second window, the circuit opens and the serving pod falls back to GBDT-based ranking (precomputed scores, no TF Serving call) for the duration of the circuit-open period (default 30 seconds). Third, request coalescing: multiple concurrent requests for the same user_id within a 100 ms window are deduplicated — only one feature fetch and scoring is performed, and the result is fanned out to all waiting requests.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Detection | Recovery |
|---|---|---|---|---|
| **FAISS index (pod crash)** | OOM or segfault in sidecar | Pod removed from pool; candidates from Redis CF fallback | Readiness probe failure → k8s event | Pod reschedule; new pod loads from S3 in ~90s |
| **Redis cluster node failure** | Network partition or hardware failure | Feature store miss for ~1/64th of keys during failover | Sentinel detects; Redis cluster alerts | Sentinel promotes replica to master in < 30s; serving pod retries with 50ms backoff |
| **TF Serving unavailable** | Model OOM, deployment error | Ranking degraded | HTTP 503 → circuit breaker opens | Fallback to precomputed item popularity scores in Redis; circuit half-opens after 30s |
| **Kafka broker failure** | Disk failure, GC pause | Feedback events delayed (not lost) | Consumer lag metric > 100K events | Kafka replication ensures durability; consumer rebalances to healthy brokers |
| **Batch training failure** | Spark job OOM, data corruption | Model staleness (last good model used) | Airflow task failure alert | Airflow retry (3 attempts); if all fail, page on-call; last successful model continues serving |
| **Feature pipeline lag** | Flink checkpoint failure | Stale user features (up to 5 min old) | Flink checkpoint failure metric | Flink restores from last checkpoint; features briefly stale but not absent |
| **ClickHouse write failure** | Replication lag, disk full | Interaction events lost if not durable | ClickHouse replication lag alert | Kafka acts as durable buffer; ClickHouse consumer replays from last committed offset |
| **API Gateway overload** | DDoS, sudden traffic spike | Request shedding | RPS spike, CPU alert | Auto-scaling triggers within 60s; rate limiter sheds excess traffic |

### Failover Strategy

1. **Recommendation serving fails → Popularity fallback**: Pre-computed global and category-level popularity rankings are refreshed every 5 minutes and stored in Redis as sorted sets. If the full ML pipeline is unavailable, the serving layer reads from `popularity:global` ZSET in under 1 ms.

2. **Redis unavailable → ClickHouse fallback for features**: Serving pods detect Redis connection failures via health check. A ClickHouse HTTP endpoint exposes the same feature data with a slightly higher latency (~20 ms). The degraded serving path accepts the additional latency to preserve personalization.

3. **Regional failure → Multi-region active-active**: Two AWS regions (us-east-1 primary, eu-west-1 secondary) operate independently with eventual consistency. Route 53 latency routing directs users to the nearest healthy region. Feature stores replicate via cross-region Kafka replication with ~500 ms lag.

### Retries and Idempotency

- **Feedback events**: Events carry a client-generated `event_id` (UUID v4). The feedback ingestion service maintains a 24-hour Bloom filter of seen `event_id`s. Duplicate submissions are dropped. Kafka producer uses `acks=all` + `retries=Integer.MAX_VALUE` for at-least-once delivery; idempotency is enforced at the application layer.
- **Feature store reads**: Retried once with 5 ms exponential backoff on timeout. Second failure triggers the ClickHouse fallback. No retry storms due to jitter.
- **TF Serving calls**: Single retry with 10 ms delay on timeout. If second attempt fails, circuit breaker opens.
- **Batch pipeline writes to ClickHouse**: Kafka consumer commits offset only after successful ClickHouse insert. Failures cause consumer to replay from last committed offset, ensuring exactly-once semantics at the cost of potential duplicates (deduplicated by ClickHouse's `ReplacingMergeTree` engine using `event_id` as the dedup key).

### Circuit Breaker Configuration

```
TF Serving circuit breaker:
  - CLOSED: requests flow normally
  - Open condition: > 50% requests > 40ms OR > 10% requests timeout in 10s window
  - OPEN state: fail-fast, return popularity scores, duration=30s
  - HALF-OPEN: allow 5% test requests; if < 20% fail, close circuit
  
Redis circuit breaker:
  - Open condition: > 5 connection timeouts in 5s
  - Fallback: ClickHouse HTTP feature endpoint
```

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Tool |
|---|---|---|---|
| Recommendation API p99 latency | Histogram | > 200 ms | Prometheus + Grafana |
| FAISS ANN recall@100 (offline) | Gauge | < 97% | Nightly batch job → Prometheus pushgateway |
| TF Serving inference p99 | Histogram | > 40 ms | Prometheus (TF Serving exporter) |
| Redis hit rate | Gauge | < 90% | Redis INFO + Prometheus |
| CTR per surface (online) | Counter ratio | 20% drop vs 7-day avg | Custom ML metrics dashboard |
| Watch completion rate | Counter ratio | 15% drop vs 7-day avg | Custom ML metrics dashboard |
| Model staleness (hours since last retrain) | Gauge | > 36 h | Airflow → Prometheus |
| Feature freshness (Flink checkpoint lag) | Gauge | > 10 min | Flink metrics → Prometheus |
| Feedback event Kafka consumer lag | Gauge | > 500K events | Kafka Consumer Lag Monitor |
| A/B experiment imbalance | Gauge | bucket assignment drift > 1% | Experiment platform |
| Serving pod OOM rate | Counter | > 0 per hour | Kubernetes event alerts |
| Diversity score (avg category entropy of slates) | Gauge | Drop > 10% week-over-week | Custom analytics job |
| New item exploration coverage | Gauge | < 80% of new items getting 50 impressions in 48h | Custom job |

### Distributed Tracing

Every recommendation request carries a `trace_id` (W3C TraceContext header). Traces are emitted via OpenTelemetry SDK and sent to Jaeger (self-hosted) or AWS X-Ray. Each span covers:

- `api_gateway.auth` — JWT validation
- `rec_service.candidate_gen` — FAISS ANN search
- `rec_service.feature_fetch` — Redis pipeline call
- `rec_service.ranking` — TF Serving gRPC call
- `rec_service.business_rules` — filtering
- `rec_service.diversity` — MMR application

Trace sampling: 100% for errors; 1% for success paths (to limit storage cost while enabling representative profiling).

### Logging

Structured JSON logs emitted to stdout, collected by Fluentd DaemonSets, shipped to Elasticsearch (7-day hot tier) → S3 (90-day warm tier, Athena queryable) → Glacier (1-year cold archive).

Log levels:
- **ERROR**: Any exception, circuit breaker open/close transitions, model load failures.
- **WARN**: Feature store miss (cache miss degraded path), A/B assignment collision.
- **INFO**: Every recommendation request summary (user_id hashed, surface, model_version, item_count, latency_ms, experiment_bucket) — for offline analysis.
- **DEBUG**: Full feature vectors, candidate scores — emitted only in shadow mode or when `X-Debug: true` admin header present.

Sensitive data (PII): user_id is hashed with HMAC-SHA256 using a rotating daily key before logging. Raw user_id is never written to logs.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A (Chosen) | Option B (Rejected) | Reason for Choice |
|---|---|---|---|
| Candidate retrieval algorithm | FAISS IVF_PQ (in-process) | Pinecone (managed vector DB) | In-process eliminates network hop; 3.2 GB fits in pod RAM; Pinecone adds $0.08/1M queries overhead and 5–15 ms extra latency |
| Ranking model architecture | DCN-v2 (30 ms inference) | Transformer sequential ranker (100 ms) | DCN-v2 meets 150 ms end-to-end budget; transformer exceeds it; retraining cost 4× lower |
| Feature store technology | Redis Cluster | DynamoDB | Redis 0.2 ms vs DynamoDB 2–8 ms for feature reads; feature store is the hottest read path at 70K RPS |
| Training objective | Watch fraction > 50% | Raw CTR | CTR optimization creates clickbait loops; watch fraction better proxies satisfaction |
| Feedback ingestion pattern | Async Kafka (202 Accepted) | Synchronous DB write (201 Created) | 46K feedback RPS would require massive DB write cluster; Kafka decouples and buffers |
| Diversity enforcement | MMR post-ranking | Diversified candidate retrieval | MMR preserves ranking quality for top positions; diversified retrieval is harder to tune without harming relevance |
| A/B experiment assignment | Deterministic hash (user_id) | Random per-request | Hash ensures stable bucket across sessions and devices; random creates inconsistent user experience |
| Cold-start strategy | Content embedding + bandit | Pure popularity fallback | Bandit exploration introduces new items faster; content embedding provides better personalization than pure popularity |
| Item embedding freshness | Nightly rebuild + delta index | Fully real-time incremental update | FAISS IVF_PQ doesn't support efficient incremental inserts; delta flat index bridges the gap at low cost |
| Interaction label definition | Watch fraction > 0.5 (positive label) | Binary click (positive label) | Click is too noisy; watch fraction reduces false positives and resists clickbait optimization |

---

## 11. Follow-up Interview Questions

**Q1: How would you design the system to support multi-objective optimization (e.g., balance click rate, watch time, and creator diversity)?**

A: We use a Pareto-optimal ranking approach via multi-objective Pareto fronts or a learned weighted linear combination. In practice, we define a composite score: `score = w1 × CTR_pred + w2 × watch_time_pred + w3 × creator_diversity_bonus`. The weights w1, w2, w3 are tuned via a constrained optimization: maximize w1 subject to creator diversity entropy ≥ threshold and average watch time ≥ baseline. These weights are A/B tested quarterly. For harder trade-offs, we implement "opportunity sets": the ranker optimizes CTR, but a post-processing step guarantees at least 2 of the top-20 slots go to creators with < 10K followers (creator diversity). This separates the optimization objective from the business policy, making it easier to tune policy without retraining the model.

**Q2: How would you handle GDPR right-to-erasure requests for recommendation data?**

A: Erasure has three components. (1) User data in online stores: delete Redis keys `emb:user:{user_id}`, `feat:user:{user_id}`, `filter:user:{user_id}` immediately (< 1 second via Redis DEL). (2) Interaction log in ClickHouse: ClickHouse does not support row-level deletes efficiently. We use ClickHouse's `ALTER TABLE ... DELETE WHERE user_id = X` (deferred merge operation). This completes within 24 hours but is not atomic. To bridge the gap, we maintain a "suppression list" (a Redis SET of erased user_ids) that is checked before returning any data. (3) Model artifacts: user_id embeddings in the FAISS index and the PyTorch embedding table cannot be "un-trained." GDPR interpretation (recital 26) holds that models trained on pseudonymized data are compliant if re-identification is not reasonably possible. We apply machine unlearning techniques (gradient-based influence function approximations) for high-risk users, but for most cases pseudonymization satisfies the standard. Erasure SLA: 30 days, verified by automated compliance tests.

**Q3: How would you detect and prevent feedback loops in the recommendation system?**

A: Feedback loops occur when popular items get more recommendations → more clicks → become even more popular, suppressing long-tail content. We detect them by monitoring the Gini coefficient of item impression distribution weekly: if the coefficient exceeds 0.8 (highly unequal), we flag a potential feedback loop. Prevention mechanisms: (1) Exploration budgets (2/20 slots for exploration) inject diversity. (2) We add an inverse-popularity penalty to item scores: `adjusted_score = model_score - α × log(item_impressions_7d / median_impressions)`. α is a tunable parameter that penalizes items already over-exposed. (3) We run periodic "forest fires" — a 1-week holdout where a 5% user slice receives popularity-reweighted recommendations — and compare long-term satisfaction vs the main algorithm to measure feedback loop effects.

**Q4: How would you scale the system to support 10× more items (1 billion items)?**

A: At 1 B items, the FAISS IVF_PQ index grows from 3.2 GB to 32 GB — too large for a single pod. Solutions: (1) Increase number of FAISS shards to 10 (one per 100 M items), each shard on a dedicated pod. Fan-out queries to all shards, merge candidates. (2) Add a pre-filter stage before ANN: given user's top-3 categories, query only the 2–3 relevant category shards (covering 300 M items at most). (3) Use hierarchical ANN: coarse-grained cluster index returns top-100 clusters, then fine-grained search within those clusters — reducing effective search space 10×. (4) Adopt a learned sparse retrieval approach (SPLADE-style) where items are represented in a high-dimensional sparse space indexed by inverted lists — allows sub-linear retrieval over 1 B items. Google's ScaNN and Meta's FAISS GPU variants also handle this scale with dedicated GPU serving.

**Q5: How would you implement real-time personalization that reacts to in-session behavior (e.g., user just watched 3 horror films and you want to update recommendations immediately)?**

A: This requires a session-aware serving path. When the user watches a film, the feedback event flows through Kafka → Flink stream processor, which within 1–2 seconds updates the `last_session_cats` field in the user's Redis feature hash. The next recommendation request reads this field and passes it as context. At the model level, we augment the user embedding with a "session delta": `effective_user_vec = 0.7 × stored_user_vec + 0.3 × session_item_embedding_average`. The session item embeddings are computed from the item IDs the user just engaged with (fetched from Redis `emb:item:*`). This gives a continuously updated user representation without waiting for the nightly batch retrain. For even faster adaptation, a small session-based transformer (4-head, 2-layer, 64-dim) operates on the sequence of the last 10 items consumed in-session and outputs a session embedding concatenated to the user tower input — this adds < 5 ms inference time.

**Q6: What happens to recommendations if a user purchases a product? How do you prevent re-recommending already-purchased items?**

A: Purchase events flow via Kafka → Feedback Ingestion Service, which synchronously (for purchase events specifically, we use synchronous processing because purchase re-recommendation is a critical UX failure) writes to the `user_item_filters` table with `reason='already_purchased'` and pushes the item_id into the user's Bloom filter in Redis (key: `filter:user:{user_id}`). The Bloom filter is checked at the final business-rules stage of serving, before the response is returned. Bloom filter false-positive rate: 0.1% (configured with 10 KB filter capacity for ~8,000 filtered items per user). For users who have purchased thousands of items (rare but possible), the Bloom filter is extended to 100 KB. Bloom filter updates are asynchronous after the first 30 seconds (Flink stream handles it); in the 30-second window after purchase, a direct Redis SET lookup is used as fallback to guarantee freshness.

**Q7: How would you handle a malicious user attempting to game recommendations (e.g., click farming to boost specific items)?**

A: Click fraud is detected and mitigated at multiple layers. (1) Rate limiting: the feedback API limits 10,000 events/hour per user. Unusual volumes trigger account review. (2) Behavioral anomaly detection: a Flink streaming job computes per-user click rate and item CTR in real-time. A Z-score > 4 (vs user's 30-day baseline) flags suspicious activity and marks events with `fraud_suspect=true`. Flagged events are excluded from feature computation and training data. (3) Item-side quality signals: a sudden spike in item CTR (> 10× 7-day average within 1 hour) triggers the spam_score classifier to re-evaluate the item. Items with spam_score > 0.7 are placed in a review queue and removed from recommendations pending human review. (4) Graph-based detection: coordinated click farming often involves a cluster of accounts with no social overlap. Graph anomaly detection (community detection on the user-item interaction bipartite graph) identifies coordinated groups.

**Q8: How do you ensure fairness — e.g., not discriminating against certain creator demographics in recommendation distribution?**

A: Fairness in recommendations requires both measurement and enforcement. Measurement: we segment impression share and engagement rates by creator demographic attributes (gender, nationality, account age, follower tier) and compute equal opportunity metrics — specifically whether similarly-quality content from different creator segments receives proportional exposure. Enforcement: we add a fairness regularization term to the ranking objective: `fair_loss = λ × Σ_groups (|avg_exposure_group - target_exposure_group|)`. Target exposure is set proportional to creator representation in the catalog, subject to a quality floor (items below the spam threshold). We also audit model predictions for disparate impact quarterly: if items from any protected group have model scores systematically lower by > 10% when controlling for engagement quality features, we investigate and retrain with debiased training data (re-weighted to remove historical exposure bias).

**Q9: What's your strategy for freshness vs. relevance trade-off? A brand-new item might be highly relevant but has no engagement history.**

A: Freshness and relevance are handled separately and combined at scoring time. Freshness score: `freshness = 1 / (1 + age_hours / half_life)` where half_life = 48 hours for news/articles and 720 hours (30 days) for evergreen content like films. This is a precomputed feature updated hourly. In the ranking model, freshness is an explicit input feature, so the model learns the appropriate weight for each content type and user context. Additionally, new items with no engagement history receive an "exploration bonus" multiplier: for the first 48 hours, their ranking score gets a +0.05 additive boost (on a 0–1 scale), ensuring they compete with established items even before the model has learned their quality. This bonus decays linearly to 0 at hour 48. The magnitude of the exploration bonus is itself A/B tested — too large wastes impression value on low-quality new content; too small means high-quality new content never surfaces.

**Q10: How do you validate that your offline evaluation metrics (AUC, NDCG) actually predict online business metrics (CTR, retention)?**

A: This is the "offline-online correlation problem." We measure it explicitly: for each of the last 12 A/B experiments, we record the offline metric delta (ΔAUC, ΔNDCG@10) and the online metric delta (ΔCTR, ΔD7 retention). We plot the correlation and compute Pearson r. Historically: ΔAUC correlates with ΔCTR at r=0.72 (moderate); ΔNDCG@10 correlates with ΔD7_retention at r=0.81 (strong). These correlation coefficients tell us how much to trust offline experiments. When a model shows +0.5% NDCG@10 offline, we expect approximately +0.4% D7 retention online (using the regression fit), with substantial uncertainty. Models with predicted online improvement > 0.3% are promoted to online A/B. Models with offline improvement but failed online validation are analyzed post-hoc: common causes are position bias in offline labels, test set time-leakage, or serving skew.

**Q11: Describe how you would design the A/B testing infrastructure for this system.**

A: The A/B platform operates at user-level bucketing for consistency (a user always sees the same model version across sessions). Bucket assignment: `bucket = hash(user_id + experiment_id) % 100` — deterministic, no storage required for assignment. For analysis, we use a switchback design for detecting network effects (if one user's recommendations affect another's behavior). Statistical testing: we use CUPED (Controlled-experiment Using Pre-Experiment Data) to reduce variance: we regress the treatment metric on the user's pre-experiment baseline to get a variance-reduced estimator, typically reducing required sample size by 30–50%. Minimum detectable effect: we power experiments to detect 1% changes in CTR at 95% confidence, 80% power, which requires ~2 M users per bucket based on observed CTR variance (σ² ≈ 0.06). Guard rails: experiments automatically pause if D1 retention drops > 2% or spam_score reports > 3% in treatment bucket. Multiple testing correction: we apply Bonferroni correction when evaluating > 3 metrics simultaneously.

**Q12: How would you handle the recommendation system during major live events (Super Bowl, product launch) when traffic spikes 10×?**

A: Pre-event preparation: (1) Traffic prediction: 7-day ahead traffic forecasting model triggers Kubernetes Horizontal Pod Autoscaler pre-warming 30 minutes before predicted spike. Pods: scale from 140 → 1,400 (10× headroom). (2) FAISS index is pre-loaded in warm pods — startup time is the bottleneck, so we keep a standing fleet of pre-loaded warm pods at 50% idle capacity during high-risk windows. (3) Trending item cache warmed with event-specific content. During event: (4) Emergency circuit breaker mode: if RPS exceeds 10× normal for > 60 seconds, the serving layer automatically switches to a simplified two-step pipeline: ANN retrieval + item popularity re-ranking (skip DCN-v2 scoring). This reduces per-request latency to < 30 ms and cuts TF Serving load by 90%. (5) Rate limiting tightens: burst allowance reduced from 1,000 to 200 requests/hour per user. Post-event: (6) Scale-down is gradual (50% over 30 minutes) to avoid thrashing.

**Q13: How would the design change if this were a B2B system serving recommendations to enterprise customers via API?**

A: B2B brings fundamentally different requirements. (1) Multi-tenancy: each customer (tenant) has isolated feature stores (tenant-prefixed Redis keys), isolated model versions, and isolated interaction logs. Tenant data must never bleed across boundaries. (2) Customization API: tenants need to inject their own business rules, category weights, and content filters without redeployment. We expose a "policy DSL" that tenants configure via a dashboard. (3) SLA contractual guarantees: B2B customers pay for SLA tiers (99.9% vs 99.99%). Separate serving pools per tier with independent circuit breakers. (4) Audit logs: every recommendation served must be exportable by the tenant for compliance. (5) Rate limits are per-tenant contract (e.g., 10 M calls/day for a "pro" tier) rather than per-user. (6) Training data ownership: tenants may require their training data to be isolated and deletable, which mandates fully separate model instances rather than a shared model — increasing infrastructure cost significantly.

**Q14: What metrics do you track to detect model degradation over time?**

A: We track three classes of degradation. (1) Input drift: feature distribution shift measured by Population Stability Index (PSI) for each input feature. PSI > 0.2 triggers an alert for that feature. Common causes: seasonality, content catalog changes, user cohort shifts. (2) Prediction drift: distribution of model output scores (should be relatively stable week-to-week). A KS-test p-value < 0.01 between this week's and last week's score distributions triggers investigation. (3) Outcome drift: CTR and watch fraction trends. A 7-day moving average below the rolling 30-day average by > 5% triggers a retraining alert. We also track "concept drift" specifically: if the model's top-10 features (by SHAP value) change ranking significantly between monthly evaluations, the feature space has shifted and we schedule an architectural review.

**Q15: How would you redesign this system if items were ephemeral — like Stories or Reels that expire in 24–48 hours?**

A: Ephemeral content fundamentally changes the time-sensitivity requirements. (1) Freshness decay is aggressive: `freshness = max(0, 1 - age_hours / 48)` — an item expires in 48 hours. The scoring model must heavily weight freshness. (2) Batch candidate generation is insufficient: the full ANN index would need rebuilding every few hours as new stories appear. We replace the nightly FAISS rebuild with a hot delta approach: a separate "stories FAISS index" containing only the last 48 hours of stories is rebuilt every 30 minutes. This index is small (hours of stories = thousands to millions of items, not 100 M) and can be rebuilt in < 2 minutes on 8 CPU cores. (3) No long-term user history is needed: session-based recommendation (transformer over last 20 actions in the current session) outperforms batch-trained CF for ephemeral content because the content itself has no historical signal. (4) Creator boost: for ephemeral content, users care strongly about creator identity ("I want to see what person X posted today"). The creator embedding and user's creator affinity score become the dominant features. (5) Impressions must be carefully deduplicated: since stories expire, showing the same story twice in a session is a severe UX failure. We maintain a per-session seen-story set in the serving layer's in-process cache.

---

## 12. References & Further Reading

1. **Covington, P., Adams, J., & Sargin, E. (2016).** Deep Neural Networks for YouTube Recommendations. *ACM RecSys 2016.* https://dl.acm.org/doi/10.1145/2959100.2959190
   — The canonical paper on candidate generation + ranking two-stage architecture and training with implicit feedback.

2. **Wang, R., Fu, B., Fu, G., & Wang, M. (2017).** Deep & Cross Network for Ad Click Predictions. *ADKDD @ KDD 2017.* https://arxiv.org/abs/1708.05123
   — Original DCN paper; DCN-v2 available at https://arxiv.org/abs/2008.13535

3. **Johnson, J., Douze, M., & Jégou, H. (2021).** Billion-Scale Similarity Search with GPUs. *IEEE Transactions on Big Data.* https://arxiv.org/abs/1702.08734
   — FAISS: algorithm details for IVF_PQ indexing and ANN search at billion scale.

4. **Cheng, H. et al. (2016).** Wide & Deep Learning for Recommender Systems. *DLRS @ RecSys 2016.* https://arxiv.org/abs/1606.07792
   — Wide & Deep model architecture; predecessor to DCN.

5. **Yi, X. et al. (2019).** Sampling-Bias-Corrected Neural Modeling for Large Corpus Item Recommendations. *ACM RecSys 2019.* https://dl.acm.org/doi/10.1145/3298689.3346996
   — Google's two-tower model with in-batch softmax and frequency correction; basis for our candidate generation.

6. **Zhao, Z. et al. (2019).** Recommending What Video to Watch Next: A Multitask Ranking System. *ACM RecSys 2019.* https://dl.acm.org/doi/10.1145/3298689.3346997
   — YouTube's multi-objective ranking with position bias correction and implicit feedback handling.

7. **Ma, J. et al. (2018).** Modeling Task Relationships in Multi-task Learning with Multi-gate Mixture-of-Experts. *ACM KDD 2018.* https://dl.acm.org/doi/10.1145/3219819.3220007
   — MMoE for multi-objective optimization in recommender systems (Google).

8. **Amatriain, X., & Basilico, J. (2015).** Recommender Systems in Industry: A Netflix Case Study. In *Recommender Systems Handbook (2nd ed.),* Springer.
   — Netflix's recommendation architecture evolution; A/B testing methodology.

9. **Eksombatchai, C. et al. (2018).** Pixie: A System for Recommending 3+ Billion Items to 200+ Million Users in Real-Time. *ACM WWW 2018.* https://dl.acm.org/doi/10.1145/3178876.3186183
   — Pinterest's graph-based real-time recommendation system.

10. **Pal, A. et al. (2020).** Pinnability: Machine Learning in the Pinterest Home Feed. *ACM RecSys 2020.*
    — Feature engineering and home feed ranking at Pinterest scale.

11. **Sculley, D. et al. (2015).** Hidden Technical Debt in Machine Learning Systems. *NeurIPS 2015.* https://proceedings.neurips.cc/paper/2015/hash/86df7dcfd896fcaf2674f757a2463eba-Abstract.html
    — Training-serving skew, feedback loops, and ML system design anti-patterns.

12. **Guo, H. et al. (2017).** DeepFM: A Factorization-Machine based Neural Network for CTR Prediction. *IJCAI 2017.* https://arxiv.org/abs/1703.04247
    — DeepFM for implicit feature interaction modeling in CTR prediction.

13. **Deng, Y. et al. (2023).** Feature Store for Machine Learning. *Meta Engineering Blog.*
    — Practical feature store design (Feast, Tecton, proprietary systems) for real-time ML serving.

14. **Steck, H. (2018).** Calibrated Recommendations. *ACM RecSys 2018.* https://dl.acm.org/doi/10.1145/3240323.3240372
    — Diversity and calibration in recommendation lists; mathematical framework for category entropy.

15. **Chen, M. et al. (2019).** Top-K Off-Policy Correction for a REINFORCE Recommender System. *WSDM 2019.* https://dl.acm.org/doi/10.1145/3289600.3290999
    — Reinforcement learning for recommendations; off-policy correction for logged bandit feedback.
