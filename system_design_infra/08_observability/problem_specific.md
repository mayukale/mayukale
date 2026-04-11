# Problem-Specific Details — Observability (08_observability)

---

## alerting_and_on_call_system

### Unique Stack
- Prometheus (metric-based alert rules, PromQL expressions, evaluated every 15s)
- ElastAlert (log-based alert rules, queries Elasticsearch every 60s)
- Blackbox Exporter (external probes: HTTP, TCP, ICMP, DNS; scraped by Prometheus every 30s)
- Alertmanager cluster (3-node HA via gossip protocol) — central routing, deduplication, grouping, inhibition, silencing
- PagerDuty (phone/SMS/push delivery, escalation policies, on-call schedule management)
- Alert history store: Elasticsearch index `alerts-YYYY-MM` with monthly rollover

### Key Algorithms / Design Decisions

**Multi-Window Multi-Burn-Rate SLO Alerting (Google SRE methodology):**
```
For 99.9% SLO over 30 days:
  Error budget = 0.1% × 30 days = 43.2 min of downtime
  Burn rate 14.4× → consumes entire 30-day budget in 2 days

Recording rules (pre-compute error ratios):
  slo:http_error_ratio:rate5m
  slo:http_error_ratio:rate1h
  slo:http_error_ratio:rate6h
  slo:http_error_ratio:rate1d
  slo:http_error_ratio:rate3d

Alert rules:
  # 14.4× burn rate (critical: 2% budget / 1 hour)
  SLOHighBurnRate_Critical:
    rate1h > (14.4 × 0.001) AND rate5m > (14.4 × 0.001)  for: 2m

  # 6× burn rate (warning)
  SLOHighBurnRate_Warning:
    rate6h > (6 × 0.001) AND rate30m > (6 × 0.001)  for: 5m

  # 3× burn rate (medium)
  SLOMediumBurnRate:
    rate1d > (3 × 0.001) AND rate2h > (3 × 0.001)  for: 15m

  # 1× burn rate (slow, ticket)
  SLOSlowBurnRate:
    rate3d > (1 × 0.001) AND rate6h > (1 × 0.001)  for: 30m

Two windows: long window detects sustained issues; short window confirms still active → eliminates false positives from transient spikes
```

**Alertmanager Configuration:**
```yaml
resolve_timeout: 5m
route:
  group_by: ['alertname', 'cluster', 'service']
  group_wait: 30s        # wait before sending first notification for new group
  group_interval: 5m     # wait before sending updates to group
  repeat_interval: 4h    # resend if still firing
  receiver: default
```

**Alert Quality Metrics (tracked monthly per team):**
- Actionability rate: > 80% (alerts requiring human action / total)
- False positive rate: < 5% (auto-resolved within 5 min / total)
- MTTA (Mean Time to Acknowledge): < 5 min for critical
- MTTR (Mean Time to Resolve): < 30 min for critical
- Alerts per on-call shift: < 10
- Repeat offender rate: < 20% (same alertname firing > 5×/month)

### Key Data Structure
```
Alert Event (Elasticsearch `alerts-YYYY-MM`):
  alert_id UUID
  alertname, service, cluster, severity, team
  state: firing | resolved
  fired_at, resolved_at, acknowledged_at, acknowledged_by
  value: DECIMAL  (metric value at fire time)
  summary, runbook_url
  pagerduty_incident_id
  notifications_sent: [{channel, at, to}]
```

### NFRs
- 10,000 alert rules (200 clusters × 50 rules/cluster); 667 evals/sec
- Active firing alerts: 200 steady state; 500–5,000 during incident
- 500 on-call rotations (100 teams × 5 rotations avg)
- Alert evaluation → notification: < 30s; alert → phone call: < 60s
- Alert delivery reliability: 99.99%
- False positive rate: < 5%
- Alert storm handling: 1,000+ alerts → < 10 notifications (grouping)

---

## anomaly_detection_system

### Unique Stack
- Apache Flink (real-time scoring engine, 50 TaskManagers × 4 CPU / 8 GB RAM)
- Tiered ML: Z-score/EWMA (all 12.15M series), Prophet (top 10% = 1.215M), LSTM (top 1% = 121.5K)
- NVIDIA Triton Inference Server for LSTM batch inference (GPU, V100 16 GB, < 5 ms latency)
- MLflow for model registry; Redis LRU for Prophet model cache (60 GB)
- DBSCAN correlation engine for clustering co-occurring anomalies
- CUSUM for drift detection per series; circuit breaker if FP rate > 50%
- Prometheus + Thanos feeding Kafka topic `metrics-raw` at 803K dp/sec

### Key Algorithms / Design Decisions

**Streaming Z-Score with EWMA (Flink, Tier 1 — all 12.15M series):**
```python
class StreamingZScore:
    def __init__(self, alpha=0.05, threshold=4.0):
        self.alpha = alpha    # EWMA decay factor
        self.threshold = threshold

    def update(self, value):
        delta = value - self.mean
        self.mean += self.alpha * delta
        self.variance = (1 - self.alpha) * (self.variance + self.alpha * delta**2)

        z = abs(delta) / (self.variance ** 0.5)
        confidence = 1.0 / (1.0 + math.exp(-(z - self.threshold)))  # sigmoid
        return z, confidence
```

**Prophet Training (Tier 2 — top 10% = 1.215M series):**
```python
m = Prophet(
    changepoint_prior_scale=0.05,
    seasonality_prior_scale=10.0,
    seasonality_mode='multiplicative',
    yearly_seasonality=True,
    weekly_seasonality=True,
    daily_seasonality=True,
    interval_width=0.99,   # 99% CI for anomaly bounds
)
m.add_seasonality(name='4h_batch', period=1.0/6, fourier_order=5)

# Scoring: if value outside [yhat_lower, yhat_upper]:
# deviation = (value - hi) / max(hi - yhat, 1e-6)
# anomaly_score = min(1.0, deviation / 2.0)
```

**LSTM Architecture (Tier 3 — top 1% = 121.5K series):**
```python
class InfraLSTM(nn.Module):
    """Input: 60-point sliding window (15 min @ 15s)
       Output: predicted next value as N(mean, std²)"""
    def __init__(self, hidden=64, layers=2, dropout=0.2):
        super().__init__()
        self.lstm = nn.LSTM(1, hidden, layers, dropout=dropout, batch_first=True)
        self.fc_mean    = nn.Linear(hidden, 1)
        self.fc_log_std = nn.Linear(hidden, 1)

    def forward(self, x):
        out, _ = self.lstm(x)
        h = out[:, -1, :]
        mean = self.fc_mean(h)
        std  = torch.exp(self.fc_log_std(h)).clamp(min=1e-6)
        return mean, std
# Training: 80/20 split, 90-day history, Huber loss, ~2h/model on V100
# Inference: < 1ms batch GPU via Triton
```

**DBSCAN Temporal Correlation (5-min window):**
```python
features = [
    [e['detected_at'].timestamp() / 300,          # time bucket
     hash(e['labels'].get('cluster')) % 1000 / 1000.0,
     hash(e['labels'].get('service')) % 1000 / 1000.0]
    for e in events
]
# eps=0.3 → anomalies within ~90s and same cluster/service are neighbors
# min_samples=2 → need ≥ 2 to form cluster
labels = DBSCAN(eps=0.3, min_samples=2).fit(features).labels_
```

**Root Cause Ranking:**
```
composite = (0.4 × time_score)      # earliest anomaly = higher score
          + (0.4 × causal_score)    # more downstream deps = higher score
          + (0.2 × anomaly_score)   # higher ML score = higher score
```

**Model Staleness (CUSUM drift detection per series):**
- `s_pos = max(0, s_pos + (value - predicted) - k)` — detects upward drift
- Retrain triggered when CUSUM threshold exceeded
- Circuit breaker: if last-100-feedbacks FP rate > 50%, disable model and fall back to Z-score

### Key Tables
```sql
CREATE TABLE anomaly_events (
    anomaly_id TEXT PRIMARY KEY,  -- e.g., anom-2026-04-10-14-23-45-bm-rack03-cpu
    series_key TEXT NOT NULL,
    detected_at TIMESTAMPTZ,
    severity TEXT,
    anomaly_score FLOAT,
    confidence FLOAT,
    method TEXT,                  -- zscore, prophet, lstm
    event_json JSONB,             -- full anomaly event including correlated_anomalies
    resolved_at TIMESTAMPTZ,
    incident_id TEXT,
    INDEX (series_key, detected_at DESC),
    INDEX (severity, detected_at DESC),
    INDEX (resolved_at) WHERE resolved_at IS NULL
);

CREATE TABLE anomaly_feedback (
    feedback_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    anomaly_id TEXT REFERENCES anomaly_events,
    feedback_type TEXT CHECK (feedback_type IN ('true_positive','false_positive','informational')),
    feedback_by TEXT,
    feedback_at TIMESTAMPTZ DEFAULT NOW(),
    notes TEXT,
    retraining_triggered BOOLEAN DEFAULT FALSE
);
```

### NFRs
- 12.15M time series total; 803K data points/sec
- Scale: 2M node metrics, 5M K8s pod metrics, 5M app metrics, 50K DB metrics, 100K network metrics
- Model storage: Z-score 486 MB; Prophet 60 GB (Redis LRU); LSTM 607 GB (GPU memory)
- Detection latency: < 5 minutes; false positive rate: < 5%; false negative rate: < 2%
- Retraining: nightly 02:00 UTC (50 CPU nodes + 20 GPU V100 nodes); model staleness ≤ 7 days
- Availability: 99.9% scoring; 99% training

---

## audit_log_system

### Unique Stack
- Kafka dedicated audit cluster: topic `audit.events`, 128 partitions, RF=3, `acks=all`, `min.insync.replicas=2`, zstd compression, 72h retention
- Hash Chain Service: append-only chain per sequence; `C(n) = SHA-256(H(n) || C(n-1))`; anchored every 10K events to RFC 3161 timestamping service
- S3 Object Lock WORM (Write Once Read Many): 7-year immutable cold archive in Parquet
- Elasticsearch: daily index `audit-YYYY-MM-DD`, 6 primary shards, 2 replicas; ILM hot 30d → warm 1y → delete (S3 persists 7y)
- Suspicious Activity Detector: rule-based real-time rules on audit stream

### Key Algorithms / Design Decisions

**Tamper-Evident Hash Chain:**
```java
// Append event to chain
String eventHash = SHA256(eventJson)
String chainHash = SHA256(eventHash + previousChainHash)

// Anchor every 10,000 events to RFC 3161 timestamping service
// RFC 3161 provides cryptographic proof of "this hash existed at this time"

// Verification: re-compute C(n) = SHA-256(H(n) || C(n-1)) for range [from, to]
// Any inserted, deleted, or modified event breaks the chain → detected
```

**Suspicious Activity Detection Rules:**
```
- Privilege escalation: user gains admin role  → CRITICAL
- Bulk delete: > 100 resources deleted in 1 hour → CRITICAL
- Off-hours admin access: privileged actions 11PM–6AM → WARNING
- Repeated auth failures: > 10 failures from same IP in 5 min → WARNING
- Unauthorized access attempts: > 5 consecutive 403 responses → WARNING
- First-time access from new IP/location → INFO (enrich PagerDuty context)
```

**Audit Event Schema (key fields):**
```json
{
  "event_id": "aud-2026-04-09-14-20-001-a3f2",
  "event_hash": "sha256:a1b2c3d4...",
  "chain_hash": "sha256:f6e5d4c3...",
  "chain_sequence": 1923847562,
  "timestamp": "2026-04-09T14:20:15.123Z",
  "actor": {
    "type": "user|service",
    "id": "jsmith@company.com",
    "roles": ["platform-engineer","k8s-admin"],
    "auth_method": "sso_oidc",
    "mfa_used": true,
    "source_ip": "10.0.5.42"
  },
  "action": {
    "type": "CREATE|READ|UPDATE|DELETE",
    "operation": "scale_deployment",
    "api_endpoint": "PATCH /apis/apps/v1/namespaces/scheduling/deployments/job-scheduler"
  },
  "resource": {
    "type": "kubernetes.deployment",
    "id": "scheduling/job-scheduler",
    "cluster": "k8s-prod-us-east-1"
  },
  "context": {
    "change_ticket": "CHG-2026-04-09-001",
    "approval_id": "APR-789"
  },
  "result": {
    "outcome": "success|failure",
    "previous_state": {"replicas": 5},
    "new_state": {"replicas": 10},
    "diff": {"replicas": "5 → 10"}
  }
}
```

### NFRs
- 48,600 events/sec; 97 MB/s ingestion bandwidth
- Raw: 8.4 TB/day → 2.8 TB/day compressed (3:1)
- Hot storage (ES, 30d, 2 replicas): 168 TB SSD; warm (1y, HDD): 1 PB; cold (S3, 7y): 7.2 PB
- Hash chain metadata: 54 TB/year (100 bytes × 48.6K/s × 86.4K × 365)
- Kafka buffer: 25 TB (3 replicas × 72h)
- Event → immutable store: < 30s; event → searchable: < 60s
- Query latency: < 2s (< 30d); < 30s (historical > 30d)
- Ingestion availability: 99.99%; durability: 99.999999999% (11 nines)

---

## distributed_logging_system

### Unique Stack
- Collectors: Filebeat (tails files on bare-metal), Fluent Bit DaemonSet (K8s stdout/stderr), Fluentd
- Collectors add metadata (host, cluster, namespace, pod, container), compress, buffer to disk before Kafka
- Kafka: topic `logs.raw` (acks=1, lz4, 48h, 512+ partitions); `logs.high-priority` (acks=all for ERROR/FATAL); `logs.dlq`
- Log processor: Logstash/Vector/Flink — parse, enrich (team/owner from service registry), normalize timestamps, sample DEBUG at 1%, route by level
- Elasticsearch: hot 7d SSD → warm 30d HDD → cold 90d searchable snapshot → delete 1y
- Kibana for dashboards, saved queries, log-based alerting via ElastAlert

### Key Algorithms / Design Decisions

**Elasticsearch Index Template (logging):**
```json
{
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
    "properties": {
      "@timestamp":        {"type": "date"},
      "level":             {"type": "keyword"},
      "service":           {"type": "keyword"},
      "host":              {"type": "keyword"},
      "cluster":           {"type": "keyword"},
      "namespace":         {"type": "keyword"},
      "pod":               {"type": "keyword"},
      "trace_id":          {"type": "keyword"},
      "span_id":           {"type": "keyword"},
      "message":           {"type": "text", "analyzer": "standard"},
      "error.type":        {"type": "keyword"},
      "error.stack_trace": {"type": "text", "index": false},
      "team":              {"type": "keyword"}
    }
  }
}
```

**Key design choices:**
- `refresh_interval: 5s` (not default 1s) → reduces segment flush 5×, more writes per segment
- `translog.durability: async` → avoids per-document fsync; 3–5× write throughput improvement
- `dynamic: false` → no schema-on-write mapping updates; rejects unexpected fields
- `index: false` on `error.stack_trace` → stored in `_source` for display but not indexed for search (too large)
- Force-merge warm tier to 1 segment/shard → eliminates merge overhead on aged data reads

### Log Event Schema
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
  "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
  "span_id": "00f067aa0ba902b7",
  "message": "Failed to allocate GPU resources for job j-98234",
  "error.type": "ResourceExhaustedException",
  "team": "ml-training",
  "environment": "production"
}
```

### NFRs
- Sources: 50K hosts + 500K pods + 10K infra daemons = 560K sources
- Steady-state: 2.8M events/sec at 500 bytes avg = 1.4 GB/s; peak (5×): 14M events/sec = 7 GB/s
- After 1% DEBUG sampling: 60 TB/day stored
- Hot (SSD, 7d, 1 replica): 840 TB; warm (HDD, 30d): 3.6 PB; cold (object, 90d): 5.9 PB
- Kafka buffer (48h, 3 replicas): 360 TB
- Ingestion availability: 99.9%; query availability: 99.5%
- Log written → searchable: < 30s p99; query latency (hot tier): < 2s p95

---

## distributed_tracing_system

### Unique Stack
- OpenTelemetry SDK: Java agent (auto-instrument), Python SDK (manual + auto), Go SDK (manual)
- W3C TraceContext: `traceparent: 00-{traceId}-{spanId}-{flags}` on HTTP/gRPC; manual propagation via Kafka message headers
- OTel Collector: receivers=OTLP gRPC:4317; processors=batch(10s/8192)+tail_sampling+attributes+span_metrics; exporters=Kafka+Jaeger+Prometheus
- Kafka: topic `traces.spans`, 256 partitions, **keyed by trace_id** (ensures all spans of a trace go to same partition), 24h retention
- Tail Sampling Service: 30s decision window, 100K trace buffer, policy evaluation
- Jaeger: ES-backed (index `jaeger-span-YYYY-MM-DD`, 10 primary shards); query UI with Gantt chart + service dependency graph

### Key Algorithms / Design Decisions

**Head-Based Sampling (at OTel SDK):**
```yaml
sampler: parentbased_traceidratio
arg: 0.1   # 10% base rate

# parentbased_traceidratio:
# - If parent has sampling decision in traceparent.flags → follow parent's decision (consistent trace)
# - If root span → hash traceId; sample if hash < 0.1 × MAX_HASH
# traceparent: 00-{traceId}-{spanId}-01   (01 = sampled; 00 = not sampled)
```

**Tail-Based Sampling Policies (OTel Collector):**
```yaml
tail_sampling:
  decision_wait: 30s        # wait for trace to complete before deciding
  num_traces: 100000        # max traces in decision buffer (in-memory)
  policies:
    - name: error-policy
      type: status_code
      status_code: {status_codes: [ERROR]}      # always sample errors (100%)

    - name: latency-policy
      type: latency
      latency: {threshold_ms: 5000}             # always sample traces > 5s (100%)

    - name: probabilistic-policy
      type: probabilistic
      probabilistic: {sampling_percentage: 1}   # 1% of everything else

    - name: critical-operations
      type: string_attribute
      string_attribute:
        key: http.url
        values: ["/api/v1/jobs", "/api/v1/resources/allocate"]
        enabled_regex_matching: true             # always sample critical ops
```

**Span Schema (OpenTelemetry):**
```json
{
  "traceId": "4bf92f3577b34da6a3ce929d0e0e4736",
  "spanId": "00f067aa0ba902b7",
  "parentSpanId": "a3ce929d0e0e4736",
  "name": "POST /api/v1/jobs",
  "kind": "SPAN_KIND_SERVER",
  "startTimeUnixNano": 1712678625123000000,
  "endTimeUnixNano": 1712678625456000000,
  "attributes": [
    {"key": "service.name",      "value": {"stringValue": "job-scheduler"}},
    {"key": "http.method",       "value": {"stringValue": "POST"}},
    {"key": "http.status_code",  "value": {"intValue": 200}},
    {"key": "k8s.namespace.name","value": {"stringValue": "scheduling"}}
  ],
  "status": {"code": "STATUS_CODE_OK"}
}
```

**Trace Anatomy Example:**
```
Trace: 4bf92f3577b34da6a3ce929d0e0e4736 (200ms total)
├── [api-gateway]      GET /submit-job            (200ms)
│   ├── [auth-service] ValidateToken              (15ms)
│   └── [job-scheduler] POST /api/v1/jobs         (180ms)
│       ├── [job-scheduler]    ValidateJobSpec     (5ms)
│       ├── [resource-manager] AllocateResources   (120ms)
│       │   ├── [resource-manager] CheckQuota      (10ms)
│       │   └── [resource-manager] ReserveGPU      (100ms)
│       │       └── [mysql] SELECT FROM pool        (8ms)
│       └── [kafka-producer]   Produce job-events  (10ms)
```

### NFRs
- Generated: 500K requests/sec × 8 avg spans = 4M spans/sec
- After sampling: 400K head-based + 100K tail-based = 500K spans/sec ingested
- 43.2B spans/day → 14.3 TB/day after 3:1 compression
- 7-day full retention: 200 TB (2 replicas); 30-day sampled 1%: 4.3 TB
- Kafka buffer (24h, 3 replicas): 43 TB
- Span ingestion → searchable: < 60s; query by trace ID: < 2s; search by service+time: < 5s
- Ingestion availability: 99.9%; query availability: 99.5%

---

## metrics_and_monitoring_system

### Unique Stack
- Prometheus (per cluster): HA pairs (2 replicas), 15s scrape, local TSDB (15d retention), PromQL rule engine
- Thanos Sidecar: uploads completed 2h blocks to S3; exposes StoreAPI for live data
- Thanos Store Gateway: loads S3 block metadata into memory; serves historical data via StoreAPI
- Thanos Compactor (singleton): merges blocks; downsamples to 5m and 1h; enforces retention
- Thanos Query: fan-out to Sidecars + Store Gateways; deduplicates HA pairs via `replica` label
- Thanos Query Frontend: splits long queries into 1-day sub-queries; caches in memcached
- Grafana: dashboards-as-code (Grafonnet/Terraform); template variables ($cluster, $namespace, $service)
- Pushgateway for batch jobs and short-lived processes

### Key Algorithms / Design Decisions

**Gorilla-Style TSDB Compression (1–2 bytes/sample):**
```
Timestamps (delta-of-delta encoding):
  Regular 15s scrape → delta = 15, delta-of-delta = 0 → 1 bit per sample
  Irregular scrapes → encode actual delta-of-delta with variable-length encoding

Values (XOR with previous):
  Similar consecutive values (e.g., memory total) → many leading zero bits → compress well
  Rapidly changing values → more bits but still better than raw float64

Result: average 1-2 bytes/sample vs 16 bytes uncompressed (8B timestamp + 8B float64)
```

**TSDB Write + Query Path:**
```
WRITE:
  Sample → WAL (sequential append, O(1), fast fsync)
         → Head Block (in-memory, last 2h, all series)
  Every 2h: Head Block cut → new Head Block; old serialized to disk as immutable Block
  Thanos Sidecar uploads completed Block to S3

QUERY:
  Parse PromQL → find series via inverted index (label → posting list → series IDs)
  For each series: load chunks from Head Block (memory) or persisted Blocks (disk)
  Decode XOR/delta-of-delta → apply PromQL functions → return result
```

**TSDB Memory Model:**
```
~3 KB per series in head block (labels + chunks + index entry)
5M series (per cluster): 5M × 3 KB = 15 GB
Rule of thumb: 1 GB RAM per ~300K active series
Recommended: 32 GB for 5M series; 64 GB for 10M series
```

**Block Compaction:**
```
2h blocks → 6h → 18h → 54h (Prometheus local)
S3 (Thanos Compactor):
  Vertical compaction: merge overlapping blocks (HA dedup)
  Horizontal compaction: merge adjacent blocks
  Downsampling:
    raw 15s   → 1 year S3
    5m avg    → 3 year S3  (reduce by 20×)
    1h avg    → 5 year S3  (reduce by 240×)
```

**Key PromQL Patterns:**
```promql
# CPU utilization per host
100 - (avg by (instance) (rate(node_cpu_seconds_total{mode="idle"}[5m])) * 100)

# Memory utilization
1 - (node_memory_MemAvailable_bytes / node_memory_MemTotal_bytes)

# HTTP p99 latency
histogram_quantile(0.99,
  sum by (le, service) (rate(http_request_duration_seconds_bucket[5m])))

# Disk fill prediction (linear extrapolation)
predict_linear(node_filesystem_avail_bytes{mountpoint="/"}[6h], 4*3600) < 0

# Pod restart rate
increase(kube_pod_container_status_restarts_total[1h])

# Kafka consumer lag
kafka_consumergroup_lag_sum{consumergroup="cg-logstash-elasticsearch"}
```

**histogram_quantile internals:**
```
http_request_duration_seconds_bucket{le="0.1"}  15678
http_request_duration_seconds_bucket{le="0.25"} 17234
http_request_duration_seconds_bucket{le="+Inf"} 18100

histogram_quantile(0.99, rate(...[5m])):
1. rate() → per-second increase in each bucket counter
2. Find bucket where cumulative count crosses 99% of total
3. Linear interpolate within that bucket for exact value
```

### NFRs
- 78.5M active time series total; 5.2M samples/sec; 10.4M samples/sec peak
- Breakdown: 25M node, 50M K8s pod, 3M infra service, 500K app metrics
- Memory: ~3 KB/series; 32 GB for 5M series (per Prometheus instance)
- Local retention: 10 TB per cluster (15d); Thanos S3: 246 TB raw (1y) + 37 TB downsampled (3y)
- Query latency: instant < 50ms; 1h range < 200ms; 7d range < 1s; dashboard (20 panels) < 3s; long-term (1y) < 5s
- Scrape interval: 15s default; 5s for critical services
- Cardinality limit: < 100K series per scrape target
- Ingestion availability: 99.9%; query availability: 99.5%
