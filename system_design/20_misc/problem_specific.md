# Problem-Specific Design — Misc (20_misc)

## Ticket Booking

### Unique Functional Requirements
- Per-seat locking: exactly one hold per seat; no seat double-sold under flash sale conditions (10,000 concurrent requests)
- Hold expiry: unredeemed holds released after 10 minutes; waitlist notified immediately
- Virtual waiting room: flash sales use DECR counter to cap concurrent checkout sessions; excess users queued
- Saga compensation: Stripe payment failure must release seat hold atomically; no orphaned holds

### Unique Components / Services
- **Redis SETNX Per-Seat Lock**: `SETNX seat:{seat_id}:lock {hold_id} EX 600` — 10-minute TTL; acquired before PostgreSQL SELECT FOR UPDATE; if SETNX fails → seat already held → return 409 immediately without DB call; prevents thundering herd on hot seats
- **Sorted Lock Acquisition (Deadlock Prevention)**: multi-seat bookings acquire seat locks in ascending seat_id order — `ORDER BY seat_id` in SELECT FOR UPDATE; ensures all concurrent transactions acquire in same order, eliminating circular deadlock
- **Redlock (3 Redis Nodes)**: for critical flash-sale scenarios, Redlock across 3 Redis nodes (N=3, quorum=2); prevents single-Redis SPOF from allowing double-booking during Redis failover; standard SETNX sufficient for normal load
- **Virtual Waiting Room (Flash Sales)**: Redis DECR on `sale:{event_id}:slots` counter; counter initialized to max_concurrent_checkouts; when DECR returns < 0, INCR back and return queue position; excess users get queue position and poll for their turn
- **Saga Orchestration + Outbox**: Saga steps: (1) hold_seat → (2) create_payment_intent → (3) confirm_payment → (4) issue_ticket; each step idempotent; outbox table written atomically with hold record; compensating transaction on Stripe failure: `UPDATE holds SET status='expired'; UPDATE seats SET status='available' WHERE seat_id = ANY($1)`
- **Expired Hold Reaper**: runs every 30 s; `UPDATE seats SET status='available', hold_id=NULL WHERE status='held' AND hold_expires_at < NOW() RETURNING seat_id`; for each released seat, check waitlist (`SELECT TOP 1 FROM waitlist WHERE seat_id = $1 ORDER BY created_at ASC`) and send notification

### Unique Data Model
- **seats**: event_id, section, row, number, status (available/held/sold), hold_id FK, hold_expires_at TIMESTAMPTZ, price_tier
- **holds**: seat_id, user_id, expires_at TIMESTAMPTZ, payment_intent_id, status (active/expired/converted)
- **bookings**: hold_id, user_id, seat_id, payment_intent_id, amount_cents, status (pending/confirmed/cancelled/refunded), ticket_pdf_s3_key
- **waitlist**: event_id, seat_id (nullable for any-seat), user_id, created_at, notification_sent_at
- **outbox**: booking_id, event_type (payment_capture/ticket_email/analytics), payload JSONB, status (pending/processing/done/failed), attempts, next_retry_at

### Key Differentiator
Ticket Booking's uniqueness is its **Redis SETNX per-seat lock + sorted multi-seat acquisition + virtual waiting room**: SETNX TTL=600 per seat_id rejects concurrent holds without hitting PostgreSQL (O(1) Redis vs. O(1) DB with contention); multi-seat sorted-order acquisition `ORDER BY seat_id FOR UPDATE` eliminates deadlock cycles; Redlock across 3 Redis nodes prevents Redis failover from allowing double-booking; virtual waiting room `DECR sale:{event_id}:slots` caps flash-sale concurrency without per-user sessions; Saga + outbox ensures no orphaned holds even on Stripe failure or server crash.

---

## Hotel Booking

### Unique Functional Requirements
- Date-range availability query: check all N nights have available rooms in a single query (no N+1 per-date fetches)
- Overbooking: 5% buffer above physical capacity to compensate for no-shows; walk compensation procedure for over-capacity arrivals
- Partial date availability: return how many nights are available if full date range is fully sold out
- Minimal lock window: lock only rows for the exact nights requested; avoid locking unrelated dates

### Unique Components / Services
- **Date-Range Aggregate Availability Query**: single SQL query checks all N nights simultaneously:
  ```sql
  SELECT MIN(available_rooms) >= $rooms_needed
  FROM room_inventory
  WHERE hotel_id = $hotel_id
    AND date BETWEEN $check_in AND $check_out - INTERVAL '1 day'
  HAVING COUNT(*) = ($check_out - $check_in)
  ```
  `HAVING COUNT(*) = N_nights` guards against missing inventory rows; `MIN(available_rooms)` ensures ALL nights have capacity
- **Generated Column for Available Rooms**: `available_rooms GENERATED ALWAYS AS (total_rooms - booked_rooms - blocked_rooms) STORED`; never updated directly; auto-recalculated on INSERT/UPDATE to `booked_rooms` or `blocked_rooms`; eliminates update anomalies
- **Partial Index on Available Rooms**: `CREATE INDEX idx_hotel_date_avail ON room_inventory(hotel_id, date) WHERE available_rooms > 0`; availability search skips fully-booked dates at index level; typical occupancy 60–70% → partial index 30–40% smaller than full index
- **Monthly Partitioning**: `room_inventory` partitioned by month (`PARTITION BY RANGE (date)`); old months archived to cold storage; constraint exclusion drops irrelevant partitions from query plan
- **Overbooking Buffer**: `total_rooms = physical_room_count × 1.05` (5% overbooking); `available_rooms` can be > physical capacity but booking confirmation requires `available_rooms > 0`; historical no-show rate ~4–6% for most hotel categories
- **Walk Compensation LIFO**: when available_rooms reaches 0 after all confirmed bookings, last-booked guest is walked to comparable hotel; `SELECT booking_id FROM bookings WHERE hotel_id = $1 AND check_in = $2 ORDER BY confirmed_at DESC LIMIT 1`
- **Redis Availability Cache**: `hotel:avail:{hotel_id}:{checkin}:{checkout}` → room count (TTL 60 s); search results served from cache; invalidated on booking confirmation

### Unique Data Model
- **room_inventory**: hotel_id, room_type_id, date DATE, total_rooms SMALLINT, booked_rooms SMALLINT, blocked_rooms SMALLINT DEFAULT 0, available_rooms GENERATED ALWAYS AS (total_rooms - booked_rooms - blocked_rooms) STORED; PK (hotel_id, room_type_id, date); partitioned by RANGE(date) monthly
- **bookings**: hotel_id, room_type_id, user_id, check_in DATE, check_out DATE, nights SMALLINT GENERATED ALWAYS AS (check_out - check_in), status (pending_payment/confirmed/checked_in/checked_out/cancelled/walked), rate_plan_id, total_amount_cents, stripe_payment_intent_id
- **rate_plans**: hotel_id, room_type_id, name, rate_per_night_cents, cancellation_policy_id, valid_from DATE, valid_to DATE
- **Partial Index**: `CREATE INDEX idx_avail ON room_inventory(hotel_id, date) WHERE available_rooms > 0`

### Key Differentiator
Hotel Booking's uniqueness is its **date-range MIN aggregate availability query + GENERATED ALWAYS available_rooms column + overbooking buffer**: `SELECT MIN(available_rooms) HAVING COUNT(*) = N_nights` checks all nights atomically in one query (no N+1 per-date fetches); `available_rooms GENERATED ALWAYS AS (total_rooms - booked_rooms - blocked_rooms) STORED` eliminates update anomalies by computing the derived column at write time; 5% overbooking (`total_rooms = physical × 1.05`) captures hotel industry no-show economics; monthly partitioning + partial index `WHERE available_rooms > 0` keeps availability queries fast even at millions of hotel-nights.

---

## Calendar Service

### Unique Functional Requirements
- Recurring events: store RRULE once, expand instances on read — not pre-materialized per occurrence
- Multi-device sync: last-write-wins with TIMEUUID conflict detection; < 30 s propagation across devices
- Reminders: fire at user-specified offsets (e.g., 15 min before, 1 day before) across multiple methods (push/email/SMS); deduplication across workers
- External calendar interop: CalDAV/iCal (RFC 5545) protocol translation at API Gateway for Outlook/Apple Calendar/Google Calendar sync

### Unique Components / Services
- **RFC 5545 RRULE Expand-on-Read**: recurring events stored once in PostgreSQL with `recurrence_rule TEXT` (e.g., `RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=52`); `dateutil.rrule` library expands occurrences at query time for the requested time window; no pre-materialized rows per occurrence; exception dates stored in `recurrence_exceptions TEXT[]` on the event row
- **Cassandra Events Table**: `events(user_id UUID, calendar_id UUID, start_time TIMESTAMPTZ, event_id UUID, end_time, title, description, recurrence_rule, location, status)` PK = `((user_id, calendar_id), start_time, event_id)`; time-range queries hit Cassandra partition directly; 30-day TTL not applied here (events are durable); 90-day window is the most queried range
- **TIMEUUID Sync Changelog**: `sync_changelog(user_id UUID, change_id TIMEUUID, event_id UUID, operation TEXT, payload JSONB, device_id TEXT)` PK = `(user_id, change_id)`; 30-day Cassandra TTL; clients sync via `change_id > last_seen_change_id`; TIMEUUID provides monotonic ordering without clock skew issues; conflict resolution = last TIMEUUID wins
- **Redis Sorted Set for Reminders**: `reminders:{user_id}` ZSET with `score = fire_at_unix_timestamp`, `member = "{event_id}:{method}"`; Reminder Dispatcher polls `ZRANGEBYSCORE reminders:* -inf NOW() LIMIT 100` every 10 s; distributed deduplication via `SETNX reminder:proc:{reminder_id} {worker_id} EX 120` before firing
- **Free/Busy Cache**: `freebusy:{user_id}:{date}` → JSON list of busy intervals (TTL 5 min); used by scheduling assistant and invite flow; invalidated on event create/update/delete in that user's calendar
- **CalDAV/iCal Protocol Translation**: API Gateway translates CalDAV REPORT/PUT/DELETE to internal REST API; parses VEVENT VCALENDAR objects (RFC 5545) → internal event schema; serializes back to iCal format for CalDAV responses; supports VTIMEZONE for timezone-aware events

### Unique Data Model
- **events** (Cassandra): `(user_id, calendar_id)` partition key, `start_time` clustering key ASC, event_id; stores recurrence_rule TEXT (RRULE); exceptions TEXT[]; status (confirmed/tentative/cancelled)
- **event_reminders** (Cassandra or PostgreSQL): `((user_id), fire_at TIMESTAMPTZ, event_id, method)` — one row per reminder method per event; `method` in (push/email/sms); `sent_at TIMESTAMPTZ` set after delivery
- **sync_changelog** (Cassandra): PK `(user_id, change_id TIMEUUID)`; TTL 30 days; operation (create/update/delete); payload = full event JSON; enables incremental sync without full calendar download
- **shared_calendars** (PostgreSQL): `(calendar_id, grantee_user_id)` UNIQUE; permission (view/edit/admin); used by free/busy and attendee invite flows
- **PostgreSQL events**: `event_id, calendar_id, user_id, title, start_time, end_time, recurrence_rule TEXT, recurrence_exceptions TEXT[], attendees JSONB, status`; authoritative for modification; replicated to Cassandra for time-series queries

### Key Differentiator
Calendar Service's uniqueness is its **RFC 5545 RRULE expand-on-read + TIMEUUID sync changelog + Redis sorted-set reminder dispatch**: RRULE expand-on-read (dateutil.rrule) stores one row per recurring series regardless of occurrence count — no pre-materialization; TIMEUUID sync_changelog (30-day TTL) provides monotonic ordering for last-write-wins multi-device sync without vector clocks; Redis sorted set `score=fire_at_unix` enables O(log N) range query for due reminders + SETNX deduplication across workers; CalDAV/iCal translation at API Gateway makes the service natively compatible with all major calendar clients.

---

## Code Deployment

### Unique Functional Requirements
- One active deployment per service+environment at a time: concurrent deploys to same target are blocked
- Multi-stage canary: traffic ramp 10% → 25% → 50% → 100% with automated health check gate at each step
- Rollback: single API call reverts to previously deployed artifact; manifest stored for exact reproduction
- Deployment history: immutable audit of every deployment with actor, artifact, status, duration

### Unique Components / Services
- **Deployment State Machine**: `PENDING → IN_PROGRESS → HEALTH_CHECK → SUCCEEDED / FAILED / ROLLED_BACK`; HEALTH_CHECK is mandatory — no deployment can reach SUCCEEDED without passing health checks; state transitions written to PostgreSQL with timestamp; invalid transitions rejected (e.g., SUCCEEDED → IN_PROGRESS blocked)
- **Dual-Layer Deployment Lock**: Layer 1: `SETNX deploy:lock:{service_id}:{env_id} {deployment_id} EX 3600` — fast Redis rejection of concurrent deploy requests; Layer 2: `INSERT INTO deployment_locks (service_id, env_id, deployment_id) ON CONFLICT (service_id, env_id) DO NOTHING` — PostgreSQL UNIQUE(service_id, env_id) as authoritative guard; both layers needed: Redis for speed, PostgreSQL for crash-safe guarantee
- **Canary Traffic Controller**: uses Istio VirtualService weights for Kubernetes deployments; ramp schedule: 10%→25%→50%→100%; 30 s health check between each step; health check queries Datadog/Prometheus for HTTP 5xx rate and P99 latency; automatic rollback if thresholds exceeded at any ramp stage
- **Health Check Engine**: three check types per deployment: (1) HTTP probe: `GET /healthz` → 200 OK required; (2) metric check: `error_rate < configured_threshold` (default 1%) via metrics API; (3) script check: custom shell script with exit code 0 = pass; all three types must pass for HEALTH_CHECK → SUCCEEDED transition
- **Manifest Snapshot for Rollback**: on every SUCCEEDED deployment, full K8s manifest (Deployment + Service + ConfigMap) stored in `deployments.manifest JSONB`; rollback = reuse previous deployment's manifest, create new deployment record pointing to old artifact + manifest; preserves rollback history chain

### Unique Data Model
- **deployments**: service_id, environment_id, artifact_id, strategy (blue_green/canary/rolling), status (PENDING/IN_PROGRESS/HEALTH_CHECK/SUCCEEDED/FAILED/ROLLED_BACK), canary_weight SMALLINT, manifest JSONB (K8s manifest snapshot), triggered_by, started_at, completed_at, duration_seconds GENERATED ALWAYS AS (EXTRACT(EPOCH FROM completed_at - started_at)) STORED
- **services**: name, owner_team, repo_url, default_environment_id, max_deployment_duration_seconds INT DEFAULT 3600
- **environments**: name (staging/production/canary), cluster_endpoint, namespace, deploy_constraints JSONB (required_approvers, maintenance_window)
- **deployment_locks**: `(service_id, env_id)` UNIQUE PRIMARY KEY, deployment_id, acquired_at; one row = one active deploy; released on terminal state
- **audit_log**: deployment_id, actor_id, actor_type (USER/CI_BOT/SYSTEM), action (deploy_triggered/health_check_passed/canary_advanced/rollback_initiated), before_state, after_state JSONB, ip_address, created_at; no UPDATE/DELETE ever

### Algorithms

**Canary Health Check Loop:**
```python
for weight in [10, 25, 50, 100]:
    set_canary_weight(service, weight)
    time.sleep(30)
    metrics = get_metrics(service)  # Datadog/Prometheus
    if metrics.error_rate > threshold or metrics.p99_latency > threshold:
        rollback(deployment)
        return
set_status(deployment, "SUCCEEDED")
```

### Key Differentiator
Code Deployment's uniqueness is its **dual-layer deployment lock + mandatory HEALTH_CHECK state + canary feedback loop**: dual-layer (Redis SETNX + PostgreSQL UNIQUE(service_id, env_id)) prevents concurrent deploys with both speed (Redis) and crash-safety (PostgreSQL); HEALTH_CHECK is a mandatory intermediate state — no path from IN_PROGRESS to SUCCEEDED without passing HTTP + metric + script checks; canary 10%→25%→50%→100% with 30 s health checks at each step enables automated rollback before full traffic exposure; manifest JSONB snapshot on each SUCCEEDED deployment makes rollback deterministic (exact previous artifact + K8s config reproduced).
