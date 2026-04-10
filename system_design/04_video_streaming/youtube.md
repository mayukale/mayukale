# System Design: YouTube

---

## 1. Requirement Clarifications

### Functional Requirements

1. Users can upload videos (up to 12 hours / 256 GB raw source file).
2. Uploaded videos are transcoded into multiple resolutions: 144p, 240p, 360p, 480p, 720p, 1080p, 1440p, 2160p (4K), and multiple bitrates per resolution.
3. Users can stream videos with adaptive bitrate (ABR) playback via HLS/DASH.
4. Users can search for videos by title, description, tags, and transcript (auto-generated captions).
5. Users can like, dislike, comment on, and share videos.
6. A recommendation feed is shown on the home page and in the "Up Next" sidebar.
7. Creators have a channel page, subscriber counts, and analytics dashboard.
8. Videos can be monetized via ads (pre-roll, mid-roll) and channel memberships.
9. Users can create playlists and save videos to Watch Later.
10. Live streaming is supported (out of scope for deep-dive, handled separately).

### Non-Functional Requirements

1. Availability: 99.99% uptime for video playback (< 52 minutes downtime/year).
2. Durability: Uploaded video data must never be lost; 11 nines of durability (S3-equivalent).
3. Scalability: Support 2.7 billion logged-in users per month; peak 80 million concurrent viewers.
4. Latency:
   - Search results: < 200 ms p99.
   - Video start time (TTFV - Time To First Video frame): < 2 seconds on broadband, < 5 seconds on mobile.
   - Comment post: < 500 ms p99.
5. Consistency: Eventual consistency acceptable for view counts, likes, recommendations. Strong consistency for financial transactions (ad payments).
6. Throughput: Ingest 500+ hours of video uploaded every minute.

### Out of Scope

- Live streaming infrastructure (separate system design).
- YouTube Shorts (short-form vertical video; different encoding pipeline).
- YouTube Music and YouTube TV (separate products).
- Ad auction system (DoubleClick / Google Ads internals).
- Creator Studio advanced editing features.
- Legal takedown / DMCA automation internals.

---

## 2. Users & Scale

### User Types

| User Type       | Description                                              | Primary Actions                              |
|-----------------|----------------------------------------------------------|----------------------------------------------|
| Viewer          | Unauthenticated or authenticated consumer                | Search, Watch, Browse feed                   |
| Creator         | Authenticated uploader                                   | Upload, Manage channel, View analytics       |
| Subscriber      | Viewer with channel subscription                        | Receive notifications, personalized feed     |
| Moderator       | YouTube Trust & Safety staff                            | Content review, policy enforcement           |
| Advertiser      | Entity buying ad inventory                              | Campaign setup, targeting                    |

### Traffic Estimates

**Assumptions (publicly available data + reasonable extrapolation as of 2025):**
- 2.7 billion monthly active users (MAU)
- Daily active users (DAU) = 50% of MAU = 1.35 billion
- Average session: 40 minutes/day (YouTube internal data cited in earnings calls)
- Average video length: 7 minutes
- Videos watched per session: 40 min / 7 min ≈ 5.7 videos/session
- Uploaded video: 500 hours/minute = 30,000 hours/hour

| Metric                          | Calculation                                                       | Result            |
|---------------------------------|-------------------------------------------------------------------|-------------------|
| DAU                             | 2.7B MAU × 0.50                                                   | 1.35 billion      |
| Video views per day             | 1.35B DAU × 5.7 videos                                           | ~7.7 billion/day  |
| Video views per second (avg)    | 7.7B / 86,400                                                     | ~89,000 views/sec |
| Peak views/sec (3× avg)         | 89,000 × 3                                                        | ~267,000 views/sec|
| Uploads per second              | 500 hours/min × 60 min × 3600 sec/hr / 86400 sec/day → 500×60/86400 | ~0.35 hours raw/sec ≈ 1,260 GB/sec raw ingress (500 GB avg per hour raw) |
| Upload events per second        | 500 hr/min / 60 sec × (1 video / 7 min avg) × 60 = 500/7 ≈ 71 uploads/sec | ~71 video uploads/sec |
| Search queries per second       | Assume 1 search per 3 sessions, 1.35B sessions/day / 3 / 86400   | ~5,200 QPS        |
| Comment writes per second       | Assume 0.5 comments per user per day / 86400                      | ~7,800 writes/sec |

### Latency Requirements

| Operation              | Target p50   | Target p99   | Notes                                        |
|------------------------|--------------|--------------|----------------------------------------------|
| Video start (TTFV)     | < 800 ms     | < 2,000 ms   | Broadband; higher tolerance on mobile        |
| Adaptive segment load  | < 100 ms     | < 300 ms     | 2-sec HLS segment; must buffer ahead         |
| Search results         | < 80 ms      | < 200 ms     | Results from CDN-cached index edge nodes     |
| Comment post           | < 200 ms     | < 500 ms     | Async fan-out to subscribers acceptable      |
| Recommendation refresh | < 300 ms     | < 800 ms     | Cached for 5 min; stale-while-revalidate OK  |
| Upload acknowledgement | < 1,000 ms   | < 3,000 ms   | Per chunk; full processing is async          |

### Storage Estimates

**Assumptions:**
- Average raw upload: 2 GB (7 min × ~48 Mbps raw 1080p source)
- Transcoded outputs: 8 quality levels × avg 200 MB each = 1.6 GB/video
- Thumbnails: 3 auto + creator upload = ~5 MB/video
- Metadata + captions: ~500 KB/video
- Total per video: 2 GB (raw, kept 30 days) + 1.6 GB (transcoded, permanent) + ~5.5 MB (assets)

| Category                   | Calculation                                                            | Result           |
|----------------------------|------------------------------------------------------------------------|------------------|
| New transcoded storage/day | 71 uploads/sec × 86,400 sec × 1.6 GB                                  | ~9.8 PB/day      |
| New raw storage/day (temp) | 71 × 86,400 × 2 GB                                                    | ~12.3 PB/day     |
| Metadata per day           | 71 × 86,400 × 0.5 MB                                                  | ~3.1 TB/day      |
| Cumulative transcoded (5yr)| 9.8 PB/day × 365 × 5                                                  | ~17.9 EB         |
| Thumbnail storage/day      | 71 × 86,400 × 5 MB                                                    | ~30.7 TB/day     |

Note: YouTube reported 800 million videos in 2022. At ~1.6 GB avg transcoded: ~1.28 EB — consistent order-of-magnitude.

### Bandwidth Estimates

**Assumptions:**
- Average streaming bitrate mix: ~2.5 Mbps (weighted avg of 480p @ 1 Mbps, 720p @ 2.5 Mbps, 1080p @ 5 Mbps)
- 80 million peak concurrent viewers

| Direction      | Calculation                                               | Result             |
|----------------|-----------------------------------------------------------|--------------------|
| Egress (peak)  | 80M viewers × 2.5 Mbps                                   | 200 Tbps egress    |
| Egress (avg)   | 89,000 views/sec × 7 min avg × 2.5 Mbps / ~simultaneous | ~22 Tbps avg       |
| Upload ingress | 71 uploads/sec × 2 GB avg / upload duration ~10 min      | ~190 Gbps raw      |

---

## 3. High-Level Architecture

```
                          ┌─────────────────────────────────────────┐
                          │              CLIENTS                      │
                          │  Browser  iOS  Android  Smart TV  CTVs   │
                          └──────────────┬──────────────────────────┘
                                         │ HTTPS
                          ┌──────────────▼──────────────────────────┐
                          │         GLOBAL LOAD BALANCER             │
                          │     (Anycast IP, GeoDNS routing)         │
                          └──┬──────────┬──────────┬────────────────┘
                             │          │          │
               ┌─────────────▼──┐  ┌───▼──────┐  ┌▼──────────────┐
               │  Upload API     │  │ Stream   │  │  Web/App API  │
               │  Service        │  │ API GW   │  │  (Feed,Search │
               │  (chunked PUT)  │  │          │  │   Comments)   │
               └────────┬───────┘  └──────────┘  └───────────────┘
                        │                │                │
          ┌─────────────▼──────┐         │         ┌─────▼────────┐
          │  Object Store      │         │         │  API Servers  │
          │  (Raw Video Blob)  │         │         │  (stateless)  │
          │  GCS / S3          │         │         └──────┬───────┘
          └────────┬───────────┘         │                │
                   │ event / SQS         │         ┌──────▼──────────────┐
          ┌────────▼───────────┐         │         │  Service Mesh       │
          │  Transcoding Farm  │         │         │  ┌──────────────┐   │
          │  (FFmpeg workers)  │         │         │  │  Video Meta  │   │
          │  Borg/K8s jobs     │         │         │  │  Service     │   │
          └────────┬───────────┘         │         │  └──────────────┘   │
                   │                     │         │  ┌──────────────┐   │
          ┌────────▼───────────┐         │         │  │  Search Svc  │   │
          │  Processed Video   │         │         │  └──────────────┘   │
          │  Object Store      │         │         │  ┌──────────────┐   │
          │  (HLS/DASH segs)   │         │         │  │  Comment Svc │   │
          └────────┬───────────┘         │         │  └──────────────┘   │
                   │                     │         │  ┌──────────────┐   │
          ┌────────▼─────────────────────▼─────┐   │  │  Recommend   │   │
          │         CDN EDGE NETWORK            │   │  │  Service     │   │
          │  (Google Global Cache / PoPs)       │   │  └──────────────┘   │
          │  HLS/DASH manifest + segments       │◄──┘  └──────────────────┘
          └─────────────────────────────────────┘
                   │
          ┌────────▼──────┐
          │  End User      │
          │  Player (ABR)  │
          └───────────────┘
```

**Component Roles:**

- **Global Load Balancer / GeoDNS**: Routes traffic to nearest region using Anycast. Google's Maglev handles L4; L7 routing via GFE (Google Front End).
- **Upload API Service**: Accepts multi-part/resumable uploads. Writes raw chunks to GCS with a session token. Publishes completion event to Pub/Sub.
- **Raw Object Store (GCS/S3)**: Durable blob store for original source video. Lifecycle rules delete raw files after transcoding + 30-day retention.
- **Transcoding Farm**: Fleet of Borg/K8s batch jobs running FFmpeg. Reads raw, outputs HLS segments at each resolution. Also generates thumbnails, runs speech-to-text for captions, and extracts video fingerprint for Content ID.
- **Processed Video Object Store**: Stores HLS playlist files (`.m3u8`) and `.ts`/`.fmp4` segments. Organised as `/{video_id}/{resolution}/{segment_num}.ts`.
- **CDN Edge Network**: Google's own Global Cache network (GGC), ~thousands of PoPs. Caches segments close to users. Cache hit rate >95% for popular content.
- **Stream API Gateway**: Accepts playback start requests, generates signed URLs for manifests, returns CDN-hosted manifest URL. Enforces geo-restrictions.
- **Video Metadata Service**: Stores/retrieves video entity data (title, description, uploader, view count, status). Backed by Spanner (globally consistent) + Bigtable for view count hot-path.
- **Search Service**: Elasticsearch cluster. Indexes video metadata + auto-generated transcripts. Suggests completions, handles typos via fuzzy matching.
- **Comment Service**: Write-optimised append log. Fan-out on read (load comments at view time). Backed by Bigtable.
- **Recommendation Service**: Candidate generation (two-tower neural net) + scoring + re-ranking. Pre-computed watch-next candidates stored in Redis. Batched ML training on BigQuery.

**Primary Use-Case Data Flow (Watch a Video):**

1. Client opens video page → GET `/watch?v={video_id}` to Web API.
2. Web API calls Video Metadata Service → returns title, uploader, thumbnail URL, status=`published`.
3. Client requests playback → GET `/stream/{video_id}/manifest` to Stream API Gateway.
4. Stream API Gateway validates auth, geo-restriction, generates signed CDN URL → returns `master.m3u8` URL.
5. Client fetches `master.m3u8` from CDN edge → lists all quality variants.
6. ABR player estimates bandwidth, fetches variant playlist (e.g. `720p.m3u8`) → gets segment list.
7. Player fetches first 2-3 segments from CDN → playback begins.
8. Adaptive bitrate algorithm continuously monitors buffer and switches quality variants.
9. Concurrent async: view event sent to View Counter Service → Bigtable increment, fed into analytics pipeline.

---

## 4. Data Model

### Entities & Schema (Full SQL)

```sql
-- ─────────────────────────────────────────────
-- USERS / CHANNELS
-- ─────────────────────────────────────────────
CREATE TABLE users (
    user_id         UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    username        VARCHAR(50)     NOT NULL UNIQUE,
    email           VARCHAR(255)    NOT NULL UNIQUE,
    hashed_password VARCHAR(255)    NOT NULL,
    display_name    VARCHAR(100)    NOT NULL,
    avatar_url      TEXT,
    channel_description TEXT,
    subscriber_count    BIGINT      NOT NULL DEFAULT 0,  -- denormalized counter
    total_view_count    BIGINT      NOT NULL DEFAULT 0,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    country_code    CHAR(2),
    is_verified     BOOLEAN         NOT NULL DEFAULT FALSE,
    status          VARCHAR(20)     NOT NULL DEFAULT 'active'
                                    CHECK (status IN ('active','suspended','deleted'))
);

CREATE INDEX idx_users_username ON users(username);
CREATE INDEX idx_users_email    ON users(email);

-- ─────────────────────────────────────────────
-- VIDEOS
-- ─────────────────────────────────────────────
CREATE TABLE videos (
    video_id        VARCHAR(11)     PRIMARY KEY,  -- YouTube-style base64 ID (e.g. dQw4w9WgXcQ)
    uploader_id     UUID            NOT NULL REFERENCES users(user_id),
    title           VARCHAR(200)    NOT NULL,
    description     TEXT,
    raw_storage_key TEXT,                          -- GCS path to original upload
    duration_sec    INTEGER,                       -- populated after transcoding
    file_size_bytes BIGINT,
    status          VARCHAR(20)     NOT NULL DEFAULT 'uploading'
                                    CHECK (status IN (
                                        'uploading','processing','published',
                                        'private','unlisted','removed','failed'
                                    )),
    visibility      VARCHAR(10)     NOT NULL DEFAULT 'public'
                                    CHECK (visibility IN ('public','unlisted','private')),
    category_id     SMALLINT        REFERENCES categories(category_id),
    language        CHAR(5),                       -- BCP-47 (e.g. 'en-US')
    license         VARCHAR(30)     NOT NULL DEFAULT 'standard',
    age_restricted  BOOLEAN         NOT NULL DEFAULT FALSE,
    made_for_kids   BOOLEAN         NOT NULL DEFAULT FALSE,
    allow_comments  BOOLEAN         NOT NULL DEFAULT TRUE,
    allow_ratings   BOOLEAN         NOT NULL DEFAULT TRUE,
    view_count      BIGINT          NOT NULL DEFAULT 0,  -- counter shard aggregated
    like_count      BIGINT          NOT NULL DEFAULT 0,
    dislike_count   BIGINT          NOT NULL DEFAULT 0,
    comment_count   BIGINT          NOT NULL DEFAULT 0,
    thumbnail_url   TEXT,
    published_at    TIMESTAMPTZ,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_videos_uploader   ON videos(uploader_id, published_at DESC);
CREATE INDEX idx_videos_published  ON videos(published_at DESC) WHERE status = 'published';
CREATE INDEX idx_videos_status     ON videos(status);

-- ─────────────────────────────────────────────
-- VIDEO RENDITIONS (transcoded outputs)
-- ─────────────────────────────────────────────
CREATE TABLE video_renditions (
    rendition_id    BIGSERIAL       PRIMARY KEY,
    video_id        VARCHAR(11)     NOT NULL REFERENCES videos(video_id),
    resolution      VARCHAR(10)     NOT NULL,  -- '1080p', '720p', '480p', etc.
    width           SMALLINT        NOT NULL,
    height          SMALLINT        NOT NULL,
    bitrate_kbps    INTEGER         NOT NULL,
    codec           VARCHAR(20)     NOT NULL DEFAULT 'h264',  -- 'h264','vp9','av1','hevc'
    container       VARCHAR(10)     NOT NULL DEFAULT 'fmp4',
    manifest_key    TEXT            NOT NULL,  -- GCS path to .m3u8 / .mpd
    storage_bytes   BIGINT,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    UNIQUE (video_id, resolution, codec)
);

-- ─────────────────────────────────────────────
-- TAGS
-- ─────────────────────────────────────────────
CREATE TABLE tags (
    tag_id          SERIAL          PRIMARY KEY,
    name            VARCHAR(100)    NOT NULL UNIQUE
);

CREATE TABLE video_tags (
    video_id        VARCHAR(11)     NOT NULL REFERENCES videos(video_id),
    tag_id          INTEGER         NOT NULL REFERENCES tags(tag_id),
    PRIMARY KEY (video_id, tag_id)
);

-- ─────────────────────────────────────────────
-- CATEGORIES
-- ─────────────────────────────────────────────
CREATE TABLE categories (
    category_id     SMALLINT        PRIMARY KEY,
    name            VARCHAR(50)     NOT NULL UNIQUE,
    parent_id       SMALLINT        REFERENCES categories(category_id)
);

-- ─────────────────────────────────────────────
-- SUBSCRIPTIONS
-- ─────────────────────────────────────────────
CREATE TABLE subscriptions (
    subscriber_id   UUID            NOT NULL REFERENCES users(user_id),
    channel_id      UUID            NOT NULL REFERENCES users(user_id),
    subscribed_at   TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    notify_all      BOOLEAN         NOT NULL DEFAULT FALSE,
    PRIMARY KEY (subscriber_id, channel_id)
);

CREATE INDEX idx_subs_channel ON subscriptions(channel_id, subscribed_at DESC);

-- ─────────────────────────────────────────────
-- LIKES / DISLIKES
-- ─────────────────────────────────────────────
CREATE TABLE video_votes (
    user_id         UUID            NOT NULL REFERENCES users(user_id),
    video_id        VARCHAR(11)     NOT NULL REFERENCES videos(video_id),
    vote_type       SMALLINT        NOT NULL CHECK (vote_type IN (1, -1)),  -- 1=like, -1=dislike
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (user_id, video_id)
);

-- ─────────────────────────────────────────────
-- COMMENTS
-- ─────────────────────────────────────────────
CREATE TABLE comments (
    comment_id      UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    video_id        VARCHAR(11)     NOT NULL REFERENCES videos(video_id),
    author_id       UUID            NOT NULL REFERENCES users(user_id),
    parent_id       UUID            REFERENCES comments(comment_id),  -- NULL = top-level
    body            TEXT            NOT NULL CHECK (char_length(body) <= 10000),
    like_count      INTEGER         NOT NULL DEFAULT 0,
    is_pinned       BOOLEAN         NOT NULL DEFAULT FALSE,
    is_deleted      BOOLEAN         NOT NULL DEFAULT FALSE,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_comments_video   ON comments(video_id, created_at DESC) WHERE is_deleted = FALSE;
CREATE INDEX idx_comments_parent  ON comments(parent_id) WHERE parent_id IS NOT NULL;

-- ─────────────────────────────────────────────
-- PLAYLISTS
-- ─────────────────────────────────────────────
CREATE TABLE playlists (
    playlist_id     UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id        UUID            NOT NULL REFERENCES users(user_id),
    title           VARCHAR(200)    NOT NULL,
    description     TEXT,
    visibility      VARCHAR(10)     NOT NULL DEFAULT 'public',
    video_count     INTEGER         NOT NULL DEFAULT 0,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TABLE playlist_items (
    playlist_id     UUID            NOT NULL REFERENCES playlists(playlist_id),
    video_id        VARCHAR(11)     NOT NULL REFERENCES videos(video_id),
    position        INTEGER         NOT NULL,
    added_at        TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (playlist_id, video_id)
);

-- ─────────────────────────────────────────────
-- WATCH HISTORY
-- ─────────────────────────────────────────────
CREATE TABLE watch_events (
    user_id         UUID            NOT NULL,
    video_id        VARCHAR(11)     NOT NULL,
    watched_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    watch_duration_sec INTEGER,
    completion_pct  SMALLINT,                      -- 0-100
    device_type     VARCHAR(20),
    PRIMARY KEY (user_id, video_id, watched_at)
) PARTITION BY RANGE (watched_at);

-- Partition monthly; old partitions archived to cold storage
CREATE TABLE watch_events_2025_04 PARTITION OF watch_events
    FOR VALUES FROM ('2025-04-01') TO ('2025-05-01');
```

### Database Choice

| Database            | Pros                                                               | Cons                                                        |
|---------------------|--------------------------------------------------------------------|-------------------------------------------------------------|
| PostgreSQL (OLTP)   | ACID, rich query language, mature ecosystem, partitioning          | Vertical scale limits; sharding is complex                  |
| Google Spanner      | Globally distributed, TrueTime, externally consistent, SQL        | Expensive; proprietary; higher latency than single-region   |
| MySQL + Vitess      | Web-scale horizontal sharding, YouTube's actual choice             | Sharding complexity; limited OLAP                           |
| Cassandra           | Excellent write throughput, tunable consistency, linear scaling    | No joins; complex data modeling; eventual consistency       |
| Bigtable            | Sub-10ms reads/writes at petabyte scale, ideal for time-series    | No SQL; no transactions; limited query patterns             |
| Elasticsearch       | Full-text search, faceting, fuzzy matching, horizontal scale       | Not a primary store; eventual consistency; resource heavy   |
| Redis               | Sub-millisecond reads, atomic counters, pub/sub, sorted sets      | In-memory cost; persistence is secondary                    |

**Selected Architecture (polyglot):**

- **MySQL + Vitess** (sharded by `user_id`/`video_id`): Core entity store (users, videos, subscriptions, playlists). This is YouTube's documented choice. Vitess provides horizontal sharding, connection pooling, and query rewrites transparently. Videos table sharded by `video_id` (consistent hash). Users table sharded by `user_id`.
- **Bigtable**: View counts (atomic increments), watch history (time-series append), comment storage (row key = `video_id#reverse_timestamp`). Bigtable's wide-column model handles 3 million QPS write throughput at low latency — essential for view counter hot path.
- **Elasticsearch**: Search index. Indexes `title`, `description`, `tags`, `auto_captions`. Documents replicated from MySQL via Debezium CDC. Queries use `multi_match` with `best_fields` + `phrase_prefix` for suggestions.
- **Redis Cluster**: Recommendation candidate cache (sorted sets by score), session tokens, rate-limit counters, trending videos sorted set (`ZINCRBY trending {video_id}`).
- **BigQuery**: Offline analytics, ad reporting, ML feature engineering. Receives events from Pub/Sub → Dataflow → BigQuery.

---

## 5. API Design

All endpoints are versioned under `/api/v1/`. Auth via OAuth 2.0 Bearer tokens (JWT). Rate limits enforced at the API Gateway per `user_id` (authenticated) or `ip` (anonymous).

### Video Upload

```
POST /api/v1/uploads/initiate
Authorization: Bearer {token}
Content-Type: application/json

Request:
{
  "filename": "my_video.mp4",
  "file_size_bytes": 2147483648,
  "content_type": "video/mp4",
  "title": "My Video Title",
  "description": "...",
  "visibility": "private",
  "category_id": 22
}

Response 200:
{
  "upload_session_id": "sess_abc123",
  "video_id": "dQw4w9Wg",
  "upload_url": "https://upload.youtube.com/upload/video?uploadType=resumable&upload_id=sess_abc123",
  "expires_at": "2025-04-10T00:00:00Z"
}

Rate limit: 10 uploads/hour per user
```

```
PUT /api/v1/uploads/{upload_session_id}
Authorization: Bearer {token}
Content-Range: bytes 0-5242879/2147483648
Content-Length: 5242880

Body: <binary chunk>

Response 308 Resume Incomplete:
{
  "received_bytes": 5242880
}

Response 200 (final chunk):
{
  "video_id": "dQw4w9Wg",
  "status": "processing"
}
```

### Video Playback

```
GET /api/v1/videos/{video_id}/stream
Authorization: Bearer {token}   (optional for public videos)

Response 200:
{
  "video_id": "dQw4w9Wg",
  "manifest_url": "https://cdn.youtube.com/videoplayback/dQw4w9Wg/master.m3u8?sig=abc&exp=1234567890",
  "manifest_type": "hls",
  "duration_sec": 212,
  "available_qualities": ["144p","240p","360p","480p","720p","1080p"],
  "captions": [
    {"language": "en", "url": "https://cdn.youtube.com/cc/dQw4w9Wg/en.vtt"}
  ],
  "geo_restricted": false
}

Rate limit: 10,000 req/min per IP (burst); 1,000 req/min sustained
```

### Search

```
GET /api/v1/search?q={query}&filter={filter}&sort={sort}&page_token={token}&limit={limit}
Authorization: Bearer {token}   (optional)

Query params:
  q           - Search query string (required, max 100 chars)
  filter      - Comma-separated: type:video|channel|playlist, duration:short|medium|long,
                upload_date:today|week|month|year, hd:true
  sort        - relevance|upload_date|view_count|rating (default: relevance)
  page_token  - Opaque cursor for next page
  limit       - 10-50 (default: 20)

Response 200:
{
  "items": [
    {
      "type": "video",
      "video_id": "dQw4w9Wg",
      "title": "...",
      "channel": {"id": "...", "name": "..."},
      "thumbnail_url": "...",
      "duration_sec": 212,
      "view_count": 1400000000,
      "published_at": "1981-10-25T00:00:00Z",
      "relevance_score": 0.98
    }
  ],
  "next_page_token": "eyJmcm9tIjoyMH0=",
  "total_results": 15000,
  "search_took_ms": 45
}

Rate limit: 100 req/min per user; 20 req/min unauthenticated
```

### Comments

```
GET /api/v1/videos/{video_id}/comments?sort=top|new&page_token={token}&limit={limit}
Authorization: Bearer {token}   (optional)

Response 200:
{
  "comments": [
    {
      "comment_id": "Ugk...",
      "author": {"id": "...", "display_name": "...", "avatar_url": "..."},
      "body": "Never gonna give you up!",
      "like_count": 94200,
      "reply_count": 312,
      "created_at": "2021-06-15T10:32:00Z",
      "is_pinned": false
    }
  ],
  "next_page_token": "...",
  "total_count": 2400000
}

POST /api/v1/videos/{video_id}/comments
Authorization: Bearer {token}
Content-Type: application/json

{
  "body": "Never gonna let you down!",
  "parent_id": null
}

Response 201:
{
  "comment_id": "Ugk...",
  "created_at": "2025-04-09T12:00:00Z"
}

Rate limit: 60 comment posts/hour per user
```

### Channel & Subscription

```
GET /api/v1/channels/{channel_id}
GET /api/v1/channels/{channel_id}/videos?sort=newest|popular&page_token={token}&limit={limit}

POST /api/v1/channels/{channel_id}/subscribe
Authorization: Bearer {token}
Response 204

DELETE /api/v1/channels/{channel_id}/subscribe
Authorization: Bearer {token}
Response 204
```

### Recommendations

```
GET /api/v1/feed/home?page_token={token}&limit={limit}
Authorization: Bearer {token}

GET /api/v1/videos/{video_id}/related?limit={limit}
Authorization: Bearer {token}   (optional)

Response 200:
{
  "items": [ {video objects...} ],
  "next_page_token": "...",
  "model_version": "v42",
  "personalization_level": "personalized"  // or "trending" for anonymous
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Transcoding Pipeline

**Problem it solves:**
Raw uploaded videos arrive in arbitrary formats (MOV, AVI, MKV, ProRes, H.264, H.265) at arbitrary bitrates and resolutions. Viewers use wildly different devices and network conditions. The pipeline must convert every upload into a standardized set of quality levels with adaptive bitrate (ABR) manifests, suitable for delivery over HLS or DASH from a CDN, within minutes of upload completion.

**Possible Approaches:**

| Approach                      | Pros                                                         | Cons                                                             |
|-------------------------------|--------------------------------------------------------------|------------------------------------------------------------------|
| Single server FFmpeg           | Simple; easy to debug                                        | Does not scale; one upload blocks queue                          |
| Horizontally scaled worker pool| Stateless workers; easy to scale with queue depth            | Need good queue/job management; coordination for large files     |
| Segment-parallel transcoding   | Splits video into segments, transcodes in parallel; fastest  | Requires keyframe alignment split; assembler step needed         |
| Cloud-managed (AWS MediaConvert)| Fully managed; no ops; integrates with S3                   | Expensive at YouTube scale; vendor lock-in; limited customisation|
| GPU-accelerated encoding       | NVENC/NVDEC 10-40× faster than CPU; HEVC/AV1 viability      | Higher instance cost; driver complexity; not all codecs optimal  |

**Selected Approach: Segment-parallel transcoding with a distributed worker pool on GPU machines.**

YouTube processes each video by:

1. **Splitting**: FFmpeg splits raw file into 30-second GOP-aligned segments (using `segment` muxer with `force_key_frames`). Splitting is O(1) — it's a container operation, not a re-encode.
2. **Parallel job dispatch**: For each segment × each target quality, a job is published to a Pub/Sub topic. With 8 qualities and a 10-minute video (20 segments), that's 160 independent jobs.
3. **Worker pool**: K8s jobs with `n1-standard-32` + T4 GPUs. Workers consume from queue, run FFmpeg with `-hwaccel cuda -c:v h264_nvenc`. T4 GPU encodes H.264 at ~800 fps (vs ~80 fps CPU-only).
4. **Assembly**: After all segment jobs for a quality level complete, an assembler concatenates segments into the final HLS playlist with correct discontinuity tags.
5. **Manifest generation**: Master m3u8 is written pointing to all quality variant playlists.
6. **Post-processing**: Thumbnail extraction (ffmpeg `-ss 10 -vframes 1`), auto-caption via Cloud Speech-to-Text, Content ID fingerprint extraction.

**Encoding ladder (target bitrates):**

| Resolution | Bitrate (H.264) | Bitrate (VP9) | Frame Rate |
|------------|-----------------|---------------|------------|
| 144p       | 100 Kbps        | 80 Kbps       | 15 fps     |
| 240p       | 300 Kbps        | 240 Kbps      | 24 fps     |
| 360p       | 500 Kbps        | 400 Kbps      | 30 fps     |
| 480p       | 1,000 Kbps      | 750 Kbps      | 30 fps     |
| 720p       | 2,500 Kbps      | 1,500 Kbps    | 30/60 fps  |
| 1080p      | 5,000 Kbps      | 3,000 Kbps    | 30/60 fps  |
| 1440p      | 12,000 Kbps     | 6,000 Kbps    | 30/60 fps  |
| 2160p (4K) | 35,000 Kbps     | 18,000 Kbps   | 24/30/60   |

**Implementation Detail:**
- Job coordination via Google Pub/Sub with exactly-once semantics via ack IDs and dead-letter queues.
- Workers use a lease mechanism: pull message, set visibility timeout to `estimated_transcode_time × 1.5`. If worker dies, message reappears.
- Each worker writes output segments to GCS using a temp prefix; atomically moves to final prefix on success.
- A coordinator service polls job completion per video_id (using a counter in Redis: `DECR pending_jobs:{video_id}`). When counter reaches 0, triggers manifest assembly and metadata update (`status=published`).
- Per-codec AV1 encoding (libaom-av1) is added for new uploads as AV1 delivers 30% better compression than VP9 at same quality. AV1 is CPU-expensive (10× VP9) — justified because storage/bandwidth savings at YouTube scale exceed GPU cost.

**Q&A:**

Q: Why split into segments rather than encoding the whole file per worker?
A: A 2-hour movie transcoded as one job on one machine takes ~45 minutes even with GPU. Splitting into 30-second segments means 240 parallel jobs, completing in ~3 minutes. The tradeoff is slightly higher coordination overhead and a stitching step, but the speedup is 15×.

Q: How do you ensure keyframe alignment when splitting?
A: FFmpeg's `-force_key_frames expr:gte(t,n_forced*30)` is passed during the split pre-pass. This inserts I-frames every 30 seconds, ensuring each segment starts cleanly. Without this, segment boundaries would create glitched transitions in HLS.

Q: What happens if a transcode worker crashes mid-job?
A: Pub/Sub visibility timeout expires (e.g., 10 minutes). The message becomes visible again and is picked up by another worker. The coordinator only marks a job done on receipt of a success message. No partial writes reach the final GCS prefix — the atomic rename prevents corrupt segments from being served.

Q: How do you handle re-processing of old videos when a new codec (AV1) is available?
A: Maintain a `codec_version` field in `video_renditions`. A background "re-encode" job queue targets videos above a threshold (e.g., > 1M views) ordered by view count descending. This amortizes compute cost vs. CDN bandwidth savings. Videos below a view threshold may never be re-encoded (cost doesn't justify it).

Q: How do you prevent one large upload (4-hour 4K raw, 50 GB) from starving the queue?
A: Priority queues: time-sensitive uploads (from large creators, viral-trending topics) get a HIGH priority queue. Large files get chunked into more segment jobs. Resource quotas per uploader prevent any single account from flooding the farm. Fair-share scheduling ensures average uploads complete in < 5 minutes.

---

### 6.2 Adaptive Bitrate Streaming (HLS/DASH Delivery)

**Problem it solves:**
Viewers watch on connections ranging from 5G (100+ Mbps) to 3G (1 Mbps) to shared hotel WiFi. A fixed bitrate stream either: (a) buffers constantly on slow connections, or (b) wastes bandwidth on fast ones. ABR streaming dynamically adjusts quality per segment based on observed throughput — maximising quality while keeping the playback buffer healthy.

**Possible Approaches:**

| Protocol          | Latency  | Segment Size | Codec Support | Trick Play | DRM      | Adoption         |
|-------------------|----------|--------------|---------------|------------|----------|------------------|
| HLS (Apple)       | 6-30 sec | 2-10 sec     | H.264, HEVC   | Yes        | FairPlay | Universal        |
| MPEG-DASH         | 3-15 sec | 2-10 sec     | Any           | Yes        | Widevine, PlayReady | Android, web |
| Low-latency HLS   | 1-3 sec  | 0.2 sec parts| H.264, HEVC   | Limited    | FairPlay | Growing          |
| HLS+DASH (both)   | 6-30 sec | —            | All           | Yes        | All      | YouTube's choice |
| WebRTC            | < 500 ms | Real-time    | VP8/VP9/H.264 | No         | Custom   | Live only        |
| CMAF              | 3-6 sec  | Shared chunk | H.264, HEVC   | Yes        | Multi    | Streaming first  |

**Selected: HLS (for iOS/Safari) + MPEG-DASH (for Android/Chrome/TV) served from the same fMP4 segment files via CMAF-compatible packaging.**

Common Media Application Format (CMAF) allows a single set of `.m4s` segment files to be referenced by both an HLS `.m3u8` manifest and a DASH `.mpd` manifest, eliminating the need to store two copies of segments.

**HLS Manifest Structure:**
```
# master.m3u8
#EXTM3U
#EXT-X-VERSION:6
#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1280x720,CODECS="avc1.640028,mp4a.40.2",FRAME-RATE=30
https://cdn.youtube.com/v/dQw4w9Wg/720p/playlist.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=5200000,RESOLUTION=1920x1080,CODECS="avc1.640028,mp4a.40.2",FRAME-RATE=30
https://cdn.youtube.com/v/dQw4w9Wg/1080p/playlist.m3u8
...

# 720p/playlist.m3u8
#EXTM3U
#EXT-X-TARGETDURATION:2
#EXT-X-VERSION:6
#EXT-X-MAP:URI="init.mp4"
#EXTINF:2.0,
seg_000001.m4s
#EXTINF:2.0,
seg_000002.m4s
...
#EXT-X-ENDLIST
```

**ABR Algorithm (client-side):**
YouTube's player uses a throughput-based algorithm with buffer health feedback:
1. After each segment download, measure `actual_throughput = segment_bytes / download_time`.
2. Compute `safe_throughput = actual_throughput × 0.85` (safety margin for variability).
3. Select the highest quality where `bitrate ≤ safe_throughput`.
4. Buffer target: maintain 30 seconds of buffer. If buffer < 10 seconds, downgrade quality regardless of throughput.
5. Upgrade hysteresis: do not upgrade quality unless buffer > 20 seconds AND throughput consistently above threshold for 3 consecutive segments (prevents oscillation).

**CDN Caching Strategy:**
- Segments are immutable once written (content-addressed by `video_id/resolution/segment_id`). `Cache-Control: public, max-age=31536000, immutable`.
- Master and variant playlists for VOD are also immutable once finalized. CDN cache hit rate: ~97% for popular videos, ~60% for long-tail.
- Signed URLs prevent unauthorized access to premium/geo-restricted content: `HMAC-SHA256(video_id + user_id + expiry + secret_key)`.

**Q&A:**

Q: Why 2-second segments rather than 10-second segments?
A: Shorter segments mean the ABR algorithm can react to network changes faster. With 10-second segments, a sudden bandwidth drop means 10 seconds of downloading the wrong quality before adapting. 2-second segments adapt within 4-6 seconds. The tradeoff: more HTTP requests (higher connection overhead) and more objects to cache. fMP4 with HTTP/2 multiplexing mitigates the connection overhead.

Q: How does CDN cache warm-up work for a newly published video?
A: The first request for each segment at each PoP will be a cache miss — fetched from origin GCS. YouTube pre-warms CDN for high-anticipated videos (e.g., scheduled premieres) by issuing synthetic requests from CDN nodes. For organic uploads, cold miss impact is limited because early viewers hit origin and warm the edge cache for subsequent viewers.

Q: What is the purpose of the `EXT-X-MAP` init segment in fMP4?
A: fMP4 (fragmented MP4) requires an initialization segment containing codec parameters (box types `ftyp`, `moov`). Without it, the decoder doesn't know how to interpret subsequent `mdat` boxes. The init segment is fetched once per quality switch and is typically ~5 KB. It's referenced via `EXT-X-MAP` in the HLS playlist.

Q: How does YouTube serve content in countries with restrictive CDN infrastructure (e.g., China-adjacent regions)?
A: YouTube is blocked in mainland China. For other restrictive regions, Google deploys GGC (Google Global Cache) nodes at local ISPs via peering agreements. Traffic flows from GCS → GGC node at ISP → user, reducing cross-border bandwidth. Where regulations require local data residency, separate origin buckets and CDN regions are configured.

Q: How do you handle mid-stream quality switches seamlessly without re-buffering?
A: CMAF-aligned segments across quality levels share the same timeline (presentation timestamp). When the player switches from 720p to 1080p, it continues from the next segment boundary (2-second alignment), not from the start of a buffered region. The decoder handles the resolution change transparently between segments. No seek or buffer flush is needed.

---

### 6.3 Recommendation Engine

**Problem it solves:**
YouTube has ~800 million videos. The "Up Next" autoplay and home feed must select ~20 videos from this corpus that a specific user is likely to watch and enjoy. Relevance must be personalized in real-time. The system processes 89,000 video-watch events/second to update signals and must serve recommendations in < 300 ms.

**Possible Approaches:**

| Approach                         | Pros                                            | Cons                                                     |
|----------------------------------|-------------------------------------------------|----------------------------------------------------------|
| Popularity-based (trending)      | Simple; low latency; works for new users        | Not personalized; filter bubble for existing users       |
| Collaborative filtering (matrix) | Captures user-item patterns; well studied       | Cold start; sparsity; doesn't use content features       |
| Content-based filtering          | Works for new videos (cold start)               | Over-specialization; misses serendipity                  |
| Two-tower neural net             | Scales to billions; real-time; state of art     | Complex to train; requires feature engineering           |
| Transformer-based sequential     | Captures watch sequence context                 | Very expensive to serve at < 100 ms                      |

**Selected: Two-tower deep neural network (candidate generation) + Wide & Deep model (ranking), as described in YouTube's 2016 RecSys paper.**

**Architecture:**

```
Stage 1: Candidate Generation (millions → hundreds)
  ┌──────────────────────────────────────┐
  │  User Tower                          │
  │  - watch_history embeddings (mean)   │
  │  - search_query embeddings           │
  │  - demographic features              │
  │  - recent_watch_time                 │
  │            ↓                         │
  │  DNN [256, 128] → user_embedding[64] │
  └─────────────────┬────────────────────┘
                    │  Approximate Nearest Neighbor
  ┌─────────────────▼────────────────────┐
  │  Video Tower                         │
  │  - video_id embedding                │
  │  - title/description embeddings      │
  │  - view_count, like_rate             │
  │  - freshness (hours since publish)   │
  │            ↓                         │
  │  DNN [256, 128] → video_embedding[64]│
  └──────────────────────────────────────┘
  ANN: ScaNN / FAISS on 800M video embeddings
  → Top 500 candidates in < 10 ms

Stage 2: Ranking (hundreds → 20)
  Wide & Deep Model:
  - Wide part: cross-product features (user × video interactions)
  - Deep part: embeddings of all features → DNN[1024, 512, 256] → score
  - Label: predicted watch time (regression), not click probability
    (predicts engagement, not just clicks — prevents clickbait)
  → Ranks 500 candidates → top 20 in < 50 ms

Stage 3: Re-ranking (filters, diversity)
  - Deduplication (same channel twice in a row)
  - Topic diversity (no more than 3 videos from same category)
  - Safety filters (demonetized, age-restricted)
  - Freshness boost (inject 2 recent videos from new creators)
```

**Feature Store:**
- User features updated in near-real-time via Pub/Sub → Dataflow → Bigtable.
- Video features (view count, like rate, freshness) updated every 5 minutes from BigQuery materialized views.
- Embeddings pre-computed daily via offline TensorFlow training on TPUs, served via TF Serving.
- ANN index (ScaNN) rebuilt every 6 hours; served in-memory on Recommendation Service fleet.

**Q&A:**

Q: Why predict watch time rather than click-through rate?
A: YouTube explicitly moved away from CTR optimization in 2016 because it incentivized clickbait thumbnails. Watch time is harder to game — a video must actually hold attention. Predicting expected watch time combines probability of click with conditional expected duration, surfacing genuinely engaging content.

Q: How do you handle the cold start problem for new videos?
A: New videos enter the system with no watch history. They get a freshness boost in the re-ranking stage and are served as candidates via content-based features (title/description embeddings, channel authority). The system also uses "exploration traffic": for 1-2% of recommendations, it inserts new videos regardless of predicted watch time to gather signal. Once a video has 100+ views, collaborative signals take over.

Q: How do you avoid reinforcing filter bubbles?
A: The diversity re-ranking stage enforces category caps and injects "serendipitous" recommendations based on second-degree connections (videos watched by people who also watched your watch history, across different topics). Additionally, "Explore" vs "Exploit" balance is tuned: ~10% of home feed is exploratory.

Q: How do you serve ANN queries on 800M video embeddings in < 10 ms?
A: ScaNN (Scalable Nearest Neighbors) uses quantization to compress 64-dimensional float vectors and partitions the space into clusters. A query against 800M vectors is reduced to searching ~10,000 candidates in the most relevant clusters via an inverted index over quantized vectors. The index fits in ~50 GB RAM and serves ~20,000 QPS per node. Sharded across multiple nodes for the full fleet.

Q: How do you update the two-tower model continuously as viewing patterns shift?
A: Daily offline batch training on the past 30 days of watch events (sampled to ~100 billion rows in BigQuery). A/B testing gates model pushes: new model must improve average watch time and session length by >0.5% with p < 0.01 before full rollout. The user tower is re-computed for active users every 6 hours; inactive users use stale embeddings which degrade gracefully.

---

## 7. Scaling

### Horizontal Scaling

**API Servers**: Stateless; auto-scaled on K8s with HPA (Horizontal Pod Autoscaler) targeting 70% CPU. Typically 50-200 replicas per region. New pods start in ~30 seconds.

**Transcoding Workers**: K8s batch jobs, scaled by Pub/Sub queue depth. Scale-up trigger: queue depth > 100 messages; scale-down: queue empty for 5 minutes. GPU instances are preemptible (cheaper), with a small fleet of on-demand for latency-sensitive jobs.

**CDN**: Inherently horizontally distributed. Google GGC consists of thousands of PoPs globally. Content popularity distributes naturally across PoPs.

**Search (Elasticsearch)**: Sharded to 20 primary shards with 2 replicas each. Horizontal scaling by adding data nodes. Cross-cluster search federation for global deployments.

### Database Sharding

**MySQL (via Vitess)**:
- `videos` table: sharded by `video_id` (consistent hash, 8 shards initially, expandable). Range-based sharding by video_id prefix would create hotspots for popular videos — consistent hashing distributes evenly.
- `users` table: sharded by `user_id`.
- `subscriptions` table: sharded by `subscriber_id`. To look up all subscribers of a channel, Vitess scatters query to all shards (acceptable for notification fan-out which is async).
- `comments` table: sharded by `video_id` (co-located with video for efficient reads).

**Bigtable** (for view counts): Row key = `video_id#YYYY-MM-DD`. Bigtable's tablet splitting handles hotspot rows automatically. For global viral videos, use counter sharding: `INCRBY counter:{video_id}:shard:{random(0,63)}` and aggregate on read.

### Replication

- MySQL: Each Vitess shard has 1 primary + 2 read replicas. Read queries (video metadata lookups) routed to replicas. Replication lag < 50 ms (acceptable for eventual view counts; not used for financial data).
- Redis: Redis Cluster with 6 nodes (3 primary, 3 replica), data sharded across 16,384 hash slots.
- Bigtable: Managed replication across 2 zones within a region automatically.

### Caching

| Cache Layer          | Technology    | What is Cached                           | TTL        |
|----------------------|---------------|------------------------------------------|------------|
| CDN Edge             | GGC           | Video segments (.m4s), manifests (.m3u8) | 1 year (segments), 5 min (manifests) |
| Application Cache    | Redis Cluster | Video metadata, channel info, user auth  | 5 min      |
| Recommendation Cache | Redis         | Home feed per user_id                    | 5 min, stale-while-revalidate |
| Search Suggestions   | Redis         | Top autocomplete results per prefix      | 1 hour     |
| Trending Videos      | Redis ZSET    | Top 100 trending per country             | 1 min      |
| View Counts (hot)    | Redis         | In-memory aggregation before DB flush    | Flush every 30 sec |

**Cache Invalidation**: Video metadata updates (title change, status change) published to Pub/Sub topic `video-metadata-updates`; subscribers (API servers, CDN purge) invalidate local caches and issue CDN purge for manifest URLs.

### CDN Strategy

- **Google Global Cache (GGC)**: Deployed at ISP POPs inside carrier networks. Traffic: end user → ISP → GGC node (cache hit) → user. No cross-border traffic.
- **Cache hierarchy**: L1 = ISP GGC node (200+ locations), L2 = regional GGC cluster (20 regions), L3 = origin GCS bucket.
- **Cache keys**: Segments use URL as cache key (includes `video_id/quality/segment_num`). Signed URL signature stripped before caching (to prevent signature collisions causing cache misses).
- **Pre-fetch**: For VOD, when a player fetches segment N, GGC proactively fetches N+1, N+2 (predictive pre-fetch) based on manifest analysis.

**Interviewer Q&A:**

Q: How do you handle the "thundering herd" problem when a video goes viral and millions of users simultaneously request the same new video?
A: Multiple mitigations: (1) Request coalescing at CDN edge — if 1,000 users request the same uncached segment simultaneously, the CDN makes one request to origin and holds the others until the response arrives (collapsed forwarding). (2) Staggered cache expiry: add jitter to cache TTLs to prevent simultaneous expiry. (3) Origin rate limiting with request queuing — origins respond with `retry-after` rather than crashing. (4) For anticipated viral events (sports finals, concerts), manual cache pre-warm.

Q: How do you shard the `subscriptions` table given that a popular channel (e.g., T-Series, 280M subscribers) generates fan-out writes to 280M rows on a new upload notification?
A: Subscription lookup for reads is sharded by `subscriber_id` (for "my subscriptions" feed). Fan-out notification on new upload is handled asynchronously via an event queue — not via synchronous SQL queries. When a creator uploads, an event is published; a fan-out service reads subscription records in batches of 1,000 from all shards and pushes notifications. This fan-out takes minutes for a 280M-subscriber channel — acceptable because notification delivery is eventually consistent.

Q: What happens to your MySQL cluster during a primary failure in a shard?
A: Vitess's VTOrc (the orchestrator) detects primary failure within 5-10 seconds (three missed heartbeats). It promotes the most up-to-date replica to primary and reconfigures the Vitess topology. DNS/VIP switches automatically. Downtime for write operations: 10-30 seconds. Read traffic continues uninterrupted via replicas. This meets the 99.99% availability SLA for read-heavy video playback.

Q: How do you handle "hot key" rows in Bigtable for view counts on viral videos?
A: Bigtable hotspot mitigation via counter sharding: instead of one row per video_id, write to `{video_id}#shard#{rand(0,63)}` — 64 shards spread load across 64 different tablets. Each tablet server handles its shard's increments. To read the total, scatter-gather across all 64 shards and sum. An alternative is to buffer counts in Redis (INCR is atomic) and flush to Bigtable every 30 seconds in batch — this reduces Bigtable write QPS by 30× for hot videos.

Q: At 200 Tbps peak egress, how many CDN PoPs are needed and how much capacity per PoP?
A: Google operates 30+ major regional clusters and 200+ ISP-embedded GGC nodes. If 200 Tbps is spread across 200 ISP PoPs with rough load proportional to population, average per PoP ≈ 1 Tbps. Large metro PoPs (US, EU, India, Japan) need 5-20 Tbps each. This requires 10-200 × 100G uplinks per PoP (commodity 100G DWDM transport). Google achieves this through dedicated fiber infrastructure and transit peering.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario                        | Detection                                  | Mitigation                                                        | RTO     |
|-----------------------------------------|--------------------------------------------|-------------------------------------------------------------------|---------|
| Transcoding worker crash mid-job        | Pub/Sub visibility timeout (10 min)        | Message reappears; new worker picks it up; idempotent write       | < 10 min|
| MySQL primary failure                   | VTOrc heartbeat miss (3 × 2s)              | Auto-promote replica; VIP switch; write queue buffered            | 10-30 s |
| CDN PoP outage                          | Health check from Load Balancer            | GeoDNS reroutes to next-nearest PoP; user may see quality drop    | < 60 s  |
| Redis primary shard failure             | Redis Sentinel (3 sentinels, quorum=2)     | Sentinel promotes replica; client reconnects; ~2s blip            | 2-5 s   |
| Bigtable node failure                   | Managed by Google; transparent             | Tablet migration to healthy node; brief elevated latency          | < 30 s  |
| GCS regional outage                     | GCS SLA monitoring                         | Replication to secondary GCS region; CDN serves cached segments   | minutes |
| Elasticsearch data node failure         | ES cluster health API (yellow/red)         | Replica shards promoted; reduced capacity until node replaced     | < 60 s  |
| Recommendation service OOM             | K8s liveness probe failure → pod restart   | Pod restarts; load balancer removes from rotation; Redis has stale feed | < 10 s  |
| Upload API unavailable during upload    | Client timeout on chunk PUT                | Client retries with exponential backoff; session persists 24h     | none (resumable) |
| Content ID false positive removes video | Automated + human review queue            | Uploader can dispute; video reinstated in < 48h; no data loss     | 48 h    |

**Retries & Idempotency:**
- All API mutations use idempotency keys (client-generated UUID in `Idempotency-Key` header).
- Upload chunks use `Content-Range` and server-side received-bytes tracking — re-submitting same range is a no-op.
- Transcoding jobs are idempotent: worker writes to temp GCS path, then does atomic rename. Re-running the same job overwrites with identical output (deterministic FFmpeg parameters).
- Comment posting: idempotency key prevents duplicate comments on retry.

**Circuit Breaker Pattern:**
- All inter-service calls wrapped in Hystrix/Resilience4j circuit breaker with thresholds: open after 50% failure rate over 10-second window with minimum 20 requests.
- Recommendation service: if circuit open, fall back to trending videos (pre-computed, served from Redis without personalization). User sees slightly less relevant feed but no error.
- Search service: if circuit open, fall back to trending/cached results; return `"degraded": true` in response.

---

## 9. Monitoring & Observability

| Metric                          | Type      | Alert Threshold            | Tool           |
|---------------------------------|-----------|----------------------------|----------------|
| TTFV (p50, p95, p99)            | Histogram | p99 > 3s for 5 min         | Monarch / Prometheus |
| Video start failure rate        | Counter   | > 0.1% over 5 min          | Monarch        |
| CDN cache hit rate              | Gauge     | < 90% on popular content   | GGC Dashboard  |
| Transcoding queue depth         | Gauge     | > 1,000 jobs for 10 min    | Pub/Sub metrics|
| Transcoding P99 latency         | Histogram | > 30 min for 1-hr video    | Monarch        |
| Search p99 latency              | Histogram | > 500 ms                   | Prometheus     |
| MySQL replication lag           | Gauge     | > 500 ms                   | Vitess metrics |
| Redis hit rate                  | Gauge     | < 80%                      | Redis INFO     |
| Upload failure rate             | Counter   | > 1% over 5 min            | Monarch        |
| Ad delivery success rate        | Counter   | < 99.9%                    | DFP metrics    |
| Active concurrent viewers       | Gauge     | < 10M unexpected drop      | Monarch        |
| API error rate (5xx)            | Counter   | > 0.5% over 2 min          | Monarch        |

**Distributed Tracing:**
- Every request tagged with a `trace_id` (128-bit, propagated via HTTP header `X-Trace-ID`).
- Google Dapper (published paper basis for Zipkin/Jaeger). Spans for: API gateway → service → DB query → CDN fetch.
- Sampling: 100% of errors; 1% of successful requests; 100% of requests > 5s (tail latency sampling).
- Trace visualization in Cloud Trace. Allows pinpointing which layer (DB, CDN, upstream service) contributes to latency regression.

**Logging:**
- Structured JSON logs (fields: `trace_id`, `user_id`, `video_id`, `action`, `latency_ms`, `status_code`).
- Log levels: ERROR (always alert), WARN (batch review), INFO (sampled 10%), DEBUG (never in prod).
- Log aggregation: Fluentd → Cloud Logging → BigQuery for analytical queries.
- Log retention: 30 days hot, 1 year cold (Coldline GCS).
- PII in logs: `user_id` hashed before logging; IP addresses truncated to /24 subnet for GDPR.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                        | Choice Made                          | Alternative Considered          | Trade-off                                                      |
|---------------------------------|--------------------------------------|---------------------------------|----------------------------------------------------------------|
| Transcoding: split vs whole     | Segment-parallel (30s chunks)        | Whole-file per worker           | +Speed (15×), +Fault tolerance. −Complexity, −extra stitching step |
| Video format: HLS + DASH (CMAF) | Both via shared fMP4 segments        | HLS only                        | +Universal device support. −Slightly more storage (2 manifests)|
| Database: MySQL+Vitess          | Sharded relational                   | Pure Cassandra                  | +Rich queries, +ACID. −Manual sharding complexity vs Cassandra's native scaling |
| View counts: Redis buffer        | Buffer 30s in Redis, batch flush     | Direct Bigtable increment       | +Lower Bigtable write QPS (30×). −Up to 30s stale count        |
| Recommendation: watch time target| Watch time as label                 | CTR as label                    | +Engagement quality. −Harder to debug; watch time noisy signal |
| Cache segments: immutable 1yr   | Permanent cache                      | Short TTL + invalidation        | +Near-100% hit rate. −Cannot retroactively fix bad encodes without purge |
| ANN for recommendations: ScaNN  | ScaNN in-memory                      | Elasticsearch k-NN              | +Faster (10ms vs 50ms). −Non-trivial infrastructure dependency  |
| Subscription fan-out: async     | Event queue + batch fan-out          | Synchronous write fan-out       | +Decoupled, resilient. −Notifications delayed by minutes for mega-channels |
| Comment storage: Bigtable       | Wide-column append                   | PostgreSQL                      | +Write-optimised, infinite scale. −No joins; limited query patterns |
| Search: Elasticsearch           | Separate ES cluster with CDC         | MySQL FULLTEXT                  | +Full-text relevance, fuzzy matching, facets. −Operational complexity, eventual consistency with CDC lag |

---

## 11. Follow-up Interview Questions

**Q1: How would you design the YouTube trending algorithm?**
A: Trending is not the most-viewed video overall (that would always be "Baby Shark"). It measures velocity: view rate growth over the past 24 hours relative to the channel's historical baseline, weighted by geographic spread (trending in multiple regions scores higher), engagement rate (likes + comments / views), and freshness. Implemented as a Dataflow streaming job consuming view events → emitting trending scores per video per country → stored in Redis sorted set (`ZADD trending:{country} {score} {video_id}`). Refreshed every minute.

**Q2: How do you implement Content ID (video fingerprinting for copyright)?**
A: During transcoding post-processing, a video fingerprint is extracted using YouTube's Photon algorithm (audio fingerprint via spectral analysis + visual fingerprint via perceptual hash of keyframes). The fingerprint is stored in a hash lookup table. Every new upload's fingerprint is matched against the reference database of copyrighted content. Matches above a threshold trigger automated actions configured by the rights holder (block, monetize, track). This is an O(1) lookup via LSH (Locality Sensitive Hashing) over the fingerprint space.

**Q3: How would you handle a DDoS attack targeting your video upload endpoint?**
A: Layered defense: (1) Google Cloud Armor (GFE layer) — rate limiting per IP, geo-blocking, L7 WAF rules. (2) Upload sessions are authenticated — no session token, no upload. (3) CAPTCHA challenges for anomalous upload patterns. (4) Per-user upload rate limits enforced at API gateway (10 uploads/hr). (5) Adaptive rate limiting: if upload ingress > 120% capacity, reject with `429 Too Many Requests` and `Retry-After`. Video processing queue acts as a natural buffer — uploads are acknowledged but transcoding is queued.

**Q4: How do you handle video format compatibility for very old browsers or low-end devices?**
A: The encoding ladder includes H.264 at all resolutions (widest hardware decoder support). VP9 and AV1 are offered as alternatives when the client supports them (detected via `User-Agent` or JavaScript `MediaSource.isTypeSupported()`). For Smart TVs with limited codec support, an SSAI (Server-Side Ad Insertion) manifest variant may use simpler encoding profiles. The player's capability detection runs before manifest request and appends codec preference to the API call.

**Q5: How would you design the YouTube Studio analytics dashboard that shows a creator their video's performance?**
A: Analytics pipeline: raw events (views, watch time, likes, shares, impressions) → Pub/Sub → Dataflow (windowed aggregations: 1-min, 15-min, 1-hr, 1-day windows) → BigQuery tables partitioned by `(video_id, date)`. Creator dashboard queries BigQuery materialized views (refreshed hourly for real-time, daily for historical). Real-time "first 24 hours" mode uses streaming inserts to BigQuery for < 5-min freshness. Charts served via an Analytics API that caches BigQuery results in Redis for frequent queries.

**Q6: How do you handle geo-restricted content (e.g., music videos blocked in certain countries)?**
A: Rights metadata stored in `video_geo_restrictions` table: `(video_id, country_code, restriction_type: block|allow_only)`. Stream API Gateway checks user's IP-geolocated country (MaxMind DB, refreshed weekly) against restrictions before generating the signed manifest URL. If blocked, returns `451 Unavailable For Legal Reasons`. CDN-level enforcement via Geo-based response rules prevents manifest delivery even if signed URLs are shared. This is a defense-in-depth approach.

**Q7: How would you implement resumable uploads that survive network disconnects?**
A: GCS Resumable Upload protocol (open standard also used by YouTube): (1) Client POSTs to initiate endpoint → receives upload URI + session token (valid 24h). (2) Client PUTs chunks with `Content-Range` header (e.g., `bytes 0-5242879/total_size`). (3) Server tracks last received byte in Redis: `SET upload_session:{session_id}:offset {last_byte}`. (4) On reconnect, client GETs session status → server returns `308 Resume Incomplete` with `Range: bytes=0-{last_byte}`. (5) Client resumes from `last_byte + 1`. Final chunk triggers transcoding job. If session expires (24h), client must restart from byte 0.

**Q8: What's your approach to monetization — specifically ad insertion into HLS streams?**
A: Client-Side Ad Insertion (CSAI): player makes ad request to Google IMA SDK before/during video; ad is played in a separate player context; ad impression is tracked by ad server. For platforms where CSAI is blocked (ad blockers), Server-Side Ad Insertion (SSAI) is used: origin server stitches ad segments into video manifest; from CDN's perspective it's just more video segments. SSAI requires dynamic manifest generation (not fully cacheable) — served from a separate ad-manifest service that generates personalized manifests per user per session.

**Q9: How would you handle auto-generated subtitles and caption accessibility?**
A: During transcoding post-processing, audio track extracted → sent to Cloud Speech-to-Text API (supports 125+ languages). Transcripts returned as word-level timestamped JSON → converted to WebVTT format (`.vtt` files). Caption accuracy: ~92% for clear English speech, lower for accented speech. Community-contributed captions stored as separate entity, moderated. Captions hosted on CDN, referenced in HLS manifest via `#EXT-X-MEDIA:TYPE=SUBTITLES`. Auto-translation of captions via Cloud Translation API for 100+ additional languages.

**Q10: Describe the notification system for "upload from a subscribed channel."**
A: Event flow: creator upload → video status transitions to `published` → event published to `video-published` Pub/Sub topic. Fan-out service consumes event; reads subscriber list from MySQL (sharded `subscriptions` table) in batches of 10,000. For each batch, publishes to `notification-delivery` topic. Notification service reads and delivers via FCM (Firebase Cloud Messaging) for mobile, WebSocket/SSE for browser, email (throttled: max 1/day per channel). For channels with > 10M subscribers, fan-out runs on dedicated high-throughput workers and completes within 15 minutes. Bell icon "all notifications" vs "occasional" preference stored in `subscriptions.notify_all` field.

**Q11: How do you prevent hot-shard issues when a creator with 200M subscribers uploads simultaneously with 10 other major creators?**
A: The subscriptions fan-out is rate-limited per video_id in the job queue. Even if 10 simultaneous events fire, fan-out workers process different subscriber ranges independently. MySQL shard contention is avoided because fan-out reads are spread across all shards (scatter query by subscriber_id range). The bottleneck shifts to notification delivery (FCM), which is throttled by FCM quotas. YouTube pushes notifications over minutes, not seconds, so instantaneous DB load is smoothed.

**Q12: How would you implement watch history and "continue watching" across devices?**
A: `watch_events` table records `(user_id, video_id, watched_at, completion_pct, device_type)`. "Continue watching" query: `SELECT video_id, completion_pct FROM watch_events WHERE user_id=X AND completion_pct BETWEEN 5 AND 95 ORDER BY watched_at DESC LIMIT 20`. This is stored in MySQL (queryable) and cached in Redis sorted set (keyed by user_id, score=watched_at). Cross-device sync: writes go to MySQL immediately; Redis cache invalidated. Client polls `/api/v1/user/continue-watching` which serves from cache with 30s TTL.

**Q13: What is the impact of AV1 encoding on your infrastructure and why is it worth it?**
A: AV1 delivers 30-50% better compression than H.264 at the same visual quality. At 200 Tbps peak egress, 40% reduction = 80 Tbps bandwidth savings. At $0.01/GB CDN egress, 80 Tbps × 3600s × ~5% of total traffic in AV1 = enormous savings. Cost: AV1 encoding is ~10× slower than H.264. For YouTube's corpus of 800M videos, incremental AV1 re-encoding of top 10M videos takes months on the existing farm. YouTube/Google co-developed AV1 (part of Alliance for Open Media) specifically to avoid HEVC royalty costs and achieve better compression for their scale.

**Q14: How do you design video chapters (timestamps in description)?**
A: Creator adds timestamps in description (e.g., "0:00 Intro\n1:30 Topic A"). A post-processing microservice parses the description using a regex: `(\d+:\d{2}) (.+)`. Parsed chapters stored as JSON in `videos.chapters JSONB` column. Player renders chapter markers on progress bar. Search indexes chapter titles — a search for "how to tie a tie" can match a chapter within a longer video ("Men's Fashion 101"). This is implemented as a lightweight parsing job triggered on description save, with human-readable timestamps validated against video duration.

**Q15: How would you approach a complete data center failure in one of YouTube's primary regions?**
A: YouTube is deployed in multiple GCP regions (US, EU, Asia). Architecture is active-active for reads (CDN caches content regionally) and active-passive for writes (one primary region for MySQL, others as replicas). On regional failure: (1) GeoDNS health checks detect failure in < 30s → reroutes new upload traffic to adjacent region. (2) Ongoing video playback: CDN edge nodes continue serving cached segments — most users unaffected. (3) Transcoding jobs in-flight: re-queued in surviving region (jobs are idempotent). (4) MySQL replica in surviving region promoted to primary within minutes. RTO: ~5 minutes for write traffic, ~0 for read traffic (CDN). RPO: < 60 seconds (replication lag bound).

---

## 12. References & Further Reading

1. **YouTube Infrastructure** — Vitess: https://vitess.io/ (Open-source MySQL sharding used at YouTube since 2011)
2. **YouTube Recommendation System** — Covington, P., Adams, J., Sargin, E. (2016). "Deep Neural Networks for YouTube Recommendations." ACM RecSys 2016. https://dl.acm.org/doi/10.1145/2959100.2959190
3. **HLS Specification** — Apple. "HTTP Live Streaming (RFC 8216)." IETF RFC 8216. https://datatracker.ietf.org/doc/html/rfc8216
4. **MPEG-DASH Standard** — ISO/IEC 23009-1:2022. Dynamic Adaptive Streaming over HTTP.
5. **CMAF (Common Media Application Format)** — ISO/IEC 23000-19. https://mpeg.chiariglione.org/standards/mpeg-a/common-media-application-format
6. **AV1 Codec** — Alliance for Open Media. https://aomedia.org/av1/
7. **ScaNN (Scalable Nearest Neighbors)** — Guo, R., et al. (2020). "Accelerating Large-Scale Inference with Anisotropic Vector Quantization." ICML 2020. https://arxiv.org/abs/1908.10396
8. **Google Dapper (Distributed Tracing)** — Sigelman, B.H., et al. (2010). "Dapper, a Large-Scale Distributed Systems Tracing Infrastructure." Google Technical Report. https://research.google/pubs/pub36356/
9. **FFmpeg Documentation** — https://ffmpeg.org/documentation.html
10. **Wide & Deep Learning** — Cheng, H.T., et al. (2016). "Wide & Deep Learning for Recommender Systems." DLRS Workshop, ACM RecSys. https://arxiv.org/abs/1606.07792
11. **Google Global Cache** — Google. "How Google Delivers Content." https://peering.google.com/#/infrastructure
12. **Maglev Load Balancer** — Eisenbud, D., et al. (2016). "Maglev: A Fast and Reliable Software Network Load Balancer." NSDI 2016. https://research.google/pubs/pub44824/
13. **Pub/Sub at Google** — Google Cloud Pub/Sub Documentation. https://cloud.google.com/pubsub/docs/overview
14. **Bigtable** — Chang, F., et al. (2006). "Bigtable: A Distributed Storage System for Structured Data." OSDI 2006. https://research.google/pubs/pub27898/
15. **GCS Resumable Uploads** — https://cloud.google.com/storage/docs/resumable-uploads
