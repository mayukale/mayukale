# System Design: Stripe-like Payment Gateway

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Payment Method Tokenization (Vaulting)**: Accept raw card data via client-side JS/SDK; convert to a reusable token stored in an HSM-backed vault. PAN never touches the merchant's server.
2. **Charge / Authorize / Capture**: Create charges against stored tokens; support separate auth and capture flows.
3. **Refunds**: Full and partial refunds against prior charges.
4. **Webhook Delivery**: Push event notifications (charge.succeeded, refund.created, etc.) to merchant-configured endpoints with guaranteed at-least-once delivery.
5. **Multi-Currency**: Accept payments in 135+ currencies; support automatic currency conversion via presentment vs. settlement currency.
6. **Connect Platform (Marketplace)**: Enable marketplaces to collect payments on behalf of connected sub-accounts; direct charges, destination charges, and on-behalf-of flows; automatic platform fee deduction.
7. **Fraud Detection**: Risk scoring on every transaction using velocity checks, device fingerprinting, and ML models; configurable block/challenge/allow thresholds.
8. **Saved Payment Methods**: Store tokenized cards and bank accounts for returning customers (one-click checkout).
9. **Payout Management**: Schedule payouts to merchant bank accounts; support instant, daily, weekly payout cadences.
10. **Developer API / Dashboard**: RESTful API, client SDKs (JS, iOS, Android, Python, Ruby, Go, Java), and a web dashboard for transaction management.

### Non-Functional Requirements

1. **Availability**: 99.999% (< 5.26 minutes/year downtime). Payment gateway downtime = direct revenue loss for all merchants.
2. **Latency**: Token creation P99 < 150ms. Charge creation P99 < 500ms (before network). End-to-end authorization P99 < 3s.
3. **Throughput**: 100,000 TPS at peak (all operations combined). Burst to 300,000 TPS for flash sale events.
4. **Durability**: Zero data loss for all payment events. Webhook delivery with 72-hour retry window.
5. **Security**: PCI DSS Level 1, SOC 2 Type II, ISO 27001. Encryption at rest (AES-256-GCM) and in transit (TLS 1.3). All secrets managed via HSM (FIPS 140-2 Level 3).
6. **Compliance**: GDPR, CCPA, PSD2 (SCA for EU), PCI DSS Level 1.
7. **Multi-Region**: Active-active deployment across 3+ regions. < 100ms latency for 95th percentile globally.
8. **Extensibility**: Connect platform must support 1M+ connected accounts.

### Out of Scope

- Core card network connectivity (handled by acquiring partners)
- KYB (Know Your Business) for merchant onboarding
- Underwriting / credit risk for merchants
- Crypto payments
- Card issuance
- BNPL
- Tax calculation

---

## 2. Users & Scale

### User Types

| User Type | Description | Primary Interactions |
|-----------|-------------|---------------------|
| **Merchant** | Businesses integrating the payment gateway | API calls, dashboard, webhook setup |
| **Cardholder** | End customers paying merchants | JS SDK (tokenization only), payment confirmation |
| **Connected Account** | Marketplace sub-merchant (sellers on a platform) | Receives payouts via Connect; may have limited API access |
| **Platform Developer** | Developer building marketplace on Connect | Platform API calls for routing charges to connected accounts |
| **Internal Ops** | Gateway operations team | Dispute management, fraud review, reconciliation |

### Traffic Estimates

**Assumptions:**
- 5 million active merchants
- 200 transactions/day average per merchant
- Peak-to-average ratio: 15x (Cyber Monday)
- Webhook events per transaction: 4 (created, authorized, captured, receipt)
- Connect platform: 20% of transactions involve a connected account
- Saved cards: 60% of transactions reuse a saved token

| Metric | Calculation | Result |
|--------|-------------|--------|
| Daily transactions | 5M merchants × 200 tx/day | 1,000,000,000 (1B/day) |
| Average TPS | 1B / 86,400s | ~11,574 TPS |
| Peak TPS (all operations) | 11,574 × 15 | ~173,611 TPS |
| Token creation requests/day | 1B × 40% (new cards) | 400M/day |
| Saved token lookups/day | 1B × 60% | 600M/day |
| Webhook events/day | 1B × 4 | 4B/day |
| Webhook delivery attempts/day | 4B × 1.2 (avg 1.2 tries) | 4.8B/day |
| Connect payout calculations/day | 1B × 20% | 200M/day |
| Fraud score requests/day | 1B (all transactions) | 1B/day |
| API requests/day (all types) | 1B × 5 (total API surface) | 5B/day |

**Design target: 200,000 TPS sustained, 500,000 TPS burst** (headroom for growth).

### Latency Requirements

| Operation | P50 | P95 | P99 | Notes |
|-----------|-----|-----|-----|-------|
| Token creation (card vault) | 20ms | 80ms | 150ms | HSM-bound |
| Saved token lookup | 1ms | 5ms | 10ms | Redis cache hit |
| Charge creation (API response) | 50ms | 200ms | 500ms | Before network call |
| End-to-end auth (incl. issuer) | 400ms | 1.5s | 3s | Network-bound |
| Refund creation | 30ms | 100ms | 300ms | Local write |
| Webhook first delivery | 100ms | 500ms | 2s | Merchant server-bound |
| Connect fee calculation | 5ms | 20ms | 50ms | In-memory |
| Fraud score | 20ms | 60ms | 100ms | ML inference |
| Dashboard page load | 100ms | 400ms | 1s | Read replica |

### Storage Estimates

**Assumptions:**
- Token record: ~500B
- Charge record: ~3KB
- Webhook event: ~2KB (payload + delivery log)
- Fraud score event: ~1KB
- Connect transfer record: ~1KB
- Payout record: ~500B
- Retention: tokens indefinitely (for recurring charges); transactions 10 years

| Data Type | Per-record | Daily Volume | Daily Storage | 10-Year Total |
|-----------|-----------|--------------|---------------|---------------|
| Tokens (new cards) | 500B | 400M | 200GB | ~73TB (with dedup by fingerprint) |
| Charge records | 3KB | 1B | 3TB | 10.95PB |
| Webhook events + logs | 2KB | 4.8B | 9.6TB | 35PB |
| Fraud events | 1KB | 1B | 1TB | 3.65PB |
| Connect transfers | 1KB | 200M | 200GB | 730TB |
| Payout records | 500B | 5M | 2.5GB | ~9TB |

**Total hot storage (90 days)**: ~1.2PB. Cold storage (tiered, compressed): 5-10x reduction = ~3-6PB for cold tier.

### Bandwidth Estimates

| Traffic Type | Per-request | RPS | Bandwidth |
|---|---|---|---|
| Inbound API requests | 3KB | 100,000 | 300 MB/s |
| Outbound to acquirers | 1KB | 100,000 | 100 MB/s |
| Acquirer responses | 500B | 100,000 | 50 MB/s |
| Webhook outbound | 2KB | 55,555 (4.8B/day) | 111 MB/s |
| DB replication | ~5× writes | — | ~1.5 GB/s |
| Kafka internal | ~3× events | — | ~900 MB/s |
| **Total** | | | **~3 GB/s** |

---

## 3. High-Level Architecture

```
                  ┌─────────────────────────────────────────────────────────┐
                  │                MERCHANT INTEGRATION LAYER               │
                  │                                                         │
                  │  ┌──────────────┐  ┌─────────────┐  ┌───────────────┐ │
                  │  │  JS SDK      │  │  Mobile SDK  │  │  Server SDK   │ │
                  │  │  (Stripe.js) │  │  iOS/Android │  │  Python/Ruby/ │ │
                  │  │              │  │              │  │  Go/Java/Node │ │
                  │  └──────┬───────┘  └──────┬───────┘  └──────┬────────┘ │
                  └─────────┼────────────────┼─────────────────┼──────────┘
                            │ TLS 1.3        │                 │
                  ┌─────────▼────────────────▼─────────────────▼──────────┐
                  │              GLOBAL ANYCAST / CDN EDGE                 │
                  │         (Cloudflare / AWS CloudFront)                  │
                  │   DDoS protection, TLS termination, geo-routing        │
                  └─────────────────────────┬──────────────────────────────┘
                                            │
                  ┌─────────────────────────▼──────────────────────────────┐
                  │                   API GATEWAY CLUSTER                  │
                  │  Auth (API key / JWT) │ Rate Limiting │ Request Routing│
                  │  Request Logging (scrubbed) │ Load Balancing           │
                  └────┬───────────────┬───────────────┬───────────────────┘
                       │               │               │
          ┌────────────▼──┐  ┌─────────▼───────┐  ┌───▼──────────────────┐
          │  Token         │  │  Core Charge    │  │   Connect Platform   │
          │  Vault API     │  │  Service        │  │   Service            │
          │  Service       │  │  (auth/capture/ │  │   (marketplace fees, │
          │  (PAN→token)   │  │   refund)       │  │    payouts routing)  │
          └────────┬───────┘  └────────┬────────┘  └────────┬─────────────┘
                   │                   │                     │
          ┌────────▼───────────────────▼─────────────────────▼─────────────┐
          │                    SHARED SERVICES LAYER                        │
          │  ┌──────────────┐ ┌──────────────┐ ┌─────────────────────────┐ │
          │  │ Fraud Engine │ │ Idempotency  │ │   Routing Engine        │ │
          │  │ (ML scoring +│ │ Service      │ │   (acquirer selection,  │ │
          │  │  rules)      │ │ (Redis+PG)   │ │    network selection)   │ │
          │  └──────┬───────┘ └──────────────┘ └────────────┬────────────┘ │
          │         │                                        │              │
          │  ┌──────▼──────────────────────────────────────▼────────────┐ │
          │  │               EVENT BUS (Apache Kafka)                    │ │
          │  │  Topics: charges, refunds, disputes, payouts, webhooks,   │ │
          │  │          fraud-signals, connect-transfers, audit          │ │
          │  └──────────────────────────┬────────────────────────────────┘ │
          └────────────────────────────┼────────────────────────────────────┘
                                       │
          ┌────────────────────────────┼────────────────────────────────────┐
          │        DOWNSTREAM CONSUMERS│                                    │
          │  ┌───────────────┐  ┌──────▼──────────┐  ┌───────────────────┐ │
          │  │  Webhook      │  │  Audit / Ledger  │  │  Payout Scheduler │ │
          │  │  Dispatcher   │  │  Service         │  │  (settlement)     │ │
          │  │  (at-least-   │  │  (Cassandra)     │  │                   │ │
          │  │   once HTTP)  │  └─────────────────┘  └───────────────────┘ │
          │  └───────┬───────┘                                              │
          │          │ HTTP callbacks to merchant endpoints                 │
          └──────────┼───────────────────────────────────────────────────── ┘
                     │
          ┌──────────▼──────────────────────────────────────────────────────┐
          │              DATA STORES                                        │
          │  PostgreSQL (charges, tokens_meta, accounts)                   │
          │  Redis Cluster (idempotency, token cache, rate limits)          │
          │  Cassandra (audit log, webhook delivery log)                    │
          │  S3/GCS (evidence files, settlement files, raw event archive)   │
          │  ClickHouse (analytics, fraud feature store, reporting)         │
          └─────────────────────────────────────────────────────────────────┘

          EXTERNAL:
          ┌──────────────────────────────────────────────────────────────────┐
          │  Acquirers: Adyen, Chase Paymentech, Wells Fargo, Worldpay       │
          │  Card Networks: Visa, Mastercard, Amex, Discover                │
          │  Bank Networks: ACH (NACHA), SEPA (EU), FPS (UK)                │
          │  Token Networks: Visa Token Service, Mastercard MDES             │
          │  Fraud Data: Kount, Sift, internal ML                            │
          └──────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

| Component | Role |
|-----------|------|
| **Global Anycast/CDN Edge** | Routes requests to nearest PoP; absorbs DDoS; TLS termination at edge |
| **API Gateway** | Authenticates API keys, enforces rate limits, routes to microservices, scrubs PAN from logs |
| **Token Vault API Service** | Only service that touches raw PANs; HSM-backed encryption; issues vault tokens |
| **Core Charge Service** | Charge lifecycle orchestration; calls idempotency, fraud, routing, vault, acquirer |
| **Connect Platform Service** | Manages connected accounts; computes platform fees; routes funds to sub-accounts |
| **Fraud Engine** | Synchronous ML scoring and rules engine; called in authorization critical path |
| **Idempotency Service** | Redis + Postgres deduplication layer; prevents duplicate charges |
| **Routing Engine** | Selects optimal acquirer per transaction (cost, success rate, failover) |
| **Kafka Event Bus** | Decouples producers from consumers; durable event log for all state changes |
| **Webhook Dispatcher** | Consumes Kafka events; delivers HTTP to merchant endpoints; manages retry, DLQ |
| **Audit/Ledger Service** | Appends immutable records to Cassandra; drives compliance reporting |
| **Payout Scheduler** | Processes merchant payout requests; submits to bank networks |
| **ClickHouse** | Column-store for fast analytical queries (fraud features, merchant reporting) |

**Primary Use-Case Data Flow (Charge with saved card):**

1. Merchant server: `POST /v1/charges { customer: "cus_xxx", amount: 5000, currency: "usd" }`.
2. API Gateway: validate API key, check rate limit, route to Core Charge Service.
3. Core Charge Service: call Idempotency Service — check for existing result.
4. Core Charge Service: call Fraud Engine with transaction + device context → score returned in < 50ms.
5. If score > threshold → decline with `fraudulent` code. If score → 3DS challenge → redirect.
6. Core Charge Service: call Token Vault with customer token → retrieve PAN (in HSM process).
7. Routing Engine: select acquirer (Adyen for EU, Chase for US) based on card BIN, currency, acquirer health.
8. Acquirer call: ISO 8583 auth request → card network → issuer → response.
9. Core Charge Service: persist charge record (status = AUTHORIZED), update idempotency cache.
10. Publish `charge.authorized` event to Kafka.
11. Kafka consumers: Audit Service (Cassandra append), Webhook Dispatcher (deliver to merchant).
12. Return HTTP 201 to merchant with charge object.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- MERCHANTS (gateway accounts)
-- ============================================================
CREATE TABLE merchants (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    email               VARCHAR(255)    NOT NULL UNIQUE,
    name                VARCHAR(255)    NOT NULL,
    country             CHAR(2)         NOT NULL,
    default_currency    CHAR(3)         NOT NULL DEFAULT 'usd',
    account_type        VARCHAR(20)     NOT NULL DEFAULT 'standard',  -- 'standard', 'express', 'custom'
    platform_id         UUID            REFERENCES merchants(id),     -- Connect: parent platform
    status              VARCHAR(20)     NOT NULL DEFAULT 'active',
    payout_schedule     JSONB           NOT NULL DEFAULT '{"interval": "daily"}',
    metadata            JSONB,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- CUSTOMERS (end-user payment profiles per merchant)
-- ============================================================
CREATE TABLE customers (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL REFERENCES merchants(id),
    external_id         VARCHAR(255),   -- Merchant's own customer ID
    email               VARCHAR(255),
    name                VARCHAR(255),
    phone               VARCHAR(50),
    metadata            JSONB,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_merchant_external_customer UNIQUE (merchant_id, external_id)
);

CREATE INDEX idx_customers_merchant_id ON customers (merchant_id);

-- ============================================================
-- PAYMENT METHODS (tokenized cards & bank accounts; no raw PAN)
-- ============================================================
CREATE TABLE payment_methods (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL REFERENCES merchants(id),
    customer_id         UUID            REFERENCES customers(id),
    type                VARCHAR(20)     NOT NULL,       -- 'card', 'us_bank_account', 'sepa_debit'
    vault_token_id      VARCHAR(255)    NOT NULL,       -- Reference to HSM vault
    network_token_id    VARCHAR(255),                   -- Visa/MC network token if enrolled
    card_fingerprint    VARCHAR(64)     NOT NULL,       -- HMAC of PAN; dedup without PAN exposure
    -- Card-specific display fields (non-sensitive):
    last4               CHAR(4),
    exp_month           SMALLINT,
    exp_year            SMALLINT,
    brand               VARCHAR(20),    -- 'visa', 'mastercard', 'amex', 'discover'
    funding             VARCHAR(20),    -- 'credit', 'debit', 'prepaid', 'unknown'
    issuer_country      CHAR(2),
    bin                 VARCHAR(8),
    -- Bank account fields:
    bank_last4          CHAR(4),
    routing_number_last4 CHAR(4),
    account_holder_type VARCHAR(20),    -- 'individual', 'company'
    status              VARCHAR(20)     NOT NULL DEFAULT 'active',  -- 'active', 'expired', 'detached'
    three_d_secure_usage JSONB,         -- {supported: true}
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_payment_methods_customer_id ON payment_methods (customer_id);
CREATE INDEX idx_payment_methods_fingerprint ON payment_methods (card_fingerprint);

-- ============================================================
-- PAYMENT INTENTS (modern API — tracks full lifecycle)
-- ============================================================
CREATE TABLE payment_intents (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL REFERENCES merchants(id),
    customer_id         UUID            REFERENCES customers(id),
    amount              BIGINT          NOT NULL,       -- minor units
    currency            CHAR(3)         NOT NULL,
    status              pi_status       NOT NULL DEFAULT 'requires_payment_method',
    capture_method      VARCHAR(20)     NOT NULL DEFAULT 'automatic',  -- 'automatic', 'manual'
    confirmation_method VARCHAR(20)     NOT NULL DEFAULT 'automatic',
    payment_method_id   UUID            REFERENCES payment_methods(id),
    description         VARCHAR(1000),
    statement_descriptor VARCHAR(22),   -- What appears on card statement (Visa: max 22 chars)
    idempotency_key     VARCHAR(255),
    application_fee_amount BIGINT,      -- Connect: platform fee
    transfer_data       JSONB,          -- Connect: { destination: 'acct_xxx', amount: ... }
    on_behalf_of        UUID            REFERENCES merchants(id),  -- Connect: charge on behalf of
    setup_future_usage  VARCHAR(20),    -- 'on_session', 'off_session'
    three_d_secure_data JSONB,
    fraud_score         SMALLINT,
    fraud_recommendation VARCHAR(20),
    metadata            JSONB,
    client_secret       VARCHAR(255)    NOT NULL UNIQUE,  -- Used by client to confirm
    cancellation_reason VARCHAR(50),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    version             BIGINT          NOT NULL DEFAULT 0
);

CREATE TYPE pi_status AS ENUM (
    'requires_payment_method',
    'requires_confirmation',
    'requires_action',
    'processing',
    'requires_capture',
    'canceled',
    'succeeded'
);

CREATE INDEX idx_pi_merchant_id ON payment_intents (merchant_id);
CREATE INDEX idx_pi_customer_id ON payment_intents (customer_id);
CREATE INDEX idx_pi_created_at ON payment_intents (created_at DESC);
CREATE INDEX idx_pi_status ON payment_intents (status) WHERE status NOT IN ('succeeded', 'canceled');

-- ============================================================
-- CHARGES (individual charge attempts; a PaymentIntent may have multiple)
-- ============================================================
CREATE TABLE charges (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    payment_intent_id   UUID            NOT NULL REFERENCES payment_intents(id),
    merchant_id         UUID            NOT NULL,
    amount              BIGINT          NOT NULL,
    amount_captured     BIGINT          NOT NULL DEFAULT 0,
    amount_refunded     BIGINT          NOT NULL DEFAULT 0,
    currency            CHAR(3)         NOT NULL,
    status              VARCHAR(20)     NOT NULL,  -- 'succeeded', 'failed', 'pending'
    paid                BOOLEAN         NOT NULL DEFAULT FALSE,
    captured            BOOLEAN         NOT NULL DEFAULT FALSE,
    refunded            BOOLEAN         NOT NULL DEFAULT FALSE,
    disputed            BOOLEAN         NOT NULL DEFAULT FALSE,
    payment_method_id   UUID            NOT NULL REFERENCES payment_methods(id),
    outcome_type        VARCHAR(20),    -- 'authorized', 'manual_review', 'issuer_declined', 'blocked'
    outcome_network_status VARCHAR(50),
    outcome_reason      VARCHAR(100),
    outcome_risk_level  VARCHAR(20),    -- 'normal', 'elevated', 'highest'
    outcome_risk_score  SMALLINT,
    failure_code        VARCHAR(50),
    failure_message     VARCHAR(500),
    acquirer_id         UUID,
    network_transaction_id VARCHAR(100),
    auth_code           VARCHAR(20),
    balance_transaction_id UUID,        -- FK to ledger balance_transactions
    application_fee_id  UUID,           -- Connect: fee charged to connected account
    receipt_email       VARCHAR(255),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- WEBHOOK ENDPOINTS (merchant configurations)
-- ============================================================
CREATE TABLE webhook_endpoints (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL REFERENCES merchants(id),
    url                 VARCHAR(2048)   NOT NULL,
    secret_key          VARCHAR(64)     NOT NULL,   -- Used to sign webhook payloads (HMAC-SHA256)
    enabled_events      TEXT[]          NOT NULL,   -- ['charge.succeeded', 'refund.created', ...]
    status              VARCHAR(20)     NOT NULL DEFAULT 'enabled',
    api_version         VARCHAR(20)     NOT NULL DEFAULT '2024-06-20',
    description         VARCHAR(500),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- WEBHOOK EVENTS (delivery log)
-- ============================================================
CREATE TABLE webhook_events (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL,
    endpoint_id         UUID            NOT NULL REFERENCES webhook_endpoints(id),
    event_type          VARCHAR(100)    NOT NULL,
    event_data          JSONB           NOT NULL,   -- Full event payload
    status              VARCHAR(20)     NOT NULL DEFAULT 'pending',  -- pending, delivered, failed
    attempts            SMALLINT        NOT NULL DEFAULT 0,
    last_attempt_at     TIMESTAMPTZ,
    next_retry_at       TIMESTAMPTZ,
    delivered_at        TIMESTAMPTZ,
    http_status_code    SMALLINT,
    response_body       VARCHAR(1000),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_webhook_events_endpoint ON webhook_events (endpoint_id, status, next_retry_at)
    WHERE status IN ('pending', 'failed');
CREATE INDEX idx_webhook_events_merchant ON webhook_events (merchant_id, created_at DESC);

-- ============================================================
-- CONNECT: PLATFORM FEES AND TRANSFERS
-- ============================================================
CREATE TABLE application_fees (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES merchants(id),  -- connected account
    platform_id         UUID            NOT NULL REFERENCES merchants(id),  -- platform
    charge_id           UUID            NOT NULL REFERENCES charges(id),
    amount              BIGINT          NOT NULL,
    currency            CHAR(3)         NOT NULL,
    amount_refunded     BIGINT          NOT NULL DEFAULT 0,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TABLE transfers (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    source_type         VARCHAR(20),    -- 'charge'
    source_id           UUID,
    destination_id      UUID            NOT NULL REFERENCES merchants(id),
    amount              BIGINT          NOT NULL,
    currency            CHAR(3)         NOT NULL,
    description         VARCHAR(500),
    metadata            JSONB,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- PAYOUTS (merchant bank account transfers)
-- ============================================================
CREATE TABLE payouts (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL REFERENCES merchants(id),
    amount              BIGINT          NOT NULL,
    currency            CHAR(3)         NOT NULL,
    method              VARCHAR(20)     NOT NULL DEFAULT 'standard',  -- 'standard', 'instant'
    type                VARCHAR(20)     NOT NULL DEFAULT 'bank_account',
    status              payout_status   NOT NULL DEFAULT 'pending',
    destination_id      UUID,           -- bank account token
    bank_account_last4  CHAR(4),
    failure_code        VARCHAR(50),
    failure_message     VARCHAR(255),
    arrival_date        DATE,
    statement_descriptor VARCHAR(22),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TYPE payout_status AS ENUM ('pending', 'in_transit', 'paid', 'failed', 'canceled');
```

### Database Choice

**Options Evaluated:**

| Database | Use Case Fit | Pros | Cons | Decision |
|----------|-------------|------|------|----------|
| **PostgreSQL** | ACID transactional data (charges, customers, merchants) | Strong consistency, rich SQL, JSONB, mature ecosystem | Horizontal scaling requires sharding | **Selected: core transactional** |
| **Redis Cluster** | Idempotency keys, token cache, rate limits, session | Sub-millisecond, atomic operations, TTL native | Memory-only primary; not durable as sole store | **Selected: cache/rate-limit layer** |
| **Cassandra** | Webhook delivery log, audit trail, fraud events | Extreme write throughput, append-only, tunable consistency, multi-DC | No transactions, limited query flexibility | **Selected: append-only logs** |
| **ClickHouse** | Fraud feature store, merchant analytics, reporting | Column-store OLAP, 100x faster aggregations than PG | Not suitable for OLTP, eventual consistency | **Selected: analytics/fraud features** |
| **DynamoDB** | N/A | Serverless scaling | Vendor lock-in, limited query model | Not selected |

**Justification:**
- PostgreSQL chosen for charges/payment_intents because ACID transactions prevent double-capture and double-refund invariants. The `version` column with optimistic locking ensures concurrent modifications are detected. CHECK constraints provide a data-layer safety net independent of application bugs.
- Redis chosen for idempotency cache because SETNX is atomic and 1000x faster than a Postgres lock for the 99.9% case where a key is unique. Redis TTL handles automatic expiry. The Postgres idempotency table is the durable fallback.
- Cassandra for webhook logs because the access pattern is pure append (write event, update status) with reads filtered by merchant+date — a pattern Cassandra handles efficiently with partition key `(merchant_id)` and clustering key `(created_at, id)`.
- ClickHouse for fraud feature computation because velocity features (how many charges from this card in last 1 hour) require fast aggregations over 1B+ rows — a query ClickHouse can answer in < 50ms vs. PostgreSQL's minutes.

---

## 5. API Design

Authentication: All requests use `Authorization: Bearer <secret_key>` or `Authorization: Basic base64(sk_live_xxx:)`. Client-side operations use `pk_live_xxx` (publishable key — no server-side auth, scope-limited to token creation only). All mutating requests require `Idempotency-Key` header.

```
POST   /v1/payment_intents
  Auth: Secret key
  Headers: Idempotency-Key: <uuid>
  Rate Limit: 1000/min per merchant
  Body: {
    "amount": 2000,
    "currency": "usd",
    "payment_method": "pm_xxx",        // optional; can attach later
    "customer": "cus_xxx",
    "capture_method": "automatic",
    "confirm": true,                   // confirm immediately or wait
    "return_url": "https://...",       // for redirect flows (3DS, bank auth)
    "description": "Order #12345",
    "statement_descriptor": "ACME",
    "application_fee_amount": 100,     // Connect: platform fee in minor units
    "transfer_data": { "destination": "acct_xxx" },
    "metadata": { "order_id": "12345" }
  }
  Response 201: {
    "id": "pi_xxx",
    "status": "succeeded",
    "amount": 2000,
    "amount_capturable": 0,
    "amount_received": 2000,
    "client_secret": "pi_xxx_secret_yyy",
    "charges": { "data": [{ "id": "ch_xxx", ... }] }
  }

POST   /v1/payment_intents/:id/confirm
  Auth: Secret key (server) or publishable key (client SDK)
  Headers: Idempotency-Key: <uuid>
  Body: { "payment_method": "pm_xxx", "return_url": "https://..." }
  Response 200: { ...payment_intent with updated status... }
  Note: Client secret required in body when using publishable key

POST   /v1/payment_intents/:id/capture
  Auth: Secret key
  Headers: Idempotency-Key: <uuid>
  Body: { "amount_to_capture": 2000 }
  Response 200: { "id": "pi_xxx", "status": "succeeded", "amount_received": 2000 }
  Errors: 400 (not in requires_capture), 422 (amount > capturable)

POST   /v1/payment_intents/:id/cancel
  Auth: Secret key
  Body: { "cancellation_reason": "requested_by_customer" }
  Response 200: { "id": "pi_xxx", "status": "canceled" }

GET    /v1/payment_intents/:id
  Auth: Secret key
  Response 200: { full payment intent object }

GET    /v1/payment_intents
  Auth: Secret key
  Query: ?customer=cus_xxx&limit=10&starting_after=pi_xxx&created[gte]=1700000000
  Pagination: Cursor-based on created_at + id (stable sort). Max limit=100.
  Response 200: { "data": [...], "has_more": true }

POST   /v1/payment_methods
  Auth: Publishable key (client-side) OR secret key (server-side with raw card)
  Headers: Idempotency-Key: <uuid>
  Body: {
    "type": "card",
    "card": {
      "number": "4242424242424242",
      "exp_month": 12,
      "exp_year": 2028,
      "cvc": "314"
    },
    "billing_details": { "name": "Jane Doe", "address": { ... } }
  }
  Response 201: {
    "id": "pm_xxx",
    "type": "card",
    "card": { "brand": "visa", "last4": "4242", "exp_month": 12, "exp_year": 2028 }
  }
  Note: Raw card accepted ONLY on PCI-isolated token endpoint; never stored as-is

POST   /v1/payment_methods/:id/attach
  Auth: Secret key
  Body: { "customer": "cus_xxx" }
  Response 200: { payment method with customer attached }

POST   /v1/payment_methods/:id/detach
  Auth: Secret key
  Response 200: { "id": "pm_xxx", "customer": null }

POST   /v1/refunds
  Auth: Secret key
  Headers: Idempotency-Key: <uuid>
  Body: {
    "payment_intent": "pi_xxx",        // or "charge": "ch_xxx"
    "amount": 1000,
    "reason": "requested_by_customer"
  }
  Response 201: { "id": "re_xxx", "status": "pending", "amount": 1000 }

GET    /v1/customers/:id
POST   /v1/customers
GET    /v1/customers/:id/payment_methods

POST   /v1/webhook_endpoints
  Auth: Secret key
  Body: {
    "url": "https://merchant.com/stripe-webhooks",
    "enabled_events": ["charge.succeeded", "payment_intent.payment_failed"]
  }
  Response 201: { "id": "we_xxx", "secret": "whsec_xxx" }  // secret shown once

GET    /v1/webhook_endpoints
GET    /v1/webhook_endpoints/:id
POST   /v1/webhook_endpoints/:id     // update
DELETE /v1/webhook_endpoints/:id

POST   /v1/accounts                  // Connect: create connected account
GET    /v1/accounts/:id/balance
POST   /v1/transfers
POST   /v1/payouts
GET    /v1/payouts

POST   /v1/charges                   // Legacy Charges API (backward compat)
  Auth: Secret key
  Headers: Idempotency-Key: <uuid>
  Body: { "amount": 2000, "currency": "usd", "source": "tok_xxx", "description": "..." }
  Response 201: { charge object }
```

---

## 6. Deep Dive: Core Components

### 6.1 Payment Method Tokenization (Vault)

**Problem it solves:**

Raw PANs entering merchant servers would put those servers in PCI scope. The tokenization vault solves this by accepting PAN only in an isolated, HSM-backed environment and returning an opaque token. Merchants store and transmit tokens; the vault maps token → PAN only at authorization time, keeping the entire merchant-facing system out of PCI scope.

**Approaches:**

| Approach | PCI Scope | Security | Reusability |
|----------|-----------|----------|-------------|
| **Hosted fields (iframe)** | PAN enters our iframe, not merchant DOM | High; JS injection attacks contained | Token is reusable |
| **Client-side encryption (CSE)** | Merchant JS encrypts PAN; sends ciphertext to their server | Medium; merchant server handles encrypted blob | Token issued after server-side decryption |
| **Network tokenization only (VTS/MDES)** | PAN goes to card network; network issues token | Highest (network-level) | Token tied to merchant domain |
| **HSM vault tokenization** | PAN enters vault; token issued | High; HSM-isolated | Token indefinitely reusable |
| **Combined: hosted fields + vault + network token** | PAN only in iframe + HSM + card network | Highest | Full flexibility |

**Selected: Hosted Fields → HSM Vault → Network Token enrollment**

**Implementation Detail:**

```
Tokenization Flow:
1. Merchant embeds <div id="card-element"></div> in checkout page.
2. Our JS SDK (Stripe.js) injects an <iframe> hosted on our PCI domain.
   The iframe renders the card input fields — merchant DOM cannot read them.
   Content-Security-Policy: frame-ancestors <merchant-domain>.
3. Customer enters card data in iframe.
4. On submit: JS SDK encrypts PAN with ephemeral RSA-4096 public key (rotated hourly).
   POST https://api.stripe.com/v1/payment_methods to token endpoint.
   TLS 1.3 + certificate pinning in mobile SDK.
5. Token Service receives encrypted payload:
   a. Decrypt with RSA private key in HSM boundary.
   b. Perform Luhn check, BIN validation, basic card validation.
   c. Generate vault_token = secure_random(UUID).
   d. AES-256-GCM encrypt PAN with data encryption key (DEK).
   e. Store: vault_token → AES-encrypted(PAN) in vault store (isolated namespace).
      DEK stored in HSM, never exported to application memory.
   f. Compute card_fingerprint = HMAC-SHA256(PAN, static_key_in_HSM).
      Used for deduplication without PAN exposure.
   g. Return payment_method_id (public) linked to vault_token (internal).
6. Merchant stores payment_method_id; never sees PAN.

Network Token Enrollment (background, after step 5):
1. For Visa cards: call Visa Token Service (VTS) with PAN.
2. VTS issues a 16-digit network token + cryptogram.
3. Update payment_methods.network_token_id.
4. Future authorizations use network token instead of PAN.
   Benefit: issuer updates token when card re-issued → no more declines.

Key Hierarchy:
  - Master Key (MK): HSM-stored, never exported. Used to wrap KEKs.
  - Key Encryption Key (KEK): Per-vault-cluster. Wrapped by MK. Used to wrap DEKs.
  - Data Encryption Key (DEK): Per-batch of records. Wrapped by KEK.
    Stored alongside encrypted data. To decrypt: unwrap DEK using KEK (in HSM), decrypt record.
  - HMAC Key: Static, HSM-stored. Used only for fingerprint computation.
  
Key Rotation:
  - DEKs rotated monthly (re-encrypt subset of records in background).
  - KEKs rotated annually (re-wrap all DEKs).
  - MK rotated every 3 years (requires hardware ceremony with multiple custodians).
  - Zero downtime: new and old DEKs valid during rotation window.
```

**5 Interviewer Q&As:**

Q: How do you prevent the iframe from being hijacked by a compromised merchant site?
A: Multiple defenses: (1) Content-Security-Policy with `frame-ancestors` limits which domains can embed our iframe. (2) The iframe is served from a separate origin (`js.stripe.com`) — browser same-origin policy prevents merchant JS from reading iframe contents. (3) Subresource Integrity (SRI) hashes on the SDK script ensure the merchant loads an unmodified version. (4) Strict HTTPS with HSTS and certificate pinning in mobile SDKs. (5) We monitor for unusual iframe embedding domains and alert security teams.

Q: If the vault is compromised and tokens are stolen, what's the blast radius?
A: A stolen vault token is only useful to an attacker who also has access to our vault service to detokenize it. The token is an opaque UUID — no value without the vault. To mitigate: (1) Vault service has network isolation (only reachable by the Core Charge Service, not the internet). (2) Every vault access is audited (rate of detokenization attempts per token is tracked). (3) Anomaly detection: if a token is detokenized 1000x in 1 hour, alert security. (4) Even with vault access, the attacker still needs to route through a card network — Visa/MC fraud detection would flag abnormal velocity. (5) Network tokens (VTS/MDES) are domain-bound — stolen network tokens can't be used by a different merchant.

Q: How do you handle card updates when a card expires or is re-issued?
A: Two mechanisms: (1) **Automatic Network Tokenization Updates**: Visa Token Service and Mastercard MDES push updates to network tokens when the underlying card changes — expiry updates and re-issuance are handled transparently. The vault token remains valid. (2) **Account Updater**: For PANs not enrolled in network tokenization, we query the Visa/MC Account Updater service (batch, nightly) with our stored cards. The service returns new PANs/expiry dates. We update our vault records and push the update to `payment_methods` (new expiry, new last4 if PAN changed). We notify merchants via `payment_method.automatically_updated` webhook.

Q: How do you deduplicate cards across customers or merchants?
A: The `card_fingerprint` — `HMAC-SHA256(PAN, stable_hmac_key)` — is deterministic for the same PAN regardless of who tokenizes it. This allows: (1) A merchant to detect when the same customer uses the same card under different email addresses (fraud signal). (2) Cross-merchant deduplication in the vault (store PAN once; multiple merchants reference the same encrypted record). (3) Blocked card lists — if a card has a confirmed fraud chargeback, add its fingerprint to a blocklist; all future tokenizations of that PAN are blocked even if the number isn't in our blocklist directly.

Q: How do you handle CVV? Is it ever stored?
A: CVV (Card Verification Value) is accepted in the tokenization iframe, included in the first authorization only, and then immediately discarded. We have no column in any table for CVV. The PCI DSS standard (Requirement 3.3) explicitly prohibits storing CVV after authorization. The vault only stores the PAN — CVV is a challenge value for card-not-present authentication, not for recurring charges. For MIT (Merchant-Initiated Transactions) like subscriptions, CVV is not required because prior cardholder consent was established.

---

### 6.2 Webhook Delivery Guarantees

**Problem it solves:**

Merchants need reliable notification of payment events to trigger order fulfillment, send receipts, update inventory, etc. The challenge: HTTP delivery to merchant endpoints is unreliable (merchant servers can be down, slow, or return errors). We need at-least-once delivery with durability guarantees, retry logic, monitoring, and a dead-letter queue — without blocking the main payment flow.

**Approaches:**

| Approach | Delivery Guarantee | Latency | Complexity |
|----------|-------------------|---------|------------|
| **Synchronous HTTP in payment flow** | Best-effort (blocks auth if merchant down) | Low | Low |
| **Async queue + single delivery attempt** | At-most-once | Medium | Low |
| **Kafka + consumer workers + retry queue** | At-least-once | Medium | High |
| **Transactional outbox pattern + Kafka** | At-least-once (no event loss even on crash) | Medium | High |
| **Exactly-once delivery** | Exactly-once | High | Very high (requires 2PC with merchant) |

**Selected: Transactional Outbox → Kafka → Delivery Workers with Retry + DLQ**

**Implementation Detail:**

```
Delivery Pipeline:

STEP 1: Event Creation (in main payment transaction)
  BEGIN;
    UPDATE charges SET status = 'succeeded' WHERE id = :id;
    INSERT INTO webhook_outbox (
      event_type = 'charge.succeeded',
      payload = {serialized charge object},
      merchant_id = :merchant_id,
      published = FALSE
    );
  COMMIT;
  -- Event is created atomically with the charge update.
  -- If the transaction rolls back, no event is created. No phantom events.

STEP 2: Outbox Poller (CDC via Debezium or periodic polling)
  -- Polls webhook_outbox WHERE published = FALSE every 100ms.
  -- Publishes to Kafka topic 'webhook-events' partitioned by merchant_id.
  -- Marks published = TRUE after Kafka ACK.
  -- Idempotent: Kafka message key = outbox event ID; duplicate publishes deduped by Kafka.

STEP 3: Kafka Topic 'webhook-events'
  -- 96 partitions (enough for 96 parallel consumer groups).
  -- Partition key: merchant_id (ensures ordering per merchant).
  -- Retention: 7 days.

STEP 4: Delivery Workers (consumer group 'webhook-delivery')
  For each event:
    1. Look up all matching webhook endpoints for merchant + event_type.
    2. For each endpoint:
       a. Sign payload: HMAC-SHA256(payload, endpoint.secret_key).
          Header: Stripe-Signature: t=<timestamp>,v1=<hmac>
       b. HTTP POST to endpoint.url with 30s timeout.
       c. Verify response: 200-299 = success; all others = failure.
    3. On success: update webhook_events.status = 'delivered', delivered_at = NOW().
    4. On failure: increment attempts; compute next_retry_at:
       Retry schedule (exponential with jitter):
         Attempt 1: immediate
         Attempt 2: 1 minute
         Attempt 3: 5 minutes
         Attempt 4: 30 minutes
         Attempt 5: 2 hours
         Attempt 6: 5 hours
         Attempt 7-10: 8 hours each
         Max retries: 10 over ~72 hours
    5. After max retries: move to DLQ (webhook_events.status = 'failed').
       Alert merchant in dashboard: "Webhook delivery failed".
    6. Merchant can manually retry from dashboard.

STEP 5: Delivery Monitoring
  Kafka consumer lag: webhook_delivery lag > 10K events → scale workers horizontally.
  Dashboard metric per endpoint: "Last successful delivery", "Failure rate last 24h".
  P90 delivery latency tracked per merchant endpoint.

Signature Verification (Merchant Side):
  1. Receive HTTP POST with headers: Stripe-Signature: t=1623000000,v1=abc123...
  2. Extract timestamp t and signature v1.
  3. Compute expected_sig = HMAC-SHA256(t + '.' + raw_body, webhook_secret).
  4. Compare signatures (constant-time comparison to prevent timing attacks).
  5. Reject if |NOW() - t| > 300 seconds (5-minute replay protection).
  6. Reject if request body has been modified (HMAC will differ).
```

**5 Interviewer Q&As:**

Q: How do you prevent replaying a webhook event to trigger duplicate order fulfillment?
A: The webhook payload includes the `id` of the event object (e.g., `evt_xxx`). Merchant implementations should record processed event IDs in their database and skip events with already-processed IDs. We document this as the recommended idempotent webhook handler pattern. Additionally, the `Stripe-Signature` timestamp prevents replaying old events (5-minute window). We recommend merchants also persist the charge/payment_intent ID and check against it before fulfilling.

Q: How do you handle a merchant endpoint that is down for 3 days and suddenly comes back?
A: Our retry window is 72 hours. After 10 attempts, the event enters DLQ status but is still stored in our database. When the merchant fixes their endpoint and re-enables it, they can see all failed events in the dashboard and replay them individually or in bulk. Our bulk replay API: `POST /v1/webhook_endpoints/:id/test` for a single test event, or the dashboard can trigger a re-delivery of all DLQ events. Kafka topic retention (7 days) also means we can re-consume and re-deliver if needed within that window.

Q: What's the ordering guarantee for webhook events?
A: We guarantee ordering per merchant (same Kafka partition for all events of a given merchant). However, delivery to the merchant's HTTP endpoint doesn't guarantee order — if attempt 1 of event A is retrying while event B is delivered successfully, B arrives before A. Merchants should design their webhook handlers to be order-independent (process the most recent state of an object, not rely on sequence). We always include the full object state in the payload, not just a diff, so the handler has the complete picture.

Q: How do you scale webhook delivery to 100,000 events/second?
A: At 100K events/sec (8.64B/day), with 96 Kafka partitions and each consumer making a 50ms HTTP call, each worker can handle 20 events/sec, requiring 5,000 workers. Autoscaling based on consumer lag: when lag > threshold, add workers. Workers are stateless K8s pods. Bottleneck is usually the merchant endpoint (they're slow or flaky), so we also implement per-endpoint concurrency limits (max 5 concurrent requests to a single endpoint URL to avoid overwhelming a small merchant's server).

Q: How do you handle a malicious merchant's endpoint that sends 200 OK but performs no action?
A: We can't guarantee merchant-side idempotency — only delivery. If a merchant acknowledges receipt but doesn't process the event, that's their responsibility. What we can do: (1) Our SDK includes helpers for idempotent event processing. (2) For critical flows (like payout.paid), the merchant can verify against our API (`GET /v1/payouts/:id`) rather than relying solely on webhooks. (3) We expose an event log (`GET /v1/events`) that merchants can poll as a backup. Webhooks are optimistic; the API is authoritative.

---

### 6.3 Connect Platform (Marketplace Payments)

**Problem it solves:**

Platforms like marketplaces (Airbnb, Etsy, Lyft) need to collect money from buyers, pay sellers, and take a platform fee — all with a single checkout flow. Connect enables platforms to route payments to connected accounts (sellers) while the platform retains a fee. This requires complex fund routing, multi-party settlement, and compliance (KYC for each connected account).

**Approaches:**

| Approach | Control | Complexity | Fee Flexibility |
|----------|---------|------------|----------------|
| **Direct charges** | Charge created on connected account; platform has no funds liability | Low | Platform fee from connected account balance |
| **Destination charges** | Charge on platform; transfer to destination; platform takes fee | Medium | Full control over fee structure |
| **On-behalf-of charges** | Charge on platform but settled to connected account; branding of connected account | High | Platform fee via application_fee_amount |
| **Manual transfers** | Collect all funds on platform; manually transfer to sellers | Highest | Maximum control; highest compliance burden |

**Selected: All three supported; destination charges as default for marketplace use cases.**

**Implementation Detail:**

```
Destination Charge Flow (most common marketplace pattern):
  Platform: sk_live_platform_key
  Connected Account: acct_seller_xxx

1. Platform creates charge with transfer_data:
   POST /v1/payment_intents {
     amount: 10000,          // $100.00 buyer pays
     currency: 'usd',
     transfer_data: {
       destination: 'acct_seller_xxx',
       amount: 8500            // $85.00 to seller
     },
     application_fee_amount: 500   // $5.00 platform fee (remaining $9.50 covers Stripe fees)
   }

2. Core Charge Service:
   a. Charges buyer $100.00 normally.
   b. Creates an ApplicationFee record: $5.00 to platform.
   c. Creates a Transfer: $85.00 to connected account acct_seller_xxx.
   d. Platform retains: $100 - $85 - payment processing fee - $5 platform fee = net platform balance change.

3. Settlement:
   a. Platform's Stripe balance increases by $10.00 (less processing fees).
   b. Transfer of $85.00 creates a debit on platform balance, credit on connected account balance.
   c. Application fee of $5.00 stays in platform balance.
   d. Net platform balance change = $10.00 - $85.00 + $5.00 = -$70.00... 
      Wait: correct accounting: platform receives $100, transfers $85, collects $5 fee.
      Platform net = $100 - $85 = $15 gross; processing fees ~$3 (3%); platform fee revenue = $5; net = $12.
   e. Connected account balance = +$85.00 (payable on their payout schedule).

4. Payout Handling:
   a. Connected account accumulates balance from transfers.
   b. Platform sets connected account payout schedule (daily, weekly, on-demand).
   c. Payout Service creates a bank transfer via ACH/wire to connected account's bank.

KYC/AML for Connected Accounts:
  Standard accounts: Self-serve KYC (connected account completes Stripe onboarding).
  Express accounts: Stripe hosts the KYC UI; platform controls branding.
  Custom accounts: Platform collects KYC data, passes to Stripe via API.
  
  Required data: business type, legal name, date of birth (for individuals), SSN last 4,
                  business address, MCC, bank account for payouts.
  Stripe verifies against: LexisNexis, Equifax, OFAC watchlists, global sanctions.

Connect Fee Scenarios:
  application_fee_amount: flat fee or percentage
  transfer_data.amount:   controls how much goes to connected account
  No transfer_data:       platform keeps all funds (then manually transfers later)
```

**5 Interviewer Q&As:**

Q: How do you handle refunds in a destination charge scenario?
A: Refunding a destination charge unwinds the transfer. If the full amount is refunded: (1) The buyer's card is refunded $100. (2) Stripe retrieves $85 from the connected account (or reverses the transfer). (3) The $5 application fee is reversed to the platform. The platform needs to have sufficient balance or the connected account must have sufficient balance for the reversal. If the connected account has insufficient balance, the refund fails and the platform must fund it from their own balance. We expose `transfer_reversal_amount` in the refund object for clarity.

Q: How do you handle disputes (chargebacks) on Connect charges?
A: The dispute debits the platform account (since it's a destination charge). The platform can then recover from the connected account by reversing the transfer. We expose a `dispute.funds_withdrawn` event. The platform can configure `on_behalf_of` so that the connected account's branding appears on bank statements (reducing friendly fraud) and disputes are reflected on the connected account. The platform is responsible for providing dispute evidence but can require connected accounts to submit it.

Q: What prevents a platform from setting `application_fee_amount` higher than the charge amount?
A: Application fee cannot exceed charge amount — enforced with a CHECK constraint and API validation. Error: `application_fee_amount must be less than or equal to the charge amount`. Also, `transfer_data.amount + application_fee_amount` must not exceed the charge amount minus payment processing fees. We validate this in the API layer and return a 422 with a clear error message.

Q: How do you handle currency differences between platform and connected account?
A: The connected account can have a different default currency. When transferring across currencies: (1) Platform charges in USD; connected account settles in EUR. (2) Transfer triggers FX conversion at our real-time rate. (3) The connected account receives EUR; the exchange rate and any FX fee are recorded on the transfer object. (4) FX rate is locked at the time of transfer creation. Merchants can opt into `destination_payment_currency` to fix the rate at authorization time.

Q: How do you prevent connected accounts from being used for money laundering?
A: Multiple controls: (1) KYC/AML at onboarding — identity verification, business verification, OFAC screening. (2) Transaction monitoring: real-time rules for suspicious patterns (sudden volume spike, unusual geographies, high refund rates). (3) Payout delays for new connected accounts (funds held for 7 days to allow chargebacks). (4) Risk-based velocity limits: new accounts have lower volume limits, increased as track record builds. (5) Ongoing monitoring: monthly re-screening against sanctions lists. (6) SAR (Suspicious Activity Report) filing when legally required. (7) Connected account can be suspended via `POST /v1/accounts/:id` with `disabled_reason`.

---

## 7. Scaling

**Horizontal Scaling:**

- **Token Vault Service**: Critical path; scale to 20 pods minimum, auto-scale based on CPU. HSM access is the bottleneck — HSMs support ~10,000 cryptographic operations/sec; use a pool of HSMs with load balancing.
- **Core Charge Service**: Stateless; 50 pods at peak, each handling 2,000 RPS. Auto-scale based on request queue depth.
- **Fraud Engine**: GPU-backed ML inference; scale inference workers independently from rule engine. Target 50ms P99 for the combined score.
- **Webhook Dispatcher**: 1,000 pods at peak (100K events/sec, 100 deliveries/pod/sec at 10ms/delivery).
- **Kafka**: 96 partitions across 12 broker nodes (3 AZs × 4). Rack-aware replication factor 3.

**DB Sharding:**

Shard by `merchant_id` for charges and payment_intents. This co-locates all of a merchant's data on one shard, enabling efficient queries like `GET /v1/charges?limit=100` (single-shard scan). Virtual shards (2048) → physical nodes. Token vault sharded by `vault_token_id` (random UUID, naturally distributed).

**Caching Strategy:**

| Data | Cache Type | TTL | Notes |
|------|-----------|-----|-------|
| API keys | Redis hash | 5m | Invalidate on key rotation |
| Rate limit counters | Redis sorted set | 60s sliding window | Per-merchant, per-endpoint |
| Idempotency keys | Redis string | 24h | SETNX for first write |
| Saved payment method (token→metadata) | Redis hash | 1h | last4, brand, expiry for display |
| Fraud feature vectors | ClickHouse + Redis | 1m | Velocity counts |
| BIN database | Redis hash | 24h | Card brand/type by BIN prefix |
| Merchant config | Redis hash | 5m | Webhook URLs, rate limits |

**CDN:**

Stripe.js (our tokenization script) is served via CDN (Cloudflare) with SRI hash. Merchant dashboard static assets (React app, CSS) via CDN. API responses are NOT cached (financial data must be fresh); only static assets use CDN.

**Interviewer Q&As:**

Q: How do you handle a merchant that generates 1% of all your traffic (a "whale" merchant)?
A: Dedicated infrastructure slab: isolated DB shard, dedicated Kafka partitions for their webhook events, dedicated webhook delivery workers, dedicated rate limit counters. SLA negotiated separately. Their traffic doesn't affect other merchants. Internally, we detect whales when a single merchant_id exceeds 10% of a shard's TPS — automated alert to provision dedicated resources.

Q: How do you scale the fraud ML model inference to 1B requests/day?
A: 1B requests/day = ~11,574 requests/sec. With each inference taking ~10ms on GPU, each GPU server handles 100 req/sec. Need ~116 GPU servers. In practice: (1) Batch micro-batching (NVIDIA Triton batches 32 requests together, each taking 15ms → 2,000 req/sec per GPU). (2) Feature vector caching (velocity features pre-computed in ClickHouse, refreshed every 30s, served from Redis). (3) Gradient-boosted tree model (XGBoost) runs on CPU for 5ms — only deep learning models need GPU. (4) Tiered: fast rule engine (CPU, < 1ms) handles obvious cases; ML model handles uncertain cases.

Q: How do you do a rolling deploy without dropping webhooks?
A: Kafka provides the buffer. During deploy: (1) Old pods continue consuming. (2) New pods start; both old and new are in the consumer group. (3) Kafka rebalances partitions. (4) Graceful shutdown: old pods process outstanding messages before terminating. (5) Kubernetes `terminationGracePeriodSeconds: 60` gives in-flight deliveries time to complete. Zero-downtime deploy with Kafka acting as the decoupling buffer.

Q: What's your strategy for database connection pooling at 100K TPS?
A: Direct Postgres connections are expensive (~5MB per connection). Strategy: (1) PgBouncer in transaction-mode pooling between services and Postgres (100K app connections → 2,000 Postgres connections across shards). (2) Connection pool size per shard: `shard_cpu_cores * 2 + disk_count` (standard formula). (3) Connection warmup: maintain a minimum pool to avoid cold-start latency. (4) Circuit breaker per shard: if connection pool exhausted, fail fast rather than queue. (5) Long-running queries (reporting) on separate read replica pools to not starve OLTP.

Q: How do you handle a Kafka broker failure during peak?
A: Kafka replication factor 3 means data is on 3 brokers. If one broker fails: (1) Partition leader election completes in < 10 seconds (controlled by `unclean.leader.election.enable=false` — we never elect an out-of-sync replica to avoid data loss). (2) Producers retry (configured with `retries=Integer.MAX_VALUE`, `acks=all`). (3) Consumer throughput temporarily reduces (fewer partitions available) but recovers after leader election. (4) DLQ events are not lost — they're in the database, not just in Kafka. (5) Monitor: `under_replicated_partitions` metric → alert if > 0 for more than 60 seconds.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Token Vault HSM failure | All new tokenizations fail; existing tokens still work for charges | HSM health endpoint fails | HSM cluster (minimum 3 units); automated failover; warm standby |
| Fraud engine timeout | Fall-through to rules-only scoring | Timeout > 80ms | Default to allow + flag for review; circuit breaker to rules engine |
| Acquirer connection failure | Charges fail to authorize | Acquirer error rate spike | Route to secondary acquirer; queue for retry |
| Kafka broker partition leader offline | Webhook delivery pauses for affected partitions | `under_replicated_partitions` metric | Auto leader election (< 30s); no data loss with replication |
| Postgres primary failure | Write operations fail during failover | Primary health check fails | Automated promotion (Patroni/pg_auto_failover); < 30s downtime |
| Webhook endpoint returning 5xx | Events not delivered to merchant | High failure rate on endpoint | Retry with backoff; DLQ after 10 attempts; alert merchant |
| DDoS on API gateway | Rate limiting breached; latency spikes | Traffic spike, error rate | Cloudflare DDoS mitigation; per-IP rate limits; shadow ban |
| Connect transfer to suspended account | Transfer fails; buyer payment succeeded | API returns 422 on transfer | Pre-check account status before charge; reverse charge if transfer fails |
| Currency conversion rate unavailable | Multi-currency charges fail | FX service unhealthy | Cache last known rates (5m stale acceptable); fail-open at 15m stale |
| Memory pressure on Redis | Eviction of hot keys | `evicted_keys` metric | `maxmemory-policy allkeys-lru` for non-critical data; `noeviction` for idempotency keys |

**Retry and Idempotency:**

API clients: exponential backoff 1s/2s/4s/8s, max 4 retries. SDK handles automatically.
Internal service calls: 3 retries with 100ms/500ms/2s backoff.
All retries must use the same `Idempotency-Key`.

**Circuit Breakers:**

Per-acquirer, per-card-network, per-fraud-service. CLOSED → OPEN after 20% error rate in 60s window. OPEN → HALF-OPEN after 30s. Metrics exported as `circuit_breaker_state{service="adyen"} = 0|1|2`.

---

## 9. Monitoring & Observability

**Key Metrics:**

| Metric | Type | SLO / Alert | Purpose |
|--------|------|------------|---------|
| `charge.success_rate` | Gauge | < 98.5% → P1 | Core health |
| `charge.latency_p99` | Histogram | > 3s → P1 | Latency SLO |
| `tokenization.latency_p99` | Histogram | > 150ms → P2 | Vault health |
| `fraud.score.latency_p99` | Histogram | > 100ms → P2 | Fraud engine |
| `webhook.delivery_success_rate` | Gauge | < 95% → P2 | Delivery health |
| `webhook.p95_delivery_latency` | Histogram | > 5s → P3 | Delivery speed |
| `kafka.consumer_lag{group=webhook-delivery}` | Gauge | > 50K → P2 | Backlog alert |
| `connect.transfer.failure_rate` | Gauge | > 1% → P2 | Marketplace health |
| `payout.failure_rate` | Gauge | > 0.1% → P1 | Settlement health |
| `vault.hsm.operations_per_sec` | Counter | < 1K/s → P1 | HSM capacity |
| `api_key.invalid_auth_rate` | Counter | > 1%/min → security alert | Auth attacks |
| `rate_limit.breached_count` | Counter | > 10K/min → investigate | DDoS / runaway client |

**Distributed Tracing:**

Trace ID in W3C `traceparent` header propagated through: API Gateway → Charge Service → Vault → Fraud Engine → Routing → Acquirer. Spans annotated with: merchant_id, payment_intent_id, fraud_score, acquirer_name, card_brand. Error spans include: failure_code, acquirer_response_code.

100% sampling for: errors, charges > $5,000, Connect platform charges. 1% random sampling otherwise. 30-day trace retention.

**Logging:**

Structured JSON via fluentd → Elasticsearch. Mandatory fields: `trace_id`, `merchant_id`, `service`, `operation`, `duration_ms`, `status_code`. PAN-scrubbing middleware before any log sink. Log levels: INFO for normal operations, WARN for retries and declines, ERROR for unexpected failures, CRITICAL for security events (HSM errors, idempotency violations). Log retention: 90 days hot, 7 years cold (Glacier/Coldline).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen Approach | Trade-off / Alternative |
|----------|----------------|------------------------|
| Tokenization architecture | Client-side iframe + HSM vault + network tokens | Server-side tokenization expands PCI scope; network token adds enrollment latency |
| Webhook delivery | At-least-once with transactional outbox | Exactly-once requires 2PC with merchant's server — impractical |
| Connect fee model | Destination charges as default | Direct charges put fraud risk on connected account; simpler but less flexible |
| Idempotency | Redis SETNX + Postgres fallback | Redis-only fast but not durable enough for financial operations |
| Shard key | merchant_id | card_fingerprint would scatter a merchant's own data across shards |
| Fraud integration | Synchronous in critical path (< 100ms) | Async post-authorization reduces latency but can't block transactions |
| Retry window for webhooks | 72 hours / 10 attempts | Longer window increases storage cost; shorter window risks missing recoveries |
| Analytics DB | ClickHouse (column store) | Postgres for analytics is 100x slower at 1B+ row aggregations |
| HSM key hierarchy | 3-tier (MK → KEK → DEK) | Flat key model simpler but doesn't allow per-batch rotation without re-encrypting all data |
| API versioning | Date-based (2024-06-20) | Semantic versioning causes "v27" problem over time; date-based is self-documenting |

---

## 11. Follow-up Interview Questions

Q1: How do you handle SCA (Strong Customer Authentication) for PSD2 in the EU?
A: PSD2 SCA requires two-factor authentication for most EU card payments > €30. Our implementation: (1) Payment Intent creation checks if SCA is required (based on card issuer country, merchant country, exemption eligibility). (2) If required, `payment_intent.status = 'requires_action'` with `next_action = {type: 'use_stripe_sdk'}` or redirect URL. (3) Stripe.js handles the 3DS2 challenge in an iframe. (4) After authentication, the CAVV is included in the authorization. (5) Exemptions: merchant-initiated transactions, subscription renewals below threshold, low-value transactions. We track exemption usage per merchant to stay within issuer limits.

Q2: How do you design API versioning to not break existing merchants?
A: Date-based versioning (e.g., `2024-06-20`). New merchants always get the latest API version. Existing merchants are pinned to their version at signup. When we make breaking changes: (1) New behavior activates in the new version date. (2) Old behavior preserved for pinned merchants. (3) Migration guide published in advance. (4) Deprecated fields marked in changelog. (5) We support all API versions for minimum 3 years. Internally, an API version compatibility layer translates between old and new data models. Most field additions are non-breaking (additive) and go out to all versions.

Q3: What's your approach to handling multi-currency presentment vs. settlement?
A: Presentment currency = what cardholder sees on statement. Settlement currency = what merchant receives. We support Dynamic Currency Conversion (DCC) where a US merchant can show EUR price to a French customer. Internally: (1) Charge is created in settlement currency (USD); (2) If merchant enables multi-currency, we convert at real-time FX rate + our fee (e.g., 1.5%); (3) Presentment amount stored on the charge; (4) We settle in merchant's bank currency. For Connect: transfers always happen in settlement currency; FX happens at payout.

Q4: How do you prevent API key leakage and handle key rotation?
A: Keys are scoped (publishable: client-side only; secret: server-side; restricted: specific permissions). Detection: (1) Alerts on API key usage from unusual IP geographies. (2) GitHub secret scanning partner — GitHub notifies us if a key is committed to a public repo; we immediately disable it and notify the merchant. (3) Keys have optional IP allowlists. Rotation: (1) Merchant can roll a key (creates new, old still works for 24h). (2) Automated rotation recommendation after 1 year. (3) Restricted keys can be scoped to specific operations (read-only for a reporting integration).

Q5: How do you handle the "thundering herd" when Kafka consumer lag spikes after an outage?
A: After a brief outage, 10M webhook events may be queued. If all workers process at full speed simultaneously, merchant endpoints get overwhelmed. Mitigation: (1) Per-endpoint rate limiting in delivery workers (max N concurrent requests per endpoint URL). (2) Gradual consumer lag reduction: process at 2× normal rate, not unlimited rate. (3) Prioritize events by age (oldest first) to reduce merchant confusion. (4) Circuit breaker per merchant endpoint: if endpoint is failing, don't hammer it while recovering, let the backoff schedule govern it.

Q6: How do you design an embeddable checkout (like Stripe Checkout)?
A: A hosted checkout page on our domain (checkout.stripe.com) that merchants redirect to or embed in an iframe. Merchant creates a Checkout Session via API specifying products, amounts, success/cancel URLs. We render the payment form, handle 3DS, validate card, create the payment. On success, redirect to merchant's success_url with session_id. The session token is time-limited (24h) and single-use. Benefit: merchant never touches card data, reducing their PCI scope to SAQ-A (simplest self-assessment).

Q7: How do you handle marketplace fund flow compliance (e.g., money transmitter licenses)?
A: Holding and moving funds on behalf of others requires money transmitter licenses (MTLs) in each US state (47 states have them) and equivalent licenses globally (UK: FCA EMI, EU: PSD2 PI license). Our Connect product uses Stripe as the money transmitter in most jurisdictions — merchants onboard as businesses, not money transmitters. For large platforms (over certain volume thresholds), they may need their own licenses. We have a compliance team that guides platforms through licensing requirements.

Q8: How do you design the payout system for instant payouts?
A: Instant payouts use Visa Direct or Mastercard Send to push funds to a debit card in < 30 minutes. Standard payouts use ACH (1-3 days). Architecture: (1) Merchant requests instant payout via API; (2) Merchant must have a Visa/MC debit card on file as payout destination; (3) We submit a Visa Direct push payment; (4) Funds available in < 30 minutes; (5) Fee: typically 1% + $0.25. Risk: instant payouts are irrevocable — if there's a pending dispute, we may be liable. We hold a reserve (rolling 7-day hold on some balance) for instant payout merchants.

Q9: How do you handle the case where a charge succeeds but the merchant's server crashes before recording the payment?
A: The webhook + polling pattern: (1) Merchant's server creates a PaymentIntent with `confirm: true`; (2) Server crashes after getting the 201 response; (3) Webhook `payment_intent.succeeded` is delivered — merchant's webhook handler (separate process) records the payment. (4) If webhook also fails (server totally down): merchant can poll `GET /v1/payment_intents` with a date filter to find all succeeded intents they haven't fulfilled. (5) Client-side: after payment confirmation, redirect to merchant's success URL with `payment_intent` query param — merchant can verify via API before showing confirmation page. Defense in depth: don't rely on a single signal.

Q10: How do you implement test mode vs. live mode in the API?
A: API key prefix differentiates: `sk_test_xxx` (test mode), `sk_live_xxx` (live mode). Test mode: all operations use a separate virtual infrastructure — test charges don't go to real card networks. Test card numbers trigger specific outcomes: `4242424242424242` = always succeed; `4000000000000002` = always decline; `4000002500003155` = require 3DS. Test data is isolated — can't accidentally mix test and live data. Webhooks in test mode go to test webhook endpoints. Separate Kafka topics, separate Postgres schemas. Merchants develop against test mode, switch to live by replacing API key.

Q11: How would you handle a zero-day vulnerability in the TLS library on the token endpoint?
A: This is a P0 security incident. Steps: (1) Immediate: take the token endpoint offline for a maintenance window (< 5 minutes). (2) Patch: deploy updated TLS library; existing tokens in vault are unaffected (they're stored encrypted, not in TLS). (3) Assess blast radius: were any PANs exposed during the vulnerability window? Requires forensic analysis. (4) Notify: if PANs may have been exposed, follow PCI DSS breach notification procedures (notify card brands within 72 hours, notify affected merchants). (5) Post-incident: review TLS library update cadence; add automated dependency vulnerability scanning (Dependabot/Snyk).

Q12: How do you enforce rate limits at 100K RPS without Redis becoming a bottleneck?
A: Distributed rate limiting with local approximation. (1) Each API gateway node maintains a local counter per merchant (in-process, Redis-backed). (2) Local counter checks: 95% of requests without network call if local estimate is clearly below limit. (3) Only when approaching limit (within 20% of threshold): synchronize with Redis using a sliding window log or token bucket. (4) Redis Cluster (sharded by merchant_id) handles 1M+ operations/sec. (5) Accept a small amount of over-limit requests in exchange for removing Redis from the critical path: `COUNT(local) + COUNT(redis_delta) < LIMIT`. Algorithm: Token Bucket with Redis INCRBY + EXPIRE.

Q13: How do you support split payments (multiple payment methods for one order)?
A: Not natively supported in a single PaymentIntent. The merchant would create multiple PaymentIntents summing to the order total. A `payment_group_id` metadata field allows correlating them. Fulfillment triggers when all PaymentIntents in the group succeed. If one fails after others succeed, the merchant must refund the successful ones. This is application-level orchestration. We provide an `order` abstraction (beta) that handles this natively: a single Order object can accept multiple payment method contributions.

Q14: How does the Stripe.js SDK minimize performance impact on merchant pages?
A: (1) Script loaded asynchronously (`async` attribute). (2) Lazy initialization: full SDK loaded only when `loadStripe()` is called. (3) iframe inserted only when `elements.create('card')` is called — not on page load. (4) SDK served from CDN (Cloudflare edge) with SRI hash. (5) Performance budget: < 50KB gzipped initial load. (6) No blocking of page render. (7) Connection pre-established (`<link rel="preconnect" href="https://api.stripe.com">`). PageSpeed impact: minimal when loaded asynchronously.

Q15: How do you handle compliance with anti-money laundering (AML) transaction monitoring?
A: (1) Transaction monitoring rules: detect structuring (breaking up large amounts to avoid reporting thresholds), rapid fund movement (in and out same day), unusual geographic patterns. (2) FinCEN requires CTR (Currency Transaction Report) for cash transactions > $10K — for card payments, our bank partner files. (3) SAR (Suspicious Activity Report) filed for suspicious activity — we have a dedicated compliance team reviewing flagged accounts. (4) OFAC screening: all merchants and payees screened against US Treasury OFAC list at onboarding and nightly re-screening. (5) Risk scoring on payout recipients: payouts to newly added bank accounts held 7 days. (6) Travel Rule compliance for cross-border transfers > $3,000.

---

## 12. References & Further Reading

- **Stripe API Reference** (canonical REST API design for payment gateway): https://stripe.com/docs/api
- **Stripe Connect Documentation** (marketplace payments architecture): https://stripe.com/docs/connect
- **PCI DSS v4.0 Requirements** (tokenization, vault, PAN handling): https://www.pcisecuritystandards.org/document_library/
- **"Designing Stripe's API"** — Stripe Engineering Blog: https://stripe.com/blog/payment-api-design
- **Visa Token Service Developer Guide**: https://developer.visa.com/capabilities/vts
- **Mastercard MDES Documentation**: https://developer.mastercard.com/mdes-digital-enablement/documentation/
- **EMVCo 3D Secure 2.x Specification**: https://www.emvco.com/emv-technologies/3d-secure/
- **Transactional Outbox Pattern** — microservices.io: https://microservices.io/patterns/data/transactional-outbox.html
- **"Building a Reliable Webhook System"** — Engineering at Meta: https://engineering.fb.com/2020/12/19/developer-tools/webhooks-at-scale/
- **Apache Kafka Documentation** (consumer groups, exactly-once semantics): https://kafka.apache.org/documentation/
- **PSD2 Regulatory Technical Standards (SCA)** — European Banking Authority: https://www.eba.europa.eu/regulation-and-policy/payment-services-and-electronic-money/regulatory-technical-standards-on-strong-customer-authentication-and-secure-communication-under-psd2
- **"The Fraud Bible"** — Stripe's approach to fraud: https://stripe.com/guides/fraud-prevention
- **Token Bucket Algorithm** — Leaky Bucket vs. Token Bucket: https://en.wikipedia.org/wiki/Token_bucket
- **FIPS 140-2 Level 3 HSM Standard** (NIST): https://csrc.nist.gov/publications/detail/fips/140/2/final
- **ClickHouse Performance for Real-Time Analytics**: https://clickhouse.com/docs/en/introduction/performance/
