# System Design: Order Management System (OMS)

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Order lifecycle management**: Track every order from creation through submission, partial fills, full fill, cancellation, or rejection — a complete state machine per order.
2. **Multi-asset, multi-venue routing**: Support equities, ETFs, options, and futures; route to multiple execution venues (exchanges, dark pools, market makers, ATSs) based on smart order routing (SOR) logic.
3. **Smart Order Routing (SOR)**: Automatically split and route large orders across multiple venues to minimize market impact and achieve best execution.
4. **Algorithmic execution strategies**: Support TWAP (Time-Weighted Average Price), VWAP (Volume-Weighted Average Price), POV (Percentage of Volume), and Implementation Shortfall (IS) strategies for large institutional orders.
5. **Pre-trade compliance checks**: Enforce investment guidelines (no short selling in certain accounts, sector concentration limits, ESG restrictions), position limits, and regulatory restrictions before order submission.
6. **Allocation**: After fills, allocate executions across multiple client accounts that share a block order (block allocation).
7. **Real-time position and P&L**: Maintain real-time intraday positions, realized and unrealized P&L across all accounts and portfolios.
8. **FIX connectivity**: Connect to brokers and exchanges via FIX protocol; manage sessions, heartbeats, and message sequencing.
9. **Post-trade processing**: Generate confirm/affirm messages; integrate with post-trade systems (prime broker, custody, accounting).
10. **Reporting**: Execution quality reports, TCA (Transaction Cost Analysis), compliance reports, and audit logs.

### Non-Functional Requirements
1. **Latency**: Order creation to broker submission P99 < 50 ms; fill update to position P99 < 10 ms.
2. **Throughput**: 100K orders/day per fund; 10K concurrent open orders per fund manager instance.
3. **Consistency**: Position state must be strongly consistent with fills — no double-counting fills, no lost fills.
4. **Auditability**: Complete, immutable audit trail of every order event, every routing decision, every fill, and every allocation — required for SEC and FINRA examinations.
5. **Availability**: 99.99% during market hours; planned maintenance only after hours.
6. **Multi-tenancy**: Support 1,000+ portfolio managers across multiple funds; strict data isolation between funds.
7. **Compliance**: SEC Rule 17a-4 (record retention 7 years), FINRA Rule 4511, MiFID II (for EU-facing operations), SEC Rule 10b-10 (trade confirmations).
8. **Disaster Recovery**: RTO < 5 minutes, RPO < 1 minute for failover to DR site.

### Out of Scope
- Exchange matching engine
- Retail brokerage functionality
- Cryptocurrency assets
- Fund accounting / NAV computation (separate system)
- Settlement (handled by prime broker / custodian)
- Risk systems (separate pre-trade risk system, though OMS integrates with it)

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Portfolio Manager (PM) | Creates and manages orders; monitors P&L; approves allocations |
| Trader | Executes orders on behalf of PM; uses SOR and algo strategies; monitors fills |
| Compliance Officer | Reviews orders for guideline violations; approves restricted security trades |
| Operations / Middle Office | Handles allocation, confirms, and breaks; reconciles with prime broker |
| Technology / Admin | Manages FIX sessions, venue connectivity, system configuration |
| Prime Broker | External counterparty; receives allocations; provides financing and custody |

### Traffic Estimates

**Assumptions:**
- 500 portfolio managers; 50 funds; $50B AUM
- Average order size: 10,000 shares
- Orders/day: 500 PMs × 100 orders/day = 50,000 parent orders/day
- Each parent order → average 5 child orders (SOR splits + algo slices) = 250K child orders/day
- Each child order → average 3 fill events (partial fills) = 750K fill events/day
- Position updates triggered by fills: 750K/day

| Metric | Calculation | Result |
|---|---|---|
| Parent orders/day | 500 PMs × 100 | 50,000/day |
| Child orders/day | 50K × 5 avg splits | 250,000/day |
| FIX messages/day | Child orders × 10 messages (new + updates + cancel) | 2.5M FIX messages/day |
| Fill events/day | 250K × 3 avg fills | 750,000/day |
| Position updates/day | Triggered by each fill | 750,000/day |
| Average order events/sec | 2.5M / 23,400s | ~107 events/sec |
| Peak order events/sec | 107 × 5 | ~535 events/sec (design target: 10K/sec) |
| Audit log entries/day | 50K + 250K + 750K + compliance checks | ~1.5M entries/day |
| Audit storage/year | 1.5M × 500 bytes × 252 days | ~190 GB/year |
| Active positions in memory | 500 PMs × 200 positions avg × 500 bytes | ~50 MB |

### Latency Requirements
| Operation | P50 | P99 | Notes |
|---|---|---|---|
| Create parent order → submit child to broker | < 10 ms | < 50 ms | Compliance + routing decision |
| Fill receipt → position update | < 2 ms | < 10 ms | Critical for intraday P&L |
| Allocation computation | < 100 ms | < 500 ms | Post-fill; batch or real-time |
| Compliance pre-trade check | < 5 ms | < 20 ms | Must complete before submission |
| Order status query | < 10 ms | < 50 ms | Served from cache |
| TCA report generation | < 5 s | < 30 s | Async query on ClickHouse |

---

## 3. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         OMS CLIENT LAYER                                    │
│  Trader Desktop (Electron/Web)    PM Dashboard    Mobile App (read-only)   │
└───────────────────────────────────┬─────────────────────────────────────────┘
                                    │  REST + WebSocket
┌───────────────────────────────────▼─────────────────────────────────────────┐
│                        API GATEWAY (Internal)                               │
│  mTLS authentication, fund-level authorization, rate limiting               │
└──────────┬──────────────────┬──────────────────────┬────────────────────────┘
           │                  │                      │
┌──────────▼──────┐  ┌────────▼──────────┐  ┌───────▼───────────────────────┐
│  Order Service  │  │  Position Service │  │  Reporting Service            │
│  State machine  │  │  Real-time P&L    │  │  TCA, compliance, audit       │
│  CQRS pattern   │  │  per account/fund │  │  ClickHouse queries           │
│  PostgreSQL     │  │  PostgreSQL +     │  │                               │
│  + Event store  │  │  Redis cache      │  │                               │
└─────────┬───────┘  └───────────────────┘  └───────────────────────────────┘
          │
┌─────────▼─────────────────────────────────────────────────────────────────┐
│                        ORDER WORKFLOW ENGINE                               │
│                                                                           │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐   │
│  │  Compliance      │  │  SOR (Smart      │  │  Algo Engine         │   │
│  │  Engine          │  │  Order Router)   │  │  TWAP / VWAP / POV   │   │
│  │  - Guidelines    │  │  - Venue ranking │  │  - Schedule slices   │   │
│  │  - Restrictions  │  │  - Order split   │  │  - Market impact     │   │
│  │  - Reg checks    │  │  - Best price    │  │  - Adaptive logic    │   │
│  └────────┬─────────┘  └────────┬─────────┘  └──────────┬───────────┘   │
└───────────┼────────────────────┼──────────────────────────┼──────────────┘
            │                   │                          │
            └───────────────────┴──────────────────────────┘
                                │  Child orders (FIX messages)
┌───────────────────────────────▼────────────────────────────────────────────┐
│                         FIX ENGINE LAYER                                   │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────────┐ │
│  │  Broker A        │  │  Broker B        │  │  Exchange Direct (DMA)   │ │
│  │  FIX Session     │  │  FIX Session     │  │  FIX Session             │ │
│  │  Goldman Sachs   │  │  Morgan Stanley  │  │  NYSE, NASDAQ, IEX       │ │
│  └──────────────────┘  └──────────────────┘  └──────────────────────────┘ │
└────────────────────────────────────────────────────────────────────────────┘
          │  Execution Reports (fills)
┌─────────▼──────────────────────────────────────────────────────────────────┐
│                        FILL PROCESSING PIPELINE                            │
│  Fill Event → Position Update → Allocation → Confirm/Affirm → Kafka       │
│  (Kafka for downstream: accounting, risk, reporting, prime broker)         │
└────────────────────────────────────────────────────────────────────────────┘
          │
┌─────────▼──────────────────────────────────────────────────────────────────┐
│                            DATA LAYER                                      │
│  PostgreSQL (orders, positions, allocations — ACID)                        │
│  Redis (position cache, order status cache, FIX session state)             │
│  Kafka (order events, fill events, position change events)                 │
│  ClickHouse (TCA, execution quality analytics, historical P&L)             │
│  S3 (FIX message archive, audit logs — WORM)                               │
└────────────────────────────────────────────────────────────────────────────┘
```

**Primary use-case data flow (block order → SOR split → fill → allocation):**

1. PM creates block order: "Buy 100,000 shares AAPL for funds A, B, C (50K, 30K, 20K)"
2. Order Service: creates parent `ORDER` record (status=PENDING_COMPLIANCE), publishes to compliance check queue
3. Compliance Engine: checks investment guidelines (AAPL not restricted, position < 5% NAV limit, no ESG restriction), approves in 3ms
4. Order Service: updates status=APPROVED, sends to Algo Engine (VWAP strategy selected)
5. Algo Engine: computes VWAP schedule — breaks 100K shares into 40 slices of 2,500 shares over 6.5 hours (proportional to historical volume profile)
6. Every N minutes: Algo Engine sends child order to SOR: "buy 2,500 AAPL, aggressive, limit $180.05"
7. SOR: checks liquidity across NYSE, NASDAQ, IEX, dark pools; routes 1,500 to NYSE (best ask), 700 to NASDAQ, 300 to dark pool
8. FIX Engine: sends 3 `NewOrderSingle` messages via respective FIX sessions
9. Fills arrive: 1,500 @ $180.00 (NYSE), 700 @ $179.98 (NASDAQ), 300 @ $179.95 (dark pool); avg fill = $179.99
10. Fill Processor: updates child order status, updates parent order filled_qty, triggers position update
11. Position Service: updates position for 100K parent order; updates intraday P&L; refreshes Redis cache
12. After all slices complete (end of day): Allocation Service allocates 50K shares to Fund A, 30K to Fund B, 20K to Fund C at same VWAP price ($179.XX computed across all fills)
13. Confirm/Affirm: generates FIX `Allocation` and `AllocationAck` messages to prime broker
14. All events published to Kafka → accounting system (fund NAV update), risk system, TCA database

---

## 4. Data Model

### Order State Machine
```
State transitions for a parent order:
  DRAFT → PENDING_COMPLIANCE → APPROVED → WORKING → PARTIAL_FILL → FILLED
                                        ↘ REJECTED (compliance fail)
                                        ↘ CANCELLED (trader cancel)
  WORKING → CANCELLED (all child orders cancelled)
  WORKING → PARTIAL_FILL → CANCELLED (partial cancel)

State transitions for a child order (same as above, plus):
  WORKING → PENDING_CANCEL → CANCELLED (cancel request sent, awaiting exchange ACK)
  WORKING → PENDING_REPLACE → WORKING (cancel-replace in flight)
```

### PostgreSQL Schema
```sql
CREATE TABLE orders (
    order_id        UUID            PRIMARY KEY,
    parent_order_id UUID            REFERENCES orders(order_id),  -- null for parent
    fund_id         VARCHAR(50)     NOT NULL,
    account_id      VARCHAR(50)     NOT NULL,
    portfolio_id    VARCHAR(50)     NOT NULL,
    symbol          VARCHAR(20)     NOT NULL,
    side            VARCHAR(5)      NOT NULL,           -- BUY | SELL | SELL_SHORT
    order_type      VARCHAR(12)     NOT NULL,
    qty             BIGINT          NOT NULL,
    filled_qty      BIGINT          NOT NULL DEFAULT 0,
    avg_fill_price  DECIMAL(18,6),
    limit_price     DECIMAL(18,6),
    stop_price      DECIMAL(18,6),
    time_in_force   VARCHAR(5)      NOT NULL DEFAULT 'DAY',
    status          VARCHAR(30)     NOT NULL,
    algo_strategy   VARCHAR(20),                        -- TWAP | VWAP | POV | IS | null
    execution_venue VARCHAR(30),                        -- for child orders
    broker_order_id VARCHAR(64),                        -- exchange/broker-assigned ID
    compliance_result VARCHAR(10),                      -- APPROVED | REJECTED | NA
    compliance_notes TEXT,
    version         BIGINT          NOT NULL DEFAULT 0, -- optimistic locking
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT chk_side CHECK (side IN ('BUY', 'SELL', 'SELL_SHORT')),
    CONSTRAINT chk_filled CHECK (filled_qty <= qty)
);

CREATE INDEX idx_orders_fund_status ON orders(fund_id, status, created_at DESC);
CREATE INDEX idx_orders_parent ON orders(parent_order_id) WHERE parent_order_id IS NOT NULL;
CREATE INDEX idx_orders_symbol_fund ON orders(symbol, fund_id, status);

CREATE TABLE fills (
    fill_id         UUID            PRIMARY KEY,
    order_id        UUID            NOT NULL REFERENCES orders(order_id),
    parent_order_id UUID            NOT NULL REFERENCES orders(order_id),
    symbol          VARCHAR(20)     NOT NULL,
    qty             BIGINT          NOT NULL,
    price           DECIMAL(18,6)   NOT NULL,
    side            VARCHAR(5)      NOT NULL,
    execution_venue VARCHAR(30)     NOT NULL,
    exchange_trade_id VARCHAR(64),                     -- exchange-assigned trade ID
    execution_ts    TIMESTAMPTZ     NOT NULL,           -- when fill occurred at exchange
    received_ts     TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TABLE positions (
    position_id     UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id      VARCHAR(50)     NOT NULL,
    fund_id         VARCHAR(50)     NOT NULL,
    symbol          VARCHAR(20)     NOT NULL,
    qty             BIGINT          NOT NULL DEFAULT 0, -- net position (can be negative for short)
    avg_cost        DECIMAL(18,6)   NOT NULL DEFAULT 0,
    realized_pnl    DECIMAL(18,6)   NOT NULL DEFAULT 0,
    last_price      DECIMAL(18,6),                     -- from market data feed; for unrealized P&L
    last_updated    TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    version         BIGINT          NOT NULL DEFAULT 0,
    UNIQUE (account_id, symbol)
);

CREATE TABLE allocations (
    allocation_id   UUID            PRIMARY KEY,
    parent_order_id UUID            NOT NULL REFERENCES orders(order_id),
    fill_id         UUID            REFERENCES fills(fill_id),
    account_id      VARCHAR(50)     NOT NULL,
    fund_id         VARCHAR(50)     NOT NULL,
    symbol          VARCHAR(20)     NOT NULL,
    allocated_qty   BIGINT          NOT NULL,
    allocated_price DECIMAL(18,6)   NOT NULL,           -- same for all accounts (block price)
    allocation_method VARCHAR(20)   NOT NULL,           -- PRO_RATA | SEQUENTIAL | MANUAL
    status          VARCHAR(20)     NOT NULL,           -- PENDING | CONFIRMED | AFFIRMED | FAILED
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);
```

### Event Store (CQRS — append-only, for audit and replay)
```sql
-- Append-only event log; never updated or deleted; source of truth for order state
CREATE TABLE order_events (
    event_id        UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id        UUID            NOT NULL,
    event_type      VARCHAR(30)     NOT NULL,   -- ORDER_CREATED | COMPLIANCE_APPROVED | SUBMITTED
                                                 -- FILL_RECEIVED | CANCELLED | ALLOCATED | etc.
    event_ts        TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    payload         JSONB           NOT NULL,    -- full event data (schema varies by event_type)
    actor_id        VARCHAR(100)    NOT NULL,    -- user ID or system component that caused event
    sequence        BIGINT          NOT NULL,    -- monotonic per order_id (for replay ordering)
    CONSTRAINT chk_sequence CHECK (sequence > 0)
);

CREATE INDEX idx_order_events_order ON order_events(order_id, sequence);
CREATE INDEX idx_order_events_ts ON order_events(event_ts);
-- Stored on S3 (WORM) after 24h; PostgreSQL holds last 24h for fast replay
```

---

## 5. API Design

### REST API (Trader / PM Interface)
```
POST /api/v1/orders
Description: Create a new parent order
Auth: mTLS + JWT (fund scoped)
Request:
  {
    "fund_id":       string,
    "account_ids":   string[],           // for block orders across multiple accounts
    "symbol":        string,
    "side":          "buy" | "sell" | "sell_short",
    "qty":           integer,
    "order_type":    "market" | "limit" | "stop" | "stop_limit",
    "limit_price":   number (opt),
    "algo_strategy": "twap" | "vwap" | "pov" | "is" | null,
    "algo_params":   {
      "start_time":  ISO8601 (opt),
      "end_time":    ISO8601 (opt),
      "participation_rate": 0.15  // for POV: 15% of market volume
    },
    "time_in_force": "day" | "gtc",
    "notes":         string (opt)        // PM instructions to trader
  }
Response 202 Accepted:
  { "order_id": "uuid", "status": "pending_compliance", "created_at": "..." }

PATCH /api/v1/orders/{order_id}
Description: Modify an open order (cancel-replace)
  { "qty": integer (opt), "limit_price": number (opt) }
Response 200 OK: updated order object

DELETE /api/v1/orders/{order_id}
Description: Cancel an order
Response 200 OK: { "status": "cancel_pending" }

GET /api/v1/orders?fund=F001&status=working&page=1&page_size=50
Response 200: paginated order list

GET /api/v1/positions?fund=F001&as_of=2024-01-15T16:00:00Z
Response 200:
  {
    "fund_id": "F001",
    "as_of": "...",
    "total_market_value": 125000000,
    "total_unrealized_pnl": 3200000,
    "positions": [
      { "symbol": "AAPL", "qty": 50000, "avg_cost": 175.00, "last_price": 180.00,
        "market_value": 9000000, "unrealized_pnl": 250000, "unrealized_pnl_pct": 2.86 }
    ]
  }

GET /api/v1/orders/{order_id}/fills
Response 200: list of fills for this order with venue, price, qty, execution_ts

POST /api/v1/orders/{order_id}/allocate
Description: Manually trigger or adjust allocation for a filled block order
  { "allocations": [{ "account_id": "ACC001", "qty": 50000 }, ...] }
Response 200 OK: allocation confirmation

WebSocket: wss://oms.internal/stream
  Subscribe to fill events, order status updates, and position updates in real time
```

---

## 6. Deep Dive: Core Components

### Component: Smart Order Router (SOR)

**Problem it solves:** A large child order (e.g., buy 10,000 AAPL) sent entirely to one exchange will move the market against the buyer — the visible size signals demand, prices rise, and the large order pays more. The SOR splits and routes the order across multiple venues to minimize market impact and achieve the best blended execution price.

**Routing Logic:**

```
SOR inputs:
  symbol:       AAPL
  side:         BUY
  qty:          10,000 shares
  urgency:      MEDIUM (0.0–1.0 scale)
  limit_price:  $180.10

Step 1: Get venue snapshots (from Market Data Feed, cached in Redis, < 5ms stale)
  NYSE:     ask $179.97 × 2,000 shares available
  NASDAQ:   ask $179.98 × 1,500 shares available
  IEX:      ask $180.00 × 800 shares available
  Dark1:    est. ask $179.95 × 3,000 shares (probabilistic, dark pool IOI data)
  Dark2:    est. ask $179.96 × 2,500 shares

Step 2: Rank venues by effective cost (ask price + estimated market impact)
  Dark pools: lower spread, but no execution guarantee (IOC orders, fill or cancel)
  Lit venues: guaranteed liquidity at advertised price
  
Step 3: Slice into sub-orders:
  Sub-order 1: Send IOC 3,000 to Dark1 (best estimated price if dark pool fills)
  Sub-order 2: Send 2,000 to NYSE (known liquidity, best lit ask)
  Sub-order 3: Send 1,500 to NASDAQ
  Sub-order 4: Send 800 to IEX
  Reserve 2,700 shares for second pass (if dark pool doesn't fill or lit venues not enough)
  
Step 4: Collect results (within 30ms):
  Dark1: filled 1,800 (partial, not all 3,000 were available) at $179.95
  NYSE: filled 2,000 at $179.97
  NASDAQ: filled 1,500 at $179.98
  IEX: filled 800 at $180.00
  Filled so far: 6,100 / 10,000. Remaining: 3,900.
  
Step 5: Re-route remainder:
  Send 2,500 to Dark2 (IOC)
  Send 1,400 to NYSE (check if more liquidity refreshed)
  
Step 6: Aggregate: total filled 10,000, blended avg price = weighted avg of all fills

SOR decision factors:
  - Venue reliability score (historical fill rate × fill ratio)
  - Spread + fee (some venues charge maker/taker fees)
  - Market impact model (how much price moves per 1,000 shares on this venue for this symbol)
  - Regulatory best execution: all routing decisions logged with rationale
```

**Interviewer Deep-Dive Q&A:**

Q: How do you prevent information leakage when routing to dark pools?
A: Several techniques: (1) **IOC-only orders to dark pools**: send Immediate-Or-Cancel, so the order either fills immediately or is cancelled — it never sits in the dark pool book long enough to be detected. (2) **Randomized timing**: stagger sub-order submissions by a few milliseconds (random jitter) so HFT firms cannot detect a pattern that reveals the parent order size. (3) **Minimum execution size**: for very large orders, set a minimum fill size in the dark pool IOC to avoid partial fills that reveal the total size. (4) **Venue rotation**: vary which dark pools are used across slices. (5) **Confidentiality agreements**: dark pools are contractually prohibited from signaling order presence, but technical measures add defense in depth.

Q: How do you handle a SOR sub-order that partially fills and the venue rejects the remainder?
A: The SOR tracks each sub-order's filled_qty vs sent_qty. On a partial fill + cancellation of remainder: (1) update child order filled_qty; (2) compute remaining unfilled qty; (3) re-enter the routing loop for the unfilled remainder with updated venue snapshots (prices may have moved); (4) apply urgency factor — if the parent order needs to complete quickly, increase aggressiveness (relax price limit, prioritize lit venues over dark pools). The parent order tracks cumulative filled_qty across all child orders and all re-routes.

---

### Component: VWAP Execution Algorithm

**Problem it solves:** A 100,000-share order executed all at once would move the market significantly (market impact). VWAP strategy distributes executions over the trading day proportional to the historical volume profile of the stock, so the average execution price approximates the day's VWAP — a standard benchmark for measuring execution quality.

**Implementation:**

```
VWAP Schedule computation (runs at order creation):

Input: 100,000 shares AAPL, full day VWAP (9:30 AM – 4:00 PM)

1. Load historical volume profile for AAPL:
   Based on last 20 trading days, average volume % per 30-minute bucket:
   09:30–10:00: 18% of daily volume (high: market open)
   10:00–10:30: 8%
   10:30–11:00: 5%
   ...
   15:30–16:00: 12% (high: market close)

2. Compute target qty per bucket:
   09:30–10:00: 18% × 100,000 = 18,000 shares
   Divided into 1-minute slices: 18,000 / 30 = 600 shares/minute

3. Generate schedule: list of (time, qty) pairs, one per minute over 6.5 hours

4. Each minute: dispatch child order to SOR with qty from schedule
   - Adjust qty based on real-time participation rate:
     if market volume in last minute was higher than expected → send more shares (catch up)
     if market volume was lower → send fewer shares (pace down)
   
5. Track VWAP progress:
   Our cumulative fills VWAP vs market VWAP (from market data feed)
   If our VWAP > market VWAP (paying more than market average): reduce aggression
   If our VWAP < market VWAP: acceptable

6. End of day: cancel any remaining unfilled shares if TOD (Time of Day) order type
   Or carry to next day if GTC (Good 'Til Cancelled)
```

---

### Component: Block Allocation

**Problem it solves:** When a PM places a block order for multiple accounts (e.g., "buy 100,000 AAPL across funds A, B, C"), the fills must be fairly allocated across accounts at the same average price. Allocation must be auditable — regulators can ask why specific accounts got specific allocations.

**Approach:**

```
Pre-trade allocation (ideal — allocation decided before order sent):
  Fund A: 50% = 50,000 shares
  Fund B: 30% = 30,000 shares
  Fund C: 20% = 20,000 shares
  Rationale: proportional to each fund's stated investment policy for this security

Post-trade allocation (for partial fills):
  If only 80,000 of 100,000 shares filled:
  Option A: Pro-rata — all accounts get 80%: A=40K, B=24K, C=16K
  Option B: Sequential — fill A first (50K), then B (24K partial), C=0
             → Unfair: different accounts get different prices over time
  
  SEC guidance (and best practice): Pro-rata allocation for fairness.
  Each account gets fills at the same blended average price for the block.

Audit requirement:
  Allocation decision recorded BEFORE order submitted (pre-allocation on file)
  If post-trade allocation differs from pre-allocation → compliance exception raised
  All allocations stored in order_events with actor_id, timestamp, and rationale
  
Price averaging:
  All fills for the block order are averaged: if filled 50K @ $180.00 and 50K @ $179.95
  Average price = ($180.00 × 50K + $179.95 × 50K) / 100K = $179.975
  All accounts receive allocation at $179.975 regardless of when their portion was filled
  Broker confirms at block price; prime broker does internal allocations at block price
```

---

## 7. Failure Scenarios & Resilience

### Scenario 1: FIX Session Disconnect During Active Orders
- **Impact**: Child orders in WORKING state; not sure if broker received them.
- **Recovery**: FIX session reconnects and sends `ResendRequest` for all messages after last confirmed sequence number. Broker retransmits fill status for all open orders. OMS reconciles broker state vs local state: any discrepancy triggers "trade break" alert for manual ops review. All open orders must be confirmed as OPEN, FILLED, or CANCELLED before new orders are accepted on that session.

### Scenario 2: Duplicate Fill Message
- **Detection**: Each fill has a unique `exchange_trade_id` (exchange-assigned). On fill receipt, OMS checks `SELECT 1 FROM fills WHERE exchange_trade_id = ?`. If exists → discard duplicate. This check is also at Kafka consumer level — exactly-once semantic via Kafka transactions + unique constraint on fills table.
- **Why it happens**: FIX session reconnects and broker resends fills for the missed window; OMS must handle at-least-once delivery from broker.

### Scenario 3: Compliance System Unavailable
- **Policy**: Fail-closed — no orders submitted to exchanges if compliance check cannot complete. Orders queue in PENDING_COMPLIANCE status. Traders receive "compliance check unavailable" notification.
- **Recovery**: Compliance system has hot standby with < 30s failover. Queued orders processed in order once compliance resumes. If outage > 5 minutes: escalate to compliance officer who can manually approve critical orders via emergency override (with full audit trail).

### Scenario 4: Position Calculation Mismatch with Prime Broker
- **Daily reconciliation**: At end of day, OMS generates position file and compares with prime broker's record.
- **Break handling**: Any discrepancy ("break") is flagged in a "breaks" queue; middle office investigates. Common causes: missing fills, allocation errors, corporate actions (dividends, splits not applied). Each break has a deadline for resolution (T+1 for most).

---

## 8. Monitoring & Observability

| Metric | Alert Threshold | Purpose |
|---|---|---|
| Order fill rate | < 60% for MARKET orders | SOR or venue issue |
| Compliance check latency P99 | > 50 ms | Compliance service health |
| FIX session heartbeat | Missed > 1 | Broker connectivity |
| Position reconciliation breaks | > 0 breaks at EOD | Data integrity |
| Kafka fill event lag | > 500 messages | Processing backlog |
| Average slippage vs VWAP benchmark | > 5 bps | Execution quality |
| Allocation completion rate | < 99.9% | Ops error rate |
| Duplicate fill detection rate | Any spike | Feed reliability |
| Open order count (stale) | Orders open > 1 day with no activity | Zombie order detection |
| TCA report generation time | > 30s | ClickHouse performance |
