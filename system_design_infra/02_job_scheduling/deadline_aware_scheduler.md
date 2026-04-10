# System Design: Deadline-Aware Scheduler

> **Relevance to role:** Infrastructure platforms serve workloads with hard SLAs -- certificate rotation must complete before expiry, database migrations must finish within a maintenance window, and compliance reports must be delivered by regulatory deadlines. A deadline-aware scheduler ensures time-critical jobs complete on time by integrating deadline constraints into scheduling, resource allocation, and preemption decisions.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement | Detail |
|----|------------|--------|
| FR-1 | Deadline specification | Jobs declare a hard or soft deadline (absolute timestamp) |
| FR-2 | Deadline-driven scheduling | Jobs with nearer deadlines get higher scheduling priority |
| FR-3 | Feasibility check | At submission, determine if the deadline is achievable given current load |
| FR-4 | SLA violation prediction | Proactively detect jobs at risk of missing their deadline |
| FR-5 | Deadline-based preemption | Jobs approaching their deadline can preempt lower-urgency work |
| FR-6 | Soft vs. hard deadlines | Hard: must complete (trigger escalation/resource reservation). Soft: best-effort (alert but don't preempt) |
| FR-7 | Backpressure on overcommitment | Reject or defer new jobs if accepting them would cause deadline violations |
| FR-8 | Resource reservation | Reserve capacity for upcoming deadline-critical jobs |
| FR-9 | SLA reporting | Track deadline hit/miss rates per tenant, job type, and time period |
| FR-10 | Integration with priority system | Deadline urgency combined with base priority for scheduling decisions |

### Non-Functional Requirements
| NFR | Target | Rationale |
|-----|--------|-----------|
| Deadline adherence | 99.9% for hard deadlines | Hard deadlines represent contractual SLAs |
| Prediction accuracy | Detect 95% of at-risk jobs > 10 min before deadline | Enough time for corrective action |
| Scheduling latency | < 500ms for deadline-critical jobs | Deadline jobs can't wait in queue |
| Availability | 99.99% | Scheduler downtime directly causes SLA misses |
| Scale | 500K jobs with deadlines active simultaneously | Enterprise platform |
| Feasibility check latency | < 1s | Must provide quick feedback at submission time |

### Constraints & Assumptions
- Builds on the distributed job scheduler and priority-based scheduler
- Jobs declare estimated runtime (or it's predicted from historical data)
- Resource pool sizes are known and change slowly (capacity planning data available)
- MySQL for persistent state; Redis for deadline sorted sets and real-time tracking
- The scheduler knows aggregate resource availability across the worker fleet
- Network latency between scheduler and workers < 2ms

### Out of Scope
- Hard real-time scheduling with mathematical proofs (this is soft/firm real-time)
- Deadline-aware network scheduling (only compute scheduling)
- Dynamic deadline modification by external systems
- Cost optimization (minimizing cost while meeting deadlines)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Active deadline jobs | 500K | Across all tenants |
| Hard deadline jobs | 50K (10%) | Compliance, cert rotation, SLA-bound |
| Soft deadline jobs | 450K (90%) | Best-effort, reporting, batch |
| New deadline jobs/hour | 50K | Submission rate |
| Deadline checks/min | 500K / 5 min = 100K | Feasibility re-evaluation frequency |
| SLA violation predictions/hour | ~500 | 0.1% of active jobs at risk at any time |
| Preemptions for deadline/hour | ~200 | Subset of at-risk jobs that need preemption |
| Workers | 5K | Heterogeneous pool |

### Latency Requirements

| Operation | P50 | P99 | P99.9 |
|-----------|-----|-----|-------|
| Feasibility check at submission | 50ms | 500ms | 1s |
| Deadline urgency recalculation | 1ms | 10ms | 50ms |
| SLA violation prediction scan | 100ms | 1s | 5s |
| Deadline-based preemption decision | 50ms | 500ms | 2s |
| SLA dashboard query | 200ms | 1s | 5s |

### Storage Estimates

| Data | Size per record | Count | Total |
|------|----------------|-------|-------|
| Deadline metadata per job | 300 B | 500K active | 150 MB |
| Runtime estimation history | 200 B | 10M records | 2 GB |
| SLA violation events | 500 B | 5K/day | 2.5 MB/day |
| Deadline sorted set (Redis) | 50 B | 500K | 25 MB |
| Feasibility snapshots | 1 KB | 50K/hour | 50 MB/hour |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Deadline recalculations | 100K/min x 300 B = 30 MB/min | 500 KB/s |
| Violation predictions | 500/hr x 500 B = 250 KB/hr | Negligible |
| SLA metric emissions | 500K jobs x 100 B x 1/min = 50 MB/min | 833 KB/s |
| **Total** | | **~1.3 MB/s** |

---

## 3. High Level Architecture

```
    ┌──────────────────────────────────────────────────┐
    │                    API / CLI                      │
    │  (submit job with deadline, query SLA status)     │
    └────────────────────┬─────────────────────────────┘
                         │
    ┌────────────────────▼─────────────────────────────┐
    │           Admission Controller                    │
    │                                                   │
    │  ┌──────────────┐  ┌──────────────────────────┐  │
    │  │ Feasibility  │  │  Resource Reservation    │  │
    │  │ Checker      │  │  Manager                 │  │
    │  └──────────────┘  └──────────────────────────┘  │
    └────────────────────┬─────────────────────────────┘
                         │
    ┌────────────────────▼─────────────────────────────┐
    │           Deadline-Aware Dispatch Engine          │
    │                                                   │
    │  ┌──────────────┐  ┌──────────────────────────┐  │
    │  │ Urgency      │  │  Hybrid Score            │  │
    │  │ Calculator   │  │  (deadline + priority)   │  │
    │  │ (Laxity)     │  │                          │  │
    │  └──────────────┘  └──────────────────────────┘  │
    │                                                   │
    │  ┌──────────────┐  ┌──────────────────────────┐  │
    │  │ EDF Queue    │  │  Deadline Preemption     │  │
    │  │ (sorted by   │  │  Engine                  │  │
    │  │  deadline)   │  │                          │  │
    │  └──────────────┘  └──────────────────────────┘  │
    └────────────────────┬─────────────────────────────┘
                         │
    ┌────────────────────▼─────────────────────────────┐
    │           SLA Monitor & Predictor                │
    │                                                   │
    │  ┌──────────────┐  ┌──────────────────────────┐  │
    │  │ At-Risk      │  │  Violation Alerting      │  │
    │  │ Detector     │  │  & Escalation            │  │
    │  │ (every 1min) │  │                          │  │
    │  └──────────────┘  └──────────────────────────┘  │
    └────────────────────┬─────────────────────────────┘
                         │
    ┌────────────────────▼─────────────────────────────┐
    │           Worker Fleet                            │
    │  ┌────────┐  ┌────────┐  ┌────────┐             │
    │  │Reserved│  │General │  │  Burst │             │
    │  │Capacity│  │ Pool   │  │  Pool  │             │
    │  └────────┘  └────────┘  └────────┘             │
    └──────────────────────────────────────────────────┘
         │               │              │
    ┌────▼───┐     ┌─────▼────┐   ┌────▼──────┐
    │ MySQL  │     │  Redis   │   │   etcd    │
    │        │     │(deadline │   │ (leader   │
    │        │     │ sorted   │   │  election)│
    │        │     │ sets,    │   │           │
    │        │     │ reserv.) │   │           │
    └────────┘     └──────────┘   └───────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Admission Controller** | Validates deadlines at submission; checks feasibility; reserves resources for hard deadlines |
| **Feasibility Checker** | Estimates whether the job can complete before deadline given current cluster load |
| **Resource Reservation Manager** | Holds reserved capacity for upcoming hard-deadline jobs |
| **Urgency Calculator** | Computes laxity (deadline - now - estimated_remaining_runtime) for scheduling priority |
| **Hybrid Score** | Combines deadline urgency with base priority into a single scheduling score |
| **EDF Queue** | Earliest Deadline First sorted set: jobs sorted by absolute deadline |
| **Deadline Preemption Engine** | Preempts non-deadline or distant-deadline jobs when a near-deadline job can't find resources |
| **SLA Monitor & Predictor** | Periodically scans running/queued deadline jobs; predicts which will miss deadlines |
| **At-Risk Detector** | Identifies jobs whose laxity has dropped below a threshold |
| **Violation Alerting** | Sends alerts and escalations for predicted/actual SLA violations |
| **Reserved Capacity** | Dedicated workers reserved for hard-deadline jobs (never used for batch work) |

### Data Flows

**Primary: Deadline Job Submission & Execution**
1. Client submits job with deadline: `"deadline": "2026-04-09T16:00:00Z", "deadline_type": "HARD"`
2. Admission Controller runs feasibility check: estimates runtime, checks available capacity
3. If feasible: accept job, reserve resources (for HARD), insert into EDF queue
4. If infeasible: reject with explanation ("estimated completion time exceeds deadline by 45 minutes")
5. Dispatch Engine dequeues by hybrid score (deadline urgency + priority)
6. Worker executes; SLA Monitor tracks progress

**Secondary: SLA Violation Prediction**
1. SLA Monitor runs every 60 seconds
2. For each running/queued deadline job, calculate: `time_remaining = deadline - now`, `work_remaining = estimated_runtime - elapsed_runtime`
3. If `work_remaining > time_remaining * 0.8` (job needs > 80% of remaining time): mark AT_RISK
4. For AT_RISK hard-deadline jobs: trigger preemption of lower-urgency work on the same worker (get more resources) or migrate to a faster worker
5. If violation is imminent (< 5 min and still not complete): send P1 alert

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Deadline metadata (extends job_definition / job_execution)
CREATE TABLE job_deadline (
    job_id              BIGINT UNSIGNED PRIMARY KEY,
    deadline            TIMESTAMP(6)    NOT NULL COMMENT 'Absolute deadline',
    deadline_type       ENUM('HARD','SOFT') NOT NULL,
    estimated_runtime_ms BIGINT UNSIGNED NOT NULL COMMENT 'Predicted runtime in ms',
    runtime_confidence  DECIMAL(3,2)    NOT NULL DEFAULT 0.80 COMMENT 'Confidence level of estimate',
    laxity_ms           BIGINT          NULL COMMENT 'deadline - now - estimated_remaining; computed',
    urgency_score       DOUBLE          NULL COMMENT 'Combined scheduling score; computed',
    feasibility_status  ENUM('FEASIBLE','AT_RISK','INFEASIBLE','VIOLATED','MET') NOT NULL DEFAULT 'FEASIBLE',
    reservation_id      BIGINT UNSIGNED NULL COMMENT 'Resource reservation if HARD deadline',
    submitted_at        TIMESTAMP(6)    NOT NULL,
    accepted_at         TIMESTAMP(6)    NULL,
    started_at          TIMESTAMP(6)    NULL,
    estimated_completion TIMESTAMP(6)   NULL COMMENT 'submitted_at + estimated_runtime_ms',
    actual_completion   TIMESTAMP(6)    NULL,
    deadline_met        BOOLEAN         NULL COMMENT 'NULL if not yet completed',
    slack_ms            BIGINT          NULL COMMENT 'deadline - actual_completion; negative = violated',
    INDEX idx_deadline (deadline),
    INDEX idx_type_status (deadline_type, feasibility_status),
    INDEX idx_laxity (laxity_ms),
    FOREIGN KEY (job_id) REFERENCES job_definition(job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Resource reservations for hard-deadline jobs
CREATE TABLE resource_reservation (
    reservation_id      BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    job_id              BIGINT UNSIGNED NOT NULL,
    tenant_id           VARCHAR(64)     NOT NULL,
    reserved_cpu_millis INT UNSIGNED    NOT NULL,
    reserved_memory_mb  INT UNSIGNED    NOT NULL,
    reserved_gpu        INT UNSIGNED    DEFAULT 0,
    pool_name           VARCHAR(64)     NOT NULL,
    reserved_from       TIMESTAMP(6)    NOT NULL COMMENT 'When reservation becomes active',
    reserved_until      TIMESTAMP(6)    NOT NULL COMMENT 'deadline + grace_period',
    status              ENUM('PENDING','ACTIVE','CONSUMED','RELEASED','EXPIRED') DEFAULT 'PENDING',
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_pool_time (pool_name, reserved_from, reserved_until),
    INDEX idx_status (status),
    INDEX idx_job (job_id),
    FOREIGN KEY (job_id) REFERENCES job_definition(job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- SLA violation events
CREATE TABLE sla_violation (
    violation_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    job_id              BIGINT UNSIGNED NOT NULL,
    tenant_id           VARCHAR(64)     NOT NULL,
    deadline            TIMESTAMP(6)    NOT NULL,
    deadline_type       ENUM('HARD','SOFT') NOT NULL,
    violation_type      ENUM('PREDICTED','ACTUAL') NOT NULL,
    predicted_at        TIMESTAMP(6)    NULL COMMENT 'When we first predicted the violation',
    actual_violation_at TIMESTAMP(6)    NULL COMMENT 'When the deadline actually passed',
    time_overdue_ms     BIGINT          NULL COMMENT 'How late the completion was',
    root_cause          VARCHAR(512)    NULL COMMENT 'Automated root cause categorization',
    escalation_level    ENUM('NONE','WARN','PAGE','INCIDENT') DEFAULT 'NONE',
    resolved            BOOLEAN         DEFAULT FALSE,
    created_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_tenant_type (tenant_id, deadline_type, violation_type),
    INDEX idx_deadline (deadline),
    INDEX idx_unresolved (resolved, created_at),
    FOREIGN KEY (job_id) REFERENCES job_definition(job_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Runtime estimation history (for ML-based prediction)
CREATE TABLE runtime_estimation (
    estimation_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    job_type_key        VARCHAR(255)    NOT NULL COMMENT 'Grouping key for similar jobs',
    actual_runtime_ms   BIGINT UNSIGNED NOT NULL,
    resource_config     JSON            NOT NULL COMMENT '{"cpu_millis":2000,"memory_mb":4096}',
    input_size_bytes    BIGINT UNSIGNED NULL COMMENT 'For data-dependent runtime estimation',
    recorded_at         TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_type_time (job_type_key, recorded_at DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (TO_DAYS(recorded_at)) (
    PARTITION p_2026_04 VALUES LESS THAN (TO_DAYS('2026-05-01')),
    PARTITION p_2026_05 VALUES LESS THAN (TO_DAYS('2026-06-01')),
    PARTITION p_future  VALUES LESS THAN MAXVALUE
);
```

### Database Selection

Same as other schedulers: **MySQL 8.0** for persistent state, **Redis** for deadline sorted sets and resource reservation tracking.

### Indexing Strategy

| Index | Table | Purpose |
|-------|-------|---------|
| `idx_deadline` | job_deadline | EDF queue ordering, SLA monitor scan |
| `idx_type_status` | job_deadline | Filter by deadline type and current feasibility |
| `idx_laxity` | job_deadline | Find lowest-laxity jobs for preemption decisions |
| `idx_pool_time` | resource_reservation | Find active reservations for a pool |
| `idx_type_time` | runtime_estimation | Historical runtime lookup for prediction |

---

## 5. API Design

### REST API Endpoints

```
POST /api/v1/jobs
Authorization: Bearer <JWT>
Content-Type: application/json

Request:
{
  "job_name": "cert-rotation-proxy-tier",
  "command": "/opt/scripts/rotate-certs.sh --tier proxy",
  "container_image": "cert-manager:2.1.0",
  "resource_request": {"cpu_millis": 1000, "memory_mb": 2048},
  "priority": 800,
  "deadline": "2026-04-09T16:00:00Z",
  "deadline_type": "HARD",
  "estimated_runtime_ms": 300000,
  "tags": {"sla": "critical", "compliance": "pci-dss"}
}

Response (201):
{
  "job_id": 12345,
  "deadline": "2026-04-09T16:00:00Z",
  "deadline_type": "HARD",
  "feasibility": {
    "status": "FEASIBLE",
    "estimated_completion": "2026-04-09T14:35:00Z",
    "slack_ms": 5100000,
    "confidence": 0.85,
    "reservation_id": 9001,
    "reserved_resources": {"cpu_millis": 1000, "memory_mb": 2048, "pool": "reserved"}
  },
  "laxity_ms": 4800000,
  "urgency_score": 0.45
}
```

```
# Infeasible submission
Response (409 Conflict):
{
  "error": "DEADLINE_INFEASIBLE",
  "message": "Cannot guarantee completion before deadline",
  "details": {
    "estimated_completion": "2026-04-09T16:45:00Z",
    "deadline": "2026-04-09T16:00:00Z",
    "overdue_estimate_ms": 2700000,
    "suggestion": "Extend deadline by 45 minutes or increase resource allocation"
  }
}
```

```
GET /api/v1/jobs/{job_id}/deadline-status

Response:
{
  "job_id": 12345,
  "deadline": "2026-04-09T16:00:00Z",
  "deadline_type": "HARD",
  "current_status": "RUNNING",
  "elapsed_ms": 120000,
  "estimated_remaining_ms": 180000,
  "laxity_ms": 4680000,
  "feasibility_status": "FEASIBLE",
  "at_risk": false,
  "progress_percent": 40
}
```

```
GET /api/v1/sla/dashboard
GET /api/v1/sla/violations?tenant_id=X&type=HARD&period=7d
GET /api/v1/sla/at-risk-jobs
GET /api/v1/capacity/reservations?pool=reserved&status=ACTIVE

POST /api/v1/admin/deadline-override
    {"job_id": 12345, "new_deadline": "2026-04-09T17:00:00Z", "reason": "Approved extension"}
```

### CLI Design

```bash
# Submit with deadline
jobctl submit --name "cert-rotation" --deadline "2026-04-09T16:00:00Z" --hard \
  --estimated-runtime 5m --cmd "/opt/scripts/rotate-certs.sh"

# Check deadline status
jobctl deadline-status <job_id>
# Output:
# Job 12345: cert-rotation-proxy-tier
#   Deadline:      2026-04-09T16:00:00Z (HARD)
#   Time remaining: 1h 25m
#   Runtime est:    5m (confidence: 85%)
#   Laxity:        1h 20m
#   Feasibility:   FEASIBLE
#   Status:        QUEUED (position 3)
#   Reservation:   #9001 (1 CPU, 2 GB on reserved pool)

# SLA dashboard
jobctl sla dashboard
# Output:
# PERIOD     HARD_DEADLINE_MET  SOFT_DEADLINE_MET  AT_RISK  VIOLATED
# Last 1h    100.0%             98.5%              3        0
# Last 24h   99.95%             97.2%              12       1
# Last 7d    99.92%             96.8%              89       4
# Last 30d   99.90%             95.1%              312      11

# At-risk jobs
jobctl sla at-risk
# Output:
# JOB_ID   NAME                  DEADLINE          LAXITY   STATUS   ACTION_NEEDED
# 55432    compliance-report     2026-04-09T17:00  -5m      RUNNING  PREEMPTION REQUESTED
# 55441    db-migration-shard3   2026-04-09T18:00  12m      QUEUED   RESERVATION ACTIVE

# View violations
jobctl sla violations --last 7d --type HARD
# Output:
# JOB_ID  NAME              DEADLINE           COMPLETED          OVERDUE   ROOT_CAUSE
# 55100   cert-rotation-az2 2026-04-08T14:00Z  2026-04-08T14:12Z  12m      Worker OOM killed

# Capacity reservations
jobctl reservations list --pool reserved
# Output:
# RESERVATION_ID  JOB_ID  RESOURCES        ACTIVE_FROM          ACTIVE_UNTIL         STATUS
# 9001            12345   1CPU/2GB         2026-04-09T14:30:00  2026-04-09T16:30:00  ACTIVE
# 9002            12346   4CPU/16GB        2026-04-09T15:00:00  2026-04-09T18:00:00  PENDING
```

---

## 6. Core Component Deep Dives

### 6.1 Feasibility Checker & Admission Control

**Why it's hard:** At submission time, we must predict whether a job can complete before its deadline. This requires estimating: (1) queue wait time (depends on current queue depth and dispatch rate), (2) job runtime (depends on resource allocation and job characteristics), and (3) resource availability at the estimated start time (depends on other jobs' completion patterns). All of these are probabilistic, not deterministic.

**Approaches Compared:**

| Approach | Accuracy | Latency | False Positive Rate | Complexity |
|----------|---------|---------|-------------------|------------|
| Simple: now + estimated_runtime < deadline | Low (ignores queue wait) | < 1ms | High | Very Low |
| Queue-aware: now + queue_wait + runtime < deadline | Medium | 10ms | Medium | Low |
| Simulation-based: simulate dispatch sequence | High | 100-500ms | Low | High |
| ML-based: predict start/end time from features | High | 50ms | Low | Very High |
| Reservation-based: if resources can be reserved, feasible | Highest (for HARD) | 50ms | Very Low | Medium |

**Selected: Hybrid -- queue-aware estimation for SOFT deadlines, reservation-based for HARD deadlines.** SOFT deadlines use a statistical estimate (P80 of historical queue wait + P80 of historical runtime). HARD deadlines attempt to reserve dedicated capacity; if reservation succeeds, the job is guaranteed feasible.

**Implementation:**

```python
class FeasibilityChecker:
    """
    Determines if a job can meet its deadline at submission time.
    """
    
    def __init__(self, queue_stats, runtime_estimator, reservation_manager, 
                 capacity_tracker, metrics):
        self.queue_stats = queue_stats
        self.runtime_estimator = runtime_estimator
        self.reservation_manager = reservation_manager
        self.capacity_tracker = capacity_tracker
        self.metrics = metrics
    
    def check_feasibility(self, job: JobSubmission) -> FeasibilityResult:
        """
        Main feasibility check. Returns FEASIBLE, AT_RISK, or INFEASIBLE.
        """
        now = datetime.utcnow()
        time_budget_ms = (job.deadline - now).total_seconds() * 1000
        
        if time_budget_ms <= 0:
            return FeasibilityResult(
                status='INFEASIBLE',
                reason='Deadline is in the past')
        
        # Step 1: Estimate runtime
        estimated_runtime = self.runtime_estimator.estimate(
            job_type=job.job_type_key,
            resource_config=job.resource_request,
            input_size=job.input_size_bytes
        )
        
        if estimated_runtime.p80_ms > time_budget_ms:
            return FeasibilityResult(
                status='INFEASIBLE',
                reason=f'Estimated runtime ({estimated_runtime.p80_ms}ms) exceeds time budget ({time_budget_ms}ms)',
                estimated_completion=now + timedelta(milliseconds=estimated_runtime.p80_ms))
        
        if job.deadline_type == 'HARD':
            return self._check_hard_deadline(job, now, time_budget_ms, estimated_runtime)
        else:
            return self._check_soft_deadline(job, now, time_budget_ms, estimated_runtime)
    
    def _check_hard_deadline(self, job, now, time_budget_ms, estimated_runtime) -> FeasibilityResult:
        """
        For HARD deadlines: attempt to reserve resources.
        If reservation succeeds, job is guaranteed feasible.
        """
        # Calculate when we need the resources
        # Allow for queue wait: start reservation slightly before estimated dispatch
        estimated_queue_wait = self.queue_stats.get_estimated_wait(
            job.priority, job.resource_request)
        
        reserve_from = now + timedelta(milliseconds=estimated_queue_wait.p50_ms)
        reserve_until = job.deadline + timedelta(minutes=30)  # Grace period
        
        # Attempt reservation
        reservation = self.reservation_manager.reserve(
            cpu_millis=job.resource_request['cpu_millis'],
            memory_mb=job.resource_request['memory_mb'],
            gpu=job.resource_request.get('gpu', 0),
            pool='reserved',
            from_time=reserve_from,
            until_time=reserve_until
        )
        
        if reservation is not None:
            # Reservation succeeded: guaranteed feasible
            estimated_start = reserve_from
            estimated_completion = estimated_start + timedelta(
                milliseconds=estimated_runtime.p80_ms)
            slack_ms = (job.deadline - estimated_completion).total_seconds() * 1000
            
            return FeasibilityResult(
                status='FEASIBLE',
                estimated_start=estimated_start,
                estimated_completion=estimated_completion,
                slack_ms=int(slack_ms),
                confidence=estimated_runtime.confidence,
                reservation_id=reservation.id)
        else:
            # Can't reserve on dedicated pool; try general pool with preemption guarantee
            can_preempt = self._check_preemption_feasibility(job, estimated_runtime)
            
            if can_preempt:
                return FeasibilityResult(
                    status='FEASIBLE',
                    estimated_start=now + timedelta(milliseconds=estimated_queue_wait.p80_ms),
                    estimated_completion=now + timedelta(
                        milliseconds=estimated_queue_wait.p80_ms + estimated_runtime.p80_ms),
                    slack_ms=int(time_budget_ms - estimated_queue_wait.p80_ms - estimated_runtime.p80_ms),
                    confidence=estimated_runtime.confidence * 0.9,  # Lower confidence without reservation
                    note='No reservation available; will preempt if needed')
            else:
                return FeasibilityResult(
                    status='INFEASIBLE',
                    reason='Cannot reserve resources and insufficient preemption candidates',
                    estimated_completion=now + timedelta(
                        milliseconds=estimated_queue_wait.p80_ms + estimated_runtime.p80_ms))
    
    def _check_soft_deadline(self, job, now, time_budget_ms, estimated_runtime) -> FeasibilityResult:
        """
        For SOFT deadlines: statistical estimate (no reservation).
        """
        estimated_queue_wait = self.queue_stats.get_estimated_wait(
            job.priority, job.resource_request)
        
        total_time_p80 = estimated_queue_wait.p80_ms + estimated_runtime.p80_ms
        slack_ms = time_budget_ms - total_time_p80
        
        if slack_ms > 0:
            status = 'FEASIBLE'
        elif slack_ms > -estimated_runtime.p80_ms * 0.2:  # Within 20% margin
            status = 'AT_RISK'
        else:
            status = 'INFEASIBLE'
        
        return FeasibilityResult(
            status=status,
            estimated_start=now + timedelta(milliseconds=estimated_queue_wait.p80_ms),
            estimated_completion=now + timedelta(milliseconds=total_time_p80),
            slack_ms=int(slack_ms),
            confidence=estimated_runtime.confidence)
    
    def _check_preemption_feasibility(self, job, estimated_runtime) -> bool:
        """
        Check if there are enough preemptible jobs running that could be 
        evicted to make room for this hard-deadline job.
        """
        preemptible_resources = self.capacity_tracker.get_preemptible_resources(
            min_priority_delta=200,  # Can preempt jobs 200+ priority below
            resource_type=job.resource_request
        )
        
        return (preemptible_resources['cpu_millis'] >= job.resource_request['cpu_millis']
                and preemptible_resources['memory_mb'] >= job.resource_request['memory_mb'])


class RuntimeEstimator:
    """
    Estimates job runtime from historical data.
    Uses percentile-based estimation for reliability.
    """
    
    def estimate(self, job_type: str, resource_config: dict, 
                  input_size: int = None) -> RuntimeEstimate:
        """
        Returns P50, P80, P95, and P99 runtime estimates.
        """
        # Query historical runtimes for similar jobs
        history = self.runtime_repo.get_recent(
            job_type_key=job_type, 
            resource_config=resource_config,
            limit=100  # Last 100 executions
        )
        
        if len(history) < 10:
            # Not enough history: use user-provided estimate with low confidence
            return RuntimeEstimate(
                p50_ms=None, p80_ms=None, p95_ms=None, p99_ms=None,
                confidence=0.50,
                source='USER_PROVIDED'
            )
        
        runtimes = sorted([h.actual_runtime_ms for h in history])
        
        # If input_size is available, adjust for data-dependent runtime
        if input_size is not None:
            # Simple linear regression: runtime ~ a * input_size + b
            adjustment_factor = self._compute_size_adjustment(history, input_size)
            runtimes = [int(r * adjustment_factor) for r in runtimes]
        
        p50 = runtimes[int(len(runtimes) * 0.50)]
        p80 = runtimes[int(len(runtimes) * 0.80)]
        p95 = runtimes[int(len(runtimes) * 0.95)]
        p99 = runtimes[min(int(len(runtimes) * 0.99), len(runtimes) - 1)]
        
        # Confidence based on variance: lower variance = higher confidence
        cv = statistics.stdev(runtimes) / statistics.mean(runtimes) if statistics.mean(runtimes) > 0 else 1
        confidence = max(0.5, min(0.99, 1.0 - cv))  # CV of 0 → 99%, CV of 0.5 → 50%
        
        return RuntimeEstimate(
            p50_ms=p50, p80_ms=p80, p95_ms=p95, p99_ms=p99,
            confidence=round(confidence, 2),
            source='HISTORICAL',
            sample_size=len(history)
        )
```

**Failure Modes:**
- **Runtime estimate wildly inaccurate (new job type, no history):** User-provided estimate used with low confidence (0.50). SLA Monitor detects drift at runtime and adjusts prediction.
- **Reservation fails but job is accepted anyway (SOFT deadline):** Job proceeds without reservation guarantee. If it becomes at-risk, the SLA Monitor triggers corrective action (preemption, alert).
- **Capacity snapshot is stale (resources freed between check and dispatch):** Feasibility check is conservative (uses P80 estimates). Even if the snapshot is slightly stale, the 20% margin absorbs the discrepancy.

**Interviewer Q&A:**

**Q1: Why P80 instead of P50 or P99 for runtime estimates?**
A: P50 is too optimistic: 50% of jobs will take longer, leading to frequent deadline misses. P99 is too conservative: only 1% of jobs take this long, causing most jobs to be rejected as infeasible. P80 strikes a balance: we expect 80% of jobs to complete within this estimate, giving a reasonable safety margin while maintaining high acceptance rates.

**Q2: How do you handle the cold start problem (new job type with no history)?**
A: We fall back to the user-provided `estimated_runtime_ms` with confidence=0.50. The feasibility check adds a 50% safety margin (effective estimate = user_provided * 1.5). After 10 executions, we switch to historical estimates. We also suggest that new job types run in a "calibration mode" first: execute without a deadline to establish a runtime baseline.

**Q3: What if a hard-deadline job is accepted but later becomes infeasible (e.g., worker pool shrinks)?**
A: The SLA Monitor continuously re-evaluates feasibility. If a FEASIBLE job transitions to AT_RISK, we: (1) attempt to move it to the reserved pool, (2) trigger preemption on its current worker (free up resources), (3) alert the owning team. If it transitions to INFEASIBLE (deadline mathematically impossible), we escalate and prepare a violation report.

**Q4: How do resource reservations prevent fragmentation?**
A: Reservations are time-bounded (from → until) and pool-specific. We use bin-packing to fit reservations: when a new reservation request arrives, we find the time slot with the most available capacity. Reservations that overlap in time share the pool's resources proportionally. We limit total reservation to 70% of reserved pool capacity to prevent deadlock (30% buffer for unexpected bursts).

**Q5: Can feasibility checks become a bottleneck at 50K submissions/hour?**
A: Each feasibility check takes ~50ms (Redis queue stats lookup + MySQL runtime history query). At 50K/hour = 14/second, that's 14 * 50ms = 700ms of total compute per second. A single thread handles this easily. The bottleneck would be MySQL (runtime history lookups), which we mitigate with an in-memory cache of recent runtime estimates per job type (Caffeine cache, TTL 5 min).

**Q6: How do you handle deadline-aware admission when the cluster is highly utilized (> 90%)?**
A: At high utilization, queue wait times increase and feasibility checks reject more jobs. We implement graduated backpressure: (1) At 80% utilization: warn submitters that deadlines may not be met. (2) At 90%: reject SOFT deadline jobs with < 2x estimated runtime as slack. (3) At 95%: reject all new deadline jobs except HARD with reservation. (4) At 99%: only accept jobs with existing reservations.

---

### 6.2 EDF (Earliest Deadline First) Scheduling with Hybrid Scoring

**Why it's hard:** Pure EDF (always schedule the job with the nearest deadline) ignores priority: a low-priority job with a near deadline shouldn't necessarily preempt a high-priority job with a farther deadline. We need a hybrid score that balances deadline urgency with job priority. Additionally, EDF is optimal for single-processor scheduling but not for multi-processor distributed systems.

**Approaches Compared:**

| Algorithm | Optimality | Priority Support | Multi-Processor | Starvation Risk |
|-----------|-----------|-----------------|----------------|----------------|
| EDF (Earliest Deadline First) | Optimal for single CPU | None | Suboptimal | Low (deadline-based) |
| LLF (Least Laxity First) | Optimal for single CPU | None | Better than EDF | Low |
| Hybrid EDF+Priority | Configurable | Built-in | Good | Configurable |
| Rate Monotonic | Optimal for periodic tasks | Implicit (by period) | Limited | None |
| Weighted Shortest Job First | Good for throughput | Via weights | Good | High |

**Selected: Hybrid EDF + Priority** -- Scheduling score is a weighted combination of deadline urgency and base priority. This gives us the urgency-awareness of EDF with the flexibility of priority-based scheduling.

**Implementation:**

```java
@Service
public class DeadlineAwareDispatchEngine {
    
    // Weight factors for hybrid score (tunable)
    private static final double URGENCY_WEIGHT = 0.6;
    private static final double PRIORITY_WEIGHT = 0.3;
    private static final double LAXITY_WEIGHT = 0.1;
    
    @Autowired private StringRedisTemplate redis;
    @Autowired private JobDeadlineRepository deadlineRepo;
    @Autowired private RuntimeEstimator runtimeEstimator;
    
    /**
     * Calculate the hybrid scheduling score for a deadline job.
     * Lower score = higher urgency = scheduled first.
     * 
     * Score = urgency_weight * deadline_urgency 
     *       + priority_weight * (1 - normalized_priority) 
     *       + laxity_weight * laxity_urgency
     */
    public double calculateHybridScore(long jobId, int basePriority, 
                                         Instant deadline, long estimatedRuntimeMs) {
        Instant now = Instant.now();
        long timeToDeadlineMs = Duration.between(now, deadline).toMillis();
        
        if (timeToDeadlineMs <= 0) {
            // Past deadline: highest urgency
            return -1.0;
        }
        
        // Urgency: normalized 0-1 where 1 = most urgent (deadline imminent)
        // Using exponential decay: jobs near deadline have exponentially higher urgency
        double maxHorizonMs = 24 * 60 * 60 * 1000.0; // 24-hour horizon
        double deadlineUrgency = Math.exp(-timeToDeadlineMs / maxHorizonMs * 5);
        // At 24h: urgency ≈ 0.007 (low)
        // At 1h:  urgency ≈ 0.81 (high)
        // At 10m: urgency ≈ 0.97 (very high)
        
        // Priority: normalized 0-1 where 0 = highest priority (1000)
        double normalizedPriority = 1.0 - (basePriority / 1000.0);
        
        // Laxity: (timeToDeadline - estimatedRuntime) / timeToDeadline
        // Low laxity = less slack = more urgent
        double laxityMs = timeToDeadlineMs - estimatedRuntimeMs;
        double laxityRatio = Math.max(0, laxityMs / timeToDeadlineMs);
        double laxityUrgency = 1.0 - laxityRatio; // 1 = no slack, 0 = lots of slack
        
        double score = URGENCY_WEIGHT * (1 - deadlineUrgency)  // Lower = more urgent
                     + PRIORITY_WEIGHT * normalizedPriority      // Lower = higher priority
                     + LAXITY_WEIGHT * (1 - laxityUrgency);     // Lower = less laxity
        
        return score;
    }
    
    /**
     * Enqueue a deadline job into the EDF queue.
     * Score = hybrid score (lower = higher urgency = dequeued first).
     */
    public void enqueueDeadlineJob(long jobId, int priority, Instant deadline, 
                                     long estimatedRuntimeMs) {
        double score = calculateHybridScore(jobId, priority, deadline, estimatedRuntimeMs);
        
        redis.opsForZSet().add("dispatch:deadline_queue", String.valueOf(jobId), score);
        
        // Update database
        deadlineRepo.updateUrgencyScore(jobId, score);
        deadlineRepo.updateLaxity(jobId, 
            Duration.between(Instant.now(), deadline).toMillis() - estimatedRuntimeMs);
    }
    
    /**
     * Recalculate urgency scores for all queued deadline jobs.
     * Runs every 60 seconds (urgency changes over time as deadlines approach).
     */
    @Scheduled(fixedRate = 60_000)
    public void recalculateUrgencyScores() {
        if (!leaderElector.isLeader()) return;
        
        Set<String> queuedJobs = redis.opsForZSet().range("dispatch:deadline_queue", 0, -1);
        if (queuedJobs == null || queuedJobs.isEmpty()) return;
        
        Map<String, Double> updates = new HashMap<>();
        
        for (String jobIdStr : queuedJobs) {
            long jobId = Long.parseLong(jobIdStr);
            JobDeadline dl = deadlineRepo.findById(jobId).orElse(null);
            if (dl == null) continue;
            
            double newScore = calculateHybridScore(
                jobId, dl.getBasePriority(), dl.getDeadline(), dl.getEstimatedRuntimeMs());
            
            updates.put(jobIdStr, newScore);
        }
        
        // Batch update Redis scores
        if (!updates.isEmpty()) {
            redis.executePipelined((RedisCallback<Object>) connection -> {
                for (Map.Entry<String, Double> entry : updates.entrySet()) {
                    connection.zSetCommands().zAdd(
                        "dispatch:deadline_queue".getBytes(),
                        entry.getValue(),
                        entry.getKey().getBytes());
                }
                return null;
            });
        }
        
        metrics.gauge("deadline.queue.size", queuedJobs.size());
    }
    
    /**
     * Dequeue the most urgent deadline job.
     * Integrates with the priority-based scheduler's dispatch cycle.
     */
    public Optional<Long> dequeueNextDeadlineJob() {
        // Pop the lowest-scored (most urgent) job
        Set<ZSetOperations.TypedTuple<String>> result = 
            redis.opsForZSet().popMin("dispatch:deadline_queue", 1);
        
        if (result == null || result.isEmpty()) {
            return Optional.empty();
        }
        
        ZSetOperations.TypedTuple<String> entry = result.iterator().next();
        return Optional.of(Long.parseLong(entry.getValue()));
    }
}
```

**Scoring Examples:**

| Job | Priority | Deadline (from now) | Est. Runtime | Laxity | Urgency Score | Hybrid Score | Dispatch Order |
|-----|----------|-------------------|-------------|--------|--------------|-------------|----------------|
| A | 900 | 10 min | 5 min | 5 min | 0.97 | 0.046 | 1st |
| B | 500 | 30 min | 5 min | 25 min | 0.88 | 0.203 | 3rd |
| C | 800 | 15 min | 10 min | 5 min | 0.95 | 0.087 | 2nd |
| D | 300 | 60 min | 5 min | 55 min | 0.72 | 0.354 | 4th |
| E | 900 | 2 hours | 30 min | 90 min | 0.35 | 0.428 | 5th |

**Failure Modes:**
- **Score recalculation falls behind:** If the recalculation takes > 60 seconds (many queued jobs), some scores may be stale. The impact is bounded: at worst, a job is dispatched one cycle later than optimal. We parallelize recalculation across threads for large queues.
- **Hybrid score favors priority over deadline:** The weights (0.6 urgency, 0.3 priority) are tunable. If too many deadline misses occur, increase urgency weight. If high-priority non-deadline jobs are starved, increase priority weight. We A/B test weight configurations and measure deadline hit rate.

**Interviewer Q&A:**

**Q1: Why not use pure EDF (always nearest deadline first)?**
A: Pure EDF ignores business value. A priority-900 job with a 2-hour deadline should be scheduled before a priority-100 job with a 1-hour deadline (assuming both are feasible). The hybrid score balances urgency with priority. In the extreme case (deadline imminent), urgency dominates and the job is effectively EDF-scheduled.

**Q2: Is EDF optimal for multi-processor (distributed) scheduling?**
A: No. EDF is optimal for single-processor preemptive scheduling (Liu & Layland, 1973). For multi-processor, EDF can cause "Dhall's effect" where a single long job prevents other short jobs from meeting deadlines. Our hybrid approach mitigates this by incorporating laxity (not just deadline) and by using the multi-level priority queue for non-deadline jobs.

**Q3: How do you handle jobs without deadlines in the same dispatch cycle?**
A: Deadline jobs and non-deadline jobs are in separate queues. The dispatch engine allocates a portion of each dispatch batch to deadline jobs (proportional to the urgency level). If all deadline jobs have high laxity (lots of slack), the deadline allocation shrinks. If deadline jobs are at risk, the allocation grows up to 100%. Non-deadline jobs use the priority-based scheduler's weighted fair queuing.

**Q4: How often should urgency scores be recalculated?**
A: Every 60 seconds. The urgency function is exponential: most of the score change happens as the deadline approaches. A job with 24 hours of slack barely changes in 60 seconds. A job with 10 minutes of slack changes significantly. For jobs with < 10 minutes of laxity, we recalculate every 10 seconds (accelerated check for at-risk jobs).

**Q5: What happens if two jobs have the same hybrid score?**
A: Tie-breaking order: (1) HARD deadline before SOFT deadline. (2) Lower laxity (less slack) first. (3) Earlier submission time (FIFO among ties). This ensures that in equal-urgency situations, we favor the most constrained job.

**Q6: How do you prevent deadline-aware scheduling from starving non-deadline jobs?**
A: The dispatch engine reserves a minimum 20% of dispatch capacity for non-deadline jobs (configurable). Even under heavy deadline load, batch and normal-priority non-deadline jobs get some throughput. Additionally, deadline jobs with high laxity (> 1 hour) are treated as normal-priority for scheduling purposes.

---

### 6.3 SLA Monitor & Violation Predictor

**Why it's hard:** Predicting SLA violations requires real-time estimation of job progress, which is not always observable (some jobs don't report progress). The predictor must minimize false positives (unnecessary escalation) and false negatives (missed violations). It must also distinguish between transient delays (GC pause, network blip) and genuine deadline risks.

**Implementation:**

```python
class SLAMonitor:
    """
    Periodically scans all active deadline jobs and predicts SLA violations.
    Runs every 60 seconds on the scheduler leader.
    """
    
    SCAN_INTERVAL_SECONDS = 60
    
    # Thresholds for risk levels
    AT_RISK_THRESHOLD = 0.80    # Job needs > 80% of remaining time
    IMMINENT_THRESHOLD = 0.95   # Job needs > 95% of remaining time
    
    def __init__(self, deadline_repo, execution_repo, runtime_estimator,
                 alert_service, preemption_engine, metrics):
        self.deadline_repo = deadline_repo
        self.execution_repo = execution_repo
        self.estimator = runtime_estimator
        self.alert_service = alert_service
        self.preemption_engine = preemption_engine
        self.metrics = metrics
    
    def scan(self):
        """Main scan: evaluate all active deadline jobs."""
        now = datetime.utcnow()
        
        # Get all active deadline jobs (QUEUED, RUNNING)
        active_deadline_jobs = self.deadline_repo.find_active_deadlines()
        
        at_risk_count = 0
        violation_count = 0
        
        for dl_job in active_deadline_jobs:
            assessment = self._assess_job(dl_job, now)
            
            if assessment.status == 'VIOLATED':
                self._handle_violation(dl_job, assessment)
                violation_count += 1
            elif assessment.status == 'AT_RISK':
                self._handle_at_risk(dl_job, assessment)
                at_risk_count += 1
            elif assessment.status == 'IMMINENT':
                self._handle_imminent(dl_job, assessment)
                at_risk_count += 1
            
            # Update job's feasibility status
            self.deadline_repo.update_feasibility(
                dl_job.job_id, assessment.status, assessment.laxity_ms)
        
        self.metrics.gauge('sla.at_risk_jobs', at_risk_count)
        self.metrics.gauge('sla.violated_jobs', violation_count)
    
    def _assess_job(self, dl_job, now) -> DeadlineAssessment:
        """Assess a single job's deadline status."""
        deadline = dl_job.deadline
        time_remaining_ms = (deadline - now).total_seconds() * 1000
        
        # Already past deadline?
        if time_remaining_ms <= 0:
            execution = self.execution_repo.get_latest(dl_job.job_id)
            if execution and execution.status in ('SUCCEEDED',):
                # Completed (check if before or after deadline)
                completed_at = execution.completed_at
                if completed_at <= deadline:
                    return DeadlineAssessment(status='MET', laxity_ms=0)
                else:
                    overdue = (completed_at - deadline).total_seconds() * 1000
                    return DeadlineAssessment(
                        status='VIOLATED', laxity_ms=-int(overdue),
                        overdue_ms=int(overdue))
            else:
                return DeadlineAssessment(
                    status='VIOLATED', laxity_ms=-int(abs(time_remaining_ms)),
                    overdue_ms=int(abs(time_remaining_ms)))
        
        # Estimate remaining work
        execution = self.execution_repo.get_latest(dl_job.job_id)
        
        if execution is None or execution.status == 'QUEUED':
            # Not started yet: need queue_wait + full_runtime
            queue_wait = self._estimate_queue_wait(dl_job)
            remaining_work_ms = queue_wait + dl_job.estimated_runtime_ms
        elif execution.status == 'RUNNING':
            # Running: need estimated_remaining_runtime
            elapsed_ms = (now - execution.started_at).total_seconds() * 1000
            # Re-estimate remaining based on elapsed
            remaining_work_ms = self._estimate_remaining(dl_job, elapsed_ms)
        else:
            # Terminal state
            return DeadlineAssessment(status='MET' if execution.status == 'SUCCEEDED' else 'VIOLATED',
                                      laxity_ms=0)
        
        laxity_ms = time_remaining_ms - remaining_work_ms
        utilization = remaining_work_ms / time_remaining_ms  # > 1 means impossible
        
        if utilization > 1.0:
            return DeadlineAssessment(status='INFEASIBLE', laxity_ms=int(laxity_ms))
        elif utilization > self.IMMINENT_THRESHOLD:
            return DeadlineAssessment(status='IMMINENT', laxity_ms=int(laxity_ms))
        elif utilization > self.AT_RISK_THRESHOLD:
            return DeadlineAssessment(status='AT_RISK', laxity_ms=int(laxity_ms))
        else:
            return DeadlineAssessment(status='FEASIBLE', laxity_ms=int(laxity_ms))
    
    def _estimate_remaining(self, dl_job, elapsed_ms: float) -> float:
        """
        Estimate remaining runtime for a running job.
        Uses both original estimate and elapsed time for Bayesian update.
        """
        original_estimate = dl_job.estimated_runtime_ms
        
        if elapsed_ms >= original_estimate * 0.9:
            # Job has already taken 90% of estimated time but isn't done.
            # It's likely slower than estimated. Add 50% buffer.
            return original_estimate * 0.5  # Optimistic: 50% more
        
        # Simple model: assume linear progress
        # remaining = original * (1 - elapsed/original)
        progress_ratio = min(elapsed_ms / original_estimate, 0.99)
        remaining = original_estimate * (1 - progress_ratio)
        
        return remaining
    
    def _handle_at_risk(self, dl_job, assessment):
        """Handle a job that's at risk of missing its deadline."""
        if dl_job.deadline_type == 'HARD':
            # Escalate scheduling priority
            urgency_boost = 200  # Boost priority by 200
            self.priority_service.boost(dl_job.job_id, urgency_boost,
                reason='Deadline AT_RISK escalation')
            
            # If queued, attempt to fast-track dispatch
            execution = self.execution_repo.get_latest(dl_job.job_id)
            if execution and execution.status == 'QUEUED':
                # Move to front of dispatch queue
                self.dispatch_engine.prioritize(dl_job.job_id)
        
        # Record prediction
        self._record_violation_prediction(dl_job, assessment)
        
        # Alert (soft: notification, not page)
        self.alert_service.notify(
            tenant=dl_job.tenant_id,
            severity='WARNING',
            message=f"Job {dl_job.job_name} at risk of missing deadline. "
                    f"Laxity: {assessment.laxity_ms}ms. "
                    f"Deadline: {dl_job.deadline}")
    
    def _handle_imminent(self, dl_job, assessment):
        """Handle a job with imminent deadline violation."""
        if dl_job.deadline_type == 'HARD':
            # Attempt preemption to free resources
            if dl_job.execution_status == 'QUEUED':
                self.preemption_engine.preempt_for_deadline(dl_job)
            elif dl_job.execution_status == 'RUNNING':
                # Try to add more resources (if job supports parallel execution)
                # Otherwise, just alert
                pass
        
        # Page on-call for HARD deadlines
        if dl_job.deadline_type == 'HARD':
            self.alert_service.page(
                tenant=dl_job.tenant_id,
                severity='CRITICAL',
                message=f"IMMINENT SLA VIOLATION: Job {dl_job.job_name} "
                        f"will miss HARD deadline {dl_job.deadline}. "
                        f"Laxity: {assessment.laxity_ms}ms.")
    
    def _handle_violation(self, dl_job, assessment):
        """Handle an actual deadline violation."""
        # Record violation
        violation = SLAViolation(
            job_id=dl_job.job_id,
            tenant_id=dl_job.tenant_id,
            deadline=dl_job.deadline,
            deadline_type=dl_job.deadline_type,
            violation_type='ACTUAL',
            actual_violation_at=datetime.utcnow(),
            time_overdue_ms=assessment.overdue_ms,
            root_cause=self._classify_root_cause(dl_job)
        )
        self.violation_repo.save(violation)
        
        # Escalation based on deadline type
        if dl_job.deadline_type == 'HARD':
            self.alert_service.create_incident(
                title=f"HARD SLA Violation: {dl_job.job_name}",
                severity='P2',
                tenant=dl_job.tenant_id,
                details=f"Deadline: {dl_job.deadline}, Overdue: {assessment.overdue_ms}ms")
        
        self.metrics.counter('sla.violation',
            'type', dl_job.deadline_type,
            'tenant', dl_job.tenant_id).increment()
    
    def _classify_root_cause(self, dl_job) -> str:
        """Auto-classify the root cause of a deadline violation."""
        execution = self.execution_repo.get_latest(dl_job.job_id)
        
        if execution is None:
            return 'NEVER_STARTED'
        
        if execution.status == 'FAILED':
            return f'JOB_FAILURE: {execution.error_message[:100]}'
        
        if execution.status == 'RUNNING':
            elapsed = (datetime.utcnow() - execution.started_at).total_seconds() * 1000
            if elapsed > dl_job.estimated_runtime_ms * 2:
                return 'RUNTIME_EXCEEDED_2X_ESTIMATE'
            else:
                return 'SLOWER_THAN_ESTIMATED'
        
        if execution.status == 'QUEUED':
            queue_time = (datetime.utcnow() - execution.queued_at).total_seconds() * 1000
            return f'EXCESSIVE_QUEUE_WAIT: {int(queue_time)}ms'
        
        return 'UNKNOWN'
```

**Failure Modes:**
- **False positive (job predicted to miss but actually completes on time):** We use conservative thresholds (80% utilization for AT_RISK). If false positive rate exceeds 10%, we adjust thresholds. Alert fatigue is mitigated by only paging for IMMINENT (95%) hard deadlines.
- **False negative (job misses deadline without prediction):** Possible if the job's runtime suddenly spikes (e.g., 10x normal due to data size increase). We mitigate by re-estimating remaining runtime based on elapsed time (if a job has already taken 90% of its estimate but isn't done, we flag it).
- **SLA Monitor crash during scan:** Scan is stateless; new leader resumes from scratch. No scan state is persisted between cycles.

**Interviewer Q&A:**

**Q1: How do you handle jobs that don't report progress?**
A: Most infrastructure jobs are opaque (no progress reporting). We rely on elapsed time vs. estimated runtime as a proxy for progress. If `elapsed / estimated > 0.8` and the job isn't done, we mark it AT_RISK. For jobs that do report progress (e.g., data migration: 60% complete), we use the reported progress for more accurate prediction.

**Q2: What's the prediction accuracy for your SLA monitor?**
A: We measure: (1) True positive rate: % of actual violations that were predicted > 10 min in advance. Target: 95%. (2) False positive rate: % of predicted violations that didn't occur. Target: < 15%. (3) Prediction lead time: median time between first AT_RISK prediction and actual violation. Target: > 15 min. We tune thresholds monthly based on these metrics.

**Q3: How do you distinguish between "job is slow" and "job will miss deadline"?**
A: A slow job is not necessarily at risk if it has ample laxity. We compute `utilization = remaining_work / remaining_time`. A job using 50% utilization is slow but safe. A job using 90% utilization is at risk. The key insight: deadline risk is about the ratio of remaining work to remaining time, not absolute speed.

**Q4: How do you handle cascading violations (one late job causes downstream jobs to miss their deadlines)?**
A: If a deadline job is part of a DAG, the SLA Monitor traces downstream dependencies. If job A is AT_RISK and job B depends on A with its own deadline, we propagate the risk: B's effective start time is A's estimated completion time. If B's new estimated completion exceeds its deadline, B is also marked AT_RISK. We alert on the root cause (A) to focus remediation.

**Q5: What escalation actions does the SLA Monitor take for hard deadlines?**
A: Escalation ladder: (1) AT_RISK (> 80% utilization): boost scheduling priority by +200, notify owning team. (2) IMMINENT (> 95% utilization): trigger preemption, page on-call SRE, reserve burst capacity. (3) VIOLATED (deadline passed): create incident ticket, notify SLA stakeholders, begin post-mortem process. Each level is more aggressive to match the severity.

**Q6: How do you calculate SLA metrics for reporting?**
A: `Deadline adherence rate = (total deadline jobs - violations) / total deadline jobs * 100%`. Computed per: tenant, job type, deadline type (HARD/SOFT), and time period (hourly/daily/weekly/monthly). We exclude jobs that were cancelled or paused (not counted as violations). The SLA dashboard shows real-time adherence rates with drill-down capability.

---

### 6.4 Resource Reservation for Deadline-Critical Jobs

**Why it's hard:** Reserving capacity for future jobs means those resources are unavailable for current jobs, reducing overall utilization. Over-reservation wastes capacity; under-reservation leads to deadline misses. Reservations must be time-bounded, cancellable, and composable (multiple reservations on the same pool).

**Implementation:**

```java
@Service
public class ResourceReservationManager {
    
    // Max reservation as percentage of pool capacity
    private static final double MAX_RESERVATION_RATIO = 0.70;
    // Reserved pool name
    private static final String RESERVED_POOL = "reserved";
    
    @Autowired private ResourceReservationRepository reservationRepo;
    @Autowired private WorkerNodeRepository workerRepo;
    @Autowired private MeterRegistry metrics;
    
    /**
     * Attempt to reserve resources for a hard-deadline job.
     * Returns the reservation if successful, null if insufficient capacity.
     */
    @Transactional
    public ResourceReservation reserve(int cpuMillis, int memoryMb, int gpu,
                                         String pool, Instant from, Instant until) {
        // 1. Get total pool capacity
        PoolCapacity capacity = workerRepo.getPoolCapacity(pool);
        
        // 2. Get existing reservations overlapping with the requested time window
        List<ResourceReservation> overlapping = reservationRepo
            .findOverlapping(pool, from, until);
        
        // 3. Calculate peak reserved resources during the requested window
        // (simplified: sum all overlapping reservations)
        int peakReservedCpu = overlapping.stream()
            .mapToInt(ResourceReservation::getReservedCpuMillis).sum() + cpuMillis;
        int peakReservedMem = overlapping.stream()
            .mapToInt(ResourceReservation::getReservedMemoryMb).sum() + memoryMb;
        
        // 4. Check against max reservation ratio
        if (peakReservedCpu > capacity.getTotalCpuMillis() * MAX_RESERVATION_RATIO
                || peakReservedMem > capacity.getTotalMemoryMb() * MAX_RESERVATION_RATIO) {
            log.info("Reservation rejected: would exceed {}% of pool {} capacity",
                MAX_RESERVATION_RATIO * 100, pool);
            metrics.counter("reservation.rejected", "reason", "capacity").increment();
            return null;
        }
        
        // 5. Create reservation
        ResourceReservation reservation = new ResourceReservation();
        reservation.setReservedCpuMillis(cpuMillis);
        reservation.setReservedMemoryMb(memoryMb);
        reservation.setReservedGpu(gpu);
        reservation.setPoolName(pool);
        reservation.setReservedFrom(from);
        reservation.setReservedUntil(until);
        reservation.setStatus(ReservationStatus.PENDING);
        
        reservationRepo.save(reservation);
        metrics.counter("reservation.created").increment();
        metrics.gauge("reservation.total_cpu_reserved", peakReservedCpu);
        
        return reservation;
    }
    
    /**
     * When a deadline job starts executing, consume its reservation.
     * The reserved capacity is now being used.
     */
    public void consumeReservation(long reservationId) {
        reservationRepo.updateStatus(reservationId, ReservationStatus.CONSUMED);
    }
    
    /**
     * When a deadline job completes or is cancelled, release the reservation.
     */
    public void releaseReservation(long reservationId) {
        reservationRepo.updateStatus(reservationId, ReservationStatus.RELEASED);
        metrics.counter("reservation.released").increment();
    }
    
    /**
     * Expire reservations past their until time.
     * Runs every 5 minutes.
     */
    @Scheduled(fixedRate = 300_000)
    public void expireReservations() {
        int expired = reservationRepo.expireOld(Instant.now());
        if (expired > 0) {
            log.info("Expired {} stale reservations", expired);
            metrics.counter("reservation.expired").increment(expired);
        }
    }
    
    /**
     * Get available (unreserved) capacity for a pool at a given time.
     */
    public PoolCapacity getAvailableCapacity(String pool, Instant at) {
        PoolCapacity total = workerRepo.getPoolCapacity(pool);
        List<ResourceReservation> active = reservationRepo
            .findActiveAt(pool, at);
        
        int reservedCpu = active.stream()
            .mapToInt(ResourceReservation::getReservedCpuMillis).sum();
        int reservedMem = active.stream()
            .mapToInt(ResourceReservation::getReservedMemoryMb).sum();
        
        return new PoolCapacity(
            total.getTotalCpuMillis() - reservedCpu,
            total.getTotalMemoryMb() - reservedMem,
            total.getTotalGpu() // GPU reservations handled separately
        );
    }
}
```

**Interviewer Q&A:**

**Q1: Why limit reservations to 70% of pool capacity?**
A: If we reserve 100%, no non-deadline jobs can run on the reserved pool, and any burst in deadline jobs would have zero headroom. The 30% buffer allows: (1) Handling reservation estimation errors (job takes slightly more resources than reserved). (2) Running maintenance tasks on the reserved pool. (3) Absorbing burst requests without rejecting feasible jobs.

**Q2: What happens if a reservation is made but the job is never submitted?**
A: Reservations have an expiry time (`reserved_until`). If the job isn't submitted before the reservation activates, the reservation sits idle until expiry. The `expireReservations()` sweep cleans up expired entries. To minimize waste, we require that reservations are linked to a job_id at creation time.

**Q3: How do you handle over-provisioned reservations (job finishes faster than expected)?**
A: When the job completes, `releaseReservation()` immediately frees the capacity. The reservation doesn't hold resources until `reserved_until`. This means other deadline jobs can use the freed capacity immediately. We track `reservation_utilization = actual_runtime / reserved_duration` to measure efficiency.

**Q4: Can reservations span multiple workers?**
A: Currently, reservations are pool-level (not worker-specific). The dispatch engine finds a suitable worker within the pool when the job is ready to run. This provides flexibility: if a specific worker is unhealthy, the job can run on any other worker in the pool. For GPU reservations (worker-specific), we do bind to a specific worker.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

Deadline-aware placement extends the base score-based placement:

```python
def score_worker_deadline_aware(worker, job, deadline_info):
    base_score = score_worker_base(worker, job)  # From distributed scheduler
    
    if deadline_info is None:
        return base_score
    
    # Bonus for reserved pool (if reservation exists)
    if deadline_info.reservation_id and worker.pool_name == 'reserved':
        base_score += 50  # Strong preference for reserved pool
    
    # Bonus for fast workers (low average job completion time)
    avg_completion = get_avg_completion_time(worker)
    if avg_completion < deadline_info.estimated_runtime_ms * 0.8:
        base_score += 20  # This worker typically finishes faster
    
    # Penalty for overloaded workers (risk of slowdown)
    if worker.available_cpu_millis < worker.total_cpu_millis * 0.2:
        base_score -= 30  # Worker is > 80% utilized, risky for deadline
    
    return base_score
```

### Preemption Policy (Deadline-Specific)

- **When:** A hard-deadline job's laxity drops below 20% of its estimated runtime AND no workers are available.
- **Who:** Preempt the running job with the most laxity (most time before its own deadline, or no deadline at all).
- **Guard rails:** Never preempt a hard-deadline job for another hard-deadline job unless the preemptor's laxity is < 50% of the victim's laxity. This prevents cascading deadline violations.

### Starvation Prevention

- Non-deadline jobs get a minimum 20% of dispatch capacity regardless of deadline queue depth.
- Deadline jobs with laxity > 1 hour are treated as normal-priority for dispatch purposes.
- Reservation pool is capped at 70% to ensure general pool always has capacity.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling | Trigger |
|-----------|---------|---------|
| Admission Controller | Stateless; HPA on CPU | RPS > 1000 |
| SLA Monitor | Single leader (per partition if sharded) | Scan time > 30s |
| Dispatch Engine | Integrated with scheduler leader | See distributed scheduler |
| Reserved Pool | Add worker nodes | Reserved utilization > 60% |

### Database Scaling

- `job_deadline`: Relatively small (500K active rows). Fits in buffer pool.
- `runtime_estimation`: Partitioned monthly. Heavy reads (runtime estimation queries). Read replicas help.
- `sla_violation`: Small (5K/day). No special scaling needed.
- `resource_reservation`: Small (few thousand active). No special scaling needed.

### Caching

| Layer | Data | TTL |
|-------|------|-----|
| L1: Caffeine | Runtime estimates per job type | 5 min |
| L1: Caffeine | Pool capacity | 30s |
| L2: Redis | Deadline queue (sorted set) | N/A |
| L2: Redis | Active reservations | Updated on change |

### Kubernetes-Specific

```yaml
# Dedicated node pool for deadline-critical workers
apiVersion: v1
kind: Node
metadata:
  labels:
    pool: reserved
    deadline-tier: critical
spec:
  taints:
  - key: "deadline-tier"
    value: "critical"
    effect: "NoSchedule"    # Only deadline-critical pods scheduled here
```

### Interviewer Q&A

**Q1: How do you scale the reserved pool without over-provisioning?**
A: We track `reservation_utilization = peak_reserved / total_reserved_capacity` over time. If utilization > 70% consistently, we add nodes to the reserved pool. If < 30%, we release nodes back to the general pool. This is a slow feedback loop (evaluated daily) to prevent thrashing.

**Q2: What's the overhead of running the SLA Monitor on 500K active deadline jobs?**
A: Each assessment: 1 MySQL read (job_deadline) + 1 MySQL read (latest execution) + urgency calculation. At ~0.5ms per job, 500K jobs take ~250 seconds for a full scan. This exceeds the 60-second scan interval. Solution: scan only jobs with `laxity_ms < 3600000` (< 1 hour laxity) every 60 seconds, and scan all jobs every 5 minutes. The at-risk subset is typically < 5K jobs (0.1% of total), taking ~2.5 seconds.

**Q3: How do you handle burst deadline submissions (e.g., 10K compliance reports all due at the same time)?**
A: (1) Feasibility check admits only as many as can feasibly complete. (2) Reservations are first-come-first-served; once the reserved pool is 70% reserved, new reservations are rejected. (3) Remaining jobs are admitted as SOFT deadline (downgraded from HARD if cluster is overcommitted). (4) The SLA Monitor prioritizes jobs with least laxity during the burst.

**Q4: How does the deadline scheduler interact with the cron service?**
A: Cron jobs can have deadlines: "run at 2 AM, must complete by 4 AM." The cron service submits the triggered job with `deadline = 2026-04-10T04:00:00Z`. The deadline scheduler handles feasibility, scheduling, and SLA monitoring from that point. If the cron job consistently misses its 4 AM deadline, the SLA dashboard shows a pattern, prompting investigation (perhaps the 2 AM start is too late or the job needs more resources).

**Q5: What's the cost of maintaining a reserved pool?**
A: Reserved pool capacity is "always on" even if not fully utilized. If the pool has 100 workers and average utilization is 50%, that's 50 idle workers. To minimize cost: (1) Right-size reservations (P80 estimate, not P99). (2) Allow non-deadline jobs to use unreserved capacity on the reserved pool (with immediate preemption if a deadline job needs the resources). (3) Auto-scale the reserved pool based on upcoming reservation demand.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| SLA Monitor crash | Violation prediction stops | Leader election (15s) | Standby takes over | 15-20s | 1 missed scan |
| Feasibility check failure | New deadline jobs rejected | API 500 errors | Admit with warning (degraded mode) | Instant (fallback) | 0 |
| Reservation database failure | Can't create/release reservations | MySQL health check | Reservations continue (held in memory); flush on recovery | 30-60s | Possible stale reservations |
| Reserved pool worker failure | Fewer resources for deadline jobs | Heartbeat timeout | Re-dispatch to general pool; reservation re-assigned | 60-90s | Job restart |
| Runtime estimation wrong (2x actual) | Jobs predicted feasible but actually infeasible | SLA Monitor detects at runtime | Escalation, preemption, priority boost | Minutes | Potential violation |
| Thundering deadline (many jobs same deadline) | Resource contention, some violations | Queue depth spike | Preemption + burst capacity + graceful degradation | Varies | Some violations |

### Automated Recovery

```java
@Scheduled(fixedRate = 300_000) // Every 5 minutes
public void recoverOrphanedReservations() {
    // Reservations for jobs that completed/cancelled but reservation not released
    List<ResourceReservation> orphaned = reservationRepo.findOrphanedReservations();
    for (ResourceReservation r : orphaned) {
        log.warn("Releasing orphaned reservation {} for job {}", r.getId(), r.getJobId());
        releaseReservation(r.getId());
    }
}
```

### Circuit Breaker

- **Feasibility check circuit breaker:** If > 50% of feasibility checks timeout (MySQL slow), admit jobs without feasibility check (degraded mode: accept all, rely on SLA Monitor for runtime prediction).
- **Reservation circuit breaker:** If reservation DB is down, skip reservations and admit hard-deadline jobs to general pool with preemption rights.

---

## 10. Observability

### Key Metrics

| Metric | Type | Warning | Critical |
|--------|------|---------|----------|
| `deadline.adherence.hard_pct` | Gauge | < 99.95% | < 99.9% |
| `deadline.adherence.soft_pct` | Gauge | < 97% | < 95% |
| `deadline.at_risk.count` | Gauge | > 50 | > 200 |
| `deadline.violation.count` (per hour) | Counter | > 0 (hard) | > 5 (hard) |
| `deadline.laxity.p10_ms` | Histogram | < 5 min | < 1 min |
| `deadline.feasibility_check.latency_ms` | Histogram | P99 > 500ms | P99 > 2s |
| `deadline.reservation.utilization_pct` | Gauge | < 30% or > 80% | < 10% or > 90% |
| `deadline.prediction.accuracy_pct` | Gauge | < 90% | < 80% |
| `deadline.prediction.lead_time_min` | Histogram | P50 < 10 min | P50 < 5 min |

### Distributed Tracing

Deadline-specific span attributes:
```
span.attributes:
  job.deadline: "2026-04-09T16:00:00Z"
  job.deadline_type: HARD
  job.laxity_ms: 4680000
  job.urgency_score: 0.45
  job.feasibility_status: FEASIBLE
  job.reservation_id: 9001
  sla.at_risk: false
```

### Alerting

| Alert | Condition | Severity | Action |
|-------|-----------|----------|--------|
| Hard deadline violation | Any hard deadline missed | P2 | Auto-create incident |
| Hard deadline imminent | Laxity < 5 min for hard deadline | P1 | Page on-call |
| Multiple soft deadline violations | > 10 in 1 hour | P3 | Investigate root cause |
| Prediction accuracy degraded | < 80% for 24 hours | P3 | Retrain runtime model |
| Reserved pool exhausted | > 90% reserved | P2 | Scale reserved pool |

---

## 11. Security

Same security model as other schedulers. Deadline-specific:

### Authorization

```
Additional permissions:
  deadline-submit-hard:   Submit jobs with HARD deadlines (requires SLA contract)
  deadline-admin:         Override deadlines, modify reservations, manage SLA policies
  sla-viewer:            View SLA dashboard and violation reports
```

HARD deadline submission requires contractual SLA agreement (tracked in tenant metadata). This prevents casual use of expensive guaranteed capacity.

### Audit

SLA violations are audit events with full traceability: who submitted, what deadline, when predicted, when violated, root cause. Immutable log for compliance reporting.

---

## 12. Incremental Rollout Strategy

### Phases

1. **Observation only (Week 1-2):** Deploy SLA Monitor in read-only mode. Compute deadline adherence for existing jobs (retroactively). Establish baseline metrics.
2. **Soft deadlines only (Week 3-4):** Enable soft deadline submission and feasibility checking. No reservations. Monitor prediction accuracy.
3. **Hard deadlines (internal) (Week 5-6):** Enable hard deadlines for platform team's own jobs (cert rotation, backups). Enable reservations on a small reserved pool.
4. **Hard deadlines (all tenants) (Week 7-8):** Open hard deadlines to all tenants. Scale reserved pool. Enable deadline-based preemption.
5. **Tuning (Week 9-10):** Adjust weights, thresholds, and reservation caps based on production data.

### Rollback Strategy

- Disable feasibility checking: all jobs accepted without checks
- Disable reservations: release all reservations, deadline jobs use general pool
- Disable deadline preemption: deadline jobs wait in normal priority queue
- Disable SLA Monitor: no predictions or alerts (revert to manual SLA tracking)
- Each component can be disabled independently

### Rollout Interviewer Q&A

**Q1: How do you establish baseline metrics before the system is active?**
A: We retroactively annotate existing jobs with deadlines (based on historical SLA expectations). For example, nightly backups historically complete by 6 AM -- we add a 6 AM soft deadline. The SLA Monitor in observation mode calculates what the adherence rate would have been, giving us a baseline to compare against.

**Q2: What if the feasibility checker rejects too many jobs initially?**
A: Start with permissive thresholds (accept if P95 estimate < deadline, not P80). Gradually tighten as runtime estimates improve. Log rejections with reasons so we can analyze: is the estimator too conservative, or are users setting unrealistic deadlines?

**Q3: How do you train the runtime estimator before deployment?**
A: We bootstrap from existing job execution history. For each job type, we compute P50/P80/P95/P99 from historical `job_execution.runtime_ms`. This gives us an initial model without needing any ML training. After deployment, new execution data continuously improves the estimates.

**Q4: What's the risk of deadline-based preemption causing cascading failures?**
A: We guard against this with: (1) Never preempt a hard-deadline job. (2) Limit preemption rate (100/hour). (3) Preemption only if priority delta >= 200. (4) Re-queue preempted jobs (they're not lost). (5) Circuit breaker: if preemption rate exceeds threshold, stop preempting.

**Q5: How do you handle SLA reporting for compliance (e.g., PCI-DSS audit)?**
A: The SLA violation table provides a complete, immutable record. We generate monthly SLA reports: adherence rate per tenant, per job type, with details of every violation (timestamp, duration, root cause, remediation). Reports are exportable in CSV/PDF and backed by the audit log.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Selected | Trade-off | Rationale |
|----------|---------|----------|-----------|-----------|
| Scheduling algorithm | Pure EDF, Pure priority, Hybrid | Hybrid EDF + Priority | Neither pure approach is optimal | Hybrid balances urgency with business value |
| Feasibility check approach | Simple, queue-aware, simulation, ML | Queue-aware + reservations | Complexity vs. accuracy | Reservations provide hard guarantees for HARD deadlines |
| Reservation policy | No reservation, on-demand, pre-reserved pool | Pre-reserved pool (70% cap) | Capacity cost vs. guarantee | Worth the cost for HARD deadline compliance |
| Prediction approach | Percentile-based, ML regression, Bayesian | Percentile-based (P80) | Simplicity vs. accuracy | Interpretable, no ML infrastructure required; upgrade path to ML exists |
| Urgency score function | Linear, exponential, step | Exponential decay | Harder to explain vs. better urgency modeling | Exponential correctly models increasing urgency near deadline |
| SLA Monitor frequency | 10s, 30s, 60s | 60s (general), 10s (at-risk) | More frequent = more CPU vs. earlier detection | Adaptive: scan at-risk frequently, rest infrequently |

---

## 14. Agentic AI Integration

### Where AI Adds Value

| Use Case | AI Capability | Impact |
|----------|--------------|--------|
| **Runtime prediction** | ML model (gradient boosted trees) predicts runtime from job features | 30% better prediction accuracy vs. percentile-based |
| **Deadline suggestion** | LLM analyzes job characteristics and historical SLA data to suggest appropriate deadline | Reduce infeasible submissions by 50% |
| **Root cause analysis** | LLM correlates SLA violations with system events (deploys, outages, load spikes) | Automated RCA in minutes vs. hours |
| **Capacity planning** | Predict future reservation demand from deadline submission patterns | Right-size reserved pool |
| **SLA report generation** | LLM generates natural-language SLA reports from violation data | Save 2 hours/week of SRE time |

### Agent Loop Example: Automated RCA

```python
class SLAViolationRCAAgent:
    """Automatically determines root cause of SLA violations."""
    
    def observe(self, violation_id):
        violation = violation_repo.get(violation_id)
        job = job_repo.get(violation.job_id)
        execution = execution_repo.get_latest(violation.job_id)
        
        return {
            'violation': violation,
            'job': job,
            'execution': execution,
            'system_events': event_repo.get_events_around(
                violation.deadline, window_minutes=60),
            'worker_metrics': metrics_api.query(
                f'node_cpu_usage{{worker="{execution.worker_id}"}}',
                range=f'{execution.started_at}/{violation.deadline}'),
            'similar_violations': violation_repo.find_similar(
                job_type=job.job_type, period_days=30)
        }
    
    def reason(self, observation):
        prompt = f"""
        Analyze this SLA violation and determine root cause.
        
        Violation: Job '{observation['job'].job_name}' missed HARD deadline 
        {observation['violation'].deadline} by {observation['violation'].time_overdue_ms}ms.
        
        Execution: Started {observation['execution'].started_at}, 
        expected runtime {observation['job'].estimated_runtime_ms}ms,
        actual runtime {observation['execution'].runtime_ms}ms.
        
        System events around the time:
        {json.dumps(observation['system_events'][:20])}
        
        Worker CPU during execution:
        Average: {observation['worker_metrics']['avg']}%
        P99: {observation['worker_metrics']['p99']}%
        
        Similar violations in last 30 days: {len(observation['similar_violations'])}
        
        Classify root cause as one of:
        - RUNTIME_REGRESSION: Job took longer than historical baseline
        - RESOURCE_CONTENTION: Worker was overloaded
        - QUEUE_DELAY: Job waited too long before starting
        - ESTIMATION_ERROR: Runtime estimate was too optimistic
        - INFRASTRUCTURE_EVENT: Correlated with deployment/outage
        - DATA_VOLUME: Input data was larger than expected
        
        Output: {{"root_cause": str, "confidence": float, "explanation": str, 
                  "recommendation": str}}
        """
        return self.llm.generate(prompt, temperature=0.1)
    
    def act(self, violation_id, analysis):
        # Update violation with root cause
        violation_repo.update_root_cause(
            violation_id, 
            root_cause=analysis['root_cause'],
            explanation=analysis['explanation'])
        
        # Take corrective action based on root cause
        if analysis['root_cause'] == 'ESTIMATION_ERROR' and analysis['confidence'] > 0.8:
            # Adjust runtime estimate for this job type
            job = job_repo.get(violation_repo.get(violation_id).job_id)
            runtime_estimator.invalidate_cache(job.job_type_key)
            # Next estimation will use updated historical data
        
        elif analysis['root_cause'] == 'RESOURCE_CONTENTION':
            # Suggest scaling the reserved pool
            notify_api.send(
                channel='#capacity-planning',
                message=f"SLA violation due to resource contention. "
                        f"Consider increasing reserved pool capacity. "
                        f"Recommendation: {analysis['recommendation']}")
    
    def verify(self, violation_id, analysis):
        # Check if the same root cause appears in subsequent violations
        # If not, the fix is working
        pass
```

### Guard Rails

- AI never modifies deadlines or SLA policies (read-only + suggestions)
- AI RCA requires human review before inclusion in formal SLA reports
- Runtime estimate adjustments are bounded (max 2x change per day)
- Kill switch for all AI actions

---

## 15. Complete Interviewer Q&A Bank

**Q1: What is laxity and why is it more useful than time-to-deadline?**
A: Laxity = (deadline - now) - estimated_remaining_runtime. It represents "how much slack do we have." A job with 60 minutes to deadline but 59 minutes of remaining work has only 1 minute of laxity (extremely urgent). A job with 10 minutes to deadline but only 1 minute of remaining work has 9 minutes of laxity (comfortable). Time-to-deadline alone doesn't account for work remaining.

**Q2: How does your system handle a hard deadline that's only 30 seconds away?**
A: The feasibility checker evaluates whether the job can complete in 30 seconds (estimated runtime < 30s). If yes: accept, skip the queue entirely (immediate dispatch to reserved pool), highest urgency score. If no: reject at submission with explanation. We don't accept infeasible hard deadlines because it would waste resources on a job that's guaranteed to violate.

**Q3: What's the theoretical utilization limit for EDF scheduling?**
A: For a single processor with periodic tasks, EDF achieves 100% utilization (Liu & Layland, 1973). For our multi-processor system, the theoretical bound is lower. Coffman's anomaly shows that adding processors or reducing execution times can actually increase completion times under certain scheduling algorithms. Our hybrid approach + admission control ensures we don't overcommit: we admit jobs only if feasible, keeping effective utilization below the point where deadline violations occur.

**Q4: How do you handle deadline extensions (user asks for more time)?**
A: `POST /api/v1/admin/deadline-override` with `new_deadline` and `reason`. The system re-evaluates feasibility with the new deadline. If the job's reservation exists, it's extended. If the job was AT_RISK, it may return to FEASIBLE. Deadline extensions are audited and require `deadline-admin` role.

**Q5: What if the runtime estimator's P80 is consistently wrong (e.g., always 50% too low)?**
A: We track estimation accuracy: `estimation_error = actual_runtime / estimated_runtime`. If the median error is consistently > 1.3 (30% underestimate), we apply a correction factor to future estimates. The SLA Monitor also detects this pattern: if many jobs of the same type are AT_RISK at the same point in execution, it suggests the estimate is too low. After 100 samples, the correction auto-calibrates.

**Q6: How do you differentiate between "deadline not met because job failed" vs. "deadline not met because job was too slow"?**
A: The SLA violation record includes `root_cause`. `JOB_FAILURE` means the job failed (non-zero exit code, OOM, etc.) before the deadline. `SLOWER_THAN_ESTIMATED` means the job was still running when the deadline passed. `EXCESSIVE_QUEUE_WAIT` means the job never started in time. Each root cause has different remediation: failure = fix the code, slow = increase resources, queue wait = increase capacity or priority.

**Q7: How does deadline awareness interact with the DAG scheduler?**
A: DAG tasks can have individual deadlines (each task has its own deadline constraint). The DAG-level deadline is typically the last task's deadline. The critical path calculation considers deadlines: if task B has a 2 PM deadline and depends on task A which takes 1 hour, task A must start by 1 PM at the latest. The deadline scheduler ensures A is dispatched with urgency proportional to B's deadline.

**Q8: What's the overhead of maintaining a separate reserved pool?**
A: If the reserved pool is 10% of total capacity (500 workers out of 5000), the direct cost is those 500 workers. The indirect cost is lower utilization: reserved workers may be idle when no deadline jobs need them. To mitigate: allow non-deadline jobs to use unreserved capacity (with immediate preemption rights for deadline jobs). Effective utilization of the reserved pool is typically 60-80% vs. 85-95% for the general pool.

**Q9: How do you handle deadline jobs that are CPU-bound vs. I/O-bound differently?**
A: CPU-bound jobs have more predictable runtimes (linear with compute allocation). I/O-bound jobs depend on external factors (database latency, network throughput) that are harder to predict. For I/O-bound jobs: (1) runtime estimates use higher confidence intervals (P95 instead of P80), (2) SLA Monitor checks more frequently (30s instead of 60s), (3) feasibility check adds a larger buffer.

**Q10: Can deadline awareness be applied retroactively to running jobs?**
A: Yes. `PATCH /api/v1/jobs/{id}/deadline` adds a deadline to a running job. The system immediately: calculates laxity based on elapsed time, estimates remaining runtime, evaluates feasibility, and adds the job to the SLA Monitor's scan. If the deadline is infeasible (job has already run longer than deadline allows), we alert immediately.

**Q11: How do you handle timezone for deadlines?**
A: Deadlines are always stored and compared in UTC. The API accepts deadlines in any timezone (with timezone offset) and converts to UTC. Display back to users in their configured timezone. This avoids all DST issues: the deadline is an absolute point in time.

**Q12: What's the blast radius if the reserved pool's networking fails?**
A: All jobs on the reserved pool lose connectivity. Workers report heartbeat failure; scheduler marks jobs as FAILED. The SLA Monitor detects all reserved-pool deadline jobs transitioning to AT_RISK/VIOLATED simultaneously and sends a P1 alert. The scheduler attempts to re-dispatch affected deadline jobs to the general pool (which is on a different network segment). Blast radius: all hard-deadline jobs currently on the reserved pool.

**Q13: How do you measure the business impact of deadline misses?**
A: Each deadline type maps to a business impact category: (1) Compliance deadlines (regulatory) = potential fines/audit findings. (2) SLA deadlines (customer-facing) = SLA credit/penalty. (3) Internal deadlines (operational) = degraded service quality. The SLA dashboard includes estimated business impact: `violations * cost_per_violation` (configured per tenant/job type).

**Q14: How does your system compare to Google Borg's deadline-aware scheduling?**
A: Borg uses a two-level approach: (1) "prod" (production) priority for latency-sensitive work with implicit deadlines, and (2) "batch" for throughput-oriented work. Our system is more explicit: jobs declare specific deadlines, and the scheduler reasons about them directly. Borg relies on priority + preemption; we add feasibility checking, runtime prediction, and proactive SLA violation detection. Our approach provides more visibility into deadline adherence.

**Q15: What's the recovery plan for a mass SLA violation event (e.g., 100 hard deadlines missed simultaneously)?**
A: (1) Immediate: auto-create a P1 incident. SLA Monitor identifies the common cause (e.g., cluster-wide network issue, MySQL outage). (2) Short-term: page on-call SRE and infrastructure team. Identify and fix root cause. (3) Medium-term: for jobs with `FIRE_NOW` misfire policy, re-execute them immediately with boosted priority. (4) Long-term: post-mortem, update reservation capacity, adjust feasibility thresholds, improve root cause detection. All 100 violations are recorded in the SLA violation table for compliance reporting.

---

## 16. References

- [Liu & Layland, "Scheduling Algorithms for Multiprogramming in a Hard-Real-Time Environment"](https://dl.acm.org/doi/10.1145/321738.321743) (JACM, 1973)
- [Earliest Deadline First (EDF) Scheduling](https://en.wikipedia.org/wiki/Earliest_deadline_first_scheduling)
- [Least Laxity First (LLF) Scheduling](https://en.wikipedia.org/wiki/Least_slack_time_scheduling)
- [Google Borg - Priority and Preemption](https://research.google/pubs/pub43438/)
- [Kubernetes Pod Priority and Preemption](https://kubernetes.io/docs/concepts/scheduling-eviction/pod-priority-preemption/)
- [Real-Time Systems and Scheduling (RTOS concepts)](https://www.embedded.com/introduction-to-rate-monotonic-scheduling/)
- [Coffman's Anomaly in Scheduling](https://en.wikipedia.org/wiki/Coffman%E2%80%93Graham_algorithm)
- [SLA Management Best Practices (ITIL)](https://www.axelos.com/certifications/itil-service-management)
- [Redis Sorted Sets for Priority Queues](https://redis.io/docs/data-types/sorted-sets/)
- [Bayesian Runtime Estimation](https://en.wikipedia.org/wiki/Bayesian_estimation)
- [HikariCP Connection Pool Sizing](https://github.com/brettwooldridge/HikariCP/wiki/About-Pool-Sizing)
- [OpenTelemetry SLA Monitoring Patterns](https://opentelemetry.io/docs/)
