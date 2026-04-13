Reading Pattern 2: Social Media — 5 problems, 9 shared components

---

# System Design Interview Guide: Social Media Pattern

## The 5 Problems Covered
1. Twitter (X) — short-form text, 280 chars, real-time trends
2. Instagram — photo/video, Stories, Explore discovery
3. TikTok — recommendation-first video, FYP, ML at scale
4. Facebook News Feed — privacy-filtered ranked feed, 3B users
5. LinkedIn Feed — professional context, connection degrees, viral amplification

## The 9 Shared Components
1. Redis Cluster (precomputed feed cache)
2. Kafka (event bus decoupling writes from fan-out)
3. CDN (media delivery)
4. Object Storage / S3 (media blobs)
5. Fan-out Worker Service (hybrid push/pull)
6. Precomputed Feed Cache per User
7. Snowflake ID Generator
8. Elasticsearch / Full-Text Search
9. MySQL (strongly consistent social graph + user profiles)

---

# STEP 1 — ORIENTATION

This pattern covers the most commonly asked system design problem category in senior and staff-level interviews at every major tech company. The five problems share a deep structural similarity: they all need to take content written by one person and deliver it efficiently to many followers. The differences are in the constraints — character limits, media types, privacy rules, recommendation algorithms, and the shape of the social graph.

When an interviewer says "design Twitter" or "design a social media feed," they are fundamentally asking you to solve one problem well: **how do you fan out writes to millions of readers while keeping reads fast and costs sane?** Everything else — Kafka, Redis, Cassandra, the ML ranking model — is in service of that one core challenge.

---

# STEP 2 — THE MENTAL MODEL

## The One Core Idea

The core insight that unlocks the entire social media pattern is this: **social media is a read-amplification problem disguised as a write problem.**

When someone posts a tweet, the write itself is trivial — store 280 characters. The hard part is that this one write must produce a read result for potentially 100 million people (if the author is a celebrity). Every design decision in this pattern flows from that tension.

## The Real-World Analogy

Think of a newspaper. When a journalist writes a story (the write), the newspaper doesn't print a single copy and make everyone share it. They print millions of copies and distribute them to every subscriber's doorstep before they wake up (the fan-out). When you pick up the paper, reading is instant because someone already did the work of getting it to your door.

A social media feed is the digital equivalent. The fan-out worker is the printing press. The Redis timeline cache is the newspaper on your doorstep. The controversy is: how many copies do you print, and when? If a celebrity posts, printing 100 million copies of the newspaper is expensive. That's the celebrity fan-out problem.

## Why This Category Is Hard: The Fundamental Tension

There are exactly two naive approaches, and both fail:

**Fan-out on Write (Push model):** Every time someone posts, immediately push the content to every follower's inbox. Reads are instant because the work was done ahead of time. But if Kylie Jenner posts one tweet and has 37 million followers, you just created 37 million Redis writes in one second. That's catastrophic write amplification for celebrity accounts.

**Fan-out on Read (Pull model):** When someone opens their feed, fetch the most recent posts from everyone they follow, merge, sort. No write amplification at all. But if someone follows 2,000 accounts, this requires 2,000 database reads, a merge sort, deduplication, and ranking — all in real-time before the feed appears. That's unacceptably slow.

The answer in every problem in this pattern is a **hybrid**: push for regular users, pull for celebrities. The threshold between "regular" and "celebrity" varies by platform. This hybrid is the defining architectural pattern for the entire social media category.

---

# STEP 3 — INTERVIEW FRAMEWORK

## 3a. Clarifying Questions

Ask these in the first 3–4 minutes. Don't skip them. Each changes your design significantly.

**Question 1: "Is the feed chronological or ranked by an algorithm?"**
- Why it matters: Chronological means you just need a sorted list of post_ids per user. Algorithmic ranking requires a feature store, an ML inference layer, and a cascade scoring pipeline. This doubles your architecture complexity. Twitter originally was chronological; Facebook and TikTok are algorithm-ranked. Get this settled before drawing anything.
- What you say: "I want to confirm — is the home feed in chronological order of posts, or does an ML ranking algorithm determine the order?"

**Question 2: "What types of content does the platform support — text only, or also images and video?"**
- Why it matters: Text posts are trivially small (280 bytes). Images require a media upload pipeline, a CDN, multiple resolutions generated at upload time (a Media Processor fleet), and storage that runs into tens of terabytes per day. Video multiplies that by another 100x and adds transcoding. The entire storage and bandwidth estimate changes by two orders of magnitude.
- What you say: "Does the platform support media — photos or video — or is it text-only? This significantly changes the storage and upload pipeline."

**Question 3: "What is the scale? How many MAU, DAU, and what's the expected read-to-write ratio?"**
- Why it matters: The capacity estimate drives every scaling decision. A system for 10M users needs one architecture; 3 billion needs another. Social media consistently has a 20:1 to 100:1 read-to-write ratio, but knowing the absolute numbers determines whether you need 10 Redis nodes or 1,000.
- Red flag if you skip it: You'll design a system without knowing whether you need horizontal sharding at all.

**Question 4: "Are there privacy controls on posts — can posts be visible to a specific subset of followers?"**
- Why it matters: Public-only feeds (Twitter) are simple. Privacy-controlled feeds (Facebook's Public/Friends/Friends-of-Friends/Custom) require a correctness-critical privacy subsystem that cannot be glossed over — it's a potential P0 incident if wrong. This is the single thing that most distinguishes Facebook's design from everyone else's.
- What you say: "Do posts have per-post privacy controls, or is everything public to followers?"

**Question 5: "Is the feed purely social-graph-based (content from accounts you follow), or recommendation-based (content from accounts you don't follow)?"**
- Why it matters: A social-graph feed requires a follow graph and fan-out. A recommendation feed (TikTok FYP) requires an entirely different architecture: user embeddings, video embeddings, a two-tower neural network, FAISS index, and a feature store. These are not the same system at all.
- Red flag most candidates miss: They assume all social media feeds are follow-graph-based. When you ask this question about TikTok, you reveal that you understand TikTok is fundamentally different from Twitter.

## 3b. Functional Requirements

State these in the first 60 seconds after your clarifying questions. Keep it tight.

**Core Features (always in scope for a feed system):**
- Users can create posts (text, optionally media)
- Users can follow/unfollow other users
- Users see a home feed of posts from followed accounts (or recommendations)
- Users can interact with posts (like, comment, share — exact actions vary by platform)
- Users have profile pages
- Notifications for interactions on their posts

**How to state it cleanly in 60 seconds:** "I'll scope this to: post creation, follow graph management, home feed reads, and basic engagement — likes and comments. I'll treat things like direct messaging, ads, and content moderation as out of scope unless you want me to include them. Does that work for you?"

**In-scope vs. out-of-scope signals:**
- IN scope: Core post creation, feed, follow, like/comment, notifications
- OUT scope for initial design (but mention them): DM, ads system, content moderation/trust and safety, billing, live streaming, third-party API auth, search (unless asked)

**The right scope signals**: If you narrow scope confidently and the interviewer says "yes, that's fine," you've saved yourself 20 minutes of design work on a system you won't be asked about. If they say "actually let's include search," now you know to include Elasticsearch.

## 3c. Non-Functional Requirements

Work through these methodically. Derive each one with a sentence of reasoning rather than just stating it.

**Availability: 99.99% uptime (~52 minutes downtime per year)**
"Social platforms are always-on consumer products. Any downtime is high-visibility, directly impacts revenue, and makes news. 99.99% is the right target. This means we prefer eventual consistency over strict consistency — we'd rather show a slightly stale feed than show nothing."

**Feed Read Latency: p99 < 200–500ms depending on the platform**
"Feed reads are synchronous, user-facing requests. Studies show users notice latency above 300ms. Twitter targets p99 < 200ms. Facebook and LinkedIn, which have more expensive ranked feeds, target p99 < 500ms. Either way, sub-second is the requirement. This forces us to pre-compute feeds rather than compute them at read time."

**Eventual Consistency on Feeds**
"It is acceptable for a new post to appear in followers' feeds with a few seconds of lag. We do NOT need strong consistency here. This is the key consistency relaxation that makes the fan-out pattern possible — if we needed every follower to see the post within milliseconds, we'd need a completely different architecture."

**Write Durability: Posts durable once acknowledged**
"Once the system returns a 200 to the client, the post must never be lost. This drives our choice of durable write storage (Cassandra with RF=3 or MySQL with replication). It also means we should write to the database before publishing to Kafka — Kafka is durable but it's not the source of truth for posts."

**Celebrity/Hotspot Resilience**
"The system must handle accounts with millions of followers posting without degrading for other users. This is the celebrity fan-out problem. If we don't call this out, the interviewer will bring it up as a follow-up, and we'll look like we hadn't thought of it."

**Scale**
"State the read QPS and write QPS you calculated. Explain what that means for architecture. If peak read QPS is 1M, you cannot serve that from a single database — you need a caching layer in front of it."

## 3d. Capacity Estimation

**Formula Order for Social Media — Do This Every Time:**

1. **Start with MAU and DAU** (anchor the user base)
2. **Posts per day** (MAU × posts_per_day_per_active_user — varies widely: 2.5 for Twitter, 0.02 for LinkedIn)
3. **Feed reads per day** (DAU × feed_loads_per_day — typically 50–100 loads/day for a primary feed app)
4. **Average read QPS** = Feed reads per day / 86,400
5. **Peak read QPS** = Average × 3–5x multiplier (social media peaks are sharp, especially around events)
6. **Write QPS** = Posts per day / 86,400, then × peak multiplier
7. **Storage** = Posts per day × average post size × retention; media is always the dominant term
8. **Bandwidth** = QPS × average response size; CDN offloads 90% of media reads

**Anchor Numbers to Memorize:**

| Platform | MAU | DAU | Peak Read QPS | Tweet/Post Size |
|----------|-----|-----|---------------|-----------------|
| Twitter | 400M | 200M | ~1M | 280 chars = ~500 bytes |
| Instagram | 2B | 500M | ~2.3M | Photo: 3 MB, text: 1 KB |
| TikTok | 1.5B | 700M | ~1M (FYP API) | Video: 50–100 MB |
| Facebook | 3B | 2B | ~5M | Mixed, video dominates |
| LinkedIn | 300M MAU | 150M DAU | ~350K | Text-heavy, 2 KB avg |

**The One Number That Changes Everything:** 86,400 seconds per day. Divide any daily count by this to get average QPS. Then multiply by 3–5 for peak. For Twitter: 500M tweets/day / 86,400 ≈ 5,800 writes/sec. At 4x peak: 25,000 writes/sec. That's the write QPS you design for.

**What the Math Tells You:**
- Read:write ratio for social media is roughly 20:1 to 100:1. Read is the dominant case. Design your read path first and most carefully.
- Storage growth is almost entirely media. A text-only platform grows at ~1 TB/day. Add video and you're at petabytes per year.
- The peak multiplier matters: 4–5x is standard for social. Super Bowl, elections, breaking news can spike to 10–20x. Call this out.

**Time to Spend:** 5–7 minutes maximum. Get the rough order of magnitude right. Being within 2x is fine. Precision is not the point — what matters is that you can articulate the implications ("so we need a multi-region cache with sub-millisecond access to handle 1M QPS").

## 3e. High-Level Design

**The 6 Core Components for a Social Feed (draw these in order):**

1. **Clients → CDN → API Gateway**
"First thing is the entry point. Client hits CDN for static assets and media. API requests go to the API Gateway, which handles TLS termination, auth (JWT token validation), rate limiting, and routes to the right microservice."

2. **Post Service (Write Path)**
"Handles post creation. Validates content, assigns a Snowflake ID, persists to the primary store (Cassandra or MySQL), publishes a `post.created` event to Kafka, and returns the response to the client immediately. The fan-out is async."

3. **Kafka (Decoupling Layer)**
"Kafka decouples the write path from the fan-out. The Post Service doesn't wait for fan-out to finish. Kafka guarantees the event is durable. Fan-out workers consume at their own pace. This is critical for handling celebrity posts — a 37-million-follower fanout shouldn't block the post-creation API response."

4. **Fan-out Worker Service**
"Consumes `post.created` events. For each post, looks up the follower list from the Follow Graph DB. For normal users (below the celebrity threshold), pushes the post_id into each follower's Redis timeline list. For celebrities (above threshold), skips the push — read time will pull from their recent posts."

5. **Feed Service (Read Path)**
"When a user opens their feed, the Feed Service reads the precomputed timeline list from Redis for that user — this is an O(1) operation. If celebrities are followed, it also fetches their recent posts from Cassandra and merges. Then it hydrates the post_ids into full post objects."

6. **Data Stores**
"Three main stores: (1) Cassandra or MySQL for the posts themselves and the follow graph. (2) Redis Cluster for the precomputed timeline cache per user. (3) S3 + CDN for media blobs."

**Whiteboard Drawing Order:**
Draw the write path top-to-bottom first: Client → API Gateway → Post Service → Kafka → Fan-out Worker → Redis. Then draw the read path alongside: Client → API Gateway → Feed Service → Redis (cache hit) → Cassandra (cache miss / celeb pull) → Post hydration → Client. Label the celebrity threshold decision on the Fan-out Worker.

**Key Design Decisions to State Explicitly:**
- "I chose Cassandra for post storage because it's write-optimized (LSM-tree), scales horizontally by tweet_id, and tunable consistency lets us do QUORUM writes with eventual consistency on reads."
- "I chose Redis Sorted Set for the timeline cache because the score field maps naturally to tweet_id (Snowflake IDs are time-ordered), giving us O(log N) inserts and O(log N) range reads."
- "I chose Kafka because it provides durable buffering of fan-out tasks. If a Fan-out Worker crashes mid-fanout, the offset hasn't been committed, so the event is replayed. Idempotent writes to Redis (ZADD is idempotent) make replays safe."

## 3f. Deep Dive Areas

**Area 1: Hybrid Fan-out (This WILL be probed)**

Core problem: Fan-out on write creates catastrophic write amplification for celebrity accounts. Fan-out on read creates catastrophic read latency for users following thousands of accounts.

Solution: A threshold. Below N followers, push tweet_id to every follower's Redis timeline at write time. Above N followers, skip the push; at read time, merge their recent posts from Cassandra into the timeline.

The threshold numbers by platform:
- Twitter: 1 million followers
- Instagram: 1 million followers
- Facebook pages: 100,000 followers
- LinkedIn: 5,000 connections or 100,000 followers for company pages

Trade-offs:
- ✅ Keeps write amplification bounded (worst case is threshold × followers below threshold, not unlimited)
- ✅ Keeps read path fast for normal users (cache hit in Redis)
- ❌ Read path for users who follow many celebrities becomes more expensive (merge sort on the read path)
- ❌ What happens when a user crosses the celebrity threshold? Need a migration strategy (flip the flag, let old push-fanned tweets age out naturally via TTL)

Unprompted trade-off you should raise: "If a user follows 200 celebrity accounts, the read-time merge fetches 200 × 20 = 4,000 tweet_ids and merges them in memory. That's still sub-millisecond on modern hardware, so it's fine. But if someone follows 1,000 celebrity accounts, you might want a cap — say, only pull from the top 50 most recently active celebrities."

**Area 2: Timeline Cache Design and Cache Invalidation**

The Redis timeline cache per user stores the last N tweet_ids for that user (typically 800 for Twitter, 500 for Instagram). This is a Sorted Set where the score is the tweet_id (which is a Snowflake ID, so lexicographic sort equals chronological sort).

Key config decisions:
- Cap entries at 800 (use ZREMRANGEBYRANK to evict oldest when overflowing)
- TTL of 7 days — if a user hasn't logged in for 7 days, their cache is cold. On next login, fall back to fan-out on read to reconstruct the cache.
- When a tweet is deleted, it stays in Redis (fast path doesn't scan) but the hydration step skips `is_deleted=true` entries. This is acceptable for the "recently deleted" window.

Trade-offs:
- ✅ Cold path (cache miss/expired) is handled gracefully by falling back to fan-out on read
- ✅ Capping at 800 entries bounds memory usage regardless of how active the user's network is
- ❌ Deleted tweets persist in the cache until TTL — for a hard delete requirement (GDPR right to erasure), you need an explicit Redis delete plus CDN purge
- ❌ Redis memory is expensive at scale — 200M active users × ~200 bytes per entry × 800 entries = ~32 TB of Redis. You need a Redis cluster with memory tiering or a cap on active users cached.

**Area 3: Feed Ranking (for Facebook/TikTok/Instagram/LinkedIn)**

This area is asked at senior+ level when you're designing a ranked feed (not chronological).

Core problem: You can't run a full ML ranking model over all of a user's friends' posts in real-time — it would take too long. You need to pre-compute and then re-rank on the fly.

Solution: Cascade ranking. Three stages:
1. Candidate retrieval: Pull the precomputed feed from storage (up to 5,000 candidates for Facebook). This is a fast store read.
2. Cheap pre-scoring: A lightweight model (GBDT or logistic regression) pre-scores all 5,000 candidates in ~10ms. Select top 500.
3. Deep neural ranking: A heavy model (Deep Cross Network or two-tower neural network) scores the top 500 with full feature richness. Return top 20.

Trade-offs:
- ✅ Read latency is dominated by candidate retrieval (fast store read), not ML inference
- ✅ Model quality is high (full features applied to final 500 candidates)
- ❌ Two models to maintain and deploy; quality is bounded by candidate retrieval quality
- ❌ Features must be fresh: the ranker reads engagement velocity from a Feature Store (Redis or Pinot) that's updated by Kafka consumers watching reaction events

## 3g. Failure Scenarios and Resilience

State these unprompted to demonstrate senior-level thinking:

**Fan-out worker crashes mid-fan-out**
"The Fan-out Worker commits its Kafka offset only after all Redis writes succeed. If it crashes mid-fan-out, the event is replayed from the last committed offset. Since Redis ZADD is idempotent (same score and member is a no-op), replaying is safe. In the worst case, a celebrity's post gets re-fanned to some followers twice — no visible harm."

**Redis cluster failure**
"If the timeline cache is unavailable, the Feed Service falls back to fan-out on read — queries Cassandra directly for recent posts from all followees, merges, and returns. This is slower (1–2 seconds vs. 50ms) but correct. Circuit breaker opens on Redis if error rate > 5%, routes all traffic to the fallback path automatically."

**Database primary failure**
"Cassandra uses multi-DC replication (NetworkTopologyStrategy, RF=3). If the primary DC goes down, the secondary DC continues serving LOCAL_QUORUM reads and writes with ~100–200ms lag for any cross-DC replication that was in-flight. Most social media feeds tolerate this — some posts may be missing for a few minutes during DC recovery."

**Hotspot during viral event (Super Bowl, breaking news)**
"A single celebrity account posts during a major event. 100 million followers all open the app simultaneously. Defense layers: (1) Kafka absorbs the fan-out spike at its own rate — the fan-out workers catch up over minutes. (2) Celebrity bypass — their post was never in the push path anyway; read-time pull from Cassandra serves everyone. (3) CDN caches the media assets — origin sees <10% of traffic. (4) Rate limiter on the API gateway caps per-user request rates to prevent thundering-herd from client retries."

**Privacy bug (Facebook-specific)**
"A post with 'Friends' audience incorrectly shows to non-friends due to a stale friendship cache. Facebook's approach: the friendship cache has a 5-minute TTL AND an explicit invalidation event on unfollow. Even without explicit invalidation, the 5-minute window is the maximum exposure window. For regulatory compliance (GDPR), a privacy violation must be logged and potentially reported. All privacy checks are logged to an immutable audit trail."

---

# STEP 4 — COMMON COMPONENTS BREAKDOWN

For every component, understand: why is it here, what's the one key configuration decision, and what breaks if you remove it.

---

## Redis Cluster

**Why used here:** Redis is the precomputed feed cache. It stores a sorted list of post_ids per user so that "load my feed" becomes a single O(log N) sorted set read rather than an N-way merge of database queries. Without Redis, every feed read would need to hit Cassandra or MySQL for recent posts from every account the user follows — that's multiple database queries per feed load, multiplied by hundreds of thousands of concurrent users. The database would melt.

**Key config decision:** Redis Sorted Set type, with score = Snowflake ID (because Snowflake IDs encode timestamp in the top bits, lexicographic order equals chronological order — no separate timestamp needed). Cap the list at 800 entries (Twitter) or 500 (Instagram) via ZREMRANGEBYRANK on every insert. Set TTL to 7 days of inactivity.

**What happens without it:** Feed reads go directly to Cassandra for N-way merge across all followees. At 1M QPS with users following 200 accounts, that's 200M Cassandra reads per second. No NoSQL database in existence handles that. The system fails immediately.

**Platform-specific config differences:**
- Twitter: `timeline:{user_id}` → Sorted Set, 800 entries, TTL 7 days
- Instagram: `feed:{user_id}` → Sorted Set, 500 entries, TTL 3 days
- Facebook: Uses Cassandra `user_feed` table instead (5,000 entries) — feed is too large and too structured for Redis
- LinkedIn: Uses Venice/Voldemort (a purpose-built derived data store) — same concept, different implementation
- TikTok: Two caches — `fyp:{user_id}` (200 recommendation candidates, TTL 2h) + `following:{user_id}` (500 entries, TTL 3 days)

---

## Kafka (Event Bus)

**Why used here:** Kafka decouples the write path from the fan-out path. When you post a tweet, the Tweet Service writes to Cassandra and publishes an event to Kafka, then returns 200 to the client. The client sees their post instantly. The fan-out of that tweet to 10 million followers happens asynchronously in the background. Without Kafka, the post API would have to wait for all fan-out operations to complete before responding — for a celebrity account, that's millions of Redis writes that have to finish before the user gets a response.

**Key config decision:** Partition by `author_id` so all events from the same author land on the same partition in the same order. Replication factor = 3. Dead-letter queue (DLQ) for failed fan-out events. Do NOT commit the Kafka offset until the downstream operation (Redis write or Cassandra write) succeeds — this guarantees idempotent replay on crash.

**What happens without it:** Post creation is tightly coupled to fan-out. For celebrity accounts, post creation takes minutes. The API appears to hang. Or you do fire-and-forget without durability guarantees, and some followers never receive the post.

**All five problems use Kafka.** It is the single most universal component in this pattern. Mention it early and explain why.

---

## CDN (Content Delivery Network)

**Why used here:** Media (photos, videos, thumbnails) makes up the vast majority of bandwidth in every social media system. At Instagram's scale, outbound CDN traffic to users is in the terabytes per second range. The CDN has 200+ points of presence (PoPs) globally, caching media files at the network edge closest to users. Without the CDN, every photo request would go all the way back to your S3 origin in a single AWS region, adding 200+ ms of latency for international users and destroying your origin at hundreds of GB/s of traffic.

**Key config decision:** Use immutable URLs for media. When a photo is uploaded, it gets a URL that includes a hash or version identifier (e.g., `media/{media_id}/thumbnail_300.webp`). These URLs never change. The `Cache-Control` header is `max-age=31536000, immutable` — one year, forever cached. When a photo is updated or deleted, a new URL is generated. This eliminates CDN cache invalidation entirely for normal operations, which is critical because invalidating millions of CDN cached objects is expensive and slow.

**What happens without it:** At Instagram scale, media delivery to users becomes unacceptably slow and the origin servers require 100–1000x more capacity. CDN offloads typically 90–95% of media traffic. Without it, that traffic hits your origin.

**Platform-specific CDN differences:**
- Twitter/Instagram: Cloudflare, Akamai, Fastly — off-the-shelf commercial CDNs
- Facebook: Operates its own CDN infrastructure at massive scale
- TikTok: Uses Akamai/Cloudflare AND BytePlus (ByteDance's own CDN with 1,000+ PoPs), pre-warms CDN for predicted viral videos based on engagement velocity signals

---

## Object Storage / S3

**Why used here:** Social media platforms accumulate media at enormous scale — Instagram generates ~1.5 TB/day of photo data alone, and TikTok generates ~5 TB/day of video data. You cannot store this in a database or on local disk. S3 (or equivalent: Google Cloud Storage, Azure Blob Storage) offers effectively unlimited storage at low cost, with 11 nines of durability, and native CDN integration. The key insight is that media is immutable after upload — you never update a photo in place, you always write a new one. This makes object storage a perfect fit (it's optimized for write-once, read-many workloads).

**Key config decision:** Use pre-signed URLs for direct client-to-S3 uploads. When a client wants to upload a photo, your Upload Service issues a pre-signed URL with a 15-minute expiry. The client uploads the file directly to S3. The Upload Service never sees the media bytes — it only handles the metadata (< 1 KB). This means your Upload Service can run on tiny instances rather than requiring 30 GB/s of network throughput.

**What happens without it:** You'd need petabytes of structured database storage. Databases are not designed for this. BLOB columns in MySQL or Postgres would be catastrophically slow and expensive at this scale.

---

## Fan-out Worker Service

**Why used here:** The Fan-out Worker bridges the event bus (Kafka) and the timeline caches (Redis). It's the component that actually "pushes" posts to followers. By running as an independent service that consumes from Kafka, it can scale horizontally: add more worker instances to handle more fan-out volume, without touching the Post Service or the Feed Service. It also isolates failures — if the Fan-out Worker is slow or backed up, posts are just delayed in appearing in feeds (Kafka buffers them), but the platform continues operating normally for readers.

**Key config decision:** The celebrity threshold. The Fan-out Worker must check whether the posting user is above the celebrity threshold. If above: skip the push, return immediately (the read path will pull from Cassandra). If below: fan out to all followers in batches of 1,000, pipelining Redis writes for efficiency. Batch size of 1,000 balances latency (fewer round trips) against memory (holding 1,000 follower IDs in memory at once).

**What happens without it:** Either you do synchronous fan-out in the Post Service (which blocks post creation), or you do no pre-computation at all (which means reads are always expensive). Fan-out being its own service is the critical decoupling that makes the system work.

---

## Precomputed Feed Cache per User

**Why used here:** This is the output of the Fan-out Worker. It's the "newspaper on the doorstep" from the analogy. Every user has an entry in Redis (or Cassandra for Facebook, Venice for LinkedIn) that contains the post_ids they should see in their feed, in roughly the right order. When the Feed Service handles a feed read request, it reads this cache — that's the entire read operation for the "base" feed. No complex queries, no N-way merge, just a sorted list fetch.

**Key config decision:** The entry cap. Every platform caps the precomputed feed at a fixed number of entries (800 for Twitter, 500 for Instagram, 5,000 for Facebook, 200 FYP candidates for TikTok). The cap prevents unbounded memory growth. When the cap is exceeded, the oldest entries are evicted. This means that if you scroll back far enough, you'll eventually hit a "cold" region where the Feed Service has to fall back to database queries.

**What happens without it:** Every feed read requires real-time computation — fetching recent posts from every followed account and merging them. At 1M+ read QPS, this is computationally impossible within 200ms latency.

---

## Snowflake ID Generator

**Why used here:** Social media platforms need globally unique IDs for posts, comments, users, and every other entity — at rates of 25,000+ writes per second, across potentially hundreds of machines, with no single coordinator. Auto-incrementing integer IDs from a database work fine up to ~10,000 inserts/second on one machine, but they won't scale horizontally. UUID v4 is random, so sorted order means nothing (you can't tell which tweet was posted first by sorting tweet_ids). Snowflake IDs solve both problems.

**Key config decision:** The epoch offset. Snowflake IDs are 64-bit integers: 1 sign bit + 41 bits of milliseconds since a custom epoch + 10 bits of machine/datacenter ID + 12 bits of sequence. The 41-bit millisecond counter gives you 2^41 milliseconds ≈ 69 years from your chosen epoch. Twitter chose January 1, 2010 as their epoch, giving them sortable IDs until 2079. The machine IDs (0–1023) are leased from ZooKeeper — each Snowflake service instance registers, gets a unique machine ID, and renews it every 30 seconds.

**What happens without it:** Either you have a centralized ID generator (single point of failure, throughput limited to ~10K/sec), or you use UUID v4 (can't sort by creation time, so you can't use `ORDER BY id DESC` to get recent posts — you need a separate `created_at` column and index, which is slower and bulkier). Snowflake gives you both distribution and time-ordering for free.

---

## Elasticsearch (Full-Text Search)

**Why used here:** Cassandra and MySQL are excellent at lookups by primary key and range scans on indexed columns, but they cannot do efficient full-text search across 500 million tweets. Elasticsearch maintains an inverted index — a data structure that maps every word in every tweet body to the list of tweets containing that word. When a user searches for "breaking news artificial intelligence," Elasticsearch intersects those three inverted index lists in milliseconds across hundreds of millions of documents.

**Key config decision:** It is NOT the source of truth. Cassandra/MySQL hold the primary record; Elasticsearch holds an eventually-consistent copy used only for search. You index into Elasticsearch asynchronously (via a Kafka consumer watching `tweet.created` events). The indexing lag is typically 1–5 seconds. For a search index, this is acceptable — users don't expect a tweet to appear in search results the instant it's posted.

**What happens without it:** You cannot do full-text search at scale. A SQL `LIKE '%keyword%'` query on a 500-million-row Cassandra table is a full table scan that takes minutes. Without Elasticsearch (or equivalent like Solr or OpenSearch), search is either impossible or reserved for exact-match hashtag lookups only.

---

## MySQL (Strongly Consistent Social Graph)

**Why used here:** Some data in a social media system requires strong consistency and ACID guarantees. User accounts (uniqueness on username), the follow graph (you either follow someone or you don't — there's no "partially following" state), and authentication data all require a relational database with transaction support. If Cassandra accepted a follow write that then got lost due to a node failure, user A might think they follow user B but the Fan-out Worker would never push B's posts to A's feed.

**Key config decision:** Shard by `user_id` using a tool like Vitess (MySQL-compatible horizontal sharding layer used by YouTube, Twitter, and others). The follow graph table has a composite primary key on `(follower_id, followee_id)` for forward lookups (who does user A follow?) and a secondary index on `(followee_id, follower_id)` for reverse lookups (who follows user B?).

**What happens without it:** Using Cassandra for the follow graph (which some systems do) means you can have split-brain follow states during network partitions — user A's node says the follow exists, user B's node says it doesn't. This causes subtle, hard-to-diagnose feed inconsistencies. For user accounts, Cassandra lacks the uniqueness constraints needed to prevent two users from registering the same username simultaneously.

---

# STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

## Twitter

**The 1–2 unique things about this problem:**
1. The post format is uniquely constrained: 280 characters, text-centric. No GPU transcoding pipeline, no multi-resolution photo processing. The complexity is in the ID generation, trending detection, and the fact that Twitter's timeline was traditionally reverse-chronological (no ML ranking layer). The deep dive areas are Snowflake ID generation and the Count-Min Sketch trending pipeline — both of which are unique to Twitter in this pattern.
2. Real-time trending topics — identifying which hashtags are spiking in usage globally AND per region within 60 seconds — is a streaming analytics problem that requires Flink or similar. Twitter was the original problem that drove the creation of real-time stream processing at FAANG scale.

**Specific technical decision that differs:** Twitter uses a **Count-Min Sketch** for trending topic counting rather than exact hashtag frequency counts. A Count-Min Sketch is a probabilistic data structure that can estimate the count of any element in O(1) space with a ~1% error rate. Exact counting of hashtag frequencies over 5-minute sliding windows across all 500M tweets/day across 50 regions would require O(unique terms × regions × windows) memory — potentially hundreds of GB of Flink state. Count-Min Sketch collapses this to a constant-sized matrix (e.g., 50KB per region per window) with negligible accuracy loss because trending detection uses velocity (rate of change vs. baseline), not exact counts.

**"How is Twitter different from Instagram?"** Twitter is text-centric, media complexity is low, but the real-time trending pipeline and Snowflake ID generation are its signature problems. Instagram is media-first with a complex upload and transcoding pipeline, plus the ephemeral Stories feature with TTL-based expiry, plus an Explore recommendation page — significantly higher media infrastructure complexity.

---

## Instagram

**The 1–2 unique things about this problem:**
1. **Stories** — ephemeral 24-hour content. This is not just "set a TTL." You need: Cassandra row-level TTL to prevent the database from serving expired stories, an async cleanup worker to delete S3 objects (storage cost), CDN purge to prevent the CDN from serving cached stories after expiry, and a Redis bloom filter to pre-check whether a user has active stories before querying Cassandra. The correctness requirement is strict: a story must not be viewable after 24 hours under any path.
2. **Explore page** — content discovery from accounts you DON'T follow. This requires a completely different retrieval mechanism from the follow-graph-based feed. Explore uses two-tower neural network embeddings (user interest vector vs. post content vector) and FAISS approximate nearest neighbor search to find relevant posts the user hasn't seen. This is a recommendation system problem, not a fan-out problem.

**Specific technical decision that differs:** Instagram uses **pre-signed S3 URLs** for media upload. The client calls the Upload Service to get a pre-signed URL, then uploads the photo or video directly to S3 — the Upload Service never touches the media bytes. This is important because Instagram handles 100M photo/video uploads per day. Routing all of that media through application servers would require enormous bandwidth (2.8 GB/s for photos alone). Pre-signed URLs eliminate that bottleneck entirely.

**"How is Instagram different from Facebook News Feed?"** Instagram is media-first, primarily visual, with ephemeral Stories and a follow-based feed that optionally includes algorithmic ranking. Facebook News Feed is all about the privacy complexity (7 audience types that must be enforced with 100% correctness), the social graph's bidirectional friendship model (as opposed to Instagram's one-directional follow), and the multi-type content feed (text, photos, videos, links, events, shares). Facebook's TAO graph store is its defining component; Instagram's media processing pipeline is its.

---

## TikTok

**The 1–2 unique things about this problem:**
1. **The FYP (For You Page) is recommendation-first, not follow-graph-first.** This is the biggest architectural differentiator in the entire pattern. Every other problem (Twitter, Instagram, Facebook, LinkedIn) anchors the feed in the social graph — you see content from accounts you follow. TikTok's primary feed surfaces content from accounts you have never interacted with, based entirely on behavioral signals (watch %, replay rate, likes, comments, skips). The core architecture is a two-tower neural network (user embedding + video embedding) and a FAISS approximate nearest neighbor index — not a fan-out worker.
2. **Video processing pipeline complexity.** TikTok handles 50M video uploads per day, each of which must be transcoded to HLS/DASH at 5 quality levels (240p through 1080p), have visual embeddings extracted (ResNet-50 for frame features, VGGish for audio), have audio fingerprinted for sound attribution, be safety-scanned, and be uploaded to CDN origin — all within a 30-second SLA. This is a parallel microservice DAG, not a sequential pipeline.

**Specific technical decision that differs:** TikTok uses **ClickHouse** for interaction event storage. Every video watch (play, pause, skip, like, comment, share, replay, profile click) is an event. At 700M DAU × multiple events per video × multiple videos per session, this generates ~50 billion events per day. ClickHouse is a columnar OLAP database that handles this with 10:1 compression and sub-second analytical queries — critical for two use cases: (a) computing real-time features for the ML ranking model (engagement velocity for individual videos), and (b) detecting viral videos to pre-warm CDN.

**"How is TikTok different from YouTube?"** TikTok's FYP surfaces content from accounts you've never interacted with based purely on behavioral signals, and personalizes within 3–5 interactions (cold start is aggressive). YouTube Recommendations anchor more strongly in your subscription and search history. TikTok uses watch completion rate and replay as primary signals in a way YouTube cannot (YouTube videos are long — you can't replay; TikTok videos are 15–60 seconds, replays are common and meaningful signals). TikTok's "following" feed exists but is secondary; YouTube's subscription feed is primary.

---

## Facebook News Feed

**The 1–2 unique things about this problem:**
1. **Privacy filtering is 100% correctness-critical.** Facebook's post audience model has 7 types: Public, Friends, Friends of Friends, Specific People, All Except Specific People, Only Me, and Custom Lists. A post marked "Friends" must NEVER appear in a non-friend's feed — privacy violations are P0 incidents (regulatory implications, legal exposure, public scandal). This drives an entire dedicated subsystem: TAO graph traversals for relationship checks, a Bloom filter approximation for Friends-of-Friends checks, a 5-minute friendship cache with explicit invalidation on unfriend events, and fail-closed behavior (deny if uncertain).
2. **TAO (The Associations and Objects store).** Facebook built a purpose-designed distributed database to model their social graph as objects and associations. Every entity (user, post, page, group) is an object; every relationship (friendship, like, membership, posted-by) is an association. TAO is backed by MySQL with a Memcache caching layer that achieves > 99% cache hit rate and serves over 1 billion reads/second. Understanding TAO is critical to understanding how Facebook's privacy filtering, feed fan-out, and social graph traversals work efficiently at 3B user scale.

**Specific technical decision that differs:** Facebook uses **McSqueal** — a MySQL binary log tailer — to keep Memcache (TAO's caching layer) consistent with the underlying MySQL source of truth. When a MySQL write occurs (a friend is removed, a post audience is changed), the binary log entry is consumed by McSqueal, which publishes a cache invalidation event. This invalidates the relevant Memcache entries within < 100ms in the same region. This is the standard "cache aside" invalidation pattern but implemented as a log-tailing sidecar rather than a synchronous application-level double-write — it's more reliable because even if the application crashes after writing to MySQL but before invalidating Memcache, the binlog tailer will still catch the change.

**"How is Facebook different from LinkedIn?"** Facebook is consumer social, bidirectional friendships (mutual), privacy-filtered, with 3B users generating 100B feed impressions per day. LinkedIn is professional, directional following + bidirectional connections, with explicit degree computation (1st/2nd/3rd degree). LinkedIn's defining challenge is the 2nd-degree viral amplification mechanism (surfacing content that your connections have engaged with, even if you don't follow the author) and connection degree filtering at scale (48 trillion possible 2nd-degree pairs across 1B members, solved via Bloom filters).

---

## LinkedIn Feed

**The 1–2 unique things about this problem:**
1. **Connection degree computation at scale.** LinkedIn has three types of relationships: 1st degree (you're directly connected), 2nd degree (connected through one intermediary), 3rd degree (connected through two intermediaries). For 1 billion members, a full materialization of all 2nd-degree pairs would be 48 trillion rows — impossible. LinkedIn uses Bloom filters (one per active member, ~3KB each, 1% false positive rate) to approximate 1st-degree membership. For 2nd-degree checks, they use a lazy evaluation: check if any of the content author's 1st-degree connections are in the viewer's 1st-degree Bloom filter.
2. **Viral amplification via read-time injection (not fan-out).** LinkedIn has a "Viral Amplification Service" that detects when a post from a 1st-degree connection has crossed engagement velocity thresholds (e.g., 1,000 likes in 1 hour, with engagement coming from diverse companies and industries). When a post goes viral, rather than re-fanning it out to millions of 2nd-degree connections (which would be catastrophic write amplification), LinkedIn injects the viral post at read time: the Feed Service queries Apache Pinot for current viral posts, and the Feed Service merges them into the feed response. This is the same hybrid push/pull principle applied one level of indirection further out.

**Specific technical decision that differs:** LinkedIn built and open-sourced **Apache Kafka** (created at LinkedIn in 2011) and **Apache Pinot** (OLAP store for real-time feature serving). The platform uses these tools not just for historical reasons but because they fit LinkedIn's specific needs: Kafka for decoupled event streaming across all pipelines (post creation, reaction events, connection events), and Pinot for sub-second analytical queries on engagement signals (critical for viral detection and the ranking feature store, where queries like "how many likes has this post received in the past hour, broken down by company?" need to run in < 100ms).

**"How is LinkedIn different from Twitter?"** Twitter is consumer, text-limited, public/chronological-or-lightly-ranked, with no meaningful degree-of-connection model. LinkedIn is professional, has 7 reaction types, uses connection degree as a first-class concept in both feed curation and privacy, has a much lower post volume (3M posts/day vs. 500M tweets/day) but significantly higher per-post engagement, and inserts non-post content (job recommendations, PYMK cards, polls, PDFs) directly into the feed. LinkedIn's content also has a much longer engagement lifecycle — professional posts are often engaged with for days or weeks, vs. Twitter's rapid decay of tweet relevance.

---

# STEP 6 — INTERVIEW Q&A BANK

## Tier 1: Surface Questions (2–4 sentences each)

**Q1: "Why do you use Redis for the feed cache rather than Memcached?"**

The **Sorted Set data structure** in Redis is the key reason. A Sorted Set lets you store tweet_ids with a score (the Snowflake timestamp), which gives you O(log N) inserts and O(log N) time-sorted range reads natively. Memcached is a pure key-value store — it can store a list, but you'd have to fetch the entire list, add the new entry in application code, sort it, and write it back. That's a read-modify-write cycle that requires locking or CAS to prevent races. Redis does all of this atomically with ZADD. You also get free ZREMRANGEBYRANK for evicting oldest entries, which caps the timeline at 800 entries without application logic.

**Q2: "What database would you use to store tweets and why?"**

**Cassandra**, because tweets are append-only and write-heavy. Cassandra's LSM-tree storage engine is optimized for high-throughput writes — there's no B-tree write amplification from random-access page updates. Tweets are never updated after they're posted (only soft-deleted via a boolean flag), which is a perfect fit for Cassandra's immutable write model. The schema is simple: partition by `tweet_id` for point lookups, and a separate `tweets_by_user` table partitioned by `user_id` for profile page range scans. Tunable consistency lets us do QUORUM writes (durability) with LOCAL_ONE reads for timeline hydration (speed), matching the eventual consistency we've accepted.

**Q3: "How does cursor-based pagination work for the timeline, and why not use page offsets?"**

**Page offsets are unstable for live feeds.** If you use `OFFSET 20` to get page 2, and 5 new tweets have been added since page 1 was loaded, page 2 will skip some tweets (they shifted offset positions). Cursor-based pagination works by encoding the last-seen item's position — specifically, the last `tweet_id` from the previous page. The next request says "give me 20 tweets with `tweet_id < cursor`." Since tweet_ids are Snowflake IDs with timestamps encoded in the high bits, this is a natural range scan on the Redis Sorted Set using ZREVRANGEBYSCORE. The cursor is opaque to the client — it's just a base64-encoded tweet_id. As new tweets are added at the top, the cursor-based pagination stays stable because it's anchored to the last seen item's absolute position, not a numeric offset.

**Q4: "What is a Snowflake ID and why does it matter?"**

A **Snowflake ID is a 64-bit integer that is both globally unique and time-ordered.** The layout is: 1 sign bit (always 0), 41 bits of milliseconds since a custom epoch, 10 bits of machine ID (assigned from ZooKeeper), and 12 bits of sequence counter per millisecond. This produces up to 4,096 IDs per millisecond per machine. Because the timestamp is in the high bits, sorting Snowflake IDs lexicographically gives you chronological order — you can use them directly as the score in a Redis Sorted Set without a separate timestamp column. No coordination is needed between ID generators at query time. At Twitter's peak write QPS of 25,000/sec, you need roughly 7 machines generating IDs simultaneously — Snowflake handles this trivially.

**Q5: "How would you handle the 'thundering herd' problem when a celebrity posts?"**

The **celebrity bypass (hybrid fan-out) inherently prevents most of the thundering herd.** By not pushing celebrity posts to 37 million Redis keys, you avoid the situation where 37 million followers simultaneously requesting their feed all trigger cache misses at the same time. Their feeds were already populated with the celebrity's posts from the pull path — the Feed Service fetches celebrity posts directly from Cassandra on every feed read. A different thundering herd scenario is millions of followers opening the app simultaneously after a viral post — in this case, the CDN absorbs 90%+ of media traffic, and the Cassandra reads for post hydration are parallelized and cached at the application layer. Rate limiting at the API Gateway prevents any single user from sending thousands of requests per second.

**Q6: "Why does social media use eventual consistency rather than strong consistency for feeds?"**

**Because strong consistency requires cross-replica coordination that violates latency SLAs.** If every feed read required confirming that all 3 replicas agree on the current state before returning, you'd pay multiple round-trip latencies to get consensus (Paxos or Raft consensus adds 50–150ms). For a feed that needs p99 < 200ms, that's unacceptable. More importantly, the consistency requirement for feeds is genuinely weak: if a user sees a tweet 3 seconds after it was posted instead of immediately, there is zero user-facing harm. The only place where strong consistency matters in social media is for uniqueness constraints (two users can't have the same username) and for financial data. Feed freshness is not one of those cases.

**Q7: "How do you handle post deletion — specifically soft-delete vs. hard delete?"**

**Soft delete is the first operation; hard delete is deferred.** When a user deletes a tweet, the system immediately sets `is_deleted = true` in Cassandra. This is returned by every read, and the Feed Service filters out deleted tweets at hydration time. The tweet may still exist in Redis timeline caches of followers — that's acceptable because the hydration step will skip it (the post returns as deleted). For hard delete (permanent removal from Cassandra, S3, search index, and CDN), a background job runs after 30 days, which also handles regulatory requests like GDPR right-to-erasure. The CDN purge is issued at soft-delete time for any cached media to prevent it from being served via direct URL, even after application-layer deletion. For compliance, hard delete must be logged in an immutable audit log before the record is removed.

---

## Tier 2: Deep Dive Questions (with why + trade-offs)

**Q1: "Walk me through exactly what happens when a tweet is posted by a user with 50 million followers."**

"When a user with 50 million followers posts a tweet, **the celebrity bypass kicks in before the fan-out even starts.**

Step 1: The client posts to the API Gateway, which validates the JWT token and routes to the Tweet Service. The Tweet Service validates the content (280 char limit), assigns a Snowflake ID, persists the tweet to Cassandra with a QUORUM write (at least 2 of 3 replicas confirm), and publishes a `tweet.created` event to Kafka. It returns 200 to the client immediately. Total time: ~50ms.

Step 2: The Fanout Worker consumes the event from Kafka. It looks up the author's user record — `is_celebrity = true`, follower_count = 50M. Decision: skip push fan-out. The worker commits the Kafka offset and moves on. The tweet is already indexed in Cassandra's `tweets_by_user` table by `(user_id, tweet_id)`.

Step 3: When any of the 50M followers opens their feed, the Feed Service reads their Redis timeline list (which contains tweets from their non-celebrity followees, already pre-computed). Then, because this user follows a celebrity, the Feed Service fetches the celebrity's most recent 20 tweets from Cassandra (`SELECT tweet_id FROM tweets_by_user WHERE user_id = :celeb_id LIMIT 20`). This is a single Cassandra partition range scan — fast. Merge with Redis list, sort by tweet_id, return top 20.

Trade-off to mention unprompted: The celebrity's most recent tweet may appear in some followers' feeds with a brief delay (the milliseconds to populate Cassandra and for the next feed refresh) — but since fan-out was skipped, all 50 million followers get a consistent view on their next load. There is no stale cache problem because there was no push to begin with."

**Q2: "How would you design the privacy filtering system for Facebook's Feed if it needed to be correct 100% of the time?"**

"**Privacy correctness at 100% means fail-closed: when in doubt, deny.**

The architecture has multiple layers:

Layer 1 — At fan-out time: the MultiFeed Fan-out Worker pre-filters recipients based on audience type. For a 'Friends' post, it only fans out to the author's friends list (from TAO). This reduces the privacy check burden at read time because the post shouldn't be in non-friends' feeds in the first place. But pre-filtering at fan-out time is not sufficient alone — friend relationships can change after fan-out occurred.

Layer 2 — At read time: The Feed Service re-checks privacy for every post in the candidate set. For 'Public' posts, no check needed. For 'Friends' posts, the system checks the friendship cache (Redis, TTL 5 minutes, keyed on min/max of user_id pair to avoid duplication). Cache miss falls through to TAO (MySQL-backed graph DB). For 'Friends of Friends,' uses a pre-computed Bloom filter to approximate mutual friends check — false positive rate 1% (which means 1% of non-FoF users might pass the check, so we follow up with an exact TAO query on the slow path).

Layer 3 — Fail-closed on timeout: if the privacy check service doesn't respond within 100ms, the post is excluded from the feed. We don't include posts we can't verify privacy for. This means during a privacy service degradation, users see a slightly empty feed rather than seeing posts they shouldn't see.

Layer 4 — Cache invalidation on relationship change: when two users unfriend each other, a Kafka event triggers immediate Redis cache invalidation for their friendship entry. This closes the 5-minute stale window to under 1 second.

Trade-off: The Bloom filter for Friends-of-Friends creates a 1% false positive rate — 1% of non-friends-of-friends might have a post briefly show up in their feed candidate set. The exact TAO check on the false positive eliminates this at the cost of one extra TAO read. This is the right trade-off: most people are not friends-of-friends, so we avoid expensive TAO reads for the common case."

**Q3: "How does TikTok's FYP cold-start work for a brand new user?"**

"Cold-start is a **multi-phase bootstrap** that converges to personalization within 3–5 interactions.

Phase 1 (0 interactions): User selects 3+ interest categories during onboarding. The system maps each category to a pre-computed centroid embedding — the average embedding of all highly-rated videos in that category. The user's initial embedding is the mean of their selected category centroids. This means even on their very first FYP load, the two-tower ANN retrieval can find candidate videos with some relevance to stated interests.

Phase 2 (1–5 interactions): Exploration rate ε = 0.3 — 30% of FYP videos are randomly sampled from a high-quality pool outside the user's interest clusters. This intentionally diversifies early signals. Every interaction (especially watch completion %, swipe-away timing, like) is written to ClickHouse as an event. A fast online learning step applies a lightweight gradient update to the user's embedding after every 5 interactions — the embedding shifts toward content the user completed watching.

Phase 3 (5–30 interactions): The two-tower model has enough signal to retrieve meaningful candidates via FAISS. The multi-task ranker has watch% and like data to weight candidates. The exploration rate remains elevated to prevent early filter bubble formation.

Phase 4 (>30 interactions): Full personalization. The daily offline retraining job includes this user's interactions. Their embedding is updated in the nightly FAISS rebuild. The system is now fully personalized.

Trade-off unprompted: The fast online learning step (gradient update every 5 interactions) can introduce instability if the user's early interactions are atypical (watching content for non-interest reasons). The system mitigates this by capping the embedding update magnitude (learning rate 0.1) and weighting watch completion rate more heavily than like/dislike (since watch completion is harder to perform accidentally)."

**Q4: "Explain how the MultiFeed fan-out system at Facebook handles a post that reaches 100M+ views (a viral post from a public figure page)."**

"For a public figure page with 100M+ followers, the system works through the **group fan-out tiering** plus the **celebrity bypass in the MultiFeed system**.

First, the post is published to the Post Service, written to MySQL via TAO, and a `post.created` Kafka event is published.

The MultiFeed Fan-out Worker consumes the event. It checks: is this author above the page-follower threshold (100K for Facebook pages)? Yes. Decision: skip the precomputed-feed push to individual followers. The post is indexed in TAO by `(author_id, post_id)` but not written to any follower's Cassandra `user_feed` table.

At read time, the Feed Service for any follower of this page does NOT find this post in their precomputed MultiFeed cache (because it was never pushed). Instead, the Feed Service detects that the user follows this page (via TAO), fetches the page's recent posts from TAO directly (a fast association query), and merges them into the final ranked feed at read time alongside the user's precomputed feed from Cassandra.

The Ranking & Scoring Service ranks this page's post using the standard cascade (GBDT pre-score → Deep Cross Network full rank). For a truly viral post, `like_velocity` and `comment_velocity` in the Feature Store will be extremely high — the neural ranker will naturally score it near the top.

Trade-off: If a post goes viral extremely quickly (1M reactions in 10 minutes), the Feature Store's engagement velocity features might lag by 1 minute (Kafka → Scuba ingestion lag). During that window, the ranker may underweight the post. This is acceptable — the post will rank higher in the next feed load once velocity features catch up."

**Q5: "How would you redesign LinkedIn's connection degree computation if you suddenly had 10 billion members instead of 1 billion?"**

"At 10B members, the 2nd-degree problem is even more extreme — the theoretical maximum of 2nd-degree pairs goes from 48 trillion to 4,900 trillion. But the bloom filter approach scales gracefully because each bloom filter's size is determined by the number of 1st-degree connections per user, not the total member count.

The current approach (one 3KB Bloom filter per active member, encoding their 1st-degree connection set) scales linearly with active member count, not quadratically with total member pairs. At 10B members with a similar active ratio, you'd have ~2B active members × 3KB = 6 TB of Bloom filter data. That's manageable across a distributed Redis cluster.

The more interesting challenge is the **fan-out for high-degree nodes (viral posts)**. At 10B members, a post from a 'super-node' (e.g., a global company page with 500M followers) going viral requires even more care. My approach: impose a hard cap on fan-out of 10M writes per post_id per hour. Use Pinot's viral detection (engagement velocity query) to identify the post as viral. Switch from push fan-out to pull at read time for the overflow above the cap. At read time, any follower of the viral post's author sees it merged from TAO directly.

The Bloom filter false positive rate (1%) becomes more important at 10B scale. For a user with 500 1st-degree connections, a 1% false positive rate means ~5 false 'friends of friends' per connection check. Follow up with exact TAO queries for false positives. This is still tractable — 5 extra TAO reads per privacy check, and TAO has 99%+ cache hit rate, so most are sub-millisecond Memcache reads."

---

## Tier 3: Staff+ Stress Test Questions (reason aloud)

**Q1: "Twitter's engineering blog described their transition from a monolith to microservices. The timeline feature broke during the transition — follower timelines became stale for up to 5 minutes. Reason through why that happened and how you'd fix it."**

"Let me reason through this out loud.

The most likely failure mode is a **race condition in the dual-write transition period**. When moving from a monolith to microservices, there's usually an intermediate state where two systems (old monolith and new Timeline Service) are both writing to the timeline cache but using different keys, different TTLs, or different Sorted Set structures.

If the monolith was writing to Redis key `timeline:v1:{user_id}` and the new Timeline Service was reading from `timeline:v2:{user_id}`, the cache would appear empty on read (cache miss) and fall back to fan-out on read — which could take 1–2 seconds and produce a perceived 'staleness' if the fallback was returning stale data from a read replica with lag.

A more subtle failure: the Fan-out Worker was migrated to write to the new key format before the Timeline Service reader was deployed. During the deployment window, new tweets were being written to `timeline:v2:{user_id}` but the read path was still querying `timeline:v1:{user_id}` (a cache that was no longer receiving new entries). Result: the feed stops receiving new tweets — they appear stale.

Fix: **dual-write with feature flag.** During migration, both the old path and the new path receive every write. The read path switches atomically via feature flag once the new key format has been populated (with a warm-up period). The old write is deprecated only after the new read path has been confirmed correct in production.

Monitoring signal: tweet freshness age percentile (p99 of age-of-newest-tweet in timeline) should never exceed 60 seconds for an active user. A dashboard alert on this metric would have caught the 5-minute staleness within seconds of deployment."

**Q2: "Design a 'read your own writes' guarantee for social media. If I post a tweet, I must see it in my own timeline immediately, even if the fan-out hasn't completed for my followers."**

"This is a classic **session consistency** problem. The goal is: immediately after a write, the write is visible in reads from the same user session. Other users' eventual consistency is fine; the author's experience must be immediate.

There are several approaches:

Option A — Write-through to author's own cache: After the Tweet Service writes to Cassandra, immediately write the new tweet_id to the author's own Redis timeline before returning 200 to the client. This happens synchronously in the Tweet Service request path. The fan-out worker will later also write this to the author's cache, but with idempotent ZADD it's a no-op. This is the simplest approach — the author's timeline is updated inline.

Option B — Sticky session routing: Route all requests from the same user_id to the same Feed Service instance for 30 seconds after a write (via session affinity at the load balancer). That instance locally caches the just-written tweet. Subsequent feed reads from that instance see the tweet from local memory. This requires stateful session affinity, which is a load balancing complexity.

Option C — Client-side optimistic update: The client immediately inserts the new tweet into the top of the local feed (optimistic UI) without waiting for the server. If the server later returns a feed that doesn't include the tweet (because fan-out hasn't completed), the client detects the discrepancy and forces a refresh. This is common in mobile apps.

My recommendation: Option A (write-through for the author) combined with Option C (optimistic UI). Write-through for the author's own cache is a single Redis write inline in the request path, adding < 5ms. The client optimistically shows the tweet immediately. This gives the strongest UX with minimal architectural complexity."

**Q3: "Your social media platform is being used to coordinate a real-world event, and 50 million users all post within the same 60-second window (e.g., a TV show finale moment). Walk through every component that breaks and how you recover."**

"Let me trace the blast path of this event.

**Component 1: API Gateway / Load Balancers.** 50M posts / 60 seconds = 833K writes/sec, versus normal 5,800/sec baseline. That's a 143x spike. API Gateways are horizontally scaled stateless components — they'll auto-scale, but there's a lag (AWS ASG spin-up is 1–2 minutes). During the spin-up window, requests queue up. Mitigation: rate limit per user (max 10 tweets/5 minutes) — this caps the maximum from any single user and smooths the write spike. Pre-provisioned capacity for expected events (Super Bowl, finale) — you know these events in advance.

**Component 2: Tweet Service / Cassandra Write Path.** 833K Cassandra writes/sec vs. normal ~6K. Cassandra handles this via horizontal scaling of nodes. However, a 143x spike may push individual Cassandra vnodes into compaction pressure. Mitigation: pre-scale Cassandra cluster before the event (add nodes ahead of time), and use write batching (group Cassandra writes from multiple tweet service instances).

**Component 3: Kafka.** The topic partition for `tweet.created` receives 833K events/sec. Kafka can handle this — it's designed for millions of events/sec. However, consumer lag for Fan-out Workers will spike dramatically (50M events in 60 seconds, each requiring millions of Redis writes). Accept that fan-out will lag by minutes during the event. This is fine — eventual consistency. Mitigation: pre-scale Fan-out Worker instances.

**Component 4: Fan-out Worker / Redis.** 50M events × average 200 followers = 10 billion Redis writes queued. At normal throughput (~1M Redis writes/sec), this takes 10,000 seconds to drain (3 hours). This is unacceptable. Emergency mitigation: temporarily raise the celebrity threshold from 1M followers to 10K followers during the event — this causes the vast majority of users' tweets to bypass fan-out entirely, using the read-pull path instead. Announce this as a 'performance mode' trade-off. After the event, lower the threshold back and run a catch-up fan-out job.

**Component 5: CDN / Media.** 50M users embedding the same hashtag + shared media (screenshots, clips from the finale) will cause a massive CDN hit for the same media objects. CDN handles this well — high cache hit rate on popular objects. Mitigation: CDN pre-warming for anticipated media (clip from the finale that will be widely shared).

**Recovery signal:** Monitor tweet fan-out lag (measured as: time between `tweet.created` Kafka event and appearance in follower timelines) during the event. This metric spikes to 15+ minutes during the event. Once the Kafka consumer lag drops below 60 seconds, restore the normal celebrity threshold."

**Q4: "At Instagram scale, you're getting complaints that the Explore page is showing the same 5 types of content repeatedly — users are feeling 'trapped in a bubble.' How do you diagnose this and fix it architecturally?"**

"This is the **filter bubble problem** — a feedback loop in the recommendation algorithm that over-exploits a user's known interests and under-explores new ones.

Diagnosis first: I'd look at three signals: (a) Explore category diversity ratio per user — the Gini coefficient of content category distribution across a user's last 100 Explore views. Low Gini = high concentration = filter bubble. (b) Follow conversion rate from Explore — users discovering new creators they follow. This should be 2–5% per session; dropping below 1% indicates the algo is showing same creators repeatedly. (c) Session length trend — filter bubbles initially increase session length (high relevance), then cause drop-off as users get bored (shown in cohort analysis 2–4 weeks after bubble formation).

Architectural fixes:

Fix 1 — Explicit diversity injection: Reserve 15% of Explore positions (epsilon-greedy, ε = 0.15) for content from categories NOT in the user's top 5 interests. These are randomly sampled from high-quality posts in underexplored categories. Implement this as a post-ranking policy filter, not a change to the ranking model (faster to deploy, easier to tune).

Fix 2 — Category cap in candidate retrieval: During FAISS retrieval, enforce that no more than 30% of the 200 retrieved candidates can come from the same top-level category. This is a retrieval-time diversification constraint.

Fix 3 — MMR (Maximal Marginal Relevance) in re-ranking: Re-rank the final 200 candidates not just by relevance score but by (λ × relevance_score) + ((1 - λ) × diversity_from_already_selected). This naturally selects candidates that are both relevant AND different from what's already in the current page.

Fix 4 — 'Break the loop' signal: If a user has watched 20 consecutive Explore videos from the same creator or category without engaging (no like, comment, save, or follow), temporarily increase exploration rate to ε = 0.4 for that session.

Monitoring: After deployment, track category Gini coefficient improvement, follow conversion rate, and long-term session retention (users returning after 30 days). A/B test these changes — the trade-off is that initial engagement metrics (watch time, likes) may dip as the algo serves less immediately-relevant content, but long-term retention should improve."

**Q5: "LinkedIn's Feed Service uses a precomputed feed stored in Venice. Walk through exactly what happens to the feed cache for a user who hasn't logged in for 6 months and then comes back."**

"This is the **cold return problem** — a user returning after a long absence has a stale or expired feed cache.

When the user logs in after 6 months:

Step 1: API Gateway validates their OAuth token, confirms the token isn't expired (or triggers a re-auth flow).

Step 2: Feed Service requests feed for this member from Venice. Venice returns... either nothing (TTL of 7 days expired 5.5+ months ago) or stale data from 6 months ago (if Venice doesn't expire) with scores computed 6 months ago.

If Venice returns empty (TTL expired): Feed Service has a cold-return fallback path. It queries Espresso directly for the member's 1st-degree connections (via the Connection Service). For each 1st-degree connection, it fetches their most recent 5 posts from Espresso. This is a scatter-gather query: N connections × 1 Espresso query each. With 500 connections, that's 500 Espresso reads in parallel (with fan-out concurrency). Merge, score with the ranking model, write back to Venice for this member, and return the top 10. This cold-start read takes ~1–2 seconds — show a loading indicator in the UI.

If Venice returns stale data (6-month-old posts): The Feed Service detects staleness by checking the `added_at` timestamp of the most recent item. If all items are > 30 days old, treat as a cache miss and execute the cold-return fallback.

LinkedIn-specific optimization: the PYMK (People You May Know) Service runs a nightly Spark batch job that pre-computes recommendations for all members, including inactive ones, and stores them in Venice. When a returning member opens the feed for the first time, they'll see PYMK recommendations immediately (pre-computed) even before their post feed is fully reconstructed. This provides immediate social value while the feed reconstruction happens asynchronously in the background.

Post-reconstruction: after the cold-return feed is generated and written back to Venice, subsequent feed loads are fast (Venice cache hit). The Feed Fanout Worker also re-subscribes this member to future push fan-out events — their Venice entry is now active again."

---

# STEP 7 — MNEMONICS AND MEMORY ANCHORS

## The One Memory Trick: F.K.R.S.

When you sit down to design any social media feed, write four letters in the corner of the whiteboard: **F.K.R.S.**

- **F** = Fan-out Worker (how posts get to followers; always hybrid push/pull)
- **K** = Kafka (the async backbone decoupling every write from every downstream process)
- **R** = Redis (the precomputed timeline cache that makes reads fast)
- **S** = Snowflake IDs (globally unique, time-ordered, no coordinator needed)

These four components appear in every single social media problem. If you've designed them well, you've answered 70% of what the interviewer wants to know. The remaining 30% is problem-specific: media pipeline for Instagram, FYP model for TikTok, privacy filter for Facebook, degree computation for LinkedIn, trending for Twitter.

## The Celebrity Threshold Numbers

Memorize these — they come up in every interview as a follow-up:
- Twitter, Instagram, TikTok: **1 million followers** → skip push fan-out
- Facebook pages: **100K followers** → skip push fan-out
- LinkedIn: **5K connections** or **100K page followers** → skip push fan-out

## The Opening One-Liner

When the interviewer says "design Twitter" or "design a social media feed," open with this:

"At its core, this is a read-amplification problem: one write must become a fast read for potentially millions of people. My design will center on a hybrid fan-out strategy — push to followers for most users, pull at read time for celebrity accounts — with a Redis timeline cache per user, Kafka for async decoupling, and Cassandra as the durable write store. Let me start by clarifying the requirements."

This opening signals in 25 seconds that you know the core problem, you have a solution direction, and you're going to do requirements before drawing. Every interviewer relaxes when they hear this.

---

# STEP 8 — CRITIQUE AND GAPS

## What's Covered Well in This Pattern

**Fan-out mechanics:** The hybrid push/pull model is deeply covered across all five problems, with specific thresholds, implementation pseudocode, and trade-offs for every scenario (celebrity crossing threshold, inactive user cache expiry, dead-letter queue for failed fan-outs).

**Data model choices:** Each problem provides detailed schemas for every table — Cassandra, MySQL, Redis Sorted Sets — with justification for each choice. The Cassandra vs. MySQL vs. Redis decision is fully argued for each use case.

**Capacity math:** All five problems include detailed traffic estimates, storage estimates, and bandwidth calculations with specific numbers. These can be referenced as anchor numbers in any interview.

**Component depth:** Fan-out Worker, Trending (Count-Min Sketch + Flink), Media Pipeline (pre-signed URLs, async transcoding), Privacy Filter (fail-closed, bloom filters, cache invalidation), and FYP recommendation (two-tower + FAISS + multi-task ranking) all have implementation-level pseudocode.

## What's Missing or Shallow

**Real-time features:** WebSocket / server-sent events for live feed updates, live comment streams (especially for TikTok Live), and notification delivery via push (APNS/FCM) are mentioned but not deep-dived. If an interviewer asks "how do new posts appear in a user's feed in real time without them pulling to refresh?" you need to explain a WebSocket-based push notification pathway.

**Geographic distribution / multi-region:** All five problems mention active-active multi-region deployment and GeoDNS routing, but the specifics of data residency enforcement (e.g., GDPR requiring EU user data to stay in EU), cross-region replication conflicts, and the decision of when to accept cross-DC replication lag vs. when to require synchronous writes are only lightly covered.

**Content moderation pipeline:** All five problems explicitly mark this as out of scope, but at staff-level interviews at companies like Meta or TikTok, you may be expected to know the high-level hooks: async content scanning pipeline (hash-based photo matching via PhotoDNA for known CSAM, NLP classifier for harmful text, ML image classifier for NSFW), appeals workflow, and human review queues.

**Ad serving integration:** Facebook News Feed explicitly notes ads every ~5 posts; TikTok every 5th post in some markets. The ad auction architecture is out of scope, but how ads are interleaved (as synthetic feed items with `type: "ad"` in the response, injected by an Ad Server at specific positions post-ranking) is worth a sentence.

## Senior Interviewer Probes (Things They Will Ask)

1. "What happens to the Kafka consumer lag during a global event like a World Cup final?" (Fan-out lag spike, temporary threshold elevation, capacity pre-provisioning)

2. "You said Cassandra for tweets — what's the compaction strategy, and what happens when you have hot partitions for viral tweets?" (LeveledCompactionStrategy for tweet-by-user reads; hot partitions can be mitigated by prefix-salting the partition key or using a secondary read replica cluster for popular accounts)

3. "How does the ranking model handle a user who is actively trying to game the algorithm?" (Engagement bait detection classifier, hide/report feedback as strong negative signals, per-user engagement velocity anomaly detection, diversity constraints that cap any single creator)

4. "You're using Redis for timeline cache. What's your plan when Redis memory fills up?" (Eviction policy: LRU by default; but better to cap proactively with ZREMRANGEBYRANK per user; TTL means inactive users auto-expire; Redis memory tiering to SSD for less-frequently-accessed entries — Memcached with SSD backing is Instagram's approach)

5. "Why not store the full tweet object in Redis instead of just the tweet_id?" (Because the tweet object includes like_count and retweet_count that change over time. Storing the full object means stale engagement counts on every read. Storing only tweet_id forces a Cassandra read for hydration — Cassandra is the source of truth for counts. The trade-off is one extra read per post display, but accuracy is worth it. For post hydration, use a Redis object cache with short TTL (~60s) for recently-fetched posts to amortize the Cassandra reads across multiple users viewing the same viral post.)

## Common Candidate Traps

**Trap 1: Designing fan-out on write for every user without mentioning celebrities.** If you don't proactively bring up the celebrity fan-out problem and the hybrid approach, the interviewer will ask about it as a gotcha. You'll lose 10 minutes recovering. Bring it up yourself during the High-Level Design step.

**Trap 2: Using a relational database (MySQL) as the primary post store.** MySQL with a single primary can handle ~10,000 writes/sec. Twitter needs 25,000+ at peak. Candidates who use MySQL for posts either don't propose sharding (fails at scale) or propose Vitess sharding (correct, but they should explain why Cassandra is better for append-only write-heavy workloads).

**Trap 3: Forgetting cache invalidation.** When a tweet is deleted, when a user unfollows someone, when a privacy setting changes — what happens to the stale cache entries? Interviewers at FAANG will probe this specifically. The correct answer varies by case: soft-delete means hydration filters it, unfollows mean the next cache TTL will naturally exclude new posts (old posts remain in cache until they scroll off), privacy changes require a retroactive scan with eventual removal.

**Trap 4: Proposing strong consistency for feeds.** If you say "every follower must see a tweet within 1 second of posting, with strong consistency guaranteed," you'll be asked to justify that with a working architecture. It can't be done at this scale within the latency SLAs. Embrace eventual consistency for feeds proactively and explain why the user-facing impact is acceptable.

**Trap 5: Not knowing the difference between TikTok and every other platform.** Every other platform in this pattern is social-graph-based. TikTok's FYP is recommendation-based. If you design TikTok's FYP as a fan-out problem, you've fundamentally misunderstood the product. The first clarifying question should reveal this: "Is the feed based on accounts the user follows, or is it recommendation-based?" For TikTok FYP specifically, the answer is recommendation-based — which means your architecture centers on a two-tower neural network, FAISS index, and ClickHouse feature store rather than a fan-out worker.

**Trap 6: Forgetting to mention Kafka.** If you describe a synchronous write path where Post Service directly calls Fan-out Worker, you've created tight coupling and a blocking write path. Kafka is the answer, and you should introduce it proactively in the High-Level Design with a one-sentence justification: "I'll use Kafka here to decouple the write path from the fan-out, so post creation doesn't wait for fan-out to complete, and fan-out workers can scale independently."

---

*This guide is self-contained. Every concept, number, trade-off, and Q&A answer is written at the level of detail needed for a senior or staff-level system design interview. No source files need to be read.*
