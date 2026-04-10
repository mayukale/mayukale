# System Design: Live Streaming Platform (Twitch/YouTube Live Scale)

---

## 1. Requirement Clarifications

### Functional Requirements
1. Streamers can broadcast live video from desktop/mobile via RTMP, RTMPS, or WebRTC ingest.
2. Viewers can watch a live stream in near-real-time with adaptive bitrate (ABR) playback via HLS or DASH.
3. Ultra-low-latency mode (< 2 s glass-to-glass) available for interactive use cases via WebRTC re-streaming.
4. Chat messages are delivered in real time to all viewers of a stream.
5. Viewers can rewind up to 4 hours of a live stream (DVR/time-shift).
6. Live viewer count is displayed and updated every 5 seconds.
7. Streams can be recorded and made available as VOD immediately after the broadcast ends.
8. Streamers can set stream title, category/game, thumbnail, and stream key.
9. Clips: viewers can create short clips (up to 60 s) from the last 90 seconds of a live stream.
10. Notifications: subscribers receive push/email notification when a followed streamer goes live.

### Non-Functional Requirements
1. Availability: 99.99% uptime for ingest and delivery infrastructure (< 52 min downtime/year).
2. Latency (standard HLS): end-to-end < 8 s at the 99th percentile.
3. Latency (ultra-low): end-to-end < 2 s glass-to-glass via WebRTC.
4. Scalability: support 100 k concurrent live streams; each stream up to 10 M concurrent viewers.
5. Global delivery: CDN PoPs in all major regions; viewers receive stream from the nearest edge.
6. Durability: recorded VODs stored with 11-nines durability (S3-class object store).
7. Chat: < 200 ms P99 message delivery to viewers in the same region.
8. Security: stream keys are secret; ingest endpoints protected by TLS 1.3.
9. Fault tolerance: loss of a single transcoder or edge node must not drop any live stream.

### Out of Scope
- Payment processing / subscriptions / donations
- Content moderation and automated CSAM detection pipeline
- Recommendation engine / stream discovery algorithm
- Mobile/desktop streamer application development
- Ad insertion / SCTE-35 splice points (mentioned only as design note)
- DRM / Widevine / FairPlay content protection

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Streamer | Publishes live video via RTMP/WebRTC; owns stream settings |
| Viewer | Watches stream via HLS/DASH/WebRTC; may participate in chat |
| Moderator | Elevated chat permissions; can ban/timeout users |
| Platform Admin | Manages categories, bans, infrastructure config |

### Traffic Estimates

**Assumptions:**
- 100 k concurrent live streams at peak
- Average concurrent viewers per stream: 500 (power-law distribution; most streams have 10-50 viewers, a few have millions)
- Total peak concurrent viewers: 100 k streams × 500 avg = 50 M concurrent viewers
- Average video bitrate delivered: 3 Mbps (mix of 1080p60 @ 6 Mbps, 720p @ 3 Mbps, 480p @ 1.5 Mbps)
- HLS segment duration: 2 s
- Chat messages: active streams average 5 messages/s; 20% of 100 k streams are "active chat" = 20 k × 5 = 100 k messages/s platform-wide

| Metric | Calculation | Result |
|---|---|---|
| Peak concurrent streams | given | 100 k |
| Peak concurrent viewers | 100 k × 500 | 50 M |
| Ingest bandwidth | 100 k streams × 6 Mbps (ingest at highest quality) | 600 Gbps |
| Delivery bandwidth | 50 M viewers × 3 Mbps avg | 150 Tbps |
| Chat write RPS | 20 k active streams × 5 msg/s | 100 k RPS |
| Chat read fan-out | 100 k msg/s × avg 200 viewers per active stream | 20 M deliveries/s |
| HLS segment requests | 50 M viewers × 1 segment req / 2 s | 25 M req/s |
| Viewer count updates | 100 k streams × 1 update/5 s | 20 k writes/s |
| New streams/hour | assume 10% of peak start each hour | 10 k stream starts/hr |

### Latency Requirements
| Operation | Target (P50) | Target (P99) |
|---|---|---|
| RTMP ingest to first HLS segment available | < 3 s | < 5 s |
| HLS viewer glass-to-glass latency | < 5 s | < 8 s |
| WebRTC ultra-low latency | < 500 ms | < 2 s |
| Chat message delivery (same region) | < 50 ms | < 200 ms |
| Viewer count update visible to streamer | < 5 s | < 10 s |
| Clip creation (async processing) | < 10 s | < 30 s |

### Storage Estimates
| Data Type | Calculation | Result |
|---|---|---|
| Live HLS segments on edge (2 s, 6 h DVR window) | 100 k streams × (6 h × 3600 / 2) segments × 750 KB/seg | ~810 TB edge buffer |
| VOD storage per day | 100 k streams × 2 h avg stream duration × 3600 s × 375 KB/s (3 Mbps) | ~270 TB/day |
| VOD storage per year (compressed) | 270 TB × 365 × 0.7 compression ratio | ~69 PB/year |
| Chat log (text only) | 100 k msg/s × 86400 s × 200 bytes/msg | ~1.7 TB/day |
| Stream metadata DB | 100 k concurrent + 1 M historical × 2 KB/record | ~2 GB (negligible) |

### Bandwidth Estimates
| Flow | Calculation | Result |
|---|---|---|
| Ingest → Origin transcoder | 100 k streams × 6 Mbps | 600 Gbps |
| Origin → CDN edge (push/pull) | Each stream goes to ~5 CDN PoPs; 100 k × 6 Mbps × 5 | 3 Tbps |
| CDN → Viewers | 50 M × 3 Mbps | 150 Tbps |
| Chat WS traffic | 100 k msg/s × 200 bytes × 8 bits | ~160 Mbps (trivial) |

---

## 3. High-Level Architecture

```
                         ┌──────────────────────────────────────────────────────┐
                         │                   STREAMER                           │
                         │  OBS / Streaming App                                 │
                         └──────────────────┬───────────────────────────────────┘
                                            │  RTMP/RTMPS (TCP 1935)
                                            ▼
                         ┌──────────────────────────────────────────────────────┐
                         │          INGEST CLUSTER  (Regional)                  │
                         │  ┌────────────┐  ┌────────────┐  ┌────────────┐     │
                         │  │ RTMP Edge  │  │ RTMP Edge  │  │ RTMP Edge  │     │
                         │  │ (Anycast)  │  │            │  │            │     │
                         │  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘     │
                         └────────┼───────────────┼───────────────┼────────────┘
                                  │               │               │
                                  └───────────────┼───────────────┘
                                                  │  RTMP relay (internal)
                                                  ▼
                         ┌──────────────────────────────────────────────────────┐
                         │         TRANSCODING CLUSTER                          │
                         │  ┌──────────────────────────────────────────────┐   │
                         │  │  Stream Router / Job Scheduler               │   │
                         │  │  (assigns stream → transcoder pod)           │   │
                         │  └─────────────────┬────────────────────────────┘   │
                         │                    │  assigns                        │
                         │    ┌───────────────▼──────────────────────┐         │
                         │    │  FFmpeg Transcoder Pods (k8s)        │         │
                         │    │  Per stream: 360p/480p/720p/1080p    │         │
                         │    │  H.264 + AAC; HLS segments (2s)      │         │
                         │    └───────────────┬──────────────────────┘         │
                         └───────────────────┼──────────────────────────────────┘
                                             │  segment upload
                                             ▼
                         ┌───────────────────────────────────────────────────────┐
                         │   ORIGIN STORAGE LAYER                                │
                         │   ┌──────────────────────────────────────────────┐   │
                         │   │  Object Store (S3-compatible)                │   │
                         │   │  /live/{stream_id}/{rendition}/seg_NNNN.ts   │   │
                         │   │  /live/{stream_id}/master.m3u8               │   │
                         │   │  DVR window: last 4h segments retained hot  │   │
                         │   │  Full VOD: moved to cold tier after stream   │   │
                         │   └──────────────────────────────────────────────┘   │
                         │   ┌──────────────────────────────────────────────┐   │
                         │   │  Playlist Service                            │   │
                         │   │  Generates/updates .m3u8 manifests per tick  │   │
                         │   └──────────────────────────────────────────────┘   │
                         └────────────────────────┬──────────────────────────────┘
                                                  │  HTTP pull / push
                                                  ▼
                         ┌───────────────────────────────────────────────────────┐
                         │          CDN LAYER  (Global PoPs)                     │
                         │  ┌───────────┐  ┌───────────┐  ┌───────────┐        │
                         │  │  Edge PoP │  │  Edge PoP │  │  Edge PoP │        │
                         │  │  NA-East  │  │  EU-West  │  │  AP-SE    │        │
                         │  │  Cache:   │  │  Cache:   │  │  Cache:   │        │
                         │  │  segments │  │  segments │  │  segments │        │
                         │  │  manifests│  │  manifests│  │  manifests│        │
                         │  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘        │
                         └────────┼───────────────┼───────────────┼─────────────┘
                                  │               │               │  HLS/DASH
                                  ▼               ▼               ▼
                              VIEWERS         VIEWERS         VIEWERS
                           (HLS Players)  (HLS Players)  (HLS Players)

    ┌──────────────────────────────────────────────────────────────────────────────┐
    │  REAL-TIME SERVICES (separate cluster)                                       │
    │                                                                              │
    │  ┌─────────────────────────────────────────────────────────────────────┐    │
    │  │  Chat Service                                                        │    │
    │  │  WebSocket Gateway (horizontally scaled, sticky sessions via L4 LB) │    │
    │  │  Pub/Sub backbone: Redis Cluster / Kafka                            │    │
    │  │  Chat history: Cassandra (time-series writes)                       │    │
    │  └─────────────────────────────────────────────────────────────────────┘    │
    │                                                                              │
    │  ┌─────────────────────────────────────────────────────────────────────┐    │
    │  │  Viewer Count Service                                                │    │
    │  │  HyperLogLog counters in Redis; streamed to dashboards via SSE      │    │
    │  └─────────────────────────────────────────────────────────────────────┘    │
    │                                                                              │
    │  ┌─────────────────────────────────────────────────────────────────────┐    │
    │  │  Notification Service                                                │    │
    │  │  Kafka consumer; fan-out to APNs/FCM/email on stream start event    │    │
    │  └─────────────────────────────────────────────────────────────────────┘    │
    └──────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **RTMP Ingest Edge**: Accepts raw RTMP stream; terminates TCP; relays audio/video to transcoder cluster via internal RTMP or SRT. Uses Anycast IP for lowest RTT from streamer.
- **Stream Router**: Watches ingest events; picks a transcoder pod with available GPU/CPU capacity; registers stream-to-pod mapping in a coordination store (etcd/ZooKeeper).
- **FFmpeg Transcoder Pods**: Decodes incoming stream; encodes multiple renditions (360p, 480p, 720p, 1080p) concurrently using hardware encoders (NVENC/Intel QSV); muxes into 2-second HLS TS segments; uploads each segment to origin object store.
- **Playlist Service**: Stateless microservice that reads segment availability from object store and generates/updates the HLS `.m3u8` variant playlists. Keeps a sliding window for DVR.
- **Origin Object Store**: Source of truth for all segments and manifests; S3-compatible (AWS S3 / GCS / Azure Blob). Provides durability; CDN pulls from here on cache miss.
- **CDN Edge PoPs**: Cache .ts segments and .m3u8 manifests near viewers. Manifest TTL is short (1–2 s) to ensure freshness; segment TTL is long (hours) since segments are immutable once written.
- **Chat Service**: Manages WebSocket connections; brokers messages through Redis pub/sub; persists to Cassandra; enforces rate limits and moderation rules.
- **Viewer Count Service**: Counts unique viewers using HyperLogLog per stream in Redis; publishes counts via Server-Sent Events to streamer dashboard.

**Primary Data Flow (Streamer to Viewer):**
1. Streamer's OBS pushes RTMP stream to the nearest ingest edge (Anycast routing).
2. Ingest edge relays to an assigned transcoder pod.
3. Transcoder generates 2-second .ts segments for each rendition; uploads to S3 `live/{stream_id}/{rendition}/segN.ts`.
4. Playlist Service polls S3 and rewrites the `.m3u8` manifest, appending the new segment.
5. CDN PoP serving a viewer pulls the manifest (cache miss or TTL expired) from origin; caches it for 1 s.
6. Viewer's HLS player fetches the updated manifest; requests latest segment URL; CDN PoP serves from cache (cache hit for .ts files > 99% after first viewer).
7. Player buffers 2–3 segments, then plays back; end-to-end latency ≈ 3× segment duration + transcoding time + network RTT ≈ 6–8 s.

---

## 4. Data Model

### Entities & Schema

```sql
-- Stream metadata
CREATE TABLE streams (
    stream_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    title           VARCHAR(140) NOT NULL,
    category_id     INTEGER REFERENCES categories(category_id),
    stream_key      VARCHAR(64) NOT NULL UNIQUE,    -- secret; hashed in DB
    status          VARCHAR(16) NOT NULL DEFAULT 'offline',  -- offline|live|ended
    started_at      TIMESTAMPTZ,
    ended_at        TIMESTAMPTZ,
    thumbnail_url   TEXT,
    language        CHAR(5),
    is_mature       BOOLEAN DEFAULT FALSE,
    ingest_region   VARCHAR(32),                    -- e.g. 'us-east-1'
    ingest_pod      VARCHAR(64),                    -- assigned transcoder pod
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_streams_user_id   ON streams(user_id);
CREATE INDEX idx_streams_status    ON streams(status) WHERE status = 'live';
CREATE INDEX idx_streams_category  ON streams(category_id, status);

-- Users
CREATE TABLE users (
    user_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    username        VARCHAR(25) NOT NULL UNIQUE,
    display_name    VARCHAR(50),
    email           VARCHAR(254) NOT NULL UNIQUE,
    password_hash   TEXT NOT NULL,
    avatar_url      TEXT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    is_banned       BOOLEAN DEFAULT FALSE
);

-- Stream renditions (one row per quality level per stream)
CREATE TABLE stream_renditions (
    rendition_id    BIGSERIAL PRIMARY KEY,
    stream_id       UUID NOT NULL REFERENCES streams(stream_id),
    rendition_name  VARCHAR(16) NOT NULL,   -- '360p', '480p', '720p', '1080p'
    width           INTEGER NOT NULL,
    height          INTEGER NOT NULL,
    fps             INTEGER NOT NULL,
    video_bitrate   INTEGER NOT NULL,       -- kbps
    audio_bitrate   INTEGER NOT NULL,       -- kbps
    codec           VARCHAR(16) DEFAULT 'h264',
    manifest_path   TEXT NOT NULL,          -- S3 key
    UNIQUE (stream_id, rendition_name)
);

-- Segment tracking (used by Playlist Service; NOT replicated to every DB)
-- Stored in Redis sorted set: key=stream:{id}:rendition:{name}  score=segment_number
-- Persisted to object store manifest; DB row only for DVR metadata
CREATE TABLE stream_segments (
    segment_id      BIGSERIAL PRIMARY KEY,
    stream_id       UUID NOT NULL,
    rendition_name  VARCHAR(16) NOT NULL,
    segment_number  INTEGER NOT NULL,
    duration_ms     INTEGER NOT NULL,
    storage_path    TEXT NOT NULL,          -- S3 key
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (stream_id, rendition_name, segment_number)
) PARTITION BY HASH (stream_id);  -- partition for write throughput

-- Chat messages (Cassandra schema shown as CQL)
-- Cassandra table for chat persistence
/*
CREATE TABLE chat_messages (
    stream_id   UUID,
    bucket      INT,          -- partition by (unix_ts / 3600) to limit partition size
    message_id  TIMEUUID,
    user_id     UUID,
    username    TEXT,
    body        TEXT,
    badges      LIST<TEXT>,
    color       TEXT,
    deleted     BOOLEAN,
    PRIMARY KEY ((stream_id, bucket), message_id)
) WITH CLUSTERING ORDER BY (message_id ASC)
  AND default_time_to_live = 604800;  -- 7-day TTL for chat history
*/

-- Viewer count snapshots (time-series; stored in TimescaleDB or InfluxDB)
CREATE TABLE viewer_count_snapshots (
    stream_id       UUID NOT NULL,
    recorded_at     TIMESTAMPTZ NOT NULL,
    viewer_count    INTEGER NOT NULL,
    PRIMARY KEY (stream_id, recorded_at)
);

-- Follows
CREATE TABLE follows (
    follower_id     UUID NOT NULL REFERENCES users(user_id),
    followed_id     UUID NOT NULL REFERENCES users(user_id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    notifications   BOOLEAN DEFAULT TRUE,
    PRIMARY KEY (follower_id, followed_id)
);
CREATE INDEX idx_follows_followed ON follows(followed_id);

-- Clips
CREATE TABLE clips (
    clip_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    stream_id       UUID NOT NULL REFERENCES streams(stream_id),
    creator_id      UUID NOT NULL REFERENCES users(user_id),
    title           VARCHAR(140),
    start_offset_s  INTEGER NOT NULL,   -- seconds from stream start
    duration_s      INTEGER NOT NULL,   -- 5–60 seconds
    storage_path    TEXT,               -- S3 key of rendered clip
    status          VARCHAR(16) DEFAULT 'processing',  -- processing|ready|failed
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| PostgreSQL | Stream metadata, users, follows | ACID, rich queries, mature ecosystem | Vertical scaling limit; requires sharding at extreme scale | **Selected** for metadata |
| Cassandra | Chat messages | Wide-column, excellent write throughput, time-series naturally modeled, multi-DC replication | No joins, eventual consistency, schema rigidity | **Selected** for chat |
| Redis Cluster | Viewer count (HyperLogLog), segment index (sorted set), chat pub/sub | Sub-millisecond latency, HyperLogLog native, pub/sub built-in | In-memory cost, persistence needs AOF/RDB | **Selected** for real-time data |
| S3-compatible object store | Segments, manifests, VOD | Infinitely scalable, durable (11 9s), cheap at scale, CDN-friendly | Eventual consistency on LIST ops (mitigated by Playlist Service) | **Selected** for media |
| TimescaleDB | Viewer count time-series | SQL interface, hypertable auto-partitioning, retention policies | Requires PG tuning | **Selected** for analytics |
| MySQL | — | Familiar, wide ecosystem | InnoDB row locking under high write concurrency for time-series | Not selected |
| DynamoDB | Stream metadata | Serverless scaling | Vendor lock-in, limited query patterns, cost at scale | Not selected |

**Justification of key choices:**
- **Cassandra for chat**: Chat is an append-only time-series workload with very high write rates (100 k/s platform-wide). Cassandra's LSM-tree storage handles sustained writes without write amplification bottlenecks. The `(stream_id, bucket)` composite partition key keeps individual partitions bounded (< 100 MB), avoiding the Cassandra "large partition" anti-pattern.
- **Redis HyperLogLog for viewer count**: HyperLogLog provides O(1) cardinality estimation with < 1% error at the cost of only 12 KB per stream regardless of viewer count. This is far more memory-efficient than maintaining a set of viewer IDs. The `PFADD` / `PFCOUNT` commands are exactly fit for purpose.
- **PostgreSQL for metadata**: Stream metadata operations are low-volume (100 k stream starts/day) and benefit from relational integrity (foreign keys between streams, users, categories). Reads are served by replicas; writes go to primary. At extreme scale, shard by `user_id` using Citus.

---

## 5. API Design

### Ingest API (Internal — called by streamer application)

```
POST /ingest/authorize
  Auth: Bearer <stream_key>
  Body: { "app": "live", "stream_name": "<stream_key>" }
  Response: 200 { "authorized": true, "stream_id": "<uuid>", "transcoder_endpoint": "rtmp://..." }
  Rate limit: 10 req/min per IP

POST /ingest/stream_end
  Auth: Bearer <ingest_token>
  Body: { "stream_id": "<uuid>" }
  Response: 204
```

### Stream Management API (REST — called by streamer dashboard)

```
PUT /api/v1/streams/active
  Auth: Bearer <jwt>
  Body: {
    "title": "string (max 140)",
    "category_id": integer,
    "language": "en",
    "is_mature": false
  }
  Response: 200 { stream }
  Rate limit: 30 req/min per user

GET /api/v1/streams/:stream_id
  Auth: Optional (public info); Bearer required for private fields
  Response: 200 { stream_id, title, status, viewer_count, started_at, ... }
  Rate limit: 600 req/min per IP (CDN cached)

GET /api/v1/streams/:stream_id/playback
  Auth: Optional
  Response: 200 {
    "hls_url": "https://cdn.example.com/live/{stream_id}/master.m3u8",
    "webrtc_url": "wss://edge.example.com/webrtc/{stream_id}",
    "low_latency_hls": "https://cdn.example.com/live/{stream_id}/ll-master.m3u8"
  }
  Rate limit: 1000 req/min per IP
```

### Chat API (WebSocket)

```
WebSocket: wss://chat.example.com/ws?stream_id=<uuid>
  Auth: JWT in query param or initial handshake frame

Client → Server frames:
  { "type": "JOIN",    "stream_id": "<uuid>" }
  { "type": "MESSAGE", "body": "Hello!", "stream_id": "<uuid>" }
  { "type": "DELETE",  "message_id": "<timeuuid>" }   -- moderators only
  { "type": "PING" }

Server → Client frames:
  { "type": "JOINED",   "history": [ last 50 messages ] }
  { "type": "MESSAGE",  "message_id": "<timeuuid>", "user_id": "...",
    "username": "...", "body": "...", "badges": [], "color": "#FF4500",
    "ts": 1712345678.123 }
  { "type": "VIEWER_COUNT", "count": 14382 }
  { "type": "STREAM_END" }
  { "type": "PONG" }

Rate limits:
  - 20 messages/30 s per user (soft), 1 message/0.3 s (hard — token bucket)
  - Authenticated users only can send messages
  - Moderators: no rate limit on moderation actions
```

### Viewer Count API (SSE)

```
GET /api/v1/streams/:stream_id/viewer-count
  Auth: Optional
  Accept: text/event-stream
  Response: SSE stream, event every 5 s:
    data: {"stream_id": "<uuid>", "count": 14382, "ts": 1712345678}
  Rate limit: 1 connection per viewer per stream
```

### Clip API

```
POST /api/v1/clips
  Auth: Bearer <jwt>
  Body: { "stream_id": "<uuid>", "duration_s": 30 }
  Response: 202 { "clip_id": "<uuid>", "status": "processing" }
  Rate limit: 5 clips/min per user

GET /api/v1/clips/:clip_id
  Auth: Optional
  Response: 200 { clip_id, status, url (when ready), title, duration_s }
```

---

## 6. Deep Dive: Core Components

### 6.1 Adaptive Bitrate Transcoding Pipeline

**Problem it solves:**
Viewers have wildly different network conditions (1 Mbps mobile to 1 Gbps fiber). A single bitrate stream either wastes bandwidth or buffers constantly. We need to produce multiple renditions from a single ingest stream in real time, with < 3 s end-to-end latency from ingest to segment availability.

**Approaches Comparison:**

| Approach | Latency | Cost | Complexity | Failure Isolation |
|---|---|---|---|---|
| Single-machine FFmpeg (all renditions) | Low | Low | Low | Single point of failure per stream |
| GPU-accelerated per-stream pod (NVENC) | Low | Medium | Medium | Pod failure = stream dropped (mitigated by re-assignment) |
| Distributed ladder (one pod per rendition) | Very low (parallel) | High | High | Rendition-level isolation |
| Cloud transcoding API (MediaLive, etc.) | Low | Very high | Low | Vendor-managed |
| Browser-side transcoding (client ABR) | N/A | Minimal | Very high | No server cost but no quality control |

**Selected approach: GPU-accelerated per-stream pod with rendition parallelism.**

Each stream is assigned to a transcoder pod running on a GPU instance (e.g., AWS p3/g4dn). The pod runs FFmpeg with NVENC hardware encoding. All renditions are produced in a single FFmpeg command using multiple `-filter_complex` outputs, sharing the same decoder pass. This achieves:
- Single decode pass (1× CPU decode cost regardless of rendition count)
- Parallel NVENC encoding for 4 renditions (GPU handles concurrency natively)
- Segment upload to S3 triggered by FFmpeg's `hls_segment_filename` and `hls_flags delete_segments` via a custom segment completion hook

**Pseudocode / FFmpeg command structure:**

```
# Stream Router assigns a stream to a pod:
function assign_transcoder(stream_id, ingest_url):
    pod = select_least_loaded_pod(available_pods)  # min-heap on active_streams count
    reserve_slot(pod, stream_id)
    notify_pod(pod, { action: START, stream_id: stream_id, ingest_url: ingest_url })
    etcd.put(f"streams/{stream_id}/pod", pod.id)
    return pod

# On pod — segment upload hook (called by FFmpeg on each segment completion)
function on_segment_ready(stream_id, rendition, segment_path, segment_number):
    s3_key = f"live/{stream_id}/{rendition}/seg{segment_number:06d}.ts"
    s3.upload(segment_path, s3_key, content_type="video/MP2T",
              cache_control="max-age=86400, immutable")
    redis.zadd(f"stream:{stream_id}:{rendition}:segs", {s3_key: segment_number})
    playlist_service.notify(stream_id, rendition, segment_number)

# FFmpeg command (simplified)
ffmpeg \
  -i rtmp://ingest_relay/{stream_id} \
  -filter_complex "[0:v]split=4[v1][v2][v3][v4]" \
  -map "[v1]" -map 0:a -vf scale=1920:1080 -c:v h264_nvenc -b:v 6000k -preset p4 \
    -hls_time 2 -hls_list_size 0 -hls_segment_filename /tmp/{id}/1080p/seg%06d.ts \
    /tmp/{id}/1080p/index.m3u8 \
  -map "[v2]" -map 0:a -vf scale=1280:720  -c:v h264_nvenc -b:v 3000k -preset p4 \
    -hls_time 2 -hls_list_size 0 -hls_segment_filename /tmp/{id}/720p/seg%06d.ts  \
    /tmp/{id}/720p/index.m3u8 \
  -map "[v3]" -map 0:a -vf scale=854:480   -c:v h264_nvenc -b:v 1500k -preset p4 \
    -hls_time 2 -hls_list_size 0 -hls_segment_filename /tmp/{id}/480p/seg%06d.ts  \
    /tmp/{id}/480p/index.m3u8 \
  -map "[v4]" -map 0:a -vf scale=640:360   -c:v h264_nvenc -b:v 600k  -preset p4 \
    -hls_time 2 -hls_list_size 0 -hls_segment_filename /tmp/{id}/360p/seg%06d.ts  \
    /tmp/{id}/360p/index.m3u8

# Playlist Service (runs each 1-2s per active stream):
function update_playlist(stream_id, rendition, new_segment_number):
    segments = redis.zrange(f"stream:{stream_id}:{rendition}:segs",
                            new_segment_number - DVR_WINDOW_SEGS, new_segment_number)
    m3u8 = build_m3u8(segments, target_duration=2, media_sequence=max(0, new_segment_number - DVR_WINDOW_SEGS))
    s3.put(f"live/{stream_id}/{rendition}/index.m3u8", m3u8,
           cache_control="max-age=1, must-revalidate")
```

**Interviewer Q&A:**

Q1: What happens if the transcoder pod crashes mid-stream?
A1: The Stream Router detects pod failure via its health heartbeat (etcd lease expiry within 5 s). It immediately re-assigns the stream to a healthy pod. The new pod reconnects to the ingest relay RTMP endpoint (which stays alive since the ingest edge is separate). There is a ~5–10 s gap in segments during failover. Viewers experience a brief re-buffer; the HLS player retries the manifest and resumes from the new segment sequence. DVR continuity is preserved because the already-uploaded segments remain in S3.

Q2: Why use 2-second HLS segments instead of shorter segments like 500 ms?
A2: Shorter segments reduce latency but increase segment request overhead. With 2-second segments: a viewer at 3 Mbps makes 1 HTTPS request every 2 seconds for each rendition — manageable. At 500 ms, that's 8× more requests, which stresses CDN origin-pull and HTTP/2 connection multiplexing. Low-Latency HLS (LHLS) with HTTP/2 chunked transfer encoding achieves sub-2 s latency with 2-second segments by delivering partial segments. We use LHLS for the low-latency tier.

Q3: How do you handle keyframe alignment across renditions?
A3: The FFmpeg command uses `-force_key_frames "expr:gte(t,n_forced*2)"` to force a keyframe exactly every 2 seconds. This ensures all renditions have keyframes at the same timestamps, which is a hard requirement for seamless ABR switching in HLS — the player can only switch renditions at keyframe boundaries.

Q4: How does the Playlist Service avoid race conditions when multiple segments arrive simultaneously?
A4: Redis sorted sets are atomic. The `ZADD` command for adding a segment number is atomic, and `ZRANGE` reads are consistent snapshots. The Playlist Service uses a single-writer model per stream: each stream's playlist updates are serialized through a Redis stream (XADD/XREAD) consumer group with one consumer per stream, eliminating race conditions.

Q5: What's the GPU capacity planning for 100 k concurrent streams?
A5: A single NVIDIA A10G GPU can encode ~50 streams at 4-rendition 1080p60 using NVENC at quality preset p4. So 100 k streams / 50 streams/GPU = 2,000 GPU instances needed at peak. With spot instances at ~$0.30/hr each, that's ~$600/hr or ~$5.2M/year in compute for transcoding alone. In practice, most streams are lower resolution (720p or less), so one GPU handles ~100 streams, halving costs to ~$2.6M/year. This also justifies the investment in per-stream GPU assignment versus cheaper CPU-only encoding.

---

### 6.2 CDN Edge Caching Strategy for Live Streams

**Problem it solves:**
50 M concurrent viewers cannot all pull from a single origin. Without caching, each viewer requesting the same 2-second segment would create 50 M × (1 req/2 s) = 25 M origin requests per second — impossible to serve. CDN edge nodes must serve segments from cache with a cache hit rate > 99%.

**Approaches Comparison:**

| Approach | Hit Rate | Latency | Complexity | Freshness |
|---|---|---|---|---|
| Push-based: origin pushes segments to all PoPs | ~100% | Lowest | High (push fan-out) | Perfect |
| Pull-through: CDN pulls on first viewer miss | High after warm-up | Higher on miss | Low | Good |
| Hybrid: push manifest, pull segments | Very high | Low | Medium | Excellent |
| P2P CDN assist (WebRTC data channel between viewers) | Supplement only | Variable | Very high | Viewer-controlled |

**Selected: Hybrid push-manifest / pull-segment with segment pre-warming.**

- The Playlist Service pushes updated manifests to CDN PoPs via CDN API (cache invalidation + pre-push) every 2 seconds. This ensures every PoP always has the latest manifest without a cache miss.
- `.ts` segments are pulled on first request (pull-through). Since segments are immutable and served to many viewers, after the first viewer at a PoP triggers the pull, all subsequent viewers at that PoP hit the cache. Cache TTL for segments is 24 hours (or until DVR window expires, whichever is shorter).
- For very large streams (> 100 k viewers), the segment cache is pre-warmed: the Playlist Service detects high-viewer streams and sends a "warm" request to all PoPs immediately after a new segment is uploaded to S3, before any viewer requests it.

**Cache key design:**
```
Cache key:  /live/{stream_id}/{rendition}/seg{N:06d}.ts
TTL:        86400s (immutable content; segment will never change)
Vary:       none (not varying on Accept-Encoding for binary media)

Cache key:  /live/{stream_id}/{rendition}/index.m3u8
TTL:        1s (must-revalidate; CDN revalidates with If-None-Match)
Stale-while-revalidate: 1s (serve stale while fetching fresh)
```

**Pseudocode for high-stream pre-warming:**
```
function on_segment_uploaded(stream_id, rendition, segment_number, s3_url):
    viewer_count = redis.pfcount(f"viewers:{stream_id}")
    if viewer_count > PREWARM_THRESHOLD:   # e.g., 10,000
        target_pops = cdn_api.get_all_pops()
        for pop in target_pops:
            cdn_api.prewarm(pop, url=s3_url)  # async, fire-and-forget
```

**Interviewer Q&A:**

Q1: How do you handle CDN cache stampede when a stream first goes live?
A1: When a stream first goes live, the first manifest request from each PoP goes to origin — a cache miss. With thousands of viewers hitting all PoPs simultaneously, this can overwhelm origin. We mitigate with: (1) Playlist Service pushes the first manifest to all PoPs immediately on stream start (proactive push via CDN API), and (2) origin servers use request coalescing — multiple simultaneous cache misses for the same URL result in only one backend request (CDN "request collapsing" / "thundering herd protection"), which is a built-in feature of Fastly and Cloudfront.

Q2: How does DVR/time-shifting work with CDN caching?
A2: DVR segments are immutable objects in S3 with long TTLs. The viewer's player, when seeking to a past time, calculates the correct segment number from the DVR manifest and requests that segment URL. Since the URL includes the segment number (immutable), CDN caches it indefinitely. The DVR manifest (served as a separate "DVR" variant of the .m3u8) lists all segments from the last 4 hours. This manifest is regenerated every 2 seconds and has a 1-second TTL; segments themselves are permanently cached.

Q3: What CDN vendor would you choose and why?
A3: At this scale, a multi-CDN strategy using both Fastly (for real-time purge/push capabilities via their Instant Purge API, critical for manifest freshness) and Akamai/CloudFront (for raw capacity in Asia-Pacific where Fastly PoP density is lower). Origin Shield (CDN-to-origin intermediary layer) is configured in each region to collapse all PoP-to-origin requests through a single regional intermediary, reducing origin load by ~10×.

Q4: How do you handle a CDN PoP going offline?
A4: DNS-based failover: the CDN vendor's Anycast routing automatically redirects viewers to the next-nearest PoP. Recovery time < 1 minute (DNS TTL = 60 s). For the origin, we use an active-active S3 setup across two AWS regions with S3 Cross-Region Replication. If one region goes down, CDN simply pulls from the healthy region.

Q5: How do you reduce CDN costs for a long-tail of low-viewer streams?
A5: Streams with < 100 viewers are served without CDN: the origin object store (S3) has a CloudFront distribution with default settings. The low request rate (< 50 req/s per stream) does not justify advanced pre-warming. S3 per-request pricing is ~$0.0004/10k requests, so 50 req/s × 86400 s × $0.0004/10k = ~$0.17/stream/day, trivially cheap. High-viewer streams get dedicated origin shield + pre-warming treatment.

---

### 6.3 Chat Fan-out at Scale

**Problem it solves:**
When a popular stream has 1 M concurrent viewers and someone sends a chat message, that message must be delivered to all 1 M WebSocket connections within 200 ms. A naïve loop over 1 M connections in a single thread would take seconds. The fan-out problem is the central scaling challenge for chat.

**Approaches Comparison:**

| Approach | Fan-out Latency | Scalability | Memory | Consistency |
|---|---|---|---|---|
| Single server, loop all connections | O(N) — terrible at scale | None | O(N) | Strong |
| Redis pub/sub per stream channel | Sub-100 ms | Good (thousands of subscribers per channel) | Low | Eventual (network ordering) |
| Kafka per-stream topic | Sub-500 ms | Excellent | Low (consumers pull) | Ordered per partition |
| NATS JetStream broadcast | Sub-50 ms | Excellent | Low | At-least-once |
| Distributed WS gateways + Redis pub/sub | Sub-100 ms | Excellent (scale gateways independently) | Medium | Eventual |

**Selected: Distributed WebSocket Gateway cluster + Redis Cluster pub/sub.**

Architecture:
- WebSocket Gateway pods: stateless; each pod maintains WebSocket connections. Clients connect to any pod (L4 load balancer with consistent hashing on `stream_id` to reduce cross-pod pub/sub overhead; not required for correctness).
- When a viewer connects to stream X, the gateway pod subscribes to Redis channel `chat:stream:{stream_id}`.
- When a message is sent to stream X, the chat service publishes to `chat:stream:{stream_id}` in Redis.
- All subscribed gateway pods receive the message and forward it to their connected viewers.
- Redis Cluster shards channels across nodes; each channel for a stream is handled by one Redis node. A single Redis node can handle ~100 k publish operations/s and ~100 k subscriber deliveries/s.
- For mega-streams (> 500 k viewers), we shard the viewer fan-out: use 10 Redis channels per stream (`chat:stream:{stream_id}:{shard_id}` where shard 0–9). Each gateway pod subscribes only to the shards it needs. This distributes the publish load across 10 Redis nodes.

**Pseudocode:**
```
# Gateway pod: handle incoming WebSocket message
async function handle_ws_message(connection, frame):
    msg = parse(frame)
    if msg.type == "JOIN":
        stream_id = msg.stream_id
        connection.stream_id = stream_id
        redis.subscribe(f"chat:stream:{stream_id}")
        history = cassandra.query("SELECT * FROM chat_messages WHERE stream_id=? AND bucket=? ORDER BY message_id DESC LIMIT 50",
                                  stream_id, current_bucket())
        connection.send({ type: "JOINED", history: history })

    elif msg.type == "MESSAGE":
        if not rate_limiter.allow(connection.user_id):
            connection.send({ type: "ERROR", code: "RATE_LIMITED" })
            return
        message = {
            message_id: uuid_v1(),  # time-based for ordering
            stream_id: msg.stream_id,
            user_id: connection.user_id,
            username: connection.username,
            body: sanitize(msg.body),  # strip HTML, enforce max length
            badges: connection.badges,
            ts: now()
        }
        # Persist asynchronously (fire-and-forget with retry)
        kafka.produce("chat-persist", key=stream_id, value=message)
        # Fan-out synchronously via Redis
        redis.publish(f"chat:stream:{msg.stream_id}", json(message))

# Redis pub/sub callback on gateway pod
function on_redis_message(channel, data):
    stream_id = extract_stream_id(channel)
    message = parse(data)
    for connection in local_connections_for_stream(stream_id):
        connection.send_async(message)  # non-blocking write to WS buffer

# Persistence consumer (separate service, reads Kafka "chat-persist" topic)
function persist_chat_message(message):
    bucket = int(message.ts / 3600)
    cassandra.execute(
        "INSERT INTO chat_messages (stream_id, bucket, message_id, user_id, username, body, badges) VALUES (?,?,?,?,?,?,?)",
        message.stream_id, bucket, message.message_id, message.user_id,
        message.username, message.body, message.badges
    )
```

**Interviewer Q&A:**

Q1: How do you handle a Redis node failure during peak load?
A1: Redis Cluster automatically promotes a replica to primary within ~5-10 seconds. During this window, pub/sub messages to channels on the failed primary are lost — chat messages during that window are dropped (at-most-once delivery). This is acceptable for chat (users understand real-time chat can have gaps). For streams requiring higher guarantee, we could use Redis Streams with consumer groups (at-least-once) or NATS JetStream. After failover, the pub/sub channels are re-established automatically as gateway pods reconnect.

Q2: How do you scale to 1 M simultaneous WebSocket connections?
A2: Each WebSocket gateway pod, using an async I/O framework (Go goroutines or Node.js event loop), can maintain ~100 k concurrent WebSocket connections with ~2 KB memory per connection = 200 MB RAM per pod for connections alone. For 1 M connections: 10 gateway pods. The L4 load balancer distributes new connections. Existing connections remain on their pod (no migration needed — pub/sub handles cross-pod delivery).

Q3: How do you prevent chat spam from bots?
A3: Multi-layer defense: (1) Token bucket rate limiter per user_id (20 messages/30 s enforced in the gateway pod's in-memory rate limiter, not Redis, to avoid round-trips); (2) Account age gate: accounts < 5 minutes old are silently dropped; (3) Duplicate message detection: SHA-256 hash of last 5 messages per user stored in Redis with 10 s TTL — identical messages within 10 s are rejected; (4) Moderators can `/ban` or `/timeout` a user, writing to a Redis blocklist checked on each message; (5) ML-based toxicity scoring runs asynchronously (Kafka consumer) and auto-deletes flagged messages.

Q4: What's the consistency model for chat and is it acceptable?
A4: Chat has eventual consistency. Message ordering per-stream is approximately chronological (Redis pub/sub delivers to all subscribers in the order published, but different gateway pods may receive and forward in different order). For most streams this is imperceptible. For streams requiring strict ordering, we could sequence messages with a Redis INCR counter and have clients sort by sequence number. The 200 ms P99 delivery guarantee is met by the Redis pub/sub path; Cassandra persistence is asynchronous and does not add to delivery latency.

Q5: How do you handle a chat "storm" when a streamer goes live to 2 M subscribers simultaneously?
A5: The inrush of WebSocket connections causes a thundering herd. Mitigations: (1) Staggered notifications — the notification service fans out to subscribers in batches with jitter (0–30 s random delay), spreading the connection spike over 30 seconds; (2) WebSocket gateway autoscaling has a pre-scale rule: when a stream is predicted to be large (based on follower count), scale up gateway pods 2 minutes before the scheduled start; (3) Redis pub/sub subscription is lazy — gateway pods don't subscribe to a stream channel until the first viewer on that pod joins, avoiding empty subscription overhead.

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling Strategy | Notes |
|---|---|---|
| RTMP Ingest | Anycast + stateless relay pods; scale by stream count | Each relay handles ~500 concurrent RTMP streams |
| Transcoder pods | Horizontal; auto-scale on GPU utilization metric | 1 pod per ~50 streams on GPU instances |
| Playlist Service | Stateless; scale by active stream count | Partition streams by hash across pods |
| Object Store (S3) | Infinite horizontal scaling (managed service) | Prefix-based sharding to avoid S3 rate limits per prefix |
| CDN | Managed; scales with traffic automatically | Multi-CDN for geographic coverage |
| WebSocket Gateway | Horizontal; scale on connection count metric | Target < 80 k connections per pod |
| Redis Cluster | Add shards; re-shard online | Keep each shard < 50 GB RAM; use Redis Cluster with 16 shards at full scale |
| Cassandra | Add nodes; data rebalances automatically | Replication factor = 3; one DC per major region |
| PostgreSQL | Read replicas for metadata reads; primary handles writes | Shard by user_id using Citus for > 10 TB |

### DB Sharding
- **Chat (Cassandra)**: Natural sharding via consistent hashing on `(stream_id, bucket)` partition key. Data is automatically distributed across nodes. Add nodes to increase throughput; Cassandra handles rebalancing.
- **Stream metadata (PostgreSQL)**: At current scale (100 k streams/day), a single primary with 5 read replicas handles all load. At 10× scale, shard by `user_id % N` using Citus extension, keeping all streams for a user on the same shard for efficient joins.
- **S3 segments**: S3 prefixes are distributed across partitions by hash. Use `{stream_id}/{rendition}/` prefixes — since `stream_id` is a UUID, the hex distribution is uniform, naturally distributing across S3's internal partitions (S3 can handle 5,500 GET requests/s per prefix before needing prefix-level sharding).

### Replication
- PostgreSQL: synchronous replication to 1 standby (RPO = 0); 4 async read replicas (RPO = seconds).
- Cassandra: RF=3 per DC with LOCAL_QUORUM writes and reads; EACH_QUORUM for cross-DC consistency when needed.
- Redis: each primary has 1 replica; Sentinel manages failover. Cluster mode with 16 primaries = 16 replicas total.
- S3: Cross-Region Replication from us-east-1 → eu-west-1 for DR.

### Caching
| Layer | Cache | TTL | Purpose |
|---|---|---|---|
| Viewer → CDN | CDN edge (Fastly) | 1 s manifest / 24 h segment | Serve video to millions of viewers |
| API server → DB | Redis (application cache) | 30 s stream metadata | Reduce DB reads for popular stream info |
| Gateway pod | In-memory LRU | 60 s | User badge/permission lookup |
| Playlist Service | Redis sorted set | Live | Segment index for manifest generation |

### CDN Strategy
- Multi-CDN: primary CDN (Fastly) handles 80% of traffic; overflow CDN (CloudFront) handles spikes.
- Origin Shield: single regional aggregation point per CDN region reduces origin request rate by ~10×.
- Manifest pre-push: critical for liveness guarantee; manifests pushed to CDN PoPs every 2 s by Playlist Service.

**Interviewer Q&A — Scaling:**

Q1: How do you scale ingest to handle 100 k simultaneous stream starts (e.g., during a platform-wide event)?
A1: The ingest layer uses Anycast routing, so new streams connect to the nearest RTMP edge pod. RTMP edge pods are stateless connection handlers — they accept the TCP connection and relay the byte stream. Scaling is purely horizontal: add pods. We pre-scale 2× during known high-traffic events (e.g., tournament starts). The bottleneck shifts to the Stream Router which assigns transcoder pods; it reads from a pool maintained in etcd and is itself horizontally scaled with leader election for the assignment logic.

Q2: What happens to VOD storage costs at the estimated 69 PB/year?
A2: S3 standard at $0.023/GB = $0.023 × 69 × 10^6 GB = ~$1.6B/year — obviously not financially viable for all streams. Real mitigation: (1) Only store VODs for streams where the streamer opts in or has sufficient viewership; (2) Tiered storage: move VODs to S3 Glacier after 30 days ($0.004/GB) — reduces cost by ~6×; (3) Compress older VODs with AV1 re-encoding (50% size reduction vs H.264); (4) Delete VODs after 90 days unless specifically saved; (5) In practice, Twitch/YouTube have petabytes of storage and amortize costs with custom hardware at massive discount.

Q3: How do you handle the birthday problem with stream_id UUIDs as S3 prefixes?
A3: UUID v4 has 2^122 possible values, making collisions negligible. The concern with S3 is not collision but hot-partition at the prefix level: if all segments for a stream share the prefix `live/{stream_id}/`, and a mega-stream generates 1,800 segments/hour (4 renditions × 1 seg/2s × 3600), that's 1,800 PUT requests/hour = 0.5 PUT/s, well below S3's 3,500 PUT/s per prefix limit. No prefix-level sharding is needed per stream.

Q4: How do you auto-scale the transcoder cluster without over-provisioning?
A4: Transcoder pods run on a Kubernetes cluster with a custom HPA metric: `active_streams_per_gpu`. Target: 40 streams/GPU (leaving 20% headroom). When a new stream starts, the Stream Router checks capacity; if all pods are at 40+ streams/GPU, it signals the cluster autoscaler to provision a new node. New GPU nodes take ~2–3 minutes to provision (pre-warmed AMIs reduce this to ~90 s). The Stream Router queues new stream assignments for up to 30 s before returning an error, covering the autoscale warmup.

Q5: How do you handle the thundering herd on the Playlist Service when 100 k streams each need a manifest update every 2 seconds?
A5: 100 k updates / 2 s = 50 k updates/s. Each update is: (1) Redis ZRANGE (single command, ~1 ms), (2) string concatenation for m3u8 (< 1 ms), (3) S3 PUT (~5–10 ms, async). The Playlist Service is horizontally scaled: with 50 k updates/s and each update taking ~10 ms, we need at least 500 concurrent workers. 50 pods with 10 goroutines each = 500 workers = 500 × (1/10 ms) = 50 k updates/s. S3 handles 3,500 PUT/s per prefix, but since each stream has its own prefix, 100 k concurrent streams = 100 k distinct prefixes, all within S3 limits.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| RTMP Ingest pod crash | Streamer connection dropped | TCP connection reset to streamer | Streamer auto-reconnects (OBS has built-in reconnect with 10 s backoff); new pod picks up on reconnect | < 30 s |
| Transcoder pod crash | Stream drops for all viewers | Heartbeat miss in etcd (5 s TTL) | Stream Router detects, re-assigns to healthy pod; stream restarts from reconnect | < 15 s |
| Playlist Service pod crash | Manifest staleness (viewers buffer) | Kubernetes liveness probe | k8s restarts pod; another Playlist Service instance picks up stream | < 10 s |
| S3 regional outage | No new segments available; DVR unavailable | Health check on S3 endpoint | Failover to replica region (S3 CRR); CDN serves cached segments (30–60 s gap) | < 60 s |
| CDN PoP outage | Regional viewers rebuffer | Synthetic monitoring from each region | CDN Anycast routes around failed PoP; DNS failover | < 120 s |
| Redis Cluster node failure | Chat delivery gap (~5–10 s); viewer count stale | Redis Cluster gossip; Sentinel | Automatic replica promotion; gateway pods reconnect to new primary | < 10 s |
| Cassandra node failure | Chat history reads may fail | Nodetool; health endpoint | RF=3 + LOCAL_QUORUM: 2 of 3 nodes must ack; one node failure is transparent | 0 (zero impact with RF=3) |
| PostgreSQL primary failure | Stream metadata writes fail | PG streaming replication monitor | Patroni promotes standby in < 30 s; application reconnects | < 30 s |
| WebSocket gateway overload | Chat connection failures | CPU/memory/connection count alerts | Auto-scale gateway pods; shed load gracefully (reject new connections with 503 during overload) | < 2 min (autoscale) |

### Retries & Idempotency
- **Segment upload to S3**: FFmpeg segment hook retries with exponential backoff (3 retries, 1 s / 2 s / 4 s). S3 PUTs are idempotent (same key = same object overwritten safely).
- **Chat message persistence to Cassandra**: Kafka consumer at-least-once delivery; Cassandra INSERT is idempotent with `message_id` as clustering key (same TIMEUUID = same row, safe to re-insert).
- **RTMP reconnect**: OBS and most streaming software have built-in reconnect logic (configurable; recommend 3 retries with 5 s intervals). Ingest pod handles duplicate stream_key reconnects: detects if stream is already active in etcd, tears down old transcoder before starting new one (avoids duplicate transcoding).

### Circuit Breaker
- **Transcoder → S3 upload**: circuit breaker (Hystrix-style, implemented in Go `gobreaker` library) trips if S3 error rate > 10% over 30 s. While tripped: segments queued in-memory (max 30 s of segments). On recovery, flush queue to S3. This prevents transcoder pod from busy-looping on S3 failures.
- **Chat service → Cassandra**: circuit breaker trips on Cassandra timeout rate > 5%. While tripped: chat messages are still published via Redis (real-time delivery unaffected) but persistence is dropped with a warning log. Acceptable tradeoff: real-time delivery > persistence.
- **Playlist Service → Redis**: if Redis is unreachable, Playlist Service falls back to reading segment list directly from S3 bucket listing (slower but functional). This degrades performance but keeps streams alive.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `ingest_active_streams` | Gauge | — | Capacity planning |
| `ingest_rtmp_connect_latency_ms` | Histogram | P99 > 500 ms | Ingest health |
| `transcode_lag_seconds` (ingest_ts - encode_ts) | Gauge | > 5 s per stream | Transcoding falling behind |
| `segment_upload_duration_ms` | Histogram | P99 > 2000 ms | S3 performance |
| `segment_upload_error_rate` | Counter | > 1% | S3 health |
| `manifest_staleness_ms` (now - last_segment_ts) | Gauge per stream | > 4000 ms | Playlist Service lag |
| `cdn_cache_hit_rate` | Gauge per region | < 95% | CDN efficiency |
| `cdn_origin_rps` | Counter | Spike > 2× baseline | Origin overload |
| `viewer_count_total` | Gauge | — | Business metric |
| `chat_messages_per_second` | Counter | — | Chat load |
| `chat_delivery_latency_ms` | Histogram | P99 > 200 ms | Chat SLA |
| `ws_connections_per_pod` | Gauge | > 90 k | Gateway scaling trigger |
| `redis_pub_sub_latency_ms` | Histogram | P99 > 100 ms | Redis health |
| `stream_start_success_rate` | Counter | < 99% | Overall pipeline health |
| `stream_error_rate` (by type) | Counter | > 0.1% | Error classification |

### Distributed Tracing
- OpenTelemetry SDK in all services; traces exported to Jaeger/Tempo.
- Critical trace: `stream_start` span covers RTMP authorize → transcoder assignment → first segment upload → first manifest publish → CDN cache warm. This trace spans 4 services and is the most important for latency diagnosis.
- Trace sampling: 100% for error paths; 1% for success paths (too expensive to trace all 50 M viewer segment requests — use sampling with reservoir sampling for long traces).
- Segment upload traces sampled at 10% to catch S3 latency regressions.

### Logging
- Structured JSON logs (Pino / Zap) sent to Kafka log topic → Elasticsearch for search.
- Log levels: ERROR (always), WARN (rate-limited), INFO (stream start/end), DEBUG (disabled in production).
- Ingest: log `stream_key` (hashed), `ingest_pod`, `user_agent`, `remote_ip` on every stream connect.
- Transcoder: log segment upload latency, keyframe interval, actual vs. target bitrate for each rendition.
- Chat: log message_id, stream_id (but NOT message body in platform logs — privacy).
- Retention: 7 days hot (Elasticsearch), 90 days cold (S3 + Athena queryable).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A (Chosen) | Option B | Why A |
|---|---|---|---|
| Ingest protocol | RTMP (standard) | WebRTC ingest | RTMP universally supported by streaming software; WebRTC ingest available as optional overlay for lower latency |
| Delivery protocol | HLS (standard, 5-8s latency) | RTSP / raw UDP | HLS works over standard CDN HTTP; CDN-native caching; ABR support native to HLS |
| Ultra-low latency | WebRTC re-streaming (< 2s) | RTMP-based low-latency HLS | WebRTC achieves sub-second; LHLS only reaches ~2-3s |
| Transcoder architecture | GPU per-stream pod | Shared GPU pool (all streams on one machine) | Pod isolation prevents one bad stream from degrading others; simpler failure recovery |
| Segment duration | 2 seconds | 6 seconds (traditional HLS) | 2s reduces glass-to-glass latency from ~18s to ~6s; segment request rate is manageable with HTTP/2 |
| Chat persistence | Cassandra | PostgreSQL | Cassandra's write throughput and TTL support are purpose-built for time-series chat data |
| Viewer count | HyperLogLog (Redis) | Exact set (Redis SET) | HLL uses constant 12 KB/stream regardless of viewer count; < 1% error acceptable for a display counter |
| Chat delivery | Redis pub/sub | Direct WebSocket broadcast from single server | Redis decouples gateway pods; horizontal scale; < 100 ms delivery maintained |
| DVR storage | Object store (S3) | Dedicated media server (Wowza) | S3 infinite scale; CDN-native; no media server license cost |
| VOD creation | Post-stream concat from segments | Real-time parallel recording | Segment-based VOD creation is simple: just change the manifest to list all segments; no additional encoding |

---

## 11. Follow-up Interview Questions

Q1: How would you implement a clip feature that lets viewers capture a 30-second clip of a live stream?
A1: During a live stream, the last 90 seconds of segments are always available in the DVR buffer on S3. When a viewer requests a clip: (1) Calculate which segments cover the requested time window (e.g., last 30 s = last 15 segments at 2 s each); (2) Enqueue an async job (Kafka message) to the Clip Service; (3) Clip Service uses FFmpeg to concat those 15 segments into a single MP4 file, trim to exact boundaries using `-ss` and `-t` flags; (4) Upload to S3 `clips/{clip_id}.mp4`; (5) Update the clips table status to `ready` and return the CDN URL. This completes in < 10 s for a 30-second clip.

Q2: How do you implement stream key rotation if a key is compromised?
A2: The stream key is stored as an Argon2 hash in PostgreSQL (never plaintext). The ingest authorization endpoint accepts the raw key and computes the hash for lookup. To rotate: (1) Generate a new key; (2) Update the hash in the DB; (3) If there's an active stream, the old key continues working until the stream ends (existing connection is already authorized); (4) New connections with the old key are rejected immediately. We add a 5-minute overlap window for mid-stream key rotation to avoid interrupting live broadcasts.

Q3: What changes would you need to make to support 8K streams?
A3: 8K (7680×4320) at 30fps requires ~80–120 Mbps bitrate. Key changes: (1) Ingest bandwidth: 100 k × 120 Mbps = 12 Tbps ingest capacity needed (vs. 600 Gbps today — 20× increase); (2) Transcoder: 8K NVENC encoding requires H.265/HEVC or AV1 for efficient delivery; AV1 encoding on NVENC Ada Lovelace handles ~10 streams/GPU (vs. 50 for 1080p); (3) CDN egress: 150 Tbps delivery × 4 = 600 Tbps — only achievable with aggressive AV1 compression or by limiting 8K to premium CDN tiers; (4) Storage: 4× current VOD storage; (5) Realistically, 8K streaming is limited to a small fraction of streams with upgraded infrastructure for those specific channels.

Q4: How would you add a "highlights" feature that automatically detects exciting moments in a stream?
A4: A real-time audio analysis pipeline: (1) Transcoder pods emit audio loudness samples (dBFS) to a Kafka topic every 1 second alongside segment production; (2) A Highlights Detection Service consumes this and maintains a rolling 60-second average; (3) When the current loudness exceeds the 60-second average by > 6 dB (crowd cheer detection) sustained for > 3 seconds, it marks the current stream timestamp as a "highlight candidate"; (4) After stream ends, a batch ML pipeline (video captioning + audio classification) refines the candidates; (5) The streamer's dashboard shows detected highlights for easy clip creation.

Q5: How would you design the ingest system to be resilient to a streamer's home internet going down for 5 seconds?
A5: RTMP has no built-in reconnect; the TCP connection drops. The solution: (1) The ingest edge holds the RTMP session alive for 10 s after TCP disconnect (buffer the last 5 s of segments in memory); (2) When the streamer reconnects (OBS retries within 2–5 s), the ingest edge detects the same stream_key, resumes the session, and feeds the buffered segments plus new data to the transcoder; (3) The transcoder continues encoding without a gap (or with a < 5 s gap if buffers are exhausted); (4) The HLS player, seeing no new segments for 4–6 s, enters a re-buffering state and resumes when new segments arrive; viewers see a brief pause but the stream does not end.

Q6: What protocol would you use for ultra-low-latency mode and how does it work?
A6: WebRTC with SFU (Selective Forwarding Unit) architecture. Streamer ingest: browser-based streamers use the WebRTC API directly; app-based streamers use an RTMP-to-WebRTC gateway (converts RTMP to RTP). The SFU (implemented with Pion in Go or mediasoup in Node.js) receives one WebRTC stream from the streamer and forwards it to many viewer WebRTC PeerConnections without transcoding (just packet forwarding). This achieves < 500 ms glass-to-glass latency. At 1 M viewers, WebRTC SFUs must be cascaded (hierarchical tree of SFUs) since a single SFU handles ~5 k–10 k viewers. The trade-off: WebRTC does not support DVR, ABR, or CDN caching — it's a separate delivery path for interactive use cases only.

Q7: How do you handle streams that go viral unexpectedly (e.g., a stream that goes from 100 to 500 k viewers in 60 seconds)?
A7: The critical path is CDN warm-up. With 500 k viewers requesting the manifest from potentially many CDN PoPs simultaneously: (1) The viewer count monitoring (updated every 5 s via HyperLogLog) detects the surge; (2) At 10 k viewers, the system triggers the pre-warming path — Playlist Service immediately pushes current and next segments to all CDN PoPs proactively; (3) CDN request coalescing handles the initial burst (all simultaneous misses for the same manifest collapse into one origin request per PoP); (4) Redis pub/sub for chat automatically scales (adding gateway pods via autoscaler); (5) The transcoder pod for that stream is not affected — it processes one input stream regardless of viewer count.

Q8: How do you ensure the stream_key is secure?
A8: (1) Generated server-side as 32 bytes of cryptographically secure random (crypto/rand in Go), base64url-encoded = 43 characters; (2) Stored as Argon2id hash in PostgreSQL (never stored plaintext); (3) Transmitted only over TLS 1.3; (4) Ingest endpoint uses RTMPS (RTMP over TLS); (5) Rate-limited authorization endpoint (10 req/min per IP) prevents brute force; (6) Stream key is never logged (log scrubbing rules strip the key from RTMP URL logs); (7) Dashboard shows the key only on explicit click (obscured by default).

Q9: How do you implement content delivery for viewers behind restrictive firewalls (e.g., corporate networks that block port 443 for media)?
A9: HLS over HTTPS (port 443) works through most corporate firewalls since it's standard HTTPS. If port 443 is blocked (unusual), HLS can fall back to HTTP port 80 (with downgraded security, not recommended). WebRTC uses STUN/TURN and can traverse NAT via TURN relay on port 443 (TURN over TCP/443 is often allowed). For truly restrictive environments, we expose HLS on port 80 as a fallback with HTTP Strict Transport Security (HSTS) preloading not applied to CDN subdomains.

Q10: How would the architecture change if you needed to support 360° / VR streams?
A10: 360° video requires equirectangular projection delivery. Key changes: (1) Ingest and transcoding are the same pipeline (RTMP → FFmpeg) but with metadata in the m3u8 indicating equirectangular projection (EXT-X-PROJECTION); (2) Bitrate requirements are 3–5× higher due to no visible-area cropping (viewer sees 1/6th of the frame but we deliver the whole sphere); (3) For efficiency, tiled streaming (like DASH SRD — Spatial Relationship Description) delivers only the viewport tiles at high quality + peripheral tiles at low quality, reducing bandwidth by 60%; (4) Tile viewport prediction based on WebXR head tracking requires WebSocket back-channel from player to CDN edge — a fundamentally different delivery architecture requiring stateful edge compute (Cloudflare Workers / Fastly Compute@Edge).

Q11: How does the chat system handle message ordering for viewers in different regions?
A11: Messages are ordered by TIMEUUID (time-based UUID v1) which encodes a timestamp with 100-nanosecond precision. Clients sort received messages by TIMEUUID for display ordering. Since clocks across servers may drift (NTP accuracy ~1–10 ms), messages from different regions arriving within the same millisecond may appear in arbitrary order — this is acceptable for chat. For critical ordering (e.g., moderator ban before a message shows up), we use a logical clock: a Redis INCR counter per stream as a sequence number. Messages carry both a sequence number and timestamp.

Q12: How would you handle a DMCA takedown of content during a live stream?
A12: Stream key revocation: (1) Platform admin calls internal API `DELETE /admin/streams/{stream_id}/active`; (2) Ingest service closes the RTMP connection, which terminates the stream; (3) Playlist Service stops generating new manifests; (4) CDN receives cache purge for all manifest URLs (near-instant via Fastly Instant Purge API) — new segment requests return 404; (5) HLS players detect 404 on manifest and stop playback; (6) DVR segments remain in S3 but manifests are deleted, making them inaccessible via CDN (though segments are technically still in S3 — a separate retention policy job cleans them up). Total takedown time: < 5 seconds from API call to viewers seeing stream end.

Q13: How do you measure glass-to-glass latency end-to-end?
A13: Embed a visible timecode (milliseconds) in the video stream at the encoder (FFmpeg `drawtext` filter showing `%{pts}` with millisecond precision). A monitoring system uses a camera pointed at a test stream's playback screen and captures frames; OCR reads the encoded timecode and compares it to wall clock. The difference is glass-to-glass latency. This is done from 5 geographic locations to measure P50/P99 by region. Automated via a synthetic monitoring loop running every 5 minutes on all platforms (iOS, Android, Web).

Q14: What is your strategy for handling a catastrophic data center failure?
A14: Active-passive multi-region setup: (1) Primary region (us-east-1) handles all ingest and transcoding; (2) S3 CRR continuously replicates all segments to eu-west-1; (3) PostgreSQL has a physical standby in eu-west-1 with streaming replication (RPO < 5 s); (4) Cassandra runs as a multi-DC cluster — eu-west-1 DC is a full replica (RF=3 per DC); (5) On us-east-1 failure, Route53 health checks (30 s TTL) redirect DNS to eu-west-1 ingest endpoints; (6) Active streams must reconnect (streamers see a disconnect and reconnect within 30 s); (7) Viewers may experience 60–90 s interruption during DNS propagation. RTO: ~2 minutes, RPO: ~30 s of stream data.

Q15: What's the most significant architectural decision you'd revisit at 10× scale?
A15: The single-region transcoding cluster. At 1 M concurrent streams (10×), running all transcoding in one AWS region creates a single point of geographic failure and raises latency for streamers in Europe and Asia (RTMP ingest RTT from Tokyo to us-east-1 is ~150 ms, measurably impacting stream stability). The solution: regional transcoding clusters with region-aware ingest routing (Anycast per-region). Streamer in Tokyo ingests to Tokyo ingest cluster → Tokyo transcoder → Tokyo S3 bucket → replicated to us-east-1 for CDN origin. This reduces ingest RTT to < 10 ms and provides geographic fault isolation. The coordination complexity (stream routing, health checks, Playlist Service per region) increases significantly.

---

## 12. References & Further Reading

1. Apple HLS Specification (RFC 8216): https://datatracker.ietf.org/doc/html/rfc8216
2. MPEG-DASH Standard ISO/IEC 23009-1: https://www.iso.org/standard/79329.html
3. Low-Latency HLS (LHLS) Apple Developer Documentation: https://developer.apple.com/documentation/http-live-streaming/enabling-low-latency-hls
4. WebRTC RFC 8825 (Overview): https://datatracker.ietf.org/doc/html/rfc8825
5. Twitch Engineering Blog — Transcoding at Twitch: https://blog.twitch.tv/en/2017/10/10/live-video-transmuxing-transcoding-f-mpeg-vs-twitch-transcoding-4-vs-12-bffd9e574b5b/
6. Facebook Live Streaming Architecture: https://engineering.fb.com/2015/09/17/video-engineering/under-the-hood-broadcasting-live-video-to-millions/
7. Redis HyperLogLog Documentation: https://redis.io/docs/data-types/probabilistic/hyperloglogs/
8. Cassandra Data Modeling — Time Series: https://cassandra.apache.org/doc/latest/cassandra/data_modeling/data_modeling_rdbms.html
9. FFmpeg NVENC Hardware Encoding Guide: https://trac.ffmpeg.org/wiki/HWAccelIntro
10. Akamai CDN Streaming Best Practices: https://techdocs.akamai.com/adaptive-media-delivery/docs
11. SRT (Secure Reliable Transport) Protocol Specification: https://www.haivision.com/resources/white-paper/srt-protocol/
12. Designing Data-Intensive Applications — Martin Kleppmann (O'Reilly, 2017) — Chapters on replication and stream processing
13. "High Performance Browser Networking" — Ilya Grigorik, Chapter 18 (WebRTC): https://hpbn.co/webrtc/
14. AWS Architecture Blog — Video on Demand on AWS: https://aws.amazon.com/solutions/implementations/video-on-demand-on-aws/
15. Pion WebRTC (Go library used in production SFUs): https://github.com/pion/webrtc
