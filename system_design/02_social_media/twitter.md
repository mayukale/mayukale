# System Design: Twitter (X)

---

## 1. Requirement Clarifications

### Functional Requirements
- Users can post tweets (up to 280 characters, optionally with media attachments)
- Users can follow/unfollow other users
- Users see a home timeline composed of tweets from people they follow
- Users can like, retweet, quote-tweet, and reply to tweets
- Users can search tweets and hashtags
- Trending topics surface globally and by region
- Users receive notifications (likes, retweets, replies, follows, mentions)
- Direct messaging between users
- User profile pages showing their tweets

### Non-Functional Requirements
- Availability: 99.99% uptime (< 52 min downtime/year) — social media tolerates eventual consistency over strict consistency
- Timeline reads must be fast: p99 < 200ms — users notice feed lag immediately
- Tweet post write path: p99 < 500ms — acceptable slight delay on write
- Eventual consistency on timelines — it is acceptable for a newly posted tweet to appear in follower feeds within a few seconds
- Tweets are durable once acknowledged — no data loss on confirmed writes
- System must scale to handle celebrities with 100M+ followers (celebrity/hotspot problem)
- Timelines personalized by recency (and optionally relevance)
- Trending topics updated in near real-time (within 60 seconds of trending spike)

### Out of Scope
- Twitter ads serving and ad auction system
- Twitter Spaces (live audio)
- Twitter Blue / subscription billing
- Content moderation / trust and safety pipeline
- Email/SMS notification infrastructure
- OAuth and third-party app authorization flows

---

## 2. Users & Scale

### User Types
- **Readers**: Browse timelines, search, explore trending — overwhelmingly the majority of requests
- **Writers**: Post tweets, reply, retweet — active but far fewer than reads
- **Celebrities/Power Users**: Accounts with millions of followers that create hotspot problems
- **Bots/Automated accounts**: High-volume programmatic API users

### Traffic Estimates

| Metric | Value | Reasoning |
|--------|-------|-----------|
| MAU | 400 million | Assumption based on publicly discussed scale |
| DAU | 200 million | ~50% of MAU active daily, industry typical ratio |
| Tweets/day | 500 million | ~2.5 tweets/day per active user average, consistent with public numbers |
| Timeline reads/day | 20 billion | 200M DAU × 100 timeline refreshes/day avg |
| Reads/sec (QPS) | ~231,000 | 20B / 86,400 seconds |
| Writes/sec (QPS) | ~5,800 | 500M / 86,400 seconds |
| Peak multiplier | 3–5x | Major events (elections, sports finals) drive spike traffic |
| Peak read QPS | ~1,000,000 | 231K × ~4.5 peak |
| Peak write QPS | ~25,000 | 5,800 × ~4.5 peak |

### Latency Requirements
- **Read p99: 200ms** — Home timeline is a user-facing, synchronous request. Studies show 100–300ms is perceived as "instant." Exceeding 300ms causes visible lag and user frustration.
- **Write p99: 500ms** — Tweet post involves fanout; slightly higher latency is acceptable since the user sees immediate optimistic UI feedback before full fanout completes.
- **Trending topics: 60s update lag** — Near-real-time is required but strict sub-second is not; batch aggregation over windows is acceptable.

### Storage Estimates

| Data Type | Size/record | Records/day | Retention | Total |
|-----------|-------------|-------------|-----------|-------|
| Tweet text | ~500 bytes | 500M | Forever | ~250 GB/day → ~90 TB/year |
| Tweet metadata (likes, RT counts) | ~200 bytes | 500M | Forever | ~100 GB/day |
| User profile | ~1 KB | ~1M new users/day | Forever | ~1 GB/day |
| Follow graph edges | ~16 bytes (userID pairs) | ~50M new edges/day | Forever | ~800 MB/day |
| Media (images avg 300KB, video avg 3MB) | ~500 KB avg | ~50M media tweets/day | Forever | ~25 TB/day media |
| Timeline cache (precomputed) | ~1 KB/user | 200M active | Rolling 7 days | ~200 GB cache |
| Search index | ~1 KB/tweet | 500M tweets/day | 30 days rolling | ~15 TB index |

**Total persistent storage growth:** ~25 TB/day driven primarily by media. Non-media metadata ~1 TB/day.

### Bandwidth Estimates

| Direction | Calculation | Result |
|-----------|-------------|--------|
| Inbound tweet text | 500M tweets/day × 500B / 86400 | ~2.9 MB/s |
| Inbound media uploads | 50M media tweets × 500KB / 86400 | ~290 MB/s |
| Outbound timeline reads (text) | 231K QPS × 10 tweets × 500B | ~1.1 GB/s |
| Outbound media delivery (CDN origin fill) | 10% of reads fetch media, avg 300KB | ~7 GB/s origin; CDN cache offloads ~90% |
| Total outbound (CDN included) | | ~8 GB/s peak at origin, ~700 MB/s with CDN |

---

## 3. High-Level Architecture

```
                          ┌──────────────────────────────────────────────────────────┐
                          │                     CLIENTS                              │
                          │         iOS / Android / Web Browser                      │
                          └────────────────────────┬─────────────────────────────────┘
                                                   │ HTTPS
                                                   ▼
                          ┌──────────────────────────────────────────────────────────┐
                          │                  CDN (Cloudflare / Akamai)               │
                          │  Caches: static assets, media, public profile pages      │
                          └────────────────────────┬─────────────────────────────────┘
                                                   │ Cache miss / API
                                                   ▼
                          ┌──────────────────────────────────────────────────────────┐
                          │              API Gateway / Load Balancer                  │
                          │  TLS termination, rate limiting, auth token validation    │
                          │  Routes to downstream microservices                       │
                          └────┬──────────────┬──────────────┬────────────────────────┘
                               │              │              │
              ┌────────────────▼──┐   ┌───────▼──────┐  ┌───▼─────────────────┐
              │  Tweet Service    │   │ Timeline Svc  │  │  User/Follow Svc    │
              │  - Write tweet    │   │ - Read feed   │  │  - Follow/Unfollow  │
              │  - Retweet/Reply  │   │ - Fan-out     │  │  - Profile CRUD     │
              └──────────┬────────┘   └───────┬───────┘  └──────────┬──────────┘
                         │                    │                      │
          ┌──────────────▼────────────────────▼──────────────────────▼───────────┐
          │                        Message Queue (Kafka)                          │
          │   Topics: tweet.created, tweet.deleted, follow.event, fanout.tasks   │
          └───────┬───────────────────────────┬────────────────────────────────────┘
                  │                           │
   ┌──────────────▼─────────┐   ┌─────────────▼────────────────────────────┐
   │  Fanout Worker Service  │   │  Trend Aggregation Service (Flink/Storm) │
   │  - Push tweet to        │   │  - Count hashtag/keyword frequency        │
   │    follower timelines   │   │  - Sliding window aggregation             │
   │  - Hybrid fan-out logic │   │  - Write to Trend Cache (Redis)           │
   └──────────┬──────────────┘   └──────────────────────────────────────────┘
              │
              ▼
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │                           DATA STORES                                        │
 │                                                                               │
 │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────────┐   │
 │  │  Tweet Store      │  │  Timeline Cache  │  │  Follow Graph DB         │   │
 │  │  (Cassandra)      │  │  (Redis Cluster) │  │  (MySQL + Social Graph)  │   │
 │  │  Sharded by       │  │  Key: userID     │  │  Adjacency list sharded  │   │
 │  │  tweet_id         │  │  Val: tweet_id[] │  │  by follower_id          │   │
 │  └──────────────────┘  └──────────────────┘  └──────────────────────────┘   │
 │                                                                               │
 │  ┌──────────────────────────────────────────────────────────────────────┐    │
 │  │  Object Store (S3/GCS) — Media Storage                               │    │
 │  │  CDN serves media; origin on S3; transcoded versions stored          │    │
 │  └──────────────────────────────────────────────────────────────────────┘    │
 │                                                                               │
 │  ┌──────────────────────────────────────────────────────────────────────┐    │
 │  │  Search Index (Elasticsearch) — Tweet full-text + hashtag search     │    │
 │  └──────────────────────────────────────────────────────────────────────┘    │
 └─────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **CDN**: Serves static assets and cached media. Reduces origin load by ~90% for media traffic.
- **API Gateway**: Single entry point for all clients. Handles auth (JWT/OAuth token validation), rate limiting (per-user and per-IP), and routing to microservices.
- **Tweet Service**: Handles all tweet writes. Validates content, persists to Cassandra, publishes `tweet.created` event to Kafka.
- **Timeline Service**: Handles home timeline reads. Reads precomputed timeline from Redis cache. Falls back to fan-out on read for cache misses.
- **User/Follow Service**: Manages user profiles and the follow graph. Updates adjacency lists on follow/unfollow events.
- **Kafka**: Durable, ordered message bus decoupling write path from fanout. Topics are partitioned to allow parallel consumption.
- **Fanout Worker**: Consumes `tweet.created` events. For normal users: pushes tweet_id to each follower's Redis timeline list. For celebrities: skips push, uses pull at read time.
- **Trend Aggregation Service**: Consumes tweet stream, counts hashtag frequency over 5-minute sliding windows using Flink, writes results to Redis sorted set.
- **Cassandra**: Primary tweet store. Chosen for write-heavy workload, horizontal scaling, and tunable consistency.
- **Redis Cluster**: Holds precomputed timeline lists (arrays of tweet_ids per user). Also stores trending topics in sorted sets.
- **Follow Graph DB**: MySQL with read replicas stores follow relationships. Fits in-memory for hot users.
- **Elasticsearch**: Powers search with inverted index on tweet text, hashtags, and user handles.

**End-to-End Flow — Posting a Tweet:**
1. Client sends `POST /tweets` with auth token to API Gateway.
2. API Gateway validates token, applies rate limit, routes to Tweet Service.
3. Tweet Service validates content (length, media refs), assigns tweet_id (Snowflake ID), persists to Cassandra.
4. Tweet Service publishes `tweet.created` event to Kafka topic.
5. Tweet Service returns `200 OK` with tweet object to client (optimistic).
6. Fanout Worker reads event from Kafka. Looks up follower list from Follow Graph DB.
7. For each follower (if normal user): prepend tweet_id to their Redis timeline list (capped at 800 entries).
8. For celebrity accounts (>1M followers): skip push fanout; timeline reads will pull and merge in real-time.
9. Trend Aggregation Service independently processes the tweet's hashtags.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Tweet Store (Cassandra — wide-column, optimized for reads
-- by user_id and tweet_id range scans)
-- ============================================================
CREATE TABLE tweets (
    tweet_id     BIGINT,       -- Snowflake ID: encodes timestamp + datacenter + sequence
    user_id      BIGINT,       -- Author's user ID
    body         TEXT,         -- Up to 280 chars (UTF-8, ~560 bytes max)
    media_keys   LIST<TEXT>,   -- S3 object keys for attached media (0–4 items)
    reply_to_id  BIGINT,       -- tweet_id of parent tweet, NULL if top-level
    retweet_of_id BIGINT,      -- tweet_id of original if this is a retweet
    like_count   COUNTER,      -- Cassandra counter column
    retweet_count COUNTER,
    reply_count  COUNTER,
    is_deleted   BOOLEAN DEFAULT FALSE,
    created_at   TIMESTAMP,
    PRIMARY KEY (tweet_id)     -- Partition by tweet_id for point lookups
);

-- For user profile page (all tweets by a user, time-ordered)
CREATE TABLE tweets_by_user (
    user_id      BIGINT,
    tweet_id     BIGINT,       -- Snowflake, so naturally time-ordered descending
    created_at   TIMESTAMP,
    PRIMARY KEY (user_id, tweet_id)  -- Partition by user, cluster by tweet_id DESC
) WITH CLUSTERING ORDER BY (tweet_id DESC);

-- ============================================================
-- User Store (MySQL — relational, strong consistency needed
-- for login, billing, profile uniqueness)
-- ============================================================
CREATE TABLE users (
    user_id      BIGINT PRIMARY KEY AUTO_INCREMENT,
    handle       VARCHAR(15) UNIQUE NOT NULL,  -- @username
    display_name VARCHAR(50),
    bio          VARCHAR(160),
    avatar_key   VARCHAR(255),                  -- S3 object key
    follower_count INT DEFAULT 0,
    following_count INT DEFAULT 0,
    is_verified  BOOLEAN DEFAULT FALSE,
    is_celebrity BOOLEAN DEFAULT FALSE,         -- Drives fan-out strategy
    created_at   DATETIME DEFAULT NOW(),
    INDEX idx_handle (handle)
);

-- ============================================================
-- Follow Graph (MySQL — adjacency list, read replica reads)
-- ============================================================
CREATE TABLE follows (
    follower_id  BIGINT NOT NULL,
    followee_id  BIGINT NOT NULL,
    created_at   DATETIME DEFAULT NOW(),
    PRIMARY KEY (follower_id, followee_id),     -- PK ensures uniqueness + fast lookup
    INDEX idx_followee (followee_id, follower_id)  -- Reverse: "who follows this person?"
);

-- ============================================================
-- Timeline Cache (Redis — not a SQL table, shown as logical)
-- Key:   "timeline:{user_id}"
-- Type:  Redis Sorted Set (score = tweet_id for time ordering)
-- Value: tweet_ids (up to 800 most recent)
-- TTL:   7 days of inactivity before eviction
-- ============================================================

-- ============================================================
-- Notifications (Cassandra — append-heavy, fan-out read)
-- ============================================================
CREATE TABLE notifications (
    user_id      BIGINT,
    notif_id     BIGINT,        -- Snowflake
    type         VARCHAR(20),   -- 'like', 'retweet', 'reply', 'follow', 'mention'
    actor_id     BIGINT,        -- Who triggered it
    object_id    BIGINT,        -- Tweet or user ref
    is_read      BOOLEAN DEFAULT FALSE,
    created_at   TIMESTAMP,
    PRIMARY KEY (user_id, notif_id)
) WITH CLUSTERING ORDER BY (notif_id DESC);

-- ============================================================
-- Trending Topics (Redis Sorted Set — not SQL)
-- Key:   "trending:{region}:{window}"  e.g., "trending:US:5m"
-- Score: frequency count
-- TTL:   6 minutes (refreshed every 5 minutes by Flink job)
-- ============================================================

-- ============================================================
-- Search Index (Elasticsearch — not SQL)
-- Index: "tweets"
-- Mapping: tweet_id, user_id, body (analyzed), hashtags (keyword),
--          created_at, like_count, retweet_count
-- ============================================================
```

### Database Choice

| Database | Use Case | Pros | Cons |
|----------|----------|------|------|
| Cassandra | Tweet storage, notifications | Linear horizontal scaling, tunable consistency, high write throughput, wide-column suits time-series | No joins, limited secondary indexes, eventual consistency by default |
| MySQL (+ read replicas) | Users, follow graph | ACID guarantees for user accounts, familiar ops tooling, handles millions of rows fine | Vertical scaling limits, join performance degrades at very high scale |
| Redis | Timeline cache, trending | Sub-millisecond reads, Sorted Set natively handles ranked tweet_id lists, pub/sub for real-time notifs | Data size limited by RAM, persistence is secondary |
| Elasticsearch | Full-text tweet search | Inverted index for free-text, hashtag, handle search; near-real-time indexing | Not the source of truth, complex cluster management, expensive at scale |
| S3/GCS | Media blobs | Infinitely scalable object storage, cheap at scale, pairs with CDN | Not queryable, eventual consistency on overwrites |

**Decision:** Cassandra for tweets because: (1) tweets are append-only (no UPDATE after write), which eliminates compaction concerns; (2) Cassandra's LSM-tree storage engine handles write-heavy workloads without B-tree write amplification; (3) horizontal sharding by tweet_id requires no cross-shard joins; (4) tunable consistency allows `QUORUM` writes with `ONE` reads for timeline fan-out, matching our eventual consistency requirement.

---

## 5. API Design

```
POST /v1/tweets
  Description: Create a new tweet
  Auth: Bearer token (required)
  Rate limit: 300 tweets/3hr per user (header: X-RateLimit-Remaining)
  Request:  {
    body: string (max 280 chars),
    media_ids: string[] (optional, max 4, from /v1/media/upload),
    reply_to_id: string | null,
    retweet_of_id: string | null
  }
  Response 201: {
    tweet_id: string,
    user_id: string,
    body: string,
    created_at: ISO8601,
    media_urls: string[]
  }
  Response 429: { error: "rate_limit_exceeded", retry_after: 120 }

GET /v1/tweets/{tweet_id}
  Description: Fetch a single tweet by ID
  Auth: Optional (public tweets visible to unauthenticated)
  Response 200: {
    tweet_id: string,
    author: { user_id, handle, display_name, avatar_url, is_verified },
    body: string,
    like_count: int,
    retweet_count: int,
    reply_count: int,
    created_at: ISO8601,
    media: [{ url, type, width, height }]
  }

DELETE /v1/tweets/{tweet_id}
  Description: Soft-delete a tweet (sets is_deleted=true)
  Auth: Bearer token (must be tweet author or admin)
  Response 204: (no content)

GET /v1/timeline/home
  Description: Get authenticated user's home timeline
  Auth: Bearer token (required)
  Query params:
    cursor: string (opaque, encodes last seen tweet_id for pagination)
    count: int (default 20, max 200)
    include_retweets: bool (default true)
  Response 200: {
    tweets: Tweet[],
    next_cursor: string | null,
    previous_cursor: string | null
  }
  Cache-Control: no-store (personalized, not cacheable at CDN)

GET /v1/timeline/user/{user_id}
  Description: Get a user's public tweet timeline
  Auth: Optional
  Query params: cursor, count
  Response 200: { tweets: Tweet[], next_cursor: string | null }
  Cache-Control: max-age=60 (public profile cacheable)

POST /v1/tweets/{tweet_id}/like
  Description: Like a tweet
  Auth: Bearer token (required)
  Response 200: { liked: true, like_count: int }
  Idempotent: repeated calls return 200 (not 4xx)

DELETE /v1/tweets/{tweet_id}/like
  Description: Unlike a tweet
  Auth: Bearer token (required)
  Response 200: { liked: false, like_count: int }

POST /v1/follows
  Description: Follow a user
  Auth: Bearer token (required)
  Request:  { followee_id: string }
  Response 201: { following: true }

DELETE /v1/follows/{followee_id}
  Description: Unfollow a user
  Auth: Bearer token (required)
  Response 200: { following: false }

GET /v1/search/tweets
  Description: Full-text search across tweets
  Auth: Optional
  Query params:
    q: string (required, min 2 chars)
    lang: string (optional, ISO 639-1)
    since: ISO8601 date
    until: ISO8601 date
    cursor: string
    count: int (default 20, max 100)
  Response 200: {
    tweets: Tweet[],
    next_cursor: string | null,
    total_hits: int
  }

GET /v1/trends
  Description: Get trending topics
  Auth: Optional
  Query params:
    region: string (ISO 3166-1 alpha-2, default "worldwide")
    count: int (default 20, max 50)
  Response 200: {
    trends: [{ hashtag: string, tweet_volume: int, rank: int }],
    as_of: ISO8601
  }
  Cache-Control: max-age=60

POST /v1/media/upload
  Description: Upload media before attaching to tweet
  Auth: Bearer token (required)
  Content-Type: multipart/form-data
  Request: { file: binary, media_type: "image/jpeg"|"image/png"|"video/mp4" }
  Response 200: { media_id: string, expires_at: ISO8601 }
  Note: media_id valid for 24h; must be used in tweet post within that window
```

---

## 6. Deep Dive: Core Components

### Component: Fan-Out Service (Timeline Generation)

**Problem it solves:**
When a user posts a tweet, their followers need to see it in their home timeline. There are two extremes: (A) push the tweet to every follower's timeline immediately on write (fan-out on write), or (B) compute the timeline dynamically at read time by fetching tweets from everyone the user follows (fan-out on read). Twitter's challenge is that neither pure approach works at scale — a celebrity with 10M followers makes fan-out on write catastrophically expensive, while a user who follows 5,000 accounts makes fan-out on read slow.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Fan-out on Write (Push) | On post, push tweet_id to every follower's Redis timeline list | Read is O(1) — just fetch precomputed list | Write fan-out for celebrities is O(followers) — 10M Redis writes per tweet |
| Fan-out on Read (Pull) | At read time, fetch tweets from all followees, merge, sort | Write is O(1) — no fan-out | Read is O(followees × tweets_per_user) — slow with many follows |
| Hybrid (Twitter's actual approach) | Fan-out on write for normal users; fan-out on read for celebrities | Balances write and read costs | Complex routing logic; celebrity detection adds ops overhead |
| Pre-computed with lazy invalidation | Store full timeline, invalidate on any event | Fast reads | Stale data risk; complex invalidation |

**Selected Approach: Hybrid Fan-Out**

Normal users (< 1M followers): use fan-out on write. Fanout Worker reads follower list, prepends tweet_id to each follower's Redis Sorted Set (score = tweet_id since Snowflake IDs are time-ordered). This is O(followers) writes per tweet, but capped at ~1M and done asynchronously via Kafka — acceptable.

Celebrity users (> 1M followers, or configurable threshold): skip push fan-out entirely. Their tweets are NOT pushed to follower timelines. Instead, at Timeline Service read time, the system identifies which celebs the requesting user follows, fetches their recent tweets from Cassandra (fast, indexed by user_id), and merges them into the timeline result.

**Implementation Detail:**

```python
# Fanout Worker (Kafka consumer)
def handle_tweet_created(event):
    tweet_id = event['tweet_id']
    author_id = event['user_id']

    user = user_service.get_user(author_id)

    if user.follower_count > CELEBRITY_THRESHOLD:  # e.g., 1,000,000
        # Skip push fanout. Timeline reads will pull this user's tweets.
        # Index tweet in Cassandra tweets_by_user only.
        return

    # Normal user: fan out to followers in batches
    cursor = None
    while True:
        followers, cursor = follow_service.get_followers(
            user_id=author_id,
            cursor=cursor,
            batch_size=1000
        )
        if not followers:
            break

        pipeline = redis.pipeline()
        for follower_id in followers:
            key = f"timeline:{follower_id}"
            # Sorted set: score=tweet_id (Snowflake = monotonic), member=tweet_id
            pipeline.zadd(key, {tweet_id: tweet_id})
            # Cap at 800 entries to bound memory
            pipeline.zremrangebyrank(key, 0, -801)
            # Refresh TTL
            pipeline.expire(key, 7 * 24 * 3600)
        pipeline.execute()

# Timeline Service (read path)
def get_home_timeline(user_id, cursor=None, count=20):
    # Step 1: Get precomputed timeline from Redis
    key = f"timeline:{user_id}"
    max_score = cursor if cursor else '+inf'
    tweet_ids = redis.zrevrangebyscore(key, max_score, '-inf',
                                        withscores=False, start=0, num=count)

    # Step 2: Identify celebrities the user follows
    celebrity_ids = follow_service.get_followed_celebrities(user_id)

    # Step 3: Fetch recent tweets from celebrities directly
    celebrity_tweet_ids = []
    for celeb_id in celebrity_ids:
        recent = cassandra.get_tweets_by_user(celeb_id, limit=20)
        celebrity_tweet_ids.extend([t.tweet_id for t in recent])

    # Step 4: Merge + deduplicate + sort by tweet_id (time order)
    all_ids = sorted(set(list(tweet_ids) + celebrity_tweet_ids), reverse=True)
    paginated = all_ids[:count]

    # Step 5: Hydrate tweet objects (multi-get from Cassandra + cache)
    tweets = tweet_service.multi_get(paginated)
    next_cursor = paginated[-1] if len(paginated) == count else None

    return tweets, next_cursor
```

**Interviewer Q&As:**

Q: What happens when a previously normal user crosses the celebrity threshold?
A: We run a background migration job: scan the user's followers, remove the user's tweet_ids from all follower timeline caches (or simply let TTL expire), and flip the `is_celebrity` flag. After the flag is set, new tweets no longer fan out. Timeline reads immediately start pulling from their Cassandra record. The transition window may have slight inconsistency (some followers have cached tweets, others pull from Cassandra), which is acceptable.

Q: How do you handle the timeline for a user who follows 5,000 accounts, 200 of which are celebrities?
A: The non-celebrity 4,800 accounts have their tweets fan-outted to the user's Redis timeline normally. The 200 celebrities are merged at read time. We fetch the last 20 tweets from each celebrity's Cassandra row (parallelized with async I/O), merge with the Redis list, sort, and paginate. With 200 celeb accounts × 20 tweets each = 4,000 tweet IDs fetched, then merged with the Redis list of 800 IDs — all in memory, sub-millisecond sort.

Q: What if a user is inactive for weeks and their timeline cache expires?
A: On first access after cache expiry, Timeline Service detects a Redis cache miss. It falls back to fan-out on read: queries Follow Graph DB for all followees, fetches recent tweets from Cassandra for each, merges, and writes result back to Redis. This "cold start" may take 1–2 seconds; we can show a loading state. This is acceptable since it affects only returning inactive users.

Q: How does the fan-out worker handle backpressure during a viral event?
A: Kafka provides natural backpressure — the Fanout Worker consumes at its own pace. We scale Fanout Workers horizontally by adding consumer instances (Kafka consumer group auto-rebalances partitions). For extreme events (e.g., breaking news), we can temporarily increase the celebrity threshold to reduce fan-out volume, letting more tweets be handled by pull at read time.

Q: How do you ensure tweet ordering in the merged timeline is correct?
A: Snowflake IDs encode timestamp in the high bits (41 bits for millisecond timestamp), so sorted order of tweet_ids equals chronological order. No separate timestamp comparison needed. This is a key design property of Snowflake: lexicographic sort of tweet_ids is time sort. We ZADD with score = tweet_id, and ZREVRANGEBYSCORE returns newest first.

---

### Component: Trending Topics

**Problem it solves:**
Identify which hashtags and keywords are spiking in usage in near-real-time, broken down by region. "Trending" is defined as unusual acceleration of a term's frequency, not merely high volume (otherwise "#the" would always trend).

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Batch MapReduce (hourly) | Count hashtags every hour with Spark/Hadoop | Simple, accurate | Hour latency — useless for trending |
| Lambda architecture | Batch layer + speed layer (Storm) | Both accuracy and low latency | Ops complexity of maintaining two systems |
| Kappa architecture (Flink) | Pure streaming with long-window aggregation | Single code path, low latency, handles both | State management complexity for large windows |
| Approximate counting (Count-Min Sketch) | Probabilistic frequency estimation per time window | O(1) memory, scalable | Approximate; rare terms may be overcounted |

**Selected Approach: Kappa with Flink + Count-Min Sketch**

Tweets flow through Kafka. Flink consumes the tweet stream, extracts hashtags, and maintains sliding window counts (5-minute window, 1-minute slide) using Count-Min Sketch per geo-cell. Trending score uses velocity: `score = (count_last_5min - avg_count_prev_60min) / stddev`. Terms with score > 3 sigma trend. Results written to Redis sorted sets every 60 seconds.

**Implementation Detail:**

```python
# Flink job pseudocode (conceptual)
stream = kafka_source('tweet.created')

hashtag_stream = stream.flat_map(extract_hashtags_and_geo)
    # Emits: (hashtag, region, timestamp)

# Sliding window: 5-minute window, slide every 1 minute
windowed = hashtag_stream \
    .key_by(lambda x: (x.hashtag, x.region)) \
    .window(SlidingEventTimeWindows.of(minutes(5), minutes(1))) \
    .aggregate(CountMinSketchAggregator())

# Trending score: compare current window to historical baseline
trending = windowed.process(TrendingScoreFunction(
    baseline_window_minutes=60,
    z_score_threshold=3.0
))

# Write top-50 per region to Redis
trending \
    .key_by(lambda x: x.region) \
    .process(Top50Aggregator()) \
    .add_sink(RedisSink(
        key_pattern="trending:{region}",
        score_field="trend_score",
        ttl=360  # 6 minutes
    ))
```

**Interviewer Q&As:**

Q: How do you prevent manipulation of trending topics through coordinated inauthentic behavior?
A: Several mitigations: (1) Weight counts by account age and credibility score — new/bot accounts contribute 0.1x weight. (2) Dedup by IP address per window — if 1000 tweets come from the same /16 subnet, count as 1. (3) Rate limit hashtag frequency per user (max 5 unique hashtags per tweet). (4) Maintain a blocklist of synthetic trending terms detected by ML. (5) Use unique users reaching the hashtag rather than raw tweet count as the primary signal.

Q: Why Count-Min Sketch instead of exact counts?
A: Exact counts require O(unique terms × windows × regions) memory. At 500M tweets/day, with thousands of unique hashtags and 50 regions, exact counts would require hundreds of GB of Flink state. Count-Min Sketch reduces this to O(sketch_width × sketch_depth) — typically 50KB per (region, window) pair with 1% error rate. The error is acceptable because trending detection uses relative velocity, not exact counts; a 1% count error does not change whether a term trends.

Q: How do you handle time zone differences for "trending now" per country?
A: Geo-cell the tweet at ingestion using the user's configured location (or IP geolocation). Each geo-cell runs its own independent Flink sub-stream. "Trending in Japan" uses only tweets geolocated to Japan. Window boundaries align to UTC, but display "trending today" resets at midnight local time per region (a separate reset signal injected into the stream at region-specific UTC offsets).

---

### Component: Snowflake ID Generation

**Problem it solves:**
Generate globally unique, time-ordered, 64-bit tweet IDs at high throughput (25K+ writes/sec peak) without a centralized coordination point that would become a bottleneck.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Auto-increment (MySQL) | Central DB sequence | Simple | Single point of failure; max ~10K/sec per instance |
| UUID v4 | Random 128-bit | Distributed, no coordination | Not time-ordered; 128 bits too large for tweet_id column; random causes B-tree fragmentation |
| Snowflake (Twitter's own) | 64-bit: 41b timestamp + 10b machine_id + 12b sequence | Time-ordered, distributed, 64-bit, 4096 IDs/ms/machine | Machine ID allocation needs coordination (ZooKeeper) |
| ULID | 48b timestamp + 80b random | Time-ordered, URL-safe | 128-bit; overkill for tweet IDs |

**Selected Approach: Snowflake IDs**
- Bit layout: `[1 sign][41 timestamp ms][10 datacenter+machine][12 sequence]`
- Epoch offset from Jan 1, 2010 — 41 bits of millisecond timestamp lasts 69 years from epoch
- 12-bit sequence = 4096 IDs/ms/machine = 4M IDs/sec/machine
- Machine IDs assigned from ZooKeeper at startup; lease refreshed every 30 seconds
- If sequence overflows within a millisecond, spin-wait to next millisecond

---

## 7. Scaling

### Horizontal Scaling

**Tweet Service**: Stateless; scale to N instances behind load balancer. Each instance reads config and connects to Kafka producer. Auto-scale based on CPU > 60% or request queue depth > 100.

**Timeline Service**: Stateless read path. Redis handles state. Scale horizontally; all instances share the same Redis cluster. At 231K read QPS, 50 instances at ~5K QPS each (well within single-process Go/Java capacity).

**Fanout Workers**: Scale Kafka consumer group. Kafka partitions the `tweet.created` topic by `author_id % partitions`. With 100 partitions and 100 consumer instances, each instance handles ~25 fan-outs/sec at average load.

**Follow Service**: Read-heavy. MySQL with 10 read replicas; all follow lookups go to replicas. Write (follow/unfollow) goes to primary. Shard MySQL by `follower_id % shards` when single-instance IOPS becomes limiting (> 10K IOPS).

### Database Scaling

**Sharding:**

*Cassandra (tweets)*: Cassandra natively distributes partitions across nodes using consistent hashing on `tweet_id`. With a 30-node Cassandra cluster and replication factor 3, each node holds ~1/10th of data with 3x redundancy. Vnodes ensure even distribution without manual range assignment.

*MySQL (users)*: Shard by `user_id % N` for user table. Follow graph sharded by `follower_id` for "who do I follow?" queries. Secondary index on `followee_id` handled by scatter-gather across shards (acceptable since follow queries are not latency-critical). Use Vitess for MySQL sharding management.

**Replication:**

*Cassandra*: Replication factor 3 across 3 availability zones. Write with `CONSISTENCY QUORUM` (2/3 acks) — ensures 1-AZ failure tolerance. Read with `CONSISTENCY ONE` for timeline lookups (acceptable eventual consistency). Read with `CONSISTENCY QUORUM` for tweet detail pages where accuracy matters.

*MySQL*: Semi-synchronous replication from primary to replicas (at least 1 replica acknowledges before primary confirms write). Read replicas in same region serve ~95% of reads.

**Caching:**

Three-level caching:
1. **Client-side**: Last 20 tweets cached in mobile app SQLite for offline browsing (TTL: 5 minutes).
2. **Redis L1**: Timeline lists per user (tweet_id arrays). Also caches hot tweet objects by tweet_id (LRU, 32GB RAM cluster → ~64M tweet objects at 500B each).
3. **Redis L2 (tweet object cache)**: Most-read tweets (trending/viral) cached as full JSON blobs. Cache-aside pattern: Timeline Service fetches tweet_ids, then batch-gets objects from Redis; misses fall back to Cassandra multi-get.

Cache invalidation: tweet object cache invalidated by `tweet.updated` Kafka events (for like/RT counter updates). Counters are approximated in cache using Redis INCR and periodically flushed to Cassandra to avoid write amplification on every like.

**CDN:**

- All media (images, videos) served via CDN (Cloudflare Workers / Akamai). Origin is S3.
- CDN POP chosen based on client IP geolocation.
- Cache-Control: `max-age=31536000, immutable` for media (S3 keys include content hash; never mutated).
- Public tweet pages (profile, tweet detail): CDN-cacheable for 60 seconds. Cache key = URL.
- Home timeline: NOT CDN-cacheable (personalized). Served directly from Timeline Service.

### Interviewer Q&As on Scaling

Q: How do you handle hot partitions in Cassandra when a celebrity's tweet_id causes all reads to hit the same partition?
A: Tweet detail reads are by tweet_id, which is uniformly distributed by design (Snowflake ensures monotonic IDs, but Cassandra's token ring distributes them via consistent hash). The hot-read problem on celebrity tweets is solved by the Redis tweet object cache — the tweet blob is cached in Redis, so Cassandra is only hit on a cache miss. With a 99%+ cache hit rate for viral tweets, Cassandra hot partition is not an issue.

Q: Redis timeline caches 800 tweet IDs per user. What if a user follows 10,000 accounts, all active?
A: The cap of 800 entries bounds Redis memory. Users who follow many accounts will have their timeline merged from (a) 800 recent fan-out tweet IDs in Redis and (b) recent celebrity tweets pulled at read time. To serve a user who follows many non-celebrity accounts, we also fall back to a Cassandra scan for accounts not represented in the Redis cache. The UX trade-off: users who follow thousands of accounts may miss some older tweets between refreshes — but this is the same behavior as Twitter today ("in case you missed it" suggestions compensate).

Q: How do you scale the Follow Graph to support lookups of 1M+ followers for a celebrity?
A: The follow graph query "get all followers of celebrity X" is used only by the Fanout Worker — and we skip fan-out for celebrities. The critical queries are: (1) "does user A follow user B?" — O(1) lookup by PK. (2) "give me up to 1000 followers of user X" — a paginated scan of the `followee_id` index. Both are fast. For celebrities opting into fan-out (e.g., verified news accounts), we pre-cache their follower list in Redis as a set, refreshed on follow/unfollow events.

Q: How do you handle write amplification from like/retweet counters being updated millions of times?
A: We never write like counts to Cassandra on every like. Instead: (1) User's like event writes to a Likes table (user_id, tweet_id) for exact uniqueness. (2) Like count is maintained in Redis as a counter (`INCR tweet:likes:{tweet_id}`). (3) A background job (every 5 minutes) flushes Redis counter delta to Cassandra COUNTER column. This reduces Cassandra writes by 99%+ and keeps reads fast (Redis INCR is atomic and sub-millisecond).

Q: Walk me through what happens during a Twitter data center failure.
A: We run across 3 AWS regions (or equivalent). DNS has health checks; Route53 (or equivalent) fails over traffic to the healthy region within 60 seconds via low-TTL DNS records. Cassandra replication across regions (multi-DC replication) ensures no data loss — writes use `LOCAL_QUORUM` in normal operation, and during recovery the surviving region serves reads from its local replicas. MySQL uses asynchronous cross-region replication; RTO < 5 minutes with automated failover (managed by Orchestrator or equivalent). Redis is recreated from Cassandra on cold start (acceptable; timeline cache warms up quickly under real traffic).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Mitigation |
|-----------|-------------|--------|------------|
| Tweet Service instance crash | In-flight writes lost | User tweet may not post | Idempotency key on POST; client retries; Kafka producer has retry |
| Kafka partition leader failure | Tweet events delayed | Fanout delayed up to seconds | Kafka auto-elects new leader (<30s); in-sync replicas preserve messages |
| Redis node failure | Timeline cache miss for affected key range | Timeline reads fall back to Cassandra | Redis Cluster promotes replica automatically (<30s); fallback to Cassandra |
| Cassandra node failure | Read/write latency spike | Some requests time out | RF=3 and QUORUM consistency tolerates 1 node failure; coordinator retries |
| Fanout Worker crash | Some followers don't receive tweet in cache | Timeline stale until TTL expires or re-fanout | Kafka offset not committed; worker restarts and replays from last committed offset |
| Celebrity threshold bug | All users treated as celebrity | Timelines empty (no fan-out) | Monitoring: alert on timeline cache miss rate > 10%; fallback reads from Cassandra |
| Search index unavailable | Search requests fail | Degraded search experience | Circuit breaker returns cached results or empty; search is non-critical path |

### Failover Strategy

- **Multi-region active-active**: Traffic is split across regions. If region A fails, DNS TTL=30s with health checks routes traffic to regions B and C within 60 seconds.
- **Cassandra**: Multi-DC replication with `NetworkTopologyStrategy`. Each DC has RF=3. Coordinator retries on different replica automatically.
- **MySQL**: Orchestrator monitors replication lag and auto-promotes read replica to primary on primary failure. Failover time: 30–60 seconds.
- **Redis**: Redis Cluster (16384 slots, N masters + N replicas). Slot coverage maintained even if half the masters fail (Cluster majority requirement: > 50% of masters must be up).

### Retries & Idempotency

- **Tweet POST**: Client sends `X-Idempotency-Key: {client_generated_uuid}` header. Tweet Service stores (idempotency_key, response) in Redis with 24h TTL. On retry, returns cached response without re-inserting.
- **Fanout Worker**: Kafka consumer commits offset only after all Redis pipelines succeed. On crash, replays from last committed offset. Redis ZADD is idempotent (adding the same tweet_id with same score is a no-op).
- **Like/Unlike**: Like table uses (user_id, tweet_id) primary key; duplicate inserts are ignored (INSERT IGNORE or ON CONFLICT DO NOTHING).

### Circuit Breaker

- Implemented using Hystrix-pattern (or Resilience4j in Java / py-circuitbreaker in Python).
- Timeline Service has circuit breakers around: Cassandra tweet hydration (fallback: return partial tweet from cache), Search Service (fallback: empty results + retry button), Trend Service (fallback: cached trends from 5 minutes ago).
- Thresholds: Open circuit after 50% error rate over 10-second window with minimum 20 requests. Half-open probe every 30 seconds.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Why It Matters |
|--------|------|-----------------|----------------|
| Timeline read p99 latency | Histogram | > 200ms | Core user experience SLA |
| Tweet write success rate | Counter | < 99.9% | Availability SLA |
| Fanout lag (Kafka consumer lag) | Gauge | > 100K messages | Indicates fanout workers falling behind |
| Redis timeline cache hit rate | Ratio | < 90% | Indicates cold-start or eviction problem |
| Cassandra read p99 | Histogram | > 50ms | Storage layer health |
| Trending topics update lag | Gauge | > 120 seconds | Trending freshness SLA |
| Like write throughput | Counter | Spike > 5x baseline | Viral event detection |
| Fanout queue depth by topic | Gauge | > 500K | Capacity planning signal |
| Error rate by service | Counter | > 0.1% 5xx | Service health |
| Kafka consumer group lag | Gauge | > 50K/partition | Fan-out worker scaling needed |

### Distributed Tracing

- Every request tagged with `trace_id` (128-bit UUID) propagated via HTTP headers (`X-Trace-ID`).
- Spans created at: API Gateway, Tweet Service, Kafka publish, Fanout Worker, Redis write, Timeline Service read, Cassandra query.
- Traces stored in Jaeger or AWS X-Ray with 7-day retention.
- P99 latency breakdown per span identifies bottlenecks. For example: if timeline p99 is 300ms, traces reveal whether time is in Redis fetch, celebrity tweet pull from Cassandra, or object hydration.

### Logging

- Structured JSON logs from all services: `{timestamp, trace_id, service, level, user_id, tweet_id, message, duration_ms}`.
- Log levels: ERROR for 5xx and data consistency violations; WARN for retried operations; INFO for normal operations (sampled at 1%).
- Centralized in Elasticsearch/Kibana (ELK) or Splunk. Retention: ERROR 30 days, INFO 7 days.
- Key log patterns to alert on: `"cassandra timeout"`, `"fanout_skip_celebrity"` count anomalies, `"rate_limit_exceeded"` spike (potential abuse).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|----------|---------------|-----------------|--------|
| Timeline generation | Hybrid fan-out (push+pull) | Pure push or pure pull | Pure push fails for celebrities (10M Redis writes/tweet); pure pull fails for users following thousands |
| Tweet ID generation | Snowflake (64-bit, time-ordered) | UUID v4 | UUID is random (breaks B-tree locality), 128-bit (larger storage), non-time-ordered |
| Tweet primary store | Cassandra | MySQL | Cassandra LSM-tree handles write-heavy append workload without write amplification; scales horizontally |
| Timeline cache | Redis Sorted Set | Memcached | Sorted Set natively supports time-ordered tweet_id list with O(log N) insertion; Memcached is key-value only |
| Like counters | Redis INCR + periodic flush | Direct Cassandra COUNTER | Cassandra counter writes still create tombstones at scale; Redis INCR is atomic and 10x faster |
| Trending algorithm | Flink streaming + Count-Min Sketch | Batch Spark hourly | Hourly batching gives 60-minute stale trending; streaming gives <60 second freshness |
| Celebrity threshold | 1M followers | 500K or 5M | 1M is a practical cutoff; accounts with 500K–1M followers are rare enough that fan-out cost is manageable |
| Search | Elasticsearch | Cassandra full-text | Cassandra has no efficient full-text index; ES inverted index purpose-built for this |
| Consistency on reads | Eventual (Redis stale ok) | Strong consistency | Strong consistency requires QUORUM reads on every timeline refresh — 3x read latency; not worth it for social feed |
| Media storage | S3 + CDN | Self-hosted storage | S3 is infinitely scalable, 99.999999999% durability; CDN eliminates origin bandwidth cost |

---

## 11. Follow-up Interview Questions

**Q1: How would you implement "Twitter Moments" — curated stories of trending events?**
A: Moments are editorially curated collections of tweets around an event. Data model: `moments (moment_id, title, description, tweet_ids[], created_by, status)`. Tweet selection can be manual (editor picks) or algorithmic (cluster tweets by trend + engagement). Served via CDN since they're public, high-traffic, and not personalized. The same trending detection pipeline surfaces candidate moments; a lightweight ML model ranks candidates by engagement velocity.

**Q2: How would you add a "quote tweet" feature at scale?**
A: Quote tweets are tweets with a reference to another tweet. The schema already has `retweet_of_id`. For quote tweets, we add a `quoted_tweet_id` field. When displaying, the client fetches the original tweet in a nested call (or we embed it in the response). The fan-out for quote tweets works identically to original tweets. The key challenge is preventing infinite nesting — we limit display to 2 levels deep (quote of a quote shows "original tweet unavailable if further nested").

**Q3: Design the "who to follow" recommendation system.**
A: Input signals: mutual follows (users your follows also follow), shared interests (tweet content similarity), geographic proximity, phone contact list upload. Algorithm: compute 2-hop graph neighbors (users followed by people you follow) as candidates. Rank by: (a) mutual follow count, (b) follower similarity cosine score from user embedding, (c) engagement probability from historical interaction data. Pre-compute top-20 recommendations per user nightly using a Spark job on the follow graph. Serve from Redis with daily refresh.

**Q4: How would you implement Twitter's "lists" feature?**
A: Lists are curated groups of users. Data model: `lists (list_id, owner_id, name, is_public)`, `list_memberships (list_id, user_id)`. Timeline for a list = fan-out on read for all members (since lists are typically small, 10–200 users). Cache list timelines in Redis similar to home timeline but keyed by `list:{list_id}:{user_id}`. List membership changes invalidate the cache.

**Q5: How do you ensure tweet delivery ordering in a distributed fanout system?**
A: Strict ordering is not guaranteed in the fanout pipeline — Kafka partitioning means different followers may receive tweets in slightly different orders during high load. This is acceptable for social media where eventual consistency is the contract. However, for the same user's multiple tweets, we ensure ordering via Snowflake IDs (time-ordered). The client sorts by tweet_id descending, so even if fan-out arrives out of order, the display is always chronologically correct.

**Q6: How do you handle tweet deletion (GDPR compliance, user deletion)?**
A: Soft delete first: set `is_deleted=true` in Cassandra, publish `tweet.deleted` event to Kafka. Fanout workers and timeline services check this flag on hydration and omit deleted tweets. Hard delete (GDPR right to erasure): 30-day background job runs Cassandra DELETE (creates tombstone, compacted away after gc_grace_seconds). Media deleted from S3. CDN cache purged via CDN invalidation API. Search index document deleted via ES delete API. Counters in Redis DEL. User account deletion cascades to all their tweets via the same pipeline.

**Q7: What changes if we need to support 10 billion DAU (hypothetical)?**
A: At 50x current scale: (1) Cassandra cluster grows from 30 to 1500+ nodes — manageable via consistent hashing but ops-heavy; consider tiering to Apache Iceberg on S3 for cold tweets (>90 days). (2) Redis timeline cache RAM at 200GB today becomes 10TB — use Redis Cluster with 100+ shards or consider replacing with a custom in-memory service. (3) Kafka cluster scales by adding brokers; topic partitions increased from 100 to 5000. (4) Fanout workers scale from 100 to 5000 instances; consider moving to a dedicated fan-out platform (Meta-style Iris). (5) Celebrity threshold drops to 100K followers to reduce fan-out load.

**Q8: How would you implement "muted words" that filter tweets on a user's timeline?**
A: Muted words stored per user in MySQL (`muted_words: user_id, word, created_at`). Filtering happens at Timeline Service read time, not at fan-out time (filtering during fan-out would require knowing every user's muted words for every tweet — O(followers × muted_words)). Timeline Service loads user's muted words (Redis-cached, invalidated on change), applies a fast substring filter against tweet body during hydration. If a tweet matches a muted word, it's excluded from the response. This means the "count=20" may return fewer items — client requests more if under threshold.

**Q9: Explain the trade-off between using Kafka vs. direct RPC for the tweet fanout trigger.**
A: Direct RPC (e.g., Tweet Service calling Fanout Service synchronously): lower complexity, immediate delivery. But: (a) Fanout Service failure means tweet write fails (tight coupling), (b) response time increases by fanout latency, (c) no replay if fanout fails. Kafka provides: (a) decoupling — Tweet Service succeeds immediately, (b) durability — message survives Fanout Worker crash, (c) replay on failure, (d) natural backpressure via consumer lag. Trade-off: Kafka adds operational complexity (broker management, offset management) and 5–10ms additional latency. For a system at Twitter's scale, Kafka's benefits in reliability and throughput far outweigh the complexity cost.

**Q10: How do you handle time-zone-aware "top tweets of the day" for different regions?**
A: Maintain per-region sorted sets in Redis: `top_tweets:{region}:{YYYY-MM-DD_local}`. The "local date" key includes the region-specific date boundary. Flink aggregates tweet engagement (likes + retweets × weights) per region per day. At midnight UTC-12 (latest timezone), the previous day's key is snapshotted and frozen. Users request `GET /v1/top?region=US&period=today` which maps to their local date key. Retention: 7 days of daily top tweets per region.

**Q11: What are the consistency guarantees when a tweet is immediately unliked after being liked?**
A: Like and unlike are handled by (insert, delete) on the Likes table (strong consistency via MySQL PK). The Redis counter is eventually consistent — there may be a brief window where the Redis INCR has been applied but the Cassandra flush hasn't. The net result after eventual sync is correct. If the user unlikes before the Redis counter flushes, the flush job reads the current Likes table count (source of truth) and overwrites the Redis counter, resolving any discrepancy.

**Q12: How would you design push notifications for tweet interactions?**
A: Notification events published to Kafka topic `notification.events`. Notification Service consumes events, looks up user's notification preferences (MySQL), and decides whether to send push (APNs/FCM), email, or in-app notification. APNs/FCM delivery handled by a dedicated Push Gateway service (wraps vendor APIs with retry/backoff). In-app notifications stored in Cassandra `notifications` table, read on app open. Deduplication: (actor_id, object_id, type) within 30 minutes grouped into one notification ("Alice and 3 others liked your tweet").

**Q13: How do you prevent a single rogue tweet from taking down the fanout system?**
A: Defense in depth: (1) Max follower fan-out capped at 1M — accounts above threshold use celebrity fan-out. (2) Kafka message size limit (1MB) prevents malformed large events. (3) Fanout Worker has per-message timeout (5 seconds); messages that cause timeouts are dead-lettered to a `fanout.dlq` topic for inspection, not retried infinitely. (4) Redis pipeline batch size capped at 1000 — avoids blocking Redis for too long. (5) Circuit breaker on Redis: if > 50% of pipeline operations fail, circuit opens and tweet is flagged for async retry.

**Q14: Describe how you'd add "algorithmic timeline" ranking (like Twitter's current default).**
A: Currently timelines are chronological. For ranking: after fetching ~200 candidate tweet_ids (from Redis + celebrity pull), pass the list to a Ranking Service. Ranking Service: (1) fetches tweet features (engagement signals: like velocity, retweet rate, reply count, author-follower relationship strength, media presence) from a Feature Store; (2) runs a lightweight ML model (gradient boosted tree or two-tower neural net pre-trained offline) to score each tweet; (3) returns top-N reranked tweets. Feature Store is updated by a stream of engagement events (Kafka → Flink → Redis). Model inference must complete in < 50ms — achieved via ONNX runtime or TensorRT-lite.

**Q15: How do you test fanout correctness at scale without running in production?**
A: Approach: shadow testing + replay. (1) In staging, replay a production Kafka snapshot (with PII scrubbed) through the fanout system at 10x speed. (2) Compare output timeline snapshots against a "golden" reference computed by a simple O(N²) brute-force reference implementation. (3) Use chaos engineering (kill random fanout workers mid-batch) and verify Kafka replay produces identical final results. (4) Property-based tests: for any set of follows and tweets, assert that after fanout, every non-celebrity tweet appears in every follower's timeline within defined SLA bounds.

---

## 12. References & Further Reading

- Krikorian, R. (2012). "New Tweets per second record, and how!" — Twitter Engineering Blog. Describes Snowflake ID generation and early timeline architecture.
- Twitter Engineering Blog: "The Infrastructure Behind Twitter: Scale" (2017) — covers the move to Mesos and Manhattan distributed storage.
- Buluç, A. et al. "Parallel Breadth-First Search on Distributed-Memory Machines" — relevant for follow-graph traversal algorithms.
- Apache Kafka documentation: kafka.apache.org/documentation — producer/consumer semantics, exactly-once delivery guarantees.
- Apache Flink documentation: flink.apache.org — stateful stream processing, sliding window aggregations, Count-Min Sketch state backends.
- Redis documentation on Sorted Sets: redis.io/docs/data-types/sorted-sets — ZADD, ZREVRANGEBYSCORE complexity guarantees.
- Cormode, G. & Muthukrishnan, S. (2005). "An Improved Data Stream Summary: The Count-Min Sketch and its Applications." — original Count-Min Sketch paper. Journal of Algorithms, 55(1).
- Cassandra documentation: cassandra.apache.org/doc — LSM-tree storage, consistency levels, vnodes, compaction strategies.
- Lamport, L. (1978). "Time, Clocks, and the Ordering of Events in a Distributed System." — foundational paper on distributed time ordering, underpins Snowflake design rationale.
- Iyer, A. P. et al. "Bridging the GAP: Towards Approximate Graph Analytics" — relevant for approximate graph computations on large social networks.
- "Designing Data-Intensive Applications" by Martin Kleppmann (O'Reilly, 2017) — Chapters 5 (Replication), 6 (Partitioning), 11 (Stream Processing) are directly applicable.
- Facebook Engineering: "TAO: Facebook's Distributed Data Store for the Social Graph" (USENIX ATC 2013) — comparable social graph storage problem.
