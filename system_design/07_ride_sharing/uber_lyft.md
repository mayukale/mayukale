# System Design: Uber / Lyft Ride-Sharing Platform

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Rider flow**: A rider can open the app, enter a destination, see an upfront fare estimate, request a ride, be matched to a nearby driver, track the driver in real time, complete the trip, and pay automatically.
2. **Driver flow**: A driver can go online, receive ride requests with pickup/dropoff previews, accept or decline, navigate to the rider, start and end the trip, and receive payout.
3. **Surge pricing**: The system dynamically multiplies base fare when demand exceeds supply in a geographic area.
4. **Trip state machine**: Trips progress through well-defined states — `REQUESTED → DRIVER_ASSIGNED → DRIVER_EN_ROUTE → RIDER_PICKED_UP → TRIP_IN_PROGRESS → COMPLETED | CANCELLED`.
5. **Geospatial matching**: The system must find available drivers within a configurable radius (default 5 km) using efficient spatial indexing.
6. **Fare calculation**: Compute final fare from base rate, per-minute, per-km rates, surge multiplier, tolls, and promotional discounts.
7. **Payment processing**: Charge rider's stored payment method on trip completion; pay driver weekly via ACH/bank transfer.
8. **Ratings**: Riders rate drivers (1–5 stars) and drivers rate riders after each trip.
9. **Ride types**: UberX/Lyft Standard, UberXL/Lyft XL, UberBlack/Lyft Lux, Pool/Shared.
10. **Notifications**: Push notifications for driver assignment, driver arrival, trip start/end, receipt.
11. **Trip history**: Both riders and drivers can view past trips with route, fare breakdown, and receipt.
12. **Cancellation**: Either party can cancel with configurable cancellation fee windows.

### Non-Functional Requirements

- **Availability**: 99.99% uptime (≈52 min/year downtime). Ride-hailing is real-time and revenue-critical.
- **Matching latency**: Driver match returned within **3 seconds** of rider request (p99).
- **Location freshness**: Driver location updated in the system within **5 seconds** of GPS event.
- **Consistency**: Trip state transitions must be strongly consistent — no two drivers assigned to the same trip; no rider double-charged.
- **Scalability**: Support 25 million active riders, 5 million active drivers globally across 10,000+ cities.
- **Durability**: Trip and payment records must be durable (ACID-compliant) — zero data loss.
- **Security**: Payment card data handled via PCI-DSS compliant vault; PII encrypted at rest.
- **Geo-partitioning**: City-level data locality to minimize cross-region latency and comply with data-residency laws.

### Out of Scope

- Uber Eats / food delivery
- Freight / trucking
- Autonomous vehicle fleet management
- Driver background check onboarding pipeline
- Detailed fraud detection ML pipeline (assumed as a downstream async service)
- Accounting and tax reporting systems

---

## 2. Users & Scale

### User Types

| User Type | Description | Key Behaviors |
|---|---|---|
| Rider | End consumer requesting rides | Opens app, requests ride, tracks driver, pays |
| Driver | Independent contractor fulfilling trips | Streams GPS location, accepts requests, navigates |
| Operations Staff | Internal tooling users | Monitor city health dashboards, manage incidents |
| Admin | Product/engineering staff | Configure pricing rules, fare tables, geofences |

### Traffic Estimates

**Assumptions**:
- 25 M active riders globally; 5 M active drivers
- 20 M trips/day at peak (Uber's reported scale ~19 M trips/day in 2023)
- Peak hour = 2× average rate (evening commute, 5–7 PM)
- Each driver sends a GPS update every 4 seconds while online
- Drivers are online avg 6 hours/day; 5 M drivers → peak concurrent online drivers = 5 M × 0.30 peak fraction = 1.5 M drivers online at peak

| Metric | Calculation | Result |
|---|---|---|
| Trip requests/sec (avg) | 20 M / 86,400 | ~231 req/s |
| Trip requests/sec (peak) | 231 × 2 | ~462 req/s |
| Driver GPS updates/sec (avg) | 1.5 M drivers × (1 update / 4 s) | 375,000 writes/s |
| Driver GPS updates/sec (peak) | 375,000 × 1.5 | ~562,500 writes/s |
| Location reads (matching service polling) | 462 matching ops × 50 candidate drivers each | ~23,100 reads/s |
| Trip state updates/sec | 462 req/s × 5 state transitions avg | ~2,310 writes/s |
| Fare calculation requests/sec | Same as trip completions ≈ 231/s | ~231/s |
| Push notifications/sec | 231 × 4 events/trip | ~924 notif/s |

### Latency Requirements

| Operation | Target P50 | Target P99 | Rationale |
|---|---|---|---|
| Driver match returned to rider | < 1 s | < 3 s | UX — rider abandons after 5 s wait |
| Driver location update latency | < 2 s end-to-end | < 5 s | Freshness for matching & map display |
| ETA calculation | < 500 ms | < 1.5 s | Shown in ride-request confirmation screen |
| Fare estimate | < 200 ms | < 500 ms | Pre-request screen, must feel instant |
| Payment processing | < 2 s | < 5 s | Post-trip; slight delay is acceptable |
| Trip state transition | < 500 ms | < 1 s | Driver/rider UX depends on ack |

### Storage Estimates

**Assumptions**:
- Trip record: 2 KB (metadata + route snapshot)
- GPS point: 32 bytes (trip_id, driver_id, lat, lng, timestamp, accuracy)
- Route polyline per trip: avg 300 GPS points × 32 B = 9.6 KB
- User record: 500 bytes
- Retention: trips 7 years (legal), raw GPS 90 days, aggregated GPS forever

| Entity | Size/Record | Volume | Storage/Day | Total (7 yr) |
|---|---|---|---|---|
| Trip records | 2 KB | 20 M/day | 40 GB/day | 102 TB |
| Raw GPS (active trips) | 32 B/point, 300 pts/trip | 20 M trips × 300 = 6 B pts/day | 192 GB/day | 17 TB (90-day retention) |
| User profiles (riders + drivers) | 500 B | 30 M users | 15 GB total | 15 GB |
| Payment transactions | 1 KB | 20 M/day | 20 GB/day | 51 TB |
| Ratings | 100 B | 20 M/day | 2 GB/day | 5 TB |
| **Total (trips + GPS + payments)** | | | **~254 GB/day** | **~175 TB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Driver GPS ingest | 562,500 writes/s × 64 B payload | ~36 MB/s inbound |
| Trip state reads (riders polling) | 25 M active sessions × 1 poll/5 s × 500 B | ~2.5 GB/s (handled via WebSocket push instead) |
| WebSocket push to riders (driver location) | 500 K active trips × 1 push/4 s × 200 B | ~25 MB/s outbound |
| Push notifications | 924/s × 1 KB | ~1 MB/s |
| Map tile delivery | 5 M app-open sessions × 50 KB tiles | Offloaded to CDN |
| **Total backend bandwidth** | | **~65 MB/s sustained** |

---

## 3. High-Level Architecture

```
                         ┌──────────────────────────────────────────────┐
                         │              CLIENTS                         │
                         │  [Rider iOS/Android]  [Driver iOS/Android]   │
                         └───────┬──────────────────────┬───────────────┘
                                 │ HTTPS / WebSocket     │ HTTPS / WebSocket
                         ┌───────▼──────────────────────▼───────────────┐
                         │           API GATEWAY / LOAD BALANCER        │
                         │  (AWS ALB / Nginx)  TLS termination,         │
                         │  auth token validation, rate limiting        │
                         └──┬──────────┬──────────┬──────────┬──────────┘
                            │          │          │          │
               ┌────────────▼─┐  ┌─────▼──────┐  │  ┌───────▼────────┐
               │  Trip Service│  │ Driver Svc │  │  │  Rider Service  │
               │ (state machine│  │(availability│  │  │ (profile, prefs│
               │  ACID txns)  │  │ + location) │  │  │  trip history) │
               └──────┬───────┘  └─────┬───────┘  │  └───────┬────────┘
                      │                │           │          │
               ┌──────▼───────────────▼───────┐   │  ┌───────▼────────┐
               │      Matching Service        │   │  │  Fare / Pricing │
               │  (geospatial query,          │   │  │  Service        │
               │   scoring, assignment)       │   │  │  (surge engine) │
               └──────────────┬───────────────┘   │  └───────┬────────┘
                              │                   │          │
               ┌──────────────▼───────────────────▼──────────▼──────┐
               │              MESSAGE BUS (Apache Kafka)             │
               │  Topics: location-updates, trip-events,             │
               │  payment-events, notification-events                │
               └────┬────────────────────┬────────────────┬──────────┘
                    │                    │                │
          ┌─────────▼──────┐  ┌──────────▼─────┐  ┌──────▼──────────┐
          │ Location Store  │  │  Trip DB        │  │ Notification    │
          │ (Redis Geo +   │  │  (PostgreSQL    │  │ Service (FCM /  │
          │  Cassandra)    │  │  + Citus shard) │  │ APNs gateway)   │
          └────────────────┘  └──────────┬──────┘  └─────────────────┘
                                         │
                              ┌──────────▼──────────┐
                              │  Payment Service     │
                              │  (Stripe/Braintree   │
                              │   integration)       │
                              └──────────────────────┘

          ┌──────────────────────────────────────────────────────┐
          │              OBSERVABILITY PLANE                      │
          │  Prometheus + Grafana │ Jaeger (tracing) │ ELK Stack  │
          └──────────────────────────────────────────────────────┘
```

**Component Roles**:

| Component | Role |
|---|---|
| API Gateway | Single ingress: TLS termination, JWT validation, rate limiting (100 req/s per rider, 20 req/s per driver), request routing |
| Trip Service | Owns the trip state machine. Writes trips to PostgreSQL with optimistic locking; publishes `trip-events` to Kafka |
| Driver Service | Manages driver availability status (OFFLINE/AVAILABLE/ON_TRIP), ingests location updates, exposes driver-lookup API |
| Matching Service | Queries Redis Geo for nearby drivers, scores candidates, calls Driver Service to attempt atomic assignment |
| Fare/Pricing Service | Computes fare estimates and final fares; runs surge algorithm over H3 hexagons; reads supply/demand counters |
| Location Store | Redis Geo for current driver positions (hot); Cassandra for GPS time-series (warm/cold) |
| Trip DB | PostgreSQL (Citus for horizontal sharding) — authoritative source for trips, riders, drivers |
| Payment Service | Wraps Stripe SDK; stores payment method tokens; handles charge, refund, driver payout |
| Notification Service | Consumes Kafka notification-events; fans out to FCM (Android), APNs (iOS), SMS (Twilio) |
| Kafka | Decouples high-throughput location ingestion from consumers; enables replay; buffers notification fan-out |

**Primary Use-Case Data Flow — Rider Requests a Trip**:

1. Rider opens app → app fetches fare estimate from Fare Service (reads H3 surge map from Redis).
2. Rider taps "Request" → POST `/v1/trips` to Trip Service; Trip Service creates trip record in state `REQUESTED`.
3. Trip Service publishes `TRIP_REQUESTED` event to Kafka topic `trip-events`.
4. Matching Service consumes event → queries Redis Geo (`GEORADIUS` around pickup, r=5 km) → returns N candidate driver IDs.
5. Matching Service scores candidates (distance, rating, vehicle type) → selects top driver.
6. Matching Service calls Driver Service to CAS (compare-and-swap) driver status from `AVAILABLE → ASSIGNED`.
7. On success, Trip Service updates trip to state `DRIVER_ASSIGNED`; sends driver metadata to rider via WebSocket.
8. Driver app receives push notification with trip details.
9. Driver accepts → Trip Service transitions to `DRIVER_EN_ROUTE`.
10. Driver location updates stream via Kafka → Location Store → rider app receives via WebSocket.
11. Driver arrives → trip transitions to `RIDER_PICKED_UP` → `TRIP_IN_PROGRESS`.
12. Driver ends trip → Trip Service transitions to `COMPLETED`; publishes `TRIP_COMPLETED` event.
13. Payment Service consumes event → charges rider → publishes `PAYMENT_SUCCESS` event.
14. Notification Service sends receipt push to rider.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- USERS (partitioned by role for read performance)
-- ============================================================
CREATE TABLE riders (
    rider_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    phone           VARCHAR(20) UNIQUE NOT NULL,
    email           VARCHAR(255) UNIQUE,
    full_name       VARCHAR(255) NOT NULL,
    profile_photo   VARCHAR(512),            -- S3 URL
    rating          NUMERIC(3,2) DEFAULT 5.0,
    rating_count    INT DEFAULT 0,
    home_address    JSONB,                   -- {lat, lng, label}
    work_address    JSONB,
    default_payment_method_id UUID,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    updated_at      TIMESTAMPTZ DEFAULT NOW(),
    is_active       BOOLEAN DEFAULT TRUE
);

CREATE TABLE drivers (
    driver_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    phone           VARCHAR(20) UNIQUE NOT NULL,
    email           VARCHAR(255) UNIQUE,
    full_name       VARCHAR(255) NOT NULL,
    license_number  VARCHAR(50) NOT NULL,
    rating          NUMERIC(3,2) DEFAULT 5.0,
    rating_count    INT DEFAULT 0,
    status          VARCHAR(20) DEFAULT 'OFFLINE',  -- OFFLINE|AVAILABLE|ON_TRIP
    current_lat     DOUBLE PRECISION,
    current_lng     DOUBLE PRECISION,
    last_location_at TIMESTAMPTZ,
    city_id         INT NOT NULL,
    vehicle_id      UUID,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    is_active       BOOLEAN DEFAULT TRUE,
    CONSTRAINT driver_status_check CHECK (status IN ('OFFLINE','AVAILABLE','ON_TRIP'))
);

CREATE INDEX idx_drivers_status_city ON drivers (city_id, status);

-- ============================================================
-- VEHICLES
-- ============================================================
CREATE TABLE vehicles (
    vehicle_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    driver_id       UUID REFERENCES drivers(driver_id),
    make            VARCHAR(50) NOT NULL,
    model           VARCHAR(50) NOT NULL,
    year            SMALLINT NOT NULL,
    color           VARCHAR(30) NOT NULL,
    license_plate   VARCHAR(20) NOT NULL,
    vehicle_type    VARCHAR(20) NOT NULL,    -- STANDARD|XL|LUX|SHARED
    capacity        SMALLINT NOT NULL,
    is_active       BOOLEAN DEFAULT TRUE,
    CONSTRAINT vehicle_type_check CHECK (vehicle_type IN ('STANDARD','XL','LUX','SHARED'))
);

-- ============================================================
-- TRIPS
-- ============================================================
CREATE TABLE trips (
    trip_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    rider_id            UUID NOT NULL REFERENCES riders(rider_id),
    driver_id           UUID REFERENCES drivers(driver_id),
    vehicle_id          UUID REFERENCES vehicles(vehicle_id),
    status              VARCHAR(30) NOT NULL DEFAULT 'REQUESTED',
    vehicle_type        VARCHAR(20) NOT NULL,

    -- Pickup
    pickup_lat          DOUBLE PRECISION NOT NULL,
    pickup_lng          DOUBLE PRECISION NOT NULL,
    pickup_address      TEXT,
    pickup_h3_index     BIGINT,             -- H3 resolution-9 cell

    -- Dropoff
    dropoff_lat         DOUBLE PRECISION NOT NULL,
    dropoff_lng         DOUBLE PRECISION NOT NULL,
    dropoff_address     TEXT,

    -- Timing
    requested_at        TIMESTAMPTZ DEFAULT NOW(),
    driver_assigned_at  TIMESTAMPTZ,
    driver_arrived_at   TIMESTAMPTZ,
    started_at          TIMESTAMPTZ,
    completed_at        TIMESTAMPTZ,
    cancelled_at        TIMESTAMPTZ,
    cancel_reason       VARCHAR(100),
    cancelled_by        VARCHAR(10),       -- RIDER|DRIVER|SYSTEM

    -- Fare
    estimated_fare_usd  NUMERIC(8,2),
    surge_multiplier    NUMERIC(4,2) DEFAULT 1.0,
    base_fare_usd       NUMERIC(8,2),
    distance_km         NUMERIC(8,3),
    duration_minutes    NUMERIC(8,2),
    final_fare_usd      NUMERIC(8,2),
    promo_discount_usd  NUMERIC(8,2) DEFAULT 0,
    tolls_usd           NUMERIC(8,2) DEFAULT 0,

    -- Route
    polyline            TEXT,               -- encoded polyline

    city_id             INT NOT NULL,
    payment_method_id   UUID,

    CONSTRAINT trip_status_check CHECK (status IN (
        'REQUESTED','DRIVER_ASSIGNED','DRIVER_EN_ROUTE',
        'RIDER_PICKED_UP','TRIP_IN_PROGRESS','COMPLETED','CANCELLED'
    ))
);

-- Shard key for Citus: city_id
SELECT create_distributed_table('trips', 'city_id');

CREATE INDEX idx_trips_rider    ON trips (rider_id, requested_at DESC);
CREATE INDEX idx_trips_driver   ON trips (driver_id, requested_at DESC);
CREATE INDEX idx_trips_status   ON trips (status, city_id);
CREATE INDEX idx_trips_h3       ON trips (pickup_h3_index);

-- ============================================================
-- TRIP STATE TRANSITIONS (audit log)
-- ============================================================
CREATE TABLE trip_state_log (
    id          BIGSERIAL,
    trip_id     UUID NOT NULL REFERENCES trips(trip_id),
    from_state  VARCHAR(30),
    to_state    VARCHAR(30) NOT NULL,
    actor       VARCHAR(10),            -- RIDER|DRIVER|SYSTEM
    transitioned_at TIMESTAMPTZ DEFAULT NOW(),
    metadata    JSONB
);

-- ============================================================
-- RATINGS
-- ============================================================
CREATE TABLE ratings (
    rating_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    trip_id     UUID NOT NULL REFERENCES trips(trip_id),
    rater_type  VARCHAR(10) NOT NULL,  -- RIDER|DRIVER
    rater_id    UUID NOT NULL,
    ratee_id    UUID NOT NULL,
    score       SMALLINT NOT NULL CHECK (score BETWEEN 1 AND 5),
    comment     TEXT,
    created_at  TIMESTAMPTZ DEFAULT NOW()
);

-- ============================================================
-- PAYMENT METHODS
-- ============================================================
CREATE TABLE payment_methods (
    payment_method_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    rider_id            UUID NOT NULL REFERENCES riders(rider_id),
    provider            VARCHAR(20) NOT NULL,   -- STRIPE|BRAINTREE
    provider_token      VARCHAR(255) NOT NULL,  -- Stripe PaymentMethod ID
    card_last4          CHAR(4),
    card_brand          VARCHAR(20),
    exp_month           SMALLINT,
    exp_year            SMALLINT,
    is_default          BOOLEAN DEFAULT FALSE,
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- ============================================================
-- SURGE PRICING CELLS (time-series, populated by Surge Engine)
-- ============================================================
CREATE TABLE surge_cells (
    h3_index        BIGINT NOT NULL,        -- H3 resolution-7 cell
    snapshot_at     TIMESTAMPTZ NOT NULL,
    city_id         INT NOT NULL,
    demand_count    INT NOT NULL,           -- open requests in last 5 min
    supply_count    INT NOT NULL,           -- available drivers in cell
    surge_multiplier NUMERIC(4,2) NOT NULL,
    PRIMARY KEY (h3_index, snapshot_at)
);

-- ============================================================
-- CITIES (configuration)
-- ============================================================
CREATE TABLE cities (
    city_id         SERIAL PRIMARY KEY,
    name            VARCHAR(100) NOT NULL,
    country_code    CHAR(2) NOT NULL,
    timezone        VARCHAR(50) NOT NULL,
    base_fare_usd   NUMERIC(6,2) NOT NULL,
    per_km_usd      NUMERIC(6,4) NOT NULL,
    per_min_usd     NUMERIC(6,4) NOT NULL,
    min_fare_usd    NUMERIC(6,2) NOT NULL,
    geo_boundary    GEOGRAPHY(POLYGON,4326),
    is_active       BOOLEAN DEFAULT TRUE
);
```

**GPS Time-Series (Cassandra)**:

```cql
CREATE TABLE driver_location_history (
    driver_id   UUID,
    trip_id     UUID,
    recorded_at TIMESTAMP,
    lat         DOUBLE,
    lng         DOUBLE,
    heading     FLOAT,
    speed_kmh   FLOAT,
    accuracy_m  FLOAT,
    PRIMARY KEY ((driver_id, trip_id), recorded_at)
) WITH CLUSTERING ORDER BY (recorded_at ASC)
  AND default_time_to_live = 7776000;  -- 90 days TTL
```

**Redis Geo (current driver positions)**:

```
GEOADD city:{city_id}:drivers:available  <lng> <lat>  <driver_id>
GEORADIUS city:{city_id}:drivers:available  <pickup_lng> <pickup_lat>  5  km  WITHCOORD WITHDIST COUNT 50 ASC
```

### Database Choice

| Option | Pros | Cons | Fit |
|---|---|---|---|
| **PostgreSQL + Citus** | ACID, rich SQL, mature, sharding via Citus extension, excellent for trips/payments | Vertical scale limits without Citus; operational complexity at shard boundaries | **Selected for trips, riders, drivers, payments** |
| MySQL / Aurora | ACID, AWS-managed, auto-scaling read replicas | Less powerful geospatial, JSON support inferior to PostgreSQL JSONB | Viable alternative |
| MongoDB | Flexible schema, native geospatial (2dsphere) | No multi-document ACID until 4.0, lower consistency guarantees | Not selected — trip ACID critical |
| Cassandra | Massive write throughput, tunable consistency, TTL built-in | No ACID, no secondary indices by default | **Selected for GPS time-series** — write-heavy, TTL-based, append-only |
| Redis (Geo) | Sub-millisecond GEORADIUS, in-memory speed | Volatile by default, limited dataset size | **Selected for hot driver positions** — latency-critical, frequently overwritten |
| DynamoDB | Serverless, auto-scale, good for key-value | Expensive at high throughput, query flexibility limited | Alternative to Cassandra for GPS |

**Selection Justification**:
- **PostgreSQL + Citus** for trips and payments: ACID is non-negotiable — a rider must never be double-charged and a driver must never be double-assigned. Citus provides horizontal sharding by `city_id`, which maps naturally to geographic data locality. PostgreSQL's JSONB covers flexible address and metadata fields.
- **Redis Geo** for real-time driver positions: `GEORADIUS` is O(N+log M) with sorted-set backing, giving sub-millisecond spatial queries. Driver positions are ephemeral and updated every 4 seconds — the dataset fits in memory (5 M drivers × 64 bytes ≈ 320 MB per city shard).
- **Cassandra** for GPS history: Append-only writes at 562 K/s with built-in TTL satisfy the 90-day retention requirement. Cassandra's wide-row model (`PARTITION KEY = (driver_id, trip_id)`) gives O(1) time-range scans per trip for replay.

---

## 5. API Design

All endpoints require `Authorization: Bearer <JWT>` unless noted. JWTs are RS256-signed, 1-hour expiry, with `sub` (user UUID), `role` (rider|driver|admin), `city_id`.

Rate limits enforced at API Gateway per `sub`:
- Riders: 60 req/min (burst 20/s)
- Drivers: 120 req/min (burst 40/s — location updates are frequent)

---

### Fare Estimate

```
GET /v1/fare-estimate
Query params:
  pickup_lat:    float (required)
  pickup_lng:    float (required)
  dropoff_lat:   float (required)
  dropoff_lng:   float (required)
  vehicle_type:  enum[STANDARD,XL,LUX,SHARED] (default: STANDARD)

Response 200:
{
  "vehicle_type": "STANDARD",
  "estimated_fare_usd": 12.50,
  "surge_multiplier": 1.4,
  "estimated_duration_min": 18,
  "estimated_distance_km": 7.2,
  "breakdown": {
    "base_fare": 2.50,
    "distance_fare": 5.76,
    "time_fare": 3.60,
    "surge_addition": 3.39,
    "promo_discount": -3.25
  },
  "eta_to_pickup_min": 4,
  "nearby_drivers_count": 7
}
```

### Request Ride

```
POST /v1/trips
Auth: Rider JWT

Body:
{
  "pickup_lat":   37.7749,
  "pickup_lng":   -122.4194,
  "dropoff_lat":  37.3382,
  "dropoff_lng":  -121.8863,
  "vehicle_type": "STANDARD",
  "payment_method_id": "pm_uuid_...",
  "promo_code":   "SAVE10"          // optional
}

Response 201:
{
  "trip_id":             "trip_uuid",
  "status":              "REQUESTED",
  "estimated_fare_usd":  12.50,
  "surge_multiplier":    1.4,
  "created_at":          "2026-04-09T18:00:00Z"
}

Errors:
  400 Bad Request     — invalid coordinates
  402 Payment Required — no valid payment method
  409 Conflict        — rider already has an active trip
  429 Too Many Requests
```

### Get Trip Status

```
GET /v1/trips/{trip_id}
Auth: Rider or Driver JWT (must be participant)

Response 200:
{
  "trip_id":           "trip_uuid",
  "status":            "DRIVER_EN_ROUTE",
  "driver": {
    "driver_id":       "drv_uuid",
    "full_name":       "Marcus T.",
    "rating":          4.92,
    "vehicle": {
      "make":          "Toyota",
      "model":         "Camry",
      "color":         "Silver",
      "license_plate": "7ABC123",
      "type":          "STANDARD"
    },
    "current_lat":     37.7812,
    "current_lng":     -122.4156,
    "eta_minutes":     3
  },
  "pickup_address":    "Market St & 7th St, SF",
  "dropoff_address":   "San Jose Convention Center",
  "estimated_fare_usd": 12.50
}
```

### Driver — Go Online / Update Location

```
PATCH /v1/driver/status
Auth: Driver JWT

Body:
{
  "status": "AVAILABLE",         // AVAILABLE | OFFLINE
  "lat":    37.7749,
  "lng":    -122.4194,
  "heading": 270.0,
  "speed_kmh": 0.0
}

Response 200:
{ "status": "AVAILABLE", "city_id": 1 }
```

### Driver — Accept / Decline Trip Request

```
POST /v1/trips/{trip_id}/accept
Auth: Driver JWT

Response 200:
{
  "trip_id":       "trip_uuid",
  "status":        "DRIVER_ASSIGNED",
  "rider": {
    "rider_id":    "rid_uuid",
    "first_name":  "Anya",
    "rating":      4.87
  },
  "pickup_lat":    37.7749,
  "pickup_lng":    -122.4194,
  "pickup_address": "Market St & 7th St, SF"
}

Errors:
  409 Conflict — trip already assigned to another driver
  410 Gone     — trip cancelled by rider before acceptance
```

### Trip State Transitions (Driver)

```
POST /v1/trips/{trip_id}/arrived       — Driver at pickup
POST /v1/trips/{trip_id}/start         — Rider in vehicle
POST /v1/trips/{trip_id}/complete      — Trip ended
POST /v1/trips/{trip_id}/cancel        — Driver cancels

All return:
{
  "trip_id": "...",
  "status":  "<new_status>",
  "timestamp": "2026-04-09T18:12:00Z"
}
```

### Cancel Trip (Rider)

```
POST /v1/trips/{trip_id}/cancel
Auth: Rider JWT

Body: { "reason": "Changed my mind" }

Response 200:
{
  "trip_id":             "trip_uuid",
  "status":              "CANCELLED",
  "cancellation_fee_usd": 5.00,   // non-zero if driver already en route
  "cancelled_by":        "RIDER"
}
```

### Submit Rating

```
POST /v1/trips/{trip_id}/ratings
Auth: Rider or Driver JWT

Body:
{
  "score":   5,
  "comment": "Smooth ride, great driver."
}

Response 201:
{ "rating_id": "rat_uuid", "score": 5 }
```

### Trip History

```
GET /v1/trips
Auth: Rider or Driver JWT
Query: page=1&limit=20&status=COMPLETED&from=2026-01-01&to=2026-04-09

Response 200:
{
  "trips": [ { ...trip summary... } ],
  "total": 142,
  "page": 1,
  "limit": 20,
  "next_cursor": "cursor_opaque_token"
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Trip State Machine & Consistency

**Problem It Solves**: A trip must progress through exactly one path of states. Two drivers must never be assigned to the same trip. A trip in `COMPLETED` state must not be modifiable. Race conditions arise when multiple driver acceptances hit the backend concurrently.

**Approaches Comparison**:

| Approach | Mechanism | Pros | Cons |
|---|---|---|---|
| Optimistic locking (version column) | SELECT ... WHERE version=N; UPDATE ... SET status=..., version=N+1 WHERE version=N | Simple, no lock held; works with connection pooling | Retry burden on conflicts; doesn't prevent all races without careful WHERE clause |
| Pessimistic locking | SELECT ... FOR UPDATE | Guarantees exclusivity; simple correctness proof | Holds DB lock for network round-trip; poor under high contention |
| Single-writer via Kafka partition | Trip writes serialized through single Kafka consumer per trip_id | No DB-level lock; naturally ordered | Added infra; latency of Kafka round-trip; consumer must be idempotent |
| Distributed lock (Redis/Zookeeper) | SETNX trip:{id}:lock, TTL 5s | Works across stateless services | Redis lock is advisory; fencing token needed for true safety |
| **Database CAS + constraints** | Explicit status column + CHECK + NOT NULL + DB-level unique constraint on (trip_id, driver_id) WHERE status='DRIVER_ASSIGNED' | ACID guarantees; no external infra; deterministic | Requires careful SQL; needs connection pool tuning |

**Selected: Database CAS (Compare-and-Swap) with PostgreSQL**

```sql
-- Atomic assignment: only succeeds if trip is still REQUESTED
UPDATE trips
SET    status             = 'DRIVER_ASSIGNED',
       driver_id          = $driver_id,
       vehicle_id         = $vehicle_id,
       driver_assigned_at = NOW()
WHERE  trip_id = $trip_id
  AND  status  = 'REQUESTED'  -- CAS condition
RETURNING trip_id, status;
-- If 0 rows returned → another driver won the race → return 409
```

**Implementation Detail**:

The state machine is enforced at two layers:

**Layer 1 — PostgreSQL constraint**:
```sql
CREATE FUNCTION validate_trip_transition()
RETURNS TRIGGER AS $$
DECLARE valid_transitions TEXT[][] := ARRAY[
    ARRAY['REQUESTED',        'DRIVER_ASSIGNED'],
    ARRAY['DRIVER_ASSIGNED',  'DRIVER_EN_ROUTE'],
    ARRAY['DRIVER_EN_ROUTE',  'RIDER_PICKED_UP'],
    ARRAY['RIDER_PICKED_UP',  'TRIP_IN_PROGRESS'],
    ARRAY['TRIP_IN_PROGRESS', 'COMPLETED'],
    ARRAY['REQUESTED',        'CANCELLED'],
    ARRAY['DRIVER_ASSIGNED',  'CANCELLED'],
    ARRAY['DRIVER_EN_ROUTE',  'CANCELLED']
];
BEGIN
  IF NOT ARRAY[OLD.status, NEW.status] = ANY(valid_transitions) THEN
    RAISE EXCEPTION 'Invalid trip transition: % -> %', OLD.status, NEW.status;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trip_state_guard
BEFORE UPDATE ON trips
FOR EACH ROW EXECUTE FUNCTION validate_trip_transition();
```

**Layer 2 — Service-level**:
The Trip Service validates allowed transitions in code (fast-fail before DB round-trip) and publishes a `trip_state_log` entry for each transition for auditing.

**Driver CAS for Assignment**:
```sql
-- In Driver Service: mark driver unavailable atomically
UPDATE drivers
SET    status = 'ON_TRIP'
WHERE  driver_id = $driver_id
  AND  status    = 'AVAILABLE'   -- CAS: only if still available
RETURNING driver_id;
-- If 0 rows → driver went offline or was assigned to another trip
```

**Interviewer Q&A**:

Q1: What happens if the Trip Service crashes after writing `DRIVER_ASSIGNED` to the DB but before publishing the Kafka event?
A: The Kafka publish is an "at-least-once" outbox pattern. We maintain a `trip_outbox` table written in the same transaction as the trip update. A separate outbox relay process polls for unshipped events and publishes them. On recovery, the event is published; consumers are idempotent (deduplicated by trip_id + event_type).

Q2: How do you prevent a driver from accepting a trip they were never offered?
A: The Matching Service maintains a Redis set `trip:{trip_id}:offered_drivers` with a 30-second TTL. The Trip Service's accept endpoint verifies driver_id is in this set before executing the CAS UPDATE. Outside the set → 403 Forbidden.

Q3: What if the CAS UPDATE succeeds but the HTTP response to the driver times out?
A: The driver retries `POST /trips/{id}/accept`. The Trip Service is idempotent: if the trip is already in `DRIVER_ASSIGNED` state with this driver's ID, it returns 200. If assigned to a different driver, it returns 409.

Q4: How do you handle a trip stuck in `DRIVER_ASSIGNED` if the driver never moves?
A: A background sweeper job runs every 30 seconds and finds trips in `DRIVER_ASSIGNED` where `driver_assigned_at < NOW() - 3 minutes` with no subsequent state change. It re-queues the trip to `REQUESTED` state and marks the driver `OFFLINE` after logging a no-show.

Q5: Why not use an event-sourcing approach where trips are purely a series of events?
A: Pure event sourcing is appealing for auditability but adds read complexity — every state query requires replaying the event log. Given that trip status is queried every few seconds during active trips, a materialized current-state table is much more efficient. We get audit benefits from `trip_state_log` (a simple append log) without full event sourcing overhead.

---

### 6.2 Surge Pricing Algorithm

**Problem It Solves**: At peak times (concerts, rain, rush hour) demand spikes locally. Without surge pricing, available drivers are exhausted instantly with no price signal to attract more drivers. Surge balances supply and demand, manages rider expectations with upfront transparency, and generates revenue to compensate drivers.

**Approaches Comparison**:

| Approach | Mechanism | Pros | Cons |
|---|---|---|---|
| City-wide multiplier | Single multiplier for entire city | Simple; easy to communicate | Too coarse — a concert in one neighborhood shouldn't affect airport pickups |
| Grid-based (fixed cells) | Divide city into N×M lat/lng grid | Simple implementation | Cells misalign with roads; edge artifacts at boundaries |
| **H3 hexagonal cells** | Uber's H3 library; hexagons tessellate the sphere without distortion | Uniform area, smooth boundaries, hierarchical resolution | Slightly more complex to implement; library dependency |
| Demand forecasting ML | Predict future surge with ML model | Proactive; smoother UX | Higher latency, model drift, harder to explain to regulators |
| Pure market pricing | Real-time auction-style pricing | Theoretically optimal allocation | Politically controversial; unpredictable for riders |

**Selected: H3 Hexagonal Cells with Time-Windowed Supply/Demand Ratio**

**Implementation Detail**:

```python
# Pseudocode for Surge Engine (runs every 60 seconds per city)

SURGE_CONFIG = {
    'resolution': 7,          # H3 res-7 ≈ 5.16 km² avg area
    'lookback_seconds': 300,  # 5-minute rolling window
    'min_supply': 3,          # Ignore cells with fewer than 3 drivers
    'breakpoints': [          # (demand/supply ratio -> multiplier)
        (1.0, 1.0),
        (1.5, 1.2),
        (2.0, 1.5),
        (2.5, 1.8),
        (3.0, 2.0),
        (4.0, 2.5),
        (5.0, 3.0),
    ],
    'max_multiplier': 4.0,
    'smoothing_alpha': 0.3,   # EMA smoothing to prevent multiplier thrashing
}

def compute_surge(city_id: int) -> Dict[int, float]:
    # Step 1: Count available drivers per H3 cell
    driver_positions = redis.georadius_all(f'city:{city_id}:drivers:available')
    supply = defaultdict(int)
    for driver_id, lat, lng in driver_positions:
        cell = h3.geo_to_h3(lat, lng, SURGE_CONFIG['resolution'])
        supply[cell] += 1

    # Step 2: Count open ride requests per H3 cell (last 5 min)
    recent_requests = db.query("""
        SELECT pickup_h3_index, COUNT(*) as cnt
        FROM trips
        WHERE status = 'REQUESTED'
          AND city_id = %s
          AND requested_at > NOW() - INTERVAL '5 minutes'
        GROUP BY pickup_h3_index
    """, city_id)
    demand = {row.pickup_h3_index: row.cnt for row in recent_requests}

    # Step 3: Compute multiplier per cell
    multipliers = {}
    for cell in set(supply.keys()) | set(demand.keys()):
        s = supply.get(cell, 0)
        d = demand.get(cell, 0)
        if s < SURGE_CONFIG['min_supply']:
            # Neighbor-fill: use parent H3 cell aggregates
            parent = h3.h3_to_parent(cell, SURGE_CONFIG['resolution'] - 1)
            s = supply_by_parent.get(parent, 1)
            d = demand_by_parent.get(parent, 0)

        ratio = d / max(s, 1)
        raw_multiplier = interpolate(SURGE_CONFIG['breakpoints'], ratio)
        raw_multiplier = min(raw_multiplier, SURGE_CONFIG['max_multiplier'])

        # EMA smoothing with previous multiplier
        prev = redis.hget(f'surge:{city_id}', cell) or 1.0
        smoothed = (1 - SURGE_CONFIG['smoothing_alpha']) * float(prev) \
                 + SURGE_CONFIG['smoothing_alpha'] * raw_multiplier
        multipliers[cell] = round(smoothed, 2)

    # Step 4: Write to Redis (hot path for fare estimates)
    pipeline = redis.pipeline()
    for cell, mult in multipliers.items():
        pipeline.hset(f'surge:{city_id}', cell, mult)
        pipeline.expire(f'surge:{city_id}', 120)  # 2-min TTL safety net
    pipeline.execute()

    # Step 5: Write snapshot to surge_cells table for analytics
    db.bulk_insert('surge_cells', [
        (cell, now, city_id,
         demand.get(cell,0), supply.get(cell,0), mult)
        for cell, mult in multipliers.items()
    ])
    return multipliers
```

**Fare estimate using surge**:
```python
def get_surge_multiplier(city_id: int, lat: float, lng: float) -> float:
    cell = h3.geo_to_h3(lat, lng, resolution=7)
    mult = redis.hget(f'surge:{city_id}', cell)
    if mult is None:
        # Fallback: walk up H3 hierarchy
        for res in [6, 5]:
            parent = h3.h3_to_parent(cell, res)
            mult = redis.hget(f'surge:{city_id}:res{res}', parent)
            if mult: break
    return float(mult or 1.0)
```

**Rider transparency**: Before confirming, the rider sees the exact multiplier (e.g., "2.1× surge") and must explicitly confirm the upfront price. This is a regulatory requirement in many jurisdictions.

**Interviewer Q&A**:

Q1: How do you prevent multiplier oscillation — surge going 1.0 → 3.0 → 1.0 every minute?
A: EMA (exponential moving average) smoothing with α=0.3 damps rapid swings. Additionally, we apply a "ratchet" rule: the multiplier can increase by at most 0.5× per tick and decrease by at most 0.3× per tick, preventing the system from reacting to a single burst of simultaneous requests.

Q2: How do you handle the boundary between two H3 cells where one is at 2.0× and the adjacent is 1.0×?
A: The multiplier applied to a rider is based on their *pickup location* cell, not the destination. We additionally "blur" adjacent cells by averaging each cell's multiplier with a weighted average of k-ring neighbors (k=1), which smooths the boundary effect without losing localization.

Q3: Why cap the multiplier at 4.0×? What about truly extreme scenarios (hurricane evacuation)?
A: Hard caps are configurable per city and per event type. Uber's public policy allows higher caps during declared public emergencies to attract drivers, but many regulators (NYC, London) impose legal caps. The config table in cities has a `max_surge_multiplier` field. During natural disasters, the system can be manually overridden to 1.0× (no surge) at the operations team's discretion.

Q4: How do you measure whether surge pricing actually works (attracts more drivers)?
A: Key metrics: driver-online-rate by H3 cell before/after surge event (should increase), trip request fulfillment rate (should remain high despite price signal reducing demand), and median time-to-match in surging cells. A/B testing with held-out cities establishes causal attribution.

Q5: Could a competitor scrape your surge data from the API and undercut you dynamically?
A: Surge multipliers are returned per-request and are not a public feed. Rate limiting (60 req/min per account) and fraud signals (high-frequency unauthenticated probing) throttle scraping. The multiplier is also not static — it changes every 60 seconds — so even scraped data is stale within a minute.

---

### 6.3 Geospatial Driver Discovery & H3 Indexing

**Problem It Solves**: Given a pickup location, find all available drivers within radius R efficiently. With 1.5 M online drivers globally, a naive scan is O(N). Spatial indexing reduces this to O(log N + k) where k is the result set.

**Approaches Comparison**:

| Approach | Data Structure | Query Complexity | Write Complexity | Notes |
|---|---|---|---|---|
| Naive scan | Array | O(N) | O(1) | Unusable at scale |
| Bounding-box (lat/lng range) | B-tree on lat+lng | O(N) filtered | O(log N) | False positives; no actual distance |
| PostGIS 2D index | GiST/SP-GiST | O(log N + k) | O(log N) | Excellent but DB round-trip latency |
| **Redis GEOADD/GEORADIUS** | Geohash in sorted set | O(N+log M) | O(log N) | Sub-ms, in-memory, naturally clustered |
| H3 cell lookup | Hash map of cell → driver set | O(k_cells) | O(1) per update | Complementary to Redis Geo; used for batch analytics |
| Quadtree | Tree | O(log N + k) | O(log N) | Custom implementation complexity |

**Selected: Redis Geo (primary) + H3 cell index (surge/batch)**

**Redis Geo Implementation**:

```
# Driver comes online / moves:
GEOADD city:1:drivers:available  -122.4194  37.7749  "drv_uuid_abc"

# Matching service query:
GEORADIUS city:1:drivers:available  -122.4194  37.7749  5  km
    WITHCOORD WITHDIST COUNT 50 ASC
# Returns: [(driver_id, dist_km, lng, lat), ...]

# Driver goes on trip / offline:
ZREM city:1:drivers:available  "drv_uuid_abc"
GEOADD city:1:drivers:on_trip  -122.4194  37.7749  "drv_uuid_abc"
```

**Why separate sorted sets per status**: Querying `AVAILABLE` drivers only requires one `GEORADIUS` call on the available set, rather than filtering a combined set. Separate sets also give O(1) cardinality counts (`ZCARD`) per status, which feeds the surge supply counter.

**Implementation Detail — Driver Location Update Pipeline**:

```
Driver App → HTTPS POST /v1/driver/location (every 4 s)
         → API Gateway → Driver Location Service
         → 1. GEOADD city:{id}:drivers:{status} (Redis) — synchronous, <1 ms
         → 2. Publish to Kafka topic "location-updates" — async, non-blocking
             → Consumer 1: Cassandra write (GPS history)
             → Consumer 2: Active trip WebSocket push (rider tracking)
             → Consumer 3: ETA recalculation if on-trip
```

The critical path (Redis GEOADD) completes in <1 ms. Everything else is async and non-blocking to the driver app.

**Interviewer Q&A**:

Q1: Redis is in-memory. What happens when it restarts and you lose all driver positions?
A: Redis is configured with RDB snapshot every 60 seconds plus AOF persistence (fsync every second). Recovery time is under 30 seconds. During that window, we fail over to a Redis replica (Redis Sentinel or Redis Cluster with automatic failover). Drivers also send location updates every 4 seconds — so even after a cold restart, the position data self-heals within 4–8 seconds.

Q2: How do you handle a city with 200,000 active drivers (Mumbai, Beijing)?
A: Redis sorted sets scale to tens of millions of members. 200 K drivers × 64 bytes per Geo entry ≈ 12 MB — trivially fits in memory. `GEORADIUS` on 200 K members with COUNT 50 returns in <5 ms. If needed, we can shard by H3 parent cell (e.g., separate sorted sets per district) and fan out queries in parallel.

Q3: GEORADIUS is deprecated in newer Redis versions (use GEOSEARCH instead). Does this matter?
A: Yes, in Redis 6.2+, `GEOSEARCH` replaces `GEORADIUS`. The semantics are equivalent. Our driver location service abstracts the Redis call behind a `NearbyDriversFinder` interface, so upgrading the Redis client to use `GEOSEARCH` is a one-line change with no schema migration.

Q4: How do you handle drivers near city boundaries — a driver physically in City A appearing in City B's sorted set?
A: Each driver is assigned to exactly one `city_id` when they go online, based on a point-in-polygon check against the `cities.geo_boundary` column (PostGIS). Their location updates are routed to that city's Redis key. When they cross the boundary (road trip scenario), the Driver Service detects the geofence exit and migrates the driver to the new city's sorted set atomically (ZREM from old, GEOADD to new in a MULTI/EXEC block).

Q5: How would you support sub-city zones for vehicle type availability (e.g., Lux only available downtown)?
A: Each vehicle type maintains its own sorted set: `city:{id}:drivers:available:{type}`. `GEORADIUS` queries are type-specific. Zone restrictions are enforced by a geofence lookup on the driver's position at online time; the Driver Service adds them only to the appropriate type sets within permitted zones.

---

## 7. Scaling

### Horizontal Scaling

| Tier | Mechanism | Notes |
|---|---|---|
| API Gateway | Multiple ALB instances behind Route53 geolocation DNS | Auto-scales based on connection count; 10–50 instances |
| Trip Service | Stateless pods behind ALB; K8s HPA on CPU/RPS | 10–100 pods; sticky sessions not needed (state in DB) |
| Matching Service | Stateless; scales with trip request rate | Can be partitioned by city_id to avoid cross-city fan-out |
| Driver Location Service | Stateless; fan-out to Redis shards | Scale horizontally; Kafka consumer group provides natural sharding |
| Surge Engine | Single-threaded per city (worker per city_id) | Embarrassingly parallelizable; 10,000 cities = 10,000 workers (K8s Jobs) |
| Notification Service | K8s workers; partition by notification_type | Scale to 10,000 notifications/s trivially |
| Payment Service | Stateless; bounded by Stripe API rate limits | Stripe supports 10,000 req/s on enterprise plans |

### DB Sharding

**PostgreSQL (Citus)**:
- Shard key: `city_id` for trips, payments, ratings
- 32 shards across 8 physical Citus workers (4 shards/worker)
- Co-located tables: `trips`, `trip_state_log`, `ratings` all sharded by `city_id`
- Reference tables (replicated to all workers): `cities`, `vehicle_types`

**Shard calculation**:
- 20 M trips/day; avg trip size 2 KB; 40 GB/day writes
- 32 shards → ~1.25 GB/day per shard → manageable for SSD storage

**Cassandra (GPS history)**:
- Partition key: `(driver_id, trip_id)` → data for one trip on one driver co-located
- 6-node Cassandra cluster; replication factor 3; quorum reads/writes
- Natural write distribution: driver IDs are UUIDs → uniform hash distribution

### Replication

| Database | Strategy | RPO | RTO |
|---|---|---|---|
| PostgreSQL (primary) | Synchronous streaming replication to 1 replica (same AZ); async replication to cross-region read replica | 0 (sync) | < 30 s auto-failover via Patroni |
| Redis (driver positions) | Redis Sentinel: 1 master + 2 replicas; automatic failover | < 4 s (one update cycle) | < 10 s |
| Cassandra (GPS) | RF=3, NetworkTopologyStrategy across 2 AZs | ~1 s (async) | Immediate (reads from replicas) |
| Kafka | 3 brokers; replication factor 3; `acks=all` for producer | 0 (acks=all) | Broker failure transparent to clients |

### Caching Strategy

| Cache Layer | What Is Cached | TTL | Invalidation |
|---|---|---|---|
| Redis L1 (fare params) | City pricing tables (base fare, per-km, per-min) | 5 min | On admin config change → publish invalidation event |
| Redis L2 (surge cells) | H3 surge multiplier map per city | 90 s (refreshed every 60 s) | Overwritten by Surge Engine |
| Redis L3 (driver profiles) | Driver name, rating, vehicle info for active drivers | 60 s | Invalidated on rating update |
| Application-level (JVM heap) | City config, pricing rules | 30 s | TTL-based expiry |
| CDN (CloudFront) | Map tile assets, static API docs | 24 hrs | Cache-Control headers |

### CDN

- Map tiles are served from a tile server (MapBox or self-hosted) behind CloudFront with 24-hour TTL.
- Profile photos (driver, rider) stored in S3, served via CloudFront with long TTL + cache-busted URL on update.
- Static assets (iOS/Android binary updates via OTA) served from CloudFront edge locations.

### Interviewer Q&A

Q1: How do you handle a city that suddenly becomes extremely large (e.g., adding São Paulo with 5 M users)?
A: City-level sharding is the key design decision. Adding São Paulo means creating a new city_id shard in Citus (or adding a new Citus worker node if capacity is needed), a new Redis Geo key, and a new Surge Engine worker. The city goes through a load test with synthetic traffic before launch. The architecture is designed so adding a city has zero impact on existing cities.

Q2: Your Kafka topic `location-updates` receives 562,000 messages/second. How do you partition it?
A: Partition by `driver_id` hash. A driver's location updates must be ordered (for GPS trace integrity). With 562 K msg/s and each Kafka partition handling ~50 K msg/s, we need at least 12 partitions. In practice, we use 128 partitions for headroom, giving ~4,400 msg/s per partition — well within Kafka's capability.

Q3: How do you scale the WebSocket layer for real-time driver tracking?
A: WebSocket connections are long-lived and stateful. Each WebSocket server (HAProxy + Node.js or Envoy) handles ~50,000 concurrent connections. With 500,000 active trips → 10 WebSocket servers. A Redis Pub/Sub channel per active trip (`trip:{id}:location`) is used as the message bus; the WebSocket server subscribes to channels for its connected clients. Alternatively, we use a sticky session approach (consistent hashing on trip_id to WebSocket server).

Q4: Describe your database connection pooling strategy.
A: Each microservice uses PgBouncer (transaction mode) between its connection pool and PostgreSQL. PgBouncer multiplexes 1000 application-level connections down to 100 server connections per Citus worker, staying within PostgreSQL's optimal connection limit (~200 per worker). Connection limits by service: Trip Service (30), Matching Service (20), Fare Service (20), others (10 each).

Q5: How does geo-distributed deployment work for a user in Singapore vs. a user in New York?
A: We deploy the full stack (API, Trip Service, Matching, Location, DB) in each major region (us-east-1, eu-west-1, ap-southeast-1). Route53 latency-based routing directs each user to the nearest region. City data is region-specific; Singapore trips stay in ap-southeast-1. Cross-region traffic only occurs for global analytics, ML model training, and fraud detection (async).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| Trip Service pod crash | New trip requests fail | K8s liveness probe → pod restart in <10 s | HPA ensures 3+ replicas; K8s restarts crashed pod |
| PostgreSQL primary failure | Trip writes fail | Patroni health check every 2 s; auto-promotes replica | Replica promotion in <30 s; ops notified via PagerDuty |
| Redis master failure | Driver matching stalls | Redis Sentinel detects unreachable master in <10 s | Automatic replica promotion; <10 s downtime |
| Kafka broker failure | Location updates buffered | Kafka leader election; RF=3 | RF=3 means 2 remaining brokers serve traffic; transparent to producers |
| Stripe API outage | Post-trip payment fails | HTTP 5xx from Stripe SDK | Queue payment in `payment_retry_queue`; retry with exponential backoff up to 24 hrs; rider notified of delay |
| Driver app GPS failure | Driver appears frozen on map | `last_location_at` stale > 30 s | Driver marked as potentially offline; matching service excludes stale drivers |
| Matching Service timeout | Rider waits > 5 s for match | Distributed tracing alert | Re-queue trip request to matching pool; circuit breaker opens Matching Service dependency from Trip Service after 10 consecutive failures |
| City-wide surge data missing | All rides priced at 1.0× | Surge Engine heartbeat monitor | Fallback: use last known multiplier from Redis (cached 120 s); if missing, use 1.0× (conservative) |
| Cross-AZ network partition | Intermittent failures | ALB health checks | Services run in 3 AZs; RDS Multi-AZ; requests route around unhealthy AZ |

### Retries & Idempotency

**All writes are idempotent**:
- Trip creation: Client sends idempotency key (`X-Idempotency-Key: <UUID>`). Trip Service stores key→trip_id in Redis with 24-hour TTL. Duplicate requests return the original response.
- Payment charge: Payment Service uses Stripe's built-in idempotency keys (trip_id as key). Duplicate charges are deduplicated by Stripe.
- Rating submission: Unique constraint on `(trip_id, rater_id)` in DB — second insert is rejected with 409.

**Retry Policy (internal service calls)**:
- 3 retries with exponential backoff: 100 ms, 200 ms, 400 ms
- Only retry on 429, 503, 504 — never on 4xx (client errors)
- Circuit breaker: opens after 5 failures in 10 s; half-open after 30 s

### Circuit Breaker Configuration

```
Matching Service → Driver Location Service:
  failure_threshold: 5 errors in 10 s window
  open_state_duration: 30 s
  fallback: return cached nearby-drivers list from 30 s ago
  half-open: allow 1 probe request per 10 s

Trip Service → Payment Service:
  failure_threshold: 3 errors in 5 s
  open_state_duration: 60 s
  fallback: queue payment for async processing; return success to driver
  alert: PagerDuty P2 if circuit stays open > 2 min
```

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Owner |
|---|---|---|---|
| `trip.requests.rate` | Counter | < 50% of expected for city | Matching On-call |
| `trip.match.latency_p99` | Histogram | > 5 s | Matching On-call |
| `driver.location.staleness_p95` | Gauge | > 15 s | Location On-call |
| `trip.state.transition.errors` | Counter | > 5/min | Trip On-call |
| `payment.charge.success_rate` | Gauge | < 98% | Payments On-call |
| `surge.multiplier.max_by_city` | Gauge | > 3.5× (alert ops) | City Ops |
| `matching.no_drivers_found.rate` | Counter | > 10% of requests | Matching On-call |
| `kafka.consumer.lag` | Gauge per group | > 100K messages | Infra On-call |
| `redis.geo.command.latency_p99` | Histogram | > 10 ms | Infra On-call |
| `db.query.latency_p99` (trips table) | Histogram | > 100 ms | DB On-call |
| `trip.cancellation.rate` | Gauge | > 25% (unusual demand pattern) | City Ops |
| `driver.online.count` by city | Gauge | < 20% of normal for time-of-day | City Ops |

### Distributed Tracing

Every inbound request receives a `trace_id` (W3C TraceContext) at the API Gateway. All internal RPC calls (gRPC) and Kafka messages propagate the trace context.

Critical trace: `POST /v1/trips` (ride request):
```
[API Gateway] → [Trip Service: create_trip 5ms]
              → [Fare Service: estimate_fare 12ms]
              → [Matching Service: find_drivers 180ms]
                  → [Redis GEORADIUS 1ms]
                  → [Driver Service: score_candidates 8ms]
                  → [Driver Service: CAS assign 3ms]
              → [Trip Service: update_status 4ms]
              → [Kafka publish 2ms]
              Total: ~215ms p50
```

Traces exported to Jaeger; sampled at 10% in production (100% on errors). Stored 7 days in Elasticsearch.

### Logging

- Structured JSON logs (log level, service, trace_id, trip_id, city_id, duration_ms, error_code)
- Log aggregation: Fluentd → Elasticsearch → Kibana
- Retention: hot (7 days in ES), warm (30 days in S3 compressed), cold (1 year in Glacier)
- Sensitive fields (card numbers, SSNs) are masked at the Fluentd layer before writing to ES

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A (Selected) | Option B (Rejected) | Reason |
|---|---|---|---|
| Trip consistency | PostgreSQL CAS + DB trigger for state machine | Distributed saga with compensating transactions | Sagas add complexity without benefit — all trip data lives in one DB shard |
| Driver position store | Redis Geo (in-memory) | PostGIS in PostgreSQL | Redis GEORADIUS is 10–100× faster than PostGIS for hot spatial queries; position data is ephemeral |
| GPS history storage | Cassandra with TTL | PostgreSQL time-series partition | Cassandra handles 562K writes/s natively; PostgreSQL requires expensive partitioning and vacuuming |
| Geospatial indexing | H3 hexagonal cells | S2 spherical geometry cells | H3 is battle-tested by Uber in production; open-source library with Python/Java/Go bindings |
| Surge pricing granularity | H3 resolution-7 (5 km² cells) | Resolution-9 (0.1 km² cells) | Higher resolution creates sparse demand data; resolution-7 provides enough driver supply per cell for statistical significance |
| Driver matching | Greedy best-match (top-1 by score) | Hungarian algorithm (global optimal) | Hungarian runs in O(N³); greedy p99 quality is within 5% of optimal but runs in O(k log k) where k≈50 candidates |
| Real-time communication | WebSocket (long-lived TCP) | HTTP polling every 2 s | WebSocket pushes each location update; polling wastes bandwidth and adds 0–2 s latency for location freshness |
| Fare calculation | Upfront pricing (shown before confirm) | Post-trip meter pricing | Upfront pricing improves rider conversion and removes bill shock; requires good ETA accuracy |
| Payment retry | Async queue with 24-hr retry window | Synchronous payment at trip end | Blocking driver on payment adds latency; async retry with low failure rate (<0.3%) is better UX |
| DB sharding key | city_id | rider_id or driver_id | city_id co-locates all trip data for a city; enables city-level analytics without cross-shard joins |

---

## 11. Follow-up Interview Questions

Q1: How would you design the Pool/Shared ride feature where two riders share a vehicle?
A: Pool rides require the Matching Service to evaluate whether a new POOL request's route is compatible with an in-progress POOL trip (via route deviation scoring — if pickup and dropoff add < X minutes to the existing passenger's ETA). The trip data model gains a `pool_group_id` and a `trip_passengers` join table. Revenue is split by a pooling algorithm that prices each leg separately using marginal cost pricing.

Q2: Describe the payment settlement flow for a completed trip.
A: On `TRIP_COMPLETED` event: (1) Fare Service computes final fare from actual distance/duration + surge. (2) Payment Service charges the rider's stored Stripe PaymentMethod with the trip's idempotency key. (3) On success, a `driver_earnings` ledger entry is created (fare minus Uber's platform fee, typically 25%). (4) Driver payouts are batched weekly via Stripe Connect or ACH bank transfer. Tolls are added from a third-party toll detection service (e.g., TollGuru API) based on the actual route polyline.

Q3: How would you handle driver fraud (fake GPS, GPS spoofing)?
A: Multi-layer: (1) Speed validation — if a driver's GPS delta implies > 200 km/h, the update is flagged and discarded. (2) Road-snapping validation — positions off all known roads trigger a low-confidence flag. (3) Trip verification: actual distance (GPS polyline) vs. estimated distance must be within 20% tolerance. (4) Behavioral ML model trained on historical trip data flags outliers in real time. (5) Severe violations trigger automatic driver account suspension pending manual review.

Q4: How does the system perform under a "Black Friday"-style demand surge (10× normal traffic)?
A: The architecture is pre-scaled for 3× normal using K8s HPA warm pools and pre-provisioned RDS read replicas. For 10× (planned large event — Super Bowl, New Year's Eve): (1) We pre-scale all stateless services the day before. (2) Surge pricing naturally shed excess demand. (3) We use K8s Cluster Autoscaler to add nodes within 3 minutes. (4) Circuit breakers prevent cascading failure. (5) We potentially enable a request queue (Redis-backed) for matching requests rather than immediately failing with 503.

Q5: How would you add support for scheduled rides (book a ride for 6 AM tomorrow)?
A: A new trip status `SCHEDULED` is added. A scheduler service (cron-based, using quartz or K8s CronJob) processes scheduled rides by firing the match request 5 minutes before the pickup time. The challenge is driver availability — drivers are not pre-assigned; instead, a push notification is sent to eligible nearby drivers at T-10 minutes offering a premium. Fare is locked at booking time with a price guarantee.

Q6: Walk me through how you would design the driver earnings dashboard.
A: Driver earnings are written to a `driver_earnings` ledger table (append-only, sharded by driver_id). A read model is maintained in a separate OLAP store (Redshift or BigQuery) via Kafka → Flink streaming aggregations. The dashboard queries pre-aggregated views: earnings by day/week/month, trip count, peak earning hours. Redis caches the current week's running total for real-time display in the driver app.

Q7: How do you design for multi-currency support (USD, EUR, GBP, INR)?
A: Each city has a `currency_code` and `currency_symbol`. All fare calculations happen in the city's local currency (stored in the pricing table). Internally, amounts are stored as integers in the smallest currency unit (cents/paise) to avoid floating-point errors. Stripe handles multi-currency charging natively. The fare estimate API returns both `amount` (integer, e.g., 1250) and `currency` (e.g., "USD") fields. Exchange rates are fetched from a financial data API (Open Exchange Rates) and cached for 1 hour.

Q8: How would you detect and handle a "ghost trip" — a trip that started but the driver's app crashed and never ended?
A: A background sweeper runs every 5 minutes checking for trips in `TRIP_IN_PROGRESS` where `last_location_at > 30 minutes` AND no `completed_at`. The sweeper: (1) Attempts to contact the driver via push notification asking them to end the trip. (2) After 60 minutes with no response, auto-completes the trip using the last known GPS endpoint and computes fare from available telemetry. (3) Flags the trip for manual review and sends an apologetic notification to the rider.

Q9: Describe the cancellation fee logic.
A: The cancellation policy is: no fee if cancelled within 2 minutes of driver assignment, OR if the driver is more than 8 minutes away (driver fault), OR if the driver hasn't moved in 3 minutes after acceptance. Otherwise, a flat cancellation fee (city-configured, typically $5) is charged. Logic: Trip Service on cancel transition → checks `driver_assigned_at`, driver ETA from ETA service, and driver movement (from last 3 location updates). Decision is logged to `trip_state_log.metadata` for dispute resolution.

Q10: How would you ensure GDPR compliance for European users?
A: (1) Right to erasure: A GDPR delete job anonymizes PII (rider name, phone, email → NULL; substituted with random ID) in all tables without deleting trip financial records (legal retention required). GPS history is deleted immediately. (2) Data export: An async job collects all data for a user_id across all shards and returns a JSON archive. (3) Data residency: European users' data is stored exclusively in eu-west-1 (Paris). (4) Consent management: Explicit opt-in for marketing communications, stored in a separate consent service.

Q11: Why use WebSockets for driver location updates to the rider, rather than Server-Sent Events (SSE)?
A: Both are viable. SSE is simpler (HTTP/1.1 compatible, auto-reconnect) and sufficient for unidirectional push. We use WebSocket because the same channel also handles bidirectional events: trip status changes, driver chat messages, ETA updates, and cancellation notifications — all flowing in both directions. The multiplexed bidirectional channel is worth the added complexity. On degraded networks (mobile), both protocols include reconnection logic.

Q12: How would you implement "favorite drivers" — the ability for a rider to always request their preferred driver first?
A: A `rider_favorite_drivers` table stores (rider_id, driver_id, priority). During matching, after the GEORADIUS query, the Matching Service checks if any returned driver IDs appear in the rider's favorites list and applies a score bonus (e.g., 200% of normal score). If a favorite driver is available and within 2× the distance of the closest non-favorite, they are selected first. This is a soft preference — if no favorite is available, normal matching proceeds.

Q13: How does the ETA shown to the rider during trip account for real-time traffic?
A: The ETA Service is queried at trip start and re-evaluated every 30 seconds using the driver's current GPS position. The ETA calculation uses A* on a road graph where edge weights are dynamically updated from: (1) historical average speed by hour/day for each road segment, (2) real-time probe data from all active Uber drivers on those roads (anonymized speed telemetry), and (3) third-party traffic APIs (Google Maps Traffic Layer, HERE Maps). The ETA is pushed to the rider via WebSocket on each recalculation.

Q14: What consistency model do you use for the driver's status (AVAILABLE/ON_TRIP) and why?
A: Strong consistency via PostgreSQL CAS UPDATE. The driver status is the critical invariant — two trips must never be assigned to the same driver. Eventual consistency here would cause double-assignment, which is a severe UX failure. The latency cost of a synchronous DB write (3–5 ms with connection pooling) is acceptable. We cache the driver status in Redis for read-heavy workloads (matching service), but writes always go to PostgreSQL first, then invalidate the cache.

Q15: How would you design a feature to show riders the "expected availability" heat map — showing where rides will be easy to get in the next 15 minutes?
A: This is a demand forecasting feature. Offline: Train an LSTM or XGBoost model on historical trip origin density by H3 cell, day-of-week, time-of-day, weather, events calendar, and current online driver positions. Model is trained daily. Online inference: The model is served via a TensorFlow Serving endpoint. Every 5 minutes, a batch job runs inference for all H3 cells in each active city and writes predicted demand scores to Redis. The rider app fetches a GeoJSON overlay from a dedicated `GET /v1/availability-heatmap` endpoint and renders it on the map.

---

## 12. References & Further Reading

1. **Uber Engineering Blog — H3: Uber's Hexagonal Hierarchical Spatial Index** — https://eng.uber.com/h3/
2. **Uber Engineering Blog — Designing Uber's Architecture for Scale** — https://eng.uber.com/uber-tech-stack/
3. **Uber Engineering Blog — Surge Pricing for Uber: A Machine Learning Perspective** — https://eng.uber.com/surge-pricing/
4. **Uber Engineering Blog — Schemaless: Adding Structure to Uber's Datastore Using MySQL** — https://eng.uber.com/schemaless-part-one/
5. **Lyft Engineering Blog — How Lyft Discovers, Stores, and Represents Real-Time Location Data** — https://eng.lyft.com/how-lyft-discovers-openstreetmap-data-1f05c3e50af
6. **Redis Geo Commands Documentation** — https://redis.io/docs/data-types/geo/
7. **Citus Data — Sharding PostgreSQL Tables** — https://docs.citusdata.com/en/stable/sharding/
8. **Apache Kafka Documentation — Producer Configs (acks, retries)** — https://kafka.apache.org/documentation/#producerconfigs
9. **Martin Kleppmann — Designing Data-Intensive Applications** (O'Reilly, 2017) — Chapter 5 (Replication), Chapter 9 (Consistency)
10. **Google S2 Geometry Library** — https://s2geometry.io/ (alternative to H3)
11. **Stripe API Documentation — Idempotent Requests** — https://stripe.com/docs/api/idempotent_requests
12. **AWS Aurora PostgreSQL — Read Replicas and Multi-AZ** — https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/
13. **Patroni: A Template for PostgreSQL HA** — https://github.com/zalando/patroni
14. **Jaeger Distributed Tracing** — https://www.jaegertracing.io/docs/
15. **GDPR Article 17 — Right to Erasure** — https://gdpr.eu/right-to-be-forgotten/
