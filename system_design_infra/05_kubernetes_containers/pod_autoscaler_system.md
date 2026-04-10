# System Design: Pod Autoscaler System

> **Relevance to role:** Autoscaling is the mechanism that connects cost efficiency to workload demand. A cloud infrastructure platform engineer must understand how HPA, VPA, KEDA, and Cluster Autoscaler interact — including their feedback loops, stabilization windows, and edge cases — to design platforms that automatically right-size from the pod level to the node level without oscillation, over-provisioning, or outages.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Automatically scale pod replicas based on CPU, memory, and custom metrics (HPA) |
| FR-2 | Automatically adjust pod resource requests/limits based on historical usage (VPA) |
| FR-3 | Scale based on external event sources: queue depth, Kafka lag, HTTP rate, cron (KEDA) |
| FR-4 | Scale to zero for idle workloads (KEDA) |
| FR-5 | Scale cluster nodes based on unschedulable pods and underutilized nodes (Cluster Autoscaler) |
| FR-6 | Coordinate HPA and Cluster Autoscaler for smooth end-to-end scaling |
| FR-7 | Support predictive/scheduled scaling for known traffic patterns |
| FR-8 | Provide scaling observability: metrics, events, decisions |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Scale-out reaction time (metric breach → new pod running) | < 2 min |
| NFR-2 | Scale-in stabilization (prevent flapping) | 5 min window (HPA default) |
| NFR-3 | VPA recommendation accuracy | Within 20% of optimal |
| NFR-4 | Cluster scale-out time (new node ready) | < 5 min (cloud), < 20 min (bare-metal) |
| NFR-5 | Scale-to-zero → first-request latency | < 30 s (cold start) |
| NFR-6 | Autoscaler availability | 99.99% |
| NFR-7 | No service degradation during scale events | Zero dropped requests |

### Constraints & Assumptions
- Kubernetes 1.28+ with stable HPA v2 API.
- Metrics server deployed for CPU/memory metrics.
- Prometheus for custom and external metrics.
- KEDA 2.x for event-driven autoscaling.
- Cluster Autoscaler or Karpenter for node scaling.
- Applications handle graceful shutdown (SIGTERM, drain connections).

### Out of Scope
- Application-level auto-tuning (thread pool sizes, connection pool).
- Serverless platforms (Knative) — KEDA scale-to-zero covers this.
- Cost optimization beyond right-sizing (spot instances covered briefly).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|------------|--------|
| HPAs in cluster | 500 Deployments x 60% with HPA | 300 HPAs |
| VPAs in cluster | 200 Deployments with VPA recommendations | 200 VPAs |
| KEDA ScaledObjects | 100 event-driven workloads | 100 |
| Total autoscaled pods | 300 HPAs x 10 avg replicas | ~3,000 pods |
| Metric queries per HPA cycle | 300 HPAs x 1 metric query / 15s | 20 QPS to metrics API |
| Cluster Autoscaler evaluations | 1 evaluation / 10s | 0.1 QPS |
| Scale events per hour | 300 HPAs x 2 scale events/hour avg | ~600 events/hour |

### Latency Requirements

| Operation | Target | Component |
|-----------|--------|-----------|
| Metric collection → HPA evaluation | < 30 s | metrics-server scrape interval (15s) + HPA sync (15s) |
| HPA decision → pod Running | < 90 s | HPA patch + scheduler bind + image pull + container start |
| VPA recommendation → applied | Depends on updatePolicy | Off: manual. Auto/Recreate: pod restart required |
| KEDA trigger → pod Running | < 60 s | KEDA poll (30s default) + pod startup |
| Unschedulable pod → new node | < 5 min | CA detection (10s) + cloud API (1-3 min) + node bootstrap (1-2 min) |
| Node scale-in (underutilized) | > 10 min | CA stabilization window |

### Storage Estimates

| Component | Calculation | Result |
|-----------|------------|--------|
| HPA objects in etcd | 300 x 2 KB | ~600 KB |
| VPA objects in etcd | 200 x 3 KB (includes recommendations) | ~600 KB |
| KEDA ScaledObjects | 100 x 3 KB | ~300 KB |
| Metrics-server memory | 1 metric/pod x 30,000 pods x 100 B | ~3 MB |
| Prometheus (autoscaler metrics) | 1,000 time-series x 5 B x 86,400 samples/day | ~432 MB/day |

### Bandwidth Estimates

| Flow | Calculation | Result |
|------|------------|--------|
| Metrics-server → kubelet (scrape) | 5,000 nodes x 1 KB / 15s | ~333 KB/s |
| HPA → metrics API | 20 QPS x 1 KB | ~20 KB/s |
| KEDA → external sources | 100 ScaledObjects x 1 KB / 30s | ~3.3 KB/s |
| Cluster Autoscaler → cloud API | 1 evaluation/10s x 5 KB | ~0.5 KB/s |

---

## 3. High Level Architecture

```
  ┌──────────────────────────────────────────────────────────────┐
  │                    Metrics Sources                            │
  │                                                              │
  │  ┌──────────────┐  ┌───────────────┐  ┌──────────────────┐  │
  │  │ metrics-server│  │  Prometheus    │  │ External Sources │  │
  │  │ (CPU, memory) │  │  (custom)      │  │ (Kafka, SQS,    │  │
  │  │               │  │               │  │  Datadog, etc.)  │  │
  │  └──────┬───────┘  └───────┬───────┘  └────────┬─────────┘  │
  │         │                  │                    │             │
  │  ┌──────▼───────┐  ┌──────▼───────┐  ┌────────▼─────────┐  │
  │  │ Resource     │  │ Custom       │  │ External         │  │
  │  │ Metrics API  │  │ Metrics API  │  │ Metrics API      │  │
  │  │ (metrics.k8s)│  │ (prom-adapt) │  │ (KEDA)           │  │
  │  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
  └─────────┼─────────────────┼────────────────────┼─────────────┘
            │                 │                    │
  ┌─────────▼─────────────────▼────────────────────▼─────────────┐
  │                    Autoscalers                                │
  │                                                              │
  │  ┌────────────┐  ┌────────────┐  ┌─────────────────────┐   │
  │  │    HPA     │  │    VPA     │  │       KEDA          │   │
  │  │ (replicas) │  │ (resources)│  │ (event-driven       │   │
  │  │            │  │            │  │  scale + to-zero)    │   │
  │  └─────┬──────┘  └─────┬──────┘  └──────────┬──────────┘   │
  │        │               │                     │               │
  │        │       Patches Deployment/SS         │               │
  │        │        (replicas or resources)       │               │
  │        ▼               ▼                     ▼               │
  │  ┌──────────────────────────────────────────────────────┐   │
  │  │              Kubernetes Scheduler                     │   │
  │  │  (may find no feasible nodes → pending pods)         │   │
  │  └──────────────────────┬───────────────────────────────┘   │
  │                         │                                    │
  │  ┌──────────────────────▼───────────────────────────────┐   │
  │  │         Cluster Autoscaler / Karpenter               │   │
  │  │  - Detects unschedulable pods                        │   │
  │  │  - Provisions new nodes from node group/pool         │   │
  │  │  - Scales down underutilized nodes                   │   │
  │  └──────────────────────────────────────────────────────┘   │
  └──────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **metrics-server** | Collects CPU/memory from kubelets (Summary API); serves Resource Metrics API |
| **Prometheus Adapter** | Exposes Prometheus metrics as Custom Metrics API for HPA |
| **HPA Controller** | Periodically evaluates metrics; adjusts Deployment/StatefulSet replica count |
| **VPA** | Recommends (and optionally applies) optimal CPU/memory requests/limits |
| **KEDA** | Event-driven autoscaling; scales Deployments based on external triggers; supports scale-to-zero |
| **Cluster Autoscaler** | Provisions new nodes when pods are unschedulable; removes underutilized nodes |
| **Karpenter** | Alternative to CA; provisions right-sized nodes based on pending pod requirements |

### Data Flows

1. **HPA scale-out:** metrics-server scrapes kubelet → aggregates pod CPU/memory → HPA controller queries Resource Metrics API → calculates desired replicas: `ceil(currentReplicas * (currentMetricValue / targetMetricValue))` → patches Deployment replicas → scheduler places new pods → if no capacity → Cluster Autoscaler adds node.

2. **KEDA scale-from-zero:** KEDA polls Kafka topic lag → lag > 0 (scale trigger) → KEDA sets Deployment replicas from 0 to minReplicaCount → pods scheduled → process queue → lag drops to 0 → KEDA sets replicas back to 0 after cooldown.

3. **VPA recommendation:** VPA recommender analyzes 8 days of historical CPU/memory usage → calculates recommendations (target, lower bound, upper bound) → if updatePolicy=Auto → VPA admission controller intercepts pod creation → sets resource requests to recommended values → kubelet applies new cgroup settings.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# HorizontalPodAutoscaler (v2, stable since k8s 1.23)
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: web-hpa
  namespace: production
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: web-server
  minReplicas: 3
  maxReplicas: 100
  metrics:
    - type: Resource
      resource:
        name: cpu
        target:
          type: Utilization
          averageUtilization: 70     # Scale when avg CPU > 70%
    - type: Resource
      resource:
        name: memory
        target:
          type: Utilization
          averageUtilization: 80     # Scale when avg memory > 80%
    - type: Pods
      pods:
        metric:
          name: http_requests_per_second
        target:
          type: AverageValue
          averageValue: "1000"       # Scale when > 1000 RPS per pod
    - type: External
      external:
        metric:
          name: sqs_queue_depth
          selector:
            matchLabels:
              queue: orders
        target:
          type: Value
          value: "100"               # Scale when queue depth > 100
  behavior:
    scaleUp:
      stabilizationWindowSeconds: 0  # Scale up immediately
      policies:
        - type: Percent
          value: 100                  # Can double replicas in one step
          periodSeconds: 60
        - type: Pods
          value: 10                   # Or add max 10 pods per minute
      selectPolicy: Max              # Use whichever allows more pods
    scaleDown:
      stabilizationWindowSeconds: 300 # Wait 5 min before scaling down
      policies:
        - type: Percent
          value: 10                   # Scale down by max 10% per minute
          periodSeconds: 60
      selectPolicy: Min              # Use whichever removes fewer pods

status:
  currentReplicas: 5
  desiredReplicas: 8
  currentMetrics:
    - type: Resource
      resource:
        name: cpu
        current:
          averageUtilization: 82
          averageValue: "410m"
  conditions:
    - type: ScalingActive
      status: "True"
    - type: AbleToScale
      status: "True"

---
# VerticalPodAutoscaler
apiVersion: autoscaling.k8s.io/v1
kind: VerticalPodAutoscaler
metadata:
  name: web-vpa
  namespace: production
spec:
  targetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: web-server
  updatePolicy:
    updateMode: "Off"               # Recommendation only (safest)
    # "Initial"  — apply to new pods only
    # "Recreate" — evict and recreate pods to apply
    # "Auto"     — same as Recreate (InPlace coming)
  resourcePolicy:
    containerPolicies:
      - containerName: web
        minAllowed:
          cpu: "100m"
          memory: "128Mi"
        maxAllowed:
          cpu: "4"
          memory: "8Gi"
        controlledResources: ["cpu", "memory"]
        controlledValues: RequestsAndLimits

status:
  recommendation:
    containerRecommendations:
      - containerName: web
        target:
          cpu: "350m"
          memory: "512Mi"
        lowerBound:
          cpu: "250m"
          memory: "384Mi"
        upperBound:
          cpu: "500m"
          memory: "768Mi"
        uncappedTarget:
          cpu: "350m"
          memory: "512Mi"

---
# KEDA ScaledObject
apiVersion: keda.sh/v1alpha1
kind: ScaledObject
metadata:
  name: order-processor
  namespace: production
spec:
  scaleTargetRef:
    name: order-processor
  pollingInterval: 30              # Check trigger every 30s
  cooldownPeriod: 300              # Wait 5 min before scaling to zero
  idleReplicaCount: 0              # Scale to zero when idle
  minReplicaCount: 0               # Minimum replicas (0 = scale-to-zero)
  maxReplicaCount: 50
  fallback:
    failureThreshold: 3
    replicas: 5                     # If trigger fails, default to 5 replicas
  triggers:
    - type: kafka
      metadata:
        bootstrapServers: kafka:9092
        consumerGroup: order-processors
        topic: orders
        lagThreshold: "100"         # Scale when lag > 100 per partition
    - type: prometheus
      metadata:
        serverAddress: http://prometheus:9090
        metricName: http_requests_total
        query: sum(rate(http_requests_total{service="orders"}[2m]))
        threshold: "500"            # Scale when > 500 RPS
    - type: cron
      metadata:
        timezone: America/New_York
        start: 0 8 * * *           # Scale up at 8 AM
        end: 0 20 * * *            # Scale down at 8 PM
        desiredReplicas: "10"

---
# Cluster Autoscaler configuration (via Deployment args)
# --node-group-auto-discovery=asg:tag=k8s.io/cluster-autoscaler/enabled=true
# --balance-similar-node-groups=true
# --skip-nodes-with-system-pods=false
# --scale-down-utilization-threshold=0.5
# --scale-down-unneeded-time=10m
# --scale-down-delay-after-add=10m
# --max-node-provision-time=5m
# --expendable-pods-priority-cutoff=-10
```

### Database Selection

| Data Type | Storage | Justification |
|-----------|---------|---------------|
| HPA/VPA/ScaledObject specs | etcd (via API server) | Native k8s CRDs |
| VPA historical data | In-memory (VPA recommender) + Prometheus | 8-day sliding window for recommendations |
| Metrics (CPU, memory) | metrics-server (in-memory, last sample only) | Lightweight; HPA only needs current value |
| Custom metrics | Prometheus (time-series) | 15-day retention for trend analysis |
| External metrics | KEDA external sources | Real-time from Kafka, SQS, etc. |

### Indexing Strategy

| Index | Purpose |
|-------|---------|
| HPA → scaleTargetRef | Link HPA to its Deployment/StatefulSet |
| VPA → targetRef | Link VPA to its target workload |
| KEDA ScaledObject → scaleTargetRef | Link KEDA to its target |
| Pod metrics by namespace/pod | Aggregate metrics for HPA evaluation |

---

## 5. API Design

### REST/gRPC/kubectl Endpoints

```bash
# HPA management
kubectl autoscale deployment web-server --cpu-percent=70 --min=3 --max=100
kubectl get hpa -n production
kubectl describe hpa web-hpa -n production
kubectl get hpa web-hpa -o yaml   # Shows current metrics + conditions

# VPA management
kubectl get vpa -n production
kubectl describe vpa web-vpa -n production
# VPA recommendation is in .status.recommendation.containerRecommendations

# KEDA management
kubectl get scaledobjects -n production
kubectl describe scaledobject order-processor -n production
kubectl get scaledjobs -n production   # For job-based scaling

# Cluster Autoscaler
kubectl -n kube-system logs -l app=cluster-autoscaler --follow
kubectl get configmap cluster-autoscaler-status -n kube-system -o yaml

# Metrics debugging
kubectl top pods -n production --sort-by=cpu
kubectl top nodes
kubectl get --raw "/apis/metrics.k8s.io/v1beta1/namespaces/production/pods" | jq .
kubectl get --raw "/apis/custom.metrics.k8s.io/v1beta2/namespaces/production/pods/*/http_requests_per_second"
```

### Metrics API

```
Three metrics APIs consumed by HPA:

1. Resource Metrics API (metrics.k8s.io/v1beta1)
   Source: metrics-server
   Metrics: cpu, memory (per pod, per node)
   GET /apis/metrics.k8s.io/v1beta1/namespaces/{ns}/pods/{pod}
   
2. Custom Metrics API (custom.metrics.k8s.io/v1beta2)
   Source: Prometheus Adapter, Datadog Adapter, etc.
   Metrics: any Prometheus metric mapped to k8s object
   GET /apis/custom.metrics.k8s.io/v1beta2/namespaces/{ns}/pods/*/{metric}
   
3. External Metrics API (external.metrics.k8s.io/v1beta1)
   Source: KEDA, stackdriver-adapter, etc.
   Metrics: any external metric not tied to a k8s object
   GET /apis/external.metrics.k8s.io/v1beta1/namespaces/{ns}/{metric}
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: HPA — Horizontal Pod Autoscaler

**Why it's hard:**
HPA must react quickly to load spikes (scale up in seconds) while avoiding oscillation (flapping between high and low replica counts). The feedback loop between metrics collection, evaluation, and pod startup introduces delay that can cause overshooting. Multiple metrics can conflict (CPU says scale up, memory says scale down). Stabilization windows must balance responsiveness with stability.

**Approaches Compared:**

| Approach | Responsiveness | Stability | Complexity | Accuracy |
|----------|---------------|----------|-----------|---------|
| HPA v2 (k8s native) | Good (15-30s eval) | Good (stabilization windows) | Medium | Good for steady-state |
| KEDA (external triggers) | Better (30s polling) | Good (cooldown) | Medium | Excellent for queue-based |
| Custom controller | Tunable | Tunable | High | Custom |
| Predictive (ML-based) | Best (proactive) | Best | Very high | Excellent with training data |

**Selected: HPA v2 with behavior configuration + KEDA for external triggers**

**HPA Algorithm:**

```
desiredReplicas = ceil(currentReplicas * (currentMetricValue / targetMetricValue))

Example:
  Current: 5 replicas, avg CPU = 85%
  Target: 70%
  Desired = ceil(5 * (85 / 70)) = ceil(5 * 1.214) = ceil(6.07) = 7 replicas

Multiple metrics:
  - HPA evaluates each metric independently
  - Takes the MAXIMUM desired replicas across all metrics
  - Ensures all metric targets are met
  
  Example:
    CPU metric → 7 replicas needed
    Memory metric → 5 replicas needed
    RPS metric → 9 replicas needed
    Decision: scale to 9 replicas

Tolerance band:
  HPA has a tolerance of 10% (default, --horizontal-pod-autoscaler-tolerance=0.1)
  If currentMetricValue is within 10% of target, no scaling action
  This prevents micro-adjustments

Scale-up behavior:
  stabilizationWindowSeconds: 0    # No delay (default)
  policies:
    - type: Percent, value: 100    # Can double replicas per period
    - type: Pods, value: 4         # Or add max 4 pods per period
  selectPolicy: Max                # Use whichever allows more scaling

Scale-down behavior:
  stabilizationWindowSeconds: 300  # 5 min window (default)
  - HPA calculates desired replicas every 15s
  - Stores all calculations in a 5-min window
  - Uses the MAXIMUM across the window (most conservative)
  - This prevents scaling down during brief traffic dips
  
  policies:
    - type: Percent, value: 10     # Scale down max 10% per period
  selectPolicy: Min                # Use whichever removes fewer pods
```

**HPA + Container Resource Metrics (alpha in v1.30):**

```yaml
# Scale based on individual container metrics (not pod average)
metrics:
  - type: ContainerResource
    containerResource:
      name: cpu
      container: web           # Only look at the 'web' container
      target:
        type: Utilization
        averageUtilization: 70
# Useful when pods have sidecars that skew the pod-level average
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| metrics-server down | HPA cannot evaluate; no scaling decisions | HA metrics-server (2 replicas); HPA condition ScalingActive=False; fallback to last known state |
| Metric lag (stale data) | HPA makes decisions on old data; may overshoot | Monitor metric freshness; alert if metric age > 2 min |
| Flapping (oscillation) | Pods constantly created/destroyed; wasted resources | Tune stabilization windows; increase tolerance; set appropriate scale-down rate |
| maxReplicas too low | Traffic spike not absorbed; service degradation | Monitor HPA condition AbleToScale; alert when desiredReplicas == maxReplicas |
| minReplicas too low | Risk of complete outage during scale-from-min | Set minReplicas ≥ 2 for HA; use PDB |
| Prometheus adapter down | Custom metrics unavailable; HPA stuck | HA Prometheus adapter; HPA falls back to resource metrics |

**Interviewer Q&As:**

**Q1: How does HPA handle the delay between scaling decision and new pod being ready?**
A: HPA does not account for pod startup time directly. It evaluates every 15 seconds (configurable via `--horizontal-pod-autoscaler-sync-period`). If the metric is still above target after adding pods (because they haven't started yet), HPA will try to add more pods in the next cycle. To prevent overshooting: (1) The algorithm considers only Ready pods in the denominator. (2) Pods that are starting (not yet Ready) are excluded from the metric calculation. (3) The `scaleUp.stabilizationWindowSeconds` limits total scaling within a window. Still, rapid spikes can cause overshooting — pre-scaling (KEDA cron trigger) or pod warm pools help.

**Q2: Can you use HPA and VPA together?**
A: Not on the same metric for the same Deployment. If HPA scales on CPU and VPA adjusts CPU requests, they will fight: VPA increases requests → HPA sees higher utilization (because requests are higher) → HPA scales out → VPA sees lower utilization → VPA decreases requests → loop. Solutions: (1) HPA scales on custom metrics (RPS), VPA adjusts CPU/memory. (2) Use VPA in recommendation-only mode (updateMode: Off) and apply recommendations manually. (3) Multidimensional Pod Autoscaler (Google's approach): combines horizontal and vertical scaling in one controller. (4) KEDA's integration with VPA: KEDA handles scaling, VPA handles right-sizing.

**Q3: How do you scale based on a metric that is not per-pod (e.g., total queue depth)?**
A: Use HPA with `type: External` or KEDA. For HPA external metrics: (1) Deploy a metrics adapter (Prometheus Adapter or KEDA's metrics server). (2) Configure the adapter to expose the external metric. (3) HPA uses `target.type: Value` (absolute, not per-pod) or `target.type: AverageValue` (divided by current replica count). Example: queue depth = 1000, target = 100 per pod → desired = ceil(1000/100) = 10 replicas. KEDA is generally easier for external metrics because it handles the adapter and trigger configuration in a single ScaledObject.

**Q4: How does HPA handle multiple conflicting metrics?**
A: HPA evaluates each metric independently and takes the maximum desired replicas. This ensures all metrics are satisfied: if CPU says 5 replicas and RPS says 10 replicas, HPA scales to 10. For scale-down, the maximum from the stabilization window is used, which is the most conservative approach. If you need weighted metrics or more complex logic, use KEDA's composite triggers or a custom controller.

**Q5: What happens when HPA and Cluster Autoscaler interact during a scale-out event?**
A: Sequence: (1) HPA detects high CPU → sets desired replicas to 20 (from 10). (2) Scheduler tries to place 10 new pods but only 3 nodes have capacity → 7 pods are Pending. (3) Cluster Autoscaler detects 7 unschedulable pods → calculates required nodes → provisions from node group (ASG, MIG). (4) New nodes join cluster (3-5 min). (5) Scheduler places Pending pods on new nodes. (6) Pods become Ready → HPA re-evaluates and may stabilize. Total time: ~3-7 minutes. Optimization: over-provision nodes slightly (Cluster Autoscaler `--scale-down-utilization-threshold=0.5` leaves 50% headroom).

**Q6: How do you prevent HPA from scaling down during a deployment rollout?**
A: During a rolling update, pods are being replaced — metrics may temporarily drop (new pods not yet serving). If HPA scales down based on these momentarily low metrics, the rollout stalls. Solutions: (1) HPA excludes unready pods from metric calculation. (2) Set `scaleDown.stabilizationWindowSeconds: 300` — HPA waits 5 minutes before scaling down, by which time the rollout is complete. (3) Use `kubectl rollout pause` during manual maintenance. (4) HPA behavior policies limit scale-down rate (e.g., max 10% per minute).

---

### Deep Dive 2: VPA — Vertical Pod Autoscaler

**Why it's hard:**
VPA must recommend the right resource requests and limits for containers based on historical usage. Too low → OOM kills, CPU throttling. Too high → wasted resources, poor scheduling (node packing is less efficient). The recommendation must account for: spikes (95th percentile vs. average), daily patterns, growth trends, and container-specific behavior (JVM heap, Python GC).

**Approaches Compared:**

| Approach | Accuracy | Disruption | Complexity | Cost Savings |
|----------|---------|-----------|-----------|-------------|
| VPA (k8s native) | Good | High (pod restart for apply) | Medium | 20-50% typical |
| In-place resize (v1.33 alpha) | Good | None (live update) | Medium | 20-50% |
| Manual right-sizing | Low (human estimation) | Low (planned changes) | High (human effort) | Varies |
| Goldilocks (VPA in recommend mode + dashboard) | Good (VPA-based) | None (recommend only) | Low | 20-50% |
| Cloud provider autoscaling (GKE Autopilot) | Good | Low (managed) | Low | 20-50% |

**Selected: VPA in recommendation mode (Off) with Goldilocks dashboard for visibility, applied via CI/CD**

**Justification:** VPA in Auto/Recreate mode causes pod evictions, which is disruptive for production services. Instead, run VPA in Off mode, surface recommendations via Goldilocks, and apply changes through normal deployment pipelines.

**VPA Architecture:**

```
                    ┌──────────────────┐
                    │  VPA Recommender  │
                    │                  │
                    │  - Reads pod     │
                    │    resource usage│
                    │    from metrics  │
                    │    server        │
                    │                  │
                    │  - Maintains     │
                    │    histogram of  │
                    │    usage over    │
                    │    8 days        │
                    │                  │
                    │  - Calculates:   │
                    │    target (p90)  │
                    │    lowerBound    │
                    │    upperBound    │
                    └────────┬─────────┘
                             │ writes .status.recommendation
                             ▼
                    ┌──────────────────┐
                    │   VPA Object     │
                    │  (in etcd)       │
                    └────────┬─────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
     ┌────────▼──────┐  ┌───▼──────┐  ┌───▼─────────┐
     │ VPA Updater    │  │Admission │  │ Goldilocks  │
     │ (if Auto mode) │  │ Webhook  │  │ Dashboard   │
     │                │  │          │  │             │
     │ Evicts pods    │  │ Mutates  │  │ Shows reco  │
     │ with outdated  │  │ pod spec │  │ per deploy  │
     │ requests       │  │ on create│  │ for humans  │
     └───────────────┘  └──────────┘  └─────────────┘

VPA Recommendation Algorithm:
  1. Collect container resource usage samples (CPU, memory)
     - From metrics-server (current) + Prometheus (historical)
     - 8-day weighted sliding window (recent data weighted more)
  
  2. Build histogram of usage:
     - CPU: weighted histogram of CPU usage samples
     - Memory: OOM-aware histogram (bumps up after OOM events)
  
  3. Calculate recommendations:
     - target:     ~90th percentile of usage (reasonable for most workloads)
     - lowerBound: ~50th percentile (minimum viable)
     - upperBound: ~95th percentile + 15% headroom
     - uncappedTarget: target before applying min/max from resourcePolicy
  
  4. Confidence:
     - First 24 hours: low confidence → conservative (higher) recommendations
     - After 7 days: high confidence → tighter recommendations
     - After OOM: immediately bumps memory recommendation up
```

**In-Place Pod Vertical Scaling (v1.33 Alpha):**

```yaml
# Future: resize container resources without restart
apiVersion: v1
kind: Pod
spec:
  containers:
    - name: web
      resources:
        requests:
          cpu: "250m"
          memory: "256Mi"
      resizePolicy:
        - resourceName: cpu
          restartPolicy: NotRequired   # Can resize without restart
        - resourceName: memory
          restartPolicy: RestartContainer  # Memory resize requires restart
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| VPA recommends too low | OOM kills, CPU throttling | Set minAllowed in resourcePolicy; VPA learns from OOM events and adjusts up |
| VPA recommends too high | Wasted resources, poor scheduling | Set maxAllowed in resourcePolicy; review recommendations before applying |
| VPA Updater evicts pods too aggressively | Service disruption | Use PDB; VPA respects PDB during eviction. Or use updateMode: Off |
| Metric data insufficient (< 24h) | Inaccurate recommendations | VPA shows low confidence; use conservative defaults until data accumulates |
| VPA + HPA conflict | Oscillation (described above) | Separate concerns: VPA for resources, HPA/KEDA for replicas with non-resource metrics |

**Interviewer Q&As:**

**Q1: Why does VPA use 90th percentile instead of average for recommendations?**
A: Average would under-provision for spiky workloads — 50% of the time, the container would need more than the average. The 90th percentile ensures the container has sufficient resources 90% of the time. The remaining 10% is handled by burst capacity (limit > request). For memory: under-provisioning means OOM kill (catastrophic), so memory recommendations are even more conservative. For CPU: under-provisioning means throttling (degraded, not catastrophic).

**Q2: How does VPA handle Java applications with fixed heap sizes?**
A: Java (JVM) applications pre-allocate heap memory at startup (`-Xmx`). VPA sees constant memory usage equal to Xmx and recommends slightly above it — which is correct. However, VPA cannot optimize the heap size itself. Best practice: (1) Set container memory request = Xmx + ~20% overhead (for metaspace, stacks, native memory). (2) Use VPA for CPU only (memory is determined by JVM config). (3) For optimizing Xmx: use JVM ergonomics with `UseContainerSupport` — JVM reads cgroup memory limit and sets heap accordingly.

**Q3: How do you apply VPA recommendations without downtime?**
A: (1) VPA in Off mode + Goldilocks dashboard: humans review and apply via deployment pipeline. (2) VPA in Initial mode: recommendations applied only to new pods (no eviction of existing pods). New pods from rolling updates get optimal resources. (3) In-place resize (alpha): resize without pod restart (requires feature gate InPlacePodVerticalScaling). (4) For zero downtime: apply VPA recommendations as part of normal deployment cycles — update resource requests in the Deployment spec, let rolling update handle the transition.

**Q4: What is the difference between VPA target, lowerBound, upperBound, and uncappedTarget?**
A: (1) **target**: the recommended value — what VPA suggests setting requests to. Based on ~90th percentile of usage. (2) **lowerBound**: the minimum viable value — below this, the container is likely to be resource-starved. Based on ~50th percentile. (3) **upperBound**: the maximum useful value — above this, resources are wasted. Based on ~95th percentile + headroom. (4) **uncappedTarget**: the target before applying minAllowed/maxAllowed from resourcePolicy. If uncappedTarget != target, it means the policy is constraining the recommendation. Useful for seeing what VPA would recommend without constraints.

---

### Deep Dive 3: KEDA — Event-Driven Autoscaling

**Why it's hard:**
KEDA extends HPA to scale based on external event sources (queue depth, stream lag, HTTP rate, cron schedules). The challenge is: (1) polling external sources reliably without overwhelming them, (2) mapping event-source-specific semantics to replica counts, (3) handling scale-to-zero (no pods to receive the first event), and (4) coordinating with the Kubernetes scheduler and Cluster Autoscaler.

**Implementation Detail:**

```
KEDA Architecture:

  ┌─────────────────────────────────────────────────┐
  │                KEDA Operator                     │
  │                                                 │
  │  ┌─────────────────────────────────────────┐   │
  │  │          ScaledObject Controller         │   │
  │  │                                         │   │
  │  │  For each ScaledObject:                 │   │
  │  │  1. Poll triggers (external sources)    │   │
  │  │  2. Calculate desired replicas          │   │
  │  │  3. If replicas == 0: manage Deployment │   │
  │  │     scale directly (bypass HPA)         │   │
  │  │  4. If replicas > 0: create HPA         │   │
  │  │     and let HPA manage scaling          │   │
  │  └─────────────────────────────────────────┘   │
  │                                                 │
  │  ┌─────────────────────────────────────────┐   │
  │  │      KEDA Metrics Server                 │   │
  │  │                                         │   │
  │  │  Exposes external metrics as            │   │
  │  │  External Metrics API for HPA           │   │
  │  │  consumption                            │   │
  │  └─────────────────────────────────────────┘   │
  │                                                 │
  │  ┌─────────────────────────────────────────┐   │
  │  │      Trigger Scalers (60+ built-in)     │   │
  │  │                                         │   │
  │  │  Kafka, AWS SQS, RabbitMQ, Redis,       │   │
  │  │  Prometheus, Datadog, Azure Event Hub,   │   │
  │  │  PostgreSQL, MySQL, Elasticsearch,       │   │
  │  │  HTTP, Cron, CPU/Memory, gRPC, NATS...  │   │
  │  └─────────────────────────────────────────┘   │
  └─────────────────────────────────────────────────┘

Scale-to-Zero Flow:
  1. All triggers report 0 activity (queue empty, no HTTP requests)
  2. KEDA waits for cooldownPeriod (default 300s)
  3. KEDA sets Deployment replicas = 0 (direct patch, no HPA)
  4. HPA is paused/deleted (HPA min is 1, cannot scale to 0)
  5. Triggers continue polling even at 0 replicas
  6. Activity detected (message in queue, HTTP request)
  7. KEDA sets Deployment replicas = minReplicaCount (≥ 1)
  8. HPA is recreated to manage further scaling
  
  Cold start latency:
    KEDA poll interval (30s) + pod scheduling (5s) + image pull + container start
    Typical: 30-60s for warm image, 60-120s for cold pull
    
  Mitigation for cold start:
    - Set pollingInterval: 5 (faster detection, more API calls)
    - Pre-pull images on all nodes (DaemonSet)
    - Use minReplicaCount: 1 (always keep one warm pod)
    - KEDA HTTP add-on: proxy that buffers requests during cold start

KEDA Kafka Scaler Deep Dive:
  Trigger formula:
    desiredReplicas = ceil(totalLag / lagThreshold)
    
  Where:
    totalLag = sum(lag per partition across all partitions in the topic)
    lagThreshold = configured value (e.g., 100 messages per replica)
    
  Example:
    Topic: orders (10 partitions)
    Consumer group: order-processors
    Current lag per partition: [50, 100, 200, 0, 30, 80, 150, 10, 90, 60]
    Total lag: 770
    lagThreshold: 100
    desiredReplicas = ceil(770 / 100) = 8
    
  maxReplicaCount constraint: min(8, maxReplicaCount)
  Note: Kafka consumer max parallelism = number of partitions
    So maxReplicaCount should ≤ partition count (10 in this case)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| External source unreachable (Kafka down) | KEDA cannot poll; scaling frozen | `fallback.replicas: 5` — KEDA uses fallback replicas after failureThreshold consecutive failures |
| KEDA operator crash | No scaling decisions; HPA continues if already created | HA deployment (2 replicas with leader election) |
| Metric lag (stale Kafka consumer offsets) | KEDA scales based on stale data | Monitor consumer group health; verify offset reporting is current |
| Scale-to-zero during deployment | New version not deployed if replicas=0 | KEDA pauses scale-to-zero during Deployment updates |
| Too many triggers polling external sources | Overwhelm external system | Increase pollingInterval; batch queries; use push-based triggers |
| Cold start too slow | First messages processed with high latency | Keep minReplicaCount=1 for critical queues; use KEDA HTTP interceptor |

**Interviewer Q&As:**

**Q1: How does KEDA handle the transition from 0 to 1 replicas (scale-from-zero)?**
A: KEDA manages this directly (not via HPA, since HPA requires minReplicas ≥ 1). The ScaledObject controller polls triggers even when replicas = 0. When any trigger returns isActive=true (e.g., messages in queue), KEDA patches the Deployment to set replicas = minReplicaCount. After the pod starts, KEDA creates an HPA for further scaling. The cold start delay = pollingInterval + pod startup time. For HTTP workloads, KEDA's HTTP add-on can buffer incoming requests and forward them once the pod is ready.

**Q2: How do you scale Kafka consumers correctly with KEDA?**
A: Key considerations: (1) Max parallelism = number of partitions (Kafka assigns at most one consumer per partition within a consumer group). Set maxReplicaCount ≤ partition count. (2) Use consumer group lag as the trigger (not message rate) — lag directly measures backlog. (3) lagThreshold should be set based on expected processing time: if each message takes 100ms, lagThreshold=100 means each replica handles ~10s of backlog. (4) Consider partition rebalancing overhead: scaling too aggressively causes frequent rebalances (30-60s each). Set pollingInterval and cooldownPeriod to limit scale frequency.

**Q3: How does KEDA compare to knative serving for scale-to-zero?**
A: KEDA is workload-agnostic — it scales any Deployment based on external triggers. Knative Serving is specifically for HTTP workloads: it uses an activator component that buffers requests during scale-from-zero, providing lower cold-start latency. KEDA's HTTP add-on provides similar functionality but is less mature. Use KEDA for: queue processors, cron jobs, mixed workloads. Use Knative for: HTTP services with bursty traffic that need scale-to-zero.

**Q4: How do you combine multiple KEDA triggers?**
A: Multiple triggers in a ScaledObject are evaluated independently. KEDA takes the maximum desired replicas across all triggers (same as HPA with multiple metrics). Example: Kafka trigger says 5 replicas (based on lag), cron trigger says 10 replicas (business hours) → KEDA scales to 10. This allows combining reactive (queue depth) and predictive (cron schedule) scaling.

---

### Deep Dive 4: Cluster Autoscaler and Karpenter

**Why it's hard:**
Node-level autoscaling must handle: (1) heterogeneous workload requirements (different instance types, GPUs, local storage), (2) bin-packing efficiency vs. speed, (3) spot/preemptible instance management, (4) safe scale-down (drain before termination, respect PDBs), and (5) interaction with pod-level autoscalers.

**Approaches Compared:**

| Feature | Cluster Autoscaler (CA) | Karpenter |
|---------|------------------------|-----------|
| Node group model | Pre-defined node groups (ASGs) | Just-in-time provisioning (no groups) |
| Instance selection | Fixed per node group | Selects optimal instance from fleet |
| Scale-up speed | 1-3 min (ASG scaling) | 30-60s (direct API call) |
| Bin-packing | Limited (picks first feasible group) | Excellent (simulates scheduling) |
| Spot management | Via mixed instance ASGs | Native with consolidation |
| Multi-arch | Limited | Native (select ARM or x86 per pod) |
| Bare-metal | Not supported | Not supported (cloud-only) |

**Selected: Cluster Autoscaler for multi-cloud/bare-metal; Karpenter for AWS-only**

**Cluster Autoscaler Algorithm:**

```
Scale-Up:
  1. Scan for unschedulable pods (pending pods with FailedScheduling event)
  2. For each node group, simulate: "If I add a node from this group,
     can any of the pending pods be scheduled?"
  3. Select the node group that schedules the most pending pods
  4. Call cloud API to increase desired count of the node group
  5. Wait for node to join and become Ready (max 5 min, configurable)
  
  Optimizations:
  - Expander strategies: random, most-pods, least-waste, price, priority
  - least-waste: choose the node group that leaves the least unused resources
  - price: choose the cheapest node group that fits

Scale-Down:
  1. Every 10s, evaluate each node for underutilization
  2. A node is underutilized if:
     - CPU requests < 50% of allocatable (--scale-down-utilization-threshold)
     - Memory requests < 50% of allocatable
     - All pods on the node can be rescheduled elsewhere
  3. Node must be underutilized for 10 min (--scale-down-unneeded-time)
  4. Drain the node (respect PDB, pod disruption budget)
  5. Call cloud API to terminate the instance
  
  Safety:
  - Never scale down nodes with:
    - Pods with local storage (unless --skip-nodes-with-local-storage=false)
    - Pods with restrictive PDB (can't be evicted)
    - Non-replicated pods (no controller, e.g., naked pods)
    - System pods (kube-system, unless explicitly allowed)
  - --scale-down-delay-after-add=10m (don't scale down a node just added)
  - --max-graceful-termination-sec=600 (max drain time)
```

**Karpenter Algorithm (AWS-specific):**

```
1. Detect unschedulable pods
2. Group pods by scheduling constraints (node affinity, tolerations, topology)
3. For each group, select optimal instance type:
   - Consider all instance types in the region
   - Filter by: pod resource requirements, architecture, GPU, zones
   - Score by: price, availability, remaining capacity after scheduling
4. Launch instance directly via EC2 Fleet API (not ASG)
5. Apply node configuration (labels, taints, kubelet config)
6. Node joins cluster; pods scheduled

Consolidation (replaces CA scale-down):
  - Continuously evaluates: can existing pods fit on fewer/cheaper nodes?
  - If yes: cordons old node, drains pods (with PDB), terminates
  - Replaces expensive instances with cheaper ones
  - Handles spot interruption: reschedules pods before termination
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Cloud API rate limiting | Node provisioning delayed | Exponential backoff; request quota increase |
| Instance type unavailable | Pods stuck pending | Multiple instance types in node group; Karpenter auto-selects alternatives |
| Node takes > 5 min to boot | Pending pods delayed | Optimize AMI; pre-bake images; use Karpenter's faster provisioning |
| Scale-down drains production pods | Service disruption | PDB ensures minimum availability; test PDBs before enabling CA scale-down |
| Spot interruption | Node terminated with 2 min warning | Use mixed on-demand/spot; Karpenter handles interruption gracefully |
| CA and HPA race condition | CA adds nodes before HPA stabilizes | Acceptable: CA removes underutilized nodes after HPA stabilizes |

**Interviewer Q&As:**

**Q1: How do Cluster Autoscaler and HPA coordinate during a traffic spike?**
A: They don't coordinate directly — they operate at different layers: (1) HPA detects high CPU → increases desired replicas. (2) Scheduler tries to place new pods → some are Pending (no capacity). (3) CA detects Pending pods → provisions new nodes. (4) New nodes join → Pending pods scheduled → pods become Ready. (5) HPA re-evaluates metrics → may stabilize or continue scaling. The key insight: CA is reactive (triggered by Pending pods), not proactive. To reduce the gap: (1) Over-provision nodes slightly (lower scale-down-utilization-threshold). (2) Use Karpenter for faster provisioning. (3) Use KEDA cron trigger for predictable traffic patterns.

**Q2: How do you handle node scaling on bare-metal (no cloud API)?**
A: Cluster Autoscaler doesn't support bare-metal. Options: (1) **Pre-provisioned pool**: maintain a pool of warm bare-metal nodes registered as k8s nodes but tainted (NoSchedule). When CA detects Pending pods, a custom controller removes the taint on a warm node. (2) **Metal3 + custom CA**: extend CA with a Metal3 provider that provisions BareMetalHosts. (3) **Virtual kubelet**: register serverless capacity (Fargate, ACI) as virtual nodes for overflow. (4) **Over-provisioning**: run the cluster with 20-30% headroom; replenish spare nodes from hardware pool periodically.

**Q3: How do you optimize node costs with mixed on-demand and spot instances?**
A: (1) Run control plane and stateful workloads on on-demand instances. (2) Run stateless workloads on spot instances (60-90% cost savings). (3) Karpenter: specify `nodePool.spec.requirements` with `capacityType: [on-demand, spot]` and set priority to prefer spot. (4) PodDisruptionBudgets ensure graceful handling of spot interruptions. (5) Topology spread: spread replicas across on-demand and spot so some replicas survive interruption. (6) CA: use mixed instance ASGs with on-demand base + spot surplus.

---

## 7. Scheduling & Resource Management

### Autoscaler Interaction Map

```
                    Traffic Increase
                          │
                    ┌─────▼─────┐
                    │    HPA    │  Increases Deployment replicas
                    └─────┬─────┘
                          │
                    ┌─────▼──────┐
                    │ Scheduler   │  Tries to place new pods
                    └─────┬──────┘
                          │
              ┌───────────┼───────────┐
              │ Capacity? │           │ No capacity
              │           ▼           │
              │     Pods Running      │
              │                       ▼
              │               ┌───────────────┐
              │               │ Cluster Auto  │  Provisions new nodes
              │               │ scaler (CA)   │
              │               └───────┬───────┘
              │                       │
              │               ┌───────▼───────┐
              │               │  New nodes    │
              │               │  join cluster │
              │               └───────┬───────┘
              │                       │
              │               ┌───────▼───────┐
              │               │ Pending pods  │
              │               │ now scheduled │
              │               └───────────────┘
              │
              ▼
        Traffic Decrease
              │
        ┌─────▼─────┐
        │    HPA    │  Decreases replicas (after stabilization)
        └─────┬─────┘
              │
        ┌─────▼──────┐
        │  Fewer pods │  Nodes become underutilized
        └─────┬──────┘
              │
        ┌─────▼──────────┐
        │ CA scale-down   │  Detects underutilized nodes
        │ (after 10 min)  │  Drains and terminates
        └─────────────────┘
```

### Pod Priority and Preemption with Autoscaling

```yaml
# Over-provisioning with balloon pods:
apiVersion: scheduling.k8s.io/v1
kind: PriorityClass
metadata:
  name: overprovisioning
value: -1    # Lowest priority
globalDefault: false
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: overprovisioning
spec:
  replicas: 5
  template:
    spec:
      priorityClassName: overprovisioning
      containers:
        - name: pause
          image: registry.k8s.io/pause:3.9
          resources:
            requests:
              cpu: "2"
              memory: "4Gi"
# These "balloon" pods reserve capacity on nodes.
# When real pods arrive, they preempt the balloon pods.
# Balloon pods become Pending → CA provisions new nodes.
# Net effect: real pods start instantly (use balloon's capacity),
# while new nodes are being provisioned in the background.
```

---

## 8. Scaling Strategy

### End-to-End Autoscaling Architecture

| Layer | Component | Scales What | Trigger |
|-------|-----------|------------|---------|
| Application | HPA / KEDA | Pod replicas | CPU, RPS, queue depth, custom |
| Container | VPA | Resource requests/limits | Historical usage |
| Infrastructure | Cluster Autoscaler / Karpenter | Nodes | Unschedulable pods, underutilization |
| Predictive | KEDA Cron / custom | Pre-scale before demand | Time-based patterns |

### Interviewer Q&As

**Q1: Design an autoscaling strategy for a web application with predictable daily traffic patterns and occasional unexpected spikes.**
A: Layer 1 - Predictive: KEDA cron trigger scales to expected baseline at 8 AM, scales down at 10 PM. Layer 2 - Reactive: HPA with CPU target at 70%, custom metric on RPS. Scale-up: aggressive (double replicas per minute), scale-down: conservative (10% per minute, 5 min stabilization). Layer 3 - Buffer: over-provisioning balloon pods reserve 20% extra capacity for instant spike absorption. Layer 4 - Infrastructure: Cluster Autoscaler with mixed instance types, scale-down-unneeded-time=10m. Layer 5 - Right-sizing: VPA in Off mode with Goldilocks dashboard; apply recommendations quarterly.

**Q2: How do you prevent autoscaling from causing cascading failures?**
A: (1) **HPA maxReplicas**: cap total replicas to prevent overwhelming downstream dependencies. (2) **Rate limiting on scale-up**: `scaleUp.policies` limits scaling speed. (3) **Circuit breakers**: application-level circuit breakers prevent cascading to downstream services. (4) **Readiness probes**: new pods only receive traffic when ready (don't overwhelm a half-started pod). (5) **Resource quotas**: namespace-level limits prevent runaway scaling from exhausting cluster resources. (6) **Alert on rapid scaling**: if HPA scales by > 50% in 5 minutes, alert on-call — may indicate an anomaly, not just traffic.

**Q3: How do you autoscale a stateful workload (e.g., database)?**
A: Stateful workloads are harder to scale: (1) **StatefulSet scaling**: HPA can scale StatefulSets, adding replicas with stable identities. But the new replica needs: data sync, replication setup, warmup. (2) **Read replicas**: easier to auto-add read replicas. Write primary usually needs vertical scaling. (3) **VPA for vertical**: adjust CPU/memory requests based on query load. (4) **Custom metrics**: scale on application-specific metrics (query latency, connection count, replication lag). (5) **Operator integration**: database operators (MySQL, PostgreSQL, MongoDB) often have built-in scaling logic that understands data rebalancing.

**Q4: How do you handle autoscaling in a multi-tenant cluster?**
A: (1) Each tenant has their own HPAs with maxReplicas within their ResourceQuota. (2) ResourceQuota limits total pod count per namespace — even with aggressive autoscaling, a tenant cannot exhaust cluster resources. (3) Priority classes: production tenants' scaling takes precedence over development. (4) Shared Cluster Autoscaler provisions nodes for all tenants. (5) Per-tenant node pools (if needed) with tenant-specific CA configuration. (6) Cost attribution: autoscaled resources are still charged to the tenant's namespace.

**Q5: What is the ideal HPA configuration for a latency-sensitive microservice?**
A: (1) Metric: prefer latency (p99 response time) or RPS over CPU — they're closer to the user experience. (2) Scale-up: aggressive — `stabilizationWindowSeconds: 0`, `policies: [{type: Percent, value: 100}]` — double capacity immediately. (3) Scale-down: very conservative — `stabilizationWindowSeconds: 600` (10 min), `policies: [{type: Percent, value: 5}]` — only reduce 5% per minute. (4) minReplicas: at least 3 for zone-aware HA. (5) maxReplicas: high enough for worst-case traffic. (6) Over-provisioning balloon pods for instant capacity.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | metrics-server down | HPA cannot scale; frozen at current replicas | HPA condition: ScalingActive=False | HA metrics-server; restart | < 1 min |
| 2 | Prometheus down | Custom metrics unavailable for HPA | HPA ScalingActive=False; Prometheus alerts | HA Prometheus (Thanos) | < 5 min |
| 3 | KEDA operator down | Event-driven scaling frozen; existing HPA continues | KEDA pod health check | HA KEDA (leader election) | < 1 min |
| 4 | Cluster Autoscaler down | No new nodes provisioned; Pending pods | CA pod health check | HA CA; Pending pods metric | < 1 min |
| 5 | Cloud API outage | CA cannot provision nodes | Node provisioning timeout | Retry; use multiple AZs | Cloud-dependent |
| 6 | HPA flapping | Constant scale-up/down; wasted resources | HPA event count spike | Tune stabilization windows; increase tolerance | Tuning required |
| 7 | VPA causes OOM by reducing memory | Pods crash with OOM | OOM kill count increase | VPA learns from OOM; increase minAllowed | Auto-correcting |
| 8 | Scale-to-zero with stuck consumers | Messages accumulate without processing | Queue depth metric increase | KEDA fallback replicas; alert on long queue age | < 30s (KEDA poll) |
| 9 | Node scale-down breaks PDB | Service disruption during drain | PDB violation events | CA respects PDB; test PDB configuration | N/A (prevented) |
| 10 | Spot instance mass interruption | Multiple nodes terminated simultaneously | Spot interruption events | PDB + topology spread; CA provisions replacements | < 5 min |

### Automated Recovery

| Mechanism | Implementation |
|-----------|---------------|
| HPA stabilization window | Prevents premature scale-down; uses max replica count from window |
| KEDA fallback | Configurable replica count when triggers fail |
| CA node repair | Detects nodes that fail to become Ready; removes from node group |
| VPA OOM learning | Automatically bumps memory recommendation after OOM event |
| PDB respect | All autoscalers respect PodDisruptionBudgets during scale-down |

---

## 10. Observability

### Key Metrics

| Metric | Source | Alert Threshold | Meaning |
|--------|--------|----------------|---------|
| `kube_horizontalpodautoscaler_status_condition{condition="ScalingActive",status="false"}` | kube-state-metrics | == 1 | HPA cannot scale (metrics unavailable) |
| `kube_horizontalpodautoscaler_status_desired_replicas == kube_horizontalpodautoscaler_spec_max_replicas` | kube-state-metrics | True for > 5 min | HPA at max; may need higher limit |
| `kube_pod_status_phase{phase="Pending"}` | kube-state-metrics | Count > 0 for > 5 min | Pods waiting for capacity |
| `cluster_autoscaler_unschedulable_pods_count` | CA | > 0 for > 5 min | CA not provisioning fast enough |
| `cluster_autoscaler_scaled_up_nodes_total` | CA | Rate > 10/hour | Rapid node scaling (investigate cause) |
| `keda_scaled_object_errors` | KEDA | > 0 | KEDA trigger errors |
| `vpa_recommender_recommendation_latency_seconds` | VPA | p99 > 60s | VPA recommender slow |
| `container_cpu_cfs_throttled_seconds_total` | cAdvisor | Rate > 0.1s/s | CPU throttling (VPA under-provisioned) |
| `kube_pod_container_status_last_terminated_reason{reason="OOMKilled"}` | kube-state-metrics | Rate > 0 | Memory under-provisioned |

### Distributed Tracing

```
Autoscaling event chain tracing:
  1. Metric breach (timestamp, value, threshold)
  2. HPA/KEDA decision (timestamp, current → desired replicas)
  3. Pod creation (timestamp, scheduling latency)
  4. Node provisioning (if needed: CA detection, cloud API call, node ready)
  5. Pod running (timestamp, startup latency)
  
  Total latency = metric_collection + decision + scheduling + provisioning + startup
  
  Trace implementation:
  - HPA events: `kubectl get events --field-selector reason=SuccessfulRescale`
  - CA events: `kubectl get events -n kube-system --field-selector source=cluster-autoscaler`
  - KEDA events: `kubectl get events --field-selector source=keda-operator`
```

### Logging

```
Key log sources:
1. HPA controller: kube-controller-manager logs (search "horizontal-pod-autoscaler")
2. CA: cluster-autoscaler pod logs (--v=4 for detailed decisions)
3. KEDA: keda-operator pod logs (trigger evaluation, scaling decisions)
4. VPA: vpa-recommender logs (recommendation calculations)

Dashboard:
- Current vs. desired replicas per HPA over time
- Scale events timeline
- Node count over time (CA)
- Pending pods over time
- Queue depth vs. consumer count (KEDA)
```

---

## 11. Security

### Auth & AuthZ

```yaml
# HPA RBAC (kube-controller-manager already has this)
# Users need these permissions to create HPAs:
- apiGroups: ["autoscaling"]
  resources: ["horizontalpodautoscalers"]
  verbs: ["get", "list", "watch", "create", "update", "patch", "delete"]
- apiGroups: ["apps"]
  resources: ["deployments/scale", "statefulsets/scale"]
  verbs: ["get", "update"]  # HPA needs to read and update scale subresource

# KEDA RBAC (operator service account)
- apiGroups: ["keda.sh"]
  resources: ["scaledobjects", "scaledobjects/status", "scaledjobs", "scaledjobs/status"]
  verbs: ["*"]
- apiGroups: ["autoscaling"]
  resources: ["horizontalpodautoscalers"]
  verbs: ["*"]  # KEDA creates and manages HPAs

# Secrets for KEDA triggers (e.g., Kafka credentials)
- apiGroups: [""]
  resources: ["secrets"]
  verbs: ["get", "list", "watch"]
# Scope to specific namespaces
```

### Multi-tenancy Isolation

| Concern | Mechanism |
|---------|-----------|
| HPA maxReplicas | Per-tenant limit; ResourceQuota caps total pods |
| Node scaling costs | Cost attributed to tenant triggering the scaling |
| KEDA trigger secrets | Namespace-scoped; each tenant has their own credentials |
| VPA access | Namespace-scoped; tenants can view their own VPA recommendations |
| Priority class | Platform-managed; tenants select but cannot create |

---

## 12. Incremental Rollout Strategy

### Phase 1: Foundation (Week 1-2)
- Deploy metrics-server (HA, 2 replicas).
- Deploy Prometheus + Prometheus Adapter.
- Create HPA for 5 pilot workloads.
- Validate scale-up and scale-down behavior.

### Phase 2: Custom Metrics (Week 3-4)
- Configure Prometheus Adapter for custom metrics (RPS, latency).
- Create HPA with custom metrics for pilot workloads.
- Tune stabilization windows and policies.
- Deploy VPA in Off mode with Goldilocks dashboard.

### Phase 3: KEDA (Week 5-6)
- Deploy KEDA operator.
- Create ScaledObjects for queue-based workloads.
- Test scale-to-zero with non-critical workloads.
- Configure cron triggers for predictable traffic.

### Phase 4: Node Scaling (Week 7-8)
- Deploy Cluster Autoscaler (or Karpenter).
- Configure node groups/pools.
- Test end-to-end: HPA scale-out → Pending pods → CA provisions.
- Implement over-provisioning with balloon pods.

### Phase 5: Production Optimization (Week 9-10)
- Apply VPA recommendations to all workloads.
- Tune HPA parameters based on observed behavior.
- Set up comprehensive autoscaling dashboards.
- Document autoscaling policies and escalation procedures.

### Rollout Interviewer Q&As

**Q1: How do you validate autoscaling before going to production?**
A: (1) Load test in staging: use k6/locust to simulate traffic patterns (steady, spike, ramp). (2) Observe HPA behavior: verify scale-up speed, scale-down stabilization, max replicas. (3) Inject failures: kill metrics-server, verify HPA falls back safely. (4) Test CA: create pods that exceed node capacity, verify new nodes are provisioned. (5) Test KEDA: send messages to test queue, verify scale-from-zero latency. (6) Compare autoscaled resource usage vs. manual: ensure cost savings without degradation.

**Q2: How do you migrate from manual scaling to autoscaling?**
A: (1) Deploy HPA with current manual replica count as minReplicas and maxReplicas. (2) Gradually increase maxReplicas while monitoring. (3) Gradually decrease minReplicas toward the optimal minimum. (4) Monitor for 1 week with each change. (5) For VPA: start in Off mode, compare recommendations with current settings. (6) Apply VPA changes during normal deployment cycles.

**Q3: How do you handle autoscaling during a known traffic event (product launch, Black Friday)?**
A: (1) Pre-scale using KEDA cron trigger: set high minReplicaCount before the event. (2) Pre-provision nodes: increase CA node group's min size. (3) Raise HPA maxReplicas if current limit is insufficient. (4) Disable CA scale-down during the event (set scale-down-unneeded-time to 24h). (5) After the event: revert cron trigger, CA resumes normal scale-down. (6) Post-mortem: analyze autoscaling behavior, tune for next event.

**Q4: How do you debug an HPA that is not scaling as expected?**
A: (1) `kubectl describe hpa <name>`: check conditions (ScalingActive, AbleToScale, ScalingLimited). (2) Check current metrics: `.status.currentMetrics` — does the metric value exceed the target? (3) If ScalingActive=False: metrics-server or adapter is down. (4) If AbleToScale=False: check if maxReplicas is reached, or if the scale target exists. (5) If scaling but too slow: check `.spec.behavior.scaleUp.policies` — maybe limited to adding 1 pod per minute. (6) Check tolerance: if current metric is within 10% of target, HPA won't scale.

**Q5: How do you implement canary deployments with autoscaling?**
A: (1) Use two Deployments (stable + canary) each with their own HPA. (2) Canary HPA has same metric targets but lower maxReplicas (e.g., 10% of traffic). (3) Traffic splitting: use Istio VirtualService or Ingress weight to route 10% to canary. (4) Both HPAs scale independently based on their traffic share. (5) As canary percentage increases, adjust HPA maxReplicas proportionally. (6) Once canary is promoted, merge into single Deployment with full HPA.

---

## 13. Trade-offs & Decision Log

| # | Decision | Alternative Considered | Trade-off | Rationale |
|---|----------|----------------------|-----------|-----------|
| 1 | HPA v2 with behavior config | Custom autoscaler | Less flexible but well-tested and maintained | HPA v2 covers 90% of use cases; behavior API provides sufficient tuning |
| 2 | VPA in Off mode (recommend only) | VPA Auto (live apply) | Manual effort but no pod disruption | Production services cannot tolerate random pod evictions |
| 3 | KEDA for event-driven scaling | Custom HPA external metrics adapter | Additional component but 60+ pre-built triggers | KEDA eliminates boilerplate; scale-to-zero is unique capability |
| 4 | Cluster Autoscaler over Karpenter | Karpenter | Slower provisioning but multi-cloud support | CA works on AWS, GCP, Azure, bare-metal; Karpenter is AWS-only |
| 5 | Balloon pods for over-provisioning | Aggressive CA settings | Wasted resources but instant capacity | Worth the cost for latency-sensitive services |
| 6 | CPU + custom metrics (RPS) for HPA | CPU only | More complex but more accurate scaling signal | CPU utilization doesn't always correlate with user-facing load |
| 7 | 5-min scale-down stabilization | Shorter (1 min) | Slower cost savings but prevents flapping | Traffic patterns are often bursty; 5 min avoids premature scale-down |
| 8 | Prometheus Adapter | Datadog/SignalFx adapter | Open-source but more operational overhead | Vendor-neutral; avoids lock-in; integrates with existing Prometheus |
| 9 | KEDA Kafka scaler | Custom consumer-group HPA metric | Less customizable but turnkey | KEDA handles offset tracking, lag calculation, consumer-group mapping |

---

## 14. Agentic AI Integration

### AI-Powered Autoscaling

| Use Case | AI Agent Capability | Implementation |
|----------|-------------------|---------------|
| **Predictive scaling** | Forecast traffic and pre-scale before demand arrives | Agent trains on historical traffic patterns (time-series forecasting); generates KEDA cron triggers or pre-scaling schedules |
| **HPA parameter tuning** | Optimize stabilization windows, policies, targets | Agent runs experiments: try different HPA configurations on canary, measure scale-up time, flapping frequency, cost; select optimal |
| **Anomaly-triggered scaling** | Detect unusual patterns and adjust scaling parameters | Agent detects: "Tuesday spike is 2x normal — override HPA max temporarily" |
| **Cost-aware scaling** | Balance performance and cost | Agent monitors: spot prices, reserved capacity utilization, on-demand fallback; adjusts CA/Karpenter to minimize cost while meeting SLOs |
| **VPA auto-apply** | Safely apply VPA recommendations at optimal times | Agent identifies maintenance windows, validates recommendations against OOM history, applies changes during low-traffic periods |

### Example: AI-Driven Predictive Autoscaler

```python
class PredictiveAutoscaler:
    """
    Uses time-series forecasting to pre-scale workloads
    before traffic arrives.
    """
    
    def generate_scaling_schedule(self, deployment: str, namespace: str) -> list:
        # Query historical traffic (4 weeks)
        traffic_history = self.prometheus.query_range(
            f'sum(rate(http_requests_total{{deployment="{deployment}",'
            f'namespace="{namespace}"}}[5m]))',
            start='-28d', step='5m'
        )
        
        # Train forecasting model (Prophet, ARIMA, or LSTM)
        model = self.train_forecast_model(traffic_history)
        
        # Predict next 24 hours
        forecast = model.predict(horizon='24h', interval='5m')
        
        # Calculate required replicas at each time point
        schedule = []
        for timestamp, predicted_rps in forecast:
            required_replicas = math.ceil(predicted_rps / self.rps_per_pod)
            # Add 20% headroom for prediction error
            safe_replicas = math.ceil(required_replicas * 1.2)
            schedule.append((timestamp, safe_replicas))
        
        # Convert to KEDA cron triggers
        cron_triggers = self.convert_to_cron(schedule)
        
        # Generate ScaledObject patch
        return ScaledObjectPatch(
            deployment=deployment,
            namespace=namespace,
            triggers=cron_triggers,
            confidence=model.confidence_interval,
            fallback_replicas=self.calculate_fallback(traffic_history)
        )
    
    def evaluate_prediction_accuracy(self):
        """
        Compare predictions vs actual, adjust model.
        """
        for deployment in self.managed_deployments:
            predicted = self.get_predicted_replicas(deployment, yesterday)
            actual = self.get_actual_replicas(deployment, yesterday)
            
            accuracy = 1 - abs(predicted - actual) / actual
            
            if accuracy < 0.8:
                self.retrain_model(deployment)
                self.alert(f"Prediction accuracy for {deployment} dropped to {accuracy:.0%}")
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the complete autoscaling chain from traffic spike to new pod serving traffic.**
A: (1) Traffic spike hits existing pods → CPU/RPS increases. (2) metrics-server scrapes kubelet metrics (15s interval). (3) HPA controller queries metrics API (every 15s). (4) HPA calculates desired replicas: `ceil(current * (actual/target))`. (5) HPA patches Deployment's replica count. (6) ReplicaSet controller creates new pods. (7) Scheduler assigns pods to nodes (if capacity exists → pod starts in ~5s). (8) If no capacity: pods stuck in Pending. (9) Cluster Autoscaler detects Pending pods (10s scan). (10) CA provisions new node via cloud API (1-3 min). (11) New node joins cluster, becomes Ready. (12) Scheduler places Pending pods on new node. (13) Pods start, pass readiness probe. (14) Endpoint controller adds pods to Service. (15) New pods receive traffic. Total: 30s-7 min depending on whether new nodes are needed.

**Q2: How does HPA calculate desired replicas when using multiple metrics?**
A: HPA evaluates each metric independently using the formula `ceil(currentReplicas * (currentValue / targetValue))`. It then takes the maximum across all metrics. Example: CPU needs 5 replicas, RPS needs 8, queue depth needs 3 → desired = 8. This ensures all metrics are satisfied simultaneously. For scale-down, the stabilization window takes the maximum desired replicas from the last N seconds, providing a conservative scale-down signal.

**Q3: What is the impact of pod startup time on autoscaling effectiveness?**
A: Pod startup time directly affects the "reaction time" of the system. If it takes 60s for a pod to start serving traffic, traffic may be degraded for 60s after the scaling decision. Mitigations: (1) Optimize image size (smaller = faster pull). (2) Use startup probes to accurately signal when the pod is ready. (3) Pre-pull images via DaemonSet. (4) Balloon pods for instant capacity (preempt paused pods). (5) Pre-scale using KEDA cron triggers before expected traffic. (6) For JVM apps: use CRaC (Coordinated Restore at Checkpoint) or GraalVM native image for < 1s startup.

**Q4: How do you autoscale a GPU workload?**
A: (1) GPU requests are not supported by HPA natively (no "GPU utilization" in metrics-server). (2) Use DCGM Exporter to expose GPU metrics to Prometheus. (3) Configure Prometheus Adapter to expose GPU utilization as a custom metric. (4) Create HPA with custom metric: `nvidia.com/gpu_utilization` target 80%. (5) Cluster Autoscaler: configure a node group with GPU instances. (6) Challenge: GPU instances are expensive and slow to provision. (7) Use Karpenter for faster instance selection (picks smallest GPU instance that fits).

**Q5: How does VPA handle workloads with highly variable resource usage?**
A: VPA uses a weighted histogram of historical usage. For highly variable workloads: (1) The target recommendation (90th percentile) will be much higher than average — ensuring the container has enough resources for peaks. (2) The gap between lowerBound and upperBound will be wide, indicating high variability. (3) You can set `controlledValues: RequestsOnly` — VPA adjusts requests (scheduling) but leaves limits as-is (burst headroom). (4) For periodic variability (daily patterns), VPA's 8-day window captures the pattern. (5) For truly unpredictable workloads, use HPA (horizontal scaling) instead of VPA (vertical scaling).

**Q6: How do you implement scale-to-zero for an HTTP service?**
A: (1) KEDA with HTTP Add-on or Knative Serving. (2) KEDA approach: ScaledObject with HTTP trigger that monitors request count. When no requests for cooldownPeriod → scale to 0. When a request arrives → KEDA's HTTP interceptor proxy holds the request while scaling to minReplicaCount → forwards request when pod is ready. (3) Cold start latency: 30-60s typical (KEDA poll + pod startup). (4) Knative approach: activator component is always running, buffers requests, and scales from 0 faster (dedicated component). (5) For latency-sensitive services: consider minReplicaCount=1 instead of true scale-to-zero.

**Q7: What is the Cluster Autoscaler's expander strategy and which should you use?**
A: The expander determines which node group to scale when multiple groups can satisfy the pending pods. Options: (1) `random` — pick randomly among feasible groups. (2) `most-pods` — pick the group that schedules the most pending pods. (3) `least-waste` — pick the group that minimizes resource waste (most efficient packing). (4) `price` — pick the cheapest group. (5) `priority` — user-defined priority list. Recommendation: `least-waste` for cost optimization, `priority` for explicit control (e.g., prefer spot over on-demand), `price` when cost is the primary concern.

**Q8: How do you autoscale CronJobs that run periodically?**
A: CronJobs are not autoscaled in the traditional sense (they run to completion). KEDA provides ScaledJob: (1) ScaledJob creates Job resources based on trigger (e.g., queue depth). (2) Each Job runs one batch of work, then completes. (3) ScaledJob creates more Jobs when more work arrives. (4) Unlike ScaledObject (which scales a Deployment), ScaledJob creates and destroys Jobs. (5) Use case: processing a queue where each item is a separate job. (6) Alternative: HPA on a Deployment that processes the queue (long-lived consumers).

**Q9: How do you handle the "thundering herd" problem when scaling from 0 to many replicas?**
A: When KEDA detects a large backlog after scale-to-zero: (1) KEDA first scales to minReplicaCount (e.g., 1). (2) Then creates HPA which detects high load and scales further. (3) To speed this up: set `maxReplicaCount` high and let KEDA/HPA calculate the needed count immediately. (4) KEDA will calculate desired replicas based on queue depth: `ceil(1000 messages / 100 threshold) = 10` even on the first evaluation. (5) But the scheduler needs to place all 10 pods — if nodes are full, CA needs time. (6) Mitigation: balloon pods for instant capacity; pre-provisioned warm nodes.

**Q10: How do you monitor the cost impact of autoscaling?**
A: (1) Track node count over time: `cluster_autoscaler_nodes_count`. (2) Track pod count per HPA: `kube_horizontalpodautoscaler_status_current_replicas`. (3) Calculate cost: node-hours x instance cost. (4) Compare with baseline (static sizing): what would it cost without autoscaling? (5) Kubecost/OpenCost provides per-workload cost tracking including autoscaled resources. (6) Key metric: cost per request = total infrastructure cost / total requests served. Should decrease with effective autoscaling.

**Q11: How does in-place pod vertical scaling work (alpha feature)?**
A: Traditional VPA requires pod restart to change resource requests. In-place resize (InPlacePodVerticalScaling feature gate, v1.33): (1) User or VPA patches the pod's `spec.containers[].resources.requests`. (2) kubelet adjusts cgroup limits without restarting the container. (3) For CPU: cgroup cpu.max is updated — immediate effect. (4) For memory: more complex — increasing memory limit is fine, but decreasing may OOM if memory is in use. (5) Pod status reports `resize: InProgress` then `resize: Proposed` then cleared. (6) This eliminates the VPA disruption problem entirely when it reaches GA.

**Q12: How do you handle autoscaling for workloads behind a load balancer with connection draining?**
A: During scale-down: (1) HPA reduces desired replicas. (2) ReplicaSet selects pods to terminate. (3) Pod enters Terminating state: endpoint controller removes from Service. (4) Load balancer health check detects the pod is no longer an endpoint. (5) Existing connections drain during `terminationGracePeriodSeconds`. (6) New connections are routed to remaining pods. Risk: if scale-down is too aggressive and drain takes too long, remaining pods may be overloaded. Mitigation: conservative scale-down policy, preStop hook with sleep for endpoint propagation, adequate terminationGracePeriod.

**Q13: How do you autoscale a service mesh sidecar (Envoy/Istio)?**
A: The sidecar scales with the application pod (same pod = same scaling). But sidecar resource consumption may not correlate with application metrics. Solutions: (1) HPA with ContainerResource metric (v1.30+): target the application container's CPU, not the pod average. (2) VPA: separate recommendations for application container and sidecar container (VPA provides per-container recommendations). (3) Sidecar resources: typically 50-200m CPU, 50-200 MB RAM — set via Istio's global proxy config. (4) For ambient mesh (sidecar-less): the scaling concern is eliminated — no sidecar to size.

**Q14: What is the difference between Cluster Autoscaler and Karpenter's approach to node deprovisioning?**
A: CA: (1) Marks node as underutilized if requests < 50% of capacity. (2) Waits 10 min. (3) Drains node. (4) Deletes from ASG. Karpenter: (1) Continuously evaluates: "can I consolidate pods onto fewer/cheaper nodes?" (2) If yes: creates optimal replacement node, cordons old node, drains pods, terminates old node. (3) Can also replace expensive nodes with cheaper ones (e.g., on-demand with spot). (4) Karpenter is more aggressive and cost-optimized; CA is safer and simpler.

**Q15: How do you implement autoscaling for a CI/CD pipeline (batch workloads)?**
A: (1) Use KEDA ScaledJob: each CI/CD task is a Job, scaled based on queue depth (GitHub webhook events, Jenkins build queue). (2) maxReplicaCount = max concurrent builds. (3) Scale-to-zero: no runners when no builds queued. (4) Node scaling: Karpenter/CA provisions nodes for build pods. (5) Use spot instances for builds (cost savings, builds are interruptible). (6) Aggressive scale-up (builds should start quickly), conservative scale-down (nodes may be reused by next build).

---

## 16. References

| # | Reference | URL |
|---|-----------|-----|
| 1 | HPA v2 Documentation | https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/ |
| 2 | HPA Behavior (scale-up/down policies) | https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/#configurable-scaling-behavior |
| 3 | VPA Documentation | https://github.com/kubernetes/autoscaler/tree/master/vertical-pod-autoscaler |
| 4 | KEDA | https://keda.sh/ |
| 5 | Cluster Autoscaler | https://github.com/kubernetes/autoscaler/tree/master/cluster-autoscaler |
| 6 | Karpenter | https://karpenter.sh/ |
| 7 | Prometheus Adapter | https://github.com/kubernetes-sigs/prometheus-adapter |
| 8 | Goldilocks (VPA Dashboard) | https://github.com/FairwindsOps/goldilocks |
| 9 | metrics-server | https://github.com/kubernetes-sigs/metrics-server |
| 10 | In-Place Pod Vertical Scaling KEP | https://github.com/kubernetes/enhancements/tree/master/keps/sig-node/1287-in-place-update-pod-resources |
| 11 | Knative Serving (Scale-to-Zero) | https://knative.dev/docs/serving/ |
| 12 | KEDA Scalers Catalog | https://keda.sh/docs/scalers/ |
