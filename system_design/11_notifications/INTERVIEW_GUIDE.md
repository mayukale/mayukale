Reading Pattern 11: Notifications — 3 problems, 10 shared components

---

# Pattern 11: Notifications — Complete Interview Study Guide

**Problems covered:** Push Notification Service, Email Delivery System, SMS Gateway

**Shared components counted:** API Gateway, Ingest Service, Kafka dual-topic, Dispatcher Workers, Cassandra delivery events, PostgreSQL source-of-truth, Redis (opt-out + frequency cap + cache), ClickHouse analytics, Idempotency layer, Frequency cap sliding window = **10 shared components**

---

## STEP 1 — WHAT THIS PATTERN IS AND WHY IT APPEARS

Notification systems are one of the most common system design problems at Staff+ levels because every company at scale sends push notifications, emails, and SMS. The problem looks deceptively simple — "just send a message" — but hides serious distributed systems complexity: at-least-once delivery without duplicates, fan-out to hundreds of millions of devices, external gateway dependencies (APNs, ISPs, carriers) with rate limits and failure modes entirely outside your control, real-time suppression enforcement for opt-out and compliance, and reputation management that can permanently harm your ability to reach users.

All three variants in this pattern share the same skeleton. You learn that skeleton once, then apply the three to four problem-specific differentiators for each channel. That is the leverage you get from this pattern.

**The three problems at a glance:**

- **Push Notification Service** — 500M registered devices, 1M notifications/sec peak for broadcasts, dual-protocol dispatch (APNs HTTP/2 for iOS, FCM HTTP v1 for Android), fan-out orchestration is the dominant design challenge
- **Email Delivery System** — 100M addresses, 50M recipients per bulk campaign, ISP deliverability (DKIM/SPF/DMARC), bounce and complaint handling, IP reputation management — the unique complexity is that ISPs are external gatekeepers who can blacklist you
- **SMS Gateway** — 10M SMS/day, 5,000 RPS OTP peaks, SMPP protocol stack, international carrier routing by MCC/MNC, GSM-7 vs. UCS-2 encoding, two-way STOP/UNSTOP compliance

---

## STEP 2 — MENTAL MODEL

### The Core Idea

A notification system is a **reliable, high-throughput one-way message delivery pipeline** that decouples internal producers (your backend services) from external delivery infrastructure (APNs, ISPs, mobile carriers) through durable queuing, with suppression and compliance guardrails applied before every message leaves the building.

### Real-World Analogy

Think of it like a post office at massive scale. Your internal services are customers who walk up to the counter and drop off a letter. The post office clerk (Ingest Service) validates the address, checks if the recipient is on a do-not-mail list, stamps the envelope, and drops it into a sorting bin (Kafka). Specialized sorting machines (Dispatcher workers) pick up letters from the bin, look up the right delivery truck route (APNs/SMTP/SMPP), and hand them off to the delivery drivers. The drivers are third-party contractors (Apple, ISPs, carriers) who actually ring the doorbell. If a driver reports back "this address doesn't exist," the post office flags it and never mails there again (suppression / token invalidation). Meanwhile, someone on the analytics floor tracks how many letters were opened and whether customers complained.

The postal analogy also captures why it's hard: the postal clerk must never lose a letter after accepting it, must never deliver twice, must know immediately when a recipient says "stop delivering," and the delivery drivers can be slow, throttle you, or go down entirely — and you have zero control over any of that.

### Why It's Hard

1. **At-least-once vs. exactly-once tension.** Kafka gives you at-least-once. External APIs (APNs, SMTP, SMPP) are not idempotent. You need a deduplication layer to bridge the two.

2. **Fan-out at scale.** Sending one notification to 500M devices in under 10 minutes requires 833,000 external API calls per second. No single process or connection can do that. You need orchestrated parallel work distribution, checkpointing for resumability, and priority separation so a broadcast does not starve unicast traffic.

3. **External dependency fragility.** APNs, Gmail SMTP, and mobile carriers all rate-limit you, reject stale tokens, and go down. Each has unique error codes and backoff requirements. The system must handle these gracefully without losing messages.

4. **Suppression correctness under concurrency.** If a user opts out, the very next notification must be suppressed — even if 174,000 notifications per second are flowing through the ingest path. This requires a read path fast enough to check on every single request.

5. **Deliverability reputation.** For email specifically, your sending reputation is a shared resource degraded by bounces and spam complaints. Once an IP is blocklisted, you cannot deliver. This introduces an entire operational concern absent from most distributed systems problems.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing a single box on the whiteboard, ask these. Each answer changes the design significantly.

**Q1: What channel or channels are in scope — push, email, SMS, or all three?**
What changes: each channel has a completely different external dispatch protocol (HTTP/2 APNs, SMTP, SMPP v3.4). If all three, you need a unified ingest layer with per-channel routing. If just one, focus depth over breadth.

**Q2: What are the scale expectations — how many notifications per day, and is there a broadcast requirement (sending to all users at once)?**
What changes: if there are large broadcasts, you need a fan-out orchestrator with sharded parallel execution. Without broadcasts, a simple per-user queue suffices. This is the single biggest architectural fork.

**Q3: What are the latency requirements — is this transactional (OTP, order confirmation) or marketing (newsletter)?**
What changes: transactional demands sub-5-second delivery and a very short Kafka TTL (15 min for OTP email — delivering an expired OTP is harmful). Marketing tolerates 48-hour Kafka TTL and batched throttling. These need separate Kafka topics and separate dispatcher pools.

**Q4: What compliance requirements are in scope — GDPR, TCPA, CAN-SPAM, CTIA?**
What changes: GDPR requires data erasure, purge of tokens and suppression data on account deletion. TCPA/CTIA requires that STOP keywords must immediately halt all SMS. CAN-SPAM requires one-click unsubscribe. Each has architectural implications on the suppression layer.

**Q5: Should the system support user preference management — per-category opt-out, quiet hours, frequency capping?**
What changes: adds a preferences store (Redis + PostgreSQL) and a mandatory check on the hot ingest path. Frequency capping requires a Redis sliding-window counter per user per channel per time window, which adds latency and Redis cluster load.

**Red flags in the answers:** If the interviewer says "assume simple scale like 10,000 users" for a push notification service — they probably still want you to talk about the design as if it will grow. Start at current scale, call out scaling paths explicitly. If they say "no compliance needed" — still mention GDPR suppression because interviewers often add it as a follow-up; you score points by proactively raising it.

---

### 3b. Functional Requirements

**Core requirements (apply to all three problems):**

1. Send a notification to a specific user or set of users on a specified channel
2. Support both transactional (high priority, time-sensitive) and marketing (lower priority, bulk) sends
3. User preference management — per-category opt-in/opt-out, global opt-out
4. Suppress sends to opted-out users before any external call is made
5. Idempotent API — duplicate sends with the same idempotency key must not double-deliver
6. Delivery status tracking — queryable per notification, available within 60 seconds of dispatch
7. Template management — store, version, and render templates with variable substitution
8. Retry with exponential backoff for transient failures; TTL-based discard for expired messages

**Push-specific additions:** Device token registration/deregistration, broadcast to all 500M devices, notification collapsing via collapse_key, badge count, rich media payload (title/body/image/action)

**Email-specific additions:** DKIM/SPF/DMARC authentication, bounce handling (hard vs. soft), FBL complaint handling, one-click unsubscribe (RFC 8058), open/click tracking pixels, IP pool management and warming

**SMS-specific additions:** E.164 number format validation, GSM-7/UCS-2 encoding detection, multipart splitting with UDH headers, two-way SMS (inbound MO), STOP/UNSTOP keyword handling, carrier routing by MCC/MNC

**Clear problem statement (push):** Design a push notification service that delivers 1M notifications/second at peak across APNs and FCM, guarantees at-least-once delivery, enforces user opt-out within 5 seconds, completes a broadcast to 500M devices within 10 minutes, and provides per-notification delivery analytics within 60 seconds.

---

### 3c. Non-Functional Requirements

**Derive these from the problem, do not just list them:**

**Throughput:** Work backwards. 500M devices, 10 min broadcast window → 500M / 600s = 833,000 dispatches/second. Your dispatcher pool and connection pool must sustain this as the peak.

**Latency:** Two tiers exist. Ingest API must return 202 Accepted in under 200ms — this is your SLA to producers and determines how many pods you need. End-to-end delivery (API call to APNs ack) must be under 5 seconds for unicast — this determines Kafka consumer lag budget and dispatcher concurrency.

**Availability:** 99.99% for the ingest API (52 minutes downtime/year). This requires multiple availability zones, health-checked load balancing, and stateless ingest pods that restart quickly. Note: 99.99% for the dispatcher is less critical — Kafka buffers messages during dispatcher outages.

**Durability:** Zero message loss after the ingest API accepts a message (202 Accepted). This means Kafka must have `acks=all` (wait for all replicas) and `min.insync.replicas=2`. The guarantee is at the Kafka boundary, not the APNs boundary — delivery failure at APNs is a different class of failure handled by retry.

**Trade-offs to name explicitly:**

- ✅ Async 202 pattern gives very low ingest latency but ❌ means producers cannot know synchronously if delivery succeeded — requires polling or webhooks
- ✅ At-least-once + idempotent consumer gives durability without complex exactly-once but ❌ requires a Redis deduplication check on every dispatch (adds ~1ms per message)
- ✅ Separate Kafka topics for transactional vs. marketing gives priority isolation but ❌ doubles Kafka infrastructure and consumer group complexity
- ✅ Redis for opt-out hot cache gives sub-millisecond checks but ❌ Redis outage can cause fail-open behavior (must design a graceful degradation path)

---

### 3d. Capacity Estimation

Walk through this out loud. Interviewers are watching whether you can anchor numbers to architecture decisions.

**Push Notification numbers:**

Daily volume: 200M DAU × 10 notifications/day = 2B targeted + 2 broadcasts × 500M = 1B = **3B/day total**

Average ingest RPS: 3B / 86,400s = **34,700 RPS**

Peak ingest RPS (5x spike): 34,700 × 5 = **174,000 RPS**

Peak dispatch RPS (broadcast): 500M devices / 600s (10 min target) = **833,000 RPS**

Storage: Device tokens = 500M × 300B = **150 GB**. Delivery events = 4.8B/day × 30 days × 100B = **14.4 TB**. Total 30-day hot storage ≈ **60 TB**.

**Architecture implications from these numbers:**

- 174,000 ingest RPS requires ~35-50 stateless Ingest Service pods behind a load balancer (assuming 5,000 RPS per pod)
- 833,000 APNs dispatch RPS: APNs allows 1,000 concurrent HTTP/2 streams per connection; you need at least 834 connections sustained; achieve this with 100 dispatcher pods × 10 connections each = 1,000 connections × 1,000 streams = 1M in-flight headroom
- Kafka unicast topic: at 174,000 messages/second, 1,000 partitions gives ~174 messages/second per partition, allowing each dispatcher pod to consume 10 partitions comfortably
- 14.4 TB delivery events: Cassandra is the right choice here — append-only writes, no cross-row transactions, TTL-based expiry, write throughput is the primary axis

**Time to say this in interview:** 3-4 minutes max. Do not over-elaborate. Hit the key numbers, call out the implications, move on.

---

### 3e. High-Level Design

**Draw these components in this order (whiteboard order matters — show causality):**

1. **API Gateway / Load Balancer** — entry point, TLS termination, OAuth2 validation, per-producer rate limiting. Draw the producer calling in from the left.

2. **Ingest Service** — stateless pods. Four steps: validate → suppression check (Redis) → frequency cap (Redis) → template render → publish to Kafka. Returns 202 immediately. This is the gatekeeper.

3. **Kafka (dual topics)** — `notifications.transactional` (high priority, 15-min TTL for push/SMS OTP, aggressive consumers) and `notifications.marketing` (lower priority, 48h TTL, throttled consumers). Partitioned by `user_id % N` to preserve per-user ordering and enable frequency cap enforcement.

4. **Dispatcher Workers** — stateless consumer pods per channel. For push: APNs pool (iOS) and FCM pool (Android). For email: SMTP relay workers. For SMS: SMPP connection pool + HTTP carrier adapters. These handle backpressure, error codes, retries.

5. **Fan-out Orchestrator** (push only, or any broadcast-capable system) — reads broadcast job, shards user space into 1,000+ tasks, publishes per-device dispatch tasks back to the unicast topic, checkpoints progress so crashes are resumable.

6. **Data stores** — Cassandra for delivery events (append-only, TTL), PostgreSQL for templates/preferences/suppression (ACID source of truth), Redis for hot-path reads (opt-out cache, frequency cap counters).

7. **Analytics pipeline** — Kafka → Flink → ClickHouse → Grafana. Mention it exists, do not deep-dive unless asked.

**Key data flow for unicast push (say this aloud):**
Producer POST → API Gateway → Ingest Service checks opt-out in Redis (1ms) → frequency cap check in Redis (1ms) → lookup device tokens from Cassandra (3ms) → render template → publish to Kafka unicast topic → Dispatcher Worker consumes → calls APNs HTTP/2 → response code processed → delivery event written to Cassandra.

**Key design decisions to call out explicitly:**
- Why 202 Accepted: decouples producer SLA from external delivery latency; APNs can take 1-5 seconds and we cannot hold the producer's HTTP connection open
- Why Kafka not SQS/RabbitMQ: replay capability (7-day retention for consumer crash recovery), consumer groups for horizontal scaling, partition-based ordering, high throughput
- Why Cassandra not MySQL for delivery events: append-only access pattern, TTL native, linear write scaling, no cross-row transactions needed
- Why Redis not Memcached: native sorted sets for sliding-window frequency cap, native SETNX for deduplication, persistence options

---

### 3f. Deep Dive Areas

Interviewers will probe 2-3 of these. Go deep and unprompted.

**Deep Dive 1: Broadcast Fan-out at Scale**

*Problem:* Sending to 500M devices sequentially at 833K/s takes exactly 600 seconds. A single process crash at second 300 means starting over. Naively, this also blocks all unicast notifications while the broadcast is running.

*Solution — Orchestrated sharded fan-out:*
The Fan-out Orchestrator reads the broadcast job from Kafka. It creates 1,000 shard tasks covering the full `user_id` space (hash ranges 0x0000 to 0xFFFF split into 1,000 equal chunks). Each shard task is published to a separate `notifications.fanout.shards` Kafka topic. A pool of Fan-out Worker pods (auto-scaled to 1,000 pods) each claim one shard, page through device tokens in Cassandra using token-range pagination (`TOKEN(user_id)` scan), filter opted-out users using a Redis bitset (500M users = 500M/8 = 62.5 MB — fits easily in Redis), and re-publish per-device dispatch tasks to the existing unicast Kafka topic.

Checkpoint: each worker writes its `last_paged_token` and shard status to Cassandra after every 10,000 devices processed. On pod crash, the shard task is re-delivered by Kafka and the new worker resumes from the checkpoint.

Priority separation: broadcast fan-out workers publish to lower-priority partitions of the unicast topic. Dispatcher threads are weighted to drain high-priority (unicast) partitions first.

*Unprompted trade-offs to add:*
- ✅ Shard count can be increased to 10,000 (reduce broadcast time to ~1 minute) at the cost of more Kubernetes pod churn
- ❌ Fan-out at shard time means tokens read slightly later than broadcast initiation — tokens registered in the gap are included, tokens deregistered in the gap may result in one extra send followed by an invalidation response
- ✅ Idempotency key = `broadcast_id + device_id` prevents double delivery even if a shard is reprocessed

**Deep Dive 2: Suppression, Opt-out, and Frequency Capping at Ingest Throughput**

*Problem:* At 174,000 ingest RPS, a synchronous PostgreSQL read for opt-out status would add 5-20ms per request and require thousands of DB connections. The system would immediately bottleneck on the database.

*Solution — Redis write-through with atomic Lua for frequency caps:*
Opt-out state is cached in Redis as a hash per user: `HSET user_prefs:{user_id} global_opt_out 0 cat:marketing 0`. On opt-out update, PostgreSQL is written first (source of truth), then Redis is updated synchronously before returning 200 to the user. This means the next ingest request for that user — which reads from Redis — will see the opt-out within milliseconds.

Frequency caps use a Redis sorted set per user per channel per window. An atomic Lua script runs in a single Redis round-trip: prune expired entries (`ZREMRANGEBYSCORE`), count current entries (`ZCARD`), compare to cap, conditionally add. This is ~0.5ms. At 174,000 RPS across 10 Redis shards (sharded by `user_id` hash), each shard handles ~17,400 RPS — well within Redis's 100K+ ops/second capacity per node.

Redis outage degradation: circuit breaker pattern. If Redis error rate > 1%: fail-open on frequency caps (allow through, slightly over-send rather than block all traffic), maintain a local in-memory LRU cache (built at startup from PostgreSQL) for opt-out enforcement. Postgres serves as fallback for hard opt-out checks at reduced throughput.

*Unprompted trade-offs:*
- ✅ Lua script is atomic per Redis shard — no race conditions for per-user frequency cap
- ❌ Redis sorted set frequency cap does not survive Redis restarts without AOF/RDB persistence — configure `appendfsync = everysec`
- ✅ Write-through on opt-out (not write-around) ensures the very next request after opt-out sees the suppression

**Deep Dive 3: Idempotency — At-Least-Once Without Duplicates**

*Problem:* Kafka guarantees at-least-once delivery. A dispatcher pod consuming a message, calling APNs successfully, and then crashing before committing its Kafka offset will re-consume the same message and call APNs again. The user receives two identical notifications.

*Solution — Two-layer idempotency:*

**Layer 1 (ingest-side):** Client sends `Idempotency-Key` header. Ingest Service does `SET ingest_dedup:{key} {notification_id} NX EX 86400` in Redis. If the key already exists, return the cached 202 response immediately without re-publishing to Kafka. PostgreSQL has a UNIQUE constraint on `idempotency_key` as a durable fallback for when Redis has been restarted and lost the key.

**Layer 2 (dispatch-side):** Before calling APNs/FCM/SMTP/SMPP, the dispatcher does `SET sent:{notification_id}:{device_id} 1 NX EX 86400` in Redis. If `NX` fails (key already exists), skip the external call and commit the Kafka offset. This is the critical guard: if a pod crashes after APNs call but before Kafka offset commit, the next consumer will hit this key and skip the re-send.

The 24-hour TTL on both keys is a deliberate choice. Longer TTL uses more Redis memory. Shorter TTL allows duplicates if a message is retried after the TTL expires. 24 hours covers all realistic retry windows.

*Unprompted trade-offs:*
- ✅ Two layers means duplicates require both Redis layers to fail simultaneously — extremely unlikely
- ❌ If Redis is completely unavailable, Layer 2 fails open — you will re-send if a dispatcher crashes. Accept this rare failure mode rather than blocking all sends when Redis is down
- ❌ The dispatch-side key only prevents re-send within 24 hours — for scheduled notifications, if the scheduled_at is >24 hours in the future, use the `notification_id` creation timestamp as the key basis, not the current time

---

### 3g. Failure Scenarios

Senior engineers frame failures in terms of **impact scope, detection method, and recovery path**, not just "and then we retry."

| Failure | Impact | Detection | Recovery |
|---|---|---|---|
| APNs outage | All iOS push delivery blocked | APNs 5xx / connection refused rate > 5% on dispatcher pods; Grafana alert | Circuit breaker opens; Kafka buffers messages up to TTL; retry resumes when APNs recovers. Messages past TTL discarded — acceptable for marketing; transactional messages have 5s TTL so brief outage causes loss (known SLO breach, accepted) |
| Kafka broker failure | Message ingestion fails if <2 in-sync replicas | Broker health alert; producer `RecordTooLargeException` / timeout | RF=3, `min.insync.replicas=2` prevents loss on single broker failure. If 2 brokers fail simultaneously, ingest API returns 503. Ops escalation path predefined |
| Fan-out coordinator crash mid-broadcast | Partial broadcast — some users never receive | Pod heartbeat timeout; broadcast job stuck in `dispatching` state > 30 min | Cassandra-checkpointed shard progress. New coordinator reads incomplete shards, re-publishes. Idempotency key (`broadcast_id+device_id`) prevents double delivery for already-dispatched shards |
| Redis cluster down | Opt-out checks fail; frequency caps fail | Redis error rate > 1% in ingest service | Fail-open on frequency caps; local in-memory opt-out cache (loaded from PostgreSQL at startup); direct PostgreSQL read for preference fallback at reduced throughput. Alert fires — Redis must be restored within SLO window |
| Device token mass invalidation (e.g., iOS OS update) | Flood of `BadDeviceToken` responses from APNs | APNs 400/410 error rate spike | Token invalidation pipeline marks tokens inactive; reverse lookup table finds re-registered tokens; Flink anomaly detection fires alert if >1% of tokens invalidated in 1 hour |
| IP blocklisted (email) | Marketing emails rejected by major ISPs | BlocklistMonitor DNS lookup every 10 min | IP marked inactive in `ip_pools`; campaign traffic rerouted to vendor fallback (SES/SendGrid); PagerDuty alert; delisting request submitted automatically |
| Duplicate Kafka message delivery | Notification sent twice | — (prevented by idempotency, not detected) | Layer 2 Redis `SETNX` before external call prevents re-dispatch. Monitor for duplicate delivery events in Cassandra as a signal that Layer 2 is not working |

**Senior framing to use:** "The interesting failure isn't a single component going down — it's correlated failures. APNs goes down at the same time as a broadcast is in progress. Now you have Kafka filling up with broadcast fan-out messages, and the unicast topic is also backed up. The circuit breaker on APNs should open, pausing dispatch. But the fan-out orchestrator should also pause publishing new per-device tasks once the unicast topic consumer lag exceeds a threshold — otherwise you fill Kafka with messages that can't be consumed anyway. This is a backpressure propagation problem across multiple queues."

---

## STEP 4 — COMMON COMPONENTS

Every component listed in `common_patterns.md`, explained with rationale.

### 1. API Gateway (TLS + OAuth2 + Per-Producer Rate Limiting)

**Why used:** You need a single ingress point that handles cross-cutting concerns before any business logic runs. All three notification problems have multiple internal producer services calling in. The gateway enforces TLS 1.2+ (so credentials never travel plaintext), validates OAuth2 client credentials tokens (so only authorized services can send notifications), and applies per-producer rate limits (so one runaway service cannot DoS the ingest layer).

**Key config:** Per-producer rate limit stored in a config store (e.g., Consul) so it can be changed without deploys. Rate limits should be tiered: transactional producers get 10,000 RPS, marketing producers get 10 RPS (use the campaigns API instead). Burst allowance via token bucket (not leaky bucket) so a short spike does not immediately 429.

**Without it:** An internal service bug floods the ingest with millions of duplicate requests, filling Kafka, and backing up dispatchers. No way to throttle without changing application code.

---

### 2. Ingest Service (Validate → Suppression Check → Template Render → Kafka Publish)

**Why used:** All external send logic must be decoupled from the producer's call. Producers cannot wait for APNs to respond. The Ingest Service is the synchronous face of the system — it takes a notification request, validates it, applies suppressions, renders the message body, and publishes to Kafka — all within the 200ms API SLA. It returns 202 Accepted; delivery is now asynchronous.

**Key config:** Stateless — no in-process state between requests. Horizontal scaling with Kubernetes HPA on CPU + Kafka consumer lag. Preference reads from Redis cache (< 1ms). Template compilation cached in local Caffeine LRU (10-minute TTL, 1,000 entries) to avoid re-parsing Handlebars templates on every request.

**Four mandatory steps in sequence:**
1. Schema validation (required fields, E.164 for SMS, valid domain for email)
2. Suppression check in Redis (global opt-out, category opt-out, hard bounce, STOP keyword)
3. Frequency cap check in Redis (sliding-window Lua script)
4. Template render + Kafka publish with idempotency dedup

**Without it:** Producers would block waiting for external delivery, and all the suppression/validation logic would be scattered across every calling service.

---

### 3. 202 Accepted Async Pattern

**Why used:** APNs, SMTP, and SMPP can each take seconds. Holding the producer's HTTP connection open until delivery is confirmed would exhaust connection pools and create a dependency chain. The 202 pattern breaks this: the producer gets a fast response, the system durably commits the message to Kafka, and delivery proceeds asynchronously.

**Key config:** The response body includes a `notification_id` that the producer can use to poll `GET /notifications/{id}` for status, or they can register a webhook for delivery callbacks.

**Without it:** A slow APNs response (even 2-3 seconds) at 174,000 RPS would require tens of thousands of open HTTP connections on the ingest side. Producers would timeout. System collapses under load.

---

### 4. Kafka Dual Topics (Transactional High-Priority vs. Marketing Low-Priority)

**Why used:** Transactional notifications (OTP, security alert, order confirmation) must be delivered in seconds. Marketing notifications can wait hours. If they share a single Kafka topic, a bulk marketing campaign of 50M emails fills the topic and starves OTP delivery — a critical failure. Two separate topics with separate consumer groups allow different concurrency, different TTLs, and different retry policies for each traffic class.

**Key config:**
- Transactional topic: TTL = 15 minutes (email OTP), 5 minutes (SMS OTP). If not consumed within TTL, discard — a 20-minute-old OTP is worse than no OTP.
- Marketing topic: TTL = 48 hours. Campaign consumers can pause and resume without message loss.
- Partitioned by `user_id % N`: guarantees per-user ordering (important for frequency cap enforcement and notification collapsing), enables horizontal scale proportional to partition count.
- RF=3, `min.insync.replicas=2`, `acks=all` on producers.

**Without it:** A Monday morning marketing blast clogs the queue and your users stop receiving two-factor authentication codes during the busiest shopping hour of the week.

---

### 5. Dispatcher Workers (APNs/FCM / SMTP / SMPP External Calls)

**Why used:** The dispatchers are the workers that actually cross the network boundary to external systems. They must be stateless (so they can be scaled and restarted freely), maintain persistent connection pools to external systems (HTTP/2 for APNs, SMPP TCP for carriers), handle backpressure gracefully, process response codes, and route errors back to the retry or invalidation pipeline.

**Key config:**
- Push: 100 dispatcher pods each maintain 10 persistent HTTP/2 connections to APNs = 1,000 connections × 1,000 concurrent streams = 1M in-flight APNs requests headroom.
- Email: SMTP relay pool with per-ISP connection limits (Gmail: ≤20 concurrent connections per IP). Each worker knows its sending IP and enforces per-ISP rate via Redis token bucket.
- SMS: SMPP `bind_transceiver` sessions (bidirectional — same session for send and receive DLR). Per-carrier `window_size` of 10-100 unacknowledged PDUs in flight.

**Without it:** Each notification would require a new TLS handshake to APNs, adding ~200ms overhead per message. At 833,000 dispatches/second, this is impossible.

---

### 6. Cassandra for Delivery Events (Append-Only, TTL, RF=3)

**Why used:** Delivery events are append-only time-series data at extremely high write throughput. Every sent/delivered/failed/opened event is a new row — nothing is updated. Cassandra's LSM-tree storage engine is optimized precisely for this pattern. TTL-based auto-expiry eliminates expensive DELETE operations. Native multi-DC replication provides availability and disaster recovery without application-layer work.

**Key config:**
- Partition key: `(notification_id, date_bucket)` for delivery events by notification (cap partition size per day). Separate table for user-centric view: `(user_id)` partition key.
- Consistency: `LOCAL_QUORUM` — strong within a region, tolerates one node failure, does not block on cross-DC latency.
- Compaction: `LeveledCompactionStrategy` for the delivery events table (high read:write ratio for analytics) or `TimeWindowCompactionStrategy` if writes are time-ordered (better for TTL cleanup).
- TTL: 30 days for push events, 90 days for email/SMS delivery events.

**Without it:** Using PostgreSQL for 4.8B delivery events per day would require extreme sharding, cannot auto-expire data without expensive DELETE jobs, and the single-writer bottleneck would be reached almost immediately.

---

### 7. PostgreSQL for Templates, Preferences, and Suppression Lists

**Why used:** Some data requires ACID guarantees — an opt-out update that half-commits is a compliance failure. Templates need foreign-key constraints and versioning with rollback. Suppression lists need a unique constraint on addresses to prevent double-suppression. PostgreSQL provides all of this with a mature ecosystem and predictable query behavior.

**Key config:**
- Email table: monthly range partitioning on `created_at` (3.3B rows/quarter makes a single table unusable; partition pruning enables fast date-range queries).
- User preferences: read replicas (5×) for read scaling; writes go to primary and synchronously replicate.
- Patroni for automated failover; HAProxy for connection routing.

**Without it:** Using Cassandra for suppression lists loses the UNIQUE constraint guarantee — you can end up with conflicting suppression records for the same address. Compliance risk.

---

### 8. Redis (Opt-Out Hot Cache + Frequency Cap Counters + Preference Cache)

**Why used:** Everything on the ingest hot path must be sub-millisecond. A PostgreSQL read takes 5-20ms. At 174,000 RPS, that adds up to thousands of concurrent DB connections and query load the database cannot sustain. Redis provides native data structures that map exactly to the use cases: hashes for user preferences, sorted sets for sliding-window frequency caps, strings with SETNX for idempotency and deduplication.

**Key config:**
- Cluster mode with 10 shards for the preference and frequency cap data, sharded by `user_id` hash.
- `appendfsync = everysec` (AOF persistence) — lose at most 1 second of writes on a crash, acceptable for frequency caps.
- No TTL on opt-out keys (permanent until the user opts back in). TTL on frequency cap sorted sets = window size + 60 seconds.
- Circuit breaker on the ingest path: if Redis latency > 10ms or error rate > 1%, fail-open on frequency caps, use local cache for opt-outs.

**Without it:** Every ingest request requires a PostgreSQL read for opt-out status. At 174,000 RPS, this is ~17 million PostgreSQL reads per minute. Even a well-tuned Postgres cluster cannot sustain this.

---

### 9. ClickHouse for OLAP Analytics

**Why used:** Delivery events accumulate at billions of rows per day. When a product manager asks "what was the click rate for this campaign, broken down by device OS and send hour?" — PostgreSQL would time out. ClickHouse is a columnar OLAP database that is 10-100x faster than PostgreSQL for aggregate queries on time-series data. It receives events from Kafka via Flink in near-real-time (60-second lag).

**Key config:** Kafka → Flink streaming job → ClickHouse `MergeTree` tables (or `ReplacingMergeTree` if event updates are needed). Flink handles deduplication and late-arrival events before inserting to ClickHouse. Retention: 90 days of granular events; roll up to hourly aggregates beyond 90 days.

**Without it:** Analytics queries for large campaigns would time out or require separate ETL jobs that run hours after the campaign completes.

---

### 10. Idempotency Keys + Deduplication

**Why used:** Kafka's at-least-once delivery, combined with dispatcher pod crashes at any point during external API calls, means the same notification can legitimately be attempted multiple times. Without idempotency, users receive duplicate OTPs, duplicate order confirmations, duplicate push notifications — degrading trust and user experience.

**Key config:**
- Client sends `Idempotency-Key` header (required for all calls).
- Ingest side: `SET ingest_dedup:{key} {notification_id} NX EX 86400` — if exists, return cached response.
- Dispatch side: `SET sent:{notification_id}:{device_id} 1 NX EX 86400` — if exists, skip external call, commit Kafka offset.
- PostgreSQL UNIQUE constraint on `idempotency_key` column as durable fallback.
- 24-hour TTL: covers all realistic retry windows without excessive memory usage.

**Without it:** A 5-minute Kubernetes pod restart during a high-traffic period results in hundreds of thousands of duplicate notifications being delivered. Users uninstall your app.

---

### Frequency Caps (Sliding Window per User per Channel)

**Why used:** Users who receive 50 marketing push notifications in a day will disable notifications or uninstall the app. Frequency caps protect the user experience and, for email/SMS, directly protect sender reputation (high unsubscribe rates signal spam to ISPs and carriers).

**Key config:** Redis sorted set per `(user_id, channel, window_type)`. Atomic Lua script: prune expired entries, count, compare to cap, conditionally add. Cap limits stored in PostgreSQL config table and cached in Ingest Service with a 5-minute TTL. Separate caps per channel (push: 3/hour, email: 10/day, SMS: 5/day) and configurable per notification category.

---

### Scheduled Delivery (scheduled_at Timestamp)

**Why used:** Marketing campaigns need to send at specific times (Friday 10am). Timezone-aware scheduling for push (9am local time per user) requires splitting one broadcast into ~40 timezone sub-broadcasts. OTPs never use scheduling (always immediate).

**Key config:** `scheduled_at` field in the ingest payload. A Scheduler service uses PostgreSQL advisory locks to prevent duplicate firing. For timezone-aware push broadcasts, the fan-out orchestrator buckets users by timezone offset and creates sub-broadcast jobs per timezone.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Push Notification Service

**What is unique:** Push is the only problem that requires a fan-out orchestrator for broadcasts (500M devices in 10 minutes), dual-protocol dispatch to two external systems with completely different authentication and payload schemas (APNs HTTP/2 with .p8 token auth vs. FCM HTTP v1 with OAuth2 service account), and a device token lifecycle with server-side invalidation on `BadDeviceToken` / `Unregistered` responses. The delivery confirmation model is also different: APNs confirms delivery at the network edge, not at the device; "opened" events come back via the mobile SDK sending a beacon, not from APNs.

**Different decisions:**
- Device token store in Cassandra (not PostgreSQL) because 500M tokens × 300B = 150 GB with high write throughput from token rotation events. PRIMARY KEY = `(user_id, device_id)` for per-user lookups; reverse lookup table for invalidation.
- Kafka partitioned by `user_id` (not `notification_id`) specifically to enable per-user ordering and frequency cap enforcement without distributed locks — all messages for a user hit the same partition and thus the same consumer pod.
- APNs authentication uses .p8 token-based auth (JWT, valid 1 hour) rather than certificate-based auth — certificates expire annually and require manual rotation; JWTs are generated programmatically and rotated every 55 minutes automatically.

**Two-sentence differentiator:** Push is unique because it has two fundamentally different external dispatch protocols (APNs and FCM) that must be abstracted behind a unified interface, and because broadcast sends to 500M devices require a sharded fan-out orchestrator with checkpointed progress and scatter-gather parallelism — neither of which is needed for email or SMS at their respective scales.

---

### Email Delivery System

**What is unique:** Email is the only problem where the external infrastructure (ISPs like Gmail, Outlook, Yahoo) can permanently blacklist you if you violate their policies. This creates an entire class of operational concerns absent from push and SMS: DKIM/SPF/DMARC authentication pipeline, IP warm-up management (8-10 weeks per new IP), ISP-specific per-IP per-day rate limits, bounce classification (hard vs. soft, RFC 3464 DSN parsing), ISP feedback loop (FBL) complaint handling (RFC 5965 ARF parsing), and real-time spam rate monitoring via Google Postmaster Tools API. The deliverability management stack is as complex as the send path itself.

**Different decisions:**
- Separate IP pools for transactional and marketing email. A marketing campaign spam complaint spike cannot affect the IP used to send OTPs or order confirmations. IP pool separation is enforced at the Ingest Service level based on `email_type`, not at the SMTP relay level.
- SMTP vs. vendor API: at 36.3M emails/day, self-hosted Postfix relay costs ~$149K/year; SES/SendGrid costs ~$1.33M/year — a 9x difference. Hybrid approach: in-house Postfix as primary, SES/SendGrid as fallback triggered by blocklist detection or relay failure.
- 15-minute Kafka TTL for the transactional topic: a 20-minute-old OTP email is harmful (expired code, user confuses), not just useless. Automatic discard prevents delayed delivery of stale security messages — a conscious trade-off of delivery guarantee for user experience.

**Two-sentence differentiator:** Email is unique because ISPs are external gatekeepers with blacklist power, making IP reputation management, DKIM/SPF/DMARC authentication, bounce/complaint processing, and warm-up management first-class architectural concerns — not afterthoughts. The deliverability stack (FBL handler, bounce processor, blocklist monitor, IP warming scheduler) is as complex as the send pipeline itself, and it exists solely because of constraints imposed by parties outside your control.

---

### SMS Gateway

**What is unique:** SMS is the only problem that uses SMPP v3.4 — a persistent binary TCP protocol dating from the 1990s — for carrier connectivity. The protocol is stateful (persistent sessions with sequence numbers, acknowledgment windows, keepalive `enquire_link` PDUs) in contrast to the stateless HTTP/2 and SMTP connections used by push and email. International routing requires per-message carrier selection by MCC/MNC (mobile country and network code), with a continuously updated quality score feedback loop from delivery receipts. Character encoding detection per message (GSM-7 = 160 chars, UCS-2 = 70 chars) with UDH multipart splitting is mandatory for any non-trivial message. Two-way MO handling for STOP/UNSTOP keywords is legally required in multiple jurisdictions.

**Different decisions:**
- SMPP `bind_transceiver` (bidirectional session) rather than separate bind_transmitter and bind_receiver: one session handles both outbound submit_sm and inbound deliver_sm (DLRs + MO messages). This halves the number of connections and simplifies session state management.
- Carrier routing by `(MCC, MNC, message_type)` with adaptive quality scoring: static routing tables cannot respond to carrier outages or degradation. DLR-derived quality scores updated every 5 minutes via a Flink streaming job automatically route around failing carriers without manual intervention.
- Per-sender opt-out (`opt_out:{phone}:{sender}`) separate from global opt-out (`opt_out:{phone}:global`): a STOP reply to short code 12345 should not block messages from a different company using short code 67890. This granularity is required by CTIA guidelines.

**Two-sentence differentiator:** SMS is unique because it uses SMPP — a binary persistent-session protocol with sliding acknowledgment windows and bidirectional DLR receipt — for carrier connectivity, and because every single message requires per-message encoding detection, optional multipart UDH splitting, and adaptive carrier routing by MCC/MNC with live quality scoring — none of which exists in push or email delivery.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (answer in 2-4 sentences each)

**Q: Why do you return 202 Accepted instead of 200 OK from the notification ingest API?**

**202 Accepted** means "I have accepted your request and I will process it, but processing is not yet complete." The notification system never has delivery confirmation at ingest time — the actual delivery through APNs or SMTP can take seconds to minutes. Returning 200 would imply the notification was delivered, which is false. Returning 202 with a `notification_id` lets the producer poll status or receive a webhook when delivery is confirmed.

**Q: What is the difference between hard bounce and soft bounce in email?**

A **hard bounce** is a permanent delivery failure — the email address does not exist, the domain does not exist, or the recipient's server has permanently rejected the address (SMTP 5xx codes, enhanced code 5.1.1). The address must be immediately and permanently suppressed. A **soft bounce** is a temporary failure — the mailbox is full, the server is temporarily unavailable (SMTP 4xx codes). The system retries soft bounces; after 3-5 consecutive soft bounces, it escalates to a temporary suppression (90 days). Confusing the two either loses you valid recipients (treating soft as hard) or gets you blocklisted (treating hard as soft and continuing to send).

**Q: Why do you use Kafka instead of a simpler queue like SQS?**

For notification systems specifically, Kafka provides three things SQS does not: **replay capability** (7-day retention allows re-processing on consumer crash without losing messages), **consumer group parallelism** (multiple consumer pods can share a topic without message duplication), and **partition-level ordering** (partitioning by `user_id` ensures all messages for a user are processed in order, enabling per-user frequency cap enforcement without distributed locks). SQS is fine for simpler message-passing use cases but doesn't provide the throughput or ordering guarantees needed at 174,000 RPS with per-user semantics.

**Q: What is DKIM and why does email require it?**

**DKIM** (DomainKeys Identified Mail) is an email authentication standard where the sending server cryptographically signs each outgoing email using a private key, and the recipient's mail server verifies the signature using the public key published in DNS. Without DKIM, anyone can forge your `From:` address and send spam that appears to come from your domain. ISPs like Gmail require DKIM alignment for bulk senders (Google's 2024 requirements) — failing DKIM means your emails go to spam or are rejected. DKIM works alongside SPF (which authorizes sending IPs) and DMARC (which tells ISPs what to do when SPF/DKIM fail).

**Q: What is an SMPP window size and why does it matter?**

**Window size** (or transmit window) in SMPP v3.4 is the number of unacknowledged `submit_sm` PDUs the client can have in flight simultaneously before it must wait for responses. A window size of 1 means send one message, wait for `submit_sm_resp`, then send the next — very slow. A window size of 100 means 100 messages can be in-flight simultaneously, achieving 100× higher throughput on a single SMPP session. At 5,750 SMPP PDUs/second peak, you need ~58 SMPP sessions at window_size=100, or fewer sessions at larger window sizes. Window size is negotiated with the carrier and constrained by their capacity.

**Q: What happens when a device token becomes invalid?**

APNs returns HTTP 410 with reason `Unregistered` or HTTP 400 with reason `BadDeviceToken`. The dispatcher records this response and immediately marks the token as `is_active = false` in Cassandra via a token invalidation pipeline. Future sends for that user skip this token. A reverse lookup table (`device_token_reverse`) maps the old token to `(user_id, device_id)` so the app's next launch — when it registers a new token — updates the same device record rather than creating a duplicate.

**Q: How does the system prevent sending to a user who just opted out?**

On opt-out, the Ingest Service writes to PostgreSQL first (source of truth), then **synchronously** updates Redis before returning 200 to the user's client. Since all subsequent ingest requests check Redis as the first step, any notification submitted after the opt-out response is returned will see the opt-out in Redis and be suppressed. The critical word is "synchronous" — the Redis write happens before the API response, not after, closing the window where a concurrent in-flight notification could slip through.

---

### Tier 2 — Deep Dive Questions (focus on why + trade-offs)

**Q: How do you guarantee at-least-once delivery without sending duplicate notifications?**

This is a **two-layer idempotency** problem. Kafka gives at-least-once: if a dispatcher pod crashes after calling APNs but before committing its Kafka offset, the message is re-consumed. Without intervention, the user gets two notifications.

Layer 1 (client idempotency): `Idempotency-Key` header → `SETNX ingest_dedup:{key}` in Redis with 24h TTL. Duplicate API calls are short-circuited before Kafka publish.

Layer 2 (dispatch idempotency): Before every external call, `SETNX sent:{notification_id}:{device_id}` in Redis. If the key exists, skip the call, commit the Kafka offset. This guard runs even when Kafka re-delivers after a crash.

Trade-offs: If Redis is completely down, Layer 2 fails and re-sends are possible — acceptable rare failure mode rather than blocking all sends. The 24h TTL means a message retried after 24 hours could be re-sent — OTPs have a 5-15 min TTL so they're discarded before the dedup window anyway. For marketing notifications, a duplicate after 24 hours is annoying but not catastrophic.

**Q: Walk me through how a broadcast to 500M devices actually works end-to-end.**

A `POST /v1/notifications` arrives with `target.type = broadcast`. The Ingest Service validates auth, publishes a single broadcast job record to the `notifications.broadcast` Kafka topic, and returns 202.

The Fan-out Orchestrator reads this job. It creates 1,000 shard tasks covering the full `user_id` hash space (0x0000 to 0xFFFF split into 1,000 equal ranges) and publishes them to `notifications.fanout.shards` in Kafka.

1,000 Fan-out Worker pods (auto-scaled, one per shard) each claim a shard. Each worker pages through `device_tokens` in Cassandra using `TOKEN(user_id)` range pagination — pages of 1,000 devices at a time. For each batch, it checks the Redis bitset of opted-out users (62.5 MB for 500M users — loaded into worker memory at shard start). For non-opted-out users, it publishes per-device dispatch tasks to the unicast Kafka topic's low-priority partitions.

After every 10,000 devices, the worker checkpoints `last_paged_token` to Cassandra. On crash, the shard is re-consumed and processing resumes from the checkpoint.

100 dispatcher pods drain the unicast topic — broadcast tasks have lower Kafka thread priority than unicast tasks, ensuring a concurrent OTP delivery is not delayed.

At 1,000 workers × 833,000 devices processed per second across all workers, the broadcast completes in under 600 seconds. The orchestrator marks the job complete when all 1,000 shard statuses are `completed`.

Trade-off worth calling out: If a shard worker crashes after checkpointing but before all messages for that checkpoint batch are published to Kafka, some messages are re-processed from the checkpoint — the idempotency key `broadcast_id+device_id` on each per-device task prevents double dispatch.

**Q: How do you handle per-user frequency caps at 174,000 ingest RPS without a bottleneck?**

The frequency cap check is a Redis operation: one atomic Lua script call per notification per user. The Lua script runs ZREMRANGEBYSCORE (prune expired entries), ZCARD (count), compare to cap, ZADD (add if not at cap). Single round-trip, ~0.5ms per call.

At 174,000 RPS across 10 Redis shards (sharded by `user_id % 10`), each shard handles ~17,400 RPS. A well-configured Redis node handles 100,000+ ops/second — comfortable headroom.

The key insight is that the Lua script is atomic **per Redis shard** (not per cluster), but since each key is per-user, and users hash to a single shard, the atomicity guarantee is exactly what we need. No cross-shard coordination required.

If Redis latency spikes above 10ms or error rate exceeds 1%, the circuit breaker switches the ingest service to fail-open mode for frequency caps (allow through) while continuing to enforce opt-outs from a local in-memory LRU cache. The reasoning: slightly over-delivering is less harmful than blocking all notifications.

**Q: Why do you need separate IP pools for transactional and marketing email?**

IP reputation at ISPs (Gmail, Outlook, Yahoo) is per IP address and per domain. If a marketing campaign generates 0.15% spam complaints (above Gmail's 0.1% threshold), the sending IP gets flagged and subsequent emails go to spam or are rejected. If transactional and marketing emails share IPs, a bad marketing campaign makes your order confirmation and password reset emails stop arriving in inboxes — a catastrophic user experience failure.

Separate IP pools mean the marketing IP can be blocklisted without touching the transactional IP. The separation is enforced at the Ingest Service level (routing based on `email_type`), not just by configuration — a misconfigured template cannot accidentally use the wrong pool.

Additionally, transactional senders should use dedicated IPs (not shared with other customers) to prevent another sender's reputation from harming yours. Marketing can use shared IPs at lower volume but should migrate to dedicated IPs as volume grows.

**Q: What is the SMPP window size and how do you tune it for OTP throughput?**

Window size is the number of unacknowledged PDUs in flight per SMPP session. At `window_size=1`, throughput per session = `1 / (round_trip_latency)`. At 20ms RTT to the carrier, that's 50 PDUs/second per session. At `window_size=100`, that's 5,000 PDUs/second per session.

For OTP (5,000 RPS peak requirement): target `window_size=100` across ~6 sessions per carrier, distributed across ~3 dispatcher pods. Each session handles ~833 PDUs/second = `window_size / RTT = 100 / 0.12s = 833`. Check: 6 sessions × 833 = 4,998 RPS — just meets requirement with no headroom.

Real tuning: set `window_size` slightly below the carrier's stated limit (leave 10% buffer). Monitor for `ESME_RTHROTTLED` responses — this means you've exceeded the carrier's queue depth. Reduce `window_size` or add sessions. Monitor for `enquire_link` timeouts — this means sessions are dropping; reduce the enquire_link interval.

**Q: How do you handle the email case where a user's email address generates a bounce after you've already sent them 1,000 emails successfully?**

Classify the bounce first. A `550 5.1.1 User unknown` after 1,000 successful deliveries suggests the account was deleted or deactivated. This is a hard bounce — suppress immediately, permanently.

But ask: is this truly a permanent failure or a temporary ISP issue? Some ISPs temporarily return 5xx during SMTP issues. Mitigation: if the bounce is the first failure for an address with a successful delivery history (>10 prior successful deliveries), treat the first hard bounce as a soft bounce — retry once after 1 hour. If the second attempt also hard-bounces, suppress permanently. This small adjustment prevents premature suppression of valuable addresses due to transient ISP 5xx storms.

Track `consecutive_hard_bounces` per address. The standard promotion-to-permanent-suppress policy: 1 hard bounce for a previously inactive address → permanent suppress; 2 consecutive hard bounces for a historically active address → permanent suppress.

**Q: How does the STOP keyword handling work and what are the edge cases?**

The happy path: recipient replies STOP → SMPP `deliver_sm` MO message arrives → Inbound SMS Handler detects keyword → Redis `SETNX opt_out:{hash(to)}:{hash(from)}` → PostgreSQL async write → auto-reply "You have been unsubscribed" → any subsequent send to that `(to, from)` pair is suppressed in Redis < 1ms.

Edge cases worth calling out:

"STOP" variants: CTIA guidelines require honoring STOP, STOPALL, UNSUBSCRIBE, CANCEL, END, QUIT (and their regional variants in French, Spanish, etc.). The keyword list is stored in PostgreSQL and cached in the Inbound SMS Handler; the match is case-insensitive after normalization.

"STOP to short code" semantics: STOP to short code 12345 should suppress messages from any sender using that short code on your platform, not just one specific sender. This requires the opt-out key to be `opt_out:{to}:{from_short_code}` rather than `opt_out:{to}:{producer_id}`.

Re-consent via UNSTOP/START: delete the opt-out key from Redis and mark `opt_in_at` in PostgreSQL. But the TCPA requires a new express written consent for marketing SMS after an opt-out — the UNSTOP reply alone may not constitute consent. This is a legal nuance the system should surface to the marketing team rather than auto-re-enable.

Carrier-level STOP: some carriers enforce STOP independently. If we also enforce it, opt-out state can diverge. Resolution: always treat our application-level opt-out as the authoritative source; do not rely on carrier enforcement as the sole mechanism.

---

### Tier 3 — Staff+ Stress Tests (reason out loud, no single right answer)

**Q: Your fan-out orchestrator has been running for 8 minutes on a 500M device broadcast. The Cassandra cluster in us-east-1 loses two nodes simultaneously, and 3 fan-out shards were mid-read on those nodes. What happens and how does your system respond?**

Reason out loud: Cassandra LOCAL_QUORUM requires 2-of-3 nodes in the local DC. With 2 nodes down out of 3 in the shard's replica set, quorum is lost for reads on those partition ranges. The fan-out workers on those 3 shards will receive a `NoHostAvailableException` or read timeout.

The fan-out worker should implement a retry with exponential backoff for Cassandra read failures (3 retries, 1s/2s/4s). With a 2-node failure, these retries will all fail. After max retries, the shard task is re-published to Kafka with a delay (not immediately — avoid thundering herd on a degraded cluster).

Meanwhile: the other 997 shards continue normally. The broadcast is 99.7% complete in 8 minutes. The 3 failed shards' ~1.5M devices will not receive the broadcast in the initial window.

Cassandra auto-repair kicks in over minutes-to-hours depending on cluster configuration. When the nodes recover (or when Cassandra repairs via `nodetool repair`), reads from those partition ranges succeed again. The 3 shard tasks are re-consumed from Kafka (they had the DLR timeout) and the remaining ~1.5M devices get the notification — delayed but delivered.

The broadcast job status reports "completed_with_failures" with a count of pending devices. The Orchestrator can expose this to the producer via the status API so they know what happened.

Senior insight: the right answer is not "use eventual consistency so it just works." The right answer calls out the quorum loss precisely, explains the cascade, acknowledges the partial delivery outcome, and describes the reconciliation path. Also worth mentioning: could pre-shard reads across multiple Cassandra regions avoid this? Yes — with EACH_QUORUM consistency you read from both DCs, but this doubles read latency. Trade-off to state explicitly.

**Q: You're running a joint push notification + email campaign. Kafka transactional topic consumer lag is spiking to 60 seconds for the first time. APNs is healthy. What is your diagnostic and resolution playbook?**

Reason out loud: consumer lag spiking means consumers are reading slower than producers are publishing. First, isolate the layer.

Is the Kafka producer (Ingest Service) overloading the topic? Check ingest RPS — if it's 10x normal, something upstream is misbehaving (a bug publishing the same notification in a loop, or a campaign that accidentally targeted all users via the transactional topic instead of the marketing topic). Resolution: per-producer rate limiting at the API Gateway should have caught this; if it did not, temporarily reduce the offending producer's rate limit.

Are Dispatcher pods slow? Check APNs response latency (claimed healthy, but double-check percentiles — P99 might be 10 seconds even if average is fine). Check CPU and memory on dispatcher pods. Check for GC pauses if using JVM. Check the Redis dedup check latency — if Redis is slow, every dispatch is delayed.

Is the consumer group rebalancing? A pod restart causes a consumer group rebalance, which pauses consumption for 5-30 seconds (with cooperative rebalancing in Kafka 2.4+, this is shorter). If pods are crash-looping, rebalancing is continuous.

Resolution path: first, scale up dispatcher pods via HPA (or manually trigger an HPA scale-up). Second, if that doesn't work, check if the unicast topic is mixed with broadcast fan-out messages that should be on the broadcast topic (routing bug). Third, alert and escalate if lag continues to grow past 120 seconds (SLO breach for transactional notifications).

Senior insight: the framing "60 second consumer lag" is a leading indicator, not the root cause. The job is to traverse the dependency graph (ingest rate → Kafka throughput → consumer throughput → external call latency) and find which link is the bottleneck. State this traversal explicitly in the interview.

**Q: A product manager wants to add a feature: "send each user their notification at exactly 9am in their local timezone." Walk through the design end-to-end, including all the edge cases.**

Reason out loud: straightforward for small-scale; complex at 500M device scale.

The core approach: bucket users by UTC offset. Group users into ~40 timezone buckets (UTC-12 to UTC+14). For each bucket, calculate the UTC timestamp at which 9am local occurs. Create a sub-broadcast job for each bucket scheduled at that UTC timestamp.

Where this gets interesting:

**Daylight saving transitions:** A user in "America/New_York" (UTC-5 in winter, UTC-4 in summer) is not reliably at UTC-5. You must store the user's IANA timezone name, not their UTC offset. At scheduling time, use a proper timezone library (Java `ZoneId`, Python `pytz`) to compute the exact UTC timestamp for their current offset. This prevents off-by-one-hour sends on DST transition days.

**Users in rare timezones:** Some timezone buckets (e.g., UTC+5:30 for India, UTC+5:45 for Nepal) are non-standard offsets. These are real IANA zones and must be handled correctly. Some users may have no timezone stored — default to UTC or the country-level timezone derived from phone number (for SMS) or device locale (for push).

**40 sub-broadcasts = 40x the fan-out Orchestrator load:** Each sub-broadcast triggers a full fan-out for that timezone's user segment. At 500M users across 40 timezones, each sub-broadcast averages 12.5M users. The fan-out orchestrator handles this — it's just 40 smaller broadcasts instead of one large one. But they're staggered over 24 hours, so resource usage is not 40× peak simultaneously.

**What if a timezone bucket is empty?** Skip it — no job created.

**What if a user changes their timezone between scheduling and delivery?** The timezone was captured at job creation time. The user receives the notification at the time that was correct when scheduled. This is a known approximation acceptable for marketing sends. For high-precision sends, re-read the timezone at fan-out time (not at schedule creation time).

**What about users who create their account in the 23 hours before the scheduled send?** They will not be in the segment snapshot taken at schedule time (if segments are snapshotted at schedule creation). Solution: use dynamic segments — the fan-out worker queries "all active users in timezone bucket X" at shard execution time, not at schedule creation time. This picks up new users but may include users who signed up between schedule creation and send.

Senior insight: the interesting answer is not just "bucket by timezone." It's the cascade of edge cases — DST transitions, IANA vs. UTC-offset storage, dynamic vs. snapshot segments, and the fan-out infrastructure load profile. Interviewers are checking whether you go past the happy path.

**Q: Your email campaign complaint rate just crossed 0.12% — above Gmail's 0.10% threshold. You're in the middle of sending a 50M recipient campaign. What do you do in the next 30 minutes?**

Reason out loud: first, don't panic — pausing is the correct immediate action, but understand what you're pausing before you act.

**Immediate (T+0):** Pause the campaign's Kafka consumer group (stop reading from `email-marketing` topic for this campaign). New sends from this campaign stop within seconds. Campaign status in PostgreSQL → `paused`.

**T+0 to T+5 min (diagnosis):** Check complaint rate breakdown in ClickHouse: is the complaint spike from Gmail specifically, or also Yahoo/Outlook? Is it from one sending IP or all IPs? Is the complaint rate concentrated on one segment or distributed across all recipients? The answers narrow the cause.

**Likely root causes:**
- Segment quality issue: the segment included inactive addresses or non-consented addresses. Check age of the oldest email address in the segment — anything > 18 months with no engagement is a risk.
- Content issue: the email content triggered aggressive spam filter heuristics. Check link domains, subject line patterns.
- Send volume spike: a sudden increase in volume from an IP that was not fully warmed. Check `daily_count:{ip}:{date}` in Redis vs. warming schedule.
- Suppression list gap: hard-bounce addresses not properly suppressed. Check suppression coverage.

**T+5 to T+15 min:** If root cause is segment quality → re-segment, removing addresses with no engagement in 12 months, and re-validate consent. If root cause is a specific IP → remove that IP from the active pool, reroute to a healthier IP or vendor fallback. If content → revise and re-test.

**T+15 to T+30 min:** Resume campaign at 50% of previous send rate (reduce TPS to give reputation time to recover). Monitor complaint rate in ClickHouse (60-second lag). If rate drops below 0.08%, gradually increase rate. If rate stays elevated, pause again and escalate to the deliverability team.

**What NOT to do:** Do not simply reduce the send rate without addressing the root cause — a lower rate with the same bad content/segment will eventually produce the same complaint rate. Do not ignore it — Gmail will temporarily disable inbox delivery for your sending domain if the rate stays elevated for more than a few hours.

Senior insight: show that you understand the system has feedback loops (send rate → complaint rate → domain reputation → deliverability), not just individual components. The correct response is diagnose → address root cause → resume cautiously with monitoring. The wrong response is just "pause and resume."

---

## STEP 7 — MNEMONICS

### The Acronym: I-K-D-F-S

Every notification system — push, email, SMS — can be remembered with this five-letter sequence:

**I** — **Ingest with 202**: Accept fast, reject early (suppression, validation). Never block on external delivery.

**K** — **Kafka dual topics**: Transactional (short TTL, high priority) and Marketing (long TTL, throttled). Separate topics, separate SLOs.

**D** — **Dispatch idempotently**: SETNX before every external call. Two layers: ingest dedup + dispatch dedup.

**F** — **Fan-out for scale**: Broadcasts need sharded parallel workers with checkpoint-and-resume. Per-user unicast partitioned by user_id.

**S** — **Suppress everywhere**: Redis for hot-path opt-out (< 1ms), PostgreSQL for durable truth. Frequency cap is also suppression. DKIM/STOP/bounce handling are channel-specific suppression forms.

For channel-specific differentiators, remember: **Push = Fan-out + Token lifecycle**, **Email = Reputation + Bounce**, **SMS = SMPP + Routing**.

---

### Opening One-Liner

When the interviewer says "design a notification system," start with:

> "Notification systems have a deceptively simple request — send a message — but the interesting engineering is in three places: handling the impedance mismatch between your internal SLA and the external delivery system's latency, guaranteeing exactly-once delivery through an at-least-once queue with idempotent consumers, and enforcing suppression and compliance guarantees at ingestion throughput. Let me clarify which channel we're designing for, the scale, and whether broadcasts are in scope — those three answers determine the entire architecture."

This opening does three things: signals you understand the problem deeply, identifies the key clarifying questions, and frames the design around first principles rather than immediately naming components.

---

## STEP 8 — CRITIQUE

### What the Source Material Covers Well

**Exceptional depth** on APNs/FCM protocol specifics: HTTP/2 multiplexing, token-based vs. certificate-based auth, stream counts per connection, specific error codes (410 Gone / Unregistered, 400 BadDeviceToken, 429 TooManyRequests) with specific retry behavior per code. This is exactly what interviewers probe at senior levels.

**Strong on SMTP deliverability stack** (DKIM/SPF/DMARC implementation detail, IP warm-up schedule with actual numbers, FBL processing per ISP, bounce classification by RFC standard). Few candidates know this at this depth.

**SMPP v3.4 protocol mechanics** (window sizing, bind_transceiver vs. transmitter/receiver, UDH structure with actual hex values, DLR correlation via `message_id`) — extremely rare knowledge that signals genuine SMS delivery experience.

**Quantitative capacity estimates** tied to architecture decisions (833K RPS → 100 dispatcher pods × 10 connections × 1,000 streams). The source material models this correctly throughout.

**Data model specifics** — the Cassandra partition key choices, composite primary keys with date buckets, TTL configurations — show working knowledge rather than hand-waving.

---

### What Is Missing, Shallow, or Imprecise

**Multi-region active-active design is underspecified.** The source material mentions it but does not detail the conflict resolution strategy for preference updates that arrive at two regions simultaneously. If a user opts out in us-east-1 and a notification is dispatched from eu-west-1 before the Cassandra async cross-DC replication completes (~500ms), the notification goes through. For GDPR opt-out compliance, this window matters. A stronger answer: for opt-out writes, use `EACH_QUORUM` consistency level (requires acknowledgment from both DCs) at the cost of higher write latency.

**Webhook delivery to producers is mentioned but not designed.** Multiple places reference "delivery webhook callbacks" but there's no design for the webhook delivery system itself: retry policy, exponential backoff, DLQ for dead-letter webhooks, signature (HMAC) for security, idempotency of webhook receipt on the producer side. This is a common follow-up question.

**Cost optimization for push at scale is not discussed.** At 833K APNs/FCM calls per second, the cost of HTTP/2 connection management, data egress, and potential APNs overages is significant. A senior answer would mention batching strategies (FCM legacy had batch API; FCM v1 does not; APNs is per-message but HTTP/2 multiplexing amortizes cost).

**Notification priority on the device** (APNs `apns-priority` = 10 for immediate, 5 for power-saving delivery) is not mentioned. Sending all notifications at priority 10 drains device batteries and can trigger APNs throttling on devices in low-power mode. The system should map `notification.priority` to `apns-priority` appropriately.

**Email threading / conversation identification** (RFC 2822 `In-Reply-To` / `References` headers) is not covered. Some transactional email use cases (e.g., customer support ticket threads) require correct threading so email clients group messages. While this is "out of scope" in the source, interviewers for customer-facing email products will ask about it.

**SMS OTP security** — the source doesn't discuss OTP brute-force protection (e.g., rate limiting OTP verification attempts independent of SMS sending rate) or OTP code generation best practices (TOTP vs. random N-digit codes). This is a common adjacent question.

**BIMI for email** — the source mentions BIMI briefly in a Q&A answer but does not explain it as a first-class architectural component: BIMI requires `p=reject` DMARC policy, a VMC certificate, and an SVG logo file served from a specific URL. It results in brand logos being shown next to emails in Gmail and Apple Mail, improving trust and deliverability. An interviewer at a consumer-facing company may ask about this.

---

### Senior Probes to Prepare For

These are questions that separate Senior from Staff+ candidates:

1. "Your Redis cluster goes down completely during peak traffic. Walk me through exactly what happens in each of the three systems." — Tests: failure isolation, graceful degradation paths, which behaviors fail-open vs. fail-closed and why.

2. "You've just discovered that one of your ingest service pods has been silently dropping ~0.5% of messages for 6 hours due to a bug in the Kafka producer error handling. How do you detect this, quantify the impact, and recover?" — Tests: observability design, Kafka offset forensics, data reconciliation under partial loss.

3. "A mobile carrier changes their SMPP endpoint without notice, and your SMPP sessions are silently failing (sessions connect but submit_sm_resp never arrives). How does your system detect this vs. just slow delivery?" — Tests: session health monitoring, DLR timeout vs. session timeout distinction, carrier SLA monitoring.

4. "Regulatory requirement: all SMS must be delivered or definitively failed within 48 hours for compliance reporting. How do you implement 'definitively failed' given that some carriers have 70% DLR rates?" — Tests: understanding that DLR receipt is not guaranteed, TTL-based failure promotion, the `unknown` status and when to promote it to `failed`.

5. "You want to add a new notification channel: WhatsApp Business API. How much of the existing architecture can you reuse, and what requires new design?" — Tests: whether you understand the abstract pattern vs. the specific channel. Answer: the ingest service, 202 pattern, Kafka, Cassandra events, ClickHouse analytics, preference layer, and idempotency layer are all reusable. New: WhatsApp template pre-approval process (different from email/SMS templates — Meta requires template approval before use), per-user 24-hour session window (WhatsApp Business API requires a customer to initiate within 24 hours or only approved templates can be used), and the HTTP-based Meta WhatsApp Cloud API dispatch layer.

---

### Common Traps Candidates Fall Into

**Trap 1: Synchronous external calls.** Candidates propose calling APNs inline and returning 200 when APNs confirms. This fails immediately under load — APNs responses take 1-5 seconds, and at 174K RPS you'd need 174,000-870,000 concurrent HTTP connections on the ingest layer. Always async with 202.

**Trap 2: Single Kafka topic for all notifications.** A marketing blast fills the topic and starves OTP delivery. Always ask "do we need priority separation?" and answer yes with dual topics.

**Trap 3: Using PostgreSQL for delivery events.** Candidates comfortable with Postgres reach for it by default. At 4.8 billion events per day, PostgreSQL's single-writer bottleneck and expensive DELETE operations for TTL-based expiry make it wrong for this use case. The pattern is: high-throughput append-only → Cassandra.

**Trap 4: Treating opt-out as eventually consistent.** "The preference will propagate within a minute" is not acceptable for GDPR. The opt-out must be effective within seconds. The write-through Redis pattern (write to both Postgres and Redis synchronously before returning 200 to the user) is the correct answer.

**Trap 5: Not knowing APNs and FCM are separate systems.** A common mistake is to propose "a single mobile push dispatcher." APNs and FCM have different auth (JWT vs. OAuth2), different API endpoints, different payload formats, different error codes, and different rate limiting behaviors. They need separate dispatcher pools.

**Trap 6: Confusing fan-out with just "having more workers."** Broadcast fan-out is not just "add more dispatcher pods." It requires a sharded work distribution layer because the dispatch target (device tokens) lives in a database that must be scanned, and that scan must be parallelized, checkpointed, and isolated from unicast traffic. Candidates who miss the Fan-out Orchestrator concept will design a system that takes hours to complete a broadcast.

**Trap 7: Ignoring SMPP in SMS design.** Candidates who only know HTTP-based SMS APIs (Twilio REST API) will design an HTTP adapter without understanding that the most efficient carrier connectivity is SMPP — a persistent binary TCP protocol. At 5,000 RPS peak, the SMPP windowing model is essential to achieve the throughput. HTTP request-per-message would require 5,000 concurrent HTTP connections per second, far less efficient.

---

*End of Pattern 11 Interview Guide — Push Notifications, Email Delivery, SMS Gateway*
