# System Design: Proximity Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Register Entity**: Allow businesses, service providers, or users to register themselves with a latitude/longitude location and a category (restaurant, plumber, driver, etc.).
2. **Find Nearby**: Given a query location and a radius, return a ranked list of entities within that radius and optional category filter.
3. **Update Location**: Allow entities whose position changes (drivers, delivery couriers) to update their current coordinates in real time.
4. **Delete / Deactivate Entity**: Remove an entity from the proximity index when they go offline or close permanently.
5. **Configurable Radius**: Caller specifies search radius (50 m to 50 km); system returns results within that exact bound.
6. **Category Filtering**: Filter results by one or more entity categories.
7. **Pagination**: Support cursor-based pagination for large result sets.
8. **Distance Calculation**: Return the precise distance in meters from the query point to each result.

### Non-Functional Requirements

- **Read Latency**: p99 ≤ 100 ms for radius searches with radius ≤ 10 km; p99 ≤ 200 ms for radius ≤ 50 km.
- **Write Latency**: Location update acknowledged within 50 ms p99.
- **Availability**: 99.99% uptime (core search path). Location update ingestion: 99.9%.
- **Scalability**: Support 500 M registered entities; 100 K search QPS at peak; 1 M location update events/s for real-time movers.
- **Consistency**: Stale reads acceptable up to 5 s for real-time movers (e.g., ride-share drivers). Business locations: eventual consistency within 60 s.
- **Accuracy**: Return all entities within the requested radius (no false negatives for correct query). False positives (entities slightly outside radius due to geohash cell boundaries) acceptable; filtered in post-processing.
- **Security**: Location data accessible only to authorized callers; user location never exposed to arbitrary third parties.

### Out of Scope

- Routing or turn-by-turn directions between query point and results.
- Recommendation scoring (beyond distance ranking).
- Real-time social broadcasting ("see your friends on a map").
- Payment or booking within the proximity service itself.
- Indoor positioning or sub-meter accuracy.

---

## 2. Users & Scale

### User Types

| Type | Description | Primary Operation |
|---|---|---|
| End User (Consumer) | Mobile/web app user searching for nearby services | Read: radius search |
| Driver / Courier | Real-time mover updating location every 4–5 s | Write: location update (high frequency) |
| Business Owner | Registers/updates a fixed location | Write: entity registration, occasional update |
| Platform Service | Backend microservice calling Proximity API | Read: programmatic radius queries |
| Admin | Internal operator managing entity data | Admin CRUD |

### Traffic Estimates

**Assumptions**:
- 500 M registered entities (businesses + drivers + couriers + freelancers).
- 100 M active consumers/day performing proximity searches.
- Average consumer performs 5 searches/day → 500 M searches/day.
- 5 M real-time movers (drivers/couriers) active concurrently at peak, each sending a location update every 5 s.
- Peak is 3× average.

| Metric | Calculation | Result |
|---|---|---|
| Search RPS (average) | 500 M / 86 400 s | ~5 800 RPS |
| Search RPS (peak) | 5 800 × 3 | ~17 400 RPS |
| Location update RPS (avg) | 5 M movers × (1/5 s) | 1 000 000 RPS |
| Location update RPS (peak) | 1 M × 3 | 3 000 000 RPS |
| Entity registration writes | ~1 M/day → 12 RPS | ~12 RPS (negligible) |
| Concurrent search connections | Assume 10 M users active simultaneously, each query <100 ms | 10 M × (0.1 s / 1 s) = 1 M concurrent inflight searches |

**Note on update RPS**: 1 M–3 M RPS for location updates is the dominant write challenge. This requires a dedicated write path separate from the search path.

### Latency Requirements

| Operation | p50 Target | p99 Target | Notes |
|---|---|---|---|
| Radius search (≤10 km) | 20 ms | 100 ms | Core latency SLA |
| Radius search (≤50 km) | 60 ms | 200 ms | Larger area = more index cells |
| Location update ack | 10 ms | 50 ms | Must be fast to not stall driver apps |
| Entity registration | 100 ms | 500 ms | Rare; latency tolerance higher |
| Delete/deactivate | 100 ms | 500 ms | Rare |

### Storage Estimates

**Assumptions**:
- Entity record: entity_id (8 B) + name (64 B) + lat (8 B) + lon (8 B) + category (4 B) + geohash12 (12 B) + metadata (100 B avg) ≈ 200 bytes.
- 500 M entities × 200 B = 100 GB entity data.
- Geospatial index: geohash → list of entity_ids. At geohash precision 6 (1.2 km × 0.6 km cells), ~3.6 M cells on Earth. Each cell has an average of 500 M / 3.6 M ≈ 140 entities. Entry: 8 B per entity_id × 140 × 3.6 M cells = ~4 GB index.
- Location update log (real-time movers): retain 1 hour of history for audit/replay. 1 M updates/s × 3600 s × 40 bytes/update = ~144 GB hot. Compressed ×0.3 = ~43 GB.
- Historical location data (drivers, last 30 days): 5 M movers × 86 400 s / 5 s × 40 B × 30 days = ~1 PB. Typically archived to cold storage.

| Data Type | Size | Notes |
|---|---|---|
| Entity records | 100 GB (× 3 replicas = 300 GB) | In distributed KV or relational DB |
| Geospatial index (geohash) | 4 GB (× 3 = 12 GB) | Fits in memory on Redis cluster |
| Live location (real-time movers) | ~500 MB active set (× 3 = 1.5 GB) | One record per mover, in Redis |
| Location event log (1 h hot) | ~43 GB compressed | Kafka topics |
| Historical location (30 d) | ~1 PB | Object storage, cold tier |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Search response egress | 17 400 RPS × 5 KB avg response (20 results × 200 B + overhead) | ~87 MB/s |
| Location update ingress | 1 M RPS × 40 B per probe | ~40 MB/s |
| Index sync (update → index) | 1 M updates/s × 8 B (entity_id write) | ~8 MB/s |

---

## 3. High-Level Architecture

```
                    ┌────────────────────────────────────────────┐
                    │                CLIENTS                      │
                    │  Mobile App  │  Web App  │  Platform Svc   │
                    └──────┬───────┴─────┬─────┴──────┬──────────┘
                           │             │            │
                    ────────▼─────────────▼────────────▼────────
                    │          API Gateway / Load Balancer        │
                    │    (Auth, Rate Limit, TLS Termination)      │
                    ────────────────────┬────────────────────────
                                        │
               ┌────────────────────────┼───────────────────────────────┐
               │                        │                               │
    ┌──────────▼──────────┐  ┌──────────▼────────────┐  ┌──────────────▼───────────┐
    │   Search Service    │  │  Location Update Svc   │  │  Entity Registry Service │
    │  (stateless, reads  │  │  (write-optimized,     │  │  (CRUD for entities;     │
    │   geo index + DB)   │  │   feeds real-time      │  │   writes to entity DB    │
    └──────────┬──────────┘  │   location store)      │  │   and geo index)         │
               │             └──────────┬─────────────┘  └──────────────┬───────────┘
               │                        │                               │
    ┌──────────▼──────────┐  ┌──────────▼─────────────┐  ┌─────────────▼───────────┐
    │   Geo Index Store   │  │   Live Location Store   │  │     Entity DB           │
    │   (Redis Cluster,   │  │   (Redis Cluster,       │  │  (PostgreSQL + PostGIS  │
    │    geohash→entity   │  │    entity_id→lat/lon,   │  │   OR DynamoDB)          │
    │    inverted index)  │  │    updated in-place)    │  └─────────────────────────┘
    └──────────┬──────────┘  └──────────┬─────────────┘
               │                        │
    ┌──────────▼──────────┐  ┌──────────▼─────────────┐
    │   Entity Detail     │  │   Kafka (location-      │
    │   Cache (Redis)     │  │   updates topic)        │
    └─────────────────────┘  └──────────┬─────────────┘
                                         │
                             ┌───────────▼────────────┐
                             │   Geo Index Updater    │
                             │   (Kafka consumer;     │
                             │    moves entity in     │
                             │    geohash index)      │
                             └────────────────────────┘
```

**Component Roles**:

- **API Gateway**: Handles authentication (JWT/API key), rate limiting (per-user and per-endpoint), TLS termination, and request routing to the appropriate microservice.
- **Search Service**: Stateless service that accepts a `(lat, lon, radius, category)` query. Translates the query into geohash cell lookups, fetches entity_ids from the Geo Index Store, hydrates entity details from Entity Detail Cache, computes haversine distances, filters, sorts, and paginates results.
- **Location Update Service**: High-throughput write-optimized service. Receives location updates from movers. Updates Live Location Store (Redis, immediate) and publishes to Kafka for async index maintenance.
- **Entity Registry Service**: Manages entity lifecycle (create, update metadata, delete). Writes authoritative records to Entity DB. On creation/deletion, synchronously updates Geo Index Store.
- **Geo Index Store (Redis Cluster)**: Core geospatial index. Uses geohash-based inverted index: `key = "gh:{geohash6}"`, `value = sorted set of entity_ids (sorted by geohash12 for sub-cell ordering)`. Redis `GEORADIUS` command or custom geohash lookups.
- **Live Location Store (Redis Cluster)**: Stores current lat/lon per real-time mover with TTL (auto-expire if no update for 30 s → mover considered offline). Separate from static geo index to allow high-frequency updates without index rebuild overhead.
- **Entity DB**: Source of truth for entity metadata (name, category, hours, attributes). PostgreSQL with PostGIS for complex geo queries during administrative operations. DynamoDB as an alternative for global scale.
- **Kafka (location-updates topic)**: Decouples the Location Update Service write path from the geo index maintenance. Allows async geo index update with retries.
- **Geo Index Updater (Kafka consumer)**: Consumes location update events. For each event, moves the entity from its old geohash cell to the new geohash cell in the Geo Index Store. Handles cell transitions atomically.

**Primary Use-Case Data Flow (Radius Search)**:

1. Client: `GET /v1/nearby?lat=37.7749&lon=-122.4194&radius_m=1000&category=restaurant`
2. API Gateway: validates JWT, checks rate limit (100 RPS/user), routes to Search Service.
3. Search Service: converts `(lat, lon, radius)` to a set of geohash6 cells covering the search area (see Deep Dive 6.1).
4. For each geohash cell, Search Service runs `SMEMBERS gh:{cell}` on Redis Geo Index Store (pipelined, ≤ 9 cells for typical 1 km search).
5. Search Service deduplicates entity_ids, fetches entity details from Entity Detail Cache (Redis hash).
6. For live movers in results: overrides lat/lon from Live Location Store (to get freshest position).
7. Search Service computes haversine distance for each entity, filters to `distance ≤ radius_m`, filters by category, sorts by distance, paginates.
8. Returns ranked JSON response with distance_m per result.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────
-- Entity Registry (PostgreSQL + PostGIS OR DynamoDB)
-- ─────────────────────────────────────────────

CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TABLE entity (
    entity_id       UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    entity_type     VARCHAR(32)  NOT NULL,   -- "business","driver","courier","person"
    name            VARCHAR(256) NOT NULL,
    category        VARCHAR(64)  NOT NULL,   -- "restaurant","taxi","plumber",...
    sub_category    VARCHAR(64),
    lat             DOUBLE PRECISION NOT NULL,
    lon             DOUBLE PRECISION NOT NULL,
    geog            GEOGRAPHY(POINT, 4326) GENERATED ALWAYS AS
                        (ST_SetSRID(ST_MakePoint(lon, lat), 4326)::geography) STORED,
    geohash6        CHAR(6)      NOT NULL,   -- geohash at precision 6 (~1.2 km)
    geohash12       CHAR(12)     NOT NULL,   -- geohash at precision 12 (~3.7 cm)
    is_realtime     BOOLEAN      NOT NULL DEFAULT FALSE,  -- true for movers
    is_active       BOOLEAN      NOT NULL DEFAULT TRUE,
    metadata_json   JSONB,                  -- arbitrary attributes
    created_at      TIMESTAMP    NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMP    NOT NULL DEFAULT NOW(),

    INDEX idx_geog USING GIST (geog),
    INDEX idx_geohash6 (geohash6),
    INDEX idx_category (category),
    INDEX idx_active_category (is_active, category)
);

-- ─────────────────────────────────────────────
-- Live Location (stored in Redis; schema shown logically)
-- ─────────────────────────────────────────────
-- Redis Hash: HSET "loc:{entity_id}" lat <val> lon <val> speed <val> heading <val> updated_at <ts>
-- Redis TTL: EXPIRE "loc:{entity_id}" 30  -- auto-expire if no update for 30s

-- Redis Geo Index:
-- GEOADD "geo:index:{category}" lon lat entity_id
-- (Redis GEOADD stores internally as a sorted set with a 52-bit geohash score)

-- ─────────────────────────────────────────────
-- Location History (append-only, partitioned by day)
-- ─────────────────────────────────────────────

CREATE TABLE location_event (
    id              BIGSERIAL    NOT NULL,
    entity_id       UUID         NOT NULL,
    lat             DOUBLE PRECISION NOT NULL,
    lon             DOUBLE PRECISION NOT NULL,
    speed_mps       FLOAT,
    heading_deg     SMALLINT,
    accuracy_m      FLOAT,
    recorded_at     TIMESTAMP    NOT NULL,
    PRIMARY KEY (id, recorded_at)
) PARTITION BY RANGE (recorded_at);

-- Monthly partitions for lifecycle management (DROP oldest partition to purge)
CREATE TABLE location_event_2024_04 PARTITION OF location_event
    FOR VALUES FROM ('2024-04-01') TO ('2024-05-01');

-- ─────────────────────────────────────────────
-- Redis Geohash Index (logical representation)
-- ─────────────────────────────────────────────
-- Key pattern: "gh6:{geohash6_cell}"
-- Value: Redis Set of entity_ids
--   SADD "gh6:9q8yy" "uuid-1234"
--   SADD "gh6:9q8yy" "uuid-5678"
--
-- Key pattern: "entity:{entity_id}"
-- Value: Redis Hash with entity fields (name, lat, lon, category, geohash6, ...)
--   HSET "entity:uuid-1234" name "Blue Bottle Coffee" lat 37.7749 lon -122.4194 ...
--
-- Key pattern: "loc:{entity_id}"  (only for is_realtime=true entities)
-- Value: Redis Hash { lat, lon, speed, heading, updated_at }
-- TTL: 30 seconds
```

### Database Choice

| Option | Pros | Cons | Fit |
|---|---|---|---|
| **PostgreSQL + PostGIS** | Mature, rich geo functions (ST_DWithin, ST_Distance), ACID, complex queries | Single-region scalability ceiling, PostGIS queries CPU-intensive at scale | Good for entity metadata; use geo query for admin/analytics, not search hot path |
| **Redis GEORADIUS** | Built-in geospatial commands, sub-millisecond, in-memory | Limited storage (~1 B keys/cluster), no complex filtering, data loss on crash without AOF | Excellent for geo index hot path (search lookups) |
| **DynamoDB** | Serverless, auto-scaling, global tables | No native geo queries; must build geohash-based index manually | Good for entity metadata at global scale (non-geo queries) |
| **Cassandra** | Write-optimized, high availability, time-series support | No native geo indexing; lat/lon columns require manual geohash bucketing | Good for location history (append-only, time-series) |
| **Elasticsearch** | geo_distance queries, full-text search + geo in one | Heavy operationally, slower writes, eventual consistency | Good for complex search (geo + text filter combined) |
| **Google S2 / H3 custom index** | Hierarchical cells, exact coverage, flexible precision | Custom implementation required | Best for complex coverage queries at extreme scale |

**Selected Choices**:
- **Entity metadata**: PostgreSQL + PostGIS for the Entity DB (authoritative store, complex queries). PostGIS `ST_DWithin` used for admin bulk queries only, not the real-time search hot path.
- **Geo index hot path**: Redis Cluster with a custom geohash inverted index (geohash cell → set of entity_ids). Redis pipeline multi-GET handles the 9-cell lookup in a single round trip.
- **Live location (movers)**: Redis with per-entity hash + 30 s TTL. High write throughput (1 M updates/s) distributed across Redis cluster by entity_id key hash.
- **Location history**: Apache Cassandra partitioned by (entity_id, day) — pure append, time-ordered reads by entity, high write throughput, easy TTL via `default_time_to_live`.

---

## 5. API Design

```
Base URL: https://proximity-api.example.com/v1/

All endpoints require Authorization: Bearer <JWT> header.
Rate limits: 100 search QPS per user token; 10 entity registrations/min per token.
Responses are JSON. Errors use RFC 7807 Problem Details.

──────────────────────────────────────────────────────────────────
1. Radius Search
──────────────────────────────────────────────────────────────────

GET /nearby
  Auth:       Required (JWT)
  Params:
    lat           float   required  Query center latitude (-90 to 90)
    lon           float   required  Query center longitude (-180 to 180)
    radius_m      int     required  Search radius in meters (1–50 000)
    category      string  optional  Filter by category (e.g., "restaurant")
    entity_type   string  optional  Filter by type (business|driver|courier)
    limit         int     optional  Max results per page (1–100, default 20)
    cursor        string  optional  Pagination cursor from previous response
    sort          enum    optional  distance|rating (default: distance)
  Response 200:
    {
      "results": [
        {
          "entity_id": "550e8400-e29b-41d4-a716-446655440000",
          "name": "Blue Bottle Coffee",
          "category": "cafe",
          "lat": 37.7752,
          "lon": -122.4183,
          "distance_m": 342,
          "is_realtime": false,
          "metadata": { "rating": 4.5, "price_level": 2 }
        }
      ],
      "total_in_radius": 47,
      "next_cursor": "eyJsYXN0X2Rpc3RhbmNlIjozNDIsImxhc3RfaWQiOiI1NTAifQ==",
      "search_radius_m": 1000,
      "query_latency_ms": 12
    }
  Rate limit: 100 RPS per token
  Pagination: cursor encodes {last_distance_m, last_entity_id}; stable sort by (distance, entity_id)

──────────────────────────────────────────────────────────────────
2. K-Nearest Neighbors Search
──────────────────────────────────────────────────────────────────

GET /nearest
  Auth:       Required
  Params:
    lat       float  required
    lon       float  required
    k         int    required  Number of nearest results (1–100)
    category  string optional
    max_radius_m int optional  Bounds the search (default 50 000 m)
  Response 200: Same structure as /nearby, no next_cursor (k results max)
  Rate limit: 100 RPS per token

──────────────────────────────────────────────────────────────────
3. Entity Registration
──────────────────────────────────────────────────────────────────

POST /entities
  Auth:       Required (service account or business owner JWT)
  Body:
    {
      "name": "Joe's Pizza",
      "category": "restaurant",
      "entity_type": "business",
      "lat": 40.7128,
      "lon": -74.0060,
      "is_realtime": false,
      "metadata": { "hours": "Mon-Sun 11am-10pm", "phone": "+1-212-555-0100" }
    }
  Response 201:
    { "entity_id": "uuid", "geohash6": "dr5ru", "created_at": "2024-04-01T12:00:00Z" }
  Rate limit: 10 req/min per token

──────────────────────────────────────────────────────────────────
4. Location Update (Real-Time Movers)
──────────────────────────────────────────────────────────────────

PUT /entities/{entity_id}/location
  Auth:       Required (entity owner JWT only)
  Body:
    {
      "lat": 37.7760,
      "lon": -122.4171,
      "speed_mps": 8.3,
      "heading_deg": 270,
      "accuracy_m": 5.0,
      "timestamp": 1712000000
    }
  Response 204: No Content
  Rate limit: 1 req/s per entity (enforced per entity_id)
  Idempotency: timestamp field; duplicate timestamp for same entity_id is ignored.

──────────────────────────────────────────────────────────────────
5. Entity Detail
──────────────────────────────────────────────────────────────────

GET /entities/{entity_id}
  Auth:       Required
  Response 200: Full entity object including current location (if realtime).
  Cache-Control: public, max-age=30 (for static entities), no-cache (for realtime)

──────────────────────────────────────────────────────────────────
6. Update Entity Metadata
──────────────────────────────────────────────────────────────────

PATCH /entities/{entity_id}
  Auth:       Required (entity owner or admin)
  Body: JSON Merge Patch (RFC 7396) — only fields to update
  Response 200: Updated entity object

──────────────────────────────────────────────────────────────────
7. Delete Entity
──────────────────────────────────────────────────────────────────

DELETE /entities/{entity_id}
  Auth:       Required (entity owner or admin)
  Response 204: No Content
  Behavior: Marks entity inactive in DB, removes from geo index immediately.

──────────────────────────────────────────────────────────────────
8. Batch Location Update (for fleet management systems)
──────────────────────────────────────────────────────────────────

POST /entities/batch-location
  Auth:       Required (service account)
  Body:
    { "updates": [
        { "entity_id": "uuid1", "lat": ..., "lon": ..., "timestamp": ... },
        { "entity_id": "uuid2", "lat": ..., "lon": ..., "timestamp": ... }
    ] }
  Max batch size: 500
  Response 207 Multi-Status: per-entity success/failure array
  Rate limit: 10 req/s per service account
```

---

## 6. Deep Dive: Core Components

### 6.1 Geospatial Indexing Strategy (Geohash vs. Quadtree vs. S2)

**Problem it solves**: Given 500 M entities distributed across Earth, how do you translate a `(lat, lon, radius_m)` query into a set of discrete index lookups that return all entities within the radius with minimal false negatives, in under 10 ms?

**Approaches Comparison**:

| Approach | Cell Shape | Hierarchical | Neighbor Lookup | Distortion | Best For |
|---|---|---|---|---|---|
| **Geohash** | Rectangular, base-32 encoded | Yes (prefix-based) | 8 adjacent cells (explicit computation) | High at poles, moderate at equator | Simple implementation, Redis-native, string prefix queries |
| **Quadtree** | Rectangular, adaptive (dense areas subdivide) | Yes (tree structure) | Via tree traversal | Moderate | Dynamic density, in-memory tree, non-uniform distributions |
| **S2 Geometry (Google)** | Spherical Hilbert curve cells, square on sphere | Yes (64-level hierarchy) | Via S2 cell union expansion | Very low (designed for Earth) | Accurate coverage, complex shape queries, production geo systems |
| **H3 (Uber)** | Hexagonal | Yes | 6 neighbors (uniform) | Low | Spatial analytics, uniform hexagonal tessellation |
| **PostGIS R-tree** | Bounding rectangle | Yes | Spatial index scan | Depends on data | SQL-based geo queries, not in-memory lookup |
| **Custom kd-tree** | Binary spatial partition | Yes | Nearest-neighbor via tree walk | N/A (exact) | k-NN queries in memory on static datasets |

**Selected Approach**: Geohash-based inverted index stored in Redis, with a configurable precision level (precision 6 for most searches ≤ 5 km, precision 5 for larger radii).

**Detailed Reasoning**:
- Geohash at precision 6 produces ~1.2 km × 0.6 km cells. For a 1 km radius search, you need to check the cell containing the query point and all 8 adjacent cells — 9 cells total. This is a predictable, constant number of Redis lookups regardless of data distribution.
- Geohash strings are prefix-hierarchical: cells at precision 5 are exactly covered by cells at precision 6 sharing the same 5-character prefix. This allows multi-scale queries without a separate index.
- Redis natively stores geohash internally in its `GEORADIUS` sorted-set structure. Using a custom inverted index (`SADD "gh6:{cell}" entity_id`) allows multi-category filtering not supported by `GEORADIUS`.
- S2 is more accurate (especially near poles and for large radii) but requires implementing the S2 library client and is more complex to operate. For radii ≤ 50 km at mid-latitudes, geohash error is <1% of the cell size — acceptable given we filter by exact haversine distance post-lookup.
- Quadtree requires a tree structure maintained in memory or a database; it cannot be trivially implemented with Redis primitive data structures. It excels when entity density varies wildly (urban vs. rural) but the constant 9-cell lookup of geohash is simpler to reason about.

**Implementation Detail**:

```python
import geohash2  # Python geohash library
import math
import redis

GEOHASH_NEIGHBORS = {
    # Direction offsets for geohash neighbor calculation
    # Standard lookup table — precomputed for each precision level
}

class GeohashProximityIndex:
    PRECISION_BY_RADIUS = {
        # radius_m -> geohash precision to use
        # At precision P, cell width is approx: 20_000 km / 2^(P*2.5)
        50:      9,   # ~2.4 m cells
        200:     8,   # ~19 m cells
        500:     7,   # ~76 m cells
        5_000:   6,   # ~1.2 km cells
        20_000:  5,   # ~4.9 km cells
        100_000: 4,   # ~39 km cells
    }

    def __init__(self, redis_client: redis.RedisCluster):
        self.r = redis_client

    def _choose_precision(self, radius_m: int) -> int:
        """Select geohash precision where cell_size ≈ radius/2."""
        for threshold, precision in sorted(self.PRECISION_BY_RADIUS.items()):
            if radius_m <= threshold:
                return precision
        return 4  # fallback for very large radii

    def add_entity(self, entity_id: str, lat: float, lon: float, category: str):
        """Add entity to geohash index at all relevant precisions."""
        for precision in [4, 5, 6, 7]:  # multi-precision index
            cell = geohash2.encode(lat, lon, precision=precision)
            key = f"gh{precision}:{category}:{cell}"
            self.r.sadd(key, entity_id)
        # Store entity detail hash
        self.r.hset(f"entity:{entity_id}", mapping={
            "lat": lat, "lon": lon, "category": category
        })

    def remove_entity(self, entity_id: str, lat: float, lon: float, category: str):
        """Remove entity from all geohash cells."""
        for precision in [4, 5, 6, 7]:
            cell = geohash2.encode(lat, lon, precision=precision)
            key = f"gh{precision}:{category}:{cell}"
            self.r.srem(key, entity_id)
        self.r.delete(f"entity:{entity_id}")

    def search(self, lat: float, lon: float, radius_m: int,
               category: str = None) -> list[dict]:
        """
        Return entities within radius_m of (lat, lon).
        Uses geohash cells at appropriate precision + haversine post-filter.
        """
        precision = self._choose_precision(radius_m)

        # 1. Get the query cell and all 8 neighbors
        center_cell = geohash2.encode(lat, lon, precision=precision)
        neighbor_cells = geohash2.neighbors(center_cell)  # returns dict of 8 neighbors
        cells_to_check = [center_cell] + list(neighbor_cells.values())

        # 2. Fetch entity_ids from all cells via Redis pipeline
        pipe = self.r.pipeline(transaction=False)
        cat_filter = category or "*"
        for cell in cells_to_check:
            if category:
                pipe.smembers(f"gh{precision}:{category}:{cell}")
            else:
                # Without category filter, we'd need to SUNION across all categories
                # In practice, always supply a category to keep index keys manageable
                pipe.smembers(f"gh{precision}:*:{cell}")  # glob — use SCAN in prod
        cell_results = pipe.execute()

        # 3. Deduplicate entity_ids
        all_entity_ids = set()
        for s in cell_results:
            all_entity_ids.update(s)

        if not all_entity_ids:
            return []

        # 4. Fetch entity details (pipeline)
        pipe = self.r.pipeline(transaction=False)
        for eid in all_entity_ids:
            pipe.hgetall(f"entity:{eid}")
        details = pipe.execute()

        # 5. Compute haversine distance and filter
        results = []
        for eid, detail in zip(all_entity_ids, details):
            if not detail:
                continue
            e_lat = float(detail[b"lat"])
            e_lon = float(detail[b"lon"])
            dist_m = haversine_distance_m(lat, lon, e_lat, e_lon)
            if dist_m <= radius_m:
                results.append({
                    "entity_id": eid,
                    "lat": e_lat, "lon": e_lon,
                    "distance_m": dist_m,
                    "category": detail[b"category"].decode(),
                })

        # 6. Sort by distance
        results.sort(key=lambda x: x["distance_m"])
        return results


def haversine_distance_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Compute great-circle distance in meters using haversine formula."""
    R = 6_371_000  # Earth radius in meters
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda/2)**2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
```

**Interviewer Q&As**:

1. **Q: Why do we need to check 8 neighboring cells and not just the cell containing the query point?**
   A: A query point near the edge of its geohash cell can have entities just across the cell boundary that are still within the search radius. If we only check the center cell, those entities are missed (false negatives). By checking the center + 8 neighbors (the Moore neighborhood), we guarantee coverage for any point within the cell, at the cost of ~9× more lookups. The radius must be ≤ half the cell width to guarantee 9 cells suffice; for larger radii, we use a lower precision (larger cells).

2. **Q: What happens if the radius is larger than the geohash cell size (e.g., 50 km radius but precision-5 cells are ~4.9 km)?**
   A: For a 50 km radius, precision-5 cells (4.9 km) are too small — we'd need a ring of ~100 cells. We switch to precision-4 cells (~39 km) and check the center cell + 8 neighbors (9 cells covering a ~117 km × 117 km area). This over-fetches but the haversine post-filter discards false positives. Alternatively, compute the exact set of covering cells using the S2 cell union algorithm for precision with large radii.

3. **Q: Geohash distorts at the poles. Does this matter for your service?**
   A: At latitudes above 60° (Scandinavia, Alaska, Canada), geohash cells are significantly narrower east-west than at the equator, and cells near longitude ±180° can have discontinuous neighbors (the antimeridian problem). For a globally deployed service, we handle this by: (1) at high latitudes, using one precision level lower to get larger cells that compensate for distortion; (2) for entities near the antimeridian (±180°), using S2 cells instead of geohash; (3) running a validation test suite that queries near known edge cases (poles, antimeridian, equatorial crossings).

4. **Q: How do you handle the "update location" operation in the geohash index when a driver moves from one cell to another?**
   A: The Geo Index Updater (Kafka consumer) maintains the previous geohash cell per entity (cached in Redis as part of the entity detail hash). On each location update: (1) compute new geohash cell; (2) if new cell ≠ old cell, atomically `SREM old_cell entity_id` and `SADD new_cell entity_id` using a Redis pipeline; (3) update entity detail hash with new lat/lon and new geohash. This ensures no window where the entity appears in zero cells or two cells.

5. **Q: Why not use Redis GEORADIUS command directly instead of a custom geohash inverted index?**
   A: `GEORADIUS` is excellent for simple distance queries but has limitations: (1) it stores all entities in a single sorted set — category filtering requires a separate sorted set per category, and cross-category queries need `GEORADIUSBYMEMBER` on multiple keys + client-side merge; (2) for 500 M entities with 100+ categories, managing 100 sorted sets of varying sizes is feasible but our custom index provides more flexibility for multi-attribute filtering; (3) `GEORADIUS` uses a fixed geohash radius approximation that can return false positives up to 30% of the cell diagonal. Our custom index + haversine filter is equally accurate and more extensible.

---

### 6.2 High-Frequency Location Update Pipeline

**Problem it solves**: Accept 1 M–3 M location updates per second from real-time movers, update their searchable position within 5 s, and avoid overwhelming the geo index store with writes that would block reads.

**Approaches Comparison**:

| Approach | Write Throughput | Read Consistency | Complexity | Failure Mode |
|---|---|---|---|---|
| Synchronous direct Redis write | Medium (Redis single-thread per shard) | Immediate | Low | Redis overload; blocking reads during writes |
| Async Kafka → Redis consumer | Very High | 1–5 s lag | Medium | Kafka lag increases under load; catchup window |
| Client-side rate limiting (1 update/s) | High (controlled) | Immediate for limited writes | Low | Stale during high mobility |
| Write-behind cache (Redis → DB async) | Very High | Eventual (DB) | Medium | Cache crash before DB sync = data loss |
| Dedicated write path: HTTP → in-memory queue → batch Redis MSET | Very High | Near-immediate | Low-Medium | Queue overflow on crash |

**Selected Approach**: Two-tier write path: (1) immediate Redis `HSET` update for Live Location Store (lat/lon of each mover, TTL 30 s), served directly to search queries; (2) async Kafka event for Geo Index cell-transition updates (lower frequency — only when geohash cell changes).

**Detailed Reasoning**:
- Separating "live position" (Redis hash) from "geo index" (Redis set per cell) is key. The geo index only needs updating when a mover crosses a cell boundary (~every 1–2 minutes for a car at 40 km/h in precision-6 cells of 1.2 km). The raw lat/lon update to the Live Location Store happens on every probe (every 5 s) but is a single `HSET` with TTL refresh — very cheap.
- This means only ~1/24 of updates (crossing cell boundary) trigger a geo index write. Effective geo index write rate: 5 M movers × (1 cell crossing / 120 s) = ~42 K writes/s to the geo index — easily handled by Redis.
- The 30 s TTL on the Live Location Store auto-removes movers who go offline, keeping the index clean without explicit delete calls.

**Implementation Detail**:

```python
class LocationUpdateService:
    def __init__(self, redis_live: redis.RedisCluster,
                 kafka_producer: KafkaProducer,
                 geo_index: GeohashProximityIndex):
        self.redis_live = redis_live
        self.kafka_producer = kafka_producer
        self.geo_index = geo_index

    async def handle_location_update(self, entity_id: str, lat: float, lon: float,
                                      speed_mps: float, heading: int, timestamp: int):
        """
        Fast path: update live location immediately.
        Slow path: publish cell-transition event to Kafka for geo index update.
        """
        live_key = f"loc:{entity_id}"
        new_geohash6 = geohash2.encode(lat, lon, precision=6)

        # Fast path: always update live location (single Redis pipeline)
        pipe = self.redis_live.pipeline(transaction=False)
        pipe.hset(live_key, mapping={
            "lat": lat, "lon": lon, "speed": speed_mps,
            "heading": heading, "ts": timestamp,
            "gh6": new_geohash6
        })
        pipe.expire(live_key, 30)  # TTL: auto-expire in 30s if no further update
        old_data = pipe.execute()  # returns [hset_result, expire_result]

        # Check if geo cell changed (requires previous geohash from the hash itself)
        # We read it back cheaply using a pipelined HGET before the update
        # In practice, store prev_gh6 in a separate key or use a Lua script:
        old_gh6 = await self._get_previous_geohash(entity_id)

        if old_gh6 and old_gh6 != new_geohash6:
            # Slow path: publish cell-transition event to Kafka
            event = {
                "entity_id": entity_id,
                "old_gh6": old_gh6,
                "new_gh6": new_geohash6,
                "lat": lat,
                "lon": lon,
                "timestamp": timestamp
            }
            await self.kafka_producer.send(
                topic="location-cell-transitions",
                key=entity_id.encode(),   # partition by entity for ordering
                value=json.dumps(event).encode()
            )

    async def _get_previous_geohash(self, entity_id: str) -> str | None:
        """Fetch previous geohash6 from live location store."""
        result = self.redis_live.hget(f"loc:{entity_id}", "gh6")
        return result.decode() if result else None


# ─────────────────────────────────────────────
# Kafka Consumer: Geo Index Updater
# ─────────────────────────────────────────────
class GeoIndexUpdater:
    """
    Consumes cell-transition events from Kafka.
    Updates the geohash inverted index in Redis.
    Runs as a horizontally scaled consumer group.
    """
    def __init__(self, geo_index: GeohashProximityIndex,
                 entity_db: EntityDB):
        self.geo_index = geo_index
        self.entity_db = entity_db

    def process(self, event: dict):
        entity_id = event["entity_id"]
        old_gh6 = event["old_gh6"]
        new_gh6 = event["new_gh6"]
        lat = event["lat"]
        lon = event["lon"]

        # Fetch entity category from entity DB (cached in Redis)
        entity = self.entity_db.get(entity_id)
        if not entity or not entity["is_active"]:
            return  # Skip inactive or deleted entities

        category = entity["category"]

        # Atomic geo index transition: remove from old cell, add to new cell
        pipe = self.geo_index.redis.pipeline(transaction=True)  # MULTI/EXEC
        pipe.srem(f"gh6:{category}:{old_gh6}", entity_id)
        pipe.sadd(f"gh6:{category}:{new_gh6}", entity_id)
        # Update entity detail hash with new position
        pipe.hset(f"entity:{entity_id}", mapping={"lat": lat, "lon": lon, "gh6": new_gh6})
        pipe.execute()
```

**Interviewer Q&As**:

1. **Q: How do you handle a mover going offline (app killed, phone off)?**
   A: The `loc:{entity_id}` key in the Live Location Store has a 30 s TTL. When the last update expires, the key vanishes. The Search Service checks for a `loc:` key; if absent, it falls back to the entity's last known position in the entity detail hash (or excludes the entity if `show_offline=false` is specified). A separate background job can run to remove permanently offline movers from the geo index (via a Kafka heartbeat miss event).

2. **Q: At 1 M location updates/s, how many Redis writes is the system actually doing?**
   A: (1) Live location `HSET` + `EXPIRE`: 1 M/s × 2 Redis commands = 2 M Redis ops/s. (2) Geo index update (cell transition only): assume a driver crosses a geohash-6 cell (~1.2 km) every ~2 min at 40 km/h → 5 M movers / 120 s = ~42 K cell transitions/s. Redis single-threaded node handles ~200 K ops/s, so we need 2 M / 200 K = 10 Redis nodes for live location + a few nodes for geo index. A Redis cluster with 12–20 shards handles this comfortably.

3. **Q: What if a Kafka consumer (Geo Index Updater) is lagging behind? What is the impact?**
   A: The geo index may temporarily show a mover in an old cell (stale position in search results). However, the Search Service cross-references the `loc:{entity_id}` live hash (updated immediately) to get the true current position. The haversine filter uses the true position, so geo index lag causes only slightly over-fetching (entities near old cell boundary appear in results but pass the distance filter correctly). The effective staleness for a user is bounded by the Live Location Store TTL, not the Kafka lag.

4. **Q: How do you avoid a Redis hotspot when many movers are in the same geohash cell (e.g., a sports stadium parking lot)?**
   A: The `SADD "gh6:{category}:{cell}" entity_id` key is the hot set. For a stadium with 50 K drivers in the same cell, a single `SMEMBERS` call returns 50 K entity_ids, taking ~50 ms — too slow. Mitigation: (1) sub-shard large cells: if a cell's set size exceeds 10 K, split it into `gh7:` sub-cells (next precision level) and redirect queries to sub-cell lookups; (2) cache the result of `SMEMBERS` for hot cells with a 1 s TTL; (3) apply a query-time limit (first N entity_ids from the set) and post-sort by sub-cell geohash proximity.

5. **Q: How would you add support for polygon-based search (e.g., "find all restaurants within this neighborhood boundary")?**
   A: Geohash cells are axis-aligned rectangles, not arbitrary polygons. For a polygon query, use the S2 cell cover algorithm: compute the minimal set of S2 cells that covers the polygon to the desired precision. Look up all entities in those cells (same inverted index pattern), then apply a point-in-polygon test (ray casting or winding number algorithm) for precise filtering. Store the neighborhood polygon as an S2 cell union in a separate "region index" table; queries join via region_id → cell_ids → entity_ids.

---

### 6.3 Search Result Ranking

**Problem it solves**: When a radius query returns 200 entities, how do you rank them so the most relevant results appear first? Pure distance ranking ignores quality (a 1-star restaurant 10 m away is worse than a 4.5-star one 300 m away).

**Approaches Comparison**:

| Approach | Factors | Personalization | Complexity | Latency |
|---|---|---|---|---|
| Distance-only sort | Distance | None | Trivial | O(n log n) |
| Weighted linear score | Distance + rating + review_count | None | Low | O(n log n) |
| Popularity-based (recency/check-ins) | Recency, check-in count | None | Low | O(n log n) |
| ML ranking model (LambdaRank) | All features + user history | Full | High | 10–50 ms |
| Two-phase: fast filter + ML rerank | Top 100 by distance, then ML rerank | Partial | Medium | 5–20 ms |

**Selected Approach**: Two-phase ranking — (1) retrieve top-200 candidates by distance; (2) score each with a weighted formula incorporating distance, rating, review_count, and open_now status. ML reranking reserved for authenticated power users via a feature flag.

**Score formula**:
```
score = (1 / (1 + distance_m / radius_m))^0.5   # distance decay
      × (rating / 5.0)^1.2                        # rating boost (super-linear)
      × log10(1 + review_count) / log10(10001)    # popularity (log-scaled, cap at 10K reviews)
      × (1.2 if open_now else 0.8)                # open/closed modifier
```

This formula is transparent, fast (no model inference), and tunable by adjusting exponents per category. It is computed in O(n) after the distance-filter step.

**Interviewer Q&As**:

1. **Q: How do you incorporate personalization (user's past preferences) into ranking?**
   A: Personalization requires a user feature vector: preferred categories, average rating of visited places, typical price level. This is fetched from a user profile service (Redis cache, ~1 ms latency). The ML ranking model (a 3-layer neural network trained with LambdaRank on click-through data) takes entity features + user features as input and outputs a relevance score. For cold-start users (no history), the global popularity model is used.

2. **Q: How do you ensure ranking is fair for new businesses with few reviews?**
   A: The `log10(1 + review_count)` term saturates at a defined maximum, meaning a business with 10 reviews and 4.8 stars isn't buried below one with 1000 reviews and 3.5 stars. Additionally, new businesses (<30 days old) receive a "novelty boost" multiplier (1.1×) to help them gain initial visibility. Category norms are used: a new restaurant with 10 reviews is compared against the typical review count for restaurants in that area, not globally.

3. **Q: How would you A/B test a new ranking formula?**
   A: The ranking formula is version-controlled. A feature flag assigns 5% of users to "treatment" (new formula). The primary metric is Click-Through Rate (CTR) on result #1 and #3, plus downstream conversion (e.g., user navigated to the place). Secondary metrics: session abandonment rate, average position of clicked results. Results analyzed after 2 weeks with a two-sample t-test. The formula is promoted if CTR increases by >2% with p < 0.05.

---

## 7. Scaling

### Horizontal Scaling

- **Search Service**: Stateless; scale to 17 K search RPS with ~100 pods (170 RPS/pod, well within limits for Redis pipeline queries).
- **Location Update Service**: Stateless; scale to 1 M RPS with ~500 pods (2 K RPS/pod for simple Redis `HSET`).
- **Kafka**: 200 partitions on the location-cell-transitions topic; each partition consumer is a separate Geo Index Updater thread.
- **Geo Index Updater**: Scale consumer group to match Kafka partition count (200 instances max).

### DB Sharding

- **Redis Cluster (Geo Index)**: 20 shards, hash-slot-based key distribution. Key `gh6:{category}:{cell}` distributes evenly across shards by consistent hash of key.
- **Redis Cluster (Live Location)**: Separate cluster from geo index. 10 shards for `loc:{entity_id}` keys. 5 M active movers × 200 bytes = 1 GB live data — fits in 10 × 1 GB Redis shards with headroom.
- **PostgreSQL (Entity DB)**: Partition entity table by `entity_type` (range partition). Business entities are mostly static; driver entities change frequently. Consider read replicas per region for global scale; PlanetScale or Vitess for MySQL if write throughput exceeds single PostgreSQL node capacity.

### Replication

- Redis: Each shard has 1 primary + 2 replicas (for read scaling and HA). Writes go to primary; reads can go to replicas with `WAIT 1 0` for near-synchronous replication.
- Kafka: Replication factor 3, min-ISR 2.
- PostgreSQL: 1 primary + 2 read replicas; hot-standby for failover.

### Caching

| Cache Layer | Technology | TTL | Content |
|---|---|---|---|
| Entity detail (static) | Redis Hash `entity:{id}` | 5 min | name, category, lat, lon, rating |
| Live location (movers) | Redis Hash `loc:{id}` | 30 s (TTL auto-expire) | Current lat/lon, speed, heading |
| Search result page | Redis String `search:{hash(params)}` | 10 s | Paginated result JSON |
| CDN (API responses) | Fastly/CloudFront | 5 s (for /nearby) | Search result JSON for common queries |

### CDN

- `/nearby` responses with `?lat=...&lon=...&radius=...` are cache-unfriendly (continuous coordinates). However, quantizing to the nearest 0.001° (≈ 100 m) and caching with a 5 s TTL captures repeated queries from users in the same area (bus stops, office buildings). The quantization is done in the API Gateway before forwarding to the Search Service.

**Interviewer Q&As**:

1. **Q: How do you prevent a thundering herd when many users search in the same area at the same time (e.g., concert venue)?**
   A: Request coalescing at the API Gateway level: queries with identical quantized coordinates and radius are grouped; only one query runs against Redis; all waiting clients receive the same response. Additionally, the CDN caches the response for the quantized query key for 5 s. For the geo index, hot geohash cells (>10 K members) are cached as a Redis string with a 1 s TTL to avoid repeated `SMEMBERS` calls.

2. **Q: As entities grow from 500 M to 5 B, what changes?**
   A: The geo index (`SADD` sets per geohash cell) scales linearly — just more entity_ids per cell and more cells used. Redis memory scales linearly too: 5 B entities × 8 bytes/entity_id = 40 GB index, distributed across 200 Redis shards (200 MB each — trivial). The entity metadata DB (PostgreSQL) would need to move to a distributed database (CockroachDB, Spanner, or sharded MySQL via Vitess) to handle the write load. Search Service pods scale proportionally with QPS.

3. **Q: How do you handle multi-region global deployment?**
   A: Deploy the full stack (Search Service, Redis Cluster, Entity DB) in each geographic region (US, EU, APAC, etc.). Entity data is replicated cross-region via CDC (Change Data Capture) from the primary Entity DB. Real-time mover data is region-local (a driver's GPS position is only needed in the region they're operating in). Search queries are routed to the nearest region via GeoDNS. Entity metadata updates are propagated cross-region with ~30 s eventual consistency.

4. **Q: Your Redis geo index is in memory. What happens if the Redis cluster loses data (crash)?**
   A: Two-tier durability: (1) Redis AOF (append-only file) with `appendfsync everysec` provides at-most-1-second data loss on crash; (2) on startup, if AOF replay is incomplete, the Geo Index Bootstrap job reads all active entities from the PostgreSQL Entity DB and rebuilds the Redis index (500 M entities × 8 bytes × 4 precision levels ≈ 16 GB — rebuilt in ~10 minutes from a parallel batch loader). During rebuild, searches fall back to PostgreSQL PostGIS queries (slower but correct). An alert triggers on Redis startup to notify ops of the fallback.

5. **Q: How would you add support for real-time location sharing between friends ("see where your friends are")?**
   A: Friends' locations require a social graph overlay on the proximity index. When user A queries "nearby friends," we: (1) fetch A's friend list from the social graph service; (2) look up each friend's `loc:{friend_id}` key in the Live Location Store; (3) filter by those within the radius; (4) return distances. The key addition is a "friend visibility" permission model: a user can set their location to visible/invisible per friend or globally. The Live Location Store stores a `visibility` field per entity; the Search Service checks it before including results. At 1 000 friends max, this is 1 000 Redis `HGET` calls — acceptable in a pipeline.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Mitigation | Recovery |
|---|---|---|---|
| Redis geo index shard failure | ~5% of geo queries fail (1/20 shards) | Redis Cluster automatic failover to replica within 1–2 s | <5 s |
| Redis live location shard failure | ~10% of movers show stale position | Automatic failover to replica; entity detail hash used as fallback | <5 s |
| Kafka broker failure | Location update Kafka lag increases | RF=3 replication; auto-leader-election within 30 s | <30 s |
| Geo Index Updater consumer crash | Geo index cell-transitions delayed | Consumer group rebalances; remaining consumers process lagged partitions | <60 s catch-up |
| Search Service pod crash | Reduced capacity; load balancer removes failed pod | Kubernetes restarts pod; LB health check removes it immediately | <10 s |
| Entity DB (PostgreSQL) primary failure | Entity registration/metadata writes fail | Read replicas serve reads; primary failover (Patroni) within 30 s | <30 s |
| Location update service overload | Updates dropped | Kafka acts as buffer; Location Update Service sheds load gracefully with 503 back-pressure | Consumer catches up |
| Geohash index inconsistency (crash during cell transition) | Entity in 0 or 2 cells | Kafka idempotent consumer replays event; MULTI/EXEC ensures atomicity of SREM/SADD | Self-healing on replay |

**Retries & Idempotency**:
- Location updates: `timestamp` field in each event. Kafka consumer checks if the event's timestamp is newer than the stored entity timestamp; older events are discarded. Idempotent `HSET` (same value applied twice has same result).
- Entity registration: POST `/entities` is idempotent on `(entity_type, lat, lon, name)` hash — duplicate registrations return the existing entity_id rather than creating a duplicate.
- Geo Index Updater: Each Kafka message contains old_gh6 and new_gh6. Re-processing a cell-transition event results in a no-op `SREM` (key not found is not an error) and an idempotent `SADD`.

**Circuit Breaker**: Search Service implements a circuit breaker around each Redis shard. If a shard's error rate exceeds 1% in 5 s, the breaker opens; queries that would hit that shard fall back to the PostgreSQL PostGIS query (slower but functional). The breaker half-opens after 10 s.

---

## 9. Monitoring & Observability

| Metric | Description | Alert Threshold |
|---|---|---|
| `search_latency_p99_ms` | 99th percentile search latency | >100 ms |
| `location_update_lag_seconds` | Kafka consumer lag / (updates/s) | >5 s |
| `geo_index_size_cells` | Total occupied geohash cells | Informational (growth tracking) |
| `redis_memory_used_bytes` | Redis cluster memory utilization | >80% capacity |
| `location_update_rps` | Updates/s to Location Update Service | <500 K (degraded below baseline) |
| `search_false_negative_rate` | Ground-truth precision test | >0.01% |
| `entity_registration_error_rate` | Rate of failed entity writes | >0.1% |
| `kafka_consumer_lag_messages` | Lag in geo-index-updates topic | >100 K messages |
| `redis_eviction_rate` | Keys evicted due to `maxmemory` | >0/s (should never evict geo index) |
| `circuit_breaker_open_count` | Open circuit breakers per Redis shard | >0 |

**Distributed Tracing**: Each search request gets a trace ID propagated through API Gateway → Search Service → Redis pipeline calls. Traces sampled at 1% normally, 100% when latency > 100 ms. Stored in Jaeger/Tempo. Critical spans: geohash cell computation, Redis pipeline fetch, haversine filter computation, ranking computation.

**Logging**: Structured logs with fields: `{trace_id, entity_id, operation, latency_ms, geo_cells_checked, results_returned, cache_hit}`. Search logs anonymized (query coordinates rounded to geohash-5 cell for privacy). Location update logs store only entity_id, timestamp, and geohash cell — not raw coordinates.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B (Chosen) | Reason |
|---|---|---|---|
| Geo index algorithm | S2 cells | Geohash (base-32) | Simpler implementation, Redis-native support, sufficient accuracy for radii ≤ 50 km at mid-latitudes |
| Live location store | Direct geo index update | Separate Redis hash + async Kafka for cell transitions | Reduces geo index write rate from 1 M/s to ~42 K/s; TTL-based auto-cleanup for offline movers |
| Entity metadata DB | DynamoDB | PostgreSQL + PostGIS | PostGIS enables admin geo queries; ACID needed for entity count consistency |
| Search hot path DB | PostgreSQL ST_DWithin | Redis geohash inverted index | Redis gives sub-millisecond lookups; PostGIS too slow for 17 K RPS |
| Ranking method | ML LambdaRank | Weighted formula (distance + rating + review count) | ML adds 10–50 ms latency; formula is transparent, tunable, and achieves 85% of ML quality |
| Result consistency | Strong (read your writes) | Eventual (5 s for movers) | 5 s staleness acceptable for proximity; strong consistency not achievable at 1 M writes/s without sacrifice |

---

## 11. Follow-up Interview Questions

1. **Q: How would you add "ETA to entity" (not just distance) in search results?**
   A: Integrate with a routing service: after the proximity search returns candidates, call the routing service with the user's location and the top-K entity locations (batch route request). Use cached travel times for common (origin_cell, dest_cell) pairs. The routing service returns driving/walking ETAs which replace raw distance in the ranking score. This adds ~100 ms latency; apply only to top-10 candidates after initial distance ranking.

2. **Q: How would you design a "notify me when an entity enters my radius" (geo-fence push notification) feature?**
   A: A geo-fence is stored as `(user_id, center_lat, center_lon, radius_m, target_category)`. On every location update event, the Kafka consumer checks if the updated entity matches any registered geo-fences (using an inverse index: geohash_cell → list of geo-fences watching that cell). If the entity enters the fence, a notification event is published to a push notification service. Scaling challenge: 10 M active geo-fences × 1 M updates/s would require checking 10 T conditions/s — partition geo-fences by geohash cell to limit comparisons per update.

3. **Q: Describe how you'd handle driver surge in a ride-share application using this proximity service.**
   A: For surge pricing, the Platform Service queries `/nearby?category=driver&radius_m=5000` every 30 s per active demand zone (geohash-5 cell with active ride requests). Driver density = count(drivers in zone) / zone_area_km². The surge multiplier is a function of driver density vs. historical average for that zone and time-of-day. Surge calculation is done in a separate Pricing Service that calls the Proximity API; the Proximity Service itself is agnostic to surge logic.

4. **Q: How would you implement "search along route" — find gas stations within 2 km of a given driving route?**
   A: Decompose the route polyline into segments of ~2 km length. For each segment midpoint, run a `/nearby?radius_m=2000&category=gas_station` query. Deduplicate results across queries (entity_id set). This generates O(route_length_km / 2) parallel proximity queries; for a 100 km route, that's ~50 queries pipelined in parallel. A dedicated "route search" endpoint handles this internally to avoid client round trips.

5. **Q: The Geo Index Updater is a single consumer group. How do you scale it to handle 1 M cell transitions/s?**
   A: Increase Kafka partition count to 1 000; increase consumer group size to 1 000 instances. Each consumer handles 1 K transitions/s — well within Redis pipeline throughput. The consumer is stateless (no local state beyond the Kafka offset); adding instances only requires a group rebalance (seconds). The bottleneck shifts to Redis at extreme scale: 1 M writes/s to geo index requires ~50 Redis shards (20 K ops/s each, leaving headroom).

6. **Q: How do you handle soft deletes vs. hard deletes for business entities?**
   A: Soft delete (mark `is_active=FALSE`) is the default path: entity remains in the entity DB for audit but is removed from the Redis geo index immediately (the Entity Registry Service issues `SREM` on deletion). Soft-deleted entities are excluded from all search results (geo index removal ensures they don't appear). Hard delete (GDPR erasure request) requires: (1) DB record deletion, (2) all Redis keys cleaned up, (3) Kafka event for downstream consumers to purge cached data. A data-retention Dataflow job runs nightly to hard-delete soft-deleted entities older than 90 days.

7. **Q: How would you design the system to support entities that have multiple locations (a chain restaurant)?**
   A: Each physical location is a separate entity record with its own entity_id, lat/lon, and geohash. They share a `brand_id` foreign key pointing to a Brand entity (name, logo, global metadata). Search results that return multiple locations of the same brand are grouped by brand_id on the client side; the API can optionally return one result per brand (the nearest location of each brand) via a `deduplicate_by_brand=true` query parameter. This is implemented as a post-sort deduplication step in the Search Service.

8. **Q: How do you maintain the geohash index consistency when the entity DB and Redis get out of sync?**
   A: A periodic reconciliation job (runs every 15 min) queries the Entity DB for all active entities and their expected geohash6 cells, then scans the Redis geo index and compares. Discrepancies (entity in wrong cell, entity missing from index, inactive entity still in index) are corrected. This is an O(N) scan of 500 M entities; distributed across 100 workers in a Dataflow job, each scanning a geohash cell range, it completes in ~5 min. Discrepancies trigger a Slack alert if above 0.01% rate.

9. **Q: How would you design the proximity service to work in environments with low connectivity (e.g., emerging markets with 2G networks)?**
   A: Offline-capable proximity: the client downloads a compressed binary blob of entity data for the nearby geohash cells (say, a 50 km × 50 km tile of business locations — ~500 KB compressed). The local proximity query runs in-browser or in-app against this downloaded dataset. Delta syncs update the local dataset periodically (on WiFi). This is the pattern used by Google Maps offline mode and OsmAnd.

10. **Q: What are the privacy implications of storing fine-grained real-time location for 5 M movers?**
    A: Key risks: (1) unauthorized access to driver location (stalking, surveillance), (2) location history enabling behavioral profiling. Mitigations: (1) `loc:{entity_id}` keys in Redis are accessible only to the entity owner and authorized platform services (ACL enforced at API Gateway); (2) raw GPS coordinates are never written to any durable store — only geohash-level precision in the geo index; (3) full GPS history (Cassandra table) is visible only to the entity owner and is auto-deleted after 30 days; (4) aggregated fleet heatmaps are anonymized (k-anonymity ≥ 5 per cell before publishing).

11. **Q: How would you design a "find entities within a polygon" feature (not just a circle)?**
    A: Implement S2 cell cover: given a polygon defined by its vertices, compute the minimal S2 cell union that covers the polygon at precision level P. Look up all entities in those cells (same inverted index). Apply a point-in-polygon test (PIP) using ray casting for precise filtering. The S2 cell cover is computed client-side (S2 library available in JS/Go/Python) and passed as a list of cell tokens in the API request. This avoids server-side polygon computation and leverages the existing cell-based index.

12. **Q: How do you handle search near the international date line (longitude ±180°)?**
    A: Geohash has a discontinuity at ±180° — cells on either side have completely different hash strings despite being adjacent. Fix: for queries within 1 000 km of the antimeridian, generate two sets of geohash cells — one around the positive-side boundary and one around the negative-side boundary — and union the results. In the code, if `lon > 179` or `lon < -179`, a "wrap-around query" flag is set and the geohash computation runs twice. Alternatively, switch to S2 cells for users in Pacific Island regions; S2 handles the antimeridian natively because it uses a 3D cube projection.

13. **Q: How do you test the proximity service for correctness (no false negatives)?**
    A: Three-layer testing: (1) Unit tests: for each geohash precision level, generate random (lat, lon, radius) queries, compute expected results via brute-force haversine over a fixed entity set, compare with the geohash index result. Assert zero false negatives. (2) Integration tests: load a real-world dataset of 1 M businesses, run 10 K random queries, compare against PostGIS `ST_DWithin` as the ground truth. (3) Production shadow testing: 0.1% of production queries are also run against PostGIS; false-negative rate is monitored continuously.

14. **Q: How would you design "time-aware" proximity — only show businesses that are open right now?**
    A: Each business entity has an `hours_json` field encoding OpenHours spec (day/time ranges). The `open_now` filter is evaluated at query time in the Search Service: after fetching candidates from the geo index, for each candidate with `hours_json`, compute `is_open(hours_json, current_utc_time, entity_timezone)`. This is a CPU operation (no DB query) taking ~1 µs per entity. For 200 candidates per query, this adds ~0.2 ms. A pre-computed `next_open_change_at` timestamp per entity in the Redis hash allows the Search Service to skip entities that are closed until after `now` without parsing hours_json.

15. **Q: How would you charge API customers (rate limit tiers, billing)?**
    A: The API Gateway tracks request counts per API key in a Redis counter with a sliding window (token bucket algorithm). Tiers: Free (100 search calls/day), Starter (10 K/day), Business (1 M/day), Enterprise (custom). Each request is logged to a billing event Kafka topic; a billing aggregation service (Flink) computes daily totals per API key and writes to the billing DB. Overage charges are applied per additional 1 000 requests. Circuit breakers prevent billing system failures from blocking API traffic.

---

## 12. References & Further Reading

- Morton, G.M. (1966). "A Computer Oriented Geodetic Data Base and a New Technique in File Sequencing." IBM Technical Report. (Original geohash/Z-order curve concept)
- Niemeyer, G. (2008). Geohash. http://geohash.org/ (Original geohash specification)
- Google S2 Geometry Library. "S2 Geometry." https://s2geometry.io/
- Uber Engineering. "H3: Uber's Hexagonal Hierarchical Spatial Index." https://eng.uber.com/h3/ (2018)
- Redis Documentation. "Redis GEO Commands." https://redis.io/docs/data-types/geo/
- PostGIS Documentation. "ST_DWithin." https://postgis.net/docs/ST_DWithin.html
- Alex Xu. "System Design Interview Vol. 2." Chapter: Proximity Service. ByteByteGo (2022).
- Samet, H. (1990). "The Design and Analysis of Spatial Data Structures." Addison-Wesley. (Quadtree foundations)
- Eldawy, A., Mokbel, M.F. (2015). "SpatialHadoop: A MapReduce Framework for Spatial Data." *ICDE 2015*.
- Facebook Engineering. "Spatial Indexes at Scale." https://engineering.fb.com/2021/07/09/open-source/s2/
- Kafka Documentation. "Introduction to Kafka." https://kafka.apache.org/documentation/
