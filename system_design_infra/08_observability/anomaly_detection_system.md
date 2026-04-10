# System Design: Anomaly Detection System for Infrastructure Metrics

> **Relevance to role:** A cloud infrastructure platform managing 10,000+ bare-metal servers, 100,000+ Kubernetes pods, and millions of time-series metrics cannot rely on static thresholds alone. CPU usage patterns differ by workload type; network traffic is seasonal; a "high" memory reading on a batch job server is normal but the same reading on a cache server signals a memory leak. ML-based anomaly detection — combining statistical methods (Z-score, MAD), time-series forecasting (ARIMA, Prophet, LSTM), dynamic thresholds, and multi-metric correlation — is increasingly essential for maintaining large-scale infrastructure with a small SRE team. This document designs a production-grade anomaly detection engine that integrates with the alerting pipeline.

---

## 1. Requirement Clarifications

### Functional Requirements
- Detect anomalies in infrastructure time-series metrics: CPU, memory, disk I/O, network throughput, request rates, error rates, latency percentiles, Kubernetes resource usage, MySQL query performance, Elasticsearch cluster health.
- Support multiple detection algorithms: statistical baseline (Z-score, Median Absolute Deviation), trend-based (ARIMA, Holt-Winters), ML-based (LSTM, Facebook Prophet), and threshold-less pattern detection.
- Handle seasonality: daily, weekly, and annual cycles in metric patterns (e.g., traffic peaks at noon and 7pm, lower on weekends, spikes around product launches).
- Provide dynamic thresholds: automatically adjust what "normal" means for each metric/host/service based on historical patterns, replacing manually-tuned static thresholds.
- Correlate anomalies across metrics: a root cause anomaly (CPU spike on host X) may manifest as downstream anomalies (latency spike on service Y, error rate increase on service Z). Identify these correlations.
- Generate structured anomaly events integrating with the Alertmanager pipeline, enriched with confidence scores, contributing metrics, and explanations.
- Support a feedback loop: on-call engineers mark anomaly alerts as "true positive" or "false positive" to improve model accuracy over time.
- Provide an anomaly dashboard showing anomaly history, confidence trends, and model performance metrics.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Detection latency | < 5 minutes from metric anomaly to anomaly alert generated |
| False positive rate | < 5% of generated anomaly alerts (measured over rolling 30 days) |
| False negative rate | < 2% of real incidents not detected by anomaly system |
| Coverage | 100% of critical metric types; configurable for non-critical |
| Model training throughput | Re-train CPU-based models for all series within 24 hours |
| Scoring throughput | Score 12M+ time-series at 15-second resolution in real-time |
| System availability | 99.9% for scoring pipeline; 99% for training pipeline |
| Model staleness | Models retrained at least every 7 days; immediate retraining on detected drift |

### Constraints & Assumptions
- Primary metrics source: Prometheus. Fleet: 10K servers × ~200 metrics + 100K pods × ~50 metrics + 50K service instances × ~100 metrics = **12.15M active time series**.
- Historical data: 1 year available in Thanos/VictoriaMetrics for training.
- Compute budget: 20 GPU nodes (V100 16GB) for LSTM. 50 CPU nodes (32 cores each) for statistical/Prophet/ARIMA.
- Anomaly events feed into Alertmanager via a custom webhook receiver (not Prometheus alert rules).
- Java services emit metrics via Micrometer; Python services via `prometheus_client`.
- Engineering team: 5 ML/DS engineers for models, 2 SREs for pipeline infrastructure.

### Out of Scope
- Log anomaly detection (handled by Kibana Alerting on log patterns).
- Trace-based anomaly detection (handled by SLO burn rate alerting).
- Business KPI anomaly detection (revenue, conversion rate) — separate ML platform.
- Predictive capacity planning (separate system, shares models).

---

## 2. Scale & Capacity Estimates

### Metric Volume for Anomaly Scoring

| Metric Type | Series Count | Scrape Interval | Data Points/sec |
|---|---|---|---|
| Node-level (CPU, memory, disk, net) | 10K nodes × 200 = 2M series | 15s | 133,333 dp/sec |
| Kubernetes pods (container resources) | 100K pods × 50 = 5M series | 15s | 333,333 dp/sec |
| Service application metrics | 50K instances × 100 = 5M series | 15s | 333,333 dp/sec |
| Database metrics (MySQL, ES, Redis) | 500 instances × 100 = 50K series | 30s | 1,667 dp/sec |
| Network metrics | 2K devices × 50 = 100K series | 60s | 1,667 dp/sec |
| **Total** | **~12.15M series** | mixed | **~803K dp/sec** |

### Model Storage Requirements

| Model Type | Coverage | Size per Series | Total Storage |
|---|---|---|---|
| Z-score (EWMA params) | All 12.15M series | 40 bytes (mean, variance, threshold) | 486 MB |
| Prophet (trained model) | Top 10% = 1.215M series | ~50 KB | **60 GB** |
| LSTM (weights) | Top 1% = 121.5K series | ~5 MB | 607 GB (GPU model server) |
| ARIMA (params) | 500K trending series | 2 KB | 1 GB |

### Training Compute

- **Z-score/ARIMA (CPU):** ARIMA: ~10 sec/series × 500K = 5M sec / (50 nodes × 32 cores) = **52 minutes**. Feasible daily.
- **Prophet (CPU):** ~1 sec/series × 1.215M = 1.215M sec / (50 nodes × 32 cores) = **12.5 minutes**. Feasible daily.
- **LSTM (GPU):** ~2 hours/model × 121.5K / 20 GPUs = impractical daily. **Weekly retraining** with transfer learning (pre-trained base model fine-tuned per series in ~10 minutes → 121.5K / 20 GPUs × 10 min = ~60,750 minutes / 60 = 1,012 GPU-hours → still requires batching over 3 days. Practical approach: retrain top 1,000 highest-priority LSTM models per day, full rotation in ~4 months; use transfer learning from a shared backbone.

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                        METRIC DATA SOURCES                                       │
│  Prometheus + Thanos: 12.15M time series, 1 year historical data                │
│  Remote Write → Kafka "metrics-raw" topic at 803K data points/sec               │
└──────────────────────────────────┬───────────────────────────────────────────────┘
                                   │
         ┌─────────────────────────┴────────────────────────────┐
         │                                                      │
         ▼                                                      ▼
┌──────────────────────────────────┐     ┌────────────────────────────────────────────┐
│   TRAINING PIPELINE              │     │    REAL-TIME SCORING ENGINE (Apache Flink) │
│   Scheduled: nightly 02:00 UTC   │     │    Consumes: Kafka "metrics-raw"           │
│   Compute: 50 CPU + 20 GPU nodes │     │    50 TaskManagers (4 CPU, 8 GB each)      │
│                                  │     │                                            │
│  Step 1: Feature Engineering     │     │  TIER 1: All 12.15M series                 │
│    - 90-day historical windows   │     │    Streaming Z-score / MAD                 │
│    - STL seasonal decomposition  │     │    O(1) per data point, ~10 μs             │
│    - Differencing, scaling       │     │                                            │
│    - Outlier detection           │     │  TIER 2: Top 10% (1.215M series)           │
│                                  │     │    Prophet scoring                         │
│  Step 2: Model Training          │     │    Pre-loaded from Model Registry          │
│    - Z-score: all series         │     │    (Redis LRU cache in Flink state)        │
│    - Prophet: 1.215M series      │     │                                            │
│    - ARIMA: 500K trending        │     │  TIER 3: Top 1% (121.5K series)            │
│    - LSTM: 121.5K critical       │     │    LSTM inference via Triton GPU server    │
│                                  │     │    gRPC call, < 5ms latency                │
│  Step 3: Validation              │     │                                            │
│    - Holdout evaluation          │     │  OUTPUT: AnomalyScore [0.0, 1.0]           │
│    - Precision/recall metrics    │     │    per (series_key, timestamp)             │
│    - Champion/challenger compare │     │                                            │
│                                  │     └──────────────────────┬───────────────────┘
│  Step 4: Model Registry Update   │                            │
│    MLflow metadata + Redis store ◄─────────────────────────── │ Model sync
└──────────────────────────────────┘                            │
                                                                ▼
                                         ┌────────────────────────────────────────────┐
                                         │    CORRELATION ENGINE                      │
                                         │                                            │
                                         │  - Buffer anomaly scores (5-min window)    │
                                         │  - DBSCAN temporal + label clustering      │
                                         │  - Causal graph lookup (from trace data)   │
                                         │  - Root cause candidate ranking            │
                                         │                                            │
                                         │  OUTPUT: AnomalyEvent (grouped, explained) │
                                         └──────────────────┬─────────────────────────┘
                                                            │
                    ┌───────────────────────────────────────┼──────────────────────────┐
                    │                                       │                          │
                    ▼                                       ▼                          ▼
     ┌──────────────────────────┐        ┌──────────────────────────┐  ┌──────────────────────┐
     │  ALERTMANAGER WEBHOOK    │        │  ANOMALY EVENT STORE     │  │  FEEDBACK UI         │
     │  Converts to Alert format│        │  PostgreSQL (structured)  │  │  Slack button / API  │
     │  Routes: PagerDuty/Slack │        │  Elasticsearch (search)  │  │  TP/FP labels        │
     └──────────────────────────┘        └──────────────────────────┘  │  → retraining queue  │
                                                                        └──────────────────────┘
```

**Data pipeline flow:**
1. Prometheus remote-writes metrics to Kafka `metrics-raw` (803K dp/sec) and Thanos (long-term).
2. Training pipeline reads 90-day history from Thanos nightly; trains/updates models; stores in MLflow registry and Redis.
3. Flink scoring engine reads from `metrics-raw`, loads models from Redis LRU cache, scores each data point in three tiers.
4. Anomaly scores above threshold flow to the Correlation Engine (5-minute buffering window).
5. Correlation Engine clusters related anomalies, applies causal graph analysis, generates structured AnomalyEvent.
6. AnomalyEvent → Alertmanager webhook (for paging) + PostgreSQL (for history/feedback).
7. Engineer feedback → feedback database → triggers retraining for affected series.

---

## 4. Data Model

### Anomaly Event Schema (JSON, stored in PostgreSQL + Elasticsearch)

```json
{
  "anomaly_id": "anom-2026-04-10-14-23-45-bm-rack03-cpu",
  "detected_at": "2026-04-10T14:23:45Z",
  "metric": {
    "name": "node_cpu_utilization",
    "labels": {
      "instance": "bm-rack03-slot-17",
      "cluster": "prod-cluster-1",
      "rack": "rack03",
      "job": "node-exporter"
    },
    "current_value": 97.3,
    "unit": "percent"
  },
  "anomaly_score": 0.94,
  "confidence": 0.89,
  "severity": "critical",
  "detection_method": "prophet",
  "baseline": {
    "predicted_value": 45.2,
    "predicted_lower_bound": 32.1,
    "predicted_upper_bound": 58.3,
    "historical_mean": 43.8,
    "historical_stddev": 8.4,
    "z_score_equivalent": 6.3
  },
  "context": {
    "seasonality_adjusted": true,
    "time_of_week": "Thursday_afternoon",
    "similar_anomalies_last_30d": 2,
    "last_similar_anomaly_at": "2026-03-18T09:15:00Z"
  },
  "correlated_anomalies": [
    {
      "metric": "node_memory_utilization",
      "instance": "bm-rack03-slot-17",
      "anomaly_score": 0.82,
      "time_offset_seconds": -15
    },
    {
      "metric": "http_request_duration_p99",
      "service": "job-scheduler",
      "anomaly_score": 0.71,
      "time_offset_seconds": 30
    }
  ],
  "causal_hypothesis": "CPU spike on bm-rack03-slot-17 preceded memory pressure and downstream latency increase in job-scheduler. Root cause likely: runaway process on bare-metal host.",
  "recommended_runbook": "https://wiki.internal/runbooks/node-cpu-spike",
  "feedback": null
}
```

### Dynamic Threshold Model Parameters (per series, in Redis)

```json
{
  "series_key": "node_cpu_utilization{instance='bm-rack03-slot-17'}",
  "model_type": "prophet",
  "model_version": "2026.04.07.v3",
  "trained_at": "2026-04-07T02:15:00Z",
  "training_window_days": 90,
  "prophet_serialized_params": "<base64-encoded Prophet model>",
  "residual_stats": {
    "mean": 0.0,
    "std": 5.2,
    "mad": 3.8,
    "p95": 9.1,
    "p99": 14.3
  },
  "anomaly_thresholds": {
    "score_threshold": 0.80,
    "z_score_threshold": 4.0,
    "min_consecutive_anomalous_points": 3
  },
  "drift_detection": {
    "cusum_state": {"s_pos": 0.12, "s_neg": 0.0},
    "last_drift_detected": "2026-04-01T00:00:00Z",
    "retraining_triggered": false
  },
  "circuit_breaker": {
    "active": false,
    "fp_rate_last_100_feedbacks": 0.08
  }
}
```

### PostgreSQL Schema

```sql
CREATE TABLE anomaly_events (
    anomaly_id    TEXT PRIMARY KEY,
    series_key    TEXT NOT NULL,
    detected_at   TIMESTAMPTZ NOT NULL,
    severity      TEXT NOT NULL,
    anomaly_score FLOAT NOT NULL,
    confidence    FLOAT NOT NULL,
    method        TEXT NOT NULL,
    event_json    JSONB NOT NULL,
    resolved_at   TIMESTAMPTZ,
    incident_id   TEXT
);

CREATE INDEX idx_ae_series      ON anomaly_events(series_key, detected_at DESC);
CREATE INDEX idx_ae_severity    ON anomaly_events(severity, detected_at DESC);
CREATE INDEX idx_ae_unresolved  ON anomaly_events(resolved_at) WHERE resolved_at IS NULL;

CREATE TABLE anomaly_feedback (
    feedback_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    anomaly_id           TEXT NOT NULL REFERENCES anomaly_events(anomaly_id),
    feedback_type        TEXT NOT NULL CHECK (feedback_type IN (
                           'true_positive', 'false_positive', 'informational'
                         )),
    feedback_by          TEXT NOT NULL,  -- SSO identity
    feedback_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    notes                TEXT,
    retraining_triggered BOOLEAN DEFAULT FALSE
);
```

---

## 5. API Design

### Anomaly Query API (REST)

```
# Get anomalies for a service with filters
GET /api/v1/anomalies?service=job-scheduler
  &start=2026-04-10T00:00:00Z&end=2026-04-10T23:59:59Z
  &severity=critical&min_confidence=0.8
Response 200:
{
  "anomalies": [
    {
      "anomaly_id": "anom-2026-04-10-14-23-45-bm-rack03-cpu",
      "metric_name": "node_cpu_utilization",
      "instance": "bm-rack03-slot-17",
      "anomaly_score": 0.94,
      "confidence": 0.89,
      "severity": "critical",
      "detected_at": "2026-04-10T14:23:45Z",
      "status": "open",
      "correlated_count": 2
    }
  ],
  "total": 1
}

# Get dynamic threshold for a metric at a specific time
GET /api/v1/thresholds?metric=node_cpu_utilization&instance=bm-rack03-slot-17
  &time=2026-04-10T14:00:00Z
Response 200:
{
  "series_key": "node_cpu_utilization{instance='bm-rack03-slot-17'}",
  "predicted_value": 45.2,
  "lower_bound": 32.1,
  "upper_bound": 58.3,
  "anomaly_score_at_value": {
    "50.0": 0.02,
    "75.0": 0.45,
    "90.0": 0.85,
    "97.3": 0.94
  },
  "model_type": "prophet",
  "model_age_hours": 67
}

# Submit feedback
POST /api/v1/anomalies/{anomaly_id}/feedback
{
  "feedback_type": "false_positive",
  "notes": "Load test was running at this time",
  "trigger_retraining": true
}
Response 204 No Content

# Model health overview
GET /api/v1/models/health
Response 200:
{
  "total_series": 12150000,
  "models_trained": 12143200,
  "models_stale_gt_7d": 6800,
  "false_positive_rate_7d": 0.031,
  "circuit_breakers_active": 12,
  "avg_confidence": 0.84
}
```

---

## 6. Core Component Deep Dives

### 6.1 Detection Algorithms

**Why this is hard:** Infrastructure metrics are not simple Gaussian processes. They exhibit: (1) Multi-period seasonality — daily, weekly, sometimes monthly cycles. (2) Trend — disk usage grows monotonically until cleanup. (3) Step changes — workload reassignment permanently shifts baseline. (4) Noise — natural variance in normal operation. (5) Concept drift — patterns change as the business evolves. No single algorithm handles all of these efficiently at 12M+ series.

**Algorithm comparison:**

| Algorithm | Seasonality | Trend | Training Cost | Inference | Best For |
|---|---|---|---|---|---|
| Z-score (EWMA) | No | No | Trivial O(1) | O(1), ~10μs | All series initial screening |
| MAD | No | No | Trivial O(1) | O(1), ~10μs | Robust to outliers in history |
| Holt-Winters (ETS) | Single period | Yes | Low (seconds) | O(1) | Regular seasonal metrics |
| ARIMA | Limited | Yes | Medium (10s/series) | O(window) | Non-seasonal trending metrics |
| Prophet | Multi-period | Yes | Medium (1–5 min) | O(1) after training | Complex seasonality, holidays |
| LSTM | Learned | Yes | High (2h/model GPU) | O(1) after training | Complex nonlinear patterns |

**Streaming Z-score with EWMA (implemented in Flink operator):**

```python
class StreamingZScore:
    """Exponentially weighted moving average Z-score for Flink streaming."""
    def __init__(self, alpha: float = 0.05, threshold: float = 4.0):
        self.alpha = alpha          # Decay: smaller = longer memory
        self.threshold = threshold  # Sigma threshold for anomaly classification
        self.mean: float | None = None
        self.variance: float = 0.0

    def update(self, value: float) -> tuple[float, float]:
        """Returns (z_score, confidence [0.0, 1.0])."""
        if self.mean is None:
            self.mean, self.variance = value, 0.0
            return 0.0, 0.0

        delta = value - self.mean
        self.mean     += self.alpha * delta
        self.variance  = (1 - self.alpha) * (self.variance + self.alpha * delta**2)

        if self.variance < 1e-10:
            return 0.0, 0.0

        import math
        z = abs(delta) / (self.variance ** 0.5)
        # Sigmoid maps z-score to [0,1] centered at threshold
        confidence = 1.0 / (1.0 + math.exp(-(z - self.threshold)))
        return z, confidence
```

**Why MAD instead of stddev:** Standard deviation inflates when outliers (past anomalies) are in the training window, reducing sensitivity. MAD uses `median(|Xi - median(X)|)` — robust to outliers. Convert to σ-equivalent: `σ_robust ≈ 1.4826 × MAD`. Use MAD when historical data contains genuine past anomalies that should not widen the "normal" range.

**Prophet training and inference:**

```python
from prophet import Prophet
import pandas as pd

def train_prophet(df: pd.DataFrame) -> Prophet:
    """df columns: ds (datetime), y (float metric value)."""
    m = Prophet(
        changepoint_prior_scale=0.05,      # Lower = less flexible trend
        seasonality_prior_scale=10.0,      # Allow strong seasonal signals
        seasonality_mode='multiplicative', # % metrics scale with absolute level
        yearly_seasonality=True,
        weekly_seasonality=True,
        daily_seasonality=True,
        interval_width=0.99,               # 99% CI for anomaly bounds
    )
    m.add_seasonality(name='4h_batch', period=1.0/6, fourier_order=5)
    m.fit(df)
    return m

def score_point(m: Prophet, value: float, ts: pd.Timestamp) -> dict:
    f = m.predict(pd.DataFrame({'ds': [ts]}))
    yhat, lo, hi = f['yhat'].iloc[0], f['yhat_lower'].iloc[0], f['yhat_upper'].iloc[0]

    if value > hi:
        deviation = (value - hi) / max(hi - yhat, 1e-6)
    elif value < lo:
        deviation = (lo - value) / max(yhat - lo, 1e-6)
    else:
        deviation = 0.0

    return {
        'predicted': yhat, 'lower': lo, 'upper': hi,
        'anomaly_score': min(1.0, deviation / 2.0),
        'is_anomaly': deviation > 0,
    }
```

**LSTM architecture for top 1% critical series:**

```python
import torch, torch.nn as nn

class InfraLSTM(nn.Module):
    """
    Input: 60-point sliding window (15 min at 15s resolution)
    Output: predicted next value + uncertainty (mean, std)
    """
    def __init__(self, hidden: int = 64, layers: int = 2, dropout: float = 0.2):
        super().__init__()
        self.lstm = nn.LSTM(1, hidden, layers, dropout=dropout, batch_first=True)
        self.fc_mean    = nn.Linear(hidden, 1)
        self.fc_log_std = nn.Linear(hidden, 1)

    def forward(self, x: torch.Tensor):
        out, _ = self.lstm(x)
        h = out[:, -1, :]  # Last timestep
        mean = self.fc_mean(h)
        std  = torch.exp(self.fc_log_std(h)).clamp(min=1e-6)
        return mean, std  # Returns distribution: N(mean, std²)
```

Training: 80/20 train/validation split on 90-day history at 15s resolution. Huber loss (robust to outliers in training data). ~2 hours per model on V100. Batch GPU inference: < 1ms per series.

**Q&As:**

**Q: Why use MAD instead of standard deviation for Z-score calculation?**
A: Standard deviation is non-robust — the 3 genuine past anomalies in a 1,000-point history inflate σ, reducing sensitivity for future anomalies. MAD = `median(|Xi - median(X)|)` is unaffected by a few outliers; 3 anomalies in 1,000 points barely change the median. Convert to σ-equivalent with Gaussian scaling: `σ_robust = 1.4826 × MAD`. Use MAD when: (1) historical data contains genuine past anomalies that should not widen the "normal" bounds, (2) the metric has heavy-tailed distributions (e.g., tail latency percentiles), (3) you want conservative thresholds that don't drift upward due to past incidents.

**Q: How do you handle concept drift — when workload patterns permanently change?**
A: Detect drift with CUSUM (Cumulative Sum Control Chart) on the model residual (actual minus predicted). CUSUM accumulates evidence for a mean shift: `S_pos = max(0, S_pos + residual - δ)` where δ is a tolerance. When S_pos > λ (threshold), drift is declared. On drift: (1) Mark model stale, (2) Trigger retraining on recent 30-day window (faster adaptation), (3) Fall back to Z-score with 7-day rolling window until new model is ready. Concept drift parameters: δ = 0.5×expected_noise_std, λ = 5×expected_noise_std — calibrated to detect shifts larger than natural noise with minimal false drift detections.

**Q: How does Prophet's changepoint detection work?**
A: Prophet automatically identifies changepoints — times when the trend changes slope. It places 25 potential changepoints in the first 80% of training data and uses L1-regularization (`changepoint_prior_scale`) to determine which to retain. When a bare-metal host is reassigned from a 20% CPU workload to a 60% CPU workload, Prophet detects this as a changepoint and uses only post-changepoint data for future trend extrapolation — giving accurate predictions without being biased by the old (lower) baseline. Without changepoint detection, the model averages old and new baselines, predicting ~40% and generating false positives in both regimes.

**Q: What is the cold-start problem and how do you handle it for new services?**
A: New services have no history to train models on. Strategy: (1) Z-score with conservative threshold (Z > 5, not 4) for the first 7 days — wide threshold prevents false positives during the learning period. (2) Transfer learning: find the most similar existing service by DTW (Dynamic Time Warping) distance on the first 24 hours of data, use its model with adjusted level parameters as a warm start. (3) After 7 days: train a full Prophet model. After 30 days: include in LSTM training candidates. During bootstrapping, suppress anomaly confidence below 0.7 to avoid premature paging.

**Q: How does the LSTM handle missing data points in the input sequence?**
A: Missing data (collection failures, pod restarts) in the 60-point input window: (1) **Forward-fill**: substitute last known value — simple but doesn't encode "data was missing." (2) **Prophet imputation**: use the trained Prophet model to predict what the value would have been — preserves realistic variance. (3) **Masking**: pass a binary mask tensor alongside values; LSTM learns to down-weight imputed positions. For gaps > 5 consecutive points (75 seconds), mark the model output as `confidence=0` for the next 60 points (15 minutes) — the LSTM's hidden state is contaminated by imputed values and cannot reliably predict.

**Q: How do you tune the Prophet prediction interval from 99% to the right value for each service?**
A: The interval width (99%, 97%, 95%) controls FP/FN tradeoff. Start at 99% (conservative). After 2 weeks: if FP rate > 10%, widen to 99.5% or add post-detection smoothing (require 3 consecutive anomalous points instead of 1). If FP rate < 1% but real incidents were missed: narrow to 97%. Use the feedback database to compute per-service optimal thresholds automatically:
```sql
SELECT series_key, 
       COUNT(*) FILTER (WHERE feedback_type = 'false_positive') AS fps,
       COUNT(*) FILTER (WHERE feedback_type = 'true_positive')  AS tps,
       fps::FLOAT / NULLIF(fps + tps, 0) AS fp_rate
FROM anomaly_feedback
WHERE feedback_at > NOW() - INTERVAL '30 days'
GROUP BY series_key
HAVING fps + tps >= 10
ORDER BY fp_rate DESC;
```
Series with FP rate > 10%: widen interval. Series with FP rate < 1% and known missed incidents: narrow.

### 6.2 Correlation Engine

**Why it is hard:** During a real incident, dozens to hundreds of metrics anomalize simultaneously. A database connection pool exhaustion causes: latency spikes in calling services, error rate increases, pod restarts from health check failures, resource pressure from retry storms, cascading to 20+ downstream services. The correlation engine must: (1) group all related anomalies into a single incident, (2) rank root causes vs symptoms, and (3) do this in real time with < 5-minute latency at 803K dp/sec ingest rate.

**Temporal clustering (DBSCAN):**

```python
from sklearn.cluster import DBSCAN
import numpy as np

def cluster_anomalies(events: list[dict], time_window_sec: int = 300) -> list[list]:
    """Group anomaly events into incident clusters."""
    if len(events) < 2:
        return [[e] for e in events]  # Single-event "clusters"

    # Feature vector: normalized (time, cluster_hash, service_hash)
    features = np.array([
        [
            e['detected_at'].timestamp() / time_window_sec,
            hash(e['labels'].get('cluster', 'none')) % 1000 / 1000.0,
            hash(e['labels'].get('service', 'none'))  % 1000 / 1000.0,
        ]
        for e in events
    ])

    # eps=0.3: anomalies within 90 sec and same cluster/service are neighbors
    # min_samples=2: need at least 2 anomalies to form a cluster
    labels = DBSCAN(eps=0.3, min_samples=2).fit(features).labels_
    clusters: dict[int, list] = {}
    for idx, label in enumerate(labels):
        if label != -1:  # -1 = noise (isolated, not clustered)
            clusters.setdefault(label, []).append(events[idx])

    return list(clusters.values())
```

**Root cause ranking within a cluster:**

```python
def rank_root_causes(cluster: list[dict], causal_graph: dict) -> list[dict]:
    """
    causal_graph: {service_A: [service_B, service_C]} 
    meaning A's anomalies historically precede B's and C's anomalies.
    """
    scored = []
    min_time = min(e['detected_at'] for e in cluster)

    for event in cluster:
        time_score      = 1.0 - (event['detected_at'] - min_time).seconds / 300.0
        downstream_deps = len(causal_graph.get(event['labels'].get('service',''), []))
        causal_score    = downstream_deps / max(len(causal_graph), 1)
        anomaly_score   = event['anomaly_score']

        composite = (0.4 * time_score + 0.4 * causal_score + 0.2 * anomaly_score)
        scored.append({**event, 'root_cause_score': composite})

    return sorted(scored, key=lambda x: x['root_cause_score'], reverse=True)
```

**Q&As:**

**Q: How do you validate the correlation engine finds real correlations, not spurious ones?**
A: (1) **Historical replay**: run the engine against 6 months of historical anomaly data; compare its incident groupings against manually-verified incident post-mortems. Measure precision and recall. Target: precision > 0.80, recall > 0.90. (2) **Synthetic injection**: inject artificial correlated anomalies (CPU spike on 10 nodes in same rack simultaneously) and verify correct grouping. (3) **Expert rating**: sample 100 correlation outputs quarterly, have SREs rate them 1–5 for relevance. Target mean score > 3.5. (4) **Causal graph validation**: periodically compare auto-generated causal graph (from trace co-occurrence) against manually-maintained service dependency maps. Alert on divergence > 20%.

**Q: What happens when two unrelated incidents coincide in time?**
A: DBSCAN uses infrastructure label hashing as features alongside timestamp. Two incidents from different clusters (different `cluster` and `service` labels) have very different feature vectors even at similar timestamps — DBSCAN should not group them. If it does (due to timestamp proximity and hash collision): the correlation engine cross-checks the causal graph. If the two grouped services have no historical causal relationship, `correlation_confidence` is set to 0.35 (low, threshold is 0.70 for high confidence). The alert notification includes: "These anomalies may be unrelated concurrent incidents — verify independently."

### 6.3 Dynamic Threshold Visualization (Grafana)

Three bands rendered over the raw metric time series:
- **Green band:** Prophet's 99% prediction interval (`yhat_lower` to `yhat_upper`) — the "expected normal range."
- **Yellow band:** 1×–2× outside the green band — "warning zone," anomaly score 0.4–0.8.
- **Red zone:** > 2× outside the green band — "anomaly zone," anomaly score > 0.8.

The raw metric is plotted as a line. When it exits the green band, the line color changes. Engineers immediately understand WHY the alert fired: "CPU is 97.3% but the model predicted 45.2% ± 13.1% for Thursday afternoons — this is a 4-sigma event." This visual explainability, not model internals, drives engineer trust and adoption.

---

## 7. Scheduling & Resource Management

### Model Training Scheduling (Apache Airflow DAG)

**Nightly retraining (02:00 UTC, SLA: complete by 08:00 UTC):**

Priority queue for which series to retrain first:
1. **P1 (immediate):** Series where CUSUM drift detected in last 24 hours.
2. **P2 (high):** Series with ≥ 3 false positive feedbacks in last 24 hours.
3. **P3 (normal):** Series not retrained in 7+ days.
4. **P4 (background):** All remaining (scheduled rotation — all series retrained within 30 days).

Airflow dynamic task mapping: each series is a task, distributed across 50 CPU worker nodes (32 cores each). Airflow Pool limits: `prophet_training_pool` = 1,600 slots (50 × 32); `lstm_training_pool` = 20 slots (one per GPU).

**Triggered retraining on feedback:**
When ≥ 3 false positive feedbacks arrive for the same series within 24 hours → trigger immediate single-series Prophet retraining (~1 minute). New model deployed to Redis within 5 minutes. Flink operators' LRU cache refreshes on next prediction request for that series (within 15 seconds at 15s scrape interval).

### Scoring Engine (Flink) Resource Allocation

50 TaskManagers × 4 CPU cores = 200 CPU cores. Load distribution:
- Z-score (12.15M series × 803K dp/sec): 10 μs/point → 100K points/sec/core → 803K/100K = 8 cores total.
- Prophet (1.215M series): 100 μs/point → 10K points/sec/core → 80.3K/10K = 8 cores total.
- Overhead, Kafka consumer, state management: remaining cores.

GPU scoring (LSTM): 10 dedicated GPU TaskManagers (A100), each handling ~12K series. Triton model server co-located with GPU nodes, batching 256 inference requests per GPU call.

---

## 8. Scaling Strategy

### Scoring Pipeline (Flink)

Pure horizontal scale: Kafka partition count = Flink parallelism. At current 803K dp/sec with 50 TaskManagers (16,060 dp/sec per TM): comfortable. If ingest doubles to 1.6M dp/sec: double partitions (64 → 128), double TaskManagers (50 → 100). No algorithmic changes.

### Model Registry (Redis)

Current: 10 Redis nodes × 32 GB = 320 GB. Prophet models for top 10% = 60 GB (with LZ4 compression 2×: 30 GB stored). Z-score params: 290 MB. Total < 31 GB — fits in current Redis with headroom. At 10× scale (120M series): top 10% = 12M series × 50KB / 2× compression = 300 GB → scale to 100 Redis nodes × 32 GB = 3.2 TB. Or use tiered Redis: hot cache (recent 100K models) on high-memory instances, warm tier (all Prophet models) on SSD-backed Redis, cold tier (Z-score params only) on standard Redis.

### Training at 10× Scale (120M series)

Prophet training: 12M series × 1s = 12M seconds / (500 nodes × 32 cores) = 750 seconds = 12.5 minutes. Feasible with 10× workers (500 nodes instead of 50). Alternatively: reduce Prophet coverage from 10% to 5% of series, focusing compute on highest-value series — 6M series × 1s / (50 nodes × 32 cores) = 62.5 minutes. Feasible in the 6-hour nightly window.

---

## 9. Reliability & Fault Tolerance

| Failure Scenario | Impact | Mitigation | Recovery Time |
|---|---|---|---|
| Flink TaskManager crash | Scoring stops for affected Kafka partitions | Flink checkpoints every 30s; auto-restart from last checkpoint state | < 60 seconds |
| Redis (Model Registry) unavailable | Scoring falls back to Flink local LRU cache (stale models) | Alert if cache age > 24h; Z-score requires no Registry | Automatic on Redis recovery |
| Training pipeline failure | Models grow stale | Alert if model age > 10 days; Z-score always available as fallback | Next nightly run |
| Kafka consumer lag | Anomaly detection latency increases | Monitor `kafka_consumer_lag_sum`; HPA scales Flink TaskManagers | < 5 minutes |
| False positive storm (broken model) | On-call flooded with bogus alerts | Circuit breaker: disable model if FP rate > 50% in last 100 feedbacks; alert: "Model circuit-broken" | Manual investigation + retraining |
| Triton GPU server failure | LSTM scoring unavailable | Fall back to Prophet for LSTM-tier series (pre-loaded); LSTM is performance enhancement, not sole detector | Immediate fallback; Triton restored via K8s in < 2 min |

**Circuit breaker logic:**
```python
def check_circuit_breaker(series_key: str, redis_client) -> bool:
    """Returns True if model should be bypassed (circuit open)."""
    model_state = redis_client.get(f"model:{series_key}")
    if not model_state:
        return False  # No model = use Z-score (always open)
    state = json.loads(model_state)
    cb = state.get('circuit_breaker', {})
    fp_rate = cb.get('fp_rate_last_100_feedbacks', 0.0)
    return cb.get('active', False) or fp_rate > 0.50
```

---

## 10. Observability of the Observability System

```prometheus
# Scoring throughput (should equal metric ingest rate)
rate(anomaly_scorer_processed_points_total[5m])          # target = 803K dp/sec

# Anomaly event generation rate by severity
rate(anomaly_events_generated_total{severity="critical"}[5m])
rate(anomaly_events_generated_total{severity="warning"}[5m])

# False positive rate from feedback (rolling 7 days per model type)
anomaly_model_false_positive_rate{model_type="prophet"}   # target < 0.05
anomaly_model_false_positive_rate{model_type="zscore"}    # target < 0.10

# Model staleness: fraction of models older than 7 days
anomaly_models_stale_ratio                                 # target < 0.001

# Circuit breakers active (models disabled due to high FP rate)
anomaly_circuit_breakers_active_total                      # target 0

# Kafka consumer lag for scoring pipeline
kafka_consumer_lag_sum{group="anomaly-scorer", topic="metrics-raw"}

# Flink job health
flink_job_restart_total{job_name="anomaly-scorer"}         # target 0 per day
flink_checkpoint_duration_seconds{job_name="anomaly-scorer", quantile="0.99"}
```

**Grafana dashboard — "Anomaly Detection Health":**
- Scoring throughput vs metric ingest rate (gap = backlog = detection latency).
- Anomaly event rate by severity (heatmap).
- False positive rate trend (7-day rolling, by model type and service).
- Model staleness distribution (histogram of model ages).
- Circuit breaker status table (broken models, FP rates).
- Kafka consumer lag.
- Training pipeline completion time trend.

---

## 11. Security

**Metric access control:** Dedicated read-only service account for Prometheus/Thanos access. Principle of least privilege: anomaly detection system cannot modify metrics or create Prometheus alert rules.

**Model registry security:** Model parameters stored in Redis with AES-256 encryption at rest. Access controlled by network policy (only scoring engine and training pipeline can read/write Redis). Model parameters may reveal sensitive capacity information (baseline CPU levels could reveal deployment strategy).

**Model poisoning prevention:** Feedback loop trains on engineer-labeled data. Mitigations: (1) Single feedback per engineer per anomaly event (unique constraint). (2) Feedback conflicting with model confidence > 0.9 requires ≥ 3 independent engineer confirmations before triggering retraining. (3) Model sensitivity audited after each retraining: if sensitivity drops > 20% (fewer detections), human review required before deployment. (4) All feedback immutably logged with submitter SSO identity and timestamp; bulk deletion prohibited.

**PII in metrics:** Metrics should never contain PII as label values (e.g., `user_id=alice@company.com` as a metric label violates cardinality controls AND exposes PII). This is enforced by Prometheus cardinality limits (alert if any label has > 100K unique values) and code review of `@Gauge`/`@Counter` instrumentation.

---

## 12. Incremental Rollout Strategy

**Phase 1 (Month 1–2): Z-score baseline deployment**
Deploy streaming Z-score for all 12.15M series. Conservative threshold (Z > 5). Run in shadow mode for 2 weeks — generate events but do not page. Compare against actual incidents for sensitivity calibration. Measure FP rate against a 14-day incident log.

**Phase 2 (Month 3–4): Prophet for critical series + A/B test**
Train Prophet on top 100K critical series. A/B test: 50% of anomaly alerts use Z-score threshold, 50% use Prophet dynamic bounds. Measure FP rate for each approach. Expect Prophet to show 30–50% lower FP rate for seasonal metrics.

**Phase 3 (Month 5–6): Feedback loop + correlation engine**
Enable engineer feedback collection. Deploy correlation engine. Start using anomaly events as PagerDuty inputs for willing early-adopter teams (3–5 teams). Measure MTTD improvement.

**Phase 4 (Month 7–8): Full production**
Migrate all critical static threshold alerts to dynamic anomaly-based. Keep static thresholds as safety net (both must fire for highest-severity pages). Deploy LSTM for top 10K series. Establish quarterly model review cadence.

**Q&As:**

**Q: How do you prove the anomaly detection system is better than static thresholds to leadership?**
A: Controlled experiment: run both systems in parallel for 3 months. Measure: precision (% of alerts = real incidents), recall (% of real incidents detected), MTTD, on-call alert volume per shift. Static thresholds: typically high recall but low precision (many FPs). Dynamic thresholds: target higher precision with maintained recall. If dynamic system reduces weekly alert volume 30% while maintaining incident detection rate — quantify: 30 fewer on-call interruptions/week × $100 engineer-time/interruption = $3,000/week = $156,000/year for one team alone.

**Q: What do you do when the anomaly system becomes unavailable during an incident?**
A: Defense in depth: (1) Prometheus + Alertmanager static threshold alerts continue independently. (2) Flink scoring degrades gracefully via checkpointing and model cache fallback to Z-score. (3) Meta-monitoring fires within 5 minutes if scoring throughput drops below 80% of expected. (4) During system outage: on-call uses dashboards and manual threshold evaluation; core alerting remains operational. The anomaly system is an enhancement, not a replacement for static threshold alerting.

**Q: How do you handle expected spikes (end-of-month billing batch, annual Black Friday)?**
A: Two mechanisms: (1) **Prophet holidays**: add known recurring events as holiday regressors. Prophet learns the expected metric change and won't flag it as anomalous. For annual events: add them to the `holidays` DataFrame before training. (2) **Alertmanager silences**: for one-time events, create a time-bounded silence for the affected services' anomaly alerts. The anomaly system still records events (data preserved), but notifications are suppressed. Silence is created programmatically when the event ticket is created in the CMDB.

**Q: How do you handle anomaly detection for short-lived Kubernetes pods (pod lifecycle < 5 min)?**
A: Short-lived pods have insufficient per-pod history. Solutions: (1) Run anomaly detection on service-level aggregated metrics (`sum(metric) by (service_name)`) — aggregate has stable history regardless of pod churn. (2) For batch jobs: use job template labels to match against models trained on previous runs of the same job type. (3) Pod-level anomaly detection is unnecessary for most use cases — service-level aggregation catches the same anomalies (CPU spike on 1 pod = service-level CPU spike if that pod handles significant traffic).

**Q: How do you measure false negative rate (missed incidents) when you have no ground truth labels?**
A: Three approaches: (1) **Retrospective incident correlation**: after each incident is resolved, check whether the anomaly system detected it within 5 minutes. Use PagerDuty incident data as ground truth. (2) **Red team exercises**: periodically inject synthetic failures (CPU spike via stress tool on a non-critical host) and verify the anomaly system detects them within the SLA (5 minutes). (3) **Holdout evaluation during training**: hold out the last 14 days of training data; compare anomaly system predictions against known anomaly labels (manually annotated or derived from incident records). Track FN rate trend over time — if increasing, model quality is degrading.

---

## 13. Trade-offs & Decision Log

| Decision | Option A (Chosen) | Option B | Reason |
|---|---|---|---|
| Primary seasonal algorithm | Prophet | ARIMA + Fourier | Prophet: native multi-period seasonality, no stationarity assumption, handles changepoints, faster to implement. ARIMA: more theoretically rigorous but manual order selection per series at 12M scale is infeasible. |
| Model granularity | Per-series models | Per-metric-type global model | Per-series captures host-specific workload patterns (bm-rack03 genuinely differs from bm-rack01). Global model: simpler, less storage but misses per-host variation. 60 GB storage cost justified by accuracy. |
| Inference architecture | Flink LRU cache + Triton GPU server | Model server only | Flink LRU: zero network latency for cached models (~95% hit rate), tolerates model server outages. Triton: needed for LSTM GPU batching efficiency. Hybrid approach used. |
| Feedback loop latency | Near-realtime (< 1 hour on trigger) | Batch nightly only | Near-realtime essential when model is clearly wrong during active incident. Batch sufficient for systematic improvement. Both implemented. |
| Correlation approach | Graph-based + temporal clustering | Pure temporal clustering | Causal graph eliminates ~40% spurious temporal correlations (co-occurring unrelated incidents). Pure temporal: simpler but higher noise. |
| Static threshold fallback | Always maintained alongside ML | ML replaces static | Defense in depth. Static catches hard faults immediately (no ML overhead). ML catches subtle degradation that static misses. Neither system alone is sufficient. |
| Training data window | 90 days | 365 days | 90 days covers full seasonal cycles (weekly clearly visible in 14 days, annual extrapolated from yearly_seasonality) while excluding very old patterns no longer representative. 365 days: better annual seasonality but 4× training data/compute. |

---

## 14. Agentic AI Integration

### AI-Powered Anomaly Explanation

When an anomaly fires, an LLM agent automatically generates a human-readable explanation:
1. Retrieves anomaly event (current value, predicted value, historical context, similar past anomalies from PostgreSQL).
2. Queries correlated anomalies from the correlation engine.
3. Fetches recent deployment events from CI/CD system API.
4. Fetches recent configuration changes from CMDB API.
5. Generates explanation: "CPU on bm-rack03-slot-17 is 97.3% — 3.7σ above the Thursday afternoon baseline of 45.2% ± 13.1%. This host runs 3 job-scheduler pods. The anomaly started at 14:23 UTC, 18 min after a new job batch was submitted at 14:05 UTC (batch ID: JB-4891). Memory pressure (+82% anomaly score) followed 15 seconds later. A similar anomaly on 2026-03-18 was caused by an unbounded SQL query in job batch JB-3201 — resolved by adding `LIMIT 10000` clause. Recommended action: check running job ID on this host and review SQL query plans."
6. Attaches explanation to PagerDuty incident and Slack notification.

### Automated Threshold Calibration Agent

Weekly LLM-powered review of false positive patterns:
1. Queries feedback database for clusters of FP anomalies by model, service, time-of-day.
2. Identifies patterns: "Prophet model for MySQL query rate generates 80% of its FPs on Monday mornings — this matches the weekly aggregation job schedule."
3. Proposes specific model fixes: "Add a Monday 08:00–10:00 seasonality exception to the MySQL query rate Prophet model."
4. Creates a PR in the model configuration Git repository with the proposed change.
5. Engineers review and approve. Approved changes auto-applied on next nightly training run.

### Incident-Driven Model Improvement Loop

After each incident closes:
1. AI agent reviews all anomaly events from the incident window.
2. Identifies which models correctly flagged the root cause (TP) and which generated noise (FP).
3. Automatically submits bulk feedback to the feedback database with notes from the incident post-mortem.
4. This accelerates model improvement without requiring manual engineer feedback for every individual anomaly event in an incident.

---

## 15. Complete Interviewer Q&A Bank

**Q1: What is the curse of dimensionality in multi-metric anomaly detection and how do you address it?**
A: When detecting anomalies across N correlated metrics simultaneously, "normal" is a hyper-sphere in N-dimensional space. As N increases, almost every point appears anomalous in at least one dimension even for genuinely normal operation (high-dimensional data is inherently sparse). Mitigation: (1) PCA or autoencoder for dimension reduction — compress N metrics to K principal components (K << N) capturing 95% of variance. Anomaly detection runs on the K-dimensional projection. (2) Metric grouping: separate models per category (CPU, memory, network) with smaller N per group. (3) Mahalanobis distance: accounts for inter-metric correlations by transforming to a space where correlated metrics are orthogonalized, measuring deviation in the decorrelated space.

**Q2: Explain STL decomposition and its role in anomaly detection.**
A: STL (Seasonal-Trend decomposition using LOESS) decomposes `y(t) = Trend(t) + Seasonal(t) + Residual(t)`. Running anomaly detection on the Residual alone produces a near-stationary series where Z-score is highly effective. A "high" CPU reading at 9am Monday is expected (seasonal component is high at that time); the same value at 3am Sunday has a high residual component — a genuine anomaly. Without decomposition, a static threshold that accommodates the Monday 9am peak is too lenient to detect the 3am anomaly. STL enables per-time-context sensitivity that static thresholds cannot achieve.

**Q3: How does Holt-Winters exponential smoothing differ from Prophet, and when do you choose each?**
A: Holt-Winters (Triple Exponential Smoothing) has three components: level, trend, seasonal — each updated with separate smoothing parameters (α, β, γ) via closed-form updates. It handles a single seasonal period efficiently. Prophet uses Bayesian Stan inference to fit trend + multiple Fourier-series seasonalities + holiday regressors simultaneously. Holt-Winters: faster training (O(n)), lower memory, handles one seasonal period well, good for simple daily seasonality. Prophet: slower training (requires Stan MCMC), handles multiple simultaneous seasonalities (daily + weekly + monthly), handles arbitrary holiday effects, but requires more compute. Choose Holt-Winters for simple, single-period seasonal metrics with tight latency/compute constraints; Prophet for complex multi-period seasonal metrics where accuracy justifies the compute cost.

**Q4: What is an isolation forest and when would you use it for infra anomaly detection?**
A: Isolation Forest partitions the feature space with random splits and measures how quickly a point can be isolated (fewer splits needed = more anomalous). For N-dimensional feature vectors, it detects points that are unusual in the combined space even if no individual dimension is extreme. Infrastructure use case: detecting anomalous server state where CPU=70%, memory=80%, disk I/O=50% individually look "normal" but this exact combination never occurred before. Compared to Prophet: Isolation Forest doesn't handle temporal context or seasonality (treats each N-dimensional data point as i.i.d.); it's ideal for snapshot-in-time multi-metric anomaly detection. For time-series with temporal structure, Prophet is superior for individual series; Isolation Forest is better for multi-metric cross-series anomaly detection at a single timestep.

**Q5: What is the Page-Hinkley test and how do you set its parameters?**
A: PH test: sequential change-point detection using two cumulative sums. `S_pos = max(0, S_pos + r_t - δ)` where `r_t = x_t - μ_expected` is the residual, δ is a minimum detectable change, and λ is the threshold for declaring drift. Parameters: δ (sensitivity = minimum drift to detect, set to ~half the acceptable drift magnitude) and λ (confidence level = amount of cumulative evidence required, larger → fewer false drift detections). For infrastructure metrics: δ = 0.5 × residual_std (detect drifts > half a standard deviation), λ = 5 × residual_std (require 5σ-equivalent cumulative evidence). At 15-second scrape interval: a drift of 1σ sustained for 75 seconds = enough cumulative evidence to trigger. The test is O(1) per data point — ideal for 803K dp/sec streaming pipeline.

**Q6: How do you handle metrics with bimodal distributions?**
A: Bimodal metrics (e.g., cache hit rate near 10% during cold start or 90% during warm state) violate Gaussian assumptions. Solutions: (1) Gaussian Mixture Model (GMM): model as a weighted sum of two Gaussians; anomaly if likelihood under BOTH modes is low. (2) Hidden Markov Model (HMM): detect which mode the system is in using state transitions, apply separate Z-score thresholds per mode. (3) Prophet with logistic growth cap: for metrics bounded at 0–100%, Prophet's logistic growth handles natural saturation — the model learns that "normal" includes both plateau values. (4) Practical approach: detect warm-up events (metric jumps > 50 percentile points within 5 minutes) and suppress anomaly detection during the 10-minute transition window.

**Q7: How do you design the scoring engine to handle backpressure when Elasticsearch is slow?**
A: The anomaly scoring pipeline should be independent of anomaly event storage latency. Architecture: (1) Scoring engine (Flink) writes anomaly events to a Kafka topic `anomaly-events`. (2) Separate consumer (anomaly-event-consumer) reads from `anomaly-events` and writes to PostgreSQL + Elasticsearch. This decouples scoring latency from storage latency. If Elasticsearch is slow: anomaly events accumulate in `anomaly-events` Kafka topic (48-hour retention); scoring continues at full speed; Elasticsearch catches up when recovered. If PostgreSQL is slow: same pattern. The alertmanager webhook has its own retry queue independent of the storage consumers.

**Q8: How does the system handle daylight saving time transitions in seasonal models?**
A: Daylight saving time transitions cause a 1-hour jump in local time, which can confuse models trained on local timestamps. Best practice: store all metric timestamps in UTC. Prophet and LSTM train on UTC timestamps — no DST confusion. If the underlying workload is human-activity-driven (office hours traffic), the DST transition will appear as a one-time seasonal shift (traffic peaks shift by 1 hour relative to UTC). Prophet handles this as a changepoint or as a holiday event. Explicitly: add the DST transition dates as 1-day "holidays" in the Prophet model to prevent the shift from being interpreted as a drift.

**Q9: What's the difference between an anomaly detection alert and a forecast alert?**
A: Anomaly detection alert: fires when the current observed value deviates significantly from what was predicted for right now (based on historical patterns). Forecast alert (predictive): fires when the predicted future value will breach a threshold (e.g., disk predicted to be full in 4 hours). Both are useful: anomaly detection catches unexpected current state; forecast alerts enable proactive remediation before a threshold is breached. In PromQL: `predict_linear(disk_free_bytes[6h], 4*3600) < 0` is a forecast alert using linear regression projection — no ML model needed. For non-linear trends (disk usage accelerating), Prophet's future prediction is more accurate than `predict_linear`.

**Q10: How do you test your anomaly detection models before deploying to production?**
A: Test pyramid: (1) **Unit tests**: mock time series with known properties (e.g., sine wave at known frequency); verify model detects injected anomalies (step changes, spikes). (2) **Integration tests with historical replay**: run the full scoring pipeline against 3 months of historical metrics; compare detections against incident records. Measure precision, recall, MTTD. (3) **Champion/challenger evaluation**: train a new model version and evaluate on a holdout set before deploying. Deploy only if new version has better FP rate or equal recall. (4) **Shadow mode deployment**: deploy new model alongside existing, compare outputs for 1–2 weeks. Alert on divergence > 20%. (5) **Chaos injection**: use `stress-ng` to create real CPU/memory spikes on a non-critical test host; verify the system detects them within 5 minutes.

**Q11: How do you handle the "recency bias" problem in EWMA-based Z-score?**
A: EWMA-based Z-score gives exponentially more weight to recent data, making it adapt quickly to level changes (good) but also making it adapt to prolonged anomalous states — if a metric stays elevated for 30 minutes, the EWMA mean gradually shifts upward, reducing the Z-score of the ongoing anomaly. Mitigation: (1) Anomaly lockout period: after detecting an anomaly, freeze the EWMA updates for 60 minutes (don't let the ongoing anomaly contaminate the baseline). (2) Bounded adaptation: `alpha = min(alpha_normal, alpha_fast)` where alpha_fast is used during normal operation and alpha_normal is used when in "anomalous state." (3) Prophet as primary for detection, Z-score as a fast screening layer — Prophet's baseline is trained on historical data and is not affected by the current anomalous period.

**Q12: How do you handle anomaly detection across multiple datacenters with different workload patterns?**
A: Each datacenter has different workload patterns due to geographic user distribution (US-West has traffic peaks at 9am PST, US-East at 9am EST). Solutions: (1) Train separate models per datacenter: `node_cpu_utilization{datacenter="us-west-1"}` and `node_cpu_utilization{datacenter="us-east-1"}` are treated as separate time series with separate Prophet models. This is the natural result of Prometheus label-based metric identity. (2) Cross-DC anomaly correlation: if both US-West and US-East show anomalies simultaneously (despite different time zones), this is a stronger signal (shared infrastructure issue, e.g., global CDN, DNS, or shared backend service). The correlation engine detects same-time anomalies across datacenters as a separate anomaly cluster type with higher priority.

**Q13: What is the computational complexity of Prophet at scale?**
A: Prophet training uses Stan's LBFGS optimizer (not MCMC by default). Complexity: O(n log n) for the FFT-based seasonality fitting, where n is the number of training data points. For 90 days at 15-second resolution: 90 × 86,400 / 15 = 518,400 data points per series. Prophet training on this resolution: ~5 minutes per series on single core. At 5-minute resolution (downsampled): 90 × 288 = 25,920 data points → ~1 minute per series. **For 1.215M series:** use 5-minute resolution for training (good enough for daily/weekly seasonality capture) and downsample Thanos data during training. This reduces training time from 5 min → 1 min per series, fitting within the 12.5-minute training window on 50×32 core nodes.

**Q14: How do you prevent the feedback loop from being exploited to blind the anomaly system?**
A: Four controls: (1) **Minimum feedback consensus**: labels conflicting with model confidence > 0.9 require ≥ 3 independent engineers before triggering retraining. (2) **Sensitivity floor**: after retraining, verify model sensitivity doesn't drop below the 6-month historical baseline by > 20%. If it does: block deployment, require human review. (3) **Feedback velocity limit**: max 50 feedbacks per engineer per day (prevents bulk false-labeling). (4) **Adversarial audit**: quarterly review of the correlation between feedback submitters and missed incidents — if a team consistently labels anomalies as FP and later has incidents that the system should have caught, investigate the feedback quality for that team.

**Q15: How would you scale to 100× current metric volume (1.2 billion time series)?**
A: At 1.2B series, per-series models are impractical (storage alone: 1.2B × 50KB = 60 PB for Prophet). Architectural pivot: (1) **Hierarchical modeling**: train per-metric-type × per-datacenter global models (not per-series). Anomaly detection = deviation from the global model for your metric type × datacenter combination. Loses per-host personalization but dramatically reduces model count to O(metric_types × datacenters). (2) **Embedding-based models**: train a single neural network that takes (series_key_embedding, timestamp_features) → expected_value. The series embedding (learned from historical data) encodes host-specific behavior in a compact vector. Anomaly detection: actual - predicted > threshold. (3) **Approximate models**: use sketch-based structures (Count-Min Sketch for frequency distribution estimation) that operate in O(1) space per series. (4) **Columnar analytics**: instead of real-time scoring of every point, run batch Spark jobs against a columnar store (ClickHouse/DuckDB) every 5 minutes, comparing recent data against a pre-computed historical distribution table.

---

## 16. References

- Facebook Prophet: "Forecasting at Scale" (Taylor & Letham, 2018) — https://peerj.com/preprints/3190/
- CUSUM change detection: Page (1954) "Continuous Inspection Schemes" — Biometrika
- "Isolation Forest" (Liu et al., 2008) — https://cs.nju.edu.cn/zhouzh/zhouzh.files/publication/icdm08b.pdf
- "A Survey on Concept Drift Adaptation" (Gama et al., 2014) — ACM Computing Surveys 46(4)
- Netflix Tech Blog on anomaly detection — https://netflixtechblog.com/
- MLflow Model Registry — https://mlflow.org/docs/latest/model-registry.html
- Apache Flink stateful streaming — https://flink.apache.org/docs/stable/concepts/stateful-stream-processing/
- VictoriaMetrics Anomaly Detection — https://docs.victoriametrics.com/anomaly-detection/
- Prophet GitHub — https://github.com/facebook/prophet
- PyTorch LSTM — https://pytorch.org/docs/stable/generated/torch.nn.LSTM.html
- STUMPY matrix profile for time-series similarity — https://stumpy.readthedocs.io/
- NVIDIA Triton Inference Server — https://docs.nvidia.com/deeplearning/triton-inference-server/
- "Robust Statistics" (Huber, 2004) — MAD and robust estimators
- Prometheus Remote Write spec — https://prometheus.io/docs/prometheus/latest/configuration/configuration/#remote_write
