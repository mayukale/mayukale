# Infra Pattern 8: Observability — Interview Study Guide

Reading Infra Pattern 8: Observability — 6 problems, 7 shared components

---

## STEP 1 — ORIENTATION

This guide covers six related but distinct system design problems that all fall under the **Observability** umbrella:

1. **Alerting and On-Call System** — routing signals to humans, on-call scheduling, alert fatigue
2. **Anomaly Detection System** — ML-based dynamic thresholds replacing static alerts
3. **Audit Log System** — immutable, compliance-grade tamper-evident event trail
4. **Distributed Logging System** — centralized log ingestion, search, and retention
5. **Distributed Tracing System** — end-to-end request flow across microservices
6. **Metrics and Monitoring System** — time-series collection, storage, and querying

The **three pillars of observability** — metrics, logs, and traces — each have a dedicated problem. Alerting, anomaly detection, and audit logging are the operational and compliance layers built on top. In a real interview you will be asked one of these six, but an interviewer may probe any of the others as follow-ups. Understanding the shared plumbing lets you answer those probes fluently.

**Scale anchors** (memorize these — they will ground every estimation section):
- Fleet: 50,000 bare-metal hosts, 200 Kubernetes clusters, 500,000+ pods, 5,000 services
- Metrics: 78.5M active time series, 5.2M samples/sec
- Logs: 2.8M events/sec steady state, 14M/sec peak (5x burst)
- Traces: 4M spans/sec generated, 500K/sec after sampling
- Audit events: 48,600 events/sec
- Anomaly series: 12.15M time series being scored

---

## STEP 2 — MENTAL MODELS

### The Core Idea

Observability is the ability to understand the internal state of a system from its external outputs. The hard part is not generating data — every service produces torrents of it. The hard part is **making that data useful under pressure**: fast enough to debug a live incident, complete enough to satisfy a compliance audit, intelligent enough to page the right person without crying wolf.

### The Right Analogy

Think of a hospital's patient monitoring system. Sensors on patients (metrics exporters) continuously measure vitals (CPU, memory, latency). Nurses watch a central monitor (Grafana dashboards). When vitals cross a threshold the nurse station alarm fires (Alertmanager → PagerDuty). A specialist reviews the patient's full history (logs and traces) to diagnose the root cause. The hospital keeps immutable records of every intervention for liability purposes (audit log). And a research team watches population-wide patterns to catch emerging trends before they become crises (anomaly detection).

The sensor data is useless without the routing. The alarms are dangerous without the context. The context is worthless if it is not searchable. All six systems are load-bearing.

### Why It Is Hard

Three dimensions make observability genuinely difficult at scale:

**Volume.** You are dealing with 5–14 million log events per second, 5+ million metric samples per second, 500K+ spans per second, and 50K audit events per second — all arriving simultaneously, all needing to be durable, searchable, and low-latency. No single system handles all of this; the design is always a multi-tier pipeline.

**Correlation across signals.** An incident manifests as a CPU spike in metrics, a stack trace in logs, a slow span in traces, and a cascade of related anomalies — all at the same time on different systems. The trace ID is the thread that stitches them together. Getting W3C TraceContext propagated correctly through HTTP, gRPC, and Kafka headers is a coordination problem spanning every service team.

**The alert quality paradox.** Sensitive alerting catches real problems early but produces false positives that erode on-call trust. Conservative alerting reduces noise but misses real incidents. The solution — multi-window multi-burn-rate SLO alerting, ML-based dynamic thresholds, inhibition trees, and rigorous quality metrics — requires deliberate, ongoing engineering rather than a one-time design decision.

---

## STEP 3 — INTERVIEW FRAMEWORK

This section gives you the end-to-end playbook for any of the six problems. The examples below focus on the generic observability framing and call out problem-specific differences.

---

### 3a. Clarifying Questions

Always open with these four or five questions. They signal that you think before designing and they expose the constraints that drive most of your subsequent decisions.

**Question 1: What scale are we targeting — how many sources, events per second, services?**
What changes: 100 services with 10K events/sec means a single Kafka cluster and one Elasticsearch node. 50,000 hosts with 2.8M events/sec means dedicated Kafka clusters, 100+ Elasticsearch data nodes, and tiered storage. The architecture changes completely.

**Question 2: What are the durability and availability requirements? Is data loss acceptable?**
What changes: Audit logs require 11 nines durability (S3 Object Lock, `acks=all` Kafka, hash chains). Application DEBUG logs can tolerate loss (`acks=1`). This distinction drives Kafka producer configuration, replication factors, and storage choices more than almost anything else.

**Question 3: What are the retention requirements and any compliance obligations?**
What changes: Seven-year retention with tamper evidence (audit logs for SOC2/PCI-DSS) requires an entirely different pipeline — WORM storage, hash chains, separate Kafka cluster — compared to seven-day rolling retention for traces or one-year for metrics.

**Question 4: Do we need real-time alerting on this data, or just storage and search?**
What changes: Read-only archival systems can be simpler (write to S3, query with Athena). Real-time alerting adds the Alertmanager integration layer, ElastAlert workers, or a streaming anomaly detection engine — significant additional complexity.

**Question 5 (for alerting/anomaly): What signals already exist? Do we have metrics, logs, and traces already flowing somewhere?**
What changes: If Prometheus already runs per cluster, the alerting system is largely Alertmanager configuration. If there is nothing, you are building ingestion pipelines first.

**Red flags to watch for:**
- An interviewer saying "assume infinite budget" without clarifying scale — push back and anchor on numbers anyway. Your architecture cannot be credible without them.
- Assuming real-time queries over multi-year retention — that is not feasible on a single tier; you must tier storage.
- Ignoring the "who monitors the monitors" question for alerting — the dead man's switch is a senior-level detail that separates candidates.

---

### 3b. Functional Requirements

State these explicitly before designing. Do not try to satisfy unstated requirements.

**Core requirements for any observability problem:**
- Ingest data from all relevant sources at the stated scale
- Store data with the specified durability and retention
- Support search/query with stated latency (typically sub-2s for hot tier, sub-30s for historical)
- Provide a human interface (Grafana/Kibana/Jaeger UI)

**Scope decisions to make explicit:**
- "This system handles operational observability, not business analytics" — keeps you from being pulled into data warehouse territory
- "Alerting policy management is out of scope, but alerting routing is in scope" — for alert systems
- "Application-level business audit (user activity tracking in SaaS products) is out of scope" — for audit systems
- "Code-level profiling and flame graphs are out of scope" — for tracing systems

**Clear statement of what you are building:**
For a logging system: "A centralized log ingestion, storage, and search platform that accepts structured logs from 560,000 sources at up to 2.8M events/sec, stores them with hot-warm-cold tiering (7d/30d/90d), and serves sub-2-second search queries with 99.9% ingestion availability."

---

### 3c. Non-Functional Requirements

These are derived, not invented. Each NFR should be justified by a real consequence.

| NFR | Derived From | Key Trade-off |
|---|---|---|
| **Ingestion availability 99.99%** | A missed audit event is a compliance violation; a missed critical alert means an undetected outage | ✅ High reliability; ❌ requires expensive HA Kafka and collector redundancy |
| **End-to-end latency < 30s** | Engineers need to see logs/metrics within 30 seconds to debug a live incident | ✅ Fast response; ❌ limits batching size, increases write amplification |
| **Durability 11 nines (audit)** | Regulatory frameworks require proof that audit records were never modified | ✅ Compliance-proof; ❌ S3 Object Lock, hash chains, separate pipeline — expensive |
| **False positive rate < 5% (alerting)** | Alert fatigue causes on-call to ignore genuine critical pages | ✅ On-call trust; ❌ might miss some real alerts, requires ongoing quality work |
| **Cardinality limit 100K series/target** | Prometheus TSDB memory: at 3 KB/series, 100K series = 300 MB manageable; 1M series = 3 GB per Prometheus per target | ✅ Stable memory; ❌ engineers cannot put user_id as a metric label |
| **Sampling (tracing, anomaly)** | Full capture of 4M spans/sec = 350 TB/day — not economically viable | ✅ Cost-effective; ❌ some traces are not captured; must guarantee 100% errors |

**The fundamental trade-off pair in observability:** completeness vs. cost. Every design decision about sampling, tiering, compression, and retention is fundamentally this trade-off. Make it explicit.

---

### 3d. Capacity Estimation

Approach: derive → anchor → imply architecture.

**For logging (most common):**
```
Sources: 50K hosts + 500K pods + 10K daemons = 560K sources
Rate:    560K × 5 events/sec = 2.8M events/sec (steady state)
Peak:    2.8M × 5 (burst factor) = 14M events/sec
Size:    500 bytes per event (structured JSON)
BW:      2.8M × 500B = 1.4 GB/s ingress
Daily:   1.4 GB/s × 86,400 = ~121 TB/day raw
After 1% DEBUG sampling: ~60 TB/day stored
```

Time to work through: 3–4 minutes. Say the numbers aloud as you write.

**Architecture implications to call out explicitly:**
- 60 TB/day at 7-day hot retention → 840 TB SSD required → justifies multi-tier
- 1.4 GB/s ingress → requires Kafka with 256+ partitions and 100+ Logstash workers (not a single-node system)
- 60 TB/day on SSD is ~$60K/month in cloud — cold tier saves 80% of that cost

**For metrics:**
```
Series: 50K hosts × 500 + 500K pods × 200 + overhead = ~78.5M series
Rate:   78.5M / 15s = 5.2M samples/sec
Size:   1.5 bytes/sample (Prometheus TSDB XOR compression)
Daily:  5.2M × 1.5B × 86,400 = ~674 GB/day per cluster-level aggregation
Memory: 78.5M × 3 KB/series = ~235 GB RAM across all Prometheus instances
```

**For tracing:**
```
Services: 5,000 × avg 10 instances = 50,000 service instances
RPS:      500,000 requests/sec × 8 spans/trace = 4M spans/sec generated
After 10% head sampling: 400K/sec; +100K tail-captured errors = 500K/sec ingested
Size:     1 KB/span
BW:       500 MB/s ingress
Daily:    500M spans × 1KB = ~43 TB/day, ~14 TB compressed
```

**For audit:**
```
K8s API writes: 200 clusters × 1,000 rps × 20% writes = 40,000 events/sec
Auth, infra, DB: ~8,600/sec
Total: ~48,600 events/sec at 2 KB avg = 97 MB/s
Daily: ~8.4 TB raw, ~2.8 TB compressed
7 years: ~7.2 PB cold (S3 Object Lock)
```

---

### 3e. High-Level Design

Draw in this order when whiteboarding (it tells a coherent story):

1. **Sources** (top-left): what generates data — exporters, collectors, SDKs, K8s API server, audit webhooks
2. **Kafka** (middle): the universal buffer and fan-out point — always present in all six systems
3. **Processing layer** (below Kafka): Logstash/Vector for logs, OTel Collector for traces, Flink for anomaly scoring, Hash Chain Service for audit
4. **Storage** (right): hot SSD tier (Elasticsearch or Prometheus TSDB), warm HDD, cold S3
5. **Query layer** (bottom): Grafana, Kibana, Jaeger UI, API endpoints
6. **Alerting integration** (far right): Alertmanager with routes to PagerDuty and Slack

**Four key decisions to justify, not just state:**

**Decision 1: Kafka as the buffer.** Decouples producers from consumers, absorbs burst traffic (14M events/sec peak vs 2.8M steady state), enables replay for reprocessing when you deploy a new Logstash pipeline, and supports multiple consumer groups (Elasticsearch, S3 archive, ElastAlert, SIEM). Without Kafka, a producer surge would cascade directly to Elasticsearch, causing write rejections and data loss.

**Decision 2: Elasticsearch for hot search, S3 for cold archival.** Elasticsearch's inverted index enables sub-second full-text search across billions of log events. S3 is 50–100× cheaper per GB. ILM (Index Lifecycle Management) automates the transition: hot (SSD, 7d) → warm (HDD, 30d, force-merged) → cold (searchable snapshot on S3, 90d) → delete. Without tiering, the storage cost would be prohibitive.

**Decision 3: Sampling is mandatory for traces, logs, and anomaly scoring.** You cannot store 100% of everything. The design must be explicit about what gets sampled (DEBUG logs at 1%, trace spans at 10% head-based), what must never be sampled (ERROR logs, error traces, audit events), and what determines the sampling decision (head-based randomness vs. tail-based completeness for anomaly detection).

**Decision 4: Thanos for global metrics.** Each cluster runs a Prometheus pair (HA). Thanos Sidecar uploads 2h blocks to S3. Thanos Query fans out to all Sidecars + Store Gateways and deduplicates the HA pair results. This gives you a single PromQL endpoint covering all clusters and all history without running a monolithic Prometheus — which would not fit in memory at 78.5M series.

---

### 3f. Deep Dive Areas

These are the three most frequently probed areas. Have a crisp answer for each.

#### Deep Dive 1: Multi-Window Multi-Burn-Rate SLO Alerting

**The problem:** Simple threshold alerting generates two failure modes. Too tight a threshold fires on transient 30-second spikes, training engineers to ignore pages. Too loose a threshold misses slow degradation that erodes the error budget over days. Neither aligns with actual business impact.

**The solution:** Burn rate alerting derived from Google SRE methodology.

Given a 99.9% SLO over 30 days:
- Error budget = 0.1% × 30 days = 43.2 minutes of allowed downtime
- A burn rate of 14.4× means consuming the entire 30-day budget in 2 days — page immediately
- A burn rate of 1× means consuming it at the expected pace — no page needed

The **multi-window** trick: require both a long window (1h) AND a short window (5m) to exceed the burn rate threshold before firing. The long window catches sustained issues. The short window confirms the error is happening right now, not just leftover history from a resolved spike 50 minutes ago.

```
Burn Rate  Long Window  Short Window  Severity  Budget Consumed
  14.4×       1 hour       5 minutes   Critical  2% in 1 hour
  6×          6 hours      30 minutes  Critical  5% in 6 hours
  3×          1 day        2 hours     Warning   10% in 1 day
  1×          3 days       6 hours     Info      10% in 3 days
```

**Trade-offs to call out unprompted:**
- ✅ Directly tied to business impact (SLO budget), not arbitrary thresholds
- ✅ Dual-window eliminates almost all false positives from transient spikes
- ❌ Complex to configure; requires understanding burn rate math
- ❌ Low-traffic services have high error rate variance; need minimum request count guards

#### Deep Dive 2: Head vs. Tail Sampling for Distributed Tracing

**The problem:** 4M spans/sec × 1 KB = 350 TB/day if you keep everything. You must sample. But if you decide at the head (when the root span starts) to drop a trace, you lose all of its spans — including any errors that develop 200ms later in a downstream service.

**Head-based sampling** is cheap: the SDK makes a random decision on the root span, sets `flags=01` in the `traceparent` header, and all downstream services inherit the decision. No buffering needed. But it is blind to outcomes — a 10% sample rate drops 90% of your error traces at random.

**Tail-based sampling** is expensive but smart: buffer all spans in the OTel Collector for a `decision_wait` window (30 seconds), wait for the trace to complete, then evaluate policies:
- Always keep: error traces (status_code = ERROR)
- Always keep: high-latency traces (duration > 5 seconds)
- Always keep: critical operations (specific API endpoints)
- Probabilistic 1% for everything else

**Trade-offs:**
- ✅ Tail-based guarantees 100% capture of every error and every slow trace
- ❌ Requires holding 100,000 incomplete traces in memory for up to 30 seconds
- ❌ If a Collector crashes during the window, all buffered traces are lost
- ❌ Tail sampling requires keying Kafka by trace_id so all spans of a trace land on the same partition, preventing loss of span-to-trace association

**In practice:** Use both. Head-based at 10% for volume reduction. Tail-based policies for guaranteed capture of interesting traces. Total result: ~500K spans/sec instead of 4M.

#### Deep Dive 3: Tamper-Evident Hash Chain (Audit Logs)

**The problem:** Compliance regulations (SOC2, PCI-DSS, HIPAA) require proof that audit records have never been modified, deleted, or retroactively inserted. S3 alone, even with Object Lock (WORM mode), proves records were not deleted — but does not prove they were not modified before writing, and does not detect insertion of fake records into the sequence.

**The solution:** SHA-256 hash chain, analogous to a blockchain.

```
H(n) = SHA-256(event_n_json)
C(n) = SHA-256(H(n) || C(n-1))   // chain hash includes previous chain hash
```

Any modification of event_n changes H(n), which changes C(n), which invalidates every subsequent chain hash. Any deletion of event_n breaks the sequential numbering. Any insertion between n and n+1 changes the chain hash at position n+1.

Anchoring: every 10,000 events, publish C(n) to an RFC 3161 timestamping service. This provides cryptographic proof that a specific chain state existed at a specific time. An auditor can verify: (1) all events between anchor points form a valid chain, (2) the RFC 3161 certificate proves when the chain was in that state.

**Trade-offs:**
- ✅ Cryptographic proof of integrity accepted by compliance auditors
- ❌ The Hash Chain Service is a serialization bottleneck (events must be chained in sequence); mitigated by partition-level chains with a merge step
- ❌ A compromised Hash Chain Service could produce a valid-looking but fraudulent chain — mitigate with the RFC 3161 external timestamping anchor

---

### 3g. Failure Scenarios

Senior candidates lead with failure modes, not happy-path flows. For each system, know the top three failure scenarios.

**Alerting system failures:**
- **Alertmanager is down**: No alerts are delivered. Detected only by the dead man's switch (an alert that is always firing and heartbeats an external service). Without the dead man's switch, this failure mode is completely invisible. Run Alertmanager as a 3-node gossip cluster on separate physical machines/racks.
- **Alert storm during cascading failure**: 5,000 host alerts fire simultaneously. Without grouping + inhibition, the on-call receives 5,000 pages. Design: group by `[alertname, cluster]` (1,000 → ~10 notifications), add inhibition rules so a datacenter network outage suppresses all host alerts in that DC.
- **Silence misuse**: Engineers silence a critical alert during maintenance and forget to remove it. Design: max silence duration 24h, daily Slack bot listing active silences, monthly silence audit.

**Logging system failures:**
- **Elasticsearch heap pressure from large shards**: Shards over 60 GB cause GC pressure and slow queries. Design: ILM rollover trigger at 50 GB per shard; daily rollover regardless.
- **Collector disk buffer overflow during Kafka outage**: Filebeat buffers to local disk, but the buffer fills if Kafka is down for hours. Design: per-host disk buffer sized for 4h at peak log rate; alert if Kafka lag exceeds 1h.
- **Field explosion from dynamic mapping**: One service logs JSON with 10,000 unique keys. Elasticsearch maps them all, causing OOM. Design: `dynamic: false` on all index templates; unknown fields stored in `_source` but not indexed.

**Metrics system failures:**
- **Prometheus TSDB OOM from cardinality explosion**: A developer adds `user_id` as a metric label — 1 million users × 100 metrics = 100M new series, 300 GB RAM spike. Design: hard cardinality limit < 100K series per target; reject scrapes that would exceed it.
- **Thanos Compactor falling behind**: If Compactor does not keep up, the number of blocks in S3 grows, query fan-out times increase, and Store Gateway memory spikes. Design: Monitor block count per time range; alert if raw blocks > 100 per day window.
- **Split-brain HA Prometheus pair**: Both instances diverge during a network partition. Thanos Query deduplicates by `replica` label — both replicas must use identical scrape configs or deduplication produces incorrect results.

**Anomaly detection failures:**
- **Concept drift (workload change makes models stale)**: A host is reassigned from a low-CPU batch workload to a high-CPU serving workload. The Prophet model predicts ~40% CPU but the new normal is 80%. Every reading generates a false positive. Design: CUSUM drift detection on model residuals; trigger retraining when cumulative drift exceeds threshold.
- **Model cold start**: A new service has no history to train on. Z-score with conservative threshold (σ > 5 instead of 4) for 7 days; transfer learning from the most similar existing service.
- **Correlation engine false grouping**: Two unrelated incidents coincide in time and get grouped into one. Design: require infrastructure label similarity (same cluster/service) in DBSCAN features, not just timestamp; add `correlation_confidence` field; alert as "may be unrelated concurrent incidents" if below 0.70.

---

## STEP 4 — COMMON COMPONENTS

Every observability system in this pattern shares the following building blocks. Know each one cold.

---

### Elasticsearch 8.x (5 of 6 systems)

**Why used:** Inverted index enables sub-second full-text search across billions of documents. The logging, audit, alerting history, tracing, and anomaly systems all need "find me all events where this field contains this value across the last 7 days." No other system does this at our scale with the Kibana ecosystem attached.

**Key configuration:**
- `refresh_interval: 5s` (not default 1s) — reduces segment flush frequency 5×, significantly improves write throughput
- `translog.durability: async` + `translog.sync_interval: 5s` — avoids per-document fsync, 3–5× write throughput improvement (with small data loss window on crash)
- `dynamic: false` on all templates — prevents field explosion from services that log arbitrary JSON keys
- `index: false` on `error.stack_trace` — store in `_source` for display but do not index (too large, not searched by value)
- `best_compression` codec — 3–4× compression on JSON text
- ILM hot (SSD) → warm (HDD, force-merged to 1 segment/shard) → cold (S3 searchable snapshot) → delete
- Daily indices (e.g., `logs-YYYY-MM-DD`) for fast deletion via index drop vs slow delete-by-query

**What breaks without it:** Without the daily index rollover and ILM, retention enforcement requires delete-by-query — which is O(n) in document count, causes write amplification, and runs slowly. Without `dynamic: false`, one misbehaving service can OOM the entire cluster by creating millions of unique field mappings.

**Only exception:** Metrics and Monitoring — uses Prometheus TSDB + Thanos instead. Time-series data with regular-interval numeric values is a structurally different problem from free-text event search; columnar TSDB compression achieves 1.5 bytes/sample vs Elasticsearch's 50+ bytes per document.

---

### Apache Kafka (all 6 systems)

**Why used:** Decouples producers (50,000 hosts and 500,000 pods) from consumers (Elasticsearch, S3, alert engines, ML scoring). Absorbs burst traffic: logging peaks at 14M events/sec vs 2.8M steady state — the 5× burst factor requires a buffer that can absorb and drain. Enables replay: when you deploy a new Logstash pipeline, reset the consumer offset and reprocess the last 48 hours. Supports multiple consumer groups: the same log stream feeds Elasticsearch (for search), S3 (for archival), and ElastAlert (for alerting) simultaneously.

**Key configuration by system:**
- Audit logs: `acks=all`, `min.insync.replicas=2`, zstd compression, 72h retention, RF=3, 128 partitions — maximum durability
- Operational logs: `acks=1`, lz4 compression, 48h retention, 512+ partitions — maximum throughput
- Traces: key by `trace_id` — ensures all spans of a trace land on the same partition, enabling tail sampling and trace assembly
- Metrics (anomaly): key by metric series key — enables per-series state in Flink operators

**What breaks without it:** Direct producer-to-Elasticsearch writes would cascade burst traffic immediately to the storage layer. During a 5× traffic spike, Elasticsearch would reject writes and you would lose logs. Kafka absorbs the burst and lets Elasticsearch ingest at its sustained capacity.

---

### Multi-Tier Storage: Hot SSD → Warm HDD → Cold S3 (4 of 6 systems)

**Why used:** SSD costs roughly 10× more than HDD per GB, and HDD costs roughly 5× more than S3 object storage. But you need SSD query performance for recent data (last 7 days, the vast majority of searches during incidents) and cannot afford SSD for 7 years of audit data. Tiering lets you optimize cost per query at each tier.

**Key configuration:**
- **Hot tier (SSD):** Full resolution, actively indexed, 1 replica. ILM trigger: index age OR size (whichever comes first). Query: sub-2 second.
- **Warm tier (HDD):** Force-merged to 1 segment per shard (no more writes, read-optimized). Reduced replicas (often 0 for logging). Query: 2–10 seconds.
- **Cold tier (S3 searchable snapshot):** Elasticsearch mounts S3 directly as a frozen index. No local storage needed beyond a small cache. Query: 10–30 seconds.
- **Archive (S3 Parquet):** For analytics queries via Presto/Trino or Athena. Not Elasticsearch-searchable.
- **Audit exception:** S3 Object Lock (WORM) with 7-year Compliance retention period. Not deletable by anyone, including administrators.

**What breaks without it:** Storing 7-day retention entirely on SSD for logging (60 TB/day) would cost ~$600K/month in cloud storage. The tiering model reduces that to ~$100K/month. Without force-merge on warm tier, background segment merging steals CPU from queries on aged data.

---

### Alertmanager as Central Routing Hub (4 of 6 systems)

**Why used:** All four observability signals — metric threshold (Prometheus), log pattern (ElastAlert), ML anomaly (Flink correlation engine), and security event (audit log suspicious activity detector) — need to eventually page a human or post to Slack. Rather than each signal having its own notification logic, they all POST to Alertmanager's API. Alertmanager provides deduplication, grouping, inhibition, silencing, and routing — once, centrally, consistently.

**Key configuration:**
```
group_by: ['alertname', 'cluster', 'service']
group_wait: 30s         # wait before first notification; collects related alerts
group_interval: 5m      # wait before update for existing group
repeat_interval: 4h     # re-notify if still firing
resolve_timeout: 5m     # declare resolved if no update received in 5 min
```

**Three-node HA gossip:** Each Alertmanager instance runs independently. All Prometheus instances send alerts to all three. Gossip (Hashicorp Memberlist) replicates the notification log so instances do not send duplicate pages. Eventually consistent — a brief network partition can produce duplicate pages, but this is acceptable: a duplicate PagerDuty page is preferable to a missed page (availability over consistency).

**What breaks without it:** Each signal team writes their own PagerDuty integration. No deduplication across signals — the same incident generates separate pages from Prometheus, ElastAlert, and the anomaly system. Engineers receive 3–10 pages for the same incident and start ignoring all of them.

---

### W3C TraceContext Propagation (logging + tracing systems)

**Why used:** The `traceparent` header (`00-{traceId}-{spanId}-{flags}`) propagated across every HTTP call, gRPC call, and Kafka message header is the single thread that stitches together all three observability pillars for a given request. When an engineer sees an error in Kibana, they click `trace_id` and jump to the Jaeger timeline for that request. In the Jaeger timeline, they click "View Logs" and jump to Kibana filtered by that same `trace_id`. In Grafana, metric exemplars attach the `trace_id` to high-latency histogram buckets.

**Key configuration:**
- `traceparent: 00-{traceId}-{spanId}-{01|00}` — 01 = sampled, 00 = not sampled
- `trace_id` is a mandatory field in every structured log event
- Sampling decision in `flags` is propagated: if parent says "sample," all downstream services sample — ensures you never have a trace with only some spans
- Manual propagation needed for Kafka: extract `traceparent` from message headers in consumer

**What breaks without it:** Logs and traces become disconnected. An engineer debugging a slow request must manually correlate timestamps across Kibana and Jaeger, which is error-prone and slow during an incident. This is the difference between a 2-minute root cause analysis and a 20-minute one.

---

### Time-Based Sharding (all 6 systems)

**Why used:** Deleting data by time range is a core operation in every observability system. Elasticsearch index drop (`DELETE /logs-2026-03-*`) is O(1) — it removes a directory. Delete-by-query (`DELETE FROM logs WHERE timestamp < 30d ago`) scans and marks every matching document, causing massive write amplification and slowing the cluster for hours. Similarly, Prometheus TSDB's 2h block structure, Kafka's time-based retention, and S3's TSDB block key by time range all enable O(1) retention enforcement.

**Configuration:**
- Elasticsearch: `logs-YYYY-MM-DD` daily indices; ILM deletes whole indices, never documents
- Prometheus TSDB: 2h blocks on disk, uploaded by Thanos Sidecar; Compactor merges and enforces retention by deleting blocks older than policy
- Kafka: time-based retention (24–72h by system); log segments deleted when oldest segment exceeds retention
- S3/Thanos: blocks keyed by `[min_time, max_time]`; Compactor deletes block directories

---

### Sampling at Ingestion (3 of 6 systems)

**Why used:** The full volume of data generated by 500,000 Kubernetes pods is not economically or operationally feasible to store at full resolution forever. Sampling is the mechanism that makes observability at scale affordable while preserving the signals that matter.

**Three different sampling strategies:**
- **Anomaly detection — tiered by criticality:** Z-score on all 12.15M series (cheap, O(1)), Prophet on top 10% (medium cost), LSTM on top 1% critical series (expensive, GPU). Series are promoted based on business criticality and historical anomaly significance.
- **Distributed logging — by log level:** DEBUG logs sampled at 1% for high-volume services; INFO at 100%; ERROR/FATAL at 100% with `acks=all` on a separate high-priority Kafka topic. The justification: 50% of log volume is DEBUG, sampling it at 1% saves ~50% of storage with no loss of incident-relevant data.
- **Distributed tracing — head + tail combination:** Head-based at 10% (cheap, propagated via traceparent flags) plus tail-based policies that override the head decision for errors and high-latency traces. The tail sampler buffers spans for 30 seconds before committing the sampling decision.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

This is what separates the systems from each other. One short answer for each, then the two-sentence differentiator.

---

### Alerting and On-Call System

**What makes it unique:**
- The core algorithm is **multi-window multi-burn-rate SLO alerting** — not ML, not statistics, but burn-rate math that directly ties an alert to error budget consumption rate
- **Alert quality as a first-class metric**: actionability rate (> 80%), false positive rate (< 5%), MTTA, MTTR, alerts per on-call shift — tracked monthly per team like an SLO
- **Alertmanager HA via gossip**, not consensus — availability over consistency because a duplicate page is better than a missed page
- **Dead man's switch**: `alert: DeadMansSwitch; expr: vector(1)` — always fires, heartbeats an external service; detects when monitoring itself has failed
- **Inhibition trees**: root cause alert suppresses downstream symptom alerts (datacenter network down → suppress all host alerts in that DC)
- **On-call schedule design**: primary → secondary → manager → VP escalation at T+0/5/10/15 minutes; follow-the-sun for global teams

**Two-sentence differentiator:** Alerting is the only system where the core design challenge is human factors — specifically, alert fatigue that destroys on-call effectiveness. Every technical decision (SLO burn rates, inhibition rules, grouping, quality metrics) exists to ensure that when a critical page fires, it is always acted upon.

---

### Anomaly Detection System

**What makes it unique:**
- **Tiered ML**: three algorithms at three cost points covering 100% of 12.15M series: Z-score/EWMA (O(1), all series), Prophet with changepoints and multi-period seasonality (top 10%), LSTM with uncertainty output (top 1%)
- **CUSUM drift detection** triggers model retraining when residuals accumulate evidence of a mean shift — handles the concept drift problem (workload reassignment changes "normal")
- **DBSCAN correlation engine**: clusters co-occurring anomalies within a 5-minute window by temporal + label similarity; ranks root causes by earliest time (0.4), downstream dependency count (0.4), and anomaly score (0.2)
- **Feedback loop**: on-call engineers mark anomaly alerts TP/FP; ≥ 3 FP feedbacks for same series in 24h triggers immediate retraining; circuit breaker disables model (falls back to Z-score) if FP rate > 50%
- Unlike all other systems, this one is **a consumer of metrics, not an original signal source**; its output is an AnomalyEvent that feeds into Alertmanager like any other alert source

**Two-sentence differentiator:** Anomaly detection is the only system that learns what "normal" means per time series, replacing the brittle manually-tuned static thresholds that break every time workloads change. The design challenge is running 12.15M models at 803K data points/sec while keeping false positives below 5% through drift detection and engineer feedback.

---

### Audit Log System

**What makes it unique:**
- **Immutability is the primary requirement** — not query performance, not latency, not cost. Every other design decision is subordinate to "no one can modify or delete this record."
- **SHA-256 hash chain**: `C(n) = SHA-256(event_n_json || C(n-1))` — detects insertion, deletion, or modification of any record. Anchored every 10,000 events to RFC 3161 external timestamping service.
- **S3 Object Lock (WORM Compliance mode)**: prevents deletion by any IAM principal, including root, for 7 years
- **Dedicated separate Kafka cluster** with `acks=all`, `min.insync.replicas=2` — cannot share with operational logging which uses `acks=1`
- **Rich event schema**: who (actor identity, MFA status, source IP), what (action type, API endpoint, request body), when (timestamp, received_at), where (cluster, namespace), outcome (success/failure), why (change ticket, approval ID)
- **Suspicious Activity Detector**: real-time rule evaluation — privilege escalation, bulk delete > 100 resources/hour, off-hours admin access, repeated auth failures

**Two-sentence differentiator:** Audit logging is distinguished from all other observability systems by its legal and compliance function — records must be provably complete and unmodified for 7 years, not just searchable and fast. The hash chain and WORM storage are not operational conveniences; they are the proof artifacts that a SOC2 or PCI-DSS auditor examines.

---

### Distributed Logging System

**What makes it unique:**
- **Free-text search at petabyte scale** is the core capability — not numeric time-series, not structured event records, but arbitrary text messages from 560,000 sources
- **Two collector types for two source types**: Filebeat for bare-metal (tails log files, journald, MySQL slow query log); Fluent Bit as a K8s DaemonSet (captures pod stdout/stderr from container runtime log paths)
- **Field explosion is the primary operational risk** at this scale — `dynamic: false` is not optional, it is required
- **Log-level-based sampling and durability stratification**: DEBUG at 1%, INFO at 100% on `acks=1` topic, ERROR/FATAL at 100% on a separate `acks=all` high-priority topic — three tiers of durability within the same pipeline
- **Trace ID correlation bridge**: the `trace_id` field in every structured log event is what makes logs useful for debugging traces; without it, you have logs and traces but no way to connect them

**Two-sentence differentiator:** Distributed logging is the broadest-scope observability system — it must handle structured JSON, unstructured syslog, MySQL slow queries, and Kubernetes container output from 560,000 sources at up to 14M events/sec. The operational challenge is controlling costs (60 TB/day requires aggressive tiering and sampling) while guaranteeing no loss of ERROR+ level events.

---

### Distributed Tracing System

**What makes it unique:**
- **W3C TraceContext propagation** is the foundation — the `traceparent` header must be implemented correctly by every service or traces are broken at that service boundary
- **Kafka partitioning strategy is critical**: partition by `trace_id` (not service or timestamp) to ensure all spans of a trace land on the same partition, which is required for tail sampling and trace assembly
- **Head vs. tail sampling tension**: head-based is cheap but blind to outcomes; tail-based requires holding 100K incomplete traces in memory for 30 seconds; both are needed
- **Span Metrics Connector**: derives RED metrics (Rate, Errors, Duration) directly from spans, eliminating the need for separate metric instrumentation for service-level indicators
- **Cassandra vs. Elasticsearch trade-off** for trace storage: Elasticsearch for rich search and existing ELK investment; Cassandra for 2× higher write throughput and O(1) trace_id lookups without full-text search needs
- **Service dependency graph** is automatically generated from trace data — this is a byproduct of tracing that provides a continuously-updated topology map

**Two-sentence differentiator:** Distributed tracing is the only observability signal that captures the causal chain of a request across services — showing exactly which service contributed which latency to a 500ms end-to-end response. The design challenge is making sampling work correctly so that every error and slow request is always captured, while keeping storage costs manageable for the 90% of requests that are fast and successful.

---

### Metrics and Monitoring System

**What makes it unique:**
- **Pull model (Prometheus scrapes `/metrics`)** rather than push — automatic service discovery via K8s SD; clear delineation of which services are instrumented; exporters as a consistent interface pattern
- **Prometheus TSDB compression**: XOR encoding for values, delta-of-delta for timestamps — achieves 1–2 bytes/sample vs 16 bytes uncompressed. This is why Prometheus can store 15 days × 5.2M samples/sec × 1.5 bytes = ~10 TB locally without extraordinary hardware.
- **Cardinality is the dominant operational risk**: never use `user_id`, `request_id`, or `trace_id` as a metric label — a single cardinality explosion can OOM an entire Prometheus instance
- **Thanos architecture**: Sidecar (per Prometheus, uploads blocks to S3), Store Gateway (serves historical blocks), Compactor (singleton, deduplicates HA pairs, downsamples), Query (fan-out and dedup), Query Frontend (caches sub-query results)
- **PromQL internals** are commonly tested: `histogram_quantile()` works by finding the bucket where cumulative count crosses the percentile, then linearly interpolating within that bucket — accuracy depends entirely on bucket boundary placement
- **Pushgateway** handles the one case where pull model fails: batch jobs and short-lived processes that exit before Prometheus can scrape them

**Two-sentence differentiator:** Metrics and monitoring is the only observability system that uses a pull model and purpose-built TSDB compression, reflecting the fundamentally different nature of numeric time-series data vs. event streams. Cardinality management — never exceeding 100K series per target — is the primary operational discipline that separates stable production Prometheus deployments from memory-blowing failures.

---

## STEP 6 — Q&A BANK

---

### Tier 1 — Surface Questions (2–4 sentences each)

**Q: What is the difference between metrics, logs, and traces?**
**Metrics** are numeric time-series measurements sampled at regular intervals (CPU usage every 15 seconds, request rate per minute). They are great for dashboards, alerting, and capacity planning but cannot explain why a number is high. **Logs** are timestamped textual records of discrete events — errors, state transitions, debug information. They explain what happened but searching across billions of them is expensive. **Traces** capture the end-to-end journey of a single request through multiple services, showing which service contributed which latency. Together they form the three pillars of observability; cross-correlating them via a shared `trace_id` is what makes debugging at scale practical.

**Q: Why do we use Kafka between collectors and Elasticsearch instead of writing directly?**
**Decoupling and burst absorption.** Log ingestion peaks at 5× steady-state during incidents or deployments. If collectors write directly to Elasticsearch, a 5× spike would overwhelm the indexing pipeline and cause write rejections — permanent log loss. Kafka absorbs the burst: producers write at peak rate, consumers drain at Elasticsearch's sustained throughput. Kafka also enables replay: if you deploy a new Logstash pipeline with a bug, you reset the consumer offset and reprocess the last 48 hours from Kafka without any data loss.

**Q: What is Thanos and why do we need it alongside Prometheus?**
Prometheus stores data locally (TSDB) with a default 15-day retention limit. Beyond that, you either need enormous disks or you lose historical data. **Thanos** solves three problems simultaneously: it uploads completed 2-hour Prometheus blocks to S3 for cheap long-term storage (1 year+), it provides a global query layer that fans out to all 200 cluster Prometheus instances to answer queries like "show me CPU across all clusters," and it deduplicates the HA pair of Prometheus instances per cluster so you see each time series once, not twice.

**Q: What is a dead man's switch in alerting?**
A dead man's switch is an alert rule that always evaluates to true: `alert: DeadMansSwitch; expr: vector(1)`. Prometheus fires it to Alertmanager every minute. Alertmanager routes it to an external heartbeat service (Healthchecks.io or a custom endpoint). If the heartbeat stops arriving, the external service pages the on-call team directly via PagerDuty API — bypassing Alertmanager entirely. This answers the question "who monitors the monitoring?" — without it, an Alertmanager outage is completely silent.

**Q: What is alert fatigue and how do you measure it?**
Alert fatigue is the phenomenon where on-call engineers receive so many alerts (especially false positives) that they begin to ignore all alerts, including genuine critical ones. It is measured through alert quality metrics tracked monthly per team: **actionability rate** (> 80% of alerts should require human action), **false positive rate** (< 5% should auto-resolve within 5 minutes), **MTTA** (mean time to acknowledge, < 5 minutes for critical), and **alerts per on-call shift** (< 10). A team averaging 150 alerts per shift has an alert fatigue crisis.

**Q: How does Elasticsearch ILM (Index Lifecycle Management) work?**
ILM automates the hot → warm → cold → delete lifecycle for indices. You define a policy: an index moves from hot to warm when it exceeds 50 GB or is older than 1 day; it moves from warm to cold after 30 days; it is deleted after 1 year. On the warm tier, ILM triggers a force-merge to 1 segment per shard, making the index read-only and read-optimized. On the cold tier, ILM mounts the index as a searchable snapshot from S3 — the data stays in object storage and only a small local cache is needed. The benefit: automatic cost optimization without manual intervention.

**Q: What is OpenTelemetry and why does it matter?**
OpenTelemetry (OTel) is a CNCF standard for instrumentation: a single set of SDKs (Java agent, Python SDK, Go SDK) and a wire protocol (OTLP/gRPC) that works for traces, metrics, and logs. Before OTel, every tracing vendor (Zipkin, Jaeger, Datadog, Dynatrace) had its own SDK — switching vendors required code changes across every service. With OTel, you instrument once and route to any backend via the OTel Collector. The Collector can simultaneously export to Jaeger (traces), Prometheus (span-derived metrics), and Kafka (for ML pipelines) without changing application code.

---

### Tier 2 — Deep Dive Questions (with trade-offs)

**Q: Walk me through how multi-window multi-burn-rate SLO alerting works and why it is better than simple threshold alerting.**
For a 99.9% SLO over 30 days, the error budget is 0.1% × 30 × 24 × 60 = 43.2 minutes. A **burn rate** is the rate at which the error budget is being consumed — a burn rate of 14.4× means the entire 30-day budget will be exhausted in 2 days. The critical insight is that burn rate directly maps to business impact, unlike an arbitrary threshold like "error rate > 5%." The **multi-window** trick prevents false positives: require both a 1-hour window (sustained burn) AND a 5-minute window (currently still burning). The long window catches the problem; the short window confirms it has not already resolved. Without the short window, a spike 50 minutes ago still shows as a high 1-hour error rate even if the service recovered. Trade-offs: ✅ zero false positives from transient spikes, ✅ directly tied to SLO budget, ❌ complex math to configure, ❌ low-traffic services need minimum request count guards or a single error generates a 100% error rate.

**Q: How does tail-based sampling work in distributed tracing, and what are its failure modes?**
The OTel Collector buffers all incoming spans for a configurable `decision_wait` window (30 seconds). During this window, it holds all spans in memory keyed by `trace_id`. When the window expires (or a root span with `endTime` is received), the Collector evaluates sampling policies: always keep errors, always keep traces > 5 seconds, keep 1% of everything else. Only then are spans forwarded to Jaeger. The failure modes are significant: ✅ guarantees 100% capture of every error and slow trace, ✅ allows sophisticated multi-span policies impossible with head sampling, ❌ requires a large in-memory buffer (100,000 traces × average 8 spans × 1 KB = ~800 MB per Collector), ❌ Kafka must be partitioned by `trace_id` to ensure all spans reach the same Collector (otherwise the decision buffer is split across Collectors), ❌ Collector crash during the decision window loses all buffered spans. The practical approach is head-based sampling at 10% as the first filter, with tail-based policies only for the subset of traces that pass the head filter — reducing the tail sampler's memory burden by 90%.

**Q: How does Prometheus TSDB achieve 1–2 bytes per sample?**
Two compression techniques work together. **Timestamps** use delta-of-delta encoding: if scrapes happen every 15 seconds, the delta between consecutive timestamps is always 15000ms, and the delta-of-delta is 0 — which can be encoded as a single bit. Only irregular scrapes need to encode actual delta values. **Values** use XOR encoding with the previous value: if memory usage changes by 0.1% between scrapes, the XOR of the two float64 values has many leading zero bits, which Prometheus encodes with a leading-zero prefix and only stores the non-zero bits. Slowly changing values (disk usage, memory total) compress extremely well; rapidly changing values (network packet rate) compress less. The baseline uncompressed cost is 16 bytes per sample (8B timestamp + 8B float64); Prometheus achieves 1–2 bytes in practice, a 10× improvement, which is why 78.5M series × 5.2M samples/sec × 1.5 bytes ≈ 674 GB/day — manageable without compression it would be 6.7 TB/day.

**Q: Explain the Alertmanager gossip protocol and why it uses eventual consistency instead of Raft.**
Alertmanager runs as a 3-node cluster where all nodes operate independently — there is no leader. Each Prometheus instance sends alerts to all 3 nodes simultaneously. When node 1 sends a PagerDuty notification, it records the event in its notification log (nflog) and gossips this entry to nodes 2 and 3. Node 2, upon receiving the same alert, checks its nflog, sees the notification was already sent, and skips it. The fundamental trade-off is availability over consistency. Raft requires quorum (2 of 3 nodes) to make decisions. If two nodes are partitioned, Raft would block all notification decisions — outages go undetected. Gossip allows each node to operate independently: ✅ each node can page independently even with the other two nodes down, ✅ no blocking on consensus, ❌ small window where two nodes both send a notification before gossip propagates — acceptable because a duplicate page is far less harmful than a missed page. PagerDuty handles duplicate deduplication on their side via `dedup_key`.

**Q: How would you design for cardinality control in a metrics system?**
Cardinality is the number of unique time series and is determined by the Cartesian product of label values. A metric with `service` (50 values) × `endpoint` (100 values) × `status_code` (10 values) = 50,000 series — acceptable. The same metric with `user_id` (1 million values) × anything = millions of series — OOM. Three defense layers: **Static enforcement** — never use user_id, request_id, trace_id, or any unbounded identifier as a metric label; use histograms instead of individual measurements. **Per-target hard limits** — reject scrapes where the target exposes > 100K series; Prometheus `metric_relabel_configs` can drop high-cardinality labels. **Monitoring** — alert when series count for a target grows > 20% week-over-week. The incident playbook: when a cardinality explosion is detected, immediately add a `metric_relabel_config` to drop the offending label, which takes effect on the next scrape cycle (15 seconds) without a Prometheus restart. ✅ Prevents OOM without requiring service redeploy. ❌ Dropped label data is unrecoverable.

**Q: How does the SHA-256 hash chain in the audit log system prove tamper-evidence?**
The chain builds as follows: `H(n) = SHA-256(raw JSON of event n)`, then `C(n) = SHA-256(H(n) || C(n-1))`. Each event's chain hash encodes all prior chain hashes, creating a dependency where any change to event k changes C(k), which changes C(k+1), ..., which changes C(current). To verify the audit trail, an auditor provides a time range. The system retrieves the stored chain hashes, recomputes `C(n) = SHA-256(H(n) || C(n-1))` for each event in the range, and checks that computed hashes match stored hashes. A single discrepancy identifies the exact tampered event. For insertion attacks (someone inserts a fake event between n and n+1): the inserted event changes C(n+1), invalidating all subsequent chain hashes — detected immediately. The RFC 3161 timestamping anchor every 10,000 events provides additional proof that a specific chain state existed at a specific time, which prevents the attack of "we replaced the entire chain with a fraudulent but internally consistent one."

**Q: Compare Elasticsearch and Cassandra for trace storage in Jaeger.**
Elasticsearch provides rich search — find all traces of service "job-scheduler" with duration > 5 seconds and tag `job.priority=HIGH`. This uses Elasticsearch's inverted index on nested `tags` documents. Cassandra provides excellent write throughput (~2× Elasticsearch) and O(1) trace lookups by `trace_id` (the primary access pattern in production), but its secondary indexes are limited — tag-based search requires either maintaining a separate ES index for search metadata or accepting full-table scans. ✅ Elasticsearch: rich search, Kibana integration, leverages existing ELK cluster; ❌ Elasticsearch: 2× higher write cost per span, heap pressure from nested tag documents. ✅ Cassandra: better write throughput, linear horizontal scaling, efficient primary key lookups; ❌ Cassandra: weak search, requires additional infrastructure for tag queries. The practical answer: use Elasticsearch if you already run ELK for logging (share the cluster, share the expertise). Use Cassandra if write throughput is the bottleneck and you primarily look up traces by ID rather than searching by tags.

---

### Tier 3 — Staff+ Stress Tests (reason aloud, show trade-offs)

**Q: Our Prometheus cardinality has exploded from 5M to 50M series overnight due to a bad deployment. Prometheus is OOMing. Walk me through the immediate mitigation and longer-term prevention.**
Immediate (next 5 minutes): identify the offending deployment using `topk(20, count by (__name__, job) ({__name__=~".+"}) )` to find which metric name and job exploded in series count. Add a `metric_relabel_config` drop rule for the offending high-cardinality label (e.g., `user_id`) — takes effect on the next 15-second scrape without restart. If Prometheus is already dead, scale up RAM temporarily or reduce `--storage.tsdb.retention.time` to force earlier block deletion and free memory. Medium-term (next hour): roll back the offending deployment. Add the label drop rule to the service's scrape config permanently. Long-term (next week): implement cardinality enforcement in CI/CD — PR gates that estimate cardinality impact before merging metric changes. Add a Prometheus alert: `increase(prometheus_tsdb_head_series[1h]) > 500000` — fires when series count grows rapidly. The key insight to say aloud: cardinality explosions are often from a developer adding a request ID or user ID as a label without realizing each unique value creates a new time series. Education is as important as enforcement.

**Q: You need to prove to a PCI-DSS auditor that your audit logs have not been tampered with for the last 3 years. How do you do it?**
The proof has three layers. **Layer 1 — Completeness**: show that the Kafka topic for audit events has `acks=all` and `min.insync.replicas=2`, and that the collector uses synchronous Kafka writes with retry. Show that S3 Object Lock (WORM Compliance) is enabled on the audit bucket with a 7-year lock. Deletion of any object before the lock expiration is cryptographically prevented — even AWS cannot do it. **Layer 2 — Chain integrity**: run the hash chain verification API for the requested time range. It recomputes `C(n) = SHA-256(H(n) || C(n-1))` for every event and reports any discrepancy. If the chain is intact for 3 years, no event was modified, deleted, or inserted. **Layer 3 — Timestamp proof**: show RFC 3161 timestamping receipts anchored every 10,000 events (every ~12 seconds at our ingestion rate). Each receipt is a signed certificate from a trusted timestamping authority stating "this chain hash existed at this exact time." This proves the chain was not retroactively constructed — the timestamps predate the audit request. The auditor can verify RFC 3161 certificates using standard tools (openssl ts -verify). The thing to say aloud: the combination of WORM (proves no deletion) + hash chain (proves no modification or insertion) + RFC 3161 (proves timestamps are real) is a complete tamper-evidence proof accepted by SOC2, PCI-DSS, and HIPAA auditors.

**Q: Your anomaly detection system has a 30% false positive rate on a specific service. On-call engineers are ignoring its alerts. How do you fix this without turning off the system?**
Start with data, not guesses. Pull the `anomaly_feedback` table for this service over the last 30 days: `SELECT method, COUNT(*) FILTER (WHERE feedback_type = 'false_positive') AS fps, COUNT(*) FILTER (WHERE feedback_type = 'true_positive') AS tps FROM anomaly_feedback WHERE series_key LIKE '%service-name%' AND feedback_at > NOW() - INTERVAL '30 days' GROUP BY method ORDER BY fps DESC`. This tells you whether the FPs are coming from Z-score, Prophet, or LSTM — very different root causes. If Z-score: the threshold is too tight; widen it from σ=4 to σ=5 for this service's series. If Prophet: the model may not have captured a workload change — check CUSUM drift detector state; if drift is flagged, trigger immediate retraining. If it is across all methods: the metric itself may be poorly suited for ML anomaly detection (a metric that legitimately spikes, like batch job CPU) — these should be moved to static threshold alerting instead. Implement a circuit breaker: if the last-100-feedbacks FP rate for a series exceeds 50%, disable the ML model and fall back to Z-score automatically — reducing FPs while preserving some detection. Most importantly: communicate to the on-call team what you did and ask them to re-engage for 2 weeks. Engineer trust in the system is as important as the technical fix.

**Q: You need to reduce logging infrastructure cost by 60% without losing incident response capability. What do you do?**
This is a cost-performance optimization problem. Start by profiling what is actually being stored. Pull the Elasticsearch index size breakdown by service and log level. Typically DEBUG logs constitute 40–60% of volume. Current sampling: 1% DEBUG. Can we go to 0.1% for the top 5 volume services with stable, well-understood behavior? That alone might recover 20–30% cost. Next, look at retention. Is the warm tier retention at 30 days because engineers actually query 25–30 day old data, or because that was the initial setting? Audit Kibana query patterns: if 95% of queries are on data < 7 days old, drop warm retention to 14 days, saving ~50% of warm tier cost. Then evaluate moving the cold tier from searchable snapshots to pure Parquet on S3 queried via Athena — Athena queries take 30–60 seconds (vs. 10–30 for searchable snapshots) but cost 10× less per TB. For compliance investigation queries that happen monthly, acceptable. For incident debugging, keep a 7-day SSD hot tier intact. Key constraint to state explicitly: ERROR and FATAL logs must never be sampled and their retention must not be reduced — they are the incident response signal. Everything else is negotiable. The 60% cost reduction is achievable by combining DEBUG sampling (20–25%), warm retention reduction (20–25%), and cold tier migration (10–15%).

**Q: How would you design observability for a completely new service that starts cold — no history, no baseline, no traffic patterns established?**
The cold start problem affects anomaly detection most acutely. For the first 7 days: instrument the service with standard metrics (RED: Rate, Errors, Duration), push structured logs with trace_id, and emit OTel spans. Use conservative static thresholds initially (error rate > 10% for 5 minutes, latency P99 > 2 seconds) — these are directionally correct even without history. For anomaly detection: use Z-score with a wide threshold (σ > 5 instead of 4) to avoid FPs during the learning period. Apply transfer learning: find the 3 most similar existing services using DTW (Dynamic Time Warping) distance on the first 24 hours of metric data, and use their trained Prophet models with adjusted level parameters as warm-start models. Suppress anomaly confidence below 0.7 during bootstrapping (first 7 days) to prevent premature paging. After 7 days: train a full Prophet model on the accumulated data. After 30 days: add the service to the LSTM training candidate pool if it is in the top 1% critical tier. For tracing: head-based sampling at 100% (not 10%) for the first 7 days — you want every request trace during the debugging and tuning period. After 30 days, reduce to the standard 10% + tail-based error/latency capture. The staffed-level insight: the cold start protocol is a state machine with explicit transitions, not a continuous process. Each transition (7d → Prophet, 30d → LSTM candidate, 30d → reduce sampling rate) should be automated with human opt-out, not manually triggered.

---

## STEP 7 — MNEMONICS

### The MALT Stack Memory Trick

For the shared infrastructure, remember **MALT**:
- **M** — Multi-tier storage (hot SSD → warm HDD → cold S3 → delete)
- **A** — Alertmanager as the central routing hub for all signals
- **L** — "L" for Log (Elasticsearch for text search, 5 of 6 systems)
- **T** — Time-sharding (all 6 systems partition data by time for O(1) deletion)

Then the two extras: **Kafka** (buffer for all 6) and **TraceContext** (correlation thread).

### The Three-Tier Anomaly Detector

**"Z scores Pretty well, but LSTMs Learn More"** → Z-score/EWMA, Prophet, LSTM — the three tiers in increasing cost and sophistication.

### The SLO Burn Rate Anchor Numbers

Memorize the two critical burn rates:
- **14.4×** → 2 days → CRITICAL (page immediately)
- **6×** → 5 days → CRITICAL (page)

The math: `30 days / burn_rate = days until budget exhausted`. If burn rate × error_rate_threshold = `14.4 × 0.001 = 0.0144`, any short window above 1.44% error rate is a critical alert.

### Opening One-Liner for Any Observability Problem

"Observability is about three signals — metrics, logs, and traces — with Kafka as the universal buffer, Elasticsearch as the search layer, and a shared trace ID that stitches them together. The hard problems are volume (14M events/sec requires tiering), completeness (audit logs must be tamper-proof), and quality (alert fatigue destroys on-call effectiveness). Let me start with clarifying questions."

---

## STEP 8 — CRITIQUE

### Well-Covered Areas

The source materials are exceptionally thorough in several areas that frequently come up in Staff+ and Principal interviews:

- **Multi-window multi-burn-rate SLO alerting** is explained with complete math, PromQL implementation, and the exact burn rate table. This is rarely covered this precisely in other study materials.
- **Prometheus TSDB internals** — XOR and delta-of-delta compression, TSDB block structure, WAL, compaction ranges — are covered at the implementation level, which is exactly what interviewers probe.
- **Tail sampling mechanics** are explained with the exact YAML configuration for the OTel Collector tail sampling processor, including the decision window size and buffer count.
- **Hash chain implementation** includes the exact SHA-256 formula, the RFC 3161 anchoring frequency, and the verification procedure.
- **LSTM architecture for anomaly detection** includes the actual PyTorch model with uncertainty output (predicting N(mean, std²)) rather than just saying "we use LSTM."
- **Alert quality metrics** — actionability rate, MTTA, MTTR, repeat offender rate — are quantified with targets, which is what a FAANG interviewer expects.

### Missing or Shallow Areas

**Grafana Loki as an alternative to Elasticsearch for logging.** The source materials mention Loki briefly but dismiss it ("insufficient for our bare-metal + multi-platform fleet"). For K8s-native workloads, Loki's label-only indexing (no full-text) trades search capability for dramatically lower cost and simpler operation. An interviewer at a Kubernetes-first company will probe this. Know the trade-off: Loki cannot do arbitrary text search, only label-based filtering — but that is often sufficient for structured JSON logs where you filter by service, level, and cluster.

**VictoriaMetrics as a Prometheus alternative.** The source materials mention it briefly. Know that VictoriaMetrics achieves ~7× better compression than Prometheus TSDB (0.2–0.4 bytes/sample vs. 1–2 bytes) through a different compression algorithm, runs as a single binary (simpler than Thanos's 5 components), and is API-compatible with Prometheus. For cost-sensitive environments, this is a serious alternative. The downside: less mature multi-tenancy and a smaller ecosystem.

**OpenTelemetry Metrics** (as a replacement for Prometheus exposition format). The source materials focus on OTel for tracing. OTel metrics are an emerging protocol that could replace both Prometheus scraping and StatsD. Know that it exists and that the industry is moving toward OTel as the unified standard for all three signal types.

**Grafana Agent / Alloy** — the lightweight Prometheus-compatible agent that replaces the full Prometheus server for remote-write-only scenarios. Not covered in the source materials but commonly asked about in infrastructure interviews.

**Distributed tracing for async systems (Kafka spans).** The source materials mention Kafka header propagation but do not detail how `traceparent` headers are injected into Kafka message headers on produce and extracted on consume. This is a common interview follow-up: "How do you trace a request that goes through a Kafka queue?" The answer: OTel Kafka instrumentation injects `traceparent` as a Kafka record header; the consumer creates a child span with the parent context extracted from the header; the span `kind` is `CONSUMER`, linking it causally to the producer's `PRODUCER` span.

### Senior-Level Probes to Prepare For

**"You've designed this logging system. Six months later, a developer accidentally logs a user's PII (passwords, SSNs) into the application logs. What do you do?"** This is a data governance and compliance question. You need a scrubbing pipeline (Logstash filter with regex-based PII detection), a retroactive purge mechanism (Elasticsearch update-by-query to redact specific fields, followed by force-merge to overwrite segments), and a notification to the security and legal teams. The hard part: Elasticsearch does not natively support field-level deletion without reindexing — you must overwrite the `message` field with a redacted version.

**"How do you ensure your anomaly detection system doesn't learn a new incident as 'normal'?"** This is the training data contamination problem. If a service had a 3-day outage (CPU at 100%) and you trained Prophet on a 90-day window including those 3 days, the model learns "100% CPU is normal occasionally." Defense: use IQR (interquartile range) filtering to remove statistical outliers before training. Use a holdout set that excludes known incident periods. Monitor model residuals for systematic bias.

**"Your Thanos Compactor fell behind by 30 days. What are the consequences and how do you recover?"** Consequences: S3 has 30 days of raw 2h blocks instead of compacted blocks. Thanos Store Gateway's memory grows as it loads more block metadata. Range queries now fan out across many more blocks (slower). HA deduplication is not applied to the 30-day backlog (duplicate series in query results). Recovery: restart Compactor with `--compact.concurrency=8` and sufficient resources. Monitor `thanos_compact_iterations_total` to track progress. During recovery, Thanos Query Frontend's caching is less effective (no compacted blocks to cache), so expect 2–3× slower long-range queries.

### Common Interview Traps

**Trap 1: Designing the Kafka cluster as part of the logging system.** Kafka is almost certainly shared infrastructure. Say explicitly: "I would reuse the existing Kafka clusters rather than provisioning a dedicated one for logging. I would add logging-specific topics with appropriate retention and replication settings."

**Trap 2: Recommending Elasticsearch for metrics.** Elasticsearch is wrong for numeric time-series. The inverted index does not help with range queries over timestamps, and compression is terrible (50 bytes/sample vs. 1.5 bytes for Prometheus TSDB). If asked "why not ClickHouse for metrics?" — ClickHouse is columnar and would be better than ES, but still worse than TSDB for regular 15-second interval data because TSDB's delta-of-delta encoding is specifically designed for regular intervals.

**Trap 3: Not distinguishing audit logs from operational logs.** These must be separate pipelines — separate Kafka cluster (different durability settings), separate Elasticsearch cluster (different access controls, separate RBAC), separate S3 bucket (Object Lock vs. standard). Combining them is a compliance violation: a developer with access to operational logs for debugging must not have read access to audit logs containing privileged action records.

**Trap 4: Forgetting the "who monitors the monitoring" question.** Every alerting design discussion ends with "but what if Alertmanager is down?" The dead man's switch is the correct answer. If you do not mention it, the interviewer will ask, and not having a ready answer signals you have not designed a production alerting system before.

**Trap 5: Recommending static thresholds for anomaly detection without acknowledging their failure mode.** Static thresholds require manual tuning per metric per host per environment and break every time workloads change. If you recommend them, immediately acknowledge: "Static thresholds are the right starting point for new services or services with very stable patterns. For the 10,000+ services in a fleet this size, dynamic thresholds based on Prophet or similar are necessary to remain maintainable."

---

*End of Infra Pattern 8 — Observability Interview Guide*

---
