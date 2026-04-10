# System Design: Instagram

---

## 1. Requirement Clarifications

### Functional Requirements
- Users can upload photos and short videos with captions and hashtags
- Users can follow/unfollow other users
- Users see a home feed of posts from accounts they follow
- Users can like, comment on, and save posts
- Users can post Stories (ephemeral content that disappears after 24 hours)
- Users can search for accounts, hashtags, and locations
- Explore page surfaces content discovery (posts from non-followed accounts)
- Users receive notifications for likes, comments, follows, and mentions
- User profiles display post grid, follower/following counts
- Direct messaging (DMs) between users

### Non-Functional Requirements
- Availability: 99.99% uptime — social platform; availability preferred over strict consistency
- Home feed read latency: p99 < 300ms — users expect near-instant feed load
- Photo upload latency: p99 < 5 seconds end-to-end (upload + processing + availability)
- Stories are ephemeral: automatically deleted after 24 hours (hard requirement)
- Media is durable: no data loss after successful upload acknowledgment
- Feed is eventually consistent: posts may appear in followers' feeds within a few seconds of posting
- System must handle global content distribution across 50+ countries with local latency < 100ms for media delivery
- Scale to support 2 billion MAU and 100 million photos/videos uploaded per day

### Out of Scope
- Instagram Shopping and in-app checkout
- Instagram Live (real-time video streaming)
- Reels full recommendation engine (noted but not deep-dived)
- Advertising and sponsored content serving
- Content moderation pipeline

---

## 2. Users & Scale

### User Types
- **Casual users**: Browse feed, view Stories, like/comment — majority of traffic
- **Creators**: Upload photos/videos regularly, high engagement — drive content
- **Influencers/Celebrities**: Millions of followers, create hotspot problems for fanout
- **Businesses**: Brand accounts, same technical profile as creators

### Traffic Estimates

| Metric | Value | Reasoning |
|--------|-------|-----------|
| MAU | 2 billion | Assumption consistent with publicly reported figures |
| DAU | 500 million | ~25% of MAU active daily; Instagram's high engagement ratio |
| Posts (photos/videos)/day | 100 million | ~0.2 posts/day per DAU; heavily Pareto-distributed (20% of users create 80% of content) |
| Stories/day | 500 million | ~1 Story/DAU/day average; Stories are primary daily engagement driver |
| Feed reads/day | 50 billion | 500M DAU × 100 feed loads/day average |
| Feed read QPS (avg) | ~578,000 | 50B / 86,400 |
| Feed read QPS (peak) | ~2,300,000 | 4× peak multiplier for prime-time usage |
| Post write QPS (avg) | ~1,160 | 100M / 86,400 |
| Story write QPS (avg) | ~5,800 | 500M / 86,400 |
| Peak multiplier | 4× | Evening prime time; major events |

### Latency Requirements
- **Feed read p99: 300ms** — feed is the primary engagement surface; latency above 300ms measurably reduces scroll depth and session duration
- **Photo upload acknowledgment p99: 2s** — user sees immediate optimistic UI; actual processing (transcoding, CDN propagation) continues asynchronously
- **Story view p99: 150ms** — Stories are full-screen immersive; any buffering destroys the UX; CDN delivery must serve < 100ms from edge
- **Explore page p99: 500ms** — discovery is lower urgency than home feed; slightly higher latency acceptable

### Storage Estimates

| Data Type | Size/record | Records/day | Retention | Total |
|-----------|-------------|-------------|-----------|-------|
| Photo (compressed original) | 3 MB avg | 80M/day | Forever | 240 GB/day → ~85 TB/year |
| Photo thumbnails (3 sizes) | 200 KB × 3 = 600 KB | 80M/day | Forever | 48 GB/day → ~17 TB/year |
| Video (avg 30s, compressed) | 50 MB | 20M/day | Forever | 1 TB/day → ~365 TB/year |
| Story photos | 2 MB | 400M/day | 24 hours | 800 GB/day (rolling, purged after 24h, ~800 GB steady state) |
| Story videos | 20 MB | 100M/day | 24 hours | 2 TB/day (rolling) |
| Post metadata (caption, tags, location) | 1 KB | 100M/day | Forever | 100 GB/day → ~35 TB/year |
| User profiles | 2 KB | 1M new users/day | Forever | 2 GB/day |
| Follow graph edges | 16 bytes | 200M new edges/day | Forever | 3.2 GB/day |
| Comments | 500 bytes | 1B/day | Forever | 500 GB/day |
| Likes | 16 bytes | 4B/day | Forever (for feed ranking) | 64 GB/day |

**Total storage growth: ~1.5 TB/day metadata + ~1.5 TB/day media = ~3 TB/day excluding large videos.**
**With video (Reels): ~370 TB/year in video alone — object storage with tiering is essential.**

### Bandwidth Estimates

| Direction | Calculation | Result |
|-----------|-------------|--------|
| Inbound photo uploads | 80M photos × 3 MB / 86400 | ~2.8 GB/s |
| Inbound video uploads | 20M videos × 50 MB / 86400 | ~11.6 GB/s |
| Outbound feed (thumbnails) | 578K QPS × 10 posts × 200 KB | ~1.1 TB/s total; 90% CDN cached → ~110 GB/s origin |
| CDN to user (thumbnails) | 578K QPS × 10 × 200 KB | ~1.1 TB/s from CDN edges globally |
| Story delivery | 5,800 QPS × 2 MB | ~11.6 GB/s from CDN |

---

## 3. High-Level Architecture

```
                        ┌──────────────────────────────────────────────┐
                        │              CLIENTS                          │
                        │   iOS App / Android App / Web Browser         │
                        └───────────────────┬──────────────────────────┘
                                            │ HTTPS / HTTP2
                                            ▼
                        ┌──────────────────────────────────────────────┐
                        │         Global CDN (Cloudflare / Fastly)      │
                        │  Media delivery (photos, videos, Stories)     │
                        │  Edge caches in 200+ PoPs worldwide           │
                        └───────────────────┬──────────────────────────┘
                                            │ Cache miss / API requests
                                            ▼
                        ┌──────────────────────────────────────────────┐
                        │         API Gateway / Load Balancer           │
                        │  Auth, rate limiting, request routing         │
                        │  A/B experiment assignment, feature flags     │
                        └────┬───────────┬────────────┬────────────────┘
                             │           │            │
          ┌──────────────────▼──┐  ┌─────▼──────┐  ┌─▼──────────────────┐
          │   Upload Service    │  │ Feed Service│  │  User/Follow Svc   │
          │ - Receive media     │  │ - Home feed │  │  - Profile CRUD    │
          │ - Validate          │  │ - Fan-out   │  │  - Follow graph    │
          │ - Publish to queue  │  │ - Read cache│  │  - Suggestions     │
          └──────────┬──────────┘  └──────┬──────┘  └──────────┬─────────┘
                     │                    │                     │
          ┌──────────▼──────────────────────────────────────────▼──────────┐
          │                    Message Queue (Kafka)                        │
          │  Topics: media.uploaded, post.created, story.created,          │
          │          follow.event, post.deleted                             │
          └──────────┬──────────────────────────────────────────────────────┘
                     │
        ┌────────────┼────────────────────────┐
        │            │                        │
┌───────▼──────┐  ┌──▼──────────────┐  ┌─────▼──────────────────┐
│ Media        │  │ Fan-out Worker  │  │  Story Expiry Worker   │
│ Processor    │  │ - Push post_id  │  │  - Scan TTL index      │
│ - Transcode  │  │   to follower   │  │  - Delete expired      │
│ - Resize     │  │   feed caches   │  │    Stories after 24h   │
│ - CDN upload │  │ - Hybrid fanout │  └────────────────────────┘
└──────────────┘  └─────────────────┘
                                               
 ┌────────────────────────────────────────────────────────────────────┐
 │                        DATA STORES                                  │
 │                                                                      │
 │  ┌──────────────────────┐  ┌─────────────────┐  ┌───────────────┐  │
 │  │ Post Metadata Store  │  │ Feed Cache      │  │ Follow Graph  │  │
 │  │ (Cassandra)          │  │ (Redis Cluster) │  │ (MySQL)       │  │
 │  │ Sharded by post_id   │  │ post_id[] per   │  │ Adjacency     │  │
 │  └──────────────────────┘  │ user            │  │ list, sharded │  │
 │                            └─────────────────┘  └───────────────┘  │
 │  ┌──────────────────────┐  ┌─────────────────┐                     │
 │  │ Media Object Store   │  │  Search Index   │                     │
 │  │ (S3 + CloudFront)    │  │  (Elasticsearch)│                     │
 │  │ Photos, videos,      │  │  hashtags,      │                     │
 │  │ Stories              │  │  captions,      │                     │
 │  │                      │  │  usernames      │                     │
 │  └──────────────────────┘  └─────────────────┘                     │
 │  ┌──────────────────────────────────────────────────────────────┐  │
 │  │ Story Store (Cassandra with TTL on rows)                     │  │
 │  │ Row TTL = 86400 seconds; Cassandra auto-deletes on expiry    │  │
 │  └──────────────────────────────────────────────────────────────┘  │
 └────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **CDN**: Serves all media (photos, thumbnails, videos, Stories) from 200+ edge locations globally. Origin is S3. Cache hit rate target: 95%+ for photos, 80% for Stories (ephemeral, less cacheable).
- **API Gateway**: Routes requests, validates OAuth 2.0 tokens, enforces per-user rate limits, assigns A/B experiments via feature flag service (LaunchDarkly or internal).
- **Upload Service**: Accepts multipart uploads, validates MIME type and size limits, stores raw media to S3 staging bucket, publishes `media.uploaded` event to Kafka.
- **Media Processor**: Consumes `media.uploaded`, runs async transcoding/resizing pipeline (multiple thumbnail sizes: 150px, 300px, 640px; video to H.264/H.265; HDR normalization), writes processed files to production S3, updates post metadata with final media keys, publishes `post.created` event.
- **Feed Service**: Reads precomputed feed from Redis, handles cache misses, merges celebrity posts at read time (hybrid fan-out).
- **Fan-out Worker**: Pushes new post_id to follower feed caches in Redis. Differentiates celebrity vs. normal fan-out.
- **Story Expiry Worker**: Runs every minute, queries Cassandra for Stories past their 24-hour TTL, deletes S3 objects, CDN purges the keys, removes from Cassandra.
- **Kafka**: Durable event bus decoupling upload, processing, and fanout pipelines.

**End-to-End Flow — Uploading a Photo:**
1. Client initiates upload: calls `POST /v1/media/upload` to Upload Service.
2. Upload Service validates content type, size (< 30 MB for photos), and user rate limit.
3. Media streamed directly to S3 staging bucket via pre-signed URL (reduces Upload Service bandwidth load).
4. Upload Service publishes `media.uploaded {s3_key, user_id, media_type, client_metadata}` to Kafka.
5. Client receives `media_id` immediately (optimistic response). Post UI shows "processing..." state.
6. Media Processor (Kafka consumer) transcodes/resizes, writes output to production S3, generates CDN-accessible URLs.
7. Media Processor inserts post metadata to Cassandra, publishes `post.created {post_id, user_id, media_keys}`.
8. Fan-out Worker consumes `post.created`, pushes post_id to followers' Redis feed caches.
9. Client polling or WebSocket push notifies user that post is live.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Post Metadata (Cassandra — append-heavy, no updates to text)
-- ============================================================
CREATE TABLE posts (
    post_id       BIGINT,           -- Snowflake ID
    user_id       BIGINT,
    caption       TEXT,             -- Max 2200 chars
    media_keys    LIST<TEXT>,       -- S3 object keys (1–10 items)
    media_types   LIST<TEXT>,       -- 'image/jpeg', 'video/mp4', etc.
    hashtags      LIST<TEXT>,       -- Extracted at write time
    location_id   BIGINT,           -- FK to locations table, nullable
    like_count    BIGINT,           -- Denormalized, updated via Kafka
    comment_count BIGINT,
    is_deleted    BOOLEAN DEFAULT FALSE,
    created_at    TIMESTAMP,
    PRIMARY KEY (post_id)
);

-- User's own posts (for profile page grid)
CREATE TABLE posts_by_user (
    user_id    BIGINT,
    post_id    BIGINT,
    created_at TIMESTAMP,
    PRIMARY KEY (user_id, post_id)
) WITH CLUSTERING ORDER BY (post_id DESC);

-- ============================================================
-- Stories (Cassandra with row-level TTL)
-- ============================================================
CREATE TABLE stories (
    user_id    BIGINT,
    story_id   BIGINT,       -- Snowflake ID
    media_key  TEXT,         -- S3 key for story media
    media_type TEXT,         -- 'image' or 'video'
    duration_s INT,          -- Video duration in seconds
    viewers    SET<BIGINT>,  -- Who has viewed this story (bounded; can be approximate)
    caption    TEXT,
    created_at TIMESTAMP,
    PRIMARY KEY (user_id, story_id)
) WITH CLUSTERING ORDER BY (story_id DESC)
  AND default_time_to_live = 86400;  -- Cassandra auto-deletes after 24 hours

-- Story view tracking (separate table for large accounts)
CREATE TABLE story_views (
    story_id   BIGINT,
    viewer_id  BIGINT,
    viewed_at  TIMESTAMP,
    PRIMARY KEY (story_id, viewer_id)
) WITH default_time_to_live = 86400;

-- ============================================================
-- Users (PostgreSQL — relational, ACID for auth/billing)
-- ============================================================
CREATE TABLE users (
    user_id        BIGINT PRIMARY KEY,          -- Snowflake
    username       VARCHAR(30) UNIQUE NOT NULL,
    display_name   VARCHAR(50),
    bio            TEXT,                         -- Max 150 chars
    avatar_key     VARCHAR(255),                 -- S3 key
    website_url    VARCHAR(500),
    is_private     BOOLEAN DEFAULT FALSE,
    is_verified    BOOLEAN DEFAULT FALSE,
    is_celebrity   BOOLEAN DEFAULT FALSE,        -- Fanout routing flag
    follower_count INT DEFAULT 0,
    following_count INT DEFAULT 0,
    post_count     INT DEFAULT 0,
    created_at     TIMESTAMPTZ DEFAULT NOW(),
    INDEX idx_username (username)
);

-- ============================================================
-- Follow Graph (MySQL, sharded by follower_id)
-- ============================================================
CREATE TABLE follows (
    follower_id  BIGINT NOT NULL,
    followee_id  BIGINT NOT NULL,
    status       ENUM('pending', 'accepted') DEFAULT 'accepted',  -- For private accounts
    created_at   DATETIME DEFAULT NOW(),
    PRIMARY KEY (follower_id, followee_id),
    INDEX idx_followee (followee_id, follower_id)
);

-- ============================================================
-- Comments (Cassandra — high write rate, append-only)
-- ============================================================
CREATE TABLE comments (
    post_id     BIGINT,
    comment_id  BIGINT,      -- Snowflake
    user_id     BIGINT,
    body        TEXT,        -- Max 2200 chars
    like_count  BIGINT DEFAULT 0,
    is_deleted  BOOLEAN DEFAULT FALSE,
    created_at  TIMESTAMP,
    PRIMARY KEY (post_id, comment_id)
) WITH CLUSTERING ORDER BY (comment_id DESC);

-- ============================================================
-- Likes (Cassandra — extremely high write rate)
-- ============================================================
CREATE TABLE likes (
    post_id    BIGINT,
    user_id    BIGINT,
    created_at TIMESTAMP,
    PRIMARY KEY (post_id, user_id)
);

-- ============================================================
-- Feed Cache (Redis — logical, not SQL)
-- Key:   "feed:{user_id}"
-- Type:  Sorted Set (score = post_id Snowflake for time order)
-- Max:   500 entries per user; ZREMRANGEBYRANK on overflow
-- TTL:   3 days of inactivity
-- ============================================================

-- ============================================================
-- Explore/Discovery (Elasticsearch)
-- Index: "posts"
-- Fields: post_id, user_id, hashtags[], caption (analyzed),
--         location_id, like_count, created_at, embedding_vector
-- Used for: hashtag search, explore grid, related posts
-- ============================================================
```

### Database Choice

| Database | Use Case | Pros | Cons |
|----------|----------|------|------|
| Cassandra | Posts, comments, likes, stories | Linear horizontal scale; TTL natively supported (critical for Stories); write-optimized LSM-tree; wide-column for time-series | No ACID; limited secondary indexes; eventual consistency |
| PostgreSQL | User accounts | ACID for user registration (uniqueness on username); familiar ops tooling; JSONB for flexible profile fields | Vertical scaling; complex sharding setup at scale |
| MySQL | Follow graph | Strong consistency for follow relationships; PK uniqueness enforced at DB level | Join performance degrades; needs Vitess for sharding |
| Redis | Feed cache, story viewer counts | O(1) Sorted Set operations; TTL for story expiry signals; pub/sub for real-time notifications | Memory-bound; no complex queries |
| Elasticsearch | Search, explore discovery | Full-text search on captions/hashtags; vector similarity for explore recommendations | Not source of truth; operational complexity; eventual consistency with primary |
| S3 + CDN | Media blobs | Unlimited storage; 11 nines durability; CDN integration native | Not queryable; eventual consistency on metadata |

**Decision:** Cassandra chosen for posts because: (1) TTL-based automatic deletion is a first-class feature, critical for 24-hour Stories — no external cleanup job needed for the primary expiry mechanism; (2) wide-column allows efficient range scans on `(user_id, post_id)` for profile grids; (3) 100M posts/day is a write-heavy workload that Cassandra's LSM-tree handles without B-tree write amplification; (4) geo-distributed replication with multi-DC replication strategy supports global deployment.

---

## 5. API Design

```
POST /v1/media/upload
  Description: Initiate media upload (returns pre-signed S3 URL)
  Auth: Bearer token (required)
  Rate limit: 100 uploads/hour per user
  Request:  { media_type: "image/jpeg"|"video/mp4", file_size_bytes: int }
  Response 200: {
    media_id: string,
    upload_url: string,    -- Pre-signed S3 URL, expires in 15 min
    expires_at: ISO8601
  }

POST /v1/posts
  Description: Create a post with already-uploaded media
  Auth: Bearer token (required)
  Rate limit: 50 posts/day per user
  Request:  {
    media_ids: string[] (1–10),
    caption: string (max 2200 chars, optional),
    hashtags: string[] (auto-extracted but can be overridden),
    location_id: string | null
  }
  Response 201: {
    post_id: string,
    user_id: string,
    media_urls: [{ thumbnail_url, full_url, type, width, height }],
    caption: string,
    created_at: ISO8601
  }

GET /v1/feed/home
  Description: Fetch home feed
  Auth: Bearer token (required)
  Query params:
    cursor: string (opaque pagination cursor)
    count: int (default 12, max 50)
    ranked: bool (default true — use algorithmic ranking if true)
  Response 200: {
    posts: Post[],
    next_cursor: string | null
  }

GET /v1/posts/{post_id}
  Description: Get a single post
  Auth: Optional (public posts only if not authenticated)
  Response 200: {
    post_id, author: { user_id, username, avatar_url, is_verified },
    media: [{ url, thumbnail_url, type, width, height }],
    caption, hashtags, like_count, comment_count, is_liked_by_me: bool,
    created_at
  }

POST /v1/posts/{post_id}/like
  Description: Like a post (idempotent)
  Auth: Bearer token (required)
  Response 200: { liked: true, like_count: int }

DELETE /v1/posts/{post_id}/like
  Description: Unlike a post (idempotent)
  Auth: Bearer token (required)
  Response 200: { liked: false, like_count: int }

GET /v1/posts/{post_id}/comments
  Description: Get paginated comments for a post
  Auth: Optional
  Query params: cursor, count (default 20)
  Response 200: {
    comments: [{ comment_id, user: {...}, body, like_count, created_at }],
    next_cursor: string | null
  }

POST /v1/posts/{post_id}/comments
  Description: Add a comment
  Auth: Bearer token (required)
  Rate limit: 60 comments/hour
  Request:  { body: string (max 2200 chars) }
  Response 201: { comment_id, body, created_at }

POST /v1/stories
  Description: Post a Story (ephemeral, expires in 24h)
  Auth: Bearer token (required)
  Rate limit: 100 stories/day
  Request:  {
    media_id: string,   -- From /v1/media/upload
    caption: string | null,
    duration_s: int (3–15 for images displayed, actual duration for video)
  }
  Response 201: { story_id, media_url, expires_at: ISO8601 }

GET /v1/stories/feed
  Description: Get Stories from followed accounts (story tray)
  Auth: Bearer token (required)
  Response 200: {
    story_rings: [{
      user: { user_id, username, avatar_url },
      stories: [{ story_id, media_url, media_type, duration_s, is_seen: bool }],
      has_unseen: bool
    }]
  }

GET /v1/explore
  Description: Personalized discovery feed
  Auth: Bearer token (required)
  Query params: cursor, count (default 24)
  Response 200: { posts: Post[], next_cursor: string | null }

GET /v1/search
  Description: Search posts, users, hashtags
  Auth: Optional
  Query params:
    q: string (required)
    type: "top"|"accounts"|"hashtags"|"places"
    cursor, count
  Response 200: {
    accounts: User[],
    hashtags: [{ name, post_count }],
    posts: Post[],
    next_cursor: string | null
  }

POST /v1/follows
  Description: Follow a user
  Auth: Bearer token (required)
  Request:  { followee_id: string }
  Response 201: { status: "accepted"|"pending" }
  Note: "pending" if target account is private; creates follow request

DELETE /v1/follows/{followee_id}
  Description: Unfollow
  Auth: Bearer token (required)
  Response 200: { following: false }
```

---

## 6. Deep Dive: Core Components

### Component: Media Storage & Delivery Pipeline

**Problem it solves:**
Instagram handles 100M photo/video uploads per day. Each upload must be: validated, processed into multiple formats and resolutions (for different device types, bandwidth conditions, and display contexts), stored durably, and delivered globally with low latency. Raw uploads are too large and unoptimized for direct serving; CDN delivery requires standardized formats.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Synchronous processing | Process media inline during upload request | Simple code path | Upload request blocks for 10–30s; poor UX; no retry on failure |
| Async processing via queue | Upload stored raw; processing job triggered via Kafka | Decoupled; scalable; retry-able | Media not immediately available; need status polling |
| Client-side transcoding | Client resizes/compresses before upload | Reduces server load and upload size | Client quality inconsistent; can't control output format; bypassed by API clients |
| Direct S3 upload + Lambda processing | Pre-signed URL upload, S3 event triggers Lambda | Serverless scaling; no pipeline servers | Lambda cold starts; 15-minute timeout limits for large videos; hard to manage state |

**Selected Approach: Async processing via Kafka + dedicated Media Processor fleet**

Pre-signed URL direct upload to S3 staging bucket eliminates Upload Service as a bandwidth bottleneck. Kafka event triggers Media Processor workers (dedicated EC2/GKE fleet with GPU nodes for video transcoding). Multiple output formats generated in parallel.

**Implementation Detail:**

```python
# Upload Service: returns pre-signed URL
def initiate_upload(user_id, media_type, file_size_bytes):
    validate_file_size(media_type, file_size_bytes)  # images: 30MB, video: 1GB
    media_id = generate_snowflake_id()
    staging_key = f"staging/{user_id}/{media_id}/{media_type.split('/')[1]}"

    presigned_url = s3.generate_presigned_url(
        'put_object',
        Params={'Bucket': STAGING_BUCKET, 'Key': staging_key,
                'ContentType': media_type, 'ContentLength': file_size_bytes},
        ExpiresIn=900  # 15 minutes
    )

    # Register pending upload in Redis with 15-minute TTL
    redis.setex(f"pending_upload:{media_id}", 900,
                json.dumps({'user_id': user_id, 'staging_key': staging_key,
                            'media_type': media_type}))
    return {'media_id': media_id, 'upload_url': presigned_url}

# S3 event -> Kafka (via Lambda bridge or S3 event notification)
# Alternatively: client calls /v1/media/upload/complete after S3 PUT succeeds

# Media Processor Worker
def process_media(event):
    media_id = event['media_id']
    staging_key = event['staging_key']
    media_type = event['media_type']

    raw_bytes = s3.get_object(Bucket=STAGING_BUCKET, Key=staging_key)

    if media_type.startswith('image/'):
        outputs = process_image(raw_bytes, media_id)
    else:
        outputs = process_video(raw_bytes, media_id)

    # Upload all processed versions to production bucket
    production_keys = {}
    for variant, data in outputs.items():
        prod_key = f"media/{media_id}/{variant}"
        s3.put_object(Bucket=PROD_BUCKET, Key=prod_key, Body=data,
                      ContentType=outputs[variant]['content_type'],
                      CacheControl='max-age=31536000, immutable')
        production_keys[variant] = prod_key

    # Update post metadata in Cassandra
    cassandra.update(f"UPDATE posts SET media_keys = {production_keys} "
                     f"WHERE post_id = {event['post_id']}")

    # Warm CDN by prefetching popular variants
    if event.get('warm_cdn', False):
        cdn_warm(production_keys['thumbnail_300'])

    # Signal post creation complete
    kafka.publish('post.created', {**event, 'media_keys': production_keys})

def process_image(raw_bytes, media_id):
    img = PIL.Image.open(raw_bytes)
    img = auto_orient(img)  # EXIF rotation correction
    img = strip_exif(img)   # Privacy: remove GPS, device info
    return {
        'thumbnail_150': resize_and_encode(img, 150, quality=85),
        'thumbnail_300': resize_and_encode(img, 300, quality=85),
        'standard_640': resize_and_encode(img, 640, quality=90),
        'original': encode_webp(img, quality=95),  # Store as WebP for size
    }
```

**Interviewer Q&As:**

Q: How do you handle the case where Media Processor crashes mid-transcoding?
A: Kafka offset is committed only after all S3 writes and Cassandra update succeed atomically (as a batch). If the processor crashes mid-job, the event is replayed from the last committed offset. S3 PUT is idempotent (same key = overwrite); Cassandra UPDATE is idempotent with the same data. The post remains in "processing" state in Cassandra until the final update. We set a 1-hour timeout on posts stuck in "processing" state — a separate cleanup job republishes the Kafka event.

Q: How do you serve media to users on low-bandwidth connections?
A: At upload time, we generate multiple resolutions (150px, 300px, 640px, original). At serve time, Feed Service includes resolution hints in the post response. The client sends `Downlink` and `ECT` (Effective Connection Type) headers. CDN edge logic (or client-side adaptive loading) selects the appropriate variant URL. For video, we use HLS/DASH adaptive bitrate streaming with multiple quality ladders generated at transcoding time.

Q: How do you handle media deduplication to save storage?
A: At upload time, compute perceptual hash (pHash) of image. Check Redis bloom filter for known hashes. If bloom filter says "maybe seen" (1% false positive rate), check exact hash in Cassandra `media_hashes` table. If exact match, reuse existing S3 key rather than storing duplicate. This deduplication is opt-in (same user reposting same content). Cross-user deduplication raises privacy concerns and is a policy decision, not just a technical one — typically not done except for known CSAM material.

Q: Why use a pre-signed S3 URL rather than routing uploads through your own servers?
A: Routing through app servers means: (a) Upload Service must buffer or stream multi-GB video files — requires large instances or specialized media servers. (b) Bandwidth cost doubles (client-to-server + server-to-S3). (c) Upload Service becomes a bottleneck. Pre-signed S3 URLs let the client upload directly to S3, which has effectively unlimited ingest bandwidth. Upload Service only handles metadata (< 1KB) and returns the URL — it can run on tiny instances. S3 server-side encryption and bucket policy restrictions ensure only valid uploads land in the staging bucket.

Q: How do you handle the CDN cache for updated profile pictures?
A: Profile pictures use S3 keys that include the user_id and a version hash: `avatars/{user_id}/{hash}.jpg`. When a user updates their avatar, a new file is written with a new hash — the old URL is never mutated (immutable caching). The new avatar URL is stored in the user record in PostgreSQL. Old CDN-cached versions naturally expire via max-age. If immediate invalidation is needed (e.g., abuse case), we call CDN purge API with the old URL. This "cache-busting by URL change" pattern avoids the latency and cost of explicit CDN invalidations.

---

### Component: Stories (Ephemeral Content)

**Problem it solves:**
Stories are full-screen, 24-hour-ephemeral media sequences posted by users. Key requirements: (1) guaranteed deletion after 24 hours, (2) viewer tracking (who has seen each story), (3) story tray ordering (unseen first, by recency), (4) fast delivery (stories are full-screen so buffering is highly visible), (5) forward/backward navigation within a user's story sequence.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Cassandra TTL | Set row TTL = 86400 at write; Cassandra deletes automatically | Zero external job complexity; TTL enforced at storage layer | Cassandra tombstones accumulate; compaction needed; viewer set in row is size-limited |
| External expiry worker + DB soft-delete | Worker scans for expired stories, soft-deletes, then hard-deletes | Full control of deletion flow (S3, CDN, DB) | Worker complexity; potential drift if worker lags |
| Redis sorted set with score = expiry_time | Store story_id with score = created_at + 86400; ZREMRANGEBYSCORE to find expired | Fast expiry queries | Redis is not primary store; data loss risk if Redis fails before flush |
| Hybrid: Cassandra TTL + async cleanup | Cassandra TTL marks row as deleted; async worker handles S3/CDN cleanup | Reliable expiry + complete media cleanup | Two systems to coordinate |

**Selected Approach: Hybrid — Cassandra TTL for row expiry + async S3/CDN cleanup worker**

Cassandra TTL guarantees stories are not served after 24 hours (read returns null for TTL-expired rows). Async cleanup worker handles media deletion from S3 (for cost) and CDN purge (to prevent cached media from being served). Worker scans `stories` table for rows approaching TTL using a secondary index on `expires_at`.

**Implementation Detail:**

```python
# Story creation
def create_story(user_id, media_key, caption, duration_s):
    story_id = generate_snowflake_id()
    expires_at = datetime.utcnow() + timedelta(hours=24)

    # Insert with TTL; Cassandra auto-deletes after 86400 seconds
    cassandra.execute("""
        INSERT INTO stories (user_id, story_id, media_key, media_type,
                             duration_s, caption, created_at)
        VALUES (%s, %s, %s, %s, %s, %s, %s)
        USING TTL 86400
    """, (user_id, story_id, media_key, 'image', duration_s,
          caption, datetime.utcnow()))

    # Track expiry in Redis sorted set for the cleanup worker
    redis.zadd("story_expiry_queue",
               {f"{user_id}:{story_id}:{media_key}": expires_at.timestamp()})

    # Notify story tray (followers see ring update)
    kafka.publish('story.created', {'user_id': user_id, 'story_id': story_id})
    return story_id

# Story fetch — story tray for a user
def get_story_tray(requesting_user_id):
    # Get all accounts this user follows
    followees = follow_service.get_followees(requesting_user_id)

    # For each followee with active stories, fetch stories
    story_rings = []
    for followee_id in followees:
        stories = cassandra.execute("""
            SELECT story_id, media_key, media_type, duration_s, caption, created_at
            FROM stories WHERE user_id = %s
        """, (followee_id,))

        if stories:
            # Check which stories this user has seen (from story_views table)
            seen_ids = get_seen_story_ids(requesting_user_id,
                                          [s.story_id for s in stories])
            story_rings.append({
                'user': user_service.get_user(followee_id),
                'stories': [format_story(s, seen=s.story_id in seen_ids)
                            for s in stories],
                'has_unseen': any(s.story_id not in seen_ids for s in stories)
            })

    # Sort: unseen-first, then by recency of newest story
    story_rings.sort(key=lambda r: (not r['has_unseen'],
                                    -r['stories'][0]['created_at'].timestamp()))
    return story_rings

# Cleanup Worker (runs every 5 minutes)
def cleanup_expired_stories():
    now = time.time()
    # Fetch all stories with expiry_score <= now
    expired = redis.zrangebyscore("story_expiry_queue", 0, now,
                                   withscores=False)
    for entry in expired:
        user_id, story_id, media_key = entry.split(':')

        # Delete media from S3
        s3.delete_object(Bucket=PROD_BUCKET, Key=media_key)

        # Purge CDN cache for this media URL
        cdn.purge_url(f"https://cdn.instagram.com/{media_key}")

        # Remove from Redis expiry queue
        redis.zrem("story_expiry_queue", entry)
```

**Interviewer Q&As:**

Q: How do you handle the "seen" state for Stories across multiple devices?
A: Story view events (`story_id`, `viewer_id`, `viewed_at`) are written to the `story_views` Cassandra table. This table is eventually consistent across devices. When the user opens another device, the story tray queries `story_views` for all story_ids of followed users and marks seen state. There's a brief eventual consistency window (~seconds) where a story seen on one device may still appear as unseen on another — acceptable for a social feature.

Q: What happens to a Story if the CDN has cached the media and the 24-hour window expires?
A: This is a race condition handled in layers: (1) Cassandra TTL ensures the story row is deleted, so direct API calls return 404 after expiry. (2) The cleanup worker purges the CDN cache key explicitly after expiry. (3) Media is deleted from S3. The CDN purge propagates globally within 30–60 seconds. There is a small window (seconds) where CDN edge nodes may still serve cached media after S3 deletion — this is mitigated by setting `Cache-Control: max-age=3600` (1 hour) for Story media (not `immutable` like post media), so edge nodes re-validate within 1 hour maximum. For the 24-hour boundary, we also issue a preemptive CDN purge 30 minutes before expiry.

Q: How do you build the story view count and viewer list for creators to see who viewed their story?
A: The `story_views` table stores all viewer_ids with TTL matching story TTL (86400s). Creator calls `GET /v1/stories/{story_id}/viewers` which queries this table paginated. For popular creators with millions of viewers, the viewer list may have millions of rows — we paginate and store only the count in the story row itself (updated via Kafka event from a view counter service). The exact viewer list is available only to the story author via paginated API. After story expiry, the `story_views` table rows are also TTL-deleted — viewers list is gone.

Q: How do you handle the story tray for a user who follows 10,000 accounts?
A: The story tray query is a scatter-gather: fetch follower list (10K), query Cassandra for active stories for each (batch multi-get by `user_id`). With Cassandra batch reads across partitions, 10K lookups in parallel (async I/O) complete in ~50ms. Redis bloom filter pre-check: before hitting Cassandra, check a bloom filter `has_active_stories:{user_id}` (set when story created, cleared when expired) to skip users with no active stories — reduces Cassandra calls by ~90% (only ~10% of followed users have active stories on any given day).

---

### Component: Explore Page (Content Discovery)

**Problem it solves:**
The Explore page surfaces content from accounts the user doesn't follow, personalized to their interests. It must: (1) rank billions of candidate posts, (2) filter out already-seen content, (3) exclude content from blocked users, (4) balance exploration (new interests) vs. exploitation (known preferences), (5) update recommendations without full recomputation every request.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Collaborative filtering | Users similar to you liked these posts | High-quality personalization | Cold start for new users; sparse matrix problem |
| Content-based filtering | Posts similar to posts you've engaged with | Works for new users (based on current engagement) | Limited diversity; filter bubble risk |
| Two-stage: candidate generation + ranking | Fast retrieval of candidate set, then ML ranking | Industry standard; balances speed and quality | Two-stage complexity; candidate set quality critical |
| Real-time trending + engagement signals | Show trending posts with high engagement velocity | Simple; fresh content | Not personalized; popular content dominates |

**Selected Approach: Two-stage pipeline — candidate retrieval (ANN) + ML ranking**

Offline job computes 128-dimensional user embeddings and post embeddings using a two-tower neural network trained on engagement signals. At query time: (1) retrieve top-500 candidate posts via Approximate Nearest Neighbor (ANN) search in embedding space using FAISS; (2) rank candidates using a lightweight gradient boosted model using user context features; (3) apply post-ranking filters (blocked users, seen content, content policy).

```python
# Explore page request handler
def get_explore_feed(user_id, cursor, count=24):
    # Step 1: Get user embedding from Feature Store (Redis-backed)
    user_embedding = feature_store.get_user_embedding(user_id)  # 128-dim float32

    # Step 2: ANN retrieval — top-500 candidate post embeddings
    candidate_post_ids = faiss_index.search(
        query_vector=user_embedding,
        k=500,
        filters={'created_at__gte': now() - timedelta(days=7)}  # Fresh content only
    )

    # Step 3: Filter out already-seen, blocked, deleted
    seen_ids = get_seen_post_ids(user_id)  # Redis bloom filter
    blocked_user_ids = get_blocked_users(user_id)
    candidates = [p for p in candidate_post_ids
                  if p not in seen_ids and p.author_id not in blocked_user_ids]

    # Step 4: Fetch features for ranking
    post_features = feature_store.batch_get_post_features(candidates[:200])
    # Features: like_velocity, comment_velocity, author_follower_count,
    #           user-author follow distance, content category, time since post

    # Step 5: Score with ranking model (ONNX, < 5ms inference)
    scores = ranking_model.predict(
        user_context={'user_id': user_id, 'embedding': user_embedding},
        post_features=post_features
    )

    ranked = sorted(zip(candidates[:200], scores), key=lambda x: -x[1])
    result = [post_id for post_id, score in ranked[:count]]

    # Mark as seen (Redis set with TTL = 30 days)
    redis.sadd(f"seen_explore:{user_id}", *result)

    return hydrate_posts(result)
```

**Interviewer Q&As:**

Q: How do you keep post embeddings fresh as engagement data changes?
A: Post embeddings are updated in two ways: (1) Incremental updates — Kafka stream of engagement events (likes, comments, saves) updates feature store in near-real-time. The ranking model uses live features even if embeddings are hours old. (2) Nightly full retrain of embeddings using the previous 90 days of engagement data; FAISS index rebuilt and hot-swapped (blue-green deployment of FAISS shard). The retrieval model is retrained weekly; ranking model retrained daily.

Q: How do you solve the cold-start problem for new users with no engagement history?
A: Cold-start strategy: (1) Onboarding interest selection — new users pick 3+ interest categories; use category-average embedding as initial user embedding. (2) Recent popular posts in selected categories shown initially. (3) After 5 engagement events, compute a personalized embedding from early signals. (4) Explore feed updates after every session close (async embedding recalculation). The transition from cold-start to personalized happens within the first session.

Q: What stops the Explore page from showing echo chamber content (same topics forever)?
A: Exploration-exploitation trade-off addressed by epsilon-greedy injection: 15% of Explore slots are filled with high-quality posts from categories the user hasn't engaged with recently (entropy maximization on category distribution). Additionally, FAISS retrieval uses diversification (MMR — Maximal Marginal Relevance) to ensure the candidate set spans multiple content clusters.

---

## 7. Scaling

### Horizontal Scaling

**Upload Service**: Stateless; auto-scales behind ALB on CPU or queue depth metrics. At peak 1,160 write QPS (photo posts), 20 Upload Service instances at ~60 QPS each is comfortable. Pre-signed URL approach means these instances handle only metadata, not bytes.

**Media Processor**: GPU instances for video transcoding (NVIDIA T4 instances). Scale based on Kafka consumer lag metric. Photo processing on CPU-optimized instances (C5 family). Process independently from each other — Kafka partitioned by media_id % partitions for even distribution.

**Feed Service**: Stateless. Scale on CPU and p99 latency. Redis cluster handles the state. At 578K read QPS, with each Feed Service instance handling ~5K QPS (Redis pipeline batch), ~120 instances needed at average; 500 at peak.

### Database Scaling

**Sharding:**

*Cassandra (posts)*: Consistent hashing on `post_id` distributes partitions across nodes automatically. With 50-node cluster (RF=3), each node holds ~1/17th of data. As data grows, add nodes — Cassandra auto-rebalances via token range redistribution.

*Posts by user* table: Partitioned by `user_id`, so all of a user's posts are on the same node — efficient profile page queries. At 2B users with avg 50 posts each = 100B rows; Cassandra handles this with horizontal scaling.

*MySQL (follows)*: Shard by `follower_id % 64`. Managed by Vitess. Write to shard; read from read replicas per shard.

**Replication:**

*Cassandra*: Multi-DC replication (3 US regions + 2 EU + 1 APAC). `NetworkTopologyStrategy` with RF=3 per DC. Writes use `LOCAL_QUORUM`; reads use `LOCAL_ONE` for feed (eventual consistency acceptable) and `LOCAL_QUORUM` for post creation confirmation.

*PostgreSQL (users)*: Streaming replication with 5 read replicas per region. Patroni manages automatic failover. Read replicas handle ~99% of user profile lookups.

**Caching:**

- **L1: Redis Cluster (feed cache)**: 500 post_ids per user, TTL 3 days. Cluster size: 200M DAU × 500 IDs × 8 bytes = 800 GB → 10 Redis nodes with 100GB RAM each.
- **L2: Redis (post object cache)**: Hot posts (top 10% by view count) cached as full JSON blobs. LRU eviction. Reduces Cassandra reads by ~80%.
- **L3: CDN**: All media cached at edge. Cache-Control: `max-age=31536000, immutable` for permanent post media; `max-age=3600` for story media (to enable timely expiry).

**CDN:**

Instagram relies heavily on CDN for media delivery. Architecture:
1. Photos uploaded → processed → stored in S3 with content-addressed keys (hash in path).
2. CDN origin is S3 bucket; multiple CDN providers (Fastly + Akamai) for redundancy.
3. CDN edge nodes cache based on URL path. Cache hit rate ~95% for photos (popular posts requested millions of times), ~80% for Stories (shorter lifetime, more diverse).
4. For video (Reels), adaptive bitrate segments are cached per segment key — high cache hit rate since viral videos get many views.

### Interviewer Q&As on Scaling

Q: Your Redis feed cache is 800 GB. How do you handle Redis cluster split-brain?
A: Redis Cluster uses a majority-based quorum for slot ownership — split-brain is prevented by requiring > 50% of masters to be alive. During a network partition where Redis can't achieve majority, writes fail (return error) rather than accepting writes that would diverge. Feed Service handles Redis write failures by marking the post fanout as "pending" and retrying via Kafka dead-letter queue. The trade-off: brief fanout delay during partition vs. data corruption from split-brain.

Q: How do you scale the follow graph lookup for fan-out workers fetching all followers of a celebrity?
A: We don't. For celebrities (>1M followers), fan-out on write is disabled — the celebrity post is not pushed to follower feed caches. Instead, Feed Service reads the celebrity's recent posts from Cassandra `posts_by_user` at read time and merges them into the feed response. The follow graph lookup for fan-out is only needed for normal users (<1M followers), where the follower list is paginated in batches of 1000. MySQL index on `followee_id` handles paginated scans efficiently — a B-tree scan of the `idx_followee` index covering (followee_id, follower_id).

Q: The post object cache in Redis is getting too expensive. How do you reduce cost?
A: Tiering strategy: (1) Hot tier (Redis, in-memory): Top 1% of posts by engagement velocity — ~1M posts. (2) Warm tier (Memcached with SSD backing or Amazon ElastiCache): Posts viewed > 1K times in last 24h — ~10M posts. (3) Cold tier (Cassandra): All other posts. Route post hydration requests through cache hierarchy. Use a Bloom filter to avoid hitting Cassandra for keys known to not be in warm cache. This can reduce Redis cost by 80% while maintaining sub-5ms cache hit latency.

Q: How do you handle the database migration when you need to add a new column to the posts table?
A: Cassandra: Adding a column is a non-blocking, zero-downtime operation — Cassandra adds the column metadata to the schema without touching existing rows. New rows include the new column; old rows return null for the new column (application handles null gracefully). For MySQL (users, follows): use online schema change tools like pt-online-schema-change (pt-osc) or gh-ost which create a shadow table, copy data, apply incremental changes via triggers, and swap tables atomically — no downtime, no table locks for reads.

Q: What happens to the explore algorithm when Instagram onboards a new content category (e.g., AR filters)?
A: New content categories require: (1) Add category label to post metadata schema (Cassandra column addition). (2) Retrain two-tower embedding model with new category as a feature. (3) New posts from this category flow into FAISS index automatically. (4) Cold-start for the category: initially shown to users with high "novelty seeking" scores (users who click on new content types frequently). (5) Gradually increase exposure as engagement signal quality improves. A/B test the new category exposure rate before full rollout.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Mitigation |
|-----------|-------------|--------|------------|
| Upload Service crash | Client can't initiate upload | Photo post fails | Stateless; LB routes to healthy instance within 5s; client retries |
| Media Processor crash | Post stuck in "processing" state | Post not visible to followers | Kafka replay from last offset; 1-hour timeout triggers requeue |
| S3 outage (single region) | Media uploads and reads fail | Uploads fail; media not served | Cross-region S3 replication (CRR); CDN edge continues serving cached content |
| Redis cluster failure | Feed reads miss cache; fanout writes fail | Feed served from Cassandra (slow fallback); fanout delayed | Redis Cluster auto-failover (<30s); Kafka buffers fanout events during outage |
| Cassandra node failure | Some post reads/writes fail | p99 latency spike; some writes may retry | RF=3, LOCAL_QUORUM tolerates 1-node failure per DC; coordinator auto-retries |
| CDN outage (single PoP) | Media unavailable for users in that region | Media loading fails for affected users | CDN anycast routes to next nearest PoP automatically |
| Story expiry worker crash | Stories not deleted from S3/CDN on time | Expired stories potentially accessible via direct URL for extra minutes | CDN TTL: `max-age=3600` limits exposure window; Cassandra TTL prevents API serving; S3 lifecycle policy deletes objects after 25 hours |
| FAISS index corruption | Explore page returns empty/wrong results | Explore page broken | FAISS index rebuilt nightly and stored on S3; hot-swap to last known good index on error detection |

### Failover Strategy

- **Active-active multi-region**: Traffic split across us-east-1, eu-west-1, ap-southeast-1 using GeoDNS routing. Each region is independent (no cross-region synchronous writes). Cassandra multi-DC replication provides async cross-region data propagation (lag: ~100ms).
- **Circuit breakers**: Feed Service has circuit breakers around Cassandra calls (fallback: return cached response), Explore Service (fallback: return trending posts), and ML ranking service (fallback: return chronologically sorted candidates without ranking).
- **Graceful degradation**: If Feed Service can't complete within 200ms, return whatever posts are available (partial response) rather than a 503 error.

### Retries & Idempotency

- **Photo upload**: Client generates idempotency key (UUID) for `POST /v1/posts`. Server stores (idempotency_key → post_id) in Redis (24h TTL). Duplicate requests return same post_id without creating a new post.
- **Media Processor**: Idempotent — same `media_id` input produces same output files. Re-processing the same event overwrites the same S3 keys with the same content.
- **Fan-out Worker**: Redis ZADD with same (member, score) is a no-op — naturally idempotent.
- **Story creation**: Idempotency key on `POST /v1/stories` prevents duplicate stories from client retries.

### Circuit Breaker

- Implemented at Feed Service: circuit around Cassandra post hydration (open if > 40% error rate over 10s window).
- Fallback: Serve truncated feed from Redis cache (post_ids without full hydration) and show placeholder post cards until circuit closes.
- Half-open: Every 30 seconds, allow 1 request through to probe Cassandra health.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Why It Matters |
|--------|------|-----------------|----------------|
| Feed read p99 latency | Histogram | > 300ms | Core UX SLA |
| Media upload success rate | Counter | < 99.5% | Upload reliability |
| Story expiry lag (P99 time from expiry to S3 delete) | Histogram | > 30 minutes | GDPR/privacy SLA on ephemeral content |
| Media processing queue depth (Kafka lag) | Gauge | > 100K messages | Processing backlog detection |
| CDN cache hit rate | Ratio | < 90% for photos | CDN efficiency; cost signal |
| Redis feed cache hit rate | Ratio | < 85% | Cache effectiveness |
| Fan-out worker lag (Kafka) | Gauge | > 50K messages | Follower feed staleness |
| Explore ranking model inference p99 | Histogram | > 50ms | Explore UX SLA |
| FAISS query latency | Histogram | > 20ms | Retrieval bottleneck |
| Cassandra write latency p99 | Histogram | > 10ms | Storage layer health |
| Story views/second | Counter | Sudden drop > 50% | Story serving outage |
| S3 error rate | Counter | > 0.1% | Storage availability |

### Distributed Tracing

- All services instrument with OpenTelemetry. Trace IDs propagated via HTTP headers.
- Spans for: API Gateway routing, S3 upload, Kafka publish, Media Processor transcoding (per stage), Cassandra write, Fan-out per batch, Feed read, ANN query, ranking model inference.
- Traces stored in Jaeger (14-day retention). Critical: trace the full path from `POST /v1/posts` to "post visible in follower feed" to measure true end-to-end latency including async fan-out.
- Sampling: 100% for error traces; 1% for successful traces; 10% for requests with p99 > threshold.

### Logging

- Structured JSON logs: `{timestamp, trace_id, service, user_id, post_id, media_id, level, message, duration_ms, region}`.
- Key log patterns: `"s3_upload_failed"` (alert if > 10 per minute), `"story_expiry_overdue"` (alert if any), `"fanout_celebrity_skip"` (informational, monitor counts).
- Log aggregation: Splunk or OpenSearch. Retention: ERROR 90 days (compliance), INFO 7 days.
- Media processing logs include per-stage timing (download, resize, encode, upload per variant) to identify transcoding bottlenecks.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|----------|---------------|-----------------|--------|
| Media upload path | Pre-signed S3 URL (direct client-to-S3) | Route through app servers | Direct upload eliminates app server bandwidth bottleneck; scales without adding Upload Service capacity |
| Story deletion | Cassandra TTL + async S3/CDN cleanup | External polling job only | Cassandra TTL guarantees data not served after 24h at storage layer; S3 cleanup is best-effort for cost |
| Feed generation | Hybrid fan-out (push for normal, pull for celebrities) | Pure push or pure pull | Pure push creates O(followers) write storm for celebrities; pure pull too slow for users following thousands |
| Story viewer tracking | Cassandra `story_views` table with TTL | In-memory set in Redis | Redis set unbounded growth for viral stories with millions of viewers; Cassandra handles arbitrary viewer counts with time-ordered storage |
| Explore personalization | Two-tower ANN + lightweight ranker | Collaborative filtering alone | CF suffers cold-start; ANN on embeddings provides low-latency candidate retrieval; separate ranking allows feature richness |
| Photo format | WebP for storage, JPEG for legacy clients | JPEG only | WebP is 25–34% smaller than JPEG at same quality; significant storage and bandwidth savings at Instagram's scale; fallback JPEG for older iOS/Android |
| Comment storage | Cassandra partitioned by post_id | MySQL with post_id FK | Post comment workload is append-heavy and read-heavy by post_id — perfect fit for Cassandra; MySQL would require heavy indexing and sharding |
| Consistency model | Eventual consistency for feeds | Strong consistency | Strong consistency requires QUORUM reads/writes with cross-DC coordination; 100–200ms additional latency per feed load; not worth it for social content |
| CDN strategy | Two CDN providers (Fastly + Akamai) | Single CDN | Single CDN creates vendor lock-in and single point of failure; two providers enable failover and geographic preference |

---

## 11. Follow-up Interview Questions

**Q1: How would you design Instagram's Direct Messaging (DMs) at scale?**
A: DMs require: persistent conversation threads, real-time delivery (WebSocket), message status (sent/delivered/read), group messaging. Data model: `conversations (convo_id, participants[], created_at)`, `messages (convo_id, message_id Snowflake, sender_id, content, media_key, status, created_at)` in Cassandra (partitioned by convo_id). Real-time delivery: long-polling or WebSocket connections from clients to a Presence/Messaging Service. Message delivery via Kafka: publisher → Kafka topic partitioned by convo_id → consumer pushes to recipient WebSocket. Offline delivery: APNs/FCM push notification when recipient not connected. Read receipts: update message status in Cassandra; stream status updates via WebSocket.

**Q2: How do you design the "close friends" Stories feature?**
A: Users designate a subset of followers as "close friends" — their Stories are only visible to that list. Implementation: add `audience` field to stories row: `'all' | 'close_friends'`. Add `close_friends_list (user_id, friend_id)` table in MySQL (typically small list, 5–200 people). At read time, story tray fetch filters: if story has `audience='close_friends'`, only include it for requesting users on the creator's close_friends_list. Check is O(1) using a Redis set `close_friends:{user_id}` (invalidated on list updates).

**Q3: How would you add end-to-end encryption to Instagram DMs?**
A: Use Signal Protocol (Double Ratchet + X3DH key exchange). Each client generates identity key pair and one-time prekeys on first install. Public keys stored on Key Distribution Service. Before first message, sender fetches recipient's public key bundle, completes X3DH handshake, derives shared secret. All message content encrypted client-side. Server stores only ciphertext — zero knowledge of message content. Challenges: key backup (encrypted key bundle backed up to iCloud/Google Drive), multi-device sync (Sesame protocol for device linking), and inability to serve ads on encrypted message content (business model implication).

**Q4: Design a "Collab" post feature where two creators co-author a post.**
A: A Collab post appears in both creators' profile grids and in their respective followers' feeds. Data model: add `collab_author_id` field to posts. Fan-out: fanout worker processes the post twice — once for `user_id`'s followers, once for `collab_author_id`'s followers. Deduplication: if both accounts follow the same person, deduplicate using a Redis set `feed_seen:{user_id}` during fanout. Profile grids: `posts_by_user` table has rows for both creators pointing to the same `post_id`. Like/comment counts are unified on the single post object.

**Q5: How would you implement a "Paid Partnership" disclosure system for influencer posts?**
A: Paid partnerships require FTC compliance — disclosed posts must be clearly labeled. Data model: add `paid_partnership_label TEXT` and `paid_partner_business_id BIGINT` to posts. UI renders a "Paid partnership with @brand" label. Write path: creator must explicitly check "paid partnership" and select the partner business account before post goes live. Enforcement: ML classifier trained on known paid partnership posts + hashtag patterns (#ad, #sponsored) — flags potential undisclosed paid content for human review. Audit log (immutable Cassandra append) tracks all disclosed and flagged partnerships for regulatory queries.

**Q6: How do you handle image copyright violations at upload time?**
A: Three-layer approach: (1) Perceptual hash (pHash) matching against known copyright database (PhotoDNA API or internal hash database). Match within Hamming distance of 5 → block upload immediately. (2) ML scene/object classifier to detect known copyrighted characters, artworks, logos. (3) Watermark detection for common photographer watermarks. False positive rate is critical — the system surfaces matches to a human review queue rather than auto-blocking for edge cases. DMCA takedown handling is a separate manual review workflow. Hashing is done at upload time in Media Processor, before fan-out, to prevent wide distribution of copyrighted content.

**Q7: What changes to the architecture would Reels (short-form video) require?**
A: Reels adds: (1) Video-specific transcoding pipeline — HLS/DASH segmented format with multiple bitrate ladders (360p, 720p, 1080p) + vertical aspect ratio (9:16). (2) Thumbnail frame extraction at 0.5s for preview. (3) Audio extraction for audio matching (for music/lip-sync features) — fingerprint with AudD or internal audio DB. (4) Video recommendation replaces image-based FAISS — video embeddings from video encoder model (VideoMAE or similar). (5) Autoplay prefetching: client prefetches next 2–3 Reels before user scrolls to them using predicted scroll speed. (6) Loop count tracking in analytics (different from view count).

**Q8: Describe how you'd implement cross-post analytics (views, reach, impressions) for creator accounts.**
A: Analytics events: client fires `post_viewed (post_id, user_id, view_duration_ms, view_source)` events to an Analytics Ingest Service. Events buffered client-side (up to 50 events, 5s flush interval) and batched to reduce API calls. Analytics Ingest publishes to Kafka `analytics.events` topic. Flink consumes, aggregates per post over multiple windows (1h, 24h, 7d, 28d). Aggregated metrics stored in a time-series DB (ClickHouse or Apache Druid). Creator dashboard reads from ClickHouse via a pre-aggregated report. For reach (unique viewers), use HyperLogLog sketch per (post_id, day) to count distinct user_ids in O(1) memory.

**Q9: How do you build the "Suggested Users to Follow" feature?**
A: Graph-based recommendations: (1) 2-hop neighbors — users your followees follow but you don't. Computed offline via Spark job on follow graph. (2) Contact matching — users whose phone numbers/emails match your uploaded contact list (with privacy consent). (3) Interest similarity — users whose posts you engage with on Explore but don't follow. (4) Location-based — users near your GPS location (opt-in). Output: top-20 candidates ranked by mutual follow count, engagement likelihood (from collaborative filtering model). Served from Redis (precomputed per user, refreshed nightly). Cold-start: interest category users as default candidates.

**Q10: How do you handle a sudden 10× traffic spike (e.g., a celebrity posts during a global event)?**
A: Prepared responses: (1) Auto-scaling: Feed Service and Fan-out Workers scale out via Kubernetes HPA triggered by CPU/QPS metrics — scales from 120 to 1000 pods in ~2 minutes. (2) Celebrity fast path: celebrity's post bypasses push fan-out, handled by pull at read time — spike in reads, not writes. (3) Kafka absorbs write spikes — Fan-out Worker lag increases but no data loss. (4) CDN absorbs media read spike — celebrity's new post gets prefetched to CDN edge by a "warm CDN" call during fan-out for popular accounts. (5) Database read spike: Redis feed cache has high hit rate; Cassandra read load protected by cache. (6) Load shedding: API Gateway drops requests exceeding per-user rate limits and global concurrency limits with 429 responses.

**Q11: What would a data retention policy for Instagram post data look like?**
A: Four categories: (1) User-visible content (posts, comments): retained indefinitely unless user deletes. (2) Ephemeral content (Stories): deleted after 24h from all systems (see Story expiry design). (3) Deleted content: 30-day soft delete window (user can restore), then 90-day hard delete from Cassandra and S3. After 90 days, scrubbed from search index, CDN cache purged. (4) Analytics data: aggregated anonymized data retained 2 years; raw event-level data retained 90 days. GDPR data export: user can request all their data — async job queries all systems, packages into a ZIP file, sends download link (valid 24h).

**Q12: How does the feed ranking algorithm decide what to show first in the home feed?**
A: The feed ranking model (deployed in the Feed Service) scores each candidate post from the Redis cache + celebrity pull. Features used: (1) Relationship strength: how often the viewer has liked/commented on this author's posts historically. (2) Post freshness: exponential time decay (score × e^(-λt), λ tuned to ~6 hours half-life). (3) Post engagement velocity: like/comment rate per hour since posting. (4) Content format preference: does this user engage more with Reels, carousels, or single photos? (5) Session context: what has the user already seen this session. The ranking model is a shallow neural network (2 layers, ~500K parameters), inference < 3ms via TensorRT on CPU.

**Q13: What are the privacy implications of the Follow Graph and how do you protect it?**
A: The follow graph is a sensitive social graph. Protections: (1) Private accounts: follow graph not queryable by non-followers via API. (2) Mutual follow data: "followed by X people you know" shown only to the requester, not publicly. (3) Internal access controls: follow graph lookups require service-level auth tokens with specific scopes; not accessible by default. (4) Differential privacy: aggregate statistics (follower count histogram) computed with ε-differential privacy to prevent inference attacks. (5) Rate limiting: the `GET /v1/users/{user_id}/followers` API is rate-limited per requester to prevent scraping the full follow graph of a celebrity.

**Q14: How do you handle multi-region data residency requirements (GDPR for EU users)?**
A: EU users' data must be stored in EU. Implementation: User's primary region stored in their profile (set based on signup geo). All write paths check primary region and route to the appropriate regional Cassandra cluster. EU Cassandra cluster (`eu-west-1, eu-central-1`) never replicates to US clusters for EU users. Read path: if requesting user and content owner are both EU, serve from EU cluster. Cross-region follows (EU user follows US celebrity): celebrity's content is replicated to EU for serving, but EU user's data stays in EU. Audit: data lineage tracked per user_id → region mapping for compliance reporting.

**Q15: How would you design Instagram's "Live" feature (real-time video streaming)?**
A: Live streaming is fundamentally different from Stories. Components: (1) Broadcaster client captures video, encodes with WebRTC or RTMP, streams to Edge Ingest Servers (low-latency RTMP ingest). (2) Transcoding: Ingest Server transcodes to HLS with low-latency HLS (LLHLS, 2–4s latency) at multiple bitrates. (3) Distribution: LLHLS segments pushed to CDN in near-real-time (segment size = 1–2s). (4) Viewer delivery: clients pull HLS segments from CDN. (5) Live comments: WebSocket channel separate from video; comments delivered via Kafka fan-out to WebSocket connections of all viewers. (6) Scale: a single broadcaster with 1M concurrent viewers requires CDN to serve 1M × ~500KB/s = 500 GB/s — CDN edge servers absorb this; origin sees only one stream copy per CDN PoP.

---

## 12. References & Further Reading

- Instagram Engineering Blog: "Stores Architecture at Instagram" — describes the evolution from MySQL to Cassandra for post storage.
- Instagram Engineering Blog: "What Powers Instagram: Hundreds of Instances, Dozens of Technologies" — original 2011 architecture overview.
- Facebook Engineering: "The Infrastructure Behind Facebook Direct Messages" — comparable architecture for messaging at scale.
- Cormode, G. & Hadjieleftheriou, M. (2008). "Finding Frequent Items in Data Streams." — Count-Min Sketch applications in streaming systems.
- Flink documentation: flink.apache.org — stateful stream processing used in the trending/analytics pipeline.
- "Efficient Processing of Deep Neural Networks: A Tutorial and Survey" — relevant for understanding ML model serving for Explore ranking.
- FAISS documentation: faiss.wiki.github.io — Facebook AI Similarity Search, used for ANN retrieval in Explore.
- Cassandra documentation on TTL: cassandra.apache.org/doc/latest/cassandra/cql/dml.html — `USING TTL` and tombstone mechanics.
- AWS S3 Pre-signed URL documentation: docs.aws.amazon.com/AmazonS3/latest/userguide/using-presigned-url.html
- "The DataCenter as a Computer" (Barroso, Hölzle, Ranganathan) — foundational reading for data center-scale storage and compute architecture.
- Kleppmann, M. "Designing Data-Intensive Applications" (O'Reilly, 2017) — Chapters 3 (Storage engines), 10 (Batch processing), 11 (Stream processing).
- "Two-Tower Model for Retrieval" — Google AI Blog on two-tower neural networks for recommendation retrieval, directly applicable to Explore page design.
- Perceptual hashing for image deduplication: phash.org — pHash algorithm reference.
- WebP format specification: developers.google.com/speed/webp — Google's WebP codec, relevant for media compression trade-offs.
