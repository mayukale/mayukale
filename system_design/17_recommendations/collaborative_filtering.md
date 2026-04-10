# System Design: Collaborative Filtering System

---

## 1. Requirement Clarifications

### Functional Requirements

1. **User-based CF**: Given a user U, compute the N most similar users and recommend items those users liked that U has not yet seen.
2. **Item-based CF**: Given a user U's interaction history, for each interacted item find the K most similar items, aggregate and rank them.
3. **Matrix Factorization (MF) CF**: Decompose the user-item interaction matrix into latent factors; serve top-N recommendations from learned factor dot products.
4. **Explicit and implicit feedback**: Support star ratings (explicit, 1–5) and behavioral signals (implicit: views, clicks, purchases, dwell time) as separate model variants.
5. **Real-time feedback ingestion**: New interactions must be reflected in serving (via near-real-time feature updates) within 5 minutes; full model retrain within 24 hours.
6. **Batch pre-computation**: Pre-compute user-user and item-item similarity tables nightly for neighborhood-based CF serving.
7. **Model-based serving**: Serve MF/ALS-trained user and item factors for dot-product scoring at request time.
8. **Multi-domain support**: Single platform serves CF across domains (movies, books, music) with domain-specific models sharing infrastructure.

### Non-Functional Requirements

1. **Latency**: P99 serving < 50 ms for neighborhood-based CF (pre-computed); P99 < 100 ms for MF-based (factor lookup + scoring).
2. **Throughput**: Handle 200,000 recommendation RPS at peak.
3. **Availability**: 99.99% uptime. Pre-computed results ensure serving survives training pipeline failures.
4. **Scale**: Support 500 M users, 100 M items, 50 B interaction events in the interaction matrix.
5. **Matrix density**: Interaction matrix is extremely sparse — average user has interacted with 200 items → density = 200 / 100 M = 0.0002% (2 × 10⁻⁶). Algorithms must handle this gracefully.
6. **Staleness tolerance**: Similarity tables may be up to 24 hours stale; MF factors up to 24 hours stale with streaming updates narrowing gap to 5 minutes.
7. **Cold-start**: Must serve meaningful results with 0–10 interactions (see cold-start handling section).
8. **Explainability**: Item-based CF must support human-readable explanations ("Because you liked X").

### Out of Scope

- Content-based filtering (handled by a separate pipeline; CF results may be blended with it upstream).
- Social graph recommendations (follow/friend suggestions are a separate graph traversal system).
- Real-time streaming CF (like session-based transformers) — focus is classical CF methods.
- Ad serving; this is organic CF only.

---

## 2. Users & Scale

### User Types

| User Type | Interaction Profile | CF Relevance |
|---|---|---|
| **Power users** | > 500 rated/watched items | Rich CF signal; high-quality neighbors/factors |
| **Active users** | 50–500 interactions | Good CF signal; primary target |
| **Casual users** | 10–50 interactions | Moderate signal; CF blended with popularity |
| **Cold users** | 0–10 interactions | Cold-start regime; CF largely inapplicable |
| **Domain specialists** | High interactions in one domain | Cross-domain CF may hurt; domain-isolated model preferred |

### Traffic Estimates

Assumption: Platform with Netflix/Spotify-class scale. 500 M registered users, 200 M daily active users (DAU).

| Metric | Calculation | Result |
|---|---|---|
| DAU | Assumption | 200 M |
| CF recommendation requests per user per day | 3 page loads × 2 refreshes = 6 | 6 |
| Daily CF API requests | 200 M × 6 | 1.2 B/day |
| Average RPS | 1.2 B / 86,400 | ~13,889 RPS |
| Peak multiplier (prime-time 4× avg) | 13,889 × 4 | ~55,556 RPS |
| Safety headroom (2×) | 55,556 × 2 | **~111,000 RPS** (round to 100K for planning) |
| New interactions ingested per day | 200 M users × 15 interactions | 3 B events/day |
| Interaction ingestion RPS | 3 B / 86,400 | ~34,722 RPS |
| Batch training data size | 500 M users × 200 interactions avg = 100 B rows | — |
| Sparsity of interaction matrix | 200 / 100 M = 0.000002 | 99.9998% sparse |
| Non-zero entries in matrix | 500 M × 200 = 100 B (100 billion) | — |

### Latency Requirements

| Component | Target P50 | Target P99 | Notes |
|---|---|---|---|
| End-to-end CF API | 15 ms | 50 ms | Neighborhood-based (pre-computed) |
| End-to-end CF API (MF-based) | 20 ms | 100 ms | Factor lookup + dot product scoring |
| Item-item similarity lookup | 2 ms | 10 ms | Redis hash or sorted set |
| User-user neighbor fetch | 3 ms | 15 ms | Redis sorted set (top-K neighbors) |
| MF factor lookup (user + 500 items) | 5 ms | 20 ms | Redis batch get |
| Dot product scoring (500 items × 128 dims) | 0.5 ms | 2 ms | NumPy vectorized ops in-process |
| ALS incremental update (streaming) | — | 5 min (eventual) | Spark Structured Streaming |

### Storage Estimates

| Data Type | Size | Count | Total |
|---|---|---|---|
| Raw interaction log | 150 B/event | 100 B events (all time) | 15 TB |
| User-user similarity table (top-500 per user) | (8 B user_id + 4 B score) × 500 = 6 KB/user | 500 M users | 3 PB (too large — see design) |
| Item-item similarity table (top-200 per item) | (8 B item_id + 4 B score) × 200 = 2.4 KB/item | 100 M items | 240 GB |
| User MF factors (128-dim float32) | 512 B/user | 500 M users | 256 GB |
| Item MF factors (128-dim float32) | 512 B/item | 100 M items | 51 GB |
| User biases (float32) | 4 B/user | 500 M | 2 GB |
| Item biases (float32) | 4 B/item | 100 M | 400 MB |
| Interaction matrix (sparse CSR) | 12 B/entry (row, col, val) × 100 B entries | — | 1.2 TB |
| Pre-computed CF recommendation cache | 20 items × 12 B × 200 M active users | — | 48 GB |

**Key insight on user-user similarity table:** Storing all 500 M × 500 top neighbors = 3 PB is infeasible. Decision: user-user CF is served at query-time using the MF-based approach (dot product of factors), not from a pre-computed table. Only item-item similarity is pre-computed (240 GB — fits in Redis Cluster).

### Bandwidth Estimates

| Flow | Rate | Bandwidth |
|---|---|---|
| Interaction ingestion | 34,722 RPS × 150 B | ~5.2 MB/s |
| CF API responses | 100,000 RPS × 20 items × 200 B | ~400 MB/s |
| Feature store reads (factors + similarities) | 100,000 RPS × 2 KB | ~200 MB/s |
| Batch training data scan | 15 TB over 6-hour nightly window | ~694 MB/s |
| Model artifact deployment (factor tables) | 256 GB + 51 GB = 307 GB per retrain | 85 MB/s for 1-hour upload |

---

## 3. High-Level Architecture

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                               CLIENT LAYER                                    │
│   Mobile / Web / TV / API Partner                                             │
└──────────────────────┬────────────────────────────┬──────────────────────────┘
                       │ GET /cf/recommendations      │ POST /interactions
                       ▼                             ▼
┌───────────────────────────────────────────────────────────────────────────────┐
│                    API GATEWAY  (Auth, Rate Limit, Routing)                   │
└──────────────────────────────────────┬────────────────────────────────────────┘
                                       │
              ┌────────────────────────┼───────────────────────────┐
              ▼                        ▼                           ▼
┌─────────────────────┐  ┌─────────────────────────┐  ┌──────────────────────┐
│  CF SERVING SERVICE │  │  INTERACTION INGESTION  │  │  ADMIN / EXPERIMENT  │
│                     │  │  SERVICE                │  │  SERVICE             │
│  Routes to:         │  │                         │  │  (A/B, config mgmt)  │
│  - Item-item CF     │  │  Validate + deduplicate │  └──────────────────────┘
│  - User-user CF     │  │  Publish to Kafka        │
│  - MF/ALS CF        │  └────────────┬────────────┘
└──────┬──────────────┘               │
       │                              ▼
       │                ┌─────────────────────────────────────────┐
       │                │      KAFKA EVENT STREAM                 │
       │                │  Topics:                                │
       │                │  cf.interactions.explicit  (ratings)    │
       │                │  cf.interactions.implicit  (behaviors)  │
       │                └──────────────┬──────────────────────────┘
       │                               │
       │          ┌────────────────────┼─────────────────────────────┐
       │          ▼                    ▼                             ▼
       │  ┌──────────────┐   ┌──────────────────────┐  ┌────────────────────────┐
       │  │  STREAM       │   │  BATCH TRAINING      │  │  BATCH SIMILARITY      │
       │  │  PROCESSOR    │   │  PIPELINE            │  │  COMPUTATION           │
       │  │  (Flink)      │   │  (Spark + ALS)       │  │  PIPELINE (Spark)      │
       │  │               │   │                      │  │                        │
       │  │  Update user  │   │  - ALS factorization │  │  - Item-item cosine    │
       │  │  interaction  │   │  - Factor export     │  │  - User-user approx.   │
       │  │  recency feats│   │  - Bias computation  │  │  - Co-occurrence stats │
       │  └──────┬────────┘   └──────────┬───────────┘  └──────────┬─────────────┘
       │         │                       │                          │
       │         └───────────────────────┼──────────────────────────┘
       │                                 ▼
       │          ┌──────────────────────────────────────────────────────────┐
       │          │                  STORAGE LAYER                           │
       │          │                                                          │
       │          │  ┌──────────────────┐  ┌──────────────────────────────┐ │
       │          │  │  Redis Cluster   │  │  S3 + Parquet (Cold Store)   │ │
       │          │  │                  │  │                              │ │
       │          │  │  - User factors  │  │  - Full interaction matrix   │ │
       │          │  │  - Item factors  │  │  - Historical factors        │ │
       │          │  │  - Item-item sim │  │  - Training snapshots        │ │
       │          │  │  - User top-N    │  │  - Audit logs                │ │
       │          │  │    cache         │  └──────────────────────────────┘ │
       │          │  └──────────────────┘                                   │
       │          │  ┌──────────────────┐  ┌──────────────────────────────┐ │
       │          │  │  ClickHouse      │  │  PostgreSQL                  │ │
       │          │  │  (interaction    │  │  (item catalog,              │ │
       │          │  │   event log)     │  │   user metadata)             │ │
       │          │  └──────────────────┘  └──────────────────────────────┘ │
       │          └──────────────────────────────────────────────────────────┘
       │
       └──────────────────────────────────────────────────────────────────────▶
                              (serve results from pre-computed stores)
```

**Component Roles:**

- **CF Serving Service**: Routes each request to the appropriate CF variant (item-item, user-user, or MF-based) based on the surface, user history richness, and A/B experiment bucket. Stateless; horizontally scalable.
- **Interaction Ingestion Service**: Validates event schema, deduplicates via event_id Bloom filter, normalizes implicit signals (e.g., converts dwell_time_ms → 0–1 normalized score), publishes to Kafka.
- **Kafka Event Stream**: Two topics — explicit ratings (low volume, high signal) and implicit behaviors (high volume, noisier signal). Separate consumers allow different processing strategies per signal type.
- **Stream Processor (Flink)**: Maintains per-user sliding windows of interaction recency scores for near-real-time feature updates. Does NOT retrain the CF model in real-time (too expensive); instead updates user "freshness vectors" to bias serving toward recent interests.
- **Batch Training Pipeline (Spark ALS)**: Runs nightly on the full interaction matrix snapshot from ClickHouse. Produces user_factors, item_factors, user_biases, item_biases. Exports to S3 and triggers Redis refresh.
- **Batch Similarity Computation Pipeline (Spark)**: Runs nightly; computes item-item cosine similarity over item factor vectors (or raw co-occurrence for the neighborhood-based variant). Writes top-200 similar items per item to Redis.
- **Redis Cluster**: Online serving store. Holds all serving-time data: user factors, item factors, item-item similarity tables, pre-computed user top-N caches. Primary low-latency retrieval layer.
- **ClickHouse**: Append-only interaction event log. Serves as the source of truth for batch training jobs. Partitioned by month for efficient historical scans.
- **S3 + Parquet**: Persistent cold store for all training artifacts, factor snapshots, and audit data. Version-controlled; supports rollback to any nightly snapshot.

**Primary Use-Case Data Flow (Item-Based CF):**

```
1. User opens "More Like This" page for item X.
2. API Gateway validates JWT → routes to CF Serving Service.
3. Serving Service checks approach: item-item CF selected.
4. Fetch user's recent interaction history from Redis:
   key: cf:user_history:{user_id} → last 20 item_ids user interacted with (including X).
5. For each of the 20 history items, fetch top-200 similar items:
   HGETALL cf:item_sim:{item_id} → {similar_item_id: similarity_score, ...}
   (Redis pipeline, batch call → ~8 ms for 20 × 200 = 4,000 items)
6. Aggregate scores: for each candidate, sum(sim(history_item, candidate) × weight(history_item))
   where weight decays by recency: weight = 1 / (1 + age_days_of_interaction)
7. Filter: remove items already in user's interaction history.
8. Sort by aggregated score, return top-20.
9. Log impression to Kafka.
Total wall clock: ~20 ms P50.
```

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────────────────────
-- INTERACTIONS (primary CF training data — append-only)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE interactions (
    event_id        UUID            NOT NULL,
    user_id         BIGINT          NOT NULL,
    item_id         BIGINT          NOT NULL,
    domain          VARCHAR(30)     NOT NULL,   -- 'movies', 'music', 'books'
    signal_type     VARCHAR(20)     NOT NULL,   -- 'rating','view','click','purchase',
                                                -- 'skip','dwell','share'
    -- Explicit signal
    rating          SMALLINT,       -- 1–5 for explicit; NULL for implicit
    -- Implicit signal normalization
    implicit_score  FLOAT,          -- normalized 0.0–1.0 (derived from raw signal)
    dwell_time_ms   INT,            -- raw dwell time for computing implicit_score
    -- Metadata
    session_id      VARCHAR(64),
    platform        VARCHAR(20),
    event_ts        TIMESTAMP       NOT NULL,
    -- Partition key
    PRIMARY KEY (event_id, event_ts)
) ENGINE = ReplicatedMergeTree()  -- ClickHouse syntax
  PARTITION BY toYYYYMM(event_ts)
  ORDER BY (user_id, item_id, event_ts);

CREATE INDEX idx_interactions_user ON interactions (user_id) TYPE minmax;
CREATE INDEX idx_interactions_item ON interactions (item_id) TYPE minmax;

-- ─────────────────────────────────────────────────────────────
-- ITEMS (catalog)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE items (
    item_id         BIGINT PRIMARY KEY,
    domain          VARCHAR(30),
    title           VARCHAR(500),
    category        VARCHAR(100),
    subcategory     VARCHAR(100),
    tags            TEXT[],
    creator_id      BIGINT,
    publish_date    TIMESTAMP,
    is_active       BOOLEAN DEFAULT TRUE,
    created_at      TIMESTAMP,
    updated_at      TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- USERS (subset of user service data)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE users (
    user_id         BIGINT PRIMARY KEY,
    domain_prefs    VARCHAR(30)[],  -- preferred domains
    country_code    CHAR(2),
    account_age_days INT,
    created_at      TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- USER FACTORS (written nightly by ALS training pipeline)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_factors (
    user_id         BIGINT          NOT NULL,
    domain          VARCHAR(30)     NOT NULL,
    factors         FLOAT4[128],    -- ALS latent factor vector (binary in practice)
    bias            FLOAT4,         -- user bias term bᵤ
    model_version   VARCHAR(30),
    computed_at     TIMESTAMP,
    PRIMARY KEY (user_id, domain)
);

-- ─────────────────────────────────────────────────────────────
-- ITEM FACTORS (written nightly by ALS training pipeline)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE item_factors (
    item_id         BIGINT          NOT NULL,
    domain          VARCHAR(30)     NOT NULL,
    factors         FLOAT4[128],
    bias            FLOAT4,
    model_version   VARCHAR(30),
    computed_at     TIMESTAMP,
    PRIMARY KEY (item_id, domain)
);

-- ─────────────────────────────────────────────────────────────
-- ITEM-ITEM SIMILARITY (top-K per item, written by Spark pipeline)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE item_item_similarity (
    item_id         BIGINT          NOT NULL,
    domain          VARCHAR(30)     NOT NULL,
    similar_items   JSONB,          -- {"item_id_1": sim_score_1, ..., "item_id_200": sim_score_200}
    algorithm       VARCHAR(30),    -- 'cosine_als_factors', 'jaccard_cooccurrence', 'adjusted_cosine'
    model_version   VARCHAR(30),
    computed_at     TIMESTAMP,
    PRIMARY KEY (item_id, domain)
);

-- ─────────────────────────────────────────────────────────────
-- USER INTERACTION SUMMARY (materialized, updated by Flink)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_interaction_summary (
    user_id                 BIGINT          NOT NULL,
    domain                  VARCHAR(30)     NOT NULL,
    -- Interaction counts
    total_interactions      INT,
    interactions_7d         INT,
    interactions_30d        INT,
    -- Recency
    last_interaction_ts     TIMESTAMP,
    -- Top-K recent items (for item-item CF pivot)
    recent_item_ids         BIGINT[],       -- last 20 items interacted with
    recent_item_weights     FLOAT4[],       -- recency-decayed weights for each
    -- Quality signals
    avg_explicit_rating     FLOAT,          -- if available
    has_sufficient_history  BOOLEAN,        -- TRUE if total_interactions >= 10
    updated_at              TIMESTAMP,
    PRIMARY KEY (user_id, domain)
);

-- ─────────────────────────────────────────────────────────────
-- CF SERVING LOG (for offline analysis + A/B)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE cf_serving_log (
    request_id      VARCHAR(64)     PRIMARY KEY,
    user_id         BIGINT,
    domain          VARCHAR(30),
    cf_variant      VARCHAR(30),    -- 'item_item', 'user_user', 'als_mf'
    model_version   VARCHAR(30),
    experiment_bucket VARCHAR(50),
    returned_items  BIGINT[],
    scores          FLOAT4[],
    served_at       TIMESTAMP
) PARTITION BY RANGE (served_at);
```

**Redis Key Patterns:**

```
# Item-item CF
cf:item_sim:{domain}:{item_id}   → ZSET  similar_item_id → similarity_score  (TTL 25h)

# User interaction history (for item-item pivot)
cf:user_history:{domain}:{user_id} → ZSET  item_id → timestamp  (top-50 by recency, TTL 1h)

# ALS factors
cf:user_factors:{domain}:{user_id} → HSET  {vec: <binary 512B>, bias: 0.23, ver: "v8"}  (TTL 25h)
cf:item_factors:{domain}:{item_id} → HSET  {vec: <binary 512B>, bias: -0.05, ver: "v8"} (TTL 25h)

# Pre-computed top-N for active users (optional cache)
cf:user_topn:{domain}:{user_id}    → ZSET  item_id → predicted_score  (top-100, TTL 2h)

# Popularity fallback (cold-start)
cf:popular:{domain}:global         → ZSET  item_id → interaction_count  (TTL 1h)
cf:popular:{domain}:{country_code} → ZSET  item_id → interaction_count  (TTL 1h)
```

### Database Choice

| Concern | Options | Selected | Justification |
|---|---|---|---|
| **Interaction log (append-only, training source)** | ClickHouse, Cassandra, PostgreSQL partitioned | **ClickHouse** | Columnar storage compresses 100 B rows to ~1.5 TB (10× compression); aggregation queries for ALS data loading run 50–100× faster than row-store PostgreSQL; handles 34K events/s write throughput natively |
| **Online factors + similarity (serving)** | Redis, DynamoDB, Aerospike | **Redis Cluster** | User and item factors are 512 B binary blobs — Redis HGET retrieves them in 0.1–0.3 ms; ZSET for item_sim provides ranked retrieval in O(log N) per query; 256 GB + 51 GB factor storage fits in a 32-shard cluster with 24 GB/shard |
| **Item metadata + catalog** | PostgreSQL, DynamoDB, MySQL | **PostgreSQL** | CF serving needs join between item_ids and metadata (title, category) for response enrichment; PostgreSQL array types store tags naturally; JSONB for flexible item metadata; strong consistency for catalog mutations |
| **Batch training (MF/ALS)** | Spark MLlib ALS, Implicit (Python), PyTorch | **Spark MLlib ALS** | Native distributed ALS on Spark handles 100 B non-zero matrix entries via block-partitioned matrix operations; scales horizontally; integrates with existing Spark data lake; Implicit library is single-node and won't handle 1.2 TB matrix |
| **Feature storage (offline snapshots)** | S3 + Parquet, BigQuery, Snowflake | **S3 + Delta Lake** | Point-in-time correct reads for training; time-travel enables reproducibility; Delta Lake ACID prevents partial writes corrupting factor tables; cost-effective vs managed warehouse at this data volume |

---

## 5. API Design

### Authentication
Bearer JWT required for all endpoints. Includes `user_id` claim and `domain` scope. Anonymous users get a session token with degraded (popularity-only) results.

### Endpoints

```
GET /v1/cf/recommendations
```
**Purpose:** Return personalized CF-based recommendations.

**Auth:** Bearer JWT

**Rate limit:** 1,000 requests/hour per user; 50/hour anonymous.

**Query parameters:**

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| domain | string | yes | — | `movies`, `music`, `books` |
| variant | string | no | `auto` | `item_item`, `user_mf`, `auto` (system chooses based on history richness) |
| n | int | no | 20 | Results to return (max 100) |
| exclude_seen | bool | no | true | Filter out already-interacted items |
| cursor | string | no | null | Pagination cursor for next page |

**Response:**
```json
{
  "request_id": "req_a1b2c3...",
  "user_id": 123456,
  "domain": "movies",
  "cf_variant": "item_item",
  "model_version": "als_v8.2",
  "items": [
    {
      "item_id": 9901234,
      "predicted_score": 0.891,
      "explanation": "Because you watched The Dark Knight",
      "pivot_item_id": 8800012,
      "similarity_score": 0.94
    }
  ],
  "next_cursor": "eyJ1c2VyX2lkIjo..."
}
```

**Explanation field:** Populated only for item-item CF variant (maps back to the pivot item that generated this recommendation). MF variant returns `null` explanation (no interpretable pivot).

---

```
POST /v1/interactions
```
**Purpose:** Ingest a user interaction event.

**Auth:** Bearer JWT

**Rate limit:** 10,000 events/hour per user.

**Request body:**
```json
{
  "event_id": "evt_uuid_v4",
  "item_id": 9901234,
  "domain": "movies",
  "signal_type": "rating",
  "rating": 4,
  "dwell_time_ms": null,
  "session_id": "sess_abc",
  "event_ts": "2026-04-09T20:00:00Z"
}
```

**Response:** `202 Accepted`. Asynchronous; event published to Kafka. Idempotent by `event_id`.

---

```
GET /v1/cf/similar-items/{item_id}
```
**Purpose:** Return items most similar to a given item (item-item CF).

**Auth:** Bearer JWT (optional; anonymous gets popularity-weighted similarity)

**Query params:** `domain` (required), `n` (default 10, max 50)

**Response:** List of `{item_id, similarity_score, explanation}` where explanation indicates the similarity basis (e.g., "Co-watched by 45,000 users").

---

```
GET /v1/cf/similar-users/{user_id}
```
**Purpose:** Return users most similar to the given user (user-user CF). **Internal/admin only** — not exposed to end users (privacy).

**Auth:** Bearer JWT with `role: internal`

**Response:** List of `{user_id_hash, similarity_score}` — user_ids are hashed for privacy.

---

```
GET /v1/cf/explain/{user_id}/{item_id}
```
**Purpose:** Explain why item X was recommended to user U.

**Auth:** Bearer JWT (user must match user_id or have admin role)

**Response:**
```json
{
  "item_id": 9901234,
  "cf_variant": "item_item",
  "explanation": {
    "pivot_items": [
      {"item_id": 8800012, "title": "The Dark Knight", "similarity": 0.94},
      {"item_id": 8800099, "title": "Inception", "similarity": 0.87}
    ],
    "aggregated_score": 0.891
  }
}
```

---

```
POST /v1/admin/models/retrain
```
**Purpose:** Trigger an out-of-schedule model retrain (e.g., after data quality incident).

**Auth:** Bearer JWT with `role: admin`

**Request body:** `{"domain": "movies", "reason": "data_quality_fix", "requested_by": "user@company.com"}`

**Response:** `202 Accepted` with `{"job_id": "airflow_run_xyz", "estimated_completion": "2026-04-09T22:00:00Z"}`

---

## 6. Deep Dive: Core Components

### 6.1 Collaborative Filtering Algorithms: Neighborhood vs. Model-Based

**Problem it solves:** Choosing the right CF algorithm determines recommendation quality, serving latency, explainability, and scalability. Each algorithm has fundamentally different computational properties that must match system constraints.

#### Approaches Comparison

| Algorithm | Recommendation Quality | Scalability | Serving Latency | Explainability | Cold-Start | Training Cost |
|---|---|---|---|---|---|---|
| **User-based CF (exact cosine)** | High for dense users | O(U²) space — doesn't scale past 10 M users | High (real-time NN search) | Medium | Poor | O(U² × I) |
| **User-based CF (ANN approx.)** | Good | O(U × K) with FAISS | Medium (10–50 ms) | Medium | Poor | O(U × F) for FAISS |
| **Item-based CF (pre-computed cosine)** | High | O(I² × K) pre-compute; O(1) serving | Very low (pre-computed) | High ("Because you liked X") | Poor for new items | O(I² × U) |
| **Item-based CF (ALS factor cosine)** | Higher | O(I × F) storage, O(I log I) pre-compute | Low (pre-computed) | Medium | Better (content cold-start) | O(ALS training) |
| **SVD (truncated)** | Medium-High | O((U+I) × F) | Very low (dot product) | Low | Poor | O(U × I × F) per iteration |
| **ALS (Alternating Least Squares)** | High | Distributed; scales to 100B+ entries | Very low (dot product) | Low | Medium (use content for init) | O(ALS iterations × nnz) |
| **BPR (Bayesian Personalized Ranking)** | High for implicit | Good | Very low | Low | Poor | O(samples × F) |
| **Neural CF (NeuMF)** | Very High | Large model; GPU needed | Medium (10–30 ms) | Very Low | Poor | Very High |

**Selected Approach: Item-based CF (primary) + ALS Matrix Factorization (secondary)**

We use item-item CF as the primary variant because: (1) It produces explainable recommendations, which is critical for user trust (2) Pre-computation means serving is O(1) from Redis (3) Item similarities are stable — items don't change their behavior as rapidly as users do. ALS is the secondary variant for power users with deep history who benefit from the holistic latent factor representation.

---

### 6.2 ALS Matrix Factorization — Deep Dive

**Problem it solves:** User-item interaction matrices are 99.9998% sparse. Standard SVD cannot handle missing entries — it treats them as zeros, which is wrong (a missing entry means "not seen", not "disliked"). ALS solves the matrix completion problem by learning latent factors only from observed entries, while regularizing to avoid overfitting.

**Mathematical Foundation:**

The rating matrix R (U × I) is approximated as:
```
R ≈ P × Qᵀ + bᵤ + bᵢ + μ
```
where:
- P ∈ ℝ^(U×F): user factor matrix (U users, F factors)
- Q ∈ ℝ^(I×F): item factor matrix (I items, F factors)
- bᵤ ∈ ℝ^U: user bias vector (captures users who rate generously or harshly)
- bᵢ ∈ ℝ^I: item bias vector (captures inherently popular/unpopular items)
- μ ∈ ℝ: global mean rating

**Objective for explicit feedback (ratings):**

```
min_{P,Q,b} Σ_{(u,i) ∈ observed} (rᵤᵢ - μ - bᵤ - bᵢ - pᵤ · qᵢ)² 
            + λ(||pᵤ||² + ||qᵢ||² + bᵤ² + bᵢ²)
```

**ALS update rule** (fix Q, solve for P; then fix P, solve for Q — alternating):

```
pᵤ = (QᵀQ + λI)⁻¹ Qᵀ rᵤ
qᵢ = (PᵀP + λI)⁻¹ Pᵀ rᵢ
```

Each update is an independent least squares problem per user (or item), making ALS embarrassingly parallelizable.

**Implicit feedback adaptation (Hu et al., 2008):**

For implicit data (views, clicks), we define confidence cᵤᵢ = 1 + α × fᵤᵢ where fᵤᵢ is the raw interaction frequency (e.g., number of plays). The objective becomes:

```
min_{P,Q} Σ_{u,i} cᵤᵢ(pᵤᵢ - pᵤ · qᵢ)² + λ(||pᵤ||² + ||qᵢ||²)
```

where pᵤᵢ = 1 if user u interacted with item i, else 0 (preference binary indicator). The confidence weight cᵤᵢ is high for frequently consumed items (strong positive signal) and 1 for unobserved items (weak negative signal, not zero).

**Distributed ALS on Spark (implementation detail):**

```python
from pyspark.ml.recommendation import ALS
from pyspark.sql import SparkSession

spark = SparkSession.builder \
    .config("spark.executor.cores", "8") \
    .config("spark.executor.memory", "60g") \
    .config("spark.sql.shuffle.partitions", "2000") \
    .getOrCreate()

# Load last 90 days of interactions
interactions_df = spark.read \
    .format("clickhouse") \
    .option("query", """
        SELECT user_id, item_id, 
               -- For implicit: use log-scaled interaction count as rating
               log1p(COUNT(*)) AS rating
        FROM interactions
        WHERE domain = 'movies'
          AND event_ts >= now() - INTERVAL 90 DAY
          AND signal_type IN ('view', 'click', 'purchase', 'rating')
        GROUP BY user_id, item_id
    """) \
    .load()

# Remap user_id and item_id to contiguous integers (ALS requires int)
# (omitting remapping code for brevity)

als = ALS(
    maxIter=15,          # 15 ALS iterations (convergence typically by iter 10)
    rank=128,             # F=128 latent factors
    regParam=0.1,         # λ regularization
    implicitPrefs=True,   # use confidence-weighted implicit feedback
    alpha=40.0,           # α for confidence: c = 1 + 40 * f
    userCol="user_id",
    itemCol="item_id",
    ratingCol="rating",
    numUserBlocks=200,    # parallelism: 200 user blocks
    numItemBlocks=200,    # 200 item blocks
    intermediateStorageLevel="DISK_ONLY",  # prevent OOM on 100B matrix
    finalStorageLevel="MEMORY_AND_DISK"
)

model = als.fit(interactions_df)

# Extract factors
user_factors_df = model.userFactors   # (user_id, features: array<float>)
item_factors_df = model.itemFactors   # (item_id, features: array<float>)

# Write factors to S3 as Parquet
user_factors_df.write.mode("overwrite").parquet("s3://ml-models/als/v8/user_factors/")
item_factors_df.write.mode("overwrite").parquet("s3://ml-models/als/v8/item_factors/")

# Push to Redis (see Redis loader job)
```

**ALS Convergence Properties:**

```
With F=128, λ=0.1, α=40:
- Iteration 1: RMSE ≈ 1.45 (on held-out ratings)
- Iteration 5: RMSE ≈ 0.89
- Iteration 10: RMSE ≈ 0.82
- Iteration 15: RMSE ≈ 0.81  ← converged
- Iteration 20: RMSE ≈ 0.81  ← no improvement

Compute: 100B non-zero entries × 15 iterations / (100 Spark executors × 8 cores)
= 100B × 15 / 800 = 1.875B operations per core
≈ 2.5 hours on 100 c5.2xlarge executors
```

**Serving (ALS-based recommendation):**

```python
def als_recommend(user_id: int, domain: str, n: int = 20) -> List[Tuple[int, float]]:
    # 1. Fetch user vector (Redis, ~0.5 ms)
    user_vec_bytes = redis.hget(f"cf:user_factors:{domain}:{user_id}", "vec")
    user_bias = float(redis.hget(f"cf:user_factors:{domain}:{user_id}", "bias"))
    
    if user_vec_bytes is None:
        return cold_start_fallback(user_id, domain, n)
    
    user_vec = np.frombuffer(user_vec_bytes, dtype=np.float32)  # shape (128,)
    
    # 2. Check pre-computed top-N cache first
    cached = redis.zrevrange(f"cf:user_topn:{domain}:{user_id}", 0, n-1, withscores=True)
    if cached:
        return [(int(item_id), score) for item_id, score in cached]
    
    # 3. Fetch candidate item factors (FAISS ANN or brute-force top-K)
    # For MF: use FAISS over item factor matrix (128-dim, 100M items)
    # Index built nightly from item_factors; same IVF_PQ approach as recommendation_engine
    candidates = faiss_item_index.search(user_vec.reshape(1, -1), 500)[1][0]
    
    # 4. Score candidates: predicted_rating = μ + bᵤ + bᵢ + pᵤ · qᵢ
    global_mean = 3.5  # precomputed constant
    scores = []
    item_bias_pipe = redis.pipeline()
    for item_id in candidates:
        item_bias_pipe.hget(f"cf:item_factors:{domain}:{item_id}", "bias")
    item_biases = item_bias_pipe.execute()
    
    item_vecs = fetch_item_factor_batch(candidates, domain)  # 500 × 128 matrix
    dot_products = item_vecs @ user_vec                       # 500 scores, vectorized
    
    for i, (item_id, dp) in enumerate(zip(candidates, dot_products)):
        b_i = float(item_biases[i] or 0.0)
        predicted = global_mean + user_bias + b_i + dp
        scores.append((item_id, predicted))
    
    # 5. Filter seen items
    user_history = get_user_interaction_set(user_id, domain)
    scores = [(item_id, s) for item_id, s in scores if item_id not in user_history]
    
    # 6. Sort and return top-N
    scores.sort(key=lambda x: -x[1])
    result = scores[:n]
    
    # 7. Cache result (2-hour TTL)
    pipe = redis.pipeline()
    for item_id, score in result:
        pipe.zadd(f"cf:user_topn:{domain}:{user_id}", {item_id: score})
    pipe.expire(f"cf:user_topn:{domain}:{user_id}", 7200)
    pipe.execute()
    
    return result
```

#### Interviewer Q&A

**Q1: How do you handle the 99.9998% sparsity in ALS without running out of memory?**

A: Sparsity is ALS's greatest strength — we only iterate over observed entries. The interaction matrix is never materialized as a dense matrix. In Spark ALS, the matrix is stored as a sparse RDD of (user_id, item_id, rating) triples. The key insight in the Hu et al. (2008) ALS-WR (weighted regularization) derivation is that the update step for user u only requires summing over items u has interacted with. For the implicit confidence case, the formula involves a per-user matrix Cᵤ that is sparse (only non-zero for items u consumed). Spark's block-partitioned ALS shuffles user and item blocks such that each executor handles a block of users and loads only the item factors needed for that user block — no full matrix materialization. Memory footprint per executor: 200 item_blocks / 200 num_item_blocks = 1 block, each block = 100M/200 = 500K items × 128 floats × 4 bytes = ~256 MB. Perfectly manageable in 60 GB executor RAM.

**Q2: ALS gives you latent factors — how do you interpret what they represent?**

A: ALS latent factors are not directly interpretable (they're not labeled "action movies" or "comedy"). However, we can inspect them post-hoc. We use two techniques: (1) For each latent dimension k, we rank items by their q_i[k] value and look at the top-20 items for that dimension. Often a coherent theme emerges — dimension 7 might be dominated by Christopher Nolan films; dimension 23 by French-language films. These are empirical observations, not guarantees. (2) We compute the correlation between each latent dimension and metadata features (genre, language, release year). High correlation (>0.7) tells us that the latent factor approximately captures that feature, providing an explanation post-hoc. For user-facing explanations, we still prefer item-item CF (which has a clear "Because you liked X" explanation) and use ALS only where quality is paramount and explainability is less critical (e.g., email digest where no explanation is shown).

**Q3: What's the difference between training ALS on explicit vs implicit feedback, and when do you use each?**

A: Explicit feedback (star ratings) has a clear target value (1–5). The objective is to minimize squared prediction error on observed ratings — missing entries are genuinely missing, not negative signals. Implicit feedback (views, clicks, plays) has no negative signal — we only know what a user consumed, not what they actively rejected. The challenge is constructing negative examples. The Hu et al. approach treats all non-interactions as weak negatives with confidence = 1 (vs strong positives with confidence = 1 + α × frequency). A user who watched a movie 5 times has confidence c = 1 + 40×5 = 201; a movie they never watched has c = 1. The model therefore trusts positive observations much more than the implicit "negative" of non-interaction. In practice: if your platform has both signals, train separate models (explicit for rating-heavy domains like movie/music where users rate explicitly; implicit for behavioral domains like e-commerce where purchases are the signal) and blend at serving time: `final_score = w_explicit × explicit_pred + (1 - w_explicit) × implicit_pred`. For users with < 5 ratings, w_explicit = 0 (not enough to calibrate).

**Q4: How does ALS handle users and items not seen during training (cold-start)?**

A: Out-of-vocabulary (OOV) users and items are a fundamental limitation of ALS. For new items: if an item was published after the last model training, it has no item factor vector. We handle this with a content-based proxy: use the item's content embedding (from a text/audio/image encoder) projected into the ALS latent space via a learned projection matrix W: `q_new ≈ W × content_embedding`. W is trained as a side model: for items with both an ALS factor and a content embedding, we regress ALS_factor = W × content_embedding. This gives a reasonable initialization for new items. For new users: at serving time, if a user is OOV, we initialize their factor vector as the weighted average of the factor vectors of items they've interacted with (using interaction recency as weights). This is equivalent to a single ALS update step for the new user given their observed interactions, without retraining the entire model.

**Q5: How would you perform incremental/online updates to the ALS model without full retraining?**

A: Full ALS retraining takes 2.5 hours and runs nightly. Between retrains, new interactions arrive continuously. We handle this at two levels: (1) Factor freeze + recency re-ranking: the pre-trained ALS factors don't change intra-day, but the serving pipeline boosts items the user consumed in the last 2 hours by applying a recency weight. In the candidate scoring step, items in the user's recent session get a +0.2 score additive boost. This simple heuristic captures the user's current intent without retraining. (2) Streaming ALS update (experimental): for power users generating many interactions per day, we run a lightweight streaming ALS that re-computes only that user's factor vector using their updated interaction set, keeping item factors fixed (only user factors vary intra-day). This is a single ALS update step per user: `pᵤ_new = (QᵀQ + λI)⁻¹ Qᵀ rᵤ_updated`. Implemented in Flink with a windowed trigger (every 30 minutes if user has > 5 new interactions). Updates user vector in Redis within 5 minutes of trigger. Full retrain each night still reconciles these streaming updates.

---

### 6.3 Item-Item Similarity Computation

**Problem it solves:** For item-based CF, we need to answer "given item X, what are the top-K most similar items?" at sub-millisecond serving latency. Computing similarity on-the-fly at serving time is O(I × F) = O(100M × 128) per request — impossible. We must pre-compute and cache these similarities.

#### Similarity Metrics Comparison

| Metric | Formula | Properties | When to Use |
|---|---|---|---|
| **Cosine similarity** | cos(A,B) = (A·B)/(‖A‖‖B‖) | Ignores magnitude; scale-invariant | General purpose; works on factor vectors |
| **Adjusted cosine** | Subtract user mean before computing cosine | Corrects for user rating bias | Explicit feedback / rating matrices |
| **Pearson correlation** | Similar to adjusted cosine; normalized covariance | Accounts for user bias; interpretable | Explicit ratings; neighborhood-based CF |
| **Jaccard similarity** | \|A∩B\| / \|A∪B\| (binary) | Simple; handles implicit; no negatives | Co-purchase, co-view implicit data |
| **Log-likelihood ratio (LLR)** | Based on co-occurrence significance test | Robust to popularity bias | Implicit data; large catalogs |
| **ALS factor cosine** | Cosine over learned ALS factor vectors | Captures latent structure; not just co-occurrence | When ALS model is available |

**Selected: ALS factor cosine (primary) + Jaccard for new items (fallback)**

**Pre-computation algorithm:**

```python
# Spark job: compute top-200 similar items per item using ALS factors
from pyspark.sql import functions as F
from pyspark.ml.feature import BucketedRandomProjectionLSH

# Load item factors computed by ALS
item_factors = spark.read.parquet("s3://ml-models/als/v8/item_factors/")
# Schema: (item_id: bigint, features: array<float>[128])

# Convert to ML Vector type
from pyspark.ml.linalg import Vectors, VectorUDT
to_vector = F.udf(lambda x: Vectors.dense(x), VectorUDT())
item_factors = item_factors.withColumn("features_vec", to_vector("features"))

# Option A: Brute-force for small catalogs (< 1M items) — O(I²)
# For 100M items: 100M² = 10^16 operations — NOT feasible

# Option B: LSH (Locality Sensitive Hashing) — O(I × K) approximate
lsh = BucketedRandomProjectionLSH(
    inputCol="features_vec",
    outputCol="hashes",
    bucketLength=2.0,
    numHashTables=3
)
lsh_model = lsh.fit(item_factors)

# For each item, find approximate nearest neighbors within cosine distance 0.3
# LSH reduces comparisons from O(I²) to O(I × bucket_size)
similar_items = lsh_model.approxSimilarityJoin(
    item_factors,
    item_factors,
    threshold=0.7,  # max distance (note: BRP uses Euclidean, not cosine; adjust)
    distCol="distance"
).filter("datasetA.item_id != datasetB.item_id")

# Convert Euclidean distance to cosine similarity (for unit vectors):
# cos_sim = 1 - (euclidean_dist² / 2)
similar_items = similar_items.withColumn(
    "cosine_similarity",
    1 - (F.col("distance") * F.col("distance") / 2)
)

# For each item, keep top-200 most similar
from pyspark.sql.window import Window
window = Window.partitionBy("datasetA.item_id").orderBy(F.desc("cosine_similarity"))
top_similar = similar_items \
    .withColumn("rank", F.rank().over(window)) \
    .filter(F.col("rank") <= 200) \
    .select(
        F.col("datasetA.item_id").alias("item_id"),
        F.col("datasetB.item_id").alias("similar_item_id"),
        F.col("cosine_similarity"),
        F.col("rank")
    )

# Aggregate into Redis-compatible format and write
# Final output: one row per item with top-200 similar items as JSON
top_similar_agg = top_similar.groupBy("item_id").agg(
    F.collect_list(
        F.struct("similar_item_id", "cosine_similarity")
    ).alias("similar_items")
)

top_similar_agg.write.mode("overwrite").parquet("s3://ml-models/als/v8/item_similarities/")

# Redis loader: iterate parquet, write ZADD for each item
# Estimated total: 100M items × 200 similar × 12 bytes = 240 GB Redis storage
```

**Serving item-item CF:**

```python
def item_item_cf_recommend(user_id: int, domain: str, n: int = 20) -> List[dict]:
    # 1. Get user's recent interaction history (Redis ZSET by timestamp)
    history_with_scores = redis.zrevrangebyscore(
        f"cf:user_history:{domain}:{user_id}",
        "+inf", "-inf",
        start=0, num=20,
        withscores=True   # scores are timestamps
    )
    
    if not history_with_scores:
        return cold_start_fallback(user_id, domain, n)
    
    # 2. Compute recency-decayed weight for each pivot item
    now_ts = time.time()
    pivot_items = []
    for item_id_bytes, ts in history_with_scores:
        item_id = int(item_id_bytes)
        age_days = (now_ts - ts) / 86400
        weight = 1.0 / (1.0 + age_days / 7.0)  # half-life of 7 days
        pivot_items.append((item_id, weight))
    
    # 3. Fetch similar items for all pivot items in one Redis pipeline (~8ms for 20 items)
    aggregated_scores = defaultdict(float)
    aggregated_pivots = defaultdict(list)
    
    pipe = redis.pipeline()
    for item_id, _ in pivot_items:
        # ZRANGEBYSCORE to get top-50 similar items per pivot
        pipe.zrevrangebyscore(
            f"cf:item_sim:{domain}:{item_id}",
            "+inf", "-inf",
            start=0, num=50,
            withscores=True
        )
    results = pipe.execute()
    
    for (item_id, weight), similar_list in zip(pivot_items, results):
        for similar_id_bytes, sim_score in similar_list:
            similar_id = int(similar_id_bytes)
            aggregated_scores[similar_id] += weight * sim_score
            aggregated_pivots[similar_id].append((item_id, sim_score))
    
    # 4. Filter already-seen items
    user_seen = set(int(i) for i, _ in history_with_scores)
    candidates = [(item_id, score) for item_id, score in aggregated_scores.items()
                  if item_id not in user_seen]
    
    # 5. Sort by aggregated score
    candidates.sort(key=lambda x: -x[1])
    
    # 6. Build result with explanations
    result = []
    for item_id, score in candidates[:n]:
        # Best pivot item for explanation
        best_pivot = max(aggregated_pivots[item_id], key=lambda x: x[1])
        result.append({
            "item_id": item_id,
            "predicted_score": round(score, 4),
            "pivot_item_id": best_pivot[0],
            "similarity_score": round(best_pivot[1], 4),
            "explanation": f"Because you watched item {best_pivot[0]}"
        })
    
    return result
```

#### Interviewer Q&A

**Q1: Why does item-item CF scale better than user-user CF?**

A: This is a fundamental property of the item catalog vs user base dynamics. In user-user CF, similarity must be computed between pairs of users. With 500 M users, the similarity matrix would have 500 M² / 2 ≈ 125 trillion entries — even storing just the top-500 neighbors per user requires 500 M × 500 × 12 bytes = 3 PB, which is infeasible. Additionally, user preferences drift over time — their neighborhood becomes stale quickly, requiring frequent recomputation. In contrast, item-item CF benefits from two structural advantages: (1) The item catalog is typically 100× smaller than the user base (100 M items vs 500 M users in our case), making the top-K similarity table 100 GB vs 3 PB. (2) Item similarities are stable over time — two items that co-appealed to users historically continue to do so (unless there's a major cultural shift), so nightly recomputation is sufficient vs the continuous recomputation user-user CF would require. The paper by Sarwar et al. (2001) "Item-based Collaborative Filtering Recommendation Algorithms" formally demonstrated this scaling advantage and it's why all production CF systems at scale use item-item or model-based approaches rather than user-user.

**Q2: How do you prevent popular items from dominating item-item similarity (popularity bias)?**

A: This is the "hub problem" in CF — a few extremely popular items (say, a blockbuster movie) appear as the "most similar" item for thousands of other items, because nearly every user has seen both. This creates a degenerate similarity table where all items point to the same 10 popular hubs. Two corrections: (1) **Inverse Document Frequency (IDF)-style weighting**: When computing co-occurrence-based similarity, weight each user's contribution by 1/log(n_u) where n_u is the number of items that user has interacted with. Prolific users (who interact with everything) contribute less signal per interaction. (2) **Cosine on ALS factors**: The ALS factorization implicitly handles popularity bias because the item bias term b_i absorbs the global popularity signal, leaving the factor vector q_i to capture only the latent preference pattern beyond simple popularity. Items that are popular-for-different-reasons will have dissimilar factor vectors even if they have high co-occurrence counts. In practice, we use ALS factor cosine as the primary similarity metric specifically to avoid this hub problem.

**Q3: How do you handle near-duplicate items in the similarity table? (e.g., two editions of the same book)**

A: Near-duplicate items create a poor user experience: if items A and A' are nearly identical, showing both in a "similar items" list wastes slots and annoys users. We handle deduplication at two levels. (1) **Catalog-level deduplication**: during item catalog ingestion, we compute content similarity (Jaccard on title tokens + publisher + ISBN prefix) and flag suspected duplicates. Flagged duplicates are merged into a canonical item_id before entering the CF pipeline. (2) **Similarity filter**: during the item-item similarity pre-computation job, if two items have cosine similarity > 0.98 AND the same category AND the same creator, we classify them as near-duplicates and exclude one from the other's similarity list. (3) **Diversity at serving time**: we apply the same MMR diversity filter to item-item CF results — preventing two items from the same creator appearing in positions 1 and 2.

**Q4: How would you extend this system to handle multi-domain CF (movies, books, music) where a user's taste in one domain should inform recommendations in another?**

A: Cross-domain CF is an active research area. Our approach: (1) **Shared embedding space**: during ALS training, instead of training separate models per domain, we train a unified model with domain-specific bias terms. The item factor q_i lives in the same 128-dimensional space regardless of domain. Users who read thriller novels and watch thriller films will have a user factor p_u that reflects "thrillers" as a latent preference, pointing toward both film and book items. (2) **Domain adaptation**: pure cross-domain transfer can hurt when a user has domain-specific preferences (e.g., they love jazz music but hate jazz-themed films). We add a domain context vector d_domain (64-dim) to the prediction: `score = (p_u + d_domain_user) · (q_i + d_domain_item)`. This allows domain-specific adjustments while preserving cross-domain signal. (3) **Transfer gating**: at serving time, the fraction of cross-domain signal used is gated by the user's history richness in the target domain. A user with 500 movie interactions and 0 book interactions gets 30% cross-domain signal in book recommendations. A user with 50 book interactions gets 5% cross-domain signal (sufficient history in target domain).

**Q5: What is the difference between memory-based and model-based CF, and when do you use each in production?**

A: Memory-based CF (neighborhood methods: user-user, item-item cosine) operates directly on the interaction matrix without learning a model. Recommendations are produced by looking up similar users/items in the raw interaction data or pre-computed similarity tables. Model-based CF (ALS, SVD, NeuMF) learns a compact model from the interaction matrix, then uses the model's predictions at serving time. The key production trade-offs: (1) **Explainability**: memory-based wins — you can always say "users who liked X also liked Y." Model-based (ALS) produces opaque latent factors. (2) **Freshness**: memory-based can incorporate new interactions immediately (just add to the interaction set and re-run the similarity aggregation in real-time). Model-based requires periodic retraining. (3) **Cold-start**: both suffer, but model-based can use content side-information during training (side-feature ALS), while memory-based requires interaction data. (4) **Quality**: model-based consistently wins on accuracy (RMSE, NDCG) because it generalizes from sparse data rather than relying on literal co-occurrence. (5) **Scale**: at 500 M users, user-user memory-based is infeasible. Item-item memory-based works if the item catalog is manageable (< 1 M items). Model-based scales to any size via distributed ALS. Production decision: we use item-item memory-based for explainability (and the item catalog is 100 M which pre-computes to 240 GB — manageable) and ALS model-based for quality on dense-history users, blending both at serving time.

---

## 7. Scaling

### Horizontal Scaling

**CF Serving Service**: Stateless pods; auto-scales by RPS metric. At 100K peak RPS with 50 ms P99 budget: each pod handles ~500 RPS → 200 minimum pods, 400 with safety headroom. Each pod: 4 vCPU, 8 GB RAM (no in-process index; all data in Redis). Pod startup: 5 seconds (no large index to load).

**Redis Cluster**: 32 shards for factor storage (256 GB user factors + 51 GB item factors + 240 GB item similarities = 547 GB). At 24 GB/shard: 32 × 24 GB = 768 GB capacity — 70% utilized. Each shard: master + 1 replica for read scaling. Reads load-balanced across master and replica (70% replica, 30% master).

**ClickHouse**: 8-shard cluster for interaction log. 100 B events × 150 B/event = 15 TB total. Per shard: 15 TB / 8 = ~1.9 TB. With 10× columnar compression: ~190 GB effective per shard. ReplicatedMergeTree with 2 replicas per shard.

**Spark Training Cluster**: AWS EMR cluster, auto-scaled. Nightly batch: 100 c5.4xlarge workers (16 vCPU, 32 GB each = 1,600 vCPU total). Training job uses 60 GB executor memory for ALS block operations. Cluster terminates after job completion to minimize cost.

### DB Sharding

| Store | Shard Key | Rationale |
|---|---|---|
| ClickHouse interactions | `sipHash64(user_id) % 8` | Even distribution; user-scoped queries are co-located |
| Redis (factors) | `CRC16(key) % 16384` (native cluster) | Automatic; item_sim and user_factors naturally distributed |
| Redis (item similarity) | `CRC16("cf:item_sim:{domain}:{item_id}") % 16384` | Even by item_id; domain prefix prevents cross-shard confusion |

### Replication

- **Redis**: Master-replica per shard. Sentinel for automatic failover (< 30 s). During the 30-second failover window, reads fall back to the primary if it's still reachable or degrade to popularity fallback.
- **ClickHouse**: ReplicatedMergeTree. Writes go to all shard leaders simultaneously; async replication to replicas. Read queries routed to any available replica.
- **S3 (factor storage)**: S3's 11 9s durability via cross-AZ replication makes it the definitive source of truth for factor tables. Redis is a cache in front of S3.

### Caching Strategy

| Cache | TTL | Eviction | Notes |
|---|---|---|---|
| User factors (Redis) | 25 hours | LRU | Refreshed nightly by training pipeline |
| Item factors (Redis) | 25 hours | LRU | Refreshed nightly |
| Item-item similarity (Redis) | 25 hours | LRU | Pre-computed nightly; 240 GB total |
| User top-N pre-computed (Redis) | 2 hours | LRU | For frequently-active users; 20 items × active users |
| User interaction history (Redis) | 1 hour | LRU | Sliding window of last 20 interactions; updated by Flink |
| Popularity fallback (Redis) | 1 hour | Time-based | Global and per-country top-100; refreshed by batch job |

### CDN
Not applicable to CF API responses directly (they're personalized, not cacheable). However, the item metadata enrichment step (title, thumbnail, etc.) for the response body is served from CDN-cached item detail API with 1-hour TTL.

### Interviewer Q&A

**Q1: The Redis item-item similarity store is 240 GB. What do you do when a catalog update adds 10 million new items overnight?**

A: New items don't yet have ALS factor vectors (they weren't in the training run). Their item-item similarity tables don't exist in Redis. We handle this with a two-phase approach. First, for new items arriving intra-day, we compute content-based similarity (Jaccard over content tags + category) as a temporary similarity measure. This is cheap to compute in real-time (< 100 ms per item using inverted index over tag vocabulary) and doesn't require ALS. Content-based similarity is stored in Redis with a shorter TTL (6 hours) to signal that it's provisional. Second, the next nightly ALS retrain will incorporate all interactions with new items from that day and produce proper factor vectors, overwriting the provisional content-based similarities. Third, for the 240 GB → larger dataset concern: we compress item-factor vectors from float32 to int8 (32× compression at < 1% recall loss), reducing the item_sim table to ~30 GB, providing substantial headroom for catalog growth.

**Q2: How do you handle a "gray sheep" user — someone whose tastes don't align with any cluster of users?**

A: Gray sheep users have high interaction counts but low similarity to any user neighborhood, meaning CF recommendations are poor for them. We detect gray sheep by measuring their max similarity to any user: if max(sim(u, v) for v ∈ all users) < 0.2, the user is classified as a gray sheep. For these users, we fall back to content-based filtering (which doesn't require finding similar users) and their own historical preferences (re-ranked by recency and explicit ratings). Operationally, we set a flag `gray_sheep=true` in the user's feature hash in Redis. The CF serving layer checks this flag and bypasses the CF pipeline entirely for flagged users, routing them to a content-based fallback. We estimate ~2–3% of active users fall into this category based on industry literature.

**Q3: ALS training takes 2.5 hours nightly. What is your recovery plan if the training job fails?**

A: The nightly retrain is managed by Airflow DAG with the following resilience design: (1) Training writes to a versioned output path (`s3://ml-models/als/v{N}/`) not the serving path (`s3://ml-models/als/latest/`). The `latest` symlink is only updated after a successful training run passes validation checks. If training fails at any step, `latest` still points to the previous version. Staleness is bounded: users receive v(N-1) factors for at most 48 hours if two consecutive nightly runs fail. (2) Airflow retries: 3 retries with 15-minute exponential backoff. (3) Automated validation: before promoting a new model version, a validation job checks RMSE on a held-out test set. If RMSE is more than 5% worse than the previous version, promotion is blocked and the on-call engineer is paged. (4) Stale model alert: a Prometheus gauge tracks `hours_since_last_successful_train`. Alert fires at 30 hours (12 hours after expected staleness).

**Q4: How do you prevent the ALS model from being poisoned by fraudulent interactions (e.g., a vendor injecting fake ratings)?**

A: ALS model poisoning (also called "shilling attacks") is a real concern. We defend at three layers: (1) Input validation: the interaction ingestion service enforces rate limits (10K events/hour per user) and behavioral anomaly detection (Z-score on per-user click rate). Flagged events are marked `fraud_suspect=true` and excluded from training data. (2) Robust loss function: instead of squared error (L2), we can use a Huber loss for explicit ratings that is less sensitive to outlier ratings. For implicit feedback, the confidence weighting (c = 1 + α × frequency) naturally bounds the influence of any single user's interactions — a fraudulent user clicking 10,000 times raises confidence but the log-scaled approach (log1p(count)) limits the marginal impact. (3) Post-hoc detection: after each training run, we check for item factor vectors that shifted dramatically (cosine distance > 0.5 from previous version) and flag the items for manual review. Items where the top-20 similar items changed completely overnight are suspicious.

**Q5: At 200K RPS, Redis is receiving 200K feature lookups per second. How do you prevent Redis from becoming a bottleneck?**

A: 200K RPS to Redis is well within Redis Cluster's capability (single Redis instance handles 100K+ simple ops/second; 32-shard cluster handles 3.2 M ops/second total). However, each CF request generates multiple Redis calls: 1 user_history ZSET read + up to 20 ZSET reads for item similarities (via pipeline). Using a single pipeline call bundles these into one network round trip per request. The pipeline sends 21 Redis commands in one TCP packet, paying the RTT once. Estimated Redis load: 200K RPS × 21 commands = 4.2 M Redis ops/second. This is within capacity (3.2 M/s burst to 6 M/s). To further reduce load: (1) Pre-compute top-N recommendations for the top 20% most active users (80% of traffic), served directly from `cf:user_topn` with a 2-hour TTL — these users need only 1 Redis ZSET read instead of 21. (2) Local LRU cache in each serving pod: item-item similarity tables for the top 10K most popular items are cached in-process (hot ~24 GB of the 240 GB item_sim table). These top-10K items appear in ~60% of user histories, giving a ~60% Redis read reduction for item_sim lookups.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Detection | Recovery |
|---|---|---|---|---|
| **Redis primary shard failure** | Hardware crash or OOM | CF unavailable for ~1/32 of keyspace for < 30s | Sentinel health check | Sentinel promotes replica; resumes in < 30s |
| **ALS training job failure** | Spark OOM, data corruption, cluster failure | Model staleness (24h → 48h) | Airflow task failure alert | Airflow retry (3×); on-call if all fail; serving continues with previous model |
| **ClickHouse node failure** | Disk failure, network partition | Interaction ingestion lag; eventual data loss if not replicated | ClickHouse replication lag alert | Kafka consumer replays from last offset; ClickHouse replicas continue serving queries |
| **CF Serving pod OOM** | FAISS sidecar issue (N/A here; no FAISS) or memory leak | Request failures for that pod | Kubernetes OOM kill → pod restart | K8s restarts pod; HPA scales out if systemic |
| **Kafka broker failure** | Partition leader election needed | Interaction events delayed (not lost); Kafka consumer rebalances | Kafka lag metric | Replication ensures durability; new leader elected in seconds |
| **Similarity table missing for item** | New item published before nightly run | No similar items for that item | Miss counter in serving layer | Fall back to category-level popularity for that item; content-based similarity as interim |
| **User factor missing (new user)** | User registered after last training run | No ALS recommendations | OOV check at serving time | Cold-start fallback; update Redis with onboarding data |
| **ALS model quality regression** | Bad training data, bug in pipeline | Poor recommendations (RMSE spike) | Offline RMSE monitoring post-training | Block model promotion; roll back to previous version via `latest` pointer |

### Failover Strategy

1. **Redis Sentinel failover**: 3 Sentinel nodes form a quorum. If a primary fails, Sentinels vote and elect a new primary (the most up-to-date replica) within 10–30 seconds. During failover, serving pods detect connection errors and use a stale local cache (held for 60 seconds) before falling back to popularity.

2. **Popularity fallback chain**: If CF cannot be computed (no factors, no similarities), serving falls back gracefully: (1) User's personalized popularity (items popular in user's top categories); (2) Geo-local trending items; (3) Global popularity. This chain is pre-computed in Redis and always available.

3. **Model version rollback**: If the promoted model fails online metrics validation (CTR drops > 5% in the canary period), a single command updates the `latest` S3 pointer back to the previous version and triggers a Redis hot-reload: all pods pick up the rollback within 5 minutes.

### Retries and Idempotency

- **Interaction events**: Client-generated UUID `event_id`; deduplicated by 24-hour Bloom filter in ingestion service. Kafka `acks=all`, `retries=MAX`. Application-level idempotency handles duplicate deliveries.
- **Redis factor lookups**: Single retry with 5 ms delay. No retry storms due to per-request deadline (50 ms total budget).
- **ALS training job**: Spark ALS checkpoints every 2 iterations to HDFS/S3. If the job fails mid-training, it resumes from the last checkpoint rather than restarting from scratch.
- **Redis hot-reload** (after model version change): Orchestrated by a Redis loader Kubernetes Job. Loads factors from S3 in batches of 10,000 items, using `MSET` pipeline for efficiency. Avoids thundering herd by updating shards sequentially with 100 ms delay between shards.

### Circuit Breaker

```
Redis circuit breaker (per serving pod):
  CLOSED → OPEN if: > 5 Redis timeouts (> 5ms) in 2-second window
  OPEN: serve from popularity fallback only
  Duration: 10 seconds
  HALF-OPEN: send 10% of requests to Redis; if < 20% timeout, close circuit

ALS scoring circuit breaker:
  CLOSED → OPEN if: FAISS unavailable (N/A here; in-process Redis ops)
  Fallback: item-item CF similarity tables (independent of ALS factors)
```

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Dashboard |
|---|---|---|---|
| CF API p99 latency (item-item) | Histogram | > 80 ms | Service dashboard |
| CF API p99 latency (ALS-MF) | Histogram | > 120 ms | Service dashboard |
| Redis hit rate (user factors) | Gauge | < 85% | Redis dashboard |
| Redis hit rate (item_sim) | Gauge | < 90% | Redis dashboard |
| ALS training RMSE | Gauge | > 5% worse than prev version | ML model dashboard |
| ALS training job duration | Gauge | > 4 hours | Airflow dashboard |
| Hours since last successful model retrain | Gauge | > 30 hours | ML ops dashboard |
| Cold-start fallback rate | Counter ratio | > 10% of requests | CF serving dashboard |
| Gray sheep user percentage | Gauge | > 5% of active users | ML health dashboard |
| Item similarity coverage (% items with sim table) | Gauge | < 95% | CF data quality dashboard |
| Interaction ingestion lag (Kafka consumer) | Gauge | > 100K events | Kafka dashboard |
| ALS factor freshness (time since Redis update) | Gauge | > 26 hours | Redis dashboard |
| CTR (CF recs vs popularity fallback) | Counter ratio | CF CTR drops > 15% vs baseline | Business metrics dashboard |
| NDCG@10 (weekly offline evaluation) | Gauge | < 0.35 | Offline eval dashboard |
| Similarity table memory usage (Redis) | Gauge | > 85% capacity | Redis dashboard |

### Distributed Tracing

OpenTelemetry spans per CF request:

- `cf_serving.route_to_variant` — time to select CF variant
- `cf_serving.history_fetch` — Redis ZSET read for user history
- `cf_serving.similarity_fetch` — Redis pipeline for item similarities
- `cf_serving.score_aggregation` — CPU time for score computation
- `cf_serving.filter_seen` — time to filter user history
- `cf_serving.popularity_fallback` — only if fallback triggered

Sampling: 100% for errors and cold-start paths; 0.5% for normal paths.

### Logging

All CF serving decisions logged as structured JSON to Kafka `cf.serving.log` topic → ClickHouse for offline analysis. Log includes: user_id (hashed), item_ids returned, CF variant used, model version, experiment bucket, scores, timestamp. Used for: offline NDCG evaluation, A/B analysis, debugging, training data generation.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Rejected | Reason |
|---|---|---|---|
| Primary CF algorithm | Item-item CF + ALS MF | User-user CF | User-user requires 3 PB similarity storage at 500 M users; item-item with 240 GB is tractable |
| Item-item similarity metric | ALS factor cosine | Raw co-occurrence Jaccard | Factor cosine avoids hub problem; Jaccard gives popularity bias to blockbuster items |
| Serving store | Redis Cluster | DynamoDB | Redis 0.3 ms HGET vs DynamoDB 2–8 ms; CF serving budget is 50 ms with 20+ reads per request |
| ALS training frequency | Nightly batch | Continuous online update | Distributed ALS cannot be updated truly online; nightly retrain gives stable model; streaming handles recency signals separately |
| User-user CF approach | MF dot product (approximate) | Pre-computed user-user similarity table | Pre-computed table requires 3 PB at scale; MF dot product uses FAISS ANN over 256 GB user factors |
| Negative sampling (implicit) | All unobserved items as weak negatives (c=1) | Sample random negatives | The Hu et al. confidence weighting approach is theoretically sound and avoids the bias of random sampling |
| Item cold-start | Content embedding → projection into ALS space | Pure popularity fallback | Content embedding-based factor initialization produces 40% better NDCG@10 for new items vs popularity in internal A/B tests |
| Training objective | Weighted least squares (ALS) | BPR (pairwise ranking) | ALS has an efficient closed-form update per iteration; BPR requires stochastic gradient with careful sampling; ALS parallelizes better on Spark |
| Interaction log storage | ClickHouse (columnar) | Cassandra | ALS training scans 72 TB every quarter; columnar storage reduces this scan to 7.2 TB with compression; Cassandra is row-oriented and would require full disk scan |
| ALS rank (factor dimensions) | 128 | 256 or 512 | 128-dim provides 96% of the NDCG@10 quality of 256-dim at half the memory and 2× faster inference; diminishing returns above 128 confirmed in A/B test |

---

## 11. Follow-up Interview Questions

**Q1: How does Alternating Least Squares compare to Stochastic Gradient Descent for matrix factorization?**

A: Both optimize the same objective (minimizing regularized squared error) but via different strategies. SGD picks a random observed (u, i) entry, computes gradient, and updates p_u and q_i jointly with a small step. This is memory-efficient (O(F) working memory per step) and works well for dense matrices. However, SGD is hard to parallelize: updating p_u and q_i simultaneously from different workers causes conflicts. Hogwild-style asynchronous SGD can work but requires careful tuning. ALS fixes one set of variables (Q) and analytically solves for the other (P) — this is an exact solution per user, not a gradient approximation. The key advantage is that with Q fixed, the solution for each p_u is fully independent: `pᵤ = (QᵀQ + λI)⁻¹ Qᵀ rᵤ`. This is embarrassingly parallel — each user's update can run on a different machine with no inter-process communication needed. For distributed systems with hundreds of machines and billions of interactions, ALS's perfect parallelism makes it 5–10× faster than distributed SGD for the same convergence. Spark MLlib ALS achieves this via block partitioning.

**Q2: What is the "gray sheep" problem and how does it relate to the "black sheep" and "shilling attack" problems?**

A: These are three distinct failure modes in CF. Gray sheep: a user whose tastes are unusual and don't correlate with any user cluster. CF fails because no similar users exist. Solution: fall back to content-based filtering for these users. Black sheep: a related term used in some literature for users who are deliberately different (e.g., critics who dislike everything mainstream). Solution: same as gray sheep. Shilling attack: a malicious actor who injects fake high ratings for specific items (to boost those items in recommendations) or fake low ratings (to demote competitors). This is an adversarial manipulation rather than a natural user characteristic. Solutions include anomaly detection on rating patterns, robust loss functions (Huber vs L2), and cryptographic user verification. The key distinction: gray/black sheep are measurement failures; shilling attacks are adversarial inputs requiring security countermeasures.

**Q3: Explain why ALS requires holding the entire item factor matrix Q in memory during the user update step.**

A: During the user update step (fixing Q, solving for all p_u), the ALS formula is `pᵤ = (QᵀQ + λI)⁻¹ Qᵀ rᵤ`. The matrix (QᵀQ + λI) is F×F (128×128 = 16,384 entries) — trivially small. However, computing Q^T r_u requires accessing the row vectors q_i for all items i that user u has rated. In practice, users have rated a small fraction of items (200 items on average), so we only need 200 row vectors from Q. These 200 vectors must be accessible to the executor handling user u. In Spark ALS, this is handled by the "block-partitioned" approach: item factors are partitioned into B blocks, and for each user block, the relevant item factor blocks are broadcast to the executor. The maximum memory needed per executor is approximately: (items_per_block × F × 4 bytes) + (users_per_block × F × 4 bytes). With 100M items / 200 blocks = 500K items/block × 128 × 4 = 256 MB, and 500M users / 200 blocks = 2.5M users/block × 128 × 4 = 1.28 GB — totaling ~1.5 GB per executor, well within the 60 GB allocated.

**Q4: How would you design an online CF system that updates recommendations within seconds of a new interaction?**

A: Real-time CF typically requires sacrificing some model quality for latency. The architecture would combine: (1) Session-based retrieval: given the user just interacted with item X, immediately query the pre-computed item-item similarity table for X's top-K similar items. This is O(1) and reflects the interaction within 100 ms (just Redis lookup). No model update needed. (2) Streaming user vector update (for ALS): when user interacts with item i, update their factor vector using a single online ALS step: `p_u_new = (Q^T Q + λI)^{-1} Q^T r_u_new`. This requires computing Q^T Q (precomputed as a 128×128 matrix, refreshed hourly) and fetching q_i for only the items in user u's history. Implemented as a Flink stateful function: state = user's current factor vector + QᵀQ constant. On each new interaction event, the function updates p_u and pushes it to Redis. Latency: < 1 second from event arrival to updated recommendation. (3) Trade-off: online updates accumulate drift from the global ALS solution. Nightly full retraining reconciles the drift. Pure online SGD-based approaches (like Vowpal Wabbit) avoid this drift but sacrifice the quality of the global ALS solution.

**Q5: How do you measure the quality of collaborative filtering recommendations offline?**

A: We use a held-out test set constructed with temporal splits (not random splits, to avoid future-leakage). We hold out the last 10 interactions per user as test items. Metrics: (1) **RMSE** for explicit ratings: measures prediction accuracy. Limitation: only applies to explicitly rated items; doesn't measure ranking quality. (2) **Precision@K and Recall@K**: for each test user, generate top-K recommendations and measure what fraction of test items appear in the top-K. (3) **NDCG@K** (Normalized Discounted Cumulative Gain): positions items by predicted score, discounts relevance by position with 1/log(rank+1). Better than P@K for measuring ranking quality. Target: NDCG@10 > 0.35. (4) **Hit Rate@K**: fraction of users for whom at least one test item appears in top-K. (5) **MRR** (Mean Reciprocal Rank): average of 1/rank for the first relevant item in each user's list. (6) **Coverage**: fraction of items that appear in at least one user's top-20. A model with 0.40 NDCG@10 but only 5% catalog coverage is problematic for long-tail discovery. We report all these metrics in the nightly model evaluation report.

**Q6: What is the "diversity-accuracy trade-off" in CF and how do you manage it?**

A: CF models optimized purely for accuracy (RMSE, NDCG) tend to recommend a narrow set of popular items that the model is most confident about. This creates a filter bubble: users see the same popular content repeatedly, reducing long-tail discovery and potentially decreasing long-term satisfaction. The trade-off: increasing diversity (showing more varied, less-confident recommendations) typically lowers short-term accuracy metrics but may improve long-term user satisfaction. We manage this with a diversification parameter applied post-ranking. For item-item CF, after aggregating scores, we apply MMR: when selecting the (k+1)-th item, we penalize items whose item-factor vector is too similar to already-selected items: `score_MMR(i) = λ × score(i) - (1-λ) × max_{j ∈ selected} cosine(q_i, q_j)`. We tune λ per surface (λ=0.7 for "More Like This" where accuracy matters; λ=0.5 for Discovery feed where variety is valued). The A/B platform tracks both NDCG@10 (accuracy) and category entropy of served lists (diversity) as dual primary metrics, with business rules preventing either from degrading below threshold.

**Q7: How do you handle the case where item-item CF returns the same pivot item as the source of recommendations for many different users?**

A: This "hub item" scenario occurs when a single very popular item (e.g., a globally-watched sporting event) dominates many users' recent histories. The system effectively degenerates: 80% of users have item X in their history, and item X's top-similar items flood the recommendation slate. Two solutions: (1) **Recency weighting with pivot diversity**: in the aggregation step, if a single pivot item contributes > 50% of the aggregated score for a user's slate, we cap its contribution at 50% and redistribute weight to other pivot items. This forces diversity of source. (2) **IDF-style pivot weighting**: reduce the weight of interactions with very popular items. The weight of an interaction with item i is: `w(i) = log(N / df(i))` where N is total users and df(i) is the number of users who interacted with item i. For the globally-watched event (df = 50 M out of 500 M users): `w = log(500M/50M) = log(10) ≈ 2.3`. For a niche item (df = 1,000): `w = log(500,000) ≈ 13`. This gives 5.7× more weight to interactions with niche items, making the recommendation system more responsive to the user's unique tastes.

**Q8: Can collaborative filtering be used without any user history at all (pure content-based cold start)?**

A: Strictly speaking, CF requires at least one interaction to function. However, we can bootstrap CF-like behavior for zero-interaction users using proxy signals: (1) **Device/platform signals**: users on iOS in South Korea who browse the "K-drama" category at 9 PM have identifiable clusters in historical data. We can lookup the ALS centroid for the "Korean_iOS_evening" user cluster and use it as the new user's initial factor vector. (2) **Onboarding quiz**: mapping quiz responses to CF cluster centroids effectively places a new user in a CF neighborhood without actual interactions. (3) **Anonymous session CF**: for completely anonymous users with no logged-in history, we accumulate in-session interactions (items clicked/viewed in the current session) and run a real-time item-item CF using those session items as the "user history." This requires no user model — just the item-item similarity table already in Redis. Session-based CF produces surprisingly good results after just 3–5 in-session interactions, as demonstrated in the BERT4Rec and SASRec literature on session-aware recommendations.

**Q9: How would you detect and handle the long tail of items that have very few interactions in the CF model?**

A: The long tail — items with < 10 interactions — represents the majority of items in most catalogs (by count) but a tiny fraction of total interactions (by volume). ALS handles them poorly: with only 5 observations, the regularization term λ||q_i||² dominates, driving the item factor toward zero (the global mean prediction). This means long-tail items are not meaningfully differentiated from each other in the factor space. Solutions: (1) **Content-based item factor initialization**: initialize q_i for long-tail items from content embeddings (text, audio, images) projected into the ALS space. This provides a meaningful starting point even with 0 interactions. (2) **Separate long-tail model**: train a content-based model specifically for items with < 50 interactions. The CF model handles items with > 50 interactions; the content model handles the rest. Blend at serving time based on the item's interaction count. (3) **Transfer learning from similar items**: for an item with 3 interactions, initialize its factor vector as the weighted average of factor vectors of the most content-similar items with ≥ 50 interactions. This assumes the content-similar items share latent structure. (4) **Exploration budget**: ensure long-tail items receive guaranteed exploration impressions via the bandit mechanism, building up their interaction data over time.

**Q10: What changes to this architecture would enable real-time personalization for live sporting events?**

A: Live events require sub-second recommendation updates as the game progresses. Key architectural changes: (1) **Event-driven item recency boost**: as goals, touchdowns, or key plays happen, the corresponding highlight clips and related items receive a "hot topic" boost. A real-time event stream (separate from the interaction stream) publishes "boost events" that the serving layer reads from Redis (TTL 10 minutes). No model retraining needed. (2) **Session context dominance**: during a live game, the user's current session (last 5 minutes of activity) should dominate the recommendation signal. We increase the session context weight from 30% to 70%: `effective_user_vec = 0.3 × stored_user_vec + 0.7 × session_item_avg`. (3) **Trend acceleration**: Flink's trending items computation is tightened from 5-minute windows to 30-second windows during detected live events (flagged by the content team). Trending updates propagate to Redis within 30 seconds. (4) **Pre-event warm-up**: 24 hours before a scheduled event, we pre-compute and cache "event-specific" recommendation slates for users who have shown interest in the event type (based on CF factor similarity to past event attendees). These cached slates are served as fallback during high-load peak moments.

**Q11: How would you design the data pipeline to ensure point-in-time correctness in CF training?**

A: Point-in-time correctness means that when training on a (user, item, interaction) triple with timestamp T, all features used must reflect their state at time T, not their state when the training job runs. This prevents future leakage. For CF, the interaction matrix itself is the primary data — but if we use auxiliary features (user age at time T, item popularity at time T), we must be careful. Implementation: (1) The interaction log in ClickHouse stores raw events with their original event_ts. Training jobs filter: `WHERE event_ts < training_cutoff_date`. (2) For user features (account_age_days, interaction_count_at_T): we compute these by aggregating the interaction log up to event_ts for each training example. This requires a time-join in Spark: `user_feature_at_event_T = interaction_log.filter(ts <= event_ts).groupBy(user_id).agg(...)`. This is expensive but prevents leakage. (3) We use Delta Lake's time-travel feature to access item metadata snapshots at historical timestamps: `spark.read.format("delta").option("versionAsOf", training_cutoff_version).load(...)`. (4) We validate point-in-time correctness periodically by checking that no feature used in training has a timestamp later than the training example's event_ts. Violations indicate a leakage bug.

**Q12: Explain the "catalog coverage" problem and how it manifests in production CF systems.**

A: Catalog coverage is the fraction of available items that ever appear in any user's recommendation list. A CF system with 100% NDCG@10 but 5% catalog coverage is commercially problematic — 95% of items are never recommended, starving long-tail creators of revenue. In production, low coverage manifests as: (1) creators with good content never getting discovered; (2) the recommendation loop amplifying popular items while suppressing niche items; (3) user base fragmentation where users only see content from a narrow band of creators. We measure coverage as: `coverage = |unique_items_recommended_in_7d| / |total_active_items|`. Target: > 30% (Netflix historically reports ~30% coverage for their CF system). To improve coverage: (1) Exploration budget ensures niche items get minimum impressions. (2) Diversity penalty in MMR reduces over-concentration. (3) Explicit fairness constraints on creator-level exposure. (4) Long-tail boosting: items with high-quality signals (avg_rating > 4.0) but low impressions receive a temporary boost multiplier inversely proportional to their impression count: `boost = log(target_impressions / max(1, actual_impressions))`.

**Q13: How would you handle a sudden item deletion (e.g., DMCA takedown) in the CF system?**

A: An item must be removed from all recommendation surfaces immediately upon deletion (SLA: < 5 minutes). Steps: (1) Trigger: item deletion event published to Kafka `catalog.item.deleted` topic. (2) Serving layer: the item ID is added to a Redis set `cf:deleted_items` (global, not per-user). The CF serving layer checks this set during filtering — O(1) SET membership check. This is the fastest path (seconds to take effect). (3) Item similarity tables: delete the `cf:item_sim:{domain}:{item_id}` key from Redis immediately (Redis DEL command via the deletion event consumer). Also delete all references to the item in other items' similarity lists — this requires scanning the similarity tables for all items that have the deleted item as a similar item. This is done asynchronously (background job, < 1 hour) by querying an inverted index of `similar_item_id → [item_ids that contain it]`. (4) ALS factors: the item's factor vector is left in Redis but marked stale via a tombstone key. Next nightly retrain excludes the item from training data, and its factor vector is not written to the new model version. (5) Audit: all recommendation logs are flagged retroactively with `served_deleted_item=true` for compliance reporting.

**Q14: How would you design a CF system specifically for podcasts where implicit feedback is continuous (play time) rather than binary?**

A: Continuous implicit feedback (play duration) is richer than binary click/no-click but requires careful normalization. Design considerations: (1) **Signal normalization**: raw play duration in seconds isn't comparable across podcast episodes of different lengths. Normalize to completion ratio: `completion = min(play_time_sec / episode_duration_sec, 1.0)`. A 30-min play of a 1-hour episode (0.5 completion) is equivalent in signal to a 20-min play of a 40-min episode. (2) **Confidence mapping**: map completion ratio to ALS confidence: `c = 1 + α × completion`. For partial plays (0.2 completion), c is low — the user sampled but wasn't engaged. For full replays (completion > 1.0), c is very high. We set α = 100 to make a full completion highly confident. (3) **Skip detection**: if a user plays 5 seconds and skips, this is a negative signal (the user saw the thumbnail but didn't like the content). We encode this as a negative preference: for episodes with play < 5 seconds and episode_duration > 300 seconds, we set a low implicit preference score (0.0 instead of the normal 0–1 range). (4) **Episodic structure**: podcast episodes have ordering within a show. Users who complete episode 1 should see episode 2. We add an explicit "sequential episode" rule at post-processing: the next episode in a series the user has high completion on always appears in the top-5 recommendations.

**Q15: In a large production CF system, how do you A/B test changes to the CF algorithm itself without contaminating the experiment results?**

A: Testing CF changes requires careful experimental design because CF is inherently a system where users influence each other (network effects). Key challenges and solutions: (1) **User-level bucketing vs item-level**: CF recommendations for user A may change if user B's interactions (which influence item popularity and similarity scores) are from a different algorithm. This "network contamination" means standard user-level A/B tests may have inflated variance. Solution: use switchback testing — alternate between control and treatment at the time-of-day level, measuring outcomes within each time window. (2) **Holdout cluster**: partition users into completely isolated clusters (Cluster A = control algorithm, Cluster B = treatment algorithm). Within each cluster, train a separate CF model on only that cluster's interactions. This prevents cross-contamination at the cost of smaller training data per model. (3) **Interleaved evaluation**: rather than separate recommendation lists per treatment, interleave items from both algorithms in a single list for each user and measure which algorithm's items get more clicks. This is a within-user comparison (vs between-user), dramatically reducing variance. (4) **Delayed metrics**: CF quality is best measured by long-term outcomes (7-day or 30-day retention), not immediate CTR. Ensure experiments run long enough (minimum 2 weeks) to capture the delayed benefit of better recommendations.

---

## 12. References & Further Reading

1. **Hu, Y., Koren, Y., & Volinsky, C. (2008).** Collaborative Filtering for Implicit Feedback Datasets. *IEEE ICDM 2008.* https://ieeexplore.ieee.org/document/4781121
   — Foundational paper defining the confidence-weighted ALS objective for implicit feedback. Every production CF system for implicit data builds on this.

2. **Koren, Y., Bell, R., & Volinsky, C. (2009).** Matrix Factorization Techniques for Recommender Systems. *IEEE Computer, 42(8), 30–37.* https://ieeexplore.ieee.org/document/5197422
   — The canonical survey of MF methods for CF; introduces user/item biases and temporal dynamics.

3. **Sarwar, B., Karypis, G., Konstan, J., & Riedl, J. (2001).** Item-Based Collaborative Filtering Recommendation Algorithms. *ACM WWW 2001.* https://dl.acm.org/doi/10.1145/371920.372071
   — Introduced item-item CF and demonstrated its scalability advantage over user-user CF.

4. **Zhou, Y. et al. (2008).** Large-Scale Parallel Collaborative Filtering for the Netflix Prize. *AAAI 2008 / LNCS.* https://link.springer.com/chapter/10.1007/978-3-540-68880-8_32
   — Distributed ALS implementation for large-scale CF; basis for Spark MLlib ALS.

5. **Rendle, S., Freudenthaler, C., Gantner, Z., & Schmidt-Thieme, L. (2009).** BPR: Bayesian Personalized Ranking from Implicit Feedback. *UAI 2009.* https://arxiv.org/abs/1205.2618
   — Alternative to ALS for implicit feedback; pairwise ranking objective. Essential comparison point.

6. **He, X. et al. (2017).** Neural Collaborative Filtering. *ACM WWW 2017.* https://arxiv.org/abs/1708.05031
   — NeuMF: replaces dot product in MF with a neural network for non-linear interactions.

7. **Linden, G., Smith, B., & York, J. (2003).** Amazon.com Recommendations: Item-to-Item Collaborative Filtering. *IEEE Internet Computing, 7(1), 76–80.* https://ieeexplore.ieee.org/document/1167344
   — Amazon's seminal item-item CF paper; introduced the concept as a production system.

8. **Bell, R., & Koren, Y. (2007).** Scalable Collaborative Filtering with Jointly Derived Neighborhood Interpolation Weights. *IEEE ICDM 2007.*
   — Optimized neighborhood-based CF with learned interpolation weights; improves over simple cosine neighbors.

9. **Takács, G. et al. (2009).** Scalable Collaborative Filtering Approaches for Large Recommender Systems. *JMLR, 10, 623–656.*
   — Systematic comparison of ALS, SGD, and coordinate descent for large-scale MF.

10. **Netflix Technology Blog. (2012).** Netflix Recommendations: Beyond the 5 Stars (Part 1 & 2).* https://netflixtechblog.com/netflix-recommendations-beyond-the-5-stars-part-1-55838468f429
    — Netflix's engineering perspective on CF in production; discusses contextual signals and ensemble methods.

11. **Apache Spark MLlib ALS Documentation.** https://spark.apache.org/docs/latest/ml-collaborative-filtering.html
    — Implementation reference for distributed ALS; includes block-partitioning explanation.

12. **Dean, J., & Ghemawat, S. (2004).** MapReduce: Simplified Data Processing on Large Clusters. *OSDI 2004.* https://www.usenix.org/conference/osdi-04/mapreduce-simplified-data-processing-large-clusters
    — Foundational distributed computing paper underlying the Spark/Hadoop CF training infrastructure.

13. **Steck, H. (2019).** Embarrassingly Shallow Autoencoders for Sparse Data. *ACM WWW 2019.* https://arxiv.org/abs/1905.03375
    — EASE^R: a surprisingly effective CF method that outperforms deep learning approaches on many benchmarks while being analytically solvable.

14. **Kang, W., & McAuley, J. (2018).** Self-Attentive Sequential Recommendation. *IEEE ICDM 2018.* https://arxiv.org/abs/1808.09781
    — SASRec: transformer-based CF for sequential recommendation; state-of-the-art for session-based CF.

15. **Fan, J. et al. (2023).** Recommender Systems in the Era of Large Language Models (LLMs). *arXiv 2307.02046.* https://arxiv.org/abs/2307.02046
    — Survey of LLM-enhanced CF; relevant for understanding how LLMs are being integrated into traditional CF pipelines.
