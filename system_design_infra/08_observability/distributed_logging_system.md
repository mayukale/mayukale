# System Design: Distributed Logging System

> **Relevance to role:** A cloud infrastructure platform engineer manages thousands of bare-metal hosts, Kubernetes clusters, OpenStack control planes, and Java/Python microservices. A centralized logging system is the primary tool for debugging production incidents, auditing changes, and understanding fleet-wide behavior. Deep knowledge of the ELK/EFK pipeline, Kafka buffering, Elasticsearch internals, and Kubernetes log collection patterns is directly tested in infrastructure interviews.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Ingest structured and unstructured logs** from all sources: bare-metal hosts, Kubernetes pods, OpenStack services, Java/Python applications, job schedulers, MySQL slow query logs, and infrastructure daemons.
2. **Centralized search and filtering** across all log streams with sub-second query latency for recent data.
3. **Structured log format** with mandatory fields: `timestamp`, `level`, `service`, `host`, `trace_id`, `span_id`, `message`, plus arbitrary key-value tags.
4. **Log correlation** by `trace_id` to connect logs across distributed services (link to tracing system).
5. **Dashboard and visualization** via Kibana (or Grafana Loki) with saved queries, filters, and alerts.
6. **Retention tiers**: hot (fast SSD), warm (HDD), cold (object storage), with configurable TTLs per tier.
7. **Multi-tenancy**: each team/service namespace sees only its own logs; platform team sees all.
8. **Log-based alerting**: fire alerts when log patterns match (e.g., `level:ERROR AND service:payment` > 100/min).

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Ingestion throughput | 2 million events/sec at peak |
| End-to-end latency (log written to searchable) | < 30 seconds (p99) |
| Query latency on hot tier | < 2 seconds (p95) for typical queries |
| Availability | 99.9% for ingestion, 99.5% for query |
| Durability | Zero log loss for ERROR and above; best-effort for DEBUG |
| Retention | Hot 7d, Warm 30d, Cold 90d, Delete after 1 year |

### Constraints & Assumptions
- Fleet size: 50,000 bare-metal hosts + 200 Kubernetes clusters (500,000+ pods).
- Average log line size: 500 bytes (structured JSON).
- Peak-to-average ratio: 5x (during incidents, deployments, job bursts).
- Existing Kafka clusters available for buffering.
- Budget allows dedicated Elasticsearch clusters but not unlimited SSD.
- Java services use SLF4J + Logback; Python services use `structlog` or `logging` with JSON formatter.

### Out of Scope
- Application Performance Monitoring (APM) -- covered in tracing system.
- Metrics collection -- covered in metrics system.
- Audit logging for compliance -- covered in audit log system (separate immutable pipeline).
- Log analytics / business intelligence over logs.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Total log sources | 50K hosts + 500K pods + 10K infra daemons | ~560,000 sources |
| Avg logs/source/sec | 5 events/sec (blended across DEBUG, INFO, ERROR) | 5 eps |
| Steady-state ingestion | 560,000 x 5 | **2.8M events/sec** |
| Peak ingestion (5x) | 2.8M x 5 | **14M events/sec** |
| Avg event size | 500 bytes (JSON structured) | 500 B |
| Steady-state bandwidth | 2.8M x 500 B | **1.4 GB/s** |
| Peak bandwidth | 14M x 500 B | **7 GB/s** |

### Latency Requirements

| Path | Target |
|---|---|
| Application writes log → local collector picks up | < 1 second |
| Collector → Kafka | < 2 seconds |
| Kafka → Logstash → Elasticsearch indexed | < 25 seconds |
| End-to-end (write → searchable) | **< 30 seconds p99** |
| Elasticsearch query (hot tier) | < 2 seconds p95 |

### Storage Estimates

| Tier | Duration | Daily Volume | Total Storage |
|---|---|---|---|
| Raw ingestion | 1 day | 2.8M x 500B x 86400 = **~121 TB/day** | 121 TB |
| After sampling (DEBUG at 1%) | 1 day | ~60 TB/day (DEBUG is ~50% of volume) | 60 TB |
| Hot (SSD, replicated 1x) | 7 days | 60 TB x 7 x 2 (1 replica) | **840 TB SSD** |
| Warm (HDD, replicated 1x) | 30 days | 60 TB x 30 x 2 | **3.6 PB HDD** |
| Cold (S3/object store, no replica) | 90 days | 60 TB x 90 x 1.1 (overhead) | **5.9 PB object** |
| Kafka buffer (3 replicas, 48h retention) | 2 days | 60 TB x 2 x 3 | **360 TB** |

### Bandwidth Estimates

| Segment | Bandwidth |
|---|---|
| Collectors → Kafka (compressed, ~4:1) | ~350 MB/s steady, 1.75 GB/s peak |
| Kafka → Logstash (decompressed) | ~1.4 GB/s steady |
| Logstash → Elasticsearch (bulk index) | ~1.4 GB/s steady |
| Elasticsearch intra-cluster replication | ~1.4 GB/s |
| Total network egress from collectors | ~1.4 GB/s |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        LOG PRODUCERS                                    │
│  ┌──────────┐ ┌──────────────┐ ┌───────────┐ ┌──────────────────────┐  │
│  │Java Svc  │ │Python Svc    │ │K8s Pods   │ │Bare-metal Hosts      │  │
│  │SLF4J+    │ │structlog     │ │stdout/    │ │syslog, journald,     │  │
│  │Logback   │ │JSON fmt      │ │stderr     │ │MySQL slow query log  │  │
│  │JSON      │ │              │ │           │ │                      │  │
│  └────┬─────┘ └─────┬────────┘ └─────┬─────┘ └──────────┬───────────┘  │
│       │              │                │                   │              │
│       ▼              ▼                ▼                   ▼              │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │              LOCAL LOG COLLECTORS (per host/node)                │    │
│  │  Filebeat (files) / Fluent Bit (K8s DaemonSet) / Fluentd       │    │
│  │  - Tail log files or capture stdout/stderr                      │    │
│  │  - Add metadata: host, cluster, namespace, pod, container       │    │
│  │  - Buffer locally (disk-backed) for backpressure                │    │
│  │  - Compress and batch before sending                            │    │
│  └──────────────────────────┬──────────────────────────────────────┘    │
│                             │                                           │
└─────────────────────────────┼───────────────────────────────────────────┘
                              │  (Kafka producer, compressed batches)
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     KAFKA BUFFER CLUSTER                                │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  Topics: logs.{service}, logs.{cluster}, logs.raw              │    │
│  │  Partitions: 256+ per topic (keyed by service or host)         │    │
│  │  Replication factor: 3                                          │    │
│  │  Retention: 48 hours (replay window)                            │    │
│  │  Compression: lz4                                               │    │
│  └──────────────────────────┬──────────────────────────────────────┘    │
│                             │                                           │
└─────────────────────────────┼───────────────────────────────────────────┘
                              │  (Consumer groups)
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                  LOG PROCESSING PIPELINE                                │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  Logstash / Vector / Flink                                      │    │
│  │  - Parse and validate JSON structure                            │    │
│  │  - Enrich: add team/owner from service registry                 │    │
│  │  - Transform: normalize timestamps to UTC ISO8601               │    │
│  │  - Sample: 1% sampling for DEBUG-level high-volume services     │    │
│  │  - Route: by log level and service to appropriate ES index      │    │
│  │  - Dead-letter queue for malformed events                       │    │
│  └──────────┬───────────────────────────┬──────────────────────────┘    │
│             │                           │                               │
└─────────────┼───────────────────────────┼───────────────────────────────┘
              │                           │
              ▼                           ▼
┌──────────────────────────┐  ┌────────────────────────────────────────┐
│  ELASTICSEARCH CLUSTER   │  │  LONG-TERM OBJECT STORE (S3 / MinIO)  │
│  ┌────────────────────┐  │  │  ┌──────────────────────────────────┐  │
│  │ HOT TIER (SSD)     │  │  │  │  Cold tier: frozen/searchable    │  │
│  │ Index: logs-YYYY-  │  │  │  │  snapshots, Parquet for          │  │
│  │   MM-DD-{service}  │  │  │  │  analytics                       │  │
│  │ Retention: 7 days  │  │  │  │  Retention: 90 days              │  │
│  │ 1 replica          │  │  │  └──────────────────────────────────┘  │
│  ├────────────────────┤  │  └────────────────────────────────────────┘
│  │ WARM TIER (HDD)    │  │
│  │ Rolled from hot    │  │  ┌────────────────────────────────────────┐
│  │ Force-merged, RO   │  │  │  KIBANA / GRAFANA                     │
│  │ Retention: 30 days │  │  │  - Dashboards per service/team        │
│  │ 1 replica          │  │  │  - Saved queries and filters          │
│  ├────────────────────┤  │  │  - Log-based alerts                   │
│  │ COLD TIER          │  │  │  - Trace correlation links            │
│  │ Searchable snapshot│  │  └────────────────────────────────────────┘
│  │ mounted from S3    │  │
│  │ Retention: 90 days │  │  ┌────────────────────────────────────────┐
│  └────────────────────┘  │  │  LOG-BASED ALERTING                   │
│  Cluster: 100+ data     │  │  │  - ElastAlert / Kibana Alerting      │
│  nodes, 3 master, 10    │  │  │  - Pattern match → Alertmanager      │
│  coordinating            │  │  │  - Rate-based alerts                 │
└──────────────────────────┘  └────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Filebeat / Fluent Bit** | Lightweight per-host/per-node log collector. Tails files, captures K8s stdout/stderr via container runtime log path. Adds host/pod metadata. Disk-backed buffer for backpressure. |
| **Kafka** | Decouples producers from consumers. Absorbs burst traffic (48h buffer). Enables replay for reprocessing. Multiple consumer groups (ES, cold storage, alerting). |
| **Logstash / Vector** | Stateless stream processor. Parses, enriches, transforms, samples, and routes log events. Horizontally scalable consumer group. |
| **Elasticsearch** | Primary search and analytics engine. Inverted index for full-text search. Hot-warm-cold architecture for cost optimization. |
| **Kibana** | UI for log exploration, dashboards, saved searches, and alerting configuration. |
| **Object Store (S3)** | Long-term archival. Elasticsearch searchable snapshots mount S3 as frozen tier. Also stores raw compressed logs for compliance. |
| **ElastAlert** | Runs periodic queries against Elasticsearch, matches patterns, fires alerts to Alertmanager/PagerDuty. |

### Data Flows

1. **Ingestion**: Application → stdout/file → Fluent Bit (DaemonSet) or Filebeat → Kafka topic `logs.raw` → Logstash consumer group → Elasticsearch bulk index API.
2. **Query**: User → Kibana → Elasticsearch coordinating node → fan-out to data nodes → merge results → return.
3. **Tiering**: ILM policy triggers: hot (0-7d, SSD) → warm (7-37d, force-merge, HDD) → cold (37-127d, searchable snapshot on S3) → delete (>1y).
4. **Alerting**: ElastAlert queries ES every 60s → pattern match → route to Alertmanager → PagerDuty/Slack.
5. **Reprocessing**: Deploy new Logstash pipeline → reset Kafka consumer offset → replay 48h of logs.

---

## 4. Data Model

### Core Entities & Schema

**Log Event (Elasticsearch Document)**
```json
{
  "@timestamp": "2026-04-09T14:23:45.123Z",
  "level": "ERROR",
  "logger": "com.infra.scheduler.JobRunner",
  "service": "job-scheduler",
  "version": "2.4.1",
  "host": "bm-prod-us-east-0042",
  "cluster": "k8s-prod-us-east-1",
  "namespace": "scheduling",
  "pod": "job-scheduler-7b4f9d-x2k4q",
  "container": "scheduler",
  "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
  "span_id": "00f067aa0ba902b7",
  "message": "Failed to allocate GPU resources for job j-98234",
  "error.type": "ResourceExhaustedException",
  "error.stack_trace": "com.infra.scheduler.ResourceExhaustedException: ...",
  "job_id": "j-98234",
  "resource_pool": "gpu-a100-us-east",
  "team": "ml-training",
  "environment": "production",
  "datacenter": "us-east-1a"
}
```

**Elasticsearch Index Template**
```json
{
  "index_patterns": ["logs-*"],
  "template": {
    "settings": {
      "number_of_shards": 20,
      "number_of_replicas": 1,
      "index.codec": "best_compression",
      "index.refresh_interval": "5s",
      "index.translog.durability": "async",
      "index.translog.sync_interval": "5s",
      "index.routing.allocation.require.tier": "hot"
    },
    "mappings": {
      "dynamic": "false",
      "_source": { "enabled": true },
      "properties": {
        "@timestamp":    { "type": "date" },
        "level":         { "type": "keyword" },
        "logger":        { "type": "keyword" },
        "service":       { "type": "keyword" },
        "version":       { "type": "keyword" },
        "host":          { "type": "keyword" },
        "cluster":       { "type": "keyword" },
        "namespace":     { "type": "keyword" },
        "pod":           { "type": "keyword" },
        "container":     { "type": "keyword" },
        "trace_id":      { "type": "keyword" },
        "span_id":       { "type": "keyword" },
        "message":       { "type": "text", "analyzer": "standard" },
        "error.type":    { "type": "keyword" },
        "error.stack_trace": { "type": "text", "index": false },
        "team":          { "type": "keyword" },
        "environment":   { "type": "keyword" },
        "datacenter":    { "type": "keyword" }
      }
    }
  }
}
```

### Database Selection

| Storage Layer | Technology | Rationale |
|---|---|---|
| **Hot search (0-7d)** | Elasticsearch on SSD | Inverted index enables sub-second full-text search. Rich query DSL. Mature ecosystem with Kibana. |
| **Warm search (7-37d)** | Elasticsearch on HDD | Same query interface, force-merged segments for read optimization, lower cost per GB. |
| **Cold search (37-127d)** | Elasticsearch searchable snapshots on S3 | Minimal local cache, data lives in object store, queries slower but functional. |
| **Buffer** | Apache Kafka | Exactly-once semantics, replay capability, absorbs bursts, multiple consumer groups. |
| **Long-term archive (>127d)** | S3 + Parquet (via Spark jobs) | Cheapest storage, Parquet for ad-hoc analytics via Presto/Trino, meets compliance retention. |

**Why Elasticsearch over alternatives:**
- **vs. ClickHouse**: ClickHouse excels at columnar analytics but lacks the full-text inverted index that makes log search fast. Logging is primarily a search problem, not an aggregation problem.
- **vs. Grafana Loki**: Loki indexes only labels (not full text), making it cheaper but slower for arbitrary text search. Good for K8s-centric workloads but insufficient for our bare-metal + multi-platform fleet.
- **vs. Splunk**: Proprietary, extremely expensive at our scale (60 TB/day ingestion licensing).

### Indexing Strategy

| Strategy | Detail |
|---|---|
| **Index naming** | `logs-{service}-YYYY.MM.DD` (e.g., `logs-job-scheduler-2026.04.09`) |
| **Rollover** | ILM rollover when index exceeds 50 GB or 1 day age |
| **Shard count** | Target 30-50 GB per shard; 20 primary shards per daily index for large services |
| **Shard routing** | `_routing` by `service` to colocate a service's logs |
| **Force merge** | On warm tier: merge to 1 segment per shard (read-optimized) |
| **Field mapping** | `dynamic: false` to prevent field explosion from arbitrary JSON keys |
| **Doc values** | Enabled for all keyword fields (efficient aggregations); disabled for `message` text field |
| **_source** | Enabled (needed for log display); compressed with `best_compression` codec |

**Field Explosion Problem:**
Dynamic mapping with arbitrary JSON fields creates unbounded field counts, causing memory pressure and mapping conflicts. Solution: `dynamic: false` with explicit mapping. Unknown fields stored in `_source` but not indexed. If teams need to search custom fields, they submit a mapping request reviewed by the platform team.

---

## 5. API Design

### Ingestion APIs

**Elasticsearch Bulk Index API (internal, called by Logstash)**
```
POST /_bulk
Content-Type: application/x-ndjson

{"index": {"_index": "logs-job-scheduler-2026.04.09"}}
{"@timestamp": "2026-04-09T14:23:45.123Z", "level": "ERROR", ...}
{"index": {"_index": "logs-job-scheduler-2026.04.09"}}
{"@timestamp": "2026-04-09T14:23:45.456Z", "level": "INFO", ...}
```

**Kafka Producer API (called by collectors)**
```
Topic: logs.raw
Key: {service}-{host}  (ensures ordering per source)
Value: compressed JSON log event
Headers: {
  "content-type": "application/json",
  "compression": "lz4",
  "schema-version": "2"
}
```

### Query APIs

**Search Logs**
```
POST /api/v1/logs/search
{
  "query": "level:ERROR AND service:job-scheduler AND message:\"GPU resources\"",
  "time_range": {
    "from": "2026-04-09T14:00:00Z",
    "to": "2026-04-09T15:00:00Z"
  },
  "filters": {
    "cluster": ["k8s-prod-us-east-1"],
    "namespace": ["scheduling"]
  },
  "sort": [{"@timestamp": "desc"}],
  "size": 100,
  "from": 0
}

Response:
{
  "total": 1423,
  "took_ms": 234,
  "hits": [
    {
      "@timestamp": "2026-04-09T14:23:45.123Z",
      "level": "ERROR",
      "service": "job-scheduler",
      "message": "Failed to allocate GPU resources for job j-98234",
      "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
      "host": "bm-prod-us-east-0042",
      ...
    }
  ]
}
```

**Get Logs by Trace ID (correlation)**
```
GET /api/v1/logs/trace/{trace_id}?from=2026-04-09T14:00:00Z&to=2026-04-09T15:00:00Z

Response:
{
  "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
  "logs": [
    {"@timestamp": "...", "service": "api-gateway", "message": "Received job submission", ...},
    {"@timestamp": "...", "service": "job-scheduler", "message": "Scheduling job j-98234", ...},
    {"@timestamp": "...", "service": "job-scheduler", "message": "Failed to allocate GPU resources", ...}
  ]
}
```

**Log Aggregation (for dashboards)**
```
POST /api/v1/logs/aggregate
{
  "time_range": {"from": "...", "to": "..."},
  "group_by": ["service", "level"],
  "interval": "5m",
  "filters": {"environment": "production"}
}

Response:
{
  "buckets": [
    {"timestamp": "2026-04-09T14:00:00Z", "service": "job-scheduler", "level": "ERROR", "count": 47},
    {"timestamp": "2026-04-09T14:05:00Z", "service": "job-scheduler", "level": "ERROR", "count": 123},
    ...
  ]
}
```

### Admin APIs

```
# ILM Policy Management
PUT /api/v1/admin/ilm/policies/{policy_name}
{
  "hot_max_age": "1d", "hot_max_size": "50gb",
  "warm_min_age": "7d",
  "cold_min_age": "37d",
  "delete_min_age": "365d"
}

# Index Template Management
PUT /api/v1/admin/templates/{template_name}
{ "mappings": {...}, "settings": {...} }

# Sampling Rule Management
PUT /api/v1/admin/sampling-rules/{service}
{ "level": "DEBUG", "sample_rate": 0.01, "enabled": true }

# Cluster Health
GET /api/v1/admin/health
Response: { "cluster_status": "green", "active_shards": 14200, "pending_tasks": 0, ... }
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Elasticsearch Index Lifecycle Management (ILM) and Tiered Storage

**Why it's hard:**
At 60 TB/day, Elasticsearch must continuously create new indices, allocate shards across hundreds of nodes, and move aging data through tiers -- all without disrupting search latency or ingestion throughput. A single misconfigured rollover or a shard imbalance can cascade into cluster instability.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Single monolithic index** | Simple management | Unbounded growth, impossible to delete old data efficiently, shard size explosion |
| **Time-based index per day** | Easy deletion (drop index), predictable shard size | Large services create very large daily indices; small services waste shards |
| **Rollover-based with ILM** | Consistent shard sizes (50 GB target), automated tier transitions, per-service policies | Complex configuration, ILM polling interval adds delay, requires monitoring |
| **Data stream (ES 7.9+)** | Append-only abstraction, built-in rollover, cleaner API | Less flexibility for custom routing, newer feature with some edge cases |

**Selected approach: Data Streams with ILM policies**

Data streams provide an append-only abstraction where each backing index is automatically rolled over based on ILM policy. Combined with ILM, we get automated hot→warm→cold→delete transitions.

**Implementation Detail:**

```json
// ILM Policy: "logs-default-policy"
{
  "policy": {
    "phases": {
      "hot": {
        "min_age": "0ms",
        "actions": {
          "rollover": {
            "max_primary_shard_size": "50gb",
            "max_age": "1d"
          },
          "set_priority": { "priority": 100 }
        }
      },
      "warm": {
        "min_age": "7d",
        "actions": {
          "shrink": { "number_of_shards": 1 },
          "forcemerge": { "max_num_segments": 1 },
          "allocate": {
            "require": { "tier": "warm" },
            "number_of_replicas": 1
          },
          "set_priority": { "priority": 50 }
        }
      },
      "cold": {
        "min_age": "37d",
        "actions": {
          "searchable_snapshot": {
            "snapshot_repository": "s3-logs-archive",
            "force_merge_index": true
          },
          "set_priority": { "priority": 0 }
        }
      },
      "delete": {
        "min_age": "365d",
        "actions": { "delete": {} }
      }
    }
  }
}
```

**Tier transition mechanics:**
1. **Hot → Warm (day 7):** Shard allocation filter moves shards to warm-tagged nodes. Shrink reduces shard count (saves overhead). Force-merge to 1 segment per shard eliminates deleted docs and optimizes read performance.
2. **Warm → Cold (day 37):** Searchable snapshot is taken and uploaded to S3. Local index is replaced by a mounted snapshot. Only a small local cache is kept; reads fetch from S3 on-demand.
3. **Cold → Delete (day 365):** Snapshot metadata removed, S3 objects deleted.

**Failure Modes:**
- **ILM step stuck**: A node goes down during shard relocation, ILM retries indefinitely. Monitor with `GET _ilm/status` and `GET {index}/_ilm/explain`.
- **Shard imbalance after rollover**: New index may land all primary shards on same node. Use `index.routing.allocation.total_shards_per_node` to cap shards per node.
- **S3 latency spike on cold tier query**: Add SSD cache nodes for cold tier. Configure `xpack.searchable.snapshot.shared_cache.size` appropriately (e.g., 500 GB per node).

**Interviewer Q&As:**

**Q1: How do you handle shard sizing for services with wildly different log volumes?**
A: We use rollover-based ILM rather than fixed daily indices. A high-volume service (job-scheduler) might roll over every 2 hours to maintain 50 GB shards, while a low-volume service (config-server) might roll over once a day. The rollover condition uses `max_primary_shard_size: 50gb OR max_age: 1d`, whichever triggers first.

**Q2: What happens if Elasticsearch cluster goes red during a rollover?**
A: ILM operations are idempotent and will retry. However, a red cluster means unassigned primary shards. We first address the root cause (typically a node failure). ILM will automatically resume once the cluster returns to yellow/green. We also maintain a Kafka buffer with 48h retention, so ingestion data is not lost -- Logstash consumers will pause and resume.

**Q3: How do you prevent one noisy service from overwhelming the cluster?**
A: Three mechanisms: (1) Kafka topic partitioning isolates backpressure per service; (2) Logstash pipeline has per-service rate limiting; (3) Elasticsearch index-level ingestion rate limiting via `index.indexing.slowlog.threshold.index.warn`. We also enforce a mandatory DEBUG sampling rate of 1% for services exceeding 10K events/sec.

**Q4: Explain the force-merge operation. Why is it important?**
A: Elasticsearch stores data in immutable segments. Deletes are just markers (tombstones). Over time, segments accumulate deleted docs. Force-merge to 1 segment: (a) physically removes deleted docs, (b) reduces segment count (fewer file handles, faster searches), (c) improves compression. We only do this on warm tier because force-merge is I/O intensive and the index must be read-only.

**Q5: How do searchable snapshots work internally?**
A: The snapshot is stored in S3 as a set of segment files. When a query hits a cold index, Elasticsearch downloads the needed segment data on demand and caches it locally (LRU cache on SSD). The first query is slow (S3 latency), but subsequent queries on the same data are fast. The local cache is configured via `shared_cache.size` -- typically 500 GB per cold-tier node. Only metadata is kept in cluster state, so cold indices have minimal cluster overhead.

**Q6: What is the field explosion problem and how do you solve it?**
A: When dynamic mapping is enabled, every unique JSON key creates a new field in the mapping. If applications log arbitrary key-value pairs (e.g., request headers, job parameters), the mapping can grow to thousands of fields. This causes: (a) excessive memory for field data and mapping state, (b) slow cluster state updates, (c) mapping conflicts between indices. Solution: `dynamic: false` in the index template. Only explicitly mapped fields are indexed. Unknown fields are still stored in `_source` for display but cannot be searched. Teams can request new fields via a PR to the mapping template.

---

### Deep Dive 2: Kafka as Log Buffer and Backpressure Management

**Why it's hard:**
Logs are generated continuously and cannot be paused. During Elasticsearch maintenance, degradation, or burst traffic, the pipeline must absorb traffic without losing data. Kafka serves as the shock absorber, but configuring it for 14M events/sec peak with 48h retention across 360 TB requires careful partition design, consumer group management, and backpressure signaling.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Direct Filebeat → Elasticsearch** | Simple, fewer components | No buffer; ES backpressure causes collector memory pressure, potential log loss |
| **Filebeat → Redis → Logstash → ES** | Redis is fast | Redis is single-threaded, limited durability, no replay, poor at scale |
| **Filebeat → Kafka → Logstash → ES** | Durable, replayable, scalable, multiple consumers | Additional infra (Kafka cluster), operational complexity |
| **Filebeat → Kafka → Flink → ES** | Stateful stream processing, windowing, exactly-once | Flink is complex to operate; overkill for log parsing |

**Selected approach: Kafka buffer with Logstash/Vector consumers**

**Implementation Detail:**

```
Kafka Cluster Configuration:
- Brokers: 30 nodes (128 GB RAM, 12 x 4TB NVMe each)
- Topics:
    logs.raw          - 512 partitions, RF=3, retention=48h
    logs.dlq          - 64 partitions, RF=3, retention=7d (dead letter)
    logs.high-priority - 128 partitions, RF=3, retention=48h (ERROR/FATAL only)
- Compression: lz4 (producer-side, broker passes through)
- Batch size: 1 MB (producer), linger.ms: 100ms
- Acks: acks=1 (leader only - acceptable for logs, trades small loss risk for throughput)
- Min ISR: 2
```

**Partition key strategy:**
- Key = `{service}-{host}` ensures logs from the same source land in the same partition (preserving ordering per source).
- 512 partitions for `logs.raw` enables high parallelism: up to 512 Logstash consumer instances.

**Consumer group design:**
- `cg-logstash-elasticsearch`: Primary consumer group writing to Elasticsearch.
- `cg-cold-archive`: Secondary consumer group writing compressed logs to S3 for long-term archive.
- `cg-alerting`: Consumer group feeding ElastAlert for real-time log pattern matching.

**Backpressure handling:**
1. **Collector → Kafka**: If Kafka is slow, Fluent Bit disk buffer absorbs (configured at 2 GB per node). If disk buffer fills, oldest DEBUG logs are dropped first (priority-based dropping).
2. **Kafka → Logstash**: If Elasticsearch is slow, Logstash pauses consuming from Kafka. Kafka retains messages for 48h. This is the primary backpressure mechanism.
3. **Burst absorption**: At 5x peak (14M events/sec), Kafka can absorb the burst. 48h retention at peak would be: 14M x 500B x 3600 x 48 x 3 replicas = ~3.6 PB, which exceeds our allocation. In practice, peaks last minutes/hours, not 48h continuously.

**Failure Modes:**
- **Kafka broker failure**: RF=3 means 2 failures can be tolerated. ISR=2 ensures writes succeed with 1 broker down.
- **Consumer lag buildup**: If Logstash consumers fall behind, lag grows. Monitor with `kafka_consumer_lag` metric. Auto-scale Logstash instances when lag exceeds threshold (e.g., >10M messages).
- **Kafka disk full**: Alert at 70% disk usage. Emergency: increase retention-based deletion or expand brokers.
- **Partition skew**: A single hot service could overload one partition. Use consistent hashing with virtual nodes or add jitter to partition key.

**Interviewer Q&As:**

**Q1: Why acks=1 instead of acks=all for logs?**
A: For log data, we accept a small risk of message loss (leader fails before replication) in exchange for significantly higher throughput. With acks=all, every produce request waits for all ISR replicas to acknowledge, adding ~5-10ms per batch. At 14M events/sec, this latency compounds. For ERROR and above, we route to a separate `logs.high-priority` topic with acks=all.

**Q2: How do you handle reprocessing when you deploy a new Logstash parsing pipeline?**
A: We deploy the new pipeline as a new consumer group, reset its offsets to 48 hours ago, and let it consume the full Kafka buffer. Once the new pipeline catches up and we validate output quality, we decommission the old consumer group. This gives us a zero-downtime pipeline update with 48h backfill.

**Q3: How do you handle Kafka topic compaction for logs?**
A: We do not use compaction for logs. Log messages are append-only events, not key-value state. We use time-based retention (48h) which deletes whole segments when they age out. Compaction would require unique keys per message and would not reduce storage meaningfully.

**Q4: What happens when Elasticsearch is down for hours?**
A: Kafka's 48h retention window is the primary safety net. Logstash consumers pause and resume. If downtime exceeds 48h, we have the cold-archive consumer group that writes raw logs to S3 in real-time. We can backfill Elasticsearch from S3 using a batch Spark job.

**Q5: How do you monitor Kafka health for this pipeline?**
A: Key metrics: (1) `consumer_lag` per group per partition, (2) `under_replicated_partitions`, (3) `bytes_in_per_sec` / `bytes_out_per_sec` per broker, (4) `request_latency_ms` for produce/fetch, (5) disk utilization per broker. All scraped by Prometheus via JMX exporter and alerted via Alertmanager.

**Q6: Why not use Kafka Streams or Flink instead of Logstash?**
A: Logstash/Vector is sufficient for stateless per-event transformations (parse, enrich, route). Kafka Streams or Flink would add complexity but is warranted only for stateful operations (windowed aggregations, joins). Our log pipeline does not require state -- each event is processed independently. If we add features like "alert when ERROR rate exceeds threshold in 5-minute window," we would consider Flink for that specific consumer.

---

### Deep Dive 3: Kubernetes Log Collection Architecture

**Why it's hard:**
Kubernetes pods are ephemeral -- they can be created, destroyed, and rescheduled across nodes in seconds. Logs written to stdout/stderr are stored temporarily by the container runtime (containerd) and are lost when the pod is evicted. The log collector must: (1) discover all containers dynamically, (2) handle pod churn without losing log lines, (3) add rich Kubernetes metadata (namespace, pod name, labels), and (4) handle multi-line logs (Java stack traces).

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Sidecar per pod** | Isolated, per-app configuration | Massive resource overhead (500K pods x sidecar), management nightmare |
| **DaemonSet per node** | One collector per node, efficient, centralized | Must handle all containers on node, complex multi-line merging |
| **Logging library direct to Kafka** | No agent needed, structured from source | Requires every app to integrate, no coverage for infra/system logs |
| **Node-level journald forwarding** | System-native | Misses container stdout, limited metadata |

**Selected approach: DaemonSet-based Fluent Bit with enrichment**

**Implementation Detail:**

```yaml
# Fluent Bit DaemonSet (simplified)
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: fluent-bit
  namespace: logging
spec:
  selector:
    matchLabels:
      app: fluent-bit
  template:
    spec:
      serviceAccountName: fluent-bit  # RBAC for K8s API access
      tolerations:
        - operator: Exists            # Run on ALL nodes including masters
      containers:
        - name: fluent-bit
          image: fluent/fluent-bit:3.0
          resources:
            requests: { cpu: "200m", memory: "256Mi" }
            limits:   { cpu: "500m", memory: "512Mi" }
          volumeMounts:
            - name: varlog
              mountPath: /var/log
              readOnly: true
            - name: containers
              mountPath: /var/lib/containerd
              readOnly: true
            - name: buffer
              mountPath: /var/fluent-bit/buffer
      volumes:
        - name: varlog
          hostPath: { path: /var/log }
        - name: containers
          hostPath: { path: /var/lib/containerd }
        - name: buffer
          hostPath: { path: /var/fluent-bit/buffer }
```

**Log path on a Kubernetes node:**
```
/var/log/containers/{pod}_{namespace}_{container}-{container_id}.log
  → symlink to →
/var/log/pods/{namespace}_{pod}_{uid}/{container}/0.log
  → symlink to →
/var/lib/containerd/io.containerd.grpc.v1.cri/containers/{id}/...
```

Fluent Bit tails `/var/log/containers/*.log` and follows symlinks.

**Multi-line handling (Java stack traces):**
```ini
[MULTILINE_PARSER]
    name          java_multiline
    type          regex
    flush_timeout 1000
    rule          "start_state"  "/^\d{4}-\d{2}-\d{2}/"  "cont"
    rule          "cont"         "/^\s+at\s|^\s+\.\.\.\s|^\s*Caused by:/" "cont"
```

**Kubernetes metadata enrichment:**
Fluent Bit's `kubernetes` filter uses the Kubernetes API to enrich each log line with:
- `kubernetes.namespace_name`
- `kubernetes.pod_name`
- `kubernetes.container_name`
- `kubernetes.labels.*`
- `kubernetes.annotations.*`
- `kubernetes.host` (node name)

Uses a local cache with 5-minute TTL to avoid overwhelming the API server.

**Failure Modes:**
- **Pod evicted before logs flushed**: Fluent Bit tracks file position in a DB file on the host. If the container log file is rotated/deleted before Fluent Bit reads it, those lines are lost. Mitigation: Fluent Bit reads faster than log rotation (default containerd rotation: 10 MB, 5 files). Also, we set `Mem_Buf_Limit` and disk-backed buffering.
- **Kubernetes API rate limiting**: If 200 clusters each run DaemonSets querying the API server, aggregate load is high. Fluent Bit caches metadata and uses Watch API (streaming) rather than polling.
- **Multi-line mismerge**: Interleaved logs from multiple containers in the same pod can cause incorrect multi-line grouping. Solution: Fluent Bit processes each container's log file independently.

**Interviewer Q&As:**

**Q1: How do you handle log collection for init containers and short-lived Jobs?**
A: Init container logs are written to the same `/var/log/containers/` path and are tailed by Fluent Bit like any other container. For short-lived Jobs (seconds), the main risk is the Job completing and the pod being deleted before Fluent Bit processes the log file. We configure `terminationGracePeriodSeconds` generously and set Kubernetes `ttlSecondsAfterFinished` to delay pod cleanup by 300 seconds, giving Fluent Bit time to catch up.

**Q2: How does this work on bare-metal hosts outside Kubernetes?**
A: On bare-metal hosts, we deploy Filebeat via our configuration management (Ansible). Filebeat tails application log files (e.g., `/var/log/myapp/app.log`), system logs (`/var/log/syslog`, journald), and MySQL slow query logs. It adds host-level metadata (hostname, datacenter, rack) and ships to the same Kafka cluster.

**Q3: How do you handle log volume from a misbehaving pod that logs gigabytes per minute?**
A: Multiple defenses: (1) Containerd log rotation limits per-container log files (10 MB x 5 files = 50 MB max on disk). (2) Fluent Bit has `Mem_Buf_Limit` to cap memory usage; when hit, it pauses tailing that file (backpressure). (3) Logstash has per-service rate limiting. (4) We page the owning team if their log rate exceeds their allocated budget.

**Q4: Why Fluent Bit over Fluentd for the DaemonSet?**
A: Fluent Bit is written in C, uses ~10 MB RAM vs Fluentd's ~100-200 MB (Ruby). On a cluster with 500 nodes, that saves ~45 GB of aggregate RAM. Fluent Bit also has lower CPU overhead and faster startup. Fluentd's plugin ecosystem is richer, but Fluent Bit covers our needs (tail input, Kafka output, Kubernetes metadata filter, multi-line parser).

**Q5: How do you ensure no duplicate logs?**
A: Fluent Bit tracks file position in a SQLite DB on the host. After a restart, it resumes from the last committed position. However, if Fluent Bit crashes between reading and committing, a small window of duplicates exists. We use an at-least-once guarantee and rely on Elasticsearch's `_id` deduplication (hashing timestamp + host + offset) for critical pipelines.

**Q6: How do you collect logs from OpenStack control plane services?**
A: OpenStack services (Nova, Neutron, Keystone, etc.) run on bare-metal or in containers. They log to files via Python's `logging` module. We configure them to use a JSON formatter and tail the log files with Filebeat. For services running in Kubernetes (via openstack-helm), Fluent Bit captures stdout. We add OpenStack-specific metadata (project_id, request_id) via Logstash enrichment using the OpenStack request_id field.

---

### Deep Dive 4: Java Structured Logging with SLF4J + Logback

**Why it's hard:**
Java applications are the backbone of the platform (job scheduler, resource manager, API gateway). Proper structured logging with trace context propagation ensures that logs are searchable, correlated with traces, and useful during incident response. Getting this wrong means grep-unfriendly text logs, missing trace IDs, and log storms from overly verbose libraries.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Log4j2 + JSON Layout** | High performance (async loggers), native JSON | Log4j2 CVE history (Log4Shell), complex configuration |
| **SLF4J + Logback + logstash-logback-encoder** | De facto standard, well-integrated with Spring Boot, proven encoder | Slightly lower throughput than Log4j2 async |
| **Direct log to Kafka (Log4j2 Kafka appender)** | Eliminates file I/O, direct pipeline | Tight coupling, application failure if Kafka is down, no local buffer |
| **Application-level OpenTelemetry logging** | Unified with traces/metrics | API still maturing, limited ecosystem |

**Selected approach: SLF4J + Logback + logstash-logback-encoder**

**Implementation Detail:**

```xml
<!-- logback-spring.xml -->
<configuration>
  <!-- Console output (captured by K8s container runtime) -->
  <appender name="STDOUT" class="ch.qos.logback.core.ConsoleAppender">
    <encoder class="net.logstash.logback.encoder.LogstashEncoder">
      <includeMdcKeyName>trace_id</includeMdcKeyName>
      <includeMdcKeyName>span_id</includeMdcKeyName>
      <includeMdcKeyName>request_id</includeMdcKeyName>
      <customFields>{"service":"job-scheduler","version":"2.4.1"}</customFields>
      <timestampPattern>yyyy-MM-dd'T'HH:mm:ss.SSSZ</timestampPattern>
    </encoder>
  </appender>

  <!-- Async wrapper for non-blocking logging -->
  <appender name="ASYNC_STDOUT" class="ch.qos.logback.classic.AsyncAppender">
    <queueSize>8192</queueSize>
    <discardingThreshold>20</discardingThreshold> <!-- Drop DEBUG/TRACE at 80% full -->
    <neverBlock>true</neverBlock>
    <appender-ref ref="STDOUT" />
  </appender>

  <!-- Suppress noisy third-party libraries -->
  <logger name="org.apache.kafka" level="WARN" />
  <logger name="org.hibernate" level="WARN" />
  <logger name="com.zaxxer.hikari" level="INFO" />

  <root level="INFO">
    <appender-ref ref="ASYNC_STDOUT" />
  </root>
</configuration>
```

**MDC (Mapped Diagnostic Context) for trace propagation:**
```java
// Filter that extracts trace context from incoming requests
public class TraceContextFilter implements Filter {
    @Override
    public void doFilter(ServletRequest req, ServletResponse res, FilterChain chain) {
        HttpServletRequest httpReq = (HttpServletRequest) req;
        String traceId = httpReq.getHeader("X-Trace-Id");
        if (traceId == null) {
            traceId = UUID.randomUUID().toString().replace("-", "");
        }
        String spanId = generateSpanId();

        MDC.put("trace_id", traceId);
        MDC.put("span_id", spanId);
        try {
            chain.doFilter(req, res);
        } finally {
            MDC.clear(); // Prevent leaking to thread pool reuse
        }
    }
}

// Usage in application code
private static final Logger log = LoggerFactory.getLogger(JobScheduler.class);

public void scheduleJob(Job job) {
    log.info("Scheduling job", kv("job_id", job.getId()), kv("priority", job.getPriority()));
    // Produces: {"@timestamp":"...","level":"INFO","logger":"...JobScheduler",
    //            "message":"Scheduling job","job_id":"j-98234","priority":"HIGH",
    //            "trace_id":"4bf92f...","span_id":"00f067...","service":"job-scheduler"}
}
```

**Key structured logging patterns:**
- Use `kv()` (key-value) markers from logstash-logback-encoder for structured fields.
- Never use string concatenation in log messages: `log.info("Processing job " + jobId)` -- this evaluates the string even when INFO is disabled for that logger.
- Use parameterized logging: `log.debug("Processing job {}", jobId)` -- string construction is deferred.

**Failure Modes:**
- **MDC leaking in async thread pools**: MDC is ThreadLocal-based. When using `@Async` or `CompletableFuture`, MDC is not automatically propagated. Solution: Use `MDCAdapter` or OpenTelemetry's context propagation which wraps executors.
- **AsyncAppender queue full**: If logging rate exceeds appender throughput, the async queue fills. With `neverBlock: true`, events are dropped (starting with DEBUG/TRACE at 80% capacity). This prevents application threads from blocking on logging.
- **Log4Shell-type vulnerabilities**: We use Logback (not Log4j2), which was not affected. However, we maintain a BOM (Bill of Materials) to ensure no transitive Log4j2 dependencies and scan with Snyk/Trivy.

**Interviewer Q&As:**

**Q1: How does MDC work with reactive frameworks (Spring WebFlux)?**
A: MDC is ThreadLocal-based and does not work with reactive (non-blocking) frameworks where a request may be processed across multiple threads. For WebFlux, we use Reactor's Context to carry trace information and hook into Logback via a custom `MdcContextLifter` that copies Reactor Context to MDC before each logging call.

**Q2: What is the performance impact of JSON structured logging vs plain text?**
A: JSON encoding adds ~5-10% overhead compared to pattern-based text logging due to field serialization. However, this is negligible compared to I/O cost. The benefit is enormous: structured logs are directly indexable by Elasticsearch without Logstash parsing (grok patterns), which saves significant CPU on the processing pipeline.

**Q3: How do you handle log level changes in production without redeploying?**
A: Spring Boot Actuator exposes `/actuator/loggers` endpoint which allows dynamic log level changes via POST request. For fleet-wide changes, we use a centralized configuration service (Spring Cloud Config or Kubernetes ConfigMap + Logback's scan feature with `scanPeriod="30 seconds"`).

**Q4: How do you prevent sensitive data (passwords, tokens) from appearing in logs?**
A: Multiple layers: (1) Custom Logback `MaskingPatternLayout` that regex-replaces patterns matching credit cards, SSNs, API keys. (2) Code review guidelines prohibiting logging of request bodies containing auth fields. (3) Logstash pipeline filter that redacts known sensitive field names. (4) Static analysis tools (e.g., Semgrep rules) that flag `log.info("password=" + ...)` patterns.

**Q5: Why write to stdout instead of directly to Kafka from the application?**
A: Writing to stdout is the Twelve-Factor App pattern and is idiomatic for Kubernetes. Benefits: (1) No Kafka client dependency in every application. (2) If Kafka is down, the application still runs -- logs are buffered by the container runtime. (3) Single collection mechanism (Fluent Bit DaemonSet) for all applications regardless of language. (4) Simpler application configuration.

**Q6: How do you handle multi-line Java stack traces in structured JSON logging?**
A: With logstash-logback-encoder, stack traces are serialized as a single JSON field (`stack_trace`), so the entire exception is one JSON object on one line. No multi-line parsing is needed at the collector level. This is a major advantage of structured logging over plain text.

---

## 7. Scaling Strategy

### Horizontal Scaling Dimensions

| Component | Scaling Mechanism | Trigger |
|---|---|---|
| **Fluent Bit** | DaemonSet (auto-scales with nodes) | New node added to cluster |
| **Kafka** | Add brokers + rebalance partitions | Disk utilization > 70% or consumer lag growing |
| **Logstash** | Kubernetes Deployment with HPA | Consumer lag > 10M messages or CPU > 70% |
| **Elasticsearch hot** | Add data nodes | Shard count per node > 600 or indexing latency > 200ms |
| **Elasticsearch warm** | Add data nodes | Disk utilization > 80% |
| **Elasticsearch cold** | Add frozen-tier nodes | Query latency on cold tier > 30s |

### Elasticsearch Scaling Details

- **Shard count management**: Target 20-40 shards per GB of JVM heap on data nodes. With 64 GB heap nodes, max ~2,500 shards per node.
- **Coordinating nodes**: Dedicated nodes that handle query fan-out/merge. Scale to 10+ for high query load.
- **Master nodes**: 3 dedicated masters (never data nodes) with 16 GB heap. Master stability is critical.
- **Cross-cluster search**: For multi-region deployments, use Elasticsearch cross-cluster search (CCS) instead of replicating all data.

**Interviewer Q&As:**

**Q1: How do you handle a 10x traffic spike during a global incident?**
A: The pipeline is designed for 5x peak, so 10x requires active intervention: (1) Kafka absorbs the burst for up to 48h. (2) Emergency sampling: increase DEBUG sampling to 0.1% and INFO sampling to 10% via Logstash config update. (3) Auto-scale Logstash consumers (HPA reacts in ~2 minutes). (4) If ES is the bottleneck, temporarily disable replicas on hot indices (`number_of_replicas: 0`) to double write throughput. (5) After the incident, replay full-fidelity logs from Kafka if needed.

**Q2: How do you scale Elasticsearch indexing throughput?**
A: Multiple techniques: (1) Increase `refresh_interval` from 5s to 30s during high load (delays searchability but increases throughput). (2) Use `translog.durability: async` with 5s sync interval (small data loss risk on crash). (3) Ensure bulk request size is 5-15 MB (optimal for ES). (4) Add more data nodes and increase shard count. (5) Use `_routing` to avoid cross-shard queries for common access patterns.

**Q3: What is the biggest scaling bottleneck you have seen in ELK?**
A: Elasticsearch cluster state size. Every index, shard, and field mapping contributes to cluster state which is replicated to all nodes. At 14,000+ indices (daily indices x services x 30 days), cluster state can exceed 500 MB, causing slow master elections and node joins. Solution: reduce index count using data streams with fewer backing indices, and aggressively delete old indices.

**Q4: How do you handle multi-region log aggregation?**
A: Each region has its own ELK stack for latency and availability. Cross-region queries use Elasticsearch cross-cluster search (CCS): a coordinating node in region A sends sub-queries to region B's cluster and merges results. We do not replicate logs cross-region (too expensive). For global dashboards, we use a dedicated aggregation cluster that receives sampled (1%) logs from all regions.

**Q5: How do you capacity plan for Elasticsearch?**
A: Empirically. We run load tests indexing representative log data and measure: (1) GB indexed per node per second, (2) search latency under concurrent indexing load, (3) merge rate and disk I/O saturation. From this, we derive a "GB per node per day" capacity and plan node count accordingly. Typical: one hot-tier node (32 cores, 64 GB RAM, 8 TB NVMe) handles ~2 TB/day indexing with acceptable search latency.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| **Single ES data node failure** | Shards on that node become unassigned; replicas promote to primary | Cluster health yellow; `unassigned_shards` metric | ES auto-reallocates. Replica ensures no data loss. | ~5 min (reallocation) |
| **ES master node failure** | If quorum lost (2/3 down), cluster read-only | Master election timeout, cluster health red | 3 dedicated masters; majority quorum. Replace failed master. | ~30 sec (election) |
| **Kafka broker failure** | Partitions on broker unavailable until ISR catches up | Under-replicated partitions metric | RF=3, min.ISR=2. Automatic leader election. | ~10 sec |
| **All Logstash instances down** | Ingestion pipeline stopped; Kafka buffers | Consumer lag growing; zero indexing rate | Kubernetes restarts pods. Kafka retains 48h. Replay on recovery. | ~2 min (pod restart) |
| **Fluent Bit DaemonSet crash on a node** | Logs from that node stop flowing | Heartbeat missing from that host | DaemonSet auto-restarts. Disk-backed buffer prevents loss. | ~30 sec |
| **Kafka cluster total outage** | All log ingestion stops | Producer errors, zero Kafka throughput | Fluent Bit disk buffer (2 GB per node). Producers retry. Manual failover to secondary Kafka cluster. | ~15 min (failover) |
| **S3 outage** | Cold tier queries fail; new snapshots fail | ES logs S3 errors, cold query failures | Hot/warm tiers unaffected. ILM pauses cold transition. Retry on S3 recovery. | Depends on AWS |
| **Network partition (collectors ↔ Kafka)** | Logs buffer locally, eventual delivery | Producer error rate spikes | Fluent Bit disk buffer. Collectors retry with exponential backoff. | Self-healing on partition recovery |
| **Elasticsearch mapping conflict** | Index creation fails for that service's logs | Index creation errors in Logstash | Dead letter queue captures events. Fix mapping and reindex. | ~30 min (manual fix) |
| **Disk full on ES data node** | Node goes read-only (flood-stage watermark) | Disk watermark alerts (85% high, 90% flood) | ILM deletes old indices. Emergency: add nodes or increase disk. | ~15 min |

### Durability Guarantees

| Log Level | Guarantee | Mechanism |
|---|---|---|
| ERROR, FATAL | At-least-once delivery, durable | Kafka acks=all on high-priority topic, ES replica=1 |
| WARN, INFO | At-least-once, best-effort | Kafka acks=1, ES replica=1 |
| DEBUG, TRACE | Sampled (1%), best-effort | Sampling at collector; dropped if buffer full |

---

## 9. Security

### Authentication & Authorization
- **Elasticsearch**: X-Pack Security or OpenSearch Security plugin. RBAC with roles per team.
  - `logs-reader-{team}`: Can search indices matching `logs-{team's-services}-*`.
  - `logs-admin`: Can manage ILM policies, templates, and cluster settings.
  - `logs-superuser`: Platform team only, full cluster access.
- **Kibana**: SSO via SAML/OIDC (integrated with corporate IdP). Spaces per team with index pattern restrictions.
- **Kafka**: TLS encryption in transit, SASL/SCRAM authentication for producers and consumers. ACLs per topic.

### Encryption
- **In transit**: TLS 1.3 for all inter-component communication (collectors → Kafka, Kafka → Logstash, Logstash → ES, ES inter-node).
- **At rest**: Elasticsearch data directories on encrypted volumes (dm-crypt/LUKS on bare-metal, EBS encryption on cloud). S3 server-side encryption (SSE-S3 or SSE-KMS) for cold storage.

### Data Sensitivity
- **PII masking**: Logstash pipeline includes a `mutate` filter that regex-replaces known PII patterns (email, SSN, credit card numbers) before indexing.
- **Log classification**: Sensitive services (auth, payment) have separate indices with restricted access.
- **Retention enforcement**: ILM enforces deletion; no manual extension without compliance approval.

### Network Security
- Elasticsearch cluster on isolated VLAN, accessible only from Logstash/Kibana nodes and admin bastion.
- Kafka cluster on separate VLAN, accessible from collectors and Logstash.
- Kibana exposed via internal load balancer only (no public internet access).

---

## 10. Incremental Rollout

### Rollout Phases

| Phase | Scope | Duration | Success Criteria |
|---|---|---|---|
| **Phase 0: Shadow** | Deploy pipeline in parallel with existing system for 1 team | 2 weeks | Zero data loss, latency within SLO |
| **Phase 1: Early Adopters** | 5 teams, non-critical services | 4 weeks | < 1% log loss, positive feedback on search |
| **Phase 2: Platform Services** | All infrastructure services (scheduler, resource manager) | 4 weeks | Full trace correlation working, alerting integrated |
| **Phase 3: General Availability** | All teams and services | 8 weeks | Decommission legacy system |
| **Phase 4: Advanced Features** | Anomaly detection on logs, AI-assisted search | Ongoing | Measurable reduction in MTTR |

### Rollout Q&As

**Q1: How do you migrate from an existing logging system without losing data?**
A: Dual-write during migration. Collectors send to both old and new Kafka clusters. We run both systems in parallel until the new system is validated. After cutover, we keep the old system read-only for 30 days for historical access.

**Q2: How do you onboard a new team to the logging platform?**
A: Self-service workflow: (1) Team registers their services in the service registry. (2) Platform auto-creates Elasticsearch index template, ILM policy, and Kibana space for their namespace. (3) Team configures their application to output structured JSON to stdout. (4) Fluent Bit DaemonSet automatically picks up their logs. No ticket required for standard onboarding.

**Q3: How do you handle the chicken-and-egg problem of monitoring the monitoring system?**
A: The logging infrastructure itself uses a separate, minimal monitoring stack: (1) Prometheus for metrics (ES health, Kafka lag, pipeline throughput). (2) A small standalone Elasticsearch cluster (3 nodes) for logging infrastructure logs. (3) PagerDuty direct integration for critical alerts. We never depend on the main logging system to monitor itself.

**Q4: What is your canary deployment strategy for pipeline changes?**
A: Logstash pipeline changes are deployed to a canary consumer group that processes 1% of traffic (using Kafka consumer group assignment). We compare canary output against the production consumer group. If the canary produces no parsing errors and output matches expected schema, we roll out to production consumer group.

**Q5: How do you handle rollback if the new system has issues?**
A: Since we dual-write during migration, rollback is simply routing users back to the old Kibana/old system. No data is lost. For pipeline changes (Logstash config), we keep the previous version deployed as a standby consumer group and can switch back by pausing the new group and resuming the old group.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale |
|---|---|---|---|
| **Log buffer** | Redis, Kafka, direct-to-ES | Kafka | Durability, replay, scale, multiple consumers |
| **Search engine** | Elasticsearch, ClickHouse, Loki | Elasticsearch | Best full-text search, mature ecosystem, Kibana |
| **Collector (K8s)** | Fluent Bit, Fluentd, Filebeat | Fluent Bit | Lowest resource footprint (C binary, ~10 MB RAM) |
| **Collector (bare-metal)** | Filebeat, Fluentd, rsyslog | Filebeat | Native file tailing, lightweight, good ES integration |
| **Structured format** | JSON, logfmt, key=value | JSON | Native to ES, language support, human-readable |
| **Sampling strategy** | No sampling, head-based, tail-based | Head-based 1% for DEBUG | Simple, predictable, saves ~50% storage |
| **ES replication** | 0, 1, 2 replicas | 1 replica | Balance between durability and storage cost |
| **Dynamic mapping** | Enabled, disabled, strict | Disabled (`dynamic: false`) | Prevents field explosion while allowing `_source` storage |
| **Kafka acks for logs** | acks=0, acks=1, acks=all | acks=1 (acks=all for errors) | Throughput for INFO; durability for ERROR |
| **Java logging** | Log4j2, Logback, JUL | Logback + logstash-encoder | Spring Boot default, no CVE history, proven encoder |

---

## 12. Agentic AI Integration

### AI-Powered Log Analysis

**Use Case 1: Intelligent Log Search (Natural Language → Query)**
```
User: "Show me all errors from the job scheduler in the last hour related to GPU allocation"

AI Agent → Generates Elasticsearch query:
{
  "query": {
    "bool": {
      "must": [
        {"term": {"service": "job-scheduler"}},
        {"term": {"level": "ERROR"}},
        {"match": {"message": "GPU allocation"}}
      ],
      "filter": [
        {"range": {"@timestamp": {"gte": "now-1h"}}}
      ]
    }
  }
}
```

**Use Case 2: Automated Root Cause Analysis**
An LLM agent receives a PagerDuty alert, queries logs for the affected service and correlated services (via trace_id), and generates a summary:

```
INCIDENT SUMMARY (generated by AI agent):
- Alert: job-scheduler ERROR rate exceeded 100/min
- Time window: 14:20-14:35 UTC
- Root cause: GPU resource pool "gpu-a100-us-east" exhausted at 14:18
- Contributing factor: ML training job batch-7234 requested 64 GPUs (entire pool)
- Impact: 47 jobs failed with ResourceExhaustedException
- Correlated events:
  - resource-manager log at 14:18: "Pool gpu-a100-us-east capacity 0/64"
  - quota-service log at 14:15: "Approved quota increase for team ml-training"
- Suggested action: Review quota approval for team ml-training; consider pool reservation limits
```

**Use Case 3: Anomaly Detection in Log Patterns**
- Train an embedding model on historical log messages.
- For each incoming log batch, compute embedding similarity to historical baseline.
- Flag messages that are semantically novel (low cosine similarity to any cluster centroid).
- Example: a new type of error message appears that has never been seen before → immediate alert.

**Use Case 4: Automated Runbook Execution**
```
AI Agent workflow:
1. Alert fires: "Elasticsearch cluster yellow - unassigned shards"
2. Agent queries: GET _cluster/allocation/explain
3. Agent identifies: Node bm-042 has insufficient disk (92% full)
4. Agent executes runbook:
   a. Verify node disk usage (confirmed 92%)
   b. Trigger ILM force-rollover on hot indices
   c. Increase disk watermark temporarily
   d. Page on-call if not resolved in 10 minutes
5. Agent posts status update to incident channel
```

**Architecture for AI Integration:**
```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ Alert/Trigger │────▶│  AI Agent    │────▶│ Action       │
│ (PagerDuty,  │     │  (LLM +      │     │ Executor     │
│  manual)      │     │  RAG over    │     │ (APIs, CLIs) │
│              │     │  logs/docs)  │     │              │
└──────────────┘     └──────┬───────┘     └──────────────┘
                            │
                    ┌───────▼───────┐
                    │ Knowledge Base │
                    │ - Runbooks    │
                    │ - Past incidents│
                    │ - Service map │
                    │ - Log patterns│
                    └───────────────┘
```

**Guardrails:**
- AI agents operate in read-only mode by default; destructive actions require human approval.
- All AI-suggested queries and actions are logged to the audit system.
- Confidence scoring: agent must report confidence level; < 80% confidence triggers human escalation.
- Rate limiting: agent cannot execute more than 5 remediation actions per incident without human approval.

---

## 13. Complete Interviewer Q&A Bank

### Architecture & Design (Q1-Q5)

**Q1: Walk me through what happens when a Java microservice logs an ERROR in a Kubernetes pod.**
A: (1) Application calls `log.error(...)` which goes through SLF4J → Logback → logstash-logback-encoder → JSON string written to stdout. (2) Kubernetes container runtime (containerd) captures stdout and writes to `/var/log/containers/{pod}_{ns}_{container}-{id}.log` with CRI log format prefix (timestamp, stream, tag). (3) Fluent Bit DaemonSet tails this file, strips the CRI prefix, parses the JSON, adds Kubernetes metadata (namespace, pod, labels) from the API server cache. (4) Fluent Bit batches and compresses events, sends to Kafka topic `logs.raw` (or `logs.high-priority` for ERROR). (5) Logstash consumer group reads from Kafka, enriches with team/owner from service registry, routes to the correct Elasticsearch index (`logs-{service}-YYYY.MM.DD`). (6) Elasticsearch indexes the document, makes it searchable within `refresh_interval` (5 seconds). (7) ElastAlert runs its periodic query, matches the ERROR pattern, and fires an alert if the error rate exceeds the threshold. Total end-to-end latency: < 30 seconds.

**Q2: How would you design the system differently if you had 10x the current scale?**
A: At 600 TB/day: (1) Switch from Elasticsearch to ClickHouse for most aggregation queries (columnar storage is 5-10x more space efficient), keeping ES only for full-text search on a subset. (2) Implement aggressive log summarization: rather than storing every log line, extract key entities and relationships and store summaries. (3) Use tiered Kafka with remote storage (KIP-405 tiered storage) to extend retention without proportional disk. (4) Regional pipelines with global search federation. (5) Push more processing to the edge (collectors do parsing, enrichment, and sampling before sending).

**Q3: How do you ensure consistency between logs and traces?**
A: The key is the `trace_id` field. OpenTelemetry Java agent or our MDC filter injects `trace_id` and `span_id` into every log line. When a user clicks on a trace in Jaeger, a link takes them to Kibana with `trace_id:{id}` query, showing all logs for that trace. Conversely, from a log line in Kibana, a link to Jaeger shows the trace. Consistency is ensured by both systems using the same W3C TraceContext propagation and the same `trace_id` format (32 hex characters).

**Q4: What would you change if budget were cut by 50%?**
A: (1) Switch from Elasticsearch to Grafana Loki for most services (indexes only labels, 10x cheaper storage). Keep ES only for critical services needing full-text search. (2) Reduce hot tier from 7 days to 3 days. (3) Increase DEBUG sampling from 1% to 0.1%. (4) Reduce Kafka retention from 48h to 12h. (5) Use cheaper hardware for warm tier (denser storage, less RAM). (6) Drop replica count to 0 for warm tier (rely on Kafka and S3 for durability).

**Q5: How do you handle log data during a Kubernetes cluster upgrade?**
A: During a rolling node upgrade: (1) Node is cordoned and drained. (2) Pods are evicted, but Fluent Bit DaemonSet keeps running until the node is actually shut down. (3) Fluent Bit's file position DB and disk buffer on the host survive pod restart (hostPath volume). (4) When the node comes back up, the new Fluent Bit DaemonSet pod resumes from the saved position. (5) If the node's /var/log is wiped during upgrade, logs written between Fluent Bit's last flush and the wipe are lost -- this is a known trade-off. Mitigation: applications writing critical logs should use Kafka directly for those specific events.

### Performance & Optimization (Q6-Q10)

**Q6: How do you optimize Elasticsearch query performance for log search?**
A: (1) Always include a time range filter (most queries scan only recent data). (2) Use `keyword` type for structured fields and `text` only for `message`. (3) `_routing` by service ensures a service's logs are colocated on the same shard, reducing fan-out. (4) Use `filter` context (not `must`) for terms that do not need scoring -- `filter` is cached. (5) Avoid wildcards at the beginning of terms (`*error` is expensive; `error*` is fast). (6) Pre-aggregate in Kibana dashboards using date histograms with appropriate intervals. (7) Use `search_after` for deep pagination instead of `from/size`. (8) Create index aliases for commonly queried time ranges.

**Q7: What is the impact of Elasticsearch `refresh_interval` on performance?**
A: Refresh creates a new Lucene segment, making recently indexed documents searchable. Default 1s is expensive: each refresh triggers segment creation, file handle allocation, and eventually merging. For log ingestion at our scale, we set `refresh_interval: 5s` (hot tier) and `30s` (during catch-up/reindexing). Each 1s→5s change improves indexing throughput by approximately 20-30%. Trade-off: logs are searchable 5 seconds after indexing instead of 1 second.

**Q8: How do you handle Elasticsearch segment merging under heavy load?**
A: Segment merging is the most I/O intensive operation in Elasticsearch. Under heavy indexing, many small segments accumulate and must be merged into larger ones. We tune: (1) `index.merge.scheduler.max_thread_count`: limit concurrent merges to avoid saturating I/O (set to max(1, min(4, disk_count / 2))). (2) Use SSDs for hot tier (handles merge I/O better). (3) Force-merge on warm tier to 1 segment eliminates future merges on read-only data. (4) Monitor merge rate via `_nodes/stats/indices/merges` and alert if merge queue grows.

**Q9: How do you debug slow queries in Elasticsearch?**
A: (1) Enable slow search log: `index.search.slowlog.threshold.query.warn: 5s`. (2) Use `_search` with `profile: true` to see per-shard query execution breakdown. (3) Common causes: wildcard prefix queries, large result sets, queries spanning too many indices (missing time filter), high fan-out (too many shards). (4) Check `_cluster/pending_tasks` for master bottleneck. (5) Check `_nodes/hot_threads` for CPU-bound operations.

**Q10: What is the performance impact of enabling `_source` on all log documents?**
A: `_source` stores the original JSON document, typically consuming 60-80% of total index size (the rest is inverted index and doc values). Disabling `_source` would save significant disk, but we cannot: (1) We need to display log lines in Kibana (requires `_source`). (2) Reindexing requires `_source`. (3) Update-by-query requires `_source`. Instead, we use `best_compression` codec (zstd-like) which reduces `_source` size by ~30% at the cost of slightly slower reads.

### Operations & Reliability (Q11-Q15)

**Q11: How do you handle an Elasticsearch cluster split-brain?**
A: Modern Elasticsearch (7.x+) uses a quorum-based master election that prevents split-brain by design. With 3 dedicated master-eligible nodes, a quorum of 2 is required to elect a master. If a network partition splits them 2-1, only the side with 2 nodes can elect a master; the isolated node goes read-only. Pre-7.x clusters needed `minimum_master_nodes` set to `(master_count / 2) + 1`. We always use dedicated master nodes (never data+master) to ensure master stability.

**Q12: Walk me through a real incident response using this logging system.**
A: Scenario: Users report job submission failures. (1) On-call receives PagerDuty alert: "job-scheduler ERROR rate > 100/min". (2) Opens Kibana, filters `service:job-scheduler AND level:ERROR`, sees "ResourceExhaustedException" messages. (3) Clicks `trace_id` link on one error log → opens Jaeger, sees the full trace: `api-gateway → job-scheduler → resource-manager`. (4) The resource-manager span shows `status: RESOURCE_EXHAUSTED`. (5) Back in Kibana, searches `service:resource-manager AND message:"pool exhausted"`, finds the root cause: GPU pool capacity hit zero at 14:18. (6) Searches for who consumed the pool: `service:resource-manager AND message:"allocated" AND resource_pool:gpu-a100`, finds team ml-training allocated 64 GPUs. (7) Resolution: contact ml-training team to release unused GPUs, implement pool reservation limits.

**Q13: How do you handle upgrades to the Elasticsearch cluster?**
A: Rolling upgrade: (1) Disable shard allocation: `PUT _cluster/settings {"transient": {"cluster.routing.allocation.enable": "primaries"}}`. (2) Stop indexing to the node (Logstash excludes it from bulk targets). (3) Flush node: `POST _flush/synced`. (4) Upgrade node, restart. (5) Wait for node to join cluster. (6) Re-enable allocation. (7) Wait for shards to rebalance (green). (8) Repeat for next node. For major version upgrades (7→8), we use a blue-green approach: build a new cluster, reindex from the old one, and cut over.

**Q14: How do you handle the failure of the entire logging pipeline during a critical incident?**
A: This is the worst-case scenario -- you need logs most when logging is down. Mitigations: (1) Applications still write to stdout; logs are on the host disk (containerd log files) and can be accessed directly via `kubectl logs` or `ssh + tail`. (2) We maintain an independent "emergency logging" path: a small 3-node Elasticsearch cluster receiving sampled ERROR-only logs via a separate collector pipeline. (3) Operators have scripts to run distributed grep across hosts (pssh + grep). (4) Post-incident: once the main pipeline is restored, replay from Kafka or S3 backfill.

**Q15: How do you prevent log injection attacks?**
A: Log injection is when an attacker includes malicious content in input that ends up in logs (e.g., fake log lines, ANSI escape sequences, or JNDI lookups like Log4Shell). Defenses: (1) Structured JSON logging prevents log line injection -- user input is always a value in a JSON field, never raw text in the log stream. (2) Logstash sanitizes fields: strips control characters and ANSI codes. (3) Kibana auto-escapes HTML in displayed log values. (4) We use Logback (not Log4j2) which was not vulnerable to JNDI lookup injection. (5) Input validation at the application layer before logging user-controlled data.

### Advanced Topics (Q16-Q20)

**Q16: Compare ELK, Grafana Loki, and ClickHouse for log storage.**
A: **ELK (Elasticsearch)**: Full-text inverted index, sub-second search on any field, highest storage cost (~$15/GB/month hot). Best for: ad-hoc debugging, full-text search, complex queries. **Grafana Loki**: Index-free design (indexes only labels like service, level), stores compressed log chunks, much cheaper storage (~$3/GB/month). Best for: Kubernetes-centric, label-based filtering, cost optimization. Worse at: arbitrary text search. **ClickHouse**: Columnar database with excellent compression and fast aggregation. Best for: log analytics (counts, rates, percentiles over time). Worse at: full-text search (no inverted index, relies on regex scan). **Our choice**: Elasticsearch for primary search (infrastructure platform needs arbitrary text search for debugging); consider Loki for cost optimization on lower-priority services.

**Q17: How would you implement multi-tenancy in the logging system?**
A: (1) **Index-level isolation**: Each team's services write to their own index pattern (`logs-{team}-{service}-*`). (2) **Elasticsearch RBAC**: Each team's role grants access only to their index patterns. (3) **Kibana Spaces**: Each team has a Kibana space with pre-configured index patterns. (4) **Kafka topic ACLs**: Teams can only produce to their topic. (5) **Resource quotas**: ILM policies per team enforce retention and storage limits. A team exceeding their log budget gets automatic sampling applied. (6) **Cross-team access**: Platform/SRE team has a super-reader role that can query all indices for incident response.

**Q18: How do you handle schema evolution in log events?**
A: (1) Log events are schemaless JSON -- new fields can be added freely (stored in `_source` even if not indexed). (2) To make a new field searchable, we update the index template. New indices created after the template update have the new mapping; old indices do not (historical data lacks the field). (3) For breaking changes (e.g., field type change from `text` to `keyword`), we create a new field name (e.g., `status_code_v2`) and deprecate the old one. (4) Schema registry (optional): register log schemas in a Confluent Schema Registry; Logstash validates against the schema and routes non-conforming events to a DLQ.

**Q19: How do you handle log collection for serverless / short-lived functions?**
A: Serverless functions (AWS Lambda, OpenFaaS, Knative) cannot run a sidecar and may be too short-lived for file-based collection. Approach: (1) Function writes structured JSON to stdout. (2) Platform captures stdout via the runtime's log drain (CloudWatch Logs for Lambda, Knative's built-in log collection). (3) A bridge component (Lambda extension, Fluentd CloudWatch input) ships logs to our Kafka pipeline. (4) For ultra-short functions (<100ms), we batch logs in memory and flush on function shutdown via a shutdown hook.

**Q20: What observability do you have on the logging pipeline itself?**
A: Meta-monitoring metrics: (1) **Fluent Bit**: `fluentbit_input_records_total`, `fluentbit_output_errors_total`, `fluentbit_output_retries_total`, buffer usage. (2) **Kafka**: consumer lag per group, produce/fetch latency, under-replicated partitions, broker disk usage. (3) **Logstash**: events in/out/filtered per second, pipeline worker utilization, DLQ size. (4) **Elasticsearch**: indexing rate, search latency, merge rate, JVM heap usage, circuit breaker trips, pending tasks, shard count. All exposed as Prometheus metrics, dashboarded in Grafana, with PagerDuty alerts for critical thresholds. The monitoring pipeline is independent of the logging pipeline.

---

## 14. References

1. **Elasticsearch: The Definitive Guide** - Clinton Gormley, Zachary Tong (O'Reilly)
2. **Elasticsearch ILM Documentation**: https://www.elastic.co/guide/en/elasticsearch/reference/current/index-lifecycle-management.html
3. **Fluent Bit Documentation**: https://docs.fluentbit.io/
4. **Kafka: The Definitive Guide** - Neha Narkhede, Gwen Shapira, Todd Palino (O'Reilly)
5. **SLF4J + Logback Manual**: https://logback.qos.ch/manual/
6. **logstash-logback-encoder**: https://github.com/logfellow/logstash-logback-encoder
7. **Elasticsearch Searchable Snapshots**: https://www.elastic.co/guide/en/elasticsearch/reference/current/searchable-snapshots.html
8. **Google SRE Book - Chapter 17: Testing for Reliability**: https://sre.google/sre-book/
9. **OpenTelemetry Logging Specification**: https://opentelemetry.io/docs/specs/otel/logs/
10. **Kubernetes Logging Architecture**: https://kubernetes.io/docs/concepts/cluster-administration/logging/
