# System Design: Metrics & Logging System (Datadog-like)

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Metric Types**: Ingest and store all standard metric types: Counter (monotonically increasing), Gauge (point-in-time value), Histogram (distribution of values with configurable buckets), Summary (quantile estimates at client side), and Timer (specialized histogram for durations).
2. **Metric Submission**: Accept metrics via multiple protocols: StatsD UDP, HTTP push API (Datadog-compatible `/api/v1/series`), and a pull-based scrape endpoint compatible with Prometheus `/metrics` format.
3. **Time-Series Storage**: Store metric samples at their original resolution for 15 days; automatically downsample to 1-minute aggregates for 90 days and 1-hour aggregates for 2 years.
4. **Querying**: Support time-range queries with arbitrary tag filtering, group-by, and aggregation functions (sum, avg, max, min, p50, p95, p99) over any stored time window.
5. **Dashboards**: Allow users to build dashboards with multiple widgets (timeseries charts, single-value, heatmaps) each backed by a metric query.
6. **Alerting**: Create threshold-based and anomaly-based alerts on any metric query. Notify via PagerDuty, Slack, email, or webhook. Alert evaluation frequency: 1 minute minimum.
7. **Log Ingestion**: Accept structured log lines (JSON) and unstructured logs with parsed fields. Index logs for full-text search and field-value queries.
8. **Log Correlation**: Correlate log lines with traces and metrics via shared `trace_id` and `service` tags.
9. **Distributed Tracing**: Ingest OpenTelemetry spans, store in a trace store, and render flame graphs showing request latency breakdown by service.

### Non-Functional Requirements

1. **Ingest Throughput**: Handle 1 million metric data points per second across all customers.
2. **Log Throughput**: Handle 500 MB/s of log data ingestion.
3. **Query Latency**: Dashboard queries over 1-hour windows return in < 1 second; 30-day queries return in < 10 seconds.
4. **Durability**: No metric or log data loss. Data written is guaranteed to be queryable.
5. **Cardinality**: Support up to 10 million unique time series (distinct metric_name + tag_set combinations) per day across all customers.
6. **Availability**: 99.99% uptime for ingest; 99.9% uptime for query (short query outages acceptable).
7. **Multi-Tenancy**: Data from different customers is logically isolated; a noisy neighbor's high-cardinality series should not degrade others.
8. **Retention**: Raw metrics: 15 days; rolled-up metrics: 2 years; logs: configurable per customer (7 days free tier, up to 1 year paid).

### Out of Scope

- APM (Application Performance Management) code profiling (CPU flame graphs from production code sampling).
- Synthetic monitoring (uptime checks from external probes — separate product).
- Security monitoring / SIEM functionality.
- Network flow analysis (NetFlow/sFlow processing).
- On-premise deployment of the metrics system itself.

---

## 2. Users & Scale

### User Types

| Actor | Description |
|---|---|
| Service/Application | Emits metrics via agent, SDK, or StatsD UDP |
| Agent (Datadog Agent / FluentBit) | Local daemon that aggregates and forwards metrics and logs |
| Engineer / SRE | Queries dashboards, investigates alerts |
| Alert Engine | Internal service evaluating alert conditions every minute |
| Anomaly Detection | ML service computing baselines for anomaly detection |
| Billing System | Internal service computing usage per customer |

### Traffic Estimates

**Assumptions**:
- 10,000 customer organizations.
- Average 1,000 host-equivalent agents per large customer; average 50 per small customer; effective average = 200 agents/customer.
- Each agent emits 500 unique metric series at 10-second intervals = 50 data points/agent/second.
- Average customer emits: 200 agents × 50 data points/s = 10,000 data points/s per customer.
- Total: 10,000 customers × 10,000 data points/s = **100M data points/s** ... this exceeds our NFR.
- **Correction**: average agent emits 50 unique series at 10-second intervals = 5 data points/s per agent. Total: 10,000 × 200 × 5 = 10M data points/s. Design to 1M with headroom = typical active burst, not sustained average.

| Metric | Calculation | Result |
|---|---|---|
| Sustained ingest rate | 10,000 customers × 200 agents × 5 dp/s | 10M dp/s peak |
| Design headroom (burst) | Most customers are inactive at night; busy hours: top 20% customers active simultaneously | **1M dp/s sustained design target** |
| Average data point size | metric_name(50B) + tags(150B) + timestamp(8B) + value(8B) = 216 B; with framing ~250 B | ~250 B/dp |
| Ingest bandwidth | 1M dp/s × 250 B | **250 MB/s = 2 Gbps** |
| Daily data points | 1M/s × 86,400 s | 86.4B dp/day |
| Raw storage (15 days) | 86.4B × 10 B/dp (compressed TSDB format) | **1.3 TB/day × 15 = 19.5 TB** |
| 1-min rollup storage (90 days) | 86.4B dp/day / (600 raw dp per 10-min window) × 200B = 28.8GB/day | 28.8 GB/day × 90 = 2.6 TB |
| Log ingest | 500 MB/s × 86,400 s | 43 TB raw logs/day |
| Log storage (compressed, 7-day default) | 43 TB × 0.1 (10:1 compression) | ~4.3 TB/day × 7 = 30 TB |
| Unique time series | 1M series/customer × 10K active customers / 10K (most inactive) | ~10M active series |
| Alert evaluations/min | 10,000 customers × avg 100 alerts = 1M alerts × 1/min | 1M evaluations/min = 16,667/s |

### Latency Requirements

| Operation | Target |
|---|---|
| Agent metric submission (UDP) | Fire-and-forget (no ack) |
| Agent metric submission (HTTP) | < 100 ms ack (p99) |
| Alert evaluation latency from metric event | < 90 seconds (metric emitted → alert fires) |
| Dashboard query (1-hour window, 1 series) | < 500 ms (p95) |
| Dashboard query (30-day window, 10 series) | < 10 s (p95) |
| Log search (full-text, 1-hour window) | < 5 s (p95) |
| Trace flame graph render | < 2 s (p95) |

### Storage Estimates

| Tier | Content | Size/day | Retention | Total |
|---|---|---|---|---|
| Raw TSDB (hot) | 10-second resolution samples | ~1.3 TB | 15 days | 19.5 TB |
| 1-min rollup TSDB (warm) | Per-minute aggregates | ~28.8 GB | 90 days | 2.6 TB |
| 1-hour rollup TSDB (cold) | Per-hour aggregates | ~500 MB | 2 years | 365 GB |
| Log index (Elasticsearch) | Tokenized log fields | ~4 TB | 7–30 days | 28–120 TB |
| Log archive (S3) | Compressed raw logs | ~4.3 TB/day | 1 year | 1.57 PB |
| Trace store (Cassandra) | Span data | ~500 GB/day | 7 days | 3.5 TB |
| Metric metadata (Cassandra) | Metric names, tag keys/values | ~10 GB | Permanent | ~10 GB |

### Bandwidth Estimates

| Flow | Rate |
|---|---|
| Ingest (agents → collectors) | 250 MB/s = 2 Gbps |
| Collector → Kafka | 250 MB/s + overhead |
| Kafka → TSDB writers | 300 MB/s (3 consumer groups) |
| Kafka → Log indexers | 500 MB/s |
| Dashboard query responses | 10K QPS × avg 20 KB = 200 MB/s |

---

## 3. High-Level Architecture

```
                ┌───────────────────────────────────────────────────────────────┐
                │                     AGENTS / SDKs                            │
                │                                                               │
                │  ┌───────────────┐  ┌───────────────┐  ┌──────────────────┐  │
                │  │ Datadog Agent │  │ StatsD Client │  │ Prom Client      │  │
                │  │ (host metrics)│  │ (app metrics) │  │ (scrape endpoint)│  │
                │  └───────┬───────┘  └───────┬───────┘  └────────┬─────────┘  │
                └──────────┼──────────────────┼───────────────────┼────────────┘
                           │ HTTP/gRPC         │ UDP StatsD        │ HTTP scrape
                           ▼                  ▼                   ▼
                ┌───────────────────────────────────────────────────────────────┐
                │               COLLECTOR TIER (Stateless)                     │
                │  ┌──────────────────────────────────────────────────────┐    │
                │  │  Metric Collectors (×N pods)                         │    │
                │  │  - Protocol normalization (StatsD → internal format) │    │
                │  │  - Authentication (API key → customer_id)            │    │
                │  │  - Rate limiting per customer                        │    │
                │  │  - Schema validation                                 │    │
                │  │  - Publish to Kafka: metrics_raw                     │    │
                │  └──────────────────────────────────────────────────────┘    │
                │  ┌──────────────────────────────────────────────────────┐    │
                │  │  Log Collectors (×N pods)                            │    │
                │  │  - Log parsing (grok patterns, JSON extraction)      │    │
                │  │  - Publish to Kafka: logs_raw                        │    │
                │  └──────────────────────────────────────────────────────┘    │
                └───────────────────────────────────────────────────────────────┘
                                          │
                        ┌─────────────────▼──────────────────┐
                        │        Apache Kafka                 │
                        │  Topics:                            │
                        │    metrics_raw (128 partitions)     │
                        │    logs_raw    (256 partitions)     │
                        │    spans_raw   (64 partitions)      │
                        └──────────┬─────────────┬────────────┘
                                   │             │
                    ┌──────────────▼──┐      ┌───▼──────────────────┐
                    │ TSDB Write Path │      │  Log Write Path       │
                    │                 │      │                       │
                    │ ┌─────────────┐ │      │ ┌──────────────────┐ │
                    │ │ Pre-Agg     │ │      │ │ Log Parser /     │ │
                    │ │ Service     │ │      │ │ Enricher         │ │
                    │ │ (Flink)     │ │      │ └────────┬─────────┘ │
                    │ └──────┬──────┘ │      │          │           │
                    │        │        │      │ ┌────────▼─────────┐ │
                    │ ┌──────▼──────┐ │      │ │ Elasticsearch    │ │
                    │ │ TSDB Writer │ │      │ │ (log index)      │ │
                    │ │ (Victoria  │ │      │ └──────────────────┘ │
                    │ │  Metrics /  │ │      │ ┌──────────────────┐ │
                    │ │  InfluxDB)  │ │      │ │ S3 (log archive) │ │
                    │ └──────┬──────┘ │      │ └──────────────────┘ │
                    └────────┼────────┘      └───────────────────────┘
                             │
                    ┌────────▼────────────────────────────────────────┐
                    │            STORAGE TIER                         │
                    │  ┌──────────────────────────────────────────┐   │
                    │  │  Time-Series DB Cluster                  │   │
                    │  │  (VictoriaMetrics / InfluxDB IOx)        │   │
                    │  │  - Raw: 15-day retention                 │   │
                    │  │  - 1-min rollup: 90-day retention        │   │
                    │  │  - 1-hr rollup: 2-year retention         │   │
                    │  └──────────────────────────────────────────┘   │
                    │  ┌──────────────────────────────────────────┐   │
                    │  │  Metric Metadata Store (Cassandra)       │   │
                    │  │  - metric_name → tag_keys mapping        │   │
                    │  │  - tag_value cardinality index           │   │
                    │  └──────────────────────────────────────────┘   │
                    └─────────────────────────────────────────────────┘
                             │
                    ┌────────▼────────────────────────────────────────┐
                    │            QUERY TIER                           │
                    │  ┌────────────────────────────────────────┐    │
                    │  │  Query Service (MetricsQL / PromQL)    │    │
                    │  │  - Query parsing, validation            │    │
                    │  │  - Time-range routing (raw vs. rollup) │    │
                    │  │  - Fan-out across TSDB shards           │    │
                    │  │  - Result caching (Redis)               │    │
                    │  └────────────────────────────────────────┘    │
                    │  ┌────────────────────────────────────────┐    │
                    │  │  Alert Engine                          │    │
                    │  │  - Scheduled 1-min evaluations         │    │
                    │  │  - State machine (OK/WARN/CRIT)        │    │
                    │  │  - Notification fanout                 │    │
                    │  └────────────────────────────────────────┘    │
                    └─────────────────────────────────────────────────┘
```

**Component Roles**:

- **Metric Collectors**: Stateless pods handling protocol normalization. Critically: UDP StatsD packets are received and immediately acknowledged (fire-and-forget). HTTP batch submissions are acknowledged after Kafka `produce()` call returns (not after TSDB write). This decouples ingest latency from storage latency.
- **Pre-Aggregation Service (Flink)**: Performs server-side pre-aggregation to combat cardinality explosion. For counters, sums values over 10-second windows per `(customer_id, metric_name, tag_set)`. For histograms, merges HdrHistogram buckets. This reduces the number of distinct writes to the TSDB.
- **TSDB Writer**: Consumes aggregated metrics from Kafka and batches writes to VictoriaMetrics. Batching is critical for TSDB performance (merge multiple data points into a single compressed block append).
- **VictoriaMetrics / TSDB Cluster**: Column-oriented time-series database with LZ4 compression. Stores time-series data as compressed blocks sorted by time. Supports MetricsQL (Prometheus-compatible + extensions).
- **Log Collectors**: Parse and structure incoming log lines. Apply grok patterns for common formats (nginx, apache, JSON). Extract fields into indexed terms. Publish to both Elasticsearch (for indexing) and S3 (for archival).
- **Elasticsearch**: Provides full-text search and field-based filtering for logs. Each index covers one day per customer (customer_id-YYYY-MM-DD). ILM (Index Lifecycle Management) auto-rolls over and deletes indices.
- **Query Service**: The read-path hub. Parses MetricsQL/PromQL queries, determines which resolution tier to use (raw for < 15d, 1-min rollup for 15–90d, hourly for > 90d), fans out to TSDB shards, and merges results.
- **Alert Engine**: Evaluates all active alert conditions every 60 seconds. Maintains alert state machines. Routes notifications via PagerDuty SDK, Slack webhooks, etc.
- **Metric Metadata Store (Cassandra)**: Maintains the "index" of what metrics exist: all known metric names, their tag keys, and tag value cardinalities. Used by the UI for autocomplete and by the cardinality explosion detection system.

---

## 4. Data Model

### Entities & Schema

**metric_sample** (internal representation, Kafka message):
```json
{
  "customer_id":  "cust-001",
  "metric_name":  "http.requests.count",
  "metric_type":  "counter",
  "timestamp_ms": 1712649600000,
  "value":        142.0,
  "tags": {
    "host":        "web-01.us-east-1",
    "service":     "api-gateway",
    "env":         "production",
    "status_code": "200",
    "endpoint":    "/v1/users"
  }
}
```

**TSDB storage format** (VictoriaMetrics internal — shown conceptually):

VictoriaMetrics stores data as:
- **MetricID** (8-byte uint64): hash of `(metric_name + sorted_tag_set)` — the time series identifier.
- **Timestamps block**: delta-compressed timestamps (typical compression: 64 values per block, ~1 byte/timestamp).
- **Values block**: XOR-compressed float64 values (Gorilla compression, similar to Prometheus TSDB).
- **Index**: inverted index mapping each tag key/value to the set of MetricIDs that have that tag. Stored in mergeset (LSM-tree variant).

```
Time Series ID: UINT64 = MetricID
  └─ hash(customer_id="cust-001" __name__="http.requests.count" 
          env="production" host="web-01" service="api-gateway" 
          status_code="200" endpoint="/v1/users")

Storage block:
  MetricID: 0x7f3a29b1c4e5d2a0
  timestamps: [1712649600, 1712649610, 1712649620, ...]  // delta-encoded
  values:     [142.0, 145.0, 143.0, ...]                 // XOR-encoded
```

**metric_metadata** (Cassandra):
```sql
CREATE TABLE metric_names (
    customer_id     TEXT,
    metric_name     TEXT,
    metric_type     TEXT,   -- counter|gauge|histogram|summary|timer
    first_seen_at   TIMESTAMP,
    last_seen_at    TIMESTAMP,
    tag_keys        SET<TEXT>,   -- all tag keys ever seen for this metric
    series_count    COUNTER,     -- approximate unique series (from HLL)
    PRIMARY KEY (customer_id, metric_name)
);

CREATE TABLE tag_values (
    customer_id     TEXT,
    tag_key         TEXT,
    tag_value       TEXT,
    metric_names    SET<TEXT>,   -- which metrics use this tag key/value
    PRIMARY KEY ((customer_id, tag_key), tag_value)
);

CREATE TABLE cardinality_tracker (
    customer_id     TEXT,
    date            DATE,
    metric_name     TEXT,
    unique_series   BIGINT,     -- HLL cardinality estimate
    high_cardinality_tags TEXT, -- JSON: {tag_key: cardinality}
    PRIMARY KEY ((customer_id, date), metric_name)
);
```

**alert_definition**:
```sql
CREATE TABLE alert_definition (
    customer_id     UUID,
    alert_id        UUID,
    name            TEXT NOT NULL,
    query           TEXT NOT NULL,      -- MetricsQL/PromQL query string
    condition       TEXT NOT NULL,      -- "> 0.95" or "anomaly:3sigma"
    window_seconds  INT DEFAULT 300,    -- evaluation window
    eval_interval_s INT DEFAULT 60,     -- how often to evaluate
    severity        TEXT,               -- critical|warning|info
    notification_channels JSONB,        -- [{type: "pagerduty", key: "..."}, ...]
    state           TEXT DEFAULT 'OK',  -- OK|WARNING|CRITICAL|NO_DATA
    last_eval_at    TIMESTAMP,
    last_triggered_at TIMESTAMP,
    created_at      TIMESTAMP DEFAULT now(),
    PRIMARY KEY (customer_id, alert_id)
);
```

**log_entry** (Elasticsearch index schema):
```json
{
  "@timestamp":     "2026-04-09T14:32:05.123Z",
  "customer_id":    "cust-001",
  "service":        "api-gateway",
  "host":           "web-01.us-east-1",
  "env":            "production",
  "log_level":      "ERROR",
  "trace_id":       "abc123",
  "span_id":        "def456",
  "message":        "Failed to connect to database: connection timeout",
  "logger_name":    "com.example.DatabasePool",
  "parsed_fields":  {
    "duration_ms": 5000,
    "db_host": "postgres-primary.internal",
    "error_code": "ETIMEDOUT"
  },
  "raw_message":    "2026-04-09T14:32:05 ERROR [api-gateway] Failed to connect..."
}
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| InfluxDB | Time-series storage | Purpose-built for TSDB, SQL-like Flux language, continuous queries for rollups | Enterprise features expensive, cardinality limits at high series count, TSM storage engine has known compaction issues under heavy write load | Considered |
| Prometheus TSDB | Time-series storage | Well-known, PromQL standard, embedded in Thanos/Cortex for scale | Single-node designed, requires Thanos/Cortex for HA which adds significant complexity | Basis for compatible query layer |
| VictoriaMetrics | Time-series storage | 5-10× more efficient than InfluxDB/Prometheus at equal throughput, handles 10M+ cardinality, built-in clustering mode, MetricsQL superset of PromQL | Less mature ecosystem than Prometheus | **Selected for TSDB** — benchmark studies show 5× better ingestion rate and 3× better compression |
| TimescaleDB | Time-series on PostgreSQL | Full SQL, familiar ops, hypertable partitioning | Memory-intensive for high cardinality; TSDB-specific engines outperform on pure time-series workloads | Rejected |
| Cassandra | URL frontier, metadata | Excellent for wide-row time-series with known access patterns | Requires careful pre-modeling; MetricsQL-style ad-hoc queries are complex to implement | **Selected for metric metadata and log index metadata** |
| Elasticsearch | Log search | Industry standard for log full-text search, strong aggregations, Kibana integration | Memory-hungry (JVM heap), complex capacity planning, cardinality in terms aggregations is expensive | **Selected for log indexing** — no better alternative for full-text log search at this scale |
| ClickHouse | Alternative TSDB / log store | Exceptional columnar aggregation, low memory footprint vs. ES | Less optimized for time-series data point lookups; less mature for log full-text search (no tokenizer ecosystem) | Alternative to Elasticsearch for logs; selected ES for richer text search features |

---

## 5. API Design

### Metric Submission API

```
POST /api/v1/series
Authorization: DD-API-KEY: <customer_api_key>
Content-Type: application/json
X-RateLimit-Policy: 500K data points per minute per customer

Request:
{
  "series": [
    {
      "metric": "http.requests.count",
      "type": "count",
      "points": [
        [1712649600, 142],
        [1712649610, 145]
      ],
      "tags": ["service:api-gateway", "env:production", "host:web-01"],
      "interval": 10
    },
    {
      "metric": "http.request.duration",
      "type": "histogram",
      "points": [[1712649600, 0.342]],
      "tags": ["service:api-gateway", "endpoint:/v1/users"],
      "interval": 10
    }
  ]
}

Response 202 Accepted:
{ "status": "ok" }

Response 429 Too Many Requests:
{ "errors": ["Rate limit exceeded: 500K data points per minute"] }
```

### Metric Query API (PromQL-compatible)

```
GET /api/v1/query_range
Authorization: Bearer <user_token>
X-RateLimit-Policy: 100 queries/min per user, 1000/min per customer

Query Parameters:
  query     REQUIRED  string    MetricsQL/PromQL expression
  start     REQUIRED  Unix timestamp or ISO-8601
  end       REQUIRED  Unix timestamp or ISO-8601
  step      REQUIRED  Duration string (e.g., "1m", "5m", "1h")
  customer_id  auto-resolved from auth token

Example:
  GET /api/v1/query_range
    ?query=rate(http.requests.count{service="api-gateway",env="production"}[5m])
    &start=2026-04-09T00:00:00Z
    &end=2026-04-09T01:00:00Z
    &step=1m

Response 200:
{
  "status": "success",
  "data": {
    "resultType": "matrix",
    "result": [
      {
        "metric": {
          "__name__": "http.requests.count",
          "service": "api-gateway",
          "env": "production",
          "host": "web-01"
        },
        "values": [
          [1712649600, "142.5"],
          [1712649660, "145.0"]
        ]
      }
    ]
  },
  "stats": {
    "series_fetched": 12,
    "data_points_read": 720,
    "query_time_ms": 45
  }
}
```

### Log Search API

```
POST /api/v1/logs/search
Authorization: Bearer <user_token>
X-RateLimit-Policy: 20 queries/min per user

Request:
{
  "query": "service:api-gateway log_level:ERROR \"connection timeout\"",
  "from": "2026-04-09T14:00:00Z",
  "to":   "2026-04-09T15:00:00Z",
  "limit": 50,
  "cursor": null,
  "columns": ["@timestamp", "message", "host", "trace_id"],
  "sort": [{"field": "@timestamp", "order": "desc"}]
}

Response 200:
{
  "hits": [
    {
      "@timestamp": "2026-04-09T14:32:05.123Z",
      "message": "Failed to connect to database: connection timeout",
      "host": "web-01.us-east-1",
      "trace_id": "abc123",
      "log_level": "ERROR"
    }
  ],
  "total": 142,
  "cursor": "eyJ0aW1lc3RhbXAiOiIyMDI2LTA0LTA5VDE0OjMxOjQ2WiJ9"
}
```

### Alert CRUD API

```
POST /api/v1/alerts
Authorization: Bearer <user_token>

Request:
{
  "name": "API Error Rate > 5%",
  "query": "sum(rate(http.requests.count{status_code=~'5..'}[5m])) / sum(rate(http.requests.count[5m])) > 0.05",
  "condition": "> 0.05",
  "window_seconds": 300,
  "eval_interval_seconds": 60,
  "severity": "critical",
  "notification_channels": [
    { "type": "pagerduty", "service_key": "xxxx" },
    { "type": "slack", "webhook_url": "https://hooks.slack.com/..." }
  ]
}

Response 201 Created:
{
  "alert_id": "alt-a1b2c3",
  "state": "OK",
  "created_at": "2026-04-09T14:00:00Z"
}
```

---

## 6. Deep Dive: Core Components

### Core Component 1: Time-Series Storage Engine

**Problem it solves**: Time-series data has unique characteristics that general-purpose databases handle poorly: (1) Write patterns are extremely write-heavy (1M inserts/second, mostly recent timestamps). (2) Data access is temporal: most queries touch the last 24 hours, rarely the last 30 days, almost never the last year. (3) Cardinality (unique time series) can explode when tags have high cardinality (e.g., `user_id`, `request_id` as tags — each unique user creates a new series). (4) Values are highly compressible when they don't change much (counters incrementing by 1, CPU gauge near 60%).

**Approach Comparison**:

| Storage Architecture | Compression | Query Latency | Write Throughput | Cardinality Limit | Rollup Support |
|---|---|---|---|---|---|
| Naive row store (Postgres) | Poor (row-major) | Slow (full table scan) | Low | High | Manual | 
| Time-partitioned Cassandra | Moderate (column compression) | Medium (needs range scan) | High | Very high | Manual |
| Prometheus TSDB | Excellent (Gorilla/XOR) | Fast | Medium | ~10M series (memory-bound) | Via recording rules |
| InfluxDB TSM | Good | Fast | High | ~1M series (documented limit) | Continuous queries |
| VictoriaMetrics | Excellent (custom codec) | Fast | Very high (5× Prometheus) | Tens of millions | Built-in retention policies |
| Apache Druid | Excellent (columnar) | Fast on rollup | High | Very high | Built-in roll-up at ingest |

**Selected Approach: VictoriaMetrics**

**Reasoning**: VictoriaMetrics' vmstorage layer uses a custom compression format that achieves:
- Timestamps: ~1 byte per sample (delta encoding, fixed 1-second resolution internally).
- Values: ~4 bytes per sample (XOR encoding of float64, Gorilla algorithm variant).
- Combined: ~5 bytes/sample vs. InfluxDB's ~15 bytes/sample and Prometheus's ~10 bytes/sample.

At 1M samples/second × 5 bytes = **5 MB/s write throughput** to storage — compared to 15 MB/s for InfluxDB, enabling 3× more time series on the same hardware.

**Write Path Implementation (conceptual)**:

```go
// VictoriaMetrics-style write path (illustrative)
type TSDBWriter struct {
    storage *vmstorage.Storage
    metricIDCache *sync.Map  // metric_name+tags → MetricID
    pendingRows []storage.MetricRow
    flushInterval time.Duration
}

type MetricRow struct {
    MetricNameRaw []byte     // pre-marshaled metric name + tags
    Timestamp     int64      // unix milliseconds
    Value         float64
}

func (w *TSDBWriter) AddSample(name string, tags map[string]string, 
                                ts int64, value float64) {
    // 1. Compute MetricID (cache lookup or compute + cache)
    key := buildCacheKey(name, tags)
    metricID, exists := w.metricIDCache.Load(key)
    if !exists {
        // Build MetricName: includes __name__ + sorted tags
        mn := buildMetricName(name, tags)
        metricID = computeMetricID(mn)  // xxHash64 of normalized name
        w.metricIDCache.Store(key, metricID)
        // Async index update (inverted index: tag_key/value → MetricID set)
        go w.updateIndex(metricID, mn)
    }
    
    // 2. Append to pending batch (accumulate for compression efficiency)
    w.pendingRows = append(w.pendingRows, storage.MetricRow{
        MetricNameRaw: buildMetricNameRaw(metricID.(uint64)),
        Timestamp:     ts,
        Value:         value,
    })
    
    // 3. Flush when batch is large enough or flush interval elapsed
    if len(w.pendingRows) >= 10000 {
        w.flush()
    }
}

func (w *TSDBWriter) flush() {
    if len(w.pendingRows) == 0 {
        return
    }
    // Sort by MetricID + Timestamp for optimal compression
    sort.Slice(w.pendingRows, func(i, j int) bool {
        if w.pendingRows[i].MetricNameRaw[0] != w.pendingRows[j].MetricNameRaw[0] {
            return string(w.pendingRows[i].MetricNameRaw) < string(w.pendingRows[j].MetricNameRaw)
        }
        return w.pendingRows[i].Timestamp < w.pendingRows[j].Timestamp
    })
    
    // Write block to storage (appends to in-memory part, later merged to disk)
    w.storage.AddRows(w.pendingRows, precisionBits)
    w.pendingRows = w.pendingRows[:0]
}
```

**Storage Tier Architecture** (Three-Tier Rollup):

```python
# Rollup service (runs as a scheduled Flink job)
class RollupService:
    """
    Continuously reads raw samples and writes pre-aggregated 1-min and 1-hr buckets.
    Runs as a separate VictoriaMetrics cluster to avoid impacting hot storage.
    """
    
    def compute_1min_rollup(self, raw_samples: list) -> list:
        """
        For each unique (metric_name, tags) time series, compute 1-min aggregates.
        Stores as a DIFFERENT metric name to avoid collision with raw data.
        """
        by_series = defaultdict(list)
        for s in raw_samples:
            key = (s['metric_name'], frozenset(s['tags'].items()))
            by_series[key].append((s['timestamp_ms'], s['value']))
        
        rollup_rows = []
        for (name, tags), points in by_series.items():
            # Find 1-minute bucket
            bucket_start = (points[0][0] // 60000) * 60000
            values = [v for _, v in points]
            
            rollup_rows.extend([
                {'metric_name': f"{name}_rollup_1m_sum",  'tags': dict(tags), 
                 'timestamp_ms': bucket_start, 'value': sum(values)},
                {'metric_name': f"{name}_rollup_1m_count",'tags': dict(tags),
                 'timestamp_ms': bucket_start, 'value': len(values)},
                {'metric_name': f"{name}_rollup_1m_max",  'tags': dict(tags),
                 'timestamp_ms': bucket_start, 'value': max(values)},
                {'metric_name': f"{name}_rollup_1m_min",  'tags': dict(tags),
                 'timestamp_ms': bucket_start, 'value': min(values)},
            ])
        return rollup_rows
```

**Interviewer Q&As**:

Q1: How do you handle the "write cliff" — where all agents submit metrics simultaneously at the top of each minute?
A: The write cliff is mitigated by: (1) Agents use jittered submission intervals (Datadog Agent jitters by up to 5 seconds from the minute mark). (2) The Kafka message bus absorbs the burst — even if 1M data points arrive in 1 second, Kafka consumers can drain at sustained rate. (3) VictoriaMetrics accumulates writes in memory parts before flushing to disk — it handles burst writes gracefully because memory is the bottleneck, not disk I/O. (4) We provision collectors for 3× the average ingest rate, so even an instantaneous 3× burst is absorbed without backpressure.

Q2: Prometheus uses a local storage engine; Thanos adds object storage for long-term. Why not just use Thanos?
A: Thanos is architecturally complex: it adds Sidecar (runs next to each Prometheus), Store Gateway (reads from object store), Querier (fan-out query layer), Compactor (downsampling), and Ruler (recording rules). This is 5 additional components to operate vs. VictoriaMetrics' 3 (vminsert, vmstorage, vmselect). VictoriaMetrics natively handles multi-tenancy, long-term storage, and downsampling without additional components. Benchmark: VictoriaMetrics achieves 5–10× better throughput than Prometheus+Thanos on identical hardware (documented in VictoriaMetrics public benchmarks against Thanos and Cortex).

Q3: How do you handle histogram metric types? Histograms have N bucket values per sample.
A: A histogram with N buckets is stored as N separate time series: `http.request.duration_bucket{le="0.1"}`, `http.request.duration_bucket{le="0.5"}`, ..., `http.request.duration_sum`, `http.request.duration_count`. This is the Prometheus convention. At the pre-aggregation layer, before writing to TSDB, we merge histogram samples from the same time window using HdrHistogram merging (add-compatible). The `le` buckets are monotonically increasing, so XOR compression works extremely well for them (adjacent-in-time values are highly correlated). At query time, quantile computation (`histogram_quantile(0.99, ...)`) uses the bucket boundaries to interpolate percentiles.

Q4: What is a "head series" in the TSDB context, and how do you limit memory usage?
A: A "head series" is a time series that is currently actively receiving data — it exists in the in-memory write buffer (the "head block"). VictoriaMetrics and Prometheus both keep recent data in memory for fast appends. Memory usage scales with the number of active series: ~1 KB/series in memory. At 10M active series: 10 GB RAM just for the head. If cardinality explodes (a new tag with millions of unique values), memory fills up and the TSDB either OOMs or starts dropping data. Mitigation: (1) Pre-aggregation at the collector tier — strip high-cardinality tags before writing to TSDB (replace `user_id=12345` with `user_id=__other__` when cardinality exceeds 1000). (2) Per-customer series limit enforced at the collector: reject or aggregate metrics that would push a customer past their quota. (3) Alert on customers approaching their cardinality limit.

Q5: How does the query tier handle time ranges that span multiple storage tiers (raw + rollup)?
A: The Query Service performs time-range stitching. For a 30-day query with step=1h: data from the first 15 days comes from the 1-min rollup tier (summed to 1-hr buckets), data from the last 15 days may come from raw data (for recency) or the rollup tier. The query service splits the time range at tier boundaries:

```python
def route_query(start: datetime, end: datetime, step_s: int) -> list:
    """Split query into sub-ranges based on data availability in each tier."""
    now = datetime.utcnow()
    raw_cutoff = now - timedelta(days=15)
    rollup_1m_cutoff = now - timedelta(days=90)
    
    segments = []
    
    if start >= raw_cutoff:
        # All raw data
        segments.append(('raw', start, end))
    elif start >= rollup_1m_cutoff and end > raw_cutoff:
        # Split: 1-min rollup + raw
        segments.append(('rollup_1m', start, raw_cutoff))
        segments.append(('raw', raw_cutoff, end))
    elif start < rollup_1m_cutoff:
        # Hourly rollup + 1-min rollup + possibly raw
        if end > raw_cutoff:
            segments.append(('rollup_1h', start, rollup_1m_cutoff))
            segments.append(('rollup_1m', rollup_1m_cutoff, raw_cutoff))
            segments.append(('raw', raw_cutoff, end))
        else:
            segments.append(('rollup_1h', start, rollup_1m_cutoff))
            segments.append(('rollup_1m', rollup_1m_cutoff, end))
    
    return segments
```

Results from each tier are merged and returned as a unified time series to the client.

---

### Core Component 2: Cardinality Explosion Prevention

**Problem it solves**: Cardinality explosion is one of the most common production incidents in metrics systems. It occurs when a high-cardinality tag (one with millions of unique values) is applied to a metric, creating millions of unique time series. Examples: using `user_id`, `request_id`, `session_id`, or `IP address` as a metric tag. Each unique value creates a new series in the TSDB head. At 10M unique request_ids/day, a single misconfigured metric creates 10M new series, consuming 10 GB of TSDB head memory and overwhelming the inverted index. This can crash a production monitoring system.

**Approach Comparison**:

| Prevention Method | When Applied | Effectiveness | Impact on Legitimate Use |
|---|---|---|---|
| Client-side education | Before code ships | Low (developers don't know) | None |
| Hard series limit per customer | At ingest | High (stops the blast) | Drops legitimate metrics above limit |
| Cardinality analysis + alerts | Near-real-time detection | Medium (fast warning) | None until action taken |
| Tag value hashing for high-cardinality | At collector | High (limits unique values) | Loses individual tag values |
| Dimension stripping | At collector | High | Loses the dimension entirely |
| Server-side pre-aggregation before TSDB | At collector | Highest | Changes metric semantics |

**Selected Approach: Multi-Layer Defense**

1. **Pre-ingest validation**: At the collector, compute estimated cardinality for the incoming metric using a running HyperLogLog estimator per `(customer_id, metric_name, tag_key)`. If a tag key's cardinality exceeds the limit (configurable, default 10,000 unique values), apply the configured action for that customer: WARN (log and alert), AGGREGATE (replace high-cardinality tag with `_other`), or DROP (drop the metric).

2. **Real-time cardinality monitoring**: A Flink job consumes from `metrics_raw` and maintains per-customer, per-metric HLL sketches. Every 5 minutes, emits cardinality statistics to the `metric_metadata` Cassandra table. A monitoring alert fires when cardinality grows > 50% in 5 minutes (explosive growth signal).

3. **Customer quota enforcement**: Each customer has a maximum active series quota (e.g., 1M series). When a new series would exceed the quota, the collector returns HTTP 429 with a specific error code: `SERIES_QUOTA_EXCEEDED`.

**Cardinality Detection Pseudocode**:

```python
class CardinalityGuard:
    """
    Runs at the collector tier. Prevents high-cardinality tag ingestion.
    """
    DEFAULT_TAG_CARDINALITY_LIMIT = 10_000
    ALERT_THRESHOLD_PCT = 80  # Alert when 80% of limit reached

    def __init__(self, redis, cardinality_db):
        self.redis = redis
        self.db = cardinality_db
        # In-process HLL estimators: {(customer_id, metric_name, tag_key): HLL}
        self._hlls = {}
        self._lock = threading.Lock()

    def check_and_record(self, metric: MetricSample) -> CardinalityCheckResult:
        """
        Check if this metric sample would cause cardinality explosion.
        Returns whether to accept, warn, or drop.
        """
        issues = []
        
        for tag_key, tag_value in metric.tags.items():
            hll_key = (metric.customer_id, metric.metric_name, tag_key)
            
            with self._lock:
                if hll_key not in self._hlls:
                    self._hlls[hll_key] = HyperLogLog(error_rate=0.01)
                hll = self._hlls[hll_key]
                
                # Add this value to HLL estimate
                hll.add(tag_value)
                cardinality_estimate = len(hll)  # HLL cardinality estimate
            
            limit = self._get_limit(metric.customer_id, metric.metric_name, tag_key)
            
            if cardinality_estimate > limit:
                issues.append(CardinalityIssue(
                    tag_key=tag_key,
                    estimated_cardinality=cardinality_estimate,
                    limit=limit,
                    action=self._get_action(metric.customer_id, tag_key)
                ))
            elif cardinality_estimate > limit * (self.ALERT_THRESHOLD_PCT / 100):
                # Approaching limit — send warning to customer
                self._emit_warning(metric.customer_id, metric.metric_name, 
                                   tag_key, cardinality_estimate, limit)
        
        return CardinalityCheckResult(issues=issues)

    def apply_mitigation(self, metric: MetricSample, 
                         issues: list) -> Optional[MetricSample]:
        """
        Apply configured mitigation actions.
        Returns modified metric (or None to drop).
        """
        for issue in issues:
            if issue.action == 'DROP':
                metrics_counter.increment('cardinality.dropped', 
                    customer_id=metric.customer_id, 
                    metric_name=metric.metric_name)
                return None
            elif issue.action == 'AGGREGATE':
                # Replace high-cardinality tag value with aggregate bucket
                # Use consistent hashing to assign to one of 100 buckets
                bucket = xxhash.xxh32(metric.tags[issue.tag_key]).intdigest() % 100
                metric.tags[issue.tag_key] = f"_bucket_{bucket:03d}"
        
        return metric
```

**HyperLogLog Configuration**:
- Error rate: 1% (standard HLL, 2^14 = 16,384 registers, ~12 KB per HLL).
- At 10M unique (customer, metric, tag_key) combinations: 120 GB RAM for all HLLs.
- Mitigation: Only keep HLLs for active time series in the last 5 minutes (LRU eviction of stale HLLs reduces to ~1M active, 12 GB RAM).

**Interviewer Q&As**:

Q1: An engineer accidentally deploys code that adds `request_id` as a metric tag. How fast does cardinality explosion happen and can you contain it?
A: At 1,000 req/s for a single service, unique request_ids grow at 1,000/second. The HLL cardinality estimate reaches 10,000 (default limit) in 10 seconds. Within 10 seconds, our CardinalityGuard detects the issue, sends a warning event to the customer's Slack/email, and begins applying the AGGREGATE mitigation (bucketing the `request_id` tag). The TSDB has already received 10,000 new series by this point — a manageable number (10 MB of head memory). Containment is within 10–15 seconds. Without this guard, at 1,000 req/s, in 24 hours the service would have created 86.4 million new series (86 GB of TSDB head memory) — enough to crash VictoriaMetrics.

Q2: The HLL has a 1% error rate. This means a tag with 9,900 actual unique values might be estimated as 10,000 and incorrectly throttled. Is this acceptable?
A: The 1% error means the threshold of 10,000 can fire as early as 9,900 actual values (1% under-estimate) or as late as 10,100 (1% over-estimate). The under-estimate case (throttling at 9,900) is a false positive — a legitimate use case is throttled too soon. To mitigate: set the hard throttle at 10,000 but the warning alert at 8,000 (80% of limit). This gives engineers a warning with ~2% head room before the hard throttle hits. The customer can request a limit increase (the system stores the limit in the customer configuration table, updatable by support). For billing-sensitive use cases, we can lower the error rate to 0.1% by using 10× more registers (120 KB per HLL, 120 GB total) or switch to a Count-Min Sketch for exact counts up to a threshold.

Q3: How do you handle cardinality monitoring for histograms, which generate N series per unique tag combination?
A: A histogram with N buckets × M unique tag combinations = N×M series. N is fixed (typically 10–30 buckets defined in the histogram). The cardinality explosion comes from M, the unique tag combinations. The HLL tracks cardinality of tag values separately from histogram buckets — we count unique values of each tag key, not unique `(tag_combination, bucket)` series. The effective series count for a histogram is `unique_tag_combinations × N_buckets`. We expose this multiplier in the cardinality dashboard: "This histogram has 500 unique tag combinations × 15 buckets = 7,500 series." The limit is applied to effective series count, not raw tag value cardinality.

---

### Core Component 3: Alert Engine

**Problem it solves**: An alerting system must evaluate up to 1 million alert conditions every 60 seconds. Each evaluation requires a TSDB query. Naive implementation (one query per alert evaluation) at 1M alerts × 1 query/min = 16,667 TSDB queries/second — this would overwhelm the query tier. Alert evaluation must also be stateful (no flapping — an alert shouldn't fire and recover on every minor oscillation), support multiple notification channels with deduplication, and handle the "no data" case (a service stops emitting metrics entirely).

**Approach Comparison**:

| Alert Architecture | Query Pattern | State Management | Scale | Flap Prevention |
|---|---|---|---|---|
| Polling each alert independently | One query/alert/interval | Stateless | Poor (16K QPS) | None |
| Group alerts by metric, batch query | One query per unique metric/window combo | Stateless | Better (fewer TSDB queries) | None |
| Alert groups + shared evaluation | Shared TSDB queries per group | Stateful (state store) | Excellent | Via evaluation history |
| Streaming evaluation (Flink) | Consume from Kafka, no TSDB queries | In Flink state | Best | Window-based |

**Selected Approach: Alert Groups with Shared TSDB Query Batching**

Group alerts that query the same base metric+tags into evaluation groups. Instead of 1M individual queries, we execute ~50K unique query executions (many alerts share the same underlying metric query with different threshold conditions). State is maintained in Redis per alert (current state, last evaluation time, consecutive evaluations in state).

**Alert Engine Pseudocode**:

```python
class AlertEngine:
    EVAL_INTERVAL_SECONDS = 60
    MIN_CONSECUTIVE_EVALS = 2  # Must be in CRITICAL for 2 consecutive evals before firing
    
    def __init__(self, tsdb_client, redis, notification_router):
        self.tsdb = tsdb_client
        self.redis = redis
        self.notifier = notification_router

    def run_evaluation_cycle(self):
        """Called every 60 seconds. Evaluates all active alerts."""
        alerts = self._get_active_alerts()
        
        # Step 1: Group alerts by their base query to batch TSDB requests
        query_groups = self._group_by_query(alerts)
        
        # Step 2: Execute TSDB queries in parallel (thread pool)
        with concurrent.futures.ThreadPoolExecutor(max_workers=200) as executor:
            futures = {
                executor.submit(self._execute_query, group_query, alerts): group_query
                for group_query, alerts in query_groups.items()
            }
            for future in concurrent.futures.as_completed(futures):
                query_result = future.result()
                group_query = futures[future]
                
                # Step 3: Evaluate all alerts sharing this query against the result
                for alert in query_groups[group_query]:
                    self._evaluate_alert(alert, query_result)

    def _evaluate_alert(self, alert: AlertDefinition, 
                        query_result: QueryResult):
        """Evaluate a single alert and transition state if needed."""
        
        # Compute current value from query result
        current_value = query_result.get_value_for_alert(alert)
        
        if current_value is None:
            new_state = AlertState.NO_DATA
        elif self._matches_condition(current_value, alert.condition):
            new_state = AlertState.CRITICAL
        else:
            new_state = AlertState.OK
        
        # Load current state from Redis (stateful evaluation)
        state_key = f"alert:state:{alert.alert_id}"
        state_data = self.redis.hgetall(state_key)
        
        prev_state = AlertState(state_data.get('state', 'OK'))
        consecutive = int(state_data.get('consecutive', 0))
        
        # Flap prevention: require MIN_CONSECUTIVE_EVALS before transition
        if new_state == prev_state:
            consecutive += 1
        else:
            consecutive = 1
        
        # State transition logic
        should_notify = False
        if (new_state == AlertState.CRITICAL and 
            consecutive >= self.MIN_CONSECUTIVE_EVALS and
            prev_state != AlertState.CRITICAL):
            # Alert firing
            should_notify = True
            
        elif (new_state == AlertState.OK and 
              prev_state == AlertState.CRITICAL):
            # Alert recovering
            should_notify = True
        
        # Update state in Redis
        self.redis.hset(state_key, mapping={
            'state': new_state.value,
            'consecutive': consecutive,
            'last_eval': time.time(),
            'last_value': str(current_value) if current_value else 'null'
        })
        self.redis.expire(state_key, 86400)  # 24-hour TTL
        
        if should_notify:
            self._send_notification(alert, new_state, current_value, prev_state)

    def _send_notification(self, alert, new_state, value, prev_state):
        """
        Route notification to all configured channels.
        Deduplication: don't send if same alert fired within last 4 hours.
        """
        dedup_key = f"alert:notified:{alert.alert_id}:{new_state.value}"
        
        if new_state == AlertState.CRITICAL:
            # Check dedup (don't re-notify if recently fired)
            if self.redis.exists(dedup_key):
                return
            self.redis.setex(dedup_key, 4 * 3600, "1")
        
        for channel in alert.notification_channels:
            self.notifier.send(
                channel=channel,
                alert=alert,
                state=new_state,
                value=value,
                previous_state=prev_state,
                dashboard_url=f"https://monitoring.example.com/alerts/{alert.alert_id}"
            )
```

**Interviewer Q&As**:

Q1: How do you handle 1 million active alerts without triggering 16,667 TSDB queries/second?
A: Query deduplication via grouping. Step 1: Parse all alert queries and extract the base metric selector (ignoring threshold conditions). Step 2: Group alerts with identical metric selectors and time windows. In practice, many alerts from the same customer query the same metric (e.g., 10 different CPU threshold alerts all query `cpu.usage{host=~".*"}`). With 1M alerts from 10K customers, after deduplication: approximately 10–50 unique base queries per customer × 10K customers = 100K–500K unique TSDB queries per cycle. Step 3: Execute each unique query once, then fan out the result to all alerts sharing that query. This reduces TSDB load to 100K–500K queries/60 seconds = 1,700–8,000 QPS — 2–10× more manageable.

Q2: What is "alert flapping" and how does your MIN_CONSECUTIVE_EVALS approach prevent it?
A: Alert flapping occurs when a metric oscillates around the threshold — one minute above (CRITICAL), the next below (OK), alternating rapidly. Without prevention, this generates dozens of notifications per hour, causing alert fatigue. Our approach: require MIN_CONSECUTIVE_EVALS=2 consecutive evaluations in the new state before transitioning. So a metric must be above threshold for 2 consecutive minutes before firing, and below threshold for 1 consecutive minute before recovering. This means: minimum time to fire = 2 minutes; minimum time to recover = 1 minute. This suppresses noise without significantly delaying legitimate alerts. For critical alerting (production outage), 2 minutes is an acceptable trade-off. For less critical alerts (warning level), we use MIN_CONSECUTIVE_EVALS=3 (3 minutes to fire).

Q3: How do you implement anomaly detection alerts (e.g., "alert when CPU is 3 standard deviations above normal")?
A: Anomaly detection requires a baseline model. We compute a rolling baseline using the last 7 days of data for the same day-of-week and time-of-day (accounting for weekly seasonality). The baseline is `mean ± 3 * std_dev` for each 1-hour window of the week. This baseline is pre-computed nightly by a Spark job and stored in the TSDB as synthetic metric series: `cpu.usage.baseline_mean{...}` and `cpu.usage.baseline_stddev{...}`. The alert query for anomaly detection becomes:
```
abs(cpu.usage - cpu.usage.baseline_mean) > 3 * cpu.usage.baseline_stddev
```
This is a standard MetricsQL query evaluated by the same alert engine, no special code path needed. More sophisticated anomaly detection (SARIMA, Prophet) runs as a separate ML service that writes anomaly score series back to the TSDB.

Q4: How do you handle alert evaluation for very slow metrics — ones emitted only once per hour?
A: Slow metrics require longer evaluation windows. The alert's `window_seconds` parameter specifies how far back to look for data. For a metric emitted hourly, `window_seconds=7200` (2 hours) ensures the most recent value is always within the window. The alert engine uses `last_over_time(metric[window])` semantics — the last value within the window is used for evaluation. If no data arrives within `window_seconds`, the alert transitions to `NO_DATA` state (separately configurable — can notify or silently ignore). The evaluation interval remains 60 seconds — checking every minute if the hourly metric's 2-hour window contains a value.

Q5: How do you prevent a single customer with 100,000 complex alerts from degrading alert evaluation for all other customers?
A: Multi-tenancy isolation for alert evaluation: (1) Customer alert quota: maximum 10,000 active alerts per customer on the standard tier. (2) Per-customer evaluation time budget: each customer's alerts are evaluated in a separate goroutine pool with a timeout. If a customer's evaluation takes > 50s (out of the 60s cycle), their alerts are marked "evaluation timeout" and will retry next cycle. (3) Query complexity limits: alerts with complex queries (too many time series selected, too long a time range) are validated at creation time and rejected if they exceed complexity thresholds. (4) Separate evaluation workers per tier: enterprise customers with > 10K alerts run on dedicated evaluation workers.

---

## 7. Scaling

### Horizontal Scaling

- **Metric Collectors**: Stateless, scale to N pods. At 1M dp/s and 250 MB/s ingest, each pod handles 50K dp/s (12.5 MB/s). Need 20 pods minimum; run 60 for redundancy.
- **TSDB (VictoriaMetrics Cluster)**: vminsert (stateless, routes to vmstorage), vmstorage (stateful, stores data sharded by MetricID hash), vmselect (stateless query fan-out). Start with 10 vmstorage nodes, each handling 1M series. Scale horizontally: 100 nodes for 100M series.
- **Alert Engine**: Stateless evaluation workers with shared Redis state. Scale to 50 workers, each evaluating 20K alerts/cycle. Redis holds state for 1M alerts × 200 B = 200 MB — negligible.
- **Elasticsearch (logs)**: 20-node cluster initially. Each node: 64 GB RAM, 2 TB SSD. Handles ~100K documents/second ingest. For 500 MB/s log ingest at 500 B/log line = 1M logs/second, need 200 Elasticsearch nodes. **Note**: This is a reason to evaluate ClickHouse for logs — ClickHouse handles 1M+ rows/second insert on fewer nodes.

### DB Sharding

- **VictoriaMetrics**: vmstorage shards by MetricID hash automatically. Adding vmstorage nodes triggers automatic rebalancing of new writes (old data stays in place). Query tier (vmselect) fans out to all vmstorage nodes and merges results.
- **Cassandra (metadata)**: Naturally partitioned by `(customer_id, metric_name)`. With 100K unique metrics per customer × 10K customers = 1B rows — Cassandra handles this across 20 nodes.

### Replication

- **VictoriaMetrics**: Write to 2 vmstorage replicas (vminsert parameter: `replicationFactor=2`). On vmselect query, results are merged from all replicas; deduplication ensures no double-counting.
- **Elasticsearch**: Index replicas = 1 (one primary + one replica per shard). With 20 primary shards per index and 20-node cluster: each node holds 1 primary + 1 replica. ES handles node failure automatically.
- **Kafka**: Replication factor 3, min.insync.replicas = 2.

### Caching

- **Query result cache**: Redis stores `(query_hash, time_range) → serialized_result` for the last 5 minutes (TTL=300s). Dashboard re-renders (common when users have auto-refresh every 30s) hit cache 80% of the time.
- **Metric metadata cache**: Popular metric names and their tag keys are cached in-process (dictionary, TTL=60s). Autocomplete suggestions are pre-computed and stored in Redis (ZRANGEBYLEX for prefix matching).
- **Alert evaluation cache**: The TSDB query result for a metric window is cached in Redis for 30 seconds. If two alerts query the same metric in the same 60-second evaluation cycle, the second alert's query hits the cache.

### CDN

- Static dashboard assets (JS, CSS) are served from CloudFront with 24-hour TTL.
- The dashboards API (metric query results) is not CDN-cacheable (personalized, real-time data). However, certain public "status page" metrics (e.g., Datadog's own status page) can be cached at the edge with short TTL (30s).

### Interviewer Q&As (Scaling)

Q1: At 10M active time series, the VictoriaMetrics index consumes how much memory, and how do you manage this?
A: VictoriaMetrics' index (inverted index for tag key→MetricID lookups) stores ~1 KB per active time series in the tsid cache. At 10M series: 10 GB RAM per vmstorage node. With 10 nodes: 100 GB total across the cluster (each node has its shard, so ~10 GB per node). Modern cloud instances have 128–256 GB RAM, so 10 GB is easily manageable. However, index flushes to disk happen periodically (mergeset/LSM compactions). During heavy compaction, query latency can increase. We schedule compaction during low-traffic windows and monitor `vminsert.slow_down_inserts` metric (VictoriaMetrics exposes this internally) to detect compaction backpressure.

Q2: How does Elasticsearch handle 1 million log ingest per second? That's an aggressive requirement.
A: 1M logs/second is indeed at the upper end of Elasticsearch capacity. A single modern Elasticsearch node (32 cores, 64 GB RAM, NVMe SSD) handles ~50K documents/second during bulk indexing (accounting for analysis/tokenization overhead). Achieving 1M/s requires ~20 such nodes. We optimize: (1) Bulk API (batch 1,000 documents per request, reducing per-request overhead). (2) Disable `_source` field compression for hot indices (trade disk for speed). (3) Use `date_detection: false` and pre-define mappings (avoids dynamic mapping overhead per document). (4) Per-customer indices shard by day, with 5 primary shards per index per 10GB of expected daily data. At this scale, an alternative to Elasticsearch worth evaluating is ClickHouse with a full-text search engine — recent versions (23.x+) have native full-text indexing that approaches Elasticsearch performance with 3-5× better compression and lower memory footprint.

Q3: How do you scale alert evaluation when the number of alerts grows to 10 million?
A: At 10M alerts, the grouping optimization becomes critical. With 10M alerts across 10K customers, average 1,000 unique metric queries per customer × 10K customers = 10M unique queries/cycle. Even with perfect grouping, 10M unique queries/60 seconds = 167K TSDB queries/second. This overwhelms the query tier. Solution: pre-computation. For the most common alert patterns (threshold on a single metric), pre-compute the current value for each metric and store in Redis every 60 seconds (push model). Alert evaluation becomes a Redis key lookup (O(1)) instead of a TSDB query. Only complex alerts (multi-metric expressions, rate() over long windows) require TSDB queries. Split alert evaluations into "simple" (Redis lookup, 80% of cases) and "complex" (TSDB query, 20%). This scales to 10M simple alerts with 800K TSDB queries/cycle for complex ones.

Q4: Your system targets 10M unique time series. Datadog reportedly handles hundreds of billions. What would need to change?
A: Scaling to 100B time series requires architectural changes: (1) VictoriaMetrics' in-memory MetricID cache cannot hold 100B entries (100B × 1 KB = 100 TB RAM). Switch to a distributed index stored in RocksDB or Cassandra with LRU caching of hot entries. (2) TSDB storage: 100B series × 10 data points/series/day × 8 bytes = 8 TB/day. Require hundreds of vmstorage nodes or a shift to object store-backed storage (VictoriaMetrics Enterprise supports this). (3) Ingest: 100B series × 1 data point/10s = 10B dp/s. Requires thousands of collector pods and Kafka partitions. (4) Query: Querying across 100B series with tag filter `{service="api"}` (potentially 1B series matching) requires distributed index server. This is the core technical challenge that differentiates Datadog's scale — their time-series index is a proprietary distributed system.

Q5: How do you handle log data at 500 MB/s when Elasticsearch has write latency of 100-200 ms?
A: Kafka acts as the buffer. Log collectors write to Kafka at 500 MB/s (Kafka easily sustains 500 MB/s per topic at 100 MB/s per broker on 5 brokers). Elasticsearch consumer lags behind but catches up. Acceptable log indexing lag: 30 seconds (logs are searched with at-least-30-second delay, acceptable for most debugging scenarios). We tune the consumer: batch size = 10,000 log lines, flush every 1 second. Elasticsearch bulk indexing speed: 50 MB/s per node, 20 nodes = 1 GB/s theoretical. With overhead (tokenization, indexing): ~200 MB/s sustained = within our 500 MB/s budget. The S3 archival path is separate and writes at full 500 MB/s directly via Kafka Connect (no processing, just compression).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Mitigation | Data Loss Risk |
|---|---|---|---|---|
| Metric Collector crash | Pod dies | K8s liveness probe | K8s restart; pending UDP packets lost (UDP is lossy by design) | UDP: up to 5s of data; HTTP: none (client retries) |
| Kafka broker failure | Leader unavailable | Consumer lag spike | KRaft auto-elects new leader < 10s | None (replication factor 3) |
| VictoriaMetrics vmstorage node failure | Write errors | vminsert detects, routes to replica | Writes go to other replica; degraded replication until node recovers | None (replication factor 2) |
| Alert Engine pod failure | Alerts not evaluated | Alert watchdog (deadman alert) | K8s restarts pod; Redis state survives (allows correct resume) | Missed evaluations for < 60s |
| Elasticsearch node failure | Shard unavailable | ES cluster health turns RED/YELLOW | Shard replica promoted automatically | None (1 replica) |
| Redis (alert state) failure | Alert state lost | Connection error | Fail safe: re-evaluate all alerts as if first run (may cause spurious re-notifications, acceptable) | Alert state reset |
| TSDB write failure during compaction | Write timeouts | vminsert backpressure metric | Kafka consumer backs up; Kafka buffer absorbs up to 7 days | None (Kafka retention) |

### Failover

Multi-AZ deployment for all stateful components. VictoriaMetrics replication ensures data survives single AZ failure. Alert Engine is stateless (state in Redis with cross-AZ replicas). Elasticsearch with cross-AZ replica placement. Kafka rack-aware replica placement.

### Retries & Idempotency

- **Metric HTTP ingest**: Client retries with same batch. VictoriaMetrics ignores duplicate data points (same MetricID + timestamp). TSDB is naturally idempotent for duplicate samples (last write wins, same value).
- **Alert notifications**: Deduplication key in Redis prevents duplicate notifications. PagerDuty API deduplication key (alert_id + state + trigger_time) prevents duplicate incidents.
- **Kafka consumer checkpointing**: Consumers commit offsets only after successful write to TSDB or Elasticsearch. Failure triggers reprocessing from last committed offset.

### Circuit Breaker

Alert Engine has a circuit breaker around TSDB queries: if > 20% of queries fail in 30 seconds, the engine enters "degraded mode" — it uses stale query results (last known value) for alert evaluation rather than querying TSDB. Alerts based on stale data may be slightly inaccurate, but the alert system remains operational during a TSDB incident.

---

## 9. Monitoring & Observability

### Key Metrics (Monitoring the Monitor)

| Metric | Type | Alert Threshold | Interpretation |
|---|---|---|---|
| `ingest.rate.per_second` | Counter | < 500K/s (dead man) | Ingest pipeline health |
| `ingest.kafka.lag.metric_consumers` | Gauge | > 5M messages | TSDB write path falling behind |
| `ingest.kafka.lag.log_consumers` | Gauge | > 50M messages (500 MB × 100) | Log indexing falling behind |
| `tsdb.write.p99_latency_ms` | Histogram | > 1000 ms | TSDB under pressure |
| `tsdb.active_series_count` | Gauge | > 12M (near limit) | Approaching cardinality limit |
| `alert_engine.eval_cycle_duration_ms` | Histogram | > 55,000 ms | Alert cycle not completing in 60s |
| `alert_engine.query_errors_rate` | Gauge | > 5% | TSDB issues affecting alert reliability |
| `elasticsearch.cluster_health` | Gauge | != 0 (GREEN) | Log search degraded |
| `elasticsearch.indexing_lag_seconds` | Gauge | > 60 s | Log search freshness degraded |
| `cardinality.high_cardinality_customers` | Counter | > 10/day | Customers hitting cardinality limits |
| `query.p95_latency_ms` | Histogram | > 2000 ms | Dashboard queries slow |
| `rollup.lag_minutes` | Gauge | > 30 min | Rollup service behind |

### Distributed Tracing

All metric submissions from agents include `trace_id` in log and metric tags. The query service uses OpenTelemetry to trace each query: `[query_parse → tag_index_lookup → vmstorage_fetch × N → merge → serialize]`. Jaeger renders the fan-out query as a flame graph, enabling identification of slow vmstorage nodes.

### Logging

Structured logging in all services. Key log events: cardinality violation (customer_id, metric_name, tag_key, estimated_cardinality), alert state transition (alert_id, prev_state, new_state, value, threshold), query error (query, customer_id, error, duration). Log pipeline is bootstrapped separately from the main pipeline (ships to a separate Elasticsearch cluster — the monitoring system cannot rely on itself to monitor itself: "eating your own dog food" is dangerous for a monitoring system's own observability).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Choice | Alternative | Reason | Accepted Trade-off |
|---|---|---|---|---|
| TSDB Engine | VictoriaMetrics | Prometheus + Thanos | 5× better write throughput, native clustering, no extra components | Smaller ecosystem than Prometheus |
| Log Storage | Elasticsearch + S3 | ClickHouse for logs | ES has best-in-class full-text search, rich query DSL, Kibana ecosystem | Memory-intensive (JVM), complex ops |
| Cardinality protection | HLL + tag stripping | Hard series limit only | HLL gives early warning and granular mitigation before hard limit | 1% HLL estimation error |
| Alert state | Redis | DB-backed | Sub-ms state reads critical for evaluation cycle throughput | State lost on Redis failure (acceptable: alerts re-initialize) |
| Alert evaluation | Batched by query | Independent per alert | 10–50× reduction in TSDB queries | Complex grouping logic |
| Metric rollups | Separate metric series (`_rollup_1m_*`) | In-place downsampling | Separate series preserves raw data intact, allows independent retention policies | 4× storage per metric (sum, count, max, min) for rollup |
| UDP acceptance | Accept without ack | Only HTTP with ack | UDP allows fire-and-forget from hot code paths with zero overhead | Up to 5s data loss during collector crash |
| Pre-aggregation | Flink pre-agg before TSDB | Write raw to TSDB only | Without pre-agg, every raw data point creates a TSDB write; at 1M dp/s with 10K instances of same metric, pre-agg reduces to 1 write/metric/second | Aggregated view in TSDB, not individual samples |
| Multi-tenancy isolation | Per-customer cardinality + quota | Shared infrastructure, no limits | Noisy neighbor prevention | Added complexity per tenant |

---

## 11. Follow-up Interview Questions

Q1: How does your system handle "metrics backfill" — importing 2 years of historical metrics from a legacy system?
A: Backfill requires writing data to the past-dated TSDB tier. For data < 15 days old, write directly to the raw TSDB via the standard API with the original timestamps. For data older than 15 days (rollup tier), convert to 1-minute aggregates and write to the rollup TSDB with the `__backfill=true` tag (to exclude from cardinality calculations). VictoriaMetrics supports backfill natively via its HTTP import API. Backfill is rate-limited to 10% of total ingest capacity to avoid impacting real-time ingest. The backfill job is a Spark batch process that reads the legacy system's export format (CSV, Prometheus remote write, OpenMetrics) and produces batched requests to the TSDB import API.

Q2: How do you implement "derived metrics" — metrics that are computed from other metrics (e.g., error rate = errors / total requests)?
A: Derived metrics are implemented via recording rules — VictoriaMetrics' equivalent of Prometheus recording rules. A recording rule is a MetricsQL expression evaluated every 1 minute and stored as a new time series:
```
record: "http.error_rate"
expr: sum(rate(http.requests.count{status_code=~"5.."}[5m])) / sum(rate(http.requests.count[5m]))
```
Recording rules are evaluated by vmrule (VictoriaMetrics' rule evaluation component, similar to Prometheus alertmanager + ruler). The result is stored as a regular metric in the TSDB. Benefits: (1) Dashboards querying `http.error_rate` don't need to compute the division on every query. (2) Alerts can reference pre-computed derived metrics for simpler queries. (3) Pre-computation makes long-range historical queries fast (the derived metric is already stored at 1-minute granularity).

Q3: Your system costs X to run. How would you reduce cost by 50% while maintaining SLAs?
A: Cost reduction options: (1) Spot instances for stateless workers (collectors, alert evaluators): 70% cost reduction on AWS Spot vs. On-Demand. Risk: spot interruption handled by Kafka buffering. (2) Tiered TSDB storage: move rollup data older than 90 days from EBS-backed vmstorage to S3-backed storage (VictoriaMetrics Enterprise supports S3-backed cold storage at $23/TB/month vs. $100/TB/month for EBS). (3) Log compression: Elasticsearch indices older than 3 days are force-merged (single segment, maximum compression) and moved to frozen tier (searchable snapshots on S3) — 90% cost reduction for aged log data. (4) Reduce Kafka retention from 7 days to 3 days (metrics data, logs) — reduces Kafka storage by 57%. (5) Aggressive rollup: downsample more aggressively at 30-day boundary (5-minute resolution instead of 1-minute for the 30–90 day tier). Combined these can reach 50% cost reduction.

Q4: How do you implement "service level objectives" (SLOs) on top of this metrics system?
A: An SLO is a target availability for a service (e.g., "99.9% of HTTP requests return 2xx within 500 ms"). It's composed of: (1) A compliance metric: `sum(rate(http.requests.count{status_code="200",duration_bucket=~"0.5"}[5m])) / sum(rate(http.requests.count[5m]))` — this is stored as a recording rule. (2) An error budget: the percentage of compliance below target (e.g., 0.1% non-compliance per 30 days = 43.2 minutes of allowed downtime). (3) Burn rate alerts: fire when the error budget is being consumed too quickly (e.g., 14.4× burn rate = budget depleted in 2 hours; 1× burn rate = on track for the 30-day window). We implement a dedicated SLO service that reads from the TSDB, computes error budget consumption, and renders an "SLO dashboard" separate from the general metrics dashboard.

Q5: How would you design a "debug-time" high-resolution mode where a metric is sampled every 100ms instead of every 10 seconds?
A: High-resolution metrics at 100ms resolution create 100× more data points. For a short debug window (say, 10 minutes), this is: 1 metric × 100/s × 600s = 60,000 data points — manageable. Implementation: (1) Agent supports configurable per-metric collection interval. For debug mode, set to 100ms. (2) Data is written to a separate "high-resolution" VictoriaMetrics cluster with a 24-hour retention policy (data is expensive to keep long-term at this resolution). (3) The Query Service is aware of the high-resolution cluster and routes queries for recent data to it when the requested step < 1s. (4) After the debug window, the customer explicitly reverts the metric interval. Billing for high-resolution metrics is separate (10× surcharge for data points at < 1s resolution). The standard TSDB cluster is not used for high-resolution data to avoid impacting standard customers.

Q6: How do you handle timezone-aware alerting — an alert should only fire during business hours (9 AM – 5 PM) in the customer's timezone?
A: Alert definitions have an optional `schedule` field:
```json
{
  "schedule": {
    "timezone": "America/New_York",
    "active_hours": [{"days": ["Mon","Tue","Wed","Thu","Fri"], "start": "09:00", "end": "17:00"}]
  }
}
```
The alert engine checks the schedule before evaluating the condition. Outside active hours, the alert is in "paused" state — no evaluation, no notification. This is stored in the alert's Redis state with a TTL set to the next active window start. We use the `pytz` / `zoneinfo` library for timezone calculations. Edge cases: alerts that span midnight (e.g., "active 22:00–06:00") are handled by splitting into two ranges; DST transitions are handled by recalculating the schedule after each DST changeover event (we subscribe to a time change notification).

Q7: How do you implement metric tags for dynamic infrastructure (Kubernetes pods that live for seconds)?
A: Kubernetes pods with short lifespans create new time series for each pod startup (new `pod_name` tag value). A deployment with 1,000 pods cycling every minute creates 1,000 × 60 = 60,000 new series per hour — high cardinality. Solutions: (1) Replace `pod_name` with `pod_template_hash` (stable per deployment revision) or aggregate at the deployment level (`kube_deployment_name` instead of `pod_name`). (2) "Ephemeral" tag policy: tags marked as ephemeral don't contribute to cardinality limits and their series are garbage-collected after 24 hours of inactivity (VictoriaMetrics marks series inactive after 24 hours of no data). (3) The Kubernetes operator (Datadog Cluster Agent equivalent) automatically maps `pod_name` → `service_name` during metric enrichment, replacing the high-cardinality `pod_name` tag with the lower-cardinality `service_name` tag before metric submission.

Q8: How does the system handle "counter resets" — when a counter metric restarts from 0 after a service restart?
A: Counter resets are a fundamental challenge with counters in Prometheus/Datadog style. When a service restarts, its counter goes from 1,000 back to 0. A naive rate calculation would show a huge negative spike. The `rate()` and `increase()` functions in MetricsQL/PromQL detect counter resets: if the current value < previous value, it's assumed to be a reset. The function handles it by using the delta from the reset point (0 to current value) instead of (previous value to current value). VictoriaMetrics follows the same semantics as Prometheus here. The only case this fails: if a counter decreases for a legitimate reason (rare, but some custom counters decrement). For such metrics, the `gauge` type is more appropriate than `counter`.

Q9: You mentioned 10-second agent collection intervals. What if an engineer queries for sub-10-second resolution?
A: Requesting step=1s on a metric collected every 10s will return data at 10-second resolution (the actual granularity) with null/no_data for intermediate steps. VictoriaMetrics fills these with the last known value when using `last_over_time()` semantics, or leaves them empty for `instant_over_time()`. We document the native collection interval in the metric metadata UI so users understand what resolution to expect. For users who genuinely need 1-second resolution, they must configure their agent to collect at 1-second intervals (with the associated 10× storage cost) or use the high-resolution debug mode described in Q5. We surface native collection intervals in the autocomplete when a user types a metric name in the query editor.

Q10: How do you handle metric schema evolution — a metric's tag set changes over time (a tag is added or renamed)?
A: Tag changes create different time series (different MetricID). Old and new series coexist in the TSDB. The metadata store records all historical tag keys for each metric. Dashboard queries using `{service="api", env="prod"}` will match both old series (with `environment="prod"`) and new series (with `env="prod"`) only if the query explicitly handles this. We help customers by: (1) Deprecation warnings in the UI when a tag key hasn't been seen for 7 days (old tag likely renamed). (2) A "tag alias" feature: register that `environment` and `env` are aliases → queries for `env` also match `environment` series. (3) Migration tooling: a job that reads old series from TSDB and re-emits them with new tag names under a new metric name (copy with rename). This is the recommended migration path.

Q11: How does the system handle a customer sending metrics at 100× their contracted rate (intentional or misconfigured)?
A: Rate limiting is enforced at the collector tier. Each customer has a DPS (data points per second) quota stored in Redis. The collector checks this quota with a token bucket algorithm (same pattern as the web crawler's rate limiter). When the quota is exceeded: the HTTP endpoint returns 429. The StatsD UDP endpoint silently drops packets. The customer receives an automatic email alert when they reach 80% of quota. Rate limit violations are logged for billing/compliance review. If a customer legitimately needs more quota (rapid growth), they can request an upgrade via the customer portal, which updates their Redis quota in real time.

Q12: How do you support "on-call rotation integration" — routing alerts to the current on-call engineer dynamically?
A: We integrate with PagerDuty's API directly: when routing to a PagerDuty service, the alert engine calls PagerDuty's `/oncalls` API to determine who is currently on call. The notification includes their name in the message body. For Slack routing, we support `@oncall` mention: the notification service calls a customer-configured on-call calendar API (PagerDuty, OpsGenie, or a custom webhook) to resolve `@oncall` to the current on-call engineer's Slack user ID at notification time. This integration is implemented as a notification channel plugin — the notification routing is extensible via a plugin system that customers can configure.

Q13: What is your disaster recovery strategy for the TSDB data?
A: TSDB data is backed up to S3 via vmbackup (VictoriaMetrics' backup tool) every 24 hours. Incremental backups every 6 hours capture only changed blocks. The backup process uses a consistent snapshot (similar to filesytem snapshot) that doesn't interrupt active writes. RTO (Recovery Time Objective): 4 hours to restore from backup to a new cluster. RPO (Recovery Point Objective): 6 hours (last incremental backup). For the most recent 6 hours, Kafka retention (7 days) allows replay of raw metrics to a new TSDB cluster. Full RTO with Kafka replay: 2 hours (restore 24-hour backup) + replay 6 hours of Kafka = total RTO 3 hours. This meets our 4-hour RTO target.

Q14: How would you build a "metric explorer" feature that shows all metrics available for a given service?
A: The metric explorer is backed by the Cassandra `metric_names` table. Query: `SELECT metric_name, metric_type, tag_keys FROM metric_names WHERE customer_id = 'X' AND metric_name LIKE 'http.%'` — but Cassandra doesn't support LIKE queries on non-partition-key columns. Solution: We build a separate search index for metric names using Elasticsearch (or a trie data structure in Redis ZADD + ZRANGEBYLEX for prefix searches). The metric explorer backend queries this search index for autocomplete as the user types. Each metric entry includes: name, type, all observed tag keys (last 7 days), current cardinality estimate, and a sparkline (last 24 hours of the metric for a sample host). The sparkline is fetched from VictoriaMetrics via a lightweight query at explorer load time.

Q15: How do you handle "multi-region" deployment — customers want their metrics to be queryable globally but want their data to stay in their region for compliance?
A: Multi-region is implemented as a federated architecture: each region (US, EU, AP) has its own independent TSDB cluster, Kafka cluster, and Elasticsearch. Metric agents route to the nearest regional collector (GeoDNS). Data never leaves the region — compliant with GDPR, APEC CBPR, and similar frameworks. Cross-region querying (for a customer with infrastructure in multiple regions) is handled by the Query Service's federation layer: when a dashboard query is executed, the Query Service determines which regions the customer has data in and fan-outs the query to each regional query service, then merges results. This is transparent to the user. Regional metadata (customer configuration, alert definitions) is replicated to all regions via CockroachDB (multi-region CRDB cluster) to ensure alerts work even during inter-region connectivity issues.

---

## 12. References & Further Reading

1. **VictoriaMetrics Documentation and Design** — VictoriaMetrics Inc. https://docs.victoriametrics.com/ — Architecture overview, benchmark comparisons, and clustering design. The basis for many TSDB design decisions in this document.

2. **"Gorilla: A Fast, Scalable, In-Memory Time Series Database"** — Pelkonen et al., *Proceedings of VLDB 2015*. Facebook's paper describing the Gorilla TSDB, including the XOR compression algorithm used by Prometheus and VictoriaMetrics.

3. **Prometheus Documentation: Storage** — Prometheus Authors. https://prometheus.io/docs/prometheus/latest/storage/ — Describes the Prometheus TSDB block format, WAL, and head chunk design.

4. **"Thanos: Highly Available Prometheus Setup"** — Bartlomiej Plotka, *KubeCon EU 2019*. https://thanos.io/tip/thanos/design.md/ — Describes the Thanos architecture and trade-offs vs. native clustering.

5. **Elasticsearch Internals** — Elastic N.V. https://www.elastic.co/guide/en/elasticsearch/reference/current/index-modules-store.html — Storage engine documentation including Lucene segment structure and index lifecycle management.

6. **"Beringei: A High-Performance Time Series Storage Engine"** — Lin et al., *Facebook Engineering Blog*, 2017. Describes Facebook's successor to Gorilla, highlighting the importance of delta-of-delta encoding for timestamp compression.

7. **"Monarch: Google's Planet-Scale In-Memory Time Series Database"** — Adams et al., *Proceedings of VLDB 2020*. How Google handles planet-scale metrics storage with zone-based leaf storage and aggregation trees.

8. **OpenTelemetry Specification** — CNCF. https://opentelemetry.io/docs/specs/otel/ — Defines the metric, trace, and log data model that modern observability systems ingest.

9. **"How Datadog Handles Billions of Metrics"** — Datadog Engineering Blog. https://www.datadoghq.com/blog/engineering/dogstatsd-metrics-aggregation-pipeline/ — High-level overview of Datadog's metrics aggregation pipeline.

10. **HyperLogLog: The Analysis of a Near-Optimal Cardinality Estimation Algorithm** — Flajolet et al., *AOFA 2007 Conference*. The foundational paper for the HyperLogLog algorithm used for cardinality estimation in our system.
