# System Design: Ticket Booking System (BookMyShow-style)

---

## 1. Requirement Clarifications

### Functional Requirements

1. Users can browse events (movies, concerts, sports) by city, date, genre, and venue.
2. Users can view seat maps for a specific show and select available seats.
3. Seats are temporarily locked (held) for a user during checkout (soft reservation).
4. Users can complete payment; on success, seats are permanently confirmed and a booking is created.
5. On payment failure or timeout, held seats are released back to available inventory.
6. Users receive a confirmation email and a downloadable PDF ticket with QR code.
7. Admins can create/edit/cancel events, shows, and seat configurations.
8. Waitlist management: users can join a waitlist when a show is sold out; they are notified when seats open.
9. Flash sale support: time-gated sale windows with high burst traffic handling.
10. Users can cancel bookings (subject to cancellation policy); cancelled seats re-enter inventory.
11. Partial refunds are supported per policy; refunds are issued to original payment method.

### Non-Functional Requirements

1. High availability: 99.99% uptime (< 52 min downtime/year).
2. Strong consistency for seat inventory — a seat must never be double-booked.
3. Low latency for seat map reads: p99 < 200 ms.
4. Seat hold/confirm operations: p99 < 500 ms.
5. Throughput: support up to 100,000 concurrent users during a flash sale.
6. PDF ticket generation must complete within 5 seconds of booking confirmation.
7. Idempotent payment APIs to handle retries without double charges.
8. Horizontal scalability with no single point of failure.
9. GDPR/PCI DSS compliance for user and payment data.
10. Observability: end-to-end tracing for every booking transaction.

### Out of Scope

- Building the payment gateway itself (integrate with Stripe/Razorpay).
- Physical ticket printing or courier logistics.
- Fraud detection ML models (assume a third-party fraud service).
- Venue management hardware integrations (turnstiles, scanners).
- Live-streaming of events.
- Mobile app specifics (assume REST/GraphQL API consumed by web and mobile clients).

---

## 2. Users & Scale

### User Types

| Role         | Description                                                         |
|--------------|---------------------------------------------------------------------|
| Guest        | Browse events, view seat maps; cannot book without authentication   |
| Registered   | Full booking, cancellation, waitlist, and history access            |
| Admin        | Create events, manage inventory, set pricing, cancel shows          |
| Operator     | Venue staff using scanning app to validate QR codes at gate         |

### Traffic Estimates

**Assumptions:**
- 50 million registered users total; 5 million daily active users (DAU) — typical booking app DAU ratio of 10%.
- Average user performs 3 page views/session, 1 search, occasionally 1 booking.
- 200,000 bookings/day normal operation; peak flash sale: 20x multiplier = 4,000,000 booking attempts in a 1-hour window.
- Average of 4 tickets per booking.
- Read:Write ratio ≈ 100:1 (browsing heavily dominates).

| Metric                         | Calculation                                                          | Result             |
|--------------------------------|----------------------------------------------------------------------|--------------------|
| Daily Active Users             | 50M * 10%                                                            | 5,000,000          |
| Page views/day                 | 5M users * 3 views                                                   | 15,000,000         |
| Searches/day                   | 5M users * 1 search                                                  | 5,000,000          |
| Normal bookings/day            | Given assumption                                                     | 200,000            |
| Normal booking RPS             | 200,000 / 86,400                                                     | ~2.3 RPS           |
| Peak booking RPS (flash sale)  | 4,000,000 / 3,600                                                    | ~1,111 RPS         |
| Normal read RPS                | 15M / 86,400                                                         | ~174 RPS           |
| Peak read RPS (10x burst)      | 174 * 10                                                             | ~1,740 RPS         |
| Seat hold RPS (flash sale)     | 4M attempts / 3600 * 3 (retries)                                     | ~3,333 RPS         |

### Latency Requirements

| Operation                    | Target p50 | Target p99 |
|------------------------------|------------|------------|
| Event/show listing           | 30 ms      | 150 ms     |
| Seat map rendering           | 50 ms      | 200 ms     |
| Seat hold (lock)             | 80 ms      | 300 ms     |
| Payment initiation           | 100 ms     | 500 ms     |
| Booking confirmation         | 200 ms     | 800 ms     |
| PDF ticket generation        | 1 s        | 5 s        |
| Waitlist notification        | < 2 s      | < 10 s     |

### Storage Estimates

**Assumptions:**
- Average event metadata: 2 KB; seat record: 200 bytes; booking record: 1 KB; ticket PDF: 150 KB.
- 10,000 active events at any time; average 1,000 seats/show; 5 shows/event = 50,000 shows = 50M seat records.
- PDFs stored for 2 years.

| Data Type           | Calculation                                          | Size        |
|---------------------|------------------------------------------------------|-------------|
| Event metadata      | 10,000 events * 2 KB                                 | 20 MB       |
| Seat records        | 50,000 shows * 1,000 seats * 200 B                   | 10 GB       |
| Booking records     | 200,000/day * 365 * 2 years * 1 KB                   | ~146 GB     |
| PDF tickets         | 200,000/day * 365 * 2 years * 150 KB                 | ~21.9 TB    |
| User profiles       | 50M users * 500 B                                    | 25 GB       |
| Total (DB)          | ~180 GB (excluding PDFs)                             | ~180 GB     |
| Total (Object store)| PDF tickets                                          | ~22 TB      |

### Bandwidth Estimates

| Traffic Type         | Calculation                                             | Bandwidth     |
|----------------------|---------------------------------------------------------|---------------|
| Read (seat maps)     | 1,740 RPS * 50 KB average response                      | ~87 MB/s      |
| Write (bookings)     | 2.3 RPS normal * 2 KB                                   | ~4.6 KB/s     |
| Peak write           | 1,111 RPS * 2 KB                                        | ~2.2 MB/s     |
| PDF delivery         | 200,000/day / 86,400 * 150 KB                           | ~347 KB/s avg |
| Total inbound        | ~5 MB/s normal, ~50 MB/s peak                          | —             |
| Total outbound       | ~100 MB/s normal, ~500 MB/s peak (with CDN offload)     | —             |

---

## 3. High-Level Architecture

```
                            ┌──────────────────────────────────────────────────┐
                            │                  CLIENT LAYER                    │
                            │         Web App  /  iOS App  /  Android App      │
                            └────────────────────┬─────────────────────────────┘
                                                 │ HTTPS
                            ┌────────────────────▼─────────────────────────────┐
                            │           CDN (CloudFront / Akamai)               │
                            │   Static assets, event images, cached seat maps   │
                            └────────────────────┬─────────────────────────────┘
                                                 │
                            ┌────────────────────▼─────────────────────────────┐
                            │           API Gateway / Load Balancer             │
                            │     TLS termination, rate limiting, auth JWT      │
                            └──┬──────────────┬──────────────┬──────────────┬──┘
                               │              │              │              │
               ┌───────────────▼──┐  ┌────────▼───────┐  ┌──▼──────────┐  ┌▼──────────────┐
               │  Discovery       │  │  Inventory /   │  │  Booking    │  │  Notification │
               │  Service         │  │  Seat Service  │  │  Service    │  │  Service      │
               │  (event search,  │  │  (hold, release│  │  (confirm,  │  │  (email, SMS, │
               │  browse, show)   │  │   seat map)    │  │  cancel)    │  │  push, PDF)   │
               └───────┬──────────┘  └───────┬────────┘  └─────┬───────┘  └───────┬───────┘
                       │                     │                  │                  │
               ┌───────▼──────────┐  ┌───────▼────────┐  ┌─────▼───────┐  ┌───────▼───────┐
               │  Read Replicas   │  │  Redis Cluster │  │  Payment    │  │  Queue        │
               │  (PostgreSQL)    │  │  (seat locks,  │  │  Service    │  │  (Kafka/SQS)  │
               │  for search      │  │  inventory     │  │  (Stripe    │  │               │
               │  queries         │  │  cache)        │  │  integration│  └───────┬───────┘
               └──────────────────┘  └───────┬────────┘  └─────┬───────┘          │
                                             │                  │           ┌───────▼───────┐
                            ┌────────────────▼──────────────────▼──┐        │  PDF Worker   │
                            │         Primary PostgreSQL DB          │        │  (generate &  │
                            │  (events, shows, seats, bookings,     │        │  upload to S3)│
                            │   users, waitlists — ACID)            │        └───────┬───────┘
                            └───────────────────────────────────────┘                │
                                                                             ┌────────▼──────┐
                                                                             │  S3 / Blob    │
                                                                             │  (PDF tickets,│
                                                                             │  event images)│
                                                                             └───────────────┘
```

**Component Roles:**

- **CDN**: Serves static assets (event posters, seat map SVGs) and caches read-heavy responses (event listings). Reduces origin load by ~80% for reads.
- **API Gateway / Load Balancer**: Entry point for all client traffic. Handles TLS termination, JWT authentication, per-user rate limiting (prevents abuse during flash sales), and routes to appropriate microservices.
- **Discovery Service**: Handles event/show search, browse by city/date/genre. Reads from PostgreSQL read replicas and a search index (Elasticsearch). No write path.
- **Inventory / Seat Service**: The most critical component. Manages seat availability, temporary holds (locks), and seat map state. Interacts with Redis for distributed locking and PostgreSQL for durable state.
- **Booking Service**: Orchestrates the full booking flow — validates hold, initiates payment, confirms booking, publishes events to Kafka. Implements the saga pattern for distributed transactions.
- **Payment Service**: Thin wrapper around Stripe/Razorpay. Handles idempotent charge creation, webhook ingestion for async payment status updates, and refund initiation.
- **Notification Service**: Consumes booking events from Kafka. Sends confirmation emails (SendGrid), SMS (Twilio), and push notifications. Triggers PDF generation job.
- **PDF Worker**: Asynchronous worker pool that generates PDF tickets with QR codes (containing signed booking token) and uploads to S3. Signed S3 URLs are sent to users.
- **Redis Cluster**: Stores seat locks with TTL, rate limit counters, session tokens, and denormalized seat availability bitmaps for fast reads.
- **PostgreSQL (Primary)**: Single source of truth for all transactional data. Uses row-level locking for seat operations.
- **Kafka**: Decouples booking confirmation from downstream side effects (notifications, PDF generation, analytics, waitlist processing).

**Primary Use-Case Data Flow (Booking a Seat):**

1. User opens seat map → API Gateway → Inventory Service → Redis (availability bitmap) → returns seat map in < 200 ms.
2. User selects seats → POST /holds → Inventory Service → Redis SETNX (distributed lock per seat, 10-min TTL) → if acquired, writes a pending hold record to PostgreSQL → returns hold_id.
3. User submits payment → POST /bookings → Booking Service → validates hold is active → calls Payment Service → Stripe charge.
4. Payment webhook (async) → Payment Service → publishes payment_succeeded event to Kafka.
5. Booking Service consumes event → updates PostgreSQL seat status to CONFIRMED → publishes booking_confirmed to Kafka.
6. Notification Service consumes booking_confirmed → sends email + SMS → triggers PDF Worker.
7. PDF Worker generates PDF, uploads to S3, updates booking record with download URL.
8. If payment fails → Booking Service releases Redis lock and sets seat status back to AVAILABLE.

---

## 4. Data Model

### Entities & Schema

```sql
-- Venues
CREATE TABLE venues (
    venue_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            VARCHAR(200) NOT NULL,
    city            VARCHAR(100) NOT NULL,
    country         VARCHAR(100) NOT NULL,
    address         TEXT NOT NULL,
    latitude        DECIMAL(9,6),
    longitude       DECIMAL(9,6),
    total_capacity  INT NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Events (a movie, concert, sports match — the abstract concept)
CREATE TABLE events (
    event_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    title           VARCHAR(300) NOT NULL,
    description     TEXT,
    category        VARCHAR(50) NOT NULL,  -- MOVIE, CONCERT, SPORTS, THEATRE
    language        VARCHAR(50),
    duration_mins   INT,
    rating          VARCHAR(10),           -- PG-13, R, etc.
    poster_url      VARCHAR(500),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Shows (a specific event at a specific venue at a specific time)
CREATE TABLE shows (
    show_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    event_id        UUID NOT NULL REFERENCES events(event_id),
    venue_id        UUID NOT NULL REFERENCES venues(venue_id),
    start_time      TIMESTAMPTZ NOT NULL,
    end_time        TIMESTAMPTZ NOT NULL,
    status          VARCHAR(20) NOT NULL DEFAULT 'SCHEDULED',
                    -- SCHEDULED, ONGOING, COMPLETED, CANCELLED
    total_seats     INT NOT NULL,
    available_seats INT NOT NULL,  -- denormalized for fast reads
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_shows_event_id (event_id),
    INDEX idx_shows_start_time (start_time),
    INDEX idx_shows_venue_id (venue_id)
);

-- Seat Categories (e.g., Platinum, Gold, Silver)
CREATE TABLE seat_categories (
    category_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    show_id         UUID NOT NULL REFERENCES shows(show_id),
    name            VARCHAR(100) NOT NULL,  -- PLATINUM, GOLD, SILVER
    price           DECIMAL(10,2) NOT NULL,
    currency        VARCHAR(3) NOT NULL DEFAULT 'USD',
    total_count     INT NOT NULL,
    available_count INT NOT NULL,
    INDEX idx_seat_cat_show (show_id)
);

-- Individual Seats
CREATE TABLE seats (
    seat_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    show_id         UUID NOT NULL REFERENCES shows(show_id),
    category_id     UUID NOT NULL REFERENCES seat_categories(category_id),
    row_label       VARCHAR(10) NOT NULL,   -- A, B, C...
    seat_number     INT NOT NULL,
    status          VARCHAR(20) NOT NULL DEFAULT 'AVAILABLE',
                    -- AVAILABLE, HELD, CONFIRMED, BLOCKED
    version         INT NOT NULL DEFAULT 0, -- optimistic locking
    UNIQUE (show_id, row_label, seat_number),
    INDEX idx_seats_show_status (show_id, status)
);

-- Users
CREATE TABLE users (
    user_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email           VARCHAR(255) UNIQUE NOT NULL,
    phone           VARCHAR(20),
    name            VARCHAR(200) NOT NULL,
    password_hash   VARCHAR(255) NOT NULL,
    is_verified     BOOLEAN NOT NULL DEFAULT FALSE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Bookings
CREATE TABLE bookings (
    booking_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    show_id         UUID NOT NULL REFERENCES shows(show_id),
    status          VARCHAR(20) NOT NULL DEFAULT 'PENDING',
                    -- PENDING, CONFIRMED, CANCELLED, REFUNDED
    total_amount    DECIMAL(10,2) NOT NULL,
    currency        VARCHAR(3) NOT NULL DEFAULT 'USD',
    payment_id      VARCHAR(255),           -- Stripe payment intent ID
    pdf_url         VARCHAR(500),
    cancellation_reason TEXT,
    cancelled_at    TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_bookings_user (user_id),
    INDEX idx_bookings_show (show_id),
    INDEX idx_bookings_payment (payment_id)
);

-- Booking Seats (which seats belong to which booking)
CREATE TABLE booking_seats (
    id              BIGSERIAL PRIMARY KEY,
    booking_id      UUID NOT NULL REFERENCES bookings(booking_id),
    seat_id         UUID NOT NULL REFERENCES seats(seat_id),
    price_paid      DECIMAL(10,2) NOT NULL,
    UNIQUE (seat_id),  -- a seat can belong to only one confirmed booking
    INDEX idx_bseats_booking (booking_id)
);

-- Seat Holds (temporary locks during checkout)
CREATE TABLE seat_holds (
    hold_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    show_id         UUID NOT NULL REFERENCES shows(show_id),
    seat_ids        UUID[] NOT NULL,       -- array of seat UUIDs
    expires_at      TIMESTAMPTZ NOT NULL,  -- now() + 10 minutes
    booking_id      UUID REFERENCES bookings(booking_id),  -- filled on confirm
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_holds_user (user_id),
    INDEX idx_holds_expires (expires_at)   -- for TTL cleanup job
);

-- Waitlist
CREATE TABLE waitlist (
    waitlist_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    show_id         UUID NOT NULL REFERENCES shows(show_id),
    category_id     UUID REFERENCES seat_categories(category_id),
    seat_count      INT NOT NULL DEFAULT 1,
    status          VARCHAR(20) NOT NULL DEFAULT 'WAITING',
                    -- WAITING, NOTIFIED, CONVERTED, EXPIRED
    notified_at     TIMESTAMPTZ,
    expires_at      TIMESTAMPTZ NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (user_id, show_id),
    INDEX idx_waitlist_show (show_id, status),
    INDEX idx_waitlist_expires (expires_at)
);

-- Cancellation Policies
CREATE TABLE cancellation_policies (
    policy_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    event_id        UUID NOT NULL REFERENCES events(event_id),
    hours_before    INT NOT NULL,    -- hours before show start
    refund_percent  DECIMAL(5,2) NOT NULL,  -- 0.00 to 100.00
    INDEX idx_cp_event (event_id)
);
```

### Database Choice

**Options Considered:**

| Database         | Pros                                                                         | Cons                                                                 |
|------------------|------------------------------------------------------------------------------|----------------------------------------------------------------------|
| PostgreSQL       | ACID transactions, row-level locking, mature ecosystem, JSONB support        | Single-node write throughput ceiling; sharding requires extra work   |
| MySQL (InnoDB)   | ACID, row-level locking, wide hosting support                                | Less powerful query planner vs PostgreSQL; no native array types     |
| CockroachDB      | Distributed ACID, horizontal scale, Postgres-compatible                      | Higher write latency (Raft consensus); operational complexity        |
| MongoDB          | Flexible schema, horizontal scale                                            | No multi-document ACID until v4.0+; not ideal for relational data    |
| Cassandra        | High write throughput, linear scalability                                    | Eventual consistency; no native transactions; complex seat locking   |

**Selected: PostgreSQL with read replicas and Redis for locking**

Justification:
1. Seat booking is fundamentally a transactional problem. The seat table requires `SELECT ... FOR UPDATE` row-level locking to prevent double booking — PostgreSQL's MVCC and row-level lock implementation is battle-tested for exactly this pattern.
2. The data model is highly relational (event → shows → seats → bookings → users). JOINs and foreign key constraints are first-class in PostgreSQL.
3. `available_seats` counter in the `shows` table is updated atomically using `UPDATE ... SET available_seats = available_seats - 1 WHERE available_seats > 0` which PostgreSQL handles efficiently.
4. Optimistic locking via the `version` column on `seats` prevents lost updates during concurrent holds.
5. Redis is layered in front for the seat hold TTL and distributed lock (SETNX + EXPIRE) to avoid hammering the DB with lock-check reads. Redis acts as the first gate; PostgreSQL is the durable source of truth.
6. For the scale described (~180 GB of transactional data), a single primary PostgreSQL instance with 3 read replicas is sufficient. If write throughput becomes a bottleneck at truly massive scale, CockroachDB is the migration path (it is Postgres-wire-compatible).

---

## 5. API Design

All endpoints require `Authorization: Bearer <JWT>` unless noted. Rate limits enforced at API Gateway per user_id (authenticated) or IP (unauthenticated).

```
BASE URL: https://api.bookmyshow.example.com/v1
```

### Events & Discovery

```
GET /events
  Query: city, category, date_from, date_to, page (default 1), limit (default 20, max 50)
  Auth: Optional (personalization requires auth)
  Rate limit: 200 req/min/IP
  Response 200:
  {
    "data": [{ "event_id", "title", "category", "shows_count", "min_price", "poster_url" }],
    "pagination": { "page", "limit", "total", "next_cursor" }
  }

GET /events/{event_id}
  Auth: Optional
  Rate limit: 200 req/min/IP
  Response 200:
  {
    "event_id", "title", "description", "category", "rating",
    "shows": [{ "show_id", "venue", "start_time", "available_seats", "categories": [{ "name", "price", "available" }] }]
  }

GET /shows/{show_id}/seatmap
  Auth: Optional
  Rate limit: 100 req/min/user
  Response 200:
  {
    "show_id", "venue_layout": { "rows": [{ "label": "A", "seats": [{ "seat_id", "number", "status", "category", "price" }] }] }
  }
  Note: status values: AVAILABLE | HELD | CONFIRMED | BLOCKED
```

### Seat Holds

```
POST /holds
  Auth: Required
  Rate limit: 10 req/min/user (prevents hold squatting)
  Body: { "show_id": "uuid", "seat_ids": ["uuid", "uuid"] }
  Validation: max 10 seats per hold; seats must be AVAILABLE; user must not have an active hold for this show
  Response 201:
  {
    "hold_id": "uuid",
    "seat_ids": ["uuid"],
    "expires_at": "ISO8601",
    "total_amount": 150.00,
    "currency": "USD"
  }
  Error 409: { "error": "SEAT_UNAVAILABLE", "conflicting_seats": ["uuid"] }
  Error 429: { "error": "RATE_LIMIT_EXCEEDED", "retry_after": 60 }

DELETE /holds/{hold_id}
  Auth: Required (must be hold owner)
  Response 204: No content (seats released immediately)
```

### Bookings

```
POST /bookings
  Auth: Required
  Rate limit: 5 req/min/user
  Body: { "hold_id": "uuid", "payment_method_id": "stripe_pm_xxx" }
  Idempotency: Idempotency-Key header required (UUID v4 from client)
  Response 202:
  {
    "booking_id": "uuid",
    "status": "PENDING",
    "payment_intent_id": "pi_xxx",
    "client_secret": "pi_xxx_secret_yyy"  // Stripe client secret for 3DS
  }
  Error 400: { "error": "HOLD_EXPIRED" }
  Error 400: { "error": "HOLD_NOT_FOUND" }

GET /bookings/{booking_id}
  Auth: Required (must be booking owner or admin)
  Response 200:
  {
    "booking_id", "status", "show": { ... }, "seats": [{ "row", "number", "category" }],
    "total_amount", "currency", "pdf_url", "created_at"
  }

GET /bookings
  Auth: Required
  Query: status, page, limit (max 50)
  Response 200: { "data": [...bookings], "pagination": { ... } }

DELETE /bookings/{booking_id}
  Auth: Required (must be booking owner)
  Body: { "reason": "optional string" }
  Response 200:
  {
    "booking_id", "status": "CANCELLED",
    "refund": { "amount": 120.00, "currency": "USD", "estimated_days": 5 }
  }
  Error 400: { "error": "CANCELLATION_NOT_ALLOWED", "reason": "Show starts in < 2 hours" }
```

### Waitlist

```
POST /waitlist
  Auth: Required
  Body: { "show_id": "uuid", "category_id": "uuid", "seat_count": 2 }
  Response 201: { "waitlist_id", "position": 14, "expires_at" }
  Error 409: { "error": "ALREADY_ON_WAITLIST" }
  Error 400: { "error": "SHOW_HAS_AVAILABLE_SEATS" }

DELETE /waitlist/{waitlist_id}
  Auth: Required
  Response 204
```

### Webhooks (inbound from Stripe)

```
POST /webhooks/stripe
  Auth: Stripe-Signature header (HMAC-SHA256 verification)
  Body: Stripe event object
  Handles: payment_intent.succeeded, payment_intent.payment_failed,
           charge.refunded
  Response 200: { "received": true }
  Note: Idempotent — duplicate webhook events are safely ignored using payment_id dedup
```

---

## 6. Deep Dive: Core Components

### 6.1 Seat Locking & Concurrency Control

**Problem it solves:**
When 1,000 users simultaneously view an available seat and attempt to book it, the system must guarantee exactly one succeeds — zero double bookings. The window between "read availability" and "write confirmation" is a classic TOCTOU (Time-of-Check-Time-of-Use) race condition.

**Approaches Comparison:**

| Approach                      | Mechanism                                    | Pros                                    | Cons                                                          |
|-------------------------------|----------------------------------------------|-----------------------------------------|---------------------------------------------------------------|
| Pessimistic DB lock           | `SELECT FOR UPDATE` on seat row              | Simple, guaranteed serialization        | DB connection held during checkout; bottleneck at scale       |
| Optimistic locking            | version column, fail on mismatch             | No lock held; high read throughput      | High contention means many retries; poor for flash sales      |
| Redis SETNX (distributed lock)| Atomic SET if not exists with TTL            | Millisecond latency; off-loads DB       | Redis failure = lost locks; requires Redlock for HA           |
| Database queue (serialized)   | Single-consumer queue per show               | Guaranteed ordering                     | Throughput bottleneck; complex to build                       |
| Redis + DB two-phase          | Redis lock first, then DB confirm            | Fast path in Redis; durability in DB    | Two round trips; consistency risk if Redis and DB diverge     |

**Selected: Redis SETNX with TTL as first gate + PostgreSQL row-level lock as durability gate**

Detailed reasoning:
- Redis SETNX for seat `seat:<seat_id>:lock` with 10-minute TTL handles the high-frequency "is this seat available?" gate at microsecond latency without touching the database for every concurrent user.
- Only when the Redis lock is acquired does the system proceed to PostgreSQL with `SELECT ... FOR UPDATE` to atomically update the seat status. This two-phase approach means the DB lock is held for milliseconds (just the UPDATE), not the entire 10-minute checkout window.
- For high availability of Redis locks, we use Redlock across 3 independent Redis nodes — the lock is considered acquired only if a majority (2 of 3) respond successfully. This prevents split-brain scenarios.
- TTL of 10 minutes automatically releases abandoned holds (user closed browser) without a cleanup job needing to run in the critical path.

**Implementation (Seat Hold Algorithm):**

```python
def hold_seats(user_id: str, show_id: str, seat_ids: list[str]) -> HoldResult:
    # Step 1: Validate inputs
    if len(seat_ids) == 0 or len(seat_ids) > 10:
        raise ValidationError("Must select 1-10 seats")

    # Check user doesn't already have an active hold for this show
    existing_hold = db.query(
        "SELECT hold_id FROM seat_holds WHERE user_id = $1 AND show_id = $2 AND expires_at > now()",
        [user_id, show_id]
    )
    if existing_hold:
        raise ConflictError("User already has an active hold for this show")

    # Step 2: Acquire Redis locks for all requested seats atomically
    # Sort seat_ids to prevent deadlock (consistent lock ordering)
    sorted_seats = sorted(seat_ids)
    lock_keys = [f"seat:{seat_id}:lock" for seat_id in sorted_seats]
    acquired_locks = []
    lock_value = f"{user_id}:{uuid4()}"  # unique value to identify this lock

    try:
        for key in lock_keys:
            # SETNX with 10-minute TTL
            acquired = redis.set(key, lock_value, nx=True, ex=600)
            if not acquired:
                # Check if seat is confirmed (not just held by someone else)
                raise SeatUnavailableError(f"Seat {key} is not available")
            acquired_locks.append(key)

        # Step 3: Begin DB transaction to durably record the hold
        with db.transaction():
            # Lock the seat rows (short-lived DB lock)
            seats = db.query(
                """SELECT seat_id, status, version FROM seats
                   WHERE seat_id = ANY($1) AND show_id = $2
                   FOR UPDATE""",
                [seat_ids, show_id]
            )

            # Verify all seats are AVAILABLE in DB (Redis is the fast path;
            # DB is the authoritative source)
            unavailable = [s for s in seats if s.status != 'AVAILABLE']
            if unavailable:
                raise SeatUnavailableError(unavailable)

            # Update seat statuses to HELD
            db.execute(
                "UPDATE seats SET status = 'HELD', version = version + 1 WHERE seat_id = ANY($1)",
                [seat_ids]
            )

            # Create hold record
            hold_id = uuid4()
            expires_at = now() + timedelta(minutes=10)
            db.execute(
                """INSERT INTO seat_holds (hold_id, user_id, show_id, seat_ids, expires_at)
                   VALUES ($1, $2, $3, $4, $5)""",
                [hold_id, user_id, show_id, seat_ids, expires_at]
            )

            # Update available_seats count
            db.execute(
                "UPDATE shows SET available_seats = available_seats - $1 WHERE show_id = $2",
                [len(seat_ids), show_id]
            )

        return HoldResult(hold_id=hold_id, expires_at=expires_at, seat_ids=seat_ids)

    except Exception as e:
        # Release any acquired Redis locks on failure
        for key in acquired_locks:
            # Only release if we own the lock (check lock_value)
            release_lock_if_owner(redis, key, lock_value)
        raise

def release_hold(hold_id: str, user_id: str):
    hold = db.query("SELECT * FROM seat_holds WHERE hold_id = $1 AND user_id = $2", [hold_id, user_id])
    if not hold or hold.expires_at < now():
        return  # Already expired or doesn't exist

    with db.transaction():
        db.execute("UPDATE seats SET status = 'AVAILABLE' WHERE seat_id = ANY($1)", [hold.seat_ids])
        db.execute("UPDATE shows SET available_seats = available_seats + $1 WHERE show_id = $2",
                   [len(hold.seat_ids), hold.show_id])
        db.execute("DELETE FROM seat_holds WHERE hold_id = $1", [hold_id])

    # Release Redis locks
    lock_value_pattern = f"*:{user_id}:*"  # stored lock_value includes user_id
    for seat_id in hold.seat_ids:
        redis.delete(f"seat:{seat_id}:lock")

    # Notify waitlist
    publish_to_kafka("seats.released", { "show_id": hold.show_id, "count": len(hold.seat_ids) })
```

**Expired Hold Cleanup (background job):**
```sql
-- Runs every 30 seconds
UPDATE seats SET status = 'AVAILABLE', version = version + 1
WHERE seat_id IN (
    SELECT unnest(seat_ids) FROM seat_holds
    WHERE expires_at < now() AND booking_id IS NULL
);

UPDATE shows s
SET available_seats = available_seats + (
    SELECT count(*) FROM seat_holds sh
    CROSS JOIN unnest(sh.seat_ids) sid
    WHERE sh.show_id = s.show_id AND sh.expires_at < now() AND sh.booking_id IS NULL
)
WHERE show_id IN (SELECT DISTINCT show_id FROM seat_holds WHERE expires_at < now());

DELETE FROM seat_holds WHERE expires_at < now() AND booking_id IS NULL;
```

**Interviewer Q&A:**

Q1: What happens if the Redis lock is acquired but the database transaction fails?
A: The `try/except` block in the hold flow always releases Redis locks on any exception. Since Redis has the TTL as a safety net anyway, even if the exception handler itself fails (e.g., Redis connection drops), the lock expires automatically in 10 minutes. The seat status in the DB was never changed (transaction rolled back), so consistency is maintained.

Q2: Why use sorted seat_ids before acquiring Redis locks?
A: Consistent lock ordering prevents deadlock. If user A tries to lock seats [1,2] and user B tries [2,1], and both acquire their first lock simultaneously, they deadlock waiting for the second. Sorting ensures both acquire locks in the same order.

Q3: How do you handle the case where Redis goes down entirely?
A: With Redlock across 3 independent Redis nodes, the system can tolerate one node failure. If all Redis nodes are unavailable, the system falls back to pure PostgreSQL pessimistic locking (`SELECT FOR UPDATE`). The fallback adds ~50ms latency but maintains correctness. A circuit breaker in the Inventory Service detects Redis unavailability and routes to the fallback path.

Q4: Why is the DB lock held for only milliseconds instead of the full 10 minutes?
A: The `SELECT ... FOR UPDATE` inside the transaction is released as soon as the transaction commits (after the UPDATE and INSERT). The PostgreSQL lock is only needed to prevent two concurrent hold requests from both seeing `status = 'AVAILABLE'` simultaneously. Redis handles the longer "seat is held during checkout" window. This design allows the DB to serve thousands of concurrent transactions.

Q5: How would you handle a user who holds seats but never pays? What prevents hold abuse?
A: Three mechanisms: (1) 10-minute TTL auto-releases. (2) Rate limiting holds to 10/min/user at the API Gateway. (3) A user can only have one active hold per show (enforced in the hold creation logic). For persistent abusers, the anomaly detection service (consuming Kafka events) can flag and temporarily ban users who repeatedly hold-and-abandon.

---

### 6.2 Payment Integration & Booking Saga

**Problem it solves:**
Payment is an external, asynchronous operation that can fail at multiple stages (network timeout, card decline, 3D Secure challenge, webhook delay). The booking must be in a consistent state regardless of outcome — seats must never be confirmed without payment, and payment must never be charged without seat confirmation.

**Approaches Comparison:**

| Approach                        | Description                                               | Pros                                    | Cons                                              |
|---------------------------------|-----------------------------------------------------------|-----------------------------------------|---------------------------------------------------|
| Synchronous two-phase commit    | DB + payment gateway in single XA transaction             | Atomic                                  | Payment gateways don't support XA; impractical    |
| Choreography-based saga         | Services react to events without central coordinator      | Loose coupling; resilient                | Hard to reason about failure paths; complex debug |
| Orchestration-based saga        | Central Booking Service coordinates steps with rollbacks  | Clear failure handling; single owner    | Booking Service becomes critical path             |
| Outbox pattern + polling        | Write events to DB outbox table, relay to Kafka           | No message loss; at-least-once delivery | Small added latency; polling overhead             |

**Selected: Orchestration-based saga with outbox pattern**

The Booking Service acts as the saga orchestrator:
1. Create booking record (status: PENDING) + outbox event in same DB transaction.
2. Outbox relay publishes `initiate_payment` to Kafka.
3. Payment Service charges card, receives Stripe webhook.
4. On success: publish `payment_succeeded`; Booking Service updates to CONFIRMED.
5. On failure: publish `payment_failed`; Booking Service triggers compensating transaction (release seats).

**Implementation (Booking Confirmation Flow):**

```python
def create_booking(user_id: str, hold_id: str, payment_method_id: str,
                   idempotency_key: str) -> BookingResult:
    # Idempotency check: if we've seen this key, return cached result
    cached = idempotency_store.get(idempotency_key)
    if cached:
        return cached

    hold = db.query(
        "SELECT * FROM seat_holds WHERE hold_id = $1 AND user_id = $2 FOR UPDATE",
        [hold_id, user_id]
    )
    if not hold:
        raise NotFoundError("Hold not found")
    if hold.expires_at < now():
        raise BadRequestError("Hold has expired")

    # Calculate total amount
    seats_with_prices = db.query(
        """SELECT s.seat_id, sc.price FROM seats s
           JOIN seat_categories sc USING (category_id)
           WHERE s.seat_id = ANY($1)""",
        [hold.seat_ids]
    )
    total_amount = sum(s.price for s in seats_with_prices)

    with db.transaction():
        # Create booking
        booking_id = uuid4()
        db.execute(
            """INSERT INTO bookings (booking_id, user_id, show_id, status, total_amount, currency)
               VALUES ($1, $2, $3, 'PENDING', $4, 'USD')""",
            [booking_id, user_id, hold.show_id, total_amount]
        )

        # Link hold to booking
        db.execute(
            "UPDATE seat_holds SET booking_id = $1 WHERE hold_id = $2",
            [booking_id, hold_id]
        )

        # Write to outbox (same transaction = no message loss)
        db.execute(
            """INSERT INTO outbox (event_type, payload, created_at)
               VALUES ('initiate_payment', $1, now())""",
            [json.dumps({
                "booking_id": str(booking_id),
                "user_id": user_id,
                "amount": str(total_amount),
                "currency": "USD",
                "payment_method_id": payment_method_id,
                "idempotency_key": idempotency_key
            })]
        )

    # Store idempotency result
    result = BookingResult(booking_id=booking_id, status="PENDING")
    idempotency_store.set(idempotency_key, result, ex=86400)  # 24h cache

    return result

# Payment Service: called by outbox relay
def charge_card(booking_id: str, amount: Decimal, payment_method_id: str,
                idempotency_key: str):
    # Create Stripe PaymentIntent (idempotent via Stripe idempotency key)
    intent = stripe.PaymentIntent.create(
        amount=int(amount * 100),  # Stripe uses cents
        currency="usd",
        payment_method=payment_method_id,
        confirm=True,
        idempotency_key=f"pi_{idempotency_key}"
    )
    # Store payment_intent.id in bookings table
    db.execute(
        "UPDATE bookings SET payment_id = $1 WHERE booking_id = $2",
        [intent.id, booking_id]
    )

# Webhook handler (called by Stripe)
def handle_stripe_webhook(event: StripeEvent):
    match event.type:
        case "payment_intent.succeeded":
            booking_id = event.data.object.metadata.get("booking_id")
            confirm_booking(booking_id)

        case "payment_intent.payment_failed":
            booking_id = event.data.object.metadata.get("booking_id")
            fail_booking(booking_id)

def confirm_booking(booking_id: str):
    with db.transaction():
        booking = db.query(
            "SELECT * FROM bookings WHERE booking_id = $1 AND status = 'PENDING' FOR UPDATE",
            [booking_id]
        )
        if not booking:
            return  # Idempotent: already confirmed or doesn't exist

        # Confirm all seats
        hold = db.query(
            "SELECT * FROM seat_holds WHERE booking_id = $1", [booking_id]
        )
        db.execute(
            "UPDATE seats SET status = 'CONFIRMED' WHERE seat_id = ANY($1)",
            [hold.seat_ids]
        )
        # Create booking_seats records
        for seat_id, price in zip(hold.seat_ids, hold.prices):
            db.execute(
                "INSERT INTO booking_seats (booking_id, seat_id, price_paid) VALUES ($1, $2, $3)",
                [booking_id, seat_id, price]
            )
        db.execute(
            "UPDATE bookings SET status = 'CONFIRMED' WHERE booking_id = $1", [booking_id]
        )
        # Outbox for notifications
        db.execute(
            "INSERT INTO outbox (event_type, payload) VALUES ('booking_confirmed', $1)",
            [json.dumps({"booking_id": str(booking_id)})]
        )

def fail_booking(booking_id: str):
    with db.transaction():
        hold = db.query(
            "SELECT * FROM seat_holds WHERE booking_id = $1", [booking_id]
        )
        # Compensating transaction: release seats
        db.execute(
            "UPDATE seats SET status = 'AVAILABLE' WHERE seat_id = ANY($1)",
            [hold.seat_ids]
        )
        db.execute(
            "UPDATE shows SET available_seats = available_seats + $1 WHERE show_id = $2",
            [len(hold.seat_ids), hold.show_id]
        )
        db.execute(
            "UPDATE bookings SET status = 'FAILED' WHERE booking_id = $1", [booking_id]
        )
        db.execute("DELETE FROM seat_holds WHERE booking_id = $1", [booking_id])

    # Notify waitlist seats are available again
    publish_to_kafka("seats.released", {"show_id": hold.show_id, "count": len(hold.seat_ids)})
```

**Interviewer Q&A:**

Q1: What if the Stripe webhook never arrives? How do you prevent bookings stuck in PENDING?
A: A scheduled job runs every 5 minutes and queries Stripe for the status of all PaymentIntents whose bookings have been PENDING for more than 10 minutes: `stripe.PaymentIntent.retrieve(payment_id)`. If the status is `succeeded`, it calls `confirm_booking`; if `failed`, it calls `fail_booking`. This reconciliation job is the safety net for missed webhooks.

Q2: How do you prevent a double charge if the client retries the POST /bookings request?
A: The `Idempotency-Key` header (UUID v4 from client) is checked against a Redis idempotency store at the top of the handler. On the first call, the result is stored in Redis with a 24-hour TTL. Subsequent calls with the same key return the cached result without executing any logic. Additionally, Stripe's own idempotency key (passed as `pi_{idempotency_key}`) prevents duplicate PaymentIntents at the gateway level.

Q3: What happens if the Booking Service crashes between writing the booking record and writing to the outbox?
A: Both are written in a single PostgreSQL transaction. If the service crashes before the transaction commits, the booking record does not exist — no orphaned state. If it crashes after commit, the outbox record exists and the outbox relay will pick it up on next poll (within seconds). The saga moves forward correctly.

Q4: How do you handle partial booking scenarios — some seats confirmed, some failed?
A: The saga treats the entire hold as a single unit. All seats in a hold either confirm together or fail together. The `confirm_booking` function updates all `seat_ids` from the hold in a single transaction. There is no partial confirmation.

Q5: How would you implement a refund on cancellation?
A: `DELETE /bookings/{booking_id}` invokes a cancellation saga: (1) check cancellation policy (hours before show → refund percentage); (2) call `stripe.Refund.create(charge=charge_id, amount=refund_amount)` idempotently; (3) update booking status to CANCELLED; (4) set seats back to AVAILABLE; (5) publish `seats.released` event to wake up waitlist. The refund is processed by Stripe and credited to the user's payment method in 5-10 business days.

---

### 6.3 Flash Sale Handling

**Problem it solves:**
A flash sale for a popular event (e.g., Taylor Swift concert) causes 100x normal traffic in seconds. Without special handling, the system either serves incorrect availability data, crashes under DB overload, or allows overselling.

**Approaches Comparison:**

| Approach                     | Mechanism                                           | Pros                               | Cons                                          |
|------------------------------|-----------------------------------------------------|------------------------------------|-----------------------------------------------|
| No special handling          | Normal flow for all traffic                         | Simple                             | DB overload; cascading failures               |
| Request queuing (FIFO)       | All booking requests enter a queue                  | Fair; prevents overload            | High latency; complex UX (position polling)   |
| Token bucket / admission     | Pre-issue tokens; only token holders can book       | Very controlled; prevents thundering herd | Token distribution is its own hard problem |
| Inventory pre-sharding       | Distribute inventory across N shards                | High write throughput              | Inventory reconciliation complexity           |
| Virtual waiting room         | Queue users before event page opens; rate-admit     | Excellent UX; industry standard    | Requires dedicated waiting room service       |

**Selected: Virtual waiting room + Redis inventory counter**

Implementation:
1. 15 minutes before flash sale opens, redirect all users to a waiting room page (served from CDN).
2. At sale open time, issue each waiting user a randomized position token (UUID) via WebSocket push.
3. Tokens are processed in batches of 500/second, admitted to the booking flow in order.
4. Inside the booking flow, seat availability is served from a Redis inventory counter (`DECR show:{show_id}:available`) before any DB interaction. `DECR` is atomic in Redis — if it returns < 0, immediately increment back and return "sold out" to the user, no DB load incurred.
5. Only requests that successfully decrement the Redis counter proceed to the full hold flow.

```python
def admit_to_booking_flow(show_id: str, seat_count: int) -> bool:
    """Atomic Redis DECR as first gate for flash sales."""
    key = f"show:{show_id}:available"
    new_count = redis.decrby(key, seat_count)
    if new_count < 0:
        redis.incrby(key, seat_count)  # compensate
        return False  # Sold out
    return True  # Proceed to hold flow

def initialize_flash_sale_inventory(show_id: str, available_seats: int):
    """Called by admin when activating flash sale mode."""
    redis.set(f"show:{show_id}:available", available_seats)
    redis.set(f"show:{show_id}:flash_sale_mode", "1")
```

---

## 7. Scaling

### Horizontal Scaling

- **API Tier**: Stateless services deployed in Kubernetes. Horizontal Pod Autoscaler (HPA) triggers on CPU > 70% and custom metric: booking_rps > 500. Minimum 3 replicas per service for HA, scaling to 50+ during flash sales.
- **Discovery Service**: Entirely read-path; scales independently to handle browse traffic without affecting booking capacity.
- **Notification Service**: Scales based on Kafka consumer lag. More consumers = faster notification processing.
- **PDF Workers**: Autoscale based on SQS queue depth. Bursty load handled by Lambda/Fargate spot instances.

### Database Sharding

At current scale (~180 GB, ~2,300 write RPS peak), a single PostgreSQL primary with connection pooling (PgBouncer, 10,000 pool size) handles the load. Sharding strategy for future scale:

- **Shard key: `show_id`** — all seats, holds, and bookings for a show live on the same shard, enabling JOIN-free queries and transactional operations within a single shard.
- **Shard routing**: Consistent hashing of `show_id` to one of N shards. A shard map stored in Zookeeper/etcd is consulted on each request.
- **Cross-shard queries** (e.g., "get all bookings for user X"): handled by fan-out queries to all shards + aggregation, or by maintaining a secondary `user_bookings` table sharded by `user_id`.
- **Global tables** (users, events, venues) replicated to all shards as read-only data.

### Replication

- **PostgreSQL**: 1 primary + 3 read replicas (streaming replication, synchronous for 1 replica to ensure RPO = 0 on failover). Read replicas serve Discovery Service queries, reducing primary load by ~70%.
- **Redis**: Redis Cluster with 3 master nodes + 3 replica nodes. Each seat lock key is assigned to a master shard. Redlock uses 3 independent Redis deployments (not cluster nodes) for lock acquisition.

### Caching Strategy

| Cache Layer       | What is Cached                          | TTL         | Invalidation                          |
|-------------------|-----------------------------------------|-------------|---------------------------------------|
| CDN (CloudFront)  | Event listings, event detail pages      | 60 seconds  | Admin API triggers cache purge        |
| Application cache | Venue details, event metadata           | 5 minutes   | TTL expiry                            |
| Redis             | Seat availability bitmaps               | 30 seconds  | Invalidated on any seat status change |
| Redis             | Seat holds (locks)                      | 10 minutes  | Explicit release or TTL               |
| Redis             | Flash sale inventory counter            | Duration of sale | Real-time DECR; synced to DB post-sale |

Seat map is the hottest read. The full seat map for a show is serialized to Redis as a hash (`HSET show:{show_id}:seats seat_id status`) on show creation. Individual seat status updates (`HSET show:{show_id}:seats {seat_id} HELD`) are O(1). The seat map endpoint reads entirely from Redis — PostgreSQL is only consulted if the Redis key is missing (cold start or TTL expired).

### CDN

- All static assets (JS, CSS, images, seat map SVG templates) served from CloudFront with 24-hour TTL.
- Event listing pages with low personalization are edge-cached for 60 seconds.
- PDF tickets served via CloudFront with signed URLs (15-minute expiry on the signed URL).
- Flash sale waiting room page is a static HTML page served purely from CDN — zero origin load during the pre-sale queue period.

**Interviewer Q&A:**

Q1: How does the system handle a Kubernetes node failure during peak traffic?
A: Kubernetes auto-reschedules pods from the failed node to healthy nodes within 30-60 seconds (pod disruption budget ensures minimum 2 replicas always running). The API Gateway's health checks remove the failed node from rotation within seconds. Active HTTP requests in flight are lost (clients retry), but no data corruption occurs because operations are idempotent and the DB saga is the source of truth.

Q2: How would you scale seat map reads to handle 50,000 concurrent users viewing the same show's seat map?
A: Redis serves the seat map as a hash. With a single Redis cluster node handling ~100,000 ops/second, this is well within capacity. For a truly viral event, we can add a local in-process cache (LRU, 1-second TTL) in each API pod — since seat map data is acceptable to be 1 second stale. This reduces Redis load proportionally to pod count.

Q3: What is your sharding strategy for the `bookings` table which needs to be queried both by show and by user?
A: Shard by `show_id` for transactional operations (booking creation, seat confirmation). Maintain a separate `user_bookings_index` table sharded by `user_id` that stores only `(user_id, booking_id, show_id, created_at)` — sufficient to support "list my bookings" queries without cross-shard joins. The index is updated via the outbox/Kafka pipeline when bookings are confirmed.

Q4: How do you handle database connection exhaustion during a flash sale?
A: PgBouncer connection pooling sits in front of PostgreSQL. PgBouncer maintains a pool of 100 server-side connections to PostgreSQL, multiplexing thousands of application connections. The Inventory Service additionally uses Redis as the first gate (decrement counter), so the vast majority of "sold out" responses are served without any DB connection. Only successful seat hold attempts reach the DB.

Q5: Describe your disaster recovery strategy if the entire primary region goes down.
A: RTO: 5 minutes, RPO: 0 (synchronous replica in secondary region). Architecture: primary region (us-east-1) with synchronous streaming replication to a standby PostgreSQL in us-west-2. Route 53 health checks detect primary failure and trigger DNS failover to the secondary region within 60 seconds. Redis is warmed from DB on failover. Kafka uses multi-region replication (MirrorMaker 2). A runbook automates promotion of the standby to primary. Active-active would be preferred at scale but requires conflict resolution — out of scope for this design.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component          | Failure Mode                                | Impact                                      | Mitigation                                                                 |
|--------------------|---------------------------------------------|---------------------------------------------|----------------------------------------------------------------------------|
| Redis (all nodes)  | All Redis nodes unreachable                 | Seat holds cannot be issued                 | Fallback to PostgreSQL pessimistic locking; alert + auto-recovery           |
| PostgreSQL primary | Primary DB crash                            | All writes fail                             | Automatic failover to sync replica in < 30 s via Patroni                   |
| Payment Service    | Stripe API unavailable                      | New bookings fail                           | Queue payment requests; retry with exponential backoff; user shown status  |
| Kafka              | Broker failure                              | Notifications/PDF delayed, not lost         | Kafka replication factor 3; ISR ensures at-least-once delivery             |
| Notification Svc   | Email provider down                         | Confirmation emails delayed                 | Retry queue with exponential backoff; fallback to SMS                      |
| PDF Worker         | Worker crash mid-generation                 | PDF not generated                           | Kafka consumer group: another worker picks up message on restart           |
| API Gateway        | Pod crash                                   | Brief request failures                      | 3+ replicas; health checks; circuit breaker                                |
| Hold expiry job    | Job fails to run                            | Stale holds occupy seats                    | Job is idempotent; re-runs catch up; Redis TTL is independent safety net   |

### Failover

- **Patroni** manages PostgreSQL HA. Patroni + etcd provides automatic leader election and promotion in < 30 seconds on primary failure.
- All services implement **retry with exponential backoff and jitter** (initial delay 100ms, max 5 retries, max delay 16s, ±20% jitter).
- **Idempotency** on all write operations ensures retries do not cause duplicate effects.

### Circuit Breaker

Implemented using the Hystrix/Resilience4j pattern on all inter-service calls:
- **Threshold**: Open circuit after 5 consecutive failures or 50% failure rate in a 10-second window.
- **Half-open state**: Allow 1 request every 30 seconds to probe recovery.
- **Fallback responses**: Payment Service circuit open → return `"service_unavailable"` to Booking Service, which queues the booking for retry rather than failing immediately.

---

## 9. Monitoring & Observability

### Metrics

| Metric Name                          | Type      | Alert Threshold                        | Dashboard         |
|--------------------------------------|-----------|----------------------------------------|-------------------|
| `booking.hold.latency_p99`           | Histogram | > 500 ms                               | Booking SLO       |
| `booking.confirmation.latency_p99`   | Histogram | > 800 ms                               | Booking SLO       |
| `seat.double_booking.count`          | Counter   | > 0 (any occurrence = critical alert)  | Data Integrity    |
| `payment.success_rate`               | Gauge     | < 95%                                  | Revenue           |
| `redis.hold_lock.acquisition_rate`   | Gauge     | < 80% during flash sale                | Flash Sale        |
| `kafka.consumer.lag`                 | Gauge     | > 10,000 messages                      | Async Processing  |
| `db.connection_pool.wait_time`       | Histogram | > 100 ms                               | DB Health         |
| `show.available_seats.sync_delta`    | Gauge     | > 5 seats drift between Redis and DB   | Inventory Sync    |
| `pdf.generation.latency_p99`         | Histogram | > 5 s                                  | Ticket Delivery   |
| `api.error_rate_5xx`                 | Gauge     | > 1% of requests                       | API Health        |

### Distributed Tracing

All requests carry a `trace_id` (W3C TraceContext format) propagated through all service-to-service calls and Kafka message headers. OpenTelemetry SDK is embedded in every service. Traces are exported to Jaeger/Honeycomb. A complete booking trace spans: API Gateway → Inventory Service → Redis → PostgreSQL → Kafka → Payment Service → Stripe → Webhook → Booking Service → PostgreSQL → Kafka → Notification Service → PDF Worker → S3.

Critical spans instrumented:
- `redis.setnx` (seat lock acquisition time)
- `postgres.transaction` (hold/confirm transaction duration)
- `stripe.payment_intent.create` (external latency)
- `pdf.generate` (rendering time per ticket)

### Logging

- Structured JSON logs with fields: `trace_id`, `span_id`, `user_id`, `booking_id`, `show_id`, `operation`, `duration_ms`, `status`.
- Log levels: DEBUG (dev), INFO (prod standard), WARN (retry/degraded), ERROR (failures).
- No PII in logs (user_id is a UUID, not email; credit card numbers never logged). PCI DSS compliance requires this.
- Logs shipped to Elasticsearch via Fluentd. Kibana dashboards for operational queries.
- **Booking audit log**: Every state transition of a booking (PENDING → CONFIRMED → CANCELLED) is written to an immutable `booking_events` append-only table and also to S3 as a cold audit trail. Retained 7 years for financial compliance.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                              | Chosen Approach                         | Alternative                         | Trade-off                                                                 |
|---------------------------------------|-----------------------------------------|-------------------------------------|---------------------------------------------------------------------------|
| Seat locking mechanism                | Redis SETNX + PG row lock               | Pure PG pessimistic locking         | Redis adds infrastructure complexity; gains 10x throughput                |
| Booking consistency                   | Orchestration saga + outbox             | XA distributed transaction          | Saga requires compensating transactions; XA is impractical with Stripe    |
| Seat map storage                      | Redis hash + PG authoritative           | PG only                             | Redis adds 2 data stores to sync; reduces seat map read latency 10x       |
| Flash sale admission                  | Virtual waiting room + Redis DECR       | FIFO queue per seat                 | Waiting room is UX investment; prevents DB stampede effectively           |
| Hold TTL                              | 10 minutes                              | 5 or 15 minutes                     | 10 min balances user checkout time vs seat availability for others        |
| PDF generation                        | Async via Kafka + S3 + signed URL       | Synchronous in booking API          | Async means user waits ~30s for PDF link; avoids blocking the booking API |
| Notification delivery                 | Kafka consumer + retry                  | Direct API call from Booking Svc    | Kafka decouples; guarantees delivery even if notification svc is down     |
| DB choice                             | PostgreSQL                              | CockroachDB                         | CockroachDB provides better write scale; PG simpler ops at this scale     |

---

## 11. Follow-up Interview Questions

Q1: How would you design the seat map for a stadium with 80,000 seats and complex geometry?
A: Store the seat layout as an SVG template with seat positions encoded as data attributes. Seat status is a separate Redis bitmap (1 bit per seat, 80,000 seats = 10 KB). The client renders SVG + overlays status from the bitmap. Status updates are streamed via Server-Sent Events so users see real-time seat availability without polling.

Q2: How do you handle overbooking prevention when the `available_seats` counter and individual seat statuses can drift?
A: The `available_seats` field on `shows` is a denormalized counter for fast reads. The authoritative check is always the `seats.status` column. A reconciliation job runs every 5 minutes: `UPDATE shows SET available_seats = (SELECT COUNT(*) FROM seats WHERE show_id = shows.show_id AND status = 'AVAILABLE')`. Alerts fire if the delta exceeds 5.

Q3: How would you implement group/corporate bookings where 50 seats need to be booked in a single transaction?
A: Extend the hold API to accept up to 50 seat_ids. The Redis lock acquisition loop processes them in sorted order (deadlock prevention). The PostgreSQL transaction bulk-updates all 50 seats. No architectural changes needed — the current design supports it with a lifted limit. For 50+ seats, introduce a `group_hold` type that uses a database-level advisory lock on `show_id` to serialize group holds.

Q4: How would you implement dynamic pricing (demand-based seat price increases)?
A: Add a `pricing_rules` table with conditions (availability_threshold, multiplier). A pricing engine service reads `available_seats / total_seats` ratio every 30 seconds and updates `seat_categories.price` accordingly. The price is fetched fresh during the hold step (not cached aggressively). Flash sale prices can be locked for the hold duration (stored in `seat_holds.price_snapshot`).

Q5: Design the QR code validation system for venue entry scanning.
A: Each ticket's QR code encodes `{booking_id}:{seat_id}:{hmac_sha256(booking_id + seat_id + secret_key)}`. Scanner app calls `POST /validate-ticket` with the decoded payload. Validation service checks: (1) HMAC is valid, (2) booking status is CONFIRMED, (3) ticket has not been scanned before (set `scanned_at` atomically with a unique constraint). For offline resilience, scanners download a signed bloom filter of valid booking IDs at the start of each event day.

Q6: How would you handle concurrent users both trying to cancel and book the same seat?
A: The seat status machine enforces this. A CONFIRMED seat can only be CANCELLED (not HELD again) until the cancellation saga completes and sets it back to AVAILABLE. The compensating transaction (release hold) and the cancel flow both use `SELECT ... FOR UPDATE` on the seat row, serializing concurrent operations at the DB level.

Q7: How do you prevent a scalper bot from buying all available seats?
A: Multiple layers: (1) Rate limiting: max 5 booking attempts/user/minute, max 10 seats/booking. (2) CAPTCHA challenge triggered for anomalous browser fingerprints. (3) Virtual waiting room during flash sales randomizes queue position, defeating time-of-request ordering bots depend on. (4) Payment method velocity check: max 3 cards per account per day. (5) Fraud service flags purchases where shipping country ≠ IP country.

Q8: What changes would be needed to support reselling or transfer of tickets?
A: Add a `ticket_transfers` table. Seller initiates transfer → seat status transitions to `TRANSFER_PENDING` → buyer accepts and pays → old `booking_seats` record is updated with new `user_id` → new QR code is generated (old QR code invalidated). The HMAC in the QR code must include a nonce that changes on transfer.

Q9: How would you design the notification system to handle 200,000 confirmations being triggered simultaneously?
A: Kafka topic `booking_confirmed` with 50 partitions. 50 Notification Service consumer instances each processing their partition independently. Each consumer calls SendGrid Batch API (sends up to 1,000 emails per API call), achieving 50,000 emails/minute throughput. SMS uses a similar batched Twilio approach. Total 200,000 notifications: ~4 minutes end-to-end.

Q10: How does the waitlist get triggered and how do you prevent the same seat from being offered to 100 waitlist users simultaneously?
A: When seats are released (hold expired or booking cancelled), a `seats.released` Kafka event is published. A single Waitlist Service consumer (single partition for ordering) receives the event and processes waitlist entries one at a time: FIFO by `created_at`. For each available seat, the service sets the top waitlist entry to NOTIFIED and sends a notification with a time-limited (15-minute) exclusive hold token. If the user doesn't book within 15 minutes, the next entry is notified. This serialized processing ensures no seat is offered to multiple waitlist users simultaneously.

Q11: How would you test the seat locking mechanism under load?
A: (1) Unit tests: mock Redis and DB to test lock acquisition/release logic. (2) Integration tests: spin up real Redis + PostgreSQL in Docker, run 1,000 concurrent threads all attempting to hold the same seat, assert exactly 1 succeeds. (3) Chaos testing: kill Redis mid-hold-flow, verify DB consistency. (4) Load testing: use k6 to simulate 10,000 VUs hitting the hold endpoint for the same show_id, measure p99 latency and verify zero double-bookings in the DB post-test.

Q12: What is the impact of Kafka message redelivery on the booking flow?
A: Every Kafka consumer processes messages idempotently. `confirm_booking` checks `status = 'PENDING'` before acting — if already CONFIRMED, it is a no-op. `fail_booking` checks for PENDING status similarly. The outbox table uses a `processed_at` column; the outbox relay marks messages as processed before republishing, preventing duplicate publishes on relay crash.

Q13: How would you handle PCI DSS compliance for payment data?
A: Credit card data is never stored in our systems — Stripe's tokenization ensures we only store `payment_method_id` (a Stripe PM token). PostgreSQL contains only Stripe intent/charge IDs. Network segmentation: Payment Service runs in its own VPC subnet accessible only from Booking Service. Audit logs of all payment operations are stored immutably. Annual PCI DSS audit of Stripe integration. Stripe is a PCI Level 1 certified provider.

Q14: How would you implement seat selection for accessibility needs (wheelchair spots, companion seats)?
A: Accessibility seats are flagged with `seat_type: ACCESSIBILITY` in the seats table. The booking API accepts an `accessibility_required: boolean` filter. Accessibility seats are never available for regular booking — they can only be selected by users who have verified accessibility status in their profile. Venue configuration includes `companion_seat_id` linking an accessibility seat to its companion seat, which must be booked together.

Q15: How do you keep the denormalized `available_seats` on `shows` consistent with individual seat statuses at scale?
A: Three mechanisms: (1) All transitions that change seat count (`AVAILABLE → HELD`, `HELD → CONFIRMED`, `HELD → AVAILABLE`) atomically update `available_seats` in the same database transaction using `UPDATE ... SET available_seats = available_seats ± N`. (2) The 5-minute reconciliation job corrects any drift from bugs or crashes. (3) An alert fires if `available_seats` goes negative (impossible in correct operation). The denormalized counter avoids `SELECT COUNT(*)` on the seats table for every page view.

---

## 12. References & Further Reading

1. Martin Fowler — "Saga Pattern": https://martinfowler.com/articles/microservices.html#DecentralizingDataManagement
2. Martin Fowler — "Transactional Outbox Pattern": https://microservices.io/patterns/data/transactional-outbox.html
3. Redis Documentation — "Distributed Locks with Redis" (Redlock): https://redis.io/docs/manual/patterns/distributed-locks/
4. PostgreSQL Documentation — "Explicit Locking" (SELECT FOR UPDATE, Advisory Locks): https://www.postgresql.org/docs/current/explicit-locking.html
5. Stripe Documentation — "Idempotent Requests": https://stripe.com/docs/api/idempotent_requests
6. Stripe Documentation — "Webhooks Best Practices": https://stripe.com/docs/webhooks/best-practices
7. Designing Data-Intensive Applications — Martin Kleppmann (O'Reilly) — Chapters 7 (Transactions) and 12 (Future of Data Systems)
8. BookMyShow Engineering Blog — "How BookMyShow handles high traffic": https://medium.com/bookmyshow-engineering
9. Chris Richardson — "Microservices Patterns" (Manning) — Chapter 4: Managing Transactions with Sagas
10. PCI Security Standards Council — PCI DSS v4.0: https://www.pcisecuritystandards.org/document_library/
