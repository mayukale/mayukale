# Common Patterns — Stock Market Trading (21_stock_market_trading)

## Common Components

### In-Memory Order Book (Custom Data Structure)
- Used as: the core stateful component of any matching engine; maintains bid and ask sides per symbol sorted by price-time priority
- In order_matching_engine: TreeMap<Price, PriceLevel> for each side; PriceLevel holds a FIFO Deque<Order>; HashMap<OrderId, Order> for O(1) cancel lookup; single-threaded per symbol group; in-process ring buffer (LMAX Disruptor) for zero-GC hot path
- In stock_exchange_platform: same engine per symbol group; hot standby replication of in-memory state via event replay; symbol group partitioned across N engine instances
- In order_management_system: OMS maintains a lightweight "virtual" order book of its own orders per venue per symbol to compute estimated market impact before routing

### WAL (Write-Ahead Log) — Chronicle Queue / Custom mmap
- Used as: durable sequencing layer that makes in-memory state recoverable after crash; written synchronously before event reaches matching engine
- In order_matching_engine: Chronicle Queue on NVMe SSD; every accepted order written with sequence number before ring buffer dispatch; on crash: replay WAL from last committed sequence to rebuild exact book state
- In stock_exchange_platform: WAL replicated synchronously to hot standby site (NVMe-oF fabric); enables < 1s failover with zero data loss
- Key config: `fdatasync()` called every 100μs batch (not every message) — trades 100μs durability window for 10x throughput improvement

### Kafka (Downstream Fan-out, Not Hot Path)
- Used as: event bus for all downstream consumers (analytics, audit log, clearing, position updates, market data archive); never used on the order-acknowledgment critical path
- In order_matching_engine: fill events + order book updates published to Kafka after acknowledgment sent; partition key = symbol for ordered processing per symbol
- In stock_exchange_platform: all events → Kafka → CAT reporter, clearing integration, market data archive; 500 partitions for high-parallelism
- In trading_platform: fill events → Kafka → Position Service (portfolio update), Notification Service (push), TCA analytics
- In order_management_system: order events + fill events published to Kafka for accounting, risk, and reporting systems; event store pattern
- In market_data_feed: normalized tick events published to Kafka for tick archive pipeline and downstream risk engine; latency-tolerant consumers use Kafka, latency-critical use Redis Pub/Sub or UDP multicast

### Redis Cluster
- Used as: hot cache for real-time state that is reconstructible from authoritative source; never used as source of truth for financial data
- In trading_platform: balance cache (TTL=30s), position snapshot (TTL=60s), idempotency key dedup (TTL=24h), PDT counter, quote cache
- In order_management_system: order status cache, position cache per fund (TTL=10s), FIX session state (active orders per session), compliance check results (TTL=5min)
- In market_data_feed: NBBO state cache (TTL=1s per symbol), WebSocket fanout via Redis Pub/Sub channels (`quote:<symbol>`), subscription registry (which symbols each WebSocket node serves)
- Key policy: `allkeys-lru` eviction; all cached values have TTLs; cache miss always falls back to authoritative store (PostgreSQL or in-memory engine state)

### PostgreSQL (ACID Source of Truth)
- Used as: authoritative store for all structured financial metadata: orders, positions, accounts, allocations, fills; ACID guarantees prevent double-fills and negative balances
- In trading_platform: accounts, account_balances (optimistic lock via version column, CHECK balance >= 0), orders, positions; PgBouncer connection pooling; primary + 2 async read replicas; Patroni for HA
- In order_management_system: orders, fills, positions, allocations, order_events (CQRS event store); sharded by fund_id; version column for optimistic locking on positions and orders
- In stock_exchange_platform: member firm registry, symbol registry, trading sessions; lower write rate (metadata only); not used for tick data or fill audit
- Key pattern: optimistic locking — `UPDATE ... WHERE version = :expected_version` — avoids distributed locks while preventing concurrent modification races

### Cassandra / Append-Only Log (Audit Trail)
- Used as: immutable, high-write audit trail for all order events — never updated, never deleted, write-optimized LSM-tree storage
- In order_management_system: order_events table (append-only CQRS event store); partition by order_id; clustering by sequence number; RF=3, LOCAL_QUORUM; 7-year retention, TTL=0
- In stock_exchange_platform: CAT reporting source; compliance audit trail; all order events with nanosecond timestamps; tamper-evident via SHA-256 chaining
- Alternative: S3 WORM + Parquet for audit storage at exchange scale (immutability enforced by S3 Object Lock in compliance mode, 7-year retention)

### ClickHouse (OLAP — TCA, Analytics, Historical)
- Used as: columnar store for analytics queries over billions of order and fill records; not used on the trading hot path
- In trading_platform: execution quality analysis, platform-wide order flow analytics, regulatory best execution reports (Rule 606)
- In order_management_system: TCA (Transaction Cost Analysis) — compare fill prices to benchmarks (VWAP, TWAP, arrival price); historical P&L reports; VWAP schedule computation (uses historical volume profiles stored in ClickHouse)
- In market_data_feed: OHLCV candle storage (ReplacingMergeTree for in-progress candle updates); 5-year tick archive queryable by symbol and time range; partitioned by (symbol, date)

### FIX Protocol Engine
- Used as: industry-standard messaging protocol for order entry, execution reports, and broker connectivity
- In order_matching_engine: exchange accepts FIX 4.4/5.0 and binary OUCH/ITCH from members; FIX session layer handles sequence numbers, heartbeats, resend requests, and session recovery
- In order_management_system: OMS sends `NewOrderSingle (D)`, `OrderCancelRequest (F)`, `OrderCancelReplaceRequest (G)` to brokers/exchanges; receives `ExecutionReport (8)` for all order status changes; FIX session tracked with sequence number state for gap recovery
- In trading_platform: backend broker uses FIX to route to market makers (PFOF venues) or exchanges; retail app layer uses REST/WebSocket (never FIX directly)
- Key pattern: FIX sequence number gap recovery — on reconnect, send `ResendRequest` for all messages after last confirmed seqnum; broker retransmits all fills in the gap

### Circuit Breaker Pattern (Per Service + Per Symbol)
- Used as: both a trading safeguard (halt trading when price moves too fast) and a software resilience pattern (stop calling a failing external service)
- In order_matching_engine: per-symbol circuit breaker: halt if price moves > 10% in 5 minutes (regulatory Level 1/2/3 triggers); price collar: reject orders > 5% from last trade price (fat-finger protection)
- In trading_platform: software circuit breaker on exchange FIX connection (Resilience4j / custom); OPEN after 3 failed FIX sends in 5s; HALF_OPEN after 10s; on OPEN: queue orders, notify user
- In market_data_feed: per-feed circuit breaker: if sequence gap unrecovered > 500ms, mark symbol NBBO as stale, alert subscribers; switch to TCP backup feed; fail-open (keep last known NBBO rather than showing error)
- In order_management_system: circuit breaker on compliance service: fail-closed (queue orders, reject new orders); circuit breaker on broker connectivity: retry with exponential backoff

### Kernel-Bypass Networking (DPDK / RDMA)
- Used as: eliminates Linux kernel network stack overhead to achieve < 1μs network latency for the order-to-fill path
- In order_matching_engine: DPDK (Data Plane Development Kit) for UDP packet processing without kernel syscalls; reduces network latency from ~10μs to ~1μs; used for co-located member connections and inter-engine communication
- In stock_exchange_platform: RDMA (Remote Direct Memory Access) over InfiniBand for WAL replication to hot standby site; reduces replication latency from ~100μs to ~5μs
- In market_data_feed: UDP multicast delivery to co-located HFT subscribers uses DPDK on the feed distribution side; receiver side uses DPDK too (HFT firms use this in their market data handlers)
- Note: not used in retail trading platform (network latency to retail clients is dominated by internet RTT, not kernel overhead)

## Common Databases

### PostgreSQL
- trading_platform, order_management_system, stock_exchange_platform: ACID, optimistic locking via version column, PgBouncer pooling, Patroni HA, primary + 2 async read replicas
- Schema: always includes `version BIGINT NOT NULL DEFAULT 0` on mutable financial tables for optimistic locking

### ClickHouse
- market_data_feed, order_management_system, trading_platform: ReplacingMergeTree for upsertable time-series (candles), MergeTree for append-only analytics (ticks, TCA); partitioned by (symbol/fund, date)

### In-Memory State (Ring Buffer + Custom Structs)
- order_matching_engine, stock_exchange_platform: no DB on the hot path; all order book state is in-memory, structured as fixed-size off-heap structs to avoid GC pauses (Java/C++ with Chronicle Map or direct `malloc`)

## Common Queues / Event Streams

### Kafka
- All 5 problems: downstream fan-out, not on the latency-critical path; partition key = primary entity key (symbol or order_id); replication factor 3; retention 7 days (replay window)

### WAL (Write-Ahead Log)
- order_matching_engine, stock_exchange_platform: on-path durability; Chronicle Queue or custom mmap'd append-only file; fsync batching for throughput; enables deterministic replay for crash recovery

### UDP Multicast (Market Data)
- market_data_feed, order_matching_engine: low-latency fan-out to co-located subscribers; no ACK required; packet loss handled by sequence gap detection + retransmit request

## Common Communication Patterns

### FIX Protocol (External Connectivity)
- order_matching_engine, stock_exchange_platform, order_management_system: industry standard for order entry and execution reports; FIX 4.4 / 5.0 for standard; binary OUCH/ITCH for HFT co-location

### WebSocket (Retail / Non-HFT Subscribers)
- trading_platform, market_data_feed: streaming price updates, order status updates; Redis Pub/Sub as fan-out intermediary between feed handler and N WebSocket nodes

### REST (CRUD Operations + Historical Queries)
- trading_platform, market_data_feed, order_management_system: order entry, account management, historical data queries; all authenticated via JWT or mTLS

### Optimistic Locking (No Distributed Locks)
- trading_platform, order_management_system, stock_exchange_platform: `UPDATE ... WHERE version = :expected_version` on positions, orders, and balances; 0 rows → conflict → retry; avoids distributed lock overhead while preventing concurrent modification races

## Common Scalability Techniques

### Single-Threaded Engine Per Partition (No Lock Contention)
- order_matching_engine, market_data_feed: single-threaded per symbol group eliminates all lock contention; scale-out via horizontal partitioning (10K symbols / 100 engine instances = 100 symbols/engine); sequential processing within partition is faster than parallel with locks

### Event Sourcing / CQRS
- order_management_system: append-only order_events table is the source of truth; current state is a projection/materialization of events; enables replay for debugging, audit, and state reconstruction after failures

### Pre-Computation / Schedule-Based Execution
- order_management_system (VWAP/TWAP): compute execution schedule at order creation; dispatch slices on a timer rather than recomputing on every market update; reduces compute overhead on the hot path

### Idempotency Keys on All Write Operations
- trading_platform: `Idempotency-Key` header on all POST /orders; Redis SETNX for fast in-flight dedup + PostgreSQL UNIQUE constraint (client_order_id) as durable safety net
- order_management_system: `client_order_id` UNIQUE in orders table; fills have `exchange_trade_id` UNIQUE in fills table — prevents double-booking of the same fill on reconnect

## Common Deep Dive Questions

### How do you guarantee exactly-once fill processing?
Answer: Two-layer approach. (1) At the FIX session level: sequence numbers detect duplicates on reconnect; `ResendRequest` mechanism retransmits fills, but OMS deduplicates by `exchange_trade_id` (UNIQUE constraint in fills table) — a duplicate fill is silently discarded. (2) At the Kafka consumer level: Kafka transactions + exactly-once semantics (idempotent producer + transactional consumer) ensure each fill event processed exactly once. Position updates use optimistic locking: if `version` mismatch on `UPDATE positions`, retry.
Present in: order_management_system, trading_platform, stock_exchange_platform

### How do you prevent an order from being double-submitted to an exchange?
Answer: Every order has an `Idempotency-Key` (UUID) set by the OMS or client. Before submission: Redis `SETNX idempotency:<key> <order_id>` with 24h TTL. If Redis key already exists: the order was already submitted; return original order_id without re-submitting. If Redis fails: check PostgreSQL `UNIQUE (client_order_id)` — INSERT will fail with conflict if already submitted. FIX ClOrdID field serves as the exchange-level idempotency key (exchange rejects duplicate ClOrdIDs within a session).
Present in: all five problems

### How do you ensure the order book is correct after a crash and restart?
Answer: The Order Sequencer writes every accepted order to a WAL (Write-Ahead Log) with a sequence number before forwarding to the matching engine. On restart: replay the WAL from sequence 1 to the last committed entry; the matching engine processes all events in order, rebuilding identical in-memory book state. This is deterministic: same input sequence → same output state. Two optimizations: (1) periodic snapshots of book state to disk (every 10 minutes) reduce replay time; (2) WAL is truncated after snapshot (only keep events after last snapshot). Recovery time: replay from last snapshot + subsequent WAL events = typically < 30 seconds.
Present in: order_matching_engine, stock_exchange_platform

### How do you handle clock skew between the matching engine and external clients?
Answer: For latency measurement: use exchange-timestamp (set by exchange at event acceptance) as the authoritative time, not ingestion timestamp (when our system received it). For ordering within the book: use sequence number (assigned by sequencer), not timestamps — sequence number is strictly monotonic and unaffected by clock skew. For regulatory reporting: use PTP (IEEE 1588) GPS-synchronized clocks achieving < 100ns accuracy; required by SEC for CAT reporting with nanosecond precision.
Present in: order_matching_engine, stock_exchange_platform, market_data_feed

### How do you handle a sudden 10x traffic spike at market open?
Answer: (1) Matching engine: single-threaded, processes ~600M ops/sec per core (LMAX Disruptor); our 2M events/sec peak is a small fraction of capacity. Bottleneck is usually the WAL write speed — addressed by fsync batching. (2) Market data WebSocket: Redis Pub/Sub fan-out scales horizontally; add WebSocket nodes pre-emptively before market open (auto-scaling triggered at 9:20 AM scheduled job). (3) Order entry gateway: stateless, auto-scaled behind load balancer; pre-scale to 2x capacity before market open. (4) Risk gateway: in-memory checks are fast; the bottleneck is Redis balance reads — Redis Cluster handles 1M+ ops/sec. (5) Database: PostgreSQL write path is not on the hot path (fills written asynchronously via Kafka); read replicas serve position queries.
Present in: all five problems

## Common NFRs

- **Market hours availability**: 99.999% during market hours (9:30 AM – 4:00 PM ET); maintenance only in off-hours windows
- **Financial data durability**: Zero data loss for fills and orders; WAL-backed or ACID-backed all financial state
- **Regulatory retention**: All order events, fills, and audit records retained 7 years (SEC Rule 17a-4, FINRA Rule 4511)
- **Latency tiers**: Exchange/HFT path: < 1μs (kernel bypass); institutional OMS: < 50ms; retail trading: < 500ms end-to-end
- **Fairness / no preferential treatment**: Strict price-time priority; equal network treatment for same-tier members (physical fairness via equal fiber length in co-location environments)
- **Idempotency**: Every write operation must be idempotent — network retries and FIX session reconnects must never cause duplicate fills or double orders
- **Consistency model**: Strong consistency for financial balances (positions, cash); eventual consistency acceptable for analytics and reporting (TCA, performance attribution)
