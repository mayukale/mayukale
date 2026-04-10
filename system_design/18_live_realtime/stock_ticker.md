# System Design: Real-Time Stock Ticker & Market Data Platform

---

## 1. Requirement Clarifications

### Functional Requirements
1. Ingest real-time market data feeds from multiple exchanges (NYSE, NASDAQ, CBOE, etc.) via FIX protocol or proprietary binary feed (e.g., NASDAQ ITCH, NYSE OpenBook).
2. Normalize price data across exchanges into a unified format (symbol, bid, ask, last trade, volume).
3. Maintain a live order book (Level 2 data) per security showing bid/ask depth up to 10 levels.
4. Compute National Best Bid and Offer (NBBO) in real time across all contributing exchanges.
5. Push real-time price updates to subscribers via WebSocket.
6. Support subscriptions at three granularities: (a) individual symbol, (b) watchlist of up to 500 symbols, (c) sector/index subscription (e.g., all S&P 500 components).
7. Provide historical OHLCV (Open, High, Low, Close, Volume) candlestick data at 1-minute, 5-minute, 1-hour, 1-day intervals.
8. REST API for snapshot queries (last price, order book depth, daily statistics).
9. Support approximately 10,000 unique instruments (equities, ETFs, options chain tickers).
10. Maintain a 5-year archive of all tick data queryable by symbol and time range.

### Non-Functional Requirements
1. Latency: price updates must reach client WebSocket within 50 ms P99 (end-to-end, exchange feed to client).
2. Throughput: handle peak feed ingestion of 5 M messages/second during market open/close (first/last 30 min of trading day).
3. Availability: 99.99% uptime during market hours (9:30 AM–4:00 PM ET, Mon–Fri); planned maintenance only off-hours.
4. Consistency: NBBO must reflect latest prices from all exchanges; eventual consistency is acceptable (NBBO can lag real exchange NBBO by < 5 ms).
5. Ordering: per-symbol price updates delivered to clients in strict exchange-timestamp order.
6. Historical data: full tick data archived; query latency < 2 s for any 1-day range; < 10 s for 1-year range.
7. Scalability: support 500 k concurrent WebSocket subscribers with up to 500-symbol watchlists each.
8. Security: client authentication via API key; data licensing controls (Bloomberg/Refinitiv data cannot be re-distributed without license).
9. Compliance: audit log of all data feeds received; immutable for 7 years (SEC Rule 17a-4).

### Out of Scope
- Order execution / brokerage functionality
- Options pricing model (Black-Scholes calculations)
- Fundamental data (P/E ratios, earnings, financial statements)
- News feeds / sentiment analysis
- Portfolio tracking and P&L calculations
- Regulatory reporting (CAT, OATS)

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Retail Investor | WebSocket subscriber for watchlist of 10–50 symbols; uses web/mobile app |
| Power User / Trader | WebSocket subscriber with 500-symbol watchlist; low latency critical |
| Institutional Client | Direct feed consumer (FIX/binary); co-location at data center; latency-critical |
| Algorithm / Bot | Programmatic WebSocket or FIX consumer; may subscribe to full symbol universe |
| Analytics User | Historical data queries via REST API; batch job consumers |

### Traffic Estimates

**Assumptions:**
- 10,000 unique instruments actively traded
- Peak trading hours (market open/close): 5 M feed messages/s
- Normal trading hours: 500 k feed messages/s
- Average message size (normalized tick): 100 bytes
- Concurrent WebSocket subscribers at peak: 500 k
- Average symbols per subscriber: 100 (mix of small watchlists and large ones)
- Fan-out factor: each symbol update triggers delivery to average 200 subscribers
- Market open: 6.5 hours × 5 days = 32.5 hours/week of peak data

| Metric | Calculation | Result |
|---|---|---|
| Peak feed ingest rate | 5 M messages/s × 100 bytes | 500 MB/s (4 Gbps) ingest |
| Normal feed ingest rate | 500 k messages/s × 100 bytes | 50 MB/s (400 Mbps) |
| WebSocket subscribers | given | 500 k concurrent |
| Subscription pairs | 500 k subscribers × 100 symbols avg | 50 M symbol-subscriber pairs |
| WebSocket push rate (peak) | 5 M updates/s × 200 avg subscribers/symbol (for traded symbols) | Up to 1 B deliveries/s (requires fan-out optimization) |
| Practical WebSocket push rate | Only 1 k symbols trade at once during peak; 1 k × 5 updates/s × 200 subscribers | 1 M deliveries/s |
| Order book updates/s | 1 k active symbols × 50 order book changes/s per symbol | 50 k order book updates/s |
| REST API snapshot requests | 500 k users × 10 req/min / 60 | ~83 k req/s |
| Tick data storage/day | 5 M msg/s × 6.5 h × 3600 s × 100 bytes | ~11.7 TB/day raw ticks |
| Tick data storage/year | 11.7 TB × 252 trading days | ~2.95 PB/year |
| 5-year archive | 2.95 PB × 5 | ~14.7 PB |

### Latency Requirements
| Operation | Target (P50) | Target (P99) | Notes |
|---|---|---|---|
| Feed ingest to internal normalized event | < 1 ms | < 5 ms | Within data center |
| Normalized event to WebSocket push | < 5 ms | < 20 ms | Including pub/sub routing |
| End-to-end exchange-to-client | < 15 ms | < 50 ms | Network dependent |
| NBBO computation latency | < 1 ms | < 5 ms | Per symbol, triggered per update |
| Order book update latency | < 2 ms | < 10 ms | Level 2 data |
| REST snapshot API | < 10 ms | < 50 ms | Served from cache |
| Historical 1-day query | < 500 ms | < 2 s | |
| Historical 1-year query | < 2 s | < 10 s | May require columnar DB |

### Storage Estimates
| Data | Size | Retention | Total |
|---|---|---|---|
| Raw tick data (compressed Parquet) | 11.7 TB/day × 0.15 compression ratio | 5 years | ~2.2 PB |
| OHLCV candles (1m, 5m, 1h, 1d) | 10 k symbols × 4 intervals × 252 days × ~50 bytes/candle × 5 years | Forever | ~6.3 GB (trivial) |
| Order book snapshots (every 1s, top 10 levels) | 10 k symbols × 1/s × 6.5h × 3600 × 20 levels × 8 bytes × 252 days | 30 days | ~270 TB/year; keep 30 days: ~22 TB |
| NBBO history (1 record/update) | Same as tick data | 5 years | Included in tick archive |
| Audit log (immutable) | Same as raw feed | 7 years | ~3.1 PB |

### Bandwidth Estimates
| Flow | Calculation | Result |
|---|---|---|
| Exchange feeds → Ingest | 4 Gbps peak | 4 Gbps |
| Ingest → Message bus | 4 Gbps × 1.2 overhead | ~5 Gbps |
| Message bus → WebSocket servers | 1 M deliveries/s × 100 bytes × 8 bits | 800 Mbps |
| WebSocket servers → Clients | 500 k clients × avg 10 updates/s × 100 bytes × 8 bits | 4 Gbps |
| Tick archive writes (S3) | 11.7 TB/day / 86400 s | ~1.1 Gbps |

---

## 3. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                         EXCHANGE FEEDS                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  NYSE ARCA   │  │    NASDAQ    │  │     CBOE     │  │   IEX / SIP  │       │
│  │  (ARCA XDP) │  │  (ITCH 5.0)  │  │  (PITCH 2.x) │  │  (DEEP/TOPS) │       │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘       │
└─────────┼─────────────────┼─────────────────┼─────────────────┼────────────────┘
          │ UDP multicast   │ TCP/UDP          │ TCP             │ WebSocket/TCP
          ▼                 ▼                 ▼                 ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                    FEED HANDLER LAYER (Co-located)                              │
│  ┌──────────────────────────────────────────────────────────────────────────┐  │
│  │  Feed Handler Processes (one per exchange, kernel-bypass, DPDK/RDMA)    │  │
│  │  - Parse binary protocol (ITCH/XDP/PITCH) using zero-copy parsing       │  │
│  │  - Sequence gap detection and retransmission request                    │  │
│  │  - Timestamp with hardware clock (PTP-synchronized, ns precision)       │  │
│  │  - Normalize to internal Tick struct: {symbol, price, size, side,       │  │
│  │    exchange, seq_num, exchange_ts, ingest_ts}                           │  │
│  └─────────────────────────────┬────────────────────────────────────────────┘  │
└────────────────────────────────┼────────────────────────────────────────────────┘
                                 │  Internal binary (Aeron / Chronicle Queue)
                                 ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                  PROCESSING LAYER                                               │
│                                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  Market Data Processor (per-symbol partition)                           │   │
│  │  - Order book management (price-level aggregation)                      │   │
│  │  - NBBO computation                                                     │   │
│  │  - Trade aggregation → OHLCV candle building                           │   │
│  │  - Anomaly detection (price spike filter, circuit breaker awareness)    │   │
│  └────────────────────────────┬────────────────────────────────────────────┘   │
│                               │                                                 │
│  ┌────────────────────────────▼────────────────────────────────────────────┐   │
│  │  State Store (in-process, off-heap)                                     │   │
│  │  - Current order book per symbol (in-memory HashMap<symbol, OrderBook>) │   │
│  │  - Last NBBO per symbol                                                 │   │
│  │  - Current candle accumulators per interval per symbol                 │   │
│  └────────────────────────────┬────────────────────────────────────────────┘   │
└────────────────────────────────┼────────────────────────────────────────────────┘
                                 │  Kafka (partitioned by symbol)
                ┌────────────────┼────────────────────────────────┐
                │                │                                │
                ▼                ▼                                ▼
┌───────────────────┐  ┌──────────────────────┐  ┌─────────────────────────────┐
│  TICK ARCHIVE     │  │  WEBSOCKET PUSH       │  │  CANDLE/OHLCV SERVICE       │
│                   │  │  ENGINE               │  │                             │
│  Kafka Consumer   │  │  ┌────────────────┐  │  │  Kafka Consumer             │
│  → Parquet files  │  │  │  Sub Manager   │  │  │  Accumulates into Redis     │
│  → S3 (Iceberg)   │  │  │  symbol→[conn] │  │  │  sorted sets per interval   │
│  → Columnar DB    │  │  └───────┬────────┘  │  │  Writes completed candles   │
│  (for queries)    │  │          │ fan-out    │  │  → TimescaleDB              │
│                   │  │  ┌───────▼────────┐  │  │                             │
│                   │  │  │  WS Gateway    │  │  └─────────────────────────────┘
│                   │  │  │  Pods (1k/pod) │  │
│                   │  │  └───────┬────────┘  │  ┌─────────────────────────────┐
│                   │  │          │            │  │  REST SNAPSHOT API          │
│                   │  └──────────┼────────────┘  │                             │
└───────────────────┘             │               │  Reads from Redis cache     │
                                  ▼               │  (last price, order book)   │
                               CLIENTS            │  Falls back to Processing   │
                            (Web / Mobile /       │  Layer for fresh data       │
                             Algorithms)          └─────────────────────────────┘
```

**Component Roles:**
- **Feed Handler**: Low-level binary protocol parser running with kernel bypass (DPDK) for microsecond-level processing. One process per exchange. Handles gap detection (sequence numbers), heartbeats, and retransmission.
- **Market Data Processor**: Core logic — maintains order book state, computes NBBO, builds candles. Partitioned by symbol (consistent hashing) to ensure single-writer semantics per symbol.
- **Kafka**: Durable event bus. Tick events partitioned by symbol hash ensure per-symbol ordering. Enables fan-out to multiple consumers (archive, WebSocket, candle builder) without coupling.
- **WebSocket Push Engine**: Subscription manager (symbol → list of WebSocket connections) + gateway pods. Receives updates from Kafka and fans out to subscribed clients.
- **Tick Archive**: Parquet files on S3 organized as Apache Iceberg tables for time-travel and efficient range scans.
- **REST Snapshot API**: Stateless; reads from Redis for current prices (< 1 ms); falls back to Processor state for order book depth.

---

## 4. Data Model

### Entities & Schema

```sql
-- Instruments reference data (slowly changing)
CREATE TABLE instruments (
    symbol          VARCHAR(20) PRIMARY KEY,
    name            VARCHAR(200) NOT NULL,
    exchange        VARCHAR(10) NOT NULL,       -- primary listing exchange
    asset_type      VARCHAR(20) NOT NULL,       -- 'equity', 'etf', 'index'
    currency        CHAR(3) NOT NULL DEFAULT 'USD',
    lot_size        INTEGER NOT NULL DEFAULT 100,
    tick_size       DECIMAL(12, 6) NOT NULL,    -- minimum price increment
    is_active       BOOLEAN DEFAULT TRUE,
    listed_date     DATE,
    delisted_date   DATE,
    isin            CHAR(12) UNIQUE,
    cusip           CHAR(9),
    sector          VARCHAR(50),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Current market snapshot (kept in Redis; SQL schema for documentation)
-- Redis hash: market:snapshot:{symbol}
-- Fields: last_price, bid, ask, bid_size, ask_size, volume, open, high, low,
--         prev_close, change, change_pct, last_trade_ts, exchange_ts, seq_num
-- TTL: none (updated continuously during market hours; stale detection via last_trade_ts)

-- OHLCV candles (TimescaleDB hypertable)
CREATE TABLE ohlcv_candles (
    symbol          VARCHAR(20) NOT NULL,
    interval        VARCHAR(5) NOT NULL,        -- '1m', '5m', '1h', '1d'
    open_ts         TIMESTAMPTZ NOT NULL,
    close_ts        TIMESTAMPTZ NOT NULL,
    open_price      DECIMAL(18, 6) NOT NULL,
    high_price      DECIMAL(18, 6) NOT NULL,
    low_price       DECIMAL(18, 6) NOT NULL,
    close_price     DECIMAL(18, 6) NOT NULL,
    volume          BIGINT NOT NULL,
    trade_count     INTEGER,
    vwap            DECIMAL(18, 6),
    PRIMARY KEY (symbol, interval, open_ts)
);

-- Convert to TimescaleDB hypertable, partitioned by open_ts
SELECT create_hypertable('ohlcv_candles', 'open_ts',
    partitioning_column => 'symbol',
    number_partitions => 8,
    chunk_time_interval => INTERVAL '1 day');

-- Retention policy: compress 1m data after 7 days, drop after 2 years
SELECT add_compression_policy('ohlcv_candles', INTERVAL '7 days');
SELECT add_retention_policy('ohlcv_candles', INTERVAL '2 years');

-- Order book snapshot (for historical Level 2 queries; Redis for live)
CREATE TABLE order_book_snapshots (
    symbol          VARCHAR(20) NOT NULL,
    snapshot_ts     TIMESTAMPTZ NOT NULL,
    exchange        VARCHAR(10) NOT NULL,
    side            CHAR(1) NOT NULL,           -- 'B' (bid) or 'A' (ask)
    level           SMALLINT NOT NULL,          -- 1 = best, 10 = 10th best
    price           DECIMAL(18, 6) NOT NULL,
    size            INTEGER NOT NULL,
    order_count     INTEGER,
    PRIMARY KEY (symbol, snapshot_ts, exchange, side, level)
);
-- TimescaleDB hypertable on snapshot_ts, retained 30 days

-- Subscriptions (client subscription management; in Redis for speed, persisted here)
CREATE TABLE client_subscriptions (
    client_id       UUID NOT NULL,
    symbol          VARCHAR(20) NOT NULL,
    subscribed_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    subscription_type VARCHAR(20) DEFAULT 'quote',  -- 'quote', 'trade', 'orderbook'
    PRIMARY KEY (client_id, symbol)
);
CREATE INDEX idx_subs_symbol ON client_subscriptions(symbol);

-- API keys (authentication)
CREATE TABLE api_keys (
    key_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL,
    key_hash        TEXT NOT NULL UNIQUE,       -- SHA-256 of raw API key
    name            VARCHAR(100),
    permissions     JSONB NOT NULL DEFAULT '{"realtime": true, "history": true}',
    data_entitlements JSONB NOT NULL DEFAULT '{"delayed": true, "realtime": false}',
    rate_limit_rps  INTEGER DEFAULT 100,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_used_at    TIMESTAMPTZ,
    expires_at      TIMESTAMPTZ,
    is_active       BOOLEAN DEFAULT TRUE
);

-- Audit log (immutable; append-only; stored in S3/WORM for compliance)
-- Kafka topic: feed-audit (compaction disabled, retention = forever)
-- Schema (Avro):
/*
{
  "type": "record",
  "name": "FeedAuditEvent",
  "fields": [
    {"name": "event_id", "type": "string"},
    {"name": "exchange", "type": "string"},
    {"name": "symbol", "type": "string"},
    {"name": "seq_num", "type": "long"},
    {"name": "exchange_ts", "type": "long"},       // epoch microseconds
    {"name": "ingest_ts", "type": "long"},         // epoch microseconds
    {"name": "message_type", "type": "string"},    // ADD/MODIFY/DELETE/TRADE
    {"name": "raw_bytes", "type": "bytes"},        // original binary message
    {"name": "normalized", "type": "string"}       // JSON of normalized tick
  ]
}
*/
```

**Redis Schema (live state):**
```
# Current best quote per symbol
HSET market:quote:AAPL
    bid       "150.25"
    ask       "150.26"
    bid_size  "500"
    ask_size  "300"
    last      "150.255"
    volume    "45321000"
    ts        "1712345678123456"   # microseconds epoch
    seq       "10293847"

# Order book (sorted set, score = price, negative for bids)
# Bids: ZADD market:book:AAPL:bid -150.25 "500|3|NYSE"  (score=-price for reverse sort)
# Asks: ZADD market:book:AAPL:ask  150.26 "300|2|NASDAQ"
# Format: "size|order_count|exchange"

# Subscription index (who is watching which symbol)
# Stored in Subscription Manager process memory (not Redis) for lowest latency
# Redis used only for cross-pod subscription routing (which pod holds a connection)
HSET ws:conn:{connection_id}  pod_id "ws-pod-7"  user_id "abc123"
SADD ws:symbol:AAPL:pods  "ws-pod-7" "ws-pod-12" "ws-pod-3"
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| Redis (in-memory) | Live quotes, order book, hot cache | Sub-millisecond read/write, sorted sets perfect for order book, pub/sub built-in | In-memory cost; single-threaded command execution | **Selected** for live state |
| TimescaleDB | OHLCV candles, historical queries | SQL + time-series optimization, auto-partitioning, continuous aggregates, compression | Requires PostgreSQL tuning; not suited for tick-level data | **Selected** for candles |
| Apache Iceberg on S3 | Tick archive (PB-scale) | Columnar Parquet, predicate pushdown, time-travel queries, schema evolution, cost-effective at PB scale | Query latency (minutes for large scans without Trino/Spark) | **Selected** for tick archive |
| Kafka (as durable log) | Event streaming, audit trail | Ordered per partition, durable, replay capability, multi-consumer | Not a query engine; storage cost for long retention | **Selected** as message bus |
| PostgreSQL | Instruments reference, API keys, subscriptions | ACID, relational integrity | Not suited for time-series at this scale | **Selected** for operational data |
| InfluxDB | Time series metrics | Native time-series, flux query language | Weaker SQL support; OSS limits; licensing changes | Not selected |
| DynamoDB | Snapshot cache | Serverless | Higher latency than Redis; no sorted sets for order book | Not selected |
| Cassandra | Tick data | Write throughput | Read latency for range queries worse than Iceberg+Trino at PB scale | Not selected for tick archive |

---

## 5. API Design

### WebSocket API (Real-Time)

```
Connection: wss://stream.example.com/v1/ws
Auth: API key in header (X-API-Key: <key>) or query param ?apikey=<key>

# Client subscribe/unsubscribe
Client → Server:
{
  "action": "subscribe",
  "symbols": ["AAPL", "MSFT", "GOOG"],
  "channels": ["quote", "trade", "orderbook"]  // channels are additive
}
{
  "action": "unsubscribe",
  "symbols": ["GOOG"],
  "channels": ["orderbook"]
}
{
  "action": "ping"
}

# Server → Client messages
{
  "type": "quote",
  "symbol": "AAPL",
  "bid": 150.25,
  "ask": 150.26,
  "bid_size": 500,
  "ask_size": 300,
  "exchange_ts": 1712345678123456,   // microseconds since epoch
  "seq": 10293847
}
{
  "type": "trade",
  "symbol": "AAPL",
  "price": 150.255,
  "size": 100,
  "side": "B",                      // B=buyer-initiated, S=seller-initiated
  "exchange": "NASDAQ",
  "exchange_ts": 1712345678124000,
  "seq": 10293848
}
{
  "type": "orderbook",
  "symbol": "AAPL",
  "bids": [[150.25, 500], [150.24, 1200], ...],   // [price, size] top 10
  "asks": [[150.26, 300], [150.27, 800], ...],
  "exchange_ts": 1712345678125000
}
{
  "type": "status",
  "symbol": "AAPL",
  "market_status": "open",          // open|pre_market|post_market|halted|closed
  "halt_code": null
}
{
  "type": "error",
  "code": "NOT_ENTITLED",
  "message": "Real-time data requires a paid subscription"
}
{
  "type": "pong"
}

Rate limits:
  - Subscribe actions: 10/s per connection
  - Max symbols per connection: 500 (configurable by tier)
  - Max concurrent connections per API key: 10
  - Heartbeat: server sends ping every 30 s; client must pong within 10 s
```

### REST API

```
# Current quote snapshot
GET /v1/quote/{symbol}
  Auth: X-API-Key header
  Response: 200 {
    "symbol": "AAPL",
    "bid": 150.25, "ask": 150.26,
    "last": 150.255, "volume": 45321000,
    "open": 148.50, "high": 151.20, "low": 147.80,
    "prev_close": 149.00, "change": 1.255, "change_pct": 0.84,
    "market_status": "open",
    "exchange_ts": "2024-04-06T14:32:58.123456Z"
  }
  Cache: 1 s (CDN cacheable for delayed data tier only)
  Rate limit: 100 req/s per API key

# Batch quotes
GET /v1/quotes?symbols=AAPL,MSFT,GOOG,AMZN   (max 100 symbols)
  Auth: X-API-Key
  Response: 200 { "quotes": [ ...quote objects... ] }
  Rate limit: 10 req/s per API key

# Order book (Level 2)
GET /v1/orderbook/{symbol}?levels=10
  Auth: X-API-Key (orderbook entitlement required)
  Response: 200 {
    "symbol": "AAPL",
    "bids": [[150.25, 500, 3], [150.24, 1200, 8], ...],  // [price, size, orders]
    "asks": [[150.26, 300, 2], [150.27, 800, 5], ...],
    "exchange_ts": "2024-04-06T14:32:58.125Z"
  }
  Rate limit: 50 req/s per API key

# Historical OHLCV candles
GET /v1/candles/{symbol}?interval=1m&start=2024-04-01T09:30:00Z&end=2024-04-01T16:00:00Z
  Auth: X-API-Key (history entitlement required)
  Response: 200 {
    "symbol": "AAPL",
    "interval": "1m",
    "candles": [
      { "ts": "2024-04-01T09:30:00Z", "o": 148.50, "h": 149.20, "l": 148.30, "c": 149.00, "v": 3200000 },
      ...
    ]
  }
  Rate limit: 10 req/s per API key; max 1-year range per request

# Tick data (historical raw trades)
GET /v1/ticks/{symbol}?start=2024-04-01T09:30:00Z&end=2024-04-01T09:31:00Z
  Auth: X-API-Key (tick entitlement required — premium tier)
  Response: 200 {
    "symbol": "AAPL",
    "ticks": [
      { "ts": "2024-04-01T09:30:00.123456Z", "price": 148.50, "size": 100, "exchange": "NASDAQ", "type": "trade" },
      ...
    ],
    "total": 842
  }
  Rate limit: 5 req/s; max 1-minute range per request (tick data is dense)
```

---

## 6. Deep Dive: Core Components

### 6.1 Order Book Management

**Problem it solves:**
An order book tracks all outstanding limit orders for a security at each price level, organized by price (bids descending, asks ascending). Exchanges send ADD, MODIFY, CANCEL, and TRADE messages at rates up to 500 k messages/second per symbol during peak. The system must process each message in < 1 ms and maintain a consistent view of the top-10 price levels across all exchanges.

**Approaches Comparison:**

| Approach | Throughput | Latency | Memory | Complexity |
|---|---|---|---|---|
| Sorted map (TreeMap/SortedDict) | Good (O(log N) per op) | ~5 µs | O(N price levels) | Low |
| Skip list | Good (O(log N)) | ~3 µs | Higher than tree | Medium |
| Price-level array (price → bucket) | Excellent (O(1) per op) | < 1 µs | Fixed-size, wasteful for sparse prices | Low |
| Doubly linked list by price + hashmap | Excellent (O(1) insert/delete) | < 1 µs | O(N) | Medium |
| **Combination: HashMap<price, Level> + TreeSet<price> for iteration** | Excellent | ~1 µs | O(N) | Low-medium |

**Selected: HashMap<price_as_integer, Level> + NavigableSet<price> (Java TreeSet / C++ std::map).**

Price is stored as a fixed-point integer (price × 10,000 for 4 decimal places) to avoid floating-point rounding. A HashMap gives O(1) lookup for ADD/MODIFY/CANCEL by price. The TreeSet maintains insertion-ordered price keys for O(log N) best-price queries and top-N iterations.

**Pseudocode:**

```
struct PriceLevel:
    price_int:  int64     # price × 10_000 (fixed-point)
    total_size: int64     # total shares at this price level
    order_count: int32    # number of orders at this level
    orders:     map[order_id] -> Order  # individual orders (for MODIFY/CANCEL)

struct OrderBook:
    bids: TreeMap[price_int DESC -> PriceLevel]   # highest bid first
    asks: TreeMap[price_int ASC  -> PriceLevel]   # lowest ask first
    symbol: str
    exchange: str
    last_seq: int64

function apply_message(book: OrderBook, msg: FeedMessage):
    # Validate sequence number for gap detection
    if msg.seq_num != book.last_seq + 1:
        request_retransmit(book.exchange, book.last_seq + 1, msg.seq_num - 1)
        # queue msg; process after gap fills
        return

    match msg.type:
        case ADD_ORDER:
            level = book.bids.get(msg.price_int) if msg.side == 'B' \
                    else book.asks.get(msg.price_int)
            if level is None:
                level = PriceLevel(price_int=msg.price_int)
                (book.bids if msg.side=='B' else book.asks).put(msg.price_int, level)
            level.total_size += msg.size
            level.order_count += 1
            level.orders[msg.order_id] = Order(msg.order_id, msg.price, msg.size)

        case MODIFY_ORDER:
            order = find_order(book, msg.order_id)  # O(1) with order_id index
            old_level = (book.bids if order.side=='B' else book.asks).get(order.price_int)
            delta = msg.new_size - order.size
            old_level.total_size += delta
            order.size = msg.new_size
            if old_level.total_size == 0:
                (book.bids if order.side=='B' else book.asks).remove(order.price_int)

        case CANCEL_ORDER:
            order = find_order(book, msg.order_id)
            level = (book.bids if order.side=='B' else book.asks).get(order.price_int)
            level.total_size -= order.size
            level.order_count -= 1
            del level.orders[msg.order_id]
            if level.total_size == 0:
                (book.bids if order.side=='B' else book.asks).remove(order.price_int)

        case TRADE:
            # Trades reduce size from top-of-book level
            top_level = book.bids.first() if msg.side == 'S' else book.asks.first()
            top_level.total_size -= msg.size
            if top_level.total_size <= 0:
                (book.bids if msg.side=='S' else book.asks).remove_first()

    book.last_seq = msg.seq_num
    emit_book_update(book)  # publish top-10 levels to Kafka

function get_top_n(book: OrderBook, n: int) -> (bids: List[Level], asks: List[Level]):
    bids = list(itertools.islice(book.bids.values(), n))  # first n from descending sorted map
    asks = list(itertools.islice(book.asks.values(), n))
    return bids, asks
```

**Interviewer Q&A:**

Q1: How do you handle sequence gaps in the feed (missed messages)?
A1: Each exchange uses monotonically increasing sequence numbers. The feed handler tracks `last_seq` per exchange-symbol. When a gap is detected (msg.seq_num > last_seq + 1), we: (1) Buffer incoming messages; (2) Send a retransmission request (TCP-based for NASDAQ; for UDP multicast feeds use the exchange's "gap fill" TCP channel); (3) Apply buffered messages in order once the gap is filled; (4) If retransmission takes > 200 ms, we fall back to a market snapshot from the exchange's snapshot endpoint, which gives us the full order book at a point-in-time, then replay the buffered messages on top. Gaps corrupt the order book state, so processing is always halted during gap resolution — a stale book is better than a wrong book.

Q2: How much memory does an order book consume?
A2: For a typical equity at market open: 10,000 active orders × ~200 bytes/order (price, size, ID, side, level pointer) = 2 MB per symbol's individual order map. The price-level map for top 10 levels = ~10 × 50 bytes = 500 bytes. For 10,000 symbols: 10,000 × 2 MB = 20 GB total for individual order tracking. In practice, individual order tracking is only needed for exchanges that send ADD/CANCEL/MODIFY by order_id (like NASDAQ ITCH). Exchanges that only send price-level updates (like NYSE OpenBook) require only the price-level map, reducing memory to ~5 MB total.

Q3: How do you compute NBBO from multiple exchange order books?
A3: NBBO (National Best Bid and Offer) is max(all exchange best bids) for bid and min(all exchange best asks) for ask. Since we maintain one OrderBook per symbol per exchange, NBBO is computed after every order book update: iterate over all exchange books for that symbol (typically 5–8 exchanges), find the best bid and ask. This is O(E) where E = number of exchanges = O(1) effectively. We cache the last NBBO per symbol in a struct; when a new order book update arrives, we only need to re-check if the updated exchange's best price could change the NBBO (price >= current best bid or price <= current best ask). This reduces unnecessary NBBO recomputation.

Q4: The order book processing is single-threaded per symbol. Is that a bottleneck?
A4: Single-threaded per symbol is intentional — it eliminates locking overhead and ensures strict message ordering. A single modern CPU core can process 5–10 M order book operations/s. At 500 k messages/s peak across all symbols, and with 10,000 symbols, that's 50 messages/s per symbol average. Even bursty symbols during earnings announcements might see 10,000 messages/s — well within one core's capacity. The bottleneck is actually network I/O and cache misses, not compute. Using LMAX Disruptor (Java) or a lock-free ring buffer (C++) as the inter-thread queue eliminates scheduling overhead.

Q5: How would you handle a flash crash where prices move 10% in 1 second?
A5: (1) Anomaly detection: each price update is checked against a rolling 30-second VWAP; if the new price deviates by more than 3%, the update is flagged but still applied; (2) Circuit breaker awareness: exchanges publish Limit Up/Limit Down (LULD) bands; the processor checks incoming trades against LULD bands and rejects out-of-band prints; (3) If a security is halted (exchange sends an HALT message), the processor marks it halted and pushes a `status: halted` event to subscribers; (4) Downstream WebSocket subscribers receive the halt status and the UI can display a warning. The system does not reject valid exchange data — it processes it and flags it for downstream consumers to handle appropriately.

---

### 6.2 WebSocket Fan-out at 500 K Subscribers

**Problem it solves:**
At 500 k concurrent WebSocket connections each subscribing to ~100 symbols, and with 5 M market data updates/s during peak, the naive approach of iterating subscriber lists on every update is computationally infeasible. A single update to AAPL (subscribed by an estimated 100 k users) would require 100 k WebSocket writes per update × 50 updates/s × 1,000 active symbols = 5 B write operations/s. We need an architecture that delivers updates to the right subscribers in < 20 ms without iterating all subscriptions.

**Approaches Comparison:**

| Approach | Fan-out Throughput | Latency | Memory | Cross-pod |
|---|---|---|---|---|
| Single server loop | O(subscribers) | Grows with sub count | O(N) | N/A |
| Redis pub/sub (one channel per symbol) | ~1 M deliveries/s per Redis node | < 5 ms | Low | Yes |
| Kafka consumer per WS pod | High throughput, ordered | 10–100 ms | Low | Yes |
| In-process inverted index (symbol→connections) + smart batching | Highest, O(1) lookup | < 1 ms within pod | O(N) | Requires routing layer |
| Bloom filter routing | Near-O(1) | < 1 ms | Very low (false positives OK) | Yes |

**Selected: Per-pod inverted index with smart batching + inter-pod routing via Kafka.**

Each WebSocket gateway pod maintains an in-memory inverted index: `HashMap<symbol, Set<connection_id>>`. When an update for AAPL arrives (from Kafka), the pod looks up AAPL in its local index and writes the update to all local connections subscribed to AAPL. This is O(1) lookup + O(local_AAPL_subscribers) write. Across 50 pods each handling 10 k connections, each pod has ~2 k AAPL subscribers (100 k total / 50 pods). Writing 2 k messages is fast (async, non-blocking write to WS send buffers).

**Message deduplication and batching:**
When multiple updates arrive for the same symbol within the same 5 ms "batch window," only the latest quote is sent (last-write-wins). This is critical during market open when AAPL may have 50 updates/s: without batching, we'd send 50 × 100 k = 5 M writes/s for AAPL alone. With 5 ms batching: at most 200 batches/s × 100 k connections = 20 M writes/s — still high but manageable with async I/O.

**Pseudocode:**

```
# WebSocket Gateway Pod

# Per-pod state
subscriptions: HashMap<symbol, Set<ConnectionID>>  # inverted index
connections: HashMap<ConnectionID, WebSocketConn>  # connection registry
pending_updates: HashMap<symbol, QuoteMessage>      # latest update per symbol in window

# Kafka consumer thread: reads updates from Kafka market-data topic
function kafka_consumer_loop():
    while running:
        batch = kafka.poll(max_records=10000, timeout_ms=5)
        for record in batch:
            symbol = record.key
            update = deserialize(record.value)
            # Last-write-wins: only keep latest update per symbol
            pending_updates[symbol] = update
        flush_pending_updates()

# Flush at most every 5ms
function flush_pending_updates():
    updates_to_send = pending_updates.copy()
    pending_updates.clear()

    for symbol, update in updates_to_send.items():
        local_conns = subscriptions.get(symbol, empty_set)
        if local_conns:
            encoded = encode_ws_frame(update)  # encode once, reuse bytes
            for conn_id in local_conns:
                conn = connections[conn_id]
                conn.write_buffer.append(encoded)  # non-blocking, async
            # Flush all buffers in event loop iteration

# Connection handler: subscribe/unsubscribe
function on_subscribe(conn_id, symbols, channels):
    for symbol in symbols:
        subscriptions[symbol].add(conn_id)
        # Also register in Redis for cross-pod discovery
        redis.sadd(f"ws:symbol:{symbol}:pods", THIS_POD_ID)
    # Send current snapshot immediately
    snapshots = redis.hmget([f"market:quote:{s}" for s in symbols])
    connections[conn_id].send(batch_snapshot(snapshots))

function on_unsubscribe(conn_id, symbols):
    for symbol in symbols:
        subscriptions[symbol].discard(conn_id)
        if len(subscriptions[symbol]) == 0:
            # No more local subscribers; unsubscribe this pod from Redis routing
            redis.srem(f"ws:symbol:{symbol}:pods", THIS_POD_ID)

function on_disconnect(conn_id):
    conn = connections.pop(conn_id, None)
    for symbol in conn.subscribed_symbols:
        subscriptions[symbol].discard(conn_id)
```

**Interviewer Q&A:**

Q1: How does the Kafka consumer keep up with 5 M messages/s during market open?
A1: The Kafka topic `market-data` is partitioned by symbol hash (1024 partitions for 10 k symbols). Each WebSocket gateway pod consumes all partitions (since it needs all symbols to serve any subscriber). With 5 M records/s and 50 gateway pods each consuming the full topic independently, each pod sees 5 M records/s from Kafka. However, the 5 ms batch window means each pod processes at most 25 k distinct symbols per flush (5 M/s × 0.005 s). Since we have 10 k symbols, this is well within capacity. The key optimization is last-write-wins batching: 50 updates/s for AAPL become 1 update per 5 ms flush cycle.

Q2: How do you handle a WebSocket pod crash and its connections?
A2: WebSocket connections are TCP connections — they drop immediately when the pod crashes. Clients detect the disconnect (TCP close or ping timeout) within 10–30 s and reconnect. Reconnection goes to any healthy gateway pod (round-robin L4 LB). The new pod re-processes the SUBSCRIBE frames from the client and rebuilds the local subscription state. No server-side session state needs migration. The client may miss updates during the reconnection window (up to 30 s); on reconnect, the server immediately sends current snapshots for all subscribed symbols, so the client gets a fresh view.

Q3: What if one symbol has 400 k subscribers (e.g., all retail users subscribe to AAPL)?
A3: With 50 pods and 400 k subscribers, each pod has ~8 k AAPL subscribers. On each AAPL update (50 updates/s), a pod must write to 8 k connections. At 200 bytes/message × 8 k connections × 50 updates/s = 80 MB/s outbound per pod. With 10 Gbps NIC = 1.25 GB/s capacity, each pod has headroom for ~15 such symbols simultaneously. The key is async non-blocking I/O (epoll/io_uring on Linux) and kernel socket buffers absorbing micro-bursts. If a client's TCP buffer is full (slow client), we drop the oldest pending updates for that connection (market data is time-sensitive; backpressure means serving stale data anyway).

Q4: How do you prevent a slow client from blocking fan-out to other clients?
A4: The write to each connection is non-blocking (using async I/O / event loop). The connection has a send buffer (e.g., 64 KB). If the buffer is full, the write is dropped (market data) or the oldest buffered message is evicted (last-write-wins). After 3 consecutive dropped messages (configurable), the server closes the connection with code 1008 (Policy Violation: too slow). This prevents head-of-line blocking where one slow client delays updates to fast clients.

Q5: How do you handle a burst to 2 M concurrent WebSocket connections during a major market event?
A5: Auto-scaling: the WebSocket gateway is deployed on Kubernetes. A custom HPA metric `ws_connections_per_pod` triggers scale-out when it exceeds 70 k (leaving 30% headroom). At 2 M connections: 2 M / 70 k = ~29 pods needed. k8s can provision new pods in ~60 s. During the scale-out, new connections are distributed to existing + new pods. The L4 load balancer (Google Cloud Load Balancing / AWS NLB) handles this via health check-based routing. Pre-scaling: 1 hour before major events (Fed announcements, earnings releases), we pre-scale to the expected pod count based on historical connection patterns.

---

### 6.3 NBBO Price Aggregation & Data Normalization

**Problem it solves:**
Exchanges send proprietary binary formats (NASDAQ ITCH is binary; NYSE XDP is binary). Prices may be represented differently across exchanges (fixed-point integers with different precision). Symbols may have different names (e.g., Berkshire Hathaway is "BRK.B" on NYSE but "BRK/B" on NASDAQ). We must normalize all feeds into a unified format and compute NBBO in real time.

**Approaches Comparison:**

| Approach | Throughput | Accuracy | Latency |
|---|---|---|---|
| Stream processor (Kafka Streams / Flink) | High | High | 10–100 ms (stream processing overhead) |
| Custom in-process C++ normalizer | Highest | Highest | < 100 µs |
| Python/Node.js normalizer | Low (GIL / single-threaded) | Same | 1–10 ms |
| Shared memory + IPC (zero-copy) | Highest | Highest | < 10 µs |

**Selected: Custom C++ normalizer per exchange, running in the same process as feed handler, using shared memory IPC to the order book processor.**

Implementation detail: The feed handler and normalizer run in a single C++ process using a lock-free SPSC (single-producer, single-consumer) ring buffer (LMAX Disruptor pattern). No serialization overhead. A reference data cache (instrument_id → symbol, tick_size, lot_size) is loaded at startup from PostgreSQL and refreshed every 5 minutes.

**Pseudocode (normalized tick struct + NBBO computation):**

```cpp
// Normalized tick (64 bytes, cache-line aligned)
struct alignas(64) NormalizedTick {
    uint32_t instrument_id;     // internal ID; not exchange symbol string
    int64_t  price_fixed;       // price × 10_000_000 (7 decimal places)
    int32_t  size;
    uint8_t  side;              // 0=bid, 1=ask, 2=trade
    uint8_t  msg_type;          // ADD=0, MODIFY=1, CANCEL=2, TRADE=3
    uint8_t  exchange_id;       // enum: NYSE=0, NASDAQ=1, CBOE=2, ...
    uint8_t  _padding;
    int64_t  exchange_ts_ns;    // nanoseconds since epoch (PTP clock)
    int64_t  ingest_ts_ns;      // nanoseconds since epoch (local clock)
    uint64_t seq_num;           // exchange sequence number
    uint64_t order_id;          // for ADD/MODIFY/CANCEL; 0 for TRADE
};

// NBBO state per instrument
struct NBBOState {
    std::atomic<int64_t> best_bid[MAX_EXCHANGES];  // per-exchange best bid
    std::atomic<int64_t> best_ask[MAX_EXCHANGES];  // per-exchange best ask
    std::atomic<int64_t> nbbo_bid;                 // national best bid
    std::atomic<int64_t> nbbo_ask;                 // national best ask
    std::atomic<int64_t> last_update_ns;
};

// Called by order book processor after every book update for an exchange
void update_nbbo(uint32_t instrument_id, uint8_t exchange_id,
                 int64_t new_best_bid, int64_t new_best_ask) {
    NBBOState& state = nbbo_states[instrument_id];

    state.best_bid[exchange_id].store(new_best_bid, std::memory_order_release);
    state.best_ask[exchange_id].store(new_best_ask, std::memory_order_release);

    // Recompute NBBO
    int64_t max_bid = INT64_MIN, min_ask = INT64_MAX;
    for (int i = 0; i < num_active_exchanges; i++) {
        int64_t b = state.best_bid[i].load(std::memory_order_acquire);
        int64_t a = state.best_ask[i].load(std::memory_order_acquire);
        if (b > max_bid) max_bid = b;
        if (a < min_ask) min_ask = a;
    }

    int64_t old_bid = state.nbbo_bid.exchange(max_bid, std::memory_order_acq_rel);
    int64_t old_ask = state.nbbo_ask.exchange(min_ask, std::memory_order_acq_rel);

    // Only publish if NBBO changed
    if (max_bid != old_bid || min_ask != old_ask) {
        publish_nbbo_update(instrument_id, max_bid, min_ask);
    }
}

// Normalization: exchange-specific symbol → internal instrument_id
uint32_t normalize_symbol(uint8_t exchange_id, const char* exchange_symbol) {
    // Two-level lookup: exchange → symbol string → instrument_id
    // exchange_symbol_map[exchange_id] is a hashmap loaded at startup
    auto it = exchange_symbol_map[exchange_id].find(exchange_symbol);
    if (it == exchange_symbol_map[exchange_id].end()) {
        // Unknown symbol: log and skip
        unknown_symbol_counter.increment();
        return INVALID_INSTRUMENT_ID;
    }
    return it->second;
}
```

**Interviewer Q&A:**

Q1: How do you handle a new stock IPO that wasn't in the reference data at startup?
A1: Reference data is refreshed every 5 minutes from the exchange SIP (Securities Information Processor) consolidated tape, which publishes instrument additions in real-time. On refresh, new instruments are added to the in-memory symbol map without restarting the feed handler process (atomic pointer swap on the hashmap). Feed messages for unknown symbols before the refresh are logged and buffered for 10 seconds; if the symbol appears in the next refresh cycle, buffered messages are replayed.

Q2: How do you handle clock synchronization across exchanges to order events correctly?
A2: Exchange timestamps use their own hardware clocks, synchronized via PTP (Precision Time Protocol) to GPS-derived UTC. Different exchanges have clock offsets of 0–100 µs. For cross-exchange ordering: (1) We use exchange timestamps (not ingest timestamps) as the authoritative ordering signal; (2) When computing NBBO, we don't need strict cross-exchange ordering — each exchange's book is independently correct, and NBBO is computed from the latest state of each exchange book; (3) For historical tick analysis, we sort by exchange timestamp with exchange_id as a tiebreaker; (4) Clock offset calibration: our co-location servers are also PTP-synced, so ingest_ts_ns has < 1 µs accuracy, which helps detect stale exchange data.

Q3: What is the "SIP" and why might you use it instead of direct exchange feeds?
A3: The Securities Information Processor (SIP) is a regulated entity that consolidates quotes and trades from all exchanges into a single NBBO feed. Using SIP simplifies the architecture (one feed instead of 8+) and provides regulatory-compliant NBBO. However, SIP has 2–10 ms latency vs. direct exchange feeds at < 100 µs. For institutional / HFT clients, we provide direct exchange feeds. For retail clients, SIP latency is unnoticeable and acceptable. Our architecture supports both: direct feeds feed into the internal normalizer; SIP data can be used as a validation layer to verify our computed NBBO matches the official SIP NBBO within the expected latency window.

Q4: How would you handle an exchange sending corrupted or out-of-range prices?
A4: Multiple layers: (1) Field-level validation at parse time: price must be positive, size must be positive, price cannot be > 100× previous close (halting check); (2) LULD band check: trades outside the Limit Up/Limit Down band are rejected (exchanges sometimes send "erroneous" prints that are later busted); (3) Cross-validation with SIP: if our computed NBBO diverges from SIP NBBO by > 1%, an alert fires for manual review; (4) Spike detection: if a price moves > 5% within 100 ms with no corresponding halt message, the update is flagged and a human alert is triggered. The system applies these filters but logs the original (potentially bad) data unmodified in the audit trail.

Q5: How do you test the feed handler and order book without a live exchange connection?
A5: (1) PCAP replay: we capture 1 week of live exchange feed data as PCAP files. A "feed simulator" replays the PCAP at 10× speed, feeding the same binary data to the feed handler as if it were the live exchange; (2) Synthetic test scenarios: we have a generator for common edge cases — gap-fill scenarios, crossed book conditions, halt/resume, market open/close; (3) Differential testing: the same PCAP is replayed against both the production order book processor and a reference implementation (Python, slower but verified correct); any difference in final book state = a bug; (4) Shadow mode: new feed handler versions run in parallel with production, receiving the same feed, but their output is not served to clients — just compared to production output.

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling Strategy | Notes |
|---|---|---|
| Feed handlers | One process per exchange; scale by adding exchange sources | Not horizontally scalable per exchange (one TCP/UDP connection per feed) |
| Order book processors | Partition symbols across processor pods (consistent hash by symbol) | Add pods; re-shard symbol assignment via coordinator (ZooKeeper/etcd) |
| Kafka | Add brokers; increase partitions | Symbol-partitioned topic; 1024 partitions for 10 k symbols |
| WebSocket gateways | Horizontal; scale on connection count | Each pod handles 50–100 k connections |
| Redis | Redis Cluster; shard by symbol hash | 16+ shards at full scale |
| REST API | Stateless; scale horizontally behind L7 LB | CDN for delayed-data tier; application cache (Redis) for real-time |
| TimescaleDB | Read replicas for query load | Writes go to primary; chunk-based partitioning handles scale |
| Iceberg / S3 | Infinite (managed) | Trino cluster scales out for parallel query execution |

### DB Sharding
- **Redis (live quotes, order books)**: Shard by `instrument_id % num_shards`. Each Redis shard holds ~600 symbols' live quotes + order books at 16 shards. Order book ZADD commands are O(log N); 50 k/s across 16 shards = 3,125 ops/s per shard — trivial.
- **TimescaleDB**: Auto-partitioned by time (chunks) and symbol. Adding parallel workers (pg_parallel_worker_query) handles query fan-out across chunks. Sharding across multiple PG instances (if needed) uses Citus with `symbol` as the distribution column.
- **S3/Iceberg**: No sharding needed; Iceberg metadata layer handles file-level pruning. Trino queries prune partitions by `(symbol, date)` — a 10-billion-row table scans only the relevant partitions.

### Replication
- Redis: one replica per primary; Sentinel for automatic failover.
- Kafka: replication factor = 3 (min.insync.replicas = 2) for durability.
- TimescaleDB: synchronous streaming replication to hot standby; async replication to read replicas.
- Feed handler state (order book): no replication — if a processor pod fails, the new pod replays from the exchange snapshot + replay channel. Recovery time: 5–30 s depending on exchange snapshot availability.

### Caching
| Layer | What | TTL | Notes |
|---|---|---|---|
| Redis | Live quotes (HGETALL) | No TTL (updated continuously) | P50 < 0.1 ms |
| REST API server (in-process) | Last REST response per symbol | 1 s | Reduces Redis load for burst REST requests |
| CDN (delayed data tier only) | REST quotes (15-min delayed) | 60 s | Retail users on free tier get cached data |
| TimescaleDB query cache | Candle queries (read replicas) | Query result cache | PostgreSQL shared_buffers = 64 GB |

**Interviewer Q&A — Scaling:**

Q1: How do you handle Kafka consumer lag during the 9:30 AM market open burst?
A1: At market open, feed volume jumps 10× within seconds (from overnight 50 k/s to 5 M/s). Kafka consumer lag is the difference between Kafka offset and consumer offset. Mitigations: (1) Pre-warm consumers: at 9:29:55 AM, all consumers increase poll batch size from 1,000 to 10,000 records; (2) Increase consumer threads: at 9:29 AM, WebSocket gateways and order book processors increase their consumer thread count (dynamic thread pool); (3) Last-write-wins batching absorbs burst: even if the WebSocket pod's Kafka consumer lags by 500 ms, it batches all pending updates and sends only the latest, so clients get a fresh snapshot at the cost of intermediate updates; (4) Autoscale: add Kafka partitions for peak months (requires a rebalance during off-hours).

Q2: Can a single Redis instance hold all 10,000 symbols' live quotes?
A2: One HSET per symbol with ~15 fields × 30 bytes/field = 450 bytes/symbol. For 10 k symbols: 4.5 MB. Trivially small. Even the order book (ZADD sorted sets) for 10 k symbols at 10 levels per side = 20 × 30 bytes × 10 k = 6 MB. Total Redis live state for all symbols: < 50 MB. Redis Cluster is used not for memory capacity but for throughput distribution (50 k order book updates/s + 5 M quote updates/s during peak). A single Redis node handles ~1 M commands/s; 16-shard cluster handles 16 M commands/s — sufficient headroom.

Q3: How do you scale the tick archive for PB-scale historical queries?
A3: Apache Iceberg on S3 with Trino as the query engine. Iceberg's metadata layer stores partition statistics (min/max price, row count per Parquet file), enabling predicate pushdown: a query like `WHERE symbol='AAPL' AND date='2024-01-15'` scans only the AAPL partition files for that date (< 100 MB of data vs. 11.7 TB/day). Trino cluster scales horizontally: 50 worker nodes × 64 cores each = 3,200 parallel tasks for large queries. A 1-year AAPL tick query reads ~50 GB of compressed Parquet and completes in < 10 s on 50 workers.

Q4: The feed handler is a single process per exchange — how do you make it fault-tolerant?
A4: The feed handler is the most critical single point of failure. Mitigations: (1) Primary + hot standby: run two instances per exchange; the standby subscribes to the same feed but discards output (shadow mode). On primary failure (detected via heartbeat), standby goes active in < 1 s; (2) State recovery: the standby maintains an up-to-date order book by processing the same feed; no state transfer needed on failover; (3) Gap fill: the transition from primary to standby may cause a brief sequence gap; the standby handles this via the exchange's gap fill channel; (4) Monitoring: each feed handler emits a heartbeat every 100 ms; a watchdog process restarts the handler if heartbeat stops.

Q5: How would you add a new exchange without downtime?
A5: (1) Add feed handler binary for the new exchange protocol; (2) Add new exchange_id to the enum and reference data tables; (3) Deploy the new feed handler process (zero impact on existing handlers); (4) The new exchange's ticks flow through the same Kafka topic (partitioned by symbol); (5) Order book processors automatically handle the new exchange_id in `best_bid[exchange_id]` array; (6) NBBO recomputation now includes the new exchange. The only risk is if the new exchange has symbols not yet in the reference data — handled by the 5-minute refresh cycle. Total deployment: one deploy operation, no downtime for existing exchanges.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| Exchange feed disconnection | Stale data for that exchange; NBBO may be stale | Heartbeat timeout (5 s) | Reconnect with exponential backoff; mark exchange as stale; exclude from NBBO | < 5 s recovery |
| Order book processor crash | No updates for affected symbols | Pod health check (k8s) | k8s restarts pod; replay from exchange snapshot; ~30 s gap | < 60 s |
| Kafka broker failure | Consumer lag increase | Kafka metrics | RF=3 ensures continuity; consumers reconnect to replicas | < 10 s |
| Redis primary failure | Live quote reads fail | Redis Sentinel; health check | Sentinel promotes replica; clients reconnect | < 10 s |
| WebSocket gateway crash | 50 k clients disconnect | TCP disconnect detected by clients | Clients reconnect to other pods; get fresh snapshots | Client reconnect: 10–30 s |
| TimescaleDB primary failure | Historical query writes fail | PG streaming replication monitor | Patroni failover to standby | < 30 s |
| S3 regional outage | Tick archive writes fail; historical queries fail | Health check | Buffer ticks in Kafka (7-day retention) until S3 recovers; use replica region for queries | Live data unaffected; history: < 1 hr |
| Full datacenter failure | All services down | External health checks | Failover to DR datacenter; requires feed reconnection (new IP for exchange TCP) | ~5 min |

### Retries & Idempotency
- **Feed retransmission requests**: at most 3 retries per gap; after 3rd failure, request full snapshot from exchange.
- **Kafka produce (from normalizer)**: set `acks=all`, `retries=MAX_INT`, `delivery.timeout.ms=30000`; Kafka producer retries automatically.
- **Redis writes (quote updates)**: at-most-once (fire-and-forget). If Redis is down, the quote update is dropped and will be overwritten by the next tick anyway. Acceptable for real-time data.
- **TimescaleDB candle writes**: Kafka consumer with at-least-once semantics; candle INSERT uses `ON CONFLICT (symbol, interval, open_ts) DO UPDATE` (upsert) for idempotency.

### Circuit Breaker
- **Feed handler → exchange TCP**: if reconnection attempts exceed 10 in 60 s, circuit opens; alert fires; human intervention required (exchange may be down for the day).
- **Order book processor → Redis** (quote publish): if Redis error rate > 20% over 10 s, circuit opens; processor continues updating in-memory state and publishes to Kafka; Redis publish skipped. REST snapshot API falls back to reading directly from Kafka (slightly higher latency).
- **WebSocket gateway → Redis** (subscription routing): if Redis is unreachable, gateways fall back to local-only subscriptions (connections on other pods won't receive updates for missing symbols until Redis recovers, ~10 s).

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `feed_messages_per_second{exchange}` | Counter | Drop > 50% baseline | Feed health |
| `feed_sequence_gap_count{exchange}` | Counter | > 0 per minute | Data completeness |
| `order_book_update_latency_µs` | Histogram | P99 > 1000 µs | Processing lag |
| `nbbo_staleness_ms{symbol}` | Gauge | > 10 ms for any symbol | NBBO freshness |
| `kafka_consumer_lag{consumer_group}` | Gauge | > 100 k messages | Processing backlog |
| `ws_connections_total` | Gauge | — | Scaling signal |
| `ws_delivery_latency_ms` | Histogram | P99 > 50 ms | Client SLA |
| `ws_dropped_updates_rate{symbol}` | Counter | > 0.1% | Slow client backpressure |
| `redis_command_latency_ms` | Histogram | P99 > 5 ms | Redis health |
| `rest_api_latency_ms` | Histogram | P99 > 100 ms | API SLA |
| `tick_archive_write_lag_s` | Gauge | > 60 s | Archive pipeline health |
| `feed_handler_heartbeat{exchange}` | Gauge | 0 (missed heartbeat) | Feed liveness |
| `nbbo_vs_sip_divergence_bps{symbol}` | Gauge | > 5 bps | Data accuracy |

### Distributed Tracing
- OpenTelemetry with custom attributes for `exchange`, `symbol`, `seq_num`.
- Critical trace: exchange message → feed handler parse → normalizer → Kafka produce → consumer → WebSocket write. Target: entire trace < 50 ms (P99).
- Sampling: 100% for sequences containing gaps or anomalies; 0.01% for normal flow (high volume precludes full sampling).
- Market close audit: at 4:00 PM ET, run a daily reconciliation job comparing our OHLCV candles against exchange-published daily statistics files; any divergence > 0.001% triggers an alert.

### Logging
- Feed handler: log every gap event, retransmission request, heartbeat failure. Log exchange connection open/close with timestamp.
- Order book: log only on anomaly (price spike > 5%, locked/crossed book condition).
- WebSocket gateway: log client connections/disconnections with user_id, symbol subscription list size, connection duration.
- Audit trail: all raw feed bytes written to Kafka `feed-audit` topic with retention = forever (compaction disabled); batched to S3 Glacier daily as WORM storage (Object Lock) for SEC compliance.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Why Chosen |
|---|---|---|---|
| Feed handler language | C++ (kernel bypass, DPDK) | Java (Disruptor) | C++ + DPDK achieves < 10 µs processing vs. Java's ~100 µs with JVM overhead; critical for co-location |
| Order book state | In-memory per processor pod | Redis-backed | In-memory is 100× faster; Redis adds network round-trip (200 µs) on every order book update |
| Message bus | Kafka | Redis Streams | Kafka's durability and replay capability are essential for audit compliance and recovery; Redis Streams lacks at-PB scale retention |
| Historical storage | Apache Iceberg (Parquet on S3) | Cassandra or TimescaleDB | At PB scale, columnar Parquet is 10–50× more cost-efficient and faster for analytics queries than row-oriented DBs |
| NBBO computation | In-process after book update | Separate service | In-process avoids network round-trip; latency < 1 µs vs. 1–5 ms for separate service |
| WebSocket fan-out | In-pod inverted index + Kafka | Redis pub/sub | In-pod lookup is O(local_subs) with zero network calls; Redis pub/sub adds 1–5 ms per update |
| Price representation | Fixed-point int64 | float64 | Fixed-point eliminates floating-point rounding errors (critical: $0.0001 rounding error on 1M shares = $100 error) |
| Candle storage | TimescaleDB | InfluxDB | TimescaleDB's SQL compatibility and continuous aggregates are more operationally familiar; InfluxDB's licensing changed in 2023 |
| Data entitlement enforcement | API key permission check at WebSocket subscribe time | Per-message check | Subscribe-time check is O(1) per connection setup; per-message check would add latency to every update |

---

## 11. Follow-up Interview Questions

Q1: How do you handle "locked" and "crossed" markets (best bid ≥ best ask across exchanges)?
A1: In normal market conditions, the best bid across all exchanges should always be < best ask (crossed = bid > ask, locked = bid = ask). When our NBBO shows a crossed or locked market: (1) This is often a transient state during rapid market movement lasting microseconds; (2) The system marks the NBBO as "crossed" and publishes it with a `market_state: crossed` flag; (3) Downstream clients (especially trading algorithms) treat crossed markets as a signal to pause; (4) If the crossed state persists > 100 ms, an alert fires — it may indicate a stale quote from one exchange (if one exchange disconnected and its last quote is stale) or genuinely locked markets (which are technically illegal under RegNMS but happen transiently).

Q2: How would you design the system to support options chains (potentially 500 k+ tickers)?
A2: Options chains dramatically expand the symbol space: a single underlying like AAPL may have 5,000 option contracts (multiple expiries × multiple strikes × put/call). Design changes: (1) Symbol partitioning: the 10,000-symbol assumption becomes 500 k symbols; consistent hashing still works but requires more processor pods (~50 pods for 500 k symbols at 10 k/pod); (2) Selective subscription: most options contracts trade rarely; maintain order books only for contracts with active interest (use a hot/cold tier: active options in memory, inactive on Redis with lazy loading); (3) Options require additional fields: strike, expiry, type (put/call), underlying symbol — extend the NormalizedTick struct; (4) Options liquidity is concentrated in near-term ATM strikes; the top 500 contracts by OI represent 90% of activity — handle these like equities; the long tail is cold-tier.

Q3: What's the difference between Level 1, Level 2, and Level 3 market data?
A3: Level 1: best bid/ask (NBBO) and last trade price — what retail users see. Level 2: full order book depth showing all price levels with aggregate size — used by active traders to gauge market depth and direction. Level 3: individual order-level data (each order's ID, price, size, add/modify/cancel) — used by market makers and HFT firms; this is the NASDAQ ITCH / NYSE XDP raw feed. Our system processes Level 3 internally (to correctly maintain Level 2), exposes Level 1 to all users and Level 2 to premium users. Level 3 data is proprietary exchange property and cannot be redistributed without exchange data licensing agreements.

Q4: How would you handle market circuit breakers (trading halts)?
A4: Exchanges publish halt messages (NASDAQ ITCH: "Trading Action" message type 'H'). When received: (1) The order book processor marks the symbol as halted; (2) No further order book updates are processed (halted orders are non-actionable); (3) A `status` event is published to Kafka; (4) WebSocket gateways push `{ type: "status", symbol: "AAPL", market_status: "halted", halt_code: "T1" }` to all AAPL subscribers; (5) REST API reflects `market_status: halted` in quote responses; (6) On resumption, exchange sends an "Open" message; processor resumes and notifies clients. Market-wide circuit breakers (S&P 500 down 7%, 13%, 20%) trigger halts on all symbols simultaneously — the system must handle 10 k simultaneous halt events without queue buildup.

Q5: How do you ensure the 5-year tick archive is queryable within 10 seconds?
A5: Apache Iceberg on S3 with Trino. Key optimizations: (1) Iceberg partitioning by `(symbol, date)` — a 1-year AAPL query scans only 252 partition files instead of all 5×252×10,000 partitions; (2) Parquet file-level statistics (min/max price per file) allow Trino to skip files not matching WHERE predicates; (3) Z-ordering (data layout optimization) within partitions by timestamp ensures temporal locality within each Parquet file, enabling efficient range scans; (4) Trino's dynamic filtering pushes join predicates to table scans; (5) Result: for a 1-year AAPL tick query, Trino reads ~50 GB of compressed Parquet data (AAPL generates ~50 MB/day of tick data × 252 days × 0.4 Parquet compression ratio) distributed across 50 worker nodes = 1 GB/worker. At 500 MB/s S3 read throughput per worker: 2 s data read + 3 s processing = < 5 s total.

Q6: How do you handle the time zone complexity across global markets?
A6: All timestamps stored as Unix microseconds (UTC) in the database and wire protocol. The data model stores `market_timezone` per instrument in the reference data (e.g., "America/New_York" for US equities, "Europe/London" for LSE). "Market open" and "market close" events are computed from exchange calendars (maintained as a separate service that publishes market status events). Clients receive UTC timestamps; the UI layer converts to local time. This eliminates all timezone bugs in the storage and processing layers.

Q7: What is the cost of running this system at full scale?
A7: Rough estimates: (1) Feed handler co-location at exchange data centers: ~$100 k/month (cross-connects + colocation fees); (2) Kafka cluster (50 brokers × $0.50/hr): ~$18 k/month; (3) Order book processors (200 pods × $0.10/hr): ~$14.4 k/month; (4) WebSocket gateways (50 pods × $0.20/hr): ~$7.2 k/month; (5) Redis Cluster (16 nodes × $1/hr): ~$11.5 k/month; (6) TimescaleDB (primary + 5 replicas × $2/hr): ~$8.6 k/month; (7) S3 for tick archive (2.2 PB × $0.023/GB + transfer): ~$50 k/month; (8) Trino query cluster (on-demand): ~$10 k/month; (9) Data licensing fees (exchange market data): $100 k–$500 k/month depending on tier. Total: ~$200 k–$600 k/month in infrastructure + licensing. Revenue model: B2B API access fees at $1,000–$10,000/month per institutional client makes this viable at 100+ clients.

Q8: How would you implement delayed data (15-minute delay) for free-tier users?
A8: A separate Kafka consumer group for the delayed tier reads from the same `market-data` topic but applies a 15-minute hold: each message is buffered in a time-priority queue (min-heap sorted by `exchange_ts`) and released only when `now - exchange_ts >= 15 minutes`. A separate pool of WebSocket gateways and REST API servers serves delayed-tier clients. The delayed-tier gateways have a separate CDN configuration that allows caching REST responses for 60 seconds (safe since data is 15 minutes stale). Implementation detail: the 15-minute buffer can be stored in Redis Sorted Sets (score = release_time; `ZRANGEBYSCORE` to get due messages) — at 500 MB/s ingest × 900 s delay = 450 GB buffer, which exceeds practical Redis memory. Alternative: store delay buffer in a dedicated Kafka topic with an offset pointer that's 15 minutes behind the head.

Q9: How do you prevent market data from being scraped and redistributed by clients?
A9: Data licensing enforcement: (1) Each WebSocket connection has a data entitlement record (stored in `api_keys.data_entitlements`); (2) Real-time data is served only to clients with a `"realtime": true` entitlement, which requires signing a data license agreement and paying exchange fees; (3) Delayed data (15-min) can be redistributed to end users under most exchange licenses; (4) Watermarking: we embed invisible per-client watermarks in the data (small timestamp jitter within exchange-spec tolerances) that allow us to identify the source of leaked data; (5) Rate limiting prevents bulk extraction; (6) Terms of Service prohibit redistribution with legal teeth.

Q10: How do you perform load testing for market open conditions?
A10: (1) PCAP replay testing: capture 5 trading days' worth of exchange feeds; replay at 5× speed to simulate peak load without real money; (2) Synthetic generator: a load testing tool generates ITCH/XDP messages at the target rate (5 M/s) with realistic symbol distribution and message type ratios; (3) Shadow testing: deploy new code alongside production; both receive the same feed; compare outputs; (4) Chaos engineering: randomly terminate feed handler pods during the replay test to verify failover behavior; (5) Pre-market-open drill: 15 minutes before market open every Monday, run a capacity check (verify all pods healthy, Kafka lag = 0, Redis latency P99 < 1 ms).

Q11: What monitoring would tell you "the data is wrong" vs "the data is slow"?
A11: Wrong data indicators: (1) `nbbo_vs_sip_divergence_bps` > 5 bps — our NBBO doesn't match SIP; (2) Order book has a crossed/locked state for > 100 ms with no halt message; (3) Daily OHLCV reconciliation against exchange-published files shows > 0.001% deviation; (4) A symbol's last price deviates > 20% from its prior close with no corresponding news/halt. Slow data indicators: (1) `feed_messages_per_second` drop by > 50%; (2) `kafka_consumer_lag` growing; (3) `nbbo_staleness_ms` increasing; (4) `ws_delivery_latency_ms` P99 > 50 ms. These are distinct alert categories with different runbooks: "wrong data" triggers immediate shutdown of that feed/symbol; "slow data" triggers scaling actions.

Q12: How do you support institutional clients who want to receive the data via FIX protocol?
A12: FIX (Financial Information eXchange) is the standard protocol for institutional trading. We provide a FIX Gateway service: (1) Accepts FIX 4.2/5.0 connections from institutional clients; (2) Clients send MarketDataRequest (MsgType=V) specifying symbols and subscription type (full book or top-of-book); (3) Our FIX Gateway translates incoming NormalizedTick events from Kafka into FIX MarketDataIncrementalRefresh (MsgType=X) messages; (4) Full refresh on subscription start (MsgType=W); (5) FIX uses TCP; the gateway manages FIX session state (SeqNum, heartbeats); (6) Co-located clients can connect directly to the FIX Gateway in our data center, achieving < 100 µs latency.

Q13: How would you add support for cryptocurrency markets?
A13: Crypto exchanges (Coinbase, Binance, etc.) use WebSocket or REST APIs instead of ITCH/XDP binary protocols. The feed handler layer becomes protocol-agnostic: (1) Add a WebSocket feed handler for each crypto exchange that normalizes their JSON/WebSocket format to the same NormalizedTick struct; (2) Crypto markets trade 24/7 with no market hours — update the market status logic; (3) Crypto symbols use different naming conventions (BTC-USD, ETH-USDT); extend the reference data and symbol normalization; (4) Crypto exchanges have much lower message rates (100–10,000/s vs. 5 M/s for equity exchanges) — same infrastructure handles it easily; (5) NBBO concept doesn't apply to crypto (each exchange is independent, no regulated consolidated tape); replace with "best available price across exchanges."

Q14: What happens to your system if Kafka goes down completely?
A14: Kafka is the central nervous system. With Kafka down: (1) Feed handlers cannot publish normalized ticks — they buffer in their local ring buffer (pre-allocated 1 GB, ~10 s of peak data); (2) After 10 s, ring buffer fills; feed handlers start dropping messages (strategy: drop candle/history ticks first, keep NBBO ticks); (3) Order book processors cannot receive updates — use last known state (clearly marked as stale in Redis with age field); (4) WebSocket clients see stale data; (5) REST API returns last known quotes with a `stale: true` flag; (6) No new ticks written to archive. Recovery: Kafka recovers from Raft/ZooKeeper consensus in < 30 s for broker failure. Full Kafka cluster failure requires operator intervention; target RTO = 5 minutes. To mitigate: Kafka racks are spread across 3 AZs; all-AZ failure is extremely unlikely with proper topology settings.

Q15: How does your design handle the market data business model (exchange fees)?
A15: Exchange market data is licensed, not free. Per-user fees: NYSE charges per "professional user" (~$15/month/user) and "non-professional user" (~$1.50/month/user). Design implications: (1) The `api_keys.data_entitlements` field stores the client's professional/non-professional status (self-certified at signup); (2) Monthly reporting: a batch job counts distinct users who received real-time data in the reporting period and generates exchange compliance reports; (3) "Display restriction": certain exchange data can only be shown in a "top-of-book" format (not full depth) without additional licenses; the WebSocket API enforces this at the subscription layer; (4) Revenue calculation: 500 k users × $3/month avg blended = $1.5 M/month in licensing cost — a major input to the business model.

---

## 12. References & Further Reading

1. NASDAQ ITCH 5.0 Feed Specification: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
2. NYSE Integrated Feed (XDP) Specification: https://www.nyse.com/publicdocs/nyse/data/XDP_Integrated_Feed_Client_Specification.pdf
3. CTA/UTP SIP (Securities Information Processor) Overview: https://www.ctaplan.com/index
4. LMAX Disruptor Pattern: https://lmax-exchange.github.io/disruptor/disruptor.html
5. Apache Kafka Documentation: https://kafka.apache.org/documentation/
6. Apache Iceberg Table Format Specification: https://iceberg.apache.org/spec/
7. Redis HyperLogLog and Sorted Sets: https://redis.io/docs/data-types/sorted-sets/
8. TimescaleDB Documentation (Hypertables and Compression): https://docs.timescale.com/use-timescale/latest/hypertables/
9. FIX Protocol Specification (FIX 5.0 SP2): https://www.fixtrading.org/standards/
10. "Designing Data-Intensive Applications" — Martin Kleppmann, Chapters 11 (Stream Processing) and 3 (Storage/Retrieval)
11. Jane Street: OCaml in the Trading World (demonstrates low-latency processing concerns): https://www.janestreet.com/technology/
12. Regulation NMS (National Market System) — SEC: https://www.sec.gov/rules/final/34-51808.pdf
13. Aeron (low-latency messaging library): https://github.com/real-logic/aeron
14. Trino (distributed query engine) Documentation: https://trino.io/docs/current/
15. "The Art of Problem Solving in Software Engineering" — financial systems section — Various trading infrastructure blogs on Hacker News / engineering.atkin.com
