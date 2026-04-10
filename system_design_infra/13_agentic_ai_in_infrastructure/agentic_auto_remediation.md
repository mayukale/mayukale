# System Design: Agentic Auto-Remediation System

> **Relevance to role:** This is the purest expression of Agentic AI in infrastructure. The system autonomously diagnoses infrastructure failures and executes multi-step remediation plans using the ReAct (Reason + Act) pattern. As a cloud infra platform engineer, you must demonstrate you can build an agent that safely wields powerful infrastructure tools (kubectl, SSH, cloud APIs) while maintaining strict safety guarantees. This is the highest-stakes AI system in the infrastructure stack.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Accept incidents** from the AIOps triage platform (diagnosed with probable root cause and confidence score).
2. **Plan remediation**: generate a multi-step remediation plan using the ReAct pattern — interleave reasoning with tool calls.
3. **Tool execution**: execute infrastructure actions via kubectl, SSH, cloud provider APIs, database admin APIs, DNS APIs, load balancer APIs.
4. **Precondition verification**: before each action, verify it is safe to proceed (enough healthy replicas, no ongoing deployment, etc.).
5. **Blast radius estimation**: calculate how many users/requests are affected by each proposed action.
6. **Dry-run mode**: generate a complete action plan with diffs, require human approval before execution.
7. **Rate limiting**: enforce limits on actions per hour, per service, per action type.
8. **Rollback**: every action records its inverse; support full rollback of agent session.
9. **Escalation**: if the agent cannot resolve within N steps or N minutes, escalate to human with full reasoning trace.
10. **Learning**: successful remediations are recorded and indexed for future retrieval.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Time from incident assignment to first action | < 60 seconds |
| End-to-end remediation (common cases) | < 5 minutes |
| Remediation success rate | > 90% for known failure patterns |
| False remediation rate (action makes things worse) | < 0.5% |
| Availability | 99.9% |
| Audit completeness | 100% of actions logged |
| Rollback success rate | > 99% |

### Constraints & Assumptions
- The AIOps platform has already performed initial diagnosis (root cause hypothesis + confidence).
- Infrastructure is primarily Kubernetes-based (EKS/GKE) with some bare-metal and cloud VMs.
- Agent operates through service accounts with limited, audited permissions.
- LLM reasoning via Claude Sonnet/Opus API or self-hosted vLLM.
- Organization has ~200 distinct remediation patterns (restart, scale, rollback, failover, config change, etc.).

### Out of Scope
- Initial incident detection and diagnosis (handled by AIOps platform).
- Long-running remediation (> 1 hour) — these require human-driven incident management.
- Infrastructure provisioning (new clusters, regions) — handled by capacity planner.
- Application-level fixes (code changes, hotfixes) — agent can identify the need but not write code.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Incidents requiring remediation/day | 40,000 total incidents * 30% need action | ~12,000/day |
| Auto-remediated (high confidence) | 12,000 * 60% (after full rollout) | ~7,200/day |
| Human-approved remediations | 12,000 * 25% | ~3,000/day |
| Escalated (agent gives up) | 12,000 * 15% | ~1,800/day |
| Agent tool calls/day | 7,200 auto + 3,000 approved = 10,200 * 4 steps avg | ~40,800 tool calls/day |
| LLM reasoning calls/day | 10,200 * 3 reasoning steps avg | ~30,600 LLM calls/day |
| Peak concurrent remediations | 10,200 / 86400 * 10 (burst factor) * 5 min avg duration / 60 | ~10 concurrent |

### Latency Requirements

| Component | Target | Notes |
|---|---|---|
| Incident intake → first reasoning step | < 5s | Start immediately |
| LLM reasoning step | < 8s | Generating action plan |
| Precondition check | < 3s | API calls to verify safety |
| Tool execution (kubectl) | < 10s | Most K8s operations |
| Tool execution (cloud API) | < 30s | EC2/ELB operations |
| Post-action verification | < 60s | Wait for metric stabilization |
| Full remediation (simple: pod restart) | < 2 min | 1-2 steps |
| Full remediation (complex: rollback + scale) | < 10 min | 4-8 steps |

### Storage Estimates

| Data | Calculation | Size |
|---|---|---|
| Remediation sessions (1 year) | 10,200/day * 20KB avg * 365 | ~74 GB |
| Tool call logs (1 year) | 40,800/day * 5KB * 365 | ~74 GB |
| Rollback state snapshots | 10,200/day * 10KB * 30 days retention | ~3 GB |
| Remediation playbook corpus | 200 patterns * 50KB avg | ~10 MB |
| Total (1 year) | | ~150 GB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| LLM API calls (peak) | 5 concurrent * 10KB prompt | ~50 KB/s |
| Tool execution (kubectl) | 10 concurrent * 2KB | ~20 KB/s |
| Metric queries (verification) | 10 concurrent * 5KB | ~50 KB/s |

---

## 3. High Level Architecture

```
                    ┌──────────────────────────────────┐
                    │       AIOps Triage Platform       │
                    │   (incident + diagnosis + conf.)  │
                    └──────────────┬───────────────────┘
                                   │
                                   ▼
                    ┌──────────────────────────────────┐
                    │    Remediation Queue (Kafka)      │
                    │  Priority: critical > warning     │
                    └──────────────┬───────────────────┘
                                   │
                                   ▼
        ┌─────────────────────────────────────────────────────────────┐
        │                REMEDIATION AGENT ORCHESTRATOR                │
        │                                                              │
        │  ┌─────────────────────────────────────────────────────┐    │
        │  │              ReAct Loop (per incident)               │    │
        │  │                                                      │    │
        │  │  ┌──────────┐    ┌──────────┐    ┌──────────┐      │    │
        │  │  │  REASON  │───▶│   ACT    │───▶│  VERIFY  │──┐   │    │
        │  │  │ (LLM     │    │ (Execute │    │ (Check   │  │   │    │
        │  │  │  thinks) │    │  tool)   │    │  result) │  │   │    │
        │  │  └──────────┘    └──────────┘    └──────────┘  │   │    │
        │  │       ▲                                         │   │    │
        │  │       └─────────────────────────────────────────┘   │    │
        │  │                                                      │    │
        │  │  Max iterations: 8  │  Timeout: 15 min              │    │
        │  └─────────────────────────────────────────────────────┘    │
        │                                                              │
        │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
        │  │ Safety Gate  │  │ Rate Limiter │  │ Rollback Manager │  │
        │  │ (precondition│  │ (per-service │  │ (undo stack per  │  │
        │  │  checks)     │  │  per-action) │  │  session)        │  │
        │  └──────────────┘  └──────────────┘  └──────────────────┘  │
        └──────┬──────────┬──────────┬──────────┬──────────┬──────────┘
               │          │          │          │          │
               ▼          ▼          ▼          ▼          ▼
        ┌──────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────────┐
        │ kubectl  │ │  SSH   │ │ Cloud  │ │  DNS   │ │ LLM Service│
        │ executor │ │ runner │ │  APIs  │ │  API   │ │ (Claude /  │
        │          │ │        │ │(EC2,ELB│ │        │ │  vLLM)     │
        │ - get    │ │- run   │ │ ASG,   │ │- update│ │            │
        │ - apply  │ │  cmd   │ │ RDS)   │ │  record│ │            │
        │ - delete │ │- check │ │        │ │        │ │            │
        │ - scale  │ │  status│ │        │ │        │ │            │
        └──────────┘ └────────┘ └────────┘ └────────┘ └────────────┘
               │          │          │          │
               ▼          ▼          ▼          ▼
        ┌──────────────────────────────────────────────────────────┐
        │                   Infrastructure Layer                    │
        │  Kubernetes │ EC2/GCE │ RDS │ ElastiCache │ Route53     │
        └──────────────────────────────────────────────────────────┘

        ┌──────────────────────────────────────────────────────────┐
        │                  Human Approval Gateway                   │
        │  Slack/PagerDuty ← Proposed action plan → Approve/Reject│
        └──────────────────────────────────────────────────────────┘

        ┌──────────────────────────────────────────────────────────┐
        │              Immutable Audit Log (append-only)           │
        │  Every reasoning step + tool call + result logged        │
        └──────────────────────────────────────────────────────────┘
```

### Agent Loop Data Flow: Observe → Reason → Act → Verify

```
1. OBSERVE: Receive incident context (alerts, diagnosis, confidence, affected services)
       │
       ▼
2. REASON: LLM generates remediation plan
   Input:  incident context + available tools + safety constraints + past remediation examples
   Output: "I will: (1) check current pod health, (2) restart the OOMKilled pod,
            (3) verify error rate returns to normal"
       │
       ▼
3. ACT: Execute first step via tool call
   Precondition check → execute tool → capture output
   Example: kubectl get pods -n payments → see 2/3 pods in CrashLoopBackOff
       │
       ▼
4. VERIFY: Check if action had desired effect
   Query metrics, check pod status, wait for stabilization
   If resolved → DONE. If not → loop back to REASON with new observations.
       │
       ▼
5. REASON (iteration 2): "Pods are still crashing. Checking logs for root cause..."
   ... and so on, up to max 8 iterations
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Remediation session (one per incident remediation attempt)
CREATE TABLE remediation_sessions (
    session_id          UUID PRIMARY KEY,
    incident_id         UUID NOT NULL,
    status              VARCHAR(20) NOT NULL,   -- 'planning', 'executing', 'verifying',
                                                -- 'succeeded', 'failed', 'escalated', 'rolled_back'
    diagnosis           JSONB NOT NULL,         -- from AIOps platform
    initial_confidence  FLOAT NOT NULL,
    mode                VARCHAR(10) NOT NULL,   -- 'auto', 'approved', 'dry_run'
    steps_planned       INT DEFAULT 0,
    steps_executed      INT DEFAULT 0,
    started_at          TIMESTAMPTZ DEFAULT NOW(),
    completed_at        TIMESTAMPTZ,
    duration_seconds    INT,
    outcome             JSONB,                  -- {resolved: bool, root_cause_confirmed: bool, notes}
    created_by          VARCHAR(50) DEFAULT 'agent'
);

-- Individual steps in a remediation
CREATE TABLE remediation_steps (
    step_id             UUID PRIMARY KEY,
    session_id          UUID REFERENCES remediation_sessions(session_id),
    step_number         INT NOT NULL,
    phase               VARCHAR(10) NOT NULL,   -- 'reason', 'act', 'verify'
    tool_name           VARCHAR(50),            -- null for reasoning steps
    tool_input          JSONB,
    tool_output         JSONB,
    reasoning           TEXT,                   -- LLM chain-of-thought
    precondition_result JSONB,                  -- safety check result
    blast_radius        JSONB,                  -- {users_affected, requests_affected, services_affected}
    rollback_action     JSONB,                  -- inverse operation
    status              VARCHAR(20) NOT NULL,   -- 'planned', 'approved', 'executing', 'succeeded', 'failed', 'skipped'
    started_at          TIMESTAMPTZ,
    completed_at        TIMESTAMPTZ,
    approved_by         VARCHAR(100)            -- 'auto' or human username
);

-- Rate limiting state
CREATE TABLE action_rate_limits (
    service_name        VARCHAR(255) NOT NULL,
    action_type         VARCHAR(50) NOT NULL,
    window_start        TIMESTAMPTZ NOT NULL,
    action_count        INT DEFAULT 0,
    max_allowed         INT NOT NULL,
    PRIMARY KEY (service_name, action_type, window_start)
);

-- Rollback state (for undoing agent actions)
CREATE TABLE rollback_stack (
    rollback_id         UUID PRIMARY KEY,
    session_id          UUID REFERENCES remediation_sessions(session_id),
    step_id             UUID REFERENCES remediation_steps(step_id),
    rollback_action     JSONB NOT NULL,         -- {tool, params} to undo this step
    state_snapshot      JSONB,                  -- state before the action (for verification)
    executed            BOOLEAN DEFAULT FALSE,
    executed_at         TIMESTAMPTZ
);

-- Remediation playbook corpus (learned patterns)
CREATE TABLE remediation_playbooks (
    playbook_id         UUID PRIMARY KEY,
    title               VARCHAR(500) NOT NULL,
    trigger_pattern     JSONB NOT NULL,         -- {alert_types, services, symptoms}
    steps               JSONB NOT NULL,         -- ordered list of remediation steps
    success_rate        FLOAT,                  -- historical success rate
    avg_duration_sec    INT,
    times_used          INT DEFAULT 0,
    last_used_at        TIMESTAMPTZ,
    embedding           VECTOR(1536),           -- for similarity search
    created_at          TIMESTAMPTZ DEFAULT NOW(),
    updated_at          TIMESTAMPTZ DEFAULT NOW()
);
```

### Database Selection

| Store | Technology | Justification |
|---|---|---|
| Session/step data | PostgreSQL | ACID for audit trail, complex queries for reporting |
| Rate limiting | Redis | Atomic increment with TTL, fast counter operations |
| Rollback state | PostgreSQL | Must survive crashes; transactional consistency |
| Playbook embeddings | pgvector | Similarity search for matching past remediations |
| Remediation queue | Kafka | Reliable priority queue, replay capability |

### Indexing Strategy

```sql
CREATE INDEX idx_sessions_incident ON remediation_sessions (incident_id);
CREATE INDEX idx_sessions_status ON remediation_sessions (status) WHERE status IN ('planning', 'executing', 'verifying');
CREATE INDEX idx_steps_session ON remediation_steps (session_id, step_number);
CREATE INDEX idx_rate_limits_lookup ON action_rate_limits (service_name, action_type, window_start DESC);
CREATE INDEX idx_playbooks_embedding ON remediation_playbooks USING ivfflat (embedding vector_cosine_ops) WITH (lists = 50);
```

---

## 5. API Design

### REST/gRPC Endpoints

```
# Remediation sessions
POST   /api/v1/remediations                          # Start a remediation (from AIOps platform)
GET    /api/v1/remediations/{session_id}              # Get session detail + all steps
POST   /api/v1/remediations/{session_id}/approve      # Approve pending action plan
POST   /api/v1/remediations/{session_id}/reject       # Reject and escalate to human
POST   /api/v1/remediations/{session_id}/rollback     # Rollback all actions in this session
POST   /api/v1/remediations/{session_id}/abort        # Stop agent, escalate immediately

# Steps
GET    /api/v1/remediations/{session_id}/steps        # List all steps
POST   /api/v1/remediations/{session_id}/steps/{id}/approve  # Approve individual step

# Rate limits
GET    /api/v1/rate-limits?service=X                  # Check current rate limit status
PUT    /api/v1/rate-limits                            # Update rate limit configuration

# Playbooks
GET    /api/v1/playbooks                              # List all playbooks
GET    /api/v1/playbooks/search?q=...                 # Semantic search
POST   /api/v1/playbooks                              # Create/update playbook
```

### Agent Tool Call Interface

```json
{
  "tools": [
    {
      "name": "kubectl_get",
      "description": "Get Kubernetes resources",
      "parameters": {
        "resource": "string — e.g., 'pods', 'deployments', 'nodes', 'events'",
        "namespace": "string",
        "selector": "string — label selector (optional)",
        "output": "string — 'json', 'wide', 'yaml' (default 'json')"
      },
      "risk_level": "none"
    },
    {
      "name": "kubectl_describe",
      "description": "Describe a Kubernetes resource (detailed info + events)",
      "parameters": {
        "resource": "string",
        "name": "string",
        "namespace": "string"
      },
      "risk_level": "none"
    },
    {
      "name": "kubectl_logs",
      "description": "Get pod logs",
      "parameters": {
        "pod": "string",
        "namespace": "string",
        "container": "string (optional)",
        "tail": "int — number of lines (default 100)",
        "previous": "bool — get logs from previous container (default false)"
      },
      "risk_level": "none"
    },
    {
      "name": "kubectl_delete_pod",
      "description": "Delete a pod (triggers recreation by controller)",
      "parameters": {
        "pod": "string",
        "namespace": "string"
      },
      "risk_level": "low",
      "preconditions": ["min_healthy_replicas:2", "no_ongoing_deployment"]
    },
    {
      "name": "kubectl_scale",
      "description": "Scale a deployment up or down",
      "parameters": {
        "deployment": "string",
        "namespace": "string",
        "replicas": "int"
      },
      "risk_level": "medium",
      "preconditions": ["replicas_within_bounds", "cluster_has_capacity"],
      "blast_radius_calculation": "affected_traffic = (current_replicas - new_replicas) / current_replicas * 100"
    },
    {
      "name": "kubectl_rollback",
      "description": "Rollback a deployment to previous revision",
      "parameters": {
        "deployment": "string",
        "namespace": "string",
        "revision": "int (optional — default previous)"
      },
      "risk_level": "high",
      "preconditions": ["previous_revision_exists", "previous_revision_was_healthy"]
    },
    {
      "name": "kubectl_cordon_node",
      "description": "Mark a node as unschedulable",
      "parameters": {
        "node": "string"
      },
      "risk_level": "medium",
      "preconditions": ["max_cordoned_nodes_not_exceeded"]
    },
    {
      "name": "kubectl_drain_node",
      "description": "Drain all pods from a node (evict gracefully)",
      "parameters": {
        "node": "string",
        "grace_period": "int — seconds (default 30)",
        "ignore_daemonsets": "bool (default true)"
      },
      "risk_level": "high",
      "preconditions": ["node_pods_have_pdb", "other_nodes_have_capacity"]
    },
    {
      "name": "kubectl_apply",
      "description": "Apply a Kubernetes manifest (for config changes)",
      "parameters": {
        "manifest_yaml": "string — the YAML to apply",
        "namespace": "string",
        "dry_run": "bool (default true — must be explicitly set to false)"
      },
      "risk_level": "high",
      "preconditions": ["manifest_validated", "diff_reviewed"]
    },
    {
      "name": "ssh_run_command",
      "description": "Run a command on a remote host via SSH",
      "parameters": {
        "host": "string — hostname or IP",
        "command": "string — shell command to execute",
        "timeout": "int — seconds (max 60)"
      },
      "risk_level": "high",
      "preconditions": ["host_in_inventory", "command_in_allowlist"],
      "command_allowlist": [
        "systemctl restart *",
        "journalctl -u * --since *",
        "df -h", "free -m", "top -bn1",
        "netstat -tlnp", "ss -tlnp",
        "cat /var/log/*"
      ]
    },
    {
      "name": "cloud_api_call",
      "description": "Make an AWS/GCP API call",
      "parameters": {
        "provider": "string — 'aws' or 'gcp'",
        "service": "string — e.g., 'ec2', 'autoscaling', 'rds'",
        "action": "string — e.g., 'describe_instances', 'set_desired_capacity'",
        "params": "object — action-specific parameters"
      },
      "risk_level": "varies",
      "action_risk_map": {
        "describe_*": "none",
        "get_*": "none",
        "list_*": "none",
        "set_desired_capacity": "medium",
        "reboot_instances": "high",
        "failover_db_cluster": "high"
      }
    },
    {
      "name": "query_metrics",
      "description": "Query Prometheus/CloudWatch metrics",
      "parameters": {
        "query": "string — PromQL or CloudWatch query",
        "duration": "string — lookback (e.g., '30m')",
        "step": "string — resolution (e.g., '1m')"
      },
      "risk_level": "none"
    },
    {
      "name": "query_logs",
      "description": "Search logs in Loki/CloudWatch Logs/Elasticsearch",
      "parameters": {
        "service": "string",
        "query": "string",
        "duration": "string",
        "limit": "int (default 50)"
      },
      "risk_level": "none"
    },
    {
      "name": "check_blast_radius",
      "description": "Estimate user/traffic impact of a proposed action",
      "parameters": {
        "action": "string",
        "target": "string",
        "params": "object"
      },
      "risk_level": "none"
    },
    {
      "name": "escalate",
      "description": "Escalate to human on-call",
      "parameters": {
        "reason": "string",
        "context_summary": "string",
        "urgency": "string — 'high' or 'low'"
      },
      "risk_level": "none"
    }
  ]
}
```

### Human Escalation API

```
POST /api/v1/escalations
{
  "incident_id": "uuid",
  "session_id": "uuid",
  "reason": "Agent reached max iterations (8) without resolution",
  "diagnosis": { "root_cause": "...", "confidence": 0.72 },
  "actions_taken": [
    {"step": 1, "action": "restart_pod", "result": "Pod restarted but crashed again"},
    {"step": 2, "action": "check_logs", "result": "NPE in PaymentProcessor.java:142"},
    {"step": 3, "action": "rollback_deployment", "result": "Rollback succeeded but errors persist"}
  ],
  "hypothesis": "Bug exists in both current and previous version. Likely needs code fix.",
  "recommended_human_actions": [
    "Check PaymentProcessor.java:142 for null pointer",
    "Consider emergency hotfix or feature flag toggle"
  ]
}
```

---

## 6. Core Component Deep Dives

### 6.1 ReAct Agent Engine

**Why it's hard:** The agent must interleave reasoning and action in a way that is both effective (reaches the right solution) and safe (doesn't make things worse). Each step depends on the output of the previous step, so the agent must handle unexpected results gracefully. The search space is enormous — there are hundreds of possible actions at each step.

| Approach | Pros | Cons |
|---|---|---|
| **Static decision tree** | Predictable, safe, fast | Cannot handle novel failures; maintenance nightmare |
| **Pure LLM chain-of-thought** | Flexible reasoning | No grounding in actual system state; hallucination risk |
| **ReAct (Reason + Act)** | Grounded reasoning; adapts to observations | Slower (multiple LLM calls); can get stuck in loops |
| **Plan-then-execute** | Full plan visible for approval | Plan may be wrong; no adaptation to intermediate results |
| **ReAct with plan scaffolding** | Plan first, then adapt during execution | Best of both: visibility + adaptability |

**Selected: ReAct with plan scaffolding**

**Implementation:**

```python
class RemediationAgent:
    def __init__(self, incident, tools, llm, safety_gate, rate_limiter):
        self.incident = incident
        self.tools = tools
        self.llm = llm
        self.safety = safety_gate
        self.limiter = rate_limiter
        self.history = []  # (thought, action, observation) triples
        self.rollback_stack = []
        self.max_iterations = 8
        self.timeout = timedelta(minutes=15)

    async def run(self):
        # Phase 1: Generate initial plan
        plan = await self.generate_plan()
        if self.incident.mode == 'dry_run':
            return self.format_dry_run(plan)

        # Phase 2: Execute with ReAct loop
        for i in range(self.max_iterations):
            # REASON
            thought = await self.reason(self.history, plan)

            if thought.action == 'DONE':
                return self.succeed(thought.summary)
            if thought.action == 'ESCALATE':
                return self.escalate(thought.reason)

            # SAFETY CHECK
            safety_result = await self.safety.check(thought.action, thought.params)
            if not safety_result.safe:
                self.history.append(('blocked', thought.action, safety_result.reason))
                continue  # LLM will reason about why it was blocked

            # RATE LIMIT CHECK
            if not await self.limiter.allow(self.incident.service, thought.action.type):
                return self.escalate("Rate limit exceeded")

            # BLAST RADIUS CHECK
            blast = await self.check_blast_radius(thought.action)
            if blast.users_affected_pct > 20:
                return self.escalate(f"Blast radius too large: {blast.users_affected_pct}%")

            # ACT
            result = await self.execute_tool(thought.action, thought.params)
            self.rollback_stack.append(thought.action.inverse())

            # VERIFY
            verification = await self.verify(thought.expected_outcome)
            self.history.append((thought.reasoning, thought.action, verification))

            if verification.resolved:
                return self.succeed(verification.summary)

        # Max iterations reached
        return self.escalate("Max iterations reached without resolution")
```

**Prompt structure for each REASON step:**

```
System: You are an infrastructure remediation agent. You have already diagnosed
the incident. Now you must fix it step by step.

Rules:
- Always check system state before taking destructive actions.
- If unsure, gather more information rather than guessing.
- If you've tried 3 different approaches without success, escalate.
- Never take an action that could affect more than 10% of traffic.

--- INCIDENT ---
{incident.diagnosis}

--- AVAILABLE TOOLS ---
{tool_descriptions}

--- HISTORY (previous steps) ---
Step 1: Thought: ... Action: ... Observation: ...
Step 2: Thought: ... Action: ... Observation: ...

--- CURRENT PLAN ---
{remaining_plan_steps}

What is your next step? Think step by step, then choose an action.
Respond with:
THOUGHT: <your reasoning>
ACTION: <tool_name>
PARAMS: <json params>
EXPECTED_OUTCOME: <what you expect to see>
```

**Failure Modes:**
- **Agent loops**: Repeats the same action. Mitigation: detect repeated actions in history, force escalation after 2 identical actions.
- **Agent drifts**: Starts doing unrelated actions. Mitigation: validate each action against the original diagnosis; alert if action seems unrelated.
- **Tool failure**: kubectl times out. Mitigation: retry with backoff (max 2 retries), then try alternative approach or escalate.
- **Verification false positive**: Agent thinks it fixed the issue but didn't. Mitigation: wait 5 minutes and re-verify before closing.

**Interviewer Q&As:**

**Q1: Why ReAct over a simple decision tree?**
A: Decision trees can't handle novel failure combinations. With 5,000+ services, the combinatorial space of failures is enormous. ReAct lets the LLM reason about novel situations while still grounding decisions in real observations. However, for the top 50 most common failure patterns (covering ~60% of incidents), we do have pre-built playbooks that the agent follows — it only falls back to free-form reasoning for unusual cases.

**Q2: How do you prevent the agent from executing the same failed action repeatedly?**
A: Three mechanisms. (1) The full history (including failed actions) is in the LLM context, and the prompt explicitly says "do not repeat failed actions." (2) We programmatically check: if the agent proposes an action identical to one that failed in the last 3 steps, we block it and append a note to the history. (3) After 2 blocked repeated actions, we force escalation.

**Q3: How do you handle partial failures during multi-step remediation?**
A: Each step records its rollback action before execution. If step 3 fails and we need to undo steps 1-2, we pop the rollback stack in reverse order. The agent can also reason about whether to rollback or continue with an alternative approach. For example, if scaling up succeeded but the new pods are also crashing, the agent might keep the scale-up (more capacity) while investigating the crash — no rollback needed for that step.

**Q4: What if the rollback itself fails?**
A: This is an immediate human escalation with full context. The escalation message includes: what was done, what rollback was attempted, why it failed, and the current system state. This is a P0-level page. We also maintain state snapshots before each action so a human can manually restore state.

**Q5: How do you size the LLM model for this use case?**
A: We need strong reasoning capability because remediation involves multi-step planning and adaptation. A smaller model (8B params) struggles with complex reasoning chains. We use Claude Sonnet (or equivalent) as the default, and escalate to Opus for incidents involving > 5 services or after 5 iterations without resolution. Average cost: ~$0.40 per remediation session.

**Q6: Can the agent learn from its mistakes?**
A: Yes, via two mechanisms. (1) Successful remediations are stored as playbooks with their trigger patterns. Next time a similar incident occurs, the playbook is retrieved via embedding similarity and used as the initial plan. (2) Failed remediations are also stored — the LLM sees "last time we tried X and it didn't work" as part of the RAG context. Over time, the playbook corpus becomes the organization's remediation knowledge base.

---

### 6.2 Safety Gate (Precondition Verification)

**Why it's hard:** The agent proposes actions that could cause outages if executed at the wrong time or on the wrong target. The safety gate must verify conditions in real-time, be fast enough to not bottleneck the agent, and be comprehensive enough to catch dangerous actions. Missing a precondition is an outage. Being too conservative makes the agent useless.

| Approach | Pros | Cons |
|---|---|---|
| **Static rules per action type** | Simple, predictable | Can't account for dynamic state |
| **Real-time state checks (API calls)** | Accounts for current state | Slower; API failures block agent |
| **Composite: static rules + real-time checks** | Comprehensive | More complex; must handle check failures |
| **LLM-based safety reasoning** | Can catch nuanced risks | Slow; may hallucinate safety |

**Selected: Composite (static rules + real-time API checks)**

**Implementation:**

```python
class SafetyGate:
    # Static rules: fast, always checked
    STATIC_RULES = {
        'kubectl_delete_pod': [
            StaticRule('namespace_not_kube_system', lambda p: p['namespace'] != 'kube-system'),
            StaticRule('namespace_not_monitoring', lambda p: p['namespace'] != 'monitoring'),
        ],
        'kubectl_scale': [
            StaticRule('replicas_positive', lambda p: p['replicas'] > 0),
            StaticRule('replicas_max_100', lambda p: p['replicas'] <= 100),
            StaticRule('scale_change_max_3x', lambda p: p['replicas'] <= p.get('current', 1) * 3),
        ],
        'kubectl_drain_node': [
            StaticRule('not_master_node', lambda p: 'master' not in p.get('node_labels', {})),
        ],
        'ssh_run_command': [
            StaticRule('command_in_allowlist', lambda p: is_allowed_command(p['command'])),
            StaticRule('host_in_inventory', lambda p: p['host'] in KNOWN_HOSTS),
        ],
    }

    # Real-time checks: slower, verify current state
    REALTIME_CHECKS = {
        'kubectl_delete_pod': [
            RealtimeCheck('min_healthy_replicas',
                check=lambda p: get_healthy_replicas(p['namespace'], p['deployment']) >= 2,
                error="Cannot delete pod: fewer than 2 healthy replicas"),
            RealtimeCheck('no_ongoing_deployment',
                check=lambda p: not is_deploying(p['namespace'], p['deployment']),
                error="Cannot delete pod: deployment in progress"),
        ],
        'kubectl_drain_node': [
            RealtimeCheck('max_cordoned_not_exceeded',
                check=lambda _: count_cordoned_nodes() < MAX_CORDONED,
                error=f"Already {MAX_CORDONED} nodes cordoned"),
            RealtimeCheck('other_nodes_have_capacity',
                check=lambda p: remaining_capacity_without_node(p['node']) > 0.2,
                error="Draining this node would leave < 20% cluster capacity"),
        ],
    }

    async def check(self, action, params):
        # Run static checks first (fast)
        for rule in self.STATIC_RULES.get(action, []):
            if not rule.check(params):
                return SafetyResult(safe=False, reason=f"Static rule failed: {rule.name}")

        # Run real-time checks (parallel)
        checks = self.REALTIME_CHECKS.get(action, [])
        results = await asyncio.gather(*[c.check(params) for c in checks])
        for check, result in zip(checks, results):
            if not result:
                return SafetyResult(safe=False, reason=check.error)

        return SafetyResult(safe=True)
```

**Failure Modes:**
- **Check API timeout**: If a real-time check times out (e.g., can't reach K8s API), we fail closed (deny the action). The agent can retry after a delay.
- **Stale data**: Checking "3 healthy replicas" but one crashes between check and action. Mitigated by post-action verification.
- **Missing rule**: A new action type without safety rules. Mitigated by denying any action without defined rules (allowlist approach).

**Interviewer Q&As:**

**Q1: Why fail closed instead of fail open when a safety check can't be performed?**
A: In infrastructure, the cost of a wrong action vastly exceeds the cost of a delayed action. If we can't verify it's safe, we don't do it. The agent can retry, try an alternative action, or escalate. This is the same principle as a circuit breaker — when in doubt, don't act.

**Q2: How do you handle the window between the safety check and the action execution?**
A: This is the TOCTOU (Time-Of-Check-Time-Of-Use) problem. Two mitigations: (1) Keep the window small — check and execute within the same transaction where possible (e.g., K8s optimistic concurrency with resourceVersion). (2) Post-action verification catches cases where state changed between check and action. (3) For critical actions, we use K8s admission webhooks that re-validate at execution time.

**Q3: How do you maintain safety rules as infrastructure changes?**
A: Safety rules are code, stored in version control, reviewed like any other infrastructure change. We have a CI pipeline that tests safety rules against a matrix of scenarios. New action types cannot be added without corresponding safety rules — the tool registration process enforces this.

**Q4: Can the agent override safety checks?**
A: Never. Safety checks are enforced by the orchestrator, not the LLM. The LLM cannot bypass them. If the LLM disagrees with a safety check, it can reason about it and try an alternative approach, but it cannot disable the check. Only a human can override by approving a blocked action through the approval gateway.

**Q5: How do you test safety rules without causing real incidents?**
A: We maintain a staging cluster that mirrors production topology. Safety gate tests run against this cluster with injected failure scenarios. We also run chaos engineering exercises where the agent remediates injected failures in staging before promoting to production.

**Q6: What's the blast radius calculation formula?**
A: It varies by action type. For pod deletion: `blast_pct = 1 / healthy_replicas * 100`. For node drain: `blast_pct = pods_on_node / total_pods_in_service * 100` for each affected service, take the max. For deployment rollback: we estimate from canary metrics — error rate delta between current and previous version * traffic share. The blast radius is shown to the human in the approval request.

---

### 6.3 Rollback Manager

**Why it's hard:** Every agent action must be reversible. But some actions are inherently difficult to reverse (data deletion, config propagation across clusters). The rollback manager must maintain a consistent undo stack, handle partial failures, and execute rollbacks quickly during incidents when the system is already under stress.

| Approach | Pros | Cons |
|---|---|---|
| **Command pattern (store inverse)** | Simple, explicit | Not all actions have clean inverses |
| **State snapshot + restore** | Always reversible | Expensive to snapshot; restore may have side effects |
| **Git-style: checkpoint + revert** | Full state history | Complex; K8s state is too large to snapshot fully |
| **Hybrid: command pattern + selective snapshots** | Practical balance | More implementation effort |

**Selected: Hybrid (command pattern + selective state snapshots)**

**Implementation:**

| Action | Rollback Strategy | State Captured |
|---|---|---|
| `delete_pod` | No rollback needed (K8s recreates) | Pod spec (for verification) |
| `scale_deployment` | Scale back to original replica count | Original replica count |
| `rollback_deployment` | Roll forward to the version we rolled back from | Revision number |
| `cordon_node` | Uncordon | Node name |
| `drain_node` | Uncordon + re-schedule pods | Pod list before drain |
| `apply_manifest` | Apply previous manifest version | Previous YAML |
| `ssh_run_command` | Execute paired undo command | Varies (e.g., `systemctl stop` for `start`) |

```python
class RollbackManager:
    def __init__(self, session_id, db):
        self.session_id = session_id
        self.db = db
        self.stack = []  # ordered list of rollback entries

    async def record(self, step_id, action, params, rollback_action, state_snapshot=None):
        entry = RollbackEntry(
            session_id=self.session_id,
            step_id=step_id,
            rollback_action=rollback_action,
            state_snapshot=state_snapshot,
        )
        await self.db.insert(entry)
        self.stack.append(entry)

    async def rollback_all(self):
        """Roll back all actions in reverse order."""
        results = []
        for entry in reversed(self.stack):
            if entry.executed:
                continue
            try:
                result = await execute_tool(entry.rollback_action)
                entry.executed = True
                entry.executed_at = datetime.utcnow()
                results.append(('success', entry))
            except Exception as e:
                results.append(('failed', entry, str(e)))
                # Don't stop — try to rollback remaining actions
                # But alert human about partial rollback failure
        return results
```

**Failure Modes:**
- **Rollback action fails**: Log the failure, continue rolling back remaining actions, escalate to human with partial rollback state.
- **State changed between action and rollback**: E.g., we scaled to 10 replicas, someone else scaled to 15, we rollback to 5. Mitigation: rollback checks current state and only reverts to pre-agent state if current state matches post-agent state. If someone else changed it, alert and skip.
- **Rollback causes its own incident**: E.g., rolling back a deployment re-introduces a bug. Mitigation: rollback is itself subject to safety checks and verification.

**Interviewer Q&As:**

**Q1: How do you handle rollback for actions that can't be reversed?**
A: Some actions are truly irreversible (e.g., a pod was deleted and logs were lost). For these, we don't allow auto-execution — they require human approval. The tool definition marks them as `irreversible: true`, which forces manual approval regardless of confidence. We keep the set of irreversible actions as small as possible.

**Q2: What if the user requests a rollback 2 hours after the agent acted?**
A: We support rollback within a configurable window (default: 1 hour). After that, the system state may have diverged too much for automatic rollback. We show the user the current state vs. the captured state and let them decide. The rollback button remains available but with a warning about staleness.

**Q3: How do you handle concurrent rollbacks?**
A: Rollbacks acquire a per-service distributed lock (Redis). Only one rollback can execute per service at a time. If a second rollback is requested while one is in progress, it queues behind it. This prevents conflicting state changes.

**Q4: Do you test rollbacks?**
A: Yes. In staging, every new remediation playbook must demonstrate successful rollback as part of its validation. We also periodically run "rollback drills" in production — execute a benign action and immediately rollback to verify the mechanism works.

**Q5: How much storage does the rollback state require?**
A: Minimal. We only store the rollback action (a few hundred bytes) and a selective state snapshot (the original manifest or config, typically 1-5 KB). With 10,200 remediations/day and 30-day retention, that's about 3 GB — trivial.

**Q6: Can an agent rollback another agent's actions?**
A: Yes, but only through explicit request (human or automated). An agent working on incident A can see that agent B's recent action on a shared service may be relevant. It can recommend rollback of agent B's action, but this requires human approval since it crosses incident boundaries.

---

## 7. AI Agent Architecture

### Agent Loop Design

The remediation agent follows the ReAct (Reason + Act) pattern with extensions for safety and rollback:

```
┌────────────────────────────────────────────────────────────────┐
│                     AGENT LIFECYCLE                             │
│                                                                 │
│  INIT: Receive incident + diagnosis + confidence                │
│    │                                                            │
│    ▼                                                            │
│  PLAN: Generate initial remediation plan                        │
│    │   (LLM + similar past remediations from playbook corpus)   │
│    │                                                            │
│    ├── If dry_run mode → return plan, exit                      │
│    │                                                            │
│    ├── If needs_approval → send plan to human, wait             │
│    │                                                            │
│    ▼                                                            │
│  EXECUTE (ReAct loop, max 8 iterations):                        │
│    │                                                            │
│    ├─ REASON: LLM analyzes history + current state              │
│    │    Output: thought + next_action + expected_outcome        │
│    │                                                            │
│    ├─ SAFETY CHECK: preconditions + blast radius                │
│    │    If blocked → append to history, continue loop           │
│    │                                                            │
│    ├─ RATE LIMIT CHECK: per-service, per-action                 │
│    │    If exceeded → escalate                                  │
│    │                                                            │
│    ├─ ACT: Execute tool, record rollback action                 │
│    │    Capture output                                          │
│    │                                                            │
│    ├─ VERIFY: Check if action achieved expected outcome         │
│    │    If resolved → SUCCESS                                   │
│    │    If not → continue loop with new observations            │
│    │                                                            │
│    └─ If max iterations → ESCALATE                              │
│                                                                 │
│  CLEANUP: Close session, update playbook corpus, log metrics    │
└────────────────────────────────────────────────────────────────┘
```

### Tool Definitions

See Section 5 for the complete tool interface. Summary:

| Category | Tools | Count |
|---|---|---|
| Read-only (observe) | kubectl_get, describe, logs, query_metrics, query_logs | 5 |
| Low-risk write | kubectl_delete_pod | 1 |
| Medium-risk write | kubectl_scale, kubectl_cordon_node | 2 |
| High-risk write | kubectl_rollback, kubectl_drain_node, kubectl_apply, ssh_run_command | 4 |
| Meta | check_blast_radius, escalate | 2 |
| **Total** | | **14** |

### Context Window Management

```
┌─────────────────────────────────────────────────┐
│              CONTEXT BUDGET (32K tokens)          │
├─────────────────────────────────────────────────┤
│ System prompt + safety rules         │  800 tokens│
│ Incident context (diagnosis, alerts) │ 2000 tokens│
│ Available tool definitions           │ 2000 tokens│
│ Retrieved playbook (if matched)      │ 1500 tokens│
│ Conversation history (rolling)       │ 8000 tokens│
│   - Each step: ~1000 tokens          │            │
│   - Keep last 8 steps                │            │
│ Current observation (metrics/logs)   │ 2000 tokens│
│ Buffer for response                  │ 2000 tokens│
├─────────────────────────────────────────────────┤
│ Total allocated                      │18300 tokens│
│ Headroom                             │13700 tokens│
└─────────────────────────────────────────────────┘
```

**Overflow handling:** If the conversation exceeds 8 steps, summarize steps 1-4 into a compressed "story so far" block (~500 tokens), freeing space for new observations.

### Memory: Episodic, Semantic, Procedural

| Memory Type | Storage | Content | Access Pattern |
|---|---|---|---|
| **Episodic** | PostgreSQL + pgvector | "Last week, restarting payment-service pods fixed the connection pool exhaustion issue" | RAG retrieval: embed current incident, find similar past remediations |
| **Semantic** | Playbook corpus + runbook embeddings | "Payment service connection pool is configured in /etc/service/pool.yaml with max_connections=100" | RAG retrieval: embed symptoms, find relevant config/runbook |
| **Procedural** | Tool definitions + safety rules | "To restart a pod: call kubectl_delete_pod. Precondition: >= 2 healthy replicas." | Loaded into every prompt as tool descriptions |
| **Working** | LLM context window | Current incident, history of actions taken, latest observations | Maintained per-session in the agent loop |

### Guardrails and Safety

1. **Tool allowlist**: Agent can only call registered tools. No shell injection, no arbitrary API calls.
2. **Parameter validation**: Every tool input is validated against a JSON schema before execution.
3. **SSH command allowlist**: Only pre-approved commands can run via SSH. No `rm`, no `dd`, no `mkfs`.
4. **Namespace restrictions**: Agent cannot touch `kube-system`, `monitoring`, or other protected namespaces.
5. **Blast radius cap**: Actions affecting > 20% of a service's traffic require human approval.
6. **Rate limits per service**: Max 3 restarts per service per hour, max 1 rollback per service per hour, max 2 node drains per hour globally.
7. **Timeout**: 15 minutes per remediation session. After that, force-escalate.
8. **Max iterations**: 8 reasoning/action steps. Then force-escalate.
9. **Duplicate action prevention**: Cannot execute the same action on the same target twice in one session without new evidence.
10. **Kill switch**: Global and per-service disable of auto-remediation.

### Confidence Thresholds

| Scenario | Threshold | Mode |
|---|---|---|
| Known pattern, low-risk action (pod restart) | > 0.85 | Auto-execute |
| Known pattern, medium-risk action (scale, cordon) | > 0.90 | Auto-execute after 2-min wait |
| Known pattern, high-risk action (rollback, drain) | Any | Human approval required |
| Unknown pattern, low-risk action | > 0.80 | Auto-execute |
| Unknown pattern, medium/high-risk action | Any | Human approval required |
| Confidence < 0.50 for any action | N/A | Escalate immediately |

### Dry-Run Mode

Dry-run mode is the default for first-time remediation patterns and all high-risk actions.

```yaml
dry_run_output:
  session_id: "sess-abc-123"
  incident_id: "inc-xyz-789"
  diagnosis: "Kafka consumer lag on order-processor due to slow downstream dependency (inventory-service p99 latency 12s, normally 200ms)"
  planned_steps:
    - step: 1
      action: query_metrics
      target: inventory-service
      purpose: "Confirm elevated latency"
      risk: none
    - step: 2
      action: kubectl_get
      target: "pods -n inventory -l app=inventory-service"
      purpose: "Check pod health and resource usage"
      risk: none
    - step: 3
      action: kubectl_scale
      target: "deployment/inventory-service -n inventory"
      params: { replicas: 8 }  # currently 4
      purpose: "Scale up to handle backlog"
      risk: medium
      blast_radius: "None (adding capacity, not removing)"
      rollback: "kubectl_scale deployment/inventory-service --replicas=4"
    - step: 4
      action: verify
      check: "Consumer lag decreasing within 5 minutes"
      fallback: "If lag not decreasing, investigate inventory-service logs"
  estimated_duration: "3-5 minutes"
  requires_approval: true
  approval_options:
    - "Approve all steps"
    - "Approve steps 1-2 only (observe, then decide)"
    - "Reject and escalate to human"
```

---

## 8. Scaling Strategy

| Component | Scaling Strategy | Trigger |
|---|---|---|
| Agent workers | Horizontal (K8s HPA) | Queue depth > 10 pending remediations |
| LLM inference | Rate limiting + model tiering | Inference latency > 5s → switch to smaller model for simple cases |
| Safety gate API checks | Connection pooling + caching (30s TTL) | Check latency > 3s |
| PostgreSQL | Read replicas for reporting; primary for writes | Write IOPS > 70% |
| Kafka remediation queue | Partition by service hash | Consumer lag > 30s |

### Interviewer Q&As

**Q1: What happens when 50 incidents need remediation simultaneously during a major outage?**
A: Priority queuing. Critical-severity incidents go first. The agent workers scale horizontally — we can run 20 concurrent remediation sessions. But more importantly, during a major outage, many incidents are correlated. The correlation engine (upstream) should merge them into a few root incidents. If we still have 50 independent incidents, the rate limiter prevents the agents from overwhelming the infrastructure with simultaneous changes. Lower-priority remediations wait.

**Q2: How do you prevent agents from conflicting with each other?**
A: Two mechanisms. (1) Distributed locking: before executing a write action on a service, the agent acquires a per-service lock (Redis, 5-minute TTL). Only one agent can modify a service at a time. (2) Conflict detection: before executing, check if the target resource has been modified since the agent last observed it (K8s resourceVersion / etag). If it has, re-observe and re-plan.

**Q3: Can you run this across multiple regions?**
A: The remediation queue and agent orchestrator run in a primary region. Tool execution is cross-region (kubectl can target any cluster, cloud APIs can target any region). We don't run multiple orchestrators to avoid split-brain. If the primary region fails, we fail over to secondary region — remediation pauses for ~2 minutes during failover.

**Q4: How do you handle long-running remediations (e.g., draining a node takes 10 minutes)?**
A: The agent treats long-running actions as async operations. It initiates the drain, then polls for completion with a backoff. During the wait, the agent session is paused (not consuming LLM resources). A separate monitoring thread checks for completion and resumes the agent. The 15-minute timeout is for the entire session, not individual actions.

**Q5: What's the cost per remediation?**
A: LLM costs dominate. Average session: 3 reasoning calls * ~6K input tokens + ~1K output tokens = ~21K input + 3K output tokens. At Claude Sonnet pricing (~$3/M input, $15/M output): $0.063 input + $0.045 output = ~$0.11 per session. With 10,200 sessions/day: ~$1,100/day or ~$33K/month. If we add tool call overhead and verification: ~$0.20 per session, ~$60K/month. To reduce: cache common patterns, use smaller models for simple cases.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| **LLM API down** | Cannot reason about remediation | Health check; timeout | Fall back to static playbook execution (no reasoning, just follow matched playbook steps). Escalate if no playbook matches. |
| **Agent crashes mid-remediation** | Incomplete remediation, unknown state | Heartbeat timeout | Session state is persisted in PostgreSQL. On restart, check last known state, verify system state, resume or rollback. |
| **Tool execution fails** | Action didn't execute | Tool returns error | Retry once. If still fails, log and try alternative approach. After 2 tool failures, escalate. |
| **Agent takes wrong action** | Makes incident worse | Post-action metric verification | Auto-rollback if metrics worsen within 5 min. Kill switch for global disable. |
| **Agent hallucination** | Tries to use a tool that doesn't exist, or passes invalid params | Tool validation (JSON schema) | Invalid tool calls are caught by the orchestrator and appended to history as errors. The LLM then reasons about the error. |
| **Rate limiter fails (Redis down)** | Actions may exceed safe limits | Redis health check | Fail closed: if rate limiter unavailable, deny all write actions. Read-only actions still work. |
| **Safety gate API timeout** | Cannot verify preconditions | Timeout > 3s | Fail closed: deny action if safety can't be verified. |
| **Prompt injection via logs/metrics** | Agent manipulated by malicious content | Content filtering | Logs/metrics are escaped and placed in data blocks. Tool calls validated against schema. |
| **Kafka down** | No new remediations start | Broker health check | In-flight remediations continue (they don't depend on Kafka). New incidents escalate directly to humans. |
| **Database down** | No audit trail, no state persistence | PG health check | Halt all new remediations. In-flight sessions attempt graceful completion and write audit logs to Kafka as backup. |

### AI Safety Controls

1. **Three-strikes rule**: If the agent causes metric degradation 3 times in 24 hours, auto-disable for that service. Require manual re-enable after review.
2. **Canary actions**: For first-time remediation of a new service, first action is always read-only. Build confidence before writing.
3. **Peer review**: For high-risk actions, a second LLM instance reviews the plan independently. Both must agree.
4. **Time-of-day restrictions**: Certain high-risk actions (node drain, deployment rollback) are only auto-approved during business hours. Outside hours, always require human approval.
5. **Weekly safety review**: Review all auto-remediated incidents from the past week. Check for near-misses, incorrect actions that happened to work, or actions that masked the real problem.

---

## 10. Observability

### Key Metrics

| Metric | Type | Target | Alert Threshold |
|---|---|---|---|
| `remediation.success_rate` | Gauge (7d rolling) | > 90% | < 80% |
| `remediation.false_positive_rate` | Gauge (7d rolling) | < 0.5% | > 1% |
| `remediation.mttr_seconds_p50` | Histogram | < 180s | > 300s |
| `remediation.mttr_seconds_p95` | Histogram | < 600s | > 900s |
| `remediation.steps_per_session_avg` | Gauge | 3-4 | > 7 (agent struggling) |
| `remediation.escalation_rate` | Gauge | 15-20% | > 40% (agent too cautious) or < 5% (too aggressive) |
| `remediation.rollback_rate` | Gauge | < 5% | > 10% |
| `remediation.safety_block_rate` | Gauge | < 10% | > 25% (agent proposes too many unsafe actions) |
| `remediation.tool_error_rate` | Gauge | < 2% | > 5% |
| `remediation.llm_latency_p95` | Histogram | < 8s | > 15s |
| `remediation.cost_per_session` | Gauge | < $0.50 | > $2.00 |
| `remediation.concurrent_sessions` | Gauge | — | > 20 (capacity concern) |
| `remediation.human_override_rate` | Gauge | < 10% | > 25% |

### Agent Action Audit Trail

Every tool call produces an immutable audit record:

```json
{
  "audit_id": "uuid",
  "session_id": "uuid",
  "incident_id": "uuid",
  "timestamp": "2026-04-09T14:32:10Z",
  "iteration": 3,
  "phase": "act",
  "reasoning": "Inventory-service pods show high memory usage (92%). Scaling from 4 to 8 replicas to distribute load while investigating the memory issue.",
  "tool": "kubectl_scale",
  "input": {"deployment": "inventory-service", "namespace": "inventory", "replicas": 8},
  "safety_check": {"passed": true, "checks": ["replicas_within_bounds", "cluster_has_capacity"]},
  "blast_radius": {"users_affected_pct": 0, "reason": "Adding capacity only"},
  "output": {"status": "success", "previous_replicas": 4, "new_replicas": 8},
  "rollback_recorded": {"tool": "kubectl_scale", "params": {"replicas": 4}},
  "model": "claude-sonnet-4-20250514",
  "tokens": {"input": 6200, "output": 450},
  "latency_ms": 3200
}
```

---

## 11. Security

### Principle of Least Privilege

| Layer | Implementation |
|---|---|
| **K8s RBAC** | ServiceAccount `remediation-agent` with per-namespace role bindings. Can get/list/delete pods, scale deployments, cordon/drain nodes. Cannot create namespaces, modify RBAC, access secrets. |
| **AWS IAM** | Role `remediation-agent-role` with inline policy: read-only on EC2/ELB/CloudWatch, limited write on AutoScaling (SetDesiredCapacity only). No IAM, S3, or Lambda access. |
| **SSH** | Certificate-based auth (short-lived certs, 1-hour expiry). Command filtering via ForceCommand + allowlist in sshd_config. |
| **Network** | Agent runs in a dedicated network segment. Can reach K8s API, cloud APIs, monitoring APIs. Cannot reach application databases or user-facing services directly. |
| **Secrets** | Agent has no access to application secrets. Uses its own credentials rotated every 24 hours via Vault. |

### Audit Logging

- **Storage**: PostgreSQL (primary) + S3 (archive, immutable bucket policy).
- **Integrity**: Each audit record includes SHA-256 hash of previous record (hash chain). Daily integrity verification job.
- **Retention**: 2 years in S3. 90 days in PostgreSQL for fast queries.
- **Access**: Security team has read-only access. No one can delete or modify audit records.
- **Alerting**: Alert if audit log write fails (indicates potential system compromise or failure).

### Human Approval Gates

```
┌─────────────────────────────────────────────────────┐
│              APPROVAL DECISION MATRIX                │
├──────────────────┬─────────────┬────────────────────┤
│ Action Risk      │ Confidence  │ Approval Required  │
├──────────────────┼─────────────┼────────────────────┤
│ None (read-only) │ Any         │ None               │
│ Low              │ > 0.85      │ None (auto)        │
│ Low              │ 0.50-0.85   │ 1 human            │
│ Medium           │ > 0.90      │ None (2-min delay) │
│ Medium           │ 0.50-0.90   │ 1 human            │
│ High             │ Any         │ 1 human            │
│ Critical*        │ Any         │ 2 humans           │
│ Any              │ < 0.50      │ Always escalate    │
└──────────────────┴─────────────┴────────────────────┘
* Critical: affects > 50% of a service's capacity
```

---

## 12. Incremental Rollout

### Rollout Phases

| Phase | Duration | What Changes |
|---|---|---|
| **Phase 0: Observe Only** | 4 weeks | Agent receives incidents, generates plans, does NOT execute. Compare plans to human actions. |
| **Phase 1: Read-Only Tools** | 4 weeks | Agent can use read-only tools (kubectl get, query metrics/logs). Still generates plans without executing. Validates that observations are correct. |
| **Phase 2: Auto-remediate Non-Critical + Low-Risk** | 8 weeks | Agent can restart pods on non-critical services. All other actions still dry-run only. |
| **Phase 3: Expand Action Set** | 8 weeks | Add scaling and cordon to auto-approved actions for non-critical services. High-risk actions still require approval. |
| **Phase 4: Critical Services** | Ongoing | Extend to critical services with higher confidence thresholds. High-risk actions always require approval. |

### Rollout Interviewer Q&As

**Q1: What's the biggest risk during rollout and how do you mitigate it?**
A: The biggest risk is a false positive auto-remediation on a critical service. Mitigation: (1) Critical services are the last to be onboarded (Phase 4). (2) Even in Phase 4, high-risk actions on critical services require human approval. (3) The three-strikes rule auto-disables the agent for any service where it causes problems. (4) We maintain a kill switch.

**Q2: How do you measure success at each phase?**
A: Phase 0: "Plan accuracy" — how often does the agent's plan match what the human did? Target > 70%. Phase 1: "Observation accuracy" — are the agent's read-only observations correct? Target > 95%. Phase 2: "Remediation success rate" — of auto-remediated incidents, how many were actually fixed? Target > 90%. Phase 3-4: Same metrics plus "time saved" — MTTR reduction vs. manual remediation.

**Q3: How do you handle the organizational change? Engineers may resist AI remediation.**
A: We frame it as "AI assistant, not AI replacement." Phase 1-2 demonstrate value by providing instant diagnosis and suggested actions, which reduces cognitive load for on-call. We track and publish MTTR improvements. We also ensure engineers can override any AI action — the AI is a tool, not an authority. Early adopter teams champion the system.

**Q4: What if the agent performs well in staging but poorly in production?**
A: This is why Phase 0 (observe-only in production) exists. Staging can never fully replicate production complexity. By observing real production incidents for 4 weeks without acting, we validate the agent's reasoning against real scenarios. If Phase 0 accuracy is below 60%, we don't proceed — we improve the model/retrieval first.

**Q5: How do you handle rollback of the rollout itself?**
A: Each phase has a "revert to previous phase" procedure. Since each phase is additive (new capabilities on top of previous), reverting means disabling the new capabilities. The kill switch is instant — it reverts to Phase 0 (observe-only) within seconds. Phase transitions are controlled by feature flags, not code deploys.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Chosen | Rationale |
|---|---|---|---|
| Agent pattern | Decision tree, pure LLM, ReAct, plan-then-execute | ReAct with plan scaffolding | Balances flexibility (LLM reasoning) with structure (initial plan) and safety (step-by-step verification) |
| LLM model size | Small (8B), Medium (Haiku), Large (Sonnet), XL (Opus) | Sonnet default, Opus for complex | Sonnet has sufficient reasoning for most cases; Opus for multi-service incidents |
| Safety approach | LLM self-policing, static rules, real-time checks | Static + real-time checks (NOT LLM) | Safety must be deterministic, not probabilistic. Never trust the LLM to self-police. |
| Rollback strategy | Command pattern, full snapshots, hybrid | Hybrid | Full snapshots too expensive for K8s state; command pattern alone misses complex state |
| Execution model | Synchronous, async with polling | Async with polling for long operations | Avoids tying up LLM context while waiting for node drains or rollouts |
| Rate limiting | Per-action global, per-service, per-action-per-service | Per-action-per-service | Most granular; prevents one service from exhausting action quota |
| Approval medium | Email, Slack, PagerDuty, dedicated UI | Slack + PagerDuty + UI | Meet engineers where they are; Slack for fast approval, PagerDuty for escalation, UI for complex plans |

---

## 14. Complete Interviewer Q&A Bank

**Q1: How does this differ from Ansible/Terraform automation?**
A: Ansible/Terraform execute pre-defined playbooks — they can't reason about novel situations. The remediation agent can diagnose unknown failures, generate novel remediation plans, and adapt when actions don't produce expected results. However, for well-known patterns, we DO use pre-built playbooks (similar to Ansible) that the agent follows. The LLM adds value for the ~40% of incidents that don't match existing playbooks.

**Q2: Why not just use PagerDuty's built-in automation (Event Intelligence, Rundeck)?**
A: PagerDuty's automation is rule-based — you define "if alert X then run script Y." It can't handle complex multi-step diagnosis or adapt to unexpected observations. Our system complements PagerDuty: PagerDuty handles alerting and escalation, our agent handles intelligent diagnosis and remediation.

**Q3: How do you handle the cold start problem when first deploying to a new organization?**
A: Start with Phase 0 (observe-only) for 4-8 weeks. During this time: (1) Ingest existing runbooks and past incident reports into the RAG corpus. (2) The agent generates shadow diagnoses — compare to human outcomes. (3) Build the service dependency graph from service mesh data. (4) Tune confidence thresholds based on observed accuracy. An organization with good runbook documentation gets to Phase 2 faster.

**Q4: What's the boundary between what the agent can fix and what needs a human?**
A: The agent handles infrastructure-level remediation: restart, scale, rollback, failover, config changes. It does NOT handle application-level fixes (code bugs, schema migrations, data corruption). If the agent determines the root cause is a code bug, it escalates with full context, including the specific error, relevant logs, and which deployment introduced the bug.

**Q5: How do you handle secrets that might appear in logs the agent reads?**
A: The log query tool runs through a secrets-redaction layer. Common patterns (API keys, passwords, tokens, SSNs) are regex-matched and replaced with `[REDACTED]`. We also use a custom NER model trained on our log formats to catch domain-specific secrets. The agent never sees raw secrets.

**Q6: Can an attacker trigger the agent to take harmful actions by crafting specific alerts?**
A: This is a real threat vector. Mitigations: (1) Alert sources are authenticated — we only accept webhooks from verified monitoring systems. (2) Alert content is treated as untrusted data in the prompt. (3) The safety gate validates all actions against real system state — a fake alert about a non-existent service won't pass precondition checks. (4) Rate limits prevent mass actions even if tricked. (5) We log alert source and investigate anomalous alert patterns.

**Q7: How do you handle testing changes to the agent itself?**
A: Three environments. (1) Unit tests: mock tool responses, verify agent reasoning leads to correct actions. (2) Integration tests in staging: inject real failures (chaos engineering), verify end-to-end remediation. (3) Shadow mode in production: new agent version runs in parallel, generates plans but doesn't execute. Compare accuracy to production version. Only promote after 1 week of shadow testing with > 85% accuracy.

**Q8: What's the operational burden of running this system?**
A: The system requires a dedicated platform team (2-3 engineers) for: LLM prompt tuning, safety rule maintenance, playbook corpus curation, accuracy monitoring, and model upgrades. This is offset by the reduction in on-call burden: with 60% auto-remediation, the number of human-handled incidents drops significantly, improving engineer quality of life.

**Q9: How do you handle multi-cloud remediation?**
A: The tool executor has providers for each cloud (AWS, GCP, Azure). The agent doesn't need to know which cloud a service runs on — the tool executor resolves the target. The safety gate has cloud-specific rules. The agent reasons in terms of "scale this service" not "call this AWS API" — the orchestrator translates intent to provider-specific actions.

**Q10: What happens when there are conflicting remediations? E.g., agent wants to scale up service A but scale down its dependency B?**
A: The distributed lock prevents simultaneous modifications to related services. The agent also has access to the service dependency graph and checks for ongoing remediations on dependencies before acting. If agent A is remediating service X, and agent B wants to modify X's upstream service Y, agent B will see the lock and wait or escalate.

**Q11: How do you handle compliance in regulated industries (finance, healthcare)?**
A: (1) All actions logged immutably (SOC 2). (2) Change management integration: agent actions tagged with incident ID for audit. (3) Configurable approval requirements: regulated services can require human approval for ALL actions. (4) Separation of duties: the agent that diagnoses is the same that remediates, but the approval step involves a different human. (5) Regular compliance audits against the audit log.

**Q12: How would you extend this to also handle capacity issues (not just failures)?**
A: The agent architecture naturally extends. Add tools for cloud provisioning (launch instances, add nodes to cluster). Add capacity-specific playbooks (e.g., "disk > 90% → add EBS volume"). The key difference: capacity issues are less urgent, so the agent can take more time and require more approvals. We'd integrate with the capacity planner for longer-term solutions.

**Q13: What metrics would make you lose confidence in the system and consider turning it off?**
A: (1) Success rate drops below 75% for 48 hours. (2) Any single incident where agent action caused a customer-facing P0 outage. (3) Human override rate exceeds 40% for 1 week (agent suggestions are consistently wrong). (4) Cost per remediation exceeds $5 (LLM costs out of control). (5) Agent causes an incident that takes longer to resolve than the original incident.

**Q14: Compare using Claude vs GPT-4 vs a fine-tuned open-source model for this use case.**
A: Claude Sonnet/Opus: strong reasoning, good tool use, consistent behavior. GPT-4: similar capabilities, mature function calling. Fine-tuned Llama 70B: cheaper inference, can run self-hosted for latency control, but worse at complex multi-step reasoning. Our recommendation: API-based model (Claude or GPT-4) for the reasoning engine, fine-tuned smaller model for simple pattern matching (alert classification, known-pattern detection). Self-hosting gives you latency control and data privacy but requires GPU infrastructure management.

**Q15: How do you handle the "Swiss cheese model" — multiple small failures aligning to cause a catastrophe?**
A: The safety layers are designed with this in mind: tool validation, safety gate, rate limiter, blast radius check, post-action verification, kill switch. For a catastrophic action to execute, ALL layers must fail simultaneously. We test with chaos engineering: randomly disable individual safety layers and verify the remaining layers catch dangerous actions. We also run game days where red teams try to trick the agent into harmful actions.

**Q16: What's the ROI calculation for this system?**
A: Conservative estimate. Before: 12,000 incidents/day needing remediation, average MTTR 30 minutes, 10 on-call engineers. After (Phase 4): 60% auto-remediated (7,200 incidents), MTTR 3 minutes for auto-remediated. Time saved: 7,200 * 27 minutes = 3,240 engineer-hours/day. Even at 50% efficiency (some auto-remediations would have been fast manually), that's 1,620 hours/day. At $100/hour fully loaded, that's $162K/day or ~$59M/year. System cost: ~$60K/month LLM + $20K/month infrastructure + 3 FTE platform team ($600K) = ~$1.6M/year. ROI: ~37x.

---

## 15. References

1. **Yao, S. et al.** — "ReAct: Synergizing Reasoning and Acting in Language Models" (2023): https://arxiv.org/abs/2210.03629
2. **Significant-Gravitas/AutoGPT** — Open-source autonomous agent framework: https://github.com/Significant-Gravitas/AutoGPT
3. **Anthropic** — "Tool use with Claude": https://docs.anthropic.com/en/docs/build-with-claude/tool-use
4. **Kubernetes RBAC documentation**: https://kubernetes.io/docs/reference/access-authn-authz/rbac/
5. **Netflix Chaos Engineering** — "Chaos Monkey and fault injection": https://netflix.github.io/chaosmonkey/
6. **Google SRE Book** — "Automation at Google: The Evolution of SRE": https://sre.google/sre-book/automation-at-google/
7. **Wei, J. et al.** — "Chain-of-Thought Prompting Elicits Reasoning in Large Language Models" (2022): https://arxiv.org/abs/2201.11903
8. **Shinn, N. et al.** — "Reflexion: Language Agents with Verbal Reinforcement Learning" (2023): https://arxiv.org/abs/2303.11366
9. **AWS IAM Best Practices**: https://docs.aws.amazon.com/IAM/latest/UserGuide/best-practices.html
10. **OWASP** — "LLM Top 10 Security Risks": https://owasp.org/www-project-top-10-for-large-language-model-applications/
