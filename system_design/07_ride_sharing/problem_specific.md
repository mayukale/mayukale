# Problem-Specific Design — Ride Sharing (07_ride_sharing)

## Uber / Lyft (Core Platform)

### Unique Functional Requirements
- Full trip lifecycle state machine: REQUESTED → DRIVER_ASSIGNED → DRIVER_EN_ROUTE → RIDER_PICKED_UP → TRIP_IN_PROGRESS → COMPLETED / CANCELLED
- Surge pricing: demand/supply ratio per H3 hexagon (resolution 7) → surge multiplier; stored in `surge_cells` table and cached in Redis
- Multiple ride types: Standard (UberX/Lyft), XL, Lux (UberBlack/Lyft Lux), Pool/Shared
- Rider-to-driver ratings (1–5 stars) post-trip; driver-to-rider ratings
- Cancellation reasons, cancellation fees (configurable time windows)
- Payment flow: fare estimate pre-trip → final fare post-trip → charge via Stripe/Braintree; idempotency via payment_reference key

### Unique Components / Services
- **Trip Service**: owns the trip state machine; writes state transitions atomically to PostgreSQL; publishes events to Kafka on each state change; the single authority for trip status
- **Fare / Pricing Service**: computes `base_fare + distance × per_km + duration × per_min + surge_addition - promo_discount + tolls`; reads surge_multiplier from `surge_cells` (H3 resolution 7) for the pickup cell; fare estimate < 200 ms p50, < 500 ms p99
- **Payment Service**: wraps Stripe/Braintree SDK; stores tokenized card credentials (`provider_token` in `payment_methods` table); idempotent charge on trip completion; < 2 s p50, < 5 s p99
- **Rating Service**: accepts 1–5 star rating after trip completes; incremental rolling average stored on `riders.rating` and `drivers.rating` columns (not re-computed from log every time)
- **Surge Pricing Subsystem**: Flink or batch job aggregates demand (open requests in last 5 min) and supply (available drivers) per H3 cell; computes multiplier; writes to `surge_cells` table; cached in Redis per cell with short TTL

### Unique Data Model
- **trips** (PostgreSQL + Citus, shard key = city_id): trip_id, rider_id, driver_id, status (6-state enum), vehicle_type, pickup/dropoff lat/lng, pickup_h3_index (H3 resolution 9), estimated_fare_usd, surge_multiplier, base_fare_usd, distance_km, duration_minutes, final_fare_usd, promo_discount_usd, tolls_usd, polyline (encoded), city_id, payment_method_id
- **trip_state_log**: append-only log of every state transition (from_state, to_state, actor RIDER|DRIVER|SYSTEM, metadata JSONB)
- **surge_cells** (PostgreSQL): h3_index + snapshot_at (PK), demand_count (open requests last 5 min), supply_count (available drivers), surge_multiplier NUMERIC(4,2)
- **ratings**: rating_id, trip_id, rater_type (RIDER|DRIVER), rater_id, ratee_id, score SMALLINT (1–5), comment
- **payment_methods**: provider (STRIPE|BRAINTREE), provider_token, card_last4, card_brand, exp_month, exp_year, is_default

### Unique Scalability / Design Decisions
- **PostgreSQL + Citus (shard by city_id)**: all trip data for a city resides on one Citus shard; city_id is a natural partition that aligns with geographic load; enables joins within a city (trip + driver + rating) without cross-shard queries; online resharding via Citus
- **H3 resolution 9 spatial index on trips**: `idx_trips_h3 ON trips(pickup_h3_index)` enables spatial analytics (heat maps, surge calibration) without PostGIS geometry queries on the hot path
- **Encoded polyline storage**: full GPS path for completed trips stored as a Google Encoded Polyline string (~200 bytes for typical urban trip vs. raw GPS which is KB); sufficient for replay and receipt display
- **Cancellation state machine**: CANCELLED state reachable from REQUESTED, DRIVER_ASSIGNED, DRIVER_EN_ROUTE; cancellation by RIDER, DRIVER, or SYSTEM; cancellation fee logic depends on current state and elapsed time (DRIVER_EN_ROUTE stage triggers fee)
- **WebSocket for rider tracking**: after DRIVER_ASSIGNED, rider receives real-time driver position every 4 s via WebSocket; Location Service's WebSocket Dispatcher pushes to `rider:{rider_id}:ws_server` endpoint in Redis (60 s TTL)

### Key Differentiator
Uber/Lyft's uniqueness is the **full trip lifecycle orchestration**: the Trip Service owns a 6-state machine with ACID guarantees across two tables (trips + drivers) via CAS UPDATE, integrated with surge pricing (H3 hex demand/supply), payment processing (Stripe/Braintree idempotent charge), and post-trip ratings — the full end-to-end business flow that the other three problems (which focus on individual sub-systems) do not cover.

---

## Location Tracking

### Unique Functional Requirements
- Adaptive GPS sampling rate: Battery Optimizer Service analyses driver speed/heading variance; recommends reduced sampling (e.g., 8,000 ms interval instead of 4,000 ms) when stationary; pushed to driver app via WebSocket
- Road-snapping: raw GPS coordinates matched to nearest road segment (edge in road graph); `road_snapped_lat`, `road_snapped_lng`, `road_segment_id` written back to Cassandra asynchronously after storage
- Geofencing: four zone types (AIRPORT_ZONE, CITY_BOUNDARY, RESTRICTED_ZONE, SURGE_ZONE); containment check via H3 cell membership (O(1)); events stored in `geofence_events` table; actions: NOTIFY, RESTRICT, AUTO_ACCEPT
- GPS trip replay: historical trip path played back at configurable speeds (1× to 60× real-time) via Server-Sent Events stream for dispute resolution
- Multiple GPS provider support: GPS (native), NETWORK (Wi-Fi/cell), FUSED (Android/iOS fusion API); stored as `raw_provider` in Cassandra

### Unique Components / Services
- **Battery Optimizer Service**: consumes driver speed + heading time-series; computes variance; if speed < 5 km/h for > 60 s → push `update_interval_ms=8000, reason=STATIONARY`; if on highway → `update_interval_ms=4000` (standard); reduces battery drain for drivers waiting for requests by ~40%
- **Geofence Evaluation Service**: loads all active geofences for a city into process memory (H3 cell sets); for each incoming GPS point, checks membership in H3 cell precomputed per geofence; ~56,250 evaluations/s at peak (10% of 562,500 GPS writes); publishes `ENTER`/`EXIT` events to `geofence_events` table and to Kafka for downstream consumers
- **Trip Replay Service**: reads Cassandra `driver_gps_history` for (driver_id, trip_id) partition; streams GPS points via SSE at requested playback speed (rate-controlled by `sleep(point_interval_ms / playback_speed)`); endpoint: `GET /v1/trips/{trip_id}/gps-replay?playback_speed=2`
- **Cassandra Consumer (Kafka → Cassandra)**: batch-inserts GPS rows from Kafka at 500 rows/batch; uses TWCS with 1-day window; compresses with LZ4; sole writer to the durable time-series store

### Unique Data Model
- **driver_gps_history** (Cassandra): PRIMARY KEY ((driver_id, trip_id), recorded_at ASC); includes raw_provider, road_snapped_lat/lng, road_segment_id (all nullable until road-snap completes); TTL 90 days; TWCS 1-day window; LZ4 compression
- **driver_gps_by_driver** (Cassandra): partition key = driver_id, clustering by recorded_at DESC; 7-day TTL; enables "show all locations for driver X in last 7 days" without trip_id — separate access pattern from trip-level replay
- **driver_current_position** (Cassandra): simple PRIMARY KEY = driver_id; current lat/lng/heading/speed/city_id/status/trip_id; low cardinality, high update rate; alternative to Redis for fallback reads (503 fallback path)
- **geofences** (PostgreSQL + PostGIS): boundary as GEOGRAPHY(POLYGON, 4326); GiST spatial index; h3_cells JSONB (pre-computed list of H3 cell IDs at geofence's resolution for O(1) containment check)
- **driver_sampling_config** (PostgreSQL): per-driver sampling override; update_interval_ms, min_distance_m (suppress if moved < 5 m), config_reason (STATIONARY|HIGHWAY|URBAN|TRIP_START)
- **GPS point size**: 48 bytes raw uncompressed; ~58 bytes effective with RF=3 + LZ4; 90-day dataset = ~67.5 TB

### Unique Scalability / Design Decisions
- **TWCS (TimeWindowCompactionStrategy) with 1-day window**: GPS writes land in the current day's time window; old windows are immutable → TWCS compacts them without read amplification or tombstone buildup; critical at 562,500 writes/s which would overwhelm STCS compaction
- **H3 pre-computation on geofences**: at geofence creation time, all H3 cells at resolution 9 that overlap the polygon are computed and stored in `h3_cells JSONB`; at evaluation time, check if driver's current H3 cell ∈ stored cell set — O(1) hash lookup instead of PostGIS ST_Contains on every GPS point
- **Dual Cassandra access patterns**: trip-level partition (driver_id, trip_id) for replay; driver-only partition (driver_id) for 7-day history view; same data stored twice at different granularities (storage cost traded for query flexibility)
- **Redis telemetry TTL 30 s**: if a driver goes offline without explicitly sending an OFFLINE status, the telemetry key expires in 30 s and driver is treated as stale; prevents "ghost driver" appearing available when actually disconnected
- **Road-snapping async (non-blocking)**: snapping raw GPS to nearest road edge is computationally expensive (nearest-neighbor search on road graph); done asynchronously after GPS point is stored; `road_snapped_lat/lng` are nullable in Cassandra until snap job backfills them

### Key Differentiator
Location Tracking's uniqueness is its **multi-consumer fan-out architecture**: a single GPS update from a driver triggers four independent downstream actions in parallel — Redis Geo update (matching), Cassandra write (history/replay), WebSocket push (rider tracking), and Geofence evaluation — all decoupled via Kafka, each with independent failure isolation; plus the battery optimizer that adapts sampling rate per driver state (stationary vs. highway), which no other problem in this folder models.

---

## Driver Matching

### Unique Functional Requirements
- Multi-factor driver scoring: composite score = 0.60×distance + 0.20×rating + 0.10×acceptance_rate + 0.10×heading_score; configurable weights per city stored in `matching_config` table
- Radius expansion: initial 5 km → 8 km → 12 km → 20 km with wait periods [3 s, 5 s, 8 s] before each expansion; avoids immediate "no drivers" failure
- Offer waterfall: 15-second timeout per driver; if driver declines or times out, next best-scored driver receives offer; average 1.3 waterfall depth (20% of matches need a second driver offer)
- Fairness constraints: per-city hourly request counters per driver; if driver receives > 2× city average requests in 1 hour → 5-minute cooldown (excluded from matching)
- Pool/Shared compatibility: for SHARED vehicle type, evaluate route detour compatibility with in-progress SHARED trips before offering

### Unique Components / Services
- **Scoring Engine**: given a list of 50 GEORADIUS candidates, scores each using the 4-factor formula in < 5 ms p99 for 50 candidates; runs in Go goroutines (~200 µs per candidate); bearing computed via haversine formula; heading score = (cos(bearing_diff_rad) + 1) / 2 → normalized [0, 1]
- **Fairness & Cooldown Subsystem**: Redis counters `fairness:{city_id}:{hour_epoch}:{driver_id}` incremented on each offer; city average maintained in `fairness:{city_id}:{hour_epoch}:avg`; if driver counter > avg × 2.0 → set `driver:{driver_id}:cooldown` with 300 s TTL; TTL auto-expires the cooldown
- **Matching Config Service**: per-city configuration (weights, radius steps, offer timeout, min driver rating) stored in `matching_config` PostgreSQL table; read-through cache in Matching Service with low TTL; allows A/B testing different matching parameters per city without code deploy
- **In-Flight Deduplication**: `matching:inflight:{trip_id}` Redis SETNX (30 s TTL) prevents duplicate parallel matching runs on the same trip_id if the Trip Service retries; returns DUPLICATE status to second caller

### Unique Data Model
- **matching_attempts** (PostgreSQL + Citus, shard key = city_id): full record per matching attempt — candidates_found, candidates_scored, assigned_driver_id, outcome (MATCHED|NO_DRIVERS|TIMEOUT|CANCELLED|RIDER_CANCELLED), match_latency_ms, radius_expansions, scoring_weights JSONB (snapshot for analysis)
- **driver_offers**: offer_id, attempt_id, driver_id, trip_id, offered_at, expires_at, outcome (ACCEPTED|DECLINED|TIMEOUT|SUPERSEDED), composite_score, score_breakdown JSONB {distance_score, rating_score, acceptance_score, heading_score}
- **driver_request_counters**: (driver_id, city_id, window_start) → requests_offered, requests_accepted, cooldown_until; 1-hour window granularity
- **matching_config**: per-city weights (score_weight_distance 0.600, score_weight_rating 0.200, etc.), initial_radius_km, radius_steps_km JSON array, offer_timeout_ms, fairness_window_min, min_driver_rating 4.0
- **Redis ephemeral state**: `matching:inflight:{trip_id}` (30 s), `trip:{trip_id}:offered_drivers` Set (60 s), `driver:{driver_id}:pending_offer` Hash (20 s), fairness counters (3600 s), cooldown key (300 s)

### Unique Scalability / Design Decisions
- **Scoring weight calibration**: weights calibrated offline — simulate different weight combinations on historical matched trips; minimize `cancellation_rate × pickup_time`; configurable per city to account for urban density differences (dense cities: distance weight higher; sparse: rating weight higher)
- **Tie-breaking with distance**: if two drivers' composite scores are within 0.01, prefer the closer driver: `score += (1.0 / (distance_km + 0.1)) × 0.0001`; prevents arbitrary ordering of equally-scored candidates
- **CAS UPDATE atomicity (no distributed locks)**: driver acceptance races are resolved by the database: two drivers accepting simultaneously → one CAS UPDATE succeeds (0→1 row affected), other fails (0 rows affected) and receives 409; no lock service required
- **Separate scoring from geography**: GEORADIUS returns candidates geographically; scoring is a pure CPU computation (no additional DB reads for the score itself — all inputs fetched in batch before scoring loop); 23,100 scoring computations/s handled by 10 goroutines
- **ETABatch for pickup ETA pre-scoring**: before offering to a driver, the Matching Service calls `ETAService.ComputeETABatch` for all 50 candidates in one RPC; adds pickup ETA context for display (not used in scoring formula, but shown to driver on offer screen)

### Key Differentiator
Driver Matching's uniqueness is its **multi-factor scoring function + fairness system**: while location-based matching is trivial (nearest driver wins), production matching balances distance (60%), rating quality (20%), acceptance behavior (10%), and heading alignment (10%) with a fairness cooldown that prevents monopolization of high-value pickups — all implemented without a distributed lock (CAS UPDATE only) and with per-city configurable parameters enabling geography-specific tuning.

---

## ETA Service

### Unique Functional Requirements
- Sub-millisecond routing on 50 M-node, 100 M-edge road graphs via Contraction Hierarchies (CH) preprocessing
- Two-layer speed model: 7-day rolling historical speed per edge per (hour, day_of_week) + real-time probe-data delta overlay
- ML correction factor (0.9–1.1 multiplier) trained per city; reduces systematic bias from parking time, traffic signals, turn penalties
- ETABatch API: score 50 driver candidates' pickup ETAs in one gRPC call for the Matching Service
- Multi-waypoint routing for Pool/Shared trips (origin → pickup1 → pickup2 → dropoff1 → dropoff2)
- Alternative routes: top 3 options with different ETA/distance tradeoffs for driver app display
- Accuracy monitoring: compare predicted vs. actual trip duration; feed deviation metrics to ML retraining pipeline
- MAPE < 15%, P95 error < 5 minutes for trips under 30 minutes

### Unique Components / Services
- **CH Graph (in-process memory)**: city road graph preprocessed into Contraction Hierarchy; serialized with Apache Arrow format (LZ4 compressed, ~3 GB per large city); loaded from S3 at ETA pod startup (~30 s load time); zero-latency traversal; bidirectional A* over upward-only edges
- **Traffic Ingest Pipeline**: Kafka consumer on `location-updates`; aggregates median driver speed per road edge over 5-minute windows; computes delta = (current_median - historical_baseline); writes to Redis `traffic:{city_id}:edge_speeds` Hash; updates `traffic:{city_id}:updated_edges` Sorted Set for tracking freshness; runs every 5 minutes
- **Historical Speed Aggregator (Flink)**: streaming job consuming GPS history; computes 7-day rolling average speed per edge per (hour_of_day, day_of_week); stores to `road_speed_baselines` PostgreSQL table + Apache Parquet files on S3; used to initialize CH graph edge weights at startup
- **ML Correction Service**: TF Serving or lightweight XGBoost model per city; input features: graph_eta_s, distance_km, time_of_day, day_of_week, weather, event_flag, route_type (HIGHWAY|URBAN|MIXED); output: correction_factor [0.9–1.1]; applied multiplicatively: `final_eta = graph_eta × correction_factor`; inference latency 2 ms p50, 10 ms p99
- **Map Graph Updater**: daily ingests OpenStreetMap (OSM) diffs; merges new roads/closures into current graph; triggers offline CH preprocessing job (hours per city); blue-green deployment of new graph version to ETA pods
- **Accuracy Monitoring**: on trip completion, looks up `eta_accuracy_log` record; computes absolute_error_s and percentage_error; aggregates MAPE by city, route_type, hour; triggers ML model retraining when MAPE exceeds threshold

### Unique Data Model
- **Road graph (in-memory)**:
  - Node: node_id (int64), lat/lng (float32), CH rank (int32)
  - Edge: edge_id, from/to node, distance_m, max_speed_kmh, road_class (MOTORWAY=0 → RESIDENTIAL=7), flags (OneWay, Toll, Ferry, HOV), current_speed_kmh, travel_time_s
  - ShortcutEdge: total_travel_time_s, mid_node_id, via1/via2 edge_id (for path unpacking)
  - Serialization: Apache Arrow binary, LZ4 compression; 50M nodes + 100M edges + 100–150M shortcuts = ~3 GB per city
- **road_speed_baselines** (PostgreSQL): PRIMARY KEY (edge_id, hour_of_day, day_of_week); avg_speed_kmh, sample_count, updated_at; 100M edges × 24h × 7days = 67 GB per city in Parquet
- **eta_accuracy_log** (PostgreSQL + Citus, shard key = city_id): predicted_eta_s, actual_eta_s, absolute_error_s, percentage_error, route_type, had_traffic, graph_algorithm (ASTAR|CH|HYBRID), ml_correction_factor
- **graph_versions** (PostgreSQL): s3_path, ch_built, ch_build_time_s, deployed_at, is_active, build_status (PENDING|BUILDING|READY|FAILED)
- **Redis traffic cache**: `traffic:{city_id}:edge_speeds` Hash (edge_id → speed_kmh, 10 min TTL); `traffic:{city_id}:edge_probe_count` Hash; `traffic:{city_id}:updated_edges` Sorted Set (score = unix timestamp)
- **Dynamic weight formula**: effective_speed = max(5 km/h, baseline_speed[edge][hour][dow] + delta[edge]); travel_time_s = distance_m / (effective_speed × 1000/3600)

### Unique Scalability / Design Decisions
- **Contraction Hierarchies (CH) vs. plain A\***: plain Dijkstra on 50M nodes = 100 ms+ per query; CH preprocessing adds shortcut edges allowing bidirectional A* to only traverse upward-rank edges, reducing search space from millions to thousands of nodes; query time < 1 ms (long distance), < 5 ms (urban); makes 27,770 q/s with ~84 CPU cores feasible
- **Stateful ETA pods (city-scoped, consistent-hash routed)**: CH graph can't be shared across pods (too large for Redis); each pod loads specific cities; consistent hash at load balancer ensures all queries for city_id=1 route to the same pod set (warm graph cache); trade-off is reduced deployment flexibility vs. stateless
- **ETABatch API (one RPC = 50 ETAs)**: Matching Service needs ETA for all 50 candidates before scoring; if ETAs were computed serially (50 separate RPCs) at 20 ms each = 1 s overhead; batch collapses to one network round-trip + parallel CPU computation (~20 ms total)
- **Layered speed data (historical + real-time)**: historical 7-day patterns handle normal daily variation (rush hour, night); real-time probe delta handles accidents/events; fallback: if no probe data for an edge (< 3 probes in 5 min), use historical baseline — prevents noisy single-car speed from corrupting weights
- **ML correction post-CH query (not pre)**: CH graph is precomputed with historical speeds; ML correction is applied after graph ETA is computed; allows graph to remain static between graph rebuilds while ML adapts to systematic biases (parking structure exit delays, stop sign density not in OSM)
- **Daily graph rebuild (blue-green)**: new CH graph built offline from OSM diffs + updated baselines; when READY, traffic shifted to new pod set with new graph; old pods drained gracefully; ensures graph reflects road network changes without downtime

### Key Differentiator
ETA Service's uniqueness is the **Contraction Hierarchies algorithm on a 50M-node in-memory road graph**: CH is a graph acceleration technique (preprocessing + bidirectional A* on upward edges only) that achieves < 1 ms query latency — physically impossible with raw Dijkstra. Layered on top: 7-day historical speed patterns (Flink rolling aggregation) + real-time probe delta (5-min Kafka → Redis pipeline) + ML post-correction (XGBoost per city) creates a three-layer accuracy stack. No other problem in this folder has graph algorithms, ML inference, or the statefulness constraint (graph-in-memory pods).
