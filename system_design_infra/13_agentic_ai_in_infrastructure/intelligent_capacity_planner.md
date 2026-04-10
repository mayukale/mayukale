# System Design: Intelligent Capacity Planner

> **Relevance to role:** Capacity planning is a strategic infrastructure function that combines forecasting, cost optimization, and procurement automation. As an infrastructure platform engineer with Agentic AI competency, you must design a system where AI agents consume utilization data, growth trends, and business signals to generate actionable procurement recommendations — including natural language summaries for leadership. This system bridges the gap between real-time infrastructure metrics and long-horizon (weeks-to-months) planning decisions.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Demand forecasting**: Predict resource needs (compute, storage, GPU, network) 2-12 weeks ahead.
2. **Multi-signal ingestion**: Current utilization, reservation pipeline, growth trends, committed contracts, team capacity requests.
3. **Procurement lead time modeling**: For each hardware SKU, model vendor delivery time distributions.
4. **Scenario planning**: Generate optimistic, base, and pessimistic forecasts. Show capacity gaps for each.
5. **Recommendation engine**: When to order, what to order, where to deploy (region/AZ placement).
6. **Cost optimization**: Compare spot vs. reserved vs. on-demand. Model total cost of ownership (TCO).
7. **LLM-powered summaries**: Natural language reports for leadership ("You need to order 50 H100 servers in 2 weeks or you'll hit GPU capacity limits in 6 weeks").
8. **Automated procurement**: Integration with procurement system for PO generation (with human approval gate).
9. **Multi-region balancing**: Recommend workload shifts vs. capacity expansion.
10. **What-if analysis**: "What if customer X doubles their usage? What if we lose a region?"

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Forecast accuracy (2-week horizon) | MAPE < 10% |
| Forecast accuracy (12-week horizon) | MAPE < 25% |
| Planning cycle | Weekly (batch) + on-demand for urgent requests |
| Recommendation generation | < 5 minutes for full portfolio |
| Report generation (LLM summaries) | < 30 seconds |
| Data freshness | Utilization data < 1 hour old |
| Coverage | All major resource types (compute, GPU, storage, network) |

### Constraints & Assumptions
- Organization operates hybrid infrastructure: 60% cloud (AWS/GCP), 40% on-premises/colo.
- ~10,000 servers across 4 regions, 3 hardware generations.
- GPU capacity is the scarcest resource (H100, A100 clusters for ML workloads).
- Hardware procurement lead time: 2-16 weeks depending on SKU.
- Cloud reserved instances: 1 or 3-year commitments at 30-60% discount.
- Business context: known product roadmap, customer pipeline, seasonal patterns.

### Out of Scope
- Real-time autoscaling (handled by predictive autoscaler).
- Network topology design.
- Application-level performance optimization.
- Vendor negotiation (system provides data for negotiation; humans negotiate).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Resources to track | 10,000 servers + 50,000 cloud instances | ~60,000 resources |
| Metrics per resource | ~10 (CPU, memory, disk, GPU, network I/O, etc.) | 600,000 time-series |
| Planning forecasts per cycle | 60,000 resources * 4 scenarios (opt/base/pess/stress) | 240,000 forecasts |
| LLM report generations per week | 1 full report + ~20 on-demand queries | ~25 LLM calls/week |
| Dashboard users | ~50 (infra leads, finance, procurement) | 50 concurrent |
| What-if simulations per week | ~10 ad-hoc | 10/week |

### Latency Requirements

| Component | Target | Notes |
|---|---|---|
| Data ingestion (hourly) | < 10 min per cycle | Batch ETL from monitoring systems |
| Forecast generation (full portfolio) | < 5 min | Parallelized across resource types |
| Recommendation engine | < 2 min | After forecasts are ready |
| LLM summary generation | < 30s | For each report section |
| What-if simulation | < 1 min | Interactive queries |
| Dashboard load | < 3s | Pre-computed views |

### Storage Estimates

| Data | Calculation | Size |
|---|---|---|
| Historical utilization (2 years, hourly) | 600K series * 17,520 hours * 8 bytes | ~84 GB |
| Forecast results (rolling 12 weeks) | 240K forecasts * 12 weeks * 168 hours * 8 bytes | ~39 GB |
| Hardware catalog (SKUs, pricing, lead times) | ~500 SKUs * 10KB | ~5 MB |
| Procurement history | ~1,000 POs * 50KB | ~50 MB |
| LLM conversation/report logs | 25/week * 20KB * 52 weeks | ~26 MB |
| Total | | ~125 GB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Metrics ingestion | 600K series * 8 bytes / 3600s | ~1.3 KB/s (batch, bursty) |
| Dashboard queries | 50 users * 100KB / 3s | ~1.7 MB/s peak |
| LLM API calls | 25/week * 15KB | Negligible |

---

## 3. High Level Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                          Data Sources                                │
│                                                                      │
│ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────┐ │
│ │Prometheus│ │CloudWatch│ │ CMDB     │ │ Finance  │ │ Sales/     │ │
│ │(on-prem) │ │(cloud)   │ │(hardware │ │(budgets, │ │ Product    │ │
│ │          │ │          │ │ inventory│ │ contracts│ │ (roadmap,  │ │
│ │          │ │          │ │ lifecycle│ │ pricing) │ │ pipeline)  │ │
│ └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬───────┘ │
└──────┼────────────┼────────────┼────────────┼────────────┼──────────┘
       │            │            │            │            │
       ▼            ▼            ▼            ▼            ▼
┌──────────────────────────────────────────────────────────────────────┐
│                     Data Integration Layer                           │
│                  (ETL Pipeline — Airflow/Dagster)                    │
│                                                                      │
│  ┌────────────────┐  ┌────────────────┐  ┌──────────────────────┐   │
│  │ Utilization    │  │ Inventory &    │  │ Business Signals     │   │
│  │ Aggregator     │  │ Lifecycle      │  │ (customer growth,    │   │
│  │ (hourly rollup)│  │ Tracker        │  │  product launches,   │   │
│  │                │  │ (warranty,     │  │  seasonal patterns)  │   │
│  │                │  │  deprecation)  │  │                      │   │
│  └───────┬────────┘  └───────┬────────┘  └───────┬──────────────┘   │
└──────────┼───────────────────┼───────────────────┼──────────────────┘
           │                   │                   │
           ▼                   ▼                   ▼
┌──────────────────────────────────────────────────────────────────────┐
│                      Planning Data Warehouse                         │
│                      (PostgreSQL / Snowflake)                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌───────────┐  │
│  │ Utilization │  │ Hardware    │  │ Cost        │  │ Business  │  │
│  │ History     │  │ Inventory   │  │ Catalog     │  │ Signals   │  │
│  └─────────────┘  └─────────────┘  └─────────────┘  └───────────┘  │
└───────────────────────────┬──────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────────────┐
│                     Forecasting Engine                                │
│                                                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────────┐     │
│  │ Time-Series  │  │ Growth       │  │ Scenario Generator     │     │
│  │ Forecaster   │  │ Model        │  │ (optimistic / base /   │     │
│  │ (Prophet /   │  │ (logistic /  │  │  pessimistic / stress) │     │
│  │  ARIMA per   │  │  linear per  │  │                        │     │
│  │  resource)   │  │  customer)   │  │                        │     │
│  └──────────────┘  └──────────────┘  └────────────────────────┘     │
└───────────────────────────┬──────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────────────┐
│                   Capacity Gap Analyzer                               │
│                                                                      │
│  For each resource type, region, and scenario:                       │
│  gap = forecasted_demand - (current_capacity + committed_additions   │
│         - planned_decommissions)                                     │
│                                                                      │
│  If gap > 0 → capacity shortfall → need procurement                 │
│  If gap < -30% → over-provisioned → consider decommission            │
└───────────────────────────┬──────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────────────┐
│                   Recommendation Engine                               │
│                                                                      │
│  ┌──────────────────────────────┐  ┌────────────────────────────┐   │
│  │ Procurement Recommendations │  │ Cost Optimizer              │   │
│  │ (what to order, when,       │  │ (spot vs reserved vs       │   │
│  │  where, from which vendor)  │  │  on-demand; RI commitment  │   │
│  │                              │  │  analysis)                 │   │
│  └──────────────────────────────┘  └────────────────────────────┘   │
│                                                                      │
│  ┌──────────────────────────────┐  ┌────────────────────────────┐   │
│  │ Workload Shift Recommender  │  │ Decommission Recommender   │   │
│  │ (move workloads between     │  │ (retire old/underutilized  │   │
│  │  regions instead of buying) │  │  hardware)                 │   │
│  └──────────────────────────────┘  └────────────────────────────┘   │
└───────────────────────────┬──────────────────────────────────────────┘
                            │
              ┌─────────────┴─────────────┐
              │                           │
              ▼                           ▼
┌──────────────────────────┐  ┌──────────────────────────────────────┐
│   LLM Report Generator   │  │       Procurement Integration       │
│                           │  │                                      │
│  "You need to order 50   │  │  Generate POs → Human approval gate  │
│   H100s in 2 weeks or    │  │  → Submit to procurement system      │
│   GPU capacity runs out  │  │                                      │
│   in 6 weeks..."         │  │                                      │
└──────────────────────────┘  └──────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                        AI Agent Layer                                 │
│                                                                      │
│  OBSERVE: Current utilization, growth trends, business signals       │
│  REASON:  Forecast demand, identify gaps, generate recommendations   │
│  ACT:     Produce reports, submit procurement requests               │
│  VERIFY:  Track actuals vs forecast, measure recommendation quality  │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Hardware inventory
CREATE TABLE hardware_inventory (
    asset_id            VARCHAR(100) PRIMARY KEY,
    asset_type          VARCHAR(50) NOT NULL,    -- 'server', 'gpu_node', 'storage_array', 'network_switch'
    sku                 VARCHAR(100) NOT NULL,
    region              VARCHAR(50) NOT NULL,
    datacenter          VARCHAR(50),
    rack                VARCHAR(50),
    cpu_cores           INT,
    memory_gb           INT,
    gpu_type            VARCHAR(50),             -- 'H100', 'A100', null
    gpu_count           INT DEFAULT 0,
    storage_tb          FLOAT,
    status              VARCHAR(20) NOT NULL,    -- 'active', 'provisioning', 'maintenance', 'decommissioned'
    purchase_date       DATE,
    warranty_end        DATE,
    eol_date            DATE,                    -- end of life
    team_owner          VARCHAR(100),
    cloud_equivalent    VARCHAR(100),            -- e.g., 'p5.48xlarge' for H100 servers
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- Utilization history (aggregated hourly)
CREATE TABLE utilization_history (
    resource_id         VARCHAR(100) NOT NULL,
    resource_type       VARCHAR(50) NOT NULL,    -- 'compute', 'gpu', 'storage', 'memory'
    region              VARCHAR(50) NOT NULL,
    timestamp           TIMESTAMPTZ NOT NULL,
    avg_utilization     FLOAT NOT NULL,          -- 0.0 - 1.0
    peak_utilization    FLOAT NOT NULL,
    p95_utilization     FLOAT NOT NULL,
    total_capacity      FLOAT NOT NULL,          -- in resource units (cores, GB, etc.)
    total_allocated     FLOAT NOT NULL,
    total_used          FLOAT NOT NULL,
    PRIMARY KEY (resource_id, timestamp)
);

-- Demand forecasts
CREATE TABLE capacity_forecasts (
    forecast_id         UUID PRIMARY KEY,
    forecast_run_id     UUID NOT NULL,           -- groups forecasts from same planning cycle
    resource_type       VARCHAR(50) NOT NULL,
    region              VARCHAR(50) NOT NULL,
    scenario            VARCHAR(20) NOT NULL,    -- 'optimistic', 'base', 'pessimistic', 'stress'
    target_week         DATE NOT NULL,           -- which week is forecasted
    forecasted_demand   FLOAT NOT NULL,          -- in resource units
    confidence_lower    FLOAT,
    confidence_upper    FLOAT,
    current_capacity    FLOAT NOT NULL,
    committed_additions FLOAT DEFAULT 0,         -- already ordered hardware
    planned_decommissions FLOAT DEFAULT 0,
    gap                 FLOAT,                   -- demand - (capacity + additions - decommissions)
    model_used          VARCHAR(50),
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- Procurement recommendations
CREATE TABLE procurement_recommendations (
    recommendation_id   UUID PRIMARY KEY,
    forecast_run_id     UUID NOT NULL,
    sku                 VARCHAR(100) NOT NULL,
    quantity            INT NOT NULL,
    region              VARCHAR(50) NOT NULL,
    urgency             VARCHAR(20) NOT NULL,    -- 'immediate', 'within_2_weeks', 'within_4_weeks', 'planned'
    reason              TEXT NOT NULL,
    estimated_cost      DECIMAL(12,2),
    cost_currency       VARCHAR(3) DEFAULT 'USD',
    alternative_options JSONB,                   -- [{sku, qty, cost, trade_offs}]
    approved_by         VARCHAR(100),
    approved_at         TIMESTAMPTZ,
    po_number           VARCHAR(50),
    status              VARCHAR(20) DEFAULT 'pending', -- 'pending', 'approved', 'ordered', 'delivered', 'rejected'
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- Vendor lead time models
CREATE TABLE vendor_lead_times (
    sku                 VARCHAR(100) NOT NULL,
    vendor              VARCHAR(100) NOT NULL,
    lead_time_p50_days  INT NOT NULL,
    lead_time_p90_days  INT NOT NULL,
    lead_time_p99_days  INT NOT NULL,
    last_updated        TIMESTAMPTZ,
    sample_count        INT,                     -- number of past orders used for estimate
    PRIMARY KEY (sku, vendor)
);

-- Business signals
CREATE TABLE business_signals (
    signal_id           UUID PRIMARY KEY,
    signal_type         VARCHAR(50) NOT NULL,    -- 'customer_growth', 'product_launch', 'seasonal_event'
    description         TEXT,
    affected_resources  TEXT[],                  -- ['gpu', 'compute']
    affected_regions    TEXT[],
    impact_multiplier   FLOAT,                   -- e.g., 1.5 = 50% growth
    start_date          DATE,
    end_date            DATE,
    confidence          FLOAT,                   -- how likely is this signal
    source              VARCHAR(100),            -- 'sales_pipeline', 'product_roadmap', 'manual'
    created_by          VARCHAR(100),
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- Cost catalog
CREATE TABLE cost_catalog (
    sku                 VARCHAR(100) NOT NULL,
    pricing_model       VARCHAR(20) NOT NULL,    -- 'purchase', 'on_demand', 'reserved_1yr', 'reserved_3yr', 'spot'
    unit_cost           DECIMAL(10,2) NOT NULL,
    cost_period         VARCHAR(20) NOT NULL,    -- 'one_time', 'hourly', 'monthly', 'yearly'
    discount_pct        FLOAT DEFAULT 0,
    valid_from          DATE NOT NULL,
    valid_to            DATE,
    PRIMARY KEY (sku, pricing_model, valid_from)
);
```

### Database Selection

| Store | Technology | Justification |
|---|---|---|
| Planning warehouse | PostgreSQL (or Snowflake for larger orgs) | Complex analytical queries, joins across tables |
| Time-series (utilization) | TimescaleDB (PostgreSQL extension) or Prometheus+Thanos | Efficient time-series queries with SQL interface |
| Report cache | Redis | Fast dashboard rendering |
| Document store (LLM reports) | S3 | Versioned report storage |
| Workflow orchestration | Airflow / Dagster | ETL pipelines, forecast scheduling |

### Indexing Strategy

```sql
CREATE INDEX idx_utilization_resource_time ON utilization_history (resource_id, timestamp DESC);
CREATE INDEX idx_utilization_type_region_time ON utilization_history (resource_type, region, timestamp DESC);
CREATE INDEX idx_forecasts_run ON capacity_forecasts (forecast_run_id);
CREATE INDEX idx_forecasts_type_region_week ON capacity_forecasts (resource_type, region, target_week);
CREATE INDEX idx_recommendations_status ON procurement_recommendations (status) WHERE status = 'pending';
CREATE INDEX idx_inventory_type_region ON hardware_inventory (asset_type, region) WHERE status = 'active';
CREATE INDEX idx_signals_dates ON business_signals (start_date, end_date);
```

---

## 5. API Design

### REST Endpoints

```
# Forecasts
GET    /api/v1/forecasts/latest?resource_type=gpu&region=us-east-1
GET    /api/v1/forecasts/{run_id}                         # Full forecast run results
POST   /api/v1/forecasts/run                              # Trigger ad-hoc forecast

# Recommendations
GET    /api/v1/recommendations?status=pending             # Pending recommendations
POST   /api/v1/recommendations/{id}/approve               # Approve a procurement
POST   /api/v1/recommendations/{id}/reject                # Reject with reason

# What-if analysis
POST   /api/v1/what-if
{
  "scenarios": [
    {"signal": "customer_growth", "multiplier": 2.0, "affected": ["gpu"]},
    {"signal": "region_loss", "region": "us-west-2"}
  ]
}

# Business signals
POST   /api/v1/signals                                    # Register a business signal
GET    /api/v1/signals?active=true                        # Active signals

# Reports
GET    /api/v1/reports/weekly                             # Latest weekly capacity report
GET    /api/v1/reports/{id}                               # Specific report
POST   /api/v1/reports/generate                           # Generate on-demand report

# Dashboard
GET    /api/v1/dashboard/capacity-overview                # Current capacity by region/type
GET    /api/v1/dashboard/forecast-timeline                # Forecast timeline chart data
GET    /api/v1/dashboard/cost-projection                  # Cost projections
```

### Agent Tool Call Interface

```json
{
  "tools": [
    {
      "name": "get_current_utilization",
      "description": "Get current utilization metrics for a resource type in a region",
      "parameters": {
        "resource_type": "string — 'compute', 'gpu', 'storage', 'memory'",
        "region": "string",
        "period": "string — 'current', '7d', '30d', '90d'"
      }
    },
    {
      "name": "get_capacity_forecast",
      "description": "Get demand forecast for a resource type",
      "parameters": {
        "resource_type": "string",
        "region": "string",
        "horizon_weeks": "int (2-12)",
        "scenario": "string — 'base', 'optimistic', 'pessimistic'"
      }
    },
    {
      "name": "get_hardware_inventory",
      "description": "Get inventory summary by type and region",
      "parameters": {
        "asset_type": "string (optional)",
        "region": "string (optional)",
        "status": "string — 'active', 'all'"
      }
    },
    {
      "name": "get_vendor_lead_times",
      "description": "Get delivery time estimates for a hardware SKU",
      "parameters": {
        "sku": "string"
      }
    },
    {
      "name": "run_cost_comparison",
      "description": "Compare cost options for meeting a capacity need",
      "parameters": {
        "resource_type": "string",
        "quantity": "int",
        "duration_months": "int",
        "options": ["purchase", "on_demand", "reserved_1yr", "reserved_3yr", "spot"]
      }
    },
    {
      "name": "create_procurement_recommendation",
      "description": "Create a procurement recommendation for review",
      "parameters": {
        "sku": "string",
        "quantity": "int",
        "region": "string",
        "urgency": "string",
        "reason": "string",
        "estimated_cost": "float"
      }
    },
    {
      "name": "run_what_if_simulation",
      "description": "Simulate a capacity scenario",
      "parameters": {
        "scenario": "object — {signal_type, multiplier, affected_resources, regions}"
      }
    },
    {
      "name": "generate_report_section",
      "description": "Generate a natural language report section using LLM",
      "parameters": {
        "section": "string — 'executive_summary', 'gpu_capacity', 'cost_analysis', 'recommendations'",
        "data": "object — relevant metrics and forecasts"
      }
    }
  ]
}
```

### Human Escalation API

```
POST /api/v1/alerts/capacity-critical
{
  "resource_type": "gpu",
  "region": "us-east-1",
  "severity": "critical",
  "message": "GPU capacity will be exhausted in 3 weeks under base scenario. No procurement orders in pipeline. H100 vendor lead time is 8 weeks. Action needed immediately.",
  "recommendation": {
    "sku": "NVIDIA-H100-SXM5-80GB",
    "quantity": 50,
    "vendor": "Dell",
    "estimated_delivery": "2026-06-04",
    "estimated_cost": "$1,750,000"
  },
  "escalation_targets": ["infra-leadership", "finance-approvers"]
}
```

---

## 6. Core Component Deep Dives

### 6.1 Demand Forecasting Engine

**Why it's hard:** Capacity planning requires long-horizon forecasts (weeks to months), which are inherently less accurate than short-term predictions. You must combine time-series trends with business signals (product launches, customer wins, seasonal patterns) that are qualitative and uncertain. Over-forecasting wastes millions in capital. Under-forecasting causes outages.

| Approach | Pros | Cons |
|---|---|---|
| **Linear trend extrapolation** | Simple, interpretable | Misses non-linear growth, seasonality |
| **Prophet (time-series decomposition)** | Handles seasonality, changepoints | Doesn't incorporate business signals natively |
| **Growth model (logistic/Bass curve)** | Models market saturation | Requires market size assumptions |
| **Composite: time-series + growth + business signals** | Most comprehensive | Complex; multiple model coordination |
| **LLM-augmented composite** | Natural language business signal integration | LLM not reliable for numerical prediction |

**Selected: Composite model (time-series + growth model + business signal adjustments)**

**Implementation:**

```python
class DemandForecaster:
    def forecast(self, resource_type, region, horizon_weeks):
        # 1. Time-series baseline (captures trend + seasonality)
        ts_forecast = self.prophet_model.predict(
            resource_type, region,
            horizon=horizon_weeks * 7 * 24,  # hourly forecasts
            history=365 * 24  # 1 year history
        )

        # 2. Growth adjustment (captures customer growth beyond trend)
        growth_signals = self.get_business_signals(resource_type, region)
        growth_multiplier = self.compute_growth_multiplier(growth_signals)

        # 3. Combine
        base_forecast = ts_forecast * growth_multiplier

        # 4. Generate scenarios
        scenarios = {
            'optimistic': base_forecast * 0.85,   # 15% less demand than expected
            'base': base_forecast,
            'pessimistic': base_forecast * 1.25,   # 25% more demand
            'stress': base_forecast * 1.50          # 50% more (worst case)
        }

        # 5. Aggregate to weekly peaks (capacity planning cares about peaks)
        for scenario in scenarios:
            scenarios[scenario] = self.aggregate_weekly_peaks(scenarios[scenario])

        return scenarios

    def compute_growth_multiplier(self, signals):
        """Combine business signals into a growth factor."""
        multiplier = 1.0
        for signal in signals:
            # Weight by confidence
            impact = (signal.impact_multiplier - 1.0) * signal.confidence
            multiplier += impact
        return multiplier
```

**Business signal integration:**

| Signal Type | Source | Example | Impact |
|---|---|---|---|
| Customer growth | Sales CRM pipeline | "Enterprise customer X committing 500 GPU-hours/day" | +12% GPU demand in 4 weeks |
| Product launch | Product roadmap | "New AI feature launching Q2 — requires 200 GPUs" | +200 GPU units by June |
| Seasonal pattern | Historical data | "Black Friday traffic 3x normal" | 3x compute in week 48 |
| Contract renewal | Finance system | "Customer Y's 3-year RI expiring — may not renew" | -5% compute if not renewed |
| Hardware EOL | CMDB | "200 Gen3 servers EOL in 8 weeks" | -200 compute units |

**Failure Modes:**
- **Over-optimistic growth signals**: Sales pipeline inflated. Mitigation: discount pipeline signals by historical win rate (typically 30-40%).
- **Black swan events**: Unexpected viral growth or customer churn. Mitigation: stress scenario always models 50% above base.
- **Seasonal model miss**: Holiday pattern different from last year. Mitigation: compare multiple years; weight recent years higher.

**Interviewer Q&As:**

**Q1: How do you handle the fact that business signals are qualitative and uncertain?**
A: Each business signal has a confidence field (0-1). A signed contract has confidence 0.95. A sales pipeline lead has confidence 0.3 (reflecting typical win rate). The growth multiplier is weighted by confidence. We also validate: after the signal's expected date, we compare actual impact to predicted. This calibrates future confidence weights.

**Q2: How do you forecast GPU demand specifically?**
A: GPU demand is driven by ML training jobs (bursty, schedulable) and inference serving (steady, growing). We model them separately. Training demand: forecasted from the ML team's training schedule + historical job patterns. Inference demand: forecasted from production model serving traffic (correlates with user traffic). We also track GPU utilization vs allocation — many teams reserve GPUs but don't fully utilize them. We forecast allocation demand (what teams request) not just utilization.

**Q3: How accurate are 12-week forecasts in practice?**
A: For compute (CPU/memory): MAPE 10-15% at 12 weeks (strong seasonal patterns help). For GPU: MAPE 15-25% (more volatile, driven by ML project timelines). For storage: MAPE 5-10% (storage grows monotonically, very predictable). The key insight: capacity planning doesn't need perfect accuracy. It needs to be directionally correct and err on the side of slightly over-provisioning. A 20% over-forecast costs money; a 20% under-forecast causes outages.

**Q4: Why not just use cloud elasticity and avoid capacity planning?**
A: Three reasons. (1) For on-premises hardware (40% of our fleet), cloud elasticity doesn't apply — hardware needs to be ordered weeks ahead. (2) Even in cloud, reserved instances save 30-60% but require commitment. Capacity planning determines optimal RI portfolio. (3) GPU instances are capacity-constrained at cloud providers — you can't always get them on demand. Reservations ensure availability.

**Q5: How do you handle hardware end-of-life in forecasting?**
A: The CMDB tracks warranty and EOL dates. The forecast engine subtracts EOL capacity from the supply side. If 200 servers are EOL in 8 weeks, the capacity gap for week 9+ increases by 200 servers (unless replacements are ordered). This often triggers procurement recommendations — "Order 200 Gen5 servers to replace EOL Gen3 fleet."

**Q6: How do you model procurement lead times?**
A: We maintain a statistical model per SKU per vendor based on historical order data. Lead time is modeled as a distribution (not a point estimate): P50 = 6 weeks, P90 = 10 weeks for typical servers. For GPUs, P50 = 12 weeks, P90 = 16 weeks currently. The recommendation engine uses P90 as the planning lead time (buffer for delays). If actual delivery is faster, we get capacity earlier (good). If it's at P90, we're on time.

---

### 6.2 Cost Optimization Engine

**Why it's hard:** The cost landscape is complex: purchase vs. lease vs. on-demand vs. reserved (1yr vs 3yr) vs. spot. Each option has different unit economics, commitment risk, and flexibility trade-offs. The optimizer must consider current utilization, forecasted growth, discount rates, and stranded cost risk (paying for reserved capacity you don't use).

| Approach | Pros | Cons |
|---|---|---|
| **Simple threshold rules** | Easy to implement ("if > 60% utilized, buy RI") | Misses nuanced scenarios |
| **Linear programming (optimization)** | Mathematically optimal | Assumes linear cost functions; may not handle all constraints |
| **Mixed-integer programming** | Handles discrete purchasing decisions | Computationally expensive for large portfolios |
| **Simulation-based (Monte Carlo)** | Handles uncertainty in demand | Slow; hard to find optimal |
| **Hybrid: LP for base + Monte Carlo for risk** | Optimal + risk-aware | Most complex |

**Selected: Mixed-integer programming with Monte Carlo risk simulation**

**Implementation:**

```python
class CostOptimizer:
    def optimize(self, demand_forecasts, current_portfolio, budget):
        """
        Find optimal mix of purchase/RI/on-demand/spot to meet demand.

        Decision variables:
        - x_purchase[sku][region] = number of units to purchase (integer)
        - x_ri_1yr[sku][region] = number of 1-year RIs to buy (integer)
        - x_ri_3yr[sku][region] = number of 3-year RIs to buy (integer)
        - x_on_demand[sku][region][week] = on-demand units per week (continuous)
        - x_spot[sku][region][week] = spot units per week (continuous)

        Objective: minimize total cost over 12-month horizon

        Constraints:
        - Supply >= Demand for each resource_type, region, week
        - Budget: total capex <= annual budget
        - Spot: x_spot <= 30% of total (reliability constraint)
        - Minimum on-prem ratio: >= 40% for compliance
        """

        problem = MixedIntegerProgram()

        # Add decision variables
        for sku in SKUS:
            for region in REGIONS:
                problem.add_integer_var(f'purchase_{sku}_{region}', lb=0)
                problem.add_integer_var(f'ri_1yr_{sku}_{region}', lb=0)
                problem.add_integer_var(f'ri_3yr_{sku}_{region}', lb=0)
                for week in range(52):
                    problem.add_continuous_var(f'od_{sku}_{region}_{week}', lb=0)
                    problem.add_continuous_var(f'spot_{sku}_{region}_{week}', lb=0)

        # Objective: minimize total cost
        total_cost = sum_all_costs(problem.vars, COST_CATALOG)
        problem.minimize(total_cost)

        # Constraints: meet demand
        for week in range(52):
            for resource_type in RESOURCE_TYPES:
                for region in REGIONS:
                    supply = get_supply(problem.vars, resource_type, region, week)
                    demand = demand_forecasts[resource_type][region][week]
                    problem.add_constraint(supply >= demand)

        # Solve
        solution = problem.solve(timeout=120)
        return self.format_recommendations(solution)
```

**Cost comparison output:**

```
Resource Need: 50 H100 GPUs in us-east-1 for 12 months

Option 1: Purchase (own hardware)
  Cost: $1,750,000 one-time + $250,000/yr operating
  TCO (3 year): $2,500,000 ($83,333/month, $69.4/GPU-hour)
  Pros: Full control, no recurring cost after 3yr amortization
  Cons: CapEx heavy, 12-week lead time, obsolescence risk

Option 2: AWS Reserved Instances (p5.48xlarge, 3-year)
  Cost: $45,000/month * 50 = $2,250,000/yr (at 60% discount)
  TCO (3 year): $6,750,000 ($187,500/month, $156/GPU-hour)
  Pros: No lead time, no hardware management
  Cons: Locked in for 3 years, more expensive than purchase

Option 3: AWS On-Demand (p5.48xlarge)
  Cost: $112,500/month * 50 = $5,625,000/yr (no discount)
  TCO (1 year): $5,625,000 ($468,750/month, $390/GPU-hour)
  Pros: Maximum flexibility, no commitment
  Cons: 5.6x more expensive than purchase

Option 4: Hybrid (purchase 35 + on-demand 15)
  Cost: $1,225,000 purchase + $1,687,500/yr on-demand
  TCO (3 year): $6,287,500 ($174,653/month, $145/GPU-hour)
  Pros: Balanced — base load owned, burst capacity cloud
  Cons: Still requires lead time for purchases

RECOMMENDATION: Option 4 (Hybrid) — meets 70% base demand with
owned hardware and 30% burst with cloud flexibility.
```

**Interviewer Q&As:**

**Q1: How do you handle the uncertainty in demand when making commitment decisions?**
A: We run the optimizer across all four demand scenarios. The recommendation is the intersection that works for at least the base case. For commitments (purchase, RI), we size to the pessimistic scenario P25 (only commit to capacity we're 75% sure we'll need). For flexible capacity (on-demand, spot), we plan for the gap between committed and peak demand.

**Q2: What about stranded RI cost?**
A: We model RI utilization rate. If a team's RI utilization drops below 60%, we recommend shifting the RI to another team (AWS allows RI sharing within an org) or selling on the RI marketplace. We track "RI waste" as a metric: cost of unused RI capacity / total RI spend. Target: < 5%.

**Q3: How do you model the cost of over-provisioning vs under-provisioning?**
A: We assign asymmetric costs. Under-provisioning: $X per user-minute of degradation (derived from SLA penalties and revenue impact). For a P0 outage: $10K/minute. Over-provisioning: cost of idle resources. Typically, under-provisioning is 100-1000x more expensive per unit time. This asymmetry drives the "err on the side of over-provisioning" strategy.

**Q4: How often should you re-optimize the portfolio?**
A: Monthly for the full optimization. Weekly for incremental adjustments (new signals, demand changes). The full optimization runs the MIP solver (~2 min); the weekly update is a lighter-weight check of whether current capacity meets updated forecasts.

**Q5: How do you handle multi-region cost optimization?**
A: The optimizer considers region as a dimension. Some workloads are region-constrained (data sovereignty, latency). Others can move. The optimizer can recommend shifting workloads to cheaper regions if the latency impact is acceptable. For example, batch ML training can run in the cheapest region, while serving must stay close to users.

**Q6: How does the LLM add value in cost optimization?**
A: The LLM doesn't make the optimization decision — that's MIP. The LLM adds value by: (1) Translating complex cost analysis into executive-readable summaries. (2) Answering natural language queries: "Why is GPU cost 40% higher than last quarter?" → Agent queries cost data, identifies H100 shortage pricing, and explains. (3) Generating procurement justification documents from the optimization output.

---

### 6.3 LLM-Powered Reporting and Natural Language Interface

**Why it's hard:** Capacity planning data is dense and multi-dimensional. Leadership needs concise, actionable summaries, not 500-row spreadsheets. The LLM must accurately represent quantitative data (no hallucinated numbers), explain trade-offs, and adapt its communication style to the audience (engineering vs. finance vs. executive).

| Approach | Pros | Cons |
|---|---|---|
| **Template-based reports** | Reliable, no hallucination | Rigid; can't answer ad-hoc questions |
| **LLM generates from raw data** | Flexible, natural language | Hallucination risk on numbers |
| **LLM generates from structured summaries** | Lower hallucination; flexible | Need to pre-compute summaries |
| **Hybrid: templates for numbers + LLM for narrative** | Reliable numbers, flexible explanation | More implementation effort |

**Selected: Hybrid — structured data + LLM narrative**

**Implementation:**

```python
class ReportGenerator:
    async def generate_weekly_report(self, forecast_run_id):
        # 1. Compute structured data (no LLM — deterministic)
        data = {
            'capacity_summary': self.compute_capacity_summary(forecast_run_id),
            'critical_gaps': self.get_critical_gaps(forecast_run_id),
            'cost_projection': self.compute_cost_projection(forecast_run_id),
            'recommendations': self.get_pending_recommendations(forecast_run_id),
            'accuracy_metrics': self.get_forecast_accuracy(),
        }

        # 2. Generate narrative sections (LLM)
        sections = []
        for section_name in ['executive_summary', 'critical_alerts', 'cost_analysis',
                              'recommendations', 'risk_assessment']:
            prompt = self.build_prompt(section_name, data)
            narrative = await self.llm.generate(
                system="You are a capacity planning analyst. Generate clear, concise reports. "
                       "NEVER invent numbers — only use the data provided. "
                       "Include specific dates, quantities, and dollar amounts from the data.",
                user=prompt,
                max_tokens=1000
            )
            sections.append({'title': section_name, 'content': narrative})

        # 3. Assemble report with data tables + narrative
        report = self.assemble_report(data, sections)

        # 4. Validate: check that all numbers in narrative appear in source data
        validation = self.validate_numbers(report, data)
        if not validation.passed:
            report.add_warning("Some narrative content could not be validated against source data.")

        return report
```

**Example generated executive summary:**

```
WEEKLY CAPACITY REPORT — Week of April 7, 2026

CRITICAL: GPU capacity in us-east-1 will be exhausted within 6 weeks under
the base scenario. Current utilization is 78% with a growth rate of 4.2% per
week. With 50 H100 GPUs in the pipeline (ordered March 15, estimated delivery
May 28), there is a 2-week gap where demand exceeds supply.

RECOMMENDED ACTION: Order 30 additional H100 GPUs immediately (estimated cost
$1,050,000). With P90 lead time of 12 weeks, delivery by July 1 provides buffer
for pessimistic demand scenario.

COST UPDATE: Total infrastructure spend is tracking 8% under budget ($4.2M vs
$4.6M budget) due to favorable spot pricing and delayed customer onboarding.
Q2 projection remains within budget under base and optimistic scenarios. Under
pessimistic scenario, Q2 spend exceeds budget by 12% — primarily driven by GPU
procurement.

DECOMMISSION OPPORTUNITY: 85 Gen3 servers in us-west-2 are operating at < 15%
utilization. Migrating remaining workloads to Gen5 would save $127K/year in power
and maintenance.
```

**Failure Modes:**
- **LLM hallucinates a number**: Mitigated by number validation — cross-reference all numbers in the output against the source data. Flag discrepancies.
- **LLM misinterprets urgency**: Mitigated by computing urgency scores deterministically; LLM only narrates the pre-computed urgency.
- **LLM generates overly optimistic/pessimistic tone**: Mitigated by including all scenario data and instructing the LLM to present the range, not a single point estimate.

**Interviewer Q&As:**

**Q1: How do you prevent LLM hallucination in financial reports?**
A: Three layers. (1) All numbers are pre-computed and passed to the LLM as structured data — the LLM narrates, not calculates. (2) Post-generation validation: we parse all numbers from the LLM output and verify each one appears in the source data. (3) The report includes both the narrative section (LLM-generated) and data tables (deterministic). Readers can cross-reference.

**Q2: Can stakeholders ask follow-up questions?**
A: Yes. The natural language interface supports multi-turn conversation. Stakeholders can ask: "What if we delay the H100 order by 2 weeks?" → The agent runs a what-if simulation and generates an updated analysis. The conversation context includes the current report data, so follow-ups are coherent.

**Q3: How do you handle different audiences?**
A: The report generation prompt includes audience context. For engineering: include technical details (SKU names, utilization metrics, P95 values). For finance: focus on cost, ROI, budget variance. For executives: 3-bullet summary with decision points. The same data drives all versions; only the narrative style changes.

**Q4: How often are reports generated?**
A: Automated weekly report every Monday morning. On-demand reports for ad-hoc requests. Event-triggered reports when a critical capacity gap is detected (immediate escalation). Monthly deep-dive report with trend analysis.

**Q5: What's the cost of LLM usage for reporting?**
A: Minimal. ~25 LLM calls/week, average 4K input + 1K output tokens each. At Claude Sonnet pricing: ~$0.33/week. Less than $20/year. The value of automated, readable reports for 50 stakeholders far exceeds this cost.

**Q6: Can the LLM generate procurement justification documents automatically?**
A: Yes. When a procurement recommendation is approved, the LLM generates a justification document including: business case (which teams need the capacity, what workloads), financial analysis (TCO comparison), risk assessment (what happens if we don't buy), and timeline (when it's needed, vendor lead time). The human reviewer approves the document before submission.

---

## 7. AI Agent Architecture

### Agent Loop Design

```
┌──────────────────────────────────────────────────────────────┐
│              CAPACITY PLANNING AGENT (weekly cycle)           │
│                                                               │
│  1. OBSERVE (Monday 6:00 AM)                                  │
│     ├── Pull latest utilization data (all regions, types)     │
│     ├── Pull updated business signals (CRM, roadmap)          │
│     ├── Check hardware inventory changes (new, EOL, failed)   │
│     ├── Check existing procurement order status                │
│     └── Check cost catalog updates (pricing changes)          │
│                                                               │
│  2. REASON (Monday 6:15 AM)                                   │
│     ├── Run demand forecasts (all scenarios)                  │
│     ├── Compute capacity gaps                                 │
│     ├── Run cost optimizer                                    │
│     ├── Generate procurement recommendations                  │
│     ├── Identify workload shift opportunities                 │
│     └── Identify decommission opportunities                   │
│                                                               │
│  3. ACT (Monday 6:30 AM)                                      │
│     ├── Generate weekly report (LLM-powered)                  │
│     ├── Send report to stakeholders                           │
│     ├── Create critical alerts for urgent gaps                │
│     ├── Submit routine procurement recommendations            │
│     └── Update dashboard data                                 │
│                                                               │
│  4. VERIFY (ongoing)                                          │
│     ├── Track forecast accuracy (actual vs predicted weekly)  │
│     ├── Track recommendation acceptance rate                  │
│     ├── Track procurement delivery vs estimated lead time     │
│     └── Trigger model retraining if accuracy degrades         │
└──────────────────────────────────────────────────────────────┘
```

### Tool Definitions

| Tool | Description | Used In Phase |
|---|---|---|
| `query_utilization_warehouse` | SQL queries against utilization history | OBSERVE |
| `get_business_signals` | Fetch active business signals | OBSERVE |
| `get_hardware_inventory` | Current inventory status | OBSERVE |
| `get_procurement_orders` | Status of existing orders | OBSERVE |
| `run_forecast_model` | Execute demand forecast | REASON |
| `compute_capacity_gap` | Calculate supply - demand gap | REASON |
| `run_cost_optimizer` | MIP solver for cost optimization | REASON |
| `generate_llm_narrative` | LLM-powered report generation | ACT |
| `send_notification` | Email/Slack reports and alerts | ACT |
| `create_procurement_request` | Submit procurement recommendation | ACT |
| `update_dashboard` | Refresh dashboard data | ACT |

### Context Window Management

LLM usage is limited to report generation and natural language queries. Each LLM call receives:

| Section | Tokens | Content |
|---|---|---|
| System prompt | 200 | "You are a capacity planning analyst..." |
| Structured data (pre-computed) | 2,000 | Key metrics, gaps, recommendations as JSON |
| Report template | 300 | Expected format/structure |
| Previous context (for follow-ups) | 1,000 | Last report section or query |
| **Total** | **~3,500** | Well within limits |

### Memory Architecture

| Memory Type | Content | Storage |
|---|---|---|
| **Episodic** | Past forecasts and their accuracy; past procurement decisions and outcomes | PostgreSQL (capacity_forecasts, procurement_recommendations) |
| **Semantic** | Hardware catalog, vendor capabilities, cost models | PostgreSQL (cost_catalog, vendor_lead_times) |
| **Procedural** | Forecasting models, optimization algorithms | Code + model artifacts in S3 |
| **Working** | Current planning cycle data | In-memory during planning run |

### Guardrails and Safety

1. **Budget caps**: Cannot recommend procurement exceeding quarterly budget without escalation to VP-level.
2. **Quantity limits**: Single recommendation capped at $5M. Larger requests auto-escalate.
3. **Forecast sanity checks**: Reject forecasts showing > 100% growth in 4 weeks (unless business signal explains it).
4. **Number validation**: All LLM-generated reports validated against source data.
5. **Approval gates**: All procurement requires human approval (no auto-ordering).

### Confidence Thresholds

| Confidence Level | Action |
|---|---|
| High (MAPE < 10%, strong business signal) | Recommend procurement with "high confidence" label |
| Medium (MAPE 10-20%) | Recommend with range (min/max quantity) |
| Low (MAPE > 20% or conflicting signals) | Flag for human judgment; provide scenarios |
| Unknown (new resource type, insufficient data) | Do not recommend; request manual planning |

### Dry-Run Mode

```yaml
dry_run_output:
  planning_cycle: "2026-04-07"
  resource_summary:
    - type: gpu
      region: us-east-1
      current_capacity: 400 H100 units
      current_utilization: 78%
      forecast_6wk_base: 520 units
      gap_6wk_base: -120 units (shortfall)
      recommendation: "Order 50 H100 immediately + 30 within 2 weeks"
      estimated_cost: "$2,800,000"
      status: "would_create_procurement_request"

    - type: compute
      region: us-west-2
      current_capacity: 3000 cores
      current_utilization: 45%
      forecast_6wk_base: 2800 cores
      gap_6wk_base: +200 (surplus)
      recommendation: "Decommission 85 Gen3 servers (save $127K/yr)"
      status: "would_create_decommission_request"

  report_preview:
    executive_summary: "[LLM-generated preview]"
    would_send_to: ["infra-leadership@company.com", "#capacity-planning"]
```

---

## 8. Scaling Strategy

| Component | Scaling Approach | Notes |
|---|---|---|
| Data warehouse | PostgreSQL read replicas or migrate to Snowflake | Dashboard queries are read-heavy |
| Forecasting engine | Horizontal (parallel by resource type/region) | Each forecast is independent |
| Cost optimizer (MIP) | Vertical (single solver, more CPU/memory) | MIP solvers don't parallelize well |
| LLM report generation | Serial (low volume) | ~25 calls/week; scaling unnecessary |
| ETL pipeline | Airflow worker scaling | More workers for parallel ingestion |

### Interviewer Q&As

**Q1: How does this scale to 100,000 resources across 20 regions?**
A: The forecasting engine parallelizes by (resource_type, region). With 20 regions and 6 resource types, that's 120 parallel forecast jobs — each taking ~10 seconds. Total: ~10 seconds (parallel), not 120 * 10 = 20 minutes (serial). The MIP solver is the bottleneck: more decision variables. Solution: decompose by region (solve 20 smaller problems instead of one large one), then reconcile cross-region constraints.

**Q2: What's the computational cost of the MIP solver?**
A: For our scale (60K resources, ~500 SKUs, 52 weeks): ~2 minutes on a 16-core machine. The solver is CBC (open-source) or Gurobi (commercial, faster). At 100K resources, Gurobi solves in ~5 minutes; CBC in ~15 minutes. Annual Gurobi license: ~$12K. Worth it if the optimization saves millions in procurement.

**Q3: How do you handle data quality issues (missing utilization data)?**
A: The ETL pipeline performs data quality checks. Missing data points are filled with: (1) Forward-fill for gaps < 1 hour. (2) Seasonal interpolation for gaps 1-24 hours. (3) Excluded from training if gaps > 24 hours (that day is dropped). We track a "data completeness" metric: must be > 95% to include in forecasting.

**Q4: Can this handle multi-tenant capacity planning?**
A: Yes. Each team/tenant has tagged resources. Forecasts and recommendations are generated per-tenant. The cost optimizer respects per-tenant budgets. The report generator creates tenant-specific views. Cross-tenant aggregation provides the global view for infrastructure leadership.

**Q5: How do you handle emergency capacity requests outside the weekly cycle?**
A: The on-demand forecast API supports ad-hoc requests. An engineer can submit: "ML team needs 100 H100 GPUs in 3 weeks for a new training job." The system immediately runs a forecast, checks available capacity, and either allocates from existing pool or generates an urgent procurement recommendation.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| **Monitoring data stale** | Forecasts based on old utilization | Data freshness check | Alert if data > 2 hours old. Use last known values with "stale data" warning. |
| **Forecast model diverges** | Wrong capacity recommendations | MAPE tracking | Auto-flag if weekly MAPE > 25%. Fall back to simple trend extrapolation. |
| **MIP solver timeout** | No cost optimization results | Solver status check | Return best feasible solution found before timeout (MIP solvers produce increasingly better solutions). |
| **LLM hallucination in report** | Incorrect numbers in executive report | Number validation | Validate all numbers; fall back to template-based report if validation fails. |
| **Business signal incorrect** | Forecast too high or too low | Actual vs forecast tracking | Track signal accuracy per source. Reduce confidence weight for unreliable sources. |
| **Procurement system down** | Cannot submit POs | Health check | Queue recommendations; manual fallback via email to procurement. |
| **ETL pipeline failure** | No fresh data for planning cycle | Airflow monitoring | Retry pipeline. If still failing, run planning with last week's data + stale warning. |

### AI Safety Controls

1. **All procurement requires human approval** — the system recommends but never auto-orders.
2. **Report validation**: Numbers in LLM output verified against source data.
3. **Forecast accuracy SLA**: If accuracy drops below threshold, report includes explicit warning.
4. **Budget guard**: Cannot recommend spend exceeding budget without VP escalation.
5. **Audit trail**: Every forecast, recommendation, and report stored with full provenance.

---

## 10. Observability

### Key Metrics

| Metric | Type | Target | Alert Threshold |
|---|---|---|---|
| `forecast.mape_2wk` | Gauge (by resource type) | < 10% | > 15% |
| `forecast.mape_12wk` | Gauge | < 25% | > 35% |
| `recommendation.acceptance_rate` | Gauge | > 80% | < 60% |
| `recommendation.lead_time_accuracy` | Gauge | Actual within P90 estimate | Delivery > P90 for 3+ orders |
| `capacity.utilization_pct` | Gauge (by type, region) | 60-80% | > 85% (tight) or < 30% (waste) |
| `capacity.weeks_to_exhaustion` | Gauge | > 8 weeks | < 4 weeks |
| `cost.budget_variance_pct` | Gauge | < 10% | > 15% |
| `cost.ri_utilization_pct` | Gauge | > 80% | < 60% (RI waste) |
| `data.freshness_hours` | Gauge | < 2 | > 6 |
| `data.completeness_pct` | Gauge | > 95% | < 90% |
| `report.llm_validation_pass_rate` | Gauge | 100% | < 95% |
| `pipeline.cycle_duration_min` | Histogram | < 30 min | > 60 min |

### Agent Action Audit Trail

```json
{
  "planning_cycle_id": "2026-W15",
  "timestamp": "2026-04-07T06:30:00Z",
  "phase": "act",
  "action": "create_procurement_recommendation",
  "details": {
    "sku": "NVIDIA-H100-SXM5-80GB",
    "quantity": 50,
    "region": "us-east-1",
    "urgency": "immediate",
    "cost": "$1,750,000",
    "justification": "GPU utilization at 78%, forecast to exceed capacity in 6 weeks under base scenario. No existing pipeline orders for H100 in us-east-1."
  },
  "forecast_basis": {
    "model": "prophet_gpu_use1_v4",
    "mape": 0.12,
    "scenario": "base",
    "confidence": "medium"
  },
  "approval_status": "pending",
  "sent_to": ["infrastructure-vp@company.com"]
}
```

---

## 11. Security

### Principle of Least Privilege

| Component | Permissions |
|---|---|
| Data ingestion | Read-only on monitoring systems, CMDB, CRM |
| Forecasting engine | Read data warehouse, write forecasts |
| Cost optimizer | Read cost catalog + forecasts, write recommendations |
| Report generator | Read all planning data, invoke LLM API |
| Procurement integration | Write to procurement system (human-gated) |
| Dashboard | Read-only on all planning data |

### Audit Logging

- Every forecast run, recommendation, and report logged with full provenance.
- Cost data access logged (financial data is sensitive).
- LLM interactions logged with prompts and responses.
- Procurement approvals logged with approver identity.

### Human Approval Gates

| Action | Approval Required |
|---|---|
| Generate forecast/report | None (automated) |
| Send report to stakeholders | None (scheduled) |
| Create procurement recommendation | None (recommendation only) |
| Submit procurement order (PO) | VP-level approval for > $100K |
| Decommission hardware | Team owner + infra lead approval |
| Shift workload between regions | Team owner approval |

---

## 12. Incremental Rollout

### Rollout Phases

| Phase | Duration | Scope |
|---|---|---|
| **Phase 0: Data integration** | 4 weeks | Build ETL pipelines, validate data quality |
| **Phase 1: Forecast only** | 4 weeks | Generate forecasts, compare to manual planning |
| **Phase 2: Automated reports** | 4 weeks | LLM-generated weekly reports, sent to stakeholders |
| **Phase 3: Procurement integration** | 4 weeks | Recommendations feed into procurement workflow |
| **Phase 4: Full automation** | Ongoing | End-to-end: forecast → recommend → report → PO draft |

### Rollout Interviewer Q&As

**Q1: How do you validate forecasts before trusting them for procurement?**
A: Phase 1 runs forecasts in parallel with the existing manual planning process. We compare AI forecasts to human forecasts for 4 weeks. If AI achieves MAPE within 5% of human planner accuracy, we proceed. In practice, AI often outperforms humans on compute/storage (better at processing thousands of time-series) but underperforms on GPU (where business context matters more).

**Q2: What's the biggest risk in Phase 3 (procurement integration)?**
A: Over-ordering. If the forecast is systematically biased high, we could commit millions to unnecessary hardware. Mitigation: (1) Start with recommendations for non-committed resources (on-demand cloud, not hardware purchases). (2) Cap auto-recommended quantities at 80% of gap (leave 20% buffer for human judgment). (3) All POs require human approval.

**Q3: How do you handle the organizational change?**
A: Capacity planning is often done by experienced engineers with deep institutional knowledge. The system must augment, not replace them. Phase 2 (reports) is where adoption happens — if the AI-generated report is better than the manual one, planners adopt it. We frame the tool as "freeing the planner from data gathering to focus on strategic decisions."

**Q4: What if stakeholders don't trust the AI recommendations?**
A: The report always includes: (1) The data that drove the recommendation (transparency). (2) The confidence level and scenario range (honesty about uncertainty). (3) Historical accuracy metrics (track record). (4) Comparison to what the human planner would have recommended (calibration). Trust builds over time as stakeholders see accurate forecasts and cost savings.

**Q5: How do you handle the fact that procurement lead times change?**
A: We continuously update lead time models from actual delivery data. If NVIDIA H100 lead times suddenly increase from 12 to 20 weeks (supply chain issue), the next planning cycle reflects this — recommendations become more urgent, and the report explains why.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Chosen | Rationale |
|---|---|---|---|
| Forecast model | Single model, per-service, ensemble | Per-resource-type with scenario analysis | Different resource types have different patterns; scenarios handle uncertainty |
| Cost optimizer | Rules, LP, MIP, simulation | MIP + Monte Carlo | MIP gives optimal discrete decisions; Monte Carlo adds risk analysis |
| LLM usage | Core forecasting, reporting only, none | Reporting + NL interface only | ML models outperform LLMs for time-series; LLMs excel at narration |
| Report validation | None, manual review, automated number check | Automated number validation + human review for critical reports | Catches hallucination without slowing report generation |
| Procurement automation | Full auto, recommendation only, manual | Recommendation with human approval gate | Procurement involves millions of dollars; human judgment essential |
| Planning horizon | 4 weeks, 12 weeks, 52 weeks | 12 weeks primary, 52 weeks for cost optimization | 12 weeks balances accuracy with lead time coverage |
| Data warehouse | PostgreSQL, Snowflake, BigQuery | PostgreSQL (< 200 GB data) | Sufficient at our scale; migrate to Snowflake if data grows 10x |

---

## 14. Complete Interviewer Q&A Bank

**Q1: How does capacity planning differ from autoscaling?**
A: Autoscaling operates in minutes-to-hours: add pods, launch VMs. Capacity planning operates in weeks-to-months: order hardware, commit to reserved instances, plan datacenter space. They complement each other. Autoscaling handles demand within available capacity. Capacity planning ensures sufficient capacity exists. Without capacity planning, autoscaling hits ceiling ("no more GPUs available") at the worst possible time.

**Q2: How do you handle the GPU shortage? You can't always order what you want.**
A: Three strategies. (1) Buffer stock: maintain 20% GPU headroom (more than compute because lead times are longer). (2) Multi-vendor diversification: don't rely on one supplier. Maintain relationships with Dell, HPE, Supermicro for server builds, and multiple cloud providers for burst. (3) Demand management: if supply is constrained, the system generates allocation recommendations (which teams get GPU priority) based on business value.

**Q3: How do you forecast demand for a resource type you've never used before (e.g., first-time TPU deployment)?**
A: Cold start problem. (1) Use analogous resource forecasts — TPU demand might track GPU demand with a conversion factor. (2) Use the business signal approach — the team requesting TPUs provides expected usage. (3) Start with on-demand/flexible capacity and build a usage history before committing.

**Q4: What's the TCO model for on-prem vs cloud?**
A: On-prem TCO includes: hardware purchase (amortized 3-5 years), datacenter space ($150-300/kW/month), power ($0.06-0.12/kWh), cooling (PUE 1.2-1.5x power), network connectivity, maintenance (2-4% of purchase price/year), staffing (ops team), and decommission cost. Cloud TCO: instance cost * hours. On-prem is typically 40-60% cheaper for steady-state workloads at scale, but has higher upfront cost, longer lead time, and less flexibility.

**Q5: How do you handle seasonality in capacity planning?**
A: Two levels. (1) Short-term seasonality (daily/weekly): handled by autoscaling, not capacity planning. (2) Long-term seasonality (quarterly/yearly): retail has Q4 peaks, tax software has Q1 peaks. The forecast model captures this from historical data. We also pre-allocate cloud burst capacity (reserved instances for the peak month) and use on-demand for the margin.

**Q6: How do you validate the cost optimizer's recommendations?**
A: Backtesting. We run the optimizer on historical data (what it would have recommended 6 months ago) and compare to what was actually ordered. We calculate the cost difference: would the optimizer's recommendation have been cheaper while still meeting demand? In testing, the optimizer typically identifies 15-25% cost savings vs. manual procurement.

**Q7: How do you handle multi-year commitment decisions (3-year RIs)?**
A: 3-year RIs offer 60% discount but lock you in. The optimizer only recommends 3-year RIs for workloads with > 90% confidence of persisting (based on contract duration, service criticality). For uncertain workloads, it recommends 1-year RIs or on-demand. We also model early termination risk: if we estimate 10% chance of not needing the capacity, the effective cost of the 3-year RI includes 10% * remaining commitment as waste.

**Q8: How does the LLM agent handle natural language queries like "What if we lose us-west-2?"**
A: The agent: (1) Parses the query to identify the scenario (region loss). (2) Runs the what-if simulation: remove all us-west-2 capacity from supply, keep demand constant (workloads shift to other regions). (3) Computes gaps in remaining regions. (4) Runs the cost optimizer for the new scenario. (5) Generates a report: "Losing us-west-2 would create a 30% compute gap in the remaining regions. To maintain service, you'd need to scale us-east-1 by 35% and eu-west-1 by 25%. Estimated additional cost: $180K/month in on-demand instances until replacement capacity is procured."

**Q9: How do you handle the organizational politics of capacity allocation?**
A: The system provides data-driven allocation recommendations. When demand exceeds supply (especially GPUs), it recommends allocation based on: (1) Revenue impact (customer-facing workloads first). (2) Contract commitments (SLA-bound workloads). (3) Efficiency (GPU utilization per team). (4) Strategic priority (ML initiatives designated by leadership). The system doesn't enforce allocation — it provides the data for leadership decisions.

**Q10: How do you track whether a recommendation was actually a good one?**
A: We track "recommendation quality" retrospectively. For each recommendation that was executed: (1) Was the capacity actually needed? (utilization > 50% within forecast window = yes). (2) Was the timing right? (capacity available before demand = yes). (3) Was the cost optimal? (compare actual spend to optimizer's projection). We aggregate this into a "recommendation ROI" metric.

**Q11: Can this system handle hybrid cloud optimization (when to burst to cloud vs. buy on-prem)?**
A: Yes. The cost optimizer models both on-prem and cloud options. For each capacity gap, it compares: (1) On-prem purchase TCO. (2) Cloud reserved instance TCO. (3) Cloud on-demand TCO. (4) Hybrid: meet base demand on-prem, burst to cloud. The optimizer's objective function minimizes total cost subject to constraints (latency, data sovereignty, minimum on-prem ratio). The recommendation includes explicit "buy on-prem" vs. "use cloud" decisions per workload.

**Q12: How often do you need to update the cost catalog?**
A: Cloud pricing changes frequently (new instance types, price drops). We automate cost catalog updates: daily scrape of AWS/GCP pricing APIs, weekly update of hardware vendor quotes, monthly update of internal operating costs (power, space). The cost optimizer uses the latest catalog for each planning run.

**Q13: What's the most valuable insight this system can provide that a human planner can't?**
A: Cross-dimensional optimization. A human planner can optimize GPU procurement or RI portfolio or regional distribution — but optimizing all three simultaneously across 60K resources is beyond human cognitive capacity. The MIP solver finds combinations that save 15-25% more than the sum of individual optimizations. Example: "Shift ML training workloads to eu-west-1 (cheaper power) AND convert to 3-year RI (higher discount) AND decommission Gen3 GPU servers (offset CapEx)" — this three-way optimization is the system's superpower.

**Q14: How do you handle the "garbage in, garbage out" problem with business signals?**
A: (1) Track signal accuracy per source over time. If a particular sales rep's pipeline estimates are consistently 2x actual, we apply a 0.5 confidence multiplier. (2) Require signals to have a confidence field at creation. (3) After signal date passes, compare actual impact to predicted. (4) Dashboard shows a "signal reliability scorecard" — makes data quality visible and creates accountability.

**Q15: How would you extend this to handle carbon/sustainability planning?**
A: Add carbon as a dimension alongside cost. Each SKU has a carbon footprint (manufacturing + operating). Each region has a power carbon intensity (g CO2/kWh). The optimizer can include a carbon budget constraint or a carbon cost ($X per ton). This changes recommendations: "eu-north-1 (Sweden, 99% renewable) is $5K/month more but saves 40 tons CO2/year." Leadership decides the carbon price that balances cost vs sustainability goals.

---

## 15. References

1. **Hyndman, R.J. & Athanasopoulos, G.** — "Forecasting: Principles and Practice" (2021): https://otexts.com/fpp3/
2. **Taylor, S.J. & Letham, B.** — "Forecasting at Scale" (Prophet, 2018)
3. **AWS Reserved Instance documentation**: https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-reserved-instances.html
4. **Google Cloud Committed Use Discounts**: https://cloud.google.com/compute/docs/instances/committed-use-discounts-overview
5. **COIN-OR CBC** — Open-source MIP solver: https://github.com/coin-or/Cbc
6. **Gurobi Optimization**: https://www.gurobi.com/
7. **Anthropic** — "Claude for enterprise": https://www.anthropic.com/claude
8. **Uptime Institute** — "Data Center TCO Analysis" (annual report)
9. **NVIDIA H100 specifications**: https://www.nvidia.com/en-us/data-center/h100/
10. **Apache Airflow**: https://airflow.apache.org/
