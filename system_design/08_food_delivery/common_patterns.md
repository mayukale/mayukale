# Common Patterns — Food Delivery (08_food_delivery)

## Common Components

### Redis for Hot-Path Operational State
- All three problems use Redis as the primary hot-path store for current operational state with explicit short TTLs
- In doordash: `dasher:pos:{dasher_id}` String/Hash (TTL 10 s — GPS position overwritten each ping); `order:state:{order_id}` Hash (TTL 4 h — status, eta_min, lat/lng for all parties); `dasher:online` Geo Sorted Set (GEORADIUS for dispatch); `dasher:profile:{dasher_id}` Hash (30 s TTL); surge multipliers per H3 cell (60 s TTL); cart Hash per consumer (24 h TTL)
- In menu_search: `menu:{merchant_id}` full menu cache (60 s TTL); `suggest:{h3_cell_r6}:{prefix}` ZSET for autocomplete (60 s TTL); `consumer_prefs:{consumer_id}` Hash (24 h TTL); CDN cache keys keyed by (geo_tile_h3_r7, cuisine_hash, dietary_hash)
- In order_tracking: `ws:session:{session_id}` Hash (2 h TTL); `ws:order:{order_id}` Set of session_ids (2 h TTL); `order:state:{order_id}` Hash (4 h TTL); `dasher:pos:{dasher_id}` (10 s TTL); `eta_recalc_ts:{order_id}` rate-limit key (15 s TTL); `notif_dedup:{recipient_id}:{order_id}:{type}` (300 s TTL)

### Kafka for High-Throughput Event Streaming
- All three problems use Kafka as the durable async event bus with RF=3, ISR=2, producer idempotence, and at-least-once delivery guarantees
- In doordash: topics `order-events` (by order_id), `location-events` (by order_id), `dispatch-events` (by dasher_id), `notification-events` (by recipient_id); PostgreSQL transactional outbox + Debezium CDC reads WAL and publishes reliably
- In menu_search: `menu-change-events` (partitioned by merchant_id, 50 partitions); `search-events-raw` (ML pipeline input); `menu-change-events-dlq` (dead-letter after 3 retry failures)
- In order_tracking: `location-events` (50 partitions, by order_id, 125K writes/s peak); `order-status-events` (by order_id); `notification-triggers` (by recipient_id); `eta-update-events` (50 partitions, by order_id)

### PostgreSQL (Aurora) as Source of Truth
- All three use PostgreSQL Aurora as the ACID-consistent primary store for orders, users, merchants, and events; Elasticsearch and Redis are derived/caches
- In doordash: Aurora PostgreSQL Multi-AZ, automatic failover < 30 s, Aurora Global Database for cross-region read replicas with < 1 s replication lag; orders table uses `idempotency_key` UNIQUE constraint; `SERIALIZABLE` isolation for state transitions, `READ COMMITTED` for reads
- In menu_search: PostgreSQL is source of truth for menu catalog; Elasticsearch is eventual-consistency derived; `search_events` table partitioned monthly for ML training (90-day hot, 1-year S3)
- In order_tracking: `order_status_history` (full transition log), `device_tokens` (APNs/FCM registry), `notification_preferences` (per-user opt-in per channel+event); 1 writer + 2 read replicas; writes to writer, reads routed to replicas

### Cassandra for High-Write Time-Series with TTL
- Two of the three use Cassandra (RF=3, LOCAL_QUORUM) for append-only time-series data with per-row TTL and no cleanup job needed
- In doordash: `dasher_location_history` table; partition key = dasher_id, clustering by recorded_at DESC; TTL 30 days; enables dispute resolution and ETA model training
- In order_tracking: `dasher_location_history` (30-day TTL); `notifications` table (partition by recipient_id, clustering by sent_at DESC; TTL 90 days) for full audit log of push notification outcomes
- In menu_search: no Cassandra; PostgreSQL partitioned monthly tables serve the time-series need (lower write volume)

### H3 Hexagonal Grid for Geographic Aggregation
- All three use H3 hexagons for geographic bucketing, caching, and supply/demand calculation
- In doordash: H3 resolution 7 (~1.2 km² cells) for surge pricing — `online_dashers_within_2km / pending_orders_within_2km` per cell → piecewise multiplier (< 0.4 → 2.0×, 0.4–0.6 → 1.5×, 0.6–0.8 → 1.2×); computed every 60 s
- In menu_search: H3 resolution 6 (~36 km² cells) for autocomplete geo-cell caching (`suggest:{h3_cell_r6}:{prefix}` ZSET, 60 s TTL) and Elasticsearch Completion Suggester contexts (geo-filtering at H3-6 granularity); H3 resolution 7 for CDN cache key segmentation by geo-tile
- In order_tracking: haversine-based geofence (500 m circle) for "Dasher approaching" notification; not full H3 but same spatial partitioning philosophy

### OSRM (Self-Hosted Routing) for Drive Time Calculation
- Two of the three use self-hosted OSRM for route calculation (latency < 10 ms per call); chosen over Google Maps API for cost reasons (~$840/month OSRM vs. ~$20M/month Google Maps API at peak)
- In doordash: OSRM for pickup ETA (dasher → merchant) + delivery ETA (merchant → consumer); 48,000 calls/s budget; 24-node OSRM cluster (r5.2xlarge, 2K calls/s per node)
- In order_tracking: same OSRM cluster; 24,000 ETA recalculations/s (rate-limited to 1/15 s per order from 125K location pings); budget: 24,000 orders/15 s = 24K calls/s for single-leg recalcs
- In menu_search: no routing; delivery fee and ETA estimates come from precomputed lookup tables or the Pricing Service

### Elasticsearch for Restaurant/Menu Search
- Two of the three use Elasticsearch for restaurant and menu item search with function_score relevance ranking
- In doordash: `restaurant_index` (5 primary shards + 1 replica) and `menu_item_index` (10 primary shards + 1 replica); uses `function_score` with BM25 + decay functions on distance/rating + `field_value_factor` with log1p modifier; geo_point field for restaurant location; is_paused and is_active filters
- In menu_search: same indices with detailed mapping; routing by `merchant_id` on menu_item_index (all items for a restaurant on one shard → single-shard query instead of 10-shard fan-out); `autocomplete_index` with completion suggester (FST-based)
- In order_tracking: Elasticsearch not used for order tracking (pure Redis/WebSocket/Kafka)

### CDN with Surrogate-Key Purge for Menu/Restaurant Caching
- Two of the three cache public restaurant and menu data at CDN edge with Surrogate-Key invalidation for near-instant purge on merchant action
- In doordash: CDN (CloudFront/Cloudflare) for restaurant list (30 s TTL) and menu pages (60 s TTL); Surrogate-Key PURGE on merchant pause/resume or item availability change; propagation ~5 s globally
- In menu_search: identical pattern; `s-maxage=30, stale-while-revalidate=60` headers; CDN purge via API with `Surrogate-Key merchant:{id}`; combined with Redis menu cache invalidation via Pub/Sub
- In order_tracking: CDN for terminal order status history (1 h TTL) — read-heavy once order is complete

### Transactional Outbox Pattern (Debezium CDC → Kafka)
- Two of the three use the transactional outbox pattern to guarantee exactly-once Kafka publishing from PostgreSQL
- In doordash: PostgreSQL `outbox` table; each state transition writes (status change + history row + outbox row) in one atomic PostgreSQL transaction; Debezium reads PostgreSQL WAL and publishes to Kafka with at-least-once delivery
- In menu_search: Menu Service writes PostgreSQL + publishes to Kafka `menu-change-events` (effectively outbox pattern); Search Indexer deduplicates via Redis `indexed_event:{event_id}` (10-min TTL)
- In order_tracking: Order Service publishes to `order-status-events` Kafka topic; Notification Service deduplicates via Redis SET NX on (recipient_id, order_id, type)

### Push Notifications (APNs / FCM / SMS)
- All three involve push notifications delivered via APNs (iOS), FCM (Android), and SMS (Twilio) as fallback
- In doordash: Notification Service consumes `notification-events` Kafka topic; triggers APNs/FCM/SMS; stores results
- In menu_search: not a real-time notification use case (search is synchronous)
- In order_tracking: full pipeline — priority classification (CRITICAL/HIGH/INFO); fetch device_tokens from PostgreSQL (Redis-cached, 1 h TTL); dedup via Redis SET NX (5-min window); APNs HTTP/2 (apns-priority: 10 or 5); FCM HTTP v1 (priority: "high" for CRITICAL); circuit breaker (10 failures/30 s → open 60 s → SMS fallback via Twilio); dead-letter queue on 3 failed retries; outcome logged to Cassandra `notifications` table

## Common Databases

### Redis Cluster
- Used in all three for current position, session state, caches, rate limits, deduplication keys, and Pub/Sub for cross-node fan-out
- Single cluster with 16,384 hash slots; each master has 1 replica; ~1M messages/s Pub/Sub throughput

### PostgreSQL Aurora
- Used in all three as the ACID-consistent source of truth; read replicas for read offload; Multi-AZ for < 30 s failover

### Cassandra
- Used in two (doordash, order_tracking) for append-only high-write time-series with TTL (location history, notification log); RF=3, LOCAL_QUORUM

### Elasticsearch
- Used in two (doordash, menu_search) for restaurant and menu item search with BM25 + function_score ranking

## Common Queues / Event Streams

### Kafka (See Common Components above)
- RF=3, ISR=2, `acks=all`, producer idempotence enabled across all problems
- Partition count scaled pre-event (24 h before major events like Super Bowl)

## Common Communication Patterns

### WebSocket for Real-Time Consumer Map Updates
- Consumer tracking is delivered via WebSocket (not polling) for < 5 s E2E latency
- In doordash: WebSocket push of Dasher location every 4 s; 200,000 location reads/s from consumer tracking
- In order_tracking: 800K concurrent WebSocket connections at peak; Redis Pub/Sub pattern subscribe `tracking:*` for cross-node fan-out; each WS Gateway node filters locally; HTTP polling fallback for clients that can't maintain WebSocket

## Common Scalability Techniques

### ETA Rate Limiting (1 Recalculation per 15 Seconds per Order)
- Reduces 125K GPS writes/s to ~24K ETA recalculations/s; critical for OSRM cluster sizing
- Redis key `eta_recalc_ts:{order_id}` with 15 s TTL: if key exists, skip recalculation; if expired/absent, recalculate and reset key
- Present in: doordash, order_tracking

### Idempotent Event Processing (Redis Dedup Keys)
- All three use Redis SET NX or UNIQUE constraint mechanisms to prevent duplicate processing
- Menu search indexer: `indexed_event:{event_id}` (10-min TTL) prevents re-indexing on Kafka message replay
- Order tracking notifications: `notif_dedup:{recipient_id}:{order_id}:{type}` (300 s TTL) prevents duplicate pushes
- Order placement: `idempotency_key` UNIQUE constraint + Redis cache (24 h TTL) for Stripe PaymentIntent

### Reconciliation Jobs for Eventual Consistency
- Both doordash and menu_search run periodic reconciliation jobs to catch missed events
- In menu_search: background job every 5 minutes queries PostgreSQL `WHERE updated_at > now - 5 min`; idempotent ES upserts to heal any missed Kafka events
- In doordash: similar reconciliation for order state consistency between PostgreSQL and Redis

### Stateless Services Behind Kubernetes HPA
- All three services are stateless (except WebSocket gateways) and auto-scale on RPS or queue depth
- Order Service: 6 pods steady state, 10 at peak (HPA on RPS, target 100 req/pod)
- Location Service: ~65 pods at peak (125K writes/s ÷ 2K per pod)
- ETA Recalc Service: ~48 pods at peak (24K OSRM calls/s ÷ 500 calls/s per pod)

## Common Deep Dive Questions

### How do you track 125,000 Dasher GPS writes per second?
Answer: Two-tier write path. Synchronous: update Redis `dasher:pos:{dasher_id}` (10 s TTL, < 1 ms) and Redis Geo ZSET `dasher:online` for dispatch queries — this satisfies the real-time matching and rider tracking needs. Async: publish to Kafka `location-events` (partitioned by order_id for FIFO ordering). Kafka consumers: (1) Cassandra Consumer — batch inserts 500 rows at a time with RF=3, LOCAL_QUORUM, TTL 30 days; (2) WebSocket Dispatcher — fans out to consumer clients; (3) ETA Recalc Service — rate-limited to 1 recalc/15 s per order.
Present in: doordash, order_tracking

### How do you keep restaurant and menu availability fresh in search within 30 seconds?
Answer: Kafka change event pipeline. When a merchant pauses or marks an item unavailable: (1) Menu Service writes to PostgreSQL; (2) publishes `menu-change-events` to Kafka (partitioned by merchant_id); (3) Search Indexer consumes, deduplicates via Redis event ID cache (10-min TTL), issues ES partial `_update` (< 100 ms); (4) Elasticsearch refreshes its in-memory segment every 1 s (tunable to 500 ms); (5) Cache invalidation: Redis Pub/Sub busts `menu:{merchant_id}` (60 s TTL); CDN Surrogate-Key PURGE propagates globally in ~5 s. Total E2E: Kafka lag (< 1 s) + ES update (< 100 ms) + ES refresh (1 s) = ~2–3 s, well within 30 s SLA. Reconciliation job runs every 5 min to catch any missed events.
Present in: doordash, menu_search

### How do you fan out a Dasher location update to all consumers tracking that order without overwhelming the system?
Answer: Redis Pub/Sub with local session filtering on each WebSocket Gateway node. Location Fan-out Service publishes `PUBLISH tracking:{order_id} {lat,lng,heading,eta_min}` to Redis. All WS Gateway nodes are subscribed to the `tracking:*` pattern. Each node checks its LOCAL in-memory session registry for sessions subscribed to that order_id; if any found, push via WebSocket. If none, discard silently (O(1) local check). This avoids a central routing table and scales to N gateway nodes without adding per-node fan-out cost.
Present in: doordash, order_tracking

### How do you prevent duplicate push notifications on status changes?
Answer: Redis SET NX deduplication. On each status transition, Notification Service issues `SET NX notif_dedup:{recipient_id}:{order_id}:{notification_type} 1 EX 300`. If the key already exists (another instance already processed this event), skip sending. If it doesn't exist, SET NX succeeds → send notification → log outcome to Cassandra. The 5-minute (300 s) TTL ensures that genuine re-notifications (e.g., same status change fired twice from Kafka at-least-once delivery) are suppressed, while notifications for different events (different notification_type) are never blocked.
Present in: order_tracking

## Common NFRs

- **Location write ACK**: p99 < 100 ms (Dasher app must receive acknowledgment quickly)
- **Consumer location update E2E**: p99 < 5 s from GPS event to consumer screen
- **Order placement**: p99 < 500 ms
- **Push notification delivery**: p99 < 10 s from status event to device
- **Menu search**: p99 < 200 ms (restaurant list), p99 < 100 ms (autocomplete)
- **Index freshness**: ≤ 30 s for menu availability changes
- **ETA accuracy**: ±5 min at p80 (order tracking), ±3 min at p80 (dispatch phase)
- **Availability**: 99.99% for order placement and notifications; 99.9% for real-time map stream
- **Durability**: Location history 30 days; notification log 90 days; order records 3 years
