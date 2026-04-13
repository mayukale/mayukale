# Pattern 9: E-Commerce — Interview Study Guide

Reading Pattern 9: E-Commerce — 3 problems, 9 shared components

---

## STEP 1 — ORIENTATION

This pattern covers three e-commerce problems that share infrastructure but have fundamentally different challenges:

- **Amazon Product Page (PDP)**: Read-heavy, fan-out assembly, caching at massive scale
- **Shopping Cart**: Stateful user session, consistency, guest-to-auth lifecycle
- **Flash Sale**: Write-spiky, oversell prevention, queue-based fairness, bot defense

**Why these three together:** They form the full purchase funnel. A user browses a product page, adds to cart, and (sometimes) hits a flash sale. Each stage has a distinct dominant challenge — read scaling, transactional consistency, and traffic shaping — so interviewers use them to probe different dimensions of your system design knowledge.

**Common infrastructure across all three:** Redis (hot-path caching and atomic ops), Kafka (async event streaming and cache invalidation), Aurora MySQL (ACID durable storage), S3 (blob/archive storage), CDN (public content offload), Kubernetes with HPA (stateless service scaling), circuit breakers with graceful degradation, and a three-layer cache architecture (L1 in-process, L2 Redis, L3 CDN).

**Shared non-functional targets:**
- Product page P99 < 200 ms (above-fold SSR)
- Cart add/remove P99 < 100 ms; cart merge P99 < 300 ms
- Flash sale purchase confirmation P99 < 200 ms (admitted users only)
- Price and inventory freshness: ≤ 5 seconds for authoritative data
- Availability: 99.99% across all three

---

## STEP 2 — MENTAL MODEL

### Core Idea

Every e-commerce system is solving the same fundamental problem: **how do you serve accurate, fresh data at massive read scale while protecting a limited shared resource (inventory) from concurrent writes that must never oversell?**

The tension is always between speed (caching, precomputation, eventual consistency) and correctness (atomic operations, strong consistency, no double-sell). Different parts of the funnel sit at different points on that spectrum.

### Real-World Analogy: A Busy Grocery Store

Think of a grocery store where a million people are all trying to see the price of a specific item, add it to their cart, and buy the last one on the shelf.

- The **price tag on the shelf** is like your CDN cache — everyone can read it instantly, but it might be 30 seconds stale if the store manager just changed it.
- The **item in your physical cart** is like a soft inventory hold — you've reserved it, but if you abandon the cart and leave, someone else can take it after 15 minutes.
- **Checkout** is the only moment where inventory is hard-decremented — the cashier scans it and it's gone.
- A **flash sale** is like a store opening at 6 AM on Black Friday with 10 TVs at $99 — you need a numbered ticket (queue token), you wait in a physical line (virtual queue), and only the person at the front gets to buy. No ticket, no line. That's the only way to prevent mayhem.

The store can have millions of people reading the price tag simultaneously (reads scale horizontally). But the "take the last item" operation must be **serialized** — two people cannot both believe they are taking the last unit. That serialization is the hardest problem.

### Why This Is Hard

**Three compounding difficulties:**

1. **Scale vs. Freshness:** Prices and inventory change constantly (578 price updates/second, 2,314 inventory events/second at Amazon scale). You cannot query the database on every page load at 115,000 RPS. But a cache that's too stale shows wrong prices. The answer is event-driven cache invalidation with TTL as a safety net — but getting that right under failure conditions is subtle.

2. **Concurrent writes on a shared resource:** Two users adding the last unit to their cart must not both succeed at checkout. The naive solution (database row lock) doesn't scale under flash sale conditions. The production solution (Redis Lua atomic decrement) works but introduces a new SPOF and requires careful compensation logic.

3. **Traffic shaping for known cliffs:** A flash sale has a perfectly predictable traffic cliff — at T=0, one million users simultaneously send requests. No autoscaler can react in time (HPA takes 60-120 seconds). The solution is to shape the traffic before it hits the purchase path: pre-scale 5 minutes early, absorb the burst at the queue layer, and drip-feed admitted users to the purchase layer at a controlled rate.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Always ask these before drawing anything. They change the architecture significantly.

**For Amazon Product Page:**
1. "Is this for anonymous users, authenticated users, or both — and do they see different content?" *(Changes caching strategy entirely — you cannot cache personalized HTML at CDN)*
2. "What is the primary SLA — the P99 latency target for above-the-fold content?" *(Drives whether you need SSR vs. CSR, how aggressively you cache)*
3. "Is multi-seller (marketplace) or single-seller?" *(Multi-seller means buy-box selection, which is a whole separate subsystem)*
4. "Do we need reviews and Q&A on the same page, or is that a separate service?" *(Clarifies scope of the fan-out assembly)*
5. "How fresh does pricing need to be?" *(5 seconds? 30 seconds? Determines Redis TTL and whether CDN can cache pricing)*

**What changes based on answers:** If users are all anonymous, you can cache the entire SSR HTML at CDN indefinitely (almost). If pricing must be real-time, you cannot cache the page at all and must split the page into cacheable shell + live price widget. If there's no marketplace (single seller), the buy-box subsystem disappears entirely.

**Red flags:** If a candidate immediately starts drawing boxes without asking whether the page needs personalization, they will design a broken CDN caching strategy. Watch for candidates who skip the question "how fresh does inventory need to be" — that answer drives the entire consistency model.

---

**For Shopping Cart:**
1. "Do we need guest carts (pre-login) that persist and merge on login, or only authenticated carts?" *(Guest carts are 60% of the complexity — DynamoDB TTL, session service, merge algorithm)*
2. "Do we need soft inventory holds when an item is added to cart, or only hard reservation at checkout?" *(Soft holds add the Redis Sorted Set, Aurora durable hold records, and the sweep job)*
3. "What is the cart size limit? Single consumer or B2B (100+ items)?" *(B2B carts break the DynamoDB 25-item TransactWriteItems limit — you need batched writes)*
4. "Does price recalculation happen on every cart add, or lazily on cart view?" *(Lazy recalculation is the right answer, but candidates should derive it, not assume it)*

**What changes:** No guest carts = no Session Service, no merge logic, no TTL complexity. No soft holds = much simpler inventory story but worse UX (users see "out of stock" at checkout).

---

**For Flash Sale:**
1. "What is the demand-to-supply ratio? 10 units with 1,000 users, or 10,000 units with 1,000,000 users?" *(Ratio determines whether a queue is needed at all)*
2. "Is this integrated into the main e-commerce platform or an isolated system?" *(Isolation is the right answer — flash sale should not destabilize the main platform)*
3. "What are the fairness requirements — first-come-first-served, or lottery?" *(FCFS requires a sorted queue; lottery is simpler)*
4. "What are the bot prevention requirements?" *(No bot prevention = design is simpler; strong bot prevention = add Bot Filter layer and signed sale tokens)*

**What changes:** Low demand ratio (2:1) means no queue needed — just a rate limiter. High ratio (100:1) without a queue = system collapse. No bot requirement removes the most complex layer.

---

### 3b. Functional Requirements

**Amazon Product Page — Core:**
- Serve product detail page with title, description, images, bullet points, specs, and A+ content
- Show real-time pricing (current price, strikethrough original), stock status, and estimated delivery
- Display the buy-box winner (best seller/offer selection) and secondary offers
- Show aggregate ratings (average, histogram), paginated reviews with helpfulness voting
- Serve Q&A pairs (questions + ranked answers)
- Show price history for the last 90 days / 2 years
- Render recommendation widgets (Frequently Bought Together, Also Bought, Similar Items)
- Handle product variants — color, size, etc. — each mapped to a distinct child ASIN

**Scope statement:** "We are designing the read path of the Amazon product detail page. This includes the SSR assembly service, caching strategy, and the pricing/inventory/reviews subsystems. Checkout and order management are out of scope."

**Shopping Cart — Core:**
- Add, update quantity, and remove items; each item has SKU, quantity, variant, and price snapshot
- Persist carts for authenticated users indefinitely; guest carts for 30 days via session token
- Merge guest cart into authenticated cart on login (quantity sum, capped at available inventory)
- Recalculate prices lazily on cart view; show a "price changed" banner if a price shifted since add
- Apply and validate coupon codes (per-code limits, per-user limits, expiry, minimum order)
- Place soft inventory holds (15-minute TTL) on items in cart; release on checkout, remove, or inactivity
- Return cart summary: subtotal, coupon discount, shipping estimate, tax estimate, grand total

**Flash Sale — Core:**
- Schedule a flash sale with: product, quantity, price, start time, end time, max-per-user
- Show a countdown timer pre-sale; hide the buy button until T=0
- Issue signed "sale tokens" to users who pass bot checks before sale opens
- Accept purchase requests at T=0; enqueue all arrivals in a FCFS virtual queue
- Drain queue at a controlled rate (e.g., 200 users/second); admit users to the purchase step
- Atomically decrement inventory on each purchase — no oversell under any circumstances
- Show real-time remaining inventory counter to all waiting users via SSE
- Enforce per-user purchase limit (e.g., 1 unit per user per sale)

---

### 3c. Non-Functional Requirements and Trade-offs

**Latency:**
- Product page above-fold SSR: P99 < 200 ms. Every 100 ms of added latency costs ~1% conversion (Amazon internal data). This is not a soft target — it is revenue-critical.
- Cart read (GET /cart with price recalculation): P99 < 50 ms
- Cart write (add/update/remove): P99 < 100 ms
- Flash sale purchase confirmation (admitted user only): P99 < 200 ms

**Availability:** 99.99% across all three (≤ 52.6 minutes downtime/year). The cart and flash sale services are directly on the revenue critical path.

**Consistency trade-offs:**
- ✅ Pricing in Redis (up to 30 seconds stale) — acceptable for display; always re-validated at checkout against Aurora
- ✅ Rating aggregates eventually consistent within 60 seconds — acceptable for below-fold display
- ✅ CDN-cached anonymous product page HTML (30 second TTL) — acceptable because pricing is non-personalized display
- ❌ Inventory decrement at checkout — must be strongly consistent; Redis Lua atomic or Aurora row lock
- ❌ Cart contents after a write — read-your-writes guarantee required (DynamoDB ConsistentRead=true within 2 seconds of a write)
- ❌ Flash sale inventory counter — must be atomic; no oversell is a hard requirement

**Scalability:** Amazon product pages: 115,600 RPS peak (Prime Day). Cart: 27,778 RPS peak. Flash sale burst: 240,000 RPS enqueue burst for 10 seconds, then 200 RPS steady admission.

**Key trade-off to articulate:** "We are explicitly trading consistency for availability and performance in the display layer (cached prices, CDN HTML), but switching to strongly-consistent reads at every transaction boundary (checkout, cart merge, inventory decrement). This is a layered consistency model, not one-size-fits-all."

---

### 3d. Capacity Estimation

**Amazon Product Page — anchor numbers:**
- 300M active customers × ~8 page views/day = **2.5 billion page views/day**
- Average RPS: 2.5B / 86,400 = **~29,000 RPS** (normal), **~116,000 RPS** peak (4× Prime Day)
- 600 million active ASINs globally
- Product catalog storage: 600M ASINs × 10 KB = **6 TB**
- Product images: 600M × 5 images × 3 MB = **9 PB on S3**; CDN absorbs ~95% of image requests
- Reviews: 5 billion cumulative × 2 KB = **10 TB in Cassandra**
- Price history: 600M ASINs × 8,760 hours/year × 2 years × 20 bytes = **~105 TB in TimescaleDB**
- Peak bandwidth: 116K RPS × 120 KB HTML = **~13.9 GB/s** (CDN bears the vast majority)
- **Architecture implication:** At 116K RPS with a CDN hit ratio of 90%, origin sees only ~11,600 RPS. This is achievable with 500 PDP SSR pods at 100 RPS each (3 regions × 167 pods = 500 total). Without CDN, you'd need 5,000 pods.

**Shopping Cart — anchor numbers:**
- 100 million active authenticated carts × 1.7 KB/cart = **~170 GB hot storage** (fits in Redis cluster)
- 36 million guest carts (30-day TTL) × 1.7 KB = **~61 GB**
- Peak cart operations: 120M DAU × 5 ops/session = 600M ops/day = **~7,000 average RPS, ~28,000 peak RPS**
- Cart event audit log: 600M events/day × 500 B × 365 = **~110 TB/year** (cold storage, S3 Parquet)
- **Architecture implication:** 240 GB of hot cart data fits comfortably in a Redis cluster with 6 shards (3 primary + 3 replica), 64 GB each = 384 GB total. DynamoDB on-demand handles burst writes without manual capacity planning.

**Flash Sale — anchor numbers:**
- 10,000 units × 100:1 demand ratio = **1,000,000 interested users**
- 80% arrive in first 60 seconds → burst: 800K × 3 attempts / 10 seconds = **240,000 RPS** for 10 seconds
- Sustained queue drain rate: 200 users/second → sale sells out in **~50 seconds** for 10,000 units
- Queue storage: 1M entries × 200 B = **200 MB** — fits on a single Redis node
- Inventory counter: single key, single Redis node — **< 1 ms per Lua decrement**
- **Architecture implication:** The burst (240,000 RPS) hits only the Bot Filter and Queue Service — both stateless and pre-scaled. The Purchase Service only ever sees 200 RPS (the Admitter rate). This dramatic reduction (240,000 → 200) is the entire point of the queue design.

**Estimation time guidance:** Plan to spend 5-7 minutes on this. Derive numbers from first principles (active customers → page views → RPS), identify peak multiplier (4× for product pages), and always state the architectural implication ("this is why we need CDN to absorb X% of traffic" or "this is why the inventory Redis node only sees 200 ops/second, not 240,000").

---

### 3e. High-Level Design

**Amazon Product Page — 6 key components:**

1. **CDN (CloudFront/Akamai):** Caches static assets (1-year TTL, immutable content-hash URLs) and anonymous SSR HTML (30-second TTL). Absorbs 80-90% of all traffic. WAF rules block DDoS. **Without this:** Origin sees 116,000 RPS and requires 5,000 pods instead of 500.

2. **API Gateway / Load Balancer:** TLS termination, auth token validation, rate limiting, routing to PDP SSR Service and sub-services.

3. **PDP SSR Service (Node.js/React):** The core orchestrator. Fans out **in parallel** to up to 8 downstream services (Catalog, Pricing, Inventory, Reviews, Seller, A+ Content, Recommendations, Q&A) with per-service timeouts and fallbacks. Renders server-side HTML. Caches the assembled HTML in Redis (5-second TTL, anonymous). Implements fragment caching: cacheable shell (catalog, pricing, reviews) + personalization sidecar (Prime eligibility, wishlist, purchase badges — fetched client-side via XHR).

4. **Redis Cluster (L2 Cache):** 32 primary nodes, 96 TB total. Stores: prices (30-second TTL), inventory display counts (30-second TTL), rating aggregates (60-second TTL), recommendations (5-minute TTL). The read path for virtually all hot data. **Without this:** Every PDP load hits Aurora/Cassandra directly — impossible at 116K RPS.

5. **Kafka Event Bus:** Decouples writes from cache invalidation. Topics: `price-events` (578 RPS, partitioned by asin), `inventory-events` (2,314 RPS), `review-events`, `catalog-updated`. Event-driven cache invalidation: when price changes, Kafka consumer calls DEL on the Redis key + CloudFront soft-purge. TTL is the safety net if the event is dropped.

6. **Downstream service constellation:** Product Catalog Service (DynamoDB, 6 TB), Pricing Service (Aurora + Redis, buy-box selection), Inventory Service (Redis atomic + Aurora authoritative), Reviews Service (Cassandra, 10 TB), Ratings Aggregator (Kafka consumer updating Aurora), Price History Service (TimescaleDB, 105 TB), Recommendations Service (pre-computed in Redis + DynamoDB), Seller Service (Aurora MySQL).

**Data flow (whiteboard order):** Draw CDN first → API Gateway → PDP SSR Service → parallel fan-out to 8 services → Redis cluster shared by all services → Kafka for async events → databases behind each service.

**Key decision to articulate:** Why SSR (server-side rendering) instead of CSR? Because search crawlers (Google, Bing) need fully-rendered HTML for SEO. A CSR-only product page would be invisible to search, costing enormous organic traffic.

---

**Shopping Cart — 5 key components:**

1. **API Gateway:** Validates JWT (authenticated users) or session cookie (guests), extracts user_id or session_id, rate limits.

2. **Cart Service (Go/Java, stateless):** Core business logic. Three internal managers: Cart Manager (CRUD, merge, idempotency), Price Recalculator (lazy on cart view, parallel fan-out to Pricing Service per SKU, cached 60 seconds), Inventory Advisor (soft hold placement/release via Redis Lua + Aurora).

3. **Cart Store (DynamoDB + Redis):** DynamoDB is the durable source of truth. Partition key = cart_id (UUID). ConsistentRead=true for read-your-writes guarantee. Global Tables active-active in 3 regions (us-east-1, eu-west-1, ap-southeast-1). Redis is a read-through cache invalidated on every write (TTL = 60 seconds). Guest cart TTL set as DynamoDB item TTL (30-day auto-expiry).

4. **Coupon/Promo Service (Aurora MySQL):** ~10M coupon codes. Validates eligibility and applies optimistic locking: `UPDATE coupons SET usage_count = usage_count + 1 WHERE coupon_id = ? AND usage_count < max_uses` — single atomic statement, no SELECT FOR UPDATE. Unique index on (coupon_id, user_id) prevents per-user double-use.

5. **Kafka + Cart Event Logger:** All cart mutations (add, remove, merge, coupon, checkout) published to `cart-events`. Consumed by Cart Event Logger → Kinesis Firehose → S3 Parquet (queryable by Athena for fraud detection and ML training).

**Data flow (whiteboard order):** Client → API Gateway → Cart Service → DynamoDB (consistent write) + Redis invalidation → Kafka event → downstream consumers (Inventory Service for holds, Cart Event Logger for audit).

---

**Flash Sale — 6 key components:**

1. **CDN + WAF + AWS Shield:** Pre-sale countdown page cached at CDN (99% hit rate). WAF blocks known bot IPs, data center IP ranges, requests without browser User-Agents. Shield Advanced absorbs DDoS. **Critical:** the pre-sale page should never reach origin.

2. **Bot Filter / Token Gate:** Issues signed "sale tokens" (JWT, valid 5 minutes) to users passing: reCAPTCHA v3 (score ≥ 0.5), account age ≥ 30 days, device fingerprint cross-check, and behavioral scoring. A sale token is required to enter the queue. **Without this:** Bots can flood the queue and consume all positions before humans finish typing.

3. **Queue Service (100 pods, pre-scaled):** Accepts purchase requests at T=0. Adds user to Redis Sorted Set `sale_queue:{sale_id}` with score = epoch_microseconds (FCFS ordering). ZADD NX prevents duplicate entries. ZRANK returns O(log N) position. Returns a queue token (signed JWT) with position and estimated wait time.

4. **Admitter Process (single leader with Redis distributed lock):** Drains the queue at a controlled rate (e.g., 200 users/second, configurable per sale). Every 10 ms, advances the admission cursor by 2 entries and sets their queue token status to "admitted" in Redis. Standby pods acquire the lock if the leader crashes (lock TTL = 30 seconds). **Key insight:** This is the traffic shaper that converts 240,000 RPS to 200 RPS at the purchase layer.

5. **Purchase Service (200 pods, pre-scaled):** Validates queue token (SETNX `sale_token_used:{jti}` prevents replay), checks per-user limit (SETNX `purchased:{sale_id}:{user_id}`), runs Redis Lua atomic inventory decrement, writes purchase record to Aurora, publishes to Kafka. Handles exactly 200 requests/second — right-sized to the Admitter's drain rate.

6. **Inventory SSE Publisher (50 pods × 10K connections = 500K total):** Consumes Kafka `inventory-events`, pushes `{remaining: N}` to all SSE subscribers within 2 seconds. Uses SSE (not WebSocket) — unidirectional, HTTP/2 multiplexed, auto-reconnects.

**Data flow (whiteboard order):** CDN → Bot Filter (T-30s for pre-auth) → Queue Service (T=0 burst) → Admitter (rate control) → Purchase Service → Redis Lua (atomic decrement) → Aurora (order record) → Kafka → SSE Publisher → all clients.

---

### 3f. Deep Dive Areas

**Deep Dive 1 — PDP Parallel Fan-Out with Bounded Latency (most probed on product page)**

**Problem:** The PDP needs data from 8 services. Sequential calls = 8 × 50 ms = 400 ms. That violates the 200 ms P99 target.

**Solution:** Parallel fan-out with per-service timeouts and graceful degradation. All 8 service calls launch simultaneously (Promise.all in Node.js, goroutines in Go). Each has its own timeout (Catalog 100 ms, Pricing 50 ms, Inventory 50 ms, Reviews 80 ms, Recs 80 ms, A+ 100 ms). If any service times out or fails, the page assembles with a fallback: Pricing timeout → show last cached price + "Price may have changed" note; Reviews timeout → "Reviews loading..." (lazy-loaded client-side); A+ content timeout → hide the A+ section (not critical above fold).

Each downstream call is wrapped in a circuit breaker (Hystrix pattern): closed (normal) → open (when failure rate exceeds 50% in a 10-second sliding window, immediately return fallback without making the network call) → half-open (after 30-second cool-down, allow 5 probe requests).

**Unprompted trade-off to mention:** Fragment caching splits the assembled page into a "cacheable shell" (catalog data, pricing, ratings — same for all users viewing the same ASIN) and a "personalization sidecar" (Prime eligibility, wishlist status, purchased badges — user-specific). The shell is cached at CDN with a 30-second TTL. The sidecar is fetched client-side via a small XHR keyed on session cookie. This lets you cache 95% of the page at CDN while personalizing the remaining 5%, without varying the entire CDN cache by user_id (which would destroy your cache hit ratio).

---

**Deep Dive 2 — Redis Lua Atomic Inventory Decrement (most probed on flash sale)**

**Problem:** Two concurrent purchase attempts when 1 unit remains must not both succeed. The naive `GET inv:{sale_id}` then `DECR inv:{sale_id}` has a TOCTOU race — both reads can return 1, both decrements happen, counter goes to -1, you've oversold.

**Solution:** Redis Lua script executes atomically — Redis is single-threaded in command execution, so no other command can interleave during a Lua script:

```lua
local remaining = tonumber(redis.call('GET', KEYS[1]))
if remaining == nil then return -2 end  -- not warmed
if remaining < tonumber(ARGV[1]) then return -1 end  -- sold out
local new_remaining = redis.call('DECRBY', KEYS[1], ARGV[1])
return new_remaining
```

Return value: -2 = not initialized, -1 = sold out, ≥ 0 = success with new remaining count.

**Compensation on failure:** Inventory is decremented before the Aurora write (optimistic). If the Aurora write fails, the Purchase Service runs `INCR inv:{sale_id}` to restore the unit. If the service crashes between DECR and the compensation INCR, a Reconciliation Job (runs every 5 minutes) compares `total_inventory - remaining_in_redis` against `COUNT(confirmed purchases in Aurora)` and auto-corrects via INCR.

**Unprompted trade-off to mention:** A single Redis node is technically a SPOF for the inventory counter. Mitigation: Redis Sentinel with 1 replica (failover in 15 seconds; Purchase Service returns 503 with `Retry-After: 15` during failover). For hyper-scale sales (Apple iPhone launch, 10M+ concurrent), use inventory bucket sharding: split `total_inventory` across N Redis nodes (`inv:{sale_id}:0` through `inv:{sale_id}:N-1`), each holding `total_inventory/N`. Lua script is atomic per shard; no cross-shard coordination needed. Sold-out detection: check a global `inv_total:{sale_id}` counter.

---

**Deep Dive 3 — Cart Merge on Login (most probed on shopping cart)**

**Problem:** When a guest user logs in, their guest cart (keyed by session_id) must merge into their authenticated cart (keyed by user_id). This must be: (a) idempotent — if two devices log in simultaneously, items are not doubled; (b) correct — overlapping SKUs must sum quantities, capped at available inventory; (c) atomic — no partial merges.

**Three-layer safety approach:**

Layer 1 — Fast idempotency check: `Redis SETNX merge:done:{user_id}:{session_id} "1" EX 86400`. If already set, the merge already happened — return the cached result. This fires first on every merge request.

Layer 2 — Distributed lock for serialization: `Redis SET lock:cart:{user_id} {request_id} NX EX 30`. Prevents two simultaneous login events from running concurrent merges. Second concurrent request waits (max 3 retries with 100/200/400 ms backoff).

Layer 3 — Atomic write: `DynamoDB TransactWriteItems` — all-or-nothing multi-item write. For large B2B carts (>25 items), batch into groups of 25 with a `merge_progress` attribute tracking which SKUs are written, so a mid-batch failure can resume from the checkpoint.

Algorithm: Build a merged item map (sum quantities for overlapping SKUs), then for each SKU where summed qty > 1, check inventory (cap at available). Mark the guest cart as `status: "merged"` in the same transaction.

**Unprompted trade-off:** Guest soft holds (tied to the guest cart_id) become orphaned after merge. The Inventory Service background sweep (every 60 seconds, using `ZREMRANGEBYSCORE holds:{sku} 0 {now}`) handles this. There's a 1-2 minute window where holds are inconsistent — acceptable because the merged auth cart re-acquires holds on the next cart view.

---

### 3g. Failure Scenarios

**Amazon Product Page:**

| Failure | Impact | Mitigation |
|---|---|---|
| Redis cluster node failure | 1/32 of keyspace returns cache miss; increased Aurora load | Sentinel auto-promotes replica (15 s); L1 in-process cache (256 MB/pod) absorbs heat for popular ASINs |
| Aurora primary failure | No new price/review writes; reads continue on replicas | Multi-AZ auto-failover (20-30 s); application retries with exponential backoff |
| Pricing Service complete outage | Circuit breaker opens; PDP shows last-known Redis price | Staleness timestamp displayed; PagerDuty alert; price re-validated at add-to-cart |
| CDN PoP outage | High latency for users in affected region | Route 53 latency routing fails over to next PoP in < 60 s |
| Cache stampede on hot ASIN expiry | Thundering herd on Aurora | Probabilistic early expiration: when TTL < 20% remaining, random 10% of requests refresh in background; SETNX refresh lock allows only one origin request |

**Senior framing for product page:** "The product page is a read-dominated, eventually-consistent system where correctness matters at transaction boundaries. A Redis outage degrades the experience (higher latency, slightly stale data) but never causes data loss or oversell — because Redis is never the authoritative store for anything we transact on."

**Shopping Cart:**

| Failure | Impact | Mitigation |
|---|---|---|
| DynamoDB partition unavailable | Cart reads/writes fail for affected carts | Global Tables active-active; route to another region automatically |
| Redis node failure | Cache miss; DynamoDB read; 60 s window of stale computed totals | Automatic replica promotion; fallback to DynamoDB with ConsistentRead=true |
| Pricing Service unavailable during cart view | Grand total shows last-snapshotted price | Circuit breaker; display "prices may not be current" warning; block checkout until Pricing recovers |
| Inventory Service unavailable during add | Cannot place soft hold | Proceed to cart add without hold (optimistic mode); display "availability not confirmed"; hard check at checkout |

**Flash Sale:**

| Failure | Impact | Mitigation |
|---|---|---|
| Redis inventory node fails | No purchases possible (15 s window) | Sentinel failover; Purchase Service returns 503 with Retry-After: 15 |
| Admitter crashes | Queue stops draining (up to 30 s) | Standby pod acquires Redis distributed lock; resumes from admission_cursor |
| Aurora unavailable during purchase | Cannot write order record | Return 503 to client; compensate Redis INCR to restore unit; retry |
| Bot flood before queue entry | Overwhelms Bot Filter pods | WAF rate limits at CDN before requests reach Bot Filter |

**Senior framing for flash sale:** "The flash sale's correctness guarantee comes from Redis Lua atomicity. The availability guarantee comes from pre-scaling (not HPA reactivity). The fairness guarantee comes from the queue. Each guarantee is independently managed by a separate component — failure in one does not cascade to the others."

---

## STEP 4 — COMMON COMPONENTS

### Redis for Hot-Path Caching with Short TTL

**Why used:** The gap between what a database can serve (thousands of ops/second per node) and what the system needs to serve (tens of thousands to hundreds of thousands of RPS) is bridged entirely by Redis. Every frequently-read mutable value — prices, inventory display counts, rating aggregates, computed cart totals — lives in Redis with a short TTL.

**Key configuration:**
- Product page: 32-primary Redis Cluster, 96 TB total capacity, allkeys-lru eviction, hash slot partitioning (16,384 slots across nodes), Sentinel for failover (15 s RTO)
- Cart: 6-shard cluster (3 primary + 3 replica), 384 GB total, TTL per key type: cart totals 60 s, price per SKU 30 s, coupon records 5 min, soft holds 15 min via Sorted Set score
- Flash sale: single primary + replica per sale for inventory counter (atomicity isolation); separate node for queue Sorted Set (resource isolation); Sorted Set TTL 30 min (sale duration + buffer)

**What happens without it:** At 116,000 RPS, each PDP load would hit Aurora for pricing, inventory, and ratings — 3 Aurora reads × 116,000 RPS = 348,000 Aurora reads/second. Aurora max throughput (with 15 read replicas) is roughly 150,000 reads/second total. The system would be under 2× overload at peak, resulting in cascading database failures.

---

### Kafka for Async Event Streaming and Cache Invalidation

**Why used:** Writes to the source of truth (price change in Aurora, new review in Cassandra, cart add in DynamoDB) need to propagate to multiple consumers: Redis cache invalidation, CDN purge, analytics pipelines, audit logs, SSE publishers. Doing all of these synchronously in the write path would: (a) add latency, (b) create tight coupling, (c) break the write if any downstream fails.

Kafka decouples the write from all downstream propagation. The source-of-truth write succeeds independently. Kafka delivers the event to each consumer asynchronously, with at-least-once delivery and 7-day message retention (replay capability for recovery).

**Key configuration:**
- RF = 3 (replication factor) across all topics — no message loss on single-broker failure
- Partitioning by business key: `asin % 64` for price events (ensures FIFO per product), `cart_id` for cart events, `sale_id` for flash sale events
- 64-256 partitions per topic — partition count = max consumer group size for horizontal scaling
- Flash sale queue events: reconstructable from Kafka 7-day replay if Redis queue data is lost

**What happens without it:** Synchronous cache invalidation would mean: price update in Aurora → blocking call to Redis DEL → blocking CloudFront purge → blocking analytics event — all in the same write request. If CloudFront API is slow (200 ms), every price update is 200 ms slower. If analytics is down, the write path fails. Kafka eliminates all of this.

---

### Aurora MySQL for ACID Transactional Data

**Why used:** Some data requires ACID guarantees: coupon usage counts (must not over-issue), flash sale purchase records (must never double-sell), pricing records (consistent snapshot for buy-box selection), soft hold records (durable audit trail). DynamoDB is great for scale but lacks native SQL transactions across tables. Cassandra is great for append-heavy data but lacks strong consistency for update patterns. Aurora gives you MySQL semantics with Multi-AZ synchronous replication, 15 read replicas, and sub-second cross-region async replica.

**Key configuration:**
- Multi-AZ synchronous replication: primary + 2 standby in different AZs; automatic failover in 20-30 s (RTO), RPO ≈ 0
- Read replicas: up to 15 (Aurora limit); 15 on product page for pricing; 2-3 on cart and flash sale
- Cross-region async replica for DR: RPO ≤ 1 second
- Key usage pattern: Aurora is never hit on every request — it is always behind Redis cache. It is only hit on cache misses, writes, and transaction boundaries (checkout, purchase)

**What happens without it:** If you try to use Redis alone for coupons (usage counter), you lose data on Redis restart or failover. If you use DynamoDB for inventory without Aurora backup, you cannot run the reconciliation job to detect oversell discrepancies. Aurora is the durable ground truth that makes Redis a safely disposable cache.

---

### Idempotency Enforcement (Redis SETNX + Database UNIQUE Constraints)

**Why used:** Networks are unreliable. Clients retry on timeout. A cart "add item" retry should not add the item twice. A coupon application retry should not charge the discount twice. A flash sale purchase retry should not charge the customer twice. Every write operation must be idempotent.

**Key configuration — two-layer pattern:**
- **Fast layer (Redis SETNX):** `SET idem:{idempotency_key} {serialized_response} NX EX 86400`. On duplicate key, return the original response immediately without re-executing the operation. 24-hour key TTL. This is the O(1) path for client retries.
- **Durable safety net (DB UNIQUE constraint):** `UNIQUE (user_id, sale_id)` on flash_purchases prevents double-purchase even if Redis idempotency data is lost. `INSERT ... ON CONFLICT DO NOTHING` on review submissions prevents duplicate reviews. `UNIQUE (coupon_id, user_id)` prevents double-redemption.

The Redis layer handles the common case (client retries within 24 hours) at sub-millisecond speed. The DB constraint handles the rare case (Redis failure, or replay after 24 hours). Both are needed — neither alone is sufficient.

**Specific instances across problems:**
- Cart merge: `SETNX merge:done:{user_id}:{session_id}` (86400 s) + `DynamoDB TransactWriteItems` conditional write
- Review vote: `SADD voted_users:{review_id} {user_id}` (90-day Redis Set) prevents double-vote
- Flash sale purchase: `SETNX sale_token_used:{jti}` (5-min TTL) prevents queue token replay; `SETNX purchased:{sale_id}:{user_id}` prevents double-purchase; Aurora `UNIQUE KEY uq_user_sale` as final DB safety net

---

### S3 for Large Blob and Archive Storage

**Why used:** Some data is too large or too infrequently accessed for operational databases: product images (3 MB average, 9 PB total), review videos (500 KB average), A+ content HTML blobs (50 KB average), cart event audit logs (110 TB/year). S3 provides effectively unlimited storage, 11 nines of durability, no provisioning, and deep integration with CDN (images), Kinesis Firehose (event archives), and Athena (SQL queries on Parquet).

**Key configuration:**
- Product images: immutable content-hash URLs (`cdn.example.com/img/sha256-abc.jpg`). URL never changes for the same content — CDN TTL set to 1 year (`max-age=31536000, immutable`). Any image update gets a new URL, instantly propagating without cache invalidation.
- Cart event logs: Kinesis Firehose buffers events and writes Parquet files to S3 in 5-minute batches. Athena queries run SQL over the Parquet without loading data into a database. Queryable for fraud detection and ML training feature engineering.
- A+ content: HTML blobs keyed by `asin/{marketplace_id}/aplus.html`. DynamoDB stores only the S3 key and version metadata. Read via CDN (5-minute TTL). CloudFront invalidation triggered by Kafka `catalog-updated` event on seller update.

**What happens without it:** Storing product images in a database is obviously wrong. But the non-obvious case is cart event logs — storing 110 TB/year in a relational database would be expensive and require massive sharding. S3 + Parquet + Athena gives you SQL analytics at near-zero per-query cost with no schema management.

---

### CDN for Public and Cacheable HTML/Static Assets

**Why used:** At 116,000 RPS peak, the most cost-effective optimization is to answer requests before they ever reach your origin. A CDN with a 90% hit ratio converts 116,000 RPS of origin traffic into 11,600 RPS — a 10× reduction. This is the single biggest lever in the product page design.

**Key configuration:**
- Anonymous SSR HTML: `Cache-Control: public, max-age=30, stale-while-revalidate=60`. 30-second TTL balances freshness (pricing changes within 5 seconds; CDN is acceptable to lag by 30 seconds for display) with offload ratio (a 30-second window means popular pages stay in cache through most of their traffic lifetime).
- Static assets (JS, CSS): content-hash URLs, 1-year TTL, `immutable` directive.
- Product images: 1-year TTL, immutable content-hash URLs.
- Flash sale pre-sale countdown: 10-second TTL pre-sale; 2-second TTL during live sale; 0 TTL (bypass) for sold-out transition.
- Cart: NOT cached at CDN — cart data is user-specific and dynamic. Cart UI (the JavaScript bundle) is CDN-cached.

**Key CDN operation — event-driven invalidation:** When a product attribute changes, the Catalog Service publishes a `catalog-updated` event to Kafka. A cache-invalidator consumer calls CloudFront's invalidation API for `/dp/{asin}` and issues `DEL pdp:html:{asin}:*` in Redis. TTL is the safety net if the invalidation event is dropped.

**Flash sale + WAF + Shield:** For the flash sale, the CDN is not just a cache — it is also the DDoS defense layer. AWS Shield Advanced absorbs volumetric attacks (Gbps-scale). WAF rules block requests from Tor exit nodes, data center IP ranges, and User-Agents matching known bot signatures. Geo-restriction blocks regions not served by the marketplace. All of this happens before a single byte reaches origin.

---

### Inventory Reservation (Soft Hold → Hard Decrement)

**Why used:** Without reservation, two users can add the last unit to their carts simultaneously, both see "In Stock," and only one can complete checkout. The other hits a frustrating out-of-stock error at payment time. Soft holds reserve inventory tentatively — reducing the displayed available count so subsequent shoppers see accurate availability — without irreversibly committing it (the hold expires if the user abandons the cart).

**Key configuration (shopping cart):**
- Redis Sorted Set `hold:{sku}` where each member is a `hold_id` and the score is the hold expiry timestamp. ZADD NX prevents duplicate hold registration.
- On every availability check: `ZREMRANGEBYSCORE hold:{sku} 0 {now}` sweeps expired holds first (O(log N)), then `ZCARD hold:{sku}` gives active hold count.
- Lua atomic script for TOCTOU safety: the sweep + check + add is a single Lua execution.
- Aurora durable hold record for reconciliation (source of truth; Redis is the fast operational view).
- Hold TTL: 15 minutes static when cart page is inactive. Rolling TTL extension: frontend sends `POST /v1/cart/heartbeat` every 5 minutes while the cart page is open, extending holds by another 15 minutes. Abandoned carts release holds within 15 minutes of inactivity.

**Key configuration (flash sale):**
- No soft hold phase — the queue admission acts as the soft reservation. An admitted user has 2 minutes to complete purchase (configurable `purchase_window_seconds`). If they don't, the Purchase Service reclaims the slot.
- Hard atomic decrement at purchase time via Redis Lua.
- Conversion to hard hold (checkout): `hold_type` changes from `soft` to `hard` in Aurora; TTL removed from Redis. If payment fails, Checkout Service calls `INCR inv:{sale_id}` and marks the Aurora record as `cancelled`.

---

### Circuit Breakers + Graceful Degradation

**Why used:** In a microservices architecture, downstream failures are inevitable. Without circuit breakers, a slow or failing downstream service causes threads to accumulate waiting for timeouts, eventually exhausting connection pools and crashing the caller. Circuit breakers detect failure patterns and short-circuit calls to failing services, returning cached or default values immediately instead of waiting.

**Key configuration (Hystrix/Resilience4j pattern):**
- Sliding window: 10 seconds
- Failure rate threshold: 50% (when more than half of calls in the window fail → open the circuit)
- Slow call rate threshold: 80% of calls exceeding the timeout → also opens the circuit
- Wait duration in open state: 30 seconds (no calls to the failing service)
- Half-open state: allow 5 probe requests; if they succeed, close the circuit; if they fail, remain open
- Each downstream service has its own circuit breaker — not a global one

**Fallbacks per service on product page:**
- Pricing Service circuit open → use last-known Redis price + "Price last updated at [timestamp]" note
- Inventory Service circuit open → hide "Only X left" badge; show "Check availability" link
- Reviews Service circuit open → "Reviews loading..." placeholder; client-side lazy load
- A+ content circuit open → hide A+ section (not critical for above-fold experience)
- Recommendations circuit open → hide widget entirely

**Why per-service breakers (not global):** Each widget has a different criticality level and different acceptable degradation. A global circuit breaker would make all-or-nothing decisions. Pricing going down is catastrophic (need a fallback that still shows a price). Q&A going down is trivial (just hide the section). Per-service breakers allow nuanced degradation.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Amazon Product Page

**What is unique:**
- **Parallel fan-out to 8 independent services in a single SSR response under 200 ms P99** — no other problem in this pattern requires this. The PDP SSR Service is an orchestrator, not a business logic service.
- **Buy-box selection**: for multi-seller listings, a weighted scoring algorithm (`score = (1/price)×0.40 + FBA_bonus×0.30 + (seller_rating/100)×0.20 + in_stock×0.10`) determines the featured offer. Weights are stored in a config service (A/B testable per marketplace). Triggered on every price change event with a 500 ms debounce window to collapse burst repricing.
- **Cassandra for reviews**: the only one of the three problems to use Cassandra. Wide-row data model maps perfectly to ASIN-partitioned, time-sorted review retrieval. QUORUM writes, LOCAL_ONE reads for asymmetric consistency.
- **TimescaleDB for price history**: time-series data with PostgreSQL SQL compatibility; weekly hypertable chunks; continuous aggregates for hourly rollups; chunk drop policy for 2-year retention. 105 TB total.
- **Three-layer cache** (L1 in-process LRU + L2 Redis Cluster + L3 CDN): the only problem in this pattern with an L1 in-process cache (256 MB/pod, ristretto, 2-second TTL). Justified by 116K RPS — even sub-millisecond Redis latency becomes a bottleneck at this scale, so the most popular 1 million ASINs are cached in-process on each pod.
- **Fragment caching (shell + personalization sidecar)**: splits the page into a CDN-cacheable shell (same for all users per ASIN) and a client-side personalization layer. This is what makes CDN caching viable despite per-user personalization.

**Two-sentence differentiator:** The Amazon product page is the only problem where the core challenge is assembling a coherent response from 8 independent microservices within a 200 ms P99 budget using parallel fan-out, per-service circuit breakers, and fragment caching to separate the cacheable 95% from the personalized 5%. It is fundamentally a read orchestration and caching problem — no other problem in this pattern involves buy-box selection, time-series price history, Cassandra-backed reviews, or a three-layer cache architecture reaching 96 TB.

---

### Shopping Cart

**What is unique:**
- **Guest-to-authenticated cart lifecycle**: the only problem in this pattern with a pre-authentication state and a stateful transition. Guest carts are server-side (not localStorage), keyed by a session token in a cookie, with a 30-day TTL auto-managed by DynamoDB item expiry. No other problem requires a Session Service to manage anonymous session state.
- **Idempotent cart merge**: the three-layer safety net (Redis SETNX idempotency key + Redis distributed lock + DynamoDB TransactWriteItems) is specific to this problem. The algorithm (quantity summation, inventory capping, batch handling for B2B carts >25 items) is a unique design.
- **Soft inventory holds with rolling TTL**: the Redis Sorted Set `hold:{sku}` pattern with the Lua atomic sweep-and-add is specific to shopping cart. The rolling TTL extension via frontend heartbeat (`POST /v1/cart/heartbeat` every 5 minutes) is a unique UX-driven engineering decision.
- **Lazy price recalculation with snapshot tracking**: prices are snapshotted at add-to-cart time and compared against current prices on cart view. The `price_changed: true` flag and `price_warnings[]` array in the GET /cart response are unique to this problem.
- **DynamoDB Global Tables active-active**: the only problem in this pattern using DynamoDB Global Tables for multi-region active-active consistency with last-writer-wins conflict resolution.

**Two-sentence differentiator:** The shopping cart is the only problem in this pattern defined by a state lifecycle — anonymous guest session to authenticated user, with idempotent merge on login — and the only one requiring both soft inventory reservation and a lazy price recalculation cache layer simultaneously. Its consistency model is more nuanced than the others: read-your-writes for cart contents (via ConsistentRead=true), eventual consistency for computed totals (60-second cache), and optimistic locking for coupon usage counts.

---

### Flash Sale

**What is unique:**
- **Bot filter with signed sale tokens before queue entry**: the only problem in this pattern with an explicit pre-admission trust boundary. Bots are filtered before they can enter the queue, not after. The sale token (JWT, valid 5 minutes) is the output of passing: reCAPTCHA v3 score ≥ 0.5, account age ≥ 30 days, device fingerprint cross-check, behavioral ML scoring.
- **Virtual queue with single-leader Admitter**: the Redis Sorted Set queue with FCFS ordering by epoch microseconds, and the Admitter process controlling admission rate, are purpose-built for this problem. No other problem has a traffic shaping layer that converts 240,000 RPS to 200 RPS.
- **Redis Lua atomic inventory decrement (not Redis DECR)**: the key insight is that bare Redis DECR can go negative (DECR on 0 returns -1 — an oversell). Only the Lua script with an explicit guard (`if remaining < ARGV[1] then return -1 end`) is safe.
- **Pre-scaling 5 minutes before sale start**: HPA cannot react fast enough for a known traffic cliff. Flash sale is the only problem in this pattern requiring explicit pre-scaling via a sale scheduler monitoring `starts_at`.
- **SSE for real-time inventory countdown**: 500K simultaneous SSE connections (50 pods × 10K connections/pod) pushing `{remaining: N}` updates. No other problem in this pattern uses server-push technology.
- **Separate infrastructure isolation**: flash sale is intentionally a separate service cluster. A flash sale should not destabilize the main platform (product pages, cart). This means dedicated Redis nodes, dedicated Kafka topics, and dedicated Aurora tables.
- **ClickHouse for real-time analytics**: the only problem in this pattern using ClickHouse. Needed for real-time sale dashboards, bot detection model training data, and post-sale fraud analysis at high-throughput event rates.

**Two-sentence differentiator:** Flash sale is the only problem in this pattern with a known future traffic cliff — a precise timestamp at which 1 million users simultaneously attempt to purchase a strictly limited resource — requiring three sequential defense layers: bot filter (eliminates automated bots before queue), virtual queue with controlled-rate Admitter (converts 240,000 RPS burst to 200 RPS steady stream), and Redis Lua atomic decrement (guarantees zero oversell at sub-millisecond latency). It is an infrastructure design problem as much as a software design problem — pre-scaling, sale warming, SSE fan-out, and ClickHouse analytics distinguish it from every other problem in the e-commerce funnel.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2-4 sentences; show you know the domain)

**Q1: Why do you use SSR (server-side rendering) for the product page instead of client-side rendering?**
**KEY PHRASE: "SEO crawlability and Google Core Web Vitals"**
SSR produces fully-rendered HTML that Google, Bing, and other search crawlers can index without executing JavaScript. For e-commerce, organic search traffic is enormous — a product page that's invisible to search engines loses massive revenue. SSR also improves First Contentful Paint (FCP) and Largest Contentful Paint (LCP) — Google's Core Web Vitals metrics that directly affect search ranking. The cost is higher server-side compute, mitigated by caching the rendered HTML.

**Q2: What database do you use for the shopping cart, and why not just use Redis?**
**KEY PHRASE: "Redis is a cache, not a source of truth"**
DynamoDB is the primary store because it provides durable, strongly-consistent reads (`ConsistentRead=true`) and atomic conditional writes — critical for a system where a lost cart item is a lost sale. Redis is a read-through cache in front of DynamoDB, reducing latency for cart reads but not relied on for durability. Redis with no persistence would lose all carts on a node failure; Redis with persistence is still not designed for transactional multi-item writes. DynamoDB Global Tables also handle multi-region active-active replication, which Redis Cluster does not.

**Q3: How does the flash sale prevent oversell?**
**KEY PHRASE: "Redis Lua script atomic execution"**
A Redis Lua script executes atomically — no other command can interleave during script execution because Redis is single-threaded for command processing. The script checks the remaining inventory counter, and only if it's positive does it decrement. Two concurrent calls cannot both see remaining > 0 and both succeed when only 1 unit is left. The Lua guard (`if remaining < qty then return -1`) ensures the counter never goes negative. Compensation logic (INCR on post-decrement Aurora write failure) + a 5-minute reconciliation job handle edge cases.

**Q4: How do you handle price changes between when a user adds an item to their cart and when they check out?**
**KEY PHRASE: "lazy recalculation with explicit price change warning"**
The cart stores a `price_snapshot_cents` at add time and compares it against the current price (from Pricing Service, fetched on every GET /cart) on each cart view. If the price dropped, the current lower price is shown automatically. If it rose, a `price_changed: true` flag and a `price_warnings` array are returned so the UI can show a banner. At checkout, the Order Service performs a final authoritative price read against Aurora (not Redis), so the charged price is always correct. The user must acknowledge any price increase before completing checkout.

**Q5: What is the buy-box, and how do you select the winner?**
**KEY PHRASE: "weighted scoring algorithm with debounce"**
The buy-box is the featured "Add to Cart" button on a multi-seller product listing — only one seller wins it at a time. The buy-box selector runs a weighted scoring algorithm: `score = (1/price)×0.40 + FBA_bonus×0.30 + (seller_rating/100)×0.20 + in_stock×0.10`, where FBA_bonus is 1 for Amazon-fulfilled, 0.7 otherwise. The selector is triggered on each `price-events` Kafka message, but uses a 500 ms debounce window to collapse burst price changes (competitive repricing events can fire hundreds of updates/second for the same ASIN). Results are cached in Redis with a 30-second TTL fallback.

**Q6: Why does the flash sale use a virtual queue instead of just rate-limiting incoming requests?**
**KEY PHRASE: "fairness and thundering herd prevention"**
Rate limiting (returning 429 to excess requests) is unfair — users who get rejected will immediately retry, creating a thundering herd of retries that can still overwhelm the system, and users with better network latency or faster retry logic win unfairly. A virtual queue converts the burst into a fair FCFS stream: earlier arrivals get lower queue positions regardless of retry behavior. It also controls admission at a precise rate (200 users/second), allowing the Purchase Service to be right-sized to that rate rather than provisioned for the full burst. Users get a meaningful "estimated wait time," which reduces frustration and retry behavior.

**Q7: How do you ensure cart merge is idempotent when a user logs in simultaneously on two devices?**
**KEY PHRASE: "three-layer idempotency: SETNX + distributed lock + TransactWriteItems"**
Three layers work together. First, `Redis SETNX merge:done:{user_id}:{session_id} "1" EX 86400` — the fastest check; if already set, return the cached result immediately. Second, `Redis SET lock:cart:{user_id} {request_id} NX EX 30` — a distributed lock that serializes concurrent merge attempts; the second concurrent request waits up to 5 seconds for the lock. Third, `DynamoDB TransactWriteItems` with a conditional expression on the cart version — all-or-nothing write that prevents partial merges. After the first merge succeeds and sets the idempotency key, the second device's request finds the key and returns the original response without re-executing.

---

### Tier 2 — Deep Dive Questions (require explanation of why + trade-offs)

**Q1: The product page fans out to 8 services. What happens when one of them is consistently slow at P99?**
**KEY PHRASE: "per-service timeout + circuit breaker + graceful degradation"**
Each service has its own timeout (Catalog 100 ms, Pricing 50 ms, Inventory 50 ms, Reviews 80 ms). A slow P99 on Reviews (say, 200 ms) means that 1% of page loads would be blocked waiting for reviews, pushing the overall P99 above 200 ms. The circuit breaker monitors the failure rate and slow-call rate in a 10-second sliding window. When slow-call rate (calls exceeding timeout) exceeds 80%, the circuit opens — subsequent requests return the fallback immediately (0 ms) without waiting for the slow service. The page degrades gracefully: instead of rendering the top 3 reviews, it renders "Reviews loading..." with a client-side lazy load. The alert fires to the Reviews team. Trade-off: users on the degraded path don't see reviews immediately — acceptable for a below-fold, non-critical widget. For pricing (critical path), the fallback uses the last-cached Redis price, which is never older than 30 seconds.

**Q2: How do you prevent a cache stampede when a popular ASIN's CDN cache expires right before a traffic peak?**
**KEY PHRASE: "probabilistic early expiration (XFetch / PER)"**
When the remaining TTL drops below 20% of the original, a random 10% of requests are chosen to trigger a background cache refresh. These requests fetch from the origin (Aurora → Redis → CDN), but do not wait for the result — they serve the slightly stale value immediately. This spreads the recomputation load over a window rather than concentrating it at the exact expiry moment. Additionally, for the edge case where the cache has fully expired and multiple concurrent requests arrive simultaneously, we use a Redis `SETNX refresh_lock:{asin}` — only the first request to acquire the lock makes the origin call. All other concurrent requests either use the stale value (if `stale-while-revalidate` allows it) or wait briefly with a timeout fallback to direct Aurora read. The trade-off: the random 10% approach slightly increases origin load before expiry, but this is far better than the 100% stampede that occurs with simple TTL-only caching.

**Q3: The shopping cart uses DynamoDB ConsistentRead=true for read-your-writes. What is the cost, and when do you use eventual consistency instead?**
**KEY PHRASE: "conditional strongly-consistent reads to save RCU cost"**
DynamoDB consistent reads cost 2× the read capacity units of eventually-consistent reads. At 13,889 cart read RPS (peak), switching every read to consistent would double DynamoDB RCU costs. The strategy: the Cart Service tracks a per-user `last_write_sequence` in the session context. For the GET /cart that immediately follows a write (within 2 seconds of a `POST /cart/items`), `ConsistentRead=true` is set — the user just added something and must see it in the cart immediately. After 2 seconds (the typical DynamoDB propagation window), subsequent reads use `ConsistentRead=false` (eventual). This "conditional strongly-consistent read" gives the user correct feedback after their write while saving 70%+ of read capacity on non-write-following reads. Trade-off: there's a 2-second window where a read on a different device could return a slightly stale cart — acceptable for multi-device sync (the next poll refreshes).

**Q4: Why do you use Redis Sorted Set for soft holds instead of just storing hold records in DynamoDB with a TTL?**
**KEY PHRASE: "O(log N) expired-hold sweep before availability check"**
DynamoDB TTL does not expire items synchronously — AWS processes TTL deletions asynchronously and typically within 48 hours (not 15 minutes). If you rely on DynamoDB TTL for hold expiry, an abandoned hold might not release for hours, blocking other customers from seeing the item as available. Redis Sorted Set with score = expiry timestamp enables `ZREMRANGEBYSCORE hold:{sku} 0 {now}` — an O(log N) operation that atomically removes all expired holds before you count the remaining ones (`ZCARD`). This guarantees that your availability check always reflects the current reality. The full pattern — sweep expired + count remaining + add new hold — runs inside a single Lua script, making it atomic. Aurora stores durable hold records for audit and reconciliation; Redis is the operational view.

**Q5: How does the Admitter process in the flash sale protect against the Purchase Service being overwhelmed, and what happens if the Admitter itself crashes?**
**KEY PHRASE: "single-leader with Redis distributed lock + admission_cursor resumption"**
The Admitter is a single-leader process (not multiple, to avoid double-admission) that runs a tight loop: every 10 ms, advance the `admission_cursor:{sale_id}` by `(admission_rate × 0.01)` entries, and for each newly-admitted entry, set the queue token status to "admitted" in Redis. The admission rate is configurable per sale (e.g., 200/s). By admitting at exactly 200/s, the Admitter acts as a leaky bucket: no matter how fast users enter the queue, the Purchase Service only ever sees 200 RPS. If the Admitter crashes, its Redis distributed lock (`admitter_lock:{sale_id}`, TTL 30 s, renewed every 10 s) expires within 30 seconds. A standby Admitter pod acquires the lock and resumes from `admission_cursor:{sale_id}` — a durable Redis key tracking progress. Users in the queue see their estimated wait increase by up to 30 seconds. No queue positions are lost. Trade-off: 30 seconds of no admissions means 30 seconds × 200 users/s = 6,000 fewer users admitted — about 6% of a 10,000-unit sale's capacity during the failure window.

**Q6: How do you handle the price history widget at Amazon scale, given that you have 600M ASINs each with hourly price points for 2 years?**
**KEY PHRASE: "TimescaleDB hypertable with continuous aggregates and chunk drop policy"**
The raw price history table (600M ASINs × 8,760 hours × 2 years × 20 bytes) is ~105 TB — too large for a flat table. TimescaleDB's hypertable partitions data automatically by `recorded_at` (weekly chunks), making queries over any time range target only the relevant chunk partitions (not a full scan). Space partitioning by `asin_hash` prevents single-node hotspots for popular ASINs. For the price history widget (typically showing 90-day or 1-year charts), a continuous aggregate (`price_hourly_agg`) pre-materializes hourly rollups — queries hit the aggregate instead of raw data. Chunk drop policy auto-deletes data older than 2 years. Redis caches the 90-day chart response with a 5-minute TTL (price history changes only when price changes, which is infrequent at any given hour). Trade-off: TimescaleDB is better for operational queries with JOINs (e.g., "show price history for all items in user's cart") but slower than ClickHouse for pure analytics. For this use case (small response, few concurrent users viewing price history), TimescaleDB is correct.

**Q7: If the flash sale Redis inventory node fails mid-sale, how do you reconcile the inventory state afterward?**
**KEY PHRASE: "Reconciliation Job: total_inventory - remaining_in_redis vs. COUNT(confirmed purchases in Aurora)"**
During the 15-second Sentinel failover window, the Purchase Service returns 503 with `Retry-After: 15`. No purchases or decrements happen. After failover, the new primary (former replica) holds the last replicated inventory state. Due to async replication, the replica might be up to 1 second behind — meaning up to ~200 purchases (at 200 admitted/s) might not be reflected in the replica's count. The Reconciliation Job (scheduled every 5 minutes during an active sale) computes: `expected_remaining = total_inventory - COUNT(flash_purchases WHERE sale_id = X AND status != 'cancelled')`. It compares this against `inv:{sale_id}` in Redis. If Redis shows more remaining than Aurora (e.g., Redis says 1,000 but Aurora's purchase count implies 985 remaining), the job executes `SET inv:{sale_id} 985` to correct it. If Redis shows fewer remaining (phantom decrements without Aurora records), the job executes `INCR inv:{sale_id}` for the delta, and sends those users a "sorry, there was a technical issue" notice.

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud; no single right answer)

**Q1: Amazon is doing a global launch where the same product goes live simultaneously in 10 marketplaces (US, UK, DE, JP, etc.) at the same moment, with a total global inventory of 100,000 units. How would you architect the flash sale to handle cross-marketplace inventory allocation?**

This is a distributed systems problem at its hardest — globally partitioned inventory with a shared pool. Start by asking the clarifying question: "Is inventory globally shared (one pool across all marketplaces) or pre-allocated per marketplace (US gets 40K units, UK gets 15K, etc.)?"

Pre-allocated per marketplace is the sane approach: each marketplace gets its own `inv:{sale_id}:{marketplace_id}` Redis node, running its own Lua decrements independently. No cross-region coordination. If the US sells out, UK doesn't get extra units — but that's a business decision, not a technical failure.

Globally shared is the nightmare scenario. You cannot have a single Redis node for 100K units if that node must be reached from 10 global regions — cross-region network latency (50-200 ms) makes the Lua decrement path too slow for a 200 ms P99 target. The architectural options: (a) **Sharding with rebalancing** — pre-allocate 10K units per marketplace Redis node; if a marketplace sells out, its Admitter is notified to stop admission and the surplus is auctioned to the next-fastest region (complex rebalancing logic). (b) **Two-phase purchase** — each region runs a local "soft decrement" against a local counter, and asynchronously posts to a global arbiter; the arbiter finalizes or rejects within 1 second; the user sees "pending confirmation" briefly. (c) **Accept slight oversell and correct post-sale** — globally shared inventory with local counters, reconcile after the fact with refunds. Explain the trade-offs of each, recommend pre-allocation as the practical approach, and acknowledge the business need driving the question.

**Q2: Your CDN hit ratio for product pages is 90% at steady state, but you're observing that it drops to 40% during the first 30 seconds of a Prime Day flash deal launch. How would you investigate and fix this?**

A 40% CDN hit ratio at launch means 60% of requests miss the cache and hit origin — 6× the expected origin load. The question is: why is the cache cold at launch?

Investigation approach: (1) Check if the issue is a CDN cache key problem — are requests varying by parameters (user agent, Accept-Encoding, cookies) that should not affect the cache key? A misconfigured CDN cache key can fragment the cache across thousands of variants. (2) Check the cache-warmer — did the sale warming pipeline (T-5 minutes) execute correctly? Did it successfully prime the CDN edges for the deal ASIN? Check the CDN pre-warming synthetic requests. (3) Check for an invalidation storm — did the sale launch trigger a bulk CloudFront invalidation that wiped the cache for the deal ASINs right at T=0? A bulk invalidation just before T=0 would explain the cold cache at launch. (4) Check if the deal page is authenticated-user-only — authenticated requests bypass CDN entirely (because the Vary: Cookie header prevents sharing cached pages across user sessions). If the deal requires login and 90% of users are authenticated, there is no CDN cache for them.

Fix options: (1) Pre-warm CDN with a synthetic request from each PoP 5 minutes before launch. (2) Ensure the CDN cache key excludes irrelevant headers. (3) If the issue is authentication, implement fragment caching (shell + sidecar) so the shell is CDN-cached even for logged-in users. (4) For the 30-second window, accept it and pre-scale origin to 2× capacity specifically for the launch window.

**Q3: A major retailer wants to use your shopping cart system but requires that if a user adds an item to their cart and the price increases within the next 24 hours, the user is guaranteed to get the original lower price — a "price lock" feature. How would you implement this, and what are the system design implications?**

This is a business logic extension that fundamentally changes the consistency requirements. Currently, the cart is "advisory" — it stores price snapshots for display but always uses the current price at checkout. A price lock makes the price snapshot legally binding.

**Implementation approach:** Store a `price_lock_expires_at` on each line item (now + 24 hours when added). At checkout, the checkout flow reads the `price_lock_expires_at`. If `now < price_lock_expires_at`, use the `price_snapshot_cents`. If `now >= price_lock_expires_at`, use the current price. The Price Recalculator must be modified to respect this: when comparing snapshot vs. current price, if the lock is active and the price went up, keep the snapshot as the payable price (not just a display field). If the price dropped (even with a lock), always use the lower price.

**System design implications:** (1) The cart item record must store `price_lock_expires_at` — minor schema change. (2) The Checkout Service must now read the line item's `price_lock_expires_at` and conditionally choose between snapshot and current price — adds logic but no new infrastructure. (3) The Pricing Service must now honor price locks: when a seller increases their price, the Pricing Service needs to know which outstanding carts have locked that SKU. This is non-trivial — you need an index on `(sku, price_lock_expires_at > now)` across all active carts. (4) Financial exposure: if 10 million users lock a $100 item and the price rises to $150, you've committed $500M in subsidized price locks. You need a business analytics layer to estimate total price-lock liability in real-time. (5) Inventory interaction: what if the item goes out of stock during the 24-hour lock window? The lock was on price, not stock. At checkout, the user still needs stock — if unavailable, the lock is irrelevant. Clarify whether the feature is "price lock" only or "price and inventory reservation." The latter is much harder (requires 24-hour inventory holds — much longer than the current 15-minute holds).

---

## STEP 7 — MNEMONICS

### Memory Trick: "CATCH" for the five failure modes you must cover in any e-commerce interview

- **C** — Cache invalidation (what happens when the cache is stale or stampedes?)
- **A** — Atomicity (how do you prevent double-sell at the exact moment of purchase?)
- **T** — Traffic shaping (how do you handle the burst — whether Prime Day or flash sale?)
- **C** — Consistency model (which operations require strong consistency vs. eventual?)
- **H** — High availability (what fails, what degrades gracefully, what pages SRE?)

Cover all five proactively in your design and you will score well across all three problems.

---

### Memory Trick: "SQUID" for the sequential layers of the flash sale architecture

- **S** — Shield (WAF + CDN absorbs DDoS, blocks bots at edge)
- **Q** — Queue (Redis Sorted Set, FCFS by epoch microseconds, ZADD NX)
- **U** — Unblock (Admitter drains at 200/s, distributing lock, single leader)
- **I** — Inventory (Redis Lua atomic DECRBY, never goes negative, compensate on failure)
- **D** — Durable write (Aurora INSERT, UNIQUE constraint as safety net)

---

### Opening one-liner for Amazon Product Page:
"This is a parallel fan-out assembly problem under a hard 200 ms latency constraint, with the entire caching strategy pivoting on whether the user is anonymous or authenticated."

### Opening one-liner for Shopping Cart:
"The cart is the most stateful thing in e-commerce — it has to survive browser sessions, device switches, and login events while keeping prices fresh, inventory reserved, and writes idempotent."

### Opening one-liner for Flash Sale:
"Flash sales are the only system where you know exactly when your worst traffic arrives, which means pre-scaling beats autoscaling, and the only way to guarantee zero oversell at 240K RPS is to never let 240K requests reach the inventory counter in the first place."

---

## STEP 8 — CRITIQUE

### What the Source Material Covers Well

**Exceptional depth:**
- Redis Lua atomic inventory decrement: the Lua script, return codes (-2/-1/N), compensation on failure, the reconciliation job, and the Sentinel failover implications are all thoroughly covered.
- Cart merge idempotency: the three-layer approach (SETNX + distributed lock + TransactWriteItems), the B2B large-cart batching problem, and the orphaned soft hold cleanup are detailed and production-accurate.
- PDP parallel fan-out: the per-service timeout configuration (Catalog 100ms, Pricing 50ms, etc.), per-service fallbacks, circuit breaker configuration, and the fragment caching (shell + sidecar) pattern are all solid.
- Flash sale virtual queue: Sorted Set choice vs. SQS/Kafka, O(log N) ZRANK, Admitter single-leader pattern, abandoned user cleanup, and queue sharding for hyper-scale are all addressed.
- Data models: the SQL schemas (Aurora), DynamoDB table designs (GSIs, TTLs, ConsistentRead), Cassandra schemas (partition key choices, clustering keys), and Redis data structures (Sorted Set, HASH, String SETNX) are realistic and well-justified.

**Good coverage:**
- Database selection justifications (why Cassandra for reviews, why TimescaleDB for price history, why DynamoDB for carts)
- Idempotency patterns (two-layer: Redis fast path + DB UNIQUE constraint)
- Failure scenario tables with detection, mitigation, and recovery steps
- Monitoring and alerting (specific metrics, thresholds, and alert routing)

### What Is Shallow or Missing

**Missing — Multi-region conflict resolution for shopping cart writes:**
DynamoDB Global Tables uses last-writer-wins (timestamp-based) for conflicting writes. The source material mentions this but does not explore what happens when a user in the US and the same user in EU simultaneously update the same cart item (e.g., one removes it, one changes its quantity). LWW means one of these writes silently disappears. A real production system would need to either: (a) route all writes for a user_id to their home region (sticky routing) and use Global Tables only for failover reads, or (b) implement a custom merge function (compare-and-swap on version number). This gap would be probed in a Staff-level interview.

**Missing — Kafka consumer at-exactly-once semantics:**
The source material says Kafka delivers "at-least-once" and handles idempotency at the application level (e.g., bloom filter for review events, SETNX for flash sale purchase events). But it does not discuss Kafka transactions or the `enable.idempotence=true` producer config. An interviewer who works with Kafka may ask about this.

**Shallow — Review fraud and moderation pipeline:**
The source material mentions fraud detection as a note (ML model running asynchronously, can flip status from `published` to `removed`, triggers compensating Kafka event to decrement ratings). But there is no architecture for the moderation queue (how do flagged reviews get to human reviewers?), the ML model training pipeline (how does it consume events from ClickHouse/S3?), or the appeal process. This is fine for a system design interview (it is out of scope) but worth noting if the interviewer asks "how would you extend this?"

**Shallow — Tax calculation for the shopping cart:**
The source material simply says "tax is handled by an external Tax Service call" without any detail on how jurisdictional tax (US state tax, EU VAT, etc.) is computed, whether it is exact at cart time or estimated, and how it reconciles at checkout. This is a surprisingly common interview extension question ("how do you handle sales tax in all 50 US states?").

**Missing — Payment retry and exactly-once charging for flash sale:**
The flash sale purchase flow calls the payment gateway, but the source material does not address what happens if the payment call times out (charge might or might not have happened) or if the network fails after payment succeeds but before the Aurora `flash_purchases` record is written. The classic distributed payment problem: you charged the card but lost the order record. A production system handles this with a `payment_intent_id` (Stripe's idempotency key) and a separate reconciliation against the payment gateway. Senior interviewers will probe this.

### Senior Probe Questions — Things Interviewers Will Dig Into

1. "You mentioned the cart uses DynamoDB Global Tables with last-writer-wins. Walk me through a concrete conflict scenario and whether LWW is actually correct for a shopping cart."

2. "Your flash sale Redis inventory node is a SPOF. You mentioned bucket sharding for hyper-scale. But even with N buckets, each individual bucket is a SPOF. How do you handle a bucket failure mid-sale without underselling or overselling?"

3. "The buy-box algorithm weights are stored in a config service. If you change the weights during a sale, the buy-box winners flip for many ASINs simultaneously, causing a mass price update event storm. How do you deploy weight changes safely?"

4. "You use Cassandra for reviews with QUORUM writes. What happens during a Cassandra node replacement — how do you ensure reviews submitted during the nodetool removenode operation are not lost, and how long does the inconsistency window last?"

5. "The pre-sale countdown page is cached at CDN with a 10-second TTL. At T=0, you need to transition from showing the countdown to showing the 'Buy Now' button. How do you ensure all 500,000 waiting users see the 'Buy Now' button within 2 seconds of sale start, not 10 seconds?"

### Common Traps That Candidates Fall Into

**Trap 1: Proposing a global distributed lock for inventory decrement.** Distributed locks (Zookeeper, Redis Redlock) have coordination overhead (2-10 ms per operation) and don't scale to 240K RPS. The correct answer is Redis single-threaded Lua on one node — no coordination needed.

**Trap 2: Caching the entire product page at CDN without accounting for personalization.** If you cache the full authenticated user's page at CDN, all users see each other's Prime eligibility, wishlist status, and purchase badges. The answer is fragment caching (shell + sidecar), not full-page caching.

**Trap 3: Using SELECT FOR UPDATE on the inventory table for cart holds.** This is correct but doesn't scale — a single popular SKU becomes a lock hotspot. Every "add to cart" for that SKU serializes. The answer is Redis atomic operations (DECRBY inside Lua) for the display path, with Aurora only for the hard checkout lock.

**Trap 4: Ignoring the guest cart lifecycle in shopping cart design.** Many candidates design the cart as if all users are authenticated and miss the session service, 30-day TTL, merge algorithm, and the idempotency complexity. Ask yourself: "what happens before the user is logged in?" at the start of every cart design.

**Trap 5: Treating the flash sale bot problem as solvable at a single layer.** A candidate who says "just add a CAPTCHA" is demonstrating shallow thinking. The correct answer is layered defenses: WAF at edge (IP/ASN blocks), reCAPTCHA v3 (score-based, non-intrusive), account age check, device fingerprinting, behavioral ML, per-user queue limit, and post-purchase address de-duplication. Acknowledge that a professional scalper with 10,000 aged accounts cannot be stopped entirely in real-time — post-sale ML analysis and address de-duplication are the final backstops.

**Trap 6: Relying on Kafka for exactly-once semantics without explaining the application-level idempotency.** Kafka's `exactly_once` setting is for Kafka Streams applications with transactional producers and consumers. For most consumer patterns (consume from Kafka, write to database), you need to implement idempotency at the application level — either with `INSERT ON CONFLICT DO NOTHING` or with a Redis SETNX dedup key. Saying "Kafka guarantees exactly-once delivery" without this qualifier is inaccurate.

---

*End of Pattern 9: E-Commerce Interview Guide*
