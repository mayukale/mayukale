# System Design: Shopping Cart

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Add / Update / Remove Items** — Users can add items (with quantity, variant/SKU), update quantities, and remove items from the cart.
2. **Persistent Cart** — Authenticated users' carts persist across sessions, devices, and browsers indefinitely (until checkout or manual clearing).
3. **Guest Cart** — Unauthenticated users get a session-based cart stored server-side (tied to a session token in a cookie), retained for 30 days.
4. **Cart Merge on Login** — When a guest user authenticates, the guest cart is merged with any existing persistent cart. Conflict resolution: for overlapping SKUs, sum quantities (capped at available inventory).
5. **Inventory Reservation (Soft Hold)** — When an item is in the cart, optionally place a soft inventory hold (time-limited, e.g., 15 minutes) to protect the item for the shopper. Hard reservation occurs at checkout.
6. **Price Recalculation** — On cart view and before checkout, recalculate all line-item prices in real time (prices may have changed since the item was added). Show a "price changed" notice if applicable.
7. **Coupon / Promo Code Application** — Accept promotion codes, validate them (single-use, per-user limits, expiry, minimum order value), compute discounts, and display updated totals.
8. **Cart Summary** — Return item subtotal, shipping estimate, taxes (jurisdiction-based), promo discount, and grand total.
9. **Saved-for-Later** — Users can move items out of the active cart into a "saved for later" list without removing them from the system.
10. **Share Cart** — Generate a shareable cart link (optional).

### Non-Functional Requirements

1. **Availability** — 99.99% uptime. Cart writes are revenue-critical: a lost cart item = a lost sale.
2. **Latency** — Cart read (GET cart) ≤ 50 ms P99; cart write (add/update/remove) ≤ 100 ms P99.
3. **Durability** — No cart data loss once a write is acknowledged. Replication before ACK.
4. **Consistency** — Cart contents (item list, quantities) must be strongly consistent per user: a user should never see a stale cart after a successful write (read-your-writes guarantee).
5. **Scale** — Design for Amazon scale: ~100 M active carts at any time; peak write RPS of ~100,000 during flash events.
6. **Idempotency** — Duplicate add/update/remove requests (due to client retries) must not corrupt cart state.
7. **Security** — Cart data is user-private; strict ownership validation on every request; no cart enumeration attacks.
8. **Auditability** — Cart events (add, remove, checkout) must be recorded for analytics, fraud detection, and customer service.

### Out of Scope

- Checkout payment processing (payment gateway integration)
- Tax calculation engine (assumed as an external microservice)
- Shipping carrier integration
- Order management post-checkout
- Product recommendations within the cart (widget, but fulfillment is the Recommendations Service)
- Wishlist (distinct from "saved for later")

---

## 2. Users & Scale

### User Types

| User Type | Cart Behavior | Notes |
|---|---|---|
| Anonymous Guest | Session cart (cookie-based, 30-day TTL) | No login required; cart lost if cookie expires |
| Authenticated Shopper | Persistent cart tied to user_id | Multi-device sync; never expires |
| Business / B2B Customer | Larger carts (100+ line items); bulk pricing | Same system; B2B pricing handled by Pricing Service |
| Internal Systems | Checkout Service reads cart; Fraud Service inspects cart contents | Read-only access via internal service tokens |

### Traffic Estimates

**Assumptions:**
- 300 M monthly active users on an Amazon-scale platform.
- At peak (Prime Day), 10% of MAU are concurrently active = 30 M concurrent users.
- Each active user performs ~5 cart operations/session (add, view, update, remove, view).
- Average session length: 20 minutes.
- Peak multiplier over daily average: 4×.

| Metric | Calculation | Result |
|---|---|---|
| Daily active users (normal) | 300 M MAU × 40% DAU ratio | ~120 M DAU |
| Daily cart operations | 120 M users × 5 ops/session | 600 M ops/day |
| Average cart RPS | 600 M / 86,400 s | ~6,944 RPS |
| Peak cart RPS (4× Prime Day) | 6,944 × 4 | ~27,778 RPS |
| Concurrent active carts (peak) | 30 M concurrent users | 30 M carts in hot path |
| Cart add/write RPS (50% of ops) | 27,778 × 0.5 | ~13,889 write RPS |
| Cart read RPS (50% of ops) | 27,778 × 0.5 | ~13,889 read RPS |
| Price recalculation RPS (triggered per read + checkout) | ~13,889 | ~13,889 calls to Pricing Service |
| Coupon validation RPS (5% of carts apply coupon) | 13,889 × 0.05 | ~694 RPS |
| Cart merge events (guest → login) | 120 M × 20% login rate × 30% have guest cart | ~7.2 M merges/day (~83/s avg) |

### Latency Requirements

| Operation | P50 Target | P99 Target | Rationale |
|---|---|---|---|
| GET cart (full view with totals) | 20 ms | 50 ms | Displayed prominently; user waits for totals |
| POST add item | 30 ms | 100 ms | Immediate feedback required; user sees item appear |
| PATCH update quantity | 20 ms | 80 ms | Inline edit; delay feels broken |
| DELETE remove item | 20 ms | 80 ms | Undo within 5 s; must be fast |
| POST apply coupon | 50 ms | 150 ms | Validates + recomputes totals; slightly more complex |
| POST merge cart (login event) | 100 ms | 300 ms | Background merge; user sees spinner during login |
| Inventory soft hold | 50 ms | 150 ms | Async acceptable; shown within 1–2 s |

### Storage Estimates

**Assumptions:**
- Average cart has 5 line items.
- Each line item: ~300 bytes (SKU, quantity, price_snapshot, add_time, variant metadata).
- Cart metadata: ~200 bytes (user_id/session_id, currency, timestamps, promo codes).
- Total per cart: ~1.7 KB (5 × 300 B + 200 B).
- Guest carts TTL 30 days → 30% of 120 M DAU = 36 M guest carts active.
- Authenticated carts active: 100 M (persistent).

| Entity | Record Size | Count | Total Storage |
|---|---|---|---|
| Active authenticated carts | 1.7 KB | 100 M | ~170 GB |
| Active guest carts (30-day TTL) | 1.7 KB | 36 M | ~61 GB |
| Cart event log (audit) | 500 B | 600 M ops/day × 365 days | ~110 TB/year |
| Coupon code records | 200 B | 10 M active codes | ~2 GB |
| Inventory soft holds | 100 B | 50 M active holds | ~5 GB |
| Saved-for-later items | 400 B | 100 M users × 3 items avg | ~120 GB |

**Total hot storage (Redis/DynamoDB):** ~240 GB for active carts.
**Total cold storage (event log):** ~110 TB/year (S3 cold tier, Parquet format for analytics).

### Bandwidth Estimates

| Traffic Type | Calculation | Result |
|---|---|---|
| GET cart responses | 13,889 RPS × 2 KB (cart payload) | ~27.8 MB/s |
| POST add item requests | 6,944 RPS × 500 B | ~3.5 MB/s |
| Price recalculation calls (internal) | 13,889 RPS × 200 B (request) | ~2.8 MB/s |
| Kafka cart events | 27,778 RPS × 500 B | ~13.9 MB/s |
| Total inbound (peak) | Sum | ~48 MB/s (~384 Mbps) |

---

## 3. High-Level Architecture

```
                   ┌────────────────────────────────────────────────┐
                   │               CLIENTS                          │
                   │  Web Browser · Mobile App · 3rd-party Client   │
                   └──────────────────────┬─────────────────────────┘
                                          │ HTTPS
                                          ▼
                   ┌────────────────────────────────────────────────┐
                   │     API Gateway / Load Balancer                │
                   │  - Auth token validation (JWT / session cookie) │
                   │  - Rate limiting (per IP, per user_id)         │
                   │  - Routes /cart/* to Cart Service              │
                   └────────────────┬───────────────────────────────┘
                                    │
                   ┌────────────────▼───────────────────────────────┐
                   │              Cart Service                      │
                   │  (Stateless; Go or Java; horizontally scaled)  │
                   │                                                │
                   │  ┌─────────────────────────────────────────┐   │
                   │  │  Cart Manager                           │   │
                   │  │  - CRUD operations on cart items        │   │
                   │  │  - Merge logic (guest → auth)           │   │
                   │  │  - Idempotency enforcement              │   │
                   │  └──────────────────┬──────────────────────┘   │
                   │                     │                          │
                   │  ┌──────────────────▼──────────────────────┐   │
                   │  │  Price Recalculator                     │   │
                   │  │  - Calls Pricing Service for each SKU   │   │
                   │  │  - Detects price changes since add-time │   │
                   │  │  - Applies coupon discounts             │   │
                   │  └──────────────────┬──────────────────────┘   │
                   │                     │                          │
                   │  ┌──────────────────▼──────────────────────┐   │
                   │  │  Inventory Advisor                      │   │
                   │  │  - Checks stock availability per SKU    │   │
                   │  │  - Places/releases soft holds           │   │
                   │  └─────────────────────────────────────────┘   │
                   └───┬──────────────┬────────────────┬────────────┘
                       │              │                │
          ┌────────────▼──┐  ┌────────▼─────┐  ┌──────▼───────────┐
          │  Cart Store    │  │ Coupon/Promo  │  │ Inventory Svc    │
          │  (DynamoDB +   │  │   Service     │  │ (Redis soft hold │
          │   Redis Cache) │  │  (Aurora)     │  │  + Aurora)       │
          └───────────────┘  └──────────────┘  └──────────────────┘
                  │
          ┌───────▼────────────────────────────────────────────────┐
          │              Kafka Event Bus                           │
          │  Topics: cart-events, inventory-hold-events            │
          └───┬──────────────────────────────────────────┬─────────┘
              │                                          │
   ┌──────────▼──────────┐                  ┌───────────▼──────────┐
   │  Cart Event Logger  │                  │  Checkout Service    │
   │  (S3 Parquet via    │                  │  (reads cart, locks  │
   │   Kinesis Firehose) │                  │   inventory, charges)│
   └─────────────────────┘                  └──────────────────────┘

Supporting Services:
  ┌───────────────────┐   ┌────────────────────┐   ┌──────────────────┐
  │  Pricing Service  │   │  Session Service   │   │  Auth Service    │
  │  (Redis + Aurora) │   │  (Redis, TTL 30d)  │   │  (JWT issuer)    │
  └───────────────────┘   └────────────────────┘   └──────────────────┘
```

**Component Roles:**

| Component | Role |
|---|---|
| API Gateway | Validates JWT / session cookie; extracts `user_id` or `session_id`; rate limits |
| Cart Service | Core business logic: CRUD, merge, price recalculation, coupon application |
| Cart Store (DynamoDB) | Durable persistence for cart data; strong consistency reads via `ConsistentRead=true` |
| Redis Cache (cart) | Read-through cache for cart reads; invalidated on every write; TTL = 1 hr |
| Coupon/Promo Service | Validates codes, checks eligibility, computes discounts, marks codes as used |
| Inventory Service | Manages soft holds (time-limited reservations); checks stock for "add to cart" |
| Kafka | Async event bus for audit logging, inventory hold events, analytics |
| Cart Event Logger | Consumes Kafka events; writes Parquet files to S3 for analytics and fraud review |
| Checkout Service | Downstream consumer of cart; reads cart at checkout time and acquires hard inventory locks |
| Session Service | Manages guest session tokens; links session_id → guest cart_id |

**Primary Data Flow — Authenticated User Adds Item:**

1. Client: `POST /v1/cart/items { sku: "B09XYZ", qty: 2 }` with `Authorization: Bearer <JWT>`.
2. API Gateway validates JWT, extracts `user_id = 12345`, forwards to Cart Service.
3. Cart Service checks idempotency: has `Idempotency-Key` been seen before? If yes, return cached response.
4. Cart Service reads current cart from Redis (cache hit) or DynamoDB (`ConsistentRead=true` on miss).
5. Cart Service calls Inventory Service: `GET /inventory/B09XYZ/availability` → confirms qty ≥ 2.
6. Cart Service calls Pricing Service: `GET /pricing/B09XYZ` → snapshots current price.
7. DynamoDB `UpdateItem` with condition expression: adds line item, increments quantity if SKU exists.
8. Redis cache for `cart:{user_id}` is invalidated (DEL).
9. Cart event published to Kafka topic `cart-events`.
10. Response returned: updated cart JSON.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- CART (DynamoDB primary store)
-- ============================================================
-- Table: carts
--   PK: cart_id  (String, UUID)  — partition key
--   Attributes:
--     owner_type:  String  — "user" | "guest"
--     owner_id:    String  — user_id (String) if user; session_id if guest
--     marketplace_id: Number
--     currency_code:  String  — "USD"
--     status:      String  — "active" | "checked_out" | "abandoned" | "merged"
--     created_at:  String  — ISO-8601
--     updated_at:  String  — ISO-8601
--     ttl:         Number  — Unix epoch; set for guest carts (30d), NULL for auth carts
--
-- GSI-1: owner_id-index
--   PK: owner_id  (String)
--   SK: status    (String)
--   Purpose: Look up "active cart for user_id X"
--
-- GSI-2: owner_type-status-index
--   PK: owner_type
--   SK: updated_at  (for abandoned cart analytics)

-- ============================================================
-- CART LINE ITEMS (DynamoDB, stored as a List attribute on the cart item
--                 OR as a separate table for very large carts)
-- ============================================================
-- Option A (embedded list, suitable for carts ≤ 40 items):
--   carts.items: List<Map>
--     [
--       {
--         line_item_id:     "uuid",
--         sku:              "B09XYZ-RED-M",
--         asin:             "B09XYZ",
--         quantity:         2,
--         price_snapshot_cents: 3999,   -- price at time of add
--         currency_code:    "USD",
--         seller_id:        12345,
--         variant_attrs:    {"color": "Red", "size": "M"},
--         added_at:         "2026-04-01T10:00:00Z",
--         saved_for_later:  false,
--         soft_hold_id:     "hold_uuid",   -- reference to inventory hold
--         soft_hold_expiry: "2026-04-01T10:15:00Z"
--       }
--     ]
--
-- Option B (separate table for B2B carts with 100+ items):
-- Table: cart_items
--   PK: cart_id   (partition key)
--   SK: line_item_id  (sort key)
--   (same attributes as above per item)

-- ============================================================
-- INVENTORY SOFT HOLDS (Redis + Aurora)
-- ============================================================
-- Redis (hot path for hold checks):
--   Key: hold:{sku}
--   Type: Sorted Set
--   Members: hold_id
--   Score: expiry_unix_timestamp
--   → Use ZREMRANGEBYSCORE hold:{sku} 0 {now} to clean expired holds
--   → ZADD hold:{sku} {expiry} {hold_id}
--   → Total holds = ZCARD hold:{sku}  (after cleanup)

-- Aurora (durable hold records):
CREATE TABLE inventory_holds (
    hold_id         UUID            NOT NULL DEFAULT gen_random_uuid(),
    sku             VARCHAR(50)     NOT NULL,
    cart_id         UUID            NOT NULL,
    user_id         BIGINT,                   -- NULL for guest holds
    quantity_held   INT             NOT NULL,
    hold_type       VARCHAR(20)     NOT NULL DEFAULT 'soft',  -- soft | hard (checkout)
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at      TIMESTAMPTZ     NOT NULL,
    released_at     TIMESTAMPTZ,              -- NULL = still active
    PRIMARY KEY (hold_id)
);

CREATE INDEX idx_holds_sku_active ON inventory_holds(sku, expires_at)
    WHERE released_at IS NULL;
CREATE INDEX idx_holds_cart ON inventory_holds(cart_id)
    WHERE released_at IS NULL;

-- ============================================================
-- COUPON / PROMO CODES (Aurora MySQL)
-- ============================================================
CREATE TABLE coupons (
    coupon_id           BIGINT          NOT NULL AUTO_INCREMENT,
    code                VARCHAR(50)     NOT NULL UNIQUE,
    description         VARCHAR(500),
    discount_type       ENUM('percentage', 'fixed_amount', 'free_shipping', 'bxgy')
                                        NOT NULL,
    discount_value      DECIMAL(10,4)   NOT NULL,   -- e.g., 20.00 for 20% or $20
    max_discount_cents  INT,                         -- Cap for percentage discounts
    min_order_cents     INT             NOT NULL DEFAULT 0,
    valid_from          TIMESTAMPTZ     NOT NULL,
    valid_to            TIMESTAMPTZ     NOT NULL,
    max_uses_total      INT,                         -- NULL = unlimited
    max_uses_per_user   INT             NOT NULL DEFAULT 1,
    usage_count         INT             NOT NULL DEFAULT 0,
    applicable_asins    JSON,                        -- NULL = all products
    applicable_categories JSON,
    is_active           BOOLEAN         NOT NULL DEFAULT TRUE,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (coupon_id),
    INDEX idx_code (code)
);

CREATE TABLE coupon_redemptions (
    redemption_id   BIGINT          NOT NULL AUTO_INCREMENT,
    coupon_id       BIGINT          NOT NULL,
    user_id         BIGINT          NOT NULL,
    cart_id         UUID            NOT NULL,
    order_id        BIGINT,                     -- populated at checkout
    discount_applied_cents INT      NOT NULL,
    redeemed_at     TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    status          ENUM('pending', 'confirmed', 'cancelled') NOT NULL DEFAULT 'pending',
    PRIMARY KEY (redemption_id),
    INDEX idx_coupon_user (coupon_id, user_id),
    FOREIGN KEY (coupon_id) REFERENCES coupons(coupon_id)
);

-- ============================================================
-- CART EVENTS (Kafka → S3 Parquet; schema for analytics)
-- ============================================================
-- Event schema (Apache Avro / JSON representation):
-- {
--   "event_id":       "uuid",
--   "event_type":     "item_added" | "item_removed" | "quantity_updated"
--                   | "coupon_applied" | "cart_merged" | "cart_checked_out"
--                   | "item_saved_for_later",
--   "cart_id":        "uuid",
--   "user_id":        "bigint or null",
--   "session_id":     "string or null",
--   "sku":            "string or null",
--   "quantity_delta": "int",        -- +2 for add, -1 for remove
--   "price_cents":    "int or null",
--   "coupon_code":    "string or null",
--   "marketplace_id": "smallint",
--   "timestamp":      "ISO-8601",
--   "client_ip_hash": "string"
-- }

-- ============================================================
-- GUEST SESSION → CART MAPPING (Redis)
-- ============================================================
-- Key: session:cart:{session_id}
-- Value: cart_id (String)
-- TTL: 30 days (rolling, refreshed on activity)

-- ============================================================
-- IDEMPOTENCY KEYS (Redis)
-- ============================================================
-- Key: idem:{idempotency_key}
-- Value: serialized response JSON
-- TTL: 24 hours
-- SET NX EX 86400 — set only if not exists
```

### Database Choice

| Component | Options Considered | Selected | Justification |
|---|---|---|---|
| Cart Storage (primary) | Redis (only), DynamoDB, Cassandra, PostgreSQL | **DynamoDB** | Single-digit ms reads at any scale; `ConsistentRead=true` for read-your-writes; conditional writes for optimistic locking; TTL for guest carts; Global Tables for multi-region active-active; JSON-like attribute model fits flexible cart schema |
| Cart Cache (read path) | No cache, Redis, Memcached | **Redis** | Avoids DynamoDB read capacity costs on hot carts; sub-millisecond reads; simple `GET`/`SET`/`DEL` pattern; TTL auto-expiry prevents stale data issues |
| Inventory Holds (hot) | Redis, DynamoDB, Postgres | **Redis (Sorted Set) + Aurora (durable)** | Redis Sorted Set enables O(log N) hold expiry via `ZREMRANGEBYSCORE`; Aurora provides durable record for audit and reconciliation; Redis TTL auto-expires uncleared holds |
| Coupon / Promo | DynamoDB, MySQL/Aurora, Redis | **Aurora MySQL** | Coupons require transactional ACID writes (increment usage_count atomically, check max_uses); relational model fits coupon eligibility rules; ~10 M codes fits comfortably in Aurora; read-through Redis cache for frequent code lookups |
| Cart Event Audit Log | PostgreSQL, Cassandra, Kafka+S3 | **Kafka + S3 (Parquet via Kinesis Firehose)** | Events are append-only; no CRUD needed; S3 Parquet is queryable via Athena for analytics; Kinesis Firehose buffers and batch-writes efficiently; zero schema migration cost as event schema evolves |
| Session Mapping (guest) | Redis, DynamoDB | **Redis** | Session lookups happen on every guest cart request; must be sub-millisecond; 30-day TTL maps perfectly to Redis EXPIRE; small data (36 M sessions × 100 B = 3.6 GB, easily fits in Redis) |

---

## 5. API Design

All endpoints:
- **Auth**: Authenticated endpoints require `Authorization: Bearer <JWT>`. Guest endpoints use `X-Session-Token: <token>` header or `cart_session` cookie.
- **Rate Limits**: Per user_id or per IP for guests.
- **Idempotency**: All mutating (POST/PUT/PATCH/DELETE) endpoints require `Idempotency-Key: <uuid>` header.

```
# ─────────────────────────────────────────────────────────────
# 1. GET Current Cart
# ─────────────────────────────────────────────────────────────
GET /v1/cart
  ?recalculate_prices=true   (default: true; set false for speed if price freshness not needed)
  &marketplace_id=1

Response 200:
{
  "cart_id": "uuid",
  "owner_type": "user",
  "currency": "USD",
  "items": [
    {
      "line_item_id": "uuid",
      "asin": "B09XYZ",
      "sku": "B09XYZ-RED-M",
      "title": "Widget Pro Red Medium",
      "image_url": "https://cdn.example.com/img.jpg",
      "quantity": 2,
      "unit_price_cents": 3999,           // current price (recalculated)
      "price_snapshot_cents": 4299,       // price when item was added
      "price_changed": true,              // flag for UI notice
      "inventory_status": "in_stock",
      "soft_hold_expiry": "2026-04-09T10:15:00Z",
      "saved_for_later": false,
      "variant_attrs": {"color": "Red", "size": "M"}
    }
  ],
  "saved_for_later": [...],
  "pricing_summary": {
    "subtotal_cents": 7998,
    "discount_cents": 800,
    "coupon_code": "SAVE10",
    "coupon_discount_cents": 800,
    "shipping_estimate_cents": 0,         // free shipping (Prime)
    "tax_estimate_cents": 659,
    "grand_total_cents": 7857
  },
  "price_warnings": [
    { "sku": "B09XYZ-RED-M", "old_price_cents": 4299, "new_price_cents": 3999 }
  ],
  "updated_at": "2026-04-09T10:00:00Z"
}

Auth: Required (user) or X-Session-Token (guest)
Rate Limit: 300 req/min per user_id; 60 req/min per IP (guest)
Cache: Redis TTL 60 s (invalidated on any write); stale-while-revalidate pattern

# ─────────────────────────────────────────────────────────────
# 2. POST Add Item to Cart
# ─────────────────────────────────────────────────────────────
POST /v1/cart/items
Headers: Idempotency-Key: <uuid>

Request Body:
{
  "sku": "B09XYZ-RED-M",
  "asin": "B09XYZ",
  "quantity": 2,
  "seller_id": 12345
}

Response 201:
{
  "line_item_id": "uuid",
  "sku": "B09XYZ-RED-M",
  "quantity": 2,
  "unit_price_cents": 3999,
  "soft_hold_id": "hold-uuid",
  "soft_hold_expiry": "2026-04-09T10:15:00Z",
  "cart_summary": { ... }   // same as GET /cart pricing_summary
}

Response 409 (idempotency replay):
{
  "message": "Duplicate request; returning original response.",
  "original_response": { ... }
}

Response 422 (out of stock):
{
  "error": "INSUFFICIENT_INVENTORY",
  "available_quantity": 1
}

Auth: Required or guest session
Rate Limit: 60 adds/min per user_id; 20/min per IP (guest)
Idempotency: Required; 24-hr key retention in Redis

# ─────────────────────────────────────────────────────────────
# 3. PATCH Update Item Quantity
# ─────────────────────────────────────────────────────────────
PATCH /v1/cart/items/{line_item_id}
Headers: Idempotency-Key: <uuid>

Request Body:
{
  "quantity": 3    // new absolute quantity (not delta)
}

Response 200:
{
  "line_item_id": "uuid",
  "quantity": 3,
  "unit_price_cents": 3999,
  "soft_hold_expiry": "2026-04-09T10:15:00Z"
}

Response 422: { "error": "INSUFFICIENT_INVENTORY", "available_quantity": 2 }

Auth: Required or guest session (ownership validated)
Rate Limit: 120 updates/min per user_id

# ─────────────────────────────────────────────────────────────
# 4. DELETE Remove Item
# ─────────────────────────────────────────────────────────────
DELETE /v1/cart/items/{line_item_id}
Headers: Idempotency-Key: <uuid>

Response 200:
{
  "removed_line_item_id": "uuid",
  "cart_summary": { ... }
}

Response 404: { "error": "LINE_ITEM_NOT_FOUND" }

Note: Soft hold is released asynchronously via Kafka event.
Auth: Required or guest session
Rate Limit: 120 deletes/min per user_id

# ─────────────────────────────────────────────────────────────
# 5. POST Apply Coupon Code
# ─────────────────────────────────────────────────────────────
POST /v1/cart/coupon
Headers: Idempotency-Key: <uuid>

Request Body:
{
  "coupon_code": "SAVE10"
}

Response 200:
{
  "coupon_code": "SAVE10",
  "discount_type": "percentage",
  "discount_value": 10.0,
  "discount_applied_cents": 800,
  "pricing_summary": { ... }
}

Response 422 (invalid code):
{
  "error": "COUPON_INVALID",
  "reason": "expired"  // expired | already_used | minimum_not_met | not_applicable
}

Auth: Required (coupon redemption tracking requires user_id)
Rate Limit: 10 coupon attempts/min per user_id (brute-force protection)

# ─────────────────────────────────────────────────────────────
# 6. DELETE Remove Coupon Code
# ─────────────────────────────────────────────────────────────
DELETE /v1/cart/coupon
Response 200: { "pricing_summary": { ... } }

# ─────────────────────────────────────────────────────────────
# 7. POST Merge Guest Cart on Login
# ─────────────────────────────────────────────────────────────
POST /v1/cart/merge
Headers: Idempotency-Key: <uuid>

Request Body:
{
  "guest_session_token": "sess_abc123"
}

Response 200:
{
  "merged_item_count": 3,
  "conflict_resolutions": [
    {
      "sku": "B09XYZ-RED-M",
      "guest_qty": 2,
      "existing_qty": 1,
      "merged_qty": 3,
      "capped_at_inventory": false
    }
  ],
  "cart_summary": { ... }
}

Auth: Required (JWT; user_id from token)
Rate Limit: 5 merge requests/hour per user_id

# ─────────────────────────────────────────────────────────────
# 8. POST Save Item for Later
# ─────────────────────────────────────────────────────────────
POST /v1/cart/items/{line_item_id}/save-for-later
Response 200: { "line_item_id": "uuid", "saved_for_later": true }
Note: Soft hold released immediately.

# ─────────────────────────────────────────────────────────────
# 9. POST Move Saved Item Back to Cart
# ─────────────────────────────────────────────────────────────
POST /v1/cart/saved-items/{line_item_id}/move-to-cart
Response 200: { "line_item_id": "uuid", "saved_for_later": false, "cart_summary": {...} }

# ─────────────────────────────────────────────────────────────
# 10. GET Cart (Internal — used by Checkout Service)
# ─────────────────────────────────────────────────────────────
GET /internal/v1/cart/{cart_id}?lock=true

Request: Internal service token (not user JWT)
Response 200: Full cart payload + locked=true status
Note: lock=true acquires a distributed lock (Redis SETNX) preventing
      concurrent cart modifications during checkout. TTL: 10 minutes.

Auth: Internal service-to-service mTLS
Rate Limit: No limit (internal)
```

---

## 6. Deep Dive: Core Components

### 6.1 Cart Merge on Login

**Problem it solves:**
When a guest user (who has a cart tied to their session_id) authenticates, the system must merge their guest cart with their existing authenticated cart. This is a write-heavy, conflict-prone operation that must be: (a) idempotent (repeated login events should not add items twice), (b) correct under concurrent logins from multiple devices, and (c) user-friendly in conflict resolution.

**Approach Comparison:**

| Approach | Idempotency | Conflict Handling | Complexity | Notes |
|---|---|---|---|---|
| Last-write-wins (overwrite auth cart with guest cart) | Yes (idempotent) | Poor (loses auth cart items) | Low | Unacceptable: user loses items from existing cart |
| Append guest items to auth cart, ignore duplicates | Partial | Ignores qty conflicts | Low | Leads to wrong quantities; e.g., 2+1 stays 1 |
| Sum quantities for overlapping SKUs, cap at inventory | Yes (idempotent with dedup key) | Good (quantity summing) | Medium | **Selected** |
| Prompt user to choose per conflict | N/A | Excellent | High | Good UX for small conflicts; impractical for 20+ items |
| Prefer higher-quantity item per SKU | Yes | Good | Medium | Simpler but may undercount (user adds 2 on mobile, 1 on desktop; merge keeps 2 not 3) |

**Selected Approach — Idempotent Quantity Sum with Inventory Cap:**

```
Cart Merge Algorithm:

Input:
  guest_cart: { items: [{sku: A, qty: 2}, {sku: B, qty: 1}] }
  auth_cart:  { items: [{sku: A, qty: 1}, {sku: C, qty: 3}] }

Step 1: Check idempotency
  Redis SETNX merge:done:{user_id}:{guest_session_id} "1" EX 86400
  → If key already exists: return cached merge result (no-op)

Step 2: Acquire distributed lock on auth cart
  Redis SET lock:cart:{user_id} {merge_request_id} NX EX 30
  → If lock not acquired within 5 s: 503 Service Unavailable (retry)

Step 3: Build merged item map
  merged = {}
  for item in auth_cart.items:
    merged[item.sku] = item.qty
  for item in guest_cart.items:
    merged[item.sku] = merged.get(item.sku, 0) + item.qty  // sum quantities

  Result: {A: 3, B: 1, C: 3}

Step 4: For each merged SKU with summed qty > 1, check inventory
  For SKU A (qty: 3): Inventory says available = 2
    → Cap merged qty at 2
    → Record conflict: {sku: A, requested: 3, capped_at: 2}

Step 5: Write merged cart to DynamoDB
  DynamoDB TransactWriteItems:
    - Put new cart record (merged_cart_id = auth_cart_id, status="active")
    - Update each line item
    - Put: { PK: cart_id, status: "merged", merged_into: auth_cart_id }
      on guest_cart record (marks it as consumed)

Step 6: Release distributed lock
  Redis DEL lock:cart:{user_id}

Step 7: Invalidate Redis cache
  DEL cart:cache:{user_id}

Step 8: Release all guest soft holds (they'll be re-acquired on next cart view)
  Emit Kafka event: hold-release-events { cart_id: guest_cart_id }
```

**DynamoDB TransactWriteItems** is used in Step 5 to guarantee atomicity across multiple item updates. If any write fails (e.g., conditional check violation), the entire transaction rolls back — preventing partial merges.

**Concurrent Login Attack (same user logs in simultaneously from 2 devices):**
The Redis distributed lock (`SET NX EX 30`) ensures only one merge operation runs at a time per `user_id`. The second concurrent login finds the lock held and retries with exponential backoff (100 ms, 200 ms, 400 ms — max 3 attempts). After the first merge completes and the idempotency key is set, the second merge's idempotency check returns the cached result.

**Interviewer Q&As:**

Q1: What if the merge fails after writing to DynamoDB but before marking the guest cart as "merged"?
A: This creates a window where the guest cart could be merged again on retry. The idempotency key (Redis `SETNX merge:done:{user_id}:{session_id}`) prevents re-merge: it is set *before* the DynamoDB write (not after). If the write fails, the idempotency key still exists, so the retry will return the original response. However, the DynamoDB write may be partially committed — this is where `TransactWriteItems` helps: it's all-or-nothing. If the transaction itself fails, no partial state is written. The retry starts fresh.

Q2: How do you handle a guest cart with 100 items (B2B scenario) merging into an existing 80-item cart?
A: DynamoDB has a 25-item limit per `TransactWriteItems` call. For large carts, we batch the writes: split the 180 merged items into 8 batches of 25, write each batch sequentially with the guest cart's `merge_progress` attribute tracking which SKUs are done. If the operation fails mid-way, the next retry reads `merge_progress` and resumes from the last incomplete batch. The cart is not visible to the user until the merge is 100% complete (status remains "merging" during the process, and the UI shows a spinner).

Q3: What happens to the guest soft holds after a merge?
A: Guest soft holds are tied to `guest_cart_id`. After merge, they become orphaned (the guest cart is marked "merged"). The Inventory Service has a background job that sweeps expired/orphaned holds every 60 seconds (`ZREMRANGEBYSCORE hold:{sku} 0 {now}` in Redis, plus a DELETE on Aurora). Items in the merged auth cart will acquire new soft holds on the next cart view. We accept a 1-2 minute window where holds are inconsistent during the merge transition.

Q4: If a user abandons a cart, when are soft holds released?
A: Soft holds have a 15-minute TTL (set in Redis Sorted Set score and Aurora `expires_at`). They are automatically expired by the Redis TTL mechanism and the Aurora background sweep. We also proactively release holds when: (a) item is removed from cart, (b) cart is checked out, (c) cart is explicitly cleared, (d) the cart-activity heartbeat (sent every 5 min while cart page is open) stops — we detect stale carts via Kafka lag monitoring and release holds on carts inactive for >20 minutes.

Q5: How do you handle the case where the same user is browsing on mobile (guest) and logs in on desktop simultaneously?
A: The mobile guest session has its own `session_id` and `cart_id`. The desktop login triggers a merge of the mobile guest cart with the desktop auth cart. If the mobile session subsequently tries to add an item, it still writes to the guest cart (since the cookie still has the guest session token). However, the next `GET /v1/cart` on mobile (after session refresh detects the login state) will re-read the auth cart using the JWT. The guest cart is already marked "merged" and ignored. The mobile session transitions from guest to authenticated seamlessly.

---

### 6.2 Inventory Soft Hold — Preventing Oversell at Cart Stage

**Problem it solves:**
Without soft holds, two users can add the last unit to their carts simultaneously. Both see "In Stock," but only one can check out. The other gets an out-of-stock error at payment time — a terrible UX. Soft holds reserve inventory tentatively when an item is added to cart, reducing the visible available count so subsequent shoppers see accurate availability.

**Approach Comparison:**

| Approach | Oversell Prevention | Scalability | Complexity | UX Impact |
|---|---|---|---|---|
| No holds; check at checkout only | None (full oversell risk) | Trivially simple | Minimal | Frustrating: last-minute failures |
| DB row lock (SELECT FOR UPDATE) at add-to-cart | Strong | Poor (locks block reads; hotspot) | Low | Cart adds become serialized per SKU |
| Optimistic locking with version counter | Partial (retries needed) | Good | Medium | Client retries on conflict; slight UX friction |
| Redis atomic DECR on add; INCR on release | Strong (atomic) | Excellent | Medium | **Selected for display holds** |
| Distributed lease / Zookeeper | Strong | Medium | High | Operational overhead; overkill for soft holds |

**Selected Approach — Redis Atomic Decrement for Display Holds:**

```
Available inventory model:
  physical_stock = 100         (Aurora inventory table, source of truth)
  hard_reserved  = 5           (confirmed orders not yet shipped)
  soft_holds     = 12          (carts holding items, time-limited)
  display_available = physical_stock - hard_reserved - soft_holds = 83

On "Add to Cart" (qty = 2):
  Step 1: Lua script (atomic, runs in Redis single-threaded):
    local avail = tonumber(redis.call('GET', 'inv_avail:B09XYZ-RED-M'))
    if avail == nil then
      -- Cache miss: load from Aurora, write to Redis
      return -1  -- signal caller to load and retry
    end
    if avail < 2 then
      return 0   -- insufficient stock signal
    end
    redis.call('DECRBY', 'inv_avail:B09XYZ-RED-M', 2)
    redis.call('ZADD', 'holds:B09XYZ-RED-M', expiry_ts, hold_id)
    return 1   -- success signal

  Step 2: If Lua returns -1: load inventory from Aurora, prime Redis, retry once.
  Step 3: If Lua returns 0: return 422 INSUFFICIENT_INVENTORY to client.
  Step 4: If Lua returns 1:
    - Write hold record to Aurora (async, Kafka event)
    - Proceed with cart item write to DynamoDB

On Hold Expiry (background worker every 60 s):
  expired_holds = ZRANGEBYSCORE holds:{sku} 0 {now}
  for each hold_id in expired_holds:
    INCRBY inv_avail:{sku} {hold.quantity}   (restore availability)
    DELETE from Aurora inventory_holds WHERE hold_id = :hold_id
    ZREM holds:{sku} {hold_id}
```

**Why Lua scripting on Redis?**
Redis executes Lua scripts atomically — no other command runs between the `GET` and `DECRBY`. This eliminates the TOCTOU (time-of-check/time-of-use) race condition that would occur with separate GET and DECRBY commands.

**Aurora as ground truth:**
The Redis `inv_avail:{sku}` key is a cached projection of Aurora's `quantity_available`. If Redis is unavailable, the Cart Service falls back to Aurora with a `SELECT ... FOR UPDATE` (serializable isolation for the hold operation). This is slower (5–20 ms) but correct, and Redis is the common case.

**Interviewer Q&As:**

Q1: What if Redis crashes mid-operation after DECRBY but before the Lua script writes the hold to the sorted set?
A: The Lua script is atomic — Redis executes it as a single unit. If Redis crashes before the script completes, the entire script is rolled back on the next AOF/RDB recovery. There is no partial execution. However, if Redis crashes *after* the full Lua script runs but before the Aurora write completes, we have an inventory count decrease in Redis (hold counted) but no Aurora record. The nightly reconciliation job recomputes `display_available` from Aurora's physical stock minus confirmed holds, resetting Redis to the correct value. The window of inconsistency is at most 24 hours for this edge case (which affects display availability, not actual purchases — checkout always uses Aurora).

Q2: What is the risk of a hot SKU (like an iPhone launch) causing Redis key contention?
A: A single Redis key `inv_avail:B09XYZ` is a bottleneck for high-concurrency SKUs. Redis is single-threaded for command execution, handling ~1 M ops/sec per node. For a flash sale with 100,000 add-to-cart attempts/second for one SKU, this is a real bottleneck. Mitigation: **inventory sharding**. We split the SKU's inventory into N "buckets" (`inv_avail:B09XYZ:0` through `inv_avail:B09XYZ:N-1`), each holding 1/N of total stock. An add-to-cart request hashes to a random bucket. If a bucket is empty, try the next. This distributes key writes across N Redis nodes. We explore this further in the Flash Sale design.

Q3: How do you handle holds that fail to release when the Kafka event is dropped?
A: The hold TTL in Redis (15 minutes) acts as the primary safety net — expired entries are removed by `ZREMRANGEBYSCORE`. The Aurora holds table has `expires_at`; the background sweep (Aurora cron job every 5 minutes) releases expired holds and emits compensating `INCRBY` commands to Redis. Kafka events are delivered at-least-once (not relied on for correctness), only for async convenience. Correctness is guaranteed by TTL + Aurora reconciliation.

Q4: How do you size the soft hold TTL? 15 minutes seems short if a user is still shopping.
A: 15 minutes is the hold duration when the cart page is inactive. When the cart page is open, the frontend sends a "heartbeat" every 5 minutes (`POST /v1/cart/heartbeat`), which extends the hold TTL by another 15 minutes. This "rolling TTL" pattern means active shoppers always have a hold, but abandoned carts release inventory within 15 minutes of inactivity. The heartbeat approach also lets us detect when a user closes the tab (heartbeat stops) and release holds proactively after a short grace period.

Q5: For the checkout flow, when does the soft hold convert to a hard hold?
A: The Checkout Service calls `POST /internal/inventory/convert-hold` with the `hold_id`, transitioning `hold_type` from `soft` to `hard` in Aurora and removing the TTL (hard holds have no expiry — they persist until the order ships or is cancelled). This conversion is part of the checkout transaction: if payment fails, the hard hold is released. If payment succeeds, the hold converts to a "committed" status and eventually to a physical pick/pack instruction for the fulfillment center. The Cart Service soft hold is marked `released_at = NOW()` during this transition.

---

### 6.3 Price Recalculation and Coupon Application

**Problem it solves:**
Prices change between when an item is added and when the cart is viewed. Coupons add discount logic with complex eligibility rules (minimum order, applicable products, single-use). The cart must show the correct totals at all times without being prohibitively slow.

**Approach Comparison:**

| Approach | Freshness | Latency | Complexity | Notes |
|---|---|---|---|---|
| Recalculate on every GET /cart request (live) | Real-time | High (N price lookups per view) | Medium | Too slow for large carts; N=20 × 5 ms = 100 ms added |
| Snapshot price at add time, show warning on checkout only | Stale | Very low | Low | Poor UX; user sees stale prices until checkout |
| Recalculate periodically (every 5 min via background job) | 5 min lag | Low (pre-computed) | Medium | User sees stale price between background runs |
| Lazy recalculation: recalculate on view, cache result 60 s | Near-real-time | Moderate (first view) | Medium | **Selected** |
| Streaming price events to update cart totals | Real-time push | Very low (push) | High | WebSockets/SSE; good for high-value items; overkill for all carts |

**Selected Approach — Lazy Recalculation with Redis Cache:**

```
GET /v1/cart request flow:

1. Read cart from DynamoDB (or Redis cache if available).
2. For each line item, fan out in parallel to Pricing Service:
   prices = await Promise.all(
     items.map(item => pricingService.getPrice(item.sku, item.seller_id))
   )
   (Redis cached prices; P99 ~5 ms for up to 20 items = negligible fan-out time)

3. For each item:
   if current_price != item.price_snapshot:
     item.current_price = current_price
     item.price_changed = true

4. Compute subtotal:
   subtotal = sum(item.current_price * item.quantity for all items)

5. Apply coupon (if cart.coupon_code is set):
   coupon = coupons[cart.coupon_code]   (Redis cache, TTL 5 min)
   Validate eligibility:
     - Is coupon active and not expired?
     - subtotal >= coupon.min_order_cents?
     - Is user within per-user usage limit?
       (SELECT count FROM coupon_redemptions WHERE coupon_id=X AND user_id=Y
        AND status IN ('pending','confirmed'))
     - Are all applicable ASINs in cart? (for product-specific coupons)
   
   Compute discount:
     if coupon.discount_type == 'percentage':
       discount = min(subtotal * coupon.discount_value/100,
                      coupon.max_discount_cents ?? INT_MAX)
     elif coupon.discount_type == 'fixed_amount':
       discount = coupon.discount_value_cents
     elif coupon.discount_type == 'free_shipping':
       shipping_discount = shipping_estimate_cents

6. Fetch tax estimate from Tax Service:
   tax = taxService.estimate(subtotal - discount, user_address, items)

7. Compute grand_total = subtotal - discount + shipping - shipping_discount + tax

8. Cache result in Redis:
   SET cart:calc:{cart_id} {json_result} EX 60
   (Invalidated by any write to the cart or a price change event)

9. Return response with price_warnings array.
```

**Coupon Idempotency and Race Conditions:**

A coupon with `max_uses_total = 1000` risks over-redemption if 1001 users simultaneously apply it. The fix uses an **optimistic lock with a counter check**:

```sql
UPDATE coupons
SET usage_count = usage_count + 1
WHERE coupon_id = :id
  AND usage_count < max_uses_total
  AND is_active = TRUE;

-- If rows_affected = 0: coupon exhausted or inactive → return 422
-- If rows_affected = 1: success → write coupon_redemptions record
```

This is a single `UPDATE` statement — atomic in Aurora MySQL. No separate SELECT needed. Per-user limit check uses the `coupon_redemptions` table with a READ COMMITTED check before the UPDATE (acceptable race window: if two requests for the same user arrive within the same millisecond, the unique index on `(coupon_id, user_id)` with status=pending prevents double-insertion).

**Interviewer Q&As:**

Q1: Price recalculation hits the Pricing Service on every cart view. How do you avoid overloading the Pricing Service?
A: Three layers: (a) The Pricing Service itself uses Redis as its primary read store (sub-ms per lookup). (b) The Cart Service uses a 60-second Redis cache for the fully computed cart result (`cart:calc:{cart_id}`), so only the first GET after each modification hits the Pricing Service. (c) The Pricing Service has its own per-caller rate limits; Cart Service is a trusted internal caller with a high limit. At 13,889 cart view RPS, and assuming 70% Redis hit rate on cart_calc cache, the Pricing Service sees only 4,167 fan-out calls/sec — well within its capacity.

Q2: What happens if a price drops significantly between cart add and checkout — do we charge the lower or higher price?
A: We always charge the **lower** price (the current price at checkout). This is both consumer-friendly and legally simpler (charging more than advertised is a legal risk). The checkout flow performs a final price re-read against Aurora (not Redis), so the price charged is always authoritative. If the price increased since the user last viewed the cart, the checkout service blocks progression and shows a "Price updated" confirmation screen, requiring the user to acknowledge before proceeding.

Q3: How do you handle coupon codes for bulk discounts (e.g., "Buy 3, Get 1 Free")?
A: The `discount_type = 'bxgy'` (Buy X Get Y) uses a more complex evaluation function. The coupon record includes `bxgy_config: {"buy_qty": 3, "get_qty": 1, "applicable_skus": [...]}`. The coupon evaluator: (a) counts qualifying items in the cart, (b) computes how many "get" units are free (floor(buy_count / buy_qty) * get_qty), (c) applies the discount as the cost of the free units (cheapest qualifying items). This logic is handled in the Cart Service's `CouponEvaluator` class, unit-tested exhaustively with edge cases (partial sets, mixed products).

Q4: How do you prevent coupon code brute-forcing (trying millions of random codes)?
A: Three mitigations: (a) **Rate limiting**: 10 coupon attempts per minute per user_id, 5 per minute per IP. (b) **Code format**: Coupons use 12-character alphanumeric codes (62^12 ≈ 3.2 × 10^21 combinations — not brute-forceable). (c) **CAPTCHA**: After 5 failed attempts per session, a CAPTCHA challenge is required. (d) **Monitoring**: Prometheus alert fires if coupon validation failure rate exceeds 500/minute (indicates scraping/brute-force attack). IPs exceeding 50 failures/minute are blocked at the WAF.

Q5: How does price recalculation work for "subscribe and save" or tiered pricing?
A: Subscribe-and-save pricing (e.g., 15% off for subscription) is a property of the offer, not the coupon system. The Pricing Service returns the effective price based on the user's subscription status (from the Auth/Subscription Service). The Cart Service passes `user_id` with each price lookup, and the Pricing Service's price resolution logic checks: (1) is the user a subscriber? (2) does this SKU have a subscription discount? and returns the already-discounted price. This keeps the Cart Service decoupled from subscription logic — it just consumes prices.

---

## 7. Scaling

### Horizontal Scaling

- **Cart Service**: Stateless pods behind a K8s HPA. Scale on CPU (target 60%) and latency (P99 > 80 ms triggers scale-out). Each pod: 4 vCPU, 8 GB RAM, ~500 RPS capacity. At peak 27,778 RPS: 56 pods needed (comfortable at 60 pods with headroom).
- **Coupon Service**: Stateless, scales independently. Low RPS (~694/s) means 5–10 pods is sufficient.
- **Inventory Hold Worker**: Kafka consumer group; scales up to partition count (32 partitions). 2 pods in normal operation; autoscales to 32 based on consumer lag.

### DB Sharding

- **DynamoDB (Carts)**: Natively sharded by DynamoDB. Partition key = `cart_id` (UUID, high cardinality, even distribution). Monitor for hot partitions via CloudWatch `ConsumedWriteCapacityUnits`. Use DynamoDB on-demand capacity mode for auto-scaling (no manual capacity planning).
- **Aurora (Coupons)**: No sharding needed at 10 M coupons. If coupon volume grows 100×, shard by `coupon_id % N` using Vitess or Aurora global database. For now, Aurora with 3 read replicas handles the load.
- **Redis (Cart Cache + Holds)**: Redis Cluster with 6 shards (3 primary + 3 replica). Shard by key hash. Estimated hot data: 240 GB (active carts) + 5 GB (soft holds) = ~245 GB → 6 × 64 GB nodes = 384 GB capacity (57% utilization — healthy).

### Replication

- **DynamoDB Global Tables**: Active-active in 3 regions (us-east-1, eu-west-1, ap-southeast-1). Last-writer-wins conflict resolution (timestamp-based). RPO = seconds. RTO = 0 (active-active).
- **Aurora (Coupons)**: Multi-AZ synchronous primary + 2 read replicas per region. Cross-region async replica for DR (RPO <1 s).
- **Redis (Cart Cache)**: Primary + 1 replica per shard. Redis Sentinel monitors and promotes replicas within 15 s on failure.

### Caching

| Layer | Technology | TTL | What's Cached |
|---|---|---|---|
| L1 (in-process) | Ristretto LRU (32 MB/pod) | 500 ms | Coupon records for active campaign codes (top 1000 codes) |
| L2 (distributed) | Redis Cluster | 60 s | Computed cart totals (`cart:calc:{cart_id}`) |
| L2 (distributed) | Redis Cluster | 30 s | Current prices per SKU (read-through from Pricing Service) |
| L2 (distributed) | Redis Cluster | 5 min | Coupon code records (read-through from Aurora) |
| DB read replicas | Aurora read replicas | N/A | Coupon validation queries, coupon_redemptions counts |

### CDN

Cart endpoints are not cached at CDN (user-specific, dynamic). However, the Cart UI (JavaScript bundle, CSS) is CDN-cached with content-hash URLs and 1-year TTL. The cart page HTML shell (without data) can be SSR-cached for 30 seconds at CDN and hydrated client-side.

### Interviewer Q&As

Q1: DynamoDB's `TransactWriteItems` has a 25-item limit. How do you handle B2B carts with 200 line items at checkout?
A: We split the cart into batches of 25 items and write them sequentially with idempotency markers. The cart record includes a `write_version` counter; each batch includes a condition check `write_version == expected_version` to detect concurrent modifications mid-batch. If a concurrent modification is detected, we abort and retry the entire merge. For checkout specifically, we acquire a distributed lock on the cart before the multi-batch write, preventing concurrent modifications entirely.

Q2: How do you handle cart data consistency if the DynamoDB write succeeds but the Redis cache invalidation fails?
A: Redis is a cache, not the source of truth. A failed `DEL cart:cache:{user_id}` in Redis means the next GET returns stale data for up to 60 seconds (the TTL). This is acceptable for the cart (price might be 60 s stale, which is already accepted). The user can manually refresh. In practice, we use fire-and-forget invalidation with a retry (3 attempts, 100 ms apart); failure rate of all 3 is negligible. For critical operations (checkout), the Checkout Service calls `GET /cart` with `recalculate=true&bypass_cache=true`, skipping Redis entirely and reading from DynamoDB with `ConsistentRead=true`.

Q3: How does the Cart Service maintain read-your-writes consistency for a user who adds an item and immediately views the cart?
A: We use DynamoDB's `ConsistentRead=true` for the GET that immediately follows a write within the same request context. The Cart Service tracks a per-user `last_write_sequence` in the JWT or session context. If `last_write_sequence > 0` (i.e., the user just performed a write), `ConsistentRead=true` is set on the next read. After 2 seconds (typical DynamoDB propagation window), subsequent reads use `ConsistentRead=false` (eventual) to save RCU cost. This "conditional strongly-consistent read" pattern gives the user immediate feedback after their write while saving cost on subsequent reads.

Q4: How do you handle cart scalability for a Black Friday event with 10× normal traffic?
A: Pre-event preparation: (a) Pre-warm DynamoDB by running a load simulation 24 hours before; (b) scale Redis Cluster to 12 shards (double); (c) increase Cart Service pod count to 300 (from 60) via K8s `kubectl scale`; (d) notify DynamoDB of expected capacity (on-demand mode auto-adjusts, but warm partitions help); (e) enable a "cart writes to both DynamoDB and a hot Redis write-through" mode for extremely popular SKUs to reduce DynamoDB write pressure. During the event: Kafka consumer lag monitoring triggers auto-scaling for hold workers within 2 minutes of lag spike.

Q5: If the Cart Service is a monolith today, how would you decompose it?
A: Extract in this priority order: (1) **Inventory Hold Service** first — the hold/release lifecycle is cohesive and independently scalable (high write throughput); (2) **Price Recalculation Service** — pure function, easily extracted, enables caching improvements; (3) **Coupon/Promo Service** — already nearly independent (own DB); (4) **Guest Session Service** — isolated concern. The Cart core (CRUD) stays as the last piece, communicating with extracted services via gRPC for internal calls. Each extraction is done using the Strangler Fig pattern: new endpoint handles extracted function, old monolith delegates to new service, once stable old code is removed.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | Recovery |
|---|---|---|---|---|
| DynamoDB partition unavailable | Cart reads/writes fail for affected carts | DynamoDB CloudWatch `SystemErrors`, application errors | DynamoDB Global Tables active-active; requests route to another region | Automatic; DynamoDB SLA 99.999% |
| Redis Cluster node failure | L2 cache miss; increased DynamoDB load | Redis Sentinel health checks; CloudWatch | Automatic replica promotion (15 s); fallback to DynamoDB reads | Auto-recover; monitor DynamoDB RCU spike |
| Pricing Service unavailable during cart view | Grand total shows last-cached price | Circuit breaker; error rate monitoring | Show last snapshotted price with "prices may not be current" warning; block checkout until Pricing recovers | Alert; degrade gracefully |
| Coupon Service unavailable | Cannot apply or validate coupon | Error rate > 1% alert | Cache last-known coupon state in Redis (5 min TTL); return 503 for coupon endpoints only | Cart CRUD still works; coupon degraded |
| Inventory Service unavailable | Cannot check stock or place holds | Health probe failures | Allow cart add with no hold (display "availability not confirmed"); hard-check at checkout | Cart degrades to optimistic mode; oversell risk accepted for brief window |
| Kafka broker failure | Delayed audit log and hold-release events | Kafka broker health metrics | RF=3 Kafka; consumers reconnect; in-flight events held in durable log | No event loss; hold releases delayed by lag catch-up |
| Cart merge conflict (concurrent login) | Potential duplicate items in cart | Monitoring: merge collision counter | Distributed lock prevents concurrent merges; idempotency key prevents replay | Automatic via lock + idempotency |
| Aurora primary failure (Coupons DB) | Coupon write path fails | CloudWatch RDS events | Aurora Multi-AZ auto-failover (30 s); writes fail during 30 s; reads continue on replica | RTO ~30 s; brief coupon validation failure |
| Idempotency Redis store failure | Duplicate requests may be processed | Error rate spike on idempotency misses | Fall back to DB-level idempotency (`INSERT ... ON CONFLICT DO NOTHING`) | Slightly slower but correct |

### Retries and Idempotency

- **Client → Cart Service**: Clients must include `Idempotency-Key: <UUID-v4>`. The Cart Service stores the response for 24 hours in Redis (`SET NX EX 86400`). On duplicate key: return original response with HTTP 200 and header `X-Idempotent-Replay: true`.
- **Cart Service → DynamoDB**: AWS SDK automatic retry with jittered exponential backoff (3 retries, base 50 ms, max 500 ms). DynamoDB conditional writes (`ConditionExpression: attribute_exists(cart_id)`) prevent double-writes.
- **Cart Service → Pricing Service**: 2 retries with 50 ms, 150 ms backoff. Failure triggers fallback to snapshot price.
- **Cart Service → Inventory Service**: 2 retries for hold placement. Failure = proceed without hold (optimistic mode).

### Circuit Breaker

Same Hystrix-pattern configuration as the Product Page design. Per-service circuit breakers with 10 s sliding window, 50% failure rate threshold, 30 s open state, 5 probe half-open calls. Fallback for each downstream is documented in the Failure Scenarios table above.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Owner |
|---|---|---|---|
| Cart write RPS | Counter | >50,000 → capacity warning | Infra |
| Cart GET latency P99 | Histogram | >50 ms → Warning; >200 ms → Page | Cart Team |
| Cart write latency P99 | Histogram | >100 ms → Warning; >500 ms → Page | Cart Team |
| Cart add success rate | Gauge | <99% → Warning; <95% → Page | Cart Team |
| DynamoDB throttled requests | Counter | >0 → Warning; >100/min → Page | Infra |
| Redis cache hit ratio (cart) | Gauge | <75% → Warning (DynamoDB cost spike) | Infra |
| Inventory hold failure rate | Gauge | >5% → Warning | Inventory Team |
| Coupon validation failure rate | Counter | >10% of attempts → alert (brute force?) | Security |
| Cart merge success rate | Gauge | <99% → Warning | Cart Team |
| Abandoned cart rate | Gauge (business) | >80% → Product review | Product |
| Price change warnings per session | Gauge (business) | >30% sessions with warning → Pricing review | Pricing Team |
| Kafka consumer lag (cart-events) | Gauge | >5,000 events → Warning | Data Eng |

### Distributed Tracing

- OpenTelemetry SDK instruments all Cart Service code paths.
- Trace ID propagated via `traceparent` header through: API Gateway → Cart Service → DynamoDB → Redis → Pricing Service → Inventory Service → Coupon Service.
- Key spans: `cart.get`, `cart.add_item`, `cart.merge`, `cart.recalculate_prices`, `dynamo.update_item`, `redis.get`, `pricing.get_price`, `inventory.check_availability`, `inventory.place_hold`, `coupon.validate`.
- Sampling: 5% random in production; 100% for error-tagged traces; 100% for `X-Debug: true` header.

### Logging

- All cart mutations logged with: `cart_id`, `user_id` (hashed), `operation`, `sku`, `quantity_delta`, `trace_id`, `latency_ms`, `outcome` (success/failure), `failure_reason`.
- Error logs include full stack trace and relevant context (cart state snapshot at failure time).
- Cart event audit trail: Kafka → Kinesis Firehose → S3 Parquet (retained indefinitely for legal compliance; queryable via Athena).
- Guest cart session logs include hashed session_id; never raw session tokens.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Alternative Considered | Why Selected | Trade-off Accepted |
|---|---|---|---|
| DynamoDB as cart store | Redis-only, Cassandra, PostgreSQL | Managed, serverless scaling; TTL for guest carts; consistent reads; Global Tables for multi-region | Higher cost per operation than self-managed; limited query flexibility (no SQL joins) |
| Redis for soft hold tracking (Sorted Set) | Aurora row locks, DynamoDB | Atomic Lua scripts for TOCTOU safety; O(log N) expiry sweep; sub-ms latency | Redis-only hold state risks inconsistency on failure (mitigated by Aurora durable records + reconciliation) |
| Optimistic lock for coupon `usage_count` | Pessimistic lock (SELECT FOR UPDATE) | Avoids lock contention at high coupon application rates; MySQL `UPDATE WHERE count < max` is atomic | Rare race condition possible for the last available code (may over-issue by 1 in a tight race); accepted for non-monetary coupons |
| Lazy price recalculation (on-demand, cached 60 s) | Streaming price updates via WebSocket | Simpler implementation; no persistent connection needed; 60 s staleness acceptable | User may see 60 s stale price in cart view; price change always reflected before checkout |
| DynamoDB TransactWriteItems for cart merge | Manual saga with compensating transactions | Atomic multi-item write simplifies rollback logic; no partial merge state | 25-item limit requires batching for large carts; slightly higher latency (2× roundtrips for batched merge) |
| Kafka for cart event audit log | Synchronous DB write to audit table | Audit writes are fire-and-forget; Kafka provides durability + fan-out (analytics, fraud, ML training) | Brief delay (seconds) before events are durable in S3; Kafka operational complexity |
| Session-based guest cart (30-day TTL) | Cookie-only (client-side cart) | Server-side cart survives browser clearing, enables merge; consistent with server-authoritative inventory | Additional storage cost for 36 M guest carts; session cookie management complexity |

---

## 11. Follow-up Interview Questions

**Q1: How would you design the "save cart as a list" (shareable cart link) feature?**
A: Generate a `share_token` = `base64(HMAC-SHA256(cart_id + timestamp, secret_key))`. Store `(share_token → cart_id, snapshot_at)` in DynamoDB with a 7-day TTL. The share link encodes only the share_token, not the cart_id, to prevent enumeration. When a recipient visits the link, the system resolves the share_token to a cart_id, reads the cart snapshot, and creates a new cart for the recipient with those items (shallow copy). Items out of stock are flagged but included for transparency.

**Q2: How would you handle the scenario where a user's cart is corrupted (e.g., negative quantity due to a bug)?**
A: DynamoDB conditional writes prevent negative quantities: the `UpdateItem` condition `quantity + :delta >= 0` ensures quantity never goes below 0. For quantities that somehow become corrupt (e.g., legacy data), the Cart Service validates on every read: any item with `quantity <= 0` is automatically removed and a compensating Kafka event is emitted. A weekly data integrity job (Athena query on cart-events S3 data) scans for anomalies and triggers cart reconstructions from event replay.

**Q3: How do you handle tax calculation at the cart level across 50 US states with different tax rules?**
A: Tax calculation is delegated to an external Tax Service (e.g., Avalara, TaxJar, or an internal equivalent). The Cart Service calls the Tax Service asynchronously after computing the subtotal and coupon discount. Input: `{ items: [{sku, price_cents, qty, category_code}], shipping_address, shipping_cents }`. Output: `tax_cents` (total) and a breakdown by jurisdiction. The Tax Service response is cached per `(subtotal, address_postal_code, items_signature)` for 5 minutes (tax rates change infrequently). The Cart Service shows "Estimated Tax" — the final authoritative tax amount is calculated by the Order Service at checkout with the confirmed shipping address.

**Q4: How would you implement "notify me when back in stock" from the cart (when an item goes out of stock)?**
A: When an add-to-cart attempt fails with `INSUFFICIENT_INVENTORY`, the API returns an option to subscribe to a back-in-stock notification. The user's `POST /v1/dp/{asin}/stock-alerts` creates a record in DynamoDB: `stock_alerts:{sku}` → `[{user_id, created_at}]`. The Inventory Service, when stock is replenished (Kafka event from fulfillment center), triggers an "Alert Evaluator" consumer that reads all subscribed users for that SKU and enqueues push/email notifications via the Notification Service. Alerts are fire-and-forget (no guarantee the user will get the item — they still need to add to cart and check out).

**Q5: How do you design cart persistence for extremely long-lived carts (e.g., a user adds something and doesn't check out for 6 months)?**
A: Authenticated carts have no TTL in DynamoDB — they persist indefinitely. However, we run a monthly "staleness audit" job (Athena on cart-events data): carts with no activity for 90 days receive an email notification ("Your cart has items!"). After 180 days of inactivity, the cart is moved to "archived" status (still readable by the user, but soft holds are released and no more price recalculations). After 1 year, the cart is permanently deleted after sending a final "Your saved cart will expire" email. This balances user experience (long-lived intent) with storage costs.

**Q6: How would you implement "frequently bought together" suggestions within the cart?**
A: The cart page calls `GET /v1/recommendations/cart-suggestions?cart_id={id}`. The Recommendations Service reads the current cart's SKUs from DynamoDB and queries the pre-computed "frequently bought together" model (stored in Redis/DynamoDB, refreshed by daily ML batch jobs). It returns up to 5 suggested SKUs not already in the cart, ranked by co-purchase confidence score × margin (business objective). The cart UI displays these as a non-blocking widget loaded client-side after the main cart content renders.

**Q7: How do you handle currency conversion for multi-currency carts?**
A: Each marketplace has a fixed `currency_code`. A user in Germany uses EUR; a user in the UK uses GBP. Carts are marketplace-scoped (every cart has a `marketplace_id`). Cross-currency carts are not supported — if a user switches marketplace, a new cart is created for that marketplace. Prices stored in the cart are always in the marketplace's local currency. Exchange rate conversion is only relevant for analytics (reporting in a common currency) — handled by an offline batch job, not the cart runtime.

**Q8: How would you A/B test different cart merge strategies?**
A: The Cart Service reads the user's experiment assignment from a Feature Flag Service (e.g., LaunchDarkly). Experiment buckets (e.g., "merge_strategy=sum_quantities" vs. "merge_strategy=prefer_guest") are assigned at login time and stored in the JWT claim. The Cart Merge component branches on this claim. Business metrics (conversion rate from merge event to checkout completion) are logged with experiment_id on the Kafka cart-events topic and analyzed in the analytics pipeline. The flag can be dynamically adjusted without code deployment.

**Q9: How do you prevent cart theft (a malicious user modifying another user's cart via IDOR)?**
A: Every cart operation validates ownership: `cart.owner_id == JWT.user_id` (or `cart.owner_id == session_token` for guest). This check is enforced at the Cart Service layer, not the API Gateway — defense in depth. DynamoDB condition expressions include `owner_id = :caller_id` so even a compromised Cart Service pod cannot write to another user's cart. Cart IDs are UUIDs (not sequential), making guessing infeasible. All failed ownership checks are logged and monitored; >10 failures/minute from a single IP triggers a WAF block.

**Q10: How would you handle cart persistence across device changes (e.g., phone stolen, new phone)?**
A: Authenticated carts are tied to `user_id`, not `device_id`. When the user logs in on their new phone, they immediately see their existing cart — no data loss. The old device's JWT expires naturally (JWT expiry of 24 hours or revocation via token blacklist in Redis). Guest carts are tied to the device's cookie — if the device is lost, the guest cart is also lost. This is a known UX tradeoff; the recommendation is to "sign in to save your cart," which we surface prominently in the cart UI for guest users.

**Q11: How do you handle the price recalculation when a SKU is discontinued (deleted from catalog)?**
A: When a SKU is removed from the catalog, the Catalog Service emits a `sku-discontinued` Kafka event. A Cart Service consumer listens and marks affected cart line items with `status: discontinued`. On the next GET /cart, discontinued items are shown with an "Item no longer available" badge and excluded from pricing calculations. The user can remove these items; they cannot be checked out. Soft holds for discontinued items are immediately released.

**Q12: What is your disaster recovery strategy if the primary DynamoDB region goes down?**
A: DynamoDB Global Tables provides active-active replication to 3 regions. Route 53 health checks detect the primary region failure and route traffic to the next closest region within 60 seconds. Because Global Tables are active-active (not active-passive), the secondary region's DynamoDB is already accepting writes and has near-zero replication lag. RPO is effectively zero for cart data. The brief 60-second failover window means some in-flight cart operations fail — clients retry with exponential backoff and succeed against the secondary region.

**Q13: How would you implement cart-level fraud detection?**
A: A Fraud Detection Service consumes `cart-events` from Kafka in near-real-time. It applies rules and ML models: (a) Rule-based: unusually large quantities (>20 of a single SKU), high-value items added rapidly, known fraud patterns (specific ASIN combos popular for resale). (b) ML model: anomaly score based on user's cart history, device fingerprint, IP geolocation. If fraud score > threshold, the cart add is flagged (not blocked immediately) and a review queue entry is created. For high-confidence fraud (score > 0.95), the cart operation is blocked and the user is prompted for additional verification (2FA or phone number confirmation).

**Q14: How do you handle the scenario where a seller goes out of business and their items are in users' carts?**
A: When a seller account is suspended or terminated, the Seller Service emits a `seller-suspended` Kafka event. An inventory event marks all of that seller's SKUs as `seller_unavailable`. The Cart Service consumer processes this event and marks affected line items with `status: seller_unavailable` (similar to discontinued handling). Users see "Seller no longer available" and are prompted to find alternative sellers for the same ASIN via the "Other Sellers" link (which routes to the PDP's buy-box alternatives).

**Q15: What consistency guarantees does DynamoDB Global Tables provide for concurrent writes to the same cart from two regions?**
A: DynamoDB Global Tables uses a last-writer-wins (LWW) conflict resolution based on wall-clock timestamp. If two regions write to the same cart item within the same millisecond, the write with the higher timestamp wins. For most cart operations, this is acceptable — the probability of true simultaneity across regions is very low. For merge operations (which write multiple items atomically), we use `TransactWriteItems` which is a single-region transaction — the transaction executes in one region and replicates to others. To prevent cross-region merge conflicts, the merge operation acquires a distributed lock stored in a single-region DynamoDB table designated as the "lock region" for that user_id (determined by consistent hashing on user_id).

---

## 12. References & Further Reading

1. **Amazon DynamoDB: A Scalable, Predictably Performant, and Fully Managed NoSQL Database Service (2022)** — USENIX ATC '22. Covers DynamoDB Global Tables, conditional writes, and transaction semantics directly applicable to cart storage. https://www.usenix.org/conference/atc22/presentation/vig

2. **Dynamo: Amazon's Highly Available Key-Value Store (2007)** — DeCandia et al., SOSP 2007. The original paper describing eventual consistency, vector clocks, and the key-value model behind DynamoDB. https://dl.acm.org/doi/10.1145/1294261.1294281

3. **Redis Documentation — Transactions and Lua Scripting** — Covers the atomicity of Redis EVAL (Lua scripts) used for the inventory soft hold counter. https://redis.io/docs/manual/programmability/eval-intro/

4. **Redis Sorted Sets** — Documentation for `ZADD`, `ZRANGEBYSCORE`, `ZREMRANGEBYSCORE` used in the soft hold expiry mechanism. https://redis.io/docs/data-types/sorted-sets/

5. **Martin Kleppmann, "Designing Data-Intensive Applications" (2017)** — O'Reilly. Chapters on transactions (Chapter 7), linearizability vs. eventual consistency, and distributed locks are foundational to the cart merge and hold design.

6. **The Chubby Lock Service for Loosely-Coupled Distributed Systems (2006)** — Burrows, OSDI 2006. Theoretical foundation for the distributed lock used in cart merge. https://dl.acm.org/doi/10.5555/1298455.1298487

7. **Idempotency Keys — Stripe Engineering Blog** — Practical guide to implementing idempotency in payment and e-commerce APIs. https://stripe.com/blog/idempotency

8. **AWS Aurora: Aurora MySQL vs. Aurora PostgreSQL** — Amazon documentation covering ACID guarantees, Multi-AZ, and read replica behavior used for the Coupons DB. https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/

9. **Avalara Tax Compliance API Documentation** — Reference for tax calculation service integration. https://developer.avalara.com/api-reference/

10. **OpenTelemetry Specification — Trace Context Propagation (W3C)** — The distributed tracing standard used across Cart Service and its downstream calls. https://www.w3.org/TR/trace-context/

11. **Netflix Tech Blog — "Tuning the Circuit Breaker"** — Engineering details on Hystrix circuit breaker tuning for microservices, applicable to Cart Service downstream calls. https://netflixtechblog.com/making-the-netflix-api-more-resilient-a8ec62159c2d

12. **Apache Kafka Documentation — Consumer Groups and Offsets** — Describes the at-least-once delivery and offset management used for cart-events and hold-release-events topics. https://kafka.apache.org/documentation/#consumerconfigs

13. **AWS Kinesis Data Firehose — Delivering to S3** — Used in the cart audit log pipeline for buffered Parquet delivery. https://docs.aws.amazon.com/firehose/latest/dev/basic-deliver.html

14. **Amazon DynamoDB Developer Guide — Transactions** — Documents `TransactWriteItems` (25-item limit, atomicity guarantees) used for cart merge. https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/transaction-apis.html

15. **Google SRE Book — Chapter 12: Effective Alerting** — Framework for the monitoring and alerting thresholds defined in Section 9. https://sre.google/sre-book/monitoring-distributed-systems/
