# Problem-Specific Design — Analytics (15_analytics)

## Ad Click Aggregation

### Unique Functional Requirements
- 1B clicks/day; 100K peak clicks/sec; 500 B per click event
- Exactly-once click counting (deduplication); 0.01% billing accuracy SLA
- Real-time dashboard: < 1 min lag; billing-grade batch: hourly + daily reconciliation
- Fraud detection: bot traffic, click velocity, IP-level abuse flagged before billing
- Query API: arbitrary time-range + dimension filters; 1-hour window < 2 s; 30-day < 10 s
- Raw events retained 90 days; aggregated data 5 years

### Unique Components / Services
- **Click Ingest Service**: dedup check → fraud score → Kafka publish; 202 Accepted in < 50 ms p99; fail-open on Redis unavailability (batch layer corrects)
- **Two-Level Deduplicator**: RedisBloom `BF.EXISTS` (O(k), 240 MB for 100M clicks/day, 0.01% FP rate, ~13 hash functions) → on miss: Redis `SET NX click_id TTL=86400`; cross-day window handled by exact key's 24-hr TTL (not midnight-boundary)
- **Flink Real-Time Aggregation**: 1-minute tumbling windows keyed by (ad_id, publisher_id, country_code); `WatermarkStrategy.forBoundedOutOfOrderness(5 min)` for late events; HyperLogLog for unique user cardinality; late data → `late_clicks` Kafka side output; sink to Redis (dashboard) + ClickHouse (persistent)
- **Spark Batch Layer**: hourly Spark job reads S3 Parquet; exact dedup by click_id (primary key in Parquet); produces authoritative billing counts; covers late-arriving clicks that missed the Flink watermark
- **Fraud Engine**: consumes `raw_clicks` Kafka topic in parallel; scores events for bot patterns, velocity anomalies; publishes `invalid_click_ids` for filtering before billing
- **ClickHouse OLAP**: pre-aggregated rows (ad_id, campaign_id, publisher_id, geo, device, 1-min bucket → click_count); SummingMergeTree; ~200 MB/day; 5-year retention = 365 GB; dashboard queries < 1 s

### Unique Data Model
- **Kafka `raw_clicks`**: click_id (UUID v4 + server salt SHA256(publisher_id+click_id+ts)), ad_id, campaign_id, publisher_id, user_agent, ip, timestamp_ms, geo, device, referer; 500 B
- **Redis Bloom key**: `bloom:clicks:{date_str}`; 48-hr TTL (spans midnight); ~240 MB/day at 0.01% FP rate
- **Redis exact key**: `exact:click:{click_id}`; SET NX EX 86400; ~1.2 GB/day (100M × 12 B)
- **ClickHouse `click_aggregates`**: window_start, ad_id, campaign_id, publisher_id, country_code, device_type, click_count, valid_click_count, unique_users (HLL sketch), unique_ips (HLL sketch)
- **S3 Parquet**: `s3://clicks-raw/{year}/{month}/{day}/{hour}/part-{N}.parquet.snappy`; columnar layout; ~100 GB/day compressed; 90-day retention = 9 TB

### Lambda Architecture (Why Not Kappa)
- **Batch layer (authoritative)**: Spark on S3 Parquet; full exact dedup; handles all late arrivals; source of truth for invoicing; recompute any hour in isolation
- **Speed layer (approximate)**: Flink; serves real-time dashboard estimates; labeled as estimates; eventual consistency within 1 min
- **Kappa rejected**: month's replay at 500 GB/day compressed = hours even with 10 Flink workers; Lambda allows isolated batch recomputation; two-codepath complexity is mitigated by shared SQL business logic library

### Scale Numbers
- 100K clicks/sec peak; 50 MB/s ingest; Kafka: 50 MB/s × 3 RF = 150 MB/s disk
- Dedup: ~0 exact-SET ops/s in steady state (Bloom handles 99.99%); ~10 exact-SET ops/s for duplicates
- ClickHouse: ~1M agg rows/day × 200 B = 200 MB/day; query: sub-second on 5 years
- S3: 100 GB/day × 90 days = 9 TB raw; 4.3 TB/day Kafka (7-day hot replay)

### Key Differentiator
Ad Click Aggregation's uniqueness is its **Lambda architecture for billing accuracy**: dual-layer pipeline (Flink speed layer for real-time estimates + Spark batch layer for billing-grade exactness), two-level click deduplication (RedisBloom 240 MB + Redis SET NX for zero-trust duplicate prevention), HyperLogLog for unique user cardinality in 1-min windows, and ClickHouse SummingMergeTree for 5-year aggregated storage — the only problem that requires financial-grade accuracy (0.01% SLA) with sub-minute dashboard freshness simultaneously.

---

## Web Crawler

### Unique Functional Requirements
- 1B pages/day; 17K HTTP req/sec; 2 Gbps aggregate download bandwidth
- Politeness: ≤ 1 req/sec per domain; honor robots.txt Disallow/Crawl-delay
- Content deduplication: detect near-duplicate pages (90%+ content similarity)
- Recrawl scheduling: news pages within 24 hr; static pages within 30 days
- URL frontier: 9B pending URLs × 150 B = 1.35 TB; 300 TB raw HTML over 30 days
- Link extraction: 40 raw links/page → 15 after normalization; 15B new links/day; 3B new unique URLs/day

### Unique Components / Services
- **Two-Level URL Frontier**: back queues = 1,024 Redis ZSET buckets (domain_hash % 1024); score = −priority (ZPOPMIN returns highest priority); front queue = single ZSET `frontier:domain_ready` where score = `timestamp_when_domain_next_available`; atomic Lua dispatcher: `ZRANGEBYSCORE frontier:domain_ready 0 {now} LIMIT 1` → pops URL from that domain's back queue → updates domain next_available = now + crawl_delay_ms
- **Robots.txt Fetcher/Cache**: Redis cache 500 GB (100M domains × 5 KB); 24-hr TTL; fetches robots.txt on first visit or TTL expiry; 14M fetches/day = 163/s; disallow rules stored in memory per domain
- **SimHash Near-Duplicate Detector**: 64-bit SimHash (Charikar 2002); Hamming distance ≤ 3 = near-duplicate; LSH table decomposition: 4 × 16-bit segment lookup tables in Redis (`simhash:seg{0-3}:{segment_value}` → set of full hashes); O(1) near-duplicate detection across 10B corpus; 240 GB fingerprint store (30B pages × 8 B)
- **Content Fetcher Workers**: stateless; HTTP/1.1 + HTTP/2; 10 s timeout; DNS cache (10 GB, 4-hr TTL, 90% hit rate); conditional GET (ETag/Last-Modified) for recrawl efficiency; writes raw HTML to S3 → publishes crawl result to Kafka
- **Recrawl Scheduler**: reads crawl history from Cassandra; computes next_crawl_at based on content change frequency (high-change: 24 hr, low-change: 30 days); updates `url_record.next_crawl_at`; re-enqueues to frontier

### Unique Data Model
- **Cassandra `url_record`**: url_hash (xxHash64, PK), url, domain, first_seen_at, last_crawled_at, http_status, content_hash (SimHash 64-bit), content_size_bytes, etag, last_modified, crawl_depth, priority_score, next_crawl_at, s3_key
- **Redis frontier ZSET `frontier:back:{bucket}`**: member = url_hash, score = −priority_score; 1,024 buckets; ~9B URLs × 150 B ≈ 1.35 TB total
- **Redis `frontier:domain_ready`**: member = domain_hash, score = next_available_timestamp_ms; ~100K active domains at steady state
- **SimHash tables**: `simhash:seg{0-3}:{16-bit segment value}` → SET of full 64-bit SimHashes; 4 tables; 90-day TTL; 240 GB total
- **S3 raw HTML**: `crawl-raw/{year}/{month}/{day}/{hour}/{domain}/{url_hash}.html.gz`; Snappy compressed; ~10 KB/page × 1B/day = 10 TB/day; 300 TB over 30 days

### Algorithms

**Two-Level Frontier Dispatch (Atomic Lua):**
```lua
-- Find a domain ready to crawl (score ≤ now)
local ready = redis.call('ZRANGEBYSCORE', 'frontier:domain_ready', 0, ARGV[1], 'LIMIT', 0, 10)
for _, domain_hash in ipairs(ready) do
    local bucket = tonumber(domain_hash) % 1024
    local result = redis.call('ZPOPMIN', 'frontier:back:' .. bucket, 1)
    if #result > 0 then
        local crawl_delay = redis.call('HGET', 'domain:delays', domain_hash) or 1000
        redis.call('ZADD', 'frontier:domain_ready', ARGV[1] + crawl_delay, domain_hash)
        return {domain_hash, result[1]}  -- return (domain_hash, url_hash)
    end
end
```

**SimHash (Charikar 2002):**
```python
# 1. Extract 3-word shingles from page text
# 2. Hash each shingle to 64-bit integer
# 3. For each bit i: accumulate v[i] += 1 if bit set, else v[i] -= 1
# 4. Fingerprint bit i = 1 if v[i] > 0
# Near-duplicate: Hamming(h1, h2) ≤ 3

# LSH Lookup: split 64-bit hash into 4 × 16-bit segments
# By pigeonhole: Hamming ≤ 3 across 64 bits → at least one 16-bit segment is identical
# Union 4 segment lookup sets; exact Hamming check on candidates (<100 per query)
```

### Scale Numbers
- 17K HTTP req/sec; 2 Gbps download; 1B pages/day; 255 MB/s wire download
- URL frontier: 9B URLs × 150 B = 1.35 TB; 15B new links/day → 3B unique new URLs
- SimHash: 30B pages × 8 B = 240 GB fingerprint store; O(1) near-duplicate lookup
- DNS: 1,700 uncached lookups/sec (10% cache miss rate on 17K req/s); 90% cached

### Key Differentiator
Web Crawler's uniqueness is its **two-level politeness-aware URL frontier**: Redis ZSET back queues (1,024 domain-hash buckets for URL priority) + `frontier:domain_ready` front queue (domain rate-limit timestamp-based gating) with atomic Lua dispatch — combined with SimHash 64-bit near-duplicate detection (Charikar 2002) via LSH table decomposition (4 × 16-bit segment tables) enabling O(1) near-duplicate lookup across 30B+ corpus, and conditional GET (ETag/Last-Modified) for efficient recrawl — the only problem that enforces per-domain politeness as a core architectural constraint.

---

## Metrics & Logging System (Datadog-like)

### Unique Functional Requirements
- 1M data points/sec sustained; 250 MB/s ingest; 10M unique time series per day
- Multi-protocol ingest: StatsD UDP (fire-and-forget), HTTP push (Datadog `/api/v1/series`), Prometheus scrape (`/metrics`)
- Three-tier time-series storage: raw 10 s resolution (15 days), 1-min rollup (90 days), 1-hr rollup (2 years)
- Log ingestion: 500 MB/s structured + unstructured; full-text search; correlation via trace_id
- Distributed tracing: OpenTelemetry spans; flame graph rendering < 2 s p95
- Alert evaluation: 1M alerts/min = 16,667/sec; threshold + anomaly-based
- Multi-tenancy: 10,000 customers; cardinality isolation

### Unique Components / Services
- **Collector Fleet**: receives StatsD UDP (no ack) + HTTP push (< 100 ms ack); pre-aggregates (1-sec local batches); publishes to Kafka `metrics-ingest`
- **VictoriaMetrics TSDB** (selected over InfluxDB/Prometheus): custom compression — timestamps: ~1 B/sample (delta encoding); values: ~4 B/sample (Gorilla XOR float64); combined: **~5 B/sample** vs InfluxDB 15 B and Prometheus 10 B; 5 MB/s write at 1M samples/s vs 15 MB/s for InfluxDB (3× efficiency); scales to tens of millions of time series
- **TSDB Writer**: consumes Kafka; resolves MetricID via in-process `sync.Map` cache (xxHash64 of normalized metric_name + sorted tags → MetricID); writes MetricRow batches to vmstorage
- **Rollup Engine**: background job; computes 1-min aggregates from 10-s raw samples every minute; 1-hr aggregates from 1-min data every hour; uses PromQL `avg_over_time`, `sum_over_time`, etc.
- **Alert Engine**: evaluates 1M alert rules/min; loads TSDB query result per rule; state machine: OK → FIRING → RESOLVED; fan-out to PagerDuty / Slack / email / webhook
- **Elasticsearch Log Store**: 4 TB log index (7–30 day retention); tokenized field indexing; field-value queries + full-text BM25 search; correlation by trace_id field; log archive to S3 (1-year retention, 1.57 PB)
- **Trace Store (Cassandra)**: `(trace_id)` partition key; span records (service, operation, start_time, duration_ms, parent_span_id, tags); 500 GB/day, 7-day TTL = 3.5 TB; flame graph = fetch all spans for trace_id → topological sort

### Unique Data Model
- **MetricRow**: MetricNameRaw ([]byte: metric_name + sorted tag key=value pairs), Timestamp (int64 Unix ms), Value (float64)
- **VictoriaMetrics storage tiers**: `vmstorage/big/` (raw 10-s, 15 days), `vmstorage/small/` (1-min rollup, 90 days), `vmstorage/cold/` (1-hr rollup, 2 years)
- **Elasticsearch log index mapping**: `@timestamp`, `message` (text, BM25), `log_level` (keyword), `service` (keyword), `host` (keyword), `trace_id` (keyword); `_source` for full document
- **Cassandra trace_spans**: `(trace_id)` partition, `(start_time, span_id)` clustering; span_data = service, operation, parent_span_id, duration_ms, tags JSONB; TTL 7 days

### Scale Numbers
- 1M data points/sec × 5 B/sample = 5 MB/s storage write (VictoriaMetrics)
- Raw TSDB: 1.3 TB/day × 15 days = 19.5 TB; 1-min rollup: 28.8 GB/day × 90 days = 2.6 TB; 1-hr rollup: 500 MB/day × 2 years = 365 GB
- Log store: 43 TB raw/day → 4.3 TB compressed/day; ES index 28–120 TB (7–30 day retention)
- Trace: 500 GB/day × 7 days = 3.5 TB; log correlate by trace_id field in ES
- Alert evaluations: 16,667/sec; PromQL query per alert rule; 100 queries/min/user rate limit

### Key Differentiator
Metrics & Logging System's uniqueness is its **VictoriaMetrics TSDB with multi-resolution tiers**: ~5 bytes/sample (1B delta-encoded timestamp + 4B Gorilla XOR value) achieving 3× storage efficiency over InfluxDB, three-tier automatic rollup (10s/15d → 1min/90d → 1hr/2yr), multi-protocol ingest (StatsD UDP + HTTP push + Prometheus scrape), Elasticsearch log indexing with trace_id correlation, and OpenTelemetry trace store in Cassandra with < 2 s flame graph rendering — the only problem that integrates metrics + logs + traces into a unified observability platform.

---

## Real-Time Leaderboard

### Unique Functional Requirements
- 100M registered players; 10M concurrent peak; 100K score updates/sec; 600K Redis ops/sec (global + 5 regional ZSETs × 2 ops)
- Global rank query: < 10 ms p99; top-100: < 20 ms p99; friend leaderboard: < 100 ms p95
- Score update reflected in leaderboard within 1 second
- Regional leaderboards (5 regions); friend/social leaderboard (subset query)
- Historical snapshots: end-of-day, end-of-week, end-of-season; rank change WebSocket notifications
- Anti-cheat: velocity-based suspicious score detection before leaderboard update

### Unique Components / Services
- **Score Update Service**: validates anti-cheat rules (score_delta > max_per_event_type → reject 422); publishes to Kafka `score-updates`; returns 202 < 50 ms p99
- **Anti-Cheat Validator**: in-process velocity check per player (Redis sliding window: score increments over last 60 s); implausible delta → flag + reject; suspicious players queued for manual review
- **Redis Leaderboard Writer (Kafka consumer)**: consumes `score-updates`; ZADD to correct bucket ZSET + update `bucket_cumulative_above` HASH; handles score-crossing bucket boundaries (ZREM old bucket + ZADD new bucket + HINCRBY cumulative counts)
- **Bucketed ZSET Architecture**: B=100 score-range buckets (each covering 100,000 score points, max_score = 10M); each bucket = separate Redis ZSET on a different node; O(log N/B) ZADD per bucket; global rank = `HGET bucket_cumulative_above game:g1 bucket_{k}` + `ZREVRANK bucket_k player_id` (O(1) + O(log N/B))
- **Rank Notification Engine**: consumes `score-updates` Kafka; computes new rank after update; if |new_rank - old_rank| > threshold → publish to WebSocket push service; 1,000 notifications/sec × 200 B via WebSocket
- **Historical Snapshot Job**: runs at end-of-day, end-of-week, end-of-season; reads top-N from Redis ZSET buckets + all player scores from Cassandra; writes ranked list to `historical_snapshots` Cassandra table
- **Friend Leaderboard Service**: fetches friend list from social graph service (max 1,000 friends); fetches scores for each friend from Redis player metadata; sort in process; O(F log F) where F = friend count

### Unique Data Model
- **Redis bucket ZSETs**: `lb:{game_id}:bucket:{0-99}` — one per score range; member = player_id, score = player_score; ZREVRANK for rank within bucket
- **Redis `bucket_cumulative_above`**: `HSET bucket_cumulative_above game:{game_id} bucket_0 {count_above} bucket_1 {count_above} ...`; count_above = sum of sizes of all buckets with higher score ranges; updated atomically on cross-bucket movement
- **Redis player metadata**: `player:{player_id}` HASH → {score, region, display_name, updated_at}; 50 B × 10M active = 500 MB
- **Cassandra `score_records`**: `(game_id, player_id)` PK; current_score, region, last_updated_at; 100M × 200 B = 20 GB
- **Cassandra `score_history`**: `(game_id, player_id, day)` partition; `updated_at DESC` clustering; score, delta; 100M × avg 1,000 updates × 200 B = 20 TB
- **Cassandra `historical_snapshots`**: `(game_id, snapshot_type, snapshot_date)` partition; rank, player_id, score; 100M × 365 × 50 B = 1.83 TB/year

### Algorithms

**Bucketed ZSET Global Rank Computation:**
```python
def get_global_rank(game_id, player_id, score):
    bucket = min(score // BUCKET_SIZE, NUM_BUCKETS - 1)  # BUCKET_SIZE = 100,000
    bucket_key = f"lb:{game_id}:bucket:{bucket}"
    
    # O(log N/B): rank within the bucket (0-indexed from top)
    local_rank = redis.zrevrank(bucket_key, player_id)
    
    # O(1): number of players in higher-score buckets
    cumulative_above = int(redis.hget(f"bucket_cumulative_above:{game_id}", f"bucket_{bucket}") or 0)
    
    return cumulative_above + local_rank + 1  # 1-indexed
```

**Cross-Bucket Score Update:**
```python
def update_score(game_id, player_id, old_score, new_score):
    old_bucket = min(old_score // BUCKET_SIZE, NUM_BUCKETS - 1)
    new_bucket = min(new_score // BUCKET_SIZE, NUM_BUCKETS - 1)
    
    with redis.pipeline() as pipe:
        if old_bucket != new_bucket:
            pipe.zrem(f"lb:{game_id}:bucket:{old_bucket}", player_id)
            pipe.hincrby(f"bucket_sizes:{game_id}", f"bucket_{old_bucket}", -1)
            pipe.hincrby(f"bucket_sizes:{game_id}", f"bucket_{new_bucket}", 1)
            # Update cumulative_above for all buckets below new_bucket and above old_bucket
            # (simplified: handled by background reconciler for atomicity)
        pipe.zadd(f"lb:{game_id}:bucket:{new_bucket}", {player_id: new_score})
        pipe.execute()
```

**Rejected Approaches:**
- Redis Cluster hash sharding by player_id: ZRANK across shards is undefined — cross-shard rank is incorrect
- Single Redis ZSET: 37K ZADD/s max (log2(100M) ≈ 27 hops per ZADD) — insufficient for 100K/s requirement
- Pre-computed rank with periodic refresh: rank stale by refresh interval; 1-second propagation SLA not met

### Scale Numbers
- 100K score updates/sec; 600K Redis ops/sec (100K × 6: ZADD + cumulative HGET + 4 for global+regional)
- Redis ZSET: 16 B/member × 100M = 1.6 GB global; 5 regions × 1.6 GB/5 = 1.6 GB regional total
- Kafka `score-updates`: 8 MB/s × 7-day retention = 484 GB
- Cassandra: 20 GB current scores; 20 TB score history; 1.83 TB/year snapshots
- Notifications: 1,000/sec × 200 B = 200 KB/s WebSocket

### Key Differentiator
Real-Time Leaderboard's uniqueness is its **score-range bucketed ZSET with cumulative_above for O(1) global rank**: 100 Redis ZSETs (one per score range, each on a different node) + `bucket_cumulative_above` HASH enabling ZRANK computation as `cumulative_above + ZREVRANK_within_bucket` without cross-shard ZRANK, handling 100K ZADD/sec across 100M players with < 10 ms p99 — contrasted with rejected approaches (hash-sharded ZSET: undefined cross-shard rank; single ZSET: 37K ops/s ceiling) — plus WebSocket rank-change notifications and historical end-of-season snapshots.
