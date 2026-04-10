# System Design: Throttling Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Application-level throttling**: control how fast internal services (producers) send work to downstream consumers — not just reject at the API boundary, but intelligently slow, queue, and shed load.
2. **Priority queuing**: requests are classified into priority classes (P0=critical, P1=high, P2=normal, P3=low). Under overload, lower-priority requests are deferred or shed first.
3. **Adaptive throttling**: the service automatically adjusts throttle thresholds based on real-time downstream health signals (latency, error rates, saturation).
4. **Backpressure propagation**: when downstream is saturated, the throttling service propagates backpressure upstream through the call chain, causing callers to slow their request rate rather than buffering indefinitely.
5. **Graceful degradation**: when the system is overloaded, return degraded (cached, partial, or default) responses rather than errors for eligible request types.
6. **Client-side throttling**: client libraries embed throttling logic to pre-reject requests locally before even sending to the server, based on the server's last known health signal.
7. **Circuit breaker integration**: the throttling service acts as the coordinator for per-downstream circuit breakers across all callers in the fleet.
8. **Quota management**: per-service daily/hourly quotas in addition to per-second rate limits (e.g., a batch job gets 1M operations/day at 10K/sec max).

### Non-Functional Requirements

1. **Latency of throttling decision**: < 2 ms overhead per request (throttling runs in-process within each service, not as an external hop).
2. **Availability**: the throttling service's control plane (adaptive threshold computation) can tolerate 30-second outages — control plane is not in the critical request path.
3. **Throughput**: each service instance may handle 50,000 requests/second; throttling logic must not become the bottleneck.
4. **Propagation latency**: an adaptive threshold change (e.g., downstream circuit open) must propagate to all client instances within 5 seconds.
5. **No single point of failure**: if the central throttling control plane is unavailable, all service instances fall back to static configured thresholds.
6. **Observability**: every throttling decision must be observable (metrics, traces, logs) with enough context to diagnose why a request was throttled.
7. **Language-agnostic client SDK**: client libraries available for Go, Java, Python, Node.js.

### Out of Scope

- **API-boundary rate limiting** (covered in `api_rate_limiter.md`) — that system handles external client limits. This system handles internal service-to-service traffic.
- **Load balancing and service discovery** — assumed to be handled by a service mesh (e.g., Envoy, Istio).
- **Queueing infrastructure** (Kafka, RabbitMQ) — this service sits in front of those systems and controls the rate at which producers write to them.
- **SLA enforcement for external customers** — that's the API rate limiter's concern.

---

## 2. Users & Scale

### User Types

| Actor | Description | Interaction with Throttling Service |
|---|---|---|
| **Microservices (producers)** | Services that send requests to downstream services | Embed the throttling client SDK; obey throttle decisions |
| **Downstream services (consumers)** | Services that receive requests; may be overloaded | Publish health metrics consumed by the adaptive throttle |
| **Platform engineers** | Configure throttle policies, set quotas, tune adaptive parameters | Use the management UI and admin API |
| **SREs on-call** | React to incidents; manually adjust throttle settings | Emergency override API; dashboards |
| **Batch jobs** | Long-running background workers sending high-volume requests | Subject to priority-based throttling; can be shed first under load |

### Traffic Estimates

**Assumptions:**
- Microservices fleet: 500 distinct services, average 20 instances each = 10,000 service instances.
- Each service instance handles an average of 5,000 requests/second.
- Total in-fleet RPS: 10,000 instances × 5,000 RPS = 50,000,000 RPS (50M RPS internal traffic).
- Priority distribution: P0=5%, P1=25%, P2=50%, P3=20%.
- Average throttle decision rate: 2% of requests are throttled (in steady state).
- Throttled requests per second: 50M × 0.02 = 1,000,000 throttled/sec.
- Control plane operations (threshold updates): 500 services × 1 update/sec = 500 ops/sec (tiny).

| Metric | Calculation | Result |
|---|---|---|
| Total in-fleet RPS | 10,000 instances × 5,000 RPS | 50,000,000 RPS |
| Throttle checks/sec (in-process) | 50,000,000 × 1 | 50,000,000/sec |
| Throttled requests/sec (steady state) | 50,000,000 × 2% | 1,000,000/sec |
| P0 requests/sec | 50,000,000 × 5% | 2,500,000/sec |
| P3 (batch) requests/sec | 50,000,000 × 20% | 10,000,000/sec |
| Control plane updates/sec | 500 services × 1 | 500/sec |
| Metrics published to control plane | 10,000 instances × 10 metrics/sec | 100,000 metrics/sec |
| Adaptive threshold computations | 500 services × 1/sec | 500/sec |

> **Key insight**: The hot path (throttle check per request) runs entirely in-process within each service instance — zero network hops. Only the control plane (threshold synchronization) involves network communication, and that's a 500 ops/sec operation, not 50M.

### Latency Requirements

| Operation | Target p50 | Target p99 | Notes |
|---|---|---|---|
| In-process throttle decision | < 0.1 ms | < 0.5 ms | In-process: token bucket check in memory |
| Threshold sync from control plane | < 1 ms | < 3 ms | Async background refresh |
| Adaptive threshold recompute | < 10 ms | < 50 ms | Control plane computation; non-critical path |
| Priority queue dequeue decision | < 0.2 ms | < 1 ms | In-process priority queue check |
| Circuit breaker state check | < 0.05 ms | < 0.2 ms | In-process state machine |
| Emergency override propagation | < 2 s | < 5 s | Push-based; alert if > 5s |

### Storage Estimates

| Data Type | Size per Entry | Count | Total |
|---|---|---|---|
| Throttle policy per service | 2 KB JSON | 500 services | 1 MB |
| Per-instance token bucket state | 200 bytes | 10,000 instances × 20 token buckets | 40 MB (in-process only) |
| Adaptive threshold history (1 hr) | 100 bytes per datapoint | 500 services × 3,600 points | 180 MB |
| Circuit breaker state | 50 bytes | 500 services × 50 downstream dependencies | 1.25 MB |
| Audit log (throttle events) | 300 bytes per event | 1M events/sec × 86,400 sec | ~26 TB/day (compressed 10:1 → 2.6 TB/day) |

> The 26 TB/day audit log is stored in tiered object storage (S3 Intelligent Tiering). For real-time analysis, only the last 10 minutes are kept in hot storage (Elasticsearch), totaling ~26 GB.

### Bandwidth Estimates

| Traffic Type | Calculation | Result |
|---|---|---|
| Metrics published to control plane | 100,000 metrics/sec × 50 bytes | 5 MB/s |
| Threshold updates pushed to instances | 500 updates/sec × 1 KB | 500 KB/s |
| Audit log stream to Kafka | 1M events/sec × 300 bytes | 300 MB/s |
| In-process decision (zero network) | — | 0 bytes |

---

## 3. High-Level Architecture

```
      ┌─────────────────────────────────────────────────────────────────────────────┐
      │                         SERVICE INSTANCE (e.g., Search Service)             │
      │                                                                             │
      │  ┌───────────────────────────────────────────────────────────────────────┐  │
      │  │                      INBOUND REQUEST HANDLER                         │  │
      │  │  Receive request → [Priority Classifier] → [Admission Controller]    │  │
      │  │                            │                        │                │  │
      │  │                     Assign P0-P3            Check: is system healthy? │  │
      │  │                            │                Is quota available?       │  │
      │  │                            ▼                        │                │  │
      │  │                   ┌─────────────────┐       Allow / Throttle / Shed  │  │
      │  │                   │  Priority Queue  │               │               │  │
      │  │                   │  P0 ──────────── │ dequeue       │               │  │
      │  │                   │  P1 ──────────── │ ──────────────▶               │  │
      │  │                   │  P2 ──────────── │  in priority   ▼              │  │
      │  │                   │  P3 ──────────── │  order    [Request Handler]   │  │
      │  │                   └─────────────────┘               │               │  │
      │  │                                                      ▼               │  │
      │  │                                          [Outbound Throttle Client]   │  │
      │  │                                         (controls rate of calls to   │  │
      │  │                                          downstream services)         │  │
      │  └───────────────────────────────────────────────────────────────────────┘  │
      │                                                                             │
      │  ┌─────────────────────────────────────────────────────────────────────┐   │
      │  │                   IN-PROCESS THROTTLE STATE                         │   │
      │  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  │   │
      │  │  │  Token Buckets   │  │  Circuit Breakers │  │  Quota Counters  │  │   │
      │  │  │  (per downstream │  │  (per downstream  │  │  (per service,   │  │   │
      │  │  │   dependency)    │  │   dependency)     │  │   per window)    │  │   │
      │  │  └──────────────────┘  └──────────────────┘  └──────────────────┘  │   │
      │  └─────────────────────────────────────────────────────────────────────┘   │
      └─────────────────────────────────────────────────────────────────────────────┘
                               │  Health metrics (async, every 1s)
                               │  Threshold updates (async, every 1-5s)
                               ▼
      ┌─────────────────────────────────────────────────────────────────────────────┐
      │                       THROTTLING CONTROL PLANE                              │
      │                                                                             │
      │  ┌──────────────────────────────────────────────────────────────────────┐  │
      │  │                   ADAPTIVE THRESHOLD ENGINE                          │  │
      │  │                                                                      │  │
      │  │  Inputs:                          Processing:                        │  │
      │  │  - Downstream error rates    →    PID controller per service         │  │
      │  │  - Downstream p99 latency    →    Compute new throttle rate          │  │
      │  │  - Downstream queue depth    →    Apply smoothing (EWMA)             │  │
      │  │  - CPU/memory saturation     →    Detect anomalies                   │  │
      │  │                                                                      │  │
      │  │  Outputs:                                                            │  │
      │  │  - New token bucket rate per (caller, callee) pair                   │  │
      │  │  - Circuit breaker open/close signals                                │  │
      │  │  - Priority shed thresholds                                          │  │
      │  └──────────────────────────────────────────────────────────────────────┘  │
      │                                                                             │
      │  ┌──────────────────┐    ┌─────────────────────┐    ┌─────────────────┐   │
      │  │  Policy Store    │    │  Metrics Aggregator  │    │  Push Gateway   │   │
      │  │  (PostgreSQL)    │    │  (Prometheus/TSDB)   │    │  (gRPC stream   │   │
      │  │  Source of truth │    │  Stores 1hr history  │    │   to instances) │   │
      │  │  for all policies│    │  for adaptive algo   │    │                 │   │
      │  └──────────────────┘    └─────────────────────┘    └─────────────────┘   │
      └─────────────────────────────────────────────────────────────────────────────┘
                               │  Push threshold updates
                               │  Circuit breaker signals
                               ▼
      ┌─────────────────────────────────────────────────────────────────────────────┐
      │                 ALL OTHER SERVICE INSTANCES (same pattern)                  │
      └─────────────────────────────────────────────────────────────────────────────┘

      ┌─────────────────────────────────────────────────────────────────────────────┐
      │                         SUPPORTING INFRASTRUCTURE                           │
      │                                                                             │
      │  ┌──────────────────┐    ┌─────────────────────┐    ┌─────────────────┐   │
      │  │  Admin API       │    │  Kafka (Throttle     │    │  S3 + Athena    │   │
      │  │  (Policy CRUD,   │    │  Events Topic)       │    │  (Long-term     │   │
      │  │  Emergency       │    │  Audit & analytics   │    │  audit storage) │   │
      │  │  overrides)      │    │                      │    │                 │   │
      │  └──────────────────┘    └─────────────────────┘    └─────────────────┘   │
      └─────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

| Component | Role |
|---|---|
| **Priority Classifier** | Assigns P0-P3 priority class to each inbound request based on request metadata (endpoint, caller identity, payload hints). Runs in < 0.1 ms. |
| **Admission Controller** | Decides whether to allow, queue, or shed the request based on current system health, queue depth, circuit breaker state, and quota. |
| **Priority Queue** | In-process bounded queue organized by priority class. Under load, lower-priority items are dequeued last or dropped first. |
| **Outbound Throttle Client** | Token bucket per downstream dependency. Controls the rate at which this service sends requests outbound. Enforces backpressure. |
| **In-Process Throttle State** | All throttle-relevant state (token buckets, circuit breakers, quota counters) stored in process memory. Zero network latency. |
| **Adaptive Threshold Engine** | The "brain" of the throttling service. Uses health metrics from all instances to compute optimal per-service throttle rates and propagates changes. |
| **Policy Store (PostgreSQL)** | Durable storage for all throttle policies (static limits, priority rules, quota definitions). Source of truth for the control plane. |
| **Metrics Aggregator** | Aggregates per-instance health metrics (error rates, latency percentiles) for the adaptive algorithm's input. |
| **Push Gateway** | Maintains persistent gRPC streams to all service instances. Pushes threshold updates within 5 seconds. |
| **Admin API** | CRUD for throttle policies; emergency override endpoint for on-call SREs. |
| **Kafka (Throttle Events)** | Decouples throttle event logging from the hot path. All throttling decisions are published here for audit and analytics. |

**Primary Use-Case Data Flow (Overloaded Downstream):**
1. Downstream service's error rate rises above 5% threshold.
2. Service instances detect increased errors in their local circuit breaker state.
3. Instances publish health metrics to the control plane's Metrics Aggregator.
4. Adaptive Threshold Engine detects the degradation within 1 second. Computes a new (lower) token bucket rate for callers of that downstream.
5. Push Gateway sends updated threshold to all instances via gRPC stream within 5 seconds.
6. Instances update their outbound token bucket rate in-process.
7. New requests to the overloaded downstream are throttled (queued or shed). Lower-priority requests (P2, P3) are shed first. P0 requests still go through at the reduced rate.
8. The downstream's load decreases; its error rate recovers.
9. Adaptive engine detects recovery; gradually increases the throttle rate back to normal (linear ramp-up, not a step function, to avoid re-triggering overload).

---

## 4. Data Model

### Entities & Schema

**1. `throttle_policies` (PostgreSQL)**

```sql
CREATE TABLE throttle_policies (
    id              BIGSERIAL PRIMARY KEY,
    policy_name     VARCHAR(128)    NOT NULL UNIQUE,
    caller_service  VARCHAR(128)    NOT NULL,         -- service sending requests
    callee_service  VARCHAR(128)    NOT NULL,         -- service receiving requests
    callee_endpoint VARCHAR(256),                     -- NULL = all endpoints
    max_rps         INT NOT NULL,                     -- sustained max requests/sec
    burst_rps       INT NOT NULL,                     -- burst capacity (token bucket ceiling)
    priority_shed_thresholds JSONB NOT NULL,          -- {"P3": 0.8, "P2": 0.9, "P1": 0.95} (fraction of max_rps before shedding)
    adaptive_enabled BOOLEAN NOT NULL DEFAULT TRUE,
    adaptive_min_rps INT NOT NULL DEFAULT 1,          -- adaptive will never reduce below this
    adaptive_max_rps INT NOT NULL,                    -- adaptive will never exceed max_rps
    circuit_breaker_config JSONB NOT NULL,            -- {"error_threshold": 0.05, "latency_threshold_ms": 500, "window_seconds": 10, "recovery_time_seconds": 30}
    quota_config     JSONB,                           -- NULL = no quota; {"daily_limit": 1000000, "hourly_limit": 100000}
    graceful_degradation_config JSONB,                -- NULL = no degradation; {"strategy": "cached_response", "cache_ttl_seconds": 30}
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    version          INT NOT NULL DEFAULT 1,          -- optimistic locking
    created_at       TIMESTAMP NOT NULL DEFAULT NOW(),
    updated_at       TIMESTAMP NOT NULL DEFAULT NOW(),
    created_by       VARCHAR(128) NOT NULL,
    INDEX idx_caller_callee (caller_service, callee_service),
    INDEX idx_active (is_active)
);
```

**2. `priority_rules` (PostgreSQL)**

```sql
CREATE TABLE priority_rules (
    id              BIGSERIAL PRIMARY KEY,
    policy_id       BIGINT NOT NULL REFERENCES throttle_policies(id),
    rule_name       VARCHAR(128) NOT NULL,
    priority_class  SMALLINT NOT NULL CHECK (priority_class BETWEEN 0 AND 3),  -- 0=critical, 3=low
    match_conditions JSONB NOT NULL,
    -- Example: {"endpoint": "/v1/payment", "caller_metadata.user_tier": "enterprise"}
    -- Example: {"header.X-Request-Type": "batch"}
    description     TEXT,
    created_at      TIMESTAMP NOT NULL DEFAULT NOW(),
    INDEX idx_policy_priority (policy_id, priority_class)
);
```

**3. `throttle_events` (Kafka → S3/Parquet — not in PostgreSQL)**

```json
{
  "event_id":         "uuid-v4",
  "timestamp":        "2025-01-01T00:00:00.001Z",
  "event_type":       "THROTTLED | SHED | CIRCUIT_OPEN | CIRCUIT_CLOSE | QUOTA_EXCEEDED",
  "caller_service":   "search-service",
  "callee_service":   "index-service",
  "callee_endpoint":  "/v1/search",
  "priority_class":   2,
  "decision":         "SHED",
  "reason":           "priority_shed_p2_threshold_exceeded",
  "current_rps":      9800,
  "max_rps":          10000,
  "shed_threshold":   0.9,
  "queue_depth":      450,
  "instance_id":      "search-service-pod-007",
  "request_id":       "req-uuid",
  "trace_id":         "trace-uuid"
}
```

**4. In-Process State Structures (not persisted — ephemeral per instance)**

```go
// TokenBucket represents the in-process throttle state per (caller, callee) pair
type TokenBucket struct {
    mu           sync.Mutex
    Rate         float64       // tokens per second (updated by control plane)
    BurstSize    float64       // max bucket capacity
    Tokens       float64       // current token count
    LastRefill   time.Time     // last refill timestamp
    MinRate      float64       // adaptive lower bound
    MaxRate      float64       // adaptive upper bound
}

// CircuitBreaker represents per-downstream circuit state
type CircuitBreaker struct {
    mu               sync.RWMutex
    State            CircuitState  // CLOSED, OPEN, HALF_OPEN
    ErrorRate        float64       // sliding window error rate (0.0 to 1.0)
    P99LatencyMs     float64       // sliding window p99 latency
    RequestCount     int64         // requests in current window
    ErrorCount       int64         // errors in current window
    LastStateChange  time.Time
    RecoveryTimeout  time.Duration
    ErrorThreshold   float64       // e.g., 0.05 (5% error rate)
    LatencyThreshold time.Duration // e.g., 500ms
}

// QuotaCounter represents the rolling window quota state
type QuotaCounter struct {
    mu           sync.Mutex
    HourlyCount  int64
    DailyCount   int64
    HourlyLimit  int64
    DailyLimit   int64
    HourReset    time.Time
    DayReset     time.Time
}

// PriorityQueue is a bounded multi-level queue
type PriorityQueue struct {
    mu        sync.Mutex
    levels    [4]chan Request  // P0, P1, P2, P3
    maxDepths [4]int           // per-priority depth limits
}
```

### Database Choice

**For Policy Store:**

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **PostgreSQL** | JSONB for flexible config; ACID transactions; rich indexing; row-level locking for concurrent policy updates | Requires connection pooling; not suitable for hot-path reads | **Selected** |
| MySQL | Similar; JSON support less rich than PostgreSQL JSONB | JSONB indexing inferior to PostgreSQL's GIN indexes | Viable alternative |
| MongoDB | Flexible schema; easy for nested config objects | Eventual consistency by default; transactions require specific configuration | Rejected |
| etcd | Strongly consistent; designed for config storage; built-in watch/subscribe | Limited query capabilities; not suitable for complex policy queries | Viable for config-only (no metrics) |

**Justification for PostgreSQL:** The `circuit_breaker_config`, `priority_shed_thresholds`, and `graceful_degradation_config` are structured but variable JSON objects — PostgreSQL's JSONB with GIN indexing handles this elegantly. We can query policies by matching on nested JSON fields (e.g., "find all policies where circuit_breaker error_threshold < 0.05"). ACID transactions ensure a multi-step policy update (e.g., change both max_rps and priority thresholds) is atomic.

**For Metrics/Time-Series:**

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **Prometheus + VictoriaMetrics** | Purpose-built for metrics; PromQL for aggregations; efficient storage | Not ACID; query complexity for high-cardinality labels | **Selected** |
| InfluxDB | Excellent time-series performance; SQL-like query language | Higher operational complexity | Viable alternative |
| Cassandra | Excellent write throughput; good for time-series with TTL | More complex PromQL-equivalent queries | Overkill for this scale |

---

## 5. API Design

**Management API — all endpoints require `Authorization: Bearer <admin_token>` on internal network.**

---

**GET /v1/throttle/policies**

List all throttle policies.

```
GET /v1/throttle/policies?caller=search-service&callee=index-service&page=1&page_size=50
Authorization: Bearer <admin_token>

Response 200 OK:
{
  "policies": [
    {
      "id": 7,
      "policy_name": "search_to_index_default",
      "caller_service": "search-service",
      "callee_service": "index-service",
      "max_rps": 10000,
      "burst_rps": 15000,
      "adaptive_enabled": true,
      "priority_shed_thresholds": {"P3": 0.8, "P2": 0.9, "P1": 0.95},
      "circuit_breaker_config": {
        "error_threshold": 0.05,
        "latency_threshold_ms": 500,
        "window_seconds": 10,
        "recovery_time_seconds": 30
      },
      "is_active": true,
      "version": 3
    }
  ],
  "pagination": {"page": 1, "page_size": 50, "total": 1}
}
```

---

**POST /v1/throttle/policies**

Create a new throttle policy.

```
POST /v1/throttle/policies
Authorization: Bearer <admin_token>
Content-Type: application/json

{
  "policy_name": "payment_to_fraud_check",
  "caller_service": "payment-service",
  "callee_service": "fraud-check-service",
  "max_rps": 5000,
  "burst_rps": 7500,
  "adaptive_enabled": true,
  "adaptive_min_rps": 500,
  "adaptive_max_rps": 5000,
  "priority_shed_thresholds": {"P3": 0.7, "P2": 0.85, "P1": 0.95},
  "circuit_breaker_config": {
    "error_threshold": 0.01,
    "latency_threshold_ms": 200,
    "window_seconds": 5,
    "recovery_time_seconds": 60
  },
  "quota_config": null,
  "graceful_degradation_config": {
    "strategy": "cached_response",
    "cache_ttl_seconds": 30,
    "fallback_value": {"fraud_score": 0, "allow": true}
  }
}

Response 201 Created:
{ "id": 42, "version": 1, ... }
```

---

**PUT /v1/throttle/policies/{policy_id}**

Update a policy. Must include the current `version` for optimistic locking.

```
PUT /v1/throttle/policies/7
Authorization: Bearer <admin_token>
Content-Type: application/json

{
  "version": 3,
  "max_rps": 12000,
  "adaptive_max_rps": 12000
}

Response 200 OK:
{ "id": 7, "version": 4, ... }

Response 409 Conflict (stale version):
{
  "error": "VERSION_CONFLICT",
  "message": "Policy version 3 is outdated. Current version is 4. Re-fetch and retry."
}
```

---

**POST /v1/throttle/override**

Emergency override — immediately change the effective throttle for a service pair. Takes effect within 5 seconds. Used by on-call SREs during incidents.

```
POST /v1/throttle/override
Authorization: Bearer <admin_token>
Content-Type: application/json

{
  "caller_service": "search-service",
  "callee_service": "index-service",
  "override_rps": 1000,
  "override_duration_seconds": 300,
  "reason": "Index service partial outage in us-east-1a; reducing load to stabilize"
}

Response 200 OK:
{
  "override_id": "ovr-uuid",
  "effective_until": "2025-01-01T00:05:00Z",
  "previous_rps": 10000,
  "override_rps": 1000
}
```

---

**POST /v1/throttle/circuit-breaker/reset**

Manually reset a circuit breaker that is stuck in OPEN state.

```
POST /v1/throttle/circuit-breaker/reset
Authorization: Bearer <admin_token>
Content-Type: application/json

{
  "caller_service": "search-service",
  "callee_service": "index-service",
  "reason": "Manual reset after confirmed index-service recovery"
}

Response 200 OK:
{ "status": "reset_initiated", "propagation_seconds": 5 }
```

---

**GET /v1/throttle/status/{caller_service}/{callee_service}**

Inspect the real-time throttle state across all instances of a service pair.

```
GET /v1/throttle/status/search-service/index-service
Authorization: Bearer <admin_token>

Response 200 OK:
{
  "policy_id": 7,
  "effective_rps": 8500,
  "configured_max_rps": 10000,
  "adaptive_current_rps": 8500,
  "circuit_breaker_state": "CLOSED",
  "current_error_rate": 0.021,
  "current_p99_latency_ms": 187,
  "priority_shed_active": false,
  "quota_status": null,
  "instance_count": 47,
  "aggregate_rps_observed": 8312,
  "throttled_rps_last_minute": 0
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Adaptive Throttling

**Problem it solves:** Static throttle limits are set based on assumptions about downstream capacity that may be wrong or may change over time. A static limit of 10,000 RPS to a service that is currently degraded (capable of handling only 3,000 RPS) will cause cascading failures. Conversely, a static limit set conservatively (5,000 RPS) will under-utilize the downstream during healthy periods. Adaptive throttling automatically adjusts the rate limit based on real-time health signals.

#### Approaches Comparison

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **PID Controller** | Continuously adjusts rate based on the error between desired and observed system health signal (e.g., error rate) | Well-understood control theory; stable; tunable via Kp/Ki/Kd gains | Requires careful gain tuning; can oscillate if tuned poorly |
| **AIMD (Additive Increase, Multiplicative Decrease)** | Increase rate by a fixed amount when healthy; halve rate when overloaded (TCP congestion control analog) | Simple; well-proven in TCP; easy to implement | Saw-tooth pattern; slow recovery; not smooth |
| **Gradient Descent on Latency** | Use derivative of latency vs. load curve to find the operating point just below the "knee" of the curve | Can find optimal throughput mathematically | Requires stable latency signal; noisy signals cause instability |
| **BBR-inspired (Bandwidth + RTT)** | Model downstream as a bottleneck link; estimate capacity from throughput/(round-trip-latency) | Works well for network-like bottlenecks | Not directly applicable to CPU-bound services |
| **Google's Client-side Throttling (Req/Req+Reject)** | Accept rate = requests / (requests + rejects) over sliding window | Extremely simple; zero control plane needed; self-correcting | Only reacts to server rejections; slow to adapt to latency degradation |

**Selected: AIMD with EWMA smoothing (primary) + Google's client-side throttling (secondary)**

**Reasoning:**
- AIMD is the industry-proven approach (TCP has used it since 1988). It's stable by design: multiplicative decrease reacts quickly to overload; additive increase allows gradual recovery.
- EWMA (Exponentially Weighted Moving Average) smoothing on the control signal prevents the adaptive algorithm from overreacting to momentary spikes.
- Google's client-side throttling (from Google SRE book, Chapter 21) runs in each client instance as a zero-control-plane fallback. It requires no coordination: the client simply tracks its own accept/reject ratio and self-throttles when rejections are high.

#### Implementation Detail: AIMD Adaptive Throttling

```python
# Pseudocode: Adaptive Threshold Engine (Control Plane, runs every 1 second per service pair)

class AdaptiveThrottleEngine:
    def __init__(self, policy):
        self.min_rps = policy.adaptive_min_rps
        self.max_rps = policy.adaptive_max_rps
        self.current_rps = policy.max_rps  # start at max
        self.error_threshold = policy.circuit_breaker_config.error_threshold
        self.latency_threshold_ms = policy.circuit_breaker_config.latency_threshold_ms

        # AIMD parameters
        self.additive_increase_step = policy.max_rps * 0.05  # 5% of max per healthy second
        self.multiplicative_decrease_factor = 0.5             # halve on overload

        # EWMA smoothing
        self.ewma_alpha = 0.3  # higher = more weight to recent observations
        self.smoothed_error_rate = 0.0
        self.smoothed_latency_ms = 0.0

    def tick(self, observed_metrics):
        """Called every 1 second with aggregated metrics from all instances."""
        error_rate = observed_metrics.error_rate
        p99_latency = observed_metrics.p99_latency_ms

        # Apply EWMA smoothing to reduce noise
        self.smoothed_error_rate = (
            self.ewma_alpha * error_rate +
            (1 - self.ewma_alpha) * self.smoothed_error_rate
        )
        self.smoothed_latency_ms = (
            self.ewma_alpha * p99_latency +
            (1 - self.ewma_alpha) * self.smoothed_latency_ms
        )

        # Determine overload condition
        is_overloaded = (
            self.smoothed_error_rate > self.error_threshold or
            self.smoothed_latency_ms > self.latency_threshold_ms
        )

        if is_overloaded:
            # Multiplicative decrease: quickly reduce load on overloaded downstream
            new_rps = max(
                self.min_rps,
                self.current_rps * self.multiplicative_decrease_factor
            )
            self.current_rps = new_rps
            log.warn("adaptive_throttle: overload detected, reducing rps",
                     service=policy.callee_service,
                     error_rate=self.smoothed_error_rate,
                     latency_ms=self.smoothed_latency_ms,
                     new_rps=new_rps)
        else:
            # Additive increase: slowly recover toward max
            new_rps = min(
                self.max_rps,
                self.current_rps + self.additive_increase_step
            )
            self.current_rps = new_rps

        return self.current_rps
```

**Client-side throttling (in-process, zero control plane needed):**

```go
// ClientSideThrottle implements Google's "accept rate" throttling.
// Reference: Google SRE Book, Chapter 21, "Handling Overload".
type ClientSideThrottle struct {
    mu            sync.Mutex
    requests      int64    // total requests sent in window
    accepts       int64    // requests accepted by server (not rejected)
    windowStart   time.Time
    windowSeconds float64
    kFactor       float64  // conservative factor, 2.0 recommended
}

func (t *ClientSideThrottle) ShouldThrottle() bool {
    t.mu.Lock()
    defer t.mu.Unlock()

    now := time.Now()
    if now.Sub(t.windowStart).Seconds() > t.windowSeconds {
        // Reset sliding window
        t.requests = 0
        t.accepts = 0
        t.windowStart = now
        return false  // fresh window: allow
    }

    if t.requests == 0 {
        return false  // no data: allow
    }

    // Google's formula: probability of throttling =
    //   max(0, (requests - K * accepts) / (requests + 1))
    //
    // When error rate is 0: accepts == requests → probability = max(0, (N - K*N)/(N+1))
    //   With K=2: = max(0, N*(1-2)/(N+1)) = 0 (never throttle when healthy)
    // When 50% rejected: accepts = 0.5 * requests → probability = max(0, (N - K*0.5*N)/(N+1))
    //   With K=2: = max(0, N*(1-1)/(N+1)) = 0 (still no throttle)
    // When 90% rejected: accepts = 0.1 * requests → probability = max(0, (N - 2*0.1*N)/(N+1))
    //   = max(0, N*0.8/(N+1)) ≈ 0.8 (80% of requests pre-rejected locally)

    requests := float64(t.requests)
    accepts  := float64(t.accepts)
    probability := math.Max(0, (requests - t.kFactor*accepts) / (requests + 1))

    // Stochastic pre-rejection: reject this request with probability `probability`
    return rand.Float64() < probability
}

func (t *ClientSideThrottle) RecordRequest(accepted bool) {
    t.mu.Lock()
    defer t.mu.Unlock()
    t.requests++
    if accepted {
        t.accepts++
    }
}
```

#### Interviewer Q&A

**Q1: How do you prevent the adaptive throttle from thrashing (rapidly oscillating between high and low rates)?**
A: EWMA smoothing is the primary defense. With `alpha = 0.3`, a single spike in error rate has 30% weight; the smoothed value converges slowly. For example, if the error rate jumps from 0% to 20% in one second, the smoothed error rate is `0.3 * 0.20 + 0.7 * 0 = 0.06`. If the threshold is 0.05, this triggers a decrease. If the error rate drops back to 0% the next second, the smoothed rate is `0.3 * 0 + 0.7 * 0.06 = 0.042` — below threshold, no further decrease. The AIMD additive increase is slow (5%/sec) while the multiplicative decrease is fast (50%). This asymmetry prevents rapid oscillation: decreases happen quickly (protecting the downstream) but increases happen slowly (giving the downstream time to stabilize before load returns). Additionally, a "cooldown" period (30 seconds minimum between increases after a decrease) prevents premature ramp-up.

**Q2: What if the adaptive throttle incorrectly identifies a healthy downstream as overloaded (false positive)?**
A: False positives in the adaptive throttle cause under-utilization (too conservative), not system failure. The impact is degraded throughput, not an outage. Mitigations: (1) The EWMA smoothing on the error rate signal makes single-event false positives unlikely. (2) The adaptive algorithm monitors multiple signals (error rate AND p99 latency) — overload is only declared when multiple signals agree. (3) The additive increase recovery means that even after a false positive reduction, the rate returns to maximum within `(max_rps - current_rps) / increase_step` seconds. For a 10,000 RPS max, 500 RPS increase step, and a drop to 5,000 RPS: recovery takes 10 seconds. (4) Alerts fire when the adaptive throttle reduces rates significantly, triggering SRE review within minutes.

**Q3: How does the adaptive throttle handle "slow start" for a newly deployed downstream service?**
A: When a new callee service instance starts up, its local caches are cold, JIT compilation is incomplete, and database connection pools are warming up. During this period, response times are high. The adaptive throttle sees elevated latency and reduces the rate — which is correct behavior, preventing request pile-up during warmup. This is actually a desired side effect: it gives the new instance time to warm up before receiving full load. Kubernetes readiness probes should delay adding a new instance to the load balancer until it's warm, but the adaptive throttle provides a second layer of protection. In practice, set `adaptive_min_rps` to 10-20% of max to ensure the new instance receives some traffic for warmup even during the "overload" detection phase.

**Q4: What metrics does the adaptive algorithm require, and how do you collect them with < 1 second latency?**
A: Required metrics: (a) error rate (HTTP 5xx count / total count over last 10 seconds), (b) p99 latency (histogram over last 10 seconds), (c) optional: queue depth of the callee service. Collection mechanism: each service instance maintains in-process histograms (via HDRHistogram or Prometheus client). Every 1 second, each instance exports a summary (total_requests, error_count, latency_histogram_buckets) to the control plane's Metrics Aggregator via a gRPC stream (NOT REST — gRPC streaming has sub-millisecond overhead vs. REST's connection overhead). The Aggregator merges summaries from all instances (histogram merging is additive — you can add histogram buckets across instances). Total pipeline latency: metric recorded → 1s flush → gRPC transport (~1ms) → aggregation (~10ms) → adaptive tick → threshold push (~1ms) = ~2 seconds end-to-end. This meets the 5-second propagation requirement.

**Q5: How would adaptive throttling behave during a "slow drain" scenario — a downstream that gradually degrades over 30 minutes rather than failing suddenly?**
A: The EWMA with alpha=0.3 is excellent for gradual degradation. Each second, the smoothed error rate or latency drifts slightly upward tracking the actual signal. The adaptive algorithm begins reducing the rate once the signal crosses the threshold. The multiplicative decrease kicks in — possibly reducing to minimum before the downstream finally fails completely. This provides a graceful degradation curve rather than a cliff. The SRE on-call receives gradual alerts: "throttle_rate reduced to 80% of max" → "70% of max" → "50% of max" → eventually a circuit breaker opens. This gives the SRE a natural "heads up" window rather than being surprised by a sudden outage.

---

### 6.2 Priority Queuing with Graceful Degradation

**Problem it solves:** When the system is overloaded, not all requests are equally important. A payment processing request is more critical than a recommendation carousel load. Without priority queuing, uniform shedding would drop payment requests at the same rate as trivial background jobs. Priority queuing ensures critical work always completes while low-priority work is shed first.

#### Approaches Comparison

| Approach | Pros | Cons | Best For |
|---|---|---|---|
| **Multi-level priority queue (MLFQ)** | Simple; well-understood; P0 always processed first | Can starve P3 indefinitely if P0/P1 load is high | Requests with clear, static priority classes |
| **Weighted fair queuing** | P3 always gets some fraction of capacity; no starvation | More complex; doesn't give P0 absolute priority | Mixed workloads where all priorities need some service |
| **Work stealing** | P2/P3 consumers help drain P0/P1 when overloaded | Complex implementation; consumer must be re-entrant | CPU-intensive, parallelizable tasks |
| **Deadline-based scheduling** | Each request has a deadline; closest deadline processed first (EDF) | Requires accurate deadline estimation; complex | Real-time systems with strict latency SLAs |

**Selected: Multi-level priority queue (4 levels, P0-P3) with starvation prevention**

**Reasoning:** API requests have clear, metadata-derived priority (payment = P0, user-facing = P1, background = P3). Absolute priority guarantees for P0 are a hard business requirement (we MUST process payments even during overload). The starvation prevention mechanism ensures P3 batch jobs eventually make progress even under sustained P0 load.

#### Implementation Detail

```go
// MultiLevelQueue implements a 4-priority bounded queue with starvation prevention.
type MultiLevelQueue struct {
    mu           sync.Mutex
    cond         *sync.Cond
    queues       [4][]Request          // P0=0, P1=1, P2=2, P3=3
    maxDepths    [4]int                // per-priority max depth
    totalMax     int                   // total max across all priorities
    total        int                   // current total
    starvation   [4]int64              // time (ms) since last item of this priority was dequeued
    maxStarvation [4]int64             // starvation limits: P0=∞, P1=5000ms, P2=30000ms, P3=120000ms
}

func NewMultiLevelQueue(maxTotal int) *MultiLevelQueue {
    mlq := &MultiLevelQueue{
        queues:    [4][]Request{},
        maxDepths: [4]int{maxTotal/2, maxTotal/2, maxTotal/4, maxTotal/4},  // P0+P1 get more capacity
        totalMax:  maxTotal,
        maxStarvation: [4]int64{
            0,       // P0: no starvation limit (process immediately)
            5000,    // P1: max 5 seconds without processing
            30000,   // P2: max 30 seconds without processing
            120000,  // P3: max 2 minutes without processing
        },
    }
    mlq.cond = sync.NewCond(&mlq.mu)
    return mlq
}

// Enqueue adds a request to the appropriate priority queue.
// Returns ErrQueueFull if this priority level's queue is at capacity.
func (q *MultiLevelQueue) Enqueue(req Request, priority int) error {
    q.mu.Lock()
    defer q.mu.Unlock()

    if q.total >= q.totalMax {
        // System-wide max: shed the lowest-priority request currently queued
        if priority < q.lowestQueuedPriority() {
            q.shedLowestPriority()  // evict one low-priority item to make room
        } else {
            return ErrQueueFull  // this request is lower priority than what's already queued
        }
    }

    if len(q.queues[priority]) >= q.maxDepths[priority] {
        return ErrPriorityLevelFull
    }

    q.queues[priority] = append(q.queues[priority], req)
    q.total++
    q.cond.Signal()  // wake up a dequeue caller
    return nil
}

// Dequeue returns the highest-priority request that is ready.
// Considers starvation: a starved lower-priority item may "jump" ahead.
func (q *MultiLevelQueue) Dequeue() Request {
    q.mu.Lock()
    defer q.mu.Unlock()

    for {
        // Check starvation for each priority level
        nowMs := time.Now().UnixMilli()
        for p := 3; p >= 1; p-- {  // check from lowest priority upward
            if len(q.queues[p]) > 0 && q.maxStarvation[p] > 0 {
                if nowMs - q.starvation[p] > q.maxStarvation[p] {
                    // This priority level is starved: promote one item
                    return q.dequeueFrom(p, nowMs)
                }
            }
        }

        // Normal dequeue: highest priority first (P0 → P1 → P2 → P3)
        for p := 0; p <= 3; p++ {
            if len(q.queues[p]) > 0 {
                return q.dequeueFrom(p, nowMs)
            }
        }

        // Queue is empty: wait
        q.cond.Wait()
    }
}

func (q *MultiLevelQueue) dequeueFrom(priority int, nowMs int64) Request {
    req := q.queues[priority][0]
    q.queues[priority] = q.queues[priority][1:]
    q.total--
    q.starvation[priority] = nowMs  // reset starvation timer
    return req
}

func (q *MultiLevelQueue) lowestQueuedPriority() int {
    for p := 3; p >= 0; p-- {
        if len(q.queues[p]) > 0 {
            return p
        }
    }
    return 0
}

func (q *MultiLevelQueue) shedLowestPriority() {
    for p := 3; p >= 0; p-- {
        if len(q.queues[p]) > 0 {
            shed := q.queues[p][len(q.queues[p])-1]  // shed the last (oldest) item
            q.queues[p] = q.queues[p][:len(q.queues[p])-1]
            q.total--
            metrics.Counter("throttle.shed.priority_eviction").
                WithLabel("priority", strconv.Itoa(p)).Inc(1)
            emitShedEvent(shed, "priority_eviction")
            return
        }
    }
}
```

**Graceful Degradation Integration:**

When a request is shed (dropped from the queue), instead of returning an error, the throttling service can return a cached or default response if the policy specifies graceful degradation:

```go
// GracefulDegradationHandler returns a degraded response instead of an error.
func (g *GracefulDegradationHandler) HandleShed(ctx context.Context, req Request, policy ThrottlePolicy) Response {
    if policy.GracefulDegradation == nil {
        return ErrorResponse(429, "SERVICE_OVERLOADED", "Request shed due to downstream overload")
    }

    switch policy.GracefulDegradation.Strategy {
    case "cached_response":
        cacheKey := g.cacheKeyFor(req)
        if cached, ok := g.cache.Get(cacheKey); ok {
            return cached.(Response).WithHeader("X-Throttle-Degraded", "cached")
        }
        // No cached response available: fall through to default

    case "default_value":
        return Response{
            Status: 200,
            Body:   policy.GracefulDegradation.FallbackValue,
            Headers: map[string]string{
                "X-Throttle-Degraded": "default",
                "X-Throttle-Reason":   "downstream_overloaded",
            },
        }

    case "partial_response":
        // Return a subset of the response (e.g., first page of results from cache)
        return g.buildPartialResponse(req, policy)
    }

    return ErrorResponse(503, "SERVICE_OVERLOADED", "Request shed: no cached fallback available")
}
```

#### Interviewer Q&A

**Q1: How do you classify request priority without requiring the caller to self-report their priority? (Self-reporting is gameable.)**
A: Priority classification should not rely solely on caller-provided hints. The priority classifier uses a rule engine with the following signal hierarchy (in order of trust): (1) **Endpoint-based rules** (hardcoded in policy): `POST /v1/payment` → P0 always, regardless of what the caller says. Endpoints are registered with their default priority in the service catalog. (2) **Authenticated user tier** (verified by auth token, not caller-provided): enterprise users' requests are P1; free tier users' background jobs are P3. (3) **Request metadata** (request headers verified by the auth middleware, not the caller's HTTP headers): `X-Request-Type: batch` (if set by a verified internal service) → P3. (4) **Rate-limited shedding order**: if a caller has already been throttled once in this second, their next request in the same second is demoted one priority class. The key principle: **trust signals that the caller cannot forge**. Endpoint paths and verified identity tokens meet this bar; arbitrary request headers from external callers do not.

**Q2: What happens to requests sitting in the P3 queue if the system is overloaded for an extended period? Don't they accumulate indefinitely?**
A: No — the queue is bounded in two ways: (1) **Per-priority depth limits**: `maxDepths[3]` (P3) is set to 25% of total queue capacity. Once the P3 queue is full, new P3 enqueue attempts fail immediately with `ErrPriorityLevelFull`. Callers should implement their own retry logic (exponential backoff). (2) **Total queue depth limit**: when the system-wide total reaches `totalMax`, the `shedLowestPriority()` function evicts the oldest item from the lowest-occupied priority level to make room for a higher-priority item. (3) **Request TTL**: each request in the queue has an enqueue timestamp and a `max_wait_ms` field. The dequeue loop checks whether a request has exceeded its TTL; if so, it's shed with a `queue_timeout` reason rather than processed. For P3 batch requests, a 30-second queue TTL is typical — the batch job client should handle the 429/503 response and resubmit later.

**Q3: How do you handle priority inversion — a P0 request that depends on a P3 database operation to complete?**
A: Priority inversion is a classic distributed systems problem. Mitigations: (1) **Priority inheritance**: when a P0 request triggers a call to a downstream service, that downstream call inherits the P0 priority. This is implemented by propagating a priority context header (e.g., `X-Request-Priority: 0`) through the call chain. Each service's throttle client reads this header and assigns the propagated priority to the outbound request. (2) **Dedicated P0 resource pools**: critical downstream services (e.g., the payment database) maintain separate connection pools reserved for P0 traffic. P3 traffic uses a different connection pool. This prevents P3 database queries from consuming connection slots needed by P0 operations. (3) **Non-blocking P0 paths**: design P0 request handlers to avoid dependencies on work that can be deferred (e.g., logging, audit writes, cache updates). These are done asynchronously after the P0 response is sent.

**Q4: How does the priority queue interact with the backpressure mechanism?**
A: They are complementary, not competing: (1) **Backpressure** controls the RATE of requests entering the queue — the outbound token bucket limits how fast new requests are sent to the downstream. (2) **Priority queue** controls WHICH requests are processed when the system is under load — it determines the order and which requests are shed. When backpressure is active (token bucket throttling the outbound rate), the priority queue buffers work that is waiting for a token. The buffer absorbs bursts: if 1,000 P0 requests arrive in 1 second but the token bucket allows only 500/sec, the 500 excess P0 requests queue up and are processed in the next second. Meanwhile, any P3 requests that arrive are added to the P3 queue. If the P3 queue is already full, P3 requests are shed immediately without queuing. The combined effect: P0 is always processed (just possibly slightly delayed), P3 is shed under load. This is the correct behavior.

**Q5: Can you explain exactly how "graceful degradation with cached response" works in a multi-instance environment?**
A: Each service instance maintains its own response cache (in-process LRU). When a request succeeds, the result is stored in the cache with a TTL equal to `cache_ttl_seconds` from the policy. When the throttling service sheds a request and the degradation strategy is `cached_response`, it looks up the cache key (typically derived from the request path + stable query parameters). If a cached entry exists and is within TTL, it returns that cached response with `X-Throttle-Degraded: cached` header. The per-instance cache means: (a) the cache doesn't require network access — it's entirely in-process; (b) cache hit rates vary by instance based on their request history; (c) cache freshness is bounded by the TTL, so degraded responses are at most `cache_ttl_seconds` stale. For services where stale data is unacceptable (e.g., payment balance checks), `graceful_degradation_config = null` — the policy returns a hard error on shed. For services where approximate data is better than an error (e.g., product recommendation scores), a 30-second cache TTL is appropriate.

---

### 6.3 Backpressure Mechanisms

**Problem it solves:** In a microservice call chain (A → B → C → D), if D becomes overloaded, B and C will accumulate pending requests (each waiting for D to respond). Their internal queues fill up; they start rejecting requests. A continues sending requests to B, accumulating its own queue. Without backpressure, every service in the chain fills its queues and eventually crashes — cascading failure. Backpressure propagates the "slow down" signal upstream so callers reduce their send rate before queues fill up.

#### Backpressure Signal Sources

| Signal | Description | Latency to Detect | Action |
|---|---|---|---|
| **HTTP 429 / 503 from downstream** | Explicit rejection from downstream | Immediate | Reduce token bucket rate; increment reject count for client-side throttle |
| **Increased response latency** | Downstream is processing slowly (queue building up) | 100-500 ms | Adaptive throttle reduces rate when p99 > threshold |
| **Queue depth metric published by downstream** | Downstream explicitly advertises its queue depth | < 5s (via control plane) | Adaptive throttle preemptively reduces rate |
| **gRPC RESOURCE_EXHAUSTED status** | gRPC-native overload signal | Immediate | Same as HTTP 429 |
| **TCP backpressure (receive buffer full)** | OS-level signal when remote buffer is full | Milliseconds | Manifests as slow `send()` syscalls; proxy layer should honor this |
| **Custom load header** | Downstream sets `X-Load: 0.87` in responses | Per-response | Client reads header; adjusts send rate |

#### Implementation: Token Bucket with Dynamic Rate from Adaptive Throttle

The outbound throttle client (running in the calling service) combines the token bucket (rate control) with the adaptive throttle signal and the circuit breaker:

```go
// OutboundThrottleClient controls the rate of outbound calls to a single downstream.
type OutboundThrottleClient struct {
    tokenBucket      *TokenBucket
    circuitBreaker   *CircuitBreaker
    clientThrottle   *ClientSideThrottle
    quotaCounter     *QuotaCounter
    policy           ThrottlePolicy
    metrics          *ThrottleMetrics
}

// Allow checks whether an outbound request to the downstream is permitted.
// Must be called before each outbound request.
func (c *OutboundThrottleClient) Allow(ctx context.Context, req Request) (bool, ThrottleReason) {
    // Step 1: Check circuit breaker first (fastest check, in-process)
    if !c.circuitBreaker.Allow() {
        c.metrics.Record("circuit_open")
        return false, ReasonCircuitOpen
    }

    // Step 2: Check client-side throttle (pre-rejection based on recent reject ratio)
    if c.clientThrottle.ShouldThrottle() {
        c.metrics.Record("client_side_throttle")
        return false, ReasonClientSideThrottle
    }

    // Step 3: Check quota (daily/hourly limits)
    if c.quotaCounter != nil && !c.quotaCounter.Allow() {
        c.metrics.Record("quota_exceeded")
        return false, ReasonQuotaExceeded
    }

    // Step 4: Check token bucket (rate limiting)
    if !c.tokenBucket.TryConsume(1) {
        c.metrics.Record("rate_limited")
        return false, ReasonRateLimited
    }

    return true, ReasonNone
}

// RecordResponse updates the throttle state based on the downstream's response.
// Must be called after each outbound request completes.
func (c *OutboundThrottleClient) RecordResponse(statusCode int, latencyMs float64, err error) {
    isError := err != nil || statusCode >= 500 || statusCode == 429
    isAccepted := !isError

    // Update client-side throttle's accept/reject ratio
    c.clientThrottle.RecordRequest(isAccepted)

    // Update circuit breaker
    c.circuitBreaker.Record(isError, latencyMs)

    // If downstream returned a load hint, update token bucket rate
    // (In practice, this would be extracted from response headers by the HTTP transport layer
    //  and passed here as an optional argument)
}
```

**Token Bucket with dynamic rate:**

```go
// TryConsume attempts to consume a token. Thread-safe, non-blocking.
// Returns true if a token was available; false if the bucket is empty.
func (tb *TokenBucket) TryConsume(n float64) bool {
    tb.mu.Lock()
    defer tb.mu.Unlock()

    now := time.Now()
    elapsed := now.Sub(tb.LastRefill).Seconds()
    tb.LastRefill = now

    // Refill tokens based on elapsed time
    tb.Tokens = math.Min(tb.BurstSize, tb.Tokens + elapsed*tb.Rate)

    if tb.Tokens >= n {
        tb.Tokens -= n
        return true  // token available: allow request
    }

    return false  // bucket empty: deny request
}

// UpdateRate is called by the control plane push to update the token bucket rate.
// The rate change is applied immediately (next TryConsume call will use the new rate).
func (tb *TokenBucket) UpdateRate(newRate float64) {
    tb.mu.Lock()
    defer tb.mu.Unlock()
    // Refill up to the current time before changing the rate (avoid stale refill calculation)
    elapsed := time.Now().Sub(tb.LastRefill).Seconds()
    tb.Tokens = math.Min(tb.BurstSize, tb.Tokens + elapsed*tb.Rate)
    tb.LastRefill = time.Now()
    tb.Rate = math.Max(tb.MinRate, math.Min(tb.MaxRate, newRate))
}
```

#### Interviewer Q&A

**Q1: How do you prevent backpressure from propagating too aggressively and causing a "backpressure cascade" that shuts down the entire system?**
A: The backpressure cascade is a real risk (sometimes called "cascading backpressure" or "overload propagation"). Mitigations: (1) **Multiplicative decrease with a floor**: the adaptive throttle never reduces below `adaptive_min_rps` (e.g., 10% of max). This ensures the downstream always receives some traffic — enough for it to process and recover, but not enough to be overwhelmed. (2) **Priority-aware backpressure**: backpressure only sheds P2/P3 traffic; P0/P1 always flows through (at a reduced rate). This prevents critical business operations from being caught in the cascade. (3) **Decoupled backpressure domains**: backpressure is scoped to the (caller, callee) pair. Service A's backpressure to Service B does not directly affect Service A's backpressure to Service C. This limits the blast radius. (4) **Timeout-based release**: if the system has been in a throttled state for > 5 minutes without recovery, the adaptive algorithm resets to 50% of max (not minimum) to allow the system to break out of a "deadlock" where all services are at minimum rate and none are making progress.

**Q2: What's the difference between backpressure and rate limiting? Aren't they the same thing?**
A: They're related but distinct in direction and mechanism. **Rate limiting** is enforced at the receiver (or a proxy in front of the receiver) — it says "I will not accept more than N requests/second, regardless of how many you send." It's a hard boundary. **Backpressure** is a signal sent FROM the receiver to the sender saying "please slow down" — it relies on the sender to honor the signal. In practice: rate limiting is the fallback when backpressure fails (the sender doesn't respect the backpressure signal, or the signal is lost). Backpressure is the preferred mechanism because it works proactively before queues fill up, rather than reactively after requests are already rejected. In TCP, backpressure is implemented via the receive window size (the receiver tells the sender how much buffer is available). In our system, backpressure is the adaptive throttle signal propagated via the control plane; rate limiting is the token bucket that enforces it.

**Q3: How do you handle backpressure for event-driven (async) communication — e.g., a Kafka producer?**
A: Kafka's built-in `max.in.flight.requests.per.connection`, `buffer.memory`, and `linger.ms` settings provide basic producer-side flow control. For adaptive backpressure on top: (1) **Consumer lag monitoring**: the throttling service monitors Kafka consumer lag (messages in topic minus consumer offset). When lag exceeds a threshold (e.g., > 100K messages), the control plane pushes a "slow down" signal to all producers writing to that topic. (2) **Producer-side pause**: the Kafka producer client in each service instance receives the slow-down signal and calls `producer.pause()` on the topic partitions, or reduces the production rate via a token bucket that wraps the `producer.send()` call. (3) **Priority-aware production**: map message priority to Kafka partition or topic; under backpressure, only produce to P0/P1 topics. (4) The signal flow: consumer lag → control plane detects → pushes "reduce rate" to producers → producers apply token bucket to `producer.send()` → lag decreases. This is analogous to TCP congestion control but for a message queue.

**Q4: How does the throttling service interact with a service mesh like Istio/Envoy? Isn't Envoy already doing rate limiting?**
A: Envoy and the throttling service operate at different levels and are complementary: (1) **Envoy (L7 proxy)**: enforces connection limits, circuit breaking at the TCP connection level, and basic HTTP rate limiting (via the ext_authz or ratelimit filter). Envoy operates at the proxy sidecar level — it can limit requests before they reach the service's application code. (2) **Throttling service (application level)**: enforces adaptive limits based on application-level health signals (business logic errors, not just HTTP status codes), priority-based queuing (Envoy doesn't queue by priority), graceful degradation (return cached response instead of 429), and quota management. The recommended architecture: Envoy handles L7 connection management and coarse-grained rate limits; the throttling service handles fine-grained, adaptive, priority-aware application throttling. They're not redundant — Envoy is a safety net at the network level; the throttling service is the intelligent controller at the application level.

**Q5: A service is in a degraded state but not fully down. How does the throttling service distinguish "slow downstream" from "overloaded downstream" from "legitimate high-complexity request"?**
A: This is the "performance vs. overload" disambiguation problem. Approaches: (1) **Baseline latency profiling**: the throttling service maintains a rolling p50 and p99 latency baseline for each (caller, callee, endpoint) tuple over the last hour. A latency increase is only considered "overload" if it exceeds 2× the baseline p99. If the p99 was always 400 ms (e.g., complex ML inference endpoint), a 500 ms response is not overload — it's within normal variance. (2) **Error rate as a primary signal**: overloaded services typically show increased error rates (OOM errors, connection timeouts, queue full errors) in addition to higher latency. Increased latency WITHOUT increased error rates suggests legitimate high-complexity requests (or a slow network), not overload. (3) **Request classification**: the throttling service can classify requests by complexity tier (e.g., `simple`, `complex`, `batch`) using metadata. Latency thresholds are applied per complexity tier. A `complex` request taking 1 second is not overload; a `simple` request taking 1 second is a red flag.

---

## 7. Scaling

### Horizontal Scaling

**Service instances (the in-process throttle clients):** Scale horizontally — each new service instance initializes its own in-process state (token buckets, circuit breakers). Within 5 seconds of startup, the control plane pushes current thresholds to the new instance via gRPC stream. The in-process state requires no coordination between instances — consistency is handled by the control plane propagating the same thresholds to all instances.

**Control plane:** The Adaptive Threshold Engine is a stateful service (it maintains EWMA state per service pair). It can be scaled horizontally by partitioning service pairs: control plane instance 0 handles service pairs A-M; instance 1 handles N-Z. A consistent hash ring maps (caller, callee) pairs to control plane instances. Each instance pushes thresholds only for its assigned service pairs. For 500 service pairs at 500 ops/sec, a single control plane instance handles this easily; horizontal scaling is only needed at ~100K service pairs.

**Metrics Aggregator:** Scales horizontally — incoming metric streams from service instances are partitioned by `caller_service` across aggregator instances. Since metrics are additive (histograms can be merged), this is embarrassingly parallel.

**Push Gateway:** Maintains persistent gRPC streams to ~10,000 service instances. Each gRPC stream is lightweight (~1 KB). A single Push Gateway process handles ~10,000 persistent streams on modern hardware (Go's goroutine model, each stream = 1 goroutine with 2 KB stack = 20 MB total). Horizontal scaling by partitioning instances across Push Gateway nodes.

### DB Sharding

PostgreSQL (Policy Store) does not require sharding — 500 service pairs, 20 KB each = 10 MB total. A single PostgreSQL instance with a read replica handles this trivially.

VictoriaMetrics (Metrics Store) handles time-series natively with cluster mode for horizontal scaling. For 100,000 metrics/sec at 50 bytes each = 5 MB/s write throughput — within single-node capacity. Cluster mode can be added for high availability and retention scaling.

### Replication

PostgreSQL: 1 primary + 2 replicas. Admin writes to primary; control plane reads from primary (for consistency — we don't want the adaptive engine reading stale policy from a replica). Low read volume makes replication lag irrelevant.

VictoriaMetrics: 1 primary + 1 replica. The replica provides fault tolerance for metric history needed by the adaptive algorithm. 1-hour history at 5 MB/s = 18 GB per hour — easily within a single server.

### Caching

In-process policy cache: Each service instance caches its throttle policy (fetched from the control plane at startup and refreshed on push updates). No network lookup needed per request.

Control plane's policy cache: The adaptive engine caches policies from PostgreSQL in-process. Cache is invalidated on admin updates via an event published to a Redis Pub/Sub channel.

### Scaling Q&A

**Q1: What happens when a service instance restarts? How does it initialize its throttle state?**
A: On startup, the instance: (1) Fetches its throttle policies from the control plane via a gRPC `GetPolicies(caller_service=self)` call. Timeout: 3 seconds. If the control plane is unreachable, fall back to policies embedded in the service's deployment configuration (a static YAML file checked into the repo and bundled in the container image). (2) Initializes token buckets at 50% of max_rps (conservative start to avoid overwhelming downstream during service restart). (3) Opens a gRPC stream to the Push Gateway for subsequent threshold updates. (4) Initializes circuit breakers in CLOSED state. After 30 seconds of operation, the adaptive algorithm has enough data to calibrate the circuit breaker and token bucket to the actual downstream health. This "conservative start" approach prevents a restarting service from immediately flooding its downstream.

**Q2: How does the throttling service handle a control plane outage? All the adaptive logic stops?**
A: Yes — the adaptive logic pauses, but the system continues operating safely: (1) All service instances continue using their last-known thresholds (stored in-process). (2) The client-side throttle (Google's accept-rate formula) continues operating entirely in-process without any control plane input — it self-adjusts based on the downstream's response codes. (3) Circuit breakers continue operating in-process — they'll open if the downstream starts failing. (4) The control plane is stateless in terms of request handling (all throttle decisions happen in-process). The control plane's only role during steady-state is updating thresholds — losing it for 30 minutes means the thresholds don't adapt during that window, but the system doesn't fail. (5) Alert fires immediately when the control plane is unreachable from service instances; on-call SRE investigates.

**Q3: With 10,000 service instances each sending metrics every 1 second, how do you prevent the Metrics Aggregator from becoming a bottleneck?**
A: The aggregator is designed for this exact load: (1) **gRPC streaming, not polling**: each instance maintains a persistent gRPC stream to its aggregator partition. Overhead is a keep-alive heartbeat; metric payloads are small histogram snapshots (< 1 KB each). (2) **Pre-aggregation at the source**: each instance reports a pre-aggregated summary (total requests, error count, histogram bucket counts) rather than raw events. This reduces payload size by 100-1000×. (3) **Batching**: metrics are flushed every 1 second, not per-request. With 10,000 instances × 1 flush/sec × 1 KB = 10 MB/s — easily handled by a single aggregator node (which has > 10 GB/s memory bandwidth). (4) **Partition by caller service**: aggregator instance 0 receives metrics only from instances of service A-M. This splits the load across aggregator nodes and simplifies the aggregation logic (no cross-partition joins needed).

**Q4: How do you scale the priority queue when a single in-process queue per service instance isn't sufficient?**
A: In-process priority queues scale with the service instance's CPU. Each service instance has one queue per downstream dependency (not one global queue). If a single instance's queue is too slow: (1) **Separate goroutine pools per priority**: P0 requests are processed by a dedicated goroutine pool (e.g., 10 goroutines always reserved for P0). P3 requests use a shared pool that's sized dynamically. This is work-stealing with priority awareness. (2) **Multiple queue levels**: for very high-throughput services (> 100K RPS per instance), implement a lock-free priority queue using atomic operations on ring buffers (one ring buffer per priority level). This eliminates mutex contention in the queue's hot path. (3) In practice, the bottleneck is never the queue implementation — it's the downstream throughput. If 100K requests are queuing, the downstream is severely degraded, and shedding (not better queuing) is the correct response.

**Q5: How would you scale the Push Gateway to handle 100,000 service instances instead of 10,000?**
A: At 100,000 instances, each gRPC stream ~4 KB overhead = 400 MB of connection state. With Go's goroutine model (2 KB stack per goroutine + 4 KB stream overhead = 6 KB per stream), 100,000 streams = 600 MB — still fits on a single 4 GB server. However, the Push Gateway becomes a single point of failure. Scaling approach: (1) **Shard Push Gateway by service name**: instances of `search-service` connect to Push Gateway shard 0; `payment-service` connects to shard 1. Consistent hashing determines which shard. (2) **Hierarchical fan-out**: Push Gateway publishes threshold updates to a Redis Pub/Sub channel; local relay agents on each host subscribe and fan out to instances on their host. This reduces the Push Gateway's connection count from 100,000 to N_hosts × 1. (3) **Pull-based fallback**: if a Push Gateway shard is unavailable, instances fall back to polling the control plane's `GetCurrentThresholds` endpoint every 5 seconds. This increases load on the control plane but ensures forward progress.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Control plane fully down | Adaptive thresholds frozen at last-known values; no new policy updates | Control plane health check; instances report "push gateway unreachable" metric | Instances continue with cached thresholds; client-side throttle + circuit breakers handle in-process; alert fires |
| Push Gateway shard failure | ~1/N of service instances don't receive threshold updates | Missing heartbeat on gRPC stream; instance-side timeout | Instances fall back to 5s polling of control plane REST API; other Push Gateway shards unaffected |
| Single service instance crash | In-progress queued requests lost; those requests must be retried | Load balancer health check removes instance from rotation | Stateless design: other instances handle traffic; callers retry with exponential backoff |
| Downstream service complete failure | All calls to downstream fail; circuit breaker opens within 10 seconds | Circuit breaker error rate > threshold; 100% error rate | Circuit breaker OPEN: stop sending to downstream; return graceful degraded response or 503; alert fires |
| Kafka audit pipeline failure | Throttle events not logged; audit gap | Kafka producer error metric | Throttle decisions still enforced (Kafka is non-blocking); events spilled to local disk buffer (in-process ring buffer), retried when Kafka recovers |
| PostgreSQL policy store failure | Cannot update policies; control plane serves cached policies | DB connection error metric | Control plane serves in-memory cache of last-fetched policies (refreshed every 60s); admin updates blocked; alert fires |
| Clock skew between instances | EWMA timestamp calculations slightly off; negligible impact | NTP monitoring | Use NTP; EWMA is aggregated across instances so individual clock skew averages out |
| Priority queue overflow (all levels full) | All incoming requests rejected; service appears down | Queue depth metric > maxDepths; rejection rate spike | Shed P3 immediately; if P2 full, shed P2; circuit breaker on callee may open; alert fires; auto-scale trigger |

### Failover Design

**Circuit Breaker State Machine:**

```
                    error_rate > threshold
         ┌─────────────────────────────────────────┐
         │                                         │
         ▼                                         │
     ┌───────┐    ──── all requests fail ────    ┌──────┐
     │       │                                   │      │
     │CLOSED │                                   │ OPEN │
     │       │                                   │      │
     └───────┘                                   └──────┘
         ▲                                          │
         │   probe success                          │ after recovery_timeout
         │ ◄──────────────────────────────          │
         │                                          ▼
         │                                    ┌───────────┐
         └────────────────────────────────────│ HALF_OPEN │
              probe fails → back to OPEN      │           │
                                              └───────────┘
                                              1% of requests sent as probes
```

In HALF_OPEN, only 1% of requests are sent to the downstream as probes. If the probe succeeds (status < 500, latency < threshold), the circuit transitions to CLOSED and rate gradually ramps up via the adaptive throttle's additive increase. If the probe fails, the circuit returns to OPEN, and the recovery timer restarts.

### Retries and Idempotency

**Retry policy for throttled requests:**
- Throttled requests (token bucket empty, queue full) should NOT be immediately retried — they should be retried with exponential backoff + jitter.
- Recommended backoff: `base_delay * 2^attempt + random_jitter`, where `base_delay = 100ms`, max retries = 3.
- The throttling client SDK includes a built-in retry loop that callers can opt into.

**Idempotency:**
- Non-idempotent requests (e.g., `POST /payment`) must NOT be retried without a client-provided idempotency key.
- The throttling service passes through the `Idempotency-Key` header; the downstream service is responsible for deduplication.
- For retries of throttled requests, the same idempotency key is used — ensuring a retry after a throttle doesn't create duplicate operations.

### Circuit Breaker Tuning Guidelines

| Service Type | Error Threshold | Latency Threshold | Window | Recovery Time |
|---|---|---|---|---|
| Critical payment | 1% | 200 ms | 5s | 60s |
| Core API services | 5% | 500 ms | 10s | 30s |
| Recommendation/ML | 10% | 2,000 ms | 30s | 15s |
| Background batch | 20% | 10,000 ms | 60s | 10s |

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Labels | Alert Threshold |
|---|---|---|---|
| `throttle.requests.allowed` | Counter | `caller`, `callee`, `priority` | — |
| `throttle.requests.throttled` | Counter | `caller`, `callee`, `priority`, `reason` | Alert if `reason=circuit_open` > 0 |
| `throttle.requests.shed` | Counter | `caller`, `callee`, `priority` | Alert if any P0/P1 shed events |
| `throttle.queue.depth` | Gauge | `caller`, `callee`, `priority` | Alert if > 80% of max depth |
| `throttle.queue.wait_ms` | Histogram | `caller`, `callee`, `priority` | Alert if P0 p99 > 100 ms |
| `throttle.circuit_breaker.state` | Gauge | `caller`, `callee`, `state` | Alert if state = OPEN |
| `throttle.circuit_breaker.error_rate` | Gauge | `caller`, `callee` | Alert if > error_threshold |
| `throttle.adaptive.current_rps` | Gauge | `caller`, `callee` | Alert if < 50% of max_rps for > 60s |
| `throttle.adaptive.rps_change` | Gauge | `caller`, `callee`, `direction` | Alert on rapid decrease (> 30% in 10s) |
| `throttle.token_bucket.tokens` | Gauge | `caller`, `callee` | Alert if consistently 0 (permanently throttled) |
| `throttle.client_side.reject_probability` | Gauge | `caller`, `callee` | Alert if > 0.5 for > 30s |
| `throttle.quota.utilization_pct` | Gauge | `caller`, `callee`, `window` | Alert if > 90% |
| `throttle.graceful_degradation.cache_hit_rate` | Gauge | `caller`, `callee` | Alert if < 20% during degradation |
| `throttle.control_plane.push_latency_ms` | Histogram | `push_gateway_shard` | Alert if p99 > 1,000 ms |
| `throttle.control_plane.instance_push_failures` | Counter | `instance_id` | Alert if > 0 for > 30s |

### Distributed Tracing

Every throttle decision adds a span to the request trace:

```
Trace: POST /v1/payment (total: 87ms)
  ├── [Auth]                  validate_token: 1ms
  ├── [Throttle - Inbound]    admission_control: 0.1ms
  │     ├── priority_classify: P0
  │     ├── circuit_breaker: CLOSED (fraud-check-service)
  │     └── quota_check: OK (usage=4512/5000 daily)
  ├── [Payment Handler]       process_payment: 85ms
  │     ├── [Throttle - Outbound to fraud-check]  allow: 0.05ms
  │     │     ├── token_bucket: tokens=482 → 481
  │     │     └── client_throttle: reject_prob=0.02 → ALLOW
  │     ├── fraud_check_service.Check: 12ms
  │     └── db.commit_payment: 73ms
  └── [Throttle - Outbound to audit-service]  allow: 0.05ms (async, not in critical path)
```

All spans tagged with `throttle.priority`, `throttle.decision`, `throttle.reason`, `throttle.tokens_remaining`.

### Logging

**Structured log format for throttle events:**

```json
{
  "ts": "2025-01-01T00:00:00.001Z",
  "level": "warn",
  "msg": "request_throttled",
  "caller_service": "search-service",
  "callee_service": "index-service",
  "priority": 2,
  "decision": "shed",
  "reason": "priority_shed_p2_threshold_exceeded",
  "current_rps": 9812,
  "max_rps": 10000,
  "shed_threshold_fraction": 0.9,
  "queue_depth_p2": 89,
  "queue_max_p2": 100,
  "circuit_breaker_state": "CLOSED",
  "adaptive_rps": 9500,
  "request_id": "req-uuid",
  "trace_id": "trace-uuid",
  "instance_id": "search-service-pod-007"
}
```

P0/P1 shed events are always logged at `error` level and trigger immediate PagerDuty alerts.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A (Chosen) | Option B (Rejected) | Reason |
|---|---|---|---|
| Throttle architecture | In-process SDK + async control plane | Sidecar proxy (Envoy ext_authz) | In-process: zero network hops, < 0.5 ms overhead; sidecar adds 2-5 ms per call |
| Adaptive algorithm | AIMD + EWMA smoothing | PID controller | AIMD is simpler to tune and inherently stable; PID requires careful Kp/Ki/Kd calibration and can oscillate |
| Client-side throttling | Google's accept-rate formula | Fixed static threshold | Accept-rate formula is self-calibrating (no configuration); static threshold requires manual tuning per service |
| Priority queuing | Multi-level with starvation prevention | Weighted fair queuing | MLFQ gives absolute P0 priority (required for payments); WFQ allows P0 starvation at high P1/P2 load |
| Graceful degradation | Per-request cached response (in-process LRU) | Shared response cache (Redis) | In-process cache: zero network latency; Redis cache adds 1-3 ms which defeats the purpose of degradation |
| Control plane push model | gRPC persistent stream (push) | REST polling (pull) | Push achieves < 5s propagation; polling at 5s intervals can miss signals for up to 5s additional |
| Backpressure signal | Multi-signal (error rate + latency + queue depth) | Single signal (error rate only) | Single signal causes false positives (high latency ≠ error rate); multi-signal more accurate |
| Quota window type | Rolling window (daily + hourly) | Fixed calendar window | Rolling window prevents the "midnight rush" (all clients reset simultaneously at midnight and flood the system) |
| Circuit breaker tuning | Per-service-type presets | Single global threshold | Different services have vastly different normal error rates; one threshold causes false positives for noisy services |
| Audit log | Kafka async | Synchronous PostgreSQL write | Synchronous write adds 5-10ms per throttle event; with 1M throttle events/sec, this is 5-10 seconds of added latency across the fleet |

---

## 11. Follow-up Interview Questions

**Q1: How do you differentiate between a "fast fail" throttle (immediate rejection) and a "slow down" throttle (admit but at reduced pace)? When is each appropriate?**
A: Fast fail (immediate 429/503 rejection) is appropriate for: (1) requests that cannot tolerate latency (real-time user-facing requests where a 100 ms queue delay is worse than an immediate error with retry); (2) requests that will likely be retried by the caller anyway (so queuing serves no purpose); (3) stateless idempotent requests (safe to retry). Slow-down throttle (queue + gradual drain) is appropriate for: (1) batch jobs that have flexible latency requirements; (2) requests that are expensive to regenerate (the caller would rather wait than retry from scratch); (3) requests with strict ordering requirements (must be processed in submission order). Implementation: the admission controller checks the request's `X-Throttle-Policy: fast_fail|queue` header (set by the caller SDK based on the API call type) and routes accordingly.

**Q2: How do you implement application-level throttling for a service that uses connection pooling? The pool itself is a form of throttle.**
A: Connection pools and application-level throttles are complementary at different granularities: (1) **Connection pool as coarse throttle**: a pool of 100 connections to a database allows at most 100 concurrent queries. When all connections are in use, new queries block waiting for a connection. This is implicit throttling with no priority or adaptability. (2) **Application throttle wraps the pool**: before acquiring a connection from the pool, the application throttle checks priority and rate. A P3 batch query is rejected immediately if the pool would block for > 100 ms; a P0 critical query always acquires the connection (possibly after waiting up to 5 seconds). This prevents the pool from being monopolized by P3 queries, which would cause P0 queries to queue in the pool's wait list alongside them. (3) **Metrics from the pool feed the adaptive throttle**: pool utilization (active connections / max connections) is a direct signal of downstream saturation. If pool utilization > 90%, the adaptive throttle reduces the rate of new query submissions.

**Q3: Your adaptive throttle uses EWMA with alpha=0.3. How would you tune this parameter for a service with highly variable load?**
A: Alpha controls the "memory" of the EWMA: high alpha (0.7-0.9) = highly reactive, tracks recent data closely, more sensitive to noise; low alpha (0.1-0.3) = smooth, slow to adapt, less sensitive to transient spikes. Tuning process: (1) Profile the downstream's historical error rate and latency time series. Calculate the typical "signal-to-noise ratio": how large are genuine overload events (signal) compared to normal fluctuations (noise)? (2) For highly variable services (e.g., ML inference with variable input complexity): use lower alpha (0.1-0.2) to avoid false positives from natural variance. The EWMA smooths over variability. (3) For stable services that fail suddenly (e.g., a database with clear healthy/unhealthy states): use higher alpha (0.5-0.7) for faster reaction. (4) Additionally, add a "confirmation window": only act on an overload signal if the smoothed metric has been above the threshold for at least 3 consecutive ticks. This provides hysteresis — the system doesn't trigger on a single elevated reading.

**Q4: How do you handle priority queuing when the caller is a streaming gRPC service that sends thousands of messages on a single connection?**
A: gRPC streaming is a single HTTP/2 connection multiplexed with many logical streams. Throttling at the gRPC stream level requires: (1) **Stream-level priority**: when a client opens a gRPC stream, classify the entire stream's priority based on the initial metadata (call credentials, stream headers). All messages on that stream inherit the stream's priority. This is simpler than per-message priority classification. (2) **Token consumption per message**: the outbound token bucket is shared across all streams. Each message consumes 1 token. When the bucket is empty, the gRPC server applies flow control: it stops reading from the stream's receive buffer, which causes the client's `send()` to block (gRPC flow control). This is natural backpressure through the gRPC protocol. (3) **Message-level priority (advanced)**: if messages within a stream can have different priorities (e.g., control messages vs. data messages), use gRPC metadata per-message (added in gRPC v2) to classify. The server reads each message's metadata before deciding whether to process immediately or defer.

**Q5: How would you test the throttling service under realistic conditions? What scenarios are hardest to test?**
A: Testing strategy: (1) **Unit tests**: token bucket exhaustion, circuit breaker state transitions, AIMD convergence, starvation prevention in priority queue. Each testable in isolation with deterministic time injection. (2) **Integration tests**: simulate a downstream that alternates between healthy and overloaded; verify circuit breaker opens/closes correctly; verify adaptive throttle reduces/increases rate. Use a mock downstream server that returns 500 errors on demand. (3) **Load tests**: run 50K simulated clients against a test cluster; introduce artificial downstream latency (via tc-netem or a proxy like Toxiproxy); measure: (a) P0 shed rate (should be 0 under normal overload), (b) overall throughput reduction during adaptive throttle activation, (c) recovery time after simulated failure. (4) **Chaos tests**: kill downstream service instances randomly; verify graceful degradation responses; verify circuit breaker opens within expected time. Kill control plane; verify instances continue operating with cached thresholds. (5) **Hardest to test**: the EWMA/AIMD interaction with real-world traffic variability. Synthetic load tests don't capture the bursty, correlated traffic patterns of real microservice calls. Solution: canary testing in production with < 1% of real traffic, with careful monitoring.

**Q6: What is the relationship between the throttling service and SLA guarantees? How do you ensure P0 requests meet their latency SLA under extreme overload?**
A: P0 SLA guarantees require resource isolation, not just priority ordering. Priority queuing ensures P0 is processed before P2/P3, but if the system is overwhelmed with P0 traffic, even P0 requests queue. True SLA guarantees for P0 require: (1) **Reserved capacity**: allocate a fixed fraction of downstream capacity (e.g., 20% of database connections, 20% of downstream RPS budget) exclusively to P0 traffic. P0 requests use this reserved pool; P2/P3 cannot access it. (2) **P0 always flows**: the token bucket rate for P0 traffic is never reduced by the adaptive throttle. Only the shared (P1/P2/P3) token bucket is throttled. P0 has its own token bucket with a fixed rate = reserved capacity. (3) **SLA tracking**: the throttling service records p99 latency per priority class. An SLA alert fires if P0 p99 > SLA threshold (e.g., 200 ms). (4) **Load shedding for P0 protection**: when the system is so overloaded that P0 latency is at risk, the admission controller sheds ALL P2/P3 traffic immediately — even below normal shed thresholds — to protect P0.

**Q7: How does the throttling service handle tenant isolation in a multi-tenant SaaS product?**
A: Multi-tenant throttling adds a tenant dimension to the policy model: (1) **Per-tenant token buckets**: each tenant (identified by tenant_id in the auth context) has its own token bucket for calls to each downstream service. Tenant A's heavy usage cannot consume tokens from Tenant B's bucket. (2) **Tenant tier policies**: similar to user tiers in the API rate limiter, tenant tiers define different limits. Enterprise tenants have higher `max_rps` per downstream service. (3) **Tenant-level priority**: all requests from a paying enterprise tenant are at least P1; free-tier tenant requests are P2/P3. (4) **Global capacity enforcement**: a system-wide token bucket caps the total RPS to a downstream service regardless of individual tenant buckets. This prevents a single tenant (even an enterprise one) from monopolizing the downstream if they have an unusually high limit. (5) **Tenant isolation in queues**: under extreme overload, shed P3 requests across tenants proportionally (not first-come-first-served, which would favor tenants that happen to send requests first).

**Q8: How do you ensure the throttling service doesn't introduce security vulnerabilities — specifically, could a malicious service forge a high priority to bypass throttling?**
A: Priority should be assigned based on trusted signals only: (1) **Endpoint-based priority (trusted)**: the service's admin team registers each endpoint with its priority class in the policy store. The throttle SDK reads the endpoint path from the request — this is determined by the server's routing, not the client. (2) **Auth-token-based priority (trusted)**: priority derived from the verified auth token's claims (e.g., `scope: enterprise`, `role: payment_processor`). Auth tokens are signed by the auth service and cannot be forged. (3) **Client-asserted priority (untrusted)**: if a caller asserts `X-Request-Priority: 0` without authentication, this is ignored. The throttle SDK validates priority claims against an allowlist of services that are permitted to self-report priority (e.g., only the payment service can assert P0 for its own requests). (4) **Rate limiting the priority assertion itself**: even if a service is authorized to assert P0, it cannot assert P0 for ALL requests — there's a maximum P0 quota per service per second. Exceeding it causes the excess P0 requests to be downgraded to P1 automatically.

**Q9: How do you handle a situation where the adaptive throttle and the circuit breaker are sending conflicting signals?**
A: The signals can appear conflicting but serve different purposes: (1) **Circuit breaker is binary** (OPEN/CLOSED/HALF_OPEN): it determines whether to send ANY traffic to the downstream. When OPEN: zero traffic. (2) **Adaptive throttle is continuous** (RPS value): it determines HOW MUCH traffic to send. When the circuit is CLOSED, the adaptive throttle still limits the rate. Resolution rule: the circuit breaker takes precedence over the adaptive throttle — if the circuit is OPEN, the token bucket rate is irrelevant (no traffic flows regardless). When the circuit transitions to HALF_OPEN, the token bucket rate is set to 1% of `adaptive_min_rps` (the probe rate) — not the full min rate. Only after the circuit closes does the adaptive throttle gradually ramp up. This ensures the circuit breaker's recovery probe is truly minimal and doesn't re-trigger overload.

**Q10: How would you design the throttling service to handle "cold traffic" patterns — a service that receives traffic only occasionally (e.g., once per hour)?**
A: Cold traffic services have specific challenges: (1) **EWMA initialization**: on first request after a long idle period, the EWMA history is zero. The first real overload signal looks like a sudden jump from 0% to 100% error rate — triggering an immediate multiplicative decrease. Fix: initialize EWMA at a neutral value (50% error rate) for services that have been idle > the EWMA window. This prevents the algorithm from treating the first request as a baseline and the second request (which may error) as a catastrophic overload. (2) **Token bucket warm-up**: after a long idle period, the token bucket is fully charged (burst capacity). The first batch of requests can all succeed simultaneously — potentially flooding a downstream. Fix: cap token bucket at `min(burst_size, rate * max_idle_seconds)` where `max_idle_seconds = 10`. So even after 1 hour of idle, the token bucket behaves as if it was only idle for 10 seconds. (3) **Circuit breaker persistence**: for services with hourly traffic, the circuit breaker state should be persisted to the control plane (not just in-process). Otherwise, if the last run ended with the circuit OPEN, the next run would start with CLOSED and immediately hammer a potentially still-failing downstream.

**Q11: What's the performance overhead of the priority classifier for highly dynamic rules (e.g., 10,000 priority rules)?**
A: With 10,000 priority rules, a linear scan per request would be too slow. Optimization approaches: (1) **Prefix tree (trie) for endpoint matching**: endpoint paths like `/v1/payment` → `/v1/payment/confirm` are organized in a trie. Lookup is O(path_length), typically < 50 characters = O(1) practically. (2) **Compiled rule engine**: at startup and on policy update, the 10,000 rules are compiled into a deterministic finite automaton (DFA) or a ternary search trie. Rule lookup becomes a pure memory operation with O(1) average complexity. (3) **Caching**: most services have a small set of frequently-used endpoints. An in-process LRU cache of (endpoint, auth_claims) → priority class covers > 99% of requests without hitting the rule engine. Cache size: 1,000 entries × 200 bytes = 200 KB — trivial. (4) **Benchmark**: on a 2022 server CPU, a well-implemented trie with LRU cache handles > 10M rule lookups per second per core — far exceeding our 50K RPS per instance.

**Q12: How would you implement a "request hedging" strategy that works alongside throttling?**
A: Request hedging is a latency optimization where a request is sent to two downstream instances simultaneously, and the first response wins. This interacts with throttling in important ways: (1) **Token cost**: a hedged request consumes 2 tokens from the token bucket (one per hedge). The throttle client must be aware that hedging doubles the effective RPS to the downstream. (2) **Hedging threshold**: only hedge if the first response hasn't arrived within p95 latency (e.g., 200 ms). This limits hedging to the tail (5% of requests), so the average token consumption is 1.05× rather than 2×. (3) **Adaptive hedging under throttle**: when the token bucket rate is reduced by the adaptive throttle, the hedging threshold should be increased (or hedging disabled entirely). A throttled system is already under pressure; hedging doubles that pressure. Disable hedging when `current_adaptive_rps < 0.7 * max_rps`. (4) **Circuit breaker interaction**: if the circuit breaker is in HALF_OPEN state (sending probes), never hedge — the probe should be minimal.

**Q13: How do you handle a dependency graph where Service A throttles itself because of Service B, but Service B is only slow because it's waiting for Service C to respond? How do you propagate throttle signals through the chain?**
A: This is the "deep chain backpressure" problem. Solutions: (1) **Distributed tracing context propagation**: each service in the chain adds its throttle state to the trace context (`X-Throttle-State: searching-service:open,index-service:degraded`). The root service can read this context from the response and include it in its throttle decision. (2) **Aggregate health publishing**: Service B publishes its own observed health to the control plane, including the health of its downstream dependencies. The control plane's adaptive engine uses this to distinguish "B is slow because of C" from "B is intrinsically slow." If B's own CPU/memory is healthy but its outbound latency to C is high, the throttle recommendation is: callers of B should reduce their rate slightly (to reduce B's outbound queue to C), but the real fix is throttling B's calls to C. (3) **End-to-end timeout propagation**: use the `grpc-timeout` header (or HTTP `Request-Timeout`) to propagate the remaining time budget through the chain. If Service A has 500 ms total budget and has already spent 200 ms, it passes `grpc-timeout: 300ms` to Service B. Service B immediately knows it has 300 ms; if its p50 latency to C is 400 ms, it can fast-fail rather than queuing and timing out.

**Q14: What are the trade-offs between synchronous throttling (blocking the caller) and asynchronous throttling (queuing the request for later processing)?**
A: Synchronous throttling (immediate allow/reject): (1) Zero latency for allowed requests; (2) Callers get immediate feedback — they can retry, fall back, or propagate the error; (3) No memory overhead for queueing; (4) Correct for real-time user-facing requests. Asynchronous throttling (queue then drain): (1) Smooths out bursty traffic — a 10× burst for 100 ms can be absorbed if the queue is deep enough; (2) Callers don't need to implement retry logic — they get a response, just delayed; (3) Memory overhead proportional to queue depth; (4) Introduces latency (queue wait time) for all queued requests, not just overloaded ones; (5) Correct for batch, background, or latency-tolerant requests. The hybrid approach (our design): admit the request into the queue synchronously (immediate feedback: queued or rejected), then drain the queue asynchronously. The caller gets a `202 Accepted` for queued requests, or a `429/503` for shed requests. This is the best of both worlds for batch APIs. For real-time APIs, pure synchronous with fast fail is correct.

**Q15: How would you build an end-to-end "load budget" system where each incoming request is assigned a "load budget" that is consumed by each downstream call it makes, preventing "fan-out storms"?**
A: Fan-out storms occur when a single incoming request triggers dozens of downstream calls (e.g., a search request that fans out to 50 shard services). Load budget system: (1) **Budget assignment at ingress**: the API gateway assigns each incoming request a "load budget" = `max_downstream_calls × cost_per_call`. For a search request: budget = 50 (50 shard calls allowed). This budget is attached to the request context. (2) **Budget consumption at each outbound call**: before each downstream call, the outbound throttle client checks the remaining budget in the context. If budget ≥ 1: decrement and proceed. If budget = 0: fail fast (return error rather than making the downstream call). (3) **Budget propagation**: when a service makes an outbound call, it passes the remaining budget in the request context (gRPC metadata or HTTP header: `X-Load-Budget: 23`). The downstream service uses this as its own starting budget for further fan-out. (4) **Budget enforcement at the throttling service**: the admission controller at each service checks that the incoming request's load budget is > 0 before processing. This prevents a request with an exhausted budget from consuming resources it hasn't "paid for." (5) **Adaptive budget**: the control plane can reduce the per-request budget allocation during system overload, effectively reducing the fan-out multiplier fleet-wide without requiring code changes.

---

## 12. References & Further Reading

1. **Google SRE Book, Chapter 21: "Handling Overload"** (2016, O'Reilly): https://sre.google/sre-book/handling-overload/ — The definitive reference for client-side throttling, the accept-rate formula (K-factor throttling), and backend load shedding strategies. Directly informs the `ClientSideThrottle` implementation.

2. **Google SRE Book, Chapter 22: "Addressing Cascading Failures"** (2016, O'Reilly): https://sre.google/sre-book/addressing-cascading-failures/ — Covers backpressure, priority queuing, graceful degradation, and circuit breaker patterns in the context of Google's production systems.

3. **Netflix Tech Blog — "Making the Netflix API More Resilient"** (2012): https://netflixtechblog.com/making-the-netflix-api-more-resilient-a8ec62159c2d — Introduces the Hystrix circuit breaker library (now deprecated in favor of Resilience4j), priority queuing in the context of API fan-out, and graceful degradation strategies.

4. **Hystrix Design Principles** (Netflix OSS, now archived): https://github.com/Netflix/Hystrix/wiki/How-it-Works — Detailed documentation of the sliding window approach used in Hystrix for circuit breaking: count-based and time-based windows, half-open probing, fallback chain.

5. **Resilience4j Documentation — CircuitBreaker, RateLimiter, Bulkhead**: https://resilience4j.readme.io/docs — The current Java-ecosystem standard for application-level throttling patterns. The Bulkhead pattern (thread pool isolation) is particularly relevant to the resource isolation discussion.

6. **TCP Congestion Control — AIMD Analysis** (Jacobson, 1988): "Congestion avoidance and control", ACM SIGCOMM 1988 — The original AIMD paper. Our adaptive throttle's additive-increase/multiplicative-decrease is directly inspired by TCP congestion control.

7. **Backpressure in Reactive Systems — The Reactive Manifesto**: https://www.reactivemanifesto.org/ — Defines backpressure as a first-class citizen of reactive system design. Foundational reading for understanding why synchronous blocking is insufficient for high-throughput systems.

8. **Amazon Builder's Library — "Using load shedding to avoid overload"**: https://aws.amazon.com/builders-library/using-load-shedding-to-avoid-overload/ — Practical guidance from Amazon on implementing load shedding, priority queuing, and graceful degradation in distributed services. Discusses the "load shedding retry budget" concept.

9. **Uber Engineering Blog — "QALM: QoS Admission and Load Management Framework"** (2019): https://www.uber.com/blog/qalm-qos-admission-load-management-framework/ — Uber's production throttling system. Describes the admission control, priority queuing, and adaptive concurrency limiting used across Uber's microservices. Very close to the architecture described here.

10. **Cloudflare Blog — "Keeping the Cloud Throttled: Adaptive Load Shedding"**: https://blog.cloudflare.com/adaptive-load-balancing-and-load-shedding/ — Details how Cloudflare implements adaptive load shedding at the edge. Covers EWMA smoothing, overload detection, and priority-based traffic management.

11. **Martin Fowler — "Circuit Breaker" Pattern**: https://martinfowler.com/bliki/CircuitBreaker.html — The canonical description of the circuit breaker pattern. Brief but precise; defines the CLOSED/OPEN/HALF_OPEN state machine.

12. **"Thinking in Systems: A Primer"** (Meadows, 2008, Chelsea Green Publishing) — Background reading for understanding feedback loops, oscillation in control systems, and the pitfalls of over-correction — all directly relevant to designing the adaptive throttle's stability properties.
