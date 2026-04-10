# System Design: Flash Sale / Limited Inventory System

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Flash Sale Scheduling** — An admin tool lets merchandising teams create flash sales: define a product (ASIN/SKU), total quantity available, sale price, start time, end time, and rules (e.g., 1 unit per customer).
2. **Sale Countdown and Pre-Announcement** — Display a countdown timer before the sale starts; show sale product metadata without revealing the buy button prematurely.
3. **Atomic Inventory Decrement** — When a user purchases during a flash sale, the available inventory counter must be atomically decremented. Two concurrent purchases must never both succeed for the same last unit (no oversell).
4. **Queue-Based Fairness** — Under extreme traffic (10× or more requests per available unit), a virtual waiting queue ensures users are admitted fairly (in arrival order) rather than all hitting the purchase endpoint simultaneously.
5. **Per-User Purchase Limits** — Enforce maximum 1 (or N) units per user per flash sale to prevent bulk buyers and bots from consuming all inventory.
6. **Bot Protection** — Distinguish human purchasers from automated bots via CAPTCHA, behavioral signals, and account age/reputation.
7. **Real-Time Availability Display** — Show "X units remaining" updating in near-real-time as units are claimed.
8. **Sale Status Transitions** — Manage states: `scheduled → live → sold_out / ended`. Sold-out must propagate within seconds to all users to avoid wasted requests.
9. **Post-Sale Reporting** — Number of units sold, conversion rate, geographic distribution, bot attempt rate, revenue.
10. **Waitlist / Notify When Cancelled** — If a purchase is cancelled post-sale, a unit may become available again; notify waitlisted users.

### Non-Functional Requirements

1. **Correctness (No Oversell)** — This is the hardest constraint. The system must never sell more units than the declared inventory. Undersell (selling fewer than available) is acceptable as a rare, recoverable edge case.
2. **Availability** — 99.99% uptime during the sale window. The sale window may be only 10 minutes long — any downtime directly costs revenue.
3. **Scalability** — Handle extreme traffic spikes: a sale of 10,000 units may attract 1,000,000 concurrent users (100:1 demand-to-supply ratio). The system must handle this without cascading failures.
4. **Latency** — Purchase attempt response: ≤ 200 ms P99 for "you're in queue" or "purchase confirmed." Inventory remaining display: updated within 2 seconds of each unit being claimed.
5. **Fairness** — First-come-first-served (FCFS) within the constraints of arrival time measurement. Millisecond-level ties are resolved arbitrarily but consistently.
6. **Security** — Prevent: automated bot purchases, account sharing, physical queue-jumping via ticket re-selling (queue position non-transferable), and replay attacks.
7. **Isolation** — The Flash Sale system must not affect the stability of the regular e-commerce platform. Separate infrastructure is preferred.

### Out of Scope

- Payment processing and order fulfillment
- Flash sale merchandising and pricing strategy
- Inventory procurement (assuming inventory is pre-positioned at warehouses)
- Fraud detection on payment (handled post-purchase)
- Customer service tooling for refunds

---

## 2. Users & Scale

### User Types

| User Type | Behavior | Risk |
|---|---|---|
| Legitimate Shopper | Arrives near sale start; 1 purchase attempt | Low |
| Enthusiast / Power User | Uses browser automation to refresh precisely at start; multi-tab | Medium |
| Bot / Scalper | Automated scripts; multiple accounts; residential proxy IPs | High; primary threat |
| Admin / Merchandiser | Creates and monitors sales; can cancel/extend | Trusted |
| Analytics Pipeline | Reads sale metrics and purchase events | Read-only; internal |

### Traffic Estimates

**Assumptions:**
- Flash sale for a popular product (e.g., limited-edition sneaker, new console): 10,000 units available.
- Demand ratio: 100 users per unit = 1,000,000 users interested.
- 80% of users arrive within the first 60 seconds of the sale opening.
- Each interested user makes on average 3 purchase attempts (retry on failure).
- Demand spike duration before queue throttling takes effect: first 10 seconds.
- Pre-sale page views (countdown): 500,000 users continuously polling/refreshing in the last 5 minutes.

| Metric | Calculation | Result |
|---|---|---|
| Total interested users | 10,000 units × 100 demand ratio | 1,000,000 users |
| Purchase attempts (total) | 1,000,000 × 3 retries | 3,000,000 attempts |
| Burst RPS at sale open (first 10 s, 80% traffic) | 800,000 users × 3 attempts / 10 s | 240,000 RPS (burst) |
| Sustained RPS after queue (next 5 min) | 200,000 users × 1 attempt / 300 s | ~667 RPS |
| Pre-sale page view RPS (last 5 min) | 500,000 users / 300 s × 1 req/s | ~1,667 RPS |
| Inventory decrement RPS (actual sales) | 10,000 units / 60 s (typical sell-out time) | ~167 decrement ops/s |
| Queue entry RPS (burst) | 240,000 (all turned into queue entries) | 240,000 enqueue ops/s (burst, 10 s) |
| Queue drain RPS | Controlled: ~167 admitted/s (matching decrement capacity) | 167 RPS |
| Bot traffic (estimated 40% of attempts) | 240,000 × 0.40 | ~96,000 RPS of bot traffic to filter |

### Latency Requirements

| Operation | P50 | P99 | Notes |
|---|---|---|---|
| Enqueue purchase attempt | 10 ms | 50 ms | User gets "you're in queue" response immediately |
| Queue position update (poll) | 5 ms | 20 ms | User polls every 2–5 s |
| Inventory decrement (atomic) | 1 ms | 5 ms | Redis DECR; must be sub-5 ms |
| Purchase confirmation (admitted from queue) | 50 ms | 200 ms | Includes DB write for order creation |
| Sale status update (sold out) | Real-time | <2 s propagation | CDN cache invalidation + SSE push |
| Pre-sale countdown page | 10 ms | 50 ms | Static SSR; CDN-served |

### Storage Estimates

**Assumptions:**
- Each flash sale record: 1 KB.
- Each queue entry: 200 bytes (user_id, queue_token, enqueue_time, position, status).
- Purchase record: 500 bytes.
- Sale events (for analytics): 200 bytes/event.

| Entity | Record Size | Count | Total |
|---|---|---|---|
| Flash sale records | 1 KB | 10,000 sales/year | 10 MB |
| Queue entries (peak, one sale) | 200 B | 1,000,000 per sale | 200 MB per sale (ephemeral, TTL 30 min) |
| Purchase records (all time) | 500 B | 100 M purchases/year | 50 GB/year |
| Sale event log (analytics) | 200 B | 3,000,000 events/sale × 1,000 sales/year | 600 GB/year |
| Bot detection signals | 1 KB | 240,000 RPS × 10 s burst | 2.4 GB per sale (ephemeral) |
| Inventory snapshots (time series) | 50 B | 10 RPS × 600 s sale | 300 KB per sale |

### Bandwidth Estimates

| Traffic Type | Calculation | Result |
|---|---|---|
| Enqueue requests (burst 10 s) | 240,000 RPS × 500 B (request) | 120 MB/s burst |
| Enqueue responses | 240,000 RPS × 200 B | 48 MB/s burst |
| Queue position poll (steady) | 1,000,000 users × 1 poll/5 s × 100 B resp | 20 MB/s |
| Pre-sale CDN HTML | 1,667 RPS × 5 KB (SSR page) | 8.3 MB/s (CDN handles 95%) |
| SSE stream (inventory updates) | 500,000 connections × 100 B/event × 0.5 events/s | 25 MB/s |
| Internal Kafka events | 240,000 RPS × 200 B | 48 MB/s burst |

---

## 3. High-Level Architecture

```
                  ┌──────────────────────────────────────────────────────────────┐
                  │                        CLIENTS                               │
                  │    Browser  ·  Mobile App  ·  Bots (to be filtered)         │
                  └──────────────────────────┬───────────────────────────────────┘
                                             │ HTTPS
                                             ▼
                  ┌──────────────────────────────────────────────────────────────┐
                  │          CDN (CloudFront) + WAF + AWS Shield Advanced        │
                  │  - Absorbs pre-sale page traffic (SSR countdown page cached) │
                  │  - WAF: rate limits per IP, blocks known bot IPs             │
                  │  - AWS Shield: DDoS protection                               │
                  │  - Geo-blocking for unauthorized regions                     │
                  └───────────┬──────────────────────────────┬────────────────────┘
                              │ Static / pre-sale pages      │ Dynamic (purchase)
                              ▼                              ▼
         ┌────────────────────────────┐      ┌──────────────────────────────────┐
         │   Pre-Sale SSR Service     │      │   Bot Filter / Token Gate        │
         │   (countdown, product info)│      │   - CAPTCHA validation           │
         │   Heavy CDN caching        │      │   - Account age check            │
         │   Minimal load on origin   │      │   - Behavioral anomaly score     │
         └────────────────────────────┘      │   - Issues signed "sale token"   │
                                             └──────────────┬───────────────────┘
                                                            │ Signed sale token
                                                            ▼
                                             ┌──────────────────────────────────┐
                                             │      Queue Service               │
                                             │  (Redis SORTED SET + LPUSH)      │
                                             │  - Accepts enqueue requests      │
                                             │  - Assigns queue position        │
                                             │  - Issues queue token (JWT)      │
                                             │  - Exposes position poll API     │
                                             └──────────────┬───────────────────┘
                                                            │ Admitted users
                                                            ▼
                                             ┌──────────────────────────────────┐
                                             │     Purchase Service             │
                                             │  - Validates queue token         │
                                             │  - Per-user purchase limit check │
                                             │  - Atomic inventory decrement    │
                                             │    (Redis DECR + Lua script)     │
                                             │  - Writes order to Aurora        │
                                             │  - Publishes purchase event      │
                                             └──────┬──────────────────┬─────────┘
                                                    │                  │
                                     ┌──────────────▼──┐    ┌─────────▼────────────┐
                                     │  Inventory Store │    │     Kafka            │
                                     │  Redis (atomic)  │    │  purchase-events     │
                                     │  + Aurora        │    │  inventory-events    │
                                     │  (durable)       │    │  queue-events        │
                                     └─────────────────┘    └──────────────────────┘
                                                                        │
                              ┌─────────────────────────────────────────┴───────┐
                              │                                                 │
               ┌──────────────▼──────────┐              ┌────────────────────── ▼──────┐
               │  Inventory SSE Publisher│              │  Analytics / Reporting       │
               │  - Reads Kafka events   │              │  - ClickHouse                │
               │  - Pushes inventory     │              │  - Real-time sale dashboard  │
               │    count to all clients │              │  - Bot detection ML          │
               │    via SSE / WebSocket  │              └───────────────────────────────┘
               └─────────────────────────┘

                         ┌────────────────────────────────────────────────────────┐
                         │              Admin Service                             │
                         │  - Create / schedule / cancel flash sales              │
                         │  - Monitor queue depth and conversion rates            │
                         │  - Emergency inventory top-up or sale stop             │
                         └────────────────────────────────────────────────────────┘
```

**Component Roles:**

| Component | Role |
|---|---|
| CDN + WAF + Shield | Absorbs pre-sale traffic; filters known bots; DDoS mitigation |
| Pre-Sale SSR Service | Serves countdown page and product info; 99% cache hit rate at CDN |
| Bot Filter / Token Gate | Issues signed "sale tokens" to users passing anti-bot checks; tokens required to enqueue |
| Queue Service | Manages the virtual waiting queue; assigns positions; controls admission rate |
| Purchase Service | Processes admitted users' purchases; atomic inventory decrement; writes orders |
| Inventory Store (Redis) | Single-shard Redis DECR for atomic inventory counter; sub-ms latency |
| Aurora | Durable order records; inventory held quantity; purchase history (per-user limit check) |
| Kafka | Event bus for purchase, inventory, and queue events; feeds analytics and SSE publisher |
| Inventory SSE Publisher | Pushes real-time inventory count to connected clients via Server-Sent Events |
| Analytics (ClickHouse) | Real-time sale analytics; bot detection model training data |
| Admin Service | Sale lifecycle management; real-time monitoring dashboard |

**Primary Use-Case Data Flow — User Purchases During Flash Sale:**

1. User loads pre-sale countdown page → CDN-served HTML (TTL 30 s).
2. At T-30 seconds before sale: frontend fetches a "sale token" from Bot Filter (`POST /v1/sale/{sale_id}/pre-auth`). Bot Filter validates: account age ≥ 30 days, CAPTCHA passed, no bot signals. Issues a signed JWT sale token (valid 5 min).
3. At T=0 (sale opens): user clicks "Buy Now." Frontend submits `POST /v1/queue/{sale_id}/enter` with sale token.
4. Queue Service validates sale token, appends user to Redis Sorted Set (score = epoch_microseconds), returns `{queue_token: JWT, position: 42103, estimated_wait: "4 min"}`.
5. Frontend polls `GET /v1/queue/{sale_id}/status?token={queue_token}` every 3 seconds.
6. Queue Service's "Admitter" process drains queue at controlled rate (e.g., 200 users/s). When user's position reaches front, admitter marks them as "admitted" in Redis.
7. User polls, sees "admitted" status. Frontend immediately calls `POST /v1/purchase/{sale_id}` with queue_token.
8. Purchase Service validates queue_token (not expired, not redeemed), checks per-user limit (Redis SET `purchased:{sale_id}:{user_id}`), runs atomic Lua script on Redis (`DECR inv:{sale_id}` returning new value).
9. If DECR result ≥ 0: write order to Aurora, publish to Kafka `purchase-events`. Return 200 with order confirmation.
10. If DECR result < 0 (sold out): INCR (compensate), return 410 Gone "Sold Out."
11. Kafka consumer updates Inventory SSE Publisher → pushes `{remaining: N-1}` to all SSE subscribers within 2 seconds.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- FLASH SALE DEFINITIONS (Aurora MySQL)
-- ============================================================
CREATE TABLE flash_sales (
    sale_id             BIGINT          NOT NULL AUTO_INCREMENT,
    asin                CHAR(10)        NOT NULL,
    sku                 VARCHAR(50)     NOT NULL,
    marketplace_id      SMALLINT        NOT NULL DEFAULT 1,
    title               VARCHAR(500)    NOT NULL,
    sale_price_cents    INT             NOT NULL,
    original_price_cents INT            NOT NULL,
    total_inventory     INT             NOT NULL,   -- units allocated to this sale
    sold_count          INT             NOT NULL DEFAULT 0,
    status              ENUM('scheduled', 'warming', 'live', 'sold_out', 'ended', 'cancelled')
                                        NOT NULL DEFAULT 'scheduled',
    starts_at           TIMESTAMPTZ     NOT NULL,
    ends_at             TIMESTAMPTZ     NOT NULL,
    max_per_user        TINYINT         NOT NULL DEFAULT 1,
    require_captcha     BOOLEAN         NOT NULL DEFAULT TRUE,
    sale_token_ttl_secs INT             NOT NULL DEFAULT 300,   -- 5 min
    queue_admission_rate INT            NOT NULL DEFAULT 200,   -- users admitted per second
    created_by          BIGINT          NOT NULL,               -- admin user_id
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (sale_id),
    INDEX idx_status_starts (status, starts_at),
    INDEX idx_sku (sku, marketplace_id)
);

-- ============================================================
-- INVENTORY COUNTER (Redis; source of truth for atomic decrements)
-- ============================================================
-- Key: inv:{sale_id}
-- Type: String (integer)
-- Value: current available units (starts at total_inventory)
-- No TTL (manual management; deleted when sale ends or sold_out)
--
-- Lua script for atomic purchase attempt:
-- EVAL """
--   local remaining = tonumber(redis.call('GET', KEYS[1]))
--   if remaining == nil then return -2 end  -- sale not found / not warmed
--   if remaining <= 0 then return -1 end    -- sold out
--   redis.call('DECR', KEYS[1])
--   return remaining - 1                   -- returns new remaining count
-- """ 1 inv:{sale_id}
--
-- Key: inv_warmup_done:{sale_id}  → "1" (signals that cache is warmed)

-- ============================================================
-- PURCHASE RECORDS (Aurora MySQL — durable, post-sale)
-- ============================================================
CREATE TABLE flash_purchases (
    purchase_id         BIGINT          NOT NULL AUTO_INCREMENT,
    sale_id             BIGINT          NOT NULL,
    user_id             BIGINT          NOT NULL,
    queue_token_jti     VARCHAR(100)    NOT NULL UNIQUE,  -- JWT ID, prevents double-use
    order_id            BIGINT,                           -- populated after order creation
    sku                 VARCHAR(50)     NOT NULL,
    quantity            INT             NOT NULL DEFAULT 1,
    unit_price_cents    INT             NOT NULL,
    status              ENUM('reserved', 'confirmed', 'cancelled') NOT NULL DEFAULT 'reserved',
    purchased_at        TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (purchase_id),
    UNIQUE KEY uq_user_sale (user_id, sale_id),           -- enforces max_per_user = 1
    INDEX idx_sale (sale_id, status),
    FOREIGN KEY (sale_id) REFERENCES flash_sales(sale_id)
);

-- ============================================================
-- QUEUE TOKENS (Redis — ephemeral, TTL-based)
-- ============================================================
-- Hash: queue_token:{jti}
--   user_id:      Number
--   sale_id:      Number
--   enqueue_time: Number (epoch microseconds)
--   position:     Number
--   status:       String  — "waiting" | "admitted" | "expired" | "redeemed"
--   admitted_at:  Number or nil
-- TTL: 30 minutes (sale duration + buffer)

-- Sorted Set for queue ordering:
-- Key: sale_queue:{sale_id}
-- Members: jti (queue token JWT ID)
-- Score: enqueue_time_epoch_microseconds (lower = earlier = higher priority)
-- → ZADD NX (no update on duplicate)
-- → ZRANGE sale_queue:{sale_id} 0 admission_cursor WITH SCORES (for admitter)
-- → ZRANK sale_queue:{sale_id} jti (returns 0-based position)

-- Admission cursor (tracks how far the admitter has drained):
-- Key: admission_cursor:{sale_id}  → Number (last admitted position index)
-- Updated atomically by the Admitter process.

-- ============================================================
-- PER-USER PURCHASE FLAG (Redis — ephemeral, fast enforcement)
-- ============================================================
-- Key: purchased:{sale_id}:{user_id}
-- Type: String ("1")
-- TTL: 24 hours (long enough for audit; deleted after sale)
-- SET NX: can only set once → enforces max_per_user = 1

-- ============================================================
-- BOT DETECTION / SALE TOKEN (Redis + Aurora)
-- ============================================================
-- Key: sale_token:{jti}
--   Type: String  → "used" | "unused"
-- TTL: 5 minutes (sale_token_ttl_secs)
-- Used to track that a sale token (pre-auth JWT) is not reused.

-- Aurora: bot_events table for audit and ML training
CREATE TABLE bot_events (
    event_id            BIGINT          NOT NULL AUTO_INCREMENT,
    sale_id             BIGINT          NOT NULL,
    user_id             BIGINT,
    ip_address_hash     VARCHAR(64)     NOT NULL,
    event_type          ENUM('captcha_fail', 'rate_limit', 'token_reuse',
                             'account_too_new', 'behavioral_block') NOT NULL,
    bot_score           DECIMAL(4,3),   -- 0.000 to 1.000
    occurred_at         TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (event_id),
    INDEX idx_sale_ip (sale_id, ip_address_hash),
    INDEX idx_occurred (occurred_at)
);

-- ============================================================
-- INVENTORY SNAPSHOTS — time series (ClickHouse for analytics)
-- ============================================================
-- ClickHouse table:
-- CREATE TABLE inventory_snapshots (
--   sale_id     UInt64,
--   snapshot_at DateTime64(3),
--   remaining   UInt32,
--   sold        UInt32
-- ) ENGINE = MergeTree()
-- ORDER BY (sale_id, snapshot_at);
```

### Database Choice

| Component | Options Considered | Selected | Justification |
|---|---|---|---|
| Flash Sale Definitions | DynamoDB, MySQL/Aurora, Redis | **Aurora MySQL** | Low cardinality (~10,000 sales/year); relational (sale ↔ purchases); ACID transactions for sale state transitions; full SQL for admin queries |
| Inventory Counter (atomic) | Redis DECR, DynamoDB atomic counter, Postgres row lock | **Redis single-key DECR with Lua** | Redis DECR is atomic at sub-millisecond latency; single Redis node eliminates distributed coordination; Lua script wraps DECR with "sold-out" guard atomically; DynamoDB atomic counter has >5 ms P99 — too slow for burst; Postgres FOR UPDATE creates lock hotspot |
| Queue (ordering + position) | Redis Sorted Set, Kafka, SQS, in-memory | **Redis Sorted Set** | Natural fit: score = timestamp (epoch microseconds), member = queue_token_jti; `ZRANK` gives O(log N) position lookup; `ZRANGE` gives ordered drain for admitter; ~1 M members at 200 B = 200 MB — fits in a single Redis node; Kafka/SQS lack efficient position-query semantics |
| Purchase Records (durable) | DynamoDB, Aurora, Cassandra | **Aurora MySQL** | Post-purchase records need ACID (order creation + purchase record must be atomic); low write volume (10,000 sales × 10,000 units = 100 M/year); relational joins for audit and analytics; UNIQUE constraint on `(user_id, sale_id)` is the database-level safety net against double-purchase |
| Queue Tokens | Redis, DynamoDB, JWT-only | **Redis Hash + JWT** | Redis stores queue token metadata (position, status) for O(1) lookup; JWT is the signed, tamper-proof representation returned to the client; Redis TTL auto-expires tokens; without Redis, every status poll would require DynamoDB read or JWT re-verification overhead |
| Bot Detection Signals | Redis, Aurora, ClickHouse | **Redis (real-time blocking) + Aurora (audit)** | Real-time bot blocking requires sub-ms lookups (Redis); audit trail and ML training data goes to Aurora; ClickHouse receives aggregated signals for analytics |
| Real-Time Inventory Count | Redis, DynamoDB, SSE direct | **Kafka → SSE Publisher → clients** | Redis DECR provides the authoritative count; Kafka fan-out decouples the publisher from the purchase path; SSE over WebSocket for browser compatibility without persistent bidirectional overhead |

---

## 5. API Design

```
# ─────────────────────────────────────────────────────────────
# 1. GET Flash Sale Info (pre-sale and during sale)
# ─────────────────────────────────────────────────────────────
GET /v1/sales/{sale_id}

Response 200:
{
  "sale_id": 12345,
  "asin": "B09XYZ",
  "sku": "SNEAKER-RED-10",
  "title": "Limited Edition Air Series - Red",
  "original_price_cents": 25000,
  "sale_price_cents": 17500,
  "total_inventory": 10000,
  "remaining_inventory": 3421,      // from Redis inv:{sale_id}
  "status": "live",
  "starts_at": "2026-04-09T12:00:00Z",
  "ends_at": "2026-04-09T12:10:00Z",
  "max_per_user": 1,
  "user_has_purchased": false,      // requires JWT
  "user_queue_status": null         // "waiting" | "admitted" | null
}

Auth: Optional (JWT enables user-specific fields)
Rate Limit: 1000 req/min per IP (CDN-served for status != live; bypass CDN when live)
Cache: CDN TTL 10 s pre-sale; 2 s during live sale; 0 s (bypass) for sold_out transition

# ─────────────────────────────────────────────────────────────
# 2. POST Pre-Auth (get sale token before sale opens)
# ─────────────────────────────────────────────────────────────
POST /v1/sales/{sale_id}/pre-auth

Request Body:
{
  "captcha_token": "03AGdBq...",   // reCAPTCHA v3 token
  "device_fingerprint": "fp_abc123"
}

Response 200:
{
  "sale_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "valid_until": "2026-04-09T12:05:00Z"
}

Response 403 (bot detected):
{
  "error": "BOT_DETECTED",
  "reason": "captcha_failed" | "account_too_new" | "behavioral_anomaly"
}

Auth: Required (JWT — must be logged in to get sale token)
Rate Limit: 5 pre-auth attempts per user_id per sale; 20 per IP per sale
Note: sale_token is a signed JWT containing:
  { sub: user_id, sale_id: 12345, jti: uuid, iat, exp: 5 min }

# ─────────────────────────────────────────────────────────────
# 3. POST Enter Queue
# ─────────────────────────────────────────────────────────────
POST /v1/sales/{sale_id}/queue
Headers: Idempotency-Key: <uuid>

Request Body:
{
  "sale_token": "eyJhbGci..."
}

Response 202 (queued):
{
  "queue_token": "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9...",
  "position": 42103,
  "queue_depth": 850000,
  "estimated_wait_seconds": 210,
  "poll_interval_seconds": 3
}

Response 200 (immediately admitted — queue drained, spots available):
{
  "queue_token": "...",
  "position": 1,
  "status": "admitted",
  "purchase_window_seconds": 120   // user has 2 min to complete purchase
}

Response 409 (already in queue or already purchased):
{
  "error": "ALREADY_QUEUED" | "ALREADY_PURCHASED"
}

Response 410 (sold out before entering queue):
{
  "error": "SALE_SOLD_OUT"
}

Response 423 (sale not yet open):
{
  "error": "SALE_NOT_STARTED",
  "starts_at": "2026-04-09T12:00:00Z"
}

Auth: Required (JWT)
Rate Limit: 3 queue entries per user_id per sale (idempotent with same sale_token)
Idempotency: Idempotency-Key required; same key returns same queue_token

# ─────────────────────────────────────────────────────────────
# 4. GET Queue Status (polling)
# ─────────────────────────────────────────────────────────────
GET /v1/sales/{sale_id}/queue/status
  ?queue_token=eyJhbGci...

Response 200:
{
  "status": "waiting",          // waiting | admitted | expired | sold_out
  "position": 38901,            // current position (decreasing)
  "queue_depth": 850000,
  "estimated_wait_seconds": 194,
  "remaining_inventory": 3421
}

Response 200 (admitted):
{
  "status": "admitted",
  "position": 1,
  "purchase_window_seconds": 120,
  "purchase_window_expires_at": "2026-04-09T12:07:30Z"
}

Response 410: { "status": "sold_out", "message": "Sorry, this sale has ended." }
Response 401: { "error": "INVALID_OR_EXPIRED_TOKEN" }

Auth: queue_token in query string (signed JWT, no Authorization header needed)
Rate Limit: 1 poll per queue_token per 2 seconds (enforced by API Gateway per token)
Cache: No cache (dynamic per-user state)

# ─────────────────────────────────────────────────────────────
# 5. POST Purchase (admitted users only)
# ─────────────────────────────────────────────────────────────
POST /v1/sales/{sale_id}/purchase
Headers: Idempotency-Key: <uuid>

Request Body:
{
  "queue_token": "eyJhbGci...",
  "shipping_address_id": 98765,
  "payment_method_id": 11223
}

Response 200 (success):
{
  "purchase_id": 55001,
  "order_id": 99001,
  "sku": "SNEAKER-RED-10",
  "quantity": 1,
  "unit_price_cents": 17500,
  "confirmation_code": "FLASH-A3B9C",
  "estimated_delivery": "2026-04-12"
}

Response 410 (sold out — race condition, last unit taken):
{
  "error": "SOLD_OUT",
  "message": "We're sorry, the last unit was just taken."
}

Response 409 (purchase window expired):
{
  "error": "PURCHASE_WINDOW_EXPIRED",
  "message": "Your reserved spot has expired. Please re-enter the queue."
}

Response 429 (already purchased this sale):
{
  "error": "PURCHASE_LIMIT_EXCEEDED",
  "max_per_user": 1
}

Auth: Required (JWT must match queue_token.sub)
Rate Limit: 3 purchase attempts per user_id per sale
Idempotency: Required; prevents double-charge on network retry

# ─────────────────────────────────────────────────────────────
# 6. GET Inventory SSE Stream (real-time counter)
# ─────────────────────────────────────────────────────────────
GET /v1/sales/{sale_id}/inventory-stream
  (Server-Sent Events, text/event-stream)

Event stream:
  data: {"remaining": 9987, "sold": 13, "timestamp": "2026-04-09T12:00:05Z"}
  data: {"remaining": 9941, "sold": 59, "timestamp": "2026-04-09T12:00:10Z"}
  ...
  data: {"remaining": 0, "sold": 10000, "status": "sold_out", "timestamp": "..."}

Auth: Optional
Rate Limit: 1 SSE connection per user_id (server enforces via connection tracking)
Note: events emitted ~every 2 seconds or on significant inventory change (>10 units/event batch)

# ─────────────────────────────────────────────────────────────
# 7. POST Create Flash Sale (Admin)
# ─────────────────────────────────────────────────────────────
POST /internal/v1/sales
Auth: Admin JWT (role=admin)
Request Body: { full flash_sales record fields }
Response 201: { "sale_id": 12345, "status": "scheduled" }

# ─────────────────────────────────────────────────────────────
# 8. POST Cancel Flash Sale (Admin)
# ─────────────────────────────────────────────────────────────
POST /internal/v1/sales/{sale_id}/cancel
Auth: Admin JWT
Response 200: { "sale_id": 12345, "status": "cancelled", "refunds_queued": 8543 }
```

---

## 6. Deep Dive: Core Components

### 6.1 Atomic Inventory Decrement — Preventing Oversell

**Problem it solves:**
At 240,000 purchase attempts per second for 10,000 units, the inventory counter is the ultimate serialization point of the system. Two concurrent decrements must never both "succeed" when only 1 unit remains. Any solution must be: atomic (no TOCTOU race), fast (<5 ms P99), and recoverable if the purchase fails post-decrement (compensation).

**Approach Comparison:**

| Approach | Atomicity | Latency | Scalability | Oversell Risk | Notes |
|---|---|---|---|---|---|
| SQL `SELECT FOR UPDATE` on inventory row | Strong (row lock) | 5–50 ms (lock wait at high concurrency) | Poor (single row = serial writes) | None | Lock contention kills throughput at 240K RPS |
| DynamoDB atomic counter (`ADD :val` condition) | Strong | 5–15 ms P99 | Good (partition-level) | None | 3× slower than Redis; still too slow at burst |
| Redis DECR (no Lua) | Not safe alone (DECR can go negative) | <1 ms | Excellent | Present (oversell possible) | DECR of 0 returns -1; must add compensating logic |
| Redis Lua script (DECR with guard) | Strong (Lua is atomic) | <1 ms | Excellent (single node) | None | **Selected** |
| Redis inventory bucket sharding (N keys) | Strong per bucket | <1 ms | Excellent (N nodes) | None at bucket level | For extreme scale; adds complexity |
| Distributed coordinator (Zookeeper) | Strong | 2–10 ms (coordination overhead) | Poor at 240K RPS | None | Overkill; operational complexity |

**Selected Approach — Redis Single-Node Lua Script:**

```lua
-- Atomic purchase attempt Lua script
-- KEYS[1]: "inv:{sale_id}"
-- ARGV[1]: quantity requested (usually "1")

local remaining = tonumber(redis.call('GET', KEYS[1]))
if remaining == nil then
  return -2  -- inventory key not found (sale not warmed)
end
if remaining < tonumber(ARGV[1]) then
  return -1  -- sold out
end
local new_remaining = redis.call('DECRBY', KEYS[1], ARGV[1])
return new_remaining  -- returns count after decrement
```

**Why a single Redis node is correct here:**
Redis is single-threaded in its command execution. All Lua scripts execute atomically — no other command can interleave. This eliminates the need for any external locking. The tradeoff: a single Redis node is a SPOF (single point of failure) for the inventory counter.

**High Availability for the Inventory Redis Node:**

```
Replication topology for the inventory key:
  Primary:   Redis-Inv-Primary   (writes: DECR via Lua)
  Replica:   Redis-Inv-Replica   (read-only, used for remaining count display)
  
Sentinel:  3 Sentinel processes monitor Primary
  - Failover time: ~10–15 seconds
  - During failover: the Purchase Service returns "temporarily unavailable,
    please retry" for up to 15 seconds
  - After failover: new primary takes over; sale resumes
  - Data loss risk: up to 1 second of writes (async replication)
    → mitigated by: if replica count at failover is 8,990 and Aurora
      shows 9,001 purchases, the delta (11) is the "phantom sales."
      Reconciliation job identifies these and processes refunds.
```

**Compensation on Post-Decrement Failure:**
The inventory is decremented before the order is written to Aurora. If the Aurora write fails:
```
Purchase Service:
  1. Run Lua DECR → new_remaining = 9
  2. Write to Aurora: INSERT INTO flash_purchases ... → FAILS (e.g., network error)
  3. Compensation: run Redis INCR on inv:{sale_id} (restore the unit)
  4. Return 500 to client (with retry hint)
```
This is a best-effort compensation. If the Purchase Service crashes between steps 2 and 3, the compensation doesn't run. The reconciliation job (run every 5 minutes during sale) compares `total_inventory - remaining_in_redis` with `COUNT(purchase records in Aurora)`. Any discrepancy of ≥ 1 triggers an automatic Redis INCR to restore the "ghost" decrement.

**Interviewer Q&As:**

Q1: A single Redis node handling 240,000 RPS — doesn't Redis have a throughput limit?
A: Redis single-threaded command processing handles ~1 M simple operations/second (GET/SET). Lua scripts are more expensive — benchmarks show ~200,000–400,000 Lua executions/second on modern hardware (m5.2xlarge class). The actual purchase rate is much lower than 240,000 RPS — the queue system throttles the purchase path to ~200 admitted users/second (~200 Lua calls/second at the inventory key). The 240,000 RPS is the enqueue rate, which hits the Queue Service's Redis Sorted Set (a different node). The Inventory Redis node only processes admitted users. At 200 calls/second, a single Redis node is at 0.1% capacity.

Q2: How do you handle the Redis failover window (15 seconds) in terms of data consistency?
A: During the 15-second failover window, the Purchase Service returns 503 with a "Retry in 15 seconds" `Retry-After` header. No purchases are processed. No inventory is decremented. After failover completes, the new primary (former replica) has the last replicated inventory count. If replication lag was 1 second, up to 1 second of decrements may be missing from the replica. The reconciliation job (next scheduled run within 5 minutes) detects the discrepancy and corrects Redis by setting `inv:{sale_id}` to `total_inventory - COUNT(confirmed purchases in Aurora)`.

Q3: Can you avoid the single-node Redis SPOF while maintaining atomicity?
A: Yes, using **inventory bucket sharding**: split `total_inventory` across N Redis nodes (`inv:{sale_id}:0` through `inv:{sale_id}:N-1`), each holding `total_inventory / N` units. A purchase attempt randomly picks a shard; if that shard is exhausted, it tries adjacent shards. Atomicity is maintained within each shard (Lua script). Cross-shard coordination is not needed for correctness: a unit from shard 0 and a unit from shard 1 are equivalent. Tradeoff: "sold out" detection becomes probabilistic (all shards must be 0). Solution: maintain a global `inv_total:{sale_id}` key that is decremented *after* a shard DECR succeeds — checked for sold-out. This is the approach we use for hyper-popular sales (> 1 M concurrent users).

Q4: What if a user successfully decrements inventory but their payment fails at checkout?
A: The purchase record in Aurora starts with `status = 'reserved'`. If payment fails, the Checkout Service calls `POST /internal/sales/{sale_id}/release-unit` which: (1) updates the Aurora record to `status = 'cancelled'`, (2) runs `INCR inv:{sale_id}` to restore the unit in Redis, (3) publishes a `unit-restored` Kafka event to notify SSE subscribers (inventory count increases by 1). If users are in the waitlist, the Queue Admitter is triggered to admit one more user. Payment timeout (5 minutes) is enforced by the Checkout Service — unpaid reserves auto-cancel via a scheduled job.

Q5: How do you test that the system truly has zero oversell before a major sale?
A: Chaos and load testing: (a) Concurrent test: spin up 10,000 goroutines each running the Lua DECR script with inventory = 100. Verify exactly 100 return non-negative. (b) Chaos test: kill Redis primary mid-sale simulation; verify no extra units are sold during failover. (c) Integration test: run 100,000 simulated purchase attempts against a test sale with 100 units; verify exactly 100 purchase records in Aurora and `remaining = 0` in Redis. These tests run in CI against a staging environment before every sale. The reconciliation job is also tested by deliberately introducing a 5-unit discrepancy and verifying it corrects itself.

---

### 6.2 Queue-Based Fairness and Traffic Control

**Problem it solves:**
Without a queue, 1,000,000 users simultaneously sending purchase requests would: (a) overwhelm the Purchase Service, (b) result in random "winners" based on network timing (unfair), (c) create thundering herd on Redis/Aurora, and (d) make the system vulnerable to connection exhaustion. The queue converts the spike into a controlled, fair, metered stream.

**Approach Comparison:**

| Approach | Fairness | Throughput Control | Complexity | Failure Mode |
|---|---|---|---|---|
| No queue (direct purchase, first wins) | Poor (network lottery) | None (everything hits DB) | None | System overload; random errors; unfair |
| Rate limiting only (429 on excess) | Poor (retry advantage) | Partial | Low | Sophisticated bots retry more efficiently; unfair |
| SQS FIFO Queue | FIFO | Good | Low | No position visibility; can't tell users their wait time; SQS FIFO max 3,000 TPS without batching |
| Redis Sorted Set virtual queue | FIFO by timestamp | Excellent | Medium | Redis SPOF (mitigated by cluster); **Selected** |
| Dedicated queue service (e.g., Queue-it, Fastly) | FIFO | Excellent | Low (SaaS) | Vendor dependency; less customizable |
| Kafka as queue | Partition-ordered | Good | High | Kafka not designed for position queries; no random-access ZRANK equivalent |

**Selected Approach — Redis Sorted Set with Controlled Admission:**

```
Queue Entry:
  ZADD NX sale_queue:{sale_id}  {epoch_microseconds}  {jti}
  → NX: only adds if jti not already present (idempotent enqueue)
  → Returns: 1 (added) or 0 (already present)

Position Query:
  ZRANK sale_queue:{sale_id}  {jti}
  → Returns: 0-based position index (O(log N))
  → queue_position = rank + 1 (1-based for display)

Queue Depth:
  ZCARD sale_queue:{sale_id}  → total entries

Admitter Process (runs on a dedicated pod, not in Purchase Service):
  Algorithm:
    every 10ms:
      admission_batch_size = queue_admission_rate * 0.01  // (rate/s × 0.01s = rate/100)
      // e.g., 200/s × 0.01 = 2 per 10ms
      
      to_admit = ZRANGE sale_queue:{sale_id}
                   admitted_cursor
                   admitted_cursor + admission_batch_size - 1
      
      for jti in to_admit:
        HSET queue_token:{jti} status "admitted" admitted_at {now}
        EXPIRE queue_token:{jti} {purchase_window_seconds + buffer}
      
      admitted_cursor += len(to_admit)
      SET admission_cursor:{sale_id} {admitted_cursor}

  The Admitter runs as a single-leader process (ensured by a Redis distributed lock
  "admitter_lock:{sale_id}" with TTL 30s, renewed every 10s).
  If the admitter crashes, a standby admitter acquires the lock within 30s.
```

**Queue Position Estimation:**

```
estimated_wait_seconds = (position - admitted_cursor) / queue_admission_rate
```

For position 42,103, admitted_cursor = 2,000, admission_rate = 200/s:
`wait = (42,103 - 2,000) / 200 = 200.5 seconds ≈ 3.3 minutes`

**Queue Abandonment and Cleanup:**
Users who do not poll their queue status for >5 minutes are considered abandoned. A cleanup job runs every 5 minutes:
```
for jti in ZRANGE sale_queue:{sale_id} 0 -1:
  token_data = HGETALL queue_token:{jti}
  if token_data.status == "waiting" AND (now - token_data.last_poll_time) > 300s:
    HSET queue_token:{jti} status "abandoned"
    ZREM sale_queue:{sale_id} jti
    // Do NOT advance admitted_cursor; abandoned slots are naturally skipped
```
This frees queue slots for active users and prevents phantom positions.

**Dealing with Queue at Scale (1 M entries):**
Redis Sorted Set with 1 M members: ~200 MB (each entry ~200 bytes). `ZADD` is O(log N); `ZRANK` is O(log N). At 240,000 enqueue calls/second for 10 seconds = 2.4 M entries max. Redis handles this comfortably (log₂(2.4M) ≈ 21 comparisons per operation). The queue Redis node is separate from the inventory Redis node to prevent resource contention.

**Interviewer Q&As:**

Q1: What prevents a bot from acquiring thousands of queue positions using fake accounts?
A: Multi-layer defenses: (a) **Sale Token requirement**: to enter the queue, users must present a valid sale token issued by the Bot Filter. Getting a sale token requires passing reCAPTCHA v3 (>0.7 score), account age ≥ 30 days, and behavioral scoring. Bots can still pass CAPTCHA with sophisticated tools, but the account age requirement eliminates newly-created fake accounts. (b) **One token per account**: the sale token JWT embeds the user_id; the Queue Service prevents the same user_id from entering the queue more than once (Redis SETNX `queue:{sale_id}:{user_id}` → already-queued check). (c) **Per-user purchase limit**: even if a bot bypasses the queue, they can only buy 1 unit due to the `UNIQUE KEY uq_user_sale (user_id, sale_id)` constraint. The bottleneck for bots is creating verified aged accounts — an ongoing arms race.

Q2: What happens if the Admitter process crashes mid-sale?
A: The Admitter holds a distributed lock `admitter_lock:{sale_id}` with a 30-second TTL, renewed every 10 seconds. If the Admitter crashes, the lock expires in up to 30 seconds. The standby Admitter (a second pod waiting to acquire the lock) acquires it within 30 seconds and resumes from `admission_cursor:{sale_id}`. Users in the queue see their estimated wait increase by up to 30 seconds. Admitted users' tokens remain valid (they're stored in Redis with their TTL). No user loses their queue position — `admission_cursor` is the durable state for resumption.

Q3: How does the system handle the "flash sale bot" that has 10,000 valid accounts and gets 10,000 queue positions?
A: This is the hardest challenge (called "seat farming"). Defense layers: (a) The queue system gives each account 1 position — fair, but if they have 10,000 accounts, they get 10,000 positions. (b) Device fingerprinting (browser fingerprint, device ID) allows detection when multiple accounts share a device. A household can be capped at 3 concurrent queue positions per device fingerprint. (c) IP/ASN clustering: if 500 accounts enter from the same /24 subnet within 10 seconds, they're flagged as coordinated (alert, not automatic block, to avoid hurting residential NAT users). (d) Address de-duplication at checkout: if the same shipping address buys multiple units via different accounts, post-purchase cancellation of duplicates is triggered. (e) Velocity limits: an IP that creates 10 accounts within 30 days is flagged.

Q4: How do you handle the "estimated wait time" calculation becoming inaccurate as users abandon the queue?
A: Abandonment increases the effective admission rate (abandoned users are skipped quickly). We recalculate estimated wait time on every poll:
```
effective_remaining = ZCARD(sale_queue:{sale_id}) - admitted_cursor - abandoned_count_estimate
estimated_wait = effective_remaining / admission_rate
```
`abandoned_count_estimate` is maintained by the cleanup job incrementally. For simplicity in the user-facing estimate, we use a conservative formula that slightly overestimates wait time (better to surprise users with a shorter wait than promise a shorter one). The actual admission is always strictly FCFS regardless of estimate accuracy.

Q5: Can you scale the queue beyond a single Redis Sorted Set?
A: The theoretical limit of a Redis Sorted Set is 2^32 members (~4 B). At 200 bytes per entry and 2.4 M members, we use 480 MB — well within a 64 GB Redis node. Performance doesn't degrade significantly up to tens of millions of members. For truly hyper-scale sales (Apple iPhone launch: 100 M concurrent users), we'd use **consistent hashing to shard the queue across multiple sorted sets**: `queue_shard:{sale_id}:{user_id % N}`. The Admitter drains from all N shards in round-robin (round-robin ensures cross-shard fairness). Position calculation becomes approximate: `position ≈ ZRANK(shard_X) × N + shard_index`, which is accurate enough for user display purposes.

---

### 6.3 Bot Protection and Cache Warming

**Problem it solves:**
Two distinct challenges: (1) Bot protection — detecting and blocking automated purchase bots before they enter the queue, not after they've consumed resources. (2) Cache warming — ensuring all infrastructure is pre-loaded with sale data before the sale opens so the first request doesn't trigger a cold-start cascade.

#### 6.3.1 Bot Protection

**Approach Comparison:**

| Approach | Detection Accuracy | Latency Impact | False Positive Risk | Notes |
|---|---|---|---|---|
| IP rate limiting only | Low (residential proxies, VPNs) | None | Medium (NAT users) | Insufficient alone |
| reCAPTCHA v3 (score-based) | Medium (0.7+ score) | 50–200 ms | Low (transparent to users) | Good baseline |
| Device fingerprinting (FingerprintJS) | High for device-reuse | None (passive) | Low | Misses distributed bots |
| Account age + purchase history | High for new accounts | None (pre-computed) | Low | 30-day threshold eliminates fresh accounts |
| Behavioral ML model (mouse, timing) | Very high | 100–300 ms (async) | Low | **Selected as multi-layer** |
| Browser challenge (JavaScript proof-of-work) | High | 500ms–2s (client compute) | Low | Used for suspicious scores |

**Selected Multi-Layer Bot Protection:**

```
Layer 1 — WAF Rules (CloudFront + AWS WAF):
  - Block known bot IPs and ASNs (Tor exit nodes, data center IP ranges)
  - Rate limit: 100 requests/minute per IP to sale endpoints
  - Geo-restriction: block regions not served by this marketplace
  - Block requests without browser User-Agent or with known bot UAs

Layer 2 — Pre-Auth Bot Score (Bot Filter Service):
  POST /v1/sales/{sale_id}/pre-auth triggers:
  
  a. reCAPTCHA v3 validation:
     Google API call with captcha_token → score (0–1)
     Threshold: score < 0.5 → reject; 0.5–0.7 → additional challenge
  
  b. Account age check:
     user.created_at < NOW() - 30 days → require_additional_verification = true
     user.has_purchase_history = false → require_additional_verification = true
  
  c. Device fingerprint cross-reference:
     device_fingerprint in Redis SET "devices_this_sale:{sale_id}" ?
     → If present (same device tried for another account): flag as suspicious
     SADD "devices_this_sale:{sale_id}" {device_fingerprint} EX 3600
  
  d. Behavioral signal (async, does not block response):
     - Mouse movement entropy (too linear = bot)
     - Click timing distribution (too regular = bot)
     - Keyboard cadence on form fields
     - Signals sent via background XHR, scored by ML model
     - ML score > 0.8 → retroactively revoke sale token (add jti to blocklist)
  
  e. Composite bot score:
     bot_score = 0.4*(1-captcha_score) + 0.3*account_new_flag
               + 0.2*device_reuse_flag + 0.1*behavioral_score
     if bot_score > 0.6: reject (return 403)
     if bot_score > 0.4: issue sale token with "extra_verification" flag
                         (require SMS OTP at queue entry time)
     if bot_score <= 0.4: issue sale token normally

Layer 3 — Queue Entry Validation:
  - sale_token JWT signature validated (HMAC-SHA256 with server secret)
  - sale_token.exp not exceeded
  - Redis SETNX "sale_token_used:{jti}" "1" EX 300
    → If already used: reject as replay attack

Layer 4 — Purchase-Time Validation:
  - queue_token.sub (user_id) == JWT.user_id (same user)
  - queue_token.status == "admitted" in Redis
  - Redis SETNX "purchased:{sale_id}:{user_id}" "1" EX 86400
    → If already set: reject as duplicate purchase
  - Aurora UNIQUE KEY uq_user_sale as final safety net
```

#### 6.3.2 Cache Warming

**Problem:** If the sale launches cold (no pre-warmed caches), the first second of traffic causes thousands of cache misses, each falling back to Aurora/DynamoDB — potentially overloading the databases before caches warm up.

```
Sale Warming Pipeline (T-5 minutes before sale_starts_at):

Triggered by: Sale Scheduler (cron checks flash_sales WHERE
  status = 'scheduled' AND starts_at BETWEEN NOW() AND NOW() + 5 MIN)

Step 1: Warm inventory counter in Redis:
  SET inv:{sale_id} {total_inventory}
  SET inv_warmup_done:{sale_id} "1" EX 3600

Step 2: Warm sale metadata in Redis:
  HSET sale:{sale_id} status "warming" price_cents 17500 total 10000 ...
  EXPIRE sale:{sale_id} 3600

Step 3: Warm CDN edge caches:
  Send 10 synthetic GET /v1/sales/{sale_id} requests from each CDN PoP
  (CloudFront origin shield request triggers caching at edge)
  → Ensures first real user request hits CDN cache

Step 4: Pre-scale infrastructure:
  Scale Purchase Service pods: 20 → 200 (10×)
  Scale Queue Service pods: 5 → 100 (20×)
  Scale Bot Filter Service pods: 10 → 50 (5×)
  → K8s HPA overridden by pre-scale script 5 minutes before start

Step 5: Notify monitoring:
  Set alert thresholds to "flash sale mode":
  - PagerDuty: alert on error rate > 0.5% (not default 1%)
  - Queue depth monitor armed
  - Inventory SSE publisher started (0 subscribers yet, ready to accept)

Step 6: Sale status transition at T=0:
  UPDATE flash_sales SET status = 'live' WHERE sale_id = :id
  HSET sale:{sale_id} status "live"
  Soft-purge CDN cache for /v1/sales/{sale_id}
  → First real request after T=0 gets "live" status with buy button
```

**Interviewer Q&As:**

Q1: reCAPTCHA v3 is "invisible" and doesn't stop sophisticated bots — what's your fallback?
A: reCAPTCHA v3 scores intent, not humanity. For scores 0.5–0.7 (borderline), we fall back to reCAPTCHA v2 (the "I'm not a robot" checkbox challenge), which requires a human interaction. For the highest-risk situations (scores below 0.5), we require SMS OTP verification before issuing the sale token. This creates a hard human bottleneck — each phone number can receive 1 OTP per 5 minutes, and one OTP grants one sale token. Since most bots operate without real phone numbers, this is highly effective. The tradeoff: it adds 30–60 seconds to the user's pre-auth flow and slightly reduces conversion. We only enforce it for sales with bot_risk_level = "high" (configured per sale).

Q2: A sophisticated attacker has 10,000 compromised residential IP addresses and 10,000 aged bot accounts. How do you stop them?
A: This is a nation-state/professional scalper level attack and cannot be completely stopped in real-time. Mitigations at this level: (a) **Purchase address de-duplication**: at checkout, if >2 orders for the same sale ship to the same address, cancel all but 1 and refund the rest. (b) **Payment fingerprinting**: multiple accounts using the same payment method → cancel all orders. (c) **Post-sale ML analysis**: within 1 hour of sale end, the fraud ML model analyzes all purchases and flags suspicious clusters (same ISP, similar behavioral patterns, same device_fingerprint class). Flagged orders are cancelled and refunded within 24 hours. (d) **Pre-registration queue** (alternative design): require users to register interest 24 hours before a high-demand sale, verify identity (ID check), then randomly select a subset to participate. This is used by Nike's SNKRS app for extreme-demand releases.

Q3: How does CDN cache warming work precisely — won't the CDN return the "scheduling" status to real users during the 5-minute warming window?
A: The sale metadata endpoint response includes `status: "warming"` in the warming window. The frontend interprets `warming` similarly to `scheduled` — shows countdown, hides the buy button. At T=0, the Sale Scheduler updates the status to `"live"` in Redis and sends a CloudFront invalidation request for the sale metadata URL. CloudFront flushes the cached "warming" response within 1 second. The next request (real user at T=0+1s) gets a fresh response from origin with `status: "live"` — this new response is cached with a 2-second TTL. Effectively, all users see the "live" state within 3 seconds of T=0. The CDN TTL of 2 seconds during the live sale window ensures inventory data is no more than 2 seconds stale.

Q4: What is the pre-scale approach when you don't know exact demand?
A: We classify flash sales by demand tier based on historical data and pre-sale signals: **Tier 1** (< 10K interest signals): no pre-scale, HPA handles it. **Tier 2** (10K–100K): 5× pod pre-scale. **Tier 3** (100K–1M): 20× pre-scale + dedicated infrastructure (separate K8s namespace). **Tier 4** (> 1M, e.g., iPhone launch): full dedicated cluster with pre-provisioned instances, custom auto-scaling. Demand signals come from: pre-registration count, social media trend analysis (Twitter/X real-time API), historical comparables (same category, same brand last year). For critical sales, we "stress test" at T-24 hours with a simulated load matching Tier classification.

Q5: How do you handle clock skew between servers when using epoch_microseconds as the queue sort key?
A: All servers use NTP-synchronized clocks with <1 ms accuracy. For queue ordering, 1 ms accuracy means users within the same millisecond are assigned an arbitrary but consistent order (by `jti` UUID sort as a tiebreaker, since ZADD with the same score maintains existing order or uses lexicographic order). In practice, fairness at 1 ms resolution is indistinguishable to users — no one legitimately arrives within 1 ms of another due to network propagation time. The bigger concern is clock drift under load (CPU contention causing NTP sync delays). Mitigation: use `CLOCK_REALTIME` from the OS (not application-level time), which maintains NTP sync independently of application load.

---

## 7. Scaling

### Horizontal Scaling

- **Bot Filter Service**: Stateless; scales to 50 pods at sale start (pre-scaled). Each pod: 4 vCPU, 4 GB RAM. reCAPTCHA validation is an external call (Google API); throughput limited by Google API rate limit (configurable up to 1 M/day on paid plan).
- **Queue Service**: Stateless (state in Redis); scales to 100 pods. Each pod: 2 vCPU, 2 GB RAM, handles ~2,400 RPS (240K / 100 = 2,400 enqueue RPS per pod at burst).
- **Admitter Process**: Single leader (Redis lock); only 1 active pod needed for drain rate of 200/s. Failover standby: 2 additional pods waiting for lock.
- **Purchase Service**: Stateless; scales to 200 pods. Most requests are queue position polls (cheap) or purchase confirmations (rate-limited by admission). At 200 admitted/s: 200 pods handle 1 purchase/s each — trivial.
- **Inventory SSE Publisher**: Stateless; scales to 50 pods. Each pod maintains 10,000 SSE connections = 50 × 10,000 = 500,000 total concurrent SSE clients. Events are fan-out (Kafka → all pods → all clients).

### DB Sharding

- **Aurora (Flash Sales + Purchases)**: No sharding needed at 10,000 concurrent sales × 10,000 units. Purchase writes are capped by admission rate (200/s per sale × at most 10 concurrent peak sales = 2,000 writes/s). Aurora handles 20,000 writes/s without sharding.
- **Redis (Inventory Counter)**: Single node per sale (by design, for atomicity). Multiple sales use different Redis nodes (sale_id % N nodes). For hyper-scale single sales: bucket sharding as described in Section 6.1 Q3.
- **Redis (Queue)**: Single sorted set per sale; split into shards for >1 M concurrent users as described in Section 6.2 Q5.
- **ClickHouse (Analytics)**: Sharded by `sale_id` across a 3-node ClickHouse cluster. Each node handles analytics writes independently.

### Replication

- **Redis Inventory**: Primary + 1 replica; Sentinel failover (15 s RTO, ~1 s RPO).
- **Redis Queue**: Primary + 1 replica; queue sorted set is reconstructed from Kafka replay if data is lost (Kafka retains for 7 days).
- **Aurora**: Multi-AZ + 2 read replicas. Purchases written to primary; read replicas serve per-user purchase history checks.
- **Kafka**: RF=3; 8 partitions per topic (scalable to 64). Consumer groups: queue-event-consumers (analytics), purchase-event-consumers (order service, audit, SSE publisher).

### CDN

- Pre-sale countdown page: CDN TTL 30 s. Served globally from 20+ CloudFront PoPs.
- Live sale status: CDN TTL 2 s (to pick up inventory changes quickly).
- Sold-out state: CDN soft-purge issued immediately when `remaining = 0` event fires; all users see "sold out" within 3 s.
- Product images for sale: Pre-warmed at CDN with 1-year TTL (content-hash URLs).

### Interviewer Q&As

Q1: The system is designed around a single Redis DECR for inventory. What is the maximum sale size it can support?
A: The Redis DECR approach supports any sale size in terms of units (1 to 10 M units). The bottleneck is not the inventory counter (200 admitted users/s × 1 Lua call/s = trivial). The real scalability limit is the queue Redis sorted set. At 1 M users × 200 B = 200 MB — a single Redis node's memory is fine. At 100 M users × 200 B = 20 GB — exceeds practical single-node size. For sales of this scale, we use sorted set sharding (Section 6.2 Q5) or a purpose-built queue like AWS SQS with custom position tracking.

Q2: How does the system scale the SSE connection handling for 500,000 concurrent clients?
A: SSE uses long-lived HTTP/2 connections. Each SSE Publisher pod handles 10,000 connections using Node.js's event loop (non-blocking I/O) or Go's goroutines. At 500,000 connections across 50 pods: 50 × ~8,000 file descriptors/pod. Each pod subscribes to the `inventory-events` Kafka topic as a consumer — Kafka delivers events to all 50 consumers independently (fan-out via multiple consumer groups). Each pod pushes events to its 10,000 connected clients. This architecture means adding pods scales SSE linearly without Kafka reconfiguration (each new pod joins the consumer group automatically, or better: each pod uses an independent consumer with `auto.offset.reset=latest` since SSE doesn't need at-least-once — missed events are corrected by the next event).

Q3: How do you gracefully degrade if the Queue Service Redis node becomes unavailable during a live sale?
A: Failure scenario: Queue Redis goes down → Queue Service cannot accept new enqueues or answer position polls. Response: (a) API Gateway detects Queue Service errors via health probe and routes requests to a "degraded mode" handler. (b) Degraded mode: accept purchase requests directly (bypass queue), apply per-user limit check (from Aurora), attempt inventory decrement (Redis). This is "first-come-first-served without a virtual queue" — less fair but still correct (no oversell). The queue-bypass mode is pre-built and toggle-able via a feature flag, tested in chaos drills. Alerts fire immediately; engineers can restore the Redis node within minutes.

Q4: How does the system handle the "flash sale effect on regular platform"? What isolation mechanisms exist?
A: Flash sale components run in a dedicated Kubernetes namespace with its own node pool (EC2 instances tagged for flash-sale use). Network policies prevent flash-sale pods from calling regular platform services directly — they use the same downstream services (Catalog, Pricing) but through separate service instances with reserved capacity. The Redis cluster for inventory is physically separate from the main platform Redis. Aurora uses a separate cluster instance. The only shared infrastructure is the CDN (CloudFront) — but CDN is effectively infinite-scale (CloudFront handles trillions of requests/day globally). Regular platform SLAs are insulated from flash sale traffic spikes.

Q5: How do you prevent the queue system from being "gamed" by users who time their entry to be first (e.g., a script that fires at exactly millisecond 0)?
A: Several mitigations: (a) **Sale token pre-distribution is time-bounded**: sale tokens are only issued starting at T-30 minutes. Any user who got a token earlier than T-0 can enter the queue from T=0 onwards, but the first to press "Buy Now" at T=0.000 has an advantage. We accept this as "fair" — it's skill, not money or connections that determines position. (b) **Queue open window**: the queue accepts entries for only 10 seconds (the "opening window"). After 10 seconds, the queue closes to new entries. All users who entered within the 10-second window are ordered by their enqueue timestamp. (c) **Server-side timestamp**: the queue timestamp is recorded when the request arrives at the Queue Service, not at the client. Network latency variations (< 50 ms) add inherent fairness noise. (d) **True lottery option**: for ultra-high-demand sales, we can switch to a lottery model: accept all entries during a 60-second window, then randomly select winners. This eliminates timing advantage entirely and is used for PlayStation 5 / GPU drops.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | Recovery |
|---|---|---|---|---|
| Redis inventory node failure | Purchases blocked for ~15 s during failover | Sentinel health check, CloudWatch | Sentinel auto-promotes replica; Purchase Service returns 503 "Retry in 15s" | Auto; reconciliation job corrects drift |
| Redis queue node failure | New enqueues fail; position polls fail | Health probe, error rate spike | Feature flag switches to queue-bypass mode (direct purchases, per-user limit enforced) | Manual or auto toggle; Redis restart |
| Admitter process crash | Queue admission pauses up to 30 s | Lock TTL expiry + standby acquisition | Standby Admitter acquires lock and resumes from `admission_cursor` | Automatic; max 30 s delay |
| Purchase Service pod crash | In-flight purchase attempt fails | K8s liveness probe → restart | Client retries with same Idempotency-Key; idempotency check prevents double purchase | Auto-restart; client retry succeeds |
| Aurora primary failure | Purchase write fails for ~30 s | CloudWatch RDS events | Aurora Multi-AZ auto-failover; in-flight purchases: Redis DECR succeeded but order not written → reconciliation restores these units | RTO ~30 s; reconciliation corrects |
| CDN PoP outage | Increased origin load for affected region | CloudFront health metrics | Route 53 DNS failover to adjacent PoP; slight latency increase | Automatic <60 s |
| Bot Filter Service overload | Pre-auth latency spikes; users can't get sale tokens | Latency SLO breach alert | Scale Bot Filter pods horizontally; temporarily lower CAPTCHA threshold to reduce Google API calls | HPA scales within 60 s |
| SSE Publisher crash | Clients lose real-time inventory updates | K8s liveness probe | Clients reconnect (SSE has built-in reconnect with `Last-Event-ID`); missing events are covered by poll-on-reconnect | Auto-restart; SSE reconnect in <5 s |
| DDoS on sale endpoint | Origin overload | CloudFront anomaly detection | AWS Shield Advanced + WAF rate limits; offload to CDN; CDN absorbs most traffic | Automatic; WAF rules updated in real-time |
| Kafka broker failure | Event lag; SSE updates delayed | Kafka broker health; consumer lag | RF=3; consumers failover to surviving brokers; SSE may lag by seconds | Automatic; catch-up within minutes |
| Sale overclaim on Redis failover (data loss) | Inventory count understated (appears lower than actual) | Reconciliation job detects discrepancy | INCR Redis by discrepancy amount; excess buyers get refunds | Automated reconciliation within 5 min |

### Idempotency

- **Queue Entry**: Idempotency-Key + Redis NX prevents double-queue. `ZADD NX` prevents duplicate entries in sorted set.
- **Purchase**: `queue_token_jti` UNIQUE in `flash_purchases`; `SETNX purchased:{sale_id}:{user_id}` at Redis layer; Aurora UNIQUE constraint as final net.
- **Inventory Decrement**: Lua script is idempotent with respect to the counter (either decrements once or returns sold-out signal).
- **Sale Token**: `SETNX sale_token_used:{jti}` prevents replay attacks.

### Circuit Breakers

- **Purchase Service → Redis (Inventory)**: If Redis returns 5 consecutive errors (not "sold out" — actual errors), circuit opens; Purchase Service returns 503 with 15-second `Retry-After`. Prevents Aurora thundering herd fallback.
- **Queue Service → Redis (Queue)**: Circuit opens on Redis errors → activates queue-bypass mode.
- **Bot Filter → Google reCAPTCHA API**: If Google API is unavailable (>5 s timeout), circuit opens → fall back to account-age-only check (CAPTCHA requirement bypassed temporarily). Mitigates Google dependency creating a sale blocker.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Dashboard Priority |
|---|---|---|---|
| Inventory remaining | Gauge | 0 → fire "SOLD_OUT" alert immediately | P0 (real-time) |
| Purchase success rate | Counter/Rate | <95% → Page (users getting errors) | P0 |
| Queue enqueue RPS | Counter | >300K/s → capacity warning | P0 |
| Queue depth | Gauge | >5M → scale warning | P1 |
| Admission rate (actual vs. configured) | Gauge | Deviation >20% → Admitter alert | P1 |
| Redis inventory Lua latency P99 | Histogram | >5 ms → Warning | P0 |
| Bot rejection rate | Counter | >80% of attempts → possible DDoS, not just bots | P1 |
| False positive rate (legitimate users rejected) | Gauge | >2% → Warning | P1 |
| SSE connection count | Gauge | <50% of expected → alert (clients failing to connect) | P2 |
| Reconciliation discrepancy | Gauge | Any non-zero → Alert for investigation | P0 |
| Queue-bypass mode active | Boolean | True → Page (degraded operation) | P0 |
| Sale revenue rate (units sold × price / sec) | Rate | Sudden drop to 0 during live sale → Page | P0 |
| Admitter lock contention | Counter | >0 standby acquisitions → Warning | P1 |

### Distributed Tracing

- Full trace through: CDN → API Gateway → Bot Filter → Queue Service → Redis → Purchase Service → Aurora.
- Key spans: `bot_filter.validate`, `captcha.verify`, `queue.enqueue`, `redis.zadd`, `queue.get_position`, `redis.zrank`, `purchase.attempt`, `redis.lua_decr`, `aurora.insert_purchase`.
- 100% sampling during active flash sales (volume is controlled via queue admission — max 200 admitted/s, manageable trace volume).
- Trace exemplars linked from Grafana latency histograms for fast drill-down during incidents.

### Logging

- All bot rejections logged with: `user_id` (hashed), `ip_hash`, `rejection_reason`, `bot_score`, `captcha_score`, `sale_id`, `timestamp`.
- All purchases logged with: `user_id` (hashed), `sale_id`, `queue_position`, `queue_wait_seconds`, `purchase_latency_ms`, `redis_remaining_after`.
- Sale lifecycle events (scheduled, warmed, live, sold_out, ended) logged to dedicated `flash-sale-lifecycle` log stream for SLA reporting.
- Reconciliation job results logged: `sale_id`, `redis_count`, `aurora_count`, `discrepancy`, `correction_applied`.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Alternative Considered | Why Selected | Trade-off Accepted |
|---|---|---|---|
| Redis single-node Lua for inventory decrement | DynamoDB atomic counter, SQL row lock, distributed leases | Sub-millisecond atomicity; no network coordination needed; Lua script is provably atomic; high throughput (1 M ops/s) | Single node SPOF; ~15 s failover window with brief oversell prevention outage; mitigated by Sentinel + reconciliation |
| Redis Sorted Set for queue | Kafka consumer queue, SQS FIFO, in-memory heap | ZRANK provides O(log N) position queries; ZADD NX prevents duplicates; native TTL management; ~200 B per entry efficient | Redis SPOF for queue; single sorted set has practical limit (~10 M members); mitigated by bucket sharding for hyper-scale |
| Queue-based admission (virtual queue) | Direct purchase (no queue), rate-limiting + retry | Eliminates thundering herd on Purchase Service; FCFS fairness; predictable load on downstream services; better UX (estimated wait vs. random 429 retries) | Added complexity (Admitter process, queue token lifecycle); 30-second delay if Admitter crashes |
| Sale token + multi-layer bot filter | CAPTCHA only, IP rate limiting only, post-purchase fraud detection | Defense-in-depth reduces bot throughput before they consume queue resources; account age check is highly effective against fresh bot accounts | Adds 200–500 ms to pre-auth flow; legitimate users with new accounts or low CAPTCHA scores need extra verification (friction) |
| Pre-scaling (5 min before sale) | Reactive HPA only | HPA reacts to load after it arrives (60–120 s lag); 5-minute burst would be over before HPA scales; pre-scaling ensures capacity is ready at T=0 | Cost: running 200 Purchase pods for 10 min before sale costs ~$0.50 at AWS prices — negligible vs. sale revenue |
| Separate Redis for inventory vs. queue | Single Redis Cluster for both | Inventory Lua scripts must be on a dedicated node for atomicity guarantees; queue is a different workload (sorted set heavy vs. string heavy); failure isolation | Additional Redis nodes to operate; more configuration; mitigated by managed Redis (ElastiCache) |
| Reconciliation job (eventual correction) | Synchronous SAGA with rollback | Synchronous saga over distributed systems adds latency and complexity; atomic-level correctness at Redis layer is sufficient; reconciliation handles rare edge cases asynchronously | Up to 5-minute window of uncorrected discrepancy; in practice, < 0.01% of sales ever need reconciliation |
| Lottery model (optional for hyper-demand) | Pure FCFS queue | Lottery eliminates timing-skill advantage; fairer for casual shoppers; prevents bot timing optimization | Requires pre-registration (reduces impulse purchases); less exciting than "race" (some users prefer the competition) |

---

## 11. Follow-up Interview Questions

**Q1: How would you design a waitlist for flash sales (notify users when a cancellation opens up a unit)?**
A: Waitlist entries are stored in a Redis Sorted Set `waitlist:{sale_id}` with score = enqueue_time (same as main queue). When a purchase is cancelled (payment failure or explicit cancellation), the Purchase Service: (1) runs `INCR inv:{sale_id}` to restore the unit, (2) publishes a `unit-restored` Kafka event, (3) the Waitlist Admitter consumer reads the event, pops the top entry from `waitlist:{sale_id}`, sends the user a push notification / email with a time-limited link (`waitlist_token`, valid 10 minutes), (4) the user clicks the link and enters a fast-path purchase flow (bypassing the queue, since they've already waited). If they don't act within 10 minutes, the next waitlist member is notified.

**Q2: How would you design the post-sale reconciliation to detect oversells?**
A: The reconciliation job runs every 5 minutes during an active sale and once at sale end. Query: `SELECT count(*) FROM flash_purchases WHERE sale_id = :id AND status IN ('reserved', 'confirmed')`. Compare with `total_inventory - redis.GET(inv:{sale_id})`. If `aurora_count > total_inventory - redis_remaining`: oversell detected (redis_remaining went negative due to a failover data loss). Action: INCR Redis to true remaining; flag the excess purchase records for refund review. If `aurora_count < total_inventory - redis_remaining`: inventory was decremented but not recorded (Redis DECR succeeded but Aurora write failed before compensation). Action: restore units in Redis (`INCR by discrepancy`). Both cases alert the on-call engineer with full details.

**Q3: How would you implement a "flash deal" where the price drops every 5 minutes for 30 minutes until inventory depletes?**
A: A Sale Scheduler cron job reads the price schedule from the `flash_sales` table (add a `price_schedule` JSONB field: `[{"at_minute": 0, "price": 17500}, {"at_minute": 5, "price": 15000}, ...]`). Every 5 minutes, the scheduler updates Redis `HSET sale:{sale_id} price_cents 15000` and publishes a Kafka `price-change` event. The Purchase Service reads the current price from Redis at purchase time (not snapshot). The SSE publisher includes price in its event stream so clients see the new price in real-time. The cart's "price snapshot" for the purchase is set to the Redis price at the moment of the Lua DECR — preventing price changes between decrement and order creation from creating ambiguity.

**Q4: How would you build the real-time sale analytics dashboard for the merchandising team?**
A: The admin dashboard reads from ClickHouse, which receives events via Kafka (`purchase-events` topic). ClickHouse materialized views pre-aggregate: units sold per minute, geographic distribution (by user's region), bot rejection rate, conversion rate (queue entrants / purchases). The dashboard polls the ClickHouse HTTP API every 5 seconds. For inventory remaining, the dashboard reads directly from Redis (`GET inv:{sale_id}`) for sub-second freshness. Key charts: (1) Inventory countdown gauge, (2) Purchases/minute bar chart (real-time), (3) Queue depth time series, (4) Bot rejection rate pie chart, (5) Geographic heat map.

**Q5: What happens if the sale timer and the sale actual end diverge (e.g., an outage delays the timer)?**
A: The sale's `ends_at` timestamp is authoritative. When a purchase request arrives, the Purchase Service checks `NOW() > flash_sales.ends_at` (from Redis cache, refreshed every 30 s). If past `ends_at`, the response is 410 Gone regardless of remaining inventory. Similarly, even if inventory remains but time has expired, no new purchases are accepted. This is enforced at two layers: (1) the Queue Service stops admitting users 60 seconds before `ends_at`, (2) the Purchase Service rejects with 410 after `ends_at`. The sale status is updated to "ended" (not "sold_out") in Aurora and Redis, and CDN cache is purged.

**Q6: How would you implement geographic fairness — ensuring users from different regions have equal access?**
A: By default, all users globally compete in the same queue — users with lower network latency to the sale endpoint have an advantage. For true geographic fairness: implement **regional queues**. Allocate inventory proportionally to regions based on historical demand (e.g., US gets 60%, EU 30%, APAC 10%). Each region has its own queue sorted set and its own inventory slice in Redis. Users are routed to their regional queue via Route 53 geolocation routing. Winners from each region's queue get access to that region's inventory slice. This prevents a single high-concentration group (e.g., all US East Coast users) from buying out all inventory before APAC users even get a chance.

**Q7: How would you design an "early access" tier for loyalty program members?**
A: The sale has an `early_access_starts_at` timestamp (e.g., 10 minutes before `starts_at`). During the early access window, only users with `loyalty_tier >= 'gold'` (checked from Auth Service JWT claim) can obtain sale tokens and enter the queue. A separate sorted set `early_access_queue:{sale_id}` holds these entries. At `starts_at`, this queue merges with the general queue: early access members keep their original positions (their scores were set during the early window, so they naturally sort first). The general queue opens and starts accepting entries from `starts_at`. The Admitter drains the combined sorted set in FCFS order — early access members are naturally first because their enqueue_time was earlier.

**Q8: How do you handle a flash sale for a product with multiple SKU variants (sizes/colors)?**
A: Each variant (SKU) has its own inventory counter in Redis: `inv:{sale_id}:{sku}`. A user selects their variant before entering the queue. The queue is shared across all variants (one sale_queue sorted set), but the queue_token includes the selected SKU. At purchase time, the Lua DECR runs against `inv:{sale_id}:{selected_sku}`. If the selected SKU is sold out but others are available, the user is offered a substitute. The sold_out event fires per-SKU: when `inv:{sale_id}:RED-10 = 0`, only that SKU shows "sold out" — the sale continues for other variants. The overall sale status transitions to "sold_out" when all SKU counters reach 0 (checked by the Admitter after each drain cycle: `SUM(GET inv:{sale_id}:*) == 0`).

**Q9: What is your strategy for communicating to users who fail to get a unit after waiting in queue?**
A: When a user's queue position reaches the "admitted" state but inventory has just hit 0 (race condition: last unit sold while they were being admitted), the response is: `{"status": "sold_out", "message": "We're sorry, this sale has ended. You were #{position} in queue."}` plus: (a) an email confirming they were in queue when it sold out (validates their effort), (b) an automatic enroll in the waitlist for cancellations, (c) a link to similar products and the next scheduled sale (if any) for this product. This "closure experience" reduces customer service contacts and maintains brand trust.

**Q10: How would you handle a merchandise error — wrong sale price published (e.g., $1 instead of $100)?**
A: An admin calls `POST /internal/v1/sales/{sale_id}/cancel` which: (1) sets `status = 'cancelled'` in Aurora and Redis, (2) issues CloudFront invalidation (users see "sale cancelled"), (3) stops the Admitter, (4) for all existing purchases (status = 'reserved'), triggers an automatic refund workflow via the Order Service. An email is sent to all affected users explaining the error and offering a compensation coupon. The sale can be re-created with the correct price and launched as a new sale. The key architectural requirement: `sale cancel` is an idempotent admin API that can be called repeatedly without side effects (second call sees `status = cancelled` and returns 200 without re-triggering refunds).

**Q11: How does the system scale to support 100 simultaneous flash sales (e.g., Black Friday with many products on sale)?**
A: Each flash sale has its own Redis keys (namespaced by sale_id). 100 simultaneous sales use 100 inventory keys and 100 queue sorted sets. Redis Cluster distributes these keys across shards automatically. The Admitter process handles multiple sales: each sale has its own Admitter goroutine (one process, N goroutines). At 100 sales × 200 admitted/s each = 20,000 total admitted/s — well within a Redis Cluster's capacity. The shared infrastructure (API Gateway, Kafka) scales horizontally. The only concern is if all 100 sales open simultaneously: 100 × 240,000 RPS = 24 M RPS at peak — this would require 100× pre-scaling. In practice, Black Friday sales are staggered by 30-minute intervals to prevent this scenario.

**Q12: How would you design the queue system to handle a user who closes their browser mid-queue?**
A: The frontend sends heartbeat requests `POST /v1/sales/{sale_id}/queue/heartbeat?queue_token={token}` every 30 seconds while the queue page is open. The Queue Service records `last_heartbeat:{jti}` in Redis (TTL 60 s). The cleanup job checks for tokens where `last_heartbeat` TTL has expired (no heartbeat for > 60 s) — these users' browser is likely closed. Their queue entry is marked "abandoned" and removed from the sorted set. When the user reopens the browser and loads the queue page, they find their token is "abandoned" (or expired). They would need to re-enter the queue (no position preservation for abandoned slots). This is intentional: holding queue positions for inactive users wastes slots and extends wait times for active users.

**Q13: How do you prevent the sale countdown page from becoming a DDoS vector (500,000 users refreshing constantly)?**
A: The countdown page is a static SSR HTML page served entirely from CDN with a 30-second TTL. The CDN has effectively unlimited capacity for static pages. 500,000 users refreshing every 30 seconds = ~16,667 RPS to CDN. With 98% CDN hit rate, only ~333 RPS reach the origin (Pre-Sale SSR Service). The countdown timer is a JavaScript countdown running client-side — it does NOT call the server every second. The sale start time is embedded in the HTML response. The frontend only calls the server when the timer hits zero (to fetch the sale token). This design makes the countdown page nearly free in terms of origin compute.

**Q14: How would you handle a flash sale for a digital product with unlimited inventory (e.g., a discounted software license for first 10,000 buyers)?**
A: This is identical to physical inventory handling from the system's perspective — the inventory counter is set to 10,000 and decremented on each purchase. The only difference is fulfillment (deliver a license key vs. shipping a package). The Redis Lua DECR still enforces the 10,000 limit atomically. The queue system still provides fairness. Post-purchase, the Order Service routes to a "digital fulfillment" workflow (generate license key, send email) instead of a physical fulfillment workflow. No inventory reservation hold is needed (no physical stock management). The sale system is product-type-agnostic by design.

**Q15: After the system is built, how do you conduct a disaster recovery drill for a flash sale?**
A: The DR drill runs 1 week before a major planned sale. Procedure: (1) Simulate the sale at 10% scale in a staging environment. (2) Inject failures: kill Redis inventory primary mid-drill → verify Sentinel failover, verify 503 responses during failover, verify reconciliation corrects drift. (3) Kill Admitter pod → verify standby acquires lock within 30 s. (4) Kill 50% of Purchase Service pods → verify remaining pods handle load, HPA scales up. (5) Simulate DDoS (1M synthetic requests/s at WAF) → verify WAF rules block traffic. (6) Full Aurora failover simulation → verify purchases resume within 30 s. Each scenario has a documented pass/fail criterion. Results are reviewed in a game-day readout. Any failed scenario triggers a ticket and a fix before the production sale.

---

## 12. References & Further Reading

1. **Redis Command Reference — ZADD, ZRANK, ZRANGEBYSCORE** — Official Redis documentation for the Sorted Set commands used in the queue implementation. https://redis.io/commands/zadd/ https://redis.io/commands/zrank/

2. **Redis Documentation — Lua Scripting (EVAL)** — Covers atomic Lua script execution, the foundation of the oversell-safe inventory decrement. https://redis.io/docs/manual/programmability/eval-intro/

3. **Martin Kleppmann, "Designing Data-Intensive Applications" (2017)** — O'Reilly. Chapter 7 (Transactions) covers optimistic vs. pessimistic locking; Chapter 9 covers linearizability — directly applicable to the inventory decrement correctness argument.

4. **Lamport, "Time, Clocks, and the Ordering of Events in a Distributed System" (1978)** — CACM. Theoretical foundation for the timestamp-based queue ordering and the impossibility of perfectly synchronized clocks. https://dl.acm.org/doi/10.1145/359545.359563

5. **Twitter Engineering Blog — "Handling Flash Traffic with Queueing"** — Case study on handling sudden traffic spikes for high-demand events using virtual queues. https://blog.twitter.com/engineering

6. **Ticketmaster / Live Nation Engineering — Virtual Waiting Room** — Industry implementation of a queue system for high-demand ticket sales, directly analogous to the flash sale queue. https://live-nation-entertainment.engineering/

7. **Nike SNKRS Engineering Blog** — Describes Nike's approach to limited sneaker drops, including bot detection and lottery mechanisms for high-demand releases.

8. **AWS Shield Advanced Documentation** — DDoS protection service used at the CDN/WAF layer for bot traffic mitigation. https://docs.aws.amazon.com/waf/latest/developerguide/shield-advanced.html

9. **Google reCAPTCHA v3 Developer Guide** — Score-based CAPTCHA used in the Bot Filter Service pre-auth flow. https://developers.google.com/recaptcha/docs/v3

10. **ClickHouse Documentation — MergeTree Engine** — Time-series analytics storage for real-time sale dashboards and bot detection signal aggregation. https://clickhouse.com/docs/en/engines/table-engines/mergetree-family/mergetree

11. **Apache Kafka Documentation — Consumer Groups** — Foundation for the fan-out architecture (multiple consumer groups for SSE publisher, analytics, audit) from a single Kafka topic. https://kafka.apache.org/documentation/#intro_consumers

12. **Amazon ElastiCache for Redis — Sentinel Mode** — Documents the 15-second failover behavior and replica promotion used for the inventory Redis node. https://docs.aws.amazon.com/AmazonElastiCache/latest/red-ug/Replication.Redis-RedisCluster.html

13. **Pat Helland, "Idempotence Is Not a Medical Condition" (2012)** — ACM Queue. Covers the idempotency patterns used for queue entry, purchase, and token replay prevention. https://dl.acm.org/doi/10.1145/2181796.2187821

14. **Netflix Tech Blog — "Lessons Netflix Learned from the AWS Outage"** — Resilience patterns (circuit breakers, graceful degradation) applied to the flash sale system's multi-layer fallbacks. https://netflixtechblog.com/lessons-netflix-learned-from-the-aws-outage-deefe5fd0c04

15. **Brendan Gregg, "Systems Performance: Enterprise and the Cloud" (2nd ed., 2020)** — Covers Redis profiling, Linux networking (for SSE connection tuning), and capacity planning methodologies used in scaling estimates. http://www.brendangregg.com/systems-performance.html
