# System Design: Discord

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Server (Guild) / Channel model** — Users create servers; servers contain channels (text, voice, video, announcement, forum). Users join servers.
2. **Text messaging** — Real-time message delivery in text channels. Messages include text, embeds, stickers, GIFs.
3. **Message history** — Persistent, scrollable message history per channel; load older messages on scroll.
4. **Roles and permissions** — Granular permission system: server roles (e.g., @moderator, @vip) grant/restrict access to channels and actions.
5. **Reactions** — Emoji reactions on messages; counts visible to all.
6. **Voice channels** — Users join voice channels and talk in real-time using WebRTC. No scheduling required — always on.
7. **Video and screen sharing** — Go Live (screen share); video in voice channels (Nitro subscribers).
8. **Large server scalability** — "Mega-servers" with up to 500,000–1,000,000 members (e.g., official game servers, streamers).
9. **DMs and Group DMs** — Direct messages; group DMs up to 10 users (no server context).
10. **Threads** — Sub-conversations within a channel; thread messages don't clutter main channel.
11. **Forum channels** — Channel type where messages are organized as posts with replies (community Q&A format).
12. **Slash commands and bots** — Rich bot ecosystem; bots respond to slash commands and events via Gateway API.
13. **Notifications** — Desktop (push via Gateway), mobile (APNs/FCM), notification settings per server/channel.
14. **User mentions** — @user, @role, @everyone, @here mentions trigger notifications.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Availability | 99.99% (≤ 52 min/year downtime) |
| Message delivery latency (p99) | < 100 ms to online members in same region |
| Voice quality | < 20 ms jitter; < 150 ms one-way latency within continent |
| Scale | 500 M registered users; 150 M MAU; 19 M active servers daily |
| Concurrent WebSocket connections | ~8 M peak |
| Message volume | ~4 B messages/day |
| Storage | Petabyte-scale for messages; exabyte potential for media |
| Consistency | Strong ordering per channel; eventual for presence/reactions |
| Security | TLS in transit; AES-256 at rest; optional 2FA; no E2EE for messages (server can access) |

### Out of Scope

- Discord's monetization (Nitro, Server Boosts) — noted for feature gates.
- Discord's Embedded App SDK (in-app games/activities).
- AutoMod AI content filtering at implementation depth (noted at architecture level).
- Discord Shop / avatar decorations.
- Voice/video media plane implementation detail (covered at architecture level; WebRTC SRTP specifics excluded).

---

## 2. Users & Scale

### User Types

| Type | Description |
|---|---|
| **Regular User** | Joins servers, sends messages, participates in voice |
| **Server Owner** | Creates server; has all permissions; can't be kicked |
| **Server Admin** | Elevated permissions; manages roles, channels, bans |
| **Moderator** | Role-based moderation permissions (kick, ban, delete messages) |
| **Bot User** | Automated accounts registered via Developer Portal; interact via Gateway or HTTP API |
| **Nitro Subscriber** | Paid user with enhanced features (larger file uploads, video quality, custom emojis) |

### Traffic Estimates

**Assumptions:**
- 150 M MAU; 8 M peak concurrent connections (Discord's reported ~8.5 M peak in 2020; assume 10 M by 2024).
- 4 B messages/day (Discord's reported figure).
- Average text channel message size: 800 bytes (text + metadata + embed data).
- Average server size: 50 members; 20% in active channels at peak → 10 online members per channel.
- Mega-servers: 1% of servers have > 1,000 members; 0.1% have > 100,000 members.
- Reactions: 2 B/day (0.5 reactions per message on average).
- Voice channel users: 25 M concurrent at peak (Discord's video and voice).
- Files/media: 500 M/day; avg 200 KB (images, GIFs) → 100 TB/day upload.
- Peak-to-average ratio: 5x (gaming peaks are extremely spiky — new game launches, esports events).

| Metric | Calculation | Result |
|---|---|---|
| Messages/day | Given | 4 B/day |
| Messages/sec (avg) | 4 B / 86,400 | ~46,300 msg/s |
| Messages/sec (peak) | 46,300 × 5 | ~231,500 msg/s |
| Fan-out deliveries/sec (avg) | 46,300 × 10 (avg online members/channel) | ~463,000 deliveries/s |
| Fan-out deliveries/sec (peak) | 463,000 × 5 | ~2.3 M deliveries/s |
| Concurrent WebSocket connections (peak) | Given | 10 M |
| Voice/video concurrent users | Given | 25 M |
| File uploads/day | 500 M/day | 500 M |
| File upload throughput (avg) | 500 M × 200 KB / 86,400 | ~1.16 GB/s |
| Search queries/sec (avg) | 150 M DAU × 3 queries/day / 86,400 | ~5,200 queries/s |
| Reaction writes/sec (avg) | 2 B / 86,400 | ~23,100 reactions/s |

### Latency Requirements

| Operation | P50 | P99 |
|---|---|---|
| Message delivery (online, same region) | 30 ms | 100 ms |
| Message delivery (cross-region) | 100 ms | 300 ms |
| Voice latency (one-way, same continent) | 50 ms | 150 ms |
| Message history load | 50 ms | 200 ms |
| Role/permission check | 1 ms | 10 ms (cached) |
| Reaction write | 20 ms | 100 ms |
| Search results | 200 ms | 1 s |

### Storage Estimates

| Data Type | Calculation | Volume |
|---|---|---|
| Message rows (1 year, no deletion) | 4 B/day × 365 × 800 bytes | ~1.17 PB/year |
| Media/file storage (1 year) | 500 M/day × 365 × 200 KB | ~36.5 PB/year |
| Search index | ~2× message text volume | ~0.5 PB/year |
| User data (profiles, settings) | 500 M users × 5 KB | ~2.5 TB (static) |
| Server/guild metadata | 19 M active servers × 20 KB | ~380 GB (static) |
| Role/permission definitions | 19 M servers × avg 10 roles × 2 KB | ~380 GB (static) |
| Voice session logs | 25 M concurrent × 3600s avg × 100 bytes | ~9 TB/day |

### Bandwidth Estimates

| Direction | Calculation | Throughput |
|---|---|---|
| Inbound text messages (avg) | 46,300 × 800 bytes | ~37 MB/s |
| Outbound text messages (avg) | 463,000 × 800 bytes | ~370 MB/s |
| Outbound text messages (peak) | 2.3 M × 800 bytes | ~1.84 GB/s |
| Voice/video media (avg, 64 Kbps audio × 25 M users) | 25 M × 64 Kbps | ~200 GB/s (mostly peer-to-peer or SFU relay) |
| File upload inbound (avg) | 1.16 GB/s | 1.16 GB/s |
| File download outbound (avg, 4× read amplification) | 4.64 GB/s | 4.64 GB/s |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                                 Client Layer                                             │
│   Desktop (Electron)    Web (React)    iOS/Android     Bot (HTTP API / Gateway)         │
└────────┬──────────────────┬──────────────────┬─────────────────┬────────────────────────┘
         │  WSS             │  HTTPS           │  WSS/HTTPS      │  WSS/HTTPS
         │                  │                  │                 │
┌────────▼──────────────────▼──────────────────▼─────────────────▼────────────────────────┐
│                    Global Edge / Load Balancing                                          │
│    Cloudflare Anycast (DDoS, static CDN)    AWS NLB per region (TCP sticky for WS)      │
└──────────────────────────────────┬───────────────────────────────────────────────────────┘
                                   │
         ┌─────────────────────────┼─────────────────────────────────────┐
         │                         │                                     │
┌────────▼──────────┐  ┌───────────▼───────────┐  ┌───────────────────▼──────────────────┐
│  Gateway Service  │  │   HTTP REST API Svc   │  │    Voice Server (WebRTC SFU)         │
│  (WebSocket mgmt) │  │  (CRUD, permissions,  │  │    Regional voice relays             │
│  ~500 servers     │  │   message history,    │  │    Mediasoup / Janus-based           │
│  20K conns each   │  │   search, file mgmt)  │  │    DTLS-SRTP for media              │
└────────┬──────────┘  └───────────┬───────────┘  └──────────────────────────────────────┘
         │                         │
┌────────▼─────────────────────────▼─────────────────────────────────┐
│                     Event Dispatch Service                          │
│  Kafka (topics: messages, reactions, presence, member_updates,      │
│         voice_state_updates, thread_events)                         │
└──┬──────────────────────────────────────────────────────────────────┘
   │
   ├──────────────────────────────────────────────────────────────────────────┐
   │                                                                          │
┌──▼──────────────────┐  ┌─────────────────────────┐  ┌───────────────────────▼──────────┐
│   Message Store     │  │  Guild Fan-Out Service   │  │   Notification Service           │
│  (ScyllaDB,         │  │  - Large server fan-out  │  │   (APNs, FCM, desktop push)      │
│   sharded by        │  │  - Role/permission check │  │   Evaluates @mention, keywords   │
│   channel_id)       │  │  - Online member set     │  └──────────────────────────────────┘
└─────────────────────┘  └─────────────────────────┘
┌─────────────────────┐  ┌─────────────────────────┐  ┌──────────────────────────────────┐
│  Permissions Cache  │  │  Presence Service        │  │  Search Service                  │
│  (Redis, per-user   │  │  (Redis + Guild member   │  │  (Elasticsearch per-guild        │
│   per-channel perm  │  │   online set)            │  │   or shared cluster)             │
│   set cache)        │  └─────────────────────────┘  └──────────────────────────────────┘
└─────────────────────┘
┌─────────────────────────────────────────────────┐  ┌──────────────────────────────────┐
│               Guild/Metadata Store              │  │  Object Storage                  │
│  (PostgreSQL, sharded by guild_id)              │  │  (Cloudflare R2 / S3             │
│  Guilds, channels, roles, members, invites      │  │   for files, media, avatars)     │
└─────────────────────────────────────────────────┘  └──────────────────────────────────┘
```

**Component Roles:**

- **Gateway Service**: Long-lived WebSocket connections. Discord's Gateway API (clients subscribe to events per-guild). Heartbeat every 41.25 seconds (Discord's actual interval). Session resumption on reconnect (client sends last sequence number, server replays missed events). Handles presence updates and voice state changes.
- **HTTP REST API Service**: All REST operations — message CRUD, guild/channel management, role assignment, file upload URLs, invite link creation. Enforces permissions on every request.
- **Voice Server (SFU)**: Regional Selective Forwarding Unit servers for voice channels. Clients connect via WebRTC (UDP/DTLS-SRTP). SFU receives each participant's audio/video stream and forwards to other participants without mixing (selective forwarding). Media stays encrypted (DTLS-SRTP).
- **Event Dispatch Service (Kafka)**: All events flow through Kafka. Guild Fan-Out consumes and routes to connected Gateway servers.
- **Message Store (ScyllaDB)**: Stores all text channel messages. Partitioned by `channel_id`; clustered by `message_id DESC`. ScyllaDB (Cassandra-compatible, C++ implementation) for lower latency and higher throughput than Cassandra.
- **Guild Fan-Out Service**: The most complex component. Resolves which users should receive a given event, applies permission filtering, batches deliveries to Gateway servers.
- **Permissions Cache (Redis)**: Per-user, per-channel computed permission set (bitmask). Invalidated on role change. Avoids re-computing complex role hierarchy on every message check.
- **Presence Service**: Tracks which users are online in which guilds. Used for @here and online member count badges. Redis Sorted Set per guild.
- **Guild/Metadata Store (PostgreSQL, sharded)**: Guilds, channels, roles, role assignments, channel overwrites, invite links, member join/ban records. PostgreSQL for ACID guarantees on role/permission changes.
- **Object Storage (Cloudflare R2 / S3)**: All media. CDN-fronted. Avatars, emojis, attachments, stickers.

**Primary Use-Case Data Flow (user sends a text message in a guild channel):**

1. Client sends via WebSocket (`OP 0 DISPATCH / MESSAGE_CREATE`) or REST `POST /channels/{channel_id}/messages`.
2. Gateway or REST API validates auth token + user permissions (channel READ_MESSAGES + SEND_MESSAGES permission bits).
3. Message written to ScyllaDB (channel shard).
4. Kafka event published: `{type: "MESSAGE_CREATE", guild_id, channel_id, message_id, author_id, content, ...}`.
5. Guild Fan-Out Service reads event, fetches all guild members with access to the channel (from Permissions Cache + online member set in Redis).
6. Groups online members by their Gateway server. Sends batched gRPC pushes to each Gateway.
7. Each Gateway dispatches `MESSAGE_CREATE` event (JSON) to all connected clients in the batch.
8. Notification Service evaluates @mentions, keyword alerts; sends APNs/FCM for offline users.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Users (global table, replicated across regions)
-- ============================================================
CREATE TABLE users (
    user_id         BIGINT          PRIMARY KEY,  -- Discord Snowflake ID
    username        VARCHAR(32)     NOT NULL,
    discriminator   SMALLINT        NOT NULL,     -- legacy #XXXX, now 0 for new users
    global_name     VARCHAR(32),                  -- new username system
    email           VARCHAR(255)    UNIQUE,
    phone           VARCHAR(32)     UNIQUE,
    avatar_hash     VARCHAR(64),                  -- CDN URL derived from hash
    banner_hash     VARCHAR(64),
    accent_color    INT,
    flags           BIGINT          NOT NULL DEFAULT 0,  -- bitmask: bot, staff, partner, etc.
    premium_type    SMALLINT        NOT NULL DEFAULT 0,   -- 0=none, 1=Nitro Classic, 2=Nitro
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    mfa_enabled     BOOLEAN         NOT NULL DEFAULT false,
    locale          VARCHAR(10),
    INDEX idx_username (username)
) ENGINE=InnoDB;

-- ============================================================
-- Discord Snowflake ID Structure (used as primary keys throughout)
-- 64-bit integer:
--   [63..22] = milliseconds since Discord epoch (2015-01-01T00:00:00.000Z)
--   [21..17] = internal worker ID
--   [16..12] = internal process ID
--   [11..0]  = increment (per process, per ms)
-- This provides: globally unique, time-ordered IDs without coordination.
-- ============================================================

-- ============================================================
-- Guilds (Servers)
-- ============================================================
CREATE TABLE guilds (
    guild_id            BIGINT      PRIMARY KEY,
    name                VARCHAR(100) NOT NULL,
    description         TEXT,
    icon_hash           VARCHAR(64),
    splash_hash         VARCHAR(64),
    owner_id            BIGINT      NOT NULL,
    region              VARCHAR(32),  -- deprecated; now voice region per-channel
    afk_channel_id      BIGINT,
    afk_timeout         INT         NOT NULL DEFAULT 300,  -- seconds
    verification_level  SMALLINT    NOT NULL DEFAULT 0,   -- 0=NONE, 1=LOW, 2=MEDIUM, 3=HIGH, 4=VERY_HIGH
    default_msg_notif   SMALLINT    NOT NULL DEFAULT 0,   -- 0=ALL_MESSAGES, 1=ONLY_MENTIONS
    explicit_content_filter SMALLINT NOT NULL DEFAULT 0,
    features_json       JSON        NOT NULL DEFAULT '[]',  -- e.g., ["COMMUNITY", "NEWS"]
    boosts              INT         NOT NULL DEFAULT 0,
    boost_tier          SMALLINT    NOT NULL DEFAULT 0,    -- 0-3
    max_members         INT         NOT NULL DEFAULT 500000,
    member_count        INT         NOT NULL DEFAULT 0,   -- denormalized, updated async
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_owner (owner_id)
) ENGINE=InnoDB;

-- ============================================================
-- Guild Members
-- ============================================================
CREATE TABLE guild_members (
    guild_id        BIGINT      NOT NULL,
    user_id         BIGINT      NOT NULL,
    nickname        VARCHAR(32),                  -- per-guild display name override
    joined_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    premium_since   TIMESTAMPTZ,                  -- when they started boosting
    deaf            BOOLEAN     NOT NULL DEFAULT false,
    mute            BOOLEAN     NOT NULL DEFAULT false,
    timeout_until   TIMESTAMPTZ,                  -- communication timeout ("time out" feature)
    flags           INT         NOT NULL DEFAULT 0,
    PRIMARY KEY (guild_id, user_id),
    INDEX idx_gm_user (user_id)
) ENGINE=InnoDB;

-- ============================================================
-- Roles
-- ============================================================
CREATE TABLE roles (
    role_id         BIGINT      PRIMARY KEY,
    guild_id        BIGINT      NOT NULL,
    name            VARCHAR(100) NOT NULL,
    color           INT         NOT NULL DEFAULT 0,      -- RGB color integer
    hoist           BOOLEAN     NOT NULL DEFAULT false,  -- display separately in member list
    position        INT         NOT NULL DEFAULT 0,      -- ordering; higher = higher priority
    permissions     BIGINT      NOT NULL DEFAULT 0,      -- permission bitmask (64 bits)
    mentionable     BOOLEAN     NOT NULL DEFAULT false,
    managed         BOOLEAN     NOT NULL DEFAULT false,  -- managed by integration/bot
    icon_hash       VARCHAR(64),
    unicode_emoji   VARCHAR(64),
    INDEX idx_role_guild (guild_id)
) ENGINE=InnoDB;

CREATE TABLE member_roles (
    guild_id        BIGINT      NOT NULL,
    user_id         BIGINT      NOT NULL,
    role_id         BIGINT      NOT NULL,
    assigned_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (guild_id, user_id, role_id),
    INDEX idx_mr_role (guild_id, role_id)
) ENGINE=InnoDB;

-- ============================================================
-- Channels
-- ============================================================
CREATE TABLE channels (
    channel_id      BIGINT      PRIMARY KEY,
    guild_id        BIGINT,     -- NULL for DM channels
    name            VARCHAR(100),
    channel_type    SMALLINT    NOT NULL,
    -- 0=GUILD_TEXT, 1=DM, 2=GUILD_VOICE, 3=GROUP_DM, 4=GUILD_CATEGORY,
    -- 5=GUILD_NEWS, 10=GUILD_NEWS_THREAD, 11=GUILD_PUBLIC_THREAD,
    -- 12=GUILD_PRIVATE_THREAD, 13=GUILD_STAGE_VOICE, 15=GUILD_FORUM
    position        INT,
    parent_id       BIGINT,     -- category channel parent, or parent channel for threads
    topic           TEXT,
    nsfw            BOOLEAN     NOT NULL DEFAULT false,
    last_message_id BIGINT,
    slowmode_delay  INT         NOT NULL DEFAULT 0,  -- seconds between messages per user
    bitrate         INT,        -- for voice channels
    user_limit      INT,        -- for voice channels (0 = unlimited)
    rtc_region      VARCHAR(32),  -- voice region override
    video_quality   SMALLINT    NOT NULL DEFAULT 1,  -- 1=AUTO, 2=FULL (720p)
    default_auto_archive_duration INT,  -- for threads in forum/news channels
    INDEX idx_channel_guild (guild_id)
) ENGINE=InnoDB;

-- ============================================================
-- Channel Permission Overwrites (per-channel role/user overrides)
-- ============================================================
CREATE TABLE permission_overwrites (
    channel_id      BIGINT      NOT NULL,
    overwrite_id    BIGINT      NOT NULL,   -- role_id or user_id
    overwrite_type  SMALLINT    NOT NULL,   -- 0=role, 1=member
    allow_bits      BIGINT      NOT NULL DEFAULT 0,  -- permissions explicitly allowed
    deny_bits       BIGINT      NOT NULL DEFAULT 0,  -- permissions explicitly denied
    PRIMARY KEY (channel_id, overwrite_id)
) ENGINE=InnoDB;

-- ============================================================
-- Messages
-- ScyllaDB CQL (Cassandra-compatible) — not MySQL
-- Sharded by channel_id; time-ordered by message_id (Snowflake)
-- ============================================================
CREATE TABLE messages (
    channel_id      BIGINT,
    bucket          INT,          -- floor(message_id_epoch_ms / (1000*60*60*24*7)) — weekly bucket
    message_id      BIGINT,       -- Snowflake ID (time-ordered, globally unique)
    guild_id        BIGINT,
    author_id       BIGINT,
    content         TEXT,
    message_type    SMALLINT,     -- 0=DEFAULT, 7=REPLY, 18=THREAD_CREATED, 19=REPLY, 20=APP_COMMAND, etc.
    referenced_msg_id BIGINT,     -- for replies: the referenced message_id
    embeds          TEXT,         -- JSON serialized list of embed objects
    attachments     TEXT,         -- JSON serialized list of attachment objects
    mention_roles   TEXT,         -- JSON array of role_ids mentioned
    mention_everyone BOOLEAN,
    edited_timestamp BIGINT,
    pinned          BOOLEAN,
    flags           INT,          -- 64=SUPPRESS_EMBEDS, 4=CROSSPOSTED, etc.
    deleted         BOOLEAN,
    PRIMARY KEY ((channel_id, bucket), message_id)
) WITH CLUSTERING ORDER BY (message_id DESC)
  AND compaction = {'class': 'TimeWindowCompactionStrategy',
                    'compaction_window_unit': 'DAYS',
                    'compaction_window_size': 7};
-- TimeWindowCompactionStrategy: optimal for time-series data;
-- compacts data within time windows; old buckets are immutable → perfect for archival

-- ============================================================
-- Reactions
-- ============================================================
-- ScyllaDB CQL:
CREATE TABLE reactions (
    channel_id      BIGINT,
    message_id      BIGINT,
    emoji_id        BIGINT,       -- custom emoji ID or 0 for unicode
    emoji_name      TEXT,         -- unicode string or emoji name
    user_id         BIGINT,
    reacted_at      BIGINT,       -- epoch ms
    PRIMARY KEY ((channel_id, message_id, emoji_id, emoji_name), user_id)
);

-- Reaction counts denormalized on message row (updated via ScyllaDB lightweight transactions
-- or async via Kafka consumer that writes aggregated counts)
-- For read: "get all reactions for a message" = query reactions table by (channel_id, message_id)

-- ============================================================
-- Voice States (ephemeral — stored in Redis, not DB)
-- Redis Hash key: voice_state:{guild_id}:{user_id}
-- Fields: channel_id, session_id, self_mute, self_deaf, server_mute, server_deaf, self_stream, self_video
-- TTL: 90 seconds (renewed by voice server heartbeat)
-- ============================================================

-- ============================================================
-- Presence (ephemeral — Redis)
-- Key: presence:{user_id}
-- Type: Hash
-- Fields: status (online/idle/dnd/offline), activities_json, client_status_json
-- TTL: 60 seconds (renewed by Gateway heartbeat)
-- Key: guild_online:{guild_id}
-- Type: Sorted Set (user_id → last_seen_ts score)
-- Used for online member count and @here targeting
-- ============================================================

-- ============================================================
-- Threads (stored as channel rows of type THREAD in channels table)
-- Thread metadata:
-- ============================================================
CREATE TABLE thread_metadata (
    channel_id          BIGINT      PRIMARY KEY,  -- thread channel_id
    archived            BOOLEAN     NOT NULL DEFAULT false,
    auto_archive_duration INT       NOT NULL DEFAULT 1440,  -- minutes
    archive_timestamp   TIMESTAMPTZ,
    locked              BOOLEAN     NOT NULL DEFAULT false,
    invitable           BOOLEAN,    -- private threads: can non-members invite?
    create_timestamp    TIMESTAMPTZ
) ENGINE=InnoDB;

CREATE TABLE thread_members (
    channel_id  BIGINT  NOT NULL,   -- thread channel_id
    user_id     BIGINT  NOT NULL,
    joined_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (channel_id, user_id),
    INDEX idx_tm_user (user_id)
) ENGINE=InnoDB;

-- ============================================================
-- Invites
-- ============================================================
CREATE TABLE invites (
    code            VARCHAR(10) PRIMARY KEY,
    guild_id        BIGINT,
    channel_id      BIGINT      NOT NULL,
    inviter_id      BIGINT,
    max_uses        INT         NOT NULL DEFAULT 0,   -- 0 = unlimited
    uses            INT         NOT NULL DEFAULT 0,
    max_age         INT         NOT NULL DEFAULT 86400,  -- seconds; 0 = never expires
    temporary       BOOLEAN     NOT NULL DEFAULT false,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at      TIMESTAMPTZ,
    INDEX idx_invite_guild (guild_id)
) ENGINE=InnoDB;
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| **ScyllaDB** | Messages, reactions | Cassandra-compatible CQL; C++ implementation with 10x lower latency than Cassandra (sub-ms p99 reads); significantly lower tail latency; Shard-per-core architecture avoids JVM GC pauses; TimeWindowCompactionStrategy ideal for time-series messages | Newer; smaller community than Cassandra; operational expertise curve | **Selected** for message and reaction storage; Discord's actual production choice (as confirmed in Discord's engineering blog "How Discord Stores Trillions of Messages") |
| **Apache Cassandra** | Alternative to ScyllaDB | Battle-tested; huge community; CQL compatible | JVM GC pauses cause latency spikes; slower at Discord's required p99 | Not selected (Discord migrated away from Cassandra to ScyllaDB) |
| **PostgreSQL (sharded)** | Guild metadata, roles, permissions, channels | ACID; rich relational queries for permission computations; JOINs within guild shard | Scaling complexity; sharding required | **Selected** for structured relational data |
| **Redis Cluster** | Presence, voice state, permissions cache, online member sets | Sub-ms; native data structures; TTL support | Memory cost; not durable source of truth | **Selected** for ephemeral and hot-path caching |
| **Elasticsearch** | Message search | Full-text search; horizontal scale; BM25 relevance | Eventual consistency; expensive; operational complexity | **Selected** for search indexing |
| **Cloudflare R2 / S3** | Media blobs, avatars, emojis, stickers | Scale; CDN integration; 11 nines durability | Egress cost; vendor dependency | **Selected** for object storage |

**Why ScyllaDB over Cassandra:**
Discord's engineering blog "How Discord Stores Trillions of Messages" (2023) documented their migration from Cassandra to ScyllaDB. Key reasons: (1) Shard-per-core model in ScyllaDB avoids JVM GC pauses that caused Cassandra's p99 latency spikes (Cassandra GC would spike to 5-10+ seconds under heavy compaction). (2) ScyllaDB achieved 10× better tail latency at the same throughput. (3) Discord processes trillions of messages; at 4 B/day × 365 = ~1.46 trillion messages/year, the p99 latency improvement directly impacts user experience during gaming events.

---

## 5. API Design

Authentication: Discord uses Bearer token (bot tokens: `Bot {token}`; user tokens: OAuth 2.0 Bearer). All endpoints versioned: `/api/v10/`. Rate limiting: per-route, per-major parameter (channel_id or guild_id), per-token. Responses include headers: `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`, `X-RateLimit-Bucket`.

### REST Endpoints

#### Messages

```
POST /api/v10/channels/{channel_id}/messages
Authorization: Bot <token>
Request:
{
  "content": "Hello, Discord!",
  "tts": false,
  "embeds": [
    {
      "title": "Embed Title",
      "description": "Embed body",
      "color": 5814783,
      "fields": [{"name": "Field", "value": "Value", "inline": true}],
      "image": {"url": "https://example.com/image.png"},
      "timestamp": "2026-04-09T10:00:00.000Z"
    }
  ],
  "components": [...],      // Interactive components (buttons, select menus)
  "message_reference": {    // For replies
    "message_id": "1234567890",
    "channel_id": "...",
    "guild_id": "...",
    "fail_if_not_exists": false
  },
  "allowed_mentions": {
    "parse": ["users", "roles"],  // Control who gets notified
    "users": ["user_id_1"],
    "replied_user": true
  },
  "flags": 4  // SUPPRESS_EMBEDS flag
}
Response 200: { Message Object }
Rate Limit: 5 messages/5s per channel (per-route bucket); global 50 req/s per token

GET /api/v10/channels/{channel_id}/messages
Query Params:
  around=<message_id>   // messages around this ID (50 returned)
  before=<message_id>   // messages before this ID
  after=<message_id>    // messages after this ID
  limit=50 (max 100)
Response 200: [ Message Object array ]
Notes: Snowflake-based pagination (no offset). "around" returns 25 before + 25 after.
Rate Limit: 5 requests/5s per channel

PATCH /api/v10/channels/{channel_id}/messages/{message_id}
Request: { "content": "edited text", "embeds": [...] }
Response 200: { Message Object }
Note: Only message author (or users with MANAGE_MESSAGES) can edit.

DELETE /api/v10/channels/{channel_id}/messages/{message_id}
Response 204
Rate Limit: 5 deletes/1s per channel

POST /api/v10/channels/{channel_id}/messages/bulk-delete
Request: { "messages": ["id1", "id2", ...] }  // max 100 messages, not older than 2 weeks
Response 204
Rate Limit: 1 request/s per channel
```

#### Reactions

```
PUT /api/v10/channels/{channel_id}/messages/{message_id}/reactions/{emoji}/@me
// emoji: "👍" (URL-encoded) for unicode; "emoji_name:emoji_id" for custom
Response 204
Rate Limit: 1 reaction/0.25s per user (effectively 4/s)

DELETE /api/v10/channels/{channel_id}/messages/{message_id}/reactions/{emoji}/@me
Response 204

GET /api/v10/channels/{channel_id}/messages/{message_id}/reactions/{emoji}
Query: { after: user_id, limit: 100 (max) }
Response 200: [ User Object array ]  // Users who reacted with this emoji
```

#### Guilds and Channels

```
POST /api/v10/guilds
Request: { "name": "My Server", "region": null, "icon": "<base64 png>" }
Response 201: { Guild Object }
Rate Limit: 10 guild creations per account per 10 minutes

GET /api/v10/guilds/{guild_id}?with_counts=true
Response 200: { Guild Object with approximate_member_count, approximate_presence_count }

GET /api/v10/guilds/{guild_id}/members
Query: { limit: 1000 (max), after: user_id_snowflake }
Response 200: [ Guild Member Object array ]
Note: Requires GUILD_MEMBERS privileged intent for bots

POST /api/v10/guilds/{guild_id}/channels
Authorization: Bot <token> (MANAGE_CHANNELS permission required)
Request: { "name": "new-channel", "type": 0, "parent_id": "category_id", "position": 5 }
Response 201: { Channel Object }

PATCH /api/v10/channels/{channel_id}
Request: { "name": "renamed", "topic": "New topic", "slowmode_delay": 10 }
Response 200: { Channel Object }
```

#### Roles and Permissions

```
POST /api/v10/guilds/{guild_id}/roles
Request: {
  "name": "Moderator",
  "permissions": "8",         // permission bitmask as string
  "color": 3447003,
  "hoist": true,
  "mentionable": true
}
Response 200: { Role Object with role_id }

PUT /api/v10/guilds/{guild_id}/members/{user_id}/roles/{role_id}
Response 204  // Add role to member

DELETE /api/v10/guilds/{guild_id}/members/{user_id}/roles/{role_id}
Response 204  // Remove role from member

PATCH /api/v10/channels/{channel_id}/permissions/{overwrite_id}
Request: {
  "allow": "1024",   // allow VIEW_CHANNEL
  "deny": "2048",    // deny SEND_MESSAGES
  "type": 0          // 0=role overwrite, 1=member overwrite
}
Response 204
```

#### Slash Commands (Application Commands)

```
POST /api/v10/applications/{application_id}/guilds/{guild_id}/commands
Request: {
  "name": "poll",
  "type": 1,  // CHAT_INPUT (slash command)
  "description": "Create a poll",
  "options": [
    { "type": 3, "name": "question", "description": "Poll question", "required": true },
    { "type": 3, "name": "option1", "description": "First option", "required": true }
  ]
}
Response 201: { Application Command Object }

// Discord calls the bot's registered Interaction Endpoint:
POST <interactions_endpoint_url>
{
  "type": 2,  // APPLICATION_COMMAND
  "token": "interaction_token",
  "member": { "user": {...}, "roles": [...], "permissions": "..." },
  "channel_id": "C01234",
  "guild_id": "G01234",
  "data": {
    "name": "poll",
    "options": [{"name": "question", "value": "What's for lunch?"}]
  }
}
// Bot must respond within 3 seconds:
Response 200: {
  "type": 4,  // CHANNEL_MESSAGE_WITH_SOURCE
  "data": { "content": "Poll created!", "components": [...] }
}
// For deferred responses (type 5), bot has 15 minutes to follow up via API
```

### Gateway WebSocket Protocol (Discord Gateway API)

```
WSS wss://gateway.discord.gg/?v=10&encoding=json
(or etf for Erlang Term Format — more compact)

// Server → Client Opcodes:
OP 0  DISPATCH          // Event (MESSAGE_CREATE, GUILD_MEMBER_ADD, etc.)
OP 1  HEARTBEAT         // Server requests heartbeat
OP 7  RECONNECT         // Server forces reconnect
OP 9  INVALID_SESSION   // Invalid session (re-identify required)
OP 10 HELLO             // Initial handshake; contains heartbeat_interval (41250 ms)
OP 11 HEARTBEAT_ACK     // Ack for client heartbeat

// Client → Server Opcodes:
OP 1  HEARTBEAT         // Must send every heartbeat_interval ms
OP 2  IDENTIFY          // Initial auth + intent declaration
{
  "op": 2,
  "d": {
    "token": "Bot <token>",
    "intents": 33280,   // bitmask of subscribed event categories
    "properties": { "$os": "linux", "$browser": "discord.js", "$device": "discord.js" },
    "compress": false,
    "large_threshold": 50  // guilds above this size → member list loaded lazily
  }
}

OP 6  RESUME            // Resume after disconnect:
{ "op": 6, "d": { "token": "...", "session_id": "...", "seq": 42 } }

OP 8  REQUEST_GUILD_MEMBERS  // Request member list for large guild
OP 14 REQUEST_SOUNDBOARD_SOUNDS

// Key Dispatch Events (OP 0):
MESSAGE_CREATE, MESSAGE_UPDATE, MESSAGE_DELETE, MESSAGE_BULK_DELETE
REACTION_ADD, REACTION_REMOVE, REACTION_CLEAR
GUILD_MEMBER_ADD, GUILD_MEMBER_UPDATE, GUILD_MEMBER_REMOVE
CHANNEL_CREATE, CHANNEL_UPDATE, CHANNEL_DELETE
VOICE_STATE_UPDATE, VOICE_SERVER_UPDATE
PRESENCE_UPDATE, TYPING_START
INTERACTION_CREATE  // Slash command / button / select menu interactions
THREAD_CREATE, THREAD_UPDATE, THREAD_DELETE, THREAD_LIST_SYNC
```

---

## 6. Deep Dive: Core Components

### 6.1 Permissions System

**Problem it solves:**
Every message delivery, every channel access, every action in Discord is gated by a multi-layered permission system: guild-level role permissions + per-channel role overwrites + per-channel member overwrites. This must be evaluated in O(1) for 46,300 messages/sec (each requiring permission validation), for users who may have multiple roles.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **On-demand DB query** | Every action queries roles + overwrites tables | Always consistent | Multiple DB queries per message; too slow at 46 K msg/s |
| **In-memory guild cache** | Load entire guild (roles, channels, overwrites) into memory on first access | Single lookup for permission; fast | Memory: 19 M guilds × 50 KB avg = 950 GB — too large for single node |
| **Per-user-per-channel bitmask cache** | Compute and cache the final permission bitmask for (user, channel) in Redis | O(1) lookup; small cache entry (8 bytes bitmask); horizontally scalable | Cache invalidation on role change; cold start requires computation |
| **Permission-aware sharding** | Shard guild data such that all permission data for a guild is on one node | Avoids cross-shard permission lookups | Complex routing; hot guilds overload single node |
| **Event-driven cache invalidation** | Maintain permission cache; invalidate via Kafka events on role changes | Consistent with DB | Cache miss burst after large role change; eventual consistency window |

**Selected Approach: Per-user-per-channel bitmask cache in Redis with event-driven invalidation**

**Implementation Detail:**

*Permission computation (when cache misses):*
Discord's permission calculation algorithm (from official documentation):

1. Start with guild `@everyone` role permissions bitmask.
2. Apply (OR) permissions from all roles the member has, in any order (role order matters only for visual hierarchy, not permission accumulation — permissions are additive via bitwise OR).
3. If the user is the guild owner: all permissions granted immediately.
4. If the computed permissions include ADMINISTRATOR bit (bit 3): all permissions granted; skip overwrites.
5. Apply channel permission overwrites:
   a. Apply `@everyone` role overwrite for this channel (deny first, then allow).
   b. Apply all role-specific overwrites (OR all deny bits, then OR all allow bits, then apply).
   c. Apply member-specific overwrite (deny first, then allow).
6. Result: 64-bit permission bitmask for this user in this channel.

*Caching strategy:*
- Cache key: `perm:{channel_id}:{user_id}` → 8-byte bigint (permission bitmask).
- TTL: 5 minutes. Invalidated by events:
  - `MEMBER_ROLE_ADD` / `MEMBER_ROLE_REMOVE`: invalidate all channel permissions for this user in this guild: `DEL perm:{channel_id}:{user_id}` for all channels of the guild. Redis SCAN + DEL pattern or use a secondary index (`{guild_id}:{user_id}` → set of channel_ids to invalidate).
  - `ROLE_UPDATE`: invalidate all members who have this role. Can be expensive if role has 100 K members. Optimization: instead of per-member invalidation, store a `role_version` counter per guild. Permission cache key includes version: `perm:{channel_id}:{user_id}:{role_version}`. Role update increments `role_version` → all existing cache keys stale automatically (they have old version in key). No explicit DEL needed.
  - `CHANNEL_PERMISSION_OVERWRITE_UPDATE`: invalidate affected channel's cache for all users. Similarly, use `channel_perm_version:{channel_id}` counter in cache key.

*Cache warming:*
- On user connection, the Gateway prefetches and caches permissions for all channels in their current guild view (top 10 channels by activity). Cold start latency: permission computation from DB for 10 channels × 1 ms DB query = 10 ms — acceptable once per session.

*Numbers:*
- Cache size: 10 M concurrent users × avg 5 channels active = 50 M cache entries × 8 bytes = 400 MB. Trivial for Redis.
- Permission check per message: 1 Redis GET (< 0.1 ms). For 46,300 msg/s with permission check = 46,300 Redis GET/s — well within Redis cluster capacity.

**Interviewer Q&As:**

1. **Q: What happens if a role is deleted that had permissions in multiple channels? How do you maintain consistency?**
   A: Role deletion is an atomic operation in PostgreSQL (DELETE FROM roles WHERE role_id = X triggers CASCADE delete on member_roles and permission_overwrites for that role). The deletion event published to Kafka triggers: (1) Permission cache invalidation using the `role_version` increment strategy for the guild. All cached permissions computed with the old role are invalidated. (2) The Guild Fan-Out Service must re-evaluate which channels affected users can still access. For large guilds, this is a background async operation — users may briefly see incorrect channel access (eventual consistency window = cache TTL or up to 30 seconds with version-based invalidation). (3) Gateway sends `GUILD_ROLE_DELETE` event to all online guild members so clients can rerender the role UI. Clients rely on this event to re-request the guild object and update their cached permissions.

2. **Q: How does the ADMINISTRATOR permission bypass all other checks? What are the security implications?**
   A: The ADMINISTRATOR bit (value 8, bit 3) is checked after computing base permissions (step 4 in the algorithm above). If any role grants ADMINISTRATOR, the user gets all 64 permission bits set, bypassing all channel-specific denies. Security implication: granting ADMINISTRATOR to a bot or user is equivalent to making them owner-level. Discord does not allow the server owner's ADMINISTRATOR to be denied via channel overwrites — this is by design (owners can always access everything). The risk: if a bot with ADMINISTRATOR is compromised, an attacker can do anything in the guild. Discord's security guidance recommends scoping bot permissions to the minimum required. In the permission computation, after ADMINISTRATOR check, further overwrite processing is skipped — this is a short-circuit in the permission algorithm, not a security bypass.

3. **Q: Describe how you'd handle permission checking for 1,000,000 members in a mega-server receiving a message at the same time.**
   A: The Fan-Out Service needs to know which members can see the channel (have VIEW_CHANNEL permission). For a mega-server with 1 M members: (1) **Permission index**: maintain a Redis Set `channel_viewers:{channel_id}` containing all member user_ids who have VIEW_CHANNEL. Updated on join/role change events. Size: 1 M members × 8 bytes = 8 MB per channel — manageable. (2) **Fan-out to online viewers**: intersect `channel_viewers:{channel_id}` (Redis Set) with `guild_online:{guild_id}` (Redis Sorted Set) using `ZINTERSTORE` or a custom Lua script. Returns the online members with VIEW_CHANNEL access. (3) **For truly large channels** (all 1 M members can view): bypass the intersection — send to all online members (guild_online set) and let the client permission-check locally. Client has the guild's role data and can compute permissions client-side. This is how Discord handles large public guilds: the "lazy guild loading" approach means the server doesn't send full member lists, and permission enforcement is layered.

4. **Q: How do Discord's permission overwrites work for threads inside a channel?**
   A: Threads inherit the parent channel's permission overwrites by default. A private thread additionally restricts access to explicitly added members (`thread_members` table). Permission check for a thread: (1) Compute channel permissions for the parent channel (same as above). (2) If the thread is private: additionally check if `user_id` is in `thread_members` for this thread_id. If not, access denied regardless of parent channel permissions. (3) If the thread is public: only parent channel permissions apply — if user can see parent channel, they can see the thread. This layering is simple but creates an edge case: a user removed from a private thread can still read the parent channel; they just can't see the thread messages. The Gateway sends `THREAD_MEMBERS_UPDATE` to keep clients in sync.

5. **Q: How do you test and verify the permission system doesn't have logic bugs that could leak private channel content?**
   A: Permission bugs are critical (privacy breach). Testing strategy: (1) **Formal specification**: the permission algorithm is deterministic given a set of roles + overwrites. Write a reference implementation in Python (simple, auditable) and a property-based test suite (Hypothesis library) that generates random role/overwrite configurations and asserts the reference and production implementations agree. (2) **Permission matrix tests**: for each of 64 permission bits, automated test matrix covering: role grants, channel deny overwrite, ADMIN bypass, @everyone role, member-specific overwrite. (3) **Audit log**: every permission overwrite change is logged to an append-only audit log table. Compliance auditors can review. (4) **Red team**: quarterly penetration testing exercises focused on horizontal privilege escalation (accessing private channels via unexpected permission paths). (5) **Observability**: track `permission_denied` events; alert on anomalous spikes (could indicate a bypass).

---

### 6.2 Large Server (Guild) Scalability

**Problem it solves:**
A Discord server with 1 million members receiving a single message in `#general` must deliver it to potentially 500,000 online concurrent users within 100 ms p99. This is a 500,000× fan-out for a single write — the dominant scaling challenge in Discord's architecture.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Direct O(N) fan-out** | For each online member, push message | Simple | At 500 K members, 500 K gRPC calls → impossible within latency SLA |
| **Pub/Sub broadcast (Redis)** | Publish to guild channel; gateways subscribe | Fast; O(num_gateways) work | Redis Pub/Sub doesn't scale to 10 M subscriptions per channel well |
| **Tiered fan-out with batching** | Fan-out service groups members by gateway; 1 batch call per gateway | Reduces 500 K calls to ~500 calls (1 per gateway serving members) | Still O(num_online_gateways) calls from single service |
| **Lazy member loading (client pull)** | Don't send full member list; client subscribes to relevant member subsets | Dramatically reduces server state per connection | Complex client logic; not suitable for message delivery (still need push) |
| **Sharded fan-out workers** | Partition guild members across multiple fan-out worker shards; each shard fans out to its member subset | Parallelizes fan-out; O(1) per worker per member subset | Coordination overhead; partition rebalancing on scale-out |
| **Gateway-local subscription** | Each gateway subscribes to guild channels for its connected users | Fan-out is distributed across gateways | Subscription management at scale; 10 M connections × N guilds each = enormous subscription state |

**Selected Approach: Sharded Fan-Out Workers + Tiered Batching + Large Guild Lazy Loading**

**Implementation Detail:**

*Normal guilds (< 1,000 members):*
- Fan-Out Service fetches online member list from `guild_online:{guild_id}` Redis Sorted Set.
- Groups by gateway server. Issues gRPC batch pushes (1 call per gateway).
- Latency: Redis ZRANGEBYSCORE (~1 ms) + gRPC to ~5 gateways (~5 ms each, parallel) = ~6 ms total. Well within 100 ms SLA.

*Large guilds (1,000 – 100,000 members):*
- Online member count: 10 K – 50 K (50% online assumption).
- Sharded fan-out: assign guild_id to a consistent hash ring of fan-out worker shards. 16 fan-out shards, each handles 1/16 of the guild's online members.
- Each shard independently queries its member subset (Redis ZRANGEBYLEX with member_id ranges), groups by gateway, sends gRPC batches.
- Total: 16 parallel fan-out operations → 16× speedup. At 50 K online members spread across 200 gateways = 250 members/gateway, 16 shards each handling ~13 gateways = 13 batch gRPC calls per shard. Latency: ~2 ms (parallel) per shard + gRPC 5 ms = ~7 ms. Excellent.

*Mega-guilds (> 100,000 members, e.g., official game servers with 1 M members):*
- Discord introduced "lazy guild loading" for large guilds: the Gateway API does NOT send the full member list to clients on connection. Clients receive only online members they've interacted with recently (Discord's "member chunking").
- Server-side fan-out strategy: (1) Maintain a per-channel active subscriber set in Redis: `channel_subscribers:{channel_id}` — users who have the channel actively visible in their client (sent a `CHANNEL_OPEN` signal). This is a small subset (< 1,000 users scrolling #general at once). (2) For the broader delivery: use a push-to-all-gateways model. Each gateway server maintains a set of guild_ids for all its connected users. When a message event arrives for guild G, the fan-out publishes to all gateways that have at least one member of guild G. (3) Each gateway then filters: from all its connected users in guild G, check which have VIEW_CHANNEL for this channel → deliver. Local permission cache makes this O(1) per user.
- Gateway subscription registration: `SADD gateway_guilds:{gateway_server_id} {guild_id}` on user connect; `SREM` on disconnect. When fan-out publishes message for guild G, queries `gateway_guild_members:{guild_id}` to find relevant gateways (inverse index: per guild, which gateways have connected members).

*Numbers for mega-guild (1 M members, 500 K online):*
- Online users spread across 10 M / 10 M connections × 500 K = ... Let's calculate directly: 500 K online users / 20 K connections per gateway = 25 gateways have this guild's members.
- Fan-out: 1 Kafka message → Fan-Out Service reads guild-gateway map → sends to 25 gateway servers (25 gRPC calls, parallel) → each gateway checks its ~20 K connections for members of this guild with VIEW_CHANNEL → delivers to matching users.
- Per-gateway time: 20 K connections, maybe 500 are from this guild (500 K / 25 gateways / 20 K = 1%) = 200 users per gateway. Filter + push: 200 WebSocket writes per gateway = trivial (microseconds).
- Total delivery time: 1 Kafka consume (~5 ms) + Redis map lookup (~1 ms) + 25 gRPC calls parallel (~5 ms each) = ~11 ms. Well within 100 ms SLA.

**Interviewer Q&As:**

1. **Q: How does Discord handle the "gateway sharding" requirement for bots in large guilds?**
   A: Bots connecting to Discord must shard their Gateway connections when their bot is in > 2,500 guilds. Discord assigns the bot N shards based on guild count. Each shard handles `guild_id % N == shard_id` guilds. This is the bot's responsibility (Discord provides the shard count recommendation via `GET /gateway/bot`). For server-side implications: when Discord's Fan-Out Service routes events for a guild, it knows which shard a bot uses (from the bot's gateway session metadata). It routes events to the correct gateway server handling that shard. Discord's `READY` event includes `shard: [shard_id, num_shards]` so the bot knows which guilds to handle. This prevents a single bot process from receiving events for all guilds (would be O(all_guilds) × events).

2. **Q: During a major gaming event (new game launch), a mega-server might receive 10,000 messages per second. How do you handle this?**
   A: 10,000 msg/s in one channel of a 1M-member server = 10,000 × 500,000 potential deliveries/s = 5 billion delivery events/s from one server. This is clearly impossible to fan out. Discord applies rate limiting: (1) **Slowmode**: server admins can set slowmode (minimum seconds between messages per user). During events, Discord automatically suggests or enforces slowmode. At 10 user/s × 1 min slowmode per user, max throughput in a channel is bounded. (2) **Fan-out shedding**: if fan-out backlog grows beyond threshold (Kafka lag > 10 s), switch to "push to active subscribers only" mode — only users who have the channel open and have recently scrolled get real-time delivery. Other users get a "X new messages" indicator on reconnect. (3) **Client-side pull degradation**: Gateway sends a `CHANNEL_TOO_BUSY` notification; client switches to polling mode for that channel. (4) **Announcement channels**: for official game announcements, Discord uses GUILD_NEWS channel type which fans out to followers differently, with Discord's own rate limits applied.

3. **Q: How does Discord's voice channel work when 10,000 people join simultaneously (e.g., a stage event)?**
   A: Stage channels (OP 13 — GUILD_STAGE_VOICE) are designed for large audiences: only the "Stage Moderators" and invited speakers have audio; everyone else is an audience member (receive-only). This reduces the SFU load from O(N²) streams (full mesh) to O(N × speaker_count). Architecture: (1) Speaker audio → SFU receives → SFU forwards to all audience connections. At 10,000 audience members with 5 speakers: SFU forwards 5 streams × 10,000 recipients = 50,000 stream deliveries. (2) At 64 Kbps per audio stream: 5 streams × 10,000 × 64 Kbps = 3.2 Gbps egress from SFU. Large but a single SFU cluster handles this. (3) Multiple SFU servers in a region: load balanced by number of audience members; when a stage hits capacity, overflow to additional SFU nodes.

4. **Q: How do you prevent "member list" queries from DoS-ing the system for mega-guilds?**
   A: The GET `/guilds/{guild_id}/members` endpoint requires the `GUILD_MEMBERS` privileged intent and is heavily rate-limited (1 request / 1 second). For bots: chunked member fetching via OP 8 `REQUEST_GUILD_MEMBERS` (Gateway API), which paginated delivery in chunks of 1,000 members. Discord added `large_threshold` parameter to IDENTIFY (OP 2): guilds above this threshold don't send member lists proactively at connect time. Clients use OP 14 `REQUEST_GUILD_MEMBERS` to subscribe to specific member subsets (by user_id or query prefix for search). Member count is denormalized on the guild object for fast display. Online presence count uses a Redis HLL (HyperLogLog) approximation per guild for display without enumerating all members.

5. **Q: How does Discord handle the case where a user is in 100 servers and all 100 servers are active simultaneously?**
   A: The Gateway connection for a user delivers events for ALL guilds the user is in. On connect (IDENTIFY), Discord sends a `READY` event with all guild objects the user is in. For users in 100 guilds, `READY` payload could be large — Discord sends `READY` with "unavailable guild" stubs for large guilds and loads them incrementally via `GUILD_CREATE` events. The Gateway server holds the user's 100 guild subscriptions in memory: when any of those 100 guilds generates an event that affects this user, it's routed to this user's Gateway connection. This is manageable: most guilds are quiet most of the time. The Gateway's subscription table (guild_id → set of connected user_ids) is an in-memory hash on the Gateway server, updated on connect/disconnect. At 20,000 connections per gateway × avg 20 guilds per user = 400,000 guild memberships in memory per gateway — at ~20 bytes each = ~8 MB per gateway. Trivial.

---

### 6.3 Voice and Video (WebRTC Architecture)

**Problem it solves:**
Discord's voice channels must provide low-latency (<150 ms one-way), high-quality audio to N simultaneous participants, with no scheduling required (join/leave at any time), supporting up to 25,000 concurrent listeners per channel (Stage events) and regional routing to minimize latency.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Full mesh P2P (WebRTC)** | Each participant sends audio to every other participant | No server media relay; lowest latency | Upload bandwidth = (N-1) × audio bitrate; fails at N > 4; private IP traversal issues |
| **MCU (Multipoint Control Unit)** | Server mixes all audio into one stream; sends one stream to each participant | Minimal client bandwidth | High CPU for mixing; mixing introduces ~50-100ms latency; single point of failure |
| **SFU (Selective Forwarding Unit)** | Server receives each stream; forwards to other participants without mixing | No mixing latency; bandwidth efficient; scales to thousands | Client receives N streams (CPU); SFU must manage N×M forwarding |
| **SFU + Simulcast** | Senders transmit multiple quality layers; SFU forwards appropriate quality per receiver | Adapts to receiver bandwidth | Higher sender bandwidth; client-side complexity |
| **Cascaded SFUs** | Multiple SFU servers; streams cascade between them for large events | Scales to very large audiences | Added latency per cascade hop; complexity |

**Selected Approach: SFU (Mediasoup-compatible architecture) with regional deployment + Cascaded SFUs for Stage events**

**Implementation Detail:**

*Voice channel join flow:*
1. Client sends REST `PATCH /api/v10/guilds/{guild_id}/voice-states/@me` with `channel_id`.
2. REST API writes to Redis voice state hash: `voice_state:{guild_id}:{user_id}` with `channel_id`, `session_id`.
3. Kafka event `VOICE_STATE_UPDATE` published.
4. Gateway dispatches `VOICE_STATE_UPDATE` to all guild members (so they see the updated member list in the voice channel UI).
5. REST API also sends `VOICE_SERVER_UPDATE` event to the joining client via Gateway, containing:
   ```json
   { "token": "<session_token>", "guild_id": "...", "endpoint": "us-east-1.discord.media:443" }
   ```
   This tells the client which voice server to connect to.
6. Client connects to the regional voice server via WebRTC (UDP with DTLS-SRTP for media; HTTPS for signaling).

*SFU media flow:*
- Client negotiates WebRTC (SDP offer/answer) with the SFU via the voice server's HTTP signaling endpoint.
- Sends audio (Opus codec, 64 Kbps), video (VP8/VP9/H.264 via simulcast: 1080p, 720p, 360p layers).
- SFU receives audio from each participant (N streams) and forwards each to all other participants.
- For small channels (≤ 25 users): standard SFU with per-participant routing. Total streams: N×(N-1). At N=25: 600 forwarded streams. At 64 Kbps: 600 × 64 Kbps = 38.4 Mbps per SFU server. Well within 10 Gbps NIC capacity — one SFU server handles thousands of such small channels.
- For Stage channels: cascaded SFU — 5 speakers send to Root SFU → Root SFU forwards to Leaf SFU servers → Leaf SFUs forward to audience shards. At 25,000 listeners: Root to N Leaf SFUs, each Leaf serving 1,000 listeners. 5 streams × 25 Leaf SFUs = 125 stream forwards from Root. Each Leaf forwards 5 streams to 1,000 listeners = 5,000 stream deliveries per Leaf. At 64 Kbps: 5,000 × 64 Kbps = 320 Mbps per Leaf server. Feasible.

*Regional routing:*
- Voice servers deployed in 20+ regions (us-east, us-west, eu-west, eu-central, ap-southeast, etc.).
- When a user joins, Discord selects the optimal voice server based on: (1) user's geographic region (IP-based), (2) voice server load (connections, CPU, bandwidth). Announced in `VOICE_SERVER_UPDATE`.
- Participants in different regions: for a voice channel with users in US and Europe, Discord assigns a "hub" region (lowest total RTT sum across participants). Alternatively, users may notice slightly higher latency when the voice server is in a distant region.

*Numbers:*
- Concurrent voice users: 25 M.
- Avg voice channel size: 3 users (most are small friend groups).
- Channels: 25 M / 3 ≈ 8.3 M concurrent voice channels.
- SFU servers needed: at 5,000 concurrent voice channels per SFU server, need 8.3 M / 5,000 = 1,660 SFU servers globally.
- Audio bandwidth per SFU: 5,000 channels × avg 3 users × 2 streams × 64 Kbps = 1.92 Gbps — within 10 Gbps NIC.

**Interviewer Q&As:**

1. **Q: How does Discord handle NAT traversal for WebRTC voice connections?**
   A: WebRTC NAT traversal uses ICE (Interactive Connectivity Establishment): (1) **STUN**: clients send STUN requests to Discord's STUN server to discover their public IP:port. Discord runs STUN servers in all regions. (2) **TURN**: if direct P2P fails (symmetric NAT), use TURN (relay). Discord's voice servers double as TURN relays — all media passes through them anyway (SFU model), so NAT traversal is guaranteed. Clients don't need to do P2P hole-punching because the SFU is the only peer. The WebRTC data channel connects client → SFU, and SFU has a public IP. (3) **ICE candidates**: Discord's voice server provides its public IP as an ICE candidate. Client and server exchange SDP, agree on ICE candidate, connect via UDP (preferred) or TCP fallback. 99%+ of users successfully connect via UDP to the SFU.

2. **Q: What codec does Discord use for audio, and why?**
   A: Discord uses the **Opus codec** exclusively for audio. Technical properties that make it the right choice: (1) **Variable bitrate**: 6 Kbps (voice, low bandwidth) to 510 Kbps (audio sharing). Discord uses 64 Kbps for standard voice, 256 Kbps for music bots. (2) **Frame size flexibility**: 2.5 ms to 60 ms frames. Discord uses 20 ms frames for real-time voice (balance between latency and packetization overhead). (3) **Built-in FEC (Forward Error Correction)**: handles up to 15% packet loss without retransmission. Critical for UDP delivery over the internet. (4) **Built-in DTX (Discontinuous Transmission)**: encodes silence at 400 bps — dramatic bandwidth reduction when user is not speaking. (5) **SILK + CELT hybrid**: SILK for speech, CELT for music — automatically switches based on content. This is why Discord can both support gaming voice chat and music bot quality.

---

## 7. Scaling

**Horizontal Scaling:**
- **Gateway**: 500 servers at 20 K connections = 10 M connections. Auto-scale on connection count. Consistent-hash sticky routing on user_id. Rolling deployments send WebSocket close frames; clients reconnect seamlessly.
- **ScyllaDB cluster**: Shard-per-core model; each node handles multiple vCPUs as independent shards. Consistent hashing, RF=3. Linear throughput scale: add nodes → Scylla auto-rebalances (streaming). 200-node cluster at 40 TB SSD each = 8 PB raw, ~2.7 PB usable (RF=3).
- **PostgreSQL (guild metadata)**: 128 shards by guild_id. Each shard: 1 primary + 2 read replicas. Patroni for HA. Citus for online resharding. Read replicas absorb 90% of traffic (most ops are reads).
- **Fan-Out Workers**: Horizontally scaled Kafka consumers. Each instance handles a partition subset. Auto-scale on Kafka consumer lag metric.
- **Voice Servers**: Region-specific ASGs. Scale on concurrent channel connections. At peak: 1,660 SFU servers globally (as computed above). Auto-scale 30% headroom.

**DB Sharding:**
- ScyllaDB: partition key `(channel_id, bucket)` distributes naturally across nodes. Hot channels (popular servers) spread across nodes via consistent hashing. No single-node hotspot for any channel.
- PostgreSQL: sharded by `guild_id`. Guild + all its channels, roles, members collocated on one shard. Avoids cross-shard JOINs for guild operations.
- Message hot partitions: `channel_id` provides sufficient distribution (millions of channels). Weekly `bucket` key prevents unbounded partition growth. Old buckets are immutable once the week passes — TimeWindowCompactionStrategy SSTs them efficiently.

**Replication:**
- ScyllaDB: RF=3 across 3 AZs per region. QUORUM writes (2/3 agree), LOCAL_ONE reads (fast; stale reads acceptable for messages — 1-second staleness unnoticeable in chat). Cross-region: async replication.
- PostgreSQL: streaming replication, semi-synchronous to 1 replica. Failover < 30 s with Patroni.
- Redis: Redis Cluster with 1 primary + 2 replicas per slot. Online resharding with zero downtime (CLUSTER RESHARDING).

**Caching:**
- Guild metadata (roles, channels, overwrites): Redis, TTL 5 minutes. Invalidated on change.
- Permission bitmask: Redis, versioned key (includes role_version counter), TTL 5 minutes.
- Recent messages (last 50 per channel): Redis LRU. Serves history requests for active channels.
- Online member set per guild: Redis Sorted Set, updated by Gateway on connect/disconnect. This is the source of truth for presence fan-out.

**CDN:**
- All media (attachments, avatars, emojis, stickers) served via Cloudflare's CDN. Immutable content (content-addressed by hash). Cache-Control: public, max-age=604800. CDN absorbs >95% of media reads.

### Interviewer Q&As on Scaling

1. **Q: ScyllaDB uses shard-per-core. How does this change operational behavior compared to Cassandra?**
   A: In Cassandra (JVM), all cores share a thread pool and JVM heap. GC pressure from concurrent compaction and query processing causes stop-the-world pauses (even with G1GC) — Discord observed 30+ second pauses under heavy load. ScyllaDB's shard-per-core model: each CPU core runs an isolated Seastar reactor with its own memory arena and thread. No cross-core locking for read/write operations; each core owns a portion of the token ring. Benefits: (1) No JVM GC — memory managed manually with C++ RAII. (2) Linear CPU scalability — adding cores linearly increases throughput. (3) Predictable tail latency — one core's compaction doesn't affect other cores' queries. (4) Operationally: CPU utilization is the primary scaling metric (vs. heap size and GC pause time in Cassandra). Discord's migration reduced p99 latency from seconds (Cassandra under GC pressure) to < 5 ms.

2. **Q: How does Discord handle the database migration for trillion-scale message history?**
   A: Discord's actual migration (Cassandra → ScyllaDB) used a dual-write strategy: (1) For several months, all writes went to both Cassandra and ScyllaDB. (2) Reads served from Cassandra (source of truth). (3) Background job bulk-migrated historical data from Cassandra to ScyllaDB using the Cassandra → ScyllaDB migration tool (SSTLoader). (4) Once ScyllaDB caught up and data was validated (row count, spot checks), traffic was shifted: reads first to ScyllaDB, then Cassandra writes dropped. (5) Historical data not in ScyllaDB yet (before the migration start date): served from Cassandra cold path. (6) Zero downtime by virtue of dual-write. Total migration time: several months for trillion-row datasets.

3. **Q: How do you scale the Elasticsearch search cluster for Discord's message volume?**
   A: At 4 B messages/day and 1.17 PB/year, full search indexing of all messages is prohibitively expensive. Discord's approach (based on available information): (1) **Selective indexing**: only messages from opted-in servers or servers with search feature enabled (Nitro / Community servers). Not all 4 B messages/day indexed — reduces volume by ~80%. (2) **Per-server index**: isolated ES indices per server, or shared ES cluster with per-guild routing keys. (3) **Index lifecycle**: recent 90 days in hot shards (NVMe SSDs); older in warm/cold phases (HDD-backed or searchable snapshots). (4) **Query scope**: Discord's search UI scopes to a single server and optionally a single channel. Query hits only the server's index. (5) **Scale**: 10-node ES cluster per large server (100+ M messages); shared 20-node clusters for small servers.

4. **Q: How do you handle the "thundering herd" when Discord releases a major update and 10 million users reconnect within 5 minutes?**
   A: 10 M reconnects / 300 s = 33,333 reconnections/second. Mitigations: (1) **Jitter in client reconnect**: Gateway close sends a `resume_gateway_url` — clients that can resume (have a valid session_id + sequence) reconnect to the same gateway URL with jitter (random 0-5 second delay). (2) **Session resumption**: resumable sessions don't need a full `READY` payload (no guild object resend). Only missed events (sequence-based) are replayed. Reduces payload by 90%. (3) **Gateway capacity**: 500 gateways × 20 K connection capacity = 10 M connections. An update triggers graceful close + reconnect; rolling gateway restarts ensure not all 500 go down simultaneously. (4) **Rate limiting reconnects at LB**: NLB can throttle new TCP connections (SYN cookies, connection rate limits) to protect backend during storm. (5) **Pre-warming**: Discord pre-scales the Gateway ASG before planned update rollouts.

5. **Q: How would you design sharding for the PostgreSQL guild metadata database if a single shard hosts a 1 million-member guild?**
   A: A guild with 1 M members has `guild_members`: 1 M rows × ~200 bytes = 200 MB. With roles: 1 M rows in `member_roles` × ~50 bytes = 50 MB. Plus `roles`, `channels`, `permission_overwrites`: ~100 MB. Total: ~350 MB per mega-guild — small enough to fit on a single PostgreSQL shard (shard has 50+ GB of RAM, handles 100s of guilds). The challenge is write throughput: if 10,000 users join/leave this guild per second during an event, that's 10,000 INSERT/DELETE per second on `guild_members` on one shard. PostgreSQL can handle ~5,000-10,000 writes/sec on a modern primary. At the limit: (1) Use PostgreSQL's UNLOGGED tables for join events (high write, low durability requirement). (2) Batch member join/leave events into PostgreSQL using mini-batches (100 rows per transaction vs 1 per transaction). (3) Vertical scale the shard to a larger instance. (4) Cache membership in Redis (Redis Set `guild_members:{guild_id}`); write-through to PostgreSQL asynchronously. The Redis set is authoritative for online operations; PostgreSQL is the durable record.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| Gateway server crash | ~20K users disconnected | TCP RST; health check 10 s | Clients reconnect via session resume; offline events replayed from Kafka (sequence-based) |
| ScyllaDB node failure | 1/3 replicas offline for token range | ScyllaDB gossip < 5 s | RF=3: 2 nodes serve QUORUM; repair stream from other replicas; hinted handoff during downtime |
| PostgreSQL shard primary failure | Guild writes fail for 30 s | Patroni health check | Patroni promotes replica; PgBouncer re-routes connections; writes in-flight return 503 + retry |
| Kafka broker failure | Events may delay | Kafka controller < 30 s | RF=3; leader election < 30 s; consumers reconnect and resume from last committed offset |
| Voice server failure | Active voice sessions drop | Health check; session timeout | Clients auto-reconnect to new voice server (Gateway sends new VOICE_SERVER_UPDATE); brief audio interruption (2-5 s) |
| Redis cluster failure | Presence, cache, voice state unavailable | Redis Cluster FAIL detection < 5 s | Replica promotion; cache cold-start from PostgreSQL; presence falls back to "unknown" |
| Fan-out service overload | Message delivery delayed | Kafka consumer lag alert | Auto-scale fan-out workers; large guild fan-out switches to degraded mode |
| CDN PoP failure | Media downloads fail for a region | Cloudflare monitoring | Cloudflare automatically routes to next nearest PoP; CDN redundancy built-in |
| Total region failure | All services in region unavailable | CloudWatch alarms + Route53 health checks | DNS failover to adjacent region; users in that region experience higher latency (cross-region) but service available |

### Idempotency

- **Message send**: Snowflake message_id generated by server before ScyllaDB write. Same `nonce` (client-generated) detected via Kafka dedup set (Redis, 60-second TTL). If duplicate nonce from same client, return existing message.
- **Reaction add**: ScyllaDB primary key `(channel_id, message_id, emoji_id, emoji_name, user_id)` — duplicate reaction insert is idempotent (upsert).
- **Voice state update**: Idempotent PUT operations; latest state wins (LWW in Redis).
- **Gateway event delivery**: Sequence numbers allow clients to detect and ignore duplicate events replayed during resume.

### Circuit Breaker

- Gateway → ScyllaDB writes: circuit opens if write latency p99 > 2 seconds over 10 seconds. Fallback: write to a Kafka "retry" topic; background consumer drains when ScyllaDB recovers. Client receives the message (optimistic delivery) but DB write is async.
- Fan-out → Gateway gRPC: circuit opens on 50% error rate per gateway server. Fallback: route to offline queue.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `gateway.connections.active` | Gauge | > 90% per server capacity |
| `message.delivery.latency_p99` | Histogram | > 100 ms |
| `scylladb.write.latency_p99` | Histogram | > 5 ms |
| `scylladb.read.latency_p99` | Histogram | > 10 ms |
| `kafka.consumer_lag.fanout` | Gauge | > 50,000 messages |
| `voice.active_channels` | Gauge | Track against SFU capacity |
| `voice.session.connect_time_p99` | Histogram | > 3 s |
| `permission_cache.hit_rate` | Counter Rate | < 90% (indicates invalidation storm) |
| `fanout.delivery_latency_p99` | Histogram | > 80 ms |
| `postgres.replication_lag_s` | Gauge | > 10 s |
| `redis.memory_used_pct` | Gauge | > 85% |
| `guild_online_member_count` | Gauge per large guild | Anomaly detection (sudden drop = mass disconnect) |
| `search.query_latency_p99` | Histogram | > 1 s |
| `cdn.error_rate` | Counter Rate | > 0.1% |

### Distributed Tracing

OpenTelemetry with Jaeger. Trace context propagated via HTTP headers and Kafka message headers. Key spans:
- `gateway.receive_event`
- `permission.compute_or_cache_hit`
- `kafka.publish`
- `fanout.resolve_online_members`
- `gateway.push_batch`
- `scylladb.write_message`
- `elasticsearch.index`
- `voice.webrtc_negotiate`

Sampling: 100% errors; 0.01% successes (at 231 K msg/s, 0.01% = 23 traces/sec).

### Logging

Structured JSON. Fields: `trace_id`, `guild_id`, `channel_id`, `user_id` (hashed), `event_type`, `latency_ms`, `error_code`. Aggregated via Fluentd → Kafka → Elasticsearch. Retention: 7 days hot, 30 days cold.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Why Chosen | What You Give Up |
|---|---|---|---|---|
| Message DB | ScyllaDB | Cassandra, PostgreSQL | 10× lower tail latency; no JVM GC; shard-per-core; same CQL API as Cassandra | Smaller community; newer; operational expertise cost |
| Permission model | Role bitmask + Redis cache + version-based invalidation | DB query on every check | O(1) permission check; scales to 46 K msg/s | Eventual consistency on permission change (seconds); stale cache during invalidation storm |
| Fan-out for large guilds | Sharded workers + gateway-local delivery | Direct O(N) fan-out | Parallelized; gateway-local filter reduces N dramatically | Complexity; partition rebalancing; subscription state management |
| Voice architecture | SFU (regional) + cascaded for Stage events | Full mesh P2P | Scales to N>4; no client upload amplification; Discord controls media path | Discord's servers relay all voice (bandwidth cost); SFU is SPOF (mitigated by redundancy) |
| Snowflake IDs | Time-ordered 64-bit integers | UUID v4, DB auto-increment | Time-ordered (enables cursor pagination without separate ts column); encodes worker info; globally unique without coordination | Reveals approximate creation time of any entity (minor privacy concern for users) |
| Session resumption | Sequence number + resume URL | Full reconnect on disconnect | Minimizes reconnect payload (only missed events replayed); reduces thundering herd | Client must maintain sequence state; server must buffer missed events per session (Kafka replay) |
| E2EE | None (server can read messages) | Signal Protocol | Enables moderation (AutoMod, spam detection, trust & safety); search indexing; compliance exports | User privacy: Discord can read all messages; potential for data breach |
| Lazy guild member loading | Client pulls member chunks on demand | Push full member list on connect | Reduces READY payload for mega-guilds from MB to KB; reduces initial connection time | Client doesn't have full member list; autocomplete and @mention must request chunks |

---

## 11. Follow-up Interview Questions

1. **Q: How does Discord's AutoMod work technically, and how do you scale it without blocking message delivery?**
   A: AutoMod is an async post-processing pipeline: (1) Message written to ScyllaDB and event published to Kafka. Message is immediately delivered to channel members. (2) AutoMod Kafka consumer reads message event. (3) Applies rule engine: keyword match (regex/literal), mention spam detector (count @mentions in message), link spam detector (URL domain blocklist), ML-based hate speech classifier (transformer model inference — ~50ms). (4) If violation: delete message (REST API call), optionally timeout the user, post to a designated log channel. (5) Async nature means AutoMod takes 100-500ms; users may briefly see a message before it's deleted. This is acceptable — blocking message delivery for AutoMod would increase latency unacceptably. For zero-tolerance rules (e.g., CSAM detection using PhotoDNA hash matching): hash checked synchronously at upload time before media URL is returned.

2. **Q: How would you implement Discord's "slash command" interaction model with under-3-second response time guarantee?**
   A: Slash command flow: (1) User types `/command` → client sends interaction to Discord's API. (2) Discord validates command (registered in application commands table). (3) Discord forwards interaction payload to the bot's registered Interactions Endpoint URL via HTTP POST. (4) Bot must respond within 3 seconds (Discord's webhook timeout). If the operation takes longer, bot should respond with `type: 5` (DEFERRED_CHANNEL_MESSAGE_WITH_SOURCE) immediately, which shows a "thinking..." indicator. Then, within 15 minutes, bot calls `POST /webhooks/{application_id}/{interaction_token}` to send the real response. This deferred model decouples the response time SLA (3s for ack) from the processing time SLA (15 min for actual response). Implementation on bot side: async worker picks up the deferred task; uses the `response_url` / webhook to follow up. Discord rate limits follow-up messages (5 per interaction token).

3. **Q: How does Discord handle GDPR "right to be forgotten" requests for a user who sent millions of messages?**
   A: GDPR deletion is complex at Discord's scale. Steps: (1) Account deletion requested. (2) User's email/phone/personally-identifiable profile fields zeroed out immediately (PostgreSQL update). Username replaced with a deleted user placeholder. (3) Message content: Discord's approach is not to delete individual messages for GDPR — messages are authored content posted in others' conversations, and deleting them would degrade the experience for all channel members. Instead, Discord anonymizes: the `author_id` on messages is replaced with a "deleted user" account ID (not the real user_id). The message content remains (it was published to a shared space). (4) DMs: messages in DM channels deleted from both parties' views. (5) Legal basis: Discord argues messages in guild channels are not purely personal data under GDPR (they're contributions to shared discourse). This is a legally contested area. (6) Data export (GDPR Article 20 right to portability): Discord provides a data export within 30 days — a Kafka + ScyllaDB query job that collects all messages where `author_id = user_id` across all channels.

4. **Q: How do bots receive events at scale — what's the difference between Gateway intents and HTTP event subscriptions?**
   A: **Gateway Intents**: bots connect via WebSocket (Gateway API) and declare which event categories (intents) they subscribe to. Intents bitmask: e.g., `GUILDS (1)`, `GUILD_MESSAGES (512)`, `DIRECT_MESSAGES (4096)`. Privileged intents (`GUILD_PRESENCES`, `GUILD_MEMBERS`) require explicit enablement in Developer Portal. Benefits: real-time, push-based, stateful (resume on reconnect). Downside: bot must maintain WebSocket connection; large bots must shard (OP 2 IDENTIFY with `shard` param). **HTTP event subscriptions (Interactions Endpoint)**: for slash commands and component interactions only. Discord calls the bot's registered HTTPS URL per interaction. Bot is stateless (no WebSocket). Benefits: no connection management; scales horizontally (each invocation is independent HTTP request). Downside: 3-second response SLA; can't receive ambient events (message creates, reactions) — only explicit interactions. Most production bots use Gateway for event-driven behavior and optionally an Interactions Endpoint for slash commands.

5. **Q: How would you design Discord's "Server Insights" analytics dashboard for large servers?**
   A: Server Insights shows: message volume over time, member growth, top channels, peak activity hours. Implementation: (1) **Write path**: every message event (Kafka) consumed by an Analytics Service. Writes time-bucketed counters to a time-series database (ClickHouse or TimescaleDB): `INSERT INTO channel_stats (guild_id, channel_id, bucket_hour, message_count) VALUES (...)` with ON CONFLICT DO UPDATE (increment). (2) **Aggregation**: pre-aggregate hourly → daily → weekly in scheduled jobs. Stored in summary tables. (3) **Read path**: dashboard queries summary tables via HTTP API. ClickHouse can handle complex aggregation queries over billions of rows in seconds due to columnar storage and vectorized execution. (4) **Member growth**: derived from `guild_member_joins` time-series (separate counter per guild per day). (5) **Privacy**: analytics aggregated; individual user-level data not exposed in dashboard (only admins of large Community servers with >= 500 members can access). (6) **Scale**: ClickHouse cluster of 10 nodes handles Discord's analytics volume with columnar compression reducing storage 10×.

6. **Q: How does Discord's message search differ from Slack's? What are the key differences in implementation?**
   A: Key differences: (1) **Scope**: Discord search is scoped to a single server (guild) by default; Slack search scopes to a workspace but can filter by channel. Discord's indexing is per-server; Slack's is per-workspace. (2) **Access control**: Discord's permission system is more complex (role + channel overwrite hierarchy). Elasticsearch documents must be updated when permission overwrites change — an event-driven update more complex than Slack's flat channel membership model. (3) **Indexing coverage**: Discord doesn't index all guilds (scale would be prohibitive for free servers). Search is a feature available to Community servers and Nitro-boosted servers at certain tiers. (4) **Message types**: Discord has more message types (system messages for joins/leaves/pins, thread create events, voice join events) — many excluded from text search by default. (5) **Latency**: Discord's search latency target is slightly more relaxed (1s p99) vs Slack's 500ms p99, reflecting gaming-community vs. enterprise product differences.

7. **Q: How does Discord handle message pinning and what are the concurrency implications?**
   A: Pins are stored as a list per channel (not a separate table — in practice, `pinned = true` flag on the message row in ScyllaDB). Limited to 50 pins per channel (product limit). Operations: (1) `POST /channels/{channel_id}/pins/{message_id}` — updates message row: `UPDATE messages SET pinned = true WHERE channel_id = ? AND bucket = ? AND message_id = ?`. (2) `GET /channels/{channel_id}/pins` — ScyllaDB secondary index on `pinned = true` per channel, OR a separate `channel_pins` table in PostgreSQL with (channel_id, message_id) for faster enumeration (avoids full channel scan). Concurrency: two simultaneous pin requests could both succeed (idempotent — pin is a boolean set). Unpin is similarly idempotent. The 50-pin limit enforced by the API layer (count before insert; if at limit, return error). Count is cached in Redis per channel: `pin_count:{channel_id}`. Increment/decrement on pin/unpin with floor at 0. Invalidated on cache miss.

8. **Q: Describe Discord's Nitro subscription model's technical implications.**
   A: Nitro changes several technical limits that affect system behavior: (1) **File upload limit**: Free = 10 MB, Nitro Basic = 50 MB, Nitro = 500 MB. The Media Service validates file size against the user's Nitro tier (fetched from user record, cached). Large file uploads go through multipart upload with 10 MB chunks to S3. (2) **Video quality**: Nitro users can stream at 1080p 60fps vs 720p 30fps for free. SFU must handle higher bitrate streams (up to 8 Mbps for 1080p vs 2 Mbps for 720p). Per-session bitrate cap enforced by SFU based on user's Nitro tier (fetched from a session-level auth token claim). (3) **Custom emoji in any server**: Nitro users can use emojis from any server in any chat. Emoji validation at message send time checks if the user has Nitro (flag in user record). (4) **Server boosts**: affect server-level limits (max bitrate in voice channels, max emoji count, max file size for members). Server boost tier stored on guild object; checked at API validation layer.

9. **Q: How does Discord handle the "netsplit" problem — when two data centers temporarily lose connectivity?**
   A: A netsplit (network partition between two Discord DCs) can cause split-brain scenarios. Mitigations: (1) **ScyllaDB**: LOCAL_QUORUM writes require 2/3 replicas in local DC — not affected by cross-DC partition. Async cross-DC replication pauses; resumes on reconnect. (2) **PostgreSQL**: single primary per shard (primary is in one DC). If the primary DC loses connectivity to the replica DC, the replica should NOT auto-promote (split-brain prevention). Patroni uses Raft or etcd for consensus; the replica DC won't promote without quorum, so writes fail (better than split-brain). (3) **Gateway sessions**: users on DC1 can still use services in DC1. Cross-DC dependencies (guild metadata shards in DC2 unreachable from DC1) return 503 for those guilds. (4) **Presence and voice**: per-DC Redis; cross-DC presence replication pauses. Some users may show as offline in the other DC during partition. (5) **Recovery**: partition heals → Kafka cross-DC replication drains backlog → ScyllaDB async replication catches up → eventual consistency restored.

10. **Q: What is the Snowflake ID scheme and why does Discord use it over UUID?**
    A: Discord Snowflakes are 64-bit integers encoding: bits [63-22] = milliseconds since Discord epoch (Jan 1, 2015 UTC = 1420070400000), bits [21-17] = internal worker ID (0-31), bits [16-12] = process ID (0-31), bits [11-0] = increment per process per millisecond (0-4095). Max rate: 4,096 IDs/ms/process = 4.096 M IDs/sec/process. Advantages over UUID v4: (1) **Time-ordered**: newer IDs are numerically larger → cursor-based pagination without a separate timestamp column. `WHERE message_id > last_id ORDER BY message_id ASC` is an efficient index scan. (2) **Compact**: 8 bytes vs 16 bytes for UUID. Significant at trillion-scale: 500 B rows × 8 bytes saved = 4 TB storage reduction in IDs alone. (3) **Human-readable time extraction**: `snowflake_to_ts = (snowflake >> 22) + DISCORD_EPOCH`. (4) **Sortable in string form**: when serialized as a decimal string (Discord's API uses strings to avoid JS float precision issues with 64-bit ints), alphabetical order == time order for same-length strings. Disadvantage: reveals creation time (minor privacy concern).

11. **Q: How does Discord's Channel following (cross-post / announcement channels) work at scale?**
    A: Guild Announcement channels allow server admins to "publish" messages, which are cross-posted to follower channels in other servers. Architecture: (1) Server A publishes a message in its Announcement channel. (2) Discord creates "followed channel" relationships stored in `channel_follows` table: `(source_channel_id, destination_channel_id, destination_guild_id)`. (3) On publish (`POST /channels/{channel_id}/messages/{message_id}/crosspost`): a Crosspost Service reads all followed destinations for this channel (could be tens of thousands). (4) For each destination: creates a new message in the destination channel (with `CROSSPOSTED` flag and `message_reference` pointing to the original). (5) Each destination message triggers normal fan-out to that channel's members. Scaling: crosspost could create thousands of messages from one publish. Rate limit: Discord limits crosspost to 10 per hour per channel. At 10/hour, even with 10,000 followers, that's 100,000 message creates/hour = 28/second from one source channel. Manageable.

12. **Q: How would you add end-to-end encryption to Discord without breaking bots, search, and moderation?**
    A: This is fundamentally incompatible with Discord's current feature set — and that's the correct answer. E2EE with server-opacity would break: (1) **Bots**: bots receive message content via Gateway API — if encrypted, bots can't read commands. Would require bots to also be in the Signal key exchange, which is architecturally complex and breaks the stateless bot model. (2) **Search**: Elasticsearch indexes plaintext — encrypted content can't be indexed. Client-side search only, no cross-device sync of search index. (3) **AutoMod / content moderation**: server can't detect hate speech or CSAM in encrypted messages. Would require client-side scanning (controversial: "on-device AI scanning"). (4) **Compliance**: enterprises need message retention for legal holds — E2EE prevents server-side export. **Hybrid approach** (Discord could theoretically implement): opt-in E2EE for DMs only (like iMessage). Group channels remain unencrypted for bot/moderation compatibility. But Discord has not implemented E2EE and their public stance is that server-side moderation is a core product value.

13. **Q: How do you handle emoji uploads and storage at Discord's scale?**
    A: Discord supports custom server emojis (up to 50 per server for free; up to 750 for boosted servers). That's 19 M servers × avg 25 emojis = 475 M custom emojis stored. (1) **Upload**: `POST /guilds/{guild_id}/emojis` with base64 image data. API validates: max 256 KB, PNG/GIF/WEBP only, max 128×128 px. Image processed by Media Service: resize to 28×28 and 56×56 (for 2× DPI), store to S3/R2 with content-addressed key (SHA-256 hash of pixels). (2) **CDN**: emoji served via CDN (`cdn.discordapp.com/emojis/{emoji_id}.png`). Immutable content (emoji images can't change — only delete+recreate). CDN cache: public, max-age=604800. (3) **Storage**: 475 M emojis × avg 5 KB (two resolutions) = ~2.4 TB. Small. (4) **Animated emojis**: GIF processing — transcode to WebP animated for size reduction. Requires ImageMagick/ffmpeg pipeline. (5) **Emoji lookup**: `GET /guilds/{guild_id}/emojis` fetches all guild emojis (from PostgreSQL guild shard, cached in Redis for 1 hour). In messages, emoji referenced by `{emoji_name}:{emoji_id}` — client renders by fetching from CDN URL derived from emoji_id.

14. **Q: How does Discord keep the "member list" sidebar updated in real-time for a server with 100,000 members?**
    A: The sidebar shows online members grouped by role (hoisted roles). For servers with > `large_threshold` members (default 50), Discord uses **lazy member loading**: (1) Client does NOT receive the full member list in `READY`. (2) Client sends OP 14 (`REQUEST_GUILD_MEMBERS` equivalent) to subscribe to a "view range" — the first 100 members visible in the sidebar. (3) Gateway sends `GUILD_MEMBER_LIST_UPDATE` events as presence changes within the subscribed range. (4) As user scrolls, client subscribes to additional ranges. (5) For role sections: Discord sends the member counts per role section, not all members. Sections expanded by user request. This "progressive disclosure" model means the gateway server only needs to track which member list ranges each connection has subscribed to (stored in-memory per gateway connection), not push all 100,000 members. Presence updates for members outside subscribed ranges are batched and sent as count updates, not individual events.

15. **Q: Describe a scenario where Discord's design could cause cascading failures and how you would prevent it.**
    A: Scenario: A viral game launch causes a 1M-member official server to receive 50,000 messages in 10 minutes. The Fan-Out Service creates 50,000 × 500,000 (all online) = 25 billion delivery events in 10 minutes. Kafka consumer lag explodes. Redis connection registry (channel subscriber sets) hit with 25 B lookups. Gateway servers overloaded with WebSocket writes. Cascading failure: (1) Redis latency spikes → permission cache misses flood PostgreSQL → PostgreSQL overwhelmed → guild metadata queries fail → all API requests fail for the affected guild. Prevention: (1) **Per-channel message rate limiting**: max 10 messages/second in a single channel, enforced at the Gateway before Kafka publish. Server owner can increase limit. (2) **Fan-out circuit breaker**: if Kafka lag > 60 seconds, switch to "critical delivery only" mode — only deliver @mention messages; all others queued. (3) **Slowmode enforcement**: Discord auto-suggests slowmode when channel receives > 5 messages/second. (4) **Dedicated mega-guild infrastructure**: large guilds isolated to dedicated Gateway and Fan-Out server pools so their load doesn't cascade to other guilds. (5) **Back-pressure propagation**: if Gateway's outbound queue exceeds threshold, it signals Fan-Out to slow delivery (rate signal via gRPC streaming).

---

## 12. References & Further Reading

- **Discord Engineering Blog — "How Discord Stores Trillions of Messages"**: https://discord.com/blog/how-discord-stores-trillions-of-messages
- **Discord Engineering Blog — "How Discord Supercharges Network Disks for Extreme Low Latency"**: https://discord.com/blog/how-discord-supercharges-network-disks-for-extreme-low-latency
- **Discord Engineering Blog — "Migrating Billions of Messages"**: https://discord.com/blog/migrating-billions-of-messages
- **ScyllaDB Documentation**: https://docs.scylladb.com/
- **Seastar Framework (ScyllaDB's async core)**: https://seastar.io/
- **Discord Developer Documentation — Gateway**: https://discord.com/developers/docs/topics/gateway
- **Discord Developer Documentation — Permissions**: https://discord.com/developers/docs/topics/permissions
- **Discord Snowflake IDs**: https://discord.com/developers/docs/reference#snowflakes
- **WebRTC Specification**: W3C / IETF. https://webrtc.org/
- **Opus Codec**: IETF RFC 6716. https://www.rfc-editor.org/rfc/rfc6716
- **Mediasoup SFU**: https://mediasoup.org/
- **TimeWindowCompactionStrategy (Cassandra/ScyllaDB)**: https://docs.scylladb.com/stable/cql/compaction.html
- **Cloudflare R2**: https://developers.cloudflare.com/r2/
- **DTLS-SRTP (WebRTC Security)**: IETF RFC 5764. https://www.rfc-editor.org/rfc/rfc5764
- **ICE (Interactive Connectivity Establishment)**: IETF RFC 8445. https://www.rfc-editor.org/rfc/rfc8445
