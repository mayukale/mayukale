# System Design: WhatsApp

---

## 1. Requirement Clarifications

### Functional Requirements

1. **1-to-1 messaging** — Users can send text, images, video, audio, and documents to another user.
2. **Group messaging** — Groups of up to 1,024 members; any member can send a message visible to all.
3. **Message delivery receipts** — Three-tier status: sent (single tick), delivered (double tick), read (blue double tick).
4. **Offline message queuing** — Messages sent while a recipient is offline must be queued and delivered when they reconnect; queue retained for 30 days.
5. **End-to-end encryption (E2EE)** — Messages encrypted on-sender device; only recipient device can decrypt. Server never sees plaintext.
6. **Presence indicators** — "Online", "Last seen at <timestamp>", typing indicators ("…is typing").
7. **Media storage and delivery** — Media uploaded to blob storage; encrypted media URL shared in message payload.
8. **Push notifications** — Notify recipients on mobile via APNs/FCM when app is backgrounded.
9. **Voice and video calls** — Out of scope (see below) for this design, but signaling layer noted.
10. **Message deletion** — "Delete for me" and "Delete for everyone" within a time window.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Availability | 99.99% (≤ 52 min/year downtime) |
| Message delivery latency (p99) | < 500 ms for online recipients |
| Message ordering | Per-conversation monotonic order |
| Durability | Zero message loss; at-least-once delivery |
| Consistency model | Eventual consistency; strong per-conversation ordering |
| Security | E2EE with Signal Protocol; keys never leave devices |
| Scale | 2 billion MAU; 100 billion messages/day |
| Media storage | Exabyte-scale; deduplicated |
| Encryption key exchange | Asynchronous, pre-key bundles |

### Out of Scope

- Voice/video call media relay (TURN/STUN infrastructure), though signaling is noted.
- Payments (WhatsApp Pay).
- Status stories (ephemeral media).
- Business API and catalog.
- Desktop client specifics.

---

## 2. Users & Scale

### User Types

| Type | Description |
|---|---|
| **Regular User** | Sends/receives 1:1 and group messages via mobile app |
| **Group Admin** | Creates groups, manages membership, sets permissions |
| **Business Account** | Verified account with higher-rate API access (out of scope for core design) |

### Traffic Estimates

**Assumptions:**
- 2 billion MAU, 1.5 billion DAU (75% DAU/MAU ratio, consistent with WhatsApp's reported figures).
- 100 billion messages/day (WhatsApp-reported figure).
- Average message size: 1 KB text payload (before encryption overhead).
- Media messages: 20% of total → 20B media messages/day.
- Average media size: 500 KB (mix of images ~200 KB and videos ~2 MB).
- Group messages: 30% of messages; average group size 50 → each group message fans out to 49 additional recipients.
- Peak-to-average ratio: 3x.

| Metric | Calculation | Result |
|---|---|---|
| Messages/day | Given | 100 B/day |
| Messages/sec (avg) | 100 B / 86,400 | ~1.16 M msg/s |
| Messages/sec (peak) | 1.16 M × 3 | ~3.5 M msg/s |
| Group fan-out deliveries/sec (avg) | 30% × 1.16 M × 49 | ~17 M delivery events/s |
| Media uploads/day | 20% × 100 B | 20 B/day |
| Media upload throughput (avg) | 20 B × 500 KB / 86,400 | ~116 GB/s |
| Media upload throughput (peak) | ~116 GB/s × 3 | ~348 GB/s |
| New connections/sec (morning spike) | 1.5 B users / 3 h × 10% reconnect | ~14 K connections/s |
| Concurrent WebSocket connections | 1.5 B DAU × 30% online at peak | ~450 M persistent connections |

### Latency Requirements

| Operation | P50 | P99 | Notes |
|---|---|---|---|
| Message delivery (online) | 100 ms | 500 ms | Network + server fan-out |
| Message delivery (push notification) | 500 ms | 2 s | APNs/FCM add ~300 ms |
| Media upload ack | 200 ms | 1 s | CDN edge ingest |
| Presence update propagation | 500 ms | 3 s | Gossip-based, eventual |
| Key bundle fetch | 50 ms | 200 ms | Cached at edge |
| Read receipt delivery | 200 ms | 1 s | Same path as messages |

### Storage Estimates

**Assumptions:**
- Messages stored for 30 days on server only if undelivered; once delivered, ephemeral on server.
- Media stored encrypted for 30 days post-upload.
- Message metadata (routing, receipts) stored permanently for support.

| Data Type | Calculation | Daily Volume | 1-Year Volume |
|---|---|---|---|
| Text message payloads (undelivered buffer) | 100 B × 1 KB × avg 5% undelivered at any moment | 5 TB in-flight | — |
| Media storage | 20 B/day × 500 KB | 10 PB/day | 3.65 EB/year |
| Message metadata rows | 100 B × 200 bytes/row | 20 TB/day | 7.3 PB/year |
| Encryption key bundles | 2 B users × 10 pre-keys × 100 bytes | — | ~2 TB (static) |
| Group metadata | 1 B groups × 10 KB avg | — | ~10 TB (static) |

### Bandwidth Estimates

| Direction | Calculation | Throughput |
|---|---|---|
| Inbound text (avg) | 1.16 M msg/s × 1 KB | ~1.16 GB/s |
| Outbound text to online recipients (avg) | 1.16 M × 1 KB × avg 1.5 recipients | ~1.74 GB/s |
| Inbound media upload (avg) | 116 GB/s | 116 GB/s |
| Outbound media download (avg, 3x read amplification) | 116 GB/s × 3 | ~348 GB/s |
| Peak total egress | (1.74 + 348) × 3 | ~1.05 TB/s |

---

## 3. High-Level Architecture

```
                            ┌─────────────────────────────────────────────────────────┐
                            │                    Client Layer                         │
                            │   iOS App          Android App          Web Client      │
                            │  (Signal SDK)      (Signal SDK)        (Signal SDK)     │
                            └────────────┬──────────────┬──────────────┬─────────────┘
                                         │              │              │
                                   (TLS/WSS)      (TLS/WSS)      (TLS/WSS)
                                         │              │              │
                            ┌────────────▼──────────────▼──────────────▼─────────────┐
                            │               Global Load Balancer (Anycast)            │
                            │         (Route53 latency-based + L4 NLB per region)     │
                            └──────────────────────────┬──────────────────────────────┘
                                                       │
                       ┌───────────────────────────────┼─────────────────────────────────┐
                       │                               │                                 │
          ┌────────────▼────────────┐    ┌─────────────▼────────────┐   ┌───────────────▼──────────────┐
          │   Connection Gateway    │    │   HTTP/REST API Gateway   │   │     Media Gateway            │
          │   (WebSocket servers)   │    │   (Key exchange, profile, │   │  (Upload/download media)     │
          │   ~10K servers          │    │    group mgmt, auth)      │   │  Chunked upload, encryption  │
          │   ~45K conns each       │    └───────────┬───────────────┘   └───────────────┬──────────────┘
          └────────────┬────────────┘               │                                   │
                       │                            │                                   │
          ┌────────────▼────────────────────────────▼───────────────┐                  │
          │                   Message Router Service                 │                  │
          │  - Looks up recipient connection location               │                  │
          │  - Fan-out for group messages                           │                  │
          │  - Writes to offline queue if recipient offline         │                  │
          └──┬──────────────────────────────────────────────────────┘                  │
             │                                                                          │
     ┌───────┴──────────────────────────────────────────┐                              │
     │                                                  │                              │
┌────▼───────────┐   ┌────────────────┐   ┌────────────▼──────────┐   ┌───────────────▼──────────────┐
│  Message Store │   │  Offline Queue │   │  Presence Service     │   │     Media Store              │
│  (Cassandra)   │   │  (Redis/SQS)   │   │  (Redis cluster)      │   │  (S3-compatible object store)│
│  - Receipts    │   │  - 30-day TTL  │   │  - Online/offline     │   │  - Encrypted blobs           │
│  - Metadata    │   │  - Per-user Q  │   │  - Typing indicators  │   │  - CDN-fronted               │
└────────────────┘   └────────────────┘   └───────────────────────┘   └──────────────────────────────┘
             │
     ┌───────▼──────────┐   ┌────────────────────┐   ┌──────────────────────┐
     │   Key Service    │   │  Notification Srv  │   │  Group Service       │
     │  (Pre-key store) │   │  (APNs/FCM bridge) │   │  (Membership, admin) │
     │  Per-user bundles│   │  Fanout to devices │   │  Cassandra-backed    │
     └──────────────────┘   └────────────────────┘   └──────────────────────┘
```

**Component Roles:**

- **Connection Gateway**: Maintains long-lived WebSocket connections (one per device). Responsible for receiving encrypted message envelopes from senders and forwarding to the Message Router. Stateful — each server knows which user IDs are connected to it. Registers itself in a distributed connection registry (Redis) on connection.
- **HTTP/REST API Gateway**: Handles all non-real-time operations — registration, key bundle upload/fetch, profile updates, group CRUD, media URL generation (presigned URLs).
- **Media Gateway**: Accepts chunked encrypted media uploads, stores them to object storage, returns a content-addressed URL (SHA-256 of ciphertext). Never decrypts content.
- **Message Router Service**: Core fan-out engine. For 1:1 messages, looks up recipient's gateway server from connection registry and forwards. For group messages, fetches member list from Group Service and fans out to each member. If recipient offline, enqueues to Offline Queue.
- **Message Store (Cassandra)**: Stores message metadata (not plaintext), delivery receipts, and message IDs for deduplication. Partitioned by conversation_id.
- **Offline Queue**: Per-user message queue with 30-day TTL. Redis Sorted Set (scored by enqueue timestamp) or SQS FIFO queue. Drained when user reconnects.
- **Presence Service**: Redis-backed; tracks last heartbeat per user. Publishes presence changes to interested subscribers (e.g., users who have an open chat with this user).
- **Key Service**: Stores Signal Protocol pre-key bundles. Provides one-time pre-keys for X3DH key agreement. Replenished by client when supply drops below threshold.
- **Notification Service**: Receives delivery failure events (user offline) and sends APNs/FCM push notifications with an opaque payload (no message content in notification, just a "you have a new message" signal).
- **Group Service**: Manages group membership, admin roles, group metadata. Used by Message Router to resolve group membership at fan-out time.

**Primary Use-Case Data Flow (sending a 1:1 message):**

1. Sender app encrypts message using recipient's Signal Protocol session (if no session, fetches pre-key bundle from Key Service first via HTTP API).
2. Sender app sends encrypted envelope to Connection Gateway via WebSocket.
3. Connection Gateway forwards envelope to Message Router Service.
4. Message Router looks up recipient in connection registry:
   - **Online path**: Routes to recipient's gateway server → gateway pushes to recipient's WebSocket.
   - **Offline path**: Writes to recipient's Offline Queue; triggers Notification Service to send silent push.
5. Message Router writes metadata row to Message Store (Cassandra) with status = SENT.
6. Connection Gateway sends SENT acknowledgment back to sender (single tick).
7. Recipient's gateway sends DELIVERED receipt back through Message Router → sender's gateway → sender app (double tick).
8. Recipient app displays and decrypts message; sends READ receipt (blue ticks).

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Users
-- ============================================================
CREATE TABLE users (
    user_id        UUID        PRIMARY KEY,
    phone_number   TEXT        UNIQUE NOT NULL,  -- E.164 format
    display_name   TEXT        NOT NULL,
    avatar_url     TEXT,
    public_key     BYTEA       NOT NULL,          -- Identity public key (Signal)
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at   TIMESTAMPTZ,
    account_status TEXT        NOT NULL DEFAULT 'active' -- active | suspended | deleted
);

-- ============================================================
-- Pre-Key Bundles (Key Service — stored separately, high read)
-- ============================================================
CREATE TABLE pre_key_bundles (
    user_id              UUID    NOT NULL,
    device_id            INT     NOT NULL,
    registration_id      INT     NOT NULL,
    identity_key         BYTEA   NOT NULL,   -- IK public key
    signed_pre_key_id    INT     NOT NULL,
    signed_pre_key       BYTEA   NOT NULL,   -- SPK public key
    signed_pre_key_sig   BYTEA   NOT NULL,   -- SPK signature
    one_time_pre_key_id  INT,                -- OPK id (nullable: may be exhausted)
    one_time_pre_key     BYTEA,              -- OPK public key
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, device_id)
);

-- One-time pre-keys stored separately so they can be consumed atomically
CREATE TABLE one_time_pre_keys (
    user_id    UUID  NOT NULL,
    device_id  INT   NOT NULL,
    key_id     INT   NOT NULL,
    public_key BYTEA NOT NULL,
    consumed   BOOL  NOT NULL DEFAULT false,
    PRIMARY KEY (user_id, device_id, key_id)
);

-- ============================================================
-- Conversations (1:1 and group logical container)
-- ============================================================
CREATE TABLE conversations (
    conversation_id  UUID        PRIMARY KEY,
    conversation_type TEXT       NOT NULL,  -- 'direct' | 'group'
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_message_id  UUID,
    last_message_at  TIMESTAMPTZ
);

CREATE TABLE conversation_participants (
    conversation_id UUID        NOT NULL,
    user_id         UUID        NOT NULL,
    joined_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    role            TEXT        NOT NULL DEFAULT 'member',  -- 'member' | 'admin'
    muted_until     TIMESTAMPTZ,
    PRIMARY KEY (conversation_id, user_id)
);
CREATE INDEX idx_cp_user ON conversation_participants(user_id);

-- ============================================================
-- Messages (Cassandra schema — wide-row model)
-- Partition key: conversation_id
-- Clustering key: (bucket, message_id DESC) for time-range scans
-- ============================================================
-- Cassandra CQL:
CREATE TABLE messages (
    conversation_id  UUID,
    bucket           INT,        -- floor(unix_ts / 86400) — daily bucket to bound partition size
    message_id       TIMEUUID,   -- version-1 UUID encodes timestamp, used for ordering
    sender_id        UUID,
    message_type     TEXT,       -- 'text' | 'image' | 'video' | 'audio' | 'document' | 'deleted'
    ciphertext       BLOB,       -- encrypted message payload (Signal ciphertext)
    media_url        TEXT,       -- encrypted media URL (null for text)
    media_sha256     TEXT,       -- SHA-256 of ciphertext for deduplication
    server_ts        TIMESTAMP,
    PRIMARY KEY ((conversation_id, bucket), message_id)
) WITH CLUSTERING ORDER BY (message_id DESC)
  AND default_time_to_live = 0;  -- messages retained indefinitely in metadata store

-- ============================================================
-- Message Delivery Receipts
-- ============================================================
-- Cassandra CQL:
CREATE TABLE message_receipts (
    conversation_id UUID,
    message_id      TIMEUUID,
    recipient_id    UUID,
    status          TEXT,        -- 'sent' | 'delivered' | 'read'
    status_ts       TIMESTAMP,
    PRIMARY KEY ((conversation_id, message_id), recipient_id)
);

-- ============================================================
-- Groups
-- ============================================================
CREATE TABLE groups (
    group_id       UUID        PRIMARY KEY,
    name           TEXT        NOT NULL,
    description    TEXT,
    avatar_url     TEXT,
    invite_link    TEXT        UNIQUE,
    max_members    INT         NOT NULL DEFAULT 1024,
    created_by     UUID        NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    settings_json  JSONB       NOT NULL DEFAULT '{}'  -- e.g., who_can_send, who_can_add
);

CREATE TABLE group_members (
    group_id    UUID        NOT NULL,
    user_id     UUID        NOT NULL,
    role        TEXT        NOT NULL DEFAULT 'member',  -- 'owner' | 'admin' | 'member'
    joined_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    added_by    UUID,
    PRIMARY KEY (group_id, user_id)
);
CREATE INDEX idx_gm_user ON group_members(user_id);

-- ============================================================
-- Offline Message Queue (Redis data model — not SQL)
-- Key:   offline_queue:{user_id}
-- Type:  Sorted Set, score = enqueue_unix_timestamp
-- Value: serialized MessageEnvelope (protobuf bytes, base64)
-- TTL:   30 days (set on the key, or per-member via lazy expiry)
-- ============================================================

-- ============================================================
-- Presence (Redis data model)
-- Key:   presence:{user_id}
-- Type:  Hash
--   Fields: last_seen_ts, status (online|offline), typing_in_conv
-- TTL:   60 seconds (renewed by client heartbeat every 30 s)
-- ============================================================

-- ============================================================
-- Connection Registry (Redis data model)
-- Key:   conn:{user_id}:{device_id}
-- Type:  String
-- Value: gateway_server_id (e.g., "gw-use1-042")
-- TTL:   90 seconds (renewed by gateway on each heartbeat)
-- ============================================================

-- ============================================================
-- Media Metadata (PostgreSQL)
-- ============================================================
CREATE TABLE media_objects (
    media_id         UUID        PRIMARY KEY,
    sha256_ciphertext TEXT       UNIQUE NOT NULL,  -- dedup key
    storage_key      TEXT        NOT NULL,          -- S3 object key
    content_type     TEXT        NOT NULL,
    size_bytes       BIGINT      NOT NULL,
    uploaded_by      UUID        NOT NULL,
    uploaded_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at       TIMESTAMPTZ NOT NULL            -- 30 days from upload
);
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| **Apache Cassandra** | Messages, receipts | Linearly scalable writes; wide-row model perfectly maps to time-ordered messages per conversation; multi-datacenter replication; tunable consistency (QUORUM for writes); handles 3.5 M writes/s across cluster | No joins; limited query flexibility; compaction overhead; eventual consistency requires careful receipt logic | **Selected** for message store and receipts |
| **PostgreSQL** | Users, groups, media metadata | ACID transactions; rich query capability; JSONB for flexible settings; foreign key integrity for group membership | Vertical scaling ceiling; sharding complex; WAL replication lag under heavy write load | **Selected** for structured relational data (users, groups, media metadata) |
| **Redis Cluster** | Offline queue, presence, connection registry | Sub-millisecond latency; Sorted Sets for ordered queues; TTL support for presence/queue expiry; Pub/Sub for presence change notifications | In-memory cost; persistence via AOF adds latency; not suited for large payloads | **Selected** for ephemeral/hot-path data |
| **Amazon S3 / GCS** | Media blobs | Exabyte scale; 11 nines durability; globally distributed with regional replication; content-addressed storage natural fit; lifecycle policies for 30-day expiry | Not a database; requires metadata layer; egress costs high at scale | **Selected** for encrypted media blobs |
| **MySQL (InnoDB)** | — | Good ACID; widely understood | Row-level locking contention at WhatsApp's write volume; WhatsApp historically moved away from MySQL for message storage | Not selected for hot paths |

**Justification for Cassandra as message store:**
Cassandra's partition model maps directly to the access pattern: "give me the last N messages in conversation X". The partition key `(conversation_id, bucket)` collocates all messages for a daily window on the same node, enabling single-node reads for recent messages. At 3.5 M writes/s, Cassandra's LSM-tree storage engine with no read-before-write semantics on inserts outperforms B-tree engines (PostgreSQL, MySQL) which require page-level locking. The `TIMEUUID` clustering key provides globally unique, time-ordered message IDs without a centralized sequence generator.

---

## 5. API Design

All APIs use HTTPS. WebSocket connections use WSS. Authentication uses a JWT issued at registration, bound to device_id and user_id, signed with server's RSA-2048 key. Rate limits enforced at the API Gateway layer using a token bucket per (user_id, endpoint).

### REST Endpoints

#### Authentication & Registration

```
POST /v1/auth/register
Authorization: None (initial registration)
Request:
{
  "phone_number": "+14155552671",   // E.164
  "registration_id": 12345,
  "identity_key": "<base64>",
  "signed_pre_key": {
    "key_id": 1,
    "public_key": "<base64>",
    "signature": "<base64>"
  },
  "one_time_pre_keys": [
    {"key_id": 1, "public_key": "<base64>"},
    ...  // up to 100 OPKs
  ]
}
Response 201:
{
  "user_id": "uuid",
  "device_id": 1,
  "access_token": "jwt",
  "expires_at": "2026-06-01T00:00:00Z"
}
Rate Limit: 5 requests/phone/hour (SMS OTP verification gates this)
```

```
POST /v1/auth/keys/replenish
Authorization: Bearer <jwt>
Request:
{
  "one_time_pre_keys": [
    {"key_id": 101, "public_key": "<base64>"},
    ...
  ]
}
Response 200: { "accepted": 50 }
Rate Limit: 10 requests/device/day
```

#### Key Bundle Fetch (for E2EE session establishment)

```
GET /v1/keys/{user_id}/{device_id}
Authorization: Bearer <jwt>
Response 200:
{
  "user_id": "uuid",
  "device_id": 1,
  "identity_key": "<base64>",
  "signed_pre_key": { "key_id": 1, "public_key": "<base64>", "signature": "<base64>" },
  "one_time_pre_key": { "key_id": 42, "public_key": "<base64>" }  // consumed atomically
}
Rate Limit: 1000 requests/user/hour (fetching keys for many contacts)
Notes: one_time_pre_key may be absent if exhausted; client must handle fallback (use only SPK)
```

#### Media Upload

```
POST /v1/media/upload-url
Authorization: Bearer <jwt>
Request:
{
  "content_type": "image/jpeg",
  "size_bytes": 204800,
  "sha256_ciphertext": "<hex>"   // SHA-256 of encrypted bytes (for dedup check)
}
Response 200:
{
  "media_id": "uuid",
  "upload_url": "https://media.whatsapp.net/upload/<presigned>",  // expires in 10 min
  "already_exists": false   // true if dedup hit; skip upload
}
Rate Limit: 100 uploads/user/hour
```

```
PUT <upload_url>   (direct to Media Gateway / S3)
Body: <raw encrypted bytes>
Response 200: { "media_url": "https://media.whatsapp.net/objects/<media_id>" }
```

#### Groups

```
POST /v1/groups
Authorization: Bearer <jwt>
Request: { "name": "Family", "member_ids": ["uuid1", "uuid2"] }
Response 201: { "group_id": "uuid", "invite_link": "https://chat.whatsapp.com/abc123" }
Rate Limit: 20 groups created/user/day

GET /v1/groups/{group_id}
Authorization: Bearer <jwt>
Response 200: { group_id, name, avatar_url, member_count, members: [{user_id, role}...], settings }
Rate Limit: 200 requests/user/min

POST /v1/groups/{group_id}/members
Authorization: Bearer <jwt>  (must be admin)
Request: { "user_ids": ["uuid3"] }
Response 200: { "added": ["uuid3"], "failed": [] }

DELETE /v1/groups/{group_id}/members/{user_id}
Authorization: Bearer <jwt>  (admin or self-removal)
Response 204
```

#### Message History (for pagination / new device sync)

```
GET /v1/conversations/{conversation_id}/messages
Authorization: Bearer <jwt>
Query Params:
  before_message_id: <timeuuid>   // pagination cursor
  limit: int (max 50, default 20)
Response 200:
{
  "messages": [
    {
      "message_id": "<timeuuid>",
      "sender_id": "uuid",
      "message_type": "text",
      "ciphertext": "<base64>",   // still encrypted; client decrypts
      "media_url": null,
      "server_ts": "2026-04-09T10:00:00Z",
      "receipts": { "delivered_count": 3, "read_count": 2 }  // group only
    }
  ],
  "has_more": true,
  "next_cursor": "<timeuuid>"
}
Rate Limit: 100 requests/user/min
Notes: Cursor-based pagination, not offset. Offset pagination requires full table scan in Cassandra.
```

### WebSocket Protocol

```
WSS /v1/ws
Authorization: Bearer <jwt> (in query param or Upgrade header)

// Client → Server frames (protobuf, framed with 4-byte length prefix)
MessageEnvelope {
  envelope_id:    bytes  // 16-byte random nonce, for dedup
  recipient_id:   bytes  // user UUID
  device_id:      uint32
  message_type:   enum   // CIPHERTEXT | PREKEY_BUNDLE | RECEIPT | TYPING | HEARTBEAT
  content:        bytes  // Signal Protocol ciphertext
  server_ts:      uint64 // set by server on receive
}

// Server → Client frames
ServerMessage {
  type: enum  // MESSAGE_ENVELOPE | ACK | RECEIPT | PRESENCE_UPDATE | OFFLINE_FLUSH
  payload: bytes
}

// Heartbeat: client sends HEARTBEAT every 30s; server renews presence TTL
// ACK: server sends ACK with envelope_id after accepting message for routing
// RECEIPT: server forwards delivery/read receipts from other devices
```

---

## 6. Deep Dive: Core Components

### 6.1 End-to-End Encryption (Signal Protocol)

**Problem it solves:**
Ensure that messages cannot be read by the server, by network attackers, or by anyone other than the intended recipient(s). This includes forward secrecy (compromise of long-term keys doesn't expose past messages) and break-in recovery (future messages are secure even after a session key compromise).

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **TLS only** | Rely on TLS for encryption; server sees plaintext | Simple; server can do spam filtering, backup, search | Server is a single point of trust; any server breach exposes all messages |
| **Symmetric encryption with shared secret** | Pre-shared key between two users, stored on server | Low overhead | Key management nightmare; server stores keys; no forward secrecy |
| **PGP / RSA message encryption** | Each user has RSA key pair; messages encrypted with recipient's public key | Decentralized key management | No forward secrecy; long-term key compromise exposes all history; key rotation complex |
| **Signal Protocol (X3DH + Double Ratchet)** | Async key exchange (X3DH) for session establishment; Double Ratchet for per-message key derivation | Perfect forward secrecy; break-in recovery; async (no simultaneous online required); widely audited | Complex implementation; key pre-distribution required; no server-side searchability |
| **MLS (Messaging Layer Security)** | IETF standard for group E2EE; tree-based key structure | O(log N) key operations for groups vs O(N) in Signal for groups | Less battle-tested; complex group state management; not yet widely deployed at WhatsApp's scale |

**Selected Approach: Signal Protocol (X3DH + Double Ratchet)**

**Detailed Reasoning:**

*X3DH (Extended Triple Diffie-Hellman) for session initiation:*
- Alice fetches Bob's key bundle: identity key (IK_B), signed pre-key (SPK_B), one-time pre-key (OPK_B).
- Alice generates an ephemeral key (EK_A).
- Four DH computations: DH(IK_A, SPK_B), DH(EK_A, IK_B), DH(EK_A, SPK_B), DH(EK_A, OPK_B).
- Master secret = KDF(DH1 || DH2 || DH3 || DH4) → 32-byte root key + chain keys.
- Bob can compute the same master secret when he comes online — no interactive handshake required. This is critical for WhatsApp's async nature (Bob may be offline for hours).

*Double Ratchet for per-message keys:*
- Symmetric-key ratchet: each message advances a chain key via HMAC-SHA256, deriving a unique message key. Chain key is immediately deleted; only message key used for encryption.
- Diffie-Hellman ratchet: each time a message is received, a new DH ratchet step derives a new root key, destroying the previous one. Provides break-in recovery: compromise of key material at time T does not expose messages at T+N.
- Implementation: AES-256-CBC for message encryption, HMAC-SHA256 for authentication, Curve25519 for DH operations.

*Group messaging E2EE:*
- WhatsApp uses the Sender Keys protocol (part of Signal): each group member generates a sender key chain. A member distributes their sender key to all other members via pairwise-encrypted X3DH sessions. Each message is encrypted once with the sender key (O(1) encryption per message), then the sender key chain advances.
- Server receives N identical ciphertexts? No — each recipient receives the same sender key ciphertext for the content, but the sender key distribution is done per-pair. The Message Router simply fans out the same ciphertext blob to all group members, which is O(1) on the sender side. Each recipient has the sender's sender key and can decrypt independently.

*Pre-key exhaustion:*
- Each OPK is single-use. If Bob's OPKs are exhausted, Alice falls back to using only SPK. This reduces forward secrecy slightly (SPK is reused across multiple session initiations until Bob replenishes) but doesn't break encryption. Server notifies device to upload more OPKs when count drops below 10.

*Numbers:*
- X3DH handshake: 4 Curve25519 DH operations ≈ 0.3 ms on modern hardware. Negligible.
- Double Ratchet: 1 HMAC-SHA256 + 1 AES-256 operation per message ≈ 10 μs. Negligible.
- Key bundle size: ~200 bytes per pre-key bundle. At 2 B users × 100 OPKs each = 200 GB for all OPKs — easily fits in distributed cache with PostgreSQL backend.

**Interviewer Q&As:**

1. **Q: How does WhatsApp handle E2EE for group messages — does the server encrypt each copy separately?**
   A: No. WhatsApp uses Signal's Sender Keys protocol. The sender encrypts the group message once using their Sender Key chain (AES-256). The server fans out this single ciphertext to all group members. Each member independently holds the sender's Sender Key (received via pairwise-encrypted sessions when they joined or first received a message) and decrypts independently. This makes group message send O(1) in computation for the sender and O(N) only in distribution of the sender key itself, which happens once per member per sender, not per message.

2. **Q: What happens if a user loses their phone (all key material)? Can they recover messages?**
   A: By design, E2EE with forward secrecy means past messages cannot be recovered from the server — the server never held the plaintext or the keys. WhatsApp offers an optional encrypted backup to iCloud/Google Drive. The backup key is derived from a user-provided 64-digit key or stored in WhatsApp's HSM-backed cloud key storage (the "backup key vault"). The backup is encrypted with AES-256 before upload, but this is a separate mechanism from the real-time E2EE session — it's a user choice, and the server-side vault option does mean WhatsApp can technically access the backup decryption key, which is a policy/trust tradeoff.

3. **Q: How does E2EE work when a user has multiple devices (phone + web)?**
   A: Each device registers independently and has its own identity key, generating its own pre-key bundle. When Alice sends to Bob, she sends separate encrypted envelopes for each of Bob's registered devices (phone, web, desktop). The sender's app fetches all of Bob's device IDs and their respective key bundles. This means N devices = N separate encrypted copies sent to the server, each addressed to a specific device_id. The server routes each envelope to the correct device.

4. **Q: What is the threat model WhatsApp E2EE protects against, and what does it NOT protect against?**
   A: Protected: passive network eavesdroppers (ISPs, governments doing bulk surveillance), compromised CDN/transit, server database breaches (server stores only ciphertext). Not protected: compromised endpoint device (malware on sender or recipient's phone), metadata (WhatsApp knows who talks to whom, when, and how often — only content is hidden), Apple/Google notification content in push payloads (WhatsApp sends only a "you have a message" signal, not content, via APNs/FCM), and WhatsApp itself as a trusted party for the key distribution infrastructure (users must trust that the server provides legitimate public keys and doesn't substitute attacker keys — this is mitigated by the "security number" / safety code verification feature).

5. **Q: How would you scale the Key Service to handle billions of key bundle fetches per day?**
   A: Key bundle fetches happen when establishing a new session — O(1) per new conversation, not per message. At 2 B users, new conversation initiations might be ~50 M/day (a rough assumption of 2.5% of users start a new conversation with someone daily). That's ~578/sec average. The Key Service can be a horizontally scaled PostgreSQL read replica fleet with an in-process LRU cache (identity key and SPK are stable for weeks/months; only OPKs change). One-time pre-key consumption is the only write-heavy operation — this requires a SELECT + DELETE (or UPDATE consumed=true) with row-level locking. This can be handled by a small PostgreSQL primary with connection pooling (PgBouncer) and read replicas for non-OPK key data. OPK consumption can be batched and processed via a single-writer pattern per user's key row.

---

### 6.2 Message Delivery Receipts and Offline Queuing

**Problem it solves:**
Users expect real-time feedback on message delivery status (sent, delivered, read). Messages sent to offline users must be reliably buffered and delivered when the user reconnects, with correct ordering and exactly-once delivery to the application layer.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Poll-based delivery** | Client polls server every N seconds for new messages | Simple; stateless server | Latency = N seconds worst case; massive poll traffic at scale |
| **Server-Sent Events (SSE)** | Server pushes to client over HTTP/1.1 long-poll | Simpler than WebSocket; works through HTTP proxies | Unidirectional; client can't push receipts without separate connection |
| **WebSocket with in-memory delivery + DB persistence** | Gateway holds open WSS connection; persists to DB, delivers over socket | Low latency; bidirectional; delivery confirmation in-protocol | Connection state management at scale; reconnect logic needed |
| **Message queue per user (SQS/Kafka)** | Dedicated queue per user; consumer = device | Durable; at-least-once delivery; replay on reconnect | SQS cost at 2 B users; Kafka partition-per-user doesn't scale; polling adds latency |
| **Redis Sorted Set as queue + WebSocket push** | Redis queue for offline buffering; WebSocket for online delivery; client ACKs to drain queue | Low latency online path; durable offline path; TTL-based expiry | Redis memory cost; need careful ACK handling to avoid re-delivery |

**Selected Approach: WebSocket online delivery + Redis Sorted Set offline queue**

**Detailed Implementation:**

*Online delivery path (p99 < 500 ms):*
1. Sender's gateway receives MessageEnvelope. Writes to Cassandra (async, fire-and-forget with LOCAL_QUORUM write consistency — 2 of 3 replicas in local DC must ack before proceeding).
2. Looks up recipient's gateway server in Redis connection registry (`conn:{user_id}:{device_id}` → `gw-id`). Redis GET: ~0.1 ms.
3. Sends envelope to recipient gateway via internal gRPC call. gRPC connection is persistent (HTTP/2 multiplexed) between all gateway servers. Latency: <1 ms intra-DC.
4. Recipient gateway pushes to client over WebSocket.
5. Recipient gateway sends a DELIVERED event back to sender's gateway when push succeeds (TCP ack from recipient's WebSocket stack confirms delivery to the OS buffer — not truly application-level delivered, but sufficient for "delivered" tick semantics at WhatsApp's definition).
6. Sender's gateway forwards DELIVERED receipt to sender app. Cassandra receipt row updated to 'delivered'.

*Offline queuing path:*
1. Connection registry lookup misses (no entry for user). User is offline.
2. Message Router serializes MessageEnvelope as protobuf bytes.
3. Atomic ZADD to Redis: `ZADD offline_queue:{user_id} {unix_ts} {base64_envelope}`. Score is timestamp, enabling ordering and retrieval of messages in chronological order.
4. Key TTL set to 30 days: `EXPIREAT offline_queue:{user_id} {now + 30*86400}`.
5. Triggers Notification Service to send silent push (APNs content-available:1 / FCM data message). Push payload contains only `{ "user_id": "...", "unread_count": N }` — no message content.

*Reconnect and queue drain:*
1. User reconnects. WebSocket Upgrade completes. Gateway registers in connection registry (TTL 90s, renewed every 30s via heartbeat).
2. Gateway immediately issues: `ZRANGEBYSCORE offline_queue:{user_id} -inf +inf WITHSCORES`. Fetches all queued messages in timestamp order.
3. Batches up to 50 envelopes per WebSocket frame. Sends to client.
4. Client sends ACK frame per envelope_id. Gateway atomically removes from sorted set: `ZREM offline_queue:{user_id} {base64_envelope}`.
5. After all messages ACKed, gateway sets Cassandra receipt status to 'delivered' for each.
6. If client disconnects mid-drain, messages remain in queue (ZREM not yet issued) and are re-delivered on next reconnect. Client deduplicates by envelope_id.

*Receipt tracking:*
- SENT: written by sender's gateway to Cassandra when server accepts message. Sent to sender app immediately.
- DELIVERED: written when recipient gateway pushes to recipient WebSocket (or when recipient reconnects and drains queue). Forwarded to sender.
- READ: sent by recipient app explicitly when user opens the conversation and views the message. Forwarded to sender via Message Router.
- Group receipts: aggregated — sender sees "delivered to X/N" and "read by X/N". Stored as individual rows in `message_receipts` table; aggregated on read.

*Numbers:*
- Offline queue size at peak: assume 5% of 1.5 B DAU are offline and receiving messages at any moment = 75 M users with queued messages. Average queue depth = 10 messages each = 750 M entries in Redis.
- Redis memory per entry: 16 bytes (ZSet overhead) + ~300 bytes (protobuf envelope) ≈ 316 bytes.
- Total Redis memory for offline queues: 750 M × 316 bytes ≈ 237 GB across Redis cluster. A 10-node Redis cluster with 32 GB RAM each = 320 GB capacity. Feasible.

**Interviewer Q&As:**

1. **Q: How do you guarantee message ordering when messages can arrive via different gateway servers?**
   A: Ordering is enforced at two levels. The TIMEUUID message_id encodes the generation timestamp (from the sender's gateway clock) and a random component, making it globally unique and approximately time-ordered. The client displays messages sorted by TIMEUUID. For the offline queue, messages are scored by server-receipt timestamp (not client-send timestamp), ensuring the queue drain is ordered by server arrival order. True total ordering per conversation is achieved by the Cassandra clustering key `(message_id DESC)` — clients fetch history via this ordering. Note: if two messages are sent within the same millisecond, TIMEUUID random bits resolve the tie consistently. Clock skew between gateway servers is bounded by NTP (typically <10 ms), which is below the granularity of concern for chat ordering.

2. **Q: What happens if the Redis offline queue goes down? How do you avoid message loss?**
   A: Redis persistence via AOF (Append-Only File) with `appendfsync everysec` provides durability with at most 1 second of data loss in a hard failure. Redis Cluster with replication (1 primary + 2 replicas per shard) handles single-node failures transparently. For additional durability, the Message Router also writes undelivered messages to Cassandra with status = 'queued'. On Redis failure, the system falls back to polling Cassandra for undelivered messages on reconnect. This is a slower path but ensures zero message loss. In practice, Redis is so reliable that the Cassandra fallback is rarely triggered.

3. **Q: How do you handle the thundering herd problem when a major event (Super Bowl, New Year) causes massive simultaneous reconnects?**
   A: Several mitigations: (1) Load balancer uses consistent hashing for WebSocket connections so reconnects go to the same gateway server (warm cache), (2) Reconnect backoff: client uses exponential backoff with jitter (base 1s, max 30s) after disconnect, staggering reconnect attempts across time, (3) Queue drain rate limiting: gateways limit drain to 50 messages/burst per reconnect, throttling DB and network load, (4) Redis cluster auto-scales horizontally — AWS ElastiCache with cluster mode supports online resharding, (5) Connection gateway capacity is pre-scaled to handle 450 M concurrent connections in steady state; peak events add ~20% — handled by pre-provisioned headroom in the load balancer target group.

4. **Q: What is the "delivered" tick semantics? Is it delivered to OS or to app?**
   A: WhatsApp's "delivered" (double tick) means the message reached the recipient's device and was acknowledged at the WebSocket/TCP layer — specifically, the recipient device's OS TCP stack acknowledged the packet. This is not the same as the app displaying the message (the app may be backgrounded). "Read" (blue ticks) require the user to open WhatsApp and view the specific conversation, which the app explicitly signals. This distinction is a product decision: WhatsApp chose OS-level delivery for the double tick rather than app-level acknowledgment because it gives more reliable delivery confirmation (app-level ack would require the app to be in foreground and process the message).

5. **Q: How do you scale the connection registry lookup to handle 450 M concurrent connections without making Redis a bottleneck?**
   A: The connection registry is partitioned across Redis Cluster shards by user_id hash. With 50 shards (typical for this scale), each shard handles 9 M entries. The lookup is O(1) GET: negligible CPU per operation. At 3.5 M messages/sec, each requiring 1 registry lookup = 3.5 M GET/sec across the cluster. Redis can handle ~1 M ops/sec per shard, so 3.5 M GET/sec requires ~4 shards just for registry lookups. With 50 shards handling mixed workloads, this is comfortably within capacity. Additionally, gateways can cache the registry lookup result locally for 5 seconds (short TTL to handle reconnects), reducing Redis load by 90%+ for active conversations where both parties are online.

---

### 6.3 Group Message Fan-Out at Scale

**Problem it solves:**
When a user sends a message to a group of 1,024 members, the system must deliver it to potentially 1,023 online recipients within the latency SLA, while handling the amplification effect — 1 message create = 1,023 delivery events — without cascading overload.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Synchronous fan-out in sender's gateway** | Gateway looks up all members, sends gRPC calls to their gateways inline | Simple | Blocks sender's gateway for 1,023 serial/parallel calls; head-of-line blocking; sender wait = max(all delivery latencies) |
| **Async fan-out via message queue (Kafka)** | Write one message to Kafka; fan-out consumer reads and delivers to each member | Decoupled; durable; parallelizable | Added latency of Kafka; topic-per-group doesn't scale; single topic = ordering issues |
| **Pub/Sub fan-out (Redis Pub/Sub)** | Group has a channel; gateways subscribe to channels for their connected users | Low latency; natural fit for real-time | Redis Pub/Sub not persistent; messages lost if consumer down; doesn't handle offline users |
| **Dedicated fan-out service with worker pool** | Message Router dispatches to Fan-out Service; pool of workers handles parallel delivery; integrates offline queue for offline members | Decoupled; parallel; handles offline; back-pressure manageable | Additional service hop; need to scale worker pool |
| **Hybrid: fan-out service + copy-on-write per shard** | Group members sharded into delivery buckets; each bucket handled by a worker; single ciphertext blob sent to recipients within same gateway | Collapses multiple gRPC calls into batched calls | Higher implementation complexity |

**Selected Approach: Dedicated Fan-Out Service with parallel worker pool**

**Implementation Detail:**

1. Message Router receives group message from sender's gateway.
2. Reads group member list from Group Service cache (Redis-backed, group membership cached with 5-second TTL; refreshed on membership change). Member list lookup: ~2 ms including network.
3. Dispatches to Fan-Out Service via gRPC (async, fire-and-forget from Message Router's perspective). Message Router immediately sends SENT ack back to sender.
4. Fan-Out Service has a goroutine pool (Go-based, sized to 10K goroutines per instance). For a 1,024-member group:
   - Queries connection registry for all 1,024 members in a Redis MGET pipeline: `MGET conn:{uid1}, conn:{uid2}, ...`. Redis MGET is O(N) but in a single round trip. At 1,024 keys × 30 bytes/key ≈ 30 KB request. Redis responds in ~1 ms.
   - Partitions members by gateway server. If 1,024 members spread across 10K gateways, expect ~0.1 members/gateway → ~900 unique gateways. But in practice, members cluster (same region, same ISP) → maybe 50-100 unique gateways for a typical group.
   - For each unique gateway, sends one batched gRPC call with all envelopes for that gateway's connected users. This collapses 1,024 potential gRPC calls to ~50-100 batch calls.
   - Offline members (no registry entry): enqueued to Redis offline queues in a pipeline ZADD batch. 300 offline members = 300 ZADD commands pipelined = 1 round trip.
5. Each gateway delivers batch envelopes to its connected sockets.

*Capacity math:*
- Group messages: 30% of 3.5 M msg/s (peak) = 1.05 M group messages/sec.
- Average group size: 50 (many small groups; 1,024-member groups are rare).
- Fan-out events: 1.05 M × 49 = 51.45 M delivery events/sec.
- Fan-out Service instances needed: at 500 K fan-out events/sec/instance (conservative, based on goroutine throughput with Redis + gRPC), need 103 instances. Round to 120 instances for headroom.
- Large group (1,024 members) fan-out: 1,023 delivery events. At 120 instances handling them in parallel across goroutines, time-to-complete a single large group fan-out ≈ max(Redis MGET latency, batched gRPC to gateways) ≈ 5 ms. Well within 500 ms SLA.

**Interviewer Q&As:**

1. **Q: How does the fan-out cost change when WhatsApp adds a feature for "communities" with 100,000 members?**
   A: At 100 K members, the MGET pipeline is 100 K keys ≈ 3 MB request — single Redis round trip still feasible (~5 ms). gRPC batching to ~5,000 unique gateways. The bottleneck shifts to fan-out throughput. With 100 K members and assuming 50% online = 50 K delivery events per message. At 1 M such community messages/day, peak = ~1,157 community messages/sec × 50 K events = 57.8 M events/sec from communities alone — comparable to all group traffic currently. This requires a dedicated fan-out tier for large communities with partitioned fan-out (split 100 K into 10 buckets of 10 K, handled by 10 workers in parallel). Additionally, communities likely warrant a different product constraint: rate-limit community messages to admins only, which reduces sender rate by 99%+.

2. **Q: How do you handle group membership changes (someone added/removed) racing with a fan-out in progress?**
   A: The fan-out reads a snapshot of membership from cache. If a member is removed after the snapshot but before delivery, they receive the message — this is acceptable (the message was legitimately in-group at snapshot time). If a member is added after snapshot, they don't receive it — also acceptable (they joined after the message was sent; they can fetch history). The critical consistency requirement is that the cache snapshot is not stale for more than the cache TTL (5 seconds). For security-sensitive operations (removing a member who should not see new messages), a "group state version" can be attached to fan-out requests, and membership changes increment the version. The fan-out service rejects deliveries for members whose version doesn't match (forcing a re-read), but this adds complexity and is typically not needed for chat semantics.

3. **Q: What is "write fan-out vs. read fan-out" and which does WhatsApp use?**
   A: Write fan-out (push model): on message send, immediately deliver to all recipients' connections/queues. Read fan-out (pull model): store one copy; each recipient reads on demand. WhatsApp uses write fan-out: messages are pushed to each recipient's WebSocket or offline queue at send time. Write fan-out optimizes for read latency (no per-read lookup needed; message is already at the consumer) at the cost of write amplification. For a chat app where reads happen once per message per user but writes happen once and must reach all recipients promptly, write fan-out is correct. Read fan-out would suit a social feed where one post is read by millions sporadically (like Twitter's timeline for celebrities).

---

## 7. Scaling

### Horizontal Scaling Strategy

**Connection Gateways:**
- Stateful servers; each holds N WebSocket connections. Scale horizontally by adding servers — load balancer consistent-hashes on user_id to minimize reconnect storm during scale-out.
- At 45 K connections/server (typical limit for WebSocket servers on 8-core, 32 GB RAM), need 450 M / 45 K = 10,000 gateway servers for peak load.
- Auto-scaling group (ASG) scales on connection count per server metric; triggers scale-out at 80% connection capacity.

**Message Router / Fan-Out Service:**
- Stateless; horizontally scalable. Add instances behind a L7 load balancer.
- Cassandra writes distributed across the cluster via consistent hashing on token range.

**Cassandra Cluster:**
- Partition key design limits hot partitions: `(conversation_id, bucket)` where bucket = daily shard. A group with 1,000 messages/day in one bucket = 1,000 rows per partition — manageable.
- 3 replicas per partition (RF=3) across 3 availability zones. Write consistency: LOCAL_QUORUM (2/3). Read consistency: LOCAL_ONE for message history (eventual ok); LOCAL_QUORUM for receipt status.
- Cluster size: 100 B messages/day × 1 KB = ~100 TB/day write volume. Cassandra compresses ~50% = 50 TB/day new data. At 10 TB SSD per node, need 5 nodes/day of new storage. Retain 90 days of delivered messages = 4,500 TB. At 10 TB/node = 450 Cassandra nodes. With RF=3 = 1,350 physical nodes across 3 DCs.

**Sharding Strategy:**
- Messages (Cassandra): sharded automatically by Cassandra's consistent hashing on the partition key token. No manual shard management.
- Users/Groups (PostgreSQL): shard by user_id modulo N (N=64 initial). Use Citus or application-level sharding. Each shard on a PostgreSQL primary + 2 read replicas. Group lookups require shard fan-out if group_id and user_id are on different shards — mitigated by denormalizing group member lists to a Cassandra table (read-optimized).

**Caching:**
- Group membership: Redis cluster, LRU, 5s TTL. Hot groups (large groups with active messages) stay in cache perpetually.
- User profile / key bundles: Redis cluster, 1-hour TTL. Cache hit rate > 99% for active users.
- Presence data: Redis is the source of truth (not a cache on top of a DB). Presence is inherently ephemeral.

**CDN for Media:**
- Media downloads routed through CDN (Cloudflare or Akamai) with edge caches. Cache-Control: public, max-age=86400. Media content-addressed (SHA-256 of ciphertext), so CDN caches indefinitely — content never changes at a given URL.
- CDN absorbs ~80% of media reads (popular media like stickers sent to groups hit many users). Net egress from origin to CDN edges: 348 GB/s × 20% = 70 GB/s — achievable with 30+ CDN PoPs.
- Media URLs signed with HMAC (15-minute expiry) so only authorized users can download — CDN validates signature at edge using shared secret, no origin roundtrip.

**Database Replication:**
- Cassandra: multi-DC active-active replication. DC1 (US-East), DC2 (EU-West), DC3 (AP-South). Each DC takes LOCAL_QUORUM for its region's users. Cross-DC replication async.
- PostgreSQL: streaming replication (synchronous to 1 standby for HA, async to 2 additional read replicas). Standby promoted via Patroni in < 30 seconds on primary failure.
- Redis: Redis Cluster with 1 primary + 2 replicas per shard. Sentinel or cluster-native HA.

### Interviewer Q&As on Scaling

1. **Q: How do you scale to 450 million concurrent WebSocket connections across 10,000 servers — how does the load balancer manage that many backends?**
   A: At L4, AWS NLB (or equivalent) handles millions of connections with consistent hash routing based on 5-tuple (src IP, src port, dst IP, dst port, protocol). For WebSocket persistence, we need sticky sessions: hash on src IP or a custom header (user_id in the upgrade request). 10,000 backends is large but within NLB capacity (AWS NLB supports 55,000+ targets per load balancer with target groups). Each gateway server registers with the LB via auto-scaling group. Health checks run every 10 seconds; unhealthy servers are deregistered, and client reconnects route to healthy servers. The connection registry in Redis ensures the Message Router can always find the current gateway for a user regardless of which server they land on after reconnect.

2. **Q: How do you shard the offline message queue in Redis if one user receives an enormous volume of messages (e.g., a celebrity or spam target)?**
   A: The offline queue is keyed by user_id, so one user's queue lives on one Redis shard (determined by hash slot). A user receiving 1 M messages while offline would put significant load on that shard. Mitigations: (1) Rate-limit inbound messages per recipient (no single user can receive more than 10 K queued messages/minute from the system, with excess dropped and sender notified). (2) Large queues overflow to Cassandra (a secondary store for offline messages keyed by user_id + message_id, Cassandra handles arbitrary data volumes). (3) Redis CLUSTER KEYSLOTS can use hash tags to distribute a single user's queue across multiple keys: `offline_queue:{user_id}:shard:{0-7}` with round-robin writes and full-scan reads. (4) Operational: identify pathological users and place them on isolated Redis cluster segments.

3. **Q: The Cassandra cluster has 1,350 nodes. How do you manage schema changes, compaction, and upgrades without downtime?**
   A: Cassandra's rolling upgrade model: upgrade one node at a time (drain → upgrade → restart → verify → proceed). With RF=3, the cluster tolerates 1 node down per replica group. Schema changes use `ALTER TABLE` which Cassandra propagates gossip-based; nodes that haven't applied the schema yet handle requests with the old schema (backward-compatible changes only — add nullable columns, never rename). Compaction: use STCS (SizeTieredCompactionStrategy) for write-heavy message tables; schedule major compaction during off-peak hours (2–4 AM UTC) on a rolling basis. Monitor compaction pending tasks metric; alert if > 1000 to detect SSTable buildup. For major migrations, use the dual-write pattern: write to both old and new schema for N weeks, then cut read traffic over.

4. **Q: How does CDN caching work with E2EE media — doesn't the CDN need to see the content?**
   A: No. Media is stored and served as opaque encrypted bytes. The CDN caches the ciphertext, not the plaintext. The CDN sees an object at URL `https://media.whatsapp.net/objects/{media_id}` with a `Content-Type: application/octet-stream` header. It has no idea whether it's a photo or a document. The decryption key is never sent to the CDN — it's transmitted inline with the message by the sender (as part of the Signal Protocol message payload, encrypted to the recipient). The CDN only needs to validate the request signature (HMAC on the URL) before serving. This design elegantly separates access control (HMAC validation at CDN edge) from content privacy (decryption key only in client app memory).

5. **Q: How would you design cross-region message delivery for a sender in Japan sending to a recipient in Brazil?**
   A: Both users connect to their nearest regional cluster (sender → AP-South DC; recipient → SA-East DC). The sender's gateway writes the message and routes it: the Message Router in AP-South checks the global connection registry (cross-DC Redis replication or a global routing tier) to find the recipient's gateway in SA-East. It then calls the SA-East Message Router via inter-DC gRPC (routed over dedicated backbone, ~200 ms RTT JP → BR). The SA-East router delivers to recipient's gateway. Total latency: AP-South write (~20 ms) + cross-DC gRPC (~200 ms) + SA-East delivery (~10 ms) ≈ 230 ms. This meets the 500 ms p99 for online recipients across continents. The Cassandra message store replicates asynchronously across DCs — the recipient's DC gets the message row eventually, but delivery doesn't wait for this replication.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| Single gateway server crash | Users on that server lose connection | TCP RST to clients; health check failure within 10 s | Clients reconnect (exponential backoff + jitter); redirect to healthy server; messages remain in offline queue (Redis) |
| Redis shard failure (offline queue) | Offline queue inaccessible for ~5-10% of users | Redis Sentinel / cluster failure detection < 5 s | Automatic failover to replica (promoted in < 30 s); AOF ensures < 1 s data loss; fallback: Cassandra-backed offline store |
| Cassandra node failure | 1 of 3 replicas offline for affected token range | Cassandra gossip detects within seconds | RF=3 means 2 nodes still serve reads/writes at QUORUM; failed node auto-replaced via nodetool; use hinted handoff to buffer writes to failed node |
| Message Router service crash | Messages not routed; accumulate in sender's socket buffer | gRPC health check | Stateless; load balancer stops routing to failed instance; retried by gateway on gRPC error (idempotent: same envelope_id) |
| Cross-DC network partition | AP users can't reach EU DC | BGP route monitoring; latency probes | Each DC operates independently (LOCAL_QUORUM); cross-DC replication pauses and resumes on reconnect; users served by their local DC |
| Media storage (S3) outage | Media uploads fail; media downloads fail | S3 health endpoint; CloudWatch metrics | Retry upload with backoff; serve from CDN cache for downloads (popular media already cached); media messages degrade gracefully (show placeholder) |
| Fan-out service overload | Group message delivery delayed | CPU/queue depth metrics | Fan-out service auto-scales (ASG); back-pressure: Message Router queues overflow to a durable Kafka topic as buffer |
| Notification service (APNs/FCM) failure | Push notifications not delivered | Error response from APNs/FCM API | Silent failure acceptable (user opens app manually); retry with exponential backoff (APNs allows retry up to 24h for persistent notifications) |
| Database primary failure (PostgreSQL) | Group/user writes fail | Patroni health check | Patroni promotes standby within 30 s; application uses connection pool (PgBouncer) which re-routes on primary change; writes that fail in the window return 503, client retries |
| Global DNS failure | All clients can't resolve DNS | DNS health checks (Route53 health checks) | Clients cache DNS for 300 s TTL; multiple NS records in different providers; fallback to hardcoded IP for bootstrap |

### Failover Strategy

**WebSocket gateway failover:** Client maintains exponential backoff retry logic (1s → 2s → 4s → ... max 32s, with ±30% jitter). On reconnect, the client sends its last-seen message ID; the server drains offline queue and re-delivers any messages received during disconnect.

**Idempotency:** Every MessageEnvelope has a 16-byte random `envelope_id`. Message Router checks for duplicate envelope_ids (Redis SET with 24-hour TTL: `SET dedup:{envelope_id} 1 EX 86400 NX`). If duplicate, silently drops. This ensures that gateway retries (on gRPC timeout) don't result in duplicate delivery.

**Circuit breaker:** Fan-out Service → Gateway gRPC calls protected by a circuit breaker (Hystrix/Resilience4j pattern). If error rate to a specific gateway > 50% over 10 seconds, open circuit — stop sending to that gateway, fall back to queuing those users' messages in offline queue. Probe with a single request every 30 seconds for recovery.

**Retry policy for Cassandra writes:** Write with LOCAL_QUORUM. On timeout (> 2 s), retry once immediately. On second failure, write to a local write-ahead log (append-only file on the gateway server's SSD) for async retry. Alert operations if local WAL exceeds 10 K entries.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Threshold / Alert |
|---|---|---|
| `gateway.connections.active` | Gauge | Alert > 80% of server capacity |
| `message.delivery.latency_p99` | Histogram | Alert > 500 ms |
| `message.delivery.latency_p50` | Histogram | Alert > 150 ms |
| `offline_queue.depth_p99` | Histogram | Alert > 10 K (per user, indicates stuck queue) |
| `fan_out.events_per_sec` | Counter | Alert < 50% of expected rate (fan-out service degradation) |
| `cassandra.write.latency_p99` | Histogram | Alert > 50 ms |
| `cassandra.pending_compactions` | Gauge | Alert > 500 |
| `redis.memory_used_pct` | Gauge | Alert > 85% |
| `media.upload.success_rate` | Counter/Rate | Alert < 99% |
| `key_service.opk_exhaustion_rate` | Counter | Alert > 0.1% of key fetches (users running out of OPKs) |
| `receipt.delivery_delay_p99` | Histogram | Alert > 2 s (receipts delayed indicates queue backup) |
| `websocket.error_rate` | Counter/Rate | Alert > 0.5% |
| `push_notification.delivery_rate` | Counter/Rate | Alert < 95% |
| `group_fanout.latency_p99` | Histogram | Alert > 1 s for groups < 100 members |

### Distributed Tracing

Every MessageEnvelope carries a `trace_id` (128-bit, generated at sender's gateway). All services propagate the trace_id via gRPC metadata headers (`x-trace-id`). Spans created for:
- Gateway receive + WebSocket write
- Connection registry lookup
- Message Router dispatch
- Cassandra write
- Fan-out Service execution
- Per-gateway batch delivery

Traces collected by OpenTelemetry collectors → Jaeger or AWS X-Ray. Sampling strategy: 100% for errors; 1% for success (at 3.5 M msg/s, 1% = 35 K traces/sec — still substantial; may use 0.1% for nominal and 100% for errors). Alert on p99 trace duration > 1 s.

### Logging

Structured JSON logs (key-value pairs) at all services. Log levels: INFO for normal operations, WARN for retries, ERROR for failures. Log fields include: `trace_id`, `user_id` (hashed for PII compliance), `message_id`, `service`, `action`, `duration_ms`, `status`.

Log pipeline: Fluentd → Kafka → Elasticsearch. Retention: 7 days hot (Elasticsearch), 30 days cold (S3). Alert on log error rate > 0.01% of requests using Elasticsearch alerting.

PII handling: `user_id` logged as `SHA256(user_id || daily_salt)` — linkable within a day for debugging, not across days for privacy.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen Approach | Alternative | Why Chosen | What You Give Up |
|---|---|---|---|---|
| Message encryption | Signal Protocol (E2EE) | TLS-only server-side | Zero server-side plaintext; regulatory compliance; user trust | Server-side spam/abuse filtering; search; backup complexity |
| Message storage | Cassandra | PostgreSQL | Linear horizontal scale; wide-row model for time-ordered messages; 3.5 M writes/sec without B-tree contention | No joins; limited ad-hoc queries; eventual consistency |
| Offline queue | Redis Sorted Set | Kafka per-user, SQS | Sub-ms enqueue/dequeue; TTL support; in-memory speed; sorted set ordering | Memory cost; persistence requires AOF (small lag); not suited for very large queues per user |
| Group fan-out | Write fan-out (push) | Read fan-out (pull) | Messages ready at client connection time; no read latency; matches chat UX expectations | Write amplification (1 msg = N deliveries); fan-out service cost |
| Connection management | WebSocket (persistent) | Long-polling, SSE | Bidirectional; lowest latency for receipts and typing indicators; standard | 450 M persistent connections require 10 K servers; more complex than stateless HTTP |
| Group E2EE | Sender Keys | Per-pair encryption for each member | O(1) encryption per message (vs O(N)); sender key distributed once per new member | Sender key compromise → all group messages from that sender decryptable (until ratchet); more complex key distribution |
| Media dedup | SHA-256 of ciphertext | No dedup / SHA-256 of plaintext | Server-side dedup without decryption; same sticker sent to 1000 groups stored once | Dedup keyed on ciphertext → same plaintext encrypted differently = no dedup; acceptable tradeoff |
| Presence architecture | Redis TTL-based heartbeat | DB-backed with WebSocket event | Sub-ms lookups; automatic expiry on disconnect; no explicit disconnect event needed | Redis memory; presence slightly stale (up to 60 s); not perfectly real-time |

---

## 11. Follow-up Interview Questions

1. **Q: How would you add support for message search while maintaining E2EE?**
   A: Full server-side search is incompatible with E2EE because the server can't index plaintext. Options: (1) Client-side search: maintain an encrypted local search index on device (SQLite FTS with encrypted columns). Works offline, no server involvement. Downside: doesn't span devices. (2) Private Information Retrieval (PIR): cryptographic technique where client queries an encrypted server-side index without revealing the query. Computationally expensive (~100x vs plaintext search). Not practical at WhatsApp's scale today. (3) "Trusted computing" approach: run search in a Trusted Execution Environment (Intel SGX) on the server — plaintext available only in the enclave. Requires hardware attestation and trusting Intel. WhatsApp's current approach is (1): local device search only.

2. **Q: How do you handle message deletion ("delete for everyone") when the message may have already been delivered?**
   A: The sender sends a special DELETE envelope containing the target message_id. This is routed like a normal message to all conversation participants. Each recipient's app, upon receiving the DELETE envelope, removes the message from local storage. The server marks the Cassandra row with `message_type = 'deleted'` and nullifies the ciphertext. If the recipient is offline, the DELETE message is queued in their offline queue and processed on reconnect. The race condition — recipient opens message before DELETE arrives — is unavoidable in a distributed system. WhatsApp allows delete within a 60-hour window (product policy). After 60 hours, the message has certainly been seen, so no delete-for-everyone. The server enforces this window server-side by rejecting DELETE envelopes older than 60 hours.

3. **Q: How would you implement end-to-end encrypted group voice calls for groups of up to 32 participants?**
   A: Group voice calls require a different architecture than 1:1 calls. Options: (1) Full mesh WebRTC (P2P): each participant sends their audio stream to every other participant. Sender bandwidth = (N-1) × audio bitrate. At N=32, 31 streams at 32 Kbps = ~1 Mbps upload — feasible but high. (2) SFU (Selective Forwarding Unit): a media server receives each participant's stream and forwards to others (no mixing). E2EE compatible: media stays encrypted; SFU forwards ciphertext. Bandwidth on sender: 1 upstream. Used by WhatsApp for group calls. Key distribution: use a Group Call Key (GCK) agreed upon via X3DH + Sender Keys pattern, distributed via the existing messaging channel. Each audio packet encrypted with derived key. SFU validates packet authentication but can't decrypt audio.

4. **Q: What happens to message delivery when a user switches from WhatsApp on an old phone to a new phone?**
   A: The new phone registers with a new device_id and new identity key (new Signal keypair generated). The old pre-key bundles for the old device become stale. Any messages in-flight to the old device_id are delivered to the old device (if it's still accessible) or queued. After registration, contacts who message this user will get a "security notification" in their chat — indicating the safety code changed — because WhatsApp detects the new identity key and alerts users to re-verify. This is Signal Protocol's key transparency feature: the system doesn't silently swap keys; it notifies. History on the old device is not transferred (E2EE means server doesn't have plaintext), unless the user used encrypted backup (iCloud/Google Drive).

5. **Q: How do you prevent spam and abuse if the server can't see message content?**
   A: WhatsApp's anti-spam strategy operates on metadata and behavioral signals, not content: (1) **Rate limiting**: messages per user per time window — exceeded rate triggers a captcha or temporary ban. (2) **Forwarding labels**: messages that have been forwarded 5+ times are labeled "frequently forwarded" (server tracks forward count in metadata). (3) **Phone number reputation**: new phone numbers have lower trust scores; require SMS verification to unlock sending at scale. (4) **Graph analysis**: unusual messaging patterns (one user sending to many strangers simultaneously) flagged by behavioral ML models running on metadata graph. (5) **User reports**: encrypted report mechanism where user can optionally include the last 5 messages (decrypted on device, re-encrypted for WhatsApp review) when reporting a contact. (6) **Business vs. personal accounts**: bulk-sending Business API accounts are pre-vetted and rate-limited.

6. **Q: How would you design the "last seen" and "online" status with privacy controls?**
   A: Users can choose: "Everyone", "My Contacts", "My Contacts Except...", or "Nobody" for who sees their last seen/online status. Implementation: Presence Service stores status with a privacy setting. When User A requests User B's presence, the system checks: is A in B's allowed-to-see list? If not, return null/hidden. The privacy setting is stored in PostgreSQL (user_privacy_settings table). Presence lookups require a JOIN or a secondary lookup against privacy settings — cached aggressively (privacy settings change rarely). Edge case: if B can't see A's last seen (A hides it), A also can't see B's last seen (WhatsApp's reciprocity rule). This prevents inference attacks where A can determine B is ignoring them.

7. **Q: How do you handle network partitions between your data centers while maintaining message ordering?**
   A: During a DC partition, each DC operates independently on LOCAL_QUORUM. Messages written to DC1 won't be visible in DC2 until the partition heals and async replication catches up. For a user whose conversation partner is in the other DC, messages during the partition will be buffered (offline queue) and delivered in order once cross-DC routing is restored. Cassandra's conflict resolution uses last-write-wins (LWW) by timestamp for receipt updates. For message ordering, since TIMEUUID is the clustering key and includes the server timestamp, messages written during partition in DC1 and DC2 will be interleaved correctly by timestamp when replication catches up. Monotonic ordering within a conversation is preserved because all messages for a conversation are routed through the same local DC (affinity based on user's location).

8. **Q: Describe the end-to-end flow of sending a photo to a group of 50 people.**
   A: (1) App encrypts photo with a random AES-256 key (media key). (2) Uploads ciphertext to Media Gateway (presigned URL from REST API); receives media_url. (3) Constructs a message payload containing: media_url, media_key (encrypted per-group member using Sender Keys), media_sha256, thumbnail (also encrypted). (4) Encrypts message payload with Sender Key chain → ciphertext. (5) Sends MessageEnvelope via WebSocket to Gateway. (6) Gateway → Message Router → Fan-Out Service. Fan-Out fetches 50 member connections, batches gRPC calls to ~10 gateways. (7) Each recipient's app receives the ciphertext. (8) App decrypts using Sender Key → gets media_url + media_key. (9) App downloads media from CDN using media_url. (10) App decrypts ciphertext using media_key → displays photo. Total sender-to-display: 500 ms – 2 s depending on media size and connection speed.

9. **Q: How do you ensure the Key Service can't be exploited to perform a man-in-the-middle attack by substituting public keys?**
   A: WhatsApp's first line of defense is the "safety number" (security code) feature: each conversation has a 60-digit code derived from both parties' identity keys. Users can compare this out-of-band (in person or via phone call). If keys are substituted (MitM by the server), the safety number would change and a vigilant user would notice. However, most users never check. WhatsApp could publish identity keys to a transparency log (similar to Certificate Transparency for TLS certs) — a publicly auditable, append-only log where any key change is visible. WhatsApp has not implemented this publicly, which remains a valid criticism. Academic proposals (CONIKS, SEEMless) address this but require client-side verification of log inclusion proofs. In practice, WhatsApp's scale and audit by Signal Protocol implementors provide some trust.

10. **Q: What are the memory and CPU implications of maintaining 450 million simultaneous WebSocket connections?**
    A: Per WebSocket connection on a modern server: ~2–4 KB for TCP socket buffer (kernel), ~8 KB for application-level buffers = ~12 KB per connection. For 45 K connections per server: 45,000 × 12 KB = 540 MB RAM per server — manageable on 32 GB servers. CPU: WebSocket connections are idle most of the time (heartbeat only every 30s). Epoll (Linux) handles 50 K idle connections with near-zero CPU. CPU load comes from receiving and routing messages: at 3.5 M msg/s across 10 K servers = 350 msgs/sec per server. At 0.1 ms per message (protobuf parse + Redis lookup + gRPC dispatch) = 35 ms CPU per second per server = 3.5% CPU for message routing. Heartbeat processing: 450 M / 30s = 15 M heartbeats/sec across 10 K servers = 1,500 heartbeats/sec per server = negligible.

11. **Q: How does WhatsApp handle the transition from 3G to 4G or WiFi mid-conversation?**
    A: The TCP connection breaks on network switch. The client detects the link change (Android/iOS network change callback), closes the current WebSocket, and establishes a new one. Exponential backoff prevents hammering the server during poor connectivity. The new WebSocket connection triggers an offline queue drain — any messages received during the brief disconnect are delivered. Client-side: messages sent during disconnect are held in a local retry queue (SQLite) with their envelope_ids. On reconnect, the client retransmits unsent messages. The server's dedup layer (Redis SET on envelope_id) prevents duplicate delivery for messages that were actually sent before the network dropped but the ACK was lost.

12. **Q: How do you scale presence to avoid a fan-out storm when a user with 500 contacts comes online?**
    A: Presence fan-out: when User A comes online, all users who have A in their active conversation view should be notified. This is not implemented as a direct fan-out to 500 contacts. Instead: (1) User A's presence is updated in Redis. (2) Other clients poll or subscribe to specific users' presence on-demand: when User B opens a chat with A, B's client subscribes to A's presence channel in Redis Pub/Sub. (3) When A's presence changes, only clients currently viewing a conversation with A receive the event. Not all 500 contacts are notified — only the subset currently looking at a chat with A. This drastically limits fan-out: rather than 500 events per user online event, it's O(users_currently_viewing_chat_with_A) which is typically 1-5. For "last seen" updates, clients fetch presence lazily when they open a conversation, not proactively.

13. **Q: What database would you use for the connection registry and why not just use the Message Router's in-memory state?**
    A: The connection registry must be externalized (Redis) rather than in-memory in the Message Router because: (1) The Message Router is stateless and horizontally scaled — any of N router instances must be able to find a user's gateway. In-memory state would require the router to either fan-out to all gateways (O(N) connections to find one user) or maintain a complete in-memory copy synchronized across all routers (distributed state management nightmare). (2) Gateway server failures require graceful handling — when a gateway crashes, its entries should auto-expire (TTL). Redis TTL handles this automatically; in-memory maps would require distributed failure detection. (3) Redis's O(1) GET for a key is more predictable than any distributed hash table implementation at this scale.

14. **Q: How does message ordering work for multi-device users (phone and laptop simultaneously)?**
    A: Each device has its own device_id and WebSocket connection. Outgoing messages from either device are routed through the server and delivered to all devices (including the sending device itself, minus the sending device). Each message has a TIMEUUID, and devices display messages sorted by this ID. If the phone and laptop send messages simultaneously, the server assigns TIMEUIDs on receipt, and both devices receive both messages (fan-out to all devices) and render them in server-receipt order. Clock skew between the phone's and laptop's local clocks is irrelevant — the server timestamp is authoritative for ordering. This produces the same conversation view on all devices.

15. **Q: How would you design a "disappearing messages" feature where messages auto-delete after 7 days?**
    A: Disappearing messages are implemented client-side, enforced by the app. The conversation metadata includes a `disappear_after` timer (7 days, 24 hours, 90 days). Each message's metadata includes `created_at`. The client app schedules a local deletion job for each message at `created_at + disappear_after`. Server-side: Cassandra message rows use a TTL: `INSERT INTO messages (...) USING TTL 604800` (7 days in seconds). This ensures server-side metadata also expires. The sender's app sends a control message when toggling disappearing messages on/off; all devices sync this setting. Limitation: a user can screenshot or export messages before they disappear — this is explicitly noted in WhatsApp's product messaging as a known behavior.

---

## 12. References & Further Reading

- **Signal Protocol whitepaper**: Marlinspike, M. & Perrin, T. (2016). "The Double Ratchet Algorithm." Signal Foundation. https://signal.org/docs/specifications/doubleratchet/
- **X3DH specification**: Marlinspike, M. & Perrin, T. (2016). "The X3DH Key Agreement Protocol." Signal Foundation. https://signal.org/docs/specifications/x3dh/
- **Sender Keys**: Signal Foundation. "Sealed Sender and Sender Keys." https://signal.org/blog/sealed-sender/
- **WhatsApp E2EE whitepaper**: WhatsApp. (2023). "WhatsApp Encryption Overview." https://www.whatsapp.com/security/WhatsApp-Security-Whitepaper.pdf
- **Cassandra architecture**: Apache Software Foundation. "Apache Cassandra Architecture." https://cassandra.apache.org/doc/latest/cassandra/architecture/
- **WebSocket RFC**: Fette, I. & Melnikov, A. (2011). RFC 6455 — The WebSocket Protocol. IETF. https://www.rfc-editor.org/rfc/rfc6455
- **High-volume messaging at scale**: Kulkarni, N. (2019). "Scaling WhatsApp to 2 Billion Users." QCon London. (Talk transcript widely available)
- **Redis Sorted Sets**: Redis Labs. "Sorted Sets." https://redis.io/docs/data-types/sorted-sets/
- **TIMEUUID in Cassandra**: DataStax. "Using time UUIDs." https://docs.datastax.com/en/cql-oss/3.3/cql/cql_reference/timeuuid_functions_r.html
- **Messaging Layer Security (MLS) IETF RFC**: Barnes, R. et al. (2023). RFC 9420 — The Messaging Layer Security (MLS) Protocol. IETF. https://www.rfc-editor.org/rfc/rfc9420
- **Patroni PostgreSQL HA**: Zalando. "Patroni: A Template for PostgreSQL High Availability." https://github.com/zalando/patroni
- **Certificate Transparency (analog for key transparency)**: Laurie, B. et al. (2021). RFC 9162 — Certificate Transparency Version 2.0. https://www.rfc-editor.org/rfc/rfc9162
