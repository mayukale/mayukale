# System Design: Real-Time Driver Location Tracking

---

## 1. Requirement Clarifications

### Functional Requirements

1. **High-frequency GPS ingestion**: Accept GPS updates from millions of drivers every 4 seconds while online.
2. **Real-time position serving**: Serve the current position of any active driver with < 2-second staleness.
3. **Live rider tracking**: Push driver location updates to the rider tracking their assigned driver's approach in real time.
4. **Geofencing**: Detect when a driver enters or exits a defined geographic boundary (airport zone, city boundary, restricted zone) and trigger events.
5. **Historical trip replay**: Replay a completed trip's GPS trace at configurable speed — used for dispute resolution and driver earnings verification.
6. **GPS time-series storage**: Store every GPS point for every trip for 90 days; archive summaries permanently.
7. **Road snapping**: Snap raw GPS coordinates to the nearest road segment for map display and ETA accuracy.
8. **Battery optimization signals**: Serve adaptive sampling rate instructions to the driver app to reduce GPS sampling frequency when the driver is stationary or on a highway.
9. **Speed and heading telemetry**: Store and expose speed and heading alongside position for ETA recalculation.
10. **Driver proximity query**: Given a pickup location, return all online available drivers within radius R.

### Non-Functional Requirements

- **Write throughput**: 500,000+ GPS writes per second at peak (1.5 M online drivers × 1 update / 4 s × 1.5 peak factor).
- **Read latency**: Current driver position served in < 10 ms (p99) for matching and rider tracking.
- **Data durability**: GPS history must be durable (no point loss once acknowledged to the driver app).
- **Availability**: 99.99% — location system is on the critical path for matching and active ride tracking.
- **Freshness**: Position data in the serving layer must reflect the latest GPS point within 5 seconds end-to-end.
- **Storage efficiency**: Raw GPS points compressed to minimize storage costs; 90-day TTL on raw data.
- **Scalability**: Linear horizontal scaling; must handle 10× traffic spikes (events, rain) without pre-warming.
- **Battery impact**: The location system's protocol must minimize driver phone battery drain — target < 5% battery/hour from location service alone.

### Out of Scope

- Navigation turn-by-turn routing (that is the ETA service's responsibility)
- Rider location tracking (riders do not stream GPS)
- Asset tracking for non-vehicle use cases (bikes, scooters)
- Fraud detection ML pipeline (consumes location events but is a separate system)
- Traffic computation from driver probe data (separate analytics pipeline)

---

## 2. Users & Scale

### User Types

| User Type | Interaction with Location System | Scale |
|---|---|---|
| Driver (GPS producer) | Sends GPS update every 4 s while online | 5 M total; 1.5 M peak concurrent |
| Rider (position consumer) | Receives pushed driver position during active trip | 500 K active trips at peak |
| Matching Service | Queries nearby available drivers by geolocation | 462 queries/s (at 20 M trips/day) |
| ETA Service | Reads latest driver position + speed for re-routing | ~500 queries/s (1 per active trip per 30 s) |
| Ops Dashboard | Reads driver density maps for city health | ~100 concurrent dashboards |
| Dispute Resolution Tool | Replays GPS trace of a historical trip | ~50 simultaneous replays |

### Traffic Estimates

**Assumptions**:
- 5 M registered drivers; 30% online at peak → 1.5 M concurrent online drivers
- GPS update frequency: 1 update / 4 s while online
- Peak factor: 1.5× over average during 5–7 PM commute
- Average GPS payload: 64 bytes (compressed protobuf)
- Active trips at peak: 500,000 simultaneous riders being tracked
- Rider location push: 1 push / 4 s per active trip

| Metric | Calculation | Result |
|---|---|---|
| GPS writes/s (avg) | 1.5 M drivers × 0.25 updates/s | 375,000 writes/s |
| GPS writes/s (peak) | 375,000 × 1.5 | 562,500 writes/s |
| GPS ingest bandwidth | 562,500 × 64 B | ~36 MB/s inbound |
| Rider location pushes/s | 500,000 active trips × 0.25 push/s | 125,000 push/s |
| Rider push bandwidth | 125,000 × 200 B | ~25 MB/s outbound |
| Matching GEORADIUS queries/s | 462 req/s (from uber_lyft.md) | 462 queries/s |
| ETA recalculation reads/s | 500,000 active trips / 30 s refresh | ~16,667 reads/s |
| Geofence evaluations/s | 562,500 updates × 0.1 (10% near a fence) | ~56,250/s |
| GPS points/day | 375,000/s × 86,400 s | ~32.4 B points/day |
| Trip GPS points/day | 20 M trips × 300 pts/trip avg | 6 B points/day (trip-associated) |

### Latency Requirements

| Operation | P50 Target | P99 Target | Why |
|---|---|---|---|
| GPS write acknowledged to driver app | < 50 ms | < 200 ms | Affects driver app responsiveness; battery efficiency |
| Current position served to matching | < 1 ms | < 10 ms | On critical matching path; Redis in-memory |
| Driver position pushed to rider | < 500 ms | < 2 s end-to-end from GPS event | Rider tracking UX |
| Geofence event generated | < 1 s | < 3 s after GPS point arrives | Near-real-time but not hard real-time |
| Historical replay start | < 2 s | < 5 s | Dispute tool; not latency-critical |

### Storage Estimates

**Assumptions**:
- Raw GPS point: 48 bytes uncompressed (driver_id 16B, trip_id 16B, timestamp 8B, lat 4B, lng 4B)
- Cassandra stores with replication factor 3 → 3× raw on-disk
- Compression ratio with Cassandra's LZ4: ~2.5× → effective on-disk per point ≈ 58 bytes (48 × 3 / 2.5)
- After 90 days raw TTL: aggregate summaries (1 row/trip, ~200 bytes) kept forever

| Data Type | Record Size | Daily Volume | Daily Storage (RF=3, compressed) | 90-Day Storage |
|---|---|---|---|---|
| Raw GPS points (all drivers) | 48 B | 32.4 B/day | ~750 GB/day | 67.5 TB |
| Trip GPS only (on-trip) | 48 B | 6 B/day | ~140 GB/day | 12.6 TB |
| Current driver positions (Redis) | 64 B | 1.5 M records | 96 MB total | N/A (in-memory) |
| Trip route summaries (permanent) | 200 B | 20 M/day | 4 GB/day | 29 TB/year |
| Geofence event log | 100 B | ~500 K events/day | 50 MB/day | 4.5 GB (90 days) |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Driver → Location Ingest Service | 562,500 writes/s × 64 B | ~36 MB/s in |
| Location Service → Redis (position write) | 562,500 × 32 B (GEOADD cmd overhead) | ~18 MB/s |
| Location Service → Kafka | 562,500 × 64 B | ~36 MB/s |
| Kafka → Cassandra consumers | Same as Kafka ingest | ~36 MB/s |
| Kafka → WebSocket consumers (active trips) | 125,000 push/s × 200 B | ~25 MB/s |
| WebSocket → Rider apps | ~25 MB/s | ~25 MB/s out |
| Matching Service GEORADIUS responses | 462 queries/s × 50 drivers × 32 B | ~750 KB/s |

---

## 3. High-Level Architecture

```
  ┌─────────────────────────────────────────────────────────────────┐
  │                    DRIVER MOBILE APP                            │
  │   GPS Provider (every 4s) → Location Manager SDK               │
  │   Adaptive rate controller (speed-based sampling)              │
  └──────────────────────────────┬──────────────────────────────────┘
                                 │ HTTPS/2 + protobuf  (TLS 1.3)
                                 │ (batched: up to 3 pts per request)
                         ┌───────▼──────────────────────┐
                         │     API GATEWAY / LB          │
                         │  (AWS ALB, TLS termination,   │
                         │   JWT auth, rate limiting)    │
                         └───────┬──────────────────────┘
                                 │
                    ┌────────────▼──────────────────┐
                    │    LOCATION INGEST SERVICE     │
                    │  (stateless, horizontally      │
                    │   scaled, Go microservice)     │
                    │  • Validates GPS payload       │
                    │  • Applies road-snap (async)   │
                    │  • Speed/heading validation    │
                    └─────┬─────────────┬────────────┘
                          │             │
          ┌───────────────▼───┐   ┌─────▼────────────────────┐
          │  REDIS GEO CLUSTER│   │   KAFKA: location-updates │
          │ (current position │   │  Partitioned by driver_id │
          │  per city, sorted │   │  562,500 msg/s peak       │
          │  set per status)  │   │  Retention: 24 hours      │
          └─────────┬─────────┘   └──────┬───────────────────┘
                    │                    │
                    │         ┌──────────┼──────────────────────┐
                    │         │          │                       │
          (read by  │  ┌──────▼───┐ ┌───▼──────┐   ┌───────────▼────┐
          Matching  │  │ Cassandra│ │WebSocket │   │ Geofence        │
          + ETA     │  │ Consumer │ │Dispatcher│   │ Evaluation Svc  │
          Services) │  │(GPS hist)│ │(rider    │   │ (H3-based, per  │
                    │  │          │ │tracking) │   │ active fence)   │
                    │  └──────────┘ └───┬──────┘   └───────┬─────────┘
                    │                   │                   │
                    │  ┌────────────────▼──┐    ┌──────────▼──────────┐
                    │  │ CASSANDRA CLUSTER  │    │ GEOFENCE EVENT BUS  │
                    │  │ (GPS time-series   │    │ (Kafka: geo-events)  │
                    │  │  90-day TTL,       │    │ → Trip Service       │
                    │  │  RF=3)             │    │ → Surge Engine       │
                    │  └────────────────────┘    └─────────────────────┘
                    │
          ┌─────────▼──────────────────────────────────────┐
          │           QUERY / READ SERVICES                 │
          │  • Matching Service: GEORADIUS (Redis)          │
          │  • ETA Service: latest position + speed         │
          │  • Trip Replay Service: Cassandra time-range    │
          │  • Ops Dashboard: aggregated density tiles      │
          └────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────┐
  │                   BATTERY OPTIMIZER SERVICE                      │
  │  Reads driver speed/state from Redis → pushes config update      │
  │  to driver app via WebSocket (sampling rate instruction)         │
  └──────────────────────────────────────────────────────────────────┘
```

**Component Roles**:

| Component | Role |
|---|---|
| Location Ingest Service | Validates, rate-limits, and fans out GPS updates. Synchronously writes to Redis Geo (sub-ms). Async publishes to Kafka. |
| Redis Geo Cluster | Sub-millisecond current-position store. One sorted set per city per driver status. Supports GEORADIUS queries for matching. |
| Kafka `location-updates` | Durable, high-throughput event bus. Decouples ingest from consumers. Partitioned by driver_id for ordering. |
| Cassandra Consumer | Consumes from Kafka; bulk-inserts into Cassandra GPS history table. Batches 500 rows per insert for efficiency. |
| WebSocket Dispatcher | Consumes from Kafka; for each GPS point on an active trip, pushes position update to rider's WebSocket connection. |
| Geofence Evaluation Service | Stateful service that holds active geofence definitions in memory; evaluates each GPS point against relevant fences using H3 containment check. |
| Battery Optimizer Service | Analyzes driver movement patterns (speed, heading variance) to recommend adaptive sampling rates. Pushes config to driver app. |
| Trip Replay Service | Reads Cassandra for a given (driver_id, trip_id) time range; streams GPS points back to caller at requested playback speed. |

---

## 4. Data Model

### Entities & Schema

```cql
-- ============================================================
-- CASSANDRA: GPS Time-Series (primary history store)
-- ============================================================
CREATE KEYSPACE location WITH replication = {
    'class': 'NetworkTopologyStrategy',
    'us-east-1': 3,
    'eu-west-1': 3
};

-- All GPS points for a driver during a specific trip
CREATE TABLE location.driver_gps_history (
    driver_id       UUID,
    trip_id         UUID,           -- NULL for non-trip (positioning/idle) updates
    recorded_at     TIMESTAMP,
    lat             DOUBLE,
    lng             DOUBLE,
    heading         FLOAT,          -- degrees 0–360
    speed_kmh       FLOAT,
    accuracy_m      FLOAT,          -- GPS accuracy radius
    altitude_m      FLOAT,
    raw_provider    TEXT,           -- GPS | NETWORK | FUSED
    road_snapped_lat DOUBLE,        -- after async road-snap
    road_snapped_lng DOUBLE,
    road_segment_id  BIGINT,        -- FK to road graph edge (optional)
    PRIMARY KEY ((driver_id, trip_id), recorded_at)
) WITH CLUSTERING ORDER BY (recorded_at ASC)
  AND default_time_to_live = 7776000  -- 90 days
  AND compaction = { 'class': 'TimeWindowCompactionStrategy',
                     'compaction_window_unit': 'DAYS',
                     'compaction_window_size': 1 }
  AND compression = { 'sstable_compression': 'LZ4Compressor' };

-- Index for lookups by driver without knowing trip_id
-- (Use a separate wide-row table for this access pattern)
CREATE TABLE location.driver_gps_by_driver (
    driver_id       UUID,
    recorded_at     TIMESTAMP,
    trip_id         UUID,
    lat             DOUBLE,
    lng             DOUBLE,
    heading         FLOAT,
    speed_kmh       FLOAT,
    PRIMARY KEY (driver_id, recorded_at)
) WITH CLUSTERING ORDER BY (recorded_at DESC)
  AND default_time_to_live = 604800;   -- 7-day TTL for this access pattern

-- ============================================================
-- CASSANDRA: Current driver snapshot (denormalized for speed)
-- ============================================================
CREATE TABLE location.driver_current_position (
    driver_id       UUID PRIMARY KEY,
    lat             DOUBLE,
    lng             DOUBLE,
    heading         FLOAT,
    speed_kmh       FLOAT,
    city_id         INT,
    driver_status   TEXT,           -- AVAILABLE | ON_TRIP | OFFLINE
    current_trip_id UUID,
    updated_at      TIMESTAMP
);
-- Note: This table is a fallback; Redis Geo is the hot path.
```

```sql
-- ============================================================
-- POSTGRESQL: Geofence definitions
-- ============================================================
CREATE TABLE geofences (
    geofence_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            VARCHAR(100) NOT NULL,
    geofence_type   VARCHAR(30) NOT NULL,   -- AIRPORT_ZONE|CITY_BOUNDARY|RESTRICTED|SURGE_ZONE
    city_id         INT REFERENCES cities(city_id),
    boundary        GEOGRAPHY(POLYGON, 4326) NOT NULL,
    h3_cells        JSONB,                  -- Pre-computed H3 cells for fast containment check
    h3_resolution   SMALLINT DEFAULT 9,
    action          VARCHAR(50) NOT NULL,   -- NOTIFY|RESTRICT|AUTO_ACCEPT
    is_active       BOOLEAN DEFAULT TRUE,
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_geofences_city ON geofences (city_id) WHERE is_active = TRUE;
CREATE INDEX idx_geofences_boundary ON geofences USING GIST (boundary);

-- ============================================================
-- POSTGRESQL: Geofence events log
-- ============================================================
CREATE TABLE geofence_events (
    event_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    geofence_id     UUID NOT NULL REFERENCES geofences(geofence_id),
    driver_id       UUID NOT NULL,
    event_type      VARCHAR(10) NOT NULL,   -- ENTER | EXIT
    lat             DOUBLE PRECISION NOT NULL,
    lng             DOUBLE PRECISION NOT NULL,
    occurred_at     TIMESTAMPTZ NOT NULL,
    trip_id         UUID,
    processed_at    TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_geoevents_driver ON geofence_events (driver_id, occurred_at DESC);

-- ============================================================
-- POSTGRESQL: Battery optimization configs (pushed to driver apps)
-- ============================================================
CREATE TABLE driver_sampling_config (
    driver_id           UUID PRIMARY KEY,
    update_interval_ms  INT DEFAULT 4000,       -- normal: 4 s
    min_distance_m      INT DEFAULT 5,           -- suppress update if moved < 5 m
    last_config_push    TIMESTAMPTZ,
    config_reason       VARCHAR(50)              -- STATIONARY|HIGHWAY|URBAN|TRIP_START
);
```

**Redis Data Structures**:

```
# Current position (Geo sorted set, one per city per status)
Key:     city:{city_id}:drivers:{status}
Type:    Sorted Set (Geo encoding)
Example: GEOADD city:1:drivers:available  -122.4194  37.7749  "drv-uuid-abc"
TTL:     None (managed by Driver Service on status change)

# Driver speed & heading (Hash, TTL 30s)
Key:     driver:{driver_id}:telemetry
Type:    Hash
Fields:  speed_kmh, heading, updated_at, trip_id, city_id
TTL:     30 seconds (auto-expires if driver goes offline)

# Active trip → driver mapping (Hash, TTL = trip duration)
Key:     trip:{trip_id}:driver_location
Type:    Hash
Fields:  lat, lng, heading, speed_kmh, updated_at
TTL:     4 hours (trips shouldn't exceed this)

# WebSocket session registry (which WS server owns this rider's connection)
Key:     rider:{rider_id}:ws_server
Type:    String
Value:   ws-server-07.internal:8080
TTL:     60 seconds (refreshed by heartbeat)
```

### Database Choice

| Option | Write Throughput | Latency | TTL Support | Query Flexibility | Operational Cost |
|---|---|---|---|---|---|
| **Cassandra** | 500K+ writes/s (native) | 1–5 ms writes | Native TTL per row | Limited (partition-key queries) | Medium |
| PostgreSQL TimescaleDB | ~100K writes/s | 5–20 ms writes | Partition + cron | Full SQL | Low (familiar) |
| InfluxDB | 500K+ writes/s | 1–3 ms | Native retention policies | InfluxQL/Flux | Medium |
| Kafka (as storage) | 1M+ writes/s | <1 ms produce | Retention by time/size | Stream only; needs secondary | Low |
| DynamoDB | 500K+ writes/s | 1–10 ms | TTL attribute | Key-value + sparse | High (cost) |
| **Redis Geo** | 500K+ ops/s | <1 ms | Per-key TTL | GEORADIUS only | Medium |
| HBase | 500K+ writes/s | 5–20 ms | TTL via coprocessors | Column-family scans | High |

**Selected Databases**:

**Redis Geo** — hot path for current driver positions. Justification: `GEORADIUS` is the sole critical read pattern for matching and is O(N + log M) on a sorted set, executing in < 1 ms. Redis's in-memory model is the only technology that meets the < 10 ms p99 read requirement at 562 K writes/s. The dataset fits in memory: 1.5 M drivers × 64 bytes = 96 MB per city cluster.

**Apache Cassandra** — GPS time-series history. Justification: (1) Cassandra's LSM-tree architecture converts random writes into sequential I/O, enabling 500 K+ writes/s without write amplification. (2) The `TimeWindowCompactionStrategy` aligns compaction with TTL windows, minimizing read amplification for recent data and efficiently tombstoning expired rows. (3) Native per-row TTL eliminates the need for a separate cleanup job. (4) The `(driver_id, trip_id)` partition key provides O(1) access to a specific trip's GPS trace for replay, exactly matching the dispute resolution access pattern.

**PostgreSQL** — geofence definitions and event log. Justification: Geofences are low-volume, infrequently written but require complex spatial queries (PostGIS `ST_Contains`, `ST_Intersects`). PostgreSQL's GiST spatial index handles polygon containment checks efficiently. ACID guarantees ensure geofence configuration changes are atomic.

---

## 5. API Design

All driver endpoints require `Authorization: Bearer <Driver-JWT>`. Rate limit: 120 req/min per driver (burst: 30 req/s).

---

### Location Update (Driver → Server)

```
POST /v1/driver/location
Auth: Driver JWT

Body (protobuf-encoded, Content-Type: application/x-protobuf):
{
  "updates": [                  // batch of up to 3 points
    {
      "lat":          37.7749,
      "lng":          -122.4194,
      "heading":      270.5,
      "speed_kmh":    35.2,
      "accuracy_m":   4.5,
      "altitude_m":   14.0,
      "recorded_at":  "2026-04-09T18:00:04.123Z",  // client timestamp
      "provider":     "FUSED"   // GPS|NETWORK|FUSED
    }
  ]
}

Response 200 (protobuf):
{
  "server_time":           "2026-04-09T18:00:04.210Z",
  "next_update_interval_ms": 4000,     // Battery optimizer instruction
  "min_distance_m":          5,        // Suppress if moved < 5m
  "active_trip_id":          null      // null if not on trip
}

Errors:
  400  — malformed coordinates (lat outside [-90,90], lng outside [-180,180])
  401  — invalid JWT
  403  — driver marked OFFLINE by system (cannot submit location)
  429  — rate limit exceeded (>30 updates/min)
```

### Get Current Driver Position (Internal — Matching / ETA Services)

```
GET /v1/internal/drivers/{driver_id}/position
Auth: Service-to-service JWT (internal only)

Response 200:
{
  "driver_id":    "drv-uuid",
  "lat":          37.7749,
  "lng":          -122.4194,
  "heading":      270.5,
  "speed_kmh":    35.2,
  "updated_at":   "2026-04-09T18:00:04.000Z",
  "staleness_ms": 1200         // ms since last update
}

Errors:
  404  — driver not found or offline
  503  — Redis unavailable (fallback to Cassandra)
```

### Nearby Drivers Query (Internal — Matching Service)

```
GET /v1/internal/drivers/nearby
Auth: Service-to-service JWT

Query params:
  lat:          float (required)
  lng:          float (required)
  radius_km:    float (default: 5.0, max: 20.0)
  vehicle_type: enum[STANDARD,XL,LUX,SHARED] (optional)
  limit:        int (default: 50, max: 200)
  city_id:      int (required)

Response 200:
{
  "drivers": [
    {
      "driver_id":   "drv-uuid-1",
      "lat":         37.7801,
      "lng":         -122.4112,
      "distance_km": 0.87,
      "heading":     180.0,
      "speed_kmh":   0.0,
      "updated_at":  "2026-04-09T18:00:03.000Z"
    },
    ...
  ],
  "query_lat":    37.7749,
  "query_lng":    -122.4194,
  "radius_km":    5.0,
  "total_found":  23
}
```

### Trip GPS Replay (Dispute / Audit)

```
GET /v1/trips/{trip_id}/gps-replay
Auth: Admin or Rider/Driver (own trips only) JWT
Query: driver_id=<uuid>&playback_speed=2 (1=real-time, 2=2×, max 60)

Response: Server-Sent Events (text/event-stream)
  data: {"lat":37.7749,"lng":-122.4194,"heading":270,"speed_kmh":35,"ts":"..."}
  data: {"lat":37.7751,"lng":-122.4196,"heading":271,"speed_kmh":34,"ts":"..."}
  ...
  data: {"event":"trip_end","final_lat":37.3382,"final_lng":-121.8863}
```

### Geofence Management (Admin)

```
POST /v1/geofences
Auth: Admin JWT

Body:
{
  "name":         "SFO Airport Pickup Zone",
  "type":         "AIRPORT_ZONE",
  "city_id":      1,
  "boundary":     { "type": "Polygon", "coordinates": [[...]] },
  "h3_resolution": 9,
  "action":       "NOTIFY"
}
Response 201: { "geofence_id": "gf-uuid", ... }

GET /v1/geofences?city_id=1
Response 200: { "geofences": [...] }

DELETE /v1/geofences/{geofence_id}
Response 204
```

### Adaptive Sampling Rate Override (Battery Optimizer → Driver App)

This is pushed via WebSocket, not pulled. Format:
```json
{
  "type":                    "SAMPLING_CONFIG_UPDATE",
  "update_interval_ms":      8000,
  "min_distance_m":          20,
  "reason":                  "STATIONARY",
  "valid_for_seconds":       60
}
```

---

## 6. Deep Dive: Core Components

### 6.1 High-Throughput GPS Ingestion Pipeline

**Problem It Solves**: Accept 562,500 GPS writes per second reliably, with sub-200ms end-to-end acknowledgment to the driver app, while simultaneously fanning out to Redis (for matching), Cassandra (for history), and WebSocket dispatchers (for rider tracking) — without the ingestion path blocking on any downstream consumer.

**Approaches Comparison**:

| Approach | Throughput | Latency | Durability | Complexity |
|---|---|---|---|---|
| Synchronous write to DB | Low (~10K/s per node) | 5–20 ms | High | Low |
| Write to Redis only | Very high (500K+/s) | < 1 ms | Low (volatile) | Low |
| Kafka-first, async consumers | Very high (1M+/s) | 1–5 ms produce | High (RF=3) | Medium |
| **Redis synchronous + Kafka async** | Very high | 1–2 ms Redis + 2–5 ms Kafka async | High (Kafka) | Medium |
| UDP ingest with best-effort delivery | Highest | <1 ms | None | Low |
| gRPC streaming | High | 1–3 ms | Medium | Medium |

**Selected: Redis synchronous (critical path) + Kafka async (durable fan-out)**

**Implementation Detail**:

```go
// Location Ingest Service — Go pseudo-code

type GPSUpdate struct {
    DriverID    string
    TripID      string
    Lat, Lng    float64
    Heading     float32
    SpeedKmh    float32
    AccuracyM   float32
    RecordedAt  time.Time
    Provider    string
    CityID      int
    DriverStatus string
}

func HandleLocationUpdate(ctx context.Context, updates []GPSUpdate) (*IngestResponse, error) {
    if len(updates) == 0 || len(updates) > 3 {
        return nil, ErrBadRequest("batch size must be 1–3")
    }

    latestUpdate := updates[len(updates)-1]   // most recent point

    // --- Validation (fast path, in-process) ---
    for _, u := range updates {
        if err := validateGPSPoint(u); err != nil {
            return nil, err   // 400 Bad Request
        }
        if err := validateSpeed(u); err != nil {
            // Speed > 300 km/h: discard silently, do not error
            // to prevent driver app retry storms
            log.Warn("suspicious GPS speed", u)
            continue
        }
    }

    // --- Step 1: Update Redis Geo (synchronous, <1 ms) ---
    // This is the critical path — matching service needs it immediately
    pipe := redisClient.Pipeline()
    pipe.GeoAdd(ctx,
        fmt.Sprintf("city:%d:drivers:%s", latestUpdate.CityID, latestUpdate.DriverStatus),
        &redis.GeoLocation{
            Name:      latestUpdate.DriverID,
            Latitude:  latestUpdate.Lat,
            Longitude: latestUpdate.Lng,
        },
    )
    // Update telemetry hash
    pipe.HSet(ctx, fmt.Sprintf("driver:%s:telemetry", latestUpdate.DriverID),
        "speed_kmh", latestUpdate.SpeedKmh,
        "heading",   latestUpdate.Heading,
        "updated_at", latestUpdate.RecordedAt.UnixMilli(),
        "city_id",   latestUpdate.CityID,
    )
    pipe.Expire(ctx, fmt.Sprintf("driver:%s:telemetry", latestUpdate.DriverID), 30*time.Second)
    if _, err := pipe.Exec(ctx); err != nil {
        // Redis write failed — circuit breaker; still publish to Kafka
        circuitBreaker.RecordFailure("redis")
        log.Error("redis write failed", err)
    }

    // --- Step 2: Publish to Kafka (async, non-blocking to caller) ---
    // Publish all points in batch
    go func() {
        for _, u := range updates {
            msg, _ := proto.Marshal(&pb.GPSPoint{
                DriverId:   u.DriverID,
                TripId:     u.TripID,
                Lat:        u.Lat,
                Lng:        u.Lng,
                Heading:    u.Heading,
                SpeedKmh:   u.SpeedKmh,
                RecordedAt: u.RecordedAt.UnixMilli(),
                CityId:     int32(u.CityID),
            })
            kafkaProducer.Produce(&kafka.Message{
                TopicPartition: kafka.TopicPartition{
                    Topic:     &topicLocationUpdates,
                    Partition: kafka.PartitionAny,
                },
                Key:   []byte(u.DriverID),  // ensures ordered delivery per driver
                Value: msg,
            }, nil)
        }
    }()

    // --- Step 3: Determine adaptive sampling rate ---
    nextInterval := computeAdaptiveSamplingInterval(latestUpdate)

    return &IngestResponse{
        ServerTime:          time.Now(),
        NextUpdateIntervalMs: nextInterval,
        MinDistanceM:        computeMinDistance(latestUpdate),
    }, nil
}
```

**Kafka Consumer: Cassandra Writer** (batched bulk writes):

```go
func CassandraWriterConsumer(ctx context.Context) {
    batch := make([]*GPSPoint, 0, 500)
    ticker := time.NewTicker(100 * time.Millisecond)

    for {
        select {
        case msg := <-kafkaConsumer.Messages():
            point := unmarshal(msg)
            batch = append(batch, point)
            if len(batch) >= 500 {
                flushBatch(ctx, batch)
                batch = batch[:0]
            }
        case <-ticker.C:
            if len(batch) > 0 {
                flushBatch(ctx, batch)
                batch = batch[:0]
            }
        }
    }
}

func flushBatch(ctx context.Context, points []*GPSPoint) {
    // Cassandra unlogged batch (same partition — driver_id+trip_id)
    // Group by partition key first
    byPartition := groupByPartition(points)
    for _, group := range byPartition {
        batch := gocql.NewBatch(gocql.UnloggedBatch)
        for _, p := range group {
            batch.Query(`INSERT INTO driver_gps_history
                (driver_id, trip_id, recorded_at, lat, lng, heading, speed_kmh, accuracy_m)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
                p.DriverID, p.TripID, p.RecordedAt,
                p.Lat, p.Lng, p.Heading, p.SpeedKmh, p.AccuracyM)
        }
        cassandraSession.ExecuteBatch(batch)
    }
}
```

**Interviewer Q&A**:

Q1: Your Kafka producer is in a goroutine (`go func()`). What happens if Kafka is down for 30 seconds?
A: The Kafka producer library (librdkafka) has an internal queue (`queue.buffering.max.messages = 1,000,000`). If the broker is unreachable, messages queue in memory for up to `message.timeout.ms = 300000` (5 minutes) with automatic retry. The Location Ingest Service still responds success to the driver app (Redis was written). We monitor Kafka producer queue depth — an alert fires if it exceeds 100K messages. If Kafka stays down beyond 5 minutes, we fall back to writing GPS points to a local disk ring buffer (bounded 500 MB).

Q2: How do you handle clock skew between the driver's phone and the server?
A: Each GPS point includes a client-side `recorded_at` timestamp, and the server logs its own `received_at`. We compare the two and apply validation: if |received_at - recorded_at| > 30 seconds, we flag the point as suspicious but still store it with a `clock_skew_ms` field. For playback and billing, we use client-side timestamps (since they represent when the driver was actually at that location) but apply NTP-based correction if the skew exceeds 5 seconds. Cassandra uses client-side timestamps in the CQL INSERT to maintain correct time ordering even for late-arriving batched points.

Q3: The driver app sends batches of up to 3 GPS points per request. Why 3? Why not 10?
A: The batch size is a trade-off between request overhead (TLS handshake, JWT validation) and data freshness. At 4-second intervals, a batch of 3 covers 12 seconds of history — the maximum acceptable staleness for a GPS trace. Batches of 10 would cover 40 seconds, which would make the current-position data on Redis 40 seconds stale for drivers with spotty connectivity. We process the latest point in the batch first for Redis updates, then write all points to history storage. 3 is the sweet spot for connectivity-impaired environments (tunnels, basements) where the app queues failed sends.

Q4: How do you handle 500,000 simultaneous Cassandra writes per second without overwhelming the cluster?
A: The Cassandra Writer consumers are Kafka-backed and write in batched groups of 500 rows per unlogged batch. We group by partition key `(driver_id, trip_id)` before batching — an unlogged batch is only efficient when all rows share the same partition, as it avoids a coordinator-to-replica round-trip for each row. With 562,500 msg/s at 500/batch → 1,125 Cassandra batch operations/s. Each operation writes to the responsible replica set (RF=3, LOCAL_QUORUM = 2 nodes). A 6-node cluster handles ~50K writes/s each → 300K writes/s. For peak 562K, we scale to 12 nodes. Cassandra's LSM tree absorbs all writes into an in-memory MemTable and flushes to SSTables, making sustained write throughput the primary design goal.

Q5: How does the WebSocket dispatcher know which WebSocket server connection is holding the rider's session?
A: We maintain a `rider:{rider_id}:ws_server` key in Redis mapping rider_id to the WebSocket server's internal hostname and port. This key has a 60-second TTL refreshed by the WebSocket server's heartbeat. When the Kafka consumer in the WebSocket dispatcher receives a GPS update for trip_id, it looks up the rider_id from trip metadata (cached in Redis), then finds the WebSocket server, and sends the update via an internal HTTP/2 push call (or Redis Pub/Sub). Alternative: each WebSocket server subscribes to a Redis Pub/Sub channel `trip:{trip_id}:location` — only the server holding the rider's connection will forward the message. This avoids the lookup overhead at the cost of all servers receiving the message and discarding it.

---

### 6.2 Geofencing at Scale

**Problem It Solves**: A city has hundreds of defined geofences (airport pickup/dropoff zones, restricted neighborhoods, city entry boundaries, surge evaluation zones). Each of the 562,500 GPS updates per second must be evaluated against relevant fences and trigger events (ENTER/EXIT) within 3 seconds. A naive approach (check every GPS point against every polygon) is O(N × M) where N = updates/s and M = fences.

**Approaches Comparison**:

| Approach | Algorithm | Throughput | Latency | Accuracy | Memory |
|---|---|---|---|---|---|
| PostGIS ST_Contains per update | R-tree spatial index query | ~5,000/s | 5–20 ms | Exact | N/A |
| Grid-based precomputation | Hash grid cell → fence set | 500K+/s | <1 ms lookup | Approximate (boundary) | Medium |
| **H3 cell precomputation** | H3 cell → fence membership map | 500K+/s | <1 ms | Approximate (sub-cell) | Low |
| R-tree in memory (Java/Go) | In-process R-tree | 100K+/s | <1 ms | Exact | Medium |
| Bounding-box pre-filter + exact check | 2-pass | 100K+/s | 1–3 ms | Exact | Low |

**Selected: H3 Cell Precomputation with In-Memory Lookup**

**Implementation Detail**:

At startup (and on geofence config change), the Geofence Evaluation Service precomputes which H3 cells (at resolution 9, ≈ 0.1 km² each) are contained within or intersect each active geofence polygon. This creates an inverted index: `h3_cell → [geofence_id_1, geofence_id_2, ...]`.

```python
# Geofence precomputation (runs on fence create/update)
def precompute_fence_cells(geofence_id: str, polygon_geojson: dict, resolution: int = 9):
    # Fill the polygon with H3 cells at resolution 9
    cells = h3.polyfill_geojson(polygon_geojson, resolution)
    # Also get cells that partially overlap the boundary
    boundary_cells = set()
    for cell in cells:
        for neighbor in h3.k_ring(cell, 1):
            if neighbor not in cells:
                boundary_cells.add(neighbor)

    # Store in Redis as a set
    pipe = redis.pipeline()
    for cell in cells | boundary_cells:
        pipe.sadd(f'geofence:cells:{cell}', geofence_id)
        pipe.expire(f'geofence:cells:{cell}', 86400)  # 1-day TTL
    pipe.sadd(f'geofence:{geofence_id}:cells', *cells)
    pipe.execute()

    # Also update PostgreSQL geofences.h3_cells JSONB for durability
    db.execute("""
        UPDATE geofences SET h3_cells = %s WHERE geofence_id = %s
    """, json.dumps(list(cells | boundary_cells)), geofence_id)
```

```python
# Geofence evaluation per GPS update (hot path)
def evaluate_geofences(driver_id: str, lat: float, lng: float,
                        prev_lat: float, prev_lng: float) -> List[GeofenceEvent]:
    current_cell = h3.geo_to_h3(lat, lng, resolution=9)
    prev_cell = h3.geo_to_h3(prev_lat, prev_lng, resolution=9)

    if current_cell == prev_cell:
        return []   # No cell change → no fence transition possible

    # O(1) lookup: which fences contain current cell?
    current_fences = redis.smembers(f'geofence:cells:{current_cell}')
    prev_fences    = redis.smembers(f'geofence:cells:{prev_cell}')

    entered = current_fences - prev_fences   # fences entered
    exited  = prev_fences - current_fences   # fences exited

    events = []
    for fence_id in entered:
        events.append(GeofenceEvent(
            driver_id=driver_id, geofence_id=fence_id,
            event_type='ENTER', lat=lat, lng=lng
        ))
    for fence_id in exited:
        events.append(GeofenceEvent(
            driver_id=driver_id, geofence_id=fence_id,
            event_type='EXIT', lat=lat, lng=lng
        ))

    # Publish to Kafka geo-events topic
    for event in events:
        kafka.publish('geo-events', key=driver_id, value=serialize(event))

    return events
```

**Why H3 resolution 9**: Each resolution-9 cell is ≈ 0.1 km² (roughly 300 m × 300 m). An airport geofence of 2 km² contains ~20 cells — the inverted index is compact. The approximation error (a driver 150 m inside the boundary may be in a boundary cell not fully contained) is acceptable for geofencing use cases; we accept early ENTER events within ~150 m of the actual boundary.

**Interviewer Q&A**:

Q1: The H3 approach fires events when a driver crosses a cell boundary, not the actual geofence polygon boundary. Is this acceptable?
A: For most use cases (airport zone notifications, city boundary transitions), ±150 m accuracy is acceptable. For cases requiring exact boundary detection (legal billing zones, regulated restricted areas), we use a two-pass approach: H3 gives a fast candidate filter, then we run an exact `ST_Contains` check via PostGIS only for the subset of updates that cross an H3 boundary cell (the "boundary_cells" set computed during precomputation). This reduces the PostGIS load by ~99% — only boundary crossings (rare) need exact check.

Q2: A new geofence is added while 1.5 M drivers are online. How do you update the fence without downtime?
A: Geofence precomputation writes to Redis with a 24-hour TTL. Adding a new fence: (1) Insert into PostgreSQL. (2) Publish a `geofence_updated` event to Kafka. (3) Geofence Evaluation Service consumers read the event and call `precompute_fence_cells()`. This takes ~200ms for a complex polygon at resolution 9. (4) New evaluations immediately use the new Redis data. No restart required. Existing in-flight GPS evaluations that miss the new fence for up to 200ms are acceptable (the next GPS update will catch the entry).

Q3: How do you handle the case where a driver is already inside a geofence when it's created?
A: On geofence creation and on driver status change to AVAILABLE, the Geofence Evaluation Service runs a "snapshot check": look up the driver's current Redis Geo position, compute their H3 cell, and check the geofence index. If they're inside a newly created fence, emit a synthetic ENTER event. This is important for operational fences (e.g., "how many drivers are currently at SFO?").

Q4: How many geofences can the system support before H3 precomputation becomes expensive?
A: At resolution 9, a city has ~100,000 cells covering a typical metropolitan area (1,000 km²). If each of 500 geofences covers 2 km², we store 500 × 20 cells = 10,000 Redis keys for geofence membership. Each key lookup is O(1). The memory footprint is 10,000 sets × avg 3 geofence IDs per cell × 16 bytes per ID = ~500 KB — trivial. We can support 10,000+ geofences per city before the precomputation time (dominated by the PostgreSQL polygon-fill query) becomes a concern.

Q5: Describe how driver-centric geofencing (e.g., "alert this specific driver when they enter Zone A") differs from fleet-level geofencing.
A: Driver-centric fences are stored as driver-specific subscriptions. We maintain a Redis set `driver:{id}:subscribed_fences` alongside the global fence index. During evaluation, after the global H3 lookup, we also check the driver's personal subscription list. This is an additional O(1) set intersection per GPS update. Driver-specific fences have a much shorter precomputed cell set (just their notification zones) and are garbage-collected when the driver goes offline.

---

### 6.3 Battery Optimization — Adaptive Sampling Rate

**Problem It Solves**: Updating GPS every 4 seconds drains the driver's phone battery. A stationary driver waiting for a request, or a driver on a straight highway at constant speed, gains nothing from 4-second updates — position is highly predictable. But in a dense urban area executing a pickup, 4-second updates are necessary for rider tracking accuracy and ETA recalculation. The system must adapt sampling rate to context without degrading matching or tracking quality.

**Approaches Comparison**:

| Strategy | Logic | Battery Saving | Accuracy Impact | Complexity |
|---|---|---|---|---|
| Fixed 4s always | No adaptation | None | Baseline | None |
| **Speed-based adaptive** | Slow speed → longer interval | Medium (30–40%) | Low | Low |
| Dead-reckoning client-side | App predicts next position; only sends if error > threshold | High (50–60%) | Very low | Medium |
| Significant motion only | Only update on motion API event | High (varies) | High (misses constant-speed movement) | Low (OS API) |
| ML-predicted suppression | Model predicts when update adds info | Highest (60–70%) | Lowest | High |
| **Hybrid: speed + motion + trip state** | Combines signals | High (40–50%) | Low | Medium |

**Selected: Hybrid Adaptive Sampling (Speed + Trip State + Motion)**

```python
# Battery Optimizer Service — decision function
def compute_adaptive_sampling(update: GPSUpdate, driver_state: str) -> SamplingConfig:
    speed_kmh = update.speed_kmh
    trip_state = driver_state  # AVAILABLE | ON_TRIP | DRIVER_EN_ROUTE

    if trip_state == 'ON_TRIP':
        # Active ride: high frequency needed for rider tracking
        if speed_kmh < 5:   # Stopped at traffic light
            return SamplingConfig(interval_ms=4000,  min_distance_m=5)
        elif speed_kmh < 30:  # Urban driving
            return SamplingConfig(interval_ms=4000,  min_distance_m=10)
        elif speed_kmh < 80:  # Suburban driving
            return SamplingConfig(interval_ms=5000,  min_distance_m=20)
        else:                  # Highway
            return SamplingConfig(interval_ms=8000,  min_distance_m=50)

    elif trip_state == 'DRIVER_EN_ROUTE':
        # Driving to pickup: rider tracking + ETA need frequent updates
        return SamplingConfig(interval_ms=4000, min_distance_m=10)

    elif trip_state == 'AVAILABLE':
        # Waiting for request: only need for matching freshness
        if speed_kmh < 2:   # Truly stationary
            return SamplingConfig(interval_ms=30000, min_distance_m=50)
        elif speed_kmh < 10: # Very slow (traffic, parking lot)
            return SamplingConfig(interval_ms=10000, min_distance_m=20)
        else:                 # Moving but available
            return SamplingConfig(interval_ms=8000,  min_distance_m=30)
```

**Client-side Dead Reckoning**: At 8-second intervals with dead reckoning, the client app predicts the next position using last known speed and heading. If the predicted position is within `min_distance_m` of the actual GPS position, the update is suppressed entirely. This is implemented using the Haversine formula in the driver app SDK.

**Accuracy Analysis**:
- Highway at 100 km/h with 8-second interval: position moves 222 m between updates. For a rider already in the car, 222 m granularity is acceptable (rider is not watching turn-by-turn).
- Urban at 30 km/h with 4-second interval: 33 m movement. Sufficient for ETA recalculation (± 6 seconds accuracy) and rider map display.

**Interviewer Q&A**:

Q1: What happens to matching quality if an available driver goes to a 30-second update interval?
A: The matching service's `GEORADIUS` query reads from Redis Geo, which was last updated up to 30 seconds ago. A driver could have moved up to ~250 m in that time (at walking speed) or 400 m (at 50 km/h). The matching algorithm adds a 500 m buffer to the estimated distance when driver telemetry is stale (detected by `updated_at > 15 s ago`). If the best match is within a threshold distance and telemetry is stale, we do a fresh position request before finalizing assignment. This adds ~100ms to the match time in rare cases.

Q2: How do you ensure the driver app correctly implements the server-issued sampling rate instruction?
A: The driver SDK is Uber/Lyft-controlled (not a third-party app). The sampling config is cryptographically signed with a server HMAC key. The app's location manager reads `next_update_interval_ms` from the Location Update response and adjusts its `CLLocationManager.distanceFilter` (iOS) or `LocationRequest.setInterval()` (Android) accordingly. Config is verified for sanity bounds (min 2 s, max 60 s) before application. Out-of-bounds values from a compromised response are ignored.

Q3: A driver disputes that they completed a long highway trip at 120 km/h but GPS updates were every 8 seconds. How accurate is the reconstructed distance for billing?
A: At 120 km/h with 8-second intervals, consecutive GPS points are ~267 m apart. The reconstructed distance using point-to-point Haversine sums will be slightly less than the actual odometer distance (straight-line between points underestimates curves). Analysis on production data shows < 2% underestimation on highways. Our billing engine applies a road-snapping step: each GPS pair is matched to the road graph and the road distance is used (not straight-line). This recovers the curve correction and yields < 0.5% billing error.

Q4: What's the impact of a GPS outage (driver enters a tunnel) on rider tracking and driver billing?
A: The driver app transitions to network/cell tower location (accuracy_m increases to 50–200 m) and continues sending updates. If all location sources fail: (1) The rider app shows "Location temporarily unavailable" without freezing. (2) The ETA Service uses the last known position + estimated speed from road graph for up to 60 seconds. (3) For billing, the system reconstructs the tunnel segment using the road graph: entry point + exit point → shortest path through known tunnel road geometry. Cassandra records GPS gaps with `provider=DEAD_RECKONING` flag so dispute resolution can identify them.

Q5: How does the battery optimizer interact with iOS background location restrictions (significant location change mode)?
A: iOS's significant location change API fires only on cell tower change (~500 m precision, varies). During active trips, we use CLLocationManager in the full-accuracy mode with a background task. For AVAILABLE drivers, we use a hybrid: significant location change API for coarse positioning (free, no battery cost), and when a ride request is imminent (Matching Service pre-selects the driver as a candidate), the server sends a silent push notification that wakes the app to switch to full-accuracy GPS for 30 seconds. This "GPS on demand" model reduces idle driver battery consumption by ~60% compared to continuous full-accuracy tracking.

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling Mechanism | Bottleneck | Solution |
|---|---|---|---|
| Location Ingest Service | K8s HPA on CPU (Go processes, 10–200 pods) | CPU per GPS validation | Stateless, scales linearly |
| Redis Geo Cluster | Redis Cluster with 16 hash slots per shard | Memory per city (96 MB/city) | Shard by city_id hash; add shards on memory pressure |
| Kafka `location-updates` | Add partitions and brokers | Partition throughput (~50K msg/s) | 128 partitions; scale to 256 for 10× |
| Cassandra GPS Writer | Add Cassandra nodes; add consumers | Disk I/O per node | Add nodes horizontally; Cassandra scales linearly |
| WebSocket Dispatcher | Add servers (Node.js/Go); sticky by trip_id | Memory per connection (~50 KB) | 50K connections/server; need 500K/50K = 10 servers for 500K trips |
| Geofence Evaluation | Stateless pods; partition by city_id | In-memory fence cell map | Each pod owns subset of cities; add pods = add city coverage |

### DB Sharding

**Redis Geo**: Redis Cluster shards the key space by CRC16(key) mod 16384. Keys like `city:{city_id}:drivers:available` are deliberately city-namespaced so that all driver positions for one city land on the same shard — enabling `GEORADIUS` on a single shard without cross-shard scatter. City IDs are distributed across shards manually to balance memory.

**Cassandra**: Partition key `(driver_id, trip_id)` distributes data across the ring via Murmur3 hash. With UUIDs as driver_id, distribution is naturally uniform. Token-aware routing in the Go Cassandra driver ensures the coordinator node handles the write directly on the responsible vnodes. Virtual nodes (vnodes=256) ensure balanced distribution even with heterogeneous hardware.

### Replication

| Store | Replication | Cross-Region |
|---|---|---|
| Redis Geo | Redis Cluster: 1 master + 1 replica per shard; Sentinel-based auto-failover | Async replica in DR region (position data, not critical to be exact) |
| Cassandra GPS | RF=3, NetworkTopologyStrategy: 2 replicas in us-east-1, 1 in eu-west-1 | Multi-region via NTS; LOCAL_QUORUM writes maintain low latency |
| PostgreSQL (geofences) | Synchronous primary → replica; async DR replica | RDS read replica in same region; DR in separate region |

### Caching Strategy

| Layer | Data | Cache Store | TTL | Invalidation |
|---|---|---|---|---|
| In-process (Go heap) | Active geofence definitions | Sync.Map per service | 5 min | On `geofence_updated` Kafka event |
| Redis | H3 cell → fence membership | Redis Set | 24 hrs | Precomputation job on fence change |
| Redis | Driver telemetry (speed, heading) | Hash | 30 s | Auto-expiry |
| Redis | Trip metadata (rider_id, ws_server) | Hash | 4 hrs | On trip completion |

### Interviewer Q&A

Q1: How do you scale the WebSocket layer when 500,000 simultaneous active trips each require real-time location pushes?
A: Each WebSocket server handles ~50,000 concurrent connections (Node.js with uWebSockets.js or Go with gorilla/websocket). 500,000 connections needs 10 servers. Rider connections are routed by a consistent hash on trip_id at the load balancer, ensuring the same WebSocket server always handles a given trip. The Kafka consumer in the WebSocket dispatcher subscribes to all 128 partitions but only forwards messages for connections it owns. Redis Pub/Sub is an alternative fan-out mechanism but introduces a hop; the consistent-hash approach avoids the extra Redis round-trip.

Q2: If Kafka consumer lag on `location-updates` grows (downstream Cassandra is slow), what is the impact?
A: Kafka consumer lag on the Cassandra writer consumer has zero impact on the critical path (Redis writes are synchronous, WebSocket pushes come from a separate consumer group). Lag means GPS history is delayed in Cassandra, but the current position in Redis is always up-to-date. We alert on lag > 100K messages (representing ~3 minutes of unprocessed data) and scale the Cassandra consumer pool. If Cassandra is truly overloaded, we can park messages in Kafka for up to 24 hours (retention period) and replay once the backpressure resolves.

Q3: How do you handle 10× traffic spikes without pre-warming?
A: K8s Cluster Autoscaler adds nodes within 3–5 minutes. For the ingest path: Kafka buffers messages during scale-out. Redis Cluster is pre-provisioned at 3× normal capacity (memory headroom). Cassandra benefits from buffering: the Kafka consumer batches writes, so burst writes are absorbed by Kafka and drained steadily. The most important pre-warming: Redis Geo nodes must be running before the spike (memory-based, no cold start issue). We maintain a minimum of 3 Redis master nodes per city region regardless of traffic.

Q4: What are the memory requirements for Redis Geo at peak, and when would you hit Redis memory limits?
A: Redis Geo uses a sorted set where each member is a 64-bit integer (Geohash) mapped to the member name (driver UUID, 36 bytes). At 1.5 M drivers × (36 B UUID + 8 B geohash + 24 B overhead) ≈ 102 MB total across all cities. A single Redis Cluster with 3 masters and 32 GB RAM each handles this trivially. The telemetry hashes add ~1.5 M × 200 B ≈ 300 MB. Total Redis memory for location data: ~400 MB — far below typical Redis node capacity of 32–256 GB. Memory limits are not a concern until driver count reaches 100 M+.

Q5: Describe your disaster recovery plan for the GPS history in Cassandra.
A: Cassandra's RF=3 across 2 AZs provides automatic recovery from single-node and single-AZ failures. For full-region DR: Kafka retains all GPS messages for 24 hours (`retention.ms = 86400000`). In a region loss event: (1) Spin up new Cassandra cluster in DR region. (2) Replay Kafka topic from 24 hours ago. (3) Missing data beyond 24 hours is irrecoverable — acceptable per our SLA (we lose at most 24 hours of GPS history in a full-region catastrophic failure; we do not lose current positions which are already in the DR Redis replica). Long-term: daily Cassandra snapshots to S3 (using `nodetool snapshot`) enable full restore with 24-hour + replay-time recovery.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component Failed | Impact | Detection | Mitigation |
|---|---|---|---|
| Location Ingest pod crash | GPS updates fail; drivers see HTTP errors | K8s liveness probe; auto-restart in < 10 s | HPA maintains 5+ pods; drivers retry on next 4-second interval |
| Redis Geo primary shard failure | Matching cannot query nearby drivers; new GPS writes fail | Redis Sentinel detects in < 10 s; auto-promotes replica | Failover < 15 s; 2–3 missed GPS update cycles; drivers re-appear on next update |
| Cassandra node failure (1 of 6) | Write quorum still achieved (LOCAL_QUORUM = 2 of 3 per DC) | Cassandra Gossip protocol; hint-based hints | RF=3 means 1 node loss is transparent; hinted handoff stores missed writes for replay on recovery |
| Kafka broker failure | Location-update messages routed to remaining brokers | Kafka controller detects in < 30 s | RF=3 partitions — surviving brokers re-elect leader; producers transparently retry |
| WebSocket server crash | Active trip riders lose location tracking | ALB health check fails; sessions lost | Riders reconnect within 5 s (client auto-reconnect); new session established on a different server; rider may miss 1–2 location updates |
| Network partition between AZs | Split-brain in Redis Cluster | Redis Cluster bus | PRIMARY_PREFERS mode: writes go to AZ with quorum (majority of masters) |
| GPS spoofing / anomalous update | Fraudulent location data stored | Speed validation > 300 km/h; off-road detection | Discard anomalous point; log for fraud ML pipeline; do not update Redis |
| Cassandra full disk | Writes start failing | Disk usage alert at 75% | Auto-scale storage (AWS EBS gp3 resize); add node with token range transfer |
| Geofence service OOM | Geofence events stop | K8s memory limit eviction; pod restart | OOM-protected by heap limit; fence cell map is rebuilt from Redis on restart in < 5 s |

### Retries & Idempotency

- **GPS writes to Redis**: If Redis write fails (circuit breaker open), we skip and continue. The next GPS update (4 seconds later) will self-heal the position. Location data is idempotent by nature — writing the same position twice is harmless.
- **Kafka publish**: librdkafka retries indefinitely with exponential backoff until `message.timeout.ms`. GPS points accumulate in the producer queue (in-memory). We accept potential loss of up to 5 minutes of GPS history (not current position) in a prolonged Kafka outage.
- **Cassandra writes**: Cassandra driver retries on `NoNodeAvailableException` or `WriteTimeoutException` with 3 retries. Batch inserts are idempotent (INSERT … VALUES is upsert-by-timestamp in Cassandra's conflict resolution).
- **WebSocket pushes**: If a push fails (broken connection), the WebSocket server marks the connection dead and the rider app reconnects. On reconnect, the server sends the latest driver position from Redis immediately (not the dropped messages). Rider sees a brief frozen map, then catches up.

### Circuit Breakers

```
Location Ingest → Redis:
  failure_threshold: 100 errors in 1 s
  open_duration: 5 s
  fallback: skip Redis write; still publish to Kafka (Kafka consumer will backfill position eventually)
  half-open: 1 probe per second

Location Ingest → Kafka:
  failure_threshold: 10 errors in 1 s
  open_duration: 10 s
  fallback: write to in-process ring buffer (500 MB, ~10 M GPS points)
  half-open: probe every 5 s

WebSocket Dispatcher → Redis Pub/Sub:
  failure_threshold: 50 errors in 1 s
  fallback: direct Kafka consumer with in-process connection registry
```

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Description |
|---|---|---|---|
| `location.ingest.requests_per_second` | Counter | < 80% of baseline for city | GPS ingestion rate drop |
| `location.ingest.latency_p99` | Histogram | > 200 ms | Ingest acknowledgment slowdown |
| `location.redis.geoadd_latency_p99` | Histogram | > 10 ms | Redis write slowdown |
| `location.driver.staleness_p95` | Gauge | > 15 s | Driver position freshness |
| `location.driver.offline_unexpected_count` | Counter | > 1% of online drivers/min | Unexpected driver drops |
| `location.kafka.consumer_lag` (GPS writer) | Gauge | > 500K messages | Cassandra write backlog |
| `location.websocket.active_connections` | Gauge | < 90% of expected | WS server capacity issue |
| `location.websocket.push_latency_p99` | Histogram | > 2 s | Rider tracking delay |
| `location.geofence.evaluation_latency_p99` | Histogram | > 100 ms | Geofence evaluation bottleneck |
| `location.geofence.events_per_minute` | Counter | Sudden 10× spike (config bug) | Geofence misconfiguration |
| `location.cassandra.write_errors` | Counter | > 0.1% of writes | Cassandra cluster health |
| `location.battery_optimizer.interval_avg` | Gauge | < 4 s (no optimization happening) | Battery optimizer malfunction |

### Distributed Tracing

Each GPS batch request carries a W3C TraceContext header. Critical path trace:

```
[Driver App] POST /v1/driver/location
  [API Gateway: JWT validation] 2ms
  [Location Ingest Service]     5ms total
    [GPS validation]            0.1ms
    [Redis GEOADD]              0.8ms
    [Kafka produce async]       2ms (non-blocking)
  Response to driver            ~8ms
  
  [Kafka Consumer: Cassandra Writer]  95ms (async, after response)
    [Batch accumulation]              90ms
    [Cassandra batch INSERT]          5ms
  
  [Kafka Consumer: WebSocket Dispatcher]  12ms (async)
    [Redis trip lookup]                   1ms
    [WS server registry lookup]           1ms
    [Internal push to WS server]          8ms
    [WS server → Rider app push]          2ms
  
  Total end-to-end (GPS → Rider map):  ~120ms p50, ~500ms p99
```

Sampling: 5% in production; 100% for errors and p99 outliers (tail-based sampling via Jaeger).

### Logging

- **Ingest Service**: Structured JSON with fields: `driver_id`, `city_id`, `lat`, `lng`, `speed_kmh`, `redis_latency_ms`, `kafka_queued`, `trace_id`
- **Geofence Service**: Log fence ENTER/EXIT events with driver_id, geofence_id, transition time
- **WebSocket Dispatcher**: Log connection open/close with rider_id, trip_id, ws_server_id; log push failures
- **Cassandra Writer**: Log batch sizes, write latencies, retry counts
- PII masking: lat/lng in logs are rounded to 3 decimal places (±56 m precision) for non-active-trip log levels. Full precision retained at DEBUG level, masked in production by default.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Selected | Alternative | Reason |
|---|---|---|---|
| Current position store | Redis Geo | PostGIS in PostgreSQL | Redis GEORADIUS < 1 ms; PostGIS 5–20 ms; requirement is < 10 ms p99 |
| GPS history store | Cassandra | TimescaleDB | Cassandra handles 500K+ writes/s natively with LSM; TimescaleDB write rate ~100K/s per node |
| Ingest protocol | HTTPS/2 + protobuf | UDP + custom binary | UDP gives no delivery guarantees and no backpressure; HTTPS gives acks and retries |
| Fan-out mechanism | Kafka (persistent) | Redis Pub/Sub (ephemeral) | Kafka provides durability (24-hr retention) and replay for consumers that fall behind |
| Geofencing algorithm | H3 cell precomputation | Real-time PostGIS per update | 562K/s PostGIS queries impossible; H3 reduces to O(1) hash lookup per cell change |
| Sampling rate | Adaptive (speed+state) | Fixed 4s | 40–50% battery saving with < 2% distance billing error; major driver retention improvement |
| WebSocket routing | Consistent hash on trip_id | Redis Pub/Sub fan-out | Hash avoids Pub/Sub overhead for 500K channels; each WS server handles only its trips |
| GPS validation | Server-side + client-side | Server-side only | Client-side validation (speed filter) prevents battery-wasting retries for known-bad points |
| Cassandra write batching | 500-row unlogged batch | 1 row per insert | 500× reduction in Cassandra coordinator overhead; unlogged batch is safe per-partition |
| Clock authority | Client timestamp (recorded_at) | Server receipt timestamp | Client timestamp accurately records when the driver was at the location; server time distorted by network delay |

---

## 11. Follow-up Interview Questions

Q1: How would you design a "live fleet dashboard" showing density heatmaps for all drivers in a city in real time?
A: The dashboard queries Redis GEORADIUS for full city coverage (large radius), but a full scan for 100K+ drivers is expensive. Better: maintain a separate H3 resolution-6 (city-district level) counter in Redis — a Hash where `field = h3_cell` and `value = count of available drivers in that cell`. The Location Ingest Service increments/decrements this counter atomically on every status change. The dashboard reads the full hash (`HGETALL`) at most every 5 seconds, getting an instant district-level density map in < 1 ms.

Q2: A driver claims their GPS trace shows they took a longer route but Uber billed based on a shorter route. How do you resolve this dispute?
A: Run the Trip Replay Service on the `driver_gps_history` for the trip. Compare: (1) Billed distance (road-snapped GPS trace from Cassandra). (2) Driver-claimed distance. If the GPS trace shows a genuine detour (points clearly off the shortest-path road), re-bill based on actual GPS distance. If GPS points are sparse (battery-saving mode), we use road-graph distance between GPS anchors. We require GPS recording for billing purposes to be at least 1 point per 60 seconds; trips with excessive GPS gaps are flagged for manual review.

Q3: How does the system behave when a driver's phone has no internet connectivity for 2 minutes (underground parking)?
A: The driver app queues GPS updates in local storage (SQLite ring buffer, 100 entries max = 6.7 minutes at 4s). When connectivity is restored, it replays the queue to the server as a batch of up to 3 points per request (multiple sequential requests). The Location Ingest Service processes them in received order; timestamps are client-side so Cassandra stores them in correct chronological order. Redis Geo receives the most recent point from the reconnected batch, restoring matching availability. The driver was effectively invisible to matching for 2 minutes — the matching service's "stale driver" logic excluded them during that window.

Q4: How would you implement a "driver heading filter" for matching — preferring drivers moving toward the pickup location?
A: In the scoring function of the Matching Service: given a candidate driver with heading H (0–360°) at position (dlat, dlng), and pickup at (plat, plng), compute the bearing from the driver to the pickup: `bearing = atan2(sin(Δlng)cos(plat), cos(dlat)sin(plat) - sin(dlat)cos(plat)cos(Δlng))`. If the angular difference between H and bearing is < 45°, the driver is "heading toward pickup" — add a score bonus of 10%. Drivers moving away (angular difference > 135°) get a 15% penalty. Driver heading is stored in the `driver:{id}:telemetry` Redis hash and read during scoring.

Q5: How would you store and serve a "live trip polyline" — the rider's app showing the driver's driven path as a growing line?
A: For active trips, the driven GPS trace is maintained in a Redis List: `RPUSH trip:{id}:trace "{lat},{lng}"`. On each GPS update (via Kafka consumer), append the point. The rider app receives each new GPS point via WebSocket and appends it to the local map polyline. Batch fetch on reconnect: `LRANGE trip:{id}:trace 0 -1` returns the full driven path. TTL on the list = 4 hours. After trip completion, the final polyline is encoded and written to `trips.polyline` in PostgreSQL; the Redis list is deleted.

Q6: What are the privacy implications of storing high-resolution GPS history, and how do you handle data requests?
A: GPS history at 4-second intervals is extremely sensitive PII. Storage protection: (1) Cassandra data encrypted at rest (AES-256 via AWS KMS). (2) Access requires service-account JWT with explicit `location:history:read` scope. (3) Logs do not contain raw GPS at INFO level. For user data requests (GDPR Article 15): an async job queries all Cassandra partitions for `driver_id = ?` and assembles a JSON archive of GPS traces, returned as a secure download link. For right to erasure (Article 17): TTL-based deletion handles the 90-day window automatically; earlier deletion uses `DELETE FROM driver_gps_history WHERE driver_id = ? AND trip_id = ?` per trip.

Q7: How would you architect a "nearby event detection" feature — alerting all drivers within 500m of a venue when a concert ends?
A: The event scheduler (city ops creates a "venue exit event" at event end time) publishes a `venue_event` message to Kafka at T=0. The Geofence Evaluation Service processes this as a one-time scan: `GEORADIUS city:{id}:drivers:available {venue_lng} {venue_lat} 0.5 km COUNT 1000`. The returned driver IDs are sent to the Notification Service for a push notification. The scan is O(k + log M) where k is nearby drivers — at most a few hundred. Total processing time: ~5 ms. The event geofence is created 30 minutes before the concert ends and deleted 30 minutes after.

Q8: Describe the performance impact of road-snapping on the ingestion pipeline.
A: Road-snapping (mapping a raw GPS lat/lng to the nearest road segment using a map-matching algorithm like Viterbi HMM on the road graph) is a compute-heavy operation — ~5–50 ms per point depending on road density. Doing this synchronously at 562K points/s would require thousands of CPU cores. Our approach: road-snapping is fully async. The Ingest Service writes raw coordinates to Redis and Cassandra immediately. A separate Road-Snap Worker consumes from Kafka and performs map-matching using a local road graph (OSM data) loaded into a PostGIS spatial index in a dedicated DB. Snapped coordinates are written back to Cassandra via a Cassandra UPDATE on the existing row. For billing, we use snapped coordinates fetched at trip-end; for real-time rider display, raw coordinates are good enough (the minor GPS jitter is filtered client-side by the map SDK).

Q9: How does the system handle drivers who cross international borders (e.g., US-Canada border)?
A: Cross-border driving is rare and restricted by regulatory requirements (drivers must be licensed in the jurisdiction). When a GPS point crosses a country boundary geofence (EXIT from US, ENTER Canada), the Geofence service generates a `COUNTRY_EXIT` event. The Driver Service catches this event and transitions the driver to OFFLINE status if they are not authorized in the new country. The driver app displays a "You have left your service area" notification. For authorized cross-border drivers (rare — special fleet categories), their `city_id` is migrated to the appropriate city on the other side.

Q10: How would you build a "driver replay on map" feature for the rider's trip history screen?
A: The Trip Replay Service accepts `GET /v1/trips/{trip_id}/gps-replay?playback_speed=5`. It queries Cassandra: `SELECT * FROM driver_gps_history WHERE driver_id = ? AND trip_id = ?`. Cassandra returns points in recorded_at order (clustering key). The service reads the time span (T_end - T_start), divides by playback_speed, and emits each point as an SSE event timed to (point_time - T_start) / playback_speed. The rider's app animates a car icon along the route. Edge case: points stored at 4-second intervals are interpolated to 1-second resolution for smooth animation using linear interpolation between consecutive GPS points.

Q11: What is the impact of GPS coordinate precision on storage and accuracy?
A: A `DOUBLE PRECISION` float in Cassandra/PostgreSQL is 8 bytes, giving ~15 significant digits. At Earth's surface, 15 digits of latitude corresponds to sub-nanometer precision — massively over-precise. Actual consumer GPS accuracy is ±4–10 meters. We could use `FLOAT` (4 bytes, ~7 significant digits → ±1 cm precision at GPS scales) and halve storage. Calculation: 32.4 B GPS points/day × (8B - 4B lat + 8B - 4B lng) = 32.4 B × 8 B saved = 259 GB/day storage reduction. We retain DOUBLE for lat/lng because (1) road snapping algorithms benefit from higher precision and (2) storage is not the binding constraint (throughput is).

Q12: How do you handle GPS drift — a stationary driver's GPS coordinates jumping 10–20 meters randomly?
A: A Kalman filter or exponential moving average on the driver app side smooths positional noise before sending. The SDK applies a `min_distance_m` threshold (default 5 m) — if the new GPS point is within 5 m of the last sent point, the update is suppressed. Server-side, the road-snapping algorithm's Viterbi HMM naturally smooths GPS noise by preferring continuous road paths over random jumps. For the matching service's GEORADIUS query, 20 m of position drift at scale (5 km search radius) has zero practical impact on driver assignment quality.

Q13: Describe how you would implement a "SafetyKit" feature where the rider can share their live location with a trusted contact.
A: When the rider activates SafetyKit, the Rider Service creates a temporary share token (UUID, 4-hour TTL stored in Redis). The trusted contact opens a share URL `/track/{token}`. The SafetyKit Share Service resolves the token to the active `trip_id`, subscribes to the `trip:{trip_id}:location` Redis Pub/Sub channel (or Kafka), and streams location updates to the trusted contact's browser via WebSocket. The contact sees the driver's position (same feed as the rider). The share URL expires when the trip ends. No separate location storage needed — the existing trip location pipeline serves both the rider and the shared contact.

Q14: How do you measure location accuracy quality at scale?
A: We run a "ground truth" test program in each city: a QA vehicle drives a known route with a high-precision GNSS device (< 0.5 m accuracy). We compare the QA device's trace against the driver app's GPS trace for the same trip. Metrics: mean absolute error in position (meters), GPS gap rate (% of 4-s windows with no update), road-snap accuracy (% of points snapped to correct road segment). These metrics are published to a Grafana dashboard per city, per driver app version, per OS version. If a new Android GPS API change degrades accuracy, it shows up in city-level accuracy metrics within 24 hours.

Q15: What changes to this architecture would be needed to support pedestrian couriers (food delivery) instead of vehicles?
A: Key differences: (1) Pedestrians move at 5–15 km/h → lower sampling rate acceptable (8-second intervals, 5 m dead-zone). (2) Indoor positioning — GPS is unreliable; need WiFi fingerprinting or Bluetooth beacon integration for restaurant/building locations. (3) Geofencing becomes more important — entering/leaving a restaurant or an apartment building entrance needs sub-10 m accuracy. This requires higher H3 resolution (resolution 11, ≈30 m² cells). (4) Battery is even more critical — a delivery driver uses their phone all shift; the adaptive sampling must be even more aggressive. (5) The road graph for pedestrian routing includes footpaths, elevators, and building entrances — the road-snap service needs OpenStreetMap pedestrian data, not just driving roads.

---

## 12. References & Further Reading

1. **Uber H3 Library (Open Source)** — https://github.com/uber/h3 — Hexagonal hierarchical spatial indexing system
2. **Uber Engineering: Handling Millions of Location Updates** — https://eng.uber.com/real-time-location/
3. **Redis Geo Documentation (GEOADD, GEOSEARCH)** — https://redis.io/docs/data-types/geo/
4. **Apache Cassandra Architecture Guide** — https://cassandra.apache.org/doc/latest/cassandra/architecture/
5. **Cassandra TimeWindowCompactionStrategy** — https://cassandra.apache.org/doc/latest/cassandra/operating/compaction/twcs.html
6. **Lyft Engineering Blog: Real-Time Location Tracking** — https://eng.lyft.com/location-tracking-e18f8b53de2c
7. **Google Protocol Buffers (protobuf)** — https://protobuf.dev/ — Used for compact GPS payload encoding
8. **W3C TraceContext Specification** — https://www.w3.org/TR/trace-context/ — Distributed tracing propagation standard
9. **Kalman Filter Tutorial for GPS** — https://www.bzarg.com/p/how-a-kalman-filter-works-in-pictures/
10. **OpenStreetMap for Road Graphs** — https://wiki.openstreetmap.org/wiki/Overpass_API — Source for road snapping graph data
11. **iOS Core Location Background Modes** — https://developer.apple.com/documentation/corelocation/getting_the_current_location_of_a_device
12. **Android FusedLocationProvider** — https://developers.google.com/location-context/fused-location-provider
13. **GDPR Article 5: Principles of Processing Personal Data** — https://gdpr-info.eu/art-5-gdpr/
14. **Viterbi Algorithm for Map Matching** — Newson & Krumm, "Hidden Markov Map Matching", ACM SIGSPATIAL 2009
15. **Martin Kleppmann — Designing Data-Intensive Applications** (O'Reilly, 2017) — Chapter 3 (Storage and Retrieval), Chapter 11 (Stream Processing)
