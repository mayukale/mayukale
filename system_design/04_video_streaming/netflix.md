# System Design: Netflix

---

## 1. Requirement Clarifications

### Functional Requirements

1. Users can browse a catalog of movies, TV shows, and documentaries.
2. Users can stream video content with adaptive bitrate playback (multiple resolutions and audio tracks).
3. Netflix supports multiple user profiles per account (up to 5 profiles).
4. A personalized recommendation feed is shown on the home screen.
5. Users can search content by title, genre, actor, director, and mood.
6. Users can download content for offline viewing on mobile devices.
7. Content is region-locked based on licensing agreements.
8. Multiple concurrent streams per subscription tier (Basic: 1, Standard: 2, Premium: 4).
9. Users can rate content and manage a "My List" watchlist.
10. Netflix produces original content (Netflix Originals) distributed globally simultaneously.

### Non-Functional Requirements

1. Availability: 99.99% uptime for streaming (< 52 min/year downtime). Netflix targets this globally across all AWS regions.
2. Durability: Content master files must never be lost (11 nines durability via S3 + Glacier replication).
3. Scalability: 300 million paid subscribers; peak 50 million concurrent streams.
4. Latency:
   - Playback start: < 2 seconds on broadband (measured as time from play button to first frame).
   - Bitrate adaptation: seamless within 2 segments (< 8 seconds of content).
   - Search results: < 200 ms p99.
5. Consistency: Eventual consistency acceptable for ratings, watch history, recommendations. Strong consistency for subscription state and payment processing.
6. Global reach: Operations in 190+ countries; localized content metadata in 30+ languages.

### Out of Scope

- Netflix Games (mobile games product).
- Live sports and events (Netflix launched live events in 2023; separate infrastructure).
- Content acquisition and licensing negotiations.
- Netflix Studio production workflows.
- Payment processing and fraud detection internals.
- DVD/Blu-ray distribution (Netflix's legacy business).

---

## 2. Users & Scale

### User Types

| User Type            | Description                                               | Primary Actions                                    |
|----------------------|-----------------------------------------------------------|----------------------------------------------------|
| Subscriber           | Paid account holder                                       | Stream, Download, Rate, Search, Browse             |
| Profile User         | Individual within an account (child or adult profile)    | Personalized feed, Watch history, My List          |
| Content Manager      | Netflix internal staff                                    | Ingest master files, set metadata, publish         |
| Advertiser           | Brands running ads on ad-supported tier (Netflix Basic)  | Campaign targeting, reporting                      |

### Traffic Estimates

**Assumptions (based on Netflix Q4 2024 earnings and public statements):**
- 300 million paid subscribers globally.
- Average daily active users (DAU): 45% of MAU = 135 million.
- Average viewing time per DAU: 2 hours/day (Netflix reported ~2hr avg globally).
- Average bitrate streamed: 5 Mbps (weighted avg; Netflix's premium streams go to 25 Mbps 4K, but mobile/SD is 0.3-1.5 Mbps).
- Peak concurrency: ~50 million simultaneous streams (8 PM local time overlap across time zones).
- Content catalog size: ~17,000 titles (movies + shows), each movie ~2 hrs, each episode ~45 min.

| Metric                           | Calculation                                                          | Result              |
|----------------------------------|----------------------------------------------------------------------|---------------------|
| DAU                              | 300M × 0.45                                                          | 135 million         |
| Daily streaming hours            | 135M × 2 hr                                                          | 270 million hours   |
| Average concurrent streams       | 270M hr × 3600s / 86400s                                             | ~11.25M streams     |
| Peak concurrent streams          | Stated by Netflix (public)                                           | ~50M streams        |
| Peak egress bandwidth            | 50M × 5 Mbps                                                         | 250 Tbps            |
| Search QPS                       | 135M DAU × 2 searches/day / 86400                                   | ~3,125 QPS          |
| Play events per second           | 135M DAU × 4 plays/day / 86400                                       | ~6,250 plays/sec    |
| Recommendation requests/sec      | 6,250 play events + 2,000 browse sessions/sec                        | ~8,250 rec req/sec  |

### Latency Requirements

| Operation                       | Target p50   | Target p99    | Notes                                      |
|---------------------------------|--------------|---------------|--------------------------------------------|
| Playback start (TTFF)           | < 500 ms     | < 2,000 ms    | Includes OCA selection + first segment     |
| Bitrate switch (buffer drain)   | Imperceptible| < 8 s content | ABR adapts within 2 segments               |
| Search results                  | < 60 ms      | < 200 ms      | Served from Elasticsearch + Redis          |
| Home screen load (metadata)     | < 300 ms     | < 800 ms      | Personalized row ordering + thumbnails     |
| Download initiation             | < 1,000 ms   | < 3,000 ms    | DRM license issuance included              |
| A/B test assignment             | < 5 ms       | < 20 ms       | Inline with every API call                 |

### Storage Estimates

**Assumptions:**
- Average movie: 2 hours raw = ~100 GB uncompressed 4K raw camera footage.
- Netflix encodes at multiple bitrates (15+ per title) × multiple codecs (H.264, HEVC, AV1) × multiple audio tracks.
- Average encoded storage per title (all renditions): ~500 GB for a movie, ~1.2 TB for a full season.
- Total catalog: 17,000 titles; average 30 episodes per TV show, 40% TV, 60% movies.

| Category                     | Calculation                                                           | Result          |
|------------------------------|-----------------------------------------------------------------------|-----------------|
| Movie storage (10,200 titles)| 10,200 × 500 GB                                                       | ~5.1 PB         |
| TV show storage (6,800 titles)| 6,800 × 1.2 TB                                                        | ~8.2 PB         |
| Total encoded catalog        | 5.1 + 8.2 PB                                                          | ~13.3 PB        |
| Raw master file storage      | Same 17,000 titles × 1 TB avg (masters stored in Glacier)            | ~17 PB          |
| Thumbnail variants           | 17,000 titles × 100 artwork variants × 2 MB (A/B test artworks)      | ~3.4 TB         |
| User data (watch history)    | 300M users × 500 events/year × 200 bytes/event                        | ~30 TB/year     |
| Metadata (Cassandra)         | 300M users × 5 profiles × 1 KB profile state                          | ~1.5 TB         |

### Bandwidth Estimates

| Direction              | Calculation                                       | Result           |
|------------------------|---------------------------------------------------|------------------|
| Peak streaming egress  | 50M streams × 5 Mbps avg                         | 250 Tbps         |
| Average streaming egress| 11.25M × 5 Mbps                                  | ~56 Tbps         |
| Download (offline) egress| ~5% of DAU download daily; 135M × 5% × 1 GB file| ~63 Gbps avg    |
| Content ingestion      | ~5 new titles/day avg × 100 GB raw / 86,400      | ~5.8 Gbps raw   |
| OCA → edge sync        | New content pre-pushed to OCAs before release     | Varies by launch |

---

## 3. High-Level Architecture

```
                        ┌────────────────────────────────────────────┐
                        │               CLIENTS                       │
                        │  iOS  Android  Smart TV  Roku  PS5  Web    │
                        └──────────────────┬─────────────────────────┘
                                           │ HTTPS
                        ┌──────────────────▼─────────────────────────┐
                        │       AWS Global Accelerator / Route 53     │
                        │       (GeoDNS → nearest AWS region)        │
                        └──┬──────────────┬───────────────┬──────────┘
                           │              │               │
               ┌───────────▼──┐  ┌────────▼───┐  ┌───────▼────────┐
               │  API Gateway  │  │  Playback  │  │  Identity &   │
               │  (Zuul/Spring)│  │  API Svc   │  │  Auth Service  │
               └──────┬───────┘  └─────┬──────┘  └────────────────┘
                      │                │
          ┌───────────▼────────────┐   │
          │   Netflix Microservices │   │
          │  ┌─────────────────┐   │   │
          │  │  Catalog Svc    │   │   │
          │  │  (EVCache)      │   │   │
          │  └─────────────────┘   │   │
          │  ┌─────────────────┐   │   │
          │  │  Recommendation │   │   │
          │  │  Svc (ML)       │   │   │
          │  └─────────────────┘   │   │
          │  ┌─────────────────┐   │   │
          │  │  Search Service │   │   │
          │  │  (Elasticsearch)│   │   │
          │  └─────────────────┘   │   │
          │  ┌─────────────────┐   │   │
          │  │  User Profile   │   │   │
          │  │  Svc (Cassandra)│   │   │
          │  └─────────────────┘   │   │
          └────────────────────────┘   │
                                       │ Playback request
                        ┌──────────────▼──────────────────────────────┐
                        │         PLAYBACK SERVICE                     │
                        │  1. Auth check                               │
                        │  2. License validation (DRM)                 │
                        │  3. OCA selection (closest Open Connect)     │
                        │  4. Return OCA IP + signed manifest URL      │
                        └──────────────────────────────────────────────┘
                                                │
                                                │ Client connects directly
                        ┌───────────────────────▼────────────────────┐
                        │       OPEN CONNECT APPLIANCES (OCA)         │
                        │   ISP-embedded servers (~1000+ locations)   │
                        │   Store: HEVC/AV1 video + audio segments    │
                        │   Serve: DASH segments via HTTP/2           │
                        └────────────────────────────────────────────┘
                                                │
                        ┌───────────────────────▼────────────────────┐
                        │               CLIENT PLAYER                  │
                        │   ABR algorithm + DRM decryption            │
                        │   Nflx Playback Engine (cross-platform SDK) │
                        └────────────────────────────────────────────┘

CONTENT PIPELINE (asynchronous):
   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
   │ Master File  │→   │ AWS S3 Media │→   │ Encoding Farm│→   │  OCA Content │
   │ (camera raw) │    │ Asset Store  │    │ (Mezzanine→  │    │  Push (IPVS) │
   └──────────────┘    └──────────────┘    │  H.264/HEVC/ │    └──────────────┘
                                            │  AV1 DASH)   │
                                            └──────────────┘
```

**Component Roles:**

- **AWS Global Accelerator / Route 53**: GeoDNS routes API traffic to nearest AWS region (us-east-1, eu-west-1, ap-northeast-1 are primary). Anycast IP acceleration for TCP handshake latency reduction.
- **API Gateway (Zuul)**: Netflix's open-source dynamic routing gateway. Handles auth token validation, dynamic routing to microservices, A/B test assignment, request logging. ~100 billion API calls/day.
- **Playback API Service**: Core service that orchestrates playback start. Validates subscription tier, stream count limits, DRM license request, and steers client to the best OCA IP.
- **Open Connect Appliances (OCA)**: Netflix's proprietary CDN. Custom-built Linux servers deployed at ISP colos. 15+ TB NVMe SSD per appliance. Content pre-positioned nightly via BGP peering. 95%+ of traffic served from OCA, bypassing internet transit.
- **Catalog Service**: Stores metadata for all titles (title, genre, cast, description, ratings). Backed by MySQL + EVCache (Netflix's memcached-based distributed cache layer). Extremely read-heavy; catalog changes infrequently.
- **Recommendation Service**: Generates personalized rows for the home screen ("Continue Watching", "Because You Watched X", "Top 10"). ML inference served from TensorFlow Serving; batch results precomputed in Spark on AWS EMR.
- **Encoding Farm**: AWS EC2 instances running Netflix's Unified Streaming Platform (USP). Per-title encoding (not per-chunk like YouTube) using Netflix's per-scene bitrate optimization (described in Section 6.1).
- **Chaos Engineering**: Netflix Chaos Monkey / ChAP (Chaos Automation Platform) randomly terminates production instances to continuously validate fault tolerance.

**Primary Data Flow (Start Streaming a Movie):**

1. User clicks Play → POST `/api/v1/playback/start` to API Gateway.
2. API Gateway validates JWT, increments concurrent stream count (verified against tier limit in Redis), routes to Playback Service.
3. Playback Service: validates subscription, issues DRM license (Widevine/FairPlay/PlayReady), selects optimal OCA IP based on user geolocation + ISP + OCA health/load.
4. Returns manifest URL pointing to OCA (e.g., `https://oca-lon-isp1.nflxvideo.net/title_id/manifest.mpd`).
5. Client fetches DASH MPD manifest from OCA.
6. ABR player (MSE/EME in browser; Netflix SDK in app) selects initial bitrate based on throughput probe.
7. Fetches encrypted segments from OCA; DRM module decrypts using license obtained in step 3.
8. Playback begins. Heartbeats sent every 60 seconds to Playback Service for billing, telemetry, and stall detection.

---

## 4. Data Model

### Entities & Schema (Full SQL)

```sql
-- ─────────────────────────────────────────────
-- ACCOUNTS & PROFILES
-- ─────────────────────────────────────────────
CREATE TABLE accounts (
    account_id          UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    email               VARCHAR(255)    NOT NULL UNIQUE,
    hashed_password     VARCHAR(255)    NOT NULL,
    subscription_tier   VARCHAR(20)     NOT NULL DEFAULT 'standard'
                                        CHECK (subscription_tier IN ('basic','standard','premium','ads_supported')),
    max_streams         SMALLINT        NOT NULL DEFAULT 2,
    payment_method_id   VARCHAR(100),                      -- tokenized; actual data in payment vault
    subscription_status VARCHAR(20)     NOT NULL DEFAULT 'active'
                                        CHECK (subscription_status IN ('active','paused','cancelled','grace')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    billing_cycle_day   SMALLINT        CHECK (billing_cycle_day BETWEEN 1 AND 28),
    country_code        CHAR(2)         NOT NULL
);

CREATE TABLE profiles (
    profile_id          UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    display_name        VARCHAR(50)     NOT NULL,
    avatar_id           SMALLINT,
    is_kids_profile     BOOLEAN         NOT NULL DEFAULT FALSE,
    language            CHAR(5),                            -- BCP-47
    maturity_level      VARCHAR(20)     NOT NULL DEFAULT 'adult',
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT max_profiles_per_account CHECK (TRUE)        -- enforced in app layer (max 5)
);

CREATE INDEX idx_profiles_account ON profiles(account_id);

-- ─────────────────────────────────────────────
-- CONTENT CATALOG
-- ─────────────────────────────────────────────
CREATE TABLE titles (
    title_id            VARCHAR(20)     PRIMARY KEY,        -- Netflix internal ID (e.g. '70143836')
    title_type          VARCHAR(10)     NOT NULL CHECK (title_type IN ('movie','series','special')),
    original_title      VARCHAR(300)    NOT NULL,
    release_year        SMALLINT,
    rating              VARCHAR(10),                        -- 'PG-13', 'TV-MA', etc.
    duration_min        SMALLINT,                           -- for movies; NULL for series
    is_netflix_original BOOLEAN         NOT NULL DEFAULT FALSE,
    content_id_hash     VARCHAR(64),                        -- fingerprint for DRM
    status              VARCHAR(20)     NOT NULL DEFAULT 'available'
                                        CHECK (status IN ('available','coming_soon','expired','removed')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TABLE seasons (
    season_id           UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    title_id            VARCHAR(20)     NOT NULL REFERENCES titles(title_id),
    season_number       SMALLINT        NOT NULL,
    episode_count       SMALLINT,
    release_year        SMALLINT,
    UNIQUE (title_id, season_number)
);

CREATE TABLE episodes (
    episode_id          UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    season_id           UUID            NOT NULL REFERENCES seasons(season_id),
    title_id            VARCHAR(20)     NOT NULL REFERENCES titles(title_id),
    episode_number      SMALLINT        NOT NULL,
    title               VARCHAR(300)    NOT NULL,
    duration_min        SMALLINT        NOT NULL,
    synopsis            TEXT,
    UNIQUE (season_id, episode_number)
);

-- ─────────────────────────────────────────────
-- LOCALIZATION
-- ─────────────────────────────────────────────
CREATE TABLE title_localization (
    title_id            VARCHAR(20)     NOT NULL REFERENCES titles(title_id),
    language            CHAR(5)         NOT NULL,
    localized_title     VARCHAR(300),
    synopsis            TEXT,
    PRIMARY KEY (title_id, language)
);

-- ─────────────────────────────────────────────
-- GEO AVAILABILITY
-- ─────────────────────────────────────────────
CREATE TABLE title_availability (
    title_id            VARCHAR(20)     NOT NULL REFERENCES titles(title_id),
    country_code        CHAR(2)         NOT NULL,
    available_from      TIMESTAMPTZ,
    available_until     TIMESTAMPTZ,
    PRIMARY KEY (title_id, country_code)
);

CREATE INDEX idx_availability_country ON title_availability(country_code, available_from);

-- ─────────────────────────────────────────────
-- RENDITIONS
-- ─────────────────────────────────────────────
CREATE TABLE renditions (
    rendition_id        UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    title_id            VARCHAR(20)     NOT NULL REFERENCES titles(title_id),
    episode_id          UUID            REFERENCES episodes(episode_id),  -- NULL for movies
    resolution          VARCHAR(10)     NOT NULL,
    codec               VARCHAR(10)     NOT NULL CHECK (codec IN ('h264','hevc','av1','vp9')),
    bitrate_kbps        INTEGER         NOT NULL,
    hdr_format          VARCHAR(10),                        -- 'hdr10','dolby_vision','hlg', NULL
    dash_manifest_key   TEXT            NOT NULL,           -- S3 key for MPD manifest
    storage_bytes       BIGINT,
    encoded_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    UNIQUE (title_id, episode_id, resolution, codec)
);

-- ─────────────────────────────────────────────
-- WATCH HISTORY
-- ─────────────────────────────────────────────
CREATE TABLE watch_events (
    profile_id          UUID            NOT NULL,
    title_id            VARCHAR(20)     NOT NULL,
    episode_id          UUID,
    watched_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    resume_position_sec INTEGER         NOT NULL DEFAULT 0,
    completion_pct      SMALLINT        NOT NULL DEFAULT 0,
    device_type         VARCHAR(30),
    country_code        CHAR(2),
    PRIMARY KEY (profile_id, title_id, watched_at)
) PARTITION BY RANGE (watched_at);

-- ─────────────────────────────────────────────
-- RATINGS & MY LIST
-- ─────────────────────────────────────────────
CREATE TABLE ratings (
    profile_id          UUID            NOT NULL,
    title_id            VARCHAR(20)     NOT NULL,
    rating              SMALLINT        CHECK (rating IN (1, 2)),  -- 1=thumbs down, 2=thumbs up
    rated_at            TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (profile_id, title_id)
);

CREATE TABLE my_list (
    profile_id          UUID            NOT NULL,
    title_id            VARCHAR(20)     NOT NULL,
    added_at            TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (profile_id, title_id)
);

-- ─────────────────────────────────────────────
-- ACTIVE SESSIONS (stream concurrency control)
-- ─────────────────────────────────────────────
-- NOTE: In production this lives in Redis, not SQL, for sub-millisecond access.
-- Shown here for schema clarity.
CREATE TABLE active_streams (
    stream_id           UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(account_id),
    profile_id          UUID            NOT NULL,
    title_id            VARCHAR(20)     NOT NULL,
    device_fingerprint  VARCHAR(100),
    started_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    last_heartbeat_at   TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at          TIMESTAMPTZ     NOT NULL            -- = last_heartbeat + 90s
);

CREATE INDEX idx_active_streams_account ON active_streams(account_id, expires_at);
```

### Database Choice

| Database         | Pros                                                              | Cons                                                         |
|------------------|-------------------------------------------------------------------|--------------------------------------------------------------|
| MySQL (RDS)      | ACID, mature, good for relational data                            | Hard to scale writes; no built-in multi-region active-active |
| Cassandra        | Linear scale, multi-region active-active, tunable consistency     | No joins; denormalized schema; eventual consistency          |
| DynamoDB         | Managed, single-digit ms, multi-region (Global Tables)            | Expensive at Netflix scale; limited query patterns           |
| CockroachDB      | Distributed SQL, multi-region, ACID                               | Less battle-tested than Cassandra at Netflix scale           |
| EVCache          | Netflix's own Memcached wrapper, regional tiered cache            | Not a primary store; cache only                              |
| Elasticsearch    | Full-text search, facets, typo tolerance                          | Not a primary store; eventual consistency                    |

**Selected Architecture:**

- **Apache Cassandra** (primary data store for user-facing data): Netflix is one of the largest Cassandra users globally. `watch_events`, `ratings`, `my_list`, profile data stored in Cassandra. Justification: multi-region active-active topology (Netflix operates in us-east-1, eu-west-1, us-west-2 simultaneously); linear write scaling; built-in replication factor 3 across regions; eventual consistency is acceptable for watch history and ratings. Netflix's Cassandra deployment processes millions of reads/writes per second.
- **MySQL (via RDS Multi-AZ)**: `accounts`, `subscriptions`, payment references where strong consistency is required. Subscription state must be ACID — a user must not get duplicate charges.
- **EVCache** (Netflix's distributed Memcached layer): L2 cache for all catalog reads. Catalog data changes rarely (new titles added, not modified frequently). EVCache reduces Cassandra/MySQL read load by ~99%.
- **Elasticsearch**: Search index over title metadata, cast, crew, descriptions. Fed via Kafka CDC pipeline from Cassandra.
- **Cockroach DB / Spanner** (for A/B experiment assignments): Strong consistency needed for experiment assignment to prevent the same user getting different variants across regions.

---

## 5. API Design

All endpoints versioned under `/api/v1/`. OAuth 2.0 Bearer JWT. Rate limits enforced per `account_id`. Netflix internally uses gRPC between microservices; REST/HTTP for client-facing APIs.

### Playback

```
POST /api/v1/playback/start
Authorization: Bearer {token}
Content-Type: application/json

Request:
{
  "title_id": "70143836",
  "episode_id": null,
  "profile_id": "prof-uuid",
  "device_id": "device-fingerprint-hash",
  "drm_type": "widevine",
  "drm_key_request": "<base64 widevine PSSH>",
  "supported_codecs": ["hevc","h264","av1"],
  "supported_hdr": ["hdr10","dolby_vision"],
  "resume_position": true
}

Response 200:
{
  "stream_id": "stream-uuid",
  "manifest_url": "https://oca-lon3.nflxvideo.net/manifest/70143836/master.mpd?auth=signed-token",
  "manifest_type": "dash",
  "drm_license_url": "https://lic.nflxvideo.net/license",
  "drm_license_token": "eyJ...",
  "resume_position_sec": 3412,
  "oca_ip": "192.0.2.45",
  "heartbeat_interval_sec": 60,
  "heartbeat_url": "/api/v1/playback/{stream_id}/heartbeat"
}

Rate limit: 10 play starts/min per account
```

```
POST /api/v1/playback/{stream_id}/heartbeat
Authorization: Bearer {token}
Content-Type: application/json

Request:
{
  "position_sec": 3650,
  "bitrate_kbps": 8000,
  "rebuffer_count": 0,
  "rebuffer_duration_ms": 0,
  "stall_count": 0
}

Response 204

Rate limit: 1 req/60s per stream (server enforces interval)
```

```
POST /api/v1/playback/{stream_id}/stop
Authorization: Bearer {token}
Content-Type: application/json

{ "final_position_sec": 5220 }

Response 204
```

### Catalog & Discovery

```
GET /api/v1/catalog/home
Authorization: Bearer {token}
Headers: X-Profile-Id: {profile_id}

Response 200:
{
  "rows": [
    {
      "row_id": "continue_watching",
      "label": "Continue Watching",
      "items": [ {title objects with resume position} ],
      "next_cursor": null
    },
    {
      "row_id": "top_10_us",
      "label": "Top 10 in the U.S. Today",
      "items": [ {title objects with rank badge} ],
      "next_cursor": null
    },
    {
      "row_id": "reco_because_watched_70143836",
      "label": "Because you watched Inception",
      "items": [ {title objects} ],
      "next_cursor": "eyJ..."
    }
  ],
  "model_version": "v187",
  "experiment_ids": ["home_row_order_v3", "artwork_ab_v12"]
}

Rate limit: 60 req/min per profile
```

```
GET /api/v1/catalog/titles/{title_id}
Authorization: Bearer {token}
Headers: X-Profile-Id: {profile_id}, Accept-Language: en-US

Response 200:
{
  "title_id": "70143836",
  "type": "movie",
  "title": "Inception",
  "synopsis": "...",
  "rating": "PG-13",
  "duration_min": 148,
  "genres": ["Sci-Fi","Thriller"],
  "cast": [...],
  "directors": [...],
  "artwork_url": "https://art.nflximg.net/70143836/artwork_v42.webp",
  "trailers": [...],
  "is_in_my_list": false,
  "user_rating": null,
  "match_score": 97
}
```

### Search

```
GET /api/v1/search?q={query}&profile_id={id}&limit={limit}&offset={offset}
Authorization: Bearer {token}

Response 200:
{
  "results": [
    {
      "title_id": "...",
      "type": "movie",
      "title": "...",
      "relevance_score": 0.98,
      "match_reason": "title"
    }
  ],
  "total": 43,
  "suggestions": ["inception explained","inception ending"],
  "search_took_ms": 32
}

Rate limit: 30 req/min per profile
```

### User Actions

```
PUT /api/v1/profiles/{profile_id}/my-list/{title_id}
Response 204

DELETE /api/v1/profiles/{profile_id}/my-list/{title_id}
Response 204

PUT /api/v1/profiles/{profile_id}/ratings/{title_id}
Content-Type: application/json
{ "rating": 1 }    -- 1=thumbs_up, -1=thumbs_down
Response 204
```

---

## 6. Deep Dive: Core Components

### 6.1 Content Encoding — Per-Scene Bitrate Optimization (Netflix's "Per-Title Encoding")

**Problem it solves:**
Traditional encoding uses a fixed bitrate profile for all content (e.g., 1080p always at 5 Mbps). Action movies with high-motion scenes need more bits; static documentaries need fewer. Using a fixed profile wastes bandwidth for simple content and under-serves complex content. Netflix's per-title (and later per-scene) encoding assigns bits dynamically to maintain consistent visual quality (constant perceptual quality target).

**Possible Approaches:**

| Approach                            | Quality at given bitrate | Complexity        | Storage Impact |
|-------------------------------------|--------------------------|-------------------|----------------|
| Fixed bitrate profile (CBR)         | Inconsistent             | Very simple       | Wasteful        |
| Variable bitrate (CRF)              | More consistent          | Simple            | Better          |
| Per-title encoding (Netflix 2015)   | Optimized per title      | Moderate          | 20% reduction   |
| Per-shot/scene encoding (2016+)     | Optimal per scene        | High              | 30-40% reduction|
| ML-driven bitrate ladder            | Near-optimal             | Very high         | Best            |

**Selected Approach: Per-Scene Encoding with Dynamic Bitrate Ladder**

Netflix's approach (described in their 2015/2016 tech blog):

1. **Complexity Analysis**: For each title, a pre-processing pass measures spatial information (SI) and temporal information (TI) per scene using FFmpeg `signalstats` filter. A cartoon (low SI, low TI) needs far fewer bits than an action film (high SI, high TI).

2. **Convex Hull Optimization**: Netflix encodes each title at many different bitrate/resolution combinations (e.g., 20+ combinations). For each combination they compute VMAF (Video Multi-Method Assessment Fusion — Netflix's quality metric that correlates with human perception). They find the "convex hull" — the set of (bitrate, quality) pairs where no higher-quality encoding exists at the same bitrate with lower resolution.

3. **Per-Scene Dynamic Ladder**: Rather than one ladder per title, split the title into scenes (shot boundaries detected via FFmpeg scene filter). Each scene gets its own encoding parameters. A low-complexity scene (black screen, static shot) might be encoded at 480p/300kbps while matching the VMAF of a 1080p/5Mbps encoding. A high-motion chase scene gets more bits.

4. **Codec stack**:
   - H.264 (AVC): Universal baseline, for all devices.
   - HEVC (H.265): 40% better compression than H.264 at same quality. Used for 4K HDR content and as primary codec on modern TVs and mobile.
   - AV1: 30% better than HEVC. Netflix encodes AV1 for newer devices (those with hardware AV1 decoders: Chromecast 4K, Android 10+, newer Samsung TVs). AV1 encoding is 100× slower than H.264 CPU encoding — only economical at Netflix's scale.
   - Dolby Vision / HDR10: HDR metadata carried in HEVC container.

5. **VMAF Scoring**: Netflix's open-source VMAF (libvmaf) measures perceived quality on a 0-100 scale by combining visual features: detail (VIF), motion (MS-SSIM), noise (PSNR). Target VMAF = 93 for 1080p streaming. If a scene hits VMAF=93 at 2 Mbps instead of 5 Mbps, that's a 60% bandwidth saving for that scene.

**Implementation Detail:**
- Encoding pipeline runs on AWS EC2 c5.18xlarge instances (72 vCPU) with the Unified Streaming Pipeline (Netflix's internal framework).
- Encoding a 2-hour movie in all codecs/bitrates: ~10,000 CPU-hours → parallelized across hundreds of instances → completes in ~24 hours per title (new releases pre-encoded weeks before launch).
- VMAF target is tuned by content type: animated content targets lower VMAF (animation is forgiving) than live-action. This is configurable per title by content managers.
- Per-scene segments aligned to 4-second DASH segment boundaries to maintain ABR compatibility.

**Q&A:**

Q: How does per-scene encoding affect DASH manifest complexity?
A: The DASH MPD must reference the varying segment durations and bitrates correctly. Netflix uses a custom MPD generator that creates "Representation" elements with correct bandwidth values per segment group. The client ABR algorithm must handle variable-bitrate segments without assuming constant bitrate per quality level. Netflix's player implements this correctly; older DASH players that assume CBR segments would be confused.

Q: What is VMAF and why is it better than PSNR?
A: PSNR measures pixel-level difference between original and encoded frame. It correlates poorly with human perception — a blurry uniform background has high PSNR but looks terrible. VMAF uses a support vector machine trained on human opinion scores (MOS — Mean Opinion Score) from Netflix's video quality lab, combining multiple features: VIF (detail fidelity), ADM2 (block artifact detection), and motion compensation. VMAF scores 0.92 Pearson correlation with human MOS; PSNR scores ~0.70.

Q: How long does it take to encode a new season of a Netflix Original that releases all at once (e.g., Stranger Things: 8 episodes × 50 min)?
A: 8 episodes × 50 min = 400 min raw, plus 15+ codec/bitrate combinations × per-scene analysis = ~400,000 CPU-hours. Parallelized across 5,000 EC2 instances: 80 CPU-hours per instance → completes in ~80 hours wall time. Netflix pre-encodes original content weeks before premiere, so this timeline is fine. Launch-day encoding crisis only occurs if master files are delivered late by production.

Q: How do you handle encoding failures for mid-long tail content vs Originals?
A: For licensed content (mid-long tail), encoding errors put the title into a `processing_failed` state with a dead-letter queue for retry. Netflix Originals have dedicated encoding queues with priority, human monitoring, and on-call engineers. SLA for Originals: new content available for streaming within 24 hours of master file delivery. For licensed content with lower urgency: 72-hour SLA.

Q: How does Netflix decide which codec to use for a given device/user?
A: The Playback API queries the device capability registry (built from device registration telemetry) — every device reports its supported codecs via the initial app handshake. The Playback API selects the best codec the device supports (AV1 > HEVC > VP9 > H.264) and returns the corresponding manifest. Codec preference can also be A/B tested to measure whether AV1 saves enough bandwidth to be worth the slightly higher CPU decode load on older devices.

---

### 6.2 Open Connect CDN — Design and OCA Placement Strategy

**Problem it solves:**
At 250 Tbps peak egress, delivering content over the public internet via third-party CDN (Akamai, Cloudflare) would cost hundreds of millions of dollars per month and introduce latency variability. Netflix built Open Connect (OCA) — their own CDN — and deploys custom appliances inside ISPs' facilities. This eliminates inter-network transit, reduces latency (content is 1-2 hops from user), and gives Netflix full control over content placement and cache hit rates.

**Possible Approaches:**

| CDN Strategy                    | Cost           | Control | Latency    | ISP Relationships |
|---------------------------------|----------------|---------|------------|-------------------|
| Commercial CDN (Akamai/CF)      | Very high       | Low     | Moderate   | None              |
| Cloud CDN (CloudFront/CDN360)   | High            | Medium  | Moderate   | None              |
| Netflix Open Connect (ISP embed)| Medium (HW)     | Full    | Very low   | Strong (peering)  |
| Hybrid (OCA + fallback CDN)     | Moderate        | Full    | Low        | Strong            |

**Selected: Open Connect with ISP-embedded OCAs + AWS CloudFront fallback for cold content.**

**OCA Hardware Specification (as published by Netflix):**
- Custom 1U/2U Linux servers running FreeBSD (changed to Linux in later generations).
- Storage: 16–36 TB NVMe SSD (fast random read for segment serving).
- Network: 2 × 100GbE NICs for traffic; 1 × 10GbE for management.
- CPU: Dual Xeon, primarily for TLS termination and HTTP/2 multiplexing (not for compute).
- RAM: 256 GB (for NVMe page cache and TLS session storage).
- Software: nginx-based HTTP server with custom modules for Netflix content steering.

**Content Fill Strategy:**
1. Each night (off-peak hours), Netflix's Content Fill system analyzes view statistics by ISP/region.
2. Top-N most popular titles in that ISP's region (based on past 7-day view data) are pre-pushed to all OCAs in that ISP.
3. Fill uses BGP anycast routing within the Open Connect network — OCAs pull new content from Netflix S3 origin over dedicated 10/100G circuits (not public internet).
4. Less popular content (long-tail) is fetched on-demand from origin or a parent OCA cluster (hierarchical caching).
5. OCA-to-OCA: if an OCA is full and receives a request for uncached content, it can serve via a "sibling" OCA at the same ISP or fetch from Netflix's cloud clusters.

**OCA Selection Algorithm (Steering):**
When the Playback API selects an OCA for a user:
1. The user's ISP (from IP geolocation) maps to a set of candidate OCAs.
2. OCAs are filtered by: (a) has the requested title cached, (b) is healthy (heartbeat < 10s ago), (c) not overloaded (CPU < 70%, bandwidth headroom available).
3. From the healthy set, select the OCA with lowest RTT to the user (measured via periodic latency probes).
4. If no healthy ISP-embedded OCA has the content, fall back to nearest Open Connect IX cluster, then to AWS CloudFront origin pull.

**Q&A:**

Q: How does Netflix maintain OCA cache consistency — what if a content license expires and a title must be removed from OCAs globally?
A: Netflix's Clearance system sends a `DELETE` command to all OCAs via the management network. Each OCA deletes the title files from disk. The Playback API immediately stops routing requests for that title (database update). In-flight streams are allowed to complete (there's no way to interrupt mid-stream gracefully at scale). Complete propagation of a delete takes < 30 minutes across all OCAs. This is sufficient for content expiry, as licenses have multi-day notice.

Q: How many OCAs does Netflix operate and what is the total capacity?
A: Netflix has reported ~1,000+ ISP partners and deploys OCAs in over 1,000 locations globally. Each OCA stores 16-36 TB of content and can serve 40-100 Gbps of traffic. Total network capacity is in the tens of Tbps. Combined with the catalog structure (~13 PB encoded), a single OCA holds only a subset of the catalog — the most popular ~500 GB in that ISP's region. The 80/20 rule means the top 10% of titles (~1,700 titles) account for 80% of streams; these fit easily in 36 TB with all codec variants.

Q: What happens if an OCA hardware fails mid-stream for 10,000 users?
A: The Playback Service receives heartbeat failures from affected streams within 90 seconds. It issues new OCA steering decisions for affected sessions. Clients implement retry logic: on stream stall > 10 seconds, the client calls `/api/v1/playback/steer` to get a new OCA IP. The redirect is seamless from the user's perspective — the DASH player resumes from the last successfully buffered position. OCA health monitoring detects the failure and removes the dead OCA from steering decisions immediately.

Q: How does Open Connect handle ISPs that refuse to participate in the free peering program?
A: Netflix offers OCA hardware free to ISPs in exchange for peering (no transit cost). Some ISPs historically refused, creating congestion on Netflix traffic (throttling issue in 2014 US). Netflix uses commercial transit providers as fallback, routes through IX (internet exchange) points, and uses AWS CloudFront as last resort. Paid interconnects (like the Comcast deal Netflix signed in 2014) are used where free peering isn't available.

Q: How does the OCA network handle a new blockbuster release with millions of streams starting simultaneously?
A: Content is pre-positioned on all relevant OCAs before the release date. For a Netflix Original premiere (e.g., Stranger Things S4), all OCAs in every ISP globally receive the full season 48 hours before release via the nightly fill. On premiere night, 100% of requests hit warm cache — no origin fetch required. The main concern becomes OCA bandwidth saturation: Netflix monitors OCA bandwidth utilization in real-time and has spare OCAs on standby at IX clusters to absorb overflow.

---

### 6.3 A/B Testing at Scale (Netflix Experimentation Platform)

**Problem it solves:**
Netflix runs 1,000+ concurrent A/B experiments globally — from UI layout and artwork selection to recommendation algorithms and encoding parameters. With 300 million subscribers and a culture of data-driven decisions, every product change must be validated with statistical rigor. The experimentation platform must assign users to experiment variants deterministically, serve different experiences without latency, and compute statistically valid results on tens of billions of events.

**Possible Approaches:**

| Approach                           | Consistency    | Latency   | Analysis Power |
|------------------------------------|----------------|-----------|----------------|
| Client-side (cookie-based)         | Weak           | Zero      | Limited         |
| Server-side (DB lookup per request)| Strong         | +10-50ms  | Full            |
| Feature flag service (LaunchDarkly)| Strong         | +5-15ms   | Limited (vendor)|
| In-process hash assignment         | Deterministic  | ~1ms      | Full            |
| Centralized config + caching       | Strong + fast  | ~5ms      | Full            |

**Selected: Hash-based deterministic assignment cached in EVCache, analysis in Apache Spark.**

**Assignment:**
- Experiment config stored in Zookeeper (key: `experiments/{exp_id}/variants`, value: variant definitions with traffic allocation percentages).
- Assignment = `hash(profile_id + experiment_id) mod 100` → maps to variant bucket.
- This is computed inline in every API request (< 1ms) with no external call.
- Assignment is sticky: same profile always gets same variant (no variant switching mid-experiment).
- New experiments added to Zookeeper propagate to all API servers in < 5 seconds via Zookeeper watches.

**Experiment Layers:**
Experiments are organized in orthogonal layers (inspired by Google's Overlapping Experiment Infrastructure). Each layer controls a different product dimension (UI layout, algorithm, artwork). A user can be in one variant per layer simultaneously without interference, as long as layers are orthogonal.

**Event Collection:**
- Every API response includes `experiment_ids` and `variant_ids` in the response header / body.
- Client SDK logs exposure events (user saw this variant) and interaction events (click, play, scroll) with experiment context.
- Events streamed via Kafka → Apache Spark Streaming → written to S3 Parquet files.

**Analysis:**
- Metrics computed in Spark SQL on S3 Parquet: `AVG(completion_pct)` per variant, `COUNT(play_events)` per variant, `SUM(streaming_hours)` per variant.
- Statistical significance: Two-sample t-test for means; chi-squared for proportions. Netflix applies Benjamini-Hochberg correction for multiple hypothesis testing.
- CUPED (Controlled-experiment Using Pre-Experiment Data): reduces variance by subtracting pre-experiment metric from post-experiment metric — increases statistical power, allowing shorter experiment durations.
- Netflix typically runs experiments for 1-2 weeks minimum to account for day-of-week effects and novelty bias.

**Q&A:**

Q: How do you prevent experiment interference when a user is in 50 simultaneous experiments?
A: Orthogonal layering ensures that experiments in different layers don't interact. Two experiments on the same feature (e.g., two artwork experiments) would be in the same layer and therefore mutually exclusive. Netflix's experiment platform validates conflicts before launching new experiments. For interactions that are scientifically important (does algorithm A work better WITH UI B?), a factorial design experiment is launched deliberately.

Q: How does Netflix validate that the artwork recommendation model is actually better?
A: The artwork A/B test metric is "click-through rate + completion rate" (not just CTR, to avoid clickbait). Variant A: current artwork selection. Variant B: ML-selected artwork per user. After 2 weeks, Spark computes difference in composite metric with 95% confidence interval. If Variant B is superior and the confidence interval excludes zero, the model is shipped. Additionally, guardrail metrics (e.g., subscription cancellation rate) are monitored to ensure no negative side effects.

Q: How does Chaos Engineering relate to A/B testing?
A: Chaos Monkey (Netflix's open-source tool) terminates random production EC2 instances. This tests that the system gracefully handles failures. In the context of A/B testing: if the experiment assignment service dies, the system falls back to control variant (no experiment). This ensures that chaos events don't introduce confounds into experiment data. Chaos is run during business hours, not during experiments' critical measurement periods.

Q: How do you handle A/B test ramp-up for risky experiments (e.g., new recommendation algorithm)?
A: Progressive rollout: start at 0.1% traffic → monitor guardrail metrics (cancellation, support contacts) → expand to 1% → 10% → 50% → 100%. Automated rollback triggered if any guardrail metric degrades > 1σ within 1 hour of expansion. Netflix's experimentation platform integrates with PagerDuty for automatic rollback alerts.

Q: What is the difference between a user-level and device-level experiment?
A: User-level: the same variant is seen by the same profile regardless of device (e.g., recommendation algorithm). Device-level: variant is specific to a device type (e.g., encoding codec experiment, where you need to test on the exact device that will decode). Hash function changes: user-level uses `profile_id`, device-level uses `(profile_id, device_fingerprint)`. Device-level experiments must also account for device heterogeneity — an AV1 codec experiment can only run on devices that support AV1.

---

## 7. Scaling

### Horizontal Scaling

- **API Layer (Zuul gateways)**: Auto-scaled on AWS Auto Scaling Groups targeting 60% CPU. Thousands of instances across 3+ regions. Route 53 health checks remove unhealthy instances from rotation within 30s.
- **Microservices**: Each microservice deployed independently on AWS ECS/EC2 with separate auto-scaling policies. Netflix uses Titus (their internal container management platform) for container orchestration.
- **Cassandra**: Add nodes to ring; Cassandra rebalances automatically (virtual nodes/vnodes). Netflix runs 10,000+ Cassandra nodes globally. New nodes added with `nodetool bootstrap`.
- **Encoding Farm**: AWS EC2 Spot Instances (70% cheaper than on-demand). Spot interruption handling: encoding jobs are checkpointed every 30 minutes; on interruption, job resumes from checkpoint on new instance.

### Database Sharding

- **Cassandra**: Data partitioned by partition key. Watch events: partition key = `profile_id` (all events for a profile on same node). Subscriptions/account data: partition key = `account_id`. RF=3 across 3 AZs.
- **MySQL (accounts)**: Master-replica setup per region. Accounts table sharded by `account_id` mod N if write load requires it. Currently read replicas sufficient — account writes (subscription changes, payment updates) are low frequency.

### Replication

- **Cassandra**: Replication factor 3 per region; multi-region replication for geo-distributed active-active. Netflix uses LOCAL_QUORUM consistency for reads/writes (2/3 nodes within local region must acknowledge). Cross-region replication is asynchronous — acceptable for watch history.
- **EVCache**: Regional clusters in all 3 AWS regions. Cache is populated per-region on first miss; no cross-region cache replication (cost not justified vs. slightly higher miss rate during region failover).
- **S3 (content storage)**: Cross-region replication to 2 secondary regions. Combined with CloudFront, content is available globally even if primary S3 region fails.

### Caching Strategy

| Cache Layer         | Technology    | What is Cached                             | TTL         |
|---------------------|---------------|--------------------------------------------|-------------|
| API response cache  | EVCache       | Catalog metadata, localization             | 10 min      |
| Home screen rows    | EVCache       | Pre-computed per profile (stale OK)        | 5 min       |
| OCA content         | NVMe SSD      | Video segments (immutable)                 | Until evicted (LRU) |
| A/B assignments     | Local JVM     | Experiment config from Zookeeper           | 30 sec TTL  |
| DRM license cache   | Client-side   | License valid for content key              | 24 hours    |
| Active streams      | Redis         | Stream concurrency per account             | 90 sec TTL  |
| Search results      | EVCache       | Popular search queries                     | 5 min       |

**Interviewer Q&A:**

Q: How does Netflix handle cache stampede when a popular title's metadata expires from EVCache simultaneously across thousands of API servers?
A: Probabilistic early expiry (PER): instead of all caches expiring at the same TTL, each cache entry gets TTL = base_TTL + jitter(±20%). Additionally, EVCache uses "stale-while-revalidate": expired cache entries continue to be served while one background request refreshes the value. This means cache expiry never causes a thundering herd to the backend — only one refresh request is issued per cache key.

Q: Describe Netflix's approach to active-active multi-region architecture for Cassandra.
A: Netflix runs Cassandra in us-east-1, us-west-2, and eu-west-1 simultaneously. All three regions accept writes. Cassandra's multi-datacenter replication (NetworkTopologyStrategy, RF=3 per DC) replicates writes asynchronously across DCs. LOCAL_QUORUM consistency ensures writes complete without cross-region latency (2/3 nodes in local DC). Cross-region replication lag is typically < 100 ms. Conflict resolution uses last-write-wins (LWW) with client timestamps. For watch history (append-only), LWW is safe. For subscription status, MySQL (with strong consistency, single region primary) is used instead.

Q: How does Netflix handle the scenario where AWS us-east-1 (their largest region) goes completely down?
A: This is Netflix's most serious failure scenario. Pre-conditions: Cassandra data is fully replicated to us-west-2 and eu-west-1. EVCache caches are region-local (miss on failover is acceptable). Route 53 has health checks and failover routing configured. On us-east-1 failure: (1) Route 53 detects unhealthy endpoints in < 60 seconds, routes traffic to us-west-2 and eu-west-1. (2) API servers in us-west-2 and eu-west-1 absorb traffic (pre-scaled to handle 100% of global traffic; Netflix over-provisions). (3) MySQL failover: RDS Multi-AZ promotes replica in us-east-1b; if full region failure, Cross-Region Read Replica in us-west-2 is manually promoted (15-30 min RTO). (4) OCAs continue serving video — they are not AWS-dependent.

Q: How does Netflix serve recommendations at 8,250 QPS given that ML inference is expensive?
A: Netflix pre-computes recommendations for all active profiles in batch (Spark on EMR, running nightly on last 30 days of data). Results stored in Cassandra: `profile_id → [list of title_ids with scores]`. At query time, the Recommendation API reads pre-computed results from Cassandra via EVCache (< 5ms). Real-time signals (just-watched video, current session) are used for "Continue Watching" row (simple query) and "Because you watched" row (look up pre-computed candidates by just-watched title). Full real-time inference is only used for the "What to Watch Next" feature after a video ends (lower QPS, higher latency tolerance).

Q: What is Hystrix and how does Netflix use it?
A: Hystrix is Netflix's open-source latency and fault tolerance library (now in maintenance mode; succeeded by Resilience4j). Every service-to-service call is wrapped in a Hystrix command with: (1) Thread pool isolation — each downstream call gets a dedicated thread pool so a slow service can't exhaust the shared request thread pool. (2) Circuit breaker — opens after configurable failure rate, preventing calls to a degraded service. (3) Timeout — each call has a strict timeout (e.g., Recommendation Service: 200 ms; if exceeded, circuit opens). (4) Fallback — on failure/timeout, return a cached or degraded response (e.g., return trending titles instead of personalized). This ensures that a failure in the Recommendation Service doesn't take down the home screen.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario                        | Detection                                      | Mitigation                                                               | RTO       |
|-----------------------------------------|------------------------------------------------|--------------------------------------------------------------------------|-----------|
| OCA hardware failure mid-stream         | Heartbeat timeout (90s) + client stall         | Playback Service steers client to backup OCA; resume from buffer         | < 90 s    |
| Cassandra node failure                  | Gossip protocol (30s) + read/write errors       | Cassandra auto-heals; requests routed to RF=3 replicas                   | < 30 s    |
| AWS EC2 API server termination          | ELB health check (30s interval)                | Auto-scaling replaces instance; traffic rerouted                         | < 60 s    |
| Recommendation Service timeout          | Hystrix timeout (200ms)                         | Fallback: return cached home rows (stale-while-revalidate)               | Instant   |
| DRM License Service failure             | HTTP 5xx from license endpoint                 | Client retries 3× with exponential backoff; cached license if valid      | < 30 s    |
| Encoding failure (job crash)            | Missing completion message in SQS queue        | Dead-letter queue → retry up to 3×; alert on-call after 3 failures       | < 24 hr   |
| Regional AWS outage                     | Route 53 health checks + CloudWatch alarms      | Route 53 failover to another region; Cassandra cross-region replication  | < 5 min   |
| OCA fill failure (nightly sync fails)   | Fill completion check at 3 AM local time        | Alert on-call; manual re-trigger; OCAs serve cached content (no new content delivered until fixed) | 24 hr  |
| S3 outage (content origin)              | HTTP 503 from S3 endpoint                      | OCA NVMe cache serves existing content; CloudFront fallback for cache miss| Transparent |
| Subscription DB (MySQL) primary failure | RDS Enhanced Monitoring (< 30s detection)       | RDS Multi-AZ automatic failover to standby (60-120s)                    | 60-120 s  |

**Chaos Engineering (Netflix-specific):**

Netflix's Chaos Monkey randomly terminates EC2 instances in production during business hours. The Simian Army toolset includes:
- **Chaos Monkey**: Kills random EC2 instances.
- **Latency Monkey**: Injects artificial latency into service calls (tests Hystrix timeouts).
- **Conformity Monkey**: Checks instances comply with best practices (auto-scaling enabled, etc.).
- **Chaos Kong**: Simulates entire AWS region failure (run quarterly).
- **ChAP (Chaos Automation Platform)**: Runs automated failure injection experiments, measures impact against baseline, auto-rolls back if impact > threshold.

The philosophy: "If a system can't handle an instance dying, it will definitely fail in production unexpectedly. Better to discover this during business hours with engineers available."

**Idempotency & Retries:**
- All state-changing API calls include `Idempotency-Key: {uuid}` header.
- Playback start: idempotent — same `(profile_id, title_id, device_id)` within 5 minutes returns the same stream_id.
- Heartbeats: idempotent — duplicate heartbeats for same stream_id are no-ops.
- Cassandra writes: idempotent due to LWT (Lightweight Transactions) for critical operations; for watch events, natural idempotency via `(profile_id, title_id, watched_at)` primary key.

---

## 9. Monitoring & Observability

| Metric                              | Type      | Alert Threshold                 | Tool                    |
|-------------------------------------|-----------|---------------------------------|-------------------------|
| Playback start success rate         | Counter   | < 99.5% over 5 min              | Netflix Atlas           |
| TTFF (p50, p95, p99)                | Histogram | p99 > 2s for 3 min              | Atlas                   |
| Rebuffering ratio (rebuf / playtime)| Gauge     | > 0.5% sustained                | Atlas                   |
| OCA cache hit rate                  | Gauge     | < 90% per OCA                   | OCA Management System   |
| Cassandra read/write latency        | Histogram | p99 > 10 ms                     | Atlas                   |
| API error rate (5xx)                | Counter   | > 0.1% over 2 min               | Atlas                   |
| Active concurrent streams           | Gauge     | > 110% of expected peak         | Atlas (auto-scale)      |
| Encoding queue depth                | Gauge     | > 500 pending jobs              | SQS CloudWatch          |
| DRM license failure rate            | Counter   | > 0.01%                         | Atlas                   |
| Recommendation freshness            | Gauge     | > 30 min since last batch       | Spark monitoring        |
| A/B experiment coverage             | Gauge     | < 100% of expected traffic      | XP platform dashboard   |
| Subscription state inconsistency    | Counter   | Any non-zero                    | Atlas + PagerDuty       |

**Netflix Atlas:**
Netflix's in-house time-series monitoring system. Stores metrics in memory with efficient compression (step-aligned timestamps). Handles 1.3 billion metrics/minute. Query language (NFQL) supports arbitrary aggregations, percentile computation, and alerting. Unlike Prometheus, Atlas uses a push model (Spectator SDK in each service pushes to Atlas).

**Distributed Tracing:**
Netflix uses Zipkin (open-source, which Netflix contributed to). Every request tagged with `X-B3-TraceId`. Spans include service name, operation, duration, error flags. Zipkin server aggregates traces. Sampling: 0.1% normal traffic, 100% errors, 10% slow requests (> 1s). Netflix's "Edgar" tool provides end-to-end playback session tracing — a support agent can look up any stream ID and see every service call that occurred.

**Logging:**
- Structured JSON via log4j → Kafka → S3 → Elasticsearch (Kibana dashboards).
- Real-time log alerting via Kibana Watcher for `level=ERROR AND service=playback-api`.
- Log retention: 7 days in Elasticsearch hot tier, 90 days in warm tier, 1 year in S3.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                              | Choice Made                            | Alternative Considered            | Trade-off                                                              |
|---------------------------------------|----------------------------------------|-----------------------------------|------------------------------------------------------------------------|
| CDN: Own (Open Connect) vs commercial | Own Open Connect                       | Akamai / Cloudflare               | +Cost control, +Latency, +Control. −Capital cost, −Ops complexity     |
| Database: Cassandra vs relational     | Cassandra (user data) + MySQL (billing)| Pure PostgreSQL                   | +Write scale, +Multi-region active-active. −No joins, −Eventual consistency |
| Encoding: Per-scene vs fixed          | Per-scene dynamic ladder               | Fixed bitrate profiles             | +30% bandwidth savings. −Encoding complexity, −Longer processing time  |
| Codec: HEVC + AV1                     | Multi-codec support                    | H.264 only                        | +40% bandwidth savings. −Device compatibility matrix, −Encoding cost   |
| Recommendations: Batch vs real-time   | Batch precompute + real-time signals   | Full real-time inference           | +Low latency (< 5ms serve). −Stale personalization (6hr lag)           |
| Chaos Engineering: Prod failures      | Chaos Monkey in production             | Staging environment only          | +Genuine resilience testing. −Risk of real user impact (mitigated)     |
| A/B testing: Hash-based inline        | Deterministic hash, no network call    | Centralized feature flag service  | +Zero latency overhead. −Config propagation delay (Zookeeper watches)  |
| Multi-region: Active-active           | Active-active (Cassandra + Route53)    | Active-passive                    | +No failover delay, +No data loss. −Conflict resolution complexity     |
| DRM: Multi-DRM                        | Widevine + FairPlay + PlayReady        | Single DRM vendor                 | +Universal device support. −License management complexity              |
| Experiment analysis: Spark batch      | Nightly Spark batch on S3              | Real-time streaming analytics     | +Cost-effective, +Correct (full dataset). −Results available next day  |

---

## 11. Follow-up Interview Questions

**Q1: How does Netflix handle password sharing crackdowns technically?**
A: Netflix's paid sharing model (2023) uses device fingerprinting, IP address history, and WiFi network SSID to determine a user's "primary location." The system builds a trusted device list per account based on consistent usage patterns. When a device logs in from a novel location not seen in historical data, it may be flagged and required to verify via email or SMS, or charged for an "extra member" add-on. Technically: device signals are sent with every API call, aggregated in Flink streaming jobs, and compared against a user's location model stored in Cassandra. This is essentially anomaly detection on behavioral signals.

**Q2: How does Netflix implement offline downloads with DRM protection?**
A: Downloads use Widevine offline license: client requests an "offline license" for specific title/resolution. Widevine offline license contains the content key, bound to the specific device's hardware-backed keystore (TEE/TrustZone). Download is a standard DASH segment fetch (same OCA), stored encrypted on device storage. The license has an expiry (typically 30 days since download, 48 hours since first play). On playback, Widevine decrypts using the stored offline license. If license expires, content is unplayable even if segments are still on disk. Netflix sets per-title download limits (e.g., some titles: max 25 downloads per account).

**Q3: How does Netflix's personalized artwork system work?**
A: For each title, Netflix's Content team creates 5-30 different artwork variants (different character focus, mood, composition). A multi-armed bandit algorithm (Thompson Sampling) serves different artwork to different users and measures "click + play" conversion rate. The algorithm converges on the best artwork per user segment (e.g., action movie fans see different Stranger Things artwork than comedy fans). This is implemented as a separate A/B experiment layer (artwork selection is orthogonal to algorithm experiments). Artwork is stored in S3, served via CDN with `Cache-Control: no-cache` (personalized per user, so no CDN caching of the image URL decision — just the image file itself is cached).

**Q4: How do you design the "Continue Watching" feature that resumes exactly where you left off across devices?**
A: Resume position written to Cassandra on every heartbeat (every 60s) and on stop event. Data model: `profile_id` as partition key, `title_id` as clustering key, `resume_position_sec` and `last_watched_at` as columns. On playback start, Playback API fetches last resume position from Cassandra (low latency: EVCache hit typical). Write path: heartbeat → POST to Playback Service → async write to Cassandra via event queue. The 60s heartbeat granularity means worst case 60s lost position on crash. For episode completion detection: if `completion_pct > 90%`, mark episode as watched and advance `next_episode_id`.

**Q5: How does Netflix implement Dolby Atmos and spatial audio delivery?**
A: Dolby Atmos audio is encoded separately from video as an additional audio track in the DASH manifest. The audio track uses Dolby Digital Plus (EC-3) container with Atmos metadata. In the MPD, separate `<AdaptationSet>` elements for Stereo (AAC), 5.1 (AC-3), and Atmos (EC-3 + Atmos). The player selects audio track based on device capability (queried at registration). Atmos track is ~640 Kbps vs 192 Kbps for stereo AAC — approximately 3× bandwidth for audio. Netflix's encoding pipeline includes Dolby's Atmos mastering tools in the encoding farm alongside FFmpeg.

**Q6: How does Netflix ensure global content availability on premiere night (e.g., all regions simultaneously)?**
A: Coordinated pre-positioning: fill is triggered 48 hours before release. The OCA fill system uses a global fanout: all OCAs in all ISPs in all regions begin pulling content simultaneously from Netflix S3 origin over dedicated circuits. Given OCAs have dedicated pull bandwidth, a 50 GB episode can be pushed to 1,000 OCAs in ~1 hour over 100 Mbps links. Netflix monitors fill completion per OCA and alerts if any OCA hasn't completed fill 4 hours before premiere. Premiere releases also have a "soft launch" check: Netflix does a 10-minute internal accessibility check before the public premiere time.

**Q7: What is VMAF and how does it differ from SSIM?**
A: VMAF (Video Multi-Method Assessment Fusion) is Netflix's open-source video quality metric. SSIM (Structural Similarity Index) measures structural luminance and contrast similarity between original and compressed frames — good for uniform distortions but poor at modeling human perception of complex scenes. VMAF uses a support vector machine trained on human opinion scores (MOS), combining: VIF (Visual Information Fidelity — models detail), ADM2 (Anti-blocking measure for DCT artifacts), and motion model. VMAF scores 0.92 Pearson correlation with human MOS vs ~0.70 for SSIM. Netflix uses VMAF as the objective function for their per-title encoding optimization.

**Q8: How does Netflix handle content availability in regions with slow internet (e.g., Indonesia, Nigeria)?**
A: Netflix offers a "Save Data" mode (0.3 GB/hour, ~560 Kbps) for mobile. Low-bandwidth encoding profiles are included in all title manifests. OCAs are deployed at ISPs in key developing markets. Netflix also partners with telcos for "zero-rating" plans (Netflix traffic doesn't count against data cap) to drive adoption. For 2G/edge connections (< 200 Kbps), Netflix has a mobile-only low-definition plan at 480p maximum (removed in some markets but still relevant in others). Netflix's video player includes bandwidth estimation that can select 240p (< 300 Kbps) when needed.

**Q9: How does Netflix measure and reduce time-to-first-frame (TTFF)?**
A: TTFF = time from user click to first rendered frame. Components: (1) Auth token validation (~10 ms). (2) Playback API call including OCA selection (~50 ms). (3) DRM license fetch (~80 ms). (4) DASH manifest fetch from OCA (~20 ms RTT). (5) First segment fetch (~100-300 ms). Total: ~260-460 ms ideal. Netflix optimizations: (a) Pre-fetch: when user hovers over a title, pre-fetch manifest (speculative, cancelled if user navigates away). (b) DRM license pre-fetch for titles in "Continue Watching" row. (c) HTTP/2 connection pre-established to OCA. (d) Initial segment (first 2s) pushed via HTTP/2 server push in some implementations. Result: median TTFF < 500 ms on good connections.

**Q10: How does Netflix test new recommendation models without impacting user experience?**
A: Shadow mode testing: new model runs in parallel with production, receiving the same inputs, but its outputs are discarded (not shown to users). Model outputs are logged and compared against actual user behavior that resulted from the production model. This allows offline validation before any A/B test. After shadow validation, the model enters a 0.1% A/B test with automatic guardrail monitoring. Netflix also uses counterfactual evaluation: given historical data, simulate what the metric would have been if the new model had made different recommendations (corrected for selection bias using Inverse Propensity Scoring).

**Q11: How do you prevent multiple charges if a user clicks "Subscribe" twice?**
A: Idempotency key in payment API calls (UUID generated client-side, stored server-side for 24 hours). Subscription creation in MySQL uses a UNIQUE constraint on `(email, payment_method_id)` for the active subscription state. The payment processor (Stripe/Braintree) also has idempotency keys at the API level. If two concurrent subscription requests arrive with the same idempotency key, the second is a no-op returning the first request's response. MySQL transaction + SELECT FOR UPDATE on the account row prevents concurrent writes.

**Q12: How does Netflix's search handle typos and partial matches in 30+ languages?**
A: Elasticsearch provides: (1) `fuzzy` matching (Levenshtein distance 1-2 for typos). (2) `phrase_prefix` for autocomplete. (3) Language-specific analyzers: `english` analyzer for stemming (watching→watch), `icu_analyzer` for CJK (Chinese/Japanese/Korean tokenization), `arabic` analyzer for root extraction. Search index is per-language or multilingual depending on title availability in that region. Phonetic matching (`phonetic` analyzer with Beider-Morse encoding) handles transliteration searches (Japanese title searched in romanized form). Netflix also uses query understanding ML models to expand queries ("movies like Inception" → genre+mood entity extraction).

**Q13: How do you handle a security breach where content DRM keys are compromised?**
A: DRM architecture uses content key rotation: each title has a unique content key in the Widevine/FairPlay KMS (Key Management Service). If keys are compromised: (1) Immediate: revoke affected license tokens (license service stops issuing new tokens). (2) Re-key: generate new content keys, re-encrypt all content segments. (3) Re-encode or re-encrypt: segments can be re-encrypted (just swap encryption without re-encoding video data) — faster. Re-encryption of a 2-hour movie: ~1 hour. OCA fill then pushes re-encrypted segments globally. Users who cached old segments see playback failure on next license renewal — they re-download new encrypted segments transparently. In practice, DRM key compromise is catastrophic and rare; defense-in-depth prevents it.

**Q14: Explain Netflix's "microservices" architecture and why it differs from YouTube's monolith/semi-monolith.**
A: Netflix broke its monolith in 2009-2011, migrating to AWS. Today Netflix has hundreds of microservices: each service owns its own data store, deployment lifecycle, and scaling. Services communicate via REST/gRPC. This enables: independent deployability (200 deploys/day), technology heterogeneity (each team chooses their stack), and fault isolation (failure in Recommendation Service doesn't cascade to Playback). The cost: distributed systems complexity, network latency between services, distributed tracing overhead, and the need for API contracts. Netflix partially addresses this complexity with Zuul (routing), Hystrix (resilience), Eureka (service discovery), and Ribbon (client-side load balancing) — all open-sourced as the Netflix OSS stack.

**Q15: How does Netflix handle the cold-start problem for brand new subscribers?**
A: New subscribers have no watch history. During onboarding: (1) Genre preference selection (multi-select screen: "What do you like?"). (2) Explicit title ratings (optional: "Rate titles to improve recommendations"). These signals immediately populate the recommendation model's warm start. Without any input: Netflix defaults to regionally popular content (Top 10 in your country) plus demographically similar users' top titles (collaborative filtering on age/gender signals from registration). After 3 viewing sessions, the model has enough implicit signal to become personalized. Netflix's data shows personalization quality reaches 80% of "mature user" quality after just 5 hours of viewing.

---

## 12. References & Further Reading

1. **Netflix Open Connect** — https://openconnect.netflix.com/Open-Connect-Overview.pdf
2. **Netflix Per-Title Encoding** — Ronny Rondén (2015). "Per-title encode optimization." Netflix Tech Blog. https://netflixtechblog.com/per-title-encode-optimization-7e99442b62a2
3. **Netflix VMAF** — Li, Z., et al. "VMAF: The Journey Continues." Netflix Tech Blog. https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12
4. **Netflix Cassandra Usage** — https://netflixtechblog.com/tagged/cassandra
5. **Netflix Chaos Monkey** — https://github.com/Netflix/chaosmonkey; Basiri, A., et al. (2016). "Chaos Engineering." IEEE Software.
6. **Netflix Zuul 2** — https://netflixtechblog.com/zuul-2-the-netflix-journey-to-asynchronous-non-blocking-systems-45947377fb5c
7. **Netflix Hystrix** — https://github.com/Netflix/Hystrix/wiki
8. **Widevine DRM** — Google. https://www.widevine.com/
9. **MPEG-DASH Standard** — ISO/IEC 23009-1:2022.
10. **Netflix A/B Testing** — Kohavi, R., Longbotham, R. "Online Controlled Experiments at Large Scale." KDD 2013. (Basis for Netflix's XP platform philosophy).
11. **CUPED** — Deng, A., et al. (2013). "Improving the Sensitivity of Online Controlled Experiments by Utilizing Pre-Experiment Data." WSDM 2013. https://exp-platform.com/Documents/2013-02-CUPED-ImprovingSensitivityOfControlledExperiments.pdf
12. **Netflix Atlas Monitoring** — https://netflixtechblog.com/introducing-atlas-netflixs-primary-telemetry-platform-bd31f4d8ed9a
13. **Netflix Titus (Container Management)** — https://netflixtechblog.com/titus-the-netflix-container-management-platform-is-now-open-source-f868c9fb5436
14. **Overlapping Experiments** — Tang, D., et al. (2010). "Overlapping Experiment Infrastructure: More, Better, Faster Experimentation." KDD 2010. https://dl.acm.org/doi/10.1145/1835804.1835810
15. **Netflix EVCache** — https://github.com/Netflix/EVCache
