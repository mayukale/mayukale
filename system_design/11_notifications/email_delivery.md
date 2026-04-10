# System Design: Email Delivery System

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Send transactional emails** — order confirmations, password resets, receipts, OTPs — with low latency (sub-5-second delivery to provider).
2. **Send marketing/bulk emails** — newsletters, promotional campaigns, product announcements — to large user lists (up to 50 M recipients per campaign).
3. **Email template management** — create, version, and render templates using variable substitution; support HTML + plain-text multipart.
4. **Unsubscribe management** — honor one-click unsubscribe (RFC 8058), manage per-category suppression lists, expose unsubscribe links in all marketing emails.
5. **Bounce handling** — process hard bounces (permanent invalid addresses) and soft bounces (temporary delivery failures); automatically suppress hard-bounced addresses.
6. **Spam complaint handling** — receive and process ISP feedback loop (FBL) complaints; automatically suppress complainants.
7. **Delivery tracking** — record sent, delivered, bounced, complained, opened, and clicked events; expose analytics.
8. **DKIM, SPF, and DMARC** — authenticate all outbound email so ISPs accept and deliver it.
9. **Sending domain and IP reputation management** — rotate IPs per campaign type; warm up new IPs; monitor blocklist status.
10. **Email scheduling** — send immediately or schedule for a future timestamp; support time-zone-aware scheduling.
11. **Attachment support** — send PDFs, images (up to 10 MB total) as attachments (transactional only; attachments banned for bulk/marketing).

### Non-Functional Requirements

1. **Scale**: 100 M active email addresses; peak bulk send of 50 M emails in 4 hours; transactional throughput of 50,000 emails/min peak.
2. **Latency**: Transactional emails handed to SMTP relay within 2 seconds of API call; delivered to recipient mailbox within 5 minutes (P95, dependent on ISPs).
3. **Availability**: 99.99% uptime for the send API.
4. **Durability**: Zero email loss after API acceptance; at-least-once delivery semantics with idempotency to prevent duplicates.
5. **Deliverability**: Inbox placement rate > 95% for transactional; > 85% for marketing. Hard-bounce rate maintained < 2%; spam complaint rate < 0.1% (Gmail/Yahoo thresholds).
6. **Compliance**: CAN-SPAM Act compliance (unsubscribe within 10 business days — target immediate); GDPR email consent tracking; CASL compliance for Canadian recipients.
7. **Security**: No PII in logs; email content encrypted in transit (TLS 1.2+); API credentials never in email headers.
8. **Reputation resilience**: Automatic IP rotation away from blocked IPs; blocklist monitoring with alerting within 15 minutes.

### Out of Scope

- Inbound email parsing / IMAP/POP3 inbox features.
- End-to-end encryption (S/MIME, PGP) — referenced but not implemented.
- Customer-facing email clients or webmail UI.
- Email thread / conversation management.

---

## 2. Users & Scale

### User Types

| Actor | Description |
|---|---|
| **Producer (internal service)** | Microservice calling the email API (e.g., auth service for OTPs, order service for receipts). |
| **Marketing operator** | Internal user creating campaigns, uploading recipient lists, scheduling bulk sends. |
| **End user (recipient)** | Person whose email address receives the email; can click unsubscribe. |
| **Admin** | Manages sending domains, IP pools, DKIM keys, suppression list overrides. |
| **Analyst** | Queries delivery and engagement analytics dashboards. |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 100 M active email addresses in the system.
- Transactional emails: 50,000/min peak = ~833 RPS peak.
- Marketing campaigns: 2 per week, each sending to 50 M recipients; complete within 4 hours.
- Average email size: 50 KB (HTML + images via external CDN links, not inline; attachments rare).
- Event ingest: for each sent email, expect delivery event + possibly open/click events.
- Open rate: 25% of delivered; click rate: 3% of delivered.

| Metric | Calculation | Result |
|---|---|---|
| Transactional peak RPS (send API ingest) | 50,000 / 60 s | ~833 RPS |
| Transactional daily volume | 833 RPS × 86,400 s × 0.3 (not sustained at peak) | ~21.6 M/day |
| Marketing send rate (bulk) | 50 M recipients / 4 hours / 3,600 s | ~3,472 RPS during campaign |
| Peak combined RPS (campaign + transactional) | 3,472 + 833 | ~4,305 RPS |
| SMTP relay throughput required | 4,305 emails/s × 50 KB avg | ~215 MB/s egress |
| Delivery events (sent) | 21.6 M transact + 50 M marketing (2/wk) = ~36.3 M/day avg | ~420 events/s avg |
| Open events | 36.3 M/day × 25% | ~9 M/day = ~104/s |
| Click events | 36.3 M/day × 3% | ~1.1 M/day = ~13/s |
| Bounce events (hard + soft) | 36.3 M/day × 3% combined bounce rate | ~1.1 M/day = ~13/s |
| FBL complaint events | 36.3 M/day × 0.05% | ~18,000/day = ~0.2/s |
| Total event ingest RPS | 420 + 104 + 13 + 13 + 0.2 | ~550 events/s avg; ~3,000/s peak |

### Latency Requirements

| Operation | Target |
|---|---|
| API acceptance (POST /emails) | P99 < 200 ms |
| Transactional email → SMTP relay | P99 < 2 s |
| Transactional email → recipient inbox | P95 < 5 minutes (ISP-dependent) |
| Marketing email: campaign send start | < 60 s after scheduled time |
| Unsubscribe effective | < 5 s (before any further sends) |
| Bounce suppression (hard bounce received) | < 30 s from bounce event receipt |
| Blocklist alert | < 15 minutes from IP listing |
| Delivery event query (analytics) | Data available within 2 minutes of event |

### Storage Estimates

| Data | Size per record | Count / Retention | Total |
|---|---|---|---|
| Email records (metadata only, not body) | 1 KB | 36.3 M/day × 90 days | 3.3 TB |
| Email body/HTML (template-rendered) | 50 KB | 36.3 M/day × 7 days hot | 12.7 TB |
| Delivery events | 200 B | 550 RPS × 86,400 × 90 days | 856 GB |
| Suppression list (bounces + complaints) | 200 B | 100 M addresses (worst case) | 20 GB |
| Unsubscribe records | 150 B | 100 M addresses | 15 GB |
| Templates | 100 KB avg (HTML + text) | 50,000 templates | 5 GB |
| Campaign records | 2 KB | 1 M campaigns | 2 GB |
| DKIM keys | 2 KB per key | 1,000 sending domains | 2 MB |
| **Total hot storage (90-day)** | | | ~17 TB |

Email bodies older than 7 days are archived to S3 (compressed ~5:1 → ~2.5 TB/week incremental). After 90 days, only metadata and events retained.

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Ingest API ingress | 4,305 RPS × 1 KB (metadata; body stored separately) | ~4.3 MB/s peak |
| Email body ingress (bulk campaign upload) | 4,305 RPS × 50 KB | ~215 MB/s peak (campaign window) |
| SMTP egress to ISPs | 4,305 RPS × 50 KB average email size | ~215 MB/s peak |
| Event ingress (tracking pixels, clicks) | 3,000 events/s × 500 B | ~1.5 MB/s |
| ISP feedback / bounce ingress | ~100 bounce messages/s peak | < 1 MB/s |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                         PRODUCER SERVICES                             │
│        (Auth Svc, Order Svc, Marketing UI, Campaign Scheduler)        │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ HTTPS REST
                               ▼
              ┌────────────────────────────────────┐
              │     API Gateway / Load Balancer      │
              │  (TLS termination, auth, rate limit) │
              └────────────────┬───────────────────┘
                               │
               ┌───────────────▼──────────────────────┐
               │         Email Ingest Service           │
               │  - Validate request                    │
               │  - Check suppression list (Redis)      │
               │  - Check unsubscribe status (Redis)    │
               │  - Resolve template / render HTML      │
               │  - Assign sending IP / domain          │
               │  - Generate Message-ID, DKIM sign stub │
               │  - Publish to Kafka (priority routing) │
               └─────────┬──────────────┬──────────────┘
                         │              │
              ┌──────────▼────┐  ┌──────▼─────────────┐
              │ Kafka:         │  │ Kafka:              │
              │ transactional  │  │ marketing / bulk    │
              │ (high-priority)│  │ (low-priority)      │
              └──────┬─────────┘  └───────┬─────────────┘
                     │                    │
        ┌────────────▼──────┐   ┌─────────▼──────────────────┐
        │  SMTP Relay Pool   │   │   Campaign Throttle Worker  │
        │  (Postfix / custom)│   │  - Reads bulk Kafka topic   │
        │  - Signs DKIM      │   │  - Enforces send rate       │
        │  - Selects IP pool │   │  - Calls SMTP Relay Pool    │
        │  - TLS to ISP MX   │   │  - Respects ISP rate limits │
        │  - Handles SMTP    │   └──────────┬─────────────────┘
        │    responses       │              │
        └────────────┬───────┘              │
                     └──────────┬───────────┘
                                │
          ┌─────────────────────▼──────────────────────────────┐
          │             Delivery Event Pipeline                   │
          │                                                       │
          │  ┌───────────────────┐   ┌────────────────────────┐  │
          │  │  Bounce Processor │   │  FBL Complaint Handler │  │
          │  │  (SMTP DSN / MX)  │   │  (ISP feedback loops)  │  │
          │  └────────┬──────────┘   └──────────┬─────────────┘  │
          │           │                          │                 │
          │  ┌────────▼──────────────────────────▼─────────────┐  │
          │  │          Suppression List Updater                │  │
          │  │  - Writes to Redis (immediate) + Postgres (durable) │
          │  └──────────────────────────────────────────────────┘  │
          │                                                         │
          │  ┌─────────────────────────────────────────────────┐   │
          │  │    Open / Click Tracking (pixel + redirect)      │   │
          │  │  - 1x1 GIF pixel server (CDN-fronted)            │   │
          │  │  - Click redirect proxy (logs click, redirects)  │   │
          │  └──────────────────────────┬──────────────────────┘   │
          └─────────────────────────────┼──────────────────────────┘
                                        │
          ┌─────────────────────────────▼──────────────────────────┐
          │                       Data Stores                        │
          │  ┌────────────┐  ┌─────────────┐  ┌─────────────────┐  │
          │  │ Email Meta │  │ Suppression │  │  Delivery Events │  │
          │  │ (Postgres) │  │ (Redis +    │  │  (Cassandra +   │  │
          │  │            │  │  Postgres)  │  │   ClickHouse)   │  │
          │  └────────────┘  └─────────────┘  └─────────────────┘  │
          │  ┌────────────┐  ┌─────────────┐                        │
          │  │ Templates  │  │ Campaigns   │                        │
          │  │ (Postgres) │  │ (Postgres)  │                        │
          │  └────────────┘  └─────────────┘                        │
          └─────────────────────────────────────────────────────────┘
                                        │
          ┌─────────────────────────────▼──────────────────────────┐
          │            Analytics & Reputation Monitoring             │
          │  (ClickHouse → Grafana; Blocklist Monitor; SenderScore) │
          └─────────────────────────────────────────────────────────┘
```

**Component roles:**

- **API Gateway**: Terminates TLS, enforces per-producer rate limits, validates OAuth2 tokens.
- **Email Ingest Service**: Stateless; validates payload, checks suppression/unsubscribe, renders template, assigns sending IP/domain based on email type and sender reputation, publishes to Kafka.
- **Kafka (transactional topic)**: High-priority queue, small TTL (15 min — transactional emails that can't be sent quickly are useless), aggressively consumed.
- **Kafka (marketing topic)**: Low-priority queue, large TTL (48 hours), campaign throttle workers enforce per-ISP rate limits.
- **SMTP Relay Pool**: Postfix instances (or custom Go SMTP client) with multiple IP addresses per pool; performs DKIM signing, establishes TLS connections to ISP MX servers, processes SMTP response codes.
- **Campaign Throttle Worker**: Enforces ISP-specific send rates (e.g., Gmail allows ~100 k/day/IP for new senders; Yahoo has different limits). Uses a token-bucket per `(IP, ISP)` pair stored in Redis.
- **Bounce Processor**: Listens on a dedicated MX address for bounce-back emails (DSN — Delivery Status Notifications) and FBL complaints; parses RFC 3464 DSN messages to extract bounce type and address.
- **FBL Complaint Handler**: Receives ISP feedback loop emails (Gmail, Outlook, Yahoo) via a dedicated inbox; parses RFC 5965 ARF format to extract the complained-about address.
- **Suppression List Updater**: Writes hard-bounce and complaint addresses to Redis (immediate hot cache) and Postgres (durable); checked synchronously during ingest.
- **Open/Click Tracking**: Pixel server (1x1 GIF) fronted by CDN; click redirect proxy; both emit events to Kafka for event pipeline processing.
- **ClickHouse**: OLAP for delivery funnel analytics; receives events from Kafka via Flink.

**Primary use-case data flow (transactional):**

1. Order Service POST `/v1/emails` with `{to, template_id: "order_confirmation", data: {order_id, total}}`.
2. Ingest Service validates request; checks `to` address against Redis suppression list (< 1 ms).
3. Template rendered server-side: HTML body generated with Handlebars, tracking pixel inserted, links rewritten via click-tracking proxy.
4. Sending IP selected from transactional IP pool (dedicated IPs, not shared with marketing).
5. DKIM signature computed; Message-ID generated (`<uuid@sendingdomain.com>`).
6. Published to `emails.transactional` Kafka topic.
7. SMTP Relay worker picks up within 100 ms; establishes SMTP/TLS connection to recipient's MX server.
8. Email sent; SMTP 250 OK response → write `sent` event to Cassandra.
9. ISP delivers to inbox; if delivery confirmation available (via SMTP DSN), write `delivered` event.
10. Recipient opens email → tracking pixel fires → pixel server writes `opened` event to Kafka.

---

## 4. Data Model

### Entities & Schema

```sql
-- =============================================
-- Emails (PostgreSQL)
-- =============================================
CREATE TABLE emails (
    email_id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    producer_id         TEXT        NOT NULL,
    idempotency_key     TEXT        UNIQUE,
    email_type          TEXT        NOT NULL,  -- 'transactional' | 'marketing' | 'operational'
    campaign_id         UUID,                   -- null for transactional
    template_id         UUID,
    "from"              TEXT        NOT NULL,  -- "Company Name <noreply@company.com>"
    reply_to            TEXT,
    to_address          TEXT        NOT NULL,  -- single recipient; campaigns expand from list
    subject             TEXT        NOT NULL,
    message_id          TEXT        UNIQUE NOT NULL,  -- RFC 5322 Message-ID
    sending_ip          INET,
    sending_domain      TEXT,
    body_s3_key         TEXT,                  -- S3 key where rendered HTML body stored
    status              TEXT        NOT NULL DEFAULT 'queued',
    -- 'queued'|'sending'|'sent'|'delivered'|'bounced'|'complained'|'suppressed'
    priority            INT         NOT NULL DEFAULT 5,  -- 1=highest, 10=lowest
    scheduled_at        TIMESTAMPTZ,
    sent_at             TIMESTAMPTZ,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    ttl_at              TIMESTAMPTZ,
    metadata            JSONB
);

CREATE INDEX idx_emails_campaign ON emails(campaign_id) WHERE campaign_id IS NOT NULL;
CREATE INDEX idx_emails_to ON emails(to_address);
CREATE INDEX idx_emails_status_created ON emails(status, created_at);
CREATE INDEX idx_emails_idempotency ON emails(idempotency_key) WHERE idempotency_key IS NOT NULL;

-- =============================================
-- Delivery Events (Cassandra CQL)
-- =============================================
-- Partitioned by (email_id) for per-email event lookup
-- Partitioned by (to_address_bucket, date) for per-recipient history
CREATE TABLE delivery_events_by_email (
    email_id        UUID,
    event_id        TIMEUUID,
    event_type      TEXT,   -- 'queued'|'sent'|'delivered'|'soft_bounce'|'hard_bounce'|'complained'|'opened'|'clicked'|'unsubscribed'
    smtp_response   TEXT,   -- e.g. "250 2.0.0 OK"
    bounce_type     TEXT,   -- 'hard'|'soft'|null
    bounce_code     TEXT,   -- SMTP enhanced status code e.g. "5.1.1"
    bounce_message  TEXT,
    click_url       TEXT,   -- only for 'clicked' events
    user_agent      TEXT,   -- only for 'opened'/'clicked'
    ip_address      INET,   -- source IP for open/click
    created_at      TIMESTAMP,
    PRIMARY KEY (email_id, event_id)
) WITH CLUSTERING ORDER BY (event_id DESC)
  AND default_time_to_live = 7776000;  -- 90 days

-- =============================================
-- Suppression List (PostgreSQL source of truth)
-- =============================================
CREATE TABLE suppression_list (
    address             TEXT        NOT NULL,
    suppression_type    TEXT        NOT NULL,  -- 'hard_bounce'|'complaint'|'manual'|'unsubscribe'
    category            TEXT,                  -- null=global; 'marketing'|'newsletters' etc.
    reason              TEXT,
    source_email_id     UUID,
    suppressed_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at          TIMESTAMPTZ,           -- null = permanent
    PRIMARY KEY (address, suppression_type, category)
);

CREATE INDEX idx_suppression_address ON suppression_list(address);

-- Redis representation for hot-path check:
-- Key: suppress:{sha256(lower(address))}
-- Type: Hash
-- Fields: global (1/0), marketing (1/0), newsletters (1/0), hard_bounce (1/0)
-- TTL: none (permanent until invalidated on address removal)

-- =============================================
-- Unsubscribe Records (PostgreSQL)
-- =============================================
CREATE TABLE unsubscribe_records (
    address         TEXT        NOT NULL,
    category        TEXT        NOT NULL DEFAULT 'all',  -- 'all'|'marketing'|'newsletters'|etc.
    unsubscribed_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    source          TEXT        NOT NULL,  -- 'link'|'one_click'|'api'|'admin'
    campaign_id     UUID,
    PRIMARY KEY (address, category)
);

-- =============================================
-- Templates (PostgreSQL)
-- =============================================
CREATE TABLE email_templates (
    template_id         UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    name                TEXT            NOT NULL UNIQUE,
    category            TEXT            NOT NULL,
    email_type          TEXT            NOT NULL,
    subject_template    TEXT            NOT NULL,
    html_body_template  TEXT            NOT NULL,   -- Handlebars/MJML source
    text_body_template  TEXT            NOT NULL,
    from_name           TEXT,
    reply_to            TEXT,
    version             INT             NOT NULL DEFAULT 1,
    created_by          TEXT            NOT NULL,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    is_active           BOOLEAN         NOT NULL DEFAULT TRUE,
    metadata            JSONB
);

CREATE TABLE email_template_versions (
    template_id         UUID            NOT NULL REFERENCES email_templates(template_id),
    version             INT             NOT NULL,
    html_body_template  TEXT            NOT NULL,
    text_body_template  TEXT            NOT NULL,
    subject_template    TEXT            NOT NULL,
    created_by          TEXT            NOT NULL,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (template_id, version)
);

-- =============================================
-- Campaigns (PostgreSQL)
-- =============================================
CREATE TABLE campaigns (
    campaign_id         UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    name                TEXT            NOT NULL,
    template_id         UUID            NOT NULL REFERENCES email_templates(template_id),
    segment_id          UUID,           -- references audience segment
    scheduled_at        TIMESTAMPTZ,
    started_at          TIMESTAMPTZ,
    completed_at        TIMESTAMPTZ,
    status              TEXT            NOT NULL DEFAULT 'draft',
    -- 'draft'|'scheduled'|'sending'|'completed'|'cancelled'|'paused'
    sending_domain      TEXT            NOT NULL,
    ip_pool             TEXT            NOT NULL DEFAULT 'marketing',
    total_recipients    INT,
    created_by          TEXT            NOT NULL,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    metadata            JSONB
);

-- =============================================
-- Sending Domains & IP Pools (PostgreSQL)
-- =============================================
CREATE TABLE sending_domains (
    domain              TEXT        PRIMARY KEY,
    dkim_selector       TEXT        NOT NULL,
    dkim_private_key_id TEXT        NOT NULL,  -- Vault secret path
    dkim_key_bits       INT         NOT NULL DEFAULT 2048,
    spf_record          TEXT,
    dmarc_policy        TEXT        NOT NULL DEFAULT 'quarantine',
    ip_pool             TEXT        NOT NULL,  -- 'transactional'|'marketing'|'operational'
    reputation_score    NUMERIC(5,2),
    is_active           BOOLEAN     NOT NULL DEFAULT TRUE,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE ip_pools (
    ip_address          INET        PRIMARY KEY,
    pool_name           TEXT        NOT NULL,  -- 'transactional'|'marketing'
    reputation_score    NUMERIC(5,2),
    daily_send_count    INT         NOT NULL DEFAULT 0,
    daily_send_limit    INT         NOT NULL DEFAULT 500000,
    is_warmed           BOOLEAN     NOT NULL DEFAULT FALSE,
    is_active           BOOLEAN     NOT NULL DEFAULT TRUE,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_blocklist_check TIMESTAMPTZ
);
```

### Database Choice

| Database | Strengths | Weaknesses | Fit |
|---|---|---|---|
| **PostgreSQL** | ACID, FK constraints, JSONB, mature full-text search, excellent for relational data with moderate scale | Single-writer bottleneck at extreme write rates; needs sharding/partitioning for very large tables | **Selected** for emails metadata, templates, campaigns, suppression list (source of truth), sending domains |
| **Cassandra** | High write throughput, TTL, wide-column for time-series event data | No secondary indexes at scale, limited aggregation, no ACID | **Selected** for delivery events (append-only, time-series, high write volume, TTL-based expiry) |
| **Redis** | Sub-millisecond reads, Bloom filter, sorted sets | Volatile; memory-bounded; not a primary store | **Selected** for suppression list hot cache, per-ISP rate-limit counters, idempotency keys |
| **ClickHouse** | Columnar OLAP, 10-100x aggregation speed vs. Postgres | Not OLTP; eventual consistency | **Selected** for analytics: open rates, click rates, delivery funnels per campaign |
| **S3** | Cheap, durable, scalable object store | Not queryable; cold retrieval latency | **Selected** for rendered email body archival |
| **SendGrid / SES** | Managed SMTP relay, built-in reputation management | Vendor lock-in; cost at scale; less control | **Not selected as primary** (vendor API-based delivery discussed in Deep Dive as an alternative) |

**PostgreSQL justification for emails table**: Campaigns require relational joins (campaign → emails → events). Template versioning benefits from FK constraints. Idempotency key uniqueness is enforced via PostgreSQL's UNIQUE constraint + serializable isolation level — no race conditions. The emails table is partitioned by `created_at` (monthly range partitions) to manage table size; old partitions archived to read-only tablespaces. At 36.3 M emails/day × 90 days = 3.3 B rows, table partitioning is essential.

**Cassandra justification for delivery events**: 550 events/s average with 3,000/s peak, 90-day retention → ~142 B total events. Cassandra's LSM-tree handles this append-only write pattern without index contention. TTL-based automatic deletion eliminates expensive DELETE scans. Per-email reads (`WHERE email_id = ?`) are single-partition lookups regardless of dataset size.

---

## 5. API Design

All endpoints require `Authorization: Bearer <oauth2_client_credentials_token>`. Rate limits per `producer_id`.

### Send Email

```
POST /v1/emails
Rate limit: 1,000 RPS per producer (transactional); 10 RPS per producer (bulk, use campaigns API)
Idempotency: Idempotency-Key header required

Request:
{
  "to": "user@example.com",
  "from": "Company <noreply@company.com>",    // must be a verified sending domain
  "reply_to": "support@company.com",
  "email_type": "transactional",
  "template_id": "uuid",
  "template_data": {
    "user_name": "Alice",
    "order_id": "ORD-12345",
    "total": "$49.99"
  },
  "subject": "Your order is confirmed",       // overrides template subject if provided
  "attachments": [                            // optional; transactional only
    {
      "filename": "receipt.pdf",
      "content_type": "application/pdf",
      "content_base64": "<base64-encoded-content>",  // max 10 MB total
      "s3_key": "receipts/uuid.pdf"           // alternative to inline base64
    }
  ],
  "scheduled_at": "2026-04-10T09:00:00Z",    // optional
  "ttl_seconds": 900,                         // default 900 s for transactional
  "metadata": { "order_id": "ORD-12345" },   // passthrough; appears in delivery webhooks
  "options": {
    "track_opens": true,
    "track_clicks": true,
    "sandbox": false                          // true = validate but do not send
  }
}

Response 202 Accepted:
{
  "email_id": "uuid",
  "message_id": "<uuid@noreply.company.com>",
  "status": "queued" | "suppressed",
  "suppression_reason": null | "hard_bounce" | "complaint" | "unsubscribed" | "global_opt_out"
}

Response 400 Bad Request:
{ "error": "INVALID_FROM_DOMAIN", "message": "noreply@unknown.com is not a verified sending domain" }

Response 409 Conflict:
{ "error": "DUPLICATE_REQUEST", "email_id": "uuid-of-original" }
```

### Create Campaign

```
POST /v1/campaigns
Rate limit: 10 RPS per producer

Request:
{
  "name": "April Newsletter",
  "template_id": "uuid",
  "segment_id": "uuid",           // audience segment (resolved to email list)
  "scheduled_at": "2026-04-15T14:00:00Z",
  "sending_domain": "newsletter.company.com",
  "ip_pool": "marketing",
  "options": {
    "track_opens": true,
    "track_clicks": true,
    "unsubscribe_group": "newsletters"
  }
}

Response 201 Created:
{
  "campaign_id": "uuid",
  "status": "scheduled",
  "estimated_recipients": 2500000
}
```

### Get Email Status

```
GET /v1/emails/{email_id}
Rate limit: 2,000 RPS per producer

Response 200:
{
  "email_id": "uuid",
  "message_id": "<uuid@company.com>",
  "to": "user@example.com",
  "subject": "Your order is confirmed",
  "status": "delivered",
  "sent_at": "2026-04-09T12:00:01Z",
  "delivery_events": [
    { "event_type": "sent", "created_at": "2026-04-09T12:00:01Z" },
    { "event_type": "delivered", "created_at": "2026-04-09T12:00:15Z" },
    { "event_type": "opened", "created_at": "2026-04-09T12:05:30Z", "user_agent": "Apple Mail" }
  ]
}
```

### Manage Suppression

```
POST /v1/suppressions
GET  /v1/suppressions?address={email}&type={type}
DELETE /v1/suppressions/{address}?type={type}
Rate limit: 100 RPS per producer

POST Request:
{
  "address": "user@example.com",
  "suppression_type": "manual",
  "category": "marketing",      // null = global
  "reason": "Customer request"
}
```

### Unsubscribe (public endpoint, called from email link)

```
GET /v1/unsubscribe?token={signed_jwt}&category={category}
POST /v1/unsubscribe  (RFC 8058 List-Unsubscribe-Post)
Body: List-Unsubscribe=One-Click

-- token is a short-lived JWT (7 days) containing {email_id, address, category}
-- signed with HMAC-SHA256 using a per-deployment secret
Response 200: HTML confirmation page or JSON {status: "unsubscribed"}
```

### Get Campaign Analytics

```
GET /v1/campaigns/{campaign_id}/analytics
Rate limit: 100 RPS per producer

Response 200:
{
  "campaign_id": "uuid",
  "status": "completed",
  "total_recipients": 2500000,
  "sent": 2490000,
  "delivered": 2400000,
  "soft_bounced": 45000,
  "hard_bounced": 5000,
  "complained": 1200,
  "opened": 620000,
  "clicked": 74000,
  "unsubscribed": 12000,
  "open_rate": 0.258,
  "click_rate": 0.031,
  "bounce_rate": 0.020,
  "complaint_rate": 0.0005
}
```

---

## 6. Deep Dive: Core Components

### 6.1 SMTP vs. API-Based Delivery — Architecture Choice and SMTP Relay Design

**Problem it solves:**
The fundamental question is how to deliver email to recipient ISPs: operate our own SMTP relay infrastructure (Postfix or custom SMTP client), or use a third-party API (SendGrid, Amazon SES, Mailgun). The choice determines cost, deliverability control, and operational complexity.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Third-party API only (SendGrid/SES)** | Route all emails through a vendor's API | No SMTP ops; managed deliverability; easy setup | Vendor lock-in; $0.10/1,000 emails × 36.3 M/day = $3,630/day; limited IP reputation control; vendor outages affect us |
| **In-house SMTP relay (Postfix)** | Run Postfix clusters; manage our own sending IPs | Full control; lower per-email cost; own IP reputation | Complex ops; deliverability team needed; IP warming required; MTA expertise |
| **Custom SMTP client (Go/Java)** | Write an SMTP client optimized for our access patterns | Maximum control; no Postfix config complexity; built-in metrics | High engineering investment; reinventing mature software |
| **Hybrid: own relay + vendor fallback (selected)** | Primary traffic on in-house relay; vendor API as fallback | Cost-efficient at scale; vendor provides ISP relationships for fallback; IP reputation control | Dual system complexity; needs routing logic |
| **Multi-vendor API (SendGrid + SES + Mailgun)** | Route across multiple vendors by ISP or type | Resilience; no single vendor outage; can route Gmail to SES | Still vendor-dependent; cost at scale; complex routing logic |

**Selected approach — Hybrid: In-house SMTP relay with vendor API fallback:**

At 36.3 M emails/day, vendor API cost is ~$3,600-$7,200/day depending on tier. In-house SMTP relay costs: ~50 EC2 c5.2xlarge instances ($0.34/hr each) = $408/day + bandwidth. Break-even favors in-house at this volume. Additionally, owning the relay provides: (1) control over sending IPs and reputation, (2) custom DKIM key rotation, (3) per-ISP rate shaping.

**In-house SMTP relay design:**

Each SMTP relay pod is a Postfix instance configured for:
- **Outbound TLS**: STARTTLS with `smtp_tls_security_level = may`; mandatory for major ISPs.
- **IP binding**: Each Postfix instance bound to a specific IPv4/IPv6 pair from the appropriate IP pool. Transactional emails use a dedicated IP pool (separate from marketing) to prevent bulk reputation contamination.
- **DKIM signing**: Postfix with the OpenDKIM milter; signs with the domain's 2048-bit RSA private key (stored in Vault, fetched at milter startup).
- **Connection limits**: Postfix `smtp_destination_concurrency_limit` per domain (e.g., Gmail: 100 simultaneous connections per IP; Yahoo: 20).
- **Queue management**: Postfix deferred queue for temporary failures; `maximal_queue_lifetime = 5d`; bounce DSNs generated and published to bounce processor.

**ISP-specific rate limiting:**

Each major ISP has documented or empirically determined per-IP/per-day limits:

| ISP | Approx. limit/IP/day | Notes |
|---|---|---|
| Gmail | 2,000 (cold IP) to 100,000+ (warmed IP) | Rate-responsive; reduce on 4xx |
| Yahoo/AOL | 1,000 (cold) to 50,000 (warm) | Throttle on 421 responses |
| Outlook/Hotmail | 500 (cold) to 10,000 (warm) | Uses SNDS for reputation |
| Apple iCloud | 1,000 (cold) | Less documented |

The Campaign Throttle Worker maintains a Redis token bucket per `(sending_ip, isp_domain)` with limits informed by historical acceptance rates and ISP guidelines. When a relay returns a `421` (try again later), the worker pauses that `(ip, isp)` combination for the backoff period indicated in the response.

**DKIM / SPF / DMARC implementation:**

- **SPF**: DNS `TXT` record: `v=spf1 ip4:<relay_ip_range> ip6:<ipv6_range> ~all`. Maintained automatically — when new relay IPs are added, a DNS update job updates the SPF record via the DNS API (Route 53 API in our case). SPF limit: 10 DNS lookups — use `include:` sparingly or use `ip4:` directly.
- **DKIM**: 2048-bit RSA keys per sending domain; selector format `smtp{YYYYMM}._domainkey.sendingdomain.com`. Key rotation every 6 months: new key added to DNS 48 hours before use (for DNS propagation); old key kept in DNS for 72 hours after rotation (for delayed-delivery validation). Private keys stored in Vault; OpenDKIM fetches via Vault agent sidecar.
- **DMARC**: `v=DMARC1; p=quarantine; rua=mailto:dmarc-reports@company.com; ruf=mailto:dmarc-failures@company.com; pct=100; adkim=r; aspf=r`. Aggregate reports (`rua`) processed by a DMARC report parser (open source: `parsedmarc`) and surfaced in a Grafana dashboard showing DMARC alignment rates per sending domain. Policy starts at `p=none` for new domains, graduates to `quarantine` after 30 days of clean reports, then `reject`.

**Vendor API fallback:**

When the in-house relay returns persistent 5xx errors for a destination (not a bounce — e.g., Postfix queue full, internal error), or when an IP is listed on a blocklist: route to the vendor API (SES or SendGrid) via a fallback queue in Kafka. The failover decision is made by the relay health monitor, which publishes a routing override to Redis (`relay_override:{ip_pool}:fallback = true`) that the Campaign Throttle Worker and SMTP Relay dispatcher read.

**Interviewer Q&As:**

Q: Gmail has started requiring DKIM, SPF, and DMARC for bulk senders (Google's 2024 requirements). How do you ensure compliance?
A: Google's February 2024 requirements for bulk senders (>5,000 emails/day to Gmail): (1) DKIM authentication — implemented with 2048-bit keys per sending domain. (2) SPF alignment — our SPF record includes all relay IPs; envelope-from matches the `From:` domain (aligned). (3) DMARC policy of at least `p=none` — we use `p=quarantine`, exceeding the requirement. (4) One-click unsubscribe (RFC 8058) — our `List-Unsubscribe-Post` header is included in all marketing emails; the unsubscribe endpoint processes the one-click request. (5) Spam rate below 0.10% — monitored via Google Postmaster Tools API; automated alerts at 0.05%. Compliance dashboard tracks all five dimensions per sending domain.

Q: How do you prevent your shared marketing IP pool from harming transactional deliverability?
A: Strict IP pool separation — transactional and marketing emails are assigned IPs from disjoint pools. Transactional IPs (`10.0.1.x` pool) never send marketing. Marketing IPs (`10.0.2.x` pool) never send transactional. IP assignment is enforced at the Ingest Service level based on `email_type`. If a marketing IP is listed on a blocklist or soft-blocked by an ISP, it has zero impact on transactional IPs. Additionally, transactional emails use a dedicated `From:` domain (`mail.company.com`) while marketing uses `news.company.com` — separate domain reputations.

Q: How does DKIM key rotation work without causing delivery failures?
A: DNS propagation takes up to 48 hours (TTL-dependent). Rotation procedure: (1) Generate new key pair; upload private key to Vault; create new DNS `TXT` record for new selector (e.g., `smtp202605`). Wait 48 hours for full propagation. (2) Update relay configuration to sign with the new selector. Deploy gradually (canary — 10% of relays for 1 hour). (3) Monitor DMARC reports for signing failures. (4) Complete rollout. (5) Retire old selector from DNS after 72 hours (leaving it in DNS allows ISPs with cached messages to still validate). The relay configuration stores the current selector in a ConfigMap; updating it triggers a rolling pod restart with zero downtime.

Q: Why not just use Amazon SES for everything and save the engineering complexity?
A: At 36.3 M emails/day, SES pricing is $0.10/1,000 = $3,630/day = ~$1.33 M/year in email sending costs alone (plus attachment storage and data transfer). In-house relay at 50 EC2 c5.2xlarge instances = $408/day = ~$149 K/year — a 9x cost difference. Additionally: (1) SES gives limited IP reputation control (shared IPs on standard tier; dedicated IPs add cost). (2) Vendor outages directly affect deliverability — our hybrid approach uses SES only as fallback. (3) Custom metrics and IP reputation data are not available from SES. The engineering investment (3 engineers, 6 months) pays back in < 2 months at this scale.

Q: How do you handle SMTP connection timeouts and slow ISPs (some ISPs take 30+ seconds to accept)?
A: Per-destination timeout configuration in Postfix (and in the Kafka consumer): `smtp_connect_timeout = 30s`, `smtp_data_init_timeout = 120s`. Emails to slow ISPs are sent asynchronously from the main queue — Postfix's deferred queue handles retry scheduling. For the Campaign Throttle Worker, connection attempts to an ISP that consistently times out are paused (circuit breaker per ISP in Redis) and the affected emails re-queued with an exponential backoff delay. The per-email TTL ensures that transactional emails not delivered within 15 minutes are bounced internally (not lingering in the queue for days).

---

### 6.2 Bounce Handling and Suppression List Management

**Problem it solves:**
Sending to invalid email addresses generates hard bounces that signal to ISPs that the sender has poor list hygiene — triggering spam filtering. Sending to complainants triggers FBL reports that directly increase the sender's spam rate. Both must result in immediate, durable suppression to protect deliverability and comply with regulations.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Ignore bounces** | Don't process bounce-back emails | No effort | ISP blocklisting; catastrophic for deliverability |
| **Poll bounce mailbox periodically** | Check bounce inbox every 5 min | Simple | Up to 5-min lag; addresses sent to during lag |
| **Real-time DSN processing (selected)** | Dedicated MX server receives DSN; processed via SMTP pipeline | Sub-30-second suppression | Requires MX server; DSN parsing complexity |
| **Vendor bounce webhook** | Use SES/SendGrid bounce webhooks | Managed | Vendor lock-in; still need to process and store |

**Selected approach — Real-time DSN processing:**

A dedicated Bounce Processor MX server receives all bounce-back emails at `bounces@mail.company.com`. This address is set as the SMTP envelope `Return-Path` for all outgoing emails. When an ISP cannot deliver an email, it sends a DSN (RFC 3464) message back to this address.

**Bounce classification:**

| SMTP Code | Type | SMTP Enhanced Code | Action |
|---|---|---|---|
| 550 | Hard bounce | 5.1.1 User unknown | Suppress permanently |
| 550 | Hard bounce | 5.1.2 Bad destination mailbox | Suppress permanently |
| 551 | Hard bounce | 5.1.6 Mailbox moved | Suppress; try alternate if available |
| 421 | Soft bounce | 4.2.2 Mailbox full | Retry; suppress after 5 retries |
| 450 | Soft bounce | 4.7.1 Greylisting | Retry after 10 min |
| 452 | Soft bounce | 4.5.3 Too many recipients | Retry |
| 550 | Spam block | 5.7.1 Message rejected (spam) | Do not suppress address; investigate content/IP reputation |

The Bounce Processor parses the DSN using RFC 3464 multipart/report parsing, extracts the original `Final-Recipient`, and classifies the bounce. Hard bounces are immediately written to the suppression list.

**Suppression list architecture:**

Two layers:
1. **Redis Bloom Filter** for ultra-fast "is this address suppressed?" check — probabilistic, false positive rate 0.01%, false negative rate 0%. Used at ingest time (before rendering template). Size: 100 M addresses × 10 bits/element (Bloom filter math: `-n * ln(p) / (ln2)^2` = 100M × ln(0.0001) / 0.48 = ~286 MB).
2. **Redis Hash** for exact lookup + suppression type — `suppress:{sha256(lower(address))}` — used when Bloom filter indicates suppression, to retrieve the specific suppression type and provide meaningful `suppression_reason` in the API response.
3. **PostgreSQL** as the source of truth — all suppressions written durably; Redis rebuilt from Postgres on startup.

**FBL complaint handling:**

ISPs operating feedback loops:
- **Gmail**: Google Postmaster Tools — no direct FBL; instead, monitor Gmail spam rate via Postmaster Tools API (daily aggregate). React by pausing sending to Gmail if spam rate > 0.08%.
- **Outlook/Hotmail**: JMRP (Junk Mail Reporting Program) — direct FBL; ARF format (RFC 5965). The reported email's `X-original-recipient` header is extracted; address added to suppression list with `complaint` type.
- **Yahoo**: Yahoo Complaint Feedback Loop — ARF format. Same processing.
- **Apple**: Not documented publicly; Apple provides bulk sender guidelines but no direct FBL.

**Implementation detail — FBL inbox processing:**

A Postfix instance receives FBL emails at `complaints@mail.company.com`. A Python-based FBL processor (using `flanker` for MIME parsing, `python-abusix` or custom ARF parser) reads from the Postfix queue via Postfix's `pipe` transport, extracts the complained-about address, and publishes to a `email.complaints` Kafka topic. The Suppression List Updater consumer writes to Redis and Postgres. End-to-end latency from complaint receipt to suppression: < 30 seconds.

**Interviewer Q&As:**

Q: How do you handle a situation where a corporate MX server issues a 550 for a valid employee because of a misconfigured spam filter — i.e., a false hard bounce?
A: Hard bounce suppression by default is permanent. For this case: (1) Implement a "bounce dispute" API: `DELETE /v1/suppressions/{address}?type=hard_bounce` — callable by producers with a compliance acknowledgment flag. (2) Log all manual removals for audit purposes. (3) After manual removal, monitor subsequent sends to that address — if another 5xx is received within 30 days, re-suppress automatically. (4) For enterprise customers: implement a "safe-send" override that bypasses suppression with an explicit `force_send: true` flag (requires special producer permission scope). This is a rare operational exception — the default behavior (permanent suppression) protects the overall sender reputation.

Q: How do you distinguish a full-mailbox soft bounce (retryable) from a "mailbox over quota permanently" soft bounce that should eventually become a hard suppression?
A: Track retry count per address. When the `4.2.2 Mailbox Full` bounce fires, increment `bounce_retries:{address}:{campaign_id}` in Redis. After 5 soft bounces for the same address across different campaigns within 30 days, promote to a soft-but-persistent suppression (suppress for 90 days, not permanently). Re-check after 90 days by removing from suppression and allowing one send; if another bounce, suppress for 1 year. This three-tier escalation prevents permanent loss of valid addresses that were temporarily full (e.g., on vacation), while protecting reputation from chronic bounces.

Q: How do you handle bounces for a marketing campaign that was sent to 50 M addresses? The bounce ingest volume must be significant.
A: At 2% bounce rate, a 50 M campaign generates 1 M bounce DSNs. These arrive over 1-48 hours as ISPs attempt delivery. The Bounce MX server receives ~10-50 bounce DSNs/second during peak campaign delivery. Postfix queue handles incoming DSNs up to ~10,000/s (well within capacity). The FBL/bounce processor reads from Postfix pipe at ~1,000/s and publishes to Kafka. The Suppression List Updater batches Redis writes (Redis PIPELINE / HMSET batches of 100). Postgres writes are batched via `INSERT ... ON CONFLICT DO NOTHING` with batches of 500 rows. The suppression list is updated within 30 seconds for each bounce, providing real-time protection for any subsequent campaigns that might overlap.

Q: What prevents a suppressed address from being re-added to a marketing list by a data provider who re-uploaded a CSV?
A: The suppression check at the Ingest Service happens at send time, not list-upload time. Regardless of how an email address enters a recipient list (segment, CSV upload, API), the Ingest Service always checks the suppression list before dispatching. Even if a marketing operator manually overrides a suppression at list-upload time, the ingest-time check provides an independent, automated backstop. Hard-bounce suppressions are permanent and cannot be overridden via list upload — only via the explicit dispute API (admin-only). This defense-in-depth means the list hygiene layer and the send-time layer both independently protect reputation.

Q: How do you ensure GDPR Article 17 (right to erasure) for email addresses in the suppression list?
A: This creates a conflict: GDPR requires erasure, but suppression lists exist to protect the user from unwanted email. Resolution: (1) On erasure request, replace the suppression record with a one-way hash of the email address (SHA-256 + pepper). (2) The hash is still checked at send time — if a future send to the same address arrives, the hash matches and it is suppressed. (3) The actual email address is erased. This balances the user's right to erasure with the user's right not to receive unsolicited email after opting out (both GDPR rights). The hashed suppression record is tagged with `is_erased = true` in Postgres; the email address column is overwritten with the hash.

---

### 6.3 Email Deliverability: IP Warming, Reputation Management, and Blocklist Monitoring

**Problem it solves:**
A new sending IP with no history is treated with suspicion by ISPs. Sending large volumes immediately triggers spam filtering. Additionally, IP/domain reputation degrades when bounce rates, complaint rates, or spam-trap hits exceed ISP thresholds. This must be managed proactively.

**Approaches:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **No warming — blast immediately** | Send full volume from day one | No delay | Immediate spam filtering; permanent reputation damage |
| **Manual warming (operations-driven)** | Ops team follows a schedule manually | Precise control | Error-prone; doesn't adapt to ISP signals |
| **Automated ramp-up schedule (selected)** | System enforces graduated volume per IP per ISP per day | Consistent; auditable; adaptive | Requires per-IP volume tracking; scheduling complexity |
| **Vendor-managed IPs (SendGrid/SES)** | Use vendor's pre-warmed shared IPs | No warming effort | Shared reputation; less control; neighbor domain risk |

**Selected approach — Automated IP warming with adaptive feedback:**

**Warming schedule (standard):**

| Day | Volume per IP (total across all ISPs) |
|---|---|
| 1-3 | 500/day |
| 4-7 | 2,000/day |
| 8-14 | 10,000/day |
| 15-21 | 50,000/day |
| 22-28 | 200,000/day |
| 29+ | 500,000+/day (full capacity) |

The warming schedule is stored as a config table in Postgres. A `WarmingScheduler` service reads current day for each IP and sets the `daily_send_limit` in `ip_pools`. The Campaign Throttle Worker enforces this limit via a Redis counter `daily_count:{ip}:{date}` (incremented per send, compared to limit before dispatch).

**Adaptive response to ISP signals:**

If the relay receives `421` (throttle) from an ISP for a given IP, the Campaign Throttle Worker: (1) Pauses sends from that IP to that ISP for the duration specified in the SMTP response (or 1 hour if unspecified). (2) If `421` recurs > 3 times in 24 hours, reduces the IP's daily limit for that ISP by 50% and files a warming regression event (resets warming to previous tier).

**Blocklist monitoring:**

Key Real-time Blocklists (RBLs):
- Spamhaus ZEN (SBL + XBL + PBL) — most important; checked via DNS query (`<reversed_ip>.zen.spamhaus.org`).
- SORBS — secondary; checked similarly.
- Barracuda BRBL.
- SpamCop.

A `BlocklistMonitor` service runs every 10 minutes (not 15 — given the 15-minute alert SLA, 10-minute checks provide margin). For each active sending IP, it performs DNS lookups against all RBLs. If an IP is listed:
1. Set `is_active = false` in `ip_pools` immediately.
2. Publish a `ip_blocklisted` event to Kafka; a consumer creates a PagerDuty incident.
3. Route that IP's scheduled traffic to the vendor fallback (SES/SendGrid) until delisting.
4. Begin delisting process: automated submission to Spamhaus lookup tool; notify the deliverability team.

**Domain reputation monitoring (Google Postmaster Tools):**

Google Postmaster Tools provides a daily API with `domain_reputation` (HIGH/MEDIUM/LOW/BAD) and `spam_rate` per sending domain. A daily job polls this API; if `spam_rate > 0.08%`, pause all marketing sends from that domain for 24 hours and alert the deliverability team.

**Spam trap avoidance:**

Spam traps are email addresses maintained by ISPs and anti-spam organizations to catch senders with poor list hygiene. Types: (1) pristine traps (addresses that never opted in); (2) recycled traps (abandoned addresses re-activated as traps). Mitigations: (1) Require double opt-in for all marketing lists. (2) Implement list-age hygiene: suppress addresses that have not opened/clicked in 18 months (engagement-based suppression). (3) Monitor for spam-trap hit signals from vendors like Validity ReturnPath or Kickbox.

**Interviewer Q&As:**

Q: You're mid-campaign and 2 of your 20 marketing IPs get listed on Spamhaus. How do you respond without stopping the campaign?
A: The BlocklistMonitor detects the listing within 10 minutes, sets those 2 IPs to `is_active = false`. The Campaign Throttle Worker, which checks `is_active` before dispatching, automatically stops using those IPs. The campaign continues on the remaining 18 IPs at 10% reduced throughput (2/20 = 10% capacity reduction). Simultaneously: (1) The blocklisted IPs' pending Kafka messages are re-routed to the vendor fallback queue. (2) A PagerDuty alert fires; the deliverability team investigates the cause (spike in bounces? spam complaints?). (3) Delisting request submitted to Spamhaus (SBL delisting requires demonstrating the abuse has stopped; PBL/XBL have automatic removal). Campaign completion is delayed by < 30 minutes.

Q: What is a DMARC aggregate report and what would you look for in it?
A: DMARC aggregate reports (`rua` tag) are sent daily by ISPs (Gmail, Outlook, Yahoo) to the address specified in your DMARC record. They are XML files containing: (1) Source IP — which IPs are sending email claiming to be from your domain. (2) DKIM pass/fail — whether DKIM signature validated. (3) SPF pass/fail — whether envelope-from was authorized. (4) DMARC alignment — whether SPF/DKIM aligned with the `From:` domain. (5) Disposition — none/quarantine/reject. I look for: (a) Unexpected source IPs (potential phishing/spoofing of our domain — need to add to SPF or investigate). (b) DKIM failure rate > 0% (misconfigured milter, expired key). (c) SPF alignment failures (sending from a domain not in SPF). (d) Low overall pass rate — indicates a configuration problem. We parse these reports automatically with `parsedmarc` and surface metrics in Grafana.

Q: How do you handle the reputation consequences of a phishing campaign that spoofs your sending domain?
A: DMARC `p=reject` prevents spoofed emails from reaching Gmail/Outlook inboxes — they are discarded. However: (1) We see the spoofed source IPs in DMARC aggregate reports; alert if spoofed send volume exceeds 1,000/day. (2) For ISPs without DMARC enforcement (some smaller providers), the emails may be delivered and generate complaints that feed back to us via FBL. The FBL address is in our DSN header — complaints for spoofed emails arrive at our complaint processor. (3) Response: publish a known-phishing alert (BIMI implementation helps users visually verify our emails); coordinate with ISP abuse desks to block the offending IP ranges. (4) BIMI (Brand Indicators for Message Identification) — requires a VMC (Verified Mark Certificate) and DMARC `p=reject`; causes Gmail to display our logo next to emails, helping recipients distinguish legitimate from spoofed.

Q: How do you implement engagement-based list hygiene to improve deliverability?
A: Track per-address last-open-timestamp and last-click-timestamp in ClickHouse (derived from delivery events). A weekly batch job: (1) Identify addresses with no open or click in the last 18 months that are not transactional-email-only recipients. (2) Add these to a `low_engagement` suppression with `expires_at = NOW() + 180 days`. (3) Before suppressing, send a re-engagement email ("Do you still want to hear from us?") — 3 days later, if no open, proceed with suppression. This reduces the "dead" address fraction in marketing lists, directly improving the open-rate-based reputation signals ISPs use. This also reduces the risk of hitting recycled spam traps (addresses inactive > 12 months are prime candidates for recycling).

Q: What is IPv6 for email delivery and should you use it?
A: IPv6 offers a nearly unlimited address space, meaning a sender can obtain millions of IPs cheaply. However: (1) Many ISPs apply strict policies to IPv6 — Gmail requires IPv6 senders to have an established IPv4 reputation in parallel (they check both). (2) Many blocklists don't cover IPv6 comprehensively, so a blocklisted IPv6 IP may not trigger standard checks. (3) The reputation model for IPv6 is per-/48 prefix (not per-IP), so a bad actor on your prefix harms you. Decision: use IPv4 as the primary sending protocol; add IPv6 for senders that specifically request it (some enterprises). Ensure Postfix sends on IPv4 by default (`inet_protocols = ipv4` in main.cf). IPv6 is enabled per sending domain only after dual-protocol reputation is established over 30+ days.

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling strategy | Bottleneck handled |
|---|---|---|
| **API Gateway** | Auto-scale on RPS/CPU; stateless | Inbound traffic spikes |
| **Email Ingest Service** | K8s HPA on CPU; stateless | Suppression check, template render throughput |
| **Kafka** | Add brokers; 200 partitions for transactional, 500 for marketing; RF=3 | Message throughput and retention |
| **SMTP Relay Pool** | Add Postfix pods; each handles ~5,000 SMTP deliveries/min | SMTP connection concurrency to ISPs |
| **Campaign Throttle Workers** | K8s HPA on Kafka consumer lag (marketing topic) | Campaign throughput during bulk sends |
| **Bounce Processor MX** | Scale Postfix instances behind NLB; DSN processing workers auto-scale | Bounce ingest during large campaigns |
| **Suppression List Updater** | Kafka consumer group; auto-scale on lag | Suppression write throughput |
| **Open/Click Tracker** | Stateless HTTP server behind CDN (CloudFront); auto-scale | Tracking pixel / redirect burst traffic |
| **PostgreSQL** | Read replicas (5) for analytics queries; primary for writes; partition emails table by month | Metadata read throughput; storage manageability |
| **Cassandra** | Add nodes; RF=3; LCS compaction | Delivery event write throughput and storage |
| **Redis** | Redis Cluster; 10 shards; shard by address hash | Suppression check throughput |
| **ClickHouse** | Horizontal sharding; replication; MergeTree table engine | Analytics query throughput |

### DB Sharding

PostgreSQL `emails` table: range-partitioned by `created_at` (monthly). Queries filtered by `created_at` benefit from partition pruning. Partition maintenance: each month, create next month's partition; detach and archive partitions older than 90 days.

Cassandra `delivery_events_by_email`: partitioned by `email_id` — uniform distribution via UUID random generation. No hotspot risk. `delivery_events_by_address` (optional secondary table): partitioned by `(address_hash_bucket, date)` — 256 address hash buckets prevents hotspots for high-volume addresses.

### Caching

| Cache layer | Data | TTL | Size |
|---|---|---|---|
| Redis Bloom Filter | Global suppression list membership | No TTL; rebuilt on restart | ~286 MB (100 M addresses) |
| Redis Hash | Suppression type by address | No TTL; write-through | ~20 GB (100 M entries × 200 B) |
| Redis Counter | Per-IP daily send count | Until midnight UTC | 10 B per IP (thousands of IPs) |
| Redis Token Bucket | Per `(IP, ISP)` rate limit state | Sliding 60-s window | Small; fits in memory |
| Local process cache | Compiled template objects | 15 minutes; LRU 500 entries | ~50 MB per pod |
| CDN (CloudFront) | Open-tracking pixel GIF (1x1) | 1 year (immutable) | Edge-cached; zero origin load |

### Replication

- **Kafka**: RF=3; `min.insync.replicas=2`; `acks=all`. Partitions distributed evenly across 3+ brokers.
- **PostgreSQL**: Synchronous streaming replication to 1 hot-standby (same AZ, RPO=0). 4 async read replicas in 2 AZs. Patroni for automated failover; PgBouncer for connection pooling.
- **Cassandra**: RF=3, two DCs (us-east-1, eu-west-1). `LOCAL_QUORUM` for reads and writes within each DC.
- **Redis**: 3 replicas per shard; AOF persistence (`appendfsync = everysec`).

### Interviewer Q&As — Scaling

Q: The emails table will have 3.3 B rows after 90 days. How do you handle queries across that volume?
A: (1) Monthly range partitioning means most queries include a `created_at` filter and touch only 1-3 partitions (< 110 M rows). (2) Covering indexes on `(campaign_id, created_at)` and `(to_address, created_at)` handle the most common query patterns. (3) Analytics queries (open rates, bounce rates per campaign) are offloaded to ClickHouse — the emails table in Postgres is not queried for aggregations. (4) After 90 days, partitions are converted to archive-only read replicas (detached from primary, stored in cheap EBS sc1 class). Current partitions stay on gp3 SSD.

Q: How do you scale the suppression list check to 4,305 RPS without Redis becoming a bottleneck?
A: The Bloom filter lookup is a local operation — the filter is loaded into Ingest Service pod memory at startup (286 MB RAM) and updated via a Redis Pub/Sub channel on new suppressions. The in-process Bloom filter check takes < 1 μs. Only addresses that the Bloom filter marks as "possibly suppressed" (0.01% false positive rate + true positives) make a Redis call for the exact suppression type. At 4,305 RPS with 5% of addresses suppressed: 4,305 × 5% × (1 + 0.01%) ≈ 215 Redis calls/s — easily handled by a 3-node Redis cluster.

Q: If a campaign sends 50 M emails in 4 hours, how many SMTP relay pods do you need?
A: 50 M / 4 hours / 3,600 s = 3,472 emails/s. Each Postfix pod delivers ~1,000 emails/min (60 SMTP connections × 17 emails/connection-minute; limited by SMTP handshake overhead and ISP acceptance rate). 3,472 emails/s = 208,333 emails/min ÷ 1,000 emails/min/pod = 209 pods. Round up to 250 pods for headroom. On AWS, using EC2 c5.xlarge instances at $0.17/hr, 250 pods run for 4 hours = $170 for the campaign's SMTP relay compute. Pre-scale 30 minutes before campaign start; scale down over 2 hours after completion.

Q: How do you handle ISPs that temporarily throttle your bulk campaign (421 responses)?
A: Adaptive throttling: when a Postfix relay receives `421 too many connections from your IP` from Gmail MX, Postfix queues the message for retry. The Campaign Throttle Worker monitors per-ISP `421` rate via Postfix log streaming to Kafka. If `421` rate for `(IP, ISP)` > 10% of attempts in the last 5 minutes: (1) Reduce that IP's Gmail concurrency limit by 50%. (2) Shift more of the Gmail-destined volume to a different IP in the pool. (3) If all IPs are throttled by Gmail, slow the overall campaign pace (increase the inter-message sleep in the throttle worker) rather than creating massive retry queues. The campaign will take longer to complete; update the estimated completion time in the campaign status.

Q: How would you scale to 500 M emails/day (10x the current scale)?
A: (1) SMTP relay: scale from 250 to 2,500 pods; need more egress IP addresses (~2,500 IPs in the pool vs. current hundreds). Obtain a /16 or multiple /24 subnets; warm each IP following the warming schedule. (2) Kafka: increase partition count from 500 to 5,000 for the marketing topic. (3) Cassandra: add nodes to handle 5,500 delivery events/s (10x current). (4) PostgreSQL emails table: move to Citus (distributed Postgres) sharding by `(producer_id, created_at)`, or migrate campaign/email metadata to DynamoDB. (5) IP reputation: obtain dedicated Class C IP blocks per ISP (some ISPs like Outlook provide a dedicated IP reputation agreement for large senders). (6) Sending domain diversification: add 10 additional sending subdomains to distribute reputation risk.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| **SMTP relay pod crash** | In-flight emails lost | Health check; Kafka consumer group rebalance | Emails in Kafka queue reprocessed; at-most-once delivery risk mitigated by idempotency key + Postfix deferred queue |
| **ISP MX unavailable** | Emails to that ISP not delivered | 5xx / connection refused from ISP | Postfix deferred queue retries over 5 days; after 1 day, SES fallback activated for that ISP |
| **Kafka broker failure** | Potential in-flight message loss | Broker down alert | RF=3, `min.insync.replicas=2` prevents loss; producer blocks until 2 replicas ack |
| **Postgres primary failure** | Metadata writes fail | Primary health check; Patroni failover | Failover to synchronous standby in < 30 s (RPO=0, RTO=30 s) |
| **Redis cluster shard failure** | Suppression checks fail for that shard | Error rate spike | Bloom filter in-process fallback; fail-closed (suppress all sends to affected addresses until Redis recovered) |
| **Blocklist listing** | ISP filtering of affected IP | BlocklistMonitor detects in < 10 min | IP deactivated; traffic shifted to other IPs + vendor fallback; delisting initiated |
| **Sending domain DMARC failure spike** | Emails quarantined/rejected | DMARC report parse; Postmaster Tools | Pause marketing sends; investigate SPF/DKIM misconfiguration; rollback recent config change |
| **Template rendering error** | Email not sent; 500 error | Error rate alert | Return 400 to producer with template validation error; store template render errors; alert template owner |
| **FBL mailbox unavailable** | Complaint suppressions delayed | FBL MX health check | FBL emails queued at MX; processed in batch when recovered; max delay = MX queue hold time (hours) |
| **Campaign scheduler crash mid-send** | Campaign partially sent, state unclear | Checkpoint heartbeat | Campaign state in Postgres (shard checkpoints); restart resumes from last checkpoint; idempotency prevents double-send |

### Retry Policy

| Error | Retry strategy | Max duration | Notes |
|---|---|---|---|
| SMTP 421 / 450 (soft bounce, temporary) | Postfix deferred queue; exponential | 5 days | Standard MTA behavior; drops email and generates DSN after max lifetime |
| SMTP 550 / 551 (hard bounce) | No retry | — | Immediate DSN; suppress address |
| SMTP 5xx from our relay (internal error) | Re-enqueue to Kafka; backoff | 3 retries, 15 min TTL (transactional) | Transactional emails have short TTL; stale OTPs are useless |
| Postgres write failure (timeout) | Retry with exponential backoff | 3 retries, 5 s each | After 3 failures, write to dead-letter Kafka topic for manual review |
| Redis write failure | Log + async retry | Best-effort | Postgres is source of truth; Redis is rebuilt from Postgres if needed |

### Idempotency

Each email send generates a `message_id = <{uuid}@{sending_domain}>`. This is unique per send. Before Postfix dispatches, an idempotency check: `SET NX email_dispatch:{idempotency_key} 1 EX 86400` in Redis. If NX fails, the email was already dispatched from a previous consumer poll — skip. This prevents double-sends on Kafka consumer rebalance (at-least-once delivery in Kafka means a message can be consumed twice after rebalance if the consumer fails after sending but before committing the offset).

### Circuit Breaker

Per-ISP circuit breakers on the Campaign Throttle Worker and SMTP Relay Pool:
- **Closed**: Normal; count 5xx / connection failures.
- **Open**: > 30% of SMTP connection attempts to an ISP fail over a 10-minute window. All sends to that ISP are paused; messages held in Kafka.
- **Half-Open**: After 15-minute cooldown, send 10 probe emails to that ISP. If > 8 succeed, close circuit. Otherwise re-open.

Separate circuit breakers for each major ISP (Gmail, Yahoo, Outlook) and each sending IP.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert threshold | Meaning |
|---|---|---|---|
| `email.send.rps` | Counter | — | Ingest throughput |
| `email.send.latency_p99_ms` | Histogram | > 500 ms | Ingest API slowness |
| `email.kafka.transactional_lag_s` | Gauge | > 30 s | Transactional queue backup |
| `email.kafka.marketing_lag_s` | Gauge | > 300 s | Marketing queue backup |
| `email.smtp.delivery_rate` | Gauge | < 95% overall | Overall deliverability |
| `email.smtp.bounce_rate` | Gauge | > 2% | List hygiene / IP quality |
| `email.complaint_rate` | Gauge | > 0.08% | Content / targeting quality |
| `email.suppression.hard_bounce_additions` | Counter | > 10,000/hr | Mass invalid address campaign |
| `email.relay.421_rate_by_isp` | Gauge | > 15% for any ISP | ISP throttling our IP |
| `email.relay.5xx_rate` | Gauge | > 1% | ISP rejecting our email |
| `ip.blocklist_status` | Gauge (0/1 per IP) | Any = 1 (listed) | Blocklist listing |
| `dmarc.pass_rate` | Gauge | < 95% | Authentication misconfiguration |
| `campaign.completion_progress_pct` | Gauge | — | Campaign send progress |
| `email.open_rate_7d` | Gauge | < 15% sustained | Engagement health |
| `email.click_rate_7d` | Gauge | < 1.5% sustained | Content engagement health |
| `email.unsubscribe_rate` | Counter | > 0.5%/campaign | Targeting or content issue |
| `postmaster_tools.spam_rate.gmail` | Gauge | > 0.08% | Gmail reputation alarm |

### Distributed Tracing

OpenTelemetry spans:
1. `email.ingest.handle_request` — covers validation, suppression check, template render.
2. `email.suppression.check` — Bloom filter + Redis hash lookup; attribute: `suppression_result`.
3. `email.template.render` — template engine call; attributes: `template_id`, `render_ms`.
4. `kafka.produce.email` — publish to Kafka; attributes: `topic`, `partition`, `offset`.
5. `email.smtp.dispatch` — Postfix delivery attempt; attributes: `to_domain`, `smtp_response_code`, `relay_ip`.
6. `email.bounce.process` — DSN parse + suppression write; attributes: `bounce_type`, `smtp_code`.

Trace context propagated via `X-Email-Trace-ID` header embedded in the email body (as a hidden element, used if open/click events need to correlate to the original trace). For server-side tracing, Kafka message headers carry the W3C TraceContext.

### Logging

Structured JSON logs. Key fields: `email_id`, `campaign_id`, `to_domain` (not full address), `sending_ip`, `smtp_response`, `trace_id`.

PII handling: `to_address` is logged only as `sha256(lower(to_address))` in application logs. Full address only in audit logs (separate log stream, access-controlled). Email content never logged.

Log levels:
- **ERROR**: SMTP 5xx, rendering failure, Postgres/Cassandra write failure, blocklist listing.
- **WARN**: ISP throttle (421), soft bounce, campaign pace reduction.
- **INFO**: Email accepted, campaign start/complete, suppression added.
- **DEBUG**: Per-connection SMTP dialog (disabled in production).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B (selected) | Reason |
|---|---|---|---|
| Delivery method | Vendor API only (SES/SendGrid) | Hybrid: in-house SMTP + vendor fallback | 9x cost saving at scale; full IP reputation control; vendor retained for resilience |
| Suppression check at ingest | Postgres synchronous query | Redis Bloom filter + hash | Postgres adds 5-10 ms/check at 4,305 RPS → 43-107 ms cumulative latency on ingest path; Redis < 1 ms; Bloom filter enables in-process check at 1 μs |
| Bounce processing | Periodic polling of bounce mailbox | Real-time DSN via dedicated MX | Polling introduces up to 5-min lag; additional invalid-address sends during lag; real-time processing suppresses within 30 s |
| DKIM key management | Self-managed files on relay servers | Vault-managed with sidecar injection | File-based keys are a security risk (accessible to anyone with server access); Vault provides audit trail, access control, automated rotation |
| Delivery tracking (open/click) | Third-party tracking pixel (vendor) | Self-hosted pixel + click proxy (CDN-fronted) | Vendor tracking causes third-party cookie issues; self-hosted allows custom UTM parameters; CDN provides scale without origin load |
| IP reputation management | Vendor handles (shared or dedicated IPs) | Self-managed IP pools with automated warming | Own IPs: complete control over reputation, no neighbor-domain risk, 9x lower cost; warming automation reduces manual ops burden |
| DMARC policy | Start at `p=reject` | Graduated: `none` → `quarantine` → `reject` | Immediate `reject` on a new domain blocks all legitimate email if SPF/DKIM misconfigured; graduated approach catches misconfigurations in `none`/`quarantine` stages |
| Campaign recipient expansion | At schedule time (pre-compute email records) | At send time (lazy expansion from segment) | Pre-compute creates 50 M rows in DB for large campaigns; lazy expansion uses segment queries at fan-out time, reducing write amplification; segment membership may change between schedule and send (acceptable) |
| Template rendering location | Client-side (producer passes HTML) | Server-side (server renders template) | Client-side: no template management, variable injection errors; server-side: consistent rendering, centralized template versioning, tracking pixel injection, click-link rewriting |

---

## 11. Follow-up Interview Questions

Q1: How would you implement a multi-step email drip campaign (e.g., Day 0: welcome email, Day 3: follow-up, Day 7: discount offer)?
A: Model a `drip_campaign` with a sequence of `drip_steps` (step index, template_id, delay_days, condition). When a user enrolls in a drip campaign (via API or trigger), create a `drip_enrollment` record in Postgres. A `DripScheduler` service runs every minute, queries for enrollments with `next_step_due_at <= NOW()`, and publishes the email task to the ingest API. Between steps, re-evaluate eligibility conditions (e.g., "skip if user has already purchased"). Step delivery is tracked as a standard email; the drip enrollment records the last-completed step index. If a user opts out of the category, all pending drip steps are cancelled. The scheduler must be idempotent — use `FOR UPDATE SKIP LOCKED` in Postgres to claim step records without concurrent processing.

Q2: How do you handle email pre-sending validation (e.g., detecting likely-to-be-invalid email addresses before sending)?
A: Three validation tiers at list-upload time: (1) **Syntax validation** (RFC 5321/5322): MX record exists for the domain; address format valid (regex). Reject addresses with no MX record. (2) **SMTP ping validation** (optional): Connect to the recipient's MX and issue `RCPT TO:` without sending the email body; many ISPs disable this (no-op on `RCPT TO`) but some respond with 550 for non-existent users. Latency: 2-5 s per address; use async batching only for new lists. (3) **List hygiene vendors**: Integrate with Kickbox, Neverbounce, or ZeroBounce APIs — they maintain continuously updated invalid-address databases. Validate large uploaded lists asynchronously before first send; block send if > 5% invalid rate detected. (4) At send time: the suppression list already excludes hard-bounced addresses from previous campaigns.

Q3: What is BIMI and how does it affect email design at this system level?
A: BIMI (Brand Indicators for Message Identification) enables a verified brand logo to display next to emails in Gmail, Outlook, and Yahoo. System-level requirements: (1) DMARC `p=reject` (must be at enforcement). (2) A BIMI DNS `TXT` record: `v=BIMI1; l=https://brand.company.com/logo.svg; a=https://brand.company.com/cert.pem`. (3) A VMC (Verified Mark Certificate) purchased from DigiCert or Entrust (~$1,500/year). (4) The `a=` URI points to the VMC (a PEM certificate embedding the SVG logo, signed by the CA). Implementation: (a) Publish the BIMI TXT record per sending domain. (b) Store the VMC in S3 at a stable URL with appropriate `Content-Type`. (c) No changes needed to the email content or SMTP relay — BIMI is entirely DNS + certificate based. Benefit: 10-20% open rate improvement reported by early adopters.

Q4: How do you enforce rate limits on the open-tracking pixel endpoint during a large campaign?
A: During a 50 M email campaign with 25% open rate and 7-day open window, the pixel server receives ~12.5 M open events over ~2 days with heavy concentration in the first 2 hours (~1.5 M events in the first hour = ~417 events/s). The pixel server (`GET /pixel/{email_id}.gif`) is served by a CloudFront CDN; the CDN serves the pixel GIF from edge cache (immutable, 1-year TTL) — zero origin requests for the GIF itself. The `email_id` tracking is done via CDN custom log delivery to Kinesis Data Firehose → Kafka → event processor. Each CDN edge logs the request with the query string containing the email_id. The origin (pixel server) is only called for cache misses on new email_ids — at 417 events/s with warm cache, origin load is negligible. CDN scales automatically to handle any open-event spike.

Q5: How would you handle transactional email during a Kafka outage?
A: Two-tier fallback: (1) **Primary path**: API Gateway → Ingest Service → Kafka → SMTP Relay. (2) **If Kafka is down** (circuit breaker on Kafka producer open): Ingest Service switches to **synchronous fallback mode** — directly calls the SMTP relay pool via HTTP (bypassing Kafka) for `email_type = transactional`. This adds ~500 ms to the ingest API latency but keeps transactional emails flowing. The synchronous path does not provide the same durability guarantee as Kafka, but at-least-once delivery is maintained because the ingest API does not return 202 until the SMTP relay acknowledges acceptance. (3) Marketing/bulk emails are halted during Kafka outage — the Ingest Service returns `503 Service Unavailable` for bulk emails; the producer must retry.

Q6: How does CAN-SPAM compliance affect the email system's design?
A: CAN-SPAM (US law for commercial email) requires: (1) Honest `From:` and `Subject:` — enforced at template validation time; templates with deceptive patterns are rejected. (2) Physical mailing address in email body — template validation ensures marketing templates include a `{physical_address}` variable; the template engine injects the sending organization's address. (3) Opt-out mechanism that works for at least 30 days — our unsubscribe infrastructure processes opt-outs immediately and stores them permanently; 30 days is trivially exceeded. (4) Honor opt-out within 10 business days — we honor within 5 seconds (real-time suppression). (5) No deceptive routing — sending domain must match the `From:` address domain; enforced at Ingest Service. Penalties: up to $51,744 per violation. These requirements are enforced at send-time, not audit-time — the Ingest Service rejects emails that would violate CAN-SPAM before they enter the pipeline.

Q7: How do you implement transactional email priority so a password-reset email is never delayed by a marketing campaign?
A: Strict queue separation: transactional emails published to `emails.transactional` Kafka topic; marketing to `emails.marketing`. SMTP relay workers are two separate pools with dedicated pods: `transactional-relay` pods only consume from the transactional topic; `marketing-relay` pods only consume from the marketing topic. K8s resource quotas: transactional relay pods have `resources.requests.cpu = 2, memory = 2Gi` with Guaranteed QoS class; marketing relay pods have `resources.requests.cpu = 1, memory = 1Gi` with Burstable QoS. On a node resource crunch, K8s evicts Burstable pods before Guaranteed. Additionally, transactional emails use a dedicated IP pool and a different `From:` domain — if the marketing IP pool gets throttled, transactional IPs are unaffected.

Q8: How do you detect and block a rogue internal producer that is spamming users?
A: (1) Per-producer daily email volume cap enforced via Redis counter (same pattern as frequency caps). Default cap: 10 M emails/day; configurable per producer in Postgres. (2) Per-producer complaint rate monitoring: if `complaints_from_producer_X / sends_from_producer_X > 0.2%` in a 24-hour window, auto-pause producer X and alert. (3) Per-producer bounce rate monitoring: > 5% bounce rate triggers a pause and alert. (4) Admin-level kill switch: `UPDATE producers SET is_active = false WHERE producer_id = ?` — Ingest Service reads this from a Redis flag (updated within 5 s of Postgres write). (5) Audit log: all emails log the `producer_id`; the complaint/bounce attribution is stored per producer. This enables post-hoc investigation of which service sent problematic emails.

Q9: How would you implement email scheduling across user timezones (send at 9 AM local time)?
A: The Ingest Service accepts `scheduled_at` as a UTC timestamp. For timezone-aware scheduling: (1) The marketing operator specifies `send_at_local_time = "09:00"` in the campaign. (2) The campaign scheduler has access to recipient timezone (stored in the user profile or inferred from email domain/IP geolocation history). (3) For each recipient, calculate `scheduled_at = today_in_user_tz.combine(09:00, tz).astimezone(utc)`. (4) Group recipients into hourly UTC buckets; publish to Kafka with `headers.deliver_after = <unix_timestamp>`. (5) The Campaign Throttle Worker checks `deliver_after` before dispatching — messages not yet due are re-queued with a 60-second check interval. This creates 24 delivery waves for a global campaign, which also naturally smooths load. For users with unknown timezone, default to UTC or the campaign's configured default timezone.

Q10: How do you design the unsubscribe link to be tamper-proof and time-limited?
A: The unsubscribe link embedded in emails uses a signed JWT: `https://mail.company.com/unsubscribe?token=<jwt>`. The JWT payload: `{sub: sha256(address), email_id: "uuid", category: "marketing", exp: <7_days_from_send>, iat: <send_time>}`. Signed with HMAC-SHA256 using a secret stored in Vault. The signature prevents: (1) Guessing another user's unsubscribe URL. (2) Forging an unsubscribe request for someone else. The `exp` field (7 days) limits the window, but for CAN-SPAM compliance (30-day opt-out requirement), extend to 30 days: `exp: <30_days_from_send>`. After expiry, clicking the link redirects to an "enter your email" form (fallback manual unsubscribe). The `sub` is `sha256(address)` (not the raw address) so the address cannot be extracted from the URL even if the JWT secret is not known.

Q11: What is an email warm-up and how long does it take for a new IP?
A: An IP warm-up is the gradual increase in email volume sent from a new IP to build a positive sending reputation with ISPs. ISPs use machine learning models that require historical data — a new IP with no history is inherently suspicious. Duration: 30-60 days to full capacity (500 K/day/IP) following the graduated schedule described in the Deep Dive. Key milestones: (1) Day 1-7: 500-2,000/day — ISPs observe initial traffic; spam rate signals begin forming. (2) Day 8-14: 10,000/day — sustained engagement signals (opens, clicks) feed ISP models. (3) Day 15-28: 50,000-200,000/day — IP transitions from "new" to "established" in most ISP models. (4) Day 29+: 500,000+/day. Best practices during warm-up: send to the most-engaged users first (highest expected open rate); avoid purchased lists; monitor bounce and complaint rates daily; if rates exceed thresholds, pause and investigate before resuming.

Q12: How do you handle email attachment security — preventing malware delivery?
A: (1) Content-type whitelist: only allow `application/pdf`, `image/png`, `image/jpeg`, `image/gif` attachments in the API. Reject `application/zip`, `application/exe`, and any script MIME types. (2) Size limit: 10 MB total attachment size per email; attachments stripped and email rejected if exceeded. (3) Virus scanning: for PDFs (which can contain embedded JavaScript or vulnerabilities), scan with ClamAV (open source) or VirusTotal API before attaching. Scan result must be `CLEAN` to proceed. (4) S3 storage: attachments stored in S3 with SSE-S3 encryption; signed URLs generated at dispatch time (1-hour expiry) rather than embedding base64 in the email queue. This prevents attachment bloat in Kafka messages. (5) Transactional-only rule: attachments only allowed for `email_type = transactional`; marketing emails cannot carry attachments (spam filter signal; also prevents MAILER-DAEMON abuse).

Q13: How do you implement List-Unsubscribe-Post (RFC 8058) for one-click unsubscribe in Gmail?
A: Gmail and Yahoo require `List-Unsubscribe` and `List-Unsubscribe-Post` headers in all bulk email since February 2024. Implementation: (1) In the email headers, add: `List-Unsubscribe: <https://mail.company.com/v1/unsubscribe?token={jwt}>` and `List-Unsubscribe-Post: List-Unsubscribe=One-Click`. (2) The Ingest Service injects these headers when rendering marketing email templates. The JWT is the same signed token used for the link-based unsubscribe. (3) When a user clicks "Unsubscribe" in Gmail, Gmail issues a `POST /v1/unsubscribe` with body `List-Unsubscribe=One-Click` and the `token` as a query parameter. (4) The endpoint validates the JWT, extracts the address and category, adds to the suppression list, and returns HTTP 200. No user-facing page required. (5) Ensure the endpoint is publicly accessible (no auth required) — Gmail does not send auth headers; security is provided by the JWT signature.

Q14: How do you handle email opens tracked via pixel in Apple Mail's Mail Privacy Protection (MPP)?
A: Apple MPP (introduced iOS 15) pre-fetches all email resources including tracking pixels, causing all Apple Mail users to appear as "opened" regardless of actual engagement. Impact: open rate inflation (Apple Mail is ~40% of email clients). Mitigations: (1) **Do not use open rate alone for engagement decisions** — use click rate as the primary engagement signal (clicks cannot be pre-fetched by MPP). (2) **Bot/proxy filtering**: MPP fetches from Apple's CDN proxy IPs (documented ranges); identify these requests by User-Agent (`Mozilla/5.0 ... Apple-Mail-Proxy`) and IP range; mark as `apple_mpp_open` rather than `genuine_open`. (3) **Machine learning correction**: Build a model that estimates "genuine open rate" from click-through correlation; apply a discount factor to Apple opens. (4) **Engagement scoring**: Weight clicks 5x vs. opens when computing per-user engagement scores for segment targeting.

Q15: How would you design email deliverability SLOs and report on them?
A: Define three deliverability SLOs: (1) **Transactional delivery SLO**: 95% of transactional emails reach recipient mailbox within 5 minutes of API acceptance. Measured via ISP delivery timestamps from DSN "delivered" notifications (not all ISPs send these; supplement with sender-domain-level aggregate data from Postmaster Tools). (2) **Inbox placement SLO**: 90% of sent marketing emails land in the inbox (not spam). Measured via inbox-placement tools (Validity Inbox Monitoring, 250ok) that maintain seed addresses across ISPs and report whether test emails hit inbox or spam. Alert if inbox placement drops below 85% for any major ISP. (3) **Hard bounce rate SLO**: Hard-bounce rate stays below 2%. Measured as `hard_bounces / sends` per campaign; automated pause if exceeded. These SLOs are published to an internal dashboard (Grafana); weekly executive summary auto-generated by a ClickHouse query. Each SLO violation is a P2 incident with a defined response runbook.

---

## 12. References & Further Reading

1. RFC 5321 — Simple Mail Transfer Protocol: https://datatracker.ietf.org/doc/html/rfc5321
2. RFC 5322 — Internet Message Format: https://datatracker.ietf.org/doc/html/rfc5322
3. RFC 3464 — An Extensible Message Format for Delivery Status Notifications: https://datatracker.ietf.org/doc/html/rfc3464
4. RFC 5965 — An Extensible Format for Email Feedback Reports (ARF): https://datatracker.ietf.org/doc/html/rfc5965
5. RFC 8058 — Signaling One-Click Functionality for List Email Headers: https://datatracker.ietf.org/doc/html/rfc8058
6. RFC 7208 — Sender Policy Framework (SPF): https://datatracker.ietf.org/doc/html/rfc7208
7. RFC 6376 — DomainKeys Identified Mail (DKIM) Signatures: https://datatracker.ietf.org/doc/html/rfc6376
8. RFC 7489 — Domain-based Message Authentication, Reporting, and Conformance (DMARC): https://datatracker.ietf.org/doc/html/rfc7489
9. BIMI Group Specification: https://bimigroup.org/resources/
10. Google Email Sender Guidelines (2024): https://support.google.com/mail/answer/81126
11. Yahoo Sender Best Practices: https://senders.yahooinc.com/best-practices/
12. Amazon SES Documentation — Bounce and Complaint Handling: https://docs.aws.amazon.com/ses/latest/dg/send-email-concepts-deliverability.html
13. Postfix Documentation — SMTP TLS and DKIM: http://www.postfix.org/TLS_README.html
14. Spamhaus DNS Blocklist Usage: https://www.spamhaus.org/organization/dnsblusage/
15. Google Postmaster Tools Help: https://support.google.com/mail/answer/6227174
16. Apple Mail Privacy Protection — Developer Documentation: https://developer.apple.com/documentation/networkextension/network_content_filter_providers/content_filtering_on_apple_mail
17. parsedmarc — Open-source DMARC report analyzer: https://github.com/domainaware/parsedmarc
