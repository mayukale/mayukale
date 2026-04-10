# System Design: Distributed Tracing System

> **Relevance to role:** In a cloud infrastructure platform with hundreds of Java/Python microservices, Kubernetes clusters, message brokers, and databases, a single user request can traverse 10+ services. Distributed tracing provides end-to-end visibility into request flow, latency breakdown, and failure propagation. Deep understanding of OpenTelemetry, Jaeger architecture, sampling strategies, and trace context propagation is critical for debugging production issues and optimizing service performance.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **End-to-end request tracing** across all services: API gateway → job scheduler → resource manager → database, including async flows through Kafka.
2. **Trace context propagation** via W3C TraceContext standard (`traceparent` header) across HTTP, gRPC, and Kafka message headers.
3. **Span collection** with operation name, duration, tags (key-value metadata), logs (structured events within a span), and status (OK/ERROR).
4. **Trace search and visualization**: search by trace ID, service name, operation, duration range, tags. Visualize as timeline (Gantt chart) and dependency graph.
5. **Correlation with logs and metrics**: trace_id available in log entries and metric exemplars for cross-signal navigation.
6. **Service dependency map**: automatically generated from trace data showing service-to-service communication patterns.
7. **Latency analysis**: identify slow spans, bottleneck services, and latency distribution per operation.
8. **Sampling**: intelligent sampling to control costs while retaining interesting traces (errors, high latency).

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Span ingestion rate | 500,000 spans/sec |
| End-to-end latency (span generated to searchable) | < 60 seconds |
| Trace query latency (by trace ID) | < 2 seconds |
| Trace search latency (by service + time range) | < 5 seconds |
| Availability | 99.9% for ingestion, 99.5% for query |
| Retention | 7 days full traces, 30 days sampled |
| Sampling rate | 1-10% head-based, 100% for errors and high-latency |

### Constraints & Assumptions
- Fleet: 5,000 Java/Python services across 200 Kubernetes clusters.
- Average trace depth: 8 spans (services per request).
- Average span size: 1 KB (including tags and logs).
- Peak request rate: 500,000 requests/sec across all services → ~4M spans/sec before sampling.
- After 10% head-based sampling: 400K spans/sec. After tail-based error/latency capture: ~500K spans/sec total.
- Java services use Spring Boot; Python services use Flask/FastAPI.
- OpenTelemetry is the chosen instrumentation standard.

### Out of Scope
- Application Performance Monitoring (APM) features: code-level profiling, flame graphs, database query plans.
- Real User Monitoring (RUM) / browser-side tracing.
- Business transaction monitoring.
- Log collection (see distributed_logging_system.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Total services | 5,000 unique service deployments | 5,000 |
| Total service instances | 5,000 services x avg 10 pods each | 50,000 instances |
| Request rate (across all services) | 500,000 requests/sec (user-facing + internal) | 500K rps |
| Spans per request (avg trace depth) | 8 spans per trace | 8 |
| Total span generation rate | 500K x 8 | **4M spans/sec** |
| After head-based sampling (10%) | 4M x 0.1 | 400K spans/sec |
| Additional tail-based capture (errors, slow) | ~100K spans/sec (errors + high-latency traces) | 100K spans/sec |
| **Total ingestion rate** | 400K + 100K | **500K spans/sec** |
| Avg span size | Tags, logs, timing, context | ~1 KB |
| Ingestion bandwidth | 500K x 1 KB | **500 MB/s** |

### Latency Requirements

| Operation | Target |
|---|---|
| Span export from application to collector | < 5 seconds (batched) |
| Collector to Kafka | < 2 seconds |
| Kafka to storage (Elasticsearch/Cassandra) | < 30 seconds |
| End-to-end (span created to searchable) | **< 60 seconds** |
| Trace lookup by ID | < 2 seconds |
| Trace search (service + time range + tags) | < 5 seconds |

### Storage Estimates

| Storage | Calculation | Value |
|---|---|---|
| Daily span volume | 500K/s x 86,400s = 43.2B spans/day | 43.2B spans |
| Daily storage (raw) | 43.2B x 1 KB | **~43 TB/day** |
| With compression (3:1) | 43 TB / 3 | **~14.3 TB/day** |
| 7-day retention (full) | 14.3 TB x 7 x 2 (replica) | **~200 TB** |
| 30-day retention (sampled, 1%) | 14.3 TB x 0.01 x 30 | **~4.3 TB** |
| Kafka buffer (3 replicas, 24h) | 14.3 TB x 1 x 3 | **~43 TB** |

### Bandwidth Estimates

| Segment | Bandwidth |
|---|---|
| Applications → OTel Collector (OTLP/gRPC, compressed) | ~170 MB/s (500 MB/s / 3 compression) |
| OTel Collector → Kafka | ~170 MB/s |
| Kafka → Jaeger Ingester → Storage | ~500 MB/s (decompressed) |
| Storage intra-cluster replication | ~500 MB/s |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     INSTRUMENTED SERVICES                                   │
│                                                                             │
│  ┌────────────────────┐  ┌────────────────────┐  ┌──────────────────────┐  │
│  │ Java Service        │  │ Python Service      │  │ Go Service           │  │
│  │ (Spring Boot)       │  │ (FastAPI)           │  │ (net/http)           │  │
│  │                     │  │                     │  │                      │  │
│  │ OpenTelemetry       │  │ OpenTelemetry       │  │ OpenTelemetry        │  │
│  │ Java Agent          │  │ Python SDK          │  │ Go SDK               │  │
│  │ (auto-instrument)   │  │ (manual+auto)       │  │ (manual)             │  │
│  │                     │  │                     │  │                      │  │
│  │ W3C TraceContext    │  │ W3C TraceContext     │  │ W3C TraceContext     │  │
│  │ propagation         │  │ propagation          │  │ propagation          │  │
│  └─────────┬──────────┘  └─────────┬──────────┘  └──────────┬───────────┘  │
│            │ OTLP/gRPC              │ OTLP/gRPC              │ OTLP/gRPC   │
│            │ (batched, compressed)   │                        │              │
└────────────┼────────────────────────┼────────────────────────┼──────────────┘
             │                        │                        │
             ▼                        ▼                        ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│              OPENTELEMETRY COLLECTOR (Deployment or DaemonSet)               │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │  Receivers          │  Processors           │  Exporters             │   │
│  │  ┌────────────┐     │  ┌────────────────┐   │  ┌──────────────────┐ │   │
│  │  │ OTLP       │     │  │ Batch          │   │  │ Kafka Exporter   │ │   │
│  │  │ (gRPC:4317)│     │  │ (10s/8192 max) │   │  │ (topic: spans)   │ │   │
│  │  │            │     │  ├────────────────┤   │  ├──────────────────┤ │   │
│  │  │ Jaeger     │     │  │ Tail Sampling  │   │  │ Jaeger Exporter  │ │   │
│  │  │ (thrift)   │     │  │ Processor      │   │  │ (direct, for     │ │   │
│  │  │            │     │  │ (error/latency │   │  │  low-volume)     │ │   │
│  │  │ Zipkin     │     │  │  retention)    │   │  ├──────────────────┤ │   │
│  │  │ (compat)   │     │  ├────────────────┤   │  │ Prometheus       │ │   │
│  │  └────────────┘     │  │ Attributes     │   │  │ Exporter (span   │ │   │
│  │                     │  │ (enrich with   │   │  │  metrics: R.E.D) │ │   │
│  │                     │  │  cluster, env) │   │  └──────────────────┘ │   │
│  │                     │  ├────────────────┤   │                       │   │
│  │                     │  │ Span Metrics   │   │                       │   │
│  │                     │  │ Connector      │   │                       │   │
│  │                     │  │ (generate      │   │                       │   │
│  │                     │  │  request rate, │   │                       │   │
│  │                     │  │  latency histo │   │                       │   │
│  │                     │  │  from spans)   │   │                       │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          KAFKA BUFFER                                        │
│  Topic: traces.spans  │  Partitions: 256  │  RF: 3  │  Retention: 24h      │
│  Key: trace_id (ensures all spans of a trace go to same partition)          │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │
                    ┌──────────────┼──────────────────┐
                    │              │                   │
                    ▼              ▼                   ▼
┌──────────────────────┐ ┌─────────────────┐ ┌────────────────────────────┐
│ Jaeger Ingester      │ │ Tail Sampling   │ │ Span Analytics             │
│ (Kafka consumer →    │ │ Service         │ │ (Flink/Spark)              │
│  storage writer)     │ │ (buffers spans, │ │ - Service dependency graph │
│                      │ │  samples on     │ │ - Latency percentiles      │
│                      │ │  trace complete)│ │ - Error rate aggregation   │
└──────────┬───────────┘ └─────────────────┘ └────────────────────────────┘
           │
           ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                       TRACE STORAGE                                         │
│                                                                             │
│  Option A: Elasticsearch                    Option B: Cassandra             │
│  ┌────────────────────────────────┐   ┌──────────────────────────────────┐ │
│  │ Index: jaeger-span-YYYY-MM-DD │   │ Table: traces (partition key:    │ │
│  │ - Full-text search on tags    │   │   trace_id, clustering: span_id) │ │
│  │ - Service + operation index   │   │ - Excellent write throughput     │ │
│  │ - Duration range queries      │   │ - Lookup by trace_id is O(1)    │ │
│  │ - 7-day retention via ILM     │   │ - Weak search (requires         │ │
│  │                                │   │   secondary indexes or ES)      │ │
│  └────────────────────────────────┘   └──────────────────────────────────┘ │
│                                                                             │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                       JAEGER QUERY + UI                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │  - Search traces by: service, operation, tags, duration, time range │    │
│  │  - Trace timeline view (Gantt chart)                                │    │
│  │  - Service dependency graph (DAG)                                   │    │
│  │  - Trace comparison (diff two traces)                               │    │
│  │  - Deep link: /trace/{traceId}                                     │    │
│  │  - Links to Kibana (logs by trace_id) and Grafana (metrics)         │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **OpenTelemetry SDK/Agent** | Instrument application code. Auto-instrument HTTP/gRPC/JDBC/Kafka clients. Create spans, propagate context, export via OTLP. |
| **OpenTelemetry Collector** | Receive spans from SDKs, batch, process (sampling, enrichment), export to Kafka and/or directly to Jaeger. Acts as a buffer and processing layer. |
| **Kafka** | Decouples span producers from storage. Enables replay, multiple consumers. Key by trace_id for partition-level trace locality. |
| **Jaeger Ingester** | Kafka consumer that writes spans to storage (Elasticsearch or Cassandra). Horizontally scalable. |
| **Tail Sampling Service** | Buffers spans, waits for trace completion, then makes sampling decisions based on trace characteristics (error, latency). |
| **Trace Storage (ES/Cassandra)** | Persists spans. ES provides rich search; Cassandra provides high write throughput and efficient trace_id lookups. |
| **Jaeger Query + UI** | Query service reads from storage. UI provides trace search, timeline visualization, and dependency graphs. |
| **Span Metrics Connector** | Derives RED metrics (Rate, Error, Duration) from spans, exports to Prometheus. Eliminates need for separate metric instrumentation for tracing-derived metrics. |

### Data Flows

1. **Instrumentation**: Application SDK creates span → propagates `traceparent` header to downstream calls → exports batched spans via OTLP/gRPC to OTel Collector.
2. **Collection**: OTel Collector receives spans → applies processors (batch, sample, enrich) → exports to Kafka topic `traces.spans`.
3. **Storage**: Jaeger Ingester consumes from Kafka → writes to Elasticsearch/Cassandra.
4. **Query**: User → Jaeger UI → Jaeger Query → Elasticsearch/Cassandra → returns trace.
5. **Correlation**: User clicks trace_id link in log → opens Jaeger trace view. User clicks "View Logs" in Jaeger → opens Kibana with `trace_id:{id}` filter.

---

## 4. Data Model

### Core Entities & Schema

**Span (OpenTelemetry Model)**
```json
{
  "traceId": "4bf92f3577b34da6a3ce929d0e0e4736",
  "spanId": "00f067aa0ba902b7",
  "parentSpanId": "a3ce929d0e0e4736",
  "traceState": "",
  "name": "POST /api/v1/jobs",
  "kind": "SPAN_KIND_SERVER",
  "startTimeUnixNano": 1712678625123000000,
  "endTimeUnixNano": 1712678625456000000,
  "attributes": [
    {"key": "service.name", "value": {"stringValue": "job-scheduler"}},
    {"key": "service.version", "value": {"stringValue": "2.4.1"}},
    {"key": "http.method", "value": {"stringValue": "POST"}},
    {"key": "http.url", "value": {"stringValue": "/api/v1/jobs"}},
    {"key": "http.status_code", "value": {"intValue": 200}},
    {"key": "http.response_content_length", "value": {"intValue": 1234}},
    {"key": "k8s.namespace.name", "value": {"stringValue": "scheduling"}},
    {"key": "k8s.pod.name", "value": {"stringValue": "scheduler-7b4f9d-x2k4q"}},
    {"key": "k8s.cluster.name", "value": {"stringValue": "k8s-prod-us-east-1"}},
    {"key": "job.id", "value": {"stringValue": "j-98234"}},
    {"key": "job.priority", "value": {"stringValue": "HIGH"}}
  ],
  "events": [
    {
      "timeUnixNano": 1712678625200000000,
      "name": "Acquired resource lock",
      "attributes": [
        {"key": "lock.name", "value": {"stringValue": "gpu-pool-lock"}},
        {"key": "lock.wait_ms", "value": {"intValue": 45}}
      ]
    }
  ],
  "status": {
    "code": "STATUS_CODE_OK",
    "message": ""
  },
  "links": [
    {
      "traceId": "abc123...",
      "spanId": "def456...",
      "attributes": [{"key": "link.type", "value": {"stringValue": "follows_from"}}]
    }
  ]
}
```

**Trace (collection of spans):**
```
Trace: 4bf92f3577b34da6a3ce929d0e0e4736
├── [api-gateway] GET /submit-job (200ms)
│   ├── [auth-service] ValidateToken (15ms)
│   └── [job-scheduler] POST /api/v1/jobs (180ms)
│       ├── [job-scheduler] ValidateJobSpec (5ms)
│       ├── [resource-manager] AllocateResources (120ms)
│       │   ├── [resource-manager] CheckQuota (10ms)
│       │   └── [resource-manager] ReserveGPU (100ms)
│       │       └── [mysql] SELECT FROM resource_pool (8ms)
│       └── [kafka-producer] Produce to job-events (10ms)
```

**Elasticsearch Index Mapping (Jaeger)**
```json
{
  "jaeger-span-2026-04-09": {
    "mappings": {
      "properties": {
        "traceID":       { "type": "keyword" },
        "spanID":        { "type": "keyword" },
        "parentSpanID":  { "type": "keyword" },
        "operationName": { "type": "keyword" },
        "serviceName":   { "type": "keyword" },
        "startTime":     { "type": "long" },
        "startTimeMillis": { "type": "date" },
        "duration":      { "type": "long" },
        "tags":          { "type": "nested",
          "properties": {
            "key":   { "type": "keyword" },
            "value": { "type": "keyword" },
            "type":  { "type": "keyword" }
          }
        },
        "logs":          { "type": "nested" },
        "process": {
          "properties": {
            "serviceName": { "type": "keyword" },
            "tags":        { "type": "nested" }
          }
        },
        "references":    { "type": "nested" }
      }
    }
  }
}
```

### Database Selection

| Storage Backend | Pros | Cons | Best For |
|---|---|---|---|
| **Elasticsearch** | Rich search (tags, duration ranges, full-text), Kibana integration, existing ELK infrastructure | Higher write cost per span, index management overhead | Organizations already running ELK for logs; need advanced search |
| **Cassandra** | Excellent write throughput (~2x ES), efficient trace_id lookups, linear horizontal scaling | Weak search capabilities (secondary indexes are limited), no full-text search | Write-heavy workloads, trace-id-only lookups, massive scale |
| **ClickHouse** | Columnar compression (10x), fast aggregation queries, emerging Jaeger support | Less mature Jaeger integration, requires ClickHouse expertise | Analytics over traces (latency distributions, service maps) |

**Selected: Elasticsearch** (leveraging existing ELK cluster from logging system)

Rationale: We already operate large Elasticsearch clusters for logging. Sharing infrastructure reduces operational overhead. Elasticsearch's rich search is valuable for trace exploration (search by custom tags, duration ranges, error types). The logging and tracing teams can share ILM policies and cluster management.

### Indexing Strategy

| Strategy | Detail |
|---|---|
| **Index pattern** | `jaeger-span-YYYY-MM-DD`, `jaeger-service-YYYY-MM-DD` |
| **Shard count** | 10 primary shards per daily span index (target 30-50 GB per shard) |
| **Retention** | 7 days (ILM delete), with daily rollover |
| **Key fields indexed** | `traceID` (keyword), `serviceName` (keyword), `operationName` (keyword), `startTimeMillis` (date), `duration` (long), `tags.key+value` (nested keyword) |
| **Optimization** | Tags stored as nested documents for exact match queries; `_source` enabled for full span display |

---

## 5. API Design

### Query APIs

**Find Traces (Search)**
```
GET /api/traces?service=job-scheduler&operation=POST+/api/v1/jobs&minDuration=100ms&maxDuration=5s&tags={"job.priority":"HIGH"}&start=1712674800000000&end=1712678400000000&limit=20

Response:
{
  "data": [
    {
      "traceID": "4bf92f3577b34da6a3ce929d0e0e4736",
      "spans": [
        {
          "traceID": "4bf92f3577b34da6a3ce929d0e0e4736",
          "spanID": "00f067aa0ba902b7",
          "operationName": "POST /api/v1/jobs",
          "serviceName": "job-scheduler",
          "startTime": 1712678625123000,
          "duration": 333000,
          "tags": [...],
          "logs": [...],
          "references": [...]
        },
        ...
      ],
      "processes": {
        "p1": {"serviceName": "job-scheduler", "tags": [...]}
      }
    }
  ]
}
```

**Get Trace by ID**
```
GET /api/traces/{traceId}
GET /api/traces/4bf92f3577b34da6a3ce929d0e0e4736

Response: (same format as above, single trace with all spans)
```

**Get Services**
```
GET /api/services
Response: {"data": ["api-gateway", "job-scheduler", "resource-manager", ...]}
```

**Get Operations for Service**
```
GET /api/services/{service}/operations
GET /api/services/job-scheduler/operations
Response: {"data": ["POST /api/v1/jobs", "GET /api/v1/jobs/{id}", "DELETE /api/v1/jobs/{id}"]}
```

**Get Dependencies (Service Map)**
```
GET /api/dependencies?endTs=1712678400000&lookback=86400000
Response:
{
  "data": [
    {"parent": "api-gateway", "child": "job-scheduler", "callCount": 1234567},
    {"parent": "job-scheduler", "child": "resource-manager", "callCount": 987654},
    {"parent": "resource-manager", "child": "mysql", "callCount": 876543}
  ]
}
```

### Ingestion APIs

**OTLP gRPC (OpenTelemetry Protocol)**
```protobuf
service TraceService {
  rpc Export(ExportTraceServiceRequest) returns (ExportTraceServiceResponse);
}

message ExportTraceServiceRequest {
  repeated ResourceSpans resource_spans = 1;
}

message ResourceSpans {
  Resource resource = 1;
  repeated ScopeSpans scope_spans = 2;
}
```

**OTLP HTTP (JSON)**
```
POST /v1/traces
Content-Type: application/json

{
  "resourceSpans": [
    {
      "resource": {
        "attributes": [
          {"key": "service.name", "value": {"stringValue": "job-scheduler"}}
        ]
      },
      "scopeSpans": [
        {
          "scope": {"name": "io.opentelemetry.spring-webmvc"},
          "spans": [...]
        }
      ]
    }
  ]
}
```

### Admin APIs

```
# Sampling configuration
PUT /api/v1/admin/sampling
{
  "service": "job-scheduler",
  "strategy": "probabilistic",
  "param": 0.1
}

# Health check
GET /api/v1/admin/health
Response: {"collector": "healthy", "storage": "healthy", "kafka": "healthy"}

# Trace statistics
GET /api/v1/admin/stats
Response: {"spansIngested": 432000000, "tracesStored": 54000000, "storageUsed": "14.3TB"}
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Sampling Strategies (Head-Based vs. Tail-Based)

**Why it's hard:**
At 4M spans/sec (pre-sampling), storing all traces is prohibitively expensive (43 TB/day). Sampling reduces volume but risks losing interesting traces (errors, high latency). The fundamental tension: sampling decisions must be consistent across all spans in a trace (you cannot sample some spans and drop others), but interesting signals (error, latency) are often known only at trace completion, not at trace start.

**Approaches:**

| Strategy | Description | Pros | Cons |
|---|---|---|---|
| **No sampling** | Store everything | Complete data | Cost-prohibitive at scale (43 TB/day) |
| **Head-based: Probabilistic** | Decide at trace start; each trace has X% chance of being sampled | Simple, consistent, predictable cost | Blind to trace content; may miss rare errors |
| **Head-based: Rate-limiting** | Cap at N traces/sec per service | Predictable throughput | Burst traffic may exceed cap; unfair across operations |
| **Head-based: Deterministic** | Hash trace_id, sample if hash < threshold | Consistent across services (same trace_id = same decision) | Same blindness as probabilistic |
| **Tail-based** | Buffer all spans, decide after trace completes based on content | Can keep 100% of errors and high-latency traces | Complex, requires buffering, higher resource cost |
| **Hybrid (head + tail)** | Head-based for baseline + tail-based for interesting traces | Best of both worlds | Most complex to implement |

**Selected approach: Hybrid (deterministic head-based + tail-based for errors/latency)**

**Head-Based Sampling Implementation:**
```yaml
# OTel SDK configuration (per-service)
otel:
  traces:
    sampler:
      type: parentbased_traceidratio
      arg: 0.1  # 10% sampling rate

# How parentbased_traceidratio works:
# 1. If incoming request has a parent span with sampling flag set → follow parent's decision
# 2. If no parent (root span) → hash trace_id, sample if hash < 0.1 * MAX_HASH
# 3. Sampling decision propagated via W3C traceparent header:
#    traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
#                                                                          ^^
#                                                                    01 = sampled
#                                                                    00 = not sampled
```

**Tail-Based Sampling Implementation:**

```yaml
# OTel Collector configuration with tail sampling processor
processors:
  tail_sampling:
    decision_wait: 30s        # Wait 30s for trace to complete
    num_traces: 100000        # Max traces in decision buffer
    expected_new_traces_per_sec: 50000
    policies:
      # Policy 1: Always sample errors
      - name: error-policy
        type: status_code
        status_code:
          status_codes: [ERROR]

      # Policy 2: Always sample high-latency traces (>5s)
      - name: latency-policy
        type: latency
        latency:
          threshold_ms: 5000

      # Policy 3: Probabilistic for everything else
      - name: probabilistic-policy
        type: probabilistic
        probabilistic:
          sampling_percentage: 1

      # Policy 4: Always sample specific operations
      - name: critical-operations
        type: string_attribute
        string_attribute:
          key: http.url
          values: ["/api/v1/jobs", "/api/v1/resources/allocate"]
          enabled_regex_matching: true
```

**Tail-Based Sampling Architecture:**
```
┌─────────────────────────────────────────────────────────────┐
│                 TAIL SAMPLING SERVICE                         │
│                                                               │
│  ┌─────────────┐    ┌──────────────┐    ┌─────────────────┐ │
│  │ Span Buffer  │    │ Trace        │    │ Decision        │ │
│  │ (in-memory)  │───▶│ Assembler    │───▶│ Engine          │ │
│  │              │    │ (group spans │    │ (evaluate       │ │
│  │ Key: traceId │    │  by traceId) │    │  policies on    │ │
│  │ TTL: 30s     │    │              │    │  complete trace)│ │
│  └─────────────┘    └──────────────┘    └────────┬────────┘ │
│                                                   │          │
│         ┌────────────────────────────────────────┘          │
│         │ SAMPLE                    │ DROP                   │
│         ▼                           ▼                        │
│  ┌─────────────┐            ┌─────────────┐                 │
│  │ Export to    │            │ Discard     │                 │
│  │ Kafka/Jaeger │            │ (free mem)  │                 │
│  └─────────────┘            └─────────────┘                 │
└─────────────────────────────────────────────────────────────┘
```

**Challenge: Trace completion detection.**
How do you know a trace is "complete"? You do not, with certainty. The system uses a `decision_wait` timer (30s). After 30s with no new spans arriving for a trace_id, the trace is considered complete and the sampling decision is made. This means:
- Traces with spans arriving >30s after the first span may be partially sampled.
- The buffer must hold 30s worth of all traces: at 500K spans/sec x 30s = 15M spans x 1 KB = ~15 GB RAM.

**Challenge: Distributed tail sampling.**
If OTel Collectors are horizontally scaled, spans from the same trace may arrive at different collector instances. Solutions: (1) Use Kafka with trace_id as partition key -- all spans for a trace go to the same partition/consumer. (2) Use a dedicated tail-sampling service that receives all spans (bottleneck risk). (3) Use the OTel Collector's `loadbalancing` exporter to route by trace_id to specific tail-sampling instances.

**Failure Modes:**
- **Buffer overflow**: More traces in flight than `num_traces`. Oldest traces are force-decided (sampled or dropped based on default policy). Monitor `otelcol_processor_tail_sampling_count_traces_dropped`.
- **Late-arriving spans**: Spans arriving after decision_wait are either dropped (trace already decided) or sent without context. Mitigation: increase `decision_wait` (at the cost of more memory) or accept partial traces.
- **Tail-sampling service crash**: Buffered spans are lost. Upstream (Kafka) retains data for replay. But sampling decisions are lost -- replayed spans need re-decision.

**Interviewer Q&As:**

**Q1: Why not just sample 100% of everything?**
A: At 4M spans/sec with 1 KB/span, that is 43 TB/day or ~310 TB/week. At $0.05/GB/month for Elasticsearch SSD, that is ~$15,500/day just for storage, plus compute for indexing. With 10% head-based sampling, we reduce to ~4.3 TB/day ($1,550/day). Tail-based sampling adds ~10% more (high-value traces) for a total of ~5 TB/day. The 10x cost reduction makes the system economically viable.

**Q2: How do you ensure sampling consistency across services in a distributed trace?**
A: The W3C TraceContext specification propagates the sampling decision in the `traceparent` header's flags byte. When service A decides to sample a trace (flag = 01), it includes this flag in the outgoing header to service B. Service B's SDK uses `parentbased` sampler: if parent says "sampled," B also samples. This ensures all services in a trace agree on the sampling decision. For tail-based sampling, all spans are initially collected (but may be dropped later by the tail sampler).

**Q3: How do you handle traces that span asynchronous Kafka messages?**
A: OpenTelemetry's Kafka instrumentation propagates trace context in Kafka message headers. The producer creates a "PRODUCER" span and injects `traceparent` into the message header. The consumer extracts `traceparent` from the header and creates a "CONSUMER" span with the producer span as parent. The trace spans both the synchronous HTTP path and the asynchronous Kafka path. The trace may appear as two "branches" in the Jaeger UI, connected via the Kafka producer/consumer link.

**Q4: What happens when a service is not instrumented? Does the trace break?**
A: If service C (between B and D) is not instrumented, it does not propagate the `traceparent` header. Service D receives no trace context and starts a new trace. The trace appears as two disconnected traces: A→B and D→E. This is a common problem during incremental rollout. Mitigation: (1) Service mesh (Istio/Envoy) can propagate `traceparent` at the proxy level, even if the application is not instrumented. (2) Prioritize instrumenting all services on the critical path.

**Q5: How do you handle trace_id collision?**
A: OpenTelemetry trace IDs are 128-bit (16 bytes) random values. The probability of collision is approximately 1 in 2^64 for the birthday paradox threshold. With 500K traces/sec, it would take ~600 million years to have a 50% chance of collision. In practice, collisions are not a concern. If using 64-bit trace IDs (older Zipkin format), collision risk is higher -- migrate to 128-bit.

**Q6: How do you implement adaptive sampling (dynamically adjusting sample rate)?**
A: The OTel Collector can implement adaptive sampling by: (1) Monitoring the incoming span rate per service. (2) If rate exceeds a target (e.g., 50K spans/sec per service), reduce the probabilistic sampling rate. (3) If rate is below target, increase sampling rate (up to 100%). (4) Jaeger's adaptive sampling works similarly: the Jaeger Agent communicates with the Jaeger Collector to get per-service sampling strategies, which are updated based on observed traffic. The key metric is target traces per second per service, not a fixed percentage.

---

### Deep Dive 2: OpenTelemetry Java Agent Auto-Instrumentation

**Why it's hard:**
The Java platform runs thousands of services with diverse frameworks (Spring Boot, gRPC, JDBC, Kafka, Redis, Elasticsearch clients). Manually instrumenting every library call with span creation and context propagation is impractical. The OpenTelemetry Java Agent uses bytecode manipulation (via Java Instrumentation API) to automatically instrument supported libraries at JVM load time, with zero code changes.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Manual SDK instrumentation** | Full control, precise span boundaries | Enormous engineering effort (5000 services), maintenance burden, inconsistent |
| **OpenTelemetry Java Agent (javaagent)** | Zero code changes, 100+ libraries auto-instrumented, consistent | Black-box behavior, potential performance overhead, version compatibility |
| **Spring Cloud Sleuth/Micrometer Tracing** | Spring-native, good Spring Boot integration | Only Spring ecosystem, no non-Spring coverage |
| **Jaeger client library** | Mature, well-tested | Deprecated in favor of OpenTelemetry, vendor-specific |
| **Bytecode instrumentation (custom)** | Full control | Massive engineering effort, maintenance nightmare |

**Selected approach: OpenTelemetry Java Agent with selective manual instrumentation**

**Implementation Detail:**

```bash
# JVM startup with OTel Java Agent
java -javaagent:/opt/opentelemetry-javaagent.jar \
     -Dotel.service.name=job-scheduler \
     -Dotel.exporter.otlp.endpoint=http://otel-collector:4317 \
     -Dotel.exporter.otlp.protocol=grpc \
     -Dotel.resource.attributes=service.version=2.4.1,k8s.cluster.name=prod-east-1 \
     -Dotel.traces.sampler=parentbased_traceidratio \
     -Dotel.traces.sampler.arg=0.1 \
     -Dotel.instrumentation.jdbc.enabled=true \
     -Dotel.instrumentation.kafka.enabled=true \
     -Dotel.instrumentation.spring-webmvc.enabled=true \
     -jar job-scheduler.jar
```

**What the agent auto-instruments:**
```
HTTP Servers:  Spring MVC, Spring WebFlux, Servlet, Netty, Tomcat, Jetty
HTTP Clients:  OkHttp, Apache HttpClient, Spring RestTemplate, WebClient
gRPC:          gRPC client and server
Database:      JDBC (MySQL, PostgreSQL), Hibernate, MyBatis
Messaging:     Kafka producer/consumer, RabbitMQ, Redis (Jedis, Lettuce)
Caching:       Redis, Memcached
Other:         Elasticsearch client, AWS SDK, gRPC, Reactor
```

**How the agent works internally:**
1. JVM loads the agent JAR via `-javaagent` flag (premain method).
2. Agent registers a ClassFileTransformer with the Java Instrumentation API.
3. As classes are loaded, the transformer checks each class against registered instrumentation modules.
4. For matched classes (e.g., `org.springframework.web.servlet.DispatcherServlet`), the agent injects bytecode using ByteBuddy library:
   - Before method entry: create span, set attributes, start timer.
   - After method exit: record duration, set status, end span.
   - On exception: record error, set status to ERROR.
5. Context propagation: the agent wraps executor services and thread pools to propagate trace context across thread boundaries.

**Manual instrumentation for custom business logic:**
```java
import io.opentelemetry.api.GlobalOpenTelemetry;
import io.opentelemetry.api.trace.Span;
import io.opentelemetry.api.trace.Tracer;

@Service
public class ResourceAllocator {
    private final Tracer tracer = GlobalOpenTelemetry.getTracer("job-scheduler");

    public AllocationResult allocateGPU(Job job) {
        Span span = tracer.spanBuilder("allocateGPU")
            .setAttribute("job.id", job.getId())
            .setAttribute("resource.type", "GPU")
            .setAttribute("resource.count", job.getGpuCount())
            .startSpan();

        try (Scope scope = span.makeCurrent()) {
            // Business logic here
            AllocationResult result = resourcePool.allocate(job);

            span.setAttribute("allocation.success", result.isSuccess());
            span.setAttribute("allocation.pool", result.getPool());

            if (!result.isSuccess()) {
                span.setStatus(StatusCode.ERROR, "Allocation failed: " + result.getReason());
                span.recordException(new AllocationException(result.getReason()));
            }

            return result;
        } catch (Exception e) {
            span.setStatus(StatusCode.ERROR, e.getMessage());
            span.recordException(e);
            throw e;
        } finally {
            span.end();
        }
    }
}
```

**Kubernetes Deployment:**
```yaml
# Pod spec with OTel Java Agent
spec:
  containers:
    - name: job-scheduler
      image: infra/job-scheduler:2.4.1
      env:
        - name: JAVA_TOOL_OPTIONS
          value: "-javaagent:/opt/otel/opentelemetry-javaagent.jar"
        - name: OTEL_SERVICE_NAME
          value: "job-scheduler"
        - name: OTEL_EXPORTER_OTLP_ENDPOINT
          value: "http://otel-collector.observability:4317"
        - name: OTEL_TRACES_SAMPLER
          value: "parentbased_traceidratio"
        - name: OTEL_TRACES_SAMPLER_ARG
          value: "0.1"
        - name: OTEL_RESOURCE_ATTRIBUTES
          value: "k8s.cluster.name=prod-east-1,k8s.namespace.name=$(K8S_NAMESPACE)"
        - name: K8S_NAMESPACE
          valueFrom:
            fieldRef:
              fieldPath: metadata.namespace
      volumeMounts:
        - name: otel-agent
          mountPath: /opt/otel
  initContainers:
    - name: otel-agent-init
      image: infra/opentelemetry-javaagent:1.32.0
      command: ["cp", "/javaagent.jar", "/opt/otel/opentelemetry-javaagent.jar"]
      volumeMounts:
        - name: otel-agent
          mountPath: /opt/otel
  volumes:
    - name: otel-agent
      emptyDir: {}
```

**Performance overhead:**
- CPU: 2-5% increase (bytecode transformation, span creation, context propagation).
- Memory: 50-100 MB additional heap (agent classes, span buffers, exporter queues).
- Latency: < 1ms per span creation (typically 10-50 microseconds).
- Network: OTLP export is batched and compressed; negligible compared to application traffic.

**Failure Modes:**
- **Agent crash**: The Java agent runs in-process. A bug in the agent (rare) can crash the JVM. Mitigation: thorough testing in staging, gradual rollout, pin agent version.
- **Exporter queue full**: If the OTel Collector is unreachable, the SDK's BatchSpanProcessor queue fills up. Default: 2048 spans. When full, spans are dropped silently (configurable). Application is not blocked.
- **Context propagation failure**: Thread pool reuse without context wrapping loses trace context. The agent wraps common executors (`java.util.concurrent`), but custom thread pools may not be wrapped. Fix: use `Context.taskWrapping(executor)` for custom pools.
- **Library version conflict**: The agent shades its dependencies, but some older applications may have bytecode compatibility issues. Fix: use `otel.javaagent.extensions` to customize or disable specific instrumentation modules.

**Interviewer Q&As:**

**Q1: How does the OTel Java Agent handle context propagation across thread boundaries?**
A: The agent instruments commonly used executor services (`ThreadPoolExecutor`, `ForkJoinPool`, Spring's `@Async` executor, Reactor's schedulers). It wraps submitted `Runnable`/`Callable` objects to capture the current trace context at submission time and restore it when the task executes on the worker thread. Internally, this uses `Context.current().wrap(runnable)`. For custom executors not instrumented by the agent, developers use `Context.taskWrapping(executorService)` explicitly.

**Q2: How does the agent instrument JDBC without modifying the database driver?**
A: The agent intercepts calls to `java.sql.DriverManager.getConnection()` and wraps the returned `Connection` object in a tracing proxy. Every call to `connection.prepareStatement()`, `statement.executeQuery()`, etc., is intercepted by the proxy, which creates a span with the SQL query as the operation name, the database name as an attribute, and the query duration. The original driver is unmodified. Sensitive query parameters can be optionally sanitized.

**Q3: How do you roll out the OTel Agent to 5,000 services safely?**
A: (1) Start with non-production environments (dev, staging) for 4 weeks. (2) Roll out to 5 canary services in production with monitoring of CPU, memory, latency overhead. (3) If overhead < 5%, enable for 50 services (one per team). (4) Gradual rollout: 50 → 200 → 1000 → all services over 8 weeks. (5) Each phase: compare latency percentiles before/after. (6) Kill switch: `OTEL_JAVAAGENT_ENABLED=false` environment variable disables the agent without redeploying. (7) All rollouts are controlled by Kubernetes Deployment annotations and feature flags.

**Q4: How do you handle tracing for services behind Envoy/Istio service mesh?**
A: Envoy/Istio generates its own spans for proxy-level operations (connection establishment, TLS handshake, upstream request). These spans use the same W3C TraceContext headers, so they appear in the same trace as application-level spans. The trace shows: `client-app → envoy-sidecar (client) → envoy-sidecar (server) → server-app`. This gives visibility into mesh overhead. Configuration: Istio's `meshConfig.enableTracing` and `extensionProviders` with OpenTelemetry export.

**Q5: How does the agent handle Spring Boot Actuator health endpoints?**
A: By default, the agent instruments all HTTP endpoints, including health checks (`/actuator/health`). Since health checks are called frequently (every 10-15s by Kubernetes liveness/readiness probes), they generate a large volume of low-value spans. Solution: configure agent to exclude these endpoints via `otel.instrumentation.http.server.capture-request-url-path-template=true` and `otel.instrumentation.spring-webmvc.excluded-urls=/actuator/**` or use sampling rules that drop health check spans.

**Q6: What are the differences between OpenTelemetry and the old Jaeger/Zipkin client libraries?**
A: (1) OpenTelemetry is vendor-neutral: exports to any backend (Jaeger, Zipkin, Datadog, Honeycomb) via OTLP or backend-specific exporters. Old clients were tied to one backend. (2) OpenTelemetry covers traces, metrics, and logs in a single SDK. Old clients were traces-only. (3) OpenTelemetry has a richer data model: span events, links, status codes, semantic conventions. (4) The Java Agent auto-instruments 100+ libraries. Old agents covered fewer. (5) Jaeger and Zipkin clients are officially deprecated in favor of OpenTelemetry. Migration path: use OTel SDK with Jaeger/Zipkin exporters for backward compatibility.

---

### Deep Dive 3: W3C TraceContext Propagation

**Why it's hard:**
Trace context must be propagated across process boundaries: HTTP headers, gRPC metadata, Kafka message headers, and even through databases (via stored trace_id). Any break in propagation creates disconnected trace fragments, making end-to-end debugging impossible. The W3C TraceContext standard provides interoperability, but real-world systems have legacy propagation formats, proxies that strip headers, and async processing patterns that break context.

**W3C TraceContext Header Format:**
```
traceparent: {version}-{trace-id}-{parent-id}-{trace-flags}
traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01

version:     00 (current version)
trace-id:    4bf92f3577b34da6a3ce929d0e0e4736 (128-bit, 32 hex chars)
parent-id:   00f067aa0ba902b7 (64-bit span ID, 16 hex chars)
trace-flags: 01 (bit 0 = sampled)

tracestate: vendor1=value1,vendor2=value2
(optional: vendor-specific context, e.g., for multi-tenant routing)
```

**Propagation across different transports:**

| Transport | Header/Mechanism | Example |
|---|---|---|
| HTTP | `traceparent` and `tracestate` headers | `traceparent: 00-abc...-def...-01` |
| gRPC | gRPC metadata (same as HTTP headers) | `traceparent: 00-abc...-def...-01` |
| Kafka | Message headers (binary or string) | Header key: `traceparent`, value: `00-abc...-def...-01` |
| RabbitMQ | Message properties (headers map) | `headers: {"traceparent": "00-abc...-def...-01"}` |
| MySQL | Comment in SQL query or session variable | `/* traceparent=00-abc...-def...-01 */ SELECT ...` |
| Thread pool | Context wrapper on Runnable/Callable | `Context.current().wrap(runnable)` |

**Failure Modes:**
- **Proxy strips headers**: A reverse proxy or API gateway that does not forward `traceparent` breaks propagation. Fix: configure proxy to forward all `traceparent*` headers.
- **B3 vs W3C format mismatch**: Older Zipkin services use B3 format (`X-B3-TraceId`, `X-B3-SpanId`). OTel supports multi-format propagation: `otel.propagators=tracecontext,b3multi`.
- **Context lost in async processing**: A message is consumed from Kafka and processed later without extracting trace context. Fix: always extract `traceparent` from message headers in the consumer.

**Interviewer Q&As:**

**Q1: How do you propagate trace context through a Kafka message broker?**
A: The OpenTelemetry Kafka instrumentation injects `traceparent` into Kafka message headers when producing. On the consumer side, it extracts `traceparent` from the message header and creates a new span linked to the producer span. The producer span kind is `PRODUCER`, and the consumer span kind is `CONSUMER`. The trace shows the asynchronous flow: `...producer-span → [kafka] → consumer-span...`. The latency between producer and consumer spans represents the Kafka queue time.

**Q2: How do you handle trace propagation through a service that does not support W3C TraceContext?**
A: Options: (1) If the service is a proxy/gateway, configure it to forward `traceparent` header even if it does not create spans. (2) If the service is a legacy application, use the OpenTelemetry Java Agent to instrument it (zero code change). (3) If the service cannot be modified at all, accept the trace break and document it. (4) Use the OTel Collector's `routing` feature to correlate upstream and downstream traces via timing heuristics.

**Q3: What is the difference between `traceparent` and `tracestate`?**
A: `traceparent` is the mandatory header containing trace_id, span_id, and flags. It is used by all participants for trace correlation. `tracestate` is optional and carries vendor-specific data. For example, a multi-tenant system might use `tracestate: tenant=abc123` to route traces to tenant-specific storage. `tracestate` is opaque to most participants; they forward it unchanged.

**Q4: How do you ensure trace context is not lost in a retry/dead-letter-queue scenario?**
A: When a message fails processing and is sent to a DLQ: (1) The DLQ message should retain the original `traceparent` header. (2) When the DLQ message is reprocessed, the consumer extracts the original trace context and creates a new span linked (via Span Link, not parent-child) to the original trace. (3) The reprocessed span gets a new trace_id (since it is a new processing attempt) but links back to the original trace for correlation.

**Q5: How do you correlate database queries with the calling trace?**
A: The OTel Java Agent instruments JDBC calls, creating a span for each query. This span is a child of the calling service's span, so the trace shows `service-span → jdbc-span`. The JDBC span includes attributes: `db.system=mysql`, `db.name=jobs_db`, `db.statement=SELECT * FROM jobs WHERE id=?` (sanitized). For deeper DB-side correlation, some teams inject `trace_id` as a SQL comment (`/* trace_id=abc... */`) so it appears in MySQL's slow query log and can be cross-referenced.

**Q6: How do you propagate trace context through a serverless function invocation?**
A: For AWS Lambda invoked via API Gateway: the `traceparent` header is passed through the API Gateway to the Lambda function's event input. The Lambda handler extracts it and creates a child span. For Lambda invoked via SQS/SNS: the trace context is placed in message attributes. For Lambda invoked via direct invocation (SDK): the trace context is passed in the `ClientContext` field. The OTel Lambda extension handles this automatically for supported runtimes.

---

## 7. Scaling Strategy

### Scaling Dimensions

| Component | Scaling Mechanism | Trigger |
|---|---|---|
| **OTel Collector** | Kubernetes HPA (CPU/memory) | CPU > 70%, incoming span rate |
| **Kafka (traces topic)** | Add partitions + brokers | Consumer lag growing, disk > 70% |
| **Jaeger Ingester** | Add consumer instances (up to partition count) | Kafka consumer lag > 1M |
| **Elasticsearch (trace storage)** | Add data nodes | Indexing latency > 300ms, disk > 80% |
| **Jaeger Query** | Horizontal replicas behind LB | Query latency > 5s, concurrent users |

**Interviewer Q&As:**

**Q1: How do you handle 4M spans/sec before sampling?**
A: The OTel Collector is deployed as a DaemonSet (one per Kubernetes node) to minimize network hops from applications. Each DaemonSet pod handles spans from ~50-100 pods on that node. With 200 clusters x ~500 nodes per cluster = 100,000 collector instances, each handling ~40 spans/sec (after sampling at the SDK level). For pre-sampling aggregation, we deploy a "gateway" OTel Collector (Deployment) that receives from DaemonSet collectors and performs tail-sampling before exporting to Kafka.

**Q2: How do you handle trace storage at 500K spans/sec?**
A: Elasticsearch with 10 dedicated data nodes for tracing. Each node handles ~50K span writes/sec (bulk indexing). Daily index rotation with ILM. Key optimization: disable `_all` field, use `keyword` types for all tag keys/values (no full-text search on tags), set `refresh_interval: 10s` (trace search can tolerate 10s delay). Alternative: Cassandra can handle higher write throughput with less tuning.

**Q3: What is the bottleneck in the tracing pipeline?**
A: Typically Elasticsearch bulk indexing throughput. Mitigations: (1) Increase bulk request size (5-15 MB optimal). (2) Add more data nodes. (3) Reduce span size by dropping low-value attributes. (4) Increase refresh interval. (5) Use Cassandra for writes and Elasticsearch only for search (hybrid approach used by Jaeger's Spark integration).

**Q4: How do you handle trace fan-out from a single high-traffic service?**
A: A service like the API gateway generates a span for every request (500K rps). Even with 10% sampling, that is 50K spans/sec from one service. Solutions: (1) Per-service sampling rates: API gateway at 1%, backend services at 10%. (2) Head-based sampling at the SDK level reduces collector load. (3) Kafka partition by trace_id distributes load across Jaeger Ingesters.

**Q5: How do you scale the OTel Collector for tail-based sampling?**
A: Tail-based sampling requires all spans for a trace to arrive at the same collector instance. Options: (1) Deploy tail-sampling as a second-tier collector that receives from first-tier DaemonSet collectors. Use `loadbalancing` exporter in first-tier to route by trace_id. (2) Use Kafka with trace_id as partition key, and run tail-sampling as a Kafka consumer. (3) Accept approximate tail-sampling: each collector independently samples based on local spans, accepting that some multi-collector traces may be inconsistently sampled.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| **OTel Collector crash (DaemonSet)** | Spans from that node buffered in SDK, retried | SDK export failures, collector pod restart count | DaemonSet auto-restart. SDK buffers 2048 spans. | ~30 sec |
| **Kafka broker failure** | Partition leadership shifts, brief write pause | Under-replicated partitions | RF=3, min.ISR=2. Auto leader election. | ~10 sec |
| **Jaeger Ingester crash** | Consumer lag increases, delayed storage | Kafka consumer lag metric | Kubernetes restarts pod. Other ingesters take over partitions. | ~1 min |
| **Elasticsearch trace storage down** | New traces not searchable; old traces unavailable | ES cluster health, indexing errors | Kafka 24h buffer. Replay on recovery. Query fallback to cache. | Variable |
| **Jaeger Query down** | Users cannot search traces | Health check, HTTP errors | Multiple replicas behind LB. | ~30 sec |
| **Network partition (app ↔ collector)** | Spans lost after SDK buffer full | SDK export failure count metric | DaemonSet collector on same node minimizes network dependency. | Self-healing |
| **OTel Agent bug crashes JVM** | Application down | Application health checks, pod restarts | Kill switch: `OTEL_JAVAAGENT_ENABLED=false`, redeploy. | ~5 min |
| **Tail-sampling buffer overflow** | Traces force-decided, some interesting traces lost | `tail_sampling_count_traces_dropped` metric | Increase buffer size, add more tail-sampling instances. | ~5 min |
| **Trace_id propagation failure** | Disconnected traces, partial visibility | Monitor for traces with single spans (no children) | Audit propagation; service mesh passthrough; test in staging. | Configuration fix |
| **High-cardinality tag explosion** | ES mapping explosion, slow queries | ES field count alerts, slow query log | OTel Collector `attributes` processor to limit tag keys. | ~15 min |

---

## 9. Security

### Authentication & Authorization
- **OTel Collector**: Accepts OTLP only from internal pod network (Kubernetes NetworkPolicy). No external access.
- **Kafka**: TLS + SASL/SCRAM. OTel Collector has producer ACL; Jaeger Ingester has consumer ACL.
- **Elasticsearch**: X-Pack Security with RBAC. `jaeger-writer` role for ingesters; `jaeger-reader` for Jaeger Query.
- **Jaeger UI**: SSO via OAuth2 proxy in front of Jaeger Query. Role-based access: all engineers can view traces for their team's services. Platform team can view all.

### Data Sensitivity in Traces
- **Request/response bodies**: Never stored in spans by default. If needed for debugging, use OTel's `http.request.body` attribute with explicit opt-in and automatic PII detection.
- **SQL queries**: Sanitized by default (parameter values replaced with `?`). Full queries available only in debug mode.
- **Headers**: `Authorization`, `Cookie`, and other sensitive headers are excluded from span attributes by default.
- **Tag filtering**: OTel Collector's `attributes` processor can remove sensitive attributes before storage.

### Encryption
- In transit: TLS for all OTLP, gRPC, Kafka, and Elasticsearch communication.
- At rest: Elasticsearch encrypted volumes; Kafka encrypted disks.

---

## 10. Incremental Rollout

### Rollout Phases

| Phase | Scope | Duration | Success Criteria |
|---|---|---|---|
| **Phase 0: Infra setup** | Deploy OTel Collector, Kafka topic, Jaeger, ES indices | 2 weeks | Pipeline end-to-end test with synthetic traces |
| **Phase 1: Pilot services** | Instrument 5 critical-path services with OTel Java Agent | 4 weeks | Full traces visible, <5% overhead, team feedback positive |
| **Phase 2: Platform services** | All infrastructure services (scheduler, resource manager, DB proxies) | 4 weeks | Service dependency map complete, latency analysis working |
| **Phase 3: Broad rollout** | All 5,000 services via automated deployment pipeline | 8 weeks | 80% of services instrumented, tail sampling operational |
| **Phase 4: Advanced** | Log-trace correlation, metric exemplars, AI-assisted analysis | Ongoing | MTTR reduction measured |

### Rollout Q&As

**Q1: How do you instrument 5,000 services without touching their code?**
A: The OpenTelemetry Java Agent is injected via a shared init container and environment variable (`JAVA_TOOL_OPTIONS=-javaagent:...`). This is configured in the base Kubernetes deployment template. A platform admission webhook automatically injects the OTel sidecar init container for any Deployment with the label `tracing: enabled`. Teams opt in by adding the label; the platform team can also enable it fleet-wide.

**Q2: How do you validate that traces are correct and complete?**
A: (1) Synthetic trace tests: a test harness sends a known request through 5 services and validates the trace has exactly 5 spans with correct parent-child relationships. (2) "Trace completeness" metric: compare the number of root spans with the number of expected child spans (from the service dependency map). Alert if completeness drops below 95%. (3) Compare span counts per service with request counts from metrics (they should correlate at the sampling rate).

**Q3: How do you handle the performance impact of tracing on latency-sensitive services?**
A: (1) Measure overhead empirically: A/B test with tracing enabled/disabled. Typical overhead is 2-5%. (2) For ultra-latency-sensitive services (<1ms response time), reduce span attributes (fewer tags = less serialization). (3) Use async span export (BatchSpanProcessor) so export never blocks the request path. (4) SDK sampling at the source reduces the number of spans created (not just exported).

**Q4: How do you handle rollback if the OTel Agent causes issues?**
A: Kill switch via environment variable: set `OTEL_JAVAAGENT_ENABLED=false` or `OTEL_SDK_DISABLED=true` and restart pods. This can be done cluster-wide via Kubernetes ConfigMap update. The agent detects the flag and becomes a no-op. No application code change, no redeployment needed (just a pod restart to pick up the new env var).

**Q5: How do you migrate from Jaeger/Zipkin client libraries to OpenTelemetry?**
A: (1) Deploy OTel Collector with both Jaeger and OTLP receivers. (2) Services using old Jaeger client continue sending to the Jaeger receiver. (3) Gradually migrate services to OTel SDK/Agent (they send to OTLP receiver). (4) Both old and new spans flow through the same pipeline to Kafka and storage. (5) Remove old Jaeger clients after all services are migrated. OTel Collector acts as the compatibility layer during transition.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale |
|---|---|---|---|
| **Instrumentation standard** | Jaeger client, Zipkin client, OpenTelemetry | OpenTelemetry | Vendor-neutral, active development, unified traces+metrics+logs, CNCF graduated |
| **Java instrumentation** | Manual SDK, Java Agent, Spring Sleuth | OTel Java Agent | Zero code change, 100+ libraries, consistent across all services |
| **Trace storage** | Elasticsearch, Cassandra, ClickHouse | Elasticsearch | Existing ELK infrastructure, rich search, Kibana correlation |
| **Buffer** | Direct to storage, Redis, Kafka | Kafka | Durability, replay, backpressure handling, consistent with logging pipeline |
| **Sampling** | None, head-only, tail-only, hybrid | Hybrid (head + tail) | Cost control with head-based; retain interesting traces with tail-based |
| **Context propagation** | B3, Jaeger, W3C TraceContext | W3C TraceContext | Industry standard, interoperable, supported by all OTel SDKs |
| **Collector deployment** | Sidecar, DaemonSet, Deployment | DaemonSet + Gateway Deployment | DaemonSet minimizes network hops; Gateway for centralized tail-sampling |
| **Trace ID format** | 64-bit, 128-bit | 128-bit | W3C standard, no collision risk, forward-compatible |
| **Head-based sample rate** | 1%, 5%, 10%, 100% | 10% | Provides enough traces for debugging without excessive cost |

---

## 12. Agentic AI Integration

### AI-Powered Trace Analysis

**Use Case 1: Automated Root Cause Analysis from Traces**
```
AI Agent receives alert: "p99 latency for /api/jobs exceeded 5s"

1. Queries Jaeger for recent slow traces:
   GET /api/traces?service=job-scheduler&operation=POST+/api/v1/jobs&minDuration=5s&limit=50

2. Analyzes trace structure:
   - Identifies the slowest span in each trace
   - Aggregates: 45/50 traces have slow span in "resource-manager.AllocateResources"
   - Within that span: "mysql.SELECT FROM resource_pool" takes 4.2s avg

3. Cross-references with metrics:
   - MySQL query latency p99 spiked from 10ms to 4.5s at 14:15
   - MySQL connection pool utilization at 100%

4. Root cause report:
   "p99 latency spike caused by MySQL connection pool exhaustion in resource-manager.
    The resource_pool table has 5M rows with a full table scan (missing index on pool_id).
    Remediation: Add index on resource_pool(pool_id); increase HikariCP max pool size from 10 to 30."
```

**Use Case 2: Trace-Based Dependency Impact Analysis**
```
AI Agent: "What is the blast radius if the auth-service goes down?"

1. Queries service dependency map from Jaeger:
   GET /api/dependencies

2. Traverses dependency DAG:
   auth-service → [api-gateway, job-scheduler, admin-portal, billing-service]
   api-gateway → [all user-facing endpoints]

3. Calculates impact:
   "auth-service is called by 4 direct dependents and 23 transitive dependents.
    Total impacted request rate: 340,000 rps (68% of all traffic).
    Services with no auth fallback: [billing-service, admin-portal].
    Recommendation: implement auth token caching (5-minute TTL) in api-gateway
    to survive auth-service outages."
```

**Use Case 3: Anomalous Trace Detection**
```
AI Agent continuously analyzes trace structure:

1. Baseline: job-scheduler traces normally have 8 spans, 200ms total latency
2. Detection: new traces have 12 spans (4 additional retry spans to resource-manager)
3. Alert: "job-scheduler traces show 4 retry spans to resource-manager.
   This started at 14:00 and correlates with resource-manager deploy v2.3.1.
   The new version returns HTTP 503 on 30% of requests, triggering client retries."
```

**Use Case 4: Natural Language Trace Search**
```
User: "Show me traces where the payment service took more than 2 seconds
       and there was a database timeout"

AI Agent → Jaeger query:
GET /api/traces?service=payment-service&minDuration=2s&tags={"db.error":"timeout"}
```

**Guardrails:**
- AI agent has read-only access to Jaeger Query API.
- Root cause suggestions are hypotheses, not certainties -- labeled with confidence score.
- Remediation actions (add index, increase pool size) require human approval via change management.
- All AI analysis is logged for review and feedback loop (was the root cause correct?).

---

## 13. Complete Interviewer Q&A Bank

### Fundamentals (Q1-Q5)

**Q1: Explain the difference between a trace, a span, and a span context.**
A: A **trace** represents the full journey of a request through a distributed system. It is a collection of spans forming a directed acyclic graph (DAG). A **span** represents a single unit of work within a trace: one service handling one operation. It has a name, start time, duration, attributes, events, and status. A **span context** is the minimal set of information needed to identify a span and propagate it across process boundaries: trace_id, span_id, and trace flags. Span context is serialized into the `traceparent` header for propagation.

**Q2: What is the difference between SPAN_KIND_CLIENT and SPAN_KIND_SERVER?**
A: When service A calls service B via HTTP: Service A creates a `CLIENT` span (representing the outgoing request from A's perspective). Service B creates a `SERVER` span (representing the incoming request from B's perspective). Both spans share the same trace_id. The CLIENT span's span_id becomes the SERVER span's parent_span_id. Duration of CLIENT span includes network latency; duration of SERVER span is just processing time. The difference in duration reveals network overhead.

**Q3: How do you correlate traces with logs?**
A: The trace_id and span_id are injected into every log line via MDC (Java) or logging context (Python). When viewing a trace in Jaeger, the user can click "View Logs" which links to Kibana with query `trace_id:{id}`. When viewing logs in Kibana, the trace_id is a clickable link to Jaeger. Implementation: OTel Java Agent automatically populates MDC with `trace_id` and `span_id` if Logback/Log4j2 is detected. For Python: `logging_instrumentor` does the same.

**Q4: What are span events vs. span attributes?**
A: **Attributes** are key-value pairs attached to the span, describing the operation (e.g., `http.method=POST`, `db.system=mysql`). They are set once. **Events** are timestamped annotations within a span's lifetime, representing something that happened during the span (e.g., "acquired lock at t=123", "cache miss at t=456"). Events have a name and optional attributes. The most common event is `exception`: `span.recordException(e)` creates an event with the exception type, message, and stack trace.

**Q5: What is the difference between span links and parent-child relationships?**
A: Parent-child: a causal, synchronous relationship. The child span cannot exist without the parent. Example: service A calls service B → B's span is a child of A's span. **Links**: a reference to another span without a causal relationship. Example: a batch job processes messages from 100 different requests. The batch processing span links to all 100 producer spans. Links express "this span is related to these other spans" without implying a timing dependency.

### Architecture & Design (Q6-Q10)

**Q6: Why use Kafka as a buffer between collectors and Jaeger storage?**
A: (1) **Backpressure**: If Elasticsearch is slow, Kafka absorbs the overflow (24h retention). Without Kafka, collector memory would fill and spans would be dropped. (2) **Replay**: If we need to reindex traces (e.g., after fixing a mapping), we can reset Kafka consumer offsets. (3) **Multiple consumers**: Kafka allows separate consumers for storage, analytics (service map computation), and alerting without duplicating ingestion. (4) **Ordering**: Kafka with trace_id key ensures all spans for a trace go to the same partition, enabling partition-local tail-sampling.

**Q7: How does the service dependency map work in Jaeger?**
A: Two approaches: (1) **Real-time**: Jaeger analyzes traces as they are stored and extracts parent_service → child_service relationships. The `/api/dependencies` endpoint queries the last N hours and aggregates call counts. (2) **Batch (Spark/Flink)**: A separate job reads all traces from storage, computes the full dependency graph with call counts and error rates, and writes it to a dedicated table/index. This is more accurate but delayed. The dependency graph is displayed as a DAG in the Jaeger UI.

**Q8: How do you handle tracing in a polyglot environment (Java, Python, Go, Node.js)?**
A: OpenTelemetry provides SDKs for all major languages, all implementing the same W3C TraceContext propagation. Java: auto-instrumentation agent. Python: auto-instrumentation via `opentelemetry-instrument` CLI tool or manual SDK. Go: manual instrumentation with OTel Go SDK (no auto-agent due to Go's compilation model). Node.js: auto-instrumentation via `@opentelemetry/auto-instrumentations-node`. All SDKs export via OTLP to the same OTel Collector. The key requirement is that all SDKs propagate the same `traceparent` format.

**Q9: How do you handle traces that are hours long (e.g., a long-running batch job)?**
A: Standard Jaeger storage assumes traces complete within minutes. For long-running traces: (1) Use span events to mark progress within a long-running span (rather than many child spans). (2) Increase tail-sampling `decision_wait` for specific operations (or disable tail-sampling and use head-based only). (3) For batch jobs, create a parent span at job start and child spans per batch chunk. The parent span ends when the job completes. (4) Long-duration spans consume more buffer memory in tail-sampling -- size buffers accordingly.

**Q10: How would you design a tracing system from scratch without OpenTelemetry?**
A: Core components: (1) **Context propagation**: Define a header format (like traceparent) and implement injection/extraction in HTTP/gRPC/Kafka clients. (2) **Span creation**: Intercept framework entry/exit points (HTTP handlers, DB drivers) to create spans. In Java, use bytecode manipulation (ByteBuddy) or AOP (AspectJ). (3) **Export**: Batch spans and send to a collector via gRPC (efficient binary protocol). (4) **Collection**: A stateless collector that receives, batches, and forwards to storage. (5) **Storage**: Time-series-aware database (Elasticsearch or Cassandra) with trace_id as primary key. (6) **Query**: API that assembles traces from stored spans and serves a UI. This is essentially what Jaeger and Zipkin built before OpenTelemetry standardized the instrumentation layer.

### Sampling & Performance (Q11-Q15)

**Q11: You have a service that generates 1M spans/sec. How do you handle it?**
A: (1) SDK-level head-based sampling at 0.1% (1000 spans/sec after sampling). (2) Tail-based sampling captures 100% of error traces (additional ~500 spans/sec for a 5% error rate). (3) Total: ~1500 spans/sec. (4) If 1500/sec is still too much, increase `batch` processor timeout and size in the collector to amortize overhead. (5) Ensure the service's spans are not skewing the Kafka partition distribution (use trace_id key, not service name).

**Q12: How do you calculate the error rate from trace data?**
A: OTel Collector's `spanmetrics` connector automatically derives metrics from spans: `traces_spanmetrics_calls_total{service, operation, status_code}` and `traces_spanmetrics_duration_bucket{service, operation, le}`. Error rate = `sum(rate(traces_spanmetrics_calls_total{status_code="STATUS_CODE_ERROR"}[5m])) / sum(rate(traces_spanmetrics_calls_total[5m]))`. This is exported to Prometheus and displayed in Grafana. This means you do not need separate metric instrumentation for RED metrics if you have tracing.

**Q13: What is the overhead of distributed tracing on an application?**
A: Measured empirically on our Java services: (1) **CPU**: +3% (span creation, attribute setting, OTLP serialization). (2) **Memory**: +80 MB heap (agent classes, span buffer, exporter queue). (3) **Latency**: +0.5ms per request (aggregate of context extraction, span creation, context injection). (4) **Network**: +5 KB per sampled request (OTLP export, compressed). At 10% sampling, 90% of requests have minimal overhead (context propagation only, no span export). The SDK's `BatchSpanProcessor` is async and never blocks the request thread.

**Q14: How do you debug a trace that shows a 5-second gap between parent and child spans?**
A: A gap between a CLIENT span ending and the SERVER span starting suggests network latency or queuing. Debugging steps: (1) Check if the gap is in the trace data (clock skew between services) -- ensure NTP sync across hosts. (2) Check if there is a load balancer/proxy between the services that adds latency (look for proxy spans). (3) Check if the server has a connection queue (thread pool saturation -- the request waited in the accept queue). (4) If through Kafka (async), the gap is expected (queue time). (5) If the gap is in the middle of a span (span duration >> actual work), the span may include waiting on a lock or sleeping.

**Q15: How do you handle clock skew between services in distributed traces?**
A: Clock skew makes trace timelines confusing: a child span might appear to start before its parent. Mitigations: (1) All hosts run NTP with high accuracy (<1ms skew). (2) Jaeger UI has built-in clock skew correction: it adjusts child span timestamps to be within the parent span's time range. (3) In cloud environments, use cloud provider's time sync (e.g., AWS Time Sync Service with microsecond accuracy). (4) The OTel SDK records spans using a monotonic clock (not wall clock) for duration calculation, so duration is always accurate even if timestamps have skew.

### Advanced Topics (Q16-Q20)

**Q16: Explain Exemplars and how they connect metrics to traces.**
A: An exemplar is a specific trace_id attached to a metric sample. For example, a histogram bucket for p99 latency might include an exemplar: `{trace_id="abc123"}` pointing to the actual trace that contributed to the p99 value. In Grafana, you see the p99 latency on a graph and can click the exemplar dot to jump directly to the trace in Jaeger. Implementation: Micrometer's Prometheus registry supports exemplars natively with OTel integration. The Prometheus scrape response includes exemplar data in OpenMetrics format.

**Q17: How do you implement distributed context propagation through a database?**
A: Scenario: service A writes a record to MySQL with a job_id. Service B reads the record and processes it. There is no direct HTTP/gRPC call. Options: (1) Store trace_id as a column in the database table. Service B reads the trace_id and creates a span linked to A's trace. (2) Use SQL comments: `/* traceparent=00-abc...-def...-01 */ INSERT INTO jobs ...`. Not practical for reads. (3) Store the trace context in a separate context table keyed by correlation ID (job_id). Service B looks up the context when processing. (4) Use a message broker instead of polling: A publishes an event to Kafka (with trace context in headers), B consumes it.

**Q18: How do you use traces for capacity planning?**
A: Trace data reveals: (1) **Service dependency depth**: if adding a new service adds 2 more hops, predict the latency impact. (2) **Per-service latency contribution**: if service X contributes 40% of end-to-end latency, optimizing X has the highest impact. (3) **Concurrency patterns**: trace fan-out (parallel calls) vs. sequential calls. Optimize by parallelizing sequential calls. (4) **Database query frequency**: traces show how many DB queries each request generates -- optimize N+1 patterns. (5) **Caching effectiveness**: traces show cache hit/miss spans, revealing the cache hit ratio and its impact on latency.

**Q19: How does Jaeger compare to Zipkin, Tempo, and Honeycomb?**
A: **Jaeger**: CNCF graduated, full-featured (search, dependency map, comparison), supports ES/Cassandra. Strong in self-hosted environments. **Zipkin**: Simpler, lighter, earlier project. Less features than Jaeger. **Grafana Tempo**: Trace storage only (no search by attributes -- only trace_id lookup). Ultra-cheap (stores in object store with minimal indexing). Pairs with Grafana for exemplar-based navigation. Best for organizations that primarily navigate from metrics/logs to traces. **Honeycomb**: SaaS, arbitrary high-cardinality querying, excellent for exploration. Expensive at scale. **Our choice**: Jaeger for self-hosted with full search capability. Would consider Tempo if we wanted to minimize infrastructure (trade search for cost).

**Q20: How would you implement tracing for a bare-metal job scheduler that does not use HTTP?**
A: The job scheduler communicates via custom TCP protocol and shared state in MySQL. Approach: (1) Create a custom OTel instrumentation for the TCP client/server that injects/extracts trace context in the protocol headers (add a trace_context field to the protocol). (2) For MySQL-based communication (polling): store trace_id in the job record (as a column). When the executor picks up the job, it reads the trace_id and creates a linked span. (3) For the job lifecycle itself, create spans: `schedule_job → queue_job → allocate_resources → execute_job → complete_job`. Each transition creates a child span. (4) The batch scheduler can use Pushgateway-like pattern: push span data to a local agent that exports to the OTel Collector.

---

## 14. References

1. **Distributed Systems Observability** - Cindy Sridharan (O'Reilly)
2. **OpenTelemetry Documentation**: https://opentelemetry.io/docs/
3. **W3C Trace Context Specification**: https://www.w3.org/TR/trace-context/
4. **Jaeger Documentation**: https://www.jaegertracing.io/docs/
5. **Dapper: Google's Distributed Tracing Infrastructure** - Sigelman et al. (Google, 2010)
6. **OpenTelemetry Java Agent**: https://opentelemetry.io/docs/instrumentation/java/automatic/
7. **Tail-Based Sampling in OpenTelemetry Collector**: https://opentelemetry.io/docs/collector/configuration/#tail-sampling-processor
8. **Jaeger Architecture**: https://www.jaegertracing.io/docs/architecture/
9. **Exemplars in Prometheus and Grafana**: https://grafana.com/docs/grafana/latest/fundamentals/exemplars/
10. **OTLP Specification**: https://opentelemetry.io/docs/specs/otlp/
