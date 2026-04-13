# System Design: Order Matching Engine

---

## 1. Requirement Clarifications

### Functional Requirements
1. Accept incoming orders: **Limit**, **Market**, **Stop**, **Stop-Limit**, and **Iceberg** orders.
2. Maintain a **per-symbol order book** with bid side (buy) and ask side (sell), sorted by price-time priority (best price first, FIFO within price level).
3. **Match orders** in real time: when a new order arrives, attempt to fill it against resting orders on the opposite side.
4. Produce **trade executions (fills)**: when a match occurs, emit a fill event with symbol, price, quantity, buyer order ID, seller order ID, and timestamp.
5. Emit **order book update events** after every change (new order, cancel, fill) for downstream market data distribution.
6. Support **order cancellation** and **modification** (cancel-replace).
7. Handle **partial fills**: an order may be filled in multiple pieces against multiple resting orders.
8. Enforce **price collars**: reject orders priced more than X% away from the last trade price (circuit breaker for erroneous orders).
9. Provide **order status queries**: clients can query the current status of any open or recently completed order.
10. Implement **market-open and market-close auctions**: collect all orders during pre-market, then run a single-price batch auction to determine opening/closing price.

### Non-Functional Requirements
1. **Latency**: order-to-fill notification P99 < 10 microseconds (μs) within the matching engine; end-to-end (network + engine) P99 < 500 μs for co-located clients.
2. **Throughput**: sustain 500,000 order events/second (new, cancel, modify) per symbol group; burst to 2M events/second during market open.
3. **Ordering**: strict deterministic ordering — two runs of the same input must produce identical outputs (required for audit and replay).
4. **Durability**: every accepted order and every fill must be durably logged before acknowledgment is returned to the sender.
5. **Consistency**: no order can be matched twice; no shares can be created or destroyed (conservation: total shares bought = total shares sold per match).
6. **Availability**: 99.999% during market hours (< 5.25 minutes/year); planned failover in < 1 second via hot standby.
7. **Auditability**: immutable, ordered log of every event replayed for regulatory audit (SEC Rule 17a-4, 7-year retention).
8. **Fairness**: strict price-time priority — no client receives preferential treatment at the same price level.

### Out of Scope
- Client-facing order entry API (handled by Order Management System)
- Post-trade settlement and clearing (handled by clearinghouse integration)
- Risk management pre-trade checks (handled by Risk Gateway upstream)
- Portfolio margin calculations
- Market maker obligations enforcement
- Options pricing or complex derivatives matching

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Market Maker | High-frequency order placer; co-located; submits 100K+ orders/day per symbol |
| Institutional Broker | Routes large client orders; may use algorithmic execution (TWAP/VWAP) |
| Retail Broker Gateway | Aggregates retail client orders; lower frequency; risk-checked upstream |
| Market Data Subscriber | Reads order book updates and fill events; does not send orders |
| Regulatory Auditor | Reads immutable order/fill log for compliance review |

### Traffic Estimates

**Assumptions:**
- 10,000 actively traded symbols (equities + ETFs)
- Matching engines partitioned: 1 engine per symbol group (~100 symbols/engine = 100 engine instances)
- Peak trading hours: market open/close 30-minute windows
- Average order lifetime: 50 ms (most HFT orders cancelled within milliseconds)

| Metric | Calculation | Result |
|---|---|---|
| New orders/day | 10K symbols × 50K orders/symbol/day | 500M orders/day |
| Cancel/modify events/day | ~70% of orders cancelled before fill | 350M cancels/day |
| Total order events/day | New + Cancel + Modify | ~900M events/day |
| Average events/sec | 900M / 23,400s (6.5h trading day) | ~38,500 events/sec |
| Peak events/sec (market open) | 10× average | ~385,000 events/sec |
| Fills/day | ~30% of orders result in a fill | ~150M fills/day |
| Order book updates/sec (per symbol) | ~100 updates/sec | 100 × 10K = 1M updates/sec |
| Audit log entries/day | 900M events + 150M fills | ~1.05B entries/day |
| Audit log storage/day | 1.05B × 200 bytes | ~210 GB/day |
| Audit log storage/7 years | 210 GB × 252 trading days × 7 years | ~370 TB |

### Latency Requirements
| Operation | P50 | P99 | Notes |
|---|---|---|---|
| Order acceptance (write to log) | < 1 μs | < 5 μs | In-process; kernel-bypass networking |
| Order matching (engine internal) | < 1 μs | < 10 μs | In-memory price level lookup |
| Fill notification to sender | < 50 μs | < 500 μs | Including network RTT within data center |
| Order book update to market data | < 100 μs | < 1 ms | Fan-out to subscribers |
| Order cancel | < 1 μs | < 10 μs | O(1) with hash map lookup |
| Audit log write (async) | < 1 ms | < 10 ms | Async to durable store; not on hot path |

### Storage Estimates
| Data | Size/record | Volume | Retention | Total |
|---|---|---|---|---|
| Order events (audit log) | 200 bytes | 900M/day | 7 years | ~370 TB |
| Fill records | 150 bytes | 150M/day | 7 years | ~58 TB |
| Order book snapshots (every 1s) | 10K symbols × 20 levels × 8 bytes | 1.6 MB/snapshot × 86,400s | 30 days | ~4.2 TB |
| In-memory order book (live) | 10K symbols × 1K resting orders × 100 bytes | | — | ~1 GB RAM per engine |

---

## 3. High-Level Architecture

```
                          ┌─────────────────────────────────────────────────────┐
                          │              ORDER ENTRY GATEWAY                     │
                          │  (FIX 4.4/5.0 protocol, binary OUCH/ITCH for HFT)  │
                          │  Kernel-bypass networking (DPDK / RDMA)             │
                          └──────────────────────┬──────────────────────────────┘
                                                 │  Validated orders (binary)
                          ┌──────────────────────▼──────────────────────────────┐
                          │              RISK GATEWAY (Pre-Trade)                │
                          │  - Buying power check (margin / cash)               │
                          │  - Position limit check                              │
                          │  - Price collar / fat-finger filter                 │
                          │  - Duplicate order detection (idempotency key)      │
                          └──────────────────────┬──────────────────────────────┘
                                                 │  Risk-cleared orders
                          ┌──────────────────────▼──────────────────────────────┐
                          │          ORDER SEQUENCER (Single-threaded)           │
                          │  - Assigns globally-monotonic sequence number        │
                          │  - Writes to Durable Log (WAL / Aeron / Chronicle)  │
                          │  - Publishes to in-process ring buffer               │
                          └──────────────────────┬──────────────────────────────┘
                                                 │  Sequenced order stream
                     ┌───────────────────────────▼──────────────────────────────────┐
                     │               MATCHING ENGINE CLUSTER                         │
                     │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
                     │  │  Engine 1   │  │  Engine 2   │  │  Engine N   │         │
                     │  │ Symbols A–J │  │ Symbols K–S │  │ Symbols T–Z │         │
                     │  │             │  │             │  │             │         │
                     │  │ In-memory   │  │ In-memory   │  │ In-memory   │         │
                     │  │ Order Books │  │ Order Books │  │ Order Books │         │
                     │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘         │
                     └─────────┼────────────────┼────────────────┼────────────────┘
                               │                │                │
              ┌────────────────▼────────────────▼────────────────▼──────────────┐
              │                   EVENT OUTPUT BUS                               │
              │  Fill Events ──────────────────────────────► Execution Reports  │
              │  Order Book Updates ───────────────────────► Market Data Feed   │
              │  All Events ───────────────────────────────► Audit Log (Kafka → │
              │                                                Parquet on S3)    │
              └────────────────────────────────────────────────────────────────┘
```

**Component roles:**

- **Order Entry Gateway:** Receives orders from external clients via FIX protocol (standard) or binary protocol (HFT/co-location). Handles session management, heartbeats, and protocol parsing. Uses kernel-bypass networking (DPDK or RDMA) to eliminate OS network stack overhead — reducing latency from ~10μs to ~1μs per message.
- **Risk Gateway:** Pre-trade risk checks in microseconds: buying power, position limits, price collars. Runs in-memory against a real-time risk state maintained by the Risk Service. Must complete in < 5μs to not dominate the matching latency budget.
- **Order Sequencer:** The most critical component for correctness. Assigns a globally monotonic sequence number to every accepted order. Writes the sequenced order to a durable WAL (Write-Ahead Log) before forwarding to the matching engine. This provides crash recovery: on restart, replay the WAL to rebuild exact in-memory state.
- **Matching Engine:** Single-threaded in-memory process. Receives sequenced orders from an in-process ring buffer (zero GC, zero allocation on hot path). Maintains price-level-sorted order book per symbol. Runs matching algorithm deterministically. Emits fill events and book update events.
- **Durable Log:** Append-only, fsync-on-write log. Can be Aeron (low-latency messaging), Chronicle Queue (off-heap mapped file), or a custom WAL. Purpose: exactly-once replay after crash. Not Kafka — Kafka's latency (2–5ms) is too high for the sequencing step.
- **Event Output Bus:** Publishes fills to execution report handlers and book updates to market data distribution. This path uses Kafka for fan-out (latency-tolerant downstream consumers).

**Primary use-case data flow (limit order submission → fill):**

1. Client sends FIX message: `NewOrderSingle` for 100 shares of AAPL at $180.00 limit buy
2. Gateway parses FIX → internal binary order struct, assigns session ID
3. Risk Gateway checks: sufficient cash? position limit OK? price within 5% collar? → passes
4. Sequencer assigns seq# 9,847,231, writes to WAL (fsync), publishes to ring buffer
5. Matching Engine receives order from ring buffer:
   - Looks up AAPL ask side (sell orders): lowest ask is $179.95 (100 shares resting)
   - Buy limit $180.00 ≥ ask $179.95 → match! Fill at $179.95 (resting order's price)
   - Emit fill: `{buyOrderId: X, sellOrderId: Y, symbol: AAPL, price: 179.95, qty: 100, seq: 9847231, ts: <nanosecond timestamp>}`
   - Remove resting sell order from book; incoming buy order fully filled
   - Emit order book update: ask side changed
6. Fill event → Execution Report Service → FIX ExecutionReport back to both buyer and seller
7. Fill event → Post-Trade Service → clearinghouse notification
8. Book update → Market Data Distribution → WebSocket push to subscribers

---

## 4. Data Model

### In-Memory Order Book Structure

```
// Per-symbol order book (one per symbol, in-memory only)
OrderBook {
    symbol:       string
    
    // Bid side: sorted descending by price (best bid = highest price first)
    bids:         PriceLevelMap   // price → PriceLevel
    
    // Ask side: sorted ascending by price (best ask = lowest price first)  
    asks:         PriceLevelMap   // price → PriceLevel
    
    // Fast cancel lookup: O(1) order retrieval by ID
    orderIndex:   HashMap<OrderId, Order>
    
    lastTradePrice: Decimal
    lastTradeQty:   Long
    lastTradeTime:  NanoTimestamp
    
    sequence:     Long            // monotonically increasing, matches sequencer
}

PriceLevel {
    price:        Decimal         // e.g., 180.00
    totalQty:     Long            // sum of all resting orders at this price
    orders:       Deque<Order>    // FIFO queue within price level (time priority)
}

Order {
    orderId:      UUID
    clientOrderId: String
    symbol:       String
    side:         Enum(BUY, SELL)
    type:         Enum(LIMIT, MARKET, STOP, STOP_LIMIT, ICEBERG)
    price:        Decimal         // null for MARKET orders
    qty:          Long
    filledQty:    Long
    status:       Enum(NEW, PARTIAL, FILLED, CANCELLED)
    timeInForce:  Enum(DAY, GTC, IOC, FOK)
    icebergPeakQty: Long          // null unless ICEBERG type
    submittedAt:  NanoTimestamp
    seqNum:       Long
    firmId:       String
}
```

### Durable Audit Log Schema (Parquet on S3)

```sql
-- order_events (Parquet, partitioned by date and symbol)
CREATE TABLE order_events (
    seq_num         BIGINT NOT NULL,          -- global monotonic sequence number
    event_type      VARCHAR(20) NOT NULL,     -- NEW | CANCEL | MODIFY | FILL | REJECT
    order_id        UUID NOT NULL,
    client_order_id VARCHAR(64),
    symbol          VARCHAR(20) NOT NULL,
    side            CHAR(1) NOT NULL,         -- B | S
    order_type      VARCHAR(12) NOT NULL,
    price           DECIMAL(18,6),            -- null for MARKET
    qty             BIGINT NOT NULL,
    filled_qty      BIGINT NOT NULL DEFAULT 0,
    fill_price      DECIMAL(18,6),            -- populated for FILL events
    contra_order_id UUID,                     -- the matched order on FILL events
    firm_id         VARCHAR(20) NOT NULL,
    time_in_force   VARCHAR(5) NOT NULL,
    event_ts        BIGINT NOT NULL,          -- nanosecond epoch timestamp
    engine_id       VARCHAR(20) NOT NULL,
    session_id      VARCHAR(40) NOT NULL
)
PARTITIONED BY (event_date DATE, symbol VARCHAR(20));
```

### Redis (Hot State — non-authoritative, reconstructible from WAL)

```
order:status:<order_id>         → Hash{status, filled_qty, avg_fill_price}  TTL=1h
book:snapshot:<symbol>          → Serialized top-10-level book snapshot      TTL=5s
risk:buyingpower:<firm_id>      → Decimal (current buying power)             TTL=30s
risk:position:<firm_id>:<sym>   → Integer (current net position)             TTL=30s
```

---

## 5. API Design

### FIX Protocol (Primary — for brokers and institutions)

```
// New Order (FIX Tag 35=D NewOrderSingle)
FIX Message NewOrderSingle:
  ClOrdID (11):        string (client's unique order ID, for idempotency)
  Symbol (55):         string (e.g., "AAPL")
  Side (54):           1=Buy | 2=Sell
  OrdType (40):        1=Market | 2=Limit | 3=Stop | 4=StopLimit | P=Iceberg
  Price (44):          decimal (required for Limit/StopLimit)
  StopPx (99):         decimal (required for Stop/StopLimit)
  OrderQty (38):       integer
  TimeInForce (59):    0=Day | 1=GTC | 3=IOC | 4=FOK
  MaxFloor (111):      integer (visible qty for Iceberg orders)
  TransactTime (60):   UTCTimestamp (nanosecond precision)

Response — ExecutionReport (FIX 35=8):
  OrderID (37):        string (exchange-assigned order ID)
  OrdStatus (39):      0=New | 1=PartialFill | 2=Filled | 4=Cancelled | 8=Rejected
  ExecType (150):      0=New | F=Trade | 4=Cancelled | 8=Rejected
  LastPx (31):         decimal (fill price, if ExecType=F)
  LastQty (32):        integer (fill quantity, if ExecType=F)
  CumQty (14):         integer (total filled qty)
  LeavesQty (151):     integer (remaining open qty)
  AvgPx (6):           decimal (average fill price)
  TransactTime (60):   UTCTimestamp (nanosecond)
```

### Internal REST API (for admin and ops tooling)

```
GET /api/v1/books/{symbol}
Description: Get current order book snapshot
Auth: Internal service auth (mTLS)
Response 200 OK:
  {
    "symbol": "AAPL",
    "timestamp": "2024-01-15T14:30:00.123456789Z",
    "sequence": 9847231,
    "bids": [
      { "price": "179.95", "qty": 500, "order_count": 3 },
      { "price": "179.90", "qty": 1200, "order_count": 8 }
    ],
    "asks": [
      { "price": "180.00", "qty": 300, "order_count": 2 },
      { "price": "180.05", "qty": 800, "order_count": 5 }
    ],
    "last_trade": { "price": "179.95", "qty": 100, "timestamp": "..." }
  }

GET /api/v1/orders/{order_id}
Description: Query order status
Auth: Firm JWT (can only query own orders)
Response 200 OK:
  {
    "order_id": "uuid",
    "symbol": "AAPL",
    "side": "BUY",
    "type": "LIMIT",
    "price": "180.00",
    "qty": 100,
    "filled_qty": 100,
    "avg_fill_price": "179.95",
    "status": "FILLED",
    "submitted_at": "...",
    "fills": [
      { "fill_id": "...", "price": "179.95", "qty": 100, "ts": "..." }
    ]
  }

POST /api/v1/engine/{engine_id}/halt
Description: Emergency halt (circuit breaker trigger)
Auth: Exchange operations team only (role=exchange_ops)
Body: { "reason": "market_disruption", "symbols": ["AAPL"] | "ALL" }
Response 200 OK: { "halted": true, "effective_ts": "..." }
```

---

## 6. Deep Dive: Core Components

### Component: Matching Algorithm (Price-Time Priority)

**Problem it solves:** When a new order arrives, determine which resting orders it should match against, in what order, and at what price. The algorithm must be deterministic, fair (no favoritism within a price level), and fast (< 1μs for typical case).

**All Possible Approaches:**

| Algorithm | Description | Pros | Cons |
|---|---|---|---|
| Price-Time Priority (FIFO) | Best price first; within same price, first order received first | Standard for most equities exchanges (NYSE, NASDAQ); maximally fair within a price level | Rewards co-location (fastest network wins); market makers must constantly refresh quotes |
| Pro-Rata | Best price first; within same price, fill proportional to each order's size | Used for interest-rate futures (CME); rewards large orders | Complex; market makers post artificially large orders and cancel remainder; gaming-prone |
| Allocation (size-priority) | Best price first; within same price, larger orders filled first | Encourages liquidity provision at large sizes | Unfair to small retail orders; less common for equities |
| FIFO with LMM allocation | Price-time, but a designated Lead Market Maker gets guaranteed allocation % | Used on some options exchanges; incentivizes DMMs | Added complexity; regulatory requirements for DMM eligibility |

**Selected Approach: Price-Time Priority (FIFO)**

Implementation with data structures:

```
// Data structure choice for order book sides:

Option A: Sorted Map (TreeMap / Red-Black Tree)
  - bids: TreeMap<Decimal, PriceLevel> sorted descending
  - asks: TreeMap<Decimal, PriceLevel> sorted ascending
  - Best bid/ask: O(1) via firstEntry()/lastEntry()
  - Insert new price level: O(log P) where P = number of distinct price levels
  - Remove price level when empty: O(log P)
  - In practice: P ~ 100-500 active price levels per side; log(500) ≈ 9 comparisons → negligible
  
Option B: Price-Level Linked List
  - Doubly-linked list of price levels sorted by price
  - Best bid/ask: O(1) (head pointer)
  - Insert at arbitrary price: O(P) worst case
  - Too slow for markets with many price levels spread far apart

Option C: Array-of-price-levels with offset indexing
  - Pre-allocate array indexed by (price - minPrice) / tickSize
  - O(1) insert, O(1) lookup for any price
  - Memory: at $0.01 tick size, AAPL $100-$300 range = 20,000 slots × 8 bytes = 160KB
  - Best for high-frequency, tight-spread instruments; used in LMAX Disruptor-based engines

Selected: TreeMap for correctness + simplicity; array-indexed for ultra-low-latency engines.

// Matching pseudocode (LIMIT BUY order arriving):
func match(incomingOrder: Order, asks: TreeMap<Price, PriceLevel>):
    while incomingOrder.leavesQty > 0 and asks is not empty:
        bestAsk = asks.firstEntry()  // O(1)
        
        if incomingOrder.price < bestAsk.price:
            break  // no match possible; incoming price is below best ask
        
        // Match at best ask price (resting order's price)
        matchPrice = bestAsk.price
        priceLevel = bestAsk.value
        
        while incomingOrder.leavesQty > 0 and priceLevel.orders is not empty:
            restingOrder = priceLevel.orders.peekFirst()  // O(1), FIFO
            fillQty = min(incomingOrder.leavesQty, restingOrder.leavesQty)
            
            emitFill(incomingOrder, restingOrder, matchPrice, fillQty)
            
            incomingOrder.filledQty += fillQty
            restingOrder.filledQty += fillQty
            priceLevel.totalQty -= fillQty
            
            if restingOrder.leavesQty == 0:
                priceLevel.orders.pollFirst()  // remove fully filled order
                orderIndex.remove(restingOrder.orderId)
        
        if priceLevel.orders is empty:
            asks.pollFirstEntry()  // remove empty price level
    
    if incomingOrder.leavesQty > 0:
        if incomingOrder.timeInForce == IOC:
            cancelRemainder(incomingOrder)  // IOC: cancel unfilled portion immediately
        elif incomingOrder.timeInForce == FOK:
            // FOK: if not fully filled, cancel entire order (pre-check before matching)
            cancelAll(incomingOrder)
        else:
            addToBook(incomingOrder, bids)  // rest in book (LIMIT, GTC, DAY)
```

**Interviewer Deep-Dive Q&A:**

Q: How does the matching engine handle Iceberg orders without leaking the true size to the market?
A: An Iceberg order has a `totalQty` and a `peakQty` (the visible portion). Only `peakQty` appears in the order book at any level. When the peak is fully filled, the engine automatically replenishes it from the hidden reserve, placing the refreshed peak at the back of the FIFO queue at that price level (losing time priority — this is intentional; the book has "seen" the peak execute). The hidden quantity is stored in the `Order` struct off-book. Externally, what appears in the Level 2 feed is only the peak quantity. The trade-off: iceberg orders get worse time priority on each replenishment, but hide large institutional size from price impact.

Q: What happens when two orders arrive simultaneously with the same sequence number?
A: This cannot happen by design — the Sequencer is single-threaded and assigns sequence numbers atomically from a monotonic counter. "Simultaneously" from the network perspective becomes strictly ordered at the Sequencer. The Sequencer is the serialization point for the entire system. This is why the Sequencer has a hot standby but not active-active replication — two sequencers would require a distributed consensus protocol (like Raft), adding latency. The trade-off: single Sequencer is a theoretical single point of failure, mitigated by < 1s hot standby failover.

Q: How do you handle a Market order when there is no liquidity on the other side?
A: Several protections: (1) **Collar check** in the Risk Gateway: a Market order will be rejected if the book's best price is more than X% from the last trade price, preventing "market order eating through the book." (2) **Minimum quantity check**: if the book has zero resting orders on the ask side for a Market Buy, the engine rejects the order with `OrdStatus=Rejected, ExecType=8` and reason "no liquidity." (3) **Trading halt**: if rapid price movement is detected (e.g., price moves 10% in 1 minute), the exchange can halt the symbol — the Risk Gateway rejects all incoming Market orders for that symbol until trading resumes. This is the exchange circuit breaker mechanism.

Q: Why is the matching engine single-threaded? Can't you parallelize it?
A: The matching engine is single-threaded per symbol group by design. Matching requires consistent state: you cannot match two orders on the same symbol in parallel without a lock, and a lock becomes the bottleneck. The single-threaded design eliminates all lock contention. The scale-out strategy is horizontal partitioning: 10,000 symbols split across 100 engine instances → each instance handles 100 symbols sequentially. Cross-symbol trades (e.g., ETF basket arbitrage) are handled at the trading strategy layer, not the matching engine. Within a single engine, the LMAX Disruptor ring buffer pattern achieves ~600M operations/second on a single core — far exceeding our 500K events/sec requirement.

---

### Component: Durable Order Log (WAL)

**Problem it solves:** The matching engine is in-memory. On crash, all open orders and book state are lost. The log must: (1) persist every accepted order before the engine processes it, (2) be replayable to reconstruct exact engine state, (3) not add significant latency to the matching path.

**All Possible Approaches:**

| Approach | Write Latency | Recovery Time | Notes |
|---|---|---|---|
| Kafka | 2–5 ms | Fast (replay from topic) | Too slow for sequencer path; fine for downstream |
| PostgreSQL WAL | 1–5 ms | Fast (pg_basebackup + WAL replay) | OLTP DB adds too much overhead |
| Chronicle Queue (memory-mapped file) | < 1 μs | Medium (replay file) | Off-heap, zero GC; used in production HFT systems |
| Aeron (low-latency messaging + persistent channel) | < 1 μs | Fast | RDMA-capable; designed for financial systems |
| Custom append-only mmap'd file + fsync | < 1 μs | Medium | Full control; used by exchanges like NASDAQ |

**Selected: Chronicle Queue / custom mmap'd WAL for sequencer path; Kafka for downstream fan-out.**

Reasoning: The sequencer must write before forwarding to the engine. At 500K events/sec, a 2ms Kafka write would require 1000 concurrent in-flight writes — impossible for a strict serial sequencer. Chronicle Queue uses a memory-mapped file that the OS flushes asynchronously via `msync(MS_ASYNC)`, with explicit `fdatasync()` called every N microseconds for durability batching. At 500K events/sec, a 100μs sync batch writes 50 events per fsync — reducing disk I/O to 10K fsyncs/sec (manageable for NVMe SSDs at 100K+ IOPS).

---

### Component: Hot Standby Failover

**Problem it solves:** The matching engine is a single-threaded, stateful process. If it crashes, the exchange is down. Recovery must happen in < 1 second to meet 99.999% availability during market hours.

**Approach:**

```
Active Engine ──────── Replication stream (seq-numbered events) ──────── Standby Engine
     │                                                                          │
     │ WAL write                                                                │ Applies same events
     │ Matching                                                                 │ Keeps identical book
     │                                                                          │
     ▼                                                                          │
Event Output Bus                                                     (suppressed output)

Failover:
1. Health monitor detects Active Engine missed 3 consecutive heartbeats (3 × 100ms = 300ms)
2. Standby promoted to Active: begins accepting sequencer input and emitting output
3. Total failover time: ~500ms (detection + promotion + reconnect)
4. Clients receive a brief gap in execution reports; retransmit requests handled by FIX resend mechanism
```

Key insight: The standby processes all events but suppresses output. On promotion, it simply starts emitting. The book state is identical to the active engine — no replay needed.

---

## 7. Failure Scenarios & Resilience

### Scenario 1: Order Sequencer Crash
- **Impact**: New orders cannot be accepted; in-flight orders may be lost.
- **Detection**: Health monitor via heartbeat (every 100ms).
- **Recovery**: Standby Sequencer takes over. WAL on shared storage (NVMe over Fabric) provides last committed sequence number. Standby resumes from seq+1. In-flight orders in transit (not yet written to WAL) are rejected — clients retry using FIX resend or application-level idempotency key. RTO: ~500ms.

### Scenario 2: Matching Engine Memory Corruption
- **Impact**: Incorrect fills or panics.
- **Detection**: Checksum validation on order book state every 100ms (hash of all resting order quantities per symbol). Mismatch triggers alert + halt.
- **Recovery**: HALT the affected symbol; replay WAL from last checkpoint to rebuild book state. Typical replay of 1 trading day's events: < 30 seconds. Halted symbol resumes after validation.

### Scenario 3: Network Partition Between Engine and Risk Gateway
- **Impact**: Risk Gateway cannot check incoming orders; engine cannot accept new orders.
- **Recovery**: Fail-closed: orders are queued at the gateway, not forwarded to the engine, until connectivity restores. Unlike the market data path (fail-open acceptable), the risk path must fail-closed. Queue depth limit: 10K orders; beyond that, orders are rejected with `ExecType=Rejected, text="risk gateway unavailable"`.

### Scenario 4: Runaway Algorithm / Fat Finger
- **Detection**: Price collar check at Risk Gateway (reject if >5% from last trade). Per-firm order rate limiter (> 5K orders/sec from one firm → throttle). Per-symbol circuit breaker (> 10% price move in 5 minutes → trading halt).
- **Recovery**: Trading halt issued; exchange ops team reviews; manual resume after validation.

### Scenario 5: Clock Skew Between Engine Instances
- **Impact**: Sequence numbers from different engines could appear out of order in the audit log.
- **Prevention**: Each engine uses a single monotonic clock (not wall clock). Sequence numbers from the Sequencer are the authoritative ordering — not timestamps. Timestamps are for human readability and are supplemental. Exchange uses GPS-synchronized PTP clocks (IEEE 1588) across all data center nodes, achieving < 100ns clock accuracy.

---

## 8. Monitoring & Observability

| Metric | Alert Threshold | Purpose |
|---|---|---|
| Order-to-fill latency P99 | > 50 μs | Matching engine performance |
| Sequencer throughput | < 80% of SLA | Sequencer health |
| WAL write latency P99 | > 10 μs | Disk I/O health |
| Order book depth (best bid-ask spread) | Spread > 2% | Liquidity monitoring |
| Fill rate (fills / new orders) | Sudden drop > 50% | Engine or connectivity issue |
| Engine heartbeat | Missed > 3 | Trigger failover |
| Risk rejection rate | > 1% of orders | Anomalous client behavior |
| Audit log lag | > 1s behind real time | Log pipeline health |
| Circuit breaker activations | Any | Immediate ops notification |
