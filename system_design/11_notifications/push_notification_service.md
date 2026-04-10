# System Design: Push Notification Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Send push notifications** to iOS (via APNs) and Android (via FCM) devices for a given user or set of users.
2. **Device token management** — register, update, and deregister device tokens per user per device.
3. **Targeted delivery modes**:
   - Single user (unicast)
   - Segment / topic broadcast (multicast)
   - All users (broadcast / "blast")
4. **User preference management** — per-user, per-notification-category opt-in/opt-out controls; quiet hours; frequency caps.
5. **Notification grouping / collapsing** — collapse multiple in-flight notifications for the same user into one (e.g., "3 new messages" instead of 3 separate alerts).
6. **Delivery tracking** — record sent, delivered, opened events; expose aggregate and per-notification analytics.
7. **Scheduling** — send immediately or schedule for a future UTC timestamp.
8. **Rich notifications** — support title, body, image URL, action buttons, badge count, sound, custom data payload.
9. **Retry and TTL** — automatic retries with exponential backoff; honor TTL after which stale notifications are discarded.
10. **Template management** — store and render notification templates with variable substitution.

### Non-Functional Requirements

1. **Scale**: 500 million registered devices; peak throughput of 1 million notifications/second for large broadcasts.
2. **Latency**: P99 end-to-end delivery (API call to APNs/FCM acknowledgment) under 5 seconds for unicast; broadcast completion within 10 minutes for 500 M devices.
3. **Availability**: 99.99% uptime for the ingest API (≤52 minutes downtime/year).
4. **Durability**: Zero notification loss after accepted by the ingest API; at-least-once delivery semantics.
5. **Idempotency**: Duplicate API calls with the same idempotency key must not double-deliver.
6. **Compliance**: GDPR — purge all device tokens on user account deletion within 30 days; honor unsubscribe immediately.
7. **Observability**: Per-notification delivery status queryable within 60 seconds of send.
8. **Security**: Token storage encrypted at rest (AES-256); mTLS between internal services; APNs/FCM credentials stored in a secrets manager, never in application code.

### Out of Scope

- In-app notification UI rendering (handled by client SDKs).
- Email or SMS delivery (covered in separate design documents).
- Web push (browser-based Push API) — shares concepts but differs in protocol.
- A/B testing framework for notification content.
- Real-time bidirectional messaging (WebSocket / XMPP).

---

## 2. Users & Scale

### User Types

| Actor | Description |
|---|---|
| **Producer (internal service)** | Backend microservice that calls the notification API to trigger sends (e.g., order service, social feed service). |
| **End user (consumer)** | Mobile app user who receives push notifications on their device. |
| **Operations / analyst** | Internal user querying delivery analytics dashboards. |
| **Admin** | Manages APNs certificates, FCM keys, notification templates, and rate-limit policies. |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 500 M registered devices (DAU = 200 M; devices can be inactive but still registered).
- Average notification sends per DAU per day = 10 (social apps send more, banking less; 10 is a reasonable mid-range).
- Peak-to-average ratio = 5x (e.g., a major live event triggers a blast).
- Broadcast events: 2 per day affecting all 500 M devices.
- Unicast / targeted: remainder.

| Metric | Calculation | Result |
|---|---|---|
| Daily notifications (targeted) | 200 M DAU × 10 notifs/day | 2 B/day |
| Daily notifications (broadcast) | 2 blasts × 500 M devices | 1 B/day |
| Total daily notifications | 2 B + 1 B | 3 B/day |
| Average ingest RPS | 3 B / 86,400 s | ~34,700 RPS |
| Peak ingest RPS (5x) | 34,700 × 5 | ~174,000 RPS |
| APNs/FCM dispatch RPS (broadcast peak) | 500 M devices / 600 s (10-min window) | ~833,000 RPS |
| Device token write RPS (app installs/updates) | 1% of DAU update token/day = 2 M/day | ~23 RPS (spiky on OS release days) |
| Delivery event ingest RPS | 3 B events/day (sent) + 1.5 B (delivered ~50%) + 300 M (opened ~10%) = 4.8 B/day | ~55,500 RPS |

### Latency Requirements

| Operation | Target |
|---|---|
| Ingest API (POST /notifications) | P99 < 50 ms (API acceptance, not delivery) |
| Unicast end-to-end (API → APNs/FCM ack) | P99 < 5 s |
| Broadcast completion (500 M devices) | < 10 minutes |
| Device token registration | P99 < 100 ms |
| Preference update (opt-out) | Applied within 5 s (before next send attempt) |
| Delivery status query | Data available within 60 s of send |

### Storage Estimates

| Data | Size per record | Count | Total |
|---|---|---|---|
| Device tokens | 300 B (token 200B + metadata) | 500 M | 150 GB |
| Notification records | 500 B (metadata + payload summary) | 3 B/day × 30-day retention | 45 TB |
| Delivery events | 100 B (notif_id, device_id, status, ts) | 4.8 B/day × 30 days | 14.4 TB |
| User preferences | 200 B | 500 M users | 100 GB |
| Templates | ~2 KB each | 10,000 templates | 20 GB |
| **Total (30-day hot storage)** | | | **~60 TB** |

Cold archival (S3/GCS) at 30 days: compressed ~10:1 → ~6 TB/month incremental.

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Ingest API ingress | 174,000 RPS × 1 KB payload avg | ~174 MB/s peak |
| APNs/FCM dispatch egress | 833,000 RPS × 500 B avg push payload | ~417 MB/s peak |
| Delivery event ingress | 55,500 RPS × 100 B | ~5.5 MB/s |
| Analytics read | 100 analysts × 1 Mbps average | 100 Mbps |

---

## 3. High-Level Architecture

```
                        ┌─────────────────────────────────────────────────────────┐
                        │                   PRODUCER SERVICES                      │
                        │  (Order Svc, Social Feed Svc, Marketing Svc, etc.)       │
                        └────────────────────────┬────────────────────────────────┘
                                                 │ HTTPS / REST
                                                 ▼
                        ┌────────────────────────────────────────┐
                        │         API Gateway / Load Balancer     │
                        │   (Rate limiting, Auth, TLS termination)│
                        └────────────────────────┬───────────────┘
                                                 │
                         ┌───────────────────────▼────────────────────────┐
                         │            Notification Ingest Service          │
                         │  - Validates request, checks user preferences   │
                         │  - Applies frequency caps                        │
                         │  - Looks up device tokens for target user(s)    │
                         │  - Resolves template variables                  │
                         │  - Publishes to Kafka topic(s)                  │
                         └───────────┬──────────────────────┬─────────────┘
                                     │                      │
                         ┌───────────▼──────┐   ┌──────────▼──────────┐
                         │  Kafka: unicast   │   │  Kafka: broadcast   │
                         │  topic (partitioned│   │  topic (fan-out     │
                         │  by user_id)      │   │  orchestrator)      │
                         └───────────┬───────┘   └──────────┬──────────┘
                                     │                      │
              ┌──────────────────────▼──────┐  ┌───────────▼───────────────────┐
              │    Dispatcher Pool (iOS)     │  │    Fan-out Orchestrator        │
              │  - Reads from unicast topic  │  │  - Reads broadcast job         │
              │  - Batches by APNs token     │  │  - Shards user list by segment │
              │  - Calls APNs HTTP/2         │  │  - Publishes N unicast tasks   │
              │  - Handles 429 / token expiry│  │    back to unicast topic       │
              └─────────────┬────────────────┘  └───────────────────────────────┘
                            │ (mirrored for FCM)
              ┌─────────────▼────────────────┐
              │    Dispatcher Pool (Android)  │
              │  - Reads from unicast topic   │
              │  - Batches up to 500 tokens   │
              │  - Calls FCM v1 HTTP API      │
              │  - Handles token invalidation │
              └─────────────┬────────────────┘
                            │
              ┌─────────────▼────────────────────────────────────────┐
              │              Delivery Event Processor                  │
              │  - Receives APNs/FCM response codes                   │
              │  - Writes sent/delivered/failed status to Cassandra   │
              │  - Triggers token cleanup on InvalidRegistration      │
              │  - Publishes delivery webhook events                  │
              └─────────────┬────────────────────────────────────────┘
                            │
          ┌─────────────────▼──────────────────────────────────────────┐
          │                    Data Stores                               │
          │  ┌──────────────────┐  ┌─────────────────┐  ┌───────────┐  │
          │  │  Device Token DB │  │  Delivery Events │  │ User Prefs│  │
          │  │  (Cassandra)     │  │  (Cassandra)     │  │  (Redis + │  │
          │  │                  │  │                  │  │  Postgres)│  │
          │  └──────────────────┘  └─────────────────┘  └───────────┘  │
          └────────────────────────────────────────────────────────────┘
                            │
          ┌─────────────────▼─────────────────────────────────────────┐
          │              Analytics / Observability                      │
          │  (Kafka → Flink → ClickHouse → Grafana / internal dashbd) │
          └────────────────────────────────────────────────────────────┘
```

**Component roles:**

- **API Gateway**: Terminates TLS, enforces per-producer rate limits, authenticates with OAuth2 client credentials, routes to Ingest Service.
- **Notification Ingest Service**: Stateless; validates payloads, reads user preferences from Redis (< 1 ms), expands target user lists for segment sends, publishes serialized notification tasks to Kafka. Returns 202 Accepted immediately.
- **Kafka (unicast topic)**: Durably queues individual per-device dispatch tasks. Partitioned by `user_id` hash to preserve ordering per user and enable parallelism.
- **Kafka (broadcast topic)**: Receives large-blast jobs. A separate orchestrator reads these and fans out into millions of unicast tasks to avoid head-of-line blocking on the unicast topic.
- **Dispatcher Pools (iOS / Android)**: Stateless worker pools. Each worker maintains a persistent HTTP/2 connection pool to APNs (iOS) or FCM (Android). Batches requests, handles backpressure, processes response codes, marks tokens invalid.
- **Fan-out Orchestrator**: Reads a broadcast job from Kafka, pages through the device-token store in parallel (scatter-gather), and re-publishes per-device tasks. Checkpoints progress so it can resume on failure.
- **Delivery Event Processor**: Receives APNs/FCM responses asynchronously, writes delivery status records, triggers token invalidation cleanup, and can emit webhook callbacks to producers.
- **Device Token DB (Cassandra)**: Wide-column store; high write throughput for token registration; supports fast lookup by `user_id` (all devices for a user) and by token (reverse lookup for invalidation).
- **Delivery Events (Cassandra)**: Append-only time-series write pattern; high ingest throughput; TTL-based automatic expiry.
- **User Preferences (Redis + Postgres)**: Redis for sub-millisecond hot-path reads of opt-out flags and quiet hours; Postgres as the source of truth with async write-through.
- **Analytics (Flink + ClickHouse)**: Stream processing aggregates delivery funnels in near-real-time; ClickHouse serves OLAP queries for dashboards.

**Primary use-case data flow (unicast):**

1. Producer POST `/v1/notifications` with `{user_id, template_id, data}`.
2. Ingest Service validates auth, checks preference store (Redis) — if opted out, return 200 with `status: suppressed`.
3. Frequency cap check (Redis sliding-window counter per user per category).
4. Device token lookup for `user_id` from Cassandra (or Redis token cache for hot users).
5. Template rendered with `data` substitution.
6. One Kafka message published per device token, topic `notifications.unicast`, partition = `hash(user_id) % N`.
7. Dispatcher worker consumes message, calls APNs HTTP/2 or FCM v1 API.
8. Response code processed: success → write `delivered` event; `BadDeviceToken` → queue token invalidation; `TooManyRequests` → re-queue with backoff.
9. Delivery event written to Cassandra.

---

## 4. Data Model

### Entities & Schema

```sql
-- =============================================
-- Device Token Registry (Cassandra CQL)
-- =============================================
CREATE TABLE device_tokens (
    user_id        UUID,
    device_id      UUID,           -- stable per install, generated client-side
    platform       TEXT,           -- 'ios' | 'android'
    token          TEXT,           -- APNs device token or FCM registration token
    app_bundle_id  TEXT,           -- e.g. 'com.example.app'
    app_version    TEXT,
    os_version     TEXT,
    created_at     TIMESTAMP,
    updated_at     TIMESTAMP,
    last_seen_at   TIMESTAMP,
    is_active      BOOLEAN,
    PRIMARY KEY (user_id, device_id)
) WITH default_time_to_live = 7776000  -- 90 days since last update
  AND compaction = {'class': 'LeveledCompactionStrategy'};

-- Secondary index for reverse lookup (token → user) used during invalidation
CREATE TABLE device_token_reverse (
    token          TEXT,
    user_id        UUID,
    device_id      UUID,
    platform       TEXT,
    PRIMARY KEY (token)
);

-- =============================================
-- Notifications (Cassandra CQL)
-- =============================================
CREATE TABLE notifications (
    notification_id UUID,
    producer_id     TEXT,           -- identifies the calling service
    idempotency_key TEXT,
    target_type     TEXT,           -- 'user' | 'segment' | 'broadcast'
    target_id       TEXT,           -- user_id, segment_id, or 'all'
    template_id     UUID,
    payload         TEXT,           -- JSON: rendered title/body/data
    priority        TEXT,           -- 'high' | 'normal'
    ttl_seconds     INT,
    scheduled_at    TIMESTAMP,
    created_at      TIMESTAMP,
    status          TEXT,           -- 'queued' | 'dispatching' | 'completed' | 'cancelled'
    PRIMARY KEY (notification_id)
) WITH default_time_to_live = 2592000;  -- 30-day retention

-- Idempotency key deduplication table (TTL 24h)
CREATE TABLE notification_idempotency (
    idempotency_key     TEXT,
    notification_id     UUID,
    created_at          TIMESTAMP,
    PRIMARY KEY (idempotency_key)
) WITH default_time_to_live = 86400;

-- =============================================
-- Delivery Events (Cassandra CQL)
-- =============================================
-- Bucketed by (notification_id, date) to keep partition sizes bounded
CREATE TABLE delivery_events (
    notification_id UUID,
    date_bucket     DATE,           -- partition key includes date to cap partition size
    event_id        TIMEUUID,
    device_id       UUID,
    user_id         UUID,
    platform        TEXT,
    status          TEXT,           -- 'sent' | 'delivered' | 'failed' | 'suppressed' | 'opened'
    failure_reason  TEXT,           -- null or 'BadDeviceToken' | 'Unregistered' | 'TTLExpired' | etc.
    gateway_response TEXT,          -- raw APNs/FCM reason string
    created_at      TIMESTAMP,
    PRIMARY KEY ((notification_id, date_bucket), event_id)
) WITH CLUSTERING ORDER BY (event_id DESC)
  AND default_time_to_live = 2592000;

-- User-centric delivery view for "did user X receive notification Y?"
CREATE TABLE delivery_by_user (
    user_id         UUID,
    notification_id UUID,
    device_id       UUID,
    status          TEXT,
    updated_at      TIMESTAMP,
    PRIMARY KEY ((user_id), notification_id, device_id)
) WITH default_time_to_live = 2592000;

-- =============================================
-- User Preferences (PostgreSQL)
-- =============================================
CREATE TABLE user_notification_preferences (
    user_id             UUID        PRIMARY KEY,
    global_opt_out      BOOLEAN     NOT NULL DEFAULT FALSE,
    quiet_hours_start   TIME,       -- e.g. 22:00 local time
    quiet_hours_end     TIME,       -- e.g. 08:00 local time
    timezone            TEXT        NOT NULL DEFAULT 'UTC',
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE user_category_preferences (
    user_id     UUID        NOT NULL,
    category    TEXT        NOT NULL,   -- e.g. 'marketing' | 'transactional' | 'social'
    opt_in      BOOLEAN     NOT NULL DEFAULT TRUE,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (user_id, category)
);

CREATE INDEX idx_user_category_prefs_user ON user_category_preferences(user_id);

-- Frequency cap tracking (Redis, shown here for documentation)
-- Redis key: freq_cap:{user_id}:{category}:{window}
-- Type: sorted set (sliding window) or simple counter with TTL
-- Example: INCR freq_cap:uuid123:marketing:daily → expire at midnight

-- =============================================
-- Templates (PostgreSQL)
-- =============================================
CREATE TABLE notification_templates (
    template_id     UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    name            TEXT            NOT NULL UNIQUE,
    category        TEXT            NOT NULL,
    title_template  TEXT            NOT NULL,   -- Mustache/Handlebars syntax
    body_template   TEXT            NOT NULL,
    image_url       TEXT,
    action_url      TEXT,
    custom_data     JSONB,
    platform        TEXT            NOT NULL DEFAULT 'all',  -- 'ios'|'android'|'all'
    created_by      TEXT            NOT NULL,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    is_active       BOOLEAN         NOT NULL DEFAULT TRUE
);
```

### Database Choice

| Database | Strengths | Weaknesses | Fit |
|---|---|---|---|
| **Cassandra** | Tunable consistency, linear write scale, TTL native, multi-DC replication, wide-column for per-user token lists | No secondary indexes that scale, limited ad-hoc queries, operational complexity | **Selected** for device tokens, delivery events, notification records |
| **PostgreSQL** | ACID, rich indexing, JSONB, mature ecosystem, easy ad-hoc queries | Single primary write bottleneck, vertical scaling limits at TB scale | **Selected** for preferences, templates (low write volume, transactional correctness needed) |
| **DynamoDB** | Fully managed, auto-scaling, predictable latency | Vendor lock-in, expensive at high read/write capacity, limited query flexibility | Viable alternative to Cassandra if team wants managed infra |
| **Redis** | Sub-millisecond reads, native data structures (sorted sets, counters), pub/sub | Volatile without AOF/RDB persistence, RAM-constrained, not a primary store | **Selected** for preference hot cache, frequency cap counters, token hot cache |
| **MySQL / Aurora** | Familiar, good tooling | Same limitations as Postgres; MySQL lacks native JSONB | Not selected |
| **ClickHouse** | Columnar OLAP, 10-100x faster than Postgres for aggregation queries | Not suited for OLTP, eventual consistency model | **Selected** for analytics only |

**Cassandra justification for tokens and delivery events**: The primary access pattern is `SELECT * FROM device_tokens WHERE user_id = ?` — a single-partition read. Writes are extremely high-throughput (token registrations, delivery events). Cassandra's LSM-tree storage engine optimizes writes; LeveledCompactionStrategy controls read amplification. Native TTL avoids expensive delete operations. Multi-DC active-active replication ensures availability during regional failures. Consistency level `LOCAL_QUORUM` gives strong consistency within a region while tolerating node failures.

**PostgreSQL justification for preferences**: User preferences are updated infrequently (user settings changes) but must be read consistently — if a user opts out, that must take effect immediately. Postgres's serializable transactions and row-level locking prevent race conditions on preference updates. The dataset fits comfortably in memory (200 M users × 200 B = ~40 GB) for a well-tuned Postgres instance with read replicas.

---

## 5. API Design

All endpoints require `Authorization: Bearer <oauth2_access_token>` (client credentials grant for service-to-service, user JWT for preference endpoints). Rate limits enforced per `producer_id` claim.

### Send Notification

```
POST /v1/notifications
Rate limit: 10,000 RPS per producer (configurable)
Idempotency: Idempotency-Key header required for retries

Request:
{
  "target": {
    "type": "user" | "segment" | "broadcast",
    "user_id": "uuid",          // when type=user
    "segment_id": "uuid"        // when type=segment
  },
  "template_id": "uuid",        // mutually exclusive with inline content
  "content": {                  // alternative to template_id
    "title": "string",
    "body": "string",
    "image_url": "string",
    "action_url": "string",
    "badge_count": 0,
    "sound": "default" | "none" | "custom_sound_name",
    "custom_data": {}
  },
  "template_data": {            // variables for template rendering
    "user_name": "Alice"
  },
  "category": "transactional" | "marketing" | "social",
  "priority": "high" | "normal",
  "ttl_seconds": 86400,
  "scheduled_at": "2026-04-10T09:00:00Z",  // optional, omit for immediate
  "collapse_key": "string",     // notifications with same key collapse on device
  "options": {
    "respect_quiet_hours": true,
    "respect_frequency_cap": true,
    "dry_run": false
  }
}

Response 202 Accepted:
{
  "notification_id": "uuid",
  "status": "queued" | "suppressed",
  "estimated_delivery_ms": 2000,
  "suppression_reason": null | "opted_out" | "quiet_hours" | "frequency_cap"
}

Response 400 Bad Request:
{ "error": "INVALID_TARGET", "message": "user_id not found" }

Response 409 Conflict:
{ "error": "DUPLICATE_REQUEST", "notification_id": "uuid-of-original" }
```

### Get Notification Status

```
GET /v1/notifications/{notification_id}
Rate limit: 1,000 RPS per producer

Response 200:
{
  "notification_id": "uuid",
  "status": "queued" | "dispatching" | "completed" | "failed" | "cancelled",
  "target": { "type": "user", "user_id": "uuid" },
  "created_at": "2026-04-09T12:00:00Z",
  "delivery_summary": {
    "total_devices": 3,
    "sent": 3,
    "delivered": 2,
    "failed": 1,
    "opened": 1
  }
}
```

### Get Delivery Events (paginated)

```
GET /v1/notifications/{notification_id}/events?page_token=<cursor>&limit=100
Rate limit: 500 RPS per producer

Response 200:
{
  "events": [
    {
      "event_id": "timeuuid",
      "device_id": "uuid",
      "platform": "ios",
      "status": "delivered",
      "failure_reason": null,
      "created_at": "2026-04-09T12:00:05Z"
    }
  ],
  "next_page_token": "base64-encoded-cursor",
  "total_count": 847
}
```

### Register Device Token

```
POST /v1/device-tokens
Auth: User JWT (contains user_id claim)
Rate limit: 100 RPS per user (app-level; very rarely called)

Request:
{
  "device_id": "uuid",     // generated by app on first install, stored in keychain/keystore
  "platform": "ios" | "android",
  "token": "string",       // APNs hex token or FCM registration token
  "app_bundle_id": "com.example.app",
  "app_version": "4.2.1",
  "os_version": "17.4"
}

Response 200:
{ "device_id": "uuid", "registered_at": "2026-04-09T12:00:00Z" }
```

### Deregister Device Token

```
DELETE /v1/device-tokens/{device_id}
Auth: User JWT
Response 204 No Content
```

### User Preferences

```
GET /v1/users/{user_id}/notification-preferences
PUT /v1/users/{user_id}/notification-preferences
Auth: User JWT (user_id must match token sub claim)

PUT Request:
{
  "global_opt_out": false,
  "quiet_hours": { "start": "22:00", "end": "08:00", "timezone": "America/New_York" },
  "categories": {
    "marketing": false,
    "transactional": true,
    "social": true
  }
}

Response 200:
{ "updated_at": "2026-04-09T12:00:00Z" }
```

---

## 6. Deep Dive: Core Components

### 6.1 Notification Fan-out for Broadcasts

**Problem it solves:**
When a broadcast is sent to 500 M devices, naively iterating through all users in a single process would take hours and block other notifications. The fan-out mechanism must distribute work across many workers, be resumable on failure, and not starve unicast notifications.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Single-process sequential scan** | One worker reads all device tokens and calls APNs/FCM | Simple | Takes hours; SPOF; no concurrency |
| **Database fan-out** | DB stored procedure or job iterates tokens and writes tasks | No external queue needed | DB becomes bottleneck; hard to scale |
| **Kafka fat message** | One Kafka message contains all 500 M token references | Avoids Cassandra scan | Kafka messages limited to ~1 MB; impractical |
| **Orchestrated sharded fan-out (selected)** | Coordinator shards by user_id range; each shard dispatched as a Kafka partition range; workers claim shards | Parallelism; resumable; isolated from unicast traffic | More complex; requires coordinator state |
| **Pub/Sub + subscriber groups** | Broadcast to a topic; consumers each receive a shard | Scalable | Cloud vendor-specific; less control over completion tracking |

**Selected approach — Orchestrated sharded fan-out:**

A broadcast job record is written to Cassandra with a list of shards (e.g., 1,000 shards covering user_id space 0x0000–0xFFFF). A Fan-out Coordinator service reads the broadcast Kafka topic and:

1. Creates 1,000 shard tasks and publishes them to a `notifications.fanout.shards` Kafka topic.
2. A pool of Fan-out Worker pods (auto-scaled) each consume one shard task, page through `device_tokens` in Cassandra (using token-range pagination with `TOKEN(user_id)` paging), filter suppressed users (Redis bitset or bloom filter for opted-out users), and publish individual per-device dispatch tasks to the existing unicast Kafka topic. Workers checkpoint progress (last paged token) back to Cassandra.
3. On worker failure, the shard task is re-delivered (Kafka consumer group re-assignment) and the worker resumes from the last checkpoint.
4. The coordinator tracks shard completion and marks the broadcast done when all 1,000 shards complete.

**Priority separation**: Broadcast fan-out workers publish to a lower-priority partition of the unicast topic. Dispatchers process high-priority partitions first (configurable via Kafka consumer thread assignment).

**Implementation detail — opted-out user filtering at fan-out time:**

Rather than doing a per-user Cassandra read for preferences during fan-out (which would double the DB load), maintain a Redis bitset or HyperLogLog of opted-out `user_id` hashes. The fan-out worker checks this in-memory structure (loaded at shard start) to skip opted-out users without DB I/O. The bitset for 500 M users is 500M/8 = 62.5 MB — easily fits in Redis.

**Interviewer Q&As:**

Q: How do you ensure a broadcast does not delay urgent transactional notifications?
A: Separate Kafka topics and consumer groups for broadcast fan-out vs. unicast dispatch. Dispatchers have dedicated thread pools per topic, with unicast given higher thread allocation. Additionally, the APNs/FCM connection pool for unicast is separate from broadcast dispatchers so they don't share backpressure.

Q: What happens if the fan-out coordinator crashes midway through a 500 M device broadcast?
A: Each shard's progress is checkpointed as a Cassandra row (shard_id, last_paged_token, status). On coordinator restart, it reads all incomplete shards and re-publishes their tasks. Kafka consumer group offsets ensure shard tasks that were already processed are not re-consumed. The idempotency key on each per-device dispatch task (broadcast_id + device_id) prevents double delivery even if a shard re-processes.

Q: How would you reduce fan-out latency for a 500 M broadcast from 10 minutes to 1 minute?
A: Increase shard count from 1,000 to 10,000 and scale fan-out workers to 1,000 pods. Each worker pages through ~50,000 devices. I/O is the bottleneck; parallelize Cassandra reads per worker using async reads (DataStax Java driver async API). Also pre-shard at write time — store devices in a sharded table where each partition already contains ~50,000 devices. This trades write complexity for read speed.

Q: How do you handle the case where a user's device token changes between broadcast initiation and delivery?
A: The fan-out reads tokens at shard execution time (not at broadcast creation time), so a token updated seconds before execution is picked up. If APNs/FCM returns `BadDeviceToken` for a stale token, the dispatcher marks the old token inactive and logs the event. A separate token refresh job looks up the new token from the reverse-lookup table (if the device re-registered with a new token) and retries delivery.

Q: How do you support scheduled broadcasts (e.g., "send to all users at 9 AM their local time")?
A: Implement timezone-aware scheduling by bucketing users into timezone groups. For each timezone offset, calculate the UTC time at which 9 AM local occurs and schedule a shard job for that UTC time. This turns one broadcast into ~40 timezone-aware sub-broadcasts, spread over 24 hours. A scheduler service (using a distributed cron backed by Postgres advisory locks) triggers each sub-broadcast at the correct UTC time.

---

### 6.2 APNs / FCM Dispatcher with Token Lifecycle Management

**Problem it solves:**
Device tokens expire, rotate (on iOS when users backup/restore), and can be revoked. Sending to stale tokens wastes quota, triggers rate limiting by APNs/FCM, and pollutes delivery metrics. The dispatcher must efficiently send notifications while maintaining a clean token registry.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Naive one-request-per-token** | HTTP call per notification | Simple | 833K HTTP connections at peak; connection overhead dominates |
| **HTTP/2 multiplexing (APNs native)** | Multiple concurrent streams on one TCP connection | APNs requirement; 1,000 concurrent streams per connection | Requires HTTP/2 client; connection management |
| **FCM batch API** | Send up to 500 tokens in one HTTP request | 500x fewer HTTP calls | FCM v1 deprecated batch (topic messaging preferred now) — need per-request sends in v1 |
| **Connection pool + async dispatch (selected)** | Pool of persistent HTTP/2 connections; async futures for responses | Maximizes throughput; handles backpressure gracefully | Complex retry/timeout logic |

**Selected approach — connection pool with async dispatch:**

Each Dispatcher pod maintains:
- **APNs**: A pool of 10 persistent HTTP/2 connections to `api.push.apple.com:443`. Each connection supports 1,000 concurrent streams (per APNs specification). Maximum concurrent in-flight = 10,000 per pod. At 100 dispatcher pods, total = 1 M concurrent in-flight APNs requests — sufficient for broadcast peaks.
- **FCM**: FCM v1 API (`fcm.googleapis.com/v1/projects/{project_id}/messages:send`) is HTTP/1.1 or HTTP/2 depending on client library. Maintain a pool of 50 connections per pod; FCM service account credentials refreshed via Google Auth Library before expiry (token valid 1 hour).

**Token lifecycle management:**

APNs sends specific HTTP status + reason for each failed notification:
- `410 Gone` + `Unregistered`: Token permanently invalid. Mark `is_active = false` in Cassandra immediately, asynchronously delete.
- `400 Bad Request` + `BadDeviceToken`: Malformed token. Same — mark inactive.
- `429 Too Many Requests`: APNs rate limiting. Exponential backoff: 100 ms, 200 ms, 400 ms... up to 30 s. Re-enqueue the task in Kafka with a delay (use a delay queue pattern — publish to a retry topic with a delivery timestamp; a separate consumer re-publishes when timestamp is reached).
- `503 Service Unavailable`: APNs temporarily down. Same backoff as 429; if sustained, trigger circuit breaker.

FCM token lifecycle:
- `404 Not Found`: Token not registered. Mark inactive.
- `SENDER_ID_MISMATCH`: App re-installed with different sender. Mark inactive; notify producer that token is invalid.
- `QUOTA_EXCEEDED`: Back off per FCM documentation (minimum 1 minute).

**Token rotation (APNs-specific):** When iOS rotates a device token, APNs provides the new token via the APNs feedback service or as a `apns-id` header in response. The dispatcher stores the `apns-id` (APNs message UUID) alongside each send; on token rotation feedback, correlates old → new token and updates the registry.

**Implementation detail — delay queue for retries:**

Kafka does not natively support per-message delays. Implement a delay queue using a Redis sorted set: score = delivery_timestamp (Unix ms), member = serialized task JSON. A `RetryScheduler` process polls with `ZRANGEBYSCORE 0 <now>` every 100 ms, pops due tasks, and publishes to the unicast Kafka topic. This adds at most 100 ms scheduling jitter, acceptable for retry paths.

**Interviewer Q&As:**

Q: APNs requires a valid APNS certificate or token-based auth. Which do you use and why?
A: Token-based authentication (JWT, using the APNs Auth Key — a p8 file). Reasons: (1) Certificate-based auth expires annually and requires manual rotation, causing outages. (2) Auth key JWTs are valid for 1 hour and are generated programmatically — no certificate renewal ops. (3) One auth key works for all apps under the same Apple developer account (with different `apns-topic` header per app). The JWT is generated by each dispatcher pod using the Apple algorithm (ES256 with the p8 key), stored only in memory, and refreshed every 55 minutes.

Q: How do you handle the APNs sandbox vs. production environment split?
A: Maintain separate token registries per environment — the device token registration API accepts an `environment` field (`sandbox` | `production`). Dispatcher pools for sandbox and production are separate, pointing to `api.sandbox.push.apple.com` and `api.push.apple.com` respectively. Tokens registered in sandbox are never sent to the production APNs endpoint and vice versa (APNs would reject them with `BadDeviceToken`).

Q: What is the FCM v1 API migration impact vs. the legacy FCM API?
A: FCM legacy HTTP API was deprecated in June 2024 and fully shut down in June 2024. FCM v1 requires OAuth2 service account credentials rather than a simple server key. This means: (1) Dispatcher pods need Google Application Default Credentials or a service account JSON — managed via Kubernetes secrets. (2) No batch sends (FCM v1 removed batch) — each send is a separate HTTP call, increasing connection overhead. Mitigation: HTTP/2 persistent connections with multiplexing; alternatively use Firebase Admin SDK which wraps this. (3) FCM v1 supports per-platform message overrides (different payload for iOS vs. Android in one request — not relevant here since we have separate pipelines).

Q: If a user has 5 devices and we send a notification, how do we handle partial delivery (3 succeed, 2 fail)?
A: Each device-level send is tracked as a separate delivery event. The overall notification status transitions to `completed` when all per-device attempts have reached a terminal state (delivered, failed, suppressed). The delivery summary API reports the per-status counts. Producers can query these to trigger fallback actions (e.g., if all devices failed, fall back to email). Failed devices with retryable errors (e.g., temporary APNs outage) are retried up to 3 times within the TTL window before being marked as permanently failed.

Q: How do you handle notification collapsing (e.g., a user gets 10 rapid-fire notifications)?
A: Two levels: (1) **Server-side collapsing**: Before dispatching, check if there is an in-flight notification with the same `collapse_key` for this user. If so, update the in-flight notification payload (upsert in the delay queue / retry store) rather than sending a new one. Implemented using a Redis key `collapse:{user_id}:{collapse_key}` with TTL = dispatch window (5 seconds). (2) **Platform-level collapsing**: APNs `apns-collapse-id` header and FCM `collapse_key` field — both platforms replace an earlier undelivered notification with the same key. This handles collapsing on the device for notifications that were sent but not yet delivered.

---

### 6.3 User Preference Enforcement and Frequency Capping

**Problem it solves:**
Without preference enforcement, users receive notifications after opting out (legal and trust issue) or receive excessive notifications that cause app uninstalls. Frequency capping must be enforced consistently at high throughput without becoming a bottleneck.

**Approaches:**

| Approach | Description | Throughput | Consistency |
|---|---|---|---|
| **Synchronous DB read per notification** | Read Postgres for every ingest call | Low (limited by DB IOPS) | Strong |
| **Local cache per pod** | Each Ingest pod caches prefs with TTL | High | Eventually consistent (TTL-bounded lag) |
| **Redis distributed cache (selected)** | Prefs cached in Redis; write-through on update | Very high (< 1 ms reads) | Strong for writes; reads see at most 5 s stale |
| **Bloom filter for opt-outs** | Probabilistic structure, no false negatives | Highest | Approximate (false positives possible) |

**Selected approach — Redis with write-through and Lua atomic operations:**

Preferences are stored in Redis as a hash per user:
```
HSET user_prefs:{user_id}
  global_opt_out 0
  quiet_hours_start 2200
  quiet_hours_end 0800
  timezone America/New_York
  cat:marketing 0
  cat:transactional 1
  cat:social 1
```

On preference update (PUT /preferences), the Ingest Service updates Postgres (source of truth) and then immediately invalidates/updates the Redis hash. This ensures opt-outs take effect for subsequent sends within milliseconds.

**Frequency cap implementation:**

Redis sliding-window counter using a sorted set per user per category per window:
```
Key: freq_cap:{user_id}:{category}:{window_type}
Type: Sorted Set
Score: current_timestamp_ms
Member: notification_id (unique)
```

Atomic Lua script (executed as EVAL):
```lua
local key = KEYS[1]
local now = tonumber(ARGV[1])
local window_ms = tonumber(ARGV[2])
local cap = tonumber(ARGV[3])
local notif_id = ARGV[4]

-- Remove expired entries
redis.call('ZREMRANGEBYSCORE', key, 0, now - window_ms)

-- Count current entries
local count = redis.call('ZCARD', key)

if count >= cap then
  return 0  -- cap exceeded
end

-- Add this notification
redis.call('ZADD', key, now, notif_id)
redis.call('EXPIRE', key, math.ceil(window_ms / 1000) + 60)
return 1  -- allowed
```

This is a single Redis round-trip per notification, taking ~0.5 ms. At 174,000 ingest RPS, this requires a Redis cluster with ~10 shards.

**Quiet hours enforcement:**

At ingest time, if `respect_quiet_hours = true`, calculate the user's local time using their stored timezone. If currently in quiet hours:
- For `priority: high` (transactional): Bypass quiet hours — these are time-critical (e.g., security alerts, OTP).
- For `priority: normal` (marketing, social): Schedule delivery for the end of quiet hours (quietly enqueue with `scheduled_at` = next quiet-hours-end timestamp in user's timezone).

**Interviewer Q&As:**

Q: What is the consistency model for opt-out enforcement? Can a user receive a notification 1 second after opting out?
A: The write path: user submits PUT /preferences → Postgres is updated (strong consistency) → Redis is updated synchronously in the same request handler before responding 200. Since the Ingest Service reads from Redis, and Redis is updated before the response is returned, any notifications submitted after the opt-out response is received will be suppressed. There is a small window (< 50 ms — the Postgres + Redis write time) during which a concurrent in-flight notification could bypass the check. This is acceptable and disclosed to the user as "may take a few seconds to take effect." For GDPR compliance, we also filter at the dispatcher level using a Kafka Streams enrichment step that checks preferences at dispatch time, providing a second line of defense.

Q: How does frequency capping work for broadcast notifications that generate millions of individual notification tasks?
A: During broadcast fan-out, frequency cap checks are performed per-user by the fan-out worker before publishing the per-device task to Kafka. Each fan-out worker runs the Redis Lua script for the target user before emitting the task. This adds ~0.5 ms per user, acceptable given workers are already doing Cassandra I/O. Crucially, the frequency cap is checked at fan-out time (not dispatch time) to avoid wasting a Kafka message slot on a suppressed notification.

Q: How do you handle the Redis cluster going down? Would all notifications stop?
A: Three mitigations: (1) Redis Cluster with 3 replicas per shard — can tolerate shard-level failures. (2) Circuit breaker: if Redis latency exceeds 10 ms or error rate exceeds 1%, the Ingest Service switches to "fail-open" mode for frequency cap checks (allow through) but continues enforcing opt-outs from a local in-memory cache populated at startup. (3) The Postgres preference table serves as a fallback — at reduced throughput — for opt-out checks if Redis is completely unavailable.

Q: How do you implement "max 3 marketing notifications per week per user" without a central bottleneck?
A: Redis sorted-set sliding window with a 7-day window (604,800,000 ms). The Lua script above handles this. The sorted set key is `freq_cap:{user_id}:marketing:weekly`. Each ZREMRANGEBYSCORE call prunes entries older than 7 days before counting. Since Redis is sharded by `user_id` hash, this scales horizontally. One concern: Lua scripts are not atomic across Redis Cluster shards, but since each key is per-user, the script always executes on a single shard — atomicity guaranteed.

Q: What happens if a user registers the same preference opt-out on two devices simultaneously?
A: Both requests hit the Ingest Service concurrently. Both issue a Postgres UPDATE for the same row. Postgres row-level locking ensures one wins; the second sees the already-committed opt-out and is a no-op (both set `marketing = false`, last-write-wins is safe here since both writes are setting the same value in response to the same user intent). The Redis write is idempotent (HSET overwrites). No inconsistency results.

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling strategy | Bottleneck handled |
|---|---|---|
| **API Gateway** | Stateless; add nodes behind ALB; auto-scale on CPU/RPS | Inbound traffic spikes |
| **Ingest Service** | Stateless; K8s HPA on CPU + Kafka consumer lag; target 50% CPU | Producer traffic, preference lookup |
| **Kafka** | Add brokers; increase partition count (recommend 1,000 partitions for unicast topic at this scale); replication factor 3 | Message throughput, retention |
| **Fan-out Workers** | K8s HPA on Kafka consumer lag metric (`notifications.fanout.shards` topic); scale to 1,000 pods for 1-minute broadcast | Broadcast fan-out throughput |
| **Dispatcher Pods (iOS)** | 100 pods; each holds 10 APNs HTTP/2 connections × 1,000 streams = 10,000 in-flight; HPA on Kafka consumer lag | APNs connection limits |
| **Dispatcher Pods (Android)** | 100 pods; HPA on Kafka consumer lag | FCM throughput |
| **Cassandra** | Add nodes to ring; data distributed by consistent hash; target <50% disk utilization per node | Token/event storage throughput |
| **Redis** | Redis Cluster; 10 shards; add shards on memory/CPU threshold | Preference/cap lookup throughput |
| **Delivery Event Processor** | Stateless Kafka consumer; 50 pods; writes to Cassandra async batches | Event ingest throughput |

### DB Sharding

Cassandra handles sharding natively via consistent hashing on the partition key. `device_tokens` partitioned by `user_id` — tokens for a single user are on the same node (co-located). `delivery_events` partitioned by `(notification_id, date_bucket)` — delivery writes for a single notification are co-located, limiting partition size to one day's events.

For Postgres (preferences, templates): Use read replicas (5 replicas) for read scaling. Sharding not needed — dataset fits on a single primary with replicas. If needed in future: shard `user_notification_preferences` by `user_id % N` across N Postgres instances using Citus or application-level sharding.

### Caching

| Cache layer | Data | TTL | Eviction |
|---|---|---|---|
| Redis (L1) | User preferences | 5 minutes (write-through invalidation) | LRU + explicit invalidation |
| Redis (L1) | Hot device tokens (top 10 M DAU) | 15 minutes | LRU |
| Redis (L1) | Frequency cap counters | Dynamic (window size) | Automatic via EXPIRE |
| Local process cache (Caffeine) | Template compiled objects | 10 minutes | Size-bounded LRU, 1,000 entries |
| Local process cache | APNs auth JWT (per pod) | 55 minutes | Single entry per app |

### Replication

- **Cassandra**: Replication factor 3 per DC; 2 DCs (e.g., us-east-1, us-west-2); `NetworkTopologyStrategy` with RF=3 per DC. Read at `LOCAL_QUORUM`; write at `LOCAL_QUORUM`. Guarantees 2-of-3 nodes must agree — tolerates 1 node failure per DC.
- **Postgres**: Primary in us-east-1 with synchronous standby in same AZ (RPO=0 for failover); 5 async read replicas across both regions (RPO < 1 s). Patroni for automated failover; HAProxy for connection routing.
- **Kafka**: Replication factor 3; `min.insync.replicas = 2`; `acks = all` for ingest produces. Guarantees no message loss on single broker failure.
- **Redis**: Redis Cluster with 3 replicas per shard; `appendfsync = everysec` (AOF). On shard primary failure, replica promoted in < 10 s.

### Interviewer Q&As — Scaling

Q: Kafka has 1,000 partitions for the unicast topic. How do you add more partitions if throughput grows?
A: Kafka allows increasing partition count online (`kafka-topics.sh --alter --partitions`). However, this re-hashes assignments: existing consumers will be rebalanced, causing a brief pause (< 10 s with cooperative rebalancing in Kafka 2.4+). Messages already in old partitions drain normally. For consumers partitioning by `user_id`, after the partition increase, the hash function `user_id % new_N` reassigns some users to new partitions — this is fine since notification ordering per user is preserved within each partition, and the new mapping is deterministic. Plan partition increases during off-peak hours.

Q: How would you scale APNs dispatch beyond 1 M RPS?
A: APNs limits concurrent streams per connection and connections per IP. Mitigation: (1) Distribute dispatcher pods across multiple egress IP addresses (multiple NAT Gateway IPs or distinct VPC subnets). (2) Increase HTTP/2 stream multiplexing — Apple supports up to 1,000 concurrent streams per connection; with 10 connections × 200 pods = 2,000 connections × 1,000 streams = 2 B in-flight — effectively unlimited at our scale. (3) APNs does not publish a hard RPS cap but recommends avoiding unnecessary retries; a 1 M RPS ceiling is achievable with 200 pods each sustaining 5,000 RPS.

Q: How would you detect and handle a "hot" user with thousands of devices (e.g., a test account)?
A: The Ingest Service checks device count for a user_id before enqueueing. If `device_count > threshold` (e.g., 50), log a warning and apply a per-user device cap (send to the 50 most recently active devices). Prevents a single malformed account from flooding Kafka. Operational alert fires for human review.

Q: Describe your multi-region active-active strategy for the ingest API.
A: Deploy the full stack in two regions (e.g., us-east-1 and eu-west-1). Route users to the nearest region via Route 53 latency-based routing. Cassandra uses multi-DC replication — writes in either region are visible globally within < 500 ms (async cross-DC replication). Kafka is region-scoped; cross-region replication uses Kafka MirrorMaker 2 (async) for disaster recovery, not for active-active dispatching. Each region dispatches independently. For GDPR, EU users' data is only written to EU Cassandra nodes using datacenter-aware routing in the Cassandra driver.

Q: How do you handle back-pressure when APNs is slow?
A: Kafka consumer lag increases — the HPA detects this and scales up dispatcher pods. Additionally, dispatcher pods implement a semaphore on in-flight requests: if the semaphore is full (all HTTP/2 streams busy), the consumer pauses fetching from Kafka (back-pressure propagated upstream). This prevents memory blow-up from buffering millions of pending notifications in the pod's heap. The Kafka consumer group lag metric is visible in Grafana; SLO alerts fire at > 30 s consumer lag for high-priority partitions.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| **APNs outage** | iOS notifications not delivered | APNs 5xx / connection refused rate > 5% | Circuit breaker opens; notifications queued in Kafka (up to TTL); retry with backoff when APNs recovers |
| **FCM outage** | Android notifications not delivered | FCM 5xx rate spike | Same circuit breaker pattern; Kafka provides buffer |
| **Cassandra node failure** | Partial token lookup failures | Node health check; read error rate | `LOCAL_QUORUM` tolerates 1 node failure; automatic repair via Cassandra repair jobs |
| **Kafka broker failure** | Message loss if < 3 replicas | Broker down alert | RF=3, `min.insync.replicas=2` prevents loss; producer blocks until 2 replicas ack |
| **Ingest Service pod crash** | In-flight requests fail | Health check; ALB removes from pool | Stateless; requests retried by client with same idempotency key; K8s restarts pod |
| **Redis outage** | Preference / cap checks fail | Redis error rate > 1% | Fail-open on freq cap; local cache for opt-outs; Postgres fallback |
| **Fan-out coordinator crash mid-broadcast** | Broadcast partially sent | Pod not heartbeating | Cassandra-checkpointed shard progress; new pod resumes incomplete shards |
| **Duplicate Kafka message delivery** | Notification sent twice | — | Idempotency key check in dispatch: Redis SET NX with TTL before calling APNs/FCM |
| **Token theft / spoofing** | Unauthorized notification | — | Tokens stored encrypted; sent only over mTLS to APNs/FCM; device_id validated against JWT |
| **Clock skew affecting scheduling** | Notifications sent at wrong time | — | All timestamps in UTC; NTP enforced on all pods; scheduler uses database-stored UTC times |

### Retry Policy

| Error class | Retry strategy | Max retries | Backoff |
|---|---|---|---|
| APNs 429 TooManyRequests | Exponential + jitter | Unlimited until TTL | 100 ms × 2^n, max 30 s, ±20% jitter |
| APNs 503 | Same as 429 | Same | Same |
| FCM QUOTA_EXCEEDED | Exponential + jitter | Unlimited until TTL | 1 min × 2^n, max 1 hour |
| APNs BadDeviceToken (400) | No retry | 0 | — (mark token invalid) |
| APNs Unregistered (410) | No retry | 0 | — (mark token inactive) |
| Network timeout | Exponential | 3 | 500 ms × 2^n |
| Cassandra write failure | Retry on next replica | 3 | 50 ms, 100 ms, 200 ms |

### Idempotency

Every per-device dispatch task includes a `dispatch_id = SHA256(notification_id || device_id || attempt_number)`. Before calling APNs/FCM, the dispatcher attempts `SET NX dispatch:{dispatch_id} 1 EX 3600` in Redis. If NX fails (key exists), the send is skipped (already dispatched). This prevents duplicate sends on Kafka message re-delivery.

### Circuit Breaker

Implemented using Resilience4j (JVM dispatchers) or a custom Go middleware. State machine:
- **Closed**: Normal operation. Count failures.
- **Open**: > 50% failure rate over 60-second window OR > 10 consecutive failures. All calls fail fast; cached error returned.
- **Half-Open**: After 30-second cool-down, allow 5 probe requests. If > 3 succeed, transition to Closed; else re-open.

Separate circuit breakers per: APNs production, APNs sandbox, FCM, each Cassandra DC.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert threshold | Meaning |
|---|---|---|---|
| `notifications.ingest.rps` | Counter | — | Ingest throughput |
| `notifications.ingest.latency_p99_ms` | Histogram | > 100 ms | Ingest API slowness |
| `notifications.kafka.consumer_lag` | Gauge | > 30 s (high-priority) | Dispatcher backlog |
| `notifications.dispatch.success_rate` | Gauge | < 95% | APNs/FCM failure spike |
| `notifications.dispatch.apns_4xx_rate` | Counter | > 1% | Token quality issues |
| `notifications.dispatch.apns_5xx_rate` | Counter | > 0.5% | APNs outage signal |
| `notifications.dispatch.fcm_error_rate` | Counter | > 0.5% | FCM issue |
| `notifications.fanout.shard_completion_p99_s` | Histogram | > 30 s/shard | Fan-out performance |
| `notifications.broadcast.total_completion_s` | Histogram | > 600 s | Broadcast SLO |
| `device_tokens.invalid_rate` | Counter | > 5% of sends | Token staleness |
| `device_tokens.registered_total` | Gauge | — | Fleet size tracking |
| `preferences.redis_hit_rate` | Gauge | < 90% | Cache effectiveness |
| `preferences.optout_suppressions_rate` | Counter | — | Opt-out volume |
| `circuit_breaker.apns.state` | Enum | state=OPEN | APNs outage |
| `circuit_breaker.fcm.state` | Enum | state=OPEN | FCM outage |
| `delivery.p50_e2e_latency_ms` | Histogram | P99 > 5000 ms | E2E delivery SLO |

### Distributed Tracing

Using OpenTelemetry SDK with Jaeger/Tempo backend. Trace spans:
1. `ingest.handle_request` — covers preference lookup, template render, Kafka publish.
2. `kafka.produce` — Kafka client produce call with topic, partition, offset as attributes.
3. `fanout.shard_execute` — covers token page + preference filter + per-device publish.
4. `dispatch.apns_send` — covers APNs HTTP/2 call; attributes: `apns.response_code`, `apns.reason`, `device_id`.
5. `dispatch.fcm_send` — covers FCM HTTP call.
6. `event.write` — Cassandra write of delivery event.

Trace context propagated via Kafka message headers (W3C TraceContext format). This allows an end-to-end trace from ingest API call → Kafka → dispatcher → APNs → delivery event, visible in Jaeger.

### Logging

Structured JSON logs (via logfmt or zap). Key fields: `notification_id`, `user_id`, `device_id`, `platform`, `trace_id`, `span_id`, `status`, `error_code`.

Log levels:
- **ERROR**: APNs/FCM errors, Cassandra write failures, circuit breaker state changes.
- **WARN**: Token invalidation events, frequency cap suppressions, preference opt-out suppressions.
- **INFO**: Notification ingest accepted, broadcast start/complete.
- **DEBUG**: Per-stream APNs response, Redis cache hit/miss (disabled in production by default).

Log aggregation: Fluentd → Elasticsearch → Kibana. Retention: 14 days hot, 90 days cold (S3). PII scrubbing: `user_id` logged as opaque UUID; device tokens truncated to last 8 characters in logs.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B (selected) | Reason |
|---|---|---|---|
| Queue technology | RabbitMQ | Kafka | Kafka's log retention enables replay on dispatcher failure; higher throughput (millions/s); consumer group semantics for fan-out; offset-based checkpointing for broadcast resumption |
| Token storage | DynamoDB | Cassandra | Cassandra avoids vendor lock-in; multi-DC active-active natively; comparable performance; token-range pagination for fan-out is idiomatic in Cassandra |
| APNs auth | Certificate-based | Token-based (JWT) | Token-based never expires; no annual rotation; one key per developer account regardless of app count |
| Broadcast fan-out | Database-driven scan | Kafka-based sharded fan-out | Kafka provides durable checkpointing; fan-out is decoupled from DB load; scales independently; no single-process bottleneck |
| Frequency cap | Postgres counter | Redis sliding-window sorted set | Redis ops < 1 ms vs. ~5 ms for Postgres; sorted set ZRANGEBYSCORE gives true sliding window vs. fixed-bucket counters; Lua atomicity eliminates race conditions |
| Preference consistency | Eventual (cache-only) | Write-through Redis + Postgres source of truth | Opt-outs must take effect immediately for GDPR compliance; write-through adds < 5 ms to preference update but guarantees next ingest sees the change |
| Delivery tracking storage | MySQL | Cassandra | 4.8 B events/day; Cassandra's append-only write pattern, TTL, and horizontal scale handles this; MySQL would require aggressive partitioning and becomes operationally complex |
| Retry for transient errors | Immediate re-enqueue | Redis delay-sorted-set queue | Immediate re-enqueue creates thundering herd on APNs recovery; delay queue respects backoff windows; adds < 100 ms scheduling jitter on retries |

---

## 11. Follow-up Interview Questions

Q1: How would you add web push (browser notifications) to this system?
A: Web push uses the VAPID protocol over Web Push API (RFC 8030). Add a third dispatcher pool alongside APNs/FCM: a Web Push Dispatcher that maintains connections to push services (e.g., `fcm.googleapis.com` for Chrome, `updates.push.services.mozilla.com` for Firefox). Device token equivalent is the Web Push subscription object (endpoint URL + auth keys). Store subscription objects in the same device token table with `platform = 'web'`. The ingest and fan-out pipeline is identical. Encryption is required: the payload must be encrypted with the subscription's public key (AES-128-GCM per RFC 8291). The main complexity is that each browser vendor operates its own push service with different rate limits.

Q2: How would you implement notification analytics, specifically open-rate tracking?
A: On notification delivery, the client SDK records a unique click/open event. iOS: implement `UNUserNotificationCenterDelegate.userNotificationCenter(_:didReceive:)` callback; this fires when the user taps the notification. Android: intercept the `onMessageReceived` or notification click intent. The SDK sends a `POST /v1/events` call with `{notification_id, event_type: "opened", device_id, timestamp}`. This event flows through the Delivery Event Processor and is written to Cassandra's `delivery_events` table. For background delivery confirmation (silent push), APNs can send feedback but it's unreliable; FCM has delivery analytics in the Firebase console but the API provides this data. Open rates are computed in ClickHouse as `opened / delivered`.

Q3: How would you ensure GDPR right-to-erasure for a user who requests account deletion?
A: Implement a `UserDeletionEvent` published to a dedicated Kafka topic when a user account is deleted. A `DataPurgeWorker` consumes this event and: (1) Marks all device tokens for `user_id` as inactive in Cassandra immediately; (2) Schedules asynchronous deletion of all token rows (Cassandra `DELETE` with `timestamp` for conflict resolution); (3) Deletes `user_notification_preferences` from Postgres; (4) Deletes preference cache entries from Redis (`DEL user_prefs:{user_id}`); (5) For delivery events (contain `user_id`), either delete or anonymize (replace `user_id` with a null UUID). All steps are idempotent and logged to a compliance audit trail. Total SLA: complete within 30 days; target: within 24 hours.

Q4: How would you implement notification priority (critical alerts on iOS)?
A: iOS supports an "interruption level" concept (introduced iOS 15): `passive`, `active`, `time-sensitive`, and `critical`. Critical alerts (e.g., emergency amber alerts) can bypass Do Not Disturb and require special Apple entitlement (`com.apple.developer.usernotifications.critical-alerts`). In the APNs request, set `apns-priority: 10` (immediate delivery) and `interruption-level: critical`. In our system, add `ios_interruption_level` field to the notification payload. Critical-alert-capable notifications require the app to have the entitlement; enforce this at template-creation time (admin sets `requires_critical_entitlement: true` on the template). Route these through a separate APNs connection pool with dedicated threads to prevent any queueing delay.

Q5: How do you handle the notification thundering herd on app launch after a maintenance window?
A: After a deployment or maintenance window, all Ingest Service pods come back simultaneously. If producers queued notifications during downtime, Kafka has a large backlog. The dispatcher pools consume aggressively, potentially hitting APNs/FCM rate limits. Mitigation: (1) Dispatcher pods apply a soft-start on first launch — linear ramp from 10% to 100% throughput over 60 seconds (configurable via feature flag). (2) Kafka consumer `max.poll.records` is set conservatively (e.g., 500) to limit burst per poll cycle. (3) Notifications with expired TTL are discarded without sending — the dispatcher checks TTL before dispatching. (4) Apply token-bucket rate limiter per APNs connection (e.g., 10,000 req/s across all connections to that endpoint).

Q6: How would you support multi-tenant isolation (e.g., multiple apps sharing the infrastructure)?
A: Each tenant (app) has a `tenant_id` and separate APNs credentials + FCM project. Device tokens are scoped to `(tenant_id, user_id)`. Kafka topics are either per-tenant (strict isolation) or shared with `tenant_id` in the message key (cost-efficient). Dispatcher pods can be shared (read from multi-tenant topics, switching credentials per message) or dedicated (higher-isolation tier). Rate limits enforced per `tenant_id`. Cassandra keyspaces are either per-tenant (full isolation) or shared with `tenant_id` as part of the partition key. Cost attribution tracked by Kafka consumer lag metrics and APNs call counts per tenant.

Q7: What do you do when a notification payload exceeds APNs's 4 KB limit?
A: APNs limits notification payloads to 4,096 bytes. Mitigation at template-creation time: validate that the maximum possible rendered payload (including all variable substitutions at max length) fits within 4 KB. At dispatch time, if the rendered payload exceeds the limit: (1) Truncate the notification `body` field (never the `data` payload used for deep linking); (2) Replace the image_url with a smaller placeholder URL; (3) Emit a warning metric. If still over limit after truncation, send a "new update available" generic notification that triggers the app to fetch the full content on open (the silent push pattern).

Q8: How would you design notification grouping (e.g., "Alice and 5 others liked your photo")?
A: Implement a **notification aggregation buffer**: when a new `social.like` event arrives for user X, before publishing to Kafka, check a Redis hash `notif_agg:{user_id}:{group_key}` (group_key = e.g., `like:{photo_id}`). If an entry exists within a 30-second aggregation window: update the aggregate (increment count, append actor name), and extend the Redis TTL. If no entry exists: create a new aggregate entry with TTL = 30 seconds. A separate **aggregation flush** job (running every 5 seconds) reads expired aggregate entries and publishes the final aggregated notification to Kafka. This batches rapid-fire events into a single grouped notification. The 30-second window is configurable per notification category.

Q9: How do you test the dispatch pipeline without actually calling APNs/FCM in staging?
A: (1) **Mock APNs/FCM services**: Run mock HTTP/2 servers in staging that implement the APNs/FCM API contract, configured to return specific response codes for test tokens (e.g., `VALID_TOKEN_PREFIX` → success, `INVALID_TOKEN_PREFIX` → BadDeviceToken). (2) **Integration test suite**: End-to-end tests that submit a notification via API, assert the Kafka message was produced, assert the mock APNs server received the correct request, and assert the delivery event was written to Cassandra. (3) **Canary tokens**: In production, maintain a small set of real test devices owned by the ops team; a synthetic monitor sends a test notification every 5 minutes and verifies receipt via the device's confirmation callback. (4) **APNs sandbox**: iOS simulators and development builds use APNs sandbox — use separate sandbox dispatcher pool for pre-production validation.

Q10: What's your strategy for handling notification storms (e.g., World Cup final — 100 M users notified simultaneously)?
A: Pre-plan by pre-warming: (1) Scale dispatcher pods to 3x normal capacity 30 minutes before the event (Kubernetes `kubectl scale`). (2) Pre-publish the notification as a "scheduled broadcast" — fan-out completes pre-event, per-device dispatch tasks are in Kafka, TTL set to 5 minutes. At event time, dispatchers release the tasks (TTL unlock mechanism: tasks held in a "scheduled" partition, a trigger moves them to "active" at the target timestamp). (3) Coordinate with APNs/FCM pre-event: both provide advanced-notice pathways for large partners to pre-warm capacity. (4) Use notification collapse: if the match result takes > 5 minutes to confirm, multiple draft notifications are merged via `collapse_key` so only the final result is delivered.

Q11: How do you handle notification ordering for a user? (e.g., "match started" must arrive before "match ended")
A: Kafka partitioning by `user_id` hash guarantees that all notifications for a given user land in the same partition and are processed in order by a single consumer thread. This provides ordering within a Kafka partition. However, APNs and FCM do not guarantee delivery order on the device. For strict ordering: (1) Set increasing `apns-priority` sequence numbers in custom data; the client app buffers received notifications and displays in sequence-number order. (2) Use `collapse_key` — only the latest in a sequence is shown if earlier ones were undelivered. (3) Add `sequence_number` to notification payload; the client app holds and displays notifications in order, waiting up to 2 seconds for gaps to fill.

Q12: Describe your approach to notification A/B testing.
A: Add an `experiment_id` and `variant` field to notifications. The Ingest Service integrates with the feature flag / experimentation service: given a `user_id` and `experiment_id`, the service assigns the user to a variant (deterministically, via hash) and selects the appropriate template. Delivery and open events include `experiment_id` and `variant` fields. ClickHouse aggregates open rates, click-through rates, and conversion metrics per variant. The experimentation service computes statistical significance and surfaces results in the internal dashboard. Mutually exclusive experiments are enforced by the experimentation service to prevent users from being in conflicting experiments.

Q13: How would you implement delivery receipts if APNs doesn't confirm actual device delivery?
A: APNs v3 (HTTP/2) returns a response immediately upon acceptance — it does not confirm that the device received the notification. True delivery confirmation requires: (1) Client-side delivery callback: implement `UNUserNotificationCenterDelegate.userNotificationCenter(_:willPresent:)` which fires when notification is received while app is in foreground. For background delivery, implement `application(_:didReceiveRemoteNotification:fetchCompletionHandler:)`. Both callbacks POST to `POST /v1/events` with `{notification_id, event_type: "delivered"}`. (2) Silent push + acknowledgment: send a content-available silent push that triggers the app to call back — works when the app is in the background. Tradeoff: increases data usage; iOS may throttle silent pushes for battery optimization. (3) For FCM on Android, FCM provides delivery receipts via the FCM Admin SDK if the app sends an ack.

Q14: How do you manage APNs certificates / auth keys across multiple environments and applications?
A: Use a secrets manager (AWS Secrets Manager or HashiCorp Vault). Keys stored: `apns/auth_key/{key_id}` (p8 file contents, encrypted). Dispatcher pods retrieve the key at startup using the Vault agent sidecar — no key material in container images or environment variables. Key rotation: when Apple issues a new auth key, upload to Vault, set the new `key_id` in application config (via ConfigMap), and roll dispatcher pods with zero downtime (rolling deployment). Old key remains valid for 90 days after rotation allowing gradual rollout. Audit trail: Vault logs every key access with pod identity and timestamp.

Q15: What monitoring would you add specifically to detect notification spam / abuse by a producer?
A: (1) Per-producer notification volume anomaly detection: rolling 5-minute window; alert if a producer sends > 10x their historical baseline. (2) User complaint rate: track notification opt-out rate and "uninstall" events (if available) per producer; high opt-out rate indicates spammy content. (3) Delivery rate per producer: low delivery rate (high `BadDeviceToken` rate) suggests a producer is using stale token lists. (4) Category misuse: marketing-categorized sends to users opted out of marketing — if > 1% bypass (indicating a bug), alert. (5) Quota enforcement: hard per-producer daily limit (configurable, stored in Postgres); enforced at Ingest Service with Redis counter; producers approaching 80% of limit receive a warning response header.

---

## 12. References & Further Reading

1. Apple Developer Documentation — APNs (Apple Push Notification service): https://developer.apple.com/documentation/usernotifications/sending-notification-requests-to-apns
2. Apple Developer Documentation — Establishing a Token-Based Connection to APNs: https://developer.apple.com/documentation/usernotifications/establishing-a-token-based-connection-to-apns
3. Firebase Documentation — Send messages using the FCM v1 HTTP API: https://firebase.google.com/docs/cloud-messaging/send-message
4. Firebase Documentation — FCM architecture overview: https://firebase.google.com/docs/cloud-messaging/fcm-architecture
5. Cassandra DataStax Documentation — Data Modeling: https://docs.datastax.com/en/cassandra-oss/3.x/cassandra/dml/dmlAboutDataConsistency.html
6. Apache Kafka Documentation — Consumer Group Protocol: https://kafka.apache.org/documentation/#consumergroup
7. Resilience4j Documentation — CircuitBreaker: https://resilience4j.readme.io/docs/circuitbreaker
8. RFC 8030 — Generic Event Delivery Using HTTP Push: https://datatracker.ietf.org/doc/html/rfc8030
9. RFC 8291 — Message Encryption for Web Push: https://datatracker.ietf.org/doc/html/rfc8291
10. Meta Engineering Blog — "Building Mobile Notifications at Scale": https://engineering.fb.com/2015/08/07/android/delivering-billions-of-messages-instantly-using-carrier-pigeon/
11. Uber Engineering Blog — "uForce: Uber's Real-Time Push Notification Infrastructure": https://www.uber.com/en-US/blog/uforce-push-notification/
12. Redis Documentation — ZADD / ZRANGEBYSCORE for sliding window rate limiting: https://redis.io/commands/zadd/
13. OpenTelemetry Specification — Context Propagation: https://opentelemetry.io/docs/concepts/context-propagation/
14. Google Cloud Blog — Firebase Cloud Messaging Migration Guide (Legacy to HTTP v1): https://firebase.google.com/docs/cloud-messaging/migrate-v1
15. AWS Documentation — Amazon SNS Mobile Push Notifications: https://docs.aws.amazon.com/sns/latest/dg/sns-mobile-application-as-subscriber.html
