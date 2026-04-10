# System Design: News Feed Ranking

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Personalized feed generation**: Assemble and rank a list of posts (text, images, videos, links) tailored to each user's interests, social graph, and engagement history.
2. **Multi-stage pipeline**: Implement a candidate generation → scoring → filtering → ranking pipeline that narrows from millions of candidate posts to a ranked list of ~100 posts per feed refresh.
3. **Engagement signal integration**: Incorporate real-time and historical engagement signals: click-through rate (CTR), dwell time, shares, comments, reactions, saves.
4. **Freshness decay**: Apply time-decay to post scores such that older content naturally ranks lower unless its engagement quality justifies its continued prominence.
5. **Diversity enforcement**: Prevent over-representation of any single author, topic, or content type within a single feed page.
6. **Social graph signals**: Boost posts from accounts the user follows, accounts they interact with frequently, and accounts with high friend-of-friend relevance.
7. **Spam and clickbait detection**: Filter or demote posts detected as spam, misleading, or engagement-bait before ranking.
8. **Author and page boost**: Apply configurable boosts based on creator authority, verification status, and user-specific creator affinity scores.
9. **Real-time feed serving**: Return a ranked feed within 200 ms of a feed request.
10. **Pagination**: Support infinite-scroll pagination via cursor-based pagination.

### Non-Functional Requirements

1. **Latency**: P50 < 80 ms, P99 < 200 ms end-to-end for feed requests.
2. **Throughput**: 1 M feed requests per second at peak (Facebook-scale DAU).
3. **Availability**: 99.99% uptime. Feed falls back to pre-computed ranked slates if real-time ranking is unavailable.
4. **Freshness**: New posts from followed accounts must appear in feed within 30 seconds of publishing.
5. **Engagement signal latency**: A viral post's rising engagement signals (shares, comments) must be reflected in its feed ranking within 5 minutes.
6. **Scalability**: Handle 2 B users, 100 M posts published per day, 500 M active feeds served per day.
7. **Fairness**: No systematic demotion of any protected category of content creators.
8. **Explainability**: Each feed item includes a human-readable reason ("Because you follow X", "Trending in your area").

### Out of Scope

- Stories / ephemeral content (handled by a separate stories ranking system).
- Ads (separate auction/bidding system; ads are injected into the feed at designated slots post-ranking).
- Direct messages / private content (separate messaging system).
- Content moderation (trust & safety system feeds filtered post IDs; ranking system trusts that feed).
- Graph construction (social graph is pre-built and queried via a graph service API).

---

## 2. Users & Scale

### User Types

| User Type | Feed Behavior | Ranking Relevance |
|---|---|---|
| **Highly active users** | Multiple feed refreshes/day, explicit reactions | High engagement signal; rich history for personalization |
| **Passive consumers** | Daily opens; mostly scroll without reacting | Dwell time is primary signal; CTR is secondary |
| **Content creators** | Primarily publish; also consume feed | Creator-specific boost logic for posts they author |
| **New users (< 30 days)** | Limited social graph; few follows | Cold-start: trending + topic affinity from onboarding |
| **Returning users (low frequency)** | Weekly or less; want recap of what they missed | Freshness decay adjusted; show best of last N days |
| **Bots / automated accounts** | High-volume fake engagement | Filtered out at signal ingestion; not a user type for ranking |

### Traffic Estimates

Assumption: Facebook-class platform with 2 B registered users, 1 B DAU.

| Metric | Calculation | Result |
|---|---|---|
| DAU | Assumption | 1 B |
| Feed requests per user per day | 3 app opens × 2 refreshes + 4 infinite-scroll loads = 10 | 10 |
| Daily feed API calls | 1 B × 10 | 10 B/day |
| Average RPS | 10 B / 86,400 | ~115,741 RPS |
| Peak multiplier (evening prime-time, 4× avg) | 115,741 × 4 | ~462,963 RPS |
| Safety headroom (2×) | ~462,963 × 2 | **~1 M RPS** |
| Posts published per day | Assumption (Facebook scale) | 100 M/day |
| Posts published per second | 100 M / 86,400 | ~1,157 posts/second |
| Engagement events per day (likes, comments, shares, clicks) | 1 B users × 30 events | 30 B/day |
| Engagement events per second | 30 B / 86,400 | ~347,222 events/s |
| Candidate posts evaluated per feed request | Social graph pull → 1,500 candidates → score top 500 | 500 scored per request |
| Total post scores computed per day | 10 B requests × 500 scores | 5 T scores/day |

### Latency Requirements

| Component | Target P50 | Target P99 | Notes |
|---|---|---|---|
| Full feed API (end-to-end) | 60 ms | 200 ms | At load balancer |
| Candidate generation (graph pull + fan-in) | 10 ms | 40 ms | Graph service RPC |
| Feature fetch (post + user + context) | 8 ms | 30 ms | Redis pipeline |
| Ranking model inference | 15 ms | 50 ms | TF Serving / Triton |
| Filtering (spam, diversity, seen) | 2 ms | 8 ms | In-process |
| Response serialization + network | 3 ms | 15 ms | Protobuf |
| Post content fan-out (new post → followers) | — | 30 seconds | Async write fan-out |
| Engagement signal reflection in ranking | — | 5 minutes | Flink stream processor |

### Storage Estimates

| Data Type | Size | Count | Total |
|---|---|---|---|
| Post (text, metadata, media refs) | 1 KB/post | 100 M/day × 365 days = 36.5 B posts | 36.5 TB/year |
| Post engagement counters (likes, shares, comments, clicks) | 32 B × 4 counters | 36.5 B posts | ~4.7 TB |
| Post feature vector (100 features) | 400 B | 36.5 B posts | ~14.6 TB/year (only recent posts cached) |
| User feed cache (pre-ranked, top-100) | 20 KB/user | 500 M active daily users | 10 TB |
| User engagement history (signals for personalization) | 100 B/event × 30 B events/year | — | 3 TB/year |
| Social graph edges | 16 B/edge × 200 B edges (avg 200 friends × 1 B users) | — | 3.2 TB |
| Spam/clickbait model scores (recent posts) | 4 B/post | 100 M new posts/day | 400 MB/day |
| Ranking model artifact | — | — | ~5 GB per model version |
| Fanout queues (post → follower inboxes) | 8 B/edge × 500 avg followers × 1,157 posts/s | — | 4.6 MB/s throughput |

### Bandwidth Estimates

| Flow | Rate | Bandwidth |
|---|---|---|
| Feed API responses (100 posts × 500 B rendered) | 1 M RPS × 50 KB | ~50 GB/s outbound |
| Engagement event ingestion | 347,222 events/s × 200 B | ~69 MB/s |
| Fanout writes to inbox tables | 4.6 MB/s continuous | ~4.6 MB/s |
| Feature store reads (ranking) | 1 M RPS × 10 KB features/request | ~10 GB/s internal |
| Ranking model gRPC | 1 M RPS × 500 candidates × 400 B feature vector | ~200 GB/s (batched) |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                              CLIENT LAYER                                        │
│   Mobile App (iOS/Android)  /  Web Browser  /  Third-party API Consumer         │
└────────────────────────────────┬─────────────────────────────────────────────────┘
                                 │  GET /v1/feed
                                 ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│                   API GATEWAY  (Auth, Rate Limit, A/B Routing)                   │
└────────────────────────────────┬─────────────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│                         FEED ORCHESTRATION SERVICE                               │
│                                                                                  │
│   ┌──────────────────┐   ┌──────────────────────┐   ┌──────────────────────┐    │
│   │  1. CANDIDATE    │   │  2. FEATURE          │   │  3. RANKING          │    │
│   │  GENERATION      │──▶│  ASSEMBLY            │──▶│  MODEL               │    │
│   │                  │   │                      │   │                      │    │
│   │  - Social graph  │   │  - Post features     │   │  - Gradient boosted  │    │
│   │    pull          │   │  - User features     │   │    tree (GBDT)       │    │
│   │  - Interest-     │   │  - Engagement        │   │    + DNN ranker      │    │
│   │    based         │   │    counters          │   │  - Multi-task:       │    │
│   │    retrieval     │   │  - Freshness decay   │   │    CTR, share,       │    │
│   │  - Trending      │   │  - Context (time,    │   │    comment, dwell    │    │
│   │    injection     │   │    location, device) │   └──────────┬───────────┘    │
│   └──────────────────┘   └──────────────────────┘              │               │
│                                                                 ▼               │
│                                              ┌──────────────────────────────┐   │
│                                              │  4. FILTERING                │   │
│                                              │  - Spam/clickbait demotion   │   │
│                                              │  - Already-seen suppression  │   │
│                                              │  - Muted/blocked users       │   │
│                                              │  - Geo restrictions          │   │
│                                              └──────────────┬───────────────┘   │
│                                                             │                   │
│                                              ┌──────────────▼───────────────┐   │
│                                              │  5. DIVERSITY & BOOST        │   │
│                                              │  - Max 3 posts per author    │   │
│                                              │  - Max 2 per topic           │   │
│                                              │  - Author boost              │   │
│                                              │  - Freshness boost (new)     │   │
│                                              └──────────────┬───────────────┘   │
└─────────────────────────────────────────────────────────────┼───────────────────┘
                                                              │
              ┌───────────────────────────────────────────────┼──────────────────┐
              ▼                            ▼                  ▼                  ▼
┌────────────────────┐  ┌──────────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│  SOCIAL GRAPH      │  │  FEATURE STORE       │  │  RANKING MODEL   │  │  SPAM CLASSIFIER │
│  SERVICE           │  │  (Redis Cluster)     │  │  REGISTRY        │  │  SERVICE         │
│                    │  │                      │  │  (TF Serving)    │  │                  │
│  - Follows graph   │  │  - Post features     │  │                  │  │  - GBDT on post  │
│  - Affinity scores │  │  - User features     │  │  - Multi-task    │  │    content + meta│
│  - Friend-of-friend│  │  - Engagement        │  │    DNN           │  │  - Updated hourly│
│  (Graph DB)        │  │    counters (TTL 1h) │  │  - Versioned     │  │  - Scores in     │
└────────────────────┘  │  - Trending signals  │  │    deployment    │  │    Redis         │
                        └──────────────────────┘  └──────────────────┘  └──────────────────┘

POST PUBLISHING PATH:
┌─────────────────────────────────────────────────────────────────────────────────────┐
│  USER PUBLISHES POST                                                                │
│  ─────────────────                                                                  │
│  1. Post stored in post database (Cassandra/MySQL)                                  │
│  2. Post ID published to Kafka: feed.new_posts                                      │
│  3. Fan-out service reads from Kafka:                                               │
│     a. Push-on-write: write post_id to top-N followers' inbox tables (Redis)       │
│     b. Pull-on-read: for users with > 10K followers, mark "has new content" flag   │
│  4. Feature pipeline computes post features and writes to Redis                    │
│  5. Spam classifier scores post and writes score to Redis                           │
│                                                                                     │
│  ENGAGEMENT EVENT PATH:                                                             │
│  User likes/shares → Kafka feed.engagements → Flink processor                      │
│  → Update engagement counters in Redis (atomic INCR)                               │
│  → Update trending scores in Redis ZSET                                             │
│  → Retrain engagement prediction model (nightly batch)                              │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

- **Feed Orchestration Service**: Coordinates the 5-stage pipeline for each feed request. Stateless; makes parallel RPCs to Graph Service and Feature Store simultaneously. Owns the business logic for diversity rules and author boosts.
- **Social Graph Service**: Returns a user's follow graph (up to 1,500 followees for the pull-on-read path) and pre-computed affinity scores (how often user U interacted with each followee). GraphDB (backed by TAO at Facebook scale or Neo4j/Dgraph at smaller scale).
- **Feature Store (Redis)**: Serves all serving-time features: post engagement counters (real-time), user feed preferences, trending signals. TTLs ensure stale data is evicted.
- **Ranking Model Registry (TF Serving)**: Serves the multi-task DNN ranker. Receives batched feature matrices; returns predicted scores for CTR, share rate, comment rate, and dwell time simultaneously.
- **Spam Classifier Service**: Runs hourly batch scoring of new posts using a GBDT on post content + metadata features. Results cached in Redis with 6-hour TTL.
- **Fan-out Service**: Pushes new post IDs to follower inbox tables in Redis (push path) for users with manageable fan-out (< 10K followers). For high-follower accounts (celebrities), uses pull-on-read: when a follower opens their feed, the system pulls recent posts from followed celebrities on demand.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────────────────────
-- POSTS (write-once; updates only to metadata, not content)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE posts (
    post_id         BIGINT          PRIMARY KEY,  -- Snowflake ID (timestamp + machine)
    author_user_id  BIGINT          NOT NULL,
    content_type    VARCHAR(20)     NOT NULL,  -- 'text', 'image', 'video', 'link', 'reel'
    -- Content references
    text_content    TEXT,           -- up to 63K chars
    media_urls      TEXT[],         -- S3/CDN URLs for images/video
    link_url        VARCHAR(2048),  -- for link posts
    link_domain     VARCHAR(255),   -- extracted from link_url for domain-level quality signals
    -- Classification
    topic_tags      VARCHAR(100)[],  -- ML-predicted topics (up to 10)
    language_code   CHAR(5),
    -- Geo
    geo_lat         FLOAT,
    geo_lon         FLOAT,
    country_code    CHAR(2),
    -- Visibility
    visibility      VARCHAR(20),    -- 'public', 'friends', 'followers', 'private'
    -- Moderation
    moderation_status VARCHAR(20) DEFAULT 'pending',  -- 'clear', 'pending', 'removed'
    spam_score      FLOAT,          -- from spam classifier [0–1]
    clickbait_score FLOAT,          -- from clickbait classifier [0–1]
    -- Timestamps
    created_at      TIMESTAMP       NOT NULL,
    updated_at      TIMESTAMP
) -- In production: Cassandra with post_id as partition key for fast single-post lookups

-- ─────────────────────────────────────────────────────────────
-- POST ENGAGEMENT COUNTERS (high write rate — denormalized)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE post_engagement (
    post_id         BIGINT          PRIMARY KEY,
    like_count      BIGINT DEFAULT 0,
    comment_count   BIGINT DEFAULT 0,
    share_count     BIGINT DEFAULT 0,
    click_count     BIGINT DEFAULT 0,
    impression_count BIGINT DEFAULT 0,
    total_dwell_ms  BIGINT DEFAULT 0,   -- sum of all users' dwell time on this post
    -- Computed ratios (refreshed every 5 min by Flink)
    ctr             FLOAT,              -- click_count / impression_count
    share_rate      FLOAT,              -- share_count / impression_count
    comment_rate    FLOAT,              -- comment_count / impression_count
    avg_dwell_sec   FLOAT,              -- total_dwell_ms / 1000 / impression_count
    -- Windowed (updated by Flink streaming job)
    likes_1h        INT,
    likes_24h       INT,
    shares_1h       INT,
    shares_24h      INT,
    impressions_1h  INT,
    ctr_1h          FLOAT,
    -- Velocity (rate of engagement change — key for viral detection)
    engagement_velocity FLOAT,          -- weighted (likes_1h + 5×shares_1h) / max(likes_24h,1)
    updated_at      TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- USERS (minimal subset for feed ranking)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE users (
    user_id             BIGINT PRIMARY KEY,
    -- Feed preferences
    preferred_topics    VARCHAR(100)[],    -- top-5 ML-inferred topics
    avg_session_length_sec INT,
    -- Social graph summary
    following_count     INT,
    follower_count      INT,
    -- Quality signals
    account_age_days    INT,
    is_verified         BOOLEAN,
    content_quality_score FLOAT,          -- [0–1] from historical engagement rates
    -- Context
    country_code        CHAR(2),
    primary_language    CHAR(5),
    created_at          TIMESTAMP
);

-- ─────────────────────────────────────────────────────────────
-- USER ENGAGEMENT HISTORY (for personalization features)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_post_engagement (
    user_id         BIGINT          NOT NULL,
    post_id         BIGINT          NOT NULL,
    event_type      VARCHAR(20)     NOT NULL,  -- 'impression','click','like','share','comment','dwell','skip'
    event_value     FLOAT,          -- dwell_sec for 'dwell'; null for click/like
    event_ts        TIMESTAMP       NOT NULL,
    PRIMARY KEY (user_id, post_id, event_type, event_ts)
) -- ClickHouse: partitioned by toYYYYMMDD(event_ts), ordered by (user_id, event_ts)

-- ─────────────────────────────────────────────────────────────
-- SOCIAL GRAPH EDGES
-- ─────────────────────────────────────────────────────────────
CREATE TABLE social_graph (
    follower_user_id    BIGINT          NOT NULL,
    followee_user_id    BIGINT          NOT NULL,
    follow_type         VARCHAR(20)     NOT NULL,  -- 'follow', 'friend', 'close_friend'
    -- Affinity scores (updated weekly by batch job)
    affinity_score      FLOAT,          -- [0–1] how often follower engages with followee's posts
    created_at          TIMESTAMP       NOT NULL,
    PRIMARY KEY (follower_user_id, followee_user_id)
);
-- Shard by follower_user_id for fast followee lookup
-- Separate shard by followee_user_id for fast follower lookup (fan-out)

-- ─────────────────────────────────────────────────────────────
-- USER FEED INBOX (push-model: pre-populated candidate list)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE user_feed_inbox (
    user_id         BIGINT          NOT NULL,
    post_id         BIGINT          NOT NULL,
    author_id       BIGINT          NOT NULL,
    pushed_at       TIMESTAMP       NOT NULL,
    -- Used for pull-on-read deduplication
    already_ranked  BOOLEAN DEFAULT FALSE,
    PRIMARY KEY (user_id, post_id)
);
-- High write rate; TTL 7 days; implemented as Redis sorted set (score = pushed_at timestamp)
-- Key: feed:inbox:{user_id} → ZSET  post_id → timestamp

-- ─────────────────────────────────────────────────────────────
-- RANKING FEATURES SNAPSHOT (for offline training — post-impression label join)
-- ─────────────────────────────────────────────────────────────
CREATE TABLE feed_ranking_log (
    feed_token      VARCHAR(64)  PRIMARY KEY,
    user_id         BIGINT,
    experiment_bucket VARCHAR(50),
    model_version   VARCHAR(30),
    ranked_post_ids BIGINT[],
    ranked_scores   FLOAT[],
    feature_snapshot JSONB,   -- key user and post features at ranking time
    served_at       TIMESTAMP
) PARTITION BY RANGE (served_at);

-- ─────────────────────────────────────────────────────────────
-- TRENDING TOPICS / POSTS
-- ─────────────────────────────────────────────────────────────
CREATE TABLE trending (
    scope           VARCHAR(50)     NOT NULL,  -- 'global', 'country:US', 'topic:sports'
    entity_type     VARCHAR(20)     NOT NULL,  -- 'post', 'topic', 'hashtag'
    entity_id       VARCHAR(100)    NOT NULL,
    trend_score     FLOAT           NOT NULL,
    rank            INT,
    window_hours    INT,            -- 1, 6, 24
    computed_at     TIMESTAMP,
    PRIMARY KEY (scope, entity_type, entity_id)
);
```

**Redis Key Patterns:**

```
# Feed inbox (push-on-write, candidates for feed)
feed:inbox:{user_id}              → ZSET  post_id → publish_timestamp  (TTL 7d)

# Post engagement counters (real-time, updated by Flink INCR)
feed:post:eng:{post_id}           → HSET  {like_count, share_count, comment_count, 
                                            click_count, impression_count,
                                            ctr_1h, share_rate_1h, engagement_velocity}  (TTL 72h)

# Post features (scored by batch pipeline, refreshed hourly)
feed:post:feat:{post_id}          → HSET  {spam_score, clickbait_score, author_quality,
                                            topic_tags[], freshness_score, content_type}  (TTL 72h)

# User feed features
feed:user:feat:{user_id}          → HSET  {preferred_topics[], avg_session_len,
                                            country_code, is_verified}  (TTL 24h)

# User-author affinity (how often user interacts with a specific author)
feed:affinity:{user_id}:{author_id} → FLOAT  affinity_score  (TTL 7d, updated by Flink)

# Trending posts by scope
feed:trending:global              → ZSET  post_id → trend_score  (TTL 1h)
feed:trending:country:{cc}        → ZSET  post_id → trend_score  (TTL 1h)
feed:trending:topic:{topic}       → ZSET  post_id → trend_score  (TTL 1h)

# User seen-posts (Bloom filter for suppression)
feed:seen:{user_id}               → Bloom filter  post_id membership  (TTL 30d, ~10 KB)

# Pre-computed feed cache (for high-traffic users)
feed:cache:{user_id}              → ZSET  post_id → ranking_score  (TTL 5 min)

# Celebrity post registry (for pull-on-read path)
feed:celebrity_posts:{user_id}    → ZSET  post_id → timestamp (user is celebrity, TTL 24h)
```

### Database Choice

| Concern | Options | Selected | Justification |
|---|---|---|---|
| **Post storage (write-once, random reads)** | Cassandra, MySQL (sharded), DynamoDB | **Cassandra** | Wide-column store with `post_id` as partition key; sub-millisecond random post reads; 1,157 posts/second writes distributed across 20+ nodes; no joins required for post serving |
| **Engagement counters (high write rate atomic)** | Redis (INCR), Cassandra (counter columns), DynamoDB | **Redis + Cassandra async** | Redis provides atomic INCR with sub-ms latency for real-time serving; Cassandra persists engagement asynchronously via Flink for durability; Redis-only would be volatile |
| **User engagement history (append-only, analytical)** | ClickHouse, BigQuery, Redshift | **ClickHouse** | 30 B events/day; columnar aggregations for feature pipeline; MergeTree with user_id ordering for fast user-scoped queries |
| **Social graph** | Neo4j, AWS Neptune, TAO (Facebook), MySQL + cache | **MySQL sharded + Redis cache** | At 200 B edges, a dedicated graph DB (Neo4j) struggles with sharding. MySQL sharded by `follower_user_id` handles 1-hop queries (who does user follow) in O(1) with index. Graph traversal beyond 2-hops is not needed for feed. Redis caches the top-150 followees per user (sorted by affinity_score) for the hot serving path |
| **Feed inbox (candidate store)** | Redis ZSET, Cassandra, DynamoDB | **Redis ZSET** | 500 M active user inboxes × 100 candidates = 50 B entries; Redis ZSET allows O(log N) ZRANGEBYSCORE by timestamp and O(1) ZADD for fanout; at 100 entries/user × 8 bytes = 800 B/user → 400 GB for 500 M users (fits in cluster) |
| **Ranking model serving** | TF Serving, Triton, custom gRPC | **TF Serving** | Versioned model management, native batching, gRPC proto interface, shadow/canary deployment support |
| **Trending computation** | Redis ZSET + Flink, Spark batch, ClickHouse MV | **Redis ZSET updated by Flink** | Flink maintains sliding window aggregations with 5-minute micro-batches; writes to Redis ZSET; TTL auto-expires stale trending data |

---

## 5. API Design

### Authentication
Bearer JWT required. Includes `user_id`, `device_type`, `locale`. Feed content varies by locale/language and device type (video-heavy for mobile on WiFi; text-heavy for low-bandwidth).

### Endpoints

```
GET /v1/feed
```
**Purpose:** Retrieve the user's ranked news feed.

**Auth:** Bearer JWT

**Rate limit:** 300 feed fetches/hour per user (accounts for infinite scroll and background prefetch).

**Query parameters:**

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| limit | int | no | 25 | Posts to return per page (max 50) |
| cursor | string | no | null | Pagination cursor for next page |
| surface | string | no | `main_feed` | `main_feed`, `explore`, `following_only` |
| max_post_age_hours | int | no | 72 | Oldest acceptable post age in hours |
| lat / lon | float | no | null | Geo context for local content |

**Response:**
```json
{
  "feed_token": "ft_a1b2c3...",
  "experiment_bucket": "ranker_v7_treatment",
  "posts": [
    {
      "post_id": 9908827334,
      "author_user_id": 12345678,
      "content_type": "video",
      "ranking_score": 0.923,
      "reason": "Trending in Technology",
      "position": 1,
      "estimated_dwell_sec": 45,
      "social_context": {
        "liked_by_friends": ["Alice M.", "Bob K."],
        "comment_count": 1204,
        "share_count": 8920
      }
    }
  ],
  "next_cursor": "eyJ0aW1lc3...",
  "feed_freshness_ts": "2026-04-09T20:15:00Z"
}
```

---

```
POST /v1/feed/engagement
```
**Purpose:** Report engagement events for feed items (impressions, clicks, dwell, reactions).

**Auth:** Bearer JWT

**Rate limit:** 50,000 events/hour per user.

**Request body:**
```json
{
  "feed_token": "ft_a1b2c3...",
  "events": [
    {
      "event_id": "evt_uuid",
      "post_id": 9908827334,
      "event_type": "dwell",
      "event_value": 34.5,
      "position_when_seen": 1,
      "scroll_depth_pct": 0.45,
      "event_ts": "2026-04-09T20:15:35Z"
    }
  ]
}
```

**Response:** `202 Accepted`. Asynchronous; published to Kafka.

---

```
POST /v1/feed/action
```
**Purpose:** Record explicit user actions on feed posts (like, share, comment, hide, "not interested").

**Auth:** Bearer JWT

**Request body:**
```json
{
  "post_id": 9908827334,
  "action_type": "not_interested",
  "reason": "too_political",
  "feed_token": "ft_a1b2c3..."
}
```

**Response:** `200 OK`. For `not_interested`, synchronously adds the post_id and optionally the author_id to the user's suppression list in Redis (< 1 second propagation to next feed load).

---

```
GET /v1/feed/explain/{post_id}
```
**Purpose:** Return a detailed explanation for why a post appears in the user's feed.

**Auth:** Bearer JWT

**Response:**
```json
{
  "post_id": 9908827334,
  "explanations": [
    {"factor": "author_follow", "weight": 0.40, "description": "You follow @TechWriter"},
    {"factor": "topic_affinity", "weight": 0.30, "description": "Matches your interest in AI"},
    {"factor": "social_context", "weight": 0.20, "description": "3 of your friends reacted"},
    {"factor": "trending", "weight": 0.10, "description": "Trending in Technology"}
  ],
  "model_version": "ranker_v7"
}
```

---

```
GET /v1/trending
```
**Purpose:** Return trending posts/topics for a given scope.

**Auth:** Bearer JWT (optional)

**Query params:** `scope` (`global`, `country`, `topic`), `topic` (if scope=topic), `n` (default 10)

**Response:** List of `{post_id, trend_score, trend_rank, topic}`.

---

## 6. Deep Dive: Core Components

### 6.1 Multi-Stage Ranking Pipeline

**Problem it solves:** The user follows potentially thousands of accounts that publish millions of posts per day. Showing all of them in chronological order would overwhelm users (legacy Twitter/Facebook pre-2009 approach). Ranking must select the ~100 most relevant, interesting, and diverse posts from this firehose within 200 ms.

#### Pipeline Stages

```
STAGE 1: CANDIDATE GENERATION
──────────────────────────────
Input:  user_id, request context
Output: ~1,500 candidate post_ids

Sources:
  A. Social inbox (push): Redis ZSET feed:inbox:{user_id}
     → ZREVRANGEBYSCORE by timestamp, limit 1,200 posts (last 72h)
  B. Trending injection: Redis ZSET feed:trending:{scope} 
     → top-100 trending posts not in social inbox
  C. Interest-based retrieval: ML embedding ANN search 
     → top-100 posts matching user's topic embedding
  D. Follow-indirect (viral): posts from non-followed accounts
     with high engagement from followed accounts
     → top-50 from graph service "social trending" endpoint
Total: ~1,500 candidates

STAGE 2: FEATURE ASSEMBLY
──────────────────────────
Input:  1,500 candidate post_ids + user_id
Output: feature matrix [1,500 × 120 features]

Per-post features (80 features):
  - Engagement: ctr_1h, ctr_24h, share_rate_1h, share_rate_24h,
    comment_rate_24h, avg_dwell_sec, engagement_velocity
  - Quality: spam_score, clickbait_score, author_quality_score,
    avg_author_ctr (7d), link_domain_quality
  - Content: content_type (one-hot), language match, has_media (bool),
    topic_tags (top-3, one-hot over 200 topics)
  - Freshness: age_minutes, freshness_score (decay function)
  - Author: is_verified, follower_count (log), is_followed_by_user,
    affinity_score (user-author)
  - Social: friend_like_count, friend_comment_count, 
    friend_share_count (from social graph)

Per-user features (30 features):
  - Preferences: preferred_topics (top-5), avg_session_len,
    preferred_content_types
  - Session: current_session_topics, time_of_day (sin/cos encoding),
    device_type, connection_type (wifi/4G)
  - History: avg_dwell_time (7d), click_rate (7d), share_rate (7d)
  - Context: is_following_author (bool), affinity_score (user-author pair)

Cross features (10):
  - topic_match: bool (user top_topic ∩ post topic_tags)
  - language_match: bool
  - affinity × engagement_velocity (product feature)
  - time_of_day × content_type match (e.g., user watches videos in evening)

STAGE 3: RANKING MODEL
───────────────────────
Input:  feature matrix [1,500 × 120]
        (in practice, 500 pre-filtered after light heuristics)
Output: per-post scores for 4 tasks

Architecture: Multi-Task Learning (MTL) with shared bottom + task towers
(details in Section 6.2)

Prune 1,500 → 500 before model call (heuristic pre-filter):
  - Remove posts with spam_score > 0.7 (fast reject)
  - Remove posts with age > 72h AND engagement_velocity < 0.05
  - Remove posts from blocked/muted authors
  This reduces model calls from 1,500 to 500, saving 66% inference cost.

STAGE 4: FILTERING
────────────────────
Input:  500 posts with 4 model scores each
Output: filtered set (typically ~450)

Rules applied in order (fail-fast):
  1. Remove posts already in user's seen Bloom filter (feed:seen:{user_id})
  2. Remove posts with spam_score > 0.5 (secondary threshold after model)
  3. Remove posts with clickbait_score > 0.6 AND ctr > 0.15 (high CTR but clickbait)
  4. Remove posts from accounts user marked "not_interested" in last 30d
  5. Remove posts violating geo restrictions
  6. Remove posts with visibility='friends' where user is not a friend

STAGE 5: DIVERSITY ENFORCEMENT + FINAL RANKING
───────────────────────────────────────────────
Input:  ~450 scored posts
Output: top-100 ranked posts (25 returned per page)

Diversity rules (greedy sequential selection):
  - Max 3 consecutive posts from same author
  - Max 5 total posts per author in top-25
  - Max 3 posts per topic_l1 in top-25
  - Minimum 20% posts from non-followed accounts (exploration)

Author and freshness boosts (applied to ranking_score):
  - Verified account × affinity > 0.7: +0.05 boost
  - Post age < 30 min: +0.03 freshness boost
  - Social context (≥ 3 friends engaged): +0.04 social proof boost
```

#### Approaches Comparison for Ranking Model

| Model Type | Quality | Latency | Feature Interactions | Explainability | Training Cost |
|---|---|---|---|---|---|
| **Logistic Regression** | Low | < 1 ms | Manual feature engineering | High | Very low |
| **Gradient Boosted Trees (GBDT: LightGBM)** | Medium-High | 2–5 ms | Automatic | Medium | Low-medium |
| **Wide & Deep** | High | 10–20 ms | LR for memorization + DNN for generalization | Low | Medium |
| **Multi-Task Learning DNN** | Very High | 20–40 ms | Deep feature interactions + task correlation | Low | High |
| **Transformer (full sequence)** | Very High | 50–200 ms | Attention over full user history | Very Low | Very High |
| **Two-stage: GBDT pre-rank + DNN re-rank** | High | 5–15 ms (GBDT) + 20–30 ms (DNN on top-100) | Best of both | Medium | High |

**Selected: Multi-Task Learning DNN (primary) + LightGBM GBDT (fast pre-rank for 1,500→500 pruning)**

**Implementation details in Section 6.2.**

#### Interviewer Q&A

**Q1: Why do you use a multi-stage pipeline instead of running the full model on all 1,500 candidates?**

A: Running the 120-feature MTL DNN on 1,500 posts per request at 1 M RPS would require: 1 M × 1,500 = 1.5 B DNN inferences per second. At 20 ms per batch of 500, this would need 3,000 GPU replicas just for inference. The multi-stage approach amortizes this cost: the cheap heuristic pre-filter (microseconds) reduces to 500, the fast GBDT (2 ms, CPU-only) on 500 reduces to 200 pre-ranked, and the DNN scores only the top-200 candidates in a single batch. Cost reduction: 7.5× fewer DNN inferences. Quality impact: the GBDT pre-filter has > 97% recall@200 (meaning 97% of the time, the DNN's eventual top-20 results are present in the 200 GBDT passes through) — this was validated empirically by comparing DNN-only full-scan vs two-stage NDCG@20 on a held-out test set.

**Q2: How does fan-out work for accounts with millions of followers, and where is the bottleneck?**

A: Fan-out is the process of distributing a new post to all followers' inbox tables. For typical users (< 1,000 followers), push-on-write is used: when the post is published, a fan-out service worker writes the post_id to all followers' Redis ZSET inboxes synchronously within 1–2 seconds. For accounts with millions of followers (celebrities, major brands): push-on-write would require millions of Redis ZADD operations per post — at 1,000 ms per 1,000 ZADD operations, a celebrity with 10 M followers would take ~10,000 seconds to fan out. This is infeasible. The solution is a hybrid push-pull model: (1) The celebrity's post is stored in a Redis key `feed:celebrity_posts:{celebrity_user_id}`. (2) When any follower opens their feed, the feed orchestration service makes a pull-on-read call to the graph service: "which celebrities does this user follow?" and fetches recent posts from each celebrity's post key (O(1) per celebrity). (3) The threshold for push vs pull is configurable; we use > 10K followers = pull path. This trades write cost for read latency: celebrity post fetches add ~5 ms to read latency but eliminate the millions-of-writes bottleneck.

**Q3: How do you ensure the ranked feed reflects a post going viral in real-time?**

A: Viral post detection and ranking update operates through three mechanisms: (1) Real-time engagement counter updates: Flink consumes the engagement event stream from Kafka and updates `engagement_velocity` in Redis within 30 seconds using a 1-hour sliding window aggregation. The next feed request for any user with that post in their candidate set will see the updated velocity. (2) Trending injection: Flink also maintains the `feed:trending:{scope}` ZSET. When a post's engagement_velocity crosses a threshold (e.g., top-0.01% of hourly velocity), it's added to the trending ZSET and injected as a candidate for all users in that scope, even users who don't follow the author. (3) Fan-out amplification: when a followed user shares a viral post, that share event triggers a fan-out of the original post (not just the share) to the sharer's followers. This is the social graph amplification effect — viral posts propagate through the follow graph organically within minutes.

**Q4: How do you handle "position bias" in the ranking training data?**

A: Position bias is severe in news feeds: posts shown at position 1 receive clicks not because they're the best but because users see them first. If we train on raw click data, the model learns to prefer whatever was already ranked first — a vicious cycle. Correction strategies: (1) **Inverse Propensity Scoring (IPS)**: estimate the probability of a user seeing each position (position propensity) via randomization experiments (periodically shuffle ranking for a holdout cohort; measure true click-position curves). Weight training examples by 1/P(position k). At position 1: P≈0.40; position 5: P≈0.15; position 10: P≈0.07. (2) **Dwell time over CTR as the primary label**: dwell time (time spent reading a post after clicking) is less susceptible to position bias than CTR because the user must actively read to accumulate dwell time. A post clicked impulsively at position 1 but immediately scrolled past has low dwell time. (3) **Randomization layers**: 5% of feed requests serve randomly shuffled rankings (for a random holdout user slice). Training data from these randomized slates is labeled with position=random, providing unbiased training signal. Meta/Facebook uses a variant of this called "treatment logs vs organic logs" separation.

**Q5: How would you handle a breaking news event (e.g., major earthquake) that requires immediate feed prioritization?**

A: Breaking news requires overriding the ML-ranked feed with human editorial judgment. The system supports an "emergency boost" mechanism: (1) Editorial team triggers a "critical event" flag via the admin API: `POST /admin/events {type: "breaking_news", topic_tag: "earthquake_turkey_2026", boost_factor: 3.0, ttl_minutes: 120}`. (2) The feed orchestration service reads this flag from Redis (key: `feed:emergency_boost`, checked on every request). Posts matching the `earthquake_turkey_2026` topic tag have their ranking scores multiplied by boost_factor=3.0. (3) Trending injection: affected posts are force-added to the `feed:trending:global` ZSET with score = MAX_FLOAT, ensuring they appear in every user's candidate set. (4) Safety controls: emergency boosts have a mandatory TTL (120 minutes max) and require 2-person approval to prevent misuse. All boost events are logged to an immutable audit trail.

---

### 6.2 Engagement Signals — CTR, Dwell Time, Freshness Decay

**Problem it solves:** Raw engagement counts are biased by item age, popularity baseline, and position. We need normalized, comparable signals that capture the quality of engagement rather than its volume, and we need to decay old content so the feed stays fresh.

#### Signal Processing

**CTR Normalization:**

Raw CTR (clicks/impressions) is noisy for new posts with few impressions. A post with 10 clicks out of 10 impressions (CTR=1.0) should not outrank a post with 10,000 clicks out of 150,000 impressions (CTR=0.067) because the former has a confidence interval too wide to trust.

We use **Wilson Score Interval** to compute a lower-bound CTR that is confidence-adjusted:

```
Wilson Lower Bound CTR:
  n = impressions
  p̂ = clicks / n  (observed CTR)
  z = 1.96  (95% confidence)

  lower_bound = (p̂ + z²/2n - z√(p̂(1-p̂)/n + z²/4n²)) / (1 + z²/n)
```

For the 10/10 post: `lower_bound ≈ 0.72` (we're 95% sure CTR is at least 72%).
For the 10K/150K post: `lower_bound ≈ 0.063` (tighter interval around 6.7%).

The Wilson score naturally penalizes low-sample posts and rewards statistical certainty.

**Dwell Time Signal:**

Dwell time is measured from when a post enters the viewport to when the user scrolls past (viewport exit). We normalize by expected dwell time for the content type:

```
normalized_dwell(post) = actual_dwell_sec / expected_dwell_sec(content_type)

expected_dwell_sec:
  - text (< 280 chars): 8 seconds
  - text (> 280 chars): 20 seconds
  - image: 10 seconds
  - video: 0.5 × video_duration_sec (expected 50% completion)
  - link: 15 seconds
```

Normalized dwell > 1.0 means the user spent more time than average on that content type (positive signal). Normalized dwell < 0.3 is a "skip" signal — shown in viewport but user didn't read.

**Weighted Engagement Score:**

Individual signals are combined into a single engagement quality score used as the ranking model's training label for the engagement task:

```python
def compute_engagement_score(post_engagement: dict, user_action: str) -> float:
    """
    Computes normalized engagement score for training label.
    Higher = stronger positive engagement.
    """
    weights = {
        'like': 1.0,
        'comment': 3.0,       # more effort = stronger signal
        'share': 5.0,         # highest intent signal
        'click': 0.5,         # could be accidental; low weight
        'dwell_norm': 2.0,    # per unit of normalized dwell
        'save': 4.0,          # explicit save = strong positive
        'skip': -0.5,         # negative signal
        'report': -5.0,       # strong negative signal
        'hide': -3.0          # negative signal
    }
    
    score = 0.0
    for action, weight in weights.items():
        if action in post_engagement:
            if action == 'dwell_norm':
                score += weight * min(post_engagement[action], 3.0)  # cap at 3×expected
            else:
                score += weight * post_engagement.get(action, 0)
    
    return score
```

**Freshness Decay:**

Feed freshness is critical — users expect to see new content, not the same posts recycled. We apply a logarithmic time decay:

```
freshness_score(age_minutes) = 1 / (1 + log(1 + age_minutes / half_life_minutes))

half_life values:
  - News / breaking: half_life = 60 min    → 50% score at 1 hour
  - General content: half_life = 360 min   → 50% score at 6 hours
  - Evergreen content (tutorials, longform): half_life = 2880 min → 50% at 48 hours

Content type is ML-classified during post ingestion.
```

Freshness score is multiplied into the final ranking score:

```
final_score = model_score × freshness_score × author_boost × social_boost
```

**Engagement Velocity (for viral detection):**

```python
def compute_engagement_velocity(post_id: str) -> float:
    """
    Velocity = how much faster engagement is growing vs. the post's own baseline.
    Values >> 1 indicate viral acceleration.
    """
    eng = redis.hgetall(f"feed:post:eng:{post_id}")
    
    likes_1h = int(eng.get('likes_1h', 0))
    likes_24h = int(eng.get('likes_24h', 1))  # avoid division by zero
    shares_1h = int(eng.get('shares_1h', 0))
    
    # Weighted engagement: shares count 5× more than likes for virality
    weighted_1h = likes_1h + 5 * shares_1h
    weighted_24h = likes_24h / 24  # normalize to per-hour baseline
    
    velocity = weighted_1h / max(weighted_24h, 0.1)  # ratio of current to average
    return min(velocity, 100.0)  # cap at 100 to prevent extreme outliers
```

Posts with velocity > 10 are considered viral and added to trending ZSETs.

#### Interviewer Q&A

**Q1: Why is dwell time a better signal than CTR for feed ranking quality?**

A: CTR measures the user's decision to click — which is easily manipulated by misleading thumbnails and clickbait headlines. A post with a dramatic headline and a boring article will have high CTR but low satisfaction. Dwell time (time spent on the post after clicking/stopping) measures actual engagement with the content, which is much harder to game. Research from Facebook (Diuk 2016, "Serving Diverse Content") demonstrated that optimizing for CTR created a clickbait feedback loop: CTR-optimized models learned that sensational content gets more clicks, so they recommended more of it, which trained users to click more on sensational content. Switching to a dwell-time weighted objective reduced clickbait complaints by 35%. Dwell time has its own limitations though: long articles with boring intros still get high dwell time from confused users. The composite weighted engagement score (combining dwell, shares, comments, and explicit signals) provides the most robust training signal.

**Q2: How do you measure dwell time accurately on mobile where the user might leave the app during reading?**

A: Mobile dwell time measurement is tricky. We use viewport-based measurement: the client SDK fires a `viewport_enter` event when a post enters the viewable area and a `viewport_exit` event when it leaves (via IntersectionObserver on web; via UIScrollView observation on iOS). Dwell = `viewport_exit.timestamp - viewport_enter.timestamp`. Edge cases: (1) App backgrounding: if the user presses Home without scrolling, `viewport_exit` is never fired. We handle this by flushing all in-progress viewport sessions to the server when `UIApplicationDidEnterBackground` fires (iOS) or `onPause` (Android). (2) Network loss: dwell events are buffered locally (max 50 events) and flushed on next network connection. Buffered events are accepted with up to 24-hour delay (we match them to the `feed_token` for the session). (3) Maximum dwell cap: dwell time > 5 minutes is capped (post was likely abandoned with the app open). The cap is content-type-specific (videos are capped at their duration + 30 seconds).

**Q3: How do you implement the freshness score so that high-quality old content isn't completely suppressed?**

A: Pure freshness decay would suppress excellent evergreen content (a tutorial written 2 years ago that's still the best explanation of a topic). We balance freshness with quality via "quality-adjusted freshness": the freshness decay rate is modulated by the post's absolute engagement quality. A post with exceptionally high avg_dwell_sec (e.g., 3× the category average) has a longer half-life applied to it: `effective_half_life = base_half_life × quality_multiplier` where `quality_multiplier = min(2.0, avg_dwell_norm / 1.0)`. A post with 2× average dwell time has its half-life doubled: from 6 hours to 12 hours. This prevents the feed from becoming all ephemeral low-quality content while still keeping the feed fresh. Additionally, for returning users (> 3 days since last login), the freshness decay is globally relaxed: we serve "best of the past 7 days" with reduced freshness penalty, presenting the content they missed.

**Q4: What's the difference between engagement velocity and engagement volume, and which matters more for ranking?**

A: Volume measures total engagement (1 M likes = high volume). Velocity measures how fast engagement is accelerating relative to baseline. For ranking, velocity is more informative in two scenarios: (1) Freshness: a 1-hour-old post with 10,000 likes growing at 500/minute is more feed-worthy than a 24-hour-old post with 50,000 likes but growing at 5/minute. The latter has peaked; the former is rising. (2) Emerging content: volume discriminates against new posts that haven't had time to accumulate engagement. A post with 100 shares in its first 30 minutes is performing better than one with 500 shares over 48 hours, even though the volume is lower. In our composite ranking score, both matter: volume enters as a baseline signal (log-normalized) and velocity enters as a time-sensitive boost. `score = α × log(1 + engagement_volume) + β × tanh(engagement_velocity)`. We use tanh to cap velocity's influence (preventing a single viral tweet from flooding all feeds) and log for volume (diminishing returns for very high volumes). The weights α and β are learned by the ranking model as part of the feature importance.

**Q5: How do you prevent the freshness decay from creating a "tyranny of the new" where only just-published content appears?**

A: This is a real failure mode in chronological or near-chronological feeds. Three mechanisms prevent it: (1) Engagement quality floor: posts below a minimum engagement quality threshold (ctr < 0.02 OR spam_score > 0.3) are suppressed regardless of freshness. A brand-new low-quality post doesn't get boosted. (2) Author affinity weight: the user's affinity score for an author amplifies older posts from authors the user genuinely values. An 18-hour-old post from a highly-valued author (affinity = 0.9) outranks a 2-hour-old post from a low-affinity account. `age_adjusted_score = freshness_score × (1 + affinity_weight)`. (3) Diversity rules: the maximum consecutive posts from new accounts (age < 1h) in a feed page is capped at 3. This ensures the feed has a mix of very fresh and high-quality slightly-older content. The result is a feed that feels fresh (recent events are represented) but not superficial (established quality content still appears).

---

### 6.3 Spam and Clickbait Detection

**Problem it solves:** News feed ranking must actively identify and demote low-quality content before it reaches users. Spam (fake, automated, or fraudulent posts) and clickbait (headlines engineered for clicks but delivering unsatisfying content) systematically poison the ranking signal and degrade user trust.

#### Detection Approaches

| Approach | Signal Type | Accuracy | Real-time? | Covers New Posts? |
|---|---|---|---|---|
| **Rule-based (keyword blacklists, link domain blocks)** | Content text, domain | Low (high false positives) | Yes | Yes | 
| **GBDT on content features** | Text TF-IDF, metadata | Medium-High | Near-real-time (1-min batch) | Yes |
| **Fine-tuned BERT (text classification)** | Full text understanding | High | No (50–200ms/post) | With GPU |
| **Engagement pattern anomaly detection** | Behavioral signals | High | Near-real-time | Only for established posts |
| **Cross-account coordination detection** | Graph-based | Very High for coordinated attacks | Batch (hourly) | No (only patterns) |
| **Satisfaction gap detection (CTR vs dwell)** | Engagement signals | High (for clickbait) | 5-min delay | No (needs engagement history) |

**Selected: GBDT on content features (fast batch) + Satisfaction Gap Model (engagement-based)**

**GBDT Spam/Clickbait Classifier Features:**

```python
spam_features = {
    # Content text features
    'caps_ratio': len([c for c in text if c.isupper()]) / max(len(text), 1),
    'exclamation_count': text.count('!'),
    'url_count': len(re.findall(r'http\S+', text)),
    'hashtag_count': text.count('#'),
    'mention_count': text.count('@'),
    'text_length': len(text),
    
    # Title/headline features (most predictive for clickbait)
    'starts_with_number': int(bool(re.match(r'^\d+', headline))),  # "10 reasons..."
    'has_curiosity_gap': contains_phrases(['you won't believe', "what happened next",
                                          "will shock you", "the truth about"]),
    'headline_sentiment_magnitude': abs(vader_sentiment(headline)),  # extreme sentiment
    
    # Link quality features
    'domain_reputation_score': get_domain_reputation(link_domain),  # precomputed
    'has_shortened_url': int('bit.ly' in text or 't.co' in text),
    'link_url_matches_headline_topic': compute_topic_similarity(headline, link_url),
    
    # Author features
    'author_historic_spam_rate': get_author_spam_rate(author_id),   # from history
    'author_account_age_days': author.account_age_days,
    'author_following_follower_ratio': author.following_count / max(author.follower_count, 1),
    'has_verified_badge': int(author.is_verified),
    
    # Temporal features
    'post_hour': datetime.hour,  # spam posts cluster at certain hours
    'is_weekend': int(datetime.weekday() >= 5),
}
```

**Satisfaction Gap Detection (clickbait detector from engagement):**

Clickbait has a distinctive engagement pattern: high CTR but low dwell time (users click, realize it's clickbait, immediately close). We measure the satisfaction gap:

```python
def compute_satisfaction_gap(post_id: str) -> float:
    """
    Satisfaction gap = difference between predicted and actual dwell time.
    High gap = clickbait (people click but don't read).
    """
    eng = get_post_engagement(post_id)
    
    # Predicted dwell: what we'd expect given the CTR and content type
    predicted_dwell = get_average_dwell_for_category(
        content_type=eng.content_type,
        category=eng.category_l1
    )
    
    # Actual dwell (from engagement signals)
    actual_dwell = eng.avg_dwell_sec
    
    # Satisfaction gap: how much worse than expected
    if predicted_dwell > 0:
        gap = (predicted_dwell - actual_dwell) / predicted_dwell
        # gap = 0: met expectations
        # gap = 0.7: user dwelled only 30% of expected time (clicked but didn't read)
    else:
        gap = 0.0
    
    return max(0.0, gap)  # only penalize negative surprises

# Thresholds:
# gap > 0.7 AND impressions > 1000: likely clickbait → clickbait_score += 0.5
# gap > 0.5 AND impressions > 5000: moderate clickbait → clickbait_score += 0.25
```

**Coordinated Inauthentic Behavior (CIB) Detection:**

Coordinated spam accounts exhibit suspicious graph patterns: a cluster of newly-created accounts that all follow each other and engage with the same set of posts within a short time window. Detected via:

1. **Bipartite graph community detection**: Build a bipartite graph of accounts → posts they engaged with. Apply Louvain community detection on this graph hourly in Flink/Spark. Communities of > 10 accounts with > 80% shared engagement patterns flag as coordinated.

2. **Temporal engagement bursts**: If a post receives > 1,000 likes in 5 minutes from accounts created in the last 48 hours, flag for human review and temporarily reduce the post's ranking score.

#### Interviewer Q&A

**Q1: How do you handle false positives — legitimate posts incorrectly labeled as spam?**

A: False positive rate must be kept very low because incorrectly suppressing legitimate content harms creators and user trust. We use a tiered action system rather than binary suppress/pass: (1) score 0.3–0.5: "reduced distribution" — post appears lower in feed rankings but isn't removed. (2) score 0.5–0.7: "limited reach" — post only shown to direct followers; not amplified to non-followers or trending. (3) score > 0.7: post placed in a human review queue before serving. Creators can appeal any restriction via a structured appeal API. Appeal outcomes are used to retrain the classifier (false positives become negative training examples). We track false positive rate weekly: if the appeal reversal rate exceeds 5%, the score threshold is adjusted upward. We also segment false positive rate by creator demographics to detect any disparate impact.

**Q2: How do you prevent the spam classifier from being reverse-engineered (adversarial examples)?**

A: Adversarial attacks on spam classifiers are a constant arms race. Mitigations: (1) **Ensemble secrecy**: the full feature set and thresholds of the spam classifier are never publicly disclosed. The model is a black box from the creator's perspective. (2) **Behavioral signals over content signals**: sophisticated spammers can evade text-based features (rewrite the text, use images instead) but cannot easily fake engagement patterns (fake engagement requires bot farms, which are detectable via device fingerprinting and IP clustering). Over time, we shift weight toward behavioral signals. (3) **Model drift monitoring**: if spam classifier precision/recall degrades over 2 weeks (measured by a human-labeled holdout set reviewed weekly), we know adversarial adaptation is occurring and trigger an emergency retrain with fresh adversarial examples. (4) **CAPTCHA-gated appeals**: to prevent automated adversarial feedback loops through the appeal system, appeals require CAPTCHA completion and manual text explanation, making automated adversarial probing economically expensive.

**Q3: What is the "engagement bait" detection problem distinct from clickbait, and how do you handle it?**

A: Engagement bait is different from clickbait in mechanism: where clickbait tricks users into clicking by withholding information, engagement bait explicitly solicits engagement through requests like "Like this if you agree!", "Share if you love your mom", "Tag a friend who needs this!". Engagement bait inflates engagement counts artificially — the post gets many likes/shares not because the content is valuable but because the creator issued a direct call-to-action. Detection: we train a binary classifier specifically for engagement bait patterns using a labeled dataset of 500K posts manually reviewed for engagement bait language. Features include NLP patterns (imperative sentences + engagement verb + social reinforcement), comment text analysis (if most comments are @mentions with no substantive content, that indicates tag-baiting). Detected engagement bait posts have their engagement signals discounted: `effective_engagement = raw_engagement × (1 - engagement_bait_score)`. This prevents the inflated signals from propagating into ranking model training or trending lists.

**Q4: How do you ensure the spam classifier generalizes across languages?**

A: The classifier must handle 50+ languages. Single-model approaches: (1) **Multilingual BERT** (mBERT or XLM-RoBERTa): a single transformer model fine-tuned on spam detection in 10+ languages generalizes to unseen languages reasonably well via transfer learning. Downside: 50–200 ms inference per post. (2) **Language-agnostic features**: many spam features are language-independent — caps_ratio, exclamation_count, URL patterns, engagement pattern features, author history features. A LightGBM on these features achieves 85% accuracy across all languages. Our solution: language-agnostic LightGBM (fast, deployed for all languages) + XLM-RoBERTa fine-tuned classifier (slower, only for high-volume languages: English, Spanish, Mandarin, Arabic, Portuguese, French, Hindi). The fast model runs within 50 ms per post; the slow model is applied asynchronously after publishing, updating the spam_score in Redis within 2 minutes.

**Q5: A creator claims their post was unfairly suppressed by the spam classifier. How do you investigate and resolve the complaint?**

A: Investigation follows a standard process: (1) **Fetch the model's decision**: query the serving log for the post's spam_score, clickbait_score, and the features that contributed most (using SHAP values logged during scoring). Present these to the reviewer: "Post was scored 0.65 for spam due to: high URL count (3 URLs), domain reputation 0.3 for 'example-domain.com', and author's historical spam rate 0.12." (2) **Human review**: a trust & safety reviewer examines the post, the feature explanation, and the domain reputation database. If the domain is incorrectly labeled as low-reputation (a common false positive cause), the domain reputation database is corrected. (3) **Appeal outcome**: if the complaint is upheld, the post's spam_score is manually set to 0.1, the post is re-entered into the ranking pipeline, and the (post, features, label=0) tuple is added to the classifier's correction training set. (4) **Retroactive distribution**: if the post was suppressed during the review period (typically 24 hours), we evaluate whether "make-good" distribution is appropriate — pushing the post to the creator's followers as a one-time non-ranked notification.

---

## 7. Scaling

### Horizontal Scaling

**Feed Orchestration Service**: Stateless pods on Kubernetes. At 1 M peak RPS with 200 ms P99 budget: each pod handles ~200 RPS (fanout RPC to graph service + Redis pipeline + TF Serving gRPC). 1 M / 200 = 5,000 pods minimum; 10,000 with safety headroom. Each pod: 4 vCPU, 4 GB RAM. Cluster: 10,000 × 4 vCPU = 40,000 vCPUs. Auto-scales on RPS metric.

**Redis Cluster (feature store + inbox)**: 
- Post engagement features: 100 M active posts × 400 B = 40 GB
- User features: 500 M users × 200 B = 100 GB
- Feed inboxes: 500 M users × 100 posts × 8 B = 400 GB
- Total: ~540 GB. 32-shard cluster, 24 GB/shard = 768 GB capacity.

**TF Serving (ranking model)**:
- 1 M RPS; each request scores 500 candidates with 120 features
- Batch inference: bundle 1,000 requests × 500 posts = 500,000 scores/batch
- At 30 ms/batch: throughput = 1,000 / 0.030 = 33,333 batches/second (single GPU)
- Throughput required: 1 M RPS / batch_size_100 = 10,000 batches/second
- GPU replicas needed: 10,000 / 33,333 = ~300 A100 GPUs
- With batching optimization: target batch_size=500, throughput improves to 100 batches/s per GPU → 100 GPUs needed. Realistic with CPU for simpler models.

**Kafka**: 
- Engagement events: 347,222 events/s × 200 B = 69 MB/s
- New posts: 1,157 posts/s × 1 KB = 1.2 MB/s
- Total ingest: ~70 MB/s. 8 brokers with 5× headroom handles 350 MB/s burst.

### DB Sharding

| Store | Shard Key | Strategy | Notes |
|---|---|---|---|
| Cassandra posts | `post_id` (token ring) | Consistent hash | Uniform read/write; post_id is Snowflake (timestamp-prefixed) which provides natural write distribution |
| MySQL social graph | `follower_user_id % N` | Hash | All follows from user U on same shard; fast followee list queries |
| ClickHouse engagement history | `sipHash64(user_id) % 8` | Hash | User-scoped aggregations stay on one shard |
| Redis feed inbox | `CRC16(feed:inbox:{user_id}) % 16384` | Hash slots (native) | Inbox ZSET operations isolated to one shard |
| Redis post engagement | `CRC16(feed:post:eng:{post_id}) % 16384` | Hash slots | Uniform distribution by post_id |

### Replication

- **Cassandra**: Replication factor 3 across 3 AZs. `LOCAL_QUORUM` writes (2/3 replicas must ACK). `LOCAL_ONE` reads (fastest). Write latency < 5 ms; read latency < 2 ms.
- **Redis**: Master + 2 read replicas per shard. Reads load-balanced across replicas. Feed inbox writes go to master only; engagement counter INCs go to master (atomic).
- **MySQL social graph**: Primary + 1 replica per region. Writes to primary (follow/unfollow); reads (who does user follow?) from replica. Eventual consistency (follow appears in feed within 30 seconds).
- **ClickHouse**: ReplicatedMergeTree with 2 replicas per shard. Engagement history writes async to both replicas.

### Caching Strategy

| Cache | TTL | Notes |
|---|---|---|
| Feed pre-computed cache (`feed:cache:{user_id}`) | 5 minutes | For rapid re-opens (back-back opens within same session) |
| Post feature cache (`feed:post:feat:{post_id}`) | 72 hours | Evicted when post is older than serving window |
| Post engagement counters (`feed:post:eng:{post_id}`) | 72 hours | Updated by Flink; TTL aligned with serving window |
| User features (`feed:user:feat:{user_id}`) | 24 hours | Refreshed by nightly feature pipeline |
| Trending ZSETs | 1 hour | Refreshed by Flink every 5 minutes; TTL as safety net |
| Social graph top-150 followees (`feed:follows:{user_id}`) | 1 hour | User's most-affinity followees, sorted by affinity score |
| Spam/clickbait scores (`feed:post:feat:spam_score`) | 6 hours | Refreshed when post is re-scored hourly |

### CDN
Post media (images, video thumbnails, link previews) served from CDN (Cloudflare). Cache-key: `post_id + media_type + resolution`. TTL: 7 days (posts don't change their media after publishing). Geographic distribution: 50+ POPs globally.

### Interviewer Q&A

**Q1: The fan-out service needs to write a celebrity's post to potentially 100 million inbox tables. How do you prevent this from causing massive write latency?**

A: Celebrities with 100 M followers use the pull-on-read architecture (described in Section 6.1 Q2). For non-celebrity fan-out (< 10K followers), the 10K ZADD operations can be performed in parallel across Redis shards. Each ZADD takes ~0.1 ms; 10,000 ZADDs in parallel across 32 shards = ~3 ms total (limited by the slowest shard). Fan-out throughput: if 1,157 posts/second are published with an average 300 followers each: 1,157 × 300 = 347,100 ZADD operations per second. This is ~10,853 ops/shard/second — well within Redis's capacity of 100K+ ops/second per node. For very popular non-celebrity accounts (10K–1M followers), we use a middle path: fan-out to a "super-followers" subset (top 10K highest-affinity followers get push fan-out immediately; the remaining followers get pull-on-read). The affinity-sorted list in Redis provides the "super-followers" set in O(log N).

**Q2: How do you scale the social graph service to handle 1 M RPS of "who does user follow?" queries?**

A: The social graph query ("give me user U's top-150 followees sorted by affinity") is the most latency-critical dependency of the feed pipeline (required for candidate generation). We scale it via three layers: (1) In-Redis cache: each user's top-150 followees (sorted by affinity_score) is pre-computed and stored in `feed:follows:{user_id}` with 1-hour TTL. At 1 M RPS: 1 M Redis ZRANGE calls per second. With 32-shard Redis cluster at 100K ops/shard/second = 3.2 M total ops/second — sufficient. (2) Local pod cache: the 10 K most-active users' follow lists are held in each serving pod's in-process LRU cache (bounded to 50 MB). These users generate ~50% of all requests (Pareto distribution). Cache hit eliminates the Redis call entirely. (3) Graph service pre-computation: for users whose follow graph changed recently (follow/unfollow events via Kafka), a Graph Service job asynchronously re-computes the affinity-sorted followee list and updates Redis. Eventual consistency: a new follow appears in feed within 30 seconds (Kafka → Graph Service → Redis update).

**Q3: With 10,000 serving pods each holding a local cache, how do you invalidate the cache when a user mutes or blocks an author?**

A: Pod-local caches are intentionally short-lived (30-second TTL for seen-posts; 1-hour for follow lists). For mute/block operations, we use a hybrid approach: (1) Immediate Redis update: mute/block synchronously writes to Redis (`feed:blocked:{user_id}` set). All serving pods check this set on each feed request (1 Redis SISMEMBER call, ~0.1 ms). No pod-local cache for blocked users — this is always authoritative. (2) Pod-local cache invalidation: for the followee list cache (`feed:follows:{user_id}` pod-local copy), we use a version number. Redis stores a per-user version counter (`feed:follows:ver:{user_id}` INT). When a follow/unfollow/block event occurs, we INCR this version. Each pod checks if its cached version matches Redis; if not, it refreshes. The version check costs 1 extra Redis GET per request but is batched with the main Redis pipeline call (no additional RTT). Stale pods will serve outdated follow lists for at most 30 seconds (the pod-local cache TTL), which is acceptable.

**Q4: How do you handle the "thundering herd" problem when a major event causes millions of users to open their feeds simultaneously?**

A: Thundering herd occurs when a sudden traffic surge causes all clients to simultaneously request fresh feed data, overwhelming the backend. Mitigations: (1) **Client-side staggering**: the mobile client randomizes feed refresh with ±30-second jitter around the user-triggered refresh. This spreads what would be a simultaneous 1-second spike over a 60-second window. (2) **Pre-computed feed cache**: the `feed:cache:{user_id}` in Redis with 5-minute TTL means that if 100 users refresh simultaneously, only 1 or 2 of them trigger a full feed computation; the others are served the cached result (with a freshness timestamp indicating when it was computed). (3) **Adaptive rate limiting at the edge**: when API Gateway detects > 5× normal RPS, it activates "surge mode": the feed cache TTL is extended from 5 to 60 seconds (users see slightly stale feeds); the candidate generation step skips the expensive graph ANN query and uses only the pre-populated inbox; the ranking model falls back to the GBDT (faster) instead of the DNN. (4) **Auto-scaling pre-warming**: a traffic forecasting model runs every 5 minutes. If it predicts > 2× normal RPS in the next 30 minutes (based on time-of-day patterns and breaking news signals), it triggers proactive pod scaling.

**Q5: How do you shard the ClickHouse engagement history for maximum training pipeline throughput?**

A: The ClickHouse engagement history (30 B events/day, 3 TB/year) is used by the nightly training pipeline to compute training labels and user features. Shard key `sipHash64(user_id) % 8` ensures all of a user's events are on one shard, enabling efficient user-scoped aggregations (no cross-shard joins for most queries). Training pipeline parallelism: the Spark ALS training job reads from all 8 shards in parallel via the Spark-ClickHouse connector, applying consistent hash partitioning on `user_id` in Spark to match the ClickHouse sharding. This prevents data shuffles: each Spark partition reads from exactly one ClickHouse shard. For feature computation, each shard handles 1/8 of users: `SELECT user_id, COUNT(*) as interactions_7d FROM interactions WHERE user_id % 8 = shard_num AND event_ts > now() - 7 days GROUP BY user_id`. ClickHouse's vectorized execution scans 30 B / 8 = 3.75 B events per shard at ~3 GB/s scan speed → 1.25 TB per shard / 3 GB/s = ~416 seconds per shard = ~7 minutes. With all 8 shards running in parallel: full feature computation takes ~7 minutes, well within the 4-hour nightly window.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Detection | Recovery |
|---|---|---|---|---|
| **Redis cluster shard failure** | Node crash, OOM | Post engagement features stale; feed inbox degraded for 1/32 of users | Sentinel heartbeat | Sentinel promotes replica in < 30s; serving falls back to Cassandra for engagement |
| **Social graph service unavailable** | Network partition, DB timeout | Candidate generation falls back to inbox only (no graph traversal) | Health check failure → circuit breaker opens | Serving uses last-cached followee list from Redis; circuit half-opens after 30s |
| **TF Serving ranking model crash** | OOM on GPU, bad model deployment | Feed quality degrades; GBDT fallback activated | HTTP 503 → circuit breaker | Circuit breaker activates GBDT ranker; TF Serving pod restarts; DNN re-enabled after health check |
| **Fan-out service lag** | Kafka consumer lag, deployment issue | New posts delayed in followers' feeds (not lost; Kafka persists) | Consumer lag > 500K events alert | Scale up fan-out service; Kafka provides durable buffer; posts delivered when lag clears |
| **Spam classifier unavailable** | Pod crash | New posts served without spam filtering | Health check failure | Posts served with `spam_score=0` (no demotion) and added to a review queue for retroactive scoring |
| **Cassandra node failure** | Disk failure, GC pause | Post metadata reads degraded (LOCAL_QUORUM still met with 2/3) | Cassandra nodetool alerts | Cassandra repairs automatically; read consistency maintained by quorum |
| **Ranking model quality regression** | Bad training data, data pipeline bug | Poor recommendations (NDCG degrades) | CTR drop > 10% in 2h Grafana alert | Rollback model version via TF Serving tag change; revert `latest` to previous version |
| **ClickHouse write failure** | Disk full, node failure | Engagement history logging delayed; eventual consistency impact | Flink consumer exception alerts | Kafka consumer replays from last offset; ClickHouse replicas continue serving |

### Failover Strategy

**Feed degradation levels (graceful degradation ladder):**

```
Level 0 (Normal): Full DNN ranking + all features + real-time engagement
Level 1 (Degraded): GBDT ranking (no TF Serving); real-time engagement still available
Level 2 (Degraded): GBDT ranking + cached engagement (Redis unavailable for some keys)
Level 3 (Heavily degraded): Pre-computed ranked feed cache (feed:cache:{user_id}) if it exists
Level 4 (Minimal): Social inbox in reverse-chronological order (no ranking)
Level 5 (Emergency): Global trending feed (no personalization)
```

Each level is triggered by a specific health check failure, with automatic progression and manual override capability.

**Multi-region active-active**: Two AWS regions (us-east-1, eu-west-1). Each region serves its geographically nearest users. Feed state (inboxes, engagement features) is replicated cross-region via Kafka cross-cluster replication with ~500 ms lag. Route53 latency routing + health checks ensure automatic failover in < 60 seconds.

### Retries and Idempotency

- **Engagement events**: Client-generated `event_id` (UUID v4). Bloom filter deduplication in ingestion service (24-hour window). Kafka `acks=all` for at-least-once delivery. ClickHouse `ReplacingMergeTree` by `event_id` ensures at-most-once storage effect.
- **Feed inbox fan-out (ZADD)**: Idempotent by design — `ZADD NX` (only add if not exists) prevents duplicate inbox entries if fan-out retries.
- **Redis counter INCs (engagement)**: INCR is atomic and idempotent within a TTL window. If Flink processes an event twice (due to checkpoint failure), the counter is double-incremented. Mitigation: Flink uses exactly-once semantics with Kafka transactions for engagement counter updates (more expensive but guarantees correctness).
- **Graph service social graph updates**: Follow/unfollow events are idempotent in MySQL (INSERT IGNORE). If replayed, no duplicate rows created.

### Circuit Breaker Configuration

```
Social Graph Service circuit breaker:
  - Open condition: > 20% requests > 30 ms in 10-second window  
  - OPEN: use Redis-cached followee list only (no graph service call)
  - Duration: 15 seconds
  - HALF-OPEN: 10% test traffic to graph service

TF Serving (DNN ranker) circuit breaker:
  - Open condition: > 10% requests return 500/503 in 5-second window
  - OPEN: GBDT fallback activated
  - Duration: 30 seconds (TF Serving pod typically restarts in < 20s)
  - HALF-OPEN: 5% test traffic; close if < 5% errors

Redis circuit breaker:
  - Open condition: > 5 connection timeout in 2-second window
  - OPEN: Cassandra fallback for post features; chronological inbox for candidates
  - Duration: 10 seconds
```

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Dashboard |
|---|---|---|---|
| Feed API p99 latency | Histogram | > 250 ms | Service SLA dashboard |
| Feed API p50 latency | Histogram | > 100 ms | Service SLA dashboard |
| Feed error rate (5xx) | Counter ratio | > 0.1% | Service SLA dashboard |
| DNN ranker inference p99 | Histogram | > 60 ms | ML serving dashboard |
| GBDT fallback rate | Counter ratio | > 5% (indicates DNN issues) | ML serving dashboard |
| Fan-out service lag (Kafka consumer) | Gauge | > 1 M events | Fan-out dashboard |
| Post freshness in feed (avg age of top-5 shown posts) | Gauge | > 4 hours | Feed quality dashboard |
| Feed CTR (7-day moving avg) | Gauge | Drop > 10% week-over-week | Business metrics |
| Feed share rate (7-day moving avg) | Gauge | Drop > 8% week-over-week | Business metrics |
| Avg dwell time per post (7-day) | Gauge | Drop > 10% week-over-week | Business metrics |
| Spam classifier precision (weekly evaluation) | Gauge | < 0.90 | Trust & Safety dashboard |
| Spam classifier recall (weekly evaluation) | Gauge | < 0.85 | Trust & Safety dashboard |
| False positive rate (appeal reversal rate) | Gauge | > 5% | Trust & Safety dashboard |
| Redis hit rate (feed features) | Gauge | < 88% | Redis dashboard |
| Model version skew (% traffic on latest model) | Gauge | < 90% on latest after 30 min | Deployment dashboard |
| Diversity score (category entropy of top-10 per user) | Gauge | Drop > 15% week-over-week | Feed quality dashboard |
| Cold-start fallback rate | Counter ratio | > 8% | Feed quality dashboard |

### Distributed Tracing

OpenTelemetry with Jaeger. Spans per feed request:

- `feed.candidate_gen.inbox_fetch` — Redis inbox ZSET read
- `feed.candidate_gen.graph_service_rpc` — social graph API call
- `feed.candidate_gen.trending_fetch` — Redis trending ZSET read
- `feed.feature_assembly.post_features` — Redis pipeline for 500 posts
- `feed.feature_assembly.user_features` — Redis user feature fetch
- `feed.feature_assembly.engagement_counters` — Redis engagement hash reads
- `feed.ranking.dnn_inference` — TF Serving gRPC call
- `feed.filtering.spam_check` — spam score check
- `feed.filtering.bloom_filter` — seen-posts Bloom filter check
- `feed.diversity.mmr_rerank` — diversity enforcement

Sampling: 100% for requests with p99 > 300 ms or any 5xx errors; 0.1% for normal traffic.

### Logging

Structured JSON logging at every pipeline stage. Critical log line: the "ranking decision log" — for every post in the feed result, we log `{post_id, model_score, freshness_score, author_boost, social_boost, final_score, position, reason_code}`. This enables post-hoc debugging of "why did post X appear at position 3?" without re-running the model.

Sensitive data: user_id is HMAC-SHA256 hashed in logs. Post content is never logged; only post_id references.

Log retention: 7-day hot tier in Elasticsearch; 90-day warm tier in S3 + Athena; 1-year archive in Glacier.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Rejected | Reason |
|---|---|---|---|
| Fan-out model for celebrities | Pull-on-read (> 10K followers) | Push-on-write for all | Push for 100M followers = millions of writes per post; pull eliminates write bottleneck at cost of read-path fan-out latency |
| Ranking model | Multi-Task DNN | Single-objective CTR model | MTL captures correlated engagement signals (CTR, dwell, share) jointly; single CTR model creates clickbait loop; MTL NDCG@20 is 12% better in A/B |
| Freshness signal | Logarithmic decay (smooth) | Exponential decay (aggressive) | Exponential decay completely suppresses content within hours; logarithmic preserves high-quality content longer; tunable per content type |
| Engagement label | Weighted composite (like=1, comment=3, share=5, dwell=2/s) | Binary click | Binary click is noisy and gameable; composite score better correlates with 7-day retention in offline analysis (r=0.78 vs r=0.52 for binary click) |
| Spam score computation | Batch GBDT (near-real-time, 1 min) + satisfaction gap | Real-time per-post ML API | Real-time API at 1,157 posts/s with 200ms BERT inference would require 231 GPU replicas; batch GBDT achieves 90% of the quality at 1/50th the cost |
| Position bias correction | IPS + dwell time as primary label | No correction | Uncorrected training on click data creates well-documented clickbait feedback loop (demonstrated by Facebook's 2016 "feed quality" study) |
| Diversity enforcement | MMR post-ranking | Diversified candidate retrieval | Post-ranking MMR allows full optimization of top candidates before diversity; diversified retrieval is harder to calibrate without harming relevance in top slots |
| Social graph access pattern | Redis-cached top-150 followees + MySQL canonical | Real-time MySQL read per request | MySQL read at 1 M RPS × per-user joins = 1 M queries/second on MySQL — not feasible; Redis cache serves 99% of requests with < 1 ms |
| Feed pagination | Cursor-based (opaque timestamp + post_id) | Offset-based | Offset pagination suffers from "gap" and "duplication" problems as new posts are inserted; cursor-based provides stable pagination for infinite scroll |
| A/B bucketing granularity | User-level (stable across sessions) | Request-level | Request-level bucketing creates inconsistent user experience within a session; user-level ensures deterministic, consistent feed experience per experiment |

---

## 11. Follow-up Interview Questions

**Q1: How would you design the system to support an algorithmic transparency requirement — allowing users to see exactly why each post was ranked in their feed?**

A: Algorithmic transparency requires three capabilities: (1) **Feature logging**: at ranking time, log the top-5 most influential features for each post's ranking score (computed via SHAP values in shadow mode). These are stored in the `feed_ranking_log.feature_snapshot` column. (2) **Human-readable explanation mapping**: a lookup table maps feature names to user-friendly strings: `{author_affinity: "Because you frequently engage with @Author", topic_match: "Because you're interested in Technology", trending: "Trending in your area"}`. The top-3 contributing factors are selected for the explanation. (3) **Explanation API** (`GET /v1/feed/explain/{post_id}`) exposes a detailed breakdown for any post in a user's feed (within the last 7 days). GDPR Article 22 requires algorithmic explanations for automated decisions — our logging makes this compliance requirement achievable. The challenge: SHAP value computation adds ~5 ms per post if done at serving time. Solution: compute SHAP values only for the top-25 shown posts (not all 500 ranked), reducing the overhead to acceptable levels.

**Q2: How would you detect and handle a "coordinated manipulation" campaign where thousands of fake accounts are promoting a specific narrative?**

A: Coordinated inauthentic behavior (CIB) detection requires graph-level analysis: (1) **Account clustering**: build a bipartite graph of accounts and the posts they engage with. Apply community detection (Louvain algorithm) hourly on Flink. Clusters of > 20 accounts with > 85% shared engagement patterns in the last 2 hours are flagged. (2) **Temporal burst analysis**: if a post receives 10,000 engagements from accounts created within the last 7 days in a 30-minute window, it's flagged for CIB review. (3) **Network velocity**: measure the diameter of the engagement network (how many hops separate engaging accounts in the social graph). CIB accounts often have no real social connections between them (freshly created), giving a very high average diameter. Authentic viral content spreads through dense social clusters (low diameter). (4) **Content fingerprinting**: CIB campaigns often share near-identical text across many posts. LSH (MinHash) fingerprinting detects duplicate or near-duplicate content clusters published by different accounts in the same time window. Detected CIB content is demoted from feed, its engagement signals are discounted, and the account cluster is queued for human review and potential suspension.

**Q3: How do you handle privacy-preserving personalization under regulations like GDPR and CCPA?**

A: Privacy-preserving personalization requires restricting what data is used and for how long: (1) **Data minimization**: feed personalization uses only interaction data (posts seen, clicked, shared) — no browsing history outside the platform, no inferred sensitive attributes (health, religion, politics). (2) **Data retention limits**: per GDPR Article 5(1)(e), engagement history is retained for 12 months maximum for personalization purposes. After 12 months, raw events are deleted from ClickHouse; only aggregated features (not raw events) persist. (3) **Opt-out of personalization**: users can opt for "chronological feed" mode. In this mode, the ranking model is bypassed entirely; feed is served from the social inbox in reverse-chronological order. This is a single flag in the user features table checked at the start of the ranking pipeline. (4) **Data access requests (DSARs)**: users can download their engagement history within 30 days. We expose an async export endpoint that generates a JSON file from ClickHouse and delivers it to the user's email. (5) **Right to erasure**: deleting a user's data requires removing their feed inbox (Redis DEL), their engagement history (ClickHouse DELETE WHERE user_id = X — completes within 24 hours via deferred merge), their user factors and features (Redis DEL), and excluding their interaction data from future training runs (suppression list maintained in the training pipeline).

**Q4: A post from a small creator goes viral unexpectedly. How does the system detect and amplify this signal without a delay?**

A: Viral detection for organic content (not seeded by the engagement manipulation we discussed) follows the engagement velocity pipeline: (1) Flink's 5-minute micro-batch detects when a post's engagement velocity exceeds 10× its 24-hour baseline (computed as: shares in last 60 min / expected shares per hour based on author's historical engagement rate). (2) The post is added to the `feed:trending:global` ZSET and category-specific trending ZSETs in Redis within 5 minutes of the viral detection event. (3) The fan-out service, which normally only pushes posts to direct followers, is triggered to do a "viral fan-out": it pushes the post to the top-10K users (by follower-count × engagement-rate) in the same topic category, not just the author's followers. This is a "viral amplification" path that operates only for posts crossing the velocity threshold. (4) The organic amplification has safeguards: the spam classifier score is checked before viral fan-out (spam_score < 0.2 required); the post must have > 100 organic engagements (not just a single power-user sharing it); and the creator must have been on the platform for > 30 days (prevents new-account spam exploitation).

**Q5: How would you redesign the feed for a different surface — a "For You" TikTok-style discovery feed where the user has no explicit social graph?**

A: TikTok's "For You Page" is fundamentally different from a social network feed: it's a pure interest-based recommendation system with no social graph dependency. Key differences: (1) **No candidate generation from social inbox**: since there are no followees, all candidates come from interest-based retrieval. We replace the social inbox with a content embedding ANN search (user interest vector → similar content vectors via FAISS) as the primary candidate source. (2) **Session-dominant signals**: TikTok users make rapid watch/skip decisions. The session context (last 10 videos watched with > 50% completion) dominates the personalization signal. We weight current-session embedding at 70% vs stored user profile at 30%. (3) **Aggressive exploration**: 40% of the feed is dedicated to exploration (new creators, new topics not yet in the user's profile). Thompson Sampling bandit determines which exploration items to show. (4) **Completion rate as primary label**: watch completion (what fraction of the video was watched) is the primary training label, replacing the weighted engagement composite. Completion is harder to fake than click and directly measures content-video fit. (5) **Content embedding freshness**: since new videos must be discoverable immediately (no social graph to amplify them), the content embedding for new videos is computed at upload time (using a video encoder for the first frame + title + audio description) and inserted into the FAISS index within minutes of posting.

**Q6: How would you detect and prevent a "filter bubble" forming in the feed — where a user only sees ideologically homogeneous content?**

A: Filter bubble prevention requires measuring and actively counteracting topic concentration: (1) **Diversity monitoring**: for each user, compute the topic entropy of their last 100 impressions: `H = -Σ p(topic_k) × log(p(topic_k))`. If H < threshold (e.g., H < 1.5 nats, indicating > 60% of content is from a single topic cluster), the user is flagged for diversity intervention. (2) **Diversity injection**: the feed pipeline has a "serendipity module" that activates for flagged users: 2 of the top-20 feed slots are reserved for "cross-category discovery" — posts from topics underrepresented in the user's recent history that nonetheless have high quality (ctr_24h > 0.05, avg_dwell_norm > 0.8). (3) **Cross-viewpoint content**: for news and opinion content specifically, we apply a "viewpoint diversity" constraint: if the user has seen > 5 posts on topic T from a single ideological perspective (classified by a viewpoint classifier), the next post on topic T shown must represent a different perspective. This is handled by a topic-and-viewpoint-aware MMR variant. (4) **Transparency**: users see a "Explore new topics" section explicitly labeled at the bottom of their feed, giving them agency to discover content outside their filter bubble without having it forced into their main feed.

**Q7: What would you do differently if feed content was much longer (e.g., blog articles of 5,000+ words rather than social posts)?**

A: Long-form content fundamentally changes the engagement signal and serving requirements: (1) **Reading time as the primary signal**: for 5,000-word articles, completion rate (reading to the end) is rarely achievable. Instead, we use "scroll depth" (what percentage of the article was scrolled) and "return visits" (did the user come back to continue reading?) as signals. Measuring these requires the client SDK to report scroll depth events periodically during reading. (2) **Content embedding richness**: for long articles, the title alone is insufficient for content-based retrieval. We embed the full article text using a passage encoder (dense retrieval model like DPR or Contriever), averaging over 512-token chunks to produce a document-level embedding. This is more expensive (GPU inference per article) but feasible at 1,157 articles/minute. (3) **Delayed label**: a 5,000-word article's engagement signal (full scroll, completion, saves) may take 20–30 minutes to accumulate after serving. Training labels for long-form content must be collected with a 1-hour delay (join impression log with engagement log from T+60 min). (4) **Feed density**: users reading long articles don't want a dense feed of many items. The feed UX shifts from "infinite scroll of short posts" to "curated daily digest of 5–10 long reads." The ranking objective shifts from maximizing total engagement events to maximizing reading time per session.

**Q8: How do you ensure the ranking system does not amplify misinformation or harmful content even when that content has high engagement?**

A: High engagement is not synonymous with quality — misinformation and outrage-inducing content often drives high CTR, shares, and comments. The ranking system must decouple engagement metrics from quality-adjusted ranking: (1) **Third-party fact-checking integration**: posts flagged by fact-checking partners (e.g., First Draft, PolitiFact API integration) receive an automatic ranking demotion (multiply ranking score by 0.3). The fact-check label is shown as context to users who still see the content (it's not removed, just demoted and labeled). (2) **Engagement quality weighting**: "angry reactions" and "hate comments" are treated as lower-quality signals than positive engagement. The engagement label weights in the ranking model are: like=1.0, comment_positive=2.0, comment_negative=0.5, share=4.0, angry_reaction=0.2. (3) **Velocity skepticism**: viral misinformation often has an unnatural velocity profile — extremely rapid initial spread followed by a dropoff (as fact-checks emerge). The engagement velocity feature is combined with a "velocity sustainability" measure: if velocity at T+2h is < 20% of peak velocity at T, it's flagged as potentially artificial. (4) **Human-in-the-loop escalation**: content with > 10K impressions AND a fact-check flag is escalated to human editorial review. The feed system pauses amplification for such content pending review.

**Q9: How do you A/B test ranking changes when engagement signals themselves depend on what was ranked?**

A: This is the "Goodhart's Law" problem in A/B testing — the measure becomes the target, and the target becomes corrupted. In feed ranking A/B tests: (1) The control group's engagement metrics are influenced by the control algorithm; the treatment group's by the treatment algorithm. A treatment that shows more videos (which have higher dwell time) will show higher average dwell time, but not because users are more satisfied — they're watching the same videos for the same duration. We use "holdout users" — users who receive neither the control nor treatment algorithm during the experiment period — to measure the true baseline and detect such composition artifacts. (2) "Inter-treatment contamination": user A (in treatment) shares a post with user B (in control). User B's engagement with that post is a treatment effect bleeding into the control group. We use geo-level randomization for tests involving social sharing: entire cities or regions are assigned to control/treatment, eliminating cross-group social graph interference. (3) North star metrics: we weight long-term metrics (7-day retention, 30-day active rate) over short-term engagement metrics. Short-term CTR is easy to optimize by showing sensational content; 30-day retention reveals whether that content kept users happy long-term.

**Q10: Describe how you would implement a "time capsule" feature where certain evergreen posts should re-surface in feeds annually.**

A: The "time capsule" feature requires an override mechanism that injects specific posts into feeds regardless of their age: (1) **Time capsule table**: a separate table `time_capsule_posts (user_id, post_id, original_date, resurface_date, source)` stores posts scheduled for resurfacing. `source` can be `'user_created'` (user explicitly saved it), `'system_anniversary'` (system detects a post from exactly 1 year ago was highly engaged by this user), or `'creator_highlighted'` (creator marks a post for periodic resurfacing). (2) **Injection at candidate generation**: at the start of each feed request, the system checks Redis for pending time capsule entries: `feed:capsule:{user_id}:pending` (ZSET with resurface_date as score). Any entries with resurface_date ≤ now are fetched and injected into the candidate set bypassing the freshness filter. (3) **Bypass freshness decay**: time capsule posts receive a fixed freshness_score=0.8 override (ignoring their actual age). They compete on engagement quality and affinity signals only. (4) **Deduplication**: once a time capsule post is shown (user scrolls past it in the feed), it's removed from the pending set to prevent repeated resurfacing in the same session.

**Q11: How would you measure the long-term effect of feed ranking on user well-being?**

A: Standard engagement metrics (CTR, time spent) are poor proxies for well-being — users may spend more time on the app while feeling worse afterward. Measuring well-being requires: (1) **In-app mood surveys**: for a randomly sampled 1% of users, show a brief "How do you feel after your last session?" survey (5-point scale, optional). These survey responses are joined with the feed ranking log to build a "post-session satisfaction" dataset. This is the most direct well-being signal but suffers from selection bias (users who respond to surveys may differ from those who don't). (2) **Passive behavioral proxies**: behaviors correlated with positive well-being: returning to the app 24h later (active re-engagement vs compulsive scrolling at midnight), voluntary sharing of content with friends (rather than angry reactions), writing substantive comments (vs one-word reactions). Behaviors correlated with negative well-being: opening and immediately closing the app (checking without satisfaction), high angry-reaction rates, unfollowing. (3) **Long-term churn analysis**: users who leave the platform permanently within 90 days, segmented by their last-30-day feed quality metrics. If users who received high-velocity outrage content are 2× more likely to churn, that's a signal that the ranking is harming well-being and long-term business outcomes simultaneously. (4) **Third-party research partnerships**: partner with academic researchers (IRB-approved) to conduct longitudinal studies correlating feed ranking experiments with validated psychological well-being scales (e.g., WHO-5 Well-Being Index).

**Q12: How would you handle a large-scale coordinated migration of users from a competitor platform (e.g., 50 million users sign up in 72 hours)?**

A: A sudden 50 M user influx has three main impact areas: (1) **Registration and onboarding capacity**: not directly part of the feed system, but the feed needs cold-start handling for 50 M simultaneous new users. Our onboarding quiz and content-based cold-start path is triggered for all users with < 10 interactions. We pre-warm Redis with trending content caches and ensure the cold-start path is optimized (no FAISS ANN needed; just Redis trending reads). (2) **Infrastructure scaling**: auto-scaling triggers in response to the RPS surge. Key bottleneck: the spam classifier must score 50 M new accounts' posts; we deploy emergency GPU capacity for the BERT-based classifier and triage new accounts through a parallel "new account review" queue. New accounts receive a temporary "new user" ranking modifier (posts from new accounts are shown with a slight suppression until the account accumulates 30 days of history, to prevent spam flooding). (3) **Feature store warming**: 50 M new user feature records must be provisioned in Redis. With no historical data, they get default values (preferred_topics = [], avg_session_len = 300s, country_code from IP). Redis capacity headroom is consumed. Emergency scaling: provision 8 additional Redis shards (from the cloud provider's elasticache) within 10 minutes via Terraform automation. Feature records are initially written to the new shards; hash slots are rebalanced automatically by Redis Cluster.

**Q13: How would you design the feed to support multiple languages in the same feed for bilingual users?**

A: Bilingual users want content in both their languages, but don't want a feed dominated by one at the expense of the other: (1) **Language profile**: each user has a `language_preferences` array with weights: `[{lang: 'en', weight: 0.6}, {lang: 'es', weight: 0.4}]`. These weights are learned from the user's engagement history (what fraction of their clicks were on English vs Spanish posts). (2) **Language diversity constraint**: in the diversity enforcement stage, we enforce that the feed contains posts in at least N languages proportional to the user's language weights. For the 60/40 English/Spanish user: in top-25 posts, at least 8 must be Spanish (40% of 20 scored posts, with some buffer). (3) **Cross-lingual embedding**: the ranking model's content features use a multilingual embedding (XLM-RoBERTa) that represents content semantics independent of language. A Spanish-language post about "Machine Learning" is close in embedding space to an English-language post about the same topic. This allows the ANN candidate generation to find relevant content across language boundaries. (4) **Fallback for low-content languages**: if insufficient high-quality content exists in the user's secondary language to fill the language quota, the quota is relaxed and a "discover more [language] content" prompt is shown to encourage following more creators in that language.

**Q14: How does your system avoid optimizing for "engagement addiction" — where users compulsively scroll even when they would prefer to stop?**

A: Optimizing for time-spent-in-app creates incentives to show content that keeps users scrolling compulsively, which can harm well-being. We address this through objective design: (1) **Cap-and-plateau objective**: our training label caps per-session time contribution at 30 minutes. Sessions longer than 30 minutes do not contribute more positive signal — the system isn't rewarded for keeping users past a natural stopping point. (2) **"I'm done" implicit signal**: if a user scrolls past 100 posts without a single click or reaction, we treat the last 50 posts as "implicit skips" — the user is over-scrolling, not genuinely engaging. These are negative training signals. (3) **Session closure prediction**: we train a secondary model to predict "is the user about to close the app?" If the model's close-prediction score exceeds 0.8, the feed serves a "natural stopping point" design — a slightly different visual treatment and a higher bar for the quality of the next 3 posts shown (only exceptional content is shown near the session end). (4) **User-controlled limits**: users can set a "daily feed limit" (e.g., 45 minutes) in settings. When reached, the feed shows a friction screen before loading more content. The feed ranking system is aware of the limit and slightly biases toward longer-form, higher-quality content as the limit approaches, rather than snackable short-form content that encourages continued scrolling.

**Q15: How would you design the observability system to catch subtle ranking quality degradation that doesn't show up as an error rate?**

A: Silent quality degradation (the model still works but recommendations get worse) is harder to detect than errors. Multi-layer observability: (1) **Engagement metric monitoring with statistical control charts**: we use CUSUM (Cumulative Sum) control charts on CTR and dwell time (rather than simple threshold alerts). CUSUM detects gradual trends (a 0.1% daily CTR decline over 2 weeks) that threshold alerts miss. Alert fires when the CUSUM statistic exceeds 5σ. (2) **Model prediction distribution monitoring**: the distribution of model output scores should be stable. We run a two-sample Kolmogorov-Smirnov test daily comparing this week's score distribution to the previous week's. A statistically significant shift (p < 0.01) flags potential model drift or data pipeline issues. (3) **Feature importance drift**: weekly SHAP analysis identifies which features the model is most relying on. If the top-3 most important features change significantly week-over-week, it indicates the training data distribution shifted. (4) **Diversity entropy monitoring**: if the Shannon entropy of topic distribution in served feeds drops below a threshold (indicating the feed is becoming less diverse), this is a quality signal independent of engagement. (5) **Canary users**: a 0.01% "canary" user group receives the production model but with their engagement data specially labeled. A human reviewer looks at 50 canary users' feeds weekly and rates their quality on a 1–5 scale. This human evaluation catches qualitative issues (e.g., "feeds feel repetitive") that quantitative metrics miss.

---

## 12. References & Further Reading

1. **Bakshy, E., Messing, S., & Adamic, L.A. (2015).** Exposure to Ideologically Diverse News and Opinion on Facebook. *Science, 348(6239), 1130–1132.* https://www.science.org/doi/10.1126/science.aaa1160
   — Empirical analysis of the filter bubble in news feed ranking; cited in diversity design discussions.

2. **Anil, R. et al. (2022).** Large Scale Learning on Non-Homophilous Graphs: New Benchmarks and Strong Simple Methods. *NeurIPS 2022 / Meta Engineering Blog.*
   — Facebook's TAO system for social graph serving and the fan-out architecture at scale.

3. **Meta Engineering Blog. (2021).** How Facebook serves billions of personalized News Feeds. https://engineering.fb.com/2021/01/26/ml-applications/news-feed-ranking/
   — Meta's multi-stage ranking pipeline; integrity and quality signals integration.

4. **Twitter Engineering Blog. (2023).** Twitter's Recommendation Algorithm (Open Source). https://github.com/twitter/the-algorithm
   — Twitter's open-sourced ranking algorithm; two-tower candidate generation + LightGBM ranker; real-world system at scale.

5. **Covington, P., Adams, J., & Sargin, E. (2016).** Deep Neural Networks for YouTube Recommendations. *ACM RecSys 2016.* https://dl.acm.org/doi/10.1145/2959100.2959190
   — Multi-stage ranking pipeline principles; training on implicit feedback; position bias mitigation.

6. **Zhao, Z. et al. (2019).** Recommending What Video to Watch Next: A Multitask Ranking System. *ACM RecSys 2019.* https://dl.acm.org/doi/10.1145/3298689.3346997
   — MTL ranking with position bias and implicit feedback; basis for multi-task DNN ranker design.

7. **Ma, J. et al. (2018).** Modeling Task Relationships in Multi-task Learning with Multi-gate Mixture-of-Experts. *ACM KDD 2018.* https://dl.acm.org/doi/10.1145/3219819.3220007
   — MMoE architecture for multi-task feed ranking; Google's approach to balancing CTR, dwell, and share objectives.

8. **Xu, J. et al. (2020).** Neural Input Search for Large Scale Recommendation Models. *ACM KDD 2020.*
   — Feature selection and neural architecture search for large-scale feed ranking at Alibaba.

9. **Diuk, C. (2016).** Helping People Have More Meaningful Social Interactions. *Meta Research.* https://research.facebook.com/blog/2016/12/news-feed-fyi-helping-make-sure-you-dont-miss-stories-from-friends-and-family/
   — Facebook's engagement quality improvements; moving beyond CTR to meaningful interactions.

10. **Wilsongranville, A. et al. (2021).** Revisiting Semi-supervised Learning with Graph Embeddings. *arXiv 2107.03036.*
    — Graph neural approaches to social signal amplification in feed ranking.

11. **Dean, J. (2012).** Building Software Systems at Google and Lessons Learned. *Stanford EE 380 Lecture.*
    — Distributed systems architecture principles underlying large-scale feed ranking infrastructure.

12. **Karimi, F. et al. (2018).** Homophily Influences Ranking of Minorities in Social Networks. *Scientific Reports, 8(1), 11077.* https://www.nature.com/articles/s41598-018-29405-7
    — Quantitative analysis of how feed ranking algorithms can systematically disadvantage minority creators.

13. **He, X., & Daumé III, H. (2012).** Imbalanced Classification via Imbalanced Sampling. *ICML 2012.*
    — Foundation for handling the extreme class imbalance in feed engagement training (few clicks vs. many non-clicks).

14. **Sculley, D. et al. (2015).** Hidden Technical Debt in Machine Learning Systems. *NeurIPS 2015.* https://proceedings.neurips.cc/paper/2015/hash/86df7dcfd896fcaf2674f757a2463eba-Abstract.html
    — Training-serving skew and ML system design debt; essential reading for feed ranking system design.

15. **Hausman, J., & Abrevaya, J. (2000).** Measuring the Effects of the Internet on News Preferences: Randomized Experiments. *Econometrica.*
    — Economic methodology for causal inference in platform experiments; relevant to long-term well-being measurement in feed ranking A/B tests.

16. **Instagram Engineering Blog. (2019).** Powered by AI: Instagram's Explore recommender system. https://engineering.fb.com/2019/11/25/core-infra/instagram-explore/
    — Two-tower candidate generation at Instagram scale; multi-stage pipeline with separate candidate sources.

17. **Lindsey, M. et al. (2023).** Responsible Innovation in AI at Scale: Lessons from News Feed Ranking. *ACM FAccT 2023.*
    — Fairness, accountability, and transparency considerations specifically in news feed ranking systems.
