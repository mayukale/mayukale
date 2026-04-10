# System Design: Video Upload Pipeline

---

## 1. Requirement Clarifications

### Functional Requirements

1. Users can upload video files up to 50 GB and 12 hours in length.
2. The system supports resumable uploads — a failed upload can be resumed from the last successfully uploaded byte without restarting.
3. Multi-part uploads: large files are split into chunks (e.g., 5 MB chunks) uploaded in parallel or sequentially.
4. Upon upload completion, the video is transcoded into multiple resolutions and bitrates (e.g., 2160p/4K, 1080p, 720p, 480p, 360p, 240p, 144p).
5. Thumbnails are automatically generated (multiple candidate frames) and optionally uploaded by the creator.
6. Metadata is extracted from the video file: duration, resolution, frame rate, codec, audio channels, file format.
7. The video is protected by DRM (Digital Rights Management) — encrypted with per-video content keys; licenses issued at playback.
8. Content moderation: automated scanning for CSAM, violence, hate speech via fingerprint matching and ML classifiers.
9. After processing, the video is published and playable via HLS/DASH adaptive streaming.
10. Creators receive status updates (email + in-app notification) at each pipeline stage.

### Non-Functional Requirements

1. Availability: 99.9% uptime for the upload API (≤ 8.7 hours downtime/year). Processing pipeline can tolerate brief outages with queued retry.
2. Durability: Once a chunk is acknowledged by the server, it must never be lost (S3 11-nines durability).
3. Scalability: Handle 100 concurrent large uploads + 5,000 smaller uploads per second at peak.
4. Latency:
   - Upload initiation: < 500 ms.
   - Chunk acknowledgement: < 1,000 ms per chunk (5 MB chunk at 100 Mbps ≈ 400 ms upload; server ACK within 600 ms).
   - Processing pipeline: video available for streaming within 30 minutes of upload completion for standard quality; full ladder within 60 minutes.
5. Idempotency: Retrying any operation (chunk upload, metadata submission, DRM key request) must be safe.
6. Security: Upload URLs are pre-signed with expiry. Stream URLs are signed. DRM content keys are never exposed in transit without encryption.

### Out of Scope

- Player SDK and adaptive bitrate client implementation.
- Ad insertion and monetization pipeline.
- Recommendation engine that uses the uploaded content.
- Creator Studio UI (covered by Video Metadata service, not the pipeline itself).
- Live streaming ingest (covered in Twitch design).
- Search indexing post-upload (downstream consumer of upload pipeline events).

---

## 2. Users & Scale

### User Types

| User Type          | Description                                               | Upload Behavior                               |
|--------------------|-----------------------------------------------------------|-----------------------------------------------|
| Consumer Creator   | Casual uploader (social platform)                         | Short clips 1-10 min, < 1 GB, frequent        |
| Professional Creator | YouTuber-style, monetized channel                      | 10-60 min, 1-10 GB, several per week          |
| Enterprise Uploader | Media company, educational platform                    | Full movies/courses, 10-50 GB, bulk uploads   |
| Internal (CMS)     | Internal content management system                        | Programmatic, high volume, via API            |

### Traffic Estimates

**Assumptions (modeling a YouTube-scale platform):**
- 500 hours of video uploaded per minute (YouTube's public stat).
- Average video length: 7 minutes.
- Average source file size: 2 GB (7 min × ~38 Mbps raw 1080p, compressed source).
- Average chunk size for upload: 5 MB.
- Peak uploads: 3× average (event-driven, e.g., after a major news event).

| Metric                           | Calculation                                                         | Result             |
|----------------------------------|---------------------------------------------------------------------|--------------------|
| Uploads per minute               | 500 hr/min / (7 min avg) × 60 sec                                  | ~4,286 uploads/min |
| Uploads per second (avg)         | 4,286 / 60                                                          | ~71 uploads/sec    |
| Uploads per second (peak)        | 71 × 3                                                              | ~214 uploads/sec   |
| Chunk upload requests/sec (avg)  | 71 uploads/sec × (2 GB / 5 MB chunks) = 71 × 400 chunks            | ~28,400 req/sec    |
| Chunk upload requests/sec (peak) | 28,400 × 3                                                          | ~85,200 req/sec    |
| Upload bandwidth ingress (avg)   | 71 × 2 GB / (avg 10 min upload time) = 71 × 200 KB/s             | ~14.2 GB/s = ~114 Gbps |
| Transcoding jobs/sec             | 71 uploads/sec × 7 quality levels                                   | ~497 transcode jobs/sec |
| Thumbnail extraction events/sec  | 71 uploads/sec                                                      | 71/sec             |
| DRM key generation events/sec    | 71 uploads/sec                                                      | 71/sec             |

### Latency Requirements

| Operation                       | Target p50    | Target p99    | Notes                                         |
|---------------------------------|---------------|---------------|-----------------------------------------------|
| Upload session initiation       | < 100 ms      | < 500 ms      | Returns upload URL + session token            |
| Chunk upload acknowledgement    | < 200 ms      | < 1,000 ms    | After chunk received (not persisted to S3 yet)|
| Full pipeline completion (720p) | < 10 min      | < 30 min      | Depends on video length; 7-min video target   |
| Full pipeline (all qualities)   | < 20 min      | < 60 min      | Including 4K if source supports it            |
| Thumbnail ready                 | < 5 min       | < 15 min      | Auto-generated thumbnails                     |
| DRM license issue (playback)    | < 50 ms       | < 200 ms      | At playback time; not in upload pipeline      |
| Content moderation verdict      | < 5 min       | < 30 min      | Fingerprint check < 30s; ML scan < 30 min     |

### Storage Estimates

| Category                       | Calculation                                              | Result            |
|--------------------------------|----------------------------------------------------------|-------------------|
| Raw upload storage per day     | 71 uploads/sec × 86,400 × 2 GB                          | ~12.3 PB/day      |
| Raw retention (processing + 30d)| 12.3 PB/day × 31 days                                   | ~381 PB raw       |
| Transcoded storage per day     | 71/sec × 86,400 × 1.6 GB (8 qualities × 200 MB avg)    | ~9.8 PB/day       |
| Metadata storage per day       | 71/sec × 86,400 × 2 KB avg                              | ~12.3 GB/day      |
| Thumbnail storage per day      | 71/sec × 86,400 × 3 thumbs × 200 KB                    | ~3.7 TB/day       |
| DRM key material per day       | 71/sec × 86,400 × 256 bytes/key × 8 quality keys        | ~1.4 GB/day       |
| Upload session state (Redis)   | 71/sec in-flight, avg 10 min active = 71×600 = ~42,600 sessions × 1 KB | ~43 MB active  |

### Bandwidth Estimates

| Direction                    | Calculation                                   | Result        |
|------------------------------|-----------------------------------------------|---------------|
| Upload ingress (avg)         | 71 uploads/sec × 200 KB/s per upload (2GB/10min) | ~14 GB/s = 114 Gbps |
| Upload ingress (peak)        | 114 Gbps × 3                                 | ~342 Gbps     |
| Egress (processed to CDN)    | 71/sec × 1.6 GB / avg 30 min spread          | ~64 Gbps      |

---

## 3. High-Level Architecture

```
                         ┌────────────────────────────────────────────────┐
                         │                 UPLOADER CLIENT                  │
                         │  Browser / Mobile App / API Client               │
                         │  Splits file into 5MB chunks                    │
                         │  Computes SHA-256 per chunk for integrity        │
                         └──────────────────────┬─────────────────────────┘
                                                │
                    ┌───────────────────────────▼──────────────────────────────┐
                    │               UPLOAD API SERVICE                          │
                    │  POST /uploads/initiate  → session token + upload URL    │
                    │  PUT  /uploads/{session}/chunks/{n} → chunk ACK          │
                    │  POST /uploads/{session}/complete  → trigger pipeline    │
                    └──────────┬──────────────────────────────────────────────┘
                               │
          ┌────────────────────┼──────────────────────────────────────────────┐
          │                    │                                               │
          ▼                    ▼                                               ▼
  ┌──────────────┐   ┌──────────────────────┐                    ┌─────────────────────┐
  │  Upload      │   │  Upload Session       │                    │  Object Store       │
  │  Session DB  │   │  State (Redis)        │                    │  (S3-compatible)    │
  │  (Postgres)  │   │  - Received bytes     │                    │  Raw chunks stored  │
  └──────────────┘   │  - Chunk status map   │                    │  in staging prefix  │
                     │  - Upload metadata    │                    └────────┬────────────┘
                     └──────────────────────┘                             │
                                                                           │ completion event
                    ┌──────────────────────────────────────────────────────▼──────────────────────┐
                    │                         MESSAGE QUEUE (SQS / Kafka)                          │
                    │  Topic: video-uploaded  →  payload: {video_id, raw_s3_key, metadata}        │
                    └──────────────────────────┬─────────────────────────────────────────────────┘
                                               │
          ┌────────────────────────────────────┼─────────────────────────────────────────────────┐
          │                                    │                                                   │
          ▼                                    ▼                                                   ▼
┌──────────────────────┐         ┌─────────────────────────┐                    ┌────────────────────────────┐
│  METADATA EXTRACTOR  │         │  CONTENT MODERATION      │                    │  DRM KEY PROVISIONER       │
│  (ffprobe)           │         │  SERVICE                 │                    │  - Generate content key    │
│  - Duration          │         │  - CSAM hash check       │                    │  - Store in KMS            │
│  - Resolution        │         │    (PhotoDNA/NCMEC)      │                    │  - Register with Widevine  │
│  - Codec, FPS        │         │  - Audio fingerprint     │                    │    / FairPlay KMS          │
│  - Audio channels    │         │    (DMCA check)          │                    └────────────┬───────────────┘
│  - Bitrate           │         │  - ML violence/hate      │                                 │
│  Writes to: PostgreSQL│         │    classifier (async)    │                                 │
└──────────────────────┘         │  Verdict → Kafka topic   │                                 │
                                 └─────────────────────────┘                                 │
                                                                                              │
                    ┌─────────────────────────────────────────────────────────────────────────▼────┐
                    │                     TRANSCODING PIPELINE                                       │
                    │                                                                                │
                    │  ┌────────────────────────────────────────────────────────────────────────┐  │
                    │  │  SEGMENT SPLITTER                                                       │  │
                    │  │  - Splits raw file into N × 30-second GOP-aligned segments              │  │
                    │  │  - Publishes N × Q transcode jobs to Transcode Job Queue               │  │
                    │  └──────────────────────────────────────────────────────────┬─────────────┘  │
                    │                                                              │ N×Q jobs        │
                    │  ┌───────────────────────────────────────────────────────────▼────────────┐  │
                    │  │  TRANSCODER WORKERS (K8s GPU Jobs)                                      │  │
                    │  │  - Pull job: {video_id, segment_key, quality_level}                    │  │
                    │  │  - Run FFmpeg with NVENC                                               │  │
                    │  │  - Write output to S3 temp prefix                                      │  │
                    │  │  - Emit completion event                                               │  │
                    │  └──────────────────────────────────────────────────────────┬─────────────┘  │
                    │                                                              │ all done         │
                    │  ┌───────────────────────────────────────────────────────────▼────────────┐  │
                    │  │  MANIFEST ASSEMBLER                                                     │  │
                    │  │  - Stitches segments into final HLS/DASH manifests                    │  │
                    │  │  - Encrypts segments with DRM content key (CENC)                      │  │
                    │  │  - Writes final segments + manifests to permanent S3 prefix           │  │
                    │  │  - Emits video-ready event                                            │  │
                    │  └──────────────────────────────────────────────────────────┬─────────────┘  │
                    └─────────────────────────────────────────────────────────────┼────────────────┘
                                                                                  │
          ┌───────────────────────────────────────────────────────────────────────▼────────────────┐
          │                              POST-PROCESSING                                             │
          │  ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────────────────┐  │
          │  │  THUMBNAIL GENERATOR │  │  CAPTION GENERATOR   │  │  SEARCH INDEXER              │  │
          │  │  (ffmpeg -vframes 1) │  │  (Speech-to-Text API)│  │  (Elasticsearch index update)|  │
          │  └──────────────────────┘  └──────────────────────┘  └──────────────────────────────┘  │
          └────────────────────────────────────────────────────────────────────────────────────────┘
                                                │
          ┌─────────────────────────────────────▼──────────────────────────────────────────────────┐
          │                       STATUS UPDATE & NOTIFICATION                                       │
          │  - Update video status to 'published' in PostgreSQL                                     │
          │  - Send email / push notification to creator                                            │
          └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Data Model

### Entities & Schema (Full SQL)

```sql
-- ─────────────────────────────────────────────
-- UPLOAD SESSIONS
-- ─────────────────────────────────────────────
CREATE TABLE upload_sessions (
    session_id          UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    video_id            VARCHAR(20)     NOT NULL,           -- assigned at initiation
    user_id             UUID            NOT NULL,
    filename            VARCHAR(500)    NOT NULL,
    content_type        VARCHAR(100)    NOT NULL DEFAULT 'video/mp4',
    total_size_bytes    BIGINT          NOT NULL,
    received_bytes      BIGINT          NOT NULL DEFAULT 0,
    chunk_size_bytes    INTEGER         NOT NULL DEFAULT 5242880,  -- 5 MB default
    total_chunks        INTEGER         NOT NULL,
    received_chunks     INTEGER         NOT NULL DEFAULT 0,
    raw_storage_prefix  TEXT            NOT NULL,           -- S3 prefix for chunks
    status              VARCHAR(20)     NOT NULL DEFAULT 'in_progress'
                                        CHECK (status IN ('in_progress','assembling','complete','failed','expired')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW() + INTERVAL '24 hours',
    client_ip           INET,
    user_agent          TEXT,
    checksum_sha256     CHAR(64)                            -- optional: full file checksum provided by client
);

CREATE INDEX idx_sessions_video  ON upload_sessions(video_id);
CREATE INDEX idx_sessions_user   ON upload_sessions(user_id, created_at DESC);
CREATE INDEX idx_sessions_expire ON upload_sessions(expires_at) WHERE status = 'in_progress';

-- ─────────────────────────────────────────────
-- UPLOAD CHUNKS (idempotent tracking)
-- ─────────────────────────────────────────────
CREATE TABLE upload_chunks (
    session_id          UUID            NOT NULL REFERENCES upload_sessions(session_id),
    chunk_number        INTEGER         NOT NULL,
    size_bytes          INTEGER         NOT NULL,
    checksum_sha256     CHAR(64),                           -- SHA-256 of chunk bytes
    s3_etag             VARCHAR(64),                        -- S3 ETag returned on PUT
    status              VARCHAR(10)     NOT NULL DEFAULT 'received'
                                        CHECK (status IN ('received','verified','failed')),
    received_at         TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (session_id, chunk_number)
);

-- ─────────────────────────────────────────────
-- VIDEOS
-- ─────────────────────────────────────────────
CREATE TABLE videos (
    video_id            VARCHAR(20)     PRIMARY KEY,
    owner_id            UUID            NOT NULL,
    title               VARCHAR(300),
    description         TEXT,
    raw_s3_key          TEXT,                               -- path to assembled raw file in S3
    duration_sec        FLOAT,                              -- from ffprobe
    width               INTEGER,
    height              INTEGER,
    frame_rate          NUMERIC(6,3),
    video_codec         VARCHAR(20),                        -- e.g. 'h264', 'hevc', 'prores'
    audio_codec         VARCHAR(20),
    audio_channels      SMALLINT,
    file_size_bytes     BIGINT,
    container_format    VARCHAR(20),                        -- 'mp4', 'mkv', 'mov', 'avi'
    status              VARCHAR(20)     NOT NULL DEFAULT 'uploading'
                                        CHECK (status IN (
                                            'uploading',
                                            'assembling',
                                            'extracting_metadata',
                                            'moderating',
                                            'transcoding',
                                            'post_processing',
                                            'published',
                                            'failed_moderation',
                                            'failed_transcode',
                                            'private',
                                            'deleted'
                                        )),
    moderation_status   VARCHAR(20)     NOT NULL DEFAULT 'pending'
                                        CHECK (moderation_status IN ('pending','approved','rejected','review')),
    moderation_flags    JSONB,                              -- {"csam": false, "violence": 0.12, "hate_speech": 0.03}
    drm_key_id          UUID,                              -- reference to KMS key record
    thumbnail_url       TEXT,                              -- selected/auto thumbnail
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    published_at        TIMESTAMPTZ,
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_videos_owner   ON videos(owner_id, created_at DESC);
CREATE INDEX idx_videos_status  ON videos(status, created_at DESC);

-- ─────────────────────────────────────────────
-- VIDEO RENDITIONS
-- ─────────────────────────────────────────────
CREATE TABLE video_renditions (
    rendition_id        UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    video_id            VARCHAR(20)     NOT NULL REFERENCES videos(video_id),
    quality_label       VARCHAR(10)     NOT NULL,           -- '2160p', '1080p', '720p', etc.
    width               INTEGER         NOT NULL,
    height              INTEGER         NOT NULL,
    bitrate_kbps        INTEGER         NOT NULL,
    codec               VARCHAR(10)     NOT NULL DEFAULT 'h264',
    hdr_format          VARCHAR(15),                        -- 'hdr10', 'dolby_vision', NULL
    container           VARCHAR(10)     NOT NULL DEFAULT 'fmp4',
    hls_manifest_key    TEXT,                               -- S3 key: /{video_id}/{quality}/playlist.m3u8
    dash_manifest_key   TEXT,
    storage_bytes       BIGINT,
    is_drm_encrypted    BOOLEAN         NOT NULL DEFAULT FALSE,
    status              VARCHAR(10)     NOT NULL DEFAULT 'pending'
                                        CHECK (status IN ('pending','processing','ready','failed')),
    started_at          TIMESTAMPTZ,
    completed_at        TIMESTAMPTZ,
    UNIQUE (video_id, quality_label, codec)
);

CREATE INDEX idx_renditions_video ON video_renditions(video_id, status);

-- ─────────────────────────────────────────────
-- THUMBNAILS
-- ─────────────────────────────────────────────
CREATE TABLE thumbnails (
    thumbnail_id        UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    video_id            VARCHAR(20)     NOT NULL REFERENCES videos(video_id),
    s3_key              TEXT            NOT NULL,
    cdn_url             TEXT,
    width               INTEGER         NOT NULL DEFAULT 1280,
    height              INTEGER         NOT NULL DEFAULT 720,
    timestamp_sec       FLOAT,                              -- frame position in source video; NULL for custom upload
    is_auto_generated   BOOLEAN         NOT NULL DEFAULT TRUE,
    is_selected         BOOLEAN         NOT NULL DEFAULT FALSE,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ─────────────────────────────────────────────
-- DRM KEY RECORDS
-- ─────────────────────────────────────────────
CREATE TABLE drm_keys (
    key_id              UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    video_id            VARCHAR(20)     NOT NULL REFERENCES videos(video_id),
    key_system          VARCHAR(20)     NOT NULL CHECK (key_system IN ('widevine','fairplay','playready','cenc')),
    key_id_hex          CHAR(32)        NOT NULL,           -- 16-byte key ID in hex
    encrypted_key       TEXT            NOT NULL,           -- content key encrypted by KMS master key
    pssh_data           TEXT,                               -- Base64 PSSH box for DASH manifest
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    rotated_at          TIMESTAMPTZ,
    UNIQUE (video_id, key_system)
);

-- ─────────────────────────────────────────────
-- PIPELINE EVENTS (audit log)
-- ─────────────────────────────────────────────
CREATE TABLE pipeline_events (
    event_id            UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    video_id            VARCHAR(20)     NOT NULL,
    stage               VARCHAR(30)     NOT NULL,           -- 'upload_complete', 'transcode_start', etc.
    status              VARCHAR(10)     NOT NULL,           -- 'started', 'completed', 'failed'
    worker_id           VARCHAR(100),                       -- pod/instance that processed this stage
    detail              JSONB,                              -- stage-specific metadata
    duration_ms         INTEGER,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_events_video ON pipeline_events(video_id, created_at DESC);

-- ─────────────────────────────────────────────
-- CONTENT MODERATION RESULTS
-- ─────────────────────────────────────────────
CREATE TABLE moderation_results (
    result_id           UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    video_id            VARCHAR(20)     NOT NULL REFERENCES videos(video_id),
    check_type          VARCHAR(30)     NOT NULL,           -- 'csam_hash', 'audio_fingerprint', 'ml_violence', 'ml_hate_speech'
    verdict             VARCHAR(10)     NOT NULL,           -- 'pass', 'fail', 'review', 'error'
    confidence          NUMERIC(5,4),                       -- 0.0 - 1.0; NULL for hash checks
    detail              JSONB,                              -- {"matched_hash": "abc...", "ncmec_report_id": "..."}
    checked_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    checker_version     VARCHAR(20),                        -- model version or service version
    UNIQUE (video_id, check_type)
);
```

### Database Choice

| Database          | Pros                                                            | Cons                                                         |
|-------------------|-----------------------------------------------------------------|--------------------------------------------------------------|
| PostgreSQL        | ACID, JSONB, partitioning, strong tooling                      | Vertical scale limits; sharding requires Citus               |
| MySQL + Vitess    | Proven at scale, horizontal sharding                           | Less rich data types than Postgres; JSONB equivalent limited |
| Cassandra         | Write-heavy workloads; append-log style events                 | No joins; eventual consistency; complex modeling             |
| DynamoDB          | Serverless, managed, global tables                             | Expensive at scale; limited query patterns                   |
| Redis             | Sub-ms session state, chunk tracking                          | Not primary store; persistence is secondary                  |
| S3 (object store) | Infinite scale, 11-nines durability, cheap cold storage        | Not a database; no queries                                   |

**Selected Architecture:**

- **PostgreSQL with Citus extension** (or Aurora PostgreSQL with read replicas): For `upload_sessions`, `videos`, `video_renditions`, `thumbnails`, `drm_keys`, `moderation_results`. JSONB support for moderation flags and pipeline event details. Partitioning for `pipeline_events` (monthly). Citus for horizontal scaling when needed (partition by `video_id`).
- **Redis**: Upload session state (chunk bitmap, received_bytes) stored in Redis for ultra-fast access during chunk upload. Format: `HSET session:{session_id} received_bytes {n} chunk_bitmap {bitfield}`. Redis Bitfield tracks which chunks are received — `SETBIT upload_chunks:{session_id} {chunk_number} 1`. Expiry: `EXPIRE session:{session_id} 86400` (24h TTL).
- **Amazon S3 (or GCS)**: Raw video chunks and assembled files, transcoded segments, thumbnails. Lifecycle rules: raw files deleted after transcoding + 30-day buffer. Multipart upload via S3 native API for large files (> 100 MB).
- **Message Queue (SQS/Kafka)**: Kafka for event-driven pipeline stages (higher throughput, replay capability). SQS for job queues (transcode jobs, thumbnail generation, moderation). Dead-letter queues for failed jobs.
- **KMS (AWS KMS / Cloud KMS)**: DRM content keys stored encrypted in KMS. Master key in HSM. Content keys are generated by the pipeline, encrypted by KMS, stored in `drm_keys` table (encrypted value only — never stored in plaintext).

---

## 5. API Design

All endpoints versioned under `/api/v1/`. OAuth 2.0 Bearer tokens. TLS required.

### Upload Session Management

```
POST /api/v1/uploads/sessions
Authorization: Bearer {token}
Content-Type: application/json
Idempotency-Key: {uuid}

Request:
{
  "filename": "my_documentary.mp4",
  "total_size_bytes": 5368709120,
  "content_type": "video/mp4",
  "chunk_size_bytes": 5242880,
  "checksum_sha256": "e3b0c44298fc1c149afb...",  -- optional: full file SHA-256
  "metadata": {
    "title": "My Documentary",
    "description": "...",
    "visibility": "private"   -- keep private until manually published
  }
}

Response 201:
{
  "session_id": "sess-uuid",
  "video_id": "aBcDeFgHiJk",
  "upload_url_template": "https://upload.platform.com/api/v1/uploads/sessions/{session_id}/chunks/{chunk_number}",
  "total_chunks": 1024,
  "chunk_size_bytes": 5242880,
  "expires_at": "2025-04-10T12:00:00Z",
  "presigned_headers": {
    "X-Upload-Token": "signed-jwt-valid-24h"
  }
}

Rate limit: 100 session initiations/hour per user
Idempotency: same Idempotency-Key returns existing session if in_progress
```

```
PUT /api/v1/uploads/sessions/{session_id}/chunks/{chunk_number}
X-Upload-Token: {signed-jwt}
Content-Type: application/octet-stream
Content-Length: 5242880
X-Chunk-Checksum: sha256={hex-sha256-of-this-chunk}

Body: <binary chunk data>

Response 200:
{
  "chunk_number": 42,
  "received_bytes": 220200960,
  "total_size_bytes": 5368709120,
  "progress_pct": 4.1,
  "status": "chunk_received"
}

Response 400 (checksum mismatch):
{
  "error": "checksum_mismatch",
  "expected": "client-provided-hash",
  "received": "server-computed-hash",
  "message": "Chunk 42 checksum mismatch. Please re-upload this chunk."
}

Rate limit: 10,000 chunk uploads/min per session (unlikely to hit; bandwidth-bound)
```

```
GET /api/v1/uploads/sessions/{session_id}/status
X-Upload-Token: {signed-jwt}

Response 200:
{
  "session_id": "sess-uuid",
  "video_id": "aBcDeFgHiJk",
  "status": "in_progress",
  "received_bytes": 220200960,
  "total_size_bytes": 5368709120,
  "received_chunks": 42,
  "total_chunks": 1024,
  "missing_chunks": [0, 7, 13],    -- for resumption after failure
  "expires_at": "2025-04-10T12:00:00Z"
}

-- Client uses missing_chunks to resume after network failure
```

```
POST /api/v1/uploads/sessions/{session_id}/complete
X-Upload-Token: {signed-jwt}
Content-Type: application/json

{
  "checksum_sha256": "e3b0c44298fc1c149afb..."  -- optional final checksum
}

Response 202:
{
  "video_id": "aBcDeFgHiJk",
  "status": "processing",
  "estimated_ready_at": "2025-04-09T13:30:00Z",
  "polling_url": "/api/v1/videos/aBcDeFgHiJk/status"
}
```

### Video Status

```
GET /api/v1/videos/{video_id}/status
Authorization: Bearer {token}

Response 200:
{
  "video_id": "aBcDeFgHiJk",
  "status": "transcoding",
  "moderation_status": "approved",
  "pipeline_stages": [
    {"stage": "upload_complete",        "status": "completed", "completed_at": "2025-04-09T13:00:00Z"},
    {"stage": "metadata_extraction",    "status": "completed", "completed_at": "2025-04-09T13:00:45Z"},
    {"stage": "content_moderation",     "status": "completed", "completed_at": "2025-04-09T13:02:00Z"},
    {"stage": "drm_provisioning",       "status": "completed", "completed_at": "2025-04-09T13:00:50Z"},
    {"stage": "transcoding_360p",       "status": "completed", "completed_at": "2025-04-09T13:05:00Z"},
    {"stage": "transcoding_720p",       "status": "completed", "completed_at": "2025-04-09T13:07:30Z"},
    {"stage": "transcoding_1080p",      "status": "in_progress", "started_at": "2025-04-09T13:07:00Z"},
    {"stage": "transcoding_2160p",      "status": "pending"},
    {"stage": "thumbnail_generation",   "status": "completed", "completed_at": "2025-04-09T13:01:30Z"},
    {"stage": "caption_generation",     "status": "in_progress"}
  ],
  "available_qualities": ["360p", "720p"],
  "thumbnail_url": "https://cdn.platform.com/thumbs/aBcDeFgHiJk/thumb_1.jpg",
  "estimated_completion": "2025-04-09T13:20:00Z"
}
```

### Thumbnail Management

```
GET /api/v1/videos/{video_id}/thumbnails
Authorization: Bearer {token}

Response 200:
{
  "thumbnails": [
    {"id": "thumb-uuid-1", "url": "...", "timestamp_sec": 12.5, "is_selected": true, "is_auto_generated": true},
    {"id": "thumb-uuid-2", "url": "...", "timestamp_sec": 45.0, "is_selected": false},
    {"id": "thumb-uuid-3", "url": "...", "timestamp_sec": 120.3, "is_selected": false}
  ]
}

PUT /api/v1/videos/{video_id}/thumbnails/{thumbnail_id}/select
Authorization: Bearer {token}
Response 204

POST /api/v1/videos/{video_id}/thumbnails
Authorization: Bearer {token}
Content-Type: multipart/form-data; boundary=----FormBoundary
  ------FormBoundary
  Content-Disposition: form-data; name="thumbnail"; filename="custom_thumb.jpg"
  Content-Type: image/jpeg
  <binary>
  ------FormBoundary--

Response 201:
{"thumbnail_id": "thumb-uuid-4", "url": "..."}
```

### Video Publish/Update

```
PATCH /api/v1/videos/{video_id}
Authorization: Bearer {token}
Content-Type: application/json

{
  "title": "My Documentary: Final Cut",
  "description": "Full description...",
  "visibility": "public",
  "tags": ["documentary","travel","nature"],
  "category_id": 19
}

Response 200:
{
  "video_id": "aBcDeFgHiJk",
  "status": "published",
  "url": "https://platform.com/watch/aBcDeFgHiJk"
}

Rate limit: 60 PATCH requests/hour per video (anti-spam)
```

---

## 6. Deep Dive: Core Components

### 6.1 Resumable Multi-Part Upload Protocol

**Problem it solves:**
Uploading a 50 GB file over a residential internet connection (30 Mbps upload) takes 3.7 hours minimum. In this time: the browser may close, the connection may drop, the mobile user may switch from WiFi to cellular. A naive upload that starts from byte 0 on failure is unusable. Resumable uploads allow the upload to continue from the last successfully received byte without losing any already-uploaded data.

**Possible Approaches:**

| Approach                          | Resume Granularity | Client Complexity | Server State | Standards          |
|-----------------------------------|--------------------|--------------------|--------------|-------------------|
| Single PUT (no resume)            | None (restart)     | Very low           | None         | HTTP PUT           |
| Range-based PUT (RFC 7233)        | Last received byte | Low                | Minimal      | HTTP Range         |
| Google Resumable Upload Protocol  | Byte-exact         | Low-medium         | Session state| GCS API standard   |
| TUS Protocol (open standard)      | Configurable       | Low (SDK)          | Session state| TUS 1.0            |
| S3 Multipart Upload               | Per-part (5MB min) | Medium             | Part ETags   | AWS S3 standard    |
| Custom chunked with DB tracking   | Per-chunk          | Medium             | Full DB state| Custom             |

**Selected: TUS-compatible protocol with S3 Multipart Upload backend.**

TUS (https://tus.io) is an open standard for resumable file uploads with client libraries in JavaScript, Swift, Android, Python, Java, and Go. We implement a TUS-compatible server that internally uses S3 Multipart Upload for storage.

**Protocol Flow:**

```
PHASE 1: Creation
  POST /api/v1/uploads/sessions
  Tus-Resumable: 1.0.0
  Upload-Length: 5368709120
  Upload-Metadata: filename dXBsb2Fk..., content-type dmlkZW8vbXA0

  Response 201 Created:
  Location: /api/v1/uploads/sessions/sess-uuid
  Tus-Resumable: 1.0.0

PHASE 2: Upload (repeated per chunk)
  PATCH /api/v1/uploads/sessions/sess-uuid
  Tus-Resumable: 1.0.0
  Upload-Offset: 0
  Content-Type: application/offset+octet-stream
  Content-Length: 5242880
  Body: <chunk bytes>

  Response 204 No Content:
  Upload-Offset: 5242880
  Tus-Checksum: sha256 {base64-sha256}

PHASE 3: Resume check (after failure)
  HEAD /api/v1/uploads/sessions/sess-uuid
  Tus-Resumable: 1.0.0

  Response 200 OK:
  Upload-Offset: 220200960    ← client resumes from here
  Upload-Length: 5368709120
  Tus-Resumable: 1.0.0

PHASE 4: Auto-complete (when Upload-Offset == Upload-Length)
  Server detects final chunk received, triggers assembly + pipeline.
```

**Server-Side Implementation:**

1. **Session Creation**: Create `upload_sessions` row in PostgreSQL. Initialize Redis state: `HSET session:{session_id} received_bytes 0 total_size {n} s3_upload_id {s3_multipart_upload_id}`. Initiate S3 Multipart Upload: `s3.CreateMultipartUpload(bucket, key)` → returns `uploadId`. Store `uploadId` in Redis session state.

2. **Chunk Upload**:
   - Validate `X-Upload-Token` JWT (signed, expiry 24h).
   - Verify `Upload-Offset` matches current `received_bytes` in Redis (prevents out-of-order writes).
   - Read chunk bytes from request body.
   - Compute SHA-256 checksum, compare with `X-Chunk-Checksum` if provided.
   - Call `s3.UploadPart(uploadId, partNumber, chunk_bytes)` → returns `ETag`.
   - Store ETag in Redis: `HSET chunk_etags:{session_id} {part_number} {etag}`.
   - Atomically increment `received_bytes` in Redis: `HINCRBY session:{session_id} received_bytes {chunk_size}`.
   - Update PostgreSQL `upload_chunks` table (async, via Kafka write) — not in the critical path.
   - Return `204 No Content` with new `Upload-Offset`.

3. **Final Chunk Detection**: When `received_bytes == total_size_bytes`, call `s3.CompleteMultipartUpload(uploadId, [{partNumber, etag}...])`. S3 assembles all parts into a single object atomically. On S3 completion, publish `video-uploaded` event to Kafka.

4. **Chunk Ordering & Parallelism**: TUS supports parallel chunk uploads (with the `tus-parallel` extension). Chunks can be uploaded out-of-order — each chunk maps to an S3 part number. S3's CompleteMultipartUpload accepts parts out of order and assembles by part number. Server tracks which parts have been received using Redis Bitfield: `SETBIT chunks_received:{session_id} {part_number} 1`. Assembly happens only when all bits are set.

5. **Resume After Failure**: On `HEAD` request: read `received_bytes` from Redis. Return as `Upload-Offset`. The client re-computes which chunks to send by comparing local chunk manifest against `missing_chunks` from `/status` endpoint. S3 ListParts API allows server to verify which parts S3 has received (handles case where Redis state is lost but S3 parts exist).

**Implementation Detail — Chunk Checksum Verification:**
```python
# Server-side chunk handler (pseudo-Python)
def handle_chunk_upload(session_id, part_number, chunk_bytes, client_checksum):
    # 1. Validate token
    jwt.validate(request.headers['X-Upload-Token'])

    # 2. Verify checksum
    server_checksum = hashlib.sha256(chunk_bytes).hexdigest()
    if client_checksum and client_checksum != server_checksum:
        return 400, {"error": "checksum_mismatch"}

    # 3. Upload to S3
    session = redis.hgetall(f"session:{session_id}")
    etag = s3.upload_part(
        Bucket=UPLOAD_BUCKET,
        Key=session['s3_key'],
        UploadId=session['s3_upload_id'],
        PartNumber=part_number,
        Body=chunk_bytes
    )

    # 4. Track in Redis
    redis.hset(f"chunk_etags:{session_id}", part_number, etag)
    redis.setbit(f"chunks_received:{session_id}", part_number, 1)
    redis.hincrby(f"session:{session_id}", 'received_parts', 1)

    # 5. Check if complete
    received = int(redis.hget(f"session:{session_id}", 'received_parts'))
    total = int(redis.hget(f"session:{session_id}", 'total_chunks'))
    if received == total:
        trigger_assembly(session_id)

    return 204, {"Upload-Offset": int(session['received_bytes']) + len(chunk_bytes)}
```

**Q&A:**

Q: What happens if the server crashes between receiving a chunk and writing to S3?
A: S3 Multipart Upload is atomic per part — either the `UploadPart` call succeeds and the part is durably stored, or it fails and nothing is written. If the server crashes after `UploadPart` succeeds but before updating Redis, the Redis state is stale (received_bytes is lower than actual). On resume (HEAD request), the server calls `s3.ListParts` to get actual received parts from S3, reconciles with Redis state, and returns the corrected `Upload-Offset`. This makes the resume path authoritative from S3's perspective.

Q: What is the maximum number of parts in an S3 Multipart Upload and how does this constrain chunk size?
A: S3 limits multipart uploads to 10,000 parts. For a 50 GB file: 50 GB / 10,000 = 5 MB minimum chunk size. For the 5 MB default chunk size, a 50 GB file = 10,000 parts exactly (boundary case). For larger files (e.g., a hypothetical 100 GB limit), chunk size must increase to 10 MB. The Upload API dynamically calculates recommended chunk size: `max(5MB, ceil(total_size / 10000))`.

Q: How do you handle concurrent uploads from the same user (e.g., two browser tabs uploading simultaneously)?
A: Each upload creates a unique `session_id`. Session-level isolation means concurrent uploads don't interfere. Redis keys are namespaced by `session_id`. Rate limiting per `user_id` (100 concurrent sessions max per user) prevents abuse. Upload token is scoped to `session_id` — a token for session A cannot be used to upload chunks to session B.

Q: What happens to orphaned S3 Multipart Uploads if a session expires before completion?
A: Orphaned S3 multipart uploads incur S3 storage charges indefinitely until completed or aborted. An S3 Lifecycle Rule on the upload bucket: `AbortIncompleteMultipartUploadDays: 2` automatically aborts any incomplete multipart upload older than 2 days. Additionally, a scheduled job (cron every 6 hours) queries `upload_sessions WHERE status = 'in_progress' AND expires_at < NOW()`, sets them to `expired`, and calls `s3.AbortMultipartUpload` for their S3 upload IDs.

Q: How do you handle very large files (> 50 GB) and ensure the upload URL template doesn't expose the S3 bucket?
A: Upload URLs in the API (`/api/v1/uploads/sessions/{session_id}/chunks/{n}`) never expose S3 directly. The Upload API service is a proxy — it accepts the chunk via HTTP, streams it to S3 Multipart Upload internally. The presigned S3 URL (if used) is generated server-side and never returned to the client. This ensures the client never has direct S3 access and all uploads are authenticated and rate-limited through the API layer. Alternatively, for the very largest files (enterprise use), the server can return pre-signed S3 part URLs directly to the client — this reduces server bandwidth usage (client uploads directly to S3, bypassing the API server). In this case, the Upload API acts as an orchestrator: issues pre-signed URLs, tracks part ETags reported by the client, and calls CompleteMultipartUpload when all parts are done.

---

### 6.2 Transcoding Workers — Multi-Resolution Parallel Transcoding

**Problem it solves:**
A 2-hour uploaded video must be transcoded into 8 quality levels (144p through 4K), optionally in multiple codecs (H.264, VP9, AV1), with audio tracks for multiple languages, within 60 minutes of upload completion. The transcoding must be: (1) Parallelized to meet time SLAs, (2) Fault-tolerant (partial failures don't lose work), (3) Cost-efficient (GPU instances are expensive), (4) Idempotent (re-running a failed job produces the same output).

**Possible Approaches:**

| Strategy                          | Parallelism     | Complexity   | Fault Tolerance | Cost        |
|-----------------------------------|-----------------|--------------|-----------------|-------------|
| Sequential single machine         | None            | Very low     | Poor            | Low         |
| Parallel by quality level         | 8×              | Low          | Medium          | Medium      |
| Segment-parallel per quality      | N×8× (N=segments)| High       | High            | High        |
| Segment-parallel all qualities    | N×8×            | Very high    | High            | Highest     |
| Adaptive (short file: parallel quality; long file: segment-parallel) | N×8× | High | High | Medium |

**Selected: Two-phase parallelism — segment splitting + parallel quality encoding.**

**Phase 1: Segment Splitting**

The raw assembled video (in S3) is split into 30-second segments using a pre-processing pass:

```bash
ffmpeg -i s3://raw/video_id.mp4 \
  -c:v copy -c:a copy \
  -f segment -segment_time 30 \
  -force_key_frames "expr:gte(t,n_forced*30)" \
  -segment_format mp4 \
  -reset_timestamps 1 \
  s3://segments/{video_id}/raw/seg_%04d.mp4
```

This is a fast demux-only pass (no re-encode): a 2-hour video produces 240 segments in ~2-3 minutes. After splitting, a job is published to the `transcode-jobs` SQS queue for each `(segment_index, quality_level)` combination: 240 segments × 8 qualities = 1,920 jobs.

**Phase 2: Parallel Quality Encoding**

Each worker pulls a job from the queue and runs FFmpeg:

```bash
ffmpeg -i s3://segments/{video_id}/raw/seg_0042.mp4 \
  -vf scale=1920:1080 \
  -c:v h264_nvenc \
  -preset p5 \            # NVENC quality preset (p1=fastest, p7=best)
  -b:v 5000k \
  -maxrate 5500k \
  -bufsize 10000k \
  -profile:v high \
  -level:v 4.0 \
  -g 60 \                 # keyframe every 2s at 30fps
  -keyint_min 60 \
  -sc_threshold 0 \       # disable scene-change detection (fixed keyframes)
  -c:a aac -b:a 192k \
  -f mp4 \
  s3://transcoded/{video_id}/1080p/seg_0042.mp4
```

**Phase 3: Assembly**

When all segment jobs for a quality level are complete (coordinator tracks via Redis counter: `DECR pending:{video_id}:1080p`), the Assembler runs:

```bash
# Create FFmpeg concat input file
echo "file 's3://transcoded/{video_id}/1080p/seg_0000.mp4'"  > /tmp/concat.txt
echo "file 's3://transcoded/{video_id}/1080p/seg_0001.mp4'" >> /tmp/concat.txt
# ... all segments

ffmpeg -f concat -safe 0 -i /tmp/concat.txt \
  -c copy \
  -f hls \
  -hls_time 2 \
  -hls_segment_type fmp4 \
  -hls_flags single_file \
  -hls_segment_filename s3://final/{video_id}/1080p/seg_%05d.m4s \
  s3://final/{video_id}/1080p/playlist.m3u8
```

**Job Queue Design:**

- Queue: SQS FIFO queue per quality tier (Priority: 1080p+ uses HIGH_PRIORITY queue; 360p and below uses STANDARD queue).
- Job schema: `{video_id, segment_index, quality_label, source_s3_key, target_s3_prefix, encoder_params}`.
- Visibility timeout: `estimated_transcode_time × 2`. For a 30-second 1080p segment on NVENC T4: ~5 seconds encode → visibility timeout = 60 seconds.
- Dead-letter queue: after 3 failures, job goes to DLQ; alert fires; human review.
- Completion tracking: Redis counter initialized to `num_segments` per quality; decremented on each worker completion. When zero → trigger assembler.

**Worker Auto-Scaling:**
- AWS Auto Scaling Group for GPU instances (`g4dn.xlarge` with T4 GPU).
- Scale metric: SQS `ApproximateNumberOfMessagesVisible`.
- Scale-up: > 50 pending jobs → add instances (one job per instance). Scale-up takes 2-3 minutes (EC2 launch + Docker pull).
- Scale-down: < 10 pending jobs for 5 minutes → terminate instances.
- Min instances: 5 (always-warm to avoid cold start latency).
- Max instances: 500 (cost cap; burstable beyond via spot instances).

**Q&A:**

Q: How do you ensure keyframe alignment across segments for seamless HLS playback?
A: The `-force_key_frames "expr:gte(t,n_forced*30)"` parameter in the splitter ensures I-frames are placed at every 30-second boundary. Each segment starts and ends at a keyframe. When FFmpeg encodes each segment, the first frame of every segment is guaranteed to be an I-frame (`-keyint_min 60 -sc_threshold 0 -g 60` at 30fps). When segments are assembled into an HLS playlist, segment boundaries align with keyframes. Without this alignment, ABR segment switching would cause a decoded video artifact at the segment boundary.

Q: How do you handle a video where the source file has variable frame rate (VFR) — common in screen recordings?
A: Variable Frame Rate (VFR) videos have non-uniform timestamps between frames. FFmpeg's HLS segment splitter with `-segment_time 30` may not split exactly at 30-second boundaries for VFR. Solution: convert VFR to CFR (Constant Frame Rate) in a pre-processing step: `ffmpeg -i input.mp4 -vf fps=30 -c:v copy output_cfr.mp4` (or `-vsync cfr` for softer conversion). This adds a pre-processing stage before splitting. Metadata extraction (`ffprobe`) detects VFR and sets a flag in `videos.is_vfr = true`, triggering the CFR conversion path.

Q: How do you handle encoding failures for specific segments (transient GPU/memory errors)?
A: Each transcode job has a retry counter (tracked in the SQS message attribute). On failure: the worker NACKs the message (SQS visibility timeout expires) → message reappears → new worker picks it up. After 3 failures, message goes to DLQ. Common causes: (1) Corrupted source segment (copy from a damaged raw file) — retries won't help; DLQ alert triggers manual re-download and re-split of that segment. (2) Transient NVENC OOM (large frame) — typically succeeds on retry on a different instance. (3) S3 write failure — retry succeeds.

Q: Why use 30-second segments for splitting rather than 10-second or 60-second?
A: 30-second segments balance parallelism granularity vs. overhead. 10-second segments: for a 2-hour video = 720 segments × 8 qualities = 5,760 jobs. More parallelism but higher SQS/coordination overhead and more S3 PUT operations. 60-second segments: 120 segments × 8 = 960 jobs. Less overhead but 2× longer minimum time for the slowest job. 30-second: 1,920 jobs, with one segment per worker at peak = 1,920 parallel workers for the fastest possible completion. In practice, with 500 worker cap, a 2-hour video at 30s segments/8 qualities finishes in: `ceil(1,920/500) × avg_transcode_time` = `4 rounds × ~10s/segment` = ~40 seconds of GPU encode time (plus coordination overhead). Actual wall-clock time including S3 latency: ~5-10 minutes.

Q: How do you implement priority scheduling so that popular or time-sensitive uploads are processed first?
A: Two SQS queues: `HIGH_PRIORITY` (Premium creators, viral-detecting uploads) and `STANDARD`. Workers poll HIGH_PRIORITY first (`ReceiveMessage` with priority queue). Priority assignment at upload time: (1) Premium/Partner creators → HIGH_PRIORITY. (2) Standard uploads → STANDARD. (3) A "virality detector" microservice monitors early view rate on new videos (views in first 5 minutes); if above a threshold, re-prioritizes the remaining transcode jobs to HIGH_PRIORITY by publishing new messages (idempotent: coordinator checks `rendition.status` before re-publishing to avoid double-processing).

---

### 6.3 DRM and Content Encryption Pipeline

**Problem it solves:**
Premium video content must be protected against unauthorized copying and redistribution. DRM (Digital Rights Management) encrypts video segments with a per-video content key. The content key is never exposed to the client directly — only delivered via a DRM license after authenticating and verifying the user's subscription/entitlement. This prevents downloading video segments and playing them outside the authorized player.

**Possible Approaches:**

| DRM System       | Device Support                      | Key Exchange           | License Complexity | Open Standard? |
|------------------|-------------------------------------|------------------------|--------------------|---------------|
| Widevine (Google)| Android, Chrome, Chromecast, Smart TV| CDM (Content Decryption Module) | Moderate      | No (proprietary) |
| FairPlay (Apple) | iOS, macOS Safari, Apple TV         | HTTPS + certificate    | Moderate           | No (proprietary) |
| PlayReady (MS)   | Windows, Xbox, Edge, Smart TV        | License server         | Complex            | No (proprietary) |
| CENC (ISO)       | Multi-system abstraction             | Per-system keys        | Complex (multi)    | Yes (ISO 23001-7) |
| ClearKey (W3C)   | All browsers supporting EME          | Plain JSON key         | Simple             | Yes            |

**Selected: CENC (Common Encryption) + Widevine + FairPlay + PlayReady (multi-DRM).**

CENC (ISO/IEC 23001-7) defines a single encryption scheme compatible with multiple DRM systems. One set of encrypted segments works with Widevine on Android/Chrome, FairPlay on Apple devices, and PlayReady on Windows — no need to store multiple copies of encrypted content.

**Encryption Scheme:**
- AES-128-CTR (CENC mode) for H.264/H.265 — encrypts only the slice data of each NAL unit, not the headers. This allows metadata parsing without decryption.
- AES-128-CBC (CBCS mode) for HEVC and video containers where Apple compatibility required.

**Pipeline Steps:**

1. **Key Generation** (triggered after assembly, before CDN upload):
   ```python
   # Generate content key (must be 16 bytes, cryptographically random)
   content_key = os.urandom(16)
   key_id = uuid.uuid4()

   # Encrypt content key with AWS KMS master key
   encrypted_key = kms.encrypt(
       KeyId='arn:aws:kms:us-east-1:account:key/master-key-id',
       Plaintext=content_key
   )['CiphertextBlob']

   # Store encrypted key in DB (never store plaintext)
   db.insert('drm_keys', {
       'video_id': video_id,
       'key_system': 'cenc',
       'key_id_hex': key_id.hex,
       'encrypted_key': base64.b64encode(encrypted_key),
   })
   ```

2. **Segment Encryption** (FFmpeg + Shaka Packager):
   Shaka Packager (Google's open-source DASH/HLS packager) handles CENC encryption:
   ```bash
   packager \
     in=s3://final/{video_id}/1080p/unencrypted.mp4,\
     stream=video,\
     output=s3://encrypted/{video_id}/1080p/encrypted.mp4 \
     in=s3://final/{video_id}/1080p/unencrypted.mp4,\
     stream=audio,\
     output=s3://encrypted/{video_id}/1080p/audio.mp4 \
     --enable_raw_key_encryption \
     --keys key_id={key_id_hex}:key={content_key_hex} \
     --protection_scheme cbcs \
     --hls_master_playlist_output s3://encrypted/{video_id}/1080p/master.m3u8 \
     --mpd_output s3://encrypted/{video_id}/1080p/manifest.mpd
   ```
   Content key is retrieved from KMS at this stage (one-time decryption for the packaging job).

3. **PSSH Box Generation**: Widevine and PlayReady require DRM-specific "Protection System Specific Header" boxes in the MP4 container. Shaka Packager generates these from the key_id. The PSSH box data is stored in `drm_keys.pssh_data` for embedding in DASH manifests.

4. **License Server (at Playback Time)**:
   - Player performs DRM handshake: sends license request (Widevine CDM challenge, FairPlay CKC request, or PlayReady license request) to the License API.
   - License API authenticates the user (validates JWT), checks subscription entitlement (`subscriptions WHERE user_id=X AND status='active'`), and retrieves `encrypted_key` from `drm_keys` table.
   - License API calls AWS KMS to decrypt the content key: `kms.decrypt(CiphertextBlob=encrypted_key)['Plaintext']`.
   - Wraps the content key in the appropriate DRM license format (Widevine/FairPlay/PlayReady) using the respective SDK/API.
   - Returns the license to the player's CDM.
   - CDM decrypts video segments in a hardware-isolated Trusted Execution Environment (TEE) — the content key never touches JavaScript.

5. **License Caching**:
   - Licenses are cached client-side by the CDM for 24-48 hours (offline viewing window).
   - License API does NOT cache licenses server-side (each playback start issues a fresh license check to verify current entitlement).
   - License API response time target: < 50 ms p50, < 200 ms p99.

**Q&A:**

Q: What is the threat model for DRM and what can it actually prevent?
A: DRM prevents casual copying — a user cannot simply download segments from CDN and play them because segments are encrypted. DRM does NOT prevent: (1) Screen recording (capturing the screen while playing). (2) HDCP downgrade attacks on older hardware. (3) Reverse engineering of CDM (academic research has extracted keys from software CDM). Netflix and YouTube accept these limitations — DRM is good enough to satisfy content licensor requirements for preventing mass redistribution, not to prevent determined reverse engineers. Content studio contracts specify Widevine L1 (hardware TEE) for 4K HDR, Widevine L3 (software) acceptable for up to 1080p.

Q: How do you rotate DRM content keys if a key is suspected to be compromised?
A: Key rotation process: (1) Generate new content key, store in `drm_keys` (new row with `rotated_at` timestamp on old row). (2) Re-encrypt all segments with the new key using Shaka Packager (identical to initial encryption; S3 objects overwritten). (3) Update `drm_keys.key_id_hex` in the manifest PSSH. (4) Issue a CDN purge for the old encrypted segments (cache must serve new segments). (5) Invalidate all outstanding licenses (set `revoked_at` in DRM license DB; license server rejects licenses issued before revocation). (6) All players attempting to play will re-request a new license, which uses the new content key. Re-encryption wall time: ~30 minutes for a 2-hour movie (limited by Shaka Packager throughput on 1 machine; parallelizable).

Q: Why store the content key encrypted in the database rather than directly in AWS KMS?
A: AWS KMS stores "data keys" (key encryption keys, KEKs), not arbitrary secrets. KMS's recommended pattern is "envelope encryption": generate a data key with `GenerateDataKey`, use it to encrypt your data (content key), store the encrypted data (content key) in your DB, and store the KEK in KMS. At use time, call `Decrypt` with the encrypted content key → KMS returns plaintext data key → use to wrap content key in DRM license. Benefits: (1) KMS API call latency (~5-10ms) only at key issuance and license request — not per segment. (2) KMS provides audit logging of all decrypt operations (CloudTrail). (3) Key material never leaves KMS hardware (for CMK-based encryption). (4) If DB is compromised, encrypted content keys are useless without KMS access.

Q: How does FairPlay differ from Widevine in the key exchange flow?
A: Widevine: CDM sends a license request (protobuf binary) to license server; license server responds with a license (encrypted with CDM's device certificate). All communication via standard HTTPS. FairPlay (Apple): Uses a 3-step process: (1) Client requests an SPC (Server Playback Context) from the CDM — requires a certificate from Apple (obtained once per app). (2) Client sends SPC to the license server. (3) License server wraps content key in CKC (Content Key Context) using Apple's server-side FairPlay SDK (requires Apple developer entitlement). The CKC is returned to the CDM, which unwraps the content key. The key difference: FairPlay requires Apple's SDK and developer account to issue CKCs; Widevine uses Google's license proxy service.

Q: How do you handle DRM for downloads (offline playback)?
A: Offline DRM requires a "persistent license": a license stored in the CDM's secure storage that survives app restarts. For Widevine: set `license_type = 'OFFLINE'` in the license request; license server returns a license with `rental_duration` and `playback_duration` policies (e.g., 30 days to start + 48 hours once started). For FairPlay: offline leases handled via a separate "lease" vs "rental" license type. The license server checks: subscription is active at download time → issues offline license. If subscription lapses before `rental_duration` expires, the license remains valid until its own expiry (no real-time subscription check during offline play — this is by design, acceptable tradeoff). License revocation for offline cases is done via `license_expiry` — the platform cannot force-revoke an offline license before its stated expiry.

---

### Additional Component: Thumbnail Generation Pipeline

**Problem it solves:**
After upload, creators need thumbnails for their video. Auto-generated thumbnails must be: (1) Representative of video content (not black frames, not transitional blurred frames). (2) Generated quickly (available before full transcoding completes). (3) Available in multiple candidates for creator selection.

**Implementation:**

1. **Frame Extraction** (runs concurrently with transcoding, as soon as raw video is in S3):
   ```bash
   # Extract frames at 10%, 25%, 50% of video duration
   # First, get duration via ffprobe
   duration=$(ffprobe -v quiet -show_entries format=duration -of csv=p=0 s3://raw/{video_id}.mp4)

   for pct in 0.10 0.25 0.50; do
       offset=$(echo "$duration * $pct" | bc)
       ffmpeg -ss $offset -i s3://raw/{video_id}.mp4 \
         -vframes 1 -q:v 2 -vf scale=1280:720 \
         s3://thumbs/{video_id}/thumb_${pct}.jpg
   done
   ```

2. **Quality Filtering**: A small ML classifier scores each candidate frame for "blurriness" (Laplacian variance), "darkness" (mean pixel value < 30 = dark frame), and "motion artifact" (average block error > threshold). Frames scoring below thresholds are re-extracted from neighboring timestamps.

3. **Sprite Sheet Generation**: For the video scrubber preview (hover over progress bar → see thumbnail): extract one frame per 10 seconds, tile into a sprite sheet image (`ffmpeg -vf thumbnail,scale=160:90,tile=N×M`). Store sprite sheet and VTT file (timing file mapping timestamps to sprite offsets).

4. **Creator Upload**: Creator can upload a custom 1280×720 JPEG/PNG. Server validates: max 2 MB, aspect ratio 16:9, not previously rejected by content moderation. Stored in S3, CDN-served.

---

## 7. Scaling

### Horizontal Scaling

- **Upload API**: Stateless HTTP servers behind an Application Load Balancer. Each instance handles streaming chunk uploads with async S3 multipart put. Scale: 100 instances handle 28,400 chunk requests/sec (284 req/sec per instance × 100).
- **Transcoder Workers**: K8s GPU jobs. Auto-scaled by SQS queue depth. 500 worker cap. Workers are stateless (pull job, execute, emit result).
- **Assembly Workers**: Lightweight CPU-only K8s pods. Triggered by coordinator when all transcode jobs for a quality are complete. One assembly worker per quality per video; 8 parallel assembly workers per video upload.
- **DRM/License Server**: Stateless K8s pods behind load balancer. License issuance is CPU-bound (KMS API call dominates). Scale: 50 pods × 500 RPS/pod = 25,000 license requests/sec.
- **Metadata Extractor**: K8s pods running ffprobe. One pod per upload (parallel with transcoding). Very fast (< 30 seconds for any file size).

### Database Sharding

- **PostgreSQL (upload_sessions, videos)**: Partition `upload_sessions` by `created_at` (monthly); old sessions archived to cold storage. Videos table: range partition by `video_id` hash for even distribution. Citus shards by `video_id` across 10 nodes initially.
- **Redis**: Redis Cluster with 6 primary shards. Upload session state is per `session_id` (naturally distributed). Chunk tracking bitmaps: `upload_chunks:{session_id}` hash key distributed by session_id hash.
- **S3**: No sharding needed (S3 is inherently distributed). Bucket prefixes: raw, segments, transcoded, encrypted, thumbnails, final. Each prefix is effectively a separate namespace. Request rate: S3 prefix-distributed > 3,500 PUT/s and 5,500 GET/s per prefix (S3 limits per prefix; mitigated by using `/video_id/` as the first path component — S3 hashes on the full key including prefix).

### Replication

- **PostgreSQL**: Primary-replica with streaming replication. 2 replicas per shard for read scaling (read queries for status polling use replicas). Failover: Patroni (open-source HA for PostgreSQL) with automatic primary election.
- **Redis**: Redis Cluster with one replica per primary shard (RF=2). Cross-region replication not needed for upload sessions (session state is ephemeral). If Redis fails, upload session state is rebuilt from PostgreSQL (slower but functional).
- **S3**: Cross-region replication for transcoded (final) and encrypted content. Raw and segment buckets: single-region (they are temporary by design).

### Caching

| Cache Layer              | Technology   | What is Cached                                   | TTL         |
|--------------------------|--------------|--------------------------------------------------|-------------|
| Upload session state     | Redis Hash   | received_bytes, chunk bitmap, s3_upload_id       | 24 hours    |
| Chunk ETag map           | Redis Hash   | Part number → S3 ETag (for CompleteMultipartUpload)| 24 hours   |
| Video status (for polling)| Redis String | Latest pipeline status per video_id              | 30 sec      |
| DRM key (per video_id)   | Redis String | Encrypted content key (save KMS round trip)      | 1 hour      |
| License validation result| Redis        | User X is entitled to Video Y (for high-traffic) | 5 min       |
| Thumbnail URLs           | Redis Hash   | Thumbnail CDN URLs per video_id                  | 10 min      |
| Transcoding job counters | Redis Counter| Pending jobs per (video_id, quality)             | Until complete |

**Interviewer Q&A:**

Q: How do you handle a very popular upload that triggers 10,000 concurrent status polling requests from fans waiting for a video to go live?
A: Three layers: (1) Cache the video status in Redis with 30-second TTL and serve all polls from cache (10,000 reads/sec from Redis at < 1ms each is trivial). (2) Stale-while-revalidate: expired cache entries continue serving stale status while one background process refreshes. (3) For extremely hot videos (anticipated releases), use WebSocket or Server-Sent Events (SSE) to push status updates to connected clients instead of polling, eliminating the polling load entirely. The `/api/v1/videos/{id}/status/stream` SSE endpoint sends an event when status changes; client subscribes once, server pushes on each pipeline stage completion.

Q: How do you prevent the transcoding farm from being overwhelmed by a large enterprise customer uploading 1,000 movies simultaneously?
A: Per-user/per-organization quotas: max 100 concurrent transcoding jobs per user_id. Jobs above the quota are queued in a PENDING state in the jobs table and admitted to the SQS queue as capacity becomes available. The quota is configurable (enterprise customers can purchase higher limits via a Tier system). Additionally, the job scheduler uses weighted fair queuing: if one user's jobs fill 100 slots, other users' jobs still get admitted up to their quota. Total farm capacity (500 workers) is shared, but no single user can monopolize it.

Q: What is the impact of S3 API rate limits on the transcoding pipeline?
A: S3 supports 3,500 PUT/s and 5,500 GET/s per prefix. At peak: 500 workers × 1 PUT per job completion = 500 PUTs/sec — well within S3 limits for a single prefix. But if multiple workers operate on the same video_id prefix: `s3://segments/{video_id}/raw/` has 240 segments × 8 qualities = 1,920 parallel writes at peak. S3 rate limit mitigation: S3 automatically scales based on object key patterns — the `{segment_id}` suffix in the key provides sufficient randomness for S3 to distribute across internal shards. AWS documentation confirms that prefixes with random key suffixes scale beyond stated limits. Monitoring: CloudWatch `ThrottlingException` metric; if > 0, increase prefix randomization.

Q: How does the pipeline handle a video that fails content moderation after transcoding has already started?
A: Moderation runs in parallel with transcoding (both start on `video-uploaded` event). However, transcoding completion does not automatically publish the video — the coordinator checks moderation_status before updating `videos.status` to `published`. If moderation fails (CSAM detected): (1) Immediately set `videos.status = 'failed_moderation'` and `moderation_status = 'rejected'`. (2) Cancel in-flight transcode jobs by publishing a `cancel-jobs:{video_id}` event (workers check for cancellation between processing steps). (3) Trigger NCMEC reporting (legal requirement for CSAM). (4) Delete all transcoded segments from S3 immediately. (5) Retain raw file in quarantine prefix (for law enforcement). Moderation completion time target: < 5 minutes (fingerprint checks are < 30s; ML scan < 5 min) — before most transcode jobs complete, to avoid wasted GPU compute.

Q: How do you ensure exactly-once processing in the transcoding pipeline — that a segment is never encoded twice and never missed?
A: Three mechanisms: (1) SQS FIFO queues with deduplication ID = `{video_id}:{segment_index}:{quality_label}` — SQS prevents duplicate messages within a 5-minute deduplication window. (2) Worker uses S3 conditional PUT (`If-None-Match: *`) when writing output — if the object already exists (from a previous execution), the PUT fails with `412 Precondition Failed`, and the worker treats this as a success (idempotent). (3) Coordinator uses Redis SETNX: `SETNX job_claimed:{video_id}:{segment}:{quality} {worker_id}` — only one worker can claim a job. If SETNX returns 0 (key exists), the worker skips and moves to the next job. Together, these ensure each job is processed exactly once even under failure and retry scenarios.

Q: How would you implement real-time progress reporting to the uploader during transcoding?
A: WebSocket server (or SSE endpoint) where authenticated users subscribe to `video_status:{video_id}` events. On each pipeline stage completion, the coordinator publishes a status update to a Redis Pub/Sub channel: `PUBLISH video_status:{video_id} {stage:transcoding_720p,status:completed}`. The WebSocket server subscribes to this channel and pushes updates to all connected clients watching that video_id. Progress percentage: `completed_renditions / total_renditions × 100`. This replaces polling (which is 10,000+ requests/s for popular uploads) with push (one Redis publish → one WebSocket push per connected viewer). Connection overhead: each WebSocket connection is long-lived; 1,000 concurrent status watchers for a popular video = 1,000 persistent connections on the WebSocket server (easily handled by one Node.js or Go server with async I/O).

---

## 8. Reliability & Fault Tolerance

| Failure Scenario                          | Detection                                    | Mitigation                                                                   | RTO          |
|-------------------------------------------|----------------------------------------------|------------------------------------------------------------------------------|--------------|
| Upload API server crash mid-chunk         | Client timeout on PUT (30s)                  | Client retries; session state in Redis; S3 part write was atomic            | < 30 s       |
| Redis failure during chunk upload         | Redis connection error                        | Fallback to PostgreSQL for session state (slower, but functional)            | < 60 s       |
| S3 write failure for chunk                | S3 PutObject 5xx                             | Client retry with exp. backoff (S3 rate: 99.999999999% durability)           | < 5 s retry  |
| S3 Multipart CompleteMultipartUpload fail | 5xx from S3                                  | Retry 3× with backoff; ListParts to verify existing parts before re-attempt | < 30 s       |
| Transcode worker OOM / crash              | SQS visibility timeout + K8s pod restart      | Job reappears in SQS; new worker picks it up; idempotent S3 conditional write| < timeout + 10s |
| Transcode coordinator crash               | Kubernetes liveness probe → pod restart       | Counter in Redis survives pod restart; coordination state recovered          | < 60 s       |
| Content moderation service outage         | HTTP 5xx from moderation API                 | Videos held in 'moderating' state; alert on-call; backlog processed on recovery | Async: no user impact until queue clears |
| DRM KMS failure (key generation)          | KMS API timeout/5xx                           | DRM key generation retried 3×; video held in 'processing' state; alert fires | < 30 s retry |
| Assembly failure (bad segments)           | FFmpeg exit code non-zero                    | DLQ alert; manual re-trigger of assembly; investigate specific bad segment   | Manual       |
| PostgreSQL primary failure                | Patroni detection (< 10s)                    | Patroni promotes replica; writes paused 5-15 seconds during election        | 15 s         |
| Complete pipeline failure (cascading)     | Kafka consumer lag alarm + video status stale| All events persisted in Kafka (7-day retention); replay pipeline on recovery | 0 data loss; backlog cleared over hours |

**Idempotency Design:**
Every pipeline stage is idempotent:
- Chunk uploads: S3 `UploadPart` is idempotent (same part number, same data = same ETag).
- Transcode jobs: S3 conditional PUT on output key prevents double-write.
- Assembly: S3 `CompleteMultipartUpload` is idempotent (calling twice with same parts is a no-op).
- Moderation: re-running moderation on the same video produces the same hash-check result (deterministic).
- DRM key generation: `drm_keys` table has UNIQUE constraint on `(video_id, key_system)` — second insertion fails silently; worker reads existing key.
- Database updates: `INSERT ON CONFLICT DO NOTHING` or `UPSERT` for all pipeline state writes.

**Kafka Event Replay:**
Kafka's `video-uploaded` topic is configured with 7-day retention. If all downstream consumers (transcoding, moderation, DRM) fail simultaneously, they can be restarted and replay all events from the last committed offset. This makes the pipeline eventually consistent and self-healing under failure conditions. Events are published with `key = video_id` (same key always goes to same partition → ordered per video).

---

## 9. Monitoring & Observability

| Metric                              | Type      | Alert Threshold                  | Tool           |
|-------------------------------------|-----------|----------------------------------|----------------|
| Upload session initiation rate      | Counter   | < 10% of expected/hr             | Prometheus     |
| Chunk upload success rate           | Counter   | < 99.5% over 10 min              | Prometheus     |
| S3 multipart complete failure rate  | Counter   | > 0.1% over 10 min               | CloudWatch     |
| Pipeline stage duration (p50/p99)  | Histogram | p99 transcode > 60 min per video | Prometheus     |
| Transcoding SQS queue depth         | Gauge     | > 5,000 pending jobs for 15 min  | CloudWatch     |
| Transcode worker GPU utilization    | Gauge     | > 95% for 30 min (add workers)   | NVIDIA DCGM    |
| Transcode failure rate (per quality)| Counter   | > 1% failure on any quality      | Prometheus     |
| DRM key generation success rate     | Counter   | < 99.9% over 5 min               | Prometheus     |
| Content moderation queue depth      | Gauge     | > 10,000 pending                 | SQS CloudWatch |
| CSAM detection events               | Counter   | Any non-zero → IMMEDIATE alert   | PagerDuty P0   |
| End-to-end pipeline latency         | Histogram | p99 > 90 min for standard video  | Prometheus     |
| Upload session expiry rate          | Counter   | > 5% of sessions expire unused   | Prometheus     |
| DRM license issuance success rate   | Counter   | < 99.9% over 2 min               | Prometheus     |
| Storage cost per GB/day             | Gauge     | > 10% above baseline             | AWS Cost Explorer |

**Distributed Tracing:**
- OpenTelemetry SDK in all pipeline services. Trace ID generated at upload initiation, propagated via Kafka message headers and SQS message attributes throughout the entire pipeline.
- Trace spans: `upload_session_create`, `chunk_upload_{n}`, `s3_complete_multipart`, `kafka_publish`, `metadata_extract`, `moderation_check_{type}`, `drm_key_generate`, `transcode_segment_{n}_{quality}`, `assembly_{quality}`, `thumbnail_generate`, `status_publish`.
- Jaeger UI for trace visualization. Pipeline dashboard: end-to-end trace for any `video_id` in one view.
- Tail-based sampling: 100% of traces for videos in `failed_transcode` or `failed_moderation` states; 1% of successful traces.

**Logging:**
- Structured JSON logs with fields: `video_id`, `session_id`, `stage`, `worker_id`, `duration_ms`, `s3_bytes_written`, `error_code` (if any).
- Transcode worker logs include: `ffmpeg_exit_code`, `gpu_utilization_pct`, `input_fps`, `output_fps` (tracks performance regressions in encoder settings).
- CSAM detection events: logged to an isolated high-security logging system with restricted access (law enforcement compliance). Separate from standard ELK stack.
- Kafka consumer lag for all pipeline consumers published to Prometheus/Grafana. Alert on lag > 10,000 messages per consumer group.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                               | Choice Made                              | Alternative Considered            | Trade-off                                                           |
|----------------------------------------|------------------------------------------|-----------------------------------|---------------------------------------------------------------------|
| Upload protocol: TUS                   | TUS 1.0 compatible server + S3 MP backend| Google Resumable Upload / custom  | +Open standard, +client library ecosystem. −Non-standard S3 integration layer |
| Chunk size: 5 MB default               | Dynamic (max(5MB, ceil(size/10000)))     | Fixed 5 MB always                 | +S3 part limit compliance for large files. −Variable client chunk logic |
| Parallelism: segment × quality         | 2D parallel (N segments × Q qualities)   | Quality-parallel only             | +15× faster for long videos. −Complexity of coordinator and assembly |
| Coordinator: Redis counter             | Redis DECR to zero                       | DB polling                        | +Sub-ms check. −Redis single point of failure (mitigated by persistence) |
| DRM: CENC + multi-DRM                  | Widevine + FairPlay + PlayReady          | Widevine only                     | +Universal device support. −3× license server complexity, 3 SDK licenses |
| Key storage: envelope encryption       | KMS envelope (encrypted key in DB)       | Key in KMS only (GenerateDataKey) | +Key available without KMS call at read time. −Encrypted blob in DB (still secure) |
| Moderation: parallel with transcode    | Start moderation + transcode concurrently| Moderation before transcode       | +Faster pipeline. −Wasted GPU compute if moderation fails (cancelled, not refunded) |
| Status updates: SSE + Redis Pub/Sub    | Push via SSE for polling clients         | Polling                           | +No thundering herd on status endpoint. −WebSocket/SSE infra needed   |
| Assembly: server-side concat           | FFmpeg concat demuxer + Shaka Packager   | Pre-assembled as one file         | +Parallelism of segment encoding. −Assembly step adds latency (< 2 min) |
| Job queue: SQS (not Kafka)             | SQS for transcode jobs; Kafka for events | Kafka for both                    | +SQS simpler for job queues (visibility timeout, DLQ). −No replay on SQS |
| Chunk integrity: SHA-256               | Per-chunk checksum                       | No integrity check                | +Detects corruption in transit. −Extra CPU compute on client + server  |
| Thumbnail: ffmpeg frame sampling       | 3 frames at 10%/25%/50% + quality filter | All keyframes as candidates       | +Fast + representatitve. −May miss best frame; creator overrides available |

---

## 11. Follow-up Interview Questions

**Q1: How would you design a bulk upload API for enterprise customers uploading 10,000 videos per day via API?**
A: Bulk upload requires: (1) Batch session creation endpoint: `POST /api/v1/uploads/sessions/batch` accepts an array of video manifests (filename, size, metadata) and returns an array of session tokens. (2) Higher rate limits for API keys with bulk entitlement (10,000 sessions/day vs 100/hour for standard). (3) Async callback (webhook) on video completion rather than polling: `POST https://customer-webhook.com/video-ready {video_id, status, stream_url}`. (4) Idempotency key on the full batch request, returning existing sessions for already-initiated uploads. (5) A bulk status endpoint: `GET /api/v1/videos/status?video_ids=id1,id2,...&limit=100` returns status for multiple videos in one request.

**Q2: How do you implement server-side encryption at rest for raw video files in S3 without DRM?**
A: AWS S3 Server-Side Encryption (SSE): (1) SSE-S3: AES-256 encryption managed by AWS with S3-managed keys. Zero configuration; suitable for non-premium content. (2) SSE-KMS: Encryption with AWS KMS master key. Each S3 object has a unique data key encrypted by the KMS CMK. CloudTrail logs every decryption. Required for content subject to regulatory requirements (HIPAA, GDPR). (3) SSE-C: Customer provides the encryption key per request. Not recommended (key management complexity). For a video platform, SSE-KMS for raw files (compliance requirement in many jurisdictions) + CENC DRM for streaming (access control). These are complementary — SSE-KMS protects data at rest in S3; DRM protects content in transit and on the client device.

**Q3: How would you design the pipeline to handle 4K HDR (HDR10/Dolby Vision) source files?**
A: HDR metadata is carried in the video container (HEVC or H.264 in a Rec.2020 color space with HDR10 static metadata or Dolby Vision RPU). Pipeline additions: (1) Metadata extraction: ffprobe reads color primaries, transfer characteristics, and HDR10 master display info. Stored in `videos.hdr_format` and `videos.color_space`. (2) HDR transcoding: HEVC encoder is required (not H.264, which doesn't support HDR10 natively). FFmpeg command: `-c:v hevc_nvenc -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc`. (3) HDR tone-mapping for SDR fallback: for devices not supporting HDR, produce an SDR HEVC rendition via tone-mapping: `-vf zscale=t=linear,tonemap=hable,zscale=p=bt709:m=bt709:r=tv`. (4) Dolby Vision: requires Dolby's commercial SDK for RPU metadata embedding — not available in open-source FFmpeg. Licensing required.

**Q4: How do you implement chunked upload progress tracking in the browser without the TUS client library?**
A: Using the Fetch API with streaming request body (Chrome 105+): ```javascript const stream = new ReadableStream({ start(controller) { // slice and enqueue 5MB chunks from File } }); fetch('/api/v1/uploads/sessions/sess-uuid/chunks/0', { method: 'PUT', body: stream, headers: {'Content-Length': chunk.size} });``` Progress: use `XMLHttpRequest.upload.onprogress` event (older API) or track bytes with a `TransformStream` wrapping the `ReadableStream`. Resume: on page reload, call `GET /api/v1/uploads/sessions/sess-uuid/status` → read `missing_chunks` array → re-upload only missing chunks. Store `session_id` in `localStorage` for cross-page-load resume.

**Q5: What are the challenges of supporting video uploads from mobile devices (iOS/Android apps)?**
A: Mobile-specific challenges: (1) Background upload: app may be backgrounded mid-upload. iOS `URLSession` background task supports background uploads; Android `WorkManager` persists upload jobs. TUS client SDK handles resumption on app foreground. (2) Codec variety: newer iPhones upload HEVC (H.265) and HEIC/HEIF in .mov containers. Android uploads H.264 in .mp4. Pipeline must handle both. (3) Variable bitrate: mobile cameras use VBR aggressively; raw file size varies. Dynamic chunk size calculation needed. (4) Network switching: mid-upload switch from WiFi to cellular. Resume protocol handles this; app monitors `Reachability` and continues on new network. (5) Storage: a 4K 10-minute video on iPhone is ~3 GB. App must buffer one chunk (5 MB) in memory, not the whole file, to avoid OOM. (6) Upload compression: mobile apps may optionally re-encode to a smaller format before upload (user choice: "upload original" vs "compress for faster upload").

**Q6: How do you handle audio-only uploads (podcasts, music) in the same pipeline?**
A: Audio-only detection: `ffprobe -show_streams` returns no video stream. `videos.width` and `videos.height` are NULL; `videos.video_codec` is NULL. Pipeline branching: (1) Skip video transcoding (no FFmpeg video encode). (2) Audio transcoding: transcode audio to multiple bitrates: 320 Kbps (AAC, lossless equivalent), 192 Kbps, 128 Kbps (mobile), 64 Kbps (very low bandwidth). Package as HLS audio-only streams. (3) Waveform generation: generate a visual waveform SVG for the audio player UI (`ffmpeg -af aformat=channel_layouts=mono,showwavespic=s=640x120`). (4) Thumbnail generation skipped (no frames); use channel art as the thumbnail. The `video_renditions` table accommodates audio-only via `width=0, height=0, codec='aac'`.

**Q7: How would you implement multi-language dubbed audio tracks in the same video?**
A: Dubbed audio tracks are separate audio files associated with the same video. Data model: `audio_tracks` table: `(track_id, video_id, language, s3_key, codec, bitrate_kbps)`. Packaging: Shaka Packager supports multiple audio tracks in DASH: each language gets its own `<AdaptationSet media_type="audio" lang="es">`. In HLS: multiple `#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",LANGUAGE="es"` lines in the master playlist. At playback: the player detects the device/user language preference and selects the matching audio track. The video track is shared (one encode); audio bandwidth is ~192 Kbps per track — manageable. DRM: all audio tracks encrypted with the same content key as the video (CENC encrypts both).

**Q8: How do you prevent the same content from being uploaded multiple times (deduplication)?**
A: Video fingerprinting for deduplication: (1) During post-processing, compute a perceptual hash of the video using pHash (perceptual hash on keyframes, averaged). Store as `videos.perceptual_hash`. (2) On each new upload completion, query for existing videos with perceptual hash distance < threshold: `SELECT video_id FROM videos WHERE perceptual_hash <-> {new_hash} < 0.1`. This is an approximate nearest neighbor query — use pgvector extension for efficient similarity search. (3) Exact deduplication: SHA-256 of the raw file (stored in `upload_sessions.checksum_sha256`). Exact hash match → definite duplicate. (4) Policy: duplicate handling depends on platform policy. Options: (a) Reject upload with "content already uploaded" error, (b) Allow upload but link to original for Content ID, (c) Allow for different uploader (they may own the rights). For CSAM: exact hash match → immediate block + NCMEC report; no exceptions.

**Q9: How do you handle extremely long videos (livestream recordings of 8-10 hours)?**
A: Challenges: (1) Upload: 8 hours × 4 Mbps = 14.4 GB raw. With 5 MB chunks: 2,880 parts, close to S3's 10,000 part limit (safe). With dynamic chunk sizing: `max(5MB, ceil(14.4GB/10000))` = max(5MB, 1.44MB) = 5MB. (2) Splitting: 8 hr × 3600s / 30s = 960 segments × 8 qualities = 7,680 jobs. Auto-scaler handles this (more jobs = more workers). (3) Assembly: 960 segments per quality. FFmpeg concat file with 960 entries processes in < 5 minutes. (4) Storage: 14.4 GB raw + 11.5 GB transcoded = ~26 GB per upload. Within acceptable range. (5) Playback: HLS manifest with 8 hours / 2s segments = 14,400 segment entries in playlist. Standard HLS allows unlimited entries for VOD; clients can seek within the manifest. DVR-style seeking: segments organized with EXT-X-DISCONTINUITY tags if source had breaks.

**Q10: What is the architecture for providing a "video preview" (animated GIF or WebP) on hover?**
A: On hover over a video thumbnail, 3-5 seconds of animated preview play (like YouTube's) or an animated thumbnail. Generation pipeline: (1) Extract 3 seconds of video centered at 25% mark as WebP animation or short MP4: `ffmpeg -ss {offset} -t 3 -i s3://raw/{video_id}.mp4 -vf scale=320:180,fps=5 -loop 0 s3://thumbs/{video_id}/preview.webp`. At 5fps, a 3-second WebP is 15 frames. (2) WebP animation is ~100-300 KB (vs GIF which would be 1-5 MB). (3) CDN-cached with 1-day TTL. (4) Served on hover via JavaScript: `img.src = preview_url` on `mouseenter` event. The preview generation is a post-processing step (does not block video availability). Failure to generate preview is non-critical — no alert needed, just retry in background.

**Q11: How do you implement a "fast playback" feature where the first few seconds of video are available immediately after upload, even before full transcoding?**
A: Priority transcoding: after upload completion, immediately kick off transcoding for only the first 2 minutes of video at 720p quality (a "fast preview" job). This takes ~60 seconds on GPU. Publish partial `status='partial'` event with partial HLS manifest pointing to first 60 segments (2 min). Client player reads partial manifest and can start playback. Manifest is updated as transcoding completes (HLS supports `EXT-X-PLAYLIST-TYPE:EVENT` for growing playlists). The partial HLS event playlist is replaced by the full VOD playlist when transcoding completes. This means viewers can start watching a long video within 2 minutes of upload rather than waiting 30-60 minutes for full transcoding.

**Q12: How does the pipeline handle concurrent uploads of identical content from the same user (race condition)?**
A: Two scenarios: (1) Same file, different upload sessions: deduplication query (Q8) catches this post-upload. During upload: `upload_sessions` can have multiple in-progress sessions per user (different session_ids). First to complete wins; second is detected as duplicate on completion. (2) Same upload session, concurrent requests: Redis session state uses atomic operations (`HINCRBY` for received_bytes). S3 UploadPart API is safe for concurrent part uploads (same part number, same data → same ETag; S3 is idempotent). Two concurrent requests for the same chunk_number will both succeed with the same result. Coordinator uses SETNX to ensure CompleteMultipartUpload is called only once: `SETNX assembly_lock:{session_id} 1` with EX 300 — only one process can trigger assembly.

**Q13: How would you add support for 360-degree (spherical) video uploads?**
A: Spherical video detection: check for Google's `SPHERICAL` metadata in MP4 container (stored in `udta` or `smhd` box, or XMP metadata). ffprobe can detect this. `videos.is_spherical = true` flag set. Transcoding changes: (1) Normal transcoding applies (FFmpeg treats it as regular video). (2) Equirectangular projection is standard for 360 video; preserve it. (3) Add the spherical metadata back to transcoded output (FFmpeg `-metadata:s:v:0 spherical-video="..."`). Playback: the player must render in a 3D sphere. WebXR API or A-Frame (HTML5) handles equirectangular → sphere projection in browser. Thumbnails: extract thumbnail normally (equirectangular thumbnail looks distorted but is standard practice). Storage: 4K equirectangular spherical = 8K effective resolution for the forward direction. Higher storage cost — tier the bitrate ladder higher for spherical content.

**Q14: How do you ensure the content moderation pipeline doesn't become a privacy risk (are human reviewers necessary)?**
A: Privacy framework: (1) Automated first-pass: hash-based checks (PhotoDNA for CSAM, AudioFingerprint for DMCA) require no human review — they're lookup operations. (2) ML classifiers (violence, hate speech) produce a confidence score. Low-confidence outputs require human review; high-confidence clear-pass requires none. Threshold: confidence > 0.95 passes without review; confidence > 0.5 but < 0.95 goes to review queue. (3) Human reviewers see only the flagged frames/segments, not the full video. (4) GDPR/privacy: content reviewed by humans must have creator consent (disclosed in Terms of Service). (5) Reviewer isolation: moderators have no access to user PII during review — they see the flagged clip and make a binary decision. Uploader identity disclosed only for CSAM (legal reporting requirement). (6) Reviewer annotations not linked to user data in the core DB — stored in a separate compliance database.

**Q15: How would you design the pipeline to support live-to-VOD: automatically converting a completed livestream into a VOD?**
A: Livestream recording: during a live stream, the HLS segments (2-second chunks stored in S3 for 90 seconds rolling window) are also written to a VOD prefix with no TTL expiry. When the stream ends: (1) A `stream-ended` event is published with `{stream_id, user_id, start_time, end_time}`. (2) VOD Pipeline Service consumes the event. Fetches the stream's full segment manifest from S3 (all segments written to VOD prefix during streaming). (3) Runs the assembler: concatenates all segments into a single HLS VOD playlist with `EXT-X-PLAYLIST-TYPE:VOD` and `EXT-X-ENDLIST`. (4) Triggers thumbnail generation (extracts frames from segments at 10%/50%/75% of total duration). (5) Optionally runs a re-encode pipeline for better compression (same as VOD transcoding) — triggered as a lower-priority background job. (6) Stores final VOD manifest in permanent S3 prefix; writes `vods` row in database. This is the Twitch "auto-archive" equivalent — every stream becomes a VOD automatically.

---

## 12. References & Further Reading

1. **TUS Resumable Upload Protocol** — https://tus.io/protocols/resumable-upload.html
2. **AWS S3 Multipart Upload** — https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html
3. **Google Resumable Upload Protocol** — https://cloud.google.com/storage/docs/resumable-uploads
4. **FFmpeg Documentation** — https://ffmpeg.org/documentation.html; FFmpeg NVENC guide: https://trac.ffmpeg.org/wiki/HWAccelIntro
5. **Shaka Packager (Google Open Source)** — https://github.com/google/shaka-packager
6. **CENC (Common Encryption)** — ISO/IEC 23001-7:2016. MPEG Common Encryption.
7. **Widevine DRM** — https://www.widevine.com/; Widevine Architecture: https://developers.google.com/widevine
8. **FairPlay Streaming** — Apple Developer. https://developer.apple.com/streaming/fps/
9. **PhotoDNA (CSAM detection)** — Microsoft. https://www.microsoft.com/en-us/photodna
10. **VMAF (Video Quality Assessment)** — Netflix. https://github.com/Netflix/vmaf
11. **HLS Specification** — RFC 8216. https://datatracker.ietf.org/doc/html/rfc8216
12. **MPEG-DASH** — ISO/IEC 23009-1:2022.
13. **AWS KMS Envelope Encryption** — https://docs.aws.amazon.com/kms/latest/developerguide/concepts.html#enveloping
14. **OpenTelemetry** — https://opentelemetry.io/
15. **NVENC Performance Guide** — NVIDIA. https://developer.nvidia.com/blog/nvidia-ffmpeg-transcoding-guide/
16. **Citus (Distributed PostgreSQL)** — https://www.citusdata.com/
17. **pgvector (Vector Similarity in PostgreSQL)** — https://github.com/pgvector/pgvector
18. **Content Moderation at Scale** — Tarleton, L., et al. (2019). "Human-Machine Collaboration for Fast Land Cover Mapping." Used as reference for human-in-loop ML moderation design.
19. **RFC 7233 (HTTP Range Requests)** — https://datatracker.ietf.org/doc/html/rfc7233
20. **Redis Bitfield** — https://redis.io/commands/bitfield/
