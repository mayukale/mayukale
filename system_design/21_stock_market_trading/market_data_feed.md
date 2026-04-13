# System Design: Market Data Feed & Distribution Platform

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Ingest** real-time market data from multiple exchanges (NYSE, NASDAQ, CBOE, IEX) and alternative trading systems (ATSs) via proprietary binary feeds (NASDAQ ITCH, NYSE Pillar, OPRA for options).
2. **Normalize** raw exchange-specific binary messages into a unified internal format (symbol, bid, ask, last trade, volume, exchange code, timestamp).
3. **Consolidate** prices across exchanges to compute the **NBBO (National Best Bid and Offer)** — the best bid and best ask across all venues for each security.
4. **Maintain live order book** (Level 2 data) per security per exchange, and a consolidated book.
5. **Distribute** real-time Level 1 (NBBO + last trade) and Level 2 (full depth) data to subscribers via WebSocket, Kafka, and low-latency multicast UDP.
6. **Historical tick data**: Store all normalized tick events; support queries by symbol, exchange, and time range (up to 5 years back).
7. **OHLCV candles**: Compute and serve 1-minute, 5-minute, 15-minute, 1-hour, and 1-day candles in real time and historically.
8. **Market status**: Publish exchange open/close, halt, and resume events to all subscribers.
9. **Reference data**: Serve static reference data (symbol → CUSIP, lot size, tick size, exchange listing) with change notifications.
10. **Entitlement enforcement**: Some data feeds (NYSE BBO, OPRA) require licensing; enforce subscriber entitlements and audit usage for billing.

### Non-Functional Requirements
1. **Latency**: Normalized event published internally in < 1 ms from raw feed receipt; subscriber delivery P99 < 10 ms from exchange timestamp.
2. **Throughput**: Ingest up to 10M messages/second peak (options OPRA feed); equities peak ~2M/sec.
3. **Availability**: 99.999% during market hours; feed gaps trigger automatic recovery and gap-fill from exchange replay.
4. **Ordering**: Per-symbol, per-exchange ordering must be preserved. NBBO computation must use exchange-timestamp-ordered events, not ingestion-timestamp-ordered.
5. **Completeness**: No message loss from exchange feed; sequence number gaps trigger gap-fill request.
6. **Scalability**: Support 100K+ concurrent subscribers with 500-symbol watchlists each.
7. **Auditability**: All raw feed messages archived in original binary format for 7 years (regulatory compliance + backtest reproducibility).
8. **Multi-asset**: Support equities, ETFs, and options chains (same platform, different decoders).

### Out of Scope
- Execution / order routing
- Portfolio analytics or P&L computation
- Fundamental data (earnings, financials)
- Alternative data (news, social sentiment)
- FX or fixed income feeds (separate asset classes)

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Retail Trading Platform | Consumes Level 1 (NBBO) for portfolio and watchlist display; 1–2s delay acceptable |
| Active Trader / HFT | Consumes Level 2 (full depth) via multicast UDP; < 1ms delivery required |
| Quant / Research | Consumes historical tick data and OHLCV via REST/batch API |
| Risk Engine (internal) | Consumes real-time prices for pre-trade risk checks; ultra-low latency |
| Market Data Vendor | Resells data to end clients (Bloomberg, Refinitiv); aggregates and re-packages |
| Algorithmic Trading System | Subscribes to full symbol universe or sector; processes events in trading algorithms |

### Traffic Estimates

**Assumptions:**
- 10,000 equities + 500,000 options contracts (options chain makes up bulk of OPRA volume)
- Equities: 2M messages/sec peak (market open)
- Options (OPRA): 10M messages/sec peak
- Average normalized event size: 100 bytes
- Subscribers: 100K concurrent; average 100 symbols watched

| Metric | Calculation | Result |
|---|---|---|
| Peak equities ingest | 2M msg/sec × 100 bytes | 200 MB/s (1.6 Gbps) |
| Peak options ingest (OPRA) | 10M msg/sec × 100 bytes | 1 GB/s (8 Gbps) |
| Total peak ingest | Equities + options | ~1.2 GB/s (9.6 Gbps) |
| NBBO compute events/sec | 2M equities updates | 2M NBBO recomputations/sec |
| WebSocket push deliveries/sec | 100K subscribers × 100 symbols × 2 updates/sec avg | 20M deliveries/sec |
| Multicast push deliveries/sec | 1K HFT subscribers × full universe | ~12M deliveries/sec (separate) |
| Tick data storage/day | 12M msg/sec total × 6.5h × 3600s × 100 bytes | ~28 TB/day raw |
| Compressed storage (Parquet, 10:1) | 28 TB / 10 | ~2.8 TB/day |
| 5-year archive | 2.8 TB × 252 days × 5 years | ~3.5 PB |
| OHLCV candle storage | 10K symbols × 5 intervals × 252 days × 50 bytes × 5 years | ~3.2 GB (trivial) |

### Latency Requirements
| Stage | P50 | P99 | Notes |
|---|---|---|---|
| Raw feed → normalized event | < 100 μs | < 1 ms | In-memory decoding, kernel-bypass |
| Normalized event → NBBO update | < 100 μs | < 500 μs | In-memory aggregation |
| NBBO update → multicast publish | < 200 μs | < 1 ms | UDP multicast, no TCP overhead |
| NBBO update → WebSocket push | < 2 ms | < 10 ms | TCP fan-out; acceptable for non-HFT |
| Ingest → Kafka (for downstream) | < 5 ms | < 20 ms | Kafka producer async batch |
| Historical tick query (1 day) | < 500 ms | < 2 s | Parquet scan on S3 via Athena |
| Historical tick query (1 year) | < 2 s | < 10 s | ClickHouse columnar query |

---

## 3. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     EXCHANGE FEED SOURCES                                   │
│  NYSE (Pillar)   NASDAQ (ITCH 5.0)   CBOE (Multicast PITCH)   IEX (TOPS)  │
│  OPRA (options)  ATS feeds           Dark pool prints          SIP (CTA/UTP)│
└──────────┬──────────────┬────────────────────┬──────────────────┬───────────┘
           │              │                    │                  │
           │  Binary UDP multicast / dedicated TCP feed handlers  │
┌──────────▼──────────────▼────────────────────▼──────────────────▼───────────┐
│                        FEED HANDLER LAYER                                   │
│  ┌─────────────────┐  ┌──────────────────┐  ┌──────────────────────────┐   │
│  │  NYSE Handler   │  │  NASDAQ Handler  │  │  OPRA Handler            │   │
│  │  Pillar decoder │  │  ITCH decoder    │  │  Options quote decoder   │   │
│  │  Seq# tracking  │  │  Seq# tracking   │  │  Seq# tracking           │   │
│  │  Gap detection  │  │  Gap detection   │  │  Gap detection           │   │
│  └────────┬────────┘  └──────┬───────────┘  └──────────┬───────────────┘   │
└───────────┼─────────────────┼─────────────────────────┼───────────────────┘
            │                 │                         │
            └─────────────────┴─────────────────────────┘
                              │  Normalized InternalQuote events
┌─────────────────────────────▼───────────────────────────────────────────────┐
│                     NORMALIZATION & NBBO ENGINE                             │
│  - Converts exchange-specific formats to InternalQuote struct              │
│  - Maintains per-exchange, per-symbol best bid/ask in memory               │
│  - Computes NBBO across all exchanges for each symbol                      │
│  - Clock skew handling: uses exchange timestamp, not ingestion timestamp   │
│  - Single-threaded per symbol group (deterministic NBBO computation)       │
└─────────────────────────────┬───────────────────────────────────────────────┘
                              │  NBBO updates + Level 2 book updates
          ┌───────────────────┼────────────────────────────────┐
          │                   │                                │
┌─────────▼────────┐ ┌────────▼──────────────────┐ ┌──────────▼─────────────┐
│  Multicast UDP   │ │  WebSocket Distribution   │ │  Kafka Publisher       │
│  Publisher       │ │  Service                  │ │                        │
│  - Co-located    │ │  - 100K+ concurrent conns │ │  - topic: market-data  │
│  - HFT clients   │ │  - Symbol subscription    │ │  - Downstream: storage,│
│  - < 1ms latency │ │  - Pub/sub via Redis      │ │    risk engine,        │
│  - UDP multicast │ │  - Redis fanout channels  │ │    analytics           │
│  - No ACK        │ │  - Horizontal scale       │ │                        │
└──────────────────┘ └────────┬──────────────────┘ └──────────┬─────────────┘
                              │                               │
                  ┌───────────▼───────────────┐  ┌────────────▼─────────────┐
                  │  Candle Computation Engine │  │  Tick Storage Pipeline  │
                  │  (1m, 5m, 15m, 1h, 1d)   │  │  Kafka → Flink → Parquet │
                  │  In-memory rolling windows │  │  on S3 + ClickHouse      │
                  │  Published to Kafka        │  │  (5-year archive)        │
                  └───────────────────────────┘  └──────────────────────────┘
```

**Primary use-case data flow (NASDAQ bid update → NBBO → WebSocket subscribers):**

1. NASDAQ sends binary ITCH `Add Order` message via UDP multicast: AAPL, bid $179.95 × 500 shares
2. NASDAQ Feed Handler: receives UDP packet, validates sequence number (seq 4,782,331 = expected), decodes ITCH binary format
3. Normalizer: creates `InternalQuote{symbol: AAPL, exchange: NASDAQ, bid: 179.95, bid_size: 500, ts_ns: <exchange_ts>}`
4. NBBO Engine: compares NASDAQ bid ($179.95) against all other exchanges:
   - NYSE best bid: $179.90
   - CBOE best bid: $179.92
   - IEX best bid: $179.95 (tie)
   - New NBBO bid: $179.95 (NASDAQ tied with IEX) → NBBO bid size: 500 + 300 = 800
5. NBBO Engine emits NBBO update event
6. Multicast Publisher: sends UDP packet to all co-located HFT subscribers on multicast group for AAPL within 200μs
7. WebSocket Distribution Service: Redis PUBLISH to channel `quote:AAPL` → all WebSocket nodes subscribed to AAPL receive event → push to relevant WebSocket clients in < 10ms
8. Kafka Publisher: async batch publish to `market-data` topic (partition key: symbol) for downstream consumers
9. Candle Engine: updates AAPL 1-minute rolling window with new bid
10. Tick Archive: Flink consumer batch-inserts tick to ClickHouse every 1 second

---

## 4. Data Model

### InternalQuote (In-Memory, Normalized)
```
InternalQuote struct (100 bytes, fixed-size, off-heap):
    symbol          [8 bytes]   // e.g., "AAPL    " (padded)
    exchange        [4 bytes]   // NYSE | NSDQ | CBOE | IEX  
    bid             [8 bytes]   // IEEE 754 double or fixed-point int64 (price × 10^6)
    bid_size        [8 bytes]   // shares
    ask             [8 bytes]   // 
    ask_size        [8 bytes]   //
    last_trade_price [8 bytes]  //
    last_trade_qty  [8 bytes]   //
    sequence_num    [8 bytes]   // exchange sequence number
    exchange_ts_ns  [8 bytes]   // exchange timestamp (nanoseconds)
    ingestion_ts_ns [8 bytes]   // when we received it
    flags           [4 bytes]   // bit flags: is_halted, is_cross, is_odd_lot, etc.
    padding         [16 bytes]  // cache-line aligned (total: 100 bytes)
```

### NBBO State (In-Memory, Per Symbol)
```
NBBOState struct (per symbol):
    symbol:             string
    best_bid:           PriceSizeExchange[]   // top bids per exchange, sorted descending
    best_ask:           PriceSizeExchange[]   // top asks per exchange, sorted ascending
    nbbo_bid:           double
    nbbo_bid_size:      long
    nbbo_bid_exchange:  string
    nbbo_ask:           double
    nbbo_ask_size:      long
    nbbo_ask_exchange:  string
    last_trade_price:   double
    last_trade_qty:     long
    last_trade_ts_ns:   long
    seq_num:            long                  // monotonic for subscribers to detect gaps
```

### Tick Archive Schema (Parquet on S3)
```sql
-- Partitioned by date and symbol for efficient range queries
-- Path: s3://market-data-archive/asset_class=equity/date=2024-01-15/symbol=AAPL/
CREATE TABLE ticks (
    symbol              VARCHAR(20),
    exchange            VARCHAR(10),
    event_type          VARCHAR(20),    -- QUOTE | TRADE | ORDER_ADD | ORDER_CANCEL | HALT
    exchange_ts_ns      BIGINT,         -- nanosecond epoch, from exchange
    ingestion_ts_ns     BIGINT,         -- when our system received it
    bid                 DOUBLE,
    bid_size            BIGINT,
    ask                 DOUBLE,
    ask_size            BIGINT,
    trade_price         DOUBLE,
    trade_qty           BIGINT,
    sequence_num        BIGINT,
    raw_message_bytes   BINARY          -- original binary feed message (for replay)
)
STORED AS PARQUET
COMPRESSION ZSTD;
```

### OHLCV Candles (ClickHouse)
```sql
CREATE TABLE ohlcv_candles (
    symbol      VARCHAR(20),
    exchange    VARCHAR(10),    -- 'CONSOLIDATED' for cross-exchange candles
    interval    VARCHAR(5),     -- 1m | 5m | 15m | 1h | 1d
    open_ts     DateTime64(9),  -- candle open timestamp (nanosecond)
    open        Float64,
    high        Float64,
    low         Float64,
    close       Float64,
    volume      Int64,
    trade_count Int32,
    vwap        Float64         -- volume-weighted average price
) ENGINE = ReplacingMergeTree(open_ts)
ORDER BY (symbol, interval, open_ts)
PARTITION BY toYYYYMM(open_ts);
-- ReplacingMergeTree: handles in-flight candle updates (same candle sent multiple times as it builds)
```

---

## 5. API Design

### WebSocket API (Subscribers)
```
Connect: wss://data.platform.com/v2/stream?token=<api_key>

Subscribe request:
  { "action": "subscribe", "id": "req-001",
    "channels": ["nbbo", "trades", "level2"],
    "symbols": ["AAPL", "MSFT", "GOOGL"],
    "exchanges": ["all"] | ["NYSE", "NASDAQ"]  // optional filter by exchange
  }

Subscribe response:
  { "id": "req-001", "status": "subscribed", 
    "snapshot": {                              // current state snapshot on subscribe
      "AAPL": { "nbbo_bid": 179.95, "nbbo_ask": 180.00, "last_trade": 179.97, "ts_ns": ... },
      "MSFT": { ... }
    }
  }

NBBO event stream:
  { "type": "nbbo", "symbol": "AAPL",
    "bid": "179.95", "bid_size": 800, "bid_exchange": "NASDAQ",
    "ask": "180.00", "ask_size": 300, "ask_exchange": "NYSE",
    "seq": 9847231, "exchange_ts_ns": 1705320600123456789 }

Trade event:
  { "type": "trade", "symbol": "AAPL", "exchange": "NASDAQ",
    "price": "179.97", "qty": 100, "conditions": ["@", "T"],
    "aggressor_side": "BUY", "seq": 9847232, "exchange_ts_ns": ... }

Level 2 event:
  { "type": "level2", "symbol": "AAPL", "exchange": "NASDAQ", "side": "bid",
    "price": "179.95", "qty": 500, "action": "ADD",
    "seq": 9847231, "exchange_ts_ns": ... }

Heartbeat (every 30s):
  { "type": "heartbeat", "ts": "...", "seq": 9847300 }
```

### REST API (Historical Data)
```
GET /v1/ticks?symbol=AAPL&from=2024-01-15T09:30:00Z&to=2024-01-15T10:00:00Z&exchange=NASDAQ
  Response: paginated JSON (default) or Parquet binary (Accept: application/x-parquet)
  Max range: 1 trading day per request

GET /v1/candles?symbol=AAPL&interval=1m&from=2024-01-01&to=2024-01-15
  Response:
  {
    "symbol": "AAPL",
    "interval": "1m",
    "candles": [
      { "ts": "2024-01-15T09:30:00Z", "open": 180.50, "high": 181.20, "low": 179.90, 
        "close": 180.75, "volume": 1250000, "vwap": 180.60 }
    ]
  }

GET /v1/snapshot/{symbol}
  Returns current NBBO, last trade, day stats (open, high, low, volume)

GET /v1/reference/symbols?as_of=2024-01-15
  Returns symbol reference data (CUSIP, lot size, exchange, etc.)
```

---

## 6. Deep Dive: Core Components

### Component: Sequence Number Gap Detection & Recovery

**Problem it solves:** Exchange UDP feeds use sequence numbers. A dropped UDP packet = a sequence gap = missing market data. Missing a quote update means stale NBBO, which could lead to incorrect risk decisions or stale prices shown to users. The system must detect gaps within milliseconds and recover.

**Approach:**

```
Per-feed gap detector (per exchange, per feed line):
    expected_seq = last_received_seq + 1
    
    On receive packet with seq = S:
        if S == expected_seq:
            process normally
            expected_seq++
        
        elif S > expected_seq:
            // Gap detected: seq range [expected_seq, S-1] is missing
            gap_start = expected_seq
            gap_end = S - 1
            
            1. Buffer the out-of-order packet (seq=S) in a reorder buffer
            2. Trigger gap recovery:
               a. Request retransmission from exchange's replay feed 
                  (NASDAQ has MOLDUDP64 retransmit; NYSE has Integrated Feed replay)
               b. Wait up to 50ms for retransmission
               c. If retransmit arrives: fill gap, process in order, continue
               d. If no retransmit in 50ms: switch to TCP backup feed (exchange provides both)
                  TCP feed is slower but reliable; replay missed messages from checkpoint
               e. If gap unrecoverable: log GAP_EVENT, publish "data gap" alert to subscribers,
                  mark affected symbols as "quote_gap" status, continue with new packets

        elif S < expected_seq:
            // Duplicate packet (can happen on failover to backup feed)
            discard (already processed)

Gap recovery time target: < 100ms (50ms retransmit wait + 50ms processing)
Alert if gap > 500ms (circuit breaker: flag symbol's NBBO as potentially stale)
```

**Interviewer Deep-Dive Q&A:**

Q: Why not just use TCP for all exchange feeds? It has built-in reliability.
A: Reliability vs latency tradeoff. TCP's retransmit mechanism can add 1–100ms latency on packet loss (depends on RTT and TCP timer settings). For co-located HFT clients, 1ms of extra latency from a TCP retransmit during volatile market conditions is unacceptable. UDP multicast also allows a single network packet to reach thousands of subscribers simultaneously (one packet from exchange → fan-out to all subscribers on the multicast group), whereas TCP requires N individual streams. The solution is UDP primary feed + TCP secondary (backup) feed: UDP gives speed, TCP gives reliability when UDP fails. All major exchanges (NASDAQ, NYSE, CBOE) provide both.

---

### Component: NBBO Computation with Clock Skew

**Problem it solves:** Each exchange has its own clock. "Best bid" requires comparing bids from different exchanges using their timestamps. If NYSE's clock is 500μs ahead of NASDAQ's, an NBBO computed using ingestion time (when our system received the message) could be incorrect — we might use a "newer" NYSE quote that is actually temporally older.

**Approach:**

```
Correct approach: use exchange_ts_ns for ordering, not ingestion_ts_ns

Per-symbol NBBO state holds:
  exchange_bids: {
    NYSE:   { price: 179.90, size: 200, exchange_ts_ns: T_NYSE },
    NASDAQ: { price: 179.95, size: 500, exchange_ts_ns: T_NASDAQ },
    CBOE:   { price: 179.92, size: 300, exchange_ts_ns: T_CBOE }
  }

NBBO bid = max of all exchange bids = 179.95 (NASDAQ)

Problem with clock skew:
  If NASDAQ clock is 1ms behind NYSE clock:
    NYSE sends update at real_time T=100ms: bid=$179.96 (exchange_ts=T-1ms=99ms)
    NASDAQ sends update at real_time T=100.5ms: bid=$179.95 (exchange_ts=T+0ms=100.5ms)
    Our system receives NASDAQ first (faster feed), then NYSE
    Correct: at T=100ms, NYSE best bid is $179.96
    If we use ingestion time: we'd compute NBBO=$179.95 (NASDAQ, arrived first)
    This is wrong — we'd be showing a stale NBBO that doesn't reflect the latest NYSE update

Solution:
    Use a "late arrival" window of 5ms: hold NBBO computation for 5ms to allow
    slightly late packets to arrive and be ordered correctly by exchange_ts_ns.
    This adds 5ms to NBBO latency but ensures correctness.
    
    For HFT clients (latency-sensitive): publish "raw" NBBO without late-arrival window;
    document that it may not reflect cross-exchange ordering precisely.
    
    For retail/analytics: publish "consolidated" NBBO with 5ms window.

Exchange clock sync:
    All exchanges sync to GPS/PTP (IEEE 1588) with < 100ns accuracy.
    Our feed handlers also sync to PTP to compare exchange_ts to our clock.
    Clock skew monitoring: alert if any exchange clock drifts > 1ms from our reference.
```

---

### Component: Candle (OHLCV) Computation

**Problem it solves:** Subscribers need historical and real-time OHLCV candlestick data for charting. Computing candles requires: (1) per-symbol aggregation over time windows, (2) handling late-arriving ticks, (3) efficiently updating in-progress candles without full recompute.

**Implementation:**

```
Candle Engine (in-memory per symbol per interval):

struct LiveCandle:
    symbol:       string
    interval:     string       // 1m | 5m | 15m | 1h | 1d
    open_ts:      long         // start of candle window
    close_ts:     long         // end of candle window
    open:         double       // first trade price in window
    high:         double       // running max
    low:          double       // running min
    close:        double       // most recent trade price
    volume:       long         // sum of trade quantities
    trade_count:  int
    vwap_numer:   double       // sum of (price × qty) for VWAP = numer/volume
    
    is_finalized: bool         // true once close_ts passed + late arrival window

On each TRADE event for a symbol:
    for each interval:
        candle = getLiveCandle(symbol, interval, trade.exchange_ts_ns)
        if candle.open == 0: candle.open = trade.price    // first trade sets open
        candle.high = max(candle.high, trade.price)
        candle.low = min(candle.low, trade.price)
        candle.close = trade.price                        // last trade sets close
        candle.volume += trade.qty
        candle.trade_count += 1
        candle.vwap_numer += trade.price × trade.qty
        
        // Publish live candle update every second (for real-time charts)
        publishCandleUpdate(candle)

On candle window close (e.g., 9:31:00 → 1-minute candle for 9:30:00 closes):
    // Apply late arrival window: wait 2 seconds for late ticks
    sleep(2s)  // non-blocking; implemented as scheduled task
    finalize and store to ClickHouse
    publishFinalCandle(candle)

Late tick handling:
    If a tick arrives after candle is finalized (exchange_ts_ns inside closed window):
    → Update ClickHouse: ReplacingMergeTree engine handles update via deduplication
    → Publish "candle revision" event to subscribers (rare, typically < 0.1% of candles)
```

---

## 7. Failure Scenarios & Resilience

### Scenario 1: Exchange Feed Outage
- **Detection**: Sequence number heartbeat timeout (most exchanges send empty heartbeat messages to keep sequence counter moving). Heartbeat timeout = 1 second.
- **Recovery**: Switch to backup feed (every exchange provides 2+ feed lines). If both feeds down: switch to SIP (Securities Information Processor — the consolidated tape that all US exchanges report to; slightly slower but covers all venues). Publish "exchange_feed_degraded" event to subscribers.

### Scenario 2: NBBO Engine Crash
- **Impact**: All NBBO computation stops; subscribers receive stale prices.
- **Recovery**: Hot standby NBBO Engine takes over within < 1s. Standby processes all events but suppresses publication. On promotion: begins publishing. Per-symbol sequence numbers allow subscribers to detect the gap and request snapshot resync.
- **Client behavior**: WebSocket clients receive "resync required" event; re-request snapshot; gaps in sequence numbers detected client-side are handled by fetching candle/tick history.

### Scenario 3: Kafka Producer Backpressure
- **Impact**: Tick archive and downstream consumers receive delayed data.
- **Recovery**: Kafka producer buffers in memory (configurable to 256 MB). If buffer full: shed load by dropping non-critical downstream messages (candle updates) while preserving raw tick stream. Market data WebSocket push uses Redis Pub/Sub directly (bypasses Kafka), so subscriber delivery is unaffected. Kafka lag is monitored; auto-scaling of Kafka brokers triggered if lag > 1M messages.

### Scenario 4: Market Halt for a Symbol
- **Exchange feed message**: Exchange sends `Trading Halt` message in ITCH/Pillar format.
- **Handling**: NBBO Engine marks symbol as HALTED in NBBOState. WebSocket subscribers receive `{ "type": "halt", "symbol": "AAPL", "reason": "circuit_breaker", "ts": "..." }`. No NBBO updates published while halted. On resume: exchange sends `Trading Resume` → NBBO Engine resumes normal processing → subscribers receive `{ "type": "resume" }` + fresh snapshot.

---

## 8. Monitoring & Observability

| Metric | Alert Threshold | Purpose |
|---|---|---|
| Feed sequence gap count | Any gap > 5ms unrecovered | Feed reliability |
| NBBO computation latency P99 | > 2 ms | Engine performance |
| WebSocket push latency P99 | > 20 ms | Subscriber delivery |
| Kafka consumer lag (tick archive) | > 1M messages | Archive pipeline health |
| Exchange clock skew | > 1 ms from reference | Data quality |
| Candle revision rate | > 0.5% of closed candles | Late arrival or clock issue |
| Feed handler CPU | > 80% per core | Scaling signal |
| Multicast packet loss rate | > 0.001% | Network quality |
| Active WebSocket connections | Expected range ± 20% | Capacity planning |
| ClickHouse ingest rate | < 90% of expected | Storage pipeline health |
