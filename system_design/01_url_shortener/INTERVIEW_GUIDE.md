# Reading Pattern 1: URL Shortener — 2 problems, 7 shared components

> This file is your single source of truth for this pattern. Read it start to finish before any interview. Every question an interviewer can ask on this pattern is covered here.

---

## STEP 1 — ORIENTATION

**Problems covered:** URL Shortener, Pastebin

**Shared components (7):**
1. Redis Cluster (cache, key pool, rate limiting, counters)
2. PostgreSQL (source of truth for metadata)
3. CloudFront CDN (edge caching, TLS, WAF)
4. Key/Code Generator Service (pre-generated random pool via Redis SPOP)
5. Kafka + Flink + ClickHouse analytics pipeline
6. L7 Load Balancer (path-based routing)
7. Route53 DNS (health-check-based failover)

**What makes these two problems a "category":**
Both are **read-heavy, write-once, lookup-by-key** systems. You create an entity once, assign it a short random identifier, and then retrieve it millions of times by that identifier. The retrieval path is the hot path and must be sub-50ms. Everything in the design flows from that constraint.

---

## STEP 2 — THE MENTAL MODEL

### The One Core Idea

**This entire pattern is about building a fast public lookup table.**

That is all it is. You have a key (short code or paste key) and a value (a long URL or text content). The challenge is not the lookup itself — that is trivial. The challenge is making that lookup happen in under 20-50 ms for millions of concurrent users without the database seeing most of the traffic.

### The Real-World Analogy

Think of a **coat check at a concert venue**. You hand in your coat (the long URL or paste content), you get a small ticket stub (the 7-character short code). When you want your coat back, you hand over the stub and immediately get your coat. The coat check attendant does not search through every coat — they go directly to slot `aB3xZ9`.

Now imagine this concert has 100 million attendees per day. You can't have one attendant. You put a sign at every entrance that says "stub `aB3xZ9` → row 5, rack 3, hook 7" — that is your CDN cache. Most people find their coat from the sign without ever talking to the attendant. The ones who can't (cache miss) talk to the attendant (Redis), and only the rare newcomer requires going to the back office (PostgreSQL).

### Why This Category Is Uniquely Hard

The hard part is not scale in the abstract. The hard part is **three specific tensions in collision**:

1. **Uniqueness vs. Speed**: You need globally unique short codes, but checking uniqueness requires coordination. If you check the database on every write, you serialize writes. If you don't check, you get collisions.

2. **Analytics vs. Latency**: You want to count every click, but adding a database write to the redirect hot path doubles latency. The analytics pipeline must be completely decoupled from the response path.

3. **301 vs. 302**: A 301 (permanent redirect) lets browsers cache the destination forever and never call your service again — great for performance, fatal for analytics. A 302 (temporary redirect) means every click hits your service — great for analytics, potentially more load. This is a product decision that has major architectural consequences, and most candidates never ask about it.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions (spend 2-3 minutes at the start)

Ask these questions. After each one, note what the answer changes.

**Question 1: What is the expected scale — DAU, new URLs per day, and read:write ratio?**
- What it changes: Everything. 100M DAU vs 1B DAU changes whether you need sharding now vs later, how large your Redis cluster is, how many redirect service instances you need. If they say "like Bitly," use 100M DAU as baseline.

**Question 2: Do we need per-link analytics — click counts, referrer, geo-location?**
- What it changes: Whether you need Kafka at all. No analytics = no async event pipeline, much simpler design. With analytics, you need to decide 301 vs 302 (301 breaks analytics if browser caches it). This question often reveals the product intent.

**Question 3: Do users need to update or delete their short links after creation?**
- What it changes: Cache invalidation complexity. If links are immutable, you can cache forever (or until TTL). If they can be updated, you need a cache invalidation strategy — CDN invalidation API, Redis DEL on write. Also affects 301 vs 302: mutable links must use 302.

**Question 4: Do we need custom aliases — users choosing their own short codes?**
- What it changes: Key generation strategy. Custom aliases require a uniqueness check at creation time. You also need a blocklist of reserved terms (system paths, brand names). This adds the namespace squatting problem.

**Question 5: What are the latency requirements for the redirect path?**
- What it changes: How aggressively you layer your cache. If they say "under 100ms is fine," one Redis layer might be enough. If they say "under 20ms p99," you need CDN + Redis + process-level negative caching all working together. Probe this.

**Red flags — what most candidates skip that interviewers notice:**
- Not asking about analytics at all, and therefore never discussing 301 vs 302
- Assuming the system is write-heavy (it is 100:1 read-heavy)
- Not asking about link expiration (affects cleanup strategy, cache TTL design, partial indexes)
- Jumping to "use DynamoDB for scale" without understanding the access pattern first — a point-lookup by primary key on PostgreSQL with read replicas handles this pattern at massive scale

---

### 3b. Functional Requirements

**State these in under 60 seconds. Be crisp.**

"The core requirements are: create a short URL that redirects to a long URL, retrieve via redirect, and optionally — custom aliases, link expiration, and click analytics. I'm going to treat analytics as in-scope since it's table stakes for any real URL shortener.

Out of scope: a full analytics dashboard UI, real-time streaming analytics, A/B testing, QR code generation, and multi-region active-active setup — we'll do single-region with a DR replica."

**In-scope:**
- Create short URL given a long URL → return unique short code
- Redirect short code → long URL with HTTP 301 or 302
- Optional custom alias (user-specified short code)
- Link expiration with configurable TTL
- Per-link analytics (click count, referrer, country, device)
- User accounts to manage their links

**Out-of-scope (and why, so you sound deliberate):**
- Full analytics dashboard UI — API-first is enough for MVP
- Password-protected links — adds auth complexity to the redirect hot path
- QR codes — a presentation-layer feature, not architecture
- Multi-region active-active — adds 3x design complexity; single region + DR is sufficient

---

### 3c. Non-Functional Requirements

**Derive them, don't just list them.** Show the reasoning.

| NFR | Value | Reasoning | Trade-off baked in |
|-----|-------|-----------|-------------------|
| **Availability (read)** | 99.99% | The redirect IS the product. If a short link goes down, it's embarrassing and breaks email campaigns, printed QR codes. ~52 min/year downtime. | ✅ Near-zero downtime ❌ More infrastructure, no single point of failure allowed |
| **Availability (write)** | 99.9% | Creation is a one-time action. Users can retry if creation fails. ~8.7 hrs/year tolerance. | ✅ Simpler write path ❌ Rare write failures are acceptable |
| **Redirect latency** | p99 < 20ms | A short link's entire value prop is speed. Anything over 100ms is user-perceptible. 20ms allows Redis hit + network RTT. | ✅ Great UX ❌ Forces layered caching; no synchronous DB writes on hot path |
| **Write latency** | p99 < 200ms | Creation is interactive but not latency-critical. 200ms is fine for a form submission. | ✅ Budget for DB write + ID generation ❌ Cannot be lazy about it |
| **Consistency** | Strong for uniqueness; eventual for analytics | Two clicks on the same code must ALWAYS go to the same URL — hard requirement. Analytics counters can lag 30s — acceptable. | ✅ Simpler analytics pipeline ❌ Dashboard data is approximate |
| **Durability** | Once created, never silently disappear | A URL that randomly disappears breaks user trust permanently. Explicit deletes OK. | ✅ Clear contract ❌ Must do soft deletes + careful cleanup jobs |
| **Read:Write ratio** | ~100:1 | 100M redirects/day vs ~1M creations/day. | ✅ Tells you to optimize aggressively for reads ❌ Cannot rely on write path for read performance |

---

### 3d. Capacity Estimation

**Spend 3-4 minutes max here. Numbers to know cold.**

**Traffic:**
```
New URLs/day:    1 million
Redirects/day:   100 million
Write QPS:       1M / 86,400 ≈ 12 QPS
Read QPS:        100M / 86,400 ≈ 1,157 QPS
Peak read QPS:   5× = ~5,785 QPS (viral events)
Read:Write:      ~100:1
```

**Storage:**
```
URL record:      ~500 bytes
5 years of URLs: 500B × 1M/day × 1,825 days ≈ 912 GB ≈ ~1 TB with indexes
Analytics events: 200 bytes × 100M/day × 365 days ≈ 7.3 TB/year (cold-stored after 1 year)
Working set:     20% of URLs get 80% of traffic → 20% × 30-day active = ~30M × 500B = 15 GB → fits in Redis
```

**What the math tells you about architecture:**

1. **1,157 read QPS is trivially served** — a single PostgreSQL read replica handles ~5,000 QPS of point lookups. The problem isn't DB capacity at baseline. The problem is p99 latency (DB at p99 is ~5-20ms, but that budget is tight). So you put Redis in front.

2. **At 95% cache hit rate**, only 58 QPS falls through to DB — 1 read replica is fine.

3. **15 GB working set** — a $100/month Redis cluster covers it. This is cheap.

4. **12 write QPS** is nearly nothing. Don't over-engineer the write path.

5. **7.3 TB/year analytics** — this cannot go into PostgreSQL. It needs a columnar store (ClickHouse) or cold storage.

---

### 3e. High-Level Design

**The 5 core components every HLD for URL Shortener needs:**

1. **CDN (CloudFront)** — caches redirect responses at edge, absorbs viral traffic
2. **Load Balancer (L7)** — path-based routing to separate read vs write services
3. **Redirect Service** (stateless) — the hot path, check Redis → fallback DB → HTTP 302
4. **Write/API Service** (stateless) — creation, validation, custom aliases, auth
5. **Redis Cluster** — hot URL cache + code pool + click counters
6. **PostgreSQL** — source of truth; primary + 2 async read replicas

Supporting components:
- ID Generator (background job maintaining code pool in Redis)
- Kafka → Flink → ClickHouse (async analytics pipeline)
- Route53 DNS

**Data flow for a redirect (this is what you trace on the whiteboard):**

```
1. User clicks https://short.ly/aB3xZ9
2. Browser → DNS (Route53) → resolves to CloudFront edge IP
3. CDN checks cache for key "aB3xZ9"
   ↳ HIT: returns HTTP 302 Location: <long_url> from edge in ~5ms. DONE.
4. CDN MISS → Load Balancer → Redirect Service instance
5. Redirect Service: redis.GET("url:aB3xZ9")
   ↳ HIT: parse long_url|expires_at, check if expired in app code, return HTTP 302 in ~10ms
6. Redis MISS → Redirect Service queries PostgreSQL READ REPLICA
   SELECT long_url, expires_at FROM urls WHERE short_code = 'aB3xZ9'
7. Found + not expired: SET redis cache (TTL=24h), return HTTP 302
8. Not found: SET redis "NOT_FOUND" tombstone (TTL=5min), return HTTP 404
9. Async (non-blocking goroutine): publish click event to Kafka
   {code, timestamp, ip, user_agent, referrer}
10. Kafka → Flink (enriches with GeoIP) → ClickHouse (time-series analytics)
```

**Data flow for URL creation:**

```
1. POST /api/v1/shorten with {long_url, optional custom_alias, optional expires_at}
2. Write Service: validate URL syntax + DNS resolution + Safe Browsing API check
3. ID Generator: redis.SPOP("available_codes") → pops one pre-generated 7-char base62 code
4. DB INSERT: INSERT INTO urls (short_code, long_url, ...) ON CONFLICT DO NOTHING
5. Cache warm: redis.SET("url:<code>", long_url + "|" + expires_at, EX=86400)
6. Return HTTP 201 {short_url, short_code, expires_at}
```

**What to draw on the whiteboard and in what order:**

1. Start with a box labeled "User" and an arrow labeled "GET /aB3xZ9"
2. Draw CDN box, label it "CloudFront — caches 302s"
3. Draw LB box, then split into two service boxes: "Redirect Service" and "Write/API Service"
4. Draw Redis Cluster next to Redirect Service, label it "hot cache, 15GB working set"
5. Draw PostgreSQL below both services, label "Primary + 2 Replicas"
6. Add the async analytics branch: Kafka → Flink → ClickHouse (keep it small — it's not the focus)
7. Add DNS/Route53 above CDN
8. Walk the happy path: CDN hit → done. CDN miss → Redis hit → done. Redis miss → DB → populate cache → done.

---

### 3f. Deep Dive Areas

#### Deep Dive 1: Short Code Generation (interviewers ALWAYS ask this)

**The core problem:** Every URL creation needs a unique 7-character short code. You need this to be fast (no DB round-trip on the hot path), random (not guessable/enumerable), and collision-free.

**The solution: Pre-generated token pool in Redis.**

Here's the argument for it:
- A background job runs every 60 seconds, generates 500K random 7-char base62 strings, batch-checks them against the DB for prior use, pushes clean ones into a Redis set (`SADD available_codes`)
- On URL creation: `redis.SPOP("available_codes")` — O(1) atomic pop, zero DB round-trip, guaranteed no collision because `SPOP` is single-threaded
- Pool target: 10M codes (~80 MB in Redis). Refill triggers when pool drops below 1M
- Keyspace: 62^7 = 3.5 trillion. At 1M URLs/day, this lasts 9,589 years. Never runs out.
- Fallback (if pool is empty): generate random code in memory, `INSERT ... ON CONFLICT (short_code) DO NOTHING`, retry on conflict up to 5 times

**Why not MD5 hash of the long URL?**
Two problems. First, you take 6-8 chars of a 128-bit hash — collision probability using the birthday paradox approaches 50% at ~2M URLs for a 42-bit prefix. Second, idempotency becomes a bug: if a user shortens the same URL twice for different campaigns, they get the same code both times. We explicitly support multiple codes per long URL.

**Why not sequential DB IDs encoded to base62?**
Sequential = enumerable. An attacker can crawl your entire database of short URLs in order. Not acceptable.

**Trade-offs to mention unprompted:**
- ✅ Pool: zero collision, zero DB read on write hot path, random codes
- ❌ Pool: requires background job management, pool exhaustion monitoring, fallback logic
- ✅ Hash approach: deterministic, no coordination
- ❌ Hash approach: birthday collision risk at scale, can't create two codes for same URL

---

#### Deep Dive 2: The Redirect Hot Path and Cache Layering

**The core problem:** 1,157 reads/second (5,785 at peak) need to complete in under 20ms p99. A cold DB query is 5-20ms by itself. You can't afford it for most requests.

**The solution: Three-layer cache.**

| Layer | What it serves | Latency | Hit rate target |
|-------|----------------|---------|-----------------|
| CDN (CloudFront) | HTTP 302 responses, globally at edge | ~5ms | 70%+ for popular links |
| Redis Cluster | short_code → long_url mapping | ~1ms | >95% of remaining |
| PostgreSQL read replica | Cache misses | ~5-20ms | ~5% of requests |

**At 95% Redis hit rate with 70% CDN hit rate:**
- CDN: serves 70% of 5,785 = 4,050 QPS → never touches origin
- Redis: serves 95% of remaining 1,735 = 1,648 QPS
- DB: sees only 87 QPS — well within one read replica's capacity

**Cache key format:** `url:<short_code>` → value: `long_url|expires_at` (pipe-delimited string, not JSON — 30% less memory, pipe cannot appear in a valid URL)

**The 301 vs 302 decision — say this unprompted:**
"I'd default to HTTP 302. A 301 tells browsers to cache the redirect permanently — once a browser caches it, every click from that user goes directly to the destination, bypassing our service entirely. That means we never receive the click event and analytics break. We use 302 so every click hits our service. The trade-off is one extra round-trip per click. For links with no expiry and where the creator opts out of analytics, we offer an opt-in 301 as a 'fast link' tier."

**Thundering herd prevention:**
When a popular URL's 24h Redis TTL expires, hundreds of concurrent requests see a cache miss simultaneously and all query the DB. Prevention:
1. **Single-flight/mutex**: `SET url:lock:<code> 1 NX PX 500` — only one request queries DB while others wait
2. **TTL jitter**: Add ±10% random jitter to TTL so entries don't all expire at the same time
3. **XFetch probabilistic early expiration**: Slightly before TTL expiry, probabilistically refresh the cache entry based on current request rate

**Tombstone caching:** Store `NOT_FOUND` with a 5-minute TTL so the DB is only queried once per 5 minutes for non-existent codes. This prevents abuse via random code enumeration from hammering the DB.

**Trade-offs to mention unprompted:**
- ✅ 302 for analytics | ❌ one extra RTT per click vs 301
- ✅ Redis cache for speed | ❌ cache invalidation complexity on URL updates
- ✅ CDN for viral absorption | ❌ CDN invalidation propagation takes up to 5 seconds

---

#### Deep Dive 3: Analytics Without Adding Latency

**The core problem:** 1,157 click events per second need to be recorded, but adding a DB write to the redirect path would increase p99 latency by ~5ms and require 100K+ DB writes/day on the read path.

**The solution: Redis INCR for real-time counts + Kafka for detailed analytics.**

Two parallel flows:
1. **Fast counter**: `redis.INCR("clicks:<short_code>")` — this is sub-millisecond and non-blocking. A background job every 30 seconds reads all `clicks:*` keys and batch-updates `urls.click_count` in PostgreSQL. Users see ~30-second-stale click count on dashboard.

2. **Rich analytics**: Redirect Service publishes to Kafka topic `click-events` in a non-blocking goroutine — fire and forget. Kafka → Flink (enriches with MaxMind GeoIP) → ClickHouse (columnar, optimized for time-series queries). Dashboard queries for "clicks by day, top referrers, country breakdown" go to ClickHouse.

**If Kafka goes down:** The Redis INCR still runs (so approximate counts are always updated). The Kafka goroutine falls back to a local in-memory ring buffer (max 100K events). When Kafka recovers, the buffer drains. During the outage, we lose detailed event data (no referrer, no country) but counts remain accurate. Analytics SLA is "best-effort within 5 minutes" — not 100% accuracy.

---

### 3g. Failure Scenarios & Resilience

**How to frame failure scenarios to signal senior-level thinking:**
Don't just say "retry." Explain the failure mode → its user-visible impact → your specific mitigation → the monitoring you'd put in place.

| Failure | User Impact | Mitigation | Monitoring |
|---------|-------------|------------|------------|
| **DB primary down** | URL creation fails; existing redirects continue (cache + replica) | Patroni/RDS Multi-AZ auto-promotes standby in ~30-60s. Write Service retries with exponential backoff + jitter. | Alert on primary connection failures; RTO target: 60s |
| **Redis cluster shard down** | That shard's URLs fall through to DB; p99 latency increases from ~1ms to ~5-20ms | Redis Cluster auto-promotes replica in ~1-2s. Circuit breaker in Redirect Service detects failure, falls through to DB gracefully. | Alert on Redis error rate > 1%; monitor cache hit rate — sudden drop signals shard failure |
| **Cache cold start (deployment)** | Thundering herd on DB as cache is empty | Pre-warm: before deploying, script fetches top 1M hot URLs (by click_count in last 7 days) and populates Redis | Monitor DB QPS spike at deployment time |
| **Code pool exhaustion** | URL creation falls to fallback path (slightly slower) | Alert fires when pool < 100K codes. Fallback: random generate + INSERT ... ON CONFLICT retry. Refill job has Kubernetes liveness probe for auto-restart. | Alert on `SCARD available_codes` < 100,000 |
| **Kafka unavailable** | Detailed analytics delayed; counts still update via Redis INCR | In-memory ring buffer absorbs events; drains when Kafka recovers | Alert on Kafka consumer lag > 1M events or > 5 minutes |
| **Viral spike (10x traffic)** | If CDN hit rate is high, nothing — CDN absorbs it. If CDN cold (new code), origin sees burst. | CDN Origin Shield coalesces all PoP misses into 1 origin request. ASG scales Redirect Service up (CPU trigger). Pre-warm CDN for anticipated events. | Monitor redirect p99 latency; set CloudWatch alarm on CDN cache miss rate |

**Senior-level framing to use:** "The key insight is that the read path and the write path have independent failure modes. If the write service is completely down, existing redirects still work — cache + DB replicas serve them. The user's experience of 'link not working' almost always comes from the read path (cache/DB/CDN issue), not the write path. I'd design monitoring to distinguish these two failure domains separately."

---

## STEP 4 — COMMON COMPONENTS BREAKDOWN

### Redis Cluster

**Why it's used here specifically:** Because the dominant access pattern is a point-lookup of a 500-byte value by a 7-character key — the exact case where Redis shines. Every millisecond of redirect latency is user-visible, and Redis delivers sub-millisecond reads. Additionally, Redis's atomic `SPOP` on a set is the correct primitive for the key pool (atomic pop, no lock contention), and `INCR` is the right primitive for click counting.

**Key configuration decision to mention:** `maxmemory-policy: allkeys-lru`. This means when Redis runs out of memory, it evicts the least-recently-used key. For our use case (hot URLs are accessed frequently, cold URLs rarely), LRU naturally keeps the most valuable entries in cache. Alternative: LFU (least-frequently-used) is slightly better for Zipf-distributed access patterns but harder to reason about with TTLs.

**What if you didn't use it:** Every redirect would query PostgreSQL directly. At 1,157 QPS average (5,785 peak), you'd need ~5-10 read replicas just for baseline load, and every "popular link going viral" event would require emergency database scaling. P99 latency for a DB query is 5-20ms vs ~0.5ms for Redis — you'd miss the 20ms p99 target on the redirect path frequently.

---

### PostgreSQL (Primary Database)

**Why it's used here specifically:** The dominant operation is a single-row point-lookup by `short_code` — `SELECT long_url FROM urls WHERE short_code = 'aB3xZ9'`. This is exactly what PostgreSQL's B-tree index is optimized for. The UNIQUE constraint on `short_code` is ACID-guaranteed — two concurrent creations cannot get the same code. After 5 years we have ~200M rows (1M/day × 1,825 days), which is well within PostgreSQL's tested range with proper indexing and read replicas. The URLs table is structured with a well-known schema — no need for schema flexibility (which would push toward MongoDB/DynamoDB).

**Key configuration decision to mention:** PgBouncer in **transaction pooling mode** as a connection pooler. Without it, each application thread/goroutine holds a DB connection for the duration of a request. At 1,157 QPS with 5ms query time, you'd have ~6 concurrent DB connections minimum — manageable. But during a spike or deployment (when many instances restart), you can get a connection storm. PgBouncer caps the physical DB connections at `pool_size × instances` (e.g., 10 × 8 = 80), well below PostgreSQL's `max_connections=200`.

**What if you didn't use it:** If you used DynamoDB instead, you lose the UNIQUE constraint for short codes (DynamoDB Transactions are available but expensive — 2x read/write units — and limited to 25 items). You lose the partial index for expiry cleanup (DynamoDB has no partial indexes). You lose the ability to do arbitrary queries without pre-declaring access patterns. At our scale (12 writes/sec, 87 cache-miss reads/sec), PostgreSQL is not a bottleneck — DynamoDB would add vendor lock-in and cost without benefit.

---

### CloudFront CDN

**Why it's used here specifically:** For URL shortener, the response to `GET /aB3xZ9` is a tiny HTTP 302 with a `Location` header — literally just a URL string. This is perfectly cacheable at the edge. CloudFront can serve this response from a PoP ~10ms away from the user, without the request ever reaching our origin servers. Viral links get 100% CDN hit rate within minutes of spreading.

**Key configuration decision to mention:** **Origin Shield** — an intermediate caching layer between CloudFront edge nodes and your origin. When a new short code gets its first requests, multiple edge PoPs simultaneously see cache misses and all try to fetch from origin. Without Origin Shield, your origin sees as many requests as there are PoPs (400+). With Origin Shield, all PoP misses are coalesced into a single request to origin. This is critical for protecting against thundering herd from a newly viral link.

**What if you didn't use it:** Every redirect for a popular link would hit your origin servers. At 50,000 QPS from a viral tweet, without CDN you'd need ~50 Redirect Service instances all simultaneously active — and you'd still see your Redis cluster under heavy load. CDN is the single highest-leverage component in this design.

---

### Key/Code Generator Service

**Why it's used here specifically:** Short code generation has a coordination problem — you need globally unique codes without a central single point of failure. Decoupling the generator into a separate service (really a background job + Redis set) means the write path has zero coordination overhead: it just pops from a pre-filled pool.

**Key configuration decision to mention:** Pool size target and alert threshold. Pool = 10M codes (~80 MB in Redis). Refill triggers at 1M remaining codes. Alert fires at 100K remaining. The alert-to-refill buffer means you get paged before the fallback path is needed under normal load, giving on-call time to investigate without user impact.

**What if you didn't use it:** If you generated codes on demand with a DB uniqueness check, every URL creation would require a `SELECT short_code FROM urls WHERE short_code = $candidate` before the INSERT. At 12 writes/second that's fine, but it adds a DB read to the write path. More importantly, at scale the collision probability in a shorter keyspace would require multiple retries per creation. The pre-generated pool eliminates this entirely.

---

### Kafka + Flink + ClickHouse Analytics Pipeline

**Why it's used here specifically:** 1,157 click events per second cannot be synchronously written to a transactional OLTP database (PostgreSQL) on the redirect hot path without adding 5-10ms of latency and requiring 100K+ DB write capacity. Kafka decouples the event capture (fire-and-forget in a goroutine) from the event processing. Flink handles stream enrichment (IP → GeoIP lookup) and windowing. ClickHouse is a columnar OLAP database optimized for the exact queries analytics needs: "give me clicks per day for this short code over the last 30 days, grouped by country."

**Key configuration decision to mention:** Kafka partition key = `short_code`. This ensures all click events for one URL land on the same partition, guaranteeing ordered processing for that URL. With 24 partitions, you can have 24 parallel Flink consumer tasks. 7-day retention on Kafka means you can replay up to 7 days of events if the Flink job crashes.

**What if you didn't use it:** Option A — synchronous PostgreSQL write on every redirect. Adds 5-10ms to p99 latency and requires ~100K writes/day to a transactional table (degrades with analytics query load). Option B — only Redis INCR counters. You get a click count but lose all granular data: no referrer, no country, no device type, no time-series breakdown. The analytics dashboard is severely limited.

---

### L7 Load Balancer

**Why it's used here specifically:** The Redirect Service and Write/API Service have completely different traffic characteristics — 1,157 QPS for redirects vs 12 QPS for writes. They should scale independently. An L7 (HTTP-aware) load balancer enables **path-based routing**: `GET /<code>` → Redirect Service instances; `POST /api/*` → Write/API Service instances. This means you can run 8 Redirect Service instances and 2 Write Service instances, sized appropriately.

**Key configuration decision to mention:** Health check interval of 5 seconds with `unhealthyThreshold=2` — a failing instance is removed from rotation in ~10 seconds. No sticky sessions — both services are stateless, so any instance can handle any request.

**What if you didn't use it:** A single service handles both redirect and write traffic. A spike in write traffic (e.g., a bulk import job) would consume instances needed for redirects. You lose the ability to scale the two paths independently.

---

### Route53 DNS

**Why it's used here specifically:** Low TTL (~60 seconds) on the DNS record for `short.ly` means that if the CDN or LB IP changes (e.g., during a failover or blue-green deployment), clients pick up the new IP within 60 seconds. Route53's health-check-based failover lets you automatically route traffic away from an unhealthy primary region to a secondary in a DR scenario.

**Key configuration decision to mention:** TTL of 60 seconds is a balance — low enough for fast failover, high enough to not cause DNS lookup overhead on every request (CDN handles the actual connection pooling after first resolution).

**What if you didn't use it:** Long DNS TTLs (e.g., 3600s) mean that during a regional failure, clients that already resolved the old IP continue hitting a dead endpoint for up to an hour.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### URL Shortener vs Pastebin — 2-sentence answer

**"How is designing a URL shortener different from designing a Pastebin?"**

> "The URL shortener's entire architecture is optimized for the redirect — a tiny sub-20ms response that returns a single URL string, where every millisecond matters because it's the product. Pastebin has a fundamentally different storage problem: the 'value' in the key-value lookup is up to 1 MB of text content, which means you can't cache everything in Redis and you need a separate object store (S3) for the content itself, with PostgreSQL only holding metadata."

---

### URL Shortener — What Makes It Unique

**Unique aspect 1: The 301 vs 302 redirect decision**

This decision has no equivalent in Pastebin. 301 (permanent redirect) is cached by browsers forever — once cached, that client never makes another request to your service for that code. This is amazing for performance but catastrophic for analytics: you'll never see that click event again. 302 (temporary) means every click hits your service. The right answer depends on the product: analytics-enabled links use 302; a "fast link" tier for users who don't need analytics can use 301.

The specific technical implication: if you serve 301 to a browser and later the user updates their URL target (PATCH endpoint), that browser will continue redirecting to the old destination forever (until browser cache is cleared). This is why mutable links must use 302.

**Unique aspect 2: The hot path latency constraint is extreme (20ms p99)**

The URL shortener needs a sub-20ms p99 redirect. Every design decision flows from this. No synchronous DB writes on redirect. Layered CDN + Redis + tombstone caching. Expiry checked in application code (not just Redis TTL). XFetch probabilistic early expiration. This extreme latency budget doesn't exist in Pastebin (50ms p99 there) because the content payload is larger anyway.

---

### Pastebin — What Makes It Unique

**Unique aspect 1: Content storage in S3, not the database**

In URL shortener, the "value" (long URL) is a 2KB text field that lives comfortably in PostgreSQL. In Pastebin, the "value" (paste content) is up to 1MB and averages ~10KB. Storing 10KB × 1M pastes/day = 10GB/day in PostgreSQL causes TOAST (The Oversized-Attribute Storage Technique) fragmentation and table bloat within months. The correct architecture is: PostgreSQL stores only metadata (~512 bytes per row), S3 stores the actual content. This means every cache miss requires two lookups: PostgreSQL for metadata + S3 for content. That's why Pastebin's read latency budget is 50ms (not 20ms like URL shortener) — S3 GET latency alone is ~20-50ms.

**Unique aspect 2: Content immutability + forking**

Pastebin makes content immutable after creation. If you want to "edit" a paste, you fork it — a server-side S3 CopyObject + new metadata row. This design choice is not arbitrary: immutability is what enables aggressive CDN caching. If content can change in-place, every CDN cache entry could be stale. With immutable content, once a paste is cached at a CDN PoP, it can stay there until the paste expires — no invalidation needed on "edits" because edits create a new paste. The trade-off is that the fork model is slightly unfamiliar to users who expect a save button.

---

## STEP 6 — INTERVIEW Q&A BANK

### Tier 1 — Surface Questions (first 10 minutes)

**Q: What database would you use for the URL mappings?**
> "PostgreSQL. The access pattern is a point-lookup by short code — one row, by primary key. PostgreSQL handles this at well over 100K QPS with read replicas. I need ACID uniqueness guarantee on the short code — can't have two URLs pointing to the same code — which PostgreSQL's UNIQUE constraint gives me atomically. NoSQL options like DynamoDB don't give me strong uniqueness without using expensive transactions."

**Q: How do you handle the redirect — HTTP 301 or 302?**
> "By default, 302. A 301 is cached permanently by browsers, which means once a user's browser caches it, we never see that click event again and analytics break completely. 302 means every click hits our service. The downside is one extra round-trip vs 301. I'd offer 301 as an opt-in for a 'no analytics, maximum speed' tier."

**Q: How do you make sure two URLs don't get the same short code?**
> "I use a pre-generated random code pool. A background job generates half a million random 7-char base62 codes, batch-checks them against the database for any that already exist, and pushes the clean ones into a Redis set. When creating a URL, I call SPOP on that set — it's an atomic O(1) pop that guarantees no two requests get the same code. If the pool is empty, I fall back to random generation with a database INSERT that has ON CONFLICT DO NOTHING."

**Q: Why not just hash the long URL to generate the short code?**
> "Two problems. One: I'm taking 6-8 characters from a 128-bit hash. The birthday paradox says collision probability approaches 50% around 2 million stored URLs at that bit length — we'd start getting collisions at real scale. Two: idempotency becomes a bug. If a user shortens the same URL twice for two different campaigns, they expect two different short codes. MD5 would give them the same code both times."

**Q: How does caching work for the redirect path?**
> "Three layers. First, the CDN — popular short codes are cached at edge nodes globally, returned in ~5ms without hitting origin. Second, Redis — on a CDN miss, the redirect service checks Redis for the short_code to long_url mapping. Hit rate target is 95%+. Third, PostgreSQL read replica as the fallback. At 95% Redis hit rate, only about 58 QPS falls through to the DB — well within one replica's capacity."

**Q: How do you count clicks without slowing down redirects?**
> "Two-track approach. For fast approximate counts: Redis INCR on every redirect — sub-millisecond, non-blocking. A background job flushes these increments to PostgreSQL every 30 seconds. For rich analytics (referrer, country, device): the redirect service publishes to Kafka in a non-blocking goroutine — fire and forget. Kafka feeds Flink, which enriches with GeoIP data and batch-inserts to ClickHouse. The redirect path itself never blocks on analytics."

**Q: What happens when a link expires?**
> "Multiple layers. The Redis cache entry has TTL set to min(URL TTL, 24 hours) — so expired entries naturally evict. But even before that, the redirect service checks expires_at in application code on every cache hit — if expired, it returns HTTP 410 and writes an 'EXPIRED' tombstone to Redis with a 60-second TTL. A background cron job runs every minute and soft-deletes (is_active=FALSE) any URLs where expires_at < NOW, using a partial index on non-null expiry rows to avoid full table scans."

---

### Tier 2 — Deep Dive Questions (strong candidates)

**Q: Walk me through exactly how you'd prevent a thundering herd when a cached URL expires and 5,000 concurrent requests hit the cache miss simultaneously.**
> "Three defenses layered together. First, **TTL jitter** — I add ±10% random jitter when setting TTL so entries don't all expire at exactly the same second. Second, **XFetch probabilistic early expiration** — the algorithm proactively refreshes a cached entry slightly before it expires, based on the current request rate. Popular URLs are effectively never allowed to fully expire because they're refreshed before they can. Third, for cases that slip through, a **distributed mutex**: `SET url:lock:<code> 1 NX PX 500` — only the request that wins the lock queries the DB; the others wait briefly and then find the cache populated. The key insight is that thundering herd is only a problem for popular URLs — and popular URLs are exactly the ones XFetch keeps fresh."

**Q: Your Redirect Service is stateless, but you need rate limiting that works across all instances. How?**
> "Centralized rate limiting via Redis, implemented as a **token bucket algorithm in a Lua script**. The Lua script runs atomically — Redis is single-threaded, so there's no race condition. The key is `rl:ip:<ip>` for anonymous users or `rl:user:<id>` for authenticated. The script checks tokens available, refills based on elapsed time since last request, decrements if tokens available, returns 0 if rate limited. All Redirect Service instances point to the same Redis cluster, so the count is globally consistent. This adds ~1ms to each request (one Redis round-trip), which is well within our 20ms p99 budget."

**Q: How would you handle a URL that was created, then the user updated the destination, but CDN still has the old destination cached?**
> "Three-step invalidation on PATCH. Step one: update PostgreSQL (immediately consistent). Step two: `redis.DEL('url:<code>')` — cache invalidated, next request misses and fetches fresh. Step three: call the CloudFront Invalidation API for path `/<short_code>` — CloudFront purges the edge cache globally, propagates in under 5 seconds. There's a ~5 second window where some edge nodes may serve the old URL. That's acceptable. The reason we default to 302 instead of 301 is exactly this — a 301 cached in a browser is not under our control and we can't invalidate it. CloudFront we can invalidate. Browser 301 cache we cannot."

**Q: How do you do a zero-downtime schema migration at 200 million rows?**
> "Use the expand-contract pattern. Step one — expand: add the new column as nullable with no default (`ALTER TABLE urls ADD COLUMN new_col TYPE NULL`). In PostgreSQL, adding a nullable column with no default is an instant metadata-only change — no table rewrite, no lock. Step two — backfill: background job updates existing rows in batches of 1,000 with a sleep between batches to avoid lock contention and index bloat. Step three — deploy new code that reads both old and new column. Step four — contract: once backfill is done and old column unused, drop it (another fast metadata op). **Never** run `ALTER TABLE ADD COLUMN NOT NULL DEFAULT 'x'` on a live 200M-row table. PostgreSQL will take an exclusive lock and rewrite the entire table, causing downtime."

**Q: If traffic grows 100x to 100,000 redirect QPS, what breaks first and in what order?**
> "Let me trace the bottleneck chain. CDN handles millions of QPS — not the issue. Redis Cluster rated 300K ops/sec total across 3 shards — at 100K QPS with 95% hit rate, Redis sees 95K ops/sec — fine. Redirect Service: at 1,000 QPS per instance, I need 100 instances. Stateless, so just add them to the ASG. DB on cache misses: at 5% miss rate on 100K QPS = 5K DB reads/sec, I'd need about 10 read replicas. The first real fix is scaling Redirect Service instances, which is trivial since they're stateless. The second concern is cache warm-up during deployments — deploy with a cache pre-warm script to prevent cold-cache thundering herd at that scale."

**Q: How does the custom alias feature introduce race conditions, and how do you handle them?**
> "The race condition is: User A and User B both request custom alias 'my-brand' simultaneously. Both check `SELECT 1 FROM urls WHERE short_code = 'my-brand'` — both see no result. Both proceed to INSERT. One succeeds, one gets a unique constraint violation. The solution is to not do a SELECT-then-INSERT — just do the INSERT with `ON CONFLICT (short_code) DO NOTHING` and check if any row was returned. If no row returned, the alias was taken in the same instant; return HTTP 409 Conflict to the user. This is why the UNIQUE constraint on `short_code` is non-negotiable — it's what makes this atomic at the database level without explicit locking."

**Q: Walk me through what happens to the system when the Redis cluster has a shard failure.**
> "Redis Cluster with 3 primary shards + 3 replicas. When a primary fails, the cluster detects it within `cluster-node-timeout` (default 15 seconds, we configure 5 seconds). The replica for that shard is promoted to primary. During that 1-5 second window: requests to keys hashed to the failed shard get CLUSTERDOWN errors. The Redirect Service's Redis client has a circuit breaker — after 3 consecutive failures it trips to 'open' and falls through directly to PostgreSQL. So during the failover window, ~1/3 of redirects (those hashing to the failed shard) see slightly higher latency as they fall through to DB. After failover completes, the circuit breaker probes with one request, sees success, and closes — Redis resumes. I'd put primary and replica in different AZs to prevent correlated failures."

---

### Tier 3 — Stress Test / Staff+ Questions (no single right answer)

**Q: Explain the consistency model of your entire system end-to-end. What is a user's experience if you relax consistency somewhere?**
> "There are three distinct consistency zones. Zone 1: URL creation — **strongly consistent**. The UNIQUE constraint on `short_code` is enforced by PostgreSQL with ACID semantics. Two concurrent creates cannot produce the same code. Zone 2: redirect resolution — **read-your-writes for the creator** (because Write Service populates Redis immediately on creation), but **eventually consistent for others** due to async replication lag (~100ms). If User A creates a URL and User B tries to redirect it immediately (within 100ms before replica catches up), User B might get a 404 momentarily — mitigated by write-through caching. Zone 3: analytics — **eventually consistent** with ~30-second lag for click counts (Redis INCR flush delay) and ~1-5 minute lag for detailed analytics (Kafka pipeline). Users see 'updated within a minute' for counts. The risk I'd flag: if we serve 301 redirects, we lose analytics consistency entirely for cached clients — which is why 302 is the default."

**Q: How would you design this system to work across three geographic regions, ensuring EU users' data never leaves the EU?**
> "Let me separate two concerns: the read path and the compliance requirement. For reads: CDN already solves latency globally — CloudFront PoPs in every region, zero data residency issue. For writes and data residency: EU users must create URLs on EU infrastructure, and that data must stay in EU. Implementation: Route53 geolocation routing sends EU source IPs to an EU-based stack with its own PostgreSQL primary. The EU primary syncs to US primary via cross-region replication for global read availability — but EU user PII rows are tagged and excluded from US sync. Click events from EU users go to a Kafka cluster in EU, processed by Flink in EU, stored in ClickHouse in EU. The US stack never sees EU user PII. The challenge: what about a EU user whose short link is clicked by a US user? The redirect request hits US CDN. If CDN miss, request reaches US Read Service. That service needs to resolve the code — which lives in the EU DB. Options: (a) replicate all URL mappings (not PII) globally, keeping PII fields null outside EU, (b) US Read Service makes a cross-region call to EU for cache misses. Option (a) is simpler and correct since the URL mapping itself (short_code → long_url) isn't PII."

**Q: Your system currently has 1M new URLs per day. The business wants to add a feature: 'deduplication — if the same long URL is shortened twice, return the same short code.' How does this change the architecture?**
> "This is a classic correctness-vs-feature trade-off. First, let me push back: deduplication might be a bug, not a feature. If a user shortens the same URL twice for two different marketing campaigns, they want two distinct short codes to track clicks separately. So I'd make deduplication opt-in, not default. For the implementation: at creation time, compute `hash(long_url)` and add a column `long_url_hash VARCHAR(64)` to the urls table with an index. Before creating a new code, do `SELECT short_code FROM urls WHERE long_url_hash = $hash AND user_id = $user`. If found, return the existing code. If not, create new. The race condition: two concurrent requests for the same URL both miss the SELECT and both INSERT. Resolution: let them both succeed — two short codes for the same long URL is not harmful, just slightly wasteful. The INDEX on long_url_hash makes the lookup O(log N). One caveat I'd raise: deduplication across users (returning User A's code to User B) is almost certainly wrong — users expect their links to be theirs."

**Q: If I told you the 80th percentile of your short codes are never redirected after the first week, how would you change your Redis caching strategy?**
> "That data point tells me the Pareto distribution is even more skewed than the typical 80/20 — most links are truly ephemeral. This suggests a few changes. First, reduce default Redis TTL from 24 hours to maybe 4-6 hours — links not accessed in 4 hours are unlikely to be accessed again soon, and evicting them earlier makes room for fresher hot links. Second, use **LFU (Least Frequently Used) eviction** instead of LRU. LRU keeps recently-accessed entries; LFU keeps frequently-accessed entries. For a distribution where 80% of links are used once and forgotten, LFU is more accurate at identifying which 20% to keep. Third, don't bother pre-warming these one-shot links — they'll be served cold from DB on creation and then expire from cache naturally. The key insight: LRU is optimized for temporal locality (I just used it, I'll use it again soon). LFU is optimized for frequency (I always use this one). Given the data showing most links are ephemeral, LFU keeps our frequently-used links in cache longer and stops wasting space on the long tail of one-shot links."

**Q: A very senior engineer on your team argues: 'We should just use DynamoDB for the URL table — it's serverless, infinite scale, and simpler to operate.' How do you respond?**
> "I'd take that seriously and engage with the specific trade-offs rather than dismissing it. DynamoDB has real advantages here: no connection pool management, auto-scaling, single-digit ms reads at any scale, managed service. The reason I chose PostgreSQL is specific to three things our design depends on. First: the UNIQUE constraint for short_code uniqueness — DynamoDB Transactions can enforce this but they cost 2x read/write units per operation and have a 25-item limit. At 12 writes/sec it's not a throughput issue, but the cost adds up and the semantics are more complex. Second: the partial index for expiry cleanup — `WHERE expires_at IS NOT NULL AND is_active = TRUE`. DynamoDB has no partial indexes; you'd need a GSI that scans the whole table or a separate table for expiring items, adding complexity. Third: read replicas — our read path at 87 QPS DB fallback is served by a single replica. DynamoDB has no concept of read replicas; you pay per read regardless. At our moderate scale, PostgreSQL with PgBouncer is not operationally burdensome. However, if the scale question changed to 'we expect 100B URLs within 2 years,' I'd reconsider — that's where DynamoDB's shardless horizontal scale starts winning on operational simplicity."

---

## STEP 7 — MNEMONICS & MEMORY ANCHORS

### Memory Trick 1: The "CRACK" Framework for this Pattern

For any URL-shortener-style interview, remember **CRACK**:

- **C** — **Cache layers** (CDN → Redis → DB — in that order, explain each layer)
- **R** — **Redirect type** (301 vs 302 — always bring this up, it signals depth)
- **A** — **Analytics decoupling** (Redis INCR + Kafka/Flink/ClickHouse — never synchronous)
- **C** — **Code generation** (pre-generated pool, Redis SPOP, not hash or sequential)
- **K** — **Key uniqueness guarantee** (PostgreSQL UNIQUE constraint, ON CONFLICT DO NOTHING)

Walk through CRACK in your head when you're asked this question. Cover each one, even briefly.

### Memory Trick 2: The Numbers to Know Cold

```
Write QPS:   ~12/sec     (1M URLs/day ÷ 86,400)
Read QPS:    ~1,157/sec  (100M redirects/day ÷ 86,400)
Peak read:   ~5,785/sec  (5x viral multiplier)
Redis size:  ~15-20 GB   (hot working set for URL cache)
DB size:     ~1 TB       (5 years of URL records with indexes)
Keyspace:    3.5 trillion (62^7 — 7-char base62 codes, lasts 9,589 years at 1M/day)
Cache miss → DB load: ~87 QPS at 95% hit rate — handled by 1 read replica
```

When you forget a number, derive it live: "100 million divided by 86,400 seconds in a day is roughly 1,200 QPS."

### Opening One-Liner

When you start your answer to "Design a URL shortener," say this:

> **"A URL shortener is fundamentally a globally distributed read-heavy key-value lookup where the value is a URL string. The entire design is about making that lookup happen in under 20ms for every user on Earth without touching the database more than 5% of the time."**

This sentence signals you understand: it's read-heavy (100:1), it's latency-critical (20ms), it's a cache problem (95% cache hit target), and you've already disqualified naive designs (no DB on every read). You've set the frame for everything that follows.

---

## STEP 8 — CRITIQUE & GAPS

### What the Source Documentation Covers Well

The source files are genuinely excellent and unusually deep on the following:

- **Short code / paste key generation**: Thorough treatment of all six approaches with trade-off analysis, collision probability math, birthday paradox numbers, and fallback behavior. Most interview notes skip the fallback path entirely — this one nails it.

- **Cache layering**: The three-layer CDN → Redis → DB design is well-articulated with actual hit rate numbers and their implications for DB sizing. The tombstone caching pattern for NOT_FOUND is correctly included.

- **Failure scenarios**: The failure table with per-failure impact and mitigation is much better than most study materials. The Redis circuit breaker configuration thresholds are specific and credible.

- **Schema design**: The partial index for expiry cleanup (`WHERE expires_at IS NOT NULL AND is_active = TRUE`) is a genuine senior-level detail that most candidates miss entirely.

- **Pastebin's S3 lifecycle / hybrid expiry**: The four-layer hybrid (S3 lifecycle + PostgreSQL cron + Redis TTL + lazy on-read) with the caveat about S3 lifecycle granularity (days, not minutes) is sophisticated and interview-ready.

### What Is Missing, Shallow, or Potentially Wrong

**1. The "read-your-writes" consistency problem after URL creation is underexplained.**
The notes mention "write-through cache on creation mitigates replication lag" but don't fully address: what if the Write Service is in a different AZ than the Redirect Service, the Write Service wrote to Redis but the Redirect Service is talking to a different Redis shard (due to hash routing), and the replica hasn't caught up? The full solution is: Write Service always writes to the same Redis cluster as Redirect Service reads, so the key is present before the HTTP 201 returns to the client. This should be stated explicitly.

**2. The 301 vs 302 section is correct but doesn't address CDN caching of 302.**
The notes say "use 302 to preserve analytics." But CDN can also cache 302 responses (with `Cache-Control: public, max-age=3600`). A CDN-cached 302 means the CDN serves the redirect without hitting origin — you get CDN performance benefits AND analytics (because the click eventually reaches your service from a CDN miss, not from the browser's 301 cache). The notes don't distinguish between "browser-cached 301 (breaks analytics)" and "CDN-cached 302 (fine for analytics)." This nuance comes up in interviews.

**3. The content deduplication in Pastebin has a subtle S3 lifecycle bug that is acknowledged but the solution is incomplete.**
The notes correctly identify that if multiple paste rows point to the same S3 object (via sha256 deduplication) and one paste expires, you can't blindly delete the S3 object. The proposed solution (use sha256 as the S3 key, run a background job) is correct but the notes don't explain **how the S3 lifecycle rule would be configured** in this case. Answer: you would NOT use S3 lifecycle rules for deduplicated objects at all — the background job is the only deletion path. S3 lifecycle rules only work for objects with no sharing. This distinction should be explicit.

**4. The public feed architecture gap.**
The notes mention the Redis Sorted Set approach for Pastebin's public feed, which is good. But there's no mention of what happens to the feed when a paste is deleted or marked private after creation. The naive implementation (just ZADD on creation, never ZREM) means deleted/private pastes stay in the feed sorted set. The full implementation needs `ZREM public:feed <paste_key>` on deletion and on visibility change to private/unlisted. This cascading cache invalidation on visibility change is a common interview trap.

**5. Analytics exactly-once semantics is asserted but not fully explained.**
The notes say "Flink uses exactly-once semantics with Kafka transactions and ClickHouse upserts keyed on event_id." This is correct but glosses over a real challenge: ClickHouse does not natively support upserts (it's eventually consistent MergeTree). "Upserts" in ClickHouse are implemented via `ReplacingMergeTree` engine, which deduplicates rows with the same key during background merges — not immediately. There's a window where duplicate events appear in ClickHouse until the merge runs. For a click count system, this is acceptable (occasional double-count, self-correcting). But a candidate who claims "exactly-once in ClickHouse" without knowing this caveat will be caught by a sharp interviewer.

### What a Senior Interviewer Would Probe That the Notes Don't Cover

**1. Hot key problem in Redis.**
The notes mention it for URL shortener but the answer focuses on CDN absorbing the traffic. A senior interviewer will ask: what if Redis itself becomes the bottleneck for a single hot key? At 50,000 redirects/sec to one URL, even if CDN serves most of them, the cache warming phase means Redis might see a spike. Real answer: Redis single-threaded per shard handles ~100K ops/sec — a single key on one shard is fine up to that limit. Beyond it, you'd need to partition the value across multiple Redis keys (e.g., `url:aB3xZ9:0`, `url:aB3xZ9:1`, ..., `url:aB3xZ9:9`) and randomly select one on read. This is a replica-of-one-key pattern that the notes don't cover.

**2. The Idempotency-Key implementation has a race condition.**
The notes describe: "check Redis for key → if not found, process → store response." Between the "check" and the "store," another request with the same Idempotency-Key can arrive and also see "not found." Both proceed to process. Both try to INSERT. One succeeds. But both try to store the response — the second store overwrites the first. The fix is `SET idempotency:<key> "PROCESSING" NX EX 30` — the `NX` flag means "only set if not exists," which is atomic in Redis. Only the first request gets the lock. The second request sees `PROCESSING` and either polls or returns 409. The notes mention the NX approach correctly but don't call out why naive check-then-set fails.

**3. Short code reuse after deletion — the 90-day quarantine edge cases.**
If a link is deleted, we quarantine the code for 90 days before it can be reused. But what if the new URL mapped to a recycled code is a phishing site, and the original 301 is still in some users' browser caches? This is exactly the phishing risk that quarantine protects against. A senior interviewer might probe: "How long is the quarantine, and how did you arrive at that number?" The honest answer is 90 days is somewhat arbitrary — based on CDN TTL (max ~30 days) + browser cache TTL (CDN-set, so also ~30 days) + some buffer. For links in printed materials (enterprise), you might want indefinite quarantine. The notes say 90 days but don't give the reasoning — you should be able to reconstruct it.

**4. Connection pooling at scale — the "noisy neighbor" problem.**
The notes describe PgBouncer correctly. What they don't address: in transaction pooling mode, a long-running PostgreSQL query (e.g., a slow analytics query) holds a server connection for the duration. If you have 5 slow queries running, PgBouncer's pool may be exhausted for all other queries. Solution: separate connection pools for OLAP queries (analytics dashboard) vs OLTP queries (redirect fallback). The analytics queries hit read replicas via a separate PgBouncer instance with `pool_size=5`. The OLTP redirect queries have `pool_size=50` on a different PgBouncer instance. This isolation is a standard operational practice that the notes don't mention.

### Common Interview Traps in This Pattern

**Trap 1: "Just use DynamoDB — it scales better."**
This is a lazy answer that interviewers recognize instantly. DynamoDB solves some scaling problems but introduces new ones (no partial indexes, weak uniqueness, expensive transactions, vendor lock-in, expensive at high read rates). The correct answer acknowledges DynamoDB's trade-offs specifically and explains why PostgreSQL + Redis + CDN is the better choice at the stated scale, while leaving the door open to DynamoDB at extreme scale (10B+ URLs).

**Trap 2: Designing the write path before understanding read:write ratio.**
Many candidates spend 30 minutes designing an elaborate write path for URL creation. The write path is 12 QPS. The read path is 1,157 QPS. The entire design should be write-simple, read-optimized. If you catch yourself spending more time on creation than on redirect, rebalance.

**Trap 3: Forgetting to ask about analytics.**
"How do you count clicks?" is a follow-up question that almost every interviewer asks. If you design the system without analytics in scope, you'll need to retrofit the async Kafka pipeline mid-interview. Ask about analytics upfront in your clarifying questions. If they say "no analytics," your design simplifies significantly (no Kafka, no 302 vs 301 debate).

**Trap 4: Saying "Redis is the database" or "Redis is the source of truth."**
Redis is a cache. Caches can lose data (eviction, restart). PostgreSQL is the source of truth. If Redis loses the URL mappings, the system degrades (cache miss storm hits DB) but does not lose data. If PostgreSQL loses data, URLs are gone forever. This distinction matters architecturally and conceptually. Never say "I'll store it in Redis" for durable data.

**Trap 5: Not mentioning the 301 vs 302 trade-off.**
This comes up in almost every URL shortener interview. If you don't bring it up yourself, the interviewer will — and you'll look reactive rather than proactive. Bring it up when discussing the redirect API design: "The first decision here is 301 vs 302 — let me explain why I'd default to 302..." This single unprompted detail signals more depth than most candidates show.

**Trap 6: For Pastebin — storing content in PostgreSQL without justification.**
This is technically possible (up to maybe 100M pastes). But any interviewer who knows Pastebin will probe "why not S3?" The moment you say "TEXT column in PostgreSQL," you need to know the TOAST fragmentation issue, the storage cost difference (5x more expensive than S3), and why PostgreSQL + S3 at the 10KB average × 1M/day = 10GB/day scale is the right split. If you default to S3 upfront, you avoid the trap entirely and look more experienced.

---

*This guide covers all material from url_shortener.md, pastebin.md, common_patterns.md, and problem_specific.md. No other files are needed.*
