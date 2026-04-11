# Problem-Specific Design — Notifications (11_notifications)

## Push Notification Service

### Unique Functional Requirements
- 500M registered devices, 200M DAU; peak 1M notifications/sec (5× safety factor = 174K RPS ingest)
- 833,000 RPS APNs/FCM dispatch during broadcast campaigns
- Dual external protocols: APNs HTTP/2 (iOS) + FCM HTTP v1 (Android) with distinct auth and payload formats
- Broadcast to all 500M devices must complete in < 10 minutes
- Unicast delivery p99 < 5 s end-to-end
- Delivery event tracking: sent / delivered / opened (open tracked via SDK beacon)
- Badge count, sound, action buttons, notification collapsing (APNs collapse-id, FCM collapse_key)
- Device token lifecycle: registration, refresh, invalidation on APNs `Unregistered` / FCM `registration_ids.error`

### Unique Components / Services
- **Fan-out Orchestrator**: handles broadcast campaigns (e.g., feature announcements to all 500M devices); pages through device-token store in parallel chunks (10K tokens/chunk); scatter-gather pattern; tracks per-chunk progress in Redis; 500M devices ÷ 10K/chunk = 50K parallel tasks; completes broadcast in < 10 min at 833K dispatches/s
- **APNs Connection Pool**: persistent HTTP/2 connections to APNs (per-team certificate or .p8 token auth); Apple recommends 1,000 concurrent requests per connection; pools 1K connections × 1K req/conn = 1M concurrent APNs requests; APNs HTTP/2 multiplexing eliminates per-notification TLS handshake overhead
- **FCM Connection Pool**: FCM HTTP v1 API; OAuth2 service account token (1 h TTL, cached in Redis); batch `messages.send` up to 500 per request; separate project IDs per app/platform
- **Device Token Store** (Cassandra): PRIMARY KEY ((app_id, user_id), registered_at DESC); stores device_token, platform (ios/android), locale, timezone, app_version, is_active; TTL 90-day auto-expire for inactive tokens (refreshed on each app open); 500M tokens × 300 B ≈ 150 GB
- **Notification Store** (Cassandra): PRIMARY KEY ((user_id), sent_at DESC, notification_id); 30-day hot retention; 500M users × 100 notifications/month × 30 days / row → 45 TB total; LeveledCompactionStrategy (high read:write ratio for notification history API)
- **Delivery Event Tracker**: receives APNs/FCM HTTP response; writes `sent` event to Cassandra; SDK sends `opened` beacon to Event API; Flink joins `sent` + `opened` events to compute open rate; streamed to ClickHouse

### Unique Data Model
- **device_tokens** (Cassandra): PRIMARY KEY = ((app_id, user_id), registered_at DESC); device_token (TEXT), platform ENUM(ios/android/web), locale, timezone, app_version, last_seen_at, is_active; TTL = 90 days (reset on app_open event); 150 GB for 500M tokens
- **Kafka**: 1,000 partitions by `user_id % 1000`; RF=3; `push-transactional` topic (p99 < 5s consumer SLA) and `push-marketing` topic (best-effort); separate consumer groups with different concurrency settings
- **broadcast_jobs** (PostgreSQL): job_id, app_id, template_id, segment_filter, scheduled_at, status, progress_pct, chunks_total, chunks_done; Fan-out Orchestrator updates progress in real time

### Unique Scalability / Design Decisions
- **APNs HTTP/2 multiplexing over individual HTTPS connections**: each APNs connection supports 1,000 concurrent streams; without HTTP/2 multiplexing, 833K/s dispatch would require 833K TLS handshakes/s; with pooled HTTP/2 connections, TLS handshake amortized across thousands of requests per connection
- **Scatter-gather Fan-out over sequential broadcast**: sequential dispatch to 500M devices at 833K/s = 600 s (10 min); scatter-gather with 50K parallel chunk tasks achieves the same throughput while allowing partial failure recovery (retry failed chunks only)
- **TTL-based device token expiry (90 days)**: stale tokens (user uninstalled app) would waste APNs/FCM quota and inflate delivery rate metrics; 90-day TTL auto-purges inactive tokens without a separate cleanup job; `last_seen_at` reset on each app_open prevents premature expiry for active users
- **Kafka partitioned by user_id (not notification_id)**: ensures all notifications for the same user are processed by the same consumer pod, enabling per-user frequency cap enforcement without distributed locking; FIFO ordering per user preserved

### Key Differentiator
Push Notification Service's uniqueness is its **Fan-out Orchestrator for 500M-device broadcasts**: the scatter-gather design (50K parallel chunks, < 10 min completion), combined with APNs HTTP/2 connection pooling (1K connections × 1K concurrent streams = 1M in-flight requests), dual-protocol dispatch (APNs .p8 token auth + FCM OAuth2 service account), and Cassandra device-token store with 90-day TTL — no other problem in this folder operates at the scale of 833K external API calls per second to two distinct mobile push protocols.

---

## Email Delivery Service

### Unique Functional Requirements
- 100M email addresses; peak 50M recipients in 4 hours (3,472 RPS sustained marketing); transactional 833 RPS
- P99 API acceptance < 200 ms; p95 inbox delivery < 5 min (ISP-dependent, not under service control)
- DKIM/SPF/DMARC authentication required for ISP inbox delivery (not optional)
- IP pool reputation management — new IP warm-up required; per-ISP throttle limits (Gmail ~100K/day/new IP)
- Hard bounce (permanent address failure) → immediate suppression; soft bounce (temporary) → retry 3×
- ISP feedback loops (FBL): Gmail/Outlook/Yahoo send complaint (spam button) data; must suppress complainers
- One-click unsubscribe (RFC 8058) in all marketing emails
- Open tracking: 1×1 GIF pixel via CDN; click tracking: redirect proxy before destination URL
- Monthly PostgreSQL range partitioning (critical at 3.3B rows/quarter)

### Unique Components / Services
- **Bounce Processor**: dedicated inbound MTA with its own MX record (`bounce.notifications.example.com`); parses RFC 3464 DSN (Delivery Status Notification) MIME parts; classifies hard vs. soft bounce by SMTP status code (5xx = hard, 4xx = soft); on hard bounce: immediately writes to `suppression_list` in PostgreSQL and `SET suppressed:{email_hash}` in Redis; on soft bounce: increments `bounce_count` — suppress after 3 consecutive soft bounces
- **FBL Handler** (ISP Feedback Loop): subscribes to Gmail Postmaster Tools, Outlook JMRPP, Yahoo FBL; receives RFC 5965 ARF (Abuse Reporting Format) complaint emails at `fbl@notifications.example.com`; parses ARF MIME to extract original recipient email; writes complaint to `suppression_list` (treat as soft opt-out); logs to ClickHouse for complaint rate monitoring; complaint rate > 0.1% → alert (Google inbox delivery at risk)
- **IP Warm-Up Manager**: new dedicated IPs start at 500/day; doubles volume weekly: 500 → 1K → 2K → 4K → … → 100K/day (full Gmail warm-up in 8–10 weeks); warm-up schedule stored in PostgreSQL `ip_warmup_schedule`; Campaign Throttle enforces per-IP daily limits; reputation monitored via Google Postmaster Tools API
- **Campaign Throttle**: per-ISP rate limiting at dispatch layer; reads per-ISP throttle config (Gmail 100K/day/IP, Outlook 50K/day/IP, Yahoo 80K/day/IP) from PostgreSQL; enforces via Redis sliding window counter `throttle:{ip}:{isp}:{date}`; back-pressures dispatcher workers via Kafka consumer pause (vs. dropping)
- **Click/Open Tracker**: click redirect URL = `https://track.example.com/click/{encoded_url}/{notification_id}`; open pixel URL = `https://track.example.com/open/{notification_id}.gif`; both CDN-fronted; Tracker Service records events to Kafka `email-engagement-events` → Flink → Cassandra + ClickHouse
- **SMTP Relay Pool**: separate IP pools per traffic class (transactional dedicated IPs vs. marketing shared IPs); uses SendGrid/SES SMTP API or self-hosted Postfix with DKIM signing; per-ISP connection pools (Gmail prefers < 20 concurrent SMTP connections per IP)

### Unique Data Model
- **emails** (PostgreSQL, monthly range partitioning on created_at): email_id, notification_id, recipient_email, sender_domain, template_id, subject, status, sent_at, opened_at, clicked_at, bounced_at, bounce_type; CRITICAL: 3.3B rows/quarter → monthly partitions for manageable table size; old partitions dropped after 90 days
- **suppression_list** (PostgreSQL): email_hash (SHA-256 of lowercase email), suppression_type ENUM(hard_bounce/soft_bounce/complaint/unsubscribe/manual), suppressed_at, source; UNIQUE on email_hash; Redis `suppressed:{email_hash}` mirrors hot entries (24h TTL, refreshed on access)
- **sender_domains** (PostgreSQL): domain, dkim_private_key_id (reference to Vault), dkim_selector, spf_record, dmarc_policy; one row per sending domain; DKIM private key stored in HashiCorp Vault, fetched by SMTP relay at startup
- **Kafka**: `email-transactional` (15-min TTL — if not consumed in 15 min, OTP is expired anyway) + `email-marketing` (48h TTL — allows re-processing on consumer crash); RF=3
- **S3**: rendered email HTML bodies (5:1 gzip compression); reference stored in Cassandra event; enables re-send and audit without re-rendering

### Unique Scalability / Design Decisions
- **Monthly PostgreSQL range partitioning (over no partitioning or hash sharding)**: 3.3B rows/quarter makes a single unpartitioned table unusable (index bloat, slow queries); range partitioning by created_at enables partition pruning for `WHERE sent_at BETWEEN x AND y` queries (most common access pattern); old partitions can be dropped instantly (DDL) rather than row-deleting 3.3B rows
- **Dedicated IPs for transactional vs. shared for marketing**: transactional emails (order confirmations, OTP) require low-latency inbox delivery and must never be delayed by marketing reputation degradation; separate IP pools ensure a marketing campaign complaint spike cannot affect transactional delivery
- **15-min TTL on `email-transactional` Kafka topic**: OTP and security alert emails have a short validity window; if they sit in Kafka for > 15 min (e.g., consumer crash), delivering them is harmful (expired OTP confuses user); TTL causes automatic discard, preventing delayed delivery of stale security messages
- **FBL complaint rate monitoring in ClickHouse**: ISPs will block entire sending domain if complaint rate > 0.1%; real-time complaint rate dashboard (Kafka → Flink → ClickHouse, 60 s lag) allows on-call to pause campaigns before reaching ISP threshold — reactive suppression alone is not fast enough at 3,472 RPS

### Key Differentiator
Email Delivery Service's uniqueness is its **ISP deliverability management stack**: DKIM/SPF/DMARC signing pipeline, IP warm-up management (8–10 week schedule, per-ISP throttle enforcement via Redis sliding window), Bounce Processor (RFC 3464 DSN parsing → hard/soft classification → immediate suppression), and FBL Handler (RFC 5965 ARF parsing for Gmail/Outlook/Yahoo complaint loops) — these components exist solely because ISPs are external gatekeepers with blacklist power, a constraint absent from push notification and SMS delivery.

---

## SMS Gateway

### Unique Functional Requirements
- 10M SMS/day; peak 5,000 RPS (OTP burst); SMPP peak 5,750 PDUs/s (accounting for multipart splits)
- International: 200+ countries; character encoding detection per message (GSM-7 = 160 chars, UCS-2 = 70 chars per segment)
- Long message splitting: messages > 1 segment require UDH (User Data Header) headers; multipart reassembly by handset
- Two-way SMS: inbound STOP/UNSTOP/HELP keyword handling; short codes, long codes, toll-free numbers
- Per-sender opt-out (STOP to a specific sender) + global opt-out (STOP to any sender stops all SMS)
- 10DLC Campaign Registry (US): all A2P traffic requires registered campaigns with TCR (The Campaign Registry)
- Delivery receipts (DLR): via SMPP `deliver_sm` PDU or carrier HTTP callback; update sms status in Cassandra
- P99 API acceptance < 100 ms; p95 carrier delivery < 60 s

### Unique Components / Services
- **SMPP Connection Manager**: maintains persistent SMPP v3.4 `bind_transceiver` sessions to each carrier aggregator (Twilio, Sinch, Vonage); per-carrier `window_size` (number of unacknowledged PDUs in flight, typically 10–100); auto-rebind on session drop (exponential backoff); SMPP `submit_sm` for send, `deliver_sm` for DLR and inbound MO (mobile-originated); 5,750 PDUs/s peak across all sessions
- **Character Encoder**: detects per-message encoding: if all characters in GSM-7 Basic Character Set → GSM-7 (160 chars/segment, 153 with UDH); if any non-GSM-7 character (emoji, CJK, etc.) → UCS-2 (70 chars/segment, 67 with UDH); splits into N segments; prepends UDH `05 00 03 {ref_num} {total} {part_no}` to each segment for concatenation; returns list of SMPP `submit_sm` PDUs
- **Carrier Router**: selects best carrier per (MCC, MNC, message_type) using `cost + quality_score` formula; carrier routes stored in PostgreSQL `carrier_routes`; cached in Redis `route:{mcc}:{mnc}:{type}` (5-min TTL); quality_score updated continuously from DLR success rates (Kafka consumer on `dlr-events` → PostgreSQL UPDATE); hot routes (US/UK/IN/DE) served from Redis in < 1 ms; cold routes (rare MCC/MNC) fall back to PostgreSQL
- **DLR Processor**: receives delivery receipts from carriers via SMPP `deliver_sm` PDU (for SMPP connections) or carrier HTTP callback (for HTTP adapters); maps carrier-specific status codes to normalized ENUM (delivered/failed/expired/rejected); updates `sms_events` in Cassandra; publishes `dlr-events` to Kafka for quality score feedback loop and producer webhook callbacks
- **Inbound MO Handler**: receives mobile-originated messages via SMPP `deliver_sm`; detects keyword: STOP → write opt-out to PostgreSQL + Redis (`SET opt_out:{phone}:{sender} 1` and/or `SET opt_out:{phone}:global 1`); UNSTOP → delete opt-out keys; HELP → auto-reply with support instructions; routes non-keyword MO messages to producer via webhook
- **HTTP/REST Adapter**: wraps SMPP sessions in a REST API for carriers without SMPP support (some regional carriers use HTTP); translates REST responses to normalized DLR status; transparent to upstream dispatcher workers

### Unique Data Model
- **sender_numbers** (PostgreSQL): sender_id, number (E.164), sender_type ENUM(short_code/long_code/toll_free), country_code, carrier, throughput_tps (short_code=100, long_code=1, toll_free=3), campaign_id (10DLC), is_active; throughput_tps enforced by per-sender Redis rate limiter
- **carrier_routes** (PostgreSQL): route_id, mcc, mnc, message_type ENUM(transactional/marketing/otp), carrier_id, cost_per_sms, quality_score (0.0–1.0, updated from DLR feedback), is_active; UNIQUE (mcc, mnc, message_type, carrier_id)
- **sms_events** (Cassandra): PRIMARY KEY = ((phone_number), sent_at DESC, sms_id); status ENUM(queued/sent/delivered/failed/expired), segments_count, carrier, mcc, mnc, dlr_received_at; TTL 90 days; 10M/day × 90 days × 200 B/row ≈ 180 GB
- **sms_bodies** (separate Cassandra table or S3): body text stored separately from events; TTL 7 days (short — body contains PII); keyed by sms_id; 10M/day × 7 days × 200 B/row ≈ 14 GB
- **Redis routing cache**: `route:{mcc}:{mnc}:{type}` Hash (5-min TTL); `opt_out:{phone}:{sender}` key (permanent); `opt_out:{phone}:global` key (permanent); `rate:{sender_id}` sliding window counter (1-second window, enforces throughput_tps)
- **Storage total**: 360 GB SMS metadata (90d) + 14 GB bodies (7d) + 180 GB events (90d) = 586 GB hot

### Unique Scalability / Design Decisions
- **SMPP bind_transceiver with per-carrier window_size (over HTTP polling)**: SMPP is a persistent binary TCP protocol; `bind_transceiver` allows simultaneous send (`submit_sm`) and receive (`deliver_sm`) on a single session; `window_size=100` allows 100 unacknowledged PDUs in flight per session — orders of magnitude higher throughput than HTTP request/response per SMS; 5,750 PDUs/s achievable with ~58 SMPP sessions at window_size=100
- **Per-message character encoding detection (over always UCS-2)**: sending all messages as UCS-2 would halve capacity (70 chars/segment vs. 160 for GSM-7); 70-char limit forces more multipart splits (more PDUs, higher cost); encoding detection adds ~0.1 ms per message (table lookup) but reduces average cost per message by ~30% (most A2P messages are Latin script = GSM-7)
- **UDH multipart over SMPP `long_message` TLV parameter**: UDH is the universal concatenation standard supported by all handsets (including feature phones); SMPP TLV `message_payload` for long messages is carrier-specific and not universally supported; UDH segments are independently deliverable and retry-able, enabling partial delivery on network failure
- **Carrier quality score feedback loop (DLR → PostgreSQL → Redis)**: delivery rates vary significantly by carrier per destination (some carriers have > 20% DLR failure for certain MCC/MNC pairs); continuously updating quality_score from DLR data (Kafka consumer → running average) and routing to highest quality_score carrier improves p95 delivery latency without manual carrier management

### Key Differentiator
SMS Gateway's uniqueness is its **SMPP protocol stack + international carrier routing**: persistent SMPP v3.4 bind_transceiver sessions with per-carrier window sizing, per-message GSM-7/UCS-2 encoding detection with UDH multipart splitting, carrier selection per (MCC, MNC, message_type) with live DLR-based quality scoring, and two-way MO handling for STOP/UNSTOP/HELP keywords with per-sender and global opt-out — the entire complexity stems from SMS being a carrier-controlled, country-specific, character-encoding-sensitive protocol that no other notification channel in this folder requires.
