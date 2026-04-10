# System Design: Live Comments (YouTube/Twitch Live Chat)

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Real-time comment delivery** — Comments posted in a live stream's chat delivered to all viewers watching at that moment within 1-2 seconds.
2. **High-volume fan-out** — A single popular stream may have 500,000+ simultaneous viewers; each comment must be fanned out to all of them.
3. **Comment moderation** — Chat owners, moderators, and automated tools can delete comments, ban users, and time out users.
4. **Hate speech filtering** — Automated ML-based filtering of hate speech, spam, and prohibited content before or shortly after delivery.
5. **Pinned messages** — Moderators can pin a single message that appears at the top of the chat for all viewers.
6. **Slow mode throttling** — Per-channel configurable minimum interval between comments from a single non-privileged user (e.g., 30 seconds).
7. **Paid highlights / Super Chats** — Paid messages highlighted with a color-coded banner; persist at the top for a duration proportional to payment amount.
8. **Subscriber-only mode** — Chat restricted to channel subscribers only; non-subscribers see chat but cannot post.
9. **Emotes / emoji** — Channel-specific subscriber emotes; global platform emotes; third-party emote integrations (Twitch: BTTV, FFZ).
10. **Comment history** — Post-stream, the chat replay is available (synchronized with VOD timeline). During a stream, recent ~150 comments visible on scroll.
11. **User mentions** — @username mentions highlight in chat for the mentioned user.
12. **Badges and flair** — Moderator badge, subscriber badge, channel points badge rendered next to usernames.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Availability | 99.99% (≤ 52 min/year; stream outages directly impact revenue) |
| Comment delivery latency (p99) | < 2 s from post to display on viewer screens |
| Fan-out throughput | Support 100 K messages/sec across all streams; individual stream peak 500 K+ viewers |
| Write throughput | 500 K comment writes/sec across all live streams |
| Moderation action latency | < 500 ms for manual mod action; < 2 s for AutoMod deletion |
| Consistency | Eventual; viewers see same comment stream (minor ordering differences acceptable) |
| Scale | 30 M concurrent viewers (Twitch peak ~7 M; YouTube Live peak ~10 M; combined/future scale ~30 M) |
| Storage | All chat messages stored; 5 years retention for replay and analytics |
| Security | Rate limiting, bot mitigation, CAPTCHA gates |

### Out of Scope

- Video stream ingest and delivery (CDN video streaming is a separate system).
- Live polling and prediction features.
- Channel points redemption system.
- Ad-break coordination.
- Mobile app push notifications for stream events.
- Third-party emote CDN integration details.

---

## 2. Users & Scale

### User Types

| Type | Description |
|---|---|
| **Viewer (non-subscriber)** | Watches stream; can comment if not restricted; subject to slow mode |
| **Subscriber** | Paid subscriber; bypass slow mode; access to subscriber-only mode; subscriber emotes |
| **Moderator** | Appointed by streamer; can delete comments, ban, timeout users |
| **Streamer / Channel Owner** | Full moderation powers; can configure chat settings |
| **Verified** | Platform-verified notable account; special badge |
| **Bot Account** | Automated comment posters (moderator bots, chatbots, interactive bots); higher rate limits |

### Traffic Estimates

**Assumptions:**
- 30 M concurrent viewers across all live streams.
- Average viewers per stream: 500 (most streams are small; a few are very large). Total active streams: 30 M / 500 = 60,000 concurrent live streams.
- Comment rate: viewers send roughly 1 comment per 5 minutes on average (mix of lurkers and active chatters). Active chatters: ~10% of viewers send 1 comment/minute; 90% lurk. Effective rate: 0.1 × 30 M users × (1/60) = 50 K comments/sec.
- Large stream (Pokimane, MKBHD launch event): 500 K viewers. At 5% active chatters × 1 comment/minute = 500,000 × 0.05 / 60 = ~417 comments/sec for one channel. At 500 K viewers this fan-outs to 500 K deliveries for each comment.
- Total fan-out deliveries: 50 K comments/sec × avg 500 viewers/stream = 25 M deliveries/sec.
- Peak streams (top 10 streams): 500 K viewers each × 417 comments/sec × 500 K fan-out = a single large stream contributes 208 M deliveries/sec at peak. This is the hardest scaling challenge.
- Moderation actions: 0.5% of comments require deletion = 250 actions/sec.
- Peak-to-average ratio: 5x (new game launch, charity events, political commentary).

| Metric | Calculation | Result |
|---|---|---|
| Concurrent viewers | Given | 30 M |
| Active live streams | 30 M / 500 avg viewers | 60,000 streams |
| Comment writes/sec (avg) | 30 M × 10% active × 1/60 | ~50,000 /s |
| Comment writes/sec (peak) | 50 K × 5 | ~250,000 /s |
| Fan-out deliveries/sec (avg) | 50 K × 500 avg viewers | 25 M deliveries/s |
| Fan-out deliveries/sec (peak) | 250 K × 500 | 125 M deliveries/s |
| Large stream fan-out events/sec | 417 comments/s × 500 K viewers | ~208 M for single large stream |
| Moderation actions/sec | 50 K × 0.5% | ~250 /s |
| Super Chats/sec | 1% of comments are paid highlights | ~500 /s |
| Stored comments/day | 50 K × 86,400 | ~4.3 B/day |

### Latency Requirements

| Operation | P50 | P99 |
|---|---|---|
| Comment post to display (online viewers) | 500 ms | 2 s |
| Moderation deletion propagation | 100 ms | 500 ms |
| Slow mode enforcement check | 5 ms | 20 ms |
| Hate speech filter (async) | 200 ms | 1 s |
| Pinned message update | 100 ms | 500 ms |
| Super Chat highlight | 200 ms | 1 s |
| Comment history load (last 150) | 50 ms | 200 ms |

### Storage Estimates

**Assumptions:**
- Average comment: 150 bytes (text + metadata — user_id, timestamp, badges, emote references).
- 5-year retention for replay.
- Super Chat metadata: additional 500 bytes/Super Chat (payment details, display duration).

| Data Type | Calculation | Volume |
|---|---|---|
| Comment rows (1 year) | 50 K/s × 86,400 × 365 × 150 bytes | ~243 TB/year |
| Comment rows (5 years) | × 5 | ~1.2 PB |
| Super Chat records (1 year) | 500/s × 86,400 × 365 × 500 bytes | ~7.9 TB/year |
| User ban/timeout records | ~10 M events/year × 200 bytes | ~2 GB/year |
| Pinned message state | 60 K streams × 2 KB | ~120 MB (ephemeral) |
| Stream session metadata | 100 K streams/day × 1 KB | ~100 MB/day |
| Chat replay index | 1 PB × 10% index overhead | ~120 TB (for 5 yr) |

### Bandwidth Estimates

| Direction | Calculation | Throughput |
|---|---|---|
| Inbound comment writes (avg) | 50 K/s × 150 bytes | ~7.5 MB/s |
| Outbound chat delivery (avg) | 25 M deliveries/s × 150 bytes | ~3.75 GB/s |
| Outbound chat delivery (peak) | 125 M × 150 bytes | ~18.75 GB/s |
| Large stream peak outbound | 208 M × 150 bytes | ~31.2 GB/s (single stream!) |
| Moderation events (deletions, bans) | 250/s × 100 bytes | ~25 KB/s (negligible) |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                              Client Layer                                                │
│   Web Browser (YouTube)    Twitch Web Client    Mobile App    Smart TV App              │
│                                                                                          │
│   Chat Panel:                           Comment Ingestion:                              │
│   - WebSocket / SSE (receive)           - HTTP POST /comment                            │
│   - Long-poll fallback                  - WebSocket send (Twitch IRC-like protocol)      │
└──────────┬─────────────────────────┬───────────────────────────────────────────────────┘
           │ (WebSocket/SSE receive) │ (HTTP POST / WS send)
           │                         │
┌──────────▼─────────────────────────▼────────────────────────────────────────────────────┐
│                        Edge / CDN / Load Balancer                                        │
│   Cloudflare for anti-bot, WAF, DDoS      AWS ALB per region                           │
│   Sticky routing for WebSocket (user_id)   Rate limit enforcement at edge               │
└──────────────────────┬──────────────────────────────┬──────────────────────────────────┘
                       │                              │
            ┌──────────▼──────────┐       ┌──────────▼──────────────────────┐
            │  Chat Receive Svc   │       │   Comment Ingest Service        │
            │  (WebSocket/SSE)    │       │   - Auth validation              │
            │  ~2,000 servers     │       │   - Slow mode check (Redis)      │
            │  15K connections ea │       │   - Subscriber-only check        │
            │                     │       │   - Write to DB + Kafka          │
            └──────────┬──────────┘       └──────────────────┬───────────────┘
                       │                                     │
                       │                  ┌──────────────────▼───────────────┐
                       │                  │         Kafka                    │
                       │                  │  Topics: raw_comments,           │
                       │                  │  moderation_actions,             │
                       │                  │  super_chats, stream_events      │
                       │                  └──────────────────┬───────────────┘
                       │                                     │
                       │          ┌──────────────────────────┼──────────────────────────────┐
                       │          │                          │                              │
                       │  ┌───────▼──────────┐  ┌───────────▼────────────┐  ┌─────────────▼──────────────┐
                       │  │  Chat Fan-Out    │  │  AutoMod / Hate Speech │  │  Chat History Service      │
                       │  │  Service         │  │  Filtering Service     │  │  (write path: Bigtable     │
                       │  │  (core fan-out   │  │  - Bloom filter        │  │   / Cassandra)             │
                       │  │   engine)        │  │  - ML classifier       │  │  (read path: recent cache) │
                       │  └───────┬──────────┘  │  - Word block list     │  └────────────────────────────┘
                       │          │             └────────────────────────┘
                       └──────────┤
                                  │ (gRPC batch push to Chat Receive Svcs)
                                  │
                  ┌───────────────┼──────────────────┐
                  │               │                  │
        ┌─────────▼──────┐  ┌────▼───────────┐  ┌──▼─────────────────────────────┐
        │ Stream Registry │  │ Viewer Set     │  │ Slow Mode / Rate Limit Store   │
        │ (Redis)         │  │ (Redis per     │  │ (Redis: per-user per-stream    │
        │ stream metadata │  │  stream)       │  │  last-comment timestamp)       │
        │ pinned msg      │  │ who's watching │  └────────────────────────────────┘
        └─────────────────┘  └────────────────┘
                                                 ┌──────────────────────────────────┐
                                                 │  Moderation Service              │
                                                 │  - Manual mod actions API        │
                                                 │  - Ban/timeout user store        │
                                                 │  - Delete comment propagation    │
                                                 │  (PostgreSQL + Kafka events)     │
                                                 └──────────────────────────────────┘
                                                 ┌──────────────────────────────────┐
                                                 │  Super Chat Service              │
                                                 │  - Payment integration           │
                                                 │  - Priority delivery             │
                                                 │  - Persistence timer             │
                                                 └──────────────────────────────────┘
```

**Component Roles:**

- **Chat Receive Service (WebSocket/SSE servers)**: Maintains persistent connections for viewers receiving chat. Each viewer connects once and receives pushed comment events. Supports WebSocket (full-duplex) and SSE (event-stream, simpler fallback for smart TVs / older clients). No business logic — pure fan-in/fan-out gateway.
- **Comment Ingest Service**: Stateless HTTP service that accepts new comments. Validates auth, checks slow mode (Redis TTL per user+stream), checks subscriber-only flag, writes comment to DB and Kafka. Returns HTTP 200 + comment_id to sender.
- **Kafka (Event Bus)**: `raw_comments` topic: all validated comments. `moderation_actions` topic: delete/ban/timeout events. `super_chats` topic: paid highlights. `stream_events` topic: stream start/stop, mode changes (slow mode toggle, subscriber-only toggle).
- **Chat Fan-Out Service**: The performance-critical core. Reads comments from Kafka; reads the stream's viewer set from Redis (list of chat receive server addresses); sends batch gRPC to each receive server; servers push to connected viewers. Tier-1 component with dedicated hardware.
- **AutoMod / Hate Speech Filtering Service**: Async Kafka consumer. Two-stage: (1) Bloom filter / word blocklist for fast rejection (< 1 ms, catches 80% of violations). (2) ML transformer model for nuanced classification (~200 ms per message). If violation detected, publishes to `moderation_actions` Kafka topic → propagates deletion to viewers.
- **Chat History Service**: Writes all validated comments to Cassandra/Bigtable (per-stream, time-ordered). Provides recent history (last 150 comments) for newly connecting viewers and post-stream VOD replay.
- **Stream Registry (Redis)**: Per-stream metadata: stream_id → {channel_id, is_live, slow_mode_seconds, is_subscriber_only, pinned_message_id, viewer_count_approx}. Central source of truth for stream state.
- **Viewer Set (Redis)**: Per-stream set of chat receive server addresses (or server IDs) that have at least one viewer connected to that stream. Updated on viewer connect/disconnect. Used by Fan-Out Service to route comment events.
- **Slow Mode Store (Redis)**: `last_comment:{user_id}:{stream_id}` with TTL = slow_mode_seconds. SET NX (only set if key doesn't exist) on comment submit — if key exists, user is rate-limited.
- **Moderation Service**: Handles manual mod actions. Writes bans/timeouts to PostgreSQL. On comment deletion, publishes to Kafka → Fan-Out propagates `DELETE_COMMENT` event to all viewers (clients remove message from UI). Ban list cached in Redis for fast pre-send checks.
- **Super Chat Service**: Handles paid comment highlights. Integrates with payment processor. On payment confirmation, assigns display tier (color, duration), writes to DB, publishes to `super_chats` Kafka topic. Fan-Out delivers with priority over regular comments.

**Primary Use-Case Data Flow (viewer posts a comment):**

1. Viewer types comment, presses Enter.
2. Client sends `POST /v1/streams/{stream_id}/comments` with `{text, user_token}`.
3. Comment Ingest Service: validates JWT token, checks ban list (Redis SISMEMBER `banned:{stream_id}`), checks slow mode (Redis SET NX `last_comment:{user_id}:{stream_id}` with TTL), checks subscriber-only (stream registry flag + user subscription status). All checks: < 5 ms total (Redis sub-ms lookups).
4. Validation passes: assigns comment_id (Snowflake), writes row to Cassandra (async, fire-and-forget for latency). Publishes to Kafka `raw_comments` topic. Returns HTTP 200 with comment_id to the poster. Poster's client displays the comment immediately (optimistic display).
5. Kafka consumer (AutoMod): reads comment. Runs bloom filter (< 1ms) → if clean, publishes to `approved_comments` topic. If flagged by ML model → publishes to `moderation_actions` topic for deletion.
6. Fan-Out Service consumes `approved_comments` topic. Looks up stream's viewer set in Redis (`stream_servers:{stream_id}` — a Set of receive server IDs that have viewers for this stream). Groups by server (already grouped — each entry is a server ID). Sends batched gRPC push to each receive server.
7. Each receive server pushes `COMMENT_CREATE` event (JSON) to all its connected viewers watching that stream.
8. Viewers' chat panels append the new comment.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Streams (live sessions)
-- ============================================================
CREATE TABLE streams (
    stream_id           BIGINT      PRIMARY KEY,  -- Snowflake ID
    channel_id          BIGINT      NOT NULL,
    title               TEXT,
    started_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    ended_at            TIMESTAMPTZ,              -- NULL if live
    is_live             BOOLEAN     NOT NULL DEFAULT true,
    game_id             BIGINT,
    language            VARCHAR(10),
    INDEX idx_stream_channel (channel_id, started_at DESC)
) ENGINE=InnoDB;  -- PostgreSQL or MySQL; low write volume

-- ============================================================
-- Stream Chat Config (per stream, mutable during stream)
-- ============================================================
CREATE TABLE stream_chat_config (
    stream_id               BIGINT      PRIMARY KEY,
    slow_mode_seconds       INT         NOT NULL DEFAULT 0,   -- 0 = disabled
    subscriber_only         BOOLEAN     NOT NULL DEFAULT false,
    emote_only              BOOLEAN     NOT NULL DEFAULT false,
    follower_only_minutes   INT         NOT NULL DEFAULT 0,   -- follow age requirement
    unique_chat_mode        BOOLEAN     NOT NULL DEFAULT false, -- no repeated messages
    pinned_comment_id       BIGINT,
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ============================================================
-- Comments
-- PRIMARY STORE: Cassandra / Apache Bigtable (time-series)
-- Schema below in CQL for Cassandra
-- ============================================================
CREATE TABLE comments (
    stream_id       BIGINT,
    bucket          INT,        -- floor(comment_id_epoch_ms / (1000*3600)) = hourly bucket
    comment_id      BIGINT,     -- Snowflake (time-ordered)
    user_id         BIGINT,
    username        TEXT,       -- denormalized for fast display (avoid join)
    display_name    TEXT,       -- channel-specific display name if set
    user_badges     TEXT,       -- JSON: ["moderator", "subscriber/12", "premium"]
    user_color      VARCHAR(7), -- HEX color string chosen by user (e.g. "#FF4500")
    text            TEXT,
    emotes          TEXT,       -- JSON: [{id, name, positions: [[start,end]]}]
    is_deleted      BOOLEAN     DEFAULT false,
    deleted_by      BIGINT,
    deleted_at      BIGINT,
    comment_type    TEXT,       -- 'chat', 'super_chat', 'subscription', 'system'
    PRIMARY KEY ((stream_id, bucket), comment_id)
) WITH CLUSTERING ORDER BY (comment_id DESC)
  AND default_time_to_live = 157680000;  -- 5 years in seconds

-- ============================================================
-- Super Chats (paid highlights) — stored in both Cassandra (as comment_type='super_chat')
-- and in a separate relational table for billing/reporting
-- ============================================================
CREATE TABLE super_chats (
    super_chat_id       BIGINT      PRIMARY KEY,
    comment_id          BIGINT      NOT NULL UNIQUE,
    stream_id           BIGINT      NOT NULL,
    user_id             BIGINT      NOT NULL,
    amount_cents        INT         NOT NULL,   -- in USD cents or local currency
    currency            VARCHAR(3)  NOT NULL,
    display_tier        SMALLINT    NOT NULL,   -- 1-7 (color tier based on amount)
    display_duration_s  INT         NOT NULL,   -- seconds to stay pinned in Super Chat bar
    message             TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    payment_reference   VARCHAR(128) NOT NULL,  -- payment processor transaction ID
    INDEX idx_sc_stream (stream_id, created_at DESC)
) ENGINE=InnoDB;

-- ============================================================
-- Bans and Timeouts
-- ============================================================
CREATE TABLE user_bans (
    ban_id          BIGINT      PRIMARY KEY,
    stream_id       BIGINT,     -- NULL = channel-wide ban; non-null = stream-specific
    channel_id      BIGINT      NOT NULL,
    user_id         BIGINT      NOT NULL,
    banned_by       BIGINT      NOT NULL,
    ban_type        ENUM('ban', 'timeout') NOT NULL,
    timeout_until   TIMESTAMPTZ,  -- NULL for permanent ban; timestamp for timeout
    reason          TEXT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE KEY uq_user_channel_ban (user_id, channel_id, ban_type),
    INDEX idx_ban_channel_user (channel_id, user_id)
) ENGINE=InnoDB;

-- ============================================================
-- Slow Mode State (Redis — not SQL)
-- Key:   slow_mode:{stream_id}:{user_id}
-- Type:  String (any value)
-- TTL:   slow_mode_seconds (set on stream_chat_config)
-- Logic: SET NX with TTL; if key exists → user is rate-limited
-- ============================================================

-- ============================================================
-- Viewer Set (Redis — not SQL)
-- Key:   stream_viewers:{stream_id}
-- Type:  Set
-- Value: chat receive server IDs (e.g., "chat-recv-001")
-- TTL:   120 seconds (renewed by receive servers every 60 s via heartbeat)
-- Logic: SADD on first viewer connect; SREM only when last viewer leaves
-- ============================================================

-- ============================================================
-- Ban Cache (Redis — not SQL)
-- Key:   ban_cache:{channel_id}
-- Type:  Set of user_ids (banned)
-- TTL:   300 seconds; refreshed on ban/unban events
-- ============================================================

-- ============================================================
-- Recent Comments Cache (Redis — not SQL)
-- Key:   recent_comments:{stream_id}
-- Type:  List (LPUSH on new comment, LTRIM to keep last 150)
-- Value: serialized comment JSON
-- TTL:   None while stream is live; expire 1 hour after stream ends
-- ============================================================

-- ============================================================
-- Unique Chat Mode Dedup (Redis — not SQL)
-- Key:   recent_texts:{stream_id}
-- Type:  Set (normalized message text, last 30 seconds)
-- TTL:   30 seconds (rolling window)
-- Logic: SISMEMBER check; if exists = duplicate message, reject
-- ============================================================

-- ============================================================
-- AutoMod Word Blocklist (Redis — not SQL)
-- Key:   blocklist:{channel_id}
-- Type:  Bloom Filter (RedisBloom module) seeded with banned terms
-- Logic: BF.EXISTS check on each comment word; < 1 ms per check
-- ============================================================
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| **Apache Cassandra / Bigtable** | Comment storage | Time-series write-heavy workload; wide-row model (stream_id + bucket = partition); linear horizontal scale; 5-year TTL support natively; handles 250 K writes/sec across cluster | No secondary index on user_id (needs separate table for per-user history); eventual consistency | **Selected** for comment storage |
| **Google Bigtable** | Alternative to Cassandra | Managed; auto-scaling; sub-ms latency; HBase API | GCP-specific; vendor lock-in; cost | **Alternative** (Twitch uses Cassandra; YouTube likely uses Bigtable/Spanner) |
| **PostgreSQL / MySQL** | Streams, users, bans, Super Chats | ACID for payment records; complex queries for moderation reporting | Doesn't scale to 250 K writes/sec for comments | **Selected** for structured relational data (streams, bans, payments) |
| **Redis Cluster** | Slow mode, viewer sets, recent cache, ban cache | Sub-ms latency; TTL support; Sorted Sets for rate limiting; Set operations for viewer tracking | Memory-only; not durable source of truth | **Selected** for all ephemeral and hot-path state |
| **Kafka** | Event bus for comments and mod actions | Durable event log; fan-out to multiple consumers; replay capability; compaction for stream state | Operational complexity; Zookeeper/KRaft overhead | **Selected** for event bus |
| **MySQL with write-through cache** | Comment storage | Familiar; ACID | Cannot sustain 250 K writes/sec without extreme sharding; B-tree index contention under heavy write | Not selected for hot comment path |

---

## 5. API Design

Authentication: OAuth 2.0 / platform JWT. Comments sent via HTTP POST or WebSocket. Chat received via WebSocket or SSE. Rate limits enforced at edge (Cloudflare) and Comment Ingest Service.

### REST Endpoints

#### Comment Ingestion

```
POST /v1/streams/{stream_id}/comments
Authorization: Bearer <user_token>
Request:
{
  "text": "PogChamp this is amazing!",
  "reply_to_comment_id": null,   // optional: reply to another comment
  "client_nonce": "uuid"         // idempotency key; prevents duplicate on retry
}
Response 200:
{
  "comment_id": "1234567890",    // Snowflake ID
  "stream_id": "...",
  "user_id": "...",
  "text": "PogChamp this is amazing!",
  "badges": ["subscriber/6"],
  "user_color": "#FF4500",
  "emotes": [{"id": "305954156", "name": "PogChamp", "positions": [[0, 8]]}],
  "ts": 1712620800123,           // server timestamp ms
  "comment_type": "chat"
}
Error Responses:
  429: { "error": "slow_mode", "retry_after": 28 }  // slow mode throttle
  403: { "error": "subscriber_only" }
  403: { "error": "banned" }
  403: { "error": "timeout", "expires_at": "2026-04-09T10:30:00Z" }
  400: { "error": "emote_only" }   // emote-only mode, no plain text
Rate Limit: Hard edge limit 10 comments/5s per user (beyond slow mode); global 50 req/s burst per user_id
```

#### Comment History (new viewer joining / VOD replay)

```
GET /v1/streams/{stream_id}/comments
Query Params:
  before_ts=<epoch_ms>    // comments before this timestamp (for "load more" / history)
  after_ts=<epoch_ms>     // comments after this timestamp (for VOD replay sync)
  limit=150 (max 500 for VOD replay, max 150 for live)
  include_deleted=false   // moderators can request deleted comments (with auth)
Response 200:
{
  "comments": [
    {
      "comment_id": "...",
      "user_id": "...",
      "username": "gamer123",
      "display_name": "Gamer 123",
      "badges": ["moderator"],
      "user_color": "#1E90FF",
      "text": "LUL that was close",
      "emotes": [...],
      "ts": 1712620800000,
      "comment_type": "chat",
      "is_deleted": false
    }
  ],
  "has_more": true,
  "next_cursor": "1712620700000"  // epoch ms timestamp cursor
}
Rate Limit: 100 requests/user/minute (for VOD scrubbing)
Notes: For live stream, serve from Redis recent_comments cache (last 150). For VOD, query Cassandra.
```

#### Moderation Actions

```
DELETE /v1/streams/{stream_id}/comments/{comment_id}
Authorization: Bearer <mod_token>  (requires MODERATOR or EDITOR role for channel)
Response 204
Notes: Publishes deletion event to Kafka → fans out to all viewers within 500 ms

POST /v1/channels/{channel_id}/bans
Authorization: Bearer <mod_token>
Request: { "user_id": "...", "ban_type": "timeout", "duration_seconds": 600, "reason": "spam" }
Response 201: { "ban_id": "...", "expires_at": "..." }

DELETE /v1/channels/{channel_id}/bans/{user_id}
Authorization: Bearer <mod_token>
Response 204
Notes: Unban — removes from ban list; cache invalidated

POST /v1/streams/{stream_id}/chat/pin
Authorization: Bearer <mod_token>
Request: { "comment_id": "..." }
Response 200: { "pinned_comment": { comment object } }
Notes: Updates stream_chat_config.pinned_comment_id; publishes PINNED_MESSAGE event to Kafka

DELETE /v1/streams/{stream_id}/chat/pin
Authorization: Bearer <mod_token>
Response 204
```

#### Stream Chat Config

```
PATCH /v1/streams/{stream_id}/chat/config
Authorization: Bearer <channel_owner_or_editor_token>
Request: {
  "slow_mode_seconds": 30,    // 0 = disable
  "subscriber_only": true,
  "emote_only": false,
  "follower_only_minutes": 10
}
Response 200: { stream_chat_config object }
Notes: Updates PostgreSQL; publishes STREAM_CONFIG_UPDATE event to Kafka;
       all Chat Receive servers update their in-memory stream config cache
```

#### WebSocket Protocol (Chat Receive)

```
WSS /v1/chat/{stream_id}
Authorization: Bearer <token> in query param (or anonymous for viewer-only)

// Server → Client (JSON events):
{
  "type": "COMMENT_CREATE",
  "data": { comment object }
}
{
  "type": "COMMENT_DELETE",
  "data": { "comment_id": "...", "deleted_by": "...", "reason": "spam" }
}
{
  "type": "USER_BANNED",
  "data": { "user_id": "...", "ban_type": "timeout", "expires_at": "..." }
}
{
  "type": "PINNED_MESSAGE",
  "data": { "comment": { comment object } | null }  // null = unpin
}
{
  "type": "SLOW_MODE_UPDATE",
  "data": { "slow_mode_seconds": 30 }
}
{
  "type": "STREAM_END",
  "data": { "stream_id": "..." }
}
{
  "type": "SUPER_CHAT",
  "data": { comment object with amount_cents, display_tier, display_duration_s }
}
// Client → Server:
{ "type": "PING" }
// Server: { "type": "PONG" }

// SSE endpoint (fallback):
GET /v1/chat/{stream_id}/events
Accept: text/event-stream
// Server sends: data: {JSON event}\n\n
// Client can only receive; to post comments, uses separate HTTP endpoint
```

---

## 6. Deep Dive: Core Components

### 6.1 High-Volume Real-Time Fan-Out

**Problem it solves:**
A single comment in a 500,000-viewer stream must be delivered to all 500,000 viewers within 2 seconds. At 417 comments/minute from such a stream, this means 417/60 = ~7 comments/sec × 500,000 viewers = 3.5 M WebSocket writes per second from a single stream alone. This is the defining engineering challenge of live chat.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Direct fan-out (single service)** | One service holds all viewer connections; fans out directly | Simple | Cannot hold 30 M connections in one service; single point of failure |
| **Pub/Sub (Redis Pub/Sub per stream)** | Chat Receive servers subscribe to `stream:{stream_id}` channel; all subscribers receive broadcast | Simple; low latency | Redis Pub/Sub limited in subscribers per channel; no persistence; memory per subscription |
| **WebSocket servers + Kafka fan-out** | Ingest → Kafka; fan-out workers push to Chat Receive servers; Chat Receive servers push to clients | Durable; scalable; decoupled; buffering under load | Kafka adds ~50-100ms latency; complex routing |
| **Hierarchical fan-out (tree broadcast)** | Comments fan-out through a tree of relay nodes; each node fans out to its subtree | Theoretical scale | High complexity; cascaded failures multiply; extra hops add latency |
| **Pull model (client polling)** | Clients poll for new comments every second | Simple; stateless server | 30 M × 1 poll/sec = 30 M req/sec; untenable; 1 second minimum latency |
| **CDN push (server push via CDN)** | Comment pushed to CDN as a stream; clients receive via CDN HTTP/2 push or chunked response | Leverages CDN infrastructure | HTTP/2 push deprecated; chunked response not true push; complex state management |
| **Gossip-based broadcast** | Chat Receive servers gossip comments to each other | No central coordinator | N² gossip messages; uncontrolled latency; ordering issues |

**Selected Approach: Kafka-based ingest + Sharded Fan-Out Workers + Chat Receive Server Tier with stream-based connection grouping**

**Implementation Detail:**

*Architecture overview:*
The key insight: viewers of the same stream must be co-located or efficiently addressed. We use a two-tier approach:

1. **Chat Receive Server (CRS) tier**: Each CRS holds WebSocket/SSE connections for viewers. Viewers are assigned to a CRS based on `hash(user_id) % num_CRS_servers` (consistent hash) for sticky routing. Each CRS maintains a local map: `stream_id → set of local connections for viewers of that stream`.

2. **Viewer Set Registry (Redis)**: When a CRS has at least one viewer for stream S, it registers: `SADD stream_servers:{stream_id} {crs_id}`. TTL refreshed every 60s. This set contains all CRS servers that have viewers for stream S.

3. **Fan-Out Service**: Kafka consumer. For each comment event:
   a. Reads `stream_servers:{stream_id}` from Redis (the set of CRS server IDs with viewers for this stream).
   b. For large streams (> 1,000 viewers): issues one gRPC broadcast call to each CRS in the set: `BroadcastToStream(stream_id, comment_payload)`. Each CRS fans out locally to its connected viewers.
   c. For small streams (< 100 viewers): can use the same mechanism, but with fewer CRS servers involved.

4. **CRS broadcast execution**: CRS receives `BroadcastToStream(stream_id, payload)` gRPC call. Iterates its local `stream_connections[stream_id]` (Go map, O(N) iteration). Writes payload to each viewer's socket (non-blocking, using goroutines). At 500 K viewers × assuming 200 CRS servers → 2,500 viewers/CRS for this stream on average. CRS writes 2,500 WebSocket frames: at 1 μs/write (non-blocking epoll) = 2.5 ms per CRS server per comment. Well within 2-second SLA.

*Numbers for 500K-viewer stream:*
- 500 K viewers / 15 K connections per CRS = ~34 CRS servers handle this stream.
- Fan-Out Service: 1 Redis SMEMBERS call (returns 34 server IDs) + 34 gRPC calls (parallel, each ~5 ms) = ~6 ms for fan-out dispatch.
- Each CRS: ~14,700 socket writes (500K / 34) = ~15 ms at 1 μs/write.
- Total delivery latency: Kafka consume (5 ms) + Redis lookup (1 ms) + gRPC dispatch (5 ms) + CRS writes (15 ms) = ~26 ms median. p99 (network variability): ~200 ms. Well within 2-second SLA.

*Handling stream chat rate bursts:*
- A viral moment (streamer wins championship) may trigger 10× normal comment rate. Kafka absorbs the write burst. Fan-Out workers auto-scale (consumer group adds instances). CRS has async write buffers per connection — if a client's TCP buffer is full (slow consumer), back-pressure handled by dropping the oldest comments (ring buffer per connection) rather than blocking the broadcast. Clients re-sync from recent_comments cache if they detect they've missed comments (comment_id sequence gap).

*Large stream viewer distribution:*
- A single stream at 500 K viewers connects to multiple CRS servers (load balancer distributes new connections round-robin or by current load). In practice, viewers from the same stream connect to all CRS servers roughly uniformly (IP-hash sticky routing for reconnects, but first connection goes to least-loaded CRS). This naturally distributes the 500 K connections across ~34 CRS servers.

**Interviewer Q&As:**

1. **Q: How do you handle comment ordering across 500,000 viewers when comments arrive at slightly different times?**
   A: Perfect ordering is neither required nor achievable at this scale, but approximate ordering is. Comments are assigned a Snowflake ID (encoding the server timestamp in the high bits) at the Comment Ingest Service. The Fan-Out Service delivers comments in the order they're consumed from Kafka (Kafka preserves partition order; messages for a stream go to the same partition). Viewers see comments in the Kafka-delivery order, which approximates posting order with < 5 ms variance from server timestamp to Kafka publish. Client-side: the chat UI doesn't re-sort received comments — they're displayed in receive order. Two viewers may see two nearly-simultaneous comments in slightly different orders if they're on different network paths with different latencies. This 50-100 ms ordering difference is imperceptible and acceptable in live chat UX. For the chat replay (VOD), comments are displayed from Cassandra in Snowflake (time) order — perfectly ordered.

2. **Q: What happens when a Chat Receive Server crashes? How do viewers reconnect without missing comments?**
   A: (1) CRS crashes → ~15 K viewers lose their WebSocket connection. Client-side: WebSocket disconnect detected immediately (TCP RST). Clients implement reconnect with exponential backoff (1s → 2s → 4s, max 10s, with ±30% jitter). (2) On reconnect: client connects to a new CRS (load balancer selects a healthy one). Client sends its last received comment_id (or timestamp) to the Chat Receive WebSocket endpoint: `WSS /v1/chat/{stream_id}?since={last_comment_id}`. (3) CRS fetches comments from `recent_comments:{stream_id}` Redis cache (last 150 comments) and sends any comments the client missed. (4) Crashed CRS is replaced by ASG within 60 seconds. (5) Fan-Out Service detects CRS failure (gRPC health check / failed gRPC call) and removes it from active routing. Viewer set registry TTL (120s) expires the CRS entry if it doesn't renew its heartbeat. Comments during the gap are re-delivered from Redis cache on reconnect. Zero comment loss for client-side gap < 150 comments (the Redis cache window).

3. **Q: How does the system handle a sudden spike from 10,000 to 500,000 viewers in 60 seconds (viral moment)?**
   A: This is the stream equivalent of a thundering herd. Mitigations: (1) **CRS scaling**: CRS tier uses an ASG with aggressive scale-out policy (scale on connections/server > 70%). New CRS instances can be provisioned in 60-120 seconds. But the initial spike sees existing CRS servers absorb connections up to their limit (15K). If all CRS servers are full, new connection requests are queued at the load balancer (connection pending) or rejected with a 503. (2) **Pre-warming**: For known large events (official esports tournaments, watched-in-advance streams), the system pre-scales CRS servers based on the stream's subscriber count or stream category. (3) **Connection acceptance rate limiting**: Limit new WebSocket connections to 50 K/second across all CRS servers, smoothing the spike. Clients with "connection pending" wait ≤ 2 seconds before the CRS opens the socket. (4) **Fan-Out adaptation**: as viewer count grows, the Fan-Out Service detects more CRS servers in `stream_servers:{stream_id}` set. Fan-out automatically scales as new CRS servers register. The Redis set update is near-real-time.

4. **Q: How do you design the fan-out for a stream with 5 million viewers (a major world event like a championship final)?**
   A: At 5 M viewers: (1) 5 M / 15 K per CRS = 334 CRS servers needed. (2) Fan-Out: Redis SMEMBERS returns 334 server IDs. Fan-Out Service issues 334 parallel gRPC calls. At 5 ms gRPC round-trip each, with 10 Fan-Out worker threads issuing 334 / 10 = 33.4 calls per thread sequentially: 33 × 5 ms = 165 ms. Still within 2-second SLA. (3) Optimization: Fan-Out worker has 334 goroutines running parallel gRPC calls → ~5 ms total (parallel). (4) CRS writes: 5 M / 334 = ~15 K viewers/CRS. At 1 μs/write → 15 ms per CRS. (5) For extreme events: partition the Fan-Out into multiple workers, each handling a subset of CRS servers. With 10 Fan-Out workers each handling 33 CRS servers: 33 parallel gRPC calls per worker = ~5 ms. Comment delivery chain: ingest (< 10 ms) + Kafka (5 ms) + fan-out dispatch (5 ms) + CRS write (15 ms) = ~35 ms median. (6) Kafka: one partition per stream for ordering. A 5 M viewer stream may produce 3,000 comments/sec (1% of 300 K active viewers posting 1/min). A single Kafka partition easily handles 3,000 messages/sec. Fan-Out worker consumes this one partition.

5. **Q: How do you ensure a newly connecting viewer sees the recent chat history without a large DB query?**
   A: Redis `recent_comments:{stream_id}` is a List of the last 150 serialized comment JSON objects. Maintained by Chat History Service: LPUSH on every new comment, LTRIM to 150. Size: 150 × 2 KB = 300 KB per stream. At 60,000 concurrent streams: 60 K × 300 KB = 18 GB in Redis. Manageable with a 64 GB Redis cluster. On new viewer connect: CRS reads `LRANGE recent_comments:{stream_id} 0 149` (one Redis command). Sends all 150 comments to client in the initial WebSocket frame batch. Total time: Redis read (~1 ms) + single WebSocket frame write (~2 ms) = ~3 ms for new viewer catch-up. This means a viewer joining mid-stream immediately sees the last ~5 minutes of chat without a DB query.

---

### 6.2 Hate Speech Filtering and AutoMod

**Problem it solves:**
At 50,000 comments/second with user-generated content, the system must detect and remove hate speech, spam, and prohibited content at scale, with < 2-second end-to-end latency (post time to deletion propagation), without creating false positives that suppress legitimate speech.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Synchronous pre-delivery filter** | Block message delivery until moderation check passes | No user sees bad content | Adds 200-500ms to every message; ML inference at 50K/sec requires ~5,000 GPU instances; false positives block legitimate speech |
| **Bloom filter word blocklist only** | Fast approximate membership test for blocked terms | Sub-ms; near-zero compute | High false positive rate; doesn't catch context-dependent hate speech; easily bypassed with character substitution (h4t3 speech) |
| **Async ML classifier + post-delivery deletion** | Deliver immediately; ML classifies async; delete if violation | Low latency for clean content; ML accuracy | Brief window where bad content is visible (~1-2 seconds) |
| **Rule engine + ML classifier pipeline** | Stage 1: fast rules (bloom filter, regex); Stage 2: ML for uncertain cases | Combines speed of rules with accuracy of ML | More complex; ML still adds latency |
| **User trust scoring + adaptive moderation** | High-trust users (long account history, good standing) get fast-tracked; new accounts get more scrutiny | Reduces ML load (trust users = 80% of traffic) | Cold start problem for new users; sophisticated bad actors create aged accounts |
| **Hash matching (PhotoDNA-style) for known content** | Hash-match known bad content (image hashes, known spam phrases) | Zero false positives for known violations | Only works for known content; novel violations bypass |

**Selected Approach: Layered async pipeline — Bloom filter (Stage 1) → Rule engine (Stage 2) → ML classifier (Stage 3), with trust-tier fast-tracking**

**Implementation Detail:**

*Delivery model:* Async post-delivery (comments visible immediately; violations deleted within 2 seconds). This is the right trade-off for live chat: blocking on moderation would ruin the real-time UX for the 99.5% of clean messages to protect against the 0.5% of violations. The brief visibility window (< 2 seconds) is acceptable given the ephemeral nature of live chat (most viewers won't see or retain a deleted comment if it's removed quickly).

*Pipeline stages:*

**Stage 1: Trust Tier Fast-Track** (< 0.1 ms)
- New users (account < 7 days old): ALL messages go through full pipeline.
- Known-bad users (account flagged): messages pass through extra scrutiny.
- Trusted users (account > 30 days, no violations, verified phone): message published directly to fan-out after Stage 2 (skip Stage 3 ML). Covers ~70% of messages.

**Stage 2: Bloom Filter + Regex Rule Engine** (< 5 ms)
- Per-channel custom word blocklist stored as RedisBloom filter (`BF.EXISTS blocklist:{channel_id} {normalized_word}`).
- Platform-wide blocklist (slurs, CSAM terms): separate Bloom filter on every Ingest server (in-memory, refreshed every 5 minutes from Redis).
- Text normalization: lowercase, l33tsp34k normalization (dictionary substitution: '4' → 'a', '3' → 'e', '@' → 'a'), strip special characters between characters. Catches common bypass attempts.
- Pattern matching: regex for spam patterns (repeated characters "aaaaaaa", promotional URLs, phone numbers in chat).
- Result: PASS (publish), REJECT (deny at ingest, return 400), SUSPICIOUS (pass to Stage 3).
- Catches ~80% of violations at near-zero cost.

**Stage 3: ML Classifier (async Kafka consumer)** (100-500 ms)
- Kafka consumer reads `raw_comments` topic.
- Batches of 32 comments sent to ML inference service (GPU cluster).
- Model: fine-tuned DeBERTa or RoBERTa (125M parameters), multi-label classifier: hate_speech, spam, off_topic_promotion, doxxing, sexual_content.
- Inference time: 32 comments × 50 ms batch inference = ~50 ms per batch (GPU). At 50 K comments/sec, need 50 K × 0.05 s / 32 comments per batch = 78 GPU inference servers. At 8 A10 GPUs each: 10 GPU servers.
- Score threshold: hate_speech > 0.85 → trigger deletion. 0.7-0.85 → log for human review queue (no auto-deletion). < 0.7 → clean.
- Model served via Triton Inference Server (NVIDIA) with dynamic batching. Scales horizontally.

**Stage 4: Action Propagation** (< 200 ms after Stage 3 decision)
- AutoMod service publishes `{type: "DELETE_COMMENT", comment_id, reason: "hate_speech", confidence: 0.95}` to `moderation_actions` Kafka topic.
- Fan-Out Service consumes `moderation_actions` and sends `COMMENT_DELETE` event to all viewers of the stream.
- Cassandra comment row updated: `is_deleted = true, deleted_by = AUTOMOD, deleted_at = now()`.
- Total E2E: post time + Kafka ingest latency (5 ms) + Stage 1-2 (5 ms) + Kafka to Stage 3 (10 ms) + ML inference (50 ms) + deletion propagation (10 ms) = ~80 ms from post to deletion. Well within 2-second SLA.

*False positive handling:*
- User can appeal AutoMod deletions via a review queue (human moderators review within 24 hours).
- Streamer can whitelist specific terms in their channel (override channel blocklist).
- False positive rate target: < 0.1% of clean comments flagged. At 50 K comments/sec × 0.1% = 50 false positives/second. Human reviewers process in batches.

*Numbers:*
- GPU inference at 50 K comments/sec: with 32-comment batches at 50 ms each, throughput per GPU server = 32/0.05 = 640 comments/sec. Need 50 K / 640 ≈ 78 A100-equivalent GPU instances. At spot pricing (~$2/hr), $156/hr ≈ $1.37 M/year. Significant cost — trust-tier fast-tracking (skipping Stage 3 for 70% of trusted users) reduces this to 30% of the ML load: 24 GPU servers, ~$420 K/year.

**Interviewer Q&As:**

1. **Q: How do you prevent bad actors from figuring out the exact threshold of the ML classifier to post borderline content?**
   A: Defense-in-depth strategy: (1) **Threshold randomization**: add ±0.05 Gaussian noise to the threshold before applying it. This makes the exact boundary unpredictable. (2) **Model opacity**: the ML model's output is not revealed to users — they just see "comment removed." No feedback loop about why exactly their comment was deleted. (3) **Adversarial training**: periodically include adversarial examples (borderline content designed to evade detection) in the training set. The model learns to handle these over time. (4) **Behavioral signals**: a user who repeatedly posts near-threshold content (even if individual messages pass) has a higher suspicion score that lowers their effective threshold. (5) **Shadow mode**: a secondary stricter model runs in parallel and flags content for human review even if the primary model doesn't delete it. High-risk accounts have the secondary model's decisions applied.

2. **Q: How would you handle a coordinated hate raid — 5,000 bots simultaneously posting hate speech in a streamer's chat?**
   A: A hate raid is a DDoS at the comment level: (1) **Rate limit per IP**: Cloudflare edge limits new WebSocket connections from the same /24 subnet. 5,000 bots from different IPs still slip through, but coordinated bot farms often share IP ranges. (2) **Account age gate**: Comment Ingest checks account age. If < 7 days old and NOT subscriber, apply maximum scrutiny + 30-second slow mode per account. Bot accounts are almost always new. (3) **Comment velocity anomaly**: if a stream goes from 100 comments/sec to 5,000 comments/sec in 10 seconds, trigger a circuit breaker — temporarily enable follower-only mode (must have followed channel for 10+ minutes to comment) automatically. Notify the streamer. (4) **Bloom filter for spam signatures**: hate raids typically use identical or very similar message templates. Bloom filter on message hash (normalized) detects repeated identical messages. Unique chat mode (dedup by text) can be auto-enabled during raids. (5) **Trust score**: new accounts with no prior comments in this channel flagged with LOW_TRUST, subjected to all three ML stages synchronously (blocking delivery until cleared — acceptable for untrusted new accounts). (6) **CAPTCHA gate**: after 5 failed comment attempts (AutoMod deletions), require CAPTCHA to continue posting.

3. **Q: How do you keep the ML model up-to-date as slurs and hate speech evolve with internet language?**
   A: (1) **Human review queue feedback loop**: human moderators who review borderline AutoMod decisions label the content (correct/incorrect). These labeled examples are added to the training set weekly. (2) **Active learning**: identify uncertain predictions (confidence 0.7-0.85) as the highest-value training examples. Send to human reviewers for labeling. (3) **Trend detection**: monitor sudden spikes in specific terms that AutoMod is missing (user reports via "report comment" feature). When a new term/phrase causes a spike in user reports without AutoMod catches, add to the word blocklist immediately (24-hour turnaround) and schedule for model training inclusion. (4) **Model versioning**: new model versions tested in shadow mode (run in parallel, compare decisions to current model, measure precision/recall on holdout set) before deployment. A/B testing on 1% of traffic for 24 hours before full rollout. (5) **Transfer learning**: fine-tune the base model on new data monthly, rather than retraining from scratch. Monthly retraining cycle keeps model current.

4. **Q: What is the false positive cost of blocking a legitimate comment and how do you measure it?**
   A: Metrics: (1) **False positive rate**: number of deleted comments later reversed by human review / total deleted comments. Target: < 5% of AutoMod decisions are false positives. (2) **User impact**: false positives frustrate legitimate users. Tracked as "comment deletion appeals" rate per MAU. Alert if > 0.1% of MAU submit appeals/week. (3) **Chilling effect**: harder to measure — users who self-censor because they fear false positives. Measured indirectly via user comment rate trends and A/B tests comparing moderation-heavy vs. moderation-light channels. (4) **Revenue impact**: if false positives disproportionately affect paying subscribers (who often post more), it directly harms subscription retention. Track subscriber churn rate correlated with AutoMod aggressiveness. (5) **Streamer satisfaction**: streamers whose chats are over-moderated complain via support channels. Track tickets mentioning "AutoMod too aggressive" as a qualitative signal. The business requirement is to minimize false positives while keeping precision high on hate speech — use F-beta score with beta < 1 (precision-weighted) for model evaluation.

5. **Q: How do you handle live language translation for international streams (e.g., a Spanish streamer with English viewers)?**
   A: Real-time translation: (1) **Language detection**: run fastText (language identification model, 50 ms, CPU) on every comment. Attach `detected_language` metadata. (2) **Translation API**: for comments in languages the viewer doesn't speak (based on their browser language preference), call Google Translate API or DeepL in the Fan-Out path. (3) **Caching**: common phrases in a stream ("!clip", "PogChamp", "KEKW") appear hundreds of times — cache translations by (source_lang, target_lang, text_hash) in Redis with 5-minute TTL. Cache hit rate for repetitive live chat: > 80%. (4) **Async translation**: translate asynchronously and send a `COMMENT_TRANSLATION` event (comment_id + translated_text + target_language) separately from the original comment delivery. This adds ~300-500 ms for translations but doesn't delay original comment display. (5) **Cost management**: translation API costs at 50 K comments/sec × 30% needing translation = 15 K translated/sec × avg 50 chars = 750 KB/sec of translated text. At Google Translate prices (~$20/M characters), this is $0.015/sec = $54,000/hour of peak traffic — expensive. Limit translation to viewer's explicitly opted-in preference (opt-in, not default) and only for streams flagged as "international."

---

### 6.3 Slow Mode, Rate Limiting, and Spam Prevention

**Problem it solves:**
In a popular stream, a single user or group of users can flood the chat, making it unreadable for others. Slow mode enforces a minimum interval between a user's messages. Other mechanisms (ban list, follower-only mode, unique chat mode, rate limiting at API layer) collectively maintain chat quality.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **DB-backed rate limiting** | Store last comment timestamp in PostgreSQL/MySQL | Durable | DB round trip on every comment = too slow at 50 K/sec |
| **In-memory rate limiting (stateful service)** | Each ingest server tracks per-user timestamps in memory | Fast (no network) | Stateless horizontal scaling breaks it — user hits different ingest server = state lost |
| **Redis-based rate limiting (centralized)** | Redis SET with TTL per (user, stream) | Fast (sub-ms); horizontally scalable ingest; atomic; TTL = slow_mode_seconds | Requires Redis call on every comment attempt (minor latency cost); Redis availability critical |
| **Sliding window log (Redis Sorted Set)** | Sorted set per user; score = timestamp; enforce N requests per window | Precise; supports burst allowance | More complex; O(log N) vs O(1) for simple slow mode |
| **Token bucket (Redis + Lua script)** | Token bucket state in Redis hash; replenished over time | Smooth burst handling | More complex Lua script; harder to reason about |

**Selected Approach: Redis SET NX with TTL for slow mode; Redis token bucket for API-level rate limiting; separate mechanisms for ban and follower checks**

**Implementation Detail:**

*Slow mode implementation:*
```
// On comment submit:
key = "slow:{stream_id}:{user_id}"
ttl = stream_chat_config.slow_mode_seconds

result = REDIS SET key "1" NX PX {ttl * 1000}
// NX = only set if Not eXists
// PX = millisecond expiry

if result == nil:  // Key already exists
    remaining_ttl = REDIS PTTL key  // Get remaining TTL in ms
    return 429 {"error": "slow_mode", "retry_after_ms": remaining_ttl}
else:  // Key was set (user hasn't commented in the slow mode window)
    proceed with comment submission
```

This is O(1) Redis operation, ~0.1 ms. The TTL atomically enforces the slow mode window — no cleanup job needed.

*Ban and timeout checking (pre-submit):*
- Ban cache: `SISMEMBER ban_cache:{channel_id} {user_id}`. Returns 0/1. Refreshed from DB every 5 minutes or on ban/unban event.
- Timeout check: `GET timeout:{channel_id}:{user_id}`. Returns expiry timestamp if timed out, null otherwise. Key TTL = timeout duration.

*Follower-only mode:*
- `follower_only_minutes > 0`: user must have followed the channel for at least N minutes.
- Check: `GET follower_ts:{channel_id}:{user_id}`. If key exists and `now() - ts < follower_only_minutes * 60`, reject. If key doesn't exist, DB lookup (PostgreSQL `channel_follows` table). Cache result in Redis for 1 hour.

*Unique chat mode:*
- Prevent identical messages in the same stream within 30 seconds.
- `SADD unique_texts:{stream_id} {normalize(text)}` with `EXPIRE unique_texts:{stream_id} 30`.
- But SET has no per-member TTL. Better: use Redis Sorted Set: `ZADD unique_texts:{stream_id} {now_ms} {text_hash}`, then `ZREMRANGEBYSCORE unique_texts:{stream_id} 0 {now_ms - 30000}` to expire old entries.
- Or simpler: Bloom filter for approximate dedup (false positive: rare duplicate incorrectly flagged; false negative: duplicate slips through occasionally — both acceptable).

*API-level rate limiting (overall):*
- Global rate limit: 10 comments per 10 seconds per user (beyond slow mode per-stream). Enforced at edge (Cloudflare rate limiting rules) and as a fallback at Ingest service.
- Implementation: sliding window using Redis Sorted Set per user_id (token bucket approach).
- IP-level rate limiting: 100 comments per 10 seconds per source IP (protects against single-IP bot farms). Enforced at Cloudflare edge.

*Bot detection:*
- Account age check: accounts < 24 hours old are rate-limited to 1 comment per 30 seconds regardless of slow mode setting.
- Behavior analysis: if `requests_per_second > 2` for a user_id over 5 minutes, flag as potential bot → require CAPTCHA on next comment attempt (frontend challenge).
- Known bot-hosting ASNs: Cloudflare WAF rules block known datacenter IP ranges from posting comments (viewers can still watch; posting from a datacenter IP requires CAPTCHA).

**Interviewer Q&As:**

1. **Q: How does slow mode work with Nitro/subscriber exemptions? Some users should be allowed to post faster than the slow mode limit.**
   A: Exempt users (subscribers, VIPs, moderators) bypass slow mode entirely. Implementation: the slow mode check is conditioned on the user's privilege level. Before the Redis SET NX call, check the user's channel membership tier (cached from auth token claims or a Redis hash `user_priv:{channel_id}:{user_id}` → tier). If tier ≥ SUBSCRIBER, skip the slow mode check entirely. This privilege check is O(1) (JWT claim or Redis GET) and adds < 0.1 ms. The privilege information is embedded in the JWT token issued at login (refreshed every 15 minutes to pick up subscription changes) — no extra Redis call needed for subscribers. Moderators have their privilege embedded in the auth token as well (set when the streamer grants mod status, triggers a token refresh).

2. **Q: How do you efficiently handle ban list lookups at 50,000 comment submissions per second?**
   A: Two-layer ban check: (1) **Bloom filter** (per-channel, in-memory on each Ingest server): false-positive rate ~0.1% (1 in 1,000 non-banned users get a false positive — checked against source of truth next). False-negative rate: 0% (no banned user gets through the Bloom filter). If Bloom filter says "possibly banned": go to layer 2. If "definitely not banned": skip layer 2. This reduces layer 2 load by ~99.9% (only 0.1% of comments hit layer 2 due to false positives + actual banned users trying to comment). (2) **Redis SET check** (centralized): `SISMEMBER ban_cache:{channel_id} {user_id}`. Redis latency: ~0.1 ms. At 50 K/sec × 0.1% false positive rate = 50 redis calls/sec from false positives, plus actual banned users trying to post. Trivial. (3) **Source of truth**: PostgreSQL `user_bans` table queried only on cache miss (TTL expiry = every 5 minutes). At 50 K comment/sec × cache miss rate ~0.01% = 5 DB queries/sec. Negligible.

3. **Q: How would you implement "Unique Chat" mode — preventing the same message from being posted multiple times in a short window?**
   A: Two approaches: (1) **Redis Sorted Set window**: `ZADD uchat:{stream_id} {now_ms} {sha256(normalize(text))}`. Before insertion, `ZRANGEBYSCORE uchat:{stream_id} {now_ms - 30000} +inf BYSCORE LIMIT 0 1 {text_hash}` to check if this text was posted in the last 30 seconds. If found: reject. `ZREMRANGEBYSCORE uchat:{stream_id} -inf {now_ms - 30000}` to prune old entries. Pros: exact; no false positives. Cons: O(log N) per insert/lookup; sorted set grows with unique messages per 30-second window. At 500 comments/sec in a large stream, 15,000 unique messages per 30-second window → 15,000 entries × 32 bytes = 480 KB. Manageable. (2) **Counting Bloom Filter (CBF)**: add message hash at comment post, decrement at expiry (30s). Check existence before posting. Pros: O(1) operations; memory efficient. Cons: approximate (false positives rare but possible); counting BF is more complex than standard BF. For production: Approach 1 for small-to-medium streams; Approach 2 with decay for mega-streams where the sorted set could grow large.

---

## 7. Scaling

**Horizontal Scaling:**
- **Comment Ingest Service**: Stateless; scales horizontally. At 250 K peak writes/sec, and each ingest server handles 10 K writes/sec (with Redis calls), need 25 ingest servers. ASG scales on CPU and request queue depth.
- **Chat Receive Service**: Scale on connection count. At 30 M viewers / 15 K per server = 2,000 CRS servers. Auto-scale on connections/server > 70%.
- **Fan-Out Workers**: Kafka consumer group. Auto-scale on Kafka lag > 5,000 messages. At 250 K comments/sec with 100 µs processing per comment (Redis lookup + 34 gRPC calls batch), one fan-out worker handles ~10 K comments/sec → need 25 workers. In practice, each worker handles a subset of Kafka partitions (one partition per stream → scale out workers as streams scale).
- **AutoMod ML**: Autoscale GPU instances on GPU utilization > 70%. Spot instances for cost efficiency (checkpointed inference jobs, acceptable if preempted).

**DB Sharding:**
- Cassandra: Partition key `(stream_id, bucket)` distributes comments naturally. Stream_ids are Snowflakes → uniform hash distribution. RF=3. 100-node cluster handles 250 K writes/sec (each Cassandra node handles ~2,500 writes/sec with replication overhead).
- PostgreSQL (streams, bans, Super Chats): Shard by `channel_id`. Low write volume (bans: 250/sec, Super Chats: 500/sec). 16 shards sufficient. Patroni HA.
- Redis: Cluster mode with 64 shards. Slot allocation by key hash. Viewer sets, slow mode keys, and ban caches distributed by key hash automatically.

**Replication:**
- Cassandra RF=3, LOCAL_QUORUM writes, LOCAL_ONE reads (recent history from Redis anyway, DB reads are for VOD/old content).
- PostgreSQL semi-synchronous replication.
- Redis Cluster 1:2 primary-to-replica ratio.

**Caching:**
- Recent comments (last 150): Redis List per stream. This is the primary read path for live comments; Cassandra is only the durable store.
- Stream config: in-memory on CRS servers (refreshed via Kafka `stream_events` topic on config change).
- Ban list: Redis Set (refreshed on ban events).
- User privilege tier: embedded in JWT (refreshed every 15 min).

**CDN:**
- Chat UI assets (JavaScript, CSS) served from CDN.
- Stream thumbnails, avatars, emotes: CDN with immutable content caching.
- Chat itself is real-time — CDN not applicable for live chat delivery.

### Interviewer Q&As on Scaling

1. **Q: How does Kafka partitioning work for live chat — do you use one partition per stream?**
   A: One Kafka partition per active stream provides ideal ordering (all comments for a stream are sequenced within the partition) and natural isolation (fan-out workers can be dedicated to specific partition ranges). At 60,000 active streams, 60,000 partitions is large but within Kafka's capacity (Confluent recommends up to 200,000 partitions per cluster with KRaft mode, no Zookeeper). Alternatively, use consistent hashing: `stream_id % N_partitions` where N = 10,000. Multiple streams share a partition; ordering within each stream is preserved as long as messages are keyed by stream_id (Kafka preserves order per key within a partition). This reduces partition count 6× at the cost of slight fan-out coupling between streams sharing a partition (one slow stream doesn't block another since they're different keys in the partition — consumer can process messages for each stream_id independently).

2. **Q: How do you handle the Cassandra write load during a major event where 100,000 streams peak simultaneously?**
   A: At 250 K writes/sec sustained: (1) Cassandra write path is append-only (LSM-tree) — writes go to memtable first, flushed to SSTables asynchronously. No read-before-write. This is optimal for comment storage. (2) Partition distribution: 100 K active streams × 800-byte avg row × 250 K writes/sec = 200 MB/sec raw write throughput. With RF=3: 600 MB/sec replicated write throughput. At 200 Mbps per node, need 3 nodes just for throughput (in practice, CPU and disk I/O are the bottleneck earlier). A 100-node cluster each handling 6 MB/sec = more than sufficient. (3) Hot partitions: a single mega-stream (500 K viewers, 7 comments/sec × 150 bytes = 1.05 KB/sec) = trivial. No hot partition risk. (4) Compaction: TimeWindowCompactionStrategy means compaction only happens within time windows. During an event, current-hour SSTs are written; previous-hour SSTs are read-only and compacted in the background independently. No write-stall from compaction.

3. **Q: How do you manage the Redis memory for viewer sets as streams scale up and down?**
   A: Viewer sets are ephemeral and small: `stream_servers:{stream_id}` is a Redis Set of CRS server IDs. At 60,000 active streams × avg 34 server IDs × 20 bytes/ID = 40.8 MB. Extremely small. The TTL mechanism (CRS heartbeat every 60s, key TTL 120s) means inactive streams automatically expire from Redis. When a stream ends: `stream_events` topic publishes STREAM_END → all CRS servers evict the stream's local connections → as the last viewer disconnects, CRS removes itself from the viewer set via `SREM`. When the set becomes empty, Redis reclaims the key (empty set is deleted). If the SREM misses (CRS crash), the 120s TTL cleans up stale entries. No memory leak possible.

4. **Q: How do you prevent a slow Kafka consumer (e.g., the ML classifier) from being a bottleneck that delays the fan-out?**
   A: Each Kafka consumer is an independent consumer group. The ML classifier (AutoMod) reads from `raw_comments` in consumer group `automod`. The Fan-Out service reads from `approved_comments` in consumer group `fanout`. These are separate topics — the Fan-Out topic receives comments immediately after the ingest service publishes them (bypassing ML classifier). The ML classifier runs independently and publishes DELETE events to `moderation_actions` topic when it detects violations. The Fan-Out service also consumes `moderation_actions` and pushes DELETE events to CRS servers. This architecture means ML latency (100-500 ms) never blocks comment delivery (< 50 ms). The worst case: a violating comment is visible for 500 ms before the ML classifier deletes it. This is the accepted trade-off for live chat UX.

5. **Q: How would you scale the system to handle 10× the current load (300 M concurrent viewers) — a global megaevent like the Olympics opening ceremony?**
   A: 300 M concurrent viewers = 10× current architecture. Scaling plan: (1) **CRS tier**: 300 M / 15 K = 20,000 servers. Pre-provisioned 2 weeks before the event. CRS servers are stateless — horizontal scaling is linear. (2) **Fan-Out**: scale Kafka partitions (pre-create 600,000 partitions for peak streams). Fan-Out workers auto-scale (Kubernetes HPA on Kafka consumer lag). (3) **Cassandra**: 1,000-node cluster for 2.5 M writes/sec (250 K × 10). Add nodes via online ring expansion 1 week before. (4) **Redis**: 640-shard cluster (10× current). Online resharding supports zero-downtime expansion. (5) **Comment Ingest**: 250 stateless servers for 2.5 M submissions/sec. (6) **Load test**: 2 weeks before, run a "Game Day" load test at 2× current peak to validate all systems. (7) **CDN pre-warming**: pre-populate CDN caches in all regions. (8) **Database pre-splitting**: Cassandra pre-splits token ranges for anticipated stream hotspots (known popular streams).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| CRS server crash | ~15 K viewers lose WebSocket connection | TCP RST; load balancer health check 10 s | Clients reconnect (exponential backoff); Redis cache provides missed comments; Redis viewer set entry expires in 120 s |
| Redis shard failure | Slow mode checks may fail; viewer set lookups slow | Redis Cluster FAIL detection < 5 s | Replica promotion < 30 s; during failover: slow mode check fails-open (allow comment) to avoid blocking all writes; viewer set uses stale or empty data temporarily |
| Cassandra node failure | Write acknowledgment delayed for 1/3 writes | Gossip < 5 s | RF=3 QUORUM writes survive; hinted handoff buffers failed node's writes; hot path (Redis recent_comments) unaffected |
| Kafka broker failure | Comment events delayed | Controller detects < 30 s | Leader election; consumer groups reconnect; comment delivery delayed by < 30 s |
| AutoMod ML service failure | No hate speech filtering | Health check on inference API | Circuit open on ML path; fall back to word blocklist only (Stage 2 only); degrade gracefully; alert for manual moderation |
| Comment Ingest service failure | Comment submission fails; 503 to users | Load balancer health check | Stateless; failed instances removed; remaining instances absorb load; users see brief failure, retry |
| Fan-Out service lag | Comments delayed > 2 s | Kafka consumer lag metric | Auto-scale Fan-Out workers; large stream fan-out switches to coarser granularity (batch delivery instead of per-comment) |
| Stream metadata Redis failure | Viewer sets inaccessible | Cluster FAIL alarm | Redis replica promotion; fan-out degrades to broadcasting to all CRS servers (conservative: O(all CRS) instead of O(stream_servers)) |
| Full region failure | All services in region down | CloudWatch + Route53 health checks | DNS failover to adjacent region; streams viewers reconnect to nearest healthy region |

### Idempotency

- **Comment submission**: Client sends `client_nonce` (UUID). Ingest service checks `SET NX dedup:{stream_id}:{client_nonce} 1 EX 60`. If key exists → duplicate, return existing comment_id without processing. Prevents double-posting on client retry.
- **Moderation actions**: DELETE action is idempotent (delete already-deleted comment = no-op in Cassandra). Ban action is upsert (INSERT ON CONFLICT UPDATE).
- **Fan-out delivery**: CRS maintains a short TTL dedup set: if `comment_id` already delivered to a connection (within 30 seconds), skip. Handles duplicate gRPC pushes from Fan-Out service retries.

### Circuit Breaker

- **Ingest → Redis (slow mode check)**: circuit opens if Redis latency > 50 ms over 10 seconds. Fallback: fail-open (skip slow mode check, allow comment through). Slow mode temporarily disabled, acceptable trade-off over blocking all comment submissions.
- **Fan-Out → CRS gRPC**: circuit opens if CRS error rate > 30% per server. Fallback: remove server from active routing list; log for ops investigation. Messages for its viewers are missed until server recovers or viewers reconnect to healthy servers.
- **AutoMod → ML inference**: circuit opens on 5 consecutive failures. Fallback: word blocklist only (Stage 2). Alert operations. Recovers automatically on next successful health probe (every 30 seconds).

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `comment.ingest.latency_p99` | Histogram | > 500 ms |
| `comment.delivery.latency_p99` | Histogram | > 2,000 ms |
| `fanout.kafka_lag` | Gauge per stream | > 5,000 messages |
| `crs.connections.active` | Gauge | > 90% per server |
| `automod.filter_rate` | Counter Rate | Anomaly: > 10% (false positive storm) or < 0.1% (filter broken) |
| `slow_mode.rejection_rate` | Counter Rate | > 30% (too aggressive slow mode hurting UX) |
| `moderation.deletion.latency_p99` | Histogram | > 3,000 ms |
| `cassandra.write.latency_p99` | Histogram | > 20 ms |
| `redis.slow_mode.op_latency_p99` | Histogram | > 10 ms |
| `super_chat.processing.success_rate` | Counter Rate | < 99.9% (payment loss risk) |
| `crs.reconnect_rate` | Counter Rate | > 5% per minute (indicates mass disconnect event) |
| `comment.unique_viewers_per_stream_p99` | Gauge | Track for capacity planning |
| `automod.ml.inference_latency_p99` | Histogram | > 1,000 ms (ML degrading) |
| `hate_speech.detection_rate_by_category` | Counter Rate | Anomaly detection (sudden spike = active raid) |

### Distributed Tracing

OpenTelemetry SDK in all services. Key spans:
- `ingest.validate_and_write`
- `ingest.slow_mode_check`
- `kafka.publish`
- `fanout.resolve_stream_servers`
- `fanout.grpc_broadcast_to_crs`
- `crs.write_to_connections`
- `automod.stage1_bloom_filter`
- `automod.stage2_ml_inference`
- `cassandra.write_comment`

Trace context propagated via Kafka headers (W3C `traceparent`). Sampling: 100% errors; 0.1% success (at 250 K/sec, 0.1% = 250 traces/sec).

### Logging

Structured JSON. Comment ingest logs include: `stream_id`, `user_id` (hashed), `comment_id`, `result` (accepted/rejected/slow_mode), `latency_ms`. Moderation action logs include: `comment_id`, `action_type`, `moderator_id` (or AUTOMOD), `reason`, `confidence_score`. Retention: 30 days hot (Elasticsearch), 1 year cold (S3). Legal hold: moderation action logs retained 7 years (regulatory requirement for some jurisdictions).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Why Chosen | What You Give Up |
|---|---|---|---|---|
| Comment delivery model | Async post-delivery moderation | Synchronous pre-delivery filtering | Minimizes delivery latency (real-time UX); only 0.5% violations; 2-second deletion window acceptable | Brief visibility of violating content; requires fast deletion propagation |
| Fan-out architecture | Kafka + Fan-Out Workers + CRS tier with batch gRPC | Direct pub/sub (Redis Pub/Sub) | Durable; replayable; decoupled; handles 125 M deliveries/sec peak; offline CRS recovery via replay | Added ~50 ms from Kafka latency; more infrastructure components |
| Slow mode enforcement | Redis SET NX with TTL | DB-backed timestamp | O(1) atomic operation; TTL eliminates cleanup jobs; sub-ms latency; horizontal ingest scalability | Redis single point (mitigated by cluster HA); fails-open on Redis failure |
| Recent chat cache | Redis List per stream (last 150) | Cassandra query on every join | Sub-ms new viewer catch-up; reduces Cassandra read load by >90% | Memory cost (18 GB for 60 K streams); approximate (may miss comments if Redis restarts mid-stream) |
| Comment storage | Cassandra with TWCS | Relational DB | Write-optimized LSM; time-series pattern fits perfectly; TWCS minimizes compaction for append-only data; 5-year TTL native | No secondary indexes by user_id (needs separate query pattern); eventual consistency |
| ML moderation | Async transformer model (Stage 3) | Synchronous ML on every comment | Non-blocking delivery; only untrusted users hit ML; 10× cheaper (trust-tier fast-track) | 200-500 ms deletion delay for violations; brief visibility window |
| Viewer set registry | Redis Set of CRS server IDs | Full member list of viewer user_ids | Small (server_id not user_id); O(num_CRS_servers) not O(num_viewers) in fan-out | Cannot target specific viewers; fan-out broadcasts to all CRS servers for a stream (each filters locally) |
| Super Chat persistence | Priority Kafka topic + separate DB | Same pipeline as regular comments | Payment records need ACID guarantees (Super Chats are revenue); separate topic allows priority delivery and different retention | Dual pipeline complexity; extra infra cost |

---

## 11. Follow-up Interview Questions

1. **Q: How would you design the VOD chat replay feature — showing chat messages synchronized with the video timeline?**
   A: VOD replay: viewer scrubs to time T in the video; chat should show comments from around time T. Implementation: (1) **Cassandra query**: `SELECT * FROM comments WHERE stream_id = ? AND bucket = floor(T_ms / BUCKET_SIZE) AND comment_id <= snowflake_at_time(T + 10s) AND comment_id >= snowflake_at_time(T - 5s)`. Returns comments around timestamp T. The Snowflake ID encodes the timestamp, so timestamp range queries are efficient index range scans. (2) **Playback sync**: client requests comments in 30-second windows. As video plays, client requests the next window when approaching the end of the current window. (3) **Caching for popular VODs**: Redis caches recent VOD scrubs (common "rewind to the best moment"). (4) **Comment density**: high-activity moments have thousands of comments per second. Client-side rendering limits display to last 50 comments visible at any time, scrolling fast. The full comment set is available for community-created "chat replay speed" overlays. (5) **Deleted comments**: VOD replay respects deletion flags — deleted comments shown as "[deleted]" or hidden entirely (configurable by channel). AutoMod deletions hidden by default in VOD replay.

2. **Q: How do you design the "Super Chat" / "Bits" paid message system to handle payment failures gracefully?**
   A: Super Chat flow: (1) Client submits payment intent (amount, message) to Super Chat Service. (2) Super Chat Service calls payment processor (Stripe/PayPal) to authorize the charge. If auth fails: return error to user, no message posted. (3) On auth success: create pending Super Chat record in PostgreSQL with status = PENDING. (4) Charge the payment (capture). If capture fails: mark FAILED, return error to user (rare edge case — auth usually implies capture success). (5) On capture success: mark AUTHORIZED, publish to `super_chats` Kafka topic. Fan-Out delivers with priority. (6) At end of stream or 1 hour: finalize payout to streamer (platform takes fee). Idempotency: each Super Chat has a client-generated idempotency_key. If the network times out after payment but before client receives confirmation, client retries with the same key — Super Chat Service returns the existing record (no double charge). Exactly-once payment guarantee via payment processor idempotency_key API.

3. **Q: How does Twitch's "IRC-based" chat protocol differ from a REST+WebSocket approach and why does it matter?**
   A: Twitch's chat uses a WebSocket wrapper around IRC (Internet Relay Chat, RFC 1459). Clients connect via `wss://irc-ws.chat.twitch.tv/` and authenticate with IRC commands (`PASS oauth:<token>`, `NICK <username>`). This has historical reasons (Twitch's chat was originally IRC) and ecosystem benefits: (1) **Bot ecosystem**: thousands of third-party chat bots use IRC libraries. Native IRC support means no protocol migration. (2) **Familiarity**: streamer tools, overlay software, and chat moderators often use IRC clients. (3) **Technical trade-offs**: IRC is text-based (slightly larger than binary framing); IRC commands are not as structured as JSON events (requires parsing). IRC doesn't have native features for reactions, Super Chats, etc. — these use Twitch-specific IRC tags (`@` prefix metadata). Modern Discord/YouTube use proper JSON WebSocket APIs. For a new system, JSON WebSocket with schema-validated events is strictly better for maintainability and feature expressiveness. Twitch keeps IRC for backward compatibility while internally managing the translation to/from modern service architecture.

4. **Q: How do you handle global chat (same chat seen by viewers in different regions with different latency)?**
   A: Global chat has an inherent ordering problem: a comment posted by a viewer in Tokyo and a viewer in New York within 100 ms of each other — Tokyo takes 180 ms to reach the US data center, New York takes 5 ms. Result: US viewers see New York's comment first; Tokyo viewers may see Tokyo's first. Approach: (1) **Server-assigned Snowflake IDs**: the ingest service in each region assigns the Snowflake (timestamp embedded) when the comment arrives at that region's server. Cross-region: comments are routed to the "primary region" for the stream (the region where the stream's channel is hosted). All comments for the same stream go through one region's ingest, ensuring a single timestamp authority. Latency cost: Tokyo viewer's comment must travel to the primary region (e.g., US-West) to get timestamped → +150-200 ms to posting latency for Tokyo viewers. (2) **Regional ingest with eventual consistency**: alternatively, allow each region to ingest and timestamp locally, then merge comments ordered by timestamp globally. This introduces reordering as global merge catches up. Viewers may briefly see comments "out of order" as the global merge fills in. For live chat, this is acceptable — no one can read chat fast enough to notice. This reduces ingest latency for all regions (post immediately to local region). (3) **Production reality**: Twitch and YouTube use option 2 (regional ingest with eventual global order). The reordering is imperceptible in a chat scrolling at 10+ comments/second.

5. **Q: How do you build a chat overlay for OBS/streaming software that reads live chat in real-time?**
   A: Chat overlay (popular feature: streamers show their chat on-screen): (1) **Chat Read API**: the Chat Receive WebSocket (`WSS /v1/chat/{stream_id}`) is the correct integration point. OBS browser source (Chromium-based) connects to a hosted overlay page (e.g., `https://overlay.twitch.tv/chat/{stream_id}`) that uses the WebSocket to receive events. (2) **Authentication**: the overlay page authenticates with the streamer's OAuth token (or a read-only token scoped to the chat). The WebSocket connection carries the auth. (3) **Rate of delivery**: all comments delivered in real-time; the overlay page implements client-side rendering with CSS animations. (4) **Custom styling**: overlay providers (StreamElements, Nightbot) poll the overlay API or use direct WebSocket connections and render with custom HTML/CSS. These are first-class API consumers. (5) **No API for direct OBS**: OBS browser source runs arbitrary JavaScript, so any web-based chat overlay is automatically supported. Dedicated API: `GET /v1/streams/{stream_id}/chat/connect` issues a short-lived token for the WebSocket connection without requiring the full OAuth flow (useful for overlay services).

6. **Q: How would you implement a "prediction" or "poll" feature in live chat with real-time vote aggregation?**
   A: Live poll: streamer creates a poll (question + options). All viewers can vote. Real-time vote counts displayed. Implementation: (1) **Poll creation**: streamer posts via REST API: `POST /v1/streams/{stream_id}/polls` → creates a poll record in PostgreSQL; publishes `POLL_CREATED` event to Kafka → Fan-Out delivers to all viewers. (2) **Vote submission**: viewer submits `POST /v1/streams/{stream_id}/polls/{poll_id}/votes` with their choice. Ingest validates (one vote per user per poll). Vote written to Redis HyperLogLog or Counter per option: `HINCRBY poll_votes:{poll_id} option_1 1`. (3) **Aggregation**: a real-time aggregator service reads vote increments from Redis every 500 ms and publishes aggregate counts to Kafka `poll_updates` topic. (4) **Fan-out**: Fan-Out delivers `POLL_UPDATE` events with current vote counts to all viewers every 1 second (not per-vote — batched to avoid 50 K events/second for popular polls). (5) **Finalization**: poll ends → final counts written to PostgreSQL. Kafka consumer validates Redis vs. DB consistency. Redis is the fast aggregation layer; PostgreSQL is the durable record. (6) **Scale**: 500 K viewers voting in 10 seconds = 50 K votes/sec. Redis HINCRBY at 50 K/sec = trivial. Broadcasting updates every 1 second to 500 K viewers = same as regular chat fan-out.

7. **Q: How do you handle chat for multi-stream collaborative events (multiple streamers raiding into each other's chats)?**
   A: A "raid" in Twitch: streamer A sends their N viewers to streamer B's stream, all arriving simultaneously. Chat implications: (1) **Viewer surge**: B's CRS tier absorbs N new WebSocket connections in seconds. Auto-scaling or pre-provisioned headroom handles this (raids are announced to B in advance via platform notification). (2) **Chat surge**: raiding viewers often spam a "raid message" simultaneously. Bot detection on Comment Ingest flags users posting identical messages. Unique chat mode (if enabled on B's channel) reduces this to one unique message per text hash. (3) **Ban list portability**: B's ban list doesn't include A's banned users. If B wants to restrict raiding viewers, follower-only mode (follow B for N minutes) is the standard mechanism. (4) **Cross-stream context**: platform sends B a `RAID` system event displayed in the chat as a system message: "A is raiding with N viewers!" This is a `comment_type = 'system'` message delivered via normal Fan-Out. (5) **No cross-stream database**: chats remain isolated. The raid is purely a user-redirection event at the client layer — viewers' clients navigate to B's stream page and connect their WebSocket to B's `stream_id`.

8. **Q: How do you design the "chat replay speed" feature where Twitch shows exactly how fast the chat was going at any point in a VOD?**
   A: Chat replay speed: a visual indicator showing comments-per-second over the VOD timeline. Implementation: (1) **Pre-computation**: when a stream ends, a batch job runs over the Cassandra message store for that stream_id: counts comments per 10-second window. Stores a time-series: `chat_density:{stream_id}` → array of (bucket_ts, comment_count) pairs. Stored in Redis during the "hot" period after stream (accessed for trending clips) and S3 for archival. (2) **Real-time during stream**: a streaming aggregator counts comments per 10-second window using Redis INCR: `INCR density:{stream_id}:{bucket}`. The density data drives the "Twitch Chat Replay" visualization. (3) **API**: `GET /v1/streams/{stream_id}/chat/density?start={ts}&end={ts}` returns the density array. Called by the VOD player to render the chat density timeline. (4) **Peak moment detection**: the platform uses chat density peaks as signals for "highlight clips" — moments with the highest comment rate are likely the most exciting moments. Automatic clip suggestions based on density spikes. This is a product feature built on top of the density data.

9. **Q: How do you handle emoteless / text-only chat modes as a spam prevention technique?**
   A: Emote-only mode: only messages containing at least one valid emote are accepted. Implementation: (1) Emote validation: at comment ingest, parse the message text and check for emote references. Each emote is referenced as `<emote_name>` or a specific syntax. The ingest service loads the channel's available emotes from Redis cache (refreshed on emote changes). (2) If emote-only is enabled (check `stream_chat_config.emote_only`): reject messages with no valid emotes (`400 emote_only`). (3) Client-side: the chat input shows "emote-only mode" indicator; keyboard is replaced by an emote picker. (4) Impact on moderation: emote-only mode dramatically reduces spam volume (text spam is impossible) but can still allow hate through emote combinations. AutoMod still runs on the text content that the emotes represent (emote names are often suggestive). (5) Follower-only + emote-only: common combination during raids — only followers who have been following for 10 minutes can post, and only emotes allowed. Dramatically reduces raid spam to near zero.

10. **Q: How would you design the "Channel Points" redemption that appears as a special message in chat?**
   A: Channel Points (Twitch): viewers earn points by watching; redeem for streamer-defined rewards (highlighted chat message, custom emote, etc.). Highlighted message in chat: (1) Viewer submits redemption: `POST /v1/channels/{channel_id}/rewards/{reward_id}/redeem` with redemption context (e.g., message text for "Highlight My Message"). (2) Channel Points Service deducts points from viewer's balance (PostgreSQL, ACID transaction). On success: creates a "redemption event" with type = 'channel_point_highlight'. (3) Publishes to Kafka `reward_redemptions` topic → Fan-Out delivers to all viewers as a special message type with visual decoration (colored border, point cost display). (4) Comment stored in Cassandra with `comment_type = 'channel_point_redemption'`. (5) Moderation: highlighted messages still pass through AutoMod. Streamer can reject/refund redemption if message violates rules (via moderation API). (6) Queue management: multiple pending redemptions queued in PostgreSQL with status = PENDING. Streamer's dashboard shows queue; approve/reject manually or set auto-approve. Queue limits (max 50 pending) prevent abuse.

11. **Q: How do you measure the effectiveness of your hate speech filter and set KPIs?**
   A: KPIs and measurement methodology: (1) **Precision**: of all comments AutoMod deletes, what fraction were actually hate speech? Measured via random sample human review (100 random deletions/day reviewed by trained annotators). Target: > 90% precision. (2) **Recall**: of all hate speech actually posted, what fraction does AutoMod catch? Measured via adversarial test set (red team posts known violations, measures what AutoMod catches). Target: > 80% recall. (3) **False positive impact**: monthly survey sample of users whose comments were deleted — "Do you think your comment violated chat rules?" Self-reported measure of false positive user experience. (4) **Time-to-detection (TTD)**: median time from comment post to deletion for actual violations. Alert if > 5 seconds. (5) **Bypass rate**: red team periodically tests novel bypass attempts (l33tspeak, new slurs, image text). Tracks the % of bypass attempts that succeed. Schedule for model retraining if bypass rate > 10%. (6) **Business metrics**: correlated streamer churn rate with AutoMod aggressiveness (do streamers leave when their viewers are over-moderated?); correlated viewer engagement drop in streams with high false positive rates.

12. **Q: How would you handle a live Q&A feature where the streamer picks comments to highlight?**
   A: Q&A mode: viewers submit questions; streamer/moderator selects questions to "feature" at the top of chat. Implementation: (1) Viewers tag messages as questions: `POST /v1/streams/{stream_id}/questions` (or a UI button that posts with `comment_type = 'question'`). (2) Questions stored separately in a `questions` table (or tagged in comments) in PostgreSQL for easy retrieval by the streamer's dashboard. (3) Streamer's dashboard: `GET /v1/streams/{stream_id}/questions?status=pending&sort=upvotes` — shows pending questions sorted by upvotes. Upvotes: viewers can upvote questions via `PUT /v1/streams/{stream_id}/questions/{q_id}/upvote`. Counter maintained in Redis (HINCRBY). (4) Streamer selects a question: `PATCH /v1/streams/{stream_id}/questions/{q_id}` with `status = featured`. (5) Fan-Out delivers a `FEATURED_QUESTION` event to all viewers — question pinned in a special Q&A overlay panel (separate from main chat). (6) Question answered: `status = answered` — moved to answered list. (7) Rate limit: viewers can ask 1 question per 60 seconds (uses same slow mode mechanism with different key). Upvote limited to 1 per question per viewer.

13. **Q: Describe your disaster recovery plan for a scenario where the entire US-East data center goes offline during a major stream.**
   A: DR scenario: US-East goes down during peak event with 10 M viewers. (1) **Detection**: Route53 health checks (30-second intervals) detect all US-East endpoints as unhealthy. CloudWatch alarms trigger. (2) **DNS failover**: Route53 fails over US-East traffic to US-West and EU-West (latency-based routing). DNS TTL: 30 seconds (short for fast failover). Failover propagates in 30-60 seconds (DNS TTL). (3) **Client reconnect**: All 10 M US-East viewers lose their WebSocket connections. Clients backoff and reconnect. DNS now points to US-West/EU-West. New connections established in 30-120 seconds. (4) **Stream continuity**: the stream ingestion (video) is separate; if US-East video ingest was live, stream may briefly go offline. CDN continues serving buffered video; streamer reconnects via US-West ingest endpoint. (5) **Chat continuity**: US-West and EU-West CRS servers accept reconnecting viewers. Redis cluster (replicated cross-region asynchronously) has recent_comments available. Comments posted in the last 30-60 seconds during failover may be partially lost (within Kafka's not-yet-replicated buffer). (6) **RPO/RTO**: RPO = 60 seconds of chat messages (Kafka cross-region replication lag). RTO = 2-3 minutes for full service restoration in secondary region. (7) **Post-mortem**: after recovery, analyze what caused the outage, run chaos engineering experiments to validate the failover procedures.

14. **Q: How would you handle copyright-infringing music detected in a stream, and what does that mean for the chat archive?**
   A: Copyright detection (DMCA) for audio is handled by the video streaming layer, not the chat layer. But the implication for chat: if a stream's VOD is muted or deleted due to DMCA, the chat replay is also affected. (1) **VOD muting**: when DMCA triggers audio muting for a time segment, the chat archive for that time segment is still accessible (chat content is separate from audio/video). However, the platform may choose to show a "this segment has been muted due to copyright" banner in the chat replay as well. (2) **VOD deletion**: if the entire VOD is deleted, the chat archive is deleted as well (foreign key relationship: `vod_id` in the stream table; cascade delete). Chat data is tied to the stream session. (3) **Chat-only DMCA**: unlikely — text chat messages are rarely copyrighted. But if a user posts copyrighted lyrics, it falls under normal comment moderation (DMCA notice → comment deletion via moderation API). (4) **Retention for appeals**: even for DMCA-deleted streams, comment data retained for 90 days in case of appeal. Stored in a separate "deleted content" archive S3 bucket, accessible only to trust & safety teams.

15. **Q: How do platform-specific emotes (Twitch subscriber emotes, YouTube Super Stickers) affect the comment storage and rendering pipeline?**
   A: Custom emotes are references, not embedded images. In a comment, an emote is stored as `{text: "PogChamp", positions: [[0, 8]], emote_id: "305954156"}`. The actual image lives on a CDN (`cdn.twitch.tv/emoticons/v2/{emote_id}/default/dark/1.0`). Storage implication: comments store only the emote reference (ID + position), not the image. The image is fetched client-side from CDN. At 50 K comments/sec with avg 2 emotes/comment: 100 K emote reference writes/sec — each is just an integer ID in a JSON field, ~8 bytes. Negligible storage overhead. Rendering pipeline: client parses the `emotes` field of the received comment, splits the text into text segments and emote image segments, renders inline images via CDN URL. Emote availability: (1) **Channel emotes**: available only in that channel's streams (subscriber perk). Emote metadata cached in browser localStorage. (2) **Global emotes**: platform-wide; always available. Pre-cached in the client app bundle or via `GET /v3/emotes/global`. (3) **Third-party emotes** (BTTV, FFZ): client-side browser extension resolves these; the platform doesn't know about them. These appear as plain text in the stored comment; the extension replaces text with images client-side. No server changes needed.

---

## 12. References & Further Reading

- **Twitch Engineering Blog — "Twitch Chat Architecture"**: https://blog.twitch.tv/en/2015/12/18/twitch-engineering-an-introduction-and-overview-1a0c6fbe7202/
- **Twitch Engineering Blog — "Optimizing for Large-Scale Real-Time Chat"**: https://blog.twitch.tv/en/2016/03/21/optimizing-twitch-chat-6-6b8b3e49c98a/
- **YouTube Engineering Blog — "Building YouTube Live Streaming"**: https://blog.youtube/inside-youtube/building-youtube-live-streaming/
- **Apache Kafka Documentation**: https://kafka.apache.org/documentation/
- **Redis Documentation — Sorted Sets**: https://redis.io/docs/data-types/sorted-sets/
- **RedisBloom — Probabilistic Data Structures**: https://redis.io/docs/stack/bloom/
- **Apache Cassandra TWCS Compaction**: https://cassandra.apache.org/doc/latest/cassandra/operating/compaction/twcs.html
- **Google Bigtable Whitepaper**: Chang, F. et al. (2006). "Bigtable: A Distributed Storage System for Structured Data." OSDI. https://research.google/pubs/pub27898/
- **Hate Speech Detection at Scale**: Schmidt, A. & Wiegand, M. (2017). "A Survey on Hate Speech Detection using Natural Language Processing." EACL. https://aclanthology.org/W17-1101/
- **DeBERTa (Transformer for NLP)**: He, P. et al. (2020). "DeBERTa: Decoding-enhanced BERT with Disentangled Attention." ICLR. https://arxiv.org/abs/2006.03654
- **IRC RFC 1459**: Oikarinen, J. & Reed, D. (1993). "Internet Relay Chat Protocol." IETF. https://www.rfc-editor.org/rfc/rfc1459
- **WebSocket RFC 6455**: https://www.rfc-editor.org/rfc/rfc6455
- **Snowflake ID (Twitter's approach)**: https://blog.twitter.com/engineering/en_us/a/2010/announcing-snowflake
- **NVIDIA Triton Inference Server**: https://developer.nvidia.com/triton-inference-server
- **Bloom Filters**: Bloom, B.H. (1970). "Space/Time Trade-offs in Hash Coding with Allowable Errors." Communications of the ACM.
