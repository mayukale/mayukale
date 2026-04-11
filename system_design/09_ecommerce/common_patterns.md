# Common Patterns — E-Commerce (09_ecommerce)

## Common Components

### Redis for Hot-Path Caching with Short TTL
- All three use Redis for frequently-read mutable data that changes faster than CDN TTLs allow
- In amazon_product_page: prices (1–5 s TTL), inventory display counts (30 s TTL), rating aggregates (60 s TTL), recommendations (5 s TTL); 32-primary Redis Cluster with 96 TB capacity (L2 cache)
- In shopping_cart: `cart:calc:{cart_id}` (60 s TTL for computed totals), per-SKU price cache from Pricing Service (30 s TTL), session → cart mapping (30-day TTL), soft hold Sorted Set (`hold:{sku}`, score=expires_at, 15-min TTL)
- In flash_sale: inventory counter (single-node, Lua atomic DECRBY), queue Sorted Set per sale (score=epoch_microseconds, ~200 B/entry, 1 M entries = 200 MB), sold-out detection

### Kafka for Async Event Streaming and Cache Invalidation
- All three use Kafka for decoupling write events from downstream consumers (cache invalidators, analytics, order processing)
- In amazon_product_page: `price-events` (578 RPS, by asin % 64), `inventory-events` (2,314 RPS, by asin), `review-events` (millions/day), `catalog-updated` (event-driven cache invalidation → DEL Redis keys + CloudFront purge); RF=3, 64–256 partitions per topic
- In shopping_cart: `cart-events` (cart CRUD, price changes, coupon applications), `inventory-hold-events` (soft hold placement/release); RF=3; consumers: Cart Event Logger → Parquet → S3 via Kinesis Firehose
- In flash_sale: `purchase-events` (purchase confirmations), `inventory-events` (decrements, compensations), `queue-events` (queue operations for analytics); RF=3, 8 partitions (scalable to 64); queue reconstructable from Kafka 7-day replay on data loss

### Aurora MySQL for ACID Transactional Data
- All three use Aurora MySQL as the durable, strongly-consistent store for critical business entities requiring ACID guarantees
- In amazon_product_page: pricing (authoritative, mirrored to Redis), sellers (~5M globally), rating_aggregates; 15 read replicas; Multi-AZ sync (3 AZs); cross-region async RPO ≤ 1 s
- In shopping_cart: coupons (~10M codes); UNIQUE constraint prevents over-issuance; Multi-AZ + 2 read replicas; future sharding via Vitess if 100×
- In flash_sale: flash_sales table (low cardinality), purchases (ACID, UNIQUE on (user_id, sale_id) as DB safety net); Multi-AZ + 2 read replicas; handles 2,000 writes/s at 200 admitted/s × 10 concurrent sales

### Idempotency Enforcement (Redis SETNX + DB UNIQUE Constraints)
- All three combine Redis SETNX for in-flight dedup with database UNIQUE constraints as durable safety net
- In amazon_product_page: `SADD voted_users:{review_id} {user_id}` (90-day expiration) prevents double helpful-vote; Bloom filter on Kafka consumer for event dedup (consumer group lag alert >10K events)
- In shopping_cart: `SET lock:cart:{user_id} {request_id} NX EX 30` (distributed lock); `SETNX merge:done:{user_id}:{session_id} EX 86400` prevents duplicate cart merge on concurrent logins
- In flash_sale: `SETNX sale_token_used:{jti}` before deducting inventory (prevents replay attacks); UNIQUE (user_id, sale_id) in Aurora as database-level double-purchase prevention

### S3 for Large Blob / Media Storage
- All three use S3 for durable storage of large files and analytics archives
- In amazon_product_page: product images (9 PB, original 3 MB × 600M × 5 variants), review images/videos (~500 KB, 4% of reviews), A+ content HTML blobs (~50 KB, 30% of ASINs); immutable content hash in URL; CDN TTL 1 year for images
- In shopping_cart: cart event Parquet archives via Kinesis Firehose → S3; queryable by Athena for ML training and fraud detection
- In flash_sale: purchase event logs and analytics archive (implicitly via Kafka → S3 pipeline for ClickHouse replication)

### CDN for Public/Cacheable HTML and Static Assets
- All three use CDN to absorb read load for non-personalized content
- In amazon_product_page: CloudFront/Akamai; SSR HTML 30 s TTL (anon), static assets 86400 s TTL; 80–90% traffic offload; event-driven invalidation via Kafka `catalog-updated` + CloudFront PURGE
- In shopping_cart: not primary (cart data is user-personalized); CDN for checkout page static assets
- In flash_sale: CloudFront + WAF + Shield; pre-sale countdown page served with 99% CDN cache hit; DDoS protection + per-IP rate limiting at edge; WAF blocks bot signatures

### Inventory Reservation (Soft Hold → Hard Decrement)
- All three implement a two-phase inventory reservation pattern: soft hold (reversible, TTL-bound) → hard decrement (at checkout/purchase)
- In amazon_product_page: Redis display-level soft reservations show reduced inventory to other shoppers; authoritative inventory in Aurora for checkout
- In shopping_cart: Redis Sorted Set `hold:{sku}` (score = expires_at, 15-min TTL); Lua atomic script for TOCTOU safety; Aurora durable hold records for reconciliation; `ZREMRANGEBYSCORE hold:{sku} 0 {now}` sweeps expired holds (O(log N)); released on item removal, checkout, or cart inactivity > 20 min
- In flash_sale: hard atomic decrement at purchase time via Redis Lua (no soft phase — queue admission acts as the soft reservation); compensating INCR on post-decrement failure

### Circuit Breakers + Graceful Degradation
- All three implement fallback behavior when downstream services are unavailable
- In amazon_product_page: per-service timeout circuit breaker (sliding 10 s window, 50% failure threshold → open, 30 s cool-down → half-open, 5 probe calls); graceful degradation: hide Q&A widget if Q&A Service fails; use stale Redis cache if Aurora fails
- In shopping_cart: 2 retries with backoff (50 ms, 150 ms) on Pricing Service failure → fall back to snapshot price from cart metadata; inventory hold failure → advisory warning (non-blocking)
- In flash_sale: if Google reCAPTCHA API unavailable (> 5 s timeout) → bypass CAPTCHA, apply account-age-only check (fallback policy); if Admitter crashes → secondary pod takes over within 30 s (Redis lock TTL expiry)

## Common Databases

### Redis Cluster (L2 Cache + Operational Store)
- All three; used for hot-path caching, atomic operations (Lua scripts), distributed locks, sorted sets for reservation queues
- amazon_product_page: 32 primary × 1 replica, 96 TB; shopping_cart: 6 shards (3 primary + 3 replica), 384 GB; flash_sale: dedicated nodes per sale for atomicity isolation

### Aurora MySQL
- All three; ACID source of truth for pricing, coupons, purchases; Multi-AZ; 2–15 read replicas depending on scale

### DynamoDB
- Two of three (amazon_product_page, shopping_cart); used for flexible-schema, managed-scaling document store; Global Tables for multi-region active-active

## Common Communication Patterns

### Three-Layer Cache Architecture (L1 in-process + L2 Redis + L3 CDN)
- Amazon product page: L1 = 256 MB/pod in-process LRU (70% hit for top 1M ASINs), L2 = Redis Cluster (96 TB, 30 s TTL safety net), L3 = CDN (30 s TTL for SSR HTML, 1 year for images)
- Shopping cart + flash sale: L2 = Redis (operational primary cache), L3 = CDN (static pages); no in-process L1 cache at this scale

### Event-Driven Invalidation + TTL Safety Net
- All three invalidate caches via Kafka events (immediate) with TTL as fallback in case invalidation events are lost
- Pattern: write to DB → publish Kafka event → cache invalidator consumes → DEL Redis key + CDN PURGE; TTL ensures eventual consistency even if event is dropped

## Common Scalability Techniques

### Horizontal Pod Auto-Scaling (HPA) on RPS + Latency
- All three scale stateless service pods based on request rate or P99 latency
- amazon_product_page: 500 pods × 100 RPS/pod × 3 regions = 150K RPS global capacity; HPA target CPU 60%
- shopping_cart: 56 pods needed at 27,778 RPS peak (500 RPS/pod); HPA on CPU + P99 > 80 ms
- flash_sale: pre-scaled 5 min before sale start to avoid 60–120 s HPA lag; Purchase Service: 200 pods stateless

### Pre-Scaling for Predictable Traffic Spikes
- Flash sales have known start times; product pages have predictable peak hours; carts spike with sales
- flash_sale: Kubernetes pre-scale 5 min before sale start; all services at N×steady-state capacity at T=0
- amazon_product_page: pre-warming Redis/CDN for anticipated viral or sale-promoted ASINs

### Partitioning Kafka by Business Entity (asin / order_id / user_id)
- All three partition Kafka topics by a business key to ensure FIFO ordering per entity
- amazon_product_page: partition by `asin % 64` for price events; by `asin` for inventory events
- shopping_cart: partition by `cart_id` or `user_id`
- flash_sale: 8 partitions by `sale_id`

## Common Deep Dive Questions

### How do you keep product price and inventory fresh within 5 seconds at 115K RPS?
Answer: Three-layer cache with event-driven invalidation. Pricing Service writes to Aurora (durable) and publishes to Kafka `price-events`. A cache invalidator consumer deletes the Redis key immediately; the TTL (5 s for prices) is the safety net. Redis L2 serves cache hits in < 5 ms. L1 in-process cache (256 MB/pod) serves the top 1M ASINs with 2 s TTL — a pod failure means the next request hits Redis (not Aurora). CDN is invalidated via CloudFront soft-purge on the `catalog-updated` event for changed ASINs.
Present in: amazon_product_page

### How do you handle inventory soft holds without double-selling?
Answer: Redis Sorted Set as a reservations queue + Aurora as the authoritative durable record. Adding a hold: Lua script atomically checks available stock (total_qty - ZCARD(hold:{sku} active)) and adds the hold entry (score=expires_at) in one script execution. Sweep: `ZREMRANGEBYSCORE hold:{sku} 0 {now}` removes expired holds before each check (O(log N)). On checkout: row-lock Aurora inventory row for hard decrement. On cart abandonment (> 20 min inactivity): release hold in both Redis and Aurora. Background job reconciles every 60 s.
Present in: shopping_cart

### How do you prevent a flash sale from being oversold under 100K concurrent users?
Answer: Redis single-threaded Lua script for atomic decrement. `DECRBY inv:{sale_id} 1` inside a Lua script is guaranteed atomic (no interleaving), so two concurrent purchase attempts cannot both see qty > 0 and both succeed. Virtual queue (Redis Sorted Set, ZADD NX, drain rate 200/s via Admitter) throttles admission so Purchase Service never sees thundering herd. Bot filter (CAPTCHA + account age + behavioral score → signed 5-min JWT) reduces malicious traffic before the queue. Reconciliation job every 5 min compares Redis counter vs. Aurora purchase count and auto-corrects discrepancies.
Present in: flash_sale

### How do you handle cache stampede on popular product pages?
Answer: Probabilistic early expiration (XFetch/PER). When remaining TTL < 20% of original, a random 10% of requests trigger a background refresh (non-blocking, don't wait for result). The first request to find an expired key uses Redis SETNX to acquire a refresh lock; it fetches from Aurora and repopulates Redis. All other concurrent requests use the stale value (acceptable for 30 s TTL) or wait briefly (timeout fallback to Aurora direct read). This prevents cache stampede by reducing the window where many concurrent requests miss simultaneously.
Present in: amazon_product_page

## Common NFRs

- **Product page load (above-fold SSR)**: p99 < 200 ms (direct revenue impact: 1% conversion loss per 100 ms)
- **Cart operation latency**: p99 < 100 ms for add/remove; p99 < 300 ms for cart merge
- **Flash sale purchase confirmation**: p99 < 200 ms (admitted user only)
- **Price/inventory freshness**: ≤ 5 s for authoritative data; ≤ 60 s for aggregates
- **Availability**: 99.99% for product pages and cart; flash sale service-level SLA during sale window
- **Idempotency**: guaranteed for cart merge, coupon application, purchase — no double-charge
- **Fairness**: FCFS for flash sale (queue by timestamp); buy-box winner by weighted score for products
