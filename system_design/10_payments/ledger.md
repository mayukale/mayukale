# System Design: Financial Ledger System

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Double-Entry Bookkeeping**: Every financial event produces two or more ledger entries that balance to zero (debits equal credits). The fundamental invariant of accounting.
2. **Immutable Append-Only Log**: Ledger entries are never modified or deleted. Corrections are made via reversal entries. The full history is permanently preserved.
3. **Account Balance Computation**: Derive the current balance of any account at any point in time by summing its ledger entries up to that timestamp.
4. **Balance Verification**: Compute and verify that the sum of all ledger entries across the entire system equals zero (balanced books) at any point in time.
5. **Reconciliation**: Automated daily reconciliation against external systems (bank statements, card network files, acquirer settlement files). Flag and surface discrepancies.
6. **Audit Trail**: Every ledger entry must be traceable to a source event, an actor, a timestamp, and a business reason. Required for regulatory audits.
7. **Reporting**: Profit & loss, balance sheet, cash flow statements; period-close reports; per-merchant financial summaries.
8. **Multi-Currency**: Support entries in any ISO 4217 currency; maintain separate sub-ledgers per currency.
9. **Large-Scale Aggregation**: Compute balance for any account at any time efficiently, even for accounts with millions of entries.
10. **Idempotent Entry Creation**: Posting the same business event twice must not create duplicate entries.

### Non-Functional Requirements

1. **Durability**: Absolute. No ledger entry must ever be lost. Write durability before acknowledging.
2. **Consistency**: Strong consistency. The balance derived from ledger entries must always match the operational balance. No phantom credits or phantom debits.
3. **Immutability**: Ledger entries, once written, cannot be modified or deleted — enforced at infrastructure level (DB permissions, append-only tables, cryptographic hash chains).
4. **Availability**: 99.99%. Inability to post entries means payments are blocked.
5. **Audit Compliance**: PCI DSS (payment records), SOX (for public companies: accurate financial reporting), local financial regulations (7-10 year retention depending on jurisdiction).
6. **Throughput**: Handle 500,000 entry insertions per second at peak (payment processor scale: 100K TPS × 2 entries per transaction + fees).
7. **Query Performance**: Balance query for any account at any point in time: P99 < 500ms, even for accounts with 10M+ entries.
8. **Scalability**: Support 100 billion+ total ledger entries over 10-year operation.

### Out of Scope

- General-purpose ERP accounting (Accounts Payable, Accounts Receivable beyond payments)
- Tax calculation and filing
- GAAP/IFRS accrual accounting (this design focuses on cash-basis payment ledger)
- Multi-entity consolidation (single legal entity assumed)
- Manual journal entry UI (operations are API-driven)

---

## 2. Users & Scale

### User Types

| User Type | Description | Primary Interactions |
|-----------|-------------|---------------------|
| **Payment System** | Core payment processing service | Posts entries via internal API |
| **Finance Ops** | Reconciliation, exception management | Reads reports, resolves discrepancies |
| **Auditor (Internal/External)** | Period-end audit, compliance review | Read-only access to full audit trail |
| **Merchant** | Reads their own financial summary | Balance, transaction history, payouts |
| **Compliance Officer** | Regulatory reporting, SAR/CTR support | Transaction investigation queries |
| **Reporting System** | Automated financial reports | Large aggregation queries |

### Traffic Estimates

**Assumptions:**
- Underlying payment system processes 100,000 TPS at peak.
- Each payment transaction generates: 2 core ledger entries (debit + credit) + 1 fee entry + 1 platform revenue entry = average 3.5 entries per transaction.
- Reconciliation jobs: 50 concurrent batch queries, each touching 10M rows.
- Balance reads: merchants and internal systems read account balances; 10× the write rate.
- Reporting: 100 concurrent analytical queries during business hours.

| Metric | Calculation | Result |
|--------|-------------|--------|
| Ledger entries per second (peak) | 100,000 TPS × 3.5 entries | 350,000 entries/sec |
| Ledger entries per day | 350,000 × 86,400 | 30.24 billion entries/day |
| Balance read requests/sec | 350,000 × 10 (read:write) | 3,500,000 RPS |
| Reconciliation row scans/day | 50 jobs × 10M rows × 5 scans | 2.5B row scans/day |
| Entries per year | 30.24B × 365 | ~11 trillion/year |
| Entries over 10 years | 11T × 10 | ~110 trillion total |
| Unique accounts (merchants + internal) | 10M merchants + 1K internal accounts | ~10M accounts |
| Average entries per account over 10 years | 110T / 10M | 11M entries/account avg |

**Design target: 500,000 entry writes/sec; 5,000,000 balance reads/sec.**

### Latency Requirements

| Operation | P50 | P95 | P99 | Notes |
|-----------|-----|-----|-----|-------|
| Post single entry batch | 2ms | 10ms | 50ms | Critical path for payments |
| Balance query (current) | 1ms | 5ms | 10ms | Materialized balance (Redis/DB) |
| Balance at point-in-time | 20ms | 100ms | 500ms | Historical computation |
| Account statement (last 30 days) | 50ms | 200ms | 500ms | Indexed range scan |
| Reconciliation query (1M entries) | 2s | 10s | 30s | Batch; not real-time |
| Full-book balance check | 5m | 20m | 60m | Nightly batch; not real-time |
| Period-close report | 10m | 30m | 2h | Scheduled job |

### Storage Estimates

**Assumptions:**
- Core ledger entry: 200B (entry_id BIGINT, account_id UUID, amount BIGINT, currency CHAR(3), direction CHAR(1), type VARCHAR(30), created_at TIMESTAMPTZ, transaction_id UUID, hash VARCHAR(64)) — total ~200B per entry after overhead.
- Index overhead: ~2× base row size (multiple indexes on account_id, transaction_id, created_at).
- Materialized balances: 1 row per account per currency = 100B.
- Audit metadata: ~100B extra per entry for actor, reason, source reference.

| Data Type | Per-record | Daily Volume | Daily Storage | 10-Year Total |
|-----------|-----------|--------------|---------------|---------------|
| Ledger entries (core) | 200B | 30.24B | 6.05TB/day | ~22PB |
| Entry indexes | ~400B | 30.24B | ~12.1TB/day | ~44PB |
| Materialized balances | 100B | 10M accounts | 1GB (low write) | ~1TB |
| Audit metadata | 100B | 30.24B | 3.02TB/day | ~11PB |
| Reconciliation records | 500B | 5M/day | 2.5GB/day | ~9TB |

**Total raw storage over 10 years: ~77PB (before compression).** With columnar compression (column stores achieve 5-10× compression on financial data): ~8-15PB practical. Tiered storage: hot (recent 90 days) = ~1.6PB; warm (90 days - 2 years) = ~5PB; cold (> 2 years) = remainder on object storage.

### Bandwidth Estimates

| Traffic Type | Per-operation | RPS | Bandwidth |
|---|---|---|---|
| Entry insertion (batched) | 10KB per batch of 50 entries | 7,000 batch RPS | ~70 MB/s write |
| Balance reads | 500B response | 3,500,000 | ~1,750 MB/s |
| Replication (3× writes) | 3× write volume | — | ~210 MB/s |
| Reconciliation query results | 1MB per job result | 1/s | ~1 MB/s |
| Kafka event consumption | 200B per entry | 350,000 | ~70 MB/s |
| **Total** | | | **~2.1 GB/s** |

---

## 3. High-Level Architecture

```
        ┌─────────────────────────────────────────────────────────────────┐
        │               UPSTREAM PAYMENT SERVICES                        │
        │  Payment Processor │ Wallet Service │ Connect Platform         │
        │  Payout Service │ Fee Engine │ FX Conversion Service           │
        └─────────────────────────┬───────────────────────────────────────┘
                                  │ gRPC / REST (internal)
                                  │ Idempotency-Key header required
        ┌─────────────────────────▼───────────────────────────────────────┐
        │                   LEDGER API SERVICE                           │
        │   - Accepts posting requests (validated journal entries)        │
        │   - Idempotency deduplication                                  │
        │   - Schema validation (balanced entries required)               │
        │   - Route to appropriate write path                             │
        └──────────────────┬───────────────────────────┬──────────────────┘
                           │ Write Path                │ Read Path
             ┌─────────────▼──────────────┐  ┌────────▼──────────────────┐
             │   WRITE COORDINATOR        │  │  READ SERVICE             │
             │   - Validates double-entry │  │  - Serves balance reads   │
             │     invariant before write │  │    from Redis cache first │
             │   - Batches entries        │  │  - Falls through to       │
             │   - Writes to primary DB   │  │    materialized balance   │
             │   - Appends hash chain     │  │    or historical compute  │
             │   - Publishes to Kafka     │  └────────┬──────────────────┘
             └─────────────┬──────────────┘           │
                           │                          │
          ┌────────────────▼────────────────┐  ┌──────▼──────────────────┐
          │  PRIMARY LEDGER STORE           │  │  BALANCE CACHE          │
          │  (Apache Cassandra + ScyllaDB)  │  │  (Redis Cluster)        │
          │  - Partition: account_id        │  │  - Materialized balances│
          │  - Cluster: (created_at, id)    │  │  - Per account/currency │
          │  - Immutable (no UPDATE/DELETE) │  │  - TTL: 60s             │
          │  - RF=3, LOCAL_QUORUM           │  └─────────────────────────┘
          │  - Write path: < 5ms            │
          └────────────────┬────────────────┘
                           │
          ┌────────────────▼────────────────────────────────────────────┐
          │               EVENT STREAMING LAYER                         │
          │               Apache Kafka                                  │
          │   Topics: ledger.entries (500 partitions)                   │
          │           ledger.balance-updates                             │
          │           ledger.reconciliation-events                      │
          └────┬──────────────┬────────────────────┬────────────────────┘
               │              │                    │
     ┌─────────▼────┐  ┌──────▼──────────┐  ┌──────▼────────────────────┐
     │ Balance       │  │  OLAP / REPORTS │  │  RECONCILIATION           │
     │ Materializer  │  │  ClickHouse     │  │  ENGINE                   │
     │ (updates      │  │  (period-close  │  │  - Compares ledger vs.    │
     │  Redis +      │  │   reports, P&L, │  │    external statements    │
     │  PG balance   │  │   balance sheet)│  │  - Flags discrepancies    │
     │  snapshot)    │  └─────────────────┘  │  - Auto-resolves if rules │
     └──────────────┘                        │    match                  │
                                             └────────────────────────────┘
          ┌────────────────────────────────────────────────────────────────┐
          │            SUPPORTING STORES                                  │
          │   PostgreSQL (accounts, materialized_balances, idempotency,   │
          │               reconciliation_exceptions)                      │
          │   S3/GCS (cold ledger archive > 2 years; settlement files;   │
          │           audit exports; hash chain verification snapshots)   │
          └────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

| Component | Role |
|-----------|------|
| **Ledger API Service** | Entry point for all posting requests; validates balanced entries; deduplicates via idempotency key |
| **Write Coordinator** | Validates double-entry invariant; batches inserts; appends cryptographic hash chain; publishes to Kafka |
| **Primary Ledger Store (Cassandra)** | Append-only, write-optimized store for all ledger entries; partitioned by account_id; clustered by created_at |
| **Balance Cache (Redis)** | Materialized current balances per account per currency; O(1) lookups; 60s TTL |
| **Kafka Event Bus** | Decouples ledger writes from downstream consumers; 500 partitions for high throughput |
| **Balance Materializer** | Consumes Kafka entries; maintains Redis cache and Postgres balance snapshots |
| **ClickHouse (OLAP)** | Column-store for period reports, P&L, reconciliation aggregations, analytical queries |
| **Reconciliation Engine** | Compares internal ledger against external settlement files; generates exception reports |
| **PostgreSQL** | Accounts metadata, materialized balance snapshots (durable), idempotency keys, reconciliation exceptions |

**Primary Use-Case Data Flow (Posting a Payment Transaction):**

1. Payment processor calls `POST /v1/ledger/post` with a journal entry (balanced set of entries).
2. Ledger API Service: validate idempotency key, validate all entries sum to zero, validate account IDs exist.
3. Write Coordinator: compute hash chain node (SHA-256 of previous hash + entry data), batch with other entries.
4. Write to Cassandra (primary ledger store) in a batch. Cassandra `BATCH` statement.
5. Update materialized balance in Postgres atomically: `UPDATE materialized_balances SET balance = balance + :delta WHERE account_id = :id AND currency = :ccy`.
6. Publish entry events to Kafka topic `ledger.entries`.
7. Return success to payment processor (after Cassandra + Postgres writes confirmed).
8. Kafka consumers (async): Balance Materializer updates Redis, ClickHouse ingests for analytics.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- CHART OF ACCOUNTS
-- ============================================================
CREATE TABLE accounts (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    code                VARCHAR(20)     NOT NULL UNIQUE,   -- e.g., '1000', '1001-MERCHANT-xxx'
    name                VARCHAR(255)    NOT NULL,
    type                account_type    NOT NULL,
    normal_balance      CHAR(1)         NOT NULL,          -- 'D' = debit-normal, 'C' = credit-normal
    currency            CHAR(3),                           -- NULL = multi-currency account
    parent_account_id   UUID            REFERENCES accounts(id),
    merchant_id         UUID,                              -- NULL for system accounts
    is_system           BOOLEAN         NOT NULL DEFAULT FALSE,
    status              VARCHAR(20)     NOT NULL DEFAULT 'active',
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TYPE account_type AS ENUM (
    'asset',        -- Cash, receivables (debit-normal)
    'liability',    -- Merchant payable balances (credit-normal)
    'equity',       -- Retained earnings (credit-normal)
    'revenue',      -- Fee income, interchange income (credit-normal)
    'expense'       -- Processing costs, refund costs (debit-normal)
);

-- System Accounts (Chart of Accounts for a payment processor):
-- 1000 - Cash (Asset, D)              Our bank account
-- 2000 - Merchant Payable (Liability, C)  What we owe merchants
-- 2001 - Customer Wallet (Liability, C)   What we owe wallet users
-- 3000 - Revenue - Processing Fees (Revenue, C)
-- 3001 - Revenue - FX Spread (Revenue, C)
-- 4000 - Expense - Interchange (Expense, D)
-- 4001 - Expense - Network Fees (Expense, D)
-- 5000 - Receivable - Cardholder (Asset, D)  Authorized but not settled
-- Note: Every merchant gets account 2000-{merchant_id}

-- ============================================================
-- LEDGER ENTRIES (primary immutable append-only table)
-- In Cassandra (primary store):
-- Partition key: account_id
-- Clustering key: (created_at DESC, id)
-- ============================================================
-- Cassandra DDL:
CREATE TABLE ledger.entries (
    account_id          UUID,
    created_at          TIMESTAMP,
    id                  TIMEUUID,                          -- UUID v1 (time-based) for uniqueness + ordering
    transaction_id      UUID            NOT NULL,          -- Groups related entries
    amount              BIGINT          NOT NULL,          -- Always positive
    direction           TINYINT         NOT NULL,          -- 1 = DEBIT, -1 = CREDIT
    currency            TEXT            NOT NULL,
    entry_type          TEXT            NOT NULL,
    balance_after       BIGINT          NOT NULL,          -- Running balance after this entry
    source_system       TEXT,
    source_reference    TEXT,                              -- External reference (charge_id, payout_id, etc.)
    actor_id            TEXT,
    actor_type          TEXT,
    description         TEXT,
    metadata            TEXT,                              -- JSON-encoded
    previous_entry_hash TEXT,                              -- SHA-256 of previous entry (hash chain)
    entry_hash          TEXT,                              -- SHA-256 of this entry's fields
    PRIMARY KEY ((account_id), created_at, id)
) WITH CLUSTERING ORDER BY (created_at DESC, id DESC)
  AND compaction = {'class': 'TimeWindowCompactionStrategy', 'compaction_window_unit': 'DAYS', 'compaction_window_size': 30}
  AND default_time_to_live = 0;  -- No TTL; entries are permanent

-- Index for transaction_id lookup (find all entries for a transaction):
CREATE MATERIALIZED VIEW ledger.entries_by_transaction AS
    SELECT * FROM ledger.entries
    WHERE transaction_id IS NOT NULL AND account_id IS NOT NULL AND created_at IS NOT NULL AND id IS NOT NULL
    PRIMARY KEY (transaction_id, account_id, created_at, id);

-- ============================================================
-- TRANSACTIONS (groups related entries; PostgreSQL)
-- ============================================================
CREATE TABLE ledger_transactions (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    idempotency_key     VARCHAR(255)    NOT NULL,
    source_system       VARCHAR(50)     NOT NULL,          -- 'payment_processor', 'wallet', 'payout'
    source_event_id     UUID            NOT NULL,          -- e.g., charge_id, transfer_id
    source_event_type   VARCHAR(50)     NOT NULL,          -- 'charge.captured', 'p2p.transfer', etc.
    entry_count         SMALLINT        NOT NULL,
    total_debit         BIGINT          NOT NULL,
    total_credit        BIGINT          NOT NULL,
    is_balanced         BOOLEAN         NOT NULL GENERATED ALWAYS AS (total_debit = total_credit) STORED,
    currency            CHAR(3),                           -- NULL for multi-currency transactions
    posted_at           TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_idempotency UNIQUE (source_system, idempotency_key),
    CONSTRAINT chk_balanced CHECK (total_debit = total_credit)  -- Double-entry invariant enforced at DB level
);

CREATE INDEX idx_ltx_source ON ledger_transactions (source_system, source_event_id);
CREATE INDEX idx_ltx_posted_at ON ledger_transactions (posted_at DESC);

-- ============================================================
-- MATERIALIZED BALANCES (for fast balance lookups; PostgreSQL)
-- ============================================================
CREATE TABLE materialized_balances (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    currency            CHAR(3)         NOT NULL,
    balance             BIGINT          NOT NULL DEFAULT 0,
    last_entry_id       UUID,                              -- Last Cassandra entry ID that updated this
    last_updated_at     TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    version             BIGINT          NOT NULL DEFAULT 0,
    CONSTRAINT uq_account_currency UNIQUE (account_id, currency),
    CONSTRAINT chk_balance_non_negative CHECK (balance >= 0 OR is_liability_account(account_id))
    -- Note: is_liability_account() is a function that checks accounts.type = 'liability'
    -- Asset accounts cannot go negative; liability accounts represent what we owe (can "increase")
);

-- ============================================================
-- BALANCE SNAPSHOTS (periodic; for efficient point-in-time queries)
-- ============================================================
CREATE TABLE balance_snapshots (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL REFERENCES accounts(id),
    currency            CHAR(3)         NOT NULL,
    balance             BIGINT          NOT NULL,
    snapshot_at         TIMESTAMPTZ     NOT NULL,          -- Time the snapshot represents
    entry_id_at_snapshot UUID           NOT NULL,          -- Last entry included in this snapshot
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_snapshots_account_time ON balance_snapshots (account_id, currency, snapshot_at DESC);

-- ============================================================
-- RECONCILIATION RUNS
-- ============================================================
CREATE TABLE reconciliation_runs (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    run_date            DATE            NOT NULL,
    source_system       VARCHAR(50)     NOT NULL,          -- 'visa_network', 'mastercard', 'adyen', 'bank'
    status              VARCHAR(20)     NOT NULL DEFAULT 'running',
    total_internal      BIGINT,                            -- Sum of our entries
    total_external      BIGINT,                            -- Sum of external file
    match_count         INT,
    exception_count     INT,
    currency            CHAR(3)         NOT NULL,
    started_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    completed_at        TIMESTAMPTZ,
    CONSTRAINT uq_recon_run UNIQUE (run_date, source_system, currency)
);

CREATE TABLE reconciliation_exceptions (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    run_id              UUID            NOT NULL REFERENCES reconciliation_runs(id),
    exception_type      VARCHAR(30)     NOT NULL,          -- 'missing_in_external', 'missing_in_internal', 'amount_mismatch'
    internal_entry_id   UUID,
    external_reference  VARCHAR(255),
    internal_amount     BIGINT,
    external_amount     BIGINT,
    currency            CHAR(3)         NOT NULL,
    status              VARCHAR(20)     NOT NULL DEFAULT 'open',
    resolution          VARCHAR(100),
    resolved_by         UUID,
    resolved_at         TIMESTAMPTZ,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- ============================================================
-- HASH CHAIN VERIFICATION (for tamper detection)
-- ============================================================
CREATE TABLE hash_chain_checkpoints (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id          UUID            NOT NULL,
    checkpoint_at       TIMESTAMPTZ     NOT NULL,
    entry_id            UUID            NOT NULL,
    cumulative_hash     VARCHAR(64)     NOT NULL,          -- Hash of all entries up to this point
    entry_count         BIGINT          NOT NULL,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);
```

### Database Choice

**Options Evaluated:**

| Database | Write Throughput | Append-Only Fit | Query Flexibility | Scale | Decision |
|----------|----------------|----------------|-----------------|-------|----------|
| **Cassandra / ScyllaDB** | Very high (100K+ writes/sec per node) | Excellent (LSM-tree is naturally append-optimized; can disable compaction on cold data) | Limited (partition key + clustering key only) | Horizontal, linear scale | **Selected: primary ledger store** |
| **PostgreSQL** | Good (50K TPS per instance) | Good (append-only table with role permissions) | Excellent (SQL, complex joins, window functions) | Vertical + sharding | **Selected: materialized balances, transactions, reconciliation** |
| **Apache Parquet on S3** | N/A (write once) | Excellent (columnar, immutable) | Via Athena/Spark (minutes latency) | Petabyte scale | **Selected: cold archive > 2 years** |
| **ClickHouse** | Good (10M+ rows/sec bulk inserts) | Good | Excellent for analytics (aggregation, GROUP BY) | Horizontal | **Selected: OLAP reporting** |
| **MySQL (InnoDB)** | Good | Good | Good | Vertical | Not selected (weaker constraint system vs. Postgres) |
| **TigerBeetle** | Extremely high (designed for ledgers) | Excellent (purpose-built financial ledger) | Limited (specialized API) | Scale-out | Consider for greenfield; less ecosystem maturity |

**Key Justification:**

- **Cassandra for primary ledger**: The ledger is the highest-write-throughput component in the system. Cassandra's LSM-tree (Log-Structured Merge Tree) architecture is fundamentally append-only: writes go to a memtable and are periodically flushed to immutable SSTables. This aligns perfectly with ledger semantics. Partition key `account_id` ensures all entries for an account are co-located — range scans for account history are single-node operations. `TimeWindowCompactionStrategy` organizes SSTables by time window, making time-range queries efficient and enabling simple data tiering (older SSTables migrate to cheaper storage).

- **Postgres for materialized balances**: Balance reads must be O(1), not O(N) where N is the number of entries. Postgres maintains the running balance as a single row per account with ACID guarantees. The balance is updated atomically with each posting (in the same DB transaction as the idempotency key write). PostgreSQL's `CHECK (total_debit = total_credit)` on `ledger_transactions` enforces the double-entry invariant at the database layer — even a bug in the application cannot post an unbalanced entry.

- **Parquet on S3 for cold archive**: After 2 years, ledger entries are compressed into Parquet files (achieves 10-20× compression on financial data) and stored in S3/GCS with immutable object lock (WORM - Write Once Read Many). AWS S3 Object Lock with Compliance mode prevents deletion even by the account owner — satisfying regulatory retention requirements. Queries against cold data use Athena or Spark.

---

## 5. API Design

Internal gRPC API (not public-facing):

```protobuf
// ledger.proto
service LedgerService {
  rpc PostTransaction (PostTransactionRequest) returns (PostTransactionResponse);
  rpc GetBalance (GetBalanceRequest) returns (GetBalanceResponse);
  rpc GetBalanceAtTime (GetBalanceAtTimeRequest) returns (GetBalanceAtTimeResponse);
  rpc GetAccountStatement (GetStatementRequest) returns (stream LedgerEntry);
  rpc VerifyHashChain (VerifyHashChainRequest) returns (VerifyHashChainResponse);
  rpc GetTransaction (GetTransactionRequest) returns (GetTransactionResponse);
  rpc RunReconciliation (RunReconciliationRequest) returns (RunReconciliationResponse);
}
```

REST API for internal tooling and reporting systems:

```
POST   /v1/ledger/transactions
  Auth: Internal mTLS (service-to-service)
  Headers: Idempotency-Key: <uuid>, X-Source-System: payment_processor
  Body: {
    "source_event_id": "charge_abc123",
    "source_event_type": "charge.captured",
    "entries": [
      {
        "account_id": "acct_cash",
        "amount": 10000,
        "direction": "debit",
        "currency": "usd",
        "entry_type": "charge_settled",
        "description": "Card payment settlement"
      },
      {
        "account_id": "acct_merchant_xyz",
        "amount": 9700,
        "direction": "credit",
        "currency": "usd",
        "entry_type": "merchant_payable",
        "description": "Net proceeds to merchant"
      },
      {
        "account_id": "acct_revenue_fees",
        "amount": 300,
        "direction": "credit",
        "currency": "usd",
        "entry_type": "processing_fee_revenue",
        "description": "Processing fee 3%"
      }
    ]
  }
  Validation: SUM(debit amounts) MUST EQUAL SUM(credit amounts) — 10000 = 9700 + 300
  Response 201: {
    "transaction_id": "ltx_xxx",
    "posted_at": "2026-04-09T14:30:01.234Z",
    "entry_ids": ["eid_1", "eid_2", "eid_3"],
    "is_balanced": true
  }
  Errors: 400 (unbalanced entries), 409 (duplicate idempotency key), 404 (account not found),
          422 (account currency mismatch), 429 (rate limit)

POST   /v1/ledger/transactions/:id/reverse
  Auth: Internal mTLS
  Headers: Idempotency-Key: <uuid>
  Body: { "reason": "charge_refunded", "source_event_id": "refund_xyz" }
  Response 201: {
    "transaction_id": "ltx_yyy",    // New reversal transaction
    "reverses": "ltx_xxx",
    "posted_at": "2026-04-09T15:00:00Z"
  }
  Note: Creates equal-and-opposite entries (swaps debits and credits of original)
  Note: Does NOT modify original entries — immutability preserved

GET    /v1/ledger/accounts/:id/balance
  Auth: Internal mTLS or Merchant JWT (for own account)
  Query: ?currency=usd
  Response 200: {
    "account_id": "acct_merchant_xyz",
    "currency": "usd",
    "balance": 97000,              // minor units; debit-normal accounts show positive for debit balance
    "balance_type": "credit",      // liability account: positive = we owe this amount
    "as_of": "2026-04-09T14:30:05Z",
    "source": "materialized"       // 'materialized' = from snapshot; 'computed' = from entries
  }

GET    /v1/ledger/accounts/:id/balance/at
  Auth: Internal mTLS
  Query: ?as_of=2026-01-01T00:00:00Z&currency=usd
  Response 200: {
    "balance": 50000,
    "computed_from_entries": 1547892,  // number of entries summed
    "snapshot_used": "snap_abc",       // nearest snapshot used as base
    "as_of": "2026-01-01T00:00:00Z"
  }

GET    /v1/ledger/accounts/:id/statement
  Auth: Internal mTLS or Merchant JWT
  Query: ?from=2026-01-01&to=2026-03-31&currency=usd&limit=100&cursor=eid_xxx
  Pagination: Cursor-based (entry ID)
  Response 200: {
    "data": [
      {
        "id": "eid_xxx",
        "transaction_id": "ltx_xxx",
        "amount": 10000,
        "direction": "debit",
        "balance_after": 150000,
        "entry_type": "charge_settled",
        "created_at": "2026-04-09T14:30:01Z",
        "description": "Card payment settlement"
      }
    ],
    "has_more": true,
    "next_cursor": "eid_yyy"
  }

GET    /v1/ledger/transactions/:id
  Auth: Internal mTLS
  Response 200: { transaction + all entries }

POST   /v1/ledger/reconcile
  Auth: Internal mTLS (Finance Ops only)
  Body: { "source": "visa_network", "date": "2026-04-08", "file_url": "s3://..." }
  Response 202: { "run_id": "recon_xxx", "status": "running" }

GET    /v1/ledger/reconcile/:run_id
  Response 200: { "status": "completed", "match_count": 9850000, "exception_count": 15 }

GET    /v1/ledger/reconcile/:run_id/exceptions
  Query: ?limit=100&cursor=exc_xxx
  Response 200: { "data": [...exceptions...], "has_more": false }

GET    /v1/ledger/verify/hash-chain
  Auth: Internal mTLS (Audit access)
  Query: ?account_id=acct_xyz&from=2026-01-01&to=2026-04-01
  Response 200: { "valid": true, "entries_verified": 1500000, "last_hash": "abc123..." }

GET    /v1/ledger/reports/trial-balance
  Auth: Internal mTLS (Finance Ops)
  Query: ?as_of=2026-04-01&currency=usd
  Response 200: {
    "accounts": [
      { "code": "1000", "name": "Cash", "type": "asset", "debit_balance": 1000000000, "credit_balance": 0 },
      { "code": "2000", "name": "Merchant Payable", "type": "liability", "debit_balance": 0, "credit_balance": 950000000 }
    ],
    "total_debits": 1000000000,
    "total_credits": 1000000000,
    "is_balanced": true
  }
```

---

## 6. Deep Dive: Core Components

### 6.1 Immutable Append-Only Log with Hash Chain Integrity

**Problem it solves:**

Financial records must be tamper-evident. If an attacker modifies historical ledger entries (to hide fraud, inflate balances, or satisfy an audit), the system must be able to detect the modification. The append-only constraint plus a cryptographic hash chain provides mathematical proof that no historical entry has been altered.

**Approaches:**

| Approach | Tamper Evidence | Complexity | Performance |
|----------|----------------|------------|-------------|
| **DB constraints (no UPDATE/DELETE role)** | Prevents accidental modification; doesn't detect sophisticated DB-level attacks | Low | None |
| **Cryptographic hash chain (Bitcoin-style)** | Detects modification of any entry; any change breaks the chain | Medium | ~1ms hash per entry |
| **Merkle tree (batch-based)** | Root hash commits to all entries in a block; efficient batch verification | High | O(log N) verification per entry |
| **External blockchain anchor** | Periodic commitment to a public blockchain provides external witness | Very high | Low throughput |
| **Append-only storage (S3 Object Lock, Cassandra)** | Infrastructure prevents modification at storage layer | Low | None |
| **Combined: DB constraints + hash chain + S3 WORM** | Defense in depth | Medium | Minimal overhead |

**Selected: Combination — DB role permissions + per-entry hash chain + periodic Merkle root anchored to immutable storage**

**Implementation Detail:**

```
Hash Chain Construction:

Each ledger entry in Cassandra includes:
  previous_entry_hash: SHA-256 hash of the previous entry for this account
  entry_hash: SHA-256(entry_id || account_id || transaction_id || amount || direction 
                      || currency || created_at || balance_after || previous_entry_hash)

For the first entry of an account:
  previous_entry_hash = SHA-256("GENESIS_" + account_id)

Hash computation (pseudocode):
  entry_data = concat(
    entry_id.bytes,
    account_id.bytes,
    transaction_id.bytes,
    amount.to_bytes(8, 'big'),   // 8-byte big-endian
    direction.to_bytes(1, 'big'),
    currency.encode('utf-8'),
    created_at.to_unix_ns().to_bytes(8, 'big'),
    balance_after.to_bytes(8, 'big'),
    previous_entry_hash.decode('hex')  // 32 bytes
  )
  entry_hash = SHA-256(entry_data).hex()

Verification Job (runs nightly):
1. For each account, fetch entries ordered by created_at ASC.
2. Recompute hash chain from genesis.
3. Verify: computed_hash[i] == stored_hash[i] for all i.
4. If mismatch at entry i: alert immediately (P0); log account_id and entry details.
5. Store checkpoint: hash_chain_checkpoints.cumulative_hash = final hash after N entries.

Incremental Verification:
  Verification of 1M entries at 1μs/hash = 1 second.
  At 30B entries/day, full verification would take 8.3 hours.
  Solution: checkpoint every 1M entries. Nightly verification only checks from last checkpoint.
  Checkpoints themselves are stored in S3 (WORM) — can't be modified without detection.

Merkle Tree (for batch-level proof):
  Every 100K entries: compute Merkle root over those entries.
  Store root in hash_chain_checkpoints AND write to S3 + optional blockchain anchor.
  To prove a single entry: provide the Merkle proof path (O(log N) hashes).
  Audit: give auditor the Merkle root; they can verify any entry without downloading all entries.

DB-Level Immutability:
  CREATE ROLE ledger_writer WITH LOGIN;
  GRANT INSERT ON ledger.entries TO ledger_writer;
  -- Note: no UPDATE, no DELETE granted
  REVOKE ALL ON ledger.entries FROM PUBLIC;
  
  In Cassandra: no CQL UPDATE or DELETE statements issued for ledger entries.
  Application-level: soft delete via reversal entries; never modify existing entries.
  
  S3 Object Lock (for archives):
  aws s3api put-bucket-object-lock-configuration 
    --bucket ledger-archive-prod 
    --object-lock-configuration '{"ObjectLockEnabled":"Enabled","Rule":{"DefaultRetention":{"Mode":"COMPLIANCE","Years":10}}}'
  -- COMPLIANCE mode: cannot be overridden even by root account until retention period expires.
```

**5 Interviewer Q&As:**

Q: How do you detect if a DBA modifies a ledger entry directly in Cassandra?
A: Three-layer detection: (1) The hash chain detects it — modifying entry[i] changes its hash, which breaks the hash chain from entry[i] onwards. The nightly verification job catches this. (2) Cassandra audit logging (via `AuditLogViewer`) records all CQL operations — any UPDATE or DELETE on the entries table is logged and triggers an alert. (3) The entry is immutable at the role level — the `ledger_writer` role has no UPDATE or DELETE permission. A DBA would have to use a superuser role, which is separately monitored and alert-triggers on any non-DDL use in production. Defense in depth: any single-layer bypass is caught by another layer.

Q: Why use SHA-256 instead of a faster hash like xxHash or MurmurHash?
A: For tamper detection (cryptographic security), we need a cryptographic hash function with collision resistance and preimage resistance. xxHash and MurmurHash are non-cryptographic — they're fast but can be "broken" (an attacker can craft a modified entry that produces the same hash). SHA-256 is a cryptographic hash — computationally infeasible to find two inputs with the same hash or to find an input that produces a given hash. For 30B entries/day at 1μs/SHA-256 = 30 seconds of CPU time (distributed across many nodes). The security tradeoff is completely worth it for financial records.

Q: How do you handle the hash chain if entries are inserted out of order (e.g., batch backfill)?
A: The hash chain is a total ordering, but Cassandra's `created_at` can have the same millisecond timestamp for concurrent entries. The `id` (TimeUUID v1) provides tie-breaking within the same millisecond. For backfills: (1) Backfill entries are inserted with their original `created_at` timestamps. (2) Before inserting, we must compute the correct `previous_entry_hash` for the insertion point. (3) All subsequent entries must be rehashed. (4) This makes backfills extremely expensive — which is by design. Backfills should be rare corrections, not routine operations. For large backfills, we create a new hash chain segment with a `backfill_genesis_hash` that documents the backfill event.

Q: How do you ensure the hash chain can't be silently broken in production without detection?
A: The nightly verification job is the primary detective control. To ensure it can't be silently disabled: (1) The verification job publishes a heartbeat to a separate monitoring system every night. (2) If the heartbeat stops, alert fires (dead man's switch). (3) Verification results (pass/fail + last checkpoint hash) are written to an immutable S3 location. (4) The verification job itself runs in an isolated environment with minimal blast radius — separate AWS account from production, read-only Cassandra access. (5) Quarterly, the audit team performs an independent verification using a copy of the data.

Q: What happens if the verification job finds a hash mismatch?
A: This is a P0 security incident: (1) Immediate alert to security team, engineering on-call, and CFO. (2) Isolate: identify the first mismatched entry (binary search on hash checkpoints, then linear scan within checkpoint window). (3) Cross-reference with Kafka archive (Kafka retains events for 7 days; S3 retains indefinitely). The event stream is a second copy of the truth — compare the entry in Cassandra against the Kafka-archived event. (4) Check DB audit logs for unauthorized modifications. (5) Preserve evidence for forensic analysis. (6) Regulatory notification may be required depending on the nature of the modification. (7) If entries are lost (not modified), they can be rebuilt from the event stream.

---

### 6.2 Balance Computation and Materialization

**Problem it solves:**

Computing a balance by summing all ledger entries for an account is O(N) where N can be 10+ million entries for an active account over years. This is too slow for real-time balance checks during transfers. The solution is to maintain a materialized (pre-computed) current balance updated with every entry, while retaining the ability to compute point-in-time historical balances efficiently using snapshots.

**Approaches:**

| Approach | Current Balance Read | Historical Balance | Complexity | Consistency |
|----------|---------------------|-------------------|------------|-------------|
| **Full scan of entries** | O(N) — unusable at scale | Exact | Low | Always correct |
| **Materialized balance only (no history)** | O(1) | Not possible | Low | Strong if ACID |
| **Materialized + periodic snapshots** | O(1) current; O(M) historical (M = entries since last snapshot) | Yes | Medium | Strong |
| **CQRS: separate read model** | O(1) from read DB | O(M) from snapshot | Medium | Eventually consistent |
| **Event sourcing with snapshots** | O(1) from snapshot | Full audit trail | High | Strong |

**Selected: Materialized balance in Postgres (O(1) reads) + periodic balance snapshots in Cassandra (for point-in-time) + full entry history for exact historical queries**

**Implementation Detail:**

```
Current Balance (O(1)):
  SELECT balance FROM materialized_balances 
  WHERE account_id = :id AND currency = :currency;
  -- Served from Redis cache (60s TTL) before hitting Postgres.
  -- Postgres update happens atomically with entry insertion.

Balance Update (atomic with entry insertion):
  BEGIN; -- Postgres transaction
    -- Insert idempotency record
    INSERT INTO ledger_transactions (...);
    
    -- Update materialized balance per entry
    UPDATE materialized_balances 
    SET 
      balance = balance + (:amount * :direction),  -- direction: +1 = credit, -1 = debit
      last_entry_id = :entry_id,
      last_updated_at = NOW(),
      version = version + 1
    WHERE account_id = :account_id AND currency = :currency;
    
    -- If direction = debit and account is asset type: check balance >= 0
    -- (enforced by CHECK constraint)
  COMMIT;
  
  -- After commit: write to Cassandra (async, via Kafka consumer)
  -- If Cassandra write fails, it's retried from Kafka (at-least-once)

Balance Snapshot Creation (nightly job):
  For each active account (accounts with activity in last 30 days):
    snapshot_balance = SELECT balance FROM materialized_balances WHERE account_id = :id
    snapshot_entry = SELECT id FROM cassandra_entries WHERE account_id = :id ORDER BY created_at DESC LIMIT 1
    
    INSERT INTO balance_snapshots (account_id, currency, balance, snapshot_at, entry_id_at_snapshot)
    VALUES (:id, :currency, :snapshot_balance, NOW(), :latest_entry_id)
    
  Retention: keep 1 snapshot per day for 90 days; 1 per month for 2 years; 1 per year forever.

Point-in-Time Balance Query (e.g., "what was account X's balance on Jan 1, 2025?"):
  Algorithm:
  1. Find nearest snapshot BEFORE the target date:
     SELECT * FROM balance_snapshots 
     WHERE account_id = :id AND currency = :currency AND snapshot_at <= :target_date
     ORDER BY snapshot_at DESC LIMIT 1;
     
  2. If snapshot found (within reasonable range, e.g., < 30 days before target):
     base_balance = snapshot.balance
     base_entry_time = snapshot.snapshot_at
     
     SELECT SUM(amount * direction) FROM cassandra_entries
     WHERE account_id = :id 
     AND created_at > :base_entry_time 
     AND created_at <= :target_date;
     
     result = base_balance + incremental_sum
     
  3. If no snapshot (account is old; no snapshot before target):
     SELECT SUM(amount * direction) FROM cassandra_entries
     WHERE account_id = :id AND created_at <= :target_date;
     -- Full scan; may take seconds for old accounts with millions of entries
     -- Cache the result for future queries at the same date
     
  4. Cache result in Redis: balance_at:{account_id}:{currency}:{date} with TTL = 24h.
     (Point-in-time balances are immutable — no TTL needed technically, but we expire for memory)

Performance:
  With daily snapshots, max entries to scan = 1 day's entries for an account.
  Most active accounts: 10,000 entries/day. Sum of 10K entries: < 10ms in Cassandra.
  Worst case (no snapshot, 10 million entries): ~5 seconds. Cache result on first query.
  
Multi-Currency Account Balance:
  SELECT * FROM materialized_balances WHERE account_id = :id
  -- Returns N rows (one per currency). All computed independently.
  -- FX valuation (converting to a single currency for reporting) done at query time
  --   using current FX rates; not stored in the ledger (which is multi-currency by design).
```

**5 Interviewer Q&As:**

Q: What if the materialized balance in Postgres gets out of sync with the sum of Cassandra entries?
A: This is the reconciliation problem for the balance itself. A nightly integrity check computes: `SELECT SUM(amount * direction) FROM cassandra_entries WHERE account_id = :id` and compares against `materialized_balances.balance`. If they differ by more than a rounding tolerance: alert immediately. To prevent drift in the first place: the materialized balance is updated in the same Postgres transaction that records the ledger transaction metadata — the Postgres write either succeeds for both or fails for both. The Cassandra write is async (from Kafka) and can be retried, but the materialized balance is the primary source of truth for operations. If Cassandra entries are missing (e.g., a failed write), the reconciliation detects it.

Q: Why not use Postgres for the primary ledger store instead of Cassandra?
A: At 500K entries/second, Postgres on a single node would need to handle 500K inserts/sec. A well-tuned Postgres on high-end hardware handles ~100K simple inserts/sec. We'd need 5 shards. Sharding Postgres adds operational complexity and requires application-level routing. Cassandra is designed for this: a 6-node Cassandra cluster handles 500K+ writes/sec natively, with automatic replication, self-healing, and no single point of failure. The tradeoff is that Cassandra doesn't support multi-row transactions — that's why we use Postgres for the transaction metadata (which is lower volume and requires ACID). The two databases serve complementary roles.

Q: How do you handle balance queries under high concurrency (e.g., 100 services reading the same account's balance simultaneously)?
A: Three layers: (1) Redis cache serves the vast majority of reads without touching Postgres. 100 concurrent readers hit Redis; Redis handles 1M+ ops/sec. (2) If Redis misses: one reader acquires a Redis lock (`SET lock:balance:{id} NX EX 1`), fetches from Postgres, populates cache. Others wait (< 10ms for lock + Postgres read). (3) For extremely high-read accounts (e.g., platform's revenue account, read by every transaction for fee calculation): use a local in-process cache (Guava LoadingCache) on each service instance with 5-second TTL. This reduces even Redis load by 100×. The balance is only approximate for display; the Postgres row is the authority for any write operation.

Q: How do you implement the trial balance report efficiently at scale?
A: The trial balance is the sum of all account balances, grouped by account type. With 10M accounts: (1) Pre-aggregate: store per-account-type sum in a separate materialized table updated by the Balance Materializer Kafka consumer. (2) OR use ClickHouse: `SELECT account_type, SUM(balance) FROM materialized_balances GROUP BY account_type` — ClickHouse can aggregate 10M rows in < 1 second. (3) Period-close trial balance (historical): use the balance snapshot table joined with incremental entries. ClickHouse handles this well for reporting purposes. The trial balance report runs nightly; it's not real-time.

Q: What's the best approach for real-time balance streaming (e.g., a dashboard that shows live balance updates)?
A: Server-Sent Events (SSE) or WebSocket from the Balance Materializer service. When a Kafka consumer processes a balance update for account X, it publishes to a Redis Pub/Sub channel `balance_updates:{account_id}`. A Balance Stream Service subscribes to relevant channels and pushes updates to connected WebSocket clients. For a merchant dashboard: the client subscribes to their account's channel on login. Each balance update arrives within < 100ms of the Kafka event. This is push-based, eliminating polling. At 1M concurrent connected merchants, we need ~100 WebSocket server pods (10K connections/pod). Connection state is stored in Redis (which pod handles which account), enabling horizontal scaling.

---

### 6.3 Reconciliation Engine

**Problem it solves:**

A payment processor receives money from card networks and pays out to merchants. The internal ledger tracks what the system believes happened. The external systems (Visa/MC settlement files, bank statements) track what actually happened at the network/bank level. These must match. Reconciliation is the process of comparing them, detecting discrepancies, and resolving them. Unreconciled discrepancies represent either fraud, bugs, or missing entries — all of which cost money.

**Approaches:**

| Approach | Automation Level | Accuracy | Scalability |
|----------|----------------|----------|-------------|
| **Manual spreadsheet comparison** | None | Error-prone | Breaks above 10K entries |
| **Simple 1:1 matching by reference** | High for matched entries | Misses complex scenarios | Scales linearly |
| **Multi-field fuzzy matching** | Medium (human review for fuzzy) | High | Scales; O(N log N) |
| **ML-based matching (for ambiguous cases)** | Very high | Highest | Scales |
| **Streaming reconciliation (real-time)** | Very high | High | Scales; prevents backlog |
| **Batch reconciliation (daily)** | High | High | Standard for financial industry |

**Selected: Daily batch reconciliation with multi-field exact matching + ML-assisted fuzzy matching for exceptions**

**Implementation Detail:**

```
Reconciliation Input Sources:
  - Visa: daily clearing file (ISO 8583 TC05) delivered via SFTP
  - Mastercard: daily clearing file (MCBS format) via SFTP
  - Adyen (acquirer): daily settlement report API
  - Bank: MT940 SWIFT statement (daily)
  - ACH: daily NACHA return file

Reconciliation Algorithm:

STEP 1: Ingest External File
  Parse clearing file → normalize into reconciliation_items table:
    { external_reference, transaction_date, amount, currency, card_brand, merchant_id, ... }

STEP 2: Match Against Internal Ledger
  Query ClickHouse for internal entries matching the period:
    SELECT source_reference, amount, currency, account_id, created_at
    FROM ledger_entries
    WHERE entry_type IN ('charge_settled', 'refund_settled')
    AND created_at BETWEEN :start AND :end

  Matching algorithm (priority order):
  
  Round 1: Exact match on (external_reference, amount, currency)
    Match rate typically: 97-99%
    
  Round 2: Fuzzy match on (amount, currency, date ± 1 day, merchant_id)
    For entries unmatched after Round 1.
    Match rate: additional 0.5-1%
    
  Round 3: ML classifier on remaining unmatched entries
    Features: amount similarity, date proximity, merchant similarity, card brand
    Output: suggested match pairs with confidence score
    High confidence (> 0.95): auto-match + flag for human review
    Low confidence (< 0.95): exception for human review
    
  Remaining unmatched after Round 3: → reconciliation_exceptions

STEP 3: Classify Exceptions
  missing_in_external: Internal entry exists; not in external file.
    Possible cause: Timing issue (next day's file), submission failure.
    Action: Check next day's file; if still missing after 3 days, re-submit.
    
  missing_in_internal: External entry exists; not in internal ledger.
    Possible cause: Lost Kafka message, missed write.
    Action: CRITICAL. Investigate immediately. May need to post backdated entry.
    
  amount_mismatch: Same reference, different amounts.
    Possible cause: FX rounding, fee computation error, partial settlement.
    Action: Calculate delta; if > $0.01, flag for human review; if <= $0.01, auto-accept (rounding).
    
  duplicate_in_external: Same reference appears twice in external file.
    Possible cause: Network/acquirer duplicate submission.
    Action: Flag for acquirer; do not create duplicate internal entry.

STEP 4: Auto-Resolve Simple Exceptions
  - Timing exceptions: Mark as "pending next day"; check in tomorrow's run.
  - Rounding exceptions (< $0.01): Auto-resolve with memo.
  - Duplicate externals: Flag to acquirer; auto-close on acquirer acknowledgment.
  
STEP 5: Human Review Queue
  Finance Ops reviews open exceptions in dashboard.
  Resolution options: match manually, post correction entry, escalate to acquirer.
  
STEP 6: Generate Exception Report
  Daily email + dashboard update:
  "Visa: 9,852,134 matched, 47 exceptions (35 timing, 8 amount mismatch, 4 missing)"
  SLA: All exceptions resolved within 3 business days.
  
Performance:
  10M entries/day × 2 sources = 20M rows to compare.
  Hash-based matching: build hash map of internal entries by reference key.
  Lookup each external entry in O(1). Total: O(N) = ~10 seconds for 20M entries.
  ClickHouse range query for internal entries: < 30 seconds.
  Full reconciliation run: < 5 minutes for 10M entries.
  At 100M entries/day: scale horizontally — partition by card_brand + date; run in parallel.
```

**5 Interviewer Q&As:**

Q: What's the business cost of an unreconciled exception?
A: Depends on exception type. Missing in external (we think we settled but network doesn't): we may have credited a merchant who wasn't actually paid by the network — loss equals the transaction amount. Missing in internal (network settled but we didn't record): we may have received funds without crediting the merchant — liability and potential fraud. Amount mismatch: difference is the loss. For a 99% match rate at 10M transactions/day with $50 average transaction: 100K unmatched × $50 = $5M/day at risk. Strict SLAs (< 0.01% exceptions, resolved within 3 days) are critical.

Q: How do you handle reconciliation across time zones and settlement windows?
A: Card networks settle on a cut-off time (Visa: 10:00 PM ET for US). Transactions after cut-off appear in the next day's file. Our reconciliation uses the network's date (from the clearing file), not the wall-clock date. We store both `transaction_date` (from network file) and `created_at` (our timestamp) on every entry. Reconciliation queries use the network's date for matching. Cross-border transactions: a transaction initiated in AEST (GMT+10) at 11 PM appears in the network's next-day file for the US — we handle this with a ±1 day date tolerance in Round 2 matching.

Q: How do you reconcile when the external file format changes (e.g., Visa changes their clearing format)?
A: File format abstraction layer: each external source has a dedicated parser (Visa parser, MC parser, Adyen parser). When Visa changes their format, we update only the Visa parser. The parser outputs a normalized `reconciliation_items` schema used by the matching engine. Visa provides advance notice (typically 6 months) of format changes. We have a parser test suite with sample files from each format version. Schema version tracking: each parser records the format version it detected, enabling debugging if a parser fails on an unexpected format.

Q: How do you prove to an auditor that your reconciliation is complete and accurate?
A: Audit evidence package: (1) Reconciliation run records in the database show every run, its status, match counts, and exception counts. (2) Exception records show all discrepancies and how they were resolved (auto vs. human, resolution notes). (3) The ledger hash chain provides cryptographic proof that no entries were modified post-reconciliation. (4) External files are archived in S3 (WORM) — the auditor can download the original Visa clearing file and independently re-run the matching. (5) Reconciliation reports are digitally signed and stored immutably. Auditors can trace any transaction from the external file to the internal ledger entry to the original payment API call.

Q: How would you design real-time reconciliation instead of daily batch?
A: Real-time reconciliation requires the external system to provide real-time settlement notifications (some acquirers offer webhooks for settlement events). Architecture: (1) Acquirer webhooks → Kafka topic `external.settlement.events`. (2) Reconciliation consumer matches each event against internal ledger in real-time (< 100ms per event). (3) Immediate exception creation for mismatches. (4) Dashboard shows live reconciliation status. Challenges: networks still batch-settle (Visa settles once/day), so true real-time is only possible with real-time settlement systems (Visa Direct, Mastercard Send, RTP). For most card transactions, daily batch reconciliation is the industry standard.

---

## 7. Scaling

**Horizontal Scaling:**

- **Ledger API Service**: Stateless; 50 pods handling 500K writes/sec (10K/pod). Autoscale based on request queue depth.
- **Cassandra Primary Store**: Start with 12 nodes (4 per AZ, 3 AZs). At 500K writes/sec with `LOCAL_QUORUM` (2-of-3 AZ nodes): each node handles ~42K writes/sec. Cassandra nodes handle 50-100K writes/sec each. Scale to 24 nodes for headroom.
- **PostgreSQL (metadata + materialized balances)**: Write load is lower (one row per transaction, not per entry). 8 shard nodes. Each shard handles ~12.5K TPS (far below Postgres limits of ~50K TPS).
- **ClickHouse (OLAP)**: 4-node cluster for analytics; scale horizontally by adding shards.
- **Reconciliation Engine**: Stateless batch workers; spin up for each nightly run; auto-terminate after completion.

**Cassandra Scaling Specifics:**

Partition key: `account_id` (UUID). Natural distribution via consistent hashing. Hotspot risk: large merchants or system accounts (e.g., the platform's main cash account) receive all transactions. Mitigate: for system accounts with extreme write rates, use a "synthetic partition key" = `account_id + shard_suffix` where `shard_suffix = entry_id % 16` (16 sub-partitions per account). Balance reads aggregate across sub-partitions.

**DB Sharding (PostgreSQL):**

Shard `ledger_transactions` and `materialized_balances` by `account_id`. 8 shards initially. Shard mapping: `shard_id = hash(account_id) % 8`. Virtual shards (256 virtual → 8 physical) for zero-downtime resharding.

**Caching:**

| Data | Cache | TTL | Notes |
|------|-------|-----|-------|
| Current balance | Redis | 60s | Primary read path; invalidated on write |
| Point-in-time balance | Redis | 24h | Historical; doesn't change |
| Account metadata | Redis | 5m | Account type, currency, merchant_id |
| Transaction fetch | Redis | 60s | Individual transaction object |
| Reconciliation run status | Redis | 5m | Poll status for running jobs |

**Kafka for Throughput:**

500 partitions for `ledger.entries` topic. Partition key: `account_id`. Ensures ordering per account. At 350K entries/sec: each partition handles 700 entries/sec. Kafka broker cluster: 30 nodes (3 per AZ × 10 AZs-equivalents). Replication factor 3. `acks=all` for durability.

**Interviewer Q&As:**

Q: How do you handle a "hot partition" in Cassandra where one account generates millions of entries/hour?
A: Synthetic partition key: instead of partitioning solely on `account_id`, use `(account_id, bucket)` where `bucket = entry_timestamp_epoch / 3600` (hourly buckets). For the platform's main revenue account: each hour's entries go to a separate partition. Balance queries must aggregate across all buckets. Trade-off: range queries become more complex (query each bucket separately). For extreme accounts (platform's global revenue account that receives a fee on every transaction): use materialized balance exclusively and never query the partition directly.

Q: At 100 billion entries, how do you maintain query performance for account statements?
A: Cassandra is designed for this. With partition key `account_id` and clustering key `created_at DESC`, a query for "last 30 days of entries for account X" is: `SELECT * FROM entries WHERE account_id = :id AND created_at >= :start ORDER BY created_at DESC LIMIT :n`. Cassandra reads directly from the head of the partition (LSM-tree newest-first access pattern). Performance is O(result_set_size), not O(total_partition_size). For 10M entries in a partition and a 1000-row result: < 10ms. The challenge is old data compaction: `TimeWindowCompactionStrategy` ensures entries from different time periods are in separate SSTables — older SSTables can be migrated to cheaper storage without affecting new-entry performance.

Q: How do you scale the nightly reconciliation when the volume reaches 100M transactions/day?
A: Parallel reconciliation: partition the work by card_brand + date_shard. Run 24 parallel reconciliation jobs (one per hour of the day × card brands). Each job processes ~4M transactions. Using ClickHouse for both internal and external data: a distributed query across 4 ClickHouse shards processes 100M rows in ~2 minutes. Kubernetes Jobs with auto-scaling: spin up 24 pods at midnight, run in parallel, aggregate results. Total elapsed time: ~5 minutes for 100M transactions. Exception matching uses a distributed hash join in ClickHouse (not in-memory in a single Python process).

Q: How do you back up 22PB of Cassandra data?
A: Three-tier backup: (1) **In-cluster replication** (RF=3): protects against node failures and is always current. Not a backup — node failures cascade if data is corrupted before propagation. (2) **SSTable snapshots**: Cassandra `nodetool snapshot` creates point-in-time snapshots of SSTables. Full snapshot weekly; incremental (only changed SSTables) daily. Snapshots transferred to S3 via Cassandra's backup tools (cassandra-medusa, DataStax DSE). At 22PB: weekly full backup ~22PB transfer (compressed: ~3-4PB), daily incremental ~60-200GB. (3) **Kafka as second source of truth**: Kafka retains all entry events for 7 days (configurable to longer). For recent data, entries can be rebuilt from Kafka if Cassandra is corrupted.

Q: How do you test that the hash chain integrity verification actually works?
A: Test-driven approach: (1) Unit tests: inject a known entry modification; verify the hash chain validator detects it. (2) Integration tests: in a staging environment, directly modify a Cassandra entry using a superuser role; run the verification job; assert it alerts. (3) Chaos tests: randomly corrupt 1 entry in 10M; verify detection. (4) Production drill (quarterly): in a dedicated test partition (with synthetic test accounts), inject a deliberate corruption; verify the production verification job catches it without needing to know which entry was modified. This is "break-glass testing" for the most critical monitoring control.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Cassandra node failure | Reduced write availability (RF=3; can survive 1 failure) | Node health check | Cassandra auto-repairs; `LOCAL_QUORUM` still succeeds with 2/3 nodes |
| Cassandra datacenter failure | Writes fail if all local nodes down | DC health metric | Fail over to multi-DC replication; `EACH_QUORUM` for cross-DC writes |
| Postgres primary failure (materialized balances) | Balance updates paused | Health check | Patroni auto-failover; < 30s; RPO=0 with sync standby |
| Redis failure | Balance reads miss cache; fall to Postgres | Redis ping failure | Serve from Postgres; no data loss; latency degrades |
| Kafka broker failure | Event consumers pause | Under-replicated partitions | Kafka auto-reassigns; 3 replicas per partition; < 30s recovery |
| Unbalanced entry posted | Double-entry invariant violated | `CHECK (total_debit = total_credit)` fails | Reject at Postgres write time; return error to caller |
| Hash chain corruption detected | Tamper evidence | Nightly verification job | P0 security incident; investigate; rebuild from Kafka archive |
| Reconciliation exception > 1% | Financial loss risk | Daily exception rate metric | Alert Finance Ops; escalate to acquirer; manual resolution |
| Write-ahead log (WAL) full | Postgres stops accepting writes | WAL size metric | Increase WAL size; investigate slow replica; emergency WAL pruning |
| Network partition (split-brain) | Two primaries might accept writes | Fencing tokens; epoch numbers | Single Postgres primary via Patroni (only leader with valid lease writes) |

**Exactly-Once Entry Posting:**

Every `POST /v1/ledger/transactions` call requires an `Idempotency-Key`. The key is stored in `ledger_transactions` with a UNIQUE constraint on `(source_system, idempotency_key)`. If a payment processor retries after a timeout (before receiving a response), the second call: (1) Hits the UNIQUE constraint, (2) Returns the original transaction response from the stored record. The entries are NOT posted again. This guarantees exactly-once semantics for entry posting.

**Saga-based Compensation:**

When a downstream failure requires reversing a ledger entry: instead of deleting the entry, post a reversal transaction (equal-and-opposite entries with `reversal_of = :original_transaction_id`). The audit trail shows both the original posting and the reversal. Balance is correct; history is complete.

---

## 9. Monitoring & Observability

| Metric | Type | SLO / Alert | Purpose |
|--------|------|------------|---------|
| `ledger.write.success_rate` | Gauge | < 99.99% → P0 | Core durability |
| `ledger.write.p99_latency` | Histogram | > 50ms → P1 | Write performance |
| `ledger.balance_read.p99_latency` | Histogram | > 10ms → P1 | Read SLO |
| `ledger.double_entry_violations` | Counter | > 0 → P0 | Fundamental invariant |
| `ledger.hash_chain.last_verification_passed` | Gauge | FALSE → P0 | Tamper detection |
| `ledger.hash_chain.last_verification_timestamp` | Gauge | > 25h → P0 | Dead-man's switch |
| `ledger.balance_drift_count` | Counter | > 0 → P0 | Materialized balance mismatch |
| `reconciliation.exception_rate` | Gauge | > 0.1% → P1 | Financial accuracy |
| `reconciliation.unresolved_exceptions_7d` | Gauge | > 0 → P1 | SLA violation |
| `cassandra.pending_compactions` | Gauge | > 100 → P2 | Storage health |
| `cassandra.replication_lag_ms` | Histogram | > 5000ms → P1 | Consistency |
| `kafka.consumer_lag{group=balance_materializer}` | Gauge | > 100K → P1 | Balance update backlog |
| `ledger.entries_per_second` | Counter | < expected → investigate | Traffic anomaly |
| `ledger.cold_archive_delay_hours` | Gauge | > 26h → P2 | Archive freshness |

**Distributed Tracing:**

Every `POST /v1/ledger/transactions` generates a trace spanning: Ledger API → Write Coordinator → Cassandra write → Postgres write → Kafka publish. Span annotations: `account_ids`, `amount`, `entry_types`, `is_balanced`. 100% sampling for all writes (financial operations warrant full observability). 30-day trace retention.

**Logging:**

Structured JSON. Mandatory fields: `trace_id`, `transaction_id`, `account_ids`, `amount`, `currency`, `operation`, `duration_ms`. Sensitive data (account owner PII) not in logs — only account IDs. Financial logs: separate log stream with 10-year retention (regulatory). Application logs: 90 days.

**Alerting Philosophy:**

- `balance_drift` and `double_entry_violations` and `hash_chain.last_verification_passed = FALSE`: **P0 — wake up the on-call engineer immediately**. These indicate financial integrity failures.
- `reconciliation.unresolved_exceptions_7d > 0`: **P1 — business hours escalation to Finance Ops**.
- Performance degradation: **P2 — 15-minute response time**.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen Approach | Trade-off / Alternative |
|----------|----------------|------------------------|
| Primary ledger store | Cassandra (append-optimized, horizontally scalable) | Postgres is simpler but doesn't scale to 500K inserts/sec without many shards |
| Balance storage | Materialized in Postgres (O(1) read) | Pure event sourcing (recompute every time) is perfectly accurate but too slow |
| Point-in-time balance | Snapshot + incremental scan | Full scan always is simplest but O(N) where N can be millions |
| Hash chain | Per-entry SHA-256 chain | Merkle tree is more space-efficient but higher complexity for audit proofs |
| Double-entry enforcement | Postgres CHECK constraint (strongest) | Application-only validation is simpler but single point of failure |
| Reconciliation approach | Daily batch (industry standard) | Real-time possible with streaming acquirers but most networks still batch-settle |
| Cold archive | S3 Parquet with Object Lock | Cassandra cold storage (cheaper to query) but more expensive storage cost per GB |
| Idempotency | `UNIQUE (source_system, idempotency_key)` in Postgres | Redis-only is faster but not durable enough for financial deduplication |
| Reversal mechanism | Equal-and-opposite entries (saga compensation) | DELETE + re-insert simpler but violates immutability |
| OLAP for reports | ClickHouse | Postgres works for small datasets; breaks at 100B+ rows for analytical queries |

---

## 11. Follow-up Interview Questions

Q1: How do you design the ledger for a system that supports multiple legal entities (subsidiaries)?
A: Extend the account hierarchy. Each legal entity gets its own sub-ledger (set of accounts): `entity_id` column on `accounts` table. Inter-entity transactions use "due from" and "due to" accounts (intercompany accounts receivable/payable). Consolidation: a nightly job sums all entity sub-ledgers, eliminates intercompany balances (which net to zero across entities), and produces a consolidated balance sheet. This is called "consolidation" in multi-entity accounting. Technically: the ledger is the same system; the entity filter is applied at query time.

Q2: How do you implement the ledger for GAAP accrual accounting (as opposed to cash-basis)?
A: Accrual accounting recognizes revenue when earned, not when cash is received. For a subscription billed monthly but earned daily: (1) On billing: DR Accounts Receivable, CR Deferred Revenue (liability). (2) Daily: DR Deferred Revenue, CR Revenue (recognize 1/30th per day). This requires a scheduled journal entry job. Implementation: `scheduled_entries` table with `entry_template`, `recurrence_rule`, `start_date`, `end_date`. A nightly job posts the scheduled entries. Standard accrual: prepaid expenses (DR Asset, CR Cash on payment; DR Expense, CR Asset monthly). The double-entry model supports this natively; it's an application-layer concern.

Q3: How do you handle the ledger during a system migration (e.g., migrating from old payment system to this one)?
A: Migration strategy: (1) Historical balances: compute the balance as of migration date from the old system; post an "opening balance" entry to the new ledger with `entry_type = 'opening_balance'` and `source_reference = 'migration_2026'`. (2) Historical entries: optionally migrate historical entries from old system — requires mapping old entry types to new types. (3) Cutover: on migration day, old system stops posting; new system starts. Any in-flight transactions from old system are completed there and the final balance imported. (4) Reconciliation: run reconciliation of new ledger against old system's final state to verify balance transfer was accurate.

Q4: How do you design the ledger to support chargebacks that happened 120 days after the original transaction?
A: A chargeback reverses a settled transaction. The original settled entry is NOT modified (immutability). Instead: (1) Post a reversal transaction: DR Merchant Payable (debit what we owe merchant), CR Cash (credit: take money back from merchant's balance), CR Chargeback Reserve (credit: fund the chargeback). (2) The reversal `source_event_id = chargeback_id`. (3) If the chargeback is won (merchant wins evidence dispute): post a second reversal of the reversal — net effect: no change from original. (4) This creates a complete audit trail: original settlement → chargeback reversal → optional reversal-of-reversal. All entries preserve the date they were posted, not the original transaction date.

Q5: How do you design aggregate balance reports for period-close (monthly, quarterly, annual)?
A: Period-close is a formal accounting process where the books are "closed" for a period. (1) On the last day of the period: run all scheduled accruals, post all adjusting entries (provisions, write-offs). (2) Create a `period_close_snapshot` — materialize the balance of every account as of the last moment of the period. This snapshot is immutable once the period is formally closed. (3) Store the snapshot with a `closed` flag — no new entries should reference this period after close. (4) Generate reports from the snapshot: trial balance, P&L (revenue - expense accounts), balance sheet (asset - liability - equity accounts). ClickHouse handles the aggregation efficiently.

Q6: How do you implement automated GL (General Ledger) account mapping for different transaction types?
A: A mapping configuration layer: a `posting_rules` table that maps `source_event_type` + `payment_method` + `card_type` + `country` → `debit_account_code + credit_account_code`. For example: `charge.captured + credit_card + US` → `{debit: '1000-Cash', credit: '2000-Merchant-{merchant_id}', fee_debit: '4000-Interchange', fee_credit: '3000-Fee-Revenue'}`. The Ledger API Service looks up the posting rule and generates the correct entries. New transaction types or account structures require only a configuration change, not code changes.

Q7: How does the ledger handle foreign currency revaluation at period end?
A: FX revaluation adjusts the carrying value of foreign currency balances to current exchange rates. At period end: (1) Identify all non-functional-currency balances (e.g., EUR balances in a USD-functional company). (2) Compute FX gain/loss: `(current_rate - entry_rate) × balance_amount`. (3) Post an adjusting entry: DR/CR the balance account for the FX adjustment, offsetting entry to `FX Gain/Loss` (an income account). (4) The adjustment is reversed on day 1 of the next period (so it doesn't affect that period's operational P&L). This is a standard accounting procedure — the ledger supports it with standard double-entry postings.

Q8: How do you design the ledger for high-frequency trading or micro-transaction scenarios (millions of tiny transactions)?
A: Micro-transactions create enormous entry volumes. Optimizations: (1) **Batch aggregation**: instead of one entry per micro-transaction, batch entries within a time window (e.g., aggregate all micro-transactions in a 1-second window per merchant into a single entry). Less granular but much lower volume. (2) **In-memory aggregation buffer**: accumulate entries in Redis for 1 second, then flush as a batch entry. Trade-off: lose per-transaction granularity; gain performance. (3) **Tiered detail**: store micro-transaction detail in a separate analytics store (ClickHouse); the primary ledger stores only aggregated entries. Reconciliation uses the ClickHouse detail against the aggregated ledger entries.

Q9: How do you ensure the ledger is correct even if the Kafka consumer fails to deliver a balance update to Redis?
A: The balance cache is a convenience cache — not the source of truth. The source of truth is the `materialized_balances` table in Postgres, which is updated atomically in the same transaction as posting the ledger entries. If the Kafka consumer fails and Redis is stale: (1) Balance reads serve the stale Redis value for up to 60s (TTL). (2) After TTL expiry, the next read falls through to Postgres, which has the accurate balance. (3) Write operations (transfers) always check Postgres, not Redis. So a stale cache only affects display accuracy for up to 60 seconds, never affects the correctness of a balance check before a transfer.

Q10: How do you implement ledger entries for a fee that's a percentage of a transaction (e.g., 2.9% + $0.30)?
A: The fee calculation happens before posting. The posting rule defines the formula: `fee = round(amount * 0.029 + 30)` (always in minor units, no floating point). The Ledger API Service computes the fee and posts a balanced 4-entry transaction: (1) DR Cash (total charge amount), (2) CR Merchant Payable (amount minus fee = net), (3) DR Processing Expense (interchange cost we pay to card network), (4) CR Fee Revenue (our processing fee). The entries always balance (total debit = total credit). Fee computation uses integer arithmetic only — `fee = (amount * 290) / 10000 + 30` (multiply before divide to avoid truncation error).

Q11: How do you handle a bug that posted incorrect entries to the ledger?
A: The correction procedure: (1) Identify all incorrect entries by `source_event_id` or `transaction_id`. (2) Post reversal transactions (equal-and-opposite) for each incorrect transaction. (3) Post correct replacement transactions with the correct amounts. (4) Document in an incident report: which entries, why they were wrong, what the correction was. (5) The audit trail shows: original (wrong) entries, reversals, and correct entries. Auditors can trace the full history. (6) If the error resulted in incorrect merchant payouts (too much or too little), initiate recovery (reclaim overpayments, post additional credits for underpayments). (7) Post-incident: fix the bug that caused incorrect entries, add tests.

Q12: How does the ledger integrate with an accounts payable system (paying suppliers)?
A: Accounts payable integration: when a payout to a bank is initiated, post a ledger entry: DR Merchant Payable (reduce what we owe merchant), CR Accounts Payable (or directly CR Cash if wire). The AP system triggers the actual bank transfer. On bank confirmation: DR Accounts Payable, CR Cash. This two-step process separates the accounting decision (reduce liability) from the cash movement (actual bank transfer). If the bank transfer fails: DR Cash, CR Merchant Payable (reverse). The ledger entry is always the first step; the actual bank movement confirms it.

Q13: How would you design the ledger to support reversals with partial amounts (e.g., a partial refund)?
A: Partial reversals post only the partial amount as equal-and-opposite entries, not the full original transaction. Implementation: (1) The reversal transaction references `original_transaction_id` and includes `reversal_type = 'partial'` and `partial_amount`. (2) Entries: DR Merchant Payable (partial amount), CR Cardholder Refund Receivable, DR Refund Expense, CR Cash. (3) The partial amount is validated to not exceed the original transaction's uncancelled amount. Running balance: `materialized_balances` reflects the net position after all partial reversals. (4) Full reversal available only if sum of partial reversals = original amount.

Q14: How do you audit the accounts of a specific merchant without giving the auditor access to all merchants' data?
A: Row-level security (RLS) in Postgres: `CREATE POLICY merchant_isolation ON materialized_balances FOR SELECT USING (merchant_id = current_setting('app.merchant_id'));`. Auditor logs in with a role bound to the specific merchant. They can query any table but only see rows for their merchant. For the Cassandra read path: a middleware proxy enforces `account_id IN (SELECT id FROM accounts WHERE merchant_id = :auditor_merchant)`. The auditor gets a full view of their sub-ledger but cannot see other merchants. For internal auditors reviewing all merchants: a separate privileged role with full access and an audit trail of the auditor's own queries.

Q15: What is TigerBeetle and would it replace this design?
A: TigerBeetle is a purpose-built financial database designed for the exact use case of a high-throughput double-entry ledger. Key properties: (1) Written in Zig; extremely high throughput (1M+ transfers/second on a single node). (2) Enforces double-entry invariant natively — every transfer atomically debits one account and credits another. (3) Append-only, crash-safe. (4) Very limited API (transfers and accounts only; no SQL). TigerBeetle would replace the Cassandra + Postgres ledger layer with a single, purpose-built system. Tradeoffs: (1) Limited ecosystem and operational tooling compared to Cassandra/Postgres. (2) No complex queries — you must build aggregation layers on top. (3) Excellent fit for the write path; the read/reporting path still requires ClickHouse or equivalent. (4) As of 2026, TigerBeetle is production-grade but has less battle-testing than Cassandra at petabyte scale. Worth evaluating for greenfield financial systems.

---

## 12. References & Further Reading

- **TigerBeetle** (purpose-built financial ledger database): https://github.com/tigerbeetle/tigerbeetle
- **"Accounting Systems Design"** — Martin Kleppmann, VLDB 2019 (on double-entry in distributed systems): https://martin.kleppmann.com/2011/03/07/accounting-for-computer-scientists.html
- **Apache Cassandra Documentation** (TimeWindowCompactionStrategy, partition design): https://cassandra.apache.org/doc/latest/
- **ScyllaDB** (Cassandra-compatible, higher throughput): https://docs.scylladb.com/
- **ClickHouse Documentation** (columnar OLAP for financial aggregations): https://clickhouse.com/docs/
- **"Designing Stripe's Financial Infrastructure"** — Stripe Engineering: https://stripe.com/blog/ledger-stripe-system-for-tracking-and-validating-money-movement
- **Double-Entry Bookkeeping** — Wikipedia (for accounting fundamentals): https://en.wikipedia.org/wiki/Double-entry_bookkeeping
- **"The Practitioner's Guide to Financial Data Management"** — ACCA: https://www.accaglobal.com/
- **SOX Compliance Requirements** (public company financial record integrity): https://pcaobus.org/
- **AWS S3 Object Lock** (WORM compliance for financial records): https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-lock.html
- **NACHA Operating Rules** (for ACH reconciliation): https://www.nacha.org/rules
- **"Kafka: The Definitive Guide"** — Narkhede, Shapira, Palino (O'Reilly, 2021) — for event streaming architecture
- **Visa Clearing and Settlement** (TC05 file format, settlement windows): https://developer.visa.com/capabilities/visa_clearing
- **"Building Event-Driven Microservices"** — Adam Bellemare (O'Reilly, 2020) — for event sourcing and CQRS patterns
- **Google Cloud Spanner** (alternative to Cassandra for globally distributed ACID ledger): https://cloud.google.com/spanner/docs
