# Common Patterns — Automated Recovery (04_automated_recovery)

## Common Components

### MySQL 8.0 as Source of Truth for Recovery State
- All 5 systems use MySQL for execution state, definitions, audit logs, and remediation records
- auto_remediation_runbook: `runbook`, `execution`, `step_execution`, `approval_request`
- automated_failover: `managed_cluster`, `cluster_member`, `failover_event`, `fencing_token`, `dns_failover_record`
- chaos_engineering: `experiment`, `injection`, `steady_state_measurement`, `experiment_result`, `gameday`
- health_check_remediation: `health_check_instance`, `entity_health_state`, `check_state`, `remediation_record`, `escalation_policy`
- self_healing_infrastructure: `detected_failures`, `remediation_actions`, `node_state`, `cooldown_periods`, `health_check_results`

### Consensus-Based Detection (N-of-M Agreement)
- 4 of 5 systems require multiple independent checkers to agree before triggering action — prevents false positives
- automated_failover: 3 detectors per region; 2-of-3 consensus required; if all 3 disagree → no failover, alert human
- chaos_engineering: pre-injection steady-state validation via multiple PromQL metrics; abort if any metric deviates > threshold
- health_check_remediation: 3 consecutive failures at configurable interval before marking unhealthy; false positive rate < 0.1%
- self_healing_infrastructure: N-of-M health checkers; safe failure mode = disagreement → no remediation, alert human

### Blast Radius Control
- 4 of 5 systems limit the number of entities affected by automated actions simultaneously
- auto_remediation_runbook: `blast_radius_limit JSON {"max_targets": 5, "max_pct": 1}` per runbook; `selected_targets = min(floor(total × max_pct/100), max_absolute_count, max_per_rack)`
- chaos_engineering: `blast_radius_config JSON {"max_percentage": 10, "max_absolute_count": 5, "max_per_az": 2}`
- health_check_remediation: SLO-aware gating checks min healthy replicas before drain; prevents cascading failures
- self_healing_infrastructure: `max_concurrent_drains_per_cluster = max(cluster_size × 0.05, 5)`; `max_reimages_per_rack = 1`

### Cooldown Periods to Prevent Remediation Storms
- 4 of 5 systems track last remediation time; suppress re-remediation within cooldown window
- auto_remediation_runbook: per-runbook cooldown after execution; configurable per runbook category
- health_check_remediation: `cooldown_until TIMESTAMP` on `entity_health_state`; skip if `now < cooldown_until`
- self_healing_infrastructure: `cooldown_periods (target_id, action_type, cooldown_start, cooldown_end)` table; checked before every action
- automated_failover: failover cooldown prevents oscillation between primary/replica

### Dry-Run Mode
- 4 of 5 systems support dry-run execution: simulates all steps without side effects, logs what would happen
- auto_remediation_runbook: `execution.dry_run BOOLEAN DEFAULT FALSE`; step_execution returns `dry_run_completed`
- chaos_engineering: dry-run validates targets, checks blast radius, skips actual injection drivers
- automated_failover: dry-run validates replica readiness and DNS records without executing failover
- self_healing_infrastructure: `remediation_actions.dry_run BOOLEAN DEFAULT FALSE`

### Rollback Support
- 4 of 5 systems execute reverse-order rollback steps when actions fail mid-execution
- auto_remediation_runbook: `rollback_engine` executes rollback steps in reverse order; `rollback_status ENUM(not_needed/pending/completed/failed)` per step
- chaos_engineering: injection rollback triggered on abort; dead-man's switch triggers emergency rollback if engine heartbeat missed > 30 s
- automated_failover: `failover_rolled_back` event type in failover_event; failed promotion → re-promote original primary
- self_healing_infrastructure: `action_type = 'rolled_back'` status; runbook executor handles rollback path

### Escalation Tiers (Auto-fix → Alert → Page → Senior)
- 4 of 5 systems define multi-tier escalation policies with timeouts between tiers
- auto_remediation_runbook: escalates to human if runbook confidence < 0.8 or approval timeout > 15 min
- health_check_remediation: `escalation_policy.tiers_json [tier1: auto_remediate 300s, tier2: alert_team_slack 900s, tier3: page_oncall 1800s, tier4: page_senior]`
- self_healing_infrastructure: escalation_tier tracked on detected_failures; advances on failed remediation
- automated_failover: `MANUAL_INTERVENTION_REQUIRED` failover_state if automated failover fails

### Immutable Audit Trail
- All 5 maintain append-only audit logs with actor, timestamp, target, before/after state
- auto_remediation_runbook: `step_execution` records every step with precondition/postcondition results; 2-year retention
- automated_failover: `failover_event` with rpo_achieved_ms, rto_achieved_ms, initiated_by
- chaos_engineering: `steady_state_measurement`, `experiment_result`, `injection` with timestamps
- health_check_remediation: `remediation_record` with slo_check_result JSON, duration_ms
- self_healing_infrastructure: every `remediation_action` logged; 2-year retention

## Common Databases

### MySQL 8.0
- All 5; state machine lifecycle, definitions, audit trail, remediation records; RANGE partitioned by time for hot/warm/cold retention

### Redis
- 4 of 5 (all except chaos); real-time composite health scores, flap state, cooldown timers, distributed locks (SETNX), rate limit counters

### Elasticsearch
- 4 of 5 (all except failover); full-text search on execution logs and definitions; time-series event indexes; searchable remediation history

### etcd
- 3 of 5 (failover, chaos, self-healing); leader election, fencing tokens, desired/actual state for reconciliation loop, cluster membership

### Kafka
- 3 of 5 (health_check, self-healing, chaos); event bus decoupling detectors from remediators; failure candidate events, state change events

## Common Communication Patterns

### REST API for Management + gRPC for Agent Data Plane
- All 5: REST CRUD for definitions and triggering; gRPC for high-frequency streaming (heartbeats, health signals, injection commands)
- automated_failover: gRPC heartbeat streams between cluster members and failure detection service
- chaos_engineering: gRPC between Experiment Engine and Chaos Agent DaemonSet for injection commands and rollback
- self_healing_infrastructure: gRPC for reconciliation loop state sync between controller and node agents

### Notifications via Slack / PagerDuty / Email
- All 5 send approval requests and escalation alerts via Slack + PagerDuty + email; approval_request tracks decision status (pending/approved/rejected/timeout)

## Common Scalability Techniques

### Sharding by Cluster / Entity
- self_healing_infrastructure: reconciliation loop sharded by cluster; each shard processes subset of nodes
- automated_failover: per-cluster state machine isolation; one state machine per managed_cluster
- chaos_engineering: target selection scoped per experiment; no cross-experiment interference

### Time-Series Partitioning for Event Data
- 4 of 5: MySQL RANGE partitioned by day or hour on created_at/checked_at; auto-drop old partitions; hot=7 days, warm=30 days, cold=90 days in Elasticsearch

## Common Deep Dive Questions

### How do you prevent cascading failures from automated remediation?
Answer: Three layers: (1) Blast radius limits — `min(floor(total × max_pct/100), max_absolute_count, max_per_unit)` caps how many nodes are touched simultaneously; (2) Cooldown periods — `cooldown_until TIMESTAMP` prevents re-remediation within a window (prevents flapping); (3) SLO-aware gating — before draining a node, verify remaining healthy replicas >= SLO minimum; if not, skip and escalate to human. All three must pass before any automated action executes.
Present in: auto_remediation_runbook, health_check_remediation, self_healing_infrastructure

### How do you avoid false-positive automated failovers?
Answer: Phi Accrual + N-of-M consensus: Phi Accrual computes φ = -log₁₀(1 - F(time_since_heartbeat)) where F is fitted to historical heartbeat intervals — φ > 8 means 99.9997% confidence of failure. This alone isn't enough; N-of-M consensus requires 2-of-3 independent detectors to agree before failover proceeds. If all 3 disagree, no failover occurs and a human is alerted. False failover rate: < 0.01%.
Present in: automated_failover, self_healing_infrastructure

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.99% for all 5 recovery systems (must exceed what they manage) |
| **Detection latency** | < 10 s (intra-AZ), < 30 s (cross-AZ, critical failures) |
| **Remediation initiation** | < 5 s (runbook, chaos), < 15 s (health check), < 60 s (self-healing) |
| **False positive rate** | < 0.1% (health check, self-healing), < 0.01% (automated failover) |
| **Scale** | 100 K bare-metal hosts + 2 M pods + 500 DB clusters |
| **Audit retention** | 2 years for all systems |
| **RTO** | < 30 s intra-region, < 5 min cross-region (failover); < 10 s pod restart; < 15 min PXE reimage |
| **RPO** | 0 s Tier 1, < 1 s Tier 2, < 5 min Tier 3 (tiered per service criticality) |
