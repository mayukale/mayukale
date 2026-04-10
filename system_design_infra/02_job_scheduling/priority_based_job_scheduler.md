# System Design: Priority-Based Job Scheduler

> **Relevance to role:** On a bare-metal IaaS platform, not all workloads are equal -- emergency capacity provisioning, SLA-bound database migrations, and routine health checks must compete for the same pool of compute. A priority-based scheduler ensures that the most important jobs get resources first while preventing starvation of lower tiers, directly impacting platform reliability and customer SLAs.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement | Detail |
|----|------------|--------|
| FR-1 | Multi-level priority | Jobs assigned a priority from 0 (lowest) to 1000 (highest) |
| FR-2 | Priority queues | Separate logical queues per priority band; highest band dequeued first |
| FR-3 | Preemption | High-priority jobs can evict running low-priority jobs when resources are exhausted |
| FR-4 | Priority inversion prevention | If a high-priority job depends on a low-priority job, the low-priority job's effective priority is boosted |
| FR-5 | Anti-starvation (aging) | Waiting jobs gradually increase in effective priority to prevent indefinite starvation |
| FR-6 | Fair scheduling within priority | Within the same priority level, jobs are dispatched fairly across tenants |
| FR-7 | Resource-aware scheduling | Priority determines queue order; resource availability determines feasibility |
| FR-8 | Priority escalation | Jobs approaching their deadline automatically escalate priority |
| FR-9 | Priority quotas | Per-tenant limits on how many high-priority jobs can be submitted |
| FR-10 | Admin override | Platform operators can manually boost or lower any job's priority |

### Non-Functional Requirements
| NFR | Target | Rationale |
|-----|--------|-----------|
| Availability | 99.99% | Scheduler downtime means SLA-critical jobs don't run |
| Scheduling latency | < 200ms for priority 900+ (critical) | Emergency jobs must start immediately |
| Scheduling latency | < 2s for priority 500-899 (normal) | Standard SLA |
| Scheduling latency | < 30s for priority 0-499 (batch) | Batch can tolerate brief waits |
| Preemption latency | < 10s from preemption decision to low-priority job SIGTERM | Must free resources quickly |
| Throughput | 50K scheduling decisions/min | Enterprise platform scale |
| Fairness | No job waits > 4 hours regardless of priority | Starvation prevention guarantee |

### Constraints & Assumptions
- Builds on the distributed job scheduler architecture (see distributed_job_scheduler.md)
- MySQL 8.0 for persistent state; Redis for priority queues and aging
- Workers have heterogeneous resources (CPU, memory, GPU)
- Jobs declare resource requirements at submission
- Preemption is expensive (killed work must be re-done); minimize unnecessary preemptions
- Tenants have Service Level Objectives (SLOs) tied to priority levels

### Out of Scope
- Real-time scheduling guarantees (hard real-time with provable bounds)
- Cross-region priority arbitration
- Cost-aware scheduling (priority != cost)
- Dynamic priority learning (ML-based priority assignment)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Total jobs submitted/day | 20M | Mix of batch, normal, and critical |
| Critical priority (900-1000) | 200K/day (1%) | Emergency provisioning, SLA incidents |
| Normal priority (500-899) | 10M/day (50%) | Standard workloads |
| Batch priority (0-499) | 9.8M/day (49%) | Background maintenance, analytics |
| Peak submissions/min | 50K | 3x average during business hours |
| Concurrent running jobs | 500K | Across all priority levels |
| Workers | 5K | Heterogeneous bare-metal + k8s nodes |
| Preemptions/day | ~5K | ~0.025% of total jobs |

### Latency Requirements

| Operation | Priority Band | P50 | P99 |
|-----------|-------------|-----|-----|
| Priority evaluation | All | 1ms | 5ms |
| Queue insertion | All | 2ms | 10ms |
| Dequeue + dispatch (critical) | 900-1000 | 10ms | 200ms |
| Dequeue + dispatch (normal) | 500-899 | 50ms | 2s |
| Dequeue + dispatch (batch) | 0-499 | 500ms | 30s |
| Preemption (decision to SIGTERM) | N/A | 2s | 10s |
| Priority aging recalculation | All | 5ms | 50ms |

### Storage Estimates

| Data | Size per record | Count | Total |
|------|----------------|-------|-------|
| Job priority metadata | 200 B | 20M/day | 4 GB/day |
| Priority audit log | 100 B | 20M/day | 2 GB/day |
| Preemption records | 500 B | 5K/day | 2.5 MB/day |
| Aging state (Redis) | 50 B per active job | 500K concurrent | 25 MB |
| Priority queue (Redis) | 50 B per queued job | 100K peak queued | 5 MB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Queue operations | 50K/min x 200 B = 10 MB/min | 170 KB/s |
| Aging recalculations | 500K jobs x 50 B x 1/min = 25 MB/min | 420 KB/s |
| Priority audit events | 50K/min x 100 B = 5 MB/min | 83 KB/s |
| **Total** | | **~0.7 MB/s** |

---

## 3. High Level Architecture

```
    ┌──────────────────────────────────────────────────┐
    │                  API / CLI                        │
    │  (submit job with priority, override priority)    │
    └────────────────────┬─────────────────────────────┘
                         │
    ┌────────────────────▼─────────────────────────────┐
    │           Priority Evaluation Engine              │
    │                                                   │
    │  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │
    │  │ Base     │  │ Tenant   │  │  Deadline     │  │
    │  │ Priority │  │ Quota    │  │  Escalation   │  │
    │  │ Validator│  │ Enforcer │  │  Calculator   │  │
    │  └──────────┘  └──────────┘  └───────────────┘  │
    └────────────────────┬─────────────────────────────┘
                         │
    ┌────────────────────▼─────────────────────────────┐
    │          Multi-Level Priority Queue               │
    │                                                   │
    │  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
    │  │ CRITICAL │  │ NORMAL   │  │    BATCH     │   │
    │  │ 900-1000 │  │ 500-899  │  │   0-499      │   │
    │  │ (Redis   │  │ (Redis   │  │  (Redis      │   │
    │  │  SortSet)│  │  SortSet)│  │   SortSet)   │   │
    │  └────┬─────┘  └────┬─────┘  └──────┬───────┘   │
    │       │              │               │           │
    │  Weight: 70%    Weight: 25%     Weight: 5%       │
    └───────┼──────────────┼───────────────┼───────────┘
            │              │               │
    ┌───────▼──────────────▼───────────────▼───────────┐
    │              Dispatch Engine                      │
    │                                                   │
    │  ┌──────────────┐  ┌──────────────┐              │
    │  │ Resource     │  │ Preemption   │              │
    │  │ Matcher      │  │ Evaluator    │              │
    │  └──────────────┘  └──────────────┘              │
    └───────┬──────────────────────────────────────────┘
            │
    ┌───────▼──────────────────────────────────────────┐
    │              Worker Fleet                         │
    │                                                   │
    │  ┌────────┐  ┌────────┐  ┌────────┐ ┌────────┐  │
    │  │Worker 1│  │Worker 2│  │Worker 3│ │Worker N│  │
    │  │running:│  │running:│  │running:│ │running:│  │
    │  │P=950   │  │P=600   │  │P=100   │ │P=750   │  │
    │  └────────┘  └────────┘  └────────┘ └────────┘  │
    └──────────────────────────────────────────────────┘
            │
    ┌───────▼──────────────────────────────────────────┐
    │              Anti-Starvation Engine               │
    │                                                   │
    │  ┌──────────────┐  ┌──────────────────────────┐  │
    │  │ Aging Timer  │  │ Priority Inversion       │  │
    │  │ (every 60s)  │  │ Detector                 │  │
    │  └──────────────┘  └──────────────────────────┘  │
    └──────────────────────────────────────────────────┘
            │
    ┌───────▼──────┐  ┌────────┐  ┌──────────────┐
    │   MySQL      │  │ Redis  │  │ Elasticsearch│
    │  (jobs,      │  │(queues,│  │  (analytics, │
    │   preempt    │  │ aging, │  │   search)    │
    │   records)   │  │ locks) │  │              │
    └──────────────┘  └────────┘  └──────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Priority Evaluation Engine** | Determines effective priority: validates base priority, checks tenant quotas, applies deadline escalation |
| **Multi-Level Priority Queue** | Three Redis sorted sets (critical/normal/batch); weighted dequeue ensures minimum throughput per band |
| **Dispatch Engine** | Matches highest-priority feasible job to available worker; triggers preemption if needed |
| **Resource Matcher** | Checks worker resource availability against job requirements |
| **Preemption Evaluator** | Decides whether to preempt running low-priority jobs to make room for high-priority jobs |
| **Anti-Starvation Engine** | Periodically ages waiting jobs and detects priority inversion patterns |
| **Aging Timer** | Every 60s, increments effective priority of waiting jobs |
| **Priority Inversion Detector** | Detects when high-priority jobs are blocked by low-priority dependencies and boosts them |

### Data Flows

**Primary: Job Submission with Priority**
1. Job submitted with base priority (e.g., 700)
2. Priority Evaluation Engine: validates priority against tenant quota, applies escalation rules
3. Job inserted into appropriate priority queue (NORMAL band, sorted by effective priority)
4. Dispatch Engine dequeues highest-priority job, matches to available worker
5. Worker executes job

**Secondary: Preemption**
1. Critical job (priority 950) submitted but no workers available
2. Dispatch Engine queries running jobs: finds lowest-priority job (P=100) on a suitable worker
3. Preemption Evaluator checks: priority delta >= 200, preemption budget not exceeded, target job not near completion
4. Worker receives SIGTERM for low-priority job; job re-queued with original priority
5. Critical job dispatched to freed worker

**Tertiary: Aging**
1. Anti-Starvation Engine runs every 60 seconds
2. For each job in queue, increment effective priority by 1 (capped at original + 200)
3. If effective priority crosses a band boundary (e.g., 499 → 500), move to the higher band queue
4. After 3+ hours of waiting, low-priority jobs compete with normal-priority jobs

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Job priority configuration (extends job_definition)
CREATE TABLE job_priority_config (
    job_id              BIGINT UNSIGNED PRIMARY KEY,
    base_priority       SMALLINT        NOT NULL COMMENT '0-1000',
    effective_priority  SMALLINT        NOT NULL COMMENT 'After aging/escalation',
    priority_band       ENUM('CRITICAL','NORMAL','BATCH') NOT NULL,
    preemptible         BOOLEAN         DEFAULT TRUE COMMENT 'Can this job be preempted?',
    preempt_others      BOOLEAN         DEFAULT FALSE COMMENT 'Can this job preempt others?',
    max_aging_bonus     SMALLINT        DEFAULT 200 COMMENT 'Max priority boost from aging',
    deadline            TIMESTAMP(6)    NULL COMMENT 'Soft deadline for escalation',
    escalation_rate     SMALLINT        DEFAULT 50 COMMENT 'Priority boost per hour approaching deadline',
    submitted_at        TIMESTAMP(6)    NOT NULL,
    queued_since        TIMESTAMP(6)    NULL COMMENT 'When job entered queue (for aging)',
    aging_applied       SMALLINT        DEFAULT 0 COMMENT 'Current aging bonus',
    FOREIGN KEY (job_id) REFERENCES job_definition(job_id),
    INDEX idx_band_effective (priority_band, effective_priority DESC),
    INDEX idx_queued_since (queued_since)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Tenant priority quotas
CREATE TABLE tenant_priority_quota (
    tenant_id           VARCHAR(64)     NOT NULL,
    priority_band       ENUM('CRITICAL','NORMAL','BATCH') NOT NULL,
    max_concurrent      INT UNSIGNED    NOT NULL COMMENT 'Max running jobs in this band',
    max_submissions_hour INT UNSIGNED   NOT NULL COMMENT 'Max submissions per hour',
    current_running     INT UNSIGNED    DEFAULT 0,
    current_hour_count  INT UNSIGNED    DEFAULT 0,
    hour_window_start   TIMESTAMP       NOT NULL,
    PRIMARY KEY (tenant_id, priority_band),
    INDEX idx_tenant (tenant_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Preemption log
CREATE TABLE preemption_record (
    preemption_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    preemptor_job_id    BIGINT UNSIGNED NOT NULL COMMENT 'The high-priority job',
    preemptor_priority  SMALLINT        NOT NULL,
    preempted_job_id    BIGINT UNSIGNED NOT NULL COMMENT 'The evicted low-priority job',
    preempted_priority  SMALLINT        NOT NULL,
    worker_id           VARCHAR(128)    NOT NULL,
    resources_freed     JSON            NOT NULL COMMENT '{"cpu_millis":2000,"memory_mb":4096}',
    preempted_at        TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    preempted_job_requeued BOOLEAN      DEFAULT TRUE,
    grace_period_ms     INT UNSIGNED    NOT NULL DEFAULT 30000,
    INDEX idx_preemptor (preemptor_job_id),
    INDEX idx_preempted (preempted_job_id),
    INDEX idx_time (preempted_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Priority change audit log
CREATE TABLE priority_audit (
    audit_id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    job_id              BIGINT UNSIGNED NOT NULL,
    old_priority        SMALLINT        NOT NULL,
    new_priority        SMALLINT        NOT NULL,
    change_reason       ENUM('AGING','ESCALATION','ADMIN_OVERRIDE',
                             'INVERSION_BOOST','QUOTA_DOWNGRADE','SUBMISSION') NOT NULL,
    changed_by          VARCHAR(128)    NOT NULL COMMENT 'system/user/agent',
    changed_at          TIMESTAMP(6)    DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_job (job_id),
    INDEX idx_time (changed_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

Same as distributed_job_scheduler.md: **MySQL 8.0** for persistent state. Priority queues and aging state stored in **Redis** for real-time performance.

| Component | Store | Rationale |
|-----------|-------|-----------|
| Priority queues | Redis Sorted Sets | O(log N) insert/remove; atomic ZPOPMIN for dequeue |
| Aging state | Redis Hash | O(1) per-job aging increment; bulk update via pipeline |
| Job priority config | MySQL | Durable; joins with job_definition |
| Preemption records | MySQL | Audit trail; historical analysis |
| Priority audit | MySQL + ES | Compliance; searchable history |

### Indexing Strategy

| Index | Table | Purpose |
|-------|-------|---------|
| `idx_band_effective` | job_priority_config | Dispatch: find highest-priority jobs per band |
| `idx_queued_since` | job_priority_config | Aging: find jobs waiting longest |
| `idx_tenant` | tenant_priority_quota | Quota enforcement |
| `idx_preempted_at` | preemption_record | Preemption analytics |

---

## 5. API Design

### REST API Endpoints

```
POST /api/v1/jobs
Authorization: Bearer <JWT>
Content-Type: application/json

Request (priority-specific fields):
{
  "job_name": "emergency-db-failover",
  "priority": 950,
  "preempt_others": true,
  "deadline": "2026-04-09T15:00:00Z",
  "resource_request": {"cpu_millis": 4000, "memory_mb": 8192},
  ...
}

Response (201):
{
  "job_id": 12345,
  "effective_priority": 950,
  "priority_band": "CRITICAL",
  "estimated_start_time": "2026-04-09T14:30:05Z",
  "queue_position": 3,
  "preemption_possible": true
}
```

```
PATCH /api/v1/jobs/{job_id}/priority
Authorization: Bearer <JWT> (requires job-admin or platform-admin role)

Request:
{
  "new_priority": 800,
  "reason": "Escalating due to customer SLA breach"
}

Response (200):
{
  "job_id": 12345,
  "old_priority": 500,
  "new_priority": 800,
  "new_band": "NORMAL",
  "new_queue_position": 7
}
```

```
GET /api/v1/scheduler/priority-stats

Response:
{
  "queues": {
    "CRITICAL": {"depth": 12, "oldest_wait_seconds": 5, "dispatch_rate_per_min": 200},
    "NORMAL": {"depth": 1523, "oldest_wait_seconds": 45, "dispatch_rate_per_min": 3000},
    "BATCH": {"depth": 45231, "oldest_wait_seconds": 1800, "dispatch_rate_per_min": 500}
  },
  "preemptions_last_hour": 42,
  "aging_active_jobs": 15000,
  "starvation_risk_jobs": 3
}
```

```
GET /api/v1/scheduler/preemptions?last=1h
POST /api/v1/admin/priority-override
    {"job_id": 12345, "new_priority": 999, "reason": "P0 incident response"}
GET /api/v1/tenants/{tenant_id}/priority-quotas
PUT /api/v1/tenants/{tenant_id}/priority-quotas
```

### CLI Design

```bash
# Submit with priority
jobctl submit --name "db-failover" --priority 950 --preempt --deadline "2026-04-09T15:00:00Z" ...

# Check queue position
jobctl queue-position <job_id>
# Output: Job 12345 is at position 3 in CRITICAL queue (effective priority: 950)

# Override priority (admin)
jobctl priority set <job_id> 800 --reason "SLA escalation"
jobctl priority get <job_id>
# Output:
# Job 12345:
#   Base priority:      500
#   Effective priority: 650 (aging: +100, escalation: +50)
#   Band:              NORMAL
#   Queue position:    142
#   Waiting since:     1h 45m
#   Aging rate:        +1/min (max +200)
#   Deadline:          2026-04-09T18:00:00Z (3h 15m remaining)

# View priority statistics
jobctl priority stats
# Output:
# BAND       QUEUED  RUNNING  DISPATCH/min  AVG_WAIT  MAX_WAIT  PREEMPTIONS/hr
# CRITICAL   12      45       200           5s        30s       8
# NORMAL     1,523   3,200    3,000         45s       5m        0
# BATCH      45,231  1,500    500           30m       3h        0 (34 preempted)

# View preemption history
jobctl preemptions list --last 1h
# Output:
# TIME                 PREEMPTOR(P)   PREEMPTED(P)   WORKER           RESOURCES_FREED
# 14:25:03  job-999(950)  job-4521(100)  worker-a-17  4 CPU, 8GB RAM
# 14:18:45  job-997(980)  job-4489(200)  worker-b-03  8 CPU, 16GB RAM

# Tenant quota management
jobctl quota show team-platform
jobctl quota set team-platform --band CRITICAL --max-concurrent 50 --max-per-hour 200
```

---

## 6. Core Component Deep Dives

### 6.1 Multi-Level Priority Queue with Weighted Fair Dequeue

**Why it's hard:** A naive "always dequeue highest priority" approach leads to starvation of lower priority bands. We need weighted dequeue that guarantees minimum throughput per band while still strongly favoring critical jobs. The queue must support O(log N) operations and handle 50K operations/min.

**Approaches Compared:**

| Approach | Starvation Prevention | Strict Priority | Complexity | Performance |
|----------|----------------------|----------------|------------|-------------|
| Single sorted set (all priorities) | None (strict priority) | Perfect | Low | O(log N) |
| Separate queues + strict priority | None | Perfect | Low | O(1) per dequeue |
| Weighted Fair Queuing (WFQ) | Built-in (by weight) | Approximation | Medium | O(log N) per queue |
| Deficit Round Robin (DRR) | Built-in | Approximation | Medium | O(1) amortized |
| Multi-level feedback queue (MLFQ) | Aging-based | Dynamic | High | Varies |

**Selected: Weighted Fair Queuing across 3 priority bands** -- Each band gets a weight (CRITICAL=70%, NORMAL=25%, BATCH=5%). Each dispatch cycle, we dequeue from each band proportional to its weight. Within a band, strict priority ordering via Redis sorted set.

**Implementation:**

```python
import redis
import time
from typing import Optional, Tuple

class MultiLevelPriorityQueue:
    """
    Three-band priority queue with weighted fair dequeue.
    
    Bands:
      CRITICAL (900-1000): 70% of dispatch capacity
      NORMAL   (500-899):  25% of dispatch capacity
      BATCH    (0-499):    5%  of dispatch capacity
    
    Within each band: sorted by effective_priority DESC, then queued_at ASC.
    Score = (1000 - effective_priority) * 1e15 + queued_at_epoch_ms
    """
    
    BANDS = {
        'CRITICAL': {'key': 'pq:critical', 'weight': 70, 'min_priority': 900},
        'NORMAL':   {'key': 'pq:normal',   'weight': 25, 'min_priority': 500},
        'BATCH':    {'key': 'pq:batch',     'weight': 5,  'min_priority': 0},
    }
    
    def __init__(self, redis_client: redis.Redis):
        self.redis = redis_client
        # Deficit counters for DRR within weighted scheme
        self._deficits = {band: 0 for band in self.BANDS}
    
    def enqueue(self, job_id: int, effective_priority: int, queued_at_ms: int):
        """Insert job into the appropriate priority band."""
        band = self._get_band(effective_priority)
        key = self.BANDS[band]['key']
        
        # Score: lower = higher priority = dequeued first
        # First sort by priority (descending), then by queue time (ascending/FIFO)
        score = (1000 - effective_priority) * 10**15 + queued_at_ms
        
        self.redis.zadd(key, {str(job_id): score})
    
    def dequeue_batch(self, batch_size: int = 100) -> list:
        """
        Dequeue up to batch_size jobs, respecting band weights.
        
        Uses Weighted Fair Queuing:
        - CRITICAL gets 70% of slots
        - NORMAL gets 25%
        - BATCH gets 5%
        
        If a band is empty, its allocation spills to the next band.
        """
        allocations = self._calculate_allocations(batch_size)
        
        results = []
        spillover = 0
        
        # Process bands in priority order (CRITICAL first)
        for band in ['CRITICAL', 'NORMAL', 'BATCH']:
            key = self.BANDS[band]['key']
            count = allocations[band] + spillover
            spillover = 0
            
            if count == 0:
                continue
            
            # Atomic pop of lowest-scored (highest priority) members
            popped = self.redis.zpopmin(key, count)
            
            results.extend([
                {'job_id': int(member), 'band': band, 'score': score}
                for member, score in popped
            ])
            
            # If we got fewer than allocated, spill to next band
            if len(popped) < count:
                spillover = count - len(popped)
        
        return results
    
    def _calculate_allocations(self, batch_size: int) -> dict:
        """Calculate how many jobs each band gets in this batch."""
        allocations = {}
        remaining = batch_size
        
        for band in ['CRITICAL', 'NORMAL', 'BATCH']:
            weight = self.BANDS[band]['weight']
            alloc = max(1, int(batch_size * weight / 100))  # At least 1 per band
            alloc = min(alloc, remaining)
            allocations[band] = alloc
            remaining -= alloc
        
        # Give any remainder to CRITICAL
        if remaining > 0:
            allocations['CRITICAL'] += remaining
        
        return allocations
    
    def _get_band(self, priority: int) -> str:
        if priority >= 900:
            return 'CRITICAL'
        elif priority >= 500:
            return 'NORMAL'
        else:
            return 'BATCH'
    
    def move_band(self, job_id: int, old_band: str, new_band: str, 
                   new_priority: int, queued_at_ms: int):
        """Move a job between bands (e.g., when aging crosses a threshold)."""
        pipe = self.redis.pipeline()
        pipe.zrem(self.BANDS[old_band]['key'], str(job_id))
        new_score = (1000 - new_priority) * 10**15 + queued_at_ms
        pipe.zadd(self.BANDS[new_band]['key'], {str(job_id): new_score})
        pipe.execute()
    
    def get_stats(self) -> dict:
        pipe = self.redis.pipeline()
        for band in self.BANDS.values():
            pipe.zcard(band['key'])
        counts = pipe.execute()
        
        return {
            band_name: {
                'depth': count,
                'weight': self.BANDS[band_name]['weight']
            }
            for band_name, count in zip(self.BANDS.keys(), counts)
        }
    
    def peek(self, band: str, count: int = 10) -> list:
        """View top N jobs in a band without removing them."""
        key = self.BANDS[band]['key']
        return self.redis.zrange(key, 0, count - 1, withscores=True)
```

**Failure Modes:**
- **Redis failure:** Priority queues are backed by MySQL (`job_execution` table with status=QUEUED). On Redis recovery, rebuild sorted sets from MySQL: `SELECT job_id, effective_priority, queued_at FROM job_priority_config WHERE status = 'QUEUED'`.
- **Incorrect band due to race condition (aging crosses threshold during dequeue):** Band transitions are atomic (Redis pipeline). Stale band membership means a job might be dequeued from the wrong band -- the dispatch engine re-checks effective priority before dispatching.
- **Weight overflow (CRITICAL always has jobs, BATCH never dequeued):** The minimum 5% allocation for BATCH ensures at least 5 jobs per 100-job batch. If CRITICAL is empty, its 70% spills to NORMAL and then BATCH.

**Interviewer Q&A:**

**Q1: Why three bands instead of a single sorted set with all 1001 priority levels?**
A: A single sorted set would strictly order by priority, meaning a priority-500 job is always dequeued before a priority-499 job, regardless of wait time. This leads to starvation: if there's always a priority-500+ job in the queue, priority-0-499 jobs never run. Bands with weighted dequeue provide a principled mechanism to guarantee minimum throughput for each tier while still strongly favoring critical work.

**Q2: How do you handle the case where the CRITICAL band is empty most of the time?**
A: Spillover. If CRITICAL has 0 jobs but was allocated 70 slots in a 100-job batch, those 70 slots spill to NORMAL (gets 95 total) and then BATCH (gets 5 total). The weights only matter during contention. When there's no contention, all bands are served immediately.

**Q3: Wouldn't Kafka with separate topics per priority be simpler?**
A: Kafka doesn't support weighted consumption across topics natively. You'd need a custom consumer that polls each topic with weighted frequency. Kafka also doesn't support sorted ordering within a partition (messages are FIFO). Redis sorted sets give us O(log N) priority-ordered dequeue with atomic pops. The trade-off: Redis is less durable than Kafka, which is why we back it with MySQL.

**Q4: How do you prevent gaming (tenants always submitting as CRITICAL)?**
A: Per-tenant quotas on CRITICAL band: `max_concurrent` (e.g., 50 concurrent critical jobs) and `max_submissions_hour` (e.g., 200/hour). Exceeding quota either downgrades the job to NORMAL or rejects the submission (configurable). Platform admins can override quotas for genuine emergencies. Quota enforcement is at the API layer, before the job enters the priority queue.

**Q5: What's the O() complexity of your dequeue_batch?**
A: `zpopmin` is O(log N * K) where K is the number of elements popped and N is the set size. For a 100-job batch across 3 bands: O(log N_critical * 70 + log N_normal * 25 + log N_batch * 5). With N = 100K total, this is ~17 * 100 = 1700 operations, completing in < 1ms on Redis.

**Q6: How do you maintain fairness within a band (across tenants)?**
A: Within a band, the sorted set is ordered by effective_priority then queued_at. All jobs at the same effective priority are FIFO (earliest queued first). This is inherently fair for same-priority jobs. For cross-tenant fairness, we add a small jitter based on tenant_id hash: `score += hash(tenant_id) % 1000`. This interleaves tenants at the same priority level without breaking priority ordering.

---

### 6.2 Preemption Engine

**Why it's hard:** Preemption is destructive -- the preempted job's work is lost and must be restarted. Deciding when preemption is worthwhile requires balancing: the urgency of the incoming job, the cost of wasted work, resource efficiency, and fairness. Incorrect preemption decisions cause thrashing (constant eviction/restart cycles).

**Approaches Compared:**

| Approach | Efficiency | Simplicity | Thrashing Risk | Implementation |
|----------|-----------|------------|----------------|----------------|
| No preemption (wait only) | Poor (critical jobs wait) | Simple | None | N/A |
| Strict priority preemption | Best for critical jobs | Simple | High | Always preempt lowest priority |
| Threshold-based preemption | Good | Medium | Low | Only preempt if Δpriority >= threshold |
| Cost-aware preemption | Optimal | Complex | Very Low | Consider wasted work cost |
| Reservation-based (no preemption) | Good | Medium | None | Reserve capacity for critical |

**Selected: Threshold-based preemption with cost-awareness** -- Preempt only when: (1) priority delta >= 200, (2) preempted job has completed < 80% of its estimated runtime, (3) preemption budget not exceeded. This balances urgency with efficiency.

**Implementation:**

```java
@Service
public class PreemptionEngine {
    
    private static final int MIN_PRIORITY_DELTA = 200;
    private static final double MAX_COMPLETION_RATIO = 0.80;
    private static final Duration GRACE_PERIOD = Duration.ofSeconds(30);
    private static final int MAX_PREEMPTIONS_PER_JOB = 3;
    private static final int MAX_PREEMPTIONS_PER_HOUR = 100;
    
    @Autowired
    private JobExecutionRepository executionRepo;
    
    @Autowired
    private PreemptionRecordRepository preemptionRepo;
    
    @Autowired
    private WorkerNodeRepository workerRepo;
    
    @Autowired
    private WorkerGrpcClient workerClient;
    
    @Autowired
    private MeterRegistry metrics;
    
    /**
     * Attempt to find a preemption victim to make room for a high-priority job.
     * Returns the freed worker if successful, null if no suitable victim found.
     */
    public Optional<WorkerNode> preemptForJob(JobDefinition highPriorityJob,
                                               JobPriorityConfig jobPriority) {
        
        if (!jobPriority.isPreemptOthers()) {
            return Optional.empty();
        }
        
        // Check global preemption budget
        long recentPreemptions = preemptionRepo.countSince(
            Instant.now().minus(1, ChronoUnit.HOURS));
        if (recentPreemptions >= MAX_PREEMPTIONS_PER_HOUR) {
            log.warn("Preemption budget exhausted ({}/hr). Job {} must wait.",
                recentPreemptions, highPriorityJob.getJobId());
            metrics.counter("preemption.budget_exhausted").increment();
            return Optional.empty();
        }
        
        // Find candidate victims: running jobs on workers that match resource requirements
        List<PreemptionCandidate> candidates = findCandidates(highPriorityJob, jobPriority);
        
        if (candidates.isEmpty()) {
            return Optional.empty();
        }
        
        // Sort by preemption cost (ascending): prefer to preempt cheapest victim
        candidates.sort(Comparator.comparingDouble(PreemptionCandidate::getCost));
        
        PreemptionCandidate best = candidates.get(0);
        
        // Execute preemption
        return executePreemption(highPriorityJob, jobPriority, best);
    }
    
    private List<PreemptionCandidate> findCandidates(JobDefinition job,
                                                       JobPriorityConfig priority) {
        List<PreemptionCandidate> candidates = new ArrayList<>();
        
        // Find all running jobs with lower priority on suitable workers
        List<JobExecution> runningJobs = executionRepo.findRunningWithPriorityBelow(
            priority.getEffectivePriority() - MIN_PRIORITY_DELTA);
        
        for (JobExecution running : runningJobs) {
            WorkerNode worker = workerRepo.findById(running.getWorkerId()).orElse(null);
            if (worker == null) continue;
            
            // Check resource match: would freeing this job provide enough resources?
            JobDefinition runningJobDef = jobDefRepo.findById(running.getJobId()).orElse(null);
            if (runningJobDef == null) continue;
            
            if (!resourcesSufficient(worker, runningJobDef, job)) continue;
            
            // Check: is the running job preemptible?
            JobPriorityConfig runningPriority = priorityRepo.findById(running.getJobId()).orElse(null);
            if (runningPriority == null || !runningPriority.isPreemptible()) continue;
            
            // Check: has this job been preempted too many times?
            long timesPreempted = preemptionRepo.countByPreemptedJobId(running.getJobId());
            if (timesPreempted >= MAX_PREEMPTIONS_PER_JOB) continue;
            
            // Check: is the running job near completion?
            double completionRatio = estimateCompletionRatio(running);
            if (completionRatio >= MAX_COMPLETION_RATIO) continue;
            
            // Calculate preemption cost: wasted work + restart overhead
            double cost = calculatePreemptionCost(running, runningJobDef, completionRatio);
            
            candidates.add(new PreemptionCandidate(
                running, runningJobDef, worker, cost, completionRatio));
        }
        
        return candidates;
    }
    
    private double calculatePreemptionCost(JobExecution running, 
                                             JobDefinition jobDef,
                                             double completionRatio) {
        // Cost = (time_already_spent * cpu_millis) + estimated_restart_overhead
        long elapsedMs = Duration.between(running.getStartedAt(), Instant.now()).toMillis();
        double wastedWork = elapsedMs * jobDef.getResourceRequest().getCpuMillis() / 1000.0;
        double restartOverhead = 30_000; // 30s estimated restart cost
        
        // Higher completion ratio = higher cost (more wasted work)
        return wastedWork * completionRatio + restartOverhead;
    }
    
    private double estimateCompletionRatio(JobExecution running) {
        long elapsedMs = Duration.between(running.getStartedAt(), Instant.now()).toMillis();
        JobDefinition jobDef = jobDefRepo.findById(running.getJobId()).orElseThrow();
        
        // Use historical average runtime for this job type
        Double avgRuntimeMs = executionRepo.getAverageRuntime(running.getJobId());
        if (avgRuntimeMs == null) {
            // No history; use timeout as upper bound
            avgRuntimeMs = (double) jobDef.getTimeoutSeconds() * 1000;
        }
        
        return Math.min(1.0, elapsedMs / avgRuntimeMs);
    }
    
    private Optional<WorkerNode> executePreemption(JobDefinition highPriorityJob,
                                                     JobPriorityConfig priority,
                                                     PreemptionCandidate victim) {
        log.info("Preempting job {} (P={}) on worker {} for job {} (P={})",
            victim.getExecution().getJobId(), victim.getExecution().getJobId(),
            victim.getWorker().getWorkerId(),
            highPriorityJob.getJobId(), priority.getEffectivePriority());
        
        // 1. Record preemption
        PreemptionRecord record = new PreemptionRecord();
        record.setPreemptorJobId(highPriorityJob.getJobId());
        record.setPreemptorPriority(priority.getEffectivePriority());
        record.setPreemptedJobId(victim.getExecution().getJobId());
        record.setPreemptedPriority(victim.getExecution().getPriority());
        record.setWorkerId(victim.getWorker().getWorkerId());
        record.setResourcesFreed(victim.getJobDef().getResourceRequest());
        record.setGracePeriodMs((int) GRACE_PERIOD.toMillis());
        preemptionRepo.save(record);
        
        // 2. Send SIGTERM to victim via worker gRPC
        try {
            workerClient.terminateJob(
                victim.getWorker().getWorkerId(),
                victim.getExecution().getExecutionId(),
                GRACE_PERIOD);
        } catch (Exception e) {
            log.error("Failed to send termination to worker {}", 
                victim.getWorker().getWorkerId(), e);
            return Optional.empty();
        }
        
        // 3. Wait for grace period (async -- don't block the scheduler)
        // The worker will report job completion/failure after SIGTERM
        // The dispatch engine will re-check resource availability after grace period
        
        // 4. Re-queue the preempted job
        stateMachine.transition(victim.getExecution().getExecutionId(), 
            JobStatus.QUEUED, null, "Preempted by job " + highPriorityJob.getJobId());
        
        metrics.counter("preemption.executed",
            "preemptor_band", priority.getPriorityBand().name(),
            "victim_band", victim.getBand().name()).increment();
        
        return Optional.of(victim.getWorker());
    }
}
```

**Failure Modes:**
- **Preemption signal lost (worker doesn't receive SIGTERM):** Worker heartbeat will eventually detect the job as "should be terminated." Alternatively, the scheduler resends after grace period.
- **Preempted job can't be re-queued (MySQL down):** Preemption is a two-phase operation: record preemption in MySQL first, then send SIGTERM. If MySQL is down, we don't send SIGTERM (preemption aborted).
- **Thrashing (same job preempted repeatedly):** `MAX_PREEMPTIONS_PER_JOB = 3` prevents infinite preemption cycles. After 3 preemptions, the job becomes non-preemptible for its remaining lifetime.
- **Resource leak (SIGTERM sent but resources not freed):** After grace period + 10s buffer, if the worker still reports the job as running, send SIGKILL. Update worker available resources after confirmation of termination.

**Interviewer Q&A:**

**Q1: Why require a priority delta of 200 for preemption?**
A: A small delta (e.g., 10) means jobs are almost the same importance -- preemption wastes work for minimal gain. A delta of 200 means the incoming job is objectively more important (e.g., CRITICAL 900 preempting BATCH 700 -- but actually that's only 200 apart; in practice CRITICAL 950 preempting BATCH 100 with delta 850 is the common case). The 200 threshold prevents "priority creep" where jobs escalate slightly above each other to gain preemption rights.

**Q2: How do you handle the 30-second grace period without blocking the scheduler?**
A: Preemption is asynchronous. The scheduler sends SIGTERM and returns immediately. The preempted job's status transitions to QUEUED. The high-priority job remains in the dispatch queue. On the next dispatch cycle (every 1s), the dispatch engine checks if the worker has freed resources (via heartbeat update). If yes, it dispatches the high-priority job. If not yet, it waits another cycle. The grace period is enforced by the worker, not the scheduler.

**Q3: What if the high-priority job needs more resources than the preempted job provides?**
A: The preemption engine checks `resourcesSufficient()` which compares the preempted job's resource allocation with the high-priority job's requirements. If preempting one job isn't enough, we can preempt multiple jobs on the same worker (gang preemption). The cost function sums up all preemptions. If no single worker can satisfy the requirements even after preempting all its low-priority jobs, we don't preempt (the job waits for a worker to become fully available).

**Q4: How do you prevent preemption from always penalizing the same tenant?**
A: The victim selection algorithm considers fairness: `cost += preemptions_against_tenant_last_hour * 1000`. Tenants that have already been preempted recently have a higher cost, making them less likely to be selected. We also publish per-tenant preemption metrics and alert if any tenant is preempted > 10 times/hour.

**Q5: Can a preempted job preempt another job when it's re-queued?**
A: No. A re-queued preempted job retains its original priority and goes back to its original position in the queue. It does not get a priority boost for being preempted. The rationale: the preempted job was legitimately lower priority. Boosting it would undermine the priority system. However, the aging mechanism still applies: if it waits long enough in the queue, it naturally increases in effective priority.

**Q6: How do you measure the effectiveness of your preemption policy?**
A: Key metrics: (1) Preemption rate (preemptions/hour) -- should be < 1% of total dispatches. (2) Preemption waste ratio: total wasted CPU-seconds from preemption / total CPU-seconds delivered. (3) Critical job wait time: P99 should be < 200ms (preemption enables this). (4) Re-preemption rate: what % of preempted jobs get preempted again after requeue -- should be < 5%. We tune the MIN_PRIORITY_DELTA and MAX_COMPLETION_RATIO based on these metrics.

---

### 6.3 Anti-Starvation: Priority Aging

**Why it's hard:** Without aging, batch-priority jobs (0-499) could wait forever if there's a constant stream of normal and critical jobs. Aging must be fast (applied to potentially 100K+ queued jobs), consistent (all scheduler instances agree on effective priorities), and bounded (aging shouldn't make all jobs the same priority).

**Approaches Compared:**

| Approach | Fairness | Performance | Complexity | Predictability |
|----------|---------|-------------|------------|---------------|
| Linear aging (+1/min) | Good | O(N) per cycle | Low | High (max boost = wait_min) |
| Exponential aging (faster as wait grows) | Aggressive | O(N) | Low | Medium |
| Step function (boost after thresholds) | Moderate | O(N but sparse) | Low | Very High |
| Virtual time (fair-share scheduling) | Excellent | O(1) per job | High | Low |

**Selected: Linear aging with cap** -- Simple, predictable, and bounded. Every minute, each queued job's effective priority increases by 1, capped at `base_priority + max_aging_bonus` (default 200). A priority-0 job reaches effective priority 200 after 200 minutes (~3.3 hours), at which point it competes with normal-priority jobs. Maximum starvation time is bounded.

**Implementation:**

```python
class PriorityAgingEngine:
    """
    Periodically increases effective priority of queued jobs.
    
    Runs every 60 seconds on the scheduler leader.
    Updates Redis sorted set scores to reflect new effective priorities.
    """
    
    AGING_INTERVAL_SECONDS = 60
    AGING_INCREMENT = 1          # Priority boost per interval
    BAND_THRESHOLDS = {'BATCH': 500, 'NORMAL': 900}  # Band boundaries
    
    def __init__(self, redis_client, priority_repo, audit_repo, metrics):
        self.redis = redis_client
        self.priority_repo = priority_repo
        self.audit_repo = audit_repo
        self.metrics = metrics
    
    def run_aging_cycle(self):
        """
        Single aging cycle. Called every AGING_INTERVAL_SECONDS.
        
        Approach: Scan all queued jobs, increment effective_priority by 1,
        check for band transitions, update Redis sorted sets.
        
        For 100K queued jobs at 50 bytes each, this is ~5MB of Redis operations.
        Use pipeline for batching.
        """
        start = time.time()
        
        # Get all jobs with aging information from MySQL
        queued_jobs = self.priority_repo.find_queued_with_aging()
        
        band_transitions = 0
        aged_count = 0
        
        pipe = self.redis.pipeline()
        
        for job in queued_jobs:
            # Calculate new effective priority
            new_aging = min(
                job.aging_applied + self.AGING_INCREMENT,
                job.max_aging_bonus
            )
            
            if new_aging == job.aging_applied:
                continue  # Already at max aging bonus
            
            new_effective = job.base_priority + new_aging
            old_effective = job.effective_priority
            old_band = self._get_band(old_effective)
            new_band = self._get_band(new_effective)
            
            # Update MySQL (batched)
            self.priority_repo.update_aging(
                job.job_id, new_aging, new_effective)
            
            # Update Redis sorted set score
            old_key = f"pq:{old_band.lower()}"
            new_score = (1000 - new_effective) * 10**15 + job.queued_at_ms
            
            if old_band != new_band:
                # Band transition: move between sorted sets
                new_key = f"pq:{new_band.lower()}"
                pipe.zrem(old_key, str(job.job_id))
                pipe.zadd(new_key, {str(job.job_id): new_score})
                band_transitions += 1
                
                # Audit the band transition
                self.audit_repo.log_change(
                    job_id=job.job_id,
                    old_priority=old_effective,
                    new_priority=new_effective,
                    reason='AGING',
                    changed_by='system/aging-engine'
                )
            else:
                # Same band: update score in place
                pipe.zadd(old_key, {str(job.job_id): new_score}, xx=True)
            
            aged_count += 1
        
        pipe.execute()
        
        elapsed = time.time() - start
        self.metrics.timer('aging.cycle.duration').record(elapsed)
        self.metrics.gauge('aging.jobs_processed', aged_count)
        self.metrics.counter('aging.band_transitions').increment(band_transitions)
        
        log.info(f"Aging cycle: {aged_count} jobs aged, "
                 f"{band_transitions} band transitions, {elapsed:.3f}s")
    
    def _get_band(self, priority: int) -> str:
        if priority >= 900:
            return 'CRITICAL'
        elif priority >= 500:
            return 'NORMAL'
        else:
            return 'BATCH'
    
    def get_starvation_risk_jobs(self, max_wait_minutes: int = 180) -> list:
        """Find jobs that have been waiting dangerously long."""
        threshold = datetime.utcnow() - timedelta(minutes=max_wait_minutes)
        return self.priority_repo.find_queued_since_before(threshold)
```

**Failure Modes:**
- **Aging engine crashes mid-cycle:** Partially aged jobs have inconsistent MySQL and Redis state. On restart, the engine re-reads from MySQL (source of truth) and re-applies aging. Idempotent: re-applying the same aging increment to an already-aged job produces the same result.
- **Clock skew between aging cycles:** We use monotonic clock for interval timing. If the leader changes and the new leader's clock is different, the aging rate might briefly fluctuate. Bounded impact: at most 1 extra or 1 missed increment.
- **Aging causes thundering herd at band boundary:** If 1000 jobs cross the BATCH→NORMAL boundary simultaneously, the NORMAL queue spikes. The weighted dequeue handles this gracefully: NORMAL's 25% allocation absorbs the spike over multiple dispatch cycles.

**Interviewer Q&A:**

**Q1: Why cap aging at +200 instead of letting it grow indefinitely?**
A: Unbounded aging would eventually make all jobs CRITICAL priority, defeating the purpose of the priority system. A priority-0 job with +200 aging reaches effective priority 200, which puts it mid-BATCH band. Even with aging, it never outcompetes a genuinely high-priority job (priority 500+). The 200 cap ensures that base priority still matters: a job submitted at priority 800 will always outrank an aged priority-0 job (effective 200).

**Q2: How do you handle aging for tens of millions of queued jobs?**
A: At 10M queued jobs, a per-job scan takes too long (even at 0.01ms/job = 100 seconds). Optimization: (1) Only age jobs that haven't reached their cap. (2) Use bulk SQL update: `UPDATE job_priority_config SET aging_applied = aging_applied + 1, effective_priority = base_priority + aging_applied + 1 WHERE aging_applied < max_aging_bonus AND status = 'QUEUED'`. (3) Redis updates via pipeline (10K operations/pipeline batch). (4) At extreme scale, shard aging across multiple threads by job_id range.

**Q3: What if a job is dequeued during an aging cycle?**
A: The aging engine reads from MySQL, but the dispatch engine dequeues from Redis. If a job is dequeued between the MySQL read and the Redis update, the `ZADD ... XX` (update only if exists) command is a no-op. If the `ZREM` (for band transition) targets a job already dequeued, it's also a no-op (ZREM on non-existent member returns 0). No corruption occurs.

**Q4: How does aging interact with deadline escalation?**
A: They're additive but capped independently. `effective_priority = base_priority + min(aging_bonus, 200) + min(deadline_escalation, 300)`. A priority-300 batch job with max aging (+200) and deadline escalation (+300) reaches effective 800, competing with normal-priority jobs. The total bonus is capped at 500 to prevent batch jobs from reaching CRITICAL band through escalation alone.

**Q5: How would you implement virtual time fair-share scheduling instead?**
A: In virtual time scheduling, each tenant has a "virtual clock" that advances proportionally to their resource consumption. Jobs are sorted by their tenant's virtual time (lowest first = least recently served). This provides perfect long-term fairness but makes priority less meaningful (high-priority jobs from a "rich" tenant rank lower than low-priority jobs from a "poor" tenant). We'd implement it as an alternative to weighted fair queuing, selectable per cluster policy.

**Q6: What's the maximum starvation time guarantee?**
A: A priority-0 job gains +1 effective priority per minute. At the 200-minute mark (3h 20min), it reaches effective priority 200 (still BATCH). To cross into NORMAL (500), it would need base priority 300+ or deadline escalation. Absolute worst case: a priority-0 job with no deadline stays at effective 200 and competes within BATCH band. The 5% minimum throughput for BATCH guarantees it eventually runs. Empirical worst case with 5% allocation: ~4 hours.

---

### 6.4 Priority Inversion Detection and Resolution

**Why it's hard:** Priority inversion occurs when a high-priority job H depends on a low-priority job L, but L is blocked behind medium-priority jobs M. H is effectively starved despite being highest priority. Classic example from systems programming (Mars Pathfinder priority inversion bug).

**Approaches:**

| Approach | Mechanism | Applicability |
|----------|-----------|---------------|
| Priority inheritance | Boost L to H's priority while H depends on L | DAG dependencies |
| Priority ceiling | Pre-assign max priority to all shared resources | Mutex/lock scenarios |
| Random boosting | Periodically boost random low-priority jobs | Probabilistic, not guaranteed |
| Dependency-aware scheduling | Schedule all transitive dependencies at the highest dependent's priority | DAG scheduling |

**Selected: Dependency-aware scheduling with priority inheritance** -- When a DAG is submitted, all tasks inherit the priority of the highest-priority downstream dependent. This prevents inversion at the source.

**Implementation:**

```python
class PriorityInversionDetector:
    """
    Detects and resolves priority inversion in DAG dependencies.
    
    Priority Inheritance Protocol:
    If task A (P=100) is an upstream dependency of task B (P=900),
    then A's effective priority is boosted to max(A.priority, B.priority) = 900
    while B is waiting.
    """
    
    def apply_priority_inheritance(self, dag_id: int) -> dict:
        """
        Propagate priority inheritance through a DAG.
        Each task's effective priority = max(own priority, max(downstream priorities)).
        
        Uses reverse topological order: process from sinks to sources.
        """
        edges = self.dag_repo.get_edges(dag_id)
        tasks = self.dag_repo.get_tasks(dag_id)
        
        # Build adjacency and reverse adjacency
        adj = defaultdict(set)      # task -> downstream tasks
        rev_adj = defaultdict(set)  # task -> upstream tasks
        all_task_ids = set()
        
        for upstream_id, downstream_id in edges:
            adj[upstream_id].add(downstream_id)
            rev_adj[downstream_id].add(upstream_id)
            all_task_ids.add(upstream_id)
            all_task_ids.add(downstream_id)
        
        # Get base priorities
        priorities = {t.job_id: t.base_priority for t in tasks}
        effective = dict(priorities)  # Start with base priorities
        
        # Reverse topological sort (sinks first)
        # Using Kahn's on reversed graph
        in_degree = defaultdict(int)
        for src in adj:
            for dst in adj[src]:
                in_degree[src] += 1  # In reverse, downstream -> upstream
        
        # Sinks: nodes with no outgoing edges (no entry in adj or empty set)
        queue = deque([
            t for t in all_task_ids 
            if not adj[t]  # No downstream dependencies
        ])
        
        reverse_order = []
        while queue:
            node = queue.popleft()
            reverse_order.append(node)
            for upstream in rev_adj[node]:
                in_degree[upstream] -= 1
                if in_degree[upstream] == 0:
                    queue.append(upstream)
        
        # Propagate: in reverse topological order (sinks first),
        # each node's effective priority = max(own, max(downstream effective))
        boosted = []
        for task_id in reverse_order:
            downstream_max = max(
                (effective[d] for d in adj[task_id]),
                default=0
            )
            
            if downstream_max > effective[task_id]:
                old_effective = effective[task_id]
                effective[task_id] = downstream_max
                boosted.append({
                    'task_id': task_id,
                    'old_priority': old_effective,
                    'new_priority': downstream_max,
                    'reason': f'Priority inheritance from downstream task(s)'
                })
        
        # Apply to database and queue
        for task_id in all_task_ids:
            if effective[task_id] != priorities[task_id]:
                self.priority_repo.update_effective_priority(
                    task_id, effective[task_id])
                self.priority_queue.update_score(
                    task_id, effective[task_id])
                self.audit_repo.log_change(
                    job_id=task_id,
                    old_priority=priorities[task_id],
                    new_priority=effective[task_id],
                    reason='INVERSION_BOOST',
                    changed_by='system/inversion-detector'
                )
        
        return {
            'dag_id': dag_id,
            'tasks_boosted': len(boosted),
            'details': boosted
        }
    
    def detect_runtime_inversion(self):
        """
        Detect priority inversion in running DAG instances.
        
        Scenario: Task B (P=900) is QUEUED, waiting for Task A (P=100)
        which is also QUEUED but behind medium-priority tasks.
        
        Runs every 30 seconds on the scheduler leader.
        """
        # Find QUEUED tasks with QUEUED upstream dependencies
        inversions = self.execution_repo.find_inversions()
        # SQL: SELECT downstream.*, upstream.*
        #      FROM dag_edge e
        #      JOIN job_execution downstream ON e.downstream_job_id = downstream.job_id
        #      JOIN job_execution upstream ON e.upstream_job_id = upstream.job_id
        #      JOIN job_priority_config dp ON downstream.job_id = dp.job_id
        #      JOIN job_priority_config up ON upstream.job_id = up.job_id
        #      WHERE downstream.status = 'QUEUED'
        #        AND upstream.status = 'QUEUED'
        #        AND dp.effective_priority > up.effective_priority + 100
        
        for inversion in inversions:
            log.warn("Priority inversion detected: task {} (P={}) blocked by task {} (P={})",
                inversion.downstream_id, inversion.downstream_priority,
                inversion.upstream_id, inversion.upstream_priority)
            
            # Boost upstream to downstream's priority
            self.priority_repo.update_effective_priority(
                inversion.upstream_id, inversion.downstream_priority)
            self.priority_queue.update_score(
                inversion.upstream_id, inversion.downstream_priority)
            
            metrics.counter("inversion.detected_and_resolved").increment()
```

**Interviewer Q&A:**

**Q1: How is this different from the Mars Pathfinder priority inversion bug?**
A: The Mars Pathfinder bug was a mutex-based priority inversion: a high-priority task was blocked on a mutex held by a low-priority task, while a medium-priority task preempted the low-priority task (preventing it from releasing the mutex). Our scenario is dependency-based: a high-priority DAG task depends on a low-priority upstream task. The solution is the same concept (priority inheritance) applied to task dependencies instead of mutexes.

**Q2: Does priority inheritance cause priority inflation (everything becomes high priority)?**
A: Only tasks that are actual dependencies of high-priority tasks get boosted. The boost lasts only while the downstream task is waiting. Once the downstream task completes, the upstream task's effective priority reverts to its base (if it has other, lower-priority dependents, it keeps the highest among them). In practice, priority inheritance affects < 1% of tasks.

**Q3: What about circular priority inheritance?**
A: DAGs are acyclic by definition -- we validate this on submission. Circular priority inheritance is impossible because there are no circular dependencies. If a cycle were somehow introduced, the topological sort in `apply_priority_inheritance` would detect it (incomplete traversal).

**Q4: How does this interact with the aging mechanism?**
A: Priority inheritance sets a floor, not a ceiling. If a task has effective_priority = 900 due to inheritance and is waiting, aging still applies: effective_priority = max(base + aging, inherited_priority). Since inheritance typically sets a higher floor, aging rarely changes the effective priority of inherited tasks. They're already high priority.

**Q5: What's the runtime complexity of priority inheritance for a large DAG?**
A: O(V + E) -- one pass in reverse topological order, visiting each node and edge once. For a DAG with 1000 tasks and 5000 edges, this completes in < 1ms. Applied once at DAG submission time and then periodically for runtime detection (every 30s, only for active DAGs with QUEUED tasks).

**Q6: Can priority inheritance cause a low-priority task to be preempted less (because it's now effectively high-priority)?**
A: Yes, that's exactly the desired behavior. If task L is blocking task H (P=900), then L's effective priority becomes 900 and it's no longer a preemption target. This ensures L completes quickly, unblocking H. Without inheritance, L might be preempted by a medium-priority task M, further delaying H -- the classic inversion scenario.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

Same score-based placement as the distributed job scheduler, but with priority-aware scoring:

```python
def score_worker(worker, job, job_priority):
    score = 0
    
    # Resource fit (30 points)
    cpu_headroom = worker.available_cpu / worker.total_cpu
    mem_headroom = worker.available_memory / worker.total_memory
    score += 30 * (cpu_headroom + mem_headroom) / 2
    
    # Priority affinity (30 points): prefer workers already running similar-priority jobs
    # This reduces fragmentation (critical jobs cluster on some workers, batch on others)
    avg_running_priority = get_avg_priority_on_worker(worker)
    priority_similarity = 1 - abs(avg_running_priority - job_priority.effective_priority) / 1000
    score += 30 * priority_similarity
    
    # Spread for resilience (20 points)
    job_count = get_running_count(worker)
    score += 20 * (1 - min(job_count / 100, 1.0))
    
    # Recency (20 points)
    heartbeat_age = (now() - worker.last_heartbeat).total_seconds()
    score += 20 * max(0, 1 - heartbeat_age / 30)
    
    return score
```

### Conflict Detection

- **Priority conflicts (two jobs want the same worker):** Dispatch engine processes one job at a time from the priority queue. No concurrent conflicts -- single-threaded dispatch within the leader.
- **Resource conflicts:** Optimistic locking on `worker_node.available_*` columns. If two dispatch attempts target the same worker, one succeeds and the other retries with the next worker.

### Queue & Priority Implementation

See Section 6.1 (Multi-Level Priority Queue with Weighted Fair Dequeue).

### Preemption Policy

See Section 6.2 (Preemption Engine).

### Starvation Prevention

See Section 6.3 (Priority Aging) and Section 6.4 (Priority Inversion Detection).

Summary:
1. **Aging:** +1 effective priority per minute, capped at +200
2. **Band minimum throughput:** 5% guaranteed for BATCH, 25% for NORMAL
3. **Inversion detection:** Priority inheritance for DAG dependencies
4. **Maximum wait guarantee:** 4 hours (empirical, based on aging + band weights)
5. **Per-tenant fairness:** Round-robin within same effective priority level

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Limit |
|-----------|-----------------|-------|
| Priority Queue (Redis) | Redis Cluster with hash slots per band | 3 bands x 3 shards = 9 Redis nodes |
| Dispatch Engine | Single leader (bounded by Redis dequeue throughput) | ~100K dispatches/min |
| Aging Engine | Parallelizable by job_id range | 10 threads, each aging 10K jobs/cycle |
| Preemption Engine | Single-threaded (avoid concurrent preemption races) | ~100 evaluations/min |

### Database Scaling

Same strategy as distributed_job_scheduler.md. Priority-specific:
- `job_priority_config` table is small (one row per active job) and fits in buffer pool
- `preemption_record` grows slowly (5K/day) -- no special partitioning needed
- `priority_audit` partitioned monthly (high write volume)

### Caching

| Layer | Data | TTL |
|-------|------|-----|
| L1: JVM Caffeine | Tenant priority quotas | 30s |
| L2: Redis | Effective priority per job | Updated by aging engine |
| L3: Redis | Worker available resources | Updated by heartbeats (10s) |

### Kubernetes-Specific

```yaml
# PriorityClass for worker pods (k8s native priority)
apiVersion: scheduling.k8s.io/v1
kind: PriorityClass
metadata:
  name: critical-workers
value: 1000000
globalDefault: false
description: "Workers for CRITICAL priority band"
preemptionPolicy: PreemptLowerPriority

---
apiVersion: scheduling.k8s.io/v1
kind: PriorityClass
metadata:
  name: batch-workers
value: 100
preemptionPolicy: Never
```

### Interviewer Q&A

**Q1: How does Kubernetes PriorityClass relate to your application-level priority?**
A: They're different layers. Kubernetes PriorityClass determines pod scheduling priority (which pods get scheduled on nodes when resources are scarce). Our application priority determines job scheduling priority (which jobs get dispatched to workers). We align them: CRITICAL worker pods get high k8s PriorityClass so they're scheduled before BATCH worker pods. But a BATCH worker pod can still execute a CRITICAL job if it's available -- our priority system is at the application layer.

**Q2: What happens to priority state during a Redis failover?**
A: Redis Sentinel failover takes 10-15s. During this window, the dispatch engine can't dequeue from the priority queue. Jobs are safely buffered (not lost -- they're in MySQL). On failover completion, the new Redis primary has the replicated data. If the failover involves data loss (async replication lag), we rebuild the sorted sets from MySQL: `SELECT job_id, effective_priority FROM job_priority_config WHERE status = 'QUEUED'`. This takes ~5s for 100K jobs.

**Q3: How do you handle priority across multiple Kubernetes clusters?**
A: Each cluster has its own scheduler and priority system. Cross-cluster priority is handled at a higher level: a global load balancer routes jobs to clusters based on capacity and SLO. If cluster A is overloaded for CRITICAL jobs, the global router sends them to cluster B. Within each cluster, the local priority system operates independently.

**Q4: How do you benchmark the fairness of your priority system?**
A: We define fairness metrics: (1) **Priority adherence:** % of time higher-priority jobs start before lower-priority jobs (target: > 99% within the same band). (2) **Band throughput ratio:** actual dispatch ratio across bands vs. configured weights (target: within 5%). (3) **Starvation metric:** max wait time for any job in the last hour (target: < 4 hours for batch). We run continuous synthetic workloads that measure these metrics and alert on deviations.

**Q5: At extreme scale, does the aging engine become a bottleneck?**
A: At 1M queued jobs, the aging cycle processes 1M updates. With Redis pipeline (10K ops/batch), this is 100 pipeline calls, completing in ~2s. The MySQL bulk update is a single query. Total cycle time: ~3s, well within the 60s interval. At 10M queued jobs, we'd parallelize: 10 threads each handle a 1M-job shard, completing in ~3s total (I/O-bound on Redis, CPU-bound on MySQL).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| Redis (priority queue) down | Dispatch paused | Sentinel (10s) | Sentinel failover; rebuild from MySQL | 10-30s | 0 |
| Scheduler leader crash | All scheduling paused | etcd lease (15s) | Leader election to standby | 15-20s | 0 |
| Aging engine failure | Starvation risk increases | Metric: aging cycle missed | New leader resumes aging | 15-20s | 1 missed cycle |
| Preemption signal lost | Critical job delayed | Grace period timeout | Re-send termination; SIGKILL after 2x grace | 60s | 0 |
| Priority database corruption | Wrong priorities applied | Checksum validation | Rebuild from audit log | 5-10 min | 0 |
| Tenant quota enforcement failure | Quota exceeded | Monitoring: over-quota alerts | Rate limit at API gateway as backup | Instant | 0 |

### Automated Recovery

- **Stuck preemption:** If a preempted job doesn't terminate within 2x grace period, send SIGKILL. If worker is unreachable, mark worker as DEAD and recover all its jobs.
- **Priority drift:** Every 5 minutes, validate 1% sample of queued jobs: verify effective_priority = base + aging + escalation. Alert if discrepancy > 10.
- **Queue-MySQL sync:** Every 10 minutes, compare Redis queue depth with MySQL `COUNT(status='QUEUED')`. If delta > 5%, trigger full rebuild.

### Retry Strategy

Same as distributed job scheduler. Priority-specific: preempted jobs are re-queued at their original priority (no retry count increment -- preemption is not a failure).

### Circuit Breaker

- **Preemption circuit breaker:** If > 10% of preemptions result in the preempted job failing (not re-queueable), open the preemption circuit. During open state, no preemptions allowed. Half-open after 5 minutes.
- **Priority queue circuit breaker:** If Redis latency P99 > 50ms, fall back to MySQL-based dispatch (slower but functional).

### Consensus & Coordination

| Resource | Mechanism | Tool |
|----------|-----------|------|
| Scheduler leadership | Lease-based election | etcd |
| Priority queue operations | Atomic ZPOPMIN/ZADD | Redis |
| Aging state | Pipeline batch updates | Redis |
| Preemption decisions | Single-threaded on leader | No coordination needed |
| Priority overrides | Optimistic locking | MySQL |

---

## 10. Observability

### Key Metrics

| Metric | Type | Warning | Critical |
|--------|------|---------|----------|
| `priority.queue.depth` (per band) | Gauge | CRITICAL > 100, BATCH > 100K | CRITICAL > 1K, BATCH > 1M |
| `priority.queue.oldest_wait_s` (per band) | Gauge | CRITICAL > 30s, BATCH > 3h | CRITICAL > 120s, BATCH > 6h |
| `priority.dispatch.latency_ms` (per band) | Histogram | CRITICAL P99 > 500ms | CRITICAL P99 > 2s |
| `priority.preemption.rate` | Counter | > 50/hr | > 200/hr |
| `priority.preemption.waste_ratio` | Gauge | > 10% | > 25% |
| `priority.aging.cycle_duration_s` | Histogram | > 30s | > 55s (risks missing cycle) |
| `priority.inversion.detected` | Counter | > 10/hr | > 50/hr |
| `priority.starvation.at_risk_jobs` | Gauge | > 10 | > 100 |
| `priority.band.throughput_ratio` | Gauge | Deviates > 10% from weight | Deviates > 25% |
| `priority.quota.exceeded` (per tenant) | Counter | > 10/hr | > 100/hr |

### Distributed Tracing

Add priority-specific span attributes:
```
span.attributes:
  job.base_priority: 500
  job.effective_priority: 650
  job.priority_band: NORMAL
  job.aging_bonus: 100
  job.deadline_escalation: 50
  job.queue_wait_ms: 45000
  job.preempted: false
```

### Alerting

| Alert | Condition | Severity |
|-------|-----------|----------|
| Critical job starvation | CRITICAL band oldest_wait > 120s | P1 |
| Preemption storm | > 200 preemptions/hour | P2 |
| Aging engine stalled | No aging cycle completed in 120s | P2 |
| Priority inversion unresolved | Inversion persists > 5 min | P3 |
| Batch starvation | BATCH band oldest_wait > 6h | P3 |

---

## 11. Security

### Authentication & Authorization

Priority-specific RBAC:
```
Roles:
  job-submitter:    Submit jobs with priority 0-499 (batch only)
  job-operator:     Submit jobs with priority 0-899 (batch + normal)
  sla-responder:    Submit jobs with priority 0-1000 (all bands, for incident response)
  priority-admin:   Override any job's priority, modify quotas
  platform-admin:   All of the above + preemption policy configuration
```

Justification: Restricting who can submit critical-priority jobs prevents abuse and ensures the CRITICAL band is reserved for genuine emergencies.

### Audit Logging

Every priority change is logged (see `priority_audit` table). Preemption events are logged with full context. Immutable audit trail for compliance and post-incident analysis.

---

## 12. Incremental Rollout Strategy

### Phases

1. **Shadow mode (Week 1-2):** Run priority evaluation in parallel with existing scheduler; compare dispatch ordering decisions without acting on them.
2. **Batch band only (Week 3-4):** Route only BATCH priority (0-499) jobs through new priority system. NORMAL and CRITICAL use old scheduler.
3. **Batch + Normal (Week 5-6):** Add NORMAL band. CRITICAL still on old scheduler.
4. **Full rollout (Week 7-8):** Route CRITICAL through new system. Enable preemption (initially with 10% budget).
5. **Preemption tuning (Week 9-10):** Gradually increase preemption budget to 100%. Tune priority delta threshold based on observed metrics.

### Rollback Strategy

- Each band can independently revert to old scheduler (routing by priority range)
- Preemption can be disabled globally without reverting the queue system

### Rollout Interviewer Q&A

**Q1: Why roll out preemption separately from the priority queue?**
A: The priority queue is a read-path change (affects dispatch ordering) -- low risk. Preemption is a write-path change (kills running jobs) -- high risk. Separating them allows us to validate queue ordering correctness before introducing the destructive preemption capability. If the priority queue has a bug that ranks jobs incorrectly, adding preemption would compound the damage.

**Q2: How do you validate that the new priority system produces better outcomes than the old one?**
A: Define success metrics before rollout: (1) Critical job P99 start time < 200ms (was: 5s). (2) Batch job max wait < 4h (was: unbounded). (3) Overall throughput unchanged (±5%). (4) Preemption waste ratio < 10%. A/B test during blue-green phase: compare metrics between old and new for equivalent workloads.

**Q3: What if the aging algorithm causes unexpected behavior in production?**
A: Aging is conservative (1 point/min, capped at +200). Even if aging creates an unexpected priority distribution, the impact is bounded. We can instantly adjust aging parameters (rate, cap) via etcd config without restart. If aging needs to be disabled entirely, we set `aging_increment = 0` and all jobs freeze at their base priority.

**Q4: How do you handle the transition period when some jobs are in the old system and some in the new?**
A: During canary/blue-green, both systems share the same MySQL state. A job submitted to the new system has priority metadata in MySQL that the old system ignores (it uses its own priority logic). The old system continues to function normally for its portion of traffic. There's no cross-contamination.

**Q5: How do you test preemption without disrupting production?**
A: We run preemption in "dry-run" mode: the engine evaluates preemption decisions and logs what it would do, but doesn't send SIGTERM. We review the logs to verify: correct victim selection, priority delta threshold compliance, cost calculation accuracy. Only after 1 week of correct dry-run decisions do we enable actual preemption.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Selected | Trade-off | Rationale |
|----------|---------|----------|-----------|-----------|
| Priority range | 0-9 (coarse), 0-100 (medium), 0-1000 (fine) | 0-1000 | More granularity adds complexity | Fine-grained priorities needed for aging and escalation math |
| Queue bands | 2 (high/low), 3 (critical/normal/batch), N (per-priority) | 3 bands | Moderate granularity vs. simplicity | 3 covers the three natural tiers; N queues add operational overhead |
| Weighted dequeue | Strict priority, WFQ, DRR | WFQ | Slightly less optimal for critical vs. starvation prevention | WFQ provides guaranteed minimum throughput per band |
| Preemption | None, threshold-based, cost-aware | Threshold + cost-aware | Complexity vs. waste reduction | Pure threshold ignores wasted work; cost-awareness prevents wasteful preemptions |
| Aging function | Linear, exponential, step | Linear with cap | Predictability vs. aggressive starvation prevention | Linear is easy to reason about and explain to users |
| Inversion handling | None, inheritance, ceiling | Inheritance | Implementation cost vs. correctness | Inversion can cause real SLA violations; inheritance is the proven solution |

---

## 14. Agentic AI Integration

### Where AI Adds Value

| Use Case | AI Capability | Impact |
|----------|--------------|--------|
| **Auto-priority assignment** | LLM analyzes job description, SLA, and historical patterns to suggest priority | Correct initial priority reduces manual escalations by 40% |
| **Preemption optimization** | ML model predicts job completion time more accurately | Reduces wasted preemptions (don't preempt a job that's 95% done) |
| **Starvation prediction** | Time-series model predicts which jobs will starve based on queue trends | Proactive aging boost before starvation occurs |
| **Quota recommendation** | Analyze tenant usage patterns to recommend fair quotas | Reduce quota-related conflicts between teams |

### Agent Loop Example: Priority Recommendation

```python
class PriorityRecommendationAgent:
    def observe(self, job_submission):
        return {
            'job_name': job_submission.name,
            'tenant': job_submission.tenant_id,
            'resource_request': job_submission.resource_request,
            'historical_priority': self.get_historical_priorities(job_submission.name),
            'current_queue_state': self.priority_queue.get_stats(),
            'tenant_sla_tier': self.get_sla_tier(job_submission.tenant_id),
        }
    
    def reason(self, observation):
        prompt = f"""
        Given this job submission and context, recommend a priority (0-1000).
        
        Job: {observation['job_name']}
        Tenant SLA tier: {observation['tenant_sla_tier']}
        Historical priorities for similar jobs: {observation['historical_priority']}
        Current queue depths: {observation['current_queue_state']}
        
        Rules:
        - CRITICAL (900-1000): Only for incident response, SLA-breaching situations
        - NORMAL (500-899): Standard workloads with SLA targets
        - BATCH (0-499): Background, non-urgent work
        - Consider the tenant's SLA tier (platinum=800+, gold=600+, silver=400+)
        
        Output: {{"priority": int, "band": str, "reasoning": str}}
        """
        return self.llm.generate(prompt, temperature=0.1)
    
    def act(self, job_id, recommendation):
        # Only suggest, don't auto-apply for priority (human-in-the-loop)
        self.notify_submitter(job_id, recommendation)
```

### Guard Rails

- AI can suggest priorities but cannot auto-assign CRITICAL (900+) without human approval
- AI preemption recommendations are dry-run only; humans approve actual preemptions
- Maximum 10 priority change suggestions per minute to prevent spam

---

## 15. Complete Interviewer Q&A Bank

**Q1: How does your priority system handle a sudden influx of CRITICAL jobs that exceed capacity?**
A: Even CRITICAL jobs are queued if no workers are available. The dispatch engine serves CRITICAL band first (70% weight), so they dequeue fastest. If CRITICAL queue depth > threshold, we trigger KEDA to scale up CRITICAL worker pool. Additionally, preemption frees resources from lower-priority jobs. In a true emergency (P0 incident), platform admins can increase CRITICAL weight to 100% temporarily via admin override.

**Q2: What's the difference between application-level priority and Kubernetes pod priority?**
A: Kubernetes pod priority determines which pods survive during node resource pressure (the kubelet evicts low-priority pods first). Our application priority determines which jobs get dispatched to workers first. They're complementary: CRITICAL worker pods should have high k8s PriorityClass so they're not evicted, and CRITICAL jobs should have high application priority so they're dispatched first.

**Q3: How do you handle priority for multi-step workflows (DAGs)?**
A: All tasks in a DAG inherit the highest priority among them (priority inheritance, Section 6.4). This ensures upstream dependencies of critical tasks are not starved by medium-priority unrelated tasks. The effective priority applies at the per-task level, not the DAG level, allowing fine-grained scheduling.

**Q4: Can a user observe the effect of priority on their job's wait time?**
A: Yes. `jobctl queue-position <job_id>` shows: current effective priority, queue position within the band, estimated wait time (based on current dispatch rate), and number of jobs ahead with higher priority. The API also returns `estimated_start_time` at submission.

**Q5: How do you prevent priority escalation wars between tenants?**
A: (1) Per-tenant quotas on CRITICAL submissions (max N per hour). (2) RBAC: only `sla-responder` role can submit CRITICAL. (3) Monitoring: alert when any tenant's average submitted priority increases by > 100 over a week. (4) Review process: CRITICAL access requires manager approval and is audited.

**Q6: What happens to running jobs during a priority queue Redis rebuild?**
A: Running jobs are unaffected (they're on workers, not in the queue). Only queued jobs are affected during the rebuild window (10-30 seconds). During this window, no new dispatches occur. On rebuild completion, all queued jobs are in the correct sorted sets and dispatching resumes. The rebuild is triggered automatically and completes without manual intervention.

**Q7: How do you handle priority for spot/preemptible infrastructure?**
A: Spot workers only accept BATCH priority jobs (never CRITICAL or NORMAL). When a spot instance is reclaimed by the infrastructure, the BATCH job is re-queued -- this is not counted as a priority preemption (it's infrastructure preemption). BATCH jobs on spot have `preemptible=true` by default. Users opting into spot get lower cost but accept re-queueing risk.

**Q8: What's the memory footprint of the priority system?**
A: Redis sorted sets: 100K queued jobs x 50 bytes = 5 MB. Aging state: 100K entries x 50 bytes = 5 MB. JVM: priority evaluation cache (tenant quotas, task configs) ~50 MB. Total: ~60 MB, negligible on a 16 GB scheduler JVM.

**Q9: How would you extend this to support multiple priority dimensions (urgency AND importance)?**
A: We'd use a 2D priority model: `effective_score = urgency_weight * urgency + importance_weight * importance`. The dispatch queue sorts by effective_score. This is equivalent to a weighted sum priority. Alternatively, we could use lexicographic ordering (sort by urgency first, then importance for ties). The current single-dimension model is simpler and sufficient for most use cases.

**Q10: How do you handle backpressure when the priority queue exceeds capacity?**
A: Redis sorted sets don't have a hard capacity limit (they grow until memory is exhausted). We monitor queue depth and apply backpressure at the API layer: when total queue depth > 1M, the API gateway rejects BATCH submissions with 429 (Too Many Requests). NORMAL and CRITICAL are always accepted. This preserves capacity for important work during overload.

**Q11: How would you implement deadline-aware priority escalation?**
A: Jobs with a `deadline` field get automatic priority escalation as the deadline approaches. Formula: `escalation = min(escalation_rate * hours_remaining_inverse, max_escalation)`. For example, a job with deadline in 1 hour and escalation_rate=50 gets +50 priority. In 30 minutes: +100. In 15 minutes: +200. This is computed in the aging engine (same 60-second cycle) alongside standard aging.

**Q12: What SLA guarantees can you offer for each priority band?**
A: CRITICAL: dispatch within 200ms P99, start within 30s (including potential preemption). NORMAL: dispatch within 2s P99, start within 5 minutes. BATCH: dispatch within 30s P99, start within 4 hours. These SLAs assume normal load. During extreme overload, we may temporarily relax BATCH SLA but never CRITICAL.

**Q13: How do you simulate the priority system for capacity planning?**
A: We built a discrete-event simulator that replays historical job submissions with different priority distributions, worker counts, and aging parameters. It predicts: queue depths over time, wait time distributions per band, preemption rates, and starvation risk. We run simulations before changing any priority system parameter.

**Q14: How does preemption interact with checkpointing?**
A: If a job supports checkpointing (saves progress periodically), preemption is less costly: the re-queued job resumes from the last checkpoint instead of restarting from scratch. The preemption cost function accounts for this: `cost *= (1 - last_checkpoint_ratio)`. Jobs with recent checkpoints are cheaper to preempt and thus preferred victims (counter-intuitively, this helps them resume faster).

**Q15: How do you handle "priority donation" (a team wanting to give their high-priority quota to another team)?**
A: We don't support direct quota transfer (too complex to audit). Instead, the donating team's admin can submit jobs on behalf of the receiving team (using a service account with cross-tenant permissions). Or the platform admin temporarily increases the receiving team's quota. All changes are audited.

---

## 16. References

- [Linux CFS Scheduler - Priority and Nice Values](https://www.kernel.org/doc/html/latest/scheduler/sched-nice-design.html)
- [Mars Pathfinder Priority Inversion Bug (JPL)](https://www.cs.unc.edu/~anderson/teach/comp790/papers/mars_pathfinder_long_version.html)
- [Kubernetes Pod Priority and Preemption](https://kubernetes.io/docs/concepts/scheduling-eviction/pod-priority-preemption/)
- [Weighted Fair Queuing (WFQ)](https://en.wikipedia.org/wiki/Weighted_fair_queueing)
- [Redis Sorted Sets ZPOPMIN](https://redis.io/commands/zpopmin/)
- [Priority Inheritance Protocol](https://en.wikipedia.org/wiki/Priority_inheritance)
- [Google Borg - Priority Bands and Preemption](https://research.google/pubs/pub43438/)
- [Dominant Resource Fairness: Fair Allocation of Multiple Resource Types](https://cs.stanford.edu/~matei/papers/2011/nsdi_drf.pdf) (Ghodsi et al., NSDI 2011)
- [Deficit Round Robin (DRR) Scheduling](https://en.wikipedia.org/wiki/Deficit_round_robin)
- [Kubernetes KEDA Autoscaler](https://keda.sh/)
