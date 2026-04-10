# System Design: Rollout Controller

> **Relevance to role:** A cloud infrastructure platform engineer operating bare-metal IaaS and large VM fleets must design and operate rollout controllers that orchestrate software deployment across thousands of heterogeneous hosts. Unlike Kubernetes Deployments (which handle rolling updates automatically), bare-metal fleets require explicit wave-based rollouts with health-check gates, blast-radius controls, geographic batching, and operator overrides. Interviewers expect you to design a system that safely updates 10,000+ servers with automatic pause on error, manual override capability, idempotent re-apply, and tight integration with monitoring (Prometheus) for promotion decisions.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|-------------|
| FR-1 | Orchestrate rolling updates across thousands of bare-metal servers, VMs, or Kubernetes nodes. |
| FR-2 | Wave-based rollout: fleet divided into waves; each wave updates a configurable batch of hosts (e.g., max 5% per wave). |
| FR-3 | Health-check gate between waves: each wave must pass health checks before proceeding to the next. |
| FR-4 | Automated pause: error rate threshold triggers automatic pause. |
| FR-5 | Manual override: operator can pause, resume, rollback, skip hosts, and adjust wave size at runtime. |
| FR-6 | Idempotent rollout: safe to re-apply the same rollout (hosts already at target version are skipped). |
| FR-7 | Timeout: per-wave timeout and overall rollout timeout. |
| FR-8 | Blast-radius control: geographic batching (AZ-aware), rack-aware, service-group batching. |
| FR-9 | Integration with monitoring: Prometheus metrics drive promotion decisions between waves. |
| FR-10 | Support for Kubernetes custom controller (controller-runtime) for k8s node operations (OS upgrades, kubelet updates). |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Rollout start latency (command to first host updating) | < 30 s |
| NFR-2 | Wave transition time (health check + promote decision) | < 5 min |
| NFR-3 | Pause time (error detected to rollout halted) | < 30 s |
| NFR-4 | Rollback time (to previous version on affected hosts) | < 10 min (wave-by-wave reverse) |
| NFR-5 | Maximum blast radius per wave | configurable, default 5% |
| NFR-6 | System availability | 99.9% |
| NFR-7 | Fleet size supported | 50,000+ hosts |

### Constraints & Assumptions
- Hosts are registered in a fleet inventory system (CMDB) with metadata: hostname, AZ, rack, role, current version.
- Hosts run an agent daemon that accepts deployment commands via gRPC.
- Artifacts are pre-staged in a local artifact cache (not downloaded from central registry during rollout).
- Health checks are a combination of local host checks (agent reports) and global metrics (Prometheus).
- Rollouts are initiated by CI/CD pipelines or operators via CLI/API.

### Out of Scope
- Artifact build and publish (handled by CI/CD pipeline).
- Host provisioning and bare-metal OS installation.
- Kubernetes pod-level rolling updates (handled by k8s Deployment controller).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Total hosts managed | Given | 20,000 |
| Rollouts per day | 10 services x 2 deploys/day | 20 rollouts/day |
| Hosts per rollout (avg) | Service has ~500 hosts | 500 |
| Max hosts per rollout | Largest service | 5,000 |
| Waves per rollout (5% batch) | 500 / 25 (5% of 500) | 20 waves |
| Health checks per wave | 25 hosts x 5 checks each | 125 |
| Prometheus queries per wave | 5 global metric checks | 5 |
| Max concurrent rollouts | 3 services rolling out simultaneously | 3 |
| Agent heartbeats (all hosts) | 20,000 hosts x 1/10s | 2,000/s |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Agent command delivery | < 5 s |
| Agent health report | Every 10 s |
| Wave health check (local) | < 30 s (all hosts in wave report healthy) |
| Wave health check (Prometheus) | < 30 s (query + evaluation) |
| Pause propagation (error to halt) | < 30 s |
| Manual override (CLI to controller) | < 2 s |
| Full rollout (500 hosts, 20 waves) | ~60-90 min (including health check soak time) |

### Storage Estimates

| Item | Calculation | Value |
|------|-------------|-------|
| Rollout records | 20/day x 365 | 7,300/year |
| Wave records per rollout | ~20 waves | 146,000/year |
| Host-level status per rollout | 500 hosts x 20 rollouts/day x 365 | 3.65M/year |
| Record size | ~500 bytes | ~1.8 GB/year |
| Agent heartbeat logs | 2,000/s x 200 bytes x 86,400 | ~34 GB/day (sampled to 10%: 3.4 GB/day) |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| gRPC commands (deploy) | 500 hosts x 10 KB command | 5 MB per rollout |
| Agent heartbeats | 2,000/s x 200 bytes | 400 KB/s |
| Artifact distribution (pre-staged) | 500 hosts x 200 MB artifact (pre-staged) | 100 GB (but distributed via P2P/CDN before rollout starts) |
| Prometheus queries | 5 queries/wave x 20 waves x 20 rollouts/day | 2,000 queries/day |

---

## 3. High Level Architecture

```
                    +--------------------+
                    |    Operator / CI   |
                    |    (CLI or API)    |
                    +--------+-----------+
                             |
                        Create rollout
                             |
                    +--------v-----------+
                    |  Rollout Controller |
                    |  (Go service with  |
                    |   state machine)   |
                    +--+-----+------+---+
                       |     |      |
          +------------+     |      +-------------+
          v                  v                    v
  +-------+------+   +------+-------+    +-------+------+
  | Wave Planner |   | Health Gate  |    | Override     |
  | (computes    |   | (Prometheus  |    | Handler      |
  |  wave         |   |  + agent    |    | (pause/      |
  |  batches)     |   |  health)   |    |  resume/     |
  +-+------+------+   +------+------+    |  rollback)  |
    |      |                 |           +-------------+
    |      |                 |
    v      v                 v
+---+--+ +-+----+    +------+-------+
| Fleet| | Rack |    | Prometheus   |
| Inv. | | Topo |    | (Metrics)    |
+------+ +------+    +--------------+
                              
                    +--------+-----------+
                    |  Agent Manager     |
                    |  (gRPC to all      |
                    |   host agents)     |
                    +--------+-----------+
                             |
              gRPC commands  |  gRPC health reports
                             |
         +-------------------+-------------------+
         |           |           |               |
    +----v---+  +----v---+  +----v---+      +----v---+
    | Agent  |  | Agent  |  | Agent  |      | Agent  |
    | Host-1 |  | Host-2 |  | Host-3 | ...  | Host-N |
    +--------+  +--------+  +--------+      +--------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **Rollout Controller** | Core state machine. Manages the rollout lifecycle: plan waves, execute wave-by-wave, evaluate health gates, handle overrides, record status. Implemented in Go with the controller pattern. |
| **Wave Planner** | Divides the fleet into ordered waves based on: batch size, AZ distribution, rack topology, service criticality. Ensures blast radius is bounded per wave. |
| **Health Gate** | Between each wave, queries Prometheus for global metrics (error rate, latency) and waits for agent health reports from the just-updated hosts. Passes/fails the wave. |
| **Override Handler** | Accepts operator commands at any time: pause, resume, rollback, skip host, adjust wave size, force-promote. All overrides are audited. |
| **Fleet Inventory** | CMDB/database of all hosts with metadata: hostname, IP, AZ, rack, role, current version, health status. |
| **Rack Topology** | Physical topology: which hosts are in which rack, which racks are in which AZ. Used for blast-radius control. |
| **Agent Manager** | Manages gRPC connections to all host agents. Sends deploy commands, receives health reports. Connection pooling and fan-out. |
| **Host Agent** | Daemon on each bare-metal host. Accepts deploy commands (download artifact, stop old process, start new process, run local health check). Reports health status every 10 seconds. |

### Data Flows

1. **Initiate:** Operator/CI calls `POST /api/v1/rollouts` with target version, fleet selector, wave config.
2. **Plan:** Wave Planner queries Fleet Inventory + Rack Topology, divides hosts into waves (AZ-balanced, rack-distributed, max 5% per wave).
3. **Execute Wave 1:** Agent Manager sends gRPC `Deploy(version, artifact_url)` to all hosts in wave 1.
4. **Local Health:** Agents execute local health checks (port open, process running, `/health` returns 200). Report status via gRPC heartbeat.
5. **Health Gate:** Controller waits for all agents in wave 1 to report healthy (timeout: 5 min). Then queries Prometheus for global metrics (error rate, p99 latency). If within thresholds -> promote.
6. **Promote:** Controller advances to wave 2. Repeat steps 3-5.
7. **Auto-Pause:** If Prometheus metrics breach thresholds (error rate > 1%), Controller pauses. Sends alert. Operator investigates.
8. **Resume/Rollback:** Operator decides to resume (issue was transient) or rollback (issue is real). Rollback: Controller sends `Deploy(previous_version)` to all already-updated hosts.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Fleet inventory (hosts)
CREATE TABLE hosts (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    hostname        VARCHAR(255) NOT NULL UNIQUE,
    ip_address      VARCHAR(45) NOT NULL,
    availability_zone VARCHAR(20) NOT NULL,             -- e.g., "us-east-1a"
    rack_id         VARCHAR(50) NOT NULL,               -- e.g., "rack-42"
    role            VARCHAR(100) NOT NULL,               -- e.g., "order-svc"
    current_version VARCHAR(128),                        -- currently deployed version
    target_version  VARCHAR(128),                        -- desired version (set by rollout)
    health_status   ENUM('healthy', 'degraded', 'unhealthy', 'unknown', 'draining') NOT NULL DEFAULT 'unknown',
    last_heartbeat  DATETIME,
    tags            JSON,                                -- {"gpu": true, "memory_gb": 256}
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_role_version (role, current_version),
    INDEX idx_az_rack (availability_zone, rack_id),
    INDEX idx_health (health_status)
) ENGINE=InnoDB;

-- Rollout definition
CREATE TABLE rollouts (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    name            VARCHAR(255) NOT NULL,               -- e.g., "order-svc-v1.2.4-20260409"
    service_role    VARCHAR(100) NOT NULL,               -- target role in fleet inventory
    target_version  VARCHAR(128) NOT NULL,
    previous_version VARCHAR(128),                       -- for rollback reference
    artifact_url    VARCHAR(1024) NOT NULL,
    status          ENUM('planned', 'in_progress', 'paused', 'completed', 'rolled_back',
                         'failed', 'cancelled') NOT NULL DEFAULT 'planned',
    
    -- Wave configuration
    wave_size_percent INT NOT NULL DEFAULT 5,            -- max % of fleet per wave
    wave_size_max     INT NOT NULL DEFAULT 50,           -- absolute max hosts per wave
    health_check_timeout_s INT NOT NULL DEFAULT 300,     -- per-wave health check timeout
    soak_time_s       INT NOT NULL DEFAULT 120,          -- min time between waves
    rollout_timeout_s INT NOT NULL DEFAULT 7200,         -- overall timeout (2 hours)
    
    -- Error thresholds for auto-pause
    error_rate_threshold DOUBLE NOT NULL DEFAULT 0.01,   -- 1%
    latency_p99_threshold_ms INT NOT NULL DEFAULT 500,
    max_failed_hosts_per_wave INT NOT NULL DEFAULT 2,    -- pause if > 2 hosts fail in a wave
    
    -- Metadata
    triggered_by    VARCHAR(255) NOT NULL,
    pipeline_run_id BIGINT,
    started_at      DATETIME,
    paused_at       DATETIME,
    finished_at     DATETIME,
    current_wave    INT NOT NULL DEFAULT 0,
    total_waves     INT,
    total_hosts     INT,
    hosts_completed INT NOT NULL DEFAULT 0,
    hosts_failed    INT NOT NULL DEFAULT 0,
    
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_service_status (service_role, status),
    INDEX idx_created (created_at)
) ENGINE=InnoDB;

-- Wave within a rollout
CREATE TABLE rollout_waves (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    rollout_id      BIGINT NOT NULL,
    wave_number     INT NOT NULL,
    status          ENUM('pending', 'deploying', 'health_checking', 'soaking', 
                         'promoted', 'failed', 'rolled_back', 'skipped') NOT NULL DEFAULT 'pending',
    host_count      INT NOT NULL,
    hosts_succeeded INT NOT NULL DEFAULT 0,
    hosts_failed    INT NOT NULL DEFAULT 0,
    started_at      DATETIME,
    health_check_at DATETIME,
    promoted_at     DATETIME,
    finished_at     DATETIME,
    
    -- Health gate results
    agent_health_passed BOOLEAN,
    prometheus_check_passed BOOLEAN,
    prometheus_error_rate DOUBLE,
    prometheus_p99_latency_ms DOUBLE,
    
    FOREIGN KEY (rollout_id) REFERENCES rollouts(id),
    INDEX idx_rollout_wave (rollout_id, wave_number)
) ENGINE=InnoDB;

-- Per-host status within a wave
CREATE TABLE wave_hosts (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    wave_id         BIGINT NOT NULL,
    host_id         BIGINT NOT NULL,
    hostname        VARCHAR(255) NOT NULL,
    status          ENUM('pending', 'deploying', 'deployed', 'health_checking',
                         'healthy', 'failed', 'timed_out', 'skipped', 'rolled_back') NOT NULL DEFAULT 'pending',
    previous_version VARCHAR(128),
    error_message   TEXT,
    deploy_started_at DATETIME,
    deploy_finished_at DATETIME,
    health_check_at DATETIME,
    FOREIGN KEY (wave_id) REFERENCES rollout_waves(id),
    INDEX idx_wave_status (wave_id, status),
    INDEX idx_host (host_id)
) ENGINE=InnoDB;

-- Operator override audit log
CREATE TABLE rollout_overrides (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    rollout_id      BIGINT NOT NULL,
    action          ENUM('pause', 'resume', 'rollback', 'skip_host', 'adjust_wave_size',
                         'force_promote', 'cancel', 'extend_timeout') NOT NULL,
    parameters      JSON,                                -- action-specific parameters
    reason          TEXT NOT NULL,
    performed_by    VARCHAR(255) NOT NULL,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (rollout_id) REFERENCES rollouts(id),
    INDEX idx_rollout (rollout_id)
) ENGINE=InnoDB;
```

### Database Selection

| Store | Engine | Rationale |
|-------|--------|-----------|
| Rollout state, wave state, host state | MySQL 8.0 | Transactional state machine; moderate write volume (~2,000 host updates/rollout). |
| Fleet inventory | MySQL 8.0 | Single source of truth for host metadata. Shared with other infra systems. |
| Agent heartbeats (recent) | Redis | High-throughput writes (2,000/s). TTL-based expiry (stale heartbeats auto-expire). |
| Metrics for health gate | Prometheus | Native metric storage; PromQL for threshold checks. |
| Audit log (long-term) | Elasticsearch | Searchable override and rollout history. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| hosts | (role, current_version) | "Which hosts for service X are on version Y?" |
| hosts | (availability_zone, rack_id) | Wave planning: distribute across AZs and racks |
| rollouts | (service_role, status) | "Is there an active rollout for service X?" |
| rollout_waves | (rollout_id, wave_number) | Wave progression within a rollout |
| wave_hosts | (wave_id, status) | "How many hosts in wave 3 are healthy?" |

---

## 5. API Design

### REST Endpoints

```
# Rollout management
POST   /api/v1/rollouts                               # Create and start a rollout
GET    /api/v1/rollouts                                # List rollouts (filterable by service, status)
GET    /api/v1/rollouts/{id}                           # Get rollout details + waves + hosts
GET    /api/v1/rollouts/{id}/waves                     # Get all waves for a rollout
GET    /api/v1/rollouts/{id}/waves/{waveNum}            # Get specific wave details + host status

# Operator overrides
POST   /api/v1/rollouts/{id}/pause                     # Pause rollout
       Body: { "reason": "Investigating error spike" }
POST   /api/v1/rollouts/{id}/resume                    # Resume paused rollout
POST   /api/v1/rollouts/{id}/rollback                  # Rollback all updated hosts to previous version
POST   /api/v1/rollouts/{id}/cancel                    # Cancel rollout (no rollback, keep current state)
POST   /api/v1/rollouts/{id}/force-promote              # Force promote current wave (skip health check)
       Body: { "reason": "Known benign metric spike" }
POST   /api/v1/rollouts/{id}/skip-host                  # Skip a problematic host
       Body: { "hostname": "host-42", "reason": "Hardware issue" }
PUT    /api/v1/rollouts/{id}/wave-config                # Adjust wave size at runtime
       Body: { "wave_size_percent": 10, "soak_time_s": 60 }

# Fleet inventory
GET    /api/v1/hosts?role={role}&az={az}                # Query hosts
GET    /api/v1/hosts/{hostname}                          # Get host details
GET    /api/v1/hosts/{hostname}/history                   # Deployment history for a host

# Health
GET    /api/v1/rollouts/{id}/health                      # Current health gate status
```

**Example: Create a rollout**
```json
POST /api/v1/rollouts
{
  "name": "order-svc-v1.2.4",
  "service_role": "order-svc",
  "target_version": "v1.2.4",
  "artifact_url": "s3://artifacts/order-svc/v1.2.4/order-svc.tar.gz",
  "wave_size_percent": 5,
  "wave_size_max": 25,
  "health_check_timeout_s": 300,
  "soak_time_s": 120,
  "error_rate_threshold": 0.01,
  "latency_p99_threshold_ms": 500,
  "blast_radius_config": {
    "max_hosts_per_az": "33%",
    "max_hosts_per_rack": 2,
    "canary_hosts": ["host-001", "host-002"]
  }
}
```

### CLI

```bash
# Start rollout
rollctl start --service order-svc --version v1.2.4 \
    --artifact s3://artifacts/order-svc/v1.2.4/order-svc.tar.gz \
    --wave-size 5% --soak-time 2m

# Watch rollout progress (real-time)
rollctl watch 12345

# Status
rollctl status 12345

# Operator overrides
rollctl pause 12345 --reason "Investigating metric anomaly"
rollctl resume 12345
rollctl rollback 12345 --reason "Confirmed regression in new version"
rollctl cancel 12345 --reason "Superseded by newer version"
rollctl force-promote 12345 --reason "Known benign metric spike from batch job"
rollctl skip-host 12345 --host host-42 --reason "Hardware failure unrelated to deployment"
rollctl adjust 12345 --wave-size 10% --soak-time 1m

# History
rollctl history --service order-svc --limit 20
rollctl host-history host-42 --limit 10

# Dry run (show planned waves without executing)
rollctl dry-run --service order-svc --version v1.2.4 --wave-size 5%
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Wave Planning Algorithm

**Why it's hard:**
Dividing 5,000 hosts into waves isn't just about batch size. Each wave must be balanced across availability zones (losing an entire AZ's hosts in one wave is catastrophic), racks (correlated hardware failures), and service criticality (canary hosts first). The planner must also handle heterogeneous fleets (some hosts have GPU, different memory) and host exclusions (hosts under maintenance).

**Approaches:**

| Approach | AZ-Aware | Rack-Aware | Canary Support | Complexity |
|----------|----------|------------|----------------|------------|
| **Simple sequential (first N hosts)** | No | No | No | Low |
| **Random shuffle then batch** | Probabilistic | Probabilistic | No | Low |
| **Round-robin across AZs** | Yes | No | No | Medium |
| **Constraint-based optimization** | Yes | Yes | Yes | High |
| **Topology-aware with canary + constraints** | Yes | Yes | Yes | Medium-High |

**Selected approach: Topology-aware wave planning with canary wave and constraints**

**Implementation detail:**

```go
// Wave Planner (Go)

type WavePlan struct {
    Waves []Wave
}

type Wave struct {
    WaveNumber int
    Hosts      []Host
    WaveType   string // "canary", "early", "main", "final"
}

type Host struct {
    Hostname string
    AZ       string
    RackID   string
    Role     string
    Version  string
}

type WaveConfig struct {
    WaveSizePercent   int
    WaveSizeMax       int
    MaxHostsPerAZ     float64 // fraction: 0.33 = max 33% of hosts in one AZ per wave
    MaxHostsPerRack   int     // absolute: max 2 hosts from same rack per wave
    CanaryHosts       []string // specific hosts for first wave
}

func PlanWaves(hosts []Host, config WaveConfig) WavePlan {
    var plan WavePlan
    
    // Filter: skip hosts already at target version (idempotent)
    pendingHosts := filterPending(hosts)
    if len(pendingHosts) == 0 {
        return plan // nothing to do
    }
    
    totalHosts := len(pendingHosts)
    waveSize := min(
        int(math.Ceil(float64(totalHosts) * float64(config.WaveSizePercent) / 100.0)),
        config.WaveSizeMax,
    )
    
    // Wave 0: Canary wave (specific hosts or smallest possible batch)
    canaryWave := buildCanaryWave(pendingHosts, config.CanaryHosts)
    plan.Waves = append(plan.Waves, canaryWave)
    remaining := removeHosts(pendingHosts, canaryWave.Hosts)
    
    // Build topology index
    azIndex := groupByAZ(remaining)
    rackIndex := groupByRack(remaining)
    
    // Main waves: topology-aware round-robin
    waveNum := 1
    for len(remaining) > 0 {
        wave := buildTopologyAwareWave(remaining, waveSize, config, azIndex, rackIndex, waveNum)
        plan.Waves = append(plan.Waves, wave)
        remaining = removeHosts(remaining, wave.Hosts)
        
        // Rebuild indexes
        azIndex = groupByAZ(remaining)
        rackIndex = groupByRack(remaining)
        waveNum++
    }
    
    return plan
}

func buildTopologyAwareWave(
    hosts []Host, 
    targetSize int, 
    config WaveConfig,
    azIndex map[string][]Host,
    rackIndex map[string][]Host,
    waveNum int,
) Wave {
    var selected []Host
    azCount := make(map[string]int)
    rackCount := make(map[string]int)
    
    // Sort AZs for deterministic ordering
    azs := sortedKeys(azIndex)
    
    // Round-robin across AZs
    for len(selected) < targetSize {
        added := false
        for _, az := range azs {
            if len(selected) >= targetSize {
                break
            }
            
            // Check AZ constraint
            maxForAZ := int(math.Ceil(float64(len(azIndex[az])) * config.MaxHostsPerAZ))
            if azCount[az] >= maxForAZ {
                continue
            }
            
            // Find a host in this AZ that doesn't violate rack constraint
            for _, host := range azIndex[az] {
                if containsHost(selected, host) {
                    continue
                }
                if rackCount[host.RackID] >= config.MaxHostsPerRack {
                    continue
                }
                
                selected = append(selected, host)
                azCount[az]++
                rackCount[host.RackID]++
                added = true
                break
            }
        }
        
        if !added {
            break // can't add more hosts without violating constraints
        }
    }
    
    waveType := "main"
    if waveNum <= 2 {
        waveType = "early"
    }
    
    return Wave{
        WaveNumber: waveNum,
        Hosts:      selected,
        WaveType:   waveType,
    }
}
```

**Example wave plan output (500 hosts, 3 AZs, 5% waves):**
```
Rollout Plan: order-svc v1.2.4
Total hosts: 500 (167 us-east-1a, 167 us-east-1b, 166 us-east-1c)
Wave size: 25 hosts (5%)

Wave  0 [canary ]: 2 hosts  (1a:1, 1b:1, 1c:0) - specific canary hosts
Wave  1 [early  ]: 25 hosts (1a:9, 1b:8, 1c:8) - max 2/rack
Wave  2 [early  ]: 25 hosts (1a:8, 1b:9, 1c:8)
Wave  3 [main   ]: 25 hosts (1a:8, 1b:8, 1c:9)
...
Wave 19 [main   ]: 23 hosts (1a:8, 1b:8, 1c:7) - remainder
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Fleet inventory stale (host decommissioned) | Deploy command to dead host | Agent Manager timeout (30s); host marked `failed`; wave continues if within failure threshold |
| Constraint unsatisfiable (all remaining hosts in one rack) | Wave can't be built within constraints | Relax constraints with warning; log the relaxation |
| Canary host is unhealthy before rollout | Wave 0 stuck | Skip unhealthy canary hosts; substitute with another host from same AZ |

**Interviewer Q&As:**

**Q1: Why not just randomly shuffle hosts and batch?**
A: Random shuffle has no topology awareness. You might get a wave where all 25 hosts are in the same AZ, and if the deploy fails, that AZ loses 15% of capacity. Topology-aware planning bounds the blast radius: no AZ loses more than 33% of its hosts per wave, and no rack loses more than 2 hosts. This is critical for services that need AZ-balanced capacity for redundancy.

**Q2: How do you handle a fleet with uneven AZ distribution (100 hosts in AZ-a, 300 in AZ-b, 100 in AZ-c)?**
A: The `MaxHostsPerAZ` constraint is relative (33% of that AZ's hosts per wave). For AZ-b with 300 hosts, that's 100 hosts max per wave. The wave planner round-robins across AZs proportionally. Early waves pull more from larger AZs. The result is that all AZs complete rollout at roughly the same time.

**Q3: What if a host is under maintenance and shouldn't be updated?**
A: Hosts tagged `maintenance=true` in the fleet inventory are excluded from the wave plan. The planner filters them out before planning. After maintenance, the host's version may be stale; the next rollout will update it (idempotent: host is behind target version, so it's included in a wave).

**Q4: How do you handle the canary wave specially?**
A: The canary wave is the first wave with 1-2 hosts. These are pre-selected hosts in production that are designated as canaries (they may have extra monitoring, or they serve a subset of traffic). The canary wave has a longer soak time (10 min vs. 2 min for main waves) and stricter health checks. If the canary wave fails, the rollout is paused immediately without affecting any other hosts.

**Q5: How do you handle adding hosts to the fleet mid-rollout?**
A: New hosts registered in the fleet inventory with the service role and a version != target are automatically included in future waves. The wave planner re-evaluates remaining hosts before each wave. This makes the rollout self-adjusting. New hosts are added to the end of the wave queue.

**Q6: What's the maximum wave size and why?**
A: Default max is 5% of fleet per wave. For a 5,000-host fleet, that's 250 hosts. The absolute cap is configurable (`wave_size_max`). Larger waves are faster but riskier. For the first rollout of a major version, we recommend 1-2% waves. For routine patches, 10% is acceptable. The wave size is a risk/speed dial that the operator controls.

---

### Deep Dive 2: Health Gate and Automated Promotion

**Why it's hard:**
The health gate must answer: "Is the wave of hosts that just updated healthy?" This requires combining local host checks (process running, port listening, local health endpoint) with global metrics (overall service error rate, latency). The challenge is distinguishing between wave-caused regressions and background noise (natural metric variance, unrelated incidents).

**Approaches:**

| Approach | Accuracy | Speed | Complexity |
|----------|----------|-------|------------|
| **Agent health check only** | Catches crashes, startup failures | Fast (30s) | Low |
| **Prometheus global metrics only** | Catches regressions | Slower (5 min for significance) | Medium |
| **Agent health + Prometheus (combined)** | Comprehensive | Medium (2-5 min) | Medium |
| **Statistical comparison (canary analysis)** | Highest accuracy | Slow (10+ min) | High |

**Selected approach: Two-phase health gate (agent check first, then Prometheus soak)**

**Implementation detail:**

```go
// Health Gate implementation

type HealthGateResult struct {
    Passed          bool
    AgentCheckPassed bool
    MetricsCheckPassed bool
    FailedHosts     []string
    ErrorRate       float64
    P99LatencyMs    float64
    Details         string
}

func (hg *HealthGate) EvaluateWave(ctx context.Context, rollout *Rollout, wave *Wave) HealthGateResult {
    result := HealthGateResult{}
    
    // Phase 1: Wait for all hosts in the wave to report healthy
    agentResult := hg.waitForAgentHealth(ctx, wave, rollout.HealthCheckTimeoutS)
    result.AgentCheckPassed = agentResult.AllHealthy
    result.FailedHosts = agentResult.FailedHosts
    
    if !agentResult.AllHealthy {
        // Check if failures exceed threshold
        if len(agentResult.FailedHosts) > rollout.MaxFailedHostsPerWave {
            result.Passed = false
            result.Details = fmt.Sprintf(
                "%d hosts failed health check (threshold: %d): %v",
                len(agentResult.FailedHosts),
                rollout.MaxFailedHostsPerWave,
                agentResult.FailedHosts,
            )
            return result
        }
        // Some hosts failed but within threshold, continue
        log.Warnf("Wave %d: %d hosts failed but within threshold", wave.WaveNumber, len(agentResult.FailedHosts))
    }
    
    // Phase 2: Soak time + Prometheus metrics check
    log.Infof("Wave %d: soaking for %ds", wave.WaveNumber, rollout.SoakTimeS)
    select {
    case <-time.After(time.Duration(rollout.SoakTimeS) * time.Second):
        // Soak complete
    case <-ctx.Done():
        result.Passed = false
        result.Details = "Rollout cancelled during soak"
        return result
    }
    
    // Query Prometheus
    metricsResult := hg.evaluatePrometheusMetrics(rollout)
    result.MetricsCheckPassed = metricsResult.Passed
    result.ErrorRate = metricsResult.ErrorRate
    result.P99LatencyMs = metricsResult.P99LatencyMs
    
    if !metricsResult.Passed {
        result.Passed = false
        result.Details = fmt.Sprintf(
            "Prometheus check failed: error_rate=%.4f (threshold: %.4f), p99=%.1fms (threshold: %dms)",
            metricsResult.ErrorRate, rollout.ErrorRateThreshold,
            metricsResult.P99LatencyMs, rollout.LatencyP99ThresholdMs,
        )
        return result
    }
    
    result.Passed = true
    result.Details = fmt.Sprintf(
        "Wave %d healthy: %d/%d hosts ok, error_rate=%.4f, p99=%.1fms",
        wave.WaveNumber, wave.HostCount - len(agentResult.FailedHosts), wave.HostCount,
        metricsResult.ErrorRate, metricsResult.P99LatencyMs,
    )
    return result
}

func (hg *HealthGate) waitForAgentHealth(ctx context.Context, wave *Wave, timeoutS int) AgentHealthResult {
    deadline := time.Now().Add(time.Duration(timeoutS) * time.Second)
    healthyHosts := make(map[string]bool)
    failedHosts := []string{}
    
    for time.Now().Before(deadline) {
        allDone := true
        for _, host := range wave.Hosts {
            if healthyHosts[host.Hostname] {
                continue
            }
            
            status := hg.agentManager.GetHostStatus(host.Hostname)
            switch status {
            case "healthy":
                healthyHosts[host.Hostname] = true
            case "failed":
                failedHosts = append(failedHosts, host.Hostname)
            default:
                allDone = false // still waiting
            }
        }
        
        if allDone {
            break
        }
        
        select {
        case <-time.After(5 * time.Second):
            // poll again
        case <-ctx.Done():
            return AgentHealthResult{AllHealthy: false, FailedHosts: failedHosts}
        }
    }
    
    // Hosts that haven't reported are timed out
    for _, host := range wave.Hosts {
        if !healthyHosts[host.Hostname] && !contains(failedHosts, host.Hostname) {
            failedHosts = append(failedHosts, host.Hostname)
        }
    }
    
    return AgentHealthResult{
        AllHealthy: len(failedHosts) == 0,
        FailedHosts: failedHosts,
    }
}

func (hg *HealthGate) evaluatePrometheusMetrics(rollout *Rollout) MetricsResult {
    // Query error rate
    errorRateQuery := fmt.Sprintf(
        `sum(rate(http_requests_total{service="%s",status=~"5.."}[2m])) / `+
        `sum(rate(http_requests_total{service="%s"}[2m]))`,
        rollout.ServiceRole, rollout.ServiceRole,
    )
    errorRate := hg.promClient.Query(errorRateQuery)
    
    // Query p99 latency
    latencyQuery := fmt.Sprintf(
        `histogram_quantile(0.99, sum(rate(http_request_duration_seconds_bucket{service="%s"}[2m])) by (le))`,
        rollout.ServiceRole,
    )
    p99Latency := hg.promClient.Query(latencyQuery) * 1000 // seconds to ms
    
    passed := errorRate <= rollout.ErrorRateThreshold &&
              p99Latency <= float64(rollout.LatencyP99ThresholdMs)
    
    return MetricsResult{
        Passed:      passed,
        ErrorRate:   errorRate,
        P99LatencyMs: p99Latency,
    }
}
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Agent heartbeat delayed (network congestion) | Host appears unhealthy, wave stuck | Timeout with tolerance: if 23/25 hosts healthy and 2 timed out, proceed if within threshold |
| Prometheus query returns no data | Can't evaluate metrics | Mark as `inconclusive`; if 3 consecutive inconclusive, pause rollout and alert |
| Background noise causes false metric breach | Rollout pauses unnecessarily | Use relative comparison (current vs. pre-rollout baseline) alongside absolute thresholds. Allow `force-promote` for known benign spikes. |
| Health check endpoint has a bug | All hosts report unhealthy | Host health check has multiple signals: (1) process running, (2) port open, (3) HTTP /health, (4) basic smoke test. Single-signal failure doesn't fail the host. |

**Interviewer Q&As:**

**Q1: How do you distinguish between a regression caused by the rollout vs. an unrelated incident?**
A: Compare metrics before the rollout started (baseline) vs. after each wave. If the error rate was already elevated before the rollout, it's likely an existing issue. We store the pre-rollout metric snapshot and compare against it, not against a fixed threshold. Additionally, if the error increase correlates exactly with the wave completion time, it's likely rollout-related.

**Q2: What if the service has no HTTP traffic (it's a Kafka consumer)?**
A: Custom health metrics. For a Kafka consumer: (1) Consumer lag (should not increase), (2) Message processing error rate, (3) Consumer group rebalance events (should not spike). These custom metrics are defined in the rollout configuration and queried via the same PromQL mechanism.

**Q3: How long should the soak time be between waves?**
A: Depends on the service and the wave. Canary wave: 10 minutes (high caution). Early waves (1-3): 5 minutes. Main waves: 2 minutes. Late waves (> 80% done): 1 minute. This is configurable per rollout. The soak time allows for: JVM warmup, cache warming, delayed error manifestation (e.g., a bug that only triggers every 5 minutes in a batch job).

**Q4: How do you handle a host that is healthy at health-check time but degrades 10 minutes later?**
A: Post-wave monitoring continues throughout the entire rollout. The controller's background loop checks global Prometheus metrics every 30 seconds. If at any point (not just during the wave's soak) the metrics breach thresholds, the rollout pauses. This catches delayed degradation.

**Q5: What if an operator force-promotes and it causes an outage?**
A: The `force-promote` action requires an explicit reason (recorded in audit log) and an elevated RBAC role. It sends a PagerDuty alert to the on-call engineer. If the operator force-promotes and an outage follows, the audit trail links the override to the outage. Post-incident review examines whether the force-promote was justified.

**Q6: How do you handle health checks for a service that takes 5 minutes to warm up?**
A: The `health_check_timeout_s` is configurable per rollout. For JVM services with large heap warmup or Spring Boot initialization, set the timeout to 600s (10 min). The agent's local health check transitions from `starting` -> `warming` -> `healthy`. The controller waits for `healthy`, not just `deployed`. Additionally, we pre-warm the JVM by sending a trickle of traffic before marking the host as healthy.

---

### Deep Dive 3: Rollback Orchestration

**Why it's hard:**
Rollback must undo the damage quickly: re-deploy the previous version to all already-updated hosts while the remaining hosts stay on the old version. The rollback itself must be safe (what if the rollback fails?), idempotent (safe to retry), and fast (minimize outage duration).

**Approaches:**

| Approach | Speed | Safety | Complexity |
|----------|-------|--------|------------|
| **Reverse wave-by-wave (same wave plan in reverse)** | Slow (~30 min for 500 hosts) | High (same health checks) | Low |
| **All-at-once (deploy previous version to all updated hosts simultaneously)** | Fast (5 min) | Lower (no health checks) | Medium |
| **Priority rollback (highest-impact hosts first)** | Medium | High | Medium |
| **LB drain + rollback (remove updated hosts from LB, then rollback in background)** | Fastest (30s traffic impact) | High | High |

**Selected approach: LB drain + parallel rollback**

**Justification:** For maximum speed, we immediately drain all updated hosts from the load balancer (traffic goes to non-updated hosts, which are still on the working old version). Then we rollback updated hosts in parallel (no waves needed since they're already drained). Once rollback is complete, hosts are re-added to the LB.

**Implementation detail:**

```go
func (rc *RolloutController) Rollback(ctx context.Context, rollout *Rollout, reason string) error {
    log.Infof("Rolling back %s: %s", rollout.Name, reason)
    
    // Record override
    rc.recordOverride(rollout, "rollback", reason)
    
    // Find all hosts that were updated in this rollout
    updatedHosts := rc.findUpdatedHosts(rollout)
    if len(updatedHosts) == 0 {
        log.Info("No hosts to rollback")
        return nil
    }
    
    // Phase 1: Drain updated hosts from load balancer (immediate)
    log.Infof("Draining %d hosts from load balancer", len(updatedHosts))
    for _, host := range updatedHosts {
        if err := rc.lbManager.DrainHost(host.Hostname); err != nil {
            log.Warnf("Failed to drain %s: %v", host.Hostname, err)
        }
    }
    
    // Wait for in-flight requests to complete (grace period)
    time.Sleep(30 * time.Second)
    
    // Phase 2: Deploy previous version to all updated hosts (parallel)
    log.Infof("Deploying previous version %s to %d hosts", rollout.PreviousVersion, len(updatedHosts))
    
    var wg sync.WaitGroup
    results := make(chan HostRollbackResult, len(updatedHosts))
    
    // Limit concurrency (don't overwhelm the artifact cache)
    semaphore := make(chan struct{}, 50) // max 50 concurrent deploys
    
    for _, host := range updatedHosts {
        wg.Add(1)
        go func(h Host) {
            defer wg.Done()
            semaphore <- struct{}{}
            defer func() { <-semaphore }()
            
            err := rc.agentManager.Deploy(ctx, h.Hostname, DeployCommand{
                Version:     rollout.PreviousVersion,
                ArtifactURL: rollout.PreviousArtifactURL,
            })
            
            results <- HostRollbackResult{
                Hostname: h.Hostname,
                Success:  err == nil,
                Error:    err,
            }
        }(host)
    }
    
    // Wait for all rollbacks to complete
    go func() { wg.Wait(); close(results) }()
    
    succeeded := 0
    failed := 0
    for r := range results {
        if r.Success {
            succeeded++
            // Re-add to load balancer
            rc.lbManager.EnableHost(r.Hostname)
        } else {
            failed++
            log.Errorf("Rollback failed for %s: %v", r.Hostname, r.Error)
        }
    }
    
    log.Infof("Rollback complete: %d/%d succeeded, %d failed", succeeded, len(updatedHosts), failed)
    
    // Update rollout status
    rollout.Status = "rolled_back"
    rollout.FinishedAt = time.Now()
    rc.db.Save(rollout)
    
    // Alert if any hosts failed rollback
    if failed > 0 {
        rc.alertManager.Alert("rollback_partial_failure", fmt.Sprintf(
            "Rollback of %s: %d hosts failed to rollback. Manual intervention needed.",
            rollout.Name, failed,
        ))
    }
    
    return nil
}
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| LB drain fails for some hosts | Some traffic still goes to broken hosts | Health checks remove unhealthy hosts from LB automatically (LB health check = 3 failures = remove) |
| Previous artifact not available | Can't rollback | Artifact retention policy: always keep previous 5 versions in local cache on each host |
| Host agent unreachable during rollback | Host stuck on new (broken) version | LB keeps host drained. Manual intervention required. Flag host for repair. |
| Rollback takes too long (slow deploy) | Extended partial outage | Parallel rollback with high concurrency (50 hosts at a time). Pre-staged artifact reduces deploy time to < 30s per host. |
| Double rollback (operator triggers rollback during a rollback) | Confusing state | Mutex on rollout: only one operation at a time. Second rollback request returns "rollback already in progress." |

**Interviewer Q&As:**

**Q1: Why drain from the LB before rolling back? Why not just rollback in-place?**
A: In-place rollback (stop old, start previous) has a window where the host serves no traffic (process restart takes 5-30s). During this window, requests to that host fail. By draining first, we ensure no traffic goes to these hosts during the transition. The remaining (non-updated) hosts absorb the load. This reduces user-facing errors to near zero.

**Q2: What if the remaining hosts can't handle the full load (capacity issue)?**
A: This is a risk when a large percentage of hosts have been updated (e.g., 60% updated, rollback drains 60%). Mitigation: (1) Rollback in reverse-waves: un-drain the most recently updated hosts first as they're rolled back. (2) Horizontal scaling: if on Kubernetes or cloud VMs, auto-scale the non-updated hosts before draining updated ones. (3) For bare-metal (can't auto-scale), ensure the wave plan never updates more than 50% of hosts before a checkpoint (configurable).

**Q3: How do you handle a service where the old version can't handle traffic from the new version's schema?**
A: This is the same database compatibility issue as blue-green. The expand-contract pattern ensures the old version works with the new schema. If a "contract" migration was run (old column dropped), rollback to the old version would fail because the old code references the dropped column. This is why contract migrations are never run in the same deploy -- they happen in a subsequent deploy after the new version is fully stable.

**Q4: How fast can you rollback 500 hosts?**
A: LB drain: 30 seconds (parallel drain + 30s grace). Parallel rollback: with 50 concurrent deploys and 30s per deploy, 500 hosts / 50 concurrency = 10 batches x 30s = 5 minutes. Re-add to LB: 30 seconds. Total: ~6 minutes. The user-facing impact is limited to the 30-second drain period (during which the remaining ~250 non-updated hosts serve all traffic at ~2x load).

**Q5: What if an operator accidentally triggers a rollback on a healthy rollout?**
A: The rollback command requires a `--reason` flag and confirmation (`--confirm`). For completed rollouts (past bake period), rollback requires an elevated RBAC role. The audit log records who triggered the rollback and why. If the rollback was accidental, the operator can immediately trigger a new rollout to re-deploy the target version.

**Q6: How do you handle rollback for a firmware update on bare-metal servers?**
A: Firmware updates are often not rollback-safe (BIOS, BMC firmware). For these, the rollout wave size is smaller (1% or single host) with longer soak times (1 hour). Rollback for firmware involves power cycling with the previous firmware image, which requires IPMI/BMC control. The rollout controller has a firmware-specific rollback path that uses ipmitool to set the boot device to the previous firmware partition.

---

## 7. Scheduling & Resource Management

### Rollout Scheduling

| Aspect | Strategy |
|--------|----------|
| Concurrent rollouts | Max 1 active rollout per service. Max 3 active rollouts across all services (prevents cascading risk). |
| Priority | Production hotfixes preempt other rollouts (hotfix rollout pauses any in-progress rollout for the same service). |
| Maintenance windows | Optional: rollouts can be restricted to maintenance windows (e.g., Tue-Thu 10am-2pm). Emergency rollouts bypass windows. |
| Agent connection pooling | Agent Manager maintains persistent gRPC connections to all hosts. Connection pool: 20,000 persistent connections with keepalive every 30s. |
| Artifact pre-staging | Before a rollout starts, the artifact is pre-distributed to all target hosts via BitTorrent or CDN. Rollout waits for artifact availability confirmation. |

### Resource Usage

| Component | Resource | Notes |
|-----------|----------|-------|
| Rollout Controller | 4 CPU, 8 Gi memory | Stateful: manages rollout state machines. 3 replicas with leader election. |
| Agent Manager | 8 CPU, 16 Gi memory | 20K gRPC connections consume memory. 3 replicas for HA. |
| Host Agent | 0.5 CPU, 256 Mi memory | Lightweight daemon on each host. Minimal overhead. |
| Prometheus queries | ~5 queries per wave per rollout | Negligible load on Prometheus. |

---

## 8. Scaling Strategy

| Challenge | Solution |
|-----------|----------|
| 50,000 hosts | Agent Manager sharded by AZ. Each shard handles ~17K connections. Rollout Controller delegates to the appropriate shard. |
| 10 concurrent rollouts | Each rollout has independent state in MySQL. Controller uses goroutines per rollout. Lock per-service prevents conflicts. |
| Agent Manager bottleneck | Fan-out via multiple Agent Manager instances. Each host connects to one shard (consistent hashing on hostname). |
| Artifact distribution for large fleet | P2P distribution (BitTorrent-style): first 50 hosts download from S3, then peer-share. 50,000 hosts seeding = massive aggregate bandwidth. |
| Prometheus load (many rollouts querying) | Recording rules pre-compute per-service error rate and p99. Rollout queries the pre-computed metrics, not raw data. |

### Interviewer Q&As

**Q1: How do you scale from 20,000 to 100,000 hosts?**
A: (1) Shard Agent Manager by AZ or region (each shard handles ~20K connections). (2) Rollout Controller coordinates across shards via API calls. (3) MySQL handles the metadata easily (100K host records is small). (4) Agent heartbeats move from centralized Redis to per-shard Redis instances. (5) Artifact distribution uses P2P (BitTorrent) to scale independently of fleet size.

**Q2: How do you handle a rollout across 50,000 hosts with 1% waves?**
A: 1% = 500 hosts per wave, 100 waves total. At 2-minute soak per wave, that's ~200 minutes (~3.3 hours). For faster rollout, increase wave size to 5% (50 hosts/wave, 20 waves, ~40 min). The trade-off: larger waves = faster but higher blast radius. For well-tested patches, 5-10% is acceptable. For major version upgrades, 1% is prudent.

**Q3: What if an agent is unreachable on a host?**
A: The Agent Manager marks the host as `unreachable` after 3 missed heartbeats (30s). Unreachable hosts are skipped during wave planning (not included in any wave). After the rollout, a background reconciler attempts to reach skipped hosts and applies the update. If the host remains unreachable for > 24 hours, it's flagged for investigation (possible hardware failure).

**Q4: How do you handle rollouts across multiple regions?**
A: Region-sequential rollout: Region 1 completes fully, then Region 2 starts. Each region has its own Rollout Controller and Agent Manager instances. A global orchestrator triggers per-region rollouts and gates between regions (Region 1 must be healthy for 30 min before Region 2 starts).

**Q5: How do you handle heterogeneous hosts (different hardware configurations)?**
A: Host tags in the fleet inventory (`{"gpu": true, "memory_gb": 256}`). The rollout's fleet selector can filter by tags: "only hosts with gpu=true." Wave planning respects hardware groups: GPU hosts and non-GPU hosts are in separate waves (different failure profiles).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Impact | Detection | Mitigation | Recovery Time |
|---|---------|--------|-----------|------------|---------------|
| 1 | Rollout Controller crash | Rollout paused (no advancement) | Pod restart; leader election | New leader reads state from MySQL, resumes from current wave | < 30 s |
| 2 | Agent Manager crash | Can't send commands or receive health | Connection drop detection | Agent reconnects to another Manager instance (consistent hashing resharding) | < 30 s |
| 3 | Host agent crash during deploy | Host stuck in `deploying` state | No heartbeat received | Host times out; marked `failed`; wave checks failure threshold | < 5 min |
| 4 | Artifact cache miss on host | Deploy fails (can't find artifact) | Agent reports error | Retry: agent downloads artifact from S3 directly. If S3 also fails, mark host failed. | < 2 min (retry) |
| 5 | Prometheus unavailable | Can't evaluate global health gate | Query timeout | Mark metric check as `inconclusive`. After 3 inconclusive checks, pause rollout and alert. | Controller resumes when Prometheus recovers |
| 6 | MySQL down | Can't persist state | Connection error | MySQL Multi-AZ failover. Controller caches current wave state in memory and retries writes. | < 30 s (failover) |
| 7 | Network partition between Controller and AZ | Can't reach hosts in one AZ | Agent heartbeat gap for all hosts in AZ | Wave planning skips hosts in partitioned AZ. Rollout continues in reachable AZs. | Partition heals |
| 8 | All hosts in a wave fail | Wave fails | agent health check failures exceed threshold | Rollout auto-paused. Alert: "Wave X failed: all hosts unhealthy." Operator investigates. | Manual |
| 9 | Operator makes mistake (rollback when shouldn't) | Healthy version rolled back | Audit log shows override | Re-deploy the target version. Rollout is idempotent (hosts already at previous version are updated again). | < 10 min (re-rollout) |
| 10 | Cascading rollout failure (new version causes downstream outage) | Service X's rollout breaks service Y | Service Y's error rate spikes (detected by Y's monitoring) | Inter-service health checks: rollout checks downstream service health too. Global pause trigger. | < 5 min |

### Idempotency Guarantee

The rollout controller is idempotent:
- If a host is already at the target version, it's skipped.
- If a rollout is re-started (controller crash), it resumes from the current wave (not the beginning).
- If a deploy command is sent twice to the same host, the agent checks "am I already at this version?" and returns success immediately.
- If a wave is re-evaluated (controller restart during health check), the same Prometheus queries return the same results.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Source |
|--------|------|-----------------|--------|
| `rollout_wave_number` | Gauge per rollout | N/A (progress tracking) | Controller |
| `rollout_status` | Gauge per rollout | `failed` or `paused` > 30 min | Controller |
| `rollout_wave_duration_seconds` | Histogram | p95 > 600 s | Controller |
| `rollout_hosts_completed` | Counter per rollout | N/A (progress) | Controller |
| `rollout_hosts_failed` | Counter per rollout | > max_failed_hosts_per_wave * wave_count | Controller |
| `rollout_auto_pause_count` | Counter | > 0 (investigate) | Controller |
| `agent_heartbeat_lag_seconds` | Histogram | p99 > 30 s | Agent Manager |
| `agent_unreachable_count` | Gauge | > 50 (fleet issue) | Agent Manager |
| `deploy_duration_seconds` | Histogram per host | p95 > 120 s | Agent |
| `health_check_duration_seconds` | Histogram per wave | p95 > 300 s | Controller |
| `prometheus_query_errors` | Counter | > 5/hour | Controller |
| `override_count` | Counter by action | > 5/day (unusual) | Controller |

### Dashboards
1. **Rollout Progress:** Wave-by-wave visualization, host status heat map (by AZ/rack), percent complete.
2. **Fleet Health:** Host version distribution, unreachable hosts, deployment history per host.
3. **Rollout Analytics:** Average rollout duration, failure rate, auto-pause frequency, override frequency.
4. **Agent Health:** Heartbeat lag distribution, unreachable hosts by AZ, agent version distribution.

---

## 11. Security

| Control | Implementation |
|---------|---------------|
| mTLS (agent <-> manager) | Host agent authenticates to Agent Manager using per-host client certificates (issued during provisioning). Agent Manager certificate signed by internal CA. |
| Command signing | Deploy commands are signed by the Rollout Controller's private key. Agent verifies signature before executing. Prevents rogue command injection. |
| Artifact integrity | Agent verifies artifact SHA-256 digest before applying. Digest is included in the signed deploy command. |
| RBAC | `rollout:create` for CI/CD pipelines. `rollout:override` for operators. `rollout:rollback` for on-call. `rollout:force-promote` requires senior engineer approval. |
| Audit logging | Every rollout action, override, and host-level event recorded with identity and timestamp. |
| Network isolation | Agent Manager accessible only from the Rollout Controller (network policy). Agents accessible only from the Agent Manager. No direct access to agents from developer laptops. |
| Rate limiting | Max 3 concurrent rollouts. Max 1 per service. Prevents accidental mass deployment. |

---

## 12. Incremental Rollout Strategy

### Rolling Out the Rollout Controller Itself

This is meta: rolling out the system that rolls things out. Extra caution is required.

**Phase 1: Shadow mode (Week 1-2)**
- New rollout controller runs alongside the existing deployment mechanism (Ansible/Chef/manual).
- Both systems target the same fleet. New controller calculates wave plans but doesn't execute.
- Compare: wave plan quality, health check accuracy, timing.

**Phase 2: Canary service (Week 3-4)**
- One low-risk service (e.g., internal tooling with 50 hosts) uses the new controller exclusively.
- Validate end-to-end: rollout, health checks, pause, resume, rollback.
- Run a deliberate rollback drill.

**Phase 3: Expanding (Week 5-8)**
- 5 additional services adopt the new controller.
- Include a large service (500+ hosts) to validate at scale.
- Validate topology-aware wave planning with real rack data.

**Phase 4: Full migration (Month 3+)**
- All services migrated. Old mechanism decommissioned.
- Run quarterly rollback drills for critical services.

### Rollout Q&As

**Q1: How do you roll out a new version of the host agent to 20,000 hosts?**
A: Dog-food: use the rollout controller to roll out the agent update. The current agent version accepts a deploy command for the new agent binary. The deploy process: (1) Download new agent binary. (2) Gracefully shut down old agent (drain active commands). (3) Start new agent. (4) New agent registers with Agent Manager. Health check: Agent Manager verifies the new agent responds to health RPC. Wave size: 1% (200 hosts) with 10-minute soak time.

**Q2: What happens if the rollout controller is down and a service needs an emergency deploy?**
A: Emergency fallback: operators can deploy directly via the host agent's CLI (`agent deploy --version v1.2.5 --artifact s3://...`). This is a break-glass procedure: bypasses wave planning and health checks but gets the fix deployed. An Ansible playbook wraps this for multi-host deployment. The break-glass action is audited and triggers a review.

**Q3: How do you test the rollout controller without risking production?**
A: (1) Unit tests for wave planning algorithm (constraint satisfaction). (2) Integration tests against a mock fleet (100 fake hosts). (3) Staging environment with 50 real bare-metal hosts for end-to-end testing. (4) Chaos tests: kill hosts mid-rollout, partition networks, simulate Prometheus failures. (5) Monthly rollback drills on production (deploy a known-good version, rollback, verify).

**Q4: How do you handle the transition period when some services use the new controller and others use the old mechanism?**
A: Fleet inventory has a `deployment_method` field per service (`legacy`, `rollout_controller`). The CI/CD pipeline checks this field and routes to the appropriate deployment system. Both systems read from the same fleet inventory. No service is deployed by both systems simultaneously.

**Q5: How do you validate that the wave planning algorithm produces correct plans?**
A: (1) Constraint validation: after planning, verify that no wave exceeds AZ/rack limits. (2) Coverage validation: every host appears in exactly one wave. (3) Property-based testing (QuickCheck/Hypothesis): generate random fleet topologies and verify constraints hold for all generated plans. (4) Visual inspection: `rollctl dry-run` outputs the wave plan for operator review before execution.

---

## 13. Trade-offs & Decision Log

| Decision | Option Chosen | Alternative | Rationale |
|----------|---------------|-------------|-----------|
| Communication protocol (controller <-> agent) | gRPC (bidirectional streaming) | SSH, REST, Ansible | gRPC provides streaming health reports, low-latency commands, and strong typing (proto). SSH is slow to establish. REST lacks streaming. Ansible doesn't support real-time health reporting. |
| State storage | MySQL | etcd, CRD (k8s) | Rollout state is complex (nested: rollout -> waves -> hosts). MySQL's relational model maps cleanly. etcd is limited to key-value. CRDs work for k8s but our controller manages bare-metal hosts. |
| Health gate design | Two-phase (agent local + Prometheus global) | Agent only, Prometheus only | Agent checks catch host-level failures (crash, startup error). Prometheus catches service-level regressions (error rate, latency). Both are needed. |
| Rollback strategy | LB drain + parallel rollback | Reverse wave-by-wave, all-at-once | LB drain minimizes user-facing errors (traffic shifts to healthy hosts immediately). Parallel rollback is fast (no need for waves since hosts are drained). |
| Wave planning | Topology-aware round-robin | Random, sequential | Topology-aware bounds blast radius per AZ and rack. Critical for maintaining service redundancy during rollout. |
| Artifact distribution | Pre-staged via P2P/CDN before rollout | Download during rollout | Pre-staging eliminates download time during the rollout (deploy takes 10s instead of 60s). P2P scales independently of fleet size. |
| Operator overrides | Real-time via API/CLI | Only via rollout config restart | Operators need immediate control during incidents. CLI commands take effect in < 2s. Restarting rollout config would lose state. |
| Concurrency limit | Max 3 concurrent rollouts | No limit | Limits blast radius: at most 3 services can be destabilized simultaneously. Prevents cascading failures from correlated deployments. |

---

## 14. Agentic AI Integration

### AI-Powered Rollout Intelligence

| Use Case | Implementation |
|----------|---------------|
| **Wave size recommendation** | Agent analyzes historical rollout data for the service: past success rates, error rates per wave size. Recommends optimal wave size for risk tolerance. "Based on 50 past rollouts, order-svc can safely use 10% waves (zero rollbacks at this size)." |
| **Anomaly detection during soak** | Agent monitors 50+ metrics during soak time and detects subtle anomalies that fixed thresholds miss. Uses isolation forest / LSTM model trained on past rollouts. "Detected unusual CPU usage pattern on hosts in wave 3 -- different from baseline." |
| **Predictive rollback** | Agent predicts whether a rollout will need to be rolled back based on early wave metrics. "Wave 1 shows 0.5% error rate (within threshold) but the trend line projects 1.5% by wave 5. Recommend pausing." |
| **Root cause analysis on failure** | When a rollout is paused, agent analyzes: logs from failed hosts, recent config changes, dependent service health. "Rollout paused due to error rate spike. Root cause: database connection pool exhaustion (new version uses 20% more connections). Recommendation: increase DB connection pool or reduce batch size." |
| **Optimal rollout timing** | Agent analyzes traffic patterns and recommends deployment windows. "order-svc has minimum traffic at 3am UTC Tuesdays. Scheduling rollout during this window reduces risk by 40%." |
| **Fleet health scoring** | Before rollout, agent scores each host's health (0-100) based on recent metrics, hardware events, and maintenance history. "Host-42 has a health score of 45 (recent disk errors). Recommend excluding from early waves." |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through a rollout of a new version to 1,000 bare-metal hosts.**
A: (1) Operator or CI runs `rollctl start --service order-svc --version v1.2.4 --wave-size 5%`. (2) Controller queries fleet inventory: 1,000 hosts with role=order-svc, current_version != v1.2.4. (3) Wave planner divides into ~20 waves of 50 hosts each, AZ-balanced, rack-distributed. (4) Wave 0 (canary): 2 designated canary hosts. Deploy, health check, soak 10 min. (5) Waves 1-19: for each wave, send gRPC deploy command to 50 hosts, wait for agent health reports (5 min timeout), query Prometheus (error rate < 1%, p99 < 500ms), soak 2 min. (6) If any wave fails health gate: auto-pause, alert operator. (7) Operator investigates, then resumes or rolls back. (8) After all waves complete, rollout marked `completed`. Total time: ~60-90 min.

**Q2: How do you handle a host where the deploy partially completes (e.g., old process killed but new process fails to start)?**
A: The host agent implements atomic deploy: (1) Download new artifact. (2) Verify checksum. (3) Start new process (health check against new process). (4) If new process healthy, stop old process. (5) If new process fails, keep old process running (no interruption). This is a "blue-green on one host" pattern. If atomic deploy fails, the agent reports `failed` and the old version continues serving.

**Q3: How do you handle a bare-metal fleet where hosts reboot during the rollout?**
A: The agent daemon starts on boot (systemd service). On startup, it checks its target version (persisted to disk) and current running version. If they differ, it self-deploys to the target version. When the host comes back online and reports healthy, the Controller reconciles it as "completed" for its wave.

**Q4: What's the biggest risk when rolling out to bare-metal vs. Kubernetes?**
A: Bare-metal has no automatic rescheduling. If a host fails, its capacity is lost until manually repaired. In Kubernetes, a failed pod is rescheduled to a healthy node in seconds. This means bare-metal rollouts must be more conservative: smaller waves, longer soak times, and always maintain enough healthy capacity to handle traffic if the wave fails.

**Q5: How do you handle a service that spans both Kubernetes pods and bare-metal hosts?**
A: Split into two rollout domains: (1) Kubernetes pods managed by Argo Rollouts (canary/blue-green). (2) Bare-metal hosts managed by the Rollout Controller. A meta-orchestrator sequences them: k8s canary first (lower risk, faster rollback), then bare-metal rollout. Both are gated: k8s canary must succeed before bare-metal rollout starts.

**Q6: How do you ensure the artifact is available on all hosts before the rollout starts?**
A: Pre-staging: before creating the rollout, the CI pipeline triggers an artifact distribution job. This pushes the artifact to a CDN or P2P network. Each host's agent downloads the artifact and reports "artifact ready." The rollout creation API checks: "Are > 95% of target hosts artifact-ready?" If not, it waits or warns the operator.

**Q7: How do you handle rolling out an OS kernel update to bare-metal hosts?**
A: Kernel updates require a reboot. The rollout agent: (1) Downloads new kernel package. (2) Installs kernel (grub update). (3) Drains the host from LB. (4) Reboots. (5) On boot: agent starts, verifies kernel version, runs health checks, reports to Controller. (6) Controller adds host back to LB. Soak time for kernel updates: 30+ min (to catch subtle instabilities). Wave size: 1%.

**Q8: How do you implement the Kubernetes custom controller pattern for node-level rollouts?**
A: Use controller-runtime (Go) to build a k8s operator. Define a `NodeRollout` CRD. The controller watches this CRD and orchestrates node-level operations (kubelet upgrade, OS patch) by: (1) Cordoning the node. (2) Draining pods (with PDB respect). (3) Executing the update (via SSH or DaemonSet job). (4) Uncordoning. (5) Verifying node status. Waves are defined by node labels (AZ, node pool).

**Q9: How do you handle rollout of a change that requires both a config change and a binary update?**
A: Two-phase deploy: (1) Push config change to all hosts (via agent's config update command). Config changes are backward-compatible with the current version. (2) Roll out the new binary. This ensures that when the new binary starts, the config is already in place. Alternatively, the new binary reads config from a central store (Consul/etcd) with versioning, and the binary knows which config version it needs.

**Q10: How do you handle the case where 5% of hosts consistently fail health checks?**
A: After a wave, if exactly the same hosts keep failing (hardware issue, not software), the operator skips them (`rollctl skip-host`). The rollout continues for the remaining 95% of hosts. Skipped hosts are flagged for hardware investigation. After repair, they're included in the next rollout (or a reconciliation pass).

**Q11: How do you prevent two teams from rolling out conflicting changes to the same fleet simultaneously?**
A: Lock per service: only one active rollout per `service_role`. If team A starts a rollout for order-svc, team B's rollout request is rejected with "Rollout already in progress for order-svc." The global concurrent rollout limit (3) prevents cascading risk across services.

**Q12: How do you handle a zero-downtime rollout for a stateful service (e.g., MySQL, Kafka broker)?**
A: Stateful services require special handling: (1) Kafka: rolling restart one broker at a time, wait for under-replicated partitions to reach 0. (2) MySQL: rolling restart replicas first, then failover primary. The rollout controller has service-specific plugins: `KafkaRolloutPlugin`, `MySQLRolloutPlugin` that implement the lifecycle (drain, update, rejoin, verify) for each service type.

**Q13: How do you handle the cold-start problem when a newly deployed host takes 5 minutes to warm up?**
A: The LB adds the host back gradually: weight starts at 10% and ramps to 100% over 5 minutes (LB weighted ramp). The agent reports `warming` status for 5 minutes before `healthy`. The health gate accepts `warming` as a non-failure state but doesn't fully count the host as healthy until it reaches `healthy`.

**Q14: How do you measure the success of a rollout system?**
A: (1) Rollout success rate (target: > 98% of rollouts complete without manual intervention). (2) Mean time to rollout (target: < 90 min for 1,000 hosts). (3) Rollback frequency (target: < 5% of rollouts). (4) Blast radius of failures (target: no outage affecting > 5% of capacity). (5) Operator override frequency (lower = more trusted automation).

**Q15: What is the most critical piece of this system to get right?**
A: The health gate. If the health gate has false negatives (passes a bad wave), the rollout continues deploying broken code to more hosts. If it has false positives (fails a good wave), rollouts are annoying but not dangerous. We bias heavily toward false positives (safety > speed). The health gate is the system's immune system.

---

## 16. References

- Kubernetes controller-runtime: https://github.com/kubernetes-sigs/controller-runtime
- Google SRE: Managing Critical State: https://sre.google/sre-book/managing-critical-state/
- Facebook Tupperware (Container Management): https://research.fb.com/publications/twine-a-unified-cluster-management-system-for-shared-infrastructure/
- LinkedIn Deployer: https://engineering.linkedin.com/blog/2017/10/scalable-and-reliable-deployment
- Argo Rollouts: https://argoproj.github.io/argo-rollouts/
- Prometheus PromQL: https://prometheus.io/docs/prometheus/latest/querying/basics/
- gRPC Go: https://grpc.io/docs/languages/go/
- HashiCorp Serf (Membership + Failure Detection): https://www.serf.io/
- BitTorrent for Software Distribution: https://engineering.fb.com/2016/05/04/production-engineering/swarming-over-the-challenges-of-managing-content-at-scale/
