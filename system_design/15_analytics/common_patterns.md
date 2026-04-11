# Common Patterns — Analytics (15_analytics)

## Common Components

### Kafka for Event Ingestion and Pipeline Decoupling
- All four systems use Kafka as the central event bus between ingest and processing tiers
- ad_click_aggregation: `raw_clicks` Kafka topic (50 MB/s ingest); `valid_clicks` topic after dedup/fraud filter; `late_clicks` side-output topic; 100K msg/s × 500 B; Flink consumes with watermark strategy (5-min allowed lateness)
- web_crawler: `crawl_results` Kafka topic (fetchers → content processors); `url_discovered` topic (content processors → frontier); `recrawl_schedule` topic; used to decouple the 17K req/s fetch pipeline from parsing/storage
- metrics_logging_system: `metrics-ingest` topic (250 MB/s from collectors); `logs-ingest` topic (500 MB/s); 3 consumer groups per topic (TSDB writer, alert evaluator, anomaly detector); Kafka as buffer between agent push and TSDB write
- realtime_leaderboard: `score-updates` topic (8 MB/s); 3 consumer groups — Redis updater, Cassandra writer, notification engine; Kafka RF=3 for durability; 7-day retention

### Redis for Fast In-Memory Operations on the Hot Path
- All four use Redis for sub-millisecond reads/writes on the critical path; persistent backing store holds the authoritative data
- ad_click_aggregation: deduplication (Bloom filter + exact SET NX, 24-hr TTL); real-time sliding window counters (~10 GB for 5-min windows); dashboard caching
- web_crawler: two-level URL frontier (`frontier:back:{bucket}` ZSET per domain bucket + `frontier:domain_ready` ZSET); DNS cache (10 GB, 4-hr TTL); robots.txt cache (500 GB, 24-hr TTL); SimHash segment lookup tables
- metrics_logging_system: metric metadata cache (metric name → MetricID); active alert state; query result cache (short TTL); customer rate limit counters
- realtime_leaderboard: bucketed ZSET per score range (100 buckets, each on a different node); `bucket_cumulative_above` HASH for O(1) global rank computation; player metadata cache (50B × 10M active = 500 MB)

### Cassandra / Wide-Column Store for High-Write Time-Series Data
- Three of four use Cassandra (or equivalent) for append-only time-series with TTL-based retention
- ad_click_aggregation: raw click events archived to S3 Parquet (Cassandra not primary here); batch layer reads Parquet for authoritative billing counts
- web_crawler: `url_record` table keyed by url_hash; crawl history (crawled_at, status, content_hash, next_crawl_at) as time-series; link graph edges (15B/day × 20B); 27 TB link graph over 90 days
- metrics_logging_system: trace store (OpenTelemetry spans, 500 GB/day, 7-day TTL); metric metadata (permanent, ~10 GB); `(trace_id)` partition key for O(1) trace lookup
- realtime_leaderboard: `score_records` (player_id, updated_at, score, delta) — write-heavy 100K/s; `score_history` (100M × avg 1,000 updates = 20 TB); `historical_snapshots` (100M × 365 × 50B = 1.83 TB/year)

### Object Store (S3) for Durable Archival
- Three of four archive raw event data to S3 for long-term storage, batch reprocessing, and compliance
- ad_click_aggregation: Parquet/Snappy compressed raw events (100 GB/day after compression, 90-day retention = 9 TB); Spark batch job reads S3 for hourly billing reconciliation; Athena for ad-hoc queries
- web_crawler: raw HTML pages (Snappy compressed, ~10 KB/page × 1B/day = 10 TB/day, 30-day retention = 300 TB); keyed by `crawl-raw/{year}/{month}/{day}/{hour}/{domain}/{hash}.html.gz`
- metrics_logging_system: log archive (compressed raw logs, ~4.3 TB/day, up to 1-year retention = 1.57 PB); cold tier for 1-hour rollup (365 GB over 2 years)
- realtime_leaderboard: historical snapshots in Cassandra (not S3); S3 not primary for leaderboard

### Two-Level Deduplication (Bloom Filter Fast Path + Exact Store)
- Three of four use a Bloom filter as the fast rejection path to avoid overwhelming exact-match stores
- ad_click_aggregation: RedisBloom `BF.EXISTS` (O(k), ~240 MB for 100M clicks at 0.01% FP rate, 13 hash functions); on miss → Redis `SET NX click_id 86400s TTL`; fail-open on Redis unavailability (batch layer corrects)
- web_crawler: Bloom filter for URL seen check (fast path); on hit → Cassandra exact lookup for `next_crawl_at` confirmation; avoids adding already-scheduled or recently-crawled URLs to frontier; false positive = missed recrawl (acceptable)
- metrics_logging_system: metric MetricID cache (`sync.Map` in process) to avoid recomputing xxHash64 of metric_name+tags on every sample write; not a dedup mechanism but a fast-path ID resolution
- realtime_leaderboard: anti-cheat rule check (in-process velocity filter: delta > max_score_per_event → reject); no Bloom filter; Cassandra write is idempotent by design

### Stateless Ingest Service + Durable Kafka Buffer
- All four implement the same pattern: stateless HTTP/UDP ingest pods validate and publish to Kafka, returning 202 Accepted; downstream processing is asynchronous and decoupled
- ad_click_aggregation: Click Ingest Service validates click, runs dedup, publishes to Kafka; returns 200/202 with < 50 ms p99; Kafka is the durable handoff point
- web_crawler: URL Frontier is the stateless ingest for URLs; Crawl Workers are stateless fetchers; Kafka carries discovered URLs and crawl results
- metrics_logging_system: Collector fleet (FluentBit-style agents) accepts StatsD UDP (fire-and-forget) or HTTP push (< 100 ms ack); publishes to Kafka; TSDB writers consume asynchronously
- realtime_leaderboard: Score Update Service accepts HTTPS POST, validates, publishes to Kafka, returns 202 < 50 ms p99; Redis ZADD and Cassandra write happen async from Kafka consumer

## Common Databases

### Kafka
- All four; central event bus; decouples ingest from processing; provides replay buffer for late-arriving events and batch reprocessing; RF=3 for all systems

### Redis
- All four; hot-path operations: dedup, counters, frontier queues, leaderboard ZSETs, caches; Bloom filter (RedisBloom module) for probabilistic membership checks in ad_click and web_crawler

### Cassandra
- web_crawler, metrics_logging_system, realtime_leaderboard; wide-column append-only time-series; high write throughput; TTL-based retention; time-bucketed partition keys for even data distribution

### S3 + Parquet/Snappy
- ad_click_aggregation, web_crawler, metrics_logging_system; long-term archival; columnar Parquet format for efficient batch processing; 5–10× compression via Snappy; Athena/Spark for analytical queries

### ClickHouse (OLAP)
- ad_click_aggregation (primary OLAP for aggregated click counts, 1-min bucketed rows, 365 GB over 5 years), metrics_logging_system (alternative OLAP for metric rollups); SummingMergeTree for aggregation-friendly schema; columnar storage; sub-second queries on 5-year history

## Common Communication Patterns

### 202 Accepted with Async Processing
- All four return 202 immediately after publishing to Kafka; downstream processing (aggregation, indexing, DB writes) is decoupled; prevents client-facing latency from being coupled to storage write latency

### Event-Driven Pipeline (Kafka → Consumer → Index/Store)
- All four use Kafka consumers to maintain derived views (Redis counters, ClickHouse aggregates, ES log index, Redis ZSET leaderboard) eventually consistent with the primary event log; Kafka provides replay capability for rebuilding derived views

## Common Scalability Techniques

### Partition Kafka by High-Cardinality Key for Parallelism
- ad_click_aggregation: partition by ad_id or campaign_id for aggregation locality
- web_crawler: partition by domain_hash for per-domain politeness ordering
- metrics_logging_system: partition by metric_name + customer_id for TSDB writer locality
- realtime_leaderboard: partition by game_id (few high-volume partitions)

### Horizontal Scaling of Stateless Processing Workers
- All four scale consumers/processors horizontally; state externalized to Redis/Cassandra/Kafka offsets; adding workers increases throughput linearly (Kafka partition-bounded)

## Common Deep Dive Questions

### How do you handle late-arriving events in a streaming aggregation pipeline?
Answer: Two mechanisms. (1) Flink/Spark watermarks: `WatermarkStrategy.forBoundedOutOfOrderness(Duration.ofMinutes(5))` — the pipeline waits up to 5 minutes beyond the current event time for late events before closing a window. Late events within the watermark are included in the correct window. (2) Sidecar late-data output: events arriving after the watermark are sent to a `late_clicks` Kafka topic; the hourly batch job (Spark on S3 raw data) includes these, making batch counts authoritative. Dashboard shows real-time estimates; billing uses batch. This Lambda architecture pattern is used in ad_click_aggregation explicitly.
Present in: ad_click_aggregation

### How do you scale a system to billions of entities with sub-millisecond lookups?
Answer: Two-level indexing with Bloom filter fast path. Bloom filter (e.g., RedisBloom, 240 MB for 100M items at 0.01% FP) rejects the vast majority of non-member queries in O(k) time. Only items that pass the Bloom check are verified against the exact store (Redis SET, Cassandra, or a DB). At 100K events/sec with 99.99% being new events, the exact store sees ~0 ops/sec. This pattern appears in click dedup (ad_click_aggregation), URL dedup (web_crawler), and SimHash near-duplicate detection.
Present in: ad_click_aggregation, web_crawler

## Common NFRs

- **Ingest latency**: 202 Accepted in < 50–100 ms p99 (all four)
- **Query latency**: < 1–2 s for recent data (1-hour window); < 10 s for historical (30-day window)
- **Throughput**: 100K–1M events/sec ingest across all systems
- **Durability**: no event loss once ACKed; Kafka as durable buffer; S3/Cassandra as long-term store
- **Availability**: 99.99% ingest; 99.9% reads
- **Eventual consistency**: processed data visible within 1–60 seconds depending on system
