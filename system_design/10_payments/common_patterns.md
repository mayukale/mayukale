# Common Patterns — Payments (10_payments)

## Common Components

### PostgreSQL (ACID Source of Truth for Financial Data)
- All four problems use PostgreSQL as the strongly-consistent, ACID-compliant store for critical financial records
- In payment_processing: payments, customers, merchants, idempotency_keys; sharded by merchant_id (1,024 virtual shards); sync replication to 1 standby (RPO=0); async to 2 read replicas; PgBouncer for 10K connection pooling per shard
- In stripe_gateway: charges, customers, merchants, payment_intents; sharded by merchant_id (2,048 virtual shards); JSONB for flexible metadata; optimistic lock via version column
- In wallet: accounts, account_balances, transfers, ledger_entries; sharded by account_id (512 virtual shards); CHECK constraint `balance >= 0`, `reserved <= balance`
- In ledger: materialized_balances, ledger_transactions (with `CHECK (total_debit = total_credit)` generated column), reconciliation_exceptions; UNIQUE (source_system, idempotency_key) for dedup

### Idempotency via Redis SETNX + PostgreSQL Fallback
- All four implement idempotency using a two-layer approach: Redis SETNX for fast in-flight dedup + PostgreSQL UNIQUE constraint as durable safety net
- In payment_processing: SHA-256(request_body) → `SETNX idempotency:{merchant_id}:{key}` with 24 h TTL; if Redis fails → PostgreSQL `SELECT FOR UPDATE` on `idempotency_keys` table (serializes concurrent requests)
- In stripe_gateway: Redis SETNX (1 h TTL for payment method cache) + PostgreSQL idempotency_keys table; UNIQUE (merchant_id, idempotency_key)
- In wallet: idempotency key enforced on transfers (TTL 24 h); prevents double-transfer on network retry
- In ledger: UNIQUE (source_system, idempotency_key) in PostgreSQL; at-most-once transaction posting guaranteed even on retry

### Cassandra for Append-Only Audit Log / Time-Series
- All four use Cassandra for high-write, time-ordered append-only records that must never be updated; RF=3, LOCAL_QUORUM consistency
- In payment_processing: `audit_log` table; partition by payment_id (TimeUUID); clustering by created_at; append-only events (AUTHORIZED, CAPTURED, REFUNDED, etc.); compaction via STCS
- In stripe_gateway: webhook_delivery_log + audit_trail; partition by merchant_id; clustering by (created_at, id)
- In wallet: `audit_log` table + `transaction_history` read model; partition by account_id; clustering by (created_at DESC, id); 7-year retention; RF=3
- In ledger: PRIMARY ledger store; partition by account_id; clustering by (created_at DESC, id); TTL=0 (permanent, no auto-delete); TimeWindowCompactionStrategy (TWCS, 30-day window); 350K writes/sec; never updated or deleted

### Kafka for Event Streaming + Transactional Outbox
- All four use Kafka with the transactional outbox pattern: write to PostgreSQL + outbox row atomically; Debezium (or poller) reads WAL → publishes to Kafka with at-least-once delivery
- In payment_processing: `payment-events` topic (48 partitions by payment_id); `webhook-events`; RF=3; Webhook Dispatcher consumes and delivers with retry (1s→4s→16s→exp, max 3 days)
- In stripe_gateway: `webhook-events` (96 partitions by merchant_id); outbox table in PostgreSQL; Debezium CDC reads WAL; retry schedule 10 attempts over 72 h; HMAC-SHA256 signature on each delivery
- In wallet: `transfer.completed` events; `aml-alerts`; `notification-triggers`; RF=3; 50 notification workers
- In ledger: 500 partitions (high-parallelism for 350K entries/sec); decouples write path from Balance Materializer, ClickHouse ingest, and Reconciliation Engine consumers

### Redis for Idempotency Cache, Balance Cache, Rate Limiting
- All four use Redis for low-latency caching of hot financial data with short TTLs; never the authoritative store
- In payment_processing: idempotency SETNX (24 h TTL), rate limit counters (1,000 req/min per merchant)
- In stripe_gateway: idempotency + token/payment_method cache (1 h TTL), rate limit counters, fraud feature vectors
- In wallet: `balance:{account_id}:{currency}` String (60 s TTL, written after Postgres COMMIT); AML velocity features (pre-computed by ClickHouse, 60 s TTL); session JWT (15 min TTL); 2FA state
- In ledger: materialized balance cache (60 s TTL); written by Balance Materializer after each Kafka event processed; balance reads at 3.5M RPS served from Redis (not Cassandra)

### Optimistic Locking via version Column
- Three of the four prevent concurrent modification races with a version column checked in the WHERE clause (no SELECT FOR UPDATE, no distributed lock)
- In payment_processing: `UPDATE payments SET captured_amount = captured_amount + :amount, version = version + 1 WHERE id = :id AND version = :expected_version`; 0 rows → retry
- In stripe_gateway: identical version column pattern on charges and payment_intents
- In wallet: `UPDATE account_balances SET balance = balance - :amount, version = version + 1 WHERE account_id = :id AND balance >= :amount AND version = :expected_version`; `balance >= :amount` in WHERE clause = atomic NSF check (no separate SELECT needed)
- In ledger: uses Cassandra append-only (no UPDATE, no version needed) + CHECK constraint at PostgreSQL materialized balance layer

### ClickHouse for OLAP, Fraud Features, and AML Velocity
- Three of the four use ClickHouse as a column-store for fast aggregations over billions of rows
- In stripe_gateway: fraud feature store (merchant analytics, chargeback patterns); 100× faster aggregations than PostgreSQL
- In wallet: AML velocity feature store — pre-computes `total_sent_last_1h`, `total_sent_last_24h`, `transfer_count_last_1h`, `unique_recipients_last_7d`, `avg_transfer_last_30d`; updated every 60 s from ClickHouse materialized view; read at <2 ms by AML Service via Redis
- In ledger: OLAP reporting (P&L, balance sheet, period-close); batch insert from Kafka at 1-second micro-batches; 10K events/sec ingest rate

### Circuit Breakers on External Services (Acquirers, AML, FX)
- All four implement circuit breakers on external service calls to prevent cascading failures
- In payment_processing: per-acquirer circuit breaker (Visa, Mastercard, Amex, Stripe, Adyen); OPEN after 20% error rate in 60 s window; HALF_OPEN after 30 s; allow 10% probe traffic; failover to secondary acquirer
- In stripe_gateway: identical circuit breaker thresholds; fraud engine timeout → fail-open (allow + flag for review) + fall back to rules-only engine
- In wallet: AML Service timeout → fail-open (allow + flag); FX rate feed unavailable → serve stale rate with 2% spread (fail at >10 min stale)
- In ledger: external acquirer/bank feeds for reconciliation; if feed unavailable, queue for retry; no fail-open (reconciliation is not real-time)

### HSM-Backed Token Vault (PCI DSS)
- Two of the four handle raw PAN and use HSM (FIPS 140-2 Level 3) for cryptographic operations
- In payment_processing: PAN never logged or stored in plaintext; AES-256-GCM encryption with key hierarchy (MK → KEK → DEK); vault_token = opaque UUID; CVV accepted for auth but NEVER stored; log scrubbing via Luhn-valid regex replace; DEK rotation monthly, KEK annually, MK every 3 years (hardware ceremony)
- In stripe_gateway: hosted iframe on js.stripe.com (merchant never sees PAN); RSA-4096 ephemeral public key (rotated hourly); HMAC-SHA256(PAN, static_key) = card_fingerprint for dedup without storing PAN; Visa Token Service (VTS) / Mastercard MDES network token enrollment; minimum 2 HSM units in separate AZs
- In wallet: uses external payment processors (Stripe/Adyen) for card processing; does not handle raw PAN directly
- In ledger: no PAN; handles amounts only

## Common Databases

### PostgreSQL
- All four; ACID source of truth; sharded by merchant_id or account_id; Patroni for HA (<30 s failover, RPO=0 with sync replica); PgBouncer for connection pooling

### Cassandra (RF=3, LOCAL_QUORUM)
- All four; append-only audit log or primary ledger store; TTL=0 (permanent) or long retention; write-optimized LSM-tree

### Redis Cluster
- All four; idempotency cache (SETNX), balance cache (TTL 60 s), rate limits, AML velocity features

### ClickHouse
- Three of four (stripe_gateway, wallet, ledger); column-store OLAP; fraud/AML analytics; reporting

## Common Communication Patterns

### Transactional Outbox Pattern (Kafka at-least-once)
- All four use the transactional outbox: write outbox row atomically with primary DB change; Debezium (or scheduler) reads WAL and publishes to Kafka; idempotent consumers handle at-least-once delivery

### Webhook Delivery with HMAC Signature + Retry
- Two of the four (payment_processing, stripe_gateway) deliver webhooks with HMAC-SHA256 signature and exponential backoff retry; 5-minute replay window on receiver side

## Common Scalability Techniques

### Virtual Shard → Physical Node Mapping (Consistent Hash)
- All four shard PostgreSQL using virtual shards (1,024–2,048 virtual → physical nodes) to allow resharding without data movement; consistent hash ring; hot merchant/account → dedicated shard

### Materialized Balances + Balance Cache (Redis 60 s TTL)
- Three of four (wallet, ledger, payment_processing) maintain a materialized balance row updated atomically with each posting; Redis cache (60 s TTL) serves the vast majority of balance reads; authoritative PostgreSQL read on cache miss

## Common Deep Dive Questions

### How do you guarantee exactly-once payment processing?
Answer: Three-layer idempotency. Layer 1: Client-generated idempotency key (UUID). Layer 2: Redis SETNX atomic check — `SETNX idempotency:{merchant_id}:{key}` returns 0 if key exists; check stored status (completed/in_flight/hash_mismatch). Layer 3: PostgreSQL `SELECT FOR UPDATE` on idempotency_keys table (serializes concurrent requests to the same key). On completion, status=completed + response_body stored (TTL 24 h). On any failure, status=failed and safe to retry. If Redis is unavailable, fall back to PostgreSQL-only (slower but correct).
Present in: payment_processing, stripe_gateway, wallet, ledger

### How do you prevent double-spending / negative balances in a wallet?
Answer: Atomic check-and-update in a single SQL statement: `UPDATE account_balances SET balance = balance - :amount WHERE account_id = :id AND balance >= :amount AND version = :expected_version`. The `balance >= :amount` condition in the WHERE clause makes the NSF check and the decrement atomic — no separate SELECT + UPDATE needed, no TOCTOU window. If 0 rows updated, either insufficient funds or concurrent modification — re-read to disambiguate. CHECK constraint `balance >= 0` at the DB layer provides an additional backstop.
Present in: wallet, ledger

### How do you ensure webhook delivery at-least-once without losing events?
Answer: Transactional outbox pattern. On payment state change: write status update + outbox row atomically in one PostgreSQL transaction. Debezium reads WAL changes and publishes to Kafka (at-least-once). Delivery workers consume Kafka, attempt HTTP POST with HMAC-SHA256 signature, retry with exponential backoff (10 attempts over 72 h). Idempotency on the receiver side: include timestamp in payload + 5-minute replay window check. After 10 failures → DLQ + merchant dashboard alert. Kafka 7-day retention allows replay on consumer crash.
Present in: payment_processing, stripe_gateway

### How do you achieve 3.5M balance reads/second at low latency?
Answer: Materialized balance + Redis cache. After each ledger posting, the Balance Materializer consumes the Kafka event and updates: (1) PostgreSQL `materialized_balances` row (durable, ACID); (2) Redis `balance:{account_id}:{currency}` key (60 s TTL). Balance reads go to Redis first (< 1 ms, O(1)); on cache miss, read from PostgreSQL materialized_balances (< 10 ms); never scan raw Cassandra ledger entries for current balance. Balance display accepts up to 60 s staleness; exact balance enforced only at write time via optimistic lock.
Present in: wallet, ledger

## Common NFRs

- **Zero double-charges**: P0 alert if `payment.double_charge.count > 0`; equivalent P0 alert in wallet for `negative_balance_count > 0`
- **Idempotency**: 24 h window; duplicate key returns cached response, never re-executes
- **Auth/charge latency**: p99 < 3 s end-to-end (card network); internal p99 < 200–500 ms
- **Balance read latency**: p99 < 100 ms (wallet), < 500 ms (ledger point-in-time)
- **Availability**: 99.99% (payment_processing, wallet, ledger); 99.999% (stripe_gateway — 5.26 min/year)
- **Durability**: RPO=0 with synchronous PostgreSQL replica; Cassandra RF=3, LOCAL_QUORUM
- **PCI DSS Level 1**: card data isolated in Token Vault; AES-256-GCM at rest; TLS 1.3 in transit; HSM for key operations; CVV never stored
- **Double-entry invariant**: SUM(debits) = SUM(credits) per transaction; enforced at DB CHECK constraint and verified hourly; drift → P0 alert
- **Reconciliation**: daily external settlement file reconciliation; unresolved exceptions > $100 → alert; >7 days unresolved → P1
