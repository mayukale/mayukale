# System Design: Automated Failover System

> **Relevance to role:** Failover is the most critical reliability mechanism in cloud infrastructure. When a primary component fails — whether it's a MySQL primary, a Kubernetes control plane, or an entire availability zone — the platform must automatically redirect traffic and promote standbys with minimal data loss (RPO) and minimal downtime (RTO). This design covers the exact failover patterns a cloud infrastructure engineer implements daily: MySQL MHA/Orchestrator, DNS-based failover, split-brain prevention with STONITH, and cross-AZ/cross-region strategies.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Failure Detection:** Detect primary component failures within seconds using heartbeat timeouts, consensus-based detection, and cross-validation with multiple probes.
2. **Active-Passive Failover:** Promote a standby replica to primary when the current primary fails, for stateful services like MySQL, etcd, and message brokers.
3. **Active-Active Failover:** Redirect traffic from a failed active instance to remaining active instances for stateless services.
4. **Split-Brain Prevention:** Ensure at most one primary exists at any time using fencing mechanisms (STONITH) and consensus protocols (Raft/Paxos).
5. **Database Failover:** Automated MySQL primary failover using MHA/Orchestrator with ProxySQL for transparent connection routing.
6. **DNS-Based Failover:** Update DNS records to redirect traffic from failed endpoints to healthy ones, with TTL management.
7. **Load-Balancer Failover:** Remove unhealthy backends from LB pools, redirect traffic to healthy backends across AZs.
8. **Cross-AZ Failover:** Redirect workloads and traffic from a failed AZ to surviving AZs within the same region.
9. **Cross-Region Failover:** Activate disaster recovery region when the primary region is entirely unavailable.
10. **Data Consistency Guarantees:** Configurable RPO/RTO targets per service tier, with explicit trade-offs between consistency and availability.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Failure detection time | < 10 seconds (intra-AZ), < 30 seconds (cross-AZ) |
| Failover execution time (stateless) | < 5 seconds |
| Failover execution time (MySQL) | < 30 seconds |
| Failover execution time (cross-region) | < 5 minutes |
| RPO (Tier 1 — financial, auth) | 0 seconds (synchronous replication) |
| RPO (Tier 2 — general production) | < 1 second (semi-synchronous) |
| RPO (Tier 3 — batch, analytics) | < 5 minutes (asynchronous) |
| RTO (all tiers) | < 30 seconds (intra-region), < 5 minutes (cross-region) |
| Availability of failover system itself | 99.999% |
| False failover rate | < 0.01% (verified by consensus) |

### Constraints & Assumptions
- MySQL 8.0 with semi-synchronous replication is the primary RDBMS.
- Kubernetes clusters span 3 AZs within a region with control plane HA.
- Each region has 3 AZs; DR region is pre-provisioned but cold/warm.
- Network latency: < 1ms intra-AZ, < 5ms cross-AZ, 50-200ms cross-region.
- Message brokers (Kafka) use ISR-based replication with min.insync.replicas=2.
- Java/Python services are deployed as Kubernetes pods with health probes.
- All services use ProxySQL or HAProxy for database connection routing.
- Assumption: network partitions between AZs are rare but possible and must be handled safely.

### Out of Scope
- Application-level data repair after failover (business logic concern).
- Self-healing of the infrastructure underneath (covered in self_healing_infrastructure.md).
- Chaos testing of failover (covered in chaos_engineering_platform.md).
- Manual runbook execution (covered in auto_remediation_runbook_system.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|---|---|---|
| MySQL clusters managed | 500 production clusters (primary + 2 replicas each) | 1,500 MySQL instances |
| Kubernetes clusters | 50 clusters x 3 AZs control plane = 150 control plane nodes | 50 clusters |
| Kafka clusters | 30 clusters x 5 brokers | 150 brokers |
| Stateless service instances | 200,000 pods across fleet | 200K endpoints |
| Heartbeat messages/sec | 1,500 MySQL + 150 K8s CP + 150 Kafka + sampling of 200K pods at 1/10s | ~25,000/sec |
| Failover events/day | ~0.1% daily failure rate x (1,500 + 150 + 150) = ~1.8 | ~2 failovers/day (normal), ~50/day (bad day) |

### Latency Requirements

| Operation | Target | P99 |
|---|---|---|
| Heartbeat round-trip (intra-AZ) | < 5ms | < 20ms |
| Heartbeat round-trip (cross-AZ) | < 10ms | < 50ms |
| Failure detection (heartbeat timeout) | < 10s | < 15s |
| MySQL failover (detect + promote + reroute) | < 15s | < 30s |
| Kubernetes control plane failover | < 30s | < 60s |
| DNS TTL propagation | < 30s (low TTL) | < 60s |
| LB backend removal | < 5s | < 10s |
| Cross-AZ traffic shift | < 10s | < 30s |
| Cross-region activation | < 3 min | < 5 min |

### Storage Estimates

| Data Type | Calculation | Result |
|---|---|---|
| Failover state (per cluster) | 1,800 managed clusters x 5 KB state | 9 MB (fits in etcd) |
| Heartbeat history (24h) | 25K/sec x 100 bytes x 86,400s | ~216 GB/day |
| Failover event logs | 50 events/day x 10 KB | 500 KB/day (negligible; 2-year retention = ~365 MB) |
| Replication lag metrics | 1,500 MySQL instances x 1 metric/sec x 8 bytes | ~12 KB/sec = ~1 GB/day |
| Fencing state (STONITH tokens) | 1,800 clusters x 256 bytes | 450 KB |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Heartbeat traffic | 25K/sec x 200 bytes (request + response) | ~5 MB/sec |
| MySQL replication (semi-sync) | 500 clusters x 1 MB/sec binlog avg | ~500 MB/sec aggregate |
| Kafka ISR replication | 30 clusters x 50 MB/sec avg | ~1.5 GB/sec aggregate |
| Failover coordination messages | Bursty: ~1 MB/sec during failover | ~1 MB/sec peak |

---

## 3. High Level Architecture

```
┌───────────────────────────────────────────────────────────────────────┐
│                    AUTOMATED FAILOVER CONTROL PLANE                   │
│                                                                       │
│  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────────────┐  │
│  │  Failure         │  │  Failover        │  │  Split-Brain         │  │
│  │  Detection       │  │  Orchestrator    │  │  Prevention          │  │
│  │  Service         │  │  (per-cluster    │  │  (Fencing/STONITH)   │  │
│  │  (consensus-     │  │   state machine) │  │                      │  │
│  │   based)         │  │                  │  │                      │  │
│  └────────┬─────────┘  └────────┬─────────┘  └──────────┬───────────┘  │
│           │                     │                        │              │
│  ┌────────▼─────────────────────▼────────────────────────▼───────────┐  │
│  │                     Coordination Layer (etcd)                     │  │
│  │  Leader election │ Fencing tokens │ Cluster state │ Config        │  │
│  └───────────────────────────────┬───────────────────────────────────┘  │
│                                  │                                      │
│  ┌───────────────────────────────▼───────────────────────────────────┐  │
│  │                        Failover Executors                         │  │
│  │  ┌──────────┐  ┌───────────┐  ┌──────────┐  ┌────────────────┐  │  │
│  │  │ MySQL    │  │ Kafka     │  │ K8s      │  │ Cross-Region   │  │  │
│  │  │ Failover │  │ Failover  │  │ Control  │  │ Failover       │  │  │
│  │  │ (MHA/    │  │ (Leader   │  │ Plane    │  │ (DNS + Data    │  │  │
│  │  │ Orchestr)│  │  elect)   │  │ Failover │  │  replication)  │  │  │
│  │  └──────────┘  └───────────┘  └──────────┘  └────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                      Traffic Management                          │   │
│  │  ┌──────────┐  ┌───────────┐  ┌──────────┐  ┌───────────────┐  │   │
│  │  │ ProxySQL │  │ HAProxy / │  │ DNS      │  │ Service Mesh  │  │   │
│  │  │ (MySQL   │  │ L4/L7 LB  │  │ Failover │  │ (Istio/Envoy  │  │   │
│  │  │  routing)│  │ (backend  │  │ (Route53/│  │  retries)     │  │   │
│  │  │          │  │  health)  │  │  CoreDNS)│  │               │  │   │
│  │  └──────────┘  └───────────┘  └──────────┘  └───────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘

               ┌──────────────────────────────────────────┐
               │         MANAGED INFRASTRUCTURE            │
               │                                           │
               │  AZ-1            AZ-2            AZ-3     │
               │  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
               │  │MySQL     │  │MySQL     │  │MySQL    │ │
               │  │Primary   │──│Replica   │──│Replica  │ │
               │  │          │  │(sync)    │  │(async)  │ │
               │  ├──────────┤  ├──────────┤  ├─────────┤ │
               │  │Kafka     │  │Kafka     │  │Kafka    │ │
               │  │Broker x2 │  │Broker x2 │  │Broker x1│ │
               │  ├──────────┤  ├──────────┤  ├─────────┤ │
               │  │K8s API   │  │K8s API   │  │K8s API  │ │
               │  │Server    │  │Server    │  │Server   │ │
               │  └──────────┘  └──────────┘  └─────────┘ │
               └──────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Failure Detection Service** | Consensus-based failure detection using heartbeats and cross-validation. Requires majority agreement before declaring failure. |
| **Failover Orchestrator** | State machine per managed cluster. Drives the failover workflow: detect → fence → promote → reroute → verify. |
| **Split-Brain Prevention** | Manages fencing tokens and STONITH (Shoot The Other Node In The Head). Ensures at most one primary exists. |
| **Coordination Layer (etcd)** | Stores leader election state, fencing tokens, cluster state, and configuration. Provides strong consistency for coordination. |
| **MySQL Failover Executor** | Implements MySQL-specific failover: stop old primary's writes (STONITH), identify best replica (least lag), promote, reconfigure replication topology, update ProxySQL routing. |
| **Kafka Failover Executor** | Handles Kafka leader election for partitions, ISR management, and broker replacement. |
| **K8s Control Plane Failover** | Manages API server, etcd, and controller-manager HA across AZs. |
| **Cross-Region Failover Executor** | Activates DR region: promote read replicas, update global DNS, warm up caches. |
| **ProxySQL** | MySQL connection proxy that routes reads/writes to the correct primary/replica. Updated automatically during failover. |
| **HAProxy/L4-L7 LB** | Removes unhealthy backends from pool based on health checks. Distributes traffic across healthy instances. |
| **DNS Failover** | Updates DNS records (A/AAAA/CNAME) to point to healthy endpoints. Uses low TTL (30s) for fast propagation. |
| **Service Mesh (Istio/Envoy)** | Application-level failover via retries, circuit breaking, and traffic shifting between zones. |

### Data Flows

**Primary Flow — MySQL Failover:**
1. Heartbeat agents on each MySQL instance send heartbeats to Failure Detection Service.
2. Heartbeat timeout (3 consecutive misses at 3s interval = 9s detection).
3. Failure Detection Service requires consensus (2-of-3 detectors agree).
4. Failover Orchestrator acquires fencing token from etcd.
5. STONITH: fence the failed primary (IPMI power-off or network isolation).
6. MySQL Failover Executor: identify best replica (smallest replication lag), apply relay logs, promote to read-write.
7. ProxySQL reconfigured: new primary gets write traffic, old primary removed.
8. Remaining replicas pointed to new primary.
9. Verification: write test row, read from replica, confirm replication flowing.

**Secondary Flow — Cross-AZ LB Failover:**
1. Health checks on backend pods fail in AZ-1.
2. LB removes AZ-1 backends from pool within 5 seconds.
3. Traffic automatically shifts to AZ-2 and AZ-3 backends.
4. If AZ-2/AZ-3 capacity is insufficient, trigger horizontal scaling (HPA).
5. Alert: "AZ-1 backends removed; capacity may be reduced."

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Managed cluster (anything that can fail over)
CREATE TABLE managed_cluster (
    cluster_id          VARCHAR(128) PRIMARY KEY,
    cluster_type        ENUM('mysql', 'kafka', 'etcd', 'k8s_control_plane',
                             'elasticsearch', 'redis', 'custom') NOT NULL,
    region              VARCHAR(64) NOT NULL,
    failover_mode       ENUM('active_passive', 'active_active', 'multi_primary') NOT NULL,
    current_primary_id  VARCHAR(256),
    primary_az          VARCHAR(64),
    rpo_target_sec      INT NOT NULL DEFAULT 1,
    rto_target_sec      INT NOT NULL DEFAULT 30,
    failover_state      ENUM('normal', 'detecting', 'fencing', 'promoting',
                             'rerouting', 'verifying', 'completed', 'failed',
                             'manual_intervention') NOT NULL DEFAULT 'normal',
    last_failover_at    TIMESTAMP NULL,
    failover_count_30d  INT NOT NULL DEFAULT 0,
    config_json         JSON,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_type_state (cluster_type, failover_state),
    INDEX idx_region (region)
) ENGINE=InnoDB;

-- Cluster members (primaries and replicas)
CREATE TABLE cluster_member (
    member_id           VARCHAR(256) PRIMARY KEY,
    cluster_id          VARCHAR(128) NOT NULL,
    role                ENUM('primary', 'sync_replica', 'async_replica', 'standby',
                             'active', 'witness') NOT NULL,
    host                VARCHAR(256) NOT NULL,
    port                INT NOT NULL,
    availability_zone   VARCHAR(64) NOT NULL,
    replication_lag_ms  INT DEFAULT 0,
    last_heartbeat_at   TIMESTAMP NULL,
    is_healthy          BOOLEAN NOT NULL DEFAULT TRUE,
    is_fenced           BOOLEAN NOT NULL DEFAULT FALSE,
    fence_method        ENUM('ipmi_power_off', 'network_isolate', 'process_kill',
                             'none') DEFAULT 'none',
    priority            INT NOT NULL DEFAULT 100,  -- lower = higher priority for promotion
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (cluster_id) REFERENCES managed_cluster(cluster_id),
    INDEX idx_cluster_role (cluster_id, role),
    INDEX idx_heartbeat (last_heartbeat_at),
    INDEX idx_az (availability_zone)
) ENGINE=InnoDB;

-- Failover events (audit trail)
CREATE TABLE failover_event (
    event_id            BIGINT PRIMARY KEY AUTO_INCREMENT,
    cluster_id          VARCHAR(128) NOT NULL,
    event_type          ENUM('failure_detected', 'failover_started', 'primary_fenced',
                             'replica_promoted', 'traffic_rerouted', 'failover_completed',
                             'failover_failed', 'failover_rolled_back',
                             'manual_intervention_required') NOT NULL,
    old_primary_id      VARCHAR(256),
    new_primary_id      VARCHAR(256),
    detection_method    VARCHAR(128),
    data_loss_estimate  VARCHAR(128),   -- e.g., "0 transactions", "~500ms of writes"
    rpo_achieved_ms     INT,
    rto_achieved_ms     INT,
    initiated_by        ENUM('automated', 'manual', 'scheduled') NOT NULL,
    initiated_by_user   VARCHAR(128),
    details_json        JSON,
    created_at          TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (cluster_id) REFERENCES managed_cluster(cluster_id),
    INDEX idx_cluster_time (cluster_id, created_at),
    INDEX idx_event_type (event_type, created_at)
) ENGINE=InnoDB;

-- Fencing tokens (for split-brain prevention)
CREATE TABLE fencing_token (
    cluster_id          VARCHAR(128) PRIMARY KEY,
    token_value         BIGINT UNSIGNED NOT NULL,
    holder_id           VARCHAR(256) NOT NULL,    -- who holds the fence
    acquired_at         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at          TIMESTAMP NOT NULL,
    FOREIGN KEY (cluster_id) REFERENCES managed_cluster(cluster_id)
) ENGINE=InnoDB;

-- Heartbeat records (recent window for debugging — bulk stored in ES)
CREATE TABLE heartbeat_log (
    id                  BIGINT PRIMARY KEY AUTO_INCREMENT,
    member_id           VARCHAR(256) NOT NULL,
    cluster_id          VARCHAR(128) NOT NULL,
    heartbeat_at        TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    latency_ms          INT,
    replication_lag_ms  INT,
    INDEX idx_member_time (member_id, heartbeat_at)
) ENGINE=InnoDB
PARTITION BY RANGE (UNIX_TIMESTAMP(heartbeat_at)) (
    -- Auto-managed partitions, 1 per hour, retain 24h
);

-- DNS failover records
CREATE TABLE dns_failover_record (
    record_id           BIGINT PRIMARY KEY AUTO_INCREMENT,
    domain_name         VARCHAR(512) NOT NULL,
    record_type         ENUM('A', 'AAAA', 'CNAME', 'SRV') NOT NULL,
    cluster_id          VARCHAR(128) NOT NULL,
    primary_target      VARCHAR(512) NOT NULL,
    failover_target     VARCHAR(512) NOT NULL,
    ttl_seconds         INT NOT NULL DEFAULT 30,
    current_target      VARCHAR(512) NOT NULL,
    health_check_id     VARCHAR(128),
    last_switched_at    TIMESTAMP NULL,
    UNIQUE INDEX idx_domain (domain_name, record_type)
) ENGINE=InnoDB;
```

### Database Selection

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **MySQL 8.0** | ACID, team expertise, replication maturity | Schema rigidity, single-writer for state updates | **Selected for failover metadata and audit logs** |
| **etcd** | Linearizable reads/writes, watch semantics, native K8s | 8 GB limit, not for high-volume data | **Selected for leader election and fencing tokens** |
| **Elasticsearch** | Full-text search, time-series | Eventually consistent | **Selected for heartbeat history and event search** |
| **CockroachDB** | Distributed SQL, multi-region consistency | Operational overhead, latency | Not selected — over-engineered for our state size |

**Justification:** The failover system's own state is small (< 100 MB). MySQL handles the metadata and audit trail. etcd provides the consistency guarantees needed for leader election and fencing tokens — these are the most critical data points where split-brain could be catastrophic. Elasticsearch indexes the high-volume heartbeat data for debugging.

### Indexing Strategy

- `managed_cluster(cluster_type, failover_state)` — find all MySQL clusters currently in failover.
- `cluster_member(cluster_id, role)` — find the current primary and replicas of a cluster.
- `cluster_member(last_heartbeat_at)` — find members with stale heartbeats.
- `failover_event(cluster_id, created_at)` — audit trail for a specific cluster.
- `heartbeat_log` — partitioned hourly, retained 24h. Partition pruning for time-range queries.

---

## 5. API Design

### REST Endpoints

```
Base URL: https://failover.infra.internal/api/v1

# Cluster management
POST   /clusters
  Body: {
    "cluster_id": "mysql-orders-prod",
    "cluster_type": "mysql",
    "failover_mode": "active_passive",
    "rpo_target_sec": 0,
    "rto_target_sec": 15,
    "members": [
      {"member_id": "mysql-orders-primary", "host": "10.0.1.10", "port": 3306,
       "role": "primary", "az": "us-east-1a", "priority": 1},
      {"member_id": "mysql-orders-replica-1", "host": "10.0.2.10", "port": 3306,
       "role": "sync_replica", "az": "us-east-1b", "priority": 2},
      {"member_id": "mysql-orders-replica-2", "host": "10.0.3.10", "port": 3306,
       "role": "async_replica", "az": "us-east-1c", "priority": 3}
    ],
    "config": {
      "heartbeat_interval_sec": 3,
      "heartbeat_timeout_sec": 9,
      "fence_method": "ipmi_power_off",
      "auto_failover_enabled": true
    }
  }
  Response: 201 Created

GET    /clusters?type=mysql&state=normal&region=us-east-1
  Response: 200 [...]

GET    /clusters/{cluster_id}
  Response: 200 { full cluster state including members, current primary, failover history }

PUT    /clusters/{cluster_id}/config
  Body: { "auto_failover_enabled": false, "reason": "planned maintenance" }
  Response: 200

# Failover operations
POST   /clusters/{cluster_id}/failover
  Body: {
    "target_primary_id": "mysql-orders-replica-1",   // optional — system selects best if omitted
    "reason": "manual failover for maintenance",
    "dry_run": false
  }
  Response: 202 Accepted { "failover_event_id": 12345 }

GET    /clusters/{cluster_id}/failover/status
  Response: 200 {
    "state": "rerouting",
    "old_primary": "mysql-orders-primary",
    "new_primary": "mysql-orders-replica-1",
    "started_at": "2026-04-09T10:30:15Z",
    "estimated_completion_sec": 5,
    "steps": [
      {"step": "detect", "status": "completed", "duration_ms": 9200},
      {"step": "fence", "status": "completed", "duration_ms": 3100},
      {"step": "promote", "status": "completed", "duration_ms": 2500},
      {"step": "reroute", "status": "in_progress", "duration_ms": null},
      {"step": "verify", "status": "pending"}
    ]
  }

# Health and monitoring
GET    /clusters/{cluster_id}/health
  Response: 200 {
    "cluster_id": "mysql-orders-prod",
    "status": "healthy",
    "primary": {"member_id": "...", "healthy": true, "replication_lag_ms": 0},
    "replicas": [
      {"member_id": "...", "role": "sync_replica", "healthy": true, "replication_lag_ms": 0},
      {"member_id": "...", "role": "async_replica", "healthy": true, "replication_lag_ms": 250}
    ],
    "rpo_current_ms": 0,
    "last_failover": "2026-03-15T08:22:00Z"
  }

# DNS failover
POST   /dns-failover
  Body: {
    "domain_name": "orders-db.internal.example.com",
    "record_type": "A",
    "cluster_id": "mysql-orders-prod",
    "primary_target": "10.0.1.10",
    "failover_target": "10.0.2.10",
    "ttl_seconds": 30,
    "health_check_id": "hc-mysql-orders"
  }
  Response: 201

# Fencing operations
POST   /clusters/{cluster_id}/fence
  Body: {
    "target_member_id": "mysql-orders-primary",
    "fence_method": "ipmi_power_off",
    "reason": "failover in progress"
  }
  Response: 200 { "fencing_token": 42, "fenced_at": "..." }

POST   /clusters/{cluster_id}/unfence
  Body: { "target_member_id": "mysql-orders-primary", "fencing_token": 42 }
  Response: 200
```

### gRPC Service (Internal high-frequency heartbeat)

```protobuf
service HeartbeatService {
  rpc SendHeartbeat(HeartbeatRequest) returns (HeartbeatResponse);
  rpc StreamHeartbeat(stream HeartbeatRequest) returns (stream HeartbeatResponse);
}

message HeartbeatRequest {
  string member_id = 1;
  string cluster_id = 2;
  string role = 3;
  int32 replication_lag_ms = 4;
  int64 timestamp_ms = 5;
  map<string, string> metadata = 6;  // e.g., GTID position
}

message HeartbeatResponse {
  bool acknowledged = 1;
  string cluster_state = 2;    // "normal", "failover_in_progress"
  string current_primary = 3;
}

service FailoverCoordinationService {
  rpc AcquireFencingToken(FenceRequest) returns (FenceResponse);
  rpc ReleaseFencingToken(UnfenceRequest) returns (UnfenceResponse);
  rpc NotifyFailoverComplete(FailoverCompleteRequest) returns (FailoverCompleteResponse);
}
```

### CLI Design

```bash
# Cluster management
failover cluster list --type=mysql --region=us-east-1
failover cluster show mysql-orders-prod --include-members --include-history
failover cluster register --config=./cluster-config.yaml
failover cluster config mysql-orders-prod --auto-failover=false --reason="maintenance"

# Failover operations
failover trigger mysql-orders-prod --target=mysql-orders-replica-1 --reason="planned"
failover trigger mysql-orders-prod --dry-run  # show what would happen
failover status mysql-orders-prod             # real-time status
failover history mysql-orders-prod --last=30d

# Health monitoring
failover health mysql-orders-prod
failover health --all --type=mysql --unhealthy-only
failover replication-lag mysql-orders-prod --watch  # live replication lag

# Fencing
failover fence mysql-orders-primary --method=ipmi_power_off --reason="failover"
failover unfence mysql-orders-primary --token=42

# DNS failover
failover dns list
failover dns switch orders-db.internal.example.com --target=10.0.2.10 --reason="AZ failure"
failover dns rollback orders-db.internal.example.com

# Cross-region
failover activate-dr --region=us-west-2 --reason="us-east-1 outage" --dry-run
failover activate-dr --region=us-west-2 --reason="us-east-1 outage" --approve
```

---

## 6. Core Component Deep Dives

### 6.1 Failure Detection Service (Consensus-Based)

**Why it's hard:** False positives in failure detection are catastrophic — unnecessarily failing over a perfectly healthy MySQL primary causes a write outage and potential data loss. But false negatives (not detecting a real failure) cause prolonged downtime. The sweet spot requires consensus: multiple independent detectors must agree before failover starts.

**Approaches Compared:**

| Approach | False Positive Rate | Detection Latency | Complexity |
|---|---|---|---|
| **Single heartbeat checker** | High (network blip = false alarm) | 1x timeout | Low |
| **Dual heartbeat + application probe** | Medium | 1x timeout | Medium |
| **N-of-M consensus (Raft-inspired)** | Very low | 1x timeout + consensus round | High |
| **Phi Accrual Failure Detector** | Low (adaptive thresholds) | Variable | Medium-High |

**Selected Approach: N-of-M consensus with Phi Accrual for adaptive thresholds.**

**Justification:** We deploy 3 failure detector instances per region (one per AZ). Each independently monitors all clusters via heartbeats. A failure is confirmed only when 2-of-3 detectors agree. This prevents false failovers caused by a single network partition between detector and target. The Phi Accrual algorithm adapts the heartbeat timeout based on historical latency distribution, avoiding both premature and late detection.

**Implementation Detail:**

**Phi Accrual Failure Detector:**
```
The Phi (φ) value represents the suspicion level that a node has failed.
φ = -log10(1 - F(timeSinceLastHeartbeat))
where F is the CDF of the normal distribution fitted to historical heartbeat intervals.

φ > 8  → ~99.9997% confidence the node has failed (declare failure candidate)
φ > 12 → ~99.999999% confidence (auto-confirm without consensus for critical clusters)

Adaptive: if network jitter increases, the distribution widens, and φ increases more
slowly — preventing false positives during network instability.
```

**Consensus Flow:**
```
Detector-AZ1  ──► "member-X heartbeat missed, φ=10" ──►
Detector-AZ2  ──► "member-X heartbeat missed, φ=9"  ──►  Consensus: 2/3 agree
Detector-AZ3  ──► "member-X heartbeat OK,    φ=2"   ──►  → FAILURE CONFIRMED
```

**Heartbeat Protocol:**
- Each managed cluster member sends gRPC heartbeats to all 3 detectors.
- Heartbeat includes: member_id, role, replication_lag_ms, GTID position (for MySQL), timestamp.
- Heartbeat interval: 3 seconds (configurable per cluster type).
- Detectors also perform active probes: TCP connect to service port, MySQL `SELECT 1`, Kafka metadata request.

**Failure Modes:**
- **Detector itself fails:** 2 remaining detectors still form a majority. System degrades to 2-of-2 consensus (unanimous required) until the failed detector recovers.
- **All 3 detectors disagree:** No consensus → no failover → alert human. This is the safe default.
- **Detector in a different AZ can't reach the target (network partition):** That detector votes "failed," but the other 2 (in the same AZ or with connectivity) vote "healthy." No false failover.

**Interviewer Q&As:**

**Q1: Why not use ZooKeeper or etcd's built-in leader election for failure detection?**
A: ZooKeeper/etcd leader election detects loss of the leader's session, not the health of an external service. We need to detect MySQL primary failure, which is a different system. We use etcd for coordinating the failover response, but the detection itself requires application-level health probes.

**Q2: What happens if the heartbeat network is partitioned from the data network?**
A: A heartbeat failure doesn't always mean the service is down — it could be a management network issue. Our detectors use both the heartbeat channel (management network) and active probes on the data network (TCP connect to MySQL port 3306). If heartbeats fail but active probes succeed, we don't fail over — we alert on management network issues.

**Q3: How do you handle a "slow primary" that responds to heartbeats but is too slow to serve queries?**
A: Heartbeats include response latency. If the primary's query latency exceeds the SLO (e.g., p99 > 500ms for 5 minutes), it's declared "performance-degraded." This triggers a different workflow: not failover, but traffic shift — replicas start serving reads, and the primary is investigated. Failover is a last resort.

**Q4: What's the minimum detection time for a hard crash (process killed)?**
A: 1 heartbeat interval (3s) × failure threshold (3 consecutive misses) = 9 seconds to detect on a single detector. Add 2 seconds for consensus round. Total: ~11 seconds. For ultra-critical clusters (auth, payments), we use 1-second heartbeats and threshold of 3, giving ~5 second detection.

**Q5: How do you prevent "failover storms" where multiple clusters fail over simultaneously?**
A: Global rate limiting: max 5 failovers in progress simultaneously per region. If more than 5 clusters are failing at once, it's likely a common cause (network event, AZ failure) and individual failovers won't help. The system queues the failovers by priority (tier-1 first) and alerts for the common cause.

**Q6: Can you explain the Phi Accrual Failure Detector in simple terms?**
A: Imagine you have a friend who texts you every 3 seconds. You learn their texting pattern — sometimes it takes 2.8s, sometimes 3.2s. If 6 seconds pass with no text, you're suspicious. If 15 seconds pass, you're nearly certain something is wrong. The Phi detector quantifies this suspicion mathematically: it learns the normal distribution of heartbeat intervals and computes a "suspicion score" based on how unlikely the current silence is. Higher score = more suspicious.

---

### 6.2 MySQL Failover Executor (MHA/Orchestrator + ProxySQL)

**Why it's hard:** MySQL failover involves multiple interacting systems: the failed primary, the promoted replica, other replicas that need re-pointing, the connection proxy, and active client connections. Getting any step wrong can cause data loss (if the promoted replica is behind), split-brain (if the old primary comes back), or client errors (if connections aren't properly rerouted).

**Approaches Compared:**

| Approach | Data Safety | Automation Level | Complexity | RTO |
|---|---|---|---|---|
| **MySQL Group Replication** | High (Paxos-based) | Fully auto | High | < 5s |
| **MHA (MySQL High Availability)** | Good (applies relay logs) | Semi-auto | Medium | 10-30s |
| **Orchestrator** | Good (topology-aware) | Fully auto | Medium | 10-20s |
| **Custom (MHA + Orchestrator concepts + ProxySQL)** | Best (combined) | Fully auto | High | 10-30s |

**Selected Approach: Custom failover combining Orchestrator's topology awareness with MHA's relay log application and ProxySQL for transparent routing.**

**Justification:** MySQL Group Replication changes the MySQL topology fundamentally (multi-primary or single-primary within the group). Our fleet uses standard semi-synchronous replication. Orchestrator provides excellent topology discovery and anti-flap logic. MHA ensures no data loss by applying relay logs from the failed primary's binlog. ProxySQL handles transparent connection rerouting without application changes.

**Implementation Detail — MySQL Failover State Machine:**

```
State: NORMAL
  │
  ├── [Failure detected by consensus]
  ▼
State: FENCING
  │  Action: Acquire fencing token from etcd (monotonically increasing)
  │  Action: STONITH old primary:
  │    Option A: IPMI power off (hard, reliable)
  │    Option B: iptables DROP on old primary's network (soft)
  │    Option C: Kill MySQL process (if accessible)
  │  Verification: Confirm old primary is not accepting writes
  │    - Attempt to write a fencing probe row
  │    - If write succeeds → fencing failed → abort failover
  │
  ├── [Fencing confirmed]
  ▼
State: PROMOTING
  │  Action: Select best replica:
  │    - Prefer sync_replica over async_replica (less data loss)
  │    - Prefer replica in a different AZ from failed primary (AZ diversity)
  │    - Prefer replica with smallest replication lag (least data loss)
  │    - Prefer replica with highest priority (admin-configurable)
  │  Action: Apply remaining relay logs on selected replica
  │    - Wait for relay log application to complete (timeout: 30s)
  │    - If relay logs can't be applied (corruption), try next best replica
  │  Action: SET GLOBAL read_only=OFF on new primary
  │  Action: RESET SLAVE ALL on new primary
  │
  ├── [Promotion complete]
  ▼
State: REROUTING
  │  Action: Update ProxySQL:
  │    - DELETE FROM mysql_servers WHERE hostgroup_id=<write_group>
  │    - INSERT INTO mysql_servers (hostgroup_id, hostname, port)
  │      VALUES (<write_group>, '<new_primary_host>', 3306)
  │    - LOAD MYSQL SERVERS TO RUNTIME
  │  Action: Update DNS (if DNS-based routing is used)
  │  Action: Re-point remaining replicas to new primary:
  │    - CHANGE MASTER TO MASTER_HOST='<new_primary>' on each replica
  │    - START SLAVE on each replica
  │
  ├── [Rerouting complete]
  ▼
State: VERIFYING
  │  Action: Write test row to new primary
  │  Action: Read test row from each replica (verify replication flowing)
  │  Action: Check ProxySQL: SELECT * FROM stats_mysql_connection_pool
  │  Action: Verify application connections are flowing (monitor query rate)
  │
  ├── [All verifications pass]
  ▼
State: COMPLETED
  │  Log: RPO achieved, RTO achieved, data loss estimate
  │  Alert: Notify SRE team of completed failover
```

**RPO Analysis for Different Replication Modes:**
| Replication Mode | RPO | Mechanism | Trade-off |
|---|---|---|---|
| **Fully synchronous** | 0 | Primary waits for all replicas to ACK | Higher write latency, lower throughput |
| **Semi-synchronous** | ~0 (in practice, 0-1 transactions) | Primary waits for at least 1 replica to ACK | Slight latency increase, good compromise |
| **Asynchronous** | 0-N seconds (depends on lag) | Primary doesn't wait for any replica | Best performance, risk of data loss |

**We default to semi-synchronous replication** with `rpl_semi_sync_source_wait_for_replica_count=1`. This means the primary waits for at least 1 replica to acknowledge each transaction before committing. If the primary crashes after commit but before the replica acknowledges, at most 1 in-flight transaction is lost.

**Failure Modes:**
- **Fencing fails (can't reach old primary):** Abort failover. DO NOT promote replica. Alert human. Risk: old primary might still be accepting writes.
- **Best replica has significant lag:** Accept data loss up to the configured RPO threshold. If lag exceeds RPO, require human approval.
- **ProxySQL update fails:** Applications get connection errors. Retry ProxySQL update. If persistent, applications' retry logic handles it (exponential backoff).
- **Old primary comes back after failover:** It must rejoin as a replica, not as primary. The fencing token in etcd prevents it from claiming primary role.

**Interviewer Q&As:**

**Q1: Why is fencing (STONITH) so important? What happens without it?**
A: Without fencing, the old primary might still be alive and accepting writes (e.g., it was network-partitioned but not crashed). If we promote a replica while the old primary is still writable, two primaries exist simultaneously — split brain. Clients connected to the old primary write data that's not replicated to the new primary, causing data divergence. STONITH ensures the old primary is definitively stopped before promotion.

**Q2: What's the "fencing token" pattern and why use it?**
A: A fencing token is a monotonically increasing integer stored in etcd. When a failover starts, the orchestrator acquires a new token (e.g., 42). All future write requests to the storage system must include this token. The storage rejects writes with a token older than the latest. This ensures that if an old primary comes back with token 41, its writes are rejected because the system is now on token 42. This is the pattern described by Martin Kleppmann in "Designing Data-Intensive Applications."

**Q3: How does ProxySQL know the failover happened?**
A: ProxySQL has an admin interface (port 6032). The failover executor connects to ProxySQL's admin interface and updates the mysql_servers table to point the write hostgroup to the new primary. ProxySQL then drains connections from the old primary and establishes new connections to the new primary. This is transparent to the application — applications connect to ProxySQL's frontend port (6033) and see no change.

**Q4: What about in-flight transactions during failover?**
A: In-flight transactions on the old primary are lost — they were never committed (if the primary crashed) or committed but not acknowledged to the client. The application must handle this: idempotent writes and retry logic. In-flight reads on replicas continue to work if the replicas are healthy. Applications using connection pooling (HikariCP for Java, SQLAlchemy for Python) retry failed connections automatically.

**Q5: How do you handle the case where the best replica is in the same AZ as the failed primary?**
A: We prefer cross-AZ promotion for AZ diversity. But if the only sync replica is in the same AZ and the async replica in another AZ has significant lag, we face a trade-off: promote the sync replica (0 data loss, same AZ) or the async replica (some data loss, different AZ). Default: prefer zero data loss (promote sync replica). Configuration: per-cluster policy based on tier.

**Q6: How long does the entire MySQL failover take?**
A: Breakdown: Detection (9s) + Consensus (2s) + Fencing (3s IPMI power-off) + Relay log application (0-5s depending on lag) + ProxySQL update (1s) + Verification (3s) = **~18s typical**. P99: 30s. The dominant factor is detection time; for lower RTO, decrease heartbeat interval (but increase heartbeat traffic).

---

### 6.3 Split-Brain Prevention

**Why it's hard:** Split brain is the most dangerous failure mode in active-passive systems. Two nodes believe they are the primary and accept writes independently. After the partition heals, their data has diverged and cannot be automatically reconciled. Prevention requires ironclad guarantees that exactly one primary exists at all times.

**Approaches Compared:**

| Approach | Safety | Availability | Complexity |
|---|---|---|---|
| **STONITH (hardware fencing)** | Highest — physically stops old primary | Moderate — fencing takes time | Low |
| **Network fencing (iptables)** | High — blocks traffic | High — fast to apply | Medium |
| **Consensus protocol (Raft/Paxos)** | Very high — majority agreement | Requires majority of nodes | High |
| **Fencing tokens (etcd-backed)** | Very high — monotonic ordering | High — fast token acquisition | Medium |

**Selected Approach: STONITH (primary mechanism) + fencing tokens (secondary mechanism).**

**Justification:** For bare-metal MySQL, STONITH via IPMI power-off is the gold standard — it physically stops the old primary. For VMs/containers, we use network fencing (security group/iptables). Fencing tokens provide an additional layer: even if the old primary somehow comes back, the storage system rejects its writes because it has an old token.

**STONITH Implementation:**
```python
class STONITHFencer:
    def fence(self, member: ClusterMember, method: str) -> bool:
        if method == "ipmi_power_off":
            # Hard fencing — physically powers off the node
            result = ipmi_command(member.bmc_ip, "power off")
            if result.success:
                # Verify power is actually off
                for attempt in range(5):
                    status = ipmi_command(member.bmc_ip, "power status")
                    if status.output == "Chassis Power is off":
                        return True
                    time.sleep(2)
            return False
        
        elif method == "network_isolate":
            # Soft fencing — block all traffic to/from the node
            result = network_api.isolate_host(member.host)
            # Verify: attempt TCP connect to member's service port
            try:
                socket.create_connection((member.host, member.port), timeout=5)
                return False  # Connection succeeded — fencing failed
            except ConnectionRefusedError:
                return True  # Connection refused — fencing succeeded
            except socket.timeout:
                return True  # Timeout — fencing succeeded
        
        elif method == "process_kill":
            # Last resort for VMs/containers
            result = ssh_command(member.host, f"kill -9 $(pidof mysqld)")
            return result.exit_code == 0
```

**Failure Modes:**
- **STONITH fails (BMC unreachable):** Abort failover. Do not promote replica. This is the safe default — it's better to have no primary than two primaries.
- **Network fence fails (firewall API error):** Fall back to IPMI. If both fail, abort.
- **Fencing token lost (etcd unavailable):** The fencing token mechanism is a secondary protection. If etcd is unavailable, STONITH alone provides safety. But we do not start failover if both STONITH and fencing tokens are unavailable.

**Interviewer Q&As:**

**Q1: What happens if the old primary was just network-partitioned and is still running? After fencing and failover, can it rejoin?**
A: After the partition heals, the old primary discovers it's been fenced. It must rejoin as a replica. Steps: (1) Detect that it's no longer the primary (check etcd for current primary). (2) Stop accepting writes (SET GLOBAL read_only=ON). (3) Re-establish replication from the new primary. (4) If it received writes during the partition that the new primary doesn't have (split-brain data), those writes are in a "diverged" binlog. We save them for manual review but do not apply them.

**Q2: How do you handle the case where fencing takes too long (BMC is slow)?**
A: We set a fencing timeout (30s). If fencing doesn't complete within the timeout, the failover is aborted and we alert humans. The timeout is a trade-off: too short and we might abort unnecessarily, too long and the outage is prolonged. 30 seconds is chosen because IPMI power-off typically completes in 5-10 seconds.

**Q3: Is STONITH necessary for all cluster types?**
A: STONITH is most critical for write-primary databases (MySQL, PostgreSQL) where split-brain causes data divergence. For stateless services (web servers), split-brain is less dangerous — multiple instances serving traffic simultaneously is normal. For Kafka, the ISR mechanism prevents split-brain because a broker can only be partition leader if it's in the ISR set, which requires acknowledgment from the controller.

**Q4: How do you prevent the failover system itself from causing a split brain?**
A: The failover orchestrator uses etcd for coordination. Only the orchestrator that holds the etcd lease for a cluster can execute failover. etcd provides linearizable guarantees, so two orchestrators cannot simultaneously acquire the lease. If the orchestrator crashes mid-failover, the lease expires and a new orchestrator takes over, reading the cluster's current state.

**Q5: What about "Byzantine" failures where the fenced node reports that it's fenced but is actually still running?**
A: We don't trust the node's self-report. STONITH verification is independent: for IPMI, we query power status from the BMC (separate from the node's OS). For network fencing, we attempt a connection from outside. Trust, but verify — from an independent vantage point.

**Q6: Can you fail over without STONITH if you can guarantee the old primary is truly crashed?**
A: If we have absolute proof the primary is crashed (e.g., the primary reported "out of memory, kernel panic" just before heartbeats stopped, and the BMC confirms the OS is not running), we can skip STONITH. But "absolute proof" is rare. In practice, we always attempt STONITH as a precaution — better safe than sorry.

---

### 6.4 Cross-Region Failover

**Why it's hard:** Cross-region failover involves: (a) much higher latency (50-200ms), making synchronous replication impractical; (b) promoting a read-only DR region to primary, which requires warming up caches, redirecting global traffic, and handling DNS propagation; (c) the decision to fail over is high-stakes — cross-region failover is a "big red button" that's hard to reverse.

**Approaches Compared:**

| Approach | RPO | RTO | Cost | Complexity |
|---|---|---|---|---|
| **Hot standby (active-active)** | 0 (synchronous) | < 30s | Very high (2x infra) | Very high |
| **Warm standby (async replication)** | Seconds-minutes | 2-5 min | High (1.5x infra) | High |
| **Cold standby (backup restore)** | Hours | 30-60 min | Low (1.1x infra) | Medium |
| **Pilot light (minimal DR, scale on demand)** | Minutes | 15-30 min | Medium (1.2x infra) | Medium |

**Selected Approach: Warm standby for Tier 1-2, Pilot light for Tier 3.**

**Justification:** Hot standby doubles infrastructure cost and adds significant complexity for cross-region synchronous writes. Cold standby has unacceptable RPO/RTO for production services. Warm standby provides a good balance: asynchronous replication keeps the DR region within seconds of the primary, and pre-provisioned infrastructure allows fast activation. Tier 3 services use pilot light since they can tolerate longer RPO/RTO.

**Implementation — Cross-Region Failover Workflow:**

```
Pre-requisites (always running):
  - MySQL async replication to DR region (lag < 5s target)
  - Kafka MirrorMaker replicating topics to DR region
  - Elasticsearch cross-cluster replication (CCR) to DR region
  - DR region has K8s cluster with scaled-down deployments (replicas=1 vs 10)
  - Global DNS (Route53) with health checks on primary region

Failover Steps:
1. DETECT: Primary region health check fails (3 consecutive failures from 3 global PoPs)
2. DECIDE: Human confirmation required (via PagerDuty + Slack approval from 2 SREs)
   - Exception: if primary region is completely unreachable for > 5 min,
     auto-failover triggers without human approval
3. ACTIVATE DR DATABASES:
   - Stop replication from primary (STOP SLAVE)
   - Promote DR MySQL replicas to primary (SET GLOBAL read_only=OFF)
   - Verify data integrity (checksum recent tables)
   - Accept data loss: log the replication lag at time of promotion
4. SCALE DR COMPUTE:
   - Scale Kubernetes deployments to production size (replicas: 1 → 10)
   - HPA kicks in as traffic arrives
   - Wait for pods to be Running (readiness probe pass)
5. WARM CACHES:
   - Pre-populate Redis/Memcached from database (cache warming script)
   - Pre-populate CDN edge locations (if applicable)
6. UPDATE TRAFFIC:
   - Update Route53 health check to point to DR region
   - Update global LB to route traffic to DR endpoints
   - TTL propagation: 30s for low-TTL records
   - Verify traffic is flowing to DR region (monitor request rate)
7. VERIFY:
   - Synthetic monitoring: send test requests to DR region
   - Check error rates, latency, and throughput
   - Compare with baseline metrics from primary region
8. COMMUNICATE:
   - Status page update: "Failover to DR region complete"
   - Estimated RPO: "~X seconds of data may be lost for async-replicated services"
```

**DNS Failover Configuration:**
```json
{
  "Route53 Health Check": {
    "Type": "HTTPS",
    "FullyQualifiedDomainName": "api.primary.example.com",
    "Port": 443,
    "ResourcePath": "/healthz",
    "RequestInterval": 10,
    "FailureThreshold": 3,
    "Regions": ["us-east-1", "eu-west-1", "ap-northeast-1"]
  },
  "Failover Record Set": {
    "Name": "api.example.com",
    "Type": "A",
    "SetIdentifier": "primary",
    "Failover": "PRIMARY",
    "AliasTarget": "api.primary.example.com",
    "HealthCheckId": "hc-primary"
  },
  "Secondary Record Set": {
    "Name": "api.example.com",
    "Type": "A",
    "SetIdentifier": "secondary",
    "Failover": "SECONDARY",
    "AliasTarget": "api.dr.example.com"
  }
}
```

**Failure Modes:**
- **DR database far behind primary:** Accept data loss or wait for replication to catch up (trade-off: availability vs consistency).
- **DR compute can't scale fast enough:** Pre-provision more capacity in DR (cost trade-off).
- **DNS propagation slow:** Some clients cache DNS beyond TTL. Use multiple traffic redirect mechanisms (DNS + LB + service mesh).
- **Failback after primary recovers:** This is equally complex. Require replication to flow back from DR to primary, verify consistency, then reverse the traffic shift.

**Interviewer Q&As:**

**Q1: Why require human approval for cross-region failover?**
A: Cross-region failover is a high-impact, hard-to-reverse decision. False positives are expensive: promoting a DR database that's behind the primary loses data. The 5-minute auto-trigger is a safety net for when humans are unavailable, but under normal circumstances, a human should verify: Is it a real regional outage or a transient issue? Is the DR region healthy? What's the current replication lag?

**Q2: How do you handle "failback" — returning to the primary region after it recovers?**
A: Failback is essentially another failover, but planned. Steps: (1) Re-establish replication from DR (now primary) to original region. (2) Wait for original region to catch up (replication lag = 0). (3) Perform a planned failover back to the original region. (4) Scale down DR. (5) Update DNS. This is done during a maintenance window with full team readiness.

**Q3: What about global services that can't tolerate any cross-region latency?**
A: For latency-sensitive services, we use active-active with conflict resolution (CRDTs or last-writer-wins). Each region serves reads and writes independently. Conflicts are resolved asynchronously. This adds application complexity but eliminates cross-region failover latency. Example: session stores, user preferences.

**Q4: How do you test cross-region failover without causing a real outage?**
A: Regular DR drills (quarterly). Steps: (1) Route synthetic traffic (not real users) to the DR region. (2) Verify full stack works end-to-end. (3) Measure RTO (how long to activate). (4) Measure RPO (how much replication lag at promotion). (5) Failback. We also run "tabletop exercises" where the team walks through the runbook without executing it.

**Q5: What's the cost overhead of maintaining a warm standby DR region?**
A: Approximately 20-30% of primary region cost. Breakdown: compute is scaled down (1/10th), databases are running (full cost for replicas), network (cross-region replication bandwidth), storage (full copy). This is cheaper than hot standby (100% overhead) and much cheaper than an actual outage (revenue loss, reputation damage, SLA credits).

**Q6: How do you handle stateful services that use local storage in the primary region?**
A: Services using local storage (e.g., Elasticsearch data nodes with local SSDs) require data to be replicated to the DR region before failover. For Elasticsearch, we use Cross-Cluster Replication (CCR) which keeps a near-real-time copy in the DR region. For services that can't replicate in real-time, we rely on periodic snapshots to object storage (S3/GCS) and accept the RPO of the snapshot interval.

---

## 7. Scheduling & Resource Management

### Failover Resource Requirements

When a failover occurs, the target environment needs sufficient resources to absorb the failed workload.

**Capacity Planning for Failover:**
| Scenario | Resource Requirement | Strategy |
|---|---|---|
| Single node failure | Other nodes in cluster absorb load | Maintain 20% headroom per cluster |
| AZ failure (33% capacity lost) | Remaining 2 AZs absorb 50% more each | Provision each AZ to handle 150% of its normal load |
| Region failure | DR region absorbs 100% | DR pre-provisioned at 30-50%; auto-scales the rest |

**Kubernetes Scheduling During AZ Failover:**
- PodTopologySpreadConstraints ensure pods are spread across AZs.
- When an AZ fails, the kube-scheduler places evicted pods on nodes in surviving AZs.
- If surviving AZs are at capacity, lower-priority workloads are preempted via PriorityClasses.

**Database Replica Placement:**
- MySQL: 1 primary (AZ-1), 1 sync replica (AZ-2), 1 async replica (AZ-3).
- This ensures any single AZ failure has a replica available for immediate promotion.
- Cross-region replica in DR region for regional failover.

### Maintenance Windows and Failover

During planned maintenance, we pre-emptively fail over:
1. Pre-failover: Verify replica health and replication lag.
2. Planned failover: Graceful primary shutdown (drain connections, stop writes, let replicas catch up).
3. Maintenance: Perform maintenance on old primary.
4. Failback: After maintenance, reverse the failover.

This uses the same failover machinery but with relaxed timing (no urgency, full verification).

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Scale Unit |
|---|---|---|
| Failure Detection Service | 3 instances per region (fixed for consensus) | Add per-region as regions expand |
| Failover Orchestrator | 1 active + 2 standby per region (leader-elected) | Scale by adding managed cluster types |
| Heartbeat processors | Scale with Kafka partition count | 1 consumer per 100 managed members |
| ProxySQL | 1 per application cluster (or shared per service) | Scale with MySQL connection volume |
| DNS Failover Service | Singleton (global, leader-elected) | Not a throughput bottleneck |

### Database Scaling

**MySQL (the service the failover system manages):**
- Each MySQL cluster has 1 primary + 2 replicas (minimum for safe failover).
- Read scaling: add more async replicas (up to 10 per cluster).
- Write scaling: shard by tenant/service at the application level (vertical partitioning before horizontal sharding).

**Failover System's Own Database:**
- Small dataset (< 100 MB). Single MySQL instance with 1 replica is sufficient.
- The failover system should not share a database cluster with the systems it manages (avoid circular dependency).

### Caching

| Layer | Technology | Purpose | TTL |
|---|---|---|---|
| Cluster state cache | In-process (Java HashMap) | Avoid repeated etcd/MySQL reads during failover | 5s |
| Heartbeat history | Ring buffer (in-memory) | Fast access to recent heartbeats for Phi calculation | 5 min window |
| ProxySQL runtime | ProxySQL in-memory | Connection routing rules | Until explicitly changed |
| DNS cache | Client-side | DNS resolution | 30s (our TTL setting) |

### Interviewer Q&As

**Q1: What happens when you need to manage 10,000 MySQL clusters instead of 500?**
A: The failure detection service scales horizontally by adding more heartbeat consumers (partition Kafka by cluster_id). The failover orchestrator is sharded by cluster — each orchestrator instance manages a subset of clusters. etcd handles the increased lease count. ProxySQL is per-cluster already, so it scales linearly.

**Q2: How do you handle a failover that causes a resource crunch in the surviving AZs?**
A: Before executing a failover, the orchestrator checks the target environment's capacity: "If I promote replica-1, and clients move their traffic to AZ-2, does AZ-2 have enough capacity?" If not, it preemptively triggers scaling (HPA, add nodes) or alerts capacity planning. This check adds 2-3 seconds to the failover but prevents cascading failures.

**Q3: Can failover run out of resources (too many failovers)?**
A: Yes. We track "failover budget" per region: how many simultaneous failovers the control plane can handle (limited by etcd throughput, orchestrator capacity, and operator attention). Default budget: 10 simultaneous failovers. If exceeded, additional failovers queue by priority.

**Q4: How do you scale ProxySQL for a large MySQL cluster with 100K connections?**
A: ProxySQL supports connection multiplexing — 100K application connections can be served by 100 backend connections. For extreme scale, deploy a fleet of ProxySQL instances behind a TCP load balancer (L4). Each ProxySQL instance has the same routing configuration.

**Q5: What's the memory footprint of the Phi Accrual Failure Detector?**
A: Per managed member: 1 KB (sliding window of 1000 heartbeat intervals × 8 bytes each + statistical parameters). At 1,800 members: ~1.8 MB per detector instance. Negligible.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery | RTO |
|---|---|---|---|---|
| MySQL primary crash | Write outage for that cluster | Heartbeat timeout (9s) | Auto-failover: fence + promote + reroute | 18s |
| MySQL replica failure | Read capacity reduced | Heartbeat timeout | Add new replica from backup | 15 min |
| Kafka broker failure | Partition leaders shift | Controller detects missing broker | Kafka auto-reassigns partition leaders | < 30s |
| etcd node failure | Reduced consensus group | etcd cluster health check | etcd auto-promotes learner if available | < 15s |
| K8s API server (1 of 3) | No impact (LB routes to healthy) | LB health check | K8s deployment restarts pod | < 60s |
| AZ network partition | Split-brain risk for AZ-local primaries | Cross-AZ heartbeat failure | Fencing + failover to other AZ | 30s |
| Full AZ failure | 33% capacity lost | Multiple failures detected | Cross-AZ failover for all services | 1-5 min |
| Full region failure | 100% capacity lost | Global health check failure | Cross-region failover to DR | 5-15 min |
| Failover orchestrator crash | Cannot execute failovers | Leader election lost | Standby takes over | < 15s |
| ProxySQL crash | MySQL connections fail | LB health check on ProxySQL | LB routes to healthy ProxySQL instance | < 5s |
| DNS propagation delay | Some clients hit failed endpoint | Monitor DNS resolution from multiple PoPs | Wait for propagation; use LB as primary redirect | 30-60s |

### Automated Recovery

**Self-healing of the failover system:**
- Failover orchestrator: 3 replicas with leader election. Kubernetes Deployment ensures crashed instances are restarted.
- etcd: 5-node cluster. Tolerates 2 node failures. Automatic snapshot + restore.
- Failure detectors: 3 instances (1 per AZ). If 1 fails, consensus degrades to 2-of-2 (still functional).
- ProxySQL: Deployed as Kubernetes DaemonSet or Deployment with health checks. LB routes around unhealthy instances.

### Retry Strategy

| Operation | Retry Strategy | Max Retries | Notes |
|---|---|---|---|
| Heartbeat send | No retry (next heartbeat in 3s) | N/A | Missing 1 heartbeat is OK |
| Fencing (IPMI) | 3 retries, 5s delay | 3 | Must succeed; abort failover if all fail |
| MySQL CHANGE MASTER | 3 retries, 2s delay | 3 | Replica reconfiguration |
| ProxySQL update | 5 retries, 1s delay | 5 | Critical for traffic rerouting |
| DNS update | 3 retries, 10s delay | 3 | API rate limits possible |
| Cross-region activation | No automatic retry | 0 | Too dangerous — human decides |

### Circuit Breaker

**Failover Circuit Breaker:**
- Per managed cluster: if failover fails 3 times in 1 hour, circuit breaker opens.
- Prevents "failover flapping" where the system repeatedly tries and fails to fail over.
- When open: alert human, stop automatic failover for that cluster.
- Half-open after 30 minutes: allow 1 failover attempt.

### Consensus & Coordination

**etcd-based coordination:**
- Fencing tokens: monotonically increasing, stored in etcd. Survives orchestrator restarts.
- Leader election: Kubernetes Lease with 15s TTL for orchestrator; etcd native lease for failure detectors.
- Cluster state machine: stored in etcd. Atomic transitions via etcd transactions (compare-and-swap).

**Consistency guarantee:** The failover orchestrator uses etcd transactions to atomically update cluster state. Example: "If cluster state = FENCING and fencing_token = 42, set cluster state = PROMOTING." This prevents race conditions between orchestrator replicas during leader transitions.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold |
|---|---|---|
| `failover_detection_latency_sec` | Histogram | P99 > 15s |
| `failover_total_duration_sec` | Histogram | P99 > 60s (MySQL), > 5min (cross-region) |
| `failover_success_rate` | Gauge | < 95% |
| `failover_rpo_achieved_sec` | Histogram | > target RPO |
| `failover_rto_achieved_sec` | Histogram | > target RTO |
| `heartbeat_latency_ms` | Histogram | P99 > 50ms (intra-AZ) |
| `replication_lag_sec` | Gauge per cluster | > 5s (sync), > 60s (async) |
| `fencing_duration_sec` | Histogram | > 30s |
| `proxysql_connection_errors` | Counter | Spike during failover |
| `cluster_failover_state` | Gauge | Any cluster in "failed" state |
| `phi_accrual_value` | Gauge per member | > 8 (suspicious) |
| `failover_circuit_breaker_state` | Gauge per cluster | Open |

### Distributed Tracing

Each failover generates a trace with spans for:
1. Detection → Consensus → Fencing → Promotion → Rerouting → Verification.
2. Trace includes: latency per step, data loss estimate, replication lag at promotion time.
3. 100% trace sampling for failover events (they're rare and critical).

### Logging

**Structured logging for failover events:**
```json
{
  "level": "INFO",
  "service": "failover-orchestrator",
  "cluster_id": "mysql-orders-prod",
  "event": "failover_started",
  "old_primary": "mysql-orders-primary",
  "new_primary": "mysql-orders-replica-1",
  "detection_latency_ms": 9200,
  "replication_lag_at_detection_ms": 0,
  "fencing_method": "ipmi_power_off",
  "trace_id": "failover-abc123"
}
```

### Alerting

| Alert | Severity | Description |
|---|---|---|
| Failover completed | P3 (info) | Notify SRE of successful failover |
| Failover failed | P1 (page) | Human intervention required |
| Replication lag high | P2 (warn) | RPO at risk |
| Multiple failovers in progress | P2 (warn) | Possible common-cause failure |
| Fencing failed | P1 (page) | Split-brain risk |
| Circuit breaker open | P1 (page) | Cluster cannot auto-failover |
| DR region unhealthy | P2 (warn) | Regional failover capability at risk |

---

## 11. Security

### Auth & AuthZ

- **Failover orchestrator → MySQL:** Dedicated MySQL user with SUPER, REPLICATION SLAVE, REPLICATION CLIENT privileges. Password stored in Vault.
- **Failover orchestrator → IPMI:** Dedicated BMC user with power control permissions. Credentials in Vault.
- **Failover orchestrator → ProxySQL:** Admin user with RUNTIME permission. Password in Vault.
- **Failover orchestrator → etcd:** Client certificate from internal CA.
- **Human → Failover API:** OIDC + RBAC. Roles: viewer (see status), operator (trigger failover), admin (configure clusters).
- **Cross-region failover:** Requires 2-person approval (dual control). Both approvers' identities logged.

### Audit Logging

**Critical for failover because:**
1. Post-incident review: "When exactly did the failover happen? How much data was lost?"
2. Compliance: Financial services require proof that failover followed documented procedure.
3. Blame analysis: Was the failover triggered by automation or a human? Was it necessary?

**Every failover event is logged with:**
- Timestamp (millisecond precision)
- Cluster ID and type
- Old and new primary
- Trigger (automated vs manual, with human identity if manual)
- Each step's duration and result
- RPO achieved (replication lag at promotion)
- RTO achieved (total outage duration)
- Data loss estimate
- Approval chain (who approved, when)

**Logs are immutable:** Written to an append-only audit table in MySQL, replicated to a compliance data lake (S3 with object lock), and indexed in Elasticsearch.

---

## 12. Incremental Rollout Strategy

**Phase 1 (Week 1-4): Detection Only**
- Deploy failure detection service in observe mode.
- Monitor heartbeats and compute Phi values for all managed clusters.
- Validate detection accuracy: compare with known outages.
- Tune heartbeat intervals and Phi thresholds per cluster type.

**Phase 2 (Week 5-8): Automated Failover for Non-Production**
- Enable automated failover for staging/development MySQL clusters.
- Run 10+ test failovers per week. Measure RTO, RPO.
- Validate STONITH, ProxySQL updates, replication reconfiguration.

**Phase 3 (Week 9-12): Tier 3 Production**
- Enable automated failover for tier-3 production MySQL clusters (batch, analytics).
- Human approval required. Monitor for 30 days.
- Remove approval gate after 30 days of 100% success rate.

**Phase 4 (Week 13-20): Tier 1-2 Production**
- Enable automated failover for tier-1/2 MySQL, Kafka, K8s control plane.
- Human approval gate for tier-1. Auto-failover for tier-2.
- Monthly DR drills begin.

**Phase 5 (Week 21-30): Cross-Region Failover**
- Enable cross-region failover capability.
- Quarterly DR drills with actual traffic failover.
- Human approval always required.

### Rollout Q&As

**Q1: How do you build confidence in automated failover before enabling it in production?**
A: Metrics: (1) Detection accuracy in observe mode (zero false positives for 30 days). (2) Failover success rate in non-production (100% for 30 days). (3) RTO/RPO consistently within targets. (4) STONITH reliability (100% fencing success). Only after all metrics are green do we proceed to production.

**Q2: What's the rollback plan if automated failover causes a problem?**
A: Set `auto_failover_enabled=false` for the affected cluster. The system stops triggering failovers but continues monitoring. In-progress failovers are allowed to complete (safer than aborting mid-failover). If a failover completed incorrectly, we perform a manual failback.

**Q3: How do you handle the period where some clusters are auto-failover and others are manual?**
A: The failover orchestrator checks `auto_failover_enabled` per cluster before initiating. For manual clusters, it detects failures and alerts humans but doesn't act. Both modes use the same detection and monitoring infrastructure.

**Q4: How often should you test failover?**
A: Tier 3: weekly (low impact). Tier 2: monthly (moderate impact). Tier 1: quarterly (high impact, careful planning). Cross-region: quarterly (full DR drill). Each test is documented with pre/post checks.

**Q5: What's the biggest risk during the rollout?**
A: A false positive failover for a tier-1 MySQL cluster. Mitigation: consensus-based detection (2-of-3 detectors agree), human approval gate for tier-1, and the ability to abort failover at any step. The fencing step is the point of no return — once the primary is fenced, we must complete the failover.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Choice | Rationale | Risk | Mitigation |
|---|---|---|---|---|---|
| Failure detection | Single checker vs consensus | Consensus (2-of-3) | Eliminates false positives from network glitches | Adds 2s to detection | Acceptable for safety |
| Heartbeat interval | 1s vs 3s vs 10s | 3s (configurable per tier) | Balance between detection speed and network overhead | More traffic | Lightweight gRPC messages |
| Fencing method | STONITH vs network fence vs none | STONITH primary, network fence backup | STONITH is the gold standard for preventing split-brain | IPMI failure | Fallback to network fence |
| MySQL failover tool | Group Replication vs MHA vs Orchestrator vs custom | Custom (Orchestrator concepts + ProxySQL) | Fleet uses semi-sync replication, not group replication | Maintenance burden | Well-tested, battle-hardened code |
| RPO for semi-sync | 0 (wait for all) vs 1 (wait for 1) | Wait for 1 replica ACK | Balance between durability and write latency | 1 transaction loss in worst case | Acceptable for tier-2 |
| Cross-region mode | Hot vs warm vs cold standby | Warm (tier 1-2), Pilot light (tier 3) | Cost/benefit optimization | Higher RTO for pilot light | Acceptable for tier-3 |
| DNS TTL | 5s vs 30s vs 300s | 30s | Fast failover without excessive DNS query volume | Some clients cache beyond TTL | Use LB as additional redirect |
| Orchestrator HA | Active-passive vs active-active | Active-passive (leader-elected) | Simpler, avoids coordination complexity | 15s leader election on failure | Acceptable RTO |
| Approval for cross-region | Auto vs human | Human required (with 5-min auto-override) | Cross-region failover is high-impact, hard to reverse | Delayed RTO | 5-min auto-override for extended outages |

---

## 14. Agentic AI Integration

### AI-Enhanced Failure Detection

**Anomaly-Based Detection:**
Traditional failure detection relies on binary heartbeat timeouts. AI can detect subtle degradations that precede failures.

```python
class AIFailurePredictor:
    """Predicts failures 5-60 minutes before they happen."""
    
    def predict(self, member: ClusterMember) -> FailurePrediction:
        features = self.extract_features(member)
        # Features include:
        # - Heartbeat latency trend (increasing?)
        # - Replication lag trend (growing?)
        # - CPU/memory/disk usage trend
        # - Error log frequency
        # - Query latency percentiles
        # - Connection count trend
        
        probability = self.model.predict_proba(features)  # 0-1
        
        if probability > 0.8:
            return FailurePrediction(
                member=member,
                probability=probability,
                predicted_time_to_failure_sec=self.estimate_ttf(features),
                recommendation="PREEMPTIVE_FAILOVER",
                evidence=self.explain(features)
            )
        elif probability > 0.5:
            return FailurePrediction(
                recommendation="ALERT_AND_MONITOR",
                evidence=self.explain(features)
            )
        return None
```

**Use case:** If the AI predicts a MySQL primary will fail in 15 minutes (growing replication lag, increasing disk I/O latency), we can perform a graceful failover now — with zero data loss — rather than waiting for a crash that might lose data.

### AI-Powered Failover Decision Making

**Optimal Replica Selection:**
When multiple replicas are available, the AI selects the best one considering factors beyond simple replication lag:

```python
class AIReplicaSelector:
    def select_best_replica(self, cluster: ManagedCluster, candidates: List[ClusterMember]) -> ClusterMember:
        scored_candidates = []
        for candidate in candidates:
            score = 0.0
            # Replication lag (lower is better)
            score += (1.0 - normalize(candidate.replication_lag_ms, 0, 10000)) * 0.3
            # Hardware health (higher is better)
            score += candidate.hardware_health_score * 0.2
            # AZ diversity from failed primary (different AZ preferred)
            if candidate.az != cluster.primary_az:
                score += 0.2
            # Historical reliability (fewer past failures preferred)
            score += (1.0 - normalize(candidate.failure_count_90d, 0, 10)) * 0.15
            # Network latency to clients (lower is better)
            score += (1.0 - normalize(candidate.avg_client_latency_ms, 0, 50)) * 0.15
            
            scored_candidates.append((candidate, score))
        
        return max(scored_candidates, key=lambda x: x[1])[0]
```

### AI-Assisted Post-Failover Analysis

After every failover, an LLM agent analyzes the event and produces a post-mortem report:

```
Input: Failover event details, metrics, logs
Output: Post-mortem report including:
  - Root cause classification (hardware, software, network, human error)
  - Timeline of events
  - Was the failover necessary? (or was it a false positive?)
  - Was the RPO/RTO within target?
  - Recommendations for preventing similar failures
  - Confidence level in each finding
```

**Guard Rails:**
- AI analysis is advisory — it generates the report, but a human reviews it.
- AI does not modify failover configuration automatically.
- All AI-generated reports are clearly labeled as AI-generated.
- If AI confidence is low (< 0.6), the report says "insufficient data for automated analysis."

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the split-brain problem in database failover and how you prevent it.**
A: Split brain occurs when two nodes both believe they are the primary and accept writes independently. This causes data divergence that's extremely hard to reconcile. Prevention: (1) STONITH — physically power off the old primary before promoting a replica. (2) Fencing tokens — the new primary gets a higher token; the old primary's writes are rejected by clients/proxies that check the token. (3) Consensus — require majority agreement before declaring failure, preventing a single network partition from triggering false failover.

**Q2: What's the difference between RPO and RTO? Give concrete examples.**
A: **RPO (Recovery Point Objective):** How much data can you afford to lose. RPO=0 means no data loss (synchronous replication required). RPO=1s means up to 1 second of recent writes may be lost. **RTO (Recovery Time Objective):** How long can you be down. RTO=30s means the service should be available within 30 seconds. Example: A payments system might have RPO=0, RTO=15s (no data loss, fast recovery). An analytics pipeline might have RPO=1h, RTO=30min (hourly snapshots are fine, can wait for recovery).

**Q3: When would you choose active-active over active-passive failover?**
A: Active-active is better when: (1) Workload is read-heavy (multiple active replicas serve reads). (2) Latency matters across regions (users connect to nearest active). (3) RPO=0 is not required (active-active with async replication has nonzero RPO). Active-passive is better when: (1) Strong consistency is required (only one writer). (2) Workload is write-heavy. (3) Application can't handle conflict resolution.

**Q4: How do you handle a network partition where the primary is in the minority partition?**
A: The primary is in AZ-1, which is partitioned from AZ-2 and AZ-3. Detectors in AZ-2 and AZ-3 agree the primary is unreachable (2-of-3 consensus). Detectors initiate STONITH on the primary (via BMC, which may be on a different network path). If STONITH succeeds, promote the sync replica in AZ-2. If STONITH fails (BMC also partitioned), DO NOT promote — risk of split brain. Alert humans and wait for partition to heal.

**Q5: How does ProxySQL handle connections during a MySQL failover?**
A: ProxySQL maintains a pool of connections to MySQL backends. During failover: (1) Existing connections to the old primary receive errors (connection closed). (2) Applications retry (connection pool handles this). (3) The failover executor updates ProxySQL's routing table (admin API). (4) New connections go to the new primary. (5) Applications transparently reconnect within 1-2 seconds. ProxySQL's connection multiplexing means 100K application connections map to ~100 backend connections, so the reconnection overhead is small.

**Q6: What happens to in-flight transactions during a MySQL failover?**
A: Three cases: (1) Transaction committed on primary, ACKed by sync replica before primary crash: NO data loss, transaction is on the promoted replica. (2) Transaction committed on primary, NOT ACKed by sync replica: POSSIBLE data loss — transaction is in the primary's binlog but not on any replica. This is the semi-sync "gap" — one transaction at most. (3) Transaction in progress (not committed): LOST — rollback on client timeout, application must retry.

**Q7: How do you handle Kafka broker failure differently from MySQL primary failure?**
A: Kafka has built-in partition leader election via the Kafka controller (KRaft mode). When a broker fails: (1) Controller detects broker is offline. (2) For each partition whose leader was on the failed broker, the controller selects a new leader from the ISR (in-sync replicas). (3) Producers and consumers discover the new leader via metadata refresh. This is fully automated and built into Kafka — we don't need external failover orchestration. Our system monitors it and alerts if ISR drops below min.insync.replicas.

**Q8: How do you prevent "failover flapping" where the primary keeps bouncing between two nodes?**
A: (1) Cooldown period: after a failover completes, the cluster cannot fail over again for 5 minutes (configurable). (2) Circuit breaker: if 3 failovers happen within 1 hour, stop auto-failover and alert humans. (3) Root cause investigation: frequent failovers indicate an underlying problem (bad hardware, misconfiguration) that failover alone can't fix.

**Q9: What's the role of the witness/arbiter node in failover?**
A: A witness (or arbiter) is a lightweight node that participates in failure detection consensus but doesn't hold data. In a 2-AZ setup (which can't have true majority with 2 detectors), a witness in a third location provides the tie-breaking vote. Example: Primary in AZ-1, replica in AZ-2, witness in AZ-3. If AZ-1 is partitioned, AZ-2 + AZ-3 detectors form a majority and can safely fail over.

**Q10: How do you handle DNS-based failover for services using long-lived connections?**
A: DNS failover only helps new connections — existing long-lived connections (gRPC streams, WebSocket, database pools) don't re-resolve DNS. Solutions: (1) Connection timeout: force reconnection every N minutes (e.g., MySQL connection max lifetime in HikariCP). (2) LB-based failover: the LB detects backend failure and resets connections. (3) Service mesh (Envoy): sidecar proxy handles connection routing and retries transparently.

**Q11: How do you estimate data loss after a failover?**
A: At the moment of promotion, we record the GTID position of the new primary. We compare it with the last known GTID position of the old primary (from the last successful heartbeat). The difference represents potentially lost transactions. For semi-sync replication, this is typically 0-1 transactions. For async replication, it depends on the replication lag at the time of failure.

**Q12: What's the testing strategy for failover?**
A: Four levels: (1) Unit tests: mock components, verify state machine transitions. (2) Integration tests: real MySQL/Kafka/etcd in Docker, simulate failures via kill/stop. (3) Staging environment: full-scale failover tests with real traffic patterns. (4) Production: monthly "game days" where we intentionally trigger failover for non-critical clusters. After every test, we verify: RPO achieved, RTO achieved, no split brain, replication healthy.

**Q13: How do you handle a "zombie" primary that comes back after being fenced?**
A: Fencing should prevent this, but if it happens (e.g., STONITH was "soft" and the node reboots): (1) The zombie checks etcd and discovers it's no longer the primary. (2) It sets itself to read_only=ON. (3) It attempts to join as a replica of the new primary. (4) If it has "zombie writes" (transactions that occurred during the partition before fencing took effect), those transactions are logged for manual review but not applied to the new primary.

**Q14: What's your approach to testing cross-region failover?**
A: Quarterly DR drills. Process: (1) Announce the drill (no surprise for the first year). (2) Simulate regional failure by updating health checks to fail. (3) Execute the cross-region runbook. (4) Measure RTO (time to full DR activation). (5) Send synthetic traffic to DR region. (6) Verify all services are functional. (7) Failback to primary region. (8) Post-drill review: what went well, what was slow, what broke. After the first year, move to unannounced drills.

**Q15: How do you handle the "thundering herd" problem when traffic shifts to the DR region?**
A: DR region caches are cold. When traffic shifts: (1) All requests miss cache and hit the database. (2) Database gets overwhelmed. Mitigation: (1) Cache warming script runs during DR activation (pre-populate top-N queries). (2) Traffic is gradually shifted (10%, 25%, 50%, 100%) over 5 minutes rather than all at once. (3) Rate limiting on the DR endpoints during the first 10 minutes. (4) Auto-scaling is pre-configured to handle 100% traffic.

**Q16: In an interview, how would you decide between consistency and availability during a partition?**
A: It depends on the service. The CAP theorem says you can't have both during a partition. For a payments system: choose consistency (CP) — it's better to reject transactions than to process them with stale data and risk double-charging. For a social media feed: choose availability (AP) — showing slightly stale content is better than showing an error page. Our failover system is configurable per cluster: `failover_mode: safe` (consistency, require successful fencing before promotion) vs `failover_mode: fast` (availability, promote even without confirmed fencing, accept split-brain risk).

---

## 16. References

1. Kleppmann, M. "Designing Data-Intensive Applications" — Chapter 8 (Distributed Systems), Fencing Tokens — O'Reilly, 2017
2. MySQL Semi-Synchronous Replication: https://dev.mysql.com/doc/refman/8.0/en/replication-semisync.html
3. MySQL Orchestrator: https://github.com/openark/orchestrator
4. ProxySQL Documentation: https://proxysql.com/documentation/
5. Hayashibara, N., Defago, X., et al. "The Phi Accrual Failure Detector" — IEEE SRDS, 2004
6. Raft Consensus Algorithm: https://raft.github.io/
7. AWS Route 53 Health Checks and DNS Failover: https://docs.aws.amazon.com/Route53/latest/DeveloperGuide/dns-failover.html
8. Kafka Replication Protocol (ISR): https://kafka.apache.org/documentation/#replication
9. Google SRE Book — Chapter 26: Data Integrity — https://sre.google/sre-book/data-integrity/
10. STONITH (Shoot The Other Node In The Head) — Linux HA: https://wiki.clusterlabs.org/wiki/STONITH
11. Netflix Tech Blog — "Active-Active for Multi-Regional Resiliency" — https://netflixtechblog.com/
