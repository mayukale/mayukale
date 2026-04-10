# System Design: Payment Processing System

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Card Authorization**: Accept card payments (credit/debit) by routing through card networks (Visa, Mastercard) to issuing banks for real-time authorization.
2. **Payment Capture**: Support two-phase commit — authorize funds (hold), then capture (settle) separately or together.
3. **Refunds**: Full and partial refunds against previously captured payments.
4. **Chargebacks**: Handle issuer-initiated dispute flow with evidence submission and status tracking.
5. **Idempotency**: Every payment operation must be idempotent — retrying the same request never double-charges.
6. **Multi-method Support**: Cards (Visa/MC/Amex/Discover), ACH/bank transfers, and digital wallets (Apple Pay, Google Pay).
7. **Webhooks**: Asynchronous event notifications for payment state changes (authorized, captured, failed, refunded, disputed).
8. **PCI DSS Compliance**: No raw PANs (Primary Account Numbers) stored or logged; tokenization required.
9. **Reconciliation**: Daily settlement reconciliation against processor reports.
10. **Merchant Dashboard**: Transaction history, refund initiation, dispute management, reporting.

### Non-Functional Requirements

1. **Availability**: 99.99% uptime (< 52 minutes/year downtime). Payments are revenue-critical.
2. **Latency**: Card authorization P99 < 3 seconds (network + issuer round-trip). Internal processing P99 < 200ms.
3. **Throughput**: Support peak 50,000 transactions per second (TPS) globally (Black Friday/Cyber Monday).
4. **Durability**: Zero payment data loss. All state transitions must be durably recorded before responding to clients.
5. **Consistency**: Strong consistency for balance and authorization state. No phantom charges, no double captures.
6. **Security**: Encryption at rest (AES-256) and in transit (TLS 1.3). HSM for key management. PCI DSS Level 1.
7. **Audit Trail**: Immutable, append-only log of every state transition with timestamps, actor, and reason.
8. **Compliance**: PCI DSS Level 1, SOC 2 Type II, GDPR (right to erasure for non-financial data).

### Out of Scope

- Cryptocurrency payments
- Buy Now Pay Later (BNPL) underwriting
- Merchant onboarding / KYB (Know Your Business) verification
- Card issuance
- FX conversion (handled by acquiring bank)
- Fraud ML model training (only inference/scoring integration)

---

## 2. Users & Scale

### User Types

| User Type | Description | Key Actions |
|-----------|-------------|-------------|
| **Cardholder** | End consumer paying for goods/services | Submits payment details at checkout |
| **Merchant** | Business accepting payments | Initiates charge, views transactions, issues refunds |
| **Merchant Developer** | Integrates payment SDK/API | API calls, webhook handling |
| **Payment Ops** | Internal ops team | Reconciliation, dispute handling, fraud review |
| **Compliance Officer** | Internal compliance team | Audit log review, PCI DSS reporting |

### Traffic Estimates

**Assumptions:**
- 10 million active merchants globally
- Average merchant processes 100 transactions/day
- Peak-to-average ratio: 10x (holiday shopping, flash sales)
- Authorization: 100% of transactions
- Capture: 95% of authorizations (5% abandoned)
- Refund rate: 2% of captures
- Chargeback rate: 0.1% of captures

| Metric | Calculation | Result |
|--------|-------------|--------|
| Daily transactions | 10M merchants × 100 tx/day | 1,000,000,000 (1B/day) |
| Average TPS | 1B / 86,400s | ~11,574 TPS |
| Peak TPS | 11,574 × 10 | ~115,740 TPS |
| Authorization requests/day | 1B × 1 | 1B/day |
| Capture requests/day | 1B × 0.95 | 950M/day |
| Refund requests/day | 950M × 0.02 | 19M/day |
| Chargebacks/day | 950M × 0.001 | 950K/day |
| Webhook events/day | 1B × 3 events avg | 3B/day |

**Design target: 50,000 TPS sustained, 150,000 TPS burst** (conservative headroom above calculated peak).

### Latency Requirements

| Operation | P50 | P95 | P99 | Notes |
|-----------|-----|-----|-----|-------|
| Card authorization (end-to-end) | 800ms | 2s | 3s | Issuer network bound |
| Internal auth processing | 20ms | 80ms | 200ms | Before network call |
| Capture | 50ms | 150ms | 500ms | Async settlement |
| Refund initiation | 30ms | 100ms | 300ms | Local write, async settle |
| Idempotency key lookup | 1ms | 5ms | 10ms | Redis lookup |
| Webhook delivery (first attempt) | 100ms | 500ms | 2s | Best-effort async |

### Storage Estimates

**Assumptions:**
- Transaction record: ~2KB (including metadata, card brand, response codes)
- Audit log entry: ~500B per state transition; average 4 transitions per transaction
- Tokenized card: ~300B
- Webhook event: ~1KB
- Retention: 7 years (PCI DSS requirement)

| Data Type | Per-record Size | Daily Volume | Daily Storage | 7-Year Total |
|-----------|-----------------|--------------|---------------|--------------|
| Transactions | 2KB | 1B | 2TB | 5.1PB |
| Audit log entries | 500B | 4B (4 × 1B) | 2TB | 5.1PB |
| Tokenized cards | 300B | 10M new cards/day | 3GB | ~7.7TB |
| Webhook events | 1KB | 3B | 3TB | 7.7PB |
| Authorization holds | 1KB | 1B | 1TB | ~2.6PB (30-day hold only) |

**Total active storage**: ~8TB/day new data. Compressed + tiered to cold storage after 90 days reduces hot storage to ~720TB active.

### Bandwidth Estimates

| Traffic Type | Per-request Size | RPS | Bandwidth |
|--------------|-----------------|-----|-----------|
| Inbound API (auth requests) | 2KB | 50,000 | 100 MB/s |
| Outbound to card networks | 1KB | 50,000 | 50 MB/s |
| Network response inbound | 500B | 50,000 | 25 MB/s |
| Webhook outbound | 1KB | 100,000 (3B/day) | 100 MB/s |
| DB replication | ~3× write volume | — | ~750 MB/s |
| **Total estimated** | | | **~1.1 GB/s** |

---

## 3. High-Level Architecture

```
                         ┌────────────────────────────────────────────────────┐
                         │                   MERCHANT / CLIENT                │
                         │  Browser/App SDK  →  Tokenize PAN (client-side)   │
                         └─────────────────────────┬──────────────────────────┘
                                                   │ HTTPS (TLS 1.3)
                                                   │ (token, amount, merchant_id, idempotency_key)
                                        ┌──────────▼──────────┐
                                        │    API Gateway /    │
                                        │   Load Balancer     │
                                        │  (Rate Limit, Auth) │
                                        └──────────┬──────────┘
                                                   │
                              ┌────────────────────┼─────────────────────┐
                              │                    │                     │
                   ┌──────────▼──────┐  ┌─────────▼──────┐  ┌──────────▼──────┐
                   │  Payment API    │  │  Webhook API   │  │  Merchant API  │
                   │  Service        │  │  Service       │  │  Service       │
                   │  (auth/capture/ │  │  (event mgmt)  │  │  (dashboard)   │
                   │   refund)       │  └────────┬───────┘  └─────────────────┘
                   └──────────┬──────┘           │
                              │                  │
           ┌──────────────────┼──────────────────┼──────────────────────┐
           │                  │                  │                      │
  ┌────────▼────────┐  ┌──────▼──────┐  ┌───────▼──────┐  ┌──────────▼──────┐
  │  Idempotency    │  │  Payment    │  │  Webhook     │  │  Token Vault   │
  │  Service        │  │  State      │  │  Dispatcher  │  │  Service       │
  │  (Redis)        │  │  Machine    │  │  (Kafka +    │  │  (HSM-backed)  │
  └────────┬────────┘  │  (Postgres) │  │   Workers)   │  └──────────┬──────┘
           │           └──────┬──────┘  └──────────────┘             │
           │                  │                                       │
           │         ┌────────▼────────┐                             │
           │         │  Routing Engine │◄────────────────────────────┘
           │         │  (acquirer/     │
           │         │   network sel.) │
           │         └────────┬────────┘
           │                  │
           │    ┌─────────────┼──────────────┐
           │    │             │              │
           │  ┌─▼──────┐  ┌──▼─────┐  ┌────▼──────┐
           │  │ Visa   │  │  MC    │  │  Amex    │
           │  │ Network│  │ Network│  │  Network  │
           │  └─┬──────┘  └──┬─────┘  └────┬──────┘
           │    │             │              │
           │    └─────────────┼──────────────┘
           │                  │ Authorization Response
           │         ┌────────▼────────┐
           │         │  Issuing Bank   │
           │         │  (approve/      │
           │         │   decline)      │
           │         └─────────────────┘
           │
           │     ┌──────────────────────┐
           │     │   Audit Log Service  │
           └────►│   (Kafka → Cassandra │
                 │    append-only)      │
                 └──────────────────────┘

              Supporting Infrastructure:
              ┌─────────────┐  ┌──────────────┐  ┌─────────────────┐
              │  PostgreSQL  │  │    Redis     │  │  Kafka          │
              │  (primary    │  │  (idempotency│  │  (async events, │
              │   tx store)  │  │   + cache)   │  │   audit, DLQ)   │
              └─────────────┘  └──────────────┘  └─────────────────┘
```

**Component Roles:**

| Component | Role |
|-----------|------|
| **API Gateway / LB** | TLS termination, JWT/HMAC auth validation, rate limiting (per merchant), request routing |
| **Payment API Service** | Orchestrates the full auth/capture/refund lifecycle; calls idempotency service first |
| **Idempotency Service** | Redis-backed deduplication — prevents double-charge on network retries |
| **Payment State Machine** | Enforces valid state transitions (INITIATED → AUTHORIZED → CAPTURED → REFUNDED); writes to Postgres with optimistic locking |
| **Token Vault Service** | Exchanges raw PAN for a network token or vault token; HSM-backed; never logs PAN |
| **Routing Engine** | Selects acquirer and card network based on cost, uptime, merchant config, card BIN |
| **Card Networks (Visa/MC/Amex)** | Routes authorization request to issuing bank; returns approval/decline with auth code |
| **Issuing Bank** | Final approve/decline decision; deducts hold from cardholder limit |
| **Webhook Dispatcher** | Consumes Kafka events; delivers HTTP callbacks to merchant endpoints with retry and DLQ |
| **Audit Log Service** | Appends every state change to Cassandra immutable log; feeds compliance reporting |
| **Merchant API Service** | Dashboard, reporting, refund initiation, dispute evidence upload |

**Primary Use-Case Data Flow (Card Authorization):**

1. Merchant calls `POST /v1/payments` with `{ token, amount, currency, idempotency_key }`.
2. API Gateway validates JWT/HMAC signature, checks rate limit.
3. Payment API Service checks Idempotency Service (Redis): if key exists, return cached response.
4. Payment API Service creates a transaction record in Postgres (status=INITIATED).
5. Token Vault exchanges network token for PAN (in HSM-isolated process, PAN never leaves vault).
6. Routing Engine selects acquirer + network based on BIN, merchant config.
7. Authorization request (ISO 8583 message) sent to card network.
8. Card network routes to issuing bank; issuer approves/declines.
9. Response returns through network → Routing Engine → Payment API Service.
10. Payment API Service updates Postgres (status=AUTHORIZED, auth_code stored), writes idempotency response to Redis.
11. Audit Log event published to Kafka, consumed by Audit Log Service → Cassandra.
12. Webhook event published to Kafka → Webhook Dispatcher → merchant endpoint.
13. HTTP 200 response with authorization result returned to merchant.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- PAYMENTS (core transaction record)
-- ============================================================
CREATE TABLE payments (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL,
    idempotency_key     VARCHAR(255)    NOT NULL,
    status              payment_status  NOT NULL DEFAULT 'INITIATED',
    amount              BIGINT          NOT NULL,  -- in minor units (cents)
    currency            CHAR(3)         NOT NULL,  -- ISO 4217
    captured_amount     BIGINT          NOT NULL DEFAULT 0,
    refunded_amount     BIGINT          NOT NULL DEFAULT 0,
    payment_method_type VARCHAR(50)     NOT NULL,  -- 'card', 'ach', 'wallet'
    payment_token_id    UUID            NOT NULL,  -- FK to payment_tokens
    description         VARCHAR(1000),
    metadata            JSONB,
    acquirer_id         UUID,
    network             VARCHAR(20),    -- 'visa', 'mastercard', 'amex'
    auth_code           VARCHAR(20),
    network_transaction_id VARCHAR(100),
    decline_code        VARCHAR(20),
    decline_message     VARCHAR(255),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    captured_at         TIMESTAMPTZ,
    settled_at          TIMESTAMPTZ,
    version             BIGINT          NOT NULL DEFAULT 0,  -- optimistic lock
    CONSTRAINT uq_merchant_idempotency UNIQUE (merchant_id, idempotency_key),
    CONSTRAINT chk_amount_positive CHECK (amount > 0),
    CONSTRAINT chk_captured_le_amount CHECK (captured_amount <= amount),
    CONSTRAINT chk_refunded_le_captured CHECK (refunded_amount <= captured_amount)
);

CREATE TYPE payment_status AS ENUM (
    'INITIATED',
    'AUTHORIZED',
    'CAPTURE_PENDING',
    'CAPTURED',
    'PARTIALLY_CAPTURED',
    'REFUND_PENDING',
    'PARTIALLY_REFUNDED',
    'REFUNDED',
    'VOIDED',
    'FAILED',
    'CHARGEBACK_INITIATED',
    'CHARGEBACK_LOST',
    'CHARGEBACK_WON'
);

CREATE INDEX idx_payments_merchant_id ON payments (merchant_id);
CREATE INDEX idx_payments_created_at ON payments (created_at DESC);
CREATE INDEX idx_payments_status ON payments (status) WHERE status NOT IN ('CAPTURED', 'REFUNDED');
CREATE INDEX idx_payments_network_tx_id ON payments (network_transaction_id);

-- ============================================================
-- PAYMENT TOKENS (vault references; no raw PAN stored here)
-- ============================================================
CREATE TABLE payment_tokens (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL,
    token_type          VARCHAR(20)     NOT NULL,   -- 'network_token', 'vault_token'
    token_reference     VARCHAR(255)    NOT NULL,   -- reference to vault; PAN stored in HSM vault only
    card_fingerprint    VARCHAR(64)     NOT NULL,   -- HMAC of PAN; for dedup/fraud without storing PAN
    last4               CHAR(4)         NOT NULL,
    exp_month           SMALLINT        NOT NULL,
    exp_year            SMALLINT        NOT NULL,
    card_brand          VARCHAR(20)     NOT NULL,
    card_type           VARCHAR(20),                -- 'credit', 'debit', 'prepaid'
    issuer_country      CHAR(2),
    bin                 VARCHAR(8)      NOT NULL,   -- 6 or 8-digit BIN
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT chk_exp_month CHECK (exp_month BETWEEN 1 AND 12)
);

-- ============================================================
-- PAYMENT STATE TRANSITIONS (audit trail; append-only)
-- ============================================================
CREATE TABLE payment_transitions (
    id                  BIGSERIAL       PRIMARY KEY,
    payment_id          UUID            NOT NULL REFERENCES payments(id),
    from_status         payment_status,
    to_status           payment_status  NOT NULL,
    actor_type          VARCHAR(20)     NOT NULL,   -- 'system', 'merchant', 'network', 'issuer'
    actor_id            VARCHAR(255),
    reason_code         VARCHAR(50),
    reason_message      VARCHAR(500),
    metadata            JSONB,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);
-- Note: No UPDATE or DELETE on this table. Enforced at app layer + DB role permissions.

CREATE INDEX idx_transitions_payment_id ON payment_transitions (payment_id);
CREATE INDEX idx_transitions_created_at ON payment_transitions (created_at DESC);

-- ============================================================
-- REFUNDS
-- ============================================================
CREATE TABLE refunds (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    payment_id          UUID            NOT NULL REFERENCES payments(id),
    merchant_id         UUID            NOT NULL,
    idempotency_key     VARCHAR(255)    NOT NULL,
    amount              BIGINT          NOT NULL,
    status              refund_status   NOT NULL DEFAULT 'PENDING',
    reason              VARCHAR(50),    -- 'duplicate', 'fraudulent', 'requested_by_customer'
    network_refund_id   VARCHAR(100),
    failure_reason      VARCHAR(255),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_merchant_refund_idempotency UNIQUE (merchant_id, idempotency_key)
);

CREATE TYPE refund_status AS ENUM ('PENDING', 'PROCESSING', 'SUCCEEDED', 'FAILED', 'CANCELLED');

-- ============================================================
-- CHARGEBACKS (disputes)
-- ============================================================
CREATE TABLE chargebacks (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    payment_id          UUID            NOT NULL REFERENCES payments(id),
    merchant_id         UUID            NOT NULL,
    network_case_id     VARCHAR(100)    NOT NULL UNIQUE,
    reason_code         VARCHAR(20)     NOT NULL,   -- Visa/MC reason code (e.g., '10.4', '4853')
    reason_description  VARCHAR(255)    NOT NULL,
    amount              BIGINT          NOT NULL,
    currency            CHAR(3)         NOT NULL,
    status              chargeback_status NOT NULL DEFAULT 'OPEN',
    evidence_due_at     TIMESTAMPTZ     NOT NULL,
    resolved_at         TIMESTAMPTZ,
    resolution          VARCHAR(20),                -- 'WON', 'LOST', 'ACCEPTED'
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TYPE chargeback_status AS ENUM (
    'OPEN', 'EVIDENCE_SUBMITTED', 'UNDER_REVIEW', 'WON', 'LOST', 'ACCEPTED', 'REVERSED'
);

-- ============================================================
-- IDEMPOTENCY KEYS (also in Redis; Postgres is source of truth)
-- ============================================================
CREATE TABLE idempotency_keys (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID            NOT NULL,
    key                 VARCHAR(255)    NOT NULL,
    request_hash        VARCHAR(64)     NOT NULL,   -- SHA-256 of request body
    response_status     SMALLINT,
    response_body       JSONB,
    payment_id          UUID,
    locked_at           TIMESTAMPTZ,
    completed_at        TIMESTAMPTZ,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW() + INTERVAL '24 hours',
    CONSTRAINT uq_merchant_idempotency_key UNIQUE (merchant_id, key)
);

-- ============================================================
-- SETTLEMENT BATCHES
-- ============================================================
CREATE TABLE settlement_batches (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    acquirer_id         UUID            NOT NULL,
    batch_date          DATE            NOT NULL,
    status              VARCHAR(20)     NOT NULL DEFAULT 'PENDING',
    payment_count       INT             NOT NULL DEFAULT 0,
    gross_amount        BIGINT          NOT NULL DEFAULT 0,
    fee_amount          BIGINT          NOT NULL DEFAULT 0,
    net_amount          BIGINT          NOT NULL DEFAULT 0,
    currency            CHAR(3)         NOT NULL,
    submitted_at        TIMESTAMPTZ,
    confirmed_at        TIMESTAMPTZ,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);
```

### Database Choice

**Options Evaluated:**

| Database | Pros | Cons | Verdict |
|----------|------|------|---------|
| **PostgreSQL** | ACID transactions, mature, JSONB, row-level locking, optimistic concurrency via version column, excellent for relational integrity | Vertical scaling limits; requires careful sharding at extreme scale | **Selected for core transaction data** |
| **MySQL (InnoDB)** | ACID, wide adoption, good tooling | Weaker JSONB support; less expressive constraint system | Acceptable alternative; not selected |
| **CockroachDB** | Distributed SQL, ACID across nodes, automatic sharding | Higher latency per transaction (consensus overhead), operational complexity | Consider at >100K TPS |
| **DynamoDB** | Massive horizontal scale, low latency | Only eventual consistency by default; conditional writes complex; no joins; limited query patterns | Selected for idempotency key cache (secondary) |
| **Cassandra** | Append-only workloads, extreme write throughput, tunable consistency | No transactions; no updates; not suitable for mutable payment records | Selected for audit log only |
| **Redis** | Sub-millisecond lookups, atomic operations (SETNX for locking), TTL support | In-memory only; not durable enough as sole idempotency store | Selected as idempotency cache layer |

**Selected: PostgreSQL (core) + Redis (idempotency cache) + Cassandra (audit log)**

**Justification for PostgreSQL as core store:**
- ACID transactions are non-negotiable for payment state transitions. A payment must not appear CAPTURED unless the database has durably committed that state.
- `SELECT ... FOR UPDATE SKIP LOCKED` enables safe concurrent capture/refund without deadlocks.
- The `version` column enables optimistic locking — concurrent refund requests check-and-set atomically.
- Row-level CHECK constraints (`captured_amount <= amount`) enforce invariants at the database layer, not just application layer, which is critical for financial correctness.
- JSONB `metadata` field handles merchant-specific extensible data without schema migrations.
- PostgreSQL's `SERIALIZABLE` isolation level is available for the most sensitive operations.

**Justification for Cassandra as audit log:**
- Payment state transitions are write-once, never updated. Cassandra's LSM-tree storage is optimized for this.
- Cassandra's `TimeUUID` partition key on `payment_id` with clustering by `created_at` provides efficient range reads per payment.
- Replication factor 3 across availability zones provides durability without a single point of failure.

---

## 5. API Design

All endpoints require:
- **Authentication**: `Authorization: Bearer <merchant_jwt>` or `Stripe-Signature: t=<ts>,v1=<hmac>` for webhooks
- **Idempotency**: `Idempotency-Key: <uuid>` header required for all mutating operations
- **Rate Limiting**: 1000 requests/minute per merchant (configurable); 429 on breach
- **Versioning**: `/v1/` prefix; breaking changes get new major version

```
POST   /v1/payments
  Auth: Bearer JWT
  Rate Limit: 1000/min per merchant
  Headers: Idempotency-Key: <uuid>
  Body: {
    "amount": 2000,                    // integer, minor units (cents)
    "currency": "usd",                 // ISO 4217
    "payment_method": {
      "type": "card",
      "token": "tok_visa_4242",        // client-side tokenized PAN
      "capture_method": "automatic"    // or "manual"
    },
    "description": "Order #12345",
    "metadata": { "order_id": "12345" }
  }
  Response 201: {
    "id": "pay_abc123",
    "status": "authorized",            // or "captured" if automatic
    "amount": 2000,
    "currency": "usd",
    "auth_code": "A12345",
    "created_at": "2026-04-09T12:00:00Z"
  }
  Errors: 400 (invalid params), 402 (card declined), 409 (duplicate idempotency key with different params),
          422 (card expired), 429 (rate limit), 500 (internal)

POST   /v1/payments/:id/capture
  Auth: Bearer JWT
  Rate Limit: 1000/min
  Headers: Idempotency-Key: <uuid>
  Body: { "amount": 2000 }             // optional; defaults to authorized amount
  Response 200: { "id": "pay_abc123", "status": "captured", "captured_amount": 2000 }
  Errors: 400 (not in authorized state), 409 (already captured), 422 (amount > authorized)

POST   /v1/payments/:id/cancel
  Auth: Bearer JWT
  Headers: Idempotency-Key: <uuid>
  Response 200: { "id": "pay_abc123", "status": "voided" }
  Errors: 400 (capture already occurred)

POST   /v1/refunds
  Auth: Bearer JWT
  Headers: Idempotency-Key: <uuid>
  Body: {
    "payment_id": "pay_abc123",
    "amount": 1000,                    // partial or full; omit for full refund
    "reason": "requested_by_customer"
  }
  Response 201: { "id": "re_xyz789", "status": "pending", "amount": 1000 }

GET    /v1/refunds/:id
  Auth: Bearer JWT
  Response 200: { "id": "re_xyz789", "status": "succeeded", "amount": 1000 }

GET    /v1/payments/:id
  Auth: Bearer JWT
  Response 200: { full payment object with timeline }

GET    /v1/payments
  Auth: Bearer JWT
  Query: ?limit=25&starting_after=pay_abc&status=captured&created[gte]=2026-01-01
  Pagination: Cursor-based (starting_after payment ID); max limit=100
  Response 200: { "data": [...], "has_more": true, "next_cursor": "pay_def456" }

POST   /v1/disputes/:id/evidence
  Auth: Bearer JWT
  Body: multipart/form-data with evidence files + metadata
  Response 200: { "id": "dp_zzz", "status": "evidence_submitted" }

GET    /v1/disputes
  Auth: Bearer JWT
  Query: ?status=open&limit=25&starting_after=dp_abc
  Response 200: { "data": [...], "has_more": false }

POST   /v1/tokens                      // Client-side only; processed by JS SDK
  Auth: Publishable API key (not secret key)
  Body: { "number": "4242...", "exp_month": 12, "exp_year": 2028, "cvc": "123" }
  Response 201: { "id": "tok_xxx", "last4": "4242", "brand": "visa" }
  Note: This endpoint runs in PCI-isolated environment; never logs card data

GET    /v1/balance                     // Reserved balance, available payout balance
  Auth: Bearer JWT
  Response 200: { "available": [{ "amount": 50000, "currency": "usd" }], "pending": [...] }

POST   /v1/webhooks
  Auth: Bearer JWT
  Body: { "url": "https://merchant.com/webhooks", "events": ["payment.captured", "refund.failed"] }
  Response 201: { "id": "wh_abc", "url": "...", "secret": "whsec_..." }
```

---

## 6. Deep Dive: Core Components

### 6.1 Idempotency and Double-Spend Prevention

**Problem it solves:**

In distributed systems, network failures cause clients to retry requests. Without idempotency, a retry after a successful charge but before the HTTP response is received results in a double-charge. For payments, this is catastrophic. The idempotency system ensures that any number of retries with the same `Idempotency-Key` produces exactly one charge and returns the same response.

**Approaches:**

| Approach | Mechanism | Pros | Cons |
|----------|-----------|------|------|
| **Client-generated key + Redis SETNX** | Client provides UUID; server does `SETNX` atomically | Simple, low latency, client controls key | Redis failure = potential duplicate; requires fallback |
| **Redis + Postgres two-store** | Redis for fast path; Postgres as durable source of truth | Survives Redis restart; strong durability | More complex; two writes per request |
| **Database UNIQUE constraint only** | `UNIQUE(merchant_id, idempotency_key)` in Postgres | Simple, fully durable | Slow for high-throughput lookups; lock contention |
| **Request fingerprinting only** | Hash request body; deduplicate on hash | No client involvement | Hash collisions; different requests same hash is catastrophic |

**Selected Approach: Redis SETNX + Postgres fallback with locking protocol**

**Implementation Detail:**

```
Algorithm:
1. Compute request_hash = SHA-256(request body).
2. Attempt Redis SETNX key="idempotency:{merchant_id}:{idempotency_key}" 
   value="{status: 'in_flight', request_hash: <hash>}" with TTL=24h.
   - If SETNX returns 0 (key exists):
     a. Read existing value.
     b. If status='completed': return stored response immediately.
     c. If status='in_flight': return 409 (request in progress, retry after 1s).
     d. If request_hash != stored hash: return 422 (idempotency key reuse with different params).
3. Begin Postgres transaction:
   a. INSERT INTO idempotency_keys (merchant_id, key, request_hash, locked_at=NOW()).
      ON CONFLICT (merchant_id, key) DO NOTHING.
   b. SELECT status, response_body FROM idempotency_keys WHERE ...
      FOR UPDATE.  -- serializes concurrent requests
   c. If row found and completed: return stored response.
   d. If row found and in_flight: return 409.
4. Execute payment logic.
5. On success:
   a. UPDATE idempotency_keys SET status='completed', response_body=..., completed_at=NOW().
   b. Update Redis: SET key="{status:'completed', response:...}" TTL=24h.
6. On failure: UPDATE idempotency_keys SET status='failed'.
```

The Postgres `FOR UPDATE` lock ensures that even if Redis fails and two requests race, the database lock serializes them. The Redis layer handles the 99.9% happy path at sub-millisecond speed.

**5 Interviewer Q&As:**

Q: What happens if the server crashes after charging the card but before writing the idempotency key to Postgres?
A: This is the classic "ghost charge" problem. We solve it by writing the idempotency key to Postgres *within the same database transaction* that writes the payment record. Postgres is the source of truth. When the client retries, the server checks Postgres (not just Redis), finds the completed record, and returns it. The Postgres write of idempotency + payment is atomic.

Q: What if the client never retries? The payment succeeds but the client thinks it failed.
A: This is the "lost response" scenario. For card payments, the card is charged. If the client doesn't retry with the same idempotency key, they may create a duplicate by submitting a new request. Merchants must implement retry logic with the same idempotency key. We provide SDK helpers that enforce this. Our webhook system also delivers `payment.captured` regardless, giving the merchant a second signal.

Q: How long should idempotency keys be valid?
A: 24 hours is standard (Stripe uses 24 hours). Long enough to cover any reasonable retry window. After expiry, the same key can theoretically be reused — but our `expires_at` column allows us to enforce that old keys are expired. For critical flows, we recommend merchants generate a new key per logical operation (one key per order, not per retry attempt — retries reuse the key).

Q: How do you handle idempotency for partial captures or partial refunds?
A: Each operation (capture, refund) requires its own idempotency key. The payment ID provides the reference. A refund with `idempotency_key=refund_order_123_attempt_1` is a different operation from the original charge. This way, a partial refund can be retried safely without risk of double-refund.

Q: What's the difference between idempotency and exactly-once delivery?
A: Idempotency means the operation (charge) happens at most once for a given key — multiple calls with the same key are safe. Exactly-once delivery is a stronger guarantee in messaging systems (Kafka, etc.) meaning the event is processed precisely once. We use idempotency for the API layer and Kafka consumer-side deduplication (using the payment_id as dedup key with a processed-offsets table) for the event processing layer. Together they achieve exactly-once charge semantics end-to-end.

---

### 6.2 Two-Phase Commit: Authorization and Capture

**Problem it solves:**

Many merchants need to authorize a payment at order time (reserving funds) but only capture (settle) when the goods ship, or capture a different amount (e.g., hotel pre-auth vs. final bill). The two-phase approach also allows batch capture jobs to optimize acquirer interchange fees and enables split captures (fulfilling an order in multiple shipments).

**Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| **Synchronous two-phase** | Authorize and capture in separate synchronous API calls | Simple client model; explicit control | Capture must be called before auth expiry (typically 7 days) |
| **Automatic capture** | Authorize and capture in single request | Simpler for simple e-commerce | No flexibility; can't adjust amount; can't batch |
| **Pre-auth with incremental auth** | Authorize initial amount; send incremental auth messages to increase hold | Hotels/car rentals; real-time adjustment | Not all issuers/networks support incremental auth |
| **Batch capture** | Buffer captured transactions; send to acquirer in daily batch | Lowers interchange; fewer acquirer calls | Funds take longer to settle; complex batching logic |

**Selected: Explicit two-phase with automatic capture as default; batch settlement.**

**Implementation Detail:**

```
Authorization Phase:
1. ISO 8583 message type 0100 sent to card network.
   Fields: PAN (from vault), expiry, CVV, amount, merchant category code (MCC), 
           acquirer BIN, terminal ID.
2. Network routes to issuer; issuer checks: credit limit, fraud score, card status.
3. Issuer responds with 0110 message: approval code or decline code.
4. On approval: payment.status = AUTHORIZED, auth_code stored.
   Hold on cardholder account: valid 7 days (Visa) / 30 days (Mastercard, varies by MCC).

Capture Phase:
1. Merchant calls POST /v1/payments/:id/capture (or automatic on authorization if capture_method=automatic).
2. Payment State Machine validates: status must be AUTHORIZED or PARTIALLY_CAPTURED.
3. Amount validation: captured_amount + new_capture_amount <= authorized_amount.
4. ISO 8583 message type 0200 (financial transaction) or 0220 (advice) sent to acquirer.
   For automatic capture: 0100 with field 61 (POS data) set to indicate combined auth+capture.
5. Acquirer adds to settlement batch.
6. payment.captured_amount updated atomically:
   UPDATE payments 
   SET captured_amount = captured_amount + :new_amount,
       status = CASE WHEN captured_amount + :new_amount = amount THEN 'CAPTURED' ELSE 'PARTIALLY_CAPTURED' END,
       version = version + 1
   WHERE id = :payment_id AND version = :expected_version AND status IN ('AUTHORIZED', 'PARTIALLY_CAPTURED');
   -- version check is the optimistic lock; 0 rows updated = concurrent modification, retry

Settlement:
1. Acquirer submits batch to card network during nightly settlement window.
2. Network performs clearing: debits issuer, credits acquirer.
3. Acquirer credits merchant account (minus interchange fees) T+1 or T+2.
4. Settlement batch record created; reconciliation job compares expected vs. received.
```

**5 Interviewer Q&As:**

Q: What happens if the authorization expires before capture?
A: Authorization holds are time-limited (Visa: 7 days for most MCCs; Mastercard: 30 days). Our system runs a daily job that checks `authorizations WHERE status='AUTHORIZED' AND created_at < NOW() - INTERVAL '6 days'` and notifies merchants. If capture isn't attempted before expiry, the network auto-voids the hold. If a merchant tries to capture after expiry, the acquirer returns a "authorization expired" decline (response code 54). We return a 422 to the merchant. The correct action is to re-authorize.

Q: How do you prevent capturing more than was authorized?
A: The `CHECK (captured_amount <= amount)` constraint in Postgres prevents this at the database level. At the application level, we validate before sending to the acquirer. Additionally, the acquirer/network enforces this — they will decline a capture that exceeds the authorized amount. The multi-layer defense (app + DB constraint + network) means even a bug in one layer is caught by another.

Q: How does split capture work for marketplace orders?
A: Each capture call specifies an amount. `captured_amount` is incremented atomically using optimistic locking (`version` column). Status transitions to `PARTIALLY_CAPTURED` until `captured_amount == amount`, then `CAPTURED`. The acquirer receives separate capture messages for each partial amount. This is useful when an order ships in multiple packages.

Q: How do you handle settlement reconciliation failures?
A: We compare our internal settlement_batches records against the acquirer's settlement file (delivered via SFTP or API daily). Discrepancies fall into: (1) missing from acquirer = re-submit; (2) acquirer has extra = investigate for fraud; (3) amount mismatch = dispute with acquirer. Our reconciliation service runs daily, generates exception reports, and pages the ops team for unresolved exceptions > $100.

Q: Can you authorize in one currency and capture in another?
A: No. Currency is set at authorization and cannot change at capture. Multi-currency is handled at the acquirer/DCC (dynamic currency conversion) layer, not by us. The authorized currency is what the cardholder sees on their statement. Changing it would require a new authorization.

---

### 6.3 PCI DSS Compliance Architecture

**Problem it solves:**

Payment Card Industry Data Security Standard (PCI DSS) Level 1 requires that any system storing, processing, or transmitting cardholder data meets 12 control requirements. The primary goal of the architecture is to minimize the PCI scope — the number of systems that touch raw cardholder data (PAN, CVV, track data).

**Approaches:**

| Approach | Scope Reduction | Complexity | Risk |
|----------|----------------|------------|------|
| **Client-side tokenization only** | Server never sees PAN; only token | Low server complexity | Client-side JS must be from PCI-compliant CDN |
| **Hosted fields / iFrame** | PAN entered in iframe hosted on our PCI-isolated domain | Good UX isolation | iframe integration adds friction |
| **Point-to-point encryption (P2PE)** | PAN encrypted at reader before reaching any server | Hardware-level isolation | Only for card-present; not applicable for CNP |
| **Network tokenization (Visa Token Service / MDES)** | PAN replaced with network token by card brand | Token is specific to merchant; reduced fraud | Requires enrollment with each card network |
| **Vault tokenization** | PAN stored in isolated HSM-backed vault; all other services use vault token | Minimal PCI scope in main app | Vault becomes critical infrastructure |

**Selected: Client-side tokenization + HSM-backed vault + Network tokenization (for returning cards)**

**Implementation Detail:**

```
PCI Scope Architecture:

┌─────────────────────────────────────────────────────────────────┐
│                    PCI DSS SCOPE BOUNDARY                       │
│                                                                 │
│  ┌──────────────────┐      ┌───────────────────────────────┐   │
│  │  Token Vault     │      │  Card Network Tokenization    │   │
│  │  Service         │      │  Service (Visa/MC/Amex)       │   │
│  │                  │      │                               │   │
│  │  - HSM (FIPS     │      │  - Visa Token Service (VTS)   │   │
│  │    140-2 Level 3)│      │  - Mastercard MDES            │   │
│  │  - PAN stored    │◄────►│  - Amex Token Hub             │   │
│  │    encrypted     │      │                               │   │
│  │  - Returns       │      │  - Network tokens tied to     │   │
│  │    vault_token   │      │    merchant domain            │   │
│  └──────────────────┘      └───────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

OUT OF PCI SCOPE (only see vault_token, never PAN):
┌─────────────────────────────────────────────────────────────────┐
│  Payment API Service | Routing Engine | Merchant API Service    │
│  Database (stores vault_token reference, never PAN)             │
│  Logs, Metrics, Tracing systems (must scrub card data)          │
└─────────────────────────────────────────────────────────────────┘

Key Controls:
1. Log scrubbing: All logging frameworks include a PAN-regex filter 
   that replaces any 13-19 digit sequence matching Luhn algorithm with [REDACTED].
2. HSM key rotation: AES-256 keys rotated annually without decrypting vault 
   (re-encrypt with new key at rotation).
3. Network tokenization lifecycle: Tokens are provisioned once per card-merchant pair.
   On file card (returning customer) uses network token directly — PAN never re-transmitted.
4. CVV handling: CVV (card verification value) is NEVER stored. It is used only for 
   the authorization request and immediately discarded. Database has no CVV column.
5. TLS pinning: Client SDK pins the certificate for the tokenization endpoint.
6. Separate deployment: Token Vault runs in isolated Kubernetes namespace with 
   NetworkPolicy allowing only the Payment API Service ingress.
   No developer access to vault namespace in production.
```

**5 Interviewer Q&As:**

Q: What is PCI DSS Level 1 and why does it matter for this design?
A: PCI DSS Level 1 applies to processors handling more than 6 million card transactions per year (or any processor that has suffered a breach). It requires an annual on-site audit by a Qualified Security Assessor (QSA) and quarterly network scans. The key architectural implication is that every system that stores, processes, or transmits cardholder data is in scope and must meet all 12 PCI DSS requirements. Our goal is to maximize scope reduction by pushing PAN handling to the smallest possible surface — the Token Vault.

Q: How do you prevent PANs from leaking into logs?
A: We implement a log scrubbing interceptor at the logging framework level (Logback/Log4j2 for JVM, zap for Go) that applies a regex replacing Luhn-valid card numbers with `[PAN REDACTED]`. The regex is `\b(?:\d[ -]?){13,19}\b` combined with a Luhn check. Additionally, request/response logging at the API gateway level is configured to mask the `number` field in token creation requests. We also run a daily scan on log aggregation storage to detect any PAN leakage.

Q: What's the difference between a vault token and a network token?
A: A vault token is our internal reference (UUID) that maps to the encrypted PAN in our HSM-backed vault. It is meaningless outside our system. A network token (e.g., Visa Token Service) is issued by the card network and replaces the PAN in the authorization message — the issuer knows the real PAN but the merchant and acquirer only see the token. Network tokens have a specific domain binding (they only work for a particular merchant domain), making them useless if stolen. Network tokens also enable push provisioning updates when a card is re-issued, preventing declines.

Q: How do you handle the CVV during authorization?
A: CVV is entered by the user in the client-side tokenization form (our hosted field / iframe). It is encrypted by the JS SDK and sent directly to the Token Vault service over TLS. The Vault includes CVV in the ISO 8583 authorization request to the network (field 45 or the encrypted PIN block equivalent) and then immediately discards it — no persistence in any storage. PCI DSS explicitly prohibits storing CVV after authorization, and our data model has no column for it.

Q: How do you handle a PAN appearing in a support ticket or customer email?
A: This is a "data spillage" incident. Our support ticket system has an automated scanner that detects Luhn-valid card numbers in submitted tickets and auto-redacts them before storage, alerting the security team. If a PAN reaches an out-of-scope system, we treat it as a potential data breach, initiate the incident response plan, assess whether the PAN was stored, and notify the card brands if required. Prevention: support agents are trained to never ask for full card numbers; they can look up payments by `last4` + expiry.

---

## 7. Scaling

**Horizontal Scaling Strategy:**

- **Payment API Service**: Stateless; horizontally scale behind L7 load balancer. Each instance holds no local state. Scale based on CPU/request latency metrics. Target 16 cores, 32GB RAM per pod; 100 pods at peak.
- **Idempotency Service (Redis)**: Redis Cluster with 6 nodes (3 primary + 3 replica); consistent hashing on `{merchant_id}:{idempotency_key}`. Auto-failover via Redis Sentinel or AWS ElastiCache.
- **Postgres (core)**: Single primary with 3 read replicas initially. Reads (GET /payments) go to replicas. Writes to primary. Shard at 500M rows by `merchant_id` range or consistent hash to maintain locality.
- **Kafka**: 48-partition topic for payment events; partitioned by `payment_id`. Allows 48 parallel consumers.

**DB Sharding:**

Shard key: `merchant_id`. This is chosen because:
- Most queries are `WHERE merchant_id = ?` (merchant views their own data).
- Merchants don't frequently query across merchant boundaries (unlike user-ID sharding in a social graph).
- Hot merchant problem: top 1% of merchants generate 30% of transactions. Mitigate with dedicated shard for large merchants.

Sharding implementation: Virtual shards (1024 virtual shards) mapped to physical shard nodes. Adding physical nodes requires only moving virtual shards, not re-sharding the entire dataset.

**Replication:**

- Postgres: synchronous replication to one standby (RPO=0 for primary failure), asynchronous to additional replicas (lower latency writes, small replication lag acceptable for reads).
- Cassandra (audit log): Replication factor 3, `LOCAL_QUORUM` for writes and reads, across 3 AZs.
- Redis: Async replication to replica; AOF persistence for durability.

**Caching Strategy:**

| Cache Target | TTL | Invalidation |
|---|---|---|
| Idempotency key lookup | 24h | On completion/failure |
| Payment GET responses | 30s | On state change event |
| Merchant config (rate limits, routing rules) | 5m | On update event |
| BIN lookup (card brand/type by BIN) | 24h | BIN table changes rarely |
| Acquirer health status | 10s | Health check result |

**CDN:** Not directly applicable for payment APIs (sensitive data, no caching). Client-side JS SDK (tokenization library) is CDN-distributed with subresource integrity (SRI) hashes for PCI compliance.

**Interviewer Q&As:**

Q: How do you handle hot merchant shards (one large merchant generating disproportionate load)?
A: Detect hot shards via monitoring (shard CPU/QPS). Solutions: (1) Dedicated shard for top merchants. (2) Read replica fan-out for GET requests — large merchants' read traffic goes to 3+ replicas. (3) Application-level caching for idempotency key lookups of high-frequency merchants. (4) Rate limit enforcement at the API gateway before DB load occurs. For Stripe-scale: a single merchant like Amazon might need its own isolated database cluster.

Q: How do you scale card network connectivity?
A: Card network connections (Visa/MC) are persistent TCP connections maintained by a pool of network gateway servers. We maintain a connection pool (e.g., 20 persistent connections per network gateway instance, multiple instances per region). Connections are health-checked; failed connections are re-established. In each region, we have at least 2 independent network gateway server pairs for redundancy. At extreme scale, card networks offer direct connection or certified third-party processor relationships.

Q: How do you handle cross-region failover?
A: We operate in 3 regions (us-east, eu-west, ap-southeast). Primary traffic routes to the nearest region. Postgres replication is cross-region asynchronous (RPO ~5-30 seconds). In case of region failure, we fail over DNS to the next region. Cross-region RPO of 30 seconds means we may re-process some in-flight transactions — idempotency keys prevent double-charging. Cross-region failover is tested quarterly with chaos engineering.

Q: How do you scale the webhook delivery system under load?
A: Webhook events are published to Kafka with 48 partitions. Worker pools consume events and deliver HTTP callbacks. Workers are auto-scaled based on Kafka consumer lag. DLQ for failed deliveries. At 100K webhook events/sec, we need approximately 10,000 delivery workers (assuming 10ms per delivery). Workers batch-acknowledge Kafka offsets after confirmation of delivery or after max retries.

Q: What's the maximum throughput of a single Postgres instance, and when do you need to shard?
A: A well-tuned Postgres instance on a 32-core, 256GB RAM server (NVMe SSD) can handle ~50,000-100,000 simple transactions/sec. At 50K TPS for payments, we're at the upper limit of a single primary. We start sharding at ~20K TPS (to leave headroom). With 8 shards, each shard handles ~6K TPS — comfortable for a Postgres instance. We use PgBouncer for connection pooling (10K application connections → 1K Postgres connections per shard).

---

## 8. Reliability & Fault Tolerance

**Failure Scenarios:**

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Card network timeout | Authorization hangs; cardholder waits | Request timeout (3s) | Retry with same idempotency; circuit breaker after 3 failures/10s |
| Issuer decline storm | High decline rates; revenue impact | Decline rate metric spike | Automatic fraud rule adjustment; alert ops |
| Primary DB failure | Write operations fail | Health check fails in 10s | Promote replica (automated); RPO=0 with sync replica |
| Redis failure (idempotency) | Idempotency key lookups fail | Redis connection error | Fallback to Postgres-only idempotency (slower but correct) |
| Kafka broker failure | Webhook delivery paused; audit log delayed | Broker offline metric | Kafka automatically reassigns partitions to remaining brokers |
| Token Vault service down | No new payments (can't detokenize PAN) | Health check | Second vault instance in separate AZ; never single instance |
| Webhook endpoint 5xx | Merchant doesn't receive event | 5xx response from merchant | Retry with exponential backoff (1s, 4s, 16s, ..., max 3 days) |
| Acquirer system down | Captures fail; no settlement | Acquirer health check | Route to backup acquirer; queue for re-try when acquirer returns |
| Network partition (split brain) | Could write to two primaries | Postgres fencing token | Only primary with valid lease (via etcd/ZooKeeper) accepts writes |

**Idempotency and Retry Protocol:**

```
Client retry policy:
- Initial request fails (timeout or 5xx): retry with SAME Idempotency-Key
- Retry after: 1s, 2s, 4s (truncated exponential backoff with jitter)
- Max retries: 3 (for synchronous client); after that, check status via GET /v1/payments/:id
- 4xx errors (except 429): do NOT retry (client error, not transient)
- 429: retry after Retry-After header value

Server retry policy (for external calls):
- Card network timeout (3s): retry once immediately (network blip)
- Card network failure: exponential backoff 100ms, 500ms, 2s; then fail with 'network_error'
- Acquirer 5xx: retry 3x with backoff; then route to backup acquirer
- All retries use same auth request (same amount, same token) — network dedup via network_transaction_id
```

**Circuit Breaker:**

```
State machine per external dependency (card_network_visa, card_network_mc, acquirer_stripe, acquirer_adyen):

CLOSED (healthy):
  - Track last 60s window: error_rate, timeout_rate
  - If error_rate > 20% OR timeout_rate > 10%: → OPEN

OPEN (failing):
  - All requests fail fast with 'service_unavailable'
  - After 30s: → HALF_OPEN

HALF_OPEN (probing):
  - Allow 10% of traffic through
  - If success_rate > 80%: → CLOSED
  - If error_rate > 20%: → OPEN (reset timer)
```

---

## 9. Monitoring & Observability

**Key Metrics:**

| Metric | Type | Alert Threshold | Purpose |
|--------|------|----------------|---------|
| `payment.authorization.success_rate` | Gauge | < 95% → page | Core health |
| `payment.authorization.latency_p99` | Histogram | > 3s → page | Latency SLO |
| `payment.capture.success_rate` | Gauge | < 99% → warn | Settlement health |
| `payment.decline_rate` | Gauge | > 10% → warn | Fraud/issuer issue |
| `payment.chargeback_rate` | Gauge | > 0.5% → warn | Fraud indicator |
| `idempotency.duplicate_rate` | Counter | > 5% → investigate | Client retry health |
| `circuit_breaker.state{service}` | Gauge | OPEN → page | Dependency failure |
| `db.replication_lag_seconds` | Gauge | > 30s → warn | Data freshness |
| `kafka.consumer_lag{topic}` | Gauge | > 10K → warn | Webhook backlog |
| `vault.detokenize.latency_p99` | Histogram | > 50ms → warn | Vault performance |
| `settlement.reconciliation.mismatch_count` | Counter | > 0 → page | Financial integrity |
| `payment.double_charge.count` | Counter | > 0 → page (P0) | Idempotency failure |

**Distributed Tracing:**

Every payment request generates a trace ID propagated through all services using W3C `traceparent` header. Spans are created for:
- API Gateway (total request time)
- Idempotency check (Redis + Postgres)
- Token Vault detokenize
- Card network request (includes network latency)
- DB write (payment state transition)
- Kafka publish

Traces sampled at 1% for high-volume payments; 100% for errors and for amounts > $10,000 (large transactions warrant full observability). Stored in Jaeger/Tempo with 7-day retention.

**Logging:**

- Structured JSON logs (no free-form strings that could contain PAN).
- Log levels: WARN for declined payments (expected), ERROR for unexpected failures, CRITICAL for idempotency violations.
- Every log line includes: `trace_id`, `payment_id`, `merchant_id` (no cardholder PII), `status_code`, `duration_ms`.
- PAN redaction middleware applied before any log sink.
- Audit logs (Cassandra) are separate from application logs — immutable, 7-year retention.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen Approach | Trade-off / Alternative |
|----------|----------------|------------------------|
| Idempotency store | Redis (fast) + Postgres (durable) | Redis-only is faster but not durable enough for financial data |
| DB for payments | PostgreSQL with optimistic locking | CockroachDB offers distributed ACID but adds latency per operation |
| Authorization model | Two-phase (auth + capture) | Automatic capture simpler but loses flexibility for pre-auth use cases |
| PAN handling | Client-side tokenization + HSM vault | Server-side tokenization would expand PCI scope to all services |
| Shard key | merchant_id | user_id would scatter a single merchant's data across shards |
| Audit log DB | Cassandra (append-only) | Postgres audit triggers work but don't scale to 4B events/day |
| Circuit breaker | Per-dependency with 30s recovery | Global circuit breaker is simpler but kills all traffic on single-service failure |
| Retry strategy | Exponential backoff with jitter | Fixed retry intervals cause thundering herd on recovery |
| Settlement | Batch nightly | Real-time gross settlement possible with some networks but higher cost |
| Webhook delivery | At-least-once with dedup | Exactly-once requires distributed transaction across delivery + merchant's system |

---

## 11. Follow-up Interview Questions

Q1: How would you design the system to handle a zero-downtime database migration (adding a new column to the payments table)?
A: Use the expand-contract pattern. Phase 1 (expand): add the column as nullable with no default — this is a metadata-only operation in Postgres, instant. Phase 2: backfill the column in batches of 1000 rows using a background job, avoiding lock contention. Phase 3: deploy application code that writes to the new column. Phase 4 (contract): add NOT NULL constraint after backfill is complete. This process takes hours to days but involves zero downtime.

Q2: How do you handle currency with floating-point precision?
A: Never use floating-point for monetary values. All amounts are stored as integers in minor currency units (cents for USD, pence for GBP, yen for JPY — which has no minor unit, so 1:1). The API accepts and returns amounts as integers. This eliminates all floating-point rounding errors (e.g., 0.1 + 0.2 ≠ 0.3 in IEEE 754). The currency code determines the exponent (USD = 2, JPY = 0, KWD = 3).

Q3: How do you prevent merchants from issuing refunds greater than the original payment?
A: Three-layer defense: (1) Application check: load payment, verify `refunded_amount + new_refund_amount <= captured_amount` before proceeding. (2) Optimistic lock: UPDATE with version check; if another refund concurrent, one will fail and retry. (3) DB constraint: `CHECK (refunded_amount <= captured_amount)` prevents any direct DB operation from violating this. Additionally, the acquirer/network will reject an over-refund at settlement.

Q4: What are response codes and how do you handle soft declines vs. hard declines?
A: ISO 8583 response codes categorize decline reasons. Hard declines (05 Do Not Honor, 41 Lost Card, 43 Stolen Card) should not be retried — the card is blocked or flagged. Soft declines (51 Insufficient Funds, 65 Exceeds Limit) may succeed on retry or with a different amount — the issuer may approve at a lower amount. We expose `decline_code` and a `retryable` boolean to merchants. For soft declines, we suggest retry after 24 hours. For hard declines, we suggest a different payment method.

Q5: How do you handle 3D Secure (3DS) authentication?
A: 3DS adds an issuer authentication step between authorization and the cardholder. Flow: (1) Merchant initiates auth; (2) We call the 3DS server (Directory Server) to check if the card is enrolled; (3) If enrolled, redirect cardholder to ACS (Access Control Server) for authentication (OTP, biometric); (4) ACS returns authentication value (CAVV) and ECI indicator; (5) We include CAVV and ECI in the authorization request to the network; (6) Liability for fraud shifts from merchant to issuer. We support 3DS 2.x (friction-less flow) where the ACS can authenticate based on device fingerprint without cardholder interaction.

Q6: How do you design the settlement reconciliation process?
A: Daily batch job: (1) Pull settlement file from acquirer (SFTP or API), (2) Parse ISO 8583 clearing records or CSV, (3) Match each record to our `payments` table by `network_transaction_id`, (4) Verify amount matches (within rounding tolerance), (5) Update `payments.settled_at` and `settlement_batches.confirmed_at`, (6) Flag exceptions (missing records, amount mismatches), (7) Alert ops team for exceptions. We maintain a `reconciliation_exceptions` table. Unresolved exceptions > $1 trigger automated escalation.

Q7: How would you add support for ACH (bank transfer) payments?
A: ACH is asynchronous by nature — funds take 1-5 business days to settle. Architecture changes: (1) New payment_method_type='ach'; (2) Collect bank account details (routing + account number); (3) Tokenize via a micro-deposit or Plaid verification flow; (4) Submit NACHA-formatted file to ODFI (Originating Depository Financial Institution) daily; (5) Poll for returns (R-codes) for up to 60 days (ACH dispute window); (6) Payment status: PENDING → PROCESSING → SETTLED or RETURNED. ACH adds significant complexity around bank account verification and return handling.

Q8: How do you ensure atomicity between charging the card and fulfilling the order in the merchant's system?
A: This is the distributed transaction problem. We can't guarantee both happen atomically. Best practice: (1) Charge first, then fulfill (if fulfillment fails, refund). (2) Use webhooks to trigger fulfillment — `payment.captured` event triggers the merchant's order service. (3) Merchant implements idempotent order fulfillment (processing the webhook twice doesn't ship the order twice). (4) Our webhook delivery guarantees at-least-once. The merchant's webhook handler deduplicates by `payment_id`. This is the saga pattern.

Q9: How do you handle card updates when a card is re-issued?
A: Visa and Mastercard offer Account Updater service — we query this service periodically with our stored cards, and the networks return updated PANs/expiry dates. With network tokenization (VTS/MDES), tokens are automatically updated when the underlying card changes — the merchant's token remains valid with no action required. This dramatically reduces "card expired" declines for subscription merchants.

Q10: How do you handle chargebacks at scale?
A: Chargebacks arrive as notifications from the card network via our acquirer. We: (1) Create a chargeback record linked to the original payment; (2) Debit the merchant's account for the chargeback amount + fee; (3) Update payment.status to CHARGEBACK_INITIATED; (4) Notify merchant via webhook and email; (5) Provide 7-10 days to submit evidence; (6) Evidence (receipts, shipping confirmation, T&C acceptance logs) uploaded via API; (7) Submit to acquirer who forwards to card network; (8) Network rules on outcome (7-45 days); (9) Update chargeback status to WON (funds returned to merchant) or LOST (funds stay with cardholder).

Q11: What is interchange and how does it affect system design?
A: Interchange is the fee paid from merchant's bank (acquirer) to cardholder's bank (issuer) for facilitating the transaction. It ranges from 0.05% (debit) to 3%+ (premium rewards credit). For system design: (1) We capture the card type ('debit', 'credit', 'prepaid', 'commercial') from the BIN database to estimate interchange cost; (2) Some merchants want least-cost routing — route to the acquirer offering the best interchange rate for the card type; (3) Level 2/3 data (purchase order number, line-item detail) passed in authorization can qualify for lower interchange rates for corporate cards.

Q12: How would you design fraud detection integration?
A: Fraud scoring is inserted into the authorization flow between idempotency check and network call. The fraud service receives: device fingerprint, IP address, velocity data (how many transactions from this card in last 1h), merchant category, amount, card history. It returns: score (0-100), recommended action (allow/review/block), and reason codes. The routing engine acts on this: score < 30 = allow; 30-70 = 3DS challenge; > 70 = decline. The fraud service must respond in < 100ms (it's in the critical path). We store fraud scores on the transaction for model feedback loops.

Q13: How do you design the system to be GDPR-compliant?
A: GDPR allows "right to erasure" but conflicts with financial record retention requirements (typically 7 years by regulation). Resolution: we pseudonymize non-financial personal data. Cardholder name and billing address are stored separately from the payment record, linked by a pseudonym ID. On GDPR deletion request: delete the mapping table entry (pseudonym → real data), leaving the payment record intact with only the pseudonym (not the real name). Financial records (amounts, dates, merchant) are retained for 7 years as legally required.

Q14: What's the difference between a payment processor, acquiring bank, and PSP?
A: **Acquiring bank (acquirer)**: licensed member of the card networks (Visa/MC); holds the merchant's account; receives settlement funds from the network and pays the merchant. **Payment processor**: the technical intermediary that formats and routes ISO 8583 messages between the acquirer and the card networks; may be the same entity as the acquirer. **PSP (Payment Service Provider)**: like Stripe or Adyen; sits above the acquiring bank; provides the developer-facing API, tokenization, fraud tools, and risk management; often has relationships with multiple acquirers for redundancy and may act as the merchant of record. In our design, we're designing the PSP layer.

Q15: How do you handle high-volume merchants (e.g., an Amazon-scale merchant)?
A: Dedicated infrastructure: (1) Separate Postgres shard or even dedicated cluster for the merchant; (2) Dedicated connection pool (PgBouncer) instances; (3) Merchant-specific Kafka topics for their event stream; (4) Dedicated webhook delivery workers to avoid their webhook traffic affecting others; (5) Dedicated acquirer connections for higher throughput and better rates; (6) Higher rate limits (negotiated SLA). The system's virtual sharding model makes this migration transparent — move all of the merchant's virtual shards to dedicated physical infrastructure without changing the application routing logic.

---

## 12. References & Further Reading

- **PCI DSS v4.0 Standard**: https://www.pcisecuritystandards.org/document_library/
- **Visa Developer Documentation (ISO 8583)**: https://developer.visa.com/capabilities/visa_direct
- **Stripe API Documentation (reference implementation)**: https://stripe.com/docs/api
- **Designing Data-Intensive Applications** — Martin Kleppmann (O'Reilly, 2017) — Chapters 7 (Transactions) and 9 (Consistency and Consensus)
- **"Exactly-once semantics in Apache Kafka"** — Neha Narkhede, Confluent Blog (2017): https://www.confluent.io/blog/exactly-once-semantics-are-possible-heres-how-apache-kafka-does-it/
- **Mastercard MDES (Mastercard Digital Enablement Service)**: https://developer.mastercard.com/mdes-digital-enablement/documentation/
- **Visa Token Service**: https://developer.visa.com/capabilities/vts
- **"Idempotency Keys"** — Stripe Engineering Blog: https://stripe.com/blog/idempotency
- **NACHA Operating Rules** (ACH): https://www.nacha.org/rules
- **ISO 4217 Currency Codes**: https://www.iso.org/iso-4217-currency-codes.html
- **3D Secure 2.x Protocol**: https://www.emvco.com/emv-technologies/3d-secure/
- **Google SRE Book — Chapter 26: Data Integrity** (for reconciliation patterns): https://sre.google/sre-book/data-integrity/
- **"Building Microservices"** — Sam Newman (O'Reilly, 2021) — Chapter on Sagas for distributed transactions
- **AWS re:Invent 2018: "How to build a financial services platform"** (architectural patterns): https://www.youtube.com/watch?v=J-9afazR2o0
