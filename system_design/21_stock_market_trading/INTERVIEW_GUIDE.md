# Pattern 21: Stock Market Trading — Complete Interview Guide

> Reading Pattern 21: Stock Market Trading — 5 problems, 9 shared components

---

## TABLE OF CONTENTS

1. [Mental Model](#step-2-mental-model)
2. [Interview Framework](#step-3-interview-framework)
   - 3a. Clarifying Questions
   - 3b. Functional Requirements
   - 3c. Non-Functional Requirements
   - 3d. Capacity Estimation
   - 3e. High-Level Design
   - 3f. Deep Dive Areas
   - 3g. Failure Scenarios
3. [Common Components Breakdown](#step-4-common-components-breakdown)
4. [Problem-Specific Differentiators](#step-5-problem-specific-differentiators)
5. [Q&A Bank](#step-6-qa-bank)
6. [Mnemonics & Memory Anchors](#step-7-mnemonics--memory-anchors)
7. [Critique](#step-8-critique)

---

## STEP 2: Mental Model

### Core Insight

**The entire financial stack is a pipeline of latency budgets.** Every component hands off to the next, and the total end-to-end latency is the sum of all hops. The architecture of each layer — whether you use kernel bypass, in-memory state, UDP vs TCP, a WAL or Kafka — is directly determined by what latency budget that layer is allowed to consume.

### Real-World Analogy

Think of a stock exchange like a **very fast auction house with a court reporter attached**. The auctioneer (matching engine) accepts bids and asks, announces matches in microseconds, and every word is transcribed in real time for regulators who may audit the tape years later. The audience (market makers, HFT firms) sit as close as physically possible to the auctioneer — measured in feet of fiber cable — because every microsecond matters. Meanwhile, retail investors watch on TV from home with a slight delay and that's perfectly acceptable for their use case.

### Why This Problem Is Hard

Three tensions compound simultaneously:

- **Speed vs. Durability**: The matching engine must be both the fastest thing in the building (< 10μs) and never lose a single order or fill (zero data loss). These normally conflict — fast means in-memory, durability means writing to disk.
- **Fairness vs. Complexity**: Financial systems must treat all participants equally (same price gets same priority), but the real-world systems that achieve this — single-threaded engines, equal fiber lengths, sequence-number-ordered WALs — are highly specialized and operationally fragile.
- **Scale vs. Consistency**: Strong consistency (no double-fills, no negative balances) is non-negotiable for money. But achieving it at millions of events per second without distributed locks requires specific patterns (optimistic locking, single-threaded engines, idempotency keys) that each add their own edge cases.

---

## STEP 3: Interview Framework

### 3a. Clarifying Questions

These are the questions you MUST ask before touching the whiteboard. Each one changes the architecture materially.

---

**Q1: Who are the users — retail investors, institutional traders, or exchange members?**

What it changes:
- **Retail** → REST/WebSocket APIs, fractional shares, PDT rule, PFOF routing, consumer-grade UX, 500ms latency acceptable
- **Institutional (OMS)** → FIX protocol, TWAP/VWAP algo execution, compliance rules engine, block allocation, 50ms latency
- **Exchange members (HFT)** → binary protocol (OUCH/ITCH), kernel bypass networking, < 500μs latency, co-location

What candidates skip (red flag): jumping straight to "we need a matching engine" without asking if this is a retail broker or an exchange. These require almost completely different architectures.

---

**Q2: What order types need to be supported?**

What it changes:
- **Market + Limit only** → simple matching logic
- **Add Stop, Stop-Limit** → need a secondary "stop book" that activates orders when price crosses a threshold; triggers re-entry into the main book
- **Add Iceberg** → hidden reserve quantity logic; peek-qty management in the order book
- **Add Opening/Closing Auction** → entire batch-auction subsystem; fundamentally different from continuous matching

---

**Q3: What's the latency requirement — microseconds, milliseconds, or sub-second?**

What it changes:
- **< 1ms (HFT / exchange co-location)** → kernel bypass (DPDK/RDMA), in-memory only, no GC (off-heap structs), single-threaded engine
- **< 50ms (institutional OMS)** → standard Linux networking acceptable, JVM with GC tuning, PostgreSQL on hot path acceptable
- **< 500ms (retail)** → cloud infrastructure, standard databases, microservices with network hops are all fine

What candidates skip: not asking this question and defaulting to "we'll use Kafka for everything." Kafka is 2–5ms minimum. Putting Kafka on the order-to-fill path for an exchange would be disqualifying.

---

**Q4: What does "best execution" mean here — and do we route to dark pools?**

What it changes:
- **Exchange**: only matches against the book it owns; no routing decisions
- **Retail broker**: routes to market makers via PFOF; must document best execution for SEC Rule 606
- **Institutional OMS**: Smart Order Router splits large orders across dark pools + lit venues to minimize market impact; requires a market impact model and venue quality scoring

---

**Q5: What are the compliance and regulatory requirements?**

What it changes:
- **SEC Rule 17a-4 / FINRA 4511**: 7-year immutable retention for all order events → forces an append-only audit trail (Parquet + S3 WORM or Cassandra append-only)
- **CAT (Consolidated Audit Trail)**: exchange must submit every order event to SEC's CAT system within 1 second → async CAT Reporter service consuming from Kafka
- **PDT rule**: retail broker with accounts < $25K → Redis rolling counter per account + PostgreSQL audit log
- **MiFID II (EU)**: if serving European clients → best execution documentation, transaction reporting to national regulator within T+1

What candidates skip: not mentioning regulatory retention. A senior interviewer will probe "where do you keep audit data for 7 years and how do you make it tamper-evident?"

---

**Q6: Multi-asset or equities only?**

What it changes:
- **Equities only** → 10,000 symbols, simpler pricing (decimal price, integer shares)
- **Add options** → 500,000 contracts (OPRA feed is 10M msg/sec vs 2M for equities); need Greeks computation; options approval tiers for retail
- **Add futures** → different clearing (CME clearinghouse, not DTCC); margin model is different; different matching priorities (pro-rata is common for interest-rate futures vs FIFO for equities)

---

### 3b. Functional Requirements

State these clearly and explicitly in the interview. The way you scope this signals seniority.

**Core features (always in scope for any variant):**
- Order placement: Market, Limit orders (at minimum)
- Order lifecycle: view open orders, cancel, view history
- Real-time fills/execution notifications
- Position tracking (what you own, at what cost basis)
- Cash/buying power management (no overdrafts)

**Scope decisions to state explicitly:**

| Decision | In Scope | Out of Scope |
|---|---|---|
| Asset classes | US equities + ETFs | Options, futures, crypto, FX, fixed income |
| Settlement | Track T+1 settlement status | Actually moving money (custodian handles that) |
| Risk | Pre-trade risk (buying power, position limits) | Post-trade risk, portfolio VaR, margin calls |
| Market data | Real-time quotes, NBBO | Fundamental data, news, research |
| Analytics | Execution quality (TCA) | Portfolio optimization, tax-loss harvesting |

How to state them clearly: "I'll focus on US equities order entry, matching, and real-time portfolio updates. I'm scoping out settlement, options, and fundamental data to keep this tractable — happy to go deeper on any of those if you want."

---

### 3c. Non-Functional Requirements

**Critical NFRs to state (and how to derive them):**

**Availability**
- **99.999% during market hours** (9:30 AM – 4 PM ET) = < 5.25 minutes downtime per year
- How to derive it: "Market hours are 6.5 hours/day, 252 trading days/year. A single 5-minute outage during open is a regulatory incident and potentially a fine. 99.999% is the industry standard for exchange-grade systems. For retail brokers, 99.99% is more common — that's ~52 minutes/year which sounds like a lot but all of it should be in off-hours maintenance windows."

**Latency (state all three tiers)**
- Exchange/HFT co-location path: **< 1μs network, < 10μs matching engine internal**, < 500μs end-to-end
- Institutional OMS: **< 50ms** order creation to broker submission
- Retail: **< 500ms** end-to-end to exchange acknowledgment

Trade-off baked in: achieving the HFT latency budget requires kernel bypass networking and single-threaded engines. This trades operational simplicity for performance. A retail platform doesn't need any of that — standard cloud infrastructure is fine.

**Durability**
- **Zero data loss for fills and orders.** Every fill represents real money changing hands. "Eventual durability" is not acceptable.
- How to derive it: "If we lose a fill event, a user's portfolio is wrong. If we lose an order event, a regulatory audit will have gaps. Both are financially and legally unacceptable. Therefore: WAL-backed or ACID-backed for all financial state."

**Consistency**
- **Strong consistency for financial balances and positions** — no double-fills, no negative balances
- Eventual consistency is acceptable only for analytics (TCA, performance attribution, charts) and reporting

**Fairness**
- Strict **price-time priority** — same price level, first submitted = first filled
- No preferential treatment at the same price level
- Physical fairness for exchange co-location: equal fiber cable length to all co-located members

**Idempotency**
- Every write operation must be safe to retry. Network timeouts cause retries; retries must not create duplicate fills or double-submitted orders.

---

### 3d. Capacity Estimation

**The formula anchor (memorize this):**

```
Peak order event rate = (total symbols) × (orders per symbol per second at peak)
                      = 10,000 symbols × 50 events/sec = 500,000 events/sec peak
```

**Anchor numbers to internalize:**

| Metric | Number | What it implies |
|---|---|---|
| US equities symbols | 10,000 | Partition matching engines by symbol group (100 symbols/engine = 100 engines) |
| OPRA options peak | 10M msg/sec | Options feed needs dedicated hardware; equities feed is only 2M/sec |
| Peak equities msg rate | 2M msg/sec | 200 MB/s ingest; needs 10 Gbps NICs at minimum |
| HFT order cancel rate | ~70% of orders cancelled before fill | Most order volume is quote refresh, not real trading intent |
| Retail users active at open | 100K simultaneous orders | Stateless order service auto-scales; risk engine Redis handles 1M+ ops/sec |
| WebSocket fan-out | 1M connections × 10 symbols × 2 updates/sec = 20M pushes/sec | Redis Pub/Sub as intermediary; horizontal scaling of WS nodes |
| Audit log (exchange) | ~210 GB/day raw → ~370 TB over 7 years | Parquet + S3; columnar compression ~10:1; manageable |
| Tick archive (market data) | ~28 TB/day raw → ~3.5 PB over 5 years | Parquet + S3; ClickHouse for queryable analytics slice |

**What the math tells you about the architecture:**

- 500K events/sec through a matching engine on a single core: LMAX Disruptor achieves ~600M ops/sec per core. We're well within budget. The bottleneck is **WAL write throughput** not CPU.
- 10M OPRA messages/sec = 1 GB/s ingest. This means options feed handlers need dedicated physical machines and NICs with kernel bypass. You cannot process this on a shared compute cluster.
- 20M WebSocket push deliveries/sec: no single process can maintain 1M TCP connections. This forces horizontal scaling of WebSocket nodes with Redis Pub/Sub as the fan-out mechanism.
- 7-year audit storage at 370 TB: this is not "Big Data" — it fits on a dozen commodity NVMe drives with Parquet compression. Don't over-engineer the storage layer.

---

### 3e. High-Level Design

**The 5 universal components across all stock market trading problems:**

```
[Client / Member] → [Order Entry / API Gateway] → [Risk Gateway]
                                                        ↓
                                               [Order Sequencer]
                                                        ↓
                                            [Matching Engine / Router]
                                                        ↓
                         [Fill Processor] → [Position Update] → [Notification]
                                ↓
                         [Audit Log / Kafka fan-out]
```

**Key decisions at each layer:**

**1. Order Entry / API Gateway**
- Retail: REST + JWT over HTTPS; standard cloud load balancer
- Institutional: FIX protocol over dedicated TCP; per-session sequence numbers
- Exchange HFT: binary protocol (OUCH) + kernel-bypass networking (DPDK)

**2. Risk Gateway**
- Pre-trade checks: buying power, position limits, price collar (fat-finger protection)
- Always **fail-closed** — if risk check is unavailable, reject the order
- All checks must be in-memory (Redis for balance/position state); no DB reads on the critical path
- P99 must be < 5μs for exchange-grade; < 10ms for retail

**3. Order Sequencer**
- **Single-threaded, assigns globally monotonic sequence numbers**
- Writes to WAL (Chronicle Queue or custom mmap file) **before** forwarding to engine
- This is the serialization point that makes the entire system deterministic
- Has a hot standby; failover < 1 second

**4. Matching Engine / Order Router**
- **Exchange**: single-threaded matching engine per symbol group; in-memory OrderBook (TreeMap bids/asks + HashMap for O(1) cancel); no DB on hot path
- **Retail broker**: stateless Order Router; PFOF routing to market makers; FIX to exchange
- **Institutional OMS**: Smart Order Router + Algo Engine (TWAP/VWAP); venues ranked by market impact model

**5. Fill Processor → Downstream**
- Fill arrives (FIX ExecutionReport or internal fill event)
- Synchronously update order status in PostgreSQL (ACID)
- Publish to Kafka for downstream fan-out: Position Service, Notification Service, TCA, Audit Log
- Kafka is NOT on the order-to-fill hot path — only on the downstream distribution path

**Whiteboard draw order:**

1. Draw the client at top-left and the exchange/market maker at top-right
2. Draw the "order entry → risk → sequencer → engine" vertical flow down the center
3. Draw the "fill → position update → notification" horizontal flow at the bottom
4. Draw Kafka as the horizontal bus connecting all downstream consumers
5. Add the data stores (PostgreSQL, Redis, S3) as boxes hanging off the services that own them
6. Circle the WAL and say: "this is the most important component for correctness"

---

### 3f. Deep Dive Areas

Pick 2-3 of these and know them cold. Mention them unprompted to demonstrate depth.

---

**Deep Dive 1: WAL + Crash Recovery (most important)**

Core problem: The matching engine is in-memory. A crash loses all open orders. Recovery must be correct AND fast (< 30 seconds to resume trading).

Solution:
- The Order Sequencer writes every accepted order to a **Write-Ahead Log** (Chronicle Queue — off-heap memory-mapped file) with a sequence number BEFORE forwarding to the matching engine
- On crash: replay the WAL from the last committed sequence number; the matching engine processes events in order, rebuilding exact in-memory state
- Two optimizations: (1) periodic book snapshots every 10 minutes reduce replay to "last snapshot + subsequent WAL events"; (2) WAL `fdatasync()` batched every 100μs (not per-message) — trades 100μs durability window for 10x throughput improvement

Trade-offs:
✅ Deterministic crash recovery — same input → same state always | ❌ Snapshot + replay adds operational complexity; must validate replay output against expected state
✅ fsync batching gives 10x throughput | ❌ 100μs durability window means up to 100μs of events could be lost if both primary and standby crash simultaneously (mitigated by hot standby)

Why Chronicle Queue, not Kafka: Kafka's minimum latency is 2–5ms. The sequencer must write before forwarding to the engine. At 500K events/sec, a 2ms Kafka write would require 1,000 concurrent in-flight writes — impossible for a strictly serial sequencer.

---

**Deep Dive 2: Exactly-Once Fill Processing**

Core problem: FIX session reconnects cause brokers to retransmit fills. If the OMS processes a fill twice, a user's position gets doubled. This is a real money error.

Solution (two-layer defense):

Layer 1 — FIX session level:
- Every fill has an `exchange_trade_id` (exchange-assigned, globally unique)
- On reconnect: FIX `ResendRequest` causes broker to retransmit all fills since last confirmed sequence
- OMS deduplicates: `SELECT 1 FROM fills WHERE exchange_trade_id = ?` before processing
- If exists: discard duplicate silently; do not update position

Layer 2 — Kafka level:
- Kafka idempotent producer + transactional consumer = exactly-once delivery semantics
- Position update uses optimistic locking: `UPDATE positions SET qty += fill_qty, version = version + 1 WHERE account_id = X AND symbol = Y AND version = expected_version`
- If 0 rows updated: version mismatch (concurrent modification) → retry with fresh read

Trade-offs:
✅ Two independent dedup layers means no fill ever processed twice | ❌ `exchange_trade_id` check adds one DB read per fill on the critical path (mitigated by Redis caching of recent trade IDs)

---

**Deep Dive 3: Single-Threaded Matching Engine — Why and How to Scale**

Core problem: Order matching requires consistent state. You cannot match two orders on the same symbol in parallel without a lock, and a lock becomes the bottleneck.

Solution: Single-threaded per symbol group — eliminates all lock contention entirely.

Scaling: 10,000 symbols partitioned across 100 engine instances = 100 symbols per engine. Each engine runs single-threaded on a dedicated CPU core. Cross-symbol interactions (e.g., ETF basket arbitrage) are handled at the trading strategy layer, not the matching engine.

Performance ceiling: LMAX Disruptor ring buffer achieves ~600M operations/second on a single core. Our requirement of 500K events/sec is ~0.1% of that ceiling. The engine is not the bottleneck — **the WAL write speed is**.

Key insight to say out loud: "The single-threaded design is the entire correctness model. Two runs of the same input always produce the same output. This is both a performance optimization (no locks) and a regulatory requirement (deterministic replay for audit)."

Trade-offs:
✅ Zero lock contention; maximum throughput per core | ❌ Vertical scaling limit (one core per symbol group); no active-active replication possible
✅ Deterministic replay | ❌ Hot standby failover adds operational complexity; sequencer is a theoretical SPOF

---

### 3g. Failure Scenarios & Resilience

Senior-level framing: **Financial systems distinguish between fail-closed and fail-open by asking "what is the cost of the wrong decision?" For anything involving money movement or compliance, fail closed. For read-only observability paths, fail open is acceptable.**

| Failure | Fail-Closed or Open? | Why | Recovery |
|---|---|---|---|
| Compliance service unavailable | **Fail-closed** | Submitting a non-compliant order is worse than delaying it | Queue orders; manual override with audit trail |
| Risk gateway unavailable | **Fail-closed** | An unvetted order could cause a fat-finger disaster or exceed position limits | Reject all new orders with "risk check unavailable" |
| Exchange feed sequence gap | **Fail-open** | Stale NBBO is better than showing users an error; last known price is directionally correct | Mark symbol NBBO as "potentially stale"; switch to TCP backup feed |
| NBBO engine crash | **Fail-open** (with degraded state) | Showing last known price is better than a blank screen | Hot standby takes over < 1s; subscribers receive resync event |
| Matching engine crash | **Fail-closed** (trading halts) | Cannot accept new orders without a sequencer or engine | WAL replay or hot standby promotion; < 30s recovery |
| FIX session disconnect with open orders | **Leave orders in book** (exchange policy) | The orders represent valid trading interest; cancelling them without consent would be harmful | `CancelOnDisconnect` flag is opt-in; default is to leave orders resting |

**Market-wide circuit breakers (must mention unprompted for exchange design):**
- Level 1: S&P 500 falls 7% from prior close → 15-minute halt for all equities
- Level 2: S&P 500 falls 13% → another 15-minute halt
- Level 3: S&P 500 falls 20% → halt for remainder of trading day
- Implementation: halt signal received from SIP (Securities Information Processor); all matching engines simultaneously halt order processing; members receive FIX `BusinessMessageReject`

**The clock skew problem (always mention for exchange-grade systems):**
- Use **exchange_ts_ns** (exchange-assigned timestamp) as authoritative ordering, NOT ingestion time
- Use **sequence numbers** (not timestamps) for within-engine ordering — sequence numbers are monotonic and immune to clock skew
- Use **PTP (IEEE 1588) GPS-synchronized clocks** across all nodes for nanosecond-accuracy regulatory timestamps (required for CAT reporting)

---

## STEP 4: Common Components Breakdown

For each component: why it's used, the one config decision to mention in interviews, and what breaks if you remove it.

---

### In-Memory Order Book (TreeMap + HashMap)

**Why used:** The core stateful data structure of any matching engine. TreeMap gives O(1) best-bid/ask access and O(log P) price-level insertion. HashMap gives O(1) cancel by order ID.

**Key config decision to mention:** Using a `TreeMap<Price, PriceLevel>` where each `PriceLevel` holds a `Deque<Order>` for FIFO time priority. This structure is why matching is O(1) amortized — you always process from the front of the best price level.

**What if you didn't use it:** If you use a flat list or array without price-level grouping, cancel operations become O(N) — at 500K events/sec with 70% cancels, you'd be doing 350K × O(N) lookups per second. The engine would fall over immediately.

---

### WAL (Write-Ahead Log — Chronicle Queue / custom mmap)

**Why used:** Makes in-memory state recoverable after crash. Written synchronously by the Order Sequencer before the event reaches the matching engine.

**Key config decision to mention:** `fdatasync()` called every 100μs batch (not every message) — trades a 100μs durability window for a 10x throughput improvement. At 500K events/sec, per-message fsync would require 500K disk writes/second, which even NVMe SSDs cannot sustain. Batching collapses this to 10K fsyncs/sec.

**What if you didn't use it:** On crash, all open orders and book state are lost. Recovery would require contacting every broker to reconcile — taking minutes to hours. During that time, trading is halted, which is a regulatory incident.

---

### Kafka (Downstream Fan-out Only — Not on Hot Path)

**Why used:** Durable, ordered event bus for all downstream consumers: audit log, position updates, analytics, notifications, CAT reporting. Partition key = symbol for per-symbol ordering.

**Key config decision to mention:** Kafka is NEVER on the order-to-fill critical path. It enters the architecture only after the fill is acknowledged. Latency: Kafka P99 is 2–5ms. Putting Kafka on the sequencer path would limit throughput to 200 events/sec per in-flight write, completely incompatible with 500K events/sec.

**What if you didn't use it:** You'd need point-to-point connections from the matching engine to every downstream consumer (position service, notification service, audit writer, CAT reporter, analytics). Adding a new consumer would require a matching engine code change. Kafka decouples producers from consumers.

---

### Redis Cluster

**Why used:** Hot cache for real-time state that is reconstructible from authoritative source (PostgreSQL or in-memory engine state). Used for: balance cache, position snapshots, idempotency key dedup, PDT counters, quote cache, WebSocket subscription registry.

**Key config decision to mention:** `allkeys-lru` eviction with TTLs on all values. Redis is NEVER the source of truth for financial data — it's always a cache. Cache miss always falls back to PostgreSQL. For large orders (> $10K notional), risk checks bypass the cache and go directly to PostgreSQL.

**What if you didn't use it:** Risk gateway reads buying power from PostgreSQL on every order. At 100K orders/sec peak, that's 100K DB reads/second — PostgreSQL handles ~10K/sec under normal conditions. The risk gateway becomes the bottleneck and trading slows to a crawl.

---

### PostgreSQL (ACID Source of Truth)

**Why used:** Authoritative store for all structured financial metadata: orders, positions, accounts, fills, allocations. ACID guarantees prevent double-fills and negative balances.

**Key config decision to mention:** Optimistic locking via `version` column: `UPDATE positions SET qty = qty + fill_qty, version = version + 1 WHERE account_id = :id AND symbol = :sym AND version = :expected_version`. Zero rows updated = concurrent modification = retry with fresh read. This avoids distributed locks entirely while preventing race conditions.

**What if you didn't use it:** Without ACID, two simultaneous fill events for the same position could both read qty=100, both compute qty+10=110, and both write 110. Result: position is 110 instead of 120. That's a financial error that creates a position break with the prime broker at end of day.

---

### Cassandra / S3 WORM (Audit Trail)

**Why used:** Immutable, high-write audit trail for all order events. Write-optimized LSM-tree storage (Cassandra) or S3 Object Lock compliance mode for exchange-scale immutability. Never updated, never deleted.

**Key config decision to mention:** For exchange-scale: Parquet files on S3 with Object Lock compliance mode (7-year retention, not deletable even by admin). Each file's SHA-256 hash stored in a separate index for tamper-evidence. For OMS-scale: Cassandra with RF=3, LOCAL_QUORUM; partition by order_id; clustering by sequence number.

**What if you didn't use it:** Using a regular mutable database for the audit log violates SEC Rule 17a-4, which requires WORM (Write Once, Read Many) storage for broker-dealer records. During a regulatory examination, if records can be modified, the entire audit is inadmissible.

---

### ClickHouse (OLAP — Analytics / TCA / Historical)

**Why used:** Columnar store for analytics queries over billions of fill and tick records. Not on the trading hot path. Used for: TCA (Transaction Cost Analysis), execution quality reports, OHLCV candle storage, historical tick queries.

**Key config decision to mention:** `ReplacingMergeTree` engine for OHLCV candles — handles in-progress candle updates (same candle sent multiple times as trades arrive during the candle window; last version wins after merge). `MergeTree` for append-only analytics (ticks, TCA). Always partition by (symbol, date) for efficient range queries.

**What if you didn't use it:** Running TCA queries (compare fill prices to VWAP benchmarks over 250K fills) on PostgreSQL would take minutes and spike the OLTP database. ClickHouse runs the same query in seconds on columnar storage.

---

### FIX Protocol Engine

**Why used:** Industry-standard messaging protocol for order entry, execution reports, and broker connectivity. All brokers, exchanges, and market makers speak FIX. It handles session management (sequence numbers, heartbeats, resend requests) that are critical for exactly-once fill processing.

**Key config decision to mention:** FIX sequence number gap recovery on reconnect: send `ResendRequest` for all messages after last confirmed sequence number; broker retransmits all fills in the gap. The OMS must deduplicate these retransmitted fills by `exchange_trade_id` to avoid double-booking.

**What if you didn't use it:** Building a proprietary protocol means no connectivity to any external broker, exchange, or market maker. The entire financial industry is wired on FIX. Not using it means you're building a closed system with zero external connectivity.

---

### Circuit Breaker Pattern (Per Symbol + Per Service)

**Why used:** Serves dual purpose — (1) trading safeguard: halt matching when price moves too fast (regulatory requirement); (2) software resilience: stop calling a failing downstream service before the failure cascades.

**Key config decision to mention:** Trading circuit breaker thresholds: halt if price moves > 10% in 5 minutes (regulatory Level 1/2/3 triggers). Fat-finger collar: reject orders > 5% from last trade price. Software circuit breaker: OPEN after 3 failures in 5s; HALF_OPEN after 10s.

**What if you didn't use it:** Without price collars, a mistyped order ("sell 1,000,000 shares of AAPL at $0.01") could execute against thin book liquidity, crashing the price and causing cascading stop-loss triggers. This has happened in real markets (the 2010 Flash Crash is partly attributable to missing circuit breakers).

---

### Kernel-Bypass Networking (DPDK / RDMA)

**Why used:** Eliminates Linux kernel network stack overhead to achieve < 1μs network latency for the order-to-fill path. Standard kernel networking adds ~10μs per message.

**Key config decision to mention:** DPDK for UDP packet processing (feed handlers, co-located order entry); RDMA over InfiniBand for WAL replication to hot standby site (reduces replication latency from ~100μs to ~5μs). Not used for retail trading — internet RTT dominates, kernel overhead is irrelevant.

**What if you didn't use it:** Co-located HFT clients would experience ~10μs kernel overhead per message. At microsecond-level competition, 10μs is a massive disadvantage. Exchange would lose co-location revenue to a competitor with lower latency.

---

## STEP 5: Problem-Specific Differentiators

### Order Matching Engine

**Unique things:**
1. The entire design is centered on **single-threaded determinism** — same input sequence always produces identical output. This is both a performance optimization (no locks) and a regulatory requirement (deterministic replay).
2. **LMAX Disruptor ring buffer** — zero GC, zero allocation on the hot path; ~600M ops/sec on a single core. The engine uses fixed-size off-heap Order structs to prevent JVM garbage collection pauses.

**Specific technical decision that's different:** Opening/Closing auctions — a fundamentally different algorithm from continuous matching. Collect all pre-market orders, compute the volume-maximizing clearing price, execute all matches simultaneously at one price. This is a batch computation that runs once at 9:28 AM, not a stream.

**How is the matching engine different from the exchange platform?**
The matching engine is the pure matching algorithm in isolation — just the order book, sequencer, WAL, and fill emitter. The exchange platform is the matching engine plus all the regulatory and member-facing infrastructure: FIX gateway per member firm, CAT reporting, DTCC clearing integration, SIP publishing, market operations console, member firm registry, and physical co-location fairness. The exchange wraps the engine in the institutional framework.

---

### Stock Exchange Platform

**Unique things:**
1. **Physical fairness** — equal fiber cable lengths from all co-located member racks to the matching engine. This is an operational and regulatory requirement, not a software feature. It prevents any member from having a network latency advantage over another at the same co-location tier.
2. **Regulatory infrastructure** — CAT (Consolidated Audit Trail) reporting to SEC within 1 second of event occurrence; DTCC/NSCC clearing integration for T+1 settlement; SIP publishing (all trades must appear on consolidated tape for Regulation NMS); member firm registry with MPID management.

**Specific technical decision that's different:** Dual data center with synchronous WAL replication over dedicated dark fiber (NJ Carteret ↔ NJ Mahwah, ~10 miles apart). This achieves zero-data-loss failover — WAL is replicated before the primary acknowledges the order — while maintaining < 1ms replication latency due to the short geographic distance.

**How is the exchange platform different from the matching engine?**
The matching engine is the core algorithm. The exchange platform adds the regulatory and operational layers: member management, circuit breakers, SIP reporting, CAT compliance, and clearing integration. The exchange's unique challenge is the dual mandate of **fairness** (equal treatment of all members) and **systemic stability** (circuit breakers, halt/resume, auction mechanics). A pure matching engine has neither of these concerns.

---

### Retail Trading Platform (Robinhood / E*TRADE)

**Unique things:**
1. **Fractional shares** — exchanges don't trade fractional shares. The broker aggregates fractional orders in 100ms windows into whole-share lots, submits to exchange as a single order, then pro-rata allocates fills back to users. This aggregation window is the platform's most unique architectural feature.
2. **PFOF routing** — retail market orders are routed to market makers (Citadel, Virtu) who pay the broker per order and provide price improvement. This is entirely absent from institutional or exchange systems.

**Specific technical decision that's different:** T+1 settlement cash tracking in three buckets — `cash_available` (tradeable now), `cash_pending` (ACH in-flight, not yet settled), `cash_settled` (T+1 settled). A user who deposits via ACH can trade immediately with pending cash, but if the ACH fails, the broker has a "good faith violation" — a regulatory risk unique to retail platforms.

**How is the retail platform different from the OMS?**
The retail platform hides institutional infrastructure behind a consumer UX: fractional shares, PDT tracking, KYC/AML, push notifications, and PFOF routing. The OMS serves institutional portfolio managers with whole-share block orders, TWAP/VWAP execution algorithms, multi-fund allocation, pre-trade compliance against investment guidelines, and post-trade TCA. The retail platform focuses on accessibility and regulatory compliance for FINRA-regulated individual investors. The OMS focuses on execution quality and compliance for registered investment advisers managing large funds.

---

### Market Data Feed

**Unique things:**
1. **Fan-out at extraordinary scale** — normalizing 12M events/sec from 5+ exchanges (each with a different binary format and sequence number space) into a single canonical format, then delivering relevant subsets to 100K+ subscribers with sub-10ms latency while simultaneously archiving everything for 5-year replay.
2. **Dual delivery tiers** — HFT subscribers via UDP multicast (< 1ms, no delivery guarantees, packet loss handled by receiver-side gap detection); retail/analytics subscribers via WebSocket over TCP (< 10ms, reliable). Same normalized event stream, two completely different delivery mechanisms.

**Specific technical decision that's different:** Storing `raw_message_bytes` (original binary feed message) alongside normalized fields in the Parquet tick archive. This enables **perfect historical replay** — quantitative researchers can reconstruct the exact book state at any nanosecond in history, which is impossible with only normalized data (because the raw binary encodes exchange-specific fields not captured in normalization).

**How is the market data feed different from the exchange platform?**
The exchange platform is a producer of market data — it generates fills and book updates from its own matching engine and publishes them to the SIP and to members. The market data feed is a consumer and aggregator — it ingests data from multiple external exchanges (NYSE, NASDAQ, CBOE, IEX, OPRA), normalizes it into a single format, computes a consolidated NBBO across all venues, and distributes it downstream. The exchange sees one venue's data; the market data feed sees all venues and consolidates them.

---

### Order Management System (OMS)

**Unique things:**
1. **CQRS + Event Sourcing** — the `order_events` table is the append-only source of truth. Current order state is a materialized projection of events. This enables exact lifecycle replay for audit, debugging, and state reconstruction after failures. Regulators can ask "show me every decision made on order X" and the entire lifecycle — compliance check, routing decision, fills, allocation — is reconstructible from the event log.
2. **Pre-trade allocation** — when a PM places a block order for multiple accounts, the allocation (which account gets what percentage) must be decided and recorded BEFORE the order is submitted to the exchange. Post-trade modification of allocations is a regulatory violation. This forces a specific order of operations: compliance check → allocation decision → order submission.

**Specific technical decision that's different:** VWAP execution schedule pre-computation at order creation (one ClickHouse query against historical volume profiles), stored in Redis for Algo Engine retrieval. This avoids per-slice computation overhead — the schedule is computed once and the Algo Engine just reads the next scheduled quantity at each time interval.

**How is the OMS different from the retail platform?**
The OMS translates investment intent (PM's "buy 100K AAPL for three funds") into optimized market execution (child orders across dark pools and lit venues over hours) while satisfying pre-trade compliance, allocation fairness, and post-trade audit requirements. The retail platform handles individual investor orders: one account, one order, routed immediately to a market maker or exchange. The OMS has no fractional shares, no PDT tracking, and no PFOF. It has algo execution strategies, block allocation, compliance guidelines engines, and TCA — none of which exist in the retail platform.

---

## STEP 6: Q&A Bank

### Tier 1: Surface Questions (First 10 Minutes)

**Q: What data structure do you use for the order book?**

**The core data structure is a TreeMap per side (bids sorted descending, asks sorted ascending), where each price level holds a FIFO Deque of orders.** You also need a `HashMap<OrderId, Order>` alongside it for O(1) cancel lookup. The TreeMap gives you O(1) best-bid/ask (just call `firstEntry()`) and O(log P) insert at any price level where P is the number of distinct price levels — in practice, P is ~100-500 for a typical stock, so log P is about 7-9 comparisons. The Deque at each price level enforces time priority (FIFO) within a price level. Without the HashMap, cancellations would require O(N) scan through the book, which is unacceptable at 350K cancels/second.

---

**Q: Why is the matching engine single-threaded?**

**Matching requires consistent state — you cannot match two orders on the same symbol in parallel without a lock, and a lock becomes the bottleneck.** The single-threaded design eliminates all lock contention entirely. The scale-out strategy is horizontal partitioning: 10,000 symbols distributed across 100 engine instances, each handling 100 symbols on a dedicated core. Within a single core, the LMAX Disruptor ring buffer pattern achieves ~600M operations/second — our 500K events/sec requirement is less than 0.1% of that ceiling. The engine is not the bottleneck; the WAL write speed is.

---

**Q: What is the WAL and why is it needed?**

**The Write-Ahead Log is how we make an in-memory system recoverable after a crash.** The Order Sequencer writes every accepted order to a durable append-only file with a sequence number before forwarding to the matching engine. On crash: replay the WAL from the last committed sequence number; the matching engine processes events in order, rebuilding identical in-memory state. We use Chronicle Queue (a memory-mapped file on NVMe SSD) rather than Kafka because Kafka's 2–5ms latency is incompatible with a serial sequencer at 500K events/sec. fsync is called every 100μs — not per message — which gives 10x better throughput at the cost of a 100μs durability window. The hot standby engine mitigates that window.

---

**Q: How do you prevent negative balances or overselling?**

**We use optimistic locking on the balance and position tables — no distributed locks.** The pattern is: `UPDATE account_balances SET cash_available = cash_available - :amount, version = version + 1 WHERE account_id = :id AND cash_available >= :amount AND version = :expected_version`. If the UPDATE affects 0 rows, either the balance was insufficient or a concurrent modification changed the version — both cases result in a retry with a fresh read. Additionally, there's a `CHECK (cash_available >= 0)` constraint at the database level as a hard backstop. This pattern avoids distributed locks entirely while guaranteeing no race condition can create a negative balance.

---

**Q: How does the system handle a fill that arrives twice (duplicate)?**

**Two-layer deduplication: at the FIX session level and at the database level.** Layer 1: every fill from an exchange has a unique `exchange_trade_id`. On fill receipt, we check `SELECT 1 FROM fills WHERE exchange_trade_id = ?`. If exists, discard silently. Layer 2: the `fills` table has a `UNIQUE` constraint on `exchange_trade_id` — even if the application-level check is bypassed by a race, the INSERT will fail with a conflict. FIX session reconnects are the primary trigger for duplicates — the broker retransmits all fills since last confirmed sequence number. The OMS must handle this gracefully because the FIX spec requires at-least-once delivery.

---

**Q: Why do you use Kafka? Why not just write directly to the database?**

**Kafka decouples the fill acknowledgment path from all the downstream work that needs to happen.** When a fill arrives, the Order Service acknowledges it synchronously (updates order status in PostgreSQL). Then it publishes to Kafka. Every downstream system — Position Service, Notification Service, Analytics, CAT Reporter, Risk updater — reads from Kafka independently at its own pace. If the notification service is slow or down, it doesn't affect order processing. If we wrote directly to all downstream systems, a slow analytics database would hold up the fill acknowledgment. Kafka gives us fan-out, replay capability (up to 7-day retention), and isolation between producers and consumers.

---

**Q: What is PFOF and how does it affect your order routing design?**

**Payment for Order Flow is where market makers pay retail brokers a fee per order routed to them, in exchange for providing price improvement over the NBBO.** The routing decision in the Order Router is: check if contracted PFOF venues offer expected price improvement ≥ half the NBBO spread; if yes, route to the PFOF venue; if no, route to an exchange. Every routing decision is logged with a rationale — SEC Rule 606 requires quarterly public disclosure of order routing statistics and PFOF received. The key architectural implication is that the Order Router needs a real-time NBBO feed (from Redis cache, < 5ms stale) and a historical price improvement model per venue to make the routing decision.

---

### Tier 2: Deep Dive Questions

**Q: Walk me through exactly what happens when the matching engine crashes and how you recover.**

**The WAL is the recovery mechanism — replay makes the crash transparent.** Here's the exact sequence: (1) The Order Sequencer assigns seq# N and writes the order to the Chronicle Queue WAL on NVMe SSD; (2) The engine processes seq# N and crashes before completing the match; (3) Health monitor detects missed heartbeat within 300ms; (4) Option A — hot standby engine: it was already processing all events but suppressing output; on promotion it simply starts emitting. Book state is identical to the crashed primary. No replay needed. Failover completes in ~500ms. Option B — no hot standby: replay WAL from last snapshot (taken every 10 minutes) through the end of the log. Each event is replayed in sequence; the engine rebuilds exact book state. Typical replay time: < 30 seconds. The crucial property that makes this work: **the matching engine is deterministic** — given the same sequence of events, it always produces the same output state.

---

**Q: How do you compute the NBBO correctly when different exchanges have clock skew?**

**Use exchange_ts_ns (the exchange's own timestamp) for ordering, NOT ingestion timestamp (when our system received it).** If NYSE's clock is 500μs ahead of NASDAQ's, ingestion time ordering would make a newer NYSE quote look older than a NASDAQ quote that arrived first at our feed handler. The NBBO Engine maintains per-symbol, per-exchange best bids and asks indexed by exchange_ts_ns. To handle late arrivals (a quote that arrives out of order due to network jitter), we apply a 5ms "late arrival window" before publishing the consolidated NBBO — this adds 5ms to NBBO latency but ensures quotes are correctly ordered. For HFT subscribers who need sub-1ms latency, we publish a "raw" NBBO without the late-arrival window, with a disclaimer that it may not reflect cross-exchange ordering precisely.

---

**Q: Explain exactly-once fill semantics. What are all the ways a fill could be processed twice?**

**The failure modes are FIX session reconnect (most common), Kafka consumer redelivery, and application restart with in-flight messages.** (1) FIX reconnect: broker retransmits fills since last confirmed sequence number. Defense: `exchange_trade_id` UNIQUE constraint + application-level dedup check. (2) Kafka consumer redelivery: if a consumer crashes after processing but before committing the offset, Kafka redelivers. Defense: Kafka idempotent producer + transactional consumer (exactly-once semantics), plus the UNIQUE constraint as backstop. (3) Application restart with in-flight messages: an order update was partially applied before crash. Defense: optimistic locking — the position update uses `WHERE version = expected_version`; if the update was partially applied and committed, the version is wrong and the retry produces 0 rows, which is the correct signal to stop retrying. **The key insight is that we use multiple independent layers** so no single point of failure can cause a double-fill.

---

**Q: How does your SOR (Smart Order Router) prevent information leakage when routing to dark pools?**

**The core principle is: send IOC-only (Immediate-Or-Cancel) to dark pools so the order is never visible in the dark pool book.** The IOC order either fills immediately against dark pool inventory or cancels — it never rests in the dark pool's order book where other participants could detect it. Additional protections: (1) Randomized submission timing — add a few milliseconds of jitter to sub-order submissions so HFT firms cannot detect a pattern revealing the parent order size; (2) Minimum execution size — for large orders, set a minimum fill size (e.g., 1,000 shares) in the IOC so partial fills of 10 shares don't reveal that a large order is probing for liquidity; (3) Venue rotation — vary which dark pools are used across algo slices; (4) All routing decisions are logged with rationale for best execution documentation. The fundamental tension: doing more to hide order size (smaller IOCs, less predictable timing) costs execution quality (more slippage from multiple re-routes).

---

**Q: Why do you use optimistic locking instead of pessimistic locking or distributed locks for positions?**

**In a high-concurrency financial system, pessimistic locking (SELECT FOR UPDATE) means every fill queues up behind a database row lock, creating a serialization bottleneck that kills throughput.** Distributed locks (Redis SETNX / Zookeeper) add a network round-trip per lock acquisition and create a single point of failure. Optimistic locking avoids both: you read the current version, compute the update, and attempt to write — only locking the row for the microseconds of the UPDATE itself. Conflict rate in practice is very low because fills for the same account-symbol combination rarely arrive simultaneously. When they do, the retry is a cheap re-read + re-apply. The `version` column costs one BIGINT per row and the pattern is: `UPDATE positions SET qty += fill_qty, version = version + 1 WHERE account_id = X AND symbol = Y AND version = expected_version`. Zero rows updated = retry. **The key insight: conflicts are rare; optimistic locking optimizes for the common case.**

---

**Q: How does the opening auction work and why is it different from continuous matching?**

**The opening auction solves a cold-start problem: at 9:30 AM there are hundreds of pre-market orders with no reference price. A single-price batch auction finds the clearing price that maximizes executed volume.** The algorithm: for each candidate price P (at tick increments from lowest ask to highest bid), compute executable buy volume (all buy limit orders ≥ P plus all Market-on-Open orders) and executable sell volume (all sell limit orders ≤ P plus Market-on-Open sells). Select the P that maximizes min(buy volume, sell volume) — that's the clearing price. All orders that can execute at the clearing price are filled simultaneously at that one price. This is fundamentally different from continuous matching: (1) no FIFO priority at the clearing price — all orders at that price are treated equally (pro-rata if there's imbalance); (2) the computation is a batch scan over the entire pre-market order set, not an incremental per-order operation; (3) the exact auction fire time is randomized within a 30-second window to prevent latency arbitrage.

---

**Q: What is CAT reporting and what happens if you miss a record?**

**CAT (Consolidated Audit Trail) is SEC Rule 613 — every US exchange and broker-dealer must report every order event (new, cancel, modify, fill) to the SEC's national audit database within 1 second of occurrence for electronic orders.** Implementation: the CAT Reporter Service is an async Kafka consumer that reads from the same audit topic as other downstream systems. It enriches records with MPID and customer account info, formats them per SEC schema, and delivers via REST to CAT CAIS (the SEC's ingestion system). For missed submissions: failed deliveries are retried with exponential backoff; if error rate exceeds 1%, a compliance alert fires. If the CAT system itself is down, records are buffered locally (up to 24 hours) and batch-delivered when it restores. The penalty for systematic non-reporting is an SEC enforcement action and fine — this is a hard compliance requirement, not a best effort.

---

### Tier 3: Stress Test / Staff+ Questions

**Q: The exchange is processing 2M events/second normally. During a major market crash, volume spikes to 10M events/second. Walk me through every layer that could be a bottleneck and how you'd handle each.**

There is no single right answer here — show your reasoning:

Start with the actual bottleneck identification. The matching engine's CPU isn't the constraint (LMAX Disruptor handles ~600M ops/sec per core; 10M events/sec is trivial for 100 engine instances). The real bottlenecks are:

(1) **WAL write throughput**: fsync batching means we're doing 10K fsyncs/sec under normal load; at 10M events/sec, that batch interval may need tuning. NVMe SSDs handle ~100K+ random writes/sec, so we have headroom, but we'd want to monitor WAL write latency P99 crossing 10μs as an early warning.

(2) **Network ingress to the order entry gateways**: 10M events × ~200 bytes = 2 GB/s ingest. Each gateway node needs a 25 Gbps NIC. Pre-scale gateway capacity before anticipated high-volume events (earnings announcements, economic data releases follow a calendar).

(3) **Redis for risk checks**: 10M risk checks/sec. Redis Cluster handles 1M+ ops/sec per shard. Horizontal scaling of Redis shards is the lever here. Alternative: move the most common risk check (buying power) to an in-process approximation using a pre-allocated budget that's reconciled against Redis every 100ms.

(4) **Market data fan-out**: 10M book updates/sec × 200 subscribers/symbol = 2B delivery/sec. The multicast publisher sends one UDP packet per update to the entire multicast group — O(1) fan-out. This scales naturally. WebSocket subscribers are the constraint; auto-scale WebSocket nodes preemptively.

(5) **Kafka lag**: downstream consumers (audit writer, position service, analytics) will lag. This is acceptable — the critical path (matching → fill → execution report → member) doesn't go through Kafka. Kafka lag is monitored and auto-scaling of consumer instances is triggered at >1M message lag.

---

**Q: A fund manager calls and says their position shows 50,000 shares of AAPL but the prime broker shows 52,000. It's end of day. How do you diagnose and fix it?**

This is a position break — one of the most serious operational events in trading. Walk through it systematically:

**Diagnosis steps:**
1. Pull the OMS fill records for the fund-account-AAPL combination for today from PostgreSQL. Sum all fills: starting position + sum(buy fills) - sum(sell fills) should equal 50,000.
2. Request the prime broker's fill report for the same fund-account-symbol-date.
3. Compare fill by fill using `exchange_trade_id` as the join key. The break is caused by exactly one of: (a) fills in the prime broker that are missing from OMS (lost fill); (b) fills in OMS that the prime broker doesn't have (phantom fill); (c) an allocation error (same fill credited to wrong account); (d) a corporate action (stock split, dividend reinvestment) applied by one side but not the other.
4. If fills match but positions don't: check the starting position. Was yesterday's reconciliation clean? Is there a settlement break from T-1?

**Fix:**
- If a fill is in the prime broker but not in OMS: it was likely dropped during a FIX session reconnect. Replay from FIX session logs (all FIX messages are logged); reprocess the missing fill through the fill processor. The `exchange_trade_id` UNIQUE constraint will prevent double-processing.
- If a fill is in OMS but not in the prime broker: the trade may not have settled. Escalate to the broker's operations team for a manual trade insertion.
- Either way: the break is logged, resolution documented, and a compliance exception filed. The `order_events` CQRS table is the source of truth for reconstruction.

---

**Q: A regulator asks you to prove that your audit trail is tamper-evident and no fill was modified after the fact. How do you demonstrate this?**

This is a cryptographic integrity problem with regulatory stakes.

**What we have:**
- Parquet files on S3 with Object Lock in compliance mode (7-year retention; cannot be deleted or modified even by the account owner or AWS support under the compliance mode policy)
- A separate SHA-256 hash index: each Parquet file's hash is computed at write time and stored in a separate append-only hash index file
- The WAL on Chronicle Queue is also immutable (append-only file; not deletable while in retention window)

**How to demonstrate:**
1. Compute the SHA-256 hash of the requested Parquet file today
2. Compare against the hash stored in the hash index at write time
3. Hashes match → the file has not been modified since it was written
4. Pull the same event from the WAL (by sequence number) and compare against the Parquet record — they should be identical
5. Show the S3 Object Lock compliance mode certificate for the bucket — this is documented proof that S3 guarantees immutability at the storage layer

**What you'd additionally build for a Staff+ answer:** cryptographic chaining — each Parquet file's header includes the SHA-256 hash of the previous file (like a blockchain). Even if someone could somehow forge a file, they'd have to recompute every subsequent hash in the chain — computationally infeasible, and the hash chain itself is stored in a separately audited system.

---

## STEP 7: Mnemonics & Memory Anchors

### The FRESH Acronym (for any stock market trading design)

**F — Fairness** (price-time priority; equal treatment at same price level)
**R — Resilience** (WAL + hot standby; no single point of failure on the trading path)
**E — Exactly-once** (idempotency keys + exchange_trade_id dedup; money must not move twice)
**S — Sequencer** (the single-threaded serialization point; everything flows through it)
**H — Hot path vs. cold path** (separate your < 1ms path from your 2ms+ Kafka path; never mix them)

Use it as a checklist during your design: have you addressed Fairness? Resilience? Exactly-once? A clear Sequencer? Did you separate Hot path from cold path?

### The One-Liner Opening for Any Problem in This Category

Memorize this and say it at the start of every stock market trading system design:

> "Financial systems have three non-negotiables: **zero data loss** for money events, **strong consistency** for balances and positions, and **strict ordering** for everything that affects matching or audit. Every architectural decision flows from which of these three properties is most at risk in that layer."

Then immediately follow up with your first clarifying question: "Before I start — are we designing for exchange-grade microseconds, institutional milliseconds, or retail sub-second latency? That single answer shapes almost everything."

---

### The Latency Stack (memorize all three tiers)

```
Exchange/HFT:    < 1μs network → < 10μs match → < 500μs end-to-end
                 (DPDK + off-heap structs + Chronicle Queue WAL)

Institutional:   < 10ms compliance → < 50ms submit → < 10ms fill → position
                 (FIX + PostgreSQL + Redis cache + Kafka downstream)

Retail:          < 100ms order service → < 500ms exchange ACK → < 3s push notify
                 (REST + JWT + PostgreSQL + Kafka + APNs/FCM)
```

---

## STEP 8: Critique

### What the Source Material Covers Well

- **The WAL + deterministic replay story** is exceptionally thorough. The fsync batching rationale, Chronicle Queue vs. Kafka reasoning, and snapshot + replay recovery time estimates are all production-accurate and interview-differentiated.
- **Optimistic locking pattern** is explained with the exact SQL and failure semantics. Most candidates describe optimistic locking abstractly; these notes give you the precise UPDATE statement and what "0 rows affected" means.
- **FIX protocol sequence number gap recovery** is detailed and accurate. Most candidates have never seen this explained at this level of precision.
- **The dual delivery tier** (UDP multicast for HFT, WebSocket for retail) in market data is a clean architectural insight that demonstrates real financial industry knowledge.
- **The PFOF routing story** for retail is complete: routing logic, SEC Rule 606 requirements, ClickHouse for reporting. Very few candidates know this.

---

### What's Missing or Shallow

**1. Options are mentioned but not designed.** OPRA is cited (10M msg/sec) and options chains are listed as a retail requirement, but there's no design for options matching (which uses different priority algorithms — pro-rata is common for options on some exchanges), no discussion of the options pricing model integration, and no explanation of how Greeks are served in real time. If an interviewer asks "extend this for options," you're on your own.

**2. Crypto is explicitly out of scope — but you'll be asked.** Crypto exchanges (Coinbase, Binance) share the matching engine architecture almost exactly but have key differences: 24/7 trading (no opening auction, no T+1 settlement), user-custodied wallets (the exchange doesn't hold shares at DTCC), no FIX protocol (REST/WebSocket only), different regulatory framework (FinCEN, not SEC/FINRA), and instant settlement (blockchain confirmation instead of T+1). Know these 5 differences.

**3. Margin accounts are mentioned but not designed.** The retail trading platform mentions margin_enabled and margin_buying_power but never explains how margin works: how buying power is calculated (typically 2:1 or 4:1 leverage), what a margin call looks like (system must forcibly liquidate positions if account equity drops below maintenance margin), or how the broker manages its own risk from margin lending. A senior interviewer at a retail trading company will ask this.

**4. The settlement failure path is thin.** T+1 settlement is mentioned but the "what happens when settlement fails" path is not covered. A buyer's ACH fails → the broker has delivered shares but not received cash → this is a "free riding" violation. The system needs a reconciliation job that monitors settlement status from the custodian (DTC/Apex/DriveWealth) and can restrict accounts with pending settlement failures.

**5. Multi-region / global exchange is not addressed.** All designs assume a single data center (or two nearby DCs for failover). What does a globally distributed exchange look like? This is a legitimate Staff+ question for companies like Coinbase (global crypto), CME (global futures), or HSBC's FX platform. The answer involves regional matching engines with no cross-region coordination on the hot path — each region is autonomous, cross-region arbitrage is handled at the trading strategy layer.

---

### Real-World Concerns a Senior Interviewer Would Probe

**1. "What is your worst-case recovery time if both the primary and the hot standby fail simultaneously?"**

The hot standby mitigates the 100μs fsync batch window. But if both primary and standby crash simultaneously (e.g., data center power failure), you replay from WAL from the last committed sequence. If the WAL replication was synchronous (NVMe-oF to standby), zero data loss. If asynchronous, you lose the last batch window. The interviewer wants to hear you acknowledge this risk and explain your replication mode choice.

**2. "How do you handle a market data vendor providing incorrect prices and how long before you detect it?"**

The notes don't address data quality monitoring beyond sequence gaps. A real system would compare the NBBO against the SIP tape (the regulatory consolidated tape that all US exchanges report to) as a cross-check. Discrepancies > some threshold trigger an alert and mark the affected symbol's NBBO as "unvalidated." This is a data quality vs. latency trade-off: adding the SIP cross-check adds ~50ms to NBBO computation.

**3. "What happens to open orders on the matching engine if a member firm goes bankrupt during the trading day?"**

This is a legal/operational question with system design implications. The exchange's member agreement specifies that on firm insolvency, the exchange has the right to cancel all open orders (and does so). The system design implication: the exchange ops console has a "mass cancel by firm MPID" API that cancels all open orders for that firm across all engine instances simultaneously. This must be atomic from the member's perspective (all cancels issued in the same millisecond, not dribbled out). The notes describe per-firm suspension but don't cover the mass-cancel atomicity requirement.

---

### Interview Traps the Notes Don't Warn About

**Trap 1: Saying "I'll use Kafka for the order acknowledgment path."**
If you put Kafka between the Order Sequencer and the Matching Engine, you've just limited your throughput to ~200K events/sec (10K partitions × 20 events/sec per partition in-flight) and added 2–5ms to every order acknowledgment. The interviewer will ask "what's the latency of Kafka?" to catch this. The answer: Kafka is for downstream fan-out only, never on the critical path.

**Trap 2: "I'll use a distributed lock (Redis SETNX) to prevent double-fills."**
Distributed locks on a per-fill basis are catastrophically slow at fill rates. A fill arrives every ~microsecond at the matching engine. A Redis round-trip is 100–500μs. Using locks per fill would cap throughput at 2,000–10,000 fills/second — far below requirements. The correct answer is the `exchange_trade_id` UNIQUE constraint + optimistic locking, not distributed locks.

**Trap 3: Not distinguishing between the matching engine and the exchange platform.**
These are often treated as the same thing. The matching engine is a sub-component of the exchange platform. An exchange platform adds: member firm management, CAT reporting, clearing integration, SIP publishing, market operations console, and regulatory halt mechanisms. Conflating them suggests you haven't thought through the full production system.

**Trap 4: Saying the order book state is persisted to a database.**
The order book is in-memory only. It is NEVER written to PostgreSQL or any database on the hot path. The WAL is the persistence mechanism — but it's a sequential append-only log, not a queryable database. On crash, state is reconstructed by replaying the WAL. If you say "I'll write each order book update to a DB," the interviewer knows you don't understand the latency constraints.

**Trap 5: Forgetting idempotency entirely.**
At any point in the system where there's a network call, there can be a retry. Every POST /orders must have an `Idempotency-Key` header. Every FIX NewOrderSingle has a `ClOrdID` that serves as the exchange-level idempotency key. Every fill processing step deduplicates by `exchange_trade_id`. If you design any write path without mentioning idempotency, a senior interviewer will ask "what happens on retry?" and you'll be on the defensive.

---

*This guide is self-contained. All concepts, architectures, trade-offs, and Q&A responses are sourced from a complete analysis of the Order Matching Engine, Stock Exchange Platform, Retail Trading Platform, Market Data Feed, and Order Management System designs, plus the common patterns shared across all five problems.*
