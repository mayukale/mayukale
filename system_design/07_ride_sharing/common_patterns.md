# Common Patterns — Ride Sharing (07_ride_sharing)

## Common Components

### Redis Geo (GEOADD / GEORADIUS) for Spatial Driver Lookup
- All four problems rely on Redis Geo sorted sets as the primary hot-path store for current driver positions
- In uber_lyft: `GEOADD city:{city_id}:drivers:available <lng> <lat> <driver_id>`; GEORADIUS with 5 km default radius, top 50 candidates, returns WITHCOORD WITHDIST; 1.5 M drivers × 64 bytes ≈ 96 MB per cluster — fits entirely in memory
- In location_tracking: Redis Geo sorted set per city per driver status (`city:{city_id}:drivers:{status}`); sub-millisecond GEORADIUS O(N + log M); separate from Cassandra history
- In driver_matching: Matching Service consumes GEORADIUS results from Location Service for candidate retrieval; 531 GEORADIUS calls/s at peak (with radius expansion)
- In eta_service: does not directly use Redis Geo but road-snap queries reference the spatial node graph — consistent-hash routing ensures all queries for a city land on the same ETA pod (analogous to shard locality)

### H3 Hexagonal Grid for Geographic Aggregation
- All four use H3 hexagons (Uber's open-source geo library) for spatial bucketing and indexing
- In uber_lyft: H3 resolution 7 for surge pricing cells (demand/supply ratio per cell → multiplier); H3 resolution 9 for `pickup_h3_index` column on the trips table (spatial index for analytics)
- In location_tracking: geofences store pre-computed H3 cells (`h3_cells JSONB`) at configurable resolution (default 9) for fast containment checks (O(1) lookup: does point's H3 cell ∈ fence's H3 cell set?)
- In driver_matching: `pickup_h3_index` on trips table; city-level dispatch scoped by city geo boundary which maps to H3 cells
- In eta_service: city-level geographic partitioning maps to H3 boundaries; ETA pod assignment by city_id (consistent hash)

### Kafka for High-Throughput Event Streaming
- All four use Kafka as the durable async event bus connecting real-time ingestion to downstream consumers
- In uber_lyft: topics `location-updates`, `trip-events`, `payment-events`, `notification-events`; decouples Trip Service from Notification Service and Payment Service
- In location_tracking: `location-updates` topic partitioned by driver_id (same driver always same partition for ordering); 24-hour retention; consumed by Cassandra Consumer (GPS history write) and WebSocket Dispatcher (rider push)
- In driver_matching: publishes `DRIVER_ASSIGNED` event to Kafka when CAS UPDATE succeeds; downstream notifications consumed by Notification Service
- In eta_service: Traffic Ingest Pipeline consumes `location-updates` from Kafka; aggregates driver speeds per road edge; publishes speed deltas to Redis every 5 minutes

### PostgreSQL with Citus (Sharded by city_id)
- Three of the four problems store their core relational data in PostgreSQL horizontally sharded by city_id using Citus
- In uber_lyft: `create_distributed_table('trips', 'city_id')`; tables: riders, drivers, vehicles, trips, trip_state_log, ratings, payment_methods, surge_cells, cities; all trip queries scoped to one city shard
- In location_tracking: geofences and geofence_events in PostgreSQL with PostGIS spatial extension (GiST index on boundary GEOGRAPHY); driver_sampling_config per driver
- In driver_matching: matching_attempts, driver_offers, driver_request_counters, matching_config — all sharded by city_id via Citus; matching_config cached in Matching Service with read-through cache
- In eta_service: eta_accuracy_log, road_speed_baselines, graph_versions in PostgreSQL + Citus; eta_accuracy_log partitioned by city_id

### City-Level Geographic Partitioning (Shard Key = city_id)
- All four partition their data and compute by city_id for data locality, compliance (GDPR per country), and natural load balancing
- In uber_lyft: PostgreSQL Citus shard key = city_id; Redis keys prefixed `city:{city_id}`; no cross-city matching
- In location_tracking: Redis Geo keys `city:{city_id}:drivers:{status}`; Cassandra partitions written with driver_id (city implied by driver's current city)
- In driver_matching: Matching Service pods partitioned by city_id for Redis cache locality; matching_config stored per city; radius expansion parameters per city
- In eta_service: one CH graph per city loaded in memory; ETA service pod assigned city_ids via consistent hash; stateful pods (graph-in-memory) route by city

### Cassandra for GPS Time-Series (TTL + TWCS)
- Three of the four use Cassandra for append-only GPS history with time-based TTL and TimeWindowCompactionStrategy
- In uber_lyft: `driver_location_history` table; PRIMARY KEY ((driver_id, trip_id), recorded_at); `default_time_to_live = 7776000` (90 days); RF=3; LZ4 compression
- In location_tracking: `driver_gps_history` (partition by (driver_id, trip_id)), `driver_gps_by_driver` (partition by driver_id, 7-day TTL), `driver_current_position`; TWCS with 1-day compaction window; LZ4Compressor
- In driver_matching: relies on Cassandra via Location Service for dispute resolution (not directly written to); uses Redis for ephemeral matching state
- In eta_service: does not use Cassandra; uses Apache Parquet on S3 for historical speed patterns (7-day rolling)

### Redis for Ephemeral State with Short TTL
- All four use Redis Hash and String keys with explicit TTLs for ephemeral hot state that must not survive beyond its useful window
- In uber_lyft: `driver:{driver_id}:telemetry` Hash (speed, heading, updated_at; 30 s TTL); `trip:{trip_id}:driver_location` Hash (4 h TTL)
- In location_tracking: `driver:{driver_id}:telemetry` (30 s TTL); `rider:{rider_id}:ws_server` String (60 s TTL, refreshed by heartbeat); `trip:{trip_id}:driver_location` (TTL = trip duration)
- In driver_matching: `matching:inflight:{trip_id}` (30 s TTL for dedup); `trip:{trip_id}:offered_drivers` Set (60 s TTL); `driver:{driver_id}:pending_offer` Hash (20 s TTL); `fairness:{city_id}:{hour}:{driver_id}` counter (3600 s TTL); `driver:{driver_id}:cooldown` (300 s TTL)
- In eta_service: `traffic:{city_id}:edge_speeds` Hash (10 min TTL); `traffic:{city_id}:updated_edges` Sorted Set (10 min TTL); `traffic:{city_id}:edge_probe_count` Hash

### WebSocket for Real-Time Rider Push
- Three of the four use WebSocket (or SSE) for pushing continuous location updates to riders tracking their driver
- In uber_lyft: WebSocket push to rider every 4 seconds from WebSocket Dispatcher; 500K active trips × 1 push/4s = 125,000 push/s; `rider:{rider_id}:ws_server` in Redis routes to the correct WS server
- In location_tracking: WebSocket Dispatcher consumes Kafka `location-updates` → pushes driver position to rider app; driver location → rider push latency < 2 s p99; end-to-end from GPS event to rider screen
- In driver_matching: driver receives offer via FCM/APNs push (not WebSocket); rider continues to receive updates via WebSocket after match
- In eta_service: ETA updates piggybacked on location push to rider (not a separate WebSocket stream)

### CAS UPDATE for Atomic Trip State Transition
- The two problems involving direct trip state transitions use Compare-And-Swap UPDATE to prevent double-assignment without distributed locks
- In uber_lyft: `UPDATE trips SET status='DRIVER_ASSIGNED', driver_id=? WHERE trip_id=? AND status='REQUESTED'`; `UPDATE drivers SET status='ON_TRIP' WHERE driver_id=? AND status='AVAILABLE'`; both must succeed (two CAS updates in sequence; rollback if second fails)
- In driver_matching: identical CAS pattern; Step 3 = CAS on trips; Step 4 = CAS on drivers; both inside Trip Service; returns 409 if already assigned
- In location_tracking / eta_service: read-only or append-only; CAS not applicable

### JWT Bearer Token Auth + Rate Limiting at API Gateway
- All four APIs authenticate with JWT Bearer tokens (RS256, 1-hour expiry) and enforce rate limits at the API Gateway
- In uber_lyft: riders 100 req/s gateway limit, 60 req/min sustained; drivers 120 req/min (burst 40/s for frequent location updates)
- In location_tracking: `POST /v1/driver/location` rate-limited 120 req/min per driver (burst 30 req/s); 400/401/403/429 documented error codes
- In driver_matching: internal gRPC service (no public rate limit); external accept/decline endpoints inherit driver JWT rate limits
- In eta_service: internal gRPC service; Matching Service authenticated to call ComputeETA batch

### gRPC for Internal Service Communication
- Driver Matching and ETA Service expose gRPC interfaces for high-throughput internal calls between microservices
- In driver_matching: `MatchingService` gRPC (MatchRide, CancelMatch, GetMatchingStats)
- In eta_service: `ETAService` gRPC (ComputeETA, ComputeETABatch, ComputeMultiWaypointETA, GetAlternativeRoutes); ETABatch allows Matching Service to score 50 drivers in one RPC (vs. 50 sequential calls)
- In uber_lyft / location_tracking: REST for external; internal calls likely gRPC (not explicitly named)

## Common Databases

### Redis Cluster
- Used in all four for current driver positions (Geo sorted sets), ephemeral trip/driver state, rate limiting, and WebSocket session registry
- Sub-millisecond reads make it the only viable hot path for 562,500 GPS writes/s and 125,000 rider pushes/s

### PostgreSQL + Citus
- Used in three of four (uber_lyft, driver_matching, eta_service) for relational trip data, matching logs, ETA accuracy, and geofence configuration; sharded by city_id

### Cassandra
- Used in two (uber_lyft, location_tracking) for high-write GPS time-series with 90-day TTL and TWCS compaction; RF=3 across two regions

## Common Communication Patterns

### Separation of Control Plane (REST/gRPC) and Data Plane (Kafka + Redis)
- State changes and requests flow through REST/gRPC APIs; continuous high-frequency data (GPS updates, ETA refreshes, rider pushes) flow through Kafka and Redis
- REST for: trip create/state transitions, driver location updates (batch), fare estimates
- Kafka for: fan-out to Cassandra consumer, WebSocket dispatcher, traffic ingest pipeline
- Redis for: zero-latency serving of current state (positions, offer state, traffic weights)

## Common Scalability Techniques

### Stateless City-Scoped Pods with Consistent Hash Routing
- All services route requests by city_id to pods that hold city-specific in-memory state (Redis connections, CH graphs) for cache locality
- In uber_lyft: API Gateway routes by city_id from trip data
- In driver_matching: Matching Service pods partitioned by city_id
- In eta_service: ETA pods assigned city_ids by consistent hash; stateful (graph in memory)

### Horizontal Sharding by Natural Partition (city_id)
- city_id is the natural partition key across all four problems; enables data locality, compliance, and independent scaling per city
- All PostgreSQL tables distributed by city_id; Redis keys prefixed by city_id; Kafka topics partitioned by driver_id (city implied)

### Protobuf / Binary Serialization for High-Throughput Paths
- Location updates use protobuf encoding (not JSON) to reduce payload size: 64 bytes per GPS point vs. ~200 bytes for JSON
- ETA requests/responses use protobuf; internal gRPC is binary

### Pre-Computation and Async Enrichment
- Road-snapping, historical speed computation, and CH graph building are all done offline/async — not in the hot request path
- In eta_service: CH graph preprocessed (hours per city); loaded into memory at startup; daily graph updates from OSM diffs
- In location_tracking: road-snapping written back to Cassandra asynchronously after GPS point is stored; does not block driver ACK

## Common Deep Dive Questions

### How do you find available drivers near a pickup point in < 10 ms?
Answer: Redis Geo sorted sets. All available driver positions are maintained in an in-memory sorted set keyed by geographic position (GeoHash-encoded score). `GEORADIUS city:{city_id}:drivers:available {lat} {lng} 5 km COUNT 50 ASC WITHCOORD WITHDIST` executes an O(N + log M) range query in < 1 ms at 96 MB dataset size. On driver status change (AVAILABLE → ON_TRIP), the driver is removed from the set atomically. The entire 1.5M-driver dataset fits in ~96 MB of RAM, easily within a single Redis instance.
Present in: uber_lyft, location_tracking, driver_matching

### How do you handle 562,500 GPS writes per second without dropping data?
Answer: Two-tier write path. Synchronous path: update Redis Geo (< 1 ms, critical for matching and rider tracking). Async path: publish to Kafka `location-updates` topic partitioned by driver_id. Kafka consumers (Cassandra writer, WebSocket dispatcher, traffic ingest) process in parallel without blocking the driver app ACK. Cassandra writer batch-inserts 500 rows per write for efficiency. LZ4 compression + TWCS compaction handles the write volume without compaction storms.
Present in: uber_lyft, location_tracking

### How do you atomically assign exactly one driver to one trip?
Answer: Compare-And-Swap UPDATE in PostgreSQL. `UPDATE trips SET status='DRIVER_ASSIGNED', driver_id={driver_id} WHERE trip_id={trip_id} AND status='REQUESTED'` — only succeeds if status is still REQUESTED. If another driver accepted first, this fails (returns 0 rows). Then `UPDATE drivers SET status='ON_TRIP' WHERE driver_id={driver_id} AND status='AVAILABLE'`. Redis SETNX `matching:inflight:{trip_id}` prevents duplicate matching runs on the same trip. No distributed lock required — the DB optimistic lock (CAS) provides atomicity.
Present in: uber_lyft, driver_matching

### How do you compute driving ETA for 27,770 requests/second with < 200 ms p99?
Answer: Contraction Hierarchies (CH) preprocessing. A* on a 50M-node graph takes 100s ms. CH preprocesses the graph by adding shortcut edges and assigning node ranks; bidirectional A* on the CH graph queries only upward edges — reducing search space from millions of nodes to thousands. CH query time: < 1 ms (long distance), < 5 ms (urban). Dynamic traffic overlaid via Redis speed deltas. Each ETA pod loads its city's CH graph (~3 GB) into process memory at startup; zero-latency access thereafter. ETABatch API scores 50 driver candidates in one RPC.
Present in: eta_service

### How do you keep real-time traffic data fresh without rebuilding the graph?
Answer: Dual-layer approach. Static base layer: Contraction Hierarchy graph computed daily from OSM data + historical speed patterns (7-day rolling average per edge per hour/day-of-week from Flink aggregation, stored in S3 Parquet). Dynamic overlay: Traffic Ingest Pipeline consumes Kafka `location-updates`, computes current median speed per road edge from probe vehicle speeds, writes delta (current_speed - baseline) to Redis Hash every 5 minutes. ETA EdgeWeight function reads delta from Redis (1 ms) and computes effective_speed = max(5 km/h, baseline + delta). CH shortcuts are not rebuilt — only leaf edge weights change.
Present in: eta_service

## Common NFRs

- **Driver match latency**: p99 < 3 s end-to-end (request → driver notified)
- **GPS write acknowledgment**: p99 < 200 ms (driver app waits for ACK)
- **Driver position staleness**: p99 < 5 s (rider sees driver position ≤ 5 s old)
- **ETA accuracy**: MAPE < 15%, P95 error < 5 min for trips < 30 min
- **Availability**: 99.99% for trip request / match flow; matching must be available
- **Strong consistency**: for trip state transitions (no double-assignment); eventual for GPS history and ETA
- **Durability**: GPS history retained 90 days for dispute resolution; trip records 7 years
- **Security**: JWT RS256 Bearer tokens; rate limiting at API Gateway; no cross-city data leakage
