# Problem-Specific Design — Stock Market Trading (21_stock_market_trading)

---

## Order Matching Engine

### Unique Functional Requirements
- Price-time priority FIFO matching with strict determinism — same input sequence must always produce identical output
- Iceberg order support: visible peak quantity conceals actual order size; peak refreshed from hidden reserve on fill, placed at back of FIFO queue at same price
- Opening and closing auction (single-price batch auction): collect all pre-market orders, compute volume-maximizing clearing price, execute all matches simultaneously at one price
- Per-symbol circuit breaker: halt matching if price moves > 10% in 5 minutes; reject orders > 5% from last trade price (fat-finger collar)
- Sequence number assignment before engine processing: strict ordering of all events for deterministic replay

### Unique Components / Services
- **LMAX Disruptor ring buffer**: Lock-free, cache-friendly single-producer/single-consumer ring buffer; zero GC; zero allocation on hot path; processes ~600M ops/sec on a single core
- **Chronicle Queue**: Off-heap, memory-mapped append-only WAL; persistence without Java GC; sub-microsecond write latency; O(1) replay by seeking to offset
- **Hot standby engine**: Processes all events, maintains identical book state, suppresses output; promotes to active in < 1s on failover — no replay needed
- **GPS/PTP clock (IEEE 1588)**: Nanosecond-accurate timestamps; required for CAT regulatory reporting and for NBBO computation with clock-skew-aware ordering

### Unique Data Models
```
In-memory only (reconstructed from WAL on restart):
  OrderBook per symbol:
    bids: TreeMap<Decimal, PriceLevel>  (descending — best bid = highest price)
    asks: TreeMap<Decimal, PriceLevel>  (ascending — best ask = lowest price)
    orderIndex: HashMap<OrderId, Order> (O(1) cancel lookup)

  PriceLevel:
    price:     Decimal
    totalQty:  Long
    orders:    Deque<Order>  (FIFO — time priority within price level)

  Order fields unique to matching engine:
    icebergPeakQty, icebergHiddenQty (null for non-iceberg)
    seqNum (globally monotonic, from sequencer)
    firmId (member firm identifier)
```

### Unique Scalability / Design Decisions
- **Single-threaded per symbol group**: eliminates lock contention; 10K symbols / 100 engine instances = deterministic sequential processing; horizontal partitioning is the only scale-out strategy
- **Kernel-bypass networking (DPDK)**: eliminates Linux kernel network stack; reduces per-message latency from ~10μs to ~1μs; required for sub-millisecond order-to-fill at co-location
- **fsync batching on WAL**: batch 500μs of events per fsync call; trades 500μs durability window for 10x throughput improvement; acceptable because standby engine provides redundancy within that window
- **Fixed-size off-heap Order structs**: prevents Java GC pauses on the hot path; Chronicle Map / direct memory allocation; all Order fields fixed-width for cache-line efficiency

### Key Differentiator
The matching engine's entire design is structured around one constraint: **single-threaded determinism**. Everything — the ring buffer, kernel-bypass network, off-heap structs, WAL before processing — exists to let a single thread process events as fast as physically possible without any coordination overhead. Two runs of the same input always produce the same output, which is both a correctness guarantee and a regulatory requirement.

---

## Stock Exchange Platform

### Unique Functional Requirements
- Member firm onboarding and entitlement management (per-firm, per-symbol order type restrictions)
- Opening and closing auction with pre-market order collection, indicative price dissemination, and volume-maximizing clearing price
- Regulatory halt mechanisms: circuit breaker (Level 1/2/3 based on S&P 500 % decline), symbol-specific halts (regulatory, volatility), and Rule 48 (news-related halts)
- CAT (Consolidated Audit Trail) reporting: deliver every order event to SEC's CAT CAIS system within 1 second of occurrence (electronic orders)
- DTCC/NSCC clearing integration: route all fills for T+1 novation and netting
- SIP (Securities Information Processor) reporting: all trades published to consolidated tape for all market participants

### Unique Components / Services
- **FIX Gateway per member firm**: dedicated TCP session per member; FIX session state (sequence numbers, heartbeats) managed per-firm; `CancelOnDisconnect` flag optional
- **CAT Reporter Service**: asynchronous consumer of Kafka audit topic; formats CAT records per SEC schema; delivers via REST to CAT CAIS; retries with backoff; stores delivery receipts for compliance audit
- **DTCC/NSCC Interface**: end-of-day (and intraday for large trades) clearing interface; formats novation records; receives settlement confirmations; updates firm buying power after settlement
- **SIP Publisher**: publishes all trade prints and NBBO to the Securities Information Processor (CTA plan for NYSE-listed; UTP plan for NASDAQ-listed); required by Regulation NMS
- **Market Operations Console**: exchange ops tool for halt/resume, listing/delisting, and auction parameter management; all actions require dual-control authorization (two operators must approve)

### Unique Data Models
```sql
-- Member firm registry (unique to exchange)
member_firms: firm_id, mpid (4-char SEC-assigned Market Participant ID), membership_type (BROKER_DEALER | MARKET_MAKER | DMM), clearing_firm

-- Symbol registry (unique to exchange)
symbols: isin, cusip, listing_venue, lot_size, tick_size, price_collar_pct, circuit_breaker_pct, listing_date

-- Trading sessions (unique to exchange)
trading_sessions: session_id, firm_id, protocol (FIX_44 | OUCH | BINARY), connected_at

-- Audit log (Parquet on S3, S3 Object Lock compliance mode, 7-year retention)
Tamper-evident: each Parquet file's SHA-256 hash stored in a separate index file
```

### Unique Scalability / Design Decisions
- **Physical fairness**: co-located members get equal fiber cable lengths from their racks to the matching engine; prevents any single member from having a network latency advantage — a regulatory and operational fairness requirement
- **Dual data center**: NJ primary (Carteret) + NJ secondary (Mahwah) ~10 miles apart; synchronous WAL replication over dedicated dark fiber; < 30s automated failover
- **Market data multicast**: UDP multicast from exchange to co-located subscribers; one packet reaches all subscribers simultaneously (O(1) fan-out, not O(N)); critical for fair and low-latency data distribution

### Key Differentiator
An exchange platform adds **regulatory infrastructure on top of the matching engine**: member management, market-wide circuit breakers, SIP reporting, CAT compliance, and clearing integration. The unique challenge vs. a pure matching engine is the **dual mandate of fairness** (physical fiber length equality, equal market data dissemination) and **systemic stability** (circuit breakers, halts, auction mechanics at open/close).

---

## Retail Trading Platform (Robinhood / E*TRADE)

### Unique Functional Requirements
- Fractional shares: invest dollar amounts (e.g., $50 of AAPL); aggregator combines fractional orders into whole-share exchange lots; pro-rata allocation of fills back to users
- Pattern Day Trader (PDT) rule enforcement: accounts < $25K equity limited to 3 day trades in rolling 5 business days; real-time tracking and blocking
- Payment for Order Flow (PFOF): retail market orders routed to market makers (Citadel, Virtu) who pay broker per order; broker must document best execution for all PFOF routing
- KYC (Know Your Customer) and account opening: identity verification, AML screening, account funding via ACH/wire, options approval tiers (0–4)
- Options chains: display Greeks (delta, theta, gamma, vega) for all strikes and expirations; requires real-time options market data subscription
- Recurring investments: scheduled automatic buys (daily/weekly/monthly); cron-style execution with market open check

### Unique Components / Services
- **Fractional Aggregator**: collects fractional orders in 100ms windows; rounds up to whole shares; submits aggregated lot to exchange; pro-rata allocates fills back to users at uniform blended price
- **PDT Tracker**: Redis counter `pdt:<account_id>` with rolling 5-day TTL; PostgreSQL pdt_events for audit; real-time day trade detection at fill time (buy and sell same symbol same day)
- **PFOF Order Router**: routes retail market orders to contracted market makers first; documents routing rationale for SEC Rule 606 quarterly disclosures; switches to direct exchange routing if market maker at capacity or price improvement below threshold
- **KYC Service**: integrates with identity verification provider (e.g., Jumio, Persona); runs AML screening (OFAC watchlist); assigns options approval level based on account activity and user-stated experience
- **Push Notification Service**: APNs (iOS) + FCM (Android) for fill notifications; triggered by Kafka fill events; < 3s delivery SLA

### Unique Data Models
```sql
account_balances:
  cash_available, cash_pending (unsettled), cash_settled (T+1 settled)
  margin_buying_power (for margin accounts)
  → All protected by CHECK constraints and optimistic locking

orders: dollar_amount (fractional orders), execution_venue (CITADEL | VIRTU | IEX | NASDAQ | NYSE)
positions: qty DECIMAL(20,8) — supports fractional quantities to 8 decimal places

Redis keys unique to this problem:
  pdt:<account_id>          → Integer (day trade count, rolling 5 days TTL)
  idempotency:<order_id>    → String (prevents double-submission on app retry)
```

### Unique Scalability / Design Decisions
- **WebSocket fan-out at scale**: 1M+ concurrent WebSocket connections for live quotes; Redis Pub/Sub as intermediary between market data feed and N stateless WebSocket server nodes; each node subscribes to channels for its connected users' symbols; horizontal scaling of WebSocket nodes behind a sticky-session load balancer (for WebSocket upgrade handshake)
- **T+1 settlement awareness**: cash is tracked in three buckets (available, pending, settled); a buy using pending cash that then fails to settle (ACH return) creates a "good faith violation"; separate reconciliation job monitors settlement with custodian
- **Fractional share aggregation window**: 100ms batching window balances user experience (fast order feedback) vs. aggregation efficiency (more users per lot → better price averaging); adjustable by ops without code change (feature flag)

### Key Differentiator
The retail platform's unique challenge is **hiding institutional infrastructure complexity behind a consumer-grade UX**. The fractional share aggregation + PFOF routing combination is specific to retail: institutional clients trade whole shares via direct exchange access (DMA). The PDT rule, KYC, and options approval tiers are compliance layers that exist only for retail broker-dealers regulated under FINRA.

---

## Market Data Feed

### Unique Functional Requirements
- Multi-exchange ingestion: NYSE Pillar, NASDAQ ITCH 5.0, CBOE Multicast PITCH, OPRA (options) — each has a different binary format and sequence number space
- Sequence gap detection and recovery: detect missing UDP packets within < 50ms; request retransmission from exchange replay feed; fall back to TCP backup feed if retransmit fails
- NBBO computation with clock skew handling: use exchange_ts_ns (not ingestion_ts_ns) for ordering; 5ms late-arrival window for consolidated NBBO; "raw" NBBO without window for HFT consumers
- Entitlement enforcement: NYSE BBO and OPRA are licensed data feeds; track subscriber usage for billing; reject unauthorized symbol access
- Real-time OHLCV candle computation: rolling windows per symbol per interval; publish live candle updates every second; handle late-arriving ticks with candle revision events

### Unique Components / Services
- **Per-exchange Feed Handler**: one handler per exchange × feed line (primary + backup); ITCH decoder, Pillar decoder, PITCH decoder; sequence number tracking per feed; gap detection with 50ms retransmit timeout
- **NBBO Engine**: maintains per-symbol, per-exchange best bid/ask; recomputes NBBO on every quote update; single-threaded per symbol group (same reason as matching engine); late-arrival window for consolidated NBBO
- **UDP Multicast Publisher**: delivers NBBO updates to co-located HFT subscribers; one UDP packet reaches all subscribers on the multicast group; no ACK, no TCP overhead; receiver-side gap detection for robustness
- **Candle Engine**: in-memory rolling window aggregation per symbol × interval; ReplacingMergeTree in ClickHouse for deduplication when late ticks arrive after candle close
- **Entitlement Checker**: per-subscriber, per-symbol entitlement check at subscription time; billing counter incremented per subscribed symbol per day; Redis HyperLogLog for unique subscriber counting

### Unique Data Models
```
InternalQuote struct (100 bytes, off-heap, fixed-size for cache efficiency):
  symbol, exchange, bid, bid_size, ask, ask_size, last_trade_price, last_trade_qty,
  sequence_num, exchange_ts_ns, ingestion_ts_ns, flags (halted, cross, odd_lot)

NBBOState per symbol (in-memory only):
  best_bid/ask per exchange → consolidated NBBO across all exchanges
  seq_num: monotonic counter for subscribers to detect delivery gaps

Tick archive (Parquet on S3):
  Includes raw_message_bytes (original binary feed message) for deterministic backtest replay
  Partitioned by (asset_class, date, symbol) — enables efficient range queries

OHLCV in ClickHouse:
  ReplacingMergeTree engine → handles in-progress candle revisions (same candle key can be inserted multiple times as trades arrive; last version wins after merge)
```

### Unique Scalability / Design Decisions
- **Options volume dominates**: OPRA (options feed) is 10M messages/sec vs 2M/sec for equities; feed handler for OPRA needs dedicated CPU cores and NIC; equity and options feed handlers on separate physical machines
- **Dual delivery tiers**: HFT clients via UDP multicast (< 1ms, no guarantees); retail/analytics via WebSocket (< 10ms, reliable); using both from same normalized event stream via different publishers
- **Parquet with raw bytes**: storing the original binary feed message alongside normalized fields enables perfect historical replay for quantitative backtesting — you can reconstruct the exact book state at any nanosecond in history

### Key Differentiator
The market data feed's defining challenge is the **fan-out problem at extraordinary scale**: normalizing 12M events/sec from 5+ exchanges into a single canonical format, then delivering relevant updates to 100K+ subscribers with sub-10ms latency while also archiving everything for 5-year replay. The dual-delivery architecture (UDP multicast for speed, WebSocket for accessibility) and the clock-skew-aware NBBO computation are the signature design choices that distinguish this from a simple pub/sub system.

---

## Order Management System (OMS)

### Unique Functional Requirements
- Multi-fund, multi-account block orders with pre-trade allocation (allocation decided before order submitted — regulatory requirement for fairness)
- Algorithmic execution strategies (TWAP, VWAP, POV, Implementation Shortfall): schedule-driven order slicing over hours; adaptive adjustment based on real-time market volume
- Smart Order Routing (SOR): split large institutional orders across dark pools and lit venues to minimize market impact and information leakage; venue selection based on historical fill rates and market impact model
- Pre-trade compliance: per-fund investment guidelines (sector limits, ESG restrictions, short-sell prohibition), per-account position limits, regulatory restriction lists — must complete in < 20ms
- Post-trade TCA (Transaction Cost Analysis): compare actual fill prices to benchmarks (arrival price, VWAP, TWAP, implementation shortfall) for execution quality reporting to investment committee
- Confirm/Affirm workflow: FIX `Allocation` and `AllocationAck` messages to prime broker for settlement; track affirm status per account per trade

### Unique Components / Services
- **VWAP/TWAP Algo Engine**: computes execution schedule from historical volume profile (ClickHouse query at order creation); dispatches child orders per schedule via cron-style timer; adapts participation rate based on real-time market volume vs. expected
- **SOR (Smart Order Router)**: venue snapshot from market data cache (Redis, < 5ms stale); market impact model per venue per symbol; IOC orders to dark pools first (information protection); result aggregation and re-routing of unfilled remainder
- **Block Allocation Service**: pre-allocation stored before order submitted; post-fill pro-rata allocation at blended block price; exception handling for partial fills; generates FIX Allocation messages; prime broker integration
- **Compliance Engine**: investment guideline rules engine (Java/Drools or custom rule DSL); per-fund rules loaded from configuration; hot-reload on rule changes without OMS restart; fail-closed on unavailability
- **TCA Service**: ClickHouse queries over fills; benchmarks computed from market data archive; reports VWAP shortfall, implementation shortfall, spread capture per order and per PM; exported to reporting dashboard

### Unique Data Models
```sql
orders: parent_order_id (self-referential for parent-child hierarchy), algo_strategy, compliance_result, compliance_notes, version (optimistic lock)

fills: exchange_trade_id UNIQUE (prevents double-booking on FIX session reconnect)

allocations: allocation_method (PRO_RATA | SEQUENTIAL | MANUAL), status (PENDING | CONFIRMED | AFFIRMED)

order_events (CQRS event store): append-only; partition by order_id; clustering by sequence; JSONB payload
  → Source of truth for audit; current state materialized from events
  → Stored in PostgreSQL for < 24h; archived to S3 WORM after 24h

positions: qty BIGINT (can be negative for short positions); realized_pnl, last_price (from market data)
```

### Unique Scalability / Design Decisions
- **CQRS + Event Sourcing**: order state as an immutable event log enables exact replay for debugging and audit; current state is a materialized view of events; regulators can reconstruct any order's complete lifecycle from the event store
- **VWAP schedule pre-computation**: compute the full execution schedule at order creation (one ClickHouse query for historical volume profile) rather than recomputing every minute; schedule stored in Redis; Algo Engine reads from schedule — no per-slice computation overhead
- **Compliance hot-reload**: investment guidelines change frequently (new ESG restrictions, sector rebalances); compliance engine loads rules from config without OMS restart; rule change is itself audited (who changed what, when, why)
- **Multi-tenancy isolation**: 1,000+ portfolio managers across 50 funds; PostgreSQL row-level security by fund_id; each fund's data invisible to other funds; single OMS instance serves all funds (operational efficiency) with strict isolation guarantees

### Key Differentiator
The OMS's defining challenge is **translating investment intent (PM's block order) into optimized market execution (child orders across venues over time) while satisfying compliance, allocation fairness, and audit requirements**. The combination of pre-trade compliance, algo execution, SOR, and post-trade allocation is entirely absent from both the exchange platform (which just matches orders) and the retail platform (which routes individual orders). The CQRS event store is the backbone that enables the full lifecycle auditability regulators require.
