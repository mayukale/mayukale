# System Design: Retail Trading Platform (Robinhood / E*TRADE-style)

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Account management**: User registration, KYC (Know Your Customer) identity verification, account funding via ACH/wire, and account types (individual, IRA, joint).
2. **Order placement**: Place market, limit, stop, and stop-limit orders for equities and ETFs; support fractional shares.
3. **Order management**: View open orders, cancel pending orders, view order history.
4. **Portfolio view**: Real-time portfolio value, positions, unrealized P&L, cost basis.
5. **Real-time quotes**: Live streaming prices for watchlisted and held securities.
6. **Trade execution notifications**: Push notifications and in-app alerts for fills, cancels, and rejections.
7. **Options trading**: View options chains; place calls and puts (higher-tier users).
8. **Fractional shares**: Buy and sell dollar amounts instead of whole shares (e.g., buy $10 of AAPL).
9. **Recurring investments**: Schedule automatic periodic buys (daily/weekly/monthly).
10. **Market data**: Charts (1D, 1W, 1M, 3M, 1Y, 5Y), company info, news, analyst ratings.
11. **Statements and tax documents**: Monthly statements, annual 1099-B tax forms (for US retail).
12. **Margin trading** (premium tier): Borrowing against portfolio to increase buying power.

### Non-Functional Requirements
1. **Availability**: 99.99% overall; 99.999% during market hours for order entry and portfolio view.
2. **Latency**: Order submission to exchange acknowledgment P99 < 500 ms (network + internal); portfolio value refresh < 1s.
3. **Throughput**: 5M active users; peak 100K simultaneous order submissions during market open.
4. **Consistency**: Account balance and position must be strongly consistent — no overselling, no negative cash balances.
5. **Scalability**: Handle 10x normal volume on volatile market days (meme stocks, earnings events).
6. **Security**: FINRA/SEC-registered broker-dealer; SIPC-insured accounts; SOC 2 Type II; PCI DSS for payment processing.
7. **Compliance**: FINRA Rule 4370 (business continuity), SEC Rule 15c3-3 (customer protection), pattern day trader (PDT) rule enforcement.
8. **Data durability**: All financial transaction records retained 7 years (FINRA Rule 4511).

### Out of Scope
- Institutional brokerage (FIX connectivity, DMA)
- Proprietary trading / market making
- Cryptocurrency trading infrastructure (separate system)
- Banking product (savings accounts, debit cards — separate)
- Research / advisory services
- International markets (assume US equities only for scope)

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Retail Investor | Primary user; mobile-first; buys/sells stocks; checks portfolio daily |
| Active Trader | Higher frequency; uses limit orders and options; intraday trading |
| Long-term Investor | Buys and holds; uses recurring investments; checks less frequently |
| Options Trader | Level 2+ approval; trades calls and puts; needs real-time Greeks |

### Traffic Estimates

**Assumptions:**
- 25M registered accounts; 5M DAU
- Average user checks portfolio 3x/day, places 0.1 orders/day
- Market hours: 9:30 AM – 4:00 PM ET; 10x peak at open
- Pre-market and after-hours trading: 10% of market-hours volume

| Metric | Calculation | Result |
|---|---|---|
| DAU | given | 5M |
| Portfolio view requests/day | 5M DAU × 3 views/day | 15M requests/day |
| Portfolio view QPS (average) | 15M / 86,400s | ~174 QPS |
| Portfolio view QPS (peak, market open) | 174 × 10 | ~1,740 QPS |
| Orders placed/day | 5M DAU × 0.1 orders/day | 500K orders/day |
| Order entry QPS (average) | 500K / 23,400s | ~21 QPS |
| Order entry QPS (peak) | 21 × 10 | ~210 QPS (design target: 100K concurrent) |
| Price quote WebSocket connections | 5M DAU × 20% on watchlist during market hours | ~1M concurrent WS connections |
| Price updates pushed/sec | 1M WS connections × 10 symbols avg × 2 updates/symbol/sec | ~20M push deliveries/sec (fan-out) |
| Storage per user per year | Portfolio snapshots + order history + statements | ~1 MB/user/year |
| Total storage (25M users, 5 years) | 25M × 1 MB × 5 | ~125 TB |

### Latency Requirements
| Operation | P50 | P99 | Notes |
|---|---|---|---|
| Order submission to exchange ACK | < 100 ms | < 500 ms | Includes routing to broker-dealer backend + exchange round-trip |
| Fill notification to user | < 1 s | < 3 s | Push notification via APNs/FCM |
| Portfolio value refresh | < 500 ms | < 1 s | On price change |
| Quote WebSocket update | < 200 ms | < 1 s | Market data latency not as critical as exchanges |
| Account balance read | < 50 ms | < 200 ms | Redis cache |
| Order history query | < 200 ms | < 500 ms | Last 90 days from cache |

---

## 3. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                            CLIENT LAYER                                         │
│   iOS App        Android App        Web App (React)        API (3rd party)      │
└──────────┬──────────────┬──────────────────┬──────────────────┬─────────────────┘
           │              │                  │                  │
           └──────────────┴──────────────────┴──────────────────┘
                                             │  HTTPS + WebSocket
┌────────────────────────────────────────────▼────────────────────────────────────┐
│                         API GATEWAY / CDN                                       │
│  Cloudflare / AWS ALB — TLS termination, rate limiting, DDoS protection         │
│  REST: /api/v1/*  →  Backend Services                                           │
│  WebSocket: /stream/*  →  Market Data Streaming Service                         │
└────────┬────────────────────┬─────────────────────┬────────────────────────────┘
         │                    │                     │
┌────────▼────────┐  ┌────────▼─────────┐  ┌───────▼────────────────────────────┐
│  Auth Service   │  │  Account Service │  │  Market Data Streaming Service      │
│  (JWT + OAuth2) │  │  (KYC, balances, │  │  - Consumes from exchange data feed │
│  Stateless      │  │   positions)     │  │  - Fan-outs to 1M+ WebSocket conns  │
│  Redis sessions │  │  PostgreSQL +    │  │  - Pub/Sub via Redis                │
│                 │  │  Redis cache     │  │  - Symbol subscription management   │
└─────────────────┘  └────────┬─────────┘  └────────────────────────────────────┘
                              │
┌──────────────────────────── ▼────────────────────────────────────────────────  ┐
│                          ORDER FLOW                                             │
│                                                                                 │
│  ┌──────────────────┐   ┌────────────────────┐   ┌──────────────────────────┐  │
│  │  Order Service   │──►│  Risk Engine       │──►│  Order Router            │  │
│  │  - Validates     │   │  - Buying power    │   │  - Routes to exchange    │  │
│  │  - Idempotency   │   │  - PDT rule check  │   │  - PFOF routing (market  │  │
│  │  - Saves to DB   │   │  - Position limits │   │    maker for retail)     │  │
│  │  - Publishes to  │   │  - Fractional      │   │  - FIX / REST to broker  │  │
│  │    Kafka         │   │    aggregation     │   │    backend / exchange    │  │
│  └──────────────────┘   └────────────────────┘   └──────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────┘
         │
┌────────▼──────────────────────────────────────────────────────────────────────┐
│                         DATA LAYER                                             │
│  PostgreSQL (accounts, orders, positions)    Redis (balance cache, sessions)  │
│  Kafka (order events, fill events)           S3 (statements, tax docs)        │
│  ClickHouse (portfolio analytics, P&L)       ElasticSearch (order history)    │
└────────────────────────────────────────────────────────────────────────────────┘
         │
┌────────▼──────────────────────────────────────────────────────────────────────┐
│                    DOWNSTREAM / EXTERNAL INTEGRATIONS                          │
│  Exchange / Market Maker (order routing)     DTCC/DTC (clearing + settlement) │
│  ACH/Wire (funding)                          Apex Clearing / DriveWealth       │
│  Apex / Pershing (custodian)                 IEX, NYSE, NASDAQ (market data)  │
│  APNs / FCM (push notifications)            Plaid (bank account linking)      │
└────────────────────────────────────────────────────────────────────────────────┘
```

**Primary use-case data flow (market order → fill notification):**

1. User taps "Buy 10 AAPL — Market" in iOS app
2. App sends `POST /api/v1/orders` with JWT
3. API Gateway authenticates JWT, rate-limits (100 orders/day free tier), routes to Order Service
4. Order Service: validates params, checks idempotency key (UUID in header), saves `ORDER` record to PostgreSQL with status=PENDING, publishes to Kafka `order-events`
5. Risk Engine: checks buying power (10 × ~$180 = $1,800 against available cash in Redis cache), checks PDT rules (< 4 day trades in 5 days for accounts < $25K), approves
6. Order Router: selects execution venue (PFOF: route to Citadel Securities or Virtu for retail market order), sends FIX `NewOrderSingle` to market maker
7. Market maker fills immediately (retail market orders get price improvement at market makers in PFOF model)
8. Fill notification arrives via FIX `ExecutionReport` → Order Service updates status=FILLED in PostgreSQL
9. Kafka `fill-events` consumed by: Position Service (updates portfolio), Notification Service (push notification to user), Analytics Service
10. Position Service: updates `positions` table and refreshes Redis balance cache
11. APNs/FCM push: "Order Filled: 10 AAPL at $179.97 — better than market!" delivered to user's iPhone in < 1s

---

## 4. Data Model

### Account and Balance (PostgreSQL — ACID required)
```sql
CREATE TABLE accounts (
    account_id      UUID            PRIMARY KEY,
    user_id         UUID            NOT NULL REFERENCES users(id),
    account_type    VARCHAR(20)     NOT NULL,   -- INDIVIDUAL | IRA | JOINT | MARGIN
    status          VARCHAR(20)     NOT NULL,   -- ACTIVE | RESTRICTED | CLOSED
    margin_enabled  BOOLEAN         NOT NULL DEFAULT FALSE,
    options_level   INTEGER         NOT NULL DEFAULT 0,  -- 0-4 (approval tier)
    pdt_counter     INTEGER         NOT NULL DEFAULT 0,  -- pattern day trade count (rolling 5 days)
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE TABLE account_balances (
    account_id      UUID            PRIMARY KEY REFERENCES accounts(account_id),
    cash_available  DECIMAL(20,6)   NOT NULL DEFAULT 0,   -- available for trading now
    cash_pending    DECIMAL(20,6)   NOT NULL DEFAULT 0,   -- ACH in-flight, not yet settled
    cash_settled    DECIMAL(20,6)   NOT NULL DEFAULT 0,   -- T+1 settled cash
    margin_buying_power DECIMAL(20,6) NOT NULL DEFAULT 0, -- for margin accounts only
    version         BIGINT          NOT NULL DEFAULT 0    -- optimistic locking
    -- CHECK (cash_available >= 0)
    -- CHECK (cash_settled >= 0)
);

-- Optimistic locking pattern (same as payments pattern):
-- UPDATE account_balances SET cash_available = cash_available - :amount, version = version + 1
-- WHERE account_id = :id AND cash_available >= :amount AND version = :expected_version
-- 0 rows affected → insufficient funds or concurrent modification → retry

CREATE TABLE positions (
    account_id      UUID            NOT NULL REFERENCES accounts(account_id),
    symbol          VARCHAR(20)     NOT NULL,
    qty             DECIMAL(20,8)   NOT NULL DEFAULT 0,   -- supports fractional shares
    avg_cost_basis  DECIMAL(18,6)   NOT NULL DEFAULT 0,
    realized_pnl    DECIMAL(18,6)   NOT NULL DEFAULT 0,
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (account_id, symbol)
    -- CHECK (qty >= 0)
);
```

### Orders (PostgreSQL)
```sql
CREATE TABLE orders (
    order_id        UUID            PRIMARY KEY,
    client_order_id VARCHAR(64)     NOT NULL,           -- client's idempotency key
    account_id      UUID            NOT NULL REFERENCES accounts(account_id),
    symbol          VARCHAR(20)     NOT NULL,
    side            VARCHAR(5)      NOT NULL,            -- BUY | SELL
    order_type      VARCHAR(12)     NOT NULL,            -- MARKET | LIMIT | STOP | STOP_LIMIT
    qty             DECIMAL(20,8)   NOT NULL,            -- supports fractional
    dollar_amount   DECIMAL(18,2),                       -- for dollar-based fractional orders
    limit_price     DECIMAL(18,6),
    stop_price      DECIMAL(18,6),
    time_in_force   VARCHAR(5)      NOT NULL DEFAULT 'DAY',
    status          VARCHAR(20)     NOT NULL,            -- PENDING | OPEN | PARTIAL | FILLED | CANCELLED | REJECTED
    filled_qty      DECIMAL(20,8)   NOT NULL DEFAULT 0,
    avg_fill_price  DECIMAL(18,6),
    exchange_order_id VARCHAR(64),                       -- exchange-assigned order ID
    execution_venue VARCHAR(20),                         -- CITADEL | VIRTU | IEX | NASDAQ | NYSE
    submitted_at    TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    filled_at       TIMESTAMPTZ,
    cancelled_at    TIMESTAMPTZ,
    idempotency_key VARCHAR(64)     NOT NULL UNIQUE      -- prevents double-submission
);

CREATE INDEX idx_orders_account_status ON orders(account_id, status, submitted_at DESC);
CREATE INDEX idx_orders_symbol_account ON orders(symbol, account_id);
```

### Redis Cache Structure
```
balance:<account_id>           → Hash { cash_available, margin_buying_power }   TTL=30s
position:<account_id>          → Hash { AAPL: "10.5", MSFT: "5.0", ... }        TTL=60s
session:<token>                → Hash { user_id, account_id, exp }              TTL=15m
idempotency:<client_order_id>  → String (order_id)                              TTL=24h
quote:<symbol>                 → Hash { price, bid, ask, ts }                   TTL=5s
pdt:<account_id>               → Integer (day trade count, rolling 5 days)      TTL=5 days
```

---

## 5. API Design

### REST API

```
POST /api/v1/orders
Description: Place a new order
Auth: Bearer JWT
Idempotency-Key: <uuid> header (required)
Request:
  {
    "symbol":       string (e.g., "AAPL"),
    "side":         "buy" | "sell",
    "type":         "market" | "limit" | "stop" | "stop_limit",
    "qty":          number (shares, fractional allowed, e.g., 0.5),
    "dollar_amount": number (alternative to qty, for dollar-based orders),
    "limit_price":  number (required for limit / stop_limit),
    "stop_price":   number (required for stop / stop_limit),
    "time_in_force": "day" | "gtc" | "ioc" | "fok" (default: "day")
  }
Response 201 Created:
  { "order_id": "uuid", "status": "pending", "submitted_at": "..." }
Response 400: invalid order params
Response 402: insufficient funds
Response 403: account restricted / options level too low
Response 409: idempotency key already used (returns original order)
Response 429: rate limit exceeded

GET /api/v1/orders?status=open&limit=50&cursor=<token>
Description: List orders with cursor-based pagination
Response 200 OK: { "orders": [...], "next_cursor": "...", "has_more": true }

DELETE /api/v1/orders/{order_id}
Description: Cancel an open order
Response 200: { "status": "cancel_pending" }
Response 409: order already filled or cancelled

GET /api/v1/portfolio
Description: Get real-time portfolio summary
Response 200 OK:
  {
    "total_value":      number (positions at current market price + cash),
    "cash_available":   number,
    "day_return":       number (today's P&L),
    "day_return_pct":   number,
    "total_return":     number,
    "positions": [
      { "symbol": "AAPL", "qty": 10.5, "market_value": 1889.85,
        "avg_cost": 170.00, "unrealized_pnl": 209.85, "unrealized_pnl_pct": 11.9 }
    ]
  }

WebSocket: wss://api.platform.com/stream
Subscribe: { "type": "subscribe", "channels": ["quotes", "orders"], "symbols": ["AAPL", "MSFT"] }

Quote update:
  { "type": "quote", "symbol": "AAPL", "price": "179.97", "change": "+1.23",
    "change_pct": "0.69", "ts": "2024-01-15T14:30:00.123Z" }

Order update:
  { "type": "order_update", "order_id": "uuid", "status": "filled",
    "filled_qty": 10, "avg_fill_price": "179.97", "ts": "..." }
```

---

## 6. Deep Dive: Core Components

### Component: Fractional Share Aggregation

**Problem it solves:** Retail users want to invest dollar amounts (e.g., "buy $50 of AAPL"). Exchanges don't trade fractional shares — minimum is 1 whole share. The broker must aggregate many fractional orders into whole-share lots for exchange execution, then allocate fills back to individual users proportionally.

**Approach:**

```
User A: Buy $50 of AAPL  → at $180/share = 0.2778 shares
User B: Buy $100 of AAPL → 0.5556 shares
User C: Buy $75 of AAPL  → 0.4167 shares
Total:  $225 = 1.25 shares

Aggregator:
1. Collect fractional orders in a 100ms window (configurable)
2. Round up to whole shares: ceil(1.25) = 2 whole shares
3. Submit 2-share MARKET order to exchange (company's proprietary account)
4. Receive fill: 2 AAPL at $179.97

Allocation back to users (pro-rata):
  User A gets: 0.2778/1.25 × 2 = 0.4444 shares at $179.97 → cost = $79.98 (broker rounds)
  Wait — dollar amounts: User A gets $50/$225 × 2 × $179.97 = 0.4444 × $179.97 = $79.98?
  
  Correct pro-rata by dollar:
  User A: ($50/$225) × 2 shares filled = 0.4444 shares
  User B: ($100/$225) × 2 = 0.8889 shares  
  User C: ($75/$225) × 2 = 0.6667 shares
  Total allocated: 2.0000 ✓
  
  All fractional allocations at same fill price ($179.97) — no price variation between users.
  Rounding residuals go to the broker's house account (disclosed in terms of service).

5. Update positions for each user atomically (batch DB update in single transaction)
6. Post-allocation fills trigger individual order status updates and push notifications
```

**Key regulatory consideration:** FINRA Rule 4314 — the broker must disclose the fractional share program and how rounding/residuals are handled. The broker acts as a riskless principal in many fractional share programs (buying the whole share and immediately allocating), not an agent.

**Interviewer Deep-Dive Q&A:**

Q: How do you prevent a user from overselling fractional shares they don't have?
A: Optimistic locking on the `positions` table prevents concurrent race conditions. The SELL flow: (1) Read current `qty` from positions; (2) Validate `qty >= requested_sell_qty`; (3) Submit order; (4) On fill, `UPDATE positions SET qty = qty - fill_qty, version = version + 1 WHERE account_id = X AND symbol = Y AND qty >= fill_qty AND version = expected_version`. If the update affects 0 rows, either another concurrent fill reduced qty below zero (race) or qty changed — reject and retry. Additionally, at order placement time, available qty is reserved (not yet decremented, but the reservation prevents concurrent orders from overselling).

Q: How do you handle the scenario where only half of the aggregated lot fills?
A: If the aggregated order partially fills, the broker allocates fills pro-rata as they arrive. If a 2-share order fills 1 share first: allocate 1 share proportionally to the 3 users (0.2222, 0.4444, 0.3333 shares). When the second share fills, allocate the remaining. If the order is cancelled (e.g., limit order not fully filled by day end), the unfilled fractional portions are cancelled back to users' cash at no cost. This is disclosed in the platform's fractional share disclosures.

---

### Component: Pattern Day Trader (PDT) Rule Enforcement

**Problem it solves:** FINRA Rule 4210 requires that accounts with < $25,000 in equity cannot execute more than 3 day trades (buying and selling the same security on the same day) in a rolling 5-business-day window. Violation restricts the account to 90 days of "close-only" trading.

**Implementation:**

```
Day trade detection logic (runs at fill time):
  A day trade = a position opened and closed on the same trading day
  
  On fill event for a SELL:
    if (bought same symbol today AND qty_sold <= qty_bought_today):
        day_trade_count += 1  // this sell closes a same-day buy = day trade
    else:
        // selling a position bought on a prior day = not a day trade

  PDT check at order placement:
    1. account equity < $25,000? (total portfolio value including unrealized)
    2. day_trade_count (rolling 5 days) >= 3?
    3. Would this order create a 4th day trade?
    → If all 3 true: REJECT order with error "PDT_LIMIT_EXCEEDED"

  PDT counter storage:
    Redis INCR pdt:<account_id> with TTL = end of 5th trading day
    PostgreSQL pdt_events table for audit (day trade records, 7-year retention)
    
  PDT reset:
    Account equity rises above $25,000 → PDT restrictions lifted, counter resets
    Time-based: 5 trading days from oldest counted day trade, counter decrements
```

---

### Component: PFOF (Payment for Order Flow) Routing

**Problem it solves:** Retail market orders can be routed to market makers (Citadel Securities, Virtu) instead of exchanges. Market makers pay the broker per order (PFOF) and in return are required to provide price improvement over the NBBO. The broker must route orders to achieve "best execution" for customers.

**Routing logic:**

```
Order Router decision (market order, equity, < 10,000 shares):
1. Query PFOF rate agreements: which market makers are contracted?
2. Compute expected price improvement: use historical price improvement data per venue
3. Check venue capacity (market maker may be at capacity → reject → route to exchange)
4. Select venue:
   - If PFOF venue offers expected price improvement ≥ NBBO spread/2 → route to PFOF venue
   - Else → route to exchange (IEX, NYSE, NASDAQ) for best price
5. Log routing decision with rationale (required for best execution documentation)

Best execution documentation:
  SEC Rule 606 requires quarterly disclosures of:
  - % orders routed to each venue
  - PFOF received per order
  - Price improvement statistics
  All stored in ClickHouse for reporting; published quarterly on website.
```

---

## 7. Failure Scenarios & Resilience

### Scenario 1: Exchange Connectivity Loss
- **Impact**: Order Router cannot route new orders to exchange/market maker.
- **Detection**: FIX session heartbeat timeout (30s). Health monitor alert in < 10s.
- **Recovery**: Order Router switches to backup FIX connection (each venue has 2 connections). If all connections to a venue fail, failover to next-best venue (based on routing rules). In-flight orders: FIX `ResendRequest` issued on reconnect to confirm order status. User-facing: orders stay in PENDING status; push notification if delayed > 30s ("Your order is taking longer than expected — market connectivity issue").

### Scenario 2: Balance Cache Stale (Redis Eviction)
- **Impact**: Risk Engine reads stale buying power; may allow over-trading.
- **Prevention**: Redis cache TTL = 30s; cache is supplemental, not authoritative. For orders > $10K notional, Risk Engine always reads from PostgreSQL (not Redis cache) as a hard rule. For orders > $50K, requires async double-check.
- **Recovery**: On cache miss, read from PostgreSQL, repopulate cache. Risk Engine is designed to fail-closed: if PostgreSQL is unavailable, reject order with "risk check unavailable" rather than allowing it.

### Scenario 3: Kafka Lag (Fill Events Not Processing)
- **Impact**: Portfolio not updated; notifications not sent.
- **Detection**: Consumer lag metric > 1,000 messages → alert. Lag > 10,000 → page on-call.
- **Recovery**: Add consumer instances (auto-scaling). If consumers are stuck (deserialization error, poison pill), move poison pill to dead-letter topic and continue. Portfolio and balance are eventually consistent with fill events — the PostgreSQL orders table is updated synchronously by the Order Service, so a manual check of order status is always accurate even during lag.

### Scenario 4: Market Closure / Early Close
- **Trigger**: Exchange announces early close (e.g., market holiday, circuit breaker).
- **Handling**: Platform subscribes to exchange status feed (FIX Session-level messages + SIP status). On close signal: all DAY orders cancelled (FIX `OrderCancelRequest` sent for all open DAY orders); GTC orders left open; users notified via push. New order entry blocked with "Market is closed — GTC orders will execute at next open."

---

## 8. Monitoring & Observability

| Metric | Alert Threshold | Purpose |
|---|---|---|
| Order submission to exchange P99 | > 2 seconds | End-to-end order flow health |
| FIX session heartbeat failures | Any | Exchange connectivity |
| Fill → notification delivery latency P99 | > 5 seconds | Push notification pipeline |
| Balance cache hit rate | < 90% | Redis health |
| Kafka fill consumer lag | > 10,000 messages | Processing backlog |
| PDT rule violations blocked | Any spike | Unusual trading activity |
| Fractional aggregation fill rate | < 95% | Aggregation pipeline |
| WebSocket connection count | Sudden drop > 20% | Client connectivity issue |
| Order rejection rate | > 5% | Risk Engine or exchange issue |
| P&L calculation accuracy (spot check) | Any deviation > $0.01 | Correctness alert |
