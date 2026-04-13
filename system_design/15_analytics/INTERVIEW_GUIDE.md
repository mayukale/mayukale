# Pattern 15: Analytics — Interview Study Guide

Reading Pattern 15: Analytics — 4 problems, 6 shared components

**Problems covered:** Ad Click Aggregation, Web Crawler, Realtime Leaderboard, Metrics & Logging System

**Shared components:** Kafka (event bus), Redis (hot-path operations), Cassandra (high-write time-series), S3 + Parquet (archival), Bloom filter deduplication, Stateless ingest + async processing

---

## STEP 1 — ORIENTATION

### What this pattern covers

Analytics problems all share the same fundamental shape: something generates a stream of events at high volume, and you need to (a) ingest them reliably, (b) process them to derive useful information, and (c) serve that information quickly to users who need it. The four problems in this pattern each live at a different point in that space.

**Ad Click Aggregation** is the classic example from the source material. You have up to 100,000 click events per second coming in. Advertisers want to see counts in near-real-time so they can monitor campaign performance. The billing system needs counts that are accurate to 0.01% because money changes hands. Fraud detection has to identify and exclude invalid clicks before they reach billing. These three things — real-time dashboards, accurate billing, and fraud filtering — pull in different directions and the core design challenge is satisfying all three simultaneously.

**Web Crawler** is analytically different but architecturally similar. Instead of receiving push events, you are actively pulling content from the web. The ingestion challenge is scheduling which URL to fetch next (politeness, priority, freshness) rather than receiving what is sent to you. The processing challenge is near-duplicate detection across a corpus of billions of pages. The serving challenge is feeding crawled content to downstream consumers (search indexers, ML pipelines) reliably.

**Realtime Leaderboard** is the data structure problem in disguise. Redis sorted sets are exactly the right abstraction for a leaderboard, but a single Redis instance can't handle 100 million players at 100,000 score updates per second. The design challenge is scaling a data structure beyond a single node without breaking the semantics of global rank computation.

**Metrics and Logging System** is the broadest of the four. You are building infrastructure for infrastructure — a Datadog-like observability platform. The problem combines three different data types (metrics time-series, log text, distributed traces) and requires specialized storage for each, while presenting a unified interface to users.

### Why these four belong together

All four share the same "ingest → buffer → process → serve" pipeline. All four use Kafka as the buffer between ingest and processing. All four use Redis for the hot path. All four have a tension between freshness (show me the data now) and durability (never lose the data). The specific algorithms and storage choices differ, but the skeleton is the same across all four.

---

## STEP 2 — MENTAL MODEL

### The core idea

**Every analytics system is a pipeline that transforms a stream of raw events into a query-able derived view, with freshness and accuracy as the fundamental tension.**

Think of it as a factory assembly line. Raw events come in on the conveyor belt (Kafka). Workers at different stations transform them: deduplication removes defective parts, enrichment adds details, aggregation combines similar parts into summary units, fraud detection discards bad parts. The finished goods (aggregated views, indexed data, sorted structures) are placed on shelves that customers can pull from. The freshness tension is: how quickly can you move a part from the belt to the shelf? The accuracy tension is: how sure are you that every part was processed correctly?

**Real-world analogy:** Think of an air traffic control center. Planes (events) come in continuously from all directions. The radar system (Kafka) tracks every plane in real time. Controllers (stream processors like Flink) watch the radar and take immediate action. But the official logbook (batch layer / Cassandra / S3) records every flight authoritatively for after-action review, accident investigation, and billing of landing fees. The radar gives you current state; the logbook gives you history you can trust in court.

**Why it is hard:** The freshness-accuracy tension is genuine because they require opposite architectural choices. A pure streaming system can show you data within seconds but struggles to handle late-arriving events (a mobile phone that was offline for two hours finally syncs its click history). A pure batch system is fully accurate but shows you data that is hours old. The patterns in this section exist precisely to navigate this tension. Lambda architecture (batch + streaming layers) is the formal answer, but it introduces two code paths that must be kept in sync. Kappa architecture (one streaming layer, replay on correction) simplifies that but has its own replay cost at scale. Neither is free.

The second reason it is hard is scale. These systems are interesting at 100,000 events per second, not at 100 events per minute. At scale, things that work fine in small systems become bottlenecks: a single Redis instance, a single Kafka partition, a single database table. Every design decision you make in an analytics system has a scale ceiling, and knowing where that ceiling is and how to break through it is the technical depth interviewers are probing for.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing a single box, ask these questions. Each one changes the design in a meaningful way.

**Question 1: What does "real-time" mean for this system?**
Why it matters: "Real-time" is the most abused term in system design. Does the requester mean sub-second? One minute? Five minutes? For ad click aggregation, "real-time" means within 1 minute for dashboards; for a metrics system it might mean 90 seconds for alert evaluation. The answer directly determines whether you need a dedicated streaming layer (Flink/Spark Streaming) or whether a simple polling-based batch job on a 1-minute schedule is sufficient.

What changes: Sub-second freshness requires in-memory state (Redis, Flink in-process). Five-minute freshness can use a micro-batch approach or even a fast scheduled query. This is a major architectural simplification if you can negotiate a looser freshness requirement.

**Red flag:** If the interviewer says "as real-time as possible," push back. Say: "From the user's perspective, would they notice the difference between 10 seconds and 60 seconds? In ad platforms, dashboards typically refresh every minute — is that acceptable here?" Getting a concrete number prevents you from over-designing.

**Question 2: What are the accuracy requirements, and who is downstream?**
Why it matters: A casual dashboard for internal analytics can tolerate 1% inaccuracy from dropped events or approximate aggregation. A billing system that generates invoices cannot. If accuracy is financial-grade, you need a batch reconciliation layer and exact deduplication. If it is analytics-grade, you can use probabilistic data structures (HyperLogLog for unique user counts, Bloom filters for approximate dedup) and save significant complexity.

What changes: Billing-grade accuracy → Lambda architecture with batch layer as source of truth. Analytics-grade accuracy → streaming only (Kappa) is viable.

**Red flag:** The interviewer says "high accuracy" without quantifying. Press: "What is the maximum acceptable error rate? 0.01%? 0.1%? 5%? This determines whether we use exact deduplication in a relational database or a probabilistic Bloom filter."

**Question 3: What is the query pattern on the read side?**
Why it matters: Completely determines storage choice. If users query by arbitrary time range and arbitrary dimension filters (group by country, group by device type, group by campaign), you need an OLAP columnar store (ClickHouse, Druid). If they only ever query by a single key (give me the current score for player X), a key-value store like Redis or Cassandra is sufficient. If they need full-text search (find all logs containing "database timeout"), you need Elasticsearch. Getting this wrong means you build the right pipeline into the wrong sink.

What changes: OLAP queries → ClickHouse/Druid. Point lookups → Redis/Cassandra. Full-text search → Elasticsearch. Time-series range scans → VictoriaMetrics/InfluxDB.

**Question 4: Is deduplication required, and at what level?**
Why it matters: Deduplication is expensive. Exact deduplication of 100 million events per day requires storing 100 million IDs with fast lookup — that is 1.2 GB of Redis memory just for the key set. Probabilistic deduplication (Bloom filter) uses 240 MB but allows a configurable false positive rate. If the system never receives duplicate events (the upstream guarantees exactly-once), deduplication code is unnecessary complexity.

What changes: Financial accuracy required → two-level dedup (Bloom + exact Redis SET). Analytics only → Bloom filter alone or skip dedup entirely. Upstream already deduplicates → omit from design.

**Question 5: What is the data retention requirement?**
Why it matters: Retention determines storage cost and which storage tier hosts which data. 90-day raw event retention at 500 GB/day is 45 TB — that is S3, not a database. 5-year aggregated retention at 200 MB/day is 365 GB — that is a small ClickHouse cluster. Knowing retention lets you sketch storage estimates quickly in the interview without getting lost.

What changes: Short retention (7 days) → Kafka alone may suffice. Long retention (years) → tiered storage with hot/warm/cold layers. Different retention for raw vs. aggregated → Lambda-style tiered storage.

---

### 3b. Functional Requirements

**Core capabilities every analytics system needs:**
- Ingest raw events from producers (HTTP push API or pull-based scrape)
- Deduplicate events to prevent double-counting (if financial accuracy is required)
- Process and aggregate events into queryable views
- Serve query results to clients with acceptable latency

**Scope definition for the interview:**

For Ad Click Aggregation, the scope is: accept click events, deduplicate, run fraud detection, produce real-time aggregated counts (< 1 minute lag), and produce billing-grade hourly counts. Out of scope: ad serving, impression tracking, attribution modeling, billing payment.

For Web Crawler, the scope is: discover and fetch web pages from seed URLs, deduplicate content (near-duplicate detection), schedule recrawls at appropriate intervals, and feed downstream consumers. Out of scope: JavaScript rendering, search index building, deep web crawling.

For Realtime Leaderboard, the scope is: accept score updates, maintain globally ranked leaderboard, serve global rank queries at < 10 ms, support regional and friend leaderboards, emit rank-change notifications. Out of scope: game server logic, authentication, cross-game leaderboards.

For Metrics and Logging System, the scope is: ingest metric time-series (all types), store with multi-tier retention, serve dashboard queries, evaluate alerts, ingest and search logs, ingest and render distributed traces. Out of scope: code profiling, synthetic monitoring.

**Clear statement format:** "The system should accept X from Y at Z throughput, produce W with A accuracy within B latency, and serve Q queries with R latency."

Example: "The system should accept click events from ad publishers at up to 100,000 events/second, produce deduplicated aggregated counts per ad per minute with 0.01% billing accuracy within 1 minute for dashboards and within 30 minutes for billing, and serve historical queries over 30-day windows in under 10 seconds."

---

### 3c. Non-Functional Requirements

**Derive them from the use case, don't just list them.**

**Throughput** comes from the user base and event frequency. Always compute peak throughput as average × peak-to-average ratio (typically 3× for ad traffic, 10× for esports tournament bursts). State the formula: "1 billion clicks/day ÷ 86,400 seconds = 11,574 clicks/second average × 3× peak-to-average = ~35,000/s. Add 3× headroom = 100,000 clicks/second design target."

**Durability** is driven by what happens if events are lost. For billing systems, the answer is "zero data loss is required" — this means Kafka with replication factor 3, `min.insync.replicas=2`, and `acks=all` on the producer. For a leaderboard, losing a score update is unfortunate but not catastrophic.

**Latency** has two sides: write latency (how quickly is an event acknowledged) and read latency (how quickly can a query be served). Write latency is almost always satisfied by decoupling ingest from processing — a stateless API publishes to Kafka and returns 202 Accepted in < 50 ms p99 without waiting for downstream processing. Read latency is the harder constraint and drives storage choice.

**Trade-offs you must state out loud:**

✅ Kafka decoupling means producers are isolated from downstream failures — if ClickHouse goes down, ingest keeps working.
❌ Kafka decoupling means read-your-write consistency is impossible — a click submitted now will not appear in a dashboard query for up to 60 seconds.

✅ Lambda architecture gives billing-grade accuracy via batch layer.
❌ Lambda architecture has two code paths (streaming SQL and batch SQL) that must produce identical results — any divergence is a billing discrepancy.

✅ Bloom filter deduplication uses 240 MB for 100M events/day at 0.01% false positive rate.
❌ 0.01% false positive rate means ~100,000 valid clicks per day are incorrectly dropped — under-billing advertisers by that amount. State whether this is within the SLA.

✅ Redis sorted sets give O(log N) ZADD and ZRANK — sub-millisecond at N = 1 million.
❌ A single Redis sorted set is bounded to one node — at N = 100 million, ZADD throughput tops out at ~37,000 operations/second, which is insufficient for 100,000 updates/second.

---

### 3d. Capacity Estimation

**Anchor your estimates in these numbers and derive everything else:**

**Ad Click Aggregation:**
- 1 billion clicks/day → 11,574 clicks/second average → 100,000/s peak (3× × 3× headroom)
- Each event: 500 bytes (JSON payload with all dimensions)
- Ingest bandwidth: 100,000 × 500 B = 50 MB/s = 400 Mbps
- Daily raw volume: 1B × 500 B = 500 GB/day; compressed (Parquet/Snappy ~5×): 100 GB/day
- 90-day S3 archive: ~9 TB
- Kafka hot tier (7 days): 100 GB/day × 7 = 700 GB
- Dedup Bloom filter: 240 MB (100M events × 10 bits at 0.01% FP rate)
- Aggregated ClickHouse rows: ~1M rows/day × 200 B = 200 MB/day; 5 years = 365 GB
- **Architecture implication:** S3 for raw archival, ClickHouse for aggregated OLAP. No row-store database is viable for raw event storage at this volume.
- **Time budget:** 60 seconds is your target; talk through each number in ~3 minutes total.

**Web Crawler:**
- 1 billion pages/day → 11,574 pages/second → 17,000 req/s design target (1.5× headroom)
- Average page: 50 KB uncompressed, 15 KB on wire (gzip), 10 KB stored (Snappy)
- Bandwidth: 17,000 × 15 KB = 255 MB/s = 2 Gbps aggregate download
- Raw HTML storage: 10 TB/day; 30-day retention = 300 TB
- URL frontier: 9 billion pending URLs × 150 bytes = 1.35 TB
- **Architecture implication:** URL frontier cannot fit in Redis alone (1.35 TB). Use hybrid: top 10M priority URLs in Redis (1.5 GB), full frontier backed by Cassandra.

**Realtime Leaderboard:**
- 100M registered players; 10M concurrent active
- 100,000 score updates/second; each player appears in global + regional = 6 ZADD operations per update
- 600,000 Redis operations/second required — needs Redis Cluster
- 100M players × 16 bytes/ZSET entry = 1.6 GB total ZSET memory (fits in Redis)
- Leaderboard queries: 33,000/second (10M active / 30 seconds per check)
- **Architecture implication:** Single Redis ZSET tops out at 37K ZADD/s (log2(100M) = 27 hops per op). Must shard the ZSET across nodes via score-range bucketing.

**Metrics and Logging System:**
- 1M data points/second × 250 bytes = 250 MB/s ingest (2 Gbps)
- VictoriaMetrics: ~5 bytes/sample compressed → 5 MB/s write to storage
- Raw TSDB: 1.3 TB/day × 15 days = 19.5 TB hot tier
- Log ingest: 500 MB/s → 43 TB/day raw; 4.3 TB/day compressed; 30 TB for 7-day retention
- Alert evaluations: 1M alerts × 1/minute = 16,667 evaluations/second
- **Architecture implication:** Log volume (43 TB/day) is 33× larger than metric volume. Elasticsearch is needed for searchable log index; S3 for cost-effective archival. The bottleneck is log write throughput, not metric write throughput.

---

### 3e. High-Level Design

**The universal skeleton for all four problems:**

```
Producers → [Ingest API / Agent Fleet] → Kafka → [Stream Processors] → [Storage Tiers] → [Query Service] → Consumers
```

Every component you add should be justified by one of: (a) throughput isolation, (b) storage semantics, or (c) latency requirement.

**The four to six components you must draw on the whiteboard (in this order):**

1. **Ingest tier** (stateless pods): validate, optionally deduplicate, publish to Kafka. Returns 202 Accepted immediately. Scale horizontally. This is the rate-limiting entry point — put rate limiting and auth here.

2. **Kafka** (durable message bus): the architectural center of gravity. Every component downstream reads from here. Enables replay for batch correction. Partitioning key determines processing locality. State the partition count and the partitioning key.

3. **Stream processor** (Flink / Spark Streaming): the real-time derived view builder. Maintains windowed state, computes aggregates, writes results to the hot storage tier (Redis, real-time TSDB). For ad clicks: 1-minute tumbling windows per (ad_id, publisher_id). For leaderboard: direct ZADD to Redis. For metrics: pre-aggregation to reduce cardinality before TSDB write.

4. **Hot storage** (Redis): serves all sub-100ms queries. For ad clicks: windowed aggregates and dedup state. For leaderboard: bucketed ZSETs. For metrics: query result cache and alert state.

5. **Warm storage** (ClickHouse / TSDB / Elasticsearch / Cassandra): serves queries over hours-to-months. The right technology differs by query pattern — state the reason for each choice.

6. **Query service**: the read-side fan-out layer. Routes queries to the right tier based on time range and query type. Merges results from multiple tiers when a query spans hot and warm storage.

**Add a batch layer box only if you have billing-grade accuracy requirements.** If not, keep the design to 6 components. Adding a Spark batch job for a leaderboard is unnecessary complexity — acknowledge this explicitly.

**Whiteboard order:** Draw Kafka first (it connects everything). Then draw the ingest tier to the left of Kafka. Then draw the stream processor and hot storage to the right. Then draw warm storage below stream processor. Then draw query service below warm storage. This gives you a clean left-to-right data flow.

**Key design decisions you must state out loud, not just draw:**
- Why Kafka and not a REST API directly to the TSDB/ClickHouse? "Decoupling — if ClickHouse is slow, ingest does not back up."
- Why Redis and not ClickHouse for the hot path? "ClickHouse scans blocks; Redis has O(1) key lookup. For < 1 ms reads, only in-memory wins."
- Why ClickHouse and not Cassandra for aggregated data? "ClickHouse is a column store with vectorized execution. Cassandra is a row store that requires pre-modeling every query shape. ClickHouse handles ad-hoc aggregation queries across billions of rows in seconds."

---

### 3f. Deep Dive Areas

**The two or three areas interviewers probe deepest — answer these unprompted.**

**Deep Dive 1: Deduplication (asked in ad clicks and web crawler)**

**Problem:** Click events are sent by SDKs that retry on network timeout. Mobile apps buffer and resend. Without dedup, one user action can produce 2–5 billing events. At 1 billion events/day, even a 0.1% duplicate rate is 1 million incorrect billed events.

**Solution — Two-Level Hybrid (Bloom filter + exact Redis SET):**

The Bloom filter is the first pass. You configure it for 100M elements per day at a 0.01% false positive rate. The math: optimal size is 240 MB, optimal hash functions is 13. The RedisBloom module supports this natively with `BF.EXISTS` in O(k) time. If the Bloom filter says "definitely not seen before," you add the event to the Bloom filter and add a `SET NX click_id EX 86400` exact key in the same pipeline. You return "new event" and proceed.

If the Bloom filter says "possibly seen before" (this is the false positive case), you check the exact Redis key. If the key exists, it's a confirmed duplicate. If the key does not exist, it's a Bloom false positive — you treat it as new.

**Trade-offs you volunteer without being asked:**

✅ Bloom filter handles 99.99% of traffic (all genuinely new events) in O(13) operations at 240 MB.
❌ 0.01% false positive rate means ~100,000 valid events per day are dropped. This is an accepted under-count within the 0.01% billing SLA — and it systematically under-bills, which is legally safer than over-billing.

✅ Exact Redis key catches cross-day duplicates — a retry at 12:01 AM for an event from 11:59 PM is caught because the exact key has a 24-hour TTL from insertion time, not from midnight.
❌ Redis unavailability means dedup is skipped. The correct response: **fail open** (accept the event, log that dedup was bypassed). The batch layer will deduplicate later during the Spark billing reconciliation. Never fail closed on the ingest path.

**For web crawler, URL dedup is conceptually identical** but the false positive semantics are different: a URL incorrectly identified as already-seen misses a recrawl (acceptable), versus a click incorrectly identified as a duplicate being under-billed (borderline acceptable). Different error tolerance for the same mechanism.

---

**Deep Dive 2: Lambda vs. Kappa Architecture (asked in ad clicks and metrics)**

**Problem:** Billing requires 100% accuracy — no missed events, no double-counts, correct handling of events that arrive hours late (mobile offline buffering). Streaming can show you data within minutes but has a configurable event time watermark beyond which late events are dropped. How do you reconcile real-time dashboards (streaming freshness) with billing accuracy (batch thoroughness)?

**Lambda Architecture (what to draw and say):**

Two parallel pipelines read from the same Kafka topic. The **speed layer** (Apache Flink) maintains in-memory windowed state and writes 1-minute aggregates to Redis and ClickHouse within seconds of events arriving. These are labeled as "estimates" in the dashboard API response. The **batch layer** (Apache Spark, hourly schedule) reads raw events from S3 Parquet, performs exact full-deduplication using window functions (ROW_NUMBER OVER PARTITION BY click_id ORDER BY received_at), applies fraud filtering, and writes authoritative billing counts to a separate ClickHouse billing table. Billing is computed only from batch counts.

When a mobile phone that was offline for 2 hours reconnects and sends 500 clicks from 2 hours ago, Flink's 5-minute watermark has already passed — those events go to the `late_clicks` Kafka side output. Spark reads both the main S3 archive AND the late_clicks archive when it runs the hourly job, so every event is captured in billing regardless of how late it arrived.

**Why not Kappa?** Kappa uses a single streaming layer with replay capability for corrections. Replay sounds simple, but at 500 GB/day compressed raw events with 90-day retention, a month-long replay is 45 TB — that takes hours even with 10 Flink workers. In Lambda, recomputing one disputed hour means re-running the Spark job on one hour of Parquet files (~4 GB) — that completes in minutes. The operational cost of two code paths is justified by the ability to isolate and recompute any historical time window.

✅ Lambda gives billing-grade accuracy; batch catches all late events.
❌ Two code paths (streaming SQL and batch SQL) must produce the same results — any divergence is a billing discrepancy. Mitigate by extracting business logic as a shared library called by both Flink and Spark.

---

**Deep Dive 3: Scaling a single Redis sorted set beyond one node (asked in leaderboard)**

**Problem:** A single Redis sorted set can handle log2(100M) ≈ 27 operations per ZADD. Redis does about 1 million ops/second on typical hardware, so ZADD throughput tops out at ~37,000/second. The requirement is 100,000 updates/second. Redis Cluster distributes keys across nodes but a ZSET is a single key — you cannot split one ZSET across multiple Redis Cluster nodes.

**Solution — Score-Range Bucketing:**

Divide the score range into B buckets (B=100 works for a 0-to-10M score range with 100K-point buckets). Each bucket is a separate Redis ZSET on a different Redis node. `ZADD bucket_42 score player_id` goes to the node holding bucket 42. With 100K updates/second and 100 buckets, each bucket receives ~1,000 updates/second — easily handled by a single Redis instance.

Computing global rank is the hard part. You maintain a second data structure: a Redis HASH called `bucket_cumulative_above` keyed by game_id, with fields `bucket_0` through `bucket_99`. `bucket_k` holds the total count of players whose score is in a higher bucket than k. To get global rank for a player in bucket 5: `HGET bucket_cumulative_above bucket_5` (players in buckets 6–99, O(1)) + `ZREVRANK bucket_5 player_id` (position within bucket 5, O(log N/B)). Sum plus 1 equals 1-indexed global rank.

Cross-bucket score transitions (player moves from bucket 3 to bucket 5 because their score crossed 500K) require: ZREM from old bucket + ZADD to new bucket + HINCRBY on cumulative_above for all buckets between old and new. This is done atomically via a Lua script on the same Redis node as cumulative_above.

**Trade-offs:**

✅ This distributes 100K ZADD/s across 100 nodes, each handling ~1K/s. The O(log N/B) ZREVRANK on a 1M-entry bucket is 20 operations vs. 27 on the 100M-entry global ZSET — faster per operation AND parallelized.
❌ Cross-bucket transitions require updating cumulative_above on a separate node from the bucket ZSETs — there is a brief (~1ms) inconsistency window where rank could be off by one. This is acceptable for a game leaderboard. If it were unacceptable (e.g., prize money determined by exact rank), you would need a distributed transaction which would eliminate the throughput advantage.

**Rejected alternatives you should volunteer:**

Hash-sharding by player_id in Redis Cluster: ZADD distributes, but ZRANK requires the entire sorted set on one node — this approach breaks the semantics entirely. You cannot ask Redis Cluster for the rank of player_id across all nodes.

Pre-computed rank with periodic refresh: simple to implement, but rank is stale by the refresh interval. At 100,000 updates/second with 100 million players, rank changes happen continuously — a 5-second stale rank is unacceptable if players are watching their rank move in real time.

---

### 3g. Failure Scenarios

**Know these five failure modes and how to frame them as a senior engineer:**

**1. Kafka consumer lag — the pipeline is falling behind.**
Signal: Kafka consumer lag metric grows over time. Flink or the Redis updater is not keeping up with the ingest rate.
Response: Scale the consumer horizontally — add more Flink parallelism (bounded by partition count). If partitions are exhausted, do a planned Kafka partition expansion (create new topic with more partitions, dual-consume with a clean cutover). The key insight: because Kafka retains events for 7 days, there is no data loss — consumers can catch up at their own pace.

**2. Redis unavailability — dedup or leaderboard is down.**
Response depends on which Redis: If the dedup Redis is down, **fail open** — accept all events without dedup check, log that dedup was bypassed. The batch layer will correct duplicate counts in the next billing run. If the leaderboard Redis is down, leaderboard read queries degrade to "service unavailable." Score writes should still be accepted and buffered in Kafka — Redis Updater workers will replay and restore the sorted set when Redis recovers, because Kafka retains events.

**3. ClickHouse replication lag / write failures.**
The stream processor (Flink) can continue writing to Redis for the real-time dashboard. ClickHouse writes can be buffered in a separate Kafka consumer group with its own offset, so they will catch up when ClickHouse recovers. Dashboard shows only Redis-backed real-time estimates during the ClickHouse outage — label appropriately.

**4. Clock skew — events arrive with incorrect timestamps.**
Mobile devices have wrong system clocks. An event with a timestamp 3 hours in the future will be placed into the wrong time window by Flink. Solution: always record both the client-side timestamp (for business semantics) and the server-received timestamp (for ordering). For billing purposes, use the server-received timestamp, which you control. For dashboard display, use the client timestamp.

**5. Cardinality explosion in the metrics system.**
A developer accidentally creates a metric tag with `request_id` as a tag value — 10 million unique request IDs per day, each creating a new time series. This fills the TSDB head block (1 KB/series × 10M series = 10 GB just for this one metric). Response: per-customer series quota enforcement at the collector tier — reject or aggregate any metric that would push the customer above their quota. Monitor the HyperLogLog cardinality estimate per `(customer_id, metric_name, tag_key)` and alert at 80% of quota.

**Senior framing:** Never describe these as "the system breaks and we fix it." Instead: "The design anticipates this failure mode by [specific architectural choice]. The detection mechanism is [specific metric or alert]. The recovery is [specific procedure that requires no human intervention for the common case]."

---

## STEP 4 — COMMON COMPONENTS

**Every component listed in common_patterns.md — what it does, why you picked it, critical configuration, and what breaks without it.**

---

### Apache Kafka — the event bus

**Why used:** Kafka decouples every ingest API from every downstream processor. Without Kafka, the ingest API must wait for ClickHouse writes, Flink processing, and Cassandra inserts to complete before acknowledging the client. Any downstream slowness cascades directly to the client as latency. With Kafka, the ingest API publishes a single message and returns 202 Accepted — the rest is asynchronous.

Kafka also provides the **replay capability** that makes the Lambda batch layer work. If the Spark billing job fails and needs to rerun, or if fraud rules are updated and you need to recompute last month's billing, the raw events are still available in Kafka (within the retention window) or in S3 (archived by Kafka Connect). This replay capability is the foundation of correctness in billing systems.

**Critical configuration:**
- **Replication factor = 3, min.insync.replicas = 2.** An event is only acknowledged after it is written to at least 2 replicas. A single broker failure does not cause data loss.
- **Producer acks = all.** Combined with min.insync.replicas = 2, this ensures no acknowledged event can be lost.
- **Partition count and partitioning key.** For ad clicks: 64 partitions keyed by ad_id — all events for the same ad go to the same partition, so Flink can aggregate them without cross-partition coordination. For leaderboards: 32 partitions keyed by player_id — all score updates for the same player are ordered within a partition. For metrics: 128 partitions keyed by (customer_id + metric_name). Pick the partition count as a multiple of your expected max consumer parallelism.
- **Retention period.** 7 days for hot replay. S3 archival via Kafka Connect for longer retention.

**What breaks without it:** Without Kafka, the ingest API is synchronously coupled to all downstream systems. A 5-second ClickHouse write latency becomes a 5-second API response time. Any downstream outage causes the ingest API to back up. Scaling downstream processors requires scaling ingest APIs in lockstep. And critically, there is no replay capability — if the batch billing job needs to reprocess last month, you have nothing to replay from.

---

### Redis — the hot-path in-memory store

**Why used:** Redis is used in all four systems because certain operations have sub-millisecond latency requirements that cannot be met by any disk-backed database. Bloom filter membership checks (`BF.EXISTS`) at 100,000 events/second need to complete in < 1 ms — only in-memory is viable. ZADD and ZRANK for leaderboard need to complete in < 1 ms — only sorted sets in memory work. Dashboard queries over the last 5 minutes need sub-second response — cached Redis HASHes serve this.

Beyond latency, Redis provides specific data structures with exactly the right semantics: Bloom filters for probabilistic membership, ZSET for ranked scores, INCR for atomic counters, SET NX for mutex-style deduplication.

**Critical configuration:**
- **AOF persistence (appendfsync=everysec) or RDB snapshots.** Without persistence, Redis data is lost on restart. For the leaderboard, the entire sorted set can be rebuilt by replaying Kafka — so durability is less critical. For the dedup exact-key store, losing state on restart means accepting duplicates for the next 24 hours — use AOF.
- **Redis Cluster (6 nodes: 3 primary + 3 replica)** for throughput scaling. Hash slot distribution. Be aware: a single ZSET key cannot be distributed across cluster nodes — this is the exact reason bucketing is needed for the leaderboard.
- **RedisBloom module** for Bloom filter support (`BF.EXISTS`, `BF.ADD`). Now bundled in Redis Stack.
- **Memory sizing.** 240 MB for Bloom filter; 1.6 GB for 100M-player leaderboard ZSET; 10 GB for sliding window counters in ad clicks; 500 GB for robots.txt cache in web crawler. Memory is the binding constraint for Redis — always compute it.

**What breaks without it:** Dedup logic must fall back to a database (MySQL INSERT IGNORE, ~20K writes/second — insufficient for 100K events/second). Leaderboard rank queries on Cassandra are O(N) table scans. Dashboard hot queries hit ClickHouse for every request, increasing load dramatically.

---

### Cassandra — high-write time-series store

**Why used:** Three of the four systems use Cassandra (or equivalent) as the durable persistent store for time-series data that is write-heavy, append-mostly, and keyed by a known access pattern. For the leaderboard: 100K score writes/second, queried by (game_id, player_id). For the metrics system: trace spans written at 500 GB/day, queried by trace_id. For the web crawler: 17K URL record updates/second, queried by url_hash.

Cassandra's LSM-tree storage engine is optimized for high write throughput. Writes go to an in-memory memtable and are flushed to SSTables periodically — no random I/O on the write path. Tunable consistency (QUORUM reads and writes for critical data, LOCAL_ONE for non-critical) lets you trade consistency for performance per operation.

**Critical configuration:**
- **Partition key design.** Cassandra distributes data by partition key. For score history: `(game_id, player_id)` as partition key means all of a player's score history is on the same node — efficient range queries by time. For crawl log: `url_hash` as partition key. Hotspot risk: if one partition key receives disproportionate traffic (one extremely popular player, one extremely crawled domain), that node becomes a bottleneck. Mitigate by adding a bucket component to the partition key.
- **TTL on rows.** Cassandra natively supports per-row TTL. Set TTL on score history (30 days), crawl log (90 days), and trace spans (7 days) to prevent unbounded table growth without needing a separate cleanup job.
- **Compaction strategy.** TWCS (TimeWindowCompactionStrategy) for time-series tables — groups SSTables by time window, enabling efficient deletion of expired data.

**What breaks without it:** Using PostgreSQL for 100M player scores at 100K writes/second saturates a single node. Using Redis alone for persistence risks data loss (Redis is volatile without AOF). Using S3 directly for score history means multi-second read latency for historical queries.

---

### S3 + Parquet/Snappy — archival and batch input

**Why used:** Three of the four systems archive raw events to S3. S3 provides virtually unlimited storage at extremely low cost (~$23/TB/month) with high durability (11 nines). Parquet columnar format with Snappy compression achieves 5–10× compression over raw JSON (500 GB/day of clicks → 100 GB/day in S3). Parquet's columnar layout makes Spark and Athena queries efficient — reading only the columns needed for a query, not the entire row.

For ad click aggregation, S3 Parquet is specifically the input to the Spark batch billing job. The batch job is the authoritative billing computation, and it needs all events (including late-arriving ones) available at the time it runs. S3 provides a stable, immutable, replay-capable archive of every raw event.

**Critical configuration:**
- **Partitioning scheme.** S3 is not a database — efficient queries require partitioning the data so Spark/Athena can prune entire prefixes. Use `s3://bucket/{year}/{month}/{day}/{hour}/` for time-partitioned data. This means an hourly Spark job reads only one prefix, not the entire bucket.
- **File size.** Small files (< 100 MB) hurt Spark performance due to HDFS-style small file overhead. Use Kafka Connect with a 5-minute batching interval to produce files of 500 MB–1 GB.
- **Compression codec.** Snappy for fast CPU decompression (prioritize query speed). ZSTD for maximum compression ratio (prioritize storage cost). Parquet with Snappy is the industry standard.

**What breaks without it:** Without S3 archival, there is no way to reprocess historical billing data when fraud rules are updated. The Spark batch layer has nothing to read from. Long-term retention (beyond Kafka's 7-day window) is impossible without cold-tier storage. If you only retain aggregated data, you cannot perform ad-hoc retroactive analysis on raw events.

---

### ClickHouse — OLAP columnar store

**Why used:** Ad click aggregation needs to answer queries like "how many valid clicks did campaign 12345 receive in Germany on mobile devices between 2 PM and 4 PM yesterday?" with sub-second latency. This is a multi-dimensional aggregation query over potentially hundreds of millions of rows. ClickHouse's vectorized execution engine processes hundreds of millions of rows per second using SIMD instructions. Its columnar storage (LZ4/ZSTD compressed, stored column by column) means reading only the five columns relevant to a query, not all 20 columns of the schema.

`SummingMergeTree` is the killer feature for analytics: it automatically merges rows with the same primary key by summing numeric columns. When both Flink (streaming) and Spark (batch) write to the same table for the same time window, the duplicate rows are merged by the storage engine — no application-level conflict resolution needed.

**Critical configuration:**
- **`SummingMergeTree` for aggregated tables.** Automatically sums click_count and valid_click_count when merging rows with the same (advertiser_id, campaign_id, ad_id, window_start) primary key.
- **`ReplacingMergeTree` for billing tables.** Keeps only the latest row per primary key — useful when Spark batch jobs rerun and need to replace previous counts with corrected ones.
- **Sharding by advertiser_id.** All data for one advertiser on one shard means dashboard queries almost never need cross-shard scatter-gather.
- **Partition by month.** `PARTITION BY toYYYYMM(window_start)` enables efficient data pruning — queries over the last 30 days read only the current month's partition.
- **TTL clause.** `TTL window_start + INTERVAL 5 YEAR` automatically deletes old data.

**What breaks without it:** Without ClickHouse, aggregation queries must either run on raw Parquet in Athena (10+ second latency for any dashboard query), run on Cassandra (which requires pre-modeling every query shape — no ad-hoc grouping), or use PostgreSQL (which cannot handle hundreds of millions of rows).

---

### Two-Level Deduplication (Bloom Filter + Exact Store)

**Why used:** Exact deduplication of 100M events per day requires a set of 100M IDs with fast lookup. Redis `SETNX` is O(1) but 100M × 12 bytes = 1.2 GB of exact keys per day. If the exact store is the only mechanism, every ingest event hits Redis with a write operation — 100K Redis writes/second. The Bloom filter as a first pass handles the 99.99%+ of traffic that is genuinely new (has never been seen), rejecting them in O(k hash operations) against a 240 MB structure. Only events that the Bloom filter says "possibly seen" need to go to the exact store — and that rate is < 100 events/second (only actual retries and the 0.01% false positives).

**Critical configuration:**
- **Bloom filter sizing.** For 100M elements at 0.01% false positive rate: 240 MB, 13 hash functions. Formula: m = -n × ln(p) / ln(2)² bits. k = (m/n) × ln(2) functions.
- **Daily key with TTL.** Use a separate Bloom filter key per day: `bloom:clicks:2026-04-09`. Set 48-hour TTL (not 24 hours) to handle cross-midnight retries.
- **Fail-open behavior.** If Redis is unavailable, accept all events without dedup check. Log the bypass. The batch layer corrects later. Never fail closed on ingest.
- **Exact key TTL.** Set to 24 hours from insertion time, not from midnight. This ensures cross-midnight retries are correctly caught by the exact key even after the daily Bloom filter rotates.

**What breaks without it:** Without the Bloom filter, the exact Redis SET receives 100K writes/second instead of < 100/second. At scale, this is feasible (Redis handles 200K+ simple ops/second), but you lose the memory efficiency advantage. Without any dedup, retry storms from misbehaving SDKs inflate click counts, leading to billing disputes and potential legal liability.

---

### Stateless Ingest Service + 202 Accepted

**Why used:** This is the pattern that isolates ingest latency from storage latency. The ingest API validates the event schema, optionally runs a fast in-process dedup check, publishes to Kafka (which acknowledges in < 5 ms with proper configuration), and returns 202 Accepted to the client. At this point, the event is durably in Kafka — nothing downstream can lose it. The actual ClickHouse writes, Flink aggregation, Cassandra persistence, and notification delivery happen asynchronously.

This pattern enables horizontal scaling of both the ingest tier (add more stateless pods; no shared state) and the processing tier (add more Kafka consumers; each gets a subset of partitions) independently.

**Critical configuration:**
- **Stateless design.** All state is in Kafka, Redis, and the database. Ingest pods can crash and restart without losing any events. Kubernetes HPA can scale pod count based on CPU and custom Kafka consumer lag metrics.
- **Rate limiting at the ingest tier.** Apply per-publisher or per-customer rate limits here, before events reach Kafka. An misbehaving publisher that sends 10× their contracted rate should be throttled at the ingest API, not allowed to flood Kafka.
- **202 vs 200.** Return 202 (Accepted for processing) rather than 200 (OK, done processing). This correctly communicates to the client that the event is queued for processing, not that it has been stored and reflected in queries.

**What breaks without it:** Without 202 Accepted + async processing, the API response time is equal to the slowest downstream operation. ClickHouse batch writes can take 100–500 ms. During a ClickHouse maintenance window or slow query, every click ingest API call would block for 500 ms — causing timeout cascades and massive retry storms.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Ad Click Aggregation

**The unique things about this problem:**

Lambda architecture is not optional — it is required. The billing-grade 0.01% accuracy SLA with 100,000 events/second and mobile offline buffering makes the batch layer non-negotiable. Flink serves real-time dashboard estimates; Spark serves billing invoices. These are different tables in ClickHouse with different semantics.

The two-level dedup here has financial consequences. The Bloom filter's 0.01% false positive rate means ~100,000 valid clicks per day are under-billed. Stating this trade-off explicitly — and explaining that it is within the SLA and systematically in the advertiser's favor — is what separates a senior answer from a junior answer.

HyperLogLog is used for unique user counts in the aggregated table. An exact distinct-user count per (ad_id, minute_bucket) would require storing the full user ID set — hundreds of thousands of IDs per aggregation cell. HyperLogLog provides a ±0.8% cardinality estimate using 12 KB of memory per cell.

Fraud detection runs in a separate Kafka consumer group reading from `raw_clicks`, scoring events with a combination of rule-based signals (IP velocity, user-agent classification) and async ML scoring. It writes to `valid_clicks` and `invalid_clicks` topics. Billing uses only `valid_clicks`. The dedup step happens before fraud scoring so that a duplicate of a fraudulent click is still deduped away correctly.

ClickHouse's `SummingMergeTree` is the enabling technology for the Lambda architecture's correctness: when both Flink (approximate) and Spark (authoritative) write to the same aggregation table, the engine merges rows by summing click counts. Separate table for billing uses `ReplacingMergeTree` so Spark reruns cleanly replace previous batch output.

**Two-sentence differentiator:** Ad Click Aggregation is the only problem in this pattern that requires financial-grade accuracy (0.01% billing SLA) combined with sub-minute dashboard freshness, making Lambda architecture — with Flink for estimates and Spark batch for authoritative billing counts — non-negotiable. The core intellectual challenge is the two-level deduplication system where a Bloom filter's false positive rate directly creates an accepted under-billing gap that must be within the financial SLA.

---

### Web Crawler

**The unique things about this problem:**

The fundamental inversion: this system pulls data rather than receiving push events. The "ingest" challenge is URL scheduling — deciding which URL to fetch next while respecting politeness constraints per domain. This is a data structure and scheduling problem, not a throughput problem.

The two-level URL frontier is the central architectural innovation. The back queues (1,024 Redis ZSET buckets partitioned by domain hash) store pending URLs ordered by priority. The front queue (`frontier:domain_ready` Redis ZSET) stores domains ordered by when they are next available to crawl (based on crawl-delay from robots.txt). The atomic Lua dispatcher finds a domain that is ready, pops the highest-priority URL from that domain's back queue, and updates the domain's next-available timestamp — all in one atomic Redis operation. This is the mechanism that simultaneously enforces per-domain politeness and global URL prioritization.

SimHash for near-duplicate detection is unique to the crawler. The 64-bit SimHash of a page's content (computed from 3-word shingles) allows detecting near-duplicates (90%+ content similarity) using Hamming distance ≤ 3. The LSH table decomposition (splitting the 64-bit hash into 4 × 16-bit segments and building lookup tables per segment) makes this O(1) against a 10-billion-page corpus via the pigeonhole principle — any two hashes with Hamming distance ≤ 3 must share at least one 16-bit segment exactly.

Conditional GET (ETag and Last-Modified headers) is how the crawler avoids re-downloading unchanged pages. On first crawl, the server returns the ETag header. On recrawl, the crawler sends `If-None-Match: {etag}`. If the page hasn't changed, the server returns 304 Not Modified (no body) — saving both bandwidth and storage write.

**Two-sentence differentiator:** Web Crawler is the only pull-based system in this pattern, where the core challenge is not ingesting received events but scheduling which URL to fetch next — solved by the two-level Redis sorted set frontier that simultaneously prioritizes URLs by importance and throttles by per-domain politeness rate limits using an atomic Lua dispatch. The near-duplicate detection via SimHash with LSH table decomposition is a purely algorithmic solution unique to this problem, enabling O(1) near-duplicate lookup across billions of pages without the quadratic comparison that exact Jaccard similarity would require.

---

### Realtime Leaderboard

**The unique things about this problem:**

The leaderboard is the only problem where the primary data structure is a ranked collection — not a time-series, not an event log, not an inverted index. Redis sorted sets are the direct implementation of a leaderboard. The intellectual challenge is not choosing the data structure (it is obviously Redis ZSET) but scaling it beyond a single Redis instance while preserving the semantics of global rank.

The score-range bucketing solution is the key technical insight. Instead of sharding by player_id (which breaks global rank semantics entirely), you shard by score range. You can then compute global rank as a sum of two O(1)-or-O(log N) operations: (1) how many players are in higher-scoring buckets (O(1) from `bucket_cumulative_above` hash), and (2) what is the player's rank within their own bucket (O(log N/B) ZREVRANK). The bucket transitions (a player's score crossing a bucket boundary) must be handled atomically via Lua to prevent cumulative_above from becoming stale.

The friend leaderboard is fundamentally different from the global leaderboard. There is no precomputed ZSET for a player's friends — each player's friend set is unique. The solution is to compute it at query time: fetch all friend scores via a Redis pipeline in one round-trip, sort in memory (O(F log F) for F friends), and find the player's position. For F=1,000 friends, this is under 3 ms. The key insight is that attempting to maintain a pre-computed friend ZSET per player creates O(N×F) writes on every score update (100M players × 200 avg friends = 20 billion extra ZADD operations per score update — obviously impossible).

Historical snapshots are end-of-period states of the leaderboard stored in Cassandra. The snapshot job reads all bucket ZSETs, reassembles the global ranked list, and writes it to a `leaderboard_snapshots` Cassandra table partitioned by (game_id, snapshot_type, snapshot_date). This allows players to look up their rank at end of any season.

**Two-sentence differentiator:** Realtime Leaderboard is the only problem where the primary data structure (Redis sorted set) is obvious but fundamentally cannot scale to the required throughput on a single node, requiring a non-obvious architectural pattern — score-range bucketed ZSETs with a cumulative-above index — to preserve correct global rank computation across a distributed sorted structure. The friend leaderboard adds a second distinct algorithmic challenge: computing rank within a per-query-defined subset of players without precomputing per-player ZSET views.

---

### Metrics and Logging System

**The unique things about this problem:**

This is the only problem in the pattern that handles three distinct data types (metrics, logs, traces) each requiring a different storage technology. Metrics go to VictoriaMetrics (time-series columnar). Logs go to Elasticsearch (inverted index + S3 archival). Traces go to Cassandra (wide-row by trace_id). The query tier must route to the right system based on data type.

Multi-resolution storage tiering is unique to metrics. Raw 10-second samples are retained for 15 days (hot tier: 19.5 TB). 1-minute rollups are retained for 90 days (warm tier: 2.6 TB). 1-hour rollups are retained for 2 years (cold tier: 365 GB). A query for "show me CPU usage over the last year" would be answered entirely from the 1-hour rollup tier — no need to scan the 19.5 TB raw tier. The query service performs time-range stitching: for a query that spans multiple tiers, it fetches data from each tier and merges the results.

VictoriaMetrics is selected over Prometheus, InfluxDB, and Thanos for a concrete reason: ~5 bytes/sample (1 byte delta-encoded timestamp + 4 bytes Gorilla XOR float64) versus 10 bytes/sample for Prometheus and 15 bytes/sample for InfluxDB. At 1 million samples/second, this is 5 MB/s write throughput versus 10–15 MB/s — 2–3× more efficient storage I/O on identical hardware, directly translating to 2–3× more time series per cluster.

Cardinality explosion is the unique failure mode of metrics systems. A developer who uses `user_id` or `request_id` as a metric tag value creates millions of unique time series. Each series occupies ~1 KB in the TSDB head block. At 10 million series, that is 10 GB of RAM consumed by one misconfigured metric. The defense is per-customer cardinality quotas enforced at the collector tier: maintain a HyperLogLog sketch per `(customer_id, metric_name, tag_key)` and reject or aggregate metrics that would push any tag key's cardinality above 10,000 unique values.

Multi-protocol ingest (StatsD UDP, HTTP push, Prometheus scrape) is unique to this problem. StatsD UDP is fire-and-forget — no acknowledgment, no durability guarantee. HTTP push provides a 202 Accepted acknowledgment after Kafka publish. Prometheus scrape is pull-based — the collector must actively scrape each agent's `/metrics` endpoint on a schedule.

**Two-sentence differentiator:** Metrics and Logging System is the only observability platform in this pattern that must integrate three distinct data types (metrics, logs, traces) with a different specialized database for each, unified under a single query interface that performs time-range stitching across multi-resolution storage tiers. The unique production failure mode — cardinality explosion — has no analog in the other three problems and requires a specific defense at the collector tier using HyperLogLog cardinality estimation to prevent a single misconfigured metric from crashing the entire TSDB cluster.

---

## STEP 6 — Q&A BANK

### Tier 1: Surface Questions (answer in 2–4 sentences, expected from any candidate)

**Q1: Why do you use Kafka instead of writing directly to ClickHouse or Cassandra from the ingest API?**

**KEY PHRASE: decoupling ingest from storage.** Writing directly to the database couples the ingest API's response time to the database's write latency. If ClickHouse is doing compaction and write latency spikes to 500 ms, every ingest API call blocks for 500 ms, causing timeout cascades and retry storms from clients. With Kafka in between, the API publishes a message in < 5 ms and returns 202 Accepted. The database write happens asynchronously from a consumer that can be independently scaled and that can retry on failures without affecting clients.

**Q2: What is a Bloom filter and why do you use one for click deduplication instead of just checking Redis directly?**

**KEY PHRASE: probabilistic fast rejection.** A Bloom filter is a space-efficient probabilistic data structure that answers "definitely not seen before" (guaranteed) or "possibly seen before" (with a configurable false positive rate). The benefit over a direct Redis exact-key check is memory efficiency: a Bloom filter for 100 million elements at a 0.01% false positive rate uses 240 MB, whereas an exact Redis key set for the same data uses 1.2 GB. More importantly, 99.99% of events are genuinely new, so the Bloom filter handles them in O(k hash operations) without touching the exact store — the exact Redis SET is only consulted for retried/duplicate events.

**Q3: Why is Redis sorted set the right data structure for a leaderboard?**

**KEY PHRASE: O(log N) rank operations.** Redis ZSET provides three operations that implement a leaderboard directly: ZADD in O(log N) to insert or update a score, ZRANK/ZREVRANK in O(log N) to find a member's position in the sorted order, and ZRANGE in O(log N + M) to retrieve a range of members in sorted order. No other data structure gives you all three with O(log N) complexity. Critically, ZADD is idempotent — calling `ZADD leaderboard 1500 player_A` twice leaves the leaderboard with player_A at 1500, not 3000. This makes score updates safe even if a consumer processes a Kafka message more than once.

**Q4: What is the Lambda architecture and when would you use it?**

**KEY PHRASE: batch layer as source of truth.** Lambda architecture runs two parallel pipelines from the same event stream. The speed layer (Flink/Spark Streaming) produces real-time derived views with low latency but may miss late-arriving events. The batch layer (Spark scheduled job) processes all events including late arrivals for complete, accurate results. The serving layer merges data from both: fresh data from the speed layer, historical data from the batch layer. You use Lambda when you need both sub-minute freshness for dashboards and billing-grade accuracy for financial systems — ad click aggregation being the canonical example.

**Q5: How does ClickHouse differ from Cassandra, and why would you choose one over the other?**

**KEY PHRASE: query pattern determines the database.** Cassandra is a row-oriented wide-column store optimized for high write throughput and point lookups by known primary key. It requires pre-modeling all query shapes — if you don't know the query pattern at schema design time, you cannot use Cassandra. ClickHouse is a column-oriented OLAP database optimized for aggregation queries over large datasets — it processes hundreds of millions of rows per second using vectorized SIMD execution. Choose Cassandra for high-write key-value access patterns (score events, URL records, trace spans). Choose ClickHouse for ad-hoc aggregation queries with arbitrary filters and group-by over historical data (ad click counts by geography and device type).

**Q6: What does "cardinality" mean in the context of a metrics system, and why is cardinality explosion a problem?**

**KEY PHRASE: unique time series count.** In a metrics system, cardinality is the total number of unique time series — distinct combinations of metric_name and tag key-value set. `http.requests.count{service="api",env="prod"}` is one time series. `http.requests.count{service="api",env="prod",host="web-01"}` is a different time series. Cardinality explosion occurs when a tag has millions of unique values (user_id, request_id) — each unique value creates a new time series. A TSDB keeps recent time series in a memory "head block" (~1 KB/series). At 10 million series, that is 10 GB of RAM just for the head. A single misconfigured metric can OOM the TSDB server, crashing monitoring for all customers.

**Q7: What is the difference between a tumbling window and a sliding window in stream processing?**

**KEY PHRASE: overlapping vs. non-overlapping.** A tumbling window is a fixed-size, non-overlapping time window: events from 14:00:00 to 14:00:59 form one window; events from 14:01:00 to 14:01:59 form the next. A sliding window is fixed-size but overlapping: a 5-minute sliding window evaluated every 1 minute means the window covering 13:55–14:00 overlaps with 13:56–14:01. Tumbling windows are used when you want discrete reporting periods (billing granularity per minute). Sliding windows are used when you want a continuously updated view (alert: "error rate over the last 5 minutes"). Tumbling windows are simpler to implement and require O(1) state per key. Sliding windows require maintaining state across the entire window duration.

---

### Tier 2: Deep Dive Questions (answer with why + trade-offs)

**Q1: Your Lambda architecture has two code paths — Flink streaming and Spark batch. How do you ensure they produce the same results?**

**KEY PHRASE: shared business logic library.** The risk is that a developer updates the fraud filtering rules in the Spark job but forgets to update the Flink job, causing streaming counts to diverge from billing counts. The mitigation is to extract all business logic (fraud score thresholds, dedup rules, dimension mapping) into a shared library (jar or Python package) that is imported by both the Flink job and the Spark job. Both pipelines call the same `isValidClick(event)` function. Unit tests validate the shared library against a known dataset. Integration tests run the same events through both pipelines and assert that results match within 0.01%. You monitor the divergence metric (`streaming_count / batch_count - 1`) continuously and alert if it exceeds 0.5%.

**Q2: How does your web crawler prevent a spider trap — a website generating infinite unique URLs to exhaust the frontier?**

**KEY PHRASE: multiple defense layers.** Spider traps are identified through layered heuristics rather than a single check. First, a hard depth limit: no URL more than N hops from a seed URL is enqueued (typically 10). Second, per-domain page count cap: domains that have already contributed more than 1 million pages to the corpus receive near-zero priority for new URLs. Third, URL pattern normalization: query parameters known to be pagination (`?page=N`, `?offset=N`) are normalized to a canonical form (`?page=1`) for deduplication purposes — the actual paginated URLs are still fetched, but their variants don't multiply in the frontier indefinitely. Fourth, URL template detection: if more than 10,000 URLs from a domain share the same template (e.g., `example.com/item/{id}`), new URLs matching that template receive very low priority. The combination makes spider traps expensive to execute effectively.

**Q3: Your Redis dedup Bloom filter is a single large key (240 MB per day). How does Redis Cluster handle this?**

**KEY PHRASE: shard the Bloom filter by key prefix.** Redis Cluster distributes keys across nodes by hash slot. A single 240 MB Bloom filter key lives on one node — you lose the distribution benefit and that one node becomes a memory bottleneck. The solution is to shard the Bloom filter by the first 2 hex characters of the click_id (which is a UUID v4 with uniform random prefix). This creates 256 logical buckets, grouped into 16 Redis keys using hash tags: `{bloom:0}` through `{bloom:f}`. Each key covers 1/16 of the key space and uses ~15 MB. A click_id is deterministically routed to its bloom key by `prefix = click_id[0]` (first hex character maps to a key suffix). The 16 keys distribute across cluster nodes by hash slot, spreading the memory load and the BF.EXISTS throughput.

**Q4: What happens in the metrics system when a customer sends 10× their expected metric volume during a Black Friday-scale event?**

**KEY PHRASE: per-customer rate limiting at the collector.** The collector tier enforces per-customer data point quotas. If a customer's configured limit is 500,000 data points/minute and they send 5 million, the collector returns HTTP 429 with a `Retry-After` header. Importantly, the rate limit is enforced before the Kafka publish — the message bus is protected from being overwhelmed by a single tenant (noisy neighbor problem). For customers on a metered billing model, throttling rather than billing shock may be preferred — the product team's call. The collector also monitors the per-customer current rate using Redis counters with 1-minute TTL (INCR + EXPIRE), resetting each minute. The 429 response includes the current rate and the limit so the customer's agent can implement exponential backoff.

**Q5: Your leaderboard uses score-range buckets. What happens when a new game launches and all players have scores between 0 and 1,000 — all in bucket_0?**

**KEY PHRASE: dynamic bucket boundaries via percentile-based partitioning.** Static score-range buckets fail when the score distribution is heavily skewed — a new game where everyone starts at 0 puts all 100M players in one bucket, eliminating the distribution benefit. The solution is to use percentile-based bucket boundaries instead of fixed score ranges. Every hour, a background job reads a random sample of 100,000 active player scores from Cassandra, sorts them, and computes the 100 percentile boundaries (the score at the 1st percentile, 2nd percentile, ..., 99th percentile). These boundaries are stored in a Redis HASH: `bucket_boundaries:game:g1`. The bucketing logic reads these boundaries to determine which bucket a score falls into. Over time as scores distribute, the boundaries naturally spread out. Bucket boundary updates require a brief migration window (move any players whose bucket membership changed to their new bucket), which the background job handles.

**Q6: How does the alert engine in the metrics system scale to evaluate 1 million alert rules every minute?**

**KEY PHRASE: sharded alert evaluation with distributed locking.** One million alert evaluations per minute is 16,667 evaluations per second. Each evaluation runs a PromQL query against VictoriaMetrics. A single-threaded alert engine cannot handle this volume. The solution: partition alert rules across alert engine workers by customer_id (each worker owns a subset of customers). With 100 workers, each handles 10,000 customers' alerts. Workers are stateless — alert state (OK/FIRING/RESOLVED) is stored in Cassandra keyed by alert_id. Workers are registered in a Zookeeper/etcd service registry; if a worker crashes, its alert rules are redistributed to surviving workers within one evaluation cycle. This ensures no alert rule is permanently missed due to worker failure.

**Q7: In the web crawler, how do you handle a domain that starts returning 429 Too Many Requests (rate limiting)?**

**KEY PHRASE: exponential backoff with jitter, updating crawl delay.** When the fetch worker receives HTTP 429 from a domain, it increments that domain's `error_count` in Cassandra. The next_crawl_time for all pending URLs from that domain is set to `now + base_delay × 2^min(error_count, 8)` (exponential backoff, capped at ~4 hours). A 429 response may also include a `Retry-After` header — use that value if present. The domain's entry in the `frontier:domain_ready` Redis ZSET is updated with the new next_available timestamp, so the frontier automatically stops scheduling URLs from that domain until the backoff period expires. If the domain continues returning 429 after 5 attempts, flag it as temporarily blocked and stop crawling it for 24 hours. After successful crawls resume, the error_count resets and the crawl delay returns to the robots.txt-specified value.

---

### Tier 3: Staff+ Stress Test Questions (reason aloud through the trade-offs)

**Q1: Your Lambda architecture batch layer is the source of truth for billing. The batch job runs hourly. A fraud rule change is deployed, and now you need to retroactively recompute billing for the past 30 days. How do you do this, and what are the risks?**

The retroactive billing recomputation is fundamentally a backfill operation on the batch layer. The raw Parquet files in S3 are immutable (they never get overwritten) — so you have the complete 30-day event history available. The procedure: deploy the new fraud rule to the Spark job, then trigger a backfill job that processes each historical hour from 30 days ago to today. For each hour, the Spark job reads the raw Parquet, applies the updated fraud filter, recomputes billing aggregates, and writes to the `agg_clicks_billing` ClickHouse table. Because the table uses `ReplacingMergeTree(created_at)`, the new run's rows (with later `created_at`) automatically supersede the old rows for the same primary key.

The risks: at 100 GB/day compressed raw data over 30 days = 3 TB of S3 reads. On a 100-node Spark EMR cluster, this takes roughly 2–4 hours to process. During this window, the billing tables contain a mix of old fraud-rule results (for hours not yet reprocessed) and new fraud-rule results (for already-reprocessed hours). Billing queries during the backfill window may return inconsistent results. Mitigation: run the backfill in a staging ClickHouse cluster first, validate results with spot checks, then do an atomic rename/swap to promote the staging tables to production. Communicate to billing stakeholders that reconciliation is in progress and hold invoice generation until the backfill completes.

The deeper risk is discovering that the new fraud rule is overly aggressive and retroactively marks as fraud 5% of previously-valid clicks — a massive billing adjustment. This is why any fraud rule change should go through a dry-run on a sample of recent data before being deployed for retroactive backfill.

**Q2: You have a global leaderboard with 100 million players and 100,000 score updates/second. Describe exactly what happens architecturally if the Redis node holding bucket_50 crashes. How do you recover and what do players see in the meantime?**

Bucket_50 holds all players with scores in the range [5,000,000 – 5,100,000) — approximately 1 million players in the middle of the score distribution. The Redis Cluster detects the primary failure within 10 seconds and promotes the replica for that hash slot range to primary. During these 10 seconds: ZADD operations for bucket_50 fail, causing Kafka consumer lag to grow (the Redis Updater retries with exponential backoff). Score updates for players in bucket_50 are buffered in Kafka. ZRANK queries for players in bucket_50 return errors or stale results.

After failover, the Redis Updater resumes consuming from Kafka and replays the 10-second backlog of buffered score updates for bucket_50. The `bucket_cumulative_above` hash may be slightly stale during the replay window — rank queries during this period may be off by the number of cross-bucket transitions that occurred during the outage (typically small).

Players in bucket_50 see stale or unavailable rank data for 10–30 seconds (failover time + replay time). The leaderboard API should return the cached rank from the `player:meta:{player_id}` hash if the bucket ZSET is unavailable, with a `"data_quality": "stale_10s"` flag in the response. Score writes continue being accepted via Kafka — no score update is lost.

For a production leaderboard, you would additionally implement a circuit breaker on bucket_50 reads: if the bucket is returning errors, fall back to approximate rank using the last known rank from Cassandra's `player_scores` table (point lookup, always available). The approximate rank may be 30 seconds stale but provides a meaningful response to the client rather than an error.

**Q3: In your ad click aggregation system, you discover that a major publisher has been sending clicks with spoofed click_ids that are UUIDs but are predictably sequential, not truly random. A sophisticated click farm is sending 10 million fake clicks per day with unique UUIDs, bypassing your dedup system because each fake click has a genuinely unique ID. How do you detect and eliminate this fraud without redesigning the entire pipeline?**

This is the classic arms race scenario where a single-signal fraud defense has been bypassed. The UUID dedup works correctly — each fake click does have a unique ID, so dedup is not the right tool here.

The detection relies on multi-dimensional behavioral signals that the click farm cannot easily fake simultaneously. First, publisher-level CTR anomaly detection: compute the click-through rate (CTR) for each publisher as clicks / impressions. A sudden 10× CTR increase for one publisher that is not explained by campaign changes is a strong fraud signal. This requires the impression tracking pipeline (currently out of scope) to provide denominator data — an excellent discussion point about why impression and click pipelines must be correlated.

Second, conversion rate anomaly: a legitimate publisher's clicks should convert (the user actually visits the advertiser's landing page and completes an action) at some nonzero rate. A click farm generates clicks that never convert — their post-click conversion rate approaches zero. Implement a separate conversion event pipeline that matches conversion events back to click events via click_id, compute publisher-level conversion rates in the ClickHouse billing table, and flag publishers whose conversion rate drops below a threshold.

Third, IP and device graph analysis: even with unique click_ids, 10 million fake clicks per day likely reuse IP ranges (datacenter subnets), user-agent patterns (same UA string in bulk), or show suspiciously short time-between-clicks (click farms operate faster than human reaction time). These signals exist in the raw event data already stored in S3.

The response pipeline: detected fraudulent click_ids are written to a `fraudulent_click_ids` Cassandra table. The Spark billing batch job reads this table and excludes matching click_ids from billing counts. The `ReplacingMergeTree` mechanism ensures that rerunning the billing batch with updated fraud decisions correctly updates historical billing counts.

The billing implications: the advertiser should be credited for the fraudulent clicks already billed, and the publisher should have their account flagged. This is why keeping raw events in S3 for 90 days (not just aggregated data) is non-negotiable — you need to be able to identify and retroactively exclude fraudulent events.

**Q4: Your metrics system serves 10,000 customer organizations. Customer A has a runaway process that is generating 100 million unique time series per hour — 10,000× their quota. This is causing VictoriaMetrics to OOM and crash. How does your system prevent this from happening, and how do you recover if it does happen?**

This is the **cardinality explosion** failure mode, which is the metrics system's unique failure. Prevention is the primary concern.

At the collector tier, the CardinalityGuard maintains per-customer, per-metric, per-tag-key HyperLogLog estimators. When Customer A's `request.duration` metric's `request_id` tag value cardinality exceeds 10,000 unique values, the collector switches that tag_key to "aggregate mode": all unique values above the limit are replaced with the token `__high_cardinality__`. From the TSDB's perspective, all requests above the cardinality limit are attributed to a single synthetic time series `request.duration{request_id="__high_cardinality__"}` instead of 100 million unique ones. The customer loses individual request granularity above the limit but does not crash the system.

The second layer is a hard series quota enforced by a Kafka consumer before the TSDB writer. It tracks active series per customer using an HLL and rejects (drops) new series beyond the quota with a 429-equivalent error logged for the customer.

Recovery after an OOM crash: VictoriaMetrics's storage engine is append-only — data already written to disk is not corrupted by a crash. On restart, vmstorage recovers from disk. The more urgent problem is the head block: any in-flight samples not yet flushed to disk are lost. VictoriaMetrics has a configurable retention for the in-memory accumulation — with a 30-second flush interval, at most 30 seconds of data is lost on OOM. Given that the crash was caused by cardinality explosion, the recovery procedure includes: (1) restart VictoriaMetrics, (2) immediately quarantine the offending customer (all their metrics are dropped at the collector until the issue is resolved), (3) notify the customer with details of the violating metric and tag key, (4) once the customer fixes their instrumentation, remove the quarantine.

The root cause prevention: proactively alert customers when their cardinality reaches 50% of their quota (warning) and 80% (critical). Include the specific metric name and tag key causing the growth in the alert message, so engineers can fix it before it causes a service incident.

---

## STEP 7 — MNEMONICS

### The KARDS Framework (components in every analytics system)

**K** — **Kafka** (event bus, always the center)
**A** — **Aggregation** (stream processor: Flink or Spark Streaming)
**R** — **Redis** (hot path: sub-millisecond reads, counters, dedup, leaderboard)
**D** — **Durable store** (Cassandra/ClickHouse/TSDB for the cold path)
**S** — **Serve** (query service that routes to the right tier by time range or query type)

When you draw your HLD, draw these five first. Every other component is an elaboration of one of these five.

### The Freshness-Accuracy Spectrum mnemonic

**"Speed lies. Batch doesn't."**

- Speed layer (Flink): shows you data within 1 minute but may be approximate (late events not yet arrived).
- Batch layer (Spark): shows you data 30–60 minutes old but is complete and exact.

When asked "how do you handle accuracy vs. freshness?", state the spectrum and position your system on it: "For dashboards, we accept approximate real-time data labeled as estimates. For billing, we wait for the batch layer and label it as final." This framing shows architectural maturity.

### Opening one-liner for any analytics question

**"Before I design anything, I want to understand the difference between what needs to be fast and what needs to be correct, because the answer determines whether we need one data pipeline or two."**

This single sentence signals to the interviewer that you understand the Lambda vs. Kappa tradeoff, that you derive NFRs from use cases rather than guessing, and that you will avoid the common mistake of applying the same storage solution to both real-time dashboards and billing reconciliation.

---

## STEP 8 — CRITIQUE

### What the source material covers well

The source material is exceptionally thorough on specific technical implementations. The two-level Bloom filter + exact Redis SET deduplication, including the sizing math (240 MB for 100M elements at 0.01% FP rate with 13 hash functions), is the clearest explanation of why this specific combination is used in production billing systems. The bucketed ZSET architecture for the leaderboard with `bucket_cumulative_above` is a non-obvious solution that the material derives carefully from first principles, including the rejected alternatives (hash-sharding, single ZSET) and why they fail. The SimHash with LSH table decomposition for web-scale near-duplicate detection is explained well enough to implement from scratch. The VictoriaMetrics selection rationale (~5 bytes/sample vs. 10 bytes for Prometheus and 15 for InfluxDB) is quantified rather than just asserted. These are the answers that differentiate senior from staff candidates.

### What is shallow or missing

**Distributed transactions across Redis nodes.** The leaderboard's cross-bucket transition (updating ZADD on one node and `bucket_cumulative_above` on another) is acknowledged as a potential inconsistency but the solution given ("accept 1ms inconsistency") is presented as settled rather than as an ongoing engineering trade-off. A staff-level interviewer will push on this: "What if the process crashes between the ZADD and the HINCRBY?" The source material doesn't address recovery from partial cross-bucket transitions — the answer is a background reconciliation job that periodically recomputes `bucket_cumulative_above` from actual bucket sizes, catching any partial-update drift.

**Consumer group rebalancing.** All four systems rely on Kafka consumer groups scaling horizontally. The source material doesn't address what happens during a consumer rebalance (when a pod is added or crashes) — there is a brief window where some partitions have no assigned consumer. For a billing system, this means a brief gap in event processing that could affect real-time dashboard counts. The answer is that Kafka's `session.timeout.ms` and `max.poll.interval.ms` configuration govern how quickly a rebalance completes (typically 3–15 seconds), and the count gap is recovered as soon as the new consumer assignment takes effect.

**Cost estimation.** The source material has detailed storage estimates but does not discuss cost. A staff+ interview often includes "is this architecture cost-effective?" ClickHouse on 4 nodes with 2 replicas each for 365 GB of data is expensive over-provisioning. VictoriaMetrics cluster operational costs vs. managed cloud TSDB offerings. ElastiCache (managed Redis) pricing at $0.20/GB/hour × 400 GB = $57,600/month for the SimHash store in the web crawler — you should volunteer whether this is justified or whether a Cassandra-backed fallback makes economic sense.

**Exactly-once semantics in Flink.** The source material mentions Flink with `WatermarkStrategy` and windowed aggregation, but does not explain the checkpointing mechanism that enables exactly-once processing. A deep-dive question will ask: "How does Flink recover after a crash without double-counting events?" The answer: Flink checkpointing stores the consumer offsets + operator state (window contents) to a durable store (S3 or HDFS) at a configurable interval. On recovery, Flink restores state from the last checkpoint and re-reads Kafka from the corresponding offsets, guaranteeing no event is missed or double-counted.

### Senior probes that will expose gaps

**"Your Bloom filter has a 0.01% false positive rate. Over 1 billion clicks per day, you're dropping 100,000 valid clicks. Some advertisers run campaigns where each click is worth $50. That's $5 million in under-billing per day. How do you defend this design choice?"**

The correct answer: the 0.01% FP rate is within the negotiated billing SLA. The under-billing is systematically in the advertiser's favor (never over-billing), which is legally safer. For campaigns where each click value is very high, negotiate a tighter SLA (0.001%) by tripling the Bloom filter memory to 720 MB — still within operational budget. Advertisers who dispute specific under-billing can trigger a batch re-audit against the raw S3 Parquet using the exact click_id as the dedup key — the batch layer has zero false positives.

**"You said the batch layer is the source of truth for billing. But what if the Kafka topic has already been deleted (beyond retention window) and the Spark job failed silently for that window?"**

The correct answer: this is exactly why Kafka Connect immediately archives raw events to S3 Parquet as they are produced — with a separate Kafka consumer group dedicated to archival, running at higher priority than processing consumers. The S3 archival consumer group's offset is the guard: if the archiver is healthy and its offset matches the producer offset, every event is in S3 even if processing failed. The Spark batch job reads from S3, not from Kafka directly, so Kafka retention window is irrelevant to billing correctness. The S3 Parquet files are retained for 90 days with object versioning and cross-region replication enabled — they are the immutable source of truth.

**"In your leaderboard, you said rank queries return in < 10 ms p99. Walk me through the exact sequence of Redis operations for a rank query and verify that the total latency budget is achievable."**

The correct answer: `HGET bucket_cumulative_above:game:g1 bucket_5` (O(1), one Redis hop, ~0.5 ms RTT) + `ZREVRANK lb:game:g1:bucket:5 player_id` (O(log N/B) = O(log 1,000,000) = O(20) hops, Redis internal, still ~0.5 ms RTT for the ZREVRANK call). Total Redis time: ~1 ms. Add 1–2 ms for network RTT to the application server and 1–2 ms for application processing. Total: 3–5 ms p50. p99 adds Redis-side queueing under load. At 33,000 ZRANK queries/second across 100 bucket nodes, each node handles ~330 queries/second — well within a single Redis instance's throughput capacity (~200,000 simple ops/second). p99 at this load is dominated by the occasional OS scheduling jitter, typically < 5 ms. The 10 ms p99 target is achievable with comfortable headroom.

### Traps to avoid

**Trap 1: Designing a single global ZADD for the leaderboard.** The interviewer is waiting for you to say "Redis sorted set" and will then ask "100 million players, 100,000 updates per second — how do you handle that?" If you say "Redis sorted set on a single instance," you have walked into the trap. Always pre-empt: "Redis sorted set is the right data structure, but a single instance tops out at ~37K ZADD/s for this scale. So we use score-range bucketing across 100 Redis instances with a cumulative-above index."

**Trap 2: Using PostgreSQL for any high-throughput component.** PostgreSQL is a wonderful database but it does not belong anywhere in a system handling 100,000 events per second with billions of rows. Saying "we store raw click events in Postgres" will cause the interviewer to probe until you admit it cannot scale. Know which database you would use for each component before you write it on the whiteboard.

**Trap 3: Forgetting to label real-time counts as estimates.** If a dashboard shows streaming data (from Flink/Redis) and billing shows batch data (from Spark/ClickHouse), the numbers will differ by up to 0.5% temporarily. If you don't address this, the interviewer will ask: "What do you tell the advertiser when the dashboard shows 5,000 clicks but the invoice shows 4,978 clicks?" The answer: label all streaming data as "real-time estimate" and all batch data as "billing final." Expose `data_quality` fields in the API response.

**Trap 4: Over-engineering the web crawler with ML-based content quality scoring.** The problem statement in the interview is "design a web crawler." Crawl rate, politeness, deduplication, and frontier management are the core. URL quality scoring based on PageRank estimates or ML-computed importance is an optimization — mention it as a future improvement after you have the core design solid. Interviewers who give you the web crawler problem are testing frontier design and politeness enforcement, not recommendation systems.

**Trap 5: Conflating cardinality (time-series count) with throughput (data point rate) in the metrics system.** These are independent dimensions. You can have low cardinality (1,000 time series) but very high throughput (each series emits 10,000 data points per second). You can have very high cardinality (10 million time series) but low throughput (each series emits one data point every 10 seconds). The resource constraint for cardinality is TSDB head memory (~1 KB/active series). The resource constraint for throughput is TSDB write I/O and CPU. Treat them separately in your design.

---
