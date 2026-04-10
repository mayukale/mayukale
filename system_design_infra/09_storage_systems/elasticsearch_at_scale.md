# System Design: Elasticsearch at Scale

> **Relevance to role:** Cloud infrastructure platform engineers operate Elasticsearch clusters that power log analytics, full-text search, observability dashboards, and application search. For this role, understanding cluster topology (hot/warm/cold), shard sizing strategy, ILM (Index Lifecycle Management), ingestion pipelines, and the Java Elasticsearch client is critical for operating ES clusters on bare metal that serve Java/Python services at scale.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | Document indexing | Index JSON documents with automatic or explicit mapping |
| FR-2 | Full-text search | Boolean queries, phrase match, fuzzy, wildcard, scoring |
| FR-3 | Structured queries | Term, range, exists filters on keyword/numeric/date fields |
| FR-4 | Aggregations | Terms, date_histogram, percentiles, nested, composite |
| FR-5 | Near-real-time search | Documents searchable within 1 second of indexing |
| FR-6 | Index lifecycle management | Hot → warm → cold → frozen → delete |
| FR-7 | Ingest pipelines | Preprocessing (grok, date parsing, enrichment) before indexing |
| FR-8 | Alerting/Watcher | Periodic query evaluation with alerting actions |
| FR-9 | Cross-cluster search | Query across multiple ES clusters in different regions |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Indexing throughput | 500K documents/sec (50 KB avg) |
| NFR-2 | Search latency | p50 < 50 ms, p99 < 500 ms |
| NFR-3 | Aggregation latency | p50 < 200 ms, p99 < 2 s |
| NFR-4 | Availability | 99.9% for search, 99.5% for indexing |
| NFR-5 | Data retention | 30 days hot, 90 days warm, 1 year cold |
| NFR-6 | Index size | Up to 10 TB per index (time-based indices) |
| NFR-7 | Cluster size | 50-200 nodes |

### Constraints & Assumptions
- Bare-metal servers: hot nodes (NVMe), warm nodes (SSD), cold nodes (HDD)
- Primary use cases: log analytics (80%), application search (15%), metrics (5%)
- Documents are primarily JSON logs from Java/Python services
- Kafka as the ingestion buffer between log producers and ES
- Java Elasticsearch client (8.x) for application integration
- OpenStack tenant logs isolated by index prefix

### Out of Scope
- Elasticsearch as a primary database (document store)
- Machine learning features (anomaly detection)
- Security analytics (SIEM)
- Elastic APM

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Log sources | 5,000 hosts + 20,000 containers | 25,000 sources |
| Avg log rate per source | 20 logs/sec (application + system) | 20 |
| Total log rate | 25,000 × 20 | 500K docs/sec |
| Avg document size | 50 KB (JSON, multi-line stack traces) | 50 KB |
| Daily ingest volume | 500K/sec × 86,400s × 50 KB | ~2.1 TB/day raw |
| Daily index size (with overhead) | 2.1 TB × 1.1 (index metadata ~10% overhead) | ~2.3 TB/day |
| Search queries per second | 200 dashboard users × 5 queries/10s | ~100 QPS |
| Aggregation queries per second | 50 heavy users × 1 agg/10s | ~5 QPS |

### Latency Requirements

| Operation | p50 | p99 |
|-----------|-----|-----|
| Index (single doc) | 5 ms | 20 ms |
| Bulk index (1000 docs) | 50 ms | 200 ms |
| Simple search (filter) | 10 ms | 100 ms |
| Full-text search (scoring) | 30 ms | 300 ms |
| Terms aggregation (1 index) | 50 ms | 500 ms |
| Date histogram (30 day range) | 200 ms | 2 s |
| Deep pagination (after 10K) | 100 ms | 1 s |

### Storage Estimates

| Component | Calculation | Value |
|-----------|-------------|-------|
| Hot tier (30 days) | 2.3 TB/day × 30 | ~69 TB |
| Warm tier (90 days, force-merged) | 69 TB (transferred from hot; ~20% smaller after merge) | ~55 TB |
| Cold tier (1 year, compressed) | 2.3 TB/day × 365 × 0.5 (compression) | ~420 TB |
| Total storage | Hot + Warm + Cold | ~544 TB |
| Hot nodes (4 TB NVMe × 80% util) | 69 TB / (4 TB × 0.8) | ~22 nodes |
| Warm nodes (8 TB SSD × 80% util) | 55 TB / (8 TB × 0.8) | ~9 nodes |
| Cold nodes (24 TB HDD × 80% util) | 420 TB / (24 TB × 0.8) | ~22 nodes |
| Master nodes | 3 (dedicated, small instances) | 3 |
| Coordinating nodes | 5 (for query routing) | 5 |
| **Total cluster** | 22 + 9 + 22 + 3 + 5 | **61 nodes** |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Ingest (Kafka → ES) | 500K/sec × 50 KB | ~25 GB/s |
| Replication traffic | 25 GB/s × 1 (1 replica) | ~25 GB/s |
| Search response | 100 QPS × 100 KB avg response | ~10 MB/s |
| ILM migration (hot→warm) | 2.3 TB/day moved during off-peak | ~200 MB/s burst |
| Shard relocation | Periodic rebalance | ~100 MB/s |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        LOG PRODUCERS                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────────┐  │
│  │ Java App │  │ Python   │  │ System   │  │ OpenStack          │  │
│  │ (logback │  │ (structlog│  │ (rsyslog/│  │ services           │  │
│  │  + JSON) │  │  + JSON)  │  │ journald)│  │ (oslo.log)         │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬─────────────┘  │
└───────┼──────────────┼──────────────┼───────────────┼────────────────┘
        │              │              │               │
        └──────────────┴──────────────┴───────────────┘
                              │
                    ┌─────────▼─────────┐
                    │   Kafka Cluster    │
                    │ (buffering, back-  │
                    │  pressure handling)│
                    └─────────┬─────────┘
                              │
                    ┌─────────▼─────────┐
                    │ Logstash / Vector  │
                    │ (parsing, enrich,  │
                    │  routing)          │
                    └─────────┬─────────┘
                              │
┌─────────────────────────────▼───────────────────────────────────────┐
│                    ELASTICSEARCH CLUSTER                              │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ Master-Eligible Nodes (3)                                    │   │
│  │ Cluster state, shard allocation, index management            │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ Coordinating Nodes (5)                                       │   │
│  │ Query routing, scatter-gather, result merging                 │   │
│  └─────────────────────────┬────────────────────────────────────┘   │
│                             │                                        │
│  ┌──────────┬───────────────┼───────────────┬──────────┐            │
│  │          │               │               │          │            │
│  │  ┌───────▼───────┐  ┌───▼───────────┐  ┌▼────────────────┐     │
│  │  │ HOT Data Nodes│  │ WARM Data     │  │ COLD Data       │     │
│  │  │ (22 × NVMe)   │  │ Nodes (9×SSD) │  │ Nodes (22×HDD)  │     │
│  │  │               │  │               │  │                  │     │
│  │  │ Active write  │  │ Read-only,    │  │ Read-only,       │     │
│  │  │ + recent      │  │ force-merged  │  │ compressed,      │     │
│  │  │ search        │  │               │  │ searchable       │     │
│  │  │               │  │               │  │ snapshots opt.   │     │
│  │  └───────────────┘  └───────────────┘  └──────────────────┘     │
│  │                                                                   │
│  │  ┌──────────────────────────────────────────────────────────┐    │
│  │  │ Ingest Nodes (optional, or colocated with coordinating)  │    │
│  │  │ Ingest pipelines: grok, date, GeoIP, user-agent          │    │
│  │  └──────────────────────────────────────────────────────────┘    │
│  │                                                                   │
│  └──────────────────────────────────────────────────────────────────┘│
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ ILM (Index Lifecycle Management)                              │   │
│  │ hot(30d) → warm(+60d) → cold(+275d) → delete(365d)          │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ Snapshot Repository (S3-compatible)                           │   │
│  │ Daily snapshots for disaster recovery                         │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
                              │
                    ┌─────────▼─────────┐
                    │   Kibana / Grafana │
                    │   (visualization)  │
                    └───────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Kafka** | Buffering layer between producers and ES; absorbs burst traffic; enables replay on ES downtime |
| **Logstash/Vector** | Log parsing (grok patterns for unstructured logs), field extraction, enrichment (GeoIP, DNS), routing to correct index |
| **Master-Eligible Nodes** | Manage cluster state: shard allocation, index creation/deletion, mapping updates. Dedicated to avoid resource contention with data operations. |
| **Coordinating Nodes** | Receive client requests; route to appropriate data nodes; scatter-gather for search; merge and sort results |
| **Hot Data Nodes** | NVMe storage; handle active indexing + recent queries; highest CPU/memory for indexing throughput |
| **Warm Data Nodes** | SSD storage; read-only indices (force-merged to 1 segment per shard); optimized for search, no indexing |
| **Cold Data Nodes** | HDD storage; compressed indices; infrequent queries; acceptable higher latency |
| **Ingest Nodes** | Run ingest pipelines (processors); can be colocated with coordinating nodes for simplicity |
| **ILM** | Automated index lifecycle: rollover (by size/age/doc count), migrate between tiers, force-merge, shrink, delete |
| **Snapshot Repository** | S3 bucket for cluster snapshots; daily incremental snapshots; used for DR and cold-tier restore |

### Data Flows

**Indexing:**
1. Application → Kafka topic (e.g., `logs.java-app`)
2. Logstash/Vector consumes from Kafka, parses and enriches
3. Logstash uses Elasticsearch bulk API to index documents
4. Coordinating node routes each document to the correct shard (hash of `_id` or routing key)
5. Primary shard indexes the document (inverted index + doc values)
6. Primary replicates to replica shard(s)
7. Both ACK → bulk response returned

**Search:**
1. Client sends query to coordinating node
2. Coordinating node identifies target indices (time-based pattern: `logs-2024.01.*`)
3. Scatter: forward query to all relevant primary/replica shards
4. Each shard executes query locally (inverted index lookup, scoring)
5. Each shard returns top-N results (IDs + scores)
6. Gather: coordinating node merges results, sorts by score
7. Coordinating node fetches full documents from shards (fetch phase)
8. Returns results to client

---

## 4. Data Model

### Core Entities & Schema

```json
// Index Template: logs-*
{
  "index_patterns": ["logs-*"],
  "template": {
    "settings": {
      "number_of_shards": 10,
      "number_of_replicas": 1,
      "index.lifecycle.name": "logs-ilm-policy",
      "index.lifecycle.rollover_alias": "logs-write",
      "index.routing.allocation.require.data": "hot",
      "index.codec": "best_compression",
      "index.refresh_interval": "5s",
      "index.translog.durability": "async",
      "index.translog.sync_interval": "5s"
    },
    "mappings": {
      "dynamic": "strict",
      "properties": {
        "@timestamp": { "type": "date" },
        "message": { "type": "text", "analyzer": "standard" },
        "level": { "type": "keyword" },
        "logger": { "type": "keyword" },
        "service": { "type": "keyword" },
        "instance": { "type": "keyword" },
        "trace_id": { "type": "keyword" },
        "span_id": { "type": "keyword" },
        "thread": { "type": "keyword" },
        "kubernetes": {
          "properties": {
            "namespace": { "type": "keyword" },
            "pod": { "type": "keyword" },
            "container": { "type": "keyword" },
            "node": { "type": "keyword" }
          }
        },
        "http": {
          "properties": {
            "method": { "type": "keyword" },
            "status_code": { "type": "short" },
            "url": { "type": "keyword", "ignore_above": 2048 },
            "duration_ms": { "type": "float" }
          }
        },
        "exception": {
          "properties": {
            "class": { "type": "keyword" },
            "message": { "type": "text" },
            "stacktrace": { "type": "text", "index": false }
          }
        },
        "tags": { "type": "keyword" },
        "tenant_id": { "type": "keyword" }
      }
    }
  }
}
```

```json
// ILM Policy
{
  "policy": {
    "phases": {
      "hot": {
        "min_age": "0ms",
        "actions": {
          "rollover": {
            "max_primary_shard_size": "40gb",
            "max_age": "1d"
          },
          "set_priority": { "priority": 100 }
        }
      },
      "warm": {
        "min_age": "30d",
        "actions": {
          "allocate": {
            "require": { "data": "warm" }
          },
          "forcemerge": { "max_num_segments": 1 },
          "shrink": { "number_of_shards": 2 },
          "set_priority": { "priority": 50 }
        }
      },
      "cold": {
        "min_age": "90d",
        "actions": {
          "allocate": {
            "require": { "data": "cold" }
          },
          "set_priority": { "priority": 0 },
          "readonly": {}
        }
      },
      "delete": {
        "min_age": "365d",
        "actions": {
          "delete": {}
        }
      }
    }
  }
}
```

### Database/Storage Selection

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| Document storage | Elasticsearch (Lucene) | Full-text search + structured queries + aggregations in one system |
| Ingest buffer | Kafka | Decouples producers from ES; handles backpressure; replay on failure |
| Log parsing | Logstash/Vector | Mature grok patterns; Vector for lower resource usage |
| Snapshots | S3-compatible object storage | Cheap, durable, incremental snapshots |
| Metadata | ES cluster state (stored on master nodes) | Mappings, settings, aliases managed by ES itself |

### Indexing Strategy

**Field type decisions:**

| Field | Type | doc_values | index | Rationale |
|-------|------|-----------|-------|-----------|
| `@timestamp` | date | yes | yes | Range queries + date_histogram aggregation |
| `message` | text | no | yes (inverted index) | Full-text search; doc_values disabled (too large) |
| `level` | keyword | yes | yes | Filter (term query) + terms aggregation |
| `service` | keyword | yes | yes | Filter + aggregation; high-cardinality but bounded |
| `trace_id` | keyword | yes | yes | Exact match lookup; no scoring needed |
| `http.duration_ms` | float | yes | yes | Range queries + percentile aggregation |
| `stacktrace` | text | no | no (`index: false`) | Display only; not searchable (saves indexing cost) |

**Key vs text:**
- `keyword`: exact match, sorting, aggregations. Stored in doc_values (column-oriented). Used for structured data.
- `text`: analyzed (tokenized) for full-text search. Stored in inverted index. Used for unstructured text like log messages.
- A field can be both: `message` is `text` for search, with a `.keyword` sub-field for aggregation (but this doubles storage).

---

## 5. API Design

### Elasticsearch APIs

**Bulk Indexing (Java client):**
```java
@Service
public class ElasticsearchIndexingService {

    private final ElasticsearchClient esClient;
    private final BulkProcessor bulkProcessor;

    public ElasticsearchIndexingService(ElasticsearchClient esClient) {
        this.esClient = esClient;

        // BulkProcessor: batches individual index requests
        this.bulkProcessor = BulkProcessor.builder(
                (request, listener) -> esClient.bulk(request, listener),
                new BulkProcessor.Listener() {
                    @Override
                    public void beforeBulk(long executionId, BulkRequest request) {
                        log.debug("Executing bulk #{} with {} actions",
                            executionId, request.numberOfActions());
                    }
                    @Override
                    public void afterBulk(long executionId, BulkRequest request,
                                         BulkResponse response) {
                        if (response.hasFailures()) {
                            log.error("Bulk #{} failures: {}",
                                executionId, response.buildFailureMessage());
                        }
                    }
                    @Override
                    public void afterBulk(long executionId, BulkRequest request,
                                         Throwable failure) {
                        log.error("Bulk #{} failed", executionId, failure);
                    }
                })
            .setBulkActions(1000)              // Flush every 1000 docs
            .setBulkSize(new ByteSizeValue(5, ByteSizeUnit.MB))  // or every 5 MB
            .setFlushInterval(TimeValue.timeValueSeconds(5))     // or every 5s
            .setConcurrentRequests(2)           // 2 concurrent bulk requests
            .setBackoffPolicy(BackoffPolicy.exponentialBackoff(
                TimeValue.timeValueMillis(100), 3))
            .build();
    }

    public void indexDocument(String index, String id, Map<String, Object> doc) {
        bulkProcessor.add(new IndexRequest(index)
            .id(id)
            .source(doc, XContentType.JSON)
            .routing(doc.get("tenant_id").toString()));  // Custom routing
    }
}
```

**Search API:**
```java
@Service
public class ElasticsearchSearchService {

    private final ElasticsearchClient esClient;

    /**
     * Search logs with filters and full-text query.
     * Uses filter context for structured fields (cached, no scoring)
     * and query context for message field (scored).
     */
    public SearchResponse searchLogs(LogSearchRequest req) {
        BoolQuery.Builder boolQuery = new BoolQuery.Builder();

        // Filter context (cached, no scoring)
        if (req.getLevel() != null) {
            boolQuery.filter(f -> f.term(t ->
                t.field("level").value(req.getLevel())));
        }
        if (req.getService() != null) {
            boolQuery.filter(f -> f.term(t ->
                t.field("service").value(req.getService())));
        }
        if (req.getNamespace() != null) {
            boolQuery.filter(f -> f.term(t ->
                t.field("kubernetes.namespace").value(req.getNamespace())));
        }

        // Time range filter (always present for log queries)
        boolQuery.filter(f -> f.range(r ->
            r.field("@timestamp")
             .gte(JsonData.of(req.getStartTime()))
             .lte(JsonData.of(req.getEndTime()))));

        // Query context (scored, for full-text search)
        if (req.getQuery() != null) {
            boolQuery.must(m -> m.match(mt ->
                mt.field("message").query(req.getQuery())));
        }

        // Build search request
        String indexPattern = String.format("logs-%s*",
            req.getStartTime().substring(0, 7));  // e.g., "logs-2024.01*"

        SearchResponse<Map> response = esClient.search(s -> s
            .index(indexPattern)
            .query(q -> q.bool(boolQuery.build()))
            .sort(so -> so.field(f ->
                f.field("@timestamp").order(SortOrder.Desc)))
            .from(req.getOffset())
            .size(req.getLimit())
            .highlight(h -> h.fields("message",
                hf -> hf.preTags("<em>").postTags("</em>")))
            .timeout("10s"),
            Map.class);

        return response;
    }

    /**
     * Aggregation: error rate by service over time.
     * Uses composite aggregation for pagination of high-cardinality terms.
     */
    public SearchResponse errorRateByService(String startTime, String endTime) {
        return esClient.search(s -> s
            .index("logs-*")
            .size(0)  // No documents, only aggregations
            .query(q -> q.bool(b -> b
                .filter(f -> f.range(r ->
                    r.field("@timestamp").gte(JsonData.of(startTime))
                     .lte(JsonData.of(endTime))))
                .filter(f -> f.term(t ->
                    t.field("level").value("ERROR")))))
            .aggregations("per_service", a -> a
                .terms(t -> t.field("service").size(100))
                .aggregations("over_time", sa -> sa
                    .dateHistogram(dh -> dh
                        .field("@timestamp")
                        .calendarInterval(CalendarInterval.Hour)))),
            Map.class);
    }
}
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Shard Sizing and Allocation Strategy

**Why it's hard:** Shard count is set at index creation and cannot be changed (without reindex or shrink). Too few shards → hot spots, slow indexing, shard size too large. Too many shards → high memory overhead per shard (heap for segment metadata), slow cluster state updates, expensive scatter-gather for queries.

**Approaches:**

| Approach | Pros | Cons | When to Use |
|----------|------|------|-------------|
| Fixed shard count per index | Simple | Over/under-sharded as data grows | Small, stable indices |
| Time-based indices + rollover | Shards bounded by size; old indices can be shrunk | More indices to manage | Log/event data (our case) |
| Custom routing | Avoids scatter-gather; data locality | Uneven shard sizes if routing key is skewed | Multi-tenant search |
| Data streams (ES 7.9+) | Abstraction over time-based indices; automatic rollover | Less manual control | Append-only time-series data |

**Selected approach:** Data streams with ILM-managed rollover + custom routing for tenant isolation.

**Justification:**
- Data streams provide automatic rollover when primary shard reaches 40 GB or 1 day age
- Custom routing by `tenant_id` ensures all documents for a tenant are on the same shard(s), enabling efficient single-tenant queries
- ILM handles lifecycle transitions automatically

**Implementation Detail:**

```python
class ShardSizingCalculator:
    """
    Shard sizing rules of thumb:
    1. Target 30-50 GB per shard (sweet spot for search + indexing)
    2. Max ~20 shards per GB of heap on data nodes
    3. Max ~1 shard per vCPU per node for indexing throughput
    4. Target < 1000 shards per node
    """

    def calculate_shard_count(self, daily_ingest_gb: float,
                               rollover_days: int,
                               target_shard_size_gb: float = 40,
                               replica_count: int = 1) -> dict:
        """Calculate optimal shard count for a time-based index."""

        # Total data per rollover period (before replication)
        data_per_rollover = daily_ingest_gb * rollover_days

        # Primary shards needed
        primary_shards = math.ceil(data_per_rollover / target_shard_size_gb)
        primary_shards = max(primary_shards, 1)

        # Total shards (primaries + replicas)
        total_shards = primary_shards * (1 + replica_count)

        return {
            'primary_shards': primary_shards,
            'replica_count': replica_count,
            'total_shards': total_shards,
            'estimated_shard_size_gb': data_per_rollover / primary_shards,
            'data_per_rollover_gb': data_per_rollover
        }

    def validate_cluster_capacity(self, nodes: list[dict],
                                   total_indices: int,
                                   shards_per_index: int) -> dict:
        """Validate that the cluster can handle the shard count."""

        total_shards = total_indices * shards_per_index
        total_heap_gb = sum(n['heap_gb'] for n in nodes if n['role'] == 'data')
        total_vcpus = sum(n['vcpus'] for n in nodes if n['role'] == 'data')
        num_data_nodes = sum(1 for n in nodes if n['role'] == 'data')

        warnings = []

        # Rule: max 20 shards per GB of heap
        max_shards_by_heap = total_heap_gb * 20
        if total_shards > max_shards_by_heap:
            warnings.append(
                f"Shard count {total_shards} exceeds heap limit "
                f"{max_shards_by_heap} (20/GB × {total_heap_gb} GB)")

        # Rule: max 1000 shards per node
        shards_per_node = total_shards / num_data_nodes
        if shards_per_node > 1000:
            warnings.append(
                f"Shards per node {shards_per_node:.0f} exceeds 1000 limit")

        # Rule: ~1 indexing shard per vCPU
        # (Only active/writing indices count)
        active_write_shards = shards_per_index  # Only latest index is writing
        write_shards_per_node = active_write_shards / num_data_nodes
        if write_shards_per_node > total_vcpus / num_data_nodes:
            warnings.append(
                f"Write shards per node {write_shards_per_node:.0f} "
                f"exceeds vCPU count {total_vcpus // num_data_nodes}")

        return {
            'total_shards': total_shards,
            'shards_per_node': shards_per_node,
            'max_shards_by_heap': max_shards_by_heap,
            'warnings': warnings,
            'healthy': len(warnings) == 0
        }


# Example calculation for our cluster:
calc = ShardSizingCalculator()

# Daily index with rollover at 40 GB shard size
result = calc.calculate_shard_count(
    daily_ingest_gb=2300,   # 2.3 TB/day
    rollover_days=1,
    target_shard_size_gb=40,
    replica_count=1
)
# Result: ~58 primary shards × 2 = 116 total shards per index
# But: with custom routing and multiple write aliases, we can reduce this

# Using rollover at 40GB primary shard size:
# Each day creates ~58 shards → 30 days hot = ~1740 primary shards on hot tier
# 22 hot nodes → ~79 primary shards per node → well within limits
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Shard too large (>50 GB) | Slow recovery (relocation takes hours), slow force-merge | ILM rollover at 40 GB; monitor shard sizes |
| Too many shards per node | Heap exhaustion; slow cluster state | Monitor `_cluster/health`; alert on shards per node > 800 |
| Uneven shard distribution | Hot nodes with more shards get overloaded | `cluster.routing.allocation.balance.shard` tuning; `total_shards_per_node` limit |
| Custom routing creating hot shards | One tenant generates 90% of traffic → one shard overloaded | Monitor per-shard indexing rate; split index by large tenants |
| Rollover failure | Index grows unbounded | ILM retry; alert on `ilm.step == ERROR` |
| Cluster state too large | Master node OOM; slow state distribution | Reduce total index count; use data streams; close old indices |

**Interviewer Q&As:**

**Q1: What determines the ideal shard size?**
A: 30-50 GB for the sweet spot. Smaller shards: more overhead (each shard has Lucene segments, file descriptors, heap for segment metadata). Larger shards: slower recovery (entire shard must be replicated on relocation), slower force-merge, bigger impact if corrupted. 40 GB is the commonly recommended target for log data.

**Q2: How do you handle a tenant that generates 10× more logs than others?**
A: (1) Separate index for the large tenant (dedicated shards). (2) Index-per-tenant approach with different shard counts based on volume. (3) If using shared index with custom routing, the tenant's routing value maps to a disproportionate shard — detect via monitoring and split the tenant to its own index.

**Q3: What happens when you shrink an index from 10 shards to 2?**
A: The shrink operation: (1) allocates all primary shards to a single node, (2) creates a new index with fewer shards using hard links (no data copy), (3) recovery replays the data into new shard structure. Requirement: target shard count must be a factor of source count (10→2 works because 10/2=5 docs per target shard). The original index becomes read-only during shrink.

**Q4: How does the `total_shards_per_node` setting help?**
A: `index.routing.allocation.total_shards_per_node` limits how many shards of a single index can be allocated to one node. For a 10-shard index on a 5-node cluster, setting this to 2 ensures even distribution (2 shards per node). Without it, the allocator might pile up shards on nodes with more free space.

**Q5: What's the heap overhead per shard?**
A: Each shard's Lucene segments consume heap for: ordinals (field value lookups), norms (scoring), file system cache bookkeeping. Rule of thumb: ~10 MB per shard (varies with segment count and field types). At 1000 shards per node, that's ~10 GB of heap just for shard metadata — significant on a 32 GB heap node.

**Q6: When should you use data streams vs traditional indices with ILM?**
A: Data streams (ES 7.9+) are best for append-only time-series data (logs, metrics, events). They provide: automatic backing index creation, simpler rollover (built-in), no need to manage write aliases. Traditional indices with ILM are better when you need random updates/deletes (data streams are append-only) or complex index naming.

---

### Deep Dive 2: Indexing Pipeline and Throughput Optimization

**Why it's hard:** Achieving 500K docs/sec requires careful tuning of the entire pipeline: Kafka consumer parallelism, Logstash pipeline workers, bulk request sizing, ES refresh interval, translog durability, and shard allocation. A bottleneck at any stage limits the whole pipeline.

**Approaches:**

| Component | Tuning Lever | Default | Optimized |
|-----------|-------------|---------|-----------|
| Logstash | Pipeline workers | 1 per CPU | Match Kafka partitions |
| Logstash | Batch size | 125 | 1000-5000 |
| ES bulk API | Batch size | N/A | 1000 docs or 5 MB |
| ES refresh_interval | 1s | 5-30s (for write-heavy) |
| ES translog | sync on every index | Async, sync every 5s |
| ES merge policy | tiered_merge | floor_segment 2 GB, max_merge_at_once 10 |
| ES replicas | 1 | 0 during bulk load, then set to 1 |
| Thread pool | search: max(1, cores×3/4) | Tune based on profiling |

**Selected approach:** Full pipeline optimization with Kafka → Logstash → ES bulk API.

**Implementation Detail:**

```java
/**
 * High-throughput Elasticsearch indexing service using the Java client.
 * Optimized for log ingestion at 500K docs/sec.
 */
@Service
public class HighThroughputIndexingService {

    private final ElasticsearchClient esClient;
    private final MeterRegistry meterRegistry;

    // Connection pooling via RestClient (backed by Apache HttpAsyncClient)
    @Bean
    public ElasticsearchClient elasticsearchClient() {
        // Use HikariCP-like connection pooling philosophy:
        // - Connection count ≈ number of data nodes × 2
        // - Keep-alive to avoid TCP handshake overhead
        RestClient restClient = RestClient.builder(
                new HttpHost("es-coordinating-1", 9200, "https"),
                new HttpHost("es-coordinating-2", 9200, "https"),
                new HttpHost("es-coordinating-3", 9200, "https"))
            .setHttpClientConfigCallback(httpClientBuilder ->
                httpClientBuilder
                    .setMaxConnTotal(100)           // Total connections
                    .setMaxConnPerRoute(30)          // Per coordinating node
                    .setKeepAliveStrategy((response, context) -> 60_000)
                    .setDefaultIOReactorConfig(IOReactorConfig.custom()
                        .setIoThreadCount(Runtime.getRuntime().availableProcessors())
                        .build()))
            .setRequestConfigCallback(requestConfigBuilder ->
                requestConfigBuilder
                    .setConnectTimeout(5_000)
                    .setSocketTimeout(60_000))       // Bulk requests can be slow
            .build();

        ElasticsearchTransport transport = new RestClientTransport(
            restClient, new JacksonJsonpMapper());
        return new ElasticsearchClient(transport);
    }

    /**
     * Kafka consumer → ES bulk indexing pipeline.
     * 
     * Architecture:
     * Kafka (100 partitions) → 20 consumer threads → 
     * Ring buffer (1000 docs) → 4 bulk sender threads → ES
     */
    public void startIngestionPipeline(KafkaConsumer<String, LogEvent> consumer) {
        ExecutorService bulkSenders = Executors.newFixedThreadPool(4);
        BlockingQueue<List<LogEvent>> batchQueue = new LinkedBlockingQueue<>(100);

        // Consumer thread: polls Kafka, batches, queues
        while (true) {
            ConsumerRecords<String, LogEvent> records =
                consumer.poll(Duration.ofMillis(100));

            List<LogEvent> batch = new ArrayList<>(1000);
            for (ConsumerRecord<String, LogEvent> record : records) {
                batch.add(record.value());
                if (batch.size() >= 1000) {
                    batchQueue.put(new ArrayList<>(batch));
                    batch.clear();
                }
            }
            if (!batch.isEmpty()) {
                batchQueue.put(batch);
            }
        }
    }

    /**
     * Bulk sender: takes batches from queue and sends to ES.
     */
    private void bulkSend(List<LogEvent> batch) {
        BulkRequest.Builder bulkRequest = new BulkRequest.Builder();

        for (LogEvent event : batch) {
            String index = "logs-" + event.getTimestamp()
                .format(DateTimeFormatter.ofPattern("yyyy.MM.dd"));

            bulkRequest.operations(op -> op
                .index(idx -> idx
                    .index(index)
                    .routing(event.getTenantId())  // Custom routing
                    .document(event.toMap())));
        }

        try {
            BulkResponse response = esClient.bulk(bulkRequest.build());

            // Handle partial failures
            if (response.errors()) {
                for (BulkResponseItem item : response.items()) {
                    if (item.error() != null) {
                        if (item.error().type().equals("version_conflict_engine_exception")) {
                            // Ignore: duplicate document
                            meterRegistry.counter("es.bulk.duplicate").increment();
                        } else if (item.error().type().equals("mapper_parsing_exception")) {
                            // Bad document: send to DLQ
                            sendToDLQ(batch.get(item.index()), item.error());
                            meterRegistry.counter("es.bulk.mapping_error").increment();
                        } else {
                            // Retry-able error
                            meterRegistry.counter("es.bulk.error",
                                "type", item.error().type()).increment();
                        }
                    }
                }
            }

            meterRegistry.counter("es.bulk.success").increment(
                response.items().size() - countErrors(response));
            meterRegistry.timer("es.bulk.duration").record(
                response.took(), TimeUnit.MILLISECONDS);

        } catch (ElasticsearchException e) {
            // Full bulk failure: retry entire batch
            meterRegistry.counter("es.bulk.failure").increment();
            retryWithBackoff(batch);
        }
    }
}
```

```
# Elasticsearch index settings for high-throughput indexing
PUT /logs-000001
{
  "settings": {
    "number_of_shards": 10,
    "number_of_replicas": 1,
    "refresh_interval": "5s",
    "translog.durability": "async",
    "translog.sync_interval": "5s",
    "translog.flush_threshold_size": "1gb",
    "merge.policy.floor_segment": "2gb",
    "merge.policy.max_merge_at_once": 10,
    "merge.policy.max_merged_segment": "5gb",
    "merge.scheduler.max_thread_count": 1,
    "indexing.memory.index_buffer_size": "512mb"
  }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| ES bulk rejection (429) | Backpressure; Kafka consumer lag grows | Exponential backoff in bulk sender; increase thread_pool.write.queue_size |
| Mapping conflict (text vs keyword) | Document rejected | Strict dynamic mapping; schema validation in Logstash |
| ES node failure during indexing | Replica promotes to primary; brief pause | Shard awareness; retry on `NoShardAvailableAction` |
| Kafka consumer group rebalance | Brief indexing pause during partition reassignment | Increase `max.poll.interval.ms`; reduce batch size for faster processing |
| Translog too large (async durability) | Data loss on crash (up to `sync_interval` data) | Acceptable for logs; mission-critical data uses `request` durability |
| Index mapping explosion | Too many fields → heap exhaustion | `index.mapping.total_fields.limit` (default 1000); strict mapping |

**Interviewer Q&As:**

**Q1: Why use async translog durability for log data?**
A: The translog is ES's write-ahead log. With `request` durability, every index request waits for translog fsync (safe but slow). With `async` durability, translog is fsynced every `sync_interval` (5s). If ES crashes, up to 5s of logs are lost. For log data, this is acceptable because: (1) logs can be re-collected from Kafka, (2) the throughput improvement is 2-5×.

**Q2: How does refresh_interval affect search visibility?**
A: `refresh_interval` controls how often ES creates a new Lucene segment from the in-memory buffer, making documents searchable. Default 1s gives near-real-time search but creates many small segments (expensive to merge). Setting 5s or 30s for write-heavy indices reduces segment creation overhead, improving indexing throughput by 10-30%. Trade-off: documents are searchable with 5-30s delay instead of 1s.

**Q3: What's the difference between query context and filter context?**
A: Query context: Lucene scores each document (how well does it match?); results sorted by relevance. Used for full-text search (`match`, `multi_match`). Filter context: binary yes/no (does it match?); no scoring; results are cached in a bitset for reuse. Used for structured filters (`term`, `range`, `exists`). Always use filter context for structured queries — it's faster and cached.

**Q4: How do you handle schema evolution (adding new fields)?**
A: (1) For new fields: update the index template; new indices will have the field. Existing indices don't have the field (queries return null). (2) For type changes (keyword → text): requires reindex. Create a new index with the new mapping, reindex data from old index, swap alias. (3) Explicit mapping (`dynamic: strict`) prevents accidental schema changes from log format changes.

**Q5: Why use dedicated coordinating nodes?**
A: Coordinating nodes handle the scatter-gather for search: they receive the query, fan it out to data nodes, collect results, merge and sort. This is CPU and memory intensive for large aggregations. Dedicated coordinating nodes prevent this work from competing with indexing on data nodes. They also provide a stable endpoint for clients (data nodes may be added/removed).

**Q6: How does the BulkProcessor handle backpressure?**
A: `setConcurrentRequests(2)` limits to 2 in-flight bulk requests. If ES is slow, the 3rd batch blocks until one completes. `setBackoffPolicy(exponentialBackoff(...))` retries on rejection (429). Upstream, the Kafka consumer's `poll()` loop naturally slows down because `bulkProcessor.add()` blocks when the internal queue is full. This propagates backpressure through the entire pipeline.

---

### Deep Dive 3: Search Performance and the Deep Pagination Problem

**Why it's hard:** Elasticsearch uses a scatter-gather model: a search across 10 shards requires each shard to return top-N results, and the coordinating node merges them. For `from=10000, size=10`, each shard must return 10,010 results. At deep pagination, this becomes prohibitively expensive in memory and latency.

**Approaches:**

| Approach | Max Offset | Memory | Latency | Stateless |
|----------|-----------|--------|---------|-----------|
| `from/size` | 10,000 (default `max_result_window`) | O(from + size) per shard | Increases linearly | Yes |
| `search_after` | Unlimited | O(size) per shard | Constant | Yes |
| Scroll API | Unlimited | O(total_results) on each shard | High initial cost | No (stateful) |
| PIT (Point in Time) + `search_after` | Unlimited | O(size) per shard | Constant | Semi-stateful |

**Selected approach:** `search_after` with PIT for consistent pagination; `from/size` for first 10K results.

**Implementation Detail:**

```java
/**
 * Deep pagination using search_after + Point-in-Time (PIT).
 * 
 * PIT creates a consistent snapshot of the index state,
 * so results are stable across pages even as new documents are indexed.
 * 
 * search_after uses the sort values of the last document on the
 * previous page as the starting point for the next page.
 * Each shard only needs to return `size` documents (not from+size).
 */
@Service
public class DeepPaginationService {

    private final ElasticsearchClient esClient;

    /**
     * Open a PIT (Point in Time) for consistent pagination.
     * PIT should be closed when pagination is complete.
     */
    public String openPIT(String indexPattern, Duration keepAlive) {
        OpenPointInTimeResponse response = esClient.openPointInTime(r -> r
            .index(indexPattern)
            .keepAlive(Time.of(t -> t.time(keepAlive.toMinutes() + "m"))));
        return response.id();
    }

    /**
     * Paginate through results using search_after.
     */
    public SearchResponse<Map> searchPage(String pitId,
                                           Query query,
                                           List<FieldValue> searchAfter,
                                           int pageSize) {
        SearchRequest.Builder builder = new SearchRequest.Builder()
            .pit(p -> p.id(pitId).keepAlive(Time.of(t -> t.time("5m"))))
            .query(query)
            .size(pageSize)
            .sort(s -> s.field(f -> f.field("@timestamp").order(SortOrder.Desc)))
            .sort(s -> s.field(f -> f.field("_id").order(SortOrder.Asc)));  // Tiebreaker

        if (searchAfter != null && !searchAfter.isEmpty()) {
            builder.searchAfter(searchAfter);
        }

        return esClient.search(builder.build(), Map.class);
    }

    /**
     * Complete pagination example with automatic page fetching.
     */
    public List<Map> fetchAllResults(String indexPattern, Query query,
                                      int maxResults) {
        String pitId = openPIT(indexPattern, Duration.ofMinutes(10));
        List<Map> allResults = new ArrayList<>();
        List<FieldValue> searchAfter = null;

        try {
            while (allResults.size() < maxResults) {
                SearchResponse<Map> page = searchPage(
                    pitId, query, searchAfter, 1000);

                List<Hit<Map>> hits = page.hits().hits();
                if (hits.isEmpty()) break;

                for (Hit<Map> hit : hits) {
                    allResults.add(hit.source());
                }

                // Use the sort values of the last hit for next page
                searchAfter = hits.get(hits.size() - 1).sort();
            }
        } finally {
            esClient.closePointInTime(c -> c.id(pitId));
        }

        return allResults;
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| PIT expired (keep_alive exceeded) | Next page request fails | Client must re-open PIT and restart from page 1; set adequate keep_alive |
| search_after with stale sort values | Inconsistent results (if no PIT) | Always use PIT with search_after for consistency |
| Coordinating node OOM on large result set | Node crash | Limit `max_result_window`; use search_after instead of from/size |
| Scroll context too large (many open scrolls) | Heap pressure on data nodes | Prefer PIT + search_after over Scroll; set `search.max_open_scroll_context` |

**Interviewer Q&As:**

**Q1: Why is `from/size` limited to 10,000 by default?**
A: With `from=10000, size=10` across 10 shards, each shard returns 10,010 documents to the coordinating node, which must hold all 100,100 documents in memory to sort and return the correct 10. At deep offsets, this memory usage grows linearly and can OOM the coordinating node.

**Q2: How does `search_after` solve this?**
A: `search_after` provides the sort values of the last document seen. Each shard uses these values as a lower bound, only scanning documents after that point. Each shard returns exactly `size` documents, regardless of how deep into the result set we are. Memory usage is O(size × num_shards), constant across pages.

**Q3: When would you still use the Scroll API?**
A: Scroll is for batch processing: export all matching documents (reindex, analytics). It creates a snapshot of the result set and returns a scroll_id. Each `scroll` call returns the next batch. Unlike search_after, Scroll guarantees you see every document exactly once, even if the index changes during scrolling. However, it holds resources on data nodes, so it's not suitable for user-facing pagination.

**Q4: How does PIT differ from Scroll?**
A: PIT creates a lightweight snapshot (just the segment list, no result state). It can be reused with different queries and sort orders. Scroll creates a heavy snapshot tied to a specific query and sort. PIT + search_after is the recommended replacement for Scroll in user-facing pagination. Scroll is still valid for one-time batch exports.

**Q5: How do you handle the "bouncing results" problem without PIT?**
A: Without PIT, if new documents are indexed between page requests, results can shift. A document on page 1 might move to page 2, causing it to be seen twice. Or a document could be missed. PIT prevents this by freezing the index state at the time of PIT creation.

**Q6: What's the performance impact of sorting on a high-cardinality field?**
A: Sorting uses doc_values (column-oriented, on-disk but memory-mapped). For keyword fields with millions of unique values, the sort is efficient because doc_values are stored in sorted order. For text fields, sorting requires a fielddata structure (loaded into heap) — avoid this. Always sort on keyword or numeric fields with doc_values enabled.

---

### Deep Dive 4: Index Lifecycle Management (ILM) and Tiered Architecture

**Why it's hard:** Log data follows a clear access pattern: hot (actively written, frequently queried) → warm (read-only, occasionally queried) → cold (rarely queried, must be cheap) → delete. Manually managing this lifecycle across hundreds of indices is error-prone. ILM automates transitions, but misconfiguration can cause data loss or storage bloat.

**Selected approach:** ILM with hot/warm/cold tiers, data stream abstraction.

**Implementation Detail:**

```json
// 1. Component template for ILM settings
PUT _component_template/logs-ilm-settings
{
  "template": {
    "settings": {
      "index.lifecycle.name": "logs-ilm-policy",
      "index.routing.allocation.include._tier_preference": "data_hot"
    }
  }
}

// 2. ILM Policy with all phases
PUT _ilm/policy/logs-ilm-policy
{
  "policy": {
    "phases": {
      "hot": {
        "min_age": "0ms",
        "actions": {
          "rollover": {
            "max_primary_shard_size": "40gb",
            "max_age": "1d",
            "max_docs": 500000000
          },
          "set_priority": { "priority": 100 },
          "forcemerge": {
            "max_num_segments": 5
          }
        }
      },
      "warm": {
        "min_age": "30d",
        "actions": {
          "allocate": {
            "number_of_replicas": 1
          },
          "shrink": {
            "number_of_shards": 2
          },
          "forcemerge": {
            "max_num_segments": 1
          },
          "set_priority": { "priority": 50 },
          "migrate": {
            "enabled": true
          }
        }
      },
      "cold": {
        "min_age": "90d",
        "actions": {
          "set_priority": { "priority": 0 },
          "allocate": {
            "number_of_replicas": 0
          },
          "migrate": {
            "enabled": true
          },
          "readonly": {}
        }
      },
      "frozen": {
        "min_age": "180d",
        "actions": {
          "searchable_snapshot": {
            "snapshot_repository": "logs-s3-repo"
          }
        }
      },
      "delete": {
        "min_age": "365d",
        "actions": {
          "delete": {}
        }
      }
    }
  }
}

// 3. Data stream template
PUT _index_template/logs-template
{
  "index_patterns": ["logs-*"],
  "data_stream": {},
  "composed_of": ["logs-ilm-settings", "logs-mappings"],
  "priority": 200
}
```

```python
class ILMMonitor:
    """Monitor ILM progression and detect stuck indices."""

    def __init__(self, es_client):
        self.es = es_client

    def check_ilm_health(self) -> list[dict]:
        """Find indices stuck in ILM transitions."""
        response = self.es.ilm.explain_lifecycle(index="logs-*")
        issues = []

        for index_name, info in response.items():
            if info.get('step') == 'ERROR':
                issues.append({
                    'index': index_name,
                    'phase': info['phase'],
                    'action': info['action'],
                    'step': 'ERROR',
                    'step_info': info.get('step_info', {}),
                    'severity': 'critical'
                })
            elif info.get('phase') == 'hot' and info.get('age', '') > '2d':
                # Hot phase should transition within 1 day; 2d means stuck
                issues.append({
                    'index': index_name,
                    'phase': 'hot',
                    'issue': 'stuck_in_hot',
                    'age': info['age'],
                    'severity': 'warning'
                })

        return issues

    def estimate_storage_savings(self) -> dict:
        """Calculate storage savings from ILM tiering."""
        indices = self.es.cat.indices(format='json', h=[
            'index', 'pri.store.size', 'store.size', 'status'])

        hot_size = sum(parse_size(i['pri.store.size'])
                      for i in indices if 'hot' in i.get('routing', ''))
        warm_size = sum(parse_size(i['pri.store.size'])
                       for i in indices if 'warm' in i.get('routing', ''))
        cold_size = sum(parse_size(i['pri.store.size'])
                       for i in indices if 'cold' in i.get('routing', ''))

        # Force-merge savings: typically 20-40% reduction
        # Shrink savings: proportional to shard reduction
        # Compression savings in cold: ~50%
        return {
            'hot_tier_gb': hot_size,
            'warm_tier_gb': warm_size,
            'warm_savings_from_forcemerge': warm_size * 0.3,
            'cold_tier_gb': cold_size,
            'cold_savings_from_compression': cold_size * 0.5,
            'total_savings_gb': warm_size * 0.3 + cold_size * 0.5
        }
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| ILM step ERROR (e.g., not enough space for shrink) | Index stuck in current tier | Monitor `_ilm/explain`; alert on ERROR step; retry or manually advance |
| Force-merge during peak hours | CPU/IO spike on hot nodes | Schedule via ILM min_age to trigger during off-peak; or separate force-merge schedule |
| Searchable snapshot mount failure | Frozen tier data inaccessible | Verify S3 connectivity; fall back to cold tier copy if available |
| Disk full on warm/cold tier | New allocations rejected | Monitor disk usage per tier; add nodes before 80% threshold |
| ILM policy deleted or modified | Indices orphaned without lifecycle | Protect ILM policies via Elasticsearch security roles; version control policies |
| Rollover fails (max shard count) | Write index grows indefinitely | Monitor rollover failures; increase max shard count or add nodes |

**Interviewer Q&As:**

**Q1: What is force-merge and why is it important?**
A: Force-merge reduces a shard's Lucene segments to a specified count (typically 1). Benefits: (1) fewer segments means fewer file descriptors and less heap, (2) faster search (fewer segments to scan), (3) reclaims space from deleted documents. Only do this on read-only indices (warm/cold) — force-merging a writable index wastes I/O as new segments are immediately created.

**Q2: How does searchable snapshot (frozen tier) work?**
A: The index data is stored in S3 (snapshot). ES mounts it as a "searchable snapshot" — data is loaded on-demand from S3 when queried. A local cache on the frozen node stores recently accessed segments. This allows near-infinite retention at S3 cost, with acceptable query latency for infrequent searches. Trade-off: first query to frozen data is slow (S3 latency), subsequent queries use cache.

**Q3: How do you handle the warm transition for a 40 GB shard?**
A: The shard is physically relocated from a hot node (NVMe) to a warm node (SSD). This involves copying the shard data over the network. For a 40 GB shard at 1 Gbps, this takes ~5 minutes. ES throttles recoveries (`indices.recovery.max_bytes_per_sec`, default 40 MB/s). Multiple shards can relocate in parallel up to `cluster.routing.allocation.node_concurrent_recoveries` (default 2).

**Q4: What's the impact of reducing replicas in the cold tier from 1 to 0?**
A: Cost savings (halve storage) at the expense of availability. If a cold node fails, shards on it are unavailable until the node recovers (no replica to promote). For log data older than 90 days, this is often acceptable. For critical data, keep 1 replica or use searchable snapshots (which can restore from S3).

**Q5: How do you test ILM policy changes before production?**
A: (1) Apply the new policy to a test index with a short `min_age` (e.g., 1 minute per phase). (2) Watch the index transition through all phases via `_ilm/explain`. (3) Verify shard sizes, segment counts, and node allocation at each phase. (4) Only then apply to production indices.

**Q6: How does ILM interact with data streams?**
A: Data streams automatically manage the rollover action. When ILM triggers rollover, a new backing index is created (the "write index"). The old index transitions to the warm phase. The data stream abstraction hides the index management from clients — they always write to `logs-*` and read from `logs-*`, which transparently spans all backing indices.

---

## 7. Scaling Strategy

**Scaling dimensions:**

| Dimension | Approach |
|-----------|---------|
| Indexing throughput | More hot data nodes; more primary shards; increase refresh_interval |
| Search throughput | More replicas; more coordinating nodes; caching |
| Storage | Add warm/cold nodes; use searchable snapshots for frozen tier |
| Query complexity | Pre-compute aggregations; use rollup indices; denormalize data |
| Cluster count | Cross-cluster search for multi-region; CCS for federated queries |

**Interviewer Q&As:**

**Q1: How do you scale ES for 10× more log volume?**
A: (1) Add hot data nodes (linear scaling). (2) Increase Kafka partitions for parallel ingestion. (3) Increase shard count per index to distribute writes. (4) Consider reducing replica count to 0 during initial indexing, then set to 1 after a delay. (5) Increase `refresh_interval` to 30s. (6) Use Vector instead of Logstash (lower overhead).

**Q2: How do you handle cross-region search?**
A: Cross-cluster search (CCS): configure remote clusters and query `cluster_name:index_pattern`. Queries are fanned out to remote clusters, which execute locally and return results. The coordinating node merges. For latency-sensitive queries, use cross-cluster replication (CCR) to replicate indices to the local region.

**Q3: What's the maximum recommended cluster size?**
A: Practical limit: ~200 data nodes per cluster. Beyond that, cluster state size and master node overhead become problematic. For larger deployments, split into multiple clusters (by tenant, use case, or region) and use CCS for cross-cluster queries.

**Q4: How do you handle a sudden 5× spike in log volume (incident)?**
A: (1) Kafka absorbs the burst (Kafka partitions buffer hours of data). (2) ES will exhibit increased indexing latency and bulk rejections. (3) Short-term: increase `refresh_interval` to 60s, reduce replicas temporarily. (4) Medium-term: add hot nodes via auto-scaling (if using cloud) or provisioned bare-metal spares. (5) Long-term: implement per-tenant rate limiting in Logstash.

**Q5: How do you optimize expensive aggregations?**
A: (1) Use `filter` context for all structured filters (cached in bitset). (2) Limit terms aggregation size. (3) Use `composite` aggregation for paginating through large term sets. (4) Pre-compute common aggregations into rollup indices (`_rollup` API or transforms). (5) Use `sampler` aggregation to limit the docs scanned.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO | RPO |
|---|---------|-----------|--------|----------|-----|-----|
| 1 | Single data node failure | Cluster health turns YELLOW | Replica shards promote to primary; 1 replica lost | ES auto-allocates new replicas to other nodes | 0 (reads continue) | 0 |
| 2 | Master node failure | Remaining masters elect new leader | Brief cluster state update pause | Automatic re-election (< 30s) | < 30s | 0 |
| 3 | Coordinating node failure | LB health check | Clients routed to other coordinating nodes | LB removes failed node | 0 | 0 |
| 4 | Full rack failure | Multiple node failures | Many shards lose replicas; some indices RED | Replica allocation to surviving nodes; may need snapshot restore | Minutes | 0 (if replicas on other racks) |
| 5 | Network partition | Split-brain detection | Minority side steps down (no quorum) | Partition heals; cluster reforms | Minutes | 0 (minority was read-only) |
| 6 | Disk full on data node | Watermarks: `cluster.routing.allocation.disk.watermark.flood_stage` | Node goes read-only | Add disk; delete old indices; ES removes read-only block automatically at low watermark | Minutes | 0 |
| 7 | Mapping explosion (too many fields) | Heap exhaustion on master nodes | Index creation fails; cluster state updates blocked | Set `index.mapping.total_fields.limit`; reindex with stricter mapping | Hours | 0 |
| 8 | Cluster-wide outage | All monitoring | Complete outage | Restore from S3 snapshots + Kafka replay | Hours | Minutes (Kafka retention) |

### Data Durability and Replication

- **Primary + Replica:** Each shard has 1 replica on a different node (different rack via `cluster.routing.allocation.awareness.attributes`).
- **Translog:** Each write is recorded in the translog. With `request` durability, translog is fsynced before ACK. With `async` durability, up to `sync_interval` data can be lost on crash.
- **Snapshots:** Daily incremental snapshots to S3. Retention: 30 days. Restore time: dependent on data size and S3 throughput.
- **Kafka replay:** Kafka retains logs for 7 days. On ES data loss, replay from Kafka to re-index.

---

## 9. Security

| Layer | Mechanism |
|-------|-----------|
| **Authentication** | Native realm (username/password), LDAP/AD integration, SAML/OIDC for Kibana |
| **Authorization** | Role-based access control (RBAC); index-level and field-level security; document-level security (DLS) for multi-tenancy |
| **Encryption at rest** | dm-crypt/LUKS on data node disks; S3 SSE for snapshots |
| **Encryption in transit** | TLS on all HTTP and transport ports (node-to-node) |
| **Network isolation** | ES cluster on private network; coordinating nodes exposed via LB; no direct access to data/master nodes |
| **Audit logging** | Security audit log (all authentication and authorization events) |
| **Multi-tenancy isolation** | Document-level security: `{ "match": { "tenant_id": "{{_user.metadata.tenant}}" } }` filters all queries to tenant's documents |
| **API key management** | API keys for service accounts (Java/Python services); rotation policies |

---

## 10. Incremental Rollout Strategy

| Phase | Scope | Duration | Validation |
|-------|-------|----------|-----------|
| 1 | 3-node cluster (master + 2 data); test index template + ILM | 1 week | Indexing throughput, ILM transitions, basic search |
| 2 | Kafka + Logstash pipeline; 10% of log sources | 2 weeks | End-to-end latency; parser correctness; bulk rejection rate |
| 3 | Hot/warm tiering (add warm nodes); 50% of log sources | 2 weeks | ILM warm transition; force-merge validation; search on warm tier |
| 4 | Full production (100% sources); add coordinating nodes | 2 weeks | Peak load testing; dashboard performance; alerting rules |
| 5 | Cold tier + searchable snapshots | 2 weeks | Cold query latency; snapshot/restore testing |
| 6 | Cross-cluster search (multi-region) | 3 weeks | CCS latency; failover testing |

**Rollout Q&As:**

**Q1: How do you migrate from an existing logging system (e.g., Splunk) to Elasticsearch?**
A: (1) Dual-write: send logs to both systems via Kafka (Kafka consumer for each system). (2) Backfill historical data using Logstash's file input (if logs are on disk) or Splunk-to-ES migration tool. (3) Validate: compare search results between systems for the same queries. (4) Gradual traffic shift: redirect dashboards one by one. (5) Decommission Splunk after 30-day parallel period.

**Q2: How do you validate mapping correctness before production?**
A: (1) Index a sample of production logs (1 hour) into a test index. (2) Run production queries against the test index. (3) Check for mapping conflicts (`_mapping` API). (4) Verify field types match expected query patterns (keyword for term queries, text for full-text). (5) Check `_field_caps` API for field type consistency across indices.

**Q3: How do you handle the initial bulk load (backfilling 30 days of logs)?**
A: (1) Set `refresh_interval: -1` (disable refresh during bulk load). (2) Set `number_of_replicas: 0`. (3) Bulk index from Kafka (replay from earliest offset). (4) After complete, set `refresh_interval: 5s` and `number_of_replicas: 1`. (5) Monitor recovery (replica allocation) progress.

**Q4: How do you test ILM-managed index rollover?**
A: Create a test index with a very small rollover condition (`max_docs: 100, max_age: 1m`). Insert 200 documents. Verify that a new index is created and the write alias points to it. Verify the old index transitions to the warm phase. This tests the full ILM rollover chain in minutes instead of days.

**Q5: What's your rollback strategy if ES performance is worse than the existing system?**
A: Kafka is the safety net. If ES underperforms, stop the ES consumer group. Logs continue flowing to Kafka (retained for 7 days). Fix the ES issue (tuning, scaling, mapping). Resume the consumer from the lag point — Kafka consumer offsets enable exact-once replay. Dashboards can temporarily point back to the old system.

---

## 11. Trade-offs & Decision Log

| # | Decision | Alternatives | Rationale |
|---|----------|-------------|-----------|
| 1 | Elasticsearch over Loki/ClickHouse for logs | Grafana Loki (cheaper, less featureful), ClickHouse (columnar, fast analytics) | ES provides full-text search + structured queries + aggregations; team expertise; Kibana ecosystem |
| 2 | Kafka as ingest buffer over direct log shipping | Filebeat → ES direct, Fluentd → ES | Kafka provides backpressure handling, replay capability, multi-consumer (ES + backup + analytics) |
| 3 | Strict dynamic mapping over dynamic: true | `dynamic: true` auto-detects types | Strict prevents mapping explosion from malformed logs; explicit types avoid text/keyword conflicts |
| 4 | Async translog for log data over request durability | `request` (safe, slower) | Logs are reproducible from Kafka; 5s data loss is acceptable; 2-5× throughput improvement |
| 5 | Custom routing by tenant_id over default routing | Default (hash of _id), routing by service | Tenant-scoped queries avoid scatter-gather across all shards; single-shard queries for tenant dashboards |
| 6 | Dedicated coordinating nodes over co-located | Coordinating role on data nodes | Isolation: heavy aggregation queries don't impact indexing; stable client endpoints |
| 7 | 40 GB shard target over 20 GB or 80 GB | 20 GB (more shards, more overhead), 80 GB (slower recovery) | 40 GB balances recovery time (~5 min at network speed), search performance, and heap overhead |
| 8 | Searchable snapshots (frozen) over deleting old data | Delete after 90 days, keep on cold tier | Searchable snapshots cost ~$0.01/GB/month (S3) vs $0.10/GB/month (HDD); enables 1-year retention cheaply |

---

## 12. Agentic AI Integration

| Use Case | Agentic AI Application | Implementation |
|----------|----------------------|----------------|
| **Intelligent query optimization** | Agent analyzes slow queries and suggests index optimizations | Parse slow query log; identify missing keyword fields, suboptimal aggregations, missing filters; suggest mapping changes or recording rules |
| **Automated ILM tuning** | Agent adjusts ILM thresholds based on actual access patterns | Analyze `index.search.query_total` per index age; move rarely-queried indices to cold faster; keep frequently-queried indices on hot longer |
| **Anomaly detection in logs** | Agent identifies unusual log patterns without predefined rules | Rare terms aggregation on `message` field; cluster log messages using embeddings; alert when a new cluster appears or an existing one spikes |
| **Capacity planning** | Agent forecasts storage needs and recommends cluster scaling | Time-series analysis on index sizes, shard counts, node utilization; project 30-day capacity; auto-generate Terraform for node additions |
| **Mapping drift detection** | Agent detects when log formats change and mapping needs updating | Monitor `_mapping` changes and mapping conflicts; alert when new fields appear or types conflict; propose mapping updates |
| **Automated incident investigation** | Agent searches logs for root cause when an alert fires | Given an alert, agent queries ES for correlated errors across services using trace_id; builds a timeline; surfaces probable root cause |

**Example: Automated Query Optimization Agent**

```python
class ESQueryOptimizerAgent:
    """
    Analyzes slow queries and suggests optimizations.
    Runs as a scheduled job, processes the slow query log.
    """

    def __init__(self, es_client, llm_client, alert_client):
        self.es = es_client
        self.llm = llm_client
        self.alerts = alert_client

    async def analyze_slow_queries(self):
        """Run daily: analyze yesterday's slow queries."""
        slow_queries = await self.es.search(
            index=".ds-logs-es-slowlog*",
            query={
                "bool": {
                    "filter": [
                        {"range": {"@timestamp": {"gte": "now-1d"}}},
                        {"range": {"took_millis": {"gte": 1000}}}
                    ]
                }
            },
            sort=[{"took_millis": "desc"}],
            size=100
        )

        optimizations = []
        for hit in slow_queries['hits']['hits']:
            query = hit['_source']['query']
            took_ms = hit['_source']['took_millis']
            index = hit['_source']['index']

            analysis = self.analyze_query(query, index)
            if analysis['suggestions']:
                optimizations.append({
                    'query': query,
                    'took_ms': took_ms,
                    'index': index,
                    'suggestions': analysis['suggestions'],
                    'estimated_improvement': analysis['improvement']
                })

        if optimizations:
            await self.alerts.info(
                f"Found {len(optimizations)} query optimization opportunities. "
                f"Top suggestion: {optimizations[0]['suggestions'][0]}")

        return optimizations

    def analyze_query(self, query: dict, index: str) -> dict:
        suggestions = []
        improvement = 0

        # Check 1: Full-text query without filter context
        if 'match' in str(query) and 'filter' not in str(query):
            suggestions.append(
                "Add filter context for structured fields "
                "(term, range) — filters are cached and skip scoring")
            improvement += 30

        # Check 2: Wildcard leading with *
        if '"wildcard"' in str(query) and '"*' in str(query):
            suggestions.append(
                "Leading wildcard queries scan entire inverted index. "
                "Consider n-gram tokenizer or keyword field with "
                "case-insensitive normalizer")
            improvement += 50

        # Check 3: Aggregation on text field
        if '"aggs"' in str(query) or '"aggregations"' in str(query):
            mapping = self.es.indices.get_mapping(index=index)
            for field in self.extract_agg_fields(query):
                field_type = self.get_field_type(mapping, field)
                if field_type == 'text':
                    suggestions.append(
                        f"Aggregation on text field '{field}' uses "
                        f"fielddata (heap-intensive). Use .keyword sub-field")
                    improvement += 40

        # Check 4: No time filter on time-based index
        if 'range' not in str(query) or '@timestamp' not in str(query):
            suggestions.append(
                "No @timestamp range filter — query scans all time. "
                "Add time bounds to limit shard/segment scanning")
            improvement += 60

        return {'suggestions': suggestions, 'improvement': improvement}
```

---

## 13. Complete Interviewer Q&A Bank

**Architecture:**

**Q1: Explain the difference between master, data, coordinating, and ingest nodes.**
A: Master-eligible nodes manage cluster state (shard allocation, index metadata). Only one active master; others are standby. Data nodes store shards and execute queries/indexing locally. Coordinating nodes route requests and merge results (scatter-gather). Ingest nodes run preprocessing pipelines (grok, date parsing). A node can have multiple roles, but dedicated roles are recommended for production to avoid resource contention.

**Q2: How does shard allocation awareness work for rack fault tolerance?**
A: Configure `cluster.routing.allocation.awareness.attributes: rack_id`. ES ensures primary and replica shards are on different racks. If rack A fails, all replicas are on racks B/C, so no data is lost. You can also use `forced_awareness` to prevent all replicas from being on one rack if a rack is temporarily unavailable.

**Q3: How does Elasticsearch achieve near-real-time search?**
A: The `refresh` operation creates a new Lucene segment from the in-memory buffer, making new documents searchable. Default refresh interval is 1 second. Documents in the in-memory buffer (not yet refreshed) are not searchable. The translog provides durability — even if the in-memory buffer is lost, the translog can replay uncommitted documents.

**Q4: What is the Lucene segment merge process?**
A: Lucene creates a new segment on each refresh (every 1s default). Small segments accumulate. The TieredMergePolicy periodically merges small segments into larger ones. This is important because: (1) fewer segments = faster search (fewer files to scan), (2) deleted documents are physically removed during merge (reclaiming space). Force-merge manually triggers this for read-only indices.

**Indexing & Performance:**

**Q5: How do you troubleshoot high indexing latency?**
A: (1) Check `_nodes/stats/thread_pool` for write queue rejections. (2) Check `_cat/pending_tasks` for cluster state backlog. (3) Check GC logs (long GC pauses stall indexing). (4) Check disk I/O (`iostat` on data nodes). (5) Check merge activity (`_cat/thread_pool/force_merge`). (6) Check translog size (large translog → slow fsync). (7) Check mapping complexity (many fields → slow indexing).

**Q6: What is doc_values and when should you disable it?**
A: Doc_values is a columnar data structure stored on disk alongside the inverted index. Used for: sorting, aggregations, scripted fields. Enabled by default for all field types except `text`. Disable it for fields that are only used for full-text search (never aggregated/sorted) to save disk space. Example: `"stacktrace": { "type": "text", "doc_values": false, "index": false }`.

**Q7: How does custom routing affect indexing and search?**
A: Indexing: `_routing` determines which shard stores the document. `hash(_routing) % num_shards = shard_id`. Search: if you specify `routing` in the search request, ES only queries that shard (not all shards). This is powerful for multi-tenant systems — tenant queries touch only 1 shard instead of all. Risk: if one tenant has much more data, that shard becomes disproportionately large.

**Q8: What is the difference between `_id` and `_routing`?**
A: `_id` uniquely identifies a document within an index. `_routing` determines which shard stores the document. By default, `_routing = _id`. With custom routing (e.g., `_routing = tenant_id`), multiple documents with different `_id` values can be routed to the same shard. This enables single-shard queries for tenant-scoped lookups.

**Search & Aggregations:**

**Q9: How do you handle high-cardinality terms aggregations?**
A: (1) Use `composite` aggregation for paginating through all terms (instead of `terms` with a huge `size`). (2) Use `sampler` aggregation to limit the doc set before aggregating. (3) Pre-compute the aggregation into a transform (materialize the result as a new index). (4) Use approximate aggregations (`cardinality` uses HyperLogLog, not exact count).

**Q10: What's the performance difference between match and term queries?**
A: `term` query: exact match on keyword field; looks up the exact value in the inverted index; O(1). `match` query: analyzes the search text (tokenization, lowercasing), then looks up each token in the inverted index; O(tokens). `match` is for full-text search on `text` fields. `term` is for exact match on `keyword`/`numeric`/`date` fields. Using `term` on a `text` field will fail because the stored tokens are analyzed (lowercased, stemmed).

**Q11: How does the nested aggregation work and why is it expensive?**
A: Nested fields are stored as separate hidden documents (not flattened). A `nested` aggregation must join the parent and nested documents, which requires reading both document sets and computing the intersection. For documents with many nested entries (e.g., 100 items per order), this multiplies the work by 100×. Mitigate: denormalize if possible; limit nested array size.

**Operations:**

**Q12: How do you perform a zero-downtime reindex operation?**
A: (1) Create a new index with the updated mapping. (2) Start reindexing from old to new: `POST _reindex { "source": {"index": "old"}, "dest": {"index": "new"} }`. (3) During reindex, new writes go to both old and new indices (dual-write via Kafka consumer). (4) After reindex completes, switch the alias from old to new atomically: `POST _aliases { "actions": [{"remove": {"index": "old", "alias": "my-alias"}}, {"add": {"index": "new", "alias": "my-alias"}}]}`. (5) Delete old index after verification.

**Q13: How do you handle Elasticsearch rolling upgrades?**
A: (1) Disable shard allocation: `PUT _cluster/settings {"transient": {"cluster.routing.allocation.enable": "primaries"}}`. (2) Stop ES on one node. (3) Upgrade the binary. (4) Start ES. (5) Re-enable allocation. (6) Wait for GREEN. (7) Repeat for each node. Master-eligible nodes are upgraded last. ES supports rolling upgrades within a major version (8.x → 8.y).

**Q14: How do you monitor Elasticsearch cluster health?**
A: Key metrics: (1) Cluster health (GREEN/YELLOW/RED). (2) JVM heap usage (should stay < 75%). (3) GC frequency and duration. (4) Indexing rate and latency. (5) Search rate and latency. (6) Thread pool rejections (write, search). (7) Disk usage per node (watermarks). (8) Pending tasks (cluster state queue). (9) Shard count per node. Tools: _cat APIs, Elasticsearch Exporter for Prometheus, Kibana Stack Monitoring.

**Q15: What's the impact of JVM heap size on Elasticsearch?**
A: (1) Max 50% of RAM for heap (other 50% for OS file cache — critical for Lucene). (2) Keep heap < 32 GB to stay in compressed oops (object pointer compression). If heap > 32 GB, pointers expand from 4 bytes to 8 bytes, effectively wasting ~4 GB. Optimal: 30.5-31 GB heap on a 64 GB node. (3) G1GC is default since ES 7.x; works well up to 32 GB.

**Q16: How do you handle "circuit breaker" trips in Elasticsearch?**
A: Circuit breakers prevent OOM by estimating memory usage before executing operations. `parent` breaker: overall memory limit (default 95% of heap). `fielddata` breaker: limits fielddata loading. `request` breaker: limits per-request memory. When tripped: (1) The operation is rejected (429). (2) Check which breaker tripped via `_nodes/stats/breaker`. (3) Reduce query complexity, add filters to narrow result set, or increase heap (up to 32 GB limit).

---

## 14. References

| # | Resource | Relevance |
|---|----------|-----------|
| 1 | [Elasticsearch: The Definitive Guide](https://www.elastic.co/guide/en/elasticsearch/reference/current/index.html) | Official documentation |
| 2 | [Elasticsearch Shard Sizing Guide](https://www.elastic.co/guide/en/elasticsearch/reference/current/size-your-shards.html) | Shard sizing best practices |
| 3 | [ILM Documentation](https://www.elastic.co/guide/en/elasticsearch/reference/current/index-lifecycle-management.html) | Index lifecycle management |
| 4 | [Elasticsearch Java Client 8.x](https://www.elastic.co/guide/en/elasticsearch/client/java-api-client/current/index.html) | Java client API |
| 5 | [Lucene Internals](https://lucene.apache.org/core/) | Understanding inverted index, segments, merging |
| 6 | [Elastic Blog: Hot-Warm-Cold Architecture](https://www.elastic.co/blog/implementing-hot-warm-cold-in-elasticsearch-with-index-lifecycle-management) | Tiered architecture |
| 7 | [Data Streams](https://www.elastic.co/guide/en/elasticsearch/reference/current/data-streams.html) | Modern approach for time-series data |
| 8 | [Elasticsearch Circuit Breakers](https://www.elastic.co/guide/en/elasticsearch/reference/current/circuit-breaker.html) | Memory protection |
