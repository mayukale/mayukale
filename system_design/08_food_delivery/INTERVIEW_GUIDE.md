# Pattern 8: Food Delivery — Interview Study Guide

Reading Pattern 8: Food Delivery — 3 problems, 8 shared components

**Problems covered:** DoorDash (full three-sided marketplace), Menu & Restaurant Search, Real-Time Order Tracking

**Shared components:** Redis Cluster, Kafka, PostgreSQL Aurora, Cassandra, H3 Hexagonal Grid, OSRM (self-hosted routing), Elasticsearch, Transactional Outbox (Debezium CDC)

---

## STEP 1 — ORIENTATION

Food delivery is one of the richest system design domains because it combines three hard sub-problems that each appear in other interview contexts: a real-time marketplace matching supply to demand (like ride-sharing), a geospatial search engine with complex relevance ranking (like Yelp or Google Maps), and a high-throughput event streaming and notification system (like a live sports app). What makes it distinctive is that all three of these sub-problems are tightly coupled and must work together in under two seconds for a single user action — placing an order.

The three problems in this folder build on each other. DoorDash is the complete platform design — it covers every component. Menu Search focuses on the discovery layer: how do 37 million consumers find restaurants and dishes efficiently? Order Tracking focuses on the delivery layer: once the order is placed, how does a consumer watch their food move across a map in real time? All three share the same underlying infrastructure choices and reinforce why those choices were made.

**What interviewers are really testing in this domain:**
- Can you manage three different actors (consumer, merchant, Dasher) with conflicting latency and consistency needs?
- Do you know why certain components (Cassandra for location, Redis for dispatch, WebSocket for tracking) exist and what breaks without them?
- Can you reason about the difference between 57 orders/second at average load and 125,000 GPS writes/second at peak — and design storage and compute tiers appropriately?

---

## STEP 2 — MENTAL MODEL

**Core idea:** A food delivery platform is a **three-sided real-time coordination system** that must simultaneously move state (an order) through a deterministic state machine while broadcasting that state to three different actors, each with different latency tolerances, and while matching supply (Dashers) to demand (orders) under tight time constraints.

**Real-world analogy:** Think of an air traffic control tower, but instead of one controller, you have three: one who handles passenger check-in and boarding (Order Service), one who dispatches pilots to planes (Dispatch Service), and one who broadcasts live flight positions to a giant tracking screen (Location and Notification Services). All three are watching the same flight (order) and all three must see consistent state. If any one of them acts on stale state, a real-world disaster happens — a Dasher picks up an already-cancelled order, a consumer pays twice, or a merchant makes food that was already refunded.

**Why it is hard:**

1. **Scale mismatch within a single system.** An order placement happens at 57/second on average. The GPS writes that power tracking happen at 125,000/second at peak. The same platform must handle both with different storage tiers (PostgreSQL for orders, Redis + Cassandra for location) or it collapses under write load.

2. **Consistency tiers.** Payments must be ACID-consistent (you cannot charge a consumer twice). Order state transitions must be serializable (you cannot assign a cancelled order to a Dasher). But Dasher location can be eventually consistent (being 4 seconds stale on a moving pin is fine). Mixing these consistency requirements in a single system without thinking clearly about which tier applies to which operation is the most common interview mistake.

3. **Three-sided coordination with cascading failures.** If the Dispatch Service is slow, orders pile up waiting for Dashers. If the merchant notification fails, the order goes cold while the kitchen waits. Every component's failure creates a cascading consumer-visible experience problem. Interviewers probe whether you have thought about each failure mode.

4. **Real-time constraints in a web-scale system.** A consumer expects to see their Dasher's location update every 4 seconds on a map. At 252,000 simultaneous tracking sessions, that is 63,000 location pushes per second via WebSocket. Getting that to work without polling, without a central routing bottleneck, and without melting a Redis cluster requires a specific fan-out architecture.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these within the first 3-5 minutes before drawing anything.

**Q1: Which of the three surfaces are we designing — the full platform, just search and discovery, or just real-time tracking?**
What changes: Full platform requires designing the order state machine, dispatch, payment, and ETA. Search focuses on Elasticsearch, ranking, and the index update pipeline. Tracking focuses on WebSocket fan-out and the location ingest pipeline.

**Q2: What scale are we targeting — MVP, current DoorDash scale, or 10x DoorDash?**
What changes: At MVP you can use a single PostgreSQL table and poll for location. At current DoorDash scale you need Cassandra, Redis Geo-index, and Kafka. At 10x you need global distribution and multi-region active-active, which opens a different conversation about conflict resolution.

**Q3: Are we designing for all three actors (consumer, merchant, Dasher) or just one?**
What changes: A consumer-only design skips the Dasher dispatch system, the merchant tablet integration, and the GPS ingest pipeline. If the interviewer says "just the consumer-facing app," you still need to understand what the other actors produce (GPS pings, order accepts), but you can treat those as external inputs.

**Q4: What is the latency target for the order placement critical path?**
What changes: p99 ≤ 500 ms is standard. If the interviewer says 100 ms, you need to pre-authorize payment rather than doing it inline. If they say 2 seconds is fine, you have room for synchronous payment and dispatch in the same request.

**Q5: Is this a global system or US-only?**
What changes: US-only means a single-region Aurora cluster is fine. Global means you need multi-region read replicas, possibly multi-region Kafka, and you need to discuss how timezone-based peak load rolls around the world.

**Red flags to watch for in the interviewer's answers:**
- If they say "no latency constraints," push back — food delivery without latency SLAs is not realistic and suggests they want you to set the requirements.
- If they say "just design it simply," they may be baiting you to skip important components like idempotency keys and the transactional outbox. Show you know the simple version first, then proactively add the production-hardening.

---

### 3b. Functional Requirements

**Core requirements (non-negotiable):**
- Consumer can browse restaurants by location, view menus, add items to a cart, place an order with payment, and receive real-time status updates including live Dasher location on a map.
- Merchant receives incoming orders, can accept or reject, marks items unavailable (86'd) or pauses the restaurant, and sees their order queue in real time.
- Dasher can go online/offline, receives dispatch offers with a ~60-second acceptance window, navigates to the restaurant and consumer, and marks milestones (picked up, delivered).
- Platform enforces an order state machine: placed → merchant accepted → preparing → ready for pickup → Dasher assigned → Dasher en route to merchant → Dasher at merchant → picked up → en route to consumer → delivered (or cancelled from most states with appropriate refund compensation).

**Scope boundaries to state explicitly:**
- Include: payment processing (via tokenized Stripe integration), ETA estimation, surge pricing, push notifications.
- Exclude: the ML model training pipelines, subscription billing internals, turn-by-turn navigation for the Dasher (offloaded to Google Maps SDK), consumer-to-Dasher chat.

**Clear functional statement to open the design:**
"We are building a three-sided food delivery marketplace. The system must let consumers discover restaurants, place orders, and track delivery in real time. It must dispatch Dashers to orders within 2 seconds of merchant acceptance and maintain an accurate ETA throughout the delivery lifecycle. Order state is authoritative and must survive any single component failure."

---

### 3c. Non-Functional Requirements

**Derive these from the use case, do not memorize them as a list:**

**Availability:** Order placement and dispatch must be 99.99% available (≤52 minutes downtime per year) because a restaurant not receiving orders during dinner rush is a business-critical failure. The real-time map stream can tolerate 99.9% because a 4-second gap in Dasher location updates is a minor UX degradation, not a transaction failure.

**Latency:** Order placement p99 ≤ 500 ms (includes payment authorization). Dispatch decision ≤ 2 seconds. Dasher GPS write ACK p99 ≤ 100 ms (Dasher app waits). Consumer location update end-to-end ≤ 5 seconds. Menu search p99 ≤ 200 ms. Autocomplete p99 ≤ 100 ms.

**Consistency:** ✅ Strong consistency for: order state transitions and payments (ACID via PostgreSQL, SERIALIZABLE isolation for transitions). ❌ Eventual consistency acceptable for: Dasher location data (4-second freshness tolerance), menu search index (30-second freshness SLA), surge pricing multipliers (60-second recalculation window).

**Durability:** Zero order loss. Zero payment loss. Location history: 30 days. Notification log: 90 days. Order records: 3 years.

**Scalability:** 3x baseline load during Friday/Saturday dinner (5-9 PM local time, rolling across time zones). 500K active Dashers at peak dinner. 1 million simultaneously tracked orders.

**Trade-offs to state proactively:**
- ✅ Eventual consistency on the search index (30-second window) in exchange for much simpler write path. ❌ If you required immediate search consistency, you would need synchronous ES writes in the merchant API, which couples the write path to ES availability.
- ✅ Cassandra for location history (linear write scalability, TTL-native) in exchange for ❌ no joins, no ACID, and complex data modeling requirements.
- ✅ Redis for hot-path state (sub-millisecond reads) in exchange for ❌ memory cost and the requirement to treat Redis as a cache, not a source of truth.

---

### 3d. Capacity Estimation

**Do this on the whiteboard in about 5 minutes. Show your formula before your numbers.**

**Core anchor: orders per second**
- 37 million MAU consumers × 4 orders/month ÷ 30 days ÷ 86,400 seconds = **~57 orders/second average**
- Peak multiplier 3x (dinner rush) = **~171 orders/second peak**
- Say this clearly: "171 peak orders/second is actually a moderate write rate. PostgreSQL handles this comfortably. The hard part is not the orders — it is the GPS writes."

**The hard number: GPS writes**
- 500K active Dashers at peak × 1 ping per 4 seconds = **125,000 GPS writes/second**
- This immediately forces you to explain why PostgreSQL is NOT used for location data. At 50K writes/second practical PostgreSQL limit under indexing load, 125K writes/second requires Cassandra or a write-optimized store.

**Fan-out math (for order tracking)**
- 57 orders/second × (35 minute average delivery × 60 seconds) = ~120,000 active orders at steady state
- 70% of consumers watching the map = ~84,000 active tracking sessions at steady state
- Peak: 171 × 2,100 seconds = 360,000 active orders × 70% = **252,000 simultaneous tracking sessions at peak**
- Each session receives a push every 4 seconds = **63,000 WebSocket pushes per second**

**Search load**
- 37M MAU × 3 sessions/month × 5 search interactions per session = 555M interactions/month
- 555M ÷ (30 × 86,400) = ~214 search req/second average × 3 peak multiplier × autocomplete factor = **~8,500 total search requests/second at peak**

**Storage anchor: location history**
- 125,000 writes/s × 86,400 s/day × 30 days × 50 bytes/ping = **~16.2 TB for 30-day location history**
- Orders: 4.93M/day × 3 years × 2KB/order = ~10.8 TB

**Architecture implications from these numbers:**
- 125K GPS writes/s → Cassandra, not PostgreSQL
- 252K WebSocket connections → 8 stateful Gateway nodes at 125K connections each (using Go or Node.js with epoll I/O)
- 63K fan-out pushes/s → Redis Pub/Sub (rated at ~1M messages/s per cluster, this fits)
- 8,500 search requests/s → Elasticsearch cluster (comfortably within a 5-shard index at 70M documents)

**Time box:** This section should take 5-7 minutes. Present the formula, the anchor number, and then the "so therefore I need X" architectural implication. Do not recalculate every number — pick the two or three that force architectural decisions.

---

### 3e. High-Level Design

**Draw these 4-6 components in whiteboard order (top to bottom, left to right):**

**1. Clients + API Gateway** (draw first — set the stage)
Three client types: Consumer App (iOS/Android/Web), Merchant Tablet App, Dasher App. All connect through an API Gateway (rate limiting, JWT validation, TLS termination, routing). Additionally, a separate **WebSocket Gateway** for real-time connections — this is stateful and must be drawn separately from the REST API Gateway.

**2. Core Business Services** (draw second — the heart of the system)
- **Order Service**: owns the order state machine. Writes to PostgreSQL inside a transaction, publishes events to Kafka via the transactional outbox. This is the orchestrator.
- **Dispatch Service**: Kafka consumer. When it sees `order.merchant_accepted`, queries Redis Geo-index for nearby Dashers, scores candidates, issues 60-second offers.
- **Location Service**: receives 125K GPS writes/second from Dashers. Writes current position to Redis (TTL 10 seconds) and appends to Cassandra. Publishes to Kafka.
- **Menu/Restaurant Service**: manages menu catalog. PostgreSQL as source of truth. Publishes change events to Kafka. Elasticsearch is a derived, searchable projection.
- **Notification Service**: Kafka consumer. Sends APNs, FCM, SMS.
- **Pricing Service**: computes surge multipliers per H3 hexagon every 60 seconds using supply/demand ratio.

**3. Kafka** (draw as the backbone between services)
Topics: `order-events` (partitioned by order_id), `location-events` (partitioned by order_id for FIFO per delivery), `dispatch-events` (partitioned by dasher_id), `notification-triggers` (partitioned by recipient_id), `menu-change-events` (partitioned by merchant_id).

**4. Data stores** (draw as the bottom tier)
- **PostgreSQL Aurora**: orders, users, merchants, payments (ACID, source of truth)
- **Redis Cluster**: Dasher current position, dispatch geo-index, cart, sessions, caches, ETA, Pub/Sub fan-out
- **Cassandra**: Dasher location history (30-day TTL), notification log (90-day TTL)
- **Elasticsearch**: restaurant and menu item search indexes (derived, eventually consistent)
- **CDN**: restaurant list pages, menu pages, cached with Surrogate-Key purge on menu changes

**Key data flow to narrate (consumer places an order):**
Consumer taps "Place Order" → API Gateway validates JWT → Order Service creates order in PostgreSQL (`status=pending_payment`), calls Stripe → on payment success, transitions to `placed`, writes outbox row in same transaction → Debezium reads WAL, publishes `order.placed` to Kafka → Merchant Notification Service pushes to merchant tablet → Merchant taps Accept → Order transitions to `merchant_accepted` → Dispatch Service consumes event, queries Redis GEORADIUS for nearby Dashers, scores, sends offer → Dasher accepts → `dasher_assigned` → Location Service streams GPS to consumer WebSocket.

**Key decisions to state while drawing:**
- "I'm using the transactional outbox pattern so that order state changes and Kafka publishes are atomic — I can't have an order transition in PostgreSQL but the Kafka event lost."
- "I'm separating the WebSocket Gateway from the REST API Gateway because WebSocket connections are stateful and long-lived — they need different scaling and deployment characteristics."
- "I'm using Redis GEORADIUS for dispatch candidate queries, not Elasticsearch, because I need sub-5ms geo lookups for real-time dispatch decisions."

---

### 3f. Deep Dive Areas

**The two or three areas interviewers probe most deeply:**

**1. Dispatch System (the hardest unique component)**

The problem: when an order is ready, you must find the best Dasher within a 5 km radius within 2 seconds, issue an offer with a 60-second window, handle decline/timeout, cascade to the next candidate, and never double-assign an order.

The solution uses a pipeline: Redis GEORADIUS query (5ms, returns up to 20 candidate Dasher IDs sorted by distance) → inline scoring using a pre-loaded ONNX model in the Dispatch Service process (sub-millisecond per candidate, no network hop) — the score combines proximity, acceptance rate, completion rate, current load, and direction alignment toward the merchant → insert into `dispatch_offers` with `expires_at = now() + 60s` → push offer to Dasher via FCM → a Quartz scheduler job runs every 5 seconds to expire timed-out offers and cascade to the next candidate.

Acceptance race condition prevention: the Dasher acceptance handler runs `SELECT ... FOR UPDATE` on the `dispatch_offers` row and also verifies `orders.status = 'ready_for_pickup'` still holds before committing. If the order was cancelled in the 60-second window, the acceptance is rejected with 409. The offer TTL and the row lock together make double-assignment impossible.

**Unprompted trade-off to mention:** "I chose sequential ranked offers rather than broadcasting to all Dashers simultaneously. Broadcasting is faster for assignment but creates lock contention on the order row (multiple Dashers trying to accept simultaneously) and is unfair to Dashers who decline frequently. Sequential offers with scoring maintains assignment quality and fairness at the cost of up to N×60 seconds in the worst case, which we mitigate by cascading quickly and expanding the radius after 3 rounds."

**2. Location Fan-out (the hardest scale problem)**

The problem: 125,000 GPS writes/second need to be processed, stored, and fanned out to 252,000 consumer tracking sessions, each of which expects an update every 4 seconds, all with end-to-end latency under 5 seconds.

The solution is a two-tier write path: Location Service writes synchronously to Redis (`SETEX dasher:pos:{dasher_id} 10 {lat,lng}`) and to Redis Geo sorted set for dispatch. It then publishes to Kafka asynchronously. Kafka has two consumer groups: (1) the Cassandra Consumer which batch-inserts location history rows and (2) the Location Fan-out Service. The Fan-out Service publishes `PUBLISH tracking:{order_id} {lat,lng,heading}` to Redis Pub/Sub. All WebSocket Gateway nodes subscribe to the `tracking:*` pattern. Each node checks its local in-memory session map — if it has a WebSocket connection for a consumer of that order, it pushes. Otherwise it discards silently in O(1) time.

The ETA recalculation is rate-limited: a Redis SETEX key `eta_recalc_ts:{order_id}` with a 15-second TTL prevents more than one OSRM call per active order per 15 seconds. This reduces 125,000 GPS writes/second to approximately 24,000 OSRM calls/second across the active order fleet, which a 24-node OSRM cluster handles comfortably.

**Unprompted trade-off to mention:** "Redis Pub/Sub sends every tracking message to every WebSocket Gateway node, not just the one holding the relevant connection. At 63,000 fan-out pushes/second across 10 nodes, that is 630,000 Redis Pub/Sub deliveries/second. Redis handles ~1 million messages/second per cluster, so we are fine. The trade-off is slightly higher Redis message volume versus the complexity of a central routing table that maps order_id to the specific gateway node. The local filter approach wins because the routing table would be the bottleneck."

**3. Menu Index Freshness (the search-specific problem)**

The problem: when a merchant marks an item as 86'd, search results and the menu page must reflect this within 30 seconds. With 70 million indexed documents across 700,000 merchants, you cannot do a full re-index on every change.

The solution is an event-driven partial update pipeline: Merchant action → Menu Service writes to PostgreSQL and publishes to Kafka `menu-change-events` (partitioned by merchant_id to ensure FIFO ordering per restaurant) → Search Indexer consumes the event, deduplicates via Redis `indexed_event:{event_id}` key (10-minute TTL), issues an Elasticsearch `_update` partial doc (`{ "is_available": false }`) → Elasticsearch refreshes its in-memory segment every 1 second → simultaneously, a cache invalidation event busts the Redis menu cache and triggers a CDN Surrogate-Key PURGE for all cached responses tagged with `merchant:{id}`. End-to-end: ~2–3 seconds. Well within the 30-second SLA. A reconciliation job runs every 5 minutes to catch any events dropped by the Kafka pipeline.

**Unprompted trade-off to mention:** "If I used synchronous Elasticsearch writes in the merchant API instead of this async pipeline, the write path would be tightly coupled to Elasticsearch availability. Elasticsearch is eventually consistent by design — it is not appropriate as a synchronous write target in a critical path. The async Kafka approach means PostgreSQL is always the source of truth and Elasticsearch is always a derived projection. The cost is eventual consistency with a bounded staleness of 30 seconds, which the product team accepted."

---

### 3g. Failure Scenarios

**Think through these from most likely to most catastrophic:**

**Scenario 1: Kafka consumer lag spike (Search Indexer falls behind)**
Cause: Indexer pod crashes or is redeployed. Kafka retains messages (RF=3) until consumed. On restart, the indexer replays from its last committed offset. At 81 writes/second average × 7,200 seconds of downtime = ~583K messages. Elasticsearch can process ~10K partial updates/second on a 3-node cluster. Recovery time: ~58 seconds. The 5-minute reconciliation job acts as an additional safety net. Consumer impact: menu availability may be stale for up to 30 minutes in the worst case, but never incorrect after recovery — idempotent replays converge to the correct state.

**Scenario 2: PostgreSQL Aurora primary fails**
Cause: Hardware failure or AZ outage. Aurora Multi-AZ failover promotes a read replica to primary in <30 seconds. During those 30 seconds, order writes fail. The Order Service should queue incoming order requests and retry against the new primary after failover. Orders are retried via idempotency keys — the `idempotency_key UNIQUE` constraint in the orders table prevents duplicate orders even if the service retries after the failover window. Consumer impact: up to 30 seconds of order placement failures, surfaced as a "retry" error rather than a silent double-charge.

**Scenario 3: APNs / FCM outage (push notification provider down)**
Cause: Apple or Google push infrastructure degradation. The Notification Service circuit breaker opens after 10 failures in 30 seconds. CRITICAL notifications (dasher_assigned, delivered) fall back to SMS via Twilio at ~$0.0075 per message. Non-critical notifications (promotions, minor status updates) are dropped. The circuit breaker auto-closes after 60 seconds and retries the push path. Outcome: consumers still receive critical delivery updates via SMS, preventing the experience of a delivered order that appears stuck in "preparing" state.

**Scenario 4: Redis cluster failure (dispatch and tracking impact)**
Cause: Multi-node Redis failure. This is the most severe scenario because Redis is load-bearing for dispatch (Geo-index), location fan-out (Pub/Sub), and ETA rate limiting. Mitigation requires a multi-AZ Redis cluster with automatic replica promotion. In total cluster failure, dispatch falls back to a PostgreSQL-backed Dasher lookup (slower, ~50ms vs 5ms, but functional). Location fan-out degrades to polling (consumers poll `GET /orders/{id}/tracking` every 15 seconds). This is graceful degradation with a pre-built fallback path, not a hard outage.

**Senior framing for failure scenarios:**
Frame these as "I designed for defense in depth." State the primary path, the detection mechanism (circuit breaker, dead-letter queue, alert threshold), the fallback path, and the recovery convergence. Do not just say "we retry" — explain what prevents the retry from creating a worse problem (idempotency keys, deduplication via Redis SET NX, transactional outbox for at-least-once delivery).

---

## STEP 4 — COMMON COMPONENTS

Every component used across the three problems, with the "why it's used," "key configuration," and "what breaks without it":

### Redis Cluster

**Why used:** Redis is the hot-path operational state store. It serves three distinct roles: (1) ephemeral current state with TTLs (Dasher position, active order state, cart), (2) the dispatch geo-index (Redis Geo sorted set with GEORADIUS queries), and (3) the cross-node fan-out bus (Pub/Sub for WebSocket gateway distribution).

**Key config:** Single cluster with 16,384 hash slots. Each master shard has one replica. Pub/Sub throughput: ~1 million messages/second per cluster. TTLs are mandatory — `dasher:pos:{dasher_id}` has a 10-second TTL (if a Dasher goes offline without sending an offline signal, the key expires and they drop out of dispatch consideration naturally). Menu cache: 60-second TTL. WebSocket session registry: 2-hour TTL covering the maximum order delivery duration. Cart: 24-hour TTL.

**What breaks without it:** Dispatch becomes a multi-second database query instead of a 5ms geo lookup. WebSocket fan-out requires a central routing database that becomes the bottleneck. ETA rate limiting disappears and OSRM receives 125,000 calls/second instead of 24,000. The system runs but at dramatically reduced throughput and increased latency everywhere.

---

### Apache Kafka

**Why used:** Kafka is the async event backbone. It decouples producers (Order Service, Location Service, Menu Service) from consumers (Dispatch Service, Notification Service, Search Indexer, ETA Recalc Service) and provides durable at-least-once delivery with replay capability. This is critical for the transactional outbox pattern — Debezium reads PostgreSQL WAL and publishes reliably to Kafka, ensuring that a database commit and a downstream event notification are never permanently separated.

**Key config:** Replication factor 3, ISR (in-sync replicas) 2, `acks=all` on producers, producer idempotence enabled. Partition keys are critical: `order-events` by `order_id` (FIFO ordering per order), `location-events` by `order_id` (not `dasher_id` — so a Dasher's GPS pings for a specific order arrive in order at the ETA and fan-out consumers), `menu-change-events` by `merchant_id` (FIFO per restaurant prevents out-of-order availability toggles), `notification-triggers` by `recipient_id`. Partition counts are pre-scaled 24 hours before major events (Super Bowl, Valentine's Day).

**What breaks without it:** Order Service must synchronously call Dispatch Service, Notification Service, and Search Indexer on every state transition. If any of those are slow or down, the order placement API call times out or fails. The system becomes a tightly coupled synchronous mesh. Replay capability disappears — if the Notification Service crashes during a pizza rush, all missed notifications are lost forever.

---

### PostgreSQL Aurora (Source of Truth)

**Why used:** All three problems use PostgreSQL Aurora as the ACID-consistent source of truth for entities that require strong consistency: orders, users, merchants, payments, menu catalog. SERIALIZABLE isolation is used specifically for order state transitions (the `SELECT ... FOR UPDATE` + `valid_transitions` check pattern). READ COMMITTED is used for all reads to avoid lock contention.

**Key config:** Multi-AZ with automatic failover in <30 seconds. Aurora Global Database for cross-region read replicas with <1 second replication lag. Orders table uses `idempotency_key UNIQUE` constraint to prevent duplicate orders at the database level. Shard by `merchant_id` using CitusDB for orders — this collocates all of one restaurant's orders on the same shard, enabling efficient merchant dashboard queries without cross-shard joins. Read replicas handle consumer order history reads, keeping write traffic on the primary.

**What breaks without it:** You have no ACID guarantees on payments or order state. A consumer gets charged twice. A Dasher is dispatched to a cancelled order. The audit trail (order_status_history) that powers dispute resolution is unreliable.

---

### Apache Cassandra (Location + Notification Time-Series)

**Why used:** Cassandra handles the two highest-write-volume append-only datasets: Dasher location history (125,000 writes/second, 30-day TTL) and notification log (270 pushes/second, 90-day TTL). It provides linear horizontal write scalability and native per-row TTL, which eliminates the need for a separate cleanup job. The data model maps perfectly: partition by `dasher_id` (all location history for a Dasher on the same node), cluster by `recorded_at DESC` (most recent position is the first row read).

**Key config:** Replication factor 3, LOCAL_QUORUM consistency for both reads and writes (tolerates one-node failure per datacenter). TTL 30 days on location rows (2,592,000 seconds). TTL 90 days on notification rows (7,776,000 seconds). Batch inserts of 500 rows to reduce per-write overhead. The `dasher_location_history` schema: `PRIMARY KEY ((dasher_id), recorded_at)` with `CLUSTERING ORDER BY (recorded_at DESC)` enables both "latest position" queries (O(1), first row) and "recent track for ETA model training" queries (range scan).

**What breaks without it:** You try to use PostgreSQL for 125,000 GPS writes/second. PostgreSQL can handle about 50,000 simple inserts/second on modern hardware, but with the overhead of secondary indexes (on `dasher_id`, `recorded_at`) and MVCC row versioning, you hit contention and replication lag well before that limit. Either you drop GPS writes silently, or you need a massive PostgreSQL cluster that is far more operationally complex and expensive than Cassandra for this access pattern.

---

### H3 Hexagonal Grid (Geographic Aggregation)

**Why used:** H3 is Uber's open-source hierarchical hexagonal geospatial index. It converts a (lat, lng) into a cell identifier at a given resolution. Hexagons tessellate without gaps (unlike squares), which means a restaurant 1.1 km from the center of an H3-7 cell and a restaurant 1.1 km to the left are both in the same cell — you never get edge effects where a restaurant is just outside a bounding box. Used in three places: surge pricing (resolution 7, ~1.2 km² cells, compute supply/demand ratio per cell every 60 seconds), CDN cache keys for restaurant list pages (resolution 7, consumers within the same cell see the same cached response), and autocomplete geo-context filtering (resolution 6, ~36 km² cells, large enough to cache suggestions without over-precision).

**Key config:** H3 resolution 7 for surge pricing and CDN cache keys (fine-grained, ~1.2 km²). H3 resolution 6 for autocomplete caching (coarser, ~36 km², reduces cache key space while still being geo-relevant). The Python/Java/Go H3 library converts a lat/lng to an H3 index in ~50 microseconds.

**What breaks without it:** Surge pricing requires querying "all Dashers within 2 km of this point" in real time — without H3 bucketing, this is either a full Redis GEORADIUS scan (acceptable for dispatch but expensive for every consumer's price quote) or a database spatial query. Without H3 for CDN cache keys, you cannot cache restaurant list pages by geography — every consumer's exact lat/lng is different, giving you a cache hit rate of ~0%.

---

### OSRM (Self-Hosted Routing)

**Why used:** OSRM (Open Source Routing Machine) is a self-hosted road network routing engine that computes driving time between two coordinates in <10 milliseconds using precomputed route graphs built from OpenStreetMap data. It is used for all ETA calculations: Dasher travel time to merchant and merchant to consumer. The alternative, Google Maps Distance Matrix API, would cost approximately $20 million per month at 48,000 route calls/second. OSRM on a 24-node cluster (r5.2xlarge) costs approximately $840/month.

**Key config:** 24 OSRM nodes behind a load balancer. Each node handles ~2,000 routing calls/second. Live traffic overlays: Dasher telemetry data (average speed per road segment, computed from GPS pings) is aggregated every 2 minutes and loaded into OSRM as speed profile adjustments, creating a feedback loop unique to the platform. ETA rate-limited to 1 call per active order per 15 seconds, reducing peak OSRM load from 125,000 calls/second to ~24,000 calls/second. Circuit breaker on OSRM failure: fallback to a precomputed lookup table of `(distance_bin_km, time_of_day, day_of_week) → avg_minutes`.

**What breaks without it:** The ETA becomes a static estimate from order placement, never updating as the Dasher moves. Alternatively, using Google Maps API at this scale incurs a cost so large it would eliminate the platform's margin. A distance/average-speed formula without road network data gives ETAs accurate to only ±30 minutes.

---

### Elasticsearch (Restaurant and Menu Search)

**Why used:** Elasticsearch provides three capabilities that no other component in this stack offers simultaneously: full-text BM25 relevance scoring, native geo-distance filtering with `geo_point` field type using a BKD tree (O(log N) per shard, typically <10ms for 700K restaurant documents), and `function_score` query for blending text relevance with numeric signals (rating, review count, distance decay, popularity score) in a single query. The `completion` suggester with FST-based prefix search handles autocomplete at <15ms.

**Key config:** `restaurant_index`: 5 primary shards + 1 replica. `menu_item_index`: 10 primary shards + 1 replica, with custom `_routing` by `merchant_id` so all items for one restaurant land on one shard (single-shard query instead of 10-shard fan-out for per-restaurant menu queries, ~10x latency reduction). `autocomplete_index`: 3 primary shards + 1 replica. Index refresh interval: 1 second (tunable to 500ms for the 30-second freshness SLA). Elasticsearch is always treated as a derived, eventually-consistent projection of PostgreSQL — never as a primary store.

**What breaks without it:** Restaurant and menu search falls back to PostgreSQL full-text search (`tsvector`/`tsquery`), which lacks geo-distance operators, function_score blending, the completion suggester, and faceting/aggregation support. At 8,500 search queries/second, PostgreSQL would be completely overwhelmed. Alternatively, a managed service like Algolia would cost ~$50,000/month at this query volume.

---

### CDN with Surrogate-Key Purge

**Why used:** Restaurant list pages and individual menu pages are read-heavy (8,500 requests/second) and change infrequently (menu updates at ~81 writes/second average). Serving them from CDN edge nodes eliminates origin load and reduces latency from 200ms to <20ms for most consumers. The critical capability is **Surrogate-Key** (also called cache tags) invalidation: every CDN-cached response is tagged with `merchant:{id}`. When a merchant pauses or changes their menu, a single CDN PURGE API call with `Surrogate-Key: merchant:{id}` instantly invalidates all cached responses that reference that merchant, across all CDN edge nodes, globally, within ~5 seconds.

**Key config:** `s-maxage=30, stale-while-revalidate=60` for restaurant list pages (30-second TTL, 60-second background revalidation). `s-maxage=60` for individual menu pages. CDN cache key includes the H3 resolution-7 cell, filters hash, and sort order for restaurant list pages (so different filter combinations get different cache entries). The Surrogate-Key PURGE is triggered by the Search Indexer after it updates Elasticsearch, ensuring the CDN and search index are invalidated together.

**What breaks without it:** Every consumer request for a restaurant list hits the origin API, adding 8,500 requests/second of direct traffic to the Search Service. Menu pages for popular restaurants get slammed during peak dinner hours. Without tag-based invalidation, you either wait for TTL expiry (consumers see stale menus for up to 60 seconds) or you must track exactly which cache keys reference each merchant and purge them individually — combinatorially complex given that one restaurant can appear in hundreds of geo-tile × filter combination cache keys.

---

### Transactional Outbox Pattern (Debezium CDC)

**Why used:** When the Order Service transitions an order's state in PostgreSQL, it must also publish an event to Kafka for Dispatch, Notification, and other consumers. These are two different systems, and there is no distributed transaction spanning both. If the application writes to PostgreSQL and then crashes before publishing to Kafka, the downstream services never know the order changed state. The transactional outbox solves this: the same PostgreSQL transaction that updates the order also inserts a row into an `outbox` table. Debezium reads the PostgreSQL write-ahead log (WAL) and publishes the outbox row to Kafka. Because WAL reading is at the database level, not the application level, no application crash can cause an event to be silently lost.

**Key config:** Debezium deployed as a Kafka Connect connector. It stores its WAL read offset in Kafka, so it resumes from exactly where it stopped after a restart. This provides at-least-once delivery (a restart may replay the last few events). Consumers handle duplicates via idempotency: the `idempotency_key` UNIQUE constraint on orders (for order placement retries), Redis SET NX deduplication for notifications (`notif_dedup:{recipient_id}:{order_id}:{type}` with 300-second TTL), and Redis `indexed_event:{event_id}` deduplication for the Search Indexer (10-minute TTL).

**What breaks without it:** You lose the at-least-once delivery guarantee. Application-level Kafka publishes fail silently if the Kafka broker is temporarily unreachable (network blip), the application crashes after the DB commit, or the Kafka client's internal buffer is full. In food delivery, a missed `order.merchant_accepted` event means Dispatch Service never dispatches a Dasher. The order sits in `merchant_accepted` state forever. The consumer gets no food.

---

### Push Notifications (APNs / FCM / SMS Fallback)

**Why used:** Consumers, merchants, and Dashers need to receive status change notifications even when their app is not in the foreground. WebSocket handles foreground updates; push notifications handle background alerting. APNs handles iOS devices, FCM handles Android devices and merchant tablets. SMS via Twilio is a fallback for critical notifications when push providers are unavailable.

**Key config:** Notification priority classification: CRITICAL (dasher_assigned, delivered) → APNs priority 10, FCM priority "high" (wake device from doze mode). HIGH (picked_up) → APNs priority 10. INFO (order_placed, preparing) → APNs priority 5. Circuit breaker: 10 failures in 30 seconds opens the breaker, routes CRITICAL notifications to SMS, closes automatically after 60 seconds. Deduplication: Redis SET NX `notif_dedup:{recipient_id}:{order_id}:{type}` with 300-second TTL prevents duplicate pushes from Kafka at-least-once redelivery. Device token management: tokens are invalidated when APNs returns `BadDeviceToken` or `Unregistered` responses. All outcomes logged to Cassandra `notifications` table with TTL 90 days for SLA reporting and retry analysis.

**What breaks without it:** Consumers only know their order status when they actively open the app. Dashers miss offers because they are not staring at the app waiting for one. Merchants miss incoming orders during busy periods when they have stepped away from the tablet. The platform becomes unusable in the background, which is where most food delivery actually happens.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### DoorDash (Core Three-Sided Marketplace)

**What is unique about this problem that does not appear in the other two:**
- The **three-sided marketplace** coordination: three different actors (consumer, merchant, Dasher) touching the same order at different points, each with their own app, notification requirements, and API surface.
- The **Dispatch Service**: no other problem needs this. It is a real-time matching engine that must produce a result in under 2 seconds using scored candidates from a geo-index, issue time-limited offers, handle cascading declines, expand search radius, and escalate to ops — all while never double-assigning an order.
- The **Surge Pricing Service**: evaluates supply/demand ratio per H3 resolution-7 cell every 60 seconds, applies a piecewise multiplier (ratio < 0.4 → 2.0×, ratio 0.4–0.6 → 1.5×, ratio 0.6–0.8 → 1.2×), and writes results to Redis with a 60-second TTL for both the consumer delivery fee display and the Dasher pay boost calculation.
- The **full order state machine** with 11+ states, `valid_transitions` enforcement table, and cancellation compensation logic (full refund before merchant accepts; partial or none after Dasher picks up).
- The **ONNX model inline in the Dispatch Service**: rather than calling an external ML inference service, the scoring model is loaded into the Dispatch Service process as an ONNX runtime, achieving sub-millisecond per-candidate scoring without a network hop — at 1,710 dispatch evaluations/second (171 peak orders × 10 candidates), this matters.

**Different architectural decisions compared to the other two problems:**
- CitusDB horizontal sharding of the `orders` table by `merchant_id` (collocating one restaurant's orders on one shard for efficient merchant dashboard queries).
- Debezium CDC is used here as the primary Kafka publish mechanism (not just a safety net as in Menu Search).
- The Quartz scheduler for offer expiration is unique to dispatch — no equivalent in Search or Tracking.

**Two-sentence differentiator:** DoorDash's uniqueness is simultaneously managing three stateful actors on three separate apps through a 11-state order machine while running a sub-2-second dispatch matching engine that evaluates 1,710 Dasher candidates per second using an inline ONNX scoring model. No other problem in this folder requires this level of cross-actor coordination or real-time algorithmic matching.

---

### Menu & Restaurant Search

**What is unique about this problem that does not appear in the other two:**
- The **two-stage relevance ranking pipeline**: Stage 1 is Elasticsearch `function_score` combining BM25 text relevance (boosted on `name^3`, `cuisine_types^2`, `description^1`) with a Gaussian distance decay function and log1p-modified numeric boosters for rating and review count — this runs in 20-40ms across the cluster and returns the top 50 candidates. Stage 2 is an in-process personalization re-blend in the Search Service itself, fetching cuisine affinity and dietary preference vectors from Redis in <2ms and applying `final_score = 0.7 × es_score + 0.2 × cuisine_affinity + 0.1 × dietary_match`.
- The **custom Elasticsearch routing by merchant_id** on `menu_item_index`: by default an ES query fans out to all 10 shards; with `_routing=merchant_id`, ES sends the query to exactly the 1 shard holding all items for that restaurant, reducing per-restaurant menu queries from a 10-shard fan-out to a single-shard lookup, approximately 10x lower latency.
- The **multi-layer autocomplete pipeline**: ES Completion Suggester (FST-based, geo-contexted at H3 resolution 6, <15ms) as primary, a Redis ZSET geo-cell cache (`suggest:{h3_cell_r6}:{prefix}`, 60-second TTL, ~80% cache hit rate reducing ES load), and a nightly top-1000 prefix cache in Redis (<1ms for the most common queries). Client-side debounce of 150ms further reduces actual query volume.
- The **search quality measurement system**: p-NDCG@10 measured offline against the `search_events` table (labeled click/order data). Baseline BM25-only = 0.52; function_score = 0.68; with personalization = 0.73. This is a data-driven ranking system, not a hand-coded formula.
- Dietary flag filtering with `keyword` array fields in ES — both restaurant-level and item-level dietary flags, with the filter applied in the `bool.filter` clause (not the `must` clause, so it is a zero-score filter rather than a relevance factor).

**Two-sentence differentiator:** Menu Search's uniqueness is its two-stage ranking pipeline — Elasticsearch function_score for large-scale geo+text+boost filtering (20-40ms) followed by in-process personalization re-blend using Redis-served preference vectors (<2ms overhead) — achieving p-NDCG@10 of 0.73 without the full operational cost of a learning-to-rank system. No other problem in this folder requires this level of search sophistication, multi-index management, or ranking quality measurement.

---

### Real-Time Order Tracking

**What is unique about this problem that does not appear in the other two:**
- The **WebSocket fan-out architecture at 800K concurrent connections**: 8-10 WebSocket Gateway nodes each holding ~125K connections using epoll-based async I/O. The Redis Pub/Sub pattern-subscribe design (`tracking:*`) means every gateway node receives every tracking message and filters locally in O(1) time using its in-memory session registry — eliminating the need for a central routing table that would become the bottleneck at scale.
- **Client-side dead reckoning**: the server sends Dasher position every 4 seconds. The client predicts intermediate positions at 30+ FPS using `predicted_lat = last_lat + (speed × cos(heading) × elapsed_time) / EARTH_RADIUS`. If the predicted position diverges more than 50 meters from the server-reported position, the client snaps to the actual position. This provides a smooth map animation experience without increasing server-side push frequency.
- **Multi-device WebSocket session management**: `ws:user:{user_id}` is a Redis Set of session IDs. On any status event, the system iterates all session IDs for the user and pushes to each connected device simultaneously, enabling a consumer to track their order on both their phone and tablet.
- **Priority-classified push notifications with circuit breaker and SMS fallback**: CRITICAL notifications (dasher_assigned, delivered) use APNs priority 10 / FCM "high" to wake the device from doze mode. A circuit breaker monitors failure rate (10 failures/30 seconds opens; auto-closes after 60 seconds) and routes CRITICAL notifications to SMS (Twilio) during APNs/FCM outages, while dropping non-critical events to avoid SMS cost.
- **Dasher-approaching geofence**: a haversine check per GPS ping triggers a single "Your Dasher is 2 minutes away" notification when the Dasher crosses within 500 meters of the delivery address, deduped via the standard notification dedup key to fire exactly once per order.

**Two-sentence differentiator:** Order Tracking's uniqueness is its fan-out architecture at 1 million simultaneously tracked orders — Redis Pub/Sub with local session filtering on each of 8-10 WebSocket Gateway nodes avoids any central routing bottleneck, while client-side dead reckoning provides 30+ FPS animation from 4-second server updates. No other problem in this folder requires managing 800K concurrent stateful WebSocket connections, designing a dead-reckoning animation system, or building a priority-classified notification pipeline with provider-level circuit breakers.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2-4 sentences each)

**Q: How does a consumer placing an order trigger a Dasher being dispatched?**
**KEY PHRASE: transactional outbox → Kafka → Dispatch Service**
The Order Service writes the order state transition to PostgreSQL and an `outbox` row in the same atomic transaction. Debezium reads the PostgreSQL WAL and publishes the `order.merchant_accepted` event to Kafka. The Dispatch Service consumes this event, queries Redis GEORADIUS for nearby Dashers, scores candidates using an inline ONNX model, and issues an offer to the top-ranked Dasher with a 60-second acceptance window.

**Q: Why do you use three different databases (PostgreSQL, Redis, Cassandra) instead of one?**
**KEY PHRASE: each database matches one access pattern**
PostgreSQL provides ACID transactions and complex queries for orders, users, and payments where correctness is non-negotiable. Redis provides sub-millisecond reads and native Pub/Sub for operational state that expires naturally (Dasher position, cart, session data). Cassandra provides linear write scalability and native per-row TTL for the 125,000 GPS writes/second that would overwhelm PostgreSQL's write throughput limits. Using one database for all three patterns would mean either sacrificing correctness (PostgreSQL under GPS write load), sacrificing query capability (Cassandra for orders), or sacrificing durability (Redis for orders).

**Q: How do you prevent a consumer from placing the same order twice by double-tapping "Place Order"?**
**KEY PHRASE: idempotency_key UNIQUE constraint**
The consumer app generates a unique idempotency key (UUID) when the order checkout screen loads and sends it with every order placement request. The `orders` table has a UNIQUE constraint on `idempotency_key`. If the request is retried (due to network timeout), the second attempt hits the UNIQUE constraint and the server returns the original order response from a Redis cache (keyed by idempotency_key, 24-hour TTL) rather than creating a duplicate. This prevents both double-charges and duplicate orders.

**Q: How does the menu search stay up to date when a merchant marks an item as unavailable?**
**KEY PHRASE: Kafka change event pipeline + ES partial update + CDN Surrogate-Key purge**
The merchant action writes to PostgreSQL and publishes a `menu-change-events` Kafka message. The Search Indexer consumes the event and issues an Elasticsearch partial `_update` doc. Elasticsearch refreshes its search index every 1 second. Simultaneously, a cache invalidation message busts the Redis menu cache and triggers a CDN Surrogate-Key PURGE for all cached responses tagged with `merchant:{id}`. End-to-end propagation takes 2-3 seconds, well within the 30-second SLA.

**Q: How does a consumer see the Dasher's location moving on their map in real time?**
**KEY PHRASE: WebSocket + Redis Pub/Sub fan-out + client dead reckoning**
The Dasher app pings the Location Service every 4 seconds. The Location Service publishes the update to Kafka. The Location Fan-out Service consumes the event and publishes to a Redis Pub/Sub channel `tracking:{order_id}`. All WebSocket Gateway nodes are subscribed to the `tracking:*` pattern. Each node checks its in-memory session registry and pushes the update to the consumer's WebSocket connection. The consumer app smoothly animates the Dasher pin between these 4-second server updates using client-side dead reckoning at 30+ FPS.

**Q: What is surge pricing and how is it computed?**
**KEY PHRASE: supply/demand ratio per H3 cell → piecewise multiplier → Redis TTL**
Surge pricing increases the delivery fee when demand (pending orders) exceeds supply (available Dashers) in a geographic area. Every 60 seconds, the Pricing Service computes the ratio of online Dashers to pending orders within each H3 resolution-7 hexagonal cell (~1.2 km²). A piecewise function maps the ratio to a multiplier: below 0.4 → 2.0×, 0.4 to 0.6 → 1.5×, 0.6 to 0.8 → 1.2×, above 0.8 → 1.0× (no surge). The multipliers are written to Redis with a 60-second TTL. Both the consumer's delivery fee display and the Dasher's pay estimate use the multiplier for the relevant hexagon.

**Q: How do you handle a Dasher who accepts an order but then never arrives at the restaurant?**
**KEY PHRASE: GPS liveness check + location timeout + re-dispatch**
The Location Service monitors each Dasher's last ping timestamp. If no GPS ping is received for more than 60 seconds (`dasher:pos:{dasher_id}` TTL expires), the system treats the Dasher as unreachable. The Order Service detects this via a health check job, transitions the order to `dasher_assignment_failed`, and the Dispatch Service re-runs the matching process from the beginning. The consumer receives a push notification ("We are finding you a new Dasher") and the ETA is recalculated. The original Dasher's profile records the incomplete delivery against their completion rate.

---

### Tier 2 — Deep Dive Questions (why + trade-offs)

**Q: Walk me through the trade-offs you considered for the dispatch matching algorithm. Why did you choose ranked sequential offers over broadcasting to all nearby Dashers?**
**KEY PHRASE: fairness + lock contention vs. assignment speed**
Broadcasting to all nearby Dashers and taking the first accept is faster for assignment but creates two problems: (1) lock contention on the order row — multiple Dashers simultaneously attempt `SELECT ... FOR UPDATE` on the same order, all but one must retry, which under high load causes cascading retries and latency spikes; (2) fairness — the Dasher with the lowest-latency connection always wins, disadvantaging Dashers with older phones or worse network conditions. Sequential ranked offers with a scoring model address both: only one Dasher receives an offer at a time, eliminating contention, and the scoring function balances proximity with acceptance rate and direction-of-travel alignment. The trade-off is worst-case latency: if five Dashers decline, assignment takes up to 5 × 60 seconds. We mitigate this by using a tight ML-based score to surface the most likely acceptors first, reducing the average offer cascade length to 1.3 in practice.

**Q: Why did you choose Elasticsearch over Algolia or a native PostgreSQL full-text search for the restaurant/menu search?**
**KEY PHRASE: cost at 8,500 req/s + geo_point native support + function_score blending**
Algolia has excellent typo tolerance and developer experience but charges per query — at 8,500 requests/second, the estimated cost is over $50,000/month, which is prohibitive. PostgreSQL full-text search (`tsvector`/`tsquery`) does not have native geo-distance query support or function_score-style relevance blending; it would require joining against a separate PostGIS spatial query for every search request, adding 20-50ms of latency and substantial read load to the transactional database. Elasticsearch natively handles full-text BM25 scoring, `geo_distance` filter using a BKD tree (O(log N), <10ms), and `function_score` blending of text relevance with distance decay and numeric boosters in a single query. The operational overhead of running an ES cluster is justified by this capability density at the required query volume.

**Q: How does the ETA calculation stay accurate without recomputing OSRM routes for every one of the 125,000 GPS writes per second?**
**KEY PHRASE: rate-limit to 1 recalc per 15 seconds per order via Redis SETEX**
The ETA Recalc Service consumes GPS location events from Kafka but checks a Redis rate-limit key `eta_recalc_ts:{order_id}` before making an OSRM call. If the key exists (set within the last 15 seconds), it skips the recalculation. If the key is absent or expired, it calls OSRM and then sets the key with a 15-second TTL. This converts 125,000 writes/second across all Dashers into approximately 24,000 OSRM calls/second across all active orders — a ratio driven by the fact that each order takes ~35 minutes and we recalculate every 15 seconds, giving ~140 recalculations per delivery. The 24,000 calls/second is comfortably served by a 24-node OSRM cluster. Between recalculations, the consumer app uses client-side dead reckoning to animate the Dasher pin smoothly.

**Q: Explain the transactional outbox pattern. What specific failure scenario does it solve, and what would go wrong without it?**
**KEY PHRASE: atomic PostgreSQL commit + outbox row + Debezium WAL read = guaranteed-at-least-once Kafka publish**
The problem is the "dual write": writing to PostgreSQL and publishing to Kafka are two separate I/O operations with no shared transaction. If the application writes to PostgreSQL and then crashes before the Kafka `producer.send()` call completes, the database change is durable but the downstream event is permanently lost. In food delivery, this means Dispatch Service never dispatches a Dasher to an order the merchant already accepted. The outbox pattern solves this by adding a third table (`outbox`) that is written inside the PostgreSQL transaction: if the database transaction succeeds, the outbox row is guaranteed to exist. Debezium reads the PostgreSQL WAL (write-ahead log) as a physical side channel — independent of the application process — and publishes the outbox row to Kafka. Because Debezium stores its WAL read position in Kafka, it resumes from exactly the right point after a crash, providing at-least-once delivery. The application never loses the event because it never held the event in memory.

**Q: How do you prevent a single slow Elasticsearch query from degrading all search results at peak?**
**KEY PHRASE: async personalization with deadline + ES query timeout + circuit breaker**
Three isolation mechanisms: (1) The personalization Redis fetch has a 20ms deadline — if it times out, the Search Service returns the pure ES results without the personalization re-blend. The system degrades gracefully from p-NDCG@10=0.73 to 0.68, which is acceptable. (2) ES queries have a `timeout` parameter set to 200ms. If a query exceeds this, ES returns partial results from however many shards responded within the window rather than failing entirely. (3) A circuit breaker at the Search Service level monitors ES error rate: if the rate exceeds 10% over 30 seconds, the circuit opens and all searches fall back to a PostgreSQL-backed restaurant list sorted by distance. This is lower quality but prevents a total search outage.

**Q: Walk me through how you handle notification deduplication across multiple Kafka consumers and service restarts.**
**KEY PHRASE: Redis SET NX with (recipient_id, order_id, notification_type) as composite key**
Kafka guarantees at-least-once delivery, meaning the same `notification-triggers` event can be delivered to the Notification Service more than once (on broker failover or consumer restart). Without deduplication, the consumer receives two "Your Dasher has arrived!" push notifications. The Notification Service uses Redis SET NX: before sending any notification, it attempts to atomically set `notif_dedup:{recipient_id}:{order_id}:{notification_type}` to `1` with EX 300 (5-minute TTL). SET NX returns 1 (new key set) only if the key did not exist — meaning this is the first processing of this notification. If it returns 0, another instance already processed it and the notification is skipped. The 5-minute TTL ensures that if the same notification type is legitimately re-triggered after 5 minutes (e.g., a second ETA update event), it goes through rather than being suppressed indefinitely.

**Q: How would you scale the WebSocket gateway to handle a sudden 3x spike in active tracking sessions?**
**KEY PHRASE: Kubernetes HPA on custom metric websocket_connection_count + Redis Pub/Sub scales horizontally**
The WebSocket Gateway is stateless with respect to business logic — all connection metadata is stored in Redis (ws:session, ws:order, ws:user). Kubernetes HPA monitors a custom Prometheus metric `websocket_connection_count` divided by a target of 100,000 connections per pod. When the ratio exceeds 0.8, HPA adds pods. New pods immediately subscribe to the Redis Pub/Sub `tracking:*` pattern and start receiving messages. The load balancer (Layer 4, TCP-aware) distributes new WebSocket connection upgrades across all available pods using round-robin. Existing connections stay on their current pod (sticky at the TCP level) — there is no connection migration needed because Redis Pub/Sub distributes messages to all pods regardless. The only constraint is Redis Pub/Sub throughput: at ~1M messages/second per cluster, a 3x spike would require Redis cluster scaling as well, which is handled by adding shard nodes.

---

### Tier 3 — Staff+ Stress Tests (reason aloud)

**Q: Your Dispatch Service is falling behind during a Friday dinner rush — Kafka consumer lag is at 45 seconds and Dashers are expiring offers before the event is even processed. How do you debug this and fix it in real time?**
**KEY PHRASE: consumer lag → scale horizontally → partition count is the ceiling**
First, confirm the diagnosis: is the lag on one partition (one bad actor, e.g., a restaurant sending a burst of orders) or across all partitions (Dispatch Service is simply underpowered)? If one partition: an upstream merchant is sending pathologically many orders, which should be rate-limited at the API Gateway level — throttle that merchant's order placement API calls. If all partitions: the Dispatch Service pods cannot keep up. Check pod CPU and memory: if they are CPU-saturated, the ONNX scoring model is the bottleneck — scale Dispatch Service pods horizontally. But note the constraint: the number of Kafka consumers in a consumer group cannot exceed the number of partitions. If `dispatch-events` has 20 partitions and we have 20 pods, adding more pods does not help — the extra pods sit idle. The real-time fix is to add more partitions to `dispatch-events`, but Kafka partition count cannot be reduced after increasing (repartitioning requires a consumer group reset and is disruptive). The medium-term fix is to pre-provision partitions 24 hours before predicted peak events (Super Bowl, Valentine's Day, major sports finals).

**Q: A postmortem shows that during a 10-minute Kafka outage, all order state transitions succeeded in PostgreSQL but no Dashers were dispatched and no consumers received notifications. The business lost approximately 30,000 orders worth of goodwill. How do you prevent this from happening again?**
**KEY PHRASE: Debezium WAL replay guarantees at-least-once delivery even during broker outage**
The root cause is that the system was using application-level Kafka publishes (calling `producer.send()` inline in the Order Service) rather than the transactional outbox pattern. During the Kafka outage, the `producer.send()` calls failed and the errors were either swallowed or logged but not acted upon. The fix is two-part: (1) Implement the transactional outbox. Every state transition writes an outbox row in the same PostgreSQL transaction. During the Kafka outage, the outbox rows accumulate in PostgreSQL. Debezium reconnects after the broker recovers and replays all unprocessed outbox rows in order, so every state transition that occurred during the outage is eventually published to Kafka. Downstream services catch up within minutes of broker recovery. (2) Add an alerting rule on Debezium lag (unprocessed outbox rows older than 30 seconds triggers a P1 alert). The outbox also provides a human-readable audit of exactly which events were delayed, enabling the ops team to verify completeness after recovery.

**Q: A large city partner is demanding that search results for their city always return results in under 50ms p99, even during a regional infrastructure outage. How would you design for this?**
**KEY PHRASE: pre-materialized geo-tile cache at CDN edge + cross-region ES replica + graceful degradation**
This is a multi-layer availability problem. The first layer is CDN edge caching: restaurant list pages for the most common geo-tile × filter combinations (representing ~80% of traffic by Pareto distribution) are pre-cached at CDN edge nodes co-located with the city. Cache TTL is 30 seconds with `stale-while-revalidate=60`. A cache hit costs <5ms and does not touch the origin. For cache misses, the second layer is a cross-region Elasticsearch replica: deploy an ES read replica in the same cloud region as the city's user base. The primary cluster writes to the replica asynchronously (typically <500ms replication lag). During a primary region outage, the Search Service routes to the replica, accepting up to 30 seconds of stale results — still within the 30-second menu freshness SLA. For a complete multi-region failure, the third layer is a pre-computed fallback: a static S3-hosted restaurant list per city, rebuilt every 5 minutes, served from S3 directly via CDN. This has no relevance ranking, but returns the most popular restaurants for the city instantly. The cascade is: CDN hit (<5ms) → regional ES replica (<50ms) → S3 static fallback (<20ms) → graceful error. Under this design, the only scenario producing a user-visible failure is simultaneous CDN + regional ES + S3 outage in the same city, which is a multi-datacenter failure scenario requiring a separate business continuity plan.

**Q: The engineering organization wants to move from sequential Dasher offers to a global batch optimization (Hungarian algorithm) to reduce average delivery time by 8%. The PM says this is worth doing. Talk me through the system design changes, the risks, and your recommendation.**
**KEY PHRASE: batching window adds latency per order + partition key changes + A/B test required before rollout**
The Hungarian algorithm solves the global minimum-cost bipartite matching: given all unmatched orders and all available Dashers at a point in time, find the assignment that minimizes total delivery time across the entire fleet. The gain is real — the current greedy sequential approach can assign a nearby Dasher to an order when a slightly farther Dasher would be a much better match globally. The system design changes: (1) Instead of processing each `order.merchant_accepted` event immediately, accumulate a batch over a fixed window (say, 1 second). (2) At the end of the window, collect all unmatched orders and available Dashers in the relevant geographic area, run the Hungarian algorithm (O(n³) where n is the batch size — at 171 orders/second and a 1-second window, n ≈ 171, giving ~5M operations per batch, well within 1 second on modern hardware), then issue offers in parallel to all matched Dashers. (3) The `dispatch-events` Kafka partition key needs to change from `order_id` (for FIFO per order) to something that enables batch aggregation — consider a geo-region key that co-locates orders in the same geographic area on the same partition. The risks: (1) The 1-second batching window adds 0-1 seconds of latency to every single order dispatch — at 171 orders/second this affects every consumer, not just some. (2) Batch failures require careful handling: if the algorithm fails mid-batch, do you reprocess all orders or just the failed ones? (3) The 8% delivery time improvement is an offline model estimate; real-world improvement requires an A/B test because Dasher behavior changes in response to the system (Dashers may cluster differently when offers are batched). My recommendation: implement the batching infrastructure in shadow mode (run both algorithms, compare outputs, do not change actual dispatch) for 30 days to validate the 8% improvement claim with production data. If confirmed, roll out to 10% of markets using a feature flag, measure actual delivery time difference, then ramp. The latency trade-off (up to 1 second per dispatch) is the kill criterion — if A/B test shows consumer order rate drops because the 1-second delay correlates with consumer impatience (order abandonment), the improvement is not worth it.

**Q: You are in a postmortem after a P0 incident where 12,000 consumers received duplicate "Your order has been delivered" push notifications over a 3-hour window. What was the most likely root cause and how do you prevent recurrence?**
**KEY PHRASE: Redis SET NX dedup key TTL too short OR notification-triggers topic replayed from beginning of offset**
There are two plausible root causes. Root cause A: the Redis SET NX deduplication key TTL of 300 seconds was too short for this scenario. If the Notification Service pod restarted and Kafka replayed `notification-triggers` events that were delivered more than 5 minutes ago (e.g., due to a consumer group offset reset during the pod restart), the dedup keys had already expired. The replay events passed the SET NX check and triggered second sends. Fix: increase the dedup TTL to 24 hours (the order delivery event is only relevant for the duration of one day) and add a secondary check against the Cassandra `notifications` table (which has a 90-day TTL) before sending — if a `delivered` notification for this `recipient_id` + `order_id` combination already exists in Cassandra with status `sent`, skip the send. Root cause B: the Kafka consumer group offset for `notification-triggers` was manually reset to the beginning by an operator during an unrelated incident (e.g., trying to replay missed notifications for a different bug). This replayed all historical notification events, bypassing the 300-second TTL dedup window. Fix: access controls on Kafka offset reset operations (require a two-person approval for consumer group offset changes in production), plus the 24-hour dedup TTL and Cassandra cross-check as above. The combination of a longer dedup TTL, a persistent dedup log in Cassandra, and access controls on Kafka consumer group management prevents all three paths to this class of incident.

---

## STEP 7 — MNEMONICS

**Mnemonic 1: "ORDER DECK" — the 8 shared components**

- **O** — OSRM (self-hosted routing, <10ms per ETA call)
- **R** — Redis (hot-path state, Geo-index, Pub/Sub fan-out)
- **D** — Debezium / Transactional Outbox (guaranteed Kafka publish)
- **E** — Elasticsearch (restaurant and menu search, function_score ranking)
- **R** — (PostgreSQL) Aurora (ACID source of truth, state machine)
- **D** — Dead-letter queues + deduplication (idempotency everywhere)
- **E** — Event backbone (Kafka, RF=3, partitioned by entity ID)
- **C** — Cassandra (125K GPS writes/s, TTL-native time series)
- **K** — CDN with surrogate-Key purge (menu and restaurant page caching)

**Mnemonic 2: "GPS-ETA" — the flow from Dasher ping to consumer map update**

- **G** — GPS ping arrives at Location Service (every 4 seconds)
- **P** — PostgreSQL? No — write to Redis (10s TTL) + Cassandra (30-day history)
- **S** — Stream to Kafka `location-events` (partitioned by order_id)
- **E** — ETA Recalc Service consumes (rate-limited 1/15s per order via Redis SETEX)
- **T** — OSRM Tells us the new ETA (< 10ms per call)
- **A** — All WebSocket Gateway nodes receive via Redis Pub/Sub, filter locally, push to consumer

**Opening one-liner for the interview:**
"Food delivery is three problems in one: a real-time marketplace matching Dashers to orders in under 2 seconds, a geo-aware search engine that must stay fresh within 30 seconds of merchant changes, and a 125,000-writes-per-second GPS ingest pipeline that fans out location updates to 252,000 simultaneous tracking sessions — all on the same infrastructure. I'll walk you through the design by starting with the most latency-sensitive path, which is order placement and dispatch."

---

## STEP 8 — CRITIQUE

### Well-Covered in the Source Material

The source files are exceptionally detailed in several areas. The order state machine is thoroughly defined with all 11+ states, the `valid_transitions` table enforcement pattern, and the race condition prevention logic (`SELECT ... FOR UPDATE`). The dispatch system is well-specified including the ONNX inline scoring model, Quartz scheduler for offer expiration, radius expansion logic, and the exact Redis GEORADIUS command with parameters. The ETA engine is well-designed with the GBM prep time model, OSRM drive time calculation, p75 vs p50 rationale, and the 15-second rate-limiting pattern. The Elasticsearch schema is fully specified including mapping types, shard counts, and the custom routing by merchant_id optimization. The capacity estimation numbers are internally consistent and derived from stated assumptions. The transactional outbox pattern and Debezium CDC are correctly described with failure mode analysis. The WebSocket fan-out architecture with Redis Pub/Sub pattern subscription is well-explained.

### Missing, Shallow, or Wrong

**Missing: Multi-region active-active design.** The source material mentions Aurora Global Database and "cross-region read replicas" but does not address what happens during a US-East primary region failure. For a platform serving 37M MAU, there should be a section on how orders in flight are handled during a region failover, whether consumers in the failing region are routed to a secondary region's read replicas (which cannot accept writes during failover), and how the ~30-second Aurora failover window is experienced by consumers. Interviewers at FAANG frequently ask about this.

**Missing: Payment architecture detail.** The source material says "calls Payment Service (Stripe tokenized charge)" and moves on. For a Staff+ interview, you need to explain: two-phase Stripe authorization (authorize at checkout, capture after Dasher picks up), Stripe idempotency key usage (same as the order idempotency key), how a timeout during authorization is handled (the order stays in `pending_payment`, the consumer sees a retry prompt), and how partial refunds are issued during cancellation after capture (Stripe Refunds API, partial amount, asynchronous).

**Missing: Data privacy and GDPR compliance.** The source material lists "GDPR/CCPA for consumer PII" as a compliance requirement but never explains how this is implemented. In an interview you should be able to state: Dasher location history is retained for 30 days then deleted by Cassandra TTL (data minimization). Consumer location (delivery address) is only passed to the Dasher as lat/lng coordinates, not the full address, until the Dasher is within 200m of the destination (progressive disclosure). Consumer PII is stored in PostgreSQL and can be deleted via a right-to-erasure pipeline that removes/anonymizes rows and rebuilds downstream caches.

**Shallow: ML model details.** The source material references "GBM model for prep time" and "ONNX scoring model for dispatch" without discussing feature engineering, training data, or model staleness handling. For a system design interview, you do not need to go deep on ML architecture, but you should be able to say: the prep time model is retrained weekly on (merchant_id, time_of_day, day_of_week, order_size, queue_depth) → actual prep time from order history; staleness detection monitors prediction error drift via a dashboard and triggers retraining if p80 prep time error exceeds 5 minutes.

**Potentially wrong or oversimplified: CitusDB sharding claim.** The source material states "orders table uses CitusDB sharding by merchant_id" but CitusDB (now Citus on Azure) is an extension for PostgreSQL that enables distributed tables — it adds significant operational complexity compared to Aurora's native read replica scaling. At 171 orders/second peak, a single Aurora PostgreSQL primary can handle this write load comfortably without sharding (PostgreSQL handles ~50K simple transactions/second). The sharding recommendation is premature optimization for the stated scale. A better answer is: "We use a single Aurora PostgreSQL primary for orders at current scale. If we needed to scale past 50K orders/second, we would consider sharding by `merchant_id` using Citus or by migrating to a distributed store like CockroachDB."

### Senior Probes to Prepare For

- "You said you use Redis for the dispatch geo-index. What happens to in-flight dispatch offers if the Redis primary node fails and the replica is promoted? Are there any orders that never get a Dasher?"
- "Your Kafka has 50 partitions for `location-events`. You are at 125K writes/second. Each partition is getting 2,500 writes/second. Is any single consumer able to keep up with that rate? Show me your math on ETA Recalc Service pod sizing."
- "You mentioned the reconciliation job runs every 5 minutes to catch missed Elasticsearch updates. How do you verify that the reconciliation job itself is not silently failing? What is your alerting strategy for the reconciliation pipeline?"
- "A merchant with 500 locations (a large chain) does a bulk menu price update at 9 PM on a Friday. That is 500 × 100 items = 50,000 index updates hitting the Search Indexer simultaneously. Walk me through whether your system handles this gracefully."
- "You chose SERIALIZABLE isolation for order state transitions. PostgreSQL SERIALIZABLE isolation can significantly reduce throughput under contention. At 171 orders/second across hundreds of merchants, what is the actual contention scenario, and do you really need SERIALIZABLE or is READ COMMITTED + optimistic locking sufficient?"

### Common Interview Traps

**Trap 1: Using PostgreSQL for GPS writes.** Every candidate who misses the "125,000 writes/second" implication and puts all location data in PostgreSQL will be exposed when the interviewer asks "can a single PostgreSQL instance handle 125K writes per second?" The answer is no, and the candidate has no fallback. Preemptively state the write rate and justify Cassandra.

**Trap 2: Using WebSocket for everything.** Some candidates use WebSocket for the REST API gateway as well, which means all three client types connect via WebSocket and all services respond via push. This creates a stateful API tier that is very hard to load balance and scale. The right answer is HTTP/REST for request-response operations (order placement, menu fetch, payment) and WebSocket specifically for real-time push (location updates, status changes).

**Trap 3: Treating Elasticsearch as the source of truth.** Elasticsearch is not an ACID store. Candidates who make ES the canonical data store for menu items will be exposed when the interviewer asks "what happens to ES data after a cluster failure and recovery?" The answer must always be: PostgreSQL is the source of truth; ES is rebuilt from PostgreSQL via the Kafka pipeline or full re-index.

**Trap 4: Not mentioning idempotency.** Any candidate who designs the order placement API without mentioning idempotency keys will be probed on "what happens if the user's request times out and they retry?" The answer "they get a second order" is unacceptable. Always mention the idempotency_key UNIQUE constraint and the Redis cache for returning the original response on retry.

**Trap 5: Confusing the two ETA rate limits.** There are two different rate limits in this system that candidates often conflate: (1) the Dasher GPS ingest rate limit (1 write per 3 seconds per Dasher, server-side enforced at the API Gateway to prevent flood) and (2) the ETA recalculation rate limit (1 OSRM call per 15 seconds per active order, server-side enforced by Redis SETEX). The first is a client throttle. The second is a compute budget optimization. They are at different granularities (per Dasher vs. per order) and serve different purposes.

---

*End of Interview Guide — Pattern 8: Food Delivery*
