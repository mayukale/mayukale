# System Design: DoorDash — Three-Sided Food Delivery Marketplace

---

## 1. Requirement Clarifications

### Functional Requirements

**Consumer side**
- Browse restaurants by location, cuisine, rating, and delivery time estimate
- View menus and add items to a cart
- Place orders with payment (card, DoorDash Pay, Apple Pay, Google Pay)
- Receive real-time order status updates and live Dasher location tracking
- Rate orders and leave reviews

**Merchant (restaurant) side**
- Receive incoming orders on a tablet/POS integration
- Accept, reject, or mark orders as "ready for pickup"
- Manage menu availability (item 86'd, restaurant pause)
- View sales dashboard and payout summaries

**Dasher (driver) side**
- Go online/offline to accept delivery opportunities
- Receive dispatch offers with estimated pay and route
- Accept or decline within a short window (typically 40–60 seconds)
- Navigate to restaurant and then consumer; mark pickup and drop-off milestones
- Track earnings and schedule "Dash Now" or future Dash shifts

**Platform**
- Order lifecycle state machine: placed → merchant accepted → preparing → ready → dasher assigned → picked up → delivered (or cancelled)
- Dynamic delivery fee and surge pricing based on demand/supply imbalance
- ETA estimation for delivery time
- Fraud detection on payment and suspicious order patterns

### Non-Functional Requirements

- **Availability**: 99.99% for order placement and dispatch paths (≤ 52 min downtime/year)
- **Latency**: order placement API ≤ 500 ms p99; Dasher location write ≤ 100 ms p99; dispatch decision ≤ 2 s
- **Consistency**: payments and order state must be strongly consistent; location data can be eventually consistent
- **Durability**: zero order loss; event log persisted before any state transition
- **Scalability**: handle 3× baseline load during peak meal windows (Friday/Saturday 5–9 PM local time)
- **Compliance**: PCI DSS for payment data; GDPR/CCPA for consumer PII; SOC 2 for internal systems

### Out of Scope

- DashPass subscription billing engine internals
- Alcohol/age-gated delivery legal compliance detail
- Drive (white-label API for third-party merchants)
- Full ML model training pipelines (referenced but not designed)
- International multi-currency specifics

---

## 2. Users & Scale

### User Types

| Actor    | Description                                              | Approx. MAU (2024 est.) |
|----------|----------------------------------------------------------|-------------------------|
| Consumer | Places food orders via app or web                        | 37 million              |
| Merchant | Restaurant/ghost kitchen receiving and fulfilling orders | 700 000 locations       |
| Dasher   | Contractor driver fulfilling deliveries                  | 7 million (active pool) |

### Traffic Estimates

**Assumptions**
- 37 M MAU consumers; average order frequency = 4 orders/month → 148 M orders/month
- Peak-to-average multiplier: 3× during Friday/Saturday dinner (5–9 PM local, rolling across time zones)
- Dasher location ping interval: every 4 seconds while on a Dash
- ~500 K active Dashers on platform at peak dinner hour

| Metric                          | Calculation                                                                 | Result               |
|---------------------------------|-----------------------------------------------------------------------------|----------------------|
| Orders per day                  | 148 M / 30                                                                  | ~4.93 M/day          |
| Average orders per second       | 4.93 M / 86 400                                                             | ~57 orders/s         |
| Peak orders per second          | 57 × 3                                                                      | ~171 orders/s        |
| Dasher location writes (peak)   | 500 K Dashers × (1 ping / 4 s)                                              | 125 000 writes/s     |
| Dasher location reads (tracking)| Assume 1 M consumers actively tracking × (1 poll or push / 5 s)            | 200 000 reads/s      |
| Menu page views                 | Assume 5 page views per order funnel × 171 peak orders/s × 10 browse-only  | ~8 500 views/s (est) |
| Dispatch evaluations            | 171 peak orders/s × avg 10 candidate Dashers evaluated                      | 1 710 evaluations/s  |

### Latency Requirements

| Operation                        | p50 target | p99 target | Notes                                    |
|----------------------------------|------------|------------|------------------------------------------|
| Restaurant list page load        | 80 ms      | 300 ms     | Cached; user-facing                      |
| Menu page load                   | 80 ms      | 250 ms     | Heavily cached                           |
| Order placement (end-to-end)     | 200 ms     | 500 ms     | Includes payment auth                    |
| Dispatch assignment              | <1 s       | 2 s        | Time-sensitive; Dasher window is ~60 s   |
| Dasher location write ACK        | 20 ms      | 100 ms     | High-throughput ingest path              |
| ETA recalculation                | 50 ms      | 200 ms     | Called on each location update           |
| Push notification delivery       | <2 s       | <10 s      | Status change events                     |

### Storage Estimates

| Data Type                        | Calculation                                                                    | Result        |
|----------------------------------|--------------------------------------------------------------------------------|---------------|
| Order records                    | 4.93 M/day × 365 days × 3 years × 2 KB/order                                  | ~10.8 TB      |
| Order items / line items         | avg 3 items/order × same volume × 0.5 KB                                       | ~8 TB         |
| Dasher location history          | 500 K peak × 4 s interval × 86 400 s/day × 20 bytes/point × 30 day retention  | ~21.6 TB/month|
| Menu catalog                     | 700 K restaurants × avg 100 items × 1 KB/item                                  | ~70 GB        |
| User profiles + addresses        | 37 M × 2 KB                                                                    | ~74 GB        |
| Payment tokens (vault reference) | 37 M × 200 bytes                                                                | ~7.4 GB       |
| Review/rating data               | 4.93 M orders/day × 40% review rate × 500 bytes × 365 × 3                     | ~1.08 TB      |

### Bandwidth Estimates

| Flow                             | Calculation                                                                    | Result          |
|----------------------------------|--------------------------------------------------------------------------------|-----------------|
| Dasher location ingress          | 125 000 writes/s × 200 bytes/payload                                           | 25 MB/s ingress |
| Consumer location stream egress  | 200 000 reads/s × 150 bytes/update                                             | 30 MB/s egress  |
| Menu page API egress             | 8 500 requests/s × 20 KB avg response                                          | 170 MB/s egress |
| Order placement ingress          | 171 orders/s × 5 KB/payload                                                    | 0.86 MB/s       |
| Total estimated peak egress      | —                                                                               | ~250 MB/s       |

---

## 3. High-Level Architecture

```
                           ┌──────────────────────────────────────────────────────────────────┐
                           │                          CLIENTS                                  │
                           │   Consumer App (iOS/Android/Web)                                  │
                           │   Merchant Tablet App / POS Integration                           │
                           │   Dasher App (iOS/Android)                                       │
                           └───────────────────────┬──────────────────────────────────────────┘
                                                   │ HTTPS / WSS
                                     ┌─────────────▼──────────────┐
                                     │    API Gateway / CDN Edge   │
                                     │  (Rate Limiting, Auth JWT,  │
                                     │   TLS Termination, Routing) │
                                     └──────────┬──────────────────┘
                                                │
               ┌────────────────────────────────┼─────────────────────────────────┐
               │                                │                                 │
    ┌──────────▼───────────┐       ┌────────────▼──────────┐      ┌──────────────▼──────────┐
    │   Order Service      │       │  Location Service      │      │  Restaurant/Menu Service │
    │  (Order lifecycle,   │       │  (Dasher GPS ingest,  │      │  (Catalog, availability, │
    │   state machine,     │       │   ETA engine,         │      │   search index write)    │
    │   payment bridge)    │       │   WebSocket fanout)   │      └──────────────────────────┘
    └──────────┬───────────┘       └────────────┬──────────┘
               │                                │
    ┌──────────▼───────────┐       ┌────────────▼──────────┐
    │  Dispatch Service    │       │   Notification Service │
    │  (Dasher matching,   │       │  (APNs / FCM / SMS /  │
    │   offer generation,  │       │   WebSocket push)     │
    │   acceptance window) │       └───────────────────────┘
    └──────────┬───────────┘
               │
    ┌──────────▼───────────┐
    │  Pricing Service     │
    │  (Surge, delivery    │
    │   fee, pay estimate) │
    └──────────────────────┘

         ┌────────────────────────────────────────────────────────┐
         │                   MESSAGING BACKBONE                   │
         │         Apache Kafka (partitioned by order_id)         │
         │   Topics: order-events, location-events,               │
         │           dispatch-events, notification-events         │
         └────────────────────────────────────────────────────────┘

    ┌──────────────────────┐  ┌──────────────────┐  ┌───────────────────────┐
    │  PostgreSQL Cluster  │  │  Redis Cluster   │  │  Cassandra Cluster    │
    │  (Orders, Users,     │  │  (Sessions,      │  │  (Location time-      │
    │   Merchants, Payments│  │   cart, Dasher   │  │   series, ephemeral   │
    │   ACID transactions) │  │   current loc,   │  │   tracking data)      │
    │                      │  │   surge zones,   │  └───────────────────────┘
    │                      │  │   menu cache)    │
    └──────────────────────┘  └──────────────────┘

    ┌──────────────────────────────────────────────────────────────┐
    │  Elasticsearch Cluster                                       │
    │  (Restaurant search, menu item search, geo-filtered queries) │
    └──────────────────────────────────────────────────────────────┘
```

**Component roles:**

- **API Gateway**: Single ingress; terminates TLS, validates JWT tokens, enforces per-consumer/per-Dasher rate limits, routes to downstream services. Runs at edge PoPs via Cloudflare/AWS CloudFront.
- **Order Service**: Owns the canonical order state machine. Writes to PostgreSQL inside a distributed transaction; publishes `order-events` to Kafka. Acts as the orchestrator for payment, dispatch trigger, and merchant notification.
- **Location Service**: Receives high-throughput Dasher GPS pings (125K writes/s). Writes current position to Redis (single-key per Dasher, TTL 10 s) and appends to Cassandra for history. Fans out updates to subscribed consumers via WebSocket. Recomputes ETA on each ping using a road-network graph service.
- **Dispatch Service**: Subscribes to `order-events` (specifically `merchant_accepted`). Queries nearby Dashers from Redis geo-index. Scores candidates (proximity, rating, current load, acceptance rate). Sends offer to top-N Dashers with a 60-second TTL. Handles acceptance/rejection FSM.
- **Restaurant/Menu Service**: Serves menu catalog. Writes menu changes to PostgreSQL and syncs to Elasticsearch for search. Handles merchant pause/resume signals. Maintains Redis cache of active menus.
- **Pricing Service**: Computes delivery fee quote at checkout. Evaluates supply/demand ratio per geo-hexagon (H3 resolution 7). Emits surge multipliers consumed by Dispatch for Dasher pay boosts and by Order Service for consumer fee display.
- **Notification Service**: Consumes `order-events`, `dispatch-events`, and triggers APNs/FCM push, SMS (Twilio), and in-app WebSocket messages for all three actor types.

**Primary use-case data flow (consumer places order):**

1. Consumer selects items, taps "Place Order." App calls `POST /v1/orders`.
2. API Gateway validates JWT, rate-limits, routes to Order Service.
3. Order Service creates order record in PostgreSQL (`status=pending_payment`), calls Payment Service (Stripe tokenized charge). On success, transitions to `placed`.
4. Order Service publishes `order.placed` event to Kafka `order-events` topic, keyed by `restaurant_id` for locality.
5. Merchant Notification Service consumes event; pushes to merchant tablet via FCM. Merchant taps Accept → `PATCH /v1/orders/{id}/accept`.
6. Order Service transitions order to `merchant_accepted`; publishes event.
7. Dispatch Service consumes `order.merchant_accepted`; queries Redis geo-index for Dashers within 3 km radius. Scores, sends offer to top Dasher via push.
8. Dasher accepts → order transitions to `dasher_assigned`; consumer and merchant receive push.
9. Dasher arrives at restaurant, taps "Picked Up" → `dasher_picked_up`. Location Service streams Dasher coordinates to consumer WebSocket.
10. Dasher arrives at consumer address, taps "Delivered" → order `delivered`. Payment finalizes (tip release). Reviews prompt sent.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────
-- USERS
-- ─────────────────────────────────────────────
CREATE TABLE users (
    user_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email           TEXT UNIQUE NOT NULL,
    phone           TEXT,
    hashed_password TEXT,           -- NULL for SSO users
    full_name       TEXT NOT NULL,
    role            TEXT NOT NULL CHECK (role IN ('consumer','dasher','merchant_staff','admin')),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE consumer_profiles (
    user_id         UUID PRIMARY KEY REFERENCES users(user_id),
    default_address_id UUID,
    stripe_customer_id TEXT,        -- Stripe token reference, NOT raw card
    doordash_credits_cents INT NOT NULL DEFAULT 0
);

-- ─────────────────────────────────────────────
-- ADDRESSES
-- ─────────────────────────────────────────────
CREATE TABLE addresses (
    address_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID REFERENCES users(user_id),
    label           TEXT,           -- 'Home', 'Work', etc.
    street          TEXT NOT NULL,
    city            TEXT NOT NULL,
    state           TEXT NOT NULL,
    zip             TEXT NOT NULL,
    country         TEXT NOT NULL DEFAULT 'US',
    lat             DOUBLE PRECISION NOT NULL,
    lng             DOUBLE PRECISION NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ─────────────────────────────────────────────
-- MERCHANTS
-- ─────────────────────────────────────────────
CREATE TABLE merchants (
    merchant_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            TEXT NOT NULL,
    slug            TEXT UNIQUE NOT NULL,
    description     TEXT,
    cuisine_types   TEXT[],         -- ['american','burgers']
    address_id      UUID REFERENCES addresses(address_id),
    lat             DOUBLE PRECISION NOT NULL,
    lng             DOUBLE PRECISION NOT NULL,
    rating          NUMERIC(3,2),
    review_count    INT NOT NULL DEFAULT 0,
    is_active       BOOLEAN NOT NULL DEFAULT true,
    is_paused       BOOLEAN NOT NULL DEFAULT false,   -- merchant manually paused
    prep_time_min   INT NOT NULL DEFAULT 15,          -- baseline prep estimate
    commission_rate NUMERIC(5,4) NOT NULL DEFAULT 0.15,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE merchant_hours (
    hours_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id     UUID REFERENCES merchants(merchant_id),
    day_of_week     SMALLINT NOT NULL CHECK (day_of_week BETWEEN 0 AND 6),
    open_time       TIME NOT NULL,
    close_time      TIME NOT NULL
);

-- ─────────────────────────────────────────────
-- MENUS
-- ─────────────────────────────────────────────
CREATE TABLE menu_categories (
    category_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id     UUID REFERENCES merchants(merchant_id),
    name            TEXT NOT NULL,
    display_order   INT NOT NULL DEFAULT 0
);

CREATE TABLE menu_items (
    item_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID REFERENCES merchants(merchant_id),
    category_id         UUID REFERENCES menu_categories(category_id),
    name                TEXT NOT NULL,
    description         TEXT,
    price_cents         INT NOT NULL,
    image_url           TEXT,
    is_available        BOOLEAN NOT NULL DEFAULT true,
    dietary_flags       TEXT[],   -- ['vegan','gluten-free','halal']
    calorie_count       INT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE item_modifiers (
    modifier_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    item_id         UUID REFERENCES menu_items(item_id),
    group_name      TEXT NOT NULL,  -- 'Sauce', 'Size'
    option_name     TEXT NOT NULL,
    extra_price_cents INT NOT NULL DEFAULT 0,
    is_required     BOOLEAN NOT NULL DEFAULT false
);

-- ─────────────────────────────────────────────
-- ORDERS
-- ─────────────────────────────────────────────
CREATE TYPE order_status AS ENUM (
    'pending_payment',
    'placed',
    'merchant_accepted',
    'merchant_rejected',
    'preparing',
    'ready_for_pickup',
    'dasher_assigned',
    'dasher_en_route_to_merchant',
    'dasher_at_merchant',
    'dasher_picked_up',
    'dasher_en_route_to_consumer',
    'delivered',
    'cancelled'
);

CREATE TABLE orders (
    order_id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    consumer_id         UUID REFERENCES users(user_id),
    merchant_id         UUID REFERENCES merchants(merchant_id),
    dasher_id           UUID REFERENCES users(user_id),
    delivery_address_id UUID REFERENCES addresses(address_id),
    status              order_status NOT NULL DEFAULT 'pending_payment',
    subtotal_cents      INT NOT NULL,
    delivery_fee_cents  INT NOT NULL,
    service_fee_cents   INT NOT NULL,
    tip_cents           INT NOT NULL DEFAULT 0,
    tax_cents           INT NOT NULL,
    total_cents         INT NOT NULL,
    surge_multiplier    NUMERIC(4,2) NOT NULL DEFAULT 1.00,
    special_instructions TEXT,
    estimated_delivery_at TIMESTAMPTZ,
    actual_delivery_at  TIMESTAMPTZ,
    payment_intent_id   TEXT,       -- Stripe PaymentIntent ID
    idempotency_key     TEXT UNIQUE NOT NULL,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_orders_consumer_id    ON orders(consumer_id, created_at DESC);
CREATE INDEX idx_orders_merchant_id    ON orders(merchant_id, created_at DESC);
CREATE INDEX idx_orders_dasher_id      ON orders(dasher_id, created_at DESC);
CREATE INDEX idx_orders_status         ON orders(status) WHERE status NOT IN ('delivered','cancelled');

CREATE TABLE order_items (
    order_item_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id        UUID REFERENCES orders(order_id),
    item_id         UUID REFERENCES menu_items(item_id),
    quantity        INT NOT NULL DEFAULT 1,
    unit_price_cents INT NOT NULL,  -- snapshot at order time
    item_name       TEXT NOT NULL,  -- snapshot
    modifiers       JSONB           -- [{group:'Sauce',option:'Ranch',price_cents:50}]
);

CREATE TABLE order_status_history (
    history_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id        UUID REFERENCES orders(order_id),
    from_status     order_status,
    to_status       order_status NOT NULL,
    actor_id        UUID,           -- who triggered the transition
    actor_role      TEXT,
    notes           TEXT,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ─────────────────────────────────────────────
-- DASHERS
-- ─────────────────────────────────────────────
CREATE TABLE dasher_profiles (
    user_id             UUID PRIMARY KEY REFERENCES users(user_id),
    vehicle_type        TEXT NOT NULL CHECK (vehicle_type IN ('car','bike','scooter','walk')),
    background_check_status TEXT NOT NULL DEFAULT 'pending',
    rating              NUMERIC(3,2),
    delivery_count      INT NOT NULL DEFAULT 0,
    acceptance_rate     NUMERIC(5,4),
    completion_rate     NUMERIC(5,4),
    is_online           BOOLEAN NOT NULL DEFAULT false,
    last_known_lat      DOUBLE PRECISION,
    last_known_lng      DOUBLE PRECISION,
    last_location_at    TIMESTAMPTZ,
    stripe_account_id   TEXT        -- for payouts
);

CREATE TABLE dispatch_offers (
    offer_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id        UUID REFERENCES orders(order_id),
    dasher_id       UUID REFERENCES users(user_id),
    offered_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at      TIMESTAMPTZ NOT NULL,
    status          TEXT NOT NULL CHECK (status IN ('pending','accepted','declined','expired')),
    estimated_pay_cents INT NOT NULL,
    distance_to_merchant_m INT NOT NULL
);

-- ─────────────────────────────────────────────
-- PAYMENTS
-- ─────────────────────────────────────────────
CREATE TABLE payments (
    payment_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id        UUID REFERENCES orders(order_id),
    stripe_payment_intent_id TEXT UNIQUE NOT NULL,
    amount_cents    INT NOT NULL,
    currency        TEXT NOT NULL DEFAULT 'usd',
    status          TEXT NOT NULL,  -- 'requires_capture','succeeded','refunded'
    captured_at     TIMESTAMPTZ,
    refunded_at     TIMESTAMPTZ,
    refund_reason   TEXT
);

-- ─────────────────────────────────────────────
-- REVIEWS
-- ─────────────────────────────────────────────
CREATE TABLE reviews (
    review_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id        UUID REFERENCES orders(order_id) UNIQUE,
    consumer_id     UUID REFERENCES users(user_id),
    merchant_id     UUID REFERENCES merchants(merchant_id),
    dasher_id       UUID REFERENCES users(user_id),
    food_rating     SMALLINT CHECK (food_rating BETWEEN 1 AND 5),
    delivery_rating SMALLINT CHECK (delivery_rating BETWEEN 1 AND 5),
    comment         TEXT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

### Database Choice

**Requirements driving the decision:**

1. Orders and payments require ACID transactions across multiple tables (order row + payment row + status history row must succeed or all fail atomically).
2. Dasher location is a high-frequency write (125K writes/s), eventually consistent, time-series-like, and has a short retention window (30 days) — very different access pattern from orders.
3. Menu catalog is read-heavy, rarely updated, benefits from full-text and geo search.
4. Session/cart/ephemeral state needs sub-millisecond access and natural TTL support.

| Database          | Pros                                                          | Cons                                                               | Use in this system          |
|-------------------|---------------------------------------------------------------|--------------------------------------------------------------------|-----------------------------|
| PostgreSQL        | ACID transactions, rich SQL, JSONB, PostGIS, mature tooling   | Vertical scalability ceiling; write throughput limited per shard   | Orders, users, merchants, payments, reviews |
| Cassandra         | Linear horizontal write scalability, tunable consistency, TTL | No joins; eventual consistency by default; complex data modeling   | Dasher location time-series |
| Redis (Cluster)   | Sub-ms reads/writes, built-in TTL, Geo commands, Pub/Sub      | In-memory only (expensive at scale), limited durability guarantees | Current Dasher position, cart, sessions, surge zones, menu cache |
| Elasticsearch     | Full-text search, geo-distance queries, aggregations          | Not a primary store; eventual consistency; complex cluster mgmt    | Restaurant + menu search    |
| DynamoDB          | Managed, infinite horizontal scale, consistent at low latency | Expensive at high RCU/WCU; limited query flexibility               | Considered for dispatch offers; PostgreSQL chosen for simplicity |

**Selected primary store: PostgreSQL 16 (AWS Aurora PostgreSQL)**
- Aurora provides multi-AZ synchronous replication with automatic failover in <30 s.
- Aurora Global Database allows cross-region read replicas with <1 s replication lag for read scaling.
- SERIALIZABLE isolation is used only for the critical order state transition; all reads use READ COMMITTED to avoid lock contention.
- Horizontal write scaling: shard orders by `merchant_id` range (so a single restaurant's orders land on the same shard, enabling ordered queries without cross-shard joins). Read replicas handle consumer order history reads.

**Dasher location: Apache Cassandra 4.x**
- Partition key: `dasher_id`; clustering key: `recorded_at` (descending) — enables efficient "latest position" and "recent track" queries.
- Replication factor 3, LOCAL_QUORUM for writes and reads provides durability with one-node failure tolerance.
- TTL of 30 days on all location rows removes the need for a separate cleanup job.
- Redis `GEO` commands maintain the live Dasher geo-index for dispatch radius queries (this is the write-through cache; Cassandra is the durable history store).

---

## 5. API Design

All endpoints require a valid JWT Bearer token in the `Authorization` header unless marked public.
Rate limits enforced at the API Gateway layer via a sliding-window counter stored in Redis.

```
Base URL: https://api.doordash.com/v1
Auth: Bearer <JWT>        (consumer/dasher/merchant role embedded in token claims)
Pagination: cursor-based  (?cursor=<opaque_token>&limit=20, max limit=100)
```

---

### Consumer Endpoints

```
GET  /restaurants
     Query: lat, lng, radius_km, cuisine[], min_rating, max_delivery_fee,
            dietary_filter[], sort_by (relevance|rating|delivery_time|delivery_fee)
     Rate limit: 100 req/min per consumer
     Response: { restaurants: [...], next_cursor, total_count }

GET  /restaurants/{restaurant_id}
     Response: full restaurant object + hours + average prep time
     Cache: public CDN cache 60 s; Surrogate-Key: restaurant:{id}

GET  /restaurants/{restaurant_id}/menu
     Response: { categories: [{ name, items: [{ item_id, name, price_cents,
                dietary_flags, modifiers, is_available }] }] }
     Cache: CDN 30 s, busted on merchant menu update

POST /carts
     Body: { restaurant_id, delivery_address_id }
     Response: { cart_id }

PUT  /carts/{cart_id}/items
     Body: { item_id, quantity, modifiers }
     Response: { cart }

GET  /carts/{cart_id}/quote
     Response: { subtotal_cents, delivery_fee_cents, service_fee_cents,
                 tax_cents, surge_multiplier, estimated_delivery_minutes }

POST /orders
     Body: { cart_id, payment_method_id, tip_cents, special_instructions,
             idempotency_key }
     Rate limit: 5 order attempts/min per consumer (burst protection)
     Response 201: { order_id, status, estimated_delivery_at }
     Response 402: payment failed
     Response 409: idempotency_key already used (returns original response)

GET  /orders/{order_id}
     Response: full order with current status and ETA

GET  /orders
     Query: status, cursor, limit
     Response: paginated order history

PATCH /orders/{order_id}/cancel
     Constraint: only allowed if status in (placed, merchant_accepted)
     Body: { reason }
     Response: { status: 'cancelled', refund_amount_cents }

POST /orders/{order_id}/rating
     Body: { food_rating (1-5), delivery_rating (1-5), comment }
     Constraint: only once per order, within 7 days of delivery
     Response 200: { review_id }
```

### Merchant Endpoints

```
GET  /merchant/orders
     Query: status[], cursor, limit
     Rate limit: 60 req/min per merchant auth token
     Response: paginated orders for the authenticated merchant

PATCH /merchant/orders/{order_id}/accept
     Response: { status: 'merchant_accepted', estimated_ready_at }

PATCH /merchant/orders/{order_id}/reject
     Body: { reason }
     Response: { status: 'merchant_rejected' }

PATCH /merchant/orders/{order_id}/ready
     Response: { status: 'ready_for_pickup' }

PUT  /merchant/menu/items/{item_id}/availability
     Body: { is_available: bool }
     Response: 200; triggers CDN cache purge + Elasticsearch index update

PATCH /merchant/pause
     Body: { paused: bool, resume_at: ISO8601 (optional) }
     Response: 200; broadcasts unavailability to consumers in near-realtime

GET  /merchant/analytics/summary
     Query: start_date, end_date
     Response: { orders_count, gross_revenue_cents, avg_order_value_cents,
                 avg_rating, top_items }
```

### Dasher Endpoints

```
POST /dasher/session/online
     Body: { lat, lng, vehicle_type }
     Response: { session_id, zone_surge_multiplier }

POST /dasher/session/offline
     Response: 200

POST /dasher/location
     Body: { lat, lng, heading_deg, speed_mps, accuracy_m, timestamp_ms }
     Rate limit: 1 call per 3 s enforced server-side (discard if too frequent)
     Response: 204

GET  /dasher/offers/{offer_id}
     Response: { offer_id, order details, estimated_pay_cents, expires_at,
                 merchant_address, delivery_address, distance_m }

POST /dasher/offers/{offer_id}/accept
     Constraint: must arrive before expires_at
     Response: { order_id, merchant navigation coordinates }

POST /dasher/offers/{offer_id}/decline
     Response: 200

PATCH /dasher/orders/{order_id}/milestone
     Body: { milestone: 'at_merchant' | 'picked_up' | 'delivered' }
     Response: { status, next_milestone }

GET  /dasher/earnings
     Query: start_date, end_date
     Response: { deliveries, gross_cents, tips_cents, breakdown_by_day }
```

### Internal / Platform Endpoints (service-to-service, mTLS)

```
POST /internal/dispatch/evaluate
     Called by: Order Service
     Body: { order_id, merchant_lat, merchant_lng, delivery_lat, delivery_lng }
     Response: { ranked_dasher_candidates: [{ dasher_id, score, distance_m }] }

GET  /internal/eta
     Query: origin_lat, origin_lng, destination_lat, destination_lng, pickup_eta_minutes
     Response: { total_eta_minutes, routing_confidence }

POST /internal/pricing/quote
     Body: { pickup_lat, pickup_lng, delivery_lat, delivery_lng, subtotal_cents }
     Response: { delivery_fee_cents, surge_multiplier, surge_zone_id }
```

---

## 6. Deep Dive: Core Components

### 6.1 Order Lifecycle State Machine

**Problem it solves:**
An order passes through a dozen states, touched by three different actors (consumer, merchant, Dasher) on three different apps. Without a rigorous state machine, race conditions arise — e.g., a Dasher marks an order as "picked up" while the merchant is still rejecting it, or a consumer cancels a moment before payment capture. The state machine enforces valid transitions and provides an audit log.

**Approaches comparison:**

| Approach                        | Description                                                   | Pros                                                   | Cons                                                            |
|---------------------------------|---------------------------------------------------------------|--------------------------------------------------------|-----------------------------------------------------------------|
| Status column + application code | Enum column in DB, service checks valid transitions in code   | Simple, no new infrastructure                           | Race conditions under concurrent requests; logic scattered      |
| Optimistic locking + version field | Add `version INT`; CAS update (`WHERE version=N`)            | Prevents lost updates; still simple                    | Still requires application-layer transition table; retry burden |
| Event Sourcing                  | Store events; derive current state from event log             | Full audit trail; time-travel; replay                  | Complex to implement and query; eventual consistency for reads  |
| DB-level SERIALIZABLE + explicit FSM table | Transition table; all writes in SERIALIZABLE transaction   | Correct; fast; audit trail via status_history table    | SERIALIZABLE can reduce throughput (acceptable at 171 orders/s)|
| Workflow orchestrator (Temporal/Conductor) | Durable workflow engine manages state + retries         | Best for long-running flows with compensating actions  | Operational complexity; adds a new stateful system              |

**Selected: DB-level enforced FSM with Kafka event log**

- A `valid_transitions` lookup table enumerates allowed `(from_status, to_status)` pairs.
- Every state change executes inside a PostgreSQL transaction:
  1. `SELECT ... FOR UPDATE` on the `orders` row (row-level lock).
  2. Check that the transition is in the `valid_transitions` table.
  3. `UPDATE orders SET status = $new, updated_at = now() WHERE order_id = $id AND status = $expected` (optimistic check as a second safety net).
  4. `INSERT INTO order_status_history ...`.
  5. Publish Kafka event (transactional outbox pattern — written to `outbox` table in the same transaction, polled by a Debezium CDC connector).

- The **transactional outbox** pattern is critical: the Kafka publish is not inside the DB transaction, so it can fail independently. Debezium reads the PostgreSQL WAL and publishes reliably, achieving at-least-once delivery to Kafka. Consumers deduplicate by `order_id + from_status + to_status`.

**Implementation detail:**

```
valid_transitions table:
 from_status                    → to_status
 NULL (new)                     → pending_payment
 pending_payment                → placed, cancelled
 placed                         → merchant_accepted, merchant_rejected, cancelled
 merchant_accepted              → preparing, cancelled
 preparing                      → ready_for_pickup
 ready_for_pickup               → dasher_assigned
 dasher_assigned                → dasher_en_route_to_merchant
 dasher_en_route_to_merchant    → dasher_at_merchant
 dasher_at_merchant             → dasher_picked_up
 dasher_picked_up               → dasher_en_route_to_consumer
 dasher_en_route_to_consumer    → delivered
 (almost all)                   → cancelled  (handled separately with compensation)
```

Cancellation compensation: if cancellation occurs after payment capture, a Stripe refund is issued asynchronously; the amount depends on the stage (full refund before merchant accepts; partial or none after pickup).

**Interviewer Q&As:**

Q1: How do you prevent a Dasher from marking an order "picked up" if the merchant rejected it 2 seconds prior?
A: The `SELECT ... FOR UPDATE` lock on the order row serializes concurrent updates. The Dasher's transition attempt will find `status=merchant_rejected`, which is not in the valid transition list from that state, and will receive a 409 Conflict. The Dasher app receives a push notification of the rejection before they can even attempt the milestone update.

Q2: Why not use a workflow engine like Temporal for the order FSM?
A: Temporal is appropriate for very long-running workflows with complex compensation. An order lifecycle is <2 hours, has predictable transitions, and the team already runs PostgreSQL. Temporal adds a stateful cluster that must be operated and scaled. At 171 orders/s our simple DB-based FSM is well within PostgreSQL's write capacity. We would reconsider Temporal for complex features like subscription billing or scheduled pickup workflows.

Q3: What happens if the Debezium CDC connector goes down?
A: Debezium stores its WAL offset in Kafka. On restart it replays from the last committed offset, so no events are lost. The `outbox` table rows remain until Debezium publishes them. Downstream services handle duplicate events via idempotency keys.

Q4: How do you handle a consumer cancelling and a Dasher accepting the dispatch offer simultaneously?
A: Both operations take a `SELECT ... FOR UPDATE` lock on the `orders` row. One succeeds, one gets the lock and finds the order is in a state where the transition is invalid. The loser receives a 409 and the appropriate compensation is triggered. The dispatch offer TTL also prevents stale accepts.

Q5: How would you add a "Dasher returned to restaurant" state for incorrect orders?
A: Add the new enum value to `order_status`, insert two rows into `valid_transitions` (`delivered → return_in_progress`, `return_in_progress → returned_to_merchant`), and deploy the transition logic. The event-sourced outbox ensures all downstream consumers react correctly when they see the new event type.

---

### 6.2 Dasher Dispatch System

**Problem it solves:**
When an order is ready for dispatch, we must find the best available Dasher within a geographic radius and extend an offer with a short acceptance window. This must happen within ~2 seconds (human perception), handle 171 simultaneous new orders at peak, be fair to Dashers (who game acceptance rates), and optimize for minimum total delivery time.

**Approaches comparison:**

| Approach                         | Description                                               | Pros                                               | Cons                                                   |
|----------------------------------|-----------------------------------------------------------|----------------------------------------------------|--------------------------------------------------------|
| Nearest Dasher (greedy distance) | Offer to closest Dasher first                             | Simple; low latency                                | Ignores Dasher load, direction-of-travel; poor matches |
| Broadcast to all nearby          | Send offer to all Dashers within radius; first accept wins | Fast assignment                                   | Fairness issues; parallel lock contention on order     |
| Sequential ranked offer          | Score candidates; offer one at a time; 60 s window        | Fair; optimizes for quality                        | Worst case: N × 60 s if all decline                    |
| Batch optimization (Hungarian)   | Solve assignment as a min-cost bipartite matching          | Globally optimal; efficient at scale               | 1–2 s batch window adds latency; complex to re-run     |
| ML-based real-time scoring       | Gradient-boosted model predicts delivery time per pairing | Best quality; adapts to conditions                 | Model serving latency; training/inference pipeline     |

**Selected: Ranked sequential offer with batched ML scoring**

DoorDash's public engineering posts describe a combination: a fast geo-query produces candidates, an ML model scores each (predicted delivery time, Dasher availability signal, historical acceptance), and offers are sent one at a time in ranked order. We replicate this:

1. **Geo-index query**: Redis `GEORADIUS dasher:online {merchant_lat} {merchant_lng} 5 km ASC COUNT 20`. This returns up to 20 candidate Dasher IDs sorted by distance. Latency: <5 ms.

2. **Scoring**: For each candidate, fetch Dasher profile from Redis hash (`dasher:profile:{id}` — cached, TTL 30 s). Compute a score:
   ```
   score = w1 * (1 / distance_km)
           + w2 * acceptance_rate
           + w3 * completion_rate
           - w4 * (orders_in_progress)    # penalize already-loaded Dashers
           + w5 * direction_alignment_score  # heading toward merchant
   ```
   Weights are calibrated offline via A/B tested gradient descent. In production, a lightweight ONNX model runs inline in the Dispatch Service (sub-millisecond inference, no network hop).

3. **Offer issuance**: Insert into `dispatch_offers` table with `expires_at = now() + 60s`. Push offer to Dasher via FCM/WebSocket. A Quartz scheduler job runs every 5 s to expire timed-out offers and cascade to the next candidate.

4. **Acceptance handling**: `POST /dasher/offers/{id}/accept` triggers a DB transaction: verify `expires_at > now()` AND `orders.status = 'ready_for_pickup'` (double-check that the order wasn't cancelled), then atomically update both.

5. **No Dasher found**: After exhausting all candidates within 5 km, expand radius to 10 km and re-query. After 3 expansion rounds with no accept, escalate to ops team and surface "No Dashers available" to consumer with an option to cancel for a full refund.

**Implementation detail — Redis geo-index maintenance:**

Each Dasher location ping hits the Location Service which does:
```
GEOADD dasher:online {lng} {lat} {dasher_id}
HSET dasher:profile:{dasher_id} lat {lat} lng {lng} heading {heading} last_seen {ts}
EXPIRE dasher:profile:{dasher_id} 30
```
When a Dasher goes offline, the Dasher Session Service runs `ZREM dasher:online {dasher_id}`. If the Dasher's app crashes, the 30-second TTL on the profile key signals staleness, and the dispatch system marks the candidate as unreliable during scoring.

**Interviewer Q&As:**

Q1: What prevents two orders from being assigned to the same Dasher simultaneously?
A: Each dispatch offer acceptance acquires a `SELECT ... FOR UPDATE` on the `dispatch_offers` row before transitioning the order. The order row has its own lock. A Dasher is only in the `dasher:online` geo-index if their current in-progress order count is below the threshold (managed by the Location Service when publishing profile updates).

Q2: How do you handle a Dasher who repeatedly declines to game the acceptance rate?
A: Acceptance rate is tracked in `dasher_profiles`. It factors into the dispatch score (lower rate → lower priority for future offers). Additionally, a Dasher who declines too frequently in a short window may be placed in a "cool-down" queue and deprioritized for 10 minutes, which is surfaced transparently in the Dasher app.

Q3: What's the latency budget for the dispatch pipeline?
A: Order Service publishes `order.merchant_accepted` → Kafka consumer in Dispatch Service processes it → geo-query (5 ms) → scoring (2 ms) → DB write (10 ms) → FCM push (50–200 ms to device). Total from event to Dasher receiving offer: ~300 ms in the happy path.

Q4: How does batched matching differ from sequential, and when would you switch?
A: Batched (Hungarian algorithm) solves the global minimum-cost assignment across all pending orders and all available Dashers simultaneously in one pass, theoretically reducing total delivery time across the system. It requires a batching window (e.g., 1 s) which adds latency for individual orders. At our scale (171 orders/s), the batching benefit is measurable. We would switch to full batch optimization if we could prove via A/B test that global delivery time improves enough to justify the 1-second added latency budget.

Q5: How do you handle zone boundaries — Dashers in the suburbs being offered downtown orders?
A: H3 hexagonal geo-indexing at resolution 7 (~1.2 km² cells) partitions Dashers and merchants into zones. Dispatch first queries the same H3 cell and immediate neighbors (ring-1 to ring-3). Cross-zone offers are only issued after intra-zone candidates are exhausted. This also enables zone-level surge pricing: if a hex has a high demand/supply ratio, a surge bonus is added to the Dasher pay estimate to attract cross-zone Dashers.

---

### 6.3 Delivery Time Estimation (ETA Engine)

**Problem it solves:**
Consumers make ordering decisions based on delivery time. Merchants and Dashers adjust behavior based on expected pickup time. Inaccurate ETAs damage trust — consistently over-estimating frustrates customers; under-estimating causes complaints. ETA must be computed at checkout (quote), updated on every Dasher location ping, and be accurate to within ±3 minutes 80% of the time.

**Approaches comparison:**

| Approach                               | Description                                             | Pros                                                  | Cons                                                        |
|----------------------------------------|---------------------------------------------------------|-------------------------------------------------------|-------------------------------------------------------------|
| Static distance ÷ speed               | ETA = straight-line distance / avg speed                | Trivially simple                                      | Ignores roads, traffic, prep time; ±30 min accuracy typical |
| Google Maps / Mapbox Distance Matrix API | External routing API for drive time                  | Accurate routing; no infrastructure                   | Per-call cost at 125K/s is prohibitive; API latency ~100 ms |
| Precomputed road-network graph (OSRM)  | Self-hosted OpenStreetMap routing engine                | Low latency (<10 ms); no per-call cost; customizable  | Requires maintaining map data; no live traffic               |
| ML two-tower model (DoorDash approach) | Separate models for prep time and drive time; blended   | Highest accuracy; learns restaurant-specific patterns | Complex ML pipeline; feature engineering; model drift        |
| Hybrid: OSRM + ML prep time predictor | OSRM for routing; gradient-boosted model for prep time  | Accurate; fast; feasible for one team to own          | Still requires ML infra; feature store                       |

**Selected: Hybrid OSRM + gradient-boosted prep time model**

ETA = `prep_time_estimate` + `dasher_travel_to_merchant` + `service_time_at_merchant` + `dasher_travel_to_consumer`

- **Prep time estimate**: GBM model trained on historical (merchant_id, time_of_day, day_of_week, order_size, current_order_queue_depth) → predicted prep minutes. Served via a Feature Store (Redis-backed with offline Spark training). Model retrained weekly. At checkout this returns a p75 estimate (slightly conservative) to set consumer expectations.

- **Drive time**: OSRM self-hosted with H3-gridded live traffic overlays (speed reduction factors updated every 2 minutes from Dasher telemetry data — a feedback loop unique to the platform). OSRM API call: `GET /route/v1/driving/{lng1},{lat1};{lng2},{lat2}?overview=false` → returns duration in seconds. Latency <10 ms on an in-region instance.

- **Recalculation trigger**: Each Dasher location ping publishes to Kafka. ETA Recalculation Worker consumes the event, calls OSRM for remaining leg(s), adds remaining prep time (if still at merchant), and publishes the updated ETA to the consumer's WebSocket session. All 125K pings/s do NOT recompute — a Redis rate-limiter ensures ETA is recalculated at most once per 15 seconds per order, reducing computation to ~66K order-ETAs/s at peak (across all active deliveries).

**Interviewer Q&As:**

Q1: How accurate is the ETA, and how do you measure it?
A: We track `|actual_delivery_at - estimated_delivery_at|` at various points in the journey. A dashboard monitors the p50 and p80 absolute error. Industry target is ±5 min at p80 at checkout and improving to ±2 min when the Dasher is within 1 km. Alert fires if p80 error exceeds 8 minutes, triggering an ML model retraining pipeline.

Q2: What happens if OSRM goes down?
A: OSRM runs as a multi-instance deployment behind a load balancer. On total failure, the ETA service falls back to a precomputed lookup table: `(distance_bin_km, time_of_day, day_of_week) → avg_minutes_historically`. This is less accurate but avoids showing a loading spinner to consumers. The circuit breaker opens after 5 consecutive OSRM failures and auto-closes after 30 seconds.

Q3: How do you handle restaurant prep time variance during a rush?
A: The ML model uses `current_order_queue_depth` as a feature — specifically the number of unacknowledged and in-preparation orders for the merchant at the moment of the estimate. This is computed from the active orders count in Redis (a merchant-keyed counter maintained by the Order Service). Queue depth has the highest feature importance in the trained model.

Q4: Why use p75 prep time at checkout rather than p50?
A: p50 means 50% of orders take longer than quoted, causing consumer complaints. p75 means only 25% of orders are late relative to the quote, at the cost of slightly longer-looking ETAs. A/B testing showed that consumers accept a longer-looking ETA more readily than an exceeded one. DoorDash engineering has published similar findings about promise reliability vs. promise brevity.

Q5: How would you incorporate bad weather or traffic incidents into ETA?
A: Two mechanisms: (1) OSRM speed profiles are updated every 2 minutes from live Dasher telemetry, so a traffic backup is naturally reflected as lower average speeds on that road segment within 2–4 minutes. (2) For severe weather (heavy rain, snow), a feature flag applies a global multiplier (e.g., 1.3×) to all ETA computations in the affected region, derived from a weather API (OpenWeatherMap) polled every 5 minutes.

---

## 7. Scaling

### Horizontal Scaling

**Order Service**: Stateless; deployed as Kubernetes pods behind a load balancer. Auto-scales on RPS metric (target 100 req/pod). At 171 peak orders/s with p99 500 ms processing: 171 × 0.5 s = 85.5 concurrent requests → ~2 pods with head-room safety margin of 4 pods. In practice, 6 pods handle steady state with 10 during peaks.

**Location Service**: Stateless writers; stateful WebSocket fan-out nodes. Writers scale horizontally — each pod writes to Redis and Cassandra. Fan-out pods maintain WebSocket connections; sticky routing by `order_id` ensures the consumer's WebSocket message is forwarded by the pod that holds that session (or via Redis Pub/Sub cross-pod fan-out).

**Dispatch Service**: Stateless; Kafka consumer group with N partitions = N max parallel consumers. Partition by `merchant_id` so all orders for a restaurant are processed in order, avoiding race conditions in sequential offer issuance.

### Database Sharding

**PostgreSQL (Orders)**: Shard by `merchant_id` using CitusDB (now integrated into Aurora for PostgreSQL via Babelfish extensions, or using AWS Aurora Sharding). Each shard holds 1/N of the merchants. Cross-shard joins are avoided by denormalizing consumer address info into the order row. A routing service maps `merchant_id → shard_id` via a consistent-hash ring stored in ZooKeeper. New shards are added during low-traffic windows by splitting a shard's key range.

**Cassandra (Location)**: Natural horizontal scaling via consistent hashing. Add nodes to the ring; `nodetool decommission` on removed nodes. Replication factor 3 ensures no data loss during re-balancing.

**Redis**: Redis Cluster with 16 384 hash slots distributed across N master nodes (each with 1 replica). Dasher geo-keys are distributed across cluster based on the key hash. For the geo-index, all Dashers share a single ZSET key `dasher:online` — at 500K Dashers this is a 500K-element sorted set, which fits comfortably in Redis memory (~40 MB) and `GEORADIUS` is O(N+log(M)) which is fast. If this becomes a bottleneck, we shard by H3 zone: `dasher:online:{h3_cell_id}` and query only neighboring cells.

### Replication

- **PostgreSQL Aurora**: 1 writer + up to 15 read replicas per cluster. Consumer order history queries routed to read replicas. Writes always go to the writer endpoint.
- **Cassandra**: RF=3, LOCAL_QUORUM consistency — survives 1-node failure in a 3-node rack.
- **Redis**: Each master has 1 replica. Sentinel or Redis Cluster promotes replica to master on failure.

### Caching Strategy

| Data                  | Cache layer    | TTL       | Invalidation trigger                              |
|-----------------------|----------------|-----------|---------------------------------------------------|
| Menu catalog          | Redis + CDN    | 60 s / 5 min | `PUT /merchant/menu/items/{id}/availability` sends `PURGE` to CDN via Surrogate-Key header |
| Restaurant list page  | CDN            | 30 s      | Merchant status change publishes cache-bust event  |
| Dasher current location | Redis        | 10 s      | New location ping overwrites                      |
| Session / JWT         | Redis          | 15 min    | Sliding TTL on each request                       |
| Cart                  | Redis          | 24 hr     | TTL, or cart checkout clears it                   |
| Surge zone multipliers | Redis         | 60 s      | Pricing Service publishes update every 60 s       |
| ETA quote             | Redis          | 15 s      | Stale-while-revalidate; recomputed asynchronously |
| Dasher dispatch profile | Redis hash   | 30 s      | Written on each location ping                     |

### CDN

Static assets (menu images, restaurant hero images) served from S3 + CloudFront. Images uploaded once on merchant/item creation; CloudFront origin shield reduces S3 API calls. API responses for public restaurant/menu pages use CloudFront as a caching reverse proxy with a 30-second TTL and vary on `Accept-Language`.

**Interviewer Q&As:**

Q1: How do you handle a hot shard when a very popular restaurant receives a spike of orders?
A: A single restaurant on one shard is bounded by the restaurant's physical throughput (at most a few hundred orders/hour). The DB shard for that `merchant_id` would serve high read traffic from the Merchant app; we route reads to a dedicated read replica for that shard. If a restaurant chain has thousands of locations, they would span many shards by design (different `merchant_id` per location).

Q2: How do you scale the WebSocket fan-out for Dasher location tracking during a major event?
A: WebSocket servers are sticky-routed by `order_id`. Each pod subscribes to a Redis Pub/Sub channel `location:{order_id}`. When the Location Service writes a new Dasher position, it publishes to that channel. The subscribing pod pushes to the connected consumer. This decouples location ingestion (125K/s) from fan-out. Redis Pub/Sub handles ~1M messages/s per cluster.

Q3: How would you handle a multi-region deployment?
A: Active-active with Aurora Global Database. Writes for a given consumer's order go to the nearest regional writer (determined by the consumer's phone's time zone / IP). Cross-region replication lag is <1 s. For conflict avoidance, orders are "owned" by the region where they were placed — a consistent-hash on `order_id` determines the owner region. Location data (Cassandra) runs as a separate multi-region cluster with RF=3 per region.

Q4: At what scale would you consider switching from PostgreSQL to a distributed SQL database like CockroachDB or Spanner?
A: When orders/s exceeds ~10 000 and horizontal write scaling of a sharded PostgreSQL becomes operationally burdensome (re-sharding, cross-shard transactions), we would evaluate CockroachDB (geo-partitioned primary regions) or Google Spanner. Both provide ACID transactions across distributed nodes. The tradeoff is higher write latency (~5–10 ms vs. <1 ms local PostgreSQL) and significant migration cost.

Q5: How do you prevent cache stampede when the menu cache expires for a popular restaurant during a lunch rush?
A: Use the "probabilistic early expiration" (PER) technique (also known as XFetch): when a cache entry is within N seconds of expiration, a small random probability triggers early recomputation. Combined with a Redis `SET NX` mutex lock on `menu_recomputing:{merchant_id}`, only one pod recomputes while others serve the stale value (stale-while-revalidate). The CDN uses `s-maxage=30, stale-while-revalidate=60` headers.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario                    | Impact                                      | Detection                                      | Mitigation                                                                  |
|-------------------------------------|---------------------------------------------|------------------------------------------------|-----------------------------------------------------------------------------|
| Order Service pod crash             | In-flight order requests dropped            | Health check fails; Kubernetes restarts pod    | Idempotency key on `POST /orders` — retry is safe; pod restart <10 s       |
| PostgreSQL writer failover          | ~30 s write unavailability (Aurora failover)| Route53 DNS record updated by Aurora           | Order Service retries with exponential backoff up to 60 s; queue in Kafka  |
| Kafka broker failure (1 of 3)       | Partition leader re-election ~5–30 s        | Controller detects missing heartbeat           | RF=3 topics; ISR=2; consumers resume from last committed offset             |
| Redis node failure                  | Cache misses + Dasher geo-index gap         | Redis Sentinel / Cluster detects failure       | Replica promoted; degraded dispatch (DB fallback query) during gap          |
| Payment provider (Stripe) timeout   | Order stuck in `pending_payment`            | HTTP timeout on payment call                   | Idempotent Stripe PaymentIntent; retry with same idempotency key; dead-letter after 3 failures with consumer notification |
| Dasher app GPS unavailable          | Location Service stops receiving pings      | Last ping TTL expires (10 s)                   | ETA calculation falls back to last known location + estimated speed; consumer sees "location unavailable" |
| Merchant tablet offline             | Merchant cannot accept order                | Webhook ACK timeout after 30 s                 | Auto-accept after 7-minute timeout (configurable per merchant); consumer notified of delay |
| OSRM routing service down           | ETA cannot be computed                      | Health check; circuit breaker opens            | Fall back to precomputed distance-based lookup table                         |
| Dispatch Service fails mid-assignment | Offer in limbo                             | Kafka consumer lag alarm                       | Offer expires via TTL in `dispatch_offers`; re-queued and re-evaluated      |
| Entire AZ goes down                 | ~33% of traffic affected                   | AWS health events                              | Multi-AZ Aurora, Cassandra RF=3, Kubernetes multi-AZ node groups            |

### Retries & Idempotency

- All order-mutating API calls require a client-generated `idempotency_key` (UUID). The server stores `(idempotency_key, response)` in a Redis key with 24-hour TTL. Duplicate requests within 24 hours return the cached response without re-executing.
- Payment calls use Stripe's native idempotency key (same as our `idempotency_key` prefixed with `stripe-`).
- Kafka consumers implement at-least-once processing with idempotent handlers: each handler checks whether the event has been processed by looking up `processed_events:{event_id}` in Redis before executing business logic.
- Exponential backoff with jitter is enforced on all inter-service HTTP calls: base delay 100 ms, multiplier 2, max 10 s, jitter ±20%.

### Circuit Breaker

Implemented via Resilience4j (JVM services) or `pybreaker` (Python services):
- **Closed state**: requests pass through normally.
- **Open state**: triggered after 5 consecutive failures or error rate >50% in a 10-second sliding window. Requests fail fast with a 503 response; downstream service gets recovery time.
- **Half-open state**: after 30 seconds, 1 probe request is allowed. If successful, circuit closes.
- Applied on: Stripe calls (Payment), OSRM calls (ETA), FCM/APNs calls (Notification), and all cross-service HTTP calls.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric                                | Type        | Alert Threshold                   | Purpose                                              |
|---------------------------------------|-------------|-----------------------------------|------------------------------------------------------|
| order_placement_latency_p99           | Histogram   | > 500 ms                          | Consumer-facing SLA violation                        |
| order_placement_success_rate          | Counter     | < 99.5%                           | Payment or server-side errors                        |
| dispatch_time_to_assign_p95           | Histogram   | > 90 s                            | Dasher supply shortage or dispatch bug               |
| orders_in_status_pending_payment_age  | Gauge       | > 120 s                           | Stuck orders (payment provider issue)                |
| dasher_location_write_latency_p99     | Histogram   | > 100 ms                          | Location ingest bottleneck                           |
| kafka_consumer_lag_dispatch           | Gauge       | > 1 000 messages                  | Dispatch Service falling behind                      |
| redis_memory_used_percent             | Gauge       | > 80%                             | Risk of eviction of geo-index or cache entries       |
| db_replication_lag_seconds            | Gauge       | > 5 s                             | Read replica staleness                               |
| circuit_breaker_state_open            | Event       | Any occurrence                    | Downstream dependency failure                        |
| eta_absolute_error_p80_minutes        | Histogram   | > 8 min                           | ETA model drift or OSRM degradation                  |
| dasher_acceptance_rate_by_zone        | Gauge       | < 60% in any zone                 | Supply shortfall; trigger surge pricing              |
| payment_failure_rate                  | Counter     | > 2%                              | Card issuer issues or fraud spike                    |

### Distributed Tracing

- OpenTelemetry (OTEL) SDK instrumented in all services. Trace propagation via `traceparent` HTTP header.
- Traces exported to Jaeger (self-hosted) or AWS X-Ray.
- Every Kafka message carries the originating `trace_id` in message headers, enabling end-to-end trace reconstruction across async boundaries.
- A single order placement trace spans: API Gateway → Order Service → Payment Service → Kafka publish → Merchant Notification → (async) Dispatch Service. Visible as a single waterfall in Jaeger.
- Sampling rate: 100% for error traces; 1% for successful traces (head-based sampling with tail-based override for slow traces >1 s).

### Logging

- Structured JSON logs emitted via SLF4J/Logback → Fluentd sidecar → Elasticsearch + Kibana (ELK stack) or AWS OpenSearch.
- Every log line includes: `service`, `trace_id`, `span_id`, `order_id` (when applicable), `user_id`, `level`, `timestamp`, `message`.
- Sensitive fields (`card_number`, `ssn`) are redacted at the Fluentd filter layer before indexing.
- Log retention: 30 days hot in OpenSearch; 1 year cold in S3 Glacier (required for PCI DSS audit trail).

---

## 10. Trade-offs & Design Decisions Summary

| Decision                          | Choice Made                                 | Alternative                         | Rationale                                                                 | Trade-off Accepted                                               |
|-----------------------------------|---------------------------------------------|-------------------------------------|---------------------------------------------------------------------------|------------------------------------------------------------------|
| Order state storage               | PostgreSQL with FSM transition table        | Temporal workflow engine            | Simpler; sufficient at current scale; ACID guarantees                     | Less resilient to long-running compensation; revisit at 10× scale |
| Location storage (history)        | Cassandra                                   | TimescaleDB / InfluxDB              | Linear write scalability; TTL-native; battle-tested at DoorDash scale     | No ad-hoc SQL joins on location data                             |
| Dispatch algorithm                | Ranked sequential with ML scoring           | Batch Hungarian assignment          | Low assignment latency (<2 s); simplicity of sequential offer tracking    | Suboptimal global assignment vs. batch; acceptable for most zones |
| ETA computation                   | Self-hosted OSRM + ML prep model            | Google Maps API                     | Cost (125K/s × $0.005/call = $625/s on Maps); latency control             | Operational burden of OSRM cluster; map data freshness           |
| Event delivery                    | Kafka (transactional outbox CDC)            | Direct HTTP webhooks                | At-least-once reliability; replay capability; decoupling                  | Eventual consistency; infrastructure complexity                   |
| Payment                           | Stripe                                      | Braintree / in-house                | Best-in-class fraud detection; PCI DSS scope reduction via tokenization   | Vendor dependency; 0.25% + $0.10 per transaction fee             |
| Search                            | Elasticsearch                               | Algolia / AWS OpenSearch            | Full control; geo-search; no per-query pricing at scale                   | Cluster operations overhead; shard management                     |
| Cache invalidation for menus      | CDN Surrogate-Key purge + Redis TTL         | Event-driven WebSocket push         | Simple; CDN handles global distribution; TTL is a safety net              | Up to 60-second stale menu data visible to consumers             |

---

## 11. Follow-up Interview Questions

**Q1: How would you design the surge pricing algorithm?**
A: Surge pricing monitors supply/demand ratio per H3 hex cell (resolution 7, ~1.2 km²). The ratio is `online_dashers_within_2km / pending_orders_within_2km`. If ratio < 0.8, apply surge. Fee multiplier is a piecewise function: ratio 0.6–0.8 → 1.2×; ratio 0.4–0.6 → 1.5×; ratio < 0.4 → 2.0× (capped by regulation in some cities). This is computed every 60 s by the Pricing Service reading from Redis zone counters and published as a Redis HSET that the Order Service reads at checkout.

**Q2: How would you handle a restaurant that consistently over-promises prep time vs. actual?**
A: The ML prep time model is trained per restaurant. If a restaurant's actual prep times consistently deviate from predictions, the model auto-corrects on next weekly retraining. Additionally, an online exponential moving average of recent prep times is maintained in Redis per merchant, used as a short-term correction factor between model retraining cycles.

**Q3: How would you design the DashPass subscription validation?**
A: DashPass membership status is stored in a `subscriptions` table. The Order Service fetches membership from a lightweight Subscription Service (cached in Redis with a 5-minute TTL). If the consumer is an active member, the delivery fee is waived (set to 0) and service fee is reduced. The Subscription Service handles billing via Stripe Subscriptions and emits `subscription.activated` / `subscription.cancelled` events to bust the cache.

**Q4: How would you prevent fraudulent orders (stolen credit cards used for food delivery)?**
A: Multi-layer approach: (1) Stripe Radar ML on the payment intent (velocity checks, card country vs. delivery country). (2) Platform-level rules: new account placing first order over $100, order to an address never used before with a new payment method → hold for manual review. (3) Velocity limits: max 3 distinct payment methods per account per 24 hours. (4) Device fingerprinting: ban device ID if multiple fraudulent orders. (5) Merchant chargeback rate monitoring: if a merchant has >1% chargebacks, investigate.

**Q5: How would you design "group orders" where multiple people add to one cart?**
A: Create a `group_carts` table with a cart owner and a shareable join code. Each participant's additions are stored as `group_cart_items` with a `contributor_id`. The cart owner sees a consolidated view and can pay for all. The cart has a lock timestamp — once the owner initiates checkout, new items are blocked. Real-time updates to collaborators use WebSocket or long-polling on the cart resource.

**Q6: How would you handle a Dasher being unable to find the consumer at drop-off?**
A: The Dasher app prompts the Dasher to call/text the consumer via the platform's masked number proxy. If unreachable after 5 minutes, the Dasher can mark "safe drop" (leave at door, photo confirmation uploaded via presigned S3 URL). Order transitions to `delivered` with a `safe_drop=true` flag. Consumer receives a push notification with the photo. No refund is issued for safe drop; dispute handled by support.

**Q7: How would you design the Dasher earnings guarantee (e.g., "earn at least $X per active hour")?**
A: Track `dasher_active_minutes` (time between "online" and "offline" events). At end of each Dash session, compute `guaranteed_pay = max(actual_earnings, guarantee_rate * active_hours)`. If guarantee_rate × active_hours > actual, issue a top-up via the Dasher's payment batch at end of week. This requires accumulating per-session stats in a `dasher_sessions` table.

**Q8: How would you handle order modifications after placement (e.g., consumer wants to add an item)?**
A: Order modifications are only allowed during `placed` or `merchant_accepted` states (before preparation begins). The consumer submits a `PATCH /orders/{id}/items` request. The system: (1) Verifies status allows modification. (2) Creates a diff of items added/removed. (3) Recomputes subtotal, fees, and charges/refunds the difference via Stripe. (4) Sends an updated order to the merchant tablet. After `preparing` starts, modifications are rejected with a 409.

**Q9: How would you design the analytics dashboard for merchants?**
A: A separate read-only Analytics Service reads from a BigQuery (or Redshift) data warehouse populated by a daily ETL pipeline from the PostgreSQL order data (via CDC to S3 Parquet to warehouse). Queries like "total revenue last 30 days" or "top 10 items by revenue" run against the warehouse, not production DB. Real-time metrics (orders in the last hour) use a pre-aggregated counter in Redis updated on each order event.

**Q10: How would you approach multi-restaurant ordering (order from two places, delivered together)?**
A: This requires an order "bundle" concept: one consumer session, two sub-orders to different merchants, with a coordinating Dasher. The Dispatch Service needs to assign a single Dasher to both, sequenced by merchant prep times (pick up from merchant A when ready, then merchant B, then deliver). This is a significant complexity increase; in practice DoorDash handles this as "Convenience Mode" with separate deliveries or a bundled pickup logic in a specialized dispatch variant. Data model: `order_bundle` table with FK to two `orders`, each with its own state machine.

**Q11: What are the consistency implications of the Redis geo-index being stale?**
A: The geo-index in Redis is a best-effort cache. If a Dasher's GPS ping is delayed (spotty network), their position is stale for up to 10 s. This means dispatch may rank a Dasher as nearby when they are farther. The practical impact is a few seconds of suboptimal ranking, not a correctness failure — the Dasher still receives the offer and can accept or decline. The Dasher profile TTL (30 s) ensures stale Dashers who have gone offline are excluded.

**Q12: How would you design the consumer notification system to avoid notification fatigue?**
A: Implement a per-consumer notification preference store. Classify notifications by urgency: CRITICAL (order confirmed, delivered), HIGH (Dasher assigned, en route), INFO (promotion, DashPass renewal). Allow consumers to opt out of INFO and HIGH via app settings. Apply debouncing: if the same status event is published twice within 5 seconds (duplicate Kafka delivery), the Notification Service deduplicates by checking `notified_events:{consumer_id}:{event_type}:{order_id}` Redis key with 10-second TTL.

**Q13: How would you handle a Dasher GPS spoofing (a driver claiming to be at delivery location without being there)?**
A: Multiple signals: (1) Speed validation — if consecutive GPS points imply speed >200 km/h, flag as anomalous. (2) Cell tower / WiFi triangulation cross-check (from device sensor data) vs. reported GPS. (3) "Photo at delivery" as soft proof for high-value orders. (4) Complaint rate: if a Dasher has repeated "not delivered" complaints from consumers, their location data is investigated. Anomalous patterns trigger a fraud alert and temporary account hold.

**Q14: What happens to active orders during a Kafka outage?**
A: The transactional outbox pattern stores events in PostgreSQL before they reach Kafka. During a Kafka outage, events accumulate in the outbox table. Order writes still succeed (PostgreSQL is up). However, downstream consumers (Dispatch, Notification) are lagged. Critical notification paths (e.g., merchant accept) have a fallback: the Order Service can directly call the Notification Service HTTP API for time-critical transitions, bypassing Kafka. Once Kafka recovers, Debezium replays the backlog.

**Q15: How would you design the system to support scheduled orders (order for 7 PM delivery, placed at noon)?**
A: Scheduled orders are stored with `status=scheduled` and a `requested_delivery_at` timestamp. A Scheduler Service (backed by a database-driven job queue, or using AWS EventBridge Scheduler) fires at `requested_delivery_at - avg_total_delivery_time` to transition the order to `placed` and begin the normal dispatch flow. The window to begin dispatch is calculated from the ETA model. Merchants receive the order confirmation at placement time but see it as a "future order" with a requested time displayed.

---

## 12. References & Further Reading

- DoorDash Engineering Blog — "How DoorDash Efficiently Dispatches Millions of Tasks" (2020): https://doordash.engineering/2020/06/18/dispatching-millions-of-tasks/
- DoorDash Engineering Blog — "Building Faster Indexing with Apache Kafka and Elasticsearch" (2021): https://doordash.engineering/2021/07/14/open-source-search-indexing/
- DoorDash Engineering Blog — "Grasping the Nettle: How We Rethought ETA Predictions" (2022): https://doordash.engineering/2022/05/31/grasping-the-nettle-how-we-rethought-eta-predictions/
- DoorDash Engineering Blog — "Taming Content Freshness with In-Sync Menu Management" (2021): https://doordash.engineering/2021/02/23/taming-content-freshness-with-insync-menu-management/
- Kleppmann, Martin — "Designing Data-Intensive Applications" (O'Reilly, 2017) — Chapters 5 (Replication), 7 (Transactions), 11 (Stream Processing)
- Uber Engineering Blog — "H3: Uber's Hexagonal Hierarchical Spatial Index" (2018): https://eng.uber.com/h3/
- Redis Geo Commands Documentation: https://redis.io/docs/data-types/geospatial/
- OSRM (Open Source Routing Machine) Documentation: http://project-osrm.org/
- Stripe PaymentIntents API (idempotency): https://stripe.com/docs/api/payment_intents
- Debezium CDC Documentation: https://debezium.io/documentation/
- Chris Richardson — "Microservices Patterns" (Manning, 2018) — Chapter 3 (Saga), Chapter 11 (CQRS)
- AWS Aurora Global Database Documentation: https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/aurora-global-database.html
- Apache Kafka Documentation — Producer Idempotence: https://kafka.apache.org/documentation/#producerconfigs_enable.idempotence
