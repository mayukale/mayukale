# Common Patterns — Notifications (11_notifications)

## Common Components

### API Gateway (TLS + OAuth2 + Per-Producer Rate Limiting)
- All three use an API Gateway as the entry point for notification requests from internal producers
- Enforces TLS 1.2+, OAuth2 client credentials, and per-producer rate limits
- In push_notification_service: rate limits per producer, validates JWT, routes to Ingest Service
- In email_delivery: rate limits per producer; transactional vs. marketing classification at ingress
- In sms_gateway: rate limits per producer; rejects malformed phone numbers before ingest

### Ingest Service (Validate → Suppression Check → Template Render → Kafka Publish)
- All three implement a stateless Ingest Service that performs the same four steps before any external dispatch
- Step 1: validate schema (required fields, phone/email format)
- Step 2: check suppression list in Redis (opt-out cache, hard bounce cache) — reject if suppressed
- Step 3: render notification body from template (Handlebars/Mustache + data payload)
- Step 4: publish to Kafka topic with idempotency key; return 202 Accepted immediately
- In push_notification_service: validates device token format; checks opt-out Set in Redis; publishes to `push-transactional` or `push-marketing` Kafka topic
- In email_delivery: validates email address + MX DNS; checks suppression table in Redis; renders MIME body; publishes to `email-transactional` (15-min TTL) or `email-marketing` (48h TTL) topic
- In sms_gateway: validates E.164 phone; checks opt-out key in Redis; detects character encoding (GSM-7 vs UCS-2); publishes to `sms-transactional` or `sms-marketing` topic

### 202 Accepted Async Pattern
- All three return 202 Accepted immediately after Kafka publish — never block the producer on external delivery
- Delivery status available via polling GET /notifications/{id}/status or webhook callback
- Decouples producer SLA from channel-specific delivery latency (APNs/FCM, ISP SMTP, carrier SMPP)

### Kafka Dual Topics (Transactional High-Priority vs. Marketing Low-Priority)
- All three use separate Kafka topics for transactional (high-priority, short TTL) vs. marketing (lower-priority, longer TTL) notifications
- Separate consumer groups allow different concurrency, batch sizes, and retry policies per traffic class
- In push_notification_service: `push-transactional` + `push-marketing`; 1,000 partitions by user_id hash
- In email_delivery: `email-transactional` (15-min TTL, high-priority consumers) + `email-marketing` (48h TTL, bulk consumers)
- In sms_gateway: `sms-transactional` (OTP/alerts) + `sms-marketing` (promotional); transactional consumers have higher concurrency

### Dispatcher Workers (APNs/FCM / SMTP / SMPP External Calls)
- All three use a pool of Dispatcher workers that consume from Kafka, batch where appropriate, and call external delivery APIs
- Workers are stateless; Kafka consumer group provides horizontal scaling and rebalance on failure
- In push_notification_service: 500 Dispatcher pods; batch to APNs HTTP/2 (up to 1,000 notifications/connection) and FCM HTTP v1; 833K RPS peak
- In email_delivery: SMTP relay workers via SendGrid/SES or self-hosted MTAs; per-ISP connection pools; per-ISP throttle limits
- In sms_gateway: SMPP v3.4 sessions to Twilio/Sinch/Vonage; bind_transceiver for bidirectional; per-carrier transmit window

### Cassandra for Delivery Events (Append-Only, TTL, RF=3)
- All three write delivery events (sent/delivered/failed/opened) to Cassandra — high-write, time-ordered, never updated
- RF=3, LOCAL_QUORUM writes, multi-region replication for cross-region availability
- In push_notification_service: `notifications` table partitioned by user_id, clustering by sent_at DESC; TTL 30-day hot (45TB); LeveledCompactionStrategy
- In email_delivery: `email_events` partitioned by recipient_id, clustering by (sent_at DESC, event_id); TTL 90 days; tracks: sent/bounced/opened/clicked/unsubscribed
- In sms_gateway: `sms_events` partitioned by phone_number, clustering by sent_at DESC; TTL 90 days; tracks sent/delivered/failed/DLR received

### PostgreSQL for Templates, Preferences, and Suppression Lists (ACID Source of Truth)
- All three use PostgreSQL as the durable store for non-event data: notification templates, user preferences, suppression lists, sender configuration
- Templates: keyed by (template_id, locale); Handlebars/Mustache syntax; versioned
- Preferences: per-user opt-in per channel and per notification type
- Suppression: hard bounce list, global opt-out, per-sender opt-out
- In push_notification_service: `notification_templates`, `user_preferences`, `device_tokens` (Cassandra for scale)
- In email_delivery: `templates`, `suppression_list` (hard bounces + FBL complaints), `sender_domains` (DKIM config)
- In sms_gateway: `sender_numbers` (short_code/long_code/toll_free + throughput_tps), `opt_out_keywords`, `carrier_routes` (MCC/MNC → carrier)

### Redis (Opt-Out Hot Cache + Frequency Cap Counters + Preference Cache)
- All three use Redis for sub-millisecond access to suppression and frequency cap data that is checked on every ingest request
- Opt-out check: Redis Set or Hash lookup before Kafka publish (avoids DB read per request)
- Frequency caps: sliding window counters (ZADD/ZRANGEBYSCORE pattern) per user per channel per time window
- Preference cache: TTL-based cache of PostgreSQL preferences (avoids DB read per delivery)
- In push_notification_service: `opt_out:{user_id}` Set; frequency cap `rate:{user_id}:{channel}` sorted set; device_token cache
- In email_delivery: `suppressed:{email_hash}` key (24h TTL); frequency cap counters; unsubscribe token cache
- In sms_gateway: `opt_out:{phone}:{sender}` key + `opt_out:{phone}:global` key; routing cache `route:{mcc}:{mnc}:{type}` (5-min TTL); OTP dedup

### ClickHouse for OLAP Analytics
- All three feed delivery events into ClickHouse via Kafka → Flink pipeline for real-time and historical analytics
- Metrics: delivery rate, open rate, click rate, bounce rate, opt-out rate — by campaign, channel, time window
- Used by product teams and ML pipelines for A/B testing, send-time optimization, engagement modeling
- In push_notification_service: per-campaign and per-template delivery/open/dismiss analytics
- In email_delivery: per-campaign open/click/bounce/complaint rates; per-ISP deliverability dashboard
- In sms_gateway: per-carrier delivery rate, latency distribution, cost per delivered message

### Idempotency Keys + Deduplication
- All three enforce idempotency on the ingest side (client-generated key) and on the dispatch side (Kafka at-least-once → idempotent consumer)
- Client key: `X-Idempotency-Key` header; Redis SETNX check before Kafka publish; PostgreSQL UNIQUE constraint as durable fallback
- Dispatch-side: before calling APNs/FCM/SMTP/SMPP, check Redis `sent:{notification_id}` key (SETNX) to prevent re-sending after Kafka redeliver
- In push_notification_service: `push:dedup:{notification_id}` Redis key (24h TTL)
- In email_delivery: `email:dedup:{notification_id}` Redis key (24h TTL); also dedups FBL complaints on ingest
- In sms_gateway: `sms:dedup:{notification_id}` Redis key (24h TTL); DLR receipt idempotency for status updates

### Frequency Caps (Sliding Window per User per Channel)
- All three cap notification volume per user to prevent notification fatigue and protect sender reputation
- Implementation: Redis sorted set with score=timestamp; ZADD + ZREMRANGEBYSCORE + ZCARD in Lua script (atomic); configurable per channel per time window
- E.g., max 3 push/hour, max 10 email/day, max 5 SMS/day; configurable per notification type
- Frequency cap check occurs in Ingest Service before Kafka publish; rejected notifications return 429

### Scheduled Delivery (scheduled_at Timestamp)
- All three support future-scheduled notifications via a `scheduled_at` field in the ingest payload
- Kafka message TTL is set to accommodate the delivery window; a Scheduler reads Kafka or a PostgreSQL scheduled_notifications table and publishes to the dispatch topic at the right time
- In push_notification_service: scheduled campaigns for time-zone-aware sends (e.g., 9am local time)
- In email_delivery: scheduled marketing campaigns published in batches 1h before scheduled_at
- In sms_gateway: OTP has immediate delivery (no scheduling); promotional SMS scheduled per campaign

## Common Databases

### Cassandra (RF=3, LOCAL_QUORUM)
- All three; append-only delivery events; time-series clustering by sent_at DESC; TTL 30–90 days; high write throughput

### PostgreSQL
- All three; ACID source of truth for templates, preferences, suppression lists, sender config; Multi-AZ

### Redis Cluster
- All three; opt-out cache, frequency cap counters (sliding window), preference cache (TTL), dedup keys

### ClickHouse
- All three; OLAP analytics pipeline from Kafka → Flink → ClickHouse; delivery/open/click/bounce dashboards

## Common Communication Patterns

### Kafka at-Least-Once + Idempotent Consumer
- All three publish to Kafka from Ingest (at-least-once) and implement idempotent dispatch workers (SETNX dedup before external call)
- Enables horizontal scaling of dispatcher workers via Kafka consumer group rebalance

### Template Rendering (Handlebars/Mustache)
- All three render notification bodies server-side from templates stored in PostgreSQL; no template logic in client or producer
- Templates versioned; rendering done in Ingest Service before Kafka publish (rendered body stored in Kafka message)

## Common Scalability Techniques

### Stateless Ingest + Dispatcher Workers (Kafka as Buffer)
- All three achieve horizontal scalability by making Ingest and Dispatcher workers fully stateless; Kafka provides backpressure and replay; no shared in-process state between pods

### Multi-Region Active-Active Cassandra Replication
- All three replicate Cassandra delivery events across regions for low-latency reads and disaster recovery
- Producers in any region can write events locally; cross-region replication via Cassandra multi-DC configuration

## Common Deep Dive Questions

### How do you guarantee at-least-once delivery without duplicating notifications?
Answer: Two-layer idempotency. Layer 1: Client provides `X-Idempotency-Key`; Ingest Service checks Redis `ingest:dedup:{key}` (SETNX, 24h TTL) — if exists, return cached 202 response without re-publishing to Kafka. Layer 2: Dispatcher worker checks `sent:{notification_id}` in Redis (SETNX) before calling external API (APNs/FCM/SMTP/SMPP) — if already sent, skip the external call but still commit Kafka offset. Cassandra delivery event written on first send only. Kafka 7-day retention allows replay on consumer crash without re-sending already-dispatched notifications.
Present in: push_notification_service, email_delivery, sms_gateway

### How do you enforce per-user frequency caps at high throughput?
Answer: Redis sorted set sliding window. Per user per channel, maintain a ZSET with score=timestamp. On each ingest: Lua script atomically executes `ZREMRANGEBYSCORE(0, now-window)` (prune expired entries) + `ZCARD` (count in window) — if count >= cap, return 429. Otherwise `ZADD(now)` and proceed. Full atomic check-and-increment in one Redis round trip (< 1 ms). Cap limits stored in PostgreSQL config table; Ingest Service caches them (5-min TTL). Separate ZSET per channel (push/email/sms) and configurable window (1h/24h).
Present in: push_notification_service, email_delivery, sms_gateway

### How do you handle opt-out processing in real time?
Answer: PostgreSQL suppression table (durable) + Redis Set cache (fast). On opt-out event: write to `suppression_list` in PostgreSQL (ACID) and `SET opt_out:{user_id}:{channel} 1` in Redis (no TTL — permanent). Ingest Service checks Redis first (< 1 ms); on cache miss (e.g., after Redis restart), falls back to PostgreSQL. For SMS, keyword STOP processed by Ingest Service in the two-way receive path; global opt-out (`opt_out:{phone}:global`) blocks all SMS from any sender. FBL complaints from ISPs are treated as soft opt-outs for email (lower engagement threshold before marking as suppressed).
Present in: push_notification_service, email_delivery, sms_gateway

## Common NFRs

- **Ingest API acceptance latency**: p99 < 100–200 ms (all three return 202 before external dispatch)
- **Push unicast delivery**: p99 < 5 s end-to-end
- **Email transactional inbox delivery**: p95 < 5 min (ISP-dependent)
- **SMS OTP delivery**: p95 < 60 s carrier delivery
- **Availability**: 99.99% for ingest API; dispatcher resilient to external API outages via retry + DLQ
- **Idempotency**: 24 h dedup window; duplicate key returns cached 202, never re-dispatches
- **Opt-out compliance**: opt-out effective within < 10 s; no notification sent post opt-out
- **Security**: no PII in logs; AES-256 at rest; TLS 1.2+ in transit; credentials in Vault (HashiCorp)
- **Frequency caps**: configurable per channel per time window; enforced before Kafka publish
