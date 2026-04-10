# System Design: Predictive Autoscaling System

> **Relevance to role:** Predictive autoscaling combines ML-based time-series forecasting with infrastructure orchestration — a core competency for the Agentic AI infrastructure role. You must demonstrate understanding of ML model serving in production, feature engineering from infrastructure signals, and the critical interplay between proactive (predicted) and reactive (HPA-based) scaling. This system prevents outages caused by sudden traffic spikes while reducing over-provisioning costs.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Forecast resource demand** (CPU, memory, request rate, queue depth) for each service 15-60 minutes ahead.
2. **Generate scaling recommendations**: target replica count, instance count, or resource allocation for each forecast window.
3. **Execute proactive scaling**: scale services ahead of predicted demand spikes.
4. **Integrate with reactive scaling**: work alongside Kubernetes HPA/VPA and cloud autoscaling as a safety net.
5. **Handle scheduled events**: incorporate known events (product launches, marketing campaigns, batch jobs) into forecasts.
6. **Feature engineering**: automatically extract features from time-series data (day-of-week, hour, lag, rolling averages).
7. **Model lifecycle**: train, evaluate, deploy, monitor, and retrain models per service.
8. **Dashboard**: visualize predictions vs. actuals, scaling decisions, cost impact.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Forecast accuracy (MAPE) | < 15% for 15-min horizon, < 25% for 60-min horizon |
| Prediction latency | < 5 seconds per service |
| Scaling lead time vs. provision time | Prediction horizon > 2x provision time |
| Cost reduction vs. over-provisioning | > 20% infrastructure cost savings |
| False scale-down rate | < 1% (almost never scale down too early) |
| Availability | 99.9% |
| Coverage | > 80% of services with valid models |

### Constraints & Assumptions
- 500 services requiring autoscaling, running on Kubernetes (EKS/GKE).
- Prometheus metrics available with 15-second scrape interval.
- Container start time: ~30 seconds. Bare-metal provision: ~15 minutes.
- Some services have predictable daily patterns; others are bursty and unpredictable.
- ML training infrastructure available (GPU cluster or managed ML platform like SageMaker).
- Budget: ML infrastructure < $10K/month.

### Out of Scope
- Real-time anomaly detection (separate system; we consume anomaly signals).
- Application-level optimization (query caching, connection pooling).
- Multi-cloud arbitrage (choosing cheapest provider in real-time).
- Network autoscaling (bandwidth, CDN).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Services to forecast | Given | 500 |
| Metrics per service | ~5 key metrics (CPU, memory, request rate, error rate, queue depth) | 2,500 time-series |
| Data points per metric per day | 86,400s / 15s scrape interval | 5,760 points/metric/day |
| Total data points per day | 2,500 * 5,760 | ~14.4M points/day |
| Predictions per cycle | 500 services * 1 prediction | 500 per cycle |
| Prediction cycles per day | Every 5 min = 288 cycles/day | 288/day |
| Total predictions per day | 500 * 288 | 144,000/day |
| Scaling actions per day | ~500 services * 2-4 scale events avg | ~1,500 actions/day |

### Latency Requirements (LLM Inference Considerations)

| Component | Target | Notes |
|---|---|---|
| Feature extraction | < 2s per service | PromQL aggregation |
| Model inference (per service) | < 500ms | Batch prediction preferred |
| Batch prediction (all 500 services) | < 60s | Parallelized |
| Scaling decision logic | < 1s | Rule-based on prediction |
| Scale-out execution (K8s) | < 30s | Pod scheduling |
| Scale-out execution (cloud VMs) | < 5 min | Instance launch |
| End-to-end: prediction → scaled | < 2 min (K8s), < 10 min (VMs) | Must be less than forecast horizon |
| LLM call (for NL summaries, optional) | < 5s | Used for dashboard explanations only |

### Storage Estimates

| Data | Calculation | Size |
|---|---|---|
| Raw metrics (1 year, downsampled to 1-min) | 2,500 series * 525,600 min * 16 bytes | ~21 GB |
| Feature store (1 year) | 500 services * 525,600 windows * 200 bytes (feature vector) | ~53 GB |
| Model artifacts (500 models) | 500 * 50MB avg | ~25 GB |
| Prediction history (1 year) | 144,000/day * 1KB * 365 | ~52 GB |
| Training data archive (2 years) | 2 * 21 GB | ~42 GB |
| Total | | ~193 GB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Prometheus metric scraping | 14.4M points/day * 16 bytes / 86400 | ~2.7 KB/s (handled by Prometheus) |
| Feature extraction queries | 500 services * 10KB response / 300s cycle | ~17 KB/s |
| Model inference I/O | 500 * 2KB input + 500 * 0.5KB output / 300s | ~4 KB/s |
| Scaling API calls | 1,500/day * 2KB | Negligible |

---

## 3. High Level Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                     Data Sources                                    │
│  Prometheus   │  CloudWatch  │  Event Calendar  │  Deployment API  │
│  (metrics)    │  (cloud)     │  (scheduled      │  (recent deploys)│
│               │              │   events)        │                  │
└──────┬────────┴──────┬───────┴──────┬───────────┴──────┬───────────┘
       │               │              │                  │
       ▼               ▼              ▼                  ▼
┌────────────────────────────────────────────────────────────────────┐
│                    Feature Engineering Pipeline                     │
│                                                                     │
│  ┌──────────┐  ┌──────────────┐  ┌──────────────┐  ┌───────────┐  │
│  │ Time     │  │ Lag Features │  │ Calendar    │  │ External  │  │
│  │ Features │  │ (t-1, t-5,  │  │ Features    │  │ Signals   │  │
│  │ (hour,   │  │  t-15, t-30 │  │ (events,    │  │ (deploys, │  │
│  │  dow,    │  │  min)       │  │  holidays,  │  │  batch    │  │
│  │  month)  │  │             │  │  campaigns) │  │  jobs)    │  │
│  └──────────┘  └──────────────┘  └──────────────┘  └───────────┘  │
│                                                                     │
│  Output: Feature vector per service per prediction window           │
└───────────────────────────┬─────────────────────────────────────────┘
                            │
                   ┌────────┴────────┐
                   │  Feature Store  │
                   │  (Redis + S3)   │
                   └────────┬────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────────────────┐
│                    Prediction Engine                                │
│                                                                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐    │
│  │ Model       │  │ Ensemble    │  │ Prediction Postprocess  │    │
│  │ Registry    │  │ Combiner    │  │ (clamp, smooth, add     │    │
│  │ (per-svc   │  │ (weighted   │  │  safety margin)         │    │
│  │  models)   │  │  average)   │  │                         │    │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘    │
│                                                                     │
│  Models: Prophet │ LSTM │ XGBoost │ ARIMA (per service, best fit) │
└───────────────────────────┬─────────────────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────────────────┐
│                    Scaling Decision Engine                          │
│                                                                     │
│  ┌──────────────────┐  ┌──────────────────┐  ┌────────────────┐   │
│  │ Predicted demand │  │ Target utiliz.   │  │ Scaling plan   │   │
│  │ (req/s, CPU%)    │  │ (70% CPU target) │  │ (replicas,     │   │
│  │                  │──│                  │──│  schedule)     │   │
│  └──────────────────┘  └──────────────────┘  └────────┬───────┘   │
│                                                        │          │
│  ┌──────────────────────────────────────────────────┐  │          │
│  │ Constraints: min/max replicas, budget, cooldown  │  │          │
│  └──────────────────────────────────────────────────┘  │          │
└────────────────────────────────────────────────────────┬───────────┘
                                                         │
                            ┌────────────────────────────┘
                            │
                ┌───────────┴───────────┐
                │                       │
                ▼                       ▼
┌──────────────────────────┐  ┌──────────────────────────┐
│  Proactive Scaler        │  │  Reactive Scaler (HPA)   │
│  (execute predicted      │  │  (safety net — still     │
│   scale actions ahead    │  │   active, triggers if    │
│   of time)               │  │   prediction misses)     │
└──────────┬───────────────┘  └──────────┬───────────────┘
           │                             │
           ▼                             ▼
┌────────────────────────────────────────────────────────────────────┐
│                    Kubernetes / Cloud Infrastructure                │
│  HPA │ VPA │ Cluster Autoscaler │ ASG │ Managed Instance Groups   │
└────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────┐
│                    Feedback Loop                                    │
│  Actual demand vs. predicted → model accuracy metrics →            │
│  trigger retraining if accuracy degrades                           │
└────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────┐
│                    Observability                                    │
│  Grafana dashboards │ Prediction accuracy │ Cost savings │ Alerts │
└────────────────────────────────────────────────────────────────────┘
```

### Agent Loop (Observe → Reason → Act → Verify)

```
OBSERVE: Collect current metrics + features for all services
    │
    ▼
REASON: Run prediction models → forecast demand 15/30/60 min ahead
    │     Compare forecast to current capacity
    │     Determine scaling actions needed
    │
    ▼
ACT:   Execute proactive scaling (increase replicas ahead of demand)
    │  Respect constraints (min/max, budget, cooldown)
    │
    ▼
VERIFY: After forecast window passes, compare predicted vs actual
        Update model accuracy metrics
        Trigger retraining if accuracy degrades
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Service scaling configuration
CREATE TABLE service_scaling_config (
    service_id          VARCHAR(255) PRIMARY KEY,
    namespace           VARCHAR(100) NOT NULL,
    cluster             VARCHAR(100) NOT NULL,
    min_replicas        INT NOT NULL DEFAULT 2,
    max_replicas        INT NOT NULL DEFAULT 50,
    target_cpu_pct      INT NOT NULL DEFAULT 70,
    target_memory_pct   INT NOT NULL DEFAULT 80,
    target_rps_per_pod  INT,                     -- if known
    scale_up_cooldown   INT DEFAULT 180,          -- seconds
    scale_down_cooldown INT DEFAULT 600,          -- seconds
    prediction_enabled  BOOLEAN DEFAULT TRUE,
    model_id            UUID REFERENCES prediction_models(model_id),
    provision_time_sec  INT DEFAULT 30,           -- container start time
    created_at          TIMESTAMPTZ DEFAULT NOW(),
    updated_at          TIMESTAMPTZ DEFAULT NOW()
);

-- Prediction models (one per service per metric)
CREATE TABLE prediction_models (
    model_id            UUID PRIMARY KEY,
    service_id          VARCHAR(255) NOT NULL,
    metric_name         VARCHAR(100) NOT NULL,   -- 'cpu_pct', 'request_rate', 'memory_pct'
    model_type          VARCHAR(50) NOT NULL,     -- 'prophet', 'lstm', 'xgboost', 'arima', 'ensemble'
    model_version       INT NOT NULL DEFAULT 1,
    artifact_path       VARCHAR(500) NOT NULL,    -- S3 path to serialized model
    training_data_range VARCHAR(50),              -- e.g., '90d'
    accuracy_mape       FLOAT,                    -- mean absolute percentage error
    accuracy_rmse       FLOAT,
    last_trained_at     TIMESTAMPTZ,
    last_evaluated_at   TIMESTAMPTZ,
    status              VARCHAR(20) DEFAULT 'active', -- 'active', 'training', 'deprecated'
    hyperparams         JSONB,
    UNIQUE(service_id, metric_name, model_version)
);

-- Predictions (stored for accuracy tracking)
CREATE TABLE predictions (
    prediction_id       UUID PRIMARY KEY,
    service_id          VARCHAR(255) NOT NULL,
    model_id            UUID REFERENCES prediction_models(model_id),
    predicted_at        TIMESTAMPTZ NOT NULL,     -- when the prediction was made
    target_time         TIMESTAMPTZ NOT NULL,     -- what time was predicted
    horizon_minutes     INT NOT NULL,             -- 15, 30, or 60
    metric_name         VARCHAR(100) NOT NULL,
    predicted_value     FLOAT NOT NULL,
    confidence_lower    FLOAT,                    -- 95% CI lower bound
    confidence_upper    FLOAT,                    -- 95% CI upper bound
    actual_value        FLOAT,                    -- filled in after target_time
    error_pct           FLOAT,                    -- filled in after target_time
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- Scaling actions taken
CREATE TABLE scaling_actions (
    action_id           UUID PRIMARY KEY,
    service_id          VARCHAR(255) NOT NULL,
    trigger             VARCHAR(20) NOT NULL,     -- 'predictive', 'reactive_hpa', 'manual', 'scheduled'
    direction           VARCHAR(10) NOT NULL,     -- 'up' or 'down'
    from_replicas       INT NOT NULL,
    to_replicas         INT NOT NULL,
    reason              TEXT,
    predicted_demand    JSONB,                    -- forecast that drove this decision
    executed_at         TIMESTAMPTZ DEFAULT NOW(),
    effective_at        TIMESTAMPTZ,              -- when the scaling actually completed
    actual_demand       JSONB,                    -- filled in retrospectively
    was_correct         BOOLEAN                   -- filled in: was this scaling necessary?
);

-- Scheduled events (known traffic changes)
CREATE TABLE scheduled_events (
    event_id            UUID PRIMARY KEY,
    name                VARCHAR(255) NOT NULL,
    description         TEXT,
    affected_services   TEXT[],
    expected_traffic_multiplier FLOAT NOT NULL,   -- e.g., 2.5 = 250% of normal
    start_time          TIMESTAMPTZ NOT NULL,
    end_time            TIMESTAMPTZ NOT NULL,
    created_by          VARCHAR(100),
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- Feature store (materialized feature vectors)
CREATE TABLE feature_store (
    service_id          VARCHAR(255) NOT NULL,
    window_start        TIMESTAMPTZ NOT NULL,
    features            JSONB NOT NULL,           -- {hour, dow, month, lag_5m, lag_15m, rolling_avg_1h, ...}
    PRIMARY KEY (service_id, window_start)
);
```

### Database Selection

| Store | Technology | Justification |
|---|---|---|
| Time-series metrics | Prometheus + Thanos | Already in place; long-term storage via Thanos S3 |
| Feature store (hot) | Redis | Fast feature retrieval for real-time prediction |
| Feature store (cold) | S3 (Parquet files) | Training data storage, cost-effective |
| Relational (config, models, actions) | PostgreSQL | ACID, complex queries for reporting |
| Model artifacts | S3 | Standard ML model storage |
| Model registry | MLflow | Model versioning, experiment tracking, deployment |

### Indexing Strategy

```sql
CREATE INDEX idx_predictions_service_time ON predictions (service_id, target_time DESC);
CREATE INDEX idx_predictions_accuracy ON predictions (model_id, error_pct) WHERE actual_value IS NOT NULL;
CREATE INDEX idx_actions_service_time ON scaling_actions (service_id, executed_at DESC);
CREATE INDEX idx_events_time ON scheduled_events (start_time, end_time);
CREATE INDEX idx_feature_store_lookup ON feature_store (service_id, window_start DESC);
```

---

## 5. API Design

### REST Endpoints

```
# Predictions
GET    /api/v1/predictions?service=X&horizon=15m         # Get latest prediction
GET    /api/v1/predictions/accuracy?service=X&period=7d  # Prediction accuracy metrics

# Scaling configuration
GET    /api/v1/services/{id}/scaling-config               # Get scaling config
PUT    /api/v1/services/{id}/scaling-config               # Update config
POST   /api/v1/services/{id}/scaling-config/simulate      # Simulate: what would scaling look like with these params?

# Scaling actions
GET    /api/v1/scaling-actions?service=X&since=T          # List scaling actions
POST   /api/v1/scaling-actions/override                   # Manual scaling override

# Scheduled events
POST   /api/v1/events                                     # Register an upcoming event
GET    /api/v1/events?active=true                         # List active/upcoming events
DELETE /api/v1/events/{id}                                # Cancel event

# Models
GET    /api/v1/models?service=X                           # List models for a service
POST   /api/v1/models/{id}/retrain                       # Trigger retraining
GET    /api/v1/models/{id}/evaluation                    # Model evaluation metrics

# Dashboard
GET    /api/v1/dashboard/summary                          # Cost savings, accuracy, coverage
GET    /api/v1/dashboard/service/{id}                    # Service-specific: predicted vs actual
```

### Agent Tool Call Interface

```json
{
  "tools": [
    {
      "name": "get_current_metrics",
      "description": "Fetch current resource utilization for a service",
      "parameters": {
        "service_id": "string",
        "metrics": ["cpu_pct", "memory_pct", "request_rate", "queue_depth"],
        "duration": "string (default '1h')"
      }
    },
    {
      "name": "get_prediction",
      "description": "Get demand prediction for a service",
      "parameters": {
        "service_id": "string",
        "horizon_minutes": "int (15, 30, or 60)"
      }
    },
    {
      "name": "execute_scaling",
      "description": "Scale a service to target replicas",
      "parameters": {
        "service_id": "string",
        "target_replicas": "int",
        "reason": "string"
      }
    },
    {
      "name": "register_event",
      "description": "Register a known upcoming traffic event",
      "parameters": {
        "name": "string",
        "affected_services": ["string"],
        "traffic_multiplier": "float",
        "start_time": "ISO 8601",
        "end_time": "ISO 8601"
      }
    },
    {
      "name": "get_scaling_history",
      "description": "Get recent scaling actions for a service",
      "parameters": {
        "service_id": "string",
        "since": "ISO 8601"
      }
    }
  ]
}
```

### Human Escalation API

```
POST /api/v1/alerts/scaling-anomaly
{
  "service_id": "checkout-service",
  "anomaly_type": "prediction_divergence",
  "detail": "Predicted 1200 rps but actual is 4800 rps (4x). Model accuracy degraded to MAPE 62%. Reactive HPA has taken over. Predictive scaling disabled for this service.",
  "recommended_actions": [
    "Investigate traffic source (potential DDoS or viral event)",
    "Manually register as scheduled event if expected",
    "Trigger model retraining once traffic stabilizes"
  ]
}
```

---

## 6. Core Component Deep Dives

### 6.1 Forecasting Model Selection and Ensemble

**Why it's hard:** Different services have wildly different traffic patterns. A B2B SaaS service has strong day-of-week patterns. A consumer mobile app has diurnal patterns with evening peaks. A batch processing service has scheduled spikes. No single model works for all. You must select, train, and maintain 500+ models.

| Model | Strengths | Weaknesses | Best For |
|---|---|---|---|
| **Prophet** | Handles seasonality, holidays, changepoints automatically. Easy to tune. | Slow for high-frequency data. Assumes additive/multiplicative decomposition. | Services with strong daily/weekly patterns |
| **ARIMA/SARIMA** | Well-understood, good for stationary series | Cannot handle complex non-linear patterns. Requires stationarity. | Stable services with simple trends |
| **LSTM (neural network)** | Captures complex temporal dependencies | Requires more data; harder to train; opaque | Services with complex, non-linear patterns |
| **XGBoost (gradient boosting)** | Fast, handles many features, interpretable | Doesn't model temporal dependencies natively | Services where external features (events, day-of-week) dominate |
| **Ensemble (weighted average)** | Most robust; reduces single-model risk | More complex; slightly higher latency | Default for production |

**Selected: Per-service model selection with ensemble fallback**

**Implementation:**

```python
class ModelSelector:
    """Select best model for a service based on backtesting accuracy."""

    CANDIDATE_MODELS = ['prophet', 'arima', 'lstm', 'xgboost']

    async def select_best_model(self, service_id, training_data):
        results = {}
        for model_type in self.CANDIDATE_MODELS:
            # Time-series cross-validation (walk-forward)
            # Train on first 80%, test on next 10%, slide forward
            cv_scores = self.walk_forward_cv(model_type, training_data, folds=5)
            results[model_type] = {
                'mape': np.mean(cv_scores['mape']),
                'rmse': np.mean(cv_scores['rmse']),
                'training_time': cv_scores['training_time']
            }

        # Select model with lowest MAPE that trains within budget
        best = min(results, key=lambda m: results[m]['mape'])

        # If best model MAPE > 20%, use ensemble of top-3
        if results[best]['mape'] > 0.20:
            top3 = sorted(results, key=lambda m: results[m]['mape'])[:3]
            return self.create_ensemble(top3, results)

        return best, results[best]

    def create_ensemble(self, models, results):
        """Inverse-MAPE weighted ensemble."""
        total_inv_mape = sum(1/results[m]['mape'] for m in models)
        weights = {m: (1/results[m]['mape'])/total_inv_mape for m in models}
        return 'ensemble', weights
```

**Feature engineering per model:**

| Feature | Calculation | Used By |
|---|---|---|
| `hour_of_day` | Extract from timestamp | All models |
| `day_of_week` | 0-6 | All models |
| `is_weekend` | Boolean | All models |
| `month` | 1-12 | Prophet, XGBoost |
| `lag_5m` | Value 5 min ago | LSTM, XGBoost |
| `lag_15m` | Value 15 min ago | LSTM, XGBoost |
| `lag_30m` | Value 30 min ago | LSTM, XGBoost |
| `lag_1h` | Value 1 hour ago | All |
| `lag_1d` | Value 24 hours ago (same time yesterday) | All |
| `lag_7d` | Value 7 days ago (same time last week) | Prophet, XGBoost |
| `rolling_avg_15m` | Mean of last 15 minutes | LSTM, XGBoost |
| `rolling_avg_1h` | Mean of last hour | All |
| `rolling_std_1h` | Std dev of last hour | XGBoost |
| `trend_1h` | Linear regression slope over last hour | XGBoost |
| `is_event` | Boolean: is a scheduled event active? | All |
| `event_multiplier` | Expected traffic multiplier from event | All |
| `recent_deployment` | Boolean: deploy in last 2 hours? | XGBoost |

**Failure Modes:**
- **Concept drift**: Traffic patterns change (e.g., new feature launch, customer churn). Mitigation: continuous accuracy monitoring, auto-retrain when MAPE > threshold.
- **Missing data**: Prometheus outage causes gaps in training data. Mitigation: linear interpolation for gaps < 30 min; exclude longer gaps.
- **Cold start**: New service has no history. Mitigation: use a similar service's model as initial model (transfer learning) or fall back to reactive HPA only.

**Interviewer Q&As:**

**Q1: Why not just use Prophet for everything? It handles seasonality automatically.**
A: Prophet is excellent for services with clean daily/weekly seasonality, but it struggles with: (1) Sub-hourly prediction horizons where recent momentum matters more than seasonality. (2) Services dominated by external events rather than patterns. (3) Highly non-linear patterns (e.g., traffic that spikes exponentially during flash sales). By running backtesting, we find the best model per service. About 40% of services work best with Prophet, 30% with XGBoost, 20% with LSTM, and 10% with ensembles.

**Q2: How do you handle retraining 500 models?**
A: Scheduled batch retraining: all models retrain weekly using the last 90 days of data. This runs on a Spark/SageMaker cluster overnight. Triggered retraining: if a model's rolling MAPE exceeds 25% for 24 hours, immediate retraining is triggered. Cost: ~$200/month for weekly retraining on managed ML infrastructure.

**Q3: How much training data do you need?**
A: Minimum: 2 weeks (to capture weekday/weekend patterns). Recommended: 90 days (to capture monthly patterns). For LSTM: 6 months minimum (needs more data). For services with strong yearly seasonality (retail), we include last year's data but with exponential time decay (recent data weighted higher).

**Q4: How do you handle the "unknown event" problem — traffic spike you didn't predict?**
A: This is where the reactive HPA safety net is critical. The predictive system handles known patterns. The HPA catches everything else. We also monitor the delta between predicted and actual — a large delta triggers an alert and temporary switch to reactive-only mode for that service.

**Q5: What's the computational cost of inference vs training?**
A: Training: ~$200/month for weekly retraining of 500 models. Inference: negligible — Prophet and XGBoost predict in < 10ms per service; LSTM in < 50ms. Even for 500 services every 5 minutes, total inference compute is < 1 CPU-hour/day. The bottleneck is feature extraction (PromQL queries), not model inference.

**Q6: How do you validate a new model before deploying it?**
A: Three-phase validation. (1) Offline: walk-forward cross-validation on historical data. Must beat current model's MAPE by > 5%. (2) Shadow: new model runs in parallel, predictions logged but not acted upon. Compare accuracy for 48 hours. (3) Canary: new model controls scaling for 10% of the service's replicas. Monitor for 24 hours. Only then promote to primary.

---

### 6.2 Proactive + Reactive Scaling Integration

**Why it's hard:** You have two scaling systems that can conflict. The predictive system scales proactively. The reactive HPA scales based on current utilization. If both act independently, you get oscillation (scale up from prediction, scale down from HPA because utilization dropped, scale up again...). They must be coordinated.

| Approach | Pros | Cons |
|---|---|---|
| **Replace HPA entirely with prediction** | Simple, one system | No safety net when prediction is wrong |
| **Prediction sets min-replicas for HPA** | HPA as safety net; prediction sets floor | HPA might scale down below prediction |
| **Prediction adjusts HPA target directly** | Seamless integration | Complex; debugging scaling decisions is hard |
| **Prediction as recommendation, HPA as enforcement** | Clear separation of concerns | Potential for conflicting signals |
| **Two-layer: prediction sets floor, HPA can add more** | Best of both: proactive + reactive | Requires custom HPA behavior |

**Selected: Prediction sets floor (minReplicas), HPA can scale above**

**Implementation:**

```
┌───────────────────────────────────────────────────┐
│           SCALING DECISION FLOW                    │
│                                                    │
│  Every 5 minutes:                                  │
│                                                    │
│  1. Predictive engine forecasts demand for +15m    │
│     → predicted_demand = 850 rps                   │
│                                                    │
│  2. Calculate required replicas:                   │
│     required = ceil(predicted_demand /              │
│                     rps_per_pod /                   │
│                     target_utilization)             │
│     = ceil(850 / 100 / 0.7) = 13 replicas         │
│                                                    │
│  3. Add safety margin (10%):                       │
│     target_min = ceil(13 * 1.10) = 15 replicas    │
│                                                    │
│  4. Clamp to bounds:                               │
│     target_min = max(config.min, min(target_min,   │
│                      config.max))                  │
│     = max(2, min(15, 50)) = 15                    │
│                                                    │
│  5. Update HPA minReplicas to 15                   │
│                                                    │
│  6. HPA continues to manage actual replicas:       │
│     - If current CPU > 70%: scale up beyond 15     │
│     - If current CPU < 70%: scale down to 15       │
│       (but never below 15, our predicted floor)    │
│                                                    │
│  7. Cooldown: don't decrease minReplicas for       │
│     10 minutes after increasing it                 │
└───────────────────────────────────────────────────┘
```

**Key design decisions:**

1. **Asymmetric cooldowns**: Scale-up cooldown: 3 minutes. Scale-down cooldown: 10 minutes. It's much safer to have extra capacity than too little.

2. **Prediction safety margin**: Always add 10-20% above predicted demand. Under-provisioning is more expensive (user impact) than over-provisioning (cost).

3. **Scale-down smoothing**: Don't decrease minReplicas in a single jump. Ramp down: reduce by at most 20% per cycle. This prevents aggressive scale-down from incorrect low predictions.

4. **HPA supremacy**: If HPA wants more replicas than the prediction, HPA wins. Prediction only sets the floor, never the ceiling.

**Failure Modes:**
- **Prediction too high**: Over-provisioned. Cost waste, but no user impact. Detected by monitoring cost-per-request. Mitigated by continuous model accuracy tracking.
- **Prediction too low**: HPA catches it reactively. Users experience brief latency spike during scale-up. Detected by HPA scale-up events that exceed predicted floor.
- **HPA and prediction oscillate**: minReplicas goes up/down while HPA scales up/down. Mitigated by scale-down smoothing and cooldown periods.
- **HPA disabled/broken**: Predictive system is sole scaler. If prediction is wrong, no safety net. Mitigated by alerting on HPA health.

**Interviewer Q&As:**

**Q1: Why not just replace HPA entirely?**
A: ML models are inherently imperfect. A model might have 85% accuracy, but that 15% error rate means ~75 services could be wrong at any given time. The HPA safety net catches these cases. It's the same principle as redundancy in infrastructure — you don't remove your backup because your primary is reliable.

**Q2: How do you prevent the "ratchet problem" where minReplicas only goes up?**
A: Two mechanisms. (1) Scale-down cooldown (10 minutes) + gradual ramp-down (max 20% decrease per cycle). This ensures we don't scale down too fast, but we DO scale down. (2) We track the "idle replica" metric — replicas consistently below 30% utilization. If idle replicas persist for > 30 minutes, force a faster scale-down.

**Q3: What happens during a scheduled event (e.g., Black Friday)?**
A: The event is registered via the API with a traffic multiplier. The prediction model receives this as an input feature. The predicted demand is scaled by the multiplier. We also pre-scale to the event's expected demand 15 minutes before the event starts (lead time). After the event, we gradually scale down over 30 minutes rather than immediately.

**Q4: How do you handle services that are completely unpredictable?**
A: Some services (e.g., webhook receivers, event-driven workers) have no discernible pattern. For these, we disable predictive scaling and rely entirely on reactive HPA. We monitor prediction accuracy per service — if MAPE > 30% consistently, we auto-disable predictions and log an alert.

**Q5: How do you handle cascading scaling? Service A scales up → calls Service B more → B needs to scale too.**
A: The prediction model for Service B should already account for Service A's traffic (it's an input feature if they're correlated). But for rapid cascading, we have a "dependent scaling" feature: services can declare dependencies, and when Service A scales up, we preemptively set a higher floor for Service B. This is simpler and faster than waiting for B's metrics to increase.

**Q6: What's the cost savings compared to just keeping services over-provisioned?**
A: Typical over-provisioning: 50% headroom (1.5x actual need). Predictive scaling: 15% headroom (1.15x actual need, with safety margin). For 500 services running ~5,000 pods average, reducing from 7,500 pods (over-provisioned) to 5,750 pods (predictive + margin): saving 1,750 pods. At $50/month per pod: $87,500/month savings. Even accounting for system cost ($10K/month), net savings > $75K/month.

---

### 6.3 Model Retraining Pipeline

**Why it's hard:** Models degrade over time as traffic patterns change (concept drift). You must detect degradation, retrain efficiently, validate new models, and deploy without downtime — at scale across 500 services.

| Approach | Pros | Cons |
|---|---|---|
| **Fixed schedule (weekly)** | Simple, predictable | May miss rapid drift; may retrain unnecessarily |
| **Trigger on accuracy degradation** | Efficient; only retrain when needed | Need accurate accuracy monitoring; delay between drift and retraining |
| **Continuous online learning** | Always up to date | Complex; risk of instability; hard to validate |
| **Hybrid: scheduled + triggered** | Robust coverage | More moving parts |

**Selected: Hybrid (weekly scheduled + triggered on accuracy degradation)**

**Implementation:**

```
┌────────────────────────────────────────────────────────┐
│              MODEL RETRAINING PIPELINE                  │
│                                                         │
│  1. MONITOR (continuous)                                │
│     For each model, track rolling 24h MAPE              │
│     If MAPE > threshold (25%) for 6 hours:              │
│     → trigger emergency retrain                         │
│                                                         │
│  2. RETRAIN (scheduled weekly + triggered)              │
│     Pull last 90 days of data from Thanos/S3            │
│     Feature engineering                                 │
│     Train all candidate models (Prophet, LSTM, etc.)    │
│     Walk-forward cross-validation                       │
│     Select best model (or ensemble)                     │
│                                                         │
│  3. VALIDATE                                            │
│     Compare new model vs current on holdout set         │
│     New model must beat current by > 2% MAPE            │
│     (prevents unnecessary model churn)                  │
│                                                         │
│  4. DEPLOY                                              │
│     Shadow mode: 24h parallel prediction, no action     │
│     If shadow accuracy confirmed: promote to primary    │
│     Old model kept as fallback for 7 days               │
│                                                         │
│  5. ARCHIVE                                             │
│     Old model artifacts archived in S3                  │
│     Training data versioned for reproducibility         │
└────────────────────────────────────────────────────────┘
```

**Interviewer Q&As:**

**Q1: How long does retraining take?**
A: Per model: Prophet ~30s, XGBoost ~10s, LSTM ~5 min (GPU), ARIMA ~5s. For 500 services with 5 metrics each (2,500 models), parallelized on a 4-GPU cluster: ~2 hours for full retraining. Emergency retrain for a single service: < 10 minutes.

**Q2: How do you handle model versioning?**
A: MLflow tracks every training run: hyperparameters, training data hash, metrics, model artifact. Each model has a version number. The production model registry maintains "current" and "candidate" pointers. Rollback to a previous version is a pointer swap.

**Q3: What triggers an emergency retrain vs. disabling predictions?**
A: If MAPE > 25% for 6 hours: emergency retrain. If MAPE > 40% for 1 hour: disable predictions for that service (fall back to HPA only) AND trigger retrain. The distinction: 25-40% MAPE means the model is degrading but still somewhat useful; > 40% means it's actively harmful.

**Q4: How do you prevent the "retraining death spiral" where a bad retrain triggers another retrain?**
A: (1) The new model must beat the current model on holdout data before deployment. If the retrained model is worse, we keep the old one. (2) We limit retrains to max 1 per service per 24 hours. (3) After 3 consecutive failed retrains, we alert the ML team for manual investigation.

---

## 7. AI Agent Architecture

### Agent Loop Design

The predictive autoscaling system is a batch-oriented agent that runs on a fixed cycle (every 5 minutes):

```
┌────────────────────────────────────────────────────────────┐
│               SCALING AGENT CYCLE (every 5 min)            │
│                                                             │
│  1. OBSERVE                                                 │
│     ├── Fetch current metrics for all 500 services          │
│     ├── Fetch feature vectors from feature store            │
│     ├── Check scheduled events calendar                     │
│     └── Check recent scaling actions (cooldown status)      │
│                                                             │
│  2. PREDICT                                                 │
│     ├── Run prediction model for each service               │
│     ├── Output: predicted demand at t+15m, t+30m, t+60m    │
│     └── Attach confidence intervals                         │
│                                                             │
│  3. DECIDE                                                  │
│     ├── For each service:                                   │
│     │   ├── Calculate required replicas from prediction     │
│     │   ├── Add safety margin                               │
│     │   ├── Check constraints (min/max, cooldown, budget)   │
│     │   └── Generate scaling action if needed               │
│     └── Prioritize: critical services first                 │
│                                                             │
│  4. ACT                                                     │
│     ├── Execute scaling actions (update HPA minReplicas)    │
│     └── Log all actions with reasoning                      │
│                                                             │
│  5. VERIFY (asynchronous, at t+15m)                         │
│     ├── Compare predicted vs actual demand                  │
│     ├── Check if scaling was sufficient                     │
│     ├── Update model accuracy metrics                       │
│     └── Trigger retraining if accuracy degraded             │
└────────────────────────────────────────────────────────────┘
```

### Tool Definitions

| Tool | Description | Frequency |
|---|---|---|
| `query_prometheus` | Fetch current and historical metrics | Every cycle, per service |
| `get_feature_vector` | Read features from feature store | Every cycle, per service |
| `run_prediction` | Execute ML model inference | Every cycle, per service |
| `update_hpa_min_replicas` | Set minReplicas on K8s HPA | Only when scaling needed |
| `get_hpa_status` | Read current HPA state | Every cycle |
| `check_cluster_capacity` | Verify cluster can accommodate scale-up | Before scale-up |
| `register_event` | Add a scheduled event | On-demand |
| `trigger_retrain` | Kick off model retraining | When accuracy degrades |

### Context Window Management

This system uses ML models, not LLMs, for its core prediction. LLMs are used only for:

1. **Natural language explanations**: "Checkout-service is scaling from 8 to 14 pods because predicted demand is 980 rps at 2pm (daily lunch peak, corroborated by 93% model accuracy)."
2. **Anomaly explanation**: "Prediction divergence detected: predicted 500 rps but actual is 2000 rps. Possible cause: unregistered marketing campaign (similar pattern seen on March 15)."
3. **Event classification**: Parsing natural language event descriptions into structured features.

LLM context is minimal and used only for human-facing summaries, not for scaling decisions.

### Memory Architecture

| Memory Type | Content | Storage |
|---|---|---|
| **Episodic** | Past scaling decisions and their outcomes | PostgreSQL `scaling_actions` table |
| **Semantic** | Service configuration, SLOs, dependency graph | PostgreSQL + config management |
| **Procedural** | Trained ML models encode traffic patterns | Model artifacts in S3 |
| **Working** | Current feature vectors, recent predictions | Redis feature store |

### Guardrails and Safety

1. **Max scale rate**: Cannot increase replicas by more than 3x in a single cycle. Prevents runaway scaling.
2. **Min replicas floor**: Never scale below configured minimum, even if demand prediction is zero.
3. **Budget cap**: Daily scaling cost cannot exceed configured budget per service. Alert if approaching limit.
4. **Cluster capacity check**: Before scaling up, verify the cluster has available CPU/memory. If not, trigger cluster autoscaler first.
5. **Prediction sanity check**: Reject predictions that are > 5x current demand (likely model error) unless a registered event explains it.
6. **Scaling cooldown**: Cannot scale the same service more than once per 3 minutes (up) or 10 minutes (down).

### Confidence Thresholds

| Confidence Interval Width | Action |
|---|---|
| Narrow (upper/lower within 20% of mean) | Scale to mean prediction |
| Medium (within 40%) | Scale to 75th percentile of prediction |
| Wide (> 40%) | Scale to upper bound (conservative) |
| No confidence available | Fall back to reactive HPA only |

### Dry-Run Mode

```yaml
dry_run_output:
  timestamp: "2026-04-09T13:55:00Z"
  cycle_id: "cycle-20260409-1355"
  services_evaluated: 500
  scaling_recommendations:
    - service: checkout-service
      current_replicas: 8
      predicted_demand_15m: "980 rps (CI: 820-1140)"
      recommended_replicas: 14
      reason: "Daily lunch peak + Tuesday above-average baseline"
      confidence: "medium (CI width 33%)"
      would_execute: true
    - service: search-service
      current_replicas: 12
      predicted_demand_15m: "650 rps (CI: 580-720)"
      recommended_replicas: 10
      reason: "Post-morning-peak decline"
      would_execute: false  # scale-down cooldown active
    - service: batch-processor
      current_replicas: 4
      predicted_demand_15m: "2400 jobs (CI: 2000-2800)"
      recommended_replicas: 8
      reason: "Scheduled batch job starting at 14:00"
      would_execute: true
  estimated_cost_delta: "-$12.50/hour (net savings from scale-downs exceeding scale-ups)"
```

---

## 8. Scaling Strategy

| Component | Scaling Approach | Trigger |
|---|---|---|
| Feature engineering pipeline | Horizontal (Kafka consumers) | Feature extraction lag > 30s |
| Prediction engine | Horizontal (stateless workers) | Prediction cycle > 60s |
| Model training | Spot GPU instances | Training queue depth > 0 |
| Feature store (Redis) | Redis Cluster (sharded) | Memory > 70% |
| PostgreSQL | Read replicas for dashboard queries | Read latency > 100ms |

### Interviewer Q&As

**Q1: How does this system scale to 10,000 services?**
A: Current bottleneck is Prometheus query fan-out (500 parallel queries per cycle). At 10K services: (1) Shard services across multiple prediction workers. (2) Use Prometheus recording rules to pre-aggregate features. (3) Increase cycle time from 5 to 10 minutes for less critical services (tiered approach). (4) Use batch model inference (GPU) for LSTM models. Total: prediction for 10K services in < 120s.

**Q2: What if the prediction engine itself becomes a bottleneck?**
A: The prediction engine is stateless — it reads features, runs models, outputs predictions. We can scale it horizontally without limit. The real bottleneck would be Prometheus queries (fan-out) or the HPA API (update rate). For Prometheus: pre-aggregate with recording rules. For HPA API: batch updates and prioritize by service criticality.

**Q3: How do you handle model training cost scaling?**
A: We use spot instances for training (70% cost reduction). Weekly training of 500 models costs ~$200 on spot. At 10K services: ~$4K/month. We also cache feature vectors — training data preparation is often more expensive than model training. With materialized features in S3, training is I/O-bound, not compute-bound.

**Q4: What happens during a cloud provider outage?**
A: If Prometheus is unavailable, the feature pipeline uses cached features (up to 15 min stale) and the prediction still runs. If the Kubernetes API is unavailable, scaling actions queue and execute when API recovers. If both are down, there's likely a larger outage and scaling is moot — the incident response system takes over.

**Q5: How do you handle multi-cluster scaling?**
A: Each cluster has its own prediction pipeline and HPA. Cross-cluster balancing (shifting traffic to less-loaded clusters) is handled by a separate global load balancer. The predictive system feeds demand signals to the global balancer, but doesn't directly manage cross-cluster traffic.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| **Prediction model produces garbage** | Over/under-provisioning | MAPE spike in monitoring | Auto-disable prediction for affected service; fall back to HPA |
| **Feature pipeline down** | No new features; predictions based on stale data | Pipeline health check; feature freshness alert | Use cached features (< 15 min acceptable). If stale > 15 min, skip prediction. |
| **Prediction engine crash** | No proactive scaling for one cycle | Heartbeat monitoring | HPA continues to work. Restart prediction engine. Missed cycles are acceptable (5-min gap). |
| **Wrong event multiplier** | Massive over/under-provisioning during event | Compare actual vs predicted during event | Allow manual override during events. Cap multiplier at configurable max (e.g., 10x). |
| **Model overfitting to historical data** | Cannot adapt to new patterns | Cross-validation metrics during training | Walk-forward validation, not random split. Regularization. Monitor production accuracy. |
| **Race condition: two scaling actions for same service** | Conflicting replica counts | Distributed lock per service | Acquire lock before updating HPA. Last-write-wins with logging. |
| **Cluster autoscaler lag** | Predictive scale-up requests pods, but no nodes available | Pending pods metric | Include node provisioning time in prediction horizon. Scale nodes proactively based on aggregate demand. |
| **Budget exceeded** | Cannot scale up when needed | Cost tracking metrics | Alert at 80% budget. Never prevent scaling for critical services regardless of budget. |

### AI Safety Controls

1. **Model circuit breaker**: Auto-disable model if MAPE > 40% for 1 hour. Human must re-enable after investigation.
2. **Prediction bounds**: Reject any prediction > 10x current demand (unless event justifies it).
3. **Scale-down governor**: Max 20% reduction per cycle. Prevents cliff-edge scale-down.
4. **Audit trail**: Every scaling decision logged with: prediction values, confidence interval, model version, features used.
5. **A/B testing**: New model versions run in shadow mode for 48h before controlling scaling.

---

## 10. Observability

### Key Metrics

| Metric | Type | Target | Alert Threshold |
|---|---|---|---|
| `prediction.mape_15m` | Gauge (per service) | < 15% | > 25% |
| `prediction.mape_60m` | Gauge (per service) | < 25% | > 40% |
| `prediction.coverage` | Gauge (global) | > 80% of services | < 70% |
| `scaling.proactive_rate` | Gauge | > 70% of scale events | < 50% (predictions not useful) |
| `scaling.reactive_save_rate` | Gauge | < 10% (HPA rarely needed) | > 30% (predictions failing) |
| `scaling.cost_savings_pct` | Gauge | > 20% vs over-provisioned baseline | < 10% |
| `scaling.latency_impact` | Gauge | 0 (no latency spike from under-provisioning) | > 0 (any latency incident from scaling) |
| `model.training_success_rate` | Gauge | > 95% | < 90% |
| `model.staleness_hours` | Gauge (per service) | < 168 (1 week) | > 336 (2 weeks) |
| `pipeline.feature_freshness_sec` | Gauge | < 60s | > 300s |
| `pipeline.cycle_duration_sec` | Histogram | < 60s | > 120s |
| `scaling.cooldown_violations` | Counter | 0 | > 0 |

### Prediction Accuracy Dashboard

```
┌────────────────────────────────────────────────────────────────┐
│   Service: checkout-service     Model: prophet v3              │
│   MAPE (7d): 12.3%            Status: healthy                 │
│                                                                 │
│   Predicted vs Actual (last 24h):                              │
│   1200│     ╭──╮                                               │
│       │    ╱    ╲    ← Predicted (dashed)                     │
│    800│───╱──────╲──────                                       │
│       │  ╱        ╲    ← Actual (solid)                       │
│    400│─╱──────────╲────                                       │
│       │╱            ╲                                          │
│      0└──────────────────────                                  │
│        00:00  06:00  12:00  18:00  00:00                       │
│                                                                 │
│   Scaling actions (last 24h): 6 proactive, 1 reactive          │
│   Cost: $45.20 (vs $67.80 over-provisioned baseline)           │
│   Savings: 33%                                                  │
└────────────────────────────────────────────────────────────────┘
```

---

## 11. Security

### Principle of Least Privilege

| Component | Permissions |
|---|---|
| Feature pipeline | Read-only on Prometheus, CloudWatch |
| Prediction engine | Read features from Redis/S3, write predictions to PostgreSQL |
| Scaling executor | Patch HPA minReplicas only (not delete, not modify other HPA fields) |
| Model training | Read training data from S3, write model artifacts to S3 |
| Dashboard | Read-only on all data stores |

### Audit Logging

Every scaling action is logged:
```json
{
  "action_id": "uuid",
  "timestamp": "2026-04-09T14:00:05Z",
  "service": "checkout-service",
  "trigger": "predictive",
  "model_id": "prophet-checkout-v3",
  "prediction": {"value": 980, "lower": 820, "upper": 1140, "horizon": "15m"},
  "features_used": {"hour": 14, "dow": 3, "lag_1h": 720, "event": null},
  "decision": {"from": 8, "to": 14, "reason": "daily_peak + safety_margin"},
  "executed": true,
  "cluster": "prod-us-east-1"
}
```

### Human Approval Gates

| Scenario | Approval |
|---|---|
| Normal predictive scaling (within 3x) | Auto |
| Large scale-up (> 3x current) | Require human confirmation |
| Scale to zero (cost saving for dev/staging) | Require human confirmation |
| First-time model deployment for a service | Shadow mode first, then human approval |
| Scaling during active incident | Defer to incident responder |

---

## 12. Incremental Rollout

### Rollout Phases

| Phase | Duration | Scope | Capability |
|---|---|---|---|
| **Phase 0: Data collection** | 4 weeks | All services | Collect metrics, build feature pipeline, no predictions |
| **Phase 1: Model training + shadow** | 4 weeks | All services | Train models, generate shadow predictions, measure accuracy |
| **Phase 2: Proactive scaling (non-critical)** | 6 weeks | 50 non-critical services | Predictions control minReplicas; HPA active as safety net |
| **Phase 3: Expand to all non-critical** | 4 weeks | 400 non-critical services | Same as Phase 2, wider scope |
| **Phase 4: Critical services** | Ongoing | All 500 services | Critical services with tighter safety margins (20% instead of 10%) |

### Rollout Interviewer Q&As

**Q1: What's the minimum data needed before Phase 2?**
A: 4 weeks of continuous metrics with < 5% data gaps. At least 2 weekend/weekday cycles. Model MAPE < 20% in shadow mode. If a service doesn't meet these criteria, it stays in Phase 1.

**Q2: How do you measure success at each phase?**
A: Phase 0: Feature pipeline reliability > 99.9%, data completeness > 95%. Phase 1: Model coverage > 80% of services, avg MAPE < 20%. Phase 2: Zero latency incidents caused by under-provisioning, cost reduction > 10% vs baseline. Phase 3-4: Same metrics at scale, cost reduction > 20%.

**Q3: What if a service has worse performance after enabling predictive scaling?**
A: Auto-rollback: if a service experiences latency spikes within 24h of enabling predictive scaling, auto-disable and revert to HPA-only. Investigation before re-enabling. The service keeps its data collection — just predictions are disabled.

**Q4: How do you handle the chicken-and-egg problem of needing historical data for a new service?**
A: Three approaches. (1) Similar service transfer: if the new service handles similar traffic (e.g., another REST API for the same user base), clone the most similar service's model as a starting point. (2) Conservative defaults: start with HPA-only and generous over-provisioning. Collect 4 weeks of data, then enable predictions. (3) Manual hints: the service owner provides expected traffic patterns (peak hours, event schedule) to bootstrap the model.

**Q5: How do you A/B test model improvements?**
A: Service hash-based routing. Services with even hash → model A, odd hash → model B. Track MAPE, cost savings, and latency impact per group. Run for 2 weeks. Require statistical significance (p < 0.05) before promoting. We can also run A/B within a single service: 50% of replicas managed by model A, 50% by model B.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Chosen | Rationale |
|---|---|---|---|
| Prediction horizon | 5m, 15m, 30m, 60m | 15m primary, 60m secondary | 15m balances lead time with accuracy. 60m for VM provisioning. |
| Model approach | Single model for all, per-service selection, ensemble | Per-service selection with ensemble fallback | No single model fits all traffic patterns; per-service maximizes accuracy |
| Scaling integration | Replace HPA, prediction sets min, prediction adjusts target | Prediction sets minReplicas, HPA as safety net | Clean separation; HPA catches prediction misses |
| Feature store | Inline computation, Redis, dedicated feature store (Feast) | Redis (hot) + S3 (cold) | Redis for low-latency serving; S3 for training. Feast adds complexity without benefit at our scale. |
| Retraining trigger | Fixed schedule only, accuracy-triggered only, hybrid | Hybrid (weekly + triggered) | Weekly catches gradual drift; triggered catches sudden shifts |
| LLM usage | Core predictions, explanations only, none | Explanations + anomaly summaries only | ML models are better for time-series forecasting. LLMs add value for human-facing explanations. |
| Safety margin | 0%, 10%, 20%, dynamic | 10% default, 20% for critical services | Balance cost vs risk. Critical services get more headroom. |

---

## 14. Complete Interviewer Q&A Bank

**Q1: Why not use a single LSTM model for all services?**
A: Traffic patterns vary dramatically. A shared LSTM would need to generalize across B2B, consumer, batch, and event-driven workloads. In practice, per-service models outperform because they specialize. The overhead of 500 models is manageable — total storage is ~25 GB, training is ~2 hours weekly. The real challenge is the operational overhead of monitoring 500 models, which we solve with automated accuracy tracking and retraining.

**Q2: How accurate is Prophet for 15-minute predictions?**
A: Prophet was designed for daily/weekly seasonality at hourly+ granularity. For 15-minute predictions, it can struggle because it doesn't capture minute-level momentum well. That's why we include lag features (5m, 15m lags) and use XGBoost or LSTM for services where short-term momentum matters more than long-term seasonality. In practice, Prophet achieves 12-18% MAPE at 15-minute horizon for services with strong seasonality, and 20-30% for bursty services (where LSTM does better).

**Q3: How do you handle correlated scaling across dependent services?**
A: Three approaches. (1) Direct dependency scaling: when Service A scales up, preemptively increase the floor for its downstream dependencies. (2) Correlated features: include Service A's metrics as features in Service B's prediction model. (3) Aggregate prediction: predict total cluster demand and distribute across services based on historical ratios. We primarily use approach (2) because it naturally captures the relationship in the ML model.

**Q4: What's the impact of Kubernetes pod startup time on your predictions?**
A: Pod startup time (typically 10-60 seconds) determines the minimum useful prediction horizon. If pods start in 30s, we need to predict at least 2-3 minutes ahead (to have new pods ready before the demand spike). That's easy. The harder case is bare-metal provisioning (15 minutes), where we need 30-60 minute predictions with lower accuracy. We handle this with a tiered approach: short-horizon high-confidence predictions for K8s, longer-horizon lower-confidence predictions for VM/bare-metal pre-warming.

**Q5: How do you handle zero-downtime deployments interacting with autoscaling?**
A: During a rolling deployment, temporary extra pods exist (old + new). The prediction model doesn't account for deployment mechanics — it predicts demand, not replica count. The scaling decision engine recognizes active deployments and adds a buffer (deployment pod count). After deployment completes, the buffer is removed over 2 cycles.

**Q6: What's better: predicting request rate or predicting CPU utilization?**
A: We predict request rate (demand), not CPU (resource utilization). Request rate is the root cause; CPU is the symptom. A code optimization might reduce CPU per request without changing demand. If we predicted CPU, we'd over-scale. By predicting demand and using a known rps-per-pod ratio, we decouple demand prediction from resource efficiency changes.

**Q7: How do you handle the "thundering herd" — many services needing to scale simultaneously?**
A: (1) Prioritize by service criticality (payment > search > recommendations). (2) Check cluster capacity before scaling — if the cluster can't accommodate all scale-ups, trigger cluster autoscaler first, then scale pods. (3) Stagger scale-ups by 10-second intervals to avoid API server overload. (4) Pre-warm cluster capacity based on aggregate demand predictions.

**Q8: Can you combine predictive autoscaling with spot instance management?**
A: Yes. The prediction engine provides demand forecasts at multiple horizons. Short-term (15m): use on-demand instances (guaranteed availability). Medium-term (1-4h): pre-bid on spot instances if predicted demand increase aligns with spot availability. This can save 60-70% on the extra capacity. The risk is spot termination — mitigated by keeping critical baseline on on-demand and only using spot for predicted surge.

**Q9: How do you evaluate whether the system is actually saving money?**
A: We maintain a "counterfactual baseline" — what would the cost have been with static over-provisioning (peak capacity 24/7). We calculate: `savings = baseline_cost - (actual_cost + system_operating_cost)`. This is tracked daily and reported weekly. We also track the inverse: did any under-provisioning event occur that wouldn't have happened with static over-provisioning? That's the "cost of accuracy errors."

**Q10: What happens when a prediction model starts performing poorly for one service but fine for others?**
A: Per-service monitoring catches this. The service's model gets auto-disabled when its MAPE exceeds the threshold. Other services are unaffected because each has independent models. Investigation follows: was there a traffic pattern change? A new feature launch? A dependency change? The service owner is notified and may need to register an event or update service metadata.

**Q11: How do you handle services that scale to zero (serverless-style)?**
A: Scale-to-zero is a special case. The prediction model must forecast not just how much demand, but when demand will resume (from zero). We model this as a classification problem: "will this service receive traffic in the next 15 minutes?" If yes, pre-warm to the minimum replica count. For functions-as-a-service patterns, we keep at least 1 warm instance during business hours.

**Q12: Compare this approach to AWS's built-in predictive scaling.**
A: AWS Predictive Scaling (for ASGs) uses a built-in model that's opaque. Advantages of our approach: (1) Model transparency — we can inspect, debug, and customize models. (2) Integration with internal signals (deployment events, custom events). (3) Works across cloud providers. (4) We can use domain-specific features. Disadvantage: more operational burden. For organizations with < 50 services, AWS built-in is sufficient. At 500+, custom is worth the investment.

**Q13: How do you handle flash crowds (viral content, HN front page)?**
A: Flash crowds are inherently unpredictable — no ML model can predict them. This is where reactive HPA is essential. The predictive system handles foreseeable demand; HPA handles the unforeseeable. We can also add a "flash crowd detector": if request rate exceeds prediction by > 3x and is still increasing, we aggressively scale up (overshoot intentionally) because undershooting a viral event is very costly.

**Q14: What feature has the highest predictive power?**
A: For most services: `lag_1d` (same time yesterday) and `lag_7d` (same time last week). These capture daily and weekly seasonality. Second most important: `hour_of_day` and `day_of_week` (calendar features). For batch processing: `scheduled_job_start_time` dominates. For event-driven: `event_multiplier` dominates during events, calendar features otherwise. We use SHAP values (feature importance from XGBoost) to identify the most important features per service.

**Q15: How would you add an LLM agent on top of this system?**
A: The LLM agent would serve as a natural language interface: "Why did checkout-service scale to 20 pods at 2pm?" → Agent queries prediction logs, feature values, model explanation (SHAP values), and generates a human-readable explanation. It could also handle event registration: "We're launching a promotion next Tuesday, expect 3x traffic on the product catalog service" → Agent creates a scheduled event. The LLM doesn't replace the ML prediction — it augments the human interface.

---

## 15. References

1. **Taylor, S.J. & Letham, B.** — "Forecasting at Scale" (2018, Prophet): https://doi.org/10.7287/peerj.preprints.3190v2
2. **Hochreiter, S. & Schmidhuber, J.** — "Long Short-Term Memory" (1997): https://doi.org/10.1162/neco.1997.9.8.1735
3. **Box, G.E.P. & Jenkins, G.M.** — "Time Series Analysis: Forecasting and Control" (ARIMA)
4. **Kubernetes HPA documentation**: https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/
5. **AWS Predictive Scaling**: https://docs.aws.amazon.com/autoscaling/ec2/userguide/ec2-auto-scaling-predictive-scaling.html
6. **Chen, T. & Guestrin, C.** — "XGBoost: A Scalable Tree Boosting System" (2016): https://arxiv.org/abs/1603.02754
7. **Lundberg, S.M. & Lee, S.I.** — "A Unified Approach to Interpreting Model Predictions" (SHAP, 2017): https://arxiv.org/abs/1705.07874
8. **MLflow** — Open-source ML lifecycle management: https://mlflow.org/
9. **Google Cloud** — "Autopilot and Predictive Autoscaling": https://cloud.google.com/kubernetes-engine/docs/concepts/autopilot-overview
10. **Hyndman, R.J. & Athanasopoulos, G.** — "Forecasting: Principles and Practice" (2021): https://otexts.com/fpp3/
