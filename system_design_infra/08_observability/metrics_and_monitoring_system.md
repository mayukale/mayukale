# System Design: Metrics and Monitoring System

> **Relevance to role:** A cloud infrastructure platform engineer must design and operate the metrics pipeline that provides real-time visibility into bare-metal hosts, Kubernetes clusters, OpenStack control planes, job schedulers, and Java/Python services. Deep understanding of Prometheus internals (TSDB, scrape model, PromQL), long-term storage (Thanos/VictoriaMetrics), Grafana dashboard-as-code, and Java instrumentation (Micrometer) is essential for this role.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Collect metrics** from all infrastructure components: bare-metal hosts (CPU, memory, disk, network), Kubernetes pods (container resource usage), OpenStack services, MySQL (query latency, connections), Elasticsearch cluster health, Kafka broker metrics, and custom application metrics.
2. **Time-series storage** with high-resolution data (15s scrape interval) for recent data and downsampled data for long-term trends.
3. **Flexible query language** (PromQL) for ad-hoc analysis, dashboard rendering, and alert rule evaluation.
4. **Dashboard system** (Grafana) with templated dashboards, drill-down capability, and dashboard-as-code (version controlled).
5. **Recording rules** to pre-compute expensive queries (e.g., percentiles over large cardinality).
6. **Long-term storage** (1+ year) for capacity planning and trend analysis.
7. **Multi-cluster federation** for global views across all Kubernetes clusters and data centers.
8. **Push support** for batch jobs and short-lived processes via Pushgateway.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Scrape interval | 15 seconds (default), 5 seconds for critical services |
| Query latency (recent data, <24h) | < 500ms p95 |
| Query latency (long-term, >30d) | < 5 seconds p95 |
| Metric ingestion rate | 50 million active time series |
| Data retention (full resolution) | 15 days local, 1 year in long-term store |
| Data retention (downsampled 5m) | 3 years |
| Availability | 99.9% for scraping and alerting, 99.5% for querying |
| Cardinality limit | < 100K series per scrape target |

### Constraints & Assumptions
- Fleet: 50,000 bare-metal hosts, 200 Kubernetes clusters (500,000+ pods), 5 data centers.
- Each host exposes ~500 metrics via Node Exporter; each pod exposes ~200 metrics.
- Existing Prometheus instances per cluster; need to unify into a global view.
- Grafana is the approved visualization tool.
- Java services use Spring Boot with Micrometer; Python services use `prometheus_client`.

### Out of Scope
- Log collection and search (see distributed_logging_system.md).
- Distributed tracing (see distributed_tracing_system.md).
- Alerting engine details (see alerting_and_on_call_system.md -- this file covers metric collection and storage).
- Business metrics and analytics (data warehouse domain).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Bare-metal host metrics | 50,000 hosts x 500 metrics | 25M series |
| Kubernetes pod metrics | 500,000 pods x 200 metrics | 100M series (but ~50% are short-lived) |
| Active K8s series (after churn) | ~50M active at any time | 50M series |
| Infrastructure service metrics | 10,000 instances x 300 metrics | 3M series |
| Custom application metrics | 5,000 services x 100 custom metrics | 500K series |
| **Total active time series** | Sum | **~78.5M series** |
| Samples per second | 78.5M / 15s scrape interval | **5.2M samples/sec** |
| Peak samples/sec (2x) | During deploys, pod churn | **10.4M samples/sec** |

### Latency Requirements

| Query Type | Target |
|---|---|
| Instant query (single metric, last value) | < 50ms |
| Range query (1 metric, 1 hour, 15s resolution) | < 200ms |
| Range query (1 metric, 7 days, 1m resolution) | < 1 second |
| Dashboard load (20 panels, 6 hour range) | < 3 seconds |
| Long-term query (1 metric, 1 year, 1h resolution) | < 5 seconds |
| Recording rule evaluation | < 10 seconds per rule group |

### Storage Estimates

| Component | Calculation | Value |
|---|---|---|
| Bytes per sample | 1-2 bytes (Prometheus TSDB compression) | ~1.5 bytes |
| Samples per day | 5.2M/s x 86,400 | ~449B samples/day |
| Daily storage (raw) | 449B x 1.5 bytes | **~674 GB/day** |
| Local retention (15 days) | 674 GB x 15 | **~10 TB per Prometheus cluster** |
| Long-term storage (1 year, raw) | 674 GB x 365 | **~246 TB** |
| Long-term storage (downsampled 5m, 3 years) | 246 TB / 20 (downsample ratio) x 3 | **~37 TB** |
| Object store total | 246 TB + 37 TB | **~283 TB** |

### Bandwidth Estimates

| Segment | Bandwidth |
|---|---|
| Scrape traffic (pull model) | 78.5M series x 200 bytes avg response / 15s = ~1 GB/s aggregate |
| Prometheus → Thanos Sidecar → S3 upload | ~674 GB/day = ~8 MB/s per cluster, ~1.6 GB/s total across 200 clusters |
| Thanos Query fan-out to stores | Variable, typically < 100 MB per query |
| Grafana → Thanos Query | ~10 MB/s aggregate (200 concurrent dashboard users) |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        METRIC SOURCES (Exporters)                           │
│                                                                             │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────────┐  │
│  │ Node Exporter│ │kube-state-   │ │ cAdvisor     │ │ JMX Exporter     │  │
│  │ (per host)   │ │metrics       │ │ (per node)   │ │ (Kafka, ES,      │  │
│  │ CPU, mem,    │ │(per cluster) │ │ container    │ │  MySQL)          │  │
│  │ disk, net    │ │ pod status,  │ │ CPU, mem,    │ │                  │  │
│  │              │ │ deploy count │ │ network      │ │                  │  │
│  └──────┬───────┘ └──────┬───────┘ └──────┬───────┘ └────────┬─────────┘  │
│         │                │                │                   │            │
│  ┌──────────────┐ ┌──────────────────┐ ┌──────────────────────────────┐   │
│  │ App Metrics  │ │ Spring Actuator  │ │ Pushgateway (batch jobs)     │   │
│  │ Python:      │ │ Micrometer +     │ │ Short-lived processes push   │   │
│  │ prom_client  │ │ Prometheus reg   │ │ metrics before exit          │   │
│  └──────┬───────┘ └──────┬───────────┘ └──────────────┬───────────────┘   │
│         │                │                             │                   │
└─────────┼────────────────┼─────────────────────────────┼───────────────────┘
          │                │                             │
          ▼                ▼                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                   PROMETHEUS INSTANCES (per cluster/DC)                      │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │  Prometheus Server (HA pair: 2 replicas scraping same targets)      │   │
│  │  ┌──────────┐ ┌──────────────┐ ┌────────────┐ ┌────────────────┐   │   │
│  │  │ Scraper  │ │ TSDB (local) │ │ Rule       │ │ Alertmanager   │   │   │
│  │  │ HTTP GET │ │ 2h blocks    │ │ Engine     │ │ Client         │   │   │
│  │  │ /metrics │ │ WAL + chunks │ │ Recording  │ │ Sends alerts   │   │   │
│  │  │ every 15s│ │ 15d retention│ │ rules +    │ │ to             │   │   │
│  │  │          │ │              │ │ alert rules│ │ Alertmanager   │   │   │
│  │  └──────────┘ └──────────────┘ └────────────┘ └────────────────┘   │   │
│  └──────────────────────────┬──────────────────────────────────────────┘   │
│                             │                                              │
│  ┌──────────────────────────▼──────────────────────────────────────────┐   │
│  │  Thanos Sidecar (per Prometheus instance)                           │   │
│  │  - Uploads completed TSDB blocks (2h) to object store              │   │
│  │  - Exposes StoreAPI for live data from Prometheus TSDB              │   │
│  └──────────────────────────┬──────────────────────────────────────────┘   │
│                             │                                              │
└─────────────────────────────┼──────────────────────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
              ▼               ▼               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     OBJECT STORE (S3 / MinIO)                               │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │  TSDB Blocks (2h chunks, compacted)                                 │    │
│  │  Raw resolution: 1 year retention                                   │    │
│  │  5m downsampled: 3 year retention                                   │    │
│  │  1h downsampled: 5 year retention                                   │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │
              ┌───────────┼──────────────────────┐
              │           │                      │
              ▼           ▼                      ▼
┌───────────────────┐ ┌──────────────────┐ ┌──────────────────────────────┐
│ Thanos Store      │ │ Thanos Compactor │ │ Thanos Query (Global)        │
│ Gateway           │ │ (Singleton)      │ │                              │
│ - Loads block     │ │ - Merges blocks  │ │ - Receives PromQL queries    │
│   metadata from   │ │   from same      │ │ - Fan-out to Store Gateways  │
│   object store    │ │   source         │ │   and Prometheus Sidecars    │
│ - Serves block    │ │ - Downsamples    │ │ - Deduplicates HA pairs      │
│   data via        │ │   (5m, 1h)       │ │ - Merges and returns         │
│   StoreAPI        │ │ - Applies        │ │                              │
│ - Caches index    │ │   retention      │ │ ┌────────────────────────┐   │
│   in memory       │ │                  │ │ │ Thanos Query Frontend  │   │
└───────────────────┘ └──────────────────┘ │ │ - Query splitting      │   │
                                           │ │ - Result caching       │   │
                                           │ │ - Retry/dedup          │   │
                                           │ └────────────────────────┘   │
                                           └──────────────┬───────────────┘
                                                          │
                                                          ▼
                                           ┌──────────────────────────────┐
                                           │  GRAFANA                     │
                                           │  - Data source: Thanos Query │
                                           │  - Dashboards-as-code        │
                                           │    (Grafonnet / Terraform)   │
                                           │  - Templating: $cluster,     │
                                           │    $namespace, $service      │
                                           │  - Alerting rules → AM       │
                                           └──────────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Exporters** | Expose metrics in Prometheus format (`/metrics` HTTP endpoint). Each exporter specializes: Node Exporter (OS-level), kube-state-metrics (K8s objects), cAdvisor (container resources), JMX Exporter (JVM/Java apps), Micrometer (Spring Boot apps). |
| **Prometheus** | Pull-based metrics collector. Scrapes targets every 15s. Stores in local TSDB. Evaluates recording rules and alert rules. Ships alerts to Alertmanager. |
| **Thanos Sidecar** | Runs alongside Prometheus. Uploads completed TSDB blocks (every 2h) to object store. Serves live Prometheus data via StoreAPI for Thanos Query. |
| **Object Store (S3)** | Durable, cheap storage for TSDB blocks. Central repository for all clusters' metrics. |
| **Thanos Store Gateway** | Serves historical block data from object store via StoreAPI. Caches block index in memory for fast lookups. |
| **Thanos Compactor** | Singleton that merges overlapping blocks, downsamples to 5m and 1h resolutions, and enforces retention policies. |
| **Thanos Query** | Global query layer. Receives PromQL queries, fans out to Sidecars (live data) and Store Gateways (historical data), deduplicates HA replicas, merges results. |
| **Thanos Query Frontend** | Caching and query-splitting layer in front of Thanos Query. Splits long time ranges into sub-queries, caches results in memcached. |
| **Grafana** | Visualization and dashboarding. Connects to Thanos Query as a Prometheus-compatible data source. Supports dashboard-as-code via Grafonnet (Jsonnet library) or Terraform provider. |
| **Pushgateway** | Accepts pushed metrics from batch jobs and short-lived processes that cannot be scraped. Prometheus scrapes the Pushgateway. |

### Data Flows

1. **Scrape**: Prometheus → HTTP GET `/metrics` on each target → parse Prometheus exposition format → append to TSDB WAL → compact to 2h block.
2. **Long-term storage**: Thanos Sidecar → uploads completed 2h block to S3 → Compactor merges and downsamples.
3. **Query (recent)**: Grafana → Thanos Query → Thanos Sidecar → Prometheus TSDB (last 15 days).
4. **Query (historical)**: Grafana → Thanos Query → Thanos Store Gateway → S3 block data.
5. **Alerting**: Prometheus evaluates alert rules every 15s → fires to Alertmanager (separate system).
6. **Push**: Batch job → HTTP POST to Pushgateway → Prometheus scrapes Pushgateway.

---

## 4. Data Model

### Core Entities & Schema

**Prometheus Time Series:**
```
<metric_name>{<label_name>=<label_value>, ...}  <timestamp>  <value>

Examples:
node_cpu_seconds_total{cpu="0", mode="idle", instance="bm-042:9100", job="node-exporter"}  1712678625  45231.45
http_request_duration_seconds_bucket{le="0.1", method="POST", path="/api/jobs", service="job-scheduler"}  1712678625  12453
container_memory_usage_bytes{namespace="scheduling", pod="scheduler-7b4f9d", container="scheduler"}  1712678625  536870912
```

**Metric Types:**

| Type | Description | Example | PromQL Usage |
|---|---|---|---|
| **Counter** | Monotonically increasing value (resets on restart) | `http_requests_total`, `node_cpu_seconds_total` | `rate()`, `increase()` |
| **Gauge** | Value that can go up and down | `node_memory_available_bytes`, `kube_pod_status_ready` | Direct value, `deriv()` |
| **Histogram** | Observations bucketed by value | `http_request_duration_seconds` with `_bucket`, `_sum`, `_count` | `histogram_quantile()` |
| **Summary** | Client-side calculated quantiles | `go_gc_duration_seconds` with quantile labels | Direct quantile reading |

**Histogram Internals (critical for interviews):**
```
# Histogram metric with bucket boundaries
http_request_duration_seconds_bucket{le="0.005"} 2341
http_request_duration_seconds_bucket{le="0.01"}  4512
http_request_duration_seconds_bucket{le="0.025"} 8923
http_request_duration_seconds_bucket{le="0.05"}  12341
http_request_duration_seconds_bucket{le="0.1"}   15678
http_request_duration_seconds_bucket{le="0.25"}  17234
http_request_duration_seconds_bucket{le="0.5"}   17890
http_request_duration_seconds_bucket{le="1.0"}   18012
http_request_duration_seconds_bucket{le="+Inf"}  18100
http_request_duration_seconds_sum                 4523.45
http_request_duration_seconds_count               18100

# Calculate p99 latency:
histogram_quantile(0.99, rate(http_request_duration_seconds_bucket[5m]))

# How it works:
# 1. rate() computes per-second increase of each bucket counter over 5m
# 2. histogram_quantile() finds the bucket where cumulative count crosses 99%
# 3. Linearly interpolates within that bucket to estimate the value
# IMPORTANT: accuracy depends on bucket boundary placement!
```

### Database Selection

| Storage | Technology | Rationale |
|---|---|---|
| **Short-term (0-15d)** | Prometheus TSDB (local) | Native integration, fastest query for recent data, WAL for durability |
| **Long-term (15d-1y)** | Thanos + S3 | Cost-effective object storage, global query, downsampling |
| **Query cache** | Memcached (via Thanos Query Frontend) | Speeds up repeated dashboard queries, splits long range queries |
| **Alternative: VictoriaMetrics** | Single binary, Prometheus-compatible | Simpler operations than Thanos, better compression (~7x vs Prometheus ~1.5x), but less mature multi-tenancy |

**Why Prometheus TSDB over alternatives:**
- **vs. InfluxDB**: Prometheus has a stronger ecosystem (exporters, PromQL, Alertmanager), better Kubernetes integration, and the pull model provides automatic service discovery.
- **vs. ClickHouse for metrics**: ClickHouse is a general-purpose columnar DB. Prometheus TSDB is purpose-built for time-series with extreme compression (1-2 bytes/sample). ClickHouse would use 8-16 bytes/sample.
- **vs. Cortex**: Cortex is a horizontally scalable Prometheus-compatible store. More complex than Thanos (requires separate ingesters, distributors, compactors). Better for SaaS multi-tenancy. Thanos is simpler for our infrastructure-owned deployment.

### Indexing Strategy

**Prometheus TSDB Internals:**
```
TSDB Structure:
├── WAL (Write-Ahead Log)          ← Incoming samples, 2h segments
├── Head Block (in-memory)          ← Last 2 hours, actively written
├── Persisted Blocks (on disk)      ← Completed 2h blocks, immutable
│   ├── meta.json                   ← Block metadata, time range, series count
│   ├── chunks/                     ← Compressed sample data (XOR encoding for values,
│   │   └── 000001                     delta-of-delta for timestamps)
│   ├── index                       ← Inverted index: label → series IDs
│   └── tombstones                  ← Marks for deleted series
```

- **Inverted index**: For each label pair (e.g., `job="node-exporter"`), the index stores a posting list of series IDs. Multi-label queries intersect posting lists.
- **Compression**: Timestamps use delta-of-delta encoding (regular 15s intervals compress to 1 bit per sample). Values use XOR encoding with previous value (similar values compress well).
- **Block compaction**: TSDB merges adjacent blocks to reduce the number of blocks to scan during queries. Default compaction ranges: 2h → 6h → 18h → 54h.

---

## 5. API Design

### Query APIs

**PromQL Instant Query**
```
GET /api/v1/query?query=rate(http_requests_total{service="job-scheduler"}[5m])&time=2026-04-09T14:00:00Z

Response:
{
  "status": "success",
  "data": {
    "resultType": "vector",
    "result": [
      {
        "metric": {"service": "job-scheduler", "method": "POST", "path": "/api/jobs"},
        "value": [1712678400, "234.56"]  // [unix_timestamp, value_as_string]
      }
    ]
  }
}
```

**PromQL Range Query**
```
GET /api/v1/query_range?query=rate(http_requests_total{service="job-scheduler"}[5m])&start=2026-04-09T13:00:00Z&end=2026-04-09T14:00:00Z&step=60s

Response:
{
  "status": "success",
  "data": {
    "resultType": "matrix",
    "result": [
      {
        "metric": {"service": "job-scheduler", "method": "POST"},
        "values": [
          [1712674800, "210.23"],
          [1712674860, "215.67"],
          ...
        ]
      }
    ]
  }
}
```

**Key PromQL Patterns for Infrastructure:**
```promql
# CPU utilization per host (percentage)
100 - (avg by (instance) (rate(node_cpu_seconds_total{mode="idle"}[5m])) * 100)

# Memory utilization per host
1 - (node_memory_MemAvailable_bytes / node_memory_MemTotal_bytes)

# Pod restart rate (last hour)
increase(kube_pod_container_status_restarts_total[1h])

# HTTP request p99 latency
histogram_quantile(0.99, sum by (le, service) (rate(http_request_duration_seconds_bucket[5m])))

# Disk will be full in 4 hours (predictive)
predict_linear(node_filesystem_avail_bytes{mountpoint="/"}[6h], 4*3600) < 0

# Kafka consumer lag
kafka_consumergroup_lag_sum{consumergroup="cg-logstash-elasticsearch"}

# JVM heap utilization
jvm_memory_used_bytes{area="heap"} / jvm_memory_max_bytes{area="heap"}
```

### Ingestion APIs

**Prometheus Scrape (Pull Model)**
```
# Target exposes /metrics endpoint
GET http://bm-042:9100/metrics

Response (Prometheus exposition format):
# HELP node_cpu_seconds_total Seconds the CPUs spent in each mode.
# TYPE node_cpu_seconds_total counter
node_cpu_seconds_total{cpu="0",mode="idle"} 45231.45
node_cpu_seconds_total{cpu="0",mode="system"} 1234.56
node_cpu_seconds_total{cpu="0",mode="user"} 5678.90
# HELP node_memory_MemAvailable_bytes Memory available in bytes.
# TYPE node_memory_MemAvailable_bytes gauge
node_memory_MemAvailable_bytes 1.34217728e+10
```

**Prometheus Remote Write (Push to Thanos/VictoriaMetrics)**
```
POST /api/v1/receive
Content-Type: application/x-protobuf
Content-Encoding: snappy

// Protobuf payload containing TimeSeries messages:
// repeated TimeSeries {
//   repeated Label labels = 1;
//   repeated Sample samples = 2;
// }
```

**Pushgateway (for batch jobs)**
```
# Push a metric for a batch job
POST /metrics/job/data-migration/instance/batch-2026-04-09

# TYPE data_migration_rows_processed counter
data_migration_rows_processed 1500000
# TYPE data_migration_duration_seconds gauge
data_migration_duration_seconds 3456.78
# TYPE data_migration_last_success_timestamp gauge
data_migration_last_success_timestamp 1712678625
```

### Admin APIs

```
# Service Discovery (file-based or HTTP SD)
GET /api/v1/targets
Response: list of all scrape targets with status (up/down), last scrape time, error

# TSDB Status
GET /api/v1/status/tsdb
Response: series count, label names count, label value pairs, memory usage

# Rules (recording + alerting)
GET /api/v1/rules
Response: all rule groups with evaluation time, last evaluation, health

# Configuration Reload (no restart needed)
POST /-/reload

# Snapshot (for backup)
POST /api/v1/admin/tsdb/snapshot
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Prometheus TSDB Internals and Performance Tuning

**Why it's hard:**
At 78.5M active time series with 15-second resolution, Prometheus must ingest 5.2M samples/sec, maintain an inverted index for fast label-based lookups, compress data efficiently, and compact blocks in the background -- all while serving queries with sub-second latency. Understanding the TSDB architecture is critical for capacity planning and troubleshooting.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Single large Prometheus** | Simple operations | Memory-bound (~78.5M series x ~3KB = ~235 GB RAM); single point of failure |
| **Functional sharding** | Each Prometheus scrapes a subset (by job/cluster) | Manual configuration, uneven load, no global view without federation |
| **Hashmod sharding** | Automatic target distribution across Prometheus replicas | Complex config, label-based routing may not balance evenly, no built-in HA |
| **Thanos Receive (push-based)** | Stateless Prometheus + Thanos Receive for storage | More components, higher latency, but simpler Prometheus instances |

**Selected approach: Functional sharding per cluster + Thanos for global query**

Each Kubernetes cluster and each datacenter gets its own Prometheus HA pair (2 replicas). Thanos provides the global query layer.

**TSDB Memory Model:**
```
Memory consumption per Prometheus instance:
- Series in head block: ~3 KB per series (labels + chunks + index)
  - For 5M series (per-cluster Prometheus): 5M x 3 KB = 15 GB
- WAL: proportional to churn rate; typically 2-4 GB
- Query temporary memory: depends on query complexity; typically 2-8 GB
- Total recommended: 32 GB for 5M series, 64 GB for 10M series

Rule of thumb: 1 GB RAM per ~300K active series
```

**TSDB Write Path:**
1. Sample arrives → written to WAL (sequential append, fast).
2. Appended to Head Block (in-memory, last 2 hours of data).
3. Every 2 hours, Head Block is cut → new Head Block created, old one written to disk as a completed Block.
4. Completed Block uploaded to S3 by Thanos Sidecar.
5. Compactor merges adjacent blocks: 2h + 2h + 2h → 6h → 18h → 54h.

**TSDB Query Path:**
1. Parse PromQL expression.
2. Find matching series using inverted index (label → posting lists → series IDs).
3. For each matching series, load chunk data from Head Block (memory) and/or persisted Blocks (disk).
4. Decode compressed samples, apply PromQL functions (rate, sum, etc.).
5. Return result.

**Performance Tuning:**

| Tuning Parameter | Default | Recommended | Impact |
|---|---|---|---|
| `--storage.tsdb.retention.time` | 15d | 15d (with Thanos) | Longer retention = more disk, more blocks to scan |
| `--storage.tsdb.min-block-duration` | 2h | 2h | Shorter = more frequent compaction, more blocks |
| `--storage.tsdb.max-block-duration` | 36h (auto) | 2h (when using Thanos) | Must be 2h for Thanos upload; larger blocks improve query but delay upload |
| `--query.max-samples` | 50M | 50M | Limits memory per query; increase for high-cardinality queries |
| `--web.max-connections` | 512 | 512 | Max concurrent HTTP connections |

**Failure Modes:**
- **OOM kill**: Too many active series or a query loading too many samples. Solution: shard Prometheus, set `--query.max-samples`, limit scrape targets.
- **WAL corruption**: Power loss during WAL write. Prometheus replays WAL on startup; corrupted entries are skipped (minor data loss). Thanos uploads provide durability.
- **Slow compaction**: Large blocks cause compaction to take hours, blocking new block creation. Monitor `prometheus_tsdb_compactions_total` and `prometheus_tsdb_compaction_duration_seconds`.
- **Head series churn**: Kubernetes pod churn creates new series every restart. Old series remain in the head block until cut. Monitor `prometheus_tsdb_head_series` and `prometheus_tsdb_head_series_created_total`.

**Interviewer Q&As:**

**Q1: How does Prometheus achieve 1-2 bytes per sample compression?**
A: Prometheus uses Gorilla-style compression (Facebook's TSDB paper). Timestamps use delta-of-delta encoding: for regular 15s intervals, most deltas-of-deltas are zero, requiring only 1 bit. Values use XOR encoding: XOR with previous value, then encode leading zeros, significant bits, and trailing zeros. For slowly-changing metrics (e.g., memory total), XOR produces many leading zeros, compressing to a few bits. Combined, the average sample is 1-2 bytes vs. 16 bytes uncompressed (8-byte timestamp + 8-byte float).

**Q2: What is the difference between a Histogram and a Summary in Prometheus?**
A: **Histogram**: Counts observations into configurable buckets (e.g., `le="0.1"`, `le="0.5"`, `le="1.0"`). Quantiles are calculated server-side via `histogram_quantile()` PromQL function. Bucket boundaries are fixed at instrumentation time. Histograms are aggregatable (you can sum buckets across instances). **Summary**: Calculates quantiles client-side (e.g., phi=0.5, phi=0.99) using streaming algorithms. Quantile values are pre-computed and cannot be aggregated across instances (you cannot average p99 across pods). **Recommendation**: Use Histograms for almost all cases. Summaries are only useful when you need precise quantiles from a single instance and cannot define bucket boundaries.

**Q3: How do you handle high cardinality labels?**
A: High cardinality labels (e.g., `user_id`, `request_id`, `job_id`) create millions of unique series, exhausting Prometheus memory. Rules: (1) Never use unbounded values as label values. (2) Use relabel_configs to drop or hash high-cardinality labels before ingestion. (3) Set `sample_limit` per scrape config to reject targets exposing too many series. (4) Monitor `prometheus_target_scrape_pool_exceeded_target_limit_total`. (5) If you need per-user metrics, push to a column store (ClickHouse) instead of Prometheus.

**Q4: How does Prometheus handle target discovery in Kubernetes?**
A: Prometheus uses kubernetes_sd_config for automatic service discovery. It watches the Kubernetes API for Pods, Services, Endpoints, and Nodes. Discovery roles: `pod` (scrape pods with annotation `prometheus.io/scrape: true`), `service` (scrape services), `node` (scrape kubelets/Node Exporters), `endpoints` (scrape endpoints behind services). Relabel configs transform discovered metadata (pod name, namespace, labels) into Prometheus labels. This is fully dynamic -- new pods are discovered within one scrape interval (15s).

**Q5: How do you monitor Prometheus itself?**
A: Prometheus exposes its own metrics at `/metrics`. We configure a second, smaller Prometheus instance ("meta-Prometheus") to scrape the primary instances. Key self-monitoring metrics: `prometheus_tsdb_head_series` (cardinality), `prometheus_tsdb_head_chunks_created_total` (churn), `prometheus_engine_query_duration_seconds` (query performance), `up` (target health), `prometheus_tsdb_compactions_failed_total`, `prometheus_tsdb_wal_corruptions_total`. Alerts on these feed into a separate Alertmanager instance that pages on-call.

**Q6: What happens to queries during a Prometheus restart?**
A: During restart: (1) Scraping stops (targets miss ~1-2 scrape intervals = 15-30s gap). (2) Queries fail for the duration of restart. (3) On startup, Prometheus replays the WAL to reconstruct the Head Block (can take 1-10 minutes depending on series count). With HA pair: the second replica continues scraping and serving queries. Thanos Query automatically routes to the healthy replica. Thanos deduplication handles the overlap when both replicas are up.

---

### Deep Dive 2: Thanos Architecture for Global Query and Long-Term Storage

**Why it's hard:**
We have 200 Kubernetes clusters and 5 datacenters, each with its own Prometheus pair. Users need a global view: "show me the p99 latency for the job-scheduler across all clusters." This requires a system that can query across 400+ Prometheus instances, deduplicate HA replicas, handle mixed time ranges (recent from Prometheus, historical from S3), and return results quickly.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Prometheus Federation** | Built-in, simple | Only aggregated metrics, no raw data, scalability limit (~10 child instances), single point of failure |
| **Thanos** | Global query, long-term storage, HA dedup, open source | Additional components (sidecar, store, compactor, query), S3 dependency |
| **Cortex** | Horizontally scalable, multi-tenant | More complex (separate ingesters, distributors, rulers), originally designed for SaaS multi-tenancy |
| **VictoriaMetrics cluster** | Single binary, simpler than Thanos, better compression | Less mature HA story, smaller community |
| **Grafana Mimir** | Cortex successor, Grafana-native, horizontally scalable | Relatively newer, Grafana Labs-driven roadmap |

**Selected approach: Thanos**

**Thanos Component Deep Dive:**

**1. Thanos Sidecar:**
- Deployed as a sidecar container in each Prometheus pod.
- Watches Prometheus data directory for completed 2h blocks.
- Uploads blocks to S3 with metadata (external labels: `cluster`, `datacenter`, `replica`).
- Serves live Prometheus data via gRPC StoreAPI (for queries hitting recent data not yet uploaded).
- External labels are critical: they distinguish data from different Prometheus instances.

**2. Thanos Store Gateway:**
- Loads block metadata (index and chunk headers) from S3 into memory.
- Serves historical data via StoreAPI.
- Memory usage: ~1 MB per block for index cache. With 200 clusters x 12 blocks/day x 365 days = ~876K blocks, that is ~876 GB. Mitigation: use index cache in memcached (external), keep only hot blocks' index in memory.
- Disk cache: stores frequently accessed chunks on local SSD for faster repeated queries.

**3. Thanos Compactor:**
- Runs as a singleton (only one instance to avoid race conditions on S3).
- Operations:
  - **Vertical compaction**: Merges overlapping blocks from the same source (e.g., HA replica deduplication at storage level).
  - **Horizontal compaction**: Merges adjacent time-range blocks from the same source (2h + 2h → 4h → etc.) to reduce block count and improve query performance.
  - **Downsampling**: Creates 5-minute and 1-hour resolution copies. Original data is retained for the configured period (1 year), then only downsampled data remains.
  - **Retention enforcement**: Deletes blocks older than the configured retention.

**4. Thanos Query:**
- Receives PromQL queries (HTTP API compatible with Prometheus).
- Discovers all StoreAPI endpoints (Sidecars + Store Gateways) via DNS or file-based discovery.
- Fan-out: sends sub-queries to relevant stores based on time range and external labels.
- Deduplication: when two replicas (HA pair) return the same series, Thanos Query deduplicates by choosing one (based on `--query.replica-label=replica`).
- Partial response: if one store is unavailable, Query can return partial results with a warning (configurable: `--query.partial-response`).

**5. Thanos Query Frontend:**
- Sits in front of Thanos Query.
- **Query splitting**: Splits a 7-day query into seven 1-day sub-queries, executes in parallel, merges results.
- **Result caching**: Caches sub-query results in memcached. Subsequent queries hitting the same time range serve from cache.
- **Retry and deduplication**: Retries failed sub-queries and deduplicates overlapping results.

**Failure Modes:**
- **S3 outage**: Store Gateway cannot serve historical data. Thanos Query returns partial results (recent data from Sidecars only). Dashboards degrade gracefully for long time ranges.
- **Compactor stuck**: Blocks accumulate in S3, Store Gateway loads more blocks, memory pressure increases. Monitor `thanos_compact_group_compactions_failures_total`. Fix: investigate S3 issues, increase Compactor resources.
- **Store Gateway OOM**: Too many blocks' indices in memory. Solution: add memcached for external index caching, limit retention, add more Store Gateway replicas with sharding (hash on block labels).
- **Query timeout**: Complex queries spanning many stores timeout. Solution: use recording rules to pre-compute expensive queries, split dashboards into smaller time ranges.

**Interviewer Q&As:**

**Q1: How does Thanos deduplication work for HA Prometheus pairs?**
A: Each Prometheus in an HA pair has an external label `replica: A` or `replica: B`. Both scrape the same targets, producing nearly identical data (minor timestamp differences). When Thanos Query receives results from both replicas, it merges the series: for each timestamp, it selects the sample from one replica (deterministic choice: usually replica A, falling back to replica B if A is missing that sample). The `--query.replica-label=replica` flag tells Thanos which label distinguishes replicas. The user sees one clean series with no gaps.

**Q2: How does the Compactor handle overlapping blocks from HA pairs?**
A: The Compactor can perform vertical compaction (deduplication at the storage level). It detects blocks from the same source (same external labels except `replica`) with overlapping time ranges. It merges them into a single block, choosing one sample per timestamp (like Query dedup). This reduces storage cost by ~50% (eliminating the HA duplicate). However, vertical compaction is optional and resource-intensive; many deployments rely on Query-time dedup only.

**Q3: How do you handle cross-cluster queries efficiently?**
A: (1) Recording rules in each cluster pre-compute common aggregations (e.g., `sum by (service) (rate(http_requests_total[5m]))`). Thanos uploads these as regular series. Cross-cluster queries on pre-aggregated data hit fewer series. (2) Query Frontend splits time ranges and caches sub-results. (3) Thanos Query uses `--store.sd-dns-interval=30s` for fast store discovery. (4) We add `cluster` as an external label and use it in queries to scope to specific clusters when global view is not needed.

**Q4: What is the cost model for long-term metrics storage with Thanos?**
A: Storage cost breakdown for 1 year: (1) S3 storage: 246 TB raw + 37 TB downsampled = 283 TB. At $0.023/GB/month (S3 Standard), that is ~$6,500/month. (2) Store Gateway compute: 10 instances x $500/month = $5,000/month. (3) Compactor: 1 large instance, ~$1,000/month. (4) S3 API costs: GET requests for queries, PUT for uploads. At our scale, ~$500/month. Total: ~$13,000/month for 1 year of metrics for the entire fleet. This is roughly 100x cheaper than keeping all data in Prometheus local storage (which would require ~246 TB of SSD).

**Q5: How does Thanos Query Frontend caching work?**
A: The Query Frontend splits a range query `[T1, T2]` into aligned sub-ranges (e.g., 24-hour chunks). Each sub-range result is cached in memcached with a key derived from the query hash, time range, and step. On subsequent requests, only uncached (typically the most recent) sub-ranges are forwarded to Thanos Query. For a 7-day dashboard that auto-refreshes, only the last day's data is re-queried; the first 6 days are served from cache. Cache invalidation: results for incomplete time ranges (current day) have a short TTL (5 minutes); completed ranges have a long TTL (24 hours).

**Q6: Compare Thanos vs. VictoriaMetrics for long-term storage.**
A: **Thanos**: (1) Prometheus-native (uses TSDB blocks as-is). (2) Requires multiple components (Sidecar, Store, Query, Compactor). (3) S3-backed, cost-effective. (4) Well-proven at large scale (used by GitLab, Red Hat, Zalando). **VictoriaMetrics**: (1) Single binary (vmselect, vminsert, vmstorage for cluster mode). (2) Better compression (7x vs Prometheus 1.5x) - uses different encoding. (3) Faster queries on large datasets. (4) Less operational complexity. (5) Smaller community, single-company project risk. We chose Thanos because: (a) it reuses Prometheus TSDB blocks, minimizing risk; (b) the Thanos community is large and well-supported; (c) our team has more experience with it. VictoriaMetrics would be a valid alternative, especially if starting fresh.

---

### Deep Dive 3: Java Metrics Instrumentation with Micrometer

**Why it's hard:**
Java services are the backbone of the platform (job scheduler, resource manager, API gateway). Instrumenting them correctly requires understanding Micrometer's abstraction layer, Prometheus-specific registry behavior, histogram bucket configuration, JVM metrics, and Spring Boot Actuator integration. Poor instrumentation leads to high cardinality, misleading percentiles, or missing critical signals.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Prometheus Java client directly** | Lightweight, no abstraction | Vendor lock-in, no auto-configuration with Spring Boot |
| **Micrometer + Prometheus registry** | Vendor-neutral abstraction, Spring Boot auto-config, rich metrics | Slight overhead of abstraction layer |
| **OpenTelemetry Metrics** | Unified with traces and logs | Java metrics API still stabilizing, less mature than Micrometer for Prometheus |
| **Dropwizard Metrics** | Battle-tested, simple API | No native Prometheus support, requires bridge, less active development |

**Selected approach: Micrometer + Prometheus MeterRegistry + Spring Boot Actuator**

**Implementation Detail:**

```java
// build.gradle
dependencies {
    implementation 'org.springframework.boot:spring-boot-starter-actuator'
    implementation 'io.micrometer:micrometer-registry-prometheus'
}
```

```yaml
# application.yml
management:
  endpoints:
    web:
      exposure:
        include: prometheus, health, info
  metrics:
    tags:
      service: job-scheduler
      environment: production
    distribution:
      percentiles-histogram:
        http.server.requests: true  # Enable histogram for HTTP metrics
      slo:
        http.server.requests: 50ms, 100ms, 200ms, 500ms, 1s, 5s
      minimum-expected-value:
        http.server.requests: 1ms
      maximum-expected-value:
        http.server.requests: 30s
```

```java
// Custom business metrics
@Component
public class JobSchedulerMetrics {
    private final Counter jobsSubmitted;
    private final Counter jobsFailed;
    private final Timer jobSchedulingLatency;
    private final Gauge activeJobs;
    private final DistributionSummary resourceUtilization;

    public JobSchedulerMetrics(MeterRegistry registry) {
        this.jobsSubmitted = Counter.builder("jobs.submitted.total")
            .description("Total number of jobs submitted")
            .tag("service", "job-scheduler")
            .register(registry);

        this.jobsFailed = Counter.builder("jobs.failed.total")
            .description("Total number of jobs that failed")
            .tag("service", "job-scheduler")
            .register(registry);

        this.jobSchedulingLatency = Timer.builder("jobs.scheduling.duration")
            .description("Time to schedule a job")
            .publishPercentileHistogram()  // Prometheus histogram buckets
            .serviceLevelObjectives(
                Duration.ofMillis(100),
                Duration.ofMillis(500),
                Duration.ofSeconds(1),
                Duration.ofSeconds(5)
            )
            .register(registry);

        this.activeJobs = Gauge.builder("jobs.active", jobStore, JobStore::getActiveCount)
            .description("Number of currently active jobs")
            .register(registry);

        this.resourceUtilization = DistributionSummary.builder("resource.utilization.ratio")
            .description("Resource pool utilization ratio")
            .baseUnit("ratio")
            .scale(100)  // Convert to percentage
            .register(registry);
    }

    public void recordJobSubmission(Job job) {
        jobsSubmitted.increment();
    }

    public void recordJobFailure(Job job, Exception e) {
        jobsFailed.increment(
            Tags.of("error_type", e.getClass().getSimpleName(),
                     "priority", job.getPriority().name())
        );
    }

    public Timer.Sample startSchedulingTimer() {
        return Timer.start();
    }

    public void recordSchedulingDuration(Timer.Sample sample) {
        sample.stop(jobSchedulingLatency);
    }
}
```

**JVM Metrics (auto-configured by Micrometer):**
```
# JVM Memory
jvm_memory_used_bytes{area="heap",id="G1 Old Gen"} 1073741824
jvm_memory_max_bytes{area="heap",id="G1 Old Gen"} 4294967296
jvm_memory_used_bytes{area="nonheap",id="Metaspace"} 67108864

# JVM GC
jvm_gc_pause_seconds_count{action="end of minor GC",cause="G1 Evacuation Pause"} 234
jvm_gc_pause_seconds_sum{action="end of minor GC",cause="G1 Evacuation Pause"} 4.567
jvm_gc_pause_seconds_max{action="end of minor GC",cause="G1 Evacuation Pause"} 0.123

# JVM Threads
jvm_threads_live_threads 156
jvm_threads_daemon_threads 134
jvm_threads_peak_threads 178

# JVM Classes
jvm_classes_loaded_classes 12345

# HikariCP Connection Pool
hikaricp_connections_active{pool="HikariPool-1"} 5
hikaricp_connections_idle{pool="HikariPool-1"} 15
hikaricp_connections_max{pool="HikariPool-1"} 20
hikaricp_connections_timeout_total{pool="HikariPool-1"} 0
```

**Failure Modes:**
- **Cardinality explosion**: A tag with unbounded values (e.g., `job_id` as a tag on a Counter) creates a new time series per job. With millions of jobs, this crashes Prometheus. Solution: use bounded tags only (e.g., `priority`, `status`, `resource_type`), never IDs.
- **Histogram bucket mismatch**: Default histogram buckets may not match the application's latency distribution. If all requests take 200-500ms but buckets are `[0.01, 0.1, 1.0, 10.0]`, the p99 will be inaccurate (interpolation across a wide 0.1-1.0 bucket). Solution: configure `slo` or `publishPercentiles` with application-appropriate boundaries.
- **Memory leak from meters**: Registering new meters in a loop (e.g., `Counter.builder(...).tag("id", uniqueId).register()`) without cleanup causes unbounded meter growth. Micrometer's `MeterFilter.maximumAllowableMetrics()` can cap total meters.

**Interviewer Q&As:**

**Q1: How does Micrometer's Timer work internally for Prometheus?**
A: A Micrometer Timer with `publishPercentileHistogram()` creates a Prometheus Histogram under the hood. It maintains: (1) a set of cumulative counters for configurable time buckets (le labels), (2) a `_count` counter (total observations), (3) a `_sum` counter (total observed time). When `timer.record(duration)`, Micrometer increments the appropriate bucket counter(s) and updates sum/count. Prometheus scrapes these counters, and `histogram_quantile()` in PromQL calculates percentiles by interpolating between buckets.

**Q2: How do you instrument asynchronous code (CompletableFuture, reactive)?**
A: For `CompletableFuture`: wrap the call with `Timer.Sample sample = Timer.start(registry)` before the async call, and `sample.stop(timer)` in the `.whenComplete()` callback. For reactive (WebFlux): Micrometer provides `MicrometerObservation` integration with Reactor's context. Spring Boot 3 uses the Observation API which propagates through reactive chains. The key is ensuring the timer starts before the async operation and stops when the actual work completes, not when the future is created.

**Q3: How do you handle metric cardinality in a multi-tenant system?**
A: (1) Use `MeterFilter.maximumAllowableTags()` to cap unique tag values per metric. (2) Use `MeterFilter.deny()` to block metrics from noisy libraries. (3) For tenant-specific metrics, use a bounded tenant_tier tag (e.g., "free", "premium", "enterprise") rather than tenant_id. (4) For per-tenant reporting, push per-tenant metrics to a column store (ClickHouse) that handles high cardinality natively, not Prometheus.

**Q4: What is the difference between `publishPercentiles()` and `publishPercentileHistogram()`?**
A: `publishPercentiles(0.5, 0.95, 0.99)` computes percentiles client-side and exports them as Summary-type metrics (phi labels). These are pre-computed and cannot be aggregated across instances. `publishPercentileHistogram()` exports Histogram-type metrics with `le` bucket labels. Percentiles are computed server-side via PromQL's `histogram_quantile()` and can be aggregated across instances. Always prefer `publishPercentileHistogram()` for Prometheus.

**Q5: How do you expose custom Spring Boot health as a metric?**
A: Spring Boot Actuator's `/health` endpoint reports UP/DOWN status. Micrometer exports this as `management_health_status{status="UP"} 1.0`. For custom health checks: implement `HealthIndicator` and register it as a Spring bean. Example: `DatabaseHealthIndicator` checks MySQL connection, `KafkaHealthIndicator` checks Kafka producer. These appear in both `/health` and as Prometheus metrics, enabling alerting on service health.

**Q6: How do you prevent /metrics endpoint from being a performance bottleneck?**
A: The `/metrics` endpoint serializes all registered meters to Prometheus exposition format on each scrape. At 10K+ meters, this can take 50-200ms. Optimizations: (1) Micrometer caches meter data between scrapes (configurable `step` duration). (2) Scrape interval of 15s is fast enough. (3) Use `management.metrics.enable.*` to disable unused metrics. (4) If the `/metrics` response exceeds 10 MB (rare), consider splitting into multiple scrape endpoints or using OpenMetrics protobuf format (more compact). (5) Ensure the `/metrics` endpoint is on a separate port (management port) from application traffic to avoid contention.

---

### Deep Dive 4: Grafana Dashboard-as-Code and Templating

**Why it's hard:**
With 200 clusters, thousands of services, and hundreds of engineers, dashboards must be standardized, version-controlled, and automatically provisioned. Manual dashboard creation in the Grafana UI leads to snowflake dashboards, inconsistent panels, and configuration drift. Dashboard-as-code using Grafonnet (Jsonnet) or Terraform ensures reproducibility and code review.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Manual UI creation** | Easy for one-off dashboards | Not version-controlled, no review process, drift |
| **JSON export/import** | Dashboards are JSON, can be stored in Git | Raw JSON is verbose and unreadable (3000+ line files), hard to review diffs |
| **Grafonnet (Jsonnet library)** | Composable, DRY, readable, generates JSON | Jsonnet learning curve, tooling maturity |
| **Terraform Grafana provider** | Declarative, integrates with existing IaC | Less flexible for complex dashboard logic |
| **Grafana Dashboard provisioning (YAML)** | File-based provisioning on startup | Static, requires Grafana restart for changes |

**Selected approach: Grafonnet (Jsonnet) + CI/CD pipeline + Grafana provisioning API**

**Implementation Detail:**

```jsonnet
// dashboards/job-scheduler.jsonnet
local grafana = import 'grafonnet/grafana.libsonnet';
local dashboard = grafana.dashboard;
local prometheus = grafana.prometheus;
local graphPanel = grafana.graphPanel;
local row = grafana.row;
local template = grafana.template;

local ds = 'Thanos';  // Data source name

dashboard.new(
  'Job Scheduler Overview',
  tags=['job-scheduler', 'platform', 'auto-generated'],
  refresh='30s',
  time_from='now-6h',
)
.addTemplate(
  template.datasource('datasource', 'prometheus', ds)
)
.addTemplate(
  template.new('cluster', ds, 'label_values(up{job="job-scheduler"}, cluster)',
    refresh='time', includeAll=true, multi=true)
)
.addTemplate(
  template.new('namespace', ds, 'label_values(up{job="job-scheduler", cluster=~"$cluster"}, namespace)',
    refresh='time')
)
.addRow(
  row.new('Request Rate & Latency')
  .addPanel(
    graphPanel.new(
      'Request Rate',
      datasource=ds,
      span=6,
      format='ops',
    )
    .addTarget(prometheus.target(
      'sum(rate(http_server_requests_seconds_count{service="job-scheduler", cluster=~"$cluster"}[5m])) by (method, status)',
      legendFormat='{{method}} {{status}}'
    ))
  )
  .addPanel(
    graphPanel.new(
      'Request Latency (p50, p95, p99)',
      datasource=ds,
      span=6,
      format='s',
    )
    .addTarget(prometheus.target(
      'histogram_quantile(0.50, sum(rate(http_server_requests_seconds_bucket{service="job-scheduler", cluster=~"$cluster"}[5m])) by (le))',
      legendFormat='p50'
    ))
    .addTarget(prometheus.target(
      'histogram_quantile(0.95, sum(rate(http_server_requests_seconds_bucket{service="job-scheduler", cluster=~"$cluster"}[5m])) by (le))',
      legendFormat='p95'
    ))
    .addTarget(prometheus.target(
      'histogram_quantile(0.99, sum(rate(http_server_requests_seconds_bucket{service="job-scheduler", cluster=~"$cluster"}[5m])) by (le))',
      legendFormat='p99'
    ))
  )
)
.addRow(
  row.new('Job Metrics')
  .addPanel(
    graphPanel.new('Active Jobs', datasource=ds, span=4, format='short')
    .addTarget(prometheus.target('jobs_active{cluster=~"$cluster"}', legendFormat='{{cluster}}'))
  )
  .addPanel(
    graphPanel.new('Job Submission Rate', datasource=ds, span=4, format='ops')
    .addTarget(prometheus.target('sum(rate(jobs_submitted_total{cluster=~"$cluster"}[5m]))', legendFormat='submissions/s'))
  )
  .addPanel(
    graphPanel.new('Job Failure Rate', datasource=ds, span=4, format='ops')
    .addTarget(prometheus.target('sum(rate(jobs_failed_total{cluster=~"$cluster"}[5m])) by (error_type)', legendFormat='{{error_type}}'))
  )
)
```

**CI/CD Pipeline:**
```yaml
# .github/workflows/dashboards.yml
name: Deploy Grafana Dashboards
on:
  push:
    paths: ['dashboards/**']
steps:
  - name: Lint Jsonnet
    run: jsonnetfmt --test dashboards/*.jsonnet
  - name: Generate JSON
    run: |
      for f in dashboards/*.jsonnet; do
        jsonnet -J vendor "$f" > "generated/$(basename $f .jsonnet).json"
      done
  - name: Validate JSON
    run: |
      for f in generated/*.json; do
        python scripts/validate_dashboard.py "$f"
      done
  - name: Deploy to Grafana
    run: |
      for f in generated/*.json; do
        curl -X POST -H "Authorization: Bearer $GRAFANA_API_KEY" \
          -H "Content-Type: application/json" \
          -d @"$f" \
          "$GRAFANA_URL/api/dashboards/db"
      done
```

**Failure Modes:**
- **Dashboard breaks after Prometheus relabeling change**: A metric name changes (e.g., `http_requests_total` → `http_server_requests_seconds_count` in Spring Boot 3). All dashboards referencing the old name break. Solution: recording rules that alias old names during migration.
- **Grafana OOM with heavy dashboards**: A dashboard with 50 panels, each running a high-cardinality query, can overwhelm Grafana. Solution: limit panels per dashboard, use recording rules for expensive queries, set `maxDataPoints` on panels.

**Interviewer Q&As:**

**Q1: How do you handle dashboard versioning and rollback?**
A: All dashboards are Jsonnet files in Git. Grafana's built-in versioning provides UI-level history, but our source of truth is Git. To roll back: revert the Git commit and re-run the CI/CD pipeline. Grafana's API accepts dashboard JSON with `overwrite: true` to replace.

**Q2: How do you standardize dashboards across teams?**
A: We provide a Jsonnet library (`infra-dashboards-lib`) with reusable components: standard row layouts (request rate, latency, error rate -- the "RED method"), common template variables (cluster, namespace, service), and standard alert annotations. Teams import this library and compose service-specific dashboards. The library enforces naming conventions, panel sizes, and query patterns.

**Q3: How do you handle Grafana at scale (200+ concurrent users)?**
A: (1) Grafana is stateless (stores dashboards in PostgreSQL, not local). Deploy 5+ Grafana replicas behind a load balancer. (2) Enable server-side query caching (Grafana Enterprise) or use Thanos Query Frontend caching. (3) Set `max_concurrent_datasource_requests` to limit backend load per user. (4) Use proxy mode (not direct mode) for Prometheus data source to avoid exposing Prometheus directly.

**Q4: How do you migrate from manually-created dashboards to dashboard-as-code?**
A: (1) Export existing dashboards as JSON via Grafana API. (2) Use `grafana-dashboard-linter` to normalize JSON. (3) Convert critical dashboards to Jsonnet manually (Jsonnet is more maintainable). (4) For less critical dashboards, store the raw JSON in Git. (5) Set Grafana to provisioning mode (read-only in UI) to prevent manual edits that would drift from Git.

**Q5: How do you implement SLO dashboards?**
A: Following the Google SRE book, we define SLOs as PromQL expressions: `availability_ratio = 1 - (sum(rate(http_server_requests_seconds_count{status=~"5.."}[30d])) / sum(rate(http_server_requests_seconds_count[30d])))`. We display: (1) Current SLO percentage (e.g., "99.95% of 99.9% target"), (2) Error budget remaining (absolute count and percentage), (3) Error budget burn rate (using multi-window burn rate), (4) Time until error budget exhaustion at current burn rate. These are computed via recording rules for efficiency and displayed on a standardized SLO dashboard template.

**Q6: What is the RED method and how does it map to dashboards?**
A: RED stands for Rate (requests per second), Errors (error rate), Duration (latency). Every service dashboard has these three panels as the first row. PromQL: Rate = `sum(rate(http_server_requests_seconds_count[5m]))`. Errors = `sum(rate(http_server_requests_seconds_count{status=~"5.."}[5m]))`. Duration = `histogram_quantile(0.99, sum(rate(http_server_requests_seconds_bucket[5m])) by (le))`. The USE method (Utilization, Saturation, Errors) is used for infrastructure resources (CPU, memory, disk). Every host dashboard uses USE.

---

## 7. Scaling Strategy

### Scaling Dimensions

| Component | Scaling Mechanism | Trigger |
|---|---|---|
| **Prometheus** | Functional sharding (one per cluster/DC) | Cardinality exceeds ~10M series per instance |
| **Thanos Store Gateway** | Add replicas with block-level sharding | Query latency increasing, memory pressure |
| **Thanos Query** | Horizontal replicas behind LB | Concurrent query count, CPU utilization |
| **Thanos Query Frontend** | Horizontal replicas | Request rate |
| **Thanos Compactor** | Vertical scaling (more CPU/memory) | Compaction backlog growing |
| **Grafana** | Horizontal replicas (stateless) | Concurrent users > 50 per instance |
| **Object Store (S3)** | Infinite (managed service) | N/A |
| **Memcached (cache)** | Add nodes to consistent hash ring | Cache miss rate > 50% |

**Interviewer Q&As:**

**Q1: How do you handle 50 million active time series with Prometheus?**
A: No single Prometheus can handle 50M series (would require ~150 GB RAM). We shard: each Kubernetes cluster gets its own Prometheus pair (~250K-2.5M series per cluster depending on cluster size). Bare-metal hosts are grouped by datacenter, each DC gets its own Prometheus pair. Total: ~200 cluster Prometheus + 10 DC Prometheus = ~210 Prometheus pairs (420 instances including HA). Thanos Query provides the global view. Per-instance series count stays under 5M.

**Q2: What happens when a new Kubernetes cluster is added?**
A: (1) Cluster provisioning automation (Terraform/Ansible) deploys Prometheus + Thanos Sidecar as part of the cluster bootstrap. (2) External labels `cluster: k8s-prod-new-cluster` and `datacenter: us-east-1` are configured. (3) Thanos Sidecar registers with the Thanos Query discovery mechanism (DNS-based SD or service mesh). (4) Grafana template variables auto-discover the new cluster label value. No manual configuration needed.

**Q3: How do you prevent a single expensive query from affecting all users?**
A: (1) Thanos Query has `--query.max-concurrent` (default 20) to limit parallel queries. (2) Query timeout: `--query.timeout=2m`. (3) Thanos Query Frontend has per-tenant rate limiting (when multi-tenant via header-based tenant ID). (4) Grafana has `max_concurrent_datasource_requests` per user. (5) For very expensive queries (e.g., `sum(...) by (instance)` across all 50K hosts), use recording rules to pre-compute.

**Q4: How do you scale metrics collection for 50,000 bare-metal hosts?**
A: Each datacenter has a Prometheus pair that scrapes all hosts in that DC. For a DC with 10,000 hosts at 500 metrics each = 5M series -- near the limit of a single Prometheus. Options: (1) Shard within the DC by rack/subnet using hashmod relabeling. (2) Use Prometheus Agent mode (scrape-only, no local TSDB) which has lower memory footprint, and ship to Thanos Receive. (3) Use VictoriaMetrics vmagent which handles 10M+ series with lower resource usage than Prometheus.

**Q5: How do you handle metric cardinality growth over time?**
A: (1) Monitor `prometheus_tsdb_head_series` across all instances -- alert if any instance exceeds 80% of capacity target. (2) Use `topk(10, count by (__name__) ({__name__=~".+"}))` to find the highest-cardinality metrics. (3) Audit new service onboardings -- require metric review as part of service launch checklist. (4) Automated cardinality limiter: Prometheus relabel configs with `hashmod` and `keep` to sample high-cardinality metrics. (5) Set `sample_limit` per scrape target to hard-cap cardinality.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| **Single Prometheus down (in HA pair)** | No impact (other replica continues) | `up` metric, Thanos Sidecar health | HA pair. Thanos deduplicates when both recover. | 0 (transparent) |
| **Both Prometheus in HA pair down** | Metrics gap for that cluster; alerts stop evaluating | Thanos Query returns no data for that cluster; meta-Prometheus alerts | Kubernetes restarts pods. WAL replay on start. Gap in data is expected. | ~5 min |
| **Thanos Store Gateway down** | Historical queries fail; recent data (from Sidecars) still available | Thanos Query logs "store unavailable" | Multiple Store Gateway replicas with block sharding. | ~30 sec (LB failover) |
| **Thanos Compactor down** | Blocks accumulate in S3; no downsampling | Growing block count, compaction backlog metric | Singleton; restart fixes it. Backlog is processed on recovery. | ~5 min |
| **S3 outage** | No block uploads; historical queries fail | Sidecar upload errors, Store Gateway errors | Prometheus retains 15d locally. Sidecar retries uploads. | Depends on S3 |
| **Grafana down** | No dashboards; users cannot visualize metrics | Health check, HTTP 503 | Multiple replicas, PostgreSQL backend for state. | ~30 sec |
| **Memcached (cache) down** | Query Frontend cache miss → all queries hit Thanos Query | Cache hit rate drops to 0% | Graceful degradation; queries are slower but functional. Add cache nodes. | ~1 min |
| **Network partition (Prometheus ↔ targets)** | Scrape failures; targets show as DOWN | `up == 0` alerts, `scrape_duration_seconds` anomalies | HA pair in different failure domains; one likely still reaches targets. | Self-healing |
| **Prometheus disk full** | TSDB stops accepting samples; WAL grows until crash | Disk usage alert at 80% | Reduce retention, increase disk, or add more Prometheus shards. | ~15 min |
| **Cardinality explosion from bad deploy** | Prometheus OOM | `prometheus_tsdb_head_series` spike | Emergency: restart Prometheus, add relabel config to drop the offending metric. | ~10 min |

---

## 9. Security

### Authentication & Authorization
- **Prometheus**: No built-in auth. Secured via network policy (only Thanos Sidecar and Grafana can reach it). TLS with mutual auth for inter-component communication.
- **Thanos**: TLS + mTLS between all components. Query layer can add auth via reverse proxy (OAuth2 proxy).
- **Grafana**: SAML/OIDC SSO with corporate IdP. Role-based access: Viewer, Editor, Admin. Org/Team-level isolation for multi-tenancy.
- **Metrics endpoint security**: Service mesh (Istio) or NetworkPolicy ensures only Prometheus can scrape `/metrics` endpoints. Sensitive metrics (e.g., database credentials in labels) are stripped via relabel_configs.

### Encryption
- In transit: TLS 1.3 for all HTTP and gRPC communication.
- At rest: S3 server-side encryption (SSE-S3 or SSE-KMS). Prometheus local TSDB on encrypted volumes.

### Metric Data Sensitivity
- Metrics rarely contain PII (they are numeric time series). However, label values could inadvertently include sensitive info.
- Relabel configs strip known sensitive labels before storage.
- Access to raw Prometheus or Thanos Query is restricted to platform team; users access through Grafana with RBAC.

---

## 10. Incremental Rollout

### Rollout Phases

| Phase | Scope | Duration | Success Criteria |
|---|---|---|---|
| **Phase 0: Proof of Concept** | 1 cluster, Prometheus + Thanos + Grafana | 2 weeks | Metrics visible in Grafana, Thanos query works |
| **Phase 1: Infrastructure Monitoring** | All bare-metal hosts (Node Exporter) + 5 K8s clusters | 4 weeks | All hosts reporting, standard dashboards deployed |
| **Phase 2: Application Metrics** | Java/Python services instrumented with Micrometer | 4 weeks | RED dashboards for all critical services |
| **Phase 3: Full Fleet** | All 200 K8s clusters, all services | 8 weeks | Global Thanos query <5s, all teams self-serve Grafana |
| **Phase 4: Long-term & Advanced** | Thanos Compactor downsampling, SLO dashboards, recording rules | 4 weeks | 1-year historical queries working, SLO tracking |

### Rollout Q&As

**Q1: How do you migrate from an existing monitoring system (e.g., Nagios, Datadog)?**
A: Parallel operation. Deploy Prometheus alongside existing system. Both collect metrics for a validation period (4 weeks). Compare alert accuracy and data quality. Gradually shift alerting rules from old system to Prometheus/Alertmanager. Decommission old system only when all teams have validated Prometheus-based dashboards. Keep old system read-only for 90 days for historical reference.

**Q2: How do you onboard a Java service to the metrics platform?**
A: (1) Add Micrometer + Prometheus registry dependencies (one `build.gradle` line). (2) Spring Boot auto-configures JVM metrics and HTTP metrics. (3) Add custom business metrics using Micrometer API. (4) Deploy. Prometheus auto-discovers via kubernetes_sd_config and `prometheus.io/scrape: true` annotation. (5) Import the standard service dashboard from the Grafonnet library. (6) Total effort: ~2 hours for basic metrics, ~1 day for custom business metrics.

**Q3: How do you validate that metrics are accurate after migration?**
A: (1) Compare Prometheus data with OS-level tools: `node_cpu_seconds_total` should match `mpstat` output. (2) Compare HTTP request counts with access log line counts. (3) Compare latency percentiles with application-level APM data. (4) Run synthetic load tests with known patterns and verify metrics match expected values. (5) Cross-reference with existing monitoring system during parallel operation.

**Q4: How do you handle rollback if Thanos has issues?**
A: Each Prometheus instance retains 15 days of data locally. If Thanos Query or Store Gateway has issues, we point Grafana directly to individual Prometheus instances (one data source per cluster). This loses the global view but maintains per-cluster visibility. Thanos issues are typically in the query path, not the storage path -- data continues uploading to S3 even if Query is down.

**Q5: How do you train 200+ engineers to use the new monitoring platform?**
A: (1) Self-service documentation with PromQL cookbook (common queries for infra engineers). (2) Pre-built dashboard templates that work out of the box for standard services. (3) "Monitoring as Code" workshops (2-hour hands-on sessions). (4) Slack channel with bot that converts natural language to PromQL. (5) Office hours with the platform monitoring team weekly for the first 3 months.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale |
|---|---|---|---|
| **Metric collection model** | Pull (Prometheus), Push (StatsD/Graphite) | Pull (Prometheus) | Automatic service discovery, target health detection, no agent push config, Kubernetes-native |
| **Long-term storage** | Thanos, Cortex, VictoriaMetrics, Mimir | Thanos | Reuses Prometheus TSDB blocks, simpler than Cortex, proven at scale |
| **Visualization** | Grafana, Kibana, Datadog | Grafana | Open source, Prometheus-native, dashboard-as-code, extensive plugin ecosystem |
| **Dashboard authoring** | Grafana UI, JSON, Grafonnet, Terraform | Grafonnet (Jsonnet) | Composable, DRY, code review via Git, reusable library components |
| **Java instrumentation** | Prometheus client, Micrometer, OTel Metrics | Micrometer | Vendor-neutral abstraction, Spring Boot auto-config, mature ecosystem |
| **HA strategy** | Single Prometheus + remote write, HA pair + Thanos dedup | HA pair + Thanos dedup | Simple, battle-tested, no special remote write receiver needed |
| **Scrape interval** | 5s, 10s, 15s, 30s | 15s (5s for critical) | Balance between resolution and cardinality/storage cost |
| **Histogram vs Summary** | Histogram, Summary | Histogram | Aggregatable across instances, server-side percentile calculation |
| **Downsampling retention** | 1y raw only, 1y raw + 3y 5m | 1y raw + 3y 5m + 5y 1h | Capacity planning needs multi-year trends; downsampled data is cheap |

---

## 12. Agentic AI Integration

### AI-Powered Metrics Analysis

**Use Case 1: Natural Language to PromQL**
```
User: "What is the average CPU usage across all hosts in us-east-1 over the last 24 hours?"

AI Agent → Generates PromQL:
avg(1 - rate(node_cpu_seconds_total{mode="idle", datacenter="us-east-1"}[5m]))

→ Executes via Thanos Query API
→ Returns: "Average CPU utilization in us-east-1 over the last 24 hours was 42.3%,
   with a peak of 78.1% at 14:30 UTC during the batch job window."
```

**Use Case 2: Automated Capacity Planning**
```
AI Agent analyzes historical trends:
1. Query: predict_linear(node_filesystem_avail_bytes[30d], 90*86400)
2. Identifies hosts where disk will be full in <90 days
3. Groups by datacenter, rack, and workload type
4. Generates capacity planning report:
   "23 hosts in us-east-1 will exhaust disk space within 90 days.
    Root cause: log growth from job-scheduler (40% of disk usage).
    Recommendation: (a) Increase log sampling to reduce disk growth,
    (b) Add 4TB to each host, (c) Move logs to remote storage."
5. Creates Jira tickets for the infrastructure team
```

**Use Case 3: Anomaly Correlation Across Metrics**
```
AI Agent monitors for correlated anomalies:
1. Detects: HTTP latency spike for job-scheduler at 14:20
2. Correlates with: (a) CPU spike on hosts bm-040 through bm-050 at 14:18
                     (b) MySQL slow queries at 14:19
                     (c) Kafka consumer lag increase at 14:20
3. Root cause analysis:
   "The latency spike correlates with a batch job on hosts bm-040-050
    that consumed 90% CPU, causing MySQL on those hosts to slow down.
    Job-scheduler depends on MySQL for job state, leading to cascading latency."
4. Suggested remediation:
   "Implement CPU cgroup limits for batch jobs to prevent noisy-neighbor effect."
```

**Use Case 4: Intelligent Alert Tuning**
```
AI Agent reviews alert history:
1. Analyzes 90 days of alert firing history
2. Identifies:
   - 15 alerts that fire and auto-resolve within 5 minutes (flapping)
   - 8 alerts with 100% false positive rate (bad thresholds)
   - 3 alerts that always fire together (should be grouped)
3. Recommends:
   - Adjust thresholds for 8 noisy alerts based on statistical analysis
   - Add for-duration to flapping alerts
   - Create inhibition rules for correlated alerts
4. Generates Alertmanager config changes as a PR for review
```

**Architecture:**
```
┌──────────────┐     ┌──────────────────────┐     ┌───────────────────┐
│ Thanos Query │◀───▶│  AI Metrics Agent     │────▶│ Actions:          │
│ API          │     │  - LLM (PromQL gen)   │     │ - Grafana API     │
│              │     │  - Time-series ML     │     │ - Alertmanager    │
└──────────────┘     │  - RAG (runbooks,     │     │   config          │
                     │    past incidents)    │     │ - Jira tickets    │
                     └──────────────────────┘     │ - Slack messages  │
                                                   └───────────────────┘
```

**Guardrails:**
- AI agent has read-only access to Thanos Query API.
- Alert configuration changes require human approval (PR review).
- Capacity planning recommendations are suggestions, not automated purchases.
- All AI-generated PromQL is validated for syntax and estimated cost (cardinality) before execution.

---

## 13. Complete Interviewer Q&A Bank

### Prometheus Internals (Q1-Q5)

**Q1: Explain the Prometheus pull model. When would you use push instead?**
A: Prometheus actively scrapes (HTTP GET) each target's `/metrics` endpoint at a configured interval. Benefits: (1) If a target is down, Prometheus detects it immediately (`up == 0`). (2) No push infrastructure needed at the target. (3) Prometheus controls the scrape rate. Push is needed when: (a) targets are behind a firewall (Prometheus cannot reach them), (b) targets are ephemeral/short-lived (batch jobs that exit before scraping -- use Pushgateway), (c) event-driven systems that cannot expose an HTTP endpoint. Even with push, the Pushgateway pattern converts it back to pull.

**Q2: What happens when Prometheus scrapes a target that takes too long to respond?**
A: The scrape has a configurable timeout (`scrape_timeout`, default 10s, must be <= `scrape_interval`). If the target does not respond within the timeout: (1) The scrape is marked as failed. (2) `up{instance=...} = 0` for that target. (3) No samples are stored for that scrape. (4) `scrape_duration_seconds` records the timeout duration. (5) Next scrape happens at the normal interval. Common causes: the target's `/metrics` endpoint is computationally expensive (too many metrics), or the target is overloaded. Fix: reduce the number of exposed metrics or increase the scrape timeout (with corresponding interval increase).

**Q3: Explain staleness in Prometheus. Why does it matter?**
A: Staleness marks a time series as "no longer being reported." Before staleness (Prometheus 1.x), disappeared series would show the last value indefinitely, leading to incorrect query results. In Prometheus 2.x+: if a target is scraped but a series is not present in the response (e.g., pod was deleted), a "stale marker" is written. Queries looking at times after the stale marker return no data for that series. Staleness timeout is 5 minutes (5x the default scrape interval). This is critical for Kubernetes where pods are ephemeral -- without staleness, a deleted pod's metrics would persist forever.

**Q4: How does `rate()` work in PromQL? What about counter resets?**
A: `rate()` calculates the per-second average rate of increase over the given time window. For a counter `c` over time window `[t1, t2]`: `rate = (c(t2) - c(t1)) / (t2 - t1)`. Counter resets (when a process restarts, counter goes back to 0): Prometheus detects when `c(t2) < c(t1)` and assumes a reset occurred. It adds the value before reset to subsequent values, effectively treating the counter as continuously increasing. `rate()` handles this transparently. Important: always use `rate()` on counters, never raw counter values. `rate()` requires at least 2 samples in the window, so the window must be >= 2x scrape interval.

**Q5: What is the difference between `rate()` and `irate()`?**
A: `rate()` computes the average per-second rate over the entire window (e.g., `[5m]` looks at all samples in 5 minutes). `irate()` computes the instantaneous rate using only the last two samples in the window. `irate()` is more responsive to spikes (shows the "now" rate) but noisy. `rate()` smooths out fluctuations and is better for alerting and trending. Rule of thumb: use `rate()` for alerts and dashboards; use `irate()` for ad-hoc debugging when you need to see short bursts.

### Thanos & Long-Term Storage (Q6-Q10)

**Q6: How do you handle the "gap" when a Prometheus HA replica restarts?**
A: Each HA replica has a slightly different scrape timing, so their data overlaps but is not identical. When replica A restarts: (1) It replays its WAL to recover in-memory data (data since last block compaction, up to 2h). (2) During the restart window (~1-10 minutes), replica A produces no new data. (3) Replica B continues scraping. (4) Thanos Query's deduplication selects B's data for the gap period. (5) After A recovers, both replicas contribute data. The result: users see a continuous, gap-free time series. The dedup label `replica` ensures Thanos knows A and B are duplicates.

**Q7: How does Thanos Compactor downsampling work?**
A: For each completed block in S3, the Compactor creates downsampled versions: (1) **5-minute resolution**: for each series, compute min, max, sum, count, counter over 5-minute windows. Stored as aggregate chunks. (2) **1-hour resolution**: same aggregates over 1-hour windows. Query behavior: Thanos Query Frontend checks the query step. If step >= 5 minutes, it queries 5-minute resolution data; if step >= 1 hour, it queries 1-hour data. This dramatically reduces the data scanned for long-range queries. A 1-year query at 1-hour resolution scans 8,760 samples per series vs. 2.1M at raw resolution.

**Q8: How do you ensure data consistency when multiple Prometheus instances write to the same S3 bucket?**
A: Each Prometheus instance generates blocks with a unique ULID (Universally Unique Lexicographically Sortable Identifier). External labels (cluster, replica) ensure blocks from different sources are distinguishable. The Compactor processes blocks per "group" (same external labels minus replica). It never mixes blocks from different sources. S3 provides strong read-after-write consistency (since December 2020), so uploaded blocks are immediately visible. The Compactor uses a lock (via S3 object) to prevent concurrent compaction.

**Q9: What is the operational cost of running Thanos?**
A: Components and resource footprint: (1) Thanos Sidecar: 100 MB RAM per instance, negligible CPU. 420 instances (one per Prometheus) = ~42 GB RAM total. (2) Thanos Store Gateway: 2-8 GB RAM per instance (depends on block count). 10 instances = ~50 GB RAM. (3) Thanos Query: 1-2 GB RAM per instance. 5 instances = ~10 GB RAM. (4) Thanos Compactor: 4-16 GB RAM (depends on block size). 1 instance. (5) Query Frontend: 1 GB RAM per instance. 3 instances. (6) Memcached: 50 GB total for query cache and index cache. Total: ~170 GB RAM. The biggest cost is S3 storage (283 TB) and API calls. Overall much cheaper than running Prometheus with 1-year local retention.

**Q10: How do you handle cross-datacenter metric queries?**
A: Each datacenter's Thanos Sidecars and Store Gateways are registered with the global Thanos Query. When a query needs data from multiple DCs: (1) Thanos Query fans out gRPC requests to all stores. (2) Results stream back, are deduplicated and merged. (3) Cross-DC latency (50-200ms) adds to query time. Optimizations: (a) Recording rules in each DC pre-aggregate common cross-DC queries. (b) Query Frontend caching reduces redundant cross-DC queries. (c) Store Gateway in each DC serves local blocks; only the Query layer is centralized (or replicated per DC with cross-DC discovery).

### Grafana & Dashboards (Q11-Q13)

**Q11: How do you handle Grafana dashboard permissions for 50+ teams?**
A: Grafana Organizations or Teams with folder-based permissions. Each team gets a folder containing their dashboards. Folder permissions: team members get Editor, everyone else gets Viewer. The "Platform" folder is viewable by everyone. Data source permissions limit which Prometheus instances a team can query (e.g., team X can only query the cluster they own). With Grafana Enterprise: Row-level access control for multi-tenant dashboards. With OSS: use multiple organizations.

**Q12: How do you alert on missing metrics (a metric that should exist but does not)?**
A: Use the `absent()` function: `absent(up{job="node-exporter", instance="bm-042:9100"})` returns 1 if no `up` metric exists for that target (meaning the target was never scraped or was removed). For a broader check: `absent(node_cpu_seconds_total{datacenter="us-east-1"})` alerts if no host in the DC is reporting CPU. For expected hosts: maintain a list of expected targets and alert when actual targets < expected. The "dead man's switch" pattern: an alert that is always firing; if it stops, the monitoring system itself is broken.

**Q13: How do you handle Grafana variable refresh and loading performance?**
A: Template variables that query label values (e.g., `label_values(up, cluster)`) hit Thanos Query on every dashboard load. Optimizations: (1) Set variable refresh to "On Dashboard Load" or "On Time Range Change" (not "On Query" which runs on every panel query). (2) Use `label_values()` on a low-cardinality metric (e.g., `up` rather than a high-volume metric). (3) Enable Thanos Query Frontend caching for label API queries. (4) For very large label sets, use static variable lists in the dashboard JSON instead of dynamic queries.

### Operations & Troubleshooting (Q14-Q18)

**Q14: A team reports their dashboard is showing "No Data." How do you debug?**
A: Systematic approach: (1) Check target status: `up{job="their-service"} == 0` means scraping failed. Check `last_scrape_error` label. (2) Check if metric name changed (service upgrade changed metric names). (3) Check cardinality: did Prometheus OOM and restart? Check `prometheus_tsdb_head_series`. (4) Check Thanos Query: can you query directly against the Prometheus instance? If yes, the issue is in Thanos routing. (5) Check Grafana data source configuration and variable values. (6) Check time range: is the dashboard showing a time range where no data exists? Most common causes: scrape config change, service down, metric renamed, Grafana variable mismatch.

**Q15: How do you handle a "thundering herd" of Grafana dashboards hitting Prometheus simultaneously?**
A: This happens during an incident when many engineers open dashboards at once. Mitigations: (1) Thanos Query Frontend caching prevents duplicate queries. (2) Grafana's `max_concurrent_datasource_requests` limits per-user load. (3) Prometheus has `--query.max-concurrency` (default 20). (4) Recording rules pre-compute expensive queries, so dashboards read pre-computed results. (5) Grafana CDN caches static dashboard definitions. (6) During major incidents, a designated "metrics lead" shares screenshots in the war room rather than everyone hitting dashboards.

**Q16: How do you handle metrics for ephemeral containers (Kubernetes Jobs, init containers)?**
A: Short-lived containers (seconds to minutes) may exit before Prometheus scrapes them. Solutions: (1) If the job runs long enough for at least 2 scrapes (30s+), normal scraping works. (2) For very short jobs, use Pushgateway: job pushes final metrics before exiting. (3) Use Kubernetes Job-level metrics from kube-state-metrics: `kube_job_status_succeeded`, `kube_job_status_failed`, `kube_job_complete_time`. These persist after the pod is gone. (4) For init container metrics, typically not scraped -- use log-based metrics instead.

**Q17: How do you handle a Prometheus instance that is falling behind on scraping?**
A: Symptoms: `prometheus_target_interval_length_seconds` exceeds configured interval, `prometheus_target_scrapes_exceeded_sample_limit_total` increases, queue buildup. Causes: (a) Too many targets (shard to another Prometheus), (b) targets slow to respond (increase timeout or fix targets), (c) CPU saturation on Prometheus host (increase CPU or reduce recording rules), (d) disk I/O bottleneck from TSDB compaction (use SSD, tune merge threads). Immediate fix: reduce scrape scope (drop non-essential targets).

**Q18: How do you implement multi-region active-active monitoring?**
A: Each region has its own complete Prometheus + Thanos stack. Global Thanos Query discovers stores from all regions via DNS (multi-region service mesh or VPN). Key considerations: (1) Cross-region network latency adds 50-200ms per query. (2) If a region's network is down, queries to that region's stores timeout -- Thanos Query returns partial results. (3) Global dashboards use recording rules (aggregated locally in each region) to minimize cross-region data transfer. (4) Alerting runs locally in each region (not dependent on cross-region connectivity).

### Advanced & Design (Q19-Q20)

**Q19: Compare pull vs. push metrics collection for infrastructure monitoring.**
A: **Pull (Prometheus)**: Advantages -- (a) Prometheus controls scrape rate, preventing overload. (b) Target health is automatically detected (`up` metric). (c) No push infrastructure needed at the target. (d) No client-side batching or retry logic. Disadvantages -- (a) Cannot reach behind firewalls/NAT. (b) Short-lived processes may not be scraped. (c) Scrape overhead on targets. **Push (StatsD, OpenTelemetry, Datadog Agent)**: Advantages -- (a) Works behind firewalls. (b) Short-lived processes can push before exiting. (c) Client controls emission rate. Disadvantages -- (a) No automatic target health detection. (b) Client must handle buffering, retry, backpressure. (c) Receiver must handle burst traffic. For infrastructure monitoring with long-lived targets, pull is superior. For serverless/event-driven systems, push is necessary.

**Q20: How would you design a monitoring system from scratch if Prometheus did not exist?**
A: Core design principles: (1) Time-series database with delta-of-delta timestamp compression and XOR value encoding (Gorilla paper). (2) Label-based data model with inverted index for fast series lookup. (3) Pull-based collection with automatic service discovery. (4) Expressive query language for rate calculations, aggregations, and percentile estimation. (5) HA via independent replicas with server-side deduplication. (6) Tiered storage with local fast tier and remote cheap tier. Key differences from Prometheus I would make: (a) Native horizontal scaling (distributed TSDB, not single-node). (b) Built-in long-term storage without separate components. (c) Better native HA (not just "run two instances"). (d) Support for both pull and push natively. This describes what VictoriaMetrics and Grafana Mimir have built.

---

## 14. References

1. **Prometheus: Up & Running** - Brian Brazil (O'Reilly)
2. **Prometheus TSDB Design**: https://fabxc.org/tsdb/
3. **Gorilla: A Fast, Scalable, In-Memory Time Series Database** - Facebook (VLDB 2015)
4. **Thanos Documentation**: https://thanos.io/
5. **Micrometer Documentation**: https://micrometer.io/docs
6. **Grafana Grafonnet Library**: https://github.com/grafana/grafonnet-lib
7. **Google SRE Book - Monitoring Distributed Systems**: https://sre.google/sre-book/monitoring-distributed-systems/
8. **USE Method (Brendan Gregg)**: https://www.brendangregg.com/usemethod.html
9. **RED Method (Tom Wilkie)**: https://grafana.com/blog/2018/08/02/the-red-method-how-to-instrument-your-services/
10. **Kubernetes Monitoring with Prometheus**: https://kubernetes.io/docs/tasks/debug/debug-cluster/resource-metrics-pipeline/
