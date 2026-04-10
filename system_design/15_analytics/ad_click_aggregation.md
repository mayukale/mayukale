# System Design: Ad Click Aggregation

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Click Ingestion**: Accept raw click events from advertisers' ad inventory (display, search, video, native) in real time.
2. **Deduplication**: Guarantee each unique click is counted exactly once — prevent double-counting caused by retries, network replays, or click farms.
3. **Real-Time Aggregation**: Serve aggregated click counts (total, by ad, by campaign, by publisher, by geo, by device type) with latency ≤ 1 minute behind real time.
4. **Batch Reconciliation**: Produce authoritative billing-grade counts in batch (hourly/daily) that serve as the source of truth for invoicing.
5. **Fraud Detection**: Flag and exclude invalid clicks (bot traffic, abnormal click velocity, IP-level abuse) before billing.
6. **Query API**: Expose an API for advertiser dashboards to query aggregated counts with arbitrary time-range and dimension filters.
7. **Alerting**: Trigger alerts when click rates deviate by configurable thresholds (e.g., CTR drops 30% in 5 minutes).

### Non-Functional Requirements

1. **Throughput**: Handle up to 100,000 click events per second at peak (Black Friday, major campaigns).
2. **Durability**: Zero data loss. Every click event must be persisted before an acknowledgment is returned.
3. **Accuracy**: Billing counts must be accurate to within 0.01% — financial SLA.
4. **Availability**: 99.99% uptime for the ingestion pipeline (< 52 minutes downtime/year).
5. **Latency (read)**: Dashboard queries over 1-hour windows return in < 2 seconds; queries over 30-day windows return in < 10 seconds.
6. **Idempotency**: Replaying the same event any number of times must not change final counts.
7. **Scalability**: Architecture must scale horizontally to 10× current peak without redesign.
8. **Data Retention**: Raw click events retained 90 days; aggregated data retained 5 years.

### Out of Scope

- Ad serving and targeting logic (which ad to show).
- Impression tracking (separate pipeline with different semantics).
- Conversion / attribution modeling (post-click events).
- Advertiser billing payment processing.
- Real-time bidding (RTB) systems.

---

## 2. Users & Scale

### User Types

| Actor | Description |
|---|---|
| Ad SDK / Pixel | Client-side or server-side click tracker sending raw events |
| Fraud Engine | Internal service consuming click stream to score events |
| Aggregation Workers | Internal consumers computing windowed sums |
| Advertiser Dashboard | Frontend querying aggregated metrics |
| Billing System | Internal service consuming authoritative batch counts |
| Ops / SRE | Monitoring pipeline health |

### Traffic Estimates

**Assumptions**:
- 1 billion ad clicks per day globally (conservative estimate for a large-scale ad platform; Google processes ~8.5B, Meta ~5B; we model a mid-tier platform).
- Peak-to-average ratio: 3× (morning commute + lunchtime + evening peaks).
- Each raw click event payload: ~500 bytes (JSON: click_id, ad_id, campaign_id, publisher_id, user_agent, ip, timestamp, geo, device, referer).

| Metric | Calculation | Result |
|---|---|---|
| Average click rate | 1B clicks/day ÷ 86,400 s/day | ~11,574 clicks/s |
| Peak click rate | 11,574 × 3 | ~35,000 clicks/s |
| Design headroom (3×) | 35,000 × 3 | **100,000 clicks/s** |
| Kafka message rate | 100,000 msg/s × 500 B | 50 MB/s ingest |
| Daily raw event volume | 1B × 500 B | 500 GB/day |
| Annual raw event volume | 500 GB × 365 | ~180 TB/year |
| Aggregated rows/day | 1B clicks ÷ avg 1,000 clicks/cell | ~1M agg rows/day |
| Aggregated storage (5 yr) | 1M rows × 200 B × 365 × 5 | ~365 GB |

### Latency Requirements

| Operation | Target SLA |
|---|---|
| Click ingest acknowledgment | < 50 ms (p99) |
| Real-time dashboard (1-min granularity) | < 1 s (p95) |
| Historical query (1-hour window) | < 2 s (p95) |
| Historical query (30-day window) | < 10 s (p95) |
| Billing batch job completion | < 30 min after hour close |

### Storage Estimates

| Tier | Data | Size | Retention |
|---|---|---|---|
| Kafka (hot) | Raw click events | 50 MB/s × 86400 = 4.3 TB/day | 7 days (hot replay) |
| Object store (warm) | Compressed raw events (Parquet/Snappy, ~5× compression) | 100 GB/day | 90 days = 9 TB |
| OLAP (ClickHouse) | Pre-aggregated rows (1-min buckets) | ~200 MB/day | 5 years = 365 GB |
| Redis | Real-time in-flight window counts | ~10 GB (sliding 5-min windows) | Ephemeral |
| Dedup store (Redis/Bloom) | click_id seen set | ~1.2 GB/day (100M unique IDs × 12 bytes) | 24 hours |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Ingest (client → API) | 100K clicks/s × 500 B | 50 MB/s = 400 Mbps |
| API → Kafka | Same + framing overhead | ~55 MB/s |
| Kafka → consumers (×3 consumer groups) | 55 MB/s × 3 | 165 MB/s |
| Aggregation → OLAP writes | ~1M rows/day × 200 B / 86400 | ~2 KB/s (batched) |
| Dashboard query responses | 10,000 QPS × avg 10 KB resp | 100 MB/s |

---

## 3. High-Level Architecture

```
                            ┌─────────────────────────────────────────────────────────┐
                            │                   CLIENT TIER                           │
                            │  Ad SDK / Pixel / Server-side click tracker             │
                            └───────────────────────┬─────────────────────────────────┘
                                                    │ HTTPS POST /v1/clicks
                                                    ▼
                            ┌─────────────────────────────────────────────────────────┐
                            │               INGEST TIER (Stateless)                   │
                            │  ┌──────────────────────────────────────────────────┐   │
                            │  │         API Gateway / Load Balancer              │   │
                            │  │  - TLS termination, rate limiting per IP/token   │   │
                            │  │  - Request validation, auth token check          │   │
                            │  └─────────────────────┬────────────────────────────┘   │
                            │                        │                                 │
                            │  ┌─────────────────────▼────────────────────────────┐   │
                            │  │        Click Ingest Service (×N pods)            │   │
                            │  │  - Dedup check (Redis Bloom / exact Redis SET)   │   │
                            │  │  - Enrich (geo-IP, device parse)                 │   │
                            │  │  - Publish to Kafka topic: raw_clicks            │   │
                            │  │  - Return 200 + ack to client                   │   │
                            │  └─────────────────────┬────────────────────────────┘   │
                            └────────────────────────┼────────────────────────────────┘
                                                     │
                            ┌────────────────────────▼────────────────────────────────┐
                            │               MESSAGE BUS (Apache Kafka)                │
                            │  Topics:                                                │
                            │    raw_clicks         (partitioned by ad_id, 64 parts) │
                            │    valid_clicks       (post-fraud-filter)               │
                            │    invalid_clicks     (quarantine)                      │
                            └───┬───────────────────┬───────────────────┬────────────┘
                                │                   │                   │
               ┌────────────────▼──┐   ┌────────────▼────────┐  ┌──────▼───────────┐
               │  Fraud Detection  │   │  Real-Time Agg       │  │  Batch Archiver  │
               │  Service          │   │  Service (Flink)     │  │  (Spark/Hadoop)  │
               │  - IP velocity    │   │  - Tumbling windows  │  │  - Hourly jobs   │
               │  - ML scoring     │   │    (1-min, 5-min)    │  │  - Billing-grade │
               │  - Publish to     │   │  - Sliding windows   │  │    counts        │
               │  valid/invalid    │   │  - Write to Redis    │  │  - Write to S3   │
               └───────────────────┘   │    + ClickHouse      │  │    + ClickHouse  │
                                       └─────────┬────────────┘  └──────────────────┘
                                                 │
                            ┌────────────────────▼────────────────────────────────────┐
                            │                  STORAGE TIER                           │
                            │  ┌────────────────┐  ┌──────────────┐  ┌────────────┐  │
                            │  │ Redis Cluster  │  │  ClickHouse  │  │  S3/GCS    │  │
                            │  │ (real-time     │  │  (OLAP,      │  │  (raw      │  │
                            │  │  window state) │  │  aggregated) │  │  Parquet)  │  │
                            │  └────────────────┘  └──────────────┘  └────────────┘  │
                            └─────────────────────────────────────────────────────────┘
                                                 │
                            ┌────────────────────▼────────────────────────────────────┐
                            │                  QUERY TIER                             │
                            │  Query Service → Redis (hot) → ClickHouse (warm/cold)  │
                            │  Dashboard API (REST + WebSocket for live updates)      │
                            └─────────────────────────────────────────────────────────┘
```

**Component Roles**:

- **API Gateway**: TLS termination, per-publisher rate limiting (prevents a single bad actor from flooding the pipeline), auth token validation, request deduplication at HTTP layer (idempotency key in header).
- **Click Ingest Service**: Stateless pods that validate the click payload schema, perform a fast deduplication check against Redis, enrich with geo/device, and produce to Kafka. Designed to be I/O-bound; scales horizontally.
- **Apache Kafka**: Durable, ordered, partitioned log. Decouples ingest from all downstream consumers. Partitioning by `ad_id` ensures ordering per ad for windowed aggregation without cross-partition coordination.
- **Fraud Detection Service**: Stateful stream processor that maintains per-IP, per-user-agent click velocity counters. Scores each click and routes to `valid_clicks` or `invalid_clicks` topic. This is the gate before billing.
- **Real-Time Aggregation Service (Apache Flink)**: Consumes `valid_clicks`, maintains in-memory windowed state, emits 1-minute tumbling window results to Redis (for dashboard freshness) and batches to ClickHouse every 5 minutes.
- **Batch Archiver (Apache Spark)**: Hourly scheduled job reads raw Parquet from S3 (written by Kafka Connect), recomputes authoritative aggregations with full deduplication pass, writes to ClickHouse billing tables. This is the lambda architecture's batch layer.
- **ClickHouse**: Column-oriented OLAP database optimized for aggregation queries over large time-series datasets. Stores both real-time-merged and batch-authoritative aggregated rows.
- **Redis Cluster**: Serves two roles: (1) deduplication store (Bloom filter + exact-match for 24-hour window), (2) real-time window cache for dashboard hot-path queries.
- **Query Service**: Fan-out layer that serves dashboard queries. For windows < 5 minutes, reads from Redis. For older data, queries ClickHouse. Handles time-range split and result merging.

**Primary Use-Case Data Flow (click to billing count)**:

1. Client sends `POST /v1/clicks` with click event JSON.
2. API Gateway authenticates and rate-limits. Forwards to Click Ingest Service.
3. Ingest Service checks `click_id` against Redis Bloom filter. If seen → return 200 (idempotent, discard). If new → add to Bloom filter and exact-match TTL set.
4. Ingest enriches with geo-IP lookup (local MaxMind DB, in-process) and device type parsing (ua-parser).
5. Ingest publishes enriched event to Kafka `raw_clicks` topic, partitioned by `ad_id % 64`.
6. Fraud Detection Service consumes from `raw_clicks`. Checks IP velocity (> 10 clicks/min from same IP on same ad → flag). Routes to `valid_clicks` or `invalid_clicks`.
7. Flink consumes `valid_clicks`. Updates in-memory tumbling window state for `(ad_id, minute_bucket)`. On window close, emits aggregate to Redis and ClickHouse.
8. Dashboard query hits Query Service → reads from Redis for recent data, ClickHouse for historical.
9. Hourly Spark job reads Parquet from S3, performs full dedup pass, writes billing-grade counts to ClickHouse billing schema.
10. Billing system queries ClickHouse billing tables to generate invoices.

---

## 4. Data Model

### Entities & Schema

**raw_click_event** (Kafka message / S3 Parquet schema):
```sql
{
  click_id       UUID NOT NULL,        -- globally unique, client-generated
  ad_id          BIGINT NOT NULL,
  campaign_id    BIGINT NOT NULL,
  advertiser_id  BIGINT NOT NULL,
  publisher_id   BIGINT NOT NULL,
  placement_id   BIGINT NOT NULL,
  user_id        BIGINT,               -- null for anonymous
  session_id     UUID,
  ip_address     INET NOT NULL,
  user_agent     VARCHAR(512),
  device_type    ENUM('desktop','mobile','tablet','ctv','unknown'),
  country_code   CHAR(2),              -- ISO 3166-1 alpha-2
  region_code    VARCHAR(8),
  timestamp_ms   BIGINT NOT NULL,      -- epoch milliseconds, client-side
  received_at_ms BIGINT NOT NULL,      -- epoch milliseconds, server-side
  referer_url    VARCHAR(2048),
  click_url      VARCHAR(2048)
}
```

**valid_click_event** (Kafka topic: valid_clicks — adds fraud scoring):
```sql
{
  ...all fields from raw_click_event,
  fraud_score    FLOAT,               -- 0.0 (clean) to 1.0 (fraud)
  fraud_signals  ARRAY<VARCHAR>,      -- ["ip_velocity", "datacenter_ip"]
  is_valid       BOOLEAN
}
```

**agg_clicks_1min** (ClickHouse — real-time serving table):
```sql
CREATE TABLE agg_clicks_1min (
  window_start     DateTime NOT NULL,         -- truncated to minute
  ad_id            UInt64 NOT NULL,
  campaign_id      UInt64 NOT NULL,
  advertiser_id    UInt64 NOT NULL,
  publisher_id     UInt64 NOT NULL,
  device_type      LowCardinality(String),
  country_code     FixedString(2),
  click_count      UInt64,
  valid_click_count UInt64,
  invalid_click_count UInt64,
  unique_users     UInt64,                    -- HLL sketch, approximate
  unique_ips       UInt64                     -- HLL sketch
)
ENGINE = SummingMergeTree()
PARTITION BY toYYYYMM(window_start)
ORDER BY (advertiser_id, campaign_id, ad_id, window_start, publisher_id, device_type, country_code)
TTL window_start + INTERVAL 5 YEAR;
```

**agg_clicks_billing** (ClickHouse — authoritative billing table, written by Spark):
```sql
CREATE TABLE agg_clicks_billing (
  hour_start       DateTime NOT NULL,
  ad_id            UInt64 NOT NULL,
  campaign_id      UInt64 NOT NULL,
  advertiser_id    UInt64 NOT NULL,
  publisher_id     UInt64 NOT NULL,
  valid_click_count UInt64,
  invalid_click_count UInt64,
  gross_click_count UInt64,
  batch_run_id     UUID NOT NULL,            -- traceability
  created_at       DateTime DEFAULT now()
)
ENGINE = ReplacingMergeTree(created_at)
PARTITION BY toYYYYMM(hour_start)
ORDER BY (advertiser_id, campaign_id, ad_id, hour_start, publisher_id);
```

**dedup_seen** (Redis — two-level deduplication):
```
Key: bloom_filter:clicks:{YYYY-MM-DD}         Type: Bloom filter (RedisBloom)
Key: exact:click_id:{click_id}                Type: String, TTL=86400s, value="1"
```

**fraud_velocity** (Redis — per-IP counters):
```
Key: fraud:ip:{ip_address}:{ad_id}:{minute_bucket}   Type: String (INCR), TTL=300s
Key: fraud:ip:{ip_address}:{minute_bucket}           Type: String (INCR), TTL=300s
```

### Database Choice

| Database | Type | Pros | Cons | Fit |
|---|---|---|---|---|
| PostgreSQL | Row-store OLTP | Strong consistency, ACID, familiar | Poor at columnar scans, doesn't scale to TB analytics | No — wrong query pattern |
| Cassandra | Wide-column | High write throughput, partition-key range scans | Complex aggregation requires pre-modeling all query shapes, no ad-hoc | Partial — could store raw but not query aggregates |
| ClickHouse | Column-store OLAP | 100–1000× faster than RDBMS on aggregate queries, vectorized execution, LZ4 compression, SummingMergeTree for idempotent count merging | Eventual consistency on merges, not for point lookups | **Selected for aggregated data** |
| Apache Druid | Column-store OLAP | Real-time ingestion from Kafka, roll-up at ingest time, mature ad-tech adoption | Complex cluster ops, more expensive, harder to backfill | Strong alternative — ClickHouse selected for operational simplicity |
| Redis | In-memory KV | Sub-ms latency, ZADD/HINCRBY for counters, Bloom filter module | Volatile (needs AOF/RDB), expensive per GB, not a query engine | **Selected for hot-path dedup and real-time window cache** |
| S3 + Parquet | Object store + columnar file | Virtually unlimited storage, very cheap, Athena/Spark queryable | High latency for interactive queries, no indexing | **Selected for raw event archive and batch input** |

**Selection Rationale**:
- **ClickHouse** is selected as the primary OLAP store because `SummingMergeTree` natively implements idempotent partial aggregation merging, which is exactly the semantics needed when both Flink (streaming) and Spark (batch) write to the same table — duplicate writes are resolved by the engine. Its column compression (LZ4/ZSTD) achieves 10–20× compression on repetitive ad-tech data. Vectorized query execution processes billions of rows in seconds. ClickHouse is the engine powering Cloudflare's analytics at 30M rows/second.
- **Kafka** (not a database, but core to the model) is selected over Kinesis because it provides configurable partition count (64 partitions for ad_id-based ordering), exactly-once semantics with idempotent producers + transactions, and longer retention (7 days) needed for batch replay.
- **Redis** with **RedisBloom** module is selected for deduplication because a counting Bloom filter provides O(1) probabilistic membership tests with ~1% false positive rate configurable to needs. The false positive case (a valid click marked as duplicate) is acceptable at < 0.01% billing error. Exact-match TTL keys provide a safety net for the hot 24-hour window.

---

## 5. API Design

### Click Ingestion (Write Path)

```
POST /v1/clicks
Authorization: Bearer <publisher_api_token>
Idempotency-Key: <client_generated_uuid>         (header)
Content-Type: application/json
X-RateLimit-Policy: 1000/s per publisher_id

Request Body:
{
  "click_id":      "550e8400-e29b-41d4-a716-446655440000",
  "ad_id":         12345678,
  "placement_id":  98765432,
  "timestamp_ms":  1712649600000,
  "user_agent":    "Mozilla/5.0 ...",
  "ip_address":    "203.0.113.45",
  "referer_url":   "https://example.com/article",
  "click_url":     "https://advertiser.com/landing"
}

Response 200 OK:
{ "status": "accepted", "click_id": "550e8400-..." }

Response 409 Conflict (duplicate):
{ "status": "duplicate", "click_id": "550e8400-..." }

Response 429 Too Many Requests:
{ "error": "rate_limit_exceeded", "retry_after_ms": 1000 }
```

### Aggregated Metrics Query (Read Path)

```
GET /v1/metrics/clicks
Authorization: Bearer <advertiser_api_token>
X-RateLimit-Policy: 100/min per advertiser_id

Query Parameters:
  advertiser_id  REQUIRED  integer
  start_time     REQUIRED  ISO-8601 datetime
  end_time       REQUIRED  ISO-8601 datetime
  granularity    OPTIONAL  enum: minute|hour|day  (default: hour)
  group_by       OPTIONAL  comma-separated: ad_id,campaign_id,publisher_id,device_type,country_code
  campaign_id    OPTIONAL  integer filter
  ad_id          OPTIONAL  integer filter

Response 200 OK:
{
  "query": {
    "advertiser_id": 9001,
    "start_time": "2026-04-09T00:00:00Z",
    "end_time": "2026-04-09T01:00:00Z",
    "granularity": "minute"
  },
  "data": [
    {
      "window_start": "2026-04-09T00:00:00Z",
      "ad_id": 12345678,
      "campaign_id": 111,
      "click_count": 4820,
      "valid_click_count": 4781,
      "invalid_click_count": 39
    }
  ],
  "pagination": {
    "cursor": "eyJ3aW5kb3ciOiIyMDI2LTA0LTA5VDAwOjA1OjAwWiJ9",
    "has_more": true
  }
}
```

### Billing Summary Query

```
GET /v1/billing/summary
Authorization: Bearer <internal_billing_service_token>
X-RateLimit-Policy: 10/min (billing system only)

Query Parameters:
  advertiser_id  REQUIRED  integer
  year_month     REQUIRED  YYYY-MM

Response 200 OK:
{
  "advertiser_id": 9001,
  "period": "2026-04",
  "campaigns": [
    {
      "campaign_id": 111,
      "gross_clicks": 14200000,
      "valid_clicks": 13950000,
      "invalid_clicks": 250000,
      "billable_clicks": 13950000
    }
  ],
  "generated_at": "2026-05-01T00:28:14Z",
  "batch_run_id": "a3f2c1d0-..."
}
```

### Real-Time Click Stream (WebSocket)

```
WS /v1/stream/clicks
Authorization: Bearer <dashboard_token>

Subscribe Message:
{
  "action": "subscribe",
  "filters": {
    "advertiser_id": 9001,
    "ad_ids": [12345678, 12345679]
  },
  "update_interval_ms": 5000
}

Server Push (every interval):
{
  "type": "window_update",
  "window_start": "2026-04-09T14:32:00Z",
  "window_end": "2026-04-09T14:33:00Z",
  "metrics": [
    { "ad_id": 12345678, "click_count": 342, "valid_count": 338 }
  ]
}
```

---

## 6. Deep Dive: Core Components

### Core Component 1: Click Deduplication

**Problem it solves**: Ad SDKs retry on network timeout; mobile apps may batch and re-send clicks; browser prefetchers trigger click URLs erroneously. Without deduplication, a single user interaction can be billed 2–5×. At 1B clicks/day, even a 0.1% duplicate rate = 1M extra billed clicks. Financial and legal consequences are severe. The challenge is performing dedup at 100K events/second with < 5ms overhead per event.

**Approach Comparison**:

| Approach | Mechanism | Throughput | False Positive Rate | Memory | Durability | Notes |
|---|---|---|---|---|---|---|
| Exact-match Redis SET | `SETNX click_id TTL=86400` | ~200K ops/s per Redis node | 0% | ~1.2 GB/day (100M × 12B IDs) | Survives Redis restart (AOF) | Best accuracy, higher memory |
| Redis Bloom Filter (RedisBloom) | Probabilistic membership, counting Bloom | ~500K ops/s | ~0.01% (configurable) | ~150 MB/day | RDB persistence | Low memory, acceptable error |
| Two-Level Hybrid | Bloom filter first pass, exact SET on bloom miss | ~500K ops/s | ~0.01% (bloom only) | ~200 MB/day | AOF+RDB | Best of both — production choice |
| Database unique index | INSERT IGNORE into MySQL | ~20K writes/s | 0% | Disk-backed | Durable | Too slow, creates DB bottleneck |
| Client-side dedup only | Publisher guarantees unique IDs | N/A | Depends on publisher | None | N/A | Unacceptable — no trust of clients |

**Selected Approach: Two-Level Hybrid (Bloom Filter + Exact Redis SET)**

**Reasoning**: The Bloom filter handles the 99.99% common case (new click) in O(1) with minimal memory (RedisBloom uses ~10 bits/element for 1% FP rate; 100M clicks/day × 10 bits = 125 MB/day). Only on a Bloom "not seen" result do we attempt an exact Redis `SET NX` with 24-hour TTL. This two-step ensures the exact SET is only called for genuinely new IDs, reducing exact-SET throughput requirement from 100K/s to effectively ~0 (only retried clicks hit exact SET, perhaps 0.01% of traffic = 10 events/s).

The 0.01% Bloom false positive rate means ~10,000 valid clicks per day are incorrectly identified as duplicates and dropped. This is an acceptable trade-off: (10K / 1B) = 0.001% billing under-count, within the 0.01% SLA, and systematically in the advertiser's favor (under-billing is legally safer than over-billing).

**Implementation Pseudocode**:

```python
class ClickDeduplicator:
    def __init__(self, redis_client, bloom_key_prefix="bloom:clicks", 
                 exact_key_prefix="exact:click"):
        self.redis = redis_client
        self.bloom_prefix = bloom_key_prefix
        self.exact_prefix = exact_key_prefix

    def is_duplicate(self, click_id: str, date_str: str) -> bool:
        """
        Returns True if this click_id has been seen before.
        Uses two-level check: Bloom filter → Exact Redis SET.
        """
        bloom_key = f"{self.bloom_prefix}:{date_str}"
        
        # Step 1: Bloom filter check (fast path)
        # BF.EXISTS is O(k) where k = number of hash functions (~7 for 1% FP)
        bloom_exists = self.redis.execute_command("BF.EXISTS", bloom_key, click_id)
        
        if not bloom_exists:
            # Definitely new — add to Bloom and create exact key atomically
            pipe = self.redis.pipeline(transaction=True)
            pipe.execute_command("BF.ADD", bloom_key, click_id)
            pipe.expire(bloom_key, 86400 * 2)  # 2 days TTL for cross-day events
            exact_key = f"{self.exact_prefix}:{click_id}"
            pipe.set(exact_key, "1", nx=True, ex=86400)
            results = pipe.execute()
            # results[2] = True if SET NX succeeded (new key), False if already exists
            # This handles a race condition where two concurrent requests for same ID
            # both pass Bloom check simultaneously (Bloom not yet updated)
            return results[2] is None  # SET NX returned None means key existed

        # Step 2: Bloom says "maybe seen" — verify with exact key
        exact_key = f"{self.exact_prefix}:{click_id}"
        existing = self.redis.get(exact_key)
        
        if existing is not None:
            return True  # Confirmed duplicate
        
        # Bloom false positive: exact key doesn't exist → this is a new click
        # Add exact key for future checks
        pipe = self.redis.pipeline(transaction=True)
        pipe.set(exact_key, "1", nx=True, ex=86400)
        pipe.execute_command("BF.ADD", bloom_key, click_id)
        results = pipe.execute()
        return results[0] is None  # True = race condition duplicate

    def process_click(self, click_event: dict) -> str:
        """Returns 'accepted', 'duplicate', or 'error'"""
        date_str = datetime.utcfromtimestamp(
            click_event['timestamp_ms'] / 1000
        ).strftime('%Y-%m-%d')
        
        try:
            if self.is_duplicate(click_event['click_id'], date_str):
                metrics.increment('clicks.duplicate')
                return 'duplicate'
            metrics.increment('clicks.accepted')
            return 'accepted'
        except redis.RedisError as e:
            # Redis unavailable: fail open (allow click through) to avoid 
            # dropping valid clicks during outage. Log for post-hoc dedup.
            logger.error(f"Dedup Redis error: {e}")
            metrics.increment('clicks.dedup_bypassed')
            return 'accepted'  # Batch layer will deduplicate later
```

**Bloom Filter Sizing Calculation**:
- n = 100,000,000 elements (clicks/day)
- p = 0.0001 (0.01% false positive rate)
- Optimal m (bits) = -n × ln(p) / (ln(2))² = -1e8 × ln(0.0001) / 0.480 = 1e8 × 9.21 / 0.480 ≈ 1.92 billion bits = **240 MB**
- Optimal k (hash functions) = (m/n) × ln(2) = 19.2 × 0.693 ≈ **13 hash functions**

**Interviewer Q&As**:

Q1: What happens if Redis goes down during deduplication?
A: We fail open — the click is accepted without dedup check. This creates a window where duplicates can slip through. The batch layer (Spark hourly job) performs full exact deduplication on raw Parquet using click_id as a true primary key. Billing is computed only from batch counts, so the financial impact is zero. The real-time counts may temporarily over-count, but they're labeled as estimates. We also persist bloom filter state to Redis RDB every 60 seconds and replicate to a standby Redis, so recovery is fast.

Q2: The Bloom filter has a 0.01% false positive rate. Over 1 billion clicks/day, that's 100,000 lost valid clicks. Is that acceptable?
A: It depends on the business model. For CPM (cost per mille) advertising, a 0.01% under-count is well within measurement uncertainty. For performance advertising (CPC campaigns with high click value), advertisers may negotiate tighter SLAs. In that case, we can reduce the FP rate to 0.001% by tripling memory (from 240 MB to 720 MB) — still well within operational budget. Additionally, the batch layer can cross-reference Bloom-rejected clicks against the exact-match audit log to recover any that were incorrectly dropped.

Q3: How do you handle click_id collisions? What if two different legitimate clicks have the same ID?
A: click_id is a client-generated UUID v4. The collision probability for UUID v4 is 1/(2^122) ≈ 10^-37, which is astronomically small — negligible for any practical system. However, we cannot trust clients. We append a server-side component: `server_click_id = SHA256(publisher_id + click_id + received_at_ms)`, and use this for dedup. This prevents a malicious publisher from deliberately reusing UUIDs to suppress competitor clicks.

Q4: How do you handle cross-day duplicates? A click sent at 11:59 PM and its retry at 12:01 AM use different Bloom filter keys.
A: The exact-key TTL is 24 hours from insertion time (not midnight boundary). So a click_id inserted at 11:59 PM has its exact key valid until 11:59 PM the next day. The Bloom filter key has a 48-hour TTL (2 days), which spans the midnight boundary. The exact-key check covers the gap: if Bloom says "not seen" but exact key exists, we correctly identify it as a duplicate.

Q5: Why use Redis for dedup rather than a distributed cache like Memcached or a database?
A: Memcached lacks atomic `SETNX` semantics needed for race-condition-free dedup — two concurrent requests for the same click_id would both see "not found" and both proceed. Redis's single-threaded execution model for commands makes `SETNX` atomic without distributed locking. Redis also supports the `BF.*` commands via the RedisBloom module (now bundled in Redis Stack), giving us the two-level approach in one system. Memcached would require us to build separate infrastructure for the Bloom filter.

---

### Core Component 2: Lambda Architecture — Real-Time vs. Batch Aggregation

**Problem it solves**: There are two conflicting requirements: (1) advertisers want to see click counts within 1 minute of the click (real-time monitoring), and (2) billing must be 100% accurate, auditable, and consistent — no missed events due to late arrivals, out-of-order events, or stream processing failures. A pure streaming system can achieve (1) but struggles with (2) due to late-arriving events (mobile clicks buffered offline for hours). A pure batch system achieves (2) but fails (1).

**Approach Comparison**:

| Architecture | Speed | Accuracy | Complexity | Late Data Handling | Operational Cost |
|---|---|---|---|---|---|
| Batch only (Spark hourly) | Minutes-to-hours lag | High (full recompute) | Low | Perfect (all data present) | Low |
| Streaming only (Flink) | Seconds lag | Medium (late events dropped/estimated) | Medium | Configurable watermarks, still lossy | Medium |
| Lambda (Batch + Speed layers) | Speed layer: seconds; Batch: hourly | Highest (batch is authoritative) | High (two codepaths) | Batch catches all late events | High |
| Kappa (single streaming layer) | Seconds lag | High (reprocess from Kafka on correction) | Medium | Replayable log covers it | Medium |
| Micro-batch (Spark Streaming) | 1-min lag | Medium | Low | Similar to streaming | Medium |

**Selected Approach: Lambda Architecture**

**Reasoning**: We chose Lambda because billing accuracy is a hard financial requirement — not a soft SLA. The batch layer is the source of truth for invoicing. The speed (streaming) layer serves the real-time dashboard. This decoupling allows us to improve/fix the batch pipeline independently of the streaming layer. Kappa architecture (single streaming system) is attractive for simplicity, but requires replaying the entire Kafka topic for any correction — at 500 GB/day compressed, a month's replay takes hours even with 10 Flink workers. Lambda's batch layer can recompute any hour in isolation. The "high complexity" tradeoff is mitigated by the fact that the batch and streaming SQL queries are nearly identical (both use GROUP BY ad_id, campaign_id, TUMBLE/bucket) — we share the business logic as a library.

**Flink Real-Time Aggregation Pseudocode**:

```java
public class ClickAggregationJob {

    public static void main(String[] args) throws Exception {
        StreamExecutionEnvironment env = StreamExecutionEnvironment.getExecutionEnvironment();
        env.setParallelism(64);  // Match Kafka partition count
        
        // Source: Kafka valid_clicks topic with exactly-once semantics
        KafkaSource<ClickEvent> source = KafkaSource.<ClickEvent>builder()
            .setBootstrapServers("kafka:9092")
            .setTopics("valid_clicks")
            .setGroupId("flink-click-aggregator")
            .setStartingOffsets(OffsetsInitializer.committedOffsets())
            .setValueOnlyDeserializer(new ClickEventDeserializer())
            .build();

        DataStream<ClickEvent> clicks = env
            .fromSource(source, WatermarkStrategy
                .<ClickEvent>forBoundedOutOfOrderness(Duration.ofMinutes(5))
                .withTimestampAssigner((event, ts) -> event.getTimestampMs()),
                "Kafka Click Source");

        // 1-minute tumbling window aggregation
        DataStream<ClickAggregate> aggregated = clicks
            .keyBy(click -> new Tuple3<>(
                click.getAdId(), 
                click.getPublisherId(), 
                click.getCountryCode()))
            .window(TumblingEventTimeWindows.of(Time.minutes(1)))
            .allowedLateness(Time.minutes(5))  // Accept late events up to 5 min
            .sideOutputLateData(lateClicksTag)
            .aggregate(new ClickCountAggregator(), new ClickWindowFunction());

        // Sink 1: Redis for real-time dashboard (fire-and-forget, at-least-once)
        aggregated.addSink(new RedisSink<>(redisConfig, new ClickAggRedisMapper()));

        // Sink 2: ClickHouse for persistent storage (batched, exactly-once via upsert)
        aggregated
            .keyBy(agg -> agg.getWindowStart())
            .window(TumblingProcessingTimeWindows.of(Time.minutes(5)))
            .process(new BatchedClickHouseWriter(clickHouseConfig));

        // Handle late-arriving clicks: write to a separate late-data topic
        // Batch layer will pick these up in the next hourly run
        DataStream<ClickEvent> lateClicks = clicks.getSideOutput(lateClicksTag);
        lateClicks.sinkTo(KafkaSink.<ClickEvent>builder()
            .setBootstrapServers("kafka:9092")
            .setRecordSerializer(KafkaRecordSerializationSchema.builder()
                .setTopic("late_clicks")
                .setValueSerializationSchema(new ClickEventSerializer())
                .build())
            .build());

        env.execute("Click Aggregation Pipeline");
    }
}

// Aggregation function — accumulates count and HLL for unique users
public class ClickCountAggregator 
    implements AggregateFunction<ClickEvent, ClickAggAccumulator, ClickAggregate> {

    @Override
    public ClickAggAccumulator createAccumulator() {
        return new ClickAggAccumulator();  // {count: 0, hll: HyperLogLog(12)}
    }

    @Override
    public ClickAggAccumulator add(ClickEvent event, ClickAggAccumulator acc) {
        acc.totalCount++;
        acc.validCount += event.isValid() ? 1 : 0;
        acc.hll.offer(event.getUserId());  // HLL for unique user approx
        acc.ipHll.offer(event.getIpAddress());
        return acc;
    }

    @Override
    public ClickAggregate getResult(ClickAggAccumulator acc) {
        return new ClickAggregate(acc.totalCount, acc.validCount, 
                                  acc.hll.cardinality(), acc.ipHll.cardinality());
    }

    @Override
    public ClickAggAccumulator merge(ClickAggAccumulator a, ClickAggAccumulator b) {
        a.totalCount += b.totalCount;
        a.validCount += b.validCount;
        a.hll.merge(b.hll);  // HLL merging is the key property we need
        a.ipHll.merge(b.ipHll);
        return a;
    }
}
```

**Spark Batch Layer Pseudocode**:

```python
def run_billing_batch(hour_start: datetime, spark: SparkSession):
    """
    Authoritative hourly batch job for billing.
    Reads raw Parquet, deduplicates, computes counts.
    """
    hour_end = hour_start + timedelta(hours=1)
    
    # Read raw events from S3 (includes 5-min buffer for very late arrivals)
    raw_df = spark.read.parquet(
        f"s3://clicks-archive/raw/{hour_start.strftime('%Y/%m/%d/%H')}/*.parquet"
    )
    
    # Also read the previous hour's late_clicks topic (Kafka → S3 via Kafka Connect)
    late_df = spark.read.parquet(
        f"s3://clicks-archive/late/{hour_start.strftime('%Y/%m/%d/%H')}/*.parquet"
    )
    
    all_clicks = raw_df.union(late_df)
    
    # Step 1: Exact deduplication by click_id
    # Window function picks the first occurrence by received_at_ms
    deduped = (all_clicks
        .withColumn("row_num", F.row_number().over(
            Window.partitionBy("click_id").orderBy("received_at_ms")
        ))
        .filter("row_num = 1")
        .drop("row_num")
    )
    
    # Step 2: Filter to events actually belonging to this hour
    # (some events are sent with wrong timestamps; use received_at_ms)
    in_window = deduped.filter(
        (F.col("received_at_ms") >= hour_start.timestamp() * 1000) &
        (F.col("received_at_ms") < hour_end.timestamp() * 1000)
    )
    
    # Step 3: Aggregate by billing dimensions
    billing_agg = (in_window
        .groupBy("ad_id", "campaign_id", "advertiser_id", "publisher_id")
        .agg(
            F.count("*").alias("gross_click_count"),
            F.sum(F.when(F.col("is_valid") == True, 1).otherwise(0))
             .alias("valid_click_count"),
            F.sum(F.when(F.col("is_valid") == False, 1).otherwise(0))
             .alias("invalid_click_count")
        )
        .withColumn("hour_start", F.lit(hour_start))
        .withColumn("batch_run_id", F.lit(str(uuid.uuid4())))
    )
    
    # Step 4: Write to ClickHouse (ReplacingMergeTree handles reruns idempotently)
    billing_agg.write \
        .format("clickhouse") \
        .option("url", CLICKHOUSE_URL) \
        .option("table", "agg_clicks_billing") \
        .mode("append") \
        .save()
    
    logger.info(f"Billing batch for {hour_start} complete. "
                f"Rows written: {billing_agg.count()}")
```

**Interviewer Q&As**:

Q1: How do you handle events that arrive more than 5 minutes late — say, a mobile app that was offline for 2 hours?
A: Late events (received_at_ms more than 5 minutes after their timestamp_ms) are routed to the `late_clicks` Kafka topic by Flink's side output mechanism. Kafka Connect writes this topic to S3 every 15 minutes. The hourly Spark batch job reads both the primary raw events AND the previous 2-hour window of late events, unions them, deduplicates, and recomputes. The `ReplacingMergeTree` engine in ClickHouse means re-running the batch job for the same hour with updated data automatically replaces old rows (via `batch_run_id` as the version). For events arriving > 6 hours late (pathological case), we run a daily reconciliation job with a 12-hour event window.

Q2: You mentioned the batch layer is the source of truth. But advertisers see real-time numbers on their dashboards. How do you avoid confusion when batch numbers differ from streaming numbers?
A: The dashboard API labels all data with its source: `"data_quality": "real_time_estimate"` for streaming data less than 1 hour old, and `"data_quality": "billing_final"` once the batch job has run. The UI displays a banner: "Data for the last 60 minutes is an estimate. Final billing counts are updated hourly." This sets correct expectations. The typical discrepancy is < 0.5% and usually in the under-count direction (late events not yet received). We track the discrepancy as a metric (`billing_vs_streaming_delta_pct`) and alert if it exceeds 2%.

Q3: Why not use Kappa architecture (just Flink + Kafka replay) instead of Lambda?
A: Kappa is operationally simpler and is the right choice for many systems. Our specific constraint is that a billing dispute might require recomputing any historical hour with updated fraud signals (e.g., we discover a click farm 3 weeks later and need to retroactively invalidate their clicks). In Kappa, this requires replaying the full Kafka log from that date. At 500 GB/day compressed raw events with 90-day retention, that's up to 45 TB of replay — hours of processing. In Lambda, we re-run the Spark job for affected hours against the already-stored Parquet files with updated fraud rules applied as a filter. This reruns in minutes per affected hour.

Q4: How does ClickHouse's SummingMergeTree handle concurrent writes from both Flink and Spark for the same time window?
A: `SummingMergeTree` doesn't conflict in the traditional sense — it merges rows with the same primary key by summing numeric columns. If Flink writes `(ad_id=123, minute=14:00, count=4820)` and Spark writes `(ad_id=123, minute=14:00, count=4781)` (the authoritative count), they'll both exist as separate parts initially. For the real-time dashboard table, this is fine — we use `sumMerge` in queries. For billing, we use a separate `ReplacingMergeTree` table where Spark writes only (Flink doesn't write to billing table), so there's no conflict. The tables are intentionally separate for exactly this reason.

Q5: What's your strategy for backfilling historical data when the aggregation logic changes (e.g., new fraud rule)?
A: The raw Parquet files in S3 are immutable and never deleted within the 90-day window. A backfill is just rerunning the Spark batch job with the new fraud rule as an additional filter, writing to ClickHouse with a new `batch_run_id`. The `ReplacingMergeTree` engine picks the latest row by `created_at`. For backfills older than 90 days, we rely on the fact that we archive compressed raw events to Glacier-class storage indefinitely (at very low cost — 500 GB/day × 365 days × $0.004/GB-month = ~$730/year). A 1-year backfill is operationally possible with Spark on a temporary EMR cluster, typically completing in 4–8 hours.

---

### Core Component 3: Fraud Detection

**Problem it solves**: Invalid clicks (click fraud) inflate advertiser costs without delivering genuine user interest. Sources include: competitor click fraud, botnet traffic, publisher self-clicks (incentivized fraud), and ad stacking (invisible ads). At 1B clicks/day, even 1% fraud = 10M fraudulent billings. Industry estimates put ad fraud at 8–10% of digital ad spend. We must detect and exclude fraud in near-real-time (before billing) and in batch (for retroactive disputes).

**Approach Comparison**:

| Signal | Detection Method | Latency | False Positive Risk | Notes |
|---|---|---|---|---|
| IP click velocity | Per-IP Redis counter with sliding window | < 1 ms | Low | >10 clicks/min from same IP on same ad = fraud |
| Datacenter/VPN IP | IP reputation DB (MaxMind GeoIP + IPQualityScore) | < 2 ms | Medium | Legitimate users also use VPNs |
| User-agent anomalies | Known bot signatures (ua-parser + custom blocklist) | < 1 ms | Low | Curl, Scrapy, Googlebot patterns |
| Click pattern (ML) | Gradient boosting model on feature vector | < 10 ms | Tunable via threshold | Best accuracy, higher latency |
| Click timing (sub-second) | Multiple clicks within 200ms on same placement | < 1 ms | Low | Humans cannot click that fast |
| Device fingerprint | Canvas/audio fingerprint hash matching | < 5 ms | Medium | Requires JS execution |

**Selected Approach: Multi-Signal Rule Engine + Async ML Scoring**

Real-time path applies rule-based signals (IP velocity, UA check, timing) with < 2 ms overhead. ML scoring runs asynchronously on a side channel and writes fraud scores back to ClickHouse for batch billing adjustment. This gives us deterministic low-latency fraud filtering without blocking the critical ingest path on model inference.

**Fraud Detection Pseudocode**:

```python
class FraudDetector:
    VELOCITY_THRESHOLD_PER_AD = 10      # clicks/min from same IP on same ad
    VELOCITY_THRESHOLD_GLOBAL = 50      # clicks/min from same IP across all ads
    MIN_CLICK_INTERVAL_MS = 200          # minimum human reaction time

    def __init__(self, redis, ip_reputation_db, ua_classifier):
        self.redis = redis
        self.ip_db = ip_reputation_db
        self.ua_clf = ua_classifier

    def score_click(self, click: ClickEvent) -> FraudResult:
        signals = []
        fraud_score = 0.0

        # Signal 1: IP velocity check (O(1) Redis INCR with TTL)
        minute_bucket = click.timestamp_ms // 60000
        ip_ad_key = f"vel:{click.ip}:{click.ad_id}:{minute_bucket}"
        ip_global_key = f"vel:{click.ip}:{minute_bucket}"
        
        pipe = self.redis.pipeline()
        pipe.incr(ip_ad_key)
        pipe.expire(ip_ad_key, 120)  # 2-min TTL
        pipe.incr(ip_global_key)
        pipe.expire(ip_global_key, 120)
        results = pipe.execute()
        
        ip_ad_count = results[0]
        ip_global_count = results[2]
        
        if ip_ad_count > self.VELOCITY_THRESHOLD_PER_AD:
            signals.append("ip_velocity_per_ad")
            fraud_score = max(fraud_score, 0.9)
        if ip_global_count > self.VELOCITY_THRESHOLD_GLOBAL:
            signals.append("ip_velocity_global")
            fraud_score = max(fraud_score, 0.95)

        # Signal 2: IP reputation check (local in-process lookup, MaxMind DB)
        ip_info = self.ip_db.lookup(click.ip_address)
        if ip_info.is_datacenter or ip_info.is_tor or ip_info.is_vpn:
            signals.append(f"ip_type:{ip_info.connection_type}")
            fraud_score = max(fraud_score, 0.7)

        # Signal 3: User-agent classification
        ua_result = self.ua_clf.classify(click.user_agent)
        if ua_result.is_bot:
            signals.append(f"bot_ua:{ua_result.bot_name}")
            fraud_score = max(fraud_score, 0.99)  # Near-certain fraud

        # Signal 4: Sub-second click timing (check last click time for this IP)
        last_click_key = f"last_click:{click.ip}:{click.placement_id}"
        last_ts = self.redis.get(last_click_key)
        if last_ts and (click.timestamp_ms - int(last_ts)) < self.MIN_CLICK_INTERVAL_MS:
            signals.append("sub_second_click")
            fraud_score = max(fraud_score, 0.85)
        self.redis.set(last_click_key, str(click.timestamp_ms), ex=60)

        is_valid = fraud_score < 0.5  # Threshold for billing exclusion

        return FraudResult(
            click_id=click.click_id,
            fraud_score=fraud_score,
            fraud_signals=signals,
            is_valid=is_valid
        )
```

**Interviewer Q&As**:

Q1: How do you handle a sophisticated click farm that distributes clicks across thousands of IPs to evade velocity limits?
A: Single-signal defenses are bypassable. We layer ML features that are harder to evade: behavioral patterns (click-to-conversion rate by publisher over rolling 7 days — publishers with 0% conversion despite high clicks are suspicious), device fingerprinting consistency (same fingerprint hash appearing under different IPs), and campaign-level anomaly detection (sudden 10× CTR spike on a specific ad). The ML model (XGBoost trained weekly on labeled fraud data from advertiser disputes) captures these multi-dimensional patterns. Click farms face an arms race, but our ML retraining cadence (weekly) keeps us ahead of most campaigns.

Q2: Your fraud detection excludes clicks from billing. What's the appeals process when a legitimate advertiser says you incorrectly flagged their traffic?
A: All fraud decisions are logged with the full signal list and score. Advertisers can open a billing dispute via the API (`POST /v1/billing/disputes`). Our dispute resolution team can pull the raw click events for the disputed period (retained in S3 for 90 days) and the fraud signals that triggered the exclusion. We can also replay the Spark batch job with relaxed fraud thresholds to compute an "appeals-adjusted" count. This audit trail is part of why the raw events must be retained even after aggregation — it's a legal and contractual requirement.

Q3: How does fraud detection interact with deduplication? Could a duplicate click slip through if the first copy was flagged as fraud?
A: Deduplication happens before fraud scoring in the pipeline. The click_id is deduplicated at the ingest layer; only one instance reaches Kafka. Fraud scoring then operates on that single instance. If the first (and only) copy is fraudulent, it goes to `invalid_clicks` — correct behavior. A retry of a fraudulent click would be deduped away before fraud scoring — also correct. The two processes are independent and the ordering (dedup first, fraud second) is intentional.

---

## 7. Scaling

### Horizontal Scaling

- **Ingest Service**: Stateless pods behind load balancer. Scale to 200 pods at 500 clicks/pod/s (100K clicks/s total). Auto-scaled via CPU + custom Kafka consumer lag metric. Kubernetes HPA with custom metrics adapter.
- **Kafka**: 64 partitions for `raw_clicks` (sized for 100K msg/s at ~1.5 KB/msg = 150 MB/s; Kafka handles 500 MB/s per broker; 3 brokers sufficient). Each consumer group gets all 64 partitions. Adding consumers beyond 64 requires partition expansion (online re-partition with Cruise Control).
- **Flink**: Parallelism = 64 (matches Kafka partition count). State backend = RocksDB (spills to disk, handles window state > memory). Flink's operator chaining minimizes serialization overhead between map and aggregate operators.
- **ClickHouse**: Sharded by `advertiser_id % N` using ClickHouse's `Distributed` table engine. Start with N=4 shards, each with 2 replicas. Each shard handles ~250K rows/day; a single ClickHouse node handles 1B rows. Query routing via Distributed table layer is transparent to the Query Service.
- **Redis**: Redis Cluster with 6 nodes (3 primary + 3 replica). Bloom filter keys are per-day so they expire naturally. Hash slot-based sharding distributes click_id keys uniformly. For dedup, 6-node cluster handles 500K+ ops/second (Bloom BF.EXISTS is memory-bound, not CPU-bound).

### DB Sharding

ClickHouse sharding strategy:

```sql
-- Distributed table definition
CREATE TABLE agg_clicks_1min_dist AS agg_clicks_1min
ENGINE = Distributed(
  'clicks_cluster',           -- cluster name
  'default',                  -- database
  'agg_clicks_1min',          -- local table
  xxHash32(advertiser_id)     -- sharding key: routes by advertiser
);
```

Sharding by `advertiser_id` is chosen because:
1. Queries are always scoped to an advertiser (enforced by auth middleware).
2. All data for one advertiser on one shard = no cross-shard scatter-gather for the 99% case.
3. Hot shard risk: top 10 advertisers may dominate. Mitigated by secondary hash if needed.

### Replication

- Kafka: replication factor 3, min.insync.replicas=2. Leader election via ZooKeeper/KRaft. An event is acknowledged only after written to 2 replicas — no data loss even on simultaneous dual broker failure.
- ClickHouse: `ReplicatedMergeTree` via ZooKeeper. Each shard has 2 replicas in separate availability zones. Replication is async but convergent; queries route to either replica, reads may see slight lag.
- Redis: AOF persistence (appendfsync=everysec) on all primaries. Replica lag is typically < 100 ms. Sentinel or Redis Cluster's built-in failover handles primary failure within 10 seconds.

### Caching

- **L1 Cache (In-process)**: Ingest service caches geo-IP lookups (MaxMind DB is loaded in memory per pod — 65 MB). UA-parser patterns are pre-compiled per pod. No Redis hop for these.
- **L2 Cache (Redis)**: Dashboard hot queries (last 5-minute window aggregates per ad_id) are stored as Redis HASHes. Cache TTL = 60 seconds. Cache hit rate expected > 95% for popular ads (zipf distribution — top 1% of ads get 80% of queries).
- **L3 (ClickHouse mark cache)**: ClickHouse's mark files (sparse index granules) are cached in memory. With 64 GB RAM per ClickHouse node and typical working set of 1 week's data (~1.4 GB), the entire hot dataset fits in mark cache, making queries sub-second.

### CDN

- Not directly applicable to click ingestion (latency-sensitive, must reach origin for dedup).
- **However**: Static ad assets (images, JS SDK) are served from CDN (CloudFront/Akamai). The click tracking pixel URL is a CDN-edge endpoint that immediately returns a 1×1 GIF (no wait for processing) and forwards the click event asynchronously to the origin ingest service. This decouples the user-visible response latency (< 10 ms CDN edge) from the processing latency (50 ms p99 origin).

### Interviewer Q&As (Scaling)

Q1: Your Kafka has 64 partitions for `raw_clicks`. What happens when you need to scale to 200 partitions as traffic grows?
A: Kafka supports online partition expansion via `kafka-topics --alter --partitions 200`. However, this does NOT rebalance existing data — old messages stay in their original partitions, new messages use the new 64–200 range. For ordering guarantees (we partition by `ad_id`), this means some ad_ids that were on partition N might now hash to partition M for new events. Since our Flink windows are time-bounded (1 minute), and we use event-time watermarks, a brief period of cross-partition disorder is handled by Flink's allowed lateness setting. For a cleaner migration, we create a new topic `raw_clicks_v2` with 200 partitions and cut over at a clean timestamp boundary. Both topics are consumed for a 1-hour overlap period.

Q2: ClickHouse is sharded by advertiser_id. What if one advertiser (e.g., Amazon) drives 20% of all traffic?
A: This is the hot shard problem. Mitigation options: (1) Add a secondary hash dimension: `xxHash32(advertiser_id || (ad_id % 4))` — this distributes a single large advertiser across 4× more shards. (2) Dedicated shard for top-10 advertisers (identified by pre-registration or usage-based routing). (3) ClickHouse's `Distributed` table supports a custom sharding key expression including modulo, allowing fine-grained control. We monitor shard disk usage and query latency percentiles per shard; a > 3× imbalance triggers a resharding operation (which in ClickHouse requires a new cluster, data copy, then cutover).

Q3: How does Redis Cluster handle the dedup Bloom filter, which is a single large key?
A: This is a known limitation of Redis Cluster — all keys in a single slot must fit on one node. A 240 MB Bloom filter is a single key, which lives on one Redis primary. With 3 primaries and one hot 240 MB key, we lose the cluster's distribution benefit for that key. Our solution: shard the Bloom filter by the first 2 hex characters of click_id (256 shards, but grouped into 16 logical buckets using `{bloom:0}` through `{bloom:f}`). Each bucket is a separate Bloom filter covering 1/16 of the key space. Queries route to the correct bucket deterministically. This distributes the 240 MB across 16 × 15 MB keys, well within Redis's per-key memory guidance.

Q4: How do you scale the fraud detection service?
A: The rule-based fraud signals (velocity, IP reputation, UA) are stateless per-event except for the Redis counters. Fraud service pods scale horizontally like the ingest service. Redis counters are the bottleneck: at 100K events/s × 4 Redis commands/event = 400K Redis ops/s. Redis handles 1M+ simple commands/second on modern hardware. For the ML scoring path, which takes 10 ms per event, we need 100K / (1000ms / 10ms) = 1000 concurrent inferences. We run a separate ML scoring service (ONNX runtime) with a thread pool of 100 and a queue depth of 10K. ML scoring is async — clicks are accepted before ML score is available, but the ML score updates the `is_valid` flag within 200 ms (well before billing runs).

Q5: At 500 GB/day of raw Parquet on S3, how long does the hourly Spark batch job take?
A: The hourly Spark job processes 500 GB/24 = ~21 GB/hour. A 20-node Spark cluster (each with 16 vCPU, 64 GB RAM, reading from S3 at 1 GB/s per executor) processes 21 GB in approximately 21 GB / (20 nodes × 0.5 GB/s effective) ≈ 2 minutes for data read. Deduplication (sort + group by click_id) over 21 GB with Spark's sort-merge join takes ~3 minutes. Total job time: ~8–10 minutes. This completes well within our 30-minute SLA. The job runs on an EMR cluster that auto-terminates after completion (cost optimization).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Mitigation | Recovery Time |
|---|---|---|---|---|
| Click Ingest Service pod crash | Pod dies mid-request | Kubernetes liveness probe | K8s restarts pod; load balancer routes to healthy pods | < 30s |
| Redis (dedup) unavailable | Connection timeout | Circuit breaker opens after 5 failures in 10s | Fail open: accept click, bypass dedup; batch layer will dedup | Automatic on Redis recovery |
| Kafka broker failure | Leader election in progress | Consumer lag metric spikes | min.insync.replicas=2; KRaft auto-elects new leader | < 10s |
| Flink job crashes | Exception in window function | Flink job manager health check | Flink restarts from last checkpoint (30s checkpoint interval) | < 60s |
| ClickHouse shard offline | Write failures to shard | Error rate alert | Distributed table routes to replica; write to replica only | < 15s |
| Spark batch job failure | OOM or S3 access error | Airflow task retry | Airflow retries 3× with exponential backoff; PagerDuty if all fail | 30 min (manual intervention) |
| Fraud service offline | Cannot score events | Error rate spike | Fail safe: mark all events as valid (over-trust is safer than blocking valid clicks); retroactive ML batch run | < 30s to failover |
| Full datacenter failure | All services in AZ down | Multi-metric alert | Traffic shifted to secondary region (active-active); Kafka replication cross-AZ | < 5 min |

### Failover

- **Active-Active Multi-Region**: Ingest API is deployed in 3 regions (US-East, EU-West, AP-South). GeoDNS routes click traffic to nearest region. Each region has its own Kafka cluster. A cross-region Kafka replicator (MirrorMaker 2) replicates `valid_clicks` to a central aggregation cluster for unified billing. If a region fails, GeoDNS health checks reroute within 30 seconds (TTL=30s on DNS records).
- **Redis Sentinel**: For non-cluster deployments, Redis Sentinel provides automatic failover. Cluster mode with cross-AZ replicas provides built-in failover.

### Retries & Idempotency

- **Client SDK retry policy**: Exponential backoff with jitter: initial=100ms, max=30s, multiplier=2, jitter=±20%. Max 5 retries. Each retry carries the same `click_id` and `Idempotency-Key` header. Dedup layer handles the retry identically to the first attempt.
- **Kafka producer**: `enable.idempotence=true` (exactly-once per producer epoch), `acks=all`, `retries=MAX_INT`. The producer library sequences messages within a partition and dedups retries at the broker level.
- **Flink checkpointing**: 30-second checkpoint interval to RocksDB state backend, stored in S3. On crash recovery, Flink reprocesses from the last checkpoint offset in Kafka. Window state is restored. This provides exactly-once processing semantics end-to-end.
- **ClickHouse writes (Flink)**: Uses `ReplacingMergeTree` with `(ad_id, window_start, publisher_id)` as ordering key. Re-inserting the same aggregate row is idempotent — the engine keeps the latest version.
- **Spark batch job**: Idempotent by design — reads from immutable S3 files, writes to `ReplacingMergeTree`. Re-running the same hour's job replaces old results.

### Circuit Breaker

```python
# Circuit breaker around Redis dedup calls
class RedisCircuitBreaker:
    FAILURE_THRESHOLD = 5       # failures to open
    HALF_OPEN_TIMEOUT = 30      # seconds in open state before half-open
    SUCCESS_THRESHOLD = 2       # successes to close from half-open

    def __init__(self):
        self.state = "CLOSED"
        self.failure_count = 0
        self.success_count = 0
        self.last_failure_time = 0

    def call(self, func, *args, fallback=None):
        if self.state == "OPEN":
            if time.time() - self.last_failure_time > self.HALF_OPEN_TIMEOUT:
                self.state = "HALF_OPEN"
                self.success_count = 0
            else:
                return fallback() if fallback else None
        
        try:
            result = func(*args)
            if self.state == "HALF_OPEN":
                self.success_count += 1
                if self.success_count >= self.SUCCESS_THRESHOLD:
                    self.state = "CLOSED"
                    self.failure_count = 0
            return result
        except Exception as e:
            self.failure_count += 1
            self.last_failure_time = time.time()
            if self.failure_count >= self.FAILURE_THRESHOLD:
                self.state = "OPEN"
                logger.error(f"Circuit OPEN: Redis failures. Failing open for dedup.")
            raise
```

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Interpretation |
|---|---|---|---|
| `clicks.ingested.rate` | Counter | < 5K/s (dead man) or > 150K/s (overload) | Traffic baseline |
| `clicks.duplicate_rate` | Gauge | > 1% | Retry storm or attacker replaying events |
| `clicks.fraud_rate` | Gauge | > 15% (sudden spike) | Click farm attack in progress |
| `kafka.consumer_lag.flink` | Gauge | > 100K messages | Flink falling behind; real-time data becomes stale |
| `kafka.consumer_lag.fraud` | Gauge | > 50K messages | Fraud service slow; unscored clicks building up |
| `ingest.p99_latency_ms` | Histogram | > 100 ms | Redis slow or ingest pods overloaded |
| `dedup.redis.error_rate` | Counter | > 0.1% of requests | Redis instability; circuit breaker may open |
| `flink.checkpoint.duration_ms` | Gauge | > 30,000 ms | State too large or S3 slow; risk of stale checkpoints |
| `clickhouse.query.p95_latency_ms` | Histogram | > 5,000 ms | OLAP table needs optimizing or shards imbalanced |
| `billing.batch.completion_seconds` | Gauge | > 1800 s (30 min) | Batch job behind; billing SLA at risk |
| `billing.streaming_delta_pct` | Gauge | > 2% | Significant discrepancy between real-time and batch |
| `fraud.ml.queue_depth` | Gauge | > 5,000 | ML scoring service falling behind |

### Distributed Tracing

Every click event carries a `trace_id` (UUID) from the moment it enters the API gateway. This trace_id propagates through:
- HTTP request headers (`X-Trace-ID`)
- Kafka message headers (`trace_id`)
- Flink event processing logs
- ClickHouse inserted rows (`trace_id` column, retained 7 days for debugging)

OpenTelemetry SDK is embedded in all services. Traces are exported to Jaeger (or Tempo in a Grafana stack). A single trace shows the full lifecycle: API gateway receive → Redis dedup check → Kafka publish → Fraud service score → Flink window assignment → ClickHouse write.

This enables debugging scenarios like "Why was click_id X not counted in ad campaign Y's hourly report?" — the trace shows exactly which step rejected or dropped it.

### Logging

- **Structured JSON logs**: All services emit JSON-formatted logs with `click_id`, `ad_id`, `publisher_id`, `trace_id`, `event_type`, `result`, `latency_ms`.
- **Log levels**: INFO for accepted events (sampled at 0.1% to avoid log volume explosion at 100K/s), WARN for duplicates, ERROR for fraud signals and system errors.
- **Log aggregation**: Fluentd sidecar in each pod ships logs to Elasticsearch (ELK stack). Kibana dashboards for operational monitoring. 7-day hot retention in Elasticsearch, 90-day cold in S3.
- **Audit log**: Every billing-relevant event (fraud flag, dedup discard, batch job completion) is written to an immutable audit log in S3 with object-lock enabled (WORM). Legal retention: 7 years.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Alternative Considered | Reason for Choice | Accepted Trade-off |
|---|---|---|---|---|
| Deduplication | Two-level Bloom + exact Redis SET | Database unique index | 50× higher throughput, O(1) check | 0.01% false positive drop rate |
| Aggregation architecture | Lambda (streaming + batch) | Kappa (streaming only) | Batch layer enables retroactive fraud adjustments and dispute resolution | Higher operational complexity (two codepaths) |
| Message bus | Apache Kafka | AWS Kinesis | Configurable partitioning (64 parts), longer retention (7 days), exactly-once | Self-managed operational overhead |
| OLAP store | ClickHouse | Apache Druid | Simpler ops, SummingMergeTree for idempotent aggregation, lower cost | Less mature real-time ingestion than Druid |
| Real-time state | Apache Flink | Spark Structured Streaming | True stream processing with event-time semantics, better late-data handling | Steeper learning curve than Spark |
| Fraud: rule-based vs ML | Hybrid (rules + async ML) | ML only or rules only | Rules give deterministic low-latency; ML catches sophisticated patterns | ML scores arrive 200 ms after event (async) |
| ClickHouse sharding | By advertiser_id | By time | Query isolation per advertiser, no cross-shard scatter for auth-scoped queries | Hot shard risk for large advertisers |
| Redis dedup: fail behavior | Fail open (accept clicks) | Fail closed (reject clicks) | Dropping valid clicks is financially and legally worse than occasional dup | Batch layer must catch the duplication |
| Late events | Side output + daily reconciliation | Drop events > 5 min late | Mobile offline clicks are real clicks; billing accuracy requires counting them | Complexity of late data pipeline |
| CDN for ingest | CDN edge + async origin forward | Direct-to-origin only | CDN edge reduces user-visible latency (1×1 GIF response < 10 ms) | Adds indirection; click might be lost if CDN→origin link fails (< 0.001%) |

---

## 11. Follow-up Interview Questions

Q1: How would you design the system differently if you needed sub-second aggregation (< 1 second staleness) for real-time bidding decisions rather than dashboard display?
A: Sub-second aggregation requires abandoning tumbling windows in favor of continuous per-event counter updates. Replace Flink tumbling windows with Redis HINCRBY operations directly in the ingest service path: `HINCRBY ad_metrics:{ad_id}:{minute_bucket} click_count 1`. This is an O(1) write with < 1ms latency. The tradeoff is losing the exactly-once guarantee (HINCRBY is at-least-once, and Redis counters can drift on failover). For RTB, approximate counts are acceptable. For Flink, we'd use the `CEP` (Complex Event Processing) library for sub-window pattern detection.

Q2: Your billing batch job runs hourly. An advertiser is running a time-limited flash sale and needs billing data in real-time to control spend. How do you handle budget capping?
A: Budget capping is a distinct problem from billing aggregation. It requires a budget control loop: real-time spend counter (Redis INCR, updated by Flink) vs. budget limit. When the counter crosses the limit, a kill signal is sent to the ad serving system to stop showing the ad. This is separate from the billing count. The real-time counter has the ±0.5% accuracy of the streaming layer, which is sufficient for budget control (the batch reconciliation at hour-end is used for actual billing, not capping). We add a safety margin: if real-time spend reaches 98% of budget, we pause the campaign — the 2% buffer absorbs the streaming-to-billing discrepancy.

Q3: How would you handle GDPR right-to-erasure requests for click data?
A: GDPR erasure is complex for an analytics pipeline because data is spread across: Kafka (raw events), S3 Parquet (raw archive), ClickHouse (aggregated), Redis (active dedup keys), and audit logs. Our approach: (1) Kafka: events in retention window are overwritten using Kafka's `kafka-delete-records` command for the affected partition offsets. (2) S3 Parquet: we run a Spark job that reads the affected files, removes records matching the user_id, and rewrites the Parquet files (copy-on-write). (3) ClickHouse: aggregated rows don't contain PII (user_id is stripped before aggregation, only HLL sketches remain) — no action needed. (4) Redis: TTL keys expire naturally within 24 hours; if erasure must be immediate, we DEL the specific keys. (5) Audit logs with WORM: legal exemption typically applies for billing audit trails. We document this exemption in the privacy policy.

Q4: How do you test the deduplication system to verify the 0.01% false positive rate in production?
A: We maintain a shadow deduplication test harness: a synthetic click generator sends 1M known-unique click_ids per day through the full dedup stack and verifies zero rejections (false positive check). Separately, it replays 100K known-duplicate click_ids and verifies 100% rejection (false negative check). Results are published as metrics. In production, we cross-reference the Bloom-rejected click log with the exact-match SET — any click_id that's in the rejected log but NOT in the exact SET was a false positive. This happens at most 0.01% × 100M = 10,000 times/day; we sample 1% of these for investigation.

Q5: The system uses event-time for Flink windows but server-received-time for Spark billing. What happens if a client sends events with deliberately wrong timestamps (clock skew attacks)?
A: Clock skew manipulation is a real attack vector. Defenses: (1) We enforce a timestamp validity window: `received_at_ms - 300,000 <= timestamp_ms <= received_at_ms + 60,000` (5 min past, 1 min future tolerance). Events outside this window are rejected at the ingest API with a 400 error. (2) Billing batch uses `received_at_ms` (server-controlled, immutable) as the billing attribution time, not the client's `timestamp_ms`. This eliminates all clock skew attacks for billing purposes. The `timestamp_ms` is used only for event-time windowing in Flink (dashboard) — inaccuracy there only affects display, not billing.

Q6: You mentioned ClickHouse's SummingMergeTree for aggregation. What happens if Flink and Spark both write conflicting counts (Flink says 4820 clicks, Spark says 4781 after full dedup) for the same window?
A: These write to separate tables by design. Flink writes to `agg_clicks_1min` (SummingMergeTree, real-time serving). Spark writes to `agg_clicks_billing` (ReplacingMergeTree, billing source of truth). Dashboard queries that need real-time use `agg_clicks_1min`; billing uses `agg_clicks_billing`. There is no merge conflict. The UI signals which table a result came from. For the "serving table merge" concern — SummingMergeTree merging is eventually consistent and queries use `sumMerge` aggregator in ClickHouse's `AggregatingMergeTree` context, so partial rows are correctly summed during query execution before background merge completes.

Q7: How would you change the architecture to support guaranteed exactly-once billing for a high-stakes CPC advertiser paying $50 per click?
A: For high-stakes advertisers, we switch from the streaming "estimate" layer entirely and rely only on the batch billing table. The real-time dashboard shows streaming estimates with a clear disclaimer. For billing, we add a cryptographic proof layer: each click event is signed by the ingest service using HMAC-SHA256 (publisher_api_key as the secret). The batch job verifies the signature before counting the click. This prevents retroactive injection of fraudulent clicks into S3. Additionally, we implement a two-party ledger: both the advertiser's system and our system independently count clicks using the same SDK, and we reconcile the counts daily. Discrepancies > 0.1% trigger a joint investigation.

Q8: Your architecture writes Parquet to S3 via Kafka Connect. What's the file size and write frequency strategy, and why?
A: Small files in S3 are expensive (per-request pricing) and slow to read (high overhead per file in Spark). Large files mean high latency before data is available for batch processing. We configure Kafka Connect's S3 Sink with `flush.size=1000000` (1M records per file) and `rotate.interval.ms=300000` (5-minute rotation, whichever comes first). At 35K events/s peak, 1M records takes ~29 seconds — so the 5-minute timer is the active constraint during normal load. Result: ~12 files/hour/partition × 64 partitions = 768 files/hour, each ~10–25 MB. Spark reads these efficiently using partition pruning (`YYYY/MM/DD/HH/` S3 prefixes). Total files per day: 768 × 24 = 18,432 — well within S3's LIST performance envelope.

Q9: How do you monitor for data pipeline freshness? How would you know if the Flink pipeline stopped processing for 10 minutes without user impact?
A: We use a "canary click" pattern: a synthetic click is injected every 60 seconds from an internal publisher account with a known click_id. A separate monitoring service queries the real-time aggregation API every 60 seconds for this publisher's click count. If the count doesn't increment within 90 seconds of the synthetic click insertion, an alert fires: "Flink pipeline freshness breach — last synthetic click not seen in aggregates." This catches silent failures that don't produce errors (e.g., Flink watermark stuck, checkpoint stalling). We also monitor `kafka.consumer_lag.flink` — a monotonically increasing lag is the primary signal.

Q10: How would you design the API tier to support 10,000 dashboard QPS with < 1s p95 latency on 1-hour time window queries?
A: The query path for 1-hour windows (most common dashboard query) is: check Redis cache first (key = `agg:{advertiser_id}:{ad_id}:{hour_start}`, TTL=60s). Cache hit rate for popular ads is ~95%. Cache miss hits ClickHouse: `SELECT sum(click_count) FROM agg_clicks_1min WHERE advertiser_id=X AND window_start BETWEEN T1 AND T2` — this scans 60 rows per ad (1 per minute), taking ~5 ms on a warm ClickHouse instance. Query service uses a connection pool of 100 ClickHouse connections, serving 5K QPS per ClickHouse node (5 ms × 5000 = 25 seconds of query time spread across 100 connections = 250 queries in flight; each takes 5 ms → 50K QPS theoretical). In practice, 3 ClickHouse nodes handle 10K QPS comfortably with mark cache warm.

Q11: What's your strategy for handling timezone-aware aggregations? An advertiser in Tokyo wants daily totals in JST, but all data is stored in UTC.
A: All raw data and aggregated data is stored in UTC. Timezone conversion is a query-time operation, not a storage-time operation. The query service accepts a `timezone` parameter and applies it in the ClickHouse query using `toTimeZone(window_start, 'Asia/Tokyo')`. For daily aggregations spanning a UTC midnight but not a JST midnight, we aggregate over the corresponding UTC range: JST 2026-04-09 00:00 = UTC 2026-04-08 15:00. The query service handles this conversion before passing timestamps to ClickHouse. We never store timezone-converted timestamps — doing so would require storing separate aggregations per timezone (hundreds of millions of rows).

Q12: How do you handle publisher fraud (a publisher inflating their click count to earn more from a revenue-share model)?
A: Publisher fraud is different from advertiser click fraud — the incentives are opposite. Signals specific to publisher fraud: (1) click-to-impression ratio > 100% (impossible — more clicks than ad was displayed; requires cross-referencing with impression tracking system), (2) conversion rate = 0% for high-CPC keywords (publisher clicks are not converting to sales), (3) geographic concentration — 80% of clicks from a single ISP, (4) session depth = 0 (click lands on page, immediately bounces). We calculate these publisher-level fraud scores in a daily Spark job and maintain a publisher trust score (`publisher_trust_level` in the publisher registry). Publishers with trust < 0.5 have all their clicks ML-scored before counting toward billing.

Q13: How would you implement real-time alerting when a campaign's CTR drops 30% in 5 minutes?
A: This is a stream anomaly detection problem. In Flink, alongside the aggregation job, we run a CEP (Complex Event Processing) pattern:

```java
// Alert if CTR drops 30% compared to 5-min rolling baseline
Pattern<ClickAggregate, ?> ctrDropPattern = Pattern
    .<ClickAggregate>begin("baseline")
    .where(new SimpleCondition<>() {
        public boolean filter(ClickAggregate agg) { return agg.getCTR() > 0; }
    })
    .followedBy("drop")
    .where(new IterativeCondition<>() {
        public boolean filter(ClickAggregate current, Context<ClickAggregate> ctx) {
            ClickAggregate baseline = ctx.getEventsForPattern("baseline").iterator().next();
            return current.getCTR() < baseline.getCTR() * 0.7;  // 30% drop
        }
    })
    .within(Time.minutes(5));
```

On pattern match, an alert event is emitted to a `campaign_alerts` Kafka topic, consumed by the notification service, which sends email/SMS/webhook to the advertiser.

Q14: The system stores 90 days of raw events on S3. How do you enforce data residency requirements (e.g., EU clicks must stay in EU)?
A: Data residency is enforced at the regional boundary. EU ingest pods write to EU Kafka clusters, which are replicated to EU S3 buckets in eu-west-1 only. The EU ClickHouse cluster is also in the EU region. Cross-region Kafka MirrorMaker replication carries only aggregated data (no user PII), not raw events. The routing decision is made at the CDN/DNS layer: requests from EU IPs are routed to the EU ingest endpoint, which has an IAM policy preventing writes to non-EU S3 buckets. We audit this quarterly and before any infrastructure change.

Q15: How would you support A/B testing of fraud detection algorithms without impacting billing accuracy?
A: We use a shadow mode approach: the current production fraud algorithm scores events and routes them to `valid_clicks` / `invalid_clicks` as normal. A new candidate algorithm runs in parallel on the same event stream, writing scores to `shadow_valid_clicks` / `shadow_invalid_clicks` topics with no impact on billing. After 7 days, we compare: (1) False negative rate (fraud that production caught but shadow missed, and vice versa), (2) False positive rate (legitimate clicks that shadow incorrectly flagged), (3) Correlation with post-hoc advertiser dispute rates. If shadow outperforms on all metrics, we promote it to production in a canary rollout (5% of traffic → 50% → 100%), monitoring billing-vs-shadow discrepancy throughout.

---

## 12. References & Further Reading

1. **"Counting at Scale"** — Alex Petrov, *Database Internals: A Deep Dive into How Distributed Data Systems Work*, O'Reilly Media, 2019. Chapter 14 covers probabilistic data structures including Bloom filters and HyperLogLog at scale.

2. **Kafka Documentation: Exactly-Once Semantics** — Apache Software Foundation. https://kafka.apache.org/documentation/#semantics — Describes idempotent producers, transactions, and exactly-once delivery guarantees.

3. **ClickHouse MergeTree Documentation** — ClickHouse Inc. https://clickhouse.com/docs/en/engines/table-engines/mergetree-family/summingmergetree — SummingMergeTree engine reference, including partial aggregate merging behavior.

4. **"The Lambda Architecture"** — Nathan Marz & James Warren, *Big Data: Principles and Best Practices of Scalable Realtime Data Systems*, Manning Publications, 2015. Original exposition of the batch/speed/serving layer model.

5. **Apache Flink: Event Time and Watermarks** — Apache Software Foundation. https://flink.apache.org/docs/stable/concepts/time.html — Official documentation on event time processing, watermarks, and late data handling in Flink.

6. **"Click Fraud in Online Advertising: A Literature Review"** — Haddadi, H. (2010). *arXiv:1009.2105*. Academic survey of click fraud detection techniques and their limitations.

7. **RedisBloom Documentation** — Redis Ltd. https://redis.io/docs/data-types/probabilistic/bloom-filter/ — Bloom filter commands, sizing parameters, and persistence behavior.

8. **"Real-time Analytics at Uber"** — Uber Engineering Blog. https://www.uber.com/en-US/blog/pinot/ — Describes Apache Pinot for real-time analytics at Uber scale; highly relevant to this problem domain.

9. **Invalid Traffic (IVT) Standards** — Media Rating Council (MRC). *MRC Invalid Traffic Detection and Filtration Guidelines*, 2021. Industry-standard definitions and thresholds for invalid click traffic in digital advertising.

10. **"Building Reliable Reprocessing and Dead Letter Queues with Apache Kafka"** — Confluent Engineering Blog. https://www.confluent.io/blog/kafka-connect-deep-dive-error-handling-dead-letter-queues/ — Covers error handling patterns relevant to the late-data side output approach.
