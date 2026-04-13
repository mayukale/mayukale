# Pattern 7: Ride Sharing — Interview Guide

Reading Pattern 7: Ride Sharing — 4 problems, 9 shared components

---

## STEP 1 — ORIENTATION

This pattern covers four distinct but deeply interrelated problems:

1. **Uber / Lyft (Core Platform)** — the full end-to-end ride-sharing system: trip lifecycle, surge pricing, payments, ratings.
2. **Real-Time Driver Location Tracking** — ingesting 562,500 GPS writes per second, fanning out to multiple consumers, and storing 90-day history.
3. **Driver Matching Service** — finding the best available driver via multi-factor scoring and atomic assignment without race conditions.
4. **ETA Service** — computing driving time estimates over a 50-million-node road graph in under 50 milliseconds.

**Why this pattern matters in interviews**: Ride sharing is one of the most popular system design prompts because it touches nearly every hard distributed systems problem simultaneously — high-throughput writes, geospatial indexing, real-time push, consistency under concurrency, graph algorithms, ML inference, and global scale. Interviewers at FAANG and NVIDIA use it to probe depth in at least three dimensions.

**What makes it uniquely hard**:
- Location data must be ingested at 500,000+ writes/second, served in under 10 milliseconds, and retained for 90 days — three requirements that no single database technology satisfies, so a tiered storage design is mandatory.
- Trip assignment must be exactly-once under concurrent race conditions, without a distributed lock manager — leading to the CAS UPDATE pattern.
- ETA computation over a 50-million-node road graph must complete in under 50 milliseconds — impossible with standard Dijkstra, requiring offline preprocessing via Contraction Hierarchies.

---

## STEP 2 — MENTAL MODEL

**Core idea**: A ride-sharing platform is fundamentally a **real-time two-sided marketplace with hard consistency constraints on the transaction (trip assignment) and high-throughput eventual consistency everywhere else (location data, ETA, surge)**.

**Real-world analogy**: Think of a taxi dispatcher at a busy airport. The dispatcher keeps a live map of all cabs (location tracking), scores which cab to send based on distance, availability, and driver quality (matching), estimates how long the cab will take to arrive (ETA), and adjusts pricing when 20 flights land at once (surge). The moment the dispatcher picks a cab, that cab is "locked" to that passenger — you can't give the same cab to two passengers, and you can't charge the passenger twice. Everything else — knowing where cabs are, estimating times, computing prices — can be slightly approximate. That division between "must be exact" (assignment, payment) and "can be eventual" (location, ETA) is the mental model you should carry into every design decision.

**Why it is genuinely hard**:

First, the **write volume is extreme**. At 1.5 million drivers each updating their GPS every 4 seconds, you get 375,000 writes per second at average load and 562,500 at peak. That single data stream must simultaneously feed the real-time matching engine (needs sub-millisecond reads from Redis), the rider tracking screen (needs a WebSocket push within 2 seconds), and the 90-day dispute resolution archive (needs durable Cassandra storage). No single store handles all three SLAs — so you must design a multi-tier pipeline.

Second, the **matching problem has a race condition at its core**. Two drivers can accept the same trip simultaneously. A naive first-come-first-served approach with a SELECT-then-UPDATE is a classic TOCTOU bug. You need CAS (Compare-And-Swap) at the database level, not an application-level lock.

Third, the **routing problem is algorithmically non-trivial**. Standard Dijkstra on a 50-million-node road graph takes hundreds of milliseconds. At 27,770 ETA requests per second, that is physically impossible. Contraction Hierarchies reduce query time to under 1 millisecond by preprocessing the graph, but they require stateful pods that hold the entire city graph in process memory — which breaks the usual assumption that microservices are stateless.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these before drawing anything. A good interviewer will reward you for these questions because they reveal that you understand the design space.

**Question 1: Which of the four sub-problems are we designing?**
"Are we designing the full Uber/Lyft platform end-to-end, or a specific component like the location tracking pipeline, the driver matching service, or the ETA service?"

*What changes*: The full platform requires you to cover the trip state machine, payments, surge, and ratings. Location tracking focuses entirely on the ingestion pipeline and fan-out architecture. Matching is all about geospatial querying, scoring, and atomicity. ETA is a graph algorithms and ML problem.

**Question 2: What is the target geographic scope and scale?**
"Are we building for one city (50K drivers), one country (500K drivers), or globally (5 million drivers)?"

*What changes*: A single-city design can get away with a single database and a single Redis instance. Global scale requires geographic partitioning by city_id, multi-region replication, and data residency compliance (GDPR in Europe, etc.).

**Question 3: What are the hard latency requirements for matching?**
"Should driver match be returned within 3 seconds (Uber's current SLA), or is the business OK with 10 seconds?"

*What changes*: 3-second p99 matching means the entire geospatial query, scoring, CAS assignment, and push notification must complete in a tight budget. This forces Redis Geo for sub-millisecond lookups and gRPC for internal calls.

**Question 4: Do we need to handle Pool / Shared rides?**
"Are we matching only one-to-one trips, or do we need to match multiple riders into a single vehicle (like UberPool/Lyft Shared)?"

*What changes*: Pool matching requires route compatibility evaluation, multi-waypoint ETA computation, and much more complex matching logic. It also changes the trip state machine.

**Question 5: What is the required data retention for GPS history?**
"How long must we store raw GPS traces — for billing accuracy, dispute resolution, or regulatory compliance?"

*What changes*: 24-hour retention fits in Kafka alone. 90 days requires Cassandra with TTL. 7-year retention (some jurisdictions) requires cold archival to object storage. The storage numbers change dramatically.

**Red flags for the interviewer**: Jumping straight to "I'll use Redis for location" without asking about durability requirements. Designing a single-city system without acknowledging the partitioning problem. Treating GPS writes as low-frequency when they are the dominant write load.

---

### 3b. Functional Requirements

**Core requirements** (always include these for the full platform):

- **Rider flow**: Open app → enter destination → see upfront fare estimate (with surge multiplier) → request ride → be matched to a nearby driver → track driver approach in real time → complete trip → pay automatically → rate driver.
- **Driver flow**: Go online → receive trip offer with pickup/dropoff preview → accept within 15 seconds → navigate to rider → start and end trip → receive weekly payout.
- **Trip state machine**: REQUESTED → DRIVER_ASSIGNED → DRIVER_EN_ROUTE → RIDER_PICKED_UP → TRIP_IN_PROGRESS → COMPLETED or CANCELLED. Every transition must be durable and exactly-once.
- **Geospatial matching**: Find available drivers within 5 km radius; expand if none found (5 → 8 → 12 → 20 km); return match within 3 seconds p99.
- **Location tracking**: Accept GPS updates from drivers every 4 seconds; serve current driver position with under 5-second staleness; push position to rider's app over WebSocket during active trip.
- **Surge pricing**: Compute demand/supply ratio per H3 hexagon; multiply base fare when demand exceeds supply; rider sees explicit multiplier before confirming.
- **ETA**: Return pickup ETA for fare estimate screen (pre-request) and continuously update during active trip; MAPE under 15%.
- **Payment**: Charge rider's stored card on trip completion; idempotent (never double-charge); pay driver weekly via ACH.
- **Ratings**: Mutual 1-5 star rating after each trip; rolling average maintained on user records.

**Clear scope statement**: "For this interview, I'll focus on [specific component]. I'll assume payments, notifications, and fraud detection are downstream services I can publish events to but don't need to design internally."

**Out of scope** (state these explicitly to save time): food delivery, freight, autonomous vehicles, driver background check pipeline, detailed fraud ML, accounting and tax systems.

---

### 3c. Non-Functional Requirements (NFRs)

**Derive these from the use case, don't just list them**:

| NFR | Target | Why / Trade-off |
|-----|--------|-----------------|
| **Matching latency** | p99 < 3 s | Rider abandons after 5 s; forces Redis Geo (not PostGIS) |
| **Location freshness** | p99 < 5 s staleness | Rider tracking UX; forces synchronous Redis write before Kafka |
| **GPS write throughput** | 562,500 writes/s peak | Forces Kafka + async fan-out; no synchronous DB write on hot path |
| **Trip assignment consistency** | Strong (no double-assignment) | Revenue-critical; forces CAS UPDATE, not eventual consistency |
| **Payment consistency** | Strong (no double-charge) | Legal requirement; forces ACID transaction + idempotency key |
| **GPS history durability** | 90-day retention, zero loss after ACK | Dispute resolution; forces Cassandra with RF=3 |
| **Availability** | 99.99% (52 min/year) | Revenue-critical real-time service; forces multi-region failover |
| **ETA latency** | p99 < 1.5 s | Fare estimate screen UX; forces Contraction Hierarchies in-memory |
| **ETA accuracy** | MAPE < 15% | If ETAs are systematically wrong, drivers get wrong predictions and riders lose trust |
| **Security** | PCI-DSS, JWT RS256, rate limiting | Payment card data; force vault tokenization, not raw card storage |

**Key trade-offs to name explicitly**:
- ✅ Freshness over durability on the hot path: Redis position update is synchronous; Cassandra write is async via Kafka. Means a server crash after Redis but before Kafka could lose a GPS point — acceptable because GPS self-heals in 4 seconds.
- ✅ Eventual consistency for ETA and surge, strong consistency for trip state and payments.
- ❌ If you try to make GPS history strongly durable on the synchronous write path, you will not meet the 562,500 writes/second requirement.

---

### 3d. Capacity Estimation

**Walk through this formula-first, then anchor to numbers**:

**Scale anchors** (memorize these):
- 25 million active riders, 5 million active drivers globally
- 20 million trips per day (Uber reported ~19M trips/day in 2023)
- Peak factor: 2× (evening commute 5–7 PM)
- Each driver sends GPS update every 4 seconds while online
- 30% of drivers online at peak → 1.5 million concurrent online drivers

**GPS write load** (the dominant load — always compute this first):
- Formula: online_drivers × (1 update / 4 s) × peak_factor
- 1,500,000 × 0.25 × 1.5 = **562,500 GPS writes/second**
- Bandwidth: 562,500 × 64 bytes (compressed protobuf) = **36 MB/s inbound**

**Trip request load**:
- Formula: trips_per_day / seconds_per_day × peak_factor
- 20,000,000 / 86,400 × 2 = **462 trip requests/second**

**ETA request load** (the second biggest compute load):
- 500K active trips, ETA refreshed every 30 seconds = 500K/30 = **16,667 ETA requests/second**
- Plus fare estimates (531/s) and matching pickup ETA (531/s)
- Total: **~18,000–28,000 ETA requests/second at peak**

**CPU for ETA**:
- CH query takes ~3 ms per request
- 27,770 queries/s × 3 ms = 83,310 ms of CPU per second = **~84 CPU cores** for routing alone

**Storage** (derive per entity):
- GPS: 32.4 billion points/day × 58 bytes effective (RF=3, LZ4) = **~750 GB/day raw GPS**; 90-day retention = **~67 TB**
- Trips: 20M/day × 2 KB = **40 GB/day**; 7-year retention = **~102 TB**
- Redis current positions: 1.5M drivers × 64 bytes = **96 MB** — fits trivially in memory

**Architecture implications from estimation**:
- 562,500 GPS writes/s → cannot write to any relational DB synchronously; must use Kafka + Redis on the write path
- 96 MB for all driver positions → Redis Geo works perfectly; no sharding needed for most cities
- 67 TB GPS history → Cassandra with TTL; PostgreSQL cannot handle this write volume
- 84 CPU cores for ETA routing → must use CH preprocessing; plain Dijkstra would require 10,000+ cores

**Time to spend**: 5–7 minutes on estimation. Show the GPS write number prominently — it justifies every major architectural decision.

---

### 3e. High-Level Design (HLD)

**Draw in this order on the whiteboard**:

1. Two client boxes: Rider App and Driver App
2. API Gateway (TLS termination, JWT validation, rate limiting)
3. Core services: Trip Service, Driver Service, Matching Service, Fare/Pricing Service
4. Kafka message bus in the middle
5. Storage layer: Redis Geo + Cassandra + PostgreSQL (Citus)
6. Downstream consumers: Notification Service, Payment Service, WebSocket Dispatcher

```
[Rider App]  [Driver App]
     |              |
  HTTPS/WebSocket   HTTPS/WebSocket (GPS updates via protobuf)
     |              |
     v              v
[API Gateway / Load Balancer]
  (JWT auth, rate limiting, TLS)
     |
  ┌──┼──────────────────────────────┐
  |  |                              |
[Trip Service]  [Driver Service]  [Fare/Pricing Service]
(state machine,  (availability,    (surge engine,
 CAS UPDATEs,    GPS ingestion,    H3 hex, estimate)
 ACID txns)      location lookup)
  |
[Matching Service]
(GEORADIUS, scoring, CAS assign)
  |
[Kafka: location-updates, trip-events, payment-events]
  |          |              |
[Redis Geo]  [Cassandra]   [WebSocket Dispatcher]
(hot pos.)   (GPS history)  (→ rider apps)
  |
[Trip DB: PostgreSQL+Citus]
  |
[Payment Service]  [Notification Service]
```

**4–6 key components and their roles**:

- **Trip Service**: Owns the 6-state trip state machine. Writes to PostgreSQL with CAS UPDATE. Publishes every state transition to Kafka `trip-events`. The single authority for trip status.
- **Matching Service**: Queries Redis Geo via GEORADIUS, scores 50 candidates with a 4-factor weighted function, attempts CAS assignment on the best candidate, waterfalls to the next if CAS fails, expands radius if no candidates found.
- **Location Ingest Service**: Validates and rate-limits driver GPS updates. Synchronously writes to Redis Geo (sub-millisecond, critical for matching). Asynchronously publishes to Kafka (non-blocking to driver app).
- **ETA Service**: Stateful pods, each holding a city's Contraction Hierarchy graph in process memory. Routes requests via consistent hash on city_id. Applies real-time traffic from Redis edge-speed cache and ML correction factor post-query.
- **Redis Cluster**: The hot-path store for everything that needs sub-millisecond reads: driver positions (Geo sorted sets), driver telemetry (Hash with 30s TTL), matching in-flight dedup (SETNX with 30s TTL), traffic edge weights, WebSocket server registry, surge multiplier per H3 cell.
- **Kafka**: Durable fan-out bus. Decouples the GPS ingestion path from three independent consumers: Cassandra writer (GPS history), WebSocket Dispatcher (rider tracking), ETA recalculation. Topic `location-updates` partitioned by driver_id for ordering.

**Key architectural decisions to state explicitly**:
1. "I chose Redis Geo over PostGIS because Redis executes GEORADIUS in under 1 ms; a database round-trip would blow the matching latency budget."
2. "I'm writing GPS to Redis synchronously and Kafka asynchronously because Redis self-heals in 4 seconds and Kafka provides durability — if I reversed this, I'd lose matching accuracy, not just a GPS point in history."
3. "PostgreSQL is sharded by city_id via Citus because all trip queries are city-scoped, this gives data locality, enables cross-table joins without cross-shard hops, and aligns with GDPR data residency."

---

### 3f. Deep Dive Areas

These are the three areas interviewers probe most deeply. Bring them up yourself before being asked.

**Deep Dive 1: Atomic Driver Assignment (Trip State Machine)**

*The problem*: Two drivers both receive the same trip offer and both tap Accept at the same moment. Without protection, both CAS UPDATEs could succeed, assigning two drivers to one trip.

*The solution*: Two sequential CAS UPDATEs in PostgreSQL:

```sql
-- Step 1: Claim the trip
UPDATE trips
SET status = 'DRIVER_ASSIGNED', driver_id = $driver_id
WHERE trip_id = $trip_id AND status = 'REQUESTED'
RETURNING trip_id;
-- 0 rows returned → another driver won → return 409 Conflict

-- Step 2: Mark driver unavailable
UPDATE drivers
SET status = 'ON_TRIP'
WHERE driver_id = $driver_id AND status = 'AVAILABLE'
RETURNING driver_id;
-- 0 rows returned → driver went offline → rollback trip assignment
```

*Why this works*: The `WHERE status = 'REQUESTED'` clause is the lock-free check. PostgreSQL serializes concurrent UPDATEs on the same row. Exactly one of them will succeed; all others get 0 rows affected. No distributed lock manager required.

*Trade-offs to name unprompted*:
- ✅ No distributed lock: avoids lock expiry edge cases (Redis SETNX lock holder dies mid-transaction).
- ✅ Idempotent retry: if the driver retries POST /accept after a timeout, the Trip Service checks: trip is already DRIVER_ASSIGNED with my driver_id → return 200. Different driver_id → return 409.
- ❌ Two-table CAS is not a single atomic operation. If step 2 fails, step 1 must be rolled back. Implement with an explicit compensating UPDATE.
- ❌ A trip can get stuck in REQUESTED if the CAS loop exhausts all candidates. A background sweeper job fires every 30 seconds to detect trips stuck in REQUESTED for over 3 minutes and trigger re-matching or cancellation.

**Deep Dive 2: GPS Ingestion Pipeline at 562,500 Writes/Second**

*The problem*: You must simultaneously update the live position store (for matching), fan out to the rider tracking WebSocket, and durably store GPS history — without blocking the driver app ACK on any slow downstream.

*The solution*: Two-tier write path.

- **Synchronous (critical path, < 2 ms)**: Write to Redis Geo with GEOADD and update telemetry Hash. ACK driver app after this completes. The matching service can now see the new position.
- **Asynchronous (non-blocking)**: Publish protobuf message to Kafka topic `location-updates` partitioned by driver_id. Return to caller without waiting.
- **Three independent Kafka consumers**:
  - Cassandra Consumer: batch-inserts 500 rows per Cassandra write, using TWCS with 1-day window, LZ4 compression. Handles 562,500 writes/s with ~3 writers.
  - WebSocket Dispatcher: for each GPS point on an active trip, looks up rider's WebSocket server from Redis (`rider:{id}:ws_server`), forwards position update to that server, which pushes to rider app.
  - Traffic Ingest Pipeline: aggregates median driver speed per road edge over 5-minute windows, writes deltas to Redis for ETA service.

*Trade-offs to name unprompted*:
- ✅ Redis write failure does not prevent Kafka durability — the driver app still gets ACK (with a log warning). Kafka + Cassandra provide the durable copy.
- ✅ TWCS (TimeWindowCompactionStrategy) in Cassandra is critical at this volume. Standard STCS would cause compaction storms. TWCS aligns compaction windows with TTL windows, making old GPS data immutable before compaction, eliminating tombstone buildup.
- ❌ At-least-once Kafka delivery means Cassandra consumer may write duplicate GPS points. Cassandra's last-write-wins resolves this (same driver_id + recorded_at PK → idempotent).
- ❌ GPS updates are sent every 4 seconds even when the driver is stationary. Battery Optimizer Service detects stationary/highway patterns and pushes adaptive sampling instructions to the driver app, reducing to 8-second intervals when stopped.

**Deep Dive 3: ETA Computation — Contraction Hierarchies**

*The problem*: Plain Dijkstra on a 50-million-node city graph takes 100+ ms. At 27,770 ETA requests/second, that means 2.77 million CPU-seconds per second — physically impossible.

*The solution*: Contraction Hierarchies (CH) preprocessing.

- **Offline preprocessing (hours per city)**: Assign each node a contraction rank. Contract nodes in rank order, adding shortcut edges: for each neighbor pair (u, v) through contracted node w, if no shorter path exists, add shortcut u→v with weight = weight(u,w) + weight(w,v). Result: original graph + 100–150M shortcut edges per large city.
- **Query time**: Bidirectional A* on the CH graph only relaxes edges to nodes with higher rank. Search space collapses from millions of nodes to thousands. Result: **< 1 ms for long-distance queries, < 5 ms for urban queries**.
- **Dynamic traffic**: CH graph is static. Real-time traffic applied as an overlay: Traffic Ingest Pipeline writes current_speed - historical_baseline per edge_id to Redis Hash. Edge weight function reads this delta at query time. No CH rebuild needed — only leaf edges change.
- **ML correction**: After CH graph returns a raw ETA, an XGBoost model per city applies a correction factor (typically 0.9–1.1×) to account for systematic biases: parking structure exit time, stop sign delays, traffic signal timing not modeled in OSM.

*Trade-offs to name unprompted*:
- ✅ CH makes 27,770 q/s achievable with ~84 CPU cores. Without CH, you'd need 10,000+ cores.
- ❌ CH preprocessing takes hours per city. Road network changes (new roads, closures) require full rebuild. Mitigation: blue-green graph deployment — build new CH offline, swap pods, drain old ones gracefully.
- ❌ ETA pods are stateful (graph in memory). They cannot be terminated freely without first migrating city assignments to other pods. Consistent-hash routing at the load balancer assigns city_ids to pod sets deterministically.
- ❌ CH does not handle forbidden turns (no U-turn at intersection). For production accuracy, turn restrictions must be modeled by expanding nodes into turn nodes during preprocessing — adds complexity but is essential for accuracy in urban environments.

---

### 3g. Failure Scenarios

**How to frame failures at the senior/staff level**: Don't just say "if Redis goes down, use a fallback." Say: "I want to walk through the blast radius of each failure mode and tell you exactly what degrades, what stays up, and how we recover."

**Failure 1: Redis cluster unavailable**

- *Impact*: GEORADIUS calls from Matching Service fail → no new matches can be made. Rider-tracking WebSocket pushes stop (no ws_server lookup). New GPS writes to Redis fail (Kafka still receives them).
- *Mitigation*: Redis Sentinel / Redis Cluster for automatic failover (sub-30-second). Circuit breaker in Location Ingest Service: if Redis write fails, log and continue — Kafka has the data. Cassandra `driver_current_position` table as a fallback read path for current positions (serves 503 response with fallback caveat). During Redis outage, matching degrades to using Cassandra current positions, which have higher read latency (5–20 ms vs. < 1 ms) but preserve basic function.
- *Recovery*: Redis restores from RDB snapshot + AOF. Drivers self-heal position data within 4 seconds of reconnection (next GPS update).

**Failure 2: Trip Service database (PostgreSQL) unavailable**

- *Impact*: No new trips can be created, no state transitions, no payments. This is the most severe failure.
- *Mitigation*: Read replicas absorb trip history reads. Multi-AZ PostgreSQL (AWS RDS) with automatic failover in < 60 seconds. Active trips in flight continue to stream GPS (Kafka and Redis are independent). Rider apps show "service disruption" banner. Kafka outbox events persist — when DB recovers, outbox relay catches up.
- *Senior framing*: This is why the Outbox Pattern is critical. If we publish to Kafka inside the same DB transaction (two-phase commit), and the DB goes down mid-commit, we need the outbox relay to re-publish. Without it, trip events are lost and payment/notification services never receive them.

**Failure 3: Kafka unavailable**

- *Impact*: GPS history writes to Cassandra stop (consumer offline). Rider tracking WebSocket pushes stop. Traffic data for ETA goes stale. Notification events back up.
- *Mitigation*: This is why Redis write is synchronous and Kafka is async. Matching and current-position serving continue uninterrupted. Location Ingest Service buffers producer messages in memory (up to configurable backlog). When Kafka recovers, buffered messages drain. Cassandra write lag is acceptable (90-day archive can have a few minutes of lag). Rider tracking degrades to polling instead of push during outage.

**Failure 4: ETA Service pods unavailable**

- *Impact*: Fare estimates unavailable. New trip matching degrades (pickup ETA not available for display on driver offer screen, though scoring still works). En-route ETA updates to rider stop.
- *Mitigation*: ETA Service is not on the critical matching path (Matching uses ETA for display, not for scoring). A cached ETA (last known, up to 60 seconds stale) can be served to rider. Graph load time is ~30 seconds — pod restarts are slow. Maintain minimum 2 pods per city to avoid single-pod city failures.

---

## STEP 4 — COMMON COMPONENTS

These nine components appear across multiple problems in this pattern. Know each one cold: what it is, why it was chosen, how it is configured, and what breaks without it.

---

### Component 1: Redis Geo (GEOADD / GEORADIUS)

**Why used**: Matching Service needs to find all available drivers within 5 km of a pickup point in under 10 ms p99. Redis Geo stores driver positions in a sorted set with GeoHash-encoded scores. `GEORADIUS` executes as O(N + log M) — where N is the result set and M is the total set size — in under 1 ms for 1.5 million drivers (96 MB dataset).

**Key configuration**:
- One sorted set per city per driver status: `city:{city_id}:drivers:available`, `city:{city_id}:drivers:on_trip`
- Query: `GEORADIUS city:1:drivers:available {lng} {lat} 5 km WITHCOORD WITHDIST COUNT 50 ASC`
- Status change: `ZREM city:1:drivers:available {driver_id}` then `GEOADD city:1:drivers:on_trip {lng} {lat} {driver_id}`
- In newer Redis (6.2+): use `GEOSEARCH` instead of deprecated `GEORADIUS` — same semantics

**What breaks without it**: You'd fall back to PostGIS with a GiST index. That adds a database round-trip (5–20 ms) instead of an in-memory call (< 1 ms). Under the 3-second matching budget with multiple retries and radius expansions, this budget deficit compounds and p99 matching latency blows past 3 seconds. You also lose the ability to get ZCARD (driver count per status) in O(1) — that feeds the surge engine.

---

### Component 2: H3 Hexagonal Grid

**Why used**: Geographic aggregation that is uniform, hierarchical, and distortion-free on a sphere. Used for surge pricing cells, geofence containment checks, and spatial analytics on trips.

**Key configuration**:
- Resolution 7 for surge pricing cells (~5.16 km² per hex, ~10,000 hexes per large city)
- Resolution 9 for trip pickup indexing (`pickup_h3_index` column on trips table, used for heat-map analytics)
- Geofences pre-compute all H3 cells at resolution 9 that overlap their polygon boundary at creation time, stored as JSONB. Containment check = O(1) hash lookup (is driver's current H3 cell in the fence's cell set?) vs. PostGIS `ST_Contains` on every GPS point (much slower)
- Surge EMA smoothing: `new_multiplier = 0.7 × prev + 0.3 × raw` to prevent oscillation; ratchet rule: max ±0.5× per 60-second tick

**What breaks without it**: Surge pricing degrades to city-level granularity (a concert downtown shouldn't affect airport pricing). Geofence evaluation requires PostGIS `ST_Contains` on every one of 562,500 GPS points per second — computationally expensive. Analytics lose the spatial bucketing that makes heat maps and demand forecasting possible.

---

### Component 3: Apache Kafka (High-Throughput Event Streaming)

**Why used**: Decouples the high-frequency write path (GPS ingest at 562,500/s) from multiple independent downstream consumers. Provides durable, replayable, ordered event delivery. Enables independent scaling of producers and consumers.

**Key configuration**:
- Topic `location-updates`: partitioned by driver_id (ensures ordered delivery per driver, essential for Cassandra write ordering); 24-hour retention (Cassandra consumer is not expected to lag more than a few minutes)
- Topic `trip-events`: partitioned by trip_id; consumed by Notification Service, Payment Service, Analytics
- Replication factor 3 across AZs
- Producer: async (fire-and-forget) from Location Ingest Service — does not block driver app ACK
- Consumer group pattern: each consumer group (Cassandra writer, WebSocket dispatcher, Traffic ingest) maintains independent offsets

**What breaks without it**: Location Ingest Service must synchronously write to Redis (fast, done), Cassandra (slow, 1–5 ms), and WebSocket dispatch (variable latency) in sequence, increasing the ACK time from ~2 ms to ~30 ms. More critically, Cassandra unavailability would cause GPS writes to fail and driver app retries. WebSocket Dispatcher failure would propagate back to the driver ACK. Kafka provides failure isolation between consumers.

---

### Component 4: PostgreSQL with Citus (Sharded by city_id)

**Why used**: ACID guarantees for trip state transitions and payments. Rich SQL for analytics (match rate by city, fare breakdowns, ETA accuracy reports). Citus adds horizontal sharding via the `create_distributed_table` API, with `city_id` as the natural shard key.

**Key configuration**:
- All hot tables distributed: `create_distributed_table('trips', 'city_id')`, `create_distributed_table('matching_attempts', 'city_id')`, `create_distributed_table('eta_accuracy_log', 'city_id')`
- CAS trigger on trips table: PostgreSQL function validates valid state transitions and rejects invalid ones at the DB level (not just application level)
- Trip shard key = city_id: enables join of trips + drivers + ratings within a single shard; no cross-shard fan-out for common queries

**What breaks without it**: Without horizontal sharding, PostgreSQL hits write volume limits at ~20 million trips/day × 5 state transitions = 100,000 writes/day per trip — manageable, but 7-year retention of 102 TB is not. More importantly, strong consistency for trip assignment requires ACID; switching to MongoDB or DynamoDB means implementing your own two-phase commit for the double-CAS (trip + driver update).

---

### Component 5: Cassandra (GPS Time-Series with TTL + TWCS)

**Why used**: Cassandra's LSM-tree architecture converts random writes to sequential disk I/O, making 562,500 GPS writes/second achievable. Native per-row TTL eliminates a separate cleanup job for 90-day retention. Wide-row model with `PRIMARY KEY ((driver_id, trip_id), recorded_at)` gives O(1) time-range scan for trip replay.

**Key configuration**:
- `default_time_to_live = 7776000` (90 days in seconds)
- `compaction = TimeWindowCompactionStrategy, compaction_window_unit = DAYS, compaction_window_size = 1`
- `compression = LZ4Compressor`
- Replication factor 3 across two regions: `NetworkTopologyStrategy, 'us-east-1': 3, 'eu-west-1': 3`
- Batch writes: consumer batches 500 rows per write (never use `BATCH` in Cassandra for multi-partition writes; use application-level grouping + individual async writes)

**Why TWCS matters specifically**: At 562,500 writes/second, GPS data fills up SSTables fast. Standard STCS (SizeTieredCompactionStrategy) compacts based on SSTable sizes — it will trigger massive compaction storms as data accumulates. TWCS creates one compaction window per day. Data in older windows is immutable (no new writes). TWCS compacts each old window exactly once, then never again. TTL expiry on whole windows happens atomically. This is the only strategy that scales at this write volume without read/write amplification spiraling.

**What breaks without it**: PostgreSQL TimescaleDB can handle maybe 100,000 writes/second per node. Scaling to 562,500 requires 6+ nodes with hypertable partitioning and careful vacuum management. Redis cannot persist 90 days of data (cost, memory). DynamoDB at this volume becomes extremely expensive.

---

### Component 6: Redis Ephemeral State (Short TTL Hashes and Strings)

**Why used**: Many pieces of matching and tracking state are inherently ephemeral — they have no value after a few seconds or minutes. Using the database for this state adds unnecessary write load and requires cleanup jobs. Redis TTLs provide automatic expiry.

**Key configuration and TTL semantics**:
- `driver:{driver_id}:telemetry` — Hash (speed_kmh, heading, updated_at); **30-second TTL**: if a driver goes offline without sending an explicit OFFLINE status, their telemetry expires in 30 seconds, making them appear stale to the matching service (no "ghost driver" problem)
- `matching:inflight:{trip_id}` — String "1" with **30-second TTL**: SETNX (set-if-not-exists) prevents duplicate parallel matching runs on the same trip_id if the Trip Service retries; second caller gets DUPLICATE status immediately
- `trip:{trip_id}:offered_drivers` — Set of driver_ids; **60-second TTL**: security check on driver accept endpoint — reject any driver not in this set
- `driver:{driver_id}:pending_offer` — Hash with offer details; **20-second TTL**: slightly longer than the 15-second offer timeout for race-condition margin
- `fairness:{city_id}:{hour_epoch}:{driver_id}` — Integer counter; **3600-second TTL**: hourly request counter per driver, auto-expires after the window
- `traffic:{city_id}:edge_speeds` — Hash (edge_id → speed_kmh); **10-minute TTL**: if Traffic Ingest Pipeline stops, edge speeds auto-expire and ETA falls back to historical baselines

**What breaks without it**: In-flight dedup without Redis requires a DB table with a cleanup job — adds latency to every matching start. Offered-driver security checks require a DB lookup on every accept — adds 5–20 ms. Ghost drivers (telemetry never expires) cause the matching service to offer trips to drivers who went offline without notice, increasing the no-accept rate.

---

### Component 7: WebSocket for Real-Time Rider Push

**Why used**: Rider must see their driver's position update every 4 seconds during approach. HTTP polling at 5-second intervals means 25 million active sessions × 1 request/5s = 5 million HTTP requests/second — a massive, unnecessary load. WebSocket maintains a persistent connection and pushes updates, eliminating polling entirely.

**Key configuration**:
- WebSocket Dispatcher consumes Kafka `location-updates`; for each GPS point on an active trip, looks up `rider:{rider_id}:ws_server` in Redis (60-second TTL, refreshed by heartbeat) to find which WebSocket server holds the connection; forwards update to that server
- Driver location push frequency: 1 push per 4 seconds per active trip; 500K active trips × 0.25 push/s = 125,000 push/s; at 200 bytes per push = 25 MB/s outbound
- ETA updates are piggybacked on location pushes — no separate stream

**What breaks without it**: Polling requires the rider app to open an HTTP connection every few seconds. At 500K active trips, that is 125,000 connections/second just for location polling — connection overhead dominates. More critically, HTTP polling introduces 0 to (polling interval) latency; WebSocket push reduces visible lag to near-real-time. Riders experience the driver "jumping" on the map with polling; WebSocket gives smooth movement.

---

### Component 8: CAS UPDATE for Atomic Trip State Transition

**Why used**: The single most critical correctness guarantee in the system. Two drivers accepting the same trip simultaneously must result in exactly one assignment — the other gets a 409 Conflict. Without CAS, TOCTOU bugs (check-then-act race) allow double-assignment.

**Key configuration**:
```sql
UPDATE trips
SET status = 'DRIVER_ASSIGNED', driver_id = $1
WHERE trip_id = $2 AND status = 'REQUESTED'
RETURNING trip_id;
```
Zero rows returned means another driver won. One row returned means success.

A PostgreSQL trigger additionally validates that all state transitions match the allowed state machine (REQUESTED → DRIVER_ASSIGNED → DRIVER_EN_ROUTE → RIDER_PICKED_UP → TRIP_IN_PROGRESS → COMPLETED; CANCELLED reachable from first three states). This is defense-in-depth — even if application code has a bug, the DB rejects invalid transitions.

**What breaks without it**: Without CAS, you need a distributed lock (Redis SETNX or Zookeeper). Redis locks are advisory and require fencing tokens for true safety. Zookeeper adds operational complexity and a new failure domain. The CAS pattern is superior: it is atomic at the database level, requires no external lock service, and is naturally idempotent.

---

### Component 9: JWT Bearer Token Auth + Rate Limiting at API Gateway

**Why used**: Services are stateless (JWT) and clients are untrusted (rate limiting). JWT RS256 tokens (signed with asymmetric key) allow any service to verify a token without calling a central auth service. Rate limiting at the gateway prevents a malfunctioning driver app from flooding the GPS ingest endpoint.

**Key configuration**:
- JWT: RS256 signed, 1-hour expiry; payload includes `sub` (user UUID), `role` (rider|driver|admin), `city_id`
- Driver rate limit: 120 req/min (burst 40/s) — higher than rider because GPS updates are frequent
- Rider rate limit: 60 req/min (burst 20/s)
- GPS endpoint rate: 30 updates/min per driver (one every 2 seconds minimum, even if app tries faster)
- Internal gRPC (Matching Service ↔ ETA Service): service-to-service JWT with `role=service`, not subject to end-user rate limits

**What breaks without it**: Without rate limiting, a single misbehaving driver app that submits GPS updates 10× per second (instead of 1/4s) can generate 10× the expected write load. At 1.5 million drivers, even 0.1% of drivers misbehaving would add 1,500 rogue drivers × 2.5 extra writes/s = 3,750 extra writes/s — enough to saturate the ingest service. Without JWT, you need a central session store that is a latency bottleneck and single point of failure.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Uber / Lyft (Core Platform)

**Unique things not in any other problem**:
- **Full trip lifecycle state machine** with 6 states, enforced at both the DB trigger level and application level with CAS UPDATE across two tables (trips + drivers)
- **Surge pricing engine**: Flink batch job (runs every 60 seconds per city) counts demand (open requests in last 5 min) and supply (available drivers) per H3 resolution-7 cell, computes demand/supply ratio, maps to multiplier via configurable breakpoints, smooths with EMA (α=0.3) and a ratchet rule, writes to Redis with 2-minute TTL for fare estimate reads
- **Payment service integration**: Stripe/Braintree tokenization (never store raw card numbers), idempotent charge via payment_reference key on trip completion, ACH payout to drivers weekly
- **Ratings system**: Incremental rolling average on `drivers.rating` column (not recomputed from log each time); mutual rating (rider rates driver, driver rates rider) post-trip
- **Outbox Pattern**: Trip state changes written to `trip_outbox` table in the same DB transaction as the trip update; outbox relay publishes to Kafka; ensures no lost Kafka events even if Trip Service crashes mid-publish

**Differentiator in two sentences**: Uber/Lyft is the only problem in this pattern that covers the full business flow: a 6-state ACID transaction lifecycle, a surge pricing engine that reads H3 cells and smooths multipliers, a tokenized payment system with idempotent charge, and post-trip ratings. The unique challenge is orchestrating all of these dependent steps (match → en route → pickup → complete → charge → rate) with exactly-once semantics for the charge and strong consistency for the assignment.

---

### Location Tracking

**Unique things not in any other problem**:
- **Adaptive battery optimization**: Battery Optimizer Service analyzes driver speed and heading variance; if speed < 5 km/h for > 60 seconds (stationary), pushes `update_interval_ms=8000, reason=STATIONARY` to driver app via WebSocket; reduces battery drain ~40% for idle drivers waiting for rides
- **Road-snapping (async)**: Raw GPS coordinates snapped to nearest road segment asynchronously after storage. `road_snapped_lat/lng` and `road_segment_id` are nullable in Cassandra, backfilled by a road-snap worker. Non-blocking to driver app ACK.
- **Geofencing**: Four zone types (AIRPORT_ZONE, CITY_BOUNDARY, RESTRICTED_ZONE, SURGE_ZONE); 56,250 evaluations/second (10% of GPS writes); Geofence Evaluation Service holds all active geofence H3 cell sets in memory; containment check is O(1) hash lookup
- **Trip GPS replay**: Server-Sent Events stream of Cassandra time-range query at configurable playback speed (1× to 60×); used for dispute resolution and earnings verification
- **Dual Cassandra access patterns**: `driver_gps_history` (partition by driver_id + trip_id, 90-day TTL) for replay; `driver_gps_by_driver` (partition by driver_id only, 7-day TTL) for "show all locations for driver X in last week" without knowing trip_id

**Differentiator in two sentences**: Location Tracking's core challenge is the multi-consumer fan-out architecture: one GPS update triggers four independent downstream pipelines (Redis Geo update, Cassandra history write, WebSocket rider push, geofence evaluation) all running in parallel via Kafka, each with different failure modes and latency SLAs. It also models the battery-device relationship that no other problem covers: the server adapts the sampling rate pushed to each driver based on their movement state, trading GPS precision for device battery life.

---

### Driver Matching

**Unique things not in any other problem**:
- **Multi-factor composite scoring**: composite = 0.60×distance_score + 0.20×rating_score + 0.10×acceptance_rate_score + 0.10×heading_score; all features normalized to [0,1] before weighting; heading score = (cos(bearing_diff) + 1) / 2; scores stored in `driver_offers.score_breakdown JSONB` for offline analysis
- **Fairness & Cooldown System**: per-city hourly request counters per driver in Redis; if a driver receives > 2× the city average requests in one hour, they enter a 5-minute cooldown (excluded from matching); prevents high-score drivers from monopolizing all high-value pickups
- **Matching Config Service**: per-city configuration table (`matching_config`) storing scoring weights, radius expansion steps, offer timeout, min driver rating; read-through cached in Matching Service; enables A/B testing different matching parameters per city without code deployment
- **In-flight deduplication**: Redis SETNX `matching:inflight:{trip_id}` (30-second TTL) prevents the Trip Service from launching two concurrent matching workflows for the same trip_id (e.g., on retry after timeout)
- **ETABatch for pre-offer display**: Matching Service calls `ETAService.ComputeETABatch` for all 50 candidates in one gRPC call before sending the offer; adds pickup ETA to the driver offer screen (not used in scoring formula)

**Differentiator in two sentences**: Driver Matching is the only problem that explicitly models fairness — the observation that a pure score-maximizing system would send all requests to the top 5% of drivers, starving the rest of income. The unique design is the composite scoring function (distance-heavy at 60%, but with quality and heading signals) combined with a cooldown system implemented entirely in Redis counters with TTL-based expiry, without any distributed lock or dedicated fairness service.

---

### ETA Service

**Unique things not in any other problem**:
- **Contraction Hierarchies (CH) algorithm**: the only problem in this pattern with an explicit graph algorithm. CH preprocessing (hours per city) adds shortcut edges and assigns node ranks; bidirectional A* at query time only relaxes upward-rank edges, reducing search space from millions to thousands of nodes; query time < 1 ms (long trips), < 5 ms (urban)
- **Stateful city-scoped pods**: ETA pods are not stateless — each pod loads specific city graphs into process memory (~3–5 GB per large city). Consistent-hash routing at the load balancer ensures all queries for a given city_id route to the same pod set. This is a deliberate trade-off of deployment flexibility for performance.
- **Three-layer speed model**: (1) historical 7-day rolling average per edge per (hour, day_of_week) from Flink aggregation stored in Apache Parquet on S3; (2) real-time probe delta from active drivers via Kafka, written to Redis Hash every 5 minutes; (3) ML correction factor (XGBoost per city, 0.9–1.1×) applied post-graph-query
- **ETABatch API**: one gRPC call returns ETAs for up to 50 driver-to-pickup pairs; used by Matching Service to get display ETAs for all candidates without 50 sequential RPCs
- **Accuracy monitoring pipeline**: on trip completion, actual duration compared to predicted ETA at trip start; MAPE tracked by city/hour/route_type; feeds ML retraining trigger

**Differentiator in two sentences**: ETA Service is the only problem in this folder with graph algorithms, ML inference, and the statefulness constraint — the CH graph cannot be served from a shared store like Redis because it requires in-process memory for zero-latency traversal. The three-layer accuracy stack (historical patterns + real-time probe delta + ML post-correction) solves the fundamental problem that a static graph gives wrong ETAs during rush hour, accidents, and special events without requiring expensive graph rebuilds.

---

## STEP 6 — Q&A BANK

### Tier 1: Surface Questions (expect these in the first 15 minutes)

**Q1: How do you find available drivers near a pickup point efficiently?**

**KEY PHRASE: Redis GEORADIUS on in-memory sorted sets**. All available driver positions are stored in a Redis sorted set per city per status (`city:{city_id}:drivers:available`), with positions encoded as GeoHash scores. `GEORADIUS` (or `GEOSEARCH` in Redis 6.2+) executes O(N + log M) in under 1 ms for a 1.5-million-driver dataset that fits in 96 MB of RAM. This is the only approach that meets the sub-10-ms read requirement for driver positions. When a driver goes offline or starts a trip, they are atomically removed from the available set and added to the appropriate status set.

**Q2: How do you handle 562,500 GPS writes per second?**

**KEY PHRASE: Two-tier write path — Redis synchronous, Kafka asynchronous**. The Location Ingest Service does two things per GPS update: synchronously updates Redis Geo (< 1 ms) so the matching service always has fresh positions, then asynchronously publishes to Kafka (non-blocking) for durable fan-out to Cassandra (GPS history), WebSocket Dispatcher (rider tracking), and the Traffic Ingest Pipeline (ETA). The driver app ACKs after the Redis write succeeds. Cassandra handles the volume with TWCS compaction and RF=3 durability.

**Q3: How do you prevent two drivers from being assigned to the same trip?**

**KEY PHRASE: Compare-and-swap UPDATE with status condition**. The Trip Service executes `UPDATE trips SET status='DRIVER_ASSIGNED', driver_id=? WHERE trip_id=? AND status='REQUESTED'`. If another driver already accepted, the WHERE clause fails and 0 rows are returned — the second driver gets a 409 Conflict response. No distributed lock is needed because PostgreSQL serializes concurrent UPDATEs on the same row. A second CAS UPDATE on the drivers table (`WHERE status='AVAILABLE'`) marks the driver unavailable atomically.

**Q4: What databases do you use and why?**

**KEY PHRASE: Right database for each access pattern**. PostgreSQL (with Citus sharding by city_id) for trips, payments, and matching logs because ACID is non-negotiable for money and assignment. Redis Geo for current driver positions because sub-millisecond GEORADIUS is the only way to meet matching latency. Cassandra for GPS history because 562,500 appends/second with 90-day TTL is exactly its design sweet spot. In-process memory for the ETA road graph because any I/O (network or disk) per graph node traversal would make CH queries infeasible. Each database is chosen because no other technology satisfies that specific combination of access pattern and performance requirement.

**Q5: How do you compute surge pricing?**

**KEY PHRASE: H3 hexagonal cells with demand/supply ratio and EMA smoothing**. Every 60 seconds, the Surge Engine reads all available driver positions from Redis and counts drivers per H3 resolution-7 hexagon (supply). It queries PostgreSQL for open requests in the last 5 minutes per pickup H3 cell (demand). The demand/supply ratio maps to a multiplier via configurable breakpoints (e.g., ratio 2.0 → 1.5×). An exponential moving average (α=0.3) prevents oscillation, and a ratchet rule caps change at ±0.5× per tick. Results are written to a Redis Hash per city with 2-minute TTL, read on every fare estimate request.

**Q6: How does real-time driver tracking work from GPS to rider screen?**

**KEY PHRASE: Kafka fan-out to WebSocket Dispatcher with Redis-backed session routing**. Driver app sends GPS update → Location Ingest Service writes to Redis Geo and publishes to Kafka `location-updates`. WebSocket Dispatcher consumes from Kafka and, for each GPS point on an active trip, looks up `rider:{rider_id}:ws_server` in Redis to find which WebSocket server holds that rider's connection, then forwards the position update to that server. The server pushes the position to the rider's app. End-to-end latency: driver GPS event to rider screen under 2 seconds p99.

**Q7: What is the trip state machine?**

**KEY PHRASE: Six states enforced by CAS UPDATE and a DB-level trigger**. States: REQUESTED → DRIVER_ASSIGNED → DRIVER_EN_ROUTE → RIDER_PICKED_UP → TRIP_IN_PROGRESS → COMPLETED. CANCELLED is reachable from the first three states. State transitions are enforced at two layers: a PostgreSQL trigger function validates that each UPDATE moves to a permitted next state (and raises an exception otherwise), and the application code runs a CAS UPDATE with the current state in the WHERE clause. Every transition is logged to `trip_state_log` for auditing.

---

### Tier 2: Deep Dive Questions (expect these after your HLD)

**Q1: Why Cassandra for GPS history instead of TimescaleDB or InfluxDB?**

**KEY PHRASE: LSM-tree + TWCS = the only compaction strategy that doesn't die at 562,500 writes/second**. TimescaleDB handles ~100K writes/second per node — you'd need 6+ nodes. InfluxDB could work (500K+ writes/s) but TWCS is the key differentiator: GPS data is append-only and time-bucketed. TWCS creates immutable compaction windows per day. Old windows compact exactly once and are never touched again. This eliminates tombstone buildup from TTL expiration and prevents compaction storms that STCS would generate. Combined with per-row 90-day TTL (no cron job needed) and native RF=3 durability, Cassandra is the right tool. The partition key `(driver_id, trip_id)` with clustering on `recorded_at ASC` makes the dispute-resolution time-range scan O(1) per trip.

**Q2: What happens during a Redis failure when matching is needed?**

**KEY PHRASE: Circuit breaker to Cassandra fallback with graceful degradation**. Redis Sentinel or Redis Cluster handles auto-failover in under 30 seconds. During the outage window: (1) Location Ingest Service circuit-breaks the Redis write path, logs GPS points to Kafka-only (not great but not catastrophic — Kafka is durable). (2) Matching Service falls back to reading `driver_current_position` from Cassandra — this table has current positions per driver but no geospatial index. The Matching Service would need to pull all available drivers for a city and filter by distance in-process (acceptable at 5–10% of normal traffic if city driver counts are manageable). (3) Rider tracking WebSocket pushes stop because `ws_server` lookup fails. Riders see a "location update paused" state. This is a degraded but not dead system — trips already in progress continue, and new trips are slower to match but not impossible.

**Q3: How do you keep ETA accurate during a major traffic incident like a highway closure?**

**KEY PHRASE: Real-time probe delta overrides historical baseline within 5 minutes**. The Traffic Ingest Pipeline consumes Kafka `location-updates`, groups GPS speed readings by road segment over 5-minute windows, computes median speed per edge, and writes `current_speed - historical_baseline` as a delta to Redis `traffic:{city_id}:edge_speeds`. When the highway closes, all drivers on it slow to 0 km/h — the median speed for those edges drops to near zero, the delta goes sharply negative, and within 5 minutes the edge weight in the ETA computation reflects the closure. The ETA pods read the delta from Redis on every query (`EdgeWeight()` function). No graph rebuild is needed. The ML correction model is less responsive (it corrects systematic biases, not acute incidents) — the probe-based delta handles the incident, ML handles normal daily patterns.

**Q4: How does the matching fairness system work, and why is it important?**

**KEY PHRASE: Per-driver hourly request counters with cooldown TTL in Redis**. Without fairness control, the highest-scoring drivers (high rating + good acceptance rate + always close to downtown) would receive a disproportionate share of all requests, earning far more than similar drivers. The Fairness Subsystem maintains `fairness:{city_id}:{hour_epoch}:{driver_id}` as an integer counter in Redis, incremented on each offer. The city average `fairness:{city_id}:{hour_epoch}:avg` is maintained separately. If a driver's counter exceeds 2× the city average in a given hour, they enter a 5-minute cooldown (Redis key `driver:{driver_id}:cooldown` with 300-second TTL). During cooldown, they are excluded from matching unless no non-cooldown drivers are available (fallback to prevent complete failure). This is important both for income equity among gig workers and for business reasons — drivers who stop getting requests churn off the platform.

**Q5: What is the Outbox Pattern and why is it critical here?**

**KEY PHRASE: Same-transaction Outbox write guarantees no lost Kafka events after DB commit**. Without the Outbox Pattern: the Trip Service writes a state transition to PostgreSQL, then publishes to Kafka. Between these two operations, the service can crash — the DB commit succeeded but the Kafka publish never happened. Payment Service never receives `TRIP_COMPLETED`, so the rider is never charged. With the Outbox Pattern: the Trip Service writes the state transition AND an outbox record to the same DB transaction. A separate Outbox Relay process polls `trip_outbox` for unshipped events and publishes them to Kafka. The Relay is idempotent: it tracks Kafka offsets and skips already-published events. Now, a crash between DB commit and Kafka publish leaves an unshipped outbox record — the Relay picks it up on recovery. This guarantees exactly-once Kafka delivery for trip events with respect to the DB state.

**Q6: Why are ETA Service pods stateful, and what problems does that cause?**

**KEY PHRASE: CH graph must be in process memory for zero-latency traversal — network I/O per node is O(millions of requests per query)**. The CH algorithm visits thousands of nodes per ETA query, each requiring an adjacency-list lookup. If the graph were in Redis (via hash lookups), each node visit would be a network call — even at 0.1 ms per call, 10,000 node visits per query = 1,000 ms per query. The graph must be in process memory for pointer-dereference-speed access. This means pods are stateful (they own specific city graphs). Stateful pods break the standard "kill and restart freely" assumption of stateless microservices. Problems: (1) Pods take ~30 seconds to load from S3 on startup — can't scale instantly. Mitigate with pre-warming before scale events. (2) Pods can't be terminated without traffic migration to another pod holding the same city. Mitigate with consistent-hash routing and graceful drain. (3) Graph updates (new roads) require a rolling restart with blue-green deployment — build new CH graph offline, route new traffic to updated pods, drain old pods.

**Q7: How do you handle the scoring function producing the same score for multiple drivers?**

**KEY PHRASE: Tie-breaking micro-term + fallback to physical distance**. The composite score is a sum of weighted normalized features. Two drivers at 2.0 km and 2.1 km with identical ratings and acceptance rates will score very close together. The implementation adds a tiny tie-breaking term: `composite += (1.0 / (distance_km + 0.1)) × 0.0001`. This term is small enough that it never overrides a meaningful score difference but always breaks ties in favor of the closer driver. The constant 0.0001 keeps the tie-break contribution an order of magnitude smaller than the smallest meaningful score difference (~0.01). This is important for fairness: without tie-breaking, arbitrary ordering of equal candidates could introduce systematic bias toward certain drivers.

---

### Tier 3: Staff+ Stress Test Questions (reason aloud, show trade-offs)

**Q1: Uber had a major city (NYC, 200K concurrent drivers) where the Redis Geo sorted set hit memory limits. How would you redesign the spatial lookup for that city?**

*Reasoning aloud*: The first question is whether this is actually a problem. 200,000 drivers × 64 bytes per Geo entry = 12.8 MB — that trivially fits in memory, so the premise is likely about query performance, not memory. `GEORADIUS` on 200K members with COUNT 50 returns in under 5 ms, which is still within the matching budget. If scale is 10× that (2M drivers, e.g., globally shared pool), I'd approach it in layers. Layer 1: shard by H3 resolution-5 parent cell (about 30 cells cover a large city) — split the sorted set into district-level sets and fan out the GEORADIUS to parallel queries on relevant cells, merge results. Layer 2: if even that doesn't work, consider pre-clustering driver positions — for the fare estimate screen, a coarser "how many drivers are nearby" count doesn't need exact positions, so a separate count-by-H3-cell sorted set serves that without hitting the Geo set. Layer 3: for extreme scale, consider a distributed spatial service like S2 geometry + a purpose-built in-memory index (like the one Uber actually built, dubbed "H3 + in-process index with gRPC fan-out to city shards"). The key insight is that the Geo set per status set per city is already a natural shard — adding a second level of sharding (by district) is the natural extension.

**Q2: Design the end-to-end system to handle a massive demand spike — a Taylor Swift concert ending in Chicago with 100,000 simultaneous ride requests in a 5-minute window. Walk through every component's behavior.**

*Reasoning aloud*: This is about understanding where each bottleneck appears and how the system degrades gracefully. 100,000 requests in 5 minutes = 333 requests/second sustained, but the spike is the first 60 seconds where maybe 40,000 requests hit simultaneously = 667 requests/second. Normal capacity is 462/s, so this is a 1.5× spike — serious but not catastrophic with horizontal scaling. Walk through each layer: API Gateway — rate limits per user prevent any single user from flooding; horizontal scaling of ALBs handles connection surge. Trip Service — sharded by city_id, Chicago all goes to the same Citus shard; this is the bottleneck. CAS UPDATEs on the trips table will see heavy contention on hot rows; connection pool exhaustion is the risk. Mitigation: read replicas for status checks, connection pooling via PgBouncer, and potentially queuing trip requests in Redis with a Lua script (atomic increment + bounded queue). Matching Service — 50 available drivers per GEORADIUS call now becomes a very scarce resource if only 8,000 drivers are available. Many requests will exhaust the initial 5 km radius and expand. Match success rate drops. Surge pricing — the H3 surge engine (running every 60 seconds) will detect the demand spike and ramp the multiplier from 1.0× to potentially 3.0–4.0× within 2 minutes. This price signal reduces demand and attracts off-duty drivers to go online. This is by design — surge is the safety valve for demand spikes. Notification Service — 100,000 push notifications in 60 seconds = 1,667 push/s; FCM/APNs can handle this. The key insight: the system is designed to degrade gracefully under demand spikes by queuing requests (not dropping them), amplifying price signal (surge), and communicating wait times to riders — not by pretending it can instantly serve everyone.

**Q3: A driver reports that they were incorrectly charged a no-show fee (the system marked them as arriving at pickup but the GPS showed they were actually 2 km away). How do you debug this, and what system changes prevent it in the future?**

*Reasoning aloud*: This is a data consistency and auditing question. First, what data do I have? The `trip_state_log` table records every state transition with actor (DRIVER), timestamp, and any metadata. The `driver_gps_history` Cassandra table stores every GPS point for that driver during that trip. I would: (1) Query Cassandra for the full GPS trace for that `(driver_id, trip_id)` around the time of the arrival event. (2) Query `trip_state_log` for the DRIVER_EN_ROUTE → RIDER_PICKED_UP transition timestamp. (3) Compare the GPS coordinates at that timestamp with the pickup_lat/lng from the trips table. If the driver's GPS shows them 2 km away at the time of the "arrived" event, the driver's claim is likely valid. Root causes to investigate: did the driver's GPS have poor accuracy (accuracy_m value in Cassandra was > 500 m)? Did the driver's app allow them to tap "arrived" without GPS validation? Did the app trigger an automatic arrival based on a geofence but the geofence was misconfigured? System changes: (1) Add a GPS validation step to the arrival endpoint — reject `POST /trips/{id}/arrived` if driver's last known GPS position is > 200 m from pickup_lat/lng (with GPS accuracy tolerance). (2) Add arrival GPS snapshot to `trip_state_log.metadata` — store the GPS position at the moment of the arrived event, so disputes can always be resolved from the log without cross-referencing Cassandra. (3) Review the geofence trigger for auto-arrival — if it fires on a driver passing through a large pickup geofence (e.g., airport zone), the trigger radius may be too large.

---

## STEP 7 — MNEMONICS

### Mnemonic: "GRAPH CLOCKS"

Use this to remember the nine shared components and their purposes:

- **G** — GPS two-tier write: Redis synchronous, Kafka async
- **R** — GEORADIUS on Redis sorted sets for sub-ms spatial lookup
- **A** — Atomic CAS UPDATE for double-assignment prevention
- **P** — PostgreSQL + Citus sharded by city_id for ACID relational data
- **H** — H3 hexagonal cells for surge pricing and geofence checks
- **C** — Cassandra with TWCS + TTL for GPS time-series history
- **L** — Load-balanced WebSocket push for real-time rider tracking
- **O** — Outbox Pattern for guaranteed Kafka publish after DB commit
- **C** — Contraction Hierarchies in-memory for sub-ms ETA routing
- **K** — Kafka for durable fan-out across four independent consumers
- **S** — Scoring function (4-factor weighted composite) with fairness cooldown

### Opening one-liner for each problem

**Full Uber/Lyft platform**: "This is a two-sided real-time marketplace where the hard problems are strong consistency on the money side (assignment is once, charges are once) and extreme throughput on the telemetry side (562,000 GPS writes per second), so the design separates those two planes — Redis + Kafka for telemetry, PostgreSQL CAS for money."

**Location Tracking**: "The core challenge is that one GPS update from a driver needs to simultaneously feed four consumers — Redis for matching, Cassandra for history, WebSocket for the rider, and geofencing — each with different SLAs, so Kafka fan-out is the only viable decoupling mechanism."

**Driver Matching**: "Matching is a race condition disguised as a geospatial problem — finding drivers is easy with Redis GEORADIUS, but assigning exactly one without a distributed lock requires CAS UPDATE, and scoring fairly without monopolization requires per-city fairness counters in Redis."

**ETA Service**: "Standard Dijkstra on a 50-million-node graph takes hundreds of milliseconds; at 27,000 requests per second that is physically impossible, so the entire design revolves around Contraction Hierarchies preprocessing — which makes pods stateful and forces consistent-hash routing — layered with real-time traffic from driver probe speeds."

---

## STEP 8 — CRITIQUE

### What these source files cover well

- **GPS ingestion pipeline**: Extremely thorough. The two-tier Redis + Kafka design, TWCS rationale, Cassandra partition key design, battery optimization, and Go pseudocode for the ingest service are all production-quality.
- **CAS UPDATE pattern**: Well explained across both Uber/Lyft and Driver Matching. The PostgreSQL trigger for state transition validation is a sophisticated touch most candidates miss.
- **Contraction Hierarchies**: Exceptional depth. The CH preprocessing pseudocode, query algorithm in Go, and dynamic weight overlay are explained clearly and accurately.
- **Data modeling**: Schema is detailed and realistic. H3 index on trips table, TWCS Cassandra configuration, Redis key naming conventions with TTLs — all correct.
- **Capacity estimation**: Numbers are internally consistent and anchor to real Uber scale (~19M trips/day). The ETA CPU core calculation (84 cores) is a strong differentiator that few candidates compute.
- **Surge pricing**: The H3 cell design with EMA smoothing and ratchet rule reflects actual production complexity; most candidates stop at "compute supply/demand ratio."

### What is missing or shallow

- **Global multi-region replication**: The source files mention multi-AZ but don't deeply cover what happens when the US-East-1 region goes down and US-West-2 must serve NYC. Active-active vs. active-passive trade-offs for the Trip DB are not covered. This is a common Staff-level follow-up.

- **Driver-to-rider communication (in-app chat/call)**: Listed as out of scope but interviewers sometimes probe this. A brief acknowledgment that this uses WebRTC (P2P audio) via a STUN/TURN server with number masking (Twilio proxy) would strengthen the answer.

- **Pool / Shared matching**: Mentioned as a feature but the route compatibility algorithm is not described. For Staff-level interviews, being able to say "Pool matching requires evaluating detour time — if adding a second rider increases the first rider's trip by more than X minutes, reject the match — and uses multi-waypoint ETA to compute the combined route" is expected.

- **Driver supply forecasting**: The surge engine is reactive (measures current supply/demand). Production Uber uses predictive surge based on forecasted demand (ML model trained on historical patterns, events, weather). Not covered here.

- **Payment dispute and refund flow**: The idempotent charge is described, but the refund/dispute flow (rider claims they were overcharged, the fare calculation is audited) is not. This requires a compensating transaction pattern on the Stripe integration.

- **Cross-region data residency**: GDPR requires EU trip data to stay in EU regions. The source files mention it in one sentence but don't explain how queries are routed (API Gateway must check JWT city_id → region mapping and route to the correct regional cluster).

### Senior probes to expect (questions that expose shallow understanding)

1. "You said CAS UPDATE handles race conditions. What is the window between the two CAS UPDATEs (trip table and drivers table) where the system is in an inconsistent state?"
   - *Expected answer*: There is a brief window where the trip is DRIVER_ASSIGNED but the driver's status is still AVAILABLE. During this window, another matching request could try to assign the same driver. The driver CAS UPDATE (`WHERE status='AVAILABLE'`) prevents this — if it fails, the trip assignment is rolled back. The key insight is that the two CAS UPDATEs are not in a DB transaction together (that would cause lock-hold during network round-trips). They are sequential optimistic checks.

2. "TWCS is the right compaction strategy for Cassandra here. What goes wrong if someone accidentally uses STCS (the default)?"
   - *Expected answer*: STCS compacts SSTables when multiple SSTables of similar size exist. At 562,000 writes/second, the system generates many large SSTables very quickly. STCS will trigger compaction storms — periods where multiple large SSTables are compacted simultaneously, consuming all disk I/O and slowing writes. More critically, when the 90-day TTL expires, the tombstone-heavy SSTables don't get cleaned up efficiently under STCS — you get read amplification from tombstone scanning. TWCS groups SSTables by creation time window; old windows are immutable; compaction is targeted and predictable.

3. "Your ETA pods are stateful. How do you roll out a new graph version (new road was built) without downtime?"
   - *Expected answer*: Blue-green graph deployment. Build the new CH graph offline (takes hours). Store it in S3 with a new version number. Start a new set of ETA pods (`blue` pods) that load the new graph. Route a small percentage of city_id traffic (canary) to the blue pods using the load balancer's weighted routing. Monitor accuracy metrics and error rates. If OK, shift 100% of traffic to blue pods. Drain the old pods (wait for in-flight requests to complete). Terminate old pods. Key risk: a CH rebuild for a large city takes hours; if the build fails halfway, the old graph remains in service (no downtime). Graph version metadata (`graph_versions` table) tracks the `is_active` flag — rollback is just flipping `is_active=FALSE` on the new version and re-routing to old pods.

4. "You mentioned Redis Sentinel for failover. Why not Redis Cluster?"
   - *Expected answer*: These serve different purposes. Redis Sentinel provides high availability (automatic failover for a single primary) but doesn't shard data. Redis Cluster shards data across multiple nodes and also provides HA. For the Geo sorted set: the entire available-driver set for a city (96 MB) fits on one node, so sharding (Redis Cluster) adds complexity without benefit. Redis Sentinel is sufficient for HA of a single-node-per-city design. For global multi-city deployments, you'd run separate Redis Sentinel groups per city/region. Redis Cluster would only make sense if a single city had hundreds of millions of drivers — which doesn't happen in practice.

### Common traps in this pattern

**Trap 1: Designing GPS ingest as synchronous DB writes**. Candidates who write GPS directly to Cassandra (synchronously on the hot path) immediately fail the 562,500 writes/second requirement because Cassandra's acknowledgment latency (1–5 ms) × that volume requires thousands of write connections. The correct design is Kafka for durability and Redis for real-time serving.

**Trap 2: Using a distributed lock for driver assignment**. Redis SETNX as a distributed lock sounds clever but has the fencing token problem — if the lock holder crashes while holding the lock, you need a fencing mechanism to prevent the old lock holder from writing to DB after the lock is acquired by a new holder. CAS UPDATE in PostgreSQL is simpler, more reliable, and doesn't require external lock infrastructure.

**Trap 3: Stating that ETA can be computed with Google Maps API**. Correct as a practical shortcut for a startup, but in a system design interview for a company at Uber's scale, this answer means you're delegating a core competitive capability (ETA accuracy drives driver supply and rider retention) to a third party with per-request cost that would be $50M+ per year at Uber's query volume. The examiner wants to see CH or at minimum bidirectional A* with traffic overlay.

**Trap 4: Forgetting the WebSocket server registry problem**. Riders are connected to WebSocket servers, not to the Matching or Location Service directly. When a new GPS update arrives at the Location Service, it needs to know which WebSocket server holds the rider's connection. Without the Redis key `rider:{rider_id}:ws_server`, the Location Service would need to broadcast to all WebSocket servers (expensive) or maintain sticky sessions (fragile). The Redis routing key is the elegant solution.

**Trap 5: Treating surge pricing as simple supply/demand without smoothing**. A naive implementation would compute demand/supply ratio → multiplier every 60 seconds without smoothing. In a 5-minute concert surge, the multiplier would jump from 1.0× to 4.0× then drop to 1.0× once drivers respond, then jump again as the next batch of riders requests rides. The EMA + ratchet rule prevents this oscillation, which both confuses riders and makes the price signal ineffective.

---

*End of Pattern 7 Interview Guide*
