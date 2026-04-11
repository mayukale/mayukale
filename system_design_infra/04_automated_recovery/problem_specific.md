# Problem-Specific Design — Automated Recovery (04_automated_recovery)

## Auto-Remediation Runbook System

### Unique Functional Requirements
- Runbook repository: 5,000+ runbooks; execution start < 5 s P99 < 10 s
- Up to 100 concurrent runbook executions; 2-year execution history retention
- Approval gates with 15-minute timeout; escalate if exceeded
- ML-based runbook selection from observed symptoms (confidence threshold 0.8)
- Decision tree branching on diagnostic step outputs with Jinja2 templating

### Unique Components / Services
- **Decision Tree Engine**: evaluates conditional branches based on diagnostic step outputs; Jinja2 templating `{{ output.field_name }}`; conditional: `if: "{{ smart_output.Reallocated_Sector_Ct | int > 500 }}"` — enables branching on real-time diagnostic values
- **Symptom Classifier + Runbook Recommender**: ML model classifies failure symptoms into categories; RAG-based retrieval selects most relevant runbook; confidence > 0.8 required to auto-select; else escalates to human
- **Blast Radius Controller**: `selected_targets = min(floor(total × max_pct/100), max_absolute_count, max_per_rack)`; enforced before dispatch
- **Outcome Learner**: feedback loop tracking runbook success/failure rates; improves classifier and recommender over time
- **Approval Gate Manager**: sends requests via Slack/PagerDuty; tracks responses; enforces 15-min timeout; `approval_request.decision ENUM(pending/approved/rejected/timeout)`
- **Rollback Engine**: on failure, executes rollback steps in reverse order; `step_execution.rollback_status ENUM(not_needed/pending/completed/failed)`

### Unique Data Model
- **runbook**: runbook_id VARCHAR(128), category ENUM(host_recovery/pod_recovery/database/network/storage/kafka/elasticsearch/kubernetes/bare_metal/security/capacity), target_type ENUM(host/pod/service/cluster/bare_metal/mysql_cluster/kafka_cluster/es_cluster), severity ENUM(critical/high/medium/low), current_version INT, requires_approval BOOLEAN, blast_radius_limit JSON `{"max_targets": 5, "max_pct": 1}`, auto_execute BOOLEAN DEFAULT FALSE
- **execution**: execution_id, runbook_id, runbook_version INT, trigger_type ENUM(automated/manual/scheduled), status ENUM(pending/running/waiting_approval/rolling_back/succeeded/failed/aborted/dry_run_completed), current_step_index INT, dry_run BOOLEAN, rollback_executed BOOLEAN
- **step_execution**: execution_id, step_index INT, step_type ENUM(diagnostic/condition/action/verification/approval/notification/wait/rollback), execution_result ENUM(success/failure/timeout/skipped/dry_run), precondition_result ENUM(passed/failed/skipped), postcondition_result, rollback_status
- **approval_request**: execution_id, step_index, timeout_at TIMESTAMP, decision ENUM(pending/approved/rejected/timeout), notification_channel ENUM(slack/pagerduty/email)

### Key Differentiator
Auto-Remediation Runbook System's uniqueness is its **decision tree engine + ML runbook recommendation + precondition/postcondition per step**: decision tree branches on real-time diagnostic outputs via Jinja2 templating — encoding senior engineer knowledge as executable logic; RAG-based recommender selects from 5,000+ runbooks with confidence threshold 0.8; pre/postconditions on every step validate the environment is correct before action and verify the fix worked after; Outcome Learner closes the loop by improving recommendations based on historical success rates.

---

## Automated Failover System

### Unique Functional Requirements
- Phi Accrual failure detection; < 10 s intra-AZ, < 30 s cross-AZ detection
- Failover execution: < 5 s stateless, < 30 s MySQL, < 5 min cross-region
- Tiered RPO: 0 s Tier 1 (synchronous), < 1 s Tier 2 (semi-sync), < 5 min Tier 3 (async)
- False failover rate < 0.01%; split-brain prevention via STONITH + fencing tokens

### Unique Components / Services
- **Phi Accrual Failure Detector**: `φ = -log₁₀(1 - F(time_since_last_heartbeat))` where F = CDF of normal distribution fitted to historical heartbeat intervals; φ > 8 → 99.9997% confidence of failure; φ > 12 → 99.999999% confidence; adaptive: increased jitter widens distribution, φ increases more slowly
- **STONITH Fencing (Split-Brain Prevention)**: fencing token with monotonically increasing token_value; old primary cannot write if token_value is stale; new primary acquires higher token before accepting writes; prevents split-brain during network partition
- **Failover Orchestrator**: state machine per cluster: NORMAL → FENCING → PROMOTING → REROUTING → VERIFYING → COMPLETED
- **MySQL Failover Executor (MHA-style)**: selects best replica by score: `(is_sync_replica × 1000) + (different_az × 100) + (1/(lag_ms+1) × 50) + (priority × 10)`; applies relay logs; promotes selected replica
- **ProxySQL Router**: transparent connection routing; updates `write_group` hostgroup during failover without application reconnect
- **DNS Failover**: TTL = 30 s; updates A/AAAA/CNAME records on failover_target within seconds of promotion

### Unique Data Model
- **managed_cluster**: cluster_id, cluster_type ENUM(mysql/kafka/etcd/k8s_control_plane/elasticsearch/redis/custom), region, failover_mode ENUM(active_passive/active_active/multi_primary), current_primary_id, rpo_target_sec INT, rto_target_sec INT DEFAULT 30, failover_state ENUM(normal/detecting/fencing/promoting/rerouting/verifying/completed/failed/manual_intervention), failover_count_30d INT
- **cluster_member**: member_id, cluster_id, role ENUM(primary/sync_replica/async_replica/standby/active/witness), availability_zone, replication_lag_ms INT DEFAULT 0, is_healthy BOOLEAN, is_fenced BOOLEAN, priority INT DEFAULT 100
- **failover_event**: event_id BIGINT AUTO_INCREMENT, event_type ENUM(failure_detected/failover_started/primary_fenced/replica_promoted/traffic_rerouted/failover_completed/failover_failed/failover_rolled_back/manual_intervention_required), old_primary_id, new_primary_id, rpo_achieved_ms INT, rto_achieved_ms INT, initiated_by ENUM(automated/manual/scheduled)
- **fencing_token**: cluster_id PK, token_value BIGINT UNSIGNED (monotonically increasing), holder_id, acquired_at, expires_at
- **dns_failover_record**: domain_name, record_type ENUM(A/AAAA/CNAME/SRV), ttl_seconds INT DEFAULT 30, primary_target, failover_target, current_target

### Algorithms

**Phi Accrual Detection:**
```
φ = -log10(1 - F(timeSinceLastHeartbeat))
φ > 8  → 99.9997% confidence (declare failure candidate)
φ > 12 → 99.999999% confidence (auto-confirm for critical clusters)
2-of-3 consensus required before failover starts
```

**Replica Selection Scoring:**
```
score = (is_sync_replica × 1000) + (different_az × 100)
      + (1/(replication_lag_ms+1) × 50) + (priority × 10)
Select replica with highest score
```

### Key Differentiator
Automated Failover's uniqueness is its **Phi Accrual + 2-of-3 consensus + STONITH fencing**: Phi Accrual adapts to network jitter (unlike fixed timeout detectors), reducing false positives; 2-of-3 consensus prevents any single checker from triggering failover; STONITH monotonically increasing fencing_token guarantees old primary cannot write after demotion regardless of network partition; replica selection score (sync replica 1000× preference + anti-AZ affinity + lag) ensures lowest-RPO replica is always chosen.

---

## Chaos Engineering Platform

### Unique Functional Requirements
- Experiment start < 5 s; abort < 3 s (P99 < 5 s) from signal to injection rollback
- Up to 50 concurrent experiments across fleet; 100 K+ targetable entities
- ~9 experiments/day average; peaks at 30/day during GameDays
- Maximum blast radius: 10% of target pool by default
- Multi-step GameDay scenarios with checkpoints and manual gates

### Unique Components / Services
- **Dead-Man's Switch**: watchdog monitoring running experiments; if engine heartbeat missed > 30 s → emergency rollback of all active injections; prevents infinite injection on orchestrator crash
- **GameDay Orchestrator**: sequences multi-step scenarios; ordered `steps_json` with experiments + checkpoints + manual gates; supports complex failure simulations (e.g., kill primary DB + kill 20% pods + inject network partition simultaneously)
- **Injection Drivers**: plugin-based; types: pod_kill, node_reboot, network_latency (tc/netem), cpu_stress (stress-ng), memory_stress, disk_fill (dd), dns_failure, process_kill, az_failure_sim, kafka_broker_kill, mysql_replica_kill, ipmi_power_off
- **Steady-State Validator**: evaluates PromQL queries; compares pre-injection baseline; `abort_config: {"steady_state_deviation_pct": 50, "error_rate_absolute": 0.05}`; auto-aborts if threshold exceeded
- **Chaos Agent (DaemonSet)**: installed on every node; receives injection commands via gRPC; executes locally; auto-rollback after 60 s timeout if no heartbeat from engine

### Unique Data Model
- **experiment**: experiment_id, name, environment ENUM(staging/production/dev), tier_restriction ENUM(all/tier2_and_below/tier3_and_below/staging_only) DEFAULT staging_only, status ENUM(draft/pending_approval/approved/running/aborting/aborted/completed/failed), hypothesis TEXT, injection_config JSON, target_config JSON, abort_config JSON `{"steady_state_deviation_pct": 50, "error_rate_absolute": 0.05}`, blast_radius_config JSON `{"max_percentage": 10, "max_absolute_count": 5, "max_per_az": 2}`, duration_sec INT
- **injection**: injection_id, experiment_id, injection_type ENUM(pod_kill/node_reboot/network_latency/network_loss/cpu_stress/memory_stress/disk_fill/dns_failure/process_kill/az_failure_sim/kafka_broker_kill/mysql_replica_kill/ipmi_power_off), target_type ENUM(pod/node/service/bare_metal/cluster), parameters_json JSON, status ENUM(pending/injecting/active/rolling_back/rolled_back/completed/failed), agent_id, injected_at, rolled_back_at
- **steady_state_measurement**: experiment_id, phase ENUM(pre_injection/during_injection/post_injection), metric_name, metric_query TEXT (PromQL), value DOUBLE, threshold DOUBLE, within_threshold BOOLEAN
- **experiment_result**: outcome ENUM(passed/failed/aborted/inconclusive), hypothesis_validated BOOLEAN, steady_state_maintained BOOLEAN, max_deviation_pct DOUBLE, recovery_time_sec INT
- **gameday**: gameday_id, steps_json JSON (ordered list of experiments + checkpoints), scheduled_date DATE

### Key Differentiator
Chaos Engineering Platform's uniqueness is its **dead-man's switch + multi-layer injection drivers + GameDay orchestration**: dead-man's switch (30 s engine heartbeat timeout → auto rollback) prevents runaway injections from orphaned experiments; 13 injection driver types (from pod_kill to ipmi_power_off) cover every infrastructure layer; GameDay Orchestrator sequences multi-step failure scenarios with checkpoints and manual gates for rehearsing incident response; `tier_restriction DEFAULT staging_only` provides safe guard against accidental production injection.

---

## Health Check and Remediation Service

### Unique Functional Requirements
- 2 M health checks/sec (100 K hosts × 20 checks/host at 10 s intervals)
- Composite health score (0–100) calculated in < 100 ms per entity
- Check-to-detection < 30 s (3 consecutive failures at 10 s); false positive rate < 0.1%
- Flap detection suppresses remediation for unstable entities
- SLO-aware gating: verifies remediation won't violate downstream SLOs before executing

### Unique Components / Services
- **Composite Score Calculator**: weighted average of 6 check types; `score = Σ(check_status[i] × weight[i]) / Σ(weight[i])`; weights: process=0.10, port=0.15, http=0.25, deep=0.35, hardware=0.10, kernel=0.05; score 100=healthy, 70–99=degraded, 0–69=unhealthy
- **Deep Health Check**: tests dependencies: DB (`SELECT 1`), cache (SET/GET roundtrip), downstream services, message broker connectivity; weight=0.35 (highest); distinguishes service-level failure from infra-level
- **Flap Detector**: counts state transitions per entity per 1-hour window; if transitions > threshold (e.g., 10) → mark flapping; suppress remediation for flap duration; records `flap_event (entity_id, transitions_count, duration_sec)`
- **SLO-Aware Gate**: before drain node: `if remaining_healthy_replicas - 1 < slo_minimum: skip_remediation("Would violate SLO")`; result in `remediation_record.slo_check_result JSON`
- **Escalation Manager**: `escalation_policy.tiers_json [tier1: auto_remediate 300s, tier2: alert_team_slack 900s, tier3: page_oncall 1800s, tier4: page_senior_oncall]`; advances tier on timeout

### Unique Data Model
- **health_check_template**: template_id, check_type ENUM(process/port/http/grpc/deep_health/hardware/kernel/custom), default_interval_sec INT DEFAULT 10, default_timeout_sec INT DEFAULT 5, default_failure_threshold INT DEFAULT 3
- **entity_health_state**: entity_id PK, composite_score DECIMAL(5,2), health_status ENUM(healthy/degraded/unhealthy/critical/unknown), checks_passing INT, checks_failing INT, is_flapping BOOLEAN, flap_suppressed_until TIMESTAMP, cooldown_until TIMESTAMP, escalation_tier INT DEFAULT 0
- **check_state**: check_id, entity_id, current_status ENUM(passing/failing/unknown/disabled), consecutive_failures INT, consecutive_successes INT, last_response_ms INT
- **remediation_record**: entity_id, action_type ENUM(restart_process/restart_pod/drain_node/cordon_node/replace_instance/reboot_host/alert_team/page_oncall/page_senior/noop), escalation_tier INT, status ENUM(pending/executing/succeeded/failed/skipped_cooldown/skipped_flapping/skipped_slo_risk/escalated), slo_check_result JSON, duration_ms INT
- **escalation_policy**: entity_type + service_tier composite; tiers_json with per-tier action + timeout_sec

### Algorithms

**Composite Health Score:**
```
score = Σ(check_status[i] × weight[i]) / Σ(weight[i])
where check_status[i] = 100 (passing) or 0 (failing)
weights: process=0.10, port=0.15, http=0.25, deep=0.35, hardware=0.10, kernel=0.05
100=healthy, 70-99=degraded, 0-69=unhealthy
```

### Key Differentiator
Health Check Service's uniqueness is its **composite weighted health score + deep dependency checks + flap suppression + SLO-aware gating**: weighted scoring (deep_health weight=0.35 > http=0.25 > port=0.15) reflects real-world impact; deep health checks test actual dependency connectivity (DB SELECT 1, cache roundtrip) not just port reachability; flap suppression (>10 transitions/hour → suppress remediation) prevents thrashing on intermittent issues; SLO-aware gating before drain/cordon verifies the cluster can absorb the action without breaching SLO.

---

## Self-Healing Infrastructure Platform

### Unique Functional Requirements
- 100 K bare-metal hosts + 2 M+ pods; 500 concurrent node repairs
- Detection < 30 s; remediation initiation < 60 s; pod restart < 10 s; PXE reimage < 15 min
- Level-triggered reconciliation: compare desired vs actual state every 10 s
- Out-of-band IPMI/BMC fallback for bare-metal recovery when in-band agents unreachable

### Unique Components / Services
- **Level-Triggered Reconciliation Loop**: `desired = etcd.get_desired_state(shard)`; `actual = observe_actual_state()`; `diff = compute_diff()`; for each unhealthy resource: check cooldown → check rate limiter → select_remediation → execute_with_timeout → record_audit; repeats every 10 s; idempotent by design
- **IPMI/BMC Controller**: out-of-band management over dedicated management network; commands: power_cycle, reset, sensor_reads; IPMI watchdog timer `ipmitool mc watchdog set timer timeout 120 action power_cycle`; OS agent must reset every 120 s; avoids OS-level agent dependency
- **PXE Boot Server**: reimages bare-metal hosts from known-good golden images; triggered on OS corruption; ~15 min reimage time; staged rollout validation before rollout to production fleet
- **Circuit Breaker**: stops requests to unhealthy downstream services after failure threshold; prevents cascading overload
- **Node Problem Detector (DaemonSet)**: detects hardware/kernel/runtime issues; reports NodeConditions (MemoryPressure, DiskPressure, NetworkUnavailable, KernelDeadlock) to K8s API server

### Unique Data Model
- **detected_failures**: failure_id VARCHAR(128), target_type ENUM(host/pod/service/bare_metal), failure_type ENUM(crash/hang/hardware/network/resource_exhaustion/kernel_panic/oom/disk_failure/runtime_failure), severity ENUM(critical/high/medium/low), detection_method ENUM(health_check/node_problem_detector/bmc_watchdog/reconciliation_diff/operator_report), consensus_count INT DEFAULT 1, status ENUM(candidate/confirmed/remediating/resolved/escalated)
- **remediation_actions**: action_id, failure_id FK, action_type ENUM(restart_pod/restart_process/drain_node/cordon_node/uncordon_node/reboot_host/reimage_host/replace_host/ipmi_reset/escalate_human/circuit_break/load_shed), status ENUM(pending/approved/executing/succeeded/failed/rolled_back/skipped_cooldown/skipped_rate_limit), requires_approval BOOLEAN, dry_run BOOLEAN
- **node_state**: node_id PK, desired_state ENUM(active/cordoned/draining/decommissioned), actual_state ENUM(active/cordoned/draining/decommissioned/unknown/unreachable), health_score DECIMAL(5,2), bmc_ip VARCHAR(45), os_image_version VARCHAR(64), cooldown_until TIMESTAMP
- **health_check_results** (PARTITIONED BY RANGE(UNIX_TIMESTAMP(checked_at))): one partition per day

### Algorithms

**Level-Triggered Reconciliation:**
```python
while True:
    desired = state_store.get_desired_state(shard)
    actual  = observe_actual_state(shard)
    diff    = compute_diff(desired, actual)
    for resource in diff:
        if resource.actual == UNHEALTHY and resource.desired == ACTIVE:
            if not cooldown_active(resource) and rate_limiter.allow(resource.type):
                action = select_remediation(resource)
                execute_with_timeout(action)
                record_audit(action)
    sleep(10)  # sync_interval
```

**Rate Limiting for Node Repairs:**
```
max_concurrent_drains_per_cluster = max(cluster_size × 0.05, 5)
max_reimages_per_rack = 1
```

### Key Differentiator
Self-Healing Infrastructure's uniqueness is its **level-triggered reconciliation + out-of-band IPMI fallback + PXE reimage**: level-triggered reconciliation (compare desired vs actual every 10 s, idempotent actions) enables crash-safe self-healing without distributed transactions; out-of-band IPMI watchdog (120 s timer → power cycle if OS agent stops resetting) handles hung kernels and in-band agent failures; PXE reimage from golden image provides bare-metal recovery when OS corruption is detected — unavailable in VM-only systems; `max_reimages_per_rack = 1` prevents rack-level outage from automated reimaging storm.
