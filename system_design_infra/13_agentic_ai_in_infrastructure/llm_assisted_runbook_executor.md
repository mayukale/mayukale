# System Design: LLM-Assisted Runbook Executor

> **Relevance to role:** Runbooks are the operational backbone of infrastructure reliability. This system transforms static runbook documents into dynamically executable workflows, where an LLM interprets alert context, selects the appropriate runbook, fills in parameters, and executes steps with human checkpoints. As a cloud infra platform engineer, you must demonstrate ability to build an agent that bridges unstructured operational knowledge (runbook docs) with structured infrastructure actions — a critical Agentic AI competency.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Runbook storage**: Store runbooks as structured YAML with steps, conditionals, rollback instructions, and human checkpoints.
2. **Runbook selection**: Given an alert/incident, the LLM selects the most appropriate runbook using semantic matching.
3. **Dynamic parameter filling**: LLM extracts parameters (hostnames, thresholds, service names) from alert context and fills them into runbook steps.
4. **Step-by-step execution**: Execute runbook steps sequentially, verifying each step's output before proceeding.
5. **Conditional branching**: Handle if/else logic within runbooks based on step outputs.
6. **Human checkpoints**: Pause at designated steps for human approval before continuing.
7. **Parallel execution**: Execute independent steps in parallel when dependencies allow.
8. **Runbook generation**: LLM generates new runbook drafts from incident post-mortems.
9. **Versioning and A/B testing**: Track runbook versions, measure execution success rates, and A/B test improvements.
10. **Audit trail**: Complete log of every parameter, decision, and action during execution.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Runbook selection accuracy | > 90% (correct runbook selected for matching incidents) |
| Parameter extraction accuracy | > 95% (correct values filled in) |
| Execution start time | < 30 seconds from incident trigger |
| Step execution reliability | > 99% (steps that should succeed do succeed) |
| Human checkpoint response time | Show plan to human within 10 seconds |
| Runbook coverage | > 70% of incident types have a matching runbook |
| Availability | 99.9% |

### Constraints & Assumptions
- Existing runbook corpus: ~3,000 runbooks in Markdown/Confluence, varying quality.
- Standardized runbook YAML format to be defined (migration from Markdown needed).
- Infrastructure tools: kubectl, Ansible, Terraform, cloud CLIs, custom scripts.
- LLM: Claude Sonnet via API for reasoning; smaller model for parameter extraction.
- Integration with AIOps platform and PagerDuty for incident intake.

### Out of Scope
- Initial incident detection (handled by monitoring).
- Root cause analysis (handled by AIOps platform — we receive a diagnosis).
- Long-running operational procedures (> 1 hour).
- Change management workflows (separate system).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Incidents requiring runbook execution/day | 40,000 * 40% have matching runbook | ~16,000/day |
| Auto-executed runbooks/day (high confidence) | 16,000 * 50% | ~8,000/day |
| Human-approved executions/day | 16,000 * 35% | ~5,600/day |
| Escalated (no matching runbook or low confidence) | 16,000 * 15% | ~2,400/day |
| Average steps per runbook | 6 | - |
| Total step executions/day | 13,600 * 6 | ~81,600/day |
| LLM calls/day (selection + param fill + per-step reasoning) | 13,600 * 4 avg | ~54,400/day |
| Peak concurrent executions | 13,600/86400 * 10 burst * 3 min avg | ~5 concurrent |
| Runbook authors (writing/editing) | ~50 engineers | 50 |

### Latency Requirements

| Component | Target | Notes |
|---|---|---|
| Runbook selection (semantic match) | < 2s | Vector search + LLM confirmation |
| Parameter extraction | < 3s | LLM parses alert context |
| Step execution (infrastructure action) | < 30s per step | Varies by action |
| Human checkpoint display | < 10s | Show plan with parameters |
| Full runbook execution (6 steps, no human wait) | < 3 min | Including verification between steps |
| LLM call latency | < 5s per call | API response |

### Storage Estimates

| Data | Calculation | Size |
|---|---|---|
| Runbook corpus (YAML) | 3,000 * 20KB avg | ~60 MB |
| Runbook embeddings | 3,000 * 10 chunks avg * 1536 dims * 4 bytes | ~184 MB |
| Execution logs (1 year) | 13,600/day * 30KB * 365 | ~149 GB |
| Generated runbook drafts | 500/year * 20KB | ~10 MB |
| Runbook version history | 3,000 * 10 versions * 20KB | ~600 MB |
| Total | | ~150 GB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| LLM API calls (peak) | 10 concurrent * 8KB prompt | ~80 KB/s |
| LLM responses (peak) | 10 concurrent * 3KB response | ~30 KB/s |
| Step execution I/O | 5 concurrent * 5KB | ~25 KB/s |

---

## 3. High Level Architecture

```
┌───────────────────────────────────────────────────────────┐
│                   Incident Source                          │
│         AIOps Platform / PagerDuty / Manual trigger        │
└────────────────────────┬──────────────────────────────────┘
                         │
                         ▼
┌───────────────────────────────────────────────────────────┐
│                Runbook Execution Queue (Kafka)             │
│   {incident_id, diagnosis, service, region, severity}     │
└────────────────────────┬──────────────────────────────────┘
                         │
                         ▼
┌───────────────────────────────────────────────────────────────────────┐
│                   RUNBOOK ORCHESTRATOR                                │
│                                                                       │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │  1. SELECT RUNBOOK                                           │    │
│  │     ├── Embed incident context                               │    │
│  │     ├── Vector search runbook corpus (top 5)                 │    │
│  │     ├── LLM re-ranks and selects best match                  │    │
│  │     └── If no good match (confidence < 0.6) → escalate      │    │
│  └──────────────────────────┬───────────────────────────────────┘    │
│                              │                                       │
│  ┌──────────────────────────┴───────────────────────────────────┐    │
│  │  2. FILL PARAMETERS                                          │    │
│  │     ├── LLM extracts entities from alert context             │    │
│  │     │   (hostname, namespace, pod name, threshold, version)  │    │
│  │     ├── Validate parameters against CMDB/K8s API             │    │
│  │     └── Show filled runbook to human (if checkpoint)         │    │
│  └──────────────────────────┬───────────────────────────────────┘    │
│                              │                                       │
│  ┌──────────────────────────┴───────────────────────────────────┐    │
│  │  3. EXECUTE STEPS                                            │    │
│  │     ├── For each step:                                       │    │
│  │     │   ├── Check preconditions                              │    │
│  │     │   ├── Execute action via tool                          │    │
│  │     │   ├── Capture output                                   │    │
│  │     │   ├── LLM evaluates: did step succeed?                 │    │
│  │     │   ├── Handle conditional branches                      │    │
│  │     │   └── If human_checkpoint: pause for approval          │    │
│  │     │                                                        │    │
│  │     ├── Parallel steps: execute concurrently if independent  │    │
│  │     └── On failure: execute rollback steps                   │    │
│  └──────────────────────────┬───────────────────────────────────┘    │
│                              │                                       │
│  ┌──────────────────────────┴───────────────────────────────────┐    │
│  │  4. VERIFY & CLOSE                                           │    │
│  │     ├── Run verification checks (metrics normalized?)        │    │
│  │     ├── Log execution outcome                                │    │
│  │     ├── Update runbook success metrics                       │    │
│  │     └── If not resolved: escalate or try alternate runbook   │    │
│  └──────────────────────────────────────────────────────────────┘    │
└──────┬──────────────┬──────────────┬───────────────┬─────────────────┘
       │              │              │               │
       ▼              ▼              ▼               ▼
┌──────────┐  ┌────────────┐ ┌────────────┐  ┌──────────────┐
│ Runbook  │  │ LLM Service│ │ Tool       │  │ Human        │
│ Store    │  │ (Claude    │ │ Executor   │  │ Approval     │
│          │  │  Sonnet)   │ │ (kubectl,  │  │ Gateway      │
│ YAML +   │  │            │ │  Ansible,  │  │ (Slack/      │
│ Vector DB│  │            │ │  scripts)  │  │  PagerDuty)  │
└──────────┘  └────────────┘ └────────────┘  └──────────────┘

┌───────────────────────────────────────────────────────────────────────┐
│                   Runbook Authoring & Management                      │
│                                                                       │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────────────────┐    │
│  │ YAML Editor  │  │ LLM Runbook  │  │ Version Control (Git)   │    │
│  │ + Validator  │  │ Generator    │  │ + A/B Testing Framework  │    │
│  │              │  │ (from post-  │  │                          │    │
│  │              │  │  mortems)    │  │                          │    │
│  └──────────────┘  └──────────────┘  └─────────────────────────┘    │
└───────────────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────────────┐
│                   Observability & Feedback                            │
│  Execution logs │ Success rates │ A/B metrics │ Coverage dashboards  │
└───────────────────────────────────────────────────────────────────────┘
```

### Agent Loop: Observe → Reason → Act → Verify

```
OBSERVE: Receive incident (alerts, diagnosis, affected service, region)
    │
    ▼
REASON: Select runbook, fill parameters, generate execution plan
    │    LLM: "This matches runbook RB-042: High Memory Usage.
    │    Parameters: service=payment-api, namespace=payments,
    │    threshold=90%, current=94%"
    │
    ▼
ACT:   Execute runbook steps sequentially
    │  Step 1: Check current memory usage → 94% (confirmed)
    │  Step 2: Identify top memory consumers → payment-api-pod-1 (2.1GB)
    │  Step 3: Restart top consumer pod → success
    │  Step 4: [human_checkpoint] Scale if memory still high
    │
    ▼
VERIFY: Check metrics after execution
        Memory dropped to 62%. Alert resolved. Close incident.
```

---

## 4. Data Model

### Core Entities & Schema

**Runbook YAML Schema:**

```yaml
# Example: RB-042 — High Memory Usage
runbook_id: "RB-042"
version: 3
title: "High Memory Usage Remediation"
description: "Steps to diagnose and remediate high memory usage on a Kubernetes service"
trigger:
  alert_names: ["HighMemoryUsage", "OOMKilled", "MemoryPressure"]
  services: ["*"]  # applies to any service
  severity: ["warning", "critical"]

parameters:
  - name: service_name
    type: string
    source: alert_label  # extract from alert labels
    label_key: "service"
    required: true
  - name: namespace
    type: string
    source: alert_label
    label_key: "namespace"
    required: true
  - name: memory_threshold_pct
    type: float
    source: alert_label
    label_key: "threshold"
    default: 90.0
  - name: max_restart_count
    type: int
    source: config
    default: 3

steps:
  - id: check_memory
    name: "Check current memory usage"
    action: query_metrics
    params:
      query: "container_memory_usage_bytes{namespace='{{namespace}}', pod=~'{{service_name}}.*'} / container_spec_memory_limit_bytes * 100"
      duration: "15m"
    output_var: current_memory_pct
    verify:
      condition: "output.status == 'success'"
      on_failure: escalate

  - id: identify_consumers
    name: "Identify top memory-consuming pods"
    action: kubectl_get
    params:
      resource: pods
      namespace: "{{namespace}}"
      selector: "app={{service_name}}"
      output: json
      sort_by: ".status.containerStatuses[0].resources.usage.memory"
    output_var: pod_list
    verify:
      condition: "len(output.items) > 0"
      on_failure: escalate

  - id: check_oom_events
    name: "Check for OOMKilled events"
    action: kubectl_get
    params:
      resource: events
      namespace: "{{namespace}}"
      field_selector: "reason=OOMKilling"
    output_var: oom_events

  - id: restart_decision
    name: "Decide whether to restart pods"
    type: conditional
    condition: "current_memory_pct > memory_threshold_pct"
    if_true: restart_pods
    if_false: monitor_only

  - id: restart_pods
    name: "Restart highest-memory pod"
    action: kubectl_delete_pod
    params:
      pod: "{{pod_list.items[0].metadata.name}}"
      namespace: "{{namespace}}"
    preconditions:
      - "healthy_replicas(namespace, service_name) >= 2"
    human_checkpoint: false  # auto for pod restart
    rollback: null  # K8s recreates pod automatically
    verify:
      condition: "output.status == 'success'"
      on_failure: escalate

  - id: wait_and_check
    name: "Wait 2 minutes and verify memory decreased"
    action: wait
    params:
      duration: 120
    next: verify_memory

  - id: verify_memory
    name: "Verify memory returned to normal"
    action: query_metrics
    params:
      query: "container_memory_usage_bytes{namespace='{{namespace}}', pod=~'{{service_name}}.*'} / container_spec_memory_limit_bytes * 100"
      duration: "5m"
    verify:
      condition: "output.value < memory_threshold_pct"
      on_failure: scale_up_step

  - id: scale_up_step
    name: "Scale up deployment if memory still high"
    action: kubectl_scale
    params:
      deployment: "{{service_name}}"
      namespace: "{{namespace}}"
      replicas: "+2"  # add 2 replicas
    human_checkpoint: true  # require approval for scaling
    rollback:
      action: kubectl_scale
      params:
        deployment: "{{service_name}}"
        namespace: "{{namespace}}"
        replicas: "-2"

  - id: monitor_only
    name: "Memory below threshold — monitor only"
    action: add_note
    params:
      message: "Memory at {{current_memory_pct}}% — below threshold. Monitoring."

rollback:
  on_failure: "Undo all write actions in reverse order"
  max_rollback_steps: 3

metadata:
  author: "sre-team"
  last_updated: "2026-03-15"
  success_rate: 0.87
  avg_execution_time_sec: 210
  times_executed: 342
  tags: ["memory", "kubernetes", "auto-remediation"]
```

**Database Schema:**

```sql
-- Runbook definitions
CREATE TABLE runbooks (
    runbook_id          VARCHAR(50) PRIMARY KEY,
    version             INT NOT NULL,
    title               VARCHAR(500) NOT NULL,
    description         TEXT,
    yaml_content        TEXT NOT NULL,
    trigger_config      JSONB NOT NULL,          -- {alert_names, services, severity}
    parameters          JSONB NOT NULL,          -- parameter definitions
    step_count          INT NOT NULL,
    has_human_checkpoint BOOLEAN DEFAULT FALSE,
    tags                TEXT[],
    author              VARCHAR(100),
    status              VARCHAR(20) DEFAULT 'active', -- 'active', 'deprecated', 'draft', 'testing'
    embedding           VECTOR(1536),
    created_at          TIMESTAMPTZ DEFAULT NOW(),
    updated_at          TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(runbook_id, version)
);

-- Execution records
CREATE TABLE runbook_executions (
    execution_id        UUID PRIMARY KEY,
    runbook_id          VARCHAR(50) NOT NULL,
    runbook_version     INT NOT NULL,
    incident_id         UUID NOT NULL,
    status              VARCHAR(20) NOT NULL,    -- 'running', 'succeeded', 'failed', 'escalated',
                                                 -- 'rolled_back', 'awaiting_approval'
    parameters_filled   JSONB NOT NULL,          -- actual parameter values used
    parameter_source    JSONB,                   -- where each param came from (alert_label, llm_extracted, default)
    confidence          FLOAT,                   -- LLM confidence in runbook selection
    started_at          TIMESTAMPTZ DEFAULT NOW(),
    completed_at        TIMESTAMPTZ,
    duration_sec        INT,
    triggered_by        VARCHAR(100),            -- 'auto', 'manual', username
    ab_group            VARCHAR(10)              -- 'A' or 'B' for A/B testing
);

-- Step execution records
CREATE TABLE step_executions (
    step_execution_id   UUID PRIMARY KEY,
    execution_id        UUID REFERENCES runbook_executions(execution_id),
    step_id             VARCHAR(100) NOT NULL,
    step_name           VARCHAR(500),
    step_number         INT NOT NULL,
    action              VARCHAR(100) NOT NULL,
    params_resolved     JSONB NOT NULL,          -- with template variables filled
    status              VARCHAR(20) NOT NULL,    -- 'pending', 'running', 'succeeded', 'failed',
                                                 -- 'skipped', 'awaiting_approval', 'rolled_back'
    output              JSONB,
    llm_interpretation  TEXT,                    -- LLM's interpretation of step output
    precondition_result JSONB,
    human_approved_by   VARCHAR(100),
    started_at          TIMESTAMPTZ,
    completed_at        TIMESTAMPTZ,
    duration_ms         INT
);

-- Runbook generation drafts (from post-mortems)
CREATE TABLE runbook_drafts (
    draft_id            UUID PRIMARY KEY,
    source_incident_id  UUID,
    source_postmortem   TEXT,
    generated_yaml      TEXT NOT NULL,
    llm_explanation     TEXT,                    -- why the LLM structured it this way
    review_status       VARCHAR(20) DEFAULT 'pending', -- 'pending', 'approved', 'rejected', 'revised'
    reviewed_by         VARCHAR(100),
    review_notes        TEXT,
    created_at          TIMESTAMPTZ DEFAULT NOW()
);

-- A/B test configuration
CREATE TABLE runbook_ab_tests (
    test_id             UUID PRIMARY KEY,
    runbook_id          VARCHAR(50) NOT NULL,
    version_a           INT NOT NULL,
    version_b           INT NOT NULL,
    traffic_split       FLOAT DEFAULT 0.5,       -- fraction going to version B
    start_date          TIMESTAMPTZ NOT NULL,
    end_date            TIMESTAMPTZ,
    status              VARCHAR(20) DEFAULT 'active',
    results             JSONB                    -- {version_a: {success_rate, avg_time}, version_b: {...}}
);
```

### Database Selection

| Store | Technology | Justification |
|---|---|---|
| Runbook definitions + versions | PostgreSQL + Git (source of truth) | ACID for metadata, Git for version history |
| Runbook embeddings | pgvector | Semantic search for runbook selection |
| Execution logs | PostgreSQL (hot) + S3 (archive) | Queryable for analytics, archived for compliance |
| Execution queue | Kafka | Reliable queueing with priority |
| Draft storage | PostgreSQL | Simple CRUD |

### Indexing Strategy

```sql
CREATE INDEX idx_runbooks_status ON runbooks (status) WHERE status = 'active';
CREATE INDEX idx_runbooks_tags ON runbooks USING GIN (tags);
CREATE INDEX idx_runbooks_embedding ON runbooks USING ivfflat (embedding vector_cosine_ops) WITH (lists = 50);
CREATE INDEX idx_executions_incident ON runbook_executions (incident_id);
CREATE INDEX idx_executions_runbook ON runbook_executions (runbook_id, started_at DESC);
CREATE INDEX idx_executions_status ON runbook_executions (status) WHERE status IN ('running', 'awaiting_approval');
CREATE INDEX idx_step_executions_execution ON step_executions (execution_id, step_number);
```

---

## 5. API Design

### REST Endpoints

```
# Runbook management
GET    /api/v1/runbooks                              # List all active runbooks
GET    /api/v1/runbooks/{id}                         # Get runbook definition
POST   /api/v1/runbooks                              # Create new runbook
PUT    /api/v1/runbooks/{id}                         # Update runbook (creates new version)
POST   /api/v1/runbooks/{id}/validate                # Validate YAML schema
POST   /api/v1/runbooks/{id}/test                    # Dry-run execution in staging

# Runbook selection (for testing)
POST   /api/v1/runbooks/match
{
  "alert_name": "HighMemoryUsage",
  "service": "payment-api",
  "severity": "critical",
  "labels": {...}
}

# Execution
POST   /api/v1/executions                            # Start execution manually
GET    /api/v1/executions/{id}                       # Get execution status + steps
POST   /api/v1/executions/{id}/approve               # Approve human checkpoint
POST   /api/v1/executions/{id}/reject                # Reject and stop
POST   /api/v1/executions/{id}/rollback              # Rollback executed steps

# Runbook generation
POST   /api/v1/runbooks/generate
{
  "source": "postmortem",
  "incident_id": "uuid",
  "postmortem_text": "..."
}
GET    /api/v1/runbooks/drafts                       # List generated drafts
POST   /api/v1/runbooks/drafts/{id}/approve          # Approve draft → publish

# A/B testing
POST   /api/v1/ab-tests                              # Start A/B test
GET    /api/v1/ab-tests/{id}/results                 # Get A/B test results

# Analytics
GET    /api/v1/analytics/coverage                    # % of incidents with matching runbook
GET    /api/v1/analytics/success-rates               # Per-runbook success rates
GET    /api/v1/analytics/parameter-accuracy           # Parameter extraction accuracy
```

### Agent Tool Call Interface

```json
{
  "tools": [
    {
      "name": "search_runbooks",
      "description": "Semantic search for runbooks matching an incident",
      "parameters": {
        "query": "string — incident description",
        "alert_name": "string — alert name",
        "service": "string — affected service",
        "top_k": "int (default 5)"
      }
    },
    {
      "name": "validate_parameter",
      "description": "Validate an extracted parameter against CMDB/K8s",
      "parameters": {
        "param_name": "string",
        "param_value": "string",
        "validation_type": "string — 'exists_in_k8s', 'exists_in_cmdb', 'is_valid_ip', etc."
      }
    },
    {
      "name": "execute_step",
      "description": "Execute a single runbook step",
      "parameters": {
        "step_id": "string",
        "action": "string — tool to call",
        "params": "object — resolved parameters"
      }
    },
    {
      "name": "evaluate_step_output",
      "description": "LLM evaluates whether a step succeeded based on its output",
      "parameters": {
        "step_id": "string",
        "expected_condition": "string",
        "actual_output": "object"
      }
    },
    {
      "name": "request_human_approval",
      "description": "Pause execution and request human approval",
      "parameters": {
        "execution_id": "string",
        "step_id": "string",
        "plan_summary": "string",
        "risk_assessment": "string"
      }
    },
    {
      "name": "generate_runbook_draft",
      "description": "Generate a new runbook YAML from a post-mortem document",
      "parameters": {
        "postmortem_text": "string",
        "incident_id": "string"
      }
    }
  ]
}
```

### Human Escalation API

```
POST /api/v1/escalations
{
  "incident_id": "uuid",
  "execution_id": "uuid",
  "reason": "no_matching_runbook",
  "attempted_matches": [
    {"runbook_id": "RB-042", "confidence": 0.45, "reason": "Service doesn't match trigger pattern"},
    {"runbook_id": "RB-089", "confidence": 0.38, "reason": "Alert type partially matches"}
  ],
  "incident_context": {
    "alert": "CustomAlertXYZ",
    "service": "new-feature-service",
    "diagnosis": "Unknown error pattern in new service"
  },
  "suggestion": "This incident may require a new runbook. Post-mortem should generate one."
}
```

---

## 6. Core Component Deep Dives

### 6.1 Runbook Selection Engine

**Why it's hard:** The runbook corpus contains 3,000 runbooks with overlapping trigger conditions. An alert might match multiple runbooks. The LLM must select the most specific, most effective runbook. False positives (wrong runbook) waste time and may cause harm. False negatives (no match when one exists) result in unnecessary human escalation.

| Approach | Pros | Cons |
|---|---|---|
| **Exact trigger matching** | Fast, deterministic | Too rigid; misses semantic matches; requires perfect alert naming |
| **Keyword search (BM25)** | Handles variations in naming | Misses semantic similarity ("OOM" vs "out of memory") |
| **Semantic search (embeddings)** | Captures meaning, not just keywords | May surface tangentially related runbooks |
| **Hybrid: trigger match + semantic + LLM re-rank** | Most accurate; handles exact and fuzzy matches | More complex; LLM latency |

**Selected: Hybrid (trigger match → semantic search → LLM re-rank)**

**Implementation:**

```python
class RunbookSelector:
    async def select(self, incident):
        # Phase 1: Exact trigger match (fast, deterministic)
        exact_matches = self.trigger_match(incident)
        if len(exact_matches) == 1:
            return exact_matches[0], confidence=0.95

        # Phase 2: Semantic search (broader net)
        query = f"{incident.alert_name} {incident.service} {incident.diagnosis}"
        semantic_matches = await self.vector_search(query, top_k=10)

        # Phase 3: Merge and deduplicate
        candidates = self.merge_candidates(exact_matches, semantic_matches)

        if len(candidates) == 0:
            return None, confidence=0.0  # escalate

        # Phase 4: LLM re-ranks
        reranked = await self.llm_rerank(incident, candidates[:5])

        # Phase 5: Confidence check
        best = reranked[0]
        if best.confidence < 0.6:
            return None, confidence=best.confidence  # escalate with partial match info

        return best.runbook, best.confidence

    async def llm_rerank(self, incident, candidates):
        prompt = f"""
Given this incident:
- Alert: {incident.alert_name}
- Service: {incident.service}
- Diagnosis: {incident.diagnosis}
- Severity: {incident.severity}

Rank the following runbooks by relevance (most relevant first).
For each, explain why it matches or doesn't match.
Rate confidence 0-1 for the best match.

Runbooks:
{self.format_candidates(candidates)}
"""
        response = await self.llm.generate(prompt)
        return self.parse_rankings(response)
```

**Failure Modes:**
- **Multiple runbooks equally relevant**: LLM picks one but might be wrong. Mitigation: show top-2 to human with reasoning.
- **Outdated runbook selected**: Runbook exists but is stale. Mitigation: weight recently-updated runbooks higher; alert if selected runbook hasn't been reviewed in 6 months.
- **Service-specific vs generic runbook**: Generic "high CPU" runbook selected instead of service-specific one. Mitigation: boost service-specific runbooks in scoring.

**Interviewer Q&As:**

**Q1: How do you handle the case where the correct runbook exists but uses different terminology than the alert?**
A: Semantic search handles this. "OOMKilled" and "out of memory" have similar embeddings. "Connection refused" and "port not reachable" map to the same intent. The embedding model captures these semantic relationships. We also maintain a synonym mapping (manually curated) that expands queries before search.

**Q2: How do you measure runbook selection accuracy?**
A: Two methods. (1) Implicit feedback: if the selected runbook executes successfully and resolves the incident, it was likely correct. (2) Explicit feedback: when a human is involved (checkpoint or escalation), they can flag "wrong runbook" and select the correct one. We track selection accuracy as: (correct selections + successful auto-executions) / total selections.

**Q3: What if multiple services share the same runbook?**
A: The runbook's trigger config supports wildcards: `services: ["*"]` for universal runbooks, or `services: ["payment-*"]` for service family matches. The LLM also considers whether a generic runbook is appropriate for the specific service, taking into account service-specific quirks.

**Q4: How do you handle versioned runbooks?**
A: Only the `active` version is used for execution. When a runbook is updated, the old version becomes `deprecated` but remains in the database for audit trail. A/B tests can compare two versions. The vector embedding is regenerated when the runbook content changes.

**Q5: How fast is the selection process?**
A: Phase 1 (trigger match): < 50ms (SQL query). Phase 2 (semantic search): < 300ms (pgvector). Phase 3 (merge): < 10ms. Phase 4 (LLM re-rank): < 3s (LLM call). Total: < 4s. If we skip LLM re-rank for exact trigger matches with single result: < 100ms.

**Q6: What about runbooks that are applicable only during certain conditions (maintenance windows, specific times)?**
A: The runbook YAML supports `applicability` conditions: `only_during: maintenance_window`, `not_during: peak_hours`, `requires_service_mesh: true`. The selector checks these conditions before including a runbook in candidates. The LLM also reasons about applicability based on current context.

---

### 6.2 Dynamic Parameter Filling

**Why it's hard:** Runbooks contain template variables ({{hostname}}, {{namespace}}, {{threshold}}) that must be filled with correct values extracted from the alert context. Incorrect parameter filling is dangerous — running a restart command on the wrong pod, or applying a config to the wrong namespace. The LLM must correctly map alert labels to runbook parameters.

| Approach | Pros | Cons |
|---|---|---|
| **Direct label mapping** | Simple, deterministic | Only works when label names match exactly |
| **Rule-based extraction** | Handles common patterns | Brittle; new patterns require new rules |
| **LLM extraction** | Flexible; handles any format | May hallucinate parameter values |
| **Hybrid: direct mapping + LLM for ambiguous** | Most robust | More implementation effort |

**Selected: Hybrid (direct mapping first, LLM for ambiguous, validation for all)**

**Implementation:**

```python
class ParameterFiller:
    async def fill_parameters(self, runbook, incident):
        filled = {}
        needs_llm = []

        for param in runbook.parameters:
            # Phase 1: Direct mapping (deterministic)
            if param.source == 'alert_label' and param.label_key in incident.labels:
                filled[param.name] = incident.labels[param.label_key]
            elif param.source == 'config' and param.default is not None:
                filled[param.name] = param.default
            else:
                needs_llm.append(param)

        # Phase 2: LLM extraction for remaining parameters
        if needs_llm:
            llm_filled = await self.llm_extract(needs_llm, incident)
            filled.update(llm_filled)

        # Phase 3: Validate ALL parameters (including directly mapped)
        for param_name, param_value in filled.items():
            validation = await self.validate(param_name, param_value, runbook)
            if not validation.valid:
                raise ParameterValidationError(
                    f"Parameter {param_name}={param_value} failed validation: {validation.reason}"
                )

        return filled

    async def validate(self, name, value, runbook):
        """Validate parameter against live infrastructure state."""
        param_def = runbook.get_param(name)

        if param_def.type == 'string' and 'namespace' in name:
            # Verify namespace exists in Kubernetes
            exists = await self.k8s.namespace_exists(value)
            if not exists:
                return ValidationResult(False, f"Namespace '{value}' does not exist")

        if param_def.type == 'string' and 'pod' in name:
            # Verify pod exists
            exists = await self.k8s.pod_exists(value, namespace=runbook.params.get('namespace'))
            if not exists:
                return ValidationResult(False, f"Pod '{value}' does not exist")

        if param_def.type == 'string' and ('host' in name or 'server' in name):
            # Verify host in CMDB
            exists = await self.cmdb.host_exists(value)
            if not exists:
                return ValidationResult(False, f"Host '{value}' not found in CMDB")

        return ValidationResult(True, "OK")
```

**LLM extraction prompt:**

```
Given the following incident alert and runbook parameters, extract the correct values.

Alert:
  Name: HighMemoryUsage
  Labels: {service: "payment-api", namespace: "payments", severity: "critical", 
           instance: "10.0.1.42:9090", cluster: "prod-us-east-1"}
  Annotations: {summary: "Payment API memory usage at 94% (threshold 90%)"}

Parameters to extract:
  - service_name (string, required): The name of the affected service
  - namespace (string, required): Kubernetes namespace
  - memory_threshold_pct (float): The configured threshold percentage

For each parameter, provide:
  - value: the extracted value
  - source: where you found it (e.g., "label:service", "annotation:summary")
  - confidence: 0-1

IMPORTANT: Only extract values that are explicitly present in the alert data.
Do NOT guess or infer values that are not directly stated.
```

**Failure Modes:**
- **LLM hallucinates a hostname**: "The server is web-prod-42" when no such server exists. Mitigation: every parameter is validated against CMDB/K8s before use.
- **Ambiguous label mapping**: Alert has `instance` and `host` labels — which maps to the runbook's `hostname` parameter? Mitigation: LLM resolves ambiguity based on runbook context.
- **Missing parameter**: Alert doesn't contain required parameter. Mitigation: check `required` field; escalate to human if missing.

**Interviewer Q&As:**

**Q1: What if the LLM extracts the wrong value with high confidence?**
A: That's why Phase 3 (validation) is critical. The LLM might extract "payment-service" when the actual pod is "payment-api." But the K8s API validation will fail because "payment-service" doesn't exist as a deployment. The system then: (1) Reports the validation failure. (2) Asks the LLM to try again with the error context. (3) If still failing, escalates to human.

**Q2: How do you handle runbooks with many parameters?**
A: Some runbooks have 10+ parameters. We process them in three tiers. Tier 1: directly mapped from alert labels (instant). Tier 2: derived from Tier 1 (e.g., pod name from service name via kubectl lookup). Tier 3: LLM-extracted or human-provided. This minimizes LLM calls and maximizes accuracy.

**Q3: Can parameter values change during execution?**
A: Yes. Some parameters are "live-resolved" at each step. For example, "top memory consumer pod" might change between steps if a pod restarts. Steps can define `resolve_at: execution_time` to re-query the value at runtime rather than using the initially filled value.

**Q4: How do you handle sensitive parameters (credentials, tokens)?**
A: Runbook parameters never contain secrets directly. Instead, they reference secret stores: `db_password: vault://secrets/payment-db/password`. The step executor resolves the reference at execution time and never logs the actual secret value.

**Q5: What's the parameter extraction accuracy in practice?**
A: For directly mapped parameters (label match): 99.9% accuracy. For LLM-extracted parameters: 93-97% accuracy (validated). Combined with validation catching errors: effective accuracy > 99%. The 1% residual risk is caught by human checkpoints on write operations.

**Q6: How do you handle parameters that require human input?**
A: The parameter definition can specify `source: human_input`. During execution, the system pauses and asks the human: "Please provide the rollback target version for service payment-api." The human response is validated before continuing.

---

### 6.3 Runbook Generation from Post-Mortems

**Why it's hard:** Post-mortems are unstructured text describing what happened, why, and how it was fixed. Extracting a structured, executable runbook from this narrative requires the LLM to identify the resolution steps, generalize them (replace specific hostnames with template variables), add preconditions and verification steps, and structure conditionals. The generated runbook must be correct enough to execute safely.

| Approach | Pros | Cons |
|---|---|---|
| **Manual writing by SRE** | High quality | Slow (days to weeks); knowledge bottleneck |
| **Template-based generation** | Fast for standard patterns | Cannot handle novel patterns |
| **LLM generation from post-mortem** | Fast, handles any pattern | May miss nuances; requires human review |
| **LLM draft + human refinement** | Fast first draft, human quality assurance | Still requires human time (but much less) |

**Selected: LLM draft + human refinement**

**Implementation:**

```python
class RunbookGenerator:
    async def generate_from_postmortem(self, postmortem_text, incident_id):
        prompt = f"""
You are a senior SRE writing a runbook. Given the following incident post-mortem,
generate a structured runbook in YAML format that could be used to remediate
similar incidents in the future.

POST-MORTEM:
{postmortem_text}

REQUIREMENTS:
1. Extract the resolution steps from the post-mortem.
2. Generalize specific values into template parameters (e.g., replace
   "payment-api-pod-7b9d4" with "{{pod_name}}").
3. Add precondition checks before any write operation.
4. Add verification steps after each action.
5. Add human_checkpoint: true for any action that could cause data loss
   or significant user impact.
6. Include a rollback section.
7. Include trigger conditions (which alerts should trigger this runbook).

Output valid YAML following this schema:
[schema definition]

IMPORTANT: This is a DRAFT. It will be reviewed by a human before activation.
Mark any assumptions with comments like "# ASSUMPTION: ..."
"""
        yaml_response = await self.llm.generate(prompt, max_tokens=4000)

        # Validate YAML syntax
        try:
            parsed = yaml.safe_load(yaml_response)
        except yaml.YAMLError as e:
            # Ask LLM to fix YAML errors
            yaml_response = await self.fix_yaml(yaml_response, str(e))

        # Validate against runbook schema
        validation = self.validate_schema(parsed)

        # Store as draft (never auto-publish)
        draft = RunbookDraft(
            source_incident_id=incident_id,
            source_postmortem=postmortem_text,
            generated_yaml=yaml_response,
            llm_explanation=self.extract_explanation(yaml_response),
            review_status='pending'
        )
        await self.db.insert(draft)
        return draft
```

**Quality checks on generated runbooks:**

1. **Schema validation**: Does the YAML conform to the runbook schema?
2. **Parameter completeness**: Are all referenced template variables defined?
3. **Safety check**: Do all write operations have preconditions?
4. **Rollback coverage**: Does every write operation have a rollback step?
5. **Verification coverage**: Does every action step have a verification step?
6. **Human checkpoint check**: Are high-risk actions marked for human approval?

**Failure Modes:**
- **LLM invents steps that weren't in the post-mortem**: Mitigation: generated runbook includes source references (which part of the post-mortem each step came from). Reviewer can verify.
- **Over-generalization**: LLM creates a generic runbook that misses service-specific nuances. Mitigation: reviewer adds service-specific conditions.
- **Under-generalization**: LLM leaves in specific hostnames/values instead of templating. Mitigation: automated check for hardcoded values.

**Interviewer Q&As:**

**Q1: How many runbooks do you expect the LLM to generate vs. human-written?**
A: Initially, most runbooks are human-written (migrated from existing docs). Over time, after every post-mortem, the system generates a draft. Expected: ~100 new runbook drafts per quarter. Of those, ~60% are approved with minor edits, ~20% need major revision, ~20% are rejected (too complex or one-off). The LLM saves ~70% of the writing effort.

**Q2: How do you handle post-mortems that describe multiple root causes or solutions?**
A: The LLM is instructed to generate one runbook per distinct resolution path. A post-mortem with two root causes may generate two runbook drafts. The LLM explains the branching: "If the root cause is X, use runbook A. If it's Y, use runbook B." Or it can generate a single runbook with conditional branching.

**Q3: Can the LLM update existing runbooks based on new post-mortems?**
A: Yes. If a post-mortem reveals that an existing runbook missed a step or a precondition, the system can generate a diff: "Based on incident #1234, consider adding this step between steps 3 and 4." The diff is presented to the runbook owner as a pull request.

**Q4: How do you test generated runbooks before production use?**
A: Three-phase testing. (1) Schema validation + automated safety checks (instant). (2) Dry-run in staging with simulated incident (30 min). (3) Shadow execution in production — run the runbook in parallel with human remediation, compare actions but don't execute the runbook's actions (1 week). Only after all three phases does the runbook enter the active corpus.

---

## 7. AI Agent Architecture

### Agent Loop Design

```
┌─────────────────────────────────────────────────────────────────┐
│             RUNBOOK EXECUTOR AGENT LIFECYCLE                     │
│                                                                  │
│  RECEIVE INCIDENT                                                │
│    │                                                             │
│    ▼                                                             │
│  SELECT RUNBOOK                                                  │
│    │  Vector search + trigger match + LLM re-rank               │
│    │  If no match: escalate with "no runbook" reason            │
│    │                                                             │
│    ▼                                                             │
│  FILL PARAMETERS                                                 │
│    │  Direct mapping → LLM extraction → validation              │
│    │  If validation fails: retry or escalate                    │
│    │                                                             │
│    ▼                                                             │
│  PRESENT PLAN (if human_checkpoint or medium/high risk)         │
│    │  Show: selected runbook, filled parameters, step summary   │
│    │  Wait for approval (timeout: 10 min → escalate)           │
│    │                                                             │
│    ▼                                                             │
│  EXECUTE STEPS (sequential or parallel per dependency graph)    │
│    │  For each step:                                            │
│    │    1. Check preconditions                                  │
│    │    2. Execute action                                       │
│    │    3. Capture output                                       │
│    │    4. LLM evaluates: success or failure?                  │
│    │    5. If conditional: branch based on output               │
│    │    6. If human_checkpoint: pause for approval              │
│    │    7. If failure: attempt rollback of completed steps      │
│    │                                                             │
│    ▼                                                             │
│  VERIFY RESOLUTION                                               │
│    │  Run verification checks from runbook                      │
│    │  Query metrics: is the incident resolved?                  │
│    │  If resolved: close, log success                           │
│    │  If not: try alternate steps or escalate                   │
│    │                                                             │
│    ▼                                                             │
│  POST-EXECUTION                                                  │
│    │  Update runbook success rate metrics                       │
│    │  Log full execution trace                                  │
│    │  If failed: suggest runbook update to author               │
└─────────────────────────────────────────────────────────────────┘
```

### Tool Definitions

| Tool | Risk | Used By |
|---|---|---|
| `search_runbooks` (vector + trigger) | None | Runbook selection |
| `validate_parameter` (K8s/CMDB lookup) | None | Parameter filling |
| `kubectl_get`, `kubectl_describe`, `kubectl_logs` | None | Read-only steps |
| `query_metrics` (Prometheus) | None | Verification steps |
| `query_logs` (Loki/ES) | None | Diagnostic steps |
| `kubectl_delete_pod` | Low | Pod restart steps |
| `kubectl_scale` | Medium | Scaling steps |
| `kubectl_apply` | High | Config change steps |
| `kubectl_rollback` | High | Rollback steps |
| `ssh_run_command` (allowlisted) | High | Server-level remediation |
| `ansible_run_playbook` | High | Multi-host operations |
| `request_human_approval` | None | Human checkpoint |

### Context Window Management

Each LLM call has a specific, focused purpose:

| Call | Context Size | Content |
|---|---|---|
| Runbook selection | ~4K tokens | Incident context + 5 candidate runbook summaries |
| Parameter extraction | ~2K tokens | Alert labels + parameter definitions |
| Step output evaluation | ~2K tokens | Step definition + step output + expected condition |
| Conditional branch decision | ~2K tokens | Current state + branch conditions |
| Post-mortem → runbook generation | ~8K tokens | Full post-mortem + schema + instructions |

No single call needs a large context window. This is by design — each LLM call is a focused reasoning task, not a long conversation.

### Memory Architecture

| Memory Type | Content | Storage |
|---|---|---|
| **Episodic** | Past runbook execution results (which worked, which failed) | PostgreSQL `runbook_executions` + `step_executions` |
| **Semantic** | Runbook corpus (YAML + embeddings) | PostgreSQL + pgvector |
| **Procedural** | Runbook step definitions, tool interfaces | Loaded from YAML at execution time |
| **Working** | Current execution state (filled params, step outputs) | In-memory per execution session |

### Guardrails and Safety

1. **Schema validation**: Every runbook must pass YAML schema validation before activation.
2. **Precondition enforcement**: Steps with write actions must have preconditions; execution halts if preconditions fail.
3. **Human checkpoints**: Configurable per-step; enforced for all high-risk actions.
4. **Parameter validation**: All parameters validated against live infrastructure state before execution.
5. **Step timeout**: Each step has a maximum execution time (default: 60s). Exceeding triggers failure handling.
6. **Max steps per execution**: 20 steps maximum. Prevents runaway execution.
7. **Rollback guarantee**: Every write step must define a rollback action or be marked as `irreversible: true` (requires human approval).
8. **No runbook auto-publish**: Generated runbooks are always drafts requiring human review.

### Confidence Thresholds

| Confidence (runbook selection) | Action |
|---|---|
| > 0.90 + all params from labels | Auto-execute (no human checkpoint unless runbook requires it) |
| 0.70 - 0.90 | Show plan to human, auto-execute after 5-min timeout if no objection |
| 0.60 - 0.70 | Show plan to human, require explicit approval |
| < 0.60 | Escalate — no good runbook match found |

### Dry-Run Mode

```yaml
dry_run_output:
  execution_id: "exec-abc-123"
  incident_id: "inc-xyz-789"
  selected_runbook:
    id: "RB-042"
    version: 3
    title: "High Memory Usage Remediation"
    confidence: 0.88
  parameters:
    service_name: "payment-api"       # source: alert_label
    namespace: "payments"             # source: alert_label
    memory_threshold_pct: 90.0        # source: alert_annotation
    max_restart_count: 3              # source: config_default
  planned_steps:
    - step: check_memory
      action: "query_metrics(container_memory_usage_bytes{namespace='payments', pod=~'payment-api.*'})"
      risk: none
      would_execute: true
    - step: identify_consumers
      action: "kubectl_get pods -n payments -l app=payment-api --sort-by memory"
      risk: none
      would_execute: true
    - step: restart_decision
      type: conditional
      condition: "memory > 90%"
      branches: ["restart_pods", "monitor_only"]
    - step: restart_pods
      action: "kubectl_delete_pod payment-api-pod-xxx -n payments"
      risk: low
      preconditions: ["healthy_replicas >= 2"]
      would_execute: true  # if condition met
    - step: verify_memory
      action: "query_metrics (wait 2m, then check memory)"
      risk: none
    - step: scale_up_step
      action: "kubectl_scale deployment/payment-api --replicas=+2"
      risk: medium
      human_checkpoint: true
      would_execute: "only if verify_memory fails"
  estimated_duration: "3-5 minutes (excluding human checkpoint wait)"
```

---

## 8. Scaling Strategy

| Component | Scaling Approach | Trigger |
|---|---|---|
| Runbook orchestrator | Horizontal (K8s HPA) | Queue depth > 10 |
| LLM calls | Rate limiting + model tiering | Latency > 5s |
| Vector DB (pgvector) | Read replicas | Query latency > 300ms |
| Step executor | Horizontal | Concurrent executions > 10 |
| Kafka queue | Partitions by service hash | Consumer lag > 30s |

### Interviewer Q&As

**Q1: Can you execute 100 runbooks simultaneously during a major incident?**
A: Yes, with constraints. (1) Each runbook execution is independent, so horizontal scaling of orchestrator workers handles the concurrency. (2) The bottleneck is LLM API rate limits — at 100 concurrent, we'd need ~400 LLM calls (4 per runbook). With a rate limit of 100 calls/min, we'd queue up. Mitigation: use smaller models for simple tasks (parameter extraction), cache common runbook selections, and prioritize critical incidents.

**Q2: What's the bottleneck?**
A: LLM inference latency. Each execution requires 3-5 LLM calls (selection, parameter extraction, step evaluation). At 5s per call, that's 15-25s of LLM time. Tool execution (kubectl, etc.) is typically < 10s per step. Mitigation: parallelize LLM calls where possible (e.g., evaluate multiple steps' outputs in a single batched call).

**Q3: How do you handle runbook corpus growth (from 3,000 to 30,000)?**
A: (1) pgvector handles 30K runbooks with < 300ms query time (it's built for this). (2) The trigger match (Phase 1) prunes most candidates before semantic search, so the effective search space remains small. (3) We'd implement runbook deduplication to merge redundant runbooks, keeping the active corpus lean.

**Q4: How do you handle multi-cluster execution?**
A: The step executor resolves the target cluster from the incident context (cluster label in the alert). kubectl commands are executed against the correct cluster's API server. The orchestrator itself runs in a single management cluster.

**Q5: What about runbook execution across time zones (follow-the-sun)?**
A: Human checkpoints are routed to the current on-call engineer (via PagerDuty). If the on-call is in Tokyo at 3am, the checkpoint goes to them. If they don't respond within the configured timeout, it escalates to the next person in the rotation. The system is timezone-aware via PagerDuty integration.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| **Wrong runbook selected** | Incorrect steps executed | Post-execution: incident not resolved | Verification step catches failure; system tries alternate runbook or escalates |
| **Wrong parameter extracted** | Step executed against wrong target | Parameter validation (K8s/CMDB check) | Validation catches most errors; human checkpoint catches rest |
| **Step execution fails** | Runbook halts mid-execution | Step status check | Retry once; if still failing, execute rollback steps and escalate |
| **LLM unavailable** | Cannot select runbook or evaluate steps | Health check; timeout | Fall back to exact trigger match only (deterministic, no LLM). Execute runbook with all human checkpoints enabled. |
| **Human doesn't respond to checkpoint** | Execution blocks indefinitely | Timeout (10 min default) | Auto-escalate after timeout. For low-risk steps, auto-proceed with logging. |
| **Runbook YAML has a bug** | Step fails or acts incorrectly | Schema validation; step output check | Runbooks go through testing before activation. Execution failure updates runbook success metrics. |
| **Concurrent executions on same service** | Conflicting actions | Distributed lock per service | Only one runbook execution per service at a time. Queue additional if needed. |
| **Infrastructure tool (kubectl) timeout** | Step hangs | Step timeout (60s) | Timeout → mark step failed → rollback → escalate |
| **Vector DB down** | Cannot do semantic search | Health check | Fall back to trigger match only (less accurate but functional) |

### AI Safety Controls

1. **Generated runbooks are never auto-activated** — always require human review.
2. **Step execution is reversible** — every write step has a defined rollback.
3. **Kill switch** — global and per-runbook disable.
4. **Success rate monitoring** — if a runbook's success rate drops below 70%, auto-disable and notify author.
5. **Canary execution** — new or updated runbooks execute in "verify-only" mode for 10 executions before enabling write actions.

---

## 10. Observability

### Key Metrics

| Metric | Type | Target | Alert Threshold |
|---|---|---|---|
| `runbook.selection_accuracy` | Gauge (7d) | > 90% | < 80% |
| `runbook.coverage_pct` | Gauge | > 70% of incident types | < 60% |
| `runbook.param_extraction_accuracy` | Gauge | > 95% | < 90% |
| `runbook.execution_success_rate` | Gauge (per runbook, 7d) | > 85% | < 70% |
| `runbook.execution_duration_p50` | Histogram | < 3 min | > 5 min |
| `runbook.execution_duration_p95` | Histogram | < 10 min | > 15 min |
| `runbook.human_checkpoint_wait_p50` | Histogram | < 2 min | > 5 min |
| `runbook.auto_execution_rate` | Gauge | > 50% | < 30% (too cautious) |
| `runbook.rollback_rate` | Gauge | < 5% | > 10% |
| `runbook.escalation_rate` | Gauge | 15-30% | > 40% (coverage gap) |
| `runbook.stale_pct` | Gauge | < 10% (not updated in 6mo) | > 20% |
| `runbook.draft_approval_rate` | Gauge | > 60% | < 40% (generation quality issue) |
| `runbook.llm_cost_per_execution` | Gauge | < $0.15 | > $0.50 |

### Agent Action Audit Trail

```json
{
  "execution_id": "exec-abc-123",
  "incident_id": "inc-xyz-789",
  "runbook_id": "RB-042",
  "runbook_version": 3,
  "timestamp": "2026-04-09T15:10:30Z",
  "selection": {
    "method": "hybrid (trigger_match + semantic + llm_rerank)",
    "candidates_considered": 5,
    "confidence": 0.88,
    "llm_reasoning": "Alert HighMemoryUsage on payment-api matches RB-042 trigger. Service is Kubernetes-based. Runbook steps are appropriate."
  },
  "parameters": {
    "service_name": {"value": "payment-api", "source": "alert_label:service", "validated": true},
    "namespace": {"value": "payments", "source": "alert_label:namespace", "validated": true},
    "memory_threshold_pct": {"value": 90.0, "source": "alert_annotation", "validated": true}
  },
  "steps": [
    {"step": "check_memory", "status": "succeeded", "duration_ms": 1200, "output_summary": "Memory at 94%"},
    {"step": "identify_consumers", "status": "succeeded", "duration_ms": 800, "output_summary": "Top: payment-api-7b9d4 (2.1GB)"},
    {"step": "restart_pods", "status": "succeeded", "duration_ms": 3500, "precondition": "3 healthy replicas (pass)"},
    {"step": "verify_memory", "status": "succeeded", "duration_ms": 122000, "output_summary": "Memory at 62%"}
  ],
  "outcome": "resolved",
  "total_duration_sec": 128,
  "llm_calls": 4,
  "llm_tokens": {"input": 8200, "output": 1800},
  "llm_cost": "$0.04"
}
```

---

## 11. Security

### Principle of Least Privilege

| Component | Permissions |
|---|---|
| Runbook selector | Read-only on runbook corpus, vector DB |
| Parameter filler | Read-only on K8s API, CMDB (for validation) |
| Step executor | Scoped to actions defined in runbook (same RBAC as remediation agent) |
| Runbook generator | Write drafts only (not active runbooks) |
| Human approval gateway | Authenticate via SSO, authorize via team membership |

### Audit Logging

- Every execution logged with full parameter values, step outputs, and LLM reasoning.
- Runbook changes tracked in Git with PR review process.
- Human checkpoint approvals logged with approver identity.
- Retention: 2 years for compliance.

### Human Approval Gates

| Scenario | Approval Required |
|---|---|
| Auto-execute (high confidence, low risk, all steps auto) | None |
| Medium confidence or medium-risk steps | Show plan, auto-proceed after 5 min |
| Low confidence or high-risk steps | Explicit human approval required |
| Generated runbook activation | Author review + peer review |
| Runbook modification to existing | PR review (same as code change) |

---

## 12. Incremental Rollout

### Rollout Phases

| Phase | Duration | Capability |
|---|---|---|
| **Phase 0: Migration** | 6 weeks | Convert top 100 Markdown runbooks to structured YAML. Build embedding index. |
| **Phase 1: Selection only** | 4 weeks | When incident triggers, suggest matching runbook to on-call (advisory). Measure accuracy. |
| **Phase 2: Parameter filling + plan display** | 4 weeks | Show filled runbook plan to on-call. Human executes manually using the plan as guide. |
| **Phase 3: Auto-execute read-only steps** | 4 weeks | System executes diagnostic steps (kubectl get, query metrics). Write steps still manual. |
| **Phase 4: Auto-execute low-risk writes** | 8 weeks | Pod restarts auto-executed. All other writes require approval. |
| **Phase 5: Full execution** | Ongoing | All steps auto-executed per confidence/risk rules. Human checkpoints for high-risk. |

### Rollout Interviewer Q&As

**Q1: Why start with migration rather than LLM generation?**
A: Existing runbooks represent years of operational knowledge. They're battle-tested. LLM-generated runbooks are unproven. By migrating existing runbooks to structured YAML first, we get the most reliable corpus. LLM generation supplements the corpus over time for new scenarios.

**Q2: How do you incentivize teams to convert their runbooks to YAML?**
A: Two carrots. (1) Converted runbooks provide instant diagnosis and plan to on-call engineers — reduces cognitive load at 3am. (2) Execution metrics show which runbooks are most effective — teams that adopt get MTTR improvements. One stick: runbook review becomes part of the post-mortem process; every post-mortem must either update an existing runbook or create a new one.

**Q3: What if the YAML format is too rigid for complex runbooks?**
A: The YAML schema supports conditionals, loops, parallel execution, sub-runbook invocation, and dynamic parameter resolution. It's not a simple sequential list. However, extremely complex runbooks (> 20 steps, deeply nested conditionals) are better decomposed into smaller sub-runbooks that can be chained.

**Q4: How do you measure Phase 1 (selection accuracy) without executing?**
A: When the system suggests a runbook and the on-call engineer sees it, they can rate: "correct match," "partial match," or "wrong." We also track: did the on-call follow the suggested runbook's steps? (Inferred from their actions matching the runbook steps.) Target: > 80% correct/partial match before proceeding.

**Q5: What's the migration effort for 3,000 runbooks?**
A: Not all 3,000 need migration. Analysis: ~500 are actively used (> 1 execution in past 6 months). ~1,000 are stale (> 1 year since last use). ~1,500 are duplicates or deprecated. We migrate the active 500 first. Effort: ~2 hours per runbook for conversion + testing = ~1,000 engineer-hours. Spread across 10 engineers over 6 weeks = manageable. The LLM can assist with conversion (generating YAML from Markdown), reducing effort to ~30 min per runbook.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Chosen | Rationale |
|---|---|---|---|
| Runbook format | Free-text Markdown, structured JSON, structured YAML | YAML | Human-readable, supports comments, widely used for config. Better than JSON for hand-editing. |
| Selection approach | Exact match only, semantic only, hybrid | Hybrid (trigger + semantic + LLM) | Exact for known patterns, semantic for fuzzy, LLM for disambiguation |
| Parameter filling | LLM only, direct mapping only, hybrid | Hybrid (direct first, LLM for gaps, validate all) | Maximize accuracy; validation as safety net |
| Execution model | Execute all at once, step-by-step with verification | Step-by-step with verification | Catch failures early; enable rollback |
| Runbook generation | Manual only, LLM only, LLM draft + human review | LLM draft + human review | Speed of LLM + quality assurance of human |
| Version control | Database only, Git only, both | Git (source of truth) + DB (operational) | Git for review process, DB for fast queries + embeddings |
| A/B testing | No A/B, random split, hash-based | Hash-based (incident ID determines group) | Deterministic, reproducible, fair comparison |

---

## 14. Complete Interviewer Q&A Bank

**Q1: How does this differ from Ansible/Chef/Puppet automation?**
A: Ansible executes pre-defined playbooks — you specify what to do. Our system decides which playbook to run based on the incident context, fills in parameters dynamically, and adapts during execution (conditional branching based on step output). Ansible is the tool executor; our system is the decision layer on top.

**Q2: Why structured YAML instead of just letting the LLM figure out the steps from free-text runbooks?**
A: Three reasons. (1) Safety: structured YAML defines exactly which tools can be called with which parameters. Free-text interpretation might lead to unexpected actions. (2) Validation: we can verify YAML schema, check preconditions, validate parameters. Free-text can't be validated programmatically. (3) Reproducibility: the same YAML produces the same execution every time. Free-text interpretation varies with LLM temperature and context.

**Q3: What if the LLM selects the wrong runbook?**
A: The verification step after execution catches this — the incident won't be resolved. The system then: (1) Tries the second-ranked runbook if confidence was close. (2) Escalates to human with full context. (3) Logs the miss to improve selection accuracy over time. The damage is limited because each step has precondition checks and rollback.

**Q4: How do you handle runbooks that are partially applicable?**
A: The LLM can adapt a partially matching runbook. If the runbook has 8 steps but only 5 apply, the LLM can skip inapplicable steps with reasoning: "Step 4 (check Redis connection) is not applicable because this service doesn't use Redis — skipping." However, more than 3 skipped steps reduces confidence, and the system may escalate instead.

**Q5: Can runbooks call other runbooks (sub-procedures)?**
A: Yes. A step can have `action: invoke_runbook` with `params: {runbook_id: "RB-099"}`. This enables composition: a "full service restart" runbook might invoke "drain traffic," "restart pods," and "verify health" sub-runbooks. Maximum nesting depth: 3 levels.

**Q6: How do you handle long-running steps (e.g., "wait for DNS propagation, up to 1 hour")?**
A: Steps can specify a `timeout` and `poll_interval`. The executor starts the action, then polls for completion: `poll: {query: "dig +short service.example.com", expected: "10.0.1.42", interval: 30, timeout: 3600}`. During the wait, the execution is suspended (not consuming LLM resources).

**Q7: How do you measure the business impact of this system?**
A: Three metrics. (1) MTTR reduction: compare average time-to-resolve for runbook-executed incidents vs. manually resolved. Target: 50% reduction. (2) On-call burden: number of pages that require human intervention (should decrease as auto-execution increases). (3) Runbook coverage: percentage of incident types with a matching, effective runbook (target: > 70%).

**Q8: How do you handle runbook conflicts when two runbooks suggest different actions for the same incident?**
A: The selection engine picks one. If two runbooks have similar confidence (within 0.1), the system presents both to the human: "Two runbooks match. RB-042 suggests pod restart (confidence 0.82). RB-089 suggests config rollback (confidence 0.78). Which would you like to execute?"

**Q9: What's the cost per runbook execution?**
A: LLM costs: 4 calls * ~4K input + 1K output tokens each = ~20K total tokens. At Claude Sonnet pricing: ~$0.08. Infrastructure overhead (compute, storage): ~$0.02. Total: ~$0.10 per execution. At 13,600 executions/day: ~$1,360/day or ~$41K/month. Optimization: cache common selections, use smaller model for parameter extraction.

**Q10: How do you handle the "runbook rot" problem where runbooks become outdated?**
A: Four mechanisms. (1) Success rate monitoring: if a runbook's success rate drops below 70%, alert the author. (2) Staleness detection: if a runbook hasn't been updated in 6 months, flag for review. (3) Post-incident integration: after every incident, check if the executed runbook needs updating. (4) Automated testing: monthly dry-run of all active runbooks in staging to catch broken steps (e.g., renamed Kubernetes resources).

**Q11: Can non-SRE engineers write runbooks?**
A: Yes, with guardrails. Any engineer can write a runbook draft. The draft goes through: (1) Schema validation. (2) Safety review (automated: preconditions on all writes, rollback defined). (3) Peer review (another engineer). (4) Dry-run test in staging. (5) Canary execution (10 runs in verify-only mode). This process is automated and takes ~1 week from draft to active.

**Q12: How do you handle runbooks for non-Kubernetes infrastructure (bare metal, network devices)?**
A: The step executor has adapters for different infrastructure types. `action: ssh_run_command` for bare metal. `action: network_api_call` for network devices. `action: cloud_api` for cloud resources. The runbook YAML is infrastructure-agnostic — it specifies the action type, and the executor resolves the right adapter. Safety rules are adapter-specific (e.g., SSH command allowlist).

**Q13: What if the organization has thousands of alerts that don't map to any runbook?**
A: This is the coverage gap. We address it proactively: (1) Analyze the top 50 most frequent unmatched alerts. (2) For each, use the LLM to generate a runbook draft from available documentation and similar incidents. (3) Prioritize by impact (frequency * severity). (4) Track coverage metric on dashboard — visible accountability for closing gaps.

**Q14: How does A/B testing of runbooks work in practice?**
A: When two versions of a runbook exist (e.g., v3 with pod restart vs. v4 with rolling restart), we create an A/B test. Incoming incidents are hash-routed: even hash → v3, odd hash → v4. We track: success rate, execution time, rollback rate, and human override rate for each version. After 50 executions per version (minimum 2 weeks), we compute statistical significance. If v4 is better (p < 0.05), it becomes the primary version.

**Q15: How do you handle the transition from Markdown runbooks to structured YAML?**
A: The LLM assists. We feed a Markdown runbook to the LLM with the YAML schema and ask it to convert. The output is ~80% correct — an engineer reviews, fixes edge cases, and validates. We've built a conversion tool: `runbook-convert --input runbook.md --output runbook.yaml --review-mode`. This reduces conversion time from 2 hours to 30 minutes per runbook.

**Q16: What happens when a new type of incident occurs that has no runbook?**
A: The system escalates immediately with the message: "No matching runbook found. Here are the 3 closest matches and why they don't apply." After the incident is resolved, the post-mortem triggers runbook generation. The system also tracks "runbook miss" events and aggregates them: "This alert type has triggered 15 times in the past month with no matching runbook — high priority for runbook creation."

---

## 15. References

1. **Google SRE Book** — "Managing Incidents" and "Postmortem Culture": https://sre.google/sre-book/
2. **Ansible Documentation**: https://docs.ansible.com/
3. **Yao, S. et al.** — "ReAct: Synergizing Reasoning and Acting in Language Models" (2023): https://arxiv.org/abs/2210.03629
4. **Anthropic** — "Tool use with Claude": https://docs.anthropic.com/en/docs/build-with-claude/tool-use
5. **PagerDuty Runbook Automation**: https://www.pagerduty.com/platform/automation/runbook-automation/
6. **pgvector** — Vector similarity search for PostgreSQL: https://github.com/pgvector/pgvector
7. **YAML specification**: https://yaml.org/spec/1.2.2/
8. **Kubernetes API documentation**: https://kubernetes.io/docs/reference/kubernetes-api/
9. **Lewis, P. et al.** — "Retrieval-Augmented Generation for Knowledge-Intensive NLP Tasks" (2020): https://arxiv.org/abs/2005.11401
10. **Shoreline.io** — "Op Packs" (commercial runbook automation): https://www.shoreline.io/
