# Common Patterns — Observability (08_observability)

## Common Components

### Elasticsearch as Primary Search Store (5 of 6 Systems)
- alerting_and_on_call, anomaly_detection, audit_log, distributed_logging, distributed_tracing all use Elasticsearch
- **Only metrics_and_monitoring does NOT** (uses Prometheus TSDB + Thanos + S3)
- Shared indexing strategy: daily rollover indices — `{prefix}-YYYY-MM-DD` (e.g., `logs-2026-04-09`, `audit-2026-04-09`, `jaeger-span-2026-04-09`)
- All keyword fields (service, level, actor.id, traceID, etc.) indexed as `type: keyword`; free-text fields (message, stack_trace) as `type: text`
- `dynamic: false` on all templates to prevent field explosion from arbitrary logs
- ILM (Index Lifecycle Management) for hot→warm→cold→delete tiering is standard across all ES users

### Kafka as Ingestion Buffer (All 6 Systems)
- All 6 use Kafka to buffer high-volume inbound data before downstream processing
- Pattern: Source → Kafka (durability guaranteed) → Consumer(s): streaming path + archival path + SIEM/downstream
- Durability settings vary by system priority:
  - `acks=all`: audit logs (zero loss), high-priority ERROR logs
  - `acks=1` (leader only): general application logs (throughput > durability)
- Compression: audit=zstd (best ratio), logs=lz4 (best throughput), tracing=standard
- Kafka retention: audit=72h, logs=48h, tracing=24h (replay window for consumer recovery)
- Partition count drives parallelism: 128 (audit), 512+ (logs), 256 (tracing)

### Multi-Tier Storage: Hot SSD → Warm HDD → Cold S3 → Delete (4 of 6 Systems)
- audit_log, distributed_logging, distributed_tracing, metrics all use tiered storage
- **Hot tier (SSD)**: recent data, full resolution, actively queried; ILM triggers: age or index size
- **Warm tier (HDD)**: rolled from hot; force-merged to 1 segment/shard (read-optimized, no more writes); reduced replicas
- **Cold tier (S3)**: searchable snapshots; query hits S3 without copying back to cluster; dramatically lower cost
- **Delete**: compliance deadline; audit exception (7-year retention via Object Lock WORM)
- Cost optimization principle: SSD ≈ 10× more expensive than HDD ≈ 5× more expensive than S3 object

### Alertmanager as Central Alert Routing Hub (4 of 6 Systems)
- alerting, anomaly_detection, audit_log (suspicious activity), distributed_logging (ElastAlert) all route through Alertmanager
- **Alertmanager configuration**: `group_by: ['alertname', 'cluster', 'service']`; `group_wait: 30s`; `group_interval: 5m`; `repeat_interval: 4h`; `resolve_timeout: 5m`
- Alertmanager provides: deduplication by fingerprint, grouping (1000 alerts → 1 notification), inhibition rules, silence management
- Destinations: PagerDuty (critical), Slack (warning/info), email
- All observability signals (metric threshold, ML anomaly, log pattern, security event) converge here

### W3C TraceContext Propagation / Trace ID Correlation (logging + tracing)
- `traceparent` header: `00-{traceId}-{spanId}-{flags}` propagated across HTTP, gRPC, and Kafka
- `trace_id` field mandatory in every structured log event; enables click-through from log → Jaeger trace → Grafana metrics
- Sampling decision in `flags` byte: `01` = sampled; propagated by parent to all downstream services ensures consistent sampling within a trace

### Time-Based Sharding (All 6 Systems)
- All partition data by time to enable fast deletion, archival, and per-period retention policies
- Elasticsearch: daily indices, drop by deleting index (O(1) vs. delete-by-query O(n))
- Prometheus TSDB: 2h blocks uploaded to S3; compaction: 2h → 6h → 18h → 54h
- Kafka: time-based retention (24–72h); offset-based replay
- S3/Thanos: TSDB blocks keyed by time range; downsample older blocks (5m, 1h resolution)

### Sampling at Ingestion (3 of 6 Systems)
- All high-volume pipelines sample at ingestion to control downstream cost, while preserving 100% of critical signals
- anomaly_detection: three-tier (Z-score all, Prophet top 10%, LSTM top 1%)
- distributed_logging: DEBUG logs sampled at 1% for high-volume services; ERROR/FATAL at 100%; `acks=all` for high-priority topic
- distributed_tracing: head-based 10% (`parentbased_traceidratio=0.1`); tail-based captures 100% of errors + latency > 5s; total ~500K spans/sec (from 4M generated)

## Common Databases

### Elasticsearch 8.x (5 of 6)
- All except metrics_and_monitoring; ILM hot/warm/cold; daily indices; `dynamic: false` mappings; keyword + text types; force-merge on warm tier

### Kafka 3.x (All 6)
- Ingest buffer; priority-based topic design; replication factor=3; min.insync.replicas=2; compression varies by use case

### S3 / MinIO (5 of 6)
- Long-term archival; Parquet for analytics; Object Lock WORM for audit (7-year); Thanos blocks for metrics (1–5 year); cold-tier searchable snapshots for logs

### PostgreSQL (3 of 6)
- alerting: alert history (optional); anomaly_detection: `anomaly_events` + `anomaly_feedback` tables; audit_log: hash chain primary store

### Redis (3 of 6)
- anomaly_detection: LRU model cache for Prophet (60 GB, top 1.215M series); metrics: Thanos Query Frontend sub-query result cache; alerting: dedup cache

## Common Communication Patterns

### Pull vs Push Ingestion Models
- **Pull (Prometheus)**: scrapes `/metrics` endpoint every 15s; automatic service discovery; low throughput (suitable for metrics)
- **Push (Kafka-based)**: Filebeat/Fluent Bit/OTel Collector push to Kafka; handles 2M+ events/sec; decouples producers from consumers

### gRPC for High-Throughput Data Paths
- distributed_tracing: OTLP/gRPC on port 4317 from applications to OTel Collector; batched + snappy compressed
- anomaly_detection: Triton GPU inference server via gRPC; < 5 ms per batch LSTM inference
- metrics: Thanos StoreAPI uses gRPC for fan-out queries

### Webhook → Alertmanager for All Observability Signals
- anomaly_detection → Alertmanager webhook with AnomalyEvent JSON
- distributed_logging → ElastAlert → Alertmanager webhook
- audit_log → Suspicious Activity Detector → Alertmanager webhook
- Alertmanager normalizes, deduplicates, routes all signals consistently

## Common Scalability Techniques

### Deduplication of HA Pairs
- metrics: Thanos Query deduplicates by `replica` label (2 Prometheus instances per cluster scraping same targets)
- alerting: Alertmanager cluster (3-node HA gossip protocol) deduplicates identical alerts by fingerprint
- tracing: OTel Collector horizontal scale + load balancer (no dedup needed; tail sampler buffers per trace)

### Cardinality Control
- metrics: hard limit < 100K series per scrape target; never use user_id/request_id/trace_id as label (explodes cardinality)
- logging: `dynamic: false` prevents new fields from auto-mapping; rejects unexpected fields
- tracing: limit number of attribute keys per span; avoid high-cardinality values in span attributes

### Query Caching and Splitting
- metrics: Thanos Query Frontend splits long range queries into 1-day sub-queries; caches sub-results in memcached; returning queries hit cache
- anomaly: Redis LRU cache for trained Prophet models (60 GB covers top 10% of series); GPU model server for LSTM

### Force-Merge on Warm Tier
- logging + audit + tracing all force-merge Elasticsearch warm-tier indices to 1 segment/shard; eliminates segment merging overhead on reads; reduces CPU usage for repeated queries on aged data

## Common Deep Dive Questions

### How do you prevent Elasticsearch from degrading under high log/trace ingestion rates?
Answer: (1) `refresh_interval: 5s` instead of default 1s — reduces segment flush frequency 5×, batch more writes per segment; (2) `translog.durability: async` + `translog.sync_interval: 5s` — avoids per-document fsync, improves write throughput 3-5×; (3) `dynamic: false` prevents schema-on-write from triggering mapping updates; (4) bulk indexing via Kafka consumers (not individual document writes); (5) separate hot/warm/cold nodes so ingest load doesn't compete with search; (6) ILM rollover at 50 GB/shard to keep shard size in the 30–50 GB optimal range; (7) force-merge warm tier to 1 segment so aged-data queries are fastest.
Present in: distributed_logging_system, audit_log_system, distributed_tracing_system

### How do you maintain end-to-end observability correlation (logs + metrics + traces)?
Answer: Three signal integration points: (1) Every structured log event includes `trace_id` and `span_id` fields (from OpenTelemetry context propagation via `traceparent` header); clicking trace_id in Kibana opens Jaeger timeline; (2) Prometheus exemplars attach trace_id to histogram samples, linking a high-latency bucket to its actual trace; (3) OTel Collector Span Metrics Connector generates Prometheus metrics (request_count, duration_histogram, error_rate) from spans, creating reciprocal links. Cross-signal navigation: Grafana alert → click trace_id → Jaeger gantt chart → click span → Kibana logs filtered by trace_id.
Present in: distributed_logging_system, distributed_tracing_system, metrics_and_monitoring_system

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Ingestion Rate** | 803K dp/s (anomaly); 48.6K ev/s (audit); 2–14M ev/s (logging); 500K spans/s (tracing); 5.2M samples/s (metrics) |
| **Ingestion Latency** | < 30s (audit, logging); < 60s (tracing); < 5 min (anomaly detection); < 30s rule eval (metrics) |
| **Query Latency (recent < 24h)** | < 2s (audit, logging, tracing by ID); < 5s (tracing search); < 500ms (metrics) |
| **Query Latency (historical > 30d)** | < 30s (audit, logging); < 5s (metrics long-term via Thanos) |
| **Availability (ingestion)** | 99.99% (alerting, audit); 99.9% (anomaly, logging, tracing, metrics) |
| **Availability (query)** | 99.5% (logging, tracing, metrics) |
| **Durability** | 11 nines — audit (S3 Object Lock WORM); best-effort DEBUG logs; zero loss for ERROR+ |
| **Retention** | 7 years (audit); 7d hot / 30d warm / 90d cold / 1y delete (logging); 7d traces / 30d sampled; 15d local + 1y S3 (metrics) |
| **False Positive Rate** | < 5% (alerting, anomaly detection over rolling 30d) |
