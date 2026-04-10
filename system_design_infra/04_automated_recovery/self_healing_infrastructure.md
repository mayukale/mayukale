# System Design: Self-Healing Infrastructure Platform

> **Relevance to role:** A cloud infrastructure platform engineer must build systems that detect degradation and automatically restore healthy state without human intervention. This is the core promise of IaaS — tenants expect the platform to absorb hardware and software failures transparently. Self-healing spans bare-metal (IPMI/BMC watchdog, PXE reimaging), Kubernetes (reconciliation loops, node auto-repair), and application layers (health probes, pod restart policies). Mastery here separates a reactive ops team from a truly autonomous infrastructure platform.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Health Monitoring:** Continuously assess the health of bare-metal hosts, VMs, containers, and application processes at configurable intervals.
2. **Automated Detection:** Identify failures across multiple layers — hardware (disk, NIC, memory), OS (kernel panics, OOM), runtime (container daemon), and application (liveness/readiness).
3. **Reconciliation Loop:** Continuously compare desired state (declarative spec) against actual state and drive convergence — the Kubernetes controller model generalized.
4. **Node Auto-Repair:** Drain workloads from unhealthy nodes, cordon the node, attempt repair (reboot, re-image), and replace if repair fails.
5. **Pod/Process Self-Healing:** Restart failed pods per restart policy (Always, OnFailure, Never), respecting backoff timers and crash-loop thresholds.
6. **Bare-Metal Self-Healing:** Use IPMI/BMC watchdog timers for hung machines, PXE reimaging for corrupted OS, and firmware-level resets.
7. **Cascading Failure Prevention:** Implement circuit breakers, load shedding, and rate-limited recovery to prevent recovery storms.
8. **Runbook Integration:** Execute structured remediation steps with preconditions, post-conditions, dry-run mode, and human approval gates for high-impact actions.
9. **Notification & Escalation:** Inform operators of automated actions taken, escalate when auto-remediation is exhausted.
10. **Audit Trail:** Record every detection, decision, and action for post-incident analysis.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Detection latency | < 30 seconds for critical failures |
| Remediation initiation | < 60 seconds after confirmed failure |
| False positive rate | < 0.1% (verified by consensus) |
| Availability of self-healing service | 99.99% (must be more available than what it heals) |
| Scale | 100,000 bare-metal hosts, 2M+ pods |
| Remediation throughput | 500 concurrent node repairs |
| Audit log retention | 2 years |

### Constraints & Assumptions
- Infrastructure spans 5 regions, 3 AZs per region.
- Bare-metal fleet is heterogeneous (different CPU generations, GPU nodes, storage nodes).
- Kubernetes clusters range from 50 to 10,000 nodes each.
- MySQL is the primary relational store; Elasticsearch for log/event indexing.
- Java and Python services are the primary workloads running on the platform.
- BMC/IPMI access is available on all bare-metal hosts via an out-of-band management network.
- Assumption: network partitions are temporary (< 5 minutes for intra-AZ), prolonged partitions trigger cross-AZ failover.

### Out of Scope
- Application-level business logic recovery (e.g., retrying failed business transactions).
- Disaster recovery across regions (covered in automated_failover_system.md).
- Chaos engineering experiments (covered in chaos_engineering_platform.md).
- Runbook authoring UI (covered in auto_remediation_runbook_system.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|---|---|---|
| Bare-metal hosts | 5 regions x 3 AZs x ~6,667 hosts/AZ | 100,000 hosts |
| Pods | ~20 pods/host average x 100K hosts | 2,000,000 pods |
| Health checks per host | Liveness + readiness + node-level = 3 checks/host x every 10s | 30,000 checks/sec (hosts) |
| Health checks per pod | 2 checks/pod x every 10s | 400,000 checks/sec (pods) |
| Total health check events | 30K + 400K | ~430,000 events/sec |
| Failure detection events | 0.1% host failure rate/day = 100 hosts/day, 0.5% pod restart rate/hour = 10K pods/hour | ~100 host + ~2.8 pod/sec |
| Remediation actions | ~80% auto-remediated | ~80 host + ~2.2 pod remediations/sec |

### Latency Requirements

| Operation | Target Latency |
|---|---|
| Health check round-trip | < 5 seconds |
| Failure detection (single check fail → confirmed) | < 30 seconds (3 consecutive failures at 10s interval) |
| Remediation decision | < 5 seconds after confirmed failure |
| Node drain initiation | < 30 seconds after decision |
| Pod restart | < 10 seconds after decision |
| Bare-metal IPMI reset | < 60 seconds end-to-end |
| PXE reimaging | < 15 minutes |
| Full node replacement (new hardware) | < 30 minutes (automated provisioning) |

### Storage Estimates

| Data Type | Calculation | Result |
|---|---|---|
| Health check events (raw) | 430K events/sec x 200 bytes x 86400 sec/day | ~7.4 TB/day |
| Health check events (retained) | 7 day hot storage, 90 day warm, 2 year cold | 52 TB hot, 670 TB warm |
| Remediation action logs | ~250 actions/sec x 1 KB x 86400 | ~21 GB/day |
| Desired state store | 100K hosts x 5 KB + 2M pods x 2 KB | ~4.5 GB (fits in memory) |
| Audit log | ~500 entries/sec x 500 bytes x 86400 | ~21 GB/day |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Health check ingress | 430K events/sec x 200 bytes | ~86 MB/sec |
| Remediation commands egress | 250 actions/sec x 2 KB | ~500 KB/sec |
| State sync (etcd/ZK) | 100K hosts x 5 KB state update/min | ~8 MB/sec |
| Event streaming (Kafka) | 430K events/sec x 200 bytes (replicated x3) | ~258 MB/sec |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         SELF-HEALING CONTROL PLANE                         │
│                                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │  Health Check │  │   Failure    │  │ Remediation  │  │   Runbook     │  │
│  │  Aggregator   │  │  Detector    │  │   Engine     │  │   Executor    │  │
│  │  (per-region) │──│  (consensus) │──│  (scheduler) │──│  (per-action) │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └───────┬───────┘  │
│         │                  │                  │                   │          │
│  ┌──────▼───────┐  ┌──────▼───────┐  ┌──────▼───────┐  ┌───────▼───────┐  │
│  │ Health Check  │  │  State Store │  │   Rate       │  │  Audit Log    │  │
│  │ Registry     │  │  (etcd)      │  │   Limiter    │  │  (MySQL + ES) │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  └───────────────┘  │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │
              ┌────────────────┼─────────────────┐
              │                │                  │
              ▼                ▼                  ▼
┌──────────────────┐ ┌────────────────┐ ┌─────────────────┐
│  KUBERNETES LAYER │ │  BARE-METAL    │ │   APPLICATION   │
│                   │ │  LAYER         │ │   LAYER         │
│ ┌──────────────┐  │ │ ┌────────────┐ │ │ ┌─────────────┐ │
│ │  kubelet     │  │ │ │ Node Agent  │ │ │ │ Sidecar     │ │
│ │  (probes)    │  │ │ │ (hardware)  │ │ │ │ (deep check)│ │
│ ├──────────────┤  │ │ ├────────────┤ │ │ ├─────────────┤ │
│ │  Node Problem│  │ │ │ BMC/IPMI   │ │ │ │ Process     │ │
│ │  Detector    │  │ │ │ Controller │ │ │ │ Manager     │ │
│ ├──────────────┤  │ │ ├────────────┤ │ │ ├─────────────┤ │
│ │  Controller  │  │ │ │ PXE Boot   │ │ │ │ Circuit     │ │
│ │  Manager     │  │ │ │ Server     │ │ │ │ Breaker     │ │
│ └──────────────┘  │ │ └────────────┘ │ │ └─────────────┘ │
└──────────────────┘ └────────────────┘ └─────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Health Check Aggregator** | Collects health signals from all layers, normalizes them into a unified health model, applies flap detection. One instance per region for locality. |
| **Failure Detector** | Consensus-based failure confirmation. Requires N-of-M health checkers to agree before declaring failure. Prevents false positives from transient issues or network glitches. |
| **Remediation Engine** | Selects and schedules remediation actions based on failure type, blast radius limits, and cooldown periods. Central scheduler ensures we don't over-remediate. |
| **Runbook Executor** | Executes multi-step remediation runbooks with preconditions, post-conditions, dry-run support, and rollback. |
| **Health Check Registry** | Stores registered health checks per service, host, and cluster. Serves as the source of truth for what checks exist. |
| **State Store (etcd)** | Stores desired state and current state for the reconciliation loop. Chosen for strong consistency and Kubernetes native integration. |
| **Rate Limiter** | Prevents remediation storms — limits concurrent node drains per cluster, concurrent reimages per rack, etc. |
| **Audit Log** | Every automated action is logged to MySQL (structured) and Elasticsearch (searchable). |
| **kubelet (probes)** | Executes liveness, readiness, and startup probes for pods. Reports to API server. |
| **Node Problem Detector** | DaemonSet that detects hardware/kernel/runtime issues and reports NodeConditions. |
| **Controller Manager** | Runs reconciliation loops — compares desired vs actual and takes corrective action. |
| **Node Agent** | Bare-metal agent that monitors hardware health (SMART, temperatures, ECC errors) via IPMI sensors. |
| **BMC/IPMI Controller** | Sends out-of-band commands to BMC — power cycle, reset, sensor reads — via the management network. |
| **PXE Boot Server** | Reimages bare-metal hosts from known-good images when OS corruption is detected. |
| **Sidecar (deep check)** | In-pod sidecar that performs deep health checks — DB connectivity, cache access, downstream service reachability. |
| **Circuit Breaker** | Prevents cascading failures by stopping requests to unhealthy downstream services after failure threshold is exceeded. |

### Data Flows

**Primary Flow — Health Check → Detection → Remediation:**
1. Health check agents (kubelet probes, node agents, sidecars) emit health signals → Kafka topic `health-events`.
2. Health Check Aggregator consumes events, applies flap detection (require 3 consecutive failures), and writes confirmed failures to `failure-candidates` topic.
3. Failure Detector consumes candidates, performs consensus check (cross-references multiple signal sources), writes confirmed failures to `confirmed-failures` topic.
4. Remediation Engine consumes confirmed failures, checks rate limits and cooldown, selects runbook, schedules execution.
5. Runbook Executor executes steps, logs results to audit log, reports outcome back to Remediation Engine.

**Secondary Flow — Reconciliation Loop:**
1. Controller reads desired state from etcd.
2. Controller observes actual state from kubelet/node-agent reports.
3. If desired != actual, controller computes diff and issues corrective actions (create pod, drain node, etc.).
4. Loop runs continuously with configurable sync period (default 10 seconds).

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Health check definitions
CREATE TABLE health_checks (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    check_id        VARCHAR(128) NOT NULL UNIQUE,
    target_type     ENUM('host', 'pod', 'service', 'bare_metal') NOT NULL,
    target_id       VARCHAR(256) NOT NULL,         -- host FQDN, pod UID, service name
    check_type      ENUM('liveness', 'readiness', 'startup', 'deep', 'hardware') NOT NULL,
    protocol        ENUM('http', 'tcp', 'exec', 'grpc', 'ipmi') NOT NULL,
    endpoint        VARCHAR(512),                   -- URL, port, command
    interval_sec    INT NOT NULL DEFAULT 10,
    timeout_sec     INT NOT NULL DEFAULT 5,
    failure_threshold INT NOT NULL DEFAULT 3,
    success_threshold INT NOT NULL DEFAULT 1,
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_target (target_type, target_id),
    INDEX idx_check_type (check_type)
) ENGINE=InnoDB;

-- Health check results (high volume — partitioned by day)
CREATE TABLE health_check_results (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    check_id        VARCHAR(128) NOT NULL,
    target_id       VARCHAR(256) NOT NULL,
    status          ENUM('healthy', 'unhealthy', 'unknown', 'degraded') NOT NULL,
    response_ms     INT,
    error_message   TEXT,
    checked_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_check_target_time (check_id, target_id, checked_at),
    INDEX idx_status_time (status, checked_at)
) ENGINE=InnoDB
PARTITION BY RANGE (UNIX_TIMESTAMP(checked_at)) (
    -- Partitions created by automation, one per day
);

-- Detected failures
CREATE TABLE detected_failures (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    failure_id      VARCHAR(128) NOT NULL UNIQUE,
    target_type     ENUM('host', 'pod', 'service', 'bare_metal') NOT NULL,
    target_id       VARCHAR(256) NOT NULL,
    failure_type    ENUM('crash', 'hang', 'hardware', 'network', 'resource_exhaustion',
                         'kernel_panic', 'oom', 'disk_failure', 'runtime_failure') NOT NULL,
    severity        ENUM('critical', 'high', 'medium', 'low') NOT NULL,
    detection_method ENUM('health_check', 'node_problem_detector', 'bmc_watchdog',
                          'reconciliation_diff', 'operator_report') NOT NULL,
    consensus_count INT NOT NULL DEFAULT 1,        -- how many detectors agree
    first_detected  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    confirmed_at    TIMESTAMP,
    resolved_at     TIMESTAMP,
    status          ENUM('candidate', 'confirmed', 'remediating', 'resolved', 'escalated') NOT NULL DEFAULT 'candidate',
    INDEX idx_target_status (target_type, target_id, status),
    INDEX idx_severity_status (severity, status),
    INDEX idx_first_detected (first_detected)
) ENGINE=InnoDB;

-- Remediation actions
CREATE TABLE remediation_actions (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    action_id       VARCHAR(128) NOT NULL UNIQUE,
    failure_id      VARCHAR(128) NOT NULL,
    runbook_id      VARCHAR(128),
    action_type     ENUM('restart_pod', 'restart_process', 'drain_node', 'cordon_node',
                         'uncordon_node', 'reboot_host', 'reimage_host', 'replace_host',
                         'ipmi_reset', 'escalate_human', 'circuit_break', 'load_shed') NOT NULL,
    target_id       VARCHAR(256) NOT NULL,
    status          ENUM('pending', 'approved', 'executing', 'succeeded', 'failed',
                         'rolled_back', 'skipped_cooldown', 'skipped_rate_limit') NOT NULL DEFAULT 'pending',
    dry_run         BOOLEAN NOT NULL DEFAULT FALSE,
    requires_approval BOOLEAN NOT NULL DEFAULT FALSE,
    approved_by     VARCHAR(128),
    started_at      TIMESTAMP,
    completed_at    TIMESTAMP,
    result_detail   TEXT,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (failure_id) REFERENCES detected_failures(failure_id),
    INDEX idx_failure (failure_id),
    INDEX idx_target_status (target_id, status),
    INDEX idx_action_type_time (action_type, created_at)
) ENGINE=InnoDB;

-- Node state (desired vs actual for reconciliation)
CREATE TABLE node_state (
    node_id         VARCHAR(256) PRIMARY KEY,
    cluster_id      VARCHAR(128) NOT NULL,
    rack_id         VARCHAR(64),
    desired_state   ENUM('active', 'cordoned', 'draining', 'decommissioned') NOT NULL,
    actual_state    ENUM('active', 'cordoned', 'draining', 'decommissioned', 'unknown', 'unreachable') NOT NULL,
    last_heartbeat  TIMESTAMP,
    health_score    DECIMAL(5,2) DEFAULT 100.00,    -- 0-100 composite score
    bmc_ip          VARCHAR(45),                     -- for IPMI commands
    os_image_version VARCHAR(64),
    cooldown_until  TIMESTAMP,                       -- no remediation until this time
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_cluster (cluster_id),
    INDEX idx_desired_actual (desired_state, actual_state),
    INDEX idx_health (health_score)
) ENGINE=InnoDB;

-- Cooldown tracking
CREATE TABLE cooldown_periods (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    target_id       VARCHAR(256) NOT NULL,
    action_type     VARCHAR(64) NOT NULL,
    cooldown_start  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    cooldown_end    TIMESTAMP NOT NULL,
    reason          VARCHAR(512),
    INDEX idx_target_action (target_id, action_type, cooldown_end)
) ENGINE=InnoDB;
```

### Database Selection

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **MySQL 8.0** | Strong consistency, ACID, partitioning, mature replication (Group Replication), familiar to ops | Write throughput ceiling for health check volume, schema rigidity | **Selected for metadata, state, and audit** |
| **Elasticsearch 8.x** | Full-text search, time-series aggregation, horizontal scaling | Eventual consistency, not suitable for state management | **Selected for health check results and event search** |
| **etcd** | Strong consistency, watch semantics, Kubernetes-native | 8 GB data size limit, not for high-volume writes | **Selected for desired/actual state store (reconciliation)** |
| **Apache Kafka** | High-throughput event streaming, durable, partitioned | Not a database — no query semantics | **Selected for event bus between components** |

**Justification:** We use each store for what it's best at. MySQL holds authoritative metadata (health check definitions, failure records, remediation actions) with ACID guarantees. Elasticsearch indexes the high-volume health check results for time-series queries and dashboards. etcd holds the small but critical desired-state/actual-state data used by reconciliation loops — it provides watch semantics that controllers need. Kafka connects all components as an event bus.

### Indexing Strategy

**MySQL:**
- `detected_failures(target_type, target_id, status)` — look up active failures for a target.
- `remediation_actions(target_id, status)` — check if a target already has an in-progress remediation.
- `cooldown_periods(target_id, action_type, cooldown_end)` — check if cooldown is active before remediation.
- `health_check_results` partitioned by day — drop old partitions for retention policy (7 days hot).

**Elasticsearch:**
- Time-based indices: `health-check-results-YYYY-MM-DD`.
- Index lifecycle management (ILM): hot → warm → cold → delete.
- Field mappings: `check_id` (keyword), `target_id` (keyword), `status` (keyword), `checked_at` (date), `error_message` (text with keyword sub-field).

**etcd:**
- Key scheme: `/nodes/{cluster_id}/{node_id}/desired`, `/nodes/{cluster_id}/{node_id}/actual`.
- Prefix watches on `/nodes/{cluster_id}/` for per-cluster controllers.

---

## 5. API Design

### REST/gRPC Endpoints

**Health Check Management (REST)**

```
POST   /api/v1/health-checks
  Body: {
    "target_type": "pod",
    "target_id": "web-server-abc123",
    "check_type": "liveness",
    "protocol": "http",
    "endpoint": "http://localhost:8080/healthz",
    "interval_sec": 10,
    "timeout_sec": 5,
    "failure_threshold": 3,
    "success_threshold": 1
  }
  Response: 201 { "check_id": "chk-a1b2c3", ... }

GET    /api/v1/health-checks?target_type=host&target_id=node-42
  Response: 200 [ { "check_id": "chk-...", ... }, ... ]

DELETE /api/v1/health-checks/{check_id}
  Response: 204

GET    /api/v1/health-checks/{check_id}/results?since=2026-04-08T00:00:00Z&limit=100
  Response: 200 { "results": [ { "status": "healthy", "response_ms": 12, ... } ] }
```

**Failure Management (REST)**

```
GET    /api/v1/failures?status=confirmed&severity=critical&limit=50
  Response: 200 { "failures": [ { "failure_id": "fail-xyz", "target_id": "node-42", ... } ] }

GET    /api/v1/failures/{failure_id}
  Response: 200 { "failure_id": "fail-xyz", "remediations": [...], ... }

POST   /api/v1/failures/{failure_id}/acknowledge
  Body: { "acknowledged_by": "oncall-user", "notes": "investigating" }
  Response: 200
```

**Remediation Management (REST)**

```
POST   /api/v1/remediations
  Body: {
    "failure_id": "fail-xyz",
    "action_type": "drain_node",
    "target_id": "node-42",
    "dry_run": false,
    "requires_approval": true
  }
  Response: 201 { "action_id": "rem-123", "status": "pending" }

POST   /api/v1/remediations/{action_id}/approve
  Body: { "approved_by": "sre-lead", "reason": "confirmed hardware failure" }
  Response: 200 { "status": "approved" }

GET    /api/v1/remediations/{action_id}/status
  Response: 200 { "status": "executing", "steps_completed": 3, "steps_total": 5 }

POST   /api/v1/remediations/{action_id}/rollback
  Body: { "reason": "post-condition failed" }
  Response: 200 { "status": "rolling_back" }
```

**Node State (gRPC — used by reconciliation controllers)**

```protobuf
service NodeStateService {
  rpc GetDesiredState(NodeRequest) returns (NodeState);
  rpc SetDesiredState(NodeStateUpdate) returns (NodeState);
  rpc WatchStateChanges(WatchRequest) returns (stream StateChange);
  rpc ReconcileNode(ReconcileRequest) returns (ReconcileResult);
}

message NodeRequest {
  string cluster_id = 1;
  string node_id = 2;
}

message NodeState {
  string node_id = 1;
  string cluster_id = 2;
  NodeStatus desired_state = 3;
  NodeStatus actual_state = 4;
  float health_score = 5;
  google.protobuf.Timestamp last_heartbeat = 6;
}

message ReconcileRequest {
  string cluster_id = 1;
  string node_id = 2;
  bool dry_run = 3;
}

message ReconcileResult {
  bool changes_made = 1;
  repeated string actions_taken = 2;
  string error = 3;
}
```

### CLI Design

```bash
# Health check management
shctl health-check list --target-type=host --target-id=node-42
shctl health-check create --target=pod:web-abc123 --type=liveness \
      --protocol=http --endpoint=/healthz --interval=10s --threshold=3
shctl health-check delete chk-a1b2c3
shctl health-check results chk-a1b2c3 --since=1h --format=table

# Failure inspection
shctl failures list --status=confirmed --severity=critical
shctl failures show fail-xyz --include-remediations
shctl failures ack fail-xyz --notes="investigating"

# Remediation
shctl remediate node-42 --action=drain --dry-run
shctl remediate node-42 --action=drain --approve
shctl remediation status rem-123
shctl remediation rollback rem-123 --reason="postcondition failed"

# Node state
shctl node list --cluster=prod-us-east-1a --state=unhealthy
shctl node show node-42 --include-health-history
shctl node cordon node-42 --reason="scheduled maintenance"
shctl node reconcile node-42 --dry-run

# Bare-metal operations
shctl bmc reset node-42 --type=power-cycle
shctl bmc sensor-read node-42 --sensor=temperature
shctl reimage node-42 --image=ubuntu-22.04-base-v42
```

---

## 6. Core Component Deep Dives

### 6.1 Reconciliation Loop Engine

**Why it's hard:** The reconciliation loop is the heart of self-healing — it must continuously compare desired state against actual state and drive convergence. The difficulty lies in: (a) avoiding oscillation (flapping between states), (b) handling partial failures (node partially drained), (c) maintaining consistency across distributed state, and (d) scaling to 100K+ nodes without the control plane becoming a bottleneck.

**Approaches Compared:**

| Approach | Consistency | Scalability | Complexity | Failure Mode |
|---|---|---|---|---|
| **Centralized controller** (single process) | Strong — single writer | Poor — single bottleneck | Low | SPOF |
| **Sharded controllers** (partition by cluster) | Strong within shard | Good — linear scaling | Medium | Shard failure loses a cluster |
| **Level-triggered reconciliation** (Kubernetes model) | Eventual — converges | Excellent — idempotent | Medium | Temporary divergence OK |
| **Edge-triggered + event-sourced** | Strong via event log | Good | High | Event log corruption |

**Selected Approach: Level-triggered reconciliation with sharded controllers (Kubernetes model).**

**Justification:** The Kubernetes controller model has proven at massive scale. Level-triggered means the controller doesn't care about the sequence of events — it only cares about the current delta between desired and actual. This makes it naturally idempotent and resilient to missed events. Sharding by cluster gives us linear scalability.

**Implementation Detail:**

```
while True:
    desired = state_store.get_desired_state(shard)
    actual  = observe_actual_state(shard)
    diff    = compute_diff(desired, actual)
    
    for resource in diff:
        if resource.actual == UNHEALTHY and resource.desired == ACTIVE:
            if not cooldown_active(resource):
                if rate_limiter.allow(resource.type):
                    action = select_remediation(resource)
                    execute_with_timeout(action)
                    record_audit(action)
        
        elif resource.actual == UNKNOWN:
            # Don't act immediately — could be transient
            if time_since_last_known(resource) > grace_period:
                mark_candidate_failure(resource)
    
    sleep(sync_interval)  # typically 10 seconds
```

Key design decisions:
- **Idempotent actions:** Every remediation action can be safely retried. "Drain node" checks if already drained before draining again.
- **Observe, don't assume:** After taking an action, the controller doesn't update actual state directly. It waits for the next observation loop to confirm the action took effect.
- **Sync period vs watch:** Use watches for low-latency detection, but always have a periodic full-sync as a fallback.

**Failure Modes:**
- **Controller crash:** Leader election promotes standby. Level-triggered means the new leader just reads current state and continues — no replay needed.
- **State store unavailable:** Controller continues using cached state; new remediations are paused but existing ones continue.
- **Split brain between controllers:** Sharding by cluster prevents overlap. If a shard reassignment happens, the new controller re-observes everything.

**Interviewer Q&As:**

**Q1: Why level-triggered over edge-triggered?**
A: Level-triggered is more resilient to failures. If we miss an event (network glitch, controller restart), the next sync cycle will observe the current state and still converge. Edge-triggered requires perfect event delivery — any missed event means permanent divergence unless you add a reconciliation fallback, which is just level-triggered with extra steps.

**Q2: How do you prevent two controllers from acting on the same node?**
A: Strict sharding — each cluster is assigned to exactly one controller instance via a lease in etcd. The controller must hold the lease to take actions. If it loses the lease (e.g., network partition), it immediately stops acting. A new controller acquires the lease and takes over.

**Q3: What if the reconciliation loop is too slow for a 10K-node cluster?**
A: Two optimizations: (1) Watch-based fast path — subscribe to state changes and reconcile immediately on change, (2) Parallel reconciliation — within a single loop iteration, reconcile independent nodes concurrently with a worker pool (e.g., 50 concurrent workers per controller).

**Q4: How do you handle cascading reconciliation (fixing node A triggers problems on node B)?**
A: The rate limiter bounds concurrency — e.g., max 5% of nodes in a cluster can be in remediation simultaneously. This prevents a scenario where draining multiple nodes overloads the remaining nodes. We also check cluster-wide health before starting new remediations.

**Q5: What happens when desired state is wrong (someone set desired=active for a broken node)?**
A: The reconciliation loop will keep trying and failing. After N failed attempts (configurable), it enters a backoff state and fires an alert for human review. The audit trail shows all attempts, making diagnosis easy.

**Q6: How do you test the reconciliation loop itself?**
A: Unit tests with mock state stores, integration tests with an in-memory etcd, and chaos tests that inject state store failures/delays. We also run shadow-mode controllers that compute actions but don't execute them, comparing shadow vs production actions.

---

### 6.2 Bare-Metal Self-Healing (IPMI/BMC + PXE)

**Why it's hard:** Bare-metal is the layer beneath Kubernetes — when the OS or runtime is broken, you can't rely on in-band agents. You need out-of-band management (IPMI/BMC) which has its own reliability issues: BMCs can hang, IPMI credentials can expire, and PXE reimaging can fail mid-way. Additionally, bare-metal reprovisioning takes minutes (not seconds like pod restart), so you must be much more cautious about false positives.

**Approaches Compared:**

| Approach | Detection Latency | False Positive Risk | Recovery Time | Scope |
|---|---|---|---|---|
| **IPMI watchdog timer** | ~60s (configurable) | Very low — hardware timer | Reboot: 2-5 min | Kernel hang only |
| **BMC sensor polling** | 30-60s | Medium — sensor noise | Depends on action | Hardware health |
| **Out-of-band health agent** (BMC-based) | 10-30s | Low | Reboot/reimage | OS + hardware |
| **In-band agent + OOB fallback** | 5-10s (in-band), 60s (OOB) | Low (combined) | Fast (in-band) / Slow (OOB) | All layers |

**Selected Approach: In-band agent with out-of-band fallback.**

**Justification:** The in-band node agent provides fast, rich health signals when the OS is healthy. When the in-band agent stops responding, we fall back to BMC/IPMI for out-of-band diagnosis and remediation. This gives us best-of-both-worlds: fast detection for common issues, reliable recovery for catastrophic failures.

**Implementation Detail:**

```
Bare-Metal Self-Healing State Machine:

  HEALTHY ──(in-band agent miss)──► SUSPECT
     ▲                                  │
     │                          (3 consecutive misses)
     │                                  │
     │                                  ▼
     │                          CONFIRMING_OOB
     │                                  │
     │                      (BMC health check)
     │                         /              \
     │                    (BMC OK,             (BMC confirms
     │                     agent bug)           failure)
     │                        │                    │
     │                        ▼                    ▼
     │                  AGENT_RESTART         FAILED
     │                        │                    │
     │                        │            (select remediation)
     │                        │              /     |      \
     │                        │         REBOOT  REIMAGE  REPLACE
     │                        │            │       │        │
     └────────────────────────┴────────────┴───────┘        │
                                                             ▼
                                                      DECOMMISSIONED
```

**IPMI Watchdog Configuration:**
```bash
# Set hardware watchdog to 120 seconds, action = power cycle
ipmitool mc watchdog set timer use 4 action 1 pretimeout 30 timeout 120
ipmitool mc watchdog reset  # must be reset every <120s by the OS agent
```

**PXE Reimaging Workflow:**
1. Cordon node in Kubernetes (prevent new pod scheduling).
2. Drain existing pods with configurable grace period (PodDisruptionBudget respected).
3. Set PXE boot order via IPMI: `ipmitool chassis bootdev pxe options=persistent`.
4. Trigger IPMI power cycle: `ipmitool power cycle`.
5. PXE server serves the golden OS image based on node's MAC address.
6. Post-boot provisioning script runs: installs kubelet, configures networking, joins cluster.
7. Health checks confirm node is healthy.
8. Uncordon node — scheduler can place pods again.

**Failure Modes:**
- **BMC unreachable:** Escalate to human — this node requires physical access. Alert with rack/row location.
- **PXE reimaging fails (boot loop):** After 3 PXE attempts, decommission node and alert for hardware investigation.
- **Partial reimaging (OS installed but kubelet won't start):** Post-boot health check catches this; trigger a second reimage with a different image version.
- **IPMI credential rotation:** BMC credentials are managed by a secrets manager with automatic rotation. If auth fails, the system retries with the previous credential before escalating.

**Interviewer Q&As:**

**Q1: Why not just use BMC for everything instead of an in-band agent?**
A: BMC/IPMI is limited in what it can detect — it sees hardware sensors and can check basic power state, but it can't detect application-level issues like a hung Java process or a full disk. The in-band agent provides much richer signals. BMC is the fallback when the in-band path is broken.

**Q2: How do you handle a rack-level BMC failure (e.g., management switch fails)?**
A: The system detects that multiple BMCs in the same rack become unreachable simultaneously. This triggers a rack-level alert (not per-node alerts), since the root cause is likely infrastructure, not the nodes themselves. We don't attempt per-node remediation — we escalate for network investigation.

**Q3: What about hosts with GPUs — how does reimaging differ?**
A: GPU hosts require additional post-provisioning steps: install NVIDIA drivers, configure GPU device plugin, run GPU health check (e.g., `nvidia-smi`, DCGM diagnostics). The reimaging workflow is parameterized by node type — the PXE server selects the appropriate image and post-boot script.

**Q4: How do you prevent a reimaging storm where many nodes get reimaged simultaneously?**
A: Per-rack and per-cluster rate limits. Default: max 1 node per rack, max 2% of cluster nodes can be reimaging simultaneously. If more nodes need reimaging, they queue and the system processes them in priority order (critical workload nodes first).

**Q5: What's the RPO for workloads on a node that gets reimaged?**
A: Stateless workloads (most pods): RPO = 0 because state is elsewhere. Pods with local storage (emptyDir, hostPath): data on the node is lost during reimaging. This is by design — pods using local storage must be designed for data loss. Stateful workloads use PVs backed by network storage (Ceph, EBS) which survive node reimaging.

**Q6: How do you ensure the "golden image" for PXE is itself healthy?**
A: The golden image goes through a CI/CD pipeline: build → automated testing (boot in a canary environment, run integration tests) → staged rollout (canary rack first, then 1%, 5%, 25%, 100%). We keep N-1 and N-2 versions available for rollback.

---

### 6.3 Cascading Failure Prevention

**Why it's hard:** When a self-healing system detects failures and starts remediating, the remediation itself can cause more failures. Example: a health check declares 10 nodes unhealthy, begins draining them simultaneously, the remaining nodes get overloaded, their health checks fail, and now we're draining even more nodes — a cascading failure caused by the self-healing system itself.

**Approaches Compared:**

| Approach | Effectiveness | Complexity | Latency Impact |
|---|---|---|---|
| **Global rate limiting** | High for bulk | Low | Delays remediation |
| **Circuit breaker on remediation** | High for cascading | Medium | Stops remediation entirely |
| **Cluster-health-aware gating** | Very high | High | Delays until safe |
| **Exponential backoff on remediation count** | Medium | Low | Slows progressively |

**Selected Approach: Cluster-health-aware gating with circuit breakers.**

**Justification:** Before starting any remediation, check the overall cluster health. If more than X% of nodes are already unhealthy or in remediation, pause new remediations and alert humans. This is the safest approach because it adapts to the actual situation rather than using static rate limits.

**Implementation:**

```python
class RemediationGatekeeper:
    def __init__(self, cluster_id):
        self.cluster_id = cluster_id
        self.max_concurrent_remediations_pct = 0.05  # 5% of cluster
        self.cluster_health_threshold = 0.80          # pause if <80% healthy
        self.circuit_breaker = CircuitBreaker(
            failure_threshold=5,    # 5 failed remediations
            reset_timeout_sec=300   # try again after 5 minutes
        )
    
    def allow_remediation(self, target_id, action_type):
        # Check circuit breaker
        if self.circuit_breaker.is_open():
            log.warn(f"Circuit breaker OPEN for {self.cluster_id}, skipping remediation")
            return False, "circuit_breaker_open"
        
        # Check cluster health
        cluster = get_cluster_state(self.cluster_id)
        healthy_pct = cluster.healthy_node_count / cluster.total_node_count
        if healthy_pct < self.cluster_health_threshold:
            log.warn(f"Cluster {self.cluster_id} health {healthy_pct:.0%} < threshold")
            alert_oncall(f"Cluster health below threshold, remediations paused")
            return False, "cluster_health_low"
        
        # Check concurrent remediation limit
        active = cluster.nodes_in_remediation_count
        max_active = int(cluster.total_node_count * self.max_concurrent_remediations_pct)
        if active >= max_active:
            return False, "rate_limit_exceeded"
        
        # Check per-rack limit (don't drain entire rack)
        rack = get_rack(target_id)
        if rack.nodes_in_remediation >= 1:
            return False, "rack_limit_exceeded"
        
        return True, "allowed"
```

**Failure Modes:**
- **False-positive health threshold:** If health data is stale, we might pause remediation unnecessarily. Mitigation: use real-time health from multiple sources.
- **Circuit breaker stuck open:** Timer-based reset ensures it eventually tries again. Operators can manually reset via CLI.

**Interviewer Q&As:**

**Q1: How do you differentiate between "the cluster is truly failing" and "we caused the failure"?**
A: Track the timeline. If cluster health dropped before we started remediating, the failure is organic. If it dropped after we started, our remediation may be contributing. In the latter case, the circuit breaker trips and we pause. We also correlate with external signals (e.g., network monitoring, power alerts) to determine root cause.

**Q2: What about load shedding — where does it fit?**
A: Load shedding is an application-level defense. If the infrastructure layer detects that a service is overloaded (high latency, connection pool exhaustion), it can instruct the load balancer to reduce traffic to that service. This is coordinated with the self-healing system — we won't drain a node if its replacement would increase load on already-stressed services.

**Q3: How do you handle the "thundering herd" after recovery?**
A: When a node comes back after remediation, we don't immediately uncordon it and let the scheduler flood it with pods. Instead, we use a warm-up period: gradually increase the node's schedulable capacity over 5 minutes, allowing caches to warm and connections to establish.

**Q4: What if the self-healing system itself needs healing?**
A: The self-healing control plane is deployed as a highly available service across 3 AZs with its own independent health monitoring (a simple watchdog, not the full system). If the control plane goes down, Kubernetes's built-in self-healing (kubelet restarts, ReplicaSet controllers) still functions — our system is an enhancement, not a replacement.

**Q5: Can an operator override the circuit breaker in an emergency?**
A: Yes. The CLI supports `shctl circuit-breaker reset --cluster=prod-us-east-1a --force --reason="operator override per incident INC-12345"`. The override is logged in the audit trail. There's also a `--bypass-all-safety` flag that requires two-person approval (second operator must confirm).

**Q6: How do you tune the thresholds (5% concurrent, 80% healthy)?**
A: Start conservative (low concurrency, high health threshold). Use historical data to analyze: "If we had used X% threshold, how many incidents would have been caught vs delayed?" Gradually relax thresholds as confidence grows. Different cluster tiers (production vs staging) can have different thresholds.

---

### 6.4 Node Problem Detector and Health Signal Aggregation

**Why it's hard:** Health signals come from many sources (kubelet probes, NPD, BMC sensors, application sidecars), at different frequencies, with different reliability levels. Aggregating these into a single authoritative health score that minimizes both false positives and false negatives is a signal processing challenge.

**Approaches Compared:**

| Approach | False Positive Rate | False Negative Rate | Complexity | Latency |
|---|---|---|---|---|
| **Any-check-fails = unhealthy** | High | Very low | Low | Fast |
| **Majority voting** | Medium | Medium | Medium | Medium |
| **Weighted composite score** | Low | Low | High | Medium |
| **ML-based anomaly detection** | Very low | Low | Very high | Higher |

**Selected Approach: Weighted composite health score with configurable thresholds.**

**Justification:** Different health signals have different reliability and importance. A failed liveness probe is more authoritative than a CPU temperature warning. Weighted scoring lets us combine signals intelligently while remaining interpretable (unlike ML black boxes).

**Implementation:**

```python
class HealthScoreCalculator:
    WEIGHTS = {
        'liveness_probe':       0.30,  # Most authoritative — process is responding
        'readiness_probe':      0.15,  # Service is ready to serve
        'node_condition':       0.20,  # Kubelet reports (MemoryPressure, DiskPressure, etc.)
        'bmc_hardware_health':  0.15,  # Hardware sensors (temp, ECC, SMART)
        'network_connectivity': 0.10,  # Can reach control plane, peers
        'kernel_log_analysis':  0.10,  # NPD parsing dmesg for errors
    }
    
    def calculate(self, node_id: str) -> float:
        score = 0.0
        for signal_type, weight in self.WEIGHTS.items():
            signal = get_latest_signal(node_id, signal_type)
            if signal is None or signal.age > MAX_SIGNAL_AGE:
                # Stale/missing signal contributes 50% (uncertain)
                score += weight * 0.5
            else:
                score += weight * signal.health_value  # 0.0 (dead) to 1.0 (healthy)
        return score * 100  # 0-100 scale
    
    # Thresholds
    # >= 80: HEALTHY — no action
    # 60-79: DEGRADED — alert, consider remediation
    # 40-59: UNHEALTHY — auto-remediate
    # < 40:  CRITICAL — immediate remediation
```

**Flap Detection:**
```python
class FlapDetector:
    """Prevent oscillation between healthy/unhealthy states."""
    
    def __init__(self, window_sec=300, max_transitions=4):
        self.window_sec = window_sec      # 5-minute window
        self.max_transitions = max_transitions
    
    def is_flapping(self, node_id: str) -> bool:
        transitions = count_state_transitions(node_id, last_n_seconds=self.window_sec)
        if transitions >= self.max_transitions:
            # Node is oscillating — hold current state, alert for investigation
            return True
        return False
```

**Node Problem Detector integration:**

NPD runs as a DaemonSet on every node, monitoring:
- **Kernel issues:** OOM kills, hung tasks, filesystem errors from `dmesg` / `journalctl`.
- **Hardware issues:** MCE (Machine Check Exception), ECC memory errors via `edac-utils`.
- **Runtime issues:** Docker/containerd daemon health, kubelet health.
- **Custom conditions:** Operator-defined scripts that return node conditions.

NPD reports via NodeConditions:
```yaml
conditions:
  - type: KernelDeadlock
    status: "False"
    reason: KernelHasNoDeadlock
  - type: ReadonlyFilesystem
    status: "True"
    reason: FilesystemIsReadOnly
    message: "/dev/sda1 mounted read-only after I/O errors"
```

**Interviewer Q&As:**

**Q1: How do you handle a health check that's always flaky (e.g., 10% failure rate)?**
A: First, the flap detector prevents it from triggering remediation. Second, we track per-check failure rates over time. If a check has > 5% false positive rate, we flag it for review and reduce its weight in the composite score. The health check registry includes a "reliability_score" field that auto-adjusts based on historical accuracy.

**Q2: What if a new health check is added that's too aggressive and starts marking healthy nodes as unhealthy?**
A: New health checks are deployed in "observe-only" mode for 7 days. They report results but don't affect the composite health score. During this burn-in period, we validate their accuracy against known failures. Only after validation are they promoted to "active" mode.

**Q3: How do you correlate multiple signals to identify root cause?**
A: Temporal correlation — if a disk I/O error (from NPD kernel logs) appears 30 seconds before liveness probes start failing, the root cause is likely disk, not the application. We build a causal graph: hardware signals → OS signals → runtime signals → application signals. Root cause attribution follows this graph.

**Q4: What's the health score of a brand-new node that just joined the cluster?**
A: New nodes start with a health score of 50 (neutral/uncertain) and a "startup" grace period of 5 minutes. During this period, missing signals are expected and don't count as negative. The score adjusts toward 100 as positive signals arrive.

**Q5: How do you handle a split-brain where the health checker says a node is down but the node is actually serving traffic fine?**
A: Cross-validate with multiple sources. If the health checker says unhealthy but the load balancer shows active connections with low error rates, we classify this as a health check issue, not a node issue. We alert the health check owner rather than remediating the node.

**Q6: Can teams customize health check weights for their services?**
A: Yes. The default weights are global, but teams can override weights per service via annotations. For example, a team running a stateless web server might weight readiness_probe at 0.50 and bmc_hardware_health at 0.05, because application health matters more than hardware margins for their workload.

---

## 7. Scheduling & Resource Management

### Remediation Scheduling

Remediations are not instant — they compete for limited resources (drain capacity, BMC bandwidth, PXE server capacity). The remediation scheduler handles this.

**Priority Queue:**
```
Priority 1 (CRITICAL): Data-loss risk — e.g., node with sole replica of a PV
Priority 2 (HIGH):     Production workload impact — node running tier-1 services
Priority 3 (MEDIUM):   Degraded but functional — node with ECC errors, high temp
Priority 4 (LOW):      Preventive — node approaching disk-full threshold
```

**Resource Constraints:**
| Resource | Limit | Reason |
|---|---|---|
| Concurrent node drains per cluster | 5% of nodes | Prevent capacity starvation |
| Concurrent reimages per rack | 1 | Shared PXE/TFTP server per rack |
| Concurrent BMC operations per management switch | 10 | BMC management network bandwidth |
| Total concurrent remediations (global) | 500 | Control plane capacity |

**Scheduling Algorithm:**
1. Dequeue highest-priority remediation.
2. Check all resource constraints (cluster, rack, global).
3. If all constraints satisfied, execute. If not, re-enqueue with a backoff.
4. After execution, release resource tokens.

### Pod Restart Policies

| Policy | Behavior | Use Case |
|---|---|---|
| `Always` | Restart container regardless of exit code | Long-running services (web servers, daemons) |
| `OnFailure` | Restart only on non-zero exit code | Batch jobs (retry on failure, stop on success) |
| `Never` | Never restart | One-shot diagnostic pods |

**Backoff Behavior:** Kubernetes uses exponential backoff for restarts: 10s, 20s, 40s, 80s, ..., capped at 5 minutes. After 5 minutes of continuous CrashLoopBackOff, the pod enters a 5-minute cooldown between restart attempts.

### Kubernetes Probes Configuration Best Practices

```yaml
# Example: Java service with slow startup
livenessProbe:
  httpGet:
    path: /healthz
    port: 8080
  initialDelaySeconds: 0      # startup probe handles grace period
  periodSeconds: 10
  timeoutSeconds: 5
  failureThreshold: 3          # 30s to declare dead

readinessProbe:
  httpGet:
    path: /ready
    port: 8080
  periodSeconds: 5
  timeoutSeconds: 3
  failureThreshold: 2          # 10s to remove from LB
  successThreshold: 2          # 10s to re-add to LB

startupProbe:
  httpGet:
    path: /healthz
    port: 8080
  periodSeconds: 5
  failureThreshold: 60         # 5min startup grace period
  # liveness probe does NOT run until startup probe succeeds
```

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Strategy | Scale Unit |
|---|---|---|
| Health Check Aggregator | 1 instance per region, sharded by AZ internally | Add instances per AZ |
| Failure Detector | 3-node consensus group per region | Increase group size for throughput |
| Remediation Engine | Partitioned by cluster | 1 engine per cluster, scale with cluster count |
| Runbook Executor | Stateless worker pool behind a queue | Add workers as remediation volume grows |
| Node Agent (bare-metal) | DaemonSet — 1 per node | Scales with fleet |
| NPD | DaemonSet — 1 per node | Scales with fleet |

**Scaling the Health Check Pipeline:**

At 430K health check events/sec, a single Kafka consumer group cannot keep up. Strategy:
- Kafka topic `health-events` with 256 partitions, keyed by `target_id`.
- Consumer group with 64 aggregator instances, each processing 4 partitions.
- Aggregators are stateless — per-target state is in a local cache (rebuilt from Kafka on restart).

### Database Scaling

**MySQL:**
- Vertical scaling first (64-core, 512GB RAM handles metadata load).
- Read replicas for reporting/dashboard queries.
- `health_check_results` table is partitioned by day; old partitions are dropped (not deleted row-by-row).
- If metadata grows beyond single-instance capacity, shard by `cluster_id` using Vitess.

**Elasticsearch:**
- Time-based indices with ILM: hot (7d, SSD), warm (90d, HDD), cold (2y, S3-backed).
- Hot tier: 3 replicas for availability. Warm: 1 replica. Cold: 0 replicas (restored from S3 on demand).
- Index rollover at 50GB or 1 day, whichever comes first.

**etcd:**
- etcd handles the desired/actual state store (~4.5 GB, well within limits).
- 5-node etcd cluster for quorum safety.
- Compaction every 5 minutes to control revision history.

### Caching

| Layer | Technology | What's Cached | TTL | Hit Rate |
|---|---|---|---|---|
| L1: In-process | JVM/Python dict | Per-target latest health score | 10s | ~90% |
| L2: Distributed | Redis Cluster | Aggregated health scores, cooldown state | 30s | ~95% |
| L3: Database | MySQL query cache (disabled — use app cache) | N/A | N/A | N/A |
| L4: CDN | N/A (internal service) | N/A | N/A | N/A |

### Interviewer Q&As

**Q1: How do you handle the "noisy neighbor" problem in health check processing — one cluster generating disproportionate events?**
A: Kafka partitioning by `target_id` naturally distributes load. For extreme cases, we use per-cluster rate limiting on the producer side (the node agent). If a cluster is generating >10x expected events, we throttle it and alert the cluster operator.

**Q2: What happens when a Kafka partition gets stuck (consumer lag growing)?**
A: We monitor consumer lag with Burrow. If lag exceeds 60 seconds, we alert. If lag exceeds 5 minutes, we add more consumer instances and rebalance partitions. Health check events older than 5 minutes are dropped as stale — we don't remediate based on old data.

**Q3: How do you handle the etcd size limit?**
A: etcd is only for desired/actual state (key-value pairs per node), not for event history. At 100K nodes x 50 bytes per key, we're at ~5 MB of data — well within etcd's 8 GB limit. We run compaction aggressively (every 5 minutes, retain 1000 revisions) to prevent unbounded growth.

**Q4: Why Redis instead of Memcached for L2 cache?**
A: Redis supports data structures (sorted sets for priority queues, hashes for node state) that we use for the cooldown timer and remediation state. Memcached is pure key-value with no data structure support. Redis Cluster gives us horizontal scaling with automatic sharding.

**Q5: How do you handle cache stampede when many targets' health scores expire simultaneously?**
A: We use jittered TTLs — each cache entry's TTL is the base TTL +/- 20% random jitter. This spreads expiration across time. We also use "stale-while-revalidate" — serve the slightly-stale cached value while asynchronously refreshing from the source.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Automated Recovery |
|---|---|---|---|
| Single node failure | Pods rescheduled, no service impact (if replicated) | Health check timeout in 30s | Drain + cordon + reboot/reimage |
| Node agent crash | Lose in-band health signals for that node | Missing heartbeat detected by aggregator | Restart agent via systemd; fall back to BMC |
| Health Check Aggregator down | Health events queue in Kafka | Consumer lag alert | Standby aggregator takes over (consumer group rebalance) |
| Failure Detector down | No new failures confirmed | Liveness probe restart | Leader election promotes replica |
| Remediation Engine down | Queued remediations stall | Liveness probe restart + queue depth alert | Standby engine acquires lease |
| etcd quorum loss | State store unavailable | etcd health check | If 1 node: auto-heal. If majority: human intervention |
| Kafka broker failure | Event pipeline degraded | Under-replicated partitions alert | Kafka auto-reassigns partitions to healthy brokers |
| MySQL primary failure | Writes fail | ProxySQL health check | MySQL Orchestrator promotes replica |
| Management network failure | BMC/IPMI unreachable | BMC ping failures from management host | Alert network team; in-band agent continues |
| Full AZ failure | 33% of regional capacity lost | Multiple node failures in same AZ | Cross-AZ rescheduling; don't try to heal the AZ |
| Self-healing causes cascading failure | Healthy nodes get overloaded | Cluster health threshold breach | Circuit breaker trips; pause all remediations |
| PXE server failure | Cannot reimage hosts | PXE health check | Failover to secondary PXE server in same AZ |

### Automated Recovery

**Tiered Recovery:**
```
Tier 1 (Seconds):   Pod restart, process restart
Tier 2 (Minutes):   Node drain + reboot
Tier 3 (Minutes):   Node reimage via PXE
Tier 4 (Minutes):   Node replacement (provision new bare-metal)
Tier 5 (Human):     Escalate — requires physical intervention
```

**Each tier has:**
- Pre-conditions: "Is the target still unhealthy? Is cooldown expired? Is rate limit OK?"
- Post-conditions: "Did the target become healthy within expected time?"
- Rollback: "If post-condition fails, escalate to next tier."

### Retry Strategy

| Component | Retry Strategy |
|---|---|
| Health check execution | No retry — next check runs on schedule |
| Failure confirmation | 3 consecutive failures before confirming |
| Remediation action | Retry with exponential backoff: 10s, 30s, 90s, then escalate |
| BMC/IPMI command | 3 retries with 5s delay (BMC can be slow) |
| PXE boot | 3 attempts, then mark node for hardware investigation |
| Kafka producer | Retry with backoff, idempotent producer enabled |
| MySQL write | Retry on deadlock (up to 3 times), fail on other errors |

### Circuit Breaker

```
Circuit Breaker States:
  CLOSED ─── (5 failed remediations in 5 min) ──► OPEN
    ▲                                                 │
    │                                          (5 min timeout)
    │                                                 │
    │                                                 ▼
    └──── (successful remediation) ◄──── HALF-OPEN
                                    (allow 1 remediation)
```

**Per-cluster circuit breaker thresholds:**
- Trip: 5 consecutive failed remediations.
- Reset timeout: 5 minutes.
- Half-open: Allow 1 remediation as a probe.
- Metrics: Track trip count, time spent open, remediations blocked.

### Consensus & Coordination

**Leader Election (etcd-based):**
- Remediation Engine instances compete for a per-cluster lease.
- Lease TTL: 15 seconds, renewed every 5 seconds.
- If lease is lost, the instance stops all remediations immediately.
- New leader observes current state (level-triggered — no replay needed).

**Distributed Locking (for node operations):**
- Before remediating a node, acquire a lock: `/locks/remediation/{node_id}`.
- Lock TTL: 30 minutes (maximum remediation duration).
- Prevents two systems from simultaneously draining the same node.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `health_check_execution_latency_ms` | Histogram | P99 > 5000ms |
| `health_check_failure_rate` | Gauge per check | > 5% (potential flaky check) |
| `failure_detection_latency_sec` | Histogram | P99 > 60s |
| `remediation_queue_depth` | Gauge | > 100 pending |
| `remediation_success_rate` | Gauge | < 90% |
| `remediation_duration_sec` | Histogram | P99 > 30min (node-level) |
| `concurrent_remediations` | Gauge per cluster | > 5% of cluster size |
| `cluster_health_score` | Gauge per cluster | < 80% |
| `circuit_breaker_state` | Gauge per cluster | State = OPEN |
| `etcd_leader_changes` | Counter | > 3 in 1 hour |
| `kafka_consumer_lag_sec` | Gauge per partition | > 60s |
| `pxe_reimage_duration_sec` | Histogram | P99 > 20min |
| `false_positive_rate` | Gauge | > 0.1% |
| `nodes_in_crashloop_remediation` | Gauge | > 0 (remediation stuck) |

### Distributed Tracing

Every remediation action generates a trace spanning:
1. Health check event → Kafka → Aggregator → Failure Detector → Remediation Engine → Executor.
2. Trace ID is propagated through Kafka message headers.
3. Each remediation step is a span: drain, cordon, reboot, health-verify, uncordon.
4. Traces are exported to Jaeger with 100% sampling for remediations (they're infrequent enough).

**Example Trace:**
```
[health-check-agent]  ──► [kafka]  ──► [aggregator]  ──► [failure-detector]
   5ms                      2ms          15ms                10ms
                                                               │
[remediation-engine] ◄─────────────────────────────────────────┘
   50ms (decision)
       │
       ├── [drain-node]     120s
       ├── [cordon-node]      2s
       ├── [ipmi-reboot]     90s
       ├── [wait-healthy]   180s
       └── [uncordon-node]    2s
                          ────────
                          Total: ~6.5 min
```

### Logging

**Structured logging (JSON) with standard fields:**
```json
{
  "timestamp": "2026-04-09T10:30:15.123Z",
  "level": "INFO",
  "service": "remediation-engine",
  "cluster_id": "prod-us-east-1a",
  "node_id": "node-42",
  "failure_id": "fail-xyz",
  "action_id": "rem-123",
  "action_type": "drain_node",
  "message": "Starting node drain",
  "trace_id": "abc123",
  "span_id": "def456"
}
```

**Log levels:**
- ERROR: Remediation failed, circuit breaker tripped, state store unreachable.
- WARN: Rate limit hit, cooldown active, health check flapping.
- INFO: Remediation started/completed, failure detected/confirmed.
- DEBUG: Individual health check results, reconciliation loop iterations.

**Log pipeline:** Application → Fluentd DaemonSet → Kafka → Elasticsearch → Kibana.

### Alerting

| Alert | Severity | Routing |
|---|---|---|
| Cluster health < 80% | P1 (page) | On-call SRE |
| Circuit breaker OPEN | P1 (page) | On-call SRE |
| Remediation queue depth > 100 | P2 (urgent) | Infra team Slack |
| Remediation success rate < 90% | P2 (urgent) | Infra team Slack |
| etcd leader election storm | P2 (urgent) | Platform team |
| Kafka consumer lag > 5 min | P2 (urgent) | Platform team |
| False positive rate > 0.1% | P3 (ticket) | Health check owners |
| PXE server unhealthy | P2 (urgent) | Bare-metal team |

---

## 11. Security

### Auth & AuthZ

**Control Plane Authentication:**
- All inter-service communication uses mTLS with certificates issued by an internal CA (e.g., Vault PKI).
- BMC/IPMI credentials stored in HashiCorp Vault with automatic rotation (90-day cycle).
- Kafka uses SASL/SCRAM authentication + TLS encryption.
- etcd uses client certificates for authentication.

**Authorization (RBAC):**
| Role | Permissions |
|---|---|
| `self-healing-controller` | Read/write health state, execute Tier 1-3 remediations |
| `bmc-operator` | Execute IPMI commands (power cycle, sensor read, PXE boot) |
| `sre-oncall` | Approve high-impact remediations, override circuit breakers |
| `platform-admin` | Full access, modify health check registry, tune thresholds |
| `read-only` | View health state, failures, remediation history |

**Principle of least privilege:** The remediation engine runs with a service account that can only drain/cordon/uncordon nodes. It cannot delete namespaces, modify RBAC, or access secrets.

### Audit Logging

Every automated action is logged with:
```json
{
  "timestamp": "2026-04-09T10:30:15Z",
  "actor": "remediation-engine/prod-us-east-1a",
  "actor_type": "service",
  "action": "drain_node",
  "target": "node-42",
  "target_type": "bare_metal_host",
  "failure_id": "fail-xyz",
  "runbook_id": "rb-node-drain-v3",
  "decision_reason": "health_score=25, below threshold=40",
  "dry_run": false,
  "approval": {
    "required": false,
    "approved_by": null
  },
  "result": "success",
  "duration_ms": 120000,
  "changes_made": [
    "cordoned node-42",
    "evicted 23 pods",
    "initiated IPMI reboot"
  ]
}
```

**Retention:** 2 years in cold storage (S3/GCS), 90 days searchable in Elasticsearch, 7 days hot in MySQL.

**Compliance:** Audit logs are append-only. The self-healing service account cannot delete audit entries. Audit log integrity is verified with periodic checksums.

---

## 12. Incremental Rollout Strategy

**Phase 1 (Week 1-4): Observe Only**
- Deploy health check aggregator and failure detector in all regions.
- All remediations are logged but NOT executed (dry-run mode).
- Validate detection accuracy: compare detected failures against actual incidents.
- Target: < 1% false positive rate.

**Phase 2 (Week 5-8): Pod-Level Self-Healing**
- Enable automated pod restarts for non-critical services.
- Gradual rollout: 1 cluster → 5 clusters → all staging → 1 production cluster → all.
- Monitor restart rates, success rates, impact on SLOs.

**Phase 3 (Week 9-12): Node-Level Self-Healing (Kubernetes)**
- Enable node drain + cordon + reboot for confirmed hardware failures.
- Start with non-GPU nodes in staging, expand to production.
- Conservative thresholds: high failure_threshold (5 consecutive failures), long cooldown (30 min).

**Phase 4 (Week 13-16): Bare-Metal Self-Healing**
- Enable IPMI reboot and PXE reimaging for confirmed OS/hardware failures.
- Start with 1 rack per AZ, expand gradually.
- Requires approval gate for PXE reimaging initially; remove after 30 days of success.

**Phase 5 (Week 17-20): Full Automation**
- Remove approval gates for routine remediations.
- Enable cascading failure prevention (circuit breakers).
- Run GameDay exercises to validate end-to-end.

### Rollout Q&As

**Q1: How do you validate that dry-run mode accurately predicts what would happen in live mode?**
A: In dry-run, we log the exact API calls that would be made (e.g., `kubectl drain node-42 --grace-period=30`). We periodically compare dry-run logs against actual manual remediations to verify they would have taken the same actions. We also run a shadow environment where dry-run actions are executed against a non-production cluster.

**Q2: What's the rollback plan if automated remediation causes an incident?**
A: Emergency kill switch: `shctl self-healing disable --scope=global` immediately stops all automated remediations across all clusters. The system reverts to observe-only mode. All in-progress remediations are allowed to complete (to avoid leaving nodes in a half-drained state), but no new ones start.

**Q3: How do you handle the transition from human-driven to automated remediation?**
A: We run both in parallel during Phase 2-4. Automated remediation proposes an action but initially requires human approval (via Slack). Over time, we measure approval rates. If >95% of proposals are approved without modification, we remove the approval gate for that action type.

**Q4: How do you measure the ROI of self-healing?**
A: Key metrics: (1) MTTR (Mean Time To Repair) — should decrease from hours to minutes. (2) Toil reduction — fewer pages for known failure types. (3) Incident count — fewer escalations for infrastructure issues. (4) Human hours saved — track time-to-acknowledge vs time-to-auto-remediate.

**Q5: What if a specific node type (e.g., GPU nodes) has different failure patterns?**
A: Health check profiles are parameterized by node type. GPU nodes have additional checks (nvidia-smi, DCGM), different thresholds (GPU memory ECC errors are critical), and different remediation paths (GPU driver reinstall before full reimage). Each node type has its own runbook set.

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Choice | Rationale | Risk |
|---|---|---|---|---|
| Reconciliation model | Event-driven vs level-triggered | Level-triggered | Idempotent, resilient to missed events, proven at scale (Kubernetes) | Slightly higher latency than pure event-driven |
| State store | etcd vs ZooKeeper vs CockroachDB | etcd | Native Kubernetes integration, watch semantics, strong consistency | 8 GB data limit (acceptable for state data) |
| Health check transport | Push (agent → aggregator) vs Pull (aggregator → agent) | Push via Kafka | Decouples producer/consumer, handles backpressure, durable | Kafka operational overhead |
| Failure detection | Single-checker vs consensus | Consensus (N-of-M) | Reduces false positives at cost of detection latency | 10-20s added latency for consensus |
| Remediation execution | Centralized vs distributed | Centralized per cluster | Easier rate limiting and blast radius control | Cluster controller is SPOF (mitigated by leader election) |
| Bare-metal fallback | Pure BMC vs in-band + BMC | In-band + BMC fallback | Fast detection via in-band, reliable recovery via BMC | Dual system complexity |
| Health scoring | Binary (up/down) vs composite score | Weighted composite score | Richer signal, fewer false positives, tunable | Complexity in weight tuning |
| Cascading prevention | Static rate limit vs dynamic health-aware | Health-aware gating + circuit breaker | Adapts to actual cluster state, not just static limits | Requires accurate real-time health data |
| Audit store | MySQL only vs MySQL + ES | MySQL + Elasticsearch | MySQL for structured queries, ES for full-text search | Two systems to maintain |
| Rollout strategy | Big bang vs incremental | Incremental with dry-run first | De-risks automation, builds operator confidence | Slower time to full automation |

---

## 14. Agentic AI Integration

### AI-Powered Failure Classification

Traditional self-healing uses static rules: "if health score < 40, drain node." Agentic AI can improve this by learning failure patterns and selecting optimal remediation strategies.

**Architecture:**

```
┌─────────────────────────────────────────────────┐
│              AI REMEDIATION ADVISOR              │
│                                                   │
│  ┌─────────────┐  ┌────────────┐  ┌───────────┐ │
│  │  Failure     │  │  Runbook   │  │  Impact   │ │
│  │  Classifier  │  │  Selector  │  │  Predictor│ │
│  │  (LLM/ML)   │  │  (RAG)     │  │  (ML)     │ │
│  └──────┬──────┘  └─────┬──────┘  └─────┬─────┘ │
│         │                │                │       │
│  ┌──────▼────────────────▼────────────────▼─────┐ │
│  │          Decision Engine (rules + AI)         │ │
│  └──────────────────────┬───────────────────────┘ │
└─────────────────────────┼─────────────────────────┘
                          │
                          ▼
              Remediation Engine (existing)
```

**1. Failure Classifier (LLM-based):**

Given a set of health signals, kernel logs, and application logs, classify the failure root cause.

```python
class AIFailureClassifier:
    def classify(self, failure_context: FailureContext) -> Classification:
        prompt = f"""
        You are an infrastructure failure analysis system.
        
        Node: {failure_context.node_id}
        Health signals: {failure_context.health_signals}
        Recent kernel logs: {failure_context.kernel_logs[-50:]}
        Recent application logs: {failure_context.app_logs[-50:]}
        Hardware sensors: {failure_context.bmc_sensors}
        
        Classify this failure into ONE of:
        - HARDWARE_DISK: Disk failure (SMART errors, I/O errors)
        - HARDWARE_MEMORY: Memory failure (ECC uncorrectable)
        - HARDWARE_NIC: Network interface failure
        - OS_KERNEL: Kernel bug/panic
        - OS_OOM: Out of memory
        - OS_FILESYSTEM: Filesystem corruption
        - RUNTIME_DOCKER: Container runtime failure
        - RUNTIME_KUBELET: Kubelet failure
        - APP_CRASH: Application crash loop
        - APP_DEADLOCK: Application deadlock/hang
        - NETWORK_PARTITION: Network connectivity lost
        - RESOURCE_EXHAUSTION: CPU/memory/disk pressure
        - UNKNOWN: Cannot determine
        
        Provide: classification, confidence (0-1), evidence, recommended_action
        """
        response = self.llm.invoke(prompt, temperature=0.1)
        return parse_classification(response)
```

**Why LLM over traditional ML for classification:**
- Traditional ML requires feature engineering and labeled training data. Kernel log messages are unstructured and vary across OS versions.
- LLMs can reason over unstructured log text, correlate multiple signals, and explain their reasoning.
- Confidence scores allow fallback to rule-based classification when LLM is uncertain.

**2. Runbook Selector (RAG-based):**

Given a classified failure, retrieve the most relevant runbook from the runbook repository.

```python
class AIRunbookSelector:
    def __init__(self):
        self.vector_store = ChromaDB("runbooks")  # Embedded runbook corpus
    
    def select(self, classification: Classification, context: FailureContext) -> Runbook:
        # Retrieve top-5 candidate runbooks via semantic similarity
        query = f"{classification.category}: {classification.evidence}"
        candidates = self.vector_store.similarity_search(query, k=5)
        
        # Use LLM to select best match considering full context
        prompt = f"""
        Failure: {classification.category} on {context.node_id}
        Evidence: {classification.evidence}
        Node type: {context.node_type}
        Cluster: {context.cluster_id}
        
        Candidate runbooks:
        {format_candidates(candidates)}
        
        Select the most appropriate runbook. Consider:
        1. Does the runbook match the failure type?
        2. Is it compatible with the node type (GPU, storage, compute)?
        3. What is the blast radius?
        4. Has this runbook been successful for similar failures before?
        
        Return: selected_runbook_id, confidence, reasoning
        """
        return self.llm.invoke(prompt, temperature=0.1)
```

**3. Impact Predictor (ML model):**

Before executing a remediation, predict its impact on the cluster.

```python
class ImpactPredictor:
    """Predicts the impact of a remediation action on cluster health and SLOs."""
    
    def predict(self, action: RemediationAction, cluster_state: ClusterState) -> ImpactPrediction:
        features = {
            'cluster_utilization': cluster_state.cpu_utilization,
            'nodes_in_remediation': cluster_state.nodes_in_remediation,
            'target_node_pod_count': action.target.pod_count,
            'target_node_pv_count': action.target.persistent_volume_count,
            'time_of_day': current_hour(),  # failures during peak have higher impact
            'action_type_encoded': encode_action(action.type),
            'node_type_encoded': encode_node_type(action.target.type),
        }
        
        # XGBoost model trained on historical remediation outcomes
        impact_score = self.model.predict(features)  # 0 (no impact) to 1 (severe)
        
        if impact_score > 0.7:
            return ImpactPrediction(
                score=impact_score,
                recommendation="REQUIRE_APPROVAL",
                reason="High predicted impact on cluster SLOs"
            )
        return ImpactPrediction(score=impact_score, recommendation="AUTO_APPROVE")
```

### Safety Guardrails for AI in Self-Healing

| Guardrail | Implementation |
|---|---|
| **Confidence threshold** | AI classification must have >0.8 confidence; below that, fall back to rule-based |
| **Human-in-the-loop** | AI-selected runbooks for critical systems require human approval for 90 days |
| **Shadow mode** | AI recommendations are logged but not executed for the first 30 days |
| **Blast radius limit** | AI cannot recommend actions affecting >N nodes simultaneously |
| **Rollback** | AI-initiated remediations are tagged and can be bulk-rolled-back |
| **Audit** | Every AI decision includes the prompt, response, confidence, and reasoning |
| **Fallback** | If AI service is unavailable, system falls back to rule-based remediation |
| **Drift detection** | Monitor AI accuracy weekly; retrain if accuracy drops below 90% |

### Agentic AI for Autonomous Incident Response

Beyond classification and runbook selection, an AI agent can orchestrate multi-step incident response:

```
Agent Goal: "Restore node-42 to healthy state"

Agent Actions:
1. [OBSERVE] Query health check history for node-42
2. [OBSERVE] Query kernel logs from node-42 via BMC serial-over-LAN
3. [THINK]   Kernel log shows "EXT4-fs error (device sda1): I/O failure"
             → Root cause: disk I/O failure
4. [THINK]   Node has 15 pods, 2 with PVs. Need to migrate PVs first.
5. [ACT]     Cordon node-42 (prevent new scheduling)
6. [ACT]     Migrate PV-backed pods to other nodes
7. [ACT]     Drain remaining pods with 30s grace period
8. [OBSERVE] Verify all pods rescheduled successfully
9. [ACT]     IPMI power cycle node-42
10. [OBSERVE] Wait for node to come back (timeout: 5 min)
11. [THINK]   Node came back but disk errors persist
12. [ACT]     Reimage node-42 via PXE (new disk image)
13. [OBSERVE] Wait for reimage completion (timeout: 15 min)
14. [ACT]     Run post-boot health checks
15. [OBSERVE] All checks pass
16. [ACT]     Uncordon node-42
17. [REPORT]  "Node-42 recovered. Root cause: disk I/O failure on sda1.
              Action: reimaged. Duration: 22 min. 15 pods rescheduled,
              2 PV migrations, 0 data loss."
```

**Key Properties of the Agent:**
- **Observe-Think-Act loop:** The agent alternates between gathering information and taking action.
- **Bounded autonomy:** The agent can only execute pre-approved action types. It cannot SSH into a node and run arbitrary commands.
- **Timeout and abort:** Each action has a timeout. If the agent gets stuck, it escalates to humans.
- **Explainability:** Every decision is logged with reasoning, making post-incident review straightforward.

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through what happens when a node's disk fails.**
A: The failure is detected through multiple signals: (1) NPD on the node detects I/O errors in dmesg and sets NodeCondition ReadonlyFilesystem=True. (2) Liveness probes for pods using local disk start failing. (3) kubelet reports DiskPressure condition. The health check aggregator combines these into a composite score below the critical threshold. The failure detector confirms via consensus (multiple signal sources agree). The remediation engine checks rate limits and cooldowns, then executes the node drain runbook: cordon → evacuate pods (respecting PDBs) → IPMI reboot → verify. If disk errors persist after reboot, escalate to PXE reimage. If reimage fails, decommission the node and alert hardware team for disk replacement.

**Q2: How do you handle a situation where 20% of nodes in a cluster simultaneously report as unhealthy?**
A: This triggers the cascading failure prevention system. When cluster health drops below 80%, the health-aware gate pauses all automated remediations and fires a P1 alert. The reason: 20% simultaneous failure is almost certainly not 20 independent failures — it's likely a common root cause (network issue, control plane problem, bad config push). Auto-remediating would likely make it worse. The system enters observe-only mode while humans investigate the root cause.

**Q3: What's the difference between liveness, readiness, and startup probes, and when would you use each?**
A: **Liveness:** "Is the process alive?" If it fails, kubelet restarts the container. Use for detecting deadlocks or hung processes. **Readiness:** "Can this instance serve traffic?" If it fails, the pod is removed from the Service endpoint list (no traffic routed). Use for startup warmup or temporary overload. **Startup:** "Has initial startup completed?" Until it succeeds, liveness and readiness probes are not checked. Use for slow-starting applications (Java with large heap, ML model loading). A common mistake is making the liveness probe check dependencies — that can cause cascading restarts. Liveness should only check "is my process alive," not "can I reach my database."

**Q4: How do you prevent a "restart storm" where pods keep crashing and restarting?**
A: Kubernetes has built-in CrashLoopBackOff: after repeated failures, the restart interval increases exponentially (10s, 20s, 40s, ... up to 5 min). Our self-healing layer adds additional protection: if a pod has restarted >10 times in an hour, we stop restarting and alert the owning team — the issue is likely a code bug, not a transient failure. We also track "restart velocity" per deployment — if multiple pods in the same deployment are crash-looping, we flag the deployment for rollback rather than restarting individual pods.

**Q5: How would you design the system to be self-healing for the self-healing system itself?**
A: The self-healing control plane is deployed as a Kubernetes Deployment with 3 replicas across 3 AZs, with PodDisruptionBudget minAvailable=2. It uses standard Kubernetes self-healing (kubelet restarts crashed pods, ReplicaSet replaces deleted pods). For the critical state store (etcd), we run a 5-node cluster. An independent watchdog (cron job) checks whether the self-healing system is functioning — it injects a synthetic failure and verifies the system detects it within 60 seconds. If the watchdog's check fails, it pages the on-call team directly (bypassing the self-healing system).

**Q6: How do you handle noisy neighbors in health checking — one service generating millions of unhealthy events?**
A: Per-service and per-node rate limiting on health check event production. The aggregator deduplicates events: for the same target and check type, only the state transition is recorded, not every unhealthy result. We also have per-target suppression: after confirming a failure and initiating remediation, we suppress further health check events for that target until remediation completes.

**Q7: What's the blast radius control strategy for automated remediation?**
A: Multiple layers: (1) Per-rack limit: max 1 node in remediation. (2) Per-cluster limit: max 5% of nodes. (3) Per-region limit: max 2% of total nodes. (4) Global limit: max 500 concurrent remediations. (5) Per-tenant limit: don't remediate >1 node hosting a given tenant's pods simultaneously. These limits are checked before every remediation action and are configurable per environment (staging is more aggressive, production is conservative).

**Q8: How do you handle health check for services behind a load balancer?**
A: Two approaches: (1) Direct pod health checks (via kubelet probes) — check each instance independently. (2) Service-level health check — check the VIP/service endpoint to verify end-to-end connectivity. Both are needed: pod-level catches individual instance failures, service-level catches LB misconfigurations or DNS issues. If the service-level check fails but all pod-level checks pass, the problem is in the infrastructure between (LB, DNS, network policy), not the pods.

**Q9: How would you handle a "gray failure" where a node is technically alive but performing poorly?**
A: Gray failures are the hardest to detect because binary health checks pass. Our composite health score catches this: a node might pass liveness (score 1.0) but have degraded network latency (score 0.5) and high CPU steal time (score 0.4), resulting in a composite score of 72 — in the DEGRADED zone. For gray failures, remediation is gentler: first try restarting the affected process, then rebooting, and only reimage as a last resort. We also use comparative analysis — if one node's P99 latency is 10x its peers in the same cluster, it's likely a gray failure even if absolute numbers are within spec.

**Q10: How do you ensure health checks don't consume too many resources on the nodes being checked?**
A: Resource budgeting. Health check agents (DaemonSet) have resource limits: 100m CPU, 128MB memory. Health check endpoints should be lightweight — /healthz should return 200 in <10ms without doing database queries. We audit health check endpoints for resource usage and flag expensive ones. For deep health checks (DB connectivity, downstream services), we run them at lower frequency (every 60s vs 10s for liveness) and with separate timeout budgets.

**Q11: How do you handle a Kubernetes API server outage? The self-healing system depends on it.**
A: The system degrades gracefully: kubelet continues running existing pods (it caches the pod spec). Node agents continue reporting health via the Kafka pipeline (which doesn't depend on the API server). The reconciliation loop pauses because it can't read/write desired state. We monitor API server availability and alert immediately. The self-healing system itself uses a dedicated API server endpoint (or a separate etcd-based state store) to reduce dependency on the shared API server.

**Q12: How would you handle a firmware bug that causes BMCs to hang across an entire vendor's hardware?**
A: Detection: multiple BMC-unreachable events correlated by hardware vendor/model. The system recognizes this as a common-cause failure and escalates rather than trying to remediate individual nodes. The runbook for this scenario: (1) Identify all affected nodes by vendor/model. (2) Check if in-band agents are still running (BMC hang doesn't necessarily mean the OS is down). (3) If OS is fine, only BMC remediation is needed (firmware update). (4) Coordinate with vendor for firmware patch. (5) Rolling BMC firmware update with per-rack rate limiting.

**Q13: How do you test changes to the self-healing system without risking production?**
A: Four-layer testing: (1) Unit tests with mock infrastructure. (2) Integration tests against a test cluster with simulated failures. (3) Shadow mode in production — new logic runs alongside existing logic, decisions are compared but only the existing logic executes. (4) Canary deployment — new version handles one cluster while old version handles the rest. We also use chaos engineering (separate system) to inject known failures and verify the self-healing system responds correctly.

**Q14: What's the relationship between this system and Kubernetes Operators?**
A: Kubernetes Operators are application-specific controllers that extend the reconciliation model. Our self-healing platform is the infrastructure-level equivalent — it manages node health, bare-metal recovery, and cluster-level concerns. Operators and our system are complementary: an Operator might handle MySQL primary failover (application concern), while our system handles the node that was running the MySQL primary (infrastructure concern). We expose hooks for Operators to register health checks and receive pre-drain notifications.

**Q15: How do you handle the cold-start problem — a brand new cluster with no historical data for AI models?**
A: The AI models are trained on fleet-wide data, not per-cluster data. A new cluster benefits from the fleet's historical failure patterns. For the first 30 days, the new cluster runs in observe-only mode to calibrate thresholds specific to its workload mix. Rule-based remediation (non-AI) is active from day one. AI-based classification kicks in after the 30-day observation period, once the system has enough cluster-specific context.

**Q16: How do you measure false positive rate for failure detection?**
A: Three methods: (1) Post-remediation analysis — if we drained a node and it comes back healthy without any remediation (just time), the detection was likely a false positive. (2) Human review sampling — SREs review a random sample of 100 detections per week and label them as true/false positive. (3) "Canary checks" — we have known-healthy nodes with synthetic health checks; any failure detection on these is definitively a false positive.

---

## 16. References

1. Kubernetes documentation — Pod Lifecycle, Container Probes: https://kubernetes.io/docs/concepts/workloads/pods/pod-lifecycle/
2. Kubernetes Node Problem Detector: https://github.com/kubernetes/node-problem-detector
3. Burns, B., et al. "Borg, Omega, and Kubernetes" — ACM Queue, 2016
4. Beyer, B., et al. "Site Reliability Engineering" — O'Reilly, 2016 (Chapter 22: Addressing Cascading Failures)
5. IPMI Specification v2.0: https://www.intel.com/content/www/us/en/products/docs/servers/ipmi/ipmi-second-gen-interface-spec-v2-rev1-1.html
6. Netflix Tech Blog — "Lessons Netflix Learned from the AWS Outage" (cascading failure prevention)
7. Google SRE Book — "Handling Overload" and "Addressing Cascading Failures"
8. Hodges, J. "Notes on Distributed Systems for Young Bloods" — https://www.somethingsimilar.com/2013/01/14/notes-on-distributed-systems-for-young-bloods/
9. Nygard, M. "Release It!" — Pragmatic Bookshelf, 2018 (Circuit breakers, bulkheads, timeouts)
10. Kubernetes Enhancement Proposal (KEP) — Startup Probes: https://github.com/kubernetes/enhancements/tree/master/keps/sig-node/950-startup-probe
