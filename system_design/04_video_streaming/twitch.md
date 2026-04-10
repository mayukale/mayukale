# System Design: Twitch

---

## 1. Requirement Clarifications

### Functional Requirements

1. Streamers can broadcast live video at up to 1080p60 fps (up to 8,000 Kbps bitrate) using RTMP/OBS.
2. Viewers can watch live streams with low latency (target: < 5 seconds behind live).
3. The system supports live chat (IRC-based) with thousands of concurrent chatters per channel.
4. Live streams are transcoded into multiple quality levels (1080p60, 720p60, 720p30, 480p, 360p, 160p).
5. Streams are recorded and stored as VODs (Video on Demand) after the stream ends.
6. Viewers can create and share Clips (30-second highlights from live or VOD).
7. Streamers can monetize via subscriptions, Bits (virtual currency for cheers), and ad revenue.
8. A directory/discovery page shows live streams sorted by viewer count per category/game.
9. Viewers receive stream-live notifications for followed channels.
10. Channel Points and prediction/poll features (interactive audience engagement).

### Non-Functional Requirements

1. Availability: 99.95% uptime for stream ingest and playback (≤ 4.4 hours downtime/year).
2. Latency:
   - Default (HLS): < 5-8 seconds behind live.
   - Low-Latency HLS (LHLS): < 3-4 seconds.
   - WebRTC (Ultra-Low Latency): < 1 second (used for special modes).
3. Scalability: Support 50,000 concurrent live streams; 2+ million concurrent viewers.
4. Chat scalability: Handle channels with 100,000+ simultaneous chatters; 1 million+ chat messages/minute at peak.
5. Throughput: Ingest 50,000 simultaneous RTMP streams.
6. Durability: VODs stored for 60 days for all users; unlimited for Partners.
7. Consistency: View count in directory: eventually consistent (updated every ~10 seconds). Chat messages: ordered within a channel; no global ordering guarantee.

### Out of Scope

- Twitch Studio (creator desktop app internals).
- Drops and rewards system (game-specific promotional items).
- Twitch Extensions (overlays, panels — third-party integrations).
- Hype Train and other channel engagement features.
- Affiliate/Partner program eligibility and payment processing.
- Mobile streaming (different ingest path).

---

## 2. Users & Scale

### User Types

| User Type      | Description                                      | Primary Actions                                         |
|----------------|--------------------------------------------------|---------------------------------------------------------|
| Streamer        | Broadcaster running a live stream                | Ingest RTMP, manage channel, interact with chat        |
| Viewer          | Consumer watching a live stream or VOD           | Watch, Chat, Sub, Clip, Bits                           |
| Moderator       | Appointed by streamer to manage chat             | Ban users, delete messages, set slow/subscriber mode   |
| Partner         | Professional streamer with revenue split        | Access to advanced analytics, custom emotes, VOD perma |
| Affiliate       | Semi-professional, basic monetization           | Subscriptions, Bits                                    |

### Traffic Estimates

**Assumptions (based on Twitch public data, Twitch Advertising reports, and industry analysis):**
- 2.5 million concurrent viewers at average times; peak: ~7-8 million.
- 50,000 concurrent live streams at peak.
- Average viewer session: 95 minutes/day (Twitch reported this in 2021 investor materials).
- Average stream duration: 4 hours.
- Chat: top 100 channels average 2,000 messages/minute; average channel: 50 messages/minute.
- 5 million new Clips created per day.
- VOD: every stream auto-recorded.

| Metric                             | Calculation                                                         | Result               |
|------------------------------------|---------------------------------------------------------------------|----------------------|
| Peak concurrent viewers            | Empirical data (Twitch public)                                      | ~7 million           |
| Average concurrent viewers         | Based on session time / day distribution                            | ~2.5 million         |
| Peak concurrent streams            | Empirical (streaming events, major games)                           | ~50,000              |
| RTMP ingest bandwidth (peak)       | 50,000 streams × 4 Mbps avg source bitrate                         | ~200 Gbps ingest     |
| Stream egress (peak)               | 7M viewers × 3.5 Mbps avg (mix of quality levels)                  | ~24.5 Tbps           |
| Chat messages/sec (peak)           | 100 channels × 2,000/min + 49,900 channels × 50/min → (200,000 + 2,495,000)/60 | ~45,000 chat msg/sec |
| Clip creation events/sec           | 5M clips/day / 86,400                                               | ~58 clips/sec        |
| VOD storage per day                | 50,000 streams × 4 hrs × 3600s × (4 Mbps avg) / 8 bits/byte × rendition factor (8 renditions at varying quality avg ~0.8× original) | ~360 TB/day          |

### Latency Requirements

| Operation                      | Target p50    | Target p99    | Notes                                       |
|--------------------------------|---------------|---------------|---------------------------------------------|
| Live stream latency (HLS)      | 4 seconds     | 8 seconds     | Glass-to-glass from encoder to viewer       |
| Live stream latency (LHLS)     | 2 seconds     | 4 seconds     | Low-Latency HLS with partial segments       |
| Chat message delivery          | < 100 ms      | < 500 ms      | To all viewers in same channel              |
| Directory load                 | < 200 ms      | < 500 ms      | Top streams per category                    |
| Clip creation                  | < 30 seconds  | < 120 seconds | Transcoding of 30-second clip               |
| VOD start (TTFF)               | < 1,000 ms    | < 3,000 ms    | On par with YouTube VOD performance         |
| Follow notification            | < 30 seconds  | < 120 seconds | Stream-live push notification               |

### Storage Estimates

| Category                     | Calculation                                                          | Result           |
|------------------------------|----------------------------------------------------------------------|------------------|
| VOD storage per day          | 50,000 streams × 4 hr avg × 1.5 GB/hr avg (compressed HLS)         | ~300 TB/day      |
| VOD retention (60 days)      | 300 TB × 60                                                          | ~18 PB active    |
| Clip storage (30s × 8 qual)  | 58 clips/sec × 86,400 × 30s × 3 Mbps / 8 bits/byte × 8 quality levels | ~720 TB/day   |
| Clip retention (unlimited)   | 720 TB/day × 365 days                                                | ~263 PB/year     |
| Chat logs (30-day retention) | 45,000 msg/sec × 86,400 × 200 bytes/msg × 30 days                   | ~22 TB           |
| Stream metadata              | 50,000 streams × 24 hr × 200 bytes/heartbeat × 1/sec               | ~86 GB/day       |

### Bandwidth Estimates

| Direction                  | Calculation                                           | Result          |
|----------------------------|-------------------------------------------------------|-----------------|
| RTMP ingest (peak)         | 50,000 streams × 4 Mbps avg source                   | 200 Gbps        |
| HLS egress (peak)          | 7M viewers × 3.5 Mbps avg                            | 24.5 Tbps       |
| Clip/VOD egress            | ~200,000 concurrent VOD viewers × 2.5 Mbps avg       | ~500 Gbps       |
| Total peak egress          | 24.5 Tbps + 0.5 Tbps                                 | ~25 Tbps        |

---

## 3. High-Level Architecture

```
STREAMER SIDE:
                    ┌──────────────────────────────────────────────────────┐
                    │  OBS / Streamlabs / SLOBS / Mobile App               │
                    │  Encodes: H.264 or H.265, AAC audio                  │
                    │  Sends: RTMP stream (rtmp://live.twitch.tv/app/{key})│
                    └───────────────────────┬──────────────────────────────┘
                                            │ RTMP (TCP, port 1935)
INGEST LAYER:       ┌───────────────────────▼──────────────────────────────┐
                    │        INGEST EDGE SERVERS (150+ PoPs globally)       │
                    │   - Anycast routing: nearest ingest PoP              │
                    │   - RTMP server (ffserver / custom RTMP)              │
                    │   - Validate stream key, authenticate                 │
                    │   - Buffer 2 seconds of incoming video               │
                    └───────────────────────┬──────────────────────────────┘
                                            │ Internal relay (TCP/custom)
TRANSCODING:        ┌───────────────────────▼──────────────────────────────┐
                    │         TRANSCODING CLUSTER (K8s + GPU farm)         │
                    │   - FFmpeg with NVENC: 1080p60, 720p60, 720p30,      │
                    │     480p30, 360p30, 160p30                           │
                    │   - Generates 2-second HLS segments per quality      │
                    │   - Writes segments to Object Store (S3-compatible)  │
                    └──────────────────┬────────────────────────────────────┘
                                       │
MANIFEST:           ┌──────────────────▼────────────────────────────────────┐
                    │      MANIFEST SERVICE                                  │
                    │   - Maintains rolling HLS playlist (last N segments)  │
                    │   - Exposes: master.m3u8 + per-quality playlists     │
                    │   - Served from origin; cached at CDN edge           │
                    └──────────────────┬─────────────────────────────────────┘
                                       │
CDN:                ┌──────────────────▼─────────────────────────────────────┐
                    │       CDN EDGE (Akamai + Fastly + custom PoPs)         │
                    │   - Caches HLS segments (2-sec TTL)                   │
                    │   - Serves viewers globally                           │
                    └──────────────────┬─────────────────────────────────────┘
                                       │
VIEWER SIDE:        ┌──────────────────▼─────────────────────────────────────┐
                    │           WEB/APP VIEWER PLAYER                        │
                    │   - Fetches master.m3u8                               │
                    │   - ABR selection                                     │
                    │   - Polls variant playlist every 2 seconds           │
                    └────────────────────────────────────────────────────────┘

CHAT SYSTEM (separate path):
   Streamer/Viewer Client
         │
         │ IRC over WebSocket (ws://irc-ws.chat.twitch.tv:443)
         ▼
   CHAT INGRESS SERVERS (IRCd-based)
         │
         │ Fan-out via message bus
         ▼
   CHAT DISTRIBUTION SERVERS
   (one server cluster per channel; distribute to all viewers in channel)
         │
         │ WebSocket push
         ▼
   Viewer clients

API SERVICES (separate path from video):
   ┌──────────────────────────────────────────────────────────────────┐
   │  Twitch API (Helix API v2)                                       │
   │  ┌──────────────┐ ┌──────────────┐ ┌─────────────────────────┐  │
   │  │ Stream Meta  │ │ Directory Svc│ │ User/Channel Service    │  │
   │  │ Service      │ │ (Redis ZSET) │ │ (MySQL + Redis)         │  │
   │  └──────────────┘ └──────────────┘ └─────────────────────────┘  │
   │  ┌──────────────┐ ┌──────────────┐ ┌─────────────────────────┐  │
   │  │ Clip Service │ │ VOD Service  │ │ Notification Service    │  │
   │  └──────────────┘ └──────────────┘ └─────────────────────────┘  │
   └──────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

- **Ingest Edge Servers**: ~150 global PoPs receive RTMP streams from streamers. Authenticate the stream key, buffer the first 2 seconds (to absorb network jitter), and relay to the transcoding cluster in the nearest data center. Use Anycast IP routing so the streamer's client automatically connects to the nearest PoP.
- **Transcoding Cluster**: Stateful workers assigned one stream each. FFmpeg reads RTMP input (or forwarded stream) and outputs multiple HLS quality levels simultaneously. GPU-accelerated (NVENC) for real-time transcoding at scale. Each worker handles one stream; scaling is linear.
- **Manifest Service**: Maintains a sliding window HLS playlist per stream × per quality. Appends new 2-second segments as they arrive from transcoding. Deletes segments older than 90 seconds (rolling window; last 45 segments retained). Playlist updates trigger CDN edge to fetch new segment.
- **CDN Edge**: Akamai CDN (Twitch's historically primary CDN) plus Fastly for burst capacity. Live segment cache TTL = 2 seconds (segment duration). Master and variant manifests have 2-second TTL. Viewers poll manifest every 2 seconds.
- **Chat System (IRC-based)**: Twitch chat is an IRC protocol implementation over WebSocket. Each channel has a dedicated "chat room" in a distributed pub/sub cluster. Messages arrive at IRCd-compatible ingress servers → published to a message bus per channel → pushed to all subscriber WebSocket connections.
- **Directory Service**: Redis sorted set (`ZADD streams:{category} {viewer_count} {channel_id}`) updated every 10 seconds. Powers "Browse" page with real-time viewer counts.
- **VOD Service**: When a stream ends, the recorded HLS segments (stored in S3) are assembled into a permanent VOD manifest. Processed for ad insertion markers and chapter detection.
- **Clip Service**: Accepts a clip request (channel_id, clip_start_timestamp, duration_seconds). Fetches source HLS segments from S3, runs FFmpeg to extract and transcode the clip, uploads to S3, and generates a permanent CDN URL.

---

## 4. Data Model

### Entities & Schema (Full SQL)

```sql
-- ─────────────────────────────────────────────
-- USERS & CHANNELS
-- ─────────────────────────────────────────────
CREATE TABLE users (
    user_id             BIGINT          PRIMARY KEY,        -- Twitch uses integer IDs
    login               VARCHAR(25)     NOT NULL UNIQUE,    -- lowercase username
    display_name        VARCHAR(25)     NOT NULL,
    email               VARCHAR(255)    NOT NULL UNIQUE,
    hashed_password     VARCHAR(255)    NOT NULL,
    user_type           VARCHAR(10)     NOT NULL DEFAULT 'user'
                                        CHECK (user_type IN ('user','affiliate','partner','staff')),
    broadcaster_type    VARCHAR(10)     NOT NULL DEFAULT ''
                                        CHECK (broadcaster_type IN ('','affiliate','partner')),
    description         TEXT,
    profile_image_url   TEXT,
    offline_image_url   TEXT,
    view_count          BIGINT          NOT NULL DEFAULT 0,  -- total all-time channel views
    follower_count      BIGINT          NOT NULL DEFAULT 0,
    subscriber_count    INTEGER         NOT NULL DEFAULT 0,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    status              VARCHAR(10)     NOT NULL DEFAULT 'active'
);

CREATE INDEX idx_users_login ON users(login);

-- ─────────────────────────────────────────────
-- STREAMS (live and ended)
-- ─────────────────────────────────────────────
CREATE TABLE streams (
    stream_id           BIGINT          PRIMARY KEY,        -- globally unique, assigned at stream start
    user_id             BIGINT          NOT NULL REFERENCES users(user_id),
    stream_key_hash     VARCHAR(64)     NOT NULL,           -- HMAC of the stream key
    title               VARCHAR(140),
    game_id             BIGINT          REFERENCES games(game_id),
    language            CHAR(5),
    is_mature           BOOLEAN         NOT NULL DEFAULT FALSE,
    tags                TEXT[],                             -- PostgreSQL array; max 5 tags
    started_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    ended_at            TIMESTAMPTZ,
    status              VARCHAR(10)     NOT NULL DEFAULT 'live'
                                        CHECK (status IN ('live','ended','error')),
    peak_viewer_count   INTEGER         NOT NULL DEFAULT 0,
    concurrent_viewers  INTEGER         NOT NULL DEFAULT 0, -- current; updated every 10s
    total_view_minutes  BIGINT          NOT NULL DEFAULT 0,
    ingest_server       VARCHAR(100),                       -- PoP that received the stream
    ingest_bitrate_kbps INTEGER,
    PRIMARY KEY (stream_id)
);

CREATE INDEX idx_streams_user      ON streams(user_id, started_at DESC);
CREATE INDEX idx_streams_live      ON streams(status, started_at DESC) WHERE status = 'live';
CREATE INDEX idx_streams_game_live ON streams(game_id, concurrent_viewers DESC) WHERE status = 'live';

-- ─────────────────────────────────────────────
-- GAMES / CATEGORIES
-- ─────────────────────────────────────────────
CREATE TABLE games (
    game_id             BIGINT          PRIMARY KEY,
    name                VARCHAR(200)    NOT NULL UNIQUE,
    box_art_url         TEXT,
    igdb_id             BIGINT,                             -- IGDB.com reference
    total_live_viewers  INTEGER         NOT NULL DEFAULT 0  -- denormalized, updated every 10s
);

-- ─────────────────────────────────────────────
-- FOLLOWS
-- ─────────────────────────────────────────────
CREATE TABLE follows (
    follower_id         BIGINT          NOT NULL REFERENCES users(user_id),
    followed_id         BIGINT          NOT NULL REFERENCES users(user_id),
    followed_at         TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    notifications_on    BOOLEAN         NOT NULL DEFAULT TRUE,
    PRIMARY KEY (follower_id, followed_id)
);

CREATE INDEX idx_follows_channel ON follows(followed_id, followed_at DESC);

-- ─────────────────────────────────────────────
-- SUBSCRIPTIONS (paid)
-- ─────────────────────────────────────────────
CREATE TABLE subscriptions (
    subscription_id     UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    subscriber_id       BIGINT          NOT NULL REFERENCES users(user_id),
    broadcaster_id      BIGINT          NOT NULL REFERENCES users(user_id),
    tier                VARCHAR(5)      NOT NULL CHECK (tier IN ('1000','2000','3000')),  -- $4.99/$9.99/$24.99
    is_gift             BOOLEAN         NOT NULL DEFAULT FALSE,
    gifter_id           BIGINT          REFERENCES users(user_id),
    started_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    renewed_at          TIMESTAMPTZ,
    expires_at          TIMESTAMPTZ,
    status              VARCHAR(10)     NOT NULL DEFAULT 'active'
                                        CHECK (status IN ('active','cancelled','expired')),
    UNIQUE (subscriber_id, broadcaster_id, status)
);

CREATE INDEX idx_subs_broadcaster ON subscriptions(broadcaster_id, status);

-- ─────────────────────────────────────────────
-- VODs
-- ─────────────────────────────────────────────
CREATE TABLE vods (
    vod_id              BIGINT          PRIMARY KEY,
    stream_id           BIGINT          NOT NULL REFERENCES streams(stream_id),
    user_id             BIGINT          NOT NULL REFERENCES users(user_id),
    title               VARCHAR(140),
    description         TEXT,
    duration_sec        INTEGER,
    vod_type            VARCHAR(10)     NOT NULL DEFAULT 'archive'
                                        CHECK (vod_type IN ('archive','highlight','upload')),
    view_count          BIGINT          NOT NULL DEFAULT 0,
    language            CHAR(5),
    published_at        TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    thumbnail_url       TEXT,
    manifest_key        TEXT,                               -- S3 key for VOD m3u8
    muted_segments      JSONB,                              -- [{offset_sec, duration_sec}] DMCA muted
    status              VARCHAR(10)     NOT NULL DEFAULT 'published',
    expires_at          TIMESTAMPTZ                         -- NULL for Partner/unlimited; 60 days otherwise
);

CREATE INDEX idx_vods_user ON vods(user_id, published_at DESC);

-- ─────────────────────────────────────────────
-- CLIPS
-- ─────────────────────────────────────────────
CREATE TABLE clips (
    clip_id             VARCHAR(20)     PRIMARY KEY,        -- Short random slug (e.g. 'FunkyBraveSloth...')
    broadcaster_id      BIGINT          NOT NULL REFERENCES users(user_id),
    creator_id          BIGINT          NOT NULL REFERENCES users(user_id),  -- who created the clip
    vod_id              BIGINT          REFERENCES vods(vod_id),
    stream_id           BIGINT          REFERENCES streams(stream_id),
    title               VARCHAR(100)    NOT NULL DEFAULT '',
    duration_sec        SMALLINT        NOT NULL,           -- 5-60 seconds
    vod_offset_sec      INTEGER,                            -- offset in VOD where clip starts
    view_count          BIGINT          NOT NULL DEFAULT 0,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    thumbnail_url       TEXT,
    embed_url           TEXT,
    status              VARCHAR(10)     NOT NULL DEFAULT 'published'
);

CREATE INDEX idx_clips_broadcaster ON clips(broadcaster_id, created_at DESC);
CREATE INDEX idx_clips_creator     ON clips(creator_id, created_at DESC);

-- ─────────────────────────────────────────────
-- CHAT MESSAGES (hot: Redis; cold: this table / data warehouse)
-- ─────────────────────────────────────────────
CREATE TABLE chat_messages (
    message_id          UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    channel_id          BIGINT          NOT NULL,
    sender_id           BIGINT          NOT NULL,
    sender_login        VARCHAR(25)     NOT NULL,
    body                VARCHAR(500)    NOT NULL,
    message_type        VARCHAR(10)     NOT NULL DEFAULT 'chat'
                                        CHECK (message_type IN ('chat','cheer','sub','resub','raid')),
    bits_used           INTEGER,
    sent_at             TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    color               CHAR(7),                            -- #RRGGBB user name color
    badges              TEXT[]                              -- ['broadcaster/1','subscriber/3']
) PARTITION BY RANGE (sent_at);
-- Partitioned monthly; only last 30 days in active DB; archived to data warehouse

CREATE INDEX idx_chat_channel ON chat_messages(channel_id, sent_at DESC);

-- ─────────────────────────────────────────────
-- BITS TRANSACTIONS
-- ─────────────────────────────────────────────
CREATE TABLE bits_transactions (
    transaction_id      UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    sender_id           BIGINT          NOT NULL REFERENCES users(user_id),
    broadcaster_id      BIGINT          NOT NULL REFERENCES users(user_id),
    bits_amount         INTEGER         NOT NULL CHECK (bits_amount > 0),
    usd_value           NUMERIC(10,4),                      -- $ value at time of cheer
    stream_id           BIGINT          REFERENCES streams(stream_id),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);
```

### Database Choice

| Database         | Pros                                                          | Cons                                                        |
|------------------|---------------------------------------------------------------|-------------------------------------------------------------|
| MySQL + Vitess   | Battle-tested for social data, horizontal sharding            | Sharding complexity; not ideal for time-series              |
| PostgreSQL       | Rich SQL, JSONB, array types, partitioning                    | Vertical scale limits; less sharding tooling                |
| Cassandra        | Linear write scale, multi-region, time-series friendly        | No joins; complex data modeling; eventual consistency       |
| Redis            | Sub-millisecond sorted sets; perfect for directory/counts     | In-memory cost; not primary store; persistence caveats      |
| MySQL (RDS)      | ACID for financial (subscriptions, Bits)                      | Single-region primary; needs Multi-AZ for HA               |
| ClickHouse       | Columnar OLAP; stream analytics at high write throughput      | Not an OLTP store; complex to operate                       |

**Selected Architecture:**

- **MySQL + Vitess**: Core entities — users, streams, follows, subscriptions, VODs, clips. Sharded by `user_id` for user-centric tables, by `stream_id` for streams. Vitess transparent sharding identical to YouTube's approach (Twitch was acquired by Amazon; infrastructure partially migrated to AWS).
- **Redis Cluster**: (a) Directory sorted sets: `ZADD live:{game_id} {viewer_count} {channel_id}` updated every 10s by a background aggregator. (b) Active stream metadata cache (title, game, thumbnail) for fast directory rendering. (c) Concurrent viewer counts per stream (atomic `INCR`/`DECR`).
- **Cassandra**: Chat message storage (append-heavy, time-series, high write QPS). Partition key = `channel_id`, clustering key = `sent_at DESC`. Supports "chat replay" for VODs.
- **Amazon S3 / compatible**: HLS segment storage for live streams (rolling 90s window) and VOD/clip storage. Lifecycle rules purge live segments after 2 hours (only VOD manifest needed long-term).
- **PostgreSQL (analytics)**: Stream analytics dashboard for streamers: viewer count over time, chat activity, subscription events during stream. Batch updated hourly from ClickHouse.
- **ClickHouse**: High-throughput write of view events, chat events for real-time streamer analytics. Columnar storage enables fast aggregations over billions of events.

---

## 5. API Design

All endpoints under Twitch Helix API (`https://api.twitch.tv/helix/`). OAuth 2.0 Bearer tokens. Rate limits: 800 points/min per user token (each endpoint costs 1-3 points).

### Stream Ingest (Streamer → Server)

```
RTMP Connection:
  URL: rtmp://live.twitch.tv/app/{stream_key}
  Protocol: RTMP over TCP port 1935
  Auth: stream_key in URL path
  Expected video: H.264, AAC audio, keyframe interval ≤ 2s

  On connect: Ingest server validates stream_key against users table
  On disconnect: Triggers stream end event → VOD processing

  Twitch recommends:
    Bitrate: 3,000-6,000 Kbps for 1080p30
    Keyframe: 2 seconds
    Audio: AAC-LC, 160 Kbps
```

```
PATCH /helix/channels
Authorization: Bearer {broadcaster_token}
Content-Type: application/json

Request:
{
  "broadcaster_id": "123456",
  "game_id": "21779",
  "title": "Playing Dota 2 - Road to Divine!",
  "broadcaster_language": "en",
  "tags": ["Strategy","Competitive"],
  "is_branded_content": false
}

Response 204

Rate limit: 5 req/min per broadcaster
```

### Streams (Viewer Discovery)

```
GET /helix/streams?game_id={id}&user_login={login}&user_id={id}&type=live&after={cursor}&first={n}
Authorization: Bearer {app_token}

Response 200:
{
  "data": [
    {
      "id": "987654321",
      "user_id": "123456",
      "user_login": "shroud",
      "user_name": "shroud",
      "game_id": "21779",
      "game_name": "Dota 2",
      "type": "live",
      "title": "Playing Dota 2 - Road to Divine!",
      "viewer_count": 47823,
      "started_at": "2025-04-09T18:00:00Z",
      "language": "en",
      "thumbnail_url": "https://static-cdn.jtvnw.net/previews-ttv/live_user_shroud-{width}x{height}.jpg",
      "tags": ["Strategy","Competitive"],
      "is_mature": false
    }
  ],
  "pagination": { "cursor": "eyJiIjp7..." }
}

Rate limit: 1 req/sec per client; 800 points/min window
```

### HLS Playback

```
-- Not a Helix API; this is a CDN URL pattern
GET https://usher.twitchapps.com/api/channel/hls/{channel_name}.m3u8
    ?sig={sig}&token={token}&allow_source=true&allow_audio_only=true

token: JWT containing:
  {
    "channel": "shroud",
    "user_id": "456789",
    "exp": 1744300000,
    "features": { "lowLatency": true }
  }
sig: HMAC-SHA256(token, server_secret)

Response 200 (master playlist):
#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=6000000,VIDEO="1080p60",CODECS="avc1.4D0028"
https://video-edge-xyz.twitch.tv/v1/segment/hls/channel/1080p60/playlist.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=3000000,VIDEO="720p60"
https://video-edge-xyz.twitch.tv/v1/segment/hls/channel/720p60/playlist.m3u8
...
```

### Chat (IRC over WebSocket)

```
WebSocket connect: wss://irc-ws.chat.twitch.tv:443

Handshake:
  PASS oauth:{oauth_token}
  NICK {username}
  JOIN #{channel_name}
  CAP REQ :twitch.tv/membership twitch.tv/tags twitch.tv/commands

Incoming message format:
  @badge-info=subscriber/8;badges=subscriber/6;color=#FF0000;display-name=CoolViewer;
  emotes=25:0-4;flags=;id=msg-uuid;mod=0;room-id=123456;subscriber=1;
  tmi-sent-ts=1744300000000;turbo=0;user-id=789012;user-type=
  :coolviewer!coolviewer@coolviewer.tmi.twitch.tv PRIVMSG #channel :Kappa Great clip!

Rate limit: 20 messages/30s per user in normal channels; 100/30s for mods
```

### Clips

```
POST /helix/clips
Authorization: Bearer {user_token}
Content-Type: application/json

{
  "broadcaster_id": "123456",
  "has_delay": false  -- if true, 5-second delay before clip start (to account for chat latency)
}

Response 202:
{
  "data": [{
    "id": "FunkyBraveSlothRuleFive-abcdef",
    "edit_url": "https://clips.twitch.tv/FunkyBraveSloth...-abcdef/edit"
  }]
}
-- Clip is asynchronously processed; poll GET /helix/clips?id={id} for status

GET /helix/clips?id={clip_id}
Response 200:
{
  "data": [{
    "id": "FunkyBraveSlothRuleFive-abcdef",
    "url": "https://clips.twitch.tv/FunkyBraveSloth...-abcdef",
    "embed_url": "https://clips.twitch.tv/embed?clip=...",
    "broadcaster_id": "123456",
    "creator_id": "789012",
    "video_id": "987654321",
    "game_id": "21779",
    "title": "Amazing play",
    "view_count": 14230,
    "created_at": "2025-04-09T19:45:00Z",
    "thumbnail_url": "...",
    "duration": 28.4
  }]
}

Rate limit: 600 clip creations/min per broadcaster (to prevent abuse); 10/min per user
```

### VODs

```
GET /helix/videos?user_id={id}&type=archive|highlight|upload&first={n}&after={cursor}
Authorization: Bearer {token}

GET /helix/videos?id={video_id}

Response 200:
{
  "data": [{
    "id": "987654321",
    "stream_id": "1234567890",
    "user_id": "123456",
    "user_name": "shroud",
    "title": "Dota 2 Stream",
    "description": "",
    "created_at": "2025-04-09T18:00:00Z",
    "published_at": "2025-04-09T18:00:00Z",
    "url": "https://www.twitch.tv/videos/987654321",
    "thumbnail_url": "...",
    "viewable": "public",
    "view_count": 82432,
    "language": "en",
    "type": "archive",
    "duration": "3h47m14s",
    "muted_segments": [{"duration": 30, "offset": 1234}]
  }],
  "pagination": { "cursor": "..." }
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Live Stream Ingest and Transcoding Pipeline

**Problem it solves:**
A streamer sends one RTMP stream at one quality level (their upload bandwidth limit). The system must accept this single ingest stream at 50,000+ simultaneous streams, transcode each into 5-6 quality levels in real-time (while the stream is happening — not after), and make those segments available for global CDN distribution within 2 seconds of being generated. This is fundamentally harder than VOD transcoding: there is no "file" — it's a continuous real-time stream.

**Possible Approaches:**

| Approach                               | Latency to viewers | Throughput | Cost         | Complexity |
|----------------------------------------|--------------------|------------|--------------|------------|
| Single box per stream (no GPU)         | ~3-4 s             | Low        | Low          | Low        |
| GPU farm (one GPU per stream)          | ~2-3 s             | High       | Medium       | Medium     |
| Distributed transcode (split by time segment) | ~5-10 s    | Very high  | High         | Very high  |
| Cloud GPU instances (on-demand)        | ~2-4 s             | Elastic    | High (spot)  | Medium     |

**Selected Approach: One transcoding worker (GPU-enabled) assigned per active stream.**

This is the correct approach for live streaming. Unlike VOD (where you can split a file), a live stream must be transcoded in sequence — you cannot parallelize across future segments that don't exist yet. The parallelism comes from having one worker per stream across a cluster.

**Detailed Flow:**

1. **RTMP Ingest**: Streamer's OBS connects to nearest ingest PoP via Anycast IP `live.twitch.tv`. Custom RTMP server validates `stream_key` via lookup in Redis (key → user_id, mapped there from MySQL at stream start). If valid, RTMP server writes stream to a local ring buffer and assigns a transcoding worker.

2. **Stream Relay**: Ingest PoP forwards the RTMP stream to the transcoding cluster via an internal relay protocol (TCP/custom). Transcoding clusters are co-located with or near ingest PoPs to minimize relay latency.

3. **Real-Time Transcoding (FFmpeg NVENC)**:
   ```
   ffmpeg -re -i rtmp://localhost/app/stream_key \
     -vf scale=1920:1080 -c:v h264_nvenc -b:v 6000k -maxrate 6500k -bufsize 12000k \
       -profile:v high -level 4.2 -g 60 -keyint_min 60 -sc_threshold 0 \
       -f hls -hls_time 2 -hls_list_size 45 -hls_flags delete_segments+append_list \
       s3://twitch-live-segments/{stream_id}/1080p60/playlist.m3u8 \
     -vf scale=1280:720 -c:v h264_nvenc -b:v 3000k ... \
       s3://twitch-live-segments/{stream_id}/720p60/playlist.m3u8 \
     -vf scale=852:480 -c:v h264_nvenc -b:v 1000k ... \
       s3://twitch-live-segments/{stream_id}/480p30/playlist.m3u8 \
     -vn -c:a copy \  -- audio passthrough (AAC, no re-encode)
       ... (additional quality levels)
   ```
   Key parameters: `-g 60` forces keyframe every 2 seconds (at 30fps). `-hls_time 2` creates 2-second segments. `-hls_list_size 45` keeps last 90 seconds of segments in playlist.

4. **Segment Storage**: 2-second segments written to Amazon S3 with `Cache-Control: max-age=2` (live; segment is immutable once written but will expire from CDN edge after 2 seconds to force re-fetch of latest). Workers use multipart upload for large chunks; segments are typically 500 KB - 1.5 MB.

5. **Manifest Update**: After each segment is written, the HLS playlist file (`.m3u8`) is updated to include the new segment and drop segments older than 90 seconds. The manifest has `EXT-X-TARGETDURATION:2` and no `EXT-X-ENDLIST` tag (indicating a live stream). Manifest update is an atomic S3 PUT.

6. **CDN Distribution**: CDN origin pull fetches the manifest on first viewer request. Subsequent viewers hit the cached manifest. CDN edge holds manifest for 2 seconds (matching segment duration) before re-fetching from origin. Segments are cached for 30 seconds (they're immutable once written for the live stream).

**Transcoder Worker Assignment:**
- Worker pool manager (backed by Redis): `LPUSH available_workers {worker_id}`. On new stream: `RPOPLPUSH available_workers busy_workers` → atomically assigns next available worker.
- Worker capacity: one T4 GPU can handle ~4 simultaneous live streams at 6 quality levels each (6 concurrent FFmpeg encode passes). With NVENC, a T4 can encode 1080p30 H.264 at ~800fps — sufficient for 4 streams at 30fps each.
- For 50,000 concurrent streams: 50,000 / 4 = 12,500 T4 GPU instances. At AWS `g4dn.xlarge` (~$0.526/hr): ~$6,500/hr for GPU transcoding. Real cost is lower due to Reserved Instances and spot mix.
- Auto-scaling: When queue of unassigned streams > 5, scale up GPU fleet. When busy workers < 50% for 5 minutes, scale down.

**Q&A:**

Q: Why not use segment-parallel transcoding for live streams like YouTube does for VODs?
A: Segment-parallel transcoding requires you to know all future segments ahead of time (to split the file). For a live stream, future segments don't exist yet. You must transcode as the stream arrives. The only valid parallelism is within a single stream's quality levels (FFmpeg's `-filter_complex` can pipe one decode to multiple encoders), which FFmpeg already does natively. The cross-stream parallelism comes from having one worker per concurrent stream.

Q: What happens if a transcoding worker crashes while a stream is live?
A: Stream viewers experience a brief stall (~5-10 seconds). The ingest server detects the relay disconnection, fetches a new worker from the pool, and re-establishes the relay. The new worker starts transcoding from the current live position (no replay of missed frames). The HLS playlist on the viewer side has a ~90-second buffer of segments; they will stall only if recovery takes more than 45 segments (90 seconds). With typical recovery in < 10 seconds, most viewers experience a brief rebuffer but the stream continues.

Q: How do you handle a burst where 1,000 new streams start simultaneously (e.g., a major gaming event announcement)?
A: Pre-scaling: Twitch's auto-scaling monitors CPU and memory trends plus calendar events (major game releases, esports events) and pre-provisions extra workers. For unexpected bursts: the worker assignment queue absorbs ingest connections (RTMP connection succeeds immediately; transcoding starts within 5 seconds as a worker becomes available). Viewers attempting to watch a stream that hasn't finished being assigned will receive a "stream starting" state from the manifest service until transcoding begins.

Q: How does Twitch handle streamers who send malformed or corrupted RTMP streams?
A: The ingest RTMP server validates: (1) Keyframe interval ≤ 4 seconds (streams with too-wide keyframes cause playback issues). (2) Bitrate within 8,500 Kbps limit (excess bitrate is capped). (3) Video codec must be H.264 or H.265 (RTMP with VP9 is rejected). (4) Audio must be AAC (MP3 audio is transcoded to AAC, accepted but logged). For catastrophically malformed streams (corrupted NAL units), FFmpeg's error concealment handles minor issues; severe corruption causes the ingest server to drop the connection with a `streamStart.error.badStream` RTMP status code.

Q: How does Twitch's Low-Latency HLS (LHLS) work and how does it differ from standard HLS?
A: Standard HLS: viewer waits for a full 2-second segment to be generated and cached before downloading it. With 3 segments of buffering, glass-to-glass latency ≈ 6-8 seconds. LHLS (also called LL-HLS per Apple's spec): segments are broken into "parts" (0.2-second partial segments). The viewer can begin downloading a part before the full 2-second segment is complete. The playlist is updated with `EXT-X-PART` tags as each part completes. The viewer requests the next part with a `_HLS_part` query parameter. Twitch's implementation achieves ~2-3 second latency. The tradeoff: more HTTP requests (10× more than standard HLS), requires HTTP/2 to multiplex efficiently, and higher CDN origin load.

---

### 6.2 Chat System at Scale (IRC-based, WebSocket fan-out)

**Problem it solves:**
Twitch chat is the defining social feature of the platform. Popular streamers (Ninja, shroud) regularly have 50,000-100,000 concurrent chatters, generating 5,000-20,000 messages per minute in a single channel. The chat system must deliver messages to all viewers with < 500 ms latency, handle spikes during exciting moments (a streamer's first boss kill generates a "PogChamp" flood), maintain message ordering within a channel, and filter/moderate banned words and spam.

**Possible Approaches:**

| Approach                              | Scale       | Latency | Ordering | Complexity |
|---------------------------------------|-------------|---------|----------|------------|
| IRC (single server per channel)       | ~10K users  | Low     | Strong   | Low        |
| IRC with horizontal fan-out           | ~100K users | Low     | Strong   | Medium     |
| MQTT pub/sub                          | Very high   | Low     | Weak     | Medium     |
| WebSocket + Redis Pub/Sub fan-out     | High        | Low     | Per-channel | Medium  |
| Kafka + WebSocket delivery            | Very high   | Moderate| Partition-ordered | High |
| Custom distributed messaging          | Very high   | Low     | Per-channel | Very high|

**Selected Approach: IRC over WebSocket with Redis Pub/Sub fan-out per channel.**

Twitch's chat uses the IRC protocol (RFC 1459) served over WebSockets (wss://irc-ws.chat.twitch.tv). Internally, message delivery uses a custom fan-out system:

**Architecture:**

```
Viewer WebSocket client
        │ PRIVMSG #channel :Hello!
        ▼
Chat Ingress Server (IRC acceptor)
        │ Parse IRC message
        │ Apply rate limits (20 msg/30s per user)
        │ Run spam/moderation filters
        │ Assign message_id (UUID)
        ▼
Channel Router
        │ PUBLISH channel:{channel_id} {serialized message}
        ▼
Redis Pub/Sub (per-channel topics)
        │ Fan-out to all subscribed Chat Distribution Servers
        ▼
Chat Distribution Servers (one cluster per channel shard)
        │ Each server maintains WebSocket connections for viewers in this channel
        │ Delivers message to all connected viewers
        ▼
Viewer WebSocket clients (receive message)
```

**Key Design Details:**

1. **Channel Sharding**: Channels are sharded across Chat Distribution Server clusters. High-traffic channels (Top 1,000) get dedicated server pools. Assignment: consistent hash of `channel_id` → server cluster. A server cluster maintains WebSocket connections for all viewers watching that channel shard.

2. **Connection Fanout Math**: With 100,000 concurrent viewers in one channel, each message must be pushed to 100,000 WebSocket connections. At 5,000 msg/min = 83 msg/sec:
   - 83 msg/sec × 100,000 connections = 8.3 million WebSocket write operations/sec for that channel alone.
   - Each message ~200 bytes. 8.3M × 200 bytes = ~1.66 Gbps per channel peak.
   - A single server with 10GbE can handle this, but Twitch distributes across a pool to handle subscriber overhead.

3. **Message Ordering**: IRC is inherently ordered per connection. Twitch adds `tmi-sent-ts` (server timestamp in milliseconds) to each message. Clients display messages in server-timestamp order. For bursty floods, messages may arrive slightly out of order at the client due to network variability; client-side sorting by timestamp for the last 100 ms window fixes this.

4. **Moderation (AutoMod)**: Before publication, messages pass through AutoMod — a machine learning classifier (BERT-based model) trained on millions of labeled Twitch chat messages. Categories: harassment, hate speech, sexual content, spam. Flagged messages are held in a review queue visible to moderators. Automod confidence threshold is configurable per channel.

5. **Emote Handling**: Twitch emotes (Kappa, PogChamp, LUL) are stored as images on CDN. The IRC message contains emote positions: `emotes=25:0-4;425618:6-11` (emote ID 25 appears at char positions 0-4). Client renders the emote image inline. Emote IDs are resolved to CDN URLs via a static mapping cached in the client. BTTV/FrankerFaceZ third-party emotes are handled by browser extensions, not the core chat system.

6. **Subscriber-only and slow modes**: Mode flags stored in Redis per channel (`HSET channel_modes:{channel_id} sub_only 1`). Ingress server checks mode before accepting a message. Slow mode: `SETNX rate_limit:{user_id}:{channel_id} 1 EX 30` — if key exists, user is rate-limited.

7. **Banned users/words**: Banned users stored in Redis Set: `SADD banned:{channel_id} {user_id}`. Banned words stored as a BloomFilter (approximate: false positives → message reviewed by exact match; no false negatives). BloomFilter for 100,000 banned phrases per channel requires ~1 MB memory at 1% false positive rate.

**Q&A:**

Q: How do you handle a chat flood during an exciting moment (e.g., streamer wins tournament) — 10,000 messages per second in one channel?
A: Rate limiting is the primary defense: 20 msg/30s per non-mod user limits each individual user's flood. But with 100,000 chatters simultaneously spamming, aggregate rate is still massive. Twitch applies: (1) Subscriber-only mode (channel owner enables → only subs can chat). (2) Emote-only mode (limits messages to emotes, shorter). (3) Slow mode (force 3-30s between messages per user). (4) Client-side throttling: player chat renders at maximum 50 messages/second visible to the viewer; excess messages are discarded client-side (viewer sees fast-scrolling chat). (5) Server-side: if aggregate rate > 10,000/sec for a channel, a "chat overflow" mode triggers — messages are randomly sampled (50% drop rate) to reduce downstream fanout.

Q: Why IRC rather than a more modern protocol like MQTT or gRPC streaming?
A: IRC was chosen at Twitch's founding because of its widespread tooling (OBS has built-in IRC integration, third-party bots use existing IRC libraries). The protocol is simple, text-based, and its client library ecosystem is massive. The WebSocket transport is modern. Twitch extended IRC with custom tags (`@badge-info=...`) for Twitch-specific data while maintaining backward compatibility with IRC clients. Migrating to a new protocol would break thousands of bots and third-party integrations — the cost exceeds any benefit.

Q: How do you store and query chat history for VOD replay (chat replay feature)?
A: During a live stream, chat messages are written to Cassandra with partition key `(channel_id, date)` and clustering key `sent_at`. This allows time-range queries: `WHERE channel_id=X AND sent_at BETWEEN start AND end ORDER BY sent_at ASC`. For VOD replay, the VOD player requests chat messages for each 30-second window as the viewer watches. Messages are returned with `vod_offset_sec = sent_at - stream_started_at`. Cassandra handles the time-series append pattern efficiently; partition size is bounded by daily message volume (even top channels generate < 5 GB/day of chat data in Cassandra).

Q: How does Twitch prevent bots from flooding chat?
A: Multi-layer detection: (1) Rate limiting (per user, per channel). (2) Device fingerprinting: new accounts from the same device/IP are rate-limited more aggressively. (3) Account age requirement: accounts < 5 minutes old cannot chat (configurable per channel). (4) CAPTCHAs on account creation. (5) AutoMod's spam classifier detects repetitive messages and copy-paste floods. (6) Verified phone numbers for chatting (configurable per channel). (7) Channel bot detection: a bot that sends identical messages to 1,000 channels simultaneously gets its OAuth token revoked and user_id banned.

Q: How do you scale the IRC WebSocket infrastructure to handle 2+ million concurrent connections?
A: Each Chat Distribution Server maintains ~50,000 WebSocket connections (limited by file descriptors, memory, and CPU for TLS). At 2 million concurrent connections: 2,000,000 / 50,000 = 40 servers minimum. With headroom (target 60% utilization): ~70 servers per region. WebSocket connections are long-lived (~session duration: 95 min average), so connection churn is relatively low. Load balancers use IP hash (consistent routing) so reconnects from the same client return to the same server (session affinity). This minimizes channel subscription re-setup overhead.

---

### 6.3 Clip Generation Pipeline

**Problem it solves:**
Viewers want to capture exciting moments from live streams and share them. A clip is a 5-60 second highlight created by any viewer during a live stream or from a VOD. The system must: (1) Buffer the last 90 seconds of every live stream (for "clip from live"), (2) Accept a clip creation request specifying a time window, (3) Extract, transcode, and make the clip available within 30-60 seconds, (4) Handle 58+ clip creation events per second globally.

**Possible Approaches:**

| Approach                                  | Latency    | Complexity  | Storage Overhead |
|-------------------------------------------|------------|-------------|------------------|
| Server-side re-encode from source segments| 30-90 s    | Medium      | Permanent clip storage |
| Client-side capture (browser MediaRecorder)| Instant   | Low         | Client bandwidth |
| Pre-split fixed-length clips              | Instant    | Low         | Very high (all windows) |
| Async transcode with live buffer          | 30-60 s    | Medium      | Permanent clip storage |

**Selected Approach: Async server-side extraction from rolling HLS segment buffer.**

**Flow:**

1. **Live Buffer Maintenance**: The transcoding worker for each active stream maintains a rolling 90-second buffer of segments in S3 (the same `hls_list_size 45` segments used for live playback). These segments are tagged with `expires: +2 hours` on S3 (to allow post-stream clip creation).

2. **Clip Request**:
   - Viewer sends `POST /helix/clips { broadcaster_id }` during live stream.
   - Clip Service records the clip request timestamp (`clip_start_unix_ms = now_ms`).
   - By default, the clip starts 30 seconds before now and ends now (capturing the last 30 seconds). With `has_delay=true`, an additional 5-second delay is added to account for streamer's display lag vs. viewer's chat.

3. **Segment Selection**:
   - Clip Service queries the live stream's HLS manifest to find which segments cover `[clip_start, clip_end]`.
   - For a 30-second clip at 2s segments: 15 segments needed.
   - Source quality: highest available (1080p60 for Partners, 720p for others).

4. **Async Transcoding**:
   - Job published to SQS queue: `{clip_id, broadcaster_id, segment_keys[], start_offset_sec, duration_sec}`.
   - Clip worker: downloads segments from S3, concatenates via `ffmpeg -f concat`, trims to exact duration, encodes at clip quality levels (720p, 480p, 360p).
   - Output: 3 quality levels × 1 clip = ~3 FFmpeg passes.
   - Duration: a 30-second clip takes ~15 seconds to transcode on a CPU worker (no GPU needed; throughput not as critical as latency).

5. **Completion**:
   - Transcoded clip segments uploaded to S3 permanent storage (`twitch-clips/{clip_id}/...`).
   - Clip metadata written to MySQL (`clips` table, `status='published'`).
   - CDN URL generated and returned via `GET /helix/clips?id={clip_id}` polling.

6. **Thumbnail**: First frame of clip extracted as JPEG: `ffmpeg -ss 0 -i clip.mp4 -vframes 1 -q:v 2 thumb.jpg`.

**Clip Storage Management:**
- 58 clips/sec × 30 MB avg (three quality levels × 30s × various bitrates) = ~1.74 GB/sec = ~150 TB/day clip storage.
- Most clips get < 100 views; long-tail clips older than 6 months are migrated to S3 Glacier for cost reduction (pre-fetch on demand when requested).
- Viral clips (> 10K views) served via CDN with `Cache-Control: public, max-age=86400` (1-day CDN cache, immutable content).

**Q&A:**

Q: How do you handle a clip request that spans a stream restart (streamer disconnected and reconnected mid-stream)?
A: Stream restarts generate a new `stream_id` in the database and a new set of HLS segments. The Clip Service detects this: the requested time window spans two stream_ids. Options: (1) Truncate clip to just before the restart (shorter clip, no error). (2) Return error to user: "Clip creation failed — stream restarted in this window." Twitch implements option 1 for a better user experience.

Q: How do you prevent a viewer from creating thousands of clips maliciously?
A: Rate limits: 600 clip creations/min per broadcaster channel (prevents their segment buffer from being heavily read); 10 clip creations/min per viewer user_id. Accounts must meet minimum criteria (account age > 7 days, or verified phone, configurable per broadcaster). Clip creation counts toward API rate limits (points-based).

Q: What is the "Edit" URL returned in the clip creation response?
A: Clip creation returns an `edit_url` (e.g., `https://clips.twitch.tv/{clip_id}/edit`) that allows the clip creator to: (1) Trim the clip to a shorter window. (2) Set the clip title. (3) Publish or keep private. The edit UI shows the source video with trim handles; on "Publish", a re-transcode job is triggered for the trimmed range. This is why clip creation returns `202 Accepted` (async) rather than `201 Created` (synchronous).

Q: How do clips work for Twitch raids, where a stream might have 10,000 viewers all creating clips simultaneously on a hype moment?
A: Raid events generate burst clip creation. The SQS queue absorbs the burst. Worker pool auto-scales (ECS service scaling based on queue depth). Clips are returned as `202 Accepted` immediately; processing completes within 60-120 seconds under load. The broadcaster is notified of popular clips via their Creator Dashboard analytics (top clips sorted by view count). Resource quotas prevent the clip worker farm from being monopolized by a single broadcaster's viral moment.

Q: How does clip creation from VOD differ from clip creation from a live stream?
A: VOD clips don't need a rolling live buffer — the entire VOD is in S3. The Clip Service fetches the exact VOD segments covering the requested time range (specified by `vod_offset_sec` in the request). Otherwise the pipeline is identical. VOD clips can be created at any time after the VOD is published, not just within 90 seconds like live clips. Users can trim VOD clips more precisely because there's no "live buffer expiry" pressure.

---

## 7. Scaling

### Horizontal Scaling

- **Ingest Edge Servers**: Stateless RTMP acceptors, horizontally scaled via Anycast routing. Adding a new PoP increases capacity and reduces latency for regional streamers. Each RTMP server handles ~500 simultaneous RTMP connections (limited by socket/CPU for RTMP parsing).
- **Transcoding Workers**: K8s Deployments + HPA scaled by pending SQS queue depth (one message per new stream start). Target: scale up when unassigned streams > 10; scale down (with 5-minute cooldown to avoid scaling storms) when workers < 30% utilized.
- **Chat Servers**: Scaled by WebSocket connection count. HPA target: 40,000 connections per pod (leaving 20% headroom from max 50,000). New pods join the Redis Pub/Sub subscription topology automatically.
- **Clip Workers**: ECS tasks scaled by SQS queue depth. Target: queue depth < 200 pending clips.

### Database Sharding

- **MySQL (users, streams, VODs)**: Sharded by `user_id` via Vitess (16 shards initially). Channel metadata queries always filter by `user_id`. Cross-shard queries (search) go through Elasticsearch.
- **Cassandra (chat)**: Partition key = `(channel_id, date)`. Natural distribution — high-traffic channels have larger partitions but same topology. Compaction strategy: TimeWindowCompactionStrategy (TWCS) — optimized for time-series data where old data is never updated.
- **Redis (directory, view counts)**: Redis Cluster, 16 shards. Slot assignment stable; resharding during low-traffic periods only.

### Replication

- **MySQL**: Vitess with 1 primary + 2 read replicas per shard. Replication lag < 100 ms. Read queries (channel metadata) use nearest replica.
- **Cassandra**: RF=3, LOCAL_QUORUM for consistency. Deployed in 2 AWS regions (us-east-1 primary, us-west-2 secondary). Async cross-region replication; acceptable for chat history (eventual consistency).
- **S3**: HLS live segments: single-region (performance and cost). VOD/clip storage: cross-region replication to secondary region for durability.

### Caching

| Cache Layer             | Technology   | What is Cached                              | TTL          |
|-------------------------|--------------|---------------------------------------------|--------------|
| Live stream metadata    | Redis Hash   | title, game, thumbnail per stream           | 10 sec       |
| Directory (game/streams)| Redis ZSET   | Sorted viewer counts per game               | 10 sec       |
| HLS segments (live)     | CDN          | 2-second video segments                     | 30 sec       |
| HLS manifest (live)     | CDN          | .m3u8 variant playlist                     | 2 sec        |
| HLS segments (VOD/clip) | CDN          | Immutable video segments                    | 7 days (clips), 60 days (VOD) |
| User auth tokens        | Redis        | OAuth tokens validated status               | TTL = token expiry |
| Channel emote list      | CDN + client | Emote ID → CDN URL mapping                  | 1 day        |
| Banned user list        | Redis Set    | Banned user IDs per channel                 | Immediate update |
| Chat moderation state   | Redis Hash   | sub_only, slow_mode, emote_only per channel | Immediate    |

**Interviewer Q&A:**

Q: How do you handle the "view count" update for the directory page when 7 million viewers are changing streams every few minutes?
A: View count is maintained via atomic Redis INCR/DECR. When a viewer joins a stream: `INCR viewcount:{stream_id}`; on leave: `DECR viewcount:{stream_id}`. A background aggregator (cron job every 10s) reads all stream view counts from Redis, batch-updates the `streams.concurrent_viewers` column in MySQL, and updates the Redis sorted set for the directory: `ZADD live:{game_id} {count} {stream_id}`. The directory page reads from Redis ZSET (O(log N + M) for top-M streams per game), not MySQL. This separates the high-frequency count writes from the low-latency directory reads.

Q: How does Twitch handle geographic distribution for streamers and viewers in different regions?
A: The ingest PoP is always in the region nearest to the streamer (low RTMP ingress latency is critical — RTMP is TCP and intolerant of high RTT). Transcoding happens in the same region or an adjacent DC. CDN edge nodes are global (Akamai/Fastly have worldwide PoPs). The CDN pull-request chain: viewer → CDN edge (local region) → CDN mid-tier → CDN origin pull → Twitch S3 origin (in streamer's ingest region). For a Korean streamer watched by US viewers, HLS segments originate from AWS ap-northeast-2, are pulled to CDN origin, then distributed to us-east CDN PoPs. After the first few viewers in each CDN PoP warm the cache, all subsequent viewers in that PoP serve from CDN edge with no cross-ocean traffic.

Q: How does Twitch scale for major esports events (e.g., League of Legends Worlds with 5 million concurrent viewers on a single stream)?
A: Single-stream peak concurrency: 5 million viewers on one channel. Challenges: (1) CDN: the manifest and segments for that one stream must be served to 5M viewers polling every 2 seconds. 5M × 6 quality × 1 segment/2s = 15 million segment requests/sec for this one stream. This is handled by CDN's global cache — all viewers get the same segments, so cache hit rate approaches 100% after warm-up. CDN capacity is horizontally infinite for repeated content. (2) Chat: 5M chatters. Subscriber-only and slow mode mandatory during peak. Clip creation rate-limited per channel. (3) API/metadata: stream metadata served from Redis, replicated to all regions. (4) Transcoding: only 1 transcoding worker needed (single ingest stream) — this is the least complex part.

Q: How would you handle a major outage where the S3 bucket storing live HLS segments becomes unavailable?
A: CDN holds segments for 30 seconds after generation. During an S3 outage, CDN continues serving the last cached segments for 30 seconds before viewers experience stalls. Mitigation: (1) Fallback to a secondary storage backend (different S3 bucket / GCS bucket) — configured in the transcoding worker as a failover write target. (2) For VOD segments (long-term storage), cross-region S3 replication provides redundancy. (3) Alert triggers immediately on S3 error rate > 1% in CloudWatch; on-call engineers investigate. RTO: if secondary storage is pre-configured, < 60 seconds failover. Otherwise, manual intervention: 5-15 minutes.

Q: How do you prevent stream key leakage and unauthorized streaming on someone else's channel?
A: Stream keys are long random tokens (> 128 bits entropy). Each request to ingest validates key in Redis (key → user_id, loaded from MySQL on stream start). Twitch supports: (1) Reset stream key via dashboard (invalidates old key immediately). (2) 2FA-protected stream key access. (3) IP rate limiting on RTMP connections per stream_key (> 3 simultaneous connections on same key → reject duplicates). (4) Stream keys never exposed in API responses (write-only after creation). (5) Stream key rotation after any security event (compromised credential flow triggers automated rotation).

---

## 8. Reliability & Fault Tolerance

| Failure Scenario                          | Detection                                  | Mitigation                                                             | RTO       |
|-------------------------------------------|--------------------------------------------|------------------------------------------------------------------------|-----------|
| RTMP ingest server crash                  | Health check miss (30s) + active streams   | Stream disconnects; OBS retries after 5s; new ingest PoP assigned     | < 30 s    |
| Transcoding worker crash mid-stream       | Heartbeat miss from worker + manifest staleness | Worker pool assigns new worker; stream resumes; brief viewer stall | < 10 s    |
| S3 write failure (segment upload)         | S3 PutObject error                         | Worker retries 3× with exp. backoff; falls back to secondary storage  | < 5 s     |
| CDN edge PoP outage                       | CDN monitoring + viewer error rate spike   | DNS TTL expiry routes to alternate PoP; Fastly failover (dual CDN)    | < 60 s    |
| Redis primary failure (view counts)       | Redis Sentinel quorum detection            | Replica promoted; view count in-flight increments lost (< 10s window) | < 5 s     |
| MySQL primary failure (stream metadata)   | Vitess VTOrc (10s detection)               | Replica promoted; stream metadata cached in Redis, unaffected         | 10-30 s   |
| Chat server crash (50K connections)       | Client WebSocket disconnect                | Client reconnects in 5s; Redis Pub/Sub re-subscription; no messages lost (Cassandra) | < 10 s |
| Cassandra node failure (chat storage)     | Gossip protocol                            | RF=3; reads/writes continue to 2 remaining replicas                   | < 30 s    |
| Clip worker fleet outage                  | SQS queue depth alarm                      | Clip creation queued; processed when fleet recovers; clips delayed    | Best-effort |
| Total region failure (AWS us-east-1)      | Route 53 health checks                     | Ingest PoPs redirect to us-west-2; Cassandra cross-region active-active | < 5 min  |

**Idempotency & Retries:**
- Clip creation: idempotent by `(broadcaster_id, clip_start_timestamp)` — duplicate requests within 30 seconds return existing `clip_id`.
- Segment uploads: S3 PutObject with conditional `If-None-Match: *` prevents duplicate segment writes; safe to retry.
- Subscription events (financial): idempotency key from payment processor; stored in `subscriptions` table with unique constraint.

**Circuit Breaker:**
- AutoMod classifier: if model inference latency > 200 ms or error rate > 5%, circuit opens → messages pass through without AutoMod filtering. Alert fires; on-call restores model. This ensures chat is never blocked by a failing ML service.
- Recommendation system (stream directory): if Redis ZSET queries fail, fall back to MySQL query sorted by `concurrent_viewers DESC`. Higher latency but functional.

---

## 9. Monitoring & Observability

| Metric                              | Type      | Alert Threshold                | Tool            |
|-------------------------------------|-----------|--------------------------------|-----------------|
| RTMP ingest failure rate            | Counter   | > 1% per PoP per 5 min        | Prometheus      |
| Transcoding lag (delay behind live) | Histogram | > 10 s p99                    | Prometheus      |
| HLS segment delivery latency        | Histogram | > 5 s p99 (CDN to viewer)     | Real User Monitoring (RUM) |
| Concurrent viewers (global)         | Gauge     | Unexpected > 20% drop         | Grafana         |
| Concurrent live streams             | Gauge     | > 60,000 (capacity warning)   | Grafana         |
| Chat message delivery latency       | Histogram | > 500 ms p99                  | Prometheus      |
| Chat message drop rate              | Counter   | > 0.1%                        | Prometheus      |
| Clip creation success rate          | Counter   | < 95% over 10 min             | Prometheus      |
| S3 error rate (segment upload)      | Counter   | > 0.5%                        | CloudWatch      |
| CDN origin pull rate (cache miss)   | Gauge     | > 10% miss rate on live segs  | CDN Dashboard   |
| Redis memory utilization            | Gauge     | > 75%                         | Grafana         |
| Transcoder GPU utilization          | Gauge     | > 90% for 5 min               | NVIDIA DCGM     |
| API 5xx error rate                  | Counter   | > 0.5% over 2 min             | Datadog         |

**Distributed Tracing:**
- Jaeger deployed on Kubernetes. Every API request tagged with `trace_id`. End-to-end trace: API Gateway → Clip Service → S3 → CDN.
- Special tracing for live stream path: ingest server emits span on `stream_start`; transcoding worker emits span per segment; manifest service emits span per manifest update. Allows "stream health" trace for any active stream.

**Logging:**
- Structured logs via Fluentd → Kafka → Elasticsearch (ELK stack).
- Real-time log anomaly detection via ElastAlert: regex patterns for `FATAL`, `OOM`, `S3 error`.
- Stream health log: every RTMP connection logged with `stream_id, user_id, ingest_server, bitrate_kbps, start_time`. Enables post-incident analysis of which streams were affected by an outage.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                              | Choice Made                           | Alternative Considered           | Trade-off                                                         |
|---------------------------------------|---------------------------------------|----------------------------------|-------------------------------------------------------------------|
| Ingest protocol: RTMP                 | RTMP (TCP, port 1935)                 | WebRTC (UDP-based)               | +Universal OBS/encoder support. −TCP latency; no FEC; ~5s glass-to-glass |
| Transcoding: one worker per stream   | Dedicated worker per concurrent stream| Shared/multiplexed workers       | +Fault isolation. −Resource waste if stream is low-bitrate        |
| Live stream storage: S3 (rolling)     | S3 with 90s rolling window + lifecycle| Local disk ring buffer           | +Durability, +Clip support. −S3 write latency adds ~100ms        |
| Chat: IRC over WebSocket              | IRC protocol                          | MQTT or custom protocol          | +Ecosystem compatibility (bots, 3rd party). −Protocol age limits features |
| Chat fan-out: Redis Pub/Sub           | Redis Pub/Sub per channel             | Kafka per channel                | +Low latency (<1ms pub/sub). −Not persistent; messages lost if sub offline |
| Directory: Redis ZSET                 | Redis sorted sets per game             | MySQL sorted query               | +Sub-ms reads. −Memory cost; cold start on Redis restart         |
| Clip pipeline: async SQS              | Async, 30-60s delivery                | Synchronous (inline transcode)   | +Better scaling. −User waits for clip; UX requires polling/push  |
| Latency mode: HLS (default)           | Standard HLS (~6s) + LHLS opt-in     | WebRTC for all (<1s)            | +Massive scale, all devices. −6s glass-to-glass vs WebRTC 1s     |
| CDN: Akamai + Fastly dual             | Multi-CDN                             | Single CDN vendor                | +Redundancy, +Negotiating leverage. −Configuration complexity    |
| Database: MySQL + Vitess              | Sharded relational                    | Pure DynamoDB                    | +Flexible queries, +AWS-agnostic. −Sharding ops complexity       |

---

## 11. Follow-up Interview Questions

**Q1: How would you design ultra-low latency streaming (< 1 second) for Twitch?**
A: WebRTC is the only protocol achieving sub-second latency at scale. Architecture: streamer sends WebRTC (instead of RTMP) to an SFU (Selective Forwarding Unit, e.g., built on Pion/Go-based media server). SFU forwards encoded media to viewer WebRTC connections. Challenges: (1) WebRTC SFU cannot fan out to millions of viewers — maximum ~10,000 per SFU cluster. For popular streams, a cascade of SFUs (origin SFU → regional SFUs → viewers) is needed, but each hop adds ~100-200 ms. (2) WebRTC uses UDP — packet loss requires FEC (Forward Error Correction) since retransmission latency is unacceptable. (3) No existing recording infrastructure — WebRTC output must be remuxed to HLS for VOD. Twitch currently offers LHLS (3-4s) as the lowest-latency option; WebRTC is for special use cases (Twitch IRL, squad stream).

**Q2: How does Twitch handle DMCA copyright strikes on VODs and live streams?**
A: Twitch uses audio fingerprinting (similar to Audible Magic) for VODs: after stream ends, the VOD audio track is fingerprinted and matched against a database of copyrighted music. Matching sections are muted (replaced with silence) and logged in `vods.muted_segments`. The broadcaster is notified. For live streams, real-time audio fingerprinting is computationally expensive — Twitch does not currently mute live streams in real-time but processes the VOD post-stream. Repeat DMCA offenders face channel suspension via a 3-strikes policy. Twitch also has a "Soundtrack by Twitch" product: licensed royalty-free music that streamers can play without risk — it's inserted as a second audio track and the copyrighted track is stripped from VODs automatically.

**Q3: How does the Twitch Predictions/Poll feature work technically?**
A: Predictions: streamer creates a prediction event ("Who wins the next game?"). Viewers commit Channel Points (virtual currency, not real money) to one outcome within a 30-second window. At outcome resolution, points are distributed to winners proportionally. Technical design: prediction events stored in MySQL (`predictions` table). Open outcomes updated in Redis (atomic `INCR` for points accumulated per outcome per prediction). When prediction closes, a worker reads total points per outcome from Redis, calculates payouts, and issues Channel Point credit transactions in batch (MySQL transaction). Total Channel Point ledger is eventually consistent (batch processed). Real-time prediction state (currently winning outcome, points distribution) served from Redis with 5-second refresh to viewers.

**Q4: How would you design the "Raids" feature (one streamer's audience teleports to another channel)?**
A: A raid sends the raiding channel's concurrent viewers to a target channel. When the raid executes: (1) The source channel sends a "raid" IRC message to chat (displayed as "X is raiding with Y viewers!"). (2) The source channel's player automatically navigates viewers to the target channel page. (3) A raid event is published to the notification service → alerts target channel's streamer via overlay alert. (4) The "raid train" chat message `(@tags :host_user!host_user@host_user RAID #target) :10000` is delivered to target chat. (5) View count transfer: source stream's concurrent_viewers decremented; target incremented (not actual viewers moving — clients navigate independently). Technically it's just a coordinated JavaScript redirect + IRC message; no data migration needed.

**Q5: How do you handle stream quality drops when the streamer's internet connection degrades mid-stream?**
A: RTMP is TCP — when a streamer's connection degrades, the TCP window shrinks, reducing throughput. The ingest server observes: (1) Input bitrate drops below configured stream bitrate → jitter/rebuffering on transcoder input. (2) RTMP connection becomes unstable (TCP RST). Handling: the ingest server monitors input bitrate. If bitrate < 50% of expected for > 5 seconds, it sends a `_error.notEnoughBandwidth` RTMP status code suggestion to the client (OBS will display a warning). The transcoder continues with reduced quality input — FFmpeg's decoder handles dropped frames via error concealment. If the RTMP connection drops entirely, OBS retries automatically. Viewers experience freezing/pixelation then a brief reconnect gap.

**Q6: How does Twitch's subscription tier system work — where does the money go?**
A: Tier 1 ($4.99): Twitch takes 50% (historically), partner takes 50% for eligible Partners (recently changed to 70/30 for top partners). The revenue split is stored per-broadcaster in the `broadcaster_subscription_config` table. Monthly subscriptions are processed via Stripe/payment processor. Subscription revenue distributed monthly: aggregate subscription revenue per broadcaster from previous month calculated via SQL query against `subscriptions` table (sum Tier 1 × $2.50 + Tier 2 × $5 + Tier 3 × $12.50 per subscriber). Results pushed to payment system for payout. Gift subscriptions credited immediately to receiver's account; billed to gifter immediately.

**Q7: How would you design a feature that lets viewers watch multiple streams simultaneously (Squad Stream / MultiView)?**
A: MultiView requires multiple independent HLS streams to be played in separate video elements, with independent ABR per stream, and a chat overlay combining all four chat rooms. Architecture: (1) Client side: 4 video players (MSE/HTML5), each with independent ABR and segment fetching. No server-side changes required — same HLS stream endpoints. (2) Combined chat: client subscribes to 4 channel IRC rooms simultaneously (already supported by IRC protocol — JOIN multiple channels). Messages color-coded by channel. Combined message rate could be 10,000+/min — client throttles to 100 visible messages/sec, round-robining channels. (3) Audio: user selects "active" stream for audio; other streams muted. This is predominantly a client-side feature; server infrastructure serves the same streams.

**Q8: Describe how Twitch's "channel points" system works at the database level.**
A: Channel Points are a virtual currency specific to each channel (not transferable). Earning: viewers earn points by watching (1 point/5 minutes), by subscribing, by following, and via channel-specific multipliers. Spending: redeem custom rewards defined by streamer. Data model: `channel_points_balance` table: `(viewer_id, broadcaster_id, balance BIGINT)`. Transactions: append-only `channel_points_ledger` table: `(txn_id UUID, viewer_id, broadcaster_id, delta INT, reason VARCHAR, created_at)`. Balance is derived from the ledger (event sourcing pattern) but materialized in `balance` table for fast reads. Earning events (watch time) are batched: Kafka stream of watch heartbeats → Flink streaming job aggregates to 5-minute windows → batch UPSERT to balance table. Redemption: `SELECT FOR UPDATE` on balance row → verify balance ≥ cost → UPDATE balance → INSERT ledger row → all in one transaction.

**Q9: How would you design Twitch's notification system for "channel live" push notifications?**
A: Event: streamer goes live → `stream_start` event published to Kafka topic `stream-starts`. Fan-out service consumes event → queries MySQL for follower list of `broadcaster_id` (potentially millions of rows, paginated in batches of 10,000). For each follower with `notifications_on = true`: check user's notification preferences (not on DND, not sleeping hours per timezone). Publish to `push-delivery` Kafka topic. Push delivery service reads topic → FCM (Android/iOS push), APNs (iOS), Web Push (browser). Delivery confirmation tracked per notification. For a broadcaster with 5M followers, full fan-out takes ~8 minutes (5M / 10,000 batch = 500 batches, ~1 second per batch at DB + push rate limits). This is acceptable — the streamer has been live for 8 minutes by the time last notifications are sent.

**Q10: How do you prevent scraping of the directory API to monitor competitor channels?**
A: API rate limits (800 points/min) apply to all tokens including app-level tokens. IP rate limits on the API gateway catch clients that rotate tokens but share IP. The public Twitch API does expose viewer counts by design (it's a discovery feature). To prevent automated data extraction for business intelligence: (1) Aggressive CAPTCHA challenges for anomalous request patterns. (2) Token velocity limits (a new app credential requesting directory data continuously → flagged for abuse review). (3) Cursored pagination with opaque cursors (prevents direct pagination skips). (4) For more competitive sensitivity (e.g., exact subscriber counts), these are restricted to broadcaster-scoped tokens only.

**Q11: How does Twitch ensure GDPR compliance for EU users, particularly for chat logs and watch history?**
A: Data minimization: chat messages older than 30 days are deleted from hot storage (Cassandra). VOD chat replay data is tied to VOD lifecycle (deleted when VOD expires). On account deletion request: user data deletion pipeline triggers — synchronous: session tokens revoked, account `status='deleted'`, MySQL row anonymized (email replaced with UUID). Async pipeline: Kafka event `user-deletion:{user_id}` triggers downstream deletions across Cassandra (chat messages), ClickHouse (analytics), S3 (uploaded profile images), and VODs (deleted from S3). GDPR data export: user can request data export from Account Settings → kicks off batch job to aggregate all MySQL, Cassandra data for that user → packaged as JSON → download link sent via email. SLA for deletion: 30 days per GDPR.

**Q12: What is the architecture for Twitch's Drops system (game developers reward viewers for watching)?**
A: Drops: watching an eligible stream for X minutes grants an in-game reward. Architecture: (1) Developer Portal: game developers configure drop campaigns (`drops_campaigns` table: `game_id, required_minutes, reward_id, start/end date`). (2) View time accumulation: viewer's watch heartbeats (every 60s) are consumed by a Flink streaming job. For each heartbeat, if the stream has an active drop campaign matching `game_id`, increment `drop_progress_{viewer_id}_{campaign_id}` in Redis. (3) Threshold check: when `progress >= required_minutes`, publish `drop-earned` event to Kafka. (4) Reward fulfillment: drops fulfillment service calls the game developer's fulfillment webhook (`POST https://developer-webhook.com/drops/fulfill {viewer_id, reward_id}`). Developer's backend grants the in-game item. (5) UI: viewer sees "Drop earned: {item}" notification in Twitch UI. Progress bar updated from Redis.

**Q13: How does multi-CDN work and when does Twitch switch between Akamai and Fastly?**
A: Twitch's playback API returns OCA/CDN URLs in the HLS master manifest. The CDN selection is made server-side at manifest generation time based on: (1) Real-time CDN health checks (latency probes from Twitch's monitoring nodes to each CDN PoP). (2) CDN capacity signals (Akamai and Fastly expose load/capacity APIs via their management dashboards). (3) Geographic routing (Fastly has stronger PoP presence in APAC; Akamai stronger in EU for some carriers). On CDN failure detection (error rate > 5% for a CDN PoP), the manifest service switches that region's viewers to the alternate CDN — manifest re-generated on next poll cycle (2-second TTL). Viewers experience one 2-second segment gap at most.

**Q14: How do you implement the "Hype Chat" feature where paid messages are highlighted in chat?**
A: Hype Chat allows viewers to pay $1.99-$500 to have a message pinned prominently in chat for 30 seconds to 5 minutes (duration scales with price). Architecture: (1) Payment via Stripe, processed server-side; on payment success, publish `hype-chat-event` to Kafka. (2) Message stored in `hype_chat_messages` table with `display_until` timestamp. (3) On chat delivery: the hype chat message is delivered as a special IRC message type with `@msg-id=highlighted-message` tag. Client renders it in a prominent card UI above regular chat. (4) After `display_until`, the client removes the pinned card (client-side TTL, no server message needed). (5) Revenue split: Twitch ~30%, Creator ~70% (per announced policy).

**Q15: How would you scale the ingest system if Twitch needed to support 500,000 simultaneous live streams?**
A: Current capacity: 50,000 streams. 10× scale requires: (1) Ingest PoPs: scale from 150 to 1,500 locations (or increase capacity per PoP; 10× more RTMP server capacity per PoP). (2) Transcoding: from ~12,500 T4 GPUs to 125,000 T4 GPUs (~$65,000/hr GPU cost). This motivates moving to cheaper transcoding — AV1 software encode is slow but AV1 ASICs (e.g., Intel Arc GPU or custom silicon) could reduce cost. Netflix-style VMAF optimization could reduce bitrate (fewer bits needed → same GPU transcodes more streams). (3) Storage: S3 live segment writes: 500,000 streams × 8 quality × 1 write/2s = 2 million PutObject/sec to S3. S3's request rate limit requires prefix sharding (different S3 bucket prefixes per stream range). (4) CDN: egress scales linearly with viewers — 10× streams doesn't mean 10× viewers; viewers distribute across streams. Total viewer capacity depends on CDN, not ingest count.

---

## 12. References & Further Reading

1. **Twitch Engineering Blog** — https://blog.twitch.tv/en/tags/engineering/
2. **Twitch Helix API Documentation** — https://dev.twitch.tv/docs/api/
3. **IRC Protocol** — RFC 1459. https://datatracker.ietf.org/doc/html/rfc1459
4. **HLS Specification** — Apple. RFC 8216. https://datatracker.ietf.org/doc/html/rfc8216
5. **Low-Latency HLS** — Apple. "Enabling Low-Latency HLS." WWDC 2019. https://developer.apple.com/videos/play/wwdc2019/502/
6. **FFmpeg NVENC** — https://developer.nvidia.com/nvidia-video-codec-sdk; https://trac.ffmpeg.org/wiki/HWAccelIntro
7. **WebRTC** — Holmberg, C., Hakansson, H., Eriksson, G. "Web Real-Time Communication." RFC 8825. https://datatracker.ietf.org/doc/html/rfc8825
8. **Selective Forwarding Unit (SFU) Architecture** — Pion WebRTC. https://github.com/pion/webrtc
9. **Twitch Open Source** — Vitess usage by Twitch. https://github.com/vitessio/vitess
10. **Redis Pub/Sub** — https://redis.io/docs/manual/pubsub/
11. **Cassandra TimeWindowCompactionStrategy** — DataStax Docs. https://cassandra.apache.org/doc/latest/cassandra/operating/compaction/twcs.html
12. **AutoMod / Content Moderation** — Twitch. https://help.twitch.tv/s/article/how-to-use-automod
13. **NVIDIA NVENC** — https://developer.nvidia.com/nvidia-video-codec-sdk
14. **Akamai Media Delivery** — https://www.akamai.com/solutions/media-delivery
15. **Fastly Streaming** — https://www.fastly.com/products/streaming-media
