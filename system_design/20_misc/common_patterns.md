# Common Patterns — Misc (20_misc)

## Common Components

### PostgreSQL as Authoritative Source of Truth
- All four systems use PostgreSQL for their authoritative state; Redis serves as a fast-path cache, not source of truth
- ticket_booking: `seats`, `bookings`, `holds`, `waitlist` — ACID transactions with SELECT FOR UPDATE; holds expire via TTL-based cleanup
- hotel_booking: `room_inventory (hotel_id, date, total_rooms, booked_rooms, blocked_rooms, available_rooms GENERATED ALWAYS AS ...)`, `bookings`, `rate_plans` — date-range inventory tracked row-per-date
- calendar_service: `events`, `calendars`, `event_reminders`, `shared_calendars` in PostgreSQL for relational integrity; Cassandra for time-series event data and sync_changelog
- code_deployment: `deployments`, `services`, `environments`, `deployment_locks`, `audit_log` — state machine lifecycle managed in PostgreSQL

### Redis Distributed Lock with TTL
- All four use Redis for fast-path distributed locking with expiry to prevent deadlocks from crashed clients
- ticket_booking: `SETNX seat:{seat_id}:lock {hold_id} EX 600` — 10-minute TTL per seat; lock acquired before SELECT FOR UPDATE; prevents thundering herd on hot seats
- hotel_booking: `SETNX hotel:{hotel_id}:booking:{date_range}:lock {request_id} EX 30` — short TTL; lock gates availability check + INSERT
- calendar_service: `SETNX reminder:proc:{reminder_id} {worker_id} EX 120` — prevents duplicate reminder delivery across workers
- code_deployment: `SETNX deploy:lock:{service_id}:{env_id} {deployment_id} EX 3600` — prevents concurrent deploys to same service+environment; Redis is fast-path, PostgreSQL UNIQUE is authoritative

### Dual-Layer Locking (Redis Fast Path + PostgreSQL Authoritative)
- ticket_booking and code_deployment explicitly implement dual-layer: Redis SETNX for fast rejection of concurrent requests, PostgreSQL SELECT FOR UPDATE or UNIQUE constraint as authoritative guarantee
- ticket_booking: Redis SETNX per seat → if acquired, proceed to PostgreSQL SELECT FOR UPDATE on seat row; if Redis fails (crash), Reaper reclaims after TTL; PostgreSQL is the ground truth for hold validity
- code_deployment: Redis SETNX `deploy:lock:{service_id}:{env_id}` → INSERT into `deployment_locks (service_id, env_id)` with UNIQUE constraint; constraint violation = concurrent deploy attempt; both layers needed for crash resilience

### Kafka for Async Side Effects
- All four use Kafka to decouple the critical booking/deployment write path from notification, analytics, and downstream processing
- ticket_booking: `booking-events` topic → email/SMS confirmation consumer, analytics consumer, waitlist processor
- hotel_booking: `booking-events` → payment capture consumer (Stripe), property management system sync, loyalty points processor
- calendar_service: `calendar-events` → notification fan-out (push/email/SMS reminders), attendee invite processor
- code_deployment: `deployment-events` → Slack notification consumer, metrics collection consumer, audit enrichment consumer

### Saga / Outbox Pattern for Distributed Transactions
- ticket_booking and hotel_booking both involve external payment systems (Stripe) and must coordinate booking with payment without a distributed transaction
- Outbox pattern: booking record + outbox message written atomically in PostgreSQL TRANSACTION; background worker polls outbox → calls Stripe API → marks outbox message processed; idempotency via Stripe PaymentIntent `idempotency_key = booking_id`
- Saga compensation: if payment fails → compensating transaction releases seat/room; if notification fails → retry with exponential backoff; each step idempotent
- code_deployment: analogous Saga pattern for multi-stage deployment (reserve slot → deploy artifact → health check → commit or rollback)

### State Machine for Lifecycle Management
- All four use explicit state machines to track resource lifecycle with valid transitions only
- ticket_booking: hold → `PENDING` → `CONFIRMED` → `CANCELLED`; failed payment → `EXPIRED`; state guards prevent double-booking
- hotel_booking: `PENDING_PAYMENT` → `CONFIRMED` → `CHECKED_IN` → `CHECKED_OUT` → `CANCELLED`; overbooking override: `WAITLISTED`
- calendar_service: event status (tentative/confirmed/cancelled per iCal VEVENT STATUS); attendee RSVP (needs_action/accepted/declined/tentative)
- code_deployment: `PENDING → IN_PROGRESS → HEALTH_CHECK → SUCCEEDED / FAILED / ROLLED_BACK`; state transitions logged with timestamp and actor; no state can skip HEALTH_CHECK before SUCCEEDED

### Immutable Append-Only Audit Log
- All four maintain an immutable audit trail of state changes with actor, before/after state, and timestamp
- ticket_booking: `booking_audit_log` — booking_id, event_type (hold_created/payment_attempted/booking_confirmed/cancelled), actor_id, details JSONB; used for dispute resolution
- hotel_booking: `booking_audit_log` — overbooking decisions, rate changes, cancellation policies applied, walk compensation events
- calendar_service: `calendar_audit_log` — event mutations, share permission grants/revokes, external CalDAV sync conflicts
- code_deployment: `audit_log` table with no UPDATE/DELETE ever; deployment_id, actor_id, actor_type (USER/CI_BOT/SYSTEM), action, before_state JSONB, after_state JSONB, ip_address

## Common Databases

### PostgreSQL
- All four; authoritative source of truth for bookings, inventory, events, deployments; ACID transactions with SELECT FOR UPDATE for concurrent resource management; partial indexes for hot-path queries; read replicas for reporting

### Redis
- All four; distributed locking (SETNX + EX TTL), session caching, rate limiting; hot-path availability caches; queue for background job coordination

### Kafka
- All four; async event bus decoupling write path from downstream consumers; idempotent consumers via Kafka consumer group offset commits; enables retry without re-executing the booking transaction

## Common Communication Patterns

### SELECT FOR UPDATE with Sorted Lock Acquisition (Deadlock Prevention)
- ticket_booking: `SELECT * FROM seats WHERE seat_id = ANY($1) ORDER BY seat_id FOR UPDATE` — ORDER BY seat_id ensures all concurrent transactions acquire locks in the same order, preventing deadlock cycles
- hotel_booking: `SELECT * FROM room_inventory WHERE hotel_id = $1 AND date BETWEEN $2 AND $3 ORDER BY date FOR UPDATE` — ORDER BY date prevents deadlock when two bookings overlap on adjacent date ranges

### Idempotency Keys for External API Calls
- All four pass idempotency keys to external APIs (Stripe, email providers, push notification services) so that retries from the outbox worker do not double-charge or double-send
- ticket_booking: `stripe.PaymentIntent.create(idempotency_key=booking_id)`
- hotel_booking: `stripe.PaymentIntent.create(idempotency_key=f"hotel-{booking_id}")`

## Common Scalability Techniques

### Redis Cache for Availability / Status Hot Path
- All four use Redis to serve the most-queried read path without hitting PostgreSQL on every request
- ticket_booking: seat availability bitmap or BITSET cached per event; refreshed on hold expiry
- hotel_booking: `hotel:avail:{hotel_id}:{checkin}:{checkout}` → available room count (TTL 60 s); invalidated on booking confirmation
- code_deployment: `deploy:status:{deployment_id}` → current state (TTL 300 s); dashboard polling hits Redis, not PostgreSQL

### Background Reaper / Cleanup Job
- ticket_booking: Reaper runs every 30 s; `UPDATE seats SET status = 'available' WHERE status = 'held' AND hold_expires_at < NOW()`; releases expired holds back to available inventory; wakes waitlist
- hotel_booking: cleanup job releases unconfirmed payment-pending bookings after 15 min
- calendar_service: reminder cleanup after fire_at + 24 h; sync_changelog TTL 30 days (Cassandra TTL handles automatically)
- code_deployment: stuck IN_PROGRESS deployments timed out after `max_duration_seconds`; Reaper marks FAILED and releases deployment_lock

## Common Deep Dive Questions

### How do you prevent double-booking under high concurrency?
Answer: The pattern across all booking systems is dual-layer locking: (1) Redis SETNX with TTL for fast rejection — the first request to acquire the lock proceeds, all others fail immediately without hitting the database; (2) PostgreSQL SELECT FOR UPDATE as the authoritative guard — even if two requests somehow pass Redis simultaneously (e.g., clock skew, Redis failover), SELECT FOR UPDATE serializes them at the database level. The acquired lock is held only for the duration of the atomic check-and-reserve transaction (< 100 ms), then released. Redis SETNX TTL ensures the lock is released even if the server crashes mid-transaction.
Present in: ticket_booking, hotel_booking, code_deployment

### How do you handle distributed transaction failures (e.g., payment success but booking DB write fails)?
Answer: The Saga + Outbox pattern: (1) write the booking record and an outbox message in a single PostgreSQL TRANSACTION — either both succeed or neither does; (2) a background worker polls the outbox and calls the external API (Stripe); (3) if the external call fails, retry with exponential backoff using the same idempotency key — Stripe will return the same result without double-charging; (4) if it permanently fails, the compensating transaction cancels the booking. The outbox table ensures no outbox message is lost even if the worker crashes mid-delivery.
Present in: ticket_booking, hotel_booking

## Common NFRs

- **Consistency**: no double-booking under any concurrency pattern; SELECT FOR UPDATE + dual-layer Redis lock
- **Latency**: booking confirmation < 500 ms P99 (Redis + PostgreSQL + Stripe pre-auth); calendar event load < 200 ms
- **Availability**: 99.9% for booking confirmation path; 99.99% for availability read path (served from Redis cache)
- **Audit**: all state transitions immutably logged; 7-year retention for financial compliance (bookings); 2-year for deployments
- **Idempotency**: all external calls retryable without side effects; booking operations idempotent via outbox + idempotency keys
