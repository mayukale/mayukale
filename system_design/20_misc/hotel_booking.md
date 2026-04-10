# System Design: Hotel Booking System (Booking.com / Expedia-style)

---

## 1. Requirement Clarifications

### Functional Requirements

1. Users can search for hotels by location, check-in date, check-out date, number of guests, and room count.
2. Users can view hotel details: photos, amenities, reviews, room types, and availability.
3. Users can view real-time room availability for a selected date range.
4. Users can book one or more rooms, providing guest information and payment details.
5. Booking confirmation is sent via email/SMS with a booking reference number.
6. Users can cancel bookings subject to the hotel's cancellation policy (full refund, partial, or non-refundable).
7. Hotels/property managers can manage room inventory: add room types, set pricing, block dates.
8. Dynamic pricing: room prices vary by date, demand level, seasonality, and length of stay.
9. Overbooking support: hotels can configure an overbooking buffer (e.g., 105% of capacity) with walkout compensation logic.
10. Users can view their booking history and download receipts.
11. Review and rating system: users can leave reviews after a stay.
12. Support for multiple room types per hotel (Standard, Deluxe, Suite) and multiple rate plans (flexible, non-refundable, breakfast-included).

### Non-Functional Requirements

1. High availability: 99.99% uptime.
2. Consistency for room inventory — no room double-booked for the same dates.
3. Search results latency: p99 < 300 ms for availability search.
4. Booking transaction latency: p99 < 1 s.
5. Support for 50 million hotels globally (assumption: 50M property listings, 1M active with live inventory).
6. Peak throughput: 50,000 concurrent search requests and 10,000 booking requests per second during holiday seasons.
7. Date-range availability queries must be efficient (checking availability across 1-30 days).
8. PCI DSS compliance for payment handling.
9. Multi-currency and multi-language support.
10. System must support both B2C (direct consumer) and B2B (OTA / travel agency API) access.

### Out of Scope

- Building the payment processor itself.
- Loyalty points / rewards programs.
- Flight or package bundling.
- Hotel property management system (PMS) integration details.
- Mobile app UI specifics.
- Revenue management / yield management algorithms.
- AI-powered recommendation engine.

---

## 2. Users & Scale

### User Types

| Role               | Description                                                                      |
|--------------------|----------------------------------------------------------------------------------|
| Guest              | Search hotels; cannot book without account                                       |
| Registered User    | Book, cancel, review, access booking history                                     |
| Hotel Manager      | Manage property listings, room types, pricing, block dates, view reservations    |
| OTA Partner        | API access to search and book on behalf of their users                           |
| Admin              | Platform-level operations, fraud review, dispute resolution                      |

### Traffic Estimates

**Assumptions:**
- 100 million registered users globally; 10 million DAU (10% DAU ratio).
- Average user performs 5 searches per session, views 3 hotel pages, completes 0.1 bookings on average per visit.
- 1 million bookings/day (10% of DAU books: 10M * 0.1).
- Average booking duration: 2 nights; 1 room per booking.
- OTA partners contribute 40% of total traffic.
- Read:Write ratio: 200:1 (search/browse dominates).

| Metric                          | Calculation                                              | Result            |
|---------------------------------|----------------------------------------------------------|-------------------|
| Daily Active Users              | 100M * 10%                                               | 10,000,000        |
| Hotel searches/day              | 10M * 5                                                  | 50,000,000        |
| Search RPS (normal)             | 50M / 86,400                                             | ~579 RPS          |
| Hotel page views/day            | 10M * 3                                                  | 30,000,000        |
| Bookings/day                    | 10M * 0.1                                                | 1,000,000         |
| Booking RPS (normal)            | 1M / 86,400                                              | ~11.6 RPS         |
| Peak search RPS (holiday 10x)   | 579 * 10                                                 | ~5,790 RPS        |
| Peak booking RPS (holiday 10x)  | 11.6 * 10                                                | ~116 RPS          |
| Cancellation rate (20%)         | 1M * 0.2 / 86,400                                        | ~2.3 RPS          |

### Latency Requirements

| Operation                         | Target p50 | Target p99 |
|-----------------------------------|------------|------------|
| Hotel search (availability)       | 80 ms      | 300 ms     |
| Hotel detail + room types         | 50 ms      | 200 ms     |
| Availability check (booking step) | 50 ms      | 200 ms     |
| Booking creation                  | 200 ms     | 1,000 ms   |
| Cancellation                      | 200 ms     | 800 ms     |
| Admin inventory update            | 200 ms     | 1,000 ms   |

### Storage Estimates

**Assumptions:**
- Hotel listing: 10 KB (metadata + amenities). Photos stored separately in object store.
- Room type record: 2 KB. Rate plan: 1 KB.
- Reservation record: 2 KB. Inventory record (per room per date): 50 bytes.
- 1M active hotels, avg 10 room types, avg 365 days of inventory = 365M inventory records.
- Hotel photos: avg 20 photos per hotel * 300 KB compressed = 6 MB/hotel.

| Data Type                  | Calculation                                              | Size         |
|----------------------------|----------------------------------------------------------|--------------|
| Hotel listings             | 1M hotels * 10 KB                                        | 10 GB        |
| Room types                 | 1M hotels * 10 room types * 2 KB                        | 20 GB        |
| Rate plans                 | 1M hotels * 10 rate plans * 1 KB                        | 10 GB        |
| Inventory records          | 1M * 10 * 365 * 50 B                                    | ~182 GB      |
| Reservations (3 years)     | 1M/day * 365 * 3 years * 2 KB                           | ~2.19 TB     |
| User profiles              | 100M * 500 B                                            | 50 GB        |
| Reviews                    | 10M reviews * 1 KB                                      | 10 GB        |
| Hotel photos               | 1M hotels * 6 MB                                        | 6 TB         |
| Total DB (excl. photos)    | ~2.5 TB                                                 | ~2.5 TB      |
| Total object store         | ~6 TB                                                   | ~6 TB        |

### Bandwidth Estimates

| Traffic Type           | Calculation                                               | Bandwidth       |
|------------------------|-----------------------------------------------------------|-----------------|
| Search responses       | 5,790 RPS peak * 20 KB (list of 10 hotels)               | ~116 MB/s       |
| Hotel detail responses | 200 RPS * 50 KB                                          | ~10 MB/s        |
| Photo delivery (CDN)   | 30M page views/day * 20 photos * 300 KB / 86,400         | ~2.1 GB/s       |
| Booking writes         | 116 RPS peak * 5 KB                                      | ~580 KB/s       |
| Total (CDN offloads    | ~80% of photo bandwidth)                                  | ~2 GB/s gross   |

---

## 3. High-Level Architecture

```
                     ┌───────────────────────────────────────────────────────────┐
                     │                        CLIENT LAYER                        │
                     │    Web App  /  Mobile App  /  OTA Partner API Consumers    │
                     └────────────────────────────┬──────────────────────────────┘
                                                  │ HTTPS
                     ┌────────────────────────────▼──────────────────────────────┐
                     │               CDN (CloudFront / Fastly)                    │
                     │   Hotel photos, static assets, cached search results       │
                     └────────────────────────────┬──────────────────────────────┘
                                                  │
                     ┌────────────────────────────▼──────────────────────────────┐
                     │          API Gateway + Load Balancer                       │
                     │    JWT auth, rate limiting, request routing                │
                     └──┬──────────────┬──────────────┬──────────────┬───────────┘
                        │              │              │              │
        ┌───────────────▼──┐  ┌────────▼─────────┐  ┌▼─────────────┐  ┌▼──────────────┐
        │  Search Service  │  │ Inventory Service │  │  Booking     │  │ Notification  │
        │  (hotel search,  │  │ (availability,   │  │  Service     │  │  Service      │
        │   availability   │  │  room inventory, │  │  (create,    │  │  (email, SMS, │
        │   filtering)     │  │  pricing)        │  │  cancel,     │  │  receipts)    │
        └──────────┬───────┘  └────────┬─────────┘  │  modify)     │  └──────┬────────┘
                   │                   │             └──────┬───────┘         │
        ┌──────────▼───────┐  ┌────────▼─────────┐         │          ┌──────▼────────┐
        │  Elasticsearch   │  │  Redis Cluster   │   ┌─────▼──────┐   │  Kafka        │
        │  (hotel search   │  │  (availability   │   │  Payment   │   │  (booking     │
        │   index, geo)    │  │  cache, pricing  │   │  Service   │   │  events,      │
        └──────────────────┘  │  cache)          │   │  (Stripe   │   │  notifications│
                              └────────┬─────────┘   │  integration   │  waitlist)   │
                                       │             └──────┬──────┘  └──────────────┘
                     ┌─────────────────▼──────────────────▼──────────────────────┐
                     │              Primary PostgreSQL Cluster                     │
                     │  (hotels, rooms, rate_plans, inventory, reservations,      │
                     │   users — fully ACID, partitioned by hotel/date)           │
                     └──────────────────────────────────────────────────────────┬─┘
                                                                                 │
                     ┌───────────────────────────────────────────────────────────▼─┐
                     │  Read Replicas (3x) — serves Search Service, Inventory reads │
                     └─────────────────────────────────────────────────────────────┘
```

**Component Roles:**

- **CDN**: Serves hotel photos and static assets. Caches search result pages for popular (city, date) combinations with a 60-second TTL. Dramatically reduces origin load.
- **API Gateway**: Entry point — handles TLS, JWT/OAuth2 validation, per-user rate limiting (100 req/min for regular users, 10,000 req/min for OTA partners).
- **Search Service**: Executes multi-filter hotel availability searches. Reads from Elasticsearch for hotel attribute filtering (amenities, star rating, location) and then cross-references availability from the Inventory Service. Aggregates and ranks results.
- **Inventory Service**: The core of availability management. Tracks room availability by (hotel_id, room_type_id, date). Handles atomic allocation during booking. Manages pricing per date range.
- **Booking Service**: Orchestrates reservation creation, modification, and cancellation. Implements saga pattern for payment + inventory updates. Stores durable booking records.
- **Payment Service**: Wrapper around Stripe. Handles charges, refunds, and webhook processing.
- **Notification Service**: Sends confirmation/cancellation emails and SMS. Generates booking receipts.
- **Elasticsearch**: Hotel search index with geo-queries (`geo_distance`), full-text search on hotel name/description, and faceted filtering on amenities. Does NOT store real-time availability — it stores attribute data indexed from PostgreSQL.
- **Redis Cluster**: Caches room availability counts and prices per (hotel, room_type, date) for ultra-fast reads. Also stores booking session state during the checkout flow.
- **PostgreSQL**: Source of truth for all transactional data — inventory allocations, reservations, user data.
- **Kafka**: Event bus for decoupled side effects — confirmation notifications, analytics, overbooking monitoring.

**Primary Use-Case Data Flow (Search + Book):**

1. User searches (city=Paris, checkin=2026-06-01, checkout=2026-06-03, guests=2) → Search Service.
2. Search Service queries Elasticsearch for hotels in Paris with capacity ≥ 2. Gets list of 500 hotel_ids.
3. Search Service calls Inventory Service with (hotel_ids, date_range). Inventory Service reads availability counts from Redis (L1 cache, 60s TTL), falls back to PostgreSQL if cache miss.
4. Hotels with availability > 0 for all nights in the range are returned to user with min price.
5. User selects hotel → Hotel Detail Service returns room types, rate plans, and per-night pricing.
6. User selects room → `POST /reservations` → Booking Service.
7. Booking Service calls Inventory Service to atomically allocate room for dates (PostgreSQL row-level lock on inventory rows).
8. Booking Service calls Payment Service (Stripe charge or payment intent creation).
9. On payment success: reservation status → CONFIRMED. Kafka event published → Notification Service sends email.
10. On payment failure: compensating transaction releases allocated inventory.

---

## 4. Data Model

### Entities & Schema

```sql
-- Hotels
CREATE TABLE hotels (
    hotel_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            VARCHAR(300) NOT NULL,
    description     TEXT,
    star_rating     SMALLINT CHECK (star_rating BETWEEN 1 AND 5),
    address         TEXT NOT NULL,
    city            VARCHAR(100) NOT NULL,
    country_code    CHAR(2) NOT NULL,
    latitude        DECIMAL(9,6) NOT NULL,
    longitude       DECIMAL(9,6) NOT NULL,
    check_in_time   TIME NOT NULL DEFAULT '15:00',
    check_out_time  TIME NOT NULL DEFAULT '11:00',
    currency        CHAR(3) NOT NULL DEFAULT 'USD',
    is_active       BOOLEAN NOT NULL DEFAULT TRUE,
    amenities       JSONB,               -- {"wifi": true, "pool": true, "gym": true}
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_hotels_city (city),
    INDEX idx_hotels_location USING GIST (point(longitude, latitude))  -- geo index
);

-- Room Types (categories of rooms within a hotel)
CREATE TABLE room_types (
    room_type_id    UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    hotel_id        UUID NOT NULL REFERENCES hotels(hotel_id),
    name            VARCHAR(100) NOT NULL,  -- Standard, Deluxe, Suite, Ocean View
    description     TEXT,
    max_occupancy   SMALLINT NOT NULL,
    bed_type        VARCHAR(50),            -- KING, QUEEN, TWIN, DOUBLE
    total_rooms     SMALLINT NOT NULL,      -- physical rooms of this type in the hotel
    amenities       JSONB,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_room_types_hotel (hotel_id)
);

-- Rate Plans (pricing tiers per room type)
CREATE TABLE rate_plans (
    rate_plan_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    room_type_id      UUID NOT NULL REFERENCES room_types(room_type_id),
    hotel_id          UUID NOT NULL REFERENCES hotels(hotel_id),
    name              VARCHAR(100) NOT NULL,  -- Flexible, Non-Refundable, Breakfast
    description       TEXT,
    includes_breakfast BOOLEAN NOT NULL DEFAULT FALSE,
    is_refundable     BOOLEAN NOT NULL DEFAULT TRUE,
    cancellation_hours INT NOT NULL DEFAULT 48,  -- free cancellation until N hours before
    cancellation_penalty_percent DECIMAL(5,2) NOT NULL DEFAULT 0.00,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_rate_plans_room_type (room_type_id)
);

-- Room Inventory (availability and pricing per room type per date)
-- This is the central table for availability queries
CREATE TABLE room_inventory (
    inventory_id    BIGSERIAL PRIMARY KEY,
    hotel_id        UUID NOT NULL REFERENCES hotels(hotel_id),
    room_type_id    UUID NOT NULL REFERENCES room_types(room_type_id),
    date            DATE NOT NULL,
    total_rooms     SMALLINT NOT NULL,      -- physical room count (can change with overbooking)
    booked_rooms    SMALLINT NOT NULL DEFAULT 0,
    blocked_rooms   SMALLINT NOT NULL DEFAULT 0,   -- hotel-blocked (maintenance etc.)
    available_rooms SMALLINT GENERATED ALWAYS AS (total_rooms - booked_rooms - blocked_rooms) STORED,
    base_price      DECIMAL(10,2) NOT NULL,
    currency        CHAR(3) NOT NULL DEFAULT 'USD',
    min_stay_nights SMALLINT NOT NULL DEFAULT 1,
    UNIQUE (hotel_id, room_type_id, date),
    INDEX idx_inventory_hotel_date (hotel_id, date),
    CONSTRAINT no_overbooking CHECK (booked_rooms <= total_rooms)
    -- Note: total_rooms can exceed physical count for airline-style overbooking
    -- (set total_rooms = physical_count * 1.05 for 5% overbooking buffer)
);

-- Reservations
CREATE TABLE reservations (
    reservation_id  UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    hotel_id        UUID NOT NULL REFERENCES hotels(hotel_id),
    room_type_id    UUID NOT NULL REFERENCES room_types(room_type_id),
    rate_plan_id    UUID NOT NULL REFERENCES rate_plans(rate_plan_id),
    check_in_date   DATE NOT NULL,
    check_out_date  DATE NOT NULL,
    num_nights      SMALLINT GENERATED ALWAYS AS (check_out_date - check_in_date) STORED,
    num_rooms       SMALLINT NOT NULL DEFAULT 1,
    num_guests      SMALLINT NOT NULL,
    status          VARCHAR(20) NOT NULL DEFAULT 'PENDING',
                    -- PENDING, CONFIRMED, CANCELLED, COMPLETED, NO_SHOW, WALKED
    total_price     DECIMAL(10,2) NOT NULL,
    currency        CHAR(3) NOT NULL,
    payment_id      VARCHAR(255),
    special_requests TEXT,
    cancellation_reason TEXT,
    cancelled_at    TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_res_user (user_id),
    INDEX idx_res_hotel (hotel_id),
    INDEX idx_res_dates (hotel_id, check_in_date, check_out_date),
    INDEX idx_res_payment (payment_id)
);

-- Reservation Nightly Breakdown (price per night for a reservation)
CREATE TABLE reservation_nights (
    id              BIGSERIAL PRIMARY KEY,
    reservation_id  UUID NOT NULL REFERENCES reservations(reservation_id),
    date            DATE NOT NULL,
    price           DECIMAL(10,2) NOT NULL,
    UNIQUE (reservation_id, date)
);

-- Users
CREATE TABLE users (
    user_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email           VARCHAR(255) UNIQUE NOT NULL,
    phone           VARCHAR(20),
    name            VARCHAR(200) NOT NULL,
    password_hash   VARCHAR(255) NOT NULL,
    nationality     CHAR(2),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Reviews
CREATE TABLE reviews (
    review_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    reservation_id  UUID UNIQUE NOT NULL REFERENCES reservations(reservation_id),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    hotel_id        UUID NOT NULL REFERENCES hotels(hotel_id),
    overall_rating  SMALLINT NOT NULL CHECK (overall_rating BETWEEN 1 AND 10),
    cleanliness     SMALLINT CHECK (cleanliness BETWEEN 1 AND 10),
    location        SMALLINT CHECK (location BETWEEN 1 AND 10),
    service         SMALLINT CHECK (service BETWEEN 1 AND 10),
    value           SMALLINT CHECK (value BETWEEN 1 AND 10),
    title           VARCHAR(200),
    body            TEXT,
    is_verified     BOOLEAN NOT NULL DEFAULT TRUE,  -- linked to actual stay
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_reviews_hotel (hotel_id)
);

-- Hotel Photo References
CREATE TABLE hotel_photos (
    photo_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    hotel_id        UUID NOT NULL REFERENCES hotels(hotel_id),
    room_type_id    UUID REFERENCES room_types(room_type_id),  -- null = hotel-level photo
    s3_key          VARCHAR(500) NOT NULL,
    cdn_url         VARCHAR(500) NOT NULL,
    is_primary      BOOLEAN NOT NULL DEFAULT FALSE,
    sort_order      SMALLINT NOT NULL DEFAULT 0,
    INDEX idx_photos_hotel (hotel_id)
);

-- Overbooking Walkout Log
CREATE TABLE walkout_log (
    walkout_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    reservation_id  UUID NOT NULL REFERENCES reservations(reservation_id),
    hotel_id        UUID NOT NULL REFERENCES hotels(hotel_id),
    check_in_date   DATE NOT NULL,
    compensation    DECIMAL(10,2),
    alternative_hotel_id UUID REFERENCES hotels(hotel_id),
    resolved_at     TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

**Key design notes:**
- `room_inventory.available_rooms` is a generated column (computed from `total_rooms - booked_rooms - blocked_rooms`), always consistent.
- For overbooking, `total_rooms` is set higher than physical count (e.g., 11 for a 10-room type if 10% overbooking is enabled). A CHECK constraint allows this intentionally.
- `reservation_nights` stores per-night pricing for dynamic pricing scenarios; the reservation total is the sum of these rows.

### Database Choice

**Options Considered:**

| Database           | Pros                                                                      | Cons                                                                        |
|--------------------|---------------------------------------------------------------------------|-----------------------------------------------------------------------------|
| PostgreSQL         | ACID, row-level locking, generated columns, excellent range queries       | Single-node write ceiling; complex partitioning                             |
| MySQL (InnoDB)     | ACID, InnoDB row locking, wide cloud hosting support                      | Weaker generated column support; less powerful CTEs                         |
| CockroachDB        | Distributed ACID, horizontal write scale, Postgres-compatible SQL         | Higher write latency (Raft); operational cost                               |
| DynamoDB           | Truly elastic scale, managed, single-digit ms reads                       | No ACID multi-item transactions for complex availability queries; expensive joins |
| Cassandra          | Excellent write throughput, time-series data                              | No multi-row transactions; date-range availability queries are complex      |

**Selected: PostgreSQL with table partitioning + Elasticsearch for search**

Justification:
1. The `room_inventory` table has a natural partition key: `(hotel_id, date)`. PostgreSQL range partitioning by `date` (monthly partitions) allows old data to be archived efficiently and keeps query plans focused on a small partition for date-range queries.
2. Reservation creation requires an ACID transaction spanning `room_inventory` (UPDATE booked_rooms) and `reservations` (INSERT) — PostgreSQL handles this natively without coordination overhead.
3. The `available_rooms` generated column and the `CHECK` constraint prevent inventory corruption at the database level — a defense-in-depth layer below application logic.
4. Elasticsearch handles the complex multi-filter hotel search queries (geo-distance, amenity facets, price range, star rating) that PostgreSQL's B-tree indexes handle poorly at scale. PostgreSQL is not used for search; it is used purely for transactional data.
5. At 2.5 TB with proper partitioning, a single primary with read replicas handles the load. CockroachDB would be selected if write throughput exceeded what connection pooling + PgBouncer can provide.

---

## 5. API Design

```
BASE URL: https://api.hotelbooking.example.com/v1
```

### Search & Discovery

```
GET /hotels/search
  Auth: Optional (personalization requires auth)
  Rate limit: 100 req/min/IP; 5,000 req/min/OTA partner
  Query params:
    city*: string
    check_in*: date (YYYY-MM-DD)
    check_out*: date (YYYY-MM-DD)
    guests*: int (default 2)
    rooms: int (default 1)
    lat, lng, radius_km: for geo search (alternative to city)
    min_stars, max_stars: int
    max_price: decimal
    amenities: comma-separated (wifi,pool,gym,parking)
    sort: PRICE_ASC | PRICE_DESC | RATING_DESC | DISTANCE (default RATING_DESC)
    page, limit (default 20, max 50)
  Response 200:
  {
    "data": [{
      "hotel_id", "name", "star_rating", "city", "distance_km",
      "avg_rating", "review_count",
      "min_price_per_night": 120.00, "currency": "USD",
      "primary_photo_url",
      "available_room_types": [{ "room_type_id", "name", "min_price" }]
    }],
    "pagination": { "page", "limit", "total" }
  }
  Note: availability is checked for ALL nights in the requested range.
        A hotel only appears if it has rooms available for every night.

GET /hotels/{hotel_id}
  Auth: Optional
  Response 200:
  {
    "hotel_id", "name", "description", "star_rating", "address", "amenities",
    "check_in_time", "check_out_time",
    "photos": [{ "url", "is_primary" }],
    "avg_rating", "review_count",
    "room_types": [{
      "room_type_id", "name", "description", "max_occupancy", "bed_type", "amenities",
      "photos": [...],
      "rate_plans": [{
        "rate_plan_id", "name", "price_per_night", "total_price",
        "includes_breakfast", "is_refundable", "cancellation_policy"
      }]
    }]
  }
  Note: room_types and rate_plans include availability and pricing
        for the date range from the search context (passed via query params
        check_in and check_out).

GET /hotels/{hotel_id}/availability
  Auth: Optional
  Query: check_in, check_out, rooms (default 1)
  Response 200:
  {
    "hotel_id",
    "availability": [{
      "room_type_id",
      "room_type_name",
      "nights": [{
        "date": "2026-06-01",
        "available_rooms": 5,
        "price": 120.00
      }],
      "total_available_for_full_stay": 5,
      "min_price_total": 240.00
    }]
  }
```

### Reservations

```
POST /reservations
  Auth: Required
  Rate limit: 10 req/min/user
  Idempotency-Key: required (UUID v4 from client)
  Body:
  {
    "hotel_id": "uuid",
    "room_type_id": "uuid",
    "rate_plan_id": "uuid",
    "check_in_date": "2026-06-01",
    "check_out_date": "2026-06-03",
    "num_rooms": 1,
    "num_guests": 2,
    "payment_method_id": "pm_stripe_xxx",
    "special_requests": "High floor preferred"
  }
  Response 202:
  {
    "reservation_id": "uuid",
    "status": "PENDING",
    "payment_intent_id": "pi_xxx",
    "client_secret": "pi_xxx_secret_yyy",
    "total_price": 240.00,
    "currency": "USD"
  }
  Error 409: { "error": "ROOM_UNAVAILABLE", "message": "No rooms available for selected dates" }
  Error 400: { "error": "MIN_STAY_REQUIRED", "min_nights": 3 }
  Error 400: { "error": "INVALID_DATE_RANGE" }

GET /reservations/{reservation_id}
  Auth: Required (must be reservation owner or hotel manager for their hotel)
  Response 200:
  {
    "reservation_id", "status", "hotel": { ... }, "room_type": { ... },
    "check_in_date", "check_out_date", "num_nights", "num_guests",
    "total_price", "currency", "nightly_breakdown": [...],
    "cancellation_policy": { "deadline": "ISO8601", "refund_percent": 100 },
    "created_at"
  }

GET /reservations
  Auth: Required
  Query: status, check_in_from, check_in_to, page, limit
  Response 200: { "data": [...reservations], "pagination": { ... } }

DELETE /reservations/{reservation_id}
  Auth: Required (must be reservation owner)
  Response 200:
  {
    "reservation_id", "status": "CANCELLED",
    "refund": { "amount": 240.00, "currency": "USD", "estimated_business_days": 5 }
  }
  Error 400: { "error": "NON_REFUNDABLE", "message": "This rate plan is non-refundable" }
  Error 400: { "error": "CANCELLATION_DEADLINE_PASSED" }

PATCH /reservations/{reservation_id}
  Auth: Required (must be reservation owner)
  Body: { "special_requests": "string" }  -- limited modifications only
  Note: Date changes require cancel + rebook to maintain inventory integrity
  Response 200: { "reservation_id", "special_requests" }
```

### Hotel Manager APIs

```
PUT /hotels/{hotel_id}/inventory
  Auth: Required (hotel manager role)
  Rate limit: 1,000 req/min/hotel
  Body:
  {
    "room_type_id": "uuid",
    "updates": [{
      "date": "2026-06-01",
      "base_price": 150.00,
      "blocked_rooms": 2,   -- temporarily block for maintenance
      "total_rooms": 10     -- can be updated for overbooking configuration
    }]
  }
  Response 200: { "updated_count": 1 }

GET /hotels/{hotel_id}/reservations
  Auth: Required (hotel manager role)
  Query: check_in_date, check_out_date, status, page, limit
  Response 200: { "data": [...reservations with guest details], "pagination": {...} }
```

---

## 6. Deep Dive: Core Components

### 6.1 Date-Range Availability Query

**Problem it solves:**
A user searching for a 5-night stay must see only hotels that have at least 1 room of a given type available on ALL 5 nights. A hotel with 2 rooms on nights 1-4 but 0 rooms on night 5 must be excluded. This requires an efficient query across multiple date rows in the `room_inventory` table, executed for potentially thousands of hotels simultaneously.

**Approaches Comparison:**

| Approach                         | Mechanism                                                          | Pros                                  | Cons                                                          |
|----------------------------------|--------------------------------------------------------------------|---------------------------------------|---------------------------------------------------------------|
| Per-night AND query              | `WHERE date IN (...) GROUP BY hotel_id HAVING COUNT(*) = N AND MIN(available) >= rooms` | Simple SQL; works in any RDBMS | Full table scan without good partitioning; slow for wide ranges |
| Availability bitmap              | Redis bitfield: 1 bit per day, per room type                      | O(1) range check with BITCOUNT        | Doesn't encode quantity; only answers "has any room" not "has N rooms" |
| Segment tree / interval tree     | Precomputed available range segments                               | O(log N) range queries                | Complex to update on booking; not standard in RDBMS           |
| Denormalized availability ranges | Table: (hotel, room_type, start_date, end_date, min_available)    | Fast range query with index            | Complex update logic on booking; overlapping segments          |
| Date-range aggregate in Redis    | Pre-aggregated min availability per (hotel, room_type, range)     | Fastest reads                          | Cache invalidation complexity; massive keyspace               |

**Selected: PostgreSQL date-range aggregate query + Redis cache**

The core availability SQL query:
```sql
-- For a search: Paris hotels, check_in=2026-06-01, check_out=2026-06-03, 1 room
-- The stay covers nights: 2026-06-01 and 2026-06-02 (check_out night not included)

SELECT
    ri.hotel_id,
    ri.room_type_id,
    MIN(ri.available_rooms) AS min_available,  -- bottleneck night
    MIN(ri.base_price)      AS min_price,
    MAX(ri.base_price)      AS max_price,
    SUM(ri.base_price)      AS total_price
FROM room_inventory ri
WHERE ri.hotel_id = ANY($1::uuid[])       -- list of hotels from geo search
  AND ri.date >= $2                        -- check_in date
  AND ri.date < $3                         -- check_out date (exclusive)
GROUP BY ri.hotel_id, ri.room_type_id
HAVING COUNT(*) = ($3 - $2)               -- all nights present
   AND MIN(ri.available_rooms) >= $4;     -- sufficient rooms every night
```

Index supporting this query:
```sql
CREATE INDEX idx_inventory_hotel_date_avail
ON room_inventory (hotel_id, date, available_rooms)
WHERE available_rooms > 0;  -- partial index, skips fully-booked inventory
```

**PostgreSQL table partitioning** for `room_inventory`:
```sql
CREATE TABLE room_inventory (
    -- columns as defined above
) PARTITION BY RANGE (date);

CREATE TABLE room_inventory_2026_01 PARTITION OF room_inventory
    FOR VALUES FROM ('2026-01-01') TO ('2026-02-01');
CREATE TABLE room_inventory_2026_02 PARTITION OF room_inventory
    FOR VALUES FROM ('2026-02-01') TO ('2026-03-01');
-- ... monthly partitions, auto-created by a management job
```
Partition pruning ensures a 5-night search query only scans relevant monthly partitions.

**Redis caching layer:**
```python
def get_availability(hotel_ids: list[str], check_in: date, check_out: date,
                     num_rooms: int) -> dict:
    # Compute cache key based on inputs
    cache_key = f"avail:{hash(frozenset(hotel_ids))}:{check_in}:{check_out}:{num_rooms}"

    cached = redis.get(cache_key)
    if cached:
        return json.loads(cached)

    # Query PostgreSQL read replica
    results = db_replica.execute(AVAILABILITY_QUERY,
                                 [hotel_ids, check_in, check_out, num_rooms])

    # Cache for 60 seconds (acceptable staleness for search results)
    redis.set(cache_key, json.dumps(results), ex=60)
    return results
```

**Per-hotel availability key for hot hotels:**
For extremely popular hotels, maintain per-room-type availability counts in Redis updated on every booking:
```
avail:{hotel_id}:{room_type_id}:{date} → count (INT)
```
Update on booking: `DECRBY avail:{hotel_id}:{room_type_id}:{date} {num_rooms}` for each date in the stay. This allows availability checks without DB reads for hot hotels.

**Pseudocode for full search flow:**
```python
def search_hotels(city: str, check_in: date, check_out: date,
                  guests: int, rooms: int, filters: dict) -> list:
    # Step 1: Geo + attribute search via Elasticsearch
    es_query = {
        "bool": {
            "must": [{"match": {"city": city}}],
            "filter": [
                {"range": {"max_occupancy": {"gte": guests}}},
                # amenity filters from `filters`
            ],
            "should": [{"term": {"amenities.wifi": True}}]
        }
    }
    hotel_ids = elasticsearch.search(index="hotels", query=es_query, size=500)

    # Step 2: Availability check (Redis + PostgreSQL)
    available = get_availability(hotel_ids, check_in, check_out, rooms)

    # Step 3: Rank and sort
    results = rank_results(available, sort_by=filters.get("sort", "RATING_DESC"))

    return results[:filters.get("limit", 20)]
```

**Interviewer Q&A:**

Q1: How does `HAVING COUNT(*) = (check_out - check_in)` work and what does it guard against?
A: It ensures that every night in the date range has an inventory record. If a hotel hasn't set up inventory for a specific date (e.g., a future date not yet configured by the property manager), it won't have a row in `room_inventory` for that date. The COUNT check ensures all N nights are present. If any night is missing (=not configured), COUNT will be less than N, and the hotel correctly appears as "not available."

Q2: What happens to availability when a booking is created concurrently by two users for the last room?
A: The booking transaction uses `SELECT ... FOR UPDATE` on all `room_inventory` rows for the stay dates, then checks `available_rooms >= num_rooms`, then does `UPDATE room_inventory SET booked_rooms = booked_rooms + num_rooms`. The `FOR UPDATE` serializes concurrent requests — the second user's transaction blocks until the first commits, then sees 0 available rooms and fails with ROOM_UNAVAILABLE.

Q3: How do you handle the case where a hotel updates its price in the middle of a user's checkout session?
A: Prices are locked at the time of booking initiation. When the user clicks "Book Now," the system records the prices shown in `reservation_nights` at that moment. Subsequent price changes to `room_inventory.base_price` do not affect in-progress bookings. This is communicated clearly to users: "Price is locked until you complete booking." If the reservation is PENDING and more than 15 minutes pass (no payment), it is cancelled and the new price would apply on re-initiation.

Q4: How would you efficiently support a "flexible dates" search (show cheapest dates ± 3 days)?
A: Precompute a materialized view updated nightly: `min_price_by_hotel_month` — the minimum available price for each (hotel_id, month). The flexible dates search queries this view to find hotels in the price range, then runs the full availability query for the specific date windows. Alternatively, Elasticsearch can index `min_price_for_weekend` and `min_price_for_weekday` as denormalized fields updated by a nightly batch job.

Q5: What is the impact of overbooking on the availability query?
A: When `total_rooms` in `room_inventory` is set to 11 for a 10-physical-room type (10% overbooking), the `available_rooms` generated column reflects the overbooked total. The query correctly returns these rooms as available. The overbooking management is entirely a hotel configuration decision — the platform respects it. A separate overbooking monitor reads daily reservations vs physical capacity and triggers the walkout workflow for check-in day overflows.

---

### 6.2 Reservation Creation & Inventory Allocation

**Problem it solves:**
Booking a multi-night stay requires atomically decrementing inventory for multiple dates (e.g., booking June 1-3 must decrement June 1 and June 2 inventory simultaneously). This must be consistent under high concurrency without under-allocating (oversell beyond overbooking limit) or over-blocking.

**Approaches Comparison:**

| Approach                           | Mechanism                                              | Pros                               | Cons                                             |
|------------------------------------|--------------------------------------------------------|------------------------------------|--------------------------------------------------|
| Per-row `SELECT FOR UPDATE`        | Lock each inventory row for the stay dates             | Correct; standard SQL              | Deadlock risk if users book different overlapping date ranges |
| Optimistic locking (version check) | Read versions; UPDATE with WHERE version = expected    | No lock contention on reads        | High retry rate under contention; not suitable for multi-row |
| Advisory locks per hotel           | PG advisory lock on hotel_id before inventory update   | Serializes all operations per hotel | Serializes reads too; throughput bottleneck       |
| Inventory reservation table        | Pre-create "inventory hold" rows; confirm after payment | Explicit hold tracking             | More rows; more complexity                        |

**Selected: `SELECT FOR UPDATE` with ordered row locking (deadlock prevention)**

```python
def create_reservation(user_id: str, hotel_id: str, room_type_id: str,
                       rate_plan_id: str, check_in: date, check_out: date,
                       num_rooms: int, num_guests: int,
                       payment_method_id: str, idempotency_key: str) -> Reservation:

    # Idempotency check
    cached = idempotency_store.get(idempotency_key)
    if cached:
        return cached

    stay_dates = [check_in + timedelta(days=i) for i in range((check_out - check_in).days)]

    with db.transaction(isolation_level='READ COMMITTED'):
        # Lock inventory rows in deterministic order (by date ASC) to prevent deadlock
        inventory_rows = db.execute(
            """SELECT inventory_id, date, available_rooms, base_price
               FROM room_inventory
               WHERE hotel_id = $1 AND room_type_id = $2
                 AND date = ANY($3::date[])
               ORDER BY date ASC
               FOR UPDATE""",
            [hotel_id, room_type_id, stay_dates]
        )

        # Validate: all nights present and available
        if len(inventory_rows) != len(stay_dates):
            raise RoomUnavailableError("Hotel not configured for all requested dates")

        unavailable_nights = [r for r in inventory_rows if r.available_rooms < num_rooms]
        if unavailable_nights:
            raise RoomUnavailableError(f"Insufficient rooms on {[r.date for r in unavailable_nights]}")

        # Check min stay requirement
        rate_plan = db.query("SELECT * FROM rate_plans WHERE rate_plan_id = $1", [rate_plan_id])
        if len(stay_dates) < rate_plan.min_stay_nights:
            raise MinStayError(rate_plan.min_stay_nights)

        # Compute total price
        total_price = sum(r.base_price * num_rooms for r in inventory_rows)

        # Create reservation
        reservation_id = uuid4()
        db.execute(
            """INSERT INTO reservations
               (reservation_id, user_id, hotel_id, room_type_id, rate_plan_id,
                check_in_date, check_out_date, num_rooms, num_guests, status,
                total_price, currency)
               VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,'PENDING',$10,'USD')""",
            [reservation_id, user_id, hotel_id, room_type_id, rate_plan_id,
             check_in, check_out, num_rooms, num_guests, total_price]
        )

        # Record nightly breakdown
        for row in inventory_rows:
            db.execute(
                "INSERT INTO reservation_nights (reservation_id, date, price) VALUES ($1,$2,$3)",
                [reservation_id, row.date, row.base_price * num_rooms]
            )

        # Atomically decrement inventory
        db.execute(
            """UPDATE room_inventory
               SET booked_rooms = booked_rooms + $1
               WHERE hotel_id = $2 AND room_type_id = $3 AND date = ANY($4::date[])""",
            [num_rooms, hotel_id, room_type_id, stay_dates]
        )

        # Outbox for payment initiation
        db.execute(
            "INSERT INTO outbox (event_type, payload) VALUES ('initiate_payment', $1)",
            [json.dumps({
                "reservation_id": str(reservation_id),
                "amount": str(total_price),
                "currency": "USD",
                "payment_method_id": payment_method_id,
                "idempotency_key": idempotency_key
            })]
        )

    result = ReservationResult(reservation_id=reservation_id, status="PENDING",
                               total_price=total_price)
    idempotency_store.set(idempotency_key, result, ex=86400)
    return result

def confirm_reservation(reservation_id: str, payment_id: str):
    with db.transaction():
        reservation = db.execute(
            "SELECT * FROM reservations WHERE reservation_id = $1 AND status = 'PENDING' FOR UPDATE",
            [reservation_id]
        )
        if not reservation:
            return  # Idempotent
        db.execute(
            "UPDATE reservations SET status = 'CONFIRMED', payment_id = $1 WHERE reservation_id = $2",
            [payment_id, reservation_id]
        )
        # Outbox for confirmation email
        db.execute(
            "INSERT INTO outbox (event_type, payload) VALUES ('reservation_confirmed', $1)",
            [json.dumps({"reservation_id": str(reservation_id)})]
        )

def cancel_reservation_compensate(reservation_id: str):
    """Compensating transaction: release inventory when payment fails."""
    with db.transaction():
        reservation = db.execute(
            "SELECT * FROM reservations WHERE reservation_id = $1 AND status = 'PENDING' FOR UPDATE",
            [reservation_id]
        )
        if not reservation:
            return
        stay_dates = [reservation.check_in_date + timedelta(days=i)
                      for i in range(reservation.num_nights)]
        db.execute(
            """UPDATE room_inventory
               SET booked_rooms = booked_rooms - $1
               WHERE hotel_id = $2 AND room_type_id = $3 AND date = ANY($4::date[])""",
            [reservation.num_rooms, reservation.hotel_id,
             reservation.room_type_id, stay_dates]
        )
        db.execute(
            "UPDATE reservations SET status = 'FAILED' WHERE reservation_id = $1",
            [reservation_id]
        )
```

**Interviewer Q&A:**

Q1: Why do you lock inventory rows in ORDER BY date ASC? What problem does that solve?
A: Deadlock prevention. Suppose User A books June 1-2 and User B books June 2-3. Without ordering, A might lock June 1 and wait for June 2, while B locks June 2 and waits for June 1 — classic deadlock. With ORDER BY date ASC, both transactions always try to acquire locks in the same order (June 1 first, then June 2), so they serialize instead of deadlocking.

Q2: What if the payment takes 3 minutes and the inventory is held in PENDING state the whole time?
A: The reservation status is PENDING and inventory is decremented during that period (other users see reduced availability). This is intentional — it prevents selling the same room to two users simultaneously. A timeout job runs every 5 minutes and cancels PENDING reservations older than 15 minutes via the compensating transaction, releasing inventory. The 15-minute window is sufficient for payment processing including 3DS challenges.

Q3: How do you handle a modification request (change dates) for an existing reservation?
A: Date changes are implemented as cancel + rebook in the same transaction. The cancellation releases inventory for the old dates; the rebook allocates for the new dates. Both happen atomically. If the rebook fails (new dates unavailable), the cancellation is rolled back. Price differences are handled as a new charge or refund.

Q4: How do you handle concurrent cancellations and new bookings for the same inventory?
A: Both cancellation (which increments `booked_rooms = booked_rooms - num_rooms`) and new booking (which decrements) use `UPDATE room_inventory WHERE hotel_id = ... AND date = ...` — PostgreSQL serializes these at the row level. The `available_rooms` generated column is always consistent because it is computed from the stored value, not a separate column that could drift.

Q5: How would the system handle a hotel that wants to stop accepting reservations for specific dates (e.g., private event)?
A: The hotel manager calls `PUT /hotels/{hotel_id}/inventory` with `blocked_rooms = total_rooms` for the desired dates. This sets `available_rooms` to 0 for those dates via the generated column. All existing reservations remain — the block only prevents new ones. An alternative is setting `total_rooms = 0` for those dates, but `blocked_rooms` is preferred because it distinguishes maintenance blocks from capacity changes.

---

### 6.3 Overbooking Strategy

**Problem it solves:**
Hotels experience ~8-12% no-show and cancellation rates. To maximize occupancy and revenue, hotels intentionally accept more reservations than physical capacity — the same strategy used by airlines. The platform must support this while handling the rare walkout scenario (when more guests actually show up than rooms available) gracefully.

**Implementation:**

Overbooking is configured per room type. `total_rooms` in `room_inventory` is set to `physical_rooms * (1 + overbooking_rate)`. For a 10-room type with 5% overbooking: `total_rooms = 11`.

**Walkout Detection and Resolution:**
```python
def run_overbooking_check(hotel_id: str, check_in_date: date):
    """Runs at midnight before check-in day."""
    # Count confirmed reservations vs physical capacity
    confirmed = db.execute(
        """SELECT rt.room_type_id, rt.total_rooms as physical,
                  COUNT(r.reservation_id) * r.num_rooms as confirmed_count
           FROM reservations r
           JOIN room_types rt USING (room_type_id)
           WHERE r.hotel_id = $1 AND r.check_in_date = $2 AND r.status = 'CONFIRMED'
           GROUP BY rt.room_type_id, rt.total_rooms""",
        [hotel_id, check_in_date]
    )

    for row in confirmed:
        if row.confirmed_count > row.physical:
            excess = row.confirmed_count - row.physical
            # Select guests to walk (LIFO: most recently booked, or lowest-value rate plan)
            to_walk = db.execute(
                """SELECT r.reservation_id, r.user_id, r.total_price
                   FROM reservations r
                   WHERE r.hotel_id = $1 AND r.check_in_date = $2
                     AND r.room_type_id = $3 AND r.status = 'CONFIRMED'
                   ORDER BY r.created_at DESC  -- most recently booked walk first
                   LIMIT $4""",
                [hotel_id, check_in_date, row.room_type_id, excess]
            )
            for res in to_walk:
                initiate_walkout(res, hotel_id, check_in_date)

def initiate_walkout(reservation: Reservation, hotel_id: str, check_in_date: date):
    """Find alternative hotel and compensate guest."""
    # Find equivalent or better hotel nearby with availability
    alternatives = search_nearby_hotels(
        hotel_id=hotel_id, check_in=check_in_date, min_stars=current_hotel.star_rating
    )
    alt_hotel = alternatives[0] if alternatives else None

    compensation = reservation.total_price * 0.25  # 25% on top of full refund

    with db.transaction():
        db.execute(
            "UPDATE reservations SET status = 'WALKED' WHERE reservation_id = $1",
            [reservation.reservation_id]
        )
        db.execute(
            """INSERT INTO walkout_log
               (reservation_id, hotel_id, check_in_date, compensation, alternative_hotel_id)
               VALUES ($1,$2,$3,$4,$5)""",
            [reservation.reservation_id, hotel_id, check_in_date,
             compensation, alt_hotel.hotel_id if alt_hotel else None]
        )
        # Trigger refund + compensation payment
        issue_refund_and_compensation(reservation, compensation)

    # Notify guest immediately
    publish_to_kafka("reservation_walked", {
        "reservation_id": str(reservation.reservation_id),
        "alternative_hotel": alt_hotel,
        "compensation": str(compensation)
    })
```

---

## 7. Scaling

### Horizontal Scaling

- **API tier**: Stateless services on Kubernetes. HPA scales on search_rps custom metric. Minimum 3 pods per service, max 100.
- **Search Service**: Scales independently from booking — reads only. Can scale to 100 pods during peak search traffic without affecting booking capacity.
- **Elasticsearch**: Configured with 5 shards, 2 replicas. Hotels index size: ~5 GB. Handles 10,000 search QPS comfortably on a 5-node cluster.
- **Inventory Service**: Scales horizontally; stateless between requests. State lives in Redis and PostgreSQL.

### Database Sharding

`room_inventory` is the largest and most write-heavy table. Sharding strategy:
- **Shard key: `hotel_id`** — all inventory rows, reservations, and rate plans for a hotel on the same shard.
- At 2.5 TB total, a single PostgreSQL primary with read replicas handles current load. Sharding triggers when write throughput exceeds ~10,000 writes/second, which corresponds to ~864M bookings/day — well beyond current projections.
- For cross-shard search, the `hotels` table (10 GB) is replicated to all shards. Only the inventory/reservation tables are sharded.

### Replication

- 1 primary PostgreSQL + 3 read replicas.
- Read replicas serve: availability queries from Search Service, hotel listing reads, user booking history.
- Synchronous replication to 1 replica (RPO = 0); asynchronous to remaining 2 replicas.
- Elasticsearch: 5 primary shards, 2 replicas = 15 total index copies. Can tolerate 2 node failures.

### Caching Strategy

| Cache Layer          | Cached Data                                        | TTL        | Invalidation Trigger                         |
|----------------------|----------------------------------------------------|------------|----------------------------------------------|
| CDN                  | Hotel photos                                       | 30 days    | Hotel manager updates photo                  |
| CDN                  | Hotel listing pages (city/date combos)             | 60 seconds | TTL expiry                                   |
| Redis                | Availability counts per (hotel, room_type, date)  | 60 seconds | Booking creation / cancellation              |
| Redis                | Hotel metadata (name, amenities, rating)           | 10 minutes | Hotel manager updates property               |
| Redis                | Search result pages for popular city+date combos   | 30 seconds | TTL expiry                                   |
| App L1 cache (LRU)   | Rate plan details, cancellation policies           | 5 minutes  | TTL expiry                                   |

### CDN

- Hotel photos: largest bandwidth consumer (2.1 GB/s). CloudFront serves from S3 with 30-day cache. Dramatically reduces origin bandwidth.
- Hotel listing API responses for popular searches (Paris, June weekend) are edge-cached using CDN with Vary: Accept-Language header for multi-language support.
- API responses that include real-time availability are NOT CDN-cached (60-second Redis cache in application layer instead).

**Interviewer Q&A:**

Q1: How do you handle a popular hotel that receives 10,000 booking requests per second for its last available room?
A: Redis availability counter per (hotel, room_type, date) acts as a soft gate: `DECRBY avail:{hotel_id}:{room_type}:{date} {num_rooms}` — if the result goes negative, increment back and reject immediately without hitting the DB. Only the first successful DECRBY proceeds to the PostgreSQL `SELECT FOR UPDATE` transaction. This reduces DB load from 10,000 RPS to effectively 1 RPS (only the winner).

Q2: How does the availability cache stay consistent with the database?
A: The booking and cancellation transactions publish Kafka events after committing. An Inventory Cache Updater service consumes these events and updates the Redis availability counts accordingly. If the Cache Updater is behind (Kafka lag), the Redis counts may be slightly optimistic (showing 1 room available when there is 0). The PostgreSQL `FOR UPDATE` check is the authoritative gate — optimistic Redis reads only reduce DB load; they don't compromise correctness.

Q3: How would you scale Elasticsearch for 50 million hotel listings globally?
A: At 50M hotels * ~5 KB indexed data = 250 GB index. Elasticsearch handles this with a 20-node cluster, 10 primary shards per region. Hotels are geographically distributed — index by continent to localize searches (European searches hit European shards). Use Elasticsearch's cross-cluster search for global queries. Hot hotels (high search frequency) are promoted to faster SSD storage using ILM (Index Lifecycle Management).

Q4: How do you maintain consistency between the Elasticsearch hotel index and PostgreSQL?
A: The Elasticsearch index is updated via Debezium CDC (Change Data Capture) reading PostgreSQL's WAL. Changes to the `hotels` table (amenities update, new photos, rating change) are streamed to Kafka, consumed by an Elasticsearch sink connector, and indexed within ~2 seconds. Elasticsearch is eventually consistent with PostgreSQL — acceptable because hotel attributes rarely change in real-time, and the impact of a 2-second lag is negligible.

Q5: Describe your multi-region deployment for international availability.
A: Three regions: us-east-1, eu-west-1, ap-southeast-1. Each region has its own API tier and caching layer. PostgreSQL uses primary in us-east-1 with async replicas in other regions (serving reads). All writes route to the primary (cross-region write latency: ~100-150 ms — acceptable for booking operations). Elasticsearch is deployed independently in each region, synced via Kafka MirrorMaker 2. Route 53 latency-based routing directs users to the nearest region. On primary region failure, the recovery process promotes the eu-west-1 replica to primary (RPO: near-zero with synchronous replication; RTO: ~2 minutes with Patroni).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component              | Failure Mode                               | Impact                                         | Mitigation                                                              |
|------------------------|--------------------------------------------|------------------------------------------------|-------------------------------------------------------------------------|
| PostgreSQL primary     | Crash / network partition                  | All writes fail; reads may continue            | Patroni auto-failover to sync replica in < 30 s                        |
| Redis availability cache | All Redis nodes down                     | Availability reads fall through to PostgreSQL  | Circuit breaker routes to DB; higher latency but correct               |
| Elasticsearch          | Node failure                               | Search may be slower; index degraded            | 2 replicas per shard; search continues with reduced capacity            |
| Payment gateway        | Stripe API unreachable                     | New bookings cannot complete payment            | PENDING reservations queued; retry; user notified of delay              |
| Kafka broker failure   | Broker unavailable                         | Notifications/confirmation delayed             | 3 broker cluster, RF=3; ISR ensures no data loss                       |
| Notification Service   | Email provider (SendGrid) outage           | Confirmation emails delayed                    | Retry queue; fallback to SMS; PDF available in user portal regardless   |
| Overbooking check job  | Job fails to run                           | Walkout management manual                      | Job is idempotent and reruns on recovery; alerting on job failure       |
| Hotel Manager API      | Wrong inventory update (blocks all rooms)  | Hotel disappears from search                   | Audit log of all inventory changes; self-service revert; admin override |

### Retries & Idempotency

- All API-to-API calls: exponential backoff with jitter (initial 100ms, max 30s, 5 retries).
- `POST /reservations`: Idempotency-Key header prevents duplicate reservations.
- `UPDATE room_inventory` uses version checks for safe retries; re-running the compensating transaction is safe because `booked_rooms >= 0` constraint prevents going below zero.
- Stripe charges use Stripe's idempotency key — retrying a charge with the same key is a no-op at the payment level.

### Circuit Breaker

- Inventory Service → Redis: opens after 3 consecutive Redis failures. Fallback: direct PostgreSQL read with a 200ms timeout.
- Booking Service → Payment Service: opens on 50% failure rate. Fallback: reject new bookings with a "payment temporarily unavailable" message and queue for retry.
- Search Service → Elasticsearch: opens on 3 consecutive failures. Fallback: serve results from PostgreSQL read replica with reduced filtering capabilities.

---

## 9. Monitoring & Observability

### Metrics

| Metric                                  | Type      | Alert Threshold                           |
|-----------------------------------------|-----------|-------------------------------------------|
| `reservation.create.latency_p99`        | Histogram | > 1 s                                     |
| `availability.query.latency_p99`        | Histogram | > 300 ms                                  |
| `inventory.double_booking.count`        | Counter   | > 0 (critical — pager)                    |
| `reservation.payment.success_rate`      | Gauge     | < 92%                                     |
| `inventory.cache_hit_rate`              | Gauge     | < 85%                                     |
| `elasticsearch.search.latency_p99`      | Histogram | > 200 ms                                  |
| `overbooking.walkout.count_per_day`     | Counter   | > 5/day (review hotel config)             |
| `db.inventory.available_rooms_negative` | Counter   | > 0 (critical — data integrity alarm)     |
| `kafka.consumer.lag.notifications`      | Gauge     | > 5,000 messages                          |
| `reservation.cancellation.rate`         | Gauge     | > 30% over 1 hour (anomaly detection)     |

### Distributed Tracing

OpenTelemetry spans propagated through all services. A complete reservation trace: API Gateway → Booking Service → Inventory Service (Redis + PG) → Payment Service → Stripe (external span boundary) → Webhook → Booking Service → PostgreSQL → Kafka → Notification Service. Exported to Honeycomb/Jaeger. Critical business-metric spans tagged with `hotel_id`, `reservation_id`, `user_id` for drill-down.

### Logging

Structured JSON logs. Fields: `trace_id`, `span_id`, `user_id`, `reservation_id`, `hotel_id`, `operation`, `duration_ms`, `status`, `error_code`. No PCI data logged. Logs to Elasticsearch (operational, 30-day retention) and S3 (compliance archive, 7-year retention). Booking creation, modification, and cancellation events written to an immutable `reservation_audit_log` table.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                           | Chosen                                      | Alternative                       | Trade-off                                                               |
|------------------------------------|---------------------------------------------|-----------------------------------|-------------------------------------------------------------------------|
| Availability storage               | PostgreSQL per-date rows + Redis cache       | Availability bitmap in Redis      | Bitmap can't encode quantity; per-row is flexible but needs good indexing |
| Search engine                      | Elasticsearch for hotel attributes           | PostgreSQL full-text search       | ES operational cost; gains geo-search and faceted filtering at scale    |
| Inventory concurrency control      | SELECT FOR UPDATE, ordered by date           | Optimistic locking                | Pessimistic reduces retry storms under contention; small throughput cost |
| Overbooking model                  | Increase total_rooms beyond physical count   | Separate overbooking_slots column | Simpler query; slightly obscures "physical vs booked" semantics          |
| Date change implementation         | Cancel + rebook in single transaction        | In-place date update             | Cancel + rebook reuses existing allocation logic; avoids duplicate code  |
| Photo storage                      | S3 + CDN                                    | DB BLOB storage                   | DB BLOBs are expensive I/O; S3+CDN is cost-effective and low-latency   |
| Reservation PENDING window         | 15 minutes                                  | 5 or 30 minutes                  | 15 min balances payment processing time vs inventory hold period        |

---

## 11. Follow-up Interview Questions

Q1: How would you implement price surge / demand-based pricing that updates in real time?
A: A pricing engine service subscribes to Kafka `inventory.updated` events and recalculates prices using a formula based on (`booked_rooms / total_rooms` ratio, seasonality factor, competitor prices). It updates `room_inventory.base_price` for future dates (not existing reservations). Redis availability cache for price data has a 60-second TTL, so price changes propagate to users within 1 minute.

Q2: How do you handle long-tail hotels (properties with very low booking frequency)?
A: For hotels that haven't been searched in 30 days, evict their availability cache entries. Their inventory is queried directly from PostgreSQL at search time (cold path). This avoids wasting Redis memory on stale cache entries for infrequently searched properties. Hot hotels (top 10% by search volume) maintain warm cache entries indefinitely.

Q3: How would you implement a "last room available" alert for users browsing a hotel?
A: WebSocket connection from the hotel detail page. When `available_rooms` drops to 1 for any room type on any date in the user's selected range, the Inventory Service publishes a `low_availability` Kafka event. A WebSocket push server (e.g., Pusher/AWS API Gateway WebSockets) delivers this to connected clients viewing that hotel. Clients display "Only 1 room left!" banner.

Q4: How do you prevent an OTA partner from bulk-scraping your pricing data?
A: Per-partner rate limits enforced at API Gateway (10,000 req/min max). API keys required for partner access; keys tied to agreements with usage clauses. Responses for availability searches include a cache hint (`Cache-Control: max-age=60`) to encourage caching. Anomaly detection flags partners with unusually high query rates for non-booking purposes. Contractual agreements cover unauthorized scraping.

Q5: How would you implement group bookings (booking 50 rooms for a corporate event)?
A: Group bookings go through a separate `group_reservation` flow: (1) Hotel receives a quote request, reviews demand, and confirms allocation; (2) Platform holds inventory with a group hold (status: GROUP_HOLD, longer TTL of 48 hours); (3) Billing is invoice-based, not immediate charge. The `room_inventory.booked_rooms` update is the same mechanism, just with a larger `num_rooms` value and a different reservation type.

Q6: How does your schema handle a hotel with different pricing per night in a multi-night stay (e.g., weekday vs weekend rates)?
A: `room_inventory` has one row per (hotel_id, room_type_id, date), each with its own `base_price`. A 5-night stay spanning a weekend reads 5 rows with potentially different prices. `reservation_nights` records each night's price individually, and `reservations.total_price` is the sum. The availability query's `SUM(base_price)` provides the total cost shown in search results.

Q7: How do you handle timezone complexities in check-in/check-out dates?
A: Hotel check-in/check-out dates are stored as `DATE` (no timezone), representing the local date at the hotel's location. The hotel's timezone is stored in the `hotels` table. All display/comparison logic converts to the hotel's local timezone before working with dates. UTC timestamps are used only for system events (created_at, updated_at). This prevents the scenario where a midnight UTC timestamp is interpreted as the previous day in a UTC+8 timezone.

Q8: What changes are needed to support short-term rental properties (Airbnb-style) with per-night custom pricing and blocked dates?
A: The current schema already supports this: `room_inventory` has per-date rows with individual prices, and `blocked_rooms` handles date-specific blocking. The only additions needed: (1) `min_stay_nights` per date (already in `room_inventory`); (2) `max_stay_nights` per date; (3) an `is_available` flag per date to allow the host to block specific dates without incrementing `blocked_rooms` (cleaner semantics); (4) `price_per_extra_guest` in rate plans.

Q9: How would you implement a "Best Price Guarantee" feature that monitors competitor prices?
A: A separate Price Intelligence Service runs scheduled jobs to fetch competitor prices for the same hotel (using the hotel's external IDs). It stores competitor price data in a separate `competitor_prices` table. The Booking Service checks if the booked price is higher than a competitor price at booking time. If it is, it applies the guarantee discount automatically and logs it for finance reconciliation. This is operationally sensitive (legal agreements) and isolated from the core booking path.

Q10: How would you handle split stays (checking in, checking out, and checking back in at the same hotel)?
A: Each contiguous stay is a separate reservation. The UI allows users to book multiple reservations for the same hotel with non-overlapping dates. The inventory logic does not need to change — each reservation allocates independently. The hotel receives separate reservation_ids but the user can see them linked in their profile (via a `stay_group_id` field linking related reservations).

Q11: How do you ensure the `booked_rooms` counter never goes negative during a cancellation race condition?
A: The cancellation `UPDATE` uses `WHERE booked_rooms >= num_rooms` and returns the updated row count. If the update affects 0 rows (booked_rooms would go negative), the cancellation is flagged for manual review — this indicates a data integrity issue. In practice, the `PENDING` timeout releases inventory via a compensating transaction path that is separate from user-initiated cancellation, preventing double-release.

Q12: How would you design the review system to prevent fake reviews?
A: Reviews are only allowed for reservations with `status = 'COMPLETED'` (check-out date has passed) and `review_count = 0` per reservation (one review per stay). The `is_verified` flag on the review is set to TRUE automatically for these reviews. The review is linked to `reservation_id` (not just `user_id` and `hotel_id`), making it impossible to review a hotel without an actual stay.

Q13: How do you handle bulk inventory updates for a hotel chain setting prices for 200 hotels simultaneously?
A: The `PUT /hotels/{hotel_id}/inventory` endpoint accepts an array of up to 365 date entries per call. For bulk multi-hotel updates, a dedicated batch API is provided: `POST /bulk-inventory` accepts an array of hotel+inventory update objects. This is processed asynchronously: the API returns a job_id, and a background worker processes the updates in batches of 1,000 rows using PostgreSQL's `INSERT ... ON CONFLICT (hotel_id, room_type_id, date) DO UPDATE` (upsert). The job status is polled via `GET /jobs/{job_id}`.

Q14: Describe your backup and point-in-time recovery strategy for the reservation database.
A: PostgreSQL continuous WAL archiving to S3 (via pgBackRest or Barman). WAL segments are shipped every 60 seconds, enabling point-in-time recovery to within 1 minute. Full base backups run daily and are retained for 30 days. Incremental WAL files retained for 7 days (beyond that, the daily base backup is the restore point). Monthly full backups retained for 1 year for compliance. Recovery is tested quarterly via restore drills on a dedicated recovery environment.

Q15: How would you expose a bulk availability API for metasearch engines like Google Hotel Ads?
A: Metasearch engines pull availability and pricing feeds, not individual room data. Provide a `GET /feed/availability` endpoint (B2B only, API key auth) that returns compressed (gzip) JSONL with availability for all hotels updated in the last N minutes: `?updated_since=ISO8601&limit=10000`. The feed is generated from a materialized view refreshed every 5 minutes. For Google Hotel Ads specifically, implement the Google Hotel API specification (XML-based price feed via SFTP or direct API). Separate from user-facing APIs — these have much higher per-call data volumes and different SLA requirements.

---

## 12. References & Further Reading

1. Booking.com Engineering Blog — "How Booking.com Processes Billions of Events": https://medium.com/booking-com-development
2. Airbnb Engineering — "Scaling Airbnb's Payment Infrastructure": https://medium.com/airbnb-engineering
3. PostgreSQL Documentation — "Table Partitioning": https://www.postgresql.org/docs/current/ddl-partitioning.html
4. PostgreSQL Documentation — "Row Security and Locking": https://www.postgresql.org/docs/current/explicit-locking.html
5. Elasticsearch Documentation — "Geo Queries": https://www.elastic.co/guide/en/elasticsearch/reference/current/geo-queries.html
6. Martin Fowler — "Event-driven Architecture": https://martinfowler.com/articles/201701-event-driven.html
7. Designing Data-Intensive Applications — Martin Kleppmann — Chapter 7: Transactions
8. Google Hotel Ads API Documentation: https://developers.google.com/hotels/hotel-ads/dev-guide/overview
9. Stripe Documentation — "Payment Intents API": https://stripe.com/docs/payments/payment-intents
10. Alex Xu — "System Design Interview Volume 2" (ByteByteGo) — Hotel Reservation System chapter
