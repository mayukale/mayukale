# Problem-Specific Design — E-Commerce (09_ecommerce)

## Amazon Product Page (PDP)

### Unique Functional Requirements
- Parallel assembly of 8 independent data sources into one SSR HTML response: product catalog, pricing, inventory, reviews, Q&A, recommendations, seller info, A+ content
- Buy-Box winner selection: multi-seller products have one "best offer" shown prominently, selected by weighted algorithm
- Price history API: time-series of price over 2 years for deal verification
- Three types of recommendation widgets: "Frequently Bought Together," "Customers Who Bought This Also Bought," "Similar Items" (+ "Sponsored Products" via separate ads path)
- Review features: 1–5 star aggregate histogram, verified-purchase badge, image/video attachments, helpfulness voting, paginated cursor-based retrieval
- A+ content: rich HTML/media blobs (50 KB avg) for enhanced brand listings
- Low-stock warning when qty ≤ 10 units (display-level, not authoritative)

### Unique Components / Services
- **PDP SSR Service**: Node.js / React server-side rendering; 500 pods per region (3 regions = 150K RPS capacity); fans out to 8 microservices in parallel with per-service timeouts (Catalog 100 ms, Pricing 50 ms, Reviews 80 ms, etc.); assembles cacheable shell (pricing, catalog, reviews — same for all users) + personalization sidecar (Prime eligibility, wishlist state, purchase badges — fetched client-side via XHR keyed on session cookie)
- **Pricing Service**: Real-time prices from Aurora (authoritative) + Redis (L2, 1–5 s TTL); 200 stateless pods; debounce 500 ms window on buy-box recomputation to collapse burst price changes; in-memory LRU cache (2 s) reduces Aurora load by ~80%
- **Price History Service**: TimescaleDB with weekly hypertable chunks partitioned by recorded_at; space-partitioned by asin_hash; 2-year retention via drop-chunk policy; continuous aggregates for hourly rollups; 600M ASINs × 8,760 hrs × 2 yrs = ~105 TB
- **Reviews Service** (Go microservice): backed by Cassandra; write-heavy (millions/day); QUORUM writes, LOCAL_ONE reads; partition key (asin, marketplace_id), clustering by created_at DESC; RF=3, 12-node cluster
- **Rating Aggregator** (Kafka consumer): reads `review-events`; atomic SQL UPDATE: `total_reviews + 1, sum_ratings + ?, average_rating = sum_ratings/total_reviews`; additive → order-independent → safe parallel consumption; nightly batch reconciliation from raw Cassandra to Aurora
- **Buy-Box Selector**: triggered on `price-events`; debounced 500 ms; weighted score: `(1/price)×0.40 + FBA_bonus×0.30 + (seller_rating/100)×0.20 + in_stock×0.10`; weights stored in config service (A/B testable per marketplace); tie-breaker = seller_id (deterministic)

### Unique Data Model
- **DynamoDB** (product catalog): partition key = ASIN; 600M ASINs × 10 KB = 6 TB; Q&A: partition key = (asin, marketplace_id), GSI on helpful_count for ranking; no TTL on catalog; on-demand capacity
- **Cassandra** (reviews): (asin, marketplace_id) partition, created_at DESC clustering, wide-row; RF=3, QUORUM write, LOCAL_ONE read; 5B reviews × 2 KB = 10 TB; `voted_users:{review_id}` Redis Set with 90-day TTL for double-vote prevention
- **Aurora MySQL** (pricing): `prices` table keyed by (asin, marketplace_id, seller_id); `rating_aggregates` keyed by (asin, marketplace_id); 15 read replicas
- **TimescaleDB** (price history): `price_snapshots` hypertable, weekly chunks, space-partitioned by asin_hash; 2-year drop policy; continuous aggregate `price_hourly_agg`
- **Redis** (L2 cache + L1 fallback): L1 = 256 MB/pod in-process LRU (70% hit rate for top 1M ASINs, 2 s TTL); L2 = Redis Cluster 32 primary nodes, 96 TB total capacity; hash slot partitioning (16,384 slots); Sentinel failover 15 s RTO
- **S3**: product images (9 PB), review media, A+ HTML; immutable content-hash URLs; CDN TTL 1 year

### Unique Scalability / Design Decisions
- **Fragment caching (shell + sidecar)**: cacheable shell (catalog, pricing, reviews = same for all users on the same ASIN) cached at CDN 30 s TTL; personalization sidecar (Prime, wishlist, purchase badges) fetched client-side XHR — eliminates the need to vary CDN cache by user; 80–90% traffic served by CDN without personalization blocking
- **Probabilistic early expiration (PER/XFetch)**: when Redis TTL < 20% remaining, random 10% of requests refresh in background (non-blocking); Redis SETNX lock prevents thundering herd — only one process fetches from Aurora; others use stale value or wait; applicable to all 30-second TTL keys
- **TimescaleDB over InfluxDB/ClickHouse**: hypertable auto-partitioning by time + PostgreSQL SQL compatibility for complex analytics queries; continuous aggregates for hourly rollups; chunk drop for retention — ClickHouse would be faster for analytics at extreme scale but TimescaleDB better for operational queries with JOINs
- **Cassandra for reviews over DynamoDB**: ASIN partition key maps to wide-row time-series access pattern (paginated sorted by created_at); write-heavy (millions/day); QUORUM writes + LOCAL_ONE reads allows asymmetric consistency (write durable, read fast); DynamoDB scan would be O(N) for time-sorted review retrieval
- **Circuit breaker per service (not global)**: each of the 8 widgets has its own SLA and degradation mode; hiding Q&A on Q&A Service failure (non-critical) vs. showing stale pricing (critical); global circuit breaker would make all or nothing

### Key Differentiator
Amazon Product Page's uniqueness is its **parallel fan-out to 8 services with fragment caching**: no other problem in this folder assembles 8 independent microservice responses in a single SSR HTML response under 200 ms p99, requiring per-service timeouts, fragment-level cache splitting (shell vs. personalization sidecar), and probabilistic cache refresh to prevent stampede on 115K RPS across 600M ASINs.

---

## Shopping Cart

### Unique Functional Requirements
- Guest carts (pre-login): server-side carts tied to a session token (not localStorage); survives browser clearing; 30-day TTL
- Cart merge on login: when a guest logs in, merge their guest cart into their authenticated cart; idempotent on duplicate login events
- Soft inventory holds: temporarily reserve units while items are in cart (15-min hold) to prevent other shoppers from purchasing the last units
- Coupon/promo application with usage limits (per-code, per-user, per-campaign)
- Lazy price recalculation: prices recomputed on cart read (not on every item add); cached 60 s to avoid repeated Pricing Service calls
- Price staleness disclosure: if cached price is ≥ 60 s old, re-validate; if price changed, display warning banner before checkout

### Unique Components / Services
- **Cart Manager (within Cart Service)**: CRUD for cart items; cart merge logic; idempotency via Redis SETNX lock + merge-done key; DynamoDB TransactWriteItems for atomic multi-item write (25-item batch limit → sequential batching for large carts)
- **Price Recalculator**: lazy on-demand computation when GET /cart called; calls Pricing Service per SKU (cached 30 s in Redis per SKU); caches computed total in `cart:calc:{cart_id}` (60 s TTL); fallback to snapshot price from cart metadata on Pricing Service failure
- **Inventory Advisor**: places and releases soft holds; Redis Sorted Set `hold:{sku}` (score=expires_at); Aurora durable hold record; Lua atomic script sweeps expired holds via ZREMRANGEBYSCORE before checking availability; releases on cart clear, checkout, or inactivity > 20 min
- **Coupon/Promo Service**: validates code eligibility; applies optimistic lock: `UPDATE coupons SET usage_count = usage_count + 1 WHERE coupon_id = ? AND usage_count < max_uses` (no SELECT FOR UPDATE — avoids contention); rare last-code race condition accepted for non-monetary coupons
- **Session Service**: issues guest session tokens; maps session_id → guest cart_id; 30-day TTL; enables browser-agnostic cart persistence

### Unique Data Model
- **DynamoDB** (carts): partition key = cart_id (UUID); ConsistentRead=true for read-your-writes; TTL on guest cart items (auto-expire at session TTL); Global Tables active-active 3 regions (us-east-1, eu-west-1, ap-southeast-1); last-writer-wins (LWW timestamp-based) conflict resolution; RPO = seconds, RTO = 0; on-demand capacity mode
- **Aurora MySQL** (coupons): ~10M codes; `coupon_id, usage_count, max_uses, expires_at, eligibility_rules_json`; UNIQUE (user_id, coupon_id) for per-user-once codes; Multi-AZ + 2 read replicas; future sharding via Vitess if 100×
- **Redis** (cart ops): `cart:calc:{cart_id}` Hash (60 s TTL); `hold:{sku}` Sorted Set (score=expires_at, 15-min entries); `lock:cart:{user_id}` String (NX EX 30 — distributed lock for merge); `merge:done:{user_id}:{session_id}` String (NX EX 86400 — idempotency); session mapping (`sess:{session_id}` → cart_id, 30-day TTL); 6-shard cluster (3 primary + 3 replica), 384 GB total
- **S3 + Athena**: cart events as Parquet via Kinesis Firehose; queryable for ML training and fraud detection

### Unique Scalability / Design Decisions
- **DynamoDB Global Tables (active-active multi-region)**: carts are globally distributed; a user traveling to a different region should see their cart; LWW conflict resolution acceptable (last add/remove wins); RPO = seconds (near-zero from DynamoDB sync replication within region); RTO = 0 (region failover automatic)
- **Cart merge idempotency (SETNX + lock + TransactWriteItems)**: concurrent logins (e.g., auto-refresh on two devices) must not double-merge; three-layer safety: (1) Redis SETNX `merge:done` (EX 86400) — fast idempotency check; (2) Redis distributed lock (NX EX 30) — serializes concurrent merge attempts; (3) DynamoDB TransactWriteItems — atomic multi-item conditional write
- **Lazy price recalculation over eager updates**: eager updates (recalculate on every item add) require a Pricing Service call per add — high latency and load; lazy recomputation on GET /cart amortizes cost; 60 s staleness acceptable (price always verified at checkout with fresh read)
- **Optimistic coupon lock over SELECT FOR UPDATE**: at high traffic (694 RPS peak), pessimistic locking creates queue contention; UPDATE WHERE usage_count < max_uses fails atomically if last code is already used (returns 0 rows); accepted trade-off: one over-issue per campaign possible if two requests arrive simultaneously for the last code (tolerable for discount codes, NOT for gift cards which need exact-once)

### Key Differentiator
Shopping Cart's uniqueness is its **guest-to-authenticated cart lifecycle**: managing session-keyed guest carts, idempotent merge on login (three-layer: SETNX + lock + DynamoDB transaction), and soft inventory holds (Redis Sorted Set + Aurora audit) while remaining globally consistent via DynamoDB Global Tables — no other problem in this folder has the pre-auth → post-auth state transition complexity.

---

## Flash Sale

### Unique Functional Requirements
- Sale starts at a precise timestamp; hundreds of thousands of users attempt to purchase simultaneously within the first seconds
- Inventory is strictly limited (e.g., 10,000 units); oversell is a hard failure (regulatory/revenue impact)
- Per-user purchase limit (e.g., 1 unit per user per sale)
- Bot prevention before queue entry
- FCFS fairness: earlier queue entry → higher chance of purchasing
- Real-time inventory countdown displayed to all waiting users (SSE-based)
- Pre-sale countdown page must not load origin servers

### Unique Components / Services
- **Bot Filter / Token Gate**: CAPTCHA (reCAPTCHA), account age check (≥ 30 days), behavioral anomaly scoring (new accounts need extra verification); issues signed sale token (JWT, valid 5 min); fallback: if Google API timeout > 5 s → bypass CAPTCHA, account-age-only check; 50 pods pre-scaled (throughput limited by reCAPTCHA API rate at 1 M/day paid tier)
- **Queue Service**: virtual waiting queue via Redis Sorted Set (score=epoch_microseconds, member=queue_token_jti); `ZADD NX` prevents duplicate entries; `ZRANK` gives O(log N) position; `ZRANGE` for ordered drain; stateless, 100 pods; ~2,400 RPS/pod at 240K enqueue burst
- **Admitter Process**: single leader controlled by Redis distributed lock; drains queue at 200 users/s (configurable); 3-pod failover (2 standbys — Redis lock TTL expires in 30 s → standby acquires); protects Purchase Service from thundering herd
- **Purchase Service**: validates queue token (SETNX `sale_token_used:{jti}`); checks per-user purchase limit; calls Redis Lua for atomic inventory decrement; writes purchase record to Aurora; publishes to Kafka; 200 stateless pods; admission-rate-limited at 200/s (Admitter is the bottleneck, not pod count)
- **Inventory SSE Publisher**: consumes Kafka `inventory-events`; pushes real-time inventory count to waiting users via SSE; 50 pods × 10,000 SSE connections/pod = 500,000 total simultaneous connections
- **Reconciliation Job**: every 5 min compares `total_inventory - GETRANGE(inv:{sale_id})` vs. `COUNT(purchases) in Aurora`; auto-corrects discrepancies ≥ 1 (e.g., compensating INCR if a purchase was rolled back without restoring Redis counter)
- **ClickHouse** (analytics): real-time sale analytics and bot detection ML training; sharded by sale_id across 3-node cluster

### Unique Data Model
- **Redis Lua atomic inventory decrement**:
  ```lua
  local remaining = tonumber(redis.call('GET', KEYS[1]))
  if remaining == nil then return -2 end      -- key not initialized
  if remaining < tonumber(ARGV[1]) then return -1 end  -- sold out
  return redis.call('DECRBY', KEYS[1], ARGV[1])
  ```
  - Single Redis node per sale; Lua script is atomic (single-threaded execution); no distributed coordination needed; < 1 ms per call
  - Compensating INCR on post-decrement failure (Aurora write fails)
- **Redis Sorted Set (queue)**: `queue:{sale_id}`; score = epoch_microseconds (FCFS ordering); member = queue_token_jti; ZADD NX prevents duplicate entries; ~200 B/entry, 1M members = 200 MB; single primary + 1 replica per sale; Sentinel failover 15 s; reconstructible from Kafka 7-day replay
- **Aurora MySQL** (flash_sales, purchases): flash_sales = sale_id, product_id, total_inventory, start_at, end_at, status; purchases = purchase_id, sale_id, user_id, quantity, purchased_at; UNIQUE (user_id, sale_id) = double-purchase prevention at DB level; handled < 2,000 writes/s without sharding
- **Bucket sharding (optional, hyper-scale)**: split `total_inventory` across N Redis nodes (`inv:{sale_id}:0` to `inv:{sale_id}:N-1`), each holding `total_inventory/N`; Lua atomic per shard; `DECRBY` targets a random bucket; `inv_total:{sale_id}` checked for sold-out detection; eliminates single-node SPOF

### Unique Scalability / Design Decisions
- **Single-node Redis Lua for inventory (over distributed atomic counter)**: single-threaded Lua execution eliminates race conditions without distributed locking overhead; < 1 ms per decrement; for hyper-scale > 1M concurrent users, bucket sharding splits across N Redis nodes with each shard independently atomic — still no cross-node coordination needed
- **Admission queue rate-limited at 200/s (over direct rate-limiting + retry)**: direct rate-limiting creates thundering herd (all rejected users immediately retry); queue provides FCFS fairness (earlier arrival → guaranteed slot in order); predictable 200/s admission rate allows Purchase Service sizing at exactly 200 pods handling 200 writes/s — dramatically simpler to right-size
- **Pre-scaling 5 minutes before sale start**: Kubernetes HPA has 60–120 s lag; pre-sale traffic spike lasts < 5 min; if relying on HPA, half the sale could be at reduced capacity; pre-scale ensures all pods ready at T=0 for ~$0.50 cost (negligible vs. sale revenue)
- **SSE over WebSocket for inventory countdown**: SSE is unidirectional (server → client), simpler to implement, auto-reconnects on network interruption, HTTP/2 multiplexed; WebSocket adds unnecessary bidirectional complexity when clients only need to receive inventory count updates

### Key Differentiator
Flash Sale's uniqueness is its **queue-based demand shaping + Redis Lua atomic inventory**: the virtual queue (Redis Sorted Set, 200/s admission, Admitter single leader) converts a thundering herd into a steady 200/s stream; the Redis Lua atomic decrement provides oversell-proof inventory control at < 1 ms per operation; combined with bot filtering (CAPTCHA + account age + behavioral score → signed JWT) before queue entry — this three-layer architecture (bot filter → queue → atomic purchase) is purpose-built for a use case that no other problem in this folder faces: a known future traffic cliff edge where 100,000+ users arrive simultaneously for a strictly limited resource.
