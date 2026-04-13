# Infra-04: Automated Recovery — Interview Study Guide

Reading Infra Pattern 4: Automated Recovery — 5 problems, 8 shared components

---

## STEP 1 — PATTERN OVERVIEW

This pattern covers the full automated recovery stack: how a cloud infrastructure platform detects failures, decides what to do about them, executes structured remediation, validates recovery, and learns from outcomes — all without waking up an engineer at 3 AM.

The five problems in this pattern are:

1. **Auto-Remediation Runbook System** — encodes human operational knowledge as version-controlled, executable decision trees; selects the right runbook automatically using ML, then executes it with safety guards (blast radius, approval gates, rollback).
2. **Automated Failover System** — detects primary failures in MySQL, Kafka, Kubernetes control planes, and stateless services; promotes standby replicas with split-brain prevention; reroutes traffic transparently; targets sub-30-second MySQL RTO and zero RPO for Tier 1 services.
3. **Chaos Engineering Platform** — systematically injects failures (pod kill, network latency, disk fill, AZ failure simulation) to validate that resilience mechanisms actually work; includes a dead-man's switch, steady-state validation, and GameDay orchestration.
4. **Health Check and Remediation Service** — the sensory layer: multi-type checks (process, port, HTTP, deep dependency), composite 0-100 health scoring, flap suppression, SLO-aware gating before any remediation executes.
5. **Self-Healing Infrastructure Platform** — level-triggered reconciliation loop (desired vs. actual state every 10 seconds), out-of-band IPMI/BMC watchdog for bare-metal, PXE reimaging for OS corruption, circuit breakers for cascading failure prevention.

**The eight shared components** (components that appear across multiple problems): MySQL 8.0 as state authority, Redis for fast operational state, Elasticsearch for log search, etcd for consensus/leader election, Kafka as the event bus, consensus-based detection (N-of-M), blast radius control, and cooldown/escalation management.

---

## STEP 2 — MENTAL MODEL

**The core idea:** Automated recovery is a feedback control system. You continuously observe real-world state, compare it to desired state, compute a corrective action, execute it with guardrails, and verify it worked. If it didn't work, you escalate to the next tier — and you do all of this faster and more consistently than any human could, at a scale no human team can manage.

**Real-world analogy:** Think of a modern aircraft's autopilot combined with its emergency systems. The autopilot (reconciliation loop) continuously observes altitude, heading, speed and makes micro-corrections to match the flight plan (desired state). The emergency systems (health checks, failure detectors) watch for conditions outside safe operating bounds. When something goes seriously wrong, they execute a pre-programmed emergency procedure (runbook) — not free-form improvisation. The crew (humans) are still there but the system handles the first 90% automatically and only escalates to them for the decisions that genuinely require human judgment.

**Why this is hard:**

First, **false positives are catastrophic.** Automatically failing over a healthy MySQL primary causes a write outage and possibly data loss. Restarting a node that had a momentary network blip takes 15 minutes and disrupts tenants. Every automated action has a cost, so you cannot simply act on the first sign of trouble.

Second, **automated actions can make things worse.** If 40 nodes in a cluster all develop the same issue simultaneously (a bad OS kernel update, a network event), blindly restarting all of them destroys the cluster. The blast radius problem is real: the system must know not just "this node looks bad" but "how many nodes can I touch at once before I cause a bigger outage than the one I'm trying to fix?"

Third, **the recovery system itself must be more reliable than the things it heals.** If your self-healing platform goes down, you lose the ability to automatically recover anything. This imposes a 99.99%+ availability requirement on systems that are themselves managing 99.9% infrastructure.

Fourth, **split-brain is the failure mode nobody talks about enough.** In active-passive failover, if you promote a standby without being 100% certain the old primary is no longer writing, you get two primaries with diverging data — and merging them automatically is often impossible. Preventing this requires physical-level fencing, not just software signals.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing anything, ask these to understand the scope and drive the conversation toward your strongest knowledge:

1. **"What kind of failures are we automating recovery from — hardware, OS, application, or database? Or all layers?"** This determines which recovery mechanisms you need (IPMI vs. Kubernetes vs. MySQL failover vs. runbook execution). The answer changes the architecture significantly.

2. **"What are our RTO and RPO targets, and do they vary by service tier?"** The answer drives whether you need synchronous replication (RPO=0), whether you use semi-sync (RPO ~0), and how fast your detection and promotion pipeline needs to be. A 30-second RTO requires fundamentally different engineering than a 5-minute one.

3. **"What does the blast radius look like — are we talking about individual pods, database clusters, or entire availability zones?"** This determines which safety mechanisms you emphasize. Pod-level recovery is mostly about Kubernetes reconciliation. AZ-level recovery requires DNS failover, LB weight shifting, and DR activation.

4. **"Do we already have monitoring/alerting infrastructure (Prometheus, Grafana, PagerDuty) or do we need to build that too?"** If they say yes, you can scope the work to the recovery layer on top. If they say no, flag that the monitoring stack is a prerequisite and double the complexity estimate.

5. **"How much human oversight do we want — should all automated actions execute silently, or do high-impact ones (like database failover, node reimage) require an approval gate?"** This drives whether you need a human-in-the-loop approval workflow, which adds an entirely different component.

**What changes based on answers:**
- If RTO < 10 seconds → you need pre-built standby replicas, heartbeat detection at 1-second intervals, and near-instant ProxySQL/DNS switching. No time for relay log application.
- If blast radius is "entire AZ" → you need cross-AZ DNS failover and capacity pre-warming in surviving AZs, not just node-level repair.
- If all actions must be approved → your system is an escalation platform, not a fully autonomous one. Different architecture.

**Red flags to watch for during the interview:**
- Candidate who skips clarifying questions and immediately starts drawing boxes. They're optimizing for looking prepared rather than solving your actual problem.
- Candidate who says "just restart the process when it fails" without addressing false positive rates, blast radius, or flapping. Shows no production experience.
- Candidate who doesn't mention fencing/STONITH when talking about database failover. This is the most dangerous missing piece.

---

### 3b. Functional Requirements

**Core (non-negotiable for any automated recovery system):**
- Multi-layer failure detection across hardware, OS, runtime, and application layers
- Consensus-based failure confirmation (multiple independent signals required before acting)
- Automated remediation with configurable actions per failure type (restart, drain, failover, reimage)
- Blast radius enforcement: cap concurrent actions at both absolute count and percentage of fleet
- Cooldown periods: minimum time between successive remediation attempts on the same target
- Escalation policy: auto-fix tier → alert team → page on-call → page senior on-call
- Immutable audit trail: every automated action with actor, timestamp, before/after state, outcome

**Scope decisions to call out explicitly:**
- Are we building runbook execution (structured multi-step remediation) or just simple reactive actions?
- Is database failover in scope? That's the highest-risk, highest-complexity piece.
- Are we doing chaos engineering (proactive resilience validation) or reactive recovery only?

**Clear problem statement for a generic automated recovery interview:** "Design a system that monitors 100,000 bare-metal hosts and 2 million pods, detects failures within 30 seconds, automatically executes the appropriate remediation (restart, drain, failover, reimage) within 60 seconds of confirmed failure, caps concurrent actions to prevent cascading failures, escalates to humans when automation is exhausted, and provides a complete audit trail for post-incident analysis."

---

### 3c. Non-Functional Requirements

**Derive these from first principles, then state the trade-offs:**

**Availability (99.99%):** The recovery system must be more available than anything it manages. If you're managing 99.9% infrastructure, your recovery system needs to be at least 99.99%. This means the recovery platform itself needs HA across multiple AZs, N-of-M detector redundancy, and its own runbook for self-repair.

**Detection latency (< 30 seconds for critical failures):** Three consecutive heartbeat misses at 10-second intervals = 30 seconds. Trade-off: faster detection means more false positives during transient network events. Slower detection means longer outages. 10-second interval with a threshold of 3 misses is the standard balance.

**False positive rate (< 0.1%):** With 100,000 hosts checked every 10 seconds, even a 0.01% false positive rate triggers 10 unnecessary remediations per 10-second window. At 0.1%, that's 100 false remediations per 10 seconds — catastrophic. Consensus-based detection (require 2-of-3 detectors to agree) brings this below 0.01%.

**RPO/RTO (tiered):** ✅ Tier 1 (auth, payments): RPO=0, RTO<30s. Requires synchronous replication, STONITH, pre-warmed ProxySQL. ❌ This costs higher write latency (every commit waits for replica ACK). ✅ Tier 2 (general production): RPO<1s, RTO<30s. Semi-synchronous replication. ✅ Tier 3 (batch, analytics): RPO<5min, RTO<5min. Async replication, cheaper standby.

**Scale:** 100,000 hosts × 20 checks each at 10-second intervals = 200,000 checks/second. These don't need to be processed centrally — distribute by region and cluster. Kafka as the event bus absorbs the spike.

**Key trade-off to volunteer:** The faster you want to recover, the more you need to pre-provision standby capacity (more expensive), maintain low-TTL DNS records (more DNS query load), and run heartbeat-based detection at tight intervals (more monitoring traffic). Speed costs money.

---

### 3d. Capacity Estimation

**Walk through this on the whiteboard — it signals production credibility:**

**Health check volume:**
```
100,000 hosts × 5 checks/host ÷ 10s interval = 50,000 checks/sec (hosts)
2,000,000 pods × 2 checks/pod ÷ 10s interval = 400,000 checks/sec (pods)
Total: ~450,000 health check events/sec
Kafka bandwidth: 450,000 × 200 bytes = ~90 MB/sec
```

**Failure events (the actual load that matters):**
```
Daily host failure rate: 0.1% × 100,000 = 100 host failures/day = ~1.2/min
Pod restart rate: 0.5%/hour × 2M pods = 10,000 pod restarts/hour = ~2.8/sec
Remediation actions: ~80% auto-remediated
Kafka "confirmed failures" topic: manageable — low hundreds per minute
```

**Database heartbeat:**
```
500 MySQL clusters × 3 members = 1,500 MySQL instances
Heartbeat at 3-second interval → 500 heartbeats/sec to failure detection service
25,000/sec total when adding K8s control planes and Kafka
```

**Storage (the numbers that imply architecture):**
- Health check results (raw, 7-day hot): 105,000 events/sec × 200 bytes × 86,400s × 7 days = ~12.6 TB → Elasticsearch hot tier with daily rolling indexes
- Remediation audit trail: ~250 actions/sec × 1 KB × 30-day retention = ~650 GB → MySQL with time-range partitioning
- Current health scores (in-memory): 2.1M entities × 200 bytes = ~420 MB → fits entirely in Redis

**Architecture implications of these numbers:**
- The 90 MB/sec health event stream → Kafka with multiple partitions (at least 20), consumer groups for each downstream processor
- 12.6 TB health history → Elasticsearch with hot/warm/cold ILM (hot 7 days SSD, warm 30 days HDD, cold 90+ days object storage or delete)
- 420 MB current state → single Redis cluster fits fine, no sharding needed for scores
- MySQL remediation state → tiny compared to ES, ACID needed for the lifecycle state machine, time-partitioned for efficient cleanup

**Time on estimation:** Spend about 5-7 minutes. State the numbers, explain what they imply, then move to HLD. Do not get lost doing arithmetic for 20 minutes.

---

### 3e. High-Level Design

**Draw these 4-6 components in this order — each one builds on the previous:**

**1. Detection Layer (draw first, it's the input)**
Multiple independent detector agents per AZ send health signals. The key design decision here is that each detector is independent — they don't coordinate during collection, only during consensus. Kafka as the event bus between detectors and the consensus layer. Three detector instances per region minimum, deployed in different AZs.

**2. Consensus Layer (draw second, it's what makes detection safe)**
A Failure Consensus Service consumes raw failure candidates from Kafka. It requires N-of-M detectors to agree (2-of-3 is the standard) before writing to the "confirmed failures" Kafka topic. State stored in Redis (fast lookups) with MySQL as the authoritative record. Without this layer, a single network blip triggers automated actions.

**3. Remediation Decision Engine (draw third, it's the brain)**
Consumes confirmed failures. Checks three gates before taking any action: (a) cooldown — when was this target last remediated? (b) blast radius — how many targets of this type are currently being acted on? (c) SLO-aware gate — would this action violate downstream availability guarantees? If all three pass, select the appropriate remediation action and dispatch it.

**4. Execution Layer (draw fourth, it's the hands)**
Plugin-based executors: Kubernetes actions (pod restart, node drain, scale), host actions (reboot, IPMI reset, PXE reimage), database actions (failover trigger, replica add), notification (Slack, PagerDuty). Each plugin is idempotent — if the same action is dispatched twice, the second invocation is a no-op. Audit every action to MySQL.

**5. Escalation and State Store (draw fifth)**
MySQL is the source of truth for recovery state lifecycle. Redis holds real-time operational data (current health scores, cooldown timestamps, rate limit counters, distributed locks). Elasticsearch indexes everything for search and post-incident analysis.

**6. Runbook / Approval Layer (call out as optional depth)**
For high-impact actions (database failover, node reimage), route through a runbook executor that checks preconditions, optionally requests human approval via Slack/PagerDuty, executes steps sequentially with rollback support, and verifies postconditions.

**Key decisions to justify when the interviewer asks:**
- **Kafka as event bus:** Decouples detectors from remediators, absorbs traffic spikes, provides replay for debugging, enables fan-out to multiple consumers (composite score calculator, flap detector, remediation engine) from the same stream.
- **MySQL for lifecycle state vs. Redis for operational state:** MySQL gives you ACID — the execution lifecycle state machine (pending → running → succeeded/failed) must not be lost on crash. Redis gives you speed — current health scores and cooldown checks happen on the hot path.
- **Consensus before action:** You can always escalate later. You cannot un-failover a healthy database. Default to inaction when uncertain.

**Whiteboard drawing order:** Detection agents → Kafka → Consensus service → Confirmed failures topic → Remediation engine (with the three gates) → Execution plugins → MySQL + Redis + Elasticsearch. This flows left to right and tells a coherent story.

---

### 3f. Deep Dive Areas

These are the three areas interviewers probe hardest. Go deep here unprompted.

#### Deep Dive 1: Preventing Cascading Failures from Automated Remediation

**The problem:** If a bad OS kernel update hits 200 nodes simultaneously, they all fail health checks at roughly the same time. A naive remediation system drains all 200 at once, destroying 200 pods' worth of capacity and taking the cluster offline. The cure is worse than the disease.

**The solution — three independent layers, all must pass:**

Layer 1: **Blast radius limits.** Per-action type, enforce: `max_concurrent = min(floor(cluster_size × 0.05), 5)` for node drains, and `max_reimages_per_rack = 1` for bare-metal reimages. These numbers are floor-of-5%-or-5-whichever-is-smaller as absolute limits. When you hit the limit, queue and wait.

Layer 2: **Cooldown periods.** After remediating a target, record a `cooldown_until` timestamp. Skip any further actions on that target until the cooldown expires. This prevents the loop: fix → fails again → fix → fails again → fix. Standard cooldown: 5 minutes after restart, 30 minutes after drain, 2 hours after reimage.

Layer 3: **SLO-aware gating.** Before draining node X, query: "How many healthy replicas does each service on this node have?" If draining would put any service below its declared SLO minimum replica count, skip the drain and escalate to human. The query is: `if remaining_healthy_replicas - 1 < slo_minimum: skip_and_alert()`.

**Trade-offs to volunteer:**
- ✅ These three layers together virtually eliminate cascading remediation storms
- ❌ They also mean that in a massive outage, the system may appear "slow" to remediate — it's deliberately throttled
- ✅ The throttling is the right behavior in a correlated failure — you want humans involved, not automated systems making it worse
- ❌ SLO-aware gating requires that every service has a declared minimum replica count, which requires buy-in from service teams

#### Deep Dive 2: False-Positive-Free Database Failover (Phi Accrual + STONITH)

**The problem:** False failovers are catastrophic for stateful services. A healthy MySQL primary that gets incorrectly failed over experiences: (a) a write outage during the failover window, (b) potential data loss if the replica wasn't fully caught up, and (c) a long recovery to reconcile the topology afterward.

**The solution — two independent mechanisms:**

Mechanism 1: **Phi Accrual Failure Detector.** Instead of a fixed "N misses = failure" threshold, Phi Accrual models the statistical distribution of heartbeat arrival intervals. The formula is `φ = -log₁₀(1 - F(time_since_last_heartbeat))` where F is the CDF of a normal distribution fitted to the last N heartbeat intervals. φ > 8 means 99.9997% confidence the node has failed. The key insight: if the network gets jittery, the distribution widens automatically, and φ increases more slowly — preventing false positives during network events without any manual threshold tuning.

Mechanism 2: **N-of-M consensus.** Deploy 3 failure detector instances across 3 AZs. Require 2-of-3 to agree before starting failover. A single detector's view of the world is not trusted. If all 3 disagree, no failover, and a human is alerted. This handles the case where one detector is in a network partition from the target but the other two can reach it fine.

**Then STONITH to prevent split-brain:** Once consensus says "fail over," before promoting any replica, physically stop the old primary (IPMI power-off, network isolation). Acquire a monotonically increasing fencing token from etcd. The promoted replica only starts accepting writes after acquiring the new token. If the old primary somehow comes back, it has a stale token and all writes are rejected. Without STONITH, two primaries can diverge, and data reconciliation may be impossible.

**Trade-offs to volunteer:**
- ✅ Phi Accrual + 2-of-3 consensus gives false failover rate < 0.01%
- ❌ The consensus round adds 2-3 seconds to detection time — acceptable because avoiding a false failover is worth it
- ✅ STONITH is the only reliable way to prevent split-brain on bare-metal
- ❌ If the BMC (IPMI) is unreachable, you cannot fence, and you must abort the failover rather than risk split-brain — the system prefers safety over availability in this case
- ❌ Fencing via IPMI power-off takes 3-5 seconds; network isolation is faster but less reliable

#### Deep Dive 3: Runbook Selection and Execution Safety

**The problem:** With 5,000+ runbooks covering dozens of failure categories and infrastructure types, how does the system select the right runbook for a novel combination of symptoms? And once selected, how do you execute a runbook safely when each step could have production side effects?

**The solution for selection — ML + RAG:**
A Symptom Classifier takes structured failure signals (entity type, failing check types, error patterns, labels) and classifies them into failure categories. A RAG-based Runbook Recommender then retrieves the most semantically relevant runbook for the classified failure, returning a ranked list with confidence scores. If the top candidate has confidence > 0.8, it auto-selects and starts execution. Below 0.8, it presents candidates to a human for selection. An Outcome Learner tracks which runbook was actually used and whether it succeeded, feeding back into the classifier and recommender over time.

**The solution for execution safety — four guards:**

Guard 1: **Preconditions before every action step.** Before draining a node, verify it's schedulable. Before a MySQL failover, verify the replica exists and is reachable. If a precondition fails, stop and escalate rather than proceeding with an invalid assumption.

Guard 2: **Postconditions after every action step.** After draining a node, verify it has no running pods. After failover, verify replication is flowing and write queries succeed. If a postcondition fails, execute rollback.

Guard 3: **Rollback engine.** Every action step defines a rollback. The rollback engine tracks which steps executed and, on failure, executes their rollbacks in reverse order. The execution record in MySQL tracks `rollback_status` per step so the engine can resume if it crashes mid-rollback.

Guard 4: **Approval gates.** High-impact steps (PXE reimaging, database failover, datacenter-level changes) are tagged `requires_approval: true`. The execution pauses, sends a structured Slack/PagerDuty message to the runbook owner, and waits up to 15 minutes for approval. Timeout without approval escalates to the next oncall tier.

**Trade-offs to volunteer:**
- ✅ Pre/postconditions catch the wrong-state execution errors that cause the worst production incidents
- ❌ They add latency — each condition check can take seconds, and an 8-step runbook might add 30-60 seconds total overhead
- ✅ Approval gates for high-impact actions are the right balance of automation and human oversight
- ❌ An approval gate with a 15-minute timeout means some failures sit unresolved for up to 15 minutes waiting for a human

---

### 3g. Failure Scenarios

Senior engineers demonstrate depth by knowing how systems fail, not just how they work. Walk through these proactively.

**Scenario 1: The failure detector itself fails.** If 1 of 3 detectors goes down, the remaining 2 still form a majority (2-of-2 becomes unanimous required). System degrades gracefully. If 2 of 3 go down, no consensus is possible → no automated actions → alert human. The recovery system degrades to alerting-only rather than taking unsafe actions. This is the safe-by-default design.

**Scenario 2: Flapping — a service oscillates between healthy and unhealthy.** Without flap detection, the remediation engine would keep restarting the service in a tight loop, making the situation worse. The flap detector tracks state transitions per entity per 1-hour window. If transitions exceed the threshold (e.g., 10 per hour), it marks the entity as flapping, suppresses further automated remediation, and escalates to human. The entity gets a `flap_suppressed_until` timestamp.

**Scenario 3: STONITH fails during MySQL failover.** If the IPMI is unreachable and network isolation also fails, you cannot definitively stop the old primary. The only safe action is to abort the failover and alert the on-call team. Do NOT promote a replica when you cannot guarantee the old primary is stopped — that path leads to split-brain and data loss. This is where the system explicitly chooses availability loss over data loss.

**Scenario 4: Runbook execution crashes mid-step.** The execution orchestrator persists state to MySQL after every step, including `current_step_index` and each step's `rollback_status`. On recovery (process restart or failover of the orchestrator itself), it reads the execution record, determines what completed and what didn't, and either resumes from the last successful step or triggers rollback. This makes runbook execution crash-safe.

**Scenario 5: A chaos experiment's orchestrator crashes mid-injection.** All injections are left active indefinitely. The dead-man's switch prevents this: the chaos engine sends heartbeats every 10 seconds. All chaos agents watch for the heartbeat. If 30 seconds pass with no heartbeat, every chaos agent autonomously rolls back its active injections. Additionally, each injection has a maximum TTL (default 60 seconds) after which the agent self-heals regardless.

**Senior framing for failure scenarios:** The pattern across all five systems is "fail safe, not fail open." When uncertain, do not act. When fencing fails, abort failover. When detectors disagree, page a human. When blast radius is exceeded, queue rather than override. Every default should lean toward inaction until the situation is confirmed.

---

## STEP 4 — COMMON COMPONENTS

Every component listed in common_patterns.md, with depth on why it's used, how it's configured, and what breaks without it.

### MySQL 8.0 as Source of Truth for Recovery State

**Why used:** Every recovery system has a lifecycle state machine — executions go through pending → running → succeeded/failed, failovers go through detecting → fencing → promoting → verifying → completed. These state transitions need ACID guarantees. You cannot lose a "currently remediating" record if the service crashes and restarts, because that would allow a second execution to start on the same target simultaneously.

**Key configuration:**
- All five systems store their primary lifecycle tables in MySQL
- Tables are time-partitioned by day/hour for efficient cleanup: `PARTITION BY RANGE (UNIX_TIMESTAMP(created_at))`
- Hot tier: 7 days, warm tier: 30 days, cold/archive: 90 days+, then truncate old partitions rather than DELETE
- Execution locks (distributed locking) stored in MySQL with `expires_at` timestamps, preventing double-execution on the same target

**Without it:** You'd use something eventually consistent (like Cassandra) and risk executing two remediations on the same target simultaneously. Or you'd use an in-memory store and lose all in-progress execution state on service restart.

**Tables by system:**
- Runbook: `runbook`, `execution`, `step_execution`, `approval_request`, `symptom_runbook_mapping`, `execution_lock`
- Failover: `managed_cluster`, `cluster_member`, `failover_event`, `fencing_token`, `dns_failover_record`
- Chaos: `experiment`, `injection`, `steady_state_measurement`, `experiment_result`, `gameday`
- Health Check: `health_check_instance`, `entity_health_state`, `check_state`, `remediation_record`, `escalation_policy`
- Self-Healing: `detected_failures`, `remediation_actions`, `node_state`, `cooldown_periods`, `health_check_results`

---

### Redis for Fast Operational State

**Why used:** The hot path — checking whether an entity is in cooldown, computing the current composite health score, acquiring a distributed lock — runs on every remediation decision and must be sub-millisecond. MySQL's commit latency is too high for per-event checks at 450,000 events/second.

**Key configuration:**
- Current composite health scores: `HSET entity:{entity_id} score 87.5 status degraded` → 420 MB total, fits in a single Redis instance
- Cooldown timers: `SET cooldown:{target_id}:{action_type} 1 EX 300` (expires in 300 seconds)
- Distributed execution locks: `SET lock:{target_id} {execution_id} NX EX 3600` — the NX flag is atomic compare-and-set (acquire only if not exists)
- Flap detection state: per-entity sliding window of state transitions, stored as a Redis sorted set by timestamp
- Rate limit counters: `INCR rate:{action_type}:{cluster_id}` with TTL; if > threshold, reject and queue

**Without it:** Every remediation decision requires a MySQL query. At 450,000 health events/second, that's 450,000 MySQL reads/second — MySQL would be overwhelmed, the system falls over, and the recovery system is itself the source of an outage.

---

### Elasticsearch for Searchable History and Logs

**Why used:** Post-incident analysis requires answers to questions like: "Show me all failed executions for node prod-042 in the last 30 days," "What runbooks were selected for MySQL replication lag failures?", "Which services had flapping health checks last week?" These are full-text search queries over time-series event data. MySQL with a table scan is too slow; Elasticsearch with inverted indexes and time-range queries is purpose-built for this.

**Key configuration (across systems):**
- Runbook: Index `runbooks` (full-text on name, description, YAML content) + `execution-logs-YYYY-MM` (searchable step outputs)
- Health Check: `health-events-YYYY-MM` for raw check results; 7-day hot (SSD), 30-day warm (HDD), 90-day cold
- Self-Healing: `remediation-events` for audit search, `health-signals` for raw event history
- Chaos: `injection-logs` and `metric-snapshots` indexed by experiment_id and timestamp
- All indexes use ILM (Index Lifecycle Management) to automatically roll to warm, cold, and delete

**Without it:** Full-text search over 12.6 TB of health events would require full MySQL table scans — minutes per query. Post-incident analysis becomes painful, SLO reporting becomes batch jobs, and debugging active incidents is slow.

---

### etcd for Consensus and Leader Election

**Why used:** Three of the five systems (Failover, Chaos, Self-Healing) need distributed consensus for leader election and coordination state. etcd provides linearizable reads and writes — when you write a fencing token, every subsequent read from any etcd client sees the latest value. This is required for split-brain prevention: the fencing token must be globally consistent.

**Key configuration:**
- Failover: `fencing_token (cluster_id, token_value BIGINT UNSIGNED)` — monotonically increasing; new primary acquires new token before accepting writes
- Failover: Leader election for the Failover Orchestrator (ensures only one orchestrator drives a given cluster's failover)
- Self-Healing: Desired state stored in etcd; the reconciliation loop reads desired state and observes actual state, then computes corrective actions
- Chaos: Experiment engine leader election; also used for distributed coordination of concurrent experiments
- Watch semantics: etcd's watch API lets reconciliation loops react to desired state changes in real time rather than polling

**Without it:** You'd need to implement your own distributed lock (error-prone) or use MySQL with SELECT FOR UPDATE (slower, not designed for tight coordination loops). etcd's 8 GB storage limit means it's for coordination state only — bulk data (heartbeat history, audit logs) stays in MySQL/ES.

---

### Kafka as Event Bus

**Why used:** Three of the five systems (Health Check, Self-Healing, Chaos) generate high-volume event streams that need to be consumed by multiple independent services. Kafka decouples producers (health check agents, chaos agents) from consumers (composite score calculator, flap detector, remediation engine), provides replay for debugging and catching up after a consumer restart, and buffers spikes without losing events.

**Key configuration:**
- Health Check: Topics `health-check-results` (raw), `normalized-health-events`, `health-state-changes`, `confirmed-failures`, `remediation-triggers`
- Self-Healing: Topics `health-events` (430,000 events/sec), `failure-candidates`, `confirmed-failures`, `remediation-commands`
- Chaos: Topic `injection-commands` (control plane → agents), `injection-status` (agents → engine)
- Partition count: `health-events` needs ~20 partitions to handle 90 MB/sec at manageable per-partition throughput
- Retention: 7 days for raw events (enough to replay for a major incident investigation), shorter for high-volume topics

**Without it:** Health check agents would need to directly call the composite score calculator, the flap detector, and the remediation engine — tight coupling that breaks when any consumer is slow or down. You'd lose events during consumer restarts. You'd have no replay capability for debugging.

---

### Consensus-Based Detection (N-of-M Agreement)

**Why used:** Any single detector is unreliable. A network partition between detector-AZ1 and target-node-AZ2 makes the node look down to that detector while it's perfectly healthy. A single misconfigured detector can fire false positives continuously. Consensus — requiring 2-of-3 independent detectors to agree — brings the false positive rate from ~1% (single detector) down to < 0.01%.

**How it works across systems:**
- **Failover:** 3 detectors per region (one per AZ). Each independently sends heartbeats to all managed clusters. A failure is confirmed when 2-of-3 report φ > 8 for the same target. If all 3 disagree, no failover and a human is alerted.
- **Health Check:** 3 consecutive failures at the configured interval (e.g., 10 seconds × 3 = 30 second confirmation window). State changes are only propagated downstream after the threshold is crossed.
- **Self-Healing:** N-of-M health checkers; `consensus_count` field on `detected_failures` tracks how many detectors agree. Only promoted to `confirmed` when consensus count meets threshold.
- **Chaos:** Pre-injection steady-state validation uses multiple PromQL metrics; if any metric is outside bounds, the experiment is blocked from starting.

**Without it:** Any network blip, any transient load spike, any monitoring agent restart causes automated remediation. At scale (100,000+ nodes), this creates a constant background noise of false remediations that degrade reliability rather than improving it.

---

### Blast Radius Control

**Why used:** Correlated failures — where many entities have the same issue at the same time — are common in practice (bad config push, kernel update, network event). Without blast radius limits, the remediation system would drain or restart the entire affected fleet simultaneously, destroying capacity and causing a worse outage than the original failure.

**Formula and configuration:**
```
max_concurrent_drains_per_cluster = max(floor(cluster_size × 0.05), 5)
max_reimages_per_rack = 1
max_targets_per_runbook = min(floor(total × max_pct/100), max_absolute_count, max_per_rack)
chaos_blast_radius = {"max_percentage": 10, "max_absolute_count": 5, "max_per_az": 2}
```

**Implementation:** A rate limiter (Redis counter with TTL) tracks how many concurrent actions of each type are in flight for each cluster. Before dispatching a new action, check the counter. If at limit, put the action in a priority queue. When an in-flight action completes, decrement the counter and process the next queued action.

**Without it:** An automated system that detects 500 unhealthy nodes and simultaneously drains all 500 will destroy the cluster. This is a well-documented failure mode from early automation systems — the cure kills the patient.

---

### Cooldown Periods and Escalation Tiers

**Why used (cooldown):** Automated remediation is not guaranteed to fix the underlying problem. If a node reboots and comes back unhealthy 60 seconds later, blindly rebooting it again in a tight loop wastes time, disrupts workloads, and creates alert fatigue without fixing anything. Cooldown periods enforce a minimum time between successive remediations on the same target, giving the system time to stabilize and the team time to investigate.

**Cooldown configuration:**
- Process restart: 5-minute cooldown
- Pod restart: 2-minute cooldown
- Node drain: 30-minute cooldown
- Bare-metal reimage: 2-hour cooldown
- Database failover: 1-hour cooldown (prevents back-and-forth oscillation)

**Why used (escalation tiers):** Automation handles the common cases. Humans are better at the unusual, novel, or multi-system failures. The escalation policy auto-advances through tiers when the current tier's action doesn't resolve the failure within the timeout.

**Typical escalation policy:**
- Tier 1 (auto-remediate): First 5 minutes. Try automated fix.
- Tier 2 (alert team Slack): 5-15 minutes. If not resolved, alert the owning team.
- Tier 3 (page on-call): 15-30 minutes. If still unresolved, page the on-call engineer.
- Tier 4 (page senior on-call): 30+ minutes. Escalate to senior for major incidents.

**Without cooldowns:** Remediation storms where 100 restarts of the same node happen in an hour. Without escalation: failures that automation can't fix silently persist while no human knows anything is wrong.

---

### Immutable Audit Trail

**Why used:** Every automated action has consequences. Post-incident analysis, regulatory compliance, and debugging all require knowing exactly what the system did, when, to what, and with what result. Audit logs must be append-only (no updates, no deletes within retention window) so that they can be trusted.

**Structure across systems:** Every audit record includes: actor (service account or human username), timestamp, target entity, action type, trigger source, before state, after state, outcome (success/failure/partial), and result details. Execution locks and approval records are also retained.

**Key fields for debugging:** `rpo_achieved_ms` and `rto_achieved_ms` on failover events let you evaluate whether the system met its SLOs for each real failover. `slo_check_result JSON` on remediation records shows why an action was skipped (SLO gate triggered). `rollback_status` per step shows exactly which steps succeeded and which triggered rollback.

**Without it:** Post-incident analysis is archaeology. You can't prove what the system did vs. what a human did. Compliance audits fail. Debugging recurrent issues is guesswork.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Auto-Remediation Runbook System

**What makes it unique:** This is the only system in the pattern that encodes multi-step, branching, conditional operational knowledge as executable code. The **decision tree engine** evaluates Jinja2-templated conditions on real-time diagnostic outputs — `if smart_output.Reallocated_Sector_Ct > 500: goto decommission_path`. The **ML runbook selector** (classifier + RAG recommender + outcome learner) handles the hard problem of "given these symptoms, which of 5,000 runbooks should I run?" without requiring a human to manually pick. Pre/postconditions on every step, plus a rollback engine that operates in reverse order, make execution safe for production infrastructure.

**Decisions different from other systems:** Runbook YAML stored in Git (not MySQL) for version control, diff, and code review. Four-store architecture (Git, MySQL, Elasticsearch, Redis) versus two or three stores in other systems. ML confidence threshold of 0.8 as the hard cutoff for auto-selection vs. human escalation.

**Two-sentence differentiator:** The Runbook System is the knowledge repository of the automated recovery stack — it's where you encode "what a senior SRE would do in this situation" as executable, version-controlled, auditable code rather than wiki pages. Its unique contribution is the decision tree + ML recommendation layer that automatically matches novel failure symptoms to the right playbook, backed by precondition/postcondition guards and a rollback engine that makes every step production-safe.

---

### Automated Failover System

**What makes it unique:** The most sophisticated detection algorithm in the pattern — **Phi Accrual** adapts its failure threshold to historical heartbeat jitter, automatically calibrating to network conditions without operator tuning. **STONITH with monotonically increasing fencing tokens** is the only mechanism that actually prevents split-brain (as opposed to hoping the old primary stays down). The **replica selection scoring algorithm** (`is_sync_replica × 1000 + different_az × 100 + 1/(lag+1) × 50 + priority × 10`) makes a principled choice about which replica minimizes data loss.

**Decisions different from other systems:** Uses etcd not just for leader election but as the primary consistency mechanism for fencing tokens and cluster state. ProxySQL for transparent MySQL connection rerouting — applications don't need to know about the failover. Hard latency targets (MySQL < 30s, stateless < 5s) that require pre-built standby replicas and tight heartbeat intervals.

**Two-sentence differentiator:** Automated Failover's defining property is its zero-trust approach to failure confirmation — it will not promote a replica without Phi Accrual consensus from 2-of-3 detectors and confirmed physical fencing of the old primary, because the cost of a false failover (split-brain data divergence) exceeds the cost of a few extra seconds of detection latency. Its replica selection score deliberately weighs synchronous replication and AZ diversity above raw replication lag, encoding the judgment that zero data loss and AZ fault tolerance matter more than raw failover speed for tier-1 services.

---

### Chaos Engineering Platform

**What makes it unique:** The only system in the pattern that is **proactive** rather than reactive — it intentionally induces failures to validate resilience assumptions before real failures expose them. The **dead-man's switch** (30-second engine heartbeat timeout triggering emergency rollback by every chaos agent autonomously) is a unique safety mechanism not found in other systems. The **GameDay Orchestrator** enables multi-step failure simulations (kill primary DB + kill 20% of pods + inject network partition simultaneously) for rehearsing incident response. The `tier_restriction DEFAULT staging_only` field is an architectural expression of the principle that production chaos requires explicit opt-in.

**Decisions different from other systems:** The only system with a DaemonSet on every node (Chaos Agent) rather than centralized executors. The only system with a steady-state hypothesis framework (define measurable baseline, validate before/during/after). Feature flag integration to exclude critical services from injection.

**Two-sentence differentiator:** Chaos Engineering is the immune system of the recovery stack — it stress-tests the other four systems to verify they actually work under pressure, not just in theory. Its unique safety architecture (dead-man's switch + per-agent TTL + blast radius limits + tier_restriction guard + pre-injection steady-state validation) reflects the hard lesson that chaos experiments can cause real outages if not carefully controlled.

---

### Health Check and Remediation Service

**What makes it unique:** The only system in the pattern with a **composite weighted health score** (0-100) that aggregates multiple check types with different weights — deep health (0.35) > HTTP (0.25) > port (0.15) > process (0.10) > hardware (0.10) > kernel (0.05). **Deep health checks** test actual dependency connectivity (DB `SELECT 1`, cache SET/GET roundtrip, downstream service reachability) not just port reachability — distinguishing "the process is up but can't connect to its database" from "the process is truly healthy." **Flap suppression** (> 10 state transitions per hour → suppress remediation) prevents thrashing on intermittent issues. **SLO-aware gating** before every drain/cordon action is unique to this system.

**Decisions different from other systems:** Highest scale requirement in the pattern — 2 million health checks per second, requiring distributed check agents, Kafka event bus, and a stream processing layer for composite scores. The only system with a health check template library enabling standardized check patterns across service teams.

**Two-sentence differentiator:** The Health Check Service is the most data-intensive system in the pattern, operating at 2 million checks/second and needing sub-100-millisecond composite scoring for 2.1 million entities simultaneously. Its unique value is the deep health check (weight 0.35, testing actual dependency connectivity) combined with SLO-aware gating — it is the only component that knows whether a proposed remediation action is safe to execute without violating downstream service contracts.

---

### Self-Healing Infrastructure Platform

**What makes it unique:** The only system with **level-triggered reconciliation** — the same pattern as Kubernetes controllers — where the loop continuously compares desired state (from etcd) with observed actual state, computes the diff, and drives convergence. This is fundamentally different from event-driven remediation because it is self-correcting after any kind of crash or state divergence. The **out-of-band IPMI/BMC controller** is unique: it operates over a separate management network and can recover hosts whose OS has completely crashed, whose in-band agent is unreachable, or whose kernel has panicked. The **PXE reimaging pipeline** provides bare-metal recovery from OS corruption — a capability that doesn't exist in VM or container-only systems.

**Decisions different from other systems:** etcd as the desired state store is a core architectural choice (not just for coordination). Node Problem Detector DaemonSet (NPD) detects K8s-specific node conditions (MemoryPressure, KernelDeadlock) and writes them back to the K8s API server. `max_reimages_per_rack = 1` as a hard blast radius limit for bare-metal recovery.

**Two-sentence differentiator:** Self-Healing Infrastructure is the lowest-level system in the pattern — it heals the physical substrate that all other systems depend on, using out-of-band IPMI/BMC (not the OS itself) to recover hosts whose operating system is unresponsive. Its level-triggered reconciliation loop (desired vs. actual state every 10 seconds, idempotent corrective actions) provides crash-safe self-healing without distributed transactions — if the control plane restarts, it simply re-converges on the next loop iteration.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (5-7 minutes each; expected in first 30 minutes of interview)

**Q: What is the difference between a liveness check and a readiness check, and why does it matter for automated recovery?**

A: A **liveness check** answers "is this process alive and not hung?" — if it fails, the process should be restarted. A **readiness check** answers "is this process ready to serve traffic?" — if it fails, the process should be removed from load balancer rotation but not restarted, because it might be warming up, draining connections, or waiting for a dependency. The distinction matters enormously for automated recovery because the remediation action is different: failing liveness → restart pod; failing readiness → remove from LB pool and wait, don't kill the process. A system that doesn't distinguish these will restart processes that are legitimately starting up, creating restart storms.

**Q: What is split-brain in the context of database failover, and how do you prevent it?**

A: **Split-brain** is the condition where two database nodes simultaneously believe they are the primary and both accept write traffic. This happens when a primary appears unreachable to the failover system (due to a network partition) but is actually still running and writing. If you promote a replica without confirming the old primary is stopped, both nodes accept writes, their data diverges, and you cannot automatically reconcile them. Prevention requires **STONITH** (Shoot The Other Node In The Head) — physically stopping the old primary via IPMI power-off or network isolation — and **fencing tokens** (a monotonically increasing token that the old primary must validate before accepting any write; if it has a stale token, writes are rejected). You must confirm fencing succeeded before promoting. If you can't confirm fencing, abort the failover — unavailability is better than data corruption.

**Q: What is a blast radius limit, and give me a concrete example of how you'd configure it?**

A: A **blast radius limit** is a cap on how many entities of a given type can be simultaneously affected by automated remediation actions. The goal is to ensure that correlated failures (many nodes failing at once due to a common cause) don't result in the remediation system making the situation worse by taking down even more capacity. Concrete example: for a 200-node Kubernetes cluster, you set `max_concurrent_node_drains = max(floor(200 × 0.05), 5) = max(10, 5) = 10`. If 50 nodes all develop the same health issue simultaneously, only 10 are drained at a time. The other 40 are queued. This keeps at least 150 nodes running while repairs proceed. A stricter example for bare-metal: `max_reimages_per_rack = 1` ensures that if an entire rack's OS becomes corrupted, you reimage one host at a time rather than taking the entire rack offline simultaneously.

**Q: What is the Phi Accrual failure detector and why is it better than a fixed-timeout approach?**

A: With a **fixed-timeout approach**, you declare a node failed if you haven't heard from it in N seconds. This requires you to set N high enough to avoid false positives on a noisy network but low enough to detect real failures quickly — and the right value changes as network conditions change. **Phi Accrual** instead models the statistical distribution of heartbeat arrival intervals for each node and computes a suspicion score φ that tells you how likely it is that the node has actually failed (not just paused). The formula is `φ = -log₁₀(1 - F(time_since_last_heartbeat))` where F is the CDF of the normal distribution fitted to recent heartbeat intervals. When network jitter increases (heartbeats are more variable), the distribution widens, and φ increases more slowly — automatically preventing false positives during network events. When the network is smooth, φ rises quickly on a real failure — giving faster detection. You set a fixed φ threshold (e.g., 8 for 99.9997% confidence) and let the timing adapt automatically.

**Q: What is a dead-man's switch in the context of chaos engineering, and why is it essential?**

A: A **dead-man's switch** is a failsafe that automatically undoes all active chaos injections if the chaos engineering orchestrator stops heartbeating. The scenario it prevents: the chaos engine injects network latency into 100 pods, then crashes. Without a dead-man's switch, those 100 pods are permanently degraded with injected network latency until someone manually rolls back each injection. With a dead-man's switch: each chaos agent monitors the engine's heartbeat (every 10 seconds). If 30 seconds pass with no heartbeat, every agent autonomously rolls back its active injections. This is essential because chaos experiments affect production infrastructure, and the chaos platform crashing during an experiment should make things better, not leave them broken indefinitely.

**Q: What does "SLO-aware gating" mean and when would you use it?**

A: **SLO-aware gating** means checking whether a proposed automated action would violate the declared Service Level Objective of an affected service before executing it. The canonical example: before automatically draining a Kubernetes node (which evicts all pods on it), query how many healthy replicas each service currently has. If any service has exactly its SLO-minimum number of replicas (say, SLO requires 3 replicas and there are currently 3 healthy), draining that node would bring it to 2 — violating the SLO. The gating check returns "unsafe" and the drain is skipped, escalating to a human instead. You use this for any destructive automated action where the action could reduce availability below a declared minimum: node drains, cordon, instance termination, failover of a replica that's the last known-good copy.

**Q: Walk me through the data stores you'd use in a health check and remediation system and why each one.**

A: Four stores, each chosen for what it does best. **MySQL** for definitions (health check configurations, escalation policies, remediation rules) and lifecycle state (what's currently being remediated, audit trail) — you need ACID guarantees so you don't process the same entity twice or lose a "remediating" record on crash. **Redis** for operational hot state (current composite health score per entity, cooldown timestamps, distributed execution locks, rate limit counters) — this is on the critical path of every check result, so it must be sub-millisecond; Redis SETNX gives you atomic lock acquisition. **Elasticsearch** for historical search (12.6 TB of raw health events, post-incident queries like "show all failing checks for node X in the last 30 days") — inverted indexes make full-text search and time-range queries fast where MySQL table scans would be unusable. **Kafka** as the event bus between check agents and downstream processors — decouples the producers (agents doing 450,000 checks/sec) from consumers (score calculator, flap detector, remediation engine) so any consumer can fall behind or restart without losing events.

---

### Tier 2 — Deep Dive Questions (expect 10-15 minutes each; asked after basic design is drawn)

**Q: How do you design the MySQL failover state machine to guarantee the fencing token is always acquired before promotion?**

A: The failover orchestrator uses a **compare-and-swap on etcd** to atomically acquire the fencing token. The token value is a monotonically increasing integer. The CAS operation is: "set `fencing_token[cluster_id]` to `current_max + 1` only if it's currently `current_max`." If the CAS succeeds, the orchestrator holds the fencing token exclusively. The promoted replica is configured to include the token value in its ProxySQL hostgroup configuration — writes are only accepted if the writer presents the current token value. If the old primary comes back and tries to write, it has the old token value, which is rejected. The state machine enforces: FENCING state (token acquisition + STONITH confirmation) must complete before transitioning to PROMOTING state. If the orchestrator crashes between acquiring the token and completing fencing, the etcd TTL on the token expires, and a fresh orchestrator acquires a new token (incrementing again) and restarts from FENCING. This guarantees you never reach PROMOTING without a valid, current fencing token.

**Q: In a health check system at 450,000 events/second, how do you compute composite health scores without creating a bottleneck?**

A: Three design choices working together. First, **sharding by entity**: partition the Kafka `health-check-results` topic by `entity_id` hash. Each partition is consumed by one composite score calculator instance. Scores for entity X always flow to the same calculator, so there's no cross-instance coordination needed for per-entity state. With 20 partitions and 20 calculator instances, each handles ~22,500 events/second — very manageable. Second, **incremental updates**: the calculator maintains per-entity state in Redis. When a new check result arrives for entity X, it reads the current state for that check, updates it, recomputes the weighted average, and writes the new score to Redis. This is O(1) per event rather than O(checks-per-entity). Third, **suppress no-ops**: if a check result arrives and the status didn't change (still "healthy" → "healthy"), do not update Redis and do not publish to downstream topics. Roughly 90% of events are stable-state no-ops, so this reduces downstream load by ~10x. The composite score calculation at 100ms target latency is achievable with this architecture — Redis SET/GET is ~0.1ms, plus ~1ms Kafka consume latency.

**Q: How do you handle the case where automated remediation makes a failure worse — how does the system detect and stop itself?**

A: Multiple safeguards operating at different layers. At the event level: **flap detection** — if an entity oscillates between healthy and unhealthy more than a threshold number of times in a sliding window (e.g., 10 transitions per hour), it's marked as flapping and suppressed from automated remediation. The system recognizes "this entity doesn't stay fixed after we act on it" as a signal that our remediation isn't the right fix. At the fleet level: **blast radius + rate limiting** — if automated remediation was making things worse at scale, the failure rate would increase, not decrease. The system tracks the success rate of remediation actions over a rolling window. If the success rate for a given action type falls below a threshold (e.g., < 30% of restarts result in the pod staying healthy), the system pauses that action type and alerts the team. At the runbook level: **postcondition failures** trigger immediate rollback and escalation. If a runbook step's postcondition doesn't pass within the timeout window, the assumption that "this fix worked" is invalidated and the system rolls back and escalates. At the chaos level: **steady-state validation** continuously checks whether the key SLO metrics (p99 latency, error rate) are within bounds during an experiment — if they drift more than 50% from baseline, the system aborts automatically.

**Q: Explain the replica selection algorithm for MySQL failover and the trade-offs in your design.**

A: The algorithm scores each candidate replica and selects the one with the highest score: `score = (is_sync_replica × 1000) + (different_az × 100) + (1/(replication_lag_ms + 1) × 50) + (priority × 10)`. The weight coefficients encode a priority hierarchy. **Sync replica preference (×1000)** is the dominant factor — a synchronous replica has zero replication lag by definition (the primary waited for its ACK before committing), so promoting it means zero data loss. An asynchronous replica might be seconds or minutes behind. **AZ diversity (×100)** is the second priority — if the primary failed due to an AZ-level event, promoting a replica in the same AZ risks the same failure affecting the new primary. **Replication lag (×50)** breaks ties between candidates of the same type — prefer the replica most caught up. **Admin-configured priority (×10)** allows operators to pin a preferred replica for non-technical reasons (e.g., hardware generation, memory size).

Trade-offs: ✅ The sync replica preference is strong — in most cases it produces the correct choice (lowest RPO). ❌ In an AZ failure where the only sync replica is in the failed AZ, you fall through to the next best option (lowest-lag async replica in a surviving AZ), potentially accepting some data loss. ✅ This is the correct trade-off for an AZ failure: prefer survival (functioning system) over zero-data-loss at the cost of availability. ❌ The scoring system doesn't account for the promoted replica's hardware specs or current load — a under-resourced replica selected as primary will immediately become a performance bottleneck. Senior engineers add a "replica capacity check" as a precondition before promotion.

**Q: How does the runbook system prevent a runbook from being updated while an execution of an older version is in progress?**

A: The execution record stores `runbook_version INT` at the time of execution start — it is pinned to a specific version. The system fetches the runbook YAML from Git at the pinned SHA (`git_commit_sha` on the `runbook_version` record), not the latest version. Updating the runbook in Git creates a new `runbook_version` record (new commit SHA, incremented version number) but does not affect in-flight executions. In-flight executions continue running against the version they started with. After the execution completes (success, failure, or rollback), the next triggered execution will use the latest approved version. The version pinning also means the audit trail for any execution shows exactly what code ran — you can reproduce the runbook logic as it was at the time, not the current version which may have been updated since. The `yaml_content_hash (SHA-256)` on each version provides tamper detection.

**Q: Design the escalation policy for a health check and remediation service. What are the tiers and how do you prevent alert fatigue?**

A: A four-tier escalation policy with explicit timeouts between tiers, and explicit resolution conditions to prevent unnecessary progression. Tier 1 (0-5 minutes): automated remediation executes. The system tries the configured automated fix (restart pod, drain node, failover replica). No human notification yet — the assumption is that most failures resolve automatically. Tier 2 (5-15 minutes): if the entity is still unhealthy after Tier 1, send a Slack alert to the owning team's channel with the entity, failure type, what was tried, and current state. This is informational, not paging. Tier 3 (15-30 minutes): if still unhealthy, page the on-call engineer via PagerDuty. This should be relatively rare (most issues resolve by Tier 2). Tier 4 (30+ minutes or severity=critical): page the senior on-call.

Alert fatigue prevention: **suppress on flapping** (if the entity is marked flapping, escalation is suppressed — only one human notification, not a continuous stream). **Cooldown between notifications** (if you already sent a Slack message 3 minutes ago, don't send another). **Auto-resolve notifications** (when the health score recovers, send a "resolved" message to the same channel that received the alert). **Tier 1 success rate reporting** (if Tier 1 auto-remediation is resolving 95%+ of issues, humans only see the 5% that are genuinely novel).

**Q: How do you ensure idempotency in automated remediation actions?**

A: Three layers. First, **execution locks**: before dispatching any remediation action, acquire a distributed lock on the target entity (`SET lock:{target_id}:{action_type} {execution_id} NX EX 3600` in Redis). The `NX` flag means "only set if not exists" — atomic check-and-set. If the lock already exists, a concurrent action is in progress; skip or queue rather than double-executing. Second, **idempotent action design**: each action plugin is designed to be a no-op if the desired state is already achieved. `drain_node`: if the node is already drained (no pods scheduled), return success without doing anything. `restart_pod`: if the pod is already restarting or already healthy, return success. `promote_replica`: if the target is already primary, return success. Third, **execution state check in MySQL**: before dispatching, query `execution WHERE target_entity = ? AND status IN ('pending', 'running')`. If an in-flight execution exists, do not create a new one. The execution ID stored in the Redis lock ties back to the MySQL execution record, so you can always look up who owns the lock and why.

---

### Tier 3 — Staff+ Stress Tests (reason aloud; 15-20 minutes each; separates senior from staff)

**Q: A widespread infrastructure event causes 2,000 of your 100,000 bare-metal nodes to fail health checks simultaneously within 60 seconds. Walk me through exactly what your automated recovery system does and where it might make things worse.**

A: This is the "correlated large-scale failure" scenario, and the honest answer is: the automated recovery system's job is to *not* make this worse, not to fix it.

Within the first 60 seconds, the health check event stream spikes to 2,000 failure notifications above baseline. The Kafka consumer group handling `health-state-changes` sees 2,000 new "unhealthy" events. The composite score calculator updates 2,000 entity scores. The flap detector checks each entity — since these are fresh failures (not oscillating yet), flap detection doesn't suppress them.

The Remediation Decision Engine now checks the three gates for each of 2,000 entities. The blast radius check is: `max_concurrent_drains_per_cluster = max(cluster_size × 0.05, 5)`. For a 10,000-node cluster, that's max 500 concurrent drains. For 2,000 failures across multiple clusters, the system would start draining up to 500 nodes per cluster simultaneously — that's actually a large number.

**Where it gets worse:** If those 2,000 nodes are in 4 clusters of 5,000 each, the blast radius per cluster is 250 simultaneous drains. Draining 250 nodes from a 5,000-node cluster removes 5% of its capacity. But if those 250 nodes were carrying 10% of the workload, the remaining nodes are now overloaded — potentially failing their health checks too, causing a cascading drain.

**What the system should do (and what you'd add):** First, the system should detect the *correlation* — 2,000 failures in 60 seconds is not 2,000 independent failures, it's one failure with 2,000 victims. Correlated failure detection (e.g., "failure rate exceeded 5% of fleet in the last 60 seconds") should trigger a global pause on all automated remediation and an emergency page. Second, the escalation policy for a large-scale event should bypass Tier 1 entirely and go directly to Tier 3 (page on-call). The automated system is not equipped to fix a fleet-wide kernel panic or network event — only humans who can identify and roll back the common cause can fix this. Third, the blast radius limits should be adaptive: if the current failure rate is > X%, halve the blast radius cap until the rate subsides.

The correct answer a Staff+ engineer gives: "My automated recovery system's job in this scenario is to preserve as much capacity as possible, page the right humans immediately, and then get out of the way. The worst outcome is the system thrashing 2,000 drains and node restarts on infrastructure that has a common-cause failure — that creates a remediation storm on top of the original outage."

**Q: Your Phi Accrual failure detector has been in production for 6 months and you notice the false positive rate is slowly increasing over time, now at 0.05% instead of the target 0.01%. How do you diagnose and fix this?**

A: Reason through three hypotheses.

**Hypothesis 1: The heartbeat interval distribution has changed.** Phi Accrual fits a normal distribution to recent heartbeat intervals. If the network has become jittery over 6 months (new traffic patterns, higher utilization, more infrastructure), the actual distribution may have heavier tails than a normal distribution models. Diagnosis: pull the last 30 days of heartbeat timing data per node, fit a distribution, compare to what Phi Accrual is assuming. If the tails are heavier, switch to a Pareto or log-normal distribution for better modeling. Also check: has the heartbeat frequency increased (more senders competing for bandwidth)?

**Hypothesis 2: The φ threshold is too sensitive for current network conditions.** The threshold of φ > 8 was calibrated 6 months ago. If baseline network jitter has increased, the distribution widened, and φ values are systematically higher now. Diagnosis: plot the distribution of peak φ values for nodes that are known-healthy (never actually failed). If the 99th percentile φ for healthy nodes has crept from 4 to 6, raising the threshold to 10 would restore the false positive rate. Trade-off: this increases detection latency for real failures — acceptable if the false positive rate improvement is worth it.

**Hypothesis 3: The consensus mechanism is insufficiently strong.** If 2-of-3 consensus was calibrated for 3-second heartbeats, and heartbeats are now effectively at 4-5 seconds (due to network issues), the overlap between "node A thinks it's a failure" and "node B agrees" is more frequent even for transient events. Diagnosis: look at false positive events in the audit trail — how many of the 2 detectors agreed, and what was their temporal overlap? If the two agreements are always within 1 second of each other (simultaneous network event), raising to 3-of-3 consensus (unanimous) for the initial period would reduce false positives. Trade-off: harder to achieve consensus during real failures if one detector is in a good network position.

**Fix recommendation:** Start with distribution fitting (Hypothesis 1) since it doesn't require threshold changes. If the distribution tail is heavier, switch to a more appropriate model. If that's not enough, instrument the Phi values for known-healthy nodes to set the threshold at (99.99th percentile of healthy-node-φ + 2σ). Re-evaluate after 30 days.

**Q: You're on call at 2 AM. A runbook is executing for a critical MySQL cluster failover and it's been in the PROMOTING state for 20 minutes — the expected duration is 30 seconds. The runbook has not failed (no postcondition failure), it just hasn't progressed. What do you do, and what does this tell you about your runbook system's design?**

A: This is a "hung execution" scenario and the right answer has two parts: immediate triage and architectural learning.

**Immediate triage (first 2 minutes):** Check the execution audit log for the last successful step and the current step. `GET /executions/{execution_id}` shows `current_step_index: 3, step_name: apply_relay_logs, status: running`. The relay log application step is hung. Why? Relay log application requires replaying all uncommitted transactions from the failed primary's binlog. If the primary had a large transaction or a very long-running import job in flight, the relay log could be hundreds of megabytes. Check the relay log size and replication lag at the time of failure. Also check: is the promoting replica's IO thread stalled? (`SHOW SLAVE STATUS\G` on the replica).

**Immediate action:** SSH to the replica being promoted, check `SHOW SLAVE STATUS\G` for `Seconds_Behind_Master` and `Slave_IO_Running/Slave_SQL_Running`. If the SQL thread is running but very slow, it's genuinely applying a large transaction — let it run. If the IO thread is stopped, there's a real connectivity problem. The runbook should have a step timeout; if it hasn't timed out, that's a design gap.

**What this tells you about runbook design:** Three gaps. First, **every step must have a configurable timeout** (`timeout_sec: 300` for relay log application is probably too short if the primary had large transactions; should be adaptive based on relay log size). Second, **progress monitoring within a step**: a step that's running for 20 minutes should emit progress events (bytes of relay log applied, estimated completion time) so the execution log isn't silent. The operator should see "relay log application: 7.2 GB processed, 1.1 GB remaining, ~4 minutes estimated." Third, **timeout behavior should be configurable**: the current design times out and moves to a rollback. For relay log application, the better behavior might be "page human for approval to continue" rather than silently rolling back — because a partial relay log application followed by rollback could leave the replica in an inconsistent state that's worse than waiting.

---

## STEP 7 — MNEMONICS

### Memory Trick 1: The Five Guards of Automated Recovery (BACRE)

Every automated recovery action must pass five guards before executing:

**B**last radius — is the concurrent action count below the limit?
**A**udit record — is there an audit record being written for this action?
**C**ooldown — has enough time passed since the last action on this target?
**R**olling back — do we have a defined rollback plan if this step fails?
**E**scalation ready — if this fails, does the escalation chain know what to do next?

If any of BACRE is missing, the system should not execute the action.

### Memory Trick 2: The Three Cs of Split-Brain Prevention

**Consensus** before acting (2-of-3 detectors must agree)
**Confirm** fencing worked (write a test probe, verify rejection)
**Claim** with a fencing token (monotonically increasing, stored in etcd)

### Opening One-Liner for an Automated Recovery Interview

"Automated recovery is a control loop: observe, compare to desired state, act within safety bounds, verify, and escalate when automation is exhausted. The hard part is not detecting failures — it's acting decisively without making a 10-node outage into a 100-node outage, and without failing over a healthy database that just had a network blip."

This one-liner shows you know: (a) the control loop mental model, (b) blast radius as the real challenge, (c) false positive prevention as equally important as detection speed.

---

## STEP 8 — CRITIQUE

### What is well-covered in the source material

The five source files are exceptionally thorough on the following areas: the Phi Accrual failure detector mathematics (φ formula, adaptive threshold behavior), STONITH/fencing token mechanics and why they're necessary for split-brain prevention, blast radius calculation formulas with concrete values, MySQL failover state machine (NORMAL → FENCING → PROMOTING → REROUTING → VERIFYING → COMPLETED), the composite health score formula and weight rationale, Kafka as event bus with specific topic names and partition implications, level-triggered reconciliation algorithm (the Python pseudocode is exact), chaos engineering dead-man's switch mechanics, and the four-store data architecture (MySQL/Redis/Elasticsearch/etcd) with clear justification for each.

The data models are detailed enough to implement — every table schema is production-quality with proper indexes, partition strategies, and foreign key relationships.

### What is missing, shallow, or could mislead

**Missing: Multi-region active-active patterns.** The material covers cross-region failover (DR activation) but does not address truly active-active multi-region architectures. A Staff+ interview at a global-scale company (Meta, Google, Netflix) will probe this. Active-active requires conflict resolution (what happens when two regions accept different writes for the same record?), eventual consistency acceptance, and CRDT-style data structures. The source material's tier-based RPO/RTO (RPO=0 for Tier 1) implicitly assumes active-passive for stateful services — be ready to explain why active-active for databases is hard and when you'd choose it.

**Missing: Network partition handling in the reconciliation loop.** The self-healing infrastructure covers IPMI out-of-band access for crashed nodes but doesn't address what happens to the reconciliation loop when the controller can't reach a partition of its nodes. In Kubernetes, this is handled by the node lifecycle controller (5-minute taint eviction). The source material doesn't explain this edge case. Be ready to answer: "If your self-healing controller can't reach 500 of its 10,000 nodes, what does it do?"

**Missing: Schema migration for execution state tables.** The source material describes MySQL schemas in detail but doesn't address zero-downtime migrations on high-volume tables like `health_check_results` (which gets millions of rows per day). Be ready to explain: how do you add a column to `health_check_results` while it's receiving 450,000 events/second? (Answer: Percona Online Schema Change or pt-online-schema-change, or MySQL 8.0's instant ADD COLUMN for non-reordering changes.)

**Shallow: The ML runbook recommender.** The source describes a RAG-based recommender with a confidence threshold of 0.8, but doesn't explain what the embedding model is, how the vector index is built, or how you handle cold-start (new failure type with no training data). An interviewer who asks "how exactly does your RAG recommender work?" will find the material thin. Be ready to describe: embedding model (sentence-transformers fine-tuned on failure descriptions), vector database (Faiss or Weaviate), retrieval by cosine similarity, and cold-start fallback (full-text search on symptom keywords to bootstrap before there's training data).

**Shallow: Chaos experiment idempotency after partial injection.** If the chaos engine crashes after injecting network latency into 30 of 50 targets, and then restarts, what happens? The source material mentions the dead-man's switch for rollback, but doesn't address the idempotency of resuming a partially-executed experiment. Be ready to explain: each injection is stored in MySQL with a status. On restart, the engine finds injections in `status='injecting'` or `status='active'`, treats them as already executed (idempotent), and either continues or rolls them all back depending on the experiment's resumption policy.

### Senior Probes to Expect

- "You said you use STONITH. What's your fallback when the IPMI network is down?" (Answer: abort the failover; don't promote without confirmed fencing; operator must manually verify and approve.)
- "Your Kafka consumer group falls behind by 30 minutes during a large failure event. What happens to your composite health scores?" (Answer: scores are stale; new failures during the backlog period aren't detected until the consumer catches up; mitigate with a lag monitor that alerts when consumer lag > 60 seconds and degrades to simpler, direct health checks as backup.)
- "How do you test that your automated recovery system itself is highly available?" (Answer: chaos engineering on the recovery platform — kill the health check service and verify it restarts; kill the remediation engine and verify it recovers from MySQL state; simulate etcd partial failure and verify failover orchestrator handles it gracefully.)
- "Your cooldown for node drain is 30 minutes. A flapping node oscillates between healthy and unhealthy every 20 minutes for 6 hours. What happens?" (Answer: flap detection suppresses remediation after the first oscillation — the entity is marked flapping and escalated to human. The cooldown and flap detection are complementary: cooldown prevents re-action within a window, flap detection prevents repeated actions on an entity that consistently rebounds.)

### Common Interview Traps

**Trap 1:** Saying "just use Kubernetes for self-healing" without addressing bare-metal, out-of-band recovery, or the fact that Kubernetes itself can fail. Kubernetes is one layer of self-healing; the source material covers 4 layers (hardware, OS, runtime, application).

**Trap 2:** Designing database failover without mentioning fencing/STONITH. This is the most common gap. Even if the candidate knows about replica promotion, skipping fencing shows they haven't operated databases at production scale where split-brain has actually occurred.

**Trap 3:** Using "restart the service" as the answer to every failure type. Interviewers from companies with large bare-metal fleets will probe: "What if the process restarts but comes back unhealthy 3 seconds later? What if the kernel panics? What if the disk is physically dying?" Have answers for each layer.

**Trap 4:** Treating chaos engineering as "let's randomly break things." The discipline requires: defined hypothesis, measured steady-state baseline, blast radius control, automated abort conditions, and result analysis. An interviewer from Netflix/Google/Meta knows this and will push back on unstructured chaos.

**Trap 5:** Ignoring the cost dimension of automated recovery. Synchronous replication (RPO=0) costs write latency. Pre-warmed standby capacity costs money. Tight heartbeat intervals cost monitoring traffic. A mature answer acknowledges: "Here are the trade-offs, here's how we tier them by service criticality, and here's why the cost is justified for Tier 1 but not Tier 3 services."

---
