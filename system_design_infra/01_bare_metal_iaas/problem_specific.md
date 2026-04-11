# Problem-Specific Notes — Bare Metal & IaaS (01_bare_metal_iaas)

## bare_metal_reservation_platform

### Unique Purpose
Reserve bare-metal machines (including 256-GPU H100 clusters) by hardware spec and time window. Prevent double-booking via hybrid interval tree + pessimistic locking. Lifecycle: requested → active → released. 50K machines, 500 req/sec peak.

### Unique Architecture
- **Interval tree** (in-memory, per machine type): O(log n) conflict detection before MySQL lock; augmented BST with `maxEnd` for range pruning
- **Topology-aware placement**: for bulk GPU reservations, considers `rack_id` and `gpu_interconnect` (NVLink vs PCIe) to co-locate machines on same rack/switch
- **Preemption**: higher-priority reservation can preempt lower; configurable grace period; tenant notified via Kafka event
- **Waitlist**: auto-assigns from waitlist when capacity frees; FIFO within same priority; `expires_at` on waitlist entries

### Key Schema
```sql
CREATE TABLE reservations (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  reservation_uuid CHAR(36) UNIQUE NOT NULL,
  idempotency_key  VARCHAR(128) UNIQUE NOT NULL,
  tenant_id        BIGINT UNSIGNED NOT NULL,
  requester_id     BIGINT UNSIGNED NOT NULL,
  status           ENUM('requested','pending_approval','confirmed','provisioning',
                        'active','draining','released','cancelled','preempted','failed') NOT NULL,
  priority         TINYINT UNSIGNED NOT NULL DEFAULT 5,  -- 1=highest
  machine_type     VARCHAR(64) NOT NULL,
  requested_count  INT UNSIGNED NOT NULL,
  start_time       DATETIME NOT NULL,
  end_time         DATETIME NOT NULL,
  actual_start_time DATETIME,
  actual_end_time   DATETIME,
  version          INT UNSIGNED NOT NULL DEFAULT 1,  -- optimistic locking
  created_at       DATETIME(6) NOT NULL,
  updated_at       DATETIME(6) NOT NULL,
  INDEX idx_tenant_status (tenant_id, status),
  INDEX idx_machine_type_time (machine_type, start_time, end_time),
  INDEX idx_status_priority (status, priority),
  INDEX idx_end_time (end_time)
);
CREATE TABLE machines (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  machine_uuid     CHAR(36) UNIQUE NOT NULL,
  hostname         VARCHAR(255) UNIQUE NOT NULL,
  state            ENUM('available','reserved','provisioning','in_use','draining',
                        'maintenance','failed','decommissioned') NOT NULL,
  machine_type     VARCHAR(64) NOT NULL,
  region           VARCHAR(32) NOT NULL,
  availability_zone VARCHAR(32) NOT NULL,
  rack_id          VARCHAR(32) NOT NULL,
  rack_position    SMALLINT UNSIGNED NOT NULL,
  gpu_type         VARCHAR(32),   -- 'H100','A100','H200',NULL
  gpu_count        TINYINT UNSIGNED NOT NULL DEFAULT 0,
  cpu_model        VARCHAR(64) NOT NULL,
  cpu_cores        SMALLINT UNSIGNED NOT NULL,
  ram_gb           SMALLINT UNSIGNED NOT NULL,
  nvme_tb          DECIMAL(5,2) NOT NULL,
  network_gbps     SMALLINT UNSIGNED NOT NULL,
  gpu_interconnect VARCHAR(32),   -- 'NVLink','PCIe'
  bmc_ip           VARCHAR(45) NOT NULL,
  bmc_mac          CHAR(17) NOT NULL,
  firmware_version VARCHAR(32),
  health_score     TINYINT UNSIGNED NOT NULL DEFAULT 100,
  last_health_check DATETIME,
  failure_count_30d SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  version          INT UNSIGNED NOT NULL DEFAULT 1,
  INDEX idx_type_state_az (machine_type, state, availability_zone),
  INDEX idx_state_health (state, health_score),
  INDEX idx_gpu_type_state (gpu_type, state)
);
CREATE TABLE reservation_slots (
  id             BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  machine_id     BIGINT UNSIGNED NOT NULL,
  reservation_id BIGINT UNSIGNED NOT NULL,
  start_time     DATETIME NOT NULL,
  end_time       DATETIME NOT NULL,
  INDEX idx_machine_interval (machine_id, start_time, end_time)  -- conflict detection
);
CREATE TABLE waitlist (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  reservation_id   BIGINT UNSIGNED NOT NULL,
  machine_type     VARCHAR(64) NOT NULL,
  requested_count  INT UNSIGNED NOT NULL,
  priority         TINYINT UNSIGNED NOT NULL,
  desired_start    DATETIME NOT NULL,
  desired_end      DATETIME NOT NULL,
  expires_at       DATETIME NOT NULL,
  status           ENUM('waiting','matched','expired','cancelled') NOT NULL DEFAULT 'waiting',
  INDEX idx_type_priority (machine_type, priority, created_at),
  INDEX idx_expires (expires_at)
);
```

### Unique Algorithm: Interval Tree Conflict Detection
```java
// Augmented BST: each node stores [start, end, maxEnd]
// maxEnd = max(self.end, left.maxEnd, right.maxEnd)
// O(log n + k) to find all overlapping intervals
public List<Reservation> findOverlapping(Instant qStart, Instant qEnd) {
    List<Reservation> result = new ArrayList<>();
    searchOverlap(root, qStart, qEnd, result);
    return result;
}
private void searchOverlap(TreeNode node, Instant qS, Instant qE, List<Reservation> r) {
    if (node == null) return;
    // Overlap iff node.start < qEnd AND node.end > qStart
    if (node.start.isBefore(qE) && node.end.isAfter(qS)) r.add(node.reservation);
    // Prune left subtree if maxEnd <= qStart (no overlap possible)
    if (node.left != null && node.left.maxEnd.isAfter(qS))
        searchOverlap(node.left, qS, qE, r);
    if (node.right != null)
        searchOverlap(node.right, qS, qE, r);
}
```

### Unique Reservation Flow
```
1. Check Redis availability cache (fast reject if machine type unavailable)
2. Interval tree check: O(log n) per candidate machine type
3. MySQL SELECT … FOR UPDATE on candidate machines
4. Re-verify no conflicts after lock acquired
5. INSERT reservation; INSERT reservation_slots; UPDATE machine.state
6. INSERT audit log; UPDATE Redis cache
7. COMMIT
8. Publish Kafka: reservation.created
9. Provisioning Orchestrator picks up; executes DAG:
   IPMI power-on → PXE boot → OS install → VLAN config → health check → mark active
```

### Unique NFRs
- Reservation API: P50 < 50 ms, P99 < 200 ms; conflict check P99 < 20 ms
- Full bare-metal provisioning: < 15 min P99
- Scale: 50K machines, 10K concurrent active reservations, 500 req/sec peak
- RPO = 0 for reservation records

---

## iaas_platform_control_plane

### Unique Purpose
Multi-tenant cloud control plane managing 100K physical servers, 500K VMs, 10K tenants. Unified REST API for compute (VM + bare-metal), network (SDN), storage (Ceph), identity, and metering. OpenStack-compatible.

### Unique Architecture
- **Cell-based**: ~5,000 hosts per cell managed independently; global super-scheduler routes requests to correct cell; cell failure isolated
- **OpenStack-compatible services**: Compute (Nova-compatible), Network (Neutron + OVN/Contrail), Storage (Cinder + Ceph RBD), Identity (Keystone), Image (Glance + Ceph RadosGW)
- **Scheduler**: Filter → Weigh → Random-top-3 selection (prevents hotspots); overcommit ratios: CPU 4:1 general, 1:1 GPU; RAM 1.5:1, 1:1 bare-metal
- **Metering**: instance lifecycle events → Kafka → Metering Service aggregates hourly/daily/monthly; eventual consistency < 30 s

### Key Schema
```sql
CREATE TABLE instances (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  instance_uuid    CHAR(36) UNIQUE NOT NULL,
  tenant_id        BIGINT UNSIGNED NOT NULL,
  project_id       BIGINT UNSIGNED NOT NULL,
  name             VARCHAR(255) NOT NULL,
  instance_type    ENUM('vm','bare_metal') NOT NULL,
  status           ENUM('BUILDING','ACTIVE','SHUTOFF','PAUSED','SUSPENDED',
                        'REBOOT','RESIZE','ERROR','DELETED') NOT NULL,
  flavor_id        BIGINT UNSIGNED NOT NULL,
  image_id         BIGINT UNSIGNED,
  host_id          BIGINT UNSIGNED,
  reservation_id   BIGINT UNSIGNED,      -- for bare-metal
  availability_zone VARCHAR(32) NOT NULL,
  vcpus            SMALLINT UNSIGNED NOT NULL,  -- denormalized from flavor
  ram_mb           INT UNSIGNED NOT NULL,
  disk_gb          INT UNSIGNED NOT NULL,
  gpu_count        TINYINT UNSIGNED NOT NULL DEFAULT 0,
  fixed_ip         VARCHAR(45),
  floating_ip      VARCHAR(45),
  user_data        TEXT,                  -- Base64-encoded cloud-init
  tags             JSON,
  launched_at      DATETIME(6),
  terminated_at    DATETIME(6),
  deleted_at       DATETIME(6),          -- soft delete
  created_at       DATETIME(6) NOT NULL,
  updated_at       DATETIME(6) NOT NULL,
  INDEX idx_tenant_status (tenant_id, status),
  INDEX idx_host (host_id),
  INDEX idx_az_status (availability_zone, status)
);
CREATE TABLE hosts (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  host_uuid        CHAR(36) UNIQUE NOT NULL,
  hostname         VARCHAR(255) UNIQUE NOT NULL,
  host_type        ENUM('hypervisor','bare_metal','storage','network') NOT NULL,
  status           ENUM('enabled','disabled','maintenance') NOT NULL,
  state            ENUM('up','down','unknown') NOT NULL,
  availability_zone VARCHAR(32) NOT NULL,
  rack_id          VARCHAR(32) NOT NULL,
  total_vcpus      SMALLINT UNSIGNED NOT NULL,
  total_ram_mb     INT UNSIGNED NOT NULL,
  total_gpus       TINYINT UNSIGNED NOT NULL DEFAULT 0,
  used_vcpus       SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  used_ram_mb      INT UNSIGNED NOT NULL DEFAULT 0,
  used_gpus        TINYINT UNSIGNED NOT NULL DEFAULT 0,
  cpu_overcommit   DECIMAL(3,1) NOT NULL DEFAULT 4.0,  -- 1.0 for GPU hosts
  ram_overcommit   DECIMAL(3,1) NOT NULL DEFAULT 1.5,
  hypervisor_type  VARCHAR(32),  -- 'kvm','xen', NULL for bare-metal
  last_heartbeat   DATETIME,
  INDEX idx_az_status (availability_zone, status, state),
  INDEX idx_capacity (host_type, availability_zone, used_vcpus, used_ram_mb)
);
CREATE TABLE quotas (
  id           BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  tenant_id    BIGINT UNSIGNED NOT NULL,
  resource_type ENUM('instances','vcpus','ram_mb','gpu','volumes',
                     'volume_gb','floating_ips','networks','snapshots') NOT NULL,
  quota_limit  INT NOT NULL DEFAULT -1,      -- -1 = unlimited
  in_use       INT UNSIGNED NOT NULL DEFAULT 0,
  reserved     INT UNSIGNED NOT NULL DEFAULT 0,
  UNIQUE KEY uk_tenant_resource (tenant_id, resource_type)
);
-- Metering: partitioned monthly
CREATE TABLE metering_events (
  id           BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  event_uuid   CHAR(36) UNIQUE NOT NULL,
  tenant_id    BIGINT UNSIGNED NOT NULL,
  resource_type VARCHAR(32) NOT NULL,
  resource_id  CHAR(36) NOT NULL,
  event_type   VARCHAR(64) NOT NULL,    -- 'instance.create','instance.delete'
  payload      JSON NOT NULL,
  timestamp    DATETIME(6) NOT NULL,
  INDEX idx_tenant_time (tenant_id, timestamp),
  INDEX idx_resource (resource_id, timestamp)
) PARTITION BY RANGE (TO_DAYS(timestamp));
```

### Unique Algorithm: Filter-Then-Weigh Scheduler
```java
// Step 1: Hard filters (eliminate candidates)
filters = [AZFilter, ResourceAvailabilityFilter, HostStateFilter,
           AffinityFilter, AntiAffinityFilter, AggregateFilter, MaintenanceFilter]
// Step 2: Soft weighers (score remaining candidates)
weighers = [(RamWeigher, 1.0), (CpuWeigher, 0.5), (GpuWeigher, 2.0),
            (RackSpreadWeigher, 0.3), (HostLoadWeigher, 0.8)]
score(host) = Σ weigher.weigh(host, request) × weigher.multiplier
// Step 3: Pick top-3 by score, select randomly → prevents scheduling hotspots
topN = sortByScoreDesc(candidates).limit(3)
selected = topN[random.nextInt(3)]
```

### Overcommit Ratios
| Resource | General-Purpose | GPU Host | Bare-Metal |
|----------|----------------|----------|------------|
| CPU | 4:1 | 1:1 | N/A |
| RAM | 1.5:1 | 1:1 | N/A |
| GPU | N/A | 1:1 (no overcommit) | N/A |

### Unique NFRs
- API: P50 < 100 ms, P99 < 500 ms; 5,000 calls/sec global
- VM boot: < 60 s; bare-metal provisioning: < 15 min
- Scale: 100K physical servers, 500K VMs, 10K tenants
- Control plane: 99.99%; data plane (running instances): 99.999%

---

## machine_pool_manager

### Unique Purpose
Fleet lifecycle for 100K machines across 500 pools. Real-time health scoring (0–100), auto-ejection on degradation, firmware rolling upgrades with blast-radius controls, vendor RMA integration, 2–8 week capacity forecasting.

### Unique Architecture
- **Health Data Collector**: per-AZ; polls IPMI/BMC every 15 min; scrapes Prometheus node_exporter + nvidia_exporter; SNMP on switches
- **Firmware Manager**: tracks versions fleet-wide; rolling upgrades in batches of 10; `max_failure_pct = 5%` auto-aborts; maintenance window scheduling
- **RMA Service**: integrates with Dell TechDirect, HPE RMA, Supermicro portals; tracks `created → submitted_to_vendor → part_shipped → part_received → installed → verified → closed`
- **Capacity Forecast Engine**: ARIMA / exponential smoothing / Prophet; 90% CI output; gap = predicted_demand − current_supply

### Key Schema
```sql
CREATE TABLE pools (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  pool_uuid        CHAR(36) UNIQUE NOT NULL,
  name             VARCHAR(128) UNIQUE NOT NULL,  -- 'gpu_h100_8x_prod_use1'
  machine_type     VARCHAR(64) NOT NULL,
  region           VARCHAR(32) NOT NULL,
  purpose          ENUM('production','staging','reserved','overflow','decommission') NOT NULL,
  state            ENUM('active','draining','frozen','retired') NOT NULL,
  target_size      INT UNSIGNED NOT NULL,
  min_size         INT UNSIGNED NOT NULL,
  max_size         INT UNSIGNED NOT NULL,
  warm_pool_target INT UNSIGNED NOT NULL DEFAULT 0,
  cold_pool_target INT UNSIGNED NOT NULL DEFAULT 0,
  target_utilization DECIMAL(4,1) NOT NULL DEFAULT 75.0,
  health_eject_threshold TINYINT UNSIGNED NOT NULL DEFAULT 30,
  health_watch_threshold TINYINT UNSIGNED NOT NULL DEFAULT 60,
  health_warning_threshold TINYINT UNSIGNED NOT NULL DEFAULT 90
);
CREATE TABLE pool_memberships (
  id         BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  pool_id    BIGINT UNSIGNED NOT NULL,
  machine_id BIGINT UNSIGNED NOT NULL,
  pool_state ENUM('warm','cold','active','draining','ejected') NOT NULL,
  joined_at  DATETIME NOT NULL,
  left_at    DATETIME,
  UNIQUE KEY uk_machine_pool (machine_id, pool_id, left_at),
  INDEX idx_pool_state (pool_id, pool_state)
);
-- Health checks: partitioned monthly
CREATE TABLE health_checks (
  id                         BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  machine_id                 BIGINT UNSIGNED NOT NULL,
  health_score               TINYINT UNSIGNED NOT NULL,
  cpu_temp_c                 SMALLINT UNSIGNED,
  memory_ecc_correctable     SMALLINT UNSIGNED,
  memory_ecc_uncorrectable   SMALLINT UNSIGNED,
  gpu_temp_c                 SMALLINT UNSIGNED,
  gpu_ecc_errors             SMALLINT UNSIGNED,
  disk_smart_status          TINYINT UNSIGNED,   -- 0=fail, 1=pass
  disk_wear_pct              TINYINT UNSIGNED,
  network_errors_1h          INT UNSIGNED,
  bmc_responsive             BOOLEAN,
  power_draw_watts           SMALLINT UNSIGNED,
  details_json               JSON,
  checked_at                 DATETIME NOT NULL,
  INDEX idx_machine_time (machine_id, checked_at),
  INDEX idx_score_time (health_score, checked_at)
) PARTITION BY RANGE (TO_DAYS(checked_at));
CREATE TABLE firmware_inventory (
  id             BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  machine_id     BIGINT UNSIGNED NOT NULL,
  component      ENUM('bios','bmc','gpu','nic','nvme','cpu_microcode','system_board') NOT NULL,
  current_version VARCHAR(64) NOT NULL,
  target_version  VARCHAR(64),
  last_updated    DATETIME NOT NULL,
  update_status   ENUM('current','pending','in_progress','failed','rollback') NOT NULL DEFAULT 'current',
  UNIQUE KEY uk_machine_component (machine_id, component)
);
CREATE TABLE rma_tickets (
  id                BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  ticket_uuid       CHAR(36) UNIQUE NOT NULL,
  machine_id        BIGINT UNSIGNED NOT NULL,
  vendor            ENUM('dell','hpe','supermicro','nvidia','other') NOT NULL,
  vendor_ticket_id  VARCHAR(128),
  component_type    VARCHAR(64) NOT NULL,
  component_serial  VARCHAR(128),
  failure_description TEXT NOT NULL,
  status            ENUM('created','submitted_to_vendor','part_shipped','part_received',
                         'installed','verified','closed') NOT NULL,
  severity          ENUM('critical','high','normal') NOT NULL,
  created_at        DATETIME NOT NULL,
  INDEX idx_machine (machine_id),
  INDEX idx_vendor_status (vendor, status)
);
```

### Unique Algorithm: Weighted Health Score
```
score = (cpu_temp_score × 0.10) +
        (memory_ecc_score × 0.25) +   # heavy: ECC errors = imminent DIMM failure
        (gpu_health_score × 0.25) +
        (disk_health_score × 0.15) +
        (network_health_score × 0.10) +
        (bmc_responsive_score × 0.10) +
        (power_score × 0.05)

# Example: 47 correctable ECC/hr → memory_ecc_score = 40/100
#   → contributes 40 × 0.25 = 10.0 → total score = 84.5 → watch threshold

Thresholds:
  score < 30 → immediate ejection from pool (machine.state = maintenance)
  score < 60 → add to watch list; check every 5 min
  score < 90 → warning; generate alert
```

### Auto-Ejection Flow
```
1. health_score drops below pool.health_eject_threshold (default 30)
2. Pool Manager:
   a. UPDATE machine.state = 'maintenance'
   b. Remove from Redis pool index (pool:{pool_id}:available sorted set)
   c. If machine has active reservation: notify tenant (Kafka machine.ejected), offer replacement
   d. Create maintenance task
   e. Publish Kafka: machine.ejected
3. If warm_pool below target → promote machine from cold pool
4. If cold pool empty → trigger capacity forecast check
```

### Unique NFRs
- Health check: 100 checks/sec (all 100K machines checked every 15 min)
- Pool membership query: < 50 ms P99 (Redis sorted set)
- Firmware upgrade: 15–45 min per machine; batch 10; abort > 5% failures
- Capacity forecast: < 30 s generation; 90% CI; 2–8 week horizon

---

## self_service_developer_portal

### Unique Purpose
Self-service infrastructure for 5,000 portal users / 10,000 CLI sessions. React SPA + Go CLI (`infra-cli`). Template-based provisioning (YAML/HCL/Terraform), multi-step workflow engine with approval chains ($10K = manager, $50K = VP), cost dashboards, budget enforcement.

### Unique Architecture
- **Workflow Engine**: polls MySQL at 1 s intervals; executes step DAGs asynchronously via Kafka; dependency resolution; rollback in reverse order on failure
- **Portal BFF** (Backend-for-Frontend): aggregates from multiple services; WebSocket for real-time workflow status updates; session management
- **CLI (Go binary)**: device authorization flow or API key auth; `--dry-run` support; outputs table/JSON/YAML; mirrors portal functionality
- **Template Engine**: wraps Terraform; parses YAML/HCL; validates against catalog + quotas; manages Terraform state in S3

### Key Schema
```sql
CREATE TABLE workflows (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  workflow_uuid    CHAR(36) UNIQUE NOT NULL,
  tenant_id        BIGINT UNSIGNED NOT NULL,
  created_by       BIGINT UNSIGNED NOT NULL,
  template_id      BIGINT UNSIGNED,
  name             VARCHAR(255) NOT NULL,
  status           ENUM('pending_approval','approved','running','paused',
                        'completed','failed','cancelled','rolling_back') NOT NULL,
  total_steps      SMALLINT UNSIGNED NOT NULL,
  completed_steps  SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  estimated_cost_usd DECIMAL(12,2),
  environment      ENUM('dev','staging','prod') NOT NULL DEFAULT 'dev',
  version          INT UNSIGNED NOT NULL DEFAULT 1,
  started_at       DATETIME(6),
  completed_at     DATETIME(6),
  error_message    TEXT,
  INDEX idx_tenant_status (tenant_id, status),
  INDEX idx_created_by (created_by, status)
);
CREATE TABLE workflow_steps (
  id          BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  workflow_id BIGINT UNSIGNED NOT NULL REFERENCES workflows(id),
  step_order  SMALLINT UNSIGNED NOT NULL,
  name        VARCHAR(255) NOT NULL,
  step_type   ENUM('provision','approval','notification','wait','script','terraform_apply') NOT NULL,
  status      ENUM('pending','waiting_dependency','running','completed','failed','skipped','rolled_back') NOT NULL DEFAULT 'pending',
  config      JSON NOT NULL,          -- step-specific params
  depends_on  JSON,                   -- array of step IDs
  resource_id VARCHAR(255),           -- created resource reference
  rollback_config JSON,
  started_at  DATETIME(6),
  completed_at DATETIME(6),
  error_message TEXT,
  INDEX idx_workflow_order (workflow_id, step_order)
);
CREATE TABLE approvals (
  id               BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  approval_uuid    CHAR(36) UNIQUE NOT NULL,
  workflow_id      BIGINT UNSIGNED NOT NULL,
  step_id          BIGINT UNSIGNED,
  approver_id      BIGINT UNSIGNED,
  required_role    VARCHAR(64) NOT NULL,    -- 'manager','vp'
  required_group   VARCHAR(128),
  status           ENUM('pending','approved','rejected','expired','auto_approved') NOT NULL DEFAULT 'pending',
  reason           TEXT,
  estimated_cost_usd DECIMAL(12,2),
  expires_at       DATETIME NOT NULL,
  decided_at       DATETIME,
  created_at       DATETIME NOT NULL,
  INDEX idx_approver (approver_id, status),
  INDEX idx_pending_group (required_group, status, expires_at)
);
-- Cost aggregates: partitioned monthly
CREATE TABLE cost_aggregates (
  id            BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  tenant_id     BIGINT UNSIGNED NOT NULL,
  team_id       BIGINT UNSIGNED,
  project_id    BIGINT UNSIGNED,
  resource_type VARCHAR(32) NOT NULL,
  machine_type  VARCHAR(64),
  hour_bucket   DATETIME NOT NULL,    -- truncated to hour
  cost_usd      DECIMAL(12,4) NOT NULL,
  quantity      INT UNSIGNED NOT NULL,
  UNIQUE KEY uk_bucket (tenant_id, team_id, project_id, resource_type, hour_bucket),
  INDEX idx_tenant_time (tenant_id, hour_bucket)
) PARTITION BY RANGE (TO_DAYS(hour_bucket));
CREATE TABLE budgets (
  id             BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
  tenant_id      BIGINT UNSIGNED NOT NULL,
  team_id        BIGINT UNSIGNED,
  project_id     BIGINT UNSIGNED,
  budget_type    ENUM('monthly','quarterly','annual') NOT NULL,
  soft_limit_usd DECIMAL(12,2) NOT NULL,
  hard_limit_usd DECIMAL(12,2) NOT NULL,
  current_spend_usd DECIMAL(12,2) NOT NULL DEFAULT 0,
  period_start   DATE NOT NULL,
  period_end     DATE NOT NULL,
  alert_sent     BOOLEAN NOT NULL DEFAULT FALSE,
  INDEX idx_tenant_period (tenant_id, period_start, period_end)
);
```

### Unique Algorithm: Workflow Engine Step Resolution
```java
@Scheduled(fixedRate = 1000)   // poll every second
void processWorkflows() {
    List<Workflow> active = workflowRepo.findByStatusIn(RUNNING, APPROVED);
    for (Workflow wf : active) {
        List<WorkflowStep> steps = stepRepo.findByWorkflowId(wf.getId());
        for (WorkflowStep step : steps) {
            if (step.status == PENDING && areDependenciesMet(step, steps)) {
                if (step.stepType == APPROVAL) {
                    createApprovalRequest(wf, step);   // notify approver via Slack
                    step.status = WAITING_DEPENDENCY;
                } else {
                    step.status = RUNNING;
                    kafkaTemplate.send("workflow.step.execute", StepExecutionRequest.of(wf, step));
                }
                stepRepo.save(step);
            }
        }
        // Terminal conditions
        if (steps.stream().allMatch(s -> s.status == COMPLETED)) markCompleted(wf);
        if (steps.stream().anyMatch(s -> s.status == FAILED)) initiateRollback(wf, steps);
    }
}
boolean areDependenciesMet(WorkflowStep step, List<WorkflowStep> all) {
    return step.dependsOn == null || all.stream()
        .filter(s -> step.dependsOn.contains(s.id))
        .allMatch(s -> s.status == COMPLETED);
}
```

### Approval Threshold Logic
```
estimated_cost_usd < $10,000  → auto-approved
estimated_cost_usd >= $10,000 → approval step: required_role = 'manager'
estimated_cost_usd >= $50,000 → two approval steps: 'manager' + 'vp'
Approver notified via Slack; approval link in message; expires_at = +48h
On rejection: workflow.status = 'failed'; requester notified with reason
```

### Unique NFRs
- Portal: 99.9% availability; page load P50 < 1 s, P99 < 3 s
- API/CLI: 99.99% availability; CLI P50 < 500 ms, P99 < 2 s
- Scale: 5K concurrent portal users, 10K CLI sessions, 1K template applies/day
- Cost dashboard: < 5 s lag (eventual consistency from Kafka metering events)
- RPO = 0 for workflow state and approval records
