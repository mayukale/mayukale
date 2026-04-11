# Problem-Specific Design — Maps & Location (12_maps_location)

## Google Maps

### Unique Functional Requirements
- Tile serving at 23 zoom levels (z0–z22), ~585 M unique tiles, 694K RPS peak
- Contraction Hierarchies routing for sub-2s continent-scale routes
- ETA prediction combining prep time (ML) + drive time (OSRM/CH) + traffic
- GPS probe ingestion at 6 M RPS → Flink 30 s sliding window → traffic layer update
- Street View: 170 B+ panoramas, ~340 PB total, 10% hot tier (34 PB)
- Hidden Markov Model (HMM) map-matching for noisy GPS traces to road network

### Unique Components / Services
- **GFE (Global Front End)**: ~130 PoPs, anycast BGP routing, TLS termination, DDoS protection
- **Tile Serving Service**: Reads from GCS/Bigtable, handles zoom-level translation, tile stitching for high-DPI, overlay composition
- **Routing Service**: Contraction Hierarchies (CH) + A* graph traversal; reads road graph from distributed graph store; incorporates current traffic layer; O(log N) query time
- **ETA Service**: Gradient-boosted trees + deep learning model; trained on historical GPS probes, road speeds, real-time traffic; reroute within ≤ 1 s of deviation detection
- **Traffic Aggregation Service (Flink)**: Consumes probe Kafka topic; computes median speed per road segment per 30 s bucket; writes to Bigtable `(segment_id, timestamp_bucket)` row key
- **Map Update Pipeline**: Offline batch (Cloud Dataflow); ingests satellite imagery, OSM diffs, manual edits; runs road-extraction ML; Mapnik/custom tile renderer; tile update pipeline: 1. Geohash buffering (~100 m × 100 m block) → 2. Affected tile computation (zoom 12-17, ~10-25 tiles per zoom) → 3. VectorTileEncoder PBF encoding → 4. GCS upload with cache headers → 5. CDN invalidation via PURGE calls → 6. Atomic metadata pointer swap
- **Probe Ingestion Gateway**: HTTP/2 or gRPC; 6 M RPS × 200 bytes = 1.2 GB/s

### Unique Data Model
- **Road nodes**: node_id, lat, lon, node_type (junction/endpoint/via), s2_cell_id (S2 level-14)
- **Road edges**: edge_id, source_node_id, target_node_id, road_class (0-7: motorway to footway), speed_limit_kmh, length_m, traversal_secs, oneway, toll, bridge, tunnel, access_flags (bitmask), geometry_wkb; 2 B edges × 120 bytes = ~240 GB
- **CH shortcuts**: shortcut_id, source_node_id, target_node_id, contracted_node, weight
- **Traffic speed** (Bigtable): `(segment_id, timestamp_bucket)` → median_speed_kmh, sample_count
- **Historical speed** (Bigtable): `(edge_id, day_of_week, hour_of_day)` → speed_p50_kmh, speed_p85_kmh
- **Tiles**: `gs://tiles-prod/{style}/{zoom}/{x}/{y}.pbf`; 13.5 TB base, 40.5 TB with 3× replication; raster avg 15 KB, vector avg 8 KB

### Unique Scalability / Design Decisions
- **CH preprocessing (offline) vs. on-the-fly routing**: CH preprocesses road graph by contracting low-importance nodes and adding shortcut edges; at query time, bidirectional A* searches only upward in the node hierarchy — O(log N) vs O(N) for Dijkstra; Apache Arrow serialization for 3 GB per-city graph for efficient transfer
- **HMM map-matching**: noisy GPS coordinates matched to road segments via Hidden Markov Model; emission probability = GPS accuracy; transition probability = road network distance; critical for building accurate traffic speed history
- **Time-dependent shortest paths (TDSP)**: departure-time-based routing uses historical speed patterns per (hour, day_of_week) per edge; Flink 7-day rolling aggregation
- **Probe batching**: mobile clients batch and compress GPS points; 6 M raw probes/s, not per-ping HTTP; dedicated ingest cluster with ring buffer for probe delivery

### Key Differentiator
Google Maps' uniqueness is its **full map rendering + routing + traffic pipeline at planetary scale**: Contraction Hierarchies for sub-2s continent-scale routing, HMM map-matching + Flink 30s window for live traffic, 694K RPS tile serving from 585M tiles across 130 CDN PoPs, and a complete offline map update pipeline (satellite → OSM diff → road extraction ML → Mapnik tile renderer → atomic CDN swap) — no other problem in this folder builds and serves the underlying map data itself.

---

## Proximity Service

### Unique Functional Requirements
- 500 M registered entities; 100 M DAU; 5 M concurrent real-time movers at peak
- Location update RPS: 1 M average, 3 M peak
- Radius search: p99 ≤ 100 ms for ≤10 km; p99 ≤ 200 ms for ≤50 km
- Entity types: business (static), driver/courier (real-time mover, 30 s auto-expire)
- Zero false negatives: no entity within radius should be missed

### Unique Components / Services
- **Search Service**: Converts (lat, lon, radius) to geohash cells; queries Redis inverted index; applies Haversine post-filter; optional two-stage ranking
- **Location Update Service**: Write-optimized; immediate Redis write + async Kafka publish; 3 M RPS write path
- **Geo Index Updater (Kafka consumer)**: Partitioned by geohash cell (200 partitions); moves entity from old geohash cell to new cell on cell boundary crossing; idempotent via timestamp check; SREM old cell + SADD new cell
- **Entity Registry Service**: Manages entity CRUD; synchronously updates geo index on registration; PostgreSQL + PostGIS source of truth
- **Anti-Hotspot Routing**: Dense urban areas (downtown NYC, Tokyo) may have millions of entities in a single P6 cell; sub-shard large cells into P7 precision (~150 m × 150 m); dispatch queries to sub-shards

### Unique Data Model
- **Redis geo index**: `gh6:{geohash6_cell}` → Set of entity_ids; 4 GB for 500 M entities at P6
- **Redis live location**: `loc:{entity_id}` → HASH {lat, lon, speed_mps, heading_deg, accuracy_m, updated_at} + 30 s TTL (auto-expire for offline movers)
- **Cassandra location history**: partition `(entity_id, day)`, clustering `recorded_at DESC`; 30-day TTL; 1 PB historical
- **PostgreSQL entity**: entity_id, entity_type ENUM(business/driver/courier), name, category, lat, lon, geohash6, geohash12, is_realtime, is_active, metadata_json

### Unique Scalability / Design Decisions
- **Two-tier write (immediate Redis + async Kafka)**: location updates are write-heavy (3 M RPS); writing synchronously to both Redis AND PostgreSQL would serialize on DB writes; Redis write is O(1) < 1 ms; Kafka async ensures geo index updater processes cell transitions without blocking the write path
- **30 s TTL for live entities**: drivers/couriers who go offline auto-expire from the live location store without a separate cleanup job; background driver liveness monitoring (app heartbeat) refreshes TTL
- **Geohash neighbor computation**: 8 adjacent cells (Moore neighborhood) guaranteed to cover any circle of radius < cell-width; pre-computed neighbor table in-process (no Haversine at index time); false positives (~50%) filtered at query time
- **Device trust scoring for fake speed detection**: submitted speed > 200 km/h or acceleration > 10 m/s² → flag as suspicious; prevents GPS spoofing from polluting entity positions

### Key Differentiator
Proximity Service's uniqueness is its **real-time mover architecture**: 5 M concurrent movers with 30 s TTL auto-expiry, two-tier write path (immediate Redis + async Kafka cell-transition updater), and anti-hotspot sub-sharding (P6→P7 for dense urban cells) — no other problem in this folder manages millions of continuously moving entities that must vanish from the index when offline.

---

## Yelp Nearby

### Unique Functional Requirements
- 10 M businesses, 500 M reviews, 10 B photos; 100 M DAU, 300 M MAU
- Full-text search with dietary filtering + geo radius + rating/review count ranking
- Two-stage ranking: ES function_score → optional ML rerank
- Review voting (useful/funny/cool), owner responses, Yelp Sort algorithm
- Photo service: S3 direct upload + Lambda resize to 3 sizes (150×150, 400×300, 1200×900)
- Recommendation engine: offline ALS (Spark) + online Feature Store

### Unique Components / Services
- **Two-Stage Ranking Pipeline**:
  - Stage 1 (ES `function_score`): BM25 on (name^3, description^1, category_titles^2) + gauss decay (location, scale=2km, decay=0.5, weight=2.0) + log1p(rating, factor=1.2, weight=1.5) + log1p(review_count, factor=0.01, weight=0.5) + log1p(popularity_score, weight=1.0); returns top 50; 20–40 ms; `score_mode=sum, boost_mode=multiply`
  - Stage 2 (in-process re-rank, optional): integrate cuisine affinity + dietary preference; 2 ms; fallback to ES score only if Feature Store unavailable
- **Rating Aggregator** (Kafka consumer): atomic incremental update: `new_avg = (old_avg × old_count + new_rating) / (old_count + 1)`; additive → order-independent → safe parallel consumption; nightly batch reconciliation from raw Cassandra to Aurora
- **Photo Service**: S3 presigned URL for direct client upload; Lambda triggered on upload → resizes to 3 sizes; metadata to `business_photos` table; CDN-fronted CDN URL in response
- **Search Indexer** (Kafka consumer): ES partial `_update` doc; retry 3× with backoff (100 ms, 200 ms, 400 ms) → dead-letter on failure; reconciliation job every 5 min

### Unique Data Model
- **ES mapping (restaurant_index)**: 10 primary shards + 1 replica; business_id (keyword), name (text + keyword, english analyzer), cuisine_types (keyword), dietary_flags (keyword), location (geo_point), aggregate_rating (float), review_count (integer), open_now_cache (boolean — updated by cron every 5 min), suggest (completion field for FST autocomplete)
- **Yelp Sort algorithm**: `score = elite_status_bonus + (useful_count × 0.4) + recency_decay(created_at) + length_bonus(text_body)`; ensures quality reviews rank above raw recency
- **UNIQUE(user_id, business_id)** constraint on reviews: one review per user per business
- **Cassandra check-ins**: (business_id) partition, checked_in_at DESC clustering; 5-year retention; 18 TB

### Unique Scalability / Design Decisions
- **`score_mode=sum, boost_mode=multiply`**: BM25 text relevance multiplied by sum of all geographic + rating function scores; ensures a restaurant irrelevant to query text never ranks at top just because it's nearby/highly-rated — text relevance gates the result
- **ES Completion Suggester (FST) for autocomplete**: Finite State Transducer loaded in JVM heap; sub-15 ms p99; geo-contexted (H3 resolution-6 cell) so autocomplete for "pizza" in NYC differs from LA; weights = 7-day rolling click count from review events
- **Lambda for photo resizing over synchronous**: photo resize is CPU-intensive (~500 ms per photo × 3 sizes); Lambda auto-scales to 23 RPS peak without provisioning persistent servers; synchronous would block upload API response
- **Offline ALS + Feature Store**: Spark ALS trains monthly on all user-business interactions; factor embeddings loaded into Feature Store (Redis); online serving blends ES score with ALS affinity for logged-in users; provides personalization without slowing the ranking API

### Key Differentiator
Yelp Nearby's uniqueness is its **multi-signal ES function_score with Yelp Sort**: combining BM25 text relevance, Gaussian distance decay, log-normalized rating/review count boosts, open-hours cache filtering, and Yelp's proprietary sort algorithm (elite bonus + usefulness + recency + length) — plus the full review ecosystem (voting, owner responses, photo service with Lambda resizing, ALS recommendation engine) that transforms a simple proximity search into a restaurant quality discovery platform.

---

## Geospatial Index (Reference Design)

### Unique Functional Requirements
- Algorithm reference design — not a deployed service; provides implementation guidance for spatial indexing
- Radius search: p99 ≤ 10 ms (1 km, in-memory); p99 ≤ 50 ms (50 km); zero false negatives
- K-NN: p99 ≤ 10 ms (K=10, in-memory)
- Point insert/update: p99 ≤ 1 ms (in-memory)
- Covers: Geohash inverted index, Quadtree, S2 Geometry, PostGIS R-tree as implementation options

### Algorithms Covered

**Geohash Inverted Index**:
- Binary interleaving of lat/lon bits (Z-order curve) → base-32 string
- Precision P: P5 = ±2.4 km (district), P6 = ±1.2 km (street, default), P7 = ±0.152 km (building), P9 = ±4.8 m
- Storage: 1 B entities × (12 B geohash12 + 8 B entity_id) = 20 GB
- Query: 9 cells × O(1) set lookup = O(9) = O(1)
- False positive rate: ~50% for P6 + 1 km radius

**Quadtree**:
- Recursive subdivision: split node when leaf.count > MAX_ENTITIES_PER_LEAF = 50
- Max depth: MAX_DEPTH = 20; leaf capacity: 50 entities
- Memory: 1 B entities → 27 M nodes × 52 B = ~1 GB
- Height for 1 B entities: log₄(1B/50) ≈ 12 levels
- K-NN: best-first traversal (min-heap by bbox distance); prune subtrees where bbox distance > K-th result so far; O(K log N) with branch-and-bound
- Split logic: 4 equal quadrants (NW, NE, SW, SE)

**S2 Geometry**:
- 6 cube faces subdivided recursively; 30 total levels; 2^30 cells per face
- 64-bit CellID: face (3 bits) + Hilbert curve position (58 bits) + level (3 bits)
- Cell cover: minimal set of S2 cells covering arbitrary polygon; min_level=10, max_level=14, max_cells=16 for circle covering
- No antimeridian discontinuity (spherical, no ±180° edge)
- Range query: B-tree scan on S2 cell ID range

**Algorithm Comparison**:
| | Geohash | Quadtree | S2 | Redis GEORADIUS |
|---|---|---|---|---|
| Lookup | O(1) | O(log N) | O(log N) | O(N+log M) |
| Memory (1B) | 20 GB | 1 GB | 9 GB | 8 GB |
| False pos. | ~50% | ~0% | ~0% | ~0% |
| K-NN | No | Yes | Yes | Yes |

### Unique Data Model
- Quadtree node: bbox, depth, children[4] (None if leaf), entities[] list
- S2 CellID: 64-bit integer; CRC-16 level encoding in low 3 bits
- Geohash ring buffer: `{geohash_string: Set[entity_id]}` (Python dict), O(1) access

### Unique Scalability / Design Decisions
- **Bulk loading with Z-order / Hilbert curve sorting**: pre-sorting entities by spatial key before bulk insert improves cache locality (spatially proximate entities in same memory page); 5× faster bulk load for geohash, 10× for quadtree
- **Antimeridian handling for Geohash**: cells crossing ±180° longitude wrap around; queries near the International Date Line must split into two separate cell lookups; S2 avoids this entirely as a spherical index
- **Vincenty vs. Haversine**: Haversine assumes spherical Earth (~0.3% error for typical distances); Vincenty assumes oblate spheroid (millimeter accuracy); for distance < 1,000 km, Haversine error < 3.3 km/1,000 km and is preferred (faster, simpler); Vincenty only needed for transoceanic routing

### Key Differentiator
Geospatial Index's uniqueness is its **comparative algorithm analysis**: the only problem in this folder that provides side-by-side implementation details of Geohash, Quadtree, S2 Hilbert curve, Jump Consistent Hash, and Rendezvous Hashing as algorithm choices — including memory footprint, lookup complexity, false-positive rates, K-NN support, and bulk loading optimization — serving as a reference for choosing the right spatial index for any use case.
