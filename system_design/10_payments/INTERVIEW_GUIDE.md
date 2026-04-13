# Pattern 10: Payments — Interview Study Guide

Reading Pattern 10: Payments — 4 problems, 8 shared components

---

## STEP 1 — ORIENTATION

This pattern covers four distinct but deeply related payment system designs:

1. **Payment Processing** — A full card payment processor: accepts card payments, routes ISO 8583 messages through card networks to issuing banks, manages a 13-state payment lifecycle, handles chargebacks, and settles with acquirers.
2. **Stripe-like Payment Gateway (PSP)** — A Payment Service Provider layer that sits between merchants and the card networks: hosted iframe tokenization, Connect marketplace platform, multi-currency, date-based API versioning, and fraud scoring.
3. **Digital Wallet** — An internal balance store for P2P money movement: double-entry ledger, multi-currency FX, KYC tiers, AML scoring, OFAC sanctions, and bank funding via ACH.
4. **Financial Ledger** — The accounting backbone: double-entry bookkeeping at 350K entries/second, cryptographic hash chain tamper detection, hourly balance snapshots for point-in-time queries, and WORM cold archival.

All four share 8 common architectural building blocks: **PostgreSQL** (ACID source of truth), **Idempotency via Redis SETNX + Postgres fallback**, **Cassandra** (append-only audit log), **Kafka** (transactional outbox event streaming), **Redis** (cache, rate limits, balance materialization), **Optimistic locking** via version column, **ClickHouse** (OLAP fraud/AML/reporting), and **Circuit breakers** on all external calls.

---

## STEP 2 — MENTAL MODEL

### Core Idea

The single most important idea in all four problems is this: **money must never be created or destroyed by a software bug.** Every operation is fundamentally a state transition that must be atomic, durable, and idempotent. The architecture of every payments system is just a series of answers to the question: "what happens when this step succeeds but the network drops the acknowledgement?"

### Real-World Analogy

Think of paying a restaurant bill. You hand your card to the server. The server takes it to the card reader out of sight (tokenization — you never see the restaurant's backend system). The reader calls your bank via Visa's network (ISO 8583). Your bank puts a hold on your account (authorization). When you sign or tap, the restaurant sends the final amount to their acquirer for settlement (capture). A few days later, your bank releases the hold and posts the actual charge (clearing/settlement). If something goes wrong — the restaurant double-charges you — you call your bank and they initiate a dispute (chargeback). Every one of those steps maps to a component in these designs.

### Why It's Hard

Three compounding problems make payments uniquely difficult:

**1. The distributed systems problem applied to money.** When your server charges a card and the network dies before the response arrives, did the charge go through? The answer determines whether you charge the customer again or not. Get it wrong either way and someone loses money. Idempotency keys are the entire answer to this, and most interview discussions stall here.

**2. Correctness requirements that cannot be relaxed.** In most systems, eventual consistency is an acceptable trade-off. In payments, "eventually consistent balance" means someone can spend money they don't have. The architecture must be strongly consistent at the balance mutation layer without making every read go to a primary database.

**3. External systems you don't control.** Card networks, issuing banks, ACH clearing houses, and fraud data providers are external black boxes that go down, return ambiguous responses, and have their own latency characteristics. You need circuit breakers, retry logic, and fallback paths, and you need to explain what happens when any one of them fails.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these at the start of any payments interview. The answers completely reshape the design.

**Question 1: What type of payment are we processing — card-present, card-not-present, ACH, P2P wallet transfer, or something else?**
What changes: Card-not-present requires tokenization and PCI DSS scope design. ACH adds async settlement (1-5 business days) and return codes. P2P wallet is internal double-entry with no external network. The entire system shape changes.

**Question 2: Are we the processor (we connect directly to card networks via ISO 8583), or are we a merchant/platform sitting on top of a processor like Stripe or Adyen?**
What changes: If you're building a processor, you need ISO 8583 integration, acquirer relationships, and a routing engine. If you're building a platform on top of a processor, the hard parts are tokenization, webhook delivery, and the developer experience. These are almost completely different designs.

**Question 3: What is the target scale — transactions per second, and what's the read/write ratio?**
What changes: 1K TPS fits on a single PostgreSQL primary. 50K TPS requires sharding. 350K entries/sec requires Cassandra as the primary write store. Balance reads at 3.5M RPS require a Redis materialized cache. The numbers drive every database choice.

**Question 4: Do we need to store cardholder data (PAN), or are we always using a third-party vault?**
What changes: Storing PAN means PCI DSS Level 1 compliance, HSM-backed vault, isolated namespace, log scrubbing, and key hierarchy management. Using a third-party vault (Stripe, Braintree) removes all of that from your scope at the cost of vendor dependency and per-transaction fees.

**Question 5: Is there a marketplace / multi-party model — for example, a platform that charges buyers and splits funds to sellers?**
What changes: A marketplace requires a Connect-style platform with destination charges, application fees, per-seller KYC, payout hold periods, and fund routing logic. This is an entire additional subsystem. Single-merchant payment processing is dramatically simpler.

**Red flags to watch for in the interview:**
- The interviewer asks "and how would you prevent double charges?" — they want the idempotency key protocol, not just "use transactions."
- The interviewer says "the card network times out" — they want you to describe the circuit breaker, retry, and what happens if the charge was already processed at the network but the response was lost.
- The interviewer asks about "settlement" vs "capture" — they're checking whether you understand the two-phase auth/capture model and the fact that authorization is not the same as money movement.

---

### 3b. Functional Requirements

**Core (must-have for any payments problem):**
- Accept payments via some method (card, bank transfer, wallet)
- Idempotent charge/transfer creation — retrying with the same key never double-charges
- Full or partial refunds against prior charges
- Transaction history with filtering and cursor-based pagination
- Payment state machine with defined valid transitions
- Webhook/notification delivery for async state changes

**Scope questions that change the design:**
- Chargebacks (requires dispute lifecycle, evidence submission, 13-state machine) — Payment Processing specific
- Hosted iframe / vault tokenization (requires PCI architecture) — Payment Processing and Stripe Gateway
- Connect marketplace (requires destination charges, application fees, payout scheduling) — Stripe Gateway specific
- KYC tiers and spend limits (requires KYC service, AML scoring, OFAC screening) — Wallet specific
- Double-entry bookkeeping with hash chain integrity — Ledger specific

**Clear functional statement for an interview opening:**
"I'm designing a system that allows merchants to charge customers' payment methods, ensures no charge is ever created twice for the same request regardless of network conditions, supports refunds, and delivers reliable event notifications for all state changes."

---

### 3c. Non-Functional Requirements (NFRs)

**Derive these from first principles, not from memorizing numbers.**

**Availability:** Payments are revenue-critical. Every minute of downtime has a quantifiable cost. Payment Processing and Wallet: 99.99% (52 minutes/year). Stripe Gateway: 99.999% (5.26 minutes/year — higher because all of your merchants' revenue runs through you). Ledger: 99.99% (you can't post entries while it's down, which blocks all payments).

**Latency:** Card authorization is bounded by the issuing bank's response time, which is measured in hundreds of milliseconds to 2 seconds. Your internal processing P99 must be well under 200ms to leave headroom. Balance reads must be under 100ms — users checking their balance are in a time-sensitive checkout flow.

**Trade-off to articulate unprompted:** Availability vs. Consistency. You cannot have both at the balance level. The correct answer is: strong consistency at write time (balance decrement is always serialized), but materialized reads with a short TTL (up to 60 seconds stale) for read-heavy balance display. Never relax write consistency for money.

**Durability:** Zero data loss is not a design aspiration; it is a hard requirement. PostgreSQL synchronous replication to one standby (RPO=0). Cassandra RF=3 with LOCAL_QUORUM (tolerates one node loss with no data loss). Redis is not a primary store for any financial data — it is always a cache in front of a durable store.

**✅ Derived NFR:** "I need strong consistency at the point of balance mutation — no eventual consistency for money writes — but I can serve balance reads from a 60-second materialized cache to handle the 10x read/write ratio."

**❌ Anti-pattern:** "I'll use DynamoDB for low latency." DynamoDB with eventual consistency is wrong for balance mutations. If you propose DynamoDB, explain exactly why: high-throughput idempotency key cache (yes), sole authoritative balance store (no).

---

### 3d. Capacity Estimation

**Formula pattern — use this structure for any payments problem:**

```
Daily transactions = (active merchants or users) × (avg transactions/day)
Average TPS        = daily transactions ÷ 86,400
Peak TPS           = average TPS × peak multiplier (5x–15x)
Storage per day    = transactions × record size
Write throughput   = peak TPS × entries per transaction
```

**Anchor numbers for common scenarios:**

| Scenario | Scale |
|---|---|
| Payment processor (10M merchants, 100 tx/day avg) | 1B tx/day, ~11K avg TPS, 50-150K peak TPS |
| Stripe-like PSP (5M merchants, 200 tx/day avg) | 1B tx/day, ~12K avg TPS, 100-200K peak TPS |
| Wallet (100M users, 20M DAU, 5 tx/DAU) | 100M tx/day, ~1,200 avg TPS, 5-10K peak TPS |
| Ledger (backing payment processor above) | 350K entries/sec at peak (3.5 entries per transaction) |

**Architecture implications of the numbers:**
- 50K TPS → need to shard PostgreSQL (single instance maxes at ~50-100K TPS but with headroom risk; shard at ~20K for safety)
- 350K entries/sec → Cassandra is required (PostgreSQL cannot sustain this on any number of nodes without distributed SQL)
- 3.5M balance reads/sec → Redis materialized balance cache is required (PostgreSQL read replicas max out well below this)
- 1B webhook events/day → Kafka with 48-96 partitions and ~1,000 delivery worker pods

**Time allocation guidance:** Spend 5-7 minutes on estimation. State your assumptions out loud. Show you know which number forces which architecture decision (e.g., "at 350K inserts/sec, PostgreSQL can't keep up, which is why I'm using Cassandra as the primary ledger store, not PostgreSQL").

---

### 3e. High-Level Design (HLD)

**Whiteboard in this order — it tells a story:**

1. **Client / Merchant** — whoever initiates the payment
2. **API Gateway** — TLS termination, JWT/API key auth, rate limiting per merchant, request routing
3. **Core Service** (Payment API / Wallet Service / Core Charge Service) — the orchestrator
4. **Idempotency Service** — Redis SETNX + Postgres fallback; always the first call in the critical path
5. **Data store trio** — PostgreSQL (state), Redis (cache), Cassandra (audit log)
6. **External dependency** — Card network, bank, HSM vault, ACH
7. **Event bus (Kafka)** — publishes state changes via transactional outbox
8. **Downstream consumers** — Webhook dispatcher, AML/fraud, audit log writer, notification service

**Key design decisions to articulate for each:**

**PostgreSQL** — ACID, JSONB, optimistic locking via `version` column, CHECK constraints that enforce financial invariants at the DB layer, sharded by `merchant_id` or `account_id` using virtual shards (1,024 virtual shards mapped to physical nodes).

**Kafka with transactional outbox** — Don't publish to Kafka directly in application code. Instead: write the state change + an outbox row in the same PostgreSQL transaction, then use Debezium (CDC on the WAL) to publish to Kafka. This gives at-least-once delivery even if the application crashes after the DB write but before the Kafka publish.

**Redis idempotency** — Redis SETNX is the fast path. If SETNX returns 0, the key already exists — return the cached response. Postgres `SELECT FOR UPDATE` is the slow fallback if Redis is unavailable. Both layers together handle the edge case where the application crashes between the card charge and the idempotency key write (see 3f for the full algorithm).

**Data flow narration (card payment, Payment Processing):**
1. Merchant `POST /v1/payments` with `{token, amount, idempotency_key}`
2. API Gateway validates auth, checks rate limit
3. Payment API Service calls Idempotency Service (Redis SETNX) — returns cached response if duplicate
4. Token Vault exchanges vault_token for PAN (in HSM-isolated process)
5. Routing Engine selects acquirer based on BIN, cost, circuit breaker state
6. ISO 8583 authorization sent to card network → issuing bank → response
7. PostgreSQL `payments` row updated (status=AUTHORIZED, auth_code stored), same transaction writes idempotency key
8. `charge.authorized` event published to Kafka via outbox
9. Kafka consumers: Audit Log → Cassandra, Webhook Dispatcher → merchant HTTP endpoint
10. HTTP 200 returned to merchant

---

### 3f. Deep Dive Areas

The three areas interviewers probe most deeply in payments, in order of frequency:

**Deep Dive 1: Idempotency and the "ghost charge" problem**

**Problem:** The application charges the card via the network, the charge succeeds, but the server crashes before writing the response to Redis/Postgres. The client retries with the same idempotency key. How do you prevent a double charge?

**Solution:** The idempotency key write and the payment record write happen in the same PostgreSQL transaction. The sequence is:
1. Redis SETNX `idempotency:{merchant_id}:{key}` with status=`in_flight` and TTL=24h.
2. Begin Postgres transaction: INSERT idempotency_key row (status=`in_flight`) + INSERT payment row.
3. Call card network (the charge happens here).
4. On success: UPDATE both idempotency_key (status=`completed`, response_body=...) and payment (status=AUTHORIZED) in the same transaction. COMMIT.
5. On crash after step 3 but before step 4: Postgres rolls back. On client retry, idempotency key is not in Postgres, SETNX succeeds, and the charge is re-attempted. The card network's own deduplication (via `network_transaction_id`) prevents a second charge at the network level.

The critical insight: writing the payment record and the idempotency completion atomically in Postgres means there is no window where the charge is "done" but the idempotency key doesn't know about it.

**Unprompted trade-off:** "I could use Redis-only for idempotency, which is faster. But Redis is not durable enough for financial data — a Redis restart would lose in-flight keys and allow double charges. The Postgres fallback is 10x slower but provides the guarantee. The Redis fast path handles 99.9% of requests."

**✅ Correct:** Redis SETNX fast path + Postgres `SELECT FOR UPDATE` fallback; atomic commit of payment + idempotency key together.

**❌ Wrong:** "I'll use a UNIQUE constraint in the database" — this alone doesn't handle the in-flight lock case and is slow for high-throughput lookups.

---

**Deep Dive 2: Preventing double-spend / negative balance atomically**

**Problem:** Two concurrent transfers from the same account both check the balance, both see $100, both proceed — the account ends up at -$100. How do you prevent this without a distributed lock?

**Solution:** The atomic check-and-decrement in a single SQL statement:

```sql
UPDATE account_balances 
SET balance = balance - :amount, 
    version = version + 1
WHERE account_id = :id 
  AND balance >= :amount 
  AND version = :expected_version;
```

If this returns 0 rows updated, two things might have happened: (a) insufficient funds, or (b) concurrent modification changed the version. Re-read the row to distinguish. The `balance >= :amount` condition in the WHERE clause is not just a filter — it is the NSF check fused with the update, eliminating the time-of-check-to-time-of-use (TOCTOU) race window entirely.

A `CHECK (balance >= 0)` constraint at the database level is a second backstop — even if application code has a bug, the database will reject a negative balance write.

**Unprompted trade-off:** "I could use SELECT FOR UPDATE to lock the row before reading the balance, which is more intuitive. The problem is that it holds a lock while the fraud check and other business logic run, creating lock contention at scale. The check-and-decrement approach is optimistic — no lock held except for the microsecond the UPDATE runs — which is orders of magnitude better for throughput."

---

**Deep Dive 3: Reliable webhook delivery**

**Problem:** The merchant's server is down when a `payment.captured` event occurs. How do you guarantee the merchant eventually receives the event, without losing it, and without delivering it twice?

**Solution:** Transactional outbox + Kafka + at-least-once delivery workers with exponential backoff.

Step 1: When the payment state changes, insert an outbox row atomically in the same Postgres transaction as the state change. This guarantees: if the state change committed, the outbox row committed.

Step 2: Debezium reads the Postgres WAL (not the application publishing directly) and publishes to a Kafka topic partitioned by `merchant_id`. Partitioning by merchant_id ensures strict ordering — a merchant always sees `charge.created` before `charge.succeeded`.

Step 3: Delivery workers consume Kafka and attempt HTTP POST to the merchant's endpoint. Each delivery includes an HMAC-SHA256 signature over the payload + timestamp. On failure: exponential backoff (immediate → 100ms → 500ms → 2s → ... up to 72 hours, 10 attempts). After all attempts fail: Dead Letter Queue + merchant dashboard alert.

Step 4: Merchant-side deduplication. The payload includes the event ID. Merchants implement idempotent handlers that check "have I already processed this event ID?" This makes the system safe under at-least-once delivery.

**Unprompted trade-off:** "Exactly-once delivery would require a two-phase commit between our Kafka consumer and the merchant's database — essentially a distributed transaction with a system we don't control. That's not feasible. At-least-once delivery with a well-defined replay window (we sign with timestamp; merchant rejects events older than 5 minutes) is the right model for this. Stripe uses this exact approach."

---

### 3g. Failure Scenarios

**Card network timeout:** Authorization request times out at 3 seconds. The circuit breaker tracks error rate over a 60-second window. If error rate exceeds 20%, the breaker opens — all requests fail fast with "network_unavailable" rather than waiting 3 seconds each. After 30 seconds, the breaker enters half-open and allows 10% of traffic through as a probe. On recovery: close the breaker. In the meantime, route to backup acquirer. The idempotency key prevents double-charge on client retry.

**Primary PostgreSQL failure:** Synchronous replication to one standby means RPO=0. When the primary fails, the standby has every committed transaction. Patroni (or equivalent) promotes the standby in under 30 seconds. Writes fail for those 30 seconds — this maps directly to why 99.99% availability (not 100%) is the right target for payments; the 52 minutes of allowable downtime per year must absorb failover events.

**Redis failure (idempotency cache down):** The application falls back to Postgres-only idempotency (SELECT FOR UPDATE on the idempotency_keys table). This is slower (~10ms vs <1ms) and creates more load on Postgres, but it is correct. The system degrades gracefully rather than failing or allowing double charges.

**Token Vault service down:** No new authorizations are possible because the PAN cannot be retrieved from the vault for the ISO 8583 message. This is why the vault must run in multiple availability zones with active-active or active-standby. There should never be a single vault instance. A vault outage is a complete payments outage — it's the highest-priority dependency.

**Senior framing for all failure scenarios:** "I think about failure in three categories: (1) degraded but correct — the system keeps working with reduced performance (Redis down → fall back to Postgres). (2) Paused but recoverable — operations fail temporarily and will succeed on retry (network partition, brief DB failover). (3) Data integrity violation — the thing we must never let happen (double charge, negative balance, lost ledger entry). My architecture puts circuit breakers, fallbacks, and idempotency on category 1 and 2, and multiple independent safeguards — application code, DB CHECK constraints, network-level deduplication — on category 3."

---

## STEP 4 — COMMON COMPONENTS

These 8 components appear across all four payment problems. For each: why it's used, critical configuration, and what breaks without it.

---

### PostgreSQL — ACID Source of Truth for Financial Data

**Why used:** Financial state transitions require ACID guarantees. A payment must not appear CAPTURED in one read and AUTHORIZED in another. The `version` column enables optimistic locking without holding long-lived row locks. CHECK constraints (`captured_amount <= amount`, `balance >= 0`) enforce financial invariants at the database layer — even a bug in application code cannot violate them. JSONB `metadata` handles extensible merchant-specific data without schema migrations.

**Key config:** Synchronous replication to one standby (RPO=0 for primary failure). PgBouncer for connection pooling (10K application connections → 1K Postgres connections per shard). Virtual shards (1,024–2,048) mapped to physical nodes via consistent hash ring — resharding moves virtual shards, not individual rows. Patroni for automated failover (< 30 seconds). SERIALIZABLE isolation available for the most sensitive operations (account balance reads that drive immediate decisions).

**What breaks without it:** Without ACID, a payment could be marked CAPTURED in one service call and not in another (split-brain state). Without CHECK constraints, an application bug could create a negative balance and the database would happily store it. Without the version column, two concurrent partial captures of the same payment could both succeed and collectively exceed the authorized amount.

---

### Idempotency via Redis SETNX + PostgreSQL Fallback

**Why used:** In distributed systems, clients must retry on failure. Without idempotency, a retry after a successful charge but before the HTTP acknowledgement causes a double charge. Every mutating operation requires an idempotency key. The two-layer approach gives you Redis speed (< 1ms) for the 99.9% case where keys are unique, and Postgres durability for the 0.1% case where Redis restarts.

**Key config:** Redis SETNX stores status (`in_flight`, `completed`, `failed`) + `request_hash` (SHA-256 of request body, to detect reuse with different parameters) with 24-hour TTL. The Postgres `idempotency_keys` table has a UNIQUE constraint on `(merchant_id, key)` and a `SELECT FOR UPDATE` path that serializes concurrent requests to the same key. The idempotency key completion is committed in the same Postgres transaction as the payment state write — this is the critical invariant that prevents ghost charges.

**What breaks without it:** Double charges on network retries. This is a P0 incident — every occurrence results in a real financial loss to a real customer. The `payment.double_charge.count > 0` metric should trigger immediate paging.

---

### Cassandra for Append-Only Audit Log and Time-Series Entries

**Why used:** Payment state transitions are write-once, never updated. Cassandra's LSM-tree storage is naturally append-optimized — writes go to a memtable and are flushed to immutable SSTables. Partition key `payment_id` or `account_id` co-locates all entries for one entity, making range scans (transaction history for an account) single-node operations. RF=3 with LOCAL_QUORUM tolerates one node loss with zero data loss and no sacrifice of read consistency.

**Key config:** Partition key = `account_id` (or `payment_id` in payment processing). Clustering key = `(created_at DESC, id)` — returns entries in reverse chronological order, which is what transaction history queries need. TimeWindowCompactionStrategy (TWCS) with 30-day windows — organizes SSTables by time, making old data easy to tier to cold storage and making time-range queries efficient. TTL=0 (permanent — financial records are never auto-deleted). In the Ledger design, Cassandra is the primary store (not just the audit log) because it handles 350K writes/sec that PostgreSQL cannot sustain.

**What breaks without it:** Using PostgreSQL for an append-only audit log at 4 billion events/day creates table bloat, index maintenance overhead, and write contention that degrades transactional query performance. Cassandra's write path is O(1) regardless of how many entries exist — PostgreSQL's is not.

---

### Kafka for Event Streaming with Transactional Outbox

**Why used:** Decouples the payment state machine from its downstream consumers (webhook delivery, audit logging, AML monitoring, balance materialization, notifications). If webhook delivery is slow or the AML service is down, the payment flow is not blocked. Events are durably stored in Kafka for 7 days — a downstream consumer crash can replay from its last committed offset.

**Key config:** The transactional outbox pattern is mandatory — never publish directly to Kafka from application code. Write an outbox row atomically with the state change in Postgres. Debezium reads the Postgres WAL and publishes to Kafka. This guarantees: if the state change is in Postgres, the event will eventually reach Kafka, even if the application crashes. Partition by `payment_id` or `merchant_id` (not random) to ensure ordering within an entity. Payment Processing: 48 partitions. Stripe Gateway: 96 partitions for webhook-events. Ledger: 500 partitions to support 350K entries/sec consumer throughput.

**What breaks without it:** Direct Kafka publish in application code fails silently when Kafka is unavailable, losing events. Downstream consumers like the webhook dispatcher would never receive the event, and the merchant would never know the payment succeeded — their order fulfillment system would not trigger.

---

### Redis for Idempotency Cache, Balance Cache, and Rate Limiting

**Why used:** Sub-millisecond lookups for hot data. Three distinct use cases: (1) Idempotency SETNX — atomic "set if not exists" with TTL, prevents duplicate in-flight requests. (2) Balance cache — materialized current balance per account/currency with 60-second TTL; serves balance reads without hitting PostgreSQL. (3) Rate limiting — sliding window counters per merchant, per user; INCR + EXPIRE provides atomic counter updates.

**Key config:** Redis Cluster with consistent hashing on the key (e.g., `{merchant_id}:{idempotency_key}` for idempotency, `balance:{account_id}:{currency}` for balances). AOF persistence (Append-Only File) for durability — not durable enough as the sole authoritative store, but prevents losing the cache on a restart. TTLs: idempotency keys 24 hours, balance cache 60 seconds, rate limit windows 1 minute, session tokens 15 minutes.

**What breaks without it:** Balance reads at 3.5M RPS cannot go to PostgreSQL directly — the database would be overwhelmed. Rate limiting without Redis has no shared state across application pods (per-pod rate limits can be trivially bypassed by distributing requests). Idempotency without Redis forces every request to hit PostgreSQL with a SELECT FOR UPDATE, creating write lock contention at scale.

---

### Optimistic Locking via Version Column

**Why used:** Prevents lost update anomalies when multiple processes modify the same payment record concurrently. For example: two partial capture requests arrive simultaneously for the same payment. Both read `captured_amount=0`, both compute `0 + 1000 = 1000`, both attempt to write. Without concurrency control, both writes succeed and `captured_amount` ends up at 1000 instead of 2000 — $1000 is silently lost.

**Key config:** `version BIGINT NOT NULL DEFAULT 0` column on all mutable financial records. Every update increments the version and checks the expected version: `UPDATE payments SET captured_amount = captured_amount + :amount, version = version + 1 WHERE id = :id AND version = :expected_version`. Zero rows updated = concurrent modification — re-read the record and retry with the new version (or return a conflict error). This is strictly preferred over `SELECT FOR UPDATE` because it holds no lock between the read and the write.

**What breaks without it:** Over-captures (more than authorized amount), over-refunds (more than captured amount), and lost concurrent balance updates. The application layer catches this with the version check, but the DB layer's CHECK constraints catch any bugs in the application's version logic. Both together provide defense in depth.

---

### ClickHouse for OLAP, Fraud Features, and AML Velocity

**Why used:** Fraud detection and AML scoring require aggregating billions of rows in real time — questions like "how many cards has this merchant seen in the last hour?" or "how much has this wallet user sent in the last 24 hours?" PostgreSQL takes minutes to answer these on billion-row tables. ClickHouse answers them in under 50 milliseconds using columnar compression and vectorized execution.

**Key config:** Batch insert from Kafka at 1-second micro-batches (not row-by-row). Materialized views compute rolling window aggregates (velocity features like `total_sent_last_1h`, `transfer_count_last_24h`, `unique_recipients_last_7d`) and write results to summary tables. These summary tables are read by the AML Service via Redis (refreshed every 60 seconds from ClickHouse into Redis) — the actual fraud/AML scoring reads Redis (<2ms), not ClickHouse directly.

**What breaks without it:** Without ClickHouse, velocity features for fraud and AML scoring either don't exist (fraud is blind to velocity attacks) or are computed in PostgreSQL (which takes seconds, making real-time scoring impossible, or crashing the database under analytical query load during peak transaction hours).

---

### Circuit Breakers on External Services

**Why used:** Card networks, acquirers, HSM vaults, AML providers, and FX feeds are all external systems that degrade or fail. Without circuit breakers, a slow or failing dependency causes your threads to pile up waiting for timeouts, exhausting the thread pool and taking down your entire service — a cascading failure. Circuit breakers fail fast when a dependency is unhealthy, protecting your service while giving the dependency time to recover.

**Key config:** Three states: CLOSED (healthy — pass all traffic), OPEN (failing — all requests fail fast with "service_unavailable"), HALF_OPEN (probing — 10% of traffic allowed through; if success rate recovers, close; if not, re-open). Threshold: OPEN after 20% error rate in a 60-second window. Recovery probe: after 30 seconds in OPEN. Per-dependency circuit breakers (one per acquirer, one per card network, one per fraud provider) — a Visa network outage doesn't stop Mastercard transactions.

**Special cases:** Fraud engine timeout → fail-open (allow the transaction + flag for manual review), not fail-closed. Blocking all transactions because the fraud service is slow is worse for the business than occasionally allowing a fraudulent one. FX rate feed stale > 2 minutes → serve stale rate with inflated spread; stale > 10 minutes → block FX conversions.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Payment Processing

**The one thing that makes it unique:** It speaks ISO 8583 — the binary wire protocol of the card networks — and connects directly to acquirers. No other problem in this folder does this. The Routing Engine selects the acquirer per transaction based on BIN prefix (first 6-8 digits of the card number map to card type and issuing bank), merchant configuration, real-time acquirer health from circuit breaker state, and cost optimization. The state machine has 13 states, including three chargeback states (CHARGEBACK_INITIATED, CHARGEBACK_LOST, CHARGEBACK_WON) that no other problem needs.

**Two-sentence differentiator:** Payment Processing is the only design in this folder that owns the full acquirer integration — it must speak ISO 8583 (message type 0100 for authorization, 0200/0220 for capture) directly to card networks, maintain persistent TCP connection pools per network, and route each transaction to the optimal acquirer in real time. The 13-state payment machine, multi-acquirer routing engine, and chargeback lifecycle management make this the design you build when you are the payment processor, not just a user of one.

---

### Stripe Gateway (PSP)

**The one thing that makes it unique:** The Connect platform. Stripe is not just a payment processor — it's a platform that enables other platforms to build marketplaces. A destination charge collects money from a buyer, takes a platform fee, and routes the remainder to a seller's connected account. Each connected account has its own KYC, payout schedule, payout holds, and capabilities. No other design in this folder has this multi-tenant, multi-account money routing architecture.

Secondary unique elements: The hosted iframe on a separate PCI-isolated domain (js.stripe.com) means the merchant's server never sees the PAN at all — the merchant is entirely out of PCI scope. Date-based API versioning (e.g., 2024-06-20) with 3-year backward compatibility support means Stripe is a platform, not just an API — you can never break existing integrations.

**Two-sentence differentiator:** Stripe Gateway's unique architecture centers on the Connect marketplace platform, which intermediates between a platform and millions of sellers via destination charges, application fees, per-account KYC, and 7-day payout holds for new accounts. Combined with the hosted iframe design (merchant never enters PCI scope) and date-pinned API versioning with multi-year backward compatibility, this design represents building a developer platform on top of a payment gateway — an entirely different engineering challenge from building the payment gateway itself.

---

### Wallet

**The one thing that makes it unique:** The compliance stack. The Wallet is the only design where regulatory compliance is a first-class driver of the data model — KYC levels (0, 1, 2) directly govern spend limits, AML velocity scoring (gradient-boosted tree model with pre-computed Redis features) is in the critical path of every transfer, and OFAC sanctions screening happens at KYC time, nightly re-screening, and again on every payout above $3,000. No other design in this folder has `kyc_level`, `reserved_balance`, or `sanctions_screenings` as core tables.

The reserved balance model is also unique: when an ACH debit is initiated (top-up from bank account), the funds are not yet cleared, so a `reserved_balance` field holds them in limbo — the `available_balance` (a PostgreSQL generated column: `balance - reserved_balance`) prevents the user from spending money that hasn't actually arrived yet.

**Two-sentence differentiator:** Wallet is the only design in this folder where regulatory compliance is a first-class architectural driver — KYC tiers govern spend limits, real-time AML scoring with velocity features is in the critical path of every transfer, and OFAC sanctions screening runs at multiple checkpoints including nightly re-screens. The multi-currency FX model with 60-second rate locks, reserved balance for pending ACH debits, and 2FA enforcement thresholds make this the design you build when you are a regulated financial institution, not just a software company that handles payments.

---

### Ledger

**The one thing that makes it unique:** Double-entry bookkeeping at financial-database scale with tamper detection and regulatory archival. Every transaction must have matching debits and credits (enforced at both application validation AND a database CHECK constraint). Each entry includes a SHA-256 hash of the previous entry's hash plus the current entry's fields — a cryptographic chain that makes silent modification of any historical entry detectable. Hourly balance snapshots make point-in-time balance queries efficient (nearest snapshot + bounded range scan) without scanning the entire history. S3 Object Lock in Compliance mode (WORM) means even AWS cannot delete archived ledger entries — satisfying SOX 7-year retention with mathematical certainty.

This is the only design where Cassandra is the primary store (not just the audit log) — because 350K inserts/second exceeds PostgreSQL's capacity even with sharding.

**Two-sentence differentiator:** Ledger is the only design in this folder focused on accounting correctness guarantees at financial-database scale — the double-entry invariant is enforced at both write-time validation and a database CHECK constraint, every entry participates in a SHA-256 hash chain for tamper detection, and cold data is archived to WORM S3 Object Lock that even the account owner cannot delete. At 350K entries/second and 110 trillion entries over 10 years, Cassandra is the primary store (not PostgreSQL) — this is the design you build when you need a financial ledger that serves as the legal record of truth, not just an operational database.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface (2-4 sentences each)

**Q1: What is an idempotency key and why is it required for payment APIs?**

An idempotency key is a client-generated unique string (typically a UUID) included in every mutating API request. If the client sends the same request twice with the same key — which happens on network retries — the server detects the duplicate via Redis SETNX and returns the original response without re-executing the operation. For payments, this is the primary defense against double charges. Without it, a client that retries after a network timeout would charge the customer twice, and neither the client nor the server would know which response corresponds to the actual charge.

**Q2: Why do you store amounts as integers instead of floats?**

Floating-point numbers (IEEE 754) cannot represent all decimal values exactly — the classic example is `0.1 + 0.2 = 0.30000000000000004` in most languages. For financial data, even a fraction-of-a-cent rounding error compounds across billions of transactions into real money. All monetary amounts are stored and transmitted as integers in minor currency units (cents for USD, pence for GBP). The currency code determines the implied decimal point — USD has exponent 2 (÷100), JPY has exponent 0 (no minor unit), KWD has exponent 3. This eliminates floating-point errors entirely at the data layer.

**Q3: What is the difference between authorization and capture?**

Authorization is a real-time request to the issuing bank to reserve (hold) funds on a cardholder's account. The bank approves and places a hold, but no money actually moves. Capture is the instruction to the acquirer to actually move the money and settle the transaction. The two-phase model exists because merchants often need to authorize at order time but capture when goods actually ship — and the capture amount can differ from the authorized amount (for example, a hotel pre-authorizes $200 but the final bill is $185). Authorization holds expire if not captured (Visa: 7 days for most MCCs).

**Q4: What is PCI DSS and why does it affect the architecture?**

PCI DSS (Payment Card Industry Data Security Standard) is a set of 12 security requirements for any system that stores, processes, or transmits cardholder data (the PAN — Primary Account Number). Level 1 applies to processors handling over 6 million transactions per year and requires an annual on-site audit. The architectural impact is that we minimize PCI scope: the card number is tokenized client-side (via a hosted iframe on our PCI-isolated domain), PANs are stored only in an HSM-backed vault, and all other services see only opaque tokens. Every system outside the vault that handles raw PANs would need to meet all 12 PCI requirements.

**Q5: What is a chargeback and how does it differ from a refund?**

A refund is merchant-initiated — the merchant voluntarily returns funds to the cardholder, typically for returns or errors. A chargeback is cardholder-initiated — the cardholder asks their issuing bank to reverse a charge, and the bank debits the merchant's account unilaterally, often with an additional dispute fee ($15-50). Chargebacks can occur for legitimate fraud, as well as for "friendly fraud" (the cardholder received the goods but disputes the charge anyway). Merchants can fight chargebacks by submitting evidence to the card network. Chargeback rates above 0.5% can result in fines or a merchant's ability to accept cards being revoked.

**Q6: Why do you use cursor-based pagination instead of offset pagination for transaction lists?**

Offset pagination (`LIMIT 25 OFFSET 200`) requires the database to scan and discard 200 rows before returning 25, which becomes exponentially slower as offset increases. More critically, if a new transaction is inserted between page 1 and page 2 requests, offset pagination shifts every row — page 2 might return a duplicate of the last row from page 1. Cursor-based pagination uses a stable, indexed column (typically `created_at` + `id`) as the cursor. The query is `WHERE created_at < :cursor ORDER BY created_at DESC LIMIT 25`, which always executes an efficient index range scan regardless of page number and is stable under concurrent inserts.

**Q7: What is double-entry bookkeeping and why does a payment system need it?**

Double-entry bookkeeping requires that every financial event produces two (or more) ledger entries that sum to zero — a debit on one account and an equal credit on another. When a merchant receives $97 from a $100 charge, the system records: debit Cash $100 (asset increases), credit Merchant Payable $97 (liability increases), credit Revenue-Fees $3 (revenue increases). The sum is zero: 100 = 97 + 3. This invariant means the entire book is always in balance — if it isn't, there's a bug. It also provides a full audit trail where every dollar can be traced from source to destination, which is required for financial reporting and regulatory audits.

---

### Tier 2 — Deep Dive (why + trade-offs)

**Q1: Walk me through exactly how you prevent a double charge when the server crashes after the card is charged but before the response is returned.**

The ghost charge scenario is solved by the atomic commit pattern. We write two things in the same Postgres transaction: the payment record (status=AUTHORIZED) and the idempotency key row (status=completed, response_body=...). If the server crashes between the card network call and the Postgres commit, the transaction rolls back — the idempotency key is not marked complete. When the client retries with the same idempotency key, Redis SETNX succeeds (key not found), and we re-attempt the charge. At the card network level, we include the original `network_transaction_id` in the retry — Visa and Mastercard use this to recognize the retry and return the original authorization code without creating a new charge. The trade-off here is that we accept a small window of uncertainty (between the network call and the DB commit) that is resolved by the network's own deduplication — this is unavoidable in any distributed system where the charge goes to an external service.

**Q2: Why do you use Cassandra instead of PostgreSQL for the audit log, and what are the failure modes of each?**

Cassandra's LSM-tree architecture is write-optimized: writes go to an in-memory memtable and a commit log, then are flushed to immutable SSTables. This means consistent write throughput regardless of dataset size. At 4 billion audit events per day (4B/86400 = ~46K writes/sec), Cassandra handles this comfortably on a small cluster. PostgreSQL's B-tree indexes must be maintained on every insert — at 46K inserts/sec on a table that grows to trillions of rows, index maintenance overhead degrades write performance significantly. The failure mode difference: Cassandra's RF=3 with LOCAL_QUORUM tolerates one node loss transparently. PostgreSQL primary failure requires failover (20-30 seconds downtime) even with a synchronous replica. The trade-off: Cassandra offers no transactions (you cannot atomically insert an audit entry and a payment record in the same Cassandra operation) and limited query flexibility (no ad-hoc JOINs, no aggregations), which is why we keep the operational payment records in PostgreSQL and only use Cassandra for the append-only audit stream.

**Q3: How does the transactional outbox pattern work, and why is it better than publishing to Kafka directly from the application?**

Direct Kafka publish from application code has a critical failure mode: the Postgres commit succeeds (payment is marked CAPTURED) but the Kafka publish fails (Kafka is briefly unavailable or the application crashes between the two calls). Result: the webhook event for `charge.captured` is never sent. The merchant never knows the payment succeeded. The transactional outbox solves this by making the event durable before Kafka: write an outbox row in the same Postgres transaction as the state change. Debezium connects to the Postgres WAL (binlog equivalent) and reads the outbox row as a CDC event, then publishes to Kafka. The critical property: Debezium reads from the WAL only after the transaction commits, and it maintains its own position in the WAL — if Debezium restarts, it replays from the last committed position, guaranteeing at-least-once delivery. The trade-off: additional dependency on Debezium; additional latency (WAL reading adds ~10-100ms vs direct publish). For payments, the correctness guarantee is worth the latency cost.

**Q4: How do you achieve 3.5 million balance reads per second without hitting the database?**

The materialized balance pattern. After each ledger entry is written, the Balance Materializer (a Kafka consumer) does two things: (1) updates `materialized_balances` in PostgreSQL atomically (durable, authoritative current balance), and (2) writes `balance:{account_id}:{currency}` to Redis with a 60-second TTL. Balance read requests go to Redis first — a single Redis node handles ~100K GET operations per second, and a Redis Cluster of 6 nodes handles over 1M. On cache miss (expired TTL or first read), the read falls through to PostgreSQL `materialized_balances` (O(1) lookup, not a scan). The critical design choice: the balance in Redis is at most 60 seconds stale for display purposes. But the balance used for the actual deduction at write time is always the PostgreSQL value with an optimistic lock — the Redis balance is only for showing the user their current balance, never for authorizing a spend. These two requirements have different consistency needs and should be served differently.

**Q5: How does the Connect payout hold work and why is it needed?**

When a new connected account (a seller on a marketplace platform) signs up, their payouts are held for 7 days before being released to their bank account. This hold exists because of chargeback timing. A buyer can dispute a charge up to 120 days after the transaction. If we immediately paid out the seller and a chargeback arrives 30 days later, the platform has already given the money away. The 7-day hold gives the platform a buffer to detect clearly fraudulent patterns (a new account with unusually high charge volume) before the money is gone. During the hold, the seller's balance is in a "pending" state in the platform's ledger — the money is in the platform's bank account, not the seller's. After the hold, a payout is scheduled to the seller's bank account via ACH. The trade-off: legitimate new sellers have worse cash flow. Platforms can configure shorter holds for verified, lower-risk sellers as they build a track record.

**Q6: How do you compute balance at a specific historical point in time efficiently?**

Naive approach: scan all ledger entries for the account from genesis to the target timestamp and sum them. Problem: a busy merchant account might have 10 million entries — this scan takes seconds. Efficient approach: hourly balance snapshots. Every hour, a background job reads the current materialized balance and writes a `balance_snapshots` row: `{account_id, currency, balance, snapshot_at, entry_id_at_snapshot}`. A point-in-time query for timestamp T becomes: (1) find the most recent snapshot before T (index scan on `snapshot_at DESC`), (2) scan only the Cassandra entries between the snapshot's `entry_id_at_snapshot` and T (at most 1 hour of entries). The bounded range scan is fast regardless of total account history. Trade-off: storage overhead for snapshot rows (minimal — one row per account per hour) and a small accuracy window (up to 1 hour behind for the snapshot, then exact from the entry scan). For SOX audit and tax reporting purposes, "balance as of end-of-day" only needs end-of-day snapshots — hourly is more than sufficient.

**Q7: How do you verify that nobody has tampered with the ledger?**

The cryptographic hash chain. Each ledger entry includes two fields: `previous_entry_hash` (SHA-256 of the previous entry's hash) and `entry_hash` (SHA-256 of `previous_entry_hash || account_id || created_at || id || amount || direction`). A tamper-detection background job (nightly) recomputes the hash chain from the first entry forward and compares each computed hash against the stored `entry_hash`. Any modification to any field of any historical entry breaks all subsequent hashes in the chain — the mismatch is immediately visible. For efficiency, hourly checkpoints store the cumulative hash at each hour boundary — the nightly verifier can verify each hour's entries independently and in parallel. A mismatch at any checkpoint is a P0 alert (potential SOX violation). The trade-off: hash chain verification is computationally expensive at 350K entries/sec (computing SHA-256 for every entry). The nightly batch job verifies historical data while the live write path only computes the hash at write time — reads and verification are decoupled.

---

### Tier 3 — Staff+ Stress Tests (reason aloud)

**Q1: We're processing 50K TPS and our primary PostgreSQL shard for large merchants is getting hot. What do you do?**

I think about this in three layers. First, read-side: most of the load is GET requests for payment status and merchant reporting. I ensure all read traffic goes to read replicas. A hot merchant — say, Amazon — gets a dedicated read replica cluster. Large merchant IDs that account for more than 5% of shard traffic get detected by our monitoring (shard CPU + QPS per `merchant_id`) and we provision dedicated shards for them rather than sharing. Second, write-side: the actual payment authorizations. At 50K TPS total, we have ~8 shards handling ~6K TPS each. A single merchant at 10K TPS would overflow a shard. The solution is dedicated sharding: the largest merchants each get their own physical shard. The virtual shard mapping (1,024 virtual → physical) means this is a configuration change, not a data migration. Third, if we've done all of the above and still have a problem, I'd look at whether we can move to CockroachDB or Spanner for the specific hot accounts — distributed SQL with automatic sharding and Raft consensus handles the hot key problem natively, at the cost of ~3-5ms higher per-transaction latency from consensus rounds. For Stripe-scale merchants, that latency trade-off is worth it.

**Q2: A compliance audit reveals that we have ledger entries that don't balance — the sum of debits doesn't equal the sum of credits for some transactions. How do you investigate and fix this?**

This is a P0 incident. I would start by establishing the scope: run a query against `ledger_transactions` where `total_debit != total_credit` (this is the generated column — if these exist, something bypassed the CHECK constraint, which should be impossible but let's verify). Simultaneously I'd query the raw entries by `transaction_id` to see which accounts are affected. For the investigation: first, check the application deployment history around when these entries appeared — was there a deploy that introduced a bug in the entry validation? Second, check whether the hash chain verifier caught these (it should have — unbalanced entries break the hash). Third, identify the source system (`source_system` column in `ledger_transactions`) to know which service posted them. For remediation: ledger entries are immutable, so we cannot modify them. The fix is a reversal: for each unbalanced transaction, we post a new transaction that is the exact equal-and-opposite (swapping debits and credits), restoring balance. Then we post a correct replacement transaction. Each of these correction transactions is a new, balanced ledger entry — the original corrupted entries remain permanently visible in the audit trail, which is actually required for regulatory purposes. Finally, I'd add a compensating control: verify balance integrity hourly (not just nightly) for high-risk source systems until the root cause is fixed.

**Q3: Stripe calls you and says their fraud ML model is down for the next 2 hours. What do you do with payment authorization flow, and what metrics do you watch?**

I think about the risks in both directions. Failing closed (block all payments) loses revenue at a rate we can calculate: if average transaction value is $100 and we're at 100K TPS, 2 hours of downtime costs roughly $720 billion in GMV — clearly not acceptable. Failing open (allow all payments) with no fraud detection exposes us to fraud losses, but the 2-hour window limits blast radius. My decision: fail-open, but with compensating controls. (1) Fall back to the rules-only engine (no ML, just deterministic rules — velocity limits, BIN blacklists, geolocation anomalies). Rules-only is 80% as effective as ML for catching obvious fraud. (2) Tighten velocity rules during the outage: reduce the per-card transaction limit from 10/hour to 3/hour. (3) Flag all transactions processed during the outage for post-hoc ML scoring once the model is restored — hold payouts for flagged merchants pending review. (4) Monitor fraud signals in real time: chargeback rate, decline rate from issuing banks (banks do their own fraud scoring), velocity anomalies. If any metric spikes more than 3 standard deviations above baseline, escalate to manual review queue rather than auto-approve. The metrics I watch: chargeback rate (should stay below 0.5%), `outcome_risk_level=highest` rate (our risk signal), issuer decline rate (if issuers are seeing fraud and declining, we should see decline rate spike), and post-authorization dispute rate.

**Q4: You need to add a new column to the `payments` table in PostgreSQL without downtime. The table has 5 billion rows. How do you do it?**

The expand-contract pattern. PostgreSQL's behavior with DDL operations is the key: adding a nullable column with no default is a metadata-only operation — it returns instantly regardless of table size (Postgres 11+ stores the default separately, so even adding a nullable column with a DEFAULT is instant). Adding a NOT NULL column without a default requires a full table rewrite — that takes hours and locks the table. So: Phase 1 (expand): `ALTER TABLE payments ADD COLUMN new_field TEXT` — nullable, no default, instant. Deploy this with no application changes. Phase 2 (backfill): background job updates rows in batches of 1,000: `UPDATE payments SET new_field = compute_value(id) WHERE id BETWEEN :start AND :end AND new_field IS NULL`. Use `pg_sleep(10ms)` between batches to avoid overwhelming the primary during business hours. This takes hours to days depending on the table size. Phase 3 (application code): deploy code that writes to the new column on all new writes. Phase 4 (constraint): once backfill is 100% complete (verify with `WHERE new_field IS NULL`), add the NOT NULL constraint — in Postgres 12+, this can be done with `ALTER TABLE payments ADD CONSTRAINT payments_new_field_not_null CHECK (new_field IS NOT NULL) NOT VALID` first (instant), then `ALTER TABLE payments VALIDATE CONSTRAINT payments_new_field_not_null` (scans the table but takes only a share lock, not an exclusive lock, so reads and writes continue). Phase 5 (contract): remove old columns that are no longer needed. The entire operation is zero-downtime because no step requires an exclusive table lock that would block reads or writes.

**Q5: How would you design the payment system to support a "pay on delivery" model where the charge only executes after a delivery confirmation event arrives from a logistics system?**

This is fundamentally an async saga. The flow: (1) At checkout, create a payment intent with `capture_method=manual` — this authorizes the card (reserves funds) but does not capture. The authorization hold lasts 7 days (Visa) or 30 days (Mastercard). Return the order to the customer with status "payment reserved." (2) The logistics system publishes `delivery.confirmed` events to our Kafka (or we poll their API). (3) A saga coordinator service consumes delivery events, looks up the associated payment intent by `order_id` (stored in `payment_intent.metadata`), and calls `POST /v1/payments/:id/capture`. (4) If the authorization has expired before delivery is confirmed (order took more than 7 days), the capture attempt will be declined by the acquirer with "authorization expired." The saga must handle this: re-authorize the card (which requires the customer's payment method to be on file — saved vault token) and then capture immediately. (5) If delivery never occurs (order canceled), call `POST /v1/payments/:id/cancel` to void the authorization and release the hold. The critical design decisions: saved vault token is required (so we can re-authorize without customer interaction); the saga must handle the auth-expired case and re-authorize; idempotency keys must be provided for both the capture and the re-authorization to prevent double-charges on retry. This is the same pattern used by hotels (pre-auth at booking, final capture at check-out) and car rentals (pre-auth at pickup, capture at return).

---

## STEP 7 — MNEMONICS

### Memory Trick: "VIRCCO" — The 6 must-say things in any payments interview

**V** — Version column (optimistic locking to prevent concurrent modification)
**I** — Idempotency key (Redis SETNX fast path + Postgres fallback; write atomically with payment record)
**R** — Redis (balance cache 60s TTL, rate limiting, idempotency cache — never authoritative)
**C** — Cassandra (append-only audit log, RF=3 LOCAL_QUORUM, TTL=0)
**C** — Circuit breaker (per external dependency: acquirer, card network, fraud service, FX feed)
**O** — Outbox pattern (transactional outbox → Debezium → Kafka; never direct publish)

Say all six of these unprompted in the first 20 minutes and you've immediately shown senior-level depth.

### Opening One-Liner for Any Payments Question

"The fundamental challenge in payments is that money must never be created or destroyed by a software failure — so the entire architecture is built around three guarantees: the charge happens exactly once regardless of network failures (idempotency), the balance can never go below zero regardless of concurrent requests (atomic check-and-decrement), and every state change is durably recorded before we acknowledge it (write-before-respond)."

This one sentence covers idempotency, consistency, and durability — the three pillars of payments. State it at the start, then build the architecture as the implementation of each guarantee.

---

## STEP 8 — CRITIQUE

### Well-Covered in the Source Material

The source files are exceptionally thorough in several areas. The idempotency protocol is covered end-to-end with the exact Redis SETNX + Postgres FOR UPDATE algorithm, including the ghost charge edge case and the request_hash mismatch detection. The data models are production-quality with realistic SQL including ENUM types, CHECK constraints, generated columns, and all indexes. The two-phase auth/capture model is explained including ISO 8583 message types (0100, 0200/0220), hold window differences between networks, and split capture for partial fulfillment. The hash chain tamper detection in the Ledger design is unusually complete — most interview prep material ignores this entirely. The Connect payout hold rationale (chargeback timing) is well-explained rather than just stated as a fact.

### Missing, Shallow, or Worth Supplementing

**3D Secure (3DS)** is mentioned briefly in one follow-up answer but not architecturally designed. Senior engineers should know: 3DS 2.x adds a Directory Server lookup and optional ACS redirect between authorization and completion. Frictionless 3DS (device fingerprint authentication, no cardholder interaction) is now the majority of EU transactions under PSD2's Strong Customer Authentication requirement. The PaymentIntent `requires_action` state in the Stripe data model exists specifically for the 3DS redirect flow. This is increasingly common in interviews about European payment systems.

**ACH / bank transfer flow** is noted as out of scope in most problems but is a real common interview variant. Key points to add: ACH is asynchronous (1-5 business days to settle), NACHA file format submitted to ODFI, R-code returns (R10 = unauthorized debit) can arrive up to 60 days later and require balance reversal, Plaid for instant bank account verification bypasses the traditional micro-deposit wait. The `reserved_balance` field in the Wallet design addresses the "pending ACH" case but the full NACHA integration isn't detailed.

**Fraud ML model architecture** is treated as a black box throughout. In a Staff+ interview, you may be asked: "How does the model stay current?" The answer involves: feature store (ClickHouse computes velocity features, Flink for real-time), model versioning (blue-green deployment with shadow mode testing), and feedback loops (chargebacks and confirmed fraud are labeled training data fed back into model training). The fail-open circuit breaker behavior is well-covered, but the model lifecycle is not.

**GDPR right to erasure** is listed as a compliance requirement in the NFRs but never architecturally addressed. In interviews, this is a trap. For financial data, GDPR right to erasure conflicts with PCI DSS (retain card tokens for 7 years) and SOX (retain ledger entries). The correct answer: PII that is not financial (email, name, device fingerprint) can be deleted. Financial records required for regulatory retention cannot be deleted — GDPR has an explicit exception for legal obligations. The architecture separates PII (users table, can be anonymized) from financial records (payments, ledger entries, cannot be deleted).

### Senior Probes You Should Be Ready For

**"What's the difference between a vault token and a network token?"** Vault token: opaque UUID we issue, maps to PAN in our HSM vault, only useful with our system. Network token: issued by Visa (VTS) or Mastercard (MDES), replaces PAN in the ISO 8583 authorization message, domain-bound to a specific merchant so it's useless if stolen by a different merchant, and automatically updated when the underlying card is re-issued (eliminating expiry declines).

**"If your Postgres primary fails during a capture, how do you know whether the capture succeeded at the acquirer?"** The acquirer response (success) was received, but the Postgres write didn't commit. On recovery, you query the acquirer via their API for the status of the `network_transaction_id`. If the acquirer shows captured, you write the state to Postgres and proceed. If the acquirer shows not captured, you retry. The `network_transaction_id` is the key that allows idempotent reconciliation with the external system.

**"How do you handle a merchant attempting to over-refund?"** Three-layer defense: (1) Application check before sending to acquirer. (2) Optimistic lock version check prevents concurrent refunds from collectively exceeding captured amount. (3) Database CHECK constraint `refunded_amount <= captured_amount` as absolute backstop. The acquirer will also reject over-refund at the network level — four independent layers.

### Common Traps in Payments Interviews

**Trap 1: Proposing DynamoDB or a NoSQL database for the core payment records.** DynamoDB with eventual consistency cannot enforce `balance >= 0` or prevent double-capture. If you suggest it, the interviewer will ask: "How do you prevent a negative balance?" and you'll have no good answer. Use PostgreSQL for ACID financial records.

**Trap 2: Saying "I'll use a database transaction to make the payment atomic."** A database transaction keeps the Postgres write atomic, but it does nothing for the external call to the card network. The card network call is outside the transaction. The question "what if the network call succeeds but the DB write fails?" requires the idempotency + network deduplication answer, not "I'll wrap it in a transaction."

**Trap 3: Proposing a distributed lock (Redis SETEX) to prevent double-spend.** A distributed lock held across the balance check and the debit is a TOCTOU-safe pattern, but it holds the lock for the duration of business logic execution. Under high concurrent load on the same account, this creates a thundering herd on the lock. The correct answer is the optimistic lock: no lock held between read and write; the version column detects the conflict after the fact and the caller retries.

**Trap 4: Forgetting that "Kafka guarantees" don't help if you publish to Kafka before committing to Postgres.** If you publish to Kafka and then the Postgres write fails, Kafka has an event for a payment that doesn't exist. The transactional outbox pattern is the only correct answer — write to Postgres (durable) first, then Kafka consumes the committed change via CDC.

**Trap 5: Saying the ledger uses PostgreSQL as the primary store without quantifying the write throughput.** At 350K entries/sec, PostgreSQL cannot keep up even with sharding. If you propose PostgreSQL for the primary ledger, the interviewer will ask "at what point does this break?" — and the answer is: it breaks before you need it. Always mention Cassandra for write-optimized append-only workloads at this scale.

---

*End of Pattern 10: Payments Interview Guide*
