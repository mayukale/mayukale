# Problem-Specific Design — Payments (10_payments)

## Payment Processing

### Unique Functional Requirements
- ISO 8583 protocol for card network communication (message types 0100 auth, 0200/0220 capture)
- Two-phase payment: authorize (hold on card) → separate capture within hold window (7 days Visa, 30 days Mastercard)
- Full 13-state payment state machine: INITIATED → AUTHORIZED → CAPTURE_PENDING → CAPTURED → PARTIALLY_CAPTURED → REFUND_PENDING → PARTIALLY_REFUNDED → REFUNDED → VOIDED → FAILED → CHARGEBACK_INITIATED → CHARGEBACK_LOST → CHARGEBACK_WON
- Acquirer routing: BIN-based routing, merchant configuration, cost optimization, uptime weighting
- Chargeback lifecycle: dispute received from issuer → evidence submission window → WIN or LOST outcome
- PCI DSS Level 1: log scrubbing, CVV handling, key hierarchy

### Unique Components / Services
- **Token Vault**: HSM FIPS 140-2 Level 3; PAN → vault_token (opaque UUID); AES-256-GCM encryption with key hierarchy (Master Key → KEK → DEK); CVV accepted for auth but discarded immediately after (NEVER stored); Log scrubbing via regex Luhn-valid card number detection; key rotation: DEK monthly, KEK annually, MK every 3 years (hardware ceremony); minimum 2 HSM units in separate AZs; also issues Visa Token Service (VTS) / MDES network tokens for card-on-file
- **Routing Engine**: selects acquirer per transaction by BIN prefix (first 6 digits), merchant configuration (preferred acquirer), least-cost routing, acquirer real-time uptime from circuit breaker state; failover to secondary acquirer on primary timeout/failure
- **Idempotency Service**: SHA-256(request_body) → request_hash to detect body mismatch; Redis SETNX (24 h TTL) + PostgreSQL FOR UPDATE fallback; status: in_flight → completed/hash_mismatch
- **Webhook Dispatcher**: Kafka consumers (48 partitions by payment_id); retry schedule: immediate → 100 ms → 500 ms → 2 s → exponential; max 3 days; 10,000 workers at peak (100K events/sec)

### Unique Data Model
- **payments** (PostgreSQL + Citus, shard by merchant_id): status (13-state ENUM), idempotency_key (UNIQUE), authorized_amount BIGINT, captured_amount BIGINT, refunded_amount BIGINT, version INT (optimistic lock); CHECK `captured_amount <= amount`, `refunded_amount <= captured_amount`
- **payment_state_log**: append-only history (from_state, to_state, transitioned_at, actor, metadata JSONB)
- **chargebacks**: chargeback_id, payment_id, reason_code, amount, evidence_due_at, status (INITIATED/WON/LOST)
- **acquirer_routing**: BIN prefix → acquirer priority list + current circuit breaker state
- **Cassandra audit log**: TimeUUID partition by payment_id, clustering by created_at; records every external ISO 8583 message sent/received; RF=3, LOCAL_QUORUM
- **Hot merchant mitigation**: top 1% of merchants (30% of traffic) get dedicated PostgreSQL shard + read replicas; detected by Routing Engine monitoring per-shard TPS

### Unique Scalability / Design Decisions
- **Shard by merchant_id (not payment_id)**: co-locates all payments for a merchant on one shard; merchant reports and reconciliation are intra-shard queries (no cross-shard joins); hot merchants isolated to dedicated shards
- **Two-phase auth+capture**: auth authorizes a hold on the cardholder account (reserve); capture triggers actual clearing; allows modification of amount between auth and capture (e.g., tip addition at restaurant); separate capture reduces decline rates on pre-authorizations
- **ISO 8583 integration (not REST)**: card network protocols use ISO 8583 binary format, not HTTP; the system must speak the protocol spoken by Visa/Mastercard interchange networks; acquirers provide ISO 8583 endpoints

### Key Differentiator
Payment Processing's uniqueness is its **acquirer routing layer + ISO 8583 protocol integration**: it is the only problem in this folder that directly interfaces with card networks via ISO 8583 messages, has a full 13-state machine including chargebacks (CHARGEBACK_INITIATED/WON/LOST), and implements multi-acquirer routing with BIN-based selection and circuit-breaker-based failover — the full stack of a payment processor, not just an application using one.

---

## Stripe Gateway (PSP)

### Unique Functional Requirements
- Payment Service Provider: merchants never see raw card numbers (hosted iframe on js.stripe.com)
- Connect platform: marketplace model where a platform charges buyers, splits funds to sellers (destination charges, application fees, payouts)
- Webhook delivery guarantees: signed HMAC-SHA256 + 5-minute replay prevention; retry 10 attempts over 72 h
- API date-based versioning (e.g., 2024-06-20); all versions supported ≥ 3 years; new merchants get latest; existing merchants pinned
- 99.999% uptime (5.26 min/year) — higher than other problems in this folder
- GDPR/CCPA/PSD2 compliance in addition to PCI DSS

### Unique Components / Services
- **Hosted iframe (js.stripe.com)**: JS SDK renders a sandboxed iframe for card input; PAN never touches merchant's domain; iframe communicates with Stripe's token endpoint only; TLS 1.3 + certificate pinning
- **RSA-4096 ephemeral encryption**: PAN encrypted client-side with hourly-rotated RSA-4096 public key before POST to token endpoint; HSM decrypts; prevents interception even if TLS is compromised
- **Card fingerprint (HMAC-SHA256(PAN, static_key))**: deduplication key without storing PAN; allows "this card is already on file" detection across merchants without sharing PAN
- **Connect Platform Service**: destination charges (`transfer_data: {destination: acct_xxx, amount: 8500}`); application fees; payout hold 7 days for new connected accounts; velocity limits; transaction monitoring; pre-charge KYC status check; charge reversal if transfer to suspended account fails
- **Fraud Engine**: GPU-backed inference (target 50 ms p50, 100 ms p99); in critical path of charge; fail-open on timeout (allow + flag for review); falls back to rules-only engine on circuit open
- **API Versioning Compatibility Layer**: translates between date-based API versions without running multiple code paths; new data model fields ignored by old versions; deprecated fields backfilled for pinned merchants
- **Global Anycast Edge (Cloudflare/CloudFront)**: DDoS mitigation, TLS termination, geo-routing; < 100 ms global p95 latency target

### Unique Data Model
- **payment_methods** (PostgreSQL): vault_token, card_fingerprint (HMAC-SHA256, for dedup), bin_info JSONB, network_token_id (VTS/MDES enrolled), payment_method_type (card/ach/wallet), customer_id; NEVER store PAN or CVV
- **connected_accounts** (PostgreSQL): account_type (express/standard/custom), kyc_status, capabilities JSONB, payout_schedule, payout_hold_until
- **transfers** (PostgreSQL): source_charge_id, destination_account_id, amount, currency, platform_fee, status; created atomically with charge capture
- **webhook_endpoints** (PostgreSQL): url, events_to_listen TEXT[], enabled_events, api_version (pinned), secret (HMAC key), disabled_at
- **Cassandra webhook_delivery_log**: partition by merchant_id, clustering by (created_at, event_id); delivery_attempt_count, last_error, next_retry_at; RF=3
- **ClickHouse fraud_features**: per-merchant/per-card aggregations (chargeback_rate_30d, decline_rate_7d, velocity features); updated every 5 min from Kafka stream

### Unique Scalability / Design Decisions
- **Webhook signature + 5-minute replay window**: HMAC-SHA256(raw_body, webhook_secret) with timestamp in payload; receiver validates signature and rejects events where |NOW() - timestamp| > 300 s; prevents replay attacks even if HTTPS is compromised; constant-time comparison prevents timing side-channel
- **96-partition webhook-events Kafka topic by merchant_id**: ensures strict event ordering per merchant (payment.created always before payment.succeeded for same payment); 1000 delivery worker pods at peak
- **Connect payout hold (7 days for new accounts)**: protects platform from disputed charges; after hold period, transfers to connected account's bank; velocity limits prevent immediate large payouts by new fraudulent merchants

### Key Differentiator
Stripe Gateway's uniqueness is its **Connect platform + hosted iframe PAN isolation + date-based API versioning**: Connect enables marketplace businesses where Stripe intermediates between a platform and many sellers (destination charges, application fees, separate KYC per connected account); the hosted iframe design is the only secure way to accept cards without the merchant entering PCI DSS scope; API versioning with 3-year support commitment makes Stripe a platform, not just an API — no other problem in this folder has this multi-tenant, multi-version, marketplace architecture.

---

## Wallet

### Unique Functional Requirements
- Multi-currency balance management with FX conversion (spot rate from Bloomberg B-PIPE, 0.5% spread, 60 s rate lock)
- KYC tiers governing spend limits: Level 0 (no KYC) = receive only, send < $200/day; Level 1 (phone + email verified) = send < $1,000/day; Level 2 (Gov ID + selfie + address) = send < $20,000/day
- AML pre-transaction real-time scoring with velocity features (gradient-boosted tree, < 60 ms p99)
- 2FA enforcement: withdrawal > $100, top-up > $500, adding new payment method
- OFAC/World-Check sanctions screening at KYC + nightly re-screen of all accounts + re-screen on payout > $3,000
- Reserved balance field: funds reserved for pending ACH debits (not yet cleared)

### Unique Components / Services
- **KYC Service**: document upload via Jumio/Onfido; OCR (name, DOB, ID number, expiry) + liveness check (video selfie, blink/turn + anti-spoofing) + face match (selfie vs. ID photo); 2,500+ document types across 195 countries; verdict: clear → kyc_level=2; consider → manual review queue; refer → compliance review (hard reject); OFAC sanctions screening via World-Check API
- **AML Service**: reads velocity features from Redis (pre-computed by ClickHouse, refreshed every 60 s); gradient-boosted tree ML model (10 ms inference); features: total_sent_last_1h, total_sent_last_24h, transfer_count_last_1h, unique_recipients_last_7d, avg_transfer_last_30d, recipient_risk_score, account_age, device_fingerprint, geo; thresholds: score < 30 → ALLOW, 30–70 → CHALLENGE (require 2FA), > 70 → BLOCK; GPU inference pods (5K scores/sec per pod, 3 pods)
- **FX Service**: spot rate from Bloomberg B-PIPE (primary) / ECB (fallback); cached in Redis 30 s TTL; 60 s rate lock per quote (prevents arbitrage); spread 0.5%; if stale > 2 min → 2% spread; if stale > 10 min → fail-open; hedges net exposure daily via Interactive Brokers / bank FX desk; alerts when exposure > $10M per pair
- **Funding Service**: initiates card charges (via Stripe/Adyen for top-up), ACH debits (top-up), ACH credits (withdrawal); maintains `reserved_balance` for pending ACH; on ACH R10 return → debit wallet; if insufficient → freeze account

### Unique Data Model
- **account_balances** (PostgreSQL): account_id, currency, balance BIGINT (cents), reserved_balance BIGINT, version INT; CHECK `balance >= 0`, CHECK `reserved_balance <= balance`; double-debit prevented by `balance >= :amount` in UPDATE WHERE clause
- **transfers** (PostgreSQL): transfer_id, sender_id, recipient_id, amount, currency, fraud_score, aml_action (allow/challenge/block), idempotency_key; status ENUM (pending/processing/completed/failed/reversed)
- **fx_conversions** (PostgreSQL): quote_id, from_account_id, to_account_id, from_amount, to_amount, from_currency, to_currency, rate, spread, fee, confirmed_at
- **kyc_documents** (PostgreSQL): user_id, document_type, provider (onfido/jumio), provider_check_id, status (clear/consider/refer), extracted fields JSONB
- **sanctions_screenings** (PostgreSQL): user_id, screened_at, result (clear/match), alert_id (if match), resolved_at
- **spend_limits** (PostgreSQL): user_id, daily_limit, weekly_limit, monthly_limit, override_reason; per-currency
- **Atomic P2P transfer SQL**: debit sender `WHERE balance >= :amount AND version = :ver` + credit recipient + insert 2 ledger entries + insert transfer record — all in one SERIALIZABLE transaction; Redis balance cache invalidated post-commit

### Unique Scalability / Design Decisions
- **`balance >= :amount` in WHERE clause (not separate SELECT)**: eliminates TOCTOU race window; if two concurrent transfers both pass the check, only one will update (the other will get 0 rows updated = race lost → retry); no distributed lock needed; no SELECT FOR UPDATE (avoids row-level contention at scale)
- **KYC tier-based rate limiting**: Level 0 users can only receive (not send); Level 1 can send up to $1,000/day; Level 2 up to $20,000/day; these limits are enforced by a velocity check in the AML layer before each transaction — KYC tier gates the amount, not the operation
- **Redis pre-computed velocity features (not real-time ClickHouse query)**: AML scoring must complete in < 60 ms; real-time ClickHouse aggregation over 1B+ rows would take seconds; instead, Flink or ClickHouse materialized views pre-compute rolling windows every 60 s; values stored in Redis per account; AML Service reads from Redis (< 2 ms) — trades 60 s staleness in velocity features for < 60 ms p99 AML scoring

### Key Differentiator
Wallet's uniqueness is its **KYC/AML pipeline + multi-currency FX + reserved balance model**: the combination of tiered KYC (0/1/2) governing spend limits, real-time AML scoring (velocity features from Redis + gradient-boosted tree at < 60 ms), OFAC sanctions screening (at KYC + nightly + payout), and FX rate locking (60 s lock to prevent arbitrage) represents the compliance stack of a regulated financial institution — no other problem in this folder has regulatory compliance as a first-class architectural concern driving data model design (kyc_level, reserved_balance, sanctions_screenings).

---

## Ledger

### Unique Functional Requirements
- Double-entry bookkeeping: every transaction must have matching debits and credits; SUM(debits) = SUM(credits) per transaction enforced at DB CHECK constraint and verified hourly
- Immutability: entries are never updated or deleted; corrections via reversal (equal-and-opposite new entries)
- Hash chain tamper detection: each entry includes SHA-256 of (account_id || created_at || id || amount || direction || previous_hash); cumulative checkpoint hash computed hourly; nightly verification job alerts on mismatch (P0)
- Point-in-time balance: efficiently compute balance at any historical timestamp via hourly balance snapshots + range scan
- Cold archive: entries > 2 years → S3 Parquet with Object Lock (Compliance mode, WORM); prevents deletion by any user
- Reconciliation: daily comparison of internal ledger vs. external acquirer/bank settlement files; auto-resolve < $1 rounding; > $1 exception → manual review; > 7 days unresolved → P1
- Scale: 350K entries/sec (100K TPS × 3.5 entries per tx); 110 trillion entries over 10 years; 77 PB raw

### Unique Components / Services
- **Write Coordinator**: validates balanced entries (SUM(debits) = SUM(credits)) before accepting; appends hash chain value; batches Cassandra inserts; publishes to Kafka; rejects unbalanced entries with error
- **Balance Materializer** (Kafka consumer): maintains Redis balance cache (60 s TTL) + PostgreSQL materialized_balances table; updated after each Kafka event; staleness ≤ 1–2 s from Cassandra write
- **Hash Chain Verifier** (nightly batch): reads Cassandra entries, recomputes cumulative hash, compares to stored checkpoints; discrepancy → P0 alert (data integrity violation)
- **Reconciliation Engine**: pulls settlement files from external systems (SFTP or API); parses ISO 8583 clearing records or CSV; matches by network_transaction_id; flags: missing_in_external, missing_in_internal, amount_mismatch; auto-resolves < $1 (rounding tolerance); creates reconciliation_exceptions for unresolved items
- **ClickHouse OLAP**: period-close P&L reports; balance sheet generation; inter-account analytics; batch insert from Kafka (1-second micro-batches)
- **Cold Archive Pipeline**: Spark job reads Cassandra entries > 2 years → converts to Parquet (10–20× compression) → writes to S3 with Object Lock (Compliance mode) → deletes from Cassandra after successful archive confirmation

### Unique Data Model
- **Chart of Accounts (PostgreSQL)**: GL structure with account_type (asset/liability/equity/revenue/expense), normal_balance ('D' or 'C'); standard accounts: 1000=Cash, 2000=Merchant Payable, 2001=Customer Wallet, 3000=Revenue-Fees, 3001=Revenue-FX Spread, 4000=Expense-Interchange, 4001=Expense-Network Fees, 5000=Receivable-Cardholder; merchant-specific accounts: `2000-{merchant_id}`
- **ledger_entries** (Cassandra, PRIMARY store): entry_id (TIMEUUID), account_id, created_at, amount BIGINT (always positive), direction TINYINT (1=debit, -1=credit), entry_type, entry_hash, previous_entry_hash; partition by account_id; clustering by (created_at DESC, id); TTL=0; TWCS 30-day window; RF=3, LOCAL_QUORUM
- **ledger_transactions** (PostgreSQL): transaction_id, source_event_id, source_system, idempotency_key (UNIQUE), total_debit, total_credit; generated column `is_balanced = (total_debit = total_credit)`; CHECK `total_debit = total_credit`
- **materialized_balances** (PostgreSQL): account_id, currency, balance BIGINT, as_of_entry_id, updated_at; updated atomically with each posting; authoritative for current balance without scanning Cassandra
- **balance_snapshots** (PostgreSQL): account_id, currency, balance, snapshot_at, entry_id_at_snapshot; created hourly; enables point-in-time query as: nearest snapshot before T + range scan of Cassandra entries from snapshot to T
- **reconciliation_exceptions** (PostgreSQL): run_id, exception_type (missing_in_external/missing_in_internal/amount_mismatch), internal_entry_id, external_ref, internal_amount, external_amount, status (open/resolved/auto_resolved); indexed on (run_id, status) for monitoring dashboard

### Unique Scalability / Design Decisions
- **Cassandra as PRIMARY ledger store (not PostgreSQL)**: 350K inserts/sec exceeds PostgreSQL write capacity even with sharding; Cassandra's LSM-tree architecture (sequential writes to memtable → SSTable) handles this rate naturally; append-only matches Cassandra's write model perfectly (no UPDATE, no DELETE); TWCS aligns compaction windows with TTL/archival boundaries — old month's data is in immutable SSTables that compact cleanly
- **Materialized balance (not derived from Cassandra scan)**: querying current balance by scanning all Cassandra entries for an account_id would be O(entries) = millions of rows per account for a busy merchant; materialized_balances keeps a running total updated atomically with each entry INSERT in PostgreSQL; Redis cache serves 3.5M RPS balance reads in < 1 ms
- **Hourly balance snapshots for point-in-time**: without snapshots, computing balance at timestamp T requires scanning ALL entries from genesis; with hourly snapshots, point-in-time query = nearest snapshot + range scan of at most 1 hour of entries (bounded scan); critical for audit, tax reporting, and dispute resolution
- **Hash chain tamper detection**: financial regulators (SOX, PCI DSS) require audit trail integrity; storing SHA-256(current entry + previous hash) creates a chain where modifying any entry breaks all subsequent hashes; cumulative checkpoint hashes allow verification without recomputing the entire chain; P0 alert on mismatch
- **WORM cold archive (S3 Object Lock, Compliance mode)**: Compliance mode prevents deletion even by root/admin users or AWS Support; meets SOX 7-year retention requirement for financial records; entries > 2 years are rarely accessed (audit, dispute resolution only); Parquet compression (10–20×) reduces 77 PB to 8–15 PB

### Key Differentiator
Ledger's uniqueness is its **accounting correctness guarantees at financial-database scale**: the double-entry invariant (enforced at both write-time validation AND database CHECK constraint), hash chain tamper detection (per-entry SHA-256 chain + hourly checkpoints + nightly verification), WORM cold archive (S3 Object Lock Compliance mode), and hourly balance snapshots for point-in-time queries — combined with 350K entries/sec via Cassandra's LSM-tree append-only storage — make this the only problem in this folder designed for regulatory compliance first, not just operational correctness.
