# System Design: Slack

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Workspace and channel model** — Organizations (workspaces) contain channels (public and private). Users belong to one or more workspaces and join channels within them.
2. **Real-time messaging** — Messages sent in a channel delivered to all online channel members in near real-time via WebSocket.
3. **Message threading** — Any message can have a thread attached. Thread replies are a sub-conversation; thread participants notified on new replies.
4. **Direct messages (DMs)** — 1:1 and group DMs (up to 9 participants) between workspace members.
5. **Message search** — Full-text search across all messages a user has access to, within a workspace. Results ranked by relevance and recency.
6. **File and media sharing** — Upload files up to 1 GB. Preview generation for images, PDFs, and code files. Snippets for code.
7. **Slash commands** — `/invite`, `/remind`, `/giphy`, `/poll`, etc. Custom slash commands via API.
8. **App integrations and webhooks** — Incoming webhooks (external services post messages to channels). Outgoing webhooks (Slack posts to external services on message events). OAuth 2.0 app installation.
9. **Message retention policies** — Workspace admins set retention: keep forever, 30 days, 90 days, 1 year. Compliance export for paid tiers.
10. **Notifications** — Desktop, mobile (APNs/FCM), and email notifications based on user preferences and @mention keywords.
11. **Reactions** — Users add emoji reactions to messages; reaction counts shown inline.
12. **User presence** — Active, Away, Do Not Disturb, custom status.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Availability | 99.99% (≤ 52 min/year) |
| Message delivery latency (p99) | < 200 ms to online members |
| Search latency (p99) | < 500 ms for most queries |
| Consistency | Strong within a channel (ordered delivery); eventual for reactions/presence |
| Scale | 20 M DAU; 10 B messages stored; ~1 M concurrent WebSocket connections |
| Storage | Petabyte-scale for messages + files |
| Security | TLS in transit; AES-256 at rest; optional Enterprise Key Management (EKM) |

### Out of Scope

- Slack Huddles / audio/video calls (WebRTC media plane).
- Slack Connect (cross-workspace channels) — noted but not detailed.
- Workflow Builder (low-code automation).
- Slack AI / summarization features.
- SCIM/SSO enterprise provisioning.

---

## 2. Users & Scale

### User Types

| Type | Description |
|---|---|
| **Member** | Standard workspace member; send/read messages, join channels |
| **Workspace Admin** | Manage members, channels, retention policies, integrations |
| **Workspace Owner** | Billing, workspace deletion, full admin |
| **Guest** | Limited access: single-channel guest or multi-channel guest |
| **Bot / Integration** | Programmatic access via OAuth token; posts messages, responds to events |

### Traffic Estimates

**Assumptions:**
- 20 M DAU (Slack's reported ~20 M DAU as of 2023).
- Average session: 8 hours/day (mostly passive; desktop app open all day).
- Messages sent: 20 M users × 40 messages/user/day = 800 M messages/day.
- Average channel membership: 50 members; 50% online at any time → 25 online recipients per message.
- Reactions: 5 reactions/message on average, but bursty; total reaction events ≈ 4 B/day.
- File uploads: 10% of messages include a file → 80 M file upload events/day; avg file size = 500 KB (mix of screenshots and documents).
- Search queries: 5 searches/user/day → 100 M searches/day.
- WebSocket connections: 20 M DAU × 50% peak simultaneous = 10 M concurrent connections.
- Peak-to-average: 4x (office hours in a single timezone spike much harder than WhatsApp).

| Metric | Calculation | Result |
|---|---|---|
| Messages/day | 20 M × 40 | 800 M/day |
| Messages/sec (avg) | 800 M / 86,400 | ~9,260 msg/s |
| Messages/sec (peak) | 9,260 × 4 | ~37,000 msg/s |
| Fan-out deliveries/sec (avg) | 9,260 × 25 | ~231,500 deliveries/s |
| Fan-out deliveries/sec (peak) | 231,500 × 4 | ~926,000 deliveries/s |
| File uploads/day | 80 M | 80 M/day |
| File upload throughput (avg) | 80 M × 500 KB / 86,400 | ~463 MB/s |
| Search queries/sec (avg) | 100 M / 86,400 | ~1,157 queries/s |
| Search queries/sec (peak) | 1,157 × 4 | ~4,630 queries/s |
| Concurrent WebSocket connections | 10 M | 10 M |

### Latency Requirements

| Operation | P50 | P99 |
|---|---|---|
| Message delivery to online member | 50 ms | 200 ms |
| Message send acknowledgment | 20 ms | 100 ms |
| Search results | 100 ms | 500 ms |
| File upload acknowledgment | 200 ms | 1 s |
| Thread reply delivery | 50 ms | 200 ms |
| Slash command response | 100 ms | 3 s (Slack's documented limit) |
| Webhook delivery to external | 100 ms | 3 s |
| Presence update propagation | 500 ms | 3 s |

### Storage Estimates

**Assumptions:**
- Average message size: 500 bytes (text + metadata).
- Message retention: varies; assume average 1-year retention for calculations.
- File metadata in DB; file blobs in object storage.

| Data Type | Calculation | Volume |
|---|---|---|
| Message rows (1 year) | 800 M/day × 365 × 500 bytes | ~146 TB/year |
| File blob storage (1 year) | 80 M/day × 365 × 500 KB | ~14.6 PB/year |
| Search index (Elasticsearch) | ~3× message volume (index overhead) | ~438 TB/year |
| Message metadata (workspace, channel, user) | ~200 bytes/row × 800 M/day × 365 | ~58 TB/year |
| Reaction events | 4 B/day × 365 × 100 bytes | ~146 TB/year |
| File metadata (PostgreSQL) | 80 M/day × 365 × 300 bytes | ~8.8 TB/year |

### Bandwidth Estimates

| Direction | Calculation | Throughput |
|---|---|---|
| Inbound messages (avg) | 9,260 msg/s × 500 bytes | ~4.6 MB/s |
| Outbound messages (avg) | 231,500 deliveries/s × 500 bytes | ~116 MB/s |
| Outbound messages (peak) | 926,000 × 500 bytes | ~463 MB/s |
| File upload inbound (avg) | 463 MB/s | 463 MB/s |
| File download outbound (avg, 3× amplification) | 1.4 GB/s | 1.4 GB/s |
| Search result payloads | 4,630 queries/s × 10 KB avg response | ~46 MB/s |

---

## 3. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            Client Layer                                     │
│   Desktop (Electron)      Web (React)       Mobile (iOS/Android)           │
└──────────┬────────────────────┬──────────────────────┬──────────────────────┘
           │  WSS               │  HTTPS               │  WSS / HTTPS
           │                   │                      │
┌──────────▼───────────────────▼──────────────────────▼──────────────────────┐
│                    Edge / Load Balancing Layer                              │
│    Cloudflare (DDoS, WAF, CDN for static assets)                           │
│    AWS ALB/NLB per region (L7 for HTTP, L4+sticky for WSS)                 │
└────────────────────────────────────┬───────────────────────────────────────┘
                                     │
          ┌──────────────────────────┼──────────────────────────────┐
          │                          │                              │
┌─────────▼──────────┐  ┌───────────▼────────────┐  ┌─────────────▼──────────┐
│  Gateway Service   │  │   REST API Service      │  │  File/Media Service    │
│  (WebSocket mgmt)  │  │  (CRUD: workspace,      │  │  (upload, preview,     │
│  ~500 servers      │  │   channel, user, msg,   │  │   CDN URL generation)  │
│  20K conns each    │  │   reaction, search)     │  └─────────────┬──────────┘
└────────┬───────────┘  └───────────┬────────────┘                │
         │                          │                             │
┌────────▼──────────────────────────▼──────────────┐              │
│              Message Bus / Event Router           │              │
│  (Kafka — topics: messages, reactions, presence,  │              │
│   thread_updates, notifications, webhooks)        │              │
└──┬─────────────────────────┬─────────────────────┘              │
   │                         │                                    │
┌──▼──────────────┐  ┌───────▼──────────┐   ┌────────────────────▼──────────┐
│  Message Store  │  │  Channel Fanout  │   │        Object Storage         │
│  (MySQL sharded │  │  Service         │   │  (S3 — files, preview images) │
│   by workspace) │  │  (reads channel  │   │  CDN-fronted for downloads    │
│  + thread model │  │   member list,   │   └───────────────────────────────┘
└─────────────────┘  │   pushes via WS  │
                     │   or offline Q)  │
┌────────────────┐   └──────────────────┘   ┌───────────────────────────────┐
│ Search Service │                           │  Notification Service         │
│ (Elasticsearch │   ┌──────────────────┐   │  (APNs, FCM, email via SES)   │
│  per workspace │   │ Presence Service │   └───────────────────────────────┘
│  cluster)      │   │ (Redis + WS hb)  │
└────────────────┘   └──────────────────┘   ┌───────────────────────────────┐
                                             │  Integration/Webhook Service  │
┌──────────────────────────────┐            │  (Slash cmds, incoming/       │
│  Retention/Compliance Svc    │            │   outgoing webhooks, OAuth)   │
│  (scheduled deletion jobs,   │            └───────────────────────────────┘
│   compliance export to S3)   │
└──────────────────────────────┘
```

**Component Roles:**

- **Gateway Service**: Manages persistent WebSocket connections. Each connection authenticated via OAuth token. Receives outgoing messages from clients and publishes to Kafka. Receives incoming messages from Channel Fanout Service and pushes to connected client sockets.
- **REST API Service**: Handles all HTTP-based operations — channel CRUD, message history pagination, user/profile management, search proxying, reaction writes.
- **File/Media Service**: Accepts file uploads; generates presigned S3 URLs; creates thumbnails/previews asynchronously; returns CDN-fronted download URLs.
- **Message Bus (Kafka)**: Central nervous system. Decouples message ingestion from delivery, search indexing, notification dispatch, and webhook delivery. Each message published once; consumed by multiple downstream services.
- **Message Store (MySQL, sharded)**: Persistent store for all messages, thread replies, reactions, and metadata. Sharded by workspace_id for isolation. MySQL chosen for Slack (consistent with Slack's engineering blog history).
- **Channel Fanout Service**: Consumes message events from Kafka; reads channel membership; dispatches messages to online members' Gateway servers; queues for offline members.
- **Search Service**: Elasticsearch cluster per workspace (or shared with workspace-level index isolation). Indexes new messages from Kafka in near-real-time (~5s lag). Handles search queries from REST API Service.
- **Presence Service**: Tracks user online/away/DND status. WebSocket heartbeat maintains presence. Redis-backed with TTL.
- **Notification Service**: Consumes Kafka events; evaluates user notification preferences; sends desktop (via Gateway's WebSocket), mobile (APNs/FCM), or email notifications.
- **Integration/Webhook Service**: Handles slash command dispatch (HTTP callback to registered command endpoint), incoming webhooks (bot posts to channels), outgoing webhooks (Slack posts to external service on keyword match), OAuth app lifecycle.
- **Retention/Compliance Service**: Scheduled jobs that apply retention policies — delete or archive messages beyond the retention window. Compliance export bundles messages into gzip archives on S3.

**Primary Use-Case Data Flow (user sends a message to a channel):**

1. Client sends message over WebSocket: `{type: "message", channel_id, text, client_msg_id}`.
2. Gateway Service validates auth token, publishes to Kafka topic `messages` with envelope including workspace_id, channel_id, user_id, text, ts.
3. REST API Service (or a dedicated Write Service) consumes from Kafka (or receives synchronously via Gateway), writes message row to MySQL shard for workspace_id. Returns ACK with server-assigned `ts` (timestamp/message ID) to sender.
4. Channel Fanout Service reads message from Kafka. Fetches channel member list (Redis cache). For each member:
   - If online (has a Gateway server entry): sends gRPC push to the correct Gateway server → WebSocket push to client.
   - If offline: enqueues in per-user offline queue.
5. Search Indexer (Kafka consumer) writes message text + metadata to Elasticsearch index for workspace.
6. Notification Service evaluates preferences; sends APNs/FCM for offline mobile users.
7. Webhook Service evaluates if any outgoing webhooks match keywords; dispatches HTTP POST to registered URLs.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Workspaces
-- ============================================================
CREATE TABLE workspaces (
    workspace_id    BIGINT      PRIMARY KEY AUTO_INCREMENT,
    name            VARCHAR(255) NOT NULL,
    domain          VARCHAR(255) UNIQUE NOT NULL,  -- e.g., mycompany.slack.com
    plan            ENUM('free', 'pro', 'business+', 'enterprise') NOT NULL,
    created_at      DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    retention_days  INT,        -- NULL = keep forever; per workspace setting
    settings_json   JSON        NOT NULL DEFAULT '{}'
) ENGINE=InnoDB;

-- ============================================================
-- Users (global, cross-workspace identity)
-- ============================================================
CREATE TABLE users (
    user_id         BIGINT      PRIMARY KEY AUTO_INCREMENT,
    email           VARCHAR(255) UNIQUE NOT NULL,
    display_name    VARCHAR(80)  NOT NULL,
    avatar_url      VARCHAR(512),
    password_hash   VARCHAR(255),   -- null if SSO-only
    created_at      DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_email (email)
) ENGINE=InnoDB;

-- ============================================================
-- Workspace Memberships
-- ============================================================
CREATE TABLE workspace_members (
    workspace_id    BIGINT      NOT NULL,
    user_id         BIGINT      NOT NULL,
    role            ENUM('owner', 'admin', 'member', 'single_channel_guest', 'multi_channel_guest') NOT NULL DEFAULT 'member',
    display_name    VARCHAR(80),    -- workspace-specific display name (overrides global)
    joined_at       DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deactivated_at  DATETIME,
    notification_preferences_json JSON NOT NULL DEFAULT '{}',
    PRIMARY KEY (workspace_id, user_id),
    INDEX idx_wm_user (user_id)
) ENGINE=InnoDB;

-- ============================================================
-- Channels
-- ============================================================
CREATE TABLE channels (
    channel_id      BIGINT      PRIMARY KEY AUTO_INCREMENT,
    workspace_id    BIGINT      NOT NULL,
    name            VARCHAR(80)  NOT NULL,
    description     TEXT,
    is_private      BOOLEAN     NOT NULL DEFAULT false,
    is_archived     BOOLEAN     NOT NULL DEFAULT false,
    created_by      BIGINT      NOT NULL,
    created_at      DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uq_workspace_channel (workspace_id, name),
    INDEX idx_channel_workspace (workspace_id)
) ENGINE=InnoDB;

CREATE TABLE channel_members (
    channel_id      BIGINT      NOT NULL,
    user_id         BIGINT      NOT NULL,
    workspace_id    BIGINT      NOT NULL,
    joined_at       DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_read_ts    BIGINT,         -- last message ts user has read (for unread count)
    is_muted        BOOLEAN     NOT NULL DEFAULT false,
    role            ENUM('member', 'admin') NOT NULL DEFAULT 'member',
    PRIMARY KEY (channel_id, user_id),
    INDEX idx_cm_user_workspace (user_id, workspace_id)
) ENGINE=InnoDB;

-- ============================================================
-- Messages
-- NOTE: Sharded by workspace_id at the application layer.
-- Each shard (MySQL instance) holds messages for a set of workspaces.
-- ============================================================
CREATE TABLE messages (
    message_id      BIGINT      NOT NULL AUTO_INCREMENT,
    workspace_id    BIGINT      NOT NULL,
    channel_id      BIGINT      NOT NULL,
    user_id         BIGINT      NOT NULL,   -- sender
    client_msg_id   VARCHAR(64) NOT NULL,   -- client-generated idempotency key
    ts              BIGINT      NOT NULL,   -- Unix microseconds, server-assigned
    text            TEXT        NOT NULL,
    message_type    ENUM('message', 'system', 'bot', 'thread_broadcast') NOT NULL DEFAULT 'message',
    thread_ts       BIGINT,                 -- NULL = top-level; non-null = thread reply (points to parent ts)
    reply_count     INT         NOT NULL DEFAULT 0,
    reaction_count  INT         NOT NULL DEFAULT 0,
    file_ids        JSON,                   -- array of file IDs attached
    edited_at       BIGINT,                 -- NULL if unedited
    deleted_at      BIGINT,                 -- soft delete; text replaced with "(Message deleted)"
    PRIMARY KEY (message_id),
    UNIQUE KEY uq_workspace_client_msg (workspace_id, client_msg_id),  -- idempotency
    INDEX idx_msg_channel_ts (channel_id, ts DESC),                    -- channel timeline
    INDEX idx_msg_thread (channel_id, thread_ts, ts),                  -- thread retrieval
    INDEX idx_msg_user (workspace_id, user_id, ts DESC)                -- user's messages
) ENGINE=InnoDB;

-- ============================================================
-- Thread participants (for "notify me on new reply" tracking)
-- ============================================================
CREATE TABLE thread_participants (
    workspace_id    BIGINT      NOT NULL,
    channel_id      BIGINT      NOT NULL,
    thread_ts       BIGINT      NOT NULL,   -- parent message ts
    user_id         BIGINT      NOT NULL,
    last_read_reply_ts BIGINT,
    PRIMARY KEY (workspace_id, channel_id, thread_ts, user_id)
) ENGINE=InnoDB;

-- ============================================================
-- Reactions
-- ============================================================
CREATE TABLE reactions (
    workspace_id    BIGINT      NOT NULL,
    channel_id      BIGINT      NOT NULL,
    message_ts      BIGINT      NOT NULL,
    emoji           VARCHAR(64)  NOT NULL,  -- e.g., "thumbsup"
    user_id         BIGINT      NOT NULL,
    reacted_at      DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (workspace_id, channel_id, message_ts, emoji, user_id),
    INDEX idx_reaction_msg (workspace_id, channel_id, message_ts)
) ENGINE=InnoDB;

-- ============================================================
-- Files
-- ============================================================
CREATE TABLE files (
    file_id         BIGINT      PRIMARY KEY AUTO_INCREMENT,
    workspace_id    BIGINT      NOT NULL,
    uploader_id     BIGINT      NOT NULL,
    filename        VARCHAR(255) NOT NULL,
    content_type    VARCHAR(128) NOT NULL,
    size_bytes      BIGINT      NOT NULL,
    storage_key     VARCHAR(512) NOT NULL,   -- S3 object key
    thumbnail_key   VARCHAR(512),            -- S3 key for preview image
    sha256          VARCHAR(64) NOT NULL,    -- for deduplication
    uploaded_at     DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at      DATETIME,               -- NULL = retain per workspace policy
    INDEX idx_file_workspace (workspace_id, uploaded_at DESC)
) ENGINE=InnoDB;

-- ============================================================
-- Slash Commands (registered by integrations)
-- ============================================================
CREATE TABLE slash_commands (
    command_id      BIGINT      PRIMARY KEY AUTO_INCREMENT,
    workspace_id    BIGINT      NOT NULL,
    command         VARCHAR(64)  NOT NULL,   -- e.g., "/poll"
    request_url     VARCHAR(512) NOT NULL,
    description     VARCHAR(256),
    usage_hint      VARCHAR(256),
    app_id          BIGINT      NOT NULL,
    UNIQUE KEY uq_workspace_command (workspace_id, command)
) ENGINE=InnoDB;

-- ============================================================
-- Webhooks (incoming)
-- ============================================================
CREATE TABLE incoming_webhooks (
    webhook_id      BIGINT      PRIMARY KEY AUTO_INCREMENT,
    workspace_id    BIGINT      NOT NULL,
    channel_id      BIGINT      NOT NULL,
    token           VARCHAR(64) UNIQUE NOT NULL,   -- URL token
    created_by      BIGINT      NOT NULL,
    created_at      DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_used_at    DATETIME
) ENGINE=InnoDB;

-- ============================================================
-- Search Index (Elasticsearch document schema — not SQL)
-- Index: messages_{workspace_id}
-- ============================================================
-- {
--   "message_id": 12345678,
--   "workspace_id": 100,
--   "channel_id": 200,
--   "user_id": 300,
--   "ts": 1712620800000000,     -- microseconds epoch
--   "text": "message body here",
--   "thread_ts": null,
--   "file_names": ["report.pdf"],
--   "reactions": ["thumbsup", "fire"],
--   "channel_is_private": false,
--   "member_ids": [300, 301, 302, ...],  -- for access control filtering
--   "deleted_at": null
-- }
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| **MySQL (InnoDB, sharded)** | Messages, channels, users, reactions | ACID transactions; proven at scale (Slack's actual choice per engineering blog); excellent for range scans on (channel_id, ts) index; well-understood operational tooling; strong consistency per shard | Vertical scaling per shard; sharding adds application complexity; JOIN across shards requires application-level coordination | **Selected** for transactional data (messages, channels, users) |
| **PostgreSQL** | Same as MySQL | Superior JSON support (JSONB); better full-text search built-in; advanced indexing (GIN) | Less battle-tested at Slack's specific sharding pattern; Slack's codebase predates modern PG sharding solutions | Not selected (MySQL has historical precedent at Slack) |
| **Elasticsearch** | Message search | Inverted index native; horizontal scaling; full-text scoring (BM25); aggregations for analytics; near-real-time indexing | Not ACID; not a source of truth; expensive memory requirements; cluster management complexity; eventual consistency | **Selected** for search indexing |
| **Redis Cluster** | Presence, channel member cache, offline queue, rate limiting | Sub-ms latency; native data structures (Sorted Sets, Hashes, Pub/Sub); TTL support | Memory cost; not durable source of truth; requires persistence configuration | **Selected** for ephemeral and hot-path data |
| **Amazon S3** | File/media blobs | Durable; scalable; lifecycle policies; presigned URLs; CDN integration | Not a database; requires metadata layer; egress cost | **Selected** for file storage |
| **Kafka** | Event bus | Durability; replayability; fan-out to multiple consumers; backpressure handling; log compaction for state topics | Operational complexity; Zookeeper/KRaft overhead; not suitable for point lookups | **Selected** for message bus and event streaming |

**Justification for MySQL over Cassandra:**
Unlike WhatsApp (billions of users, eventual consistency acceptable), Slack's access patterns benefit from strong transactional guarantees. A message "edit" needs to atomically update the text and the search index signal. Thread reply counts need atomic increment. Slack also has complex relational queries (list channels for user, unread counts joining channel_members and messages). MySQL's ACID semantics and rich indexing handle these naturally. The scale (37 K msg/sec peak) is large but achievable with ~200 MySQL shards. Slack's own engineering blog confirms MySQL as their primary data store.

---

## 5. API Design

Authentication: OAuth 2.0 for apps/bots; session token (cookie + CSRF) for web/desktop; JWT for mobile. All API responses include a `request_id` for tracing.

### REST Endpoints

#### Channels

```
POST /api/conversations.create
Authorization: Bearer <token>
Request: { "name": "engineering", "is_private": false, "team_id": "T01234" }
Response 200:
{
  "ok": true,
  "channel": { "id": "C01234", "name": "engineering", "is_private": false, "created": 1712620800 }
}
Rate Limit: 20 creates/workspace/minute

POST /api/conversations.join
Authorization: Bearer <token>
Request: { "channel": "C01234" }
Response 200: { "ok": true, "channel": { ... }, "already_in_channel": false }

GET /api/conversations.list
Authorization: Bearer <token>
Query Params:
  types=public_channel,private_channel,mpim,im
  limit=200 (max 1000)
  cursor=<next_cursor>
Response 200:
{
  "ok": true,
  "channels": [ { "id", "name", "is_private", "num_members", "unread_count" } ],
  "response_metadata": { "next_cursor": "abc123" }
}
Rate Limit: Tier 3 (50+ calls/minute)

GET /api/conversations.members
Query: { channel: "C01234", limit: 200, cursor: "..." }
Response: { "ok": true, "members": ["U001", "U002"], "response_metadata": { "next_cursor": "..." } }
```

#### Messages

```
POST /api/chat.postMessage
Authorization: Bearer <token>
Request:
{
  "channel": "C01234",
  "text": "Hello, World!",
  "thread_ts": null,          // omit for top-level; include for thread reply
  "blocks": [...],            // Block Kit rich layout (optional)
  "attachments": [...],       // legacy attachment format
  "unfurl_links": true,
  "reply_broadcast": false    // for thread replies: also post to channel
}
Response 200:
{
  "ok": true,
  "channel": "C01234",
  "ts": "1712620800.123456",   // message timestamp (Slack's unique message ID)
  "message": { "type": "message", "text": "Hello, World!", "user": "U01234", "ts": "..." }
}
Rate Limit: Tier 3 — 1 call/second per workspace for bots; higher for users

POST /api/chat.update
Request: { "channel": "C01234", "ts": "1712620800.123456", "text": "Hello, edited!" }
Response 200: { "ok": true, "channel", "ts", "text" }
Rate Limit: Tier 3

POST /api/chat.delete
Request: { "channel": "C01234", "ts": "1712620800.123456" }
Response 200: { "ok": true, "channel", "ts" }

GET /api/conversations.history
Query Params:
  channel=C01234
  latest=<ts>       // messages before this ts
  oldest=<ts>       // messages after this ts
  limit=100 (max)
  inclusive=1
  cursor=<encoded_cursor>
Response 200:
{
  "ok": true,
  "messages": [
    { "type": "message", "user": "U01234", "text": "...", "ts": "...", "reply_count": 3, "reactions": [...] }
  ],
  "has_more": true,
  "response_metadata": { "next_cursor": "..." }
}
Rate Limit: Tier 3

GET /api/conversations.replies
Query: { channel, ts (parent thread_ts), limit, cursor }
Response: { "ok": true, "messages": [parent, ...replies], "has_more": bool }
```

#### Search

```
GET /api/search.messages
Authorization: Bearer <token>
Query Params:
  query=deployment failed         // search terms + operators (in:#channel, from:@user, after:2024-01-01)
  sort=score | timestamp
  sort_dir=desc
  count=20 (max 100)
  page=1
Response 200:
{
  "ok": true,
  "query": "deployment failed",
  "messages": {
    "total": 342,
    "pagination": { "total_count": 342, "page": 1, "per_page": 20, "page_count": 18 },
    "matches": [
      {
        "type": "message",
        "channel": { "id": "C01234", "name": "ops-alerts" },
        "user": "U01234",
        "text": "Deployment failed at 14:32 UTC",
        "ts": "1712620800.123456",
        "permalink": "https://mycompany.slack.com/archives/C01234/p1712620800123456"
      }
    ]
  }
}
Rate Limit: Tier 2 (20 calls/minute)
Notes: Results filtered server-side to channels user has access to. Private channels excluded unless user is member.
```

#### Reactions

```
POST /api/reactions.add
Request: { "channel": "C01234", "timestamp": "1712620800.123456", "name": "thumbsup" }
Response 200: { "ok": true }
Rate Limit: Tier 2

GET /api/reactions.get
Query: { channel, timestamp, full=true }
Response: { "ok": true, "message": { "reactions": [{ "name": "thumbsup", "count": 5, "users": ["U001",...] }] } }
```

#### Files

```
POST /api/files.getUploadURLExternal
Request: { "filename": "report.pdf", "length": 204800, "channel_id": "C01234" }
Response 200: { "ok": true, "upload_url": "https://files.slack.com/upload/v1/...", "file_id": "F01234" }

POST <upload_url>  (multipart form upload directly to File Service / S3)
Body: form-data with file content

POST /api/files.completeUploadExternal
Request: { "files": [{ "id": "F01234", "title": "Q1 Report" }], "channel_id": "C01234", "initial_comment": "Here's the report" }
Response 200: { "ok": true, "files": [{ "id": "F01234", "url_private": "https://files.slack.com/files-pri/...", "thumb_360": "..." }] }
Rate Limit: 20 uploads/user/minute
```

#### Slash Commands (webhook-style)

```
// Slack posts to the registered command URL:
POST <registered_command_url>
Content-Type: application/x-www-form-urlencoded
Body:
  token=<verification_token>
  team_id=T01234
  team_domain=mycompany
  channel_id=C01234
  channel_name=general
  user_id=U01234
  user_name=alice
  command=/poll
  text=What's for lunch? Pizza, Sushi, Tacos
  response_url=https://hooks.slack.com/commands/...
  trigger_id=...

// App responds:
Response 200:
{
  "response_type": "in_channel",  // or "ephemeral" (visible only to sender)
  "text": "Poll created!",
  "blocks": [...]
}
// Must respond within 3000ms; can use response_url for async responses up to 30 min
```

### WebSocket RTM (Real-Time Messaging) Protocol

```
WSS /api/rtm.connect  (legacy)
// Modern clients use Socket Mode (event-based WebSocket for apps)
WSS /api/apps.connections.open  (Socket Mode for apps)

// Server → Client event types:
{
  "envelope_id": "uuid",
  "payload": {
    "type": "message",
    "channel": "C01234",
    "user": "U01234",
    "text": "Hello",
    "ts": "1712620800.123456",
    "thread_ts": null,
    "edited": null
  }
}

// Event types: message, message.changed, message.deleted, reaction_added,
//              reaction_removed, channel_joined, channel_left, user_typing,
//              presence_change, thread_marked, channel_marked (unread cursor update)

// Client → Server:
{ "id": 1, "type": "ping" }
// Server: { "reply_to": 1, "type": "pong" }

// ACK for Socket Mode:
{ "envelope_id": "uuid" }  // client must ACK within 3 seconds
```

---

## 6. Deep Dive: Core Components

### 6.1 Real-Time Message Delivery via WebSocket and Fan-Out

**Problem it solves:**
When a user sends a message to a channel, all currently-online channel members must receive it within 200 ms (p99). Channels can have thousands of members; large channels (e.g., `#general` in a 100,000-person company) could have all members online simultaneously.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Polling (short poll)** | Clients poll /messages endpoint every second | Simple; stateless servers | Terrible latency (up to 1 s); 10 M clients × 1 poll/s = 10 M req/s just for polling |
| **Long polling** | Client holds HTTP connection open until new message arrives | Simpler than WebSocket | One connection per poll; reconnect overhead; half-duplex; HTTP/1.1 connection limits |
| **Server-Sent Events (SSE)** | Server pushes events over HTTP/1.1 chunked response | Simple; browser native | Unidirectional; client typing indicators require separate POST connection |
| **WebSocket per user** | One persistent WSS connection per device | Bidirectional; low latency; efficient framing; supported everywhere | Stateful; 10 M connections requires infrastructure; connection management complexity |
| **WebSocket + Kafka fan-out** | Gateway publishes to Kafka; fan-out service delivers via gateway gRPC | Decoupled; durable; scalable consumers | Additional hop adds ~10-20 ms; needs per-gateway Kafka consumer group |
| **WebSocket + Redis Pub/Sub** | Gateway subscribes to Redis channel per Slack channel; fan-out via pub/sub | Simple fan-out; low latency | Redis Pub/Sub memory and connection overhead; no persistence; doesn't handle offline users |

**Selected Approach: WebSocket Gateways + Kafka event bus + Fan-Out Service**

**Implementation Detail:**

*Gateway tier:*
- 500 servers × 20,000 connections = 10 M concurrent connections. Each server runs an epoll-based async event loop (Go's goroutine-per-connection model or Node.js event loop).
- Connection authenticated on Upgrade handshake (token validated against auth service).
- Gateway publishes received messages to Kafka topic `raw_messages` with key = `workspace_id` (ensures all messages for a workspace go to the same partition for ordering).
- Gateway maintains an in-memory map: `{user_id → websocket_conn}` for all its connected users.

*Message routing:*
- Channel Fanout Service consumes from Kafka `raw_messages` topic.
- For each message: reads channel member list (Redis cache, warmed on channel join, TTL 60s). Average channel: 50 members. Large channel: 100,000 members (handled differently).
- Normal channels (≤ 10,000 members): fetches all online member connections from connection registry, groups by gateway server, sends batched gRPC pushes to each gateway.
- Large channels (> 10,000 members): use a Pub/Sub hierarchy — Gateways subscribe to a Redis channel named `ws_channel:{channel_id}`. Fan-out Service publishes one message to Redis; Redis fan-outs to all subscribed gateway servers. This avoids an O(N members) loop in the Fan-Out Service for large channels.

*Offline members:*
- Members not found in connection registry → added to per-user offline queue (Redis Sorted Set) keyed by `offline:{user_id}:{workspace_id}` with score = message timestamp.
- Notification Service picks up offline members and sends APNs/FCM push.
- On reconnect, client sends `{ type: "client_hello", last_event_id: "..." }`. Gateway reads offline queue and delivers pending messages.

*Numbers for large channels:*
- `#general` with 100 K members, 50% online = 50 K simultaneous deliveries per message.
- Redis Pub/Sub publish to `ws_channel:{channel_id}`: 1 publish → received by all subscribed gateway servers.
- If 50 K online members spread across 200 distinct gateway servers (250 connections/gateway for this channel on average): 200 gateway servers each receive 1 pub/sub message and push to their ~250 connections.
- Total: 1 Redis publish → 200 gateway notifications → 50 K WebSocket writes. Redis publish latency: ~1 ms. Gateway push to 250 sockets: ~5 ms parallel. Total: ~6 ms for full fan-out to 50 K members. Well within 200 ms SLA.

**Interviewer Q&As:**

1. **Q: How does Slack maintain message ordering in a channel when multiple users send simultaneously?**
   A: The server assigns a timestamp (`ts`) to each message on write. Slack's `ts` is a string-encoded Unix timestamp with microsecond precision (e.g., "1712620800.123456") — this is both the ordering key and the unique identifier. Writes to MySQL go through a single-shard writer for a given channel (the channel's workspace shard). The AUTO_INCREMENT primary key combined with the `ts` index provides ordering. At the application level, clients display messages sorted by `ts` ascending. The server guarantees monotonically increasing `ts` values for messages in the same shard by using a sequence generator (database AUTO_INCREMENT + `ts` = NOW(6) at write time). Two messages can't get the same microsecond timestamp because the shard serializes writes (InnoDB row locking on the INSERT). For multi-region, cross-DC messages, the timestamp can conflict — resolved by logical clock (Lamport timestamps could be used, but in practice Slack's single-writer-per-workspace shard avoids this).

2. **Q: How do you handle the thundering herd when 50,000 users are in #general and one message triggers 50,000 simultaneous WebSocket writes?**
   A: The Pub/Sub fan-out architecture distributes this load. Rather than one service writing 50 K times, the Redis Pub/Sub publish propagates to the ~200 Gateway servers that have members of this channel connected. Each gateway independently writes to its subset of connections (typically 250 per gateway for this channel). These 200 gateway writes happen in parallel and each is small. The Gateway's event loop handles 250 writes as 250 non-blocking socket writes (epoll) — effectively microsecond operations. The actual constraint is network bandwidth: 50 K writes × 1 KB message = 50 MB for one channel message. At 10 Gbps per gateway server, this is trivial. The Kafka consumer (Fan-Out Service) produces one Kafka message → Redis publishes one message → 200 Gateways each handle their subset. No single bottleneck handles 50 K connections.

3. **Q: How do you handle WebSocket connection state during a rolling deployment of Gateway servers?**
   A: During rolling deployment, we drain connections gracefully: (1) Load balancer marks the deploying gateway instance as "draining" — no new connections routed to it. (2) Gateway sends a WebSocket close frame with code 4001 ("server restarting") to all connected clients. (3) Clients receive close frame and reconnect, which routes them to a healthy instance. (4) Client-side reconnect logic maintains an exponential backoff but reconnects within 1-3 seconds for server-initiated closes (vs. 10-30 s for unexpected disconnects). (5) Offline queue ensures no messages are lost during the ~2-second reconnect window. The rolling deployment takes gateways out one at a time; at 500 total gateways taking out 1 at a time, the capacity drop is 0.2% per step — users on that server experience a brief reconnect, all other users are unaffected.

4. **Q: What happens when the Kafka consumer group (Fan-Out Service) lags behind during a traffic spike?**
   A: Kafka consumer lag means messages are queued in Kafka before delivery. At 37 K msg/s peak and 926 K deliveries/s, if Fan-Out Service processes 800 K deliveries/s, it will fall behind. Mitigation: (1) Auto-scale Fan-Out Service instances (each instance in the consumer group gets a subset of partitions). (2) Kafka partitions (one per workspace): 100 K workspaces with 10 partitions each = 1 M partitions — a lot, but Kafka handles it. In practice, fewer total partitions (10 K) with workspace-to-partition assignment. (3) Backpressure: Gateway can queue messages locally if Kafka write is slow (bounded queue, reject if full → client gets error → client retries). (4) Shed load: for large channels during lag, switch to a pull model for clients (client polls history API once lag exceeds threshold) and drop real-time delivery attempt. This is a graceful degradation strategy.

5. **Q: How does Slack handle "user is typing" indicators without overloading the system?**
   A: Typing indicators are ephemeral and low-priority. Implementation: (1) Client sends a WebSocket frame `{type: "user_typing", channel: "C01234"}` when user starts typing. (2) Gateway forwards directly to Fan-Out Service (bypasses Kafka for speed — this is fire-and-forget, not durable). (3) Fan-Out Service broadcasts to other channel members in the channel with a TTL of 5 seconds. (4) Rate limiting: client only sends the typing event once every 3 seconds (not on every keystroke). If user stops typing for 5 seconds, no "stopped typing" event is needed — the TTL expiry handles it. (5) For large channels (> 1,000 members): typing indicators are suppressed entirely — the UX value is zero when there are 1,000 people in a channel. This is a product + engineering co-decision to cut a disproportionate load.

---

### 6.2 Message Search with Elasticsearch

**Problem it solves:**
Users need to find messages across potentially billions of stored messages in a workspace, with results in < 500 ms (p99), filtered to only channels the user has access to, ranked by relevance and recency.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **MySQL FULLTEXT index** | Use MySQL's native FULLTEXT search on messages table | No additional infrastructure; ACID | Poor relevance scoring; no distributed search; full scans on large datasets; slow at Slack's volume |
| **PostgreSQL tsvector + GIN** | Use PG full-text search | Better than MySQL FTS; trigram index; built-in ranking | Still single-node bottleneck; complex sharding; no real-time indexing |
| **Elasticsearch** | Dedicated search cluster per workspace; Kafka→Elasticsearch indexer | Industry-standard for text search; horizontal scale; BM25 relevance; aggregations; near-real-time | Operational complexity; memory-heavy; eventual consistency (not source of truth); expensive |
| **Apache Solr** | Alternative to ES for full-text | Similar capabilities | Less ecosystem momentum; ES has better managed offerings (Elastic Cloud) |
| **Custom inverted index** | Build own index in Redis or specialized DB | Full control | Enormous engineering cost; reinventing Elasticsearch |
| **OpenSearch** | AWS fork of Elasticsearch | AWS-managed; compatible with ES API | Same trade-offs as ES; slightly behind ES feature velocity |

**Selected Approach: Elasticsearch with per-workspace index and access-control filtering**

**Implementation Detail:**

*Index structure:*
- One Elasticsearch index per workspace: `messages_T01234` (workspace ID suffix).
- Index settings: 3 primary shards, 1 replica (per workspace). Small workspaces share an Elasticsearch cluster; large workspaces get dedicated clusters.
- Mapping:
  - `text`: analyzed with `standard` analyzer + custom filter for emoji tokenization, stop words, synonyms.
  - `ts`: long (microseconds), for date range filters and recency boosting.
  - `channel_id`: keyword, for filter.
  - `user_id`: keyword, for filter.
  - `member_ids`: keyword array — all channel members at index time. Used for access control filtering.

*Indexing pipeline:*
- Kafka consumer (`search-indexer` consumer group) reads from `messages` topic.
- Batch index to Elasticsearch using Bulk API (batch size = 500 messages, flush every 1 second or 500 messages, whichever first). Bulk API reduces network overhead by 50x vs individual index calls.
- Consumer processes ~9,260 msg/s avg = 9,260 / 500 = ~18.5 bulk requests/sec. Easily handled by a 10-node Elasticsearch cluster.
- Indexed within ~5 seconds of message send (Kafka consume lag ≈ 1 s + Elasticsearch refresh interval = 1 s + network ≈ 5 s total).

*Query execution:*
- User submits search query.
- REST API Service parses query: extracts operators (`in:#channel`, `from:@user`, `after:YYYY-MM-DD`, `has:file`).
- Constructs ES bool query:
  ```json
  {
    "query": {
      "bool": {
        "must": [
          { "match": { "text": { "query": "deployment failed", "fuzziness": "AUTO" } } }
        ],
        "filter": [
          { "term": { "channel_id": "C01234" } },     // if in: operator
          { "terms": { "member_ids": ["U01234"] } }   // ACCESS CONTROL: user must be in channel
        ],
        "should": [
          { "range": { "ts": { "gte": "now-30d" } } }  // boost recent results
        ]
      }
    },
    "sort": [
      { "_score": { "order": "desc" } },
      { "ts": { "order": "desc" } }
    ],
    "size": 20,
    "from": 0
  }
  ```
- The `member_ids` filter performs access control: only messages from channels where the querying user is a member are returned. `member_ids` is updated in the index when users join/leave channels (event-driven update via Kafka).
- Private channel isolation: private channels' messages have `member_ids` populated with all channel members. The query's `terms` filter on `member_ids` naturally excludes private channel messages unless the user is a member.

*Scaling:*
- At 4,630 search queries/sec (peak), each taking ~50-100 ms on Elasticsearch: throughput = 1000 ms / 75 ms × N threads. A 20-node Elasticsearch cluster with 20 threads/node = 400 concurrent queries × 13 queries/sec/thread ≈ 5,200 queries/sec. Within capacity.
- Large workspaces (millions of messages): dedicated Elasticsearch cluster with more shards (10+). Shard by time: current month's shard is hot; older shards cold (force-merged, read-only).
- Index lifecycle management (ILM): hot phase (0-30 days) → warm phase (30-90 days, replicas reduced, force-merged) → cold phase (90+ days, searchable snapshots on S3) → delete phase (per workspace retention policy).

**Interviewer Q&As:**

1. **Q: How do you keep the Elasticsearch index consistent when messages are edited or deleted?**
   A: The Kafka consumer also processes `message_edited` and `message_deleted` events. For edits: partial update via ES Update API: `POST /messages_T01234/_update/{message_id}` with `{ "doc": { "text": "new text" } }`. For deletes: either delete the document (`DELETE /messages_T01234/_doc/{message_id}`) or update `deleted_at` field and add a filter in the search query to exclude deleted messages. The second approach (soft delete) is preferred — it allows compliance teams to export deleted messages while they still appear deleted in search. Elasticsearch's near-real-time nature means edits/deletes take up to the refresh interval (1 second) to be reflected in search results — acceptable for a chat search product.

2. **Q: How do you handle search access control for shared channels (Slack Connect) where members are from different workspaces?**
   A: This is a Slack Connect edge case (out of core scope, but good to address). For shared channels, the `member_ids` field in the search index must include all members from all connected workspaces. The search query filters on `member_ids` containing the current user. The complexity: if workspace A and B share a channel, workspace A's Elasticsearch index must contain the member list including workspace B's user IDs. This requires a cross-workspace member list join at index time and at join/leave events. An alternative: partition the index by channel rather than workspace for shared channels, with a separate Elasticsearch cluster for shared channel content. This adds complexity but cleanly isolates the access control problem.

3. **Q: How do you rank search results when both recency and relevance matter?**
   A: Elasticsearch's BM25 score handles term relevance (term frequency × inverse document frequency). Recency is incorporated via a `function_score` query that multiplies BM25 score by a decay function on the `ts` field: `gauss` decay with origin=now, scale=30d, decay=0.5 means a message from 30 days ago has its score halved. Users can override with explicit sort by timestamp. For users who consistently click on recent results, a personalization layer can learn to weight recency more heavily (offline model trained on click data). For the base implementation, the default sort is a combination: `_score` as primary, `ts` as tiebreaker, with the function_score decay baking recency into relevance.

---

### 6.3 Message Threading

**Problem it solves:**
Threads allow focused sub-conversations branching from a message without cluttering the main channel view. The system must track thread state (reply count, latest reply timestamp, participant list), deliver replies to thread participants in real-time, and display thread previews inline in the channel.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Flat messages with parent_id** | Thread replies stored as regular messages with a `thread_ts` FK | Simple; same table; no join needed | Reply count requires aggregate query; hard to paginate thread separately from channel; N+1 query for previews |
| **Separate threads table** | Thread metadata in a separate table; replies in messages table with FK | Clean schema; thread metadata queryable independently | Extra JOIN for fetching thread with replies; dual write on reply (messages + threads) |
| **Denormalized thread metadata on parent message** | Parent message row stores `reply_count`, `latest_reply_ts`, `participants_json` | Fast reads (no JOIN for preview); atomic update via InnoDB transaction | Hotspot on popular threads (many writers updating same row); JSON participants can grow large |
| **Hybrid: messages table + thread_summary table** | Messages table stores all messages; separate thread_summary table for aggregated metadata | Best of both: fast thread summary reads; normalized replies | Write path requires 2 inserts (message + thread_summary update); needs transaction |

**Selected Approach: Hybrid — messages table with thread_ts FK + thread_summary table**

**Implementation Detail:**

*Data model extension:*
```sql
CREATE TABLE thread_summaries (
    workspace_id        BIGINT      NOT NULL,
    channel_id          BIGINT      NOT NULL,
    thread_ts           BIGINT      NOT NULL,   -- parent message ts
    reply_count         INT         NOT NULL DEFAULT 0,
    latest_reply_ts     BIGINT,
    participant_ids     JSON        NOT NULL DEFAULT '[]',  -- top 3 avatars for preview
    follower_ids        JSON        NOT NULL DEFAULT '[]',  -- users subscribed to notifications
    PRIMARY KEY (workspace_id, channel_id, thread_ts)
) ENGINE=InnoDB;
```

*Write path for a thread reply:*
1. Client sends `POST /api/chat.postMessage` with `thread_ts` set to parent message's `ts`.
2. MySQL transaction (on the workspace shard):
   - `INSERT INTO messages (...)` with `thread_ts = parent_ts`.
   - `INSERT INTO thread_summaries (...) ON DUPLICATE KEY UPDATE reply_count = reply_count + 1, latest_reply_ts = VALUES(latest_reply_ts), participant_ids = JSON_SET(...)`.
3. Update parent message row: `UPDATE messages SET reply_count = reply_count + 1 WHERE ts = parent_ts` — this populates the inline preview count.
4. Kafka event published with type `thread_reply` and `thread_ts`.

*Fan-out for thread replies:*
- Thread replies are delivered to: (a) all channel members who have the thread open in their UI, (b) thread followers (users who have replied or explicitly followed the thread).
- Thread follower list maintained in `thread_summaries.follower_ids`. On reply, Fan-Out Service fetches followers and delivers via WebSocket (same as channel message delivery). Non-followers in the channel see only the inline preview update (reply_count bump on the parent message).
- `reply_broadcast: true` flag: thread reply also posted to the main channel as a separate message. Handled by Message Service duplicating the delivery — one event for thread followers, one event for all channel members.

*Read path:*
- `GET /api/conversations.replies?channel=C01234&ts=1712620800.123456`: reads messages table with `WHERE channel_id = C01234 AND thread_ts = 1712620800123456 ORDER BY ts ASC`.
- First result is the parent message; rest are replies. Paginated with cursor.
- Thread preview in channel view: message row includes `reply_count`, `latest_reply_ts`, and `participant_ids` (from thread_summaries or denormalized on the message row). Single query, no JOIN needed for the common case.

*Numbers:*
- 800 M messages/day; assume 15% are thread replies = 120 M thread replies/day.
- `thread_summaries` table: rows = number of active threads = ~100 M (estimate). At ~200 bytes/row = 20 GB — fits in MySQL buffer pool cache for hot threads.
- `ON DUPLICATE KEY UPDATE` contention: if a thread gets 100 replies in 1 second (viral message), 100 concurrent UPDATEs to the same `thread_summaries` row → InnoDB row lock contention. Mitigation: use write coalescing — Fan-Out Service batches thread_summary updates in Redis (HINCRBY reply_count) and flushes to MySQL every 5 seconds with the aggregated delta. Reduces write frequency by ~500x for hot threads.

**Interviewer Q&As:**

1. **Q: How do you handle the case where a user replies in a thread and wants to also post to the channel — what are the consistency guarantees?**
   A: When `reply_broadcast: true`, the thread reply is inserted into `messages` table with `thread_ts` set (it's a thread reply) AND Slack synthetically delivers it to the channel feed as well. The channel delivery is the same message, just included in channel history. In MySQL, this is a single INSERT (the message row). The dual delivery (thread followers + all channel members) is handled by the Fan-Out Service sending two separate fan-out events from the same Kafka message. There's no separate DB row for the channel copy — it's the same row queried with different WHERE clauses: `thread_ts = X` for thread view, or when `reply_broadcast = true`, also included in `WHERE thread_ts IS NULL OR reply_broadcast = 1` for channel view. Consistency is guaranteed because it's one atomic DB write; fan-out events are idempotent.

---

## 7. Scaling

**Horizontal Scaling:**
- **Gateway**: Add servers to ASG; sticky load balancing on user_id. 500 servers at 20 K connections each = 10 M connections. Scale-out at 80% capacity.
- **Message Router / Fan-Out**: Stateless workers; scale via Kafka consumer group (add instances to consumer group, Kafka rebalances partitions).
- **MySQL shards**: Start with 64 shards (one MySQL instance per shard). Shard by `workspace_id % 64`. Scale to 256 shards by online reshard (Vitess supports online resharding; alternatively, split shard by cloning and updating routing table). Each shard: 1 primary + 2 read replicas.
- **Elasticsearch**: Per-workspace clusters for large workspaces; shared clusters for small workspaces with tenant isolation. Add nodes to clusters; rerouting (shard rebalancing) is automatic.
- **Redis Cluster**: Add shards for offline queue, presence, channel member cache.

**DB Sharding:**
- Shard key: `workspace_id`. All entities for a workspace (messages, channels, members) co-located on the same shard → cross-table JOINs within a workspace are single-shard, no cross-shard scatter.
- Cross-workspace operations (rare): user's workspace list → foreign key lookups. Handled by a separate global `workspaces` database (PostgreSQL) that is not sharded.
- Hotspot: a very large workspace (e.g., 500,000-person company) could dominate one shard. Mitigation: large workspaces get a dedicated shard pair; the shard router configuration maps specific workspace_ids to dedicated shards.

**Replication:**
- MySQL: semi-synchronous replication (primary waits for at least 1 replica to ack before returning to client). Read queries routed to read replicas. Primary handles all writes. Replica lag target: < 1 second.
- Failover: Orchestrator (open-source MySQL HA tool) or AWS RDS Multi-AZ — automatic primary promotion within 30 seconds on primary failure.

**Caching:**
- Channel member list: Redis, 60-second TTL. Cache miss (cold start or expiry): reads from MySQL replica. Cache warming on channel join/leave events.
- User profile: Redis, 1-hour TTL. Invalidated on profile update.
- Recent messages (last 100 per channel): Redis LRU cache. Serves most conversation.history requests without MySQL query. TTL: 5 minutes.
- Unread counts: Redis HASH per (user_id, workspace_id). Updated on message receive and read receipt. Persisted to MySQL asynchronously (every 30 seconds).

**CDN:**
- Static assets (Electron app, web bundle, emoji sprites): Cloudflare CDN with 24-hour cache.
- File downloads: S3 presigned URLs proxied through CloudFront. Cache-Control: private (per-user presigned URL). Thumbnail images: public CDN cache with 1-hour TTL.

### Interviewer Q&As on Scaling

1. **Q: How do you shard messages when a Slack workspace can have billions of messages over years?**
   A: The messages table is sharded by `workspace_id`. Within a shard, the messages for all workspaces assigned to it are co-located. The shard's message table has a composite index on `(channel_id, ts DESC)` for timeline queries — this is efficient regardless of total table size because queries are always filtered by `channel_id`. For very large workspaces, time-based table partitioning (MySQL RANGE partitioning by `ts`) within the shard keeps hot data (recent months) in fast SSDs and cold data (older) on cheaper storage or archived to S3 via a tiered storage approach (pt-archiver or custom tooling). Read replicas serve all history queries, keeping write primary unloaded.

2. **Q: How does Slack handle 100,000-member channels (Slack Enterprise features like Connect channels)?**
   A: Large channels require a tiered fan-out strategy: (1) Message published to Kafka. (2) Fan-Out Service uses Redis Pub/Sub to publish to `ws_channel:{channel_id}` — all subscribed Gateway servers receive it simultaneously. (3) Each Gateway pushes to its subset of connected members. This is O(num_online_members) in total work but parallelized across ~200+ Gateway servers. For notification delivery to offline members, a background job paginates through channel members in batches of 5,000 and enqueues offline messages — this is acceptable because offline delivery doesn't need to be real-time. The channel membership cache must be distributed to avoid Redis memory overload: one sorted set per channel with member_ids is manageable (100 K × 8 bytes = 800 KB per channel). With 100 large channels, this is 80 MB — trivial.

3. **Q: How would you scale the search indexing pipeline to handle a large Slack migration (importing 5 years of messages)?**
   A: Bulk historical indexing via Elasticsearch's Bulk API with high-throughput indexing configuration: (1) Disable replicas during initial index (single replica = 0 during bulk), re-enable after. (2) Set `refresh_interval = -1` (disable auto-refresh during bulk). (3) Use 10 Gbps network to Elasticsearch cluster. (4) Bulk batch size = 5,000 documents × 500 bytes = 2.5 MB per request — within ES recommended range. (5) Rate: a 20-node ES cluster with 8 cores/node can process ~50 K documents/sec during bulk. A 5-year message history for a 10,000-person workspace = ~10 M messages = 200 seconds to index. For large enterprises (1 B messages), ~5.5 hours for initial index. Run this offline before making search available to the workspace. For ongoing production indexing, re-enable replicas and `refresh_interval = 1s`.

4. **Q: How do you handle MySQL shard rebalancing when you need to add new shards?**
   A: We use consistent hashing (or a lookup table) for workspace-to-shard mapping. Adding shards via a lookup table approach: (1) New shard added to configuration. (2) Select workspaces to migrate (prefer smallest workspaces to validate the process). (3) Copy workspace data to new shard (mysqldump or pt-table-sync). (4) Enable dual-write: writes go to both old and new shard. (5) Verify data parity between shards. (6) Update routing table: new writes and reads go to new shard. (7) Remove workspace data from old shard after cool-down period. This can be done live with Vitess's VReplication, which provides managed schema migrations and shard splits. Vitess is Slack-scale ready (YouTube, GitHub use it).

5. **Q: How do you handle the read amplification of channel.history requests when everyone opens Slack Monday morning?**
   A: Monday morning at 9 AM in a 10,000-person company: all 10,000 users open Slack and request recent channel history (last 100 messages in 10 channels each = 100,000 concurrent history requests). This is the thundering herd problem. Mitigations: (1) **Redis message cache**: last 100 messages per channel cached in Redis (as a sorted set keyed by channel_id, scored by ts). Cache hit rate: >95% since the same recent messages are requested by all users. Reduces MySQL load by 95%. (2) **Client-side cache**: Electron app persists last viewed state locally (IndexedDB/SQLite); only fetches delta since last seen. Most users fetching only messages since Friday means very small payloads. (3) **Connection staggering**: Slack's client has a built-in reconnect delay with jitter; the gateway tier doesn't all connect simultaneously. (4) **Read replica scaling**: auto-scale read replicas ahead of business hours using scheduled scaling policies.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| Gateway server crash | Connected users lose connection; ~20 K users disconnected | TCP RST; health check fails in 10 s | Clients reconnect (exponential backoff); offline queue preserves messages; rolling replacement via ASG |
| MySQL shard primary failure | Writes to that workspace's shard fail; read replicas still serve reads | Orchestrator/Patroni health check within 5 s | Promote replica within 30 s; writes queue client-side or return 503 with retry-after header |
| Kafka broker failure | Message routing delayed if partition leader unavailable | Kafka controller detects leader failure in <30 s | Kafka replicas (RF=3) elect new leader automatically; consumer groups reconnect and resume |
| Elasticsearch node failure | Search may return partial results | ES health API; Green/Yellow/Red status | Replica shard takes over; Yellow status (replica not yet allocated) still serves queries from primary shards |
| Redis shard failure | Offline queue, channel cache, presence unavailable for ~5-10% of users | Redis Cluster failover detection <5 s | Replica promoted automatically; presence falls back to "unknown"; channel cache cold-starts from MySQL |
| Fan-out service overload | Message delivery delayed; Kafka lag grows | Kafka consumer lag metric | Auto-scale Fan-Out instances; large-channel fan-out switches to degraded mode (best-effort delivery) |
| File service / S3 outage | File uploads fail; downloads fail | S3 health checks; upload error rate spike | Retry uploads with backoff; serve from CDN cache for downloads; file-only messages degrade gracefully |
| Notification service failure | Push notifications not sent | Error rate on APNs/FCM calls | Retry queue (SQS FIFO); messages still delivered via WebSocket for online users; degraded experience for offline users only |
| Global Elasticsearch outage | Search unavailable | Health endpoint monitoring | Return graceful error to users ("search temporarily unavailable"); messages still sent/received; search is non-critical path |

### Idempotency and Retry

- **Message send**: Client generates `client_msg_id` (UUID). Server uses UNIQUE constraint `(workspace_id, client_msg_id)` on messages table. Duplicate sends (client retry on timeout) insert-ignore or return existing message. Idempotent by design.
- **Kafka consumption**: Channel Fanout Service uses at-least-once semantics. Fan-out messages may be delivered twice if consumer crashes between processing and committing offset. Gateway deduplicates using `message_ts + channel_id` in a short TTL Redis set (10-second window) before pushing to WebSocket.
- **Webhook delivery**: Webhook Service uses SQS with visibility timeout; if delivery fails, message becomes visible again after timeout. Max 5 retries with exponential backoff. After 5 failures, route to DLQ and alert workspace admin.
- **Circuit breaker**: Gateway → Fan-Out Service gRPC calls: circuit breaker opens after 50% error rate over 10 seconds. Fallback: write to offline queue directly. Circuit probes every 30 seconds for recovery.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `gateway.connections.active` | Gauge | Alert > 80% per server |
| `message.publish.latency_p99` | Histogram | Alert > 100 ms |
| `message.delivery.latency_p99` | Histogram | Alert > 200 ms |
| `kafka.consumer_lag.fanout` | Gauge | Alert > 10,000 messages |
| `mysql.replication_lag_seconds` | Gauge | Alert > 5 s |
| `mysql.query.p99_latency` | Histogram | Alert > 100 ms |
| `elasticsearch.indexing_latency_p99` | Histogram | Alert > 5 s |
| `elasticsearch.search_latency_p99` | Histogram | Alert > 500 ms |
| `redis.memory_used_pct` | Gauge | Alert > 80% |
| `file_upload.success_rate` | Counter Rate | Alert < 99% |
| `notification.delivery_rate` | Counter Rate | Alert < 95% |
| `websocket.error_rate` | Counter Rate | Alert > 0.5% |
| `search.query_error_rate` | Counter Rate | Alert > 1% |
| `slash_command.timeout_rate` | Counter Rate | Alert > 5% |

### Distributed Tracing

All services instrument with OpenTelemetry. Trace context propagated via Kafka message headers (`traceparent` W3C format) and gRPC metadata. Key spans:
- `gateway.receive_message`
- `kafka.publish`
- `fanout.resolve_members`
- `gateway.push_to_socket`
- `mysql.write_message`
- `elasticsearch.index_document`

Sampling: 100% error traces; 0.1% success traces (at 37 K msg/s, 0.1% = 37 traces/sec — manageable). Exported to Jaeger or Datadog APM.

### Logging

Structured JSON logs. Fields: `trace_id`, `workspace_id`, `channel_id`, `user_id` (SHA-256 hashed for PII), `action`, `duration_ms`, `status_code`, `error_code`. Log aggregation: Filebeat → Elasticsearch (separate cluster from search). Retention: 7 days hot, 30 days cold (S3 Glacier).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Why Chosen | What You Give Up |
|---|---|---|---|---|
| Primary DB | MySQL (sharded) | Cassandra | ACID transactions; Slack's historical choice; complex queries within workspace | Horizontal scale ceiling per shard; reshard complexity |
| Event bus | Kafka | Redis Pub/Sub, RabbitMQ | Durable; replayable; fan-out to multiple consumers; at-least-once delivery | Operational complexity; latency vs direct push |
| Search | Elasticsearch | MySQL FULLTEXT | Purpose-built for text search; BM25 relevance; horizontal scale; aggregations | Eventual consistency (5s lag); additional infrastructure cost |
| Fan-out for large channels | Redis Pub/Sub broadcast | Direct gRPC to each member | Collapses fan-out to O(num_gateways) vs O(num_members); scales to 100 K member channels | Redis as critical path for large channels; message loss if Redis pub fails |
| Message ordering | Server-assigned ts | Client timestamp | Canonical ordering; prevents clock skew disagreements | Latency of server round-trip before message appears in own UI (Slack optimistically shows message immediately, then reconciles) |
| Thread model | Hybrid (messages + thread_summary) | Flat messages with parent_id | Fast thread preview without aggregation query; atomic counter updates | Dual-write on thread reply; row contention on hot threads |
| Access control in search | member_ids field in ES doc | Post-query filtering from DB | In-ES filtering avoids DB lookup per search result; scales with query complexity | `member_ids` field maintenance overhead on channel joins/leaves; larger index size |
| File storage | S3 + CDN + presigned URLs | Own storage cluster | Durability; managed scaling; CDN integration; no operational overhead | Egress cost; vendor dependency |

---

## 11. Follow-up Interview Questions

1. **Q: How would you implement message pinning in a channel?**
   A: Add a `pins` table: `(workspace_id, channel_id, message_ts, pinned_by, pinned_at)`. GET channel pins = query this table. Max 100 pins per channel (product limit). On pin, send a `message_pinned` system message in the channel (just like a join/leave system message). Fanout via same pipeline as regular messages. Pin info denormalized into channel metadata for quick display in the channel header. Cache the pins list (small, < 100 entries) in Redis.

2. **Q: How do you implement message retention policies — what happens when messages expire?**
   A: A scheduled `RetentionJob` service runs daily. For each workspace with a retention policy (e.g., 90 days), it queries the messages shard: `DELETE FROM messages WHERE workspace_id = X AND ts < (NOW - 90 days)`. Runs in batches of 10,000 rows to avoid large transactions. Also deletes from Elasticsearch (bulk delete by range query on `ts`). Files: separate job checks `files.expires_at < NOW()`, deletes from S3 and file metadata DB. Compliance export (Enterprise): before deletion, messages compressed and written to S3 compliance bucket (separate, auditor-accessible). Retention job idempotent: re-running doesn't double-delete. Metrics: `retention_job.rows_deleted`, `retention_job.duration_ms` per workspace.

3. **Q: How do you design Slack's @here and @channel notification system?**
   A: `@here` notifies all active (non-DND) members currently online in the channel. `@channel` notifies ALL channel members regardless of presence. Implementation: (1) Parser detects `@here` / `@channel` in message text. (2) Fan-Out Service switches to a "broadcast" mode: instead of notifying only followers, it notifies all channel members. (3) For `@here`: filter by presence status (Redis presence lookup) — only deliver push notifications to online members. For `@channel`: send push notifications to ALL members, including offline. (4) Rate limit: workspace admins can restrict `@channel` to admins only. API validates permission before sending. (5) Notification throttle: if a user receives `@channel` in 10 channels simultaneously, batch into one notification.

4. **Q: How do you handle Slack's "unfurling" — link previews — without leaking user URLs to third parties?**
   A: Unfurling: when a URL is posted, Slack fetches the page server-side to generate a preview (title, description, image). (1) Message posted with URL detected by parser. (2) Unfurl Service (server-side) fetches the URL from Slack's servers — the external site sees Slack's IP, not the user's IP. This preserves user privacy. (3) Response cached by URL (Redis, 30-minute TTL) to avoid re-fetching the same URL posted to multiple channels. (4) Preview rendered as a Block Kit attachment appended to the message (message.changed event pushed to channel). (5) For private/enterprise deployments: unfurling can be disabled per workspace (some enterprises don't want external fetches). (6) For Slack app unfurls: apps can register URL patterns and receive an `unfurl` event, responding with custom rich previews — no server-side fetch needed.

5. **Q: How does Slack's rate limiting work across free vs. paid tiers?**
   A: Rate limits are enforced at the API Gateway level using a token bucket per (app token, workspace, endpoint tier). Tier system: Tier 1 = 1 call/minute (very low priority), Tier 2 = 20 calls/minute, Tier 3 = 50 calls/minute, Tier 4 = 100 calls/minute. Free workspaces: Tier 3 for most endpoints. Paid: higher limits or burst allowances. When limit exceeded: HTTP 429 with `Retry-After` header (seconds to wait). Implementation: Redis INCR + EXPIRE (sliding window) or Redis sorted set (sliding window with exact precision). Token buckets allow burst: a user can exceed 50/min briefly if they have accumulated tokens from quiet periods. Enterprise: dedicated rate limit configs per API token managed in a rate-limit config database.

6. **Q: How do you design "Slack reminders" (/remind)?**
   A: Reminders are a scheduled delivery system. (1) User types `/remind me to call Alice in 2 hours`. Slash command handler parses the time expression (NLP or duckling parser). (2) Stores reminder in `reminders` table: `(reminder_id, user_id, workspace_id, remind_at, message_text)`. (3) Scheduled job runs every minute, queries `reminders WHERE remind_at <= NOW() AND delivered = false`. (4) For each due reminder, Fan-Out Service delivers a DM from the Slackbot user to the creator. (5) Updates reminder row: `SET delivered = true`. (6) At scale (10 M users × avg 1 reminder = 10 M reminders): per-minute job scans an index on `remind_at` — efficient range scan. Index: `INDEX idx_remind_at (remind_at) WHERE delivered = false`. Partitioned by minute to avoid full-table scans. Alternative: use a delay queue (SQS + DelaySeconds) or a time-wheel algorithm in Redis for sub-minute precision.

7. **Q: How would you implement Slack's compliance export feature for Enterprise Grid?**
   A: Enterprise admins can request exports of all workspace messages within a date range. (1) Admin requests export via API: `POST /api/admin.conversations.export` with date range and channel filter. (2) Export Job Service schedules an async job. (3) Job reads messages from MySQL shard in batches (1,000 rows/batch) ordered by `(channel_id, ts)`. (4) Each batch serialized to JSON Lines format (one JSON object per line). (5) Gzipped batches written to a dedicated compliance S3 bucket (separate AWS account, restricted access, 7-year retention, WORM policy via S3 Object Lock). (6) Job generates a manifest file listing all export files. (7) Admin notified via webhook/email with presigned download URL. (8) Files expire from S3 after download (or 30-day window). Encryption: export files encrypted with customer-managed KMS key (EKM). Progress: job updates a status table queried by admin dashboard.

8. **Q: How does Slack handle the case where the same user is logged in on multiple devices (laptop + phone)?**
   A: Each device has a separate session token and therefore a separate WebSocket connection. Each connection is registered in the connection registry (`conn:{user_id}:{session_id}` → `gateway_server_id`). When a message is sent to User A, the Fan-Out Service looks up ALL of User A's active connections (by querying the connection registry for all keys matching `conn:{user_id}:*`). All connections receive the message. The sender's other devices also receive the sent message (so the conversation view stays in sync across devices). Read receipts (`channel_marked` events) are synced across devices: when User A reads a channel on the laptop, the phone also updates its unread count via a `channel_marked` WebSocket event pushed to all User A's connections.

9. **Q: Describe how you would implement full-text search with AND/OR/NOT operators and phrase matching.**
   A: Elasticsearch's query DSL natively supports all of these: (1) **AND**: multiple `match` clauses in a `bool.must` array. (2) **OR**: multiple `match` clauses in a `bool.should` array with `minimum_should_match: 1`. (3) **NOT**: `bool.must_not` clause. (4) **Phrase matching**: `match_phrase` query: `{ "match_phrase": { "text": "deployment failed" } }` — matches only documents with the exact phrase. (5) **Proximity matching** (near phrase): `match_phrase_prefix` or `span_near` for "deployment" near "failed" within N words. (6) **Field-specific**: `in:#channel` maps to an ES `filter.term` on `channel_id`; `from:@user` maps to `filter.term` on `user_id`. The query parser translates the user's query string into the appropriate ES bool query. At Slack's scale, a single boolean query on ES returns results in <100ms for indexes up to 100 M documents.

10. **Q: How do you handle workspace creation at Slack's current scale — is it a bottleneck?**
    A: Workspace creation is a low-frequency operation (maybe 10,000 new workspaces/day globally). Steps: (1) Insert into global `workspaces` PostgreSQL DB. (2) Assign the workspace to a MySQL shard (least-loaded shard by message count, or round-robin for new workspaces). (3) Create the default `#general` channel. (4) Create initial admin user + workspace_member record. (5) Provision Elasticsearch index for the workspace (index creation takes ~500ms on a healthy ES cluster). (6) Initialize Redis namespace for workspace. Total: ~1-2 seconds. No bottleneck at 10,000 workspaces/day = 0.12/sec. The shard assignment is stored in a `workspace_shard_routing` table in the global PostgreSQL DB, consulted by all API services to route queries to the correct shard.

11. **Q: What are the consistency trade-offs in Slack's unread count feature?**
    A: Unread counts are inherently approximate in a distributed system. The count = (number of messages in channel since user's `last_read_ts`). Implementation: (1) `channel_members.last_read_ts` updated when user marks a channel as read (or scrolls to bottom). (2) Unread count computed as `COUNT(*) WHERE channel_id = X AND ts > last_read_ts` — expensive at query time. Solution: cache in Redis: `unread:{user_id}:{channel_id}` HINCRBY incremented by Fan-Out Service on message delivery, reset to 0 on read. (3) Consistency trade-off: if Redis is stale (node failure, TTL expiry), the count is recomputed from MySQL — eventually consistent. If Fan-Out increments Redis before MySQL confirms the message, count could be off by 1. Acceptable for UX: a ±1 unread count discrepancy is invisible to users. The "badge" number on the app icon uses the sum of all unread counts, also cached in Redis.

12. **Q: How would you implement Slack's "Do Not Disturb" mode?**
    A: DND suppresses push notifications during a configured time window (e.g., 10 PM – 8 AM). (1) User sets DND schedule in preferences. (2) Notification Service checks user's DND status before sending APNs/FCM. (3) DND status: Redis key `dnd:{user_id}` with value = DND end timestamp. Set when DND starts, expires at DND end. (4) During DND, push notifications queued in a `dnd_queue:{user_id}` Redis list instead of sent. (5) When DND ends (Redis key expires), a wake-up job processes the queue and sends notifications. (6) Urgency override: `@channel` with high priority can break through DND (configurable by user). (7) "Pause notifications" (manual DND): same mechanism but indefinite. Manual DND cleared when user explicitly disables it (DELETE `dnd:{user_id}` key). (8) Channel-level muting (`is_muted = true` in channel_members): distinct from DND — muted channels never send notifications regardless of DND. Notification Service checks both `is_muted` and DND before dispatching.

13. **Q: How do Slack's incoming webhooks work at scale?**
    A: Incoming webhooks: external services POST a JSON message to a unique webhook URL, which creates a message in the configured channel. (1) Webhook URL format: `https://hooks.slack.com/services/T01234/B01234/{token}`. (2) Webhook Service receives the POST, looks up the token in `incoming_webhooks` table (indexed on token — a fast key lookup). (3) Validates the payload (JSON schema). (4) Passes the message to the normal message pipeline (Kafka → Fan-Out → MySQL). (5) Returns HTTP 200 `ok` or error. Idempotency: webhook callers often retry on timeout; Slack doesn't deduplicate incoming webhooks (no idempotency key in the request by default). Rate limit: 1 request/second per webhook URL (to prevent abuse). At scale: 100 K active webhooks each sending 1 req/sec = 100 K requests/sec. Webhook Service is stateless and horizontally scalable; the bottleneck is the downstream message pipeline, which handles these as regular messages.

14. **Q: How would you handle a situation where a Slack channel receives 100,000 messages in 10 seconds (e.g., a bot flood or coordinated spam)?**
    A: Rate limiting at multiple layers: (1) **API Gateway**: per-token rate limit (Tier 3: 50 messages/sec for bots). A bot flooding 100,000 messages in 10 seconds = 10,000 msg/sec → immediately rate-limited after the first 50. Returns HTTP 429. (2) **Per-channel rate limit**: Slack enforces a maximum message rate per channel (e.g., 1 message/second per user in a channel, configurable by workspace admin). Excess messages dropped or queued. (3) **Workspace-level circuit breaker**: if a workspace generates > 10× its normal message rate, temporarily throttle all inbound traffic for that workspace — protecting the shared shard from hot-spot overload. (4) **Admin action**: workspace admins receive an alert; can deactivate the bot account. (5) **Slow mode**: channel setting introduced by Slack that enforces a minimum time between messages per user in a channel (e.g., 5 seconds). Effective against human spammers. All these are applied before the message enters the MySQL write path, preventing shard overload.

15. **Q: How would you design Slack's app directory and OAuth flow for third-party app installation?**
    A: (1) Developer registers app at api.slack.com: provides name, redirect URLs, OAuth scopes, slash commands, event subscriptions. Stored in `apps` table. (2) User clicks "Add to Slack": redirected to Slack's OAuth authorization page with `client_id`, `scope`, `state` params. (3) User approves: Slack redirects to app's redirect URL with an authorization `code`. (4) App exchanges `code` for `access_token` via `POST /api/oauth.v2.access` with `client_id` + `client_secret`. (5) Slack issues a bot token (`xoxb-...`) and (if user scopes requested) a user token (`xoxp-...`). Stored in `app_installations` table: `(app_id, workspace_id, bot_token_encrypted, installed_by, scopes)`. (6) Token scopes enforced: API calls with the bot token checked against granted scopes in the authorization middleware. (7) Token rotation: tokens don't expire but can be revoked by user. Revocation invalidates the installation row. (8) At scale: tokens verified via a JWT-signed lookup in Redis cache (token → scopes mapping), reducing DB lookups on every API call.

---

## 12. References & Further Reading

- **Slack Engineering Blog — "Flannel: Application-Level Edge Caching"**: https://slack.engineering/flannel-an-application-level-edge-cache-to-make-slack-scale/
- **Slack Engineering Blog — "Rebuilding Slack on the Desktop"**: https://slack.engineering/rebuilding-slack-on-the-desktop/
- **Slack Engineering Blog — "Real-Time Messaging"**: https://slack.engineering/real-time-messaging/
- **Slack Engineering Blog — "Scaling Slack's Job Queue"**: https://slack.engineering/scaling-slacks-job-queue/
- **Slack Engineering Blog — "How We Design Our APIs at Slack"**: https://slack.engineering/how-we-design-our-apis-at-slack/
- **MySQL Vitess (sharding framework)**: PlanetScale / Vitess. https://vitess.io/
- **Elasticsearch: The Definitive Guide**: Clinton Gormley & Zachary Tong. O'Reilly. https://www.elastic.co/guide/en/elasticsearch/guide/current/index.html
- **Kafka: The Definitive Guide**: Gwen Shapira et al. O'Reilly. https://www.confluent.io/resources/kafka-the-definitive-guide/
- **Slack API Documentation**: https://api.slack.com/docs
- **WebSocket RFC 6455**: https://www.rfc-editor.org/rfc/rfc6455
- **BM25 Ranking Function**: Robertson, S. & Zaragoza, H. (2009). "The Probabilistic Relevance Framework: BM25 and Beyond." Foundations and Trends in Information Retrieval.
- **Elasticsearch Index Lifecycle Management**: https://www.elastic.co/guide/en/elasticsearch/reference/current/index-lifecycle-management.html
- **Orchestrator (MySQL HA)**: https://github.com/openark/orchestrator
- **Token Bucket Rate Limiting**: Wikipedia. https://en.wikipedia.org/wiki/Token_bucket
