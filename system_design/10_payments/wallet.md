# System Design: Digital Wallet System

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Account Creation & KYC**: Users create wallet accounts; KYC (Know Your Customer) identity verification required before funding or withdrawals above threshold limits.
2. **Balance Management**: Real-time balance display; support for multiple currency balances in the same wallet.
3. **Top-Up (Deposit)**: Fund wallet from linked bank account (ACH/SEPA), debit card, or credit card.
4. **Withdrawal**: Transfer wallet balance to linked bank account.
5. **P2P Transfers**: Send money to another wallet user by phone number, email, or username — instant and free within the platform.
6. **Transaction History**: Full paginated history of all credits, debits, holds, and reversals with timestamps and counterparty info.
7. **Currency Conversion**: Convert between supported currencies at real-time exchange rates with transparent fee disclosure.
8. **Spend Controls**: Spending limits (daily, weekly, monthly) configurable by the user or enforced by compliance.
9. **Refunds & Disputes**: Process refunds for merchant payments; dispute resolution with evidence flow.
10. **AML/Compliance Monitoring**: Real-time transaction monitoring for suspicious activity; automatic SAR triggers; OFAC screening.

### Non-Functional Requirements

1. **Availability**: 99.99% uptime. Balance checks and P2P transfers are time-sensitive.
2. **Consistency**: Strong consistency for balance operations — double-spend is catastrophic. Balances must be accurate to the cent.
3. **Latency**: P2P transfer P99 < 500ms (user expects instant confirmation). Balance read P99 < 100ms. Top-up confirmation P99 < 2s.
4. **Throughput**: 100,000 concurrent active users; 50,000 P2P transfers/sec at peak.
5. **Durability**: Zero money loss. Every balance change must be durably committed before response.
6. **Idempotency**: Network retries must never create duplicate transfers.
7. **Security**: Two-factor authentication for withdrawals. Session tokens invalidated on password change. Fraud detection on every outbound transfer.
8. **Compliance**: PCI DSS (for card top-ups), FinCEN BSA/AML (US), GDPR/CCPA (privacy), FATF (international transfers).
9. **Auditability**: Immutable audit log of every balance change; retained 7 years minimum.

### Out of Scope

- Card issuance (debit cards linked to wallet)
- Investment / interest-bearing accounts
- Credit / overdraft
- Cryptocurrency
- International wire transfers (SWIFT)
- Merchant acquiring

---

## 2. Users & Scale

### User Types

| User Type | Description | Primary Operations |
|-----------|-------------|-------------------|
| **Consumer** | Individual using the wallet for personal payments | P2P transfer, top-up, withdrawal, balance check |
| **Small Business Owner** | Uses wallet to receive payments, pay suppliers | Receive transfers, withdraw to business bank account |
| **KYC Officer (Internal)** | Reviews manual KYC submissions | Document verification, account approval/rejection |
| **Compliance Analyst (Internal)** | Reviews flagged transactions | Transaction investigation, SAR filing |
| **Finance Ops (Internal)** | Reconciles float accounts, manages liquidity | Settlement reconciliation, bank account management |

### Traffic Estimates

**Assumptions:**
- 100 million registered users; 20 million Daily Active Users (DAU)
- Average 5 transactions/DAU/day (P2P sends, top-ups, merchant payments)
- Peak-to-average: 5x (weekends, holidays, events)
- Read:Write ratio for balance checks = 10:1 (users check balance frequently)
- P2P transfers: 40% of all transactions
- Top-up transactions: 20% of all transactions
- Withdrawal transactions: 10% of all transactions
- Merchant payments: 30% of all transactions

| Metric | Calculation | Result |
|--------|-------------|--------|
| Daily transactions | 20M DAU × 5 | 100M/day |
| Average TPS | 100M / 86,400 | ~1,157 TPS |
| Peak TPS | 1,157 × 5 | ~5,787 TPS |
| P2P transfers/day | 100M × 40% | 40M/day |
| P2P transfers peak TPS | 40M / 86,400 × 5 | ~2,315 TPS |
| Balance read requests/day | 100M × 10 (read:write) | 1B/day |
| Balance read peak RPS | 1B / 86,400 × 5 | ~57,870 RPS |
| Top-up events/day | 100M × 20% | 20M/day |
| Withdrawal events/day | 100M × 10% | 10M/day |
| KYC verifications/day | 100K new users × 1 | 100K/day (new user flow) |

**Design target: 10,000 TPS transaction sustained; 100,000 RPS balance reads.**

### Latency Requirements

| Operation | P50 | P95 | P99 | Notes |
|-----------|-----|-----|-----|-------|
| Balance read | 5ms | 30ms | 100ms | Cache hit |
| P2P transfer (confirmation) | 100ms | 300ms | 500ms | Must be instant feel |
| Top-up (card) | 500ms | 1.5s | 2s | Card auth latency |
| Top-up (bank/ACH) | 1-3 business days | — | — | Async; notification on settlement |
| Withdrawal (bank) | 1-2 business days | — | — | Async; ACH batch |
| Withdrawal (instant) | 500ms | 2s | 5s | Visa Direct / debit card |
| Currency conversion | 50ms | 150ms | 300ms | FX rate lookup + DB write |
| Transaction history | 20ms | 100ms | 300ms | Paginated, indexed |
| KYC verification (automated) | 5s | 30s | 5m | Document scan + ID check |
| Fraud score (per transfer) | 10ms | 30ms | 60ms | In critical path |

### Storage Estimates

**Assumptions:**
- User record: 2KB (including KYC status, limits, settings)
- Balance record: 500B per currency per user
- Transaction record: 1.5KB (amount, counterparty, metadata, status)
- Ledger entry: 500B (double-entry: 2 entries per transaction)
- AML event: 1KB
- Audit log entry: 300B
- KYC document: 2MB average (photo ID, selfie) stored in object storage

| Data Type | Per-record | Daily Volume | Daily Storage | 7-Year Total |
|-----------|-----------|--------------|---------------|--------------|
| User accounts | 2KB | 100K new/day | 200MB/day | ~511GB |
| Balance records | 500B | ~100K updated/day (new users) | 50MB/day | New; full scan: 100M × 500B = 50GB |
| Transaction records | 1.5KB | 100M/day | 150GB | ~383TB |
| Ledger entries | 500B | 200M/day (2 × 100M) | 100GB | ~256TB |
| AML events | 1KB | 100M (1 per tx) | 100GB | ~256TB |
| Audit log | 300B | 300M (3 entries avg per tx) | 90GB | ~230TB |
| KYC documents | 2MB | 100K submissions | 200GB | ~512TB |

**Total transaction hot storage (90 days)**: ~50TB. Cold: ~1PB at 7 years.

### Bandwidth Estimates

| Traffic Type | Per-request | RPS | Bandwidth |
|---|---|---|---|
| Balance read API | 500B response | 57,870 | ~29 MB/s |
| P2P transfer API | 2KB req/resp | 2,315 | ~4.6 MB/s |
| Transaction history | 10KB per page | 1,000 | ~10 MB/s |
| Notification push (FCM/APNS) | 500B | 5,000 (concurrent transfers × 2) | ~2.5 MB/s |
| DB replication (write) | 3× writes | — | ~450 MB/s |
| AML event streaming (Kafka) | 1KB | 5,787 | ~5.8 MB/s |
| **Total** | | | **~500 MB/s** |

---

## 3. High-Level Architecture

```
        ┌──────────────────────────────────────────────────────────────────┐
        │                    CLIENTS                                       │
        │   iOS App    Android App    Web App (React)    Partner API       │
        └────────────────────────────┬─────────────────────────────────────┘
                                     │ HTTPS / TLS 1.3
        ┌────────────────────────────▼─────────────────────────────────────┐
        │              API GATEWAY / EDGE LAYER                            │
        │   Authentication │ Rate Limiting │ DDoS Protection │ Routing     │
        │   JWT validation │ Device fingerprint │ Geo-routing              │
        └───────┬──────────────────┬────────────────────┬──────────────────┘
                │                  │                    │
     ┌──────────▼──────┐  ┌────────▼────────┐  ┌───────▼──────────────────┐
     │  Auth Service   │  │  Wallet Service  │  │  KYC / Onboarding       │
     │  (login, 2FA,   │  │  (core balance   │  │  Service                │
     │   session mgmt) │  │   management)    │  │  (identity verification) │
     └─────────────────┘  └────────┬────────┘  └──────────────────────────┘
                                   │
          ┌────────────────────────┼─────────────────────────────────┐
          │                        │                                 │
 ┌────────▼────────┐    ┌──────────▼────────┐           ┌───────────▼──────┐
 │  Transfer       │    │  Balance           │           │  Funding         │
 │  Service        │    │  Service           │           │  Service         │
 │  (P2P, merchant │    │  (read/cache       │           │  (top-up,        │
 │   payments)     │    │   balance)         │           │   withdrawal)    │
 └────────┬────────┘    └──────────┬────────┘           └───────────┬──────┘
          │                        │                                 │
          │             ┌──────────▼──────────────────────────────┐  │
          │             │          LEDGER SERVICE                  │  │
          │             │  (double-entry; all balance mutations)   │  │
          │             │  Postgres (sharded by account_id)        │  │
          └────────────►│  Optimistic locking │ Atomic updates     │◄─┘
                        └──────────────────────────────────────────┘
                                   │
          ┌────────────────────────▼─────────────────────────────────────┐
          │                  EVENT BUS (Apache Kafka)                     │
          │  transfer.initiated, transfer.completed, balance.updated,     │
          │  topup.succeeded, withdrawal.initiated, aml.alert, audit      │
          └───────┬───────────────────┬────────────────────────────────┬──┘
                  │                   │                                │
     ┌────────────▼──────┐  ┌─────────▼──────────────┐  ┌────────────▼────────┐
     │  Notification     │  │  AML / Compliance       │  │  Audit Log Service  │
     │  Service          │  │  Service                │  │  (Cassandra         │
     │  (push, SMS,      │  │  (real-time rule        │  │   append-only)      │
     │   email)          │  │   evaluation + ML)      │  └─────────────────────┘
     └───────────────────┘  └─────────────────────────┘
                                        │
                             ┌──────────▼──────────────────────┐
                             │    EXTERNAL SYSTEMS              │
                             │  ACH Network (bank transfers)    │
                             │  Visa Direct (instant payouts)   │
                             │  FX Providers (Bloomberg/ECB)    │
                             │  ID Verification (Jumio/Onfido)  │
                             │  OFAC Screening (World-Check)    │
                             └─────────────────────────────────┘

     DATA STORES:
     ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐  ┌────────────────┐
     │ PostgreSQL   │  │ Redis        │  │ Cassandra       │  │ S3/GCS         │
     │ (accounts,   │  │ (balance     │  │ (audit log,     │  │ (KYC documents,│
     │  ledger,     │  │  cache,      │  │  tx history     │  │  reports,      │
     │  transfers)  │  │  sessions,   │  │  read model)    │  │  evidence)     │
     │              │  │  idempotency)│  │                 │  │                │
     └──────────────┘  └──────────────┘  └─────────────────┘  └────────────────┘
```

**Component Roles:**

| Component | Role |
|-----------|------|
| **API Gateway** | JWT auth, rate limiting (per user), device fingerprinting, request routing |
| **Auth Service** | Login, 2FA (TOTP/SMS), session token issuance (JWT + refresh tokens), device management |
| **Wallet Service** | Orchestrates transfers, top-ups, withdrawals; calls fraud check before committing |
| **Transfer Service** | P2P transfer logic; resolves recipient by phone/email/username; calls Ledger Service |
| **Balance Service** | Reads balance from Redis cache; falls through to Ledger for cache miss; serves the 100K RPS read path |
| **Funding Service** | Initiates card charges (via payment processor) and ACH debits for top-up; ACH credits for withdrawal |
| **Ledger Service** | Double-entry bookkeeping; atomic debit+credit; single source of truth for balances |
| **KYC Service** | Document upload, automated OCR/liveness check (Jumio/Onfido), manual review queue |
| **AML Service** | Real-time rule evaluation + ML scoring; generates AML alerts; interfaces with compliance team |
| **Notification Service** | Push (FCM/APNS), SMS, email notifications for all events |
| **Audit Log Service** | Appends every balance change to Cassandra with full context; 7-year retention |

**Primary Use-Case Data Flow (P2P Transfer):**

1. Sender: `POST /v1/transfers { to: "+1-555-1234", amount: 5000, currency: "usd", note: "Dinner" }`.
2. API Gateway: validate JWT, check rate limit (max 10 transfers/min).
3. Wallet Service: check idempotency key.
4. Wallet Service: resolve recipient by phone number → `user_id`.
5. Wallet Service: call Fraud Service — score this transfer (sender history, velocity, recipient risk). If score > threshold → block.
6. Wallet Service: check spend limits (daily limit not exceeded).
7. Ledger Service: atomic double-entry:
   ```sql
   BEGIN;
     INSERT INTO ledger_entries (account_id, amount, direction, type, ...) -- debit sender
     INSERT INTO ledger_entries (account_id, amount, direction, type, ...) -- credit recipient
     UPDATE account_balances SET balance = balance - :amount WHERE account_id = :sender AND balance >= :amount;
     UPDATE account_balances SET balance = balance + :amount WHERE account_id = :recipient;
   COMMIT;
   ```
8. Publish `transfer.completed` event to Kafka.
9. Kafka consumers: Notification Service (push to both parties), AML Service (post-hoc monitoring), Audit Log.
10. Return HTTP 201 to sender. Push notification to recipient.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- ACCOUNTS (wallet user accounts)
-- ============================================================
CREATE TABLE accounts (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID            NOT NULL UNIQUE,   -- FK to users service
    account_number      VARCHAR(20)     NOT NULL UNIQUE,   -- Display account number
    status              account_status  NOT NULL DEFAULT 'pending_kyc',
    kyc_level           SMALLINT        NOT NULL DEFAULT 0,  -- 0: unverified, 1: basic, 2: full
    kyc_verified_at     TIMESTAMPTZ,
    daily_send_limit    BIGINT          NOT NULL DEFAULT 100000,  -- cents; $1000 default
    weekly_send_limit   BIGINT          NOT NULL DEFAULT 500000,  -- $5000 default
    monthly_send_limit  BIGINT          NOT NULL DEFAULT 2000000, -- $20000 default
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TYPE account_status AS ENUM (
    'pending_kyc', 'active', 'suspended', 'frozen', 'closed'
);

-- ============================================================
-- ACCOUNT BALANCES (one row per account per currency)
-- ============================================================
CREATE TABLE account_balances (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    currency            CHAR(3)         NOT NULL,
    balance             BIGINT          NOT NULL DEFAULT 0,  -- minor units
    reserved_balance    BIGINT          NOT NULL DEFAULT 0,  -- holds (pending transactions)
    available_balance   BIGINT          GENERATED ALWAYS AS (balance - reserved_balance) STORED,
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    version             BIGINT          NOT NULL DEFAULT 0,  -- optimistic lock
    CONSTRAINT uq_account_currency UNIQUE (account_id, currency),
    CONSTRAINT chk_balance_non_negative CHECK (balance >= 0),
    CONSTRAINT chk_reserved_le_balance CHECK (reserved_balance <= balance),
    CONSTRAINT chk_available_non_negative CHECK (balance - reserved_balance >= 0)
);

CREATE INDEX idx_account_balances_account_id ON account_balances (account_id);

-- ============================================================
-- LEDGER ENTRIES (double-entry; immutable; append-only)
-- ============================================================
CREATE TABLE ledger_entries (
    id                  BIGSERIAL       PRIMARY KEY,
    transaction_id      UUID            NOT NULL,    -- Groups debit + credit entries
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    currency            CHAR(3)         NOT NULL,
    amount              BIGINT          NOT NULL,    -- Always positive; direction indicates sign
    direction           CHAR(1)         NOT NULL,    -- 'D' = debit (reduce), 'C' = credit (add)
    entry_type          VARCHAR(30)     NOT NULL,    -- 'p2p_send', 'p2p_receive', 'topup', 'withdrawal', 'fee', 'reversal', 'fx_out', 'fx_in'
    balance_after       BIGINT          NOT NULL,    -- Running balance after this entry
    counterpart_id      UUID,                        -- Other account in this transaction
    reference           VARCHAR(255),                -- External reference (ACH trace #, etc.)
    description         VARCHAR(500),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    -- Immutability enforced: no UPDATE or DELETE permissions on this table
    CONSTRAINT chk_amount_positive CHECK (amount > 0)
);
-- No UPDATE, no DELETE. Role-based: app user has INSERT + SELECT only.
CREATE INDEX idx_ledger_account_created ON ledger_entries (account_id, created_at DESC);
CREATE INDEX idx_ledger_transaction_id ON ledger_entries (transaction_id);

-- ============================================================
-- TRANSFERS (P2P and merchant payments)
-- ============================================================
CREATE TABLE transfers (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    idempotency_key     VARCHAR(255),
    sender_account_id   UUID            NOT NULL REFERENCES accounts(id),
    recipient_account_id UUID           NOT NULL REFERENCES accounts(id),
    amount              BIGINT          NOT NULL,
    currency            CHAR(3)         NOT NULL,
    status              transfer_status NOT NULL DEFAULT 'pending',
    transfer_type       VARCHAR(20)     NOT NULL DEFAULT 'p2p',  -- 'p2p', 'merchant_payment', 'fee'
    note                VARCHAR(200),
    fraud_score         SMALLINT,
    fraud_recommendation VARCHAR(20),
    sender_ledger_entry_id BIGINT       REFERENCES ledger_entries(id),
    recipient_ledger_entry_id BIGINT    REFERENCES ledger_entries(id),
    reversed_by         UUID            REFERENCES transfers(id),
    reversal_of         UUID            REFERENCES transfers(id),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_idempotency UNIQUE (sender_account_id, idempotency_key),
    CONSTRAINT chk_amount_positive CHECK (amount > 0),
    CONSTRAINT chk_not_self_transfer CHECK (sender_account_id != recipient_account_id)
);

CREATE TYPE transfer_status AS ENUM ('pending', 'processing', 'completed', 'failed', 'reversed');

CREATE INDEX idx_transfers_sender ON transfers (sender_account_id, created_at DESC);
CREATE INDEX idx_transfers_recipient ON transfers (recipient_account_id, created_at DESC);

-- ============================================================
-- FUNDING TRANSACTIONS (top-up and withdrawal)
-- ============================================================
CREATE TABLE funding_transactions (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    type                VARCHAR(20)     NOT NULL,    -- 'topup', 'withdrawal'
    method              VARCHAR(20)     NOT NULL,    -- 'card', 'ach', 'wire', 'instant_payout'
    status              funding_status  NOT NULL DEFAULT 'pending',
    amount              BIGINT          NOT NULL,
    currency            CHAR(3)         NOT NULL,
    fee_amount          BIGINT          NOT NULL DEFAULT 0,
    net_amount          BIGINT          GENERATED ALWAYS AS (amount - fee_amount) STORED,
    external_reference  VARCHAR(100),   -- ACH trace #, card charge ID, wire ref
    bank_account_id     UUID,           -- FK to linked bank accounts
    payment_method_id   UUID,           -- FK to tokenized cards
    failure_reason      VARCHAR(255),
    idempotency_key     VARCHAR(255),
    initiated_at        TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    settled_at          TIMESTAMPTZ,
    CONSTRAINT uq_funding_idempotency UNIQUE (account_id, idempotency_key)
);

CREATE TYPE funding_status AS ENUM ('pending', 'processing', 'completed', 'failed', 'returned', 'reversed');

-- ============================================================
-- LINKED BANK ACCOUNTS (for top-up and withdrawal)
-- ============================================================
CREATE TABLE linked_bank_accounts (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    institution_name    VARCHAR(100)    NOT NULL,
    account_last4       CHAR(4)         NOT NULL,
    routing_number_last4 CHAR(4)        NOT NULL,
    account_type        VARCHAR(20)     NOT NULL,    -- 'checking', 'savings'
    account_holder_name VARCHAR(100),
    verification_status VARCHAR(20)     NOT NULL DEFAULT 'unverified',  -- 'unverified', 'verified', 'failed'
    verification_method VARCHAR(20),    -- 'micro_deposit', 'plaid', 'instant_verification'
    plaid_token         VARCHAR(255),   -- Encrypted Plaid access token
    ach_token           VARCHAR(255),   -- Encrypted tokenized routing+account for ACH
    is_default          BOOLEAN         NOT NULL DEFAULT FALSE,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- FX CONVERSIONS
-- ============================================================
CREATE TABLE fx_conversions (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    from_currency       CHAR(3)         NOT NULL,
    to_currency         CHAR(3)         NOT NULL,
    from_amount         BIGINT          NOT NULL,
    to_amount           BIGINT          NOT NULL,
    exchange_rate       NUMERIC(18, 8)  NOT NULL,
    fee_amount          BIGINT          NOT NULL DEFAULT 0,
    fee_currency        CHAR(3),
    rate_source         VARCHAR(50),    -- 'bloomberg', 'ecb', 'internal'
    rate_locked_at      TIMESTAMPTZ,
    rate_expires_at     TIMESTAMPTZ,
    status              VARCHAR(20)     NOT NULL DEFAULT 'completed',
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- KYC VERIFICATIONS
-- ============================================================
CREATE TABLE kyc_verifications (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    provider            VARCHAR(50)     NOT NULL,   -- 'jumio', 'onfido', 'manual'
    provider_session_id VARCHAR(255),
    verification_type   VARCHAR(30)     NOT NULL,   -- 'identity_document', 'liveness', 'proof_of_address'
    status              VARCHAR(20)     NOT NULL DEFAULT 'pending',  -- 'pending', 'pending_review', 'approved', 'rejected'
    rejection_reason    VARCHAR(255),
    document_type       VARCHAR(50),    -- 'passport', 'drivers_license', 'national_id'
    document_country    CHAR(2),
    document_storage_url VARCHAR(500),  -- S3 URL (access-restricted)
    risk_score          SMALLINT,
    reviewed_by         UUID,           -- KYC officer user_id if manual
    reviewed_at         TIMESTAMPTZ,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- AML ALERTS
-- ============================================================
CREATE TABLE aml_alerts (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL,
    trigger_type        VARCHAR(50)     NOT NULL,   -- 'velocity', 'large_transaction', 'pattern', 'sanctions_match'
    severity            VARCHAR(20)     NOT NULL,   -- 'low', 'medium', 'high', 'critical'
    related_transfer_id UUID,
    description         TEXT,
    status              VARCHAR(20)     NOT NULL DEFAULT 'open',  -- 'open', 'reviewed', 'escalated', 'closed', 'sar_filed'
    reviewed_by         UUID,
    review_notes        TEXT,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);
```

### Database Choice

**Options Evaluated:**

| Database | Fit for Wallet | Pros | Cons | Decision |
|----------|---------------|------|------|----------|
| **PostgreSQL** | Excellent for ACID balance mutations | Strong consistency, check constraints, optimistic locking, row-level locks | Vertical scaling; connection limits | **Selected: core balance/ledger** |
| **MySQL (InnoDB)** | Good for ACID | Widely used; good tooling | Weaker constraint system; less JSONB | Acceptable alternative |
| **CockroachDB** | Excellent for distributed ACID | Scales horizontally without sharding logic; Postgres-compatible | Higher per-transaction latency (Raft consensus overhead ~3-5ms added) | Consider if > 500K TPS needed |
| **Cassandra** | Poor for balance mutations | High write throughput | No transactions; can't enforce non-negative balance atomically | Selected for audit log read model only |
| **Redis** | Excellent for balance cache + idempotency | Sub-millisecond balance reads; SETNX for idempotency; atomic INCR | Not durable enough as sole balance store; memory-bound | Selected for balance cache + idempotency |
| **ClickHouse** | Excellent for AML analytics | Fast aggregations for velocity features | Not suitable for OLTP | Selected for AML feature store |

**Selected: PostgreSQL (accounts, balances, transfers, ledger) + Redis (balance cache, idempotency, sessions) + Cassandra (audit log, transaction history read model) + ClickHouse (AML velocity features)**

**Key Justification for PostgreSQL:**
- The `CONSTRAINT chk_available_non_negative CHECK (balance - reserved_balance >= 0)` prevents the account from going below zero even under concurrent writes.
- `UPDATE account_balances SET balance = balance - :amount WHERE account_id = :id AND balance >= :amount` — if 0 rows updated, insufficient funds; this is a safe atomic decrement in a single SQL statement.
- The `version` column with optimistic locking (`WHERE version = :expected`) prevents lost updates under concurrent transfers to/from the same account.
- Foreign key from `ledger_entries` to `account_balances` ensures referential integrity — no orphan ledger entries.

---

## 5. API Design

Authentication: JWT Bearer tokens (short-lived, 15-minute expiry) + refresh tokens (30-day expiry). 2FA required for top-up > $500, withdrawal > $100, adding new payment method.

```
POST   /v1/auth/register
  Body: { "phone": "+1-555-1234", "email": "user@example.com", "password": "..." }
  Response 201: { "user_id": "usr_xxx", "status": "pending_kyc" }

POST   /v1/auth/login
  Body: { "email": "...", "password": "...", "device_id": "..." }
  Response 200: { "access_token": "jwt...", "refresh_token": "...", "expires_in": 900 }
  Errors: 401 (invalid credentials), 403 (account suspended)

POST   /v1/auth/2fa/verify
  Body: { "code": "123456", "type": "totp" }
  Response 200: { "access_token": "elevated_jwt...", "scope": "withdrawal" }

POST   /v1/kyc/sessions
  Auth: Bearer JWT
  Body: { "type": "identity_document", "document_type": "passport" }
  Response 201: { "session_id": "kyc_xxx", "sdk_token": "...", "redirect_url": "..." }

GET    /v1/kyc/status
  Auth: Bearer JWT
  Response 200: { "kyc_level": 1, "status": "active", "limits": { "daily_send": 100000 } }

GET    /v1/wallet/balance
  Auth: Bearer JWT
  Response 200: {
    "balances": [
      { "currency": "usd", "balance": 15250, "reserved": 0, "available": 15250 },
      { "currency": "eur", "balance": 5000, "reserved": 0, "available": 5000 }
    ],
    "as_of": "2026-04-09T14:30:00Z"
  }

POST   /v1/transfers
  Auth: Bearer JWT
  Headers: Idempotency-Key: <uuid>
  Rate Limit: 10 transfers/min, 100 transfers/day per user
  Body: {
    "to": {
      "type": "phone",            // or "email", "username", "account_number"
      "value": "+1-555-9876"
    },
    "amount": 5000,               // minor units ($50.00)
    "currency": "usd",
    "note": "Thanks for dinner"
  }
  Response 201: {
    "id": "txfr_xxx",
    "status": "completed",
    "amount": 5000,
    "currency": "usd",
    "recipient": { "display_name": "John D.", "avatar_url": "..." },
    "balance_after": 10250,
    "created_at": "2026-04-09T14:30:01Z"
  }
  Errors: 402 (insufficient funds), 403 (limit exceeded / account frozen),
          404 (recipient not found), 422 (invalid amount), 429 (rate limit)

GET    /v1/transfers/:id
  Auth: Bearer JWT
  Response 200: { transfer object }

GET    /v1/transactions
  Auth: Bearer JWT
  Query: ?limit=25&starting_after=txn_xxx&type=p2p&currency=usd&created[gte]=2026-01-01
  Pagination: Cursor-based on created_at + id
  Response 200: {
    "data": [{ "id": "txn_xxx", "type": "p2p_send", "amount": -5000, ... }],
    "has_more": true
  }

POST   /v1/topup
  Auth: Bearer JWT (elevated, 2FA for amounts > $500)
  Headers: Idempotency-Key: <uuid>
  Body: {
    "amount": 10000,
    "currency": "usd",
    "method": "card",
    "payment_method_id": "pm_xxx"   // or "bank_account_id": "ba_xxx"
  }
  Response 202: {
    "id": "fund_xxx",
    "status": "processing",
    "estimated_arrival": "immediately" // or "2026-04-11" for ACH
  }

POST   /v1/withdrawals
  Auth: Bearer JWT (elevated, 2FA required)
  Headers: Idempotency-Key: <uuid>
  Body: {
    "amount": 5000,
    "currency": "usd",
    "destination": { "type": "bank_account", "bank_account_id": "ba_xxx" },
    "method": "standard"  // or "instant"
  }
  Response 202: { "id": "wdrl_xxx", "status": "pending", "arrival_date": "2026-04-11" }

POST   /v1/fx/convert
  Auth: Bearer JWT
  Headers: Idempotency-Key: <uuid>
  Body: { "from_currency": "usd", "from_amount": 10000, "to_currency": "eur" }
  Response 200: {
    "quote_id": "fx_xxx",
    "from_amount": 10000,
    "to_amount": 9150,
    "exchange_rate": 0.9150,
    "fee_amount": 45,
    "expires_at": "2026-04-09T14:31:00Z"  // 60s to confirm
  }

POST   /v1/fx/confirm/:quote_id
  Auth: Bearer JWT
  Headers: Idempotency-Key: <uuid>
  Response 200: { "id": "fx_xxx", "status": "completed", "to_amount": 9150 }

POST   /v1/payment-methods
  Auth: Bearer JWT
  Body: { "type": "bank_account", "plaid_token": "..." }
  Response 201: { "id": "ba_xxx", "last4": "6789", "institution": "Chase" }

DELETE /v1/payment-methods/:id
  Auth: Bearer JWT

GET    /v1/limits
  Auth: Bearer JWT
  Response 200: {
    "daily_sent": 5000, "daily_limit": 100000, "remaining_today": 95000,
    "weekly_sent": 12000, "weekly_limit": 500000
  }
```

---

## 6. Deep Dive: Core Components

### 6.1 Balance Management and Double-Spend Prevention

**Problem it solves:**

The core correctness requirement of a wallet: a user can never spend more than they have, and concurrent transfers can never result in a negative balance or lost money. This is the classic distributed systems problem of maintaining an invariant (balance >= 0) under concurrent updates.

**Approaches:**

| Approach | Concurrency Safety | Throughput | Complexity |
|----------|-------------------|------------|------------|
| **Single-threaded account actor** | Perfect serialization per account | Low (1 writer per account) | Medium |
| **Pessimistic locking (SELECT FOR UPDATE)** | Safe; serializes transfers per account | Medium (holds lock for duration) | Low |
| **Optimistic locking (version column)** | Safe; detects conflicts via retry | High (no lock held) | Medium |
| **Single SQL atomic decrement** | Safe without application-level lock | High (DB handles concurrency) | Low |
| **Distributed lock (Redis SETNX per account)** | Safe with proper timeout | High | High |
| **Event sourcing (balance derived from events)** | Safe; balance computed from event log | High | Very high |

**Selected: Single SQL atomic decrement for fast path + optimistic locking for multi-step operations**

**Implementation Detail:**

```
P2P Transfer — Atomic Balance Update:

BEGIN TRANSACTION;  -- SERIALIZABLE or REPEATABLE READ

-- Step 1: Debit sender with balance check in one statement
UPDATE account_balances
SET 
  balance = balance - :amount,
  version = version + 1,
  updated_at = NOW()
WHERE 
  account_id = :sender_id 
  AND currency = :currency
  AND balance >= :amount  -- Atomic check: prevents negative balance
  AND version = :expected_sender_version;  -- Optimistic lock

-- If 0 rows updated: either insufficient funds or concurrent modification
-- Disambiguate: re-read balance to determine which
GET DIAGNOSTICS rows_affected = ROW_COUNT;
IF rows_affected = 0 THEN
  ROLLBACK;
  -- Re-read to determine reason
  SELECT balance FROM account_balances WHERE account_id = :sender_id;
  IF balance < :amount THEN
    RETURN error('insufficient_funds');
  ELSE
    RETURN error('concurrent_modification_retry');
  END IF;
END IF;

-- Step 2: Credit recipient (always succeeds if account exists)
UPDATE account_balances
SET 
  balance = balance + :amount,
  version = version + 1,
  updated_at = NOW()
WHERE account_id = :recipient_id AND currency = :currency;

-- Step 3: Insert ledger entries (immutable double-entry record)
INSERT INTO ledger_entries 
  (transaction_id, account_id, currency, amount, direction, entry_type, balance_after, counterpart_id)
VALUES
  (:txn_id, :sender_id, :currency, :amount, 'D', 'p2p_send', :new_sender_balance, :recipient_id),
  (:txn_id, :recipient_id, :currency, :amount, 'C', 'p2p_receive', :new_recipient_balance, :sender_id);

-- Step 4: Insert transfer record
INSERT INTO transfers (id, sender_account_id, recipient_account_id, amount, currency, status, ...)
VALUES (:txn_id, :sender_id, :recipient_id, :amount, :currency, 'completed', ...);

COMMIT;

-- Post-commit: update Redis balance cache
PIPELINE:
  SET balance:{sender_id}:{currency} :new_sender_balance EX 60
  SET balance:{recipient_id}:{currency} :new_recipient_balance EX 60

-- Post-commit: publish to Kafka
PUBLISH transfer.completed { transfer_id, sender_id, recipient_id, amount, currency }
```

**Balance Cache Invalidation Strategy:**

```
READ PATH:
1. GET balance:{account_id}:{currency} from Redis.
2. Cache HIT: return immediately (TTL = 60s; acceptable staleness for display).
3. Cache MISS: 
   a. Acquire Redis lock: SET lock:balance:{account_id} 1 NX EX 5 (5s lock)
   b. Re-check cache (another thread may have populated).
   c. Read from Postgres: SELECT balance FROM account_balances WHERE account_id = :id.
   d. Populate cache with 60s TTL.
   e. Release lock.

WRITE PATH:
After every successful Postgres commit, invalidate (DEL) the Redis key.
Race: between the Postgres commit and Redis invalidation, a read serves stale data.
Acceptable: balance display can be 1-2 seconds stale. Actual balance checked at transfer time.
"Show approximate balance in UI; enforce exact balance at write time."
```

**5 Interviewer Q&As:**

Q: What prevents two concurrent sends from the same account both seeing sufficient balance?
A: The atomic SQL `UPDATE ... WHERE balance >= :amount` is executed as a single statement inside a transaction. PostgreSQL evaluates the WHERE clause and applies the update atomically (with row-level lock). Two concurrent sends cannot both see the same balance — the second one will see the already-decremented balance from the first. If using `REPEATABLE READ` isolation, both transactions may read the same snapshot — but the `WHERE version = :expected_version` optimistic lock ensures only one commits successfully. The other transaction detects 0 rows affected, re-reads the current balance, and returns `insufficient_funds` if balance is now too low.

Q: How do you handle a transfer that debits the sender but crashes before crediting the recipient?
A: This is handled by wrapping both the debit and credit in a single ACID transaction. PostgreSQL's transaction guarantees: if the process crashes after the debit but before the credit, the transaction is never committed — it's rolled back on restart. The money is not lost; the debit is undone. This is why both operations must be in the same database transaction. The COMMIT is the atomicity boundary — either both succeed or neither does.

Q: How do you handle accounts in different shards for cross-shard P2P transfers?
A: This is the hardest problem. Options: (1) **Two-phase commit across shards** — most RDBMS support distributed transactions poorly and they're slow. (2) **Saga pattern** — debit sender (shard A), publish event, credit recipient (shard B) asynchronously. If credit fails, debit is reversed via a compensating transaction. The transfer is eventually consistent with a `pending` state until both sides confirm. (3) **Routing to a single shard via a clearing account** — all cross-shard transfers go through an internal clearing account on a dedicated shard; each shard only makes local transactions. We use option 3 for reliability: send local debit+clearing-credit on sender shard, then clearing-debit+recipient-credit on recipient shard.

Q: How do you compute the transaction history at scale when a user has 1M+ transactions?
A: We use the Cassandra read model for transaction history. Every ledger entry is streamed via Kafka to Cassandra with partition key `(account_id)` and clustering key `(created_at DESC, id)`. Queries like "get last 25 transactions" are efficient even for power users because Cassandra reads from the head of the partition. For older pages, cursor-based pagination using `(created_at, id)` as the cursor avoids OFFSET scans. For reporting (date range aggregations), ClickHouse handles the aggregation efficiently.

Q: What happens if a user's balance shows positive but the money hasn't cleared (pending ACH)?
A: Top-up via ACH funds the `reserved_balance` first, not the `available_balance`. The `available_balance = balance - reserved_balance` stays unchanged until ACH settles (typically T+1 or T+2). We prevent the user from spending reserved funds. The UI shows: "Balance: $100 ($25 pending clearance, $75 available)". When the ACH settles, we release the reserve: `UPDATE account_balances SET balance = balance, reserved_balance = reserved_balance - :amount`. This prevents spending money that might be returned (ACH returns window is 60 days for certain return codes).

---

### 6.2 KYC/AML Compliance System

**Problem it solves:**

Financial regulations (FinCEN BSA in US, FATF internationally) require wallet providers to verify customer identity and monitor for money laundering and terrorist financing. Without KYC, the platform becomes a money laundering vehicle. AML monitoring must happen in near-real-time to block suspicious transactions before they complete, not just report them after the fact.

**Approaches for KYC:**

| Approach | Accuracy | User Experience | Cost |
|----------|----------|----------------|------|
| **Manual document review only** | High (human judgment) | Poor (days to verify) | High (staff cost) |
| **Automated OCR + liveness check** | Good (95%+ for clear documents) | Good (minutes) | Medium |
| **Third-party KYC provider (Jumio/Onfido)** | Very good (trained models) | Good | Higher per-check |
| **Database verification (Experian/Equifax)** | Good for US SSN match | Very fast | Per-lookup fee |
| **Hybrid: automated first, manual fallback** | Highest | Fast for most users | Optimized |

**Selected: Third-party provider (Onfido) for automated flow + internal manual review queue for fallbacks**

**Approaches for AML Monitoring:**

| Approach | False Positive Rate | Latency | Coverage |
|----------|-------------------|---------|----------|
| **Rule-only (threshold-based)** | High | < 1ms | Low (known patterns only) |
| **ML model only (graph neural network)** | Low | 50-200ms | High (novel patterns) |
| **Hybrid: rules filter obvious cases, ML for unclear** | Low | 20ms avg | High |
| **Post-transaction monitoring only** | N/A | No impact on UX | Misses pre-block opportunity |
| **Pre-transaction scoring (blocks suspicious transactions)** | Must be low to not hurt UX | In critical path | Blocks fraud in real time |

**Selected: Pre-transaction hybrid (rules + ML) for high-risk signals; post-transaction monitoring for full analysis.**

**Implementation Detail:**

```
KYC Flow:
Level 0 (no KYC): Can receive money; cannot send > $200/day or add bank account.
Level 1 (basic): Phone + email verified. Can send up to $1,000/day.
Level 2 (full): Government ID + selfie + address proof. Up to $20,000/day.

Automated KYC Process:
1. User submits document via Onfido SDK (in-app camera, not uploaded files).
2. Onfido performs:
   a. Document authenticity check (hologram detection, tamper detection).
   b. OCR (extract name, DOB, ID number, expiry).
   c. Liveness check: video selfie, blink/turn instructions (anti-spoofing).
   d. Face match: selfie vs. ID photo.
3. Onfido returns: clear / consider / refer verdict + breakdown.
4. On 'clear': update accounts.kyc_level = 2, kyc_verified_at = NOW().
5. On 'consider': add to manual review queue; account stays at kyc_level = 1.
6. On 'refer': flag for compliance review; may need additional documents.

Sanctions Screening (OFAC):
1. At KYC submission: name + DOB → World-Check API.
2. On any payout > $3,000: re-screen payout recipient.
3. Nightly batch: re-screen all accounts against updated sanctions list.
4. On match: freeze account, create aml_alert with severity='critical', notify compliance.

AML Pre-Transaction Check:
Algorithm:
1. For every transfer > $100:
   a. Load sender's velocity features from Redis (populated by ClickHouse pipeline):
      - total_sent_last_1h, total_sent_last_24h, transfer_count_last_1h
      - unique_recipients_last_7d, avg_transfer_amount_last_30d
   b. Check rule set:
      - BLOCK if total_sent_last_24h + amount > daily_limit
      - CHALLENGE (2FA) if total_sent_last_1h > 500 USD
      - REVIEW if recipient is new (first time sending to this person) AND amount > 1000 USD
   c. Call ML model (gradient-boosted tree, 10ms inference):
      Features: velocity, recipient risk score, account age, device fingerprint, geo
      Output: risk_score (0-100), recommended action (allow/challenge/block)
   d. Final decision:
      - score < 30 AND all rules pass: ALLOW
      - score 30-70 OR specific rule trigger: CHALLENGE (require 2FA or confirm)
      - score > 70 OR hard rule block: BLOCK (decline transfer, generate AML alert)
2. Store fraud_score and fraud_recommendation on transfer record.

Post-Transaction AML Monitoring (async, Kafka consumer):
1. Consume transfer.completed events.
2. Run pattern detection in ClickHouse:
   - Structuring: multiple transfers just below $10K threshold in 24h
   - Smurfing: same total amount split across many small transfers
   - Layering: rapid receive-and-transfer-out within minutes
3. Generate aml_alerts for pattern matches.
4. Compliance queue: analysts review alerts, escalate or close.
5. SAR filing: if suspicious activity confirmed, file FinCEN SAR within 30 days.
```

**5 Interviewer Q&As:**

Q: How do you handle KYC for users in different countries with different ID types?
A: Onfido supports document extraction for 2,500+ document types across 195 countries. Our configuration maps each country to accepted document types. Regional regulatory requirements are encoded in our configuration layer (e.g., EU users need KYC per 4AMLD/5AMLD; US users need SSN for certain account levels; Indian users need Aadhaar + PAN). A "KYC requirements engine" determines what documents/steps are required based on user's country of residence and the KYC level requested.

Q: What if the automated KYC wrongly rejects a legitimate user?
A: The manual review queue catches most false rejections. When Onfido returns 'consider' (a soft rejection — often due to poor image quality, not actual fraud), we queue the case for a human KYC analyst. Analysts use an internal tool to view documents side-by-side. For hard rejections, users can appeal via support ticket, which routes to a senior KYC analyst. We track false rejection rate and tune Onfido's accept/reject thresholds. Industry-standard false rejection rates are ~5% for automated systems; our target is < 2% with the manual fallback.

Q: How do you avoid blocking legitimate transactions with your AML rules?
A: Two mechanisms: (1) **Threshold tuning**: We use ClickHouse historical data to analyze what thresholds produce < 0.1% false positive rate (legitimate transactions blocked). The rules are tuned quarterly by the compliance team. (2) **Tiered responses**: Instead of block/allow, we have block/challenge/allow. Challenge (step-up authentication) reduces false positives — a legitimate user can complete 2FA and proceed. Block is reserved for highest-confidence flags. (3) **Feedback loop**: When users dispute a block ("I didn't authorize this transfer" → wait, they DID authorize it), compliance analysts mark the block as a false positive, which feeds the ML model retraining pipeline.

Q: How do you handle AML for P2P transfers that split a large payment across days?
A: This is "structuring" — breaking amounts below reporting thresholds. Our velocity features capture cumulative amounts: `total_sent_last_24h`, `total_sent_last_7d`, `total_sent_last_30d`. If a user sends $9,500 on day 1 and $9,500 on day 2, the 7-day and 30-day cumulative views flag this as structuring-like behavior even though each individual transaction is below $10K. The ML model sees the cumulative pattern. We flag for review; the compliance team investigates. If confirmed: file SAR, freeze account pending investigation.

Q: What's the difference between a CTR (Currency Transaction Report) and a SAR (Suspicious Activity Report)?
A: CTR: mandatory report for cash transactions > $10,000 in a single business day. For digital wallets, this typically applies to cash top-ups or withdrawals at a physical location. Filed with FinCEN within 15 days. SAR: filed when there's reasonable suspicion of money laundering, fraud, or other financial crimes — regardless of amount. SAR filing is confidential (must not tip off the subject — no "tipping off" prohibition). Filed within 30 days of discovering suspicious activity. In our system, the compliance team decides on SARs after reviewing AML alerts. CTRs are automated (> $10K cash transaction triggers automatic filing logic).

---

### 6.3 Currency Conversion

**Problem it solves:**

A global wallet must allow users to hold and transact in multiple currencies. Currency conversion involves: getting accurate real-time exchange rates, locking a rate for the duration of the transaction, applying a transparent margin fee, and executing the double-entry ledger entries atomically. The challenge is that exchange rates change constantly — locking a rate means we take FX risk if the rate moves before settlement.

**Approaches:**

| Approach | Rate Accuracy | FX Risk | User Experience |
|----------|--------------|---------|----------------|
| **Live rate at transaction time (no lock)** | Highest | Low | User doesn't know exact amount until confirmation |
| **Rate lock with expiry (60s quote)** | High (locked) | Minimal (60s exposure) | Good — user sees exact amount |
| **Daily rate (updated once/day)** | Low | High (full-day exposure) | Predictable |
| **ECB reference rate + fixed margin** | Good | Moderate | Transparent fee |
| **Real-time rate from FX provider + spread** | Highest | Low (spread covers risk) | Best |

**Selected: Real-time rate from FX provider + locked quote (60s expiry) + transparent fee**

**Implementation Detail:**

```
FX Rate Feed:
- Primary: Bloomberg B-PIPE (institutional grade, low latency)
- Fallback: European Central Bank (ECB) reference rates (hourly updates)
- Rates cached in Redis with 30-second TTL.
- Rate update job polls B-PIPE every 5 seconds; updates Redis.
- Stale rate (> 2 minutes): fail open with last cached rate + wider spread (2% instead of 1%).
- Supported currency pairs: ~50 major and emerging market pairs.

FX Conversion Flow:
1. User requests quote: POST /v1/fx/convert { from: "usd", to: "eur", amount: 10000 }
2. FX Service:
   a. Fetch spot rate from Redis: usd_eur = 0.9200
   b. Apply spread (our revenue): 0.5% = 0.9200 * (1 - 0.005) = 0.9154
   c. Apply fee: 0.4% of from_amount = $4.00 fee
   d. to_amount = from_amount * rate = 10000 * 0.9154 = 9154 cents = €91.54
   e. Store quote in Redis with 60s TTL: fx_quote:{quote_id} = {rate, amounts, expires_at}
   f. Return quote to user with expiry time.
3. User confirms: POST /v1/fx/confirm/:quote_id
   a. Check quote still valid (not expired).
   b. Execute atomic Ledger transaction:
      BEGIN;
        UPDATE account_balances SET balance = balance - 10000 WHERE account_id = :user AND currency = 'usd' AND balance >= 10000;
        UPDATE account_balances SET balance = balance + 9154 WHERE account_id = :user AND currency = 'eur';
        -- Credit fee to platform account:
        UPDATE account_balances SET balance = balance + 400 WHERE account_id = :platform AND currency = 'usd';
        INSERT INTO ledger_entries (fx_out: -10000 usd, fx_in: +9154 eur, fee: +400 usd);
        INSERT INTO fx_conversions (...);
      COMMIT;
4. If quote expired (> 60s): return error, user must request new quote.
   This prevents rate arbitrage attacks (user locks rate, waits for market to move).

FX Risk Management:
- We hold USD, EUR, GBP, etc. in platform accounts.
- Net FX exposure is hedged daily: if we sold $1M of USD to EUR for users, 
  we buy $1M USD forward to hedge.
- Real-time FX book: track net exposure per currency pair.
- Alert treasury when exposure > $10M in any pair.
- Hedging via IB (Interactive Brokers) or bank FX desk.
```

**5 Interviewer Q&As:**

Q: How do you prevent FX arbitrage when rates move rapidly?
A: The 60-second quote lock is the primary control. If USD/EUR moves 0.5% in 60 seconds (extremely rare — typical daily move is < 1%), the user would profit from our locked rate. Mitigation: (1) 60-second lock minimizes the window. (2) Our spread (0.5%) ensures that even a 0.2% adverse rate move doesn't cost us money. (3) Rate volatility guard: if the current spot rate has moved > 1% from the locked rate by the time of confirmation, we reject the confirmation with an "expired rate" error and force a new quote. (4) For large conversions (> $50,000), we use a shorter lock (30 seconds) and narrower spread.

Q: How do you handle currencies with unusual characteristics (JPY with no fractional units, KWD with 3 decimal places)?
A: The ISO 4217 standard defines each currency's minor unit exponent. We maintain a currency configuration table: USD exponent=2 (cents), JPY exponent=0 (no minor units), KWD exponent=3 (fils). All amounts are stored in minor units. API responses include the currency code, and clients must use the exponent to display the decimal amount. For JPY: 1000 stored in DB = ¥1,000 (no division needed). For KWD: 1000 stored = 1.000 KWD. FX calculations must use Decimal arithmetic (not floating point), scaled to the highest precision of the two currencies.

Q: What happens if we can't hedge a currency position (e.g., illiquid emerging market currency)?
A: Restrict availability. For illiquid currencies (e.g., Nigerian Naira, Vietnamese Dong), we either: (1) Disable real-time conversion; only offer T+1 conversion at a wider spread. (2) Use a correspondent bank with daily batch FX for that currency. (3) Offer one-directional conversion only (can't convert back). We maintain an allowlist of currencies for instant conversion and a separate list for deferred conversion. Treasury reviews the list quarterly based on liquidity and hedge availability.

Q: How do you reconcile FX positions at end of day?
A: A nightly reconciliation job: (1) Sum all FX conversions by currency pair from the ledger. (2) Compute net position for each pair (how much USD did we effectively sell?). (3) Compare against hedging positions (FX forwards, swaps). (4) Flag any unhedged positions > $100K for treasury action. (5) Generate a treasury report with P&L on FX conversions for the day. The double-entry ledger makes this straightforward — every FX conversion has a corresponding ledger entry.

Q: How do you handle rollbacks when a currency conversion is later disputed?
A: Currency conversion is not reversible in the same way as a card payment. The exchange rate used is locked in the `fx_conversions` table. If a user disputes a conversion (e.g., claims they didn't authorize it), we do a reversal conversion at the current spot rate — the user bears the FX risk of the reversal. This is disclosed in our terms. If the dispute is a confirmed fraud (someone accessed the account without authorization), we use our FX reserve fund to make the user whole at the original rate. We treat this as an insurance cost.

---

## 7. Scaling

**Horizontal Scaling:**

- **Wallet Service / Transfer Service**: Stateless; scale horizontally. Each pod handles ~500 transfers/sec. 20 pods for 10K TPS.
- **Balance Service**: Read-heavy. Scale with read replicas (Postgres) + Redis cluster. 10 read replicas handle 100K RPS.
- **Ledger Service**: Write-heavy but serialized per account. Shard by `account_id`. Each shard handles writes for its accounts independently.
- **AML Service**: Scale ML inference with GPU pods. Each pod handles ~5,000 scores/sec (XGBoost). 3 GPU pods for 15K TPS AML scoring.
- **Notification Service**: Fan-out; scale with message queue workers. 50 workers handle 50K notifications/sec.

**DB Sharding (Ledger and Balances):**

Shard by `account_id` (UUID). Consistent hashing with 512 virtual shards → physical shard nodes. Cross-shard transfers use the clearing account pattern (avoid distributed transactions). 16 physical shard nodes at 10K TPS; each handles ~625 TPS.

**Replication:**

- Postgres: synchronous replication to 1 standby (RPO=0), async to 2 read replicas per shard.
- Redis: AOF persistence + async replica per AZ. Redis Sentinel for auto-failover.
- Cassandra: RF=3, LOCAL_QUORUM reads/writes, 3 AZs.

**Caching:**

| Data | Cache | TTL | Notes |
|------|-------|-----|-------|
| Account balance | Redis string | 60s | Acceptable display staleness |
| Session tokens | Redis hash | 15m | Invalidated on logout/password change |
| FX rates | Redis hash | 30s | Polled from Bloomberg |
| FX quotes | Redis hash | 60s (quote expiry) | Auto-expires |
| Spend limits + usage | Redis hash | 1m | Recomputed from ledger on miss |
| User lookup by phone/email | Redis string | 5m | For P2P recipient resolution |
| AML velocity features | Redis hash | 1m | Populated by ClickHouse pipeline |

**Interviewer Q&As:**

Q: How does the balance cache maintain consistency with the database?
A: The cache is a "read-through, write-around" cache for display purposes. For the actual transfer decision, we always read from the database (the `UPDATE WHERE balance >= :amount` is always against Postgres). The cache is TTL-invalidated (60 seconds) and also explicitly DEL'd after every balance-changing Postgres commit. The maximum inconsistency window is 60 seconds (in case the explicit invalidation fails). This is acceptable for display but not for authorization decisions.

Q: How do you handle a "thundering herd" when Redis restarts and 100K users' balance caches expire simultaneously?
A: Cache stampede prevention: (1) **Jitter on TTL**: instead of all entries expiring at the same time, TTL = 60 + random(0, 30) seconds. (2) **Redis lock on cache miss**: use `SET lock:balance:{id} 1 NX EX 5` to ensure only one thread refills the cache. Others wait (with exponential backoff) or serve stale data if available. (3) **Warm restart protocol**: before Redis comes back online, we pre-warm the cache from the database for the top N (e.g., 10K) most active accounts. (4) **Circuit breaker**: if Postgres read replica load spikes > 80% CPU, serve stale cache data for up to 5 minutes rather than amplifying the stampede.

Q: How do you scale P2P transfers for large events (e.g., millions of transfers at midnight on New Year)?
A: Predict and pre-scale. We use historical data (last New Year's Eve) to estimate peak TPS. K8s HPA pre-scales Transfer Service pods 30 minutes before expected peak. DB connection pool pre-warmed. Kafka partitions for `transfer.completed` increased (partition count can't decrease, so we over-provision). Redis cluster nodes pre-scaled. Additionally, we implement a brief "soft queue" for transfers during extreme peaks: accept the transfer, return `status: processing`, and complete asynchronously within seconds. From the user's perspective, still feels instant; from the system's perspective, provides a pressure-relief valve.

Q: How does the AML velocity feature pipeline work at scale?
A: (1) Every `transfer.completed` Kafka event consumed by a ClickHouse sink connector. (2) ClickHouse receives ~10K events/sec; batch inserts in 1-second micro-batches. (3) A background job runs materialized views in ClickHouse every 60 seconds:
   ```sql
   SELECT account_id, SUM(amount) as total_sent_1h FROM ledger_entries
   WHERE direction='D' AND created_at > NOW() - INTERVAL '1 hour'
   GROUP BY account_id;
   ```
   (4) Results written to Redis as `velocity:{account_id}:{window}`. (5) AML pre-transaction check reads these Redis keys (< 1ms). (6) At 10K TPS, ClickHouse materialized views need to process 10K rows/sec — well within ClickHouse's capability (handles millions/sec). (7) Redis velocity keys serve the latency-sensitive AML scoring path.

Q: How do you handle schema migrations on the ledger table with 1B+ rows?
A: (1) **Additive changes only**: new nullable columns are instant (catalog change in Postgres; no table rewrite). (2) **Non-additive changes** (changing column type, adding NOT NULL): expand-contract pattern. Add new nullable column, backfill in batches of 10K rows using a background job (1-2 weeks), switch writes to new column, add NOT NULL constraint. (3) **New indexes**: `CREATE INDEX CONCURRENTLY` — doesn't lock the table. Takes hours on 1B-row table but no downtime. (4) **Partition old ledger data**: ledger entries older than 2 years are moved to partitioned tables (range partition by `created_at`). Queries with date filters only touch relevant partitions.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Postgres primary failure (balance shard) | Transfers to/from that shard fail | Health check; Patroni detects in < 10s | Automated promotion of standby; < 30s downtime; RPO=0 with sync standby |
| Redis cluster node failure | Balance cache misses; read latency spikes | Redis cluster health metric | Remaining nodes handle load; replica promoted; no data loss with AOF |
| AML service timeout | Transfer completes without fraud check | Timeout > 60ms | Fail-open: allow transfer, flag for post-hoc review; log for retrain |
| FX rate feed unavailable | Currency conversions fail | Rate age > 2m | Serve stale rate + wider spread (2%); fail completely at 10m stale |
| ACH return (R10 - unauthorized debit) | Top-up reversed; balance credited but not funded | Processor return file | Debit the balance back; if insufficient, freeze account; notify user |
| Concurrent transfer race (same sender) | Second transfer may fail (insufficient funds) | 0 rows in UPDATE | Retry loop with backoff; return 402 if still insufficient after retry |
| KYC provider outage | New user verification delayed | API health check | Queue KYC checks; complete when provider recovers; user notified |
| Notification service failure | Users not notified of transfers | Kafka consumer lag | Kafka retains events; notifications delivered when service recovers |
| Account fraudulently frozen | Legitimate user can't transact | User complaint | Expedited review SLA (< 4h); clear escalation path; auto-unfreeze if false positive |
| Data center network partition | Some users unreachable | Latency spike; timeout spike | Route to another region; cross-region DNS failover |

**Idempotency Protocol:**

Every mutating API call requires `Idempotency-Key`. The key is stored with the transfer result for 24 hours. Retry after a network timeout returns the same result, never creates a duplicate transfer.

**Double-Entry Integrity Check:**

Hourly batch job: `SELECT SUM(CASE WHEN direction='D' THEN -amount ELSE amount END) FROM ledger_entries GROUP BY currency`. For a correctly operating system, this must equal zero (every debit has a matching credit, plus platform fee credits which net to positive). Alert if any currency's sum is non-zero.

---

## 9. Monitoring & Observability

| Metric | Type | SLO / Alert | Purpose |
|--------|------|------------|---------|
| `transfer.success_rate` | Gauge | < 99.5% → P1 | Core health |
| `transfer.p99_latency` | Histogram | > 500ms → P1 | Latency SLO |
| `balance.cache_hit_rate` | Gauge | < 90% → P2 | Cache effectiveness |
| `balance.read.p99_latency` | Histogram | > 100ms → P2 | Read performance |
| `aml.block_rate` | Gauge | > 5% → investigate | False positive check |
| `aml.score_latency_p99` | Histogram | > 60ms → P2 | AML in critical path |
| `kyc.manual_queue_depth` | Gauge | > 500 → P2 | KYC backlog |
| `ledger.double_entry_drift` | Gauge | != 0 → P0 | Financial integrity |
| `ach.return_rate` | Gauge | > 0.5% → P2 | Bank account quality |
| `fx.rate_age_seconds` | Gauge | > 120s → P2 | FX rate freshness |
| `db.replication_lag_ms` | Gauge | > 5000ms → P1 | Data freshness |
| `kafka.consumer_lag{group=aml}` | Gauge | > 10K → P2 | AML backlog |
| `account.negative_balance_count` | Counter | > 0 → P0 | Balance integrity violation |
| `fraud.ml_model_drift_score` | Gauge | > 0.1 → retrain | Model staleness |

**Distributed Tracing:**

W3C `traceparent` propagated through: API Gateway → Wallet Service → Transfer Service → Ledger Service → Postgres. Spans for: idempotency check, recipient resolution, fraud scoring, DB debit, DB credit, Kafka publish. Sample 100% of P2P transfers > $1,000; 1% otherwise.

**Logging:**

Structured JSON. Mandatory fields: `trace_id`, `account_id` (not user name/email — pseudonymized), `operation`, `amount_usd_cents` (for AML), `duration_ms`, `status`. PII not logged (names, phone numbers, email addresses). AML logs have separate retention path (7 years). Compliance logs immutable in Cassandra; application logs 90 days hot.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen Approach | Trade-off / Alternative |
|----------|----------------|------------------------|
| Balance storage | Single Postgres row with atomic UPDATE | Event sourcing more auditable but much higher read complexity |
| Cross-shard transfers | Clearing account (saga) | Distributed 2PC is simpler to reason about but unreliable in practice |
| Balance cache consistency | TTL + explicit invalidation (eventual) | Strong consistency (no cache) would 10× DB read load |
| AML: pre vs. post transaction | Pre-transaction scoring (in critical path) | Post-transaction cheaper but can't block fraud in real time |
| KYC provider | Third-party (Onfido) | In-house: better control but years to build and train models |
| FX rate | Live rate with 60s lock | Daily rate simpler but creates arbitrage opportunities |
| P2P transfer latency target | 500ms P99 | Could be slower if we added more fraud checks; balance vs. security |
| Ledger read model | Cassandra for transaction history | All reads from Postgres simpler but doesn't scale to 100K users with 1M tx each |
| AML features | ClickHouse + Redis (pre-computed velocity) | Real-time SQL on Postgres would lock tables at 10K TPS |
| Notification delivery | Kafka + async workers | Synchronous push would block transfer completion on notification delivery |

---

## 11. Follow-up Interview Questions

Q1: How do you handle the case where a user's bank account is debited for ACH top-up but the ACH is returned?
A: ACH returns can happen up to 60 days after the original debit (NACHA return reason code R05, R07, R10). When an ACH return arrives: (1) We debit the wallet balance by the top-up amount. (2) If balance is sufficient, the debit succeeds; user notified. (3) If balance is insufficient (user already spent the money), the account goes negative — we freeze the account and notify the user to repay. (4) We initiate debt collection if the user doesn't respond. (5) To reduce risk: new users have a hold period (funds credited but reserved) before they can spend ACH-topped-up funds.

Q2: How do you handle a disputed P2P transfer ("I didn't send that")?
A: P2P transfers between wallet users are final by design (like cash). However, if a user claims fraud (unauthorized access to their account), we: (1) Freeze the sender's account and the recipient's account (if the funds haven't left). (2) Investigate: check device fingerprint, IP, login history. (3) If confirmed unauthorized: reverse the transfer (debit recipient, credit sender). If recipient already withdrew: contact bank for clawback (complex, often impossible). (4) Implement: session anomaly detection — unexpected device, unusual time, new location → step-up auth before transfer. (5) This is why 2FA for large transfers is important.

Q3: How do you design the wallet to support offline payments (e.g., no internet connection)?
A: True offline payments require cryptographic authorization without network access. Approach: (1) At session initialization, generate a limited-use offline token (signed JWT with embedded balance cap of $50, valid 24 hours). (2) Merchant's terminal validates the token signature locally. (3) Token includes a nonce list of committed spend amounts (offline ledger). (4) On reconnect, tokens are reconciled with the server. (5) Risk: user could double-spend within the offline cap. Mitigate by keeping offline cap low ($50) and requiring periodic online reconciliation. This is similar to how Visa's offline PIN works.

Q4: How do you implement spend controls (parental controls, business expense limits)?
A: Spend controls are configured on the `accounts` table as JSON rules. At transfer time, the pre-transfer check evaluates: daily/weekly/monthly limits (tracked in Redis as rolling window counters), merchant category restrictions (MCC-based: block gambling, block alcohol), time-of-day restrictions. For business accounts: department-specific limits, manager approval workflow for amounts > threshold (async: transfer in `pending_approval` status, notified to manager, auto-approved/declined based on manager's action). These are application-layer controls, not database-layer.

Q5: How do you handle partial top-up failure (ACH initiated but credit card charge failed)?
A: Each funding method is independent. A card top-up either succeeds or fails synchronously. An ACH top-up is initiated and enters `processing` status; the wallet balance is NOT credited until ACH settles (or we use provisional credit, which carries risk). If the card charge fails (network error after initiating), the idempotency key ensures we don't retry and accidentally charge twice. Partial funding (if we ever split a top-up across methods) would require a saga — each method commits independently; if the second fails, the first is refunded. Simpler design: never split a single top-up across methods.

Q6: How does the wallet handle regulatory requirements for dormant accounts?
A: Dormant account regulations (e.g., US: unclaimed property laws, typically 3-5 years of inactivity) require we report and remit dormant funds to the state. Implementation: (1) Monthly job: identify accounts with no activity for 2 years. (2) Send dormancy warning notifications (email, push). (3) After 3 years: account flagged as dormant; proactive outreach. (4) After 5 years: funds remitted to state unclaimed property authority. (5) Account closure with audit log entry. Users can reclaim funds from the state. This requires per-state compliance rules (each US state has different thresholds and reporting formats).

Q7: How do you protect against account takeover via SIM swap attacks?
A: SIM swap attacks compromise SMS-based 2FA. Mitigations: (1) Recommend TOTP (Google Authenticator) instead of SMS for 2FA. (2) For high-value actions (withdrawal, new bank account), require TOTP regardless of current 2FA method. (3) Withdrawal hold period: 24-hour hold after adding a new bank account (prevents immediate exfiltration after takeover). (4) Anomaly detection: new device login → notify all other devices, require re-authentication for high-value actions. (5) SIM swap detection API: some carriers (T-Mobile, Verizon) offer APIs to detect recent SIM swaps — we check before allowing SMS 2FA for sensitive actions.

Q8: How do you handle refunds for merchant payments made from the wallet?
A: When a merchant refunds a wallet payment, the refund flow is: (1) Merchant initiates refund via payment gateway. (2) Payment gateway notifies our Funding Service that a refund is coming. (3) When the refund settles (card network: T+3 to T+7), Funding Service credits the wallet account. (4) Ledger entry: `refund_credit` type. (5) User notified. For instant refunds to wallet (vs. original payment method): if merchant opts in, we credit the wallet immediately from our float account and recover from the card network refund when it arrives.

Q9: How do you handle multi-device session management?
A: Each login creates a session token (JWT + refresh token). Refresh tokens are stored in Redis with the device ID. On login: add to `sessions:{user_id}` Redis set. On logout: remove from set, add to revocation list (Redis SET `revoked:{jti}` with TTL = token expiry). On password change: revoke all sessions by incrementing a `session_version` counter in Redis; any JWT with an older session version is rejected. Maximum 5 concurrent devices — 6th login revokes the oldest session. Users can view and revoke specific device sessions from the app.

Q10: How do you implement the "request money" flow (receiving-side initiated transfers)?
A: Payment requests are stored as pending requests: (1) Recipient creates a payment request: `POST /v1/payment-requests { from_user: "...", amount: 5000 }`. (2) Sender receives a notification with a deep link. (3) Sender taps "Pay" → creates a transfer linked to the payment request. (4) Transfer completes; payment request status → fulfilled. (5) Requests expire after 7 days. (6) Requests can be cancelled by either party. Data model: `payment_requests` table with `requester_id`, `payer_id`, `amount`, `status`, `expires_at`. The transfer creation links `payment_request_id` for audit trail.

Q11: How do you handle a runaway Kafka consumer that processes AML events too slowly?
A: (1) Monitor Kafka consumer lag metric: `kafka.consumer_lag{group=aml}`. Alert at > 10K events. (2) AML consumers are horizontally scalable: add more pods to the AML consumer group; Kafka rebalances partitions automatically. (3) For lag > 100K events (major backlog after outage): enter "catch-up mode" where the AML service processes events without the real-time ML model (rules-only, 10× faster). Post-catch-up, the missed events are analyzed asynchronously for pattern detection. (4) Priority lanes: high-severity AML rules (OFAC match, large transfers) consume from a dedicated high-priority partition; lower-priority monitoring uses the bulk partition.

Q12: How do you implement card-linked offers (cashback for spending at specific merchants)?
A: (1) Merchant provides a list of their MCC codes or merchant IDs. (2) Offer stored in database: `offers(id, merchant_id, cashback_pct, max_cashback, expires_at, eligible_users)`. (3) After each merchant payment completes, a Kafka consumer checks if the charge matches any active offer for the user. (4) If match: create a `cashback_pending` ledger entry. Cashback is pending until the original payment clears (typically 3-7 days). (5) On payment clearance: `cashback_pending` → `cashback` (credits user balance). (6) If payment is refunded: corresponding cashback is reversed. (7) User sees cashback in transaction history with the linked transaction ID.

Q13: How do you design the system to support group payments (splitting a bill)?
A: Group payments are application-layer orchestration on top of P2P transfers. (1) One user creates a "group payment" for a specific amount (e.g., $120 dinner). (2) Specifies participants and amounts for each. (3) System creates individual P2P transfer requests from each participant to the organizer. (4) As participants pay, the group payment status updates (e.g., 2/4 paid). (5) Organizer can see who has/hasn't paid and send reminders. (6) Optional: organizer sends payment first (using their wallet); system tracks who owes and facilitates collection. (7) Data model: `group_payments` with `organizer_id`, `total_amount`, `participants` (JSONB array of `{user_id, amount, status}`).

Q14: How do you ensure the wallet meets GDPR "right to be forgotten" requirements?
A: Financial records (transactions, balances, ledger entries) must be retained for regulatory compliance (typically 5-7 years per financial regulations). This conflicts with GDPR's right to erasure. Resolution: (1) Pseudonymize PII: user's name, email, phone stored separately in a `user_profiles` table, linked by `user_id`. (2) On deletion request: delete or anonymize the `user_profiles` row (replace with "DELETED USER"). (3) Financial records retain only the `account_id` (an opaque UUID) — not name, email, or phone. (4) KYC documents (ID photos): delete after the legal retention period expires (or immediately on request if legally permissible). (5) Document the legal basis for retention in the privacy notice.

Q15: How do you design an instant payment feature that competes with Venmo/Cash App?
A: Key differentiators: speed, reliability, and zero-fee P2P. Architecture: (1) All P2P transfers within the platform are internal ledger transfers (no bank network involved) — instantly settled. (2) Push notifications delivered within 1 second of transfer completion. (3) Social feed (optional): public/private activity feed showing "Jane sent $20 to Bob" (with privacy controls). (4) QR code payments: each user has a QR code (encodes their account ID + amount); scan to pay in < 3 seconds. (5) Peer discovery: fuzzy search on username/phone/email for recipient lookup (with rate limiting to prevent enumeration). (6) No-fee: P2P transfers are free; revenue from card top-up fees, FX conversion spread, and instant withdrawal fees.

---

## 12. References & Further Reading

- **FinCEN BSA/AML Regulations** (Bank Secrecy Act requirements): https://www.fincen.gov/resources/statutes-regulations
- **FATF (Financial Action Task Force) 40 Recommendations**: https://www.fatf-gafi.org/en/topics/fatf-recommendations.html
- **Plaid API Documentation** (bank account verification): https://plaid.com/docs/
- **NACHA Operating Rules and Guidelines** (ACH): https://www.nacha.org/rules
- **Onfido Developer Documentation** (KYC/identity verification): https://documentation.onfido.com/
- **Visa Direct API** (instant payouts to debit cards): https://developer.visa.com/capabilities/visa_direct
- **"Designing Data-Intensive Applications"** — Martin Kleppmann (O'Reilly, 2017) — Chapter 7 (Transactions)
- **"Building Microservices"** — Sam Newman (O'Reilly, 2021) — Chapter on Saga Pattern
- **ECB Reference Exchange Rates**: https://www.ecb.europa.eu/stats/policy_and_exchange_rates/euro_reference_exchange_rates/html/index.en.html
- **"The Mechanics of AML Compliance"** — Wolfsberg Group: https://www.wolfsberg-principles.com/
- **ISO 4217 Currency Codes and Minor Units**: https://www.iso.org/iso-4217-currency-codes.html
- **OFAC Sanctions Screening** (US Treasury): https://ofac.treasury.gov/
- **Google SRE Book — Chapter 26: Data Integrity**: https://sre.google/sre-book/data-integrity/
- **Stripe Treasury Documentation** (embedded finance/wallet pattern): https://stripe.com/docs/treasury
- **Redis Documentation — Distributed Locks (Redlock)**: https://redis.io/docs/manual/patterns/distributed-locks/
