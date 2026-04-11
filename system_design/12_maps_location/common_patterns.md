# Common Patterns — Maps & Location (12_maps_location)

## Common Components

### Geospatial Indexing (Geohash / S2 / Quadtree)
- All four problems use some form of spatial indexing to enable fast proximity queries
- In google_maps: S2 cells at level 6 (~250 km × 250 km) for road graph sharding; S2 level 14 for road nodes and places; S2 cell ID (64-bit: face 3 bits + Hilbert curve 58 bits + level 3 bits)
- In proximity_service: Geohash precision 6 (~1.2 km × 0.6 km) for inverted index; 9 adjacent cells (center + 8 Moore neighbors) cover full search circle
- In yelp_nearby: Elasticsearch geo_point field; Gaussian decay function on distance (scale=2 km, offset=100 m)
- In geospatial_index: detailed comparison of Geohash, Quadtree, S2 as standalone algorithms; Geohash P6 default, Quadtree with MAX_ENTITIES_PER_LEAF=50, S2 Hilbert curve

### Haversine Distance Post-Filter
- All four use Haversine distance (or Vincenty for extreme accuracy) as a post-filter after spatial index lookup
- The spatial index returns candidates (with potential false positives at cell boundaries); Haversine computes exact distance to filter false positives
- In google_maps: used in routing and traffic speed calculation
- In proximity_service: post-filter after geohash cell candidates (false positive rate ~50% at typical radius)
- In yelp_nearby: distance-based ranking, exact filter after ES geo_distance query
- In geospatial_index: explicit zero false-negative requirement; false positives filtered by exact Haversine computation

### Redis for Real-Time Spatial Data
- All four use Redis for sub-millisecond access to frequently-read location or spatial data
- In google_maps: traffic weight per road cell (30 s TTL), in-memory LRU for routing graph, Places data (5 min TTL), route cache (30 s for identical origin/dest/mode)
- In proximity_service: `gh6:{geohash6}` Set of entity_ids; `loc:{entity_id}` HASH with 30 s TTL for live movers; entity detail cache (5 min TTL)
- In yelp_nearby: rating/review count cache (500 MB for 10 M businesses), search result cache (10 s TTL), session store
- In geospatial_index: Redis GEORADIUS option as a built-in alternative for in-memory spatial index (8 GB for 500 M entities)

### Kafka for Event Streaming and Spatial Updates
- Three of four (google_maps, proximity_service, yelp_nearby) use Kafka to decouple writes from downstream processing
- In google_maps: probe ingestion at 6 M RPS → Kafka `probe-events` topic; Flink consumer computes median speed per segment per 30 s bucket; `catalog-updated` topic for tile invalidation
- In proximity_service: `location-updates` topic (200 partitions by geohash cell); Geo Index Updater as Kafka consumer moves entity from old to new geohash cell when cell boundary crossed
- In yelp_nearby: `review-events` (Rating Aggregator consumer), `photo-events`, `business-updates` (Search Indexer consumer for ES partial update)
- In geospatial_index: not applicable (single-node library design, no messaging layer)

### CDN for Static / Cacheable Map Content
- Google Maps and Yelp use CDN to absorb high read traffic for non-personalized content
- In google_maps: CloudFront/Akamai; tile `max-age=86400` for static tiles, `max-age=1` for manifest-like files; 95%+ cache hit rate target at 694K RPS
- In yelp_nearby: CDN for photo serving (10 B photos × avg 500 KB = 5 PB); content-hash URL for immutable caching; photo TTL 1 year; 80% of photo traffic served by CDN
- In proximity_service: no CDN (entity location data changes too frequently)
- In geospatial_index: not applicable

### Eventual Consistency with Bounded Staleness SLA
- All four accept eventual consistency for spatial data with defined bounded staleness windows
- In google_maps: traffic freshness ≤ 60 s end-to-end (probe to road weight); map data eventual within 2 min after tile pipeline
- In proximity_service: real-time movers within 5 s; business locations within 60 s
- In yelp_nearby: ratings within 30 s (Kafka-driven Aggregator), search index within 60 s
- In geospatial_index: single-node, no distributed consistency concern

### Three-Tier Replication (Multi-Region)
- Google Maps and Proximity Service replicate across regions for global low-latency access
- In google_maps: 3-region Bigtable/Spanner; write quorum = 2; read from any replica; Kafka RF=3, min-ISR=2; GCS multi-region bucket (US, EU, APAC)
- In proximity_service: Redis primary + 2 replicas per shard (`WAIT 1 0` for near-sync); Kafka RF=3 min-ISR=2; PostgreSQL primary + 2 read replicas
- In yelp_nearby: PostgreSQL primary + 2 read replicas; Elasticsearch replicas (1 replica per shard, multi-AZ); S3 3× replication
- In geospatial_index: not addressed (single-node/library focus)

## Common Databases

### Redis (In-Memory Spatial/Location Cache)
- All four; used for hot location data with short TTLs; sorted sets for spatial operations; sets for entity membership per cell; hashes for entity metadata

### PostgreSQL / Aurora (Entity Metadata Source of Truth)
- All four; ACID source of truth for business/entity/user metadata; PostGIS extension for spatial data types (`geography(point, 4326)`, `ST_DWithin`)
- Proximity Service: entity table with `geography(point, 4326)`, PostGIS `ST_DWithin` for admin queries
- Yelp: businesses table with PostGIS geography, reviews, users; GIST index on geography column

### Cassandra (High-Write Time-Series Location History)
- Proximity Service and Google Maps (indirectly via Bigtable); append-only time-series for location events; TTL-based retention
- Proximity Service: location history partitioned by `(entity_id, day)`, append-only, TTL 30 days, 1 PB total
- Google Maps (Bigtable): `(segment_id, timestamp_bucket)` for traffic; `(edge_id, time)` for historical speeds; 1 PB traffic history over 5 years

### Elasticsearch (Full-Text + Geo Search)
- Yelp and proximity_service use Elasticsearch for combined text + geo queries
- Yelp: 10 primary shards + 1 replica; restaurant_index with geo_point field, function_score for BM25 + distance decay + rating boost; 50 GB index for 10 M businesses
- Proximity Service: optional ES for complex entity queries; primary index is Redis geohash inverted index

## Common Communication Patterns

### Spatial Index Lookup → Haversine Post-Filter
- All four use the two-step pattern: coarse spatial index (geohash/S2/quadtree/ES geo) returns candidates; exact Haversine distance filters false positives
- This design separates O(1) or O(log N) fast lookups from O(K) exact computation on small candidate sets

### Event-Driven Index Updates (Kafka → Consumer → Index Update)
- Google Maps, Proximity Service, and Yelp all use Kafka-driven consumers to maintain derived indexes (tile cache, geohash inverted index, ES search index) eventually consistent with the primary DB

## Common Scalability Techniques

### Cell-Based Sharding (S2 / Geohash / H3)
- All four use geographic cell hierarchies to shard data by location; ensures spatially proximate data is co-located for efficient range queries

### In-Process Spatial Index for Low-Latency Serving
- Google Maps (CH graph in-process), Proximity Service (Redis geohash index in-process), Yelp (ES in-process), Geospatial Index (FAISS / quadtree / S2 all in-process)
- Avoids network hops for frequent spatial lookups; O(log N) or O(1) per query

## Common Deep Dive Questions

### How do you find all entities within a 10 km radius efficiently?
Answer: Two-step with geohash inverted index. Convert (lat, lon, radius) to geohash precision P such that cell width ≈ radius/2. Look up 9 cells (center + 8 neighbors) in Redis `gh6:{cell}` Set — O(1) per cell lookup. Union all entity_ids. Post-filter with Haversine to remove entities at cell boundaries (false positive rate ~50%). For radius > 10 km, use precision 5 (~2.4 km cells) and extend to larger neighborhood. Zero false negatives guaranteed (cell boundaries always include full radius); false positives discarded by exact distance check.
Present in: proximity_service, geospatial_index

### How do you keep map/entity data fresh within 60 seconds at millions of RPS?
Answer: Event-driven invalidation with TTL safety net. Writer publishes to Kafka on entity update; cache invalidator consumer deletes Redis key and (for maps) purges CDN tile; TTL ensures eventual consistency even if Kafka event is dropped. For traffic data: Flink aggregates GPS probes per road segment per 30 s bucket; writes to Bigtable + Redis; routing service fetches from Redis with 30 s TTL. For entity locations: live movers push location at 4s intervals; two-tier write (immediate Redis + async Kafka for cell transitions).
Present in: google_maps, proximity_service, yelp_nearby

### What is the difference between Geohash, Quadtree, and S2 for spatial indexing?
Answer: Geohash (Z-order curve, base-32): O(1) lookup via inverted index; precision P controls cell size; simple string comparison; antimeridian discontinuity at ±180°; 50% false positive rate at cell boundaries. Quadtree: hierarchical, adapts to data density (splits only when > MAX_ENTITIES leaf threshold); O(log N) insert/query; K-NN via best-first traversal; ~1 GB in memory for 1B entities. S2 (Hilbert curve on cube projection): spherical, no discontinuities; 30 levels; 64-bit CellID; best for polygon covering; used by Google Maps for road graph sharding and PostGIS for geospatial joins. Redis GEORADIUS: built-in geohash, ~8 GB for 500M entities, GEORADIUS O(N+log M).
Present in: geospatial_index

## Common NFRs

- **Radius search latency**: p99 ≤ 100 ms (≤10 km); p99 ≤ 200 ms (≤50 km)
- **Location update ack**: p99 ≤ 50–200 ms depending on system
- **Data freshness**: ≤ 5–60 s for different entity types
- **Zero false negatives**: spatial queries must return all entities within radius
- **Availability**: 99.9%–99.99% depending on problem (Google Maps: 99.99%)
- **Read/write ratio**: heavily read-dominant (10:1 to 200:1)
