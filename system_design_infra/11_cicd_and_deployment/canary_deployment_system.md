# System Design: Canary Deployment System

> **Relevance to role:** A cloud infrastructure platform engineer must design progressive delivery systems that minimize blast radius when deploying to Kubernetes clusters and bare-metal fleets. Canary deployment -- routing a small percentage of traffic to the new version and validating metrics before expanding -- is the gold standard for safe production releases. Interviewers expect deep knowledge of traffic splitting (Istio, Envoy, Argo Rollouts), automated analysis and promotion, rollback triggers, and integration with observability systems (Prometheus, Elasticsearch).

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|-------------|
| FR-1 | Deploy a new version as a canary alongside the stable version. |
| FR-2 | Split traffic progressively: 1% -> 5% -> 25% -> 50% -> 100%. |
| FR-3 | At each step, run automated analysis against Prometheus metrics (error rate, latency, saturation). |
| FR-4 | Automatic promotion if metrics pass thresholds; automatic rollback if metrics fail. |
| FR-5 | Support header-based canary routing (route specific users/internal traffic to canary). |
| FR-6 | Support manual approval gates at configurable promotion steps. |
| FR-7 | Support Kubernetes-native canary (Argo Rollouts, Flagger, Istio VirtualService). |
| FR-8 | Support bare-metal/VM canary via load balancer weight adjustment. |
| FR-9 | Integrate with CI/CD pipeline: pipeline triggers canary, monitors, and reports final status. |
| FR-10 | A/B testing integration: route specific user segments to canary for business metric comparison. |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Canary pod scheduling latency | < 30 s |
| NFR-2 | Analysis interval | Every 60 s per canary step |
| NFR-3 | Rollback time | < 10 s |
| NFR-4 | Minimum canary traffic accuracy | within 1% of configured weight |
| NFR-5 | System availability | 99.95% |
| NFR-6 | End-to-end canary deployment (1% -> 100%) | < 60 min |

### Constraints & Assumptions
- Kubernetes services use Istio or Linkerd for traffic splitting.
- Prometheus is the primary metrics source.
- Each service defines its own analysis template (SLI/SLO-based).
- Bare-metal services use HAProxy or Envoy for weighted traffic splitting.
- Canary analysis templates are stored alongside the service code (in the same repo).

### Out of Scope
- Business metrics analysis (conversion rate A/B testing) -- can be integrated but not core.
- Multi-cluster canary coordination (each cluster runs independently).
- Database schema changes during canary (handled by blue-green pattern).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Services using canary deployment | 500 services total, 60% opt into canary | 300 |
| Canary deployments/day | 300 x 1.5 deploys/week / 5 days | ~90/day |
| Active canaries at any time | 90/day x avg 45 min canary / 24h / 60min | ~3 concurrent |
| Analysis queries per canary per step | 5 metrics x 1 query/min | 5 queries/min |
| Total Prometheus queries/min from canary system | 3 concurrent x 5 queries/min | 15 queries/min |
| Traffic split configurations/day | 90 x 5 steps (1%, 5%, 25%, 50%, 100%) | ~450 config updates/day |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Canary pod scheduling | < 30 s |
| Traffic weight update (Istio VirtualService) | < 5 s |
| Analysis query (Prometheus) | < 2 s |
| Promotion decision (after analysis) | < 5 s |
| Rollback (scale canary to 0, restore stable) | < 10 s |
| Full canary lifecycle (1% -> 100%) | 30-60 min |

### Storage Estimates

| Item | Calculation | Value |
|------|-------------|-------|
| Canary deployment records | 90/day x 365 | 32,850/year |
| Analysis results per deployment | 5 steps x 5 metrics x 10 data points | 250 data points/deployment |
| Total analysis data | 32,850 x 250 x 200 bytes | ~1.6 GB/year |
| Lightweight system; compute cost is in the additional canary pods. |

### Bandwidth Estimates

| Flow | Value |
|------|-------|
| Prometheus query traffic | < 1 MB/min (metadata only) |
| Istio VirtualService updates | < 1 KB per update |
| Image pull for canary pods | 150 MB x 2-3 canary pods = 300-450 MB per deployment |

---

## 3. High Level Architecture

```
                    +------------------+
                    |   CI/CD Pipeline |
                    +--------+---------+
                             |
                        1. Trigger canary
                             |
                             v
                    +--------+---------+
                    |  Canary          |
                    |  Controller      |
                    | (Argo Rollouts / |
                    |  Flagger /       |
                    |  Custom)         |
                    +--+-----+-----+--+
                       |     |     |
           +-----------+     |     +-----------+
           v                 v                 v
   +-------+------+  +------+-------+  +------+-------+
   | Traffic      |  | Analysis     |  | Notification |
   | Manager      |  | Engine       |  | Service      |
   +-------+------+  +------+-------+  +--------------+
           |                 |
           v                 v
   +-------+------+  +------+-------+
   | Istio / Envoy|  | Prometheus   |
   | VirtualSvc   |  | (Metrics)    |
   | (traffic     |  +--------------+
   |  weights)    |
   +-------+------+
           |
    +------+------+
    |             |
+---v---+   +----v----+
| Stable|   | Canary  |
| v1.2.3|   | v1.2.4  |
| 95%   |   | 5%      |
| Pods  |   | Pods    |
+-------+   +---------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **CI/CD Pipeline** | Builds artifact, triggers canary rollout via Argo Rollouts CRD or Canary Controller API. |
| **Canary Controller** | State machine that orchestrates the canary lifecycle: create canary pods, adjust traffic weights, run analysis, promote or rollback. Implemented as Argo Rollouts, Flagger, or a custom k8s controller. |
| **Traffic Manager** | Abstracts traffic splitting. For k8s: updates Istio VirtualService or SMI TrafficSplit weights. For bare-metal: updates HAProxy/Envoy backend weights. |
| **Analysis Engine** | Queries Prometheus at each canary step. Compares canary metrics against stable baseline and defined thresholds. Returns pass/fail/inconclusive. |
| **Notification Service** | Sends Slack/PagerDuty messages at each step (promotion, rollback, completion). |
| **Stable Pods** | Current production version receiving majority of traffic. |
| **Canary Pods** | New version receiving a small percentage of traffic for validation. |
| **Istio VirtualService** | Defines traffic weights between stable and canary subsets. |
| **Prometheus** | Source of truth for application metrics (error rate, latency percentiles, throughput). |

### Data Flows

1. **Trigger:** Pipeline creates an Argo Rollout with a new image -> Controller starts canary process.
2. **Step 1 (1%):** Controller creates canary pods (1 replica). Updates VirtualService: stable=99%, canary=1%.
3. **Analysis:** Analysis Engine queries Prometheus every 60s for analysis window (e.g., 5 min). Compares canary error rate vs. stable error rate.
4. **Promote:** If analysis passes, Controller advances to next step: stable=95%, canary=5%.
5. **Repeat:** Steps 3-4 for each weight increment (5%, 25%, 50%, 100%).
6. **Completion:** At 100%, Controller scales down old stable pods, makes canary the new stable.
7. **Rollback (if analysis fails):** Controller sets canary weight to 0%, scales canary pods to 0, traffic fully on stable. Alert sent.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Canary rollout definition per service
CREATE TABLE canary_rollouts (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    service_name        VARCHAR(255) NOT NULL,
    from_version        VARCHAR(128) NOT NULL,
    to_version          VARCHAR(128) NOT NULL,
    image_digest        CHAR(71) NOT NULL,
    status              ENUM('pending', 'progressing', 'paused', 'promoted', 'rolled_back', 
                             'failed', 'cancelled') NOT NULL DEFAULT 'pending',
    current_step        INT NOT NULL DEFAULT 0,
    current_weight      INT NOT NULL DEFAULT 0,           -- canary traffic weight (0-100)
    total_steps         INT NOT NULL,
    triggered_by        VARCHAR(255) NOT NULL,
    pipeline_run_id     BIGINT,
    started_at          DATETIME,
    promoted_at         DATETIME,
    rolled_back_at      DATETIME,
    finished_at         DATETIME,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_service_status (service_name, status),
    INDEX idx_created (created_at)
) ENGINE=InnoDB;

-- Individual canary steps
CREATE TABLE canary_steps (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    rollout_id          BIGINT NOT NULL,
    step_index          INT NOT NULL,
    step_type           ENUM('set_weight', 'analysis', 'pause', 'manual_approval') NOT NULL,
    weight_value        INT,                               -- for set_weight steps
    analysis_duration_s INT,                               -- for analysis steps
    status              ENUM('pending', 'running', 'passed', 'failed', 'skipped') NOT NULL DEFAULT 'pending',
    started_at          DATETIME,
    finished_at         DATETIME,
    FOREIGN KEY (rollout_id) REFERENCES canary_rollouts(id),
    INDEX idx_rollout_step (rollout_id, step_index)
) ENGINE=InnoDB;

-- Analysis results for each analysis step
CREATE TABLE analysis_results (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    step_id             BIGINT NOT NULL,
    metric_name         VARCHAR(255) NOT NULL,              -- e.g., "error_rate", "p99_latency"
    canary_value        DOUBLE NOT NULL,
    stable_value        DOUBLE NOT NULL,
    threshold_type      ENUM('absolute', 'relative', 'range') NOT NULL,
    threshold_value     DOUBLE NOT NULL,
    passed              BOOLEAN NOT NULL,
    query_time          DATETIME NOT NULL,
    raw_query           TEXT,                                -- PromQL query used
    FOREIGN KEY (step_id) REFERENCES canary_steps(id),
    INDEX idx_step (step_id)
) ENGINE=InnoDB;

-- Analysis templates (reusable across services)
CREATE TABLE analysis_templates (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    name                VARCHAR(255) NOT NULL UNIQUE,       -- e.g., "standard-web-service"
    metrics             JSON NOT NULL,                      -- array of metric definitions
    success_condition   VARCHAR(512) NOT NULL,              -- e.g., "all metrics pass"
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB;
```

### Database Selection

| Store | Engine | Rationale |
|-------|--------|-----------|
| Rollout state | MySQL 8.0 | Transactional state machine; low write volume (~90/day). |
| Analysis time-series | Prometheus | Native metric storage; PromQL for flexible queries; already deployed. |
| Analysis results (historical) | Elasticsearch | Searchable analysis history for trend analysis. |
| Real-time rollout state | Redis | Fast reads for "what's the current canary weight for service X?" |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| canary_rollouts | (service_name, status) | "Active canary for service X?" |
| canary_steps | (rollout_id, step_index) | Step progression within a rollout |
| analysis_results | (step_id) | "All metrics for this analysis step" |

---

## 5. API Design

### REST Endpoints

```
# Canary rollout management
POST   /api/v1/services/{name}/canary                  # Start canary rollout
       Body: { "version": "v1.2.4", "image": "...", "steps": [...], "analysis_template": "standard" }
GET    /api/v1/services/{name}/canary                   # Get active canary status
POST   /api/v1/services/{name}/canary/promote           # Manually promote to next step
POST   /api/v1/services/{name}/canary/promote-full      # Skip remaining steps, go to 100%
POST   /api/v1/services/{name}/canary/rollback          # Manually rollback
POST   /api/v1/services/{name}/canary/pause             # Pause canary at current step
POST   /api/v1/services/{name}/canary/resume            # Resume paused canary

# Analysis
GET    /api/v1/canary/{id}/analysis                     # Get analysis results for each step
GET    /api/v1/analysis-templates                        # List available analysis templates
POST   /api/v1/analysis-templates                        # Create a new analysis template

# History
GET    /api/v1/services/{name}/canary/history            # Past canary rollouts
GET    /api/v1/canary/{id}                               # Specific rollout details
```

**Example: Start canary rollout**
```json
POST /api/v1/services/order-svc/canary
{
  "version": "v1.2.4",
  "image": "registry.internal/org/order-svc@sha256:abc123...",
  "analysis_template": "standard-web-service",
  "steps": [
    { "setWeight": 1 },
    { "analysis": { "duration": "5m" } },
    { "setWeight": 5 },
    { "analysis": { "duration": "5m" } },
    { "setWeight": 25 },
    { "analysis": { "duration": "5m" } },
    { "pause": { "duration": "0" } },    // manual approval gate
    { "setWeight": 50 },
    { "analysis": { "duration": "10m" } },
    { "setWeight": 100 }
  ],
  "rollback_on": {
    "error_rate_threshold": 0.01,
    "p99_latency_threshold_ms": 500,
    "consecutive_failures": 3
  }
}
```

### CLI

```bash
# Start canary
canary start --service order-svc --version v1.2.4 --template standard-web-service

# Watch canary progress
canary watch --service order-svc

# Check status
canary status --service order-svc

# Manual promote
canary promote --service order-svc

# Full promote (skip to 100%)
canary promote-full --service order-svc

# Rollback
canary rollback --service order-svc

# Pause/resume
canary pause --service order-svc
canary resume --service order-svc

# View analysis results
canary analysis --service order-svc --step 3

# List history
canary history --service order-svc --limit 20
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Traffic Splitting Implementation

**Why it's hard:**
Accurate traffic splitting at low percentages (1%) requires a mechanism that can distribute requests with fine granularity. With 10 pods (5 stable, 1 canary), simple round-robin gives ~17% to canary, not 1%. The traffic split must operate at the network level (L7 proxy), not the pod count level.

**Approaches:**

| Approach | Granularity | Overhead | Complexity |
|----------|-------------|----------|------------|
| **Pod count ratio** | 1/N (minimum 1 canary = 1/N of fleet) | None | Low |
| **Istio VirtualService weight** | 1% granularity | Envoy sidecar per pod | Medium |
| **Linkerd TrafficSplit (SMI)** | 1% granularity | Linkerd proxy per pod | Medium |
| **Nginx Ingress canary annotation** | 1% granularity (via weighted backends) | Nginx Ingress controller | Low |
| **HAProxy weighted backends (bare-metal)** | 1% granularity | HAProxy per service | Low |
| **Application-level routing (SDK)** | Arbitrary (user-based, percentage) | SDK in application | High (app changes) |

**Selected approach: Istio VirtualService for k8s + HAProxy weighted backends for bare-metal**

**Justification:** Istio VirtualService gives us L7 traffic splitting with 1% granularity, header-based routing, and retry/timeout policies. It's the industry standard for Kubernetes canary deployments. For bare-metal, HAProxy `server weight` provides equivalent functionality.

**Implementation detail - Kubernetes with Istio:**

```yaml
# Stable Deployment
apiVersion: apps/v1
kind: Deployment
metadata:
  name: order-svc-stable
spec:
  replicas: 10
  selector:
    matchLabels:
      app: order-svc
      version: stable
  template:
    metadata:
      labels:
        app: order-svc
        version: stable
    spec:
      containers:
        - name: order-svc
          image: registry.internal/org/order-svc@sha256:stable...

---
# Canary Deployment (small replica count)
apiVersion: apps/v1
kind: Deployment
metadata:
  name: order-svc-canary
spec:
  replicas: 2                    # enough to handle 5% of traffic
  selector:
    matchLabels:
      app: order-svc
      version: canary
  template:
    metadata:
      labels:
        app: order-svc
        version: canary
    spec:
      containers:
        - name: order-svc
          image: registry.internal/org/order-svc@sha256:canary...

---
# Istio VirtualService (traffic split)
apiVersion: networking.istio.io/v1beta1
kind: VirtualService
metadata:
  name: order-svc
spec:
  hosts:
    - order-svc
  http:
    # Header-based canary (internal/beta users)
    - match:
        - headers:
            x-canary:
              exact: "true"
      route:
        - destination:
            host: order-svc
            subset: canary
    # Weighted traffic split
    - route:
        - destination:
            host: order-svc
            subset: stable
          weight: 95
        - destination:
            host: order-svc
            subset: canary
          weight: 5

---
# Istio DestinationRule (defines subsets)
apiVersion: networking.istio.io/v1beta1
kind: DestinationRule
metadata:
  name: order-svc
spec:
  host: order-svc
  subsets:
    - name: stable
      labels:
        version: stable
    - name: canary
      labels:
        version: canary
```

**Argo Rollouts implementation (preferred):**

```yaml
apiVersion: argoproj.io/v1alpha1
kind: Rollout
metadata:
  name: order-svc
spec:
  replicas: 10
  strategy:
    canary:
      canaryService: order-svc-canary
      stableService: order-svc-stable
      trafficRouting:
        istio:
          virtualService:
            name: order-svc-vsvc
            routes:
              - primary
          destinationRule:
            name: order-svc-destrule
            canarySubsetName: canary
            stableSubsetName: stable
      steps:
        - setWeight: 1
        - pause: { duration: 5m }
        - analysis:
            templates:
              - templateName: success-rate
              - templateName: latency-check
            args:
              - name: service-name
                value: order-svc
        - setWeight: 5
        - pause: { duration: 5m }
        - analysis:
            templates:
              - templateName: success-rate
              - templateName: latency-check
        - setWeight: 25
        - analysis:
            templates:
              - templateName: success-rate
              - templateName: latency-check
        - setWeight: 50
        - pause: {}                     # manual approval gate
        - analysis:
            templates:
              - templateName: success-rate
              - templateName: latency-check
            args:
              - name: analysis-duration
                value: "10m"
        - setWeight: 100
      rollbackWindow:
        revisions: 3
  selector:
    matchLabels:
      app: order-svc
  template:
    metadata:
      labels:
        app: order-svc
    spec:
      containers:
        - name: order-svc
          image: registry.internal/org/order-svc:TAG
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Istio control plane down | VirtualService updates not propagated; traffic split stale | Envoy sidecars cache last known config; traffic continues at last configured weight. Alert fires. |
| Canary pod OOM / crash | Canary traffic gets errors | Argo Rollouts detects degraded canary pods, triggers analysis re-evaluation. If pods are in CrashLoopBackOff, automatic rollback. |
| Prometheus unavailable | Analysis can't run | Analysis marked `inconclusive`. Policy: if inconclusive for > 3 intervals, pause canary and alert operator. |
| Network issue causes uneven traffic split | Canary gets more/less traffic than configured | Istio VirtualService is authoritative; Envoy enforces weights. Short-lived network issues self-resolve. |

**Interviewer Q&As:**

**Q1: How accurate is Istio's 1% traffic splitting with only 2 canary pods?**
A: Istio's traffic splitting is weight-based at the Envoy proxy level, independent of pod count. Even with 1 canary pod, Envoy routes exactly 1% of requests to it. The pod count determines throughput capacity, not traffic percentage. With 2 canary pods at 1% weight, each pod handles 0.5% of total traffic. We size canary replica count to handle the expected traffic at maximum canary weight (e.g., 50%).

**Q2: How do you handle sticky sessions during canary?**
A: By default, each request is independently routed by weight. For user-session consistency (important for A/B testing), use Istio's `consistentHash` routing based on a header, cookie, or source IP. This ensures a user always sees the same version throughout their session. But be aware: consistent hashing reduces the randomness of the canary sample, potentially introducing selection bias.

**Q3: How do you canary a service with no HTTP traffic (e.g., Kafka consumer)?**
A: For message consumers, partition-based canary: assign 1 out of 20 Kafka partitions to the canary consumer group. This gives ~5% of message traffic to the canary. For job processors, assign a percentage of job queue items to canary workers (via a header or routing key). Analysis still checks error rates and processing latency, but from application metrics rather than Istio.

**Q4: What's the minimum traffic needed for statistically significant canary analysis?**
A: At 1% canary weight, a service doing 10,000 RPS sends 100 RPS to canary. Over a 5-minute analysis window, that's 30,000 requests -- more than enough for statistical significance. For low-traffic services (< 100 RPS), 1% canary is only 1 RPS -- too few requests for reliable analysis. For these, increase canary weight to 10-20% or extend the analysis window to 15+ minutes.

**Q5: How do you handle canary for WebSocket or gRPC streaming connections?**
A: Istio supports traffic splitting for both WebSocket and gRPC. The weight applies at connection establishment time: 5% of new connections go to canary. Once established, the connection persists on that pod. For gRPC, Istio splits at the stream level if using gRPC's round-trip-per-request pattern, or at the connection level for streaming.

**Q6: What happens if someone deploys a new version while a canary is in progress?**
A: The new deployment supersedes the current canary. Argo Rollouts aborts the in-progress canary, resets to stable, then starts a new canary with the newer version. This is safe because the stable version is always the last fully promoted version.

---

### Deep Dive 2: Automated Analysis Engine

**Why it's hard:**
The analysis engine must compare canary performance against baseline (stable) in real-time, handle noisy metrics (natural variance), avoid false positives (rolling back a good version) and false negatives (promoting a bad version), and make decisions within minutes. Statistical rigor is essential.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Fixed threshold** (error rate < 1%) | Simple, deterministic | Doesn't account for baseline; fails if baseline error rate is 0.8% |
| **Baseline comparison** (canary <= stable + tolerance) | Adapts to current conditions | Requires enough traffic for reliable baseline |
| **Statistical test** (Mann-Whitney U, Welch's t-test) | Statistically rigorous | Complex; may be inconclusive with low traffic |
| **Machine learning anomaly detection** | Catches subtle anomalies | Training data needed; black-box decisions |
| **Combined: threshold + baseline comparison** | Practical balance | Two systems to maintain |

**Selected approach: Baseline comparison with configurable tolerance + absolute thresholds as safety net**

**Justification:** Baseline comparison adapts to natural variance (if stable has 0.5% error rate, canary at 0.6% is fine). Absolute thresholds provide a safety net (error rate > 5% is always bad, regardless of baseline). This is the approach used by Netflix (Kayenta) and Google.

**Implementation detail:**

```python
# Canary Analysis Engine

from dataclasses import dataclass
from enum import Enum
import requests

class AnalysisResult(Enum):
    PASS = "pass"
    FAIL = "fail"
    INCONCLUSIVE = "inconclusive"

@dataclass
class MetricAnalysis:
    metric_name: str
    canary_value: float
    stable_value: float
    threshold: float
    threshold_type: str  # "relative" or "absolute"
    result: AnalysisResult
    reason: str

class CanaryAnalysisEngine:
    def __init__(self, prometheus_url: str):
        self.prometheus_url = prometheus_url
    
    def analyze(
        self,
        service_name: str,
        canary_label: str,
        stable_label: str,
        metrics_config: list[dict],
        analysis_window: str = "5m"
    ) -> tuple[AnalysisResult, list[MetricAnalysis]]:
        """
        Compare canary vs. stable metrics.
        Returns overall result + per-metric details.
        """
        results = []
        
        for metric in metrics_config:
            analysis = self._analyze_metric(
                service_name, canary_label, stable_label,
                metric, analysis_window
            )
            results.append(analysis)
        
        # Overall result: fail if any metric fails
        if any(r.result == AnalysisResult.FAIL for r in results):
            return AnalysisResult.FAIL, results
        elif any(r.result == AnalysisResult.INCONCLUSIVE for r in results):
            return AnalysisResult.INCONCLUSIVE, results
        else:
            return AnalysisResult.PASS, results
    
    def _analyze_metric(
        self,
        service_name: str,
        canary_label: str,
        stable_label: str,
        metric_config: dict,
        analysis_window: str
    ) -> MetricAnalysis:
        metric_name = metric_config["name"]
        
        # Build PromQL queries
        if metric_name == "error_rate":
            canary_query = (
                f'sum(rate(http_requests_total{{service="{service_name}",'
                f'version="{canary_label}",status=~"5.."}}[{analysis_window}])) / '
                f'sum(rate(http_requests_total{{service="{service_name}",'
                f'version="{canary_label}"}}[{analysis_window}]))'
            )
            stable_query = canary_query.replace(canary_label, stable_label)
        
        elif metric_name == "p99_latency":
            canary_query = (
                f'histogram_quantile(0.99, sum(rate(http_request_duration_seconds_bucket'
                f'{{service="{service_name}",version="{canary_label}"}}'
                f'[{analysis_window}])) by (le))'
            )
            stable_query = canary_query.replace(canary_label, stable_label)
        
        elif metric_name == "throughput":
            canary_query = (
                f'sum(rate(http_requests_total{{service="{service_name}",'
                f'version="{canary_label}"}}[{analysis_window}]))'
            )
            stable_query = canary_query.replace(canary_label, stable_label)
        
        # Execute queries
        canary_value = self._query_prometheus(canary_query)
        stable_value = self._query_prometheus(stable_query)
        
        # Handle missing data
        if canary_value is None or stable_value is None:
            return MetricAnalysis(
                metric_name=metric_name,
                canary_value=canary_value or 0,
                stable_value=stable_value or 0,
                threshold=metric_config["threshold"],
                threshold_type=metric_config["threshold_type"],
                result=AnalysisResult.INCONCLUSIVE,
                reason="Missing data from Prometheus"
            )
        
        # Evaluate
        threshold = metric_config["threshold"]
        threshold_type = metric_config["threshold_type"]
        direction = metric_config.get("direction", "lower_is_better")
        
        if threshold_type == "relative":
            # Relative: canary must be within X% of stable
            if stable_value == 0:
                passed = canary_value == 0
            else:
                ratio = canary_value / stable_value
                if direction == "lower_is_better":
                    passed = ratio <= (1 + threshold)  # e.g., threshold=0.1 means canary <= 110% of stable
                else:
                    passed = ratio >= (1 - threshold)  # e.g., throughput should not drop > 10%
        
        elif threshold_type == "absolute":
            # Absolute: canary must be below threshold
            if direction == "lower_is_better":
                passed = canary_value <= threshold
            else:
                passed = canary_value >= threshold
        
        return MetricAnalysis(
            metric_name=metric_name,
            canary_value=canary_value,
            stable_value=stable_value,
            threshold=threshold,
            threshold_type=threshold_type,
            result=AnalysisResult.PASS if passed else AnalysisResult.FAIL,
            reason=f"canary={canary_value:.4f} vs stable={stable_value:.4f} "
                   f"(threshold: {threshold_type} {threshold})"
        )
    
    def _query_prometheus(self, query: str) -> float | None:
        try:
            resp = requests.get(
                f"{self.prometheus_url}/api/v1/query",
                params={"query": query},
                timeout=5
            )
            data = resp.json()
            if data["status"] == "success" and data["data"]["result"]:
                return float(data["data"]["result"][0]["value"][1])
            return None
        except Exception:
            return None
```

**Analysis template (Argo Rollouts AnalysisTemplate):**

```yaml
apiVersion: argoproj.io/v1alpha1
kind: AnalysisTemplate
metadata:
  name: success-rate
spec:
  args:
    - name: service-name
  metrics:
    - name: success-rate
      interval: 60s
      count: 5                          # 5 measurements
      successCondition: result[0] >= 0.99  # 99% success rate
      failureCondition: result[0] < 0.95   # below 95% is immediate failure
      failureLimit: 2                    # 2 failed measurements = rollback
      inconclusiveLimit: 3               # 3 inconclusive = pause
      provider:
        prometheus:
          address: http://prometheus.monitoring:9090
          query: |
            sum(rate(http_requests_total{
              service="{{args.service-name}}",
              status!~"5.."
            }[5m])) /
            sum(rate(http_requests_total{
              service="{{args.service-name}}"
            }[5m]))

---
apiVersion: argoproj.io/v1alpha1
kind: AnalysisTemplate
metadata:
  name: latency-check
spec:
  args:
    - name: service-name
  metrics:
    - name: p99-latency
      interval: 60s
      count: 5
      successCondition: result[0] <= 0.5    # p99 < 500ms
      failureCondition: result[0] > 1.0      # p99 > 1s is immediate failure
      failureLimit: 2
      provider:
        prometheus:
          address: http://prometheus.monitoring:9090
          query: |
            histogram_quantile(0.99,
              sum(rate(http_request_duration_seconds_bucket{
                service="{{args.service-name}}"
              }[5m])) by (le)
            )
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Prometheus query timeout | Analysis inconclusive | Retry 3x with backoff. After `inconclusiveLimit` (3), pause canary and alert. |
| Metric cardinality explosion | Slow/failed queries | Use pre-computed recording rules in Prometheus for canary metrics. |
| False positive (good version rolled back) | Wasted deploy cycle | Increase analysis window (more data = less noise). Use statistical tests. Track false-positive rate and tune thresholds. |
| False negative (bad version promoted) | Production errors | Absolute thresholds catch severe regressions. Post-promotion monitoring continues for 30 min. Emergency rollback available. |

**Interviewer Q&As:**

**Q1: How do you handle metrics noise (natural variance) in canary analysis?**
A: Three techniques: (1) Longer analysis windows (5 min instead of 1 min) smooth out short-term spikes. (2) Multiple measurements: the analysis takes 5 samples, and we require 3+ failures (not just 1) to trigger rollback (`failureLimit: 2` means 2 failures allowed out of 5). (3) Relative comparison: instead of "error rate < 1%", we use "canary error rate <= 110% of stable error rate", which adapts to current conditions.

**Q2: How do you set the right thresholds for canary analysis?**
A: We start with SLO-based thresholds: if the service's SLO is 99.9% availability, the canary threshold is 99.5% (slightly relaxed to avoid false positives). Over time, we tune based on historical data: analyze past canaries that were manually rolled back to determine the "true" failure threshold. We also provide per-service override capabilities for teams that need tighter or looser thresholds.

**Q3: What metrics should every canary analysis include?**
A: Minimum: (1) Error rate (5xx / total), (2) p99 latency, (3) Throughput (RPS). Recommended additions: (4) p50 latency (catches broad regressions), (5) CPU/memory usage (catches resource leaks), (6) Downstream dependency error rates (catches cascading failures). Service-specific: database query duration, cache hit rate, queue depth.

**Q4: How do you analyze canary for a service that processes asynchronous events (no HTTP)?**
A: Define custom application metrics. For a Kafka consumer: (1) Consumer lag (should not increase), (2) Message processing error rate, (3) Message processing duration (p99). These metrics are exposed via Prometheus client library in the application and queried by the same analysis engine. The PromQL queries are different, but the analysis logic is identical.

**Q5: How does the analysis engine handle a gradual memory leak in the canary?**
A: Memory usage is tracked as a canary metric: `container_memory_working_set_bytes`. The analysis compares canary memory growth rate vs. stable. A leak manifests as canary memory increasing over time while stable is flat. The absolute threshold catches it when memory exceeds the pod's limit (resulting in OOM). To catch it earlier, we analyze the derivative: `deriv(container_memory_working_set_bytes[5m])`. If canary's growth rate > 2x stable's, flag it.

**Q6: What if the stable baseline is itself degraded?**
A: If stable has a 5% error rate (which is already a problem), relative comparison would allow canary to have 5.5%. This is caught by absolute thresholds ("error rate must be < 1% regardless"). We also compare against SLO targets, not just the current baseline. If the current baseline violates the SLO, the analysis flags it as a warning but doesn't block the canary (the existing version is already broken).

---

### Deep Dive 3: Canary vs. Feature Flags

**Why it's hard:**
Canary deployments and feature flags both enable gradual rollouts, but they operate at different layers. Choosing the wrong one leads to either excessive infrastructure cost (unnecessary canary for a config change) or insufficient safety (using flags for infrastructure changes that need separate pods).

**When to use canary vs. feature flags:**

| Dimension | Canary Deployment | Feature Flag |
|-----------|-------------------|-------------|
| What changes | Binary/image (new code) | Behavior within the same binary |
| Rollout unit | Traffic percentage (all endpoints) | Per-feature, per-user, per-segment |
| Rollback mechanism | Scale down canary pods | Toggle flag |
| Rollback speed | ~10 s (kill canary pods) | < 1 s (flag evaluation) |
| Infrastructure cost | 2 Deployments, 2 sets of pods | None (same pods) |
| Use case | New service version, infra changes, dependency upgrades | New UI feature, algorithm change, config change |
| Testing granularity | Entire request path | Specific code path |
| A/B testing | Possible (segment by header) | Native (segment by user attribute) |

**Decision framework:**

```
Does the change require a new Docker image / binary?
  YES -> Canary deployment (infrastructure change)
  NO  -> Feature flag (behavior change)

Is the change a dependency upgrade / JVM version / OS patch?
  YES -> Canary deployment (runtime environment change)

Is the change a new feature for a subset of users?
  YES -> Feature flag (user-targeted rollout)

Is the change risky and you want to validate with production traffic?
  YES -> Canary deployment (for infra validation)
  YES -> Feature flag (for feature validation)
  
  Both are valid! Use canary for the deployment, then feature flag
  for the feature within the deployed canary.
```

**Combined approach: Canary + Feature Flag**

```
Version 1.2.4 (new code):
  - Contains new checkout flow behind feature flag (flag: "new_checkout", off by default)
  
Deploy:
  1. Canary deploy v1.2.4 (5% traffic) -- validates the binary works
  2. Enable "new_checkout" flag for 1% of users on canary pods -- validates the feature
  3. Promote canary to 100% (flag still at 1%)
  4. Gradually increase flag to 100% over 1 week
  
This separates deployment risk (canary) from feature risk (flag).
```

**Interviewer Q&As:**

**Q1: Can you just use feature flags for everything instead of canary?**
A: No. Feature flags control code paths within the same binary. If the binary itself is broken (segfault, dependency conflict, memory leak), a feature flag can't help because the pod crashes before the flag is evaluated. Canary deployment validates the binary in production with real traffic before full rollout. You need both: canary for infrastructure safety, flags for feature safety.

**Q2: How do you handle feature flag evaluation during a canary?**
A: Both stable and canary pods use the same feature flag service. Flag evaluation is identical on both. If a flag is changed during a canary, it affects both stable and canary pods. This is fine and expected -- flags and canary are orthogonal. To test a flag only on canary, use a targeting rule: "enable flag for users routed to canary" (via a header or metadata).

**Q3: Should the canary analysis consider feature flag state?**
A: Yes, ideally. If a feature flag was toggled during the analysis window, the metrics may not be comparable (flag change introduces a confounding variable). The analysis engine should detect flag changes (via the feature flag service audit log) and either extend the analysis window or mark the result as inconclusive.

**Q4: Can you A/B test with canary deployments?**
A: Yes, but it's coarser than feature flags. With canary, you A/B test the entire service version (all features). With feature flags, you A/B test individual features. For infrastructure A/B testing (e.g., comparing Java 17 vs Java 21 runtime performance), canary is the right tool. For feature A/B testing, use flags.

---

## 7. Scheduling & Resource Management

### Canary Pod Sizing

| Canary Weight | Required Canary Capacity | Canary Replicas (for 10-pod service at 10K RPS) |
|--------------|-------------------------|------------------------------------------------|
| 1% | 100 RPS | 1 pod (handles ~1K RPS) |
| 5% | 500 RPS | 1 pod |
| 25% | 2,500 RPS | 3 pods |
| 50% | 5,000 RPS | 5 pods |
| 100% | 10,000 RPS | 10 pods (full fleet) |

The Canary Controller automatically scales canary replicas based on the current weight and the service's RPS. Formula: `canary_replicas = ceil(stable_replicas * canary_weight / 100 * 1.5)` (1.5x overhead for safety).

### Resource Allocation

- Canary pods have the same resource requests/limits as stable pods.
- Canary pods use the same PriorityClass as stable pods (production priority).
- If cluster capacity is tight, canary pods are not preempted (they serve production traffic).
- Cluster autoscaler provisions additional nodes if canary pods are pending.

---

## 8. Scaling Strategy

### Scaling for High-Traffic Services

| Challenge | Solution |
|-----------|----------|
| Canary at 1% for a 100K RPS service = 1K RPS on 1-2 pods | Ensure canary pods have headroom. Use HPA for canary Deployment with min replicas = 2. |
| Analysis queries overwhelm Prometheus | Use recording rules for pre-computed canary metrics. Rate-limit analysis queries. |
| 100 services doing canary simultaneously | Each canary is independent. Total additional pods: ~100 (1 canary pod per service at 1%). Manageable. |
| Promoting from 50% to 100% requires scaling canary 2x instantly | Pre-scale canary pods before weight increase. Controller adds pods first, waits for ready, then increases weight. |

### Interviewer Q&As

**Q1: How do you canary deploy a service handling 1 million RPS?**
A: At 1% canary, that's 10K RPS on 2-3 canary pods. Canary pods must be right-sized (same resource requests as stable). Analysis has ample data (10K RPS * 5 min = 3M requests, highly significant statistically). The main challenge is scale-up during promotion: going from 5% to 25% requires growing canary from 5 pods to 25 pods. We pre-scale pods before increasing weight to avoid the new pods being overwhelmed instantly.

**Q2: How do you handle canary for services with extreme latency sensitivity (p99 < 10ms)?**
A: Low-latency services are sensitive to JVM warmup, connection pool initialization, and cache cold starts. The canary pods need a warmup period: route a trickle of traffic (0.1%) for 5 minutes to warm JIT, caches, and connection pools before starting the official analysis at 1%. This is a `setWeight: 0.1` step followed by a pause (not an analysis step).

**Q3: How do you canary across multiple clusters/regions?**
A: Sequential region canary: (1) Canary in region-1 (most isolated). (2) Full rollout in region-1. (3) Canary in region-2. This uses region-1 as a "global canary." Each region has its own Argo Rollouts instance and VirtualService. A global orchestrator coordinates the sequence.

**Q4: How does canary autoscaling work? Does HPA fight with the canary controller?**
A: The canary Deployment has its own HPA. The canary controller sets the minimum replicas based on the current weight. HPA can scale up beyond the minimum based on CPU/memory. They don't fight because: controller sets `replicas` on the Deployment (the floor), HPA adjusts based on metrics (can only go up from the floor). On weight decrease (rollback), the controller sets replicas to 0, overriding HPA.

**Q5: How do you canary a batch processing service?**
A: Instead of traffic-weight canary, use job-percentage canary. A batch scheduler assigns a percentage of jobs to canary workers. Example: 5% of daily batch jobs run on canary pods. Analysis checks: job success rate, processing time, output correctness (sample validation). The canary controller adjusts the job assignment percentage instead of VirtualService weights.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Impact | Detection | Mitigation | Recovery Time |
|---|---------|--------|-----------|------------|---------------|
| 1 | Canary pod crashes (OOM, segfault) | Canary traffic sees errors | Pod CrashLoopBackOff; analysis detects error rate spike | Automatic rollback (< 10s): set canary weight to 0 | < 30 s |
| 2 | Argo Rollouts controller crash | Canary stuck at current step | Controller pod restart; reconciliation resumes | Leader election; new controller reconciles Rollout CRD | < 15 s |
| 3 | Istio control plane (istiod) down | VirtualService weight updates not propagated | Envoy sidecars keep last config; canary stays at current weight | istiod HA (3 replicas); sidecars continue working with cached config | < 30 s |
| 4 | Prometheus down during analysis | Analysis returns inconclusive | Analysis marked inconclusive; exceeds inconclusiveLimit | Canary paused; alert to operator. Prometheus HA with Thanos. | Depends on Prometheus recovery |
| 5 | Analysis false positive (good version rolled back) | Wasted deploy cycle | Post-hoc analysis shows no real regression | Tune thresholds; increase analysis window; add statistical tests | N/A (process improvement) |
| 6 | Analysis false negative (bad version promoted) | Production errors after full promotion | Post-promotion monitoring detects errors | Emergency rollback to previous stable version (same mechanism) | < 30 s |
| 7 | Canary weight set to 50% but only 2 pods (overloaded) | Canary pods overwhelmed; high latency | CPU/memory metrics spike; analysis detects latency regression | Controller should pre-scale pods before increasing weight. Bug: analysis rollback saves the day. | < 30 s (rollback) |
| 8 | Operator accidentally promotes to 100% during testing | Bypasses analysis for remaining steps | Audit log shows manual promotion | Require 2-person approval for `promote-full`. Immediate rollback available. | < 30 s (rollback) |

### Reliability Design

- **Fail-safe default:** If the controller crashes, traffic stays at the last configured weight (Envoy caches config). The stable version always has pods running.
- **Automatic rollback on any analysis failure:** The system is biased toward safety. False positives are preferable to false negatives.
- **Rollback is always cheaper than promotion:** Rollback = set weight to 0 + scale canary to 0. Promotion requires analysis at each step.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Source |
|--------|------|-----------------|--------|
| `canary_weight_current` | Gauge per service | N/A (informational) | Canary Controller |
| `canary_analysis_result` | Counter by (service, result) | failure_count > 3/week | Analysis Engine |
| `canary_rollback_total` | Counter by service | > 2/week (systemic issue) | Canary Controller |
| `canary_duration_seconds` | Histogram | p95 > 3600 s (1 hour) | Canary Controller |
| `canary_step_duration_seconds` | Histogram by step_type | analysis step > 600 s | Canary Controller |
| `canary_error_rate_canary` | Gauge | > 5% | Prometheus (application) |
| `canary_error_rate_stable` | Gauge | > 1% (baseline degraded) | Prometheus (application) |
| `canary_p99_latency_canary_ms` | Gauge | > 2x stable baseline | Prometheus (application) |
| `prometheus_query_duration_seconds` | Histogram | p95 > 5 s | Analysis Engine |
| `canary_false_positive_rate` | Gauge | > 10% (over 30 days) | Post-hoc analysis |

### Dashboards
1. **Canary Overview:** Active canaries, current step, weight, analysis status.
2. **Analysis Detail:** Per-metric comparison (canary vs stable), time-series charts during analysis window.
3. **Canary History:** Success rate, rollback rate, average duration, per-service trends.
4. **Health Comparison:** Side-by-side canary vs. stable metrics (error rate, latency, throughput) in real-time.

---

## 11. Security

| Control | Implementation |
|---------|---------------|
| RBAC | Only CI/CD pipeline or authorized operators can start/promote/rollback canaries. `promote-full` requires elevated role. |
| Audit logging | Every canary action (start, step, promote, rollback) logged with timestamp and identity. |
| Image verification | Canary image must be signed (cosign verified by admission webhook). |
| Analysis integrity | PromQL queries are defined in version-controlled AnalysisTemplates. Templates are immutable once applied to a rollout. |
| Rate limiting | Max 1 active canary per service. Max 20 active canaries across the platform. Prevents accidental resource exhaustion. |
| Network isolation | Canary pods have the same network policies as stable pods. No additional access granted. |

---

## 12. Incremental Rollout Strategy

### Rolling Out the Canary System Itself

**Phase 1: Shadow canary (Week 1-2)**
- Install Argo Rollouts controller. All services still use standard Deployments.
- For 5 volunteer services, convert Deployment to Argo Rollout with a trivial canary (2 steps: setWeight 50%, setWeight 100%, no analysis).
- Validate traffic splitting works.

**Phase 2: Analysis integration (Week 3-4)**
- Add AnalysisTemplates for the 5 services.
- Run analysis in "dry-run" mode: analysis runs and reports results, but doesn't block promotion.
- Tune thresholds based on results.

**Phase 3: Automated canary (Week 5-8)**
- Enable full automated canary (analysis blocks/promotes) for 5 services.
- Expand to 20 services. Each team customizes their AnalysisTemplate.

**Phase 4: Default strategy (Month 3+)**
- Make canary the default deployment strategy for all new services.
- Existing services opt-in over time. Provide migration tooling (Deployment -> Rollout converter).

### Rollout Q&As

**Q1: How do you validate that traffic splitting works correctly during the canary system rollout?**
A: Deploy a test service with two versions that return different response headers (e.g., `X-Version: stable` vs `X-Version: canary`). Send 10,000 requests and verify that the ratio of canary responses matches the configured weight within 2% tolerance. Run this validation test for each traffic splitting mechanism (Istio, HAProxy).

**Q2: How do you handle services that can't use Istio for traffic splitting?**
A: Argo Rollouts supports alternative traffic routers: Nginx Ingress, ALB Ingress Controller, Traefik, SMI. For services not using a service mesh, we use Nginx Ingress annotations for canary traffic splitting. For bare-metal, we use HAProxy weighted backends managed by the canary controller.

**Q3: What if the canary system itself has a bug that prevents rollback?**
A: The rollback mechanism is simple: set VirtualService canary weight to 0 and scale canary replicas to 0. Even if the canary controller is broken, an operator can do this manually with `kubectl edit virtualservice` and `kubectl scale deployment`. We maintain a runbook for manual canary operations. Emergency: `kubectl delete rollout` fully removes canary resources and restores stable.

**Q4: How do you migrate from blue-green to canary for existing services?**
A: Canary and blue-green are not mutually exclusive. We recommend: (1) Canary from 1% to 50% (automated analysis validates the version). (2) Blue-green switch from 50% canary to 100% (instant switch with instant rollback). This gives the best of both: gradual validation + instant rollback. Argo Rollouts supports this hybrid strategy.

**Q5: How do you train teams to write good AnalysisTemplates?**
A: (1) Provide a "standard" template that works for 80% of services (error rate + p99 latency). (2) Documentation with examples for common patterns (Kafka consumer, batch processor, gRPC service). (3) A "canary dry-run" mode where analysis runs but doesn't block, so teams can iterate on thresholds. (4) Platform team reviews custom templates during onboarding.

---

## 13. Trade-offs & Decision Log

| Decision | Option Chosen | Alternative | Rationale |
|----------|---------------|-------------|-----------|
| Traffic splitting | Istio VirtualService | Nginx canary annotations, pod-ratio | Istio provides 1% granularity and header-based routing. Pod-ratio can't do 1%. Nginx is simpler but less flexible. |
| Canary controller | Argo Rollouts | Flagger, custom controller | Argo Rollouts has the best analysis integration, active community, and native Istio support. Flagger is also good but we prefer Argo ecosystem consistency (ArgoCD + Argo Rollouts). |
| Analysis approach | Baseline comparison + absolute thresholds | Statistical tests, ML anomaly detection | Practical balance: baseline comparison adapts to current conditions; absolute thresholds provide safety net. Statistical tests add complexity without proportional benefit at our scale. |
| Analysis metrics source | Prometheus | Datadog, New Relic, CloudWatch | Prometheus is self-hosted, cost-effective, and PromQL is the most flexible query language for metric analysis. Argo Rollouts has native Prometheus provider. |
| Canary step sizes | 1% -> 5% -> 25% -> 50% -> 100% | 10% increments, 2x each step | Starts small (1%) for maximum safety, then accelerates. More granular early steps where risk is highest. The 25% -> 50% jump is validated by analysis. |
| Rollback policy | Fail-fast (rollback on first analysis failure) | Allow N failures, majority vote | Biased toward safety. A single analysis failure could be noise, but we prefer a false positive (wasted deploy) over a false negative (bad code in prod). |
| Manual gate at 50% | Required for production | Fully automated | Human verification before majority traffic is a safety net for subtle issues that automated analysis misses. Configurable per-service (some teams choose fully automated). |

---

## 14. Agentic AI Integration

### AI-Powered Canary Intelligence

| Use Case | Implementation |
|----------|---------------|
| **Adaptive analysis thresholds** | AI agent analyzes historical canary data to recommend per-service thresholds. "For order-svc, error_rate threshold of 0.5% causes 15% false positive rate. Recommend 0.8%." Recomputes monthly. |
| **Anomaly detection beyond metrics** | Agent analyzes log patterns (new ERROR messages, changed response bodies, new exception types) during canary. Catches regressions that metrics miss. Uses LLM to compare canary log samples vs. stable log samples. |
| **Root cause analysis on rollback** | When a canary is rolled back, agent analyzes: which metrics failed, what changed in the code (git diff), and suggests the likely cause. "Canary rolled back due to p99 latency regression. New code adds a synchronous database call in the checkout path (file: CheckoutService.java:142)." |
| **Optimal step timing** | Agent recommends analysis window duration based on traffic volume and metric variance. Low-traffic services get longer windows; high-traffic services can use shorter windows. |
| **Canary comparison across services** | Agent detects correlated canary failures across services. "3 services deployed in the last hour all show elevated error rates. Common dependency: auth-service is degraded." Recommends pausing all canaries until the root cause is resolved. |
| **Smart rollback decisions** | Agent distinguishes between "canary is bad" and "infrastructure is bad." If both stable and canary show degradation, it's likely an infrastructure issue (not canary-specific). Agent recommends pausing canary instead of rolling back. |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through a canary deployment from start to finish.**
A: (1) CI/CD pipeline builds v1.2.4, pushes to registry. (2) Pipeline creates an Argo Rollout with the new image. (3) Argo Rollouts creates canary ReplicaSet with 1 pod. (4) Once pod is ready, updates VirtualService: stable=99%, canary=1%. (5) Analysis runs for 5 min, comparing canary error rate and p99 latency against stable. (6) If pass: weight -> 5%, analyze 5 min. (7) Continue: 25%, analyze. (8) At 50%: manual approval gate (Slack notification). (9) Operator approves. (10) 50%, analyze 10 min (longer at higher weight). (11) Weight -> 100%. (12) Old stable ReplicaSet scaled to 0. New canary becomes stable. (13) Done. Total time: ~45 min.

**Q2: A canary shows 0.5% error rate vs. stable's 0.3%. Should you rollback?**
A: Depends on the threshold. With a relative threshold of 0.5 (50% tolerance), 0.5% vs. 0.3% = 67% higher, which exceeds 50% tolerance -> rollback. With absolute threshold of 1%, 0.5% is within threshold -> pass. We use both: relative catches regressions compared to baseline, absolute catches unacceptable error rates. In this case, the relative threshold triggers a rollback, which is the correct behavior -- a 67% increase in errors is significant even if the absolute rate seems low.

**Q3: How do you canary a breaking API change?**
A: A canary can't validate a breaking API change because stable clients are randomly routed to canary pods and will fail. Breaking changes must use: (1) API versioning (new endpoint `/api/v2/orders`), deployed via canary as new code. (2) Feature flag: deploy the new API behind a flag, enable for specific clients that have migrated. The canary validates the binary; the flag controls API exposure.

**Q4: How does canary work with database schema changes?**
A: Canary pods and stable pods share the same database. Schema changes must be backward-compatible (expand-contract). The migration runs before the canary starts. Both versions work with the new schema. The canary validates the application behavior with the expanded schema. If canary is rolled back, stable continues working with the expanded schema (no harm). See `blue_green_deployment_system.md` for schema migration details.

**Q5: How do you handle canary for a globally distributed service?**
A: Region-by-region canary. Region-1 gets the full canary process (1% -> 100%). If successful, region-2 starts its canary. This uses region-1 as a "global canary." If region-1 canary fails, no other regions are affected. A global orchestrator sequences the per-region canaries and can halt all regions if a systemic issue is detected.

**Q6: What's the minimum observation time for a canary step?**
A: Depends on traffic volume. For reliable analysis, we need at least 1,000 requests at each step. At 1% weight and 10K RPS service, 1% = 100 RPS. 1,000 requests / 100 RPS = 10 seconds of data. But we recommend a minimum of 5 minutes to catch time-dependent issues (slow memory leaks, cache expiry effects, periodic background job interactions).

**Q7: How do you handle canary for a service with very low traffic (10 RPS)?**
A: At 1% canary, that's 0.1 RPS -- practically zero. Solutions: (1) Start at a higher weight (10-20%). (2) Extend analysis window to 15-30 min. (3) Use synthetic traffic: inject known test requests at a higher rate to augment real traffic. (4) Combine canary with feature-flag-based rollout.

**Q8: How do you canary a Java service with JIT warmup issues?**
A: New canary pods have a cold JVM. Initial requests are slower due to interpreted mode. This can trigger a false latency regression in analysis. Mitigation: (1) Warmup pause: route 0.1% traffic for 5 min before starting analysis (JIT compiles hot paths). (2) Exclude the first 2 minutes of data from analysis (`offset` in PromQL). (3) Use CDS/AOT compilation in the Docker image to reduce warmup time.

**Q9: What if an operator manually promotes past the analysis step?**
A: The `promote` API is logged in the audit trail. If manual promotion is allowed (configurable), the analysis step is skipped but a warning is recorded. For production, we can enforce "analysis-required" policy: manual promotion is only allowed for `promote-full` (which requires elevated RBAC role and sends PagerDuty alert).

**Q10: How does canary interact with autoscaling (HPA)?**
A: The canary Deployment has its own HPA. The canary controller sets the base replica count based on weight; HPA can scale up from there. Example: at 25% weight, controller sets 3 canary replicas. If traffic spikes, HPA scales to 5. On promotion to 100%, the canary becomes the stable Deployment with full HPA range.

**Q11: How do you handle canary rollback during a traffic spike?**
A: Rollback is always fast (< 10s): set canary weight to 0 in VirtualService, scale canary to 0. The stable Deployment absorbs 100% of traffic. If the traffic spike was the cause of canary issues (overloaded canary pods), this is the correct action. If the stable Deployment also struggles with the spike, that's an HPA/capacity issue, not a canary issue.

**Q12: How do you prevent canary analysis from being gamed (developer adds artificial health checks)?**
A: Analysis queries use real application metrics (http_requests_total, http_request_duration_seconds) that reflect actual user traffic, not synthetic health checks. We also include infrastructure metrics (CPU, memory) that can't be faked. The AnalysisTemplate is reviewed by the platform team and can't be changed without PR approval.

**Q13: What's the cost overhead of canary deployments?**
A: Minimal. During canary (1-5% weight), only 1-2 extra pods run. That's ~10% overhead per service during the 45-min deploy window. Annual extra cost: 90 deploys/day * 2 pods * 2 CPU * $0.048/CPU-hour * 0.75h = ~$12/day = ~$4,400/year. Trivial compared to the cost of a bad production deployment.

**Q14: How do you handle canary for a gRPC service behind a client-side load balancer?**
A: Client-side load balancing (common in gRPC) means the client picks a backend directly, bypassing Envoy/Istio's traffic splitting. Solutions: (1) Use lookaside LB (e.g., Envoy in sidecar) instead of client-side LB. (2) Use xDS-based client-side LB (gRPC supports xDS natively) where the control plane assigns weights. (3) Use server-side LB (Kubernetes Service) for the canary traffic split.

**Q15: Compare Argo Rollouts vs. Flagger for canary deployments.**
A: Argo Rollouts: CRD replaces Deployment; tight integration with ArgoCD; AnalysisTemplates are expressive; supports blue-green + canary + experimentation. Flagger: works alongside existing Deployments (no CRD replacement); integrates with multiple mesh providers; simpler model. We chose Argo Rollouts for its richer analysis capabilities and ArgoCD ecosystem integration.

**Q16: How do you handle a canary that passes analysis but causes downstream service degradation?**
A: The analysis template should include downstream dependency metrics. Example: if order-svc canary causes payment-svc error rate to increase, the analysis query should detect it. We recommend including "dependency error rate" as a standard metric in every analysis template. If downstream monitoring isn't in the template, post-promotion monitoring catches it, and an emergency rollback is available.

---

## 16. References

- Argo Rollouts Documentation: https://argoproj.github.io/argo-rollouts/
- Flagger Documentation: https://flagger.app/
- Istio Traffic Management: https://istio.io/latest/docs/concepts/traffic-management/
- Netflix Kayenta (Automated Canary Analysis): https://github.com/spinnaker/kayenta
- Google SRE Book - Release Engineering: https://sre.google/sre-book/release-engineering/
- SMI TrafficSplit Spec: https://smi-spec.io/
- Prometheus PromQL: https://prometheus.io/docs/prometheus/latest/querying/basics/
- Argo Rollouts Analysis: https://argoproj.github.io/argo-rollouts/features/analysis/
- Envoy xDS Protocol: https://www.envoyproxy.io/docs/envoy/latest/api-docs/xds_protocol
