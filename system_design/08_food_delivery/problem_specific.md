# Problem-Specific Design — Food Delivery (08_food_delivery)

## DoorDash (Core Platform)

### Unique Functional Requirements
- Three-sided marketplace: consumers (ordering), merchants (fulfilling), Dashers (delivering)
- Order lifecycle with 11+ states: pending_payment → placed → merchant_accepted → preparing → ready_for_pickup → dasher_assigned → dasher_en_route_to_merchant → dasher_at_merchant → dasher_picked_up → dasher_en_route_to_consumer → delivered (+ cancelled from most states with compensation)
- Dispatch: match unassigned orders to available Dashers with multi-factor scoring + cascading offers (60-second timeout per Dasher before moving to next candidate)
- Surge pricing: demand/supply ratio per H3 hexagon → delivery fee multiplier (capped by city regulation)
- ETA formula: `prep_time_estimate + dasher_travel_to_merchant + service_time_at_merchant + dasher_travel_to_consumer`
- Merchant-side: pause/resume accepting orders, mark items unavailable, confirm/reject incoming orders

### Unique Components / Services
- **Order Service**: owns the trip state machine; writes state transitions, history row, and outbox row atomically in one PostgreSQL transaction; Debezium reads WAL → publishes to Kafka `order-events`; SERIALIZABLE isolation for state changes
- **Dispatch Service**: Kafka consumer group partitioned by merchant_id; queries Redis GEORADIUS (20 candidates, 5 km, < 5 ms); applies ONNX scoring model inline (sub-millisecond); inserts `dispatch_offers` record with `expires_at = now + 60 s`; Quartz scheduler expires offers every 5 s and cascades to next candidate; radius expansion: 5 km → 10 km → 20 km after 3 failed rounds (escalates to ops)
- **Pricing Service**: computes surge multiplier per H3 resolution-7 cell every 60 s using `online_dashers_within_2km / pending_orders_within_2km` ratio; piecewise: ratio < 0.4 → 2.0×, 0.4–0.6 → 1.5×, 0.6–0.8 → 1.2×; writes results to Redis HSET (60 s TTL)
- **ETA Service (GBM + OSRM)**:
  - Prep time: GBM model trained on (merchant_id, time_of_day, day_of_week, order_size, current_order_queue_depth) → predicted prep minutes; retrained weekly; served via Feature Store (Redis-backed offline Spark training)
  - Drive time: self-hosted OSRM (< 10 ms per call); 48K calls/s budget, 24-node cluster (r5.2xlarge, ~$840/month vs. Google Maps ~$20M/month)
  - p75 prep estimate at checkout (conservative); p50 in app post-acceptance (optimistic)
  - Rate limited: 1 ETA recalculation per 15 s per order via Redis rate-limit key

### Unique Data Model
- **orders** (PostgreSQL, sharded by merchant_id via CitusDB): order_id, consumer_id, merchant_id, dasher_id, status (ENUM 11 states), idempotency_key (UNIQUE), surge_multiplier NUMERIC(4,2), payment_intent_id (Stripe), pickup_h3_index (H3 resolution 7 for spatial analytics)
- **order_status_history**: history_id, order_id, from_status, to_status, actor (consumer/merchant/dasher/system), occurred_at — append-only audit log
- **dispatch_offers**: offer_id, attempt_id, dasher_id, trip_id, offered_at, expires_at, outcome (ACCEPTED|DECLINED|TIMEOUT), composite_score, score_breakdown JSONB
- **merchant_hours** + **menu_categories** + **menu_items** + **item_modifiers**: full restaurant catalog; PostgreSQL source of truth; ES is derived
- **payments**: payment_intent_id (Stripe reference), idempotency_key, amount, refund_amount, status
- **Cassandra `dasher_location_history`**: partition key = dasher_id; clustering by recorded_at DESC; TTL 30 days; RF=3, LOCAL_QUORUM

### Unique Scalability / Design Decisions
- **Dispatch scoring ONNX model inline**: dispatching 1,710 evaluations/s (171 peak orders × 10 candidates); ONNX runtime in Dispatch Service process (no RPC) achieves sub-millisecond per candidate; avoids latency of external ML service call per evaluation
- **Quartz scheduler for offer expiration**: cascading offers use a scheduled job (every 5 s) to expire timed-out offers and move to the next candidate — simpler than long timeouts or distributed timers; accepts up to 5 s grace beyond the 60 s timeout
- **Merchant-id as Kafka partition key for dispatch-events**: ensures FIFO ordering of offer decisions per restaurant; prevents race conditions where two Dashers receive offers for the same order simultaneously
- **CitusDB sharding by merchant_id**: all order data for a merchant is co-located; enables efficient `WHERE merchant_id = X ORDER BY created_at DESC` queries for the merchant app dashboard; cross-restaurant joins are not needed

### Key Differentiator
DoorDash's uniqueness is the **three-sided marketplace orchestration**: unlike ride-sharing (two parties) or standalone search, DoorDash simultaneously manages consumer ordering, merchant kitchen state (preparing/ready/paused), and Dasher dispatch — with three separate Kafka consumer groups (Order Service, Dispatch Service, Notification Service) each independently processing the same order-events stream and a GBM-based prep time model that distinguishes DoorDash from simpler distance-only ETAs.

---

## Menu Search

### Unique Functional Requirements
- Restaurant search with geographic delivery radius filtering (each restaurant has a configurable `delivery_radius_km`)
- Dietary flag filtering: vegan, vegetarian, gluten-free, halal, kosher, nut-free (all as keyword arrays on ES documents)
- Relevance ranking optimized for CTR and order conversion (not just text relevance)
- Autocomplete with < 100 ms p99 latency at 2,565 requests/s
- Real-time index freshness: menu availability changes visible in search within 30 s
- Restaurant pause: when merchant pauses accepting orders, immediately hidden from search results

### Unique Components / Services
- **Two-Stage Ranking Pipeline**:
  - Stage 1 (Elasticsearch `function_score`): BM25 text relevance on (name^3, description^1, cuisine_types^2) + gauss decay on distance (scale=2km, decay=0.5, weight=2.0) + log1p on rating (factor=1.2, weight=1.5) + log1p on review_count (factor=0.01, weight=0.5) + log1p on popularity_score (weight=1.0); returns top 50 candidates; latency 20–40 ms
  - Stage 2 (Personalization re-blend in Search Service, post-ES): `final_score = 0.7 × es_score + 0.2 × cuisine_affinity + 0.1 × dietary_match`; cuisine_affinity from Redis `consumer_prefs:{consumer_id}` Hash (24 h TTL, nightly ML pipeline); fallback: skip stage 2 if Redis unavailable; p-NDCG@10: 0.73 (vs. 0.52 BM25-only, 0.68 function_score-only)
- **Search Indexer** (Kafka consumer): 50 partitions by merchant_id; dedup via Redis `indexed_event:{event_id}` (10-min TTL); ES partial `_update` doc `{ "is_available": false }`; publishes cache invalidation event to Redis Pub/Sub and CDN PURGE; retry 3× with exponential backoff (100 ms, 200 ms, 400 ms) → dead-letter on failure
- **Autocomplete Service (Hybrid)**:
  - Primary: ES Completion Suggester (FST-based, < 15 ms); contexts = H3 resolution-6 cell for geo-filtering; weights = 7-day rolling click count from search_events
  - Redis geo-cell cache: `suggest:{h3_cell_r6}:{prefix}` ZSET (60 s TTL) reduces ES load by ~80%
  - Top-1000 prefix ZSET: nightly refresh from search_events; < 1 ms cache hit
  - Client debounce: 150 ms idle typing before firing request
- **Personalization Service**: reads `consumer_prefs:{consumer_id}` from Redis (nightly ML pipeline writes cuisine affinities and dietary preference vectors); < 2 ms latency; score blend computed in-process within Search Service

### Unique Data Model
- **restaurant_index** (ES, 5 primary shards + 1 replica): merchant_id (keyword), name (text + keyword, english analyzer), cuisine_types (keyword), dietary_flags (keyword), location (geo_point), delivery_radius_km (float), rating (float), review_count (integer), price_tier (byte, 1–4), prep_time_min (integer), delivery_fee_cents (integer), is_active (boolean), is_paused (boolean), open_now (boolean), popularity_score (float), suggest (completion field with FST, max_input_length=50)
- **menu_item_index** (ES, 10 primary shards + 1 replica, routing by merchant_id): item_id, merchant_id (routing key), merchant_name, merchant_location (geo_point), item_name (text), description (text), price_cents, dietary_flags (keyword), is_available (boolean), calorie_count, popularity_score, category_name; routing by merchant_id ensures single-shard queries for all items of a restaurant
- **autocomplete_index** (ES, 3 primary shards + 1 replica): completion suggester field with H3-6 geo contexts, weight from 7-day click count
- **consumers_search_preferences** (PostgreSQL): consumer_id, cuisine_affinities (JSONB), dietary_preferences (TEXT[])
- **search_events** (PostgreSQL, partitioned monthly, 90-day hot retention): append-only log for ML training; used to compute 7-day rolling click counts for autocomplete weights

### Unique Scalability / Design Decisions
- **Custom routing by merchant_id on menu_item_index**: by default, a query for items of one restaurant would fan out to all 10 shards (10 parallel sub-queries, merge overhead); with `_routing=merchant_id`, ES sends the query to exactly 1 shard that holds all items for that merchant — reduces latency and cluster load by ~10×
- **Two-stage ranking separates expensive personalization from ES**: ES handles large-scale text + geo + boost filtering (50 candidates, 20–40 ms); in-process Redis lookup for personalization re-ranks only 50 results in < 2 ms; total latency budget: 40 + 2 + 10 (network) < 100 ms target
- **CDN Surrogate-Key purge for near-instant invalidation**: menu and restaurant pages are cached with `Surrogate-Key: merchant:{id}` header; on merchant pause, a single CDN PURGE API call with that key invalidates all cached responses referencing that merchant globally in ~5 s — avoids waiting for TTL expiry
- **Reconciliation job every 5 minutes**: ES is eventual-consistency; if a Kafka event is dropped or indexer crashes, items would be stale; the reconciliation job queries PostgreSQL for items updated in last 5 min and idempotently re-upserts them to ES — bounded staleness of 5 min even in failure scenarios
- **function_score with score_mode=sum, boost_mode=multiply**: BM25 relevance score is multiplied by the sum of all function scores; this ensures that a restaurant irrelevant to the query text never ranks at the top just because it's nearby and highly rated — text relevance gates the result

### Key Differentiator
Menu Search's uniqueness is its **two-stage ranking pipeline and multi-layer autocomplete caching**: the function_score ES query handles geographic + dietary + text matching; personalization (cuisine affinity + dietary preference) is blended post-ES for < 2 ms overhead; autocomplete uses a hybrid ES Completion Suggester (FST, geo-contexted) + Redis ZSET geo-cell cache (80% load reduction) + nightly top-prefix cache — no other problem in this folder requires this level of search sophistication.

---

## Order Tracking

### Unique Functional Requirements
- 1 million simultaneously tracked orders at peak (360K simultaneous at steady state, 800K WebSocket connections)
- Live Dasher location pushed to consumer every 4 seconds with < 5 s E2E latency
- Client-side dead-reckoning for smooth animation between 4 s server updates (30+ FPS rendering)
- Status push notifications with priority classification (CRITICAL/HIGH/INFO) and multi-language templates per locale
- Dasher-approaching geofence: single notification when Dasher is within 500 m of delivery address
- Multi-device support: one user may have multiple active WebSocket sessions (phone + tablet)
- Circuit breaker on APNs/FCM with SMS (Twilio) fallback

### Unique Components / Services
- **WebSocket Gateway**: stateful — each gateway node holds up to 125K persistent WebSocket connections; JWT validation on connect; local in-memory session registry (`{ connection_id → (order_id, user_id, role) }`, ~100 ns lookup); 800K connections / 125K per node = ~8 nodes at peak; round-robin LB for initial connect; Redis `ws:node:{node_id}` Set heartbeat (30 s TTL)
- **Location Fan-out Service**: Kafka consumer on `location-events`; for each GPS ping, publishes `PUBLISH tracking:{order_id} {lat, lng, heading, eta_min}` to Redis; all WS Gateway nodes receive via pattern subscribe `tracking:*`; each node checks local session registry and pushes to relevant connections — no central routing table; scales to N nodes without fan-out bottleneck (until Redis Pub/Sub limit ~1M msg/s per cluster)
- **ETA Recalc Service**: Kafka consumer on `location-events`; rate-limited to 1 recalc/15 s per order via Redis SETEX; calls OSRM with dasher's current position; two-leg formula (pre-pickup: leg1 + remaining_prep + leg2; post-pickup: leg1 only); stores to `order:state:{order_id}` HASH; publishes to `eta-update-events` Kafka topic
- **Notification Service**: consumes `notification-triggers`; fetches notification_preferences (Redis-cached, 5-min TTL) and device_tokens (Redis-cached, 1-h TTL); dedup via Redis SET NX (300 s TTL); sends via APNs HTTP/2 (priority 10 = CRITICAL, 5 = normal) or FCM HTTP v1 (priority: "high"); circuit breaker (10 failures/30 s → open 60 s → SMS fallback); logs all outcomes to Cassandra `notifications` table; retry 3× with backoff (1 s, 2 s, 4 s) → dead-letter `notification-dlq`

### Unique Data Model
- **WebSocket session registry (Redis)**:
  - `ws:session:{session_id}` Hash (user_id, order_id, role, node_id, connected_at; TTL 2 h)
  - `ws:user:{user_id}` Set of session_ids (TTL 2 h; multi-device support)
  - `ws:order:{order_id}` Set of session_ids of all parties tracking that order (TTL 2 h)
  - `ws:node:{node_id}` Set of session_ids on that gateway node (TTL 30 s; heartbeat refreshed)
- **order_status_history** (PostgreSQL): history_id, order_id, from_status, to_status, actor_id, actor_role, notes, occurred_at; indexed on (order_id, occurred_at ASC); full timeline for the order detail screen
- **device_tokens** (PostgreSQL): token_id, user_id, platform (ios/android/web), device_token, is_active, registered_at, last_used_at; UNIQUE (user_id, device_token); invalidated when APNs/FCM returns "Unregistered" or "BadDeviceToken"
- **notification_preferences** (PostgreSQL): per-user opt-in per channel (push/sms/email) and per event type (placed, dasher_assigned, picked_up, delivered, promotions)
- **notifications** (Cassandra): PRIMARY KEY ((recipient_id), sent_at DESC, notification_id); TTL 90 days; delivery_status (sent/delivered/failed); audit log for retry analysis and SLA reporting
- **Dasher approaching geofence**: haversine check per GPS ping: `haversine(dasher_lat, dasher_lng, delivery_lat, delivery_lng) < 500 m`; fires at most once per order (dedup via notification dedup key)

### Unique Scalability / Design Decisions
- **Redis Pub/Sub fan-out with local session filter**: centralized fan-out service would need to track which gateway node holds which session — O(N) mapping at scale; instead, all gateway nodes subscribe to `tracking:*` pattern; each node filters locally against its in-memory session map (< 1 µs check); trade-off is slightly higher Redis Pub/Sub message volume (sent to all nodes, not just relevant ones), but Redis handles ~1M msg/s per cluster — sufficient for 63K fan-out pushes/s
- **Client-side dead-reckoning (30+ FPS from 4-second updates)**: server sends Dasher position every 4 s; client predicts intermediate positions: `predicted_lat = last_lat + (speed × cos(heading) × elapsed_time) / EARTH_RADIUS`; renders at 30+ FPS between updates; snaps to actual position if predicted diverges > 50 m (prevents visual drift on turns)
- **Multi-device WebSocket sessions**: `ws:user:{user_id}` is a Set; on status event, iterate all session_ids for the user and push to each connected device; enables consumer tracking on phone and tablet simultaneously
- **APNs/FCM circuit breaker + SMS fallback**: if APNs/FCM is failing (10 failures/30 s → open 60 s), CRITICAL notifications (dasher_assigned, delivered) fall back to SMS (Twilio, ~$0.0075/SMS) to ensure delivery SLA; non-CRITICAL events are dropped to avoid SMS cost; circuit restores automatically after 60 s
- **Multi-language notification templates**: `notification_templates` PostgreSQL table keyed by (notification_type, locale); consumer locale from JWT claims; template variables rendered at runtime (`{dasher_name}`, `{merchant_name}`, `{eta_min}`); supports 20+ locales without code changes

### Key Differentiator
Order Tracking's uniqueness is its **fan-out architecture at 1M simultaneous orders**: the Redis Pub/Sub pattern-subscribe design with local session filtering on each gateway node avoids a central routing service bottleneck; combined with client-side dead-reckoning (30+ FPS animation from 4 s updates), priority-classified notifications with circuit breakers, and multi-device session management via Redis Sets — the full stack is purpose-built for the sub-5-second E2E latency requirement across 800K concurrent WebSocket connections.
