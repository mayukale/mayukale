# System Design: Capacity Planning System

> **Relevance to role:** A cloud infrastructure platform engineer must ensure the fleet has enough capacity to meet current and future demand while minimizing waste. For bare-metal infrastructure, capacity planning is uniquely critical — hardware procurement has a 3-6 month lead time, and you can't auto-scale physical servers in minutes. This system provides demand forecasting, utilization analysis (USE method), procurement triggers, multi-region balancing, and cost modeling across reserved, on-demand, and spot capacity tiers.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Continuously collect capacity metrics: utilization, saturation, and availability (USE method) across all resource types (CPU, RAM, GPU, disk, network) per host, cluster, region, and globally. |
| FR-2 | Forecast future demand using time-series models: linear regression, ARIMA, Prophet, and ensemble methods. |
| FR-3 | Generate procurement recommendations with lead time awareness (3-6 months for bare metal, minutes for cloud VMs). |
| FR-4 | Define and enforce capacity buffer targets (e.g., maintain 20% headroom). Alert when buffer drops below threshold. |
| FR-5 | Simulate capacity scenarios: "What if traffic grows 50%?", "What if we lose a region?", "What if we add 500 GPU nodes?" |
| FR-6 | Track and model procurement costs: on-demand, reserved instances, spot pricing, bare-metal CapEx/OpEx. |
| FR-7 | Multi-region capacity balancing: identify imbalances and recommend cross-region workload redistribution. |
| FR-8 | Provide capacity dashboards with drill-down from global → region → AZ → cluster → host. |
| FR-9 | Generate monthly capacity reports for engineering leadership and finance. |
| FR-10 | Integrate with procurement systems for automated purchase order creation when thresholds are breached. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Metric collection freshness | < 60 s lag |
| NFR-2 | Forecast generation time | < 5 min for full fleet |
| NFR-3 | Dashboard query latency (p99) | < 2 s |
| NFR-4 | Forecast accuracy (MAPE) | < 15% for 30-day forecast |
| NFR-5 | System availability | 99.9% (planning is not real-time critical) |
| NFR-6 | Historical data retention | 2 years at full resolution, 5 years aggregated |
| NFR-7 | Simulation response time | < 30 s for single scenario |

### Constraints & Assumptions

- Fleet: 50,000 hosts across 5 regions, 10 availability zones.
- 8 SKU types (CPU-general, GPU A100, GPU H100, high-memory, storage, ARM, HPC bare-metal, FPGA).
- Metrics collected from Prometheus (short-term) and shipped to long-term storage (Thanos or VictoriaMetrics).
- Bare-metal procurement lead time: 12-24 weeks (3-6 months). Cloud VM provisioning: minutes.
- Budget cycle: quarterly planning, monthly review, weekly operational adjustments.
- The system advises humans; it does not autonomously purchase hardware (except for cloud VM autoscaling, which is separate).

### Out of Scope

- Real-time autoscaling (handled by cluster autoscaler).
- Hardware vendor negotiation and supply chain management.
- Data center physical capacity (power, cooling, rack space) — handled by DCOps.
- Network capacity planning (separate system).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Total hosts | 50,000 | Given |
| Metrics per host | 50 | CPU, RAM, GPU, disk, network x 10 sub-metrics each |
| Total metric streams | 2,500,000 | 50,000 hosts x 50 metrics |
| Metric collection interval | 15 s | Standard Prometheus scrape interval |
| Metric data points per second | 166,667 | 2,500,000 / 15 |
| Forecast models to train | 800 | 10 AZs x 8 SKUs x 10 resource types |
| Dashboard users (concurrent) | 50 | Capacity planning team + engineering leads |
| Dashboard queries per second | 10 | 50 users x 1 query / 5 s |
| Capacity reports per month | 20 | Per region + per team + global |

### Latency Requirements

| Operation | p50 | p99 | p999 |
|-----------|-----|-----|------|
| Dashboard query (pre-aggregated) | 200 ms | 1 s | 3 s |
| Dashboard query (ad-hoc, raw data) | 2 s | 10 s | 30 s |
| Forecast generation (single model) | 10 s | 60 s | 120 s |
| Full forecast pipeline (all models) | 2 min | 5 min | 10 min |
| Scenario simulation | 5 s | 20 s | 60 s |
| Report generation | 30 s | 2 min | 5 min |

### Storage Estimates

| Data | Size per data point | Points per day | Daily | 2-year Total |
|------|--------------------:|---------------:|------:|-------------:|
| Raw metrics (15s interval) | 16 bytes | 14.4 B (2.5M x 5,760) | 230 GB | 168 TB |
| Downsampled 5-min | 16 bytes | 720 M | 11.5 GB | 8.4 TB |
| Downsampled 1-hour | 16 bytes | 60 M | 960 MB | 700 GB |
| Downsampled 1-day | 16 bytes | 2.5 M | 40 MB | 29 GB |
| Forecast results | 1 KB | 800 models x 365 days | 292 MB/run | N/A (overwritten) |
| Capacity reports | 5 MB | 20/month | 100 MB/mo | 2.4 GB |
| **Total (with downsampling)** | | | | **~10 TB active** |

Assumption: raw 15-second data retained for 14 days, then downsampled to 5-minute resolution (retained 90 days), then 1-hour (retained 2 years), then 1-day (retained 5 years).

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Metrics ingest (from Prometheus) | 166,667/sec x 16 bytes | 2.7 MB/s |
| Metrics ingest (with labels/metadata) | x10 overhead | 27 MB/s |
| Dashboard queries | 10/sec x 1 MB avg response | 10 MB/s |
| Forecast model reads | 800 models x 1 MB per training set / 300 sec | 2.7 MB/s |
| **Total** | | **~42 MB/s** |

---

## 3. High Level Architecture

```
     ┌──────────────────────────────────────────────────────────────┐
     │                    Data Collection Layer                      │
     │                                                               │
     │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
     │  │ Prometheus   │  │ Cloud APIs  │  │ IPMI / BMC          │  │
     │  │ (per cluster)│  │ (AWS, GCP   │  │ (bare-metal health  │  │
     │  │ 15s scrape   │  │  billing)   │  │  & power metrics)   │  │
     │  └──────┬───────┘  └──────┬──────┘  └──────────┬──────────┘  │
     │         │                 │                     │             │
     │  ┌──────▼─────────────────▼─────────────────────▼──────────┐ │
     │  │              Metrics Aggregation Pipeline                 │ │
     │  │  (Prometheus Remote Write → Thanos / VictoriaMetrics)    │ │
     │  └──────────────────────┬───────────────────────────────────┘ │
     └─────────────────────────┼─────────────────────────────────────┘
                               │
     ┌─────────────────────────▼─────────────────────────────────────┐
     │                   Storage Layer                                │
     │                                                                │
     │  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐  │
     │  │ Time-Series  │  │  MySQL 8.0   │  │  Object Storage    │  │
     │  │ Database     │  │  (capacity   │  │  (S3 / MinIO)      │  │
     │  │ (VictoriaM.  │  │   config,    │  │  (archived metrics │  │
     │  │  or Thanos)  │  │   forecasts, │  │   reports, models) │  │
     │  │ 14d raw,     │  │   inventory, │  │                    │  │
     │  │ 90d 5-min,   │  │   budgets)   │  │                    │  │
     │  │ 2y hourly    │  │              │  │                    │  │
     │  └──────┬───────┘  └──────┬───────┘  └────────┬───────────┘  │
     └─────────┼─────────────────┼───────────────────┼───────────────┘
               │                 │                   │
     ┌─────────▼─────────────────▼───────────────────▼───────────────┐
     │                   Processing Layer                             │
     │                                                                │
     │  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐  │
     │  │ Forecasting  │  │ Scenario     │  │ Report Generator   │  │
     │  │ Engine       │  │ Simulator    │  │ (monthly / weekly  │  │
     │  │ (Prophet,    │  │ (what-if     │  │  capacity reports)  │  │
     │  │  ARIMA, LR,  │  │  analysis)   │  │                    │  │
     │  │  Ensemble)   │  │              │  │                    │  │
     │  └──────────────┘  └──────────────┘  └────────────────────┘  │
     │                                                                │
     │  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐  │
     │  │ Alert Engine │  │ Cost Modeler │  │ Procurement        │  │
     │  │ (threshold   │  │ (on-demand   │  │ Recommender        │  │
     │  │  monitoring, │  │  vs reserved │  │ (order triggers,   │  │
     │  │  trend-based │  │  vs spot vs  │  │  lead time aware)  │  │
     │  │  alerts)     │  │  bare-metal) │  │                    │  │
     │  └──────────────┘  └──────────────┘  └────────────────────┘  │
     └───────────────────────────┬────────────────────────────────────┘
                                 │
     ┌───────────────────────────▼────────────────────────────────────┐
     │                   Presentation Layer                            │
     │                                                                 │
     │  ┌──────────────┐  ┌──────────────┐  ┌─────────────────────┐  │
     │  │  Dashboard   │  │  API Server  │  │  Notification       │  │
     │  │  (Grafana +  │  │  (REST)      │  │  Service            │  │
     │  │   custom)    │  │              │  │  (Slack, email,     │  │
     │  │              │  │              │  │   PagerDuty)        │  │
     │  └──────────────┘  └──────────────┘  └─────────────────────┘  │
     └────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Prometheus (per cluster)** | Scrapes host-level metrics every 15 seconds. Node exporter for CPU/RAM/disk/network. DCGM exporter for GPU metrics. Custom exporters for IPMI/BMC. |
| **Metrics Aggregation Pipeline** | Prometheus remote-write to Thanos/VictoriaMetrics for cross-cluster aggregation and long-term storage. Handles downsampling (15s → 5min → 1hr → 1day). |
| **Time-Series Database** | VictoriaMetrics (or Thanos) for multi-resolution metric storage. Supports PromQL for flexible querying. Handles 166K data points/sec ingest. |
| **MySQL 8.0** | Stores capacity configuration: fleet inventory (host SKUs, regions, AZs), forecast results, budget allocations, procurement history, alert definitions, simulation scenarios. |
| **Object Storage (S3/MinIO)** | Archived raw metrics (beyond 14 days), generated reports (PDF/CSV), trained model artifacts. |
| **Forecasting Engine** | Trains and runs demand forecasting models. Supports multiple algorithms. Produces 30/60/90-day forecasts for each resource type per AZ per SKU. |
| **Scenario Simulator** | Interactive what-if analysis: simulate traffic growth, hardware failure, region loss, new SKU introduction. Uses forecasting models + constraint solver. |
| **Report Generator** | Generates monthly capacity reports: current utilization, forecast, procurement recommendations, cost projections. Outputs PDF and structured data. |
| **Alert Engine** | Monitors capacity metrics against thresholds. Trend-based alerts (e.g., "at current growth rate, GPU capacity exhausted in 45 days"). |
| **Cost Modeler** | Compares capacity procurement options: bare-metal CapEx, cloud reserved instances, on-demand, spot. Optimizes for total cost of ownership (TCO). |
| **Procurement Recommender** | Generates specific purchase recommendations: "Order 200 H100 nodes for us-east-1 delivery by June 2026." Considers lead times, budget constraints, and demand forecasts. |
| **Dashboard (Grafana + custom)** | Interactive dashboards with drill-down from global to host level. Grafana for metrics visualization. Custom web app for simulation, forecasting, and procurement workflows. |
| **Notification Service** | Sends capacity alerts via Slack, email, and PagerDuty. Weekly capacity digests to engineering leads. |

### Data Flows

**Primary — Continuous Monitoring:**
1. Node exporters on 50,000 hosts emit metrics every 15 seconds.
2. Prometheus (per cluster) scrapes and stores locally (2-hour retention).
3. Remote-write sends data to VictoriaMetrics.
4. VictoriaMetrics downsample pipeline: raw → 5-min → 1-hour → 1-day.
5. Alert Engine evaluates rules against live and aggregated metrics.
6. Dashboards query VictoriaMetrics for visualization.

**Secondary — Forecasting Pipeline (daily):**
1. Forecasting Engine reads historical metrics from VictoriaMetrics (90 days of hourly data).
2. For each (AZ, SKU, resource_type) combination, trains Prophet/ARIMA/ensemble model.
3. Generates 30/60/90-day forecasts.
4. Stores forecast results in MySQL.
5. Compares forecasts against capacity: if demand forecast exceeds capacity within lead time, triggers procurement alert.
6. Cost Modeler evaluates procurement options.
7. Procurement Recommender generates purchase orders.

**Tertiary — Monthly Reporting:**
1. Report Generator reads utilization data, forecasts, and cost data.
2. Generates structured capacity report.
3. Report stored in S3, link sent to stakeholders via email.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Fleet inventory: all hosts and their configuration
CREATE TABLE fleet_inventory (
    host_id          CHAR(36) PRIMARY KEY,
    hostname         VARCHAR(255) NOT NULL,
    region           VARCHAR(32) NOT NULL,
    availability_zone VARCHAR(32) NOT NULL,
    cluster_id       CHAR(36) NOT NULL,
    rack_id          VARCHAR(64),
    sku_type         VARCHAR(32) NOT NULL,
    -- Physical capacity
    total_cpu_cores  INT NOT NULL,
    total_ram_gb     INT NOT NULL,
    total_gpu_count  INT NOT NULL DEFAULT 0,
    gpu_model        VARCHAR(64),                  -- 'A100-80GB', 'H100-80GB'
    total_disk_tb    DECIMAL(10,2) NOT NULL,
    total_net_gbps   INT NOT NULL,
    -- Lifecycle
    status           ENUM('active','maintenance','decommissioning','decommissioned','ordered','provisioning') NOT NULL,
    commissioned_date DATE,
    expected_eol_date DATE,                        -- end of life / warranty expiry
    purchase_order_id VARCHAR(64),
    unit_cost_usd    DECIMAL(10,2),                -- acquisition cost
    monthly_opex_usd DECIMAL(10,2),                -- monthly operating cost (power, cooling, license)
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_fleet_region_sku (region, sku_type, status),
    INDEX idx_fleet_az (availability_zone, sku_type, status),
    INDEX idx_fleet_status (status),
    INDEX idx_fleet_eol (expected_eol_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- SKU definitions (hardware profiles)
CREATE TABLE sku_definitions (
    sku_type         VARCHAR(32) PRIMARY KEY,
    display_name     VARCHAR(128) NOT NULL,
    cpu_cores        INT NOT NULL,
    ram_gb           INT NOT NULL,
    gpu_count        INT NOT NULL DEFAULT 0,
    gpu_model        VARCHAR(64),
    disk_tb          DECIMAL(10,2) NOT NULL,
    net_gbps         INT NOT NULL,
    -- Cost
    unit_capex_usd   DECIMAL(12,2) NOT NULL,       -- purchase price
    monthly_opex_usd DECIMAL(10,2) NOT NULL,       -- monthly operating cost
    depreciation_months INT NOT NULL DEFAULT 48,    -- 4-year depreciation
    -- Procurement
    lead_time_weeks  INT NOT NULL DEFAULT 16,       -- typical procurement lead time
    min_order_quantity INT NOT NULL DEFAULT 10,
    -- Lifecycle
    is_orderable     BOOLEAN NOT NULL DEFAULT TRUE,
    end_of_sale_date DATE,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Capacity snapshots (aggregated per AZ per SKU, computed hourly)
CREATE TABLE capacity_snapshots (
    snapshot_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
    snapshot_time    TIMESTAMP(3) NOT NULL,
    region           VARCHAR(32) NOT NULL,
    availability_zone VARCHAR(32) NOT NULL,
    sku_type         VARCHAR(32) NOT NULL,
    -- Host counts
    total_hosts      INT NOT NULL,
    active_hosts     INT NOT NULL,
    maintenance_hosts INT NOT NULL DEFAULT 0,
    -- Capacity totals
    total_cpu        INT NOT NULL,
    total_ram_gb     INT NOT NULL,
    total_gpu        INT NOT NULL DEFAULT 0,
    total_disk_tb    DECIMAL(12,2) NOT NULL,
    -- Utilization (USE method)
    cpu_utilization_avg DECIMAL(5,2) NOT NULL,      -- percentage
    cpu_utilization_p95 DECIMAL(5,2) NOT NULL,
    cpu_saturation_avg  DECIMAL(5,2) NOT NULL,      -- load average / core count
    ram_utilization_avg DECIMAL(5,2) NOT NULL,
    ram_utilization_p95 DECIMAL(5,2) NOT NULL,
    gpu_utilization_avg DECIMAL(5,2) DEFAULT 0,
    gpu_utilization_p95 DECIMAL(5,2) DEFAULT 0,
    disk_utilization_avg DECIMAL(5,2) NOT NULL,
    net_utilization_avg  DECIMAL(5,2) NOT NULL,
    -- Availability
    host_availability_pct DECIMAL(5,2) NOT NULL,     -- % of hosts that are healthy
    -- Committed resources (sum of all workload requests)
    committed_cpu    INT NOT NULL,
    committed_ram_gb INT NOT NULL,
    committed_gpu    INT NOT NULL DEFAULT 0,
    -- Buffer (free capacity)
    buffer_cpu_pct   DECIMAL(5,2) NOT NULL,         -- (total - committed) / total * 100
    buffer_ram_pct   DECIMAL(5,2) NOT NULL,
    buffer_gpu_pct   DECIMAL(5,2) DEFAULT 0,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UNIQUE INDEX idx_snap_time_az_sku (snapshot_time, availability_zone, sku_type),
    INDEX idx_snap_time (snapshot_time),
    INDEX idx_snap_region (region, snapshot_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Forecast results
CREATE TABLE capacity_forecasts (
    forecast_id      CHAR(36) PRIMARY KEY,
    forecast_run_id  CHAR(36) NOT NULL,            -- groups all forecasts from one run
    region           VARCHAR(32) NOT NULL,
    availability_zone VARCHAR(32) NOT NULL,
    sku_type         VARCHAR(32) NOT NULL,
    resource_type    ENUM('cpu','ram','gpu','disk','network') NOT NULL,
    -- Model info
    model_type       ENUM('prophet','arima','linear','ensemble') NOT NULL,
    model_version    VARCHAR(32) NOT NULL,
    training_data_days INT NOT NULL,                -- how many days of history used
    -- Forecast values
    forecast_date    DATE NOT NULL,                 -- the date being forecasted
    forecasted_utilization DECIMAL(5,2) NOT NULL,   -- predicted utilization %
    forecasted_demand_absolute BIGINT NOT NULL,     -- predicted absolute resource demand
    confidence_lower DECIMAL(5,2) NOT NULL,         -- lower bound of 95% CI
    confidence_upper DECIMAL(5,2) NOT NULL,         -- upper bound of 95% CI
    -- Capacity comparison
    current_capacity BIGINT NOT NULL,
    capacity_headroom_pct DECIMAL(5,2) NOT NULL,    -- (capacity - demand) / capacity * 100
    days_until_exhaustion INT,                      -- NULL if no exhaustion predicted
    -- Accuracy (filled in retrospectively)
    actual_utilization DECIMAL(5,2),                -- filled after the date passes
    forecast_error_pct DECIMAL(5,2),                -- |actual - forecast| / actual * 100
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    INDEX idx_forecast_run (forecast_run_id),
    INDEX idx_forecast_az_sku (availability_zone, sku_type, resource_type, forecast_date),
    INDEX idx_forecast_date (forecast_date),
    INDEX idx_forecast_exhaustion (days_until_exhaustion)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Procurement recommendations
CREATE TABLE procurement_recommendations (
    recommendation_id CHAR(36) PRIMARY KEY,
    forecast_run_id  CHAR(36) NOT NULL,
    -- What to order
    sku_type         VARCHAR(32) NOT NULL,
    quantity         INT NOT NULL,
    region           VARCHAR(32) NOT NULL,
    target_az        VARCHAR(32),                   -- specific AZ or NULL for region-level
    -- Why
    trigger_reason   ENUM('buffer_breach','forecast_exhaustion','eol_replacement',
                          'performance_upgrade','cost_optimization') NOT NULL,
    urgency          ENUM('critical','high','medium','low') NOT NULL,
    -- When
    needed_by_date   DATE NOT NULL,                 -- when the capacity is needed
    order_by_date    DATE NOT NULL,                 -- accounting for lead time
    lead_time_weeks  INT NOT NULL,
    -- Cost
    estimated_capex_usd DECIMAL(14,2) NOT NULL,
    estimated_monthly_opex_usd DECIMAL(12,2) NOT NULL,
    -- Supporting data
    current_utilization DECIMAL(5,2) NOT NULL,
    projected_utilization_30d DECIMAL(5,2) NOT NULL,
    projected_utilization_90d DECIMAL(5,2) NOT NULL,
    buffer_target_pct DECIMAL(5,2) NOT NULL,
    -- Approval workflow
    status           ENUM('proposed','approved','ordered','delivered','rejected','deferred') NOT NULL DEFAULT 'proposed',
    approved_by      CHAR(36),
    approved_at      TIMESTAMP(3),
    notes            TEXT,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_proc_status (status),
    INDEX idx_proc_sku_region (sku_type, region),
    INDEX idx_proc_urgency (urgency, order_by_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Budget allocations (per-region, per-quarter)
CREATE TABLE capacity_budgets (
    budget_id        CHAR(36) PRIMARY KEY,
    fiscal_year      INT NOT NULL,
    fiscal_quarter   INT NOT NULL,                  -- 1-4
    region           VARCHAR(32) NOT NULL,
    -- Budget amounts
    capex_budget_usd DECIMAL(14,2) NOT NULL,
    opex_budget_usd  DECIMAL(14,2) NOT NULL,
    capex_spent_usd  DECIMAL(14,2) NOT NULL DEFAULT 0,
    opex_spent_usd   DECIMAL(14,2) NOT NULL DEFAULT 0,
    -- Status
    status           ENUM('planned','active','closed') NOT NULL DEFAULT 'planned',
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE INDEX idx_budget_quarter_region (fiscal_year, fiscal_quarter, region)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Capacity alert definitions
CREATE TABLE capacity_alerts (
    alert_id         CHAR(36) PRIMARY KEY,
    alert_name       VARCHAR(255) NOT NULL,
    -- Scope
    region           VARCHAR(32),                   -- NULL = global
    availability_zone VARCHAR(32),                  -- NULL = region-level
    sku_type         VARCHAR(32),                   -- NULL = all SKUs
    resource_type    ENUM('cpu','ram','gpu','disk','network') NOT NULL,
    -- Thresholds
    alert_type       ENUM('utilization_threshold','buffer_breach','forecast_exhaustion',
                          'trend_acceleration','eol_approaching') NOT NULL,
    warning_threshold DECIMAL(5,2),                 -- e.g., 70%
    critical_threshold DECIMAL(5,2),                -- e.g., 85%
    -- Alert configuration
    evaluation_window_hours INT NOT NULL DEFAULT 1,
    cooldown_hours   INT NOT NULL DEFAULT 24,
    notification_channels JSON NOT NULL,            -- ["slack:#capacity", "email:team@co.com"]
    severity         ENUM('info','warning','critical') NOT NULL DEFAULT 'warning',
    enabled          BOOLEAN NOT NULL DEFAULT TRUE,
    last_fired_at    TIMESTAMP(3),
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_alerts_enabled (enabled)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Criteria | VictoriaMetrics | MySQL 8.0 | S3/MinIO | Elasticsearch |
|----------|-----------------|-----------|----------|---------------|
| **Use for** | Time-series metrics | Structured config, forecasts, procurement | Archived data, reports | Log-based capacity analysis |
| **Ingest rate** | 1M+ points/sec | Moderate writes | Bulk uploads | 50K events/sec |
| **Query language** | PromQL/MetricsQL | SQL | N/A (Athena for queries) | KQL/Lucene |
| **Retention** | Multi-resolution, years | Years | Unlimited | 30-90 days |
| **Cost at scale** | Low (compressed time-series) | Low (< 100 GB) | Very low | Medium |

**Selected: VictoriaMetrics + MySQL 8.0 + S3.**

**Justification:**
- **VictoriaMetrics** for time-series metric storage: handles 166K data points/sec ingestion, supports PromQL, built-in downsampling, and is significantly more cost-efficient than Prometheus long-term storage. It replaces Thanos with simpler operations.
- **MySQL 8.0** for structured data: fleet inventory, forecast results, procurement recommendations, budgets, alert definitions. SQL is ideal for the complex analytical queries needed for capacity planning. The role requires MySQL expertise.
- **S3/MinIO** for cold storage: archived metrics beyond 2 years, generated reports, model artifacts. Very low cost for large volumes.

### Indexing Strategy

| Index | Table | Purpose |
|-------|-------|---------|
| `idx_snap_time_az_sku` | capacity_snapshots | Primary lookup: capacity at a specific time for a specific AZ + SKU |
| `idx_fleet_region_sku` | fleet_inventory | Count hosts by region + SKU (capacity calculation) |
| `idx_forecast_az_sku` | capacity_forecasts | Forecast lookup by AZ + SKU + resource type + date |
| `idx_forecast_exhaustion` | capacity_forecasts | Find all scopes predicted to exhaust capacity |
| `idx_proc_urgency` | procurement_recommendations | Priority-order open recommendations |
| `idx_fleet_eol` | fleet_inventory | Find hosts approaching end-of-life |

---

## 5. API Design

### REST Endpoints

| Method | Path | Description | Auth | Rate Limit |
|--------|------|-------------|------|------------|
| GET | `/v1/capacity/current` | Current capacity summary | JWT | 100/min |
| GET | `/v1/capacity/current/{region}` | Regional capacity | JWT | 100/min |
| GET | `/v1/capacity/current/{region}/{az}` | AZ capacity | JWT | 100/min |
| GET | `/v1/capacity/history` | Historical capacity over time | JWT | 50/min |
| GET | `/v1/forecasts/latest` | Latest forecast results | JWT | 100/min |
| GET | `/v1/forecasts/{run_id}` | Specific forecast run | JWT | 100/min |
| POST | `/v1/forecasts/trigger` | Trigger on-demand forecast | JWT (admin) | 5/hour |
| POST | `/v1/simulations` | Run a what-if scenario | JWT | 20/min |
| GET | `/v1/simulations/{id}` | Get simulation results | JWT | 100/min |
| GET | `/v1/procurement/recommendations` | List open recommendations | JWT (admin) | 100/min |
| POST | `/v1/procurement/recommendations/{id}/approve` | Approve procurement | JWT (admin) | 20/min |
| GET | `/v1/fleet/inventory` | Fleet inventory (hosts) | JWT (admin) | 50/min |
| GET | `/v1/fleet/skus` | SKU definitions | JWT | 100/min |
| GET | `/v1/budget/{year}/{quarter}` | Budget status | JWT (finance) | 50/min |
| GET | `/v1/reports/latest` | Latest capacity report | JWT | 100/min |
| GET | `/v1/alerts` | Active capacity alerts | JWT | 100/min |
| POST | `/v1/alerts` | Create capacity alert | JWT (admin) | 50/min |

#### Full Schema Examples

**POST /v1/simulations:**
```json
// Request
{
    "name": "What if GPU demand grows 50% in 90 days",
    "scenario": {
        "type": "demand_growth",
        "parameters": {
            "resource_type": "gpu",
            "growth_factor": 1.5,
            "growth_period_days": 90,
            "affected_regions": ["us-east-1", "us-west-2"]
        }
    }
}

// Response
{
    "simulation_id": "sim-uuid-123",
    "status": "completed",
    "results": {
        "us-east-1": {
            "current_gpu_total": 1024,
            "current_gpu_used": 680,
            "current_utilization": 66.4,
            "projected_demand_90d": 1020,
            "projected_utilization_90d": 99.6,
            "days_until_exhaustion": 72,
            "recommended_action": "Order 256 H100 GPUs (32 nodes) by 2026-04-20 (16-week lead time)",
            "estimated_cost": {
                "capex_usd": 8000000,
                "monthly_opex_usd": 25000
            }
        },
        "us-west-2": {
            "current_gpu_total": 768,
            "current_gpu_used": 450,
            "current_utilization": 58.6,
            "projected_demand_90d": 675,
            "projected_utilization_90d": 87.9,
            "days_until_exhaustion": 120,
            "recommended_action": "Order 128 H100 GPUs (16 nodes) by 2026-05-15",
            "estimated_cost": {
                "capex_usd": 4000000,
                "monthly_opex_usd": 12500
            }
        }
    },
    "total_capex_usd": 12000000,
    "budget_impact": {
        "q2_2026_capex_remaining": 20000000,
        "after_this_order": 8000000
    }
}
```

**GET /v1/capacity/current?region=us-east-1&group_by=sku:**
```json
{
    "region": "us-east-1",
    "timestamp": "2026-04-09T12:00:00Z",
    "summary": {
        "total_hosts": 15000,
        "active_hosts": 14850,
        "overall_cpu_utilization": 72.3,
        "overall_ram_utilization": 68.1,
        "overall_gpu_utilization": 81.5
    },
    "by_sku": [
        {
            "sku_type": "gpu_h100",
            "total_hosts": 500,
            "total_gpus": 4000,
            "gpu_utilization_avg": 85.2,
            "gpu_utilization_p95": 97.1,
            "buffer_gpu_pct": 14.8,
            "days_until_exhaustion": 45,
            "status": "WARNING"
        },
        {
            "sku_type": "cpu_general",
            "total_hosts": 8000,
            "total_cpu_cores": 512000,
            "cpu_utilization_avg": 68.4,
            "buffer_cpu_pct": 31.6,
            "days_until_exhaustion": null,
            "status": "HEALTHY"
        }
    ]
}
```

### CLI Design

```bash
# Current capacity
capacity show --region=us-east-1 --group-by=sku --output=table
capacity show --region=us-east-1 --az=us-east-1a --resource=gpu --output=chart

# Historical
capacity history --region=us-east-1 --resource=gpu --from=90d --granularity=1d --output=chart

# Forecasts
capacity forecast show --region=us-east-1 --sku=gpu_h100 --horizon=90d --output=table
capacity forecast trigger --reason="Post-conference demand spike expected"
capacity forecast accuracy --region=us-east-1 --last=30d  # backtest accuracy

# Simulations
capacity simulate \
    --name="GPU demand surge" \
    --type=demand_growth \
    --resource=gpu \
    --growth=50% \
    --period=90d \
    --region=us-east-1,us-west-2 \
    --output=json

capacity simulate \
    --name="Lose us-east-1a" \
    --type=az_failure \
    --az=us-east-1a \
    --output=table

capacity simulate \
    --name="Add 100 H100 nodes" \
    --type=capacity_addition \
    --sku=gpu_h100 \
    --quantity=100 \
    --az=us-east-1a \
    --output=table

# Procurement
capacity procurement list --status=proposed --urgency=critical --output=table
capacity procurement approve proc-uuid-123 --comment="Approved by VP Infra"
capacity procurement history --region=us-east-1 --from=1y --output=table

# Fleet
capacity fleet list --region=us-east-1 --sku=gpu_h100 --status=active --output=table
capacity fleet eol --within=6m  # hosts reaching end-of-life in 6 months
capacity fleet cost --region=us-east-1 --breakdown=sku --output=table

# Budget
capacity budget show --year=2026 --quarter=2 --output=table

# Reports
capacity report generate --type=monthly --region=us-east-1
capacity report list --type=monthly --from=6m

# Alerts
capacity alert list --severity=critical
capacity alert create \
    --name="GPU utilization high" \
    --resource=gpu --sku=gpu_h100 \
    --type=utilization_threshold \
    --warning=70 --critical=85 \
    --notify=slack:#gpu-capacity,email:ml-team@co.com
```

---

## 6. Core Component Deep Dives

### 6.1 Demand Forecasting Engine

**Why it's hard:**
Infrastructure demand is driven by many factors: organic growth, seasonal patterns (end-of-quarter, Black Friday), ML training job schedules, new product launches, and acquisition-driven jumps. Simple linear extrapolation fails when any of these factors change. The forecast must be accurate enough to trigger procurement 3-6 months in advance (the bare-metal lead time), but every week of over-provisioning wastes capital. A 10% forecast error on a $50M GPU fleet is $5M.

**Approaches Compared:**

| Model | Seasonality | Trend | Anomaly Robust | Auto-tuning | Accuracy (typical MAPE) | Complexity |
|-------|-----------|-------|----------------|-------------|------------------------|------------|
| Linear Regression | No | Linear only | No | No | 20-30% | Very Low |
| ARIMA | Yes (manual) | Yes | No | Manual | 15-20% | Medium |
| Prophet (Facebook) | Yes (auto) | Piecewise linear | Yes (outlier detection) | Mostly auto | 10-15% | Low |
| LSTM Neural Network | Yes (learned) | Non-linear | No | No | 8-15% | High |
| Ensemble (all above) | Yes | Yes | Yes | Yes | 8-12% | High |

**Selected: Ensemble of Prophet + ARIMA + Linear Regression, with Prophet as primary.**

**Justification:** Prophet handles seasonality (weekly, monthly, yearly) and trend changes automatically, which is critical for infrastructure demand that has weekly patterns (low on weekends) and periodic jumps (product launches). The ensemble averages Prophet, ARIMA, and linear regression to reduce individual model bias. Ensemble MAPE: 8-12% for 30-day forecasts, 12-18% for 90-day forecasts. This is within the acceptable range for procurement decisions (we maintain 20% buffer to absorb forecast errors).

**Implementation (pseudocode):**

```python
import pandas as pd
from prophet import Prophet
from statsmodels.tsa.arima.model import ARIMA
from sklearn.linear_model import LinearRegression
import numpy as np

class CapacityForecaster:
    def __init__(self, tsdb_client, mysql_client):
        self.tsdb = tsdb_client
        self.db = mysql_client
        self.models = {}
    
    def run_forecast_pipeline(self):
        """Run full forecast pipeline for all (AZ, SKU, resource) combinations."""
        scopes = self.db.query("""
            SELECT DISTINCT availability_zone, sku_type 
            FROM fleet_inventory 
            WHERE status = 'active'
        """)
        
        resource_types = ['cpu', 'ram', 'gpu', 'disk', 'network']
        forecast_run_id = str(uuid4())
        
        for scope in scopes:
            for resource in resource_types:
                try:
                    forecast = self._forecast_single(
                        scope.availability_zone, scope.sku_type, 
                        resource, forecast_run_id)
                    self._save_forecast(forecast)
                    self._check_procurement_trigger(forecast)
                except Exception as e:
                    log.error(f"Forecast failed for {scope}/{resource}: {e}")
        
        # Backfill accuracy for past forecasts
        self._calculate_retrospective_accuracy(forecast_run_id)
        
        return forecast_run_id
    
    def _forecast_single(self, az: str, sku: str, resource: str, 
                          run_id: str) -> List[ForecastResult]:
        """Forecast demand for one (AZ, SKU, resource) combination."""
        
        # Fetch historical data (90 days, hourly resolution)
        query = f"""
            avg_over_time(
                node_{resource}_utilization{{availability_zone="{az}", 
                                              sku_type="{sku}"}}[1h]
            )
        """
        historical = self.tsdb.query_range(query, start='-90d', end='now', step='1h')
        
        # Convert to pandas DataFrame
        df = pd.DataFrame({
            'ds': [point.timestamp for point in historical],
            'y': [point.value for point in historical]
        })
        
        if len(df) < 168:  # need at least 1 week of data
            raise InsufficientDataError(f"Only {len(df)} data points for {az}/{sku}/{resource}")
        
        # Get current capacity
        capacity = self._get_current_capacity(az, sku, resource)
        
        # Run ensemble models
        prophet_forecast = self._run_prophet(df, horizon_days=90)
        arima_forecast = self._run_arima(df, horizon_days=90)
        linear_forecast = self._run_linear(df, horizon_days=90)
        
        # Ensemble: weighted average (Prophet gets more weight)
        ensemble = self._ensemble_forecasts(
            prophet_forecast, arima_forecast, linear_forecast,
            weights=[0.5, 0.3, 0.2])
        
        # Convert to ForecastResult objects
        results = []
        for i, (date, demand, lower, upper) in enumerate(ensemble):
            headroom = (capacity - demand) / capacity * 100
            days_to_exhaustion = None
            if demand > capacity * 0.95:
                # Find when we cross 95% threshold
                days_to_exhaustion = self._find_exhaustion_day(ensemble, capacity)
            
            results.append(ForecastResult(
                forecast_run_id=run_id,
                availability_zone=az,
                sku_type=sku,
                resource_type=resource,
                model_type='ensemble',
                forecast_date=date,
                forecasted_utilization=demand / capacity * 100,
                forecasted_demand_absolute=int(demand),
                confidence_lower=lower / capacity * 100,
                confidence_upper=upper / capacity * 100,
                current_capacity=capacity,
                capacity_headroom_pct=headroom,
                days_until_exhaustion=days_to_exhaustion
            ))
        
        return results
    
    def _run_prophet(self, df: pd.DataFrame, horizon_days: int) -> pd.DataFrame:
        """Train Prophet model and generate forecast."""
        model = Prophet(
            yearly_seasonality=True,
            weekly_seasonality=True,
            daily_seasonality=True,
            changepoint_prior_scale=0.1,    # conservative trend changes
            interval_width=0.95              # 95% confidence interval
        )
        
        # Add holidays/events as regressors
        model.add_country_holidays(country_name='US')
        
        model.fit(df)
        
        future = model.make_future_dataframe(periods=horizon_days * 24, freq='H')
        forecast = model.predict(future)
        
        # Extract future-only predictions, aggregate to daily
        future_forecast = forecast[forecast['ds'] > df['ds'].max()]
        daily = future_forecast.resample('D', on='ds').agg({
            'yhat': 'mean',
            'yhat_lower': 'min',
            'yhat_upper': 'max'
        }).reset_index()
        
        return daily
    
    def _run_arima(self, df: pd.DataFrame, horizon_days: int) -> pd.DataFrame:
        """Train ARIMA model."""
        # Resample to daily for ARIMA (more stable)
        daily = df.resample('D', on='ds').mean().reset_index()
        
        model = ARIMA(daily['y'], order=(5, 1, 2))  # p=5, d=1, q=2
        fitted = model.fit()
        
        forecast = fitted.forecast(steps=horizon_days)
        conf_int = fitted.get_forecast(steps=horizon_days).conf_int(alpha=0.05)
        
        dates = pd.date_range(start=daily['ds'].max() + pd.Timedelta(days=1), 
                               periods=horizon_days, freq='D')
        
        return pd.DataFrame({
            'ds': dates,
            'yhat': forecast.values,
            'yhat_lower': conf_int.iloc[:, 0].values,
            'yhat_upper': conf_int.iloc[:, 1].values
        })
    
    def _run_linear(self, df: pd.DataFrame, horizon_days: int) -> pd.DataFrame:
        """Simple linear regression on daily aggregated data."""
        daily = df.resample('D', on='ds').mean().reset_index()
        
        X = np.arange(len(daily)).reshape(-1, 1)
        y = daily['y'].values
        
        model = LinearRegression()
        model.fit(X, y)
        
        future_X = np.arange(len(daily), len(daily) + horizon_days).reshape(-1, 1)
        predictions = model.predict(future_X)
        
        # Estimate confidence interval from residuals
        residuals = y - model.predict(X)
        std = np.std(residuals)
        
        dates = pd.date_range(start=daily['ds'].max() + pd.Timedelta(days=1),
                               periods=horizon_days, freq='D')
        
        return pd.DataFrame({
            'ds': dates,
            'yhat': predictions,
            'yhat_lower': predictions - 1.96 * std,
            'yhat_upper': predictions + 1.96 * std
        })
    
    def _ensemble_forecasts(self, prophet, arima, linear, 
                             weights=[0.5, 0.3, 0.2]) -> List[Tuple]:
        """Weighted ensemble of forecasts."""
        results = []
        for i in range(len(prophet)):
            date = prophet.iloc[i]['ds']
            demand = (weights[0] * prophet.iloc[i]['yhat'] +
                     weights[1] * arima.iloc[i]['yhat'] +
                     weights[2] * linear.iloc[i]['yhat'])
            lower = (weights[0] * prophet.iloc[i]['yhat_lower'] +
                    weights[1] * arima.iloc[i]['yhat_lower'] +
                    weights[2] * linear.iloc[i]['yhat_lower'])
            upper = (weights[0] * prophet.iloc[i]['yhat_upper'] +
                    weights[1] * arima.iloc[i]['yhat_upper'] +
                    weights[2] * linear.iloc[i]['yhat_upper'])
            results.append((date, max(0, demand), max(0, lower), max(0, upper)))
        return results
    
    def _check_procurement_trigger(self, forecasts: List[ForecastResult]):
        """Check if forecast warrants procurement recommendation."""
        for f in forecasts:
            if f.days_until_exhaustion is not None:
                sku = self.db.query(
                    "SELECT lead_time_weeks FROM sku_definitions WHERE sku_type = %s",
                    f.sku_type)
                lead_time_days = sku.lead_time_weeks * 7
                
                if f.days_until_exhaustion <= lead_time_days + 30:  # 30-day safety margin
                    # CRITICAL: must order now
                    urgency = 'critical'
                elif f.days_until_exhaustion <= lead_time_days + 60:
                    urgency = 'high'
                elif f.days_until_exhaustion <= lead_time_days + 90:
                    urgency = 'medium'
                else:
                    continue
                
                self._create_procurement_recommendation(f, urgency)
```

**Failure Modes:**
1. **Model overfitting to recent anomaly:** A one-time traffic spike (DDoS, broken retry loop) skews the forecast upward. Mitigation: Prophet's robust outlier detection. Manual review of forecasts before procurement approval. Historical MAPE tracking — if a model's accuracy degrades, increase the weight of other models in the ensemble.
2. **Step-change in demand (acquisition, product launch):** Forecasting models trained on historical data can't predict step changes. Mitigation: manual "event" input — capacity planners can add known future events ("Product X launches April 15, expect +30% traffic"). Prophet supports custom regressors for this.
3. **Insufficient historical data for new SKUs:** A new GPU SKU has only 2 weeks of data. Mitigation: use historical data from the predecessor SKU (e.g., A100 data to bootstrap H100 forecasting). Flag forecasts with < 30 days of data as "low confidence."
4. **Correlated demand across regions:** A global product launch increases demand in all regions simultaneously. Per-region models miss this. Mitigation: global model that forecasts total demand, then distributes across regions based on historical proportions.

**Interviewer Q&As:**

**Q1: Why use Prophet as the primary model instead of LSTM?**
A: Prophet is specifically designed for business time-series with strong seasonal effects and trend changes. It requires minimal hyperparameter tuning (important when training 800 models). LSTM requires significantly more data and tuning. At 90 days of hourly data (2,160 points), Prophet performs well; LSTM would likely overfit. We can add LSTM to the ensemble in the future if we accumulate more data.

**Q2: How do you handle the uncertainty in long-range forecasts?**
A: The 95% confidence interval widens with forecast horizon. For a 30-day forecast, the CI might be +/- 10%. For 90 days, +/- 25%. We use the upper bound of the CI for procurement decisions (worst-case planning). The 20% buffer target provides additional safety margin beyond the forecast uncertainty.

**Q3: How do you validate forecast accuracy?**
A: Retrospective accuracy: every day, we compare yesterday's forecast against actual utilization and compute MAPE. We track rolling 7-day, 30-day, and 90-day MAPE for each model and each (AZ, SKU, resource) combination. If MAPE exceeds 20% for a 30-day forecast, an alert fires and the model is reviewed.

**Q4: How do you handle GPU demand forecasting differently from CPU?**
A: GPU demand is "lumpier" — driven by a few large ML training jobs rather than many small services. We supplement the time-series forecast with information from the reservation system (known future GPU reservations) and ML team roadmaps (planned training runs). This gives us a more accurate near-term GPU forecast than pure statistical models.

**Q5: What's the cost of a forecast error?**
A: Over-forecast by 10% on a $50M GPU fleet = $5M wasted CapEx sitting idle. Under-forecast by 10% = GPU shortage, blocked ML training, potential revenue impact. We err on the side of over-provisioning (use the upper confidence bound + 20% buffer) because the opportunity cost of GPU shortage is higher than the carrying cost of idle GPUs.

**Q6: How often do you retrain models?**
A: Daily. Each model retrains on the latest 90 days of data. Training 800 models takes ~5 minutes on a 16-core machine. The models are lightweight (Prophet + ARIMA + Linear Regression), not deep learning.

---

### 6.2 Scenario Simulator

**Why it's hard:**
What-if analysis requires modeling the complex interactions between demand, capacity, scheduling, and cost. A scenario like "lose region us-east-1" requires understanding: which workloads are affected, can they fail over to other regions, do those regions have capacity, how does the redistributed load affect performance, and what's the cost impact. This is a multi-variable optimization problem with many constraints.

**Approaches Compared:**

| Approach | Fidelity | Speed | Complexity |
|----------|---------|-------|------------|
| Spreadsheet-based (manual) | Low | Fast (human-driven) | Low |
| Constraint model (LP/ILP) | High | Seconds-minutes | High |
| Discrete event simulation | Very high | Minutes-hours | Very high |
| Statistical model (scale forecast) | Medium | Instant | Low |
| Hybrid (statistical + constraint) | High | Seconds | Medium-High |

**Selected: Hybrid statistical + constraint model.**

**Justification:** The statistical model scales the forecast by the scenario parameters (e.g., 50% demand growth). The constraint model then checks whether the scaled demand can be served by available capacity across regions, considering scheduling constraints, failover policies, and cost. This gives high-fidelity results in seconds.

**Scenario Types:**

| Scenario | Parameters | Model |
|----------|------------|-------|
| Demand growth | growth_factor, period, resource_type, regions | Scale forecast by factor, check capacity |
| AZ/Region failure | failed_az or failed_region | Redistribute workloads to surviving AZs/regions |
| Capacity addition | sku_type, quantity, target_az | Add capacity, re-run forecast comparison |
| SKU retirement | sku_type, retirement_date | Project workload migration to replacement SKU |
| Cost optimization | target_savings_pct | Find optimal mix of reserved/spot/on-demand |
| Traffic redistribution | source_region, dest_region, pct | Model cross-region workload shift |

**Implementation (pseudocode):**

```python
class ScenarioSimulator:
    def simulate(self, scenario: Scenario) -> SimulationResult:
        # Load current state
        current_capacity = self._load_current_capacity()
        current_forecast = self._load_latest_forecast()
        
        if scenario.type == 'demand_growth':
            return self._sim_demand_growth(scenario, current_capacity, current_forecast)
        elif scenario.type == 'az_failure':
            return self._sim_az_failure(scenario, current_capacity, current_forecast)
        elif scenario.type == 'capacity_addition':
            return self._sim_capacity_addition(scenario, current_capacity, current_forecast)
        elif scenario.type == 'cost_optimization':
            return self._sim_cost_optimization(scenario, current_capacity, current_forecast)
        else:
            raise UnsupportedScenarioError(scenario.type)
    
    def _sim_demand_growth(self, scenario, capacity, forecast) -> SimulationResult:
        """Simulate demand growing by a factor over a period."""
        results = {}
        
        for region in (scenario.affected_regions or capacity.regions()):
            regional_cap = capacity.get_region(region)
            regional_forecast = forecast.get_region(region)
            
            # Scale forecast by growth factor
            scaled_forecast = []
            for day in regional_forecast:
                growth_on_day = 1.0 + (scenario.growth_factor - 1.0) * (
                    min(day.day_index, scenario.period_days) / scenario.period_days)
                scaled_demand = day.demand * growth_on_day
                scaled_forecast.append((day.date, scaled_demand))
            
            # Find when demand exceeds capacity
            resource = scenario.resource_type
            total_cap = regional_cap.get_total(resource)
            
            exhaustion_day = None
            for date, demand in scaled_forecast:
                if demand > total_cap * 0.95:  # 95% threshold
                    exhaustion_day = (date - datetime.now().date()).days
                    break
            
            # Calculate procurement need
            peak_demand = max(d for _, d in scaled_forecast)
            buffer_target = total_cap * 0.20  # 20% buffer
            additional_needed = peak_demand + buffer_target - total_cap
            
            if additional_needed > 0:
                sku = self._best_sku_for_resource(resource, region)
                nodes_needed = math.ceil(additional_needed / sku.resource_per_node(resource))
                
                results[region] = {
                    'current_total': total_cap,
                    'current_used': regional_cap.get_used(resource),
                    'current_utilization': regional_cap.get_utilization(resource),
                    'projected_demand_peak': peak_demand,
                    'projected_utilization_peak': peak_demand / total_cap * 100,
                    'days_until_exhaustion': exhaustion_day,
                    'additional_needed': additional_needed,
                    'nodes_needed': nodes_needed,
                    'sku_type': sku.sku_type,
                    'estimated_cost': {
                        'capex_usd': nodes_needed * sku.unit_capex_usd,
                        'monthly_opex_usd': nodes_needed * sku.monthly_opex_usd
                    },
                    'order_deadline': self._calculate_order_deadline(
                        exhaustion_day, sku.lead_time_weeks)
                }
            else:
                results[region] = {
                    'current_total': total_cap,
                    'projected_demand_peak': peak_demand,
                    'projected_utilization_peak': peak_demand / total_cap * 100,
                    'status': 'SUFFICIENT',
                    'buffer_remaining_pct': (total_cap - peak_demand) / total_cap * 100
                }
        
        return SimulationResult(scenario=scenario, results=results)
    
    def _sim_az_failure(self, scenario, capacity, forecast) -> SimulationResult:
        """Simulate losing an entire AZ."""
        failed_az = scenario.failed_az
        region = failed_az.split('-')[0] + '-' + failed_az.split('-')[1]  # e.g., us-east-1a -> us-east-1
        
        # Current workloads in the failed AZ
        failed_workloads = capacity.get_az_workloads(failed_az)
        
        # Surviving AZs in the same region
        surviving_azs = [az for az in capacity.get_azs(region) if az != failed_az]
        
        # Can surviving AZs absorb the failed AZ's workloads?
        results = {'failed_az': failed_az, 'surviving_azs': {}}
        
        total_overflow = {}
        for resource in ['cpu', 'ram', 'gpu', 'disk']:
            failed_demand = capacity.get_az_committed(failed_az, resource)
            surviving_free = sum(
                capacity.get_az_free(az, resource) for az in surviving_azs)
            
            total_overflow[resource] = max(0, failed_demand - surviving_free)
            
            results[f'{resource}_failed_demand'] = failed_demand
            results[f'{resource}_surviving_free'] = surviving_free
            results[f'{resource}_overflow'] = total_overflow[resource]
            results[f'{resource}_can_absorb'] = total_overflow[resource] == 0
        
        can_survive = all(v == 0 for v in total_overflow.values())
        results['can_survive_without_procurement'] = can_survive
        
        if not can_survive:
            for resource, overflow in total_overflow.items():
                if overflow > 0:
                    sku = self._best_sku_for_resource(resource, region)
                    nodes = math.ceil(overflow / sku.resource_per_node(resource))
                    results[f'{resource}_nodes_needed'] = nodes
                    results[f'{resource}_procurement_cost'] = nodes * sku.unit_capex_usd
        
        # Per-surviving-AZ breakdown
        for az in surviving_azs:
            az_cap = capacity.get_az(az)
            proportional_load = failed_workloads.scale(
                1.0 / len(surviving_azs))  # assume even distribution
            
            results['surviving_azs'][az] = {
                'current_utilization': az_cap.utilization(),
                'post_failover_utilization': az_cap.utilization_with(proportional_load),
                'headroom_remaining': az_cap.headroom_with(proportional_load)
            }
        
        return SimulationResult(scenario=scenario, results=results)
```

**Failure Modes:**
1. **Simulation based on stale data:** Capacity snapshot is 1 hour old, significant changes occurred. Mitigation: simulations always start by refreshing the capacity snapshot from VictoriaMetrics (live query, ~2 second overhead).
2. **Overly simplistic failover model:** Real failover involves application-specific logic (not all workloads can cross AZs). Mitigation: tag workloads with failover constraints. The simulation respects these tags.
3. **Cost model outdated:** Hardware prices change. Mitigation: cost model is updated quarterly from procurement contracts. Cloud pricing is updated via API (AWS/GCP pricing APIs).

**Interviewer Q&As:**

**Q1: How do you validate simulation results?**
A: We compare past simulations against actual outcomes. Example: 6 months ago we simulated "50% GPU demand growth." We compare the simulation's predicted exhaustion date against what actually happened. We also conduct annual disaster recovery drills that provide real failover data to validate AZ failure simulations.

**Q2: How do you handle cascading effects in simulations?**
A: First-order effects are modeled explicitly (workload redistribution, capacity check). Second-order effects (performance degradation under higher load, increased failure rates) are estimated using historical correlations: "when utilization exceeds 80%, failure rate increases by 15%." Third-order effects are not modeled — we flag simulations with utilization > 85% as "high risk of cascading failure."

**Q3: Can you simulate the impact of a new application deployment?**
A: Yes. The user provides the application's resource profile (CPU, RAM, GPU per instance, expected instance count, growth rate). The simulator adds this demand to the current forecast and checks capacity. It's essentially a `demand_growth` scenario with application-specific parameters.

**Q4: How do you simulate cost optimization?**
A: The optimizer models the current workload mix as reserved capacity (long-term, discounted), on-demand (pay-as-you-go, full price), and spot (cheap, interruptible). It uses historical utilization patterns to find the optimal mix. Example: if baseline demand is 60% of capacity, reserve 60%. On-demand handles the next 25%. Spot handles the remaining 15% (interruptible batch jobs).

**Q5: What's the typical simulation runtime?**
A: Simple scenarios (demand growth): 2-5 seconds. Complex scenarios (multi-region failover with constraint checking): 10-20 seconds. Cost optimization with ILP solver: 20-30 seconds.

**Q6: How do you present simulation results to non-technical stakeholders?**
A: Three output formats: (1) Executive summary: one paragraph with key numbers ("GPU capacity will be exhausted in 72 days. Order 32 H100 nodes by April 20 for $8M CapEx."). (2) Visual dashboard: charts showing capacity vs demand over time with the scenario applied. (3) Detailed JSON/CSV for engineering analysis.

---

### 6.3 Procurement Recommender

**Why it's hard:**
Procurement decisions involve multiple constraints: budget (CapEx and OpEx), lead time (3-6 months for bare metal), SKU availability (some SKUs are end-of-sale), regional power/cooling capacity, minimum order quantities, volume discounts, and technology transitions (A100 → H100). The recommender must produce a procurement plan that satisfies demand forecasts within all these constraints while minimizing total cost.

**Implementation:**

```python
class ProcurementRecommender:
    def generate_recommendations(self, forecast_run_id: str) -> List[ProcurementRecommendation]:
        recommendations = []
        
        # Find all (AZ, SKU, resource) combinations predicted to breach thresholds
        at_risk = self.db.query("""
            SELECT DISTINCT availability_zone, sku_type, resource_type,
                   MIN(days_until_exhaustion) as min_days_to_exhaustion,
                   MAX(forecasted_utilization) as peak_utilization
            FROM capacity_forecasts
            WHERE forecast_run_id = %s
              AND days_until_exhaustion IS NOT NULL
              AND days_until_exhaustion <= 180
            GROUP BY availability_zone, sku_type, resource_type
            ORDER BY min_days_to_exhaustion ASC
        """, forecast_run_id)
        
        for risk in at_risk:
            sku = self._get_sku(risk.sku_type)
            
            # Calculate how much to order
            current_cap = self._get_current_capacity(risk.availability_zone, 
                                                       risk.sku_type, risk.resource_type)
            forecast_90d = self._get_forecast_value(forecast_run_id, risk.availability_zone,
                                                     risk.sku_type, risk.resource_type, 90)
            
            buffer_target = 0.20  # 20% headroom
            target_capacity = forecast_90d / (1 - buffer_target)
            additional_needed = target_capacity - current_cap
            
            if additional_needed <= 0:
                continue
            
            nodes_needed = max(
                math.ceil(additional_needed / sku.resource_per_node(risk.resource_type)),
                sku.min_order_quantity)
            
            # Check if SKU is still orderable
            if not sku.is_orderable:
                # Recommend replacement SKU
                replacement = self._find_replacement_sku(sku)
                if replacement:
                    sku = replacement
                    nodes_needed = math.ceil(additional_needed / 
                                             replacement.resource_per_node(risk.resource_type))
                else:
                    log.error(f"No replacement SKU available for {sku.sku_type}")
                    continue
            
            # Calculate order deadline
            lead_time_days = sku.lead_time_weeks * 7
            needed_by = datetime.now() + timedelta(days=risk.min_days_to_exhaustion)
            order_by = needed_by - timedelta(days=lead_time_days)
            
            # Determine urgency
            days_to_order_deadline = (order_by - datetime.now()).days
            if days_to_order_deadline <= 0:
                urgency = 'critical'  # already past order deadline
            elif days_to_order_deadline <= 14:
                urgency = 'high'
            elif days_to_order_deadline <= 30:
                urgency = 'medium'
            else:
                urgency = 'low'
            
            # Check budget
            budget = self._get_current_budget(risk.availability_zone.split('-')[0] + '-' + 
                                               risk.availability_zone.split('-')[1])
            capex_cost = nodes_needed * sku.unit_capex_usd
            monthly_opex = nodes_needed * sku.monthly_opex_usd
            
            recommendations.append(ProcurementRecommendation(
                forecast_run_id=forecast_run_id,
                sku_type=sku.sku_type,
                quantity=nodes_needed,
                region=risk.availability_zone[:risk.availability_zone.rfind('-')],
                target_az=risk.availability_zone,
                trigger_reason='forecast_exhaustion',
                urgency=urgency,
                needed_by_date=needed_by.date(),
                order_by_date=order_by.date(),
                lead_time_weeks=sku.lead_time_weeks,
                estimated_capex_usd=capex_cost,
                estimated_monthly_opex_usd=monthly_opex,
                current_utilization=risk.peak_utilization,
                projected_utilization_30d=self._get_forecast_value(
                    forecast_run_id, risk.availability_zone, risk.sku_type, 
                    risk.resource_type, 30) / current_cap * 100,
                projected_utilization_90d=forecast_90d / current_cap * 100,
                buffer_target_pct=buffer_target * 100
            ))
        
        # Also check for EOL replacements
        eol_recommendations = self._check_eol_replacements()
        recommendations.extend(eol_recommendations)
        
        return recommendations
```

**Interviewer Q&As:**

**Q1: How do you handle budget constraints in procurement recommendations?**
A: Each recommendation includes cost estimates. The approval workflow checks against the quarterly budget. If the total recommended CapEx exceeds budget, recommendations are prioritized by urgency. Critical recommendations that exceed budget trigger an escalation to VP-level for budget override.

**Q2: How do you handle the transition from one GPU generation to the next (e.g., A100 → H100)?**
A: The SKU definition includes an `end_of_sale_date`. When an SKU approaches end-of-sale, the recommender automatically substitutes the replacement SKU. The replacement mapping is maintained in a `sku_replacements` table. The recommender accounts for performance differences (e.g., 1 H100 ≈ 2 A100 for some workloads) when calculating quantities.

**Q3: How do you factor in volume discounts?**
A: The cost model includes tiered pricing: 1-49 nodes at list price, 50-199 at 5% discount, 200+ at 10% discount. The recommender may consolidate multiple small orders into one larger order to hit a discount tier, if the timeline allows.

**Q4: What's the lead time breakdown for bare-metal procurement?**
A: Typical: 2 weeks for purchase order processing, 8-12 weeks for manufacturing and shipping, 1-2 weeks for rack-and-stack in data center, 1 week for burn-in and provisioning. Total: 12-17 weeks. We use the sku-specific `lead_time_weeks` value which accounts for the full cycle.

**Q5: How do you handle emergency procurement (lead time too long)?**
A: Three strategies: (1) Bridge with cloud VMs (provision instantly, higher cost). (2) Borrow capacity from other regions (if available). (3) Deprioritize lower-priority workloads to free capacity. The simulation system models all three options with cost comparisons.

**Q6: How do you prevent over-ordering due to temporary demand spikes?**
A: The forecast ensemble smooths out temporary spikes. Additionally, recommendations require human approval (no automatic purchasing). The approval workflow presents historical accuracy of past forecasts, giving the approver confidence in the prediction.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

The capacity planning system does not directly schedule workloads. It provides data to the placement systems:
1. **Capacity constraints:** "AZ us-east-1a has 200 free H100 GPUs" — consumed by the compute resource allocator.
2. **Procurement triggers:** "At current growth rate, us-east-1a will run out of H100 GPUs in 45 days" — consumed by procurement workflow.
3. **Rebalancing recommendations:** "us-west-2a is 90% utilized while us-east-1b is 60%. Recommend migrating batch workloads." — consumed by operations team.

### Conflict Detection

Capacity conflicts are detected through threshold monitoring:
- **Buffer breach:** `buffer_pct < 20%` for any (AZ, SKU, resource).
- **Forecast exhaustion:** Forecasted demand exceeds capacity within lead time.
- **EOL approaching:** Hardware reaching end-of-life within 6 months without replacement plan.

### Queue & Priority

Procurement recommendations are prioritized:
1. **Critical:** Past order deadline. Capacity exhaustion imminent. Requires immediate action.
2. **High:** Within 2 weeks of order deadline. Escalation to leadership if not approved within 48 hours.
3. **Medium:** Within 1 month of order deadline. Standard approval workflow.
4. **Low:** Beyond 1 month. Included in quarterly planning review.

### Preemption Policy

Not directly applicable. However, if capacity is critically low, the system recommends:
1. Deprioritize preemptible workloads (reduce spot capacity).
2. Freeze non-critical deployments (change freeze).
3. Migrate batch workloads to other regions.

### Starvation Prevention

- **Minimum buffer target:** 20% headroom ensures no AZ/SKU combination is starved.
- **Early warning:** Alerts fire at 70% utilization, giving 30% buffer for unexpected demand.
- **Multi-region awareness:** If one region is nearing capacity, the federation layer routes new workloads to regions with headroom.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling | Notes |
|-----------|---------|-------|
| **Prometheus** | Per-cluster (10 clusters). Federation for cross-cluster queries. | Each Prometheus handles ~5,000 hosts. |
| **VictoriaMetrics** | Clustered mode (vminsert + vmselect + vmstorage). Add vmstorage nodes for capacity. | Handles 166K data points/sec easily. |
| **Forecasting Engine** | Parallelized per (AZ, SKU, resource). Can run on multiple workers. | 800 models parallelized across 10 workers = ~30 sec total. |
| **Scenario Simulator** | Stateless. Can scale horizontally for concurrent simulations. | Typically 1-2 concurrent simulations. |
| **Dashboard** | Grafana with caching proxy. | Standard horizontal scaling. |
| **API Server** | Stateless, behind LB. | Low traffic (50 concurrent users). |

### Database Scaling

| Store | Strategy |
|-------|----------|
| **VictoriaMetrics** | Add vmstorage nodes. Each handles ~2 TB. At 10 TB active, need 5 storage nodes. |
| **MySQL** | Single primary + 1 read replica. Data < 30 GB. Read replica for dashboard queries and report generation. |
| **S3** | Unlimited. Lifecycle policies: transition to Glacier after 1 year. |

### Caching

| Layer | Technology | Data | TTL | Hit Ratio |
|-------|-----------|------|-----|-----------|
| **Dashboard cache** | Grafana built-in + Redis | Pre-computed dashboard panels | 5 min | 90% |
| **API response cache** | Redis | Current capacity snapshots | 60 s | 85% |
| **Forecast cache** | Redis | Latest forecast results | Until next run (24h) | 95% |
| **Cost model cache** | In-process | SKU pricing, budget data | 1 hour | 99% |

**Interviewer Q&As:**

**Q1: How does VictoriaMetrics handle 2.5M time series?**
A: VictoriaMetrics is specifically designed for high-cardinality time series. It uses an inverted index and columnar storage with LZ4 compression. 2.5M active series at 15-second resolution is well within its capabilities (tested to 100M+ series). Each vmstorage node handles ~1M active series.

**Q2: How do you handle Prometheus federation at scale?**
A: We avoid federation queries for real-time dashboards (too slow at scale). Instead, each cluster Prometheus remote-writes to VictoriaMetrics. Cross-cluster queries go to VictoriaMetrics directly. Prometheus is used only for local alerting and short-term dashboards (< 2 hours).

**Q3: How do you manage the 168 TB of raw metric data?**
A: We don't store 168 TB of raw data. Raw 15-second data is retained for only 14 days (~3.2 TB). After 14 days, it's downsampled to 5-minute resolution (90-day retention, ~1 TB). Then 1-hour resolution (2-year retention, ~1.4 TB). Total active storage: ~5.6 TB.

**Q4: How do you handle metric backfill after a Prometheus outage?**
A: VictoriaMetrics accepts out-of-order data. If Prometheus is down for 30 minutes and then recovers, the local storage (2-hour retention) contains the missed data. Remote-write backfills it to VictoriaMetrics. For longer outages, the gap is visible in dashboards and forecasting models handle missing data gracefully (Prophet and ARIMA both handle gaps).

**Q5: How do you scale the forecasting engine for larger fleets?**
A: The current 800 models are embarrassingly parallel. Each model trains independently. We can distribute across a Spark cluster or simply add more worker processes. At 10x the fleet (500,000 hosts, 8,000 models), training takes ~5 minutes on 100 workers.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery | RTO |
|---------|--------|-----------|----------|-----|
| Prometheus down (1 cluster) | Metric gap for 5,000 hosts | Prometheus alerting, federation health | Prometheus restarts. VictoriaMetrics backfills from Prometheus WAL. | 5 min |
| VictoriaMetrics down | No cross-cluster metrics, dashboards broken | Health endpoint | Clustered mode: vmstorage replica takes over. Non-clustered: restart from WAL. | 1-5 min |
| MySQL down | Forecast results unreadable, procurement workflow blocked | Health check, replication lag | Promote read replica. | 30-60 s |
| Forecasting engine crash | Stale forecasts (up to 24 hours) | Forecast age monitoring | Restart. Forecasts are regenerated daily. | Next daily run |
| S3 down | Archived data and reports inaccessible | S3 health endpoint | AWS handles. Multi-AZ replication. | AWS SLA |
| Dashboard down (Grafana) | No visualization | HTTP health check | Restart Grafana pod. Stateless (config in configmaps). | 30 s |

### Automated Recovery

1. **Prometheus:** Kubernetes StatefulSet with PVC. Automatic restart. WAL ensures no data loss for short outages.
2. **VictoriaMetrics:** Clustered mode with replication factor 2. Automatic failover for vmstorage nodes.
3. **Forecast pipeline:** Retry on failure. If still failing, alert on-call. Stale forecasts (> 48 hours) trigger a P3 alert.
4. **Report generation:** Retry 3 times. On failure, alert capacity planning team.

### Retry Strategy

| Operation | Strategy | Retries | Backoff |
|-----------|----------|---------|---------|
| Metric query (VictoriaMetrics) | Retry | 3 | 1s, 2s, 4s |
| Forecast model training | Retry with fallback model | 2 | Immediate |
| Report generation | Retry | 3 | 30s |
| Procurement email notification | Retry with fallback channel | 5 | 1min |

### Circuit Breaker

| Dependency | Threshold | Open Duration | Behavior |
|------------|-----------|---------------|----------|
| VictoriaMetrics | 3 failures/10s | 30s | Dashboard shows "data unavailable." Cached data served. |
| MySQL | 3 failures/10s | 30s | API returns cached forecasts. Write operations queued. |
| Notification service | 5 failures/30s | 60s | Alerts queued for later delivery. |

### Consensus & Coordination

Not critical for capacity planning (it's an analytical system, not a real-time system). MySQL single-primary serializes writes. No distributed consensus needed.

---

## 10. Observability

### Key Metrics

| Metric | Type | Labels | Alert Threshold |
|--------|------|--------|-----------------|
| `capacity_utilization_pct` | Gauge | region, az, sku, resource | > 85% for 1 hour |
| `capacity_buffer_pct` | Gauge | region, az, sku, resource | < 15% for 1 hour |
| `forecast_mape_30d` | Gauge | az, sku, resource, model | > 20% |
| `forecast_days_to_exhaustion` | Gauge | az, sku, resource | < lead_time + 30 days |
| `procurement_pending_count` | Gauge | urgency | Critical > 0 for > 48h |
| `metric_ingest_rate` | Gauge | cluster | Drop > 20% from baseline |
| `metric_lag_seconds` | Gauge | cluster | > 120s |
| `forecast_age_hours` | Gauge | | > 48h |
| `budget_utilization_pct` | Gauge | region, quarter | > 90% |
| `eol_hosts_count` | Gauge | region, within_months | > 100 within 6 months |

### Distributed Tracing

Tracing is less critical for an analytical system but useful for debugging:
- Forecast pipeline: trace from data fetch → model training → result storage → alert evaluation.
- Simulation: trace from request → data load → model execution → result generation.

### Logging

| Level | When | Content |
|-------|------|---------|
| INFO | Forecast run completed | run_id, models_trained, total_time, top_risks |
| INFO | Procurement recommendation generated | sku, quantity, region, urgency, cost |
| WARN | Buffer breach | region, az, sku, resource, current_buffer |
| WARN | Forecast accuracy degradation | model, az, sku, resource, mape |
| ERROR | Forecast pipeline failure | error, model, input_data_summary |
| ERROR | Metric collection gap | cluster, duration, affected_hosts |

### Alerting

| Alert | Condition | Severity | Action |
|-------|-----------|----------|--------|
| CapacityBufferBreach | Buffer < 15% for any AZ/SKU for 1 hour | P2 | Investigate demand spike. Consider emergency procurement. |
| ForecastExhaustion | Days to exhaustion < lead_time + 30 | P2 | Create procurement recommendation. Escalate if not approved in 48h. |
| GPUCapacityCritical | GPU utilization > 90% for 4 hours | P1 | Immediate action: redistribute workloads, defer non-critical jobs. |
| ForecastStale | No forecast in > 48 hours | P3 | Investigate forecast pipeline failure. |
| EOLApproaching | > 100 hosts reaching EOL within 3 months without replacement | P2 | Create EOL replacement procurement plan. |
| BudgetExhausted | Quarterly budget > 90% spent | P3 | Alert finance team. Defer non-critical procurement. |
| MetricGap | Metric collection gap > 5 min for any cluster | P2 | Investigate Prometheus health. |

---

## 11. Security

### Auth & AuthZ

- **Dashboard access:** SSO (SAML/OIDC). Three roles: viewer (read dashboards), planner (run simulations, view forecasts), admin (manage alerts, approve procurement).
- **Procurement approval:** Requires two approvals: team lead + finance. Orders > $1M require VP approval.
- **API access:** JWT with role-based claims. Service accounts for automated integrations (billing, procurement system).
- **Metric access:** Prometheus metrics are not tenant-isolated (infrastructure metrics are shared). Dashboard filters enforce per-team visibility where needed.

### Secrets Management

- Prometheus/VictoriaMetrics: internal network only, no auth (or basic auth via reverse proxy).
- MySQL: Vault dynamic credentials, 1-hour TTL.
- S3: IAM role-based access (no static credentials).
- Grafana: SSO integration, no local accounts.

### Audit Logging

- Procurement approvals/rejections logged with approver, timestamp, and comments.
- Alert configuration changes logged.
- Simulation runs logged (who, what scenario, when).
- Budget modifications logged.

---

## 12. Incremental Rollout Strategy

**Phase 1 — Metrics Collection (Week 1-4):**
Deploy VictoriaMetrics, configure Prometheus remote-write. Validate metric completeness (all 50,000 hosts reporting).

**Phase 2 — Dashboard (Week 5-6):**
Deploy Grafana dashboards for current capacity visualization. Train team on USE method interpretation.

**Phase 3 — Forecasting (Week 7-10):**
Deploy forecasting engine. Run in shadow mode for 4 weeks. Compare forecasts against actual utilization retrospectively. Tune model weights.

**Phase 4 — Alerting (Week 11-12):**
Enable capacity alerts. Start with high thresholds (90% utilization) and lower over time.

**Phase 5 — Procurement Integration (Week 13-16):**
Connect forecasting to procurement workflow. Generate recommendations. Initially informational only (not actionable). After 4 weeks of validated recommendations, enable approval workflow.

**Phase 6 — Simulation (Week 17-20):**
Deploy scenario simulator. Train capacity planning team. Run standard scenarios (AZ failure, demand growth) quarterly.

**Rollout Q&As:**

**Q1: How do you validate forecast accuracy before trusting it for procurement?**
A: Run forecasts in shadow mode for 4-8 weeks. Compare 7-day, 14-day, and 30-day forecasts against actual utilization. Require MAPE < 15% for 30-day forecasts before using for procurement decisions. Track accuracy continuously and alert if it degrades.

**Q2: How do you handle the transition from manual to system-driven capacity planning?**
A: The system starts as an advisor, not a decision-maker. All procurement recommendations require human approval. The capacity planning team reviews recommendations against their own analysis. Over time, as trust builds, the team can rely more on the system's recommendations.

**Q3: What if the forecasting model is consistently wrong for a specific SKU?**
A: Monitor per-SKU MAPE. If a specific SKU's MAPE exceeds 20% for 30 days, fall back to manual forecasting for that SKU while investigating. Common causes: demand pattern change (the model needs retraining with different parameters), insufficient data (new SKU), or external factor not captured by the model.

**Q4: How do you handle the quarterly budget planning cycle?**
A: The system generates a quarterly capacity plan at the start of each budget cycle. The plan includes: current utilization, 90-day forecast, procurement recommendations, and cost projections. Finance reviews and allocates budget. Throughout the quarter, the system tracks budget utilization and alerts when approaching limits.

**Q5: How do you coordinate with the data center operations team for physical capacity?**
A: The procurement recommender checks available rack space, power, and cooling capacity before generating recommendations. If the data center is at physical capacity, the recommendation includes a note: "Requires new data center build-out (18-month lead time)" or "Route to cloud VMs as bridge capacity."

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Risk |
|----------|-------------------|--------|-----------|------|
| Time-series DB | Prometheus long-term vs Thanos vs VictoriaMetrics vs InfluxDB | VictoriaMetrics | Best compression ratio, lowest operational complexity, PromQL compatible. | Smaller community than Thanos. Single-vendor dependency. |
| Forecasting model | Single model (Prophet) vs Multi-model ensemble | Ensemble (Prophet + ARIMA + LR) | Reduces individual model bias. Better accuracy. | More complex to maintain and debug. |
| Buffer target | 10% vs 20% vs 30% | 20% | Balance between cost (idle capacity) and safety (demand spikes). 20% covers typical forecast error range. | Under: risk of capacity shortage. Over: wasted capital. |
| Procurement approval | Fully automated vs Fully manual vs Hybrid | Hybrid (system recommends, human approves) | High-value decisions ($millions) warrant human judgment. System handles analysis. | Slow approval process could miss ordering deadlines. |
| Downsampling strategy | Keep all raw data vs Aggressive downsample | Multi-resolution (14d raw, 90d 5-min, 2y hourly) | Balances query resolution needs with storage costs. | Loss of granularity for historical analysis. |
| Cost model scope | Compute-only vs Full TCO | Full TCO (CapEx + OpEx + depreciation + power) | More accurate cost comparison between options. | More data to maintain and keep current. |
| Dashboard tech | Custom web app vs Grafana vs Datadog | Grafana + custom simulation app | Grafana excellent for metrics viz. Custom app for simulation (Grafana can't do this). | Two systems to maintain. |

---

## 14. Agentic AI Integration

### AI-Powered Capacity Planning

**1. Intelligent Demand Forecasting:**
Replace or augment the statistical ensemble with an LLM-assisted forecasting agent. The agent combines quantitative forecasts with qualitative signals: Jira tickets for upcoming launches, Slack messages about new ML experiments, team headcount growth plans, and industry trends. Example: "Based on the statistical forecast (75% GPU utilization in 90 days) plus 3 new ML projects in the team's Q2 roadmap (each requiring ~16 GPUs), I recommend planning for 90% GPU utilization by end of Q2."

**2. Natural Language Capacity Queries:**
- "Do we have enough GPUs for the Q3 training plan?" → Agent queries forecasts, reservation data, and team roadmaps. Returns: "Current plan requires 320 H100 GPUs in Q3. We have 256 available. Recommend ordering 128 GPUs (includes 20% buffer) by May 1 ($4M CapEx)."
- "What's the cheapest way to add 100 more CPU nodes in us-east-1?" → Agent compares: bare-metal (cheapest TCO over 3 years, but 4-month lead time), cloud reserved instances (15% more expensive but available in 24 hours), cloud on-demand (50% more expensive but instant).
- "What happens if we delay the H100 order by 2 months?" → Agent runs simulation: "GPU utilization will reach 95% in 60 days. Workload queueing will increase by 3x. Estimated productivity loss: $2M. Recommendation: do not delay."

**3. Automated Report Generation:**
An LLM agent generates natural-language capacity reports from raw data:
- Executive summary for leadership.
- Technical details for infrastructure team.
- Cost analysis for finance.
- Risk assessment with mitigation recommendations.

**4. Procurement Negotiation Support:**
The AI agent prepares negotiation briefs for hardware vendors: "Based on our 3-year demand forecast, we need 2,000 H100 nodes. Here are three pricing scenarios (aggressive, moderate, conservative) with NPV calculations. Recommendation: negotiate a 3-year volume commitment for 15% discount."

**5. Anomaly-Driven Capacity Insights:**
AI agent detects unusual capacity patterns: "GPU utilization in us-west-2a dropped 15% this week. Investigation: the ML team cancelled 3 large training jobs due to model convergence issues. This is temporary — utilization will recover when they retry with adjusted hyperparameters. No procurement impact."

### Guard Rails
- All procurement recommendations require human approval regardless of AI confidence.
- Forecast models validated against backtests before deployment.
- AI-generated reports clearly marked as machine-generated, with links to source data.
- Cost projections include confidence intervals and assumptions.
- Kill switch: disable AI features without affecting core monitoring and alerting.

---

## 15. Complete Interviewer Q&A Bank

**Q1: What is the USE method and why is it important for capacity planning?**
A: USE stands for Utilization, Saturation, and Errors — a methodology by Brendan Gregg for analyzing system performance. **Utilization**: the percentage of time a resource is busy (or the percentage of capacity in use). **Saturation**: the degree to which extra work is queued (e.g., CPU run queue length). **Errors**: the count of error events. For capacity planning, utilization tells you how much capacity is consumed, saturation tells you if performance is degrading, and errors tell you if capacity limits are causing failures. A system at 70% CPU utilization but with high saturation (long run queue) needs more capacity than utilization alone suggests.

**Q2: How do you forecast demand for a resource with seasonal patterns?**
A: Prophet handles seasonality automatically: daily (workday vs night), weekly (weekday vs weekend), and yearly (holiday seasons, end-of-quarter). We configure Prophet with these seasonality components enabled. For infrastructure specifically, we also add custom seasonality for "deployment windows" (Tuesday and Thursday in many orgs) and "batch job windows" (overnight).

**Q3: What's the difference between committed resources and actual utilization?**
A: Committed (requested) resources are what workloads asked for (e.g., pod requests 4 CPU cores). Actual utilization is what they're really using (e.g., only using 1.5 cores). The gap represents overcommit opportunity. For capacity planning, we track both: committed resources determine when the scheduler can't place new workloads, while actual utilization determines when performance degrades.

**Q4: How do you handle the 3-6 month lead time for bare-metal procurement?**
A: We forecast 6 months ahead and trigger procurement recommendations when the forecast shows exhaustion within (lead_time + 30 days). The 30-day safety margin accounts for forecast uncertainty. We maintain a "warm pool" of pre-provisioned but unallocated hosts (5% of fleet) for emergency bridging while procurement is in progress.

**Q5: How do you decide the buffer capacity target?**
A: 20% is our default, derived from: (1) Historical forecast error: 90-day MAPE is ~15%, so 15% buffer covers forecast errors. (2) Spike absorption: daily utilization varies by ~10% from peak to trough. (3) Maintenance overhead: 2-3% of hosts are always in maintenance. Total: 15 + 10 + 3 ≈ 28%, rounded down to 20% (we accept some risk). GPU resources use a 15% buffer (lower because GPU demand is more predictable due to reservations).

**Q6: How do you model the total cost of ownership (TCO) for bare-metal vs cloud?**
A: TCO components: (1) **CapEx**: server purchase price, depreciated over 4 years. (2) **OpEx**: power (~$0.10/kWh x kW per server x 8,760 hours), cooling (typically 1.3x power), network (bandwidth costs), rack space ($500-1000/U/month), operations staff (allocated per-server cost). (3) **Cloud equivalent**: on-demand pricing x utilization hours. Bare-metal TCO is typically 40-60% of cloud on-demand pricing at scale (> 1,000 servers), but requires upfront capital and longer commitment.

**Q7: How do you handle capacity planning for multi-tenant environments?**
A: Per-tenant capacity tracking in addition to per-cluster. Each tenant has quotas (handled by the quota system). For capacity planning, we forecast per-tenant demand (driven by their growth rate) and aggregate. This catches scenarios where one tenant's growth consumes disproportionate capacity. We also identify "whale" tenants whose demand significantly impacts planning.

**Q8: What's the role of simulation in capacity planning?**
A: Simulation answers "what-if" questions that forecasting can't: "What if we lose a region?", "What if a competitor launches and our traffic doubles?", "What if we migrate from VMs to containers (4x density improvement)?". These are discontinuous changes that statistical models can't predict from historical data. Simulation takes the forecast as a baseline and applies scenario-specific transformations.

**Q9: How do you prioritize procurement when budget is limited?**
A: Priority order: (1) **Safety**: capacity needed to maintain SLA (avoid outages). (2) **Commitment**: capacity for committed product launches (business obligations). (3) **Growth**: capacity for organic growth (per forecast). (4) **Optimization**: capacity for efficiency improvements (e.g., newer, denser SKUs). (5) **Innovation**: capacity for R&D and experimentation. When budget is tight, categories 4 and 5 are deferred.

**Q10: How do you track and improve forecast accuracy over time?**
A: Every forecast is stored with its predicted values. When the forecasted date arrives, actual utilization is recorded and compared. We compute per-model MAPE at 7, 14, 30, 60, and 90-day horizons. Trends in accuracy are tracked: if MAPE is increasing, the model may need retraining with updated parameters or the demand pattern may have fundamentally changed. We A/B test model changes by running the new model in parallel and comparing accuracy.

**Q11: How do you handle capacity planning for GPU resources specifically?**
A: GPU capacity planning differs from CPU: (1) Higher unit cost ($30-40K per GPU vs $500 per CPU core equivalent). (2) Longer lead time (GPUs are supply-constrained). (3) Lumpier demand (large ML training jobs vs many small services). (4) Technology transitions happen faster (new GPU generations every 2 years). We use reservation data (known future GPU jobs) to supplement statistical forecasting. We also maintain relationships with GPU vendors for demand signals (allocation queues).

**Q12: How do you coordinate capacity across regions?**
A: The federation layer provides a global view. The capacity planning system generates per-region forecasts and a global aggregate. Cross-region recommendations include: workload redistribution (move batch jobs to under-utilized regions), procurement balancing (if one region is full but another has headroom, consider routing growth to the less-loaded region), and DR readiness (each region should have enough spare capacity to absorb 50% of another region's load).

**Q13: How do you handle the depreciation and refresh cycle for bare-metal hardware?**
A: Hosts are depreciated over 48 months (4 years). At month 36, they enter the "refresh planning" phase. The system identifies hosts approaching EOL, estimates the replacement SKU and quantity, and generates procurement recommendations. The goal is smooth fleet turnover: ~25% of hosts replaced each year, avoiding large batch replacements.

**Q14: What metrics would you monitor to detect that capacity planning is failing?**
A: (1) Number of workloads stuck in "pending" state > 5 minutes (capacity insufficient). (2) Number of emergency procurement events (should be zero — means planning failed to predict demand). (3) Idle capacity cost (overcommitted CapEx sitting unused). (4) Forecast MAPE (accuracy degrading). (5) Time between alert and procurement delivery (the full response time).

**Q15: How would you use this system to justify a $50M infrastructure budget to the CFO?**
A: Present: (1) Current fleet cost and utilization (showing high utilization = good ROI). (2) Demand forecast with confidence intervals (evidence-based growth projection). (3) Cost comparison: bare-metal TCO vs cloud equivalent (showing ~50% savings at our scale). (4) Risk analysis: what happens if we under-invest (outages, lost revenue, customer churn). (5) ROI calculation: the incremental capacity enables $X in revenue from product growth. (6) Quarterly procurement plan with cash flow timeline.

**Q16: How does capacity planning change with the rise of AI/ML workloads?**
A: Three major impacts: (1) GPU demand is growing 2-5x annually (vs CPU at 10-20%). Planning horizons must extend further due to GPU supply constraints. (2) ML training jobs are bursty (days of intense GPU usage followed by idle periods). This makes forecasting harder but reservation-based planning more effective. (3) GPU clusters require specialized infrastructure (InfiniBand networking, high-power racks), adding physical capacity constraints that CPU-only data centers don't face.

---

## 16. References

1. Gregg, B. "The USE Method." http://www.brendangregg.com/usemethod.html
2. Taylor, S.J., Letham, B. "Forecasting at Scale." *PeerJ Preprints, 2017* (Prophet paper).
3. Box, G.E.P., Jenkins, G.M. "Time Series Analysis: Forecasting and Control." *Wiley, 2015* (ARIMA).
4. Hamilton, J. "On Designing and Deploying Internet-Scale Services." *LISA 2007*.
5. Barroso, L.A., et al. "The Datacenter as a Computer: Designing Warehouse-Scale Machines." *Morgan & Claypool, 2018*.
6. VictoriaMetrics documentation: https://docs.victoriametrics.com/
7. Prometheus Best Practices: https://prometheus.io/docs/practices/
8. Thanos: https://thanos.io/
9. AWS Total Cost of Ownership (TCO) Calculator: https://calculator.aws/
10. Verma, A., et al. "Large-scale cluster management at Google with Borg." *EuroSys 2015* (capacity management lessons).
11. NVIDIA Data Center GPU Planning Guide: https://www.nvidia.com/en-us/data-center/resources/
