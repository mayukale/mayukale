Reading Pattern 17: Recommendations — 3 problems, 8 shared components

---

# Pattern 17: Recommendations — Complete Interview Study Guide

**Problems covered:** Recommendation Engine, Collaborative Filtering System, News Feed Ranking

**Shared components:** Multi-stage candidate funnel, Kafka event ingestion, Redis feature store, ClickHouse interaction log, S3 + Delta Lake offline storage, TF Serving model deployment, Flink stream processor, Bloom filter for seen-item suppression

---

## STEP 1 — ORIENTATION

These three problems are members of the same family. They all solve the same fundamental challenge: given a user and a catalog of content that is too large to fully scan at serving time, return the most relevant items fast enough that it feels instant. The surface manifestation differs — one is a product page, one is a "users like you also liked" engine, one is a social media feed — but the underlying machinery is nearly identical.

**Why this pattern comes up constantly in FAANG interviews:** Every company at scale has a recommendation problem. Netflix's entire value prop is showing you the right movie. YouTube lives and dies by what autoplay suggests. Amazon's recommendations drive 35% of revenue. Facebook's feed ranking is the core product. If you can articulate the architecture clearly and defend your design choices under pressure, you demonstrate that you understand the single highest-leverage engineering challenge in consumer tech.

**The three problems in brief:**

- **Recommendation Engine** — General-purpose personalized item ranking. Think Netflix homepage or YouTube autoplay. The sophistication is in the retrieval (Two-Tower + FAISS) and ranking (DCN-v2) stack. 500K RPS peak, 100M items, 150 ms P99.

- **Collaborative Filtering** — A specific algorithmic approach: find patterns in who liked what, not what the item contains. Produces explainable "Because you liked X" recommendations. The sophistication is in ALS matrix factorization and the decision to pre-compute 240 GB of item-item similarity tables to achieve sub-millisecond serving.

- **News Feed Ranking** — The hardest of the three. You must rank fresh user-generated content from a social graph within 200 ms at 1M RPS, while handling the fan-out write problem for celebrity accounts. The sophistication is in the 5-stage pipeline, multi-task learning (CTR + dwell + share + comment simultaneously), and the push-pull hybrid fan-out.

---

## STEP 2 — MENTAL MODEL

**Core idea:** Recommendations are a two-phase problem: first, find a small set of candidates that could plausibly be relevant (the recall problem); then, precisely rank that small set using expensive signals (the precision problem). You never run the expensive ranking model against the full catalog. The shape of every recommendation system is a funnel.

**Real-world analogy:** Imagine you are a librarian helping someone find a book. You do not read every one of 10 million books and then hand them the best one — that would take years. Instead, you first quickly walk to the relevant section of the library (candidate retrieval — cheap, approximate, fast) and pull 50 books off the shelf. Then you spend a minute actually scanning those 50 to pick the 5 best ones for this specific person (ranking — expensive, precise, slow). The library's physical organization (genre sections, subject indices) is your embedding space and ANN index. The moment you stop thinking of recommendations as "find the best item" and start thinking of it as "quickly find a good candidate set, then precisely rank it," the entire architecture makes sense.

**Why this is hard — three genuine difficulties:**

1. **Scale mismatch.** You have 100 million items and need to return 20 in 150 ms. That is 0.0000002 ms per item if you naively score them all — physically impossible. The ANN (approximate nearest neighbor) retrieval step exists solely because of this constraint. Understanding this constraint is what separates candidates who designed a recommendation system from candidates who are describing one they once read about.

2. **The cold-start paradox.** The recommendation system improves with more interaction data, but new users and new items have no data. You need the system to work well enough on day one to collect the data that makes it work better on day two. Breaking this chicken-and-egg problem requires a deliberate fallback hierarchy: content embeddings, onboarding signals, popularity, bandit exploration.

3. **Feedback loop degradation.** Whatever you optimize for, you will see more of. Optimize for clicks and you get clickbait. Optimize for watch time and you get addictive or extreme content. Optimize for engagement and bots game your signals. The objective function choice and the correction mechanisms (IPS for position bias, composite labels, delayed retention metrics) are where senior engineers demonstrate judgment that junior engineers miss.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before touching the whiteboard, ask these. Each answer meaningfully changes the design.

**Question 1: What type of items are we recommending and how large is the catalog?**

What changes: A 10,000-item catalog changes almost everything. You might not need ANN at all — a brute-force scan is feasible. At 100M+ items, ANN is mandatory. Item type (video, product, post) changes what features are meaningful and how cold-start works.

**Question 2: What is the scale — how many users, DAU, and peak RPS?**

What changes: Netflix-scale (100M DAU, 70K RPS) requires different infrastructure than a startup (10K DAU, 10 RPS). At high RPS you need in-process FAISS, Redis hot cache, and stateless horizontal scaling. At low RPS you can do online computation.

**Question 3: How quickly must user actions affect recommendations — seconds, minutes, or hours?**

What changes: "Seconds" forces session-based streaming inference, which is expensive. "Minutes" (the typical answer) allows a Flink stream processor that updates features in Redis within 5 minutes. "Hours" lets you get away with batch-only pipelines. This question determines whether you need a real-time streaming path at all.

**Question 4: Is there a social graph involved, or is this purely content-based personalization?**

What changes: A social graph fundamentally changes candidate generation (you pull from followed accounts' recent activity) and adds the fan-out write problem. Without a social graph, candidate generation is purely ANN over the item embedding space.

**Question 5: Do you need explainability — can the user see why an item was recommended?**

What changes: "Because you watched Inception" requires item-item CF with pre-computed similarity tables. If explainability is required, a pure neural approach (Two-Tower, DCN-v2) won't give you a clean explanation, and you need to either use interpretable models or build a separate explanation layer. This is a significant architectural fork.

**Red flags to watch for during clarification:**

- Interviewer says "just make it work for all cases" — they are testing whether you can identify that trade-offs exist. Press for specifics anyway, then propose a prioritized design.
- The requirements seem to imply real-time model retraining at serving time. Push back: "Do you mean real-time feature updates to a pre-trained model, or actually retraining the model weights in real-time?" Retraining at serving time is almost never the right answer.
- Interviewer skips the "what is success" question. You should ask: "How do we measure quality? CTR? Watch time? Long-term retention?" The answer shapes the objective function and, critically, reveals the risk of short-term metric gaming.

---

### 3b. Functional Requirements

**Core requirements (for a general recommendation engine — adapt per problem):**

1. Given a user, return N ranked items (default 20) within a latency budget.
2. Accept interaction events (clicks, watches, purchases, skips, ratings) from clients asynchronously.
3. Handle users with no history (cold-start) and items with no interaction data (item cold-start).
4. Honor hard constraints: suppress items the user has already seen, filter age-restricted or geo-blocked content, suppress items the user has explicitly blocked.
5. Support A/B testing: route a configurable percentage of traffic to experimental models without code deploys.

**Scope boundaries to explicitly call out:**

- Search ranking is out of scope (a separate information retrieval system).
- Ad recommendation is out of scope (a separate auction/bidding system with different economics).
- Content moderation is out of scope (a trust and safety system feeds filtered item IDs; the recommendation system trusts that list).
- Model training infrastructure (the MLflow/SageMaker pipeline) is assumed; the focus is on serving and feature engineering.

**Clear statement:** "We are building the serving-time recommendation system and its supporting data pipelines. The system accepts user and context signals, retrieves and ranks items from a large catalog, and returns a ranked list. It is not responsible for generating the items (content creation) or moderating them (trust and safety)."

---

### 3c. Non-Functional Requirements

**How to derive these instead of just reciting them:**

Start from the business context. "Netflix prime-time is 8-10 PM. If recommendations are slow or wrong during those 2 hours, we lose a significant fraction of daily watch time. That drives the latency and availability targets." Then work component by component.

**Latency:** End-to-end P99 < 150 ms at the API gateway. Break it down: candidate retrieval 20 ms, feature fetch 10 ms, model scoring 30 ms, business rules 5 ms, serialization 10 ms. That totals 75 ms P50 with headroom for tail behavior. The reason P99 matters more than P50 is that the users who experience slow recommendations are often the most active users (who generate the most value).

**Throughput:** 70,000 RPS (recommendation engine scale) or up to 1M RPS (news feed scale). Always justify: DAU × requests per user per day / seconds in a day × peak multiplier × safety buffer.

**Availability:** 99.99% (< 52.6 min downtime/year). The critical design implication: you must have a fallback that works when the ML model is unavailable. Popularity-based ranking is the standard fallback — it degrades gracefully without producing nonsense.

**Freshness vs. consistency trade-off:** ✅ Eventual consistency is acceptable. A user's last interaction can take up to 5 minutes to influence recommendations. This is the key trade-off that enables the entire batch + streaming hybrid architecture. If you required strong consistency (every action affects the next recommendation immediately), you would need synchronous online model updates, which is 10-100x more expensive. ❌ Strong consistency for the recommendation content itself is not needed — a 5-minute lag is invisible to users.

**Privacy:** No PII stored in the feature store. User IDs are pseudonymized (hashed). GDPR right to erasure handled within 30 days (delete user embedding, purge interaction log rows, remove from training data).

**Trade-offs to explicitly call out:**

- ✅ Recall (showing good content) vs. ❌ Latency: more ANN candidates = better recall but slower. Industry sweet spot is nprobe=64 in FAISS, giving 98.3% recall@100 in < 8ms.
- ✅ Model freshness (frequent retrains) vs. ❌ Training cost: nightly retrains are the standard balance. Real-time retraining costs 10x+ more for marginal quality gains.
- ✅ Personalization depth vs. ❌ Cold-start coverage: the more personalized the model, the worse it handles new users. The solution is a fallback hierarchy, not a single model that handles both.

---

### 3d. Capacity Estimation

**The formula to drive all estimates:**

```
Daily requests = DAU × requests_per_user_per_day
Average RPS = Daily requests / 86,400
Peak RPS = Average RPS × peak_hour_multiplier × safety_buffer
```

**Anchor numbers for recommendation engine (streaming platform scale):**

| What | How | Result |
|---|---|---|
| DAU | Given | 100M |
| Requests/user/day | Homepage + 3 widgets + 2 scroll refreshes | 6 |
| Daily API calls | 100M × 6 | 600M/day |
| Average RPS | 600M / 86,400 | ~7K RPS |
| Peak RPS (5x + 2x safety) | 7K × 5 × 2 | ~70K RPS |
| Items per response | Given | 20 |
| Feedback events/day | 100M × 40 events/user | 4B events/day |
| Feedback RPS | 4B / 86,400 | ~46K RPS |

**Storage anchors:**

- User embeddings (256-dim float32): 256 × 4 bytes × 500M users = **500 GB**
- Item embeddings: 256 × 4 × 100M items = **100 GB**
- Item-item similarity (CF): 100M items × 200 neighbors × 12 bytes = **240 GB** (fits in Redis)
- User-user similarity (CF): 500M × 500 neighbors × 12 bytes = **3 PB** — infeasible, never pre-compute this
- FAISS IVF_PQ index (32-byte PQ codes): 100M × 32 bytes = **3.2 GB per serving pod** (in-memory)
- Interaction log: 4B events/day × 200B/event = **800 GB/day → 290 TB/year**
- News feed user inbox: 500M users × 100 candidates × 8 bytes = **400 GB**

**Architecture implications to call out explicitly (this is where you score points):**

- "500 GB of user embeddings means we cannot put these in a single Redis node. We need a 64-shard Redis Cluster with 32 GB/shard."
- "3.2 GB for the FAISS index fits in the memory of a standard serving pod. We run FAISS in-process as a sidecar, which eliminates a network hop and is why ANN lookup takes 5 ms rather than 50 ms."
- "The 3 PB user-user similarity table is physically infeasible. That is why collaborative filtering at scale uses matrix factorization (ALS) with 256 GB of user factors, not neighborhood-based user-user CF."
- "290 TB/year of interaction logs cannot be queried with standard row-store PostgreSQL. That is why we use ClickHouse — columnar storage compresses this 10x to ~30 TB/year with 50-100x faster aggregation queries."

**Time budget (P99 latency breakdown for recommendation engine):**

```
Candidate retrieval (FAISS ANN):         5 ms
User embedding fetch (Redis):            2 ms
Item feature batch fetch (Redis):        8 ms
Model scoring (TF Serving, 500 items):  15 ms
Business rules + diversity:              3 ms
Serialization + network:                10 ms
Total P50:                              ~43 ms
P99 (2-3x overhead):                   ~150 ms
```

---

### 3e. High-Level Design

**4-6 components every recommendation system needs:**

1. **API Gateway / Load Balancer** — Authentication (JWT), rate limiting, TLS termination, A/B bucket routing. Every recommendation request starts here. The A/B bucket routing is critical: deterministic assignment by `hash(user_id) % 100` means the same user always goes to the same experiment bucket within a session.

2. **Recommendation Serving Service** — Stateless orchestrator. Calls the candidate retrieval, feature store, and ranking model in sequence. Horizontally scalable. Logs every served recommendation to Kafka (rec_token, model_version, feature snapshot, ranked items) for offline analysis.

3. **Candidate Generation Layer** — Either FAISS ANN over embeddings (recommendation engine), pre-computed Redis ZSET similarity lookups (collaborative filtering), or social inbox pull from Redis + trending injection (news feed). The goal is to go from 100M items to 500 candidates in < 20 ms.

4. **Feature Store** — Redis Cluster for hot-path serving (sub-millisecond reads), S3 + Delta Lake for offline training snapshots. This is the single most important component for production ML quality — training-serving skew happens when your online feature store computes features differently from your offline training pipeline.

5. **Ranking Model** — TF Serving (or Triton) serving a versioned neural model. For recommendation engine: DCN-v2 on 150 features. For news feed: MTL DNN on 120 features. For collaborative filtering: in-process NumPy dot products (no GPU needed for 500 × 128-dim).

6. **Event Ingestion Pipeline** — Kafka for inbound interaction events → Flink for real-time feature updates (5-min lag) → Batch training pipeline (Spark + Airflow, nightly) → model artifacts back to TF Serving. This is the "learn from users" loop that makes the system improve over time.

**Data flow for a homepage recommendation request (whiteboard order):**

```
1. Client GET /recommendations → API Gateway validates JWT, assigns A/B bucket
2. Recommendation Serving Service starts pipeline:
   a. Fetch user embedding from Redis         [~2 ms]
   b. ANN query FAISS → 500 candidate IDs    [~5 ms]
   c. Batch-fetch item features for 500       [~8 ms]
   d. Assemble 500 × 150 feature matrix
   e. TF Serving → 500 scores                [~15 ms]
   f. Apply business rules (filter, diversity)[~3 ms]
   g. Sort, take top-20
3. Log impression → Kafka (async, non-blocking)
4. Return JSON with 20 ranked items + rec_token
Total: ~40 ms P50
```

**Key decisions to justify on the whiteboard:**

- "I am using FAISS in-process rather than a separate vector database service to eliminate a network RTT on the hot path. The 3.2 GB index fits in pod memory."
- "I return 202 Accepted for feedback events and publish to Kafka rather than writing to the database synchronously. At 46K feedback RPS, synchronous DB writes would require hundreds of DB nodes and would bottleneck on disk I/O."
- "I use Redis for features rather than DynamoDB because Redis HGET retrieval is 0.1-0.3 ms vs 1-5 ms for DynamoDB. At 70K serving RPS, that difference compounds into a significant latency gap."

**Whiteboard order (draw in this sequence):**

Client → API Gateway → Serving Service (show the internal pipeline stages) → Feature Store (Redis) → FAISS Index → TF Serving → Kafka → Flink → Batch Pipeline → S3. Draw the read path first (serving), then the write path (feedback → Kafka → pipelines → features back into Redis and model registry).

---

### 3f. Deep Dive Areas

**Area 1: Cold-Start (most frequently probed, affects all three problems)**

**The problem:** A new user has 0 interactions. The two-tower model needs a user embedding to do ANN retrieval. ALS needs interaction data to compute latent factors. Without either, what do you do?

**The solution hierarchy (important: present this as a hierarchy, not a single answer):**

- **Level 1 — Onboarding signals (if available):** When the user signs up, show them a topic picker. Map their selections to category centroid embeddings. Average the centroids to get an initial user vector. This is not a real personalized embedding, but it is better than random.
- **Level 2 — Session context (always available):** Device type, time of day, country, referral URL. A user arriving from a cooking blog probably wants cooking content. These signals cost nothing and require no interaction history.
- **Level 3 — Popularity + geo fallback (always available):** Trending content in the user's country. Not personalized, but a much better baseline than random or globally popular items (a US user should not be served Hindi content just because it is globally popular).
- **Level 4 — Thompson Sampling bandit (for item cold-start):** Reserve 2 of the 20 recommendation slots for items younger than 48 hours. Use Thompson Sampling (`θ ~ Beta(1 + successful_engagements, 1 + non_engagements)`) to select exploration candidates. New high-quality items surface quickly; low-quality items self-select out without manual curation.

**The graduation threshold:** Industry research and empirical data both point to 10-20 interactions as the point where personalized models begin to significantly outperform demographic/popularity baselines. Below 20 interactions, blend cold-start and personalized signals. Above 20, use full personalization.

**Trade-offs to call out unprompted:**

- ✅ Onboarding quiz improves first-session quality. ❌ Requires user effort; ~40% of users skip it. So it cannot be the only strategy.
- ✅ Thompson Sampling naturally deprioritizes bad new items. ❌ Items that appeal to a niche but not the majority get systematically under-sampled. Use diversity boosts to counteract.

---

**Area 2: Training-Serving Skew (often ignored by candidates, always probed by seniors)**

**The problem:** The model was trained on features computed one way. At serving time, the same features are computed by a different code path. The model receives inputs it was not trained on and performance degrades silently. This is one of the most common production ML failures.

**The solution (three levels of enforcement):**

1. **Shared feature computation library.** The same Python package computes features both in the Spark offline training pipeline and inside the serving container. Version-pinned: model artifact metadata records `feature_transformer==2.3.1`. When the serving container starts, it validates that its feature transformer version matches what the model was trained with.

2. **Point-in-time correct feature store reads during training.** When assembling the training dataset, each (user, item, label) triple is joined with features as they existed at the time of the event, not as of batch creation time. If you train on features from today to predict clicks from 6 months ago, you have data leakage from future features. Use the feature store's `get_historical_features(entity_id, timestamp)` API.

3. **Distribution monitoring at serving time.** Compute the KL divergence between the distribution of each feature in training data vs. its distribution in real-time serving traffic. If KL divergence exceeds a threshold (e.g., 0.1 for critical features), fire an alert before model quality degrades enough to show up in A/B metrics. This is your canary for "the world has changed since we trained this model."

---

**Area 3: Fan-out for Celebrity Accounts (unique to news feed, always probed)**

**The problem:** A celebrity with 50 million followers publishes a post. In a pure push-on-write system, the fan-out service must write the post_id to 50 million followers' inbox tables within 30 seconds (the freshness SLA). At 1,000 Redis ZADD operations per second per worker, this would require 50,000 seconds = 14 hours. That is obviously infeasible.

**The solution — hybrid push-pull:**

- **Push path (≤ 10K followers):** When a post is published, a fan-out worker reads the author's follower list and writes the post_id to each follower's `feed:inbox:{follower_id}` Redis ZSET immediately. For 99% of accounts, this completes in under 1 second.
- **Pull path (> 10K followers, the "celebrity path"):** The post is stored in `feed:celebrity_posts:{celebrity_user_id}` as a Redis ZSET (post_id → timestamp). When any follower loads their feed, the Feed Orchestration Service asks the Graph Service: "which high-follower accounts does this user follow?" and fetches each celebrity's recent posts on demand (pull-on-read). This adds ~5 ms of read latency for each celebrity followed, but eliminates the millions-of-writes problem entirely.
- **The threshold (10K followers) is configurable** and operationally tuned. A celebrity who gained 100K followers overnight might temporarily be in the wrong path until the daily recomputation.

**Trade-offs to call out:**

- ✅ Push path: low read latency, posts always ready in inbox when user opens app. ❌ Write amplification: 1 post → N writes. Infeasible for large N.
- ✅ Pull path: constant write cost regardless of follower count. ❌ Higher read latency proportional to number of celebrity accounts followed. A user following 20 celebrities adds ~100 ms to feed loading.

---

### 3g. Failure Scenarios

**Scenario 1: FAISS index pod crash (affects recommendation engine and CF)**

The FAISS index is loaded in-process in the serving pod. If the pod crashes, that serving pod becomes unhealthy. Kubernetes detects the failed readiness probe and removes the pod from the load balancer pool within 30 seconds. Traffic reroutes to surviving pods. The replacement pod starts and loads the 3.2 GB FAISS index from S3 in ~2 minutes (10 Gbps internal network). During that 2 minutes, the surviving pods absorb the load (this is why you maintain 2x headroom in pod count). Secondary fallback: item-item CF pre-computed neighbors in Redis are available as a candidate source that does not require FAISS. The system degrades gracefully, not catastrophically.

**Scenario 2: Redis Feature Store unavailable (high severity)**

Redis unavailability affects every single recommendation request because all feature lookups go through Redis. Primary mitigation: Redis Cluster with master + 1 read replica per shard, Sentinel quorum for automatic failover in < 30 seconds. If the entire cluster is unavailable (DC-level failure): serving pods fall back to a local in-memory LRU cache (1 GB per pod) of the most recently used features. This covers ~70% of hot users. For users not in the local cache, fall back to popularity-based ranking with no features. The fallback serves a degraded but functional feed, preventing a complete outage.

**Senior framing:** "The interesting question is not 'what happens when Redis fails' but 'how do we make Redis failure survivable?' That means designing the fallback path first, making sure the popularity ranking backend is always warm (not just 'ready to start'), and measuring the blast radius: what percentage of users are in the local in-process cache at any given time?"

**Scenario 3: Kafka lag spike during viral event (affects all three)**

During a breaking news event, engagement events can surge 10-50x above normal. Kafka brokers can absorb the write burst (Kafka is designed for multi-GB/s ingest). The risk is that Flink stream processors fall behind. If Flink is 10 minutes behind, the trending signals in Redis are stale. The mitigation: Flink jobs have auto-scaling configured (more task slots on lag). The trending ZSET in Redis has a 1-hour TTL, so stale data expires automatically. For the serving path, popularity-weighted fallback ensures results are still reasonable even without up-to-the-minute trending signals. The lesson: decouple the "something trending" detection from the per-user feature updates — trending can tolerate more lag than user preference features.

---

## STEP 4 — COMMON COMPONENTS

Every component below appears in at least two of the three problems. For each, understand: why it is used, the key configuration that makes it work at scale, and what the system looks like without it.

---

### Multi-Stage Candidate Generation → Ranking → Filtering Pipeline

**Why used:** The ranking model (DCN-v2, MTL DNN) is too expensive to run against every item in a 100M-item catalog at serving time. Running 100M items through a neural ranker at 70K RPS would require thousands of GPUs. The funnel architecture narrows candidates to a tractable set before hitting the expensive component.

**How the funnel looks per problem:**

- Recommendation engine: 100M items → **500 ANN candidates** (FAISS) → DCN-v2 scoring → top-20 returned
- Collaborative filtering: 100M items → **500 FAISS candidates or pre-computed Redis ZSET** → dot-product scoring → top-20 returned
- News feed: social inbox (1,200) + trending (100) + ANN interest (100) + viral (50) = **1,500 candidates** → heuristic pre-filter (→ 500) → MTL DNN → top-100 diversity-filtered → **25 per page**

**Key config:** The candidate set size (500 for rec engine, 1,500 for news feed) is a deliberate trade-off between recall quality and downstream cost. Larger candidate sets improve recall but increase feature fetch and model scoring cost linearly. Industry benchmarks show diminishing recall returns past ~1,000 candidates.

**Without it:** You either score every item (infeasible at 70K+ RPS) or score so few items that recall is poor. Pre-2015 Netflix literally scored everything — it was only feasible because their catalog was smaller.

---

### Kafka for Engagement Event Ingestion

**Why used:** Interaction events arrive at 46K-347K RPS (depending on scale). No database can absorb this as synchronous writes at that rate. Kafka decouples the ingest path (fast, fire-and-forget) from the processing path (slower, durable). It also provides replay capability: if your Flink job has a bug, you can fix it and reprocess the last 7 days of events from Kafka.

**Key config:**

- Topics partitioned by `user_id % 128`. This co-locates all events for a user on the same partition, which is important for Flink jobs that maintain per-user state (e.g., session windows).
- Replication factor 3. Tolerates 2 broker failures without data loss.
- Return **202 Accepted** immediately after Kafka publish. Never acknowledge before the event reaches Kafka (risk of loss), never wait for DB write (too slow).
- Idempotency: `event_id` (UUID) is stored in a 24-hour deduplication window. Client retries on network failure will not double-count events.

**Without it:** Synchronous DB writes on the ingest path. At 46K events/s with a 5ms DB write, you would need 230 DB connections just for ingest, and any DB hiccup backs up to the client. Kafka absorbs the burst and smooths the processing rate.

---

### Redis Cluster as Feature Store for Hot-Path Serving

**Why used:** The recommendation serving path needs user embeddings (1 KB), item features (400 B per item × 500 candidates = 200 KB), and user features (200 B) on every request. These must be fetched in under 10 ms total. Redis HGET latency is 0.1-0.3 ms vs 1-5 ms for DynamoDB. At 70K RPS, each millisecond of latency difference compounds into meaningful P99 impact.

**Key config per problem:**

- Recommendation engine: `emb:user:{user_id}` → HSET {vec: 1KB binary, model_ver, ts}; `emb:item:{item_id}` → HSET; `filter:user:{user_id}` → Bloom filter ~10KB per user for seen-item suppression; `trending:global` → ZSET TTL 1h
- Collaborative filtering: `cf:item_sim:{domain}:{item_id}` → ZSET top-200 similar items with similarity scores (TTL 25h); `cf:user_factors:{domain}:{user_id}` → HSET {vec 512B, bias, version}; `cf:user_topn:{domain}:{user_id}` → ZSET pre-computed top-100 (TTL 2h)
- News feed: `feed:inbox:{user_id}` → ZSET post_id → timestamp (TTL 7d); `feed:post:eng:{post_id}` → HSET engagement counters (TTL 72h); `feed:seen:{user_id}` → Bloom filter (TTL 30d)
- Cluster configuration: 64 shards, consistent hashing with CRC16, master + 1 read replica per shard, Sentinel quorum for auto-failover in < 30 seconds.

**Without it:** Feature lookups from ClickHouse or S3 would add 50-500 ms per request. The entire latency budget would be consumed by feature fetching alone.

---

### ClickHouse for Interaction Log Storage

**Why used:** All three problems generate massive interaction logs: 4B events/day (rec engine), 3B/day (CF), 30B/day (news feed). These logs are the source of truth for batch model training. ClickHouse is chosen because: (1) columnar storage compresses time-series event data 10-100x vs row-store PostgreSQL, reducing storage cost by an order of magnitude; (2) vectorized aggregation queries run 50-100x faster, which matters when nightly ALS training needs to scan 100B rows; (3) MergeTree writes sustain 1M+ events/s per node without schema locks.

**Key config:**

- MergeTree engine, partitioned by `toYYYYMM(event_ts)`, ordered by `(user_id, event_ts)` for user-scoped range scans.
- ReplicatedMergeTree with ZooKeeper-managed replication across 3 replicas per shard.
- 8 shards, hash-distributed by `sipHash64(user_id)` for even load distribution.
- The ALS training query: `SELECT user_id, item_id, log1p(COUNT(*)) AS rating FROM interactions WHERE domain='movies' AND event_ts >= now() - INTERVAL 90 DAY GROUP BY user_id, item_id` — this aggregation is fast (seconds) in ClickHouse vs hours in row-store.

**Without it:** PostgreSQL with time-based partitioning can work up to ~1-2B events before query latency for batch training becomes prohibitive. At 100B+ non-zero entries (CF scale), PostgreSQL is infeasible; Cassandra has poor aggregation performance; ClickHouse is the standard choice.

---

### Offline Batch Training + Online Serving Pattern (Nightly Cycle)

**Why used:** Full model retraining at serving time is economically infeasible. A DCN-v2 retrain takes 6 hours on 8×A100 GPUs. ALS takes 2.5 hours on 100 Spark executors. These jobs run nightly (or on a configurable cadence via Airflow). The gap between training and serving is bridged by real-time feature updates via Flink: the model weights are stale (up to 24 hours old) but the features they score are fresh (updated within 5 minutes).

**Key config:**

- Recommendation engine: DCN-v2 nightly retrain, 8×A100 80GB, ~6 hours/epoch; 90-day rolling interaction window as training data.
- Collaborative filtering: Spark MLlib ALS, 100 c5.2xlarge executors, 15 iterations, 2.5 hours; rank=128 latent factors; α=40 confidence for implicit feedback.
- News feed: MTL DNN nightly retrain + LightGBM GBDT pre-ranker retraining.
- TF Serving shadow/canary/full-rollout deployment sequence prevents any downtime during model version transitions.

**Without it:** Either you never retrain (model quality degrades as user preferences drift and catalog changes), or you retrain too frequently (expensive, risky, high engineering overhead). The nightly cycle is the empirical sweet spot between quality and cost.

---

### S3 + Delta Lake for Offline Feature Snapshots

**Why used:** Training data lives in S3. Delta Lake adds ACID transactions to Parquet files on S3, which matters because: (1) partial writes from a failed Spark job cannot corrupt the training dataset; (2) time-travel queries let you reproduce any historical training run exactly (critical for debugging regressions); (3) point-in-time correct reads ensure training labels and features are temporally aligned.

**Key config:** Each interaction log partition (daily or weekly) is a separate Delta table. Model artifacts (embeddings, ALS factors) are versioned by model_version in S3 paths (`s3://ml-models/als/v8/user_factors/`). A new model version triggers a Redis Loader job to push the new factors to Redis atomically.

**Without it:** Without ACID transactions on S3 (plain Parquet), a failed nightly training job leaves partial writes that corrupt subsequent jobs. Without time-travel, debugging "why did the model quality drop 3 days ago?" requires manual reconstruction of what the training data looked like on that day.

---

### TF Serving for Versioned Model Deployment

**Why used:** TF Serving provides zero-downtime model version transitions, native request batching for GPU efficiency, and a gRPC Protobuf interface with low serialization overhead. The three-phase deployment (shadow → canary → full rollout) is the standard safe deployment pattern for ML models in production.

**Key config:**

- **Shadow mode:** New model version loads alongside current. All requests scored by both; only current version's scores are served. Monitor AUC and feature distributions for 24 hours.
- **Canary:** 5% of traffic routed to new model via A/B experiment bucket. Monitor CTR, watch time, P99 latency for 48 hours. Automated rollback if CTR drops > 3% or P99 > 50 ms.
- **Full rollout:** 5% → 25% → 50% → 100% over 6 hours.

**Note on collaborative filtering:** CF uses in-process NumPy dot products, not TF Serving. The MF scoring (500 candidates × 128 dims) is trivially fast on CPU (0.5 ms). No GPU required.

**Without it:** Manual model swaps require downtime or complex custom deployment logic. Canary testing without a framework requires building your own A/B routing layer. TF Serving is the standard solution that avoids reinventing this.

---

### 202 Accepted Feedback Ingestion (Async, Idempotent)

**Why used:** All three problems ingest feedback at 46K-347K events/s. The only way to handle this without a massive DB write cluster is to accept the event on the HTTP path (validate, deduplicate), publish to Kafka (< 5 ms), and return 202 immediately. Downstream consumers (Flink, batch jobs) process at their own pace.

**Key config:** Every event has an `event_id` (UUID). The ingest service maintains a 24-hour deduplication Bloom filter per topic. Duplicate `event_id` within 24 hours is silently dropped, making ingest idempotent. Clients can safely retry on network failure.

**Without it:** Synchronous writes at 46K events/s require ~230 DB connections at 5ms/write. Any DB hiccup creates cascading timeouts visible to users. The async Kafka path means a Kafka hiccup affects only ingest availability, not the serving path.

---

### Bloom Filter for Seen-Item Suppression

**Why used:** After ranking, you must filter out items the user has already seen. A naive approach — look up every candidate in a `seen_items` table — adds O(500 queries) per request. A Bloom filter in Redis answers "has this user seen this item?" in O(1) with a sub-millisecond Redis GET.

**Key config:** ~10 KB per user (for a 1% false-positive rate over 100K seen items). False positives are acceptable: the item is occasionally suppressed when it shouldn't be. False negatives are not possible (the filter never says "not seen" for something that was seen). TTL 30 days.

**Without it:** Either store a `user_item_seen` table (huge storage, slow lookup) or skip suppression entirely (users see items they already consumed, which is the biggest recommendation quality complaint).

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Recommendation Engine

**Unique things this problem requires that the others do not:**

The recommendation engine is the most "full-stack ML" of the three. It requires a complete neural retrieval and ranking stack: a **Two-Tower embedding model** trained with in-batch softmax loss (τ=0.07 temperature) to generate 256-dim user and item vectors; a **FAISS IVF_PQ index** (nlist=4096, M=32 sub-spaces, nbits=8, nprobe=64) holding 100M items in 3.2 GB in-process per serving pod with 98.3% recall@100; a **DCN-v2 ranking model** with 6 cross-network layers explicitly modeling interactions across 150 features (50 user + 60 item + 40 cross); **Maximal Marginal Relevance (MMR)** post-ranking diversity with configurable λ per surface (0.8 for search sidebar, 0.6 for homepage); and a **delta flat index** for items published in the last 24 hours that cannot yet be in the nightly-rebuilt IVF_PQ index.

The key design decisions unique to this problem are: (1) training the Two-Tower model jointly (not separately) so that the user and item embedding spaces align for ANN retrieval; (2) the dual-index pattern to handle item freshness without full index rebuilds; (3) the composite training label (watch fraction > 0.5 OR explicit like), not raw CTR, to avoid clickbait optimization; (4) IPS (Inverse Propensity Scoring) correction for position bias in the DCN-v2 training data.

**Two-sentence differentiator:** The Recommendation Engine is the general-purpose personalized ranking system — its uniqueness is in the Two-Tower + FAISS IVF_PQ + DCN-v2 stack that takes 100M items to a 20-item ranked list in 40 ms. It is the only problem that requires building and serving a neural embedding retrieval model, an ANN index, and a feature interaction ranking model as three separate but tightly integrated systems.

---

### Collaborative Filtering

**Unique things this problem requires that the others do not:**

Collaborative filtering is explicitly about learning from the pattern of "who liked what" without looking at what the item actually is. The unique components are: **ALS (Alternating Least Squares) Matrix Factorization** with Hu et al. (2008) confidence weighting for implicit feedback (`c_ui = 1 + 40 × f_ui` where f is interaction frequency; unobserved items get confidence = 1, not 0); the **pre-computed 240 GB item-item similarity table** in Redis (top-200 similar items per item, stored as ZSET, TTL 25h) which enables sub-millisecond "Because you liked X" lookups; the deliberate decision to **not pre-compute user-user similarity** (which would require 3 PB — instead, user-user CF is served at query time via ALS factor dot products); and the **streaming ALS update** for power users (re-compute individual user factor `p_u = (Q^T Q + λI)^(-1) Q^T r_u` in Flink every 30 minutes without full retraining).

The key design insight unique to CF: the difference between explicit (star ratings) and implicit (views/clicks) feedback is not just the signal type — it requires a completely different mathematical formulation. Explicit: minimize squared prediction error on observed entries. Implicit: all entries have a preference value (1 if interacted, 0 if not), but the model is more confident about interactions than non-interactions.

**Two-sentence differentiator:** Collaborative Filtering's unique challenge is decomposing a 500M × 100M, 99.9998%-sparse interaction matrix into meaningful latent factors that explain user preferences without knowing anything about item content — and doing so in a way that produces explainable "Because you liked X" recommendations at sub-millisecond serving latency. The 240 GB item-item similarity table pre-computed nightly by Spark is the key trade-off: large enough to hold meaningful neighborhood context, small enough to fit in Redis, and precise enough (ALS factor cosine, not raw co-occurrence) to give high-quality explanations.

---

### News Feed Ranking

**Unique things this problem requires that the others do not:**

News feed ranking is structurally different from the other two because the content is ephemeral (posts age out in 72 hours), the candidate set is primarily determined by the user's social graph (not ANN over a static catalog), the scale is 10-15x higher (1M RPS vs 70K), and there is a unique write-path problem: **fan-out**. The unique components are: the **hybrid push-pull fan-out architecture** (push-on-write for ≤ 10K followers, pull-on-read celebrity path for > 10K) which is the most important design decision in the problem; the **5-stage pipeline** (candidate generation → feature assembly → heuristic pre-filter → MTL DNN ranking → filtering + diversity); **Multi-Task Learning DNN** trained simultaneously on CTR, share rate, comment rate, and dwell time (with share weighted 5× and report weighted -5×); **LightGBM GBDT pre-ranker** that reduces 1,500 to 500 candidates before the DNN (97% recall@200 in 2-5 ms vs 20-40 ms for DNN on 1,500); **Wilson Score** confidence-adjusted CTR to prevent low-sample posts from gaming the metric; **freshness decay with content-type-specific half-lives** (news: 60 min, general: 360 min, evergreen: 2880 min); and **engagement velocity** for viral detection (Flink 1-hour sliding window, posts in top 0.01% force-added to trending ZSET).

The key design insight unique to news feed: this is the only problem where the candidate generation is primarily social-graph-driven (pull from inboxes of followed accounts) rather than ANN-over-catalog. The catalog of "things to recommend" is constantly changing (new posts every second) and most candidates were already decided at write time (when the post was published and fan-out ran).

**Two-sentence differentiator:** News Feed Ranking is the hardest of the three because it must solve the fan-out problem (how do you get a Beyoncé post to 50M followers in 30 seconds?) and the ranking problem simultaneously at 1M RPS — the push-pull hybrid fan-out architecture with a 10K follower threshold is the single most important design decision. The multi-task DNN that simultaneously optimizes CTR, dwell, share, and comment rate prevents the clickbait trap of single-objective optimization while the LightGBM pre-ranker reduces DNN inference cost by 7.5× without meaningful quality loss.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (Expected at All Levels)

**Q: Why do you use approximate nearest neighbor (ANN) search instead of exact cosine search for candidate retrieval?**

**KEY PHRASE: "100M dot products per request is infeasible."** Exact cosine search over 100M items at 256 dimensions requires reading 100M × 256 × 4 bytes = 100 GB of data per query. At a 70K RPS serving rate, that is 7 petabytes per second of memory bandwidth — physically impossible on any hardware. FAISS IVF_PQ solves this by (a) dividing the vector space into 4,096 Voronoi cells and only searching 64 of them (1.5% of items), and (b) compressing each 256-dim float32 vector (1,024 bytes) to a 32-byte product quantization code, fitting 100M items in 3.2 GB of in-process memory. The recall cost is 1.7% (98.3% recall@100 vs 100%), which translates to less than 0.5% NDCG@10 degradation — a well-validated acceptable trade-off.

---

**Q: Why return 202 Accepted for interaction events instead of 200 OK after a synchronous write?**

**KEY PHRASE: "46K events per second cannot be absorbed by a transactional database."** At 46,000 events per second, even a fast database write (5 ms) would require 230 persistent connections just for ingest, and any database hiccup would back up to the client response time. By returning 202 immediately after publishing to Kafka (< 2 ms), the ingest path is decoupled from the storage path. The trade-off is eventual consistency: a user's action may take up to 5 minutes to influence their recommendations. This is completely acceptable for a recommendation system, but would not be acceptable for, say, a financial transaction.

---

**Q: How do you handle a new user with no interaction history?**

**KEY PHRASE: "Cold-start is a hierarchy, not a single solution."** No single approach handles all cold-start scenarios well. The hierarchy is: (1) onboarding quiz maps topic selections to category centroid embeddings (~60% of users complete this); (2) session context signals — device type, country, time of day, referral URL — available for 100% of users; (3) geo-filtered popularity (trending in your country) as the always-available baseline; (4) Thompson Sampling bandit reserves 2 of 20 recommendation slots for content exploration that is self-correcting (bad items deprioritize themselves without manual curation). The user "graduates" to full personalization after 10-20 interactions.

---

**Q: Why ClickHouse instead of PostgreSQL or Cassandra for the interaction log?**

**KEY PHRASE: "Columnar storage gives 10-100x better aggregation performance at this event scale."** PostgreSQL with partitioning works fine at 1-2 billion events. At 100 billion non-zero entries (collaborative filtering scale) or 30 billion events per day (news feed scale), batch training jobs that need to aggregate `GROUP BY user_id, item_id` over the full history would take hours to days in a row-store. ClickHouse's vectorized columnar engine runs the same aggregations in minutes because it reads only the `user_id` and `item_id` columns from disk rather than every column of every row. It also compresses time-series event data 10x better than row stores, reducing storage cost by an order of magnitude.

---

**Q: What is training-serving skew and how do you prevent it?**

**KEY PHRASE: "The same feature computation code must run in both the offline training pipeline and the online serving path."** Training-serving skew occurs when features are computed differently at training time (in Spark or Python offline) versus serving time (in the Java/Go/Python serving container). The model learns from one distribution and receives another. Prevention has three levels: (1) a shared, version-pinned feature transformation library used by both paths; (2) point-in-time correct feature retrieval during training (features as they existed at the event timestamp, not as of batch creation); (3) KL-divergence monitoring between serving feature distributions and training feature distributions, with alerts when divergence exceeds a threshold.

---

**Q: How do you deploy a new model version without downtime?**

**KEY PHRASE: "Shadow → canary → full rollout, with automated rollback triggers."** Three phases: Shadow mode loads the new model version alongside the current one. All requests are scored by both, but only the current version's scores are served — you observe quality metrics without any user impact. Canary routes 5% of traffic to the new model via the A/B experiment platform. If CTR is within ±2% of control and P99 latency is within 5 ms after 48 hours, proceed. Full rollout shifts traffic 5% → 25% → 50% → 100% over 6 hours, with automated rollback if CTR drops more than 3% or P99 exceeds the SLA at any stage.

---

**Q: Why pre-compute item-item similarity for CF instead of computing it at serving time?**

**KEY PHRASE: "Pre-computation turns an O(I × F) per-request problem into an O(1) lookup."** Computing the similarity between a query item and all 100M other items at serving time costs O(100M × 128) = 12.8B floating point operations per request. At 100K serving RPS, that is 1.28 quadrillion FLOPS per second — requiring thousands of GPUs just for similarity computation. Pre-computing and caching the top-200 similar items per item in a Redis ZSET (240 GB total, rebuilt nightly) converts serving to a single ZGET: O(1), sub-millisecond. The cost is nightly re-computation (a Spark job that runs in a few hours) and 240 GB of Redis memory.

---

### Tier 2 — Deep Dive Questions (Probed at Senior/Staff Level)

**Q: Why does ALS use confidence weighting (c_ui = 1 + α × f) for implicit feedback rather than just treating interactions as binary positive labels?**

**KEY PHRASE: "Not-seen is not the same as not-liked."** With binary labels (interacted = 1, not-interacted = 0), the model treats every unseen item as a negative signal. A user who has not watched Schindler's List is not necessarily signaling dislike — they may not know it exists. Treating non-interactions as strong negatives would prevent the model from recommending items outside the user's current bubble. Hu et al. (2008) solved this by defining a preference value (1 if interacted, 0 if not) separately from a confidence value (how much to trust that label). Non-interactions get confidence = 1 (weak negative: "we believe they wouldn't like this, but not strongly"). An item watched 5 times gets confidence = 1 + 40×5 = 201 (very strong positive). The optimization problem then asks: fit the model to observed entries with high confidence and to unobserved entries with low confidence. ✅ This prevents the model from being dominated by the vast majority of zeros in the sparse matrix. ❌ The trade-off is a more complex optimization objective that requires the full confidence-weighted ALS update step rather than the simpler explicit-feedback update.

---

**Q: Why does the recommendation engine optimize for watch fraction > 0.5 rather than CTR? And what are the remaining limitations of that choice?**

**KEY PHRASE: "CTR optimization creates a clickbait feedback loop; watch fraction is better but still not perfect."** CTR measures the user's decision to click a thumbnail, which is easily manipulated: misleading thumbnails, shocking headlines, and curiosity gap titles all drive CTR without delivering satisfying content. Facebook's 2016 research showed CTR-optimized feeds produced 35% more clickbait complaints than dwell-time-optimized feeds. Watch fraction (did the user watch > 50% of the video?) measures actual content consumption, which is much harder to game artificially. The remaining limitation: watch fraction still does not capture long-term user wellbeing. A user addicted to emotionally manipulative content might have high watch fraction but low 7-day retention. The full objective should be a composite: 0.5 × completion_rate + 0.3 × explicit_positive_signal + 0.2 × 7_day_return_rate. The 7-day return rate label requires a delayed join (recommendation served today, outcome measured 7 days later) which adds pipeline complexity.

---

**Q: What is position bias and how do you correct for it in ranking model training data?**

**KEY PHRASE: "Items shown first get clicked more regardless of quality — and your training data knows this."** Position bias is the empirical observation that items at the top of a recommendation list receive more clicks because users see them first, not necessarily because they are more relevant. If you train on raw click data, the model learns that "shown at position 1 → clicked" and scores items higher that were historically ranked first, creating a self-reinforcing feedback loop. Correction via Inverse Propensity Scoring (IPS): estimate the probability that a user would click an item if it appeared at position k (the "propensity" P(k)). Weight each training example by 1/P(k). If P(1) ≈ 0.42 and P(10) ≈ 0.08, a click at position 10 is 5.25× more informative than a click at position 1, because the user had to actively choose that item despite its low visibility. The propensity curve is estimated by periodically randomizing the ranking for a holdout traffic slice and measuring position-click relationships on the unbiased data.

---

**Q: How do you ensure the FAISS index stays current as new items are published throughout the day, without taking it offline for rebuild?**

**KEY PHRASE: "Dual-index pattern: main IVF_PQ index + delta flat index for items published < 24 hours ago."** FAISS IVF_PQ does not support efficient incremental inserts because adding a new item might require re-assigning it to an existing Voronoi cell, which is fine, but the product quantization codebook was trained on the full corpus — inserting items one by one would not change the codebook but would also not be reflected in the existing index structure correctly. Instead: the main IVF_PQ index is rebuilt nightly (4 hours on 32 CPU cores). For items published between rebuilds, a "delta flat index" is maintained — a brute-force FAISS Flat index over the ~100K new items published in the last 24 hours (100K × 32 bytes = 3.2 MB, trivially small). At serving time, candidates are drawn from both indexes and merged. At nightly rebuild, delta items are folded into the main index. This pattern is used in production at Pinterest and LinkedIn.

---

**Q: The news feed has both a push and a pull path for fan-out. How do you handle a user who transitions from below the 10K follower threshold to above it? What about during the transition?**

**KEY PHRASE: "The threshold is configurable and checked at fan-out time, not at account creation."** When a user's follower count crosses 10K, the system transitions them from push to pull on the next post they publish. In-flight fan-outs that started on the push path complete normally. For the transition period (user just crossed the threshold, has some followers who received push fan-out for recent posts and some who will now use pull), there is a short window where their posts exist in both systems. This is handled by the Feed Orchestration Service: when fetching the social inbox, it checks both the push inbox ZSET and the celebrity pull source. Since both paths ultimately serve the same post_id, deduplication is handled by the seen-item Bloom filter. The transition window is at most the 24 hours until the next follower count recomputation. A sudden viral event (creator gains 1M followers overnight) would be caught on the next hourly threshold check and migrated to pull path — during that hour, fan-out workers would see unusually high load, which is absorbed by Kafka backpressure (workers slow down rather than crashing).

---

**Q: If you had to pick one thing that a solid candidate always gets wrong about recommendation systems, what would it be?**

**KEY PHRASE: "Conflating feature freshness with model freshness."** Almost every candidate says "we need to retrain the model in real-time to reflect user actions." This is wrong, expensive, and unnecessary. Model weights encode learned patterns from historical data — they change slowly. What needs to be fresh is the features that feed the model: the user's recent interactions, the item's current engagement counters, trending signals. A well-designed feature store updated by Flink within 5 minutes (for features) + a model retrained nightly (for weights) gives you nearly the same quality as real-time retraining at 100x lower cost. The correct framing is: "real-time features on a batch-trained model," not "real-time model."

---

**Q: How do you measure the quality of a recommendation system? What are the relevant offline and online metrics?**

**KEY PHRASE: "Offline metrics tell you if the model is learning; online metrics tell you if users are better off."** Offline metrics (computed before deployment): NDCG@K (normalized discounted cumulative gain — measures ranking quality across the top-K recommendations), AUC-ROC (discrimination ability of the ranking model), Recall@100 for the ANN candidate retrieval step. These are fast to compute, reproducible, and catch regressions before they reach users. Online metrics (from A/B tests): CTR (click-through rate), completion rate (watch fraction), share rate, 7-day retention, and session length. North star metric is typically 7-day retention because it captures whether recommendations are making users' lives better long-term. The danger: CTR and completion rate can be gamed by the model in ways that hurt retention. Always include at least one long-lag metric (7-day) in your success criteria to catch short-term metric gaming.

---

### Tier 3 — Staff+ Stress Tests (Reason Aloud)

**Q: You notice that your recommendation model's NDCG@10 on offline evaluation has been improving for 6 months, but 7-day user retention is flat. What happened and how do you diagnose it?**

This is a classic "metrics divergence" problem that indicates your offline evaluation is no longer tracking the real user value. Reason through it: NDCG@10 measures whether the model puts "good" items at the top, where "good" is defined by your training labels. If training labels are defined as "items the model has historically ranked highly that users clicked," you have label leakage from position bias — the model is learning to rank the same items it already ranks well. Offline NDCG rises because the model gets better at predicting its own past decisions, not because it finds truly better items. Diagnosis: (1) Check the training label distribution — are positive examples disproportionately from positions 1-3? (2) Look at category diversity in offline test sets vs. online served results. If offline diversity is higher, the model is being evaluated on data that does not match serving conditions. (3) Run a randomization experiment: shuffle 5% of rankings randomly, train on those examples, compare NDCG. If NDCG on randomized training data is lower but retention is higher, you have position bias in your normal training data. Fix: stronger IPS correction, include retention labels, separate the evaluation set from training distribution.

---

**Q: You are asked to reduce recommendation serving latency from P99=150ms to P99=50ms without increasing infrastructure cost. Walk through your approach.**

Start by decomposing the current latency budget: candidate retrieval (~20ms), feature fetch (~10ms), model scoring (~30ms), business rules (~5ms), serialization (~10ms), network overhead (~10ms), plus tail behavior multiplier. To hit 50ms P99, you need to cut roughly 100ms of P99 overhead. The most impactful changes, in order: (1) **Feature fetch parallelization:** if user features, item features, and the ANN query are not already concurrent, running them in parallel removes the largest sequential chunk. (2) **Reduce candidate set from 500 to 200:** NDCG@10 degrades by roughly 1% for each 2x reduction in candidate set below 500 — validate this offline. If the degradation is acceptable, you just cut model inference cost by 2.5x. (3) **Model quantization:** convert DCN-v2 from float32 to int8 inference. This typically cuts inference time 2-4x with < 1% quality loss. (4) **Serving pod colocation:** ensure FAISS sidecar, TF Serving, and Redis are all within the same availability zone to cut network RTTs. (5) **Pre-computed feed cache:** for users with predictable behavior patterns, pre-compute and cache the recommendation list every 2 hours. At 20% cache hit rate, you eliminate 20% of full pipeline executions entirely. If all these are insufficient without infrastructure cost increase, you need to challenge the premise — 50ms P99 for a full neural ranking pipeline at scale is very aggressive and may require hardware acceleration (GPU colocation with RDMA networking) that does not exist in the current setup.

---

**Q: Design the A/B experimentation platform for this recommendation system such that you can run 20 simultaneous experiments without user overlap bias, statistical interference, or model quality degradation.**

This is a systems design sub-problem within the recommendation design. The key challenges: (1) User assignment must be deterministic and stable (same user must always get the same bucket for the experiment duration). Use `hash(experiment_id || user_id) % bucket_count` — this gives stable, reproducible assignment. (2) Simultaneous experiments must not contaminate each other. The standard solution is **orthogonal experiment layers**: each experiment is assigned to a different "layer" (dimension), and users are independently assigned to each layer. A user can be in treatment for layer 1 (ranking model) and control for layer 2 (diversity rules) simultaneously. (3) Statistical interference happens when one experiment's treatment changes user behavior in a way that affects another experiment's metrics (e.g., a ranking experiment that increases engagement inflates the CTR numbers seen by a concurrent CTR-measuring experiment). Mitigation: run high-risk experiments in dedicated disjoint traffic slices (holdout groups), not overlapping layers. (4) Model quality degradation: if too many experiments run simultaneously with incompatible model versions, the serving fleet ends up with high memory pressure (10 model versions loaded simultaneously). Use shadow mode + canary rather than full A/B for preliminary model experiments. (5) Metric tracking: each experiment must independently track its metrics without interference from other experiments' effects on the same users. Use the `rec_token` → feedback correlation to attribute each engagement event to the specific experiment bucket that served the recommendation.

---

**Q: Your engagement velocity metric is detecting viral content well, but you are now seeing systematic gaming: content creators have learned that posting at 3 PM on Tuesdays (a low-baseline time) makes their content look viral because velocity is compared to a low hourly baseline. How do you fix this without breaking genuine viral detection?**

This is a practical adversarial robustness question. The bug: `velocity = engagement_1h / (engagement_24h / 24)`. When the 24h baseline is low (Tuesday 3 PM), even modest 1h engagement produces a high velocity ratio. Fix this in layers: (1) **Seasonal baseline normalization:** instead of dividing by the 24h rolling average, divide by the expected baseline for this specific hour-of-week (i.e., the average engagement for "Tuesday 3 PM" from the historical distribution). This makes velocity a deviation from seasonal expectation, not a deviation from recent average. (2) **Absolute floor:** a post must have both high velocity AND a minimum absolute engagement count (e.g., at least 1,000 interactions in the 1h window) before being added to the trending ZSET. This prevents a post with 10 shares (but 0.001 baseline) from appearing as viral. (3) **Author history normalization:** a creator who consistently posts at low-baseline times should have their velocity compared to their own historical baseline for that time slot, not the global baseline. (4) Monitor for this by tracking the time-of-post distribution of items added to the trending ZSET — if it skews heavily toward known low-baseline windows, the gaming is active and needs the seasonal normalization fix.

---

## STEP 7 — MNEMONICS

### The "CRAFT" Funnel Mnemonic

Every recommendation system is built on the same pattern. Remember **CRAFT**:

- **C** — Candidates: Narrow from catalog (100M) to candidates (500-1,500) using ANN or pre-computed neighbors. This step must be fast and high-recall.
- **R** — Rank: Score each candidate with an expensive model (DCN-v2, MTL DNN, ALS dot product). This step must be accurate and within latency budget.
- **A** — Apply rules: Business logic, filters, hard constraints (seen items, geo blocks, age restrictions, spam suppression). These are deterministic and fast.
- **F** — Fresh features: Everything the model sees must come from a fast, accurate feature store (Redis). Stale features = stale recommendations.
- **T** — Test and tune: A/B experimentation platform controls every model rollout, diversity parameter, and business rule. Nothing ships without measurement.

When you are stuck at the whiteboard, ask yourself: "Have I addressed all five CRAFT steps?"

---

### The "Tiered Freshness" Mental Model

Commit to memory: there are three time scales in a recommendation system, and each needs different infrastructure:

- **< 30 seconds:** Fan-out and engagement counter updates (Flink → Redis INCR). New posts from followed accounts appear in feed. Real engagement velocity updates.
- **< 5 minutes:** User feature updates (Flink → Redis HSET). The model's view of the user's recent behavior. Session context.
- **24 hours:** Model retraining (Spark + Airflow → TF Serving). The model weights themselves. Embedding tables. Item-item similarity.

Any time an interviewer asks about "real-time" recommendations, clarify which time scale they mean. "Real-time features on a batch-trained model" is the correct architecture for 99% of use cases. "Real-time model retraining" is almost never the right answer.

---

### Opening one-liner for the interview

When you start your answer, say this: "A recommendation system has two fundamental problems: a recall problem (how do you find the 500 relevant items among 100 million quickly?) and a precision problem (how do you rank those 500 items accurately within your latency budget?). Everything in the architecture is a consequence of solving those two problems at scale. Let me start by clarifying which problem we are primarily solving here."

This immediately signals architectural maturity and frames the entire discussion.

---

## STEP 8 — CRITIQUE OF SOURCE MATERIAL

### Well-Covered Areas

The source material is exceptionally thorough in the following areas:

**Mathematical depth:** ALS derivation (Hu et al. 2008 confidence weighting, implicit feedback adaptation, ALS update rule), DCN-v2 architecture with formal cross-network formulation, Two-Tower in-batch softmax loss with temperature τ, Wilson Score CTR normalization, IPS correction for position bias. This level of mathematical specificity is rare and valuable — it means you can answer quantitative follow-ups without hand-waving.

**Numbers, numbers, numbers:** Every architecture decision is backed by capacity estimates. The 3 PB user-user similarity infeasibility calculation, the 3.2 GB FAISS IVF_PQ calculation, the 240 GB item-item Redis calculation, the 2.5 hours ALS training time. Being able to derive these on the whiteboard is what separates senior candidates from junior ones.

**Failure modes and fallbacks:** Every component has an explicit fallback described (FAISS unavailable → CF neighbors in Redis, ML models unavailable → popularity ranking, Redis full → local in-process LRU cache). Senior interviewers specifically probe for this.

**Training data quality:** Position bias via IPS, composite labels vs raw CTR, delayed retention labels for 7-day feedback. These are the kinds of nuances that separate engineers who have shipped recommendation systems from engineers who have read about them.

---

### Gaps, Shallow Coverage, or Areas Needing Supplementation

**Sparse coverage: The two-tower model training pipeline.** The source describes the model architecture and inference well, but is light on how the Two-Tower model is actually trained in production. Key gaps: how do you handle hard negatives during training (in-batch softmax tends to have mostly easy negatives)? How do you prevent the user tower and item tower from collapsing (all embeddings converging to the same point)? Supplementation: in production (Google, Pinterest, Twitter), Two-Tower models use hard negative mining — after each batch, identify items the model nearly correctly ranked (high score for a non-interacted item) and add them as hard negatives in the next batch.

**Shallow: Embedding drift and model staleness.** The source covers nightly retraining well but does not discuss what happens to recommendation quality as the model ages intra-day. A model trained at midnight with yesterday's data may already be 20 hours old by prime-time. For rapidly evolving catalogs (news feed), item embeddings trained yesterday miss the top stories of today. Supplementation: this is why news feed ranking relies heavily on real-time engagement features (updated by Flink) rather than static item embeddings — the freshness signal from real-time engagement counters partially compensates for stale item embeddings.

**Missing: Multi-armed bandits beyond cold-start.** The source covers Thompson Sampling for cold-start item exploration but does not discuss bandit algorithms for the broader explore-exploit problem in recommendation. Upper Confidence Bound (UCB) bandits and contextual bandits (LinUCB) are used at production scale to balance exploration (showing new diverse content) with exploitation (showing reliably high-engagement content). Supplementation: Netflix uses a variant of Thompson Sampling for the entire recommendation problem, not just cold-start, treating each recommendation slot as a bandit arm.

**Missing: Real-time session-based recommendations.** The source focuses on the daily-model + streaming-features paradigm. It does not cover transformer-based sequential models (BERT4Rec, SASRec) that model the full sequence of actions within a session. These are increasingly used as re-rankers or for the "more like what I'm watching right now" use case. Supplementation: session transformers are generally too slow (50-200 ms) to use for full candidate retrieval but are viable as a re-ranker on a short candidate list (top-50 from the main ranker) for high-value session moments (autoplay, "up next" decisions).

**Shallow: Multi-objective ranking and multi-stakeholder optimization.** The source mentions multi-task learning (CTR + share + comment + dwell) but does not discuss the broader problem of balancing competing stakeholder objectives: user engagement vs. creator monetization vs. platform revenue vs. societal impact. In practice, the ranking model's loss function includes terms for multiple business goals weighted by quarterly business priorities. Supplementation: the constrained optimization formulation (maximize engagement subject to: creator diversity ≥ 20%, news content ≥ 5%, ads ≤ 15%) is a more honest representation of production recommendation systems than pure engagement maximization.

---

### Senior Probes You Must Prepare For

1. "How do you handle GDPR right-to-erasure for a user who requests deletion while their interaction data is in Kafka, being processed by Flink, and also in the Delta Lake training dataset?" (The honest answer: Kafka replay window is 7 days, so data is gone when it expires. Delta Lake requires a targeted delete on the user_id partition. The user embedding in Redis is deleted immediately. Model retraining on the purged dataset happens in the next nightly cycle. There is a window of up to 30 days where the user's patterns might still be encoded in model weights that were trained on their data — GDPR compliance for model weights is an active legal/technical question.)

2. "Your ALS model has 128 latent factors. If you increase to 256 factors, training time roughly doubles and serving memory roughly doubles. When is that trade-off worth it?" (Answer: measure NDCG@10 on your held-out test set. Plot NDCG vs. factor count. If the marginal gain in NDCG from 128→256 is < 0.5% while memory cost doubles, it is not worth it. In practice, the factor count that matters most for CF quality is whether you have enough factors to capture the long tail of niche interests, not the overall global quality metrics.)

3. "How does your recommendation system behave if Kafka is down for 2 hours?" (Answer: ingest returns 503 for feedback events — clients should buffer and retry. Serving is unaffected: model weights and features in Redis were written before the outage. Feature freshness degrades: Flink is not consuming from Kafka, so engagement counters stop updating. After 2 hours, trending signals are 2 hours stale but items are not meaningfully mis-ranked. Recovery: Flink consumers resume from the last committed Kafka offset and reprocess the 2-hour backlog over the next 30-60 minutes.)

---

### Traps to Avoid

**Trap 1: Saying "we retrain the model in real-time."** This is the most common wrong answer. Real-time weight updates require online learning algorithms (not gradient descent on a GPU cluster), which have worse quality and are extremely hard to make safe. Always distinguish between "real-time features" (achievable and necessary) and "real-time model retraining" (almost never the right answer).

**Trap 2: Forgetting to mention the fallback.** Every component can fail. If you describe FAISS without saying "and if FAISS is unavailable, we fall back to item-item CF neighbors in Redis," the senior interviewer will ask about it. Build the fallback into your initial design.

**Trap 3: Designing user-user similarity pre-computation at scale.** The candidate who proudly announces "I will pre-compute the top-500 similar users for each of 500M users" has just proposed storing 3 petabytes in Redis. Always compute this number before committing to the design. User-user CF at scale is served via ALS factor dot products, not pre-computed similarity tables.

**Trap 4: Treating CTR as the north star metric.** The most common optimization trap. If an interviewer asks "how do you know your recommendation system is working?" and you answer "we look at CTR," expect a probe about clickbait. Always lead with a composite metric that includes both a short-term signal (completion rate, share rate) and a long-term retention signal.

**Trap 5: Ignoring the write path.** Many candidates design the serving (read) path thoroughly and then handwave the feedback ingestion (write) path. "We just write interactions to the database" at 46K events/s is wrong. The write path (Kafka → Flink → Redis → batch retraining) is half the system and half the interesting engineering. Be prepared to defend every hop in the write path.

---

*End of INTERVIEW_GUIDE.md — Pattern 17: Recommendations*
