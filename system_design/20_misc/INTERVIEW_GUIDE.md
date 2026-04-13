# Pattern 20: Miscellaneous — Interview Study Guide

Reading Pattern 20: Miscellaneous — 4 problems, 6 shared components

The four problems in this pattern are: **Hotel Booking** (Booking.com / Expedia-style), **Ticket Booking** (BookMyShow-style), **Calendar Service** (Google Calendar-style), and **Code Deployment** (Spinnaker / Argo CD-style). They look unrelated at first glance, but they share a deep architectural DNA: all four are about **reserving a finite resource under concurrent contention**, managing a **lifecycle state machine**, and **coordinating with external systems** (payment, notifications, K8s) without distributed transactions. Mastering the shared patterns once gives you 80% of the answer for any of the four.

---

## STEP 1 — WHY THIS PATTERN EXISTS

These four problems get asked in senior and staff-level interviews because they test whether you can think about **correctness under concurrency** — a topic that trips up a lot of candidates who can build CRUD apps but haven't wrestled with "what happens when two users click Buy at the same millisecond." They also test whether you can design for **eventual consistency** in the right places (notifications, analytics, search indexes) while maintaining **strong consistency** in the exact places that matter (the resource allocation itself). Finally, they test real-world engineering judgment: knowing when a hotel overbooking buffer is a feature, when a Redis TTL is a safety net rather than the source of truth, and why an append-only audit log matters more for a deployment system than a booking system.

**The four problems share exactly these six components:**

1. **PostgreSQL as the authoritative source of truth** for all transactional state
2. **Redis distributed lock with TTL** as a fast-path gate before touching the database
3. **Dual-layer locking** (Redis fast path + PostgreSQL SELECT FOR UPDATE or UNIQUE constraint as durability gate)
4. **Kafka for async side effects** decoupled from the critical write path
5. **Saga / Outbox pattern** for coordinating with external systems (Stripe, Kubernetes, email providers)
6. **Immutable append-only audit log** for compliance and post-incident investigation

---

## STEP 2 — MENTAL MODEL

### The Core Idea

All four problems are fundamentally about **claiming a shared resource before someone else does, then performing a series of steps that must all succeed or all roll back.** The resource is a hotel room, a theater seat, a calendar time slot, or a deployment lock on a service+environment pair. The steps are: validate, reserve, pay (or act), confirm, notify.

### The Real-World Analogy

Think about checking into a hotel. You walk up to the desk. The clerk looks you up in the computer (Redis: is there a room available?), puts a physical key card on the counter for your room (PostgreSQL row lock: your room, no one else gets it), runs your credit card (external payment saga), and hands you the key plus a receipt (booking confirmed, Kafka event published → notification email sent). If your card declines, the key goes back in the drawer (compensating transaction: release inventory). Every step is choreographed so that at no point does the hotel either charge you without giving you a room or give two people the same room.

### Why It Is Hard

Three things make this genuinely difficult:

**First, the TOCTOU race condition.** Between the moment you read "1 room available" and the moment you write "decrement room count," another request can do the same thing. The naive implementation double-books. The solution requires locking, but the wrong lock (holding a DB lock for 10 minutes while someone sits at checkout) kills throughput.

**Second, the external system problem.** Payment, notifications, PDF generation, and Kubernetes deployments are all external. You cannot put a credit card charge inside a database transaction. So you must handle the case where the DB write succeeds but Stripe fails, or Stripe succeeds but your webhook never arrives.

**Third, scale spikes.** A Taylor Swift concert ticket release causes 100x normal traffic in one second. A hotel popular for New Year's Eve gets hammered on the day sales open. A "push to main" after a major all-hands deploys 1,000 services simultaneously. The system must not collapse under these spikes, and it must not allow the spike to cause inconsistency.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask exactly these questions at the start. Each one changes the architecture.

**For Hotel Booking:**
- Is this a pure online booking platform (like Booking.com), or do we also need to support real-time Property Management System (PMS) integration with the hotel's own reservation system? (Yes: adds PMS sync service and conflict reconciliation. No: we own all inventory.)
- Do we need to support overbooking buffers, or is a strict no-overbooking policy required? (Overbooking: `total_rooms` can exceed physical count; needs walk compensation flow. No overbooking: simpler CHECK constraint.)
- Is dynamic pricing — prices change by date, demand, season — a requirement at launch? (Yes: `room_inventory` stores price per room per date, not a flat rate. No: flat rate per room type.)
- Do we need to support multi-room bookings (group bookings), or single room per transaction? (Multi-room: the availability query must check `MIN(available_rooms) >= num_rooms`. Single: `>= 1` suffices.)
- What is the expected search volume vs booking volume? A 200:1 read-to-write ratio changes how aggressively you cache. (This tells you whether the search path needs Elasticsearch + Redis, or read replicas alone are sufficient.)

**Red flags in the answers:** If the interviewer says "just make it simple, single room, no dynamic pricing, no overbooking" — they are setting you up to demonstrate the core concurrency mechanism. Do not simplify your architecture; simplify your scope but show the locking pattern anyway.

**For Ticket Booking:**
- Flash sale support — will we have events where 100,000 users attempt to buy simultaneously? (Yes: needs virtual waiting room + Redis DECR inventory counter. No: standard hold flow.)
- Is this a seat-specific system (user picks Row A, Seat 12) or best-available (system assigns seats)? (Seat-specific: one Redis lock per seat_id. Best-available: one Redis counter for the category.)
- Do we need to generate PDF tickets with QR codes? (Yes: async PDF Worker pool + S3 + signed URLs. No: simpler confirmation flow.)

**For Calendar Service:**
- Is this Google-scale (2 billion users) or enterprise-scale (50,000 employees)? (Google scale: Cassandra for events, sharded. Enterprise: PostgreSQL alone likely sufficient.)
- Do we need to support recurring events with RFC 5545 RRULE? (Yes: expand-on-read logic, exception rows, EXDATE. No: simpler flat event model.)
- Is CalDAV/iCal interoperability required? (Yes: protocol translation layer at the API Gateway. No: REST-only interface.)

**For Code Deployment:**
- Which deployment strategies must be supported: rolling update only, or also blue-green and canary? (Canary: Traffic Controller abstraction over Istio/ALB. Rolling only: simpler K8s rollout.)
- Is automated rollback on health check failure a requirement? (Yes: Health Check Engine + rollback saga. No: manual rollback only.)
- Do we support multi-region deployments with promotion gates between regions? (Yes: pipeline stages include WAIT_FOR_REGION, MANUAL_APPROVAL. No: single-region only.)

---

### 3b. Functional Requirements

**Hotel Booking core:**
- Users search hotels by location, date range, guest count, and filters (amenities, star rating, price). Search results show only hotels that have at least one room available for every night in the requested range.
- Users can view hotel detail pages with room types, rate plans, per-night pricing, photos, and reviews.
- Users can book a room, providing payment details. System creates a reservation atomically with inventory allocation.
- Users receive email/SMS confirmation. Users can cancel per the rate plan's cancellation policy, receiving appropriate refunds.
- Hotel managers can manage room inventory, prices, and blocked dates.

**Ticket Booking core:**
- Users browse events and view seat maps showing real-time availability.
- Users can hold seats for up to 10 minutes during checkout. Held seats are exclusively reserved.
- Users complete payment; on success, seats are permanently confirmed and a PDF ticket is issued. On failure, seats are released.
- Waitlist management: users join when sold out; notified when seats release.
- Flash sale support: controlled admission to booking flow during high-demand launches.

**Calendar Service core:**
- Users create, edit, delete calendar events with full timezone support.
- Recurring events following RFC 5545 RRULE (stored once, expanded on read). Exceptions to specific occurrences are supported.
- Users invite others to events; invitees RSVP. Calendar sharing with granular permissions (free/busy only, view, edit).
- Per-event reminders delivered via email, push, or SMS at user-configured offsets.
- Multi-device sync: changes on one device appear on all others within 3 seconds.

**Code Deployment core:**
- Engineers or CI bots trigger deployments referencing a specific artifact version.
- System executes the selected rollout strategy (rolling, blue-green, canary, recreate) against the target Kubernetes environment.
- Health checks run post-deploy; automatic rollback if checks fail.
- Exactly one deployment at a time per service+environment (deployment lock).
- Immutable audit log of every action.

---

### 3c. Non-Functional Requirements and Their Trade-offs

**Consistency vs. Availability — the central NFR tension:**

For all four systems, the resource reservation must be **strongly consistent** — no double booking of a room or seat, no two deployments running simultaneously. This means you accept slightly higher latency (lock acquisition, SELECT FOR UPDATE) on the write path in exchange for correctness. The search/browse/read path is **eventually consistent** — a user's availability search may show a room that was booked 30 seconds ago (Redis cache TTL), and that is acceptable.

| NFR | Hotel Booking | Ticket Booking | Calendar Service | Code Deployment |
|-----|---------------|----------------|-----------------|-----------------|
| **Availability** | 99.99% (travel is revenue-critical) | 99.99% (flash sale miss = revenue loss) | 99.99% (productivity tool) | 99.9% (deployment unavailability doesn't affect live services) |
| **Consistency** | Strong for inventory writes; eventual for search cache | Strong for seat locks; eventual for seat map reads | Strong for own events; eventual for cross-user invite status | Strong for deployment locks; eventual for status dashboard |
| **Latency p99** | Search < 300 ms; booking < 1 s | Seat map < 200 ms; hold < 300 ms | Calendar view < 200 ms; event write < 300 ms | Deploy trigger < 200 ms; status read < 100 ms |
| **Throughput spike** | 10x holiday multiplier | 100x flash sale multiplier | 3x Monday morning spike | 1,000 simultaneous deploys peak |

**Key derived trade-offs to articulate:**

✅ **Redis caching for reads** means availability data is stale by up to 60 seconds. This is acceptable: a user seeing a room as available that was just booked by another user gets a graceful "room unavailable" error at booking time. The consistency guarantee is on the write, not the read.

❌ **Redis as the only lock mechanism** would be wrong. Redis can lose data on restart or failover. The PostgreSQL layer (SELECT FOR UPDATE or UNIQUE constraint) is the non-negotiable correctness guarantee.

✅ **Eventual delivery for notifications** via Kafka is correct. A confirmation email arriving 5 seconds after booking confirmation does not affect correctness.

❌ **Synchronous payment in the critical booking path** is wrong for hotel/ticket booking. Payment takes 200 ms–3 s and can time out. Use the outbox pattern: write booking record + outbox message atomically, process payment asynchronously.

---

### 3d. Capacity Estimation

Always present this as a formula first, then anchor numbers, then the architecture implication.

**Hotel Booking:**
- 100M users, 10% DAU = 10M DAU. Each performs 5 searches → 50M searches/day. 10% of DAU books → 1M bookings/day.
- **Formula:** Booking RPS = Bookings_per_day / 86,400 = 1M / 86,400 ≈ 12 RPS normal. Peak (10x holiday) = 120 RPS.
- **Search RPS:** 50M / 86,400 ≈ 579 RPS normal, ~5,800 RPS peak.
- **Architecture implication:** Search is 50x the write load. Separate search path entirely (Elasticsearch for attribute filtering + Redis for availability cache). PostgreSQL only sees write traffic + cache misses. At 1M bookings/day, the `room_inventory` table accumulates ~182 GB of inventory records/year for 1M hotels × 10 room types × 365 days. Monthly partitioning is mandatory.
- **Storage:** 1M hotels × 10 room types × 365 days × 50 bytes = 182 GB inventory. Reservations: 1M/day × 365 × 3 years × 2 KB ≈ 2.2 TB. Hotel photos: 1M × 6 MB = 6 TB (CDN/S3, not DB). Total DB: ~2.5 TB.
- **Time to state these:** Under 3 minutes. Say: "Let me anchor on scale. 10M DAU, 5 searches each, 10% book. That's ~580 search RPS normal, ~5,800 peak. Booking writes are only ~12 RPS normal. This is a 50:1 read-heavy system, which means I'll separate the search and booking paths. Storage: about 2.5 TB for transactional data, manageable with partitioning."

**Ticket Booking:**
- 5M DAU, 200K bookings/day normal. Peak flash sale: 4M attempts in 1 hour = 1,111 booking RPS.
- **Hold RPS at flash sale:** 4M × 3 retries / 3,600 ≈ 3,333 seat lock requests per second.
- **Architecture implication:** 3,333 Redis SETNX operations/second is fine for Redis (handles ~100K ops/second per node). But if those lock requests fall through to PostgreSQL SELECT FOR UPDATE, you have 3,333 concurrent DB transactions — a disaster. The Redis gate is mandatory; it must reject failed attempts without touching the DB.
- **Storage:** ~180 GB DB, ~22 TB PDF tickets in S3. DB is small; the challenge is throughput, not storage.

**Calendar Service:**
- 2B users, 500M DAU (25% — default mobile app), 3 calendar views/day/user = 1.5B views/day = 17,361 view RPS normal, 52,000 RPS peak (Monday morning 3x spike).
- **Architecture implication:** 52,000 RPS of calendar view queries cannot hit PostgreSQL. Cassandra's partition-key-based range scans (user_id, calendar_id, start_time) serve each user's events from a single partition at < 10 ms. PostgreSQL is used only for ACL checks and metadata updates (< 5% of traffic).
- **Storage:** 2B users × 200 events × 500 bytes = 200 TB. This is Google-scale. PostgreSQL would require extreme sharding; Cassandra provides linear horizontal scale natively.
- **Reminder delivery:** 106M reminders/day ÷ 86,400 = 1,227/second average; bursts to ~10,000/second at the top of each hour. Redis sorted set sharded by user_id handles this.

**Code Deployment:**
- 10,000 services × 4 deploys/day = 40,000 deployments/day ≈ 0.46 write RPS (extremely low!).
- **Architecture implication:** This system has almost no data volume challenge. The interesting problems are all correctness and orchestration: mutual exclusion, state machine integrity, multi-stage health checks, and audit log immutability. A single PostgreSQL instance with < 200 write RPS is more than sufficient. Redis is used for lock speed and status caching, not for scale.

---

### 3e. High-Level Design

**Components and whiteboard order (draw these in sequence):**

**Hotel Booking — 6 core components:**

1. **CDN + API Gateway** — Static assets (hotel photos) and search result caching at the edge. API Gateway handles JWT auth, rate limiting (100 req/min for users, 5K for OTA partners), and routing.
2. **Search Service** — Queries Elasticsearch (geo-distance + amenity facets) to get candidate hotel_ids, then calls Inventory Service to cross-check availability. Returns ranked results.
3. **Inventory Service** — The heart. Tracks `room_inventory(hotel_id, room_type_id, date, available_rooms GENERATED ALWAYS)`. Reads from Redis cache (60s TTL); falls back to PostgreSQL. All writes are atomic.
4. **Booking Service** — Orchestrates the reservation saga: locks inventory rows in date-order → inserts reservation → writes payment outbox event → confirms on payment webhook.
5. **Payment Service** — Thin Stripe wrapper. Creates PaymentIntents idempotently. Handles webhooks. No business logic.
6. **Notification Service** — Consumes Kafka events → sends email/SMS. Kafka decouples this from the critical booking path.

**Data flow — search to book:**
User searches → Search Service → Elasticsearch (hotel attributes) → Inventory Service (availability from Redis/PG) → results returned. User books → Booking Service → SELECT FOR UPDATE on inventory rows (date-ordered) → INSERT reservation + outbox → Payment Service → Stripe webhook → CONFIRMED → Kafka → Notification email.

**Ticket Booking — 5 core components:**

1. **API Gateway** — same role; critical for flash sale rate limiting.
2. **Discovery Service** — event search via Elasticsearch + PostgreSQL read replicas. No writes.
3. **Inventory / Seat Service** — Redis SETNX per seat_id (10 min TTL) → PostgreSQL SELECT FOR UPDATE on seat row → status = HELD. This is the concurrency core.
4. **Booking Service** — validates hold is active → writes booking + outbox → Payment Service → on success, seats → CONFIRMED; on failure, seats → AVAILABLE.
5. **PDF Worker** — async pool; consumes Kafka booking_confirmed events; generates PDF + QR; uploads to S3; updates booking with signed URL.

**Calendar Service — 6 core components:**

1. **API Gateway** — CalDAV/iCal protocol translation for native calendar clients.
2. **Calendar & Event Service** — CRUD + RRULE parsing + conflict detection (Redis free/busy cache).
3. **Invite / RSVP Service** — attendee management; RSVP state in Cassandra.
4. **Sharing & Permissions Service** — ACL enforcement for every cross-user data access; stored in PostgreSQL.
5. **Reminder Service** — Redis sorted set (score = fire_at timestamp); polled every 5 seconds; delivers via Kafka → email/push/SMS.
6. **Sync Service** — TIMEUUID-based change log in Cassandra; WebSocket push to devices; 30-day TTL changelog.

**Code Deployment — 5 core components:**

1. **Deployment Orchestrator** — Central coordinator and state machine owner. Acquires lock, selects strategy, manages PENDING → IN_PROGRESS → HEALTH_CHECK → SUCCEEDED/FAILED/ROLLED_BACK.
2. **Deployment Engine** — Strategy-specific logic (Canary, Blue-Green, Rolling, Recreate). Pluggable.
3. **Health Check Engine** — HTTP probes + metric checks (Datadog/Prometheus) + custom scripts. All three must pass before any traffic promotion.
4. **Traffic Controller** — Abstraction over Istio VirtualService, AWS ALB weights, Nginx Ingress. `set_weights(stable_pct, canary_pct)` interface.
5. **Deployment Lock Manager** — Redis SETNX + PostgreSQL UNIQUE constraint dual-layer.

---

### 3f. Deep Dive Areas

**Deep Dive 1: Concurrency Control (the most-probed area across all four problems)**

The problem: when N users simultaneously try to reserve the last unit of a resource, exactly one must succeed and N-1 must fail cleanly.

**The dual-layer pattern — explain this unprompted:**

```
Layer 1: Redis SETNX with TTL
  - SETNX seat:{seat_id}:lock {user_request_id} EX 600
  - If returns 0 (key exists): resource locked. Return 409 immediately. Zero DB calls.
  - If returns 1: proceed to Layer 2.

Layer 2: PostgreSQL SELECT FOR UPDATE
  - SELECT * FROM seats WHERE seat_id = ANY($1) ORDER BY seat_id FOR UPDATE
  - ORDER BY seat_id: prevents deadlock when two users book overlapping multi-seat selections
  - Verify status = 'AVAILABLE' in DB (Redis is fast path; DB is authoritative)
  - UPDATE seats SET status = 'HELD' WHERE seat_id = ANY($1)
  - Transaction commits. DB lock released. Total DB lock hold time: ~50ms.
```

✅ **Redis handles the thundering herd**: at 3,333 concurrent requests, Redis rejects ~3,332 of them without any DB contention.

✅ **PostgreSQL provides correctness**: if Redis has a split-brain failure and two requests both pass the SETNX check, the SELECT FOR UPDATE serializes them at the database level — the second one sees status = 'HELD' and fails cleanly.

❌ **Without the Redis layer**: 3,333 concurrent SELECT FOR UPDATE transactions hit the DB simultaneously. Connection pool exhaustion. Cascading failure.

❌ **Without the DB layer**: a Redis restart during failover drops all lock state. In-flight requests all re-acquire the lock and double-book.

**The sorted lock acquisition rule — memorize this, it comes up every interview:**

When booking multiple resources simultaneously (multi-seat ticket hold, multi-night hotel room), always acquire locks in a consistent order. If two transactions acquire locks in opposite orders, they deadlock. Fix: `ORDER BY seat_id ASC` or `ORDER BY date ASC` in the SELECT FOR UPDATE query. Both transactions now always try to acquire Lock A before Lock B, so they serialize rather than deadlock.

**Deep Dive 2: The Saga / Outbox Pattern (second most-probed area)**

The problem: you need to charge a credit card AND update your booking database, but you cannot put them in the same ACID transaction. Stripe is external.

**The correct solution — explain this fully and unprompted:**

The **outbox pattern** makes two steps atomic using the database you already have:

```
Single PostgreSQL TRANSACTION:
  INSERT INTO bookings (booking_id, status='PENDING', ...) -- booking record
  INSERT INTO outbox (event_type='initiate_payment', payload=...) -- outbox record

Both succeed or both roll back. No partial state.

Outbox relay (background thread or separate process):
  SELECT * FROM outbox WHERE status='pending' ORDER BY created_at
  For each: call Stripe with idempotency_key = booking_id
  If Stripe succeeds: mark outbox row as 'processed'; Booking Service confirms booking
  If Stripe fails: retry with exponential backoff (same idempotency_key = no double charge)
  If permanently failed: compensating transaction releases inventory
```

✅ **Idempotency key** means retrying the Stripe call never double-charges. Stripe returns the same PaymentIntent response for the same key.

✅ **Outbox row persisted in the same transaction** means even if the process crashes after the transaction commits but before calling Stripe, the outbox row survives. Next poll picks it up.

❌ **"Fire and forget" Kafka publish instead of outbox**: if the process crashes between the DB commit and the Kafka publish, you have a committed booking with no payment ever initiated. The booking is stuck in PENDING forever.

**Deep Dive 3: Flash Sale / Thundering Herd (ticket booking; highly specific)**

The problem: 100,000 users simultaneously hit "Buy Tickets" the moment sales open.

**The virtual waiting room + Redis DECR approach:**

```
Step 1: 15 minutes before sale, redirect all traffic to static waiting room page (CDN, zero origin load)
Step 2: At sale time, issue each user a randomized queue position token via WebSocket
Step 3: Admit 500 users/second in order (rate-controlled admission)
Step 4: First gate inside booking flow = Redis DECRBY:
  new_count = redis.decrby(f"show:{show_id}:available", seat_count)
  if new_count < 0:
      redis.incrby(f"show:{show_id}:available", seat_count)  # compensate
      return "sold out"  # no DB touched
  # proceed to hold flow
```

✅ **Redis DECRBY is atomic**: at 100,000 concurrent DECRBY operations, Redis serializes them. The counter reaches zero exactly when the last available seat is allocated. No oversell.

✅ **Waiting room page served from CDN**: the origin servers see zero traffic during the pre-sale queue period. The traffic spike happens in a controlled ramp (500/second), not a cliff face.

❌ **Without the waiting room**: origin servers receive 100,000 requests in the first second. Even if each request is handled correctly, connection pools, load balancers, and rate limiters collapse. The waiting room is not an optimization; it is a safety valve.

---

### 3g. Failure Scenarios

**Scenario 1: Payment webhook never arrives (hotel + ticket booking)**

The booking is PENDING; inventory is allocated; payment may or may not have been charged. The correct recovery:

A scheduled reconciliation job runs every 5-10 minutes. For all bookings in PENDING state for more than 15 minutes: call Stripe's `PaymentIntent.retrieve(payment_id)`. If status is `succeeded`: run `confirm_booking()`. If status is `failed` or `canceled`: run compensating transaction releasing inventory. If status is still `processing`: extend the timeout and check again. This reconciliation job is the safety net for missed webhooks. Senior framing: "I never rely solely on webhooks for correctness. Webhooks are an optimization — they make most bookings confirm in < 1 second. The polling job is the correctness guarantee for the long tail."

**Scenario 2: Deployment Orchestrator pod crashes while holding a deployment lock**

The Redis lock has a 1-hour TTL. The PostgreSQL `deployment_locks.expires_at` is set to 1 hour from acquisition. A stale lock recovery job runs every 5 minutes: `SELECT * FROM deployment_locks WHERE expires_at < now()`. For each: mark deployment FAILED, attempt rollback if deployment was partially through (revert to previous artifact), release both the Redis key and the DB record, notify the engineer, write audit log. Mean time to recovery after pod crash: ≤ 5 minutes.

Senior framing: "The system must be correct even when the orchestrator crashes mid-deployment. K8s state may be partially updated. The health check stage is the guard: since the deployment never reached HEALTH_CHECK → SUCCEEDED, the previous stable version's traffic routing is still in place or can be restored. The rollback just needs to delete any new pods and reset traffic weights."

**Scenario 3: Redis cluster failure (all four systems)**

For booking systems: fall back to pure PostgreSQL locking. The fallback path uses `SELECT FOR UPDATE` without the Redis fast path. Latency increases ~50 ms; throughput decreases significantly but remains correct. A circuit breaker in the Inventory/Seat Service detects Redis unavailability and routes to the fallback automatically.

For calendar reminders: the Redis sorted set loses unprocessed future reminders. Recovery: a job reads `event_reminders` in Cassandra for `fire_at BETWEEN now() AND now() + 10 minutes AND delivered = false` and repopulates the Redis sorted set. This job runs on Redis restart or after a detected outage.

**Scenario 4: Database primary failure (PostgreSQL failover)**

All systems use a primary + read replicas setup. On primary failure, one replica is promoted (usually within 30-60 seconds with synchronous replication for the replica). In-flight write transactions are lost; clients must retry with idempotency keys. Bookings in PENDING state before the failover are reconciled by the payment polling job. The Booking/Seat/Deployment services should use connection retry logic with exponential backoff during the failover window.

---

## STEP 4 — COMMON COMPONENTS

Every component listed in common_patterns.md — why each is used, how it is configured, and what breaks without it.

### PostgreSQL as Authoritative Source of Truth

**Why used:** ACID transactions with row-level locking (`SELECT FOR UPDATE`) make it the only store that can guarantee correctness for concurrent resource reservation. No other mainstream database offers both full ACID transactions and efficient row-level locking with reasonable operational overhead at this scale.

**Key config:**
- **Isolation level:** `READ COMMITTED` is sufficient for most booking operations (you re-read rows after acquiring the lock). `SERIALIZABLE` adds overhead without benefit since the lock already serializes the critical section.
- **Partitioning:** `room_inventory` and `event_reminders` are partitioned by date range (monthly partitions). Old partitions are archived. Query planner prunes irrelevant partitions automatically.
- **Generated columns:** `available_rooms GENERATED ALWAYS AS (total_rooms - booked_rooms - blocked_rooms) STORED` in hotel_booking. The database computes this at write time; it can never drift from the component values. Similarly, `num_nights GENERATED ALWAYS AS (check_out_date - check_in_date) STORED` on reservations.
- **Partial indexes:** `CREATE INDEX idx_avail ON room_inventory (hotel_id, date) WHERE available_rooms > 0` — the index only covers rows with availability. At 70% occupancy, the partial index is 30% smaller and covers all interesting queries.
- **Read replicas:** 3 replicas for the search/browse read path. Primary handles writes only. This offloads ~70% of DB load.
- **Connection pooling:** PgBouncer in transaction mode with a pool of 5,000-10,000 connections. Prevents the database from being overwhelmed during traffic spikes.

**What breaks without it:**
- Redis alone cannot guarantee correctness on restart or network partition.
- NoSQL stores (DynamoDB, Cassandra) lack multi-row ACID transactions, making the "allocate inventory across multiple dates in one atomic step" impossible without complex application-level coordination.

### Redis Distributed Lock with TTL

**Why used:** Redis `SETNX` (SET if Not eXists) with an `EX` (expiry) option is an atomic operation that sets a key only if it does not already exist and automatically deletes it after the TTL expires. This gives you mutual exclusion with crash-safety in a single operation at < 1 ms latency.

**Key config:**
- **TTL calibration:** Set the TTL to match the maximum legitimate hold duration. Ticket seat holds: 10 minutes (EX 600). Hotel booking checkout: 15 minutes (EX 900). Deployment lock: 1 hour (EX 3600). A TTL too short causes false expirations; too long means a crashed client blocks the resource indefinitely.
- **Lock value uniqueness:** Set the lock value to a unique identifier (a UUID or `{user_id}:{request_id}`), not just "locked". When releasing, verify `GET lock_key == expected_value` before `DEL`. This prevents a slow client from releasing another client's lock after the TTL expires and a new client acquires it.
- **High availability — Redlock:** For flash sale scenarios, use Redlock across 3 independent Redis nodes (not cluster replicas). Acquire the lock on a majority (2 of 3). This tolerates one Redis node failure without false lock grants.
- **Usage per system:**
  - Ticket booking: `SETNX seat:{seat_id}:lock {hold_id} EX 600` — one lock per seat
  - Hotel booking: short-lived (EX 30) per availability check + booking session
  - Calendar: `SETNX reminder:proc:{reminder_id} {worker_id} EX 120` — prevents duplicate reminder delivery
  - Code deployment: `SETNX deploy:lock:{service_id}:{env_id} {deployment_id} EX 3600`

**What breaks without it:**
- Every booking attempt hits PostgreSQL directly with SELECT FOR UPDATE. Under flash sale load (3,333 concurrent requests), connection pool exhaustion crashes the database tier. The system fails under exactly the load conditions where it must be most resilient.

### Dual-Layer Locking (Redis Fast Path + PostgreSQL Authoritative)

**Why used:** Neither Redis alone nor PostgreSQL alone is sufficient. Redis is fast but not durable. PostgreSQL is durable but slow under high concurrency. The two layers together provide both.

**Key config:** The two layers are never used in isolation — they work in concert:
1. Redis SETNX: fast rejection. Most concurrent requests stop here.
2. PostgreSQL `SELECT ... FOR UPDATE` (or `UNIQUE` constraint for deployment locks): authoritative guard. The few requests that pass Redis are serialized here.

Critically, the PostgreSQL lock is held for milliseconds only (duration of the check + update transaction). The Redis key persists for the entire hold duration (10 minutes, 1 hour). This split means the DB is never holding a lock for long periods.

**What breaks without it:** Operating with only Redis → double-booking on Redis restart. Operating with only PostgreSQL → DB lock contention kills throughput. You need both.

### Kafka for Async Side Effects

**Why used:** The booking confirmation, seat confirmation, deployment notification, and reminder fanout are all side effects of a core write. They must not fail the core write if they are slow or temporarily unavailable. Kafka decouples these consumers from the producer.

**Key config:**
- **Topics per system:** `booking-events` (hotel + ticket), `calendar-events`, `deployment-events`, `reminders.due`
- **Consumer groups:** Each downstream consumer (notification, PDF worker, analytics, waitlist, audit enricher) is its own consumer group with independent offset tracking. One slow consumer does not block others.
- **Idempotent consumers:** Every Kafka consumer must be idempotent. Kafka guarantees at-least-once delivery; consumers may receive the same message twice after a rebalance. For the notification service: check "has this booking_id already sent a confirmation email?" before sending.
- **Retention:** Set topic retention to at least 7 days. This gives recovery time if a consumer group falls behind.

**What breaks without it:**
- Synchronous notification: if the email service is slow, the booking API call takes 3+ seconds. Users abandon at checkout.
- Synchronous PDF generation: PDF generation can take 2-5 seconds. Blocking the booking response on this is unacceptable.
- Cascading failures: if any downstream service is unavailable, booking fails. With Kafka, the booking write always succeeds; downstream failures are isolated.

### Saga / Outbox Pattern for Distributed Transactions

**Why used:** You cannot put an external API call (Stripe charge, Kubernetes rollout) inside a database transaction. The Saga pattern breaks the operation into sequential steps with compensating transactions for rollback. The Outbox pattern makes the "publish the next step" action atomic with the current step's database write.

**Key config:**
- The outbox table lives in the same database as the booking/deployment table. The outbox row is inserted in the same transaction as the booking row. The outbox relay polls the outbox and delivers messages. Only after successful delivery is the outbox row marked `processed`.
- **Idempotency key per step:** Every external call carries an idempotency key. For Stripe: `idempotency_key = f"booking-{booking_id}"`. Retrying the call with the same key returns the same result without a duplicate charge.
- **Compensating transactions:** For each forward step, define the compensating step. Booking → release inventory. Seat hold → release seat. Deployment → rollback to previous artifact.

**What breaks without it:**
- "Fire and forget" Kafka publish: if the process crashes after the DB commit but before the Kafka publish, the booking is confirmed in the DB but no payment is ever initiated. Stuck in PENDING forever, user never charged but room/seat allocated.
- XA distributed transactions: payment gateways do not support XA. You cannot make Stripe participate in a two-phase commit.

### Immutable Append-Only Audit Log

**Why used:** All four systems make consequential state changes: money is charged, resources are reserved, production code is deployed. Disputes, incidents, and compliance audits require an immutable record of what happened, when, who initiated it, and what the before/after state was.

**Key config:**
- PostgreSQL table with `GRANT INSERT` only — no UPDATE, no DELETE at the database permission level. The application role used by the service does not have UPDATE or DELETE privileges on this table.
- Every state transition writes an audit row: `deployment_id`, `actor_id`, `actor_type` (USER / CI_BOT / SYSTEM), `action`, `before_state JSONB`, `after_state JSONB`, `ip_address`, `created_at`.
- S3 archival for long-term retention (2 years for deployments, 7 years for financial bookings).
- For hotel/ticket booking: dispute resolution queries the audit log to answer "was the room actually available when this booking was made?" or "was the seat ever in CONFIRMED status?"

**What breaks without it:**
- No recourse for disputes. When a user claims they were charged but never received a ticket, you cannot investigate.
- No post-incident analysis for deployment outages. Without knowing exactly what was deployed, when, by whom, and what traffic weights were set at each step, you cannot perform an effective post-mortem.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Hotel Booking

**The unique thing:** Hotel Booking is the only system where availability must be checked across a range of dates in a single query, and where the inventory unit is not a specific physical seat but a count of interchangeable rooms.

The differentiator is the **date-range MIN aggregate availability query**:
```sql
SELECT MIN(available_rooms) >= $rooms_needed
FROM room_inventory
WHERE hotel_id = $hotel_id AND date BETWEEN $check_in AND $check_out - INTERVAL '1 day'
HAVING COUNT(*) = ($check_out - $check_in)
```
`MIN(available_rooms)` ensures every night has capacity. `HAVING COUNT(*) = N_nights` guards against missing inventory rows (unconfigured dates appear as "not available" correctly).

**Unique decisions that differ from ticket booking:**
- **Count-based inventory** vs. seat-specific: a room is an interchangeable unit; you allocate a count, not a specific room_id. This enables `booked_rooms + 1` updates without per-room row locks.
- **`available_rooms` as a GENERATED ALWAYS column**: computed from `total_rooms - booked_rooms - blocked_rooms` at write time. Eliminates update anomalies. Never updated directly.
- **Overbooking buffer**: `total_rooms = physical_room_count × 1.05`. Hotels intentionally oversell by ~5% to compensate for expected no-shows. A walk compensation procedure handles the rare cases where this backfires.
- **Monthly partitioning** of `room_inventory` is required (365 rows × 1M hotels × 10 room types = 3.65B rows). Ticket booking's `seats` table is orders of magnitude smaller.

**Two-sentence differentiator:** Hotel Booking is unique because availability is a count across a date range, not a specific seat, requiring a date-range MIN aggregate SQL query with a HAVING guard against missing inventory rows. It is also the only system in this group that has a legitimate overbooking strategy (setting `total_rooms` above physical capacity) with a defined compensation workflow for walk events.

### Ticket Booking

**The unique thing:** Ticket Booking is the only system where individual seats are locked as named, specific units (Row A, Seat 12), and where demand spikes are extreme (100x in 1 second) due to flash sales.

The differentiator is the **per-seat Redis SETNX lock + virtual waiting room**:
```
SETNX seat:{seat_id}:lock {hold_id} EX 600
```
This is one Redis key per physical seat, with a 10-minute TTL. During a flash sale, the virtual waiting room queues users and trickle-admits them at a controlled rate, using `DECRBY show:{show_id}:available` as a first gate that prevents any DB operations for sold-out requests.

**Unique decisions that differ from hotel booking:**
- **Per-seat locks** vs. count locks: you can lock specific Seat A12 in Row A. Hotel booking locks a count of interchangeable rooms.
- **Sorted multi-seat lock acquisition** is critical: booking seats [A12, A13, A14] always acquires locks in seat_id order (alphabetically/numerically sorted). Prevents deadlocks between two users selecting overlapping seat ranges.
- **Hold expiry reaper**: runs every 30 seconds, releases expired holds back to AVAILABLE, wakes the waitlist. Hotel booking has a simpler version; ticket booking's reaper is more latency-sensitive because seats free up for a potentially still-live show.
- **PDF ticket generation** with QR code (signed booking token): hotel booking sends a confirmation email. Ticket booking must generate a scannable document with a cryptographic proof of purchase.

**Two-sentence differentiator:** Ticket Booking is unique because inventory is seat-specific (not interchangeable), requiring one Redis SETNX lock per seat with sorted multi-seat acquisition to prevent deadlocks, and because flash sales require a virtual waiting room + Redis DECRBY counter to prevent the database from being overwhelmed before any hold logic runs. It is the highest-concurrency problem in the group, demanding Redlock across 3 nodes for the flash sale path.

### Calendar Service

**The unique thing:** Calendar Service is the only system in this group where the primary write path does not involve external payment or a deployment orchestrator — the hard problems are recurring event storage, multi-device sync consistency, and reminder scheduling at Google scale.

The differentiator is the **RFC 5545 RRULE expand-on-read approach + TIMEUUID sync changelog**:

A recurring event like "every weekday at 9am EST for 1 year" is stored as exactly one row with `rrule = 'RRULE:FREQ=DAILY;BYDAY=MO,TU,WE,TH,FR;UNTIL=20270101T000000Z'`. When a user queries their calendar for June, the service uses `dateutil.rrule` to expand that one row into 22 instances in memory. No pre-materialized rows.

**Unique decisions that differ from all other three:**
- **Cassandra instead of PostgreSQL for event storage**: 200 TB of event data across 2B users maps perfectly to Cassandra's partition model — each user's events on their own partition, queried by time range. PostgreSQL would require extreme sharding at this scale.
- **TIMEUUID sync changelog** (not a sequence number): time-ordered UUIDs provide monotonic ordering without a centralized sequence generator. Last write wins. 30-day TTL eliminates old entries automatically.
- **CalDAV/iCal protocol translation at the API Gateway**: native integration with Apple Calendar, Outlook, and Thunderbird without requiring users to use the web app.
- **Redis sorted set for reminder scheduling**: `score = fire_at_unix_timestamp` enables `ZRANGEBYSCORE reminders:pending -inf NOW()` — an O(log N) scan for due reminders, processed every 5 seconds.
- **No payment saga**: calendar events have no external financial transaction, simplifying the write path significantly.

**Two-sentence differentiator:** Calendar Service is unique because the core challenge is not concurrency control but recurring event representation — storing one master RRULE row and expanding instances on read eliminates pre-materialized storage while supporting complex RFC 5545 patterns. At Google scale (2B users, 200 TB), it requires Cassandra for event storage (not PostgreSQL), TIMEUUID-based sync changelog for last-write-wins multi-device sync, and a Redis sorted-set-based reminder dispatcher polling every 5 seconds for O(log N) due-reminder queries.

### Code Deployment

**The unique thing:** Code Deployment is the only system in this group where the "resource" being locked is an environment slot (service + environment pair), the "booking" is a multi-stage orchestrated workflow against external infrastructure (Kubernetes), and correctness failure does not mean a double charge — it means two conflicting versions of production code running simultaneously.

The differentiator is the **deployment state machine + mandatory HEALTH_CHECK state + manifest JSONB snapshot for deterministic rollback**:

The deployment lifecycle enforces `PENDING → IN_PROGRESS → HEALTH_CHECK → SUCCEEDED / FAILED / ROLLED_BACK`. There is no valid transition from IN_PROGRESS directly to SUCCEEDED. HEALTH_CHECK is mandatory. This is enforced at the application level (state machine guard) and audited in the audit log.

**Unique decisions that differ from all other three:**
- **The resource is a deployment slot, not a bookable commodity**: there is no seat count or date range. The lock is binary: one deployment active per (service, environment) at a time.
- **Multi-stage strategy execution** (canary ramp 10% → 25% → 50% → 100%): no equivalent in booking systems. The "saga" steps are traffic weight changes with health check gates between each.
- **Traffic Controller abstraction** over Istio, ALB, and Nginx: no equivalent in any booking system. The system must integrate with whatever service mesh the deployment target uses.
- **Manifest JSONB snapshot**: every successful deployment stores the full Kubernetes manifest as JSON. Rollback is deterministic — apply the previous manifest exactly, not an approximate re-creation.
- **No user-facing latency requirements**: engineers and CI bots can tolerate seconds-to-minutes for deployment status. Compare to booking systems where users cannot wait more than 1 second. This significantly relaxes latency requirements.
- **mTLS for CI bot authentication**: service-to-service authentication instead of user JWT. No equivalent in consumer-facing booking systems.

**Two-sentence differentiator:** Code Deployment is unique because the critical resource is an environment slot (not a seat or room), requiring a dual-layer lock (Redis SETNX + PostgreSQL UNIQUE(service_id, env_id)) with a 1-hour TTL and a crash-recovery job, and because the "saga" is a multi-stage canary ramp with automated health check gates and instant traffic rollback at each stage. It is the only problem in this group where the state machine enforces a mandatory HEALTH_CHECK intermediate state, and where rollback is accomplished by replaying a stored K8s manifest snapshot rather than reversing a database transaction.

---

## STEP 6 — Q&A BANK

### Tier 1: Surface Questions (5–7, 2–4 sentences each)

**Q1: "Walk me through how you prevent double-booking in your hotel booking system."**

The system uses dual-layer locking. When a booking request arrives, the Booking Service calls the Inventory Service, which first checks Redis for a short-lived lock on the (hotel, room_type, date_range) combination. If that passes, it executes a `SELECT ... FOR UPDATE` on all `room_inventory` rows for the stay dates (ordered by date ASC to prevent deadlocks), verifies `available_rooms >= num_rooms` for every night, then atomically runs `UPDATE room_inventory SET booked_rooms = booked_rooms + num_rooms`. These two steps together guarantee that no two transactions can both see "room available" and both successfully book the last room. **The key phrase is "dual-layer: Redis for speed, PostgreSQL SELECT FOR UPDATE for correctness."**

**Q2: "How does the ticket booking system handle a seat hold that the user abandons (closes the browser)?"**

The Redis lock on the seat has a 10-minute TTL (EX 600). Even without any application-level cleanup, the lock expires automatically after 10 minutes. A background reaper job runs every 30 seconds and issues `UPDATE seats SET status = 'AVAILABLE' WHERE status = 'HELD' AND hold_expires_at < NOW()` in PostgreSQL, then increments `shows.available_seats` accordingly. The reaper also queries the `waitlist` table for users waiting on those now-available seats and publishes a `seats.released` event to Kafka, which triggers the notification service to alert waitlisted users. **The key phrase is "TTL as the safety net; the reaper as the cleanup mechanism."**

**Q3: "How do you store a recurring event that happens every Monday for 1 year?"**

Exactly one database row is stored — the master event record with `rrule = 'RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=52'`. When a user queries their calendar for any week or month, the Calendar & Event Service uses the `dateutil.rrule` library to expand that single RRULE string into the relevant occurrences in memory, filters for the requested time window, and returns synthesized instance objects. No rows are pre-materialized per occurrence. If the user modifies one specific Monday (say, moves it 30 minutes later), an exception row is inserted with a `recurrence_id` pointing to the master and `start_time` set to the original Monday's time. **The key phrase is "expand on read, not pre-materialized."**

**Q4: "Why does your code deployment system need both a Redis lock AND a PostgreSQL unique constraint? Isn't one enough?"**

Redis provides speed — lock acquisition in < 1 ms, and automatic TTL expiry when a deployment times out. But Redis can lose data on restart or during a split-brain scenario. PostgreSQL provides durability — the `UNIQUE(service_id, env_id)` constraint on `deployment_locks` means even if Redis returns a false "lock acquired," the `INSERT` into PostgreSQL will fail with a unique constraint violation, and the system rolls back. The PostgreSQL record is also the authoritative source for the "what is currently deployed where" dashboard, the stale lock recovery job, and the audit trail. **The key phrase is "Redis for speed, PostgreSQL for crash-safe correctness — never either alone."**

**Q5: "How do you handle a Stripe payment webhook that never arrives?"**

A scheduled reconciliation job runs every 5-10 minutes and queries all bookings/reservations in PENDING state for longer than 15 minutes. For each, it calls `stripe.PaymentIntent.retrieve(payment_id)` directly. If Stripe says succeeded: call `confirm_booking(booking_id)`. If Stripe says failed: run the compensating transaction (release inventory, mark booking FAILED). If Stripe says still processing: extend the timeout window and check again later. This polling job is the correctness guarantee; webhooks are just the optimization that makes most bookings confirm in under 1 second. **The key phrase is "webhooks as optimization, polling as the correctness guarantee."**

**Q6: "How does the calendar service deliver 106 million reminders per day within 30 seconds of the scheduled time?"**

Reminders are scheduled in a Redis sorted set (`reminders:pending`) with score equal to the `fire_at` Unix timestamp. A Reminder Poller service runs every 5 seconds and executes an atomic Lua script: `ZRANGEBYSCORE reminders:pending -inf {now+5}` then `ZREM` on the fetched entries. The fetched reminder IDs are published to a Kafka topic `reminders.due`. Reminder Delivery workers consume from Kafka and call email (SendGrid), push (FCM/APNs), or SMS (Twilio) APIs. Durability is provided by Cassandra's `event_reminders` table, which is used to repopulate Redis if the sorted set is lost. **The key phrase is "Redis sorted set as the scheduler, Cassandra as the durable backup."**

**Q7: "What is the canary deployment strategy and why would you use it?"**

Canary deployment routes a small percentage of production traffic (e.g., 10%) to the new version while 90% continues to the stable version. This allows real production traffic to validate the new version before full rollout. The Traffic Controller sets Istio VirtualService weights: 90% to stable pods, 10% to canary pods. The Health Check Engine monitors error rates and latency against thresholds; if they exceed limits at any stage, the orchestrator immediately sets canary weight to 0% and deletes the canary pods — rollback in < 5 seconds. This is the appropriate strategy for high-stakes services (payment APIs, authentication) where a bug reaching 100% of traffic could be catastrophic. **The key phrase is "validate with real traffic at low blast radius before full commitment."**

---

### Tier 2: Deep Dive Questions (5–7, with why + trade-offs)

**Q1: "Why do you use `ORDER BY date ASC` (or `ORDER BY seat_id ASC`) in your SELECT FOR UPDATE queries? What happens without it?"**

Without a consistent lock acquisition order, two transactions booking overlapping resources will deadlock. Suppose Transaction A books June 1-2 (acquires lock on June 1, then waits for June 2) and Transaction B books June 2-3 (acquires lock on June 2, then waits for June 1). Each waits for the other — classic deadlock. PostgreSQL detects this and kills one transaction with an error, but the detection takes time (default deadlock_timeout = 1 second), causing latency spikes and wasted retries under load. By always acquiring locks in `ORDER BY date ASC` (or `ORDER BY seat_id ASC`), both transactions try for June 1 first, so they serialize rather than deadlock. The trade-off is slightly longer wait time for one transaction (serialization), but no deadlocks. This is a correctness requirement, not an optimization.

**Q2: "You mentioned the outbox pattern. What is the risk if the outbox relay crashes after polling the outbox but before publishing to Kafka? How do you prevent duplicate processing?"**

The relay fetches outbox rows with `SELECT * FROM outbox WHERE status='pending' LIMIT 100 FOR UPDATE SKIP LOCKED`. `FOR UPDATE SKIP LOCKED` ensures that two relay workers running concurrently do not pick up the same rows. The relay updates status to 'processing' before making the external call. If the relay crashes after updating to 'processing' but before completing the Kafka publish or Stripe call, a recovery job with a timeout (e.g., rows stuck in 'processing' for more than 5 minutes are reset to 'pending') allows another worker to retry. Because all external calls use idempotency keys, the retry is safe — Stripe returns the same result, Kafka deduplication by message key prevents duplicate events. The trade-off of the outbox pattern is polling overhead and the possibility of at-least-once delivery (handled by idempotency); the benefit is no lost messages even under process crashes.

**Q3: "For the calendar service, you chose Cassandra for events but PostgreSQL for ACLs. Walk me through why you didn't just use one database for everything."**

The two datasets have fundamentally different access patterns. Events need: partition by `(user_id, calendar_id)`, range scan on `start_time`, massive horizontal write throughput (822 RPS writes, 200 TB total), and independent per-user partitions that never cross. Cassandra's wide column model is ideal — each user's partition is independent, and the clustering key `start_time ASC` makes the primary calendar range query O(log N + result size). ACLs need: relational JOINs ("find all calendars user X can access, including shared ones"), ACID transactions (granting/revoking permissions must not partially apply), and complex queries with predicates on non-primary-key columns. This is exactly what PostgreSQL is good at. ACL data is small (~1 GB for 2B users). Using Cassandra for ACLs would require denormalizing multiple query patterns, lose transactional guarantees, and add operational complexity without any scale benefit. The trade-off: operating two databases requires more operational expertise. The benefit: each database is used for what it is genuinely better at.

**Q4: "In blue-green deployment, you keep old pods running for 10 minutes after the traffic switch. During that window, you are using 2x compute resources. How do you justify this to someone concerned about cost?"**

The cost argument is straightforward: the alternative to 10 minutes of 2x resources is a 2-5 minute rollback time (spinning up old pods from scratch). If a production incident with a customer-facing service costs $50,000-$100,000 per minute in lost revenue and engineering time, spending an extra 10 minutes of compute at, say, $500 worth of extra EC2 instances is a trivially correct trade-off. Moreover, you can reduce the resource cost to near-zero by scaling the old deployment to 0 replicas (rather than deleting it) — the configuration is preserved, and scale-up happens in seconds. The old pods are still "there" conceptually; rollback is still sub-10-second. The real trade-off is operational complexity (maintaining two named deployments per service: blue and green), not cost.

**Q5: "You said the Redis DECRBY acts as the first gate for flash sales. What happens if a user passes the DECRBY gate but the subsequent seat hold fails because someone else already holds that specific seat?"**

The DECRBY counter operates at the show level (total available seats), not the seat level. Passing the DECRBY gate means "there are seats available in the show"; it does not mean "the specific seat you want is available." If the hold attempt fails because another user grabbed that exact seat, the DECRBY counter must be incremented back (`redis.incrby(key, seat_count)`) before returning the error to the user. The user is redirected to the seat map to pick alternative seats (which are re-rendered from Redis in real time). This creates a retry flow: the user may need to select different seats and re-attempt. The system correctly prevents the counter from under-reporting availability (compensating INCRBY on failure) while still blocking the much larger set of requests that would fail because the entire show is sold out. The operational risk is: if the compensating INCRBY fails (Redis crash between DECRBY and INCRBY), the counter drifts low. A reconciliation job syncs the Redis counter with `shows.available_seats` from PostgreSQL every 60 seconds.

**Q6: "The calendar conflict detection uses a Redis free/busy cache for 2 billion users. That's potentially billions of cache keys. How do you control memory usage?"**

Keys are created lazily: a `freebusy:{user_id}:{date}` sorted set only exists if the user has at least one event on that date. Keys expire after 90 days (matching the typical query window). An inactive user who has created no recent events has zero free/busy cache keys. With 500M DAU, each having ~200 events in the rolling 90-day window spanning ~200 unique dates, you have at most 500M × 200 × (500 bytes per key) ≈ 50 TB of Redis. That requires a large cluster (~500 nodes at 100 GB RAM each) but is operationally manageable. The alternative — querying Cassandra for every conflict check — would add 10-50 ms per event create/update for 822 write RPS, increasing latency unacceptably. The Redis cost is justified by the 10x latency improvement on the conflict detection hot path.

**Q7: "How does your hotel booking system handle the case where a user searches for availability, sees 2 rooms available, clicks book, but by the time they complete payment (3 minutes later), another user has already booked both rooms?"**

The reservation creation query runs `SELECT ... FOR UPDATE` on all `room_inventory` rows at the moment of booking, not at the time of search. If both rooms are now booked (booked_rooms = total_rooms), the `available_rooms` computed column returns 0, the availability check fails, and the system returns a 409 `ROOM_UNAVAILABLE` error. The user's search result is potentially stale (search results are cached for 60 seconds, and the checkout process takes up to 15 minutes). This is the correct behavior — it is analogous to a shopping cart showing a product as available until checkout. The solution at the UX level is to clearly communicate "availability is not guaranteed until booking is confirmed," and at the technical level, the 60-second search cache TTL and the locking-on-commit guarantee that the system never confirms a booking for a room that is truly unavailable.

---

### Tier 3: Staff+ Stress Test Questions (3–5, reason aloud)

**Q1: "Your ticket booking system is handling a Taylor Swift flash sale. 500,000 users are simultaneously in the virtual waiting room. The sale opens. You admit them at 500/user per second, but Kafka falls behind — booking_confirmed events are queued up for 10 minutes before PDF workers can process them. How do you handle this, and what are the downstream effects?"**

I would reason through this as follows. Kafka falling behind means PDF workers are delayed, but booking confirmations are not blocked — the booking is CONFIRMED in PostgreSQL as soon as payment succeeds and the seat status is updated to CONFIRMED. The user sees "Booking Confirmed" in the UI without the PDF link. The PDF URL is populated asynchronously when the PDF Worker finally processes the event, and the booking record is updated. The user experience impact: PDF download link appears 10+ minutes after confirmation rather than within 5 seconds. This is acceptable; the SLA on PDF generation is "within 5 minutes" under normal load.

For operational response: I would monitor Kafka consumer lag as a key metric, auto-scale PDF workers based on queue depth (using Kubernetes HPA with `queue_depth` as the custom metric), and potentially pre-warm additional worker pods before the flash sale opens. If the lag grows uncontrollably, I would shed load by delaying PDF generation for lower-priority events (e.g., events more than 2 weeks away) to prioritize PDFs for imminent events.

The real risk here is not the PDF delay — it is whether Kafka itself can sustain the booking_confirmed event throughput. At 1,111 bookings/second during flash sale, Kafka is ingesting ~1,111 events/second on the `booking-events` topic. Kafka clusters routinely handle 100,000+ messages/second. This is not a Kafka throughput problem; it is a consumer throughput problem. The fix is horizontal scaling of PDF workers, not Kafka topology changes.

**Q2: "Imagine your hotel booking system is deployed in three AWS regions (us-east-1, eu-west-1, ap-southeast-1) for global latency. A user in Paris should search against eu-west-1. But an Expedia OTA partner triggers a booking for a Paris hotel from us-east-1. How do you maintain inventory consistency across regions?"**

This is a genuinely hard problem. I would reason through three approaches:

**Option 1: Single global primary PostgreSQL (current design)**. All writes go to one region (say us-east-1). OTA partners writing from us-east-1 is natural; Paris users searching against eu-west-1 read from a cross-region read replica with acceptable staleness (< 5 seconds replication lag). This is the simplest approach and handles the described scenario correctly. The downside: write latency for a Paris hotel manager updating their inventory (they must cross the Atlantic to us-east-1). At 12 booking RPS, cross-region write latency adds ~100 ms. Acceptable.

**Option 2: Hotel-sharded multi-region writes (more complex)**. Route writes to the region where the hotel is located. A Paris hotel's `room_inventory` rows live in eu-west-1. A Tokyo hotel's rows live in ap-southeast-1. A booking from an OTA partner in us-east-1 for a Paris hotel routes to eu-west-1. This eliminates write latency for local hotel managers. The complexity: cross-region reads for search (searching Paris hotels from us-east-1 must query eu-west-1) require a routing layer. Elasticsearch is still global (replicated to all regions). Room inventory queries are routed to the authoritative region.

**Option 3: CockroachDB or Spanner (distributed ACID across regions)**. These databases provide distributed ACID transactions with automatic geo-routing. CockroachDB allows setting follow-the-workload or explicit lease preferences per key range. A Paris hotel's `room_inventory` rows would be homed to eu-west-1 with Raft replicas in other regions. Writes have low latency (Raft within the region), reads have low latency. The trade-off: significantly higher operational cost, ~50 ms added latency for Raft consensus vs. single-node PostgreSQL writes.

My recommendation: start with Option 1 (single global primary). At 12 write RPS, it is entirely sufficient. If write latency for hotel managers becomes a complaint, evaluate Option 2 with hotel-id-based region routing. Option 3 is a future-state architectural evolution.

**Q3: "You've designed the code deployment system with a mandatory HEALTH_CHECK state and automatic rollback. But what if the health check itself is flawed — it passes for a bad deployment because the health check endpoint returns 200 even though the service is silently corrupting database records? How would you detect this?"**

This is the classic problem that observability engineers call a "false negative" on health checks. The immediate answer: HTTP `/healthz` probes are necessary but not sufficient for production correctness. The Health Check Engine should also run:

**Metric-based checks**: query Datadog/Prometheus for error rates (`HTTP 5xx rate > 1%` threshold), P99 latency (`> 2x pre-deployment baseline`), and business metrics (`orders_created_per_second < 80% of pre-deployment value`). A service that is silently corrupting records may not have elevated HTTP errors but will have degraded business throughput.

**Synthetic transactions**: for payment or booking services, the health check can execute a test transaction (e.g., create and immediately cancel a test booking in a shadow environment). If the write and the subsequent read disagree, the check fails. This is the most direct way to catch silent data corruption.

**Dark read verification**: after the canary reaches 10% traffic, read back a sample of records written by the canary pods and compare them with the expected schema/values. Any silent serialization bugs or schema mismatch will surface immediately.

**Canary duration** is the final safety valve: if the canary runs at 10% for 10 minutes rather than 30 seconds, you have a much larger sample of real transactions to detect anomalies. Configurable `ramp_interval_seconds` defaults to 600 (10 minutes), not 30 seconds, for high-stakes services.

The honest acknowledgment: no health check system can catch every class of bug. Some bugs (like a slowly accumulating race condition) only manifest at scale or over days. Post-deployment monitoring, incident on-call, and a culture of "undo first, debug second" are the non-technical complements to the technical system.

**Q4: "You store the full Kubernetes manifest as a JSONB snapshot in the deployment record for rollback. What are the failure modes of this approach?"**

Let me enumerate them. First, **secret drift**: the K8s manifest snapshot may reference a ConfigMap or Secret by name that has since been deleted or rotated. Replaying the manifest fails because the referenced secret no longer exists. Fix: also snapshot the ConfigMap values (not secret values — those are in Vault) and re-apply them as part of the rollback.

Second, **CRD version mismatch**: the manifest uses a custom resource definition (e.g., an Istio VirtualService API version) that has been upgraded in the cluster since the deployment. The old manifest uses `networking.istio.io/v1alpha3` but the cluster only accepts `networking.istio.io/v1beta1`. Rollback fails with a schema validation error. Fix: the Kubernetes Client should attempt conversion; if conversion fails, fall back to a simplified manifest without the now-incompatible resources (and alert the SRE).

Third, **storage volume conflicts**: a Rolling Update leaves old pods with persistent volumes that have data written by the new version. Replaying the old manifest redeploys old code against new-format data. This is the most dangerous failure mode. Fix: the rollback system should check for persistent volumes and, if found, gate the rollback on a manual SRE approval step. Automated rollback for stateless services; manual approval for stateful ones.

Fourth, **manifest size limit**: PostgreSQL JSONB columns can store large documents, but a complex K8s manifest (Deployment + multiple Services + Ingress + HPA + PDB + VirtualService) may exceed practical JSONB query performance. Fix: store manifests in S3 and keep only the S3 URI in the JSONB column.

The key insight: manifest-snapshot-based rollback is highly reliable for stateless services and should be automated. For stateful services with databases, migration-aware rollback requires human judgment and should require explicit approval.

**Q5: "Calendar Service at Google scale: 500 million DAU means 17,000 calendar view RPS. Each view requires expanding potentially hundreds of recurring events. How do you ensure p99 < 200 ms at that throughput?"**

I would decompose the latency budget: 200 ms p99 total. The Cassandra query to fetch master events for a user's calendar for a month should take < 20 ms (single partition read, clustering key range scan). The in-memory RRULE expansion for a typical user (say 50 recurring events, monthly view = 50 expansions of ~4 instances each = 200 instances) takes < 1 ms in Python with dateutil. Serialization and network overhead: ~10 ms. Total data path: ~30 ms. Most of the 200 ms budget is available for hot path optimization and tail latency accommodation.

The problem arises for users with extreme event counts (thousands of recurring events) or ultra-complex RRULE patterns. Defense: set a maximum expansion depth (e.g., 2,500 instances per calendar view request). Paginate if the result would exceed this.

For the 17,000 RPS, horizontal scaling of Calendar & Event Service pods handles throughput (each request is CPU-bound on expansion, not I/O-bound after the Cassandra read). The Cassandra cluster handles 17,000 partition reads/second across millions of partitions with ease — each partition is independently served by its owning Cassandra node.

The last-mile optimization for popular shared calendars: a public "holidays" or "company events" calendar shared with all employees is queried 500M times/day. Pre-expand it for the current and next 3 months and cache the result in Redis with a 1-hour TTL. This converts 17,000 Cassandra reads/second for that one calendar into a single Redis GET per request.

---

## STEP 7 — MNEMONICS

### The Shared Pattern Mnemonic: **"PRKSAI"** (think: "Practise Rigorously, Keep Systems Accurate, Iterate")

- **P**ostgreSQL: authoritative source of truth for all transactional state
- **R**edis: fast-path distributed lock with TTL (SETNX + EX)
- **K**afka: async side effects decoupled from critical write path
- **S**aga + outbox: coordinating external systems without distributed transactions
- **A**udit log: immutable append-only record of every state change
- **I**dempotency keys: all external calls retryable without side effects

### The Locking Sequence Mnemonic: **"RELOAD"**

When asked how to prevent double-booking, walk through:
- **RE**dis SETNX: fast-path rejection, no DB touch
- **L**ock rows with SELECT FOR UPDATE (ordered to prevent deadlock)
- **O**nly proceed if resource is AVAILABLE in DB (verify state is still valid)
- **A**tomically UPDATE (decrement count or set status = HELD)
- **D**urable record (INSERT reservation/booking in same transaction)

### Opening one-liner

For any problem in this group, open with: "This is fundamentally a resource reservation problem under concurrent contention. The interesting challenges are: how do we prevent double-booking without killing throughput, and how do we coordinate with external systems like payment without distributed transactions. I'll use a dual-layer lock — Redis for speed, PostgreSQL for correctness — and the outbox pattern for the external system coordination."

This one sentence tells the interviewer you understand the core challenge and have a principled architecture for it before you've drawn a single box.

---

## STEP 8 — CRITIQUE

### What the Source Material Covers Well

The source material is exceptionally thorough on the following:

**Concurrency control** is covered in precise, production-grade detail. The dual-layer lock pattern (Redis SETNX + PostgreSQL SELECT FOR UPDATE), sorted lock acquisition for deadlock prevention, and the two-phase commit problem with external payment are all explained with working pseudocode. The distinction between "Redis holds the lock for the duration of the hold (10 minutes)" vs. "PostgreSQL holds the lock for the duration of the transaction (milliseconds)" is clearly articulated — this is a level of precision most candidates miss entirely.

**The Saga / Outbox pattern** is explained with actual implementation code showing the atomic double-write (booking record + outbox row in one transaction), the polling relay, and idempotency key propagation to Stripe. The specific failure modes (crash between commit and publish, duplicate webhook delivery, payment success with booking write failure) are all addressed.

**Calendar Service recurring events** are covered with exceptional depth: RRULE storage format, expand-on-read algorithm with `dateutil.rrule`, exception row structure (recurrence_id pointing to master, start_time = original occurrence time), EXDATE handling, "THIS AND FUTURE" split implementation, and DST/timezone correctness. This is rarely covered this rigorously in interview prep material.

**Code Deployment strategies** are explained with production-grade implementation: canary ramp with Istio VirtualService patches, blue-green selector flip, K8s rolling update undo, health check engine with HTTP + metric + script checks, and the manifest JSONB snapshot approach for deterministic rollback.

### What Is Missing or Shallow

**Multi-tenancy and isolation for hotel booking**: the source material does not address how you isolate one hotel's inventory operations from another hotel's during high-load scenarios. If Hotel A generates 10,000 booking RPS (a viral Airbnb-style property), its PostgreSQL lock contention should not affect Hotel B. The solution (partition by hotel_id with separate connection pools or shard routing) is implied but not stated.

**Database migration strategy for code deployment**: Section 6.2 Question 5 in the deployment source mentions "expand-contract pattern" but only in a Q&A answer, not as a structured design. For a staff-level interview, being asked "how do you handle a deployment that requires a backward-incompatible database schema change" requires a detailed answer about migration stages, feature flags, and dual-write periods.

**Geo-replication and multi-region failover**: all four systems are designed with a single-region assumption. The source material mentions read replicas but does not address the question: "what happens if your primary PostgreSQL goes down?" A complete answer includes promotion of a synchronous replica, RPO = 0 (one synchronous replica), and RTO = 30-60 seconds.

**Payment method tokenization and PCI DSS scope reduction**: hotel and ticket booking both mention Stripe integration but do not discuss how to avoid PCI DSS scope. The correct answer: Stripe.js / Stripe Elements in the front-end tokenizes card data before it reaches your servers. Your servers only receive payment_method_id tokens, never raw card numbers. This is a common follow-up in security-conscious interviews.

**Search service for ticket booking under flash sale**: the source material covers flash sale handling for the booking path (virtual waiting room, Redis DECRBY) but does not address how event listing and seat map search perform under the same load. 500,000 users simultaneously loading the seat map page for the same show could overwhelm the Discovery Service. The answer: pre-render the seat map as a static SVG served from CDN, updating dynamically via WebSocket for status changes, not by re-loading from the API.

### Senior and Staff-Level Probes to Watch For

**"What if your Redis cluster loses all data during peak booking traffic?"** — This is specifically designed to test whether you know that Redis is the fast path, not the source of truth. The answer: fall back to pure PostgreSQL locking (slower but correct), and the in-flight holds that were in Redis but not yet written to PostgreSQL are simply abandoned (users retry; they receive "seat unavailable" and must reselect). No booking is double-confirmed; some holds are lost prematurely. Acceptable.

**"What if two calendar service workers both try to fire the same reminder at the same time?"** — This is the deduplication problem for the reminder system. The answer: `SETNX reminder:proc:{event_id}:{method}:{fire_at} {worker_id} EX 120`. Both workers attempt the SETNX; exactly one succeeds. The one that fails sees the key exists and skips delivery. Even if both somehow deliver (race on the SETNX), the user receives two reminders — annoying but not incorrect. The 120-second TTL cleans up.

**"Your deployment system's health check passes, but 30 minutes later you detect a memory leak in the new version. How do you roll back when the current deployed version is now at 100% traffic and the previous version's pods no longer exist?"** — The answer: the manifest snapshot in `deployments.manifest JSONB` for the previous SUCCEEDED deployment allows you to re-create the old deployment exactly. Call `kubectl apply -f <previous_manifest>` via the K8s Client. Kubernetes creates new pods with the old image. Once they are ready, the Traffic Controller points all traffic to the old deployment. The current "new" deployment is then scaled to 0 or deleted. Total rollback time: 2-5 minutes (pod startup time).

### Common Traps That Trip Candidates

**Trap 1: Making Redis the source of truth for booking confirmation**. Some candidates propose "just check Redis for availability and accept the booking if the Redis counter is > 0." This is wrong. Redis loses data on restart and cannot participate in ACID transactions. The PostgreSQL lock is the only correctness guarantee.

**Trap 2: Using optimistic locking (version columns) as the primary concurrency mechanism for flash sales**. Optimistic locking works well at low contention (it avoids lock overhead). But when 3,333 concurrent requests all read version = 5 and all try to UPDATE WHERE version = 5, all but one fail and must retry. Under flash sale conditions, the retry amplification overwhelms the system. Use pessimistic locking (SELECT FOR UPDATE) for the seat/room write, gated by Redis to control DB traffic.

**Trap 3: Pre-materializing all recurring event instances**. Creating 52 rows for "every Monday for a year" seems simpler. But with 2B users creating 40% recurring events, each with ~52 instances, you have 2B × 0.4 × 52 = ~41B rows. That is not 200 TB; it is a completely unmanageable dataset. More importantly, changing a recurring event ("move all future Mondays to Tuesday") requires updating thousands of rows. Expand on read with RRULE is not just a storage optimization; it is the only operationally feasible approach at scale.

**Trap 4: Treating the outbox pattern as optional "for reliability"**. The outbox pattern is not an optimization for reliability; it is a correctness requirement for any system that must coordinate a database write with an external API call. Without it, a crash between the DB commit and the Stripe API call leaves the booking in PENDING forever with no mechanism for recovery.

**Trap 5: Designing Code Deployment as a simple job queue**. The key distinguisher is the **mandatory HEALTH_CHECK state**. Candidates who model it as "add deployment to queue, execute, done" miss the multi-stage orchestration, the canary feedback loop, and the fact that rollback for a partially-ramped canary (10% traffic exposed) is qualitatively different from rollback for a fully-promoted deployment. The state machine is not bureaucracy — it is the mechanism that makes automated rollback possible.

---

*End of Pattern 20 Interview Guide*
