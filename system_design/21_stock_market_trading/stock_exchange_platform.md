# System Design: Stock Exchange Platform (NYSE / NASDAQ-style)

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Member onboarding**: Register and manage member firms (brokers, market makers, institutional traders) with connectivity credentials and entitlements.
2. **Order entry**: Accept orders via FIX protocol or binary protocol from member firms; validate, risk-check, and route to the matching engine.
3. **Order matching**: Match buy and sell orders using price-time priority; support Limit, Market, Stop, Stop-Limit, and Iceberg order types.
4. **Market data dissemination**: Publish real-time Level 1 (NBBO, last trade) and Level 2 (full order book depth) data feeds to subscribers.
5. **Trade reporting**: Disseminate trade prints (filled trades) in real time to all market participants and reporting systems.
6. **Pre-trade and post-trade risk controls**: Per-firm buying power, position limits, price collars, and duplicate order detection.
7. **Clearing and settlement integration**: Route confirmed trades to the clearinghouse (DTCC) for netting and settlement (T+1).
8. **Market operations**: Support trading halts (regulatory, circuit breaker), market open/close auctions, and symbol listing/delisting.
9. **Regulatory reporting**: Generate CAT (Consolidated Audit Trail) reports and trade surveillance feeds.
10. **Historical data archive**: Store all orders, cancels, fills, and quotes with nanosecond precision; queryable for 7+ years.

### Non-Functional Requirements
1. **Latency**: Order-to-acknowledgment P99 < 100 μs for co-located members; < 1 ms for non-co-located.
2. **Throughput**: 2M order messages/second peak (market open); 500K/sec sustained.
3. **Availability**: 99.999% during market hours; planned downtime only in off-hours maintenance windows.
4. **Fairness**: Strict price-time priority; equal network latency treatment for all co-located members (deterministic fiber length "speed bumps" where applicable).
5. **Durability**: Zero order or trade data loss; WAL-backed with < 1s recovery.
6. **Compliance**: SEC Rules 15c3-5 (Market Access Rule), FINRA Rule 5210, Regulation NMS, CAT reporting.
7. **Security**: All member connections over dedicated leased lines or VPN; mutual TLS; no public internet access to order entry systems.
8. **Auditability**: Immutable audit trail of all events; tamper-evident (cryptographic chaining of log records).

### Out of Scope
- Retail-facing trading app (handled by broker platforms)
- Portfolio management or advisory services
- Derivative products (options/futures — separate exchange systems)
- Cryptocurrency trading
- Payment processing and banking infrastructure
- News or research distribution

---

## 2. Users & Scale

### User Types
| Role | Description | Volume |
|---|---|---|
| Member Firm (Broker-Dealer) | Routes client and proprietary orders; ~500 member firms | 100K–1M orders/day per firm |
| Market Maker / DMM | Posts continuous two-sided quotes; keeps book liquid | 1M+ quote updates/day per symbol |
| Institutional Trader | Large block trades; algorithmic execution (TWAP/VWAP) | 10K–100K orders/day |
| Market Data Subscriber | Real-time feed consumer (Bloomberg, Reuters, hedge funds) | Read-only; 50K+ subscribers |
| Regulatory Body (SEC/FINRA) | Audit trail consumer; surveillance | Batch reads |
| Exchange Operations | Halt/resume trading; manage listings; monitor health | Low volume, high privilege |

### Traffic Estimates

**Assumptions:**
- 8,000 listed symbols (NYSE-scale)
- 500 registered member firms
- Market hours: 9:30 AM – 4:00 PM ET (6.5 hours = 23,400 seconds)
- Peak multiplier: 5x at open/close

| Metric | Calculation | Result |
|---|---|---|
| Total order messages/day | 500 firms × 2M messages/firm/day | 1B messages/day |
| Average order events/sec | 1B / 23,400 | ~42,700/sec |
| Peak order events/sec | 42,700 × 5 | ~213,500/sec (design target: 2M/sec burst) |
| Fills/day | ~20% fill rate × 1B orders | ~200M fills/day |
| Market data updates/sec | 8K symbols × 50 book changes/sec avg | 400K book updates/sec |
| Market data fan-out | 400K × 200 avg subscribers/symbol | 80M deliveries/sec |
| CAT report records/day | 1B orders + 200M fills | ~1.2B records/day |
| CAT storage/day | 1.2B × 300 bytes | ~360 GB/day |
| Total audit archive/7 years | 360 GB × 252 days × 7 years | ~635 TB |

### Latency Requirements
| Operation | Co-located (P99) | Remote (P99) | Notes |
|---|---|---|---|
| Order entry to ACK | < 100 μs | < 1 ms | "Acknowledgment" = sequenced + WAL written |
| Order entry to fill notification | < 500 μs | < 5 ms | Including matching time |
| Market data (Level 1) publication | < 1 ms | < 10 ms | From fill event to NBBO update |
| Market data (Level 2) publication | < 2 ms | < 20 ms | Order book depth update |
| Opening auction price dissemination | < 100 ms | < 500 ms | Batch computation |
| Order cancel ACK | < 50 μs | < 500 μs | O(1) cancel with hash index |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                          MEMBER FIRM CONNECTIVITY                            │
│  Co-located Members (fiber, μs latency)                                      │
│  Remote Members (dedicated leased line or VPN, ms latency)                  │
└────────────────────────────────┬─────────────────────────────────────────────┘
                                 │  FIX / Binary (OUCH/ITCH)
┌────────────────────────────────▼─────────────────────────────────────────────┐
│                         ORDER ENTRY LAYER                                    │
│  ┌───────────────────┐   ┌───────────────────┐   ┌───────────────────┐      │
│  │  Gateway Node 1   │   │  Gateway Node 2   │   │  Gateway Node N   │      │
│  │  (per member firm)│   │  (per member firm)│   │  (per member firm)│      │
│  │  FIX session mgmt │   │  FIX session mgmt │   │  FIX session mgmt │      │
│  │  Protocol parse   │   │  Protocol parse   │   │  Protocol parse   │      │
│  └────────┬──────────┘   └────────┬──────────┘   └────────┬──────────┘      │
└───────────┼──────────────────────┼──────────────────────────┼───────────────┘
            │                      │                          │
┌───────────▼──────────────────────▼──────────────────────────▼───────────────┐
│                         RISK GATEWAY LAYER                                   │
│  Pre-trade risk checks per firm:                                             │
│  - Buying power (Redis — real-time balance)                                  │
│  - Position limits (Redis — per symbol per firm)                             │
│  - Price collar (last trade price ± threshold)                               │
│  - Order rate limiter (per firm, per second)                                 │
│  - Duplicate order detection (Bloom filter + Redis SETNX)                   │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │  Risk-cleared orders
┌──────────────────────────────────▼──────────────────────────────────────────┐
│                         ORDER SEQUENCER                                      │
│  - Single-threaded; assigns global monotonic sequence numbers               │
│  - Writes to WAL (Chronicle Queue / mmap'd file on NVMe SSD)                │
│  - Hot standby with < 1s failover                                           │
│  - Publishes sequenced order stream to Matching Engine ring buffer          │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼──────────────────────────────────────────┐
│                      MATCHING ENGINE CLUSTER                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │
│  │  Engine-A    │  │  Engine-B    │  │  Engine-C    │  │  Engine-D    │   │
│  │  Symbols A–J │  │  Symbols K–Q │  │  Symbols R–V │  │  Symbols W–Z │   │
│  │  ~2K symbols │  │  ~2K symbols │  │  ~2K symbols │  │  ~2K symbols │   │
│  │  + standby   │  │  + standby   │  │  + standby   │  │  + standby   │   │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘   │
└─────────┼────────────────┼────────────────────┼────────────────┼───────────┘
          │                │                    │                │
┌─────────▼────────────────▼────────────────────▼────────────────▼───────────┐
│                         EVENT DISTRIBUTION BUS                               │
│                                                                              │
│  Execution Reports ─────────────────────────────► Member Gateways          │
│                                                    (FIX ExecutionReport)    │
│  Trade Prints ──────────────────────────────────► SIP (Securities          │
│                                                    Information Processor)   │
│  Order Book Updates ────────────────────────────► Market Data Engine       │
│  All Events ────────────────────────────────────► Kafka → Audit Log        │
│                                                    (Parquet on S3)          │
│  Trade Confirms ────────────────────────────────► DTCC Clearing Interface  │
└─────────────────────────────────────────────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼──────────────────────────────────────────┐
│                      MARKET DATA ENGINE                                      │
│  - Aggregates book updates across all engines                               │
│  - Computes NBBO (National Best Bid and Offer)                              │
│  - Publishes Level 1 (ITCH/OPRA) and Level 2 (full depth) feeds            │
│  - Multicast delivery to subscribers (UDP multicast for low-latency fans)  │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Primary use-case data flow (limit order → fill → trade report):**

1. Member firm sends FIX `NewOrderSingle` (buy 500 AAPL @ $180.00 limit)
2. Gateway Node: parses FIX, validates message format, performs session authentication
3. Risk Gateway: checks buying power (500 × $180 = $90,000 against firm's pre-funded margin), price collar ($180 within 5% of last trade $179.95), no duplicate (idempotency check)
4. Sequencer: assigns seq# 12,847,231, writes to WAL, forwards to Engine-A ring buffer
5. Engine-A: matches against resting sell at $179.95 (500 shares available) → full fill at $179.95
6. Engine-A emits: Fill event, two ExecutionReports (buyer + seller), order book update
7. Fill event → SIP (Securities Information Processor): updates consolidated tape (last trade $179.95 × 500 AAPL)
8. ExecutionReports → Member Gateways → FIX clients (buyer and seller notified)
9. Order book update → Market Data Engine: NBBO updated, Level 2 feed updated
10. Fill event → Kafka → DTCC Interface: trade routed for T+1 settlement
11. All events → Kafka → Parquet on S3: immutable audit log (CAT reporting)

---

## 4. Data Model

### Member Firm Registry (PostgreSQL)
```sql
CREATE TABLE member_firms (
    firm_id         VARCHAR(20)  PRIMARY KEY,
    firm_name       VARCHAR(200) NOT NULL,
    mpid            CHAR(4)      NOT NULL UNIQUE, -- Market Participant ID (4 chars, SEC-assigned)
    status          VARCHAR(20)  NOT NULL,        -- ACTIVE | SUSPENDED | TERMINATED
    membership_type VARCHAR(20)  NOT NULL,        -- BROKER_DEALER | MARKET_MAKER | DMM
    clearing_firm   VARCHAR(20)  REFERENCES member_firms(firm_id),
    created_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE TABLE firm_entitlements (
    firm_id         VARCHAR(20)  NOT NULL REFERENCES member_firms(firm_id),
    symbol          VARCHAR(20),                  -- NULL = all symbols
    order_types     VARCHAR[]    NOT NULL,         -- LIMIT, MARKET, STOP, etc.
    max_order_qty   BIGINT       NOT NULL DEFAULT 999999,
    max_position    BIGINT       NOT NULL DEFAULT 10000000,
    max_notional    DECIMAL(20,2) NOT NULL,
    PRIMARY KEY (firm_id, COALESCE(symbol, ''))
);

CREATE TABLE trading_sessions (
    session_id      VARCHAR(40)  PRIMARY KEY,
    firm_id         VARCHAR(20)  NOT NULL REFERENCES member_firms(firm_id),
    gateway_node    VARCHAR(20)  NOT NULL,
    protocol        VARCHAR(10)  NOT NULL,         -- FIX_44 | FIX_50 | OUCH | BINARY
    connected_at    TIMESTAMPTZ  NOT NULL,
    disconnected_at TIMESTAMPTZ,
    status          VARCHAR(10)  NOT NULL          -- ACTIVE | DISCONNECTED
);
```

### Symbol Registry (PostgreSQL)
```sql
CREATE TABLE symbols (
    symbol          VARCHAR(20)  PRIMARY KEY,
    company_name    VARCHAR(300) NOT NULL,
    isin            CHAR(12)     NOT NULL UNIQUE,
    cusip           CHAR(9)      UNIQUE,
    listing_venue   VARCHAR(10)  NOT NULL,         -- NYSE | NASDAQ | CBOE
    lot_size        INTEGER      NOT NULL DEFAULT 1,
    tick_size       DECIMAL(10,6) NOT NULL DEFAULT 0.01,
    status          VARCHAR(20)  NOT NULL,         -- LISTED | HALTED | DELISTED
    listing_date    DATE         NOT NULL,
    price_collar_pct DECIMAL(5,2) NOT NULL DEFAULT 5.00,
    circuit_breaker_pct DECIMAL(5,2) NOT NULL DEFAULT 10.00
);
```

### Audit Log (Parquet on S3 — immutable)
```
Partition structure: s3://exchange-audit/year=2024/month=01/day=15/symbol=AAPL/
File naming: {engine_id}_{seq_start}_{seq_end}.parquet

Schema (Parquet):
  seq_num          INT64     -- global monotonic, never reused
  event_type       STRING    -- NEW_ORDER | CANCEL | MODIFY | FILL | REJECT | HALT
  event_ts_ns      INT64     -- nanosecond epoch timestamp
  symbol           STRING
  order_id         STRING
  client_order_id  STRING
  firm_mpid        CHAR(4)
  side             STRING    -- BUY | SELL
  order_type       STRING
  price            DOUBLE
  qty              INT64
  filled_qty       INT64
  fill_price       DOUBLE
  contra_order_id  STRING
  contra_firm_mpid CHAR(4)
  time_in_force    STRING
  engine_id        STRING
  session_id       STRING
  
Tamper-evidence: each Parquet file's SHA-256 hash stored in a separate index.
Files are WORM (Write Once Read Many) on S3 via Object Lock (compliance mode, 7-year retention).
```

---

## 5. API Design

### FIX Protocol (Member Firm Connection)
```
Session Layer:
  FIX 4.4 / 5.0 (standard equities)
  Binary OUCH 4.2 / ITCH 5.0 (low-latency, co-located members)
  TCP over dedicated fiber (co-location) or VPN (remote)

Key FIX Messages:
  35=D  NewOrderSingle    → order entry
  35=F  OrderCancelRequest → cancel
  35=G  OrderCancelReplaceRequest → cancel-replace (modify)
  35=H  OrderStatusRequest → query order status
  35=8  ExecutionReport   → fill/ack/reject notification (outbound)
  35=9  OrderCancelReject → cancel rejected (outbound)
  35=j  BusinessMessageReject → protocol-level reject (outbound)
```

### Internal REST API (Exchange Operations)
```
GET  /ops/v1/symbols/{symbol}/halt
POST /ops/v1/symbols/{symbol}/halt
Body: { "reason": "circuit_breaker | regulatory | volatility", "duration_minutes": 15 }
Response 200 OK: { "symbol": "AAPL", "halted": true, "resume_at": "2024-01-15T14:45:00Z" }

GET  /ops/v1/symbols/{symbol}/orderbook
Response 200 OK: { top-10 bid/ask levels, last trade, NBBO, sequence_number }

GET  /ops/v1/firms/{mpid}/risk
Response 200 OK: { buying_power, open_orders_count, gross_position_value, per_symbol_positions }

POST /ops/v1/firms/{mpid}/suspend
Body: { "reason": string }
Response 200 OK: { "firm_mpid": "ABCD", "status": "SUSPENDED", "effective_ts": "..." }
All open orders for suspended firm cancelled immediately.

GET  /audit/v1/orders?symbol=AAPL&from=2024-01-15T09:30:00Z&to=2024-01-15T16:00:00Z&firm=ABCD
Response 200 OK: paginated list of order events from audit log (Parquet query via Athena)
```

### Market Data API (Subscribers)
```
WebSocket: wss://marketdata.exchange.com/v1/stream
Subscribe: { "action": "subscribe", "channels": ["level1", "level2"], "symbols": ["AAPL", "MSFT"] }

Level 1 message (NBBO update):
  { "type": "nbbo", "symbol": "AAPL", "bid": "179.95", "bid_size": 500,
    "ask": "180.00", "ask_size": 300, "seq": 9847231, "ts_ns": 1705320600123456789 }

Level 2 message (order book depth):
  { "type": "book_update", "symbol": "AAPL", "side": "ask", "price": "180.00",
    "qty": 300, "order_count": 2, "action": "UPDATE", "seq": 9847232 }

Trade print:
  { "type": "trade", "symbol": "AAPL", "price": "179.95", "qty": 100,
    "aggressor_side": "BUY", "seq": 9847231, "ts_ns": 1705320600123456789 }
```

---

## 6. Deep Dive: Core Components

### Component: Opening Auction

**Problem it solves:** At market open (9:30 AM ET), there is a large backlog of pre-market orders with no continuous trading to establish a reference price. A batch auction collects all pre-market orders, finds the price that maximizes executed volume, and executes all matches at a single price simultaneously — fairer than continuous matching for the open.

**Algorithm:**

```
Pre-market order collection window: 4:00 AM – 9:28 AM ET
Orders accepted: LIMIT, Market-on-Open (MOO), Limit-on-Open (LOO), no cancels allowed last 2 min

Opening auction algorithm:
1. For each candidate price P (from lowest ask to highest bid, at tick increments):
   - Executable buy volume = sum of qty of all buy orders with price >= P + all MOO buys
   - Executable sell volume = sum of qty of all sell orders with price <= P + all MOO sells
   - Executable volume at P = min(executable buy volume at P, executable sell volume at P)

2. Select clearing price = P that maximizes executable volume
   - Tiebreaker 1: price closest to previous close
   - Tiebreaker 2: highest executable volume on same side as imbalance

3. Execute all matching orders at the single clearing price
   - Prioritize: MOO > LOO with best limit > LOO prorated at limit = clearing price
   - Unexecuted LOO orders cancelled; unexecuted MOO orders filled at clearing price regardless

4. Publish: Opening trade print, updated NBBO, residual orders enter continuous trading
```

**Interviewer Deep-Dive Q&A:**

Q: How do you prevent front-running in the opening auction?
A: Several mechanisms: (1) **Order type restriction**: During the last 2 minutes before the auction, cancels are not accepted (order modifications also restricted) — this prevents traders from seeing the indicative price and then cancelling to avoid execution. (2) **Indicative price dissemination**: The exchange publishes an indicative clearing price every 15 seconds during the pre-market window — but this is available to all participants simultaneously. (3) **Randomized execution time**: The exact millisecond the auction fires is randomized within a 30-second window to prevent latency arbitrage. (4) **Equal treatment**: All orders at the clearing price are filled pro-rata regardless of submission time — time priority only applies within the residual continuous book.

Q: What happens if there are more buy orders than sell orders at the clearing price (buy-side imbalance)?
A: The clearing price is the price that maximizes matched volume. If there's residual buy imbalance (more buys than sells at the clearing price), the following happens: (1) All sell-side orders that can be matched are fully executed. (2) On the buy side, MOO orders are filled in full, then LOO orders at the clearing price are filled pro-rata based on their size relative to the available sell volume. (3) The unfilled buy orders become resting orders in the continuous book at their limit prices after the auction. (4) The exchange publishes an imbalance indicator during pre-market to encourage offsetting orders.

---

### Component: Clearing and Settlement Integration

**Problem it solves:** Every fill generates a binding trade obligation between buyer and seller. These obligations must be netted, guaranteed, and settled (T+1 in the US since 2024). The clearinghouse (DTCC/NSCC) acts as the central counterparty, guaranteeing trades even if one party defaults.

**Flow:**

```
1. Fill occurs at matching engine
2. Fill event → Trade Report Processor → formats DTCC-compatible trade record
   Format: FIX TRD (Trade Report) or DTCC proprietary format
   Fields: symbol, CUSIP, quantity, price, buyer MPID, seller MPID, trade time, seq#

3. Trade Record → DTCC NSCC (National Securities Clearing Corporation)
   Via: SRO (Self-Regulatory Organization) reporting interface
   At: End-of-day batch (for T+1) + real-time intraday for large trades
   
4. DTCC performs novation: replaces buyer-seller obligation with:
   - Buyer now owes DTCC (not seller)
   - Seller now owes DTCC (not buyer)
   Central counterparty guarantee: if seller defaults, DTCC covers buyer.

5. DTCC performs netting: consolidates all trades per member firm across all exchanges
   Example: Firm A bought 1000 AAPL and sold 800 AAPL today → net delivery: 200 AAPL

6. T+1 settlement: DTC (Depository Trust Company) moves cash and securities between
   member firm accounts. Exchange updates firm positions accordingly.

7. Exchange receives settlement confirmation; updates firm buying power in Redis.
```

---

### Component: CAT (Consolidated Audit Trail) Reporting

**Problem it solves:** SEC Rule 613 requires all US exchanges and broker-dealers to report every order event (new, cancel, modify, fill) to the CAT system within seconds of occurrence. The CAT is the national audit trail for market surveillance.

**Implementation:**

```
CAT Reporter Service (separate from trading path):
1. Consumes from Kafka audit topic (same events as internal audit log)
2. Enriches with: Customer Account Information (from broker-dealer's CAT records), 
   MPID, clearing firm, event timestamps to nanosecond precision
3. Formats CAT record (JSON per SEC schema version 2.3+)
4. Delivers to CAT CAIS (CAT Acceptance and Ingestion System) via SFTP or REST API
   Deadline: Within 8 seconds for manual orders; within 1 second for electronic orders
5. Receives confirmation; logs delivery receipt for compliance audit
6. Retention: CAT records stored by CAT LLC for 6 years; exchange keeps own copy 7 years

Error handling:
- Failed submissions retried with exponential backoff
- Missed submissions (> 1% error rate) generate compliance alert
- CAT system downtime: buffer locally (up to 24h) and batch-deliver when restored
```

---

## 7. Failure Scenarios & Resilience

### Scenario 1: Exchange-Wide Outage
- **Regulatory requirement**: Exchange must notify SEC within 30 minutes if trading halted > 30 minutes.
- **Failover**: Hot standby data center (Carteret, NJ ↔ Mahwah, NJ, ~10 miles apart). Automated failover triggered by health monitor within 30 seconds. Members automatically reconnect to failover site via anycast DNS.
- **Data**: WAL replicated synchronously to standby site (same NVMe-oF fabric). Zero data loss on failover.

### Scenario 2: Regulatory Halt (SEC-Initiated)
- **Trigger**: SEC can issue a Trading Suspension for any security (10-day max).
- **Implementation**: Halt command delivered via encrypted API call from SEC Regulatory Gateway. Exchange ops confirms receipt. All open orders for the symbol cancelled (FIX OrderCancelReject with reason "regulatory halt" sent to each member). Symbol status set to HALTED in registry. Resume only via explicit SEC lift notification.

### Scenario 3: Market-Wide Circuit Breaker (Rule 48)
- **Trigger**: S&P 500 falls 7% (Level 1), 13% (Level 2), or 20% (Level 3) from prior close.
- **Implementation**: Exchange receives halt signal from SIP (cross-market coordination). Level 1 & 2: 15-minute halt for all equities; Level 3: halt for remainder of day.
- **Mechanism**: All matching engines simultaneously halt order processing. Order books frozen. Members receive FIX BusinessMessageReject. Resume announced via SIP feed.

### Scenario 4: Member Firm Connection Loss
- **FIX session**: TCP disconnect detected via FIX heartbeat timeout (30 seconds). Gateway marks session as DISCONNECTED. **All open orders for that session remain in the book** — they are NOT auto-cancelled (this is exchange policy; members can request "cancel on disconnect" feature via session parameter `CancelOnDisconnect=Y`). Members must reconnect and manually cancel via FIX OrderCancelRequest or mass cancel.

---

## 8. Monitoring & Observability

| Metric | Alert Threshold | Team |
|---|---|---|
| Gateway connection count | Unexpected drop > 10% | Network Ops |
| Order-to-ack latency P99 | > 500 μs (co-located) | Engineering |
| Matching engine heartbeat | Missed > 2 | On-call |
| WAL replication lag to standby | > 100 ms | On-call |
| CAT submission success rate | < 99% | Compliance |
| Risk rejection rate by firm | > 2% | Risk Management |
| Circuit breaker activations | Any | Exchange Ops |
| Order book spread (NBBO) | Abnormally wide for liquid symbols | Market Surveillance |
| Settlement fails | > 0.1% | Operations |
| Clearing house connectivity | Any disconnect | Operations |
