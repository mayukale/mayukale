# System Design: SMS Gateway

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Send SMS messages** — transactional (OTPs, alerts, notifications), marketing (promotions), and operational (system alerts) — to phone numbers globally.
2. **Carrier aggregation** — route through multiple carrier connections (direct connections, tier-1 aggregators like Twilio, Bandwidth, Sinch); select optimal route per destination country/carrier.
3. **Message routing** — intelligent routing based on cost, quality, latency, and carrier relationship per destination.
4. **International delivery** — support messages to 200+ countries; handle country-specific number formats, character sets, and regulations.
5. **Delivery receipts (DLR)** — receive and process delivery confirmations from carriers; expose per-message delivery status.
6. **Long message splitting (concatenation)** — automatically split messages exceeding 160 GSM-7 characters (or 70 UCS-2 characters) into multipart SMS with UDH headers; reassemble on handset.
7. **Two-way SMS** — receive inbound SMS on short codes, long codes, and toll-free numbers; route to subscribing services via webhook; support keyword-based auto-responses (STOP, HELP, etc.).
8. **Sender ID management** — support short codes (5-6 digits), long codes (10-digit US numbers), toll-free numbers (US/Canada 8XX numbers), and alphanumeric sender IDs (where permitted).
9. **Opt-out management (STOP/UNSTOP)** — honor STOP keywords (and variants) immediately; honor UNSTOP/START for re-consent; manage per-sender-number and per-user opt-out state.
10. **Message templates and variable substitution** — support parameterized message templates with character-limit-aware rendering.
11. **Scheduling** — immediate and future-scheduled delivery.
12. **Character encoding** — auto-detect and apply GSM-7 (160 chars) or UCS-2 (70 chars) encoding based on message content; report part count before send.

### Non-Functional Requirements

1. **Scale**: 10 M SMS/day; peak 5,000 SMS/second (flash campaigns or emergency alerts).
2. **Latency**: OTP/transactional SMS handed to carrier within 2 seconds of API call; carrier delivery typically within 30 seconds to 2 minutes (carrier SLA-dependent; outside our control after handoff).
3. **Availability**: 99.99% uptime for the send API (≤52 min/year downtime).
4. **Durability**: At-least-once delivery; zero message loss after API acceptance; idempotency prevents duplicates.
5. **Compliance**: TCPA (US) — no marketing SMS without prior express written consent; GDPR — PII purge on request; CTIA guidelines — mandatory STOP/HELP handling; TRAI regulations for India; PECR for UK.
6. **Security**: No plaintext PII in logs; message content encrypted in transit (TLS 1.2+) and at rest (AES-256 for stored messages); SMPP sessions authenticated.
7. **Observability**: Per-message delivery status available within 30 seconds of carrier DLR receipt.
8. **Cost**: Route optimization minimizes per-SMS cost while maintaining quality thresholds; report cost per message and per campaign.

### Out of Scope

- MMS (Multimedia Messaging Service) — different protocol; separate design.
- RCS (Rich Communication Services) — separate design; falls back to SMS.
- Voice calls (though phone number management overlaps).
- SIP/VoIP services.
- Real-time chat (WhatsApp Business API — different protocol).

---

## 2. Users & Scale

### User Types

| Actor | Description |
|---|---|
| **Producer (internal service)** | Backend microservice calling the SMS API (e.g., auth service for OTPs, fraud service for alerts). |
| **Marketing operator** | Creates SMS campaigns to opted-in user segments. |
| **End user (recipient)** | Mobile subscriber whose phone receives the SMS. |
| **End user (two-way SMS)** | Mobile subscriber who replies to a short code or long code. |
| **Admin** | Manages carrier connections, sender IDs, routing rules, and opt-out overrides. |
| **Analyst** | Queries delivery analytics. |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 10 M SMS/day total.
- Transactional (OTPs, alerts): 60% = 6 M/day.
- Marketing/bulk: 40% = 4 M/day.
- Peak OTP burst: 5,000 SMS/s (e.g., major promotional email sends OTP to authenticate purchases).
- Average message body: 80 characters GSM-7 (single part).
- 15% of messages require multipart (> 160 chars) → avg 2.5 parts = 1 additional SMPP PDU per long message.
- Inbound SMS (two-way): 2% of outbound = 200,000/day.
- Delivery receipts (DLR): 1 DLR per sent message = 10 M DLRs/day.

| Metric | Calculation | Result |
|---|---|---|
| Average outbound RPS | 10 M / 86,400 s | ~116 RPS |
| Peak outbound RPS | 5,000 SMS/s (stated requirement) | 5,000 RPS |
| SMPP submit RPS (accounting for multipart) | 10 M/day × 1.15 parts avg / 86,400 = ~133 avg; × 43 (peak factor) | ~5,750 SMPP submit_sm/s peak |
| DLR ingest RPS | 10 M/day / 86,400 s | ~116 RPS avg; ~580 RPS peak (DLRs arrive in bursts) |
| Inbound SMS RPS | 200,000/day / 86,400 s | ~2.3 RPS avg; ~50 RPS peak |
| API ingest RPS | 5,000 SMS/s peak | 5,000 RPS |
| Unique destination numbers | Assume 50 M unique registered numbers in the platform | — |
| Opt-out check RPS | Same as send RPS | 5,000 RPS |

### Latency Requirements

| Operation | Target |
|---|---|
| API acceptance (POST /sms) | P99 < 100 ms |
| OTP delivery to carrier | P99 < 2 s |
| Carrier delivery to handset | P95 < 60 s (carrier SLA; not fully our control) |
| OPT-out (STOP) effective | < 5 s from inbound STOP receipt |
| DLR status queryable | < 30 s from DLR receipt |
| Two-way SMS webhook delivery | P99 < 500 ms from inbound receipt |
| Campaign SMS send rate | 5,000 SMS/s sustained |

### Storage Estimates

| Data | Size per record | Count / Retention | Total |
|---|---|---|---|
| SMS records (metadata only) | 400 B | 10 M/day × 90 days = 900 M | 360 GB |
| Message content (body) | 200 B avg | 10 M/day × 7 days hot | 14 GB |
| DLR events | 200 B | 10 M/day × 90 days = 900 M | 180 GB |
| Inbound SMS | 400 B | 200,000/day × 90 days | 7.2 GB |
| Opt-out records | 200 B | 50 M numbers | 10 GB |
| Carrier routing config | 5 KB/route | 500 routes (country × carrier) | 2.5 MB |
| Phone number registry | 300 B | 50 M numbers | 15 GB |
| **Total hot storage (90-day)** | | | ~586 GB |

Storage is modest due to short message lengths. Primary scale concern is throughput, not storage.

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| API ingest | 5,000 RPS × 500 B payload | ~2.5 MB/s peak |
| SMPP egress to carriers | 5,750 PDUs/s × 300 B per PDU | ~1.7 MB/s peak |
| DLR ingress from carriers | 580 RPS × 200 B | ~116 KB/s peak |
| Inbound SMS ingress | 50 RPS × 400 B | ~20 KB/s peak |

Bandwidth is not the bottleneck — connection management and message throughput are.

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                      PRODUCER SERVICES                                │
│     (Auth Svc OTP, Fraud Alerts, Marketing Svc, Ops Alerting)         │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ HTTPS REST
                               ▼
              ┌────────────────────────────────────┐
              │     API Gateway / Load Balancer      │
              │  (TLS termination, auth, rate limit) │
              └────────────────┬───────────────────┘
                               │
               ┌───────────────▼─────────────────────────┐
               │           SMS Ingest Service              │
               │  - Validate E.164 number format           │
               │  - Check opt-out status (Redis)           │
               │  - Apply frequency cap (Redis)            │
               │  - Select template / render body          │
               │  - Detect encoding (GSM-7 vs UCS-2)       │
               │  - Compute part count                     │
               │  - Assign sender ID                       │
               │  - Publish to Kafka (priority routing)    │
               └─────────┬──────────────┬─────────────────┘
                         │              │
              ┌──────────▼─────┐  ┌─────▼───────────────────┐
              │ Kafka:          │  │ Kafka:                   │
              │ transactional   │  │ marketing / bulk         │
              │ (high priority) │  │ (low priority)           │
              └──────┬──────────┘  └──────┬──────────────────┘
                     │                    │
          ┌──────────▼────────────────────▼──────────────────────┐
          │                  Router / Dispatcher                   │
          │  - Selects carrier for destination (routing table)    │
          │  - Splits long messages into multipart (UDH)          │
          │  - Rate-limits per carrier per destination MCCMNC     │
          │  - Sends via SMPP or HTTP to carrier pool             │
          └──────────┬──────────────────────┬────────────────────┘
                     │                      │
          ┌──────────▼───────┐   ┌──────────▼─────────────────────┐
          │ SMPP Connection  │   │   HTTP/REST Carrier Adapters    │
          │ Pool             │   │   (Twilio, Sinch, Vonage APIs)  │
          │ (direct carriers)│   │                                 │
          └──────────┬───────┘   └──────────┬──────────────────────┘
                     │                      │
          ┌──────────▼──────────────────────▼───────────────────────┐
          │                  Carrier Network                          │
          │  (Mobile Network Operators: AT&T, Verizon, T-Mobile,    │
          │   International: Vodafone, Orange, Airtel, etc.)         │
          └──────────┬──────────────────────────────────────────────┘
                     │ Delivery Receipts (DLR) + Inbound SMS
          ┌──────────▼──────────────────────────────────────────────┐
          │              Inbound / DLR Processing                    │
          │  ┌──────────────────────┐  ┌──────────────────────────┐ │
          │  │  DLR Processor       │  │   Inbound SMS Handler    │ │
          │  │  - Receives SMPP     │  │   - STOP/HELP keywords   │ │
          │  │    deliver_sm        │  │   - Opt-out write        │ │
          │  │  - Parses status     │  │   - Webhook delivery     │ │
          │  │  - Writes to events  │  │   - Auto-responses       │ │
          │  └──────────┬───────────┘  └──────────┬───────────────┘ │
          │             │                          │                  │
          │  ┌──────────▼──────────────────────────▼──────────────┐  │
          │  │              Kafka: events topic                    │  │
          │  └──────────┬──────────────────────────────────────────┘  │
          └─────────────┼────────────────────────────────────────────┘
                        │
          ┌─────────────▼──────────────────────────────────────────┐
          │                     Data Stores                          │
          │  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐  │
          │  │  SMS Records │  │ Delivery Evts│  │  Opt-out List │  │
          │  │ (Cassandra)  │  │ (Cassandra)  │  │  (Redis +     │  │
          │  │              │  │              │  │   Postgres)   │  │
          │  └─────────────┘  └──────────────┘  └───────────────┘  │
          │  ┌─────────────┐  ┌──────────────┐                      │
          │  │ Phone Number │  │ Routing Table│                      │
          │  │ Registry     │  │  (Postgres + │                      │
          │  │ (Postgres)   │  │   Redis)     │                      │
          │  └─────────────┘  └──────────────┘                      │
          └─────────────────────────────────────────────────────────┘
                        │
          ┌─────────────▼──────────────────────────────────────────┐
          │            Analytics & Monitoring                        │
          │    (Kafka → Flink → ClickHouse → Grafana)               │
          └─────────────────────────────────────────────────────────┘
```

**Component roles:**

- **API Gateway**: Terminates TLS, enforces per-producer rate limits, validates OAuth2 tokens, normalizes phone numbers to E.164.
- **SMS Ingest Service**: Validates E.164 format, checks opt-out and frequency caps, renders template, detects GSM-7 vs. UCS-2 encoding, computes part count, assigns sender ID, publishes to Kafka.
- **Kafka (transactional / marketing topics)**: Durable queues with priority separation; transactional TTL = 15 minutes (stale OTPs are useless); marketing TTL = 48 hours.
- **Router / Dispatcher**: Reads from Kafka; applies carrier routing logic (country code → carrier → least-cost route with quality threshold); splits long messages into multipart; enforces per-carrier throughput limits; delegates to SMPP pool or HTTP adapter.
- **SMPP Connection Pool**: Persistent SMPP v3.4 sessions to tier-1 carriers; handles submit_sm, submit_sm_resp, deliver_sm (DLR and inbound), enquire_link (keepalive).
- **HTTP/REST Carrier Adapters**: Adapter pattern for Twilio, Sinch, Vonage — normalizes their HTTP APIs to an internal carrier interface; handles credential management and retry.
- **DLR Processor**: Receives SMPP `deliver_sm` PDUs or HTTP callbacks from carriers; parses delivery status; writes to Cassandra delivery events; publishes status updates to Kafka.
- **Inbound SMS Handler**: Receives mobile-originated (MO) messages; detects STOP/HELP/UNSTOP keywords (and regional variants); writes opt-out; delivers non-keyword messages via webhook to registered handlers; sends auto-responses.
- **Phone Number Registry**: Stores all provisioned sender phone numbers (short codes, long codes, toll-free) with their capabilities, registration status, and current throughput limits.
- **Routing Table**: Per `(destination_mccmnc, message_type)` mapping to ordered list of carriers with quality and cost metadata; cached in Redis.

**Primary use-case data flow (OTP transactional):**

1. Auth Service POST `/v1/sms` with `{to: "+14155552671", template_id: "otp_verification", data: {code: "394721"}}`.
2. Ingest Service validates E.164, checks opt-out in Redis (< 1 ms), frequency cap check.
3. Template rendered: "Your verification code is 394721. Valid for 5 minutes."
4. Encoding detected: all ASCII chars → GSM-7; length = 55 chars → single part.
5. Published to `sms.transactional` Kafka topic.
6. Router picks up within 100 ms; looks up routing table for `+1415` prefix (US, AT&T) → selects carrier (direct SMPP to AT&T or via Twilio based on current quality scores).
7. SMPP `submit_sm` PDU sent on persistent SMPP session; `message_id` returned in `submit_sm_resp`.
8. AT&T delivers to handset; sends `deliver_sm` DLR PDU back → DLR Processor writes `delivered` event to Cassandra.
9. Status available via `GET /v1/sms/{sms_id}`.

---

## 4. Data Model

### Entities & Schema

```sql
-- =============================================
-- SMS Records (Cassandra CQL)
-- =============================================
CREATE TABLE sms_messages (
    sms_id          UUID,
    producer_id     TEXT,
    idempotency_key TEXT,
    sms_type        TEXT,       -- 'transactional' | 'marketing' | 'operational'
    campaign_id     UUID,
    to_number       TEXT,       -- E.164 format, e.g. +14155552671
    from_number     TEXT,       -- E.164 or alphanumeric sender ID
    body            TEXT,
    encoding        TEXT,       -- 'GSM7' | 'UCS2'
    part_count      INT,
    carrier_id      TEXT,       -- selected carrier for this send
    carrier_message_id TEXT,    -- ID returned by carrier on submit
    status          TEXT,       -- 'queued'|'submitted'|'delivered'|'failed'|'undeliverable'|'suppressed'
    scheduled_at    TIMESTAMP,
    submitted_at    TIMESTAMP,
    created_at      TIMESTAMP,
    cost_units      INT,        -- carrier-reported cost units (1 per part for most carriers)
    metadata        TEXT,       -- JSON passthrough
    PRIMARY KEY (sms_id)
) WITH default_time_to_live = 7776000;  -- 90-day TTL

-- Index for per-number message history (recent)
CREATE TABLE sms_by_number (
    to_number       TEXT,
    month_bucket    TEXT,       -- 'YYYY-MM' to bound partition size
    sms_id          UUID,
    status          TEXT,
    created_at      TIMESTAMP,
    PRIMARY KEY ((to_number, month_bucket), created_at, sms_id)
) WITH CLUSTERING ORDER BY (created_at DESC)
  AND default_time_to_live = 2592000;  -- 30 days

-- =============================================
-- Delivery Events (Cassandra CQL)
-- =============================================
CREATE TABLE delivery_events (
    sms_id          UUID,
    event_id        TIMEUUID,
    event_type      TEXT,   -- 'queued'|'submitted'|'delivered'|'failed'|'undeliverable'|'expired'|'rejected'
    carrier_status  TEXT,   -- raw carrier status code/string
    carrier_err_code TEXT,
    part_index      INT,    -- for multipart: which part (0-indexed)
    created_at      TIMESTAMP,
    PRIMARY KEY (sms_id, event_id)
) WITH CLUSTERING ORDER BY (event_id DESC)
  AND default_time_to_live = 7776000;

-- =============================================
-- Opt-out / STOP Records (PostgreSQL)
-- =============================================
CREATE TABLE sms_opt_outs (
    to_number       TEXT        NOT NULL,
    from_number     TEXT        NOT NULL,   -- sender number/short code the user stopped
    -- '' (empty) = global opt-out from all senders on this platform
    opted_out_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    opt_in_at       TIMESTAMPTZ,            -- null if still opted out
    source          TEXT        NOT NULL,   -- 'inbound_stop'|'api'|'admin'|'carrier_reported'
    keyword         TEXT,                   -- 'STOP'|'UNSUBSCRIBE'|'CANCEL'|'END'|'QUIT'
    PRIMARY KEY (to_number, from_number)
);

CREATE INDEX idx_sms_optout_to ON sms_opt_outs(to_number);

-- Redis representation:
-- Key: sms_optout:{sha256(to_number)}:{sha256(from_number)}
-- Value: 1 (opted out) | 0 (active)
-- Key: sms_optout:{sha256(to_number)}:global
-- Value: 1 (globally opted out of all senders)
-- No TTL — permanent until opt-in

-- =============================================
-- Inbound SMS (Cassandra CQL)
-- =============================================
CREATE TABLE inbound_sms (
    inbound_id      UUID,
    carrier_id      TEXT,
    from_number     TEXT,
    to_number       TEXT,       -- short code, long code, or toll-free
    body            TEXT,
    encoding        TEXT,
    received_at     TIMESTAMP,
    keyword         TEXT,       -- extracted keyword if STOP/HELP/etc; null otherwise
    webhook_status  TEXT,       -- 'pending'|'delivered'|'failed' (for non-keyword messages)
    webhook_attempts INT        DEFAULT 0,
    PRIMARY KEY (inbound_id)
) WITH default_time_to_live = 2592000;

-- For keyword auto-responses tracking
CREATE TABLE inbound_by_number_date (
    to_number       TEXT,
    date_bucket     DATE,
    inbound_id      UUID,
    from_number     TEXT,
    keyword         TEXT,
    received_at     TIMESTAMP,
    PRIMARY KEY ((to_number, date_bucket), received_at, inbound_id)
) WITH CLUSTERING ORDER BY (received_at DESC)
  AND default_time_to_live = 2592000;

-- =============================================
-- Phone Number Registry (PostgreSQL)
-- =============================================
CREATE TABLE sender_numbers (
    number          TEXT        PRIMARY KEY,    -- E.164 or alphanumeric
    number_type     TEXT        NOT NULL,        -- 'short_code'|'long_code'|'toll_free'|'alphanumeric'
    country_code    TEXT        NOT NULL,
    carrier_id      TEXT,
    capabilities    TEXT[]      NOT NULL,        -- ['sms', 'mms', 'voice']
    throughput_tps  INT         NOT NULL DEFAULT 1,
    -- short_code: 100 TPS; long_code: 1 TPS; toll_free: 3 TPS; shared short code: varies
    registration_status TEXT    NOT NULL DEFAULT 'active',
    -- 'pending_registration'|'active'|'suspended'|'released'
    campaign_registry_id TEXT,                  -- 10DLC Campaign Registry ID (US)
    use_case        TEXT,                        -- 'transactional'|'marketing'|'2fa'|'mixed'
    is_active       BOOLEAN     NOT NULL DEFAULT TRUE,
    provisioned_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    metadata        JSONB
);

-- =============================================
-- Carrier Routing Table (PostgreSQL + Redis cache)
-- =============================================
CREATE TABLE carrier_routes (
    route_id        UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    mcc             TEXT        NOT NULL,   -- Mobile Country Code e.g. '310'
    mnc             TEXT        NOT NULL,   -- Mobile Network Code e.g. '410' (AT&T US)
    country_code    TEXT        NOT NULL,   -- ISO 3166-1 alpha-2
    message_type    TEXT        NOT NULL,   -- 'transactional'|'marketing'|'all'
    carrier_id      TEXT        NOT NULL    REFERENCES carriers(carrier_id),
    priority        INT         NOT NULL,   -- 1=highest priority
    cost_per_sms    NUMERIC(8,6) NOT NULL,  -- USD per message part
    quality_score   NUMERIC(5,2),          -- 0-100; updated by DLR feedback loop
    is_active       BOOLEAN     NOT NULL DEFAULT TRUE,
    UNIQUE (mcc, mnc, message_type, carrier_id)
);

CREATE TABLE carriers (
    carrier_id      TEXT        PRIMARY KEY,
    name            TEXT        NOT NULL,
    connection_type TEXT        NOT NULL,  -- 'smpp'|'http'
    smpp_host       TEXT,
    smpp_port       INT,
    http_base_url   TEXT,
    throughput_tps  INT         NOT NULL DEFAULT 100,
    supported_countries TEXT[],
    is_active       BOOLEAN     NOT NULL DEFAULT TRUE,
    credentials_vault_path TEXT NOT NULL   -- Vault path for SMPP bind password or HTTP API key
);

-- Redis routing cache:
-- Key: route:{mcc}{mnc}:{message_type}
-- Value: JSON list of carrier routes sorted by priority
-- TTL: 5 minutes (updated when routing table changes)
```

### Database Choice

| Database | Strengths | Weaknesses | Fit |
|---|---|---|---|
| **Cassandra** | High write throughput, TTL, time-series append-only pattern, wide-column per-number history | No secondary indexes at scale, limited ad-hoc queries | **Selected** for SMS records, delivery events, inbound SMS — all high-volume append-only |
| **PostgreSQL** | ACID, rich indexing, relational integrity, excellent for routing config | Vertical scaling limits at extreme throughput | **Selected** for sender numbers, carriers, routing table (infrequently written, transactionally important), opt-outs source of truth |
| **Redis** | Sub-millisecond reads, native counters, sets | Not primary store; memory-constrained | **Selected** for opt-out hot cache, routing cache, frequency caps, SMPP session state |
| **DynamoDB** | Managed, auto-scale, predictable latency | Vendor lock-in; cost at scale | Viable alternative for opt-outs and SMS records; not selected to avoid lock-in |
| **MySQL** | Familiar; good tooling | Similar limitations to Postgres; less extensible | Not selected |

**Cassandra justification**: SMS delivery events are purely append-only (new events are never updated, only new rows inserted). At 10 M SMS/day + 10 M DLR events/day = 20 M rows/day, Cassandra's LSM-tree write path is optimal. TTL ensures automatic expiry without expensive DELETE operations. The primary read pattern (`SELECT * FROM delivery_events WHERE sms_id = ?`) is a single-partition lookup regardless of total dataset size.

**PostgreSQL justification for routing**: Carrier routing rules are modified infrequently (< 100 times/day) but must be correct — an incorrect route wastes money or causes delivery failure. ACID transactions on route updates prevent partial configurations. The dataset is small (< 100 MB) and benefits from full SQL flexibility for ad-hoc routing analysis.

---

## 5. API Design

All endpoints require `Authorization: Bearer <oauth2_client_credentials_token>`. Rate limits per `producer_id`.

### Send SMS

```
POST /v1/sms
Rate limit: 5,000 RPS per platform; 1,000 RPS per producer (transactional); 50 RPS per producer (marketing)
Idempotency: Idempotency-Key header required

Request:
{
  "to": "+14155552671",           // E.164 format required
  "from": "+18005550123",         // E.164, short code (e.g. "12345"), or alphanumeric (e.g. "CompanyName")
  "sms_type": "transactional",   // 'transactional' | 'marketing' | 'operational'
  "template_id": "uuid",         // mutually exclusive with 'body'
  "body": "Your code is 394721", // alternative to template_id
  "template_data": {
    "code": "394721",
    "expiry_minutes": 5
  },
  "campaign_id": "uuid",         // optional; for marketing sends
  "scheduled_at": "2026-04-10T09:00:00Z",  // optional
  "ttl_seconds": 900,            // default: 900 s for transactional; 86400 for marketing
  "options": {
    "allow_multipart": true,     // if false, return error if message requires splitting
    "respect_quiet_hours": false, // transactional always false
    "dry_run": false,
    "preferred_carrier": "twilio"  // optional carrier override for testing
  },
  "metadata": { "user_id": "uuid" }  // passthrough; not included in SMS body
}

Response 202 Accepted:
{
  "sms_id": "uuid",
  "status": "queued" | "suppressed",
  "part_count": 1,
  "encoding": "GSM7",
  "estimated_cost_usd": 0.0075,
  "suppression_reason": null | "opted_out" | "frequency_cap" | "invalid_number"
}

Response 400 Bad Request:
{ "error": "INVALID_NUMBER", "message": "+1415555 is not a valid E.164 number" }
{ "error": "MULTIPART_REQUIRED", "message": "Message requires 3 parts; allow_multipart must be true", "part_count": 3 }
{ "error": "INVALID_SENDER", "message": "Sender +18005550123 is not provisioned for transactional use" }

Response 409 Conflict:
{ "error": "DUPLICATE_REQUEST", "sms_id": "uuid-of-original" }
```

### Get SMS Status

```
GET /v1/sms/{sms_id}
Rate limit: 2,000 RPS per producer

Response 200:
{
  "sms_id": "uuid",
  "to": "+14155552671",
  "from": "+18005550123",
  "body": "Your code is 394721",
  "status": "delivered",
  "part_count": 1,
  "encoding": "GSM7",
  "carrier_id": "twilio",
  "carrier_message_id": "SM1234567890",
  "submitted_at": "2026-04-09T12:00:01Z",
  "delivery_events": [
    { "event_type": "submitted", "created_at": "2026-04-09T12:00:01Z" },
    { "event_type": "delivered", "carrier_status": "DELIVRD", "created_at": "2026-04-09T12:00:15Z" }
  ]
}
```

### Manage Opt-outs

```
POST /v1/opt-outs
GET  /v1/opt-outs?to_number={e164}&from_number={e164}
DELETE /v1/opt-outs          (re-consent / UNSTOP)
Rate limit: 500 RPS per producer

POST Request (manual opt-out):
{
  "to_number": "+14155552671",
  "from_number": "+18005550123",    // null = global opt-out from all platform senders
  "source": "api",
  "reason": "Customer request"
}

DELETE Request (re-consent):
{
  "to_number": "+14155552671",
  "from_number": "+18005550123"
}

Response 200: { "status": "opted_out" | "re-consented", "effective_at": "2026-04-09T12:00:00Z" }
```

### Inbound SMS Webhook Registration

```
POST /v1/webhooks
Body:
{
  "from_number": "+18005550123",   // which sender number this webhook applies to
  "url": "https://service.internal/sms/inbound",
  "secret": "hmac_signing_secret",
  "events": ["inbound_sms"]
}
Response 201: { "webhook_id": "uuid" }
```

### Send Rate Preview (character counting)

```
POST /v1/sms/preview
Body:
{
  "body": "Your one-time code is {{code}}. Valid for {{expiry_minutes}} minutes.",
  "template_data": { "code": "394721", "expiry_minutes": "5" }
}
Response 200:
{
  "rendered_body": "Your one-time code is 394721. Valid for 5 minutes.",
  "character_count": 52,
  "encoding": "GSM7",
  "part_count": 1,
  "max_single_part_chars": 160,
  "max_multipart_chars_per_part": 153
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Carrier Aggregation and Intelligent Message Routing

**Problem it solves:**
No single carrier provides the best cost, quality, and coverage for all global destinations. A message to a US AT&T subscriber routes differently than one to an Indian Airtel subscriber. Routing must be dynamic — quality scores degrade when a carrier has outages or increased latency, and the system must detect this and re-route automatically.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Single carrier (Twilio only)** | Route all traffic through one API vendor | Simple; no routing logic | Vendor lock-in; no cost optimization; single point of failure; Twilio outage = total outage |
| **Static routing rules** | Hard-code country→carrier mapping | Predictable | Cannot respond to carrier outages; no quality feedback; requires manual updates |
| **Least-cost routing (LCR)** | Always select cheapest carrier per destination | Minimizes cost | Cheapest carriers often have lower quality; DLR rates suffer |
| **Waterfall routing** | Primary carrier; on failure, try next in priority list | Simple failover | Adds latency on primary failure; does not use quality signals |
| **Adaptive routing with quality scoring (selected)** | Dynamic routing based on quality score (DLR-derived) + cost; auto-failover | Balances cost and quality; self-healing | More complex; requires quality score feedback loop; eventual consistency on scores |

**Selected approach — Adaptive routing with quality scoring:**

The routing table stores per-`(MCC, MNC, message_type)` routes with a `quality_score` (0-100) maintained by a feedback loop. The Router applies the following decision algorithm:

```
route_score(route) = (quality_score × quality_weight) - (cost_per_part × cost_weight)
  where quality_weight = 0.7 (configurable), cost_weight = 0.3
```

Routes with `quality_score < 40` are excluded (below quality floor). Among the remaining, the highest `route_score` wins.

**Quality score update (DLR feedback loop):**

A Flink streaming job reads from the `delivery_events` Kafka topic. For each 5-minute window, per `(carrier_id, mcc, mnc)`:
- DLR delivery rate = `delivered / submitted`
- Latency P90 = 90th percentile of `(dlr_received_at - submitted_at)`

Quality score formula:
```
quality_score = (delivery_rate × 80) + (latency_score × 20)
where latency_score = 100 if p90_latency < 30s, 80 if < 60s, 50 if < 120s, 0 if > 120s
```

Updated quality scores written to Postgres (source of truth) and Redis (cache). Redis key: `route_quality:{carrier_id}:{mcc}{mnc}` updated every 5 minutes. Router reads from Redis (< 1 ms); Postgres write is asynchronous.

**Automatic failover:**

If a carrier returns SMPP `ESME_RMSGQFUL` (message queue full) or `ESME_RTHROTTLED` more than 10 times per minute: (1) Circuit breaker opens for that carrier + destination. (2) Router automatically re-routes to the next best route using the same score algorithm (now excluding the circuit-breaker-tripped carrier). (3) In-flight messages on the failed carrier that did not receive a `submit_sm_resp` within 5 seconds are re-enqueued to Kafka with the failed carrier excluded from routing options.

**Country-specific routing requirements:**

| Country | Special requirement |
|---|---|
| India | DLT (Distributed Ledger Technology) registration required; all templates pre-approved with TRAI; sender ID is a 6-character code (e.g., CPNAME); use carrier with DLT integration |
| US | 10DLC registration required for long codes; A2P 10DLC Campaign Registry ID must be included in routing metadata |
| EU | GDPR — message content cannot be stored in non-EU data centers; route through EU carrier; store message body in EU Cassandra DC only |
| China | Cannot deliver international SMS directly; requires ICP-licensed domestic carrier partner; content restrictions apply |
| Australia | Alphanumeric sender IDs supported; ACMA Do Not Call register must be checked for marketing |

These country-specific rules are stored as metadata in the `carrier_routes` table and enforced by the Router before dispatch.

**Implementation detail — SMPP connection management:**

SMPP v3.4 is a binary protocol over TCP. Each carrier provides an SMPP endpoint with credentials (system_id + password). Connections are long-lived (persistent TCP sessions). Key SMPP operations:
- `bind_transceiver`: Establish session for both submit and receive.
- `submit_sm`: Send a message; response `submit_sm_resp` includes `message_id` for DLR correlation.
- `deliver_sm`: Carrier sends DLR or inbound MO message; respond with `deliver_sm_resp`.
- `enquire_link` / `enquire_link_resp`: Keepalive every 30 seconds.

Each SMPP Dispatcher pod maintains a pool of SMPP sessions per carrier. Session pool size = `carrier_throughput_tps / sessions`; AT&T direct: 100 TPS → 5 sessions of 20 TPS each. Session health monitored via `enquire_link`; if no response within 10 s, session reconnected (SMPP bind again).

**Interviewer Q&As:**

Q: How do you handle the case where a carrier acknowledges a message (submit_sm_resp success) but never sends a DLR?
A: Some carriers have DLR delivery rates of only 70-80% (DLRs not guaranteed by all networks). Mitigation: (1) Set a DLR timeout per message (configurable per carrier; typical: 5 minutes for transactional, 24 hours for marketing). If no DLR received within the timeout, mark the message status as `unknown` (not `failed`). (2) The quality score feedback loop counts `unknown` statuses as soft-failures — DLR rate = `delivered / (submitted - unknown)`. (3) For OTP use cases where the user didn't receive the code, provide a "resend" button in the UI — this triggers a new SMS (not a retry). (4) Some carriers provide a status query API (`query_sm` in SMPP or HTTP status endpoint for HTTP carriers); implement per-carrier polling for OTP messages if DLR is not received within 60 s.

Q: How do you route a message when you don't know the recipient's carrier (number portability)?
A: Phone numbers can be ported between carriers (mobile number portability — MNP). The number's MCC/MNC prefix is no longer reliable. Resolution: (1) Use a Number Portability Lookup service (Twilio Lookup API, Neustar, or GSMA's network-based routing). For each destination number, query the HLR (Home Location Register) or an MNP database to get the actual current carrier. Cache the result per number: `hlr:{number}` in Redis with TTL = 24 hours (portability changes are rare). (2) Some tier-1 aggregators (Twilio, Sinch) handle MNP lookups internally — when routing through them, pass the destination number and let them route based on portability. (3) For cost-sensitive routing decisions, absorb the HLR lookup cost (~$0.001/lookup) as part of the routing infrastructure — it's justified if it prevents misrouting fees (carriers charge extra for wrongly-routed messages).

Q: How would you implement A/B testing on carrier routes to validate quality improvements?
A: Add a `routing_experiment_id` column to `carrier_routes`. When multiple active routes exist for an `(MCC, MNC, message_type)` with an experiment flag set, use probabilistic routing: route 90% of messages to the primary carrier and 10% to the challenger. Track DLR rates, latency, and cost separately per carrier per experiment. After 1,000 messages per variant (sufficient for statistical significance at 95% confidence for ±5% DLR rate difference), compare quality scores and promote the winner as the new primary. The experiment sampling uses `hash(sms_id) % 100 < experiment_pct` for determinism (same message always routes to the same carrier in a given experiment).

Q: What is gray routing and how do you protect against it?
A: Gray routing (SIM box fraud) occurs when a fraudulent intermediary delivers your A2P (application-to-person) messages through consumer SIM cards, avoiding carrier A2P surcharges. Your message is delivered, but the carrier receives none of the revenue, leading them to block the gray-route traffic — causing delivery failures. Gray routes are identifiable by: (1) Suspiciously low price ($0.001/msg vs. market rate of $0.007+ for US). (2) DLR rates that look artificially high (SIM boxes self-report delivery). (3) Unusual routing patterns (messages going through unexpected countries). Mitigation: (1) Require carriers to provide ISO 27001 certification and GSMA compliance. (2) Monitor for geographic routing anomalies (US-destined message going through Eastern Europe). (3) Set minimum price thresholds per country below which routes are rejected. (4) Use test numbers to probe actual delivery quality; gray routes often fail to deliver to real numbers eventually.

Q: How do you handle country-specific sender ID requirements (e.g., India requires a 6-character alphabetic sender ID registered with TRAI)?
A: For India: (1) The `sender_numbers` table stores the DLT-approved sender ID (e.g., `CPNAME`) with `country_code = 'IN'`, `number_type = 'alphanumeric'`, and `campaign_registry_id = '<DLT_TM_ID>'`. (2) The Router, when dispatching to an Indian mobile number (MCC 404/405), selects a carrier with DLT integration (e.g., Tata Communications, Vodafone India). (3) The SMPP `submit_sm` PDU includes the DLT template ID in the UDH (User Data Header) or SMPP optional parameter `0x1400` (carrier-specific). (4) Template validation at upload time for India: the rendered message must match the pre-approved DLT template exactly (dynamic variables only within approved ranges). Failure to comply results in carrier rejection (SMPP error `ESME_RINVDESTADDR` or similar).

---

### 6.2 Long Message Splitting (Concatenation) and Character Encoding

**Problem it solves:**
GSM networks have a hard limit of 160 7-bit characters (or 70 16-bit UCS-2 characters) per SMS part. A message like "Your appointment with Dr. Smith is confirmed for 2:30 PM on Thursday, April 15, 2026 at 123 Main Street. Reply CONFIRM or CANCEL." is 166 characters, requiring 2 parts. The split must be done with UDH (User Data Header) so the handset reassembles the message correctly.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Reject messages > 160 chars** | Return error; force producers to truncate | Simplest | Unusable for most real-world content |
| **Silent truncation** | Truncate at 160 chars | Simple | Truncated messages confuse recipients; data loss |
| **Naive line split** | Split at character limit, no UDH | Simple | Handsets display as separate messages, out of order; no reassembly |
| **UDH concatenation (selected)** | Standard GSM UDH; handsets reassemble into single message | Correct behavior; universal support | Reduces usable chars per part (153 for GSM-7 or 67 for UCS-2); reference number management |

**Selected approach — UDH concatenation:**

The SMS Ingest Service (or Router before dispatch) performs encoding detection and splitting:

**Step 1 — Encoding detection:**

GSM-7 character set covers standard ASCII + European-specific characters (£, €, è, é, etc.) plus an extended table accessed via ESC prefix (takes 2 characters: `{`, `}`, `[`, `]`, `\`, `^`, `~`, `|`, `€`). If the message contains only GSM-7 characters → use GSM-7 (160 chars/part, or 153 for multipart). If the message contains any character outside GSM-7 (emoji, CJK characters, Arabic, etc.) → use UCS-2 (70 chars/part, or 67 for multipart).

**Step 2 — Part count calculation:**

```
function calculateParts(body, encoding):
  if encoding == 'GSM7':
    extended_chars = count chars that need ESC prefix (each counts as 2 toward limit)
    effective_length = len(body) + extended_chars
    if effective_length <= 160: return 1
    parts = ceil(effective_length / 153)
  else (UCS-2):
    if len(body) <= 70: return 1
    parts = ceil(len(body) / 67)
  return parts
```

**Step 3 — UDH construction:**

For multipart messages, each SMS part includes a User Data Header:
```
UDH structure (6 bytes):
  0x05         - UDH length (5 bytes following)
  0x00         - Information Element ID: Concatenated Short Message
  0x03         - IE length (3 bytes)
  reference_number  - 1 byte (0-255); same for all parts of one message
  total_parts  - 1 byte (max 255 parts)
  part_index   - 1 byte (1-indexed)
```

`reference_number` must be unique per sender-destination pair in the time window where the handset might be reassembling messages. Implementation: Redis counter `concat_ref:{from_number}:{to_number}` with `INCR` and `MOD 256` (1-byte reference). TTL = 5 minutes (well beyond typical delivery time). If the reference number rolls over within 5 minutes for a single sender-destination pair, the handset may misassemble — this would require 256 multipart messages to the same number in 5 minutes, an unrealistic rate.

For carriers that support 16-bit reference numbers (UDH extended concatenation, IE ID `0x08`): use a 2-byte reference (0-65535) — eliminates the rollover concern.

**Step 4 — SMPP PDU construction:**

Each part is a separate `submit_sm` PDU with:
- `data_coding`: 0x00 for GSM-7; 0x08 for UCS-2.
- `esm_class`: 0x40 (UDH indicator set) for multipart parts.
- `short_message`: UDH bytes + message body segment (binary).
- `udhi` flag set.

The Router stores all part `message_id`s from `submit_sm_resp` and associates them with the parent `sms_id`. DLR correlation: when a DLR for a part `message_id` arrives, the DLR Processor looks up the parent `sms_id` and records the part-level delivery. The overall message is `delivered` only when all parts have received a `delivered` DLR.

**Implementation detail — character limit awareness in templates:**

Template validation at creation time: the SMS Ingest Service estimates maximum rendered length by substituting maximum-length placeholders for all variables. If the maximum rendered length exceeds 160 characters and `allow_multipart` is not explicitly set to `true` on the template, template creation is rejected. This prevents producers from accidentally sending expensive multipart messages without intending to.

**Interviewer Q&As:**

Q: A user's name is "Zoë" — does this require UCS-2 encoding and how does that affect cost?
A: The character "ë" (e with umlaut) is in the GSM-7 character set (it is part of the extended Latin GSM alphabet — code 0x0B in GSM-7 basic character set). However, the special character "ö" — the letter in "Zoë" — is actually "e with diaeresis" which IS in GSM-7. So "Zoë" can be sent as GSM-7. However, "🎉" (party popper emoji) is not in GSM-7 and requires UCS-2. When a message includes emoji, the entire message must be UCS-2 — you cannot mix encodings within a single SMS part. A message of "Your code is 394721 🎉" (28 chars including emoji) requires UCS-2 encoding and can fit in a single 70-char UCS-2 part. A message of "Happy New Year! 🎉 Your verification code for your account is 394721. Valid for 5 min." is 87 UCS-2 characters → requires 2 parts (ceil(87/67) = 2), doubling cost. Template design should avoid emoji in transactional OTP messages.

Q: What happens if a multipart message's parts are delivered out of order?
A: The receiving handset's SMS stack handles reassembly based on the UDH reference number, total parts, and part index. The handset buffers all received parts and displays the reassembled message once all parts arrive. Part ordering is determined by `part_index` in the UDH, not by delivery order. This is a standard feature of every modern handset (any phone from the last 15+ years). If a part is lost in transit, the handset either displays the available parts (with a gap) or waits indefinitely depending on the OS. Our DLR tracking will show a partial delivery (some parts delivered, one failed) — the overall message status is `partial_failure`.

Q: How do you handle the reference number for a very high-volume sender? E.g., sending 256 multipart messages to the same number in under 5 minutes?
A: This is pathological — no legitimate use case requires sending 256 distinct multipart messages to a single number within 5 minutes (that would be ~1 multipart message every 1.2 seconds, a clear frequency cap violation). The frequency cap Redis sliding window would block this before it reaches the splitting logic. If somehow this happened (e.g., a bug): the 257th message would reuse reference number 0 (mod 256 rollover). If earlier messages from reference 0 are still being reassembled on the handset, the handset's SMS stack may merge parts incorrectly. The solution is: (1) Use 16-bit reference numbers (supported by most modern carriers via UDH extended concatenation, IE ID `0x08`) — makes rollover at 65,536 messages, effectively impossible. (2) The frequency cap (at most 5 messages per number per hour) provides a hard guard.

Q: How do you decide between sending a long message as a multipart SMS vs. sending a short URL with a landing page?
A: The "link truncation" pattern: instead of sending 300-char transactional message (2 parts), send a 120-char message with a shortened URL that opens a web page with the full content. Pros: single-part SMS (lower cost); better formatting on the landing page; tracking. Cons: users are trained not to click unknown links in SMS (anti-phishing awareness); the landing page creates an additional infrastructure dependency; some corporate firewalls block the short URL domain. Decision heuristic: (1) OTPs: always inline — never use a link for security codes. (2) Order confirmations: use URL if > 2 parts needed. (3) Appointment reminders: use URL if > 1 part (rich content benefits from the landing page). (4) Marketing messages: prefer URL for tracking + engagement. The API supports `options.use_short_url_for_long_messages: true` which triggers URL generation if the message exceeds 1 part.

Q: How do you handle split messages when the recipient's carrier doesn't support UDH?
A: UDH support is near-universal on GSM networks (defined in 3GPP TS 23.040, mandated since 1996). However, some very old CDMA networks (pre-3G US carriers) or specialized IoT SIMs may not support UDH. Mitigation: (1) Maintain a per-carrier/per-network `supports_udh` flag in the `carriers` table. (2) For networks without UDH support, fall back to separate plaintext messages with manual numbering: "[1/2] First part..." and "[2/2] Second part..." — crude but functional. (3) Alternatively, use the carrier's own long-message support if they have a proprietary mechanism (some carriers accept messages up to 1,600 characters via a carrier-specific SMPP optional parameter or HTTP API and handle splitting server-side). (4) In practice, this fallback is needed for < 1% of global traffic and primarily for legacy M2M networks.

---

### 6.3 Two-Way SMS, STOP Keyword Handling, and Opt-out Management

**Problem it solves:**
Regulations (TCPA in the US, GDPR in EU, CASL in Canada) require that recipients can opt out of marketing SMS by replying STOP (and regional variants: ARRET, CANCEL, END, QUIT, UNSUBSCRIBE). The opt-out must be honored immediately. Two-way SMS also enables conversational use cases (appointment confirmations, customer support) and short-code keyword campaigns.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Carrier-managed STOP handling** | Let the carrier handle STOP replies; rely on carrier to suppress | Simple; no engineering effort | Carrier suppression may not sync to our database; we may try to send and get an error; no visibility into opt-out state |
| **Application-layer STOP handling (selected)** | Receive inbound SMS, detect STOP keywords in our system, write to opt-out DB immediately | Full control; sub-5 s suppression; auditability; extend to custom keywords | Requires MO (mobile-originated) routing infrastructure |
| **Hybrid** | Both carrier-level and application-level | Defense in depth | Duplicate suppression handling complexity |

**Selected approach — Application-layer with carrier cooperation:**

The STOP handling flow:

1. Recipient replies "STOP" to `+18005550123` (a toll-free sender number).
2. Carrier delivers the inbound SMS as an SMPP `deliver_sm` PDU to our SMPP connection pool (the same persistent connection is both submit and receive with `bind_transceiver`).
3. The SMPP session handler detects this is an MO message (not a DLR — distinguished by `source_addr` and `dest_addr` being the recipient's number and our short/long/TF code respectively, with no `receipted_message_id` optional parameter).
4. The Inbound SMS Handler publishes to Kafka `sms.inbound` topic.
5. The Inbound SMS Processor: (a) Normalizes the body to uppercase, trims whitespace. (b) Checks against STOP keyword list (per CTIA guidelines): STOP, STOPALL, UNSUBSCRIBE, CANCEL, END, QUIT. (c) If keyword matched: write opt-out to Redis immediately (`SET sms_optout:{hash(to_number)}:{hash(from_number)} 1 NX`) and to Postgres asynchronously. Send mandatory auto-response: "You have successfully been unsubscribed from [Company] messages. You will not receive any more messages from this number. Reply START to resubscribe."
6. Any subsequent attempt to send to that `(from_number, to_number)` pair is suppressed at the Ingest Service level (Redis check, < 1 ms).

**Regional STOP keyword variants:**

| Country / Language | Additional STOP keywords |
|---|---|
| France | ARRET, ARRÊT, STOPPER |
| Germany | STOPP, ENDE |
| Spain | PARAR, BAJA |
| Brazil | PARAR, CANCELAR, SAIR |
| US/Canada | STOP, STOPALL, UNSUBSCRIBE, CANCEL, END, QUIT |
| UK | STOP (same as US; PECR-mandated) |

All variants stored in a Redis set `stop_keywords:{language_code}` loaded from Postgres on startup.

**UNSTOP / re-consent:**

When a user replies START (or UNSTOP, SUBSCRIBE, RESUME): (1) Check that an opt-out record exists. (2) If yes: set `opt_in_at = NOW()` in Postgres; delete Redis opt-out key. (3) Auto-response: "You have been re-subscribed to [Company] messages. Reply STOP to unsubscribe." The re-consent via inbound START is considered "express consent" under CTIA guidelines — sufficient for marketing messages if the original subscription used express written consent.

**Opt-out scoping:**

The opt-out is scoped to the `(to_number, from_number)` pair — a user who STOPs one short code is not automatically opted out of all company numbers. However: (1) Global opt-out option: if the user STOPs `STOPALL`, global opt-out is applied across all sender numbers from the platform. (2) The Ingest Service checks both the specific `(to, from)` opt-out AND the global opt-out.

**Webhook delivery for two-way SMS:**

Non-keyword inbound messages are routed to the registered webhook for the sender number. Webhook delivery:
1. Publish to Kafka `sms.inbound` with webhook_url from sender number config.
2. Webhook Dispatcher picks up; HTTP POST to `webhook_url` with HMAC-SHA256 signature header `X-SMS-Signature: sha256=<hex>`.
3. Expect HTTP 200 within 5 seconds; if timeout or non-200: retry with exponential backoff (3 retries: 30 s, 60 s, 120 s).
4. After 3 failures: mark webhook `failed`; alert sender number owner.
5. Inbound SMS buffered in Cassandra for 30 days regardless of webhook delivery status.

**Interviewer Q&As:**

Q: What if a user sends STOP to a short code, but the company has 5 different short codes? Are they opted out from all?
A: By default, STOP is scoped to the specific sender number. Per CTIA guidelines, if a customer messages STOP to short code A, they are opted out from messages sent from short code A. They can still receive messages from short code B. However: (1) If the company chooses to offer platform-wide opt-out (STOPALL keyword), any message to any company number opts out from all. (2) Best practice (and Gmail's 2024 sender requirement equivalent for SMS): a "brand-level" opt-out that applies across all of a company's numbers — implemented by treating the `producer_id` as the scope of the opt-out rather than just the individual number. The API supports `opt_out_scope: 'sender_number' | 'producer' | 'platform'` in the STOP response configuration. The specific behavior is configurable per producer per regulatory jurisdiction.

Q: How do you handle a race condition where the system is dispatching an SMS at the exact same instant a STOP comes in?
A: The race condition window is: opt-out received and being written to Redis → SMPP `submit_sm` already sent to carrier. Two sub-cases: (1) `submit_sm` sent but not yet `submit_sm_resp` received: we cannot recall the message; it will be delivered. This is an accepted race condition — the window is < 100 ms (time between Redis write and SMPP submission completing). CTIA and TCPA do not impose strict real-time requirements; they allow a "reasonable" window. (2) Message in Kafka, not yet consumed: the opt-out write to Redis happens before Kafka consumption — the Redis check at dispatch time catches this and discards the Kafka message. Defense: at dispatch time (router/dispatcher), the Router re-checks the opt-out Redis key before calling `submit_sm` (double-check pattern). This reduces the race window to sub-millisecond.

Q: How do you comply with TCPA's express written consent requirement for marketing SMS?
A: TCPA requires "prior express written consent" for marketing SMS — consent obtained at the point of data capture (web form, paper form, verbal IVR with recording). System requirements: (1) Store consent records in a `sms_consent` table (Postgres) with `user_id`, `phone_number`, `consent_timestamp`, `consent_source` (web form URL, form version), `consent_text` (exact text the user agreed to), `ip_address`, and `user_agent`. (2) Before any marketing SMS, the Ingest Service verifies consent exists for that `(to_number, producer_id)`. (3) Consent is immutable — once stored, it cannot be modified, only superseded by a new consent record or revoked by STOP. (4) Consent records are retained for 4+ years (TCPA statute of limitations is 4 years). (5) The API exposes `consent_id` as a required field for marketing SMS — the producer must pass the ID of the stored consent record; if absent or expired, the send is rejected.

Q: How do you handle toll-free numbers vs. short codes for two-way SMS in the US — what are the throughput implications?
A: US sender number throughput comparison:

| Sender type | Throughput | Registration | Cost |
|---|---|---|---|
| Short code (dedicated) | 100 SMS/s | 8-12 weeks (CTIA) | $500-$1,000/month |
| Short code (shared) | Variable; carrier-limited | Varies | $20-$100/month |
| Toll-free (TFN) | 3 SMS/s (can be increased to 100/s with TFN verification) | 2-4 weeks (Twilio/Bandwidth) | $2-$10/month |
| 10DLC long code | 1 SMS/s (can be increased) | 2-4 days (TCR) | $1-$3/month |

For OTP (high throughput transactional): Use dedicated short codes (100 SMS/s). For marketing (lower volume, 2-way): Use toll-free or 10DLC. The sender number assignment at Ingest Service selects the appropriate number based on message type and required throughput, using a round-robin assignment across numbers of the same type to distribute load within the throughput limit.

Q: How would you implement an SMS chatbot / keyword campaign (e.g., text PROMO to 12345)?
A: Keyword campaigns are managed via the Inbound SMS Handler's keyword routing table in Postgres: `(short_code, keyword, action, response_template, campaign_id)`. When an inbound SMS matches a keyword (case-insensitive, trimmed): (1) Record the interaction in `inbound_sms`. (2) Add the `from_number` to the campaign's subscriber list (if consent language was in the keyword campaign's call-to-action). (3) Send the `response_template` as an outbound SMS via the normal send path. (4) Optionally trigger a webhook to a backend service for complex chatbot flows (multi-step conversations). The keyword matching is done before general webhook routing; keywords take precedence. For conversational flows (multi-turn): maintain session state in Redis keyed by `(from_number, short_code)` with TTL = 30 minutes (inactivity timeout).

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling strategy | Bottleneck handled |
|---|---|---|
| **API Gateway** | Stateless; auto-scale on RPS/CPU | Inbound traffic spikes |
| **SMS Ingest Service** | K8s HPA on CPU; stateless | Opt-out check, template render throughput |
| **Kafka** | Add brokers; 100 partitions each for transactional and marketing topics; RF=3 | Message throughput and durability |
| **Router / Dispatcher pods** | K8s HPA on Kafka consumer lag; each pod holds 5-10 SMPP sessions | Message dispatch throughput |
| **SMPP connection pool** | Connections are per pod; scale pods; negotiate higher SMPP bind counts with carriers | Carrier throughput limits |
| **Inbound SMS Handler** | Stateless Kafka consumers; auto-scale on inbound Kafka lag | STOP processing, webhook delivery |
| **DLR Processor** | Stateless Kafka consumers | DLR ingest throughput |
| **Cassandra** | Add nodes; RF=3; LCS compaction | SMS record and event storage |
| **Redis** | Redis Cluster; 5 shards; shard by phone number hash | Opt-out check, frequency cap throughput |
| **Webhook Dispatcher** | Stateless HTTP workers with per-domain connection pools | Inbound webhook delivery |

### DB Sharding

Cassandra shards natively by partition key. `sms_messages` partitioned by `sms_id` (UUID) — uniform distribution; no hotspot. `delivery_events` partitioned by `sms_id` — co-located with the message for efficient per-SMS queries. `sms_by_number` partitioned by `(to_number, month_bucket)` — month bucketing prevents unbounded partition growth for a high-volume number.

PostgreSQL `carrier_routes` and `sender_numbers`: small datasets; sharding not needed. PostgreSQL `sms_opt_outs`: up to 50 M rows but small (200 B each = 10 GB); fits comfortably with Postgres table partitioning by `to_number` hash (10 partitions) for parallel query performance.

### Caching

| Cache layer | Data | TTL | Size |
|---|---|---|---|
| Redis | Opt-out state per `(to_number, from_number)` | Permanent (explicit invalidation on UNSTOP) | ~10 GB (50 M entries × 200 B) |
| Redis | Carrier routing table per MCC+MNC | 5 minutes (updated by routing quality job) | ~5 MB (500 routes × 10 KB) |
| Redis | Per-number frequency cap (sliding window) | Dynamic (window size) | Small |
| Redis | SMPP `enquire_link` session health per carrier | 60 s | Negligible |
| Local process cache | GSM-7 character set lookup table | Permanent (static data) | < 1 KB |
| Local process cache | Compiled template objects | 15 min; LRU | ~10 MB per pod |

### Replication

- **Kafka**: RF=3; `min.insync.replicas=2`; producer `acks=all`. Message durability guaranteed.
- **Cassandra**: RF=3 per DC; two DCs (us-east-1, eu-west-1). EU users' opt-out and SMS data written only to EU DC (GDPR).
- **PostgreSQL**: Primary + 1 synchronous standby (RPO=0) + 3 async read replicas. Patroni failover.
- **Redis**: 3 replicas per shard; AOF `everysec`.

### Interviewer Q&As — Scaling

Q: Your SMPP connection pool can send 5,000 SMS/s but a single carrier connection is limited to 100 TPS. How do you scale beyond 100 TPS to a single carrier?
A: Multiple SMPP sessions to the same carrier. Most carriers support 10-50 simultaneous `bind_transceiver` sessions per system_id. At 100 TPS per session × 50 sessions = 5,000 TPS from one carrier. For higher volume: (1) Negotiate more sessions with the carrier (enterprise SMPP agreements). (2) Distribute across multiple carrier accounts (system_id pairs). (3) Use multiple carrier connections to different carriers — for US, split AT&T-destined traffic across the primary carrier and a secondary tier-1 aggregator. Each SMPP session is maintained by a dedicated goroutine (Go) or thread (Java) in the dispatcher pod; scaling sessions means scaling pods.

Q: How do you ensure the opt-out check doesn't become a bottleneck at 5,000 RPS?
A: At 5,000 RPS, the opt-out check requires 5,000 Redis reads/s. A single Redis node handles 100,000+ ops/s. With 5 Redis shards (sharded by phone number hash), each shard handles 1,000 read ops/s — far below capacity. The actual constraint is network latency (1 round-trip per check, ~0.3 ms for a Redis read in the same data center). At 5,000 RPS × 0.3 ms = 1.5 Gbps-equivalent requests — entirely manageable. Additionally, ingest pods can use Redis pipelining to batch multiple reads in a single TCP round-trip when processing bursts.

Q: How would you handle a flash campaign that needs to send 10 M SMS in 1 hour (far above normal peak)?
A: 10 M / 3,600 s = 2,778 SMS/s — below our designed peak of 5,000/s. If the campaign were 10 M in 10 minutes (16,667/s): (1) Pre-scale: trigger K8s HPA 30 minutes before campaign launch to scale Router/Dispatcher pods to handle 20,000/s (2x headroom). (2) Pre-negotiate carrier throughput: some carriers require advance notice for high-volume campaigns — contact the carrier 24 hours ahead; most tier-1 aggregators support burst capacity with notice. (3) Use campaign scheduling to start the send in waves (e.g., stagger by timezone as discussed in push notifications). (4) The marketing Kafka topic has sufficient retention; messages queued in Kafka drain over 30 minutes even at carrier capacity limits — the API returns 202 immediately. (5) After the campaign, scale dispatcher pods back down on lag = 0.

Q: How do you detect and respond to a carrier routing loop (your message goes from Carrier A → Carrier B → Carrier A)?
A: Routing loops are a gray-route / wholesale carrier issue. Detection: (1) Monitor DLR latency — routing loops manifest as extremely high delivery latency (minutes to hours vs. sub-minute expected). (2) Monitor `unknown` DLR rate for a carrier — looped messages often never deliver and no final DLR is received. (3) Implement a TTL in the SMPP `submit_sm` PDU via the `validity_period` field — if the message is still looping after the TTL, the final carrier rejects it and sends a failed DLR. (4) Some carriers include routing path information in DLR optional parameters; inspect these for repeated carrier codes. Response: if routing loop detected for a `(carrier_id, MCC, MNC)` route: disable that route in the routing table (set `is_active = false`); re-route affected messages to the next best carrier; file a dispute with the carrier.

Q: How does 10DLC (10-Digit Long Code) registration affect your system design in the US?
A: 10DLC requires all A2P (application-to-person) SMS sent from 10-digit long codes to be registered with The Campaign Registry (TCR). Registration components: (1) **Brand registration**: Company registered once with TCR (~$4 one-time fee). (2) **Campaign registration**: Each use case registered (~$10-15/month per campaign). Campaign types: 2FA, customer care, marketing, etc. (3) **Phone number association**: Each 10DLC long code must be associated with a registered campaign. System changes: (a) `sender_numbers` table includes `campaign_registry_id` for each 10DLC number. (b) Ingest Service validates that the selected sender number's campaign_registry_id matches the `sms_type` and `use_case` of the send request. (c) Unregistered 10DLC sends are filtered by carriers (they return `ESME_RREJECTMSG`). (d) Throughput for 10DLC: default 1 TPS/number; increases with registration tier up to 100 TPS for special campaigns. Our system assigns multiple 10DLC numbers (round-robin) for high-throughput campaigns.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| **SMPP carrier connection drop** | Messages queued; not dispatched | SMPP enquire_link timeout (30 s); connection error | Immediate reconnect with exponential backoff (1 s, 2 s, 4 s, max 30 s); in-flight messages re-queued to Kafka; circuit breaker opens after 5 consecutive reconnect failures |
| **Carrier rejection (SMPP 5xx error)** | Message not delivered | SMPP `submit_sm_resp` with error code | Classify error: retryable (ESME_RTHROTTLED → retry with backoff) vs. permanent (ESME_RINVDESTADDR → suppress number); re-route retryable to alternative carrier |
| **Primary carrier outage** | All messages to affected destinations fail | DLR failure rate spike + SMPP error rate | Automatic re-routing to fallback carrier (routing table switch via Redis); human alert after 5 minutes if fallback not available |
| **Kafka broker failure** | Message loss if < RF replicas | Broker health check | RF=3 prevents loss; producer blocks on `acks=all` until 2 replicas ack |
| **Ingest Service pod crash** | In-flight requests fail | Health check; K8s restart | Stateless; Kafka message not committed; reprocessed on restart |
| **Redis opt-out cache miss** | Opt-out check falls through to Postgres | Redis error rate | Postgres fallback for opt-out check (adds 5 ms latency); fail-closed on Redis error (reject send if can't verify opt-out) |
| **DLR not received** | Message status stuck at "submitted" | DLR timeout alarm after carrier SLA window | Mark as `unknown` after timeout; expose via API; producer triggers resend if needed |
| **Multipart reassembly failure on handset** | User sees partial message | — (not directly observable) | UDH with part index; handset OS handles reassembly; use 16-bit reference numbers to avoid rollover; monitor `partial_failure` DLR rate |
| **Inbound STOP not processed** | User continues to receive messages after opt-out | Complaint rate spike | STOP processing is the highest-priority Kafka consumer; dedicated topic with 0 lag SLO; Redis write is synchronous before any confirmation |
| **International delivery failure (carrier blocked)** | Messages to specific country not delivered | DLR failure rate by country | Automatic re-routing to alternative carrier for that country; notify admin if no alternative exists; return `undeliverable` to producer |

### Retry Policy

| Error | Retry | Max duration | Notes |
|---|---|---|---|
| SMPP ESME_RTHROTTLED | Exponential + jitter | Until TTL | Start 1 s, double each retry, max 60 s |
| SMPP ESME_RMSGQFUL | Exponential | Until TTL | Queue full; back off 5 s, 10 s, 20 s |
| SMPP ESME_RINVDESTADDR | No retry | — | Permanent; suppress number |
| SMPP ESME_RINVSRCADR | No retry | — | Invalid sender ID; alert admin |
| Network timeout | Reconnect; re-enqueue message | 3 reconnect attempts | |
| HTTP carrier 429 | Exponential | Until TTL | Honor `Retry-After` header |
| HTTP carrier 5xx | Exponential | Until TTL | 3 retries per carrier; then re-route |
| Cassandra write failure | Retry 3x; 50 ms apart | — | After 3 failures, dead-letter queue |

### Idempotency

Before calling `submit_sm`, the dispatcher checks: `SET NX sms_dispatch:{sms_id}:{part_index} 1 EX 3600` in Redis. If NX fails (already dispatched), skip. This prevents duplicate SMS on Kafka re-delivery after consumer rebalance. The `sms_id` + `part_index` combination uniquely identifies each SMPP submission.

### Circuit Breaker

Per carrier per destination region: Resilience4j state machine.
- **Closed**: Normal; track failure rate.
- **Open**: > 40% of SMPP submits fail or time out over 5-minute window. All submits to this carrier for this region fast-fail; messages re-routed immediately.
- **Half-Open**: After 60 s, allow 5 probe submits. If > 3 succeed, close. Otherwise re-open.

Separate circuit breakers: per `(carrier_id, country_code)` for granular region-level isolation.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert threshold | Meaning |
|---|---|---|---|
| `sms.send.rps` | Counter | — | Ingest throughput |
| `sms.send.latency_p99_ms` | Histogram | > 200 ms | Ingest API slowness |
| `sms.kafka.transactional_lag_s` | Gauge | > 10 s | OTP backlog (critical) |
| `sms.kafka.marketing_lag_s` | Gauge | > 300 s | Marketing queue backup |
| `sms.dispatch.submit_rate` | Counter | — | SMPP submission throughput |
| `sms.dlr.delivery_rate_by_carrier` | Gauge | < 80% per carrier | Carrier quality issue |
| `sms.dlr.latency_p90_s_by_carrier` | Histogram | > 60 s | Carrier latency issue |
| `sms.dispatch.smpp_error_rate` | Gauge | > 2% | SMPP errors spike |
| `sms.dispatch.unknown_dlr_rate` | Gauge | > 10% | DLR not confirmed |
| `sms.optout.suppressions_per_min` | Counter | > 1,000/min | STOP flood; reputation issue |
| `sms.optout.redis_error_rate` | Gauge | > 0.1% | Redis health critical |
| `sms.carrier.circuit_breaker_state` | Enum | OPEN | Carrier outage |
| `sms.inbound.processing_lag_ms` | Histogram | > 1,000 ms | STOP processing delay |
| `sms.multipart.part_count_p95` | Histogram | > 3 parts avg | Template content issues |
| `sms.cost.usd_per_min` | Gauge | > 150% of baseline | Unexpected cost spike |
| `sms.carrier.routing_quality_score` | Gauge | < 50 for any carrier | Route quality degradation |
| `sms.number.throughput_utilization` | Gauge | > 90% per number | Throughput limit approaching |

### Distributed Tracing

OpenTelemetry spans:
1. `sms.ingest.handle_request` — covers validation, opt-out check, template render.
2. `sms.optout.redis_check` — Redis read; attribute: `opt_out_status`.
3. `sms.template.render` — template render; attributes: `template_id`, `part_count`, `encoding`.
4. `kafka.produce.sms` — Kafka publish; attributes: `topic`, `partition`, `offset`.
5. `sms.router.select_carrier` — routing decision; attributes: `selected_carrier`, `route_score`, `alternatives_considered`.
6. `sms.smpp.submit` — SMPP `submit_sm` call; attributes: `carrier_id`, `mcc`, `mnc`, `smpp_error_code`, `carrier_message_id`, `part_index`.
7. `sms.dlr.process` — DLR processing; attributes: `carrier_status`, `dlr_latency_s`.
8. `sms.inbound.process` — Inbound SMS; attributes: `keyword_detected`, `opt_out_written`, `webhook_sent`.

Trace context propagated via Kafka message headers (W3C TraceContext). Enables end-to-end trace from API call to DLR receipt in Jaeger.

### Logging

Structured JSON logs. Key fields: `sms_id`, `carrier_id`, `to_country` (not full number), `smpp_error_code`, `trace_id`.

PII handling: `to_number` logged only as `sha256(to_number)` in application logs (except STOP processing logs which are access-controlled audit logs). Message body never logged in application logs; stored encrypted at rest in Cassandra.

Log levels:
- **ERROR**: SMPP connection failure, carrier rejection (permanent), Cassandra write failure, STOP processing failure.
- **WARN**: SMPP throttle, soft carrier error, DLR timeout, routing fallback triggered.
- **INFO**: SMS submitted, DLR received, STOP processed, campaign start/complete.
- **DEBUG**: SMPP PDU details, routing decision details (disabled in production).

SMPP PDU debug logging (enabled only on demand via feature flag): captures raw PDU bytes (redacted: message content replaced with `[REDACTED]`) for carrier troubleshooting without PII exposure.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B (selected) | Reason |
|---|---|---|---|
| Carrier integration | Single aggregator (Twilio only) | Carrier aggregation (SMPP direct + HTTP adapters) | Twilio-only: single point of failure; no cost optimization; at scale ($0.0075/msg × 10 M/day = $75K/day vs. direct carrier $0.003-0.005 = $30-50K/day); aggregation provides resilience and cost savings |
| Routing strategy | Static country→carrier mapping | Adaptive quality-score routing | Static routing cannot respond to carrier outages or quality degradation; quality-score routing is self-healing; adds Flink streaming job complexity but reduces manual routing maintenance |
| Opt-out enforcement layer | Carrier-level STOP handling only | Application-layer + carrier (defense in depth) | Carrier handling is opaque; we cannot audit when a STOP was honored; application layer gives auditability, immediate Redis write, sub-5-second enforcement, and regulatory compliance evidence |
| Long message handling | Reject messages > 160 chars | UDH concatenation with automatic splitting | Rejecting forces producers to handle splitting (inconsistent results); UDH concatenation is the industry standard; handsets handle reassembly seamlessly |
| SMPP vs. HTTP carrier integration | HTTP only (Twilio, Sinch REST APIs) | SMPP for direct carriers + HTTP adapters for REST APIs | SMPP is required for direct carrier connections (lower cost); HTTP APIs are better for tier-2 aggregators that don't offer SMPP; adapter pattern abstracts both |
| DLR reliability | Trust carrier DLRs as ground truth | DLR + timeout + `unknown` state | Carriers don't always send DLRs (60-80% coverage for some networks); timeout + unknown state prevents messages being stuck in "submitted" forever; producers can trigger resend logic |
| Frequency cap | Time-based fixed windows | Redis sliding-window sorted set | Fixed windows create boundary effects (user gets 2x cap at window boundary); sliding window is accurate; same approach as push notifications for consistency |
| Message body storage | Store full body in SQL with message record | Body in Cassandra (TTL-managed) separate from metadata | SQL row size grows; body is variable-length (up to 1,600 chars for concatenated); TTL-based Cassandra storage provides automatic cleanup; metadata searchable in Postgres without body overhead |
| Short code vs. 10DLC for US marketing | Short codes (fast, established) | 10DLC long codes (new standard, lower cost) | Short codes: $500-1,000/month per number; 10DLC: $1-3/month; at scale with many campaigns, 10DLC is significantly cheaper; short codes retained for highest-throughput OTP use cases |
| STOP scope | Stop from individual sender number | Stop from individual sender number + global STOPALL | Industry standard (per CTIA) is per-number scope; global scope via explicit STOPALL keyword reduces user confusion across multi-number campaigns without violating CTIA guidelines |

---

## 11. Follow-up Interview Questions

Q1: How would you implement OTP (one-time password) delivery with automatic fallback from SMS to voice call?
A: Define a `fallback_chain` in the notification request: `["sms", "voice"]`. The OTP service calls the SMS API with `fallback_enabled: true` and a `fallback_timeout_s: 60`. After sending the SMS, the OTP service starts a timer. If the user has not confirmed the code within 60 seconds (tracked in the OTP service), it calls the Voice API with the same code (a text-to-speech voice call saying "Your verification code is 3, 9, 4, 7, 2, 1"). Voice calls have higher delivery success on phone numbers with weak data signals (rural areas). The SMS system's responsibility is to report DLR status; the OTP service monitors this and triggers voice fallback. No changes to the SMS gateway are needed — the fallback logic is in the OTP service layer, using the DLR webhook.

Q2: How do you handle number lookup (HLR query) and what information does it provide?
A: An HLR (Home Location Register) query to a carrier's signaling network returns: (1) Number validity — is the number currently active/in service? (2) Current MCC/MNC — which network the number is currently roaming on (important for ported numbers). (3) Roaming status — is the number roaming internationally (may affect delivery reliability). (4) Number type — mobile, landline, VoIP (VoIP numbers don't receive SMS). The SMS Ingest Service runs an HLR lookup for new numbers on first contact: `hlr_lookup:{number}` in Redis with 24-hour TTL. If HLR indicates `landline` or `inactive` → suppress immediately with reason `invalid_number`. HLR lookup costs $0.001-0.005 per query; at 5,000 RPS but mostly repeat numbers, the effective rate is much lower (cache hit rate > 95% for active users).

Q3: How would you implement message scheduling with timezone-aware delivery across millions of recipients in a campaign?
A: Same approach as push notifications: (1) Campaign operator specifies `send_at_local_time = "10:00 AM"`. (2) The Campaign Scheduler groups recipients by UTC offset (from their stored profile timezone or inferred from phone number country). (3) For each UTC offset group, calculate the UTC timestamp when 10 AM local occurs. (4) Publish per-UTC-offset campaign tasks to Kafka with `deliver_after = <utc_timestamp>`. (5) The marketing Kafka topic consumer checks the `deliver_after` metadata before routing; if not yet due, the message is re-queued with a 60-second retry delay (not ideal for Kafka — prefer a Redis delay queue for scheduling precision). (6) For SMS, respect quiet hours: no marketing SMS between 9 PM and 8 AM local time (TCPA best practice, though TCPA technically allows 8 AM to 9 PM for marketing calls — SMS follows similar norms).

Q4: How do you prevent SMS pumping fraud (also known as SMS toll fraud or IRSF)?
A: SMS pumping fraud: attackers submit phone numbers they control (or premium-rate numbers they own) to trigger OTP-sending applications, generating revenue from the A2P termination fee. Detection and prevention: (1) **Anomaly detection**: Sudden spike in OTP requests to specific country codes (e.g., 10x normal rate to a specific MCCMNC) — alert and auto-pause OTP sends to that destination. (2) **Velocity limits per session/IP**: If the same IP or user session requests > 3 OTPs within 1 hour, block. (3) **Geographic pattern analysis**: OTP requests for a user whose profile indicates US location but destination is +254 (Kenya) — flag for review. (4) **SMS to premium-rate numbers**: Block sending to known premium-rate number ranges (maintained by carriers and available via HLR classification). (5) **Carrier-level filtering**: Some carriers offer fraud screening APIs; integrate where available. (6) **Cost alerting**: Real-time cost per destination monitoring; if cost to destination X exceeds $X/hour baseline, pause and alert.

Q5: How would you design a delivery receipt SLA monitoring system?
A: Define per-carrier DLR SLAs (e.g., AT&T transactional: 95% DLR within 30 s; international tier-2: 80% DLR within 120 s). Implementation: (1) At message submission, store `submitted_at` and `expected_dlr_by = submitted_at + carrier_sla_seconds` in Cassandra. (2) A Flink streaming job consuming the `delivery_events` topic tracks per-carrier, per-destination-country DLR rates in 5-minute tumbling windows. (3) A separate `DLR Timeout Checker` job runs every minute, queries `WHERE status = 'submitted' AND expected_dlr_by < NOW()` from Cassandra, and marks these as `unknown`. (4) ClickHouse stores per-carrier DLR rate timeseries; Grafana dashboard shows current vs. SLA. (5) PagerDuty alert fires if any carrier's DLR rate drops below SLA threshold for 2 consecutive 5-minute windows.

Q6: How do you handle international SMS regulations that require content pre-approval (e.g., India's DLT)?
A: India's TRAI Distributed Ledger Technology (DLT) mandate requires: (1) All business entities (PEs) register on a DLT platform (Jio, Vodafone India, BSNL, etc.). (2) All message templates pre-registered and approved (typically takes 24-72 hours). (3) Each outbound SMS must include the DLT template ID. System design impact: (a) `email_templates` equivalent for SMS templates includes `dlt_template_id` and `dlt_approval_status`. (b) Template validation at creation time: if `destination_country = 'IN'`, require DLT template ID. (c) The Router, when dispatching to Indian numbers, includes the DLT template ID in the SMPP `submit_sm` TLV optional parameter (0x1400 for Airtel, similar for other Indian carriers). (d) Dynamic content in DLT templates is strictly controlled — only specific positions allow variable data (e.g., `{#var#}` placeholders); the Router validates that the rendered message matches the approved template structure. (e) Carriers reject SMS that don't include a valid DLT template ID or whose content doesn't match — monitor for `ESME_RREJECTMSG` errors from Indian carriers.

Q7: How do you handle message delivery to a phone number that has been ported to a different carrier since our routing table was last updated?
A: Number portability is handled via: (1) **HLR pre-send query** (recommended for high-value messages): query the HLR 24 hours before sending (cached); get current carrier MCCMNC; use this for routing. HLR responses reflect portability because the carrier's signaling network knows the current serving network. (2) **DLR-based adaptive routing**: if a message sent to "AT&T network" is rejected with `ESME_RINVDESTADDR` or gets no DLR, it may be a portability issue — re-route to a different carrier or via an aggregator that handles MNP internally. (3) **Carrier-reported portability in DLR**: some carriers include the actual destination carrier in the DLR's optional parameters when the number is ported; update the routing cache based on this. (4) **Aggregator routing**: Tier-1 aggregators (Twilio, Sinch) internally handle MNP — passing a ported number to them results in correct delivery. Using an aggregator as a fallback ensures ported numbers are reached even if our routing table is stale.

Q8: How would you design a cost attribution and billing system for internal SMS usage?
A: (1) Each SMS carry `producer_id` and `campaign_id`. (2) The Delivery Event Processor records `cost_units` (per the carrier's billing model — typically 1 unit per SMS part) alongside each `submitted` event. (3) A ClickHouse streaming aggregation accumulates cost by `(producer_id, campaign_id, carrier_id, destination_country, date)` in near-real-time (5-minute windows). (4) A monthly batch job generates billing reports per producer: total SMS sent × carrier rate = total cost; broken down by type (transactional vs. marketing). (5) Internal chargeback: reports are fed to the finance system for internal team chargebacks. (6) Budget alerts: each producer configures a monthly SMS budget cap; when 80% is consumed, alert is sent; at 100%, further marketing SMS from that producer are blocked (transactional overrides the cap). (7) Multi-currency: carrier costs are in USD; for internal reporting in other currencies, apply a monthly average FX rate from a financial data provider.

Q9: How do you handle Unicode emoji in SMS when they require UCS-2 encoding?
A: UCS-2 is a 16-bit encoding supporting the Basic Multilingual Plane (BMP) — code points U+0000 to U+FFFF. Most common emoji (😀, ❤️, ✅) are in the BMP (U+1F600 etc.) — but wait, U+1F600 is ABOVE U+FFFF (it's in the Supplementary Multilingual Plane). Standard UCS-2 cannot encode emoji above U+FFFF. Resolution: (1) Use UTF-16 encoding instead of strict UCS-2 — UTF-16 can encode BMP characters in 2 bytes and supplementary characters in 4 bytes (surrogate pairs). SMPP supports this via `data_coding = 0x08` which is "UCS-2 (ISO/IEC 10646)" — most modern handsets handle surrogate pairs even if the encoding byte says UCS-2. (2) Alternative: emoji substitution at template render time — replace common emoji with text equivalents (❤️ → "(heart)") for transactional messages where emoji are decorative. (3) For marketing messages where emoji drive engagement: use UTF-16 with surrogate pairs; account for the fact that a 4-byte emoji counts as 2 UCS-2 code units, reducing the per-part character limit from 70 to 69 for messages containing supplementary emoji.

Q10: What are the tradeoffs between short codes, toll-free numbers, and 10DLC for a US B2C messaging program?
A: Decision framework based on use case:

| Use Case | Recommended | Reasoning |
|---|---|---|
| OTP / 2FA (high volume) | Dedicated short code | 100 TPS/code; high consumer recognition; fastest delivery; $500-1,000/month justified by volume |
| Marketing campaigns | 10DLC (multiple numbers) | $1-3/month/number; carriers required to support since 2023; sufficient throughput with multiple numbers |
| Customer support / two-way | Toll-free number | Consumer-friendly (free to call/text); 3-10 TPS; recognizable format |
| Low-volume transactional | Toll-free or 10DLC | Based on throughput need |
| International | Local long code or aggregator | Short codes are country-specific; local numbers improve deliverability |

For a large B2C program: use dedicated short codes for OTP (volume + trust), 10DLC pool for marketing (cost), toll-free for two-way customer support (consumer experience). This multi-number strategy requires the sender number assignment logic at Ingest to be aware of the use case + required throughput and select appropriately.

Q11: How do you ensure that a STOP reply from a shared short code is correctly associated with the right sender?
A: Shared short codes (multiple companies sharing one 5-6 digit code) are being deprecated in the US (carriers stopped supporting new shared short codes in 2021; existing ones being migrated). However, for existing setups: (1) Shared short codes route inbound SMS based on the `keyword` prefix — each company registers a unique keyword (e.g., Company A: ACME, Company B: BETA). Users text "STOP ACME" to opt out from Company A or "STOP BETA" for Company B. (2) The Inbound SMS Handler parses the keyword prefix to identify the sender before processing the opt-out. (3) Best practice today: migrate from shared short codes to dedicated short codes or 10DLC. The system supports both; the `sender_numbers` table has `is_shared` flag. (4) CTIA 2022 guidelines require all shared short code users to transition — the migration effort is tracked in the `sender_numbers` table with `migration_status`.

Q12: How do you validate phone numbers before sending (E.164 format validation)?
A: Three validation tiers at ingest time: (1) **Format validation**: The E.164 format is `+[country_code][subscriber_number]`, maximum 15 digits. Validated with Google's `libphonenumber` library (available in Go, Java, Python) — the industry standard. `libphonenumber` validates format, detects fake numbers, identifies line type (mobile, fixed, toll-free), and normalizes format. Reject if `libphonenumber.IsValidNumber()` returns false. (2) **Line type check**: `libphonenumber.GetNumberType()` — reject landlines for SMS sends (with configurable override for voice applications). (3) **HLR live lookup** (optional, for transactional high-value sends): query the live network to confirm the number is active and currently reachable. Skip HLR for bulk marketing (cost prohibitive for 50 M sends). Results cached 24 hours in Redis. False rejection rate with `libphonenumber`: < 0.1% for correctly formatted numbers. False acceptance rate: < 1% (very rarely accepts truly unreachable numbers).

Q13: How would you implement SMS whitelisting for a test environment to avoid sending real messages?
A: (1) **Sandbox mode**: The `sms_type = 'sandbox'` flag or `options.dry_run = true` processes the entire pipeline (validation, opt-out check, template render, part count calculation, routing table lookup) but does not call `submit_sm` on the SMPP connection. Returns a synthetic `sms_id` and simulated `delivered` DLR after 5 seconds. (2) **Whitelist mode**: In non-production environments (staging, QA), configure a `ALLOWED_PHONE_NUMBERS` environment variable. Any send to a non-whitelisted number is silently suppressed (logs at INFO level). Only numbers in the whitelist are actually delivered. (3) **Test carrier**: A test SMPP server (Kannel's fake SMSC, or `smppsim`) runs in the test environment; the SMPP connection pool connects to this fake SMSC instead of real carriers. Messages are accepted and fake DLRs sent. (4) **Phone number masking**: For integration tests, substitute real phone numbers with test numbers (Twilio test credentials return `+15005550006` as a valid test number).

Q14: How do you measure and optimize SMS delivery speed (time from API call to handset)?
A: The SMS delivery timeline has these components: (a) API acceptance (our control): target < 100 ms. (b) Kafka queue → SMPP submit (our control): target < 500 ms for transactional. (c) Carrier network processing (not our control): 1-30 s depending on carrier. (d) Radio network delivery to handset (not our control): depends on signal, device sleep state. Optimization opportunities: (1) Minimize Kafka queue time: dedicated high-throughput Kafka partitions for OTP; dispatchers with low `max.poll.interval.ms` for low-latency consumption. (2) SMPP window size: SMPP supports an asynchronous window (submit_sm without waiting for submit_sm_resp) — set `window_size = 100` to allow 100 in-flight PDUs per session, dramatically increasing throughput without adding latency. (3) Carrier selection: prioritize carriers with lowest P90 DLR latency (from quality score feedback loop). (4) Measure: instrument the full timeline with OpenTelemetry; track `sms_submitted_at` (our SMPP call) and `dlr_received_at` (DLR ingest) to compute carrier delivery latency. Surface as Grafana metric `sms.carrier.delivery_p90_s` per carrier per country.

Q15: What are the key differences between A2P (application-to-person) and P2P (person-to-person) SMS from a carrier perspective, and why does it matter?
A: P2P SMS is individual-to-individual messaging via a smartphone app (iMessage, WhatsApp, or native dialer). A2P SMS is software-to-person (our system). Carrier distinctions: (1) **Throughput**: P2P is throttled to ~1-3 SMS/s per number; A2P on registered short codes/10DLC supports 100+ SMS/s. (2) **Price**: Carriers charge higher termination rates for A2P ($0.003-0.008/msg) vs. P2P (included in consumer plans). (3) **Filtering**: Carriers apply different spam filters to P2P vs. A2P; A2P from registered numbers is less aggressively filtered. (4) **Compliance**: A2P requires campaign registration (10DLC, short code provisioning); P2P does not. Why it matters for our system: all our sends are A2P; we must register as A2P senders. Attempting to use consumer SIM cards (SIM farms) to send A2P traffic at P2P rates is a violation of carrier ToS and federal regulations (TCPA), and results in carrier blocking. Our SMPP connections are A2P-registered system IDs; this is validated by the carrier at bind time.

---

## 12. References & Further Reading

1. SMPP v3.4 Specification: https://smpp.org/SMPP_v3_4_Issue1_2.pdf
2. 3GPP TS 23.040 — Technical realization of the Short Message Service (SMS): https://www.3gpp.org/dynareport/23040.htm
3. CTIA Messaging Principles and Best Practices: https://api.ctia.org/wp-content/uploads/2023/05/230523-CTIA-Messaging-Principles-and-Best-Practices-FINAL.pdf
4. The Campaign Registry (TCR) — 10DLC: https://www.campaignregistry.com
5. GSMA — SMS for Machine-to-Machine (M2M) & A2P Messaging: https://www.gsma.com/iot/resources/sms-for-iot-overview/
6. RFC 5724 — URI Scheme for GSM Short Message Service: https://datatracker.ietf.org/doc/html/rfc5724
7. Twilio — SMS Best Practices: https://www.twilio.com/docs/sms/guidelines/best-practices
8. Bandwidth — A2P 10DLC Compliance Guide: https://support.bandwidth.com/hc/en-us/categories/200851978-A2P-SMS
9. TRAI DLT Guidelines (India): https://www.trai.gov.in/consultation-paper/consultation-paper-unsolicited-commercial-communication
10. FCC TCPA Compliance Regulations: https://www.fcc.gov/consumers/guides/stop-unwanted-robocalls-and-texts
11. Google libphonenumber: https://github.com/google/libphonenumber
12. Sinch — SMS Encoding Guide (GSM-7 vs. UCS-2): https://developers.sinch.com/docs/sms/api-reference/encoding
13. Spryng — SMPP UDH Concatenation Guide: https://www.spryng.nl/en/building-sms-applications/
14. Nexmo (Vonage) — Delivery Receipts Documentation: https://developer.vonage.com/messaging/sms/guides/delivery-receipts
15. GSMA — Mobile Number Portability: https://www.gsma.com/services/mobile-number-portability/
16. Kannel — Open Source WAP and SMS Gateway: https://www.kannel.org/
17. InfoBip — SMS Fraud Prevention Guide: https://www.infobip.com/security/fraud-prevention
