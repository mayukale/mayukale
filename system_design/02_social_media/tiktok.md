# System Design: TikTok

---

## 1. Requirement Clarifications

### Functional Requirements
- Users see a personalized "For You Page" (FYP) — an infinite scroll of short-form videos (15s–10min) recommended by the algorithm
- Users can upload videos with captions, hashtags, sounds, and effects
- Users can follow other creators; a "Following" feed shows only followed creators' videos
- Users can like, comment, share, and save/bookmark videos
- Users can duet (side-by-side reaction video) and stitch (cut into another video) other creators' videos
- Trending sounds, hashtags, and challenges surfaced in Discover/Explore
- Creator analytics: views, likes, shares, audience demographics
- Live streaming (real-time broadcasting)
- Monetization: creator fund, gifts in live, brand partnerships
- Direct messages between users

### Non-Functional Requirements
- Availability: 99.99% uptime — entertainment platform; any downtime is high-visibility and highly impactful
- FYP video delivery: p99 < 2 seconds for first video to start playing — buffering kills TikTok's scroll behavior
- Video upload processing: p99 < 30 seconds for video to be live and distributable after upload
- Feed recommendation freshness: new videos from followed creators visible in Following feed within 60 seconds
- Algorithm must cold-start a new user into a personalized FYP within 3–5 video interactions
- Scale: 1.5 billion MAU, ~700 million DAU, ~5 billion videos viewed per day
- Global content distribution: content must be served with < 100ms media latency from CDN edge for 95th percentile of global users
- Video quality: adaptive bitrate delivery (360p → 1080p) based on network conditions

### Out of Scope
- TikTok Ads auction system
- Creator monetization fund payment processing
- TikTok Shop (e-commerce integration)
- Brand safety tools
- Content moderation (Trust & Safety) pipeline (acknowledged but not deep-dived)
- Live streaming full architecture

---

## 2. Users & Scale

### User Types
- **Passive viewers**: Watch FYP, rarely create — 60% of users
- **Occasional creators**: Post 1–5 videos/month — 30% of users
- **Active creators**: Post daily, engaged community — 9% of users
- **Top creators / celebrities**: Millions of followers, every video goes viral — 1% of users
- **Brands / Business accounts**: Promotional content, paid campaigns — distinct use case

### Traffic Estimates

| Metric | Value | Reasoning |
|--------|-------|-----------|
| MAU | 1.5 billion | Publicly reported figure |
| DAU | 700 million | ~47% DAU/MAU; TikTok's high daily engagement rate |
| Videos viewed/day | 5 billion | ~7 videos/DAU/day avg (TikTok sessions average 52 min/day → ~7 short videos/min × 7 min avg session = ~49, but many are background scrolls; estimate conservatively at 7) |
| FYP request QPS (avg) | ~232,000 | 700M DAU × 29 feed page loads/day / 86400 |
| FYP request QPS (peak) | ~1,000,000 | ~4.3× evening global peak |
| Video uploads/day | 50 million | ~0.07 uploads per DAU; creator fraction × avg uploads |
| Upload write QPS (avg) | ~580 | 50M / 86,400 |
| Upload write QPS (peak) | ~2,500 | 4.3× peak |
| Likes/day | 10 billion | ~2 likes per viewed video |
| Comments/day | 1 billion | ~0.2 comments per viewed video |

### Latency Requirements
- **First video playback start: p99 < 2s** — TikTok's scroll behavior requires near-instant video start; buffering causes users to leave the session; this is the #1 UX metric
- **FYP page generation (API): p99 < 200ms** — The video list API must respond quickly; actual video data arrives from CDN
- **Video upload acknowledgment: p99 < 5s** — Creator must see confirmation quickly; actual processing is async
- **Following feed freshness: 60s** — Creators' followers should see new videos within 1 minute; this is the fan-out SLA
- **Like/comment write: p99 < 300ms** — Interactive actions must feel instant

### Storage Estimates

| Data Type | Size/record | Records/day | Retention | Total |
|-----------|-------------|-------------|-----------|-------|
| Raw video upload (before compression) | 50 MB avg | 50M/day | Temporary (7 days) | 2.5 PB/day staging (purged after processing) |
| Processed video (HLS segments, multiple qualities) | 100 MB avg (all variants) | 50M/day | Forever | 5 TB/day → ~1.8 PB/year |
| Video metadata | 2 KB | 50M/day | Forever | 100 GB/day |
| Thumbnails (3 sizes per video) | 200 KB total | 50M/day | Forever | 10 GB/day |
| User profiles | 5 KB | 2M new users/day | Forever | 10 GB/day |
| Follow graph edges | 16 bytes | 500M new edges/day | Forever | 8 GB/day |
| Likes | 16 bytes | 10B/day | Forever (ranking signals) | 160 GB/day |
| Comments | 500 bytes | 1B/day | Forever | 500 GB/day |
| Video interaction events (for ML training) | 200 bytes | 50B/day (all events) | 1 year for training | 10 TB/day events |

**Total permanent storage growth: ~5 TB/day (video) + ~1 TB/day (metadata, interactions) = ~6 TB/day.**
**Annual video storage: ~1.8 PB — requires a massive, tiered object storage strategy.**

### Bandwidth Estimates

| Direction | Calculation | Result |
|-----------|-------------|--------|
| Inbound video uploads | 580 QPS × 50 MB raw | ~29 GB/s upload ingest |
| Outbound video delivery (CDN edge) | 5B views/day × 30 MB avg / 86400 | ~1.7 TB/s total CDN outbound |
| CDN origin fill (5% miss rate) | | ~87 GB/s origin fill |
| FYP API responses (JSON only) | 232K QPS × 5 KB response | ~1.1 GB/s API |

---

## 3. High-Level Architecture

```
  ┌─────────────────────────────────────────────────────────────────────────┐
  │                             CLIENTS                                      │
  │                iOS / Android / Web (tiktok.com)                          │
  └─────────────────────────────────────────────────────────────────────────┘
                            │ HTTPS / HTTP2 / QUIC
                            ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │             Global CDN (Akamai / Cloudflare / BytePlus CDN)             │
  │  Video segments (HLS/DASH), thumbnails, profile photos                   │
  │  1,000+ PoPs globally; p95 media latency < 100ms                         │
  └───────────────────────────────┬─────────────────────────────────────────┘
                                  │ Cache miss / API requests
                                  ▼
  ┌─────────────────────────────────────────────────────────────────────────┐
  │           API Gateway (ByteDance's internal reverse proxy)               │
  │   TLS termination, auth, rate limiting, A/B experiment routing           │
  │   Service mesh routing (gRPC between internal services)                  │
  └─────┬────────────────┬──────────────────────┬────────────────────────────┘
        │                │                      │
  ┌─────▼──────┐  ┌──────▼──────────┐  ┌────────▼──────────────────────────┐
  │  Upload    │  │  FYP Service    │  │  User / Follow / Social Service   │
  │  Service   │  │  (Rec Engine)   │  │  - Profiles, connections          │
  │  - Ingest  │  │  - FYP gen      │  │  - Following feed                 │
  │    video   │  │  - Following    │  │  - Duet/Stitch permissions        │
  │  - Validate│  │    feed         │  └───────────────────────────────────┘
  │  - Queue   │  │  - Re-rank      │
  └─────┬──────┘  └──────┬──────────┘
        │                │
        ▼                │
  ┌──────────────────────────────────────────────────────────────────────┐
  │                   Kafka (Message Bus)                                 │
  │  Topics: video.uploaded, video.processed, interaction.event,         │
  │          follow.event, fyp.feedback, video.trending                   │
  └──────────┬───────────────────────────────────────────────────────────┘
             │
   ┌─────────┼────────────────────────────────────────────┐
   │         │                                            │
   ▼         ▼                                            ▼
┌────────────────────┐  ┌────────────────────────┐  ┌─────────────────────────┐
│ Video Processing   │  │  FYP Recommendation    │  │  Following Feed         │
│ Pipeline           │  │  Engine                │  │  Fan-out Worker         │
│ - Transcode        │  │  (TensorFlow/PyTorch)  │  │  - Push video_id to     │
│ - Extract features │  │  - Candidate retrieval │  │    follower feeds        │
│   (audio, visual)  │  │  - Ranking model       │  │  - Hybrid for top       │
│ - Safety scan      │  │  - Feedback loop       │  │    creators             │
│ - CDN upload       │  └────────────────────────┘  └─────────────────────────┘
│ - Index for rec    │
└────────────────────┘

  ┌────────────────────────────────────────────────────────────────────────┐
  │                         DATA STORES                                     │
  │                                                                          │
  │ ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐   │
  │ │  Video Metadata  │  │  FYP Cache       │  │  User/Follow Store   │   │
  │ │  (TiKV / MySQL)  │  │  (Redis Cluster) │  │  (MySQL / TiDB)      │   │
  │ │  Sharded by      │  │  Precomputed     │  │  Connection graph     │   │
  │ │  video_id        │  │  video_id lists  │  │  sharded             │   │
  │ └──────────────────┘  └──────────────────┘  └──────────────────────┘   │
  │ ┌──────────────────────────────────────────────────────────────────┐   │
  │ │  Video Object Store (S3 / ByteDance's internal object store)     │   │
  │ │  HLS segments, thumbnails, all quality variants                  │   │
  │ └──────────────────────────────────────────────────────────────────┘   │
  │ ┌──────────────────────────────────────────────────────────────────┐   │
  │ │  Interaction Event Store (ClickHouse / Apache Flink)             │   │
  │ │  Plays, likes, shares, skips, replay events for ML training      │   │
  │ └──────────────────────────────────────────────────────────────────┘   │
  └────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **CDN**: The most critical component — TikTok's business model requires near-zero video latency. ByteDance operates its own CDN infrastructure (BytePlus) in addition to using Akamai/Cloudflare. HLS video segments pre-fetched to CDN nodes based on predicted popularity.
- **Upload Service**: Accepts video uploads. Validates format (MP4/MOV/WebM), size (<10 GB), duration (15s–10min). Stores to S3 staging bucket via pre-signed URL. Publishes `video.uploaded` to Kafka.
- **Video Processing Pipeline**: The most compute-intensive component. Transcodes to HLS/DASH (5 quality ladders), extracts visual embeddings, audio fingerprints, detects objects/text/faces, runs safety classifiers. Uploads processed segments to CDN origin.
- **FYP Service**: Retrieves precomputed recommendation candidates from Redis, merges with real-time trending signals, runs re-ranking model, returns ordered video_id list.
- **FYP Recommendation Engine**: Offline/near-line ML system. Computes user embeddings and video embeddings. Runs ANN retrieval. Trains ranking models on interaction feedback. Updates candidate lists in Redis.
- **Following Feed Fan-out Worker**: Pushes new video_id to each follower's following-feed Redis list. Hybrid push/pull for top creators.
- **TiKV**: ByteDance's production deployment uses TiKV (an open-source distributed key-value store built on RocksDB + Raft) for video metadata. Alternative: MySQL/TiDB for ACID needs.
- **ClickHouse**: High-performance columnar store for interaction events. Supports sub-second analytical queries on billions of events — critical for real-time feature computation.

**End-to-End Flow — FYP Page Load:**
1. Client sends `GET /v1/fyp` with auth token, device context (battery, network type), and cursor.
2. API Gateway validates, routes to FYP Service.
3. FYP Service retrieves precomputed candidate list from Redis: `fyp:{user_id}` → [video_id, score, ...].
4. FYP Service merges candidates with: (a) trending videos in user's region (from Trend Redis sorted set), (b) new videos from followed creators (Following feed intersection).
5. Re-ranking: lightweight online model re-scores final candidate set using real-time features from ClickHouse (video's current engagement velocity).
6. Diversity enforcement: no more than 2 consecutive videos from same creator.
7. Return top-10 video_ids with metadata (caption, author, sound, thumbnail_url, video_manifest_url).
8. Client pre-fetches video manifest + first HLS segment from CDN for the next video simultaneously while playing current video.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Videos (TiKV / MySQL sharded by video_id)
-- ============================================================
CREATE TABLE videos (
    video_id          BIGINT PRIMARY KEY,       -- Snowflake ID
    creator_id        BIGINT NOT NULL,
    title             VARCHAR(150),
    caption           TEXT,                      -- Max 2200 chars
    sound_id          BIGINT,                    -- FK to sounds table (original or licensed)
    hashtags          JSON,                      -- Array of hashtag strings
    duration_s        FLOAT,                     -- 0.5s precision
    video_width       INT,
    video_height      INT,
    hls_manifest_key  VARCHAR(255),              -- S3/CDN key for master HLS playlist
    thumbnail_key     VARCHAR(255),              -- S3 key for cover thumbnail
    qualities_keys    JSON,                      -- {360p: s3key, 720p: s3key, 1080p: s3key}
    visual_embedding  BLOB,                      -- 128-dim float32 vector (binary packed)
    audio_fingerprint VARCHAR(64),               -- SHA-256 of audio signature
    view_count        BIGINT DEFAULT 0,
    like_count        BIGINT DEFAULT 0,
    comment_count     BIGINT DEFAULT 0,
    share_count       BIGINT DEFAULT 0,
    save_count        BIGINT DEFAULT 0,
    avg_watch_pct     FLOAT DEFAULT 0.0,         -- 0.0 - 1.0; updated async
    replay_rate       FLOAT DEFAULT 0.0,
    is_deleted        BOOLEAN DEFAULT FALSE,
    processing_status ENUM('pending','processing','live','failed') DEFAULT 'pending',
    region_restrictions JSON DEFAULT '[]',       -- Regions where video is blocked
    created_at        DATETIME DEFAULT NOW(),
    INDEX idx_creator (creator_id, created_at DESC),
    INDEX idx_hashtag (hashtags(100)),
    INDEX idx_sound (sound_id, like_count DESC)
);

-- ============================================================
-- Sounds (original audio tracks used in videos)
-- ============================================================
CREATE TABLE sounds (
    sound_id          BIGINT PRIMARY KEY,
    original_video_id BIGINT,                   -- Source video, if user-original
    title             VARCHAR(150),
    artist_name       VARCHAR(100),
    is_licensed       BOOLEAN DEFAULT FALSE,    -- True for commercial tracks
    audio_key         VARCHAR(255),             -- S3 key for audio clip
    waveform_key      VARCHAR(255),             -- S3 key for waveform visualization
    usage_count       BIGINT DEFAULT 0,
    created_at        DATETIME DEFAULT NOW(),
    INDEX idx_usage (usage_count DESC)
);

-- ============================================================
-- Users (MySQL / TiDB — ACID for account management)
-- ============================================================
CREATE TABLE users (
    user_id           BIGINT PRIMARY KEY,
    username          VARCHAR(24) UNIQUE NOT NULL,
    display_name      VARCHAR(30),
    bio               TEXT,                      -- Max 80 chars
    avatar_key        VARCHAR(255),
    is_verified       BOOLEAN DEFAULT FALSE,
    is_top_creator    BOOLEAN DEFAULT FALSE,     -- Drives fan-out strategy
    follower_count    BIGINT DEFAULT 0,
    following_count   BIGINT DEFAULT 0,
    video_count       INT DEFAULT 0,
    total_likes       BIGINT DEFAULT 0,
    region            VARCHAR(10),               -- ISO 3166-1; drives content restrictions
    privacy_mode      ENUM('public','private') DEFAULT 'public',
    created_at        DATETIME DEFAULT NOW(),
    INDEX idx_username (username)
);

-- ============================================================
-- Follows (MySQL sharded by follower_id)
-- ============================================================
CREATE TABLE follows (
    follower_id   BIGINT NOT NULL,
    followee_id   BIGINT NOT NULL,
    followed_at   DATETIME DEFAULT NOW(),
    PRIMARY KEY (follower_id, followee_id),
    INDEX idx_followee (followee_id, followed_at DESC)
);

-- ============================================================
-- Likes (TiKV — high write rate, simple key-value)
-- Key: (video_id, user_id) → liked_at
-- Also maintained as Redis HLL per video for fast count
-- ============================================================
CREATE TABLE likes (
    video_id    BIGINT NOT NULL,
    user_id     BIGINT NOT NULL,
    liked_at    DATETIME DEFAULT NOW(),
    PRIMARY KEY (video_id, user_id)
);

-- ============================================================
-- Comments (TiKV / MySQL sharded by video_id)
-- ============================================================
CREATE TABLE comments (
    comment_id        BIGINT PRIMARY KEY,
    video_id          BIGINT NOT NULL,
    author_id         BIGINT NOT NULL,
    body              TEXT,                      -- Max 150 chars
    parent_comment_id BIGINT,
    like_count        INT DEFAULT 0,
    is_deleted        BOOLEAN DEFAULT FALSE,
    created_at        DATETIME DEFAULT NOW(),
    INDEX idx_video (video_id, created_at DESC)
);

-- ============================================================
-- Interaction Events (ClickHouse — append-only analytics)
-- ============================================================
CREATE TABLE interaction_events (
    event_id      UUID DEFAULT generateUUIDv4(),
    user_id       BIGINT,
    video_id      BIGINT,
    event_type    LowCardinality(String),   -- 'play', 'pause', 'skip', 'like',
                                             -- 'comment', 'share', 'replay', 'save',
                                             -- 'follow_from_video', 'profile_click'
    watch_pct     Float32,                  -- 0.0–1.0; for play events
    source        LowCardinality(String),   -- 'fyp', 'following', 'search', 'trending'
    session_id    UUID,
    device_type   LowCardinality(String),
    network_type  LowCardinality(String),
    region        LowCardinality(String),
    ts            DateTime64(3)             -- Millisecond precision
) ENGINE = MergeTree()
  PARTITION BY toYYYYMM(ts)
  ORDER BY (video_id, user_id, ts)
  TTL ts + INTERVAL 365 DAY;              -- 1 year retention for ML training

-- ============================================================
-- FYP Candidate Cache (Redis — logical schema)
-- Key:   "fyp:{user_id}"
-- Type:  Sorted Set (score = recommendation_score, member = video_id)
-- Max:   200 candidates per user; refreshed every 30 minutes (or on exhaustion)
-- TTL:   2 hours
-- ============================================================

-- ============================================================
-- Following Feed Cache (Redis — logical schema)
-- Key:   "following:{user_id}"
-- Type:  Sorted Set (score = video_id Snowflake = upload time, member = video_id)
-- Max:   500 entries per user; ZREMRANGEBYRANK on overflow
-- TTL:   3 days
-- ============================================================

-- ============================================================
-- User Embedding Store (FAISS + Redis)
-- Offline: 128-dim user interest embeddings, updated every 6 hours
-- Online: delta updates on every 5 interactions (lightweight fine-tune)
-- ============================================================
```

### Database Choice

| Database | Use Case | Pros | Cons |
|----------|----------|------|------|
| TiKV (RocksDB + Raft) | Video metadata, likes | High write throughput (LSM-tree); distributed with strong consistency via Raft; horizontal scaling; ByteDance operates TiKV at scale | Operational complexity; higher latency than eventual-consistent stores |
| MySQL / TiDB | Users, follows | ACID for account management; TiDB offers MySQL compatibility + horizontal scaling | Complex sharding for follows at scale |
| Redis Cluster | FYP candidate cache, following feed | Sub-millisecond reads; Sorted Set for ranked candidate lists; perfectly sized for pre-computed 200-candidate lists | Memory-bound |
| ClickHouse | Interaction event store | Columnar compression (10:1 on interaction events); sub-second OLAP queries on billions of events; native Kafka ingest | Not for transactional workloads; schema changes are harder |
| S3 / Object Storage | Video segments | Unlimited scale; native CDN integration; 11 nines durability | Not queryable; high cost at PB scale → tiering needed |
| FAISS (vector index) | User/video embedding similarity search | ANN search in milliseconds over billion-scale embedding spaces; GPU acceleration | In-memory only; must be periodically rebuilt; not persistent source of truth |

**Decision:** TiKV chosen for video metadata because: (1) TiKV's Raft-based strong consistency ensures that once a video is marked "live", all reads agree — prevents creators from seeing a video go live then disappear; (2) LSM-tree handles TikTok's write-heavy workload (50M video uploads/day, 10B likes/day) without B-tree write amplification; (3) ByteDance created TiKV and runs it at their full scale. ClickHouse for interaction events because: TikTok's recommendation engine requires sub-second analytical queries on billions of daily events — traditional OLAP (Hive/Spark) would be too slow for real-time feature computation.

---

## 5. API Design

```
GET /v1/fyp
  Description: Get For You Page video recommendations
  Auth: Bearer token (required for personalization; guest supported with device_id)
  Query params:
    cursor: string (opaque; encodes scroll position + seen video set)
    count: int (default 10, max 20)
    device_type: "mobile"|"tablet"|"desktop"
    network_type: "wifi"|"4g"|"3g"|"2g"   -- Affects max video quality returned
  Response 200: {
    videos: [{
      video_id: string,
      author: {
        user_id, username, display_name, avatar_url, is_verified,
        is_following: bool, follower_count: int
      },
      caption: string,
      hashtags: string[],
      sound: { sound_id, title, artist_name, audio_url, is_licensed: bool },
      stats: { view_count, like_count, comment_count, share_count },
      is_liked: bool,
      is_saved: bool,
      thumbnail_url: string,
      video_manifest_url: string,     -- HLS master playlist URL (CDN)
      duration_s: float,
      content_quality: string,        -- "max_1080p"|"max_720p"|"max_360p"
      ad_break_positions: []          -- Empty for organic content
    }],
    next_cursor: string,
    session_id: string
  }

GET /v1/feed/following
  Description: Get Following feed (creators the user follows)
  Auth: Bearer token (required)
  Query params: cursor, count
  Response 200: { videos: Video[], next_cursor: string | null }

POST /v1/videos/upload/initiate
  Description: Start a video upload (returns pre-signed S3 URL)
  Auth: Bearer token (required)
  Rate limit: 10 video uploads/day per creator; 3 per hour
  Request: {
    file_size_bytes: int,
    duration_s: float,
    media_type: "video/mp4"|"video/quicktime"|"video/webm"
  }
  Response 200: {
    video_id: string,
    upload_url: string,             -- Pre-signed S3 URL, 30-min expiry
    upload_id: string,              -- For multipart upload tracking
    expires_at: ISO8601
  }

POST /v1/videos/{video_id}/publish
  Description: Finalize video after S3 upload and start processing
  Auth: Bearer token (required — must be upload owner)
  Request: {
    caption: string (max 2200 chars),
    hashtags: string[],
    sound_id: string | null,
    cover_time_s: float,            -- Timestamp for auto-generated thumbnail
    allow_duet: bool,
    allow_stitch: bool,
    visibility: "public"|"friends"|"private",
    schedule_at: ISO8601 | null     -- For scheduled posts
  }
  Response 202: {
    video_id: string,
    processing_status: "processing",
    estimated_ready_at: ISO8601
  }

GET /v1/videos/{video_id}/status
  Description: Poll processing status
  Auth: Bearer token (required — creator only)
  Response 200: { video_id, processing_status: "processing"|"live"|"failed", 
                  live_at: ISO8601 | null }

GET /v1/videos/{video_id}
  Description: Get a single video's details
  Auth: Optional
  Response 200: { video: VideoObject }

POST /v1/videos/{video_id}/like
  Description: Like a video (idempotent)
  Auth: Bearer token (required)
  Response 200: { liked: true, like_count: int }

DELETE /v1/videos/{video_id}/like
  Description: Unlike
  Auth: Bearer token (required)
  Response 200: { liked: false, like_count: int }

GET /v1/videos/{video_id}/comments
  Description: Get comments
  Auth: Optional
  Query params: cursor, count (default 20), sort: "top"|"newest"
  Response 200: { comments: Comment[], next_cursor: string | null }

POST /v1/videos/{video_id}/comments
  Description: Add a comment
  Auth: Bearer token (required)
  Rate limit: 100 comments/hour
  Request: { body: string (max 150 chars), parent_comment_id: string | null }
  Response 201: { comment_id, body, created_at }

POST /v1/interactions
  Description: Batch report interaction events (plays, skips, watches)
  Auth: Bearer token (required)
  Note: Client batches events and sends every 10 events or 30 seconds
  Request: {
    session_id: string,
    events: [{
      video_id: string,
      event_type: "play"|"pause"|"skip"|"replay"|"complete"|"swipe_away",
      watch_pct: float,         -- 0.0-1.0
      watch_duration_ms: int,
      source: "fyp"|"following"|"search",
      ts: ISO8601
    }]
  }
  Response 202: { events_received: int }

GET /v1/trending
  Description: Get trending hashtags, sounds, videos
  Auth: Optional
  Query params:
    region: string (ISO 3166-1)
    category: "hashtags"|"sounds"|"videos"|"creators"
    count: int (default 20)
  Response 200: {
    hashtags: [{ tag, video_count, view_count }],
    sounds: [{ sound_id, title, artist, usage_count, cover_url }],
    videos: Video[]
  }
  Cache-Control: max-age=60
```

---

## 6. Deep Dive: Core Components

### Component: For You Page (FYP) Recommendation Algorithm

**Problem it solves:**
TikTok's FYP is its defining competitive advantage. Unlike Instagram (social graph-based) or YouTube (search + subscription-based), TikTok's FYP serves highly relevant content to users who follow zero accounts, based purely on behavioral signals. The algorithm must: (1) cold-start new users into personalization within 3–5 interactions; (2) balance exploration (new interests) and exploitation (known preferences); (3) handle 700M DAU × 10 video requests each = 7B recommendation decisions per day; (4) avoid filter bubbles while maintaining high engagement.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Collaborative filtering | Users similar to you liked these videos | High quality at scale | Cold start; sparse matrix; doesn't capture video features |
| Content-based filtering | Videos similar to videos you engaged with | Works without social graph; captures video features | Filter bubble; limited discovery |
| Matrix factorization | Decompose user-video interaction matrix | Captures latent factors | Static model; doesn't handle real-time signals well |
| Two-tower neural model | User embedding + video embedding → retrieval | Handles cold start via content embeddings; fast ANN retrieval | Requires large training data; offline embedding freshness |
| Real-time multi-task model | Jointly predict: like, comment, share, watch%, replay | Captures nuanced engagement; rich signal | Complex architecture; inference latency |

**Selected Approach: Three-stage cascade with two-tower retrieval + multi-task ranking**

TikTok's published research (ByteDance's 2021 paper "Monolith: Real-Time Recommendation System With Collisionless Embedding Table") and engineering blog posts describe a multi-stage pipeline matching this architecture.

**Stage 1: Candidate Retrieval (Two-Tower ANN)**

```
User Tower:                    Video Tower:
User ID embedding (64d)        Video ID embedding (64d)
+ Recent interaction sequence  + Visual embedding (128d, from ResNet)
+ Demographics                 + Audio embedding (64d, from VGGish)
+ Device/context features      + Text embedding (caption/hashtags, 32d)
                               + Engagement stats (view_rate, like_rate)
         ↓                              ↓
    User embedding (128d)        Video embedding (128d)
              ↘                ↙
           Dot product similarity
           → Top-K candidates via FAISS
```

**Stage 2: Multi-Task Ranking Model**

```python
class FYPRankingModel(nn.Module):
    """
    Input features:
    - User features: embedding, demographics, session context
    - Video features: embedding, duration, quality signals
    - Context: time of day, device, network
    - Cross-features: user-video affinity, user-creator interaction history

    Output heads (multi-task learning):
    - p_watch_complete: probability of watching > 80% of video
    - p_like: probability of liking
    - p_comment: probability of commenting
    - p_share: probability of sharing
    - p_follow: probability of following creator
    - p_negative: probability of 'Not Interested' signal
    """
    def __init__(self):
        self.shared_bottom = DNN(input_dim=512, layers=[256, 128])
        self.watch_head = nn.Linear(128, 1)
        self.like_head = nn.Linear(128, 1)
        self.comment_head = nn.Linear(128, 1)
        self.share_head = nn.Linear(128, 1)
        self.follow_head = nn.Linear(128, 1)
        self.negative_head = nn.Linear(128, 1)

    def forward(self, features):
        shared = self.shared_bottom(features)
        return {
            'watch_complete': torch.sigmoid(self.watch_head(shared)),
            'like': torch.sigmoid(self.like_head(shared)),
            'comment': torch.sigmoid(self.comment_head(shared)),
            'share': torch.sigmoid(self.share_head(shared)),
            'follow': torch.sigmoid(self.follow_head(shared)),
            'negative': torch.sigmoid(self.negative_head(shared))
        }

def compute_final_score(predictions):
    """
    Weight predictions by business value:
    - watch_complete: 3.0  (most important signal; indicates content matched interest)
    - like: 1.5           (explicit positive signal)
    - comment: 2.0        (high intent signal)
    - share: 2.5          (distribution + endorsement)
    - follow: 4.0         (long-term engagement commitment)
    - negative: -10.0     (strong negative signal; penalize heavily)
    """
    return (
        3.0 * predictions['watch_complete'] +
        1.5 * predictions['like'] +
        2.0 * predictions['comment'] +
        2.5 * predictions['share'] +
        4.0 * predictions['follow'] -
        10.0 * predictions['negative']
    )
```

**Stage 3: Policy Filters + Diversity Injection**

```python
def apply_policy_and_diversity(ranked_candidates, viewer_id):
    final = []
    creator_counts = defaultdict(int)
    category_counts = defaultdict(int)
    consecutive_same_creator = 0
    prev_creator = None

    for video_id, score in ranked_candidates:
        video = get_video(video_id)
        creator_id = video.creator_id

        # Policy filter 1: no more than 3 videos from same creator in 20 slots
        if creator_counts[creator_id] >= 3:
            continue

        # Policy filter 2: no consecutive same creator
        if creator_id == prev_creator:
            continue

        # Policy filter 3: content restriction check
        if viewer_id.region in video.region_restrictions:
            continue

        # Exploration: inject 1 "serendipity" video per 10 slots
        if len(final) % 10 == 9:
            serendipity_video = get_serendipity_candidate(viewer_id, seen_categories=category_counts)
            final.append(serendipity_video)

        final.append((video_id, score))
        creator_counts[creator_id] += 1
        category_counts[video.category] += 1
        prev_creator = creator_id

        if len(final) >= 10:
            break

    return final
```

**FYP Cold Start:**

For a brand new user (0 interactions), the FYP uses:
1. **Onboarding signals**: User selects 3+ interest categories during signup → initial user embedding = average of category centroids.
2. **Device locale**: Region + language → pre-computed popular videos for that demographic.
3. **Rapid feedback loop**: After each interaction (like/skip/complete), the user's embedding is updated via a fast online learning step (lightweight gradient update with learning rate 0.1).
4. **Exploration bias**: First 50 interactions use ε = 0.3 exploration rate (30% of videos are randomly sampled from high-quality pool, not from embedding similarity).
5. After 30 interactions, the user has enough signal for the full two-tower model to be effective.

**Interviewer Q&As:**

Q: TikTok is accused of creating addictive filter bubbles. How does the algorithm balance engagement vs. content diversity?
A: Explicit diversity mechanisms: (1) Topic category cap: max 3 videos from same broad category (e.g., "cooking") per 10 FYP items — enforced post-ranking. (2) Serendipity injection: 1 in every 10 videos is randomly selected from a "high-quality diverse" pool of content outside the user's top interest clusters — measured by category embedding distance. (3) "Break the loop" signal: if a user exhibits "consumption without engagement" (watches but never likes/comments/follows for > 20 consecutive videos), the algorithm temporarily increases exploration rate to ε = 0.5 for that session. (4) Regulatory compliance: in some regions, TikTok has published that they cap the number of consecutively similar videos (Germany: "diverse feed" regulatory requirement).

Q: How does the recommendation engine handle the "cold start" for a brand new video?
A: New videos have no interaction history, so collaborative filtering cannot rank them. Strategy: (1) **Content-based bootstrap**: video embedding (visual + audio + text) is computed during processing and immediately available for retrieval via FAISS. The video enters the candidate pool for users whose user embedding is similar to the video embedding. (2) **Exploration pool**: all new videos from verified/active creators enter a "warm-up" pool. The FYP injects 1 video from the warm-up pool per 20 FYP positions. (3) **Watch percentage feedback**: after the video's first 1,000 views, its avg_watch_pct and like_rate are computed. Videos with avg_watch_pct > 0.7 are "graduated" to the main pool with higher ranking scores. (4) **Creator trust score**: new videos from creators with high historical engagement inherit a partial trust score, boosting their initial ranking.

Q: How do you prevent the FYP from recommending videos from creators who paid to be featured?
A: Paid promotion is a separate advertising system (not the organic FYP algorithm). Separation: (1) Paid content is tagged with `is_paid: true` in the video object. (2) FYP ranking model explicitly excludes paid content from organic ranking (it's injected by the Ad Server at specific interleaved positions — every 5th post in some markets). (3) Organic ranking scores are never influenced by payment — the ranking model only sees organic engagement signals. (4) Internal audit: engineering and policy teams independently audit the ranking model's features to verify no payment-correlated features are included. (5) Disclosure: paid content must show "Sponsored" label per regulatory requirements (FTC in US, ASA in UK).

Q: What signals does TikTok use that YouTube Recommendations does not?
A: TikTok-specific signals: (1) **Replay rate**: a user replaying a 30-second video 3 times is a uniquely strong TikTok signal (almost impossible on YouTube's longer content). (2) **Swipe-away timing**: if a user swipes away 2 seconds into a video, the algorithm learns the video type/creator combination is a strong negative signal for this user. (3) **Sound/audio engagement**: sharing a video "for the sound" (to create a duet) vs. "for the content" — different creator intent signals. (4) **Comment sentiment**: real-time NLP analysis of comment sentiment feeds back into ranking faster than YouTube's longer-session signals. (5) **Follow-from-FYP rate**: the rate at which FYP viewers become followers for a creator — captures "creator discovery value," unique to TikTok's non-social-graph model.

Q: How do you retrain the ranking model with real-time feedback from 700M daily users?
A: Two training pipelines: (1) **Offline batch retraining** (daily): Previous day's 10B+ interaction events from ClickHouse exported to a training cluster. Full two-tower model retrained with fresh data. Model evaluated on held-out date (yesterday's data used as validation to prevent data leakage). Promoted after A/B test metrics pass guardrails. (2) **Online streaming updates** (sub-second): Item-side features (engagement velocity, like rate) updated in real-time in the ClickHouse feature store. The ranking model reads these fresh features at inference time without retraining. (3) **Monolith architecture**: ByteDance's Monolith system uses a "collisionless embedding table" that supports online parameter updates — user/video embedding dimensions are updated incrementally as new interactions arrive, without full model retraining. This is the most sophisticated element: enabling near-real-time personalization without hourly model redeployments.

---

### Component: Video Processing Pipeline

**Problem it solves:**
50 million videos uploaded per day. Each raw video must be: (1) validated and safety-scanned; (2) transcoded to HLS/DASH at 5 quality levels (240p/360p/480p/720p/1080p); (3) thumbnail extracted; (4) visual/audio embeddings extracted for recommendation; (5) audio fingerprinted for sound matching (when users remix a sound); (6) uploaded to CDN origin. All of this must complete within 30 seconds for standard videos.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Single monolithic transcoder | One server does all processing steps sequentially | Simple | Single point of failure; no parallelism; 30s SLA impossible for complex videos |
| Microservice pipeline per step | Each processing step is a separate service | Independent scaling per step; failure isolation | Orchestration complexity; data handoff overhead between steps |
| DAG-based workflow engine | Processing steps as a directed acyclic graph (Airflow/Temporal) | Retry individual failed steps; visibility into state; parallelism | Orchestration overhead adds latency |
| Serverless per-segment | Lambda/Cloud Functions for each HLS segment | Auto-scales to 0; cost-effective | Cold start latency; Lambda 15-min limit blocks long videos |

**Selected Approach: Kafka-triggered microservice pipeline with parallel transcoding**

```
                    S3 Staging Bucket (raw upload)
                              |
               Kafka: video.uploaded event
                              |
                    ┌─────────▼────────┐
                    │ Video Validator  │ (Format check, duration check, size check)
                    └─────────┬────────┘
                              │ video.validated
                   ┌──────────┼──────────────────┐
                   │          │                  │
          ┌────────▼──┐  ┌────▼───────┐  ┌──────▼──────────────┐
          │Transcoder │  │ Thumbnail  │  │  Feature Extractor   │
          │(GPU)      │  │ Generator  │  │  - Visual embedding   │
          │5 quality  │  │            │  │  - Audio fingerprint  │
          │variants   │  │            │  │  - Content classifier │
          │→ HLS segs │  │            │  │  - Safety scan        │
          └────────┬──┘  └────┬───────┘  └──────┬──────────────┘
                   │          │                  │
                   └──────────┴──────────────────┘
                              │ All completed → video.processed
                    ┌─────────▼──────────┐
                    │  CDN Uploader      │ Upload all segments + manifests to CDN origin
                    └─────────┬──────────┘
                              │
                    ┌─────────▼──────────┐
                    │  Metadata Updater  │ Update TiKV: status='live', set all keys
                    └─────────┬──────────┘
                              │
                    ┌─────────▼──────────┐
                    │  Recommendation    │ Index video embedding in FAISS
                    │  Indexer           │ Add to creator's follower feed caches
                    └────────────────────┘
```

**Implementation Detail:**

```python
# Transcoding step (GPU-accelerated FFmpeg)
def transcode_video(video_id, raw_s3_key):
    raw_bytes = s3.get_object(STAGING_BUCKET, raw_s3_key)

    quality_ladder = [
        {'resolution': '240x426',  'bitrate': '400k',  'suffix': '240p'},
        {'resolution': '360x640',  'bitrate': '800k',  'suffix': '360p'},
        {'resolution': '480x854',  'bitrate': '1500k', 'suffix': '480p'},
        {'resolution': '720x1280', 'bitrate': '3000k', 'suffix': '720p'},
        {'resolution': '1080x1920','bitrate': '6000k', 'suffix': '1080p'},
    ]

    hls_manifests = {}
    for quality in quality_ladder:
        # Use FFmpeg with libx264 (H.264) or libx265 (H.265) encoder
        # HLS: 2-second segments for smooth adaptive bitrate switching
        output_key = f"videos/{video_id}/{quality['suffix']}/"
        manifest_key = transcode_to_hls(
            input=raw_bytes,
            resolution=quality['resolution'],
            bitrate=quality['bitrate'],
            segment_duration_s=2,
            output_s3_prefix=f"{S3_PROD_BUCKET}/{output_key}"
        )
        hls_manifests[quality['suffix']] = manifest_key

    # Generate master playlist (adaptive bitrate manifest)
    master_manifest = generate_hls_master_playlist(hls_manifests)
    master_key = f"videos/{video_id}/master.m3u8"
    s3.put_object(S3_PROD_BUCKET, master_key, master_manifest,
                  ContentType='application/x-mpegURL',
                  CacheControl='max-age=3600')

    return master_key, hls_manifests

# Feature extraction (runs in parallel with transcoding)
def extract_features(video_id, raw_s3_key):
    # Visual embedding: sample 1 frame per second, run ResNet-50 → pool → 128d vector
    frames = extract_frames(raw_s3_key, fps=1)
    frame_embeddings = resnet_model.predict(frames)        # GPU batch inference
    video_embedding = np.mean(frame_embeddings, axis=0)    # Temporal average pooling

    # Audio fingerprint: VGGish model on mel-spectrogram
    audio = extract_audio(raw_s3_key)
    audio_embedding = vggish_model.predict(audio)          # 128d audio embedding
    audio_fingerprint = hash_audio(audio)                  # SHA-256 for sound matching

    # Content safety: NSFW/violence classifier on video frames
    safety_scores = safety_model.predict(frames)           # GPU batch
    is_safe = all(s < SAFETY_THRESHOLD for s in safety_scores)

    return {
        'visual_embedding': video_embedding,
        'audio_embedding': audio_embedding,
        'audio_fingerprint': audio_fingerprint,
        'is_safe': is_safe
    }
```

**Interviewer Q&As:**

Q: How does TikTok handle a 10-minute video (maximum length) within a 30-second processing SLA?
A: 10-minute videos at high resolution (1080p) require significant transcoding time — potentially 5–10 minutes on a single GPU. Solutions: (1) **Parallel segment transcoding**: divide the video into 10 1-minute chunks (using `ffmpeg -ss` seek). Transcode each chunk independently in parallel across 10 GPU workers. Reassemble HLS segments in order. This reduces transcode time from 10 minutes to ~1 minute. (2) **Priority queuing**: short videos (<60s) get priority queue; long videos go to a lower-priority queue with a longer SLA (5 minutes acceptable for creators). (3) **Progressive availability**: while full transcoding is running, the first 60 seconds of HLS segments are uploaded to CDN first, allowing the video to go "partially live" (playable but truncated) within 30 seconds, with remaining segments becoming available as transcoding completes.

Q: How do you ensure safety classification keeps up with 50M uploads/day?
A: Safety classification workload: 50M videos × 60 frames/video = 3B frame classifications/day. On a V100 GPU, ResNet-50 processes ~500 frames/second. To handle 3B frames/day: 3B / (500 × 86400) = ~69 GPUs. Add 3× headroom for peak = ~210 GPU instances. Safety classification runs as a separate microservice (not on the same GPU as transcoding). Fast-path: if frame 1 is flagged with high confidence (>0.99), reject immediately without processing remaining frames. Tiered classification: cheap binary classifier (safe/unsafe) runs first on all frames; expensive multi-label classifier only on flagged content.

Q: How do you handle the DMCA copyright issue for audio fingerprinting?
A: When a video is uploaded, the audio is fingerprinted using a perceptual audio matching algorithm (similar to AudD or ACRCloud). The fingerprint is compared against a database of licensed audio tracks (provided by music labels). If a match is found: (1) Check if the creator has licensing rights (via linked content agreement). (2) If not, apply one of: block audio track and replace with silence; block video in specific countries; allow with ad revenue shared with rights holder; fully block globally. The audio fingerprint database is updated in real-time as new songs are released. False positive rate of audio matching must be extremely low — wrongly muting a creator's original audio would be a serious product failure.

---

### Component: Global Content Distribution (CDN Strategy)

**Problem it solves:**
TikTok serves 5 billion video views per day across 150+ countries. Video delivery is the most bandwidth-intensive part of the system: 5B views × 30 MB avg = 150 PB/day of video delivery. The CDN must: (1) serve videos with < 100ms start latency for 95th percentile of global users; (2) handle massive popularity spikes for viral videos; (3) manage costs at petabyte-per-day scale; (4) implement geographic content restrictions without leakage.

**CDN Architecture:**

```
Creator uploads → S3 Origin (us-east-1 primary + 4 replicas)
                     ↓
              CDN Edge (1,000+ PoPs globally)
                     ↓
              Client device (pre-buffer next 3 videos)

Key optimizations:
1. Pre-warm: viral videos predicted to trend are proactively pushed to all major PoPs
2. Predictive prefetch: client pre-loads the next 1-2 FYP videos' HLS manifests
3. ABR: adaptive bitrate (240p→1080p) based on measured bandwidth
4. QUIC: TikTok uses QUIC/HTTP3 for video delivery (reduces connection setup time,
         especially on mobile with packet loss — critical for 3G markets)
```

**Implementation Detail — Predictive Pre-caching:**

```python
# Pre-warm CDN for predicted viral videos (runs every 5 minutes)
def pre_warm_cdn_for_viral_candidates():
    # Query ClickHouse for videos with high engagement velocity in last 1h
    candidates = clickhouse.query("""
        SELECT video_id, COUNT(*) as plays_1h,
               AVG(watch_pct) as avg_watch_1h,
               COUNT(DISTINCT region) as geographic_spread
        FROM interaction_events
        WHERE ts > NOW() - INTERVAL 1 HOUR
          AND event_type = 'play'
        GROUP BY video_id
        HAVING plays_1h > 100000          -- 100K plays in 1h
           AND avg_watch_1h > 0.7        -- High completion
           AND geographic_spread > 3     -- Global, not regional
        ORDER BY plays_1h DESC
        LIMIT 1000
    """)

    # Get the HLS manifest and top 3 quality variants
    for video in candidates:
        video_meta = tikov.get(f"video:{video.video_id}")
        keys_to_warm = [
            video_meta.hls_manifest_key,        # Master manifest
            video_meta.qualities_keys['360p'],   # Most common quality
            video_meta.qualities_keys['720p'],   # High-quality
        ]
        # Get all HLS segments for first 30 seconds (15 segments at 2s each)
        first_30s_segments = get_hls_segments(video_meta.hls_manifest_key,
                                               up_to_seconds=30)
        keys_to_warm.extend(first_30s_segments)

        # Push to CDN via API (Cloudflare Cache Reserve API or Akamai Fast Purge)
        cdn.warm_cache(
            keys=keys_to_warm,
            regions=['us', 'eu', 'apac'],  # Warm based on geographic_spread
            priority='high'
        )

# Client-side adaptive prefetch (pseudocode in Swift/Kotlin)
class FYPPlayer:
    def on_video_30_percent_watched(self, current_position):
        # Prefetch next video's manifest and first 3 seconds
        next_video_id = self.fyp_queue[current_position + 1]
        self.prefetch_video_start(next_video_id, seconds=3)

    def prefetch_video_start(self, video_id, seconds):
        # Fetch HLS manifest from CDN (likely cached → < 50ms)
        manifest = cdn.get(f"{CDN_BASE}/{video_id}/master.m3u8")
        # Select quality based on current measured bandwidth
        quality = self.select_quality(manifest, self.measured_bandwidth_kbps)
        # Download first N seconds of HLS segments
        segments = parse_segments_for_duration(manifest, quality, seconds)
        for segment in segments:
            self.download_queue.add(cdn.get(segment.url))
```

**Interviewer Q&As:**

Q: How does TikTok manage content geo-restrictions (e.g., a video blocked in Russia but available everywhere else)?
A: Region restrictions stored in the video metadata (`region_restrictions` JSON field in TiKV). Two enforcement points: (1) **CDN-level**: CDN edge nodes in restricted regions return 403 for restricted video manifest URLs. CDN rules configured per video via API (VideoID → blocked_regions list). Propagation latency: < 60 seconds for restriction to take effect at CDN. (2) **API-level**: FYP Service filters out region-restricted videos from recommendation candidates based on the viewer's region (from auth token, with IP geolocation as fallback). API-level filtering is the primary safety layer; CDN restriction is defense-in-depth. The dual enforcement ensures: even if a user somehow obtains the direct CDN URL (e.g., via developer tools), the CDN blocks delivery in the restricted region.

Q: TikTok's CDN delivers 150 PB/day. What does that cost, and how do you optimize?
A: At commercial CDN rates (~$0.01/GB for high-volume), 150 PB/day = $1.5M/day = $540M/year in CDN costs alone. Actual cost is lower due to: (1) TikTok operates its own CDN infrastructure for major markets (similar to Netflix's Open Connect), dramatically reducing cost per GB. (2) Video compression: H.265/HEVC encoding reduces file size by ~40% vs. H.264 at same quality. TikTok serves H.265 to clients that support it (most post-2017 iOS/Android). (3) Content popularity power law: top 1% of videos account for ~80% of views. These viral videos have >99% cache hit rate. The remaining 99% of videos (long tail) are served infrequently — for these, it's acceptable to serve from a fewer number of regional origin caches rather than all 1,000+ PoPs. (4) Bit rate adaptation: serving 360p to users on 3G networks instead of 1080p reduces bandwidth by 7×.

Q: How does pre-buffering work for TikTok's swipe-based UX without wasting bandwidth?
A: The core tension: pre-buffer the next video for instant playback, but don't waste bandwidth if the user never swipes to it. TikTok's approach (based on engineering blog posts and reverse-engineered behavior): (1) After a video starts, prefetch the next video's manifest + first 3 seconds (≈ 200-600 KB depending on quality). This is the "safe" prefetch — very low waste if the user stops scrolling. (2) When current video reaches 70% watch time, prefetch the full first 15 seconds of the next video. At this point, there's high probability the user will complete the current video and swipe. (3) The client maintains a prefetch queue of 2 videos ahead. (4) When the user actually swipes, the next video starts from the prefetched buffer — perceived latency ≈ 0ms. Bandwidth waste analysis: if the user swipes before 70% watch time, only 200-600KB is wasted. At 700M DAU × 29 pages × 200KB = ~4 TB/day of prefetch waste — acceptable vs. the UX benefit.

---

## 7. Scaling

### Horizontal Scaling

**FYP Service**: Stateless; scales horizontally. At 232K read QPS, with each instance handling ~8K QPS (Redis reads are batch-efficient), need ~30 instances average. Each FYP page generation: 1 Redis read (candidate list) + 1 ClickHouse query (real-time features for re-ranking) + 1 ONNX model inference. Bottleneck is ClickHouse query — optimize with pre-aggregated feature tables.

**Upload Service**: 580 QPS average. Handles only metadata (pre-signed S3 URL). 5 instances sufficient for normal load; 20 for peak.

**Video Processing Workers**: GPU-bound (transcoding) and CPU-bound (embedding extraction). Scale GPU instances based on Kafka consumer lag on `video.uploaded` topic. At 580 uploads/sec (peak) with avg 2-minute processing per video, need steady-state ~70 GPU workers (580 × 120s / (1000 GPU ops/worker/sec)).

**Following Feed Fan-out Workers**: At 580 writes/sec, avg 1M followers for top creators. With hybrid fanout (skip push for top creators), actual fan-out writes: 580 × 1000 (normal creator avg followers) = 580K Redis writes/sec. 50 worker instances at ~12K writes/sec each.

### Database Scaling

**Sharding:**

*TiKV (video metadata)*: TiKV natively distributes key-value pairs across nodes via Raft-based region splitting. Each TiKV "Region" is ~96MB. With 5 TB/day of video data (50M videos × 100 bytes metadata = 5GB/day metadata — video files are in S3), TiKV metadata is manageable: 50M videos/day × 2KB = 100 GB/day. 5-year total: ~180 TB metadata. With 60 TiKV nodes at 3 TB each = 180 TB capacity (RF=3 = 3× storage).

*MySQL (users)*: Shard by `user_id % N`. With 1.5B users × 5KB = 7.5 TB of user data. 100 MySQL shards at 75 GB each (including indices). Vitess or similar proxy layer.

*MySQL (follows)*: Shard by `follower_id`. The critical query "get all followers of creator X" scans the secondary index on `followee_id` — cross-shard scatter-gather. For top creators (>1M followers), cache the follower list in Redis: `top_creator_followers:{creator_id}` as a set, invalidated on new follows.

**Replication:**

*TiKV*: Raft-based 3-way replication within a region. Multi-region replication via TiKV's async cross-cluster replication. FYP reads use `ReadCommitted` isolation; video creation uses `Serializable` (ensures processing status updates are atomic).

*Kafka*: RF=3 per topic. Video processing topics partitioned by `video_id % partitions` for locality. Interaction event topic partitioned by `user_id % partitions` for temporal locality per user.

**Caching:**

- **Redis L1 (FYP candidates)**: 200 video_id candidates per user (128 bytes × 200 × 700M active = 17.9 TB → 300 Redis nodes at 60 GB each). Refreshed every 30 minutes or when candidate list is 80% consumed.
- **Redis L2 (Following feed)**: 500 video_ids per user for following feed. 700M active users × 500 × 8 bytes = 2.8 TB.
- **Redis (trending cache)**: Trending hashtags/sounds/videos per region. Flink updates every 60 seconds.
- **ClickHouse (materialized views)**: Pre-aggregate video engagement metrics (likes_1h, plays_1h, avg_watch_pct_24h) via ClickHouse materialized views. Queries against materialized views complete in < 10ms vs. scanning raw events.
- **CDN (video segments)**: 95%+ cache hit rate for popular videos. Long-tail videos (viewed < 100 times) may miss CDN and serve from S3 origin — acceptable since they're rarely requested.

**CDN:**

TikTok uses a multi-tier CDN: (1) Tier 1: Private CDN PoPs co-located with ISPs in top-50 metros (ultra-low latency — these cover 60% of global traffic). (2) Tier 2: Commercial CDN (Akamai, Cloudflare) for remaining geography. (3) Tier 3: S3 origin for cache misses. Videos use `Cache-Control: max-age=31536000, immutable` (content-addressed keys). HLS manifests: `Cache-Control: max-age=3600` (may change quality variants). Geographic restriction is enforced at the CDN edge via per-URL geo-fence rules.

### Interviewer Q&As on Scaling

Q: How does TikTok handle a video that goes viral instantly — from 0 to 100M views in 24 hours?
A: The viral video challenge has two phases: (1) **Early phase (0–1M views)**: The video is warming up. FYP algorithm detects high `avg_watch_pct` and early engagement velocity in ClickHouse. The pre-warm CDN job runs every 5 minutes and picks this up. Replicas are written to all tier-1 CDN PoPs. Initial traffic served from S3 (CDN misses) — this is the expensive phase ($0.09/GB from S3 vs. $0.01/GB from CDN). (2) **Viral phase (1M–100M views)**: CDN has the video cached globally. S3 origin receives only CDN cache refresh requests (every 24h per segment). The FYP algorithm serves this video more aggressively — it enters the "trending" pool and gets injected into more users' FYP pages. The FYP Service scales horizontally (more Redis reads = more instances needed) but the CDN absorbs the media bandwidth.

Q: How do you handle the FAISS index becoming stale as 50M new videos are added per day?
A: FAISS index staleness management: (1) Incremental video indexing: when a new video is processed and its embedding computed, it's added to a "new video buffer" Redis sorted set (scored by processing time). The FAISS index is rebuilt nightly (full rebuild from all video embeddings stored in a vector DB). Between rebuilds, the FYP retrieval step checks both FAISS (for historical videos) AND the Redis new video buffer (for videos < 24h old). (2) Nightly FAISS rebuild uses a batch job: read all active video embeddings from TiKV, build FAISS index (GPU-accelerated), write to S3 as a serialized FAISS index file. FYP Service workers load the new index atomically at midnight (blue-green swap). (3) Videos older than 6 months with < 10K lifetime views are pruned from the FAISS index (long-tail pruning) to keep index size manageable.

Q: How do you scale ClickHouse to handle 50 billion interaction events per day?
A: 50B events/day = 578K inserts/second. ClickHouse insertion rate: single node can handle ~500K-1M rows/second via batch insert. Design: (1) Kafka consumers batch 10,000 events before inserting to ClickHouse (reducing insert QPS from 578K to 58 — massive reduction in insert overhead). (2) ClickHouse cluster: 20 shards × 2 replicas = 40 nodes. Partitioning by `toYYYYMM(ts)` enables efficient partition pruning for time-range queries. (3) Materialized views: pre-aggregate `plays_1h`, `likes_24h`, `avg_watch_pct` as materialized views that are updated automatically on insert. FYP ranking reads these pre-aggregated views (sub-10ms) instead of scanning raw events. (4) 1-year retention with TTL: TTL automatically drops data older than 1 year, keeping storage manageable at ~1.8 PB (50B events × 365 days × 200 bytes = 3.65 PB raw; with ClickHouse's ~4:1 columnar compression = ~900 TB).

Q: How does TikTok's "green screen" and AR effects pipeline integrate with the upload/processing pipeline?
A: Effects (green screen, face filters, AR overlays) are applied client-side in real-time using the device's GPU (Core Image on iOS, RenderScript/Vulkan on Android). The uploaded video already has effects baked in — TikTok's server doesn't apply effects post-upload. The TikTok app includes a rendering engine (similar to Snap's Lens Studio) that runs effects at 30fps on-device during recording. This shifts compute cost to the client (free for TikTok servers). The exception: some "remix" effects (e.g., background removal AI) can be applied during upload client-side. The server only receives the final rendered video, which goes through the standard processing pipeline.

Q: What database/system would you use for the "Creator Analytics" dashboard showing real-time view counts?
A: Creator analytics require: (1) Real-time view counts visible to creator within seconds. (2) Demographic breakdowns (age, region, device) with hourly granularity. (3) Historical trends (28-day, 90-day). Implementation: (1) View events → Kafka → ClickHouse real-time table (ingestion latency: <5 seconds). (2) Creator dashboard queries ClickHouse materialized views for aggregate metrics. Query: `SELECT toStartOfHour(ts), COUNT(*) FROM views WHERE video_id IN (...) GROUP BY 1 ORDER BY 1 DESC LIMIT 28`. Response time: <500ms for a ClickHouse materialized view query. (3) Real-time counter: for the "live counter" on the creator's screen during viral moments, we use Redis `INCR video:views:{video_id}` (updated by view event Kafka consumer). This gives sub-second freshness for the headline number. Exact counts reconciled with ClickHouse hourly. (4) Demographic data (requires joining viewer profiles with view events) served from ClickHouse ad-hoc queries with 1-hour cache in Redis.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Mitigation |
|-----------|-------------|--------|------------|
| Upload Service crash | Video upload fails | Creator can't post | Stateless; LB reroutes; client retries with idempotency key |
| Video Processing Worker crash | Video stuck in "processing" | Creator's video delayed | Kafka replay from last offset; 30-minute timeout triggers requeue; separate dead-letter queue for stuck jobs |
| FYP Redis cluster node failure | Some users' FYP cache evicted | FYP falls back to real-time retrieval (slower) | Redis Cluster auto-failover; offline FYP recomputation job catches up |
| ClickHouse node failure | Feature queries fail | Ranking falls back to embedding similarity only; virality signals unavailable | ClickHouse RF=2; replica query fallback; circuit breaker in FYP Service |
| CDN PoP failure | Video unavailable in that region | Users in that region see loading errors | CDN anycast routes to next closest PoP automatically; QUIC connection migration handles mid-stream PoP switch |
| FAISS index corruption | ANN retrieval returns garbage | FYP degrades to random high-quality videos | FAISS index snapshotted to S3 every 6 hours; rollback to last known good; health check on index output diversity |
| Kafka partition leader failure | Fan-out events delayed | Following feed stale for up to 60s | Kafka RF=3; new leader elected <30s; no event loss; following feed SLA is 60s, within tolerance |
| TiKV region failure | Video metadata unavailable for affected range | Affected videos return 404 until recovery | Raft leader election <10s; no data loss with 3-way replication |

### Failover Strategy

- **Multi-region active-active**: TikTok operates in multiple cloud regions globally (AWS us-east-1, eu-west-1, ap-southeast-1, and ByteDance's own IDCs). User requests route to nearest region. TiKV and MySQL replicate cross-region async.
- **FYP degraded mode**: If recommendation system fully fails, serve a "popular in your region" feed (cached globally in Redis, updated every 5 minutes from trending data). This is acceptable for brief outages — users see popular content instead of personalized content.
- **Video delivery independent from recommendation**: CDN delivery of video content is independent of the FYP recommendation system. Even if FYP Service is down, previously loaded FYP pages (cached in client) continue playing. The client buffers 5 videos ahead — effectively a 5-video buffer against brief API outages.

### Retries & Idempotency

- **Video upload**: `X-Idempotency-Key: {uuid}` header on `POST /v1/videos/upload/initiate`. Same key returns same `video_id` and pre-signed URL within 30 minutes.
- **Video publish**: Idempotent — same `video_id` + same metadata = no duplicate. Publish is upsert to TiKV.
- **Like**: TiKV PUT `(video_id, user_id)` → idempotent. Like count in Redis uses `INCR` — atomic.
- **Interaction events**: Batched delivery; duplicate events (same event_id) are deduplicated in ClickHouse via `ReplacingMergeTree` engine on `event_id`.
- **Video Processing**: Kafka offset committed only after all outputs (segments in S3, TiKV updated, FAISS indexed) succeed. Re-processing same video is idempotent (S3 overwrites same keys with same content; TiKV upsert).

### Circuit Breaker

- **FYP recommendation**: Opens if ClickHouse query error rate > 40% over 10s. Fallback: skip real-time re-ranking; serve precomputed Redis candidate order.
- **FAISS retrieval**: Opens if > 30% of ANN queries timeout. Fallback: serve popular videos from trending pool; log incident.
- **Following fan-out**: Opens if Redis write error rate > 50%. Fan-out events buffered in Kafka dead-letter queue; retry after circuit resets. Following feed may be stale until recovered.
- **CDN pre-warm**: Opens on CDN API timeout. Non-critical; virality detection continues; pre-warming resumes when CDN API recovers.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Why It Matters |
|--------|------|-----------------|----------------|
| FYP API p99 latency | Histogram | > 200ms | Core recommendation SLA |
| First video play start latency p95 | Histogram | > 2s | #1 UX metric; direct retention correlation |
| Video processing p99 time | Histogram | > 60s | Creator experience SLA |
| CDN cache hit rate (by video tier) | Ratio | < 90% popular / < 50% long-tail | CDN efficiency and cost |
| FYP Redis cache hit rate | Ratio | < 85% | Indicates cold FYP generation happening too frequently |
| Kafka consumer lag (video.uploaded) | Gauge | > 10K messages | Processing backlog |
| Kafka consumer lag (interaction.event → ClickHouse) | Gauge | > 100K | Analytics freshness |
| ClickHouse query p99 | Histogram | > 100ms | Feature store bottleneck |
| FAISS query latency p99 | Histogram | > 30ms | ANN retrieval bottleneck |
| Average FYP watch completion rate | Ratio | Sudden drop > 15% | Algorithm quality regression |
| Upload error rate | Counter | > 0.5% 5xx | Creator experience |
| Viral detection lag | Gauge | > 10 minutes | CDN pre-warm timeliness |
| TiKV write latency p99 | Histogram | > 20ms | Storage layer health |
| Following feed freshness (time from post to follower feed) | Gauge | > 120s | Fan-out SLA |
| Video safety scan false positive rate | Ratio | > 0.1% | Content availability SLA for creators |

### Distributed Tracing

- OpenTelemetry across all services. Trace propagated via gRPC metadata.
- Critical path: `GET /v1/fyp` → FYP Service (Redis read, ClickHouse query, FAISS ANN, ONNX ranking) → TiKV video hydration → CDN manifest fetch.
- Video processing trace: Upload → S3 staging → Video Validator → Transcoder (per quality) → Feature Extractor → CDN Uploader → TiKV update. Long-running traces (30+ seconds for processing) require trace sampling and async span collection.
- Flame graph reveals: transcoding step accounts for 70% of processing time; optimizing GPU utilization yields highest processing latency improvement.

### Logging

- Structured JSON logs: `{timestamp, trace_id, service, user_id_hash, video_id, event, latency_ms, status, region}`.
- FYP generation log: `{user_id_hash, session_id, fyp_version, candidates_from_redis, candidates_from_trending, final_count, ranking_model_v, latency_ms}`.
- Video processing log: per-stage timing: `{video_id, stage: "transcode_360p", duration_ms, output_size_bytes, gpu_id}`.
- CDN pre-warm log: `{video_id, regions_warmed, segments_pushed, trigger_velocity_score}`.
- Safety scan log: `{video_id, classifier_version, score_nsfw, score_violence, action: "allow"|"flag"|"block"}` — immutable audit log, 3-year retention.
- Alert patterns: `"processing_status": "failed"` count > 100/hour → video processing incident; `"fyp_redis_miss": true` rate > 20% → FYP cache degradation.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|----------|---------------|-----------------|--------|
| FYP algorithm | Multi-task neural ranking (watch%, like, comment, share jointly) | Single engagement predictor | Single predictor optimizes for only one signal; joint prediction allows nuanced weighting of different engagement types; prevents "like bait" from dominating |
| Video candidate retrieval | FAISS two-tower ANN | Inverted index (hashtag/keyword matching) | Hashtag matching misses content without explicit keywords; two-tower captures semantic similarity (visually similar cooking videos recommended even without same hashtag) |
| Video processing | Parallel microservice DAG (per-stage) | Monolithic sequential processor | Sequential processor can't parallelize transcoding across quality levels; microservice DAG allows each quality level to transcode in parallel, meeting 30s SLA |
| Interaction event storage | ClickHouse (columnar, append-only) | MySQL/Postgres | MySQL cannot sustain 578K inserts/sec without massive infrastructure; ClickHouse columnar compression reduces storage 10×; OLAP queries for feature computation complete 100× faster |
| FYP cache | Redis Sorted Set (200 candidates) | No cache (real-time retrieval) | Real-time FAISS retrieval for 700M DAU × 10 FYP loads/day = 7B FAISS queries/day — FAISS CPU cost is too high; pre-computation at recommendation refresh time amortizes cost |
| Cold start | Rapid embedding update (online learning, 5 interactions) | Wait for batch retraining | Batch retraining (nightly) means new users see non-personalized feed for 24 hours; online learning personalizes within one session |
| Celebrity fan-out | Hybrid push-pull (top creators > 1M followers use pull) | Pure push | A top creator (50M followers) posting = 50M Redis writes; pure push would require 50M × 8 bytes = 400 MB of Redis writes per post — clearly infeasible |
| Video format | HLS with 2s segments | MP4 progressive download | Progressive download requires full video buffered before seeking; HLS allows adaptive bitrate switching mid-video and efficient random seek; critical for mobile network variability |
| Video embedding freshness | Nightly full FAISS rebuild + real-time Redis buffer for new videos | Incremental index updates | Incremental FAISS updates cause index fragmentation over time; nightly rebuild maintains optimal ANN performance; Redis buffer handles <24h freshness gap |
| Geo-restriction enforcement | Dual enforcement: CDN-level + API-level | API-level only | API-level only means direct CDN URL access bypasses restriction; dual enforcement is defense-in-depth for legal compliance |
| User interaction signals | 7 signal types (play, skip, like, comment, share, replay, follow-from-FYP) | Binary liked/not-liked | Binary signal is too coarse; replay is TikTok's unique high-quality signal; skip timing (swipe away at 2s vs 15s) gives nuanced negative signal that binary "no like" cannot provide |

---

## 11. Follow-up Interview Questions

**Q1: How would you implement TikTok's "Duet" feature?**
A: Duet creates a side-by-side video where a creator records alongside another creator's video. Implementation: (1) Original video must have `allow_duet: true`. (2) Creator records their side using TikTok's in-app recorder, which plays the original video's audio in earpiece while recording. (3) At upload: the Duet video is a new video entry in TiKV with `duet_of_video_id` field referencing the original. (4) In the processing pipeline: a Duet compositor service stitches the two videos side-by-side into a single HLS stream. The original video's audio track is used as the primary audio. (5) In the FYP: Duet videos carry the original video's hashtags and sound_id (for trending counts), plus their own creator's audience. (6) Chains of duets: `duet_of_video_id` allows traversal of duet chains — TikTok shows "X creators duetted this" as social proof.

**Q2: How would you design TikTok's "Stitch" feature?**
A: Stitch clips the first 1–5 seconds of another creator's video and prepends it to your own video. Similar to Duet but temporal, not spatial. Implementation: (1) At recording: the TikTok client plays the selected clip, records the creator's response, and concatenates the clips in the upload. (2) The Stitch is uploaded as a single video with `stitch_of_video_id` metadata. (3) Server-side: the video processing pipeline detects the Stitch composition (via metadata flag, not video analysis) and links the two videos. (4) In feed: "Responding to @creator" label is shown. (5) The stitched-from video's metadata is updated with `stitch_count` increment. (6) Permission check: at recording time, the TikTok client checks `allow_stitch` flag of the original video via an API call; the server enforces at publish time.

**Q3: How do you design the FYP for a user who opens TikTok for the first time with no account?**
A: Anonymous/guest FYP: (1) Client sends `device_id` (a UUID generated on first install, stored in Keychain/Keystore) instead of auth token. (2) FYP Service treats `device_id` as a user_id for a "guest profile." (3) Guest FYP starts with geolocation-based popular videos (most-watched in last 24h in the user's country). (4) Interaction events (plays, likes, skips) are associated with `device_id` in ClickHouse. (5) After 5 interactions, the FYP begins personalizing for the `device_id`. (6) When user creates an account, their `device_id` interaction history is migrated to their new `user_id`. This allows cold-start personalization even before account creation — TikTok's "hook you before signup" strategy.

**Q4: How would you implement TikTok Live (real-time video streaming)?**
A: Live streaming requires fundamentally different infrastructure. (1) Broadcaster ingests via RTMP to Edge Ingest Servers (co-located with CDN PoPs). (2) Ingest Server transcodes to LLHLS (Low-Latency HLS, 0.5s segments vs. standard 2s) for sub-3s live latency. (3) LLHLS segments pushed to CDN origin in real-time. (4) Viewers pull LLHLS from CDN: latency = ingest-to-CDN + CDN-to-viewer ≈ 3–5s end-to-end. (5) Live gifts: virtual currency system, separate from video pipeline. Gift events processed by a Gift Service: real-time effects rendered on screen, gift count accumulated in Redis, converted to creator earnings at end of stream. (6) Live comments: WebSocket connection from viewer clients to a Live Comment Service; comments fan-out to all concurrent viewers via pub/sub. (7) Scale: a top creator's live stream can have 1M+ concurrent viewers — CDN delivers the HLS stream to all, but the Live Comment Service needs to handle 1M WebSocket connections. This requires a dedicated WebSocket cluster with connection state in Redis.

**Q5: How does TikTok handle "trend lifecycle management" — a challenge starts, peaks, and dies?**
A: Trend lifecycle: (1) **Detection (rising)**: Flink detects hashtag count anomaly (current rate vs. 7-day rolling average > 3 sigma). Trend enters "rising" state. (2) **Amplification (viral)**: FYP algorithm increases injection rate of trending-hashtag videos. Discovery page highlights the trend with a banner. This creates a positive feedback loop — TikTok intentionally amplifies rising trends. (3) **Peak**: Trending page shows the hashtag with view count. FYP injection rate at maximum. (4) **Declining (saturation)**: When engagement rate per new video with this hashtag drops below a threshold (new videos getting fewer views than 7 days ago despite same hashtag), the trend enters "declining" state. FYP reduces injection rate. Discovery page removes banner. (5) **Archive**: After 30 days in declining state, hashtag moved to "archived trends" — no longer actively promoted but searchable. User behavior naturally follows this lifecycle; the system amplifies the natural curve rather than creating it.

**Q6: How would you implement parental controls for TikTok's "Family Pairing" feature?**
A: Family Pairing links a parent account to a child account. Implementation: (1) Parent generates a QR code containing a `pairing_token` (short-lived, signed JWT with parent_id). (2) Child app scans QR code, sends token to Pairing Service, which creates `family_pair (parent_id, child_id, created_at)` in MySQL. (3) Parent's app subscribes to child account controls via REST API: `PUT /v1/family/controls/{child_id}` with `{max_daily_screen_time_min, content_filter_level, dm_disabled, search_disabled}`. (4) Child account's FYP Service checks `family_controls:{child_id}` in Redis on each session start. Controls stored in Redis (TTL = 24h; re-fetched daily from MySQL). (5) Screen time enforcement: client-side timer with server-side session kill (API returns 403 "Daily limit reached" when `used_today_min >= max_daily_min`). (6) Content filter level: maps to a set of content category exclusions injected into the FYP policy filters.

**Q7: How do you design TikTok's search to return relevant videos?**
A: TikTok search is multi-modal: (1) **Text search**: User query matched against video captions, hashtags, creator usernames, and sound names via an Elasticsearch index. Relevant because caption text is often a description of video content. (2) **Hashtag search**: Direct lookup of hashtag in MySQL `videos.hashtags` index; returns videos with that hashtag sorted by engagement. (3) **Sound search**: Lookup by sound title/artist; returns videos using that sound sorted by recency and engagement. (4) **Semantic search**: User query encoded to embedding (using same text encoder as video embedding pipeline); FAISS ANN search in video embedding space. Returns semantically similar videos even without exact keyword match. (5) **Creator search**: Username prefix search via Elasticsearch. (6) Result ranking: a separate search ranking model (not the FYP model) scores results by: exact match quality, result recency, engagement rate, creator trust score. Search results are not personalized to the same degree as FYP (search is intent-driven, not passive consumption).

**Q8: How does TikTok monetize creators and what data infrastructure supports the Creator Fund?**
A: Creator Fund: TikTok pays creators based on video performance. Data requirements: (1) Daily aggregate metrics per creator: `SUM(views)`, `SUM(qualified_views)` (> 50% watch time), unique viewer count — computed from ClickHouse. (2) Qualified views threshold: a video must have ≥ 100K lifetime views AND creator must have ≥ 10K followers to be eligible. Eligibility check at creator account level in MySQL. (3) Earnings calculation: `qualified_views × rate_per_view` where rate varies by region and is set by TikTok's finance team (not publicly disclosed, but ~$0.02–$0.04 per 1K views). (4) Payment: accumulated daily, paid monthly via creator payout pipeline (ACH/wire transfer/PayPal). The earning data pipeline: ClickHouse daily aggregate job → earnings computation service → payout queue → payment provider API. Audit trail: all earnings calculations stored in an immutable append-only ledger (DynamoDB Streams-style) for dispute resolution.

**Q9: What is TikTok's approach to content moderation at scale?**
A: Three-tier system: (1) **Automated pre-moderation** (at upload processing): safety classifiers run on every frame. High-confidence violations (CSAM, extreme violence) auto-blocked before the video goes live. Medium-confidence flags queued for human review. Low-confidence allowed with monitoring. (2) **Reactive human review**: users can report videos. Reported videos enter a review queue sorted by: report rate × view count (high-report high-view = urgent). Human moderators review and take action: remove, age-restrict, or allow. (3) **Proactive sweeps**: periodic ML-driven sweeps of live content for emerging violation patterns (new memes that evade classifiers). TikTok employs thousands of content moderators globally, regionalized for language/cultural context. Data infrastructure: moderation actions logged in an immutable audit store (S3 WORM), with appeal workflows tracked in a CRM-like case management system. Model updates triggered by human decision patterns: if moderators consistently override the ML classifier on a certain content type, that's training data for model improvement.

**Q10: How does TikTok's A/B testing infrastructure work for the FYP algorithm?**
A: A/B testing at TikTok's scale requires: (1) **Experiment assignment**: user_id hashed into experiment buckets. Bucket assignment stored in a config service (Redis-backed). Consistent assignment: same user always in same bucket. (2) **Disentangled experiments**: each A/B test is one dimension (e.g., "engagement weight change: 0.4 vs 0.5 for watch_complete"). Multiple concurrent experiments use orthogonal bucket spaces. (3) **Holdout group**: permanent 5% holdout group that never receives any experiment — used to measure cumulative algorithm improvement over time vs. baseline. (4) **Metrics**: primary = 7-day retention (did users return? — the ultimate FYP quality metric). Secondary = session duration, video completion rate, follow rate from FYP, "share" action rate. (5) **Novelty effect filtering**: new FYP algorithms often show artificial engagement spike in week 1 (users engaged by novelty). Minimum test duration = 2 weeks to see past novelty effect. (6) **Logging**: all FYP responses tagged with `{experiment_id, bucket, model_version}` stored alongside interaction events in ClickHouse for cohort analysis.

**Q11: How would you design TikTok's "Trending Sounds" feature?**
A: Sounds are a core TikTok mechanic — viral sounds define challenges. Data model: `sounds` table as defined earlier. Trending detection: Flink job consuming `video.published` events, counting sound usage per time window. Trending sound = `d(usage_count)/dt > threshold` (velocity threshold). Top trending sounds stored in Redis sorted set `trending_sounds:{region}` (score = trending velocity). The sounds feed: `GET /v1/trending?category=sounds` returns top 50 trending sounds per region. Creative flow: user taps a sound → sees all videos using that sound sorted by popularity. Implementation: Elasticsearch index of `(sound_id, video_id, view_count)` allows fast "videos using this sound" queries with pagination. Sound matching: when a user uploads with an original sound, audio fingerprint compared against `sounds` table to detect if the "original" is actually a duplicate of an existing sound (for proper attribution and trending count unification).

**Q12: How does TikTok ensure that its recommendation model doesn't perpetuate societal biases?**
A: Bias mitigation in ML systems: (1) **Training data audit**: interaction events are the training data. If the historical system showed certain demographics less often (e.g., darker skin tones due to camera optimization bias), those creators received fewer interactions, which then trained the model to show them less. Audit: compare engagement rates per video normalized by impressions, segmented by creator demographic. (2) **Fairness constraints**: explicitly add a fairness regularization term to the ranking model loss function: penalize large gaps in engagement rate between demographic groups in the training data. (3) **Creator diversity injection**: similar to FYP content diversity, ensure the candidate retrieval step includes a minimum percentage of videos from underrepresented creator categories. (4) **Measurement**: define fairness metrics (e.g., Gini coefficient of views across creators stratified by demographics) and monitor them as model guardrails. (5) **Adversarial testing**: red team exercise where test accounts with different demographic signals are created to measure what FYP they receive.

**Q13: How would you design the "Collaborative TikTok" — two creators collaborating on a single FYP page?**
A: This is a hypothetical extension. Two-creator FYP: (1) Creator A and Creator B link their accounts for a collaboration period. (2) A new "collab" user profile is created (`collab_user_id`) with merged interest embeddings: `collab_embedding = (embedding_A + embedding_B) / 2`. (3) FYP generated for `collab_user_id` draws candidates from both creators' interest spaces. (4) Follow feeds: merged following lists of both creators' followings. (5) Content creation: videos published under the collab ID appear in both creators' followers' Following feeds. (6) Analytics: split attribution between both creators. The technical challenge is the embedding merge — if creator A likes cooking and creator B likes basketball, the merged embedding may actually represent neither well (average embedding might not be near any meaningful cluster centroid in the embedding space). Better approach: concatenate embeddings rather than average, then use a two-tower model trained on co-viewing sessions for couples/friends.

**Q14: What happens to TikTok's FYP during a major infrastructure outage?**
A: Defense-in-depth strategy: (1) **Client-side buffering**: TikTok client pre-buffers 5 videos. During a complete FYP API outage, users can continue watching buffered videos for several minutes without noticing. (2) **CDN independence**: video delivery (CDN) is independent of FYP Service. Even if FYP Service is down, direct video URL access still works. (3) **Fallback FYP**: if FYP Service fails after returning a cursor, client retries against a fallback endpoint that returns a region-based popular video feed from Redis (pre-computed, available even during primary service outage). (4) **Partial degradation messaging**: "Some content may not load. We're working on it." — shown after 10s with no new content. (5) **Priority restores**: FYP Service is tier-1 (highest priority restore). SLA: P0 incident declared if > 5% of FYP requests fail for > 1 minute. On-call engineer paged immediately.

**Q15: How would you measure the "quality" of TikTok's FYP algorithm for a quarterly review?**
A: Multi-dimensional quality framework: (1) **Retention metrics**: 1-day, 7-day, 28-day user retention rates. A better FYP = users return more often. This is the ultimate business metric — aligned with TikTok's revenue model (more daily active time = more ad revenue). (2) **Session metrics**: avg session length (minutes), videos watched per session, watch completion rate. (3) **Satisfaction metrics**: "not interested" rate (explicit negative signal), uninstall rate correlated with FYP quality, user-reported satisfaction (in-app survey, 0.1% sample). (4) **Diversity metrics**: intra-session topic diversity (entropy of category distribution per session), creator diversity (unique creators per 100 videos), geographic content diversity. (5) **Creator metrics**: new creator growth rate, creator content output rate (proxy for creator satisfaction with FYP distribution fairness). (6) **Fairness metrics**: view distribution Gini coefficient across creators, engagement rate parity across creator demographic segments. (7) **Long-term health**: "addiction metrics" (sessions > 3h/day counts) monitored and capped via screen time features for user wellbeing — a regulatory and ethical metric increasingly required by the DSA (Digital Services Act) in the EU.

---

## 12. References & Further Reading

- Liu, Z. et al. "Monolith: Real-Time Recommendation System With Collisionless Embedding Table." arXiv:2209.07663, 2022. — ByteDance's recommendation system paper; the most direct description of TikTok's actual architecture.
- Covington, P. et al. "Deep Neural Networks for YouTube Recommendations." RecSys 2016. — Two-tower architecture and multi-stage ranking that influenced TikTok's design.
- Zhao, Z. et al. "Recommending What Video to Watch Next: A Multitask Ranking System." RecSys 2019. — Multi-task learning for ranking, directly applicable to TikTok's multi-task model.
- Kong, D. et al. "COLD: Towards the Next Generation of Pre-Ranking System." arXiv:2109.04253, 2021. — ByteDance's pre-ranking (Stage 1 in cascade) architecture.
- Facebook Engineering: "The Architecture of FAISS" — foundational reading for ANN retrieval systems.
- Johnson, J. et al. "Billion-scale similarity search with GPUs." IEEE Transactions on Big Data, 2021. — FAISS technical foundation.
- TikTok Engineering Blog: "Monolith: A High-Performance Recommendation System" — blog post accompanying the Monolith paper.
- Apache ClickHouse documentation: clickhouse.com/docs — used for interaction event storage and feature computation.
- TiKV documentation: tikv.org — ByteDance's distributed key-value store used for video metadata.
- Pandia, S. "Video Streaming at Scale: Netflix, YouTube, Facebook, and TikTok." ACM Tech Talk, 2021. — Overview of large-scale video pipeline architectures.
- "Adaptive Video Streaming with HLS" — Apple Developer Documentation (developer.apple.com) — HLS specification; critical for understanding TikTok's video delivery.
- Kleppmann, M. "Designing Data-Intensive Applications" (O'Reilly, 2017) — Chapter 11 (Stream Processing with Kafka/Flink) directly relevant to the interaction event pipeline.
- "ByteDance Infrastructure" — various engineering blog posts at engineering.tiktok.com and tech.bytedance.com (real URLs, content changes periodically).
- "The Filter Bubble" by Eli Pariser (Penguin, 2011) — Background reading on recommendation algorithm societal effects; relevant to the bias/diversity design questions.
- GDPR Article 22 — Automated decision-making and profiling regulations, relevant to FYP algorithm accountability requirements in the EU.
