# System Design: Google Maps

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Map Tile Serving** — Render raster or vector map tiles at multiple zoom levels (z0–z22) for any geographic region worldwide.
2. **Routing Engine** — Compute optimal routes between two or more points given a travel mode (driving, walking, cycling, transit).
3. **Turn-by-Turn Navigation** — Deliver step-by-step navigation instructions with real-time rerouting when the user deviates from the planned route.
4. **Real-Time Traffic Ingestion** — Ingest GPS probe data from millions of mobile devices and connected vehicles; aggregate into a live traffic layer.
5. **ETA Calculation** — Produce accurate arrival-time predictions that incorporate historical speed profiles, live traffic, and road events.
6. **Street View** — Serve panoramic 360° imagery for street-level locations; support photosphere browsing.
7. **Business Listings & Search** — Allow users to search for places (restaurants, gas stations, ATMs) and surface rich business metadata.
8. **Map Update Pipeline** — Accept satellite imagery, OSM edits, fleet data, and user feedback; propagate to production tiles within an SLA.
9. **Places Autocomplete** — Suggest place names and addresses as the user types in the search box.
10. **Saved Places & Timeline** — Let authenticated users save locations and review personal location history.

### Non-Functional Requirements

- **Availability**: 99.99% uptime (≤52 min/year downtime); map tiles must be served even when routing is degraded.
- **Tile Latency**: p99 tile request ≤ 100 ms worldwide; median ≤ 30 ms from edge cache.
- **Route Latency**: p99 route calculation ≤ 2 s for continent-scale queries; p50 ≤ 400 ms.
- **Navigation Reroute**: Rerouting decision delivered within 1 s of deviation detection.
- **Traffic Freshness**: Aggregated traffic layer updated ≤ 60 s end-to-end.
- **Consistency**: Eventual consistency for map content; route/ETA must reflect traffic state no older than 2 min.
- **Scalability**: Must handle 1 B+ daily active users (DAU); traffic spikes 5× above baseline during events.
- **Durability**: Map data and Street View imagery retained indefinitely; no single point of data loss.
- **Security**: All user location data encrypted in transit (TLS 1.3) and at rest (AES-256); GDPR/CCPA compliant.
- **Geo-Distribution**: Serve users from the nearest data center; no cross-ocean round trips for tile fetches.

### Out of Scope

- Indoor mapping and floor-plan navigation.
- Drone/autonomous vehicle HD mapping.
- Real-time multiplayer location sharing (though architecture notes where it would plug in).
- Payments or booking integrations within the place detail page.
- Full ML model training pipelines for road-condition classification.

---

## 2. Users & Scale

### User Types

| Type | Description | Primary Operations |
|---|---|---|
| Anonymous Viewer | Browses the map in a browser/app without signing in | Tile fetches, place search, route requests |
| Authenticated User | Signed-in user with profile | All above + saved places, timeline, reviews |
| Navigation User | Active turn-by-turn session | High-frequency GPS probe uploads, reroute requests |
| Business Owner | Manages a Google Business Profile listing | Place data writes, photo uploads, hours edits |
| Data Pipeline Bot | Internal systems ingesting imagery/OSM/fleet GPS | Bulk writes to map data store |
| Developer (API) | Third-party apps using Maps Platform API | All public endpoints via API key |

### Traffic Estimates

**Assumptions**:
- 1 B DAU; average session = 8 min.
- 30% of sessions involve active navigation (300 M navigation sessions/day).
- A navigation user uploads GPS probe every 5 s → 12 probes/min.
- Map tile: each map view loads ~20 tiles (various zoom levels).
- Each tile is 256×256 px PNG ≈ 15 KB (raster) or 8 KB (vector/pbf).
- Route request rate: 500 M requests/day across all users.
- Street View: 50 M panorama fetches/day; each panorama ≈ 6 × 512 KB tiles = 3 MB.

| Metric | Calculation | Result |
|---|---|---|
| Peak tile RPS | (1 B sessions × 20 tiles) / 86 400 s × 3 (peak factor) | ~694 K RPS |
| Probe upload RPS | 300 M nav users × (1/5 s) = 60 M probes/s raw; 10% online simultaneously → 6 M probes/s | 6 M RPS |
| Route RPS | 500 M / 86 400 × 3 | ~17 K RPS |
| Place search RPS | 200 M searches/day / 86 400 × 3 | ~7 K RPS |
| Street View RPS | 50 M / 86 400 × 3 | ~1 700 RPS |
| Navigation reroute RPS | 10% of nav sessions reroute, avg 2× per session = 60 M / 86 400 × 3 | ~2 100 RPS |

### Latency Requirements

| Operation | p50 Target | p99 Target | Reason |
|---|---|---|---|
| Tile fetch (edge cache hit) | 15 ms | 50 ms | Perceived instant map pan |
| Tile fetch (origin miss) | 60 ms | 200 ms | Cache miss allowed small penalty |
| Route calculation (local) | 150 ms | 500 ms | UX: user expects near-instant |
| Route calculation (cross-country) | 400 ms | 2 000 ms | Graph traversal is CPU-intensive |
| ETA update during nav | 200 ms | 800 ms | Infrequent but must feel live |
| Probe ingest (write ack) | 50 ms | 200 ms | Client needs confirmation quickly |
| Place search autocomplete | 30 ms | 100 ms | Typing suggestion must feel instant |

### Storage Estimates

**Assumptions**:
- Earth surface ≈ 510 M km². At zoom 17 (street level), 1 tile covers ~480 m × 480 m ≈ 0.23 km².
- Total zoom-17 tiles = 510 M / 0.23 ≈ 2.2 B tiles; only ~20% are land → 440 M land tiles.
- Summed across all 23 zoom levels (pyramid), multiplier ≈ 1.33 → ~585 M unique tiles.
- Raster tile avg 15 KB; vector tile avg 8 KB. Dual format storage: (15+8) KB × 585 M = ~13.5 TB base.
- With 3× replication → ~40 TB for tiles.
- Street View: 170 B+ panoramas (Google's public figure); each compressed ~2 MB → 340 PB. Assumption: 10% "hot" served from SSD-backed storage = 34 PB hot tier.
- Road graph (global): ~2 B edges at 120 bytes each = ~240 GB.
- Traffic history: 5 years × 365 days × ~500 GB/day aggregated speed segments = ~912 TB → ~1 PB.
- Place data: 250 M places × 2 KB average record = 500 GB.
- User timeline/saved places: 1 B users × 10 KB = 10 TB.

| Data Type | Raw Size | Replicated (3×) |
|---|---|---|
| Map tiles (raster + vector) | 13.5 TB | 40.5 TB |
| Street View imagery (total) | ~340 PB | ~1 EB (cold) |
| Street View hot tier (10%) | 34 PB | 100 PB |
| Road graph | 240 GB | 720 GB |
| Traffic history (5 yr) | ~1 PB | ~3 PB |
| Place data | 500 GB | 1.5 TB |
| User data | 10 TB | 30 TB |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Tile egress | 694 K RPS × 15 KB (avg cache-hit ratio 95%, miss 5%) | ~10.4 GB/s = ~83 Tbps total edge |
| Probe ingress | 6 M RPS × 200 bytes | ~1.2 GB/s |
| Route response egress | 17 K RPS × 50 KB (polyline + instructions) | ~850 MB/s |
| Street View egress | 1 700 RPS × 512 KB per tile × 6 tiles | ~5.2 GB/s |

---

## 3. High-Level Architecture

```
                         ┌─────────────────────────────────────────────────────────────┐
                         │                        CLIENTS                               │
                         │  iOS App  │  Android App  │  Web Browser  │  Maps API SDK   │
                         └─────┬─────┴───────┬───────┴───────┬───────┴────────┬────────┘
                               │             │               │                │
                         ──────▼─────────────▼───────────────▼────────────────▼──────
                         │                  GLOBAL LOAD BALANCER / Anycast BGP         │
                         │              (Google Front End — GFE, ~130 PoPs)             │
                         ──────────────────────────────┬───────────────────────────────
                                                        │
               ┌────────────────────────────────────────┼──────────────────────────────────────┐
               │                                        │                                      │
    ┌──────────▼──────────┐             ┌───────────────▼──────────────┐        ┌─────────────▼──────────────┐
    │   CDN / Edge Cache  │             │      API Gateway / BFF        │        │  Probe Ingestion Gateway   │
    │  (tile & SV images) │             │  (Auth, rate-limit, routing)  │        │  (GPS probe write path)    │
    └──────────┬──────────┘             └───────────┬──────────────────┘        └─────────────┬──────────────┘
               │                                    │                                          │
    ┌──────────▼──────────┐    ┌────────────────────┼──────────────┐            ┌─────────────▼──────────────┐
    │  Tile Serving Svc   │    │                    │              │            │  Kafka (probe topic)        │
    │  (reads tile store) │    │         ┌──────────▼──┐  ┌────────▼──────┐    │  Partitioned by geohash     │
    └──────────┬──────────┘    │         │ Routing Svc │  │ Places Search │    └─────────────┬──────────────┘
               │               │         │  (graph DB) │  │    Service    │                  │
    ┌──────────▼──────────┐    │         └──────────┬──┘  └───────┬───────┘    ┌─────────────▼──────────────┐
    │   Tile Store        │    │                    │             │            │  Traffic Aggregation Svc    │
    │ (GCS / Bigtable)    │    │         ┌──────────▼──┐  ┌───────▼───────┐   │  (Flink streaming job)      │
    └─────────────────────┘    │         │  ETA Svc    │  │  Places DB    │   └─────────────┬──────────────┘
                               │         │  (ML model) │  │  (Spanner)    │                 │
                               │         └─────────────┘  └───────────────┘   ┌─────────────▼──────────────┐
                               │                                               │  Traffic Layer Store        │
                               │    ┌────────────────────────────────────┐     │  (Bigtable, keyed by        │
                               │    │         Map Update Pipeline         │     │   segment + time bucket)   │
                               │    │  (satellite ingest → OSM diff →    │     └────────────────────────────┘
                               │    │   tile renderer → tile store write)│
                               │    └────────────────────────────────────┘
                               │
                               │    ┌────────────────────────────────────┐
                               │    │  Street View Service               │
                               │    │  (panorama store → projection svc) │
                               │    └────────────────────────────────────┘
                               └────────────────────────────────────────────
```

**Component Roles**:

- **GFE (Global Front End)**: Google's anycast load balancer. Routes users to the nearest GCP region via BGP anycast. Terminates TLS, enforces DDoS protection.
- **CDN / Edge Cache**: Caches immutable map tiles and Street View tiles at ~130 PoPs. Cache-Control headers set `max-age=86400` for static tiles. Cache hit rate target: 95%+.
- **API Gateway / BFF**: Validates JWT/API-key auth, rate-limits by user/key, routes requests to downstream microservices. Backends For Frontend (BFF) pattern ensures mobile vs. web get appropriately shaped responses.
- **Tile Serving Service**: Reads pre-rendered tiles from GCS/Bigtable. Handles zoom-level translation, tile stitching for high-DPI screens, and overlay composition (traffic layer, transit lines).
- **Routing Service**: Runs the graph traversal algorithm (Contraction Hierarchies + A*) against the road graph stored in a distributed graph store. Reads the current traffic layer to penalize congested edges.
- **ETA Service**: ML model (gradient-boosted trees + deep learning) trained on historical GPS probe data, road segment free-flow speeds, and real-time traffic. Returns a probability distribution over arrival times.
- **Places Search Service**: Handles full-text and geo-filtered queries. Backed by a search index (Apache Lucene / custom) and a geo index (S2 cells). Serves autocomplete via a prefix trie/FST stored in memory.
- **Probe Ingestion Gateway**: Receives GPS probes over HTTP/2 or gRPC from navigation clients. Writes to Kafka partitioned by geohash cell of the reported coordinate.
- **Traffic Aggregation Service**: Apache Flink job consuming probe Kafka topic. Joins probes with road segment map, computes median speed per segment per 30 s bucket, emits to Traffic Layer Store.
- **Traffic Layer Store**: Bigtable table keyed by `(segment_id, timestamp_bucket)`. Serves as the source-of-truth for current and historical traffic speeds consumed by Routing and ETA services.
- **Map Update Pipeline**: Offline batch pipeline. Ingests satellite imagery (Cloud Storage), OSM diffs, manual edits. Runs road-extraction ML models, tile renderer (Mapnik/custom), and writes new tile versions to Tile Store.
- **Street View Service**: Serves panoramic imagery. Handles camera projection math server-side or ships projection metadata to the client. Backed by a distributed object store (GCS cold + SSD-backed hot tier).

**Primary Use-Case Data Flow (Turn-by-Turn Navigation)**:

1. User taps "Navigate" → app sends `POST /directions` with origin, destination, travel mode.
2. API Gateway authenticates token, applies rate limit, forwards to Routing Service.
3. Routing Service pulls road graph snapshot and current traffic speeds from Traffic Layer Store; runs CH + A* → returns polyline + waypoints.
4. ETA Service called in parallel; returns ETA with confidence interval.
5. App begins rendering tiles (fetched from CDN) and starts navigation UI.
6. Every 5 s, app uploads GPS probe → Probe Ingestion Gateway → Kafka.
7. Flink aggregator re-scores affected road segments; Traffic Layer Store updated within 60 s.
8. If the user deviates >30 m from the route, the app sends `POST /reroute`; Routing Service computes a new route from current position within 1 s SLA.
9. Navigation ends; app sends a final probe batch flush and session close event.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────
-- Road Graph (stored in distributed graph DB;
-- schema shown as logical SQL for clarity)
-- ─────────────────────────────────────────────

CREATE TABLE road_node (
    node_id        BIGINT       PRIMARY KEY,
    lat            DOUBLE       NOT NULL,  -- WGS84
    lon            DOUBLE       NOT NULL,
    node_type      TINYINT      NOT NULL,  -- 0=junction,1=endpoint,2=via
    s2_cell_id     BIGINT       NOT NULL,  -- S2 level-14 cell for spatial lookup
    INDEX idx_s2 (s2_cell_id)
);

CREATE TABLE road_edge (
    edge_id           BIGINT       PRIMARY KEY,
    source_node_id    BIGINT       NOT NULL REFERENCES road_node(node_id),
    target_node_id    BIGINT       NOT NULL REFERENCES road_node(node_id),
    road_class        TINYINT      NOT NULL,  -- 0=motorway,1=trunk,...,7=footway
    speed_limit_kmh   SMALLINT,
    length_m          FLOAT        NOT NULL,
    traversal_secs    FLOAT        NOT NULL,  -- base travel time (free flow)
    oneway            BOOLEAN      NOT NULL DEFAULT FALSE,
    toll              BOOLEAN      NOT NULL DEFAULT FALSE,
    bridge            BOOLEAN      NOT NULL DEFAULT FALSE,
    tunnel            BOOLEAN      NOT NULL DEFAULT FALSE,
    access_flags      INT          NOT NULL,  -- bitmask: car, truck, bicycle, foot
    geometry_wkb      BYTEA,                  -- encoded polyline for edge shape
    INDEX idx_source (source_node_id),
    INDEX idx_target (target_node_id)
);

-- Contraction Hierarchy shortcuts (precomputed)
CREATE TABLE ch_shortcut (
    shortcut_id       BIGINT PRIMARY KEY,
    source_node_id    BIGINT NOT NULL,
    target_node_id    BIGINT NOT NULL,
    contracted_node   BIGINT NOT NULL,  -- the node that was contracted
    weight            FLOAT  NOT NULL,  -- sum of the two underlying edge weights
    INDEX idx_ch_source (source_node_id)
);

-- ─────────────────────────────────────────────
-- Traffic Layer
-- ─────────────────────────────────────────────

-- Stored in Bigtable; shown as logical schema
-- Row key: edge_id#timestamp_bucket (Unix minute)
CREATE TABLE traffic_speed (
    edge_id           BIGINT       NOT NULL,
    timestamp_bucket  INT          NOT NULL,  -- Unix epoch truncated to 60s
    median_speed_kmh  FLOAT        NOT NULL,
    sample_count      SMALLINT     NOT NULL,
    PRIMARY KEY (edge_id, timestamp_bucket)
);

-- Historical speed profile (used by ETA ML model)
CREATE TABLE historical_speed (
    edge_id           BIGINT   NOT NULL,
    day_of_week       TINYINT  NOT NULL,  -- 0=Sun...6=Sat
    hour_of_day       TINYINT  NOT NULL,
    speed_p50_kmh     FLOAT    NOT NULL,
    speed_p85_kmh     FLOAT    NOT NULL,
    sample_days       SMALLINT NOT NULL,
    PRIMARY KEY (edge_id, day_of_week, hour_of_day)
);

-- ─────────────────────────────────────────────
-- Tiles
-- ─────────────────────────────────────────────
-- Tiles are stored in GCS with path:
--   gs://tiles-prod/{style}/{zoom}/{x}/{y}.pbf
-- Metadata table in Bigtable (row key: zoom/x/y)
CREATE TABLE tile_metadata (
    tile_key          VARCHAR(64)  PRIMARY KEY,  -- "{zoom}/{x}/{y}"
    style_version     INT          NOT NULL,
    rendered_at       TIMESTAMP    NOT NULL,
    etag              CHAR(32)     NOT NULL,
    size_bytes        INT          NOT NULL,
    gcs_path          VARCHAR(256) NOT NULL
);

-- ─────────────────────────────────────────────
-- Places
-- ─────────────────────────────────────────────

CREATE TABLE place (
    place_id          BIGINT       PRIMARY KEY,
    google_place_id   VARCHAR(128) UNIQUE NOT NULL,
    name              VARCHAR(256) NOT NULL,
    lat               DOUBLE       NOT NULL,
    lon               DOUBLE       NOT NULL,
    s2_cell_id        BIGINT       NOT NULL,  -- S2 level-14 for geo queries
    category_id       INT          NOT NULL REFERENCES place_category(category_id),
    address_json      JSONB        NOT NULL,  -- {street, city, state, postal, country}
    phone             VARCHAR(32),
    website           VARCHAR(512),
    rating            NUMERIC(3,2),           -- 1.00–5.00
    review_count      INT          NOT NULL DEFAULT 0,
    price_level       TINYINT,                -- 1–4
    hours_json        JSONB,                  -- OpenHours spec
    attributes_json   JSONB,                  -- wheelchair_accessible, outdoor_seating...
    permanently_closed BOOLEAN     NOT NULL DEFAULT FALSE,
    created_at        TIMESTAMP    NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMP    NOT NULL DEFAULT NOW(),
    INDEX idx_s2 (s2_cell_id),
    INDEX idx_category (category_id)
);

CREATE TABLE place_photo (
    photo_id       BIGSERIAL    PRIMARY KEY,
    place_id       BIGINT       NOT NULL REFERENCES place(place_id),
    contributor_id BIGINT,
    gcs_uri        VARCHAR(512) NOT NULL,
    width_px       INT,
    height_px      INT,
    captured_at    TIMESTAMP,
    is_cover       BOOLEAN      NOT NULL DEFAULT FALSE,
    INDEX idx_place (place_id)
);

-- ─────────────────────────────────────────────
-- User Data
-- ─────────────────────────────────────────────

CREATE TABLE user_account (
    user_id        BIGINT       PRIMARY KEY,
    google_sub     VARCHAR(128) UNIQUE NOT NULL,  -- OIDC subject
    display_name   VARCHAR(128),
    email          VARCHAR(256),
    created_at     TIMESTAMP    NOT NULL DEFAULT NOW()
);

CREATE TABLE saved_place (
    id             BIGSERIAL    PRIMARY KEY,
    user_id        BIGINT       NOT NULL REFERENCES user_account(user_id),
    place_id       BIGINT       NOT NULL REFERENCES place(place_id),
    label          VARCHAR(64),  -- "Home", "Work", custom
    saved_at       TIMESTAMP    NOT NULL DEFAULT NOW(),
    UNIQUE (user_id, place_id)
);

CREATE TABLE location_history (
    id             BIGSERIAL    PRIMARY KEY,
    user_id        BIGINT       NOT NULL REFERENCES user_account(user_id),
    lat            DOUBLE       NOT NULL,
    lon            DOUBLE       NOT NULL,
    accuracy_m     FLOAT,
    recorded_at    TIMESTAMP    NOT NULL,
    activity_type  TINYINT,     -- 0=stationary,1=walking,2=driving,3=cycling
    INDEX idx_user_time (user_id, recorded_at)
    -- Partitioned by month for lifecycle management
);
```

### Database Choice

| Option | Pros | Cons | Use Case Fit |
|---|---|---|---|
| **PostgreSQL + PostGIS** | Mature geo extensions, rich SQL, ACID | Single-node throughput ceiling, vertical scaling only | Place data (medium scale) |
| **Google Spanner** | Globally distributed, strong consistency, SQL, horizontal scale | Higher latency vs. regional DB, expensive | Places, user data, tile metadata |
| **Bigtable** | Extreme write throughput (>10 M ops/s), low-latency key-value, time-series native | No secondary indexes, no ad-hoc SQL | Traffic speeds, tile metadata |
| **Custom Graph DB (in-house)** | Optimized for road-graph traversal, CH shortcuts, parallel edge expansion | Engineering cost, no community support | Road graph at Google scale |
| **Neo4j** | Native graph, Cypher query language | Write throughput bottleneck, no global distribution | Unsuitable for 2 B edge graph |
| **Apache Kafka** | High-throughput durable log, replayable, partitionable | Not a query store; needs downstream consumer | Probe ingestion event bus |
| **Google Cloud Storage (GCS)** | Infinite capacity, cheap cold storage, CDN-integrated | High latency for random reads | Tile blobs, Street View images |

**Selected Choices**:
- **Road graph**: Custom distributed graph store (partitioned by S2 cell ID) — the 2 B edge graph with CH shortcuts demands purpose-built traversal, not a general-purpose DB. CH queries require hop-limited BFS over a condensed graph; general-purpose graph DBs add overhead without benefit.
- **Traffic speeds**: Bigtable — row key `edge_id#minute_bucket` gives O(1) point lookups and efficient range scans over a time window. Bigtable's LSM-tree write path handles the 6 M probe/s fan-out after Flink aggregation.
- **Places / User data**: Cloud Spanner — the Place table requires geo-indexed range queries and strong consistency for review counts. Spanner's TrueTime API enables external consistency across regions without 2PC overhead.
- **Tile blobs**: GCS + CDN — tiles are immutable content-addressed objects. GCS provides 11 nines durability and integrates natively with Cloud CDN.
- **Probe bus**: Kafka (self-hosted on GKE or Cloud Pub/Sub) — durable, replayable, partitioned by geohash for locality-preserving aggregation.

---

## 5. API Design

All endpoints are served over HTTPS. Auth is via Bearer JWT (for end-users) or API key (for Maps Platform developers). Rate limits are enforced at API Gateway level.

```
Base URL: https://maps.googleapis.com/maps/api/v1/

──────────────────────────────────────────────────────────────────
1. Tiles
──────────────────────────────────────────────────────────────────

GET /tiles/{style}/{zoom}/{x}/{y}
  Auth:       None (public) or API key for usage tracking
  Headers:    If-None-Match: <etag>
  Response:   200 application/octet-stream (pbf or png)
              304 Not Modified
              Cache-Control: public, max-age=86400, immutable
  Rate limit: 10 000 req/s per API key
  Notes:      x and y are tile coordinates in Web Mercator (XYZ scheme).
              style ∈ {roadmap, satellite, terrain, hybrid}.

──────────────────────────────────────────────────────────────────
2. Directions / Routing
──────────────────────────────────────────────────────────────────

GET /directions
  Auth:       Required (JWT or API key)
  Params:
    origin          string  required  "lat,lon" or place_id
    destination     string  required  "lat,lon" or place_id
    waypoints       string  optional  pipe-separated "lat,lon" list (max 25)
    mode            enum    optional  driving|walking|bicycling|transit (default: driving)
    departure_time  int     optional  Unix timestamp; "now" for live traffic
    avoid           string  optional  tolls|highways|ferries
    alternatives    bool    optional  Return up to 3 alternative routes (default: false)
    language        string  optional  BCP-47 language code for instruction text
  Response 200:
    {
      "status": "OK",
      "routes": [
        {
          "summary": "I-80 W",
          "distance": { "text": "45.2 mi", "value": 72765 },
          "duration": { "text": "52 mins", "value": 3120 },
          "duration_in_traffic": { "text": "1 hr 4 mins", "value": 3840 },
          "polyline": { "encoded": "_p~iF~ps|U_ulLnnqC_mqNvxq`@" },
          "legs": [ { ... } ],
          "warnings": [],
          "copyrights": "Map data ©2024 Google"
        }
      ]
    }
  Rate limit: 500 req/s per key; 50 000 req/day free tier
  Pagination:  N/A (all routes in single response)

──────────────────────────────────────────────────────────────────
3. Probe Upload (Navigation GPS)
──────────────────────────────────────────────────────────────────

POST /probes
  Auth:       Required (JWT, navigation session token)
  Body (JSON):
    {
      "session_id": "uuid",
      "probes": [
        { "lat": 37.7749, "lon": -122.4194, "speed_kmh": 48.2,
          "heading": 270, "accuracy_m": 4.5, "ts": 1712000000 }
      ]
    }
  Response 204: No Content (ack)
  Rate limit: 1 req/s per session (batching encouraged; max 20 probes/batch)
  Idempotency: Duplicate probes (same session_id + ts) deduplicated in Kafka consumer.

──────────────────────────────────────────────────────────────────
4. ETA
──────────────────────────────────────────────────────────────────

GET /eta
  Auth:       Required
  Params:
    origin      string  required
    destination string  required
    mode        enum    optional  driving|walking (default: driving)
    route_id    string  optional  Previously computed route ID for in-navigation ETA refresh
  Response 200:
    {
      "eta_seconds": 3840,
      "eta_seconds_p90": 4200,
      "arrival_time": "2024-04-01T15:42:00Z",
      "traffic_condition": "heavy"  // light|moderate|heavy
    }
  Rate limit: 10 req/s per session

──────────────────────────────────────────────────────────────────
5. Places Search
──────────────────────────────────────────────────────────────────

GET /places/search
  Auth:       Required
  Params:
    query           string  required  Free-text search ("coffee near me")
    location        string  optional  "lat,lon" — center for geo bias
    radius_m        int     optional  Search radius in meters (max 50 000)
    type            string  optional  Place type filter (restaurant|gas_station|hospital...)
    open_now        bool    optional  Filter to currently open places
    min_rating      float   optional  Minimum average rating (1.0–5.0)
    max_results     int     optional  1–20 (default 10)
    page_token      string  optional  Pagination token from previous response
    language        string  optional  BCP-47
  Response 200:
    {
      "places": [ { "place_id": "...", "name": "...", "lat": ..., "lon": ...,
                    "rating": 4.3, "review_count": 1204, "types": [...],
                    "opening_hours": { "open_now": true }, "distance_m": 340 } ],
      "next_page_token": "eyJv...",
      "attribution": "Powered by Google"
    }
  Rate limit: 100 req/s per key; cursor-based pagination (next_page_token valid 60 s)

──────────────────────────────────────────────────────────────────
6. Place Details
──────────────────────────────────────────────────────────────────

GET /places/{place_id}
  Auth:       Required
  Params:
    fields  string  optional  Comma-separated field mask (name,rating,hours,photos,reviews)
    language string optional
  Response 200: Full place object with requested fields.
  Rate limit: 100 req/s per key

──────────────────────────────────────────────────────────────────
7. Autocomplete
──────────────────────────────────────────────────────────────────

GET /places/autocomplete
  Auth:       Required
  Params:
    input       string  required  Partial text typed by user
    location    string  optional  "lat,lon" for geo bias
    radius_m    int     optional
    session_token string optional UUID grouping autocomplete + detail calls for billing
    types       string  optional  geocode|address|establishment|(cities)
  Response 200:
    {
      "predictions": [
        { "place_id": "...", "description": "Starbucks, Market St, San Francisco, CA",
          "matched_substrings": [{"offset":0,"length":9}],
          "structured_formatting": { "main_text": "Starbucks", "secondary_text": "Market St..." } }
      ]
    }
  Rate limit: 300 req/s per key

──────────────────────────────────────────────────────────────────
8. Street View Metadata + Tile
──────────────────────────────────────────────────────────────────

GET /streetview/metadata
  Params: location="lat,lon" OR pano_id
  Response: { "pano_id": "...", "lat": ..., "lon": ..., "date": "2023-08" }

GET /streetview/tile/{pano_id}/{zoom}/{x}/{y}
  Response: 512×512 JPEG tile
  Cache-Control: public, max-age=2592000, immutable
```

---

## 6. Deep Dive: Core Components

### 6.1 Routing Engine (Graph Traversal + Contraction Hierarchies)

**Problem it solves**: Given a road network of ~2 B directed edges (globally), compute the shortest-time path between two arbitrary points in under 500 ms at p99. Dijkstra on a raw graph of this size would be prohibitively slow (O((V+E) log V) ≈ seconds). The routing engine must also incorporate real-time traffic weights.

**Approaches Comparison**:

| Approach | Preprocessing | Query Time | Memory | Traffic Support | Notes |
|---|---|---|---|---|---|
| Dijkstra (vanilla) | None | O((V+E) log V) ≈ minutes | Low | Easy reweight | Impractical at global scale |
| A* with heuristic | None | 10×–100× faster than Dijkstra | Low | Easy reweight | Still slow cross-country |
| Bidirectional Dijkstra | None | ~√V vs Dijkstra | Low | Moderate | Better but not enough |
| Contraction Hierarchies (CH) | Heavy (hours, offline) | Microseconds–milliseconds | High (shortcuts) | Requires overlay graph | State of the art for static graphs |
| CH + Customizable Route Planning (CRP) | Metric-independent preprocessing | Sub-millisecond | Moderate | Excellent (reweight only metric) | Used by Bing Maps, here.com |
| Hub Labeling (HL) | Extreme preprocessing | Fastest queries (nanoseconds) | Very high (O(V√V) labels) | Poor (full recompute on change) | Research; Waze uses variant |
| OSRM (CH-based, open source) | Hours | 1–5 ms | Moderate | via traffic overlay | Production-proven |

**Selected Approach**: Contraction Hierarchies (CH) with a Customizable Route Planning (CRP) overlay for traffic.

**Detailed Reasoning**:
- CH preprocessing contracts nodes in order of importance, inserting "shortcuts" that bypass less important nodes. The augmented graph is ~3× larger than the original but supports bidirectional Dijkstra that only explores a tiny "meeting point" near the top of the hierarchy.
- CRP separates the graph into two layers: a metric-independent overlay (topology, edge sequences) and a metric (travel times). When traffic changes, only the metric is recomputed for affected cells — not the full preprocessing. This allows sub-minute traffic incorporation.
- Hub Labeling is faster for query time but the O(V√V) label storage (hundreds of GB) and full recomputation on traffic changes make it unsuitable for live traffic.
- A* alone is insufficient because the heuristic (Euclidean distance) is too loose for road networks with highways that curve away from destinations.

**Implementation Detail (CH Query)**:

```python
def ch_bidirectional_dijkstra(graph, ch_graph, source, target, traffic_weights):
    """
    CH bidirectional Dijkstra.
    graph: original road edges
    ch_graph: CH-augmented graph with shortcuts
    traffic_weights: dict edge_id -> current_travel_secs (from Traffic Layer)
    """
    # Forward search from source (only go UP the node importance hierarchy)
    forward_dist = {source: 0}
    forward_prev = {}
    forward_pq = [(0, source)]

    # Backward search from target (only go UP the hierarchy)
    backward_dist = {target: 0}
    backward_prev = {}
    backward_pq = [(0, target)]

    best = float('inf')
    meeting_node = None

    while forward_pq or backward_pq:
        # Alternating step — process whichever queue has smaller tentative dist
        if forward_pq and (not backward_pq or forward_pq[0][0] <= backward_pq[0][0]):
            d, u = heappop(forward_pq)
            if d > forward_dist.get(u, float('inf')):
                continue
            # PRUNING: if d >= best, we can stop forward search
            if d >= best:
                break
            for v, edge_id, base_weight in ch_graph.upward_edges(u):
                # Apply traffic weight if edge is in live traffic overlay
                w = traffic_weights.get(edge_id, base_weight)
                nd = d + w
                if nd < forward_dist.get(v, float('inf')):
                    forward_dist[v] = nd
                    forward_prev[v] = (u, edge_id)
                    heappush(forward_pq, (nd, v))
                    # Check if v is already settled in backward search
                    if v in backward_dist:
                        cand = nd + backward_dist[v]
                        if cand < best:
                            best = cand
                            meeting_node = v
        else:
            # Symmetric backward step
            d, u = heappop(backward_pq)
            if d > backward_dist.get(u, float('inf')):
                continue
            if d >= best:
                break
            for v, edge_id, base_weight in ch_graph.upward_edges_reversed(u):
                w = traffic_weights.get(edge_id, base_weight)
                nd = d + w
                if nd < backward_dist.get(v, float('inf')):
                    backward_dist[v] = nd
                    backward_prev[v] = (u, edge_id)
                    heappush(backward_pq, (nd, v))
                    if v in forward_dist:
                        cand = forward_dist[v] + nd
                        if cand < best:
                            best = cand
                            meeting_node = v

    if meeting_node is None:
        return None  # No path found

    # Unpack shortcut edges to recover original path
    path = unpack_ch_path(forward_prev, backward_prev, meeting_node, ch_graph)
    return path, best


def unpack_ch_path(forward_prev, backward_prev, meeting, ch_graph):
    """Recursively expand CH shortcuts back to original edges."""
    # Forward half: source -> meeting
    fwd_edges = []
    node = meeting
    while node in forward_prev:
        parent, edge_id = forward_prev[node]
        if ch_graph.is_shortcut(edge_id):
            fwd_edges.extend(ch_graph.expand_shortcut(edge_id))
        else:
            fwd_edges.append(edge_id)
        node = parent
    fwd_edges.reverse()

    # Backward half: meeting -> target
    bwd_edges = []
    node = meeting
    while node in backward_prev:
        parent, edge_id = backward_prev[node]
        if ch_graph.is_shortcut(edge_id):
            bwd_edges.extend(ch_graph.expand_shortcut(edge_id))
        else:
            bwd_edges.append(edge_id)
        node = parent

    return fwd_edges + bwd_edges
```

**Interviewer Q&As**:

1. **Q: Why does CH only traverse "upward" edges during query?**
   A: During preprocessing, nodes are ordered by importance (contracted last = highest importance). The key CH invariant is that any shortest path passes through a node of higher importance. Restricting the bidirectional search to upward edges (toward higher importance) guarantees we find the optimal path through the "highest" meeting point, while dramatically shrinking the search space from millions of nodes to a few thousand.

2. **Q: How do you handle real-time traffic with a static CH preprocessing?**
   A: The CH topology (which nodes to contract, which shortcuts to add) is traffic-independent. Only edge weights change. In CRP, the graph is partitioned into cells; when traffic changes in a cell, only that cell's "customization" step re-runs (seconds vs. hours for full preprocessing). The overlay graph between cells uses precomputed boundary-node distances updated per-cell.

3. **Q: How would you route across ocean ferry crossings or with transit legs?**
   A: Multi-modal routing uses a layered graph. Each modal graph (road, ferry, rail, bus) has its own CH. The connection graph links departure/arrival nodes with scheduled departure times (RAPTOR algorithm for transit). The router finds the Pareto-optimal front of time vs. cost vs. transfers.

4. **Q: What happens when a road is closed (accident, construction) in real time?**
   A: The Traffic Aggregation Service monitors probe data; if speed on a segment drops near zero with many corroborating probes, it flags it as incident. The Routing Service marks that edge's weight to +∞ (impassable) in its local traffic weight table. CH shortcuts crossing that edge are also invalidated in the customization step for that cell. In-navigation clients receive a push event ("incident ahead") and the next reroute request reflects the closure.

5. **Q: How do you parallelize routing for a batch of 10 000 ride-share matches?**
   A: For many-to-many routing (e.g., finding the nearest N drivers to M riders), Hub Labeling is the right structure despite its cost: with precomputed labels, a single distance lookup is O(hub_size) ≈ O(√V). For the fleet-matching use case, labels are computed once offline and all pairwise distances are O(1) lookups. Alternatively, partition the fleet by geohash cell and run parallel CH queries, one per region shard.

---

### 6.2 Real-Time Traffic Ingestion & Aggregation

**Problem it solves**: Receive GPS probes from up to 6 M concurrent navigation sessions, map each probe to a road segment, compute a current speed estimate per segment, and make it available to the Routing Service within 60 s of road conditions changing.

**Approaches Comparison**:

| Approach | Throughput | Latency | Correctness | Fault Tolerance |
|---|---|---|---|---|
| Direct DB write per probe | Low (DB bottleneck) | Low | High | Poor |
| Kafka → batch Spark job | Very High | High (minutes) | High | Good |
| Kafka → Flink streaming | Very High | Low (seconds) | High | Excellent (checkpointing) |
| Kafka → Redis in-memory agg | High | Very low | Medium (no replay) | Poor (Redis restart = data loss) |
| Kafka → Flink → Bigtable | Very High | Low | High | Excellent |

**Selected Approach**: Kafka → Apache Flink → Bigtable.

**Detailed Reasoning**:
- Kafka provides durable, replayable, partitioned log. Partitioning by S2 cell keeps geographically nearby probes in the same partition, enabling Flink to efficiently join probes to road segments without cross-partition shuffles.
- Flink's stateful streaming with event-time windows provides exactly-once semantics via Flink checkpoints and Kafka transactional producers. A 30 s tumbling window per segment computes median speed.
- Bigtable is the ideal sink: its LSM write path handles bursts; row key `edge_id#minute_bucket` makes reads by the Routing Service O(1).
- Redis in-memory aggregation would be faster but loses all state on restart and cannot replay historical probes for debugging or backfill.

**Implementation Detail (Flink Job)**:

```java
// Flink Streaming Job: Probe -> Traffic Speed
DataStream<ProbeEvent> probeStream = env
    .addSource(new FlinkKafkaConsumer<>("probes", new ProbeDeserializer(), kafkaProps))
    .assignTimestampsAndWatermarks(
        WatermarkStrategy.<ProbeEvent>forBoundedOutOfOrderness(Duration.ofSeconds(10))
            .withTimestampAssigner((e, ts) -> e.getTimestampMs())
    );

// Map-match: assign each probe to the nearest road edge
DataStream<MatchedProbe> matched = probeStream
    .map(new MapMatchingFunction(roadSegmentIndex))  // spatial lookup via S2
    .filter(p -> p.getMatchConfidence() > 0.7);       // discard low-confidence matches

// Key by edge_id, tumbling 30s window
DataStream<TrafficSpeed> trafficSpeeds = matched
    .keyBy(MatchedProbe::getEdgeId)
    .window(TumblingEventTimeWindows.of(Time.seconds(30)))
    .aggregate(new MedianSpeedAggregator(), new TrafficSpeedWindowFunction());

// Sink to Bigtable
trafficSpeeds.addSink(new BigtableSink(bigtableConfig));


// MedianSpeedAggregator accumulates speeds in a sorted structure
class MedianSpeedAggregator implements AggregateFunction<MatchedProbe, TreeMultiset<Float>, Float> {
    public TreeMultiset<Float> createAccumulator() { return TreeMultiset.create(); }
    public TreeMultiset<Float> add(MatchedProbe p, TreeMultiset<Float> acc) {
        acc.add(p.getSpeedKmh());
        return acc;
    }
    public Float getResult(TreeMultiset<Float> acc) {
        if (acc.isEmpty()) return null;
        int mid = acc.size() / 2;
        return Iterables.get(acc, mid);  // approximate median
    }
    public TreeMultiset<Float> merge(TreeMultiset<Float> a, TreeMultiset<Float> b) {
        a.addAll(b); return a;
    }
}

// MapMatchingFunction performs nearest-edge lookup using S2 spatial index
class MapMatchingFunction extends RichMapFunction<ProbeEvent, MatchedProbe> {
    private S2EdgeIndex edgeIndex;  // loaded from road graph at startup
    public void open(Configuration cfg) {
        edgeIndex = RoadGraphLoader.loadS2Index();  // cell-indexed edge lookup
    }
    public MatchedProbe map(ProbeEvent p) {
        S2LatLng point = S2LatLng.fromDegrees(p.getLat(), p.getLon());
        NearestEdgeResult result = edgeIndex.nearestEdge(point, MAX_SNAP_DISTANCE_M);
        if (result == null) return MatchedProbe.unmatched(p);
        return new MatchedProbe(p, result.edgeId, result.projectedPoint, result.confidence);
    }
}
```

**Interviewer Q&As**:

1. **Q: How do you handle out-of-order probe events from mobile devices (network buffering, sleep mode)?**
   A: Flink watermarks with a bounded out-of-orderness of 10 s tolerate late arrivals. Events arriving more than 10 s late fall in a side output "late data" stream that can be applied to a corrected window if it materially changes the median. For dead-reckoning during GPS gaps, the client interpolates probes locally and submits them on reconnection.

2. **Q: What is map matching and why is it necessary?**
   A: Raw GPS has ±5–15 m accuracy. Without map matching, you cannot reliably assign a probe to a specific road edge (e.g., a point between two parallel one-way streets). Map matching projects the GPS coordinate onto the nearest road edge using the S2 spatial index, then uses a Hidden Markov Model (HMM) that considers the sequence of probes: consecutive probes must follow a connected path in the road graph, dramatically improving accuracy.

3. **Q: How do you prevent a single congested road from being overwhelmed by probe writes?**
   A: Flink's key-by(edge_id) distributes processing. For a very hot edge (e.g., a major highway with 10 K vehicles), the Flink subtask handling that key processes all probes in memory (fast); the write to Bigtable is a single row mutation per 30 s window regardless of probe count. The Bigtable row key spreads hot edges across tablet servers via consistent hashing.

4. **Q: How do you distinguish a stopped vehicle from a slow road?**
   A: The Flink aggregator computes median speed from multiple independent probes. If 10 probes on a segment all report 0–5 km/h, that's genuine congestion. If a single probe reports 0 km/h and others report 50 km/h, the median is ~50 km/h and the stopped vehicle is treated as an outlier. A separate "incident detection" model counts the ratio of near-zero probes to all probes; above 70% triggers an incident flag.

5. **Q: What is the end-to-end latency budget for a traffic update reaching a driver?**
   A: Probe collected at t=0 → Kafka ingestion ≤ 100 ms → Flink window fires at t+30 s → Bigtable write ≤ 200 ms → Routing Service cache TTL 30 s (reads fresh on expiry) → driver's next reroute check ≤ 60 s. Total worst case: ~60 s. Average: ~35 s. This satisfies the 60 s freshness SLA.

---

### 6.3 Map Tile Serving & Update Pipeline

**Problem it solves**: Render the entire world map across 23 zoom levels into tile images, serve them to billions of clients with sub-100 ms latency, and propagate map data changes (road edits, new buildings, satellite imagery updates) to production within an SLA.

**Approaches Comparison**:

| Approach | Rendering | Freshness | Scalability | Cost |
|---|---|---|---|---|
| On-demand raster render | Always fresh | Instant | Poor (CPU-bound per request) | Very high |
| Pre-rendered raster tiles | Fast serve | Stale until batch re-render | Excellent (static files) | Medium (storage) |
| Pre-rendered vector tiles (PBF) | Client renders | Near-instant rerender on data change | Excellent | Low (smaller payloads) |
| Hybrid: vector tiles + server-side raster snapshot | Best of both | Good | Excellent | Medium |

**Selected Approach**: Pre-rendered vector tiles (PBF) stored in GCS, served via CDN. Raster tiles generated on demand for low-capability clients or special styles (satellite overlay).

**Detailed Reasoning**: Vector tiles (Mapbox Vector Tile / PBF format) are 40–60% smaller than equivalent raster tiles. The client GPU renders them, offloading CPU from servers. Style changes (color themes, language) require only a CSS-like style sheet update, not full tile re-render. For the update pipeline, only changed geographic cells need re-rendering when map data is updated.

**Implementation Detail (Tile Update Pipeline)**:

```python
# Simplified tile update pipeline (runs as a batch DAG on Google Cloud Dataflow)

class TileUpdatePipeline:
    """
    Triggered whenever the map data store receives a significant diff.
    Computes the set of affected tile keys and re-renders only those.
    """

    def run(self, change_set: ChangeSet):
        """
        change_set: list of (geometry, change_type) — e.g., a road edit in San Francisco.
        """
        # 1. Compute affected tile keys across all zoom levels
        affected_tiles = set()
        for geom, _ in change_set.changes:
            bbox = geom.bounding_box().expand(TILE_BUFFER_M)
            for zoom in range(0, 23):
                tiles = bbox_to_tiles(bbox, zoom)  # XYZ tile coords covering bbox
                affected_tiles.update(tiles)

        # 2. For each affected tile, fetch source data and re-render
        with beam.Pipeline() as p:
            (
                p
                | "Create tile keys" >> beam.Create(list(affected_tiles))
                | "Fetch source layers" >> beam.Map(self.fetch_source_data)
                | "Render to PBF" >> beam.Map(self.render_tile)
                | "Upload to GCS" >> beam.Map(self.upload_and_invalidate_cdn)
            )

    def fetch_source_data(self, tile_key):
        zoom, x, y = tile_key
        bbox = tile_to_bbox(zoom, x, y)
        data = {
            "roads": road_db.query_within_bbox(bbox, zoom),
            "buildings": building_db.query_within_bbox(bbox, zoom),
            "water": water_db.query_within_bbox(bbox, zoom),
            "labels": label_db.query_within_bbox(bbox, zoom, language="mul"),
            "pois": poi_db.query_within_bbox(bbox, zoom, min_importance=zoom_to_min_poi_rank(zoom)),
        }
        return tile_key, data

    def render_tile(self, tile_key_data):
        tile_key, data = tile_key_data
        zoom, x, y = tile_key
        # Use Mapnik (C++) or Vtzero for PBF encoding
        encoder = VectorTileEncoder(extent=4096)
        for layer_name, features in data.items():
            layer = encoder.add_layer(layer_name)
            for feature in features:
                layer.add_feature(
                    geometry=feature.geometry,
                    properties=feature.properties_for_zoom(zoom)
                )
        pbf_bytes = encoder.encode()
        return tile_key, pbf_bytes

    def upload_and_invalidate_cdn(self, tile_key_pbf):
        tile_key, pbf_bytes = tile_key_pbf
        zoom, x, y = tile_key
        gcs_path = f"gs://tiles-prod/roadmap/{zoom}/{x}/{y}.pbf"
        new_etag = hashlib.md5(pbf_bytes).hexdigest()
        gcs_client.upload(gcs_path, pbf_bytes, content_type="application/x-protobuf",
                          metadata={"Cache-Control": "public, max-age=86400"})
        # Purge CDN cache for this tile path
        cdn_client.invalidate(f"/tiles/roadmap/{zoom}/{x}/{y}")
        tile_metadata_db.upsert(tile_key, etag=new_etag, rendered_at=now())
```

**Interviewer Q&As**:

1. **Q: How many tiles need re-rendering when a city block of roads is added?**
   A: A typical city block ≈ 100 m × 100 m. At zoom 17 (1 tile ≈ 480 m), it touches ~1 tile. But changes must propagate through all zoom levels. At zoom 0, the whole world is 1 tile. For zooms 0–10, changes to a single block may share a tile with huge areas and may not be visible anyway. Practically, zooms 12–17 are the affected range: ~1–4 tiles per zoom × 6 zoom levels = ~10–25 tiles. With a block-level change affecting a neighborhood, ~1 000 tiles total. The pipeline handles this in minutes.

2. **Q: How do you serve tiles in multiple languages (English, Japanese, Arabic)?**
   A: Vector tiles store label text in a neutral form (place names in all supported scripts as separate properties). The client's style sheet selects the language layer. For raster tiles, language-specific renders are pre-generated. The most common 10 languages are pre-rendered; other languages are rendered on demand and cached.

3. **Q: What is the CDN cache invalidation strategy when a tile is updated?**
   A: Tiles use content-addressed GCS paths (incorporating a version hash or using ETag). Old tiles remain in CDN until TTL expires (24 h for stable tiles). For urgent changes (e.g., post-disaster road closures), the pipeline sends explicit `PURGE` calls to each CDN PoP for the updated tile URLs. This costs money per invalidation, so it is reserved for high-priority updates.

4. **Q: How do you keep the tile store consistent when a render job partially fails mid-update?**
   A: The pipeline writes to a staging GCS prefix first. Once all tiles in the change set have been written and verified (MD5 checksum), a metadata pointer is atomically updated to the new version. The serving layer reads the pointer and serves from the new prefix. Partial failures leave the old prefix intact; only a full successful batch atomically switches the serving version.

5. **Q: Why use vector tiles instead of raster tiles?**
   A: Vector tiles are 40–60% smaller (8 KB vs. 15 KB average), reducing bandwidth and CDN costs significantly at 700 K tile RPS. Client-side GPU rendering of vector tiles enables smooth sub-pixel zoom animation, arbitrary style changes, and 3D tilting — impossible with pre-rasterized images. The trade-off is higher client CPU/GPU usage, which is acceptable on modern smartphones.

---

## 7. Scaling

### Horizontal Scaling

- **Tile Serving**: Stateless HTTP services behind GFE. Auto-scale based on RPS; target CPU utilization 60%. Each instance reads tiles from GCS/CDN — no shared state.
- **Routing Service**: Stateless query handlers. Road graph is sharded by S2 cell; each shard cached in memory on routing nodes. Route queries that cross shards fan out to multiple nodes and merge results. Scale to 17 K RPS with ~500 routing instances, each handling ~34 RPS with 400 ms average query time → 14 parallel queries per instance, well within CPU budget.
- **Flink Traffic Aggregation**: Scale Flink task managers horizontally. Kafka partition count sets the parallelism ceiling; target 600 partitions for 6 M probe RPS → 10 K probes/partition/s, feasible per Flink task slot.
- **Places Search**: Stateless. The geo index (S2 cells) is sharded by geographic region. Autocomplete FST is replicated in-memory on all instances (500 MB per instance).

### DB Sharding

- **Bigtable (Traffic)**: Automatically sharded by row key range. Pre-split tablet boundaries by edge_id range to prevent hotspots. Edge IDs assigned to avoid sequential hotspots (use consistent hash of OSM way ID).
- **Spanner (Places)**: Spanner interleaved tables for Place + PlacePhoto. Range-based splits on `s2_cell_id` keep nearby places on the same split for efficient geo range queries.
- **Road Graph**: Custom sharding by S2 level-6 cell (each cell ~250 km × 250 km). Cross-cell edge lookups use a cross-shard index of boundary nodes.

### Replication

- All databases run with 3-region replication (multi-region Spanner instance, Bigtable cluster replication). Write quorum = 2; read can serve from any replica.
- Kafka: replication factor 3, min-insync-replicas 2. Producer acks = all.
- GCS: multi-region bucket for tiles (US, EU, APAC), 11 nines durability via erasure coding.

### Caching

| Cache Layer | Technology | TTL | What's Cached |
|---|---|---|---|
| CDN edge cache | Cloud CDN / Fastly | 24 h (tiles), 30 d (Street View) | Tile PBF/PNG blobs, SV tile JPEGs |
| Routing in-memory | Process-local LRU | 30 s | Traffic weight table per road cell |
| Places in-memory | Redis cluster | 5 min | Place detail objects, search result pages |
| Autocomplete FST | In-process memory | Reload on deploy | Prefix trie for autocomplete candidates |
| Route cache | Redis | 30 s | Identical (origin, dest, mode) route responses |

### CDN

- ~130 Google PoPs worldwide. Tiles served from nearest PoP. Cache hit ratio target 95%+.
- For the 5% miss rate: 694 K RPS × 5% = 34 700 RPS to origin tile servers → 35 K tile-serving pods globally can handle this easily.
- Street View tiles cached at edge 30 days; panoramas served from the nearest regional GCS bucket.

**Interviewer Q&As**:

1. **Q: The road graph is 240 GB. How do you fit it in memory for fast routing?**
   A: The CH-augmented graph is partitioned into S2 level-6 cells (~1 000 cells globally). Each routing server loads only the cells covering its geographic responsibility plus one ring of neighbor cells for cross-border routes. A single cell averages 240 GB / 1 000 = 240 MB. A routing instance with 16 GB RAM can hold ~65 cells comfortably. Cross-cell routes are handled by an aggregation layer that fans out to multiple cell-local servers and stitches paths.

2. **Q: How do you scale the probe ingestion pipeline to handle a 5× traffic spike (New Year's Eve)?**
   A: Kafka partitions are fixed at 600. We pre-scale Flink task managers to handle 5× baseline (30 M RPS) before the event. Kafka's durable log absorbs any burst above what Flink can process in real time; Flink catches up within minutes. The Probe Ingestion Gateway auto-scales stateless HTTP pods. Bigtable's write throughput scales linearly with nodes; we pre-provision additional nodes before predicted spikes.

3. **Q: How do you handle cold start for a new routing server that needs to load its road graph partition?**
   A: The road graph partitions are stored in GCS as snapshot files. A new routing server downloads its assigned partition at startup (~240 MB, takes ~5 s on 400 Mbps link). The server registers as "warming up" with the load balancer and receives read traffic only after it passes a health check that verifies graph correctness (test query with known answer). Total cold start: ~20 s.

4. **Q: How do you ensure routing latency doesn't degrade when 10 000 clients all request routes from the same city simultaneously (event ingress)?**
   A: Request coalescing: if 1 000 requests have identical (origin_cell, dest_cell, mode) within a 500 ms window, only one CH query runs; responses are fanned out to all waiting callers. Origin and destination are snapped to their nearest road nodes; if multiple users depart from the same node, the route is shared. This "route deduplication cache" reduces routing CPU by 10–20× during events.

5. **Q: How do you design for zero-downtime deployments of the routing service when you update the road graph?**
   A: Blue/green deployment at the cell partition level. New graph version is loaded into "blue" routing nodes while "green" nodes continue serving. Health checks verify the blue nodes answer a test suite of known-answer queries within SLA. Traffic is gradually shifted to blue (5% → 25% → 100%) with automatic rollback if error rate exceeds threshold. Graph version numbers are propagated to clients so they can invalidate cached routes computed against an old graph.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Mitigation | Recovery Time |
|---|---|---|---|
| Single CDN PoP outage | 1–2% of users experience slower tiles | GFE reroutes to next-nearest PoP automatically | <30 s |
| Routing Service region failure | Route requests fail for affected region users | Multi-region routing deployment; GFE fails over to alternate region | <60 s |
| Kafka broker failure | Probe ingestion pauses briefly | RF=3, min-ISR=2; producer retries with exponential backoff | <10 s |
| Flink job crash | Traffic freshness degrades | Flink restarts from last checkpoint (30 s interval); Kafka offset replay | <2 min |
| Bigtable node failure | Traffic read latency spike | Bigtable auto-rebalances; queries served from replica | <30 s |
| GCS regional outage | Tile origin misses fail | Multi-region bucket serves from alternate region; CDN continues serving cached tiles | Seamless if CDN TTL not expired |
| Road graph corruption | Routing returns wrong results | Checksummed graph snapshots; routing canary testing before full rollout | Rollback to previous snapshot <5 min |
| Map update pipeline failure | Map data becomes stale | Pipeline is idempotent; retry from last successful checkpoint; staleness alerted at 4 h | Hours (acceptable for batch updates) |
| Spanner region failure | Place data unavailable | Spanner multi-region; automatic failover to other regions | <10 s |

**Failover**: GFE performs health checks every 1 s. A region failing 3 consecutive checks is removed from the routing table. Traffic redistributes to remaining regions automatically.

**Retries & Idempotency**:
- Probe uploads are idempotent (deduplication by session_id + timestamp in Flink).
- Route requests are read-only — retries are safe.
- Tile render pipeline uses GCS conditional writes (`If-Match` ETag) to prevent double-write races.
- All Kafka producers use idempotent producer mode (sequence numbers) + transactional API to prevent duplicate commits to Bigtable.

**Circuit Breaker**: API Gateway implements a circuit breaker per downstream service. If Routing Service error rate exceeds 5% over 30 s, the breaker opens; callers receive a cached "degraded" route or an error with retry-after. The breaker half-opens after 10 s to probe recovery.

---

## 9. Monitoring & Observability

| Metric | Description | Alert Threshold |
|---|---|---|
| `tile_request_latency_p99` | 99th percentile tile fetch latency | >100 ms |
| `tile_cache_hit_rate` | CDN cache hit ratio | <90% |
| `route_latency_p99` | Route computation latency | >2 000 ms |
| `probe_ingest_lag_seconds` | Kafka consumer lag for probe topic | >60 s |
| `traffic_freshness_seconds` | Age of newest traffic data in Bigtable | >90 s |
| `routing_error_rate` | % of route requests returning non-200 | >0.1% |
| `flink_checkpoint_duration` | Time to complete Flink checkpoint | >60 s |
| `eta_mae_seconds` | Mean absolute error of ETA vs. actual (sampled) | >120 s |
| `place_search_latency_p99` | Place search latency | >200 ms |
| `tile_render_pipeline_lag` | Minutes since last successful tile render | >240 min |

**Distributed Tracing**: OpenTelemetry traces propagated via `traceparent` header across GFE → API Gateway → Routing Service → Traffic Layer Bigtable read. Traces sampled at 0.1% for normal operation, 100% on error. Stored in Cloud Trace; queryable for tail-latency investigation.

**Logging**: Structured JSON logs (Cloud Logging). Route requests log: user_id (hashed), origin_cell, dest_cell, mode, latency_ms, route_version. No raw GPS coordinates logged for privacy. Probe upload logs are aggregated (count per session, not individual coordinates) to comply with GDPR.

**Dashboards**: Four golden signals dashboards per service. Traffic pipeline dashboard: Kafka consumer lag, Flink throughput (probes/s processed), traffic update rate (segments/s updated), freshness heat map by geographic cell.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B (Chosen) | Reason |
|---|---|---|---|
| Tile format | Raster PNG | Vector PBF | 40–60% smaller; GPU rendering; style flexibility |
| Routing algorithm | Dijkstra + A* | Contraction Hierarchies | Millisecond queries vs. minutes; CH is state of the art |
| Traffic write path | Direct DB write | Kafka → Flink → Bigtable | Decoupling, durability, exactly-once aggregation |
| Places storage | MySQL | Cloud Spanner | Global distribution, strong consistency, SQL at scale |
| Map-match granularity | Per-segment | Per-lane (future) | Per-lane requires HD map; per-segment viable today |
| Tile rendering | On-demand | Pre-rendered + partial re-render | On-demand cannot serve 700 K RPS; pre-render + targeted invalidation balances freshness and throughput |
| Street View tier | Single tier | Hot/cold split (SSD/HDD) | 90% of traffic to 10% of panoramas; tiering saves 90% SSD cost |
| Probe sampling | All probes | 1-in-N sampling per segment | At very high density, median speed stabilizes; sampling reduces Kafka throughput proportionally |

---

## 11. Follow-up Interview Questions

1. **Q: How would you design offline maps (the ability to navigate without a network connection)?**
   A: The client downloads a regional tile pack (pre-rendered PBF tiles for zoom 0–17 for the region), a regional road graph snapshot, and ETA model parameters. Navigation runs fully on-device. Route computation uses the same CH algorithm on the downloaded graph. Traffic data is unavailable offline; ETA falls back to historical speed profiles. The download is compressed (~500 MB for a US state) and served from CDN.

2. **Q: How does Google Maps handle disputed territories (e.g., regions claimed by multiple countries)?**
   A: A "geopolitical layer" database maps IP country codes and locale settings to tile style variants. The border database stores multiple interpretations of each disputed boundary with the appropriate display variant per country. The tile rendering pipeline generates region-specific tile sets for the top disputed regions. The API Gateway detects the client's country (IP geolocation) and selects the appropriate tile set variant.

3. **Q: How would you design a system to detect new roads from GPS probe data alone (without human editors)?**
   A: Probes traveling on paths not in the road graph leave a "ghost track." An offline ML pipeline clusters ghost-track probes by trajectory using DBSCAN. Clusters with >N unique device IDs and >M days of activity are proposed as new road candidates. A review queue sends candidates to human mappers for confirmation. This approach powered Google's AI-based road detection in regions with poor OSM coverage.

4. **Q: What is your approach to A/B testing a new routing algorithm without breaking existing users?**
   A: A/B routing is implemented via a feature flag in the Routing Service. The experiment assigns users to control/treatment based on a hash of user_id. Treatment users receive routes from the new algorithm. Both groups' ETA accuracy (predicted vs. actual arrival), session completion rate, and user satisfaction (implicit: did they follow the route?) are tracked in the experimentation platform. Statistical significance gates rollout.

5. **Q: How do you prevent a rogue driver from faking slow speeds to mislead other drivers?**
   A: Multiple defense layers: (1) Minimum probe count threshold — a segment requires ≥ 3 independent device IDs to show congestion. (2) Speed sanity checks — probes reporting 0 km/h on a known highway at 3 AM are flagged as anomalous. (3) Device trust scoring — newly registered devices have lower weight until they accumulate consistent history. (4) Statistical outlier removal — the Flink aggregator uses Winsorization (cap extreme values at 5th/95th percentile) before median computation.

6. **Q: How would you scale Street View globally from 0 to 170 B panoramas?**
   A: Each capture car uploads panoramas to the nearest GCS regional bucket over HTTPS. A processing pipeline (Cloud Dataflow): (1) blur faces/plates for privacy, (2) stitch 360° panoramas from camera array, (3) georeference using GPS + IMU, (4) link adjacent panoramas by computing nearest neighbors via S2 spatial index, (5) write to the hot-tier object store. A coverage index (S2 cell → list of pano_ids) is maintained in Spanner. Older panoramas aged >3 years in low-traffic areas are tiered to cold storage.

7. **Q: How do you handle ETA accuracy for a route that passes through a tunnel (GPS loss)?**
   A: The routing system knows tunnel extents from the road graph (edge attribute `tunnel=true`). During navigation in a tunnel, the app uses dead reckoning (accelerometer + last known speed + heading) to estimate position. ETA is computed using the pre-assigned travel time for tunnel edges (no live traffic possible inside). The Flink map-matcher ignores probes reported inside tunnel bounding boxes (GPS noise). ETA confidence intervals widen during tunnel transit to reflect uncertainty.

8. **Q: What changes to the architecture if you need to support 10× more DAU (10 B)?**
   A: The main bottlenecks are: (1) probe ingestion — Kafka partition count scales to 6 000, Flink parallelism increases proportionally; (2) routing servers — the road graph is already partitioned; add more instances per cell; (3) tile CDN — already scales horizontally; (4) Spanner — increase compute and node count. The most significant change is the probe pipeline: 60 M RPS ingestion requires moving to a dedicated globally distributed streaming system and per-continent Kafka clusters with replication for cross-region traffic analysis.

9. **Q: How would you implement time-aware routing ("leave at 8 AM")?**
   A: Traffic speed profiles are stored per (edge, day_of_week, hour_of_day) in the historical_speed table. For a departure-time query, the router uses a time-expanded graph: edge weights are a function of the expected traversal start time (because traversing a congested highway at rush hour takes longer than at midnight). The CH algorithm is modified for time-dependent shortest paths (TDSP). The ETA model integrates historical + predicted live traffic to produce a final arrival time.

10. **Q: How do you test that your routing engine produces correct routes?**
    A: Three-layer testing strategy: (1) Unit tests: hand-curated graph fixtures with known shortest paths, testing algorithm correctness. (2) Regression tests: a corpus of 1 M real-world (origin, destination, expected route) tuples gathered from historical production traffic, re-run after every algorithm change. (3) Shadow testing: new algorithm runs in parallel with production on 1% of traffic; deviations from production routes trigger human review if the new route is significantly longer.

11. **Q: How do you handle the "last mile" problem — routing from the parking lot to the entrance of a large venue?**
    A: Pedestrian graph (footways, crosswalks, venue-internal paths) is a separate layer in the road graph. Venues with indoor mapping (airports, malls) contribute indoor graph segments. The route switches from the driving graph to the pedestrian graph at the parking lot node, stitched by a transition edge. For venues without indoor maps, the route terminates at the nearest routable outdoor node and a "walk to entrance" instruction is appended based on the place's designated entrance coordinate.

12. **Q: What are the privacy implications of storing user location history, and how do you handle them?**
    A: Location history is the most sensitive user data we store. Mitigations: (1) opt-in only — timeline is off by default; (2) on-device encryption — history is E2E encrypted with a user key on Android; (3) auto-deletion — default 18-month rolling window, configurable down to 3 months; (4) GDPR/CCPA right to erasure — a deletion request triggers a Dataflow job that shreds user data from all stores within 30 days; (5) aggregate traffic is computed from anonymized probes stripped of user_id before Kafka ingestion for non-navigation users.

13. **Q: How would you implement "avoid tolls" in the routing algorithm?**
    A: Each road edge has an `access_flags` bitmask and a `toll` boolean. The Routing Service accepts avoid preferences at query time. For "avoid tolls", toll edges are assigned a cost penalty large enough to make them non-competitive with free alternatives (e.g., add 10 000 s to traversal time). This turns a constraint into a soft penalty, keeping the algorithm standard Dijkstra/CH. Hard avoidance (never use toll roads) sets toll edge weight to +∞.

14. **Q: How is the road graph updated from OpenStreetMap diffs?**
    A: OSM publishes minutely diff files (osc.gz). A pipeline: (1) downloads diffs, (2) applies them to a local OSM mirror, (3) extracts changed ways/nodes, (4) rebuilds affected graph edges and nodes, (5) recomputes CH shortcuts for affected cells (incremental CH is an active research area; in practice, cells are re-contracted from scratch — takes minutes per cell), (6) writes the updated cell snapshot to GCS, (7) triggers routing servers to reload the affected cell.

15. **Q: How would you design Google Maps for a city with no existing digital map data?**
    A: Bootstrapping pipeline: (1) Satellite imagery from commercial providers gives road outlines — a road segmentation ML model (DeepLab variant) detects road pixels and generates candidate polylines. (2) GPS probe data from any existing devices (smartphones, delivery drivers) overlaid on the satellite imagery allows map-matching and road graph construction. (3) Crowd-sourced field collection (Local Guides program) verifies road names, turn restrictions, and points of interest. (4) Commercial data partnerships with local government agencies provide authoritative road databases where available. Initial tile quality is lower; it improves as coverage grows.

---

## 12. References & Further Reading

- Geisberger, R., Sanders, P., Schultes, D., Delling, D. (2008). "Contraction Hierarchies: Faster and Simpler Hierarchical Routing in Road Networks." *Lecture Notes in Computer Science*, 5038, 319–333.
- Delling, D., Goldberg, A., Pajor, T., Werneck, R. (2011). "Customizable Route Planning." *SEA 2011*, LNCS 6630, 376–387.
- Google Research Blog. (2023). "How Google Maps Predicts Traffic." https://ai.googleblog.com/2020/09/traffic-prediction-with-advanced-graph.html
- Google Cloud. "Cloud Spanner: TrueTime and External Consistency." https://cloud.google.com/spanner/docs/true-time-external-consistency
- Apache Flink Documentation. "Stateful Stream Processing." https://nightlies.apache.org/flink/flink-docs-stable/docs/concepts/stateful-stream-processing/
- Mapbox. "Vector Tile Specification." https://docs.mapbox.com/data/tilesets/guides/vector-tiles-standards/
- Google S2 Geometry Library. https://s2geometry.io/
- OSRM (Open Source Routing Machine) — CH-based open source router. http://project-osrm.org/
- OpenStreetMap Wiki. "Minutely Diffs." https://wiki.openstreetmap.org/wiki/Planet.osm/diffs
- Newson, P., Krumm, J. (2009). "Hidden Markov Map Matching Through Noise and Sparseness." *ACM SIGSPATIAL GIS 2009*.
- Bigtable Paper: Chang, F. et al. (2006). "Bigtable: A Distributed Storage System for Structured Data." *OSDI 2006*.
- Abraham, I. et al. (2012). "Hierarchical Hub Labelings for Shortest Paths." *ESA 2012*, LNCS 7501.
