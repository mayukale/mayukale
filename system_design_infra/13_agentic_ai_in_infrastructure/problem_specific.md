# Problem-Specific Details — Agentic AI in Infrastructure (13_agentic_ai_in_infrastructure)

---

## agentic_auto_remediation

### Unique Stack
- ReAct orchestrator: max 8 iterations, 15-minute timeout per session
- Kafka remediation queue with priority (critical > warning > info)
- kubectl executor, SSH runner (allowlisted commands, 60s max), cloud API executor (AWS/GCP: EC2, ELB, ASG, RDS, Route53, ElastiCache)
- Rollback manager: undo stack per session; every action records its inverse operation
- Blast radius estimator before every destructive action
- 200+ remediation patterns indexed in retrieval store

### Key Algorithms / Design Decisions

**ReAct Loop:**
```python
async def run(max_iterations=8, timeout=900):
    for i in range(max_iterations):
        # REASON
        thought = await llm_reason(history, plan)

        # SAFETY CHECK (preconditions)
        safety = await safety_gate.check(thought.action, thought.params)
        if not safety.safe:
            history.append(('blocked', thought.action, safety.reason))
            continue

        # RATE LIMIT
        if not rate_limiter.allow(service, action_type):
            escalate("Rate limit exceeded")

        # BLAST RADIUS
        blast = await check_blast_radius(thought.action)
        if blast.users_affected_pct > 20:
            escalate(f"Blast radius {blast.users_affected_pct}%")

        # ACT
        result = await execute_tool(thought.action, thought.params)
        rollback_stack.append(thought.action.inverse())

        # VERIFY
        verification = await verify(thought.expected_outcome)
        history.append((thought.reasoning, thought.action, verification))

        if verification.resolved:
            succeed()

    escalate("Max iterations reached")
```

**Blast Radius Calculation:**
```
users_affected_pct    = (affected_pods / total_pods) × (service_users / total_users) × 100
requests_affected_pct = (affected_capacity / total_capacity) × 100
```

### Key Tables
```sql
CREATE TABLE remediation_sessions (
    session_id UUID PRIMARY KEY,
    incident_id UUID NOT NULL,
    status VARCHAR(20),  -- planning, executing, verifying, succeeded, failed, escalated, rolled_back
    diagnosis JSONB,
    confidence DECIMAL(3,2),
    attempted_actions INT,
    rollback_stack JSONB,   -- [{action, inverse_params}, ...]
    created_at TIMESTAMPTZ,
    completed_at TIMESTAMPTZ
);

CREATE TABLE tool_calls (
    call_id UUID PRIMARY KEY,
    session_id UUID REFERENCES remediation_sessions,
    tool_name VARCHAR(100),
    parameters JSONB,
    precondition_result JSONB,
    output JSONB,
    blast_radius_users DECIMAL,    -- % of users affected
    blast_radius_requests DECIMAL, -- % of requests
    duration_ms INT,
    status VARCHAR(20),  -- success, blocked, failed, timeout
    created_at TIMESTAMPTZ
);

CREATE TABLE escalations (
    escalation_id UUID PRIMARY KEY,
    session_id UUID REFERENCES remediation_sessions,
    reason VARCHAR(200),  -- max_iterations, timeout, rate_limit, blast_radius_too_large
    diagnosis JSONB,
    actions_taken JSONB,              -- [{step, action, result}, ...]
    recommended_human_actions TEXT[],
    created_at TIMESTAMPTZ
);
```

### NFRs
- 12,000 incidents/day requiring remediation (40K total × 30%)
- Auto-remediated: 7,200/day (60%); human-approved: 3,000/day (25%); escalated: 1,800/day (15%)
- Agent tool calls/day: 40,800 (avg 4 steps); LLM reasoning calls/day: 30,600 (avg 3 calls)
- Peak concurrent remediations: 10
- Time incident → first action: < 60 seconds; end-to-end common case: < 5 minutes
- Remediation success rate: > 90%; false remediation rate: < 0.5%
- Rollback success rate: > 99%
- Storage (1 year): 150 GB

---

## ai_ops_platform

### Unique Stack
- Alert ingestion gateway: normalizes heterogeneous sources (Prometheus, CloudWatch, Datadog, custom) → unified schema → Kafka topic `raw-alerts`
- Flink/Kafka Streams: tumbling 30s windows for alert dedup; session gap 5 min for incident grouping
- AI triage orchestrator: OBSERVE → REASON → ACT → VERIFY; max 5 iterations
- RAG engine: pgvector (1536 dims, ivfflat) + Elasticsearch (BM25) + RRF fusion
- Runbook corpus: 3,000 runbooks (920 MB embeddings); past incident corpus: 15,000 post-mortems (1.8 GB embeddings)
- PagerDuty bidirectional integration; feedback loop captures human corrections

### Key Algorithms / Design Decisions

**Confidence Scoring (Ensemble + Platt Scaling):**
```
final_confidence = (0.3 × llm_confidence)
                 + (0.3 × retrieval_score)       -- avg similarity of top-3 RAG results
                 + (0.2 × metric_clarity)         -- how anomalous metrics are (>3σ = high)
                 + (0.2 × historical_accuracy_for_service)

Apply Platt scaling calibration trained on 90 days of (predicted_conf, actual_correct) pairs
```

**Routing Logic:**
```
if final_confidence >= 0.90 AND action_risk == 'low':  auto_remediate()
elif final_confidence >= 0.75:                          notify_human(auto_execute_after=5min)
elif final_confidence >= 0.50:                          escalate_with_diagnosis()
else:                                                    escalate_urgent()
```

**Alert Correlation (Dependency Graph + Flink):**
1. Maintain service dependency graph (auto-discovered from Istio/Envoy mesh)
2. Alert fires → look up service in graph → check upstream services for alerts within 5-min window
3. Flink tumbling 30s windows for dedup; session gap 5 min groups related alerts into incidents
4. "Root service" = deepest in dependency chain with alert; `root_service` column on incidents table

**RAG Retrieval:**
```
1. Query = alert_names + service_names + error_messages
2. Vector search (pgvector, top-20): text-embedding-3-large, cosine similarity
3. BM25 search (Elasticsearch, top-20): keyword match in parallel
4. RRF fusion: score(d) = Σ(1 / (k + rank(d))), k=60
5. Optional cross-encoder re-ranking on top-20
6. Return top 5-8 chunks to LLM prompt
7. Chunks: 500 tokens with 100-token overlap; preserve section headings as metadata
```

**Context Window Budget (128K):**
```
System prompt:                500 tokens
Incident context:           2,000 tokens (summarize if > 20 alerts)
Current metrics snapshot:   1,500 tokens
Recent changes/deploys:       500 tokens
Relevant runbooks (top 3):  2,000 tokens
Similar past incidents (3): 2,000 tokens
LLM instructions:             500 tokens
```

### Key Tables
```sql
CREATE TABLE alerts (
    alert_id UUID PRIMARY KEY,
    source VARCHAR(50),       -- prometheus, cloudwatch, datadog, custom
    severity VARCHAR(10),     -- critical, warning, info
    service_name VARCHAR(255),
    region VARCHAR(50),
    alert_name VARCHAR(255),
    summary TEXT,
    labels JSONB,
    fingerprint VARCHAR(64),  -- for dedup
    fired_at TIMESTAMPTZ,
    resolved_at TIMESTAMPTZ,
    incident_id UUID REFERENCES incidents(incident_id)
);

CREATE TABLE incidents (
    incident_id UUID PRIMARY KEY,
    status VARCHAR(20),   -- open, investigating, remediated, false_positive
    alert_ids UUID[],
    root_service VARCHAR(255),        -- deepest in dependency chain
    affected_services VARCHAR(255)[],
    diagnosis TEXT,
    confidence DECIMAL(3,2),
    retrieval_confidence DECIMAL(3,2),  -- avg similarity of top-3 RAG results
    metric_clarity DECIMAL(3,2),        -- anomaly score (>3σ = higher)
    rag_results JSONB,          -- [{runbook_id, score}, {incident_id, score}]
    actions_suggested JSONB,
    action_risk_level VARCHAR(20),    -- low, medium, high
    routing_decision VARCHAR(20),     -- auto_remediate, human_approval, escalate_urgent
    created_at TIMESTAMPTZ,
    resolved_at TIMESTAMPTZ
);

CREATE TABLE rag_embeddings (
    chunk_id UUID PRIMARY KEY,
    source_type VARCHAR(20),   -- runbook, incident_postmortem
    source_id VARCHAR(255),
    chunk_number INT,
    chunk_text TEXT,
    embedding vector(1536),    -- text-embedding-3-large
    metadata JSONB
);

CREATE INDEX idx_rag_embeddings ON rag_embeddings
    USING ivfflat (embedding vector_cosine_ops) WITH (lists=100);
```

### NFRs
- 50,000 alerts/minute sustained; 2,000,000 alerts/day; 23 alerts/sec avg; 230 alerts/sec peak
- 40,000 correlated incidents/day; 120,000 LLM calls/day (avg 3 per incident)
- Alert ingestion → diagnosis: < 30s p95
- Diagnosis accuracy (top-3 includes actual cause): > 85%
- False auto-remediation rate: < 0.1%
- Availability: 99.95%; LLM spend: < $50K/month
- Storage (1 year): 2 TB (alerts raw: 1.5 TB, incidents: 146 GB, embeddings: 2.7 GB)

---

## infrastructure_copilot

### Unique Stack
- Three interfaces: CLI (`infra ask "..."`), Slack bot (`@infra-bot`), web chat (portal.internal/copilot)
- Session state in Redis per conversation (30-min TTL); structured intent state + last 10 messages
- RBAC engine: validates user permissions against Okta/Azure AD before any action
- Policy engine: org rules enforcement (max VMs per request, budget constraints, region restrictions)
- Confirmation step mandatory for all state-changing operations
- Tools: Kubernetes API, cloud APIs (EC2/GCE/ASG), Terraform Cloud, CMDB, cost engine, capacity planner, DNS, load balancer, cert management

### Key Algorithms / Design Decisions

**Intent Parsing + Validation:**
```python
SYSTEM_PROMPT = """You are an infrastructure management assistant.
Parse user requests into structured intents.
User context: name={user_name}, team={user_team}, role={user_role},
allowed_regions={allowed_regions}, max_instances={max_instances}"""

response = await llm.chat(messages=[system_prompt, history, message], tools=tool_defs)

# Validate all entities against known enums (regions, instance types, services)
for entity in response.parsed_intent.entities:
    validate(entity)   # raises ParameterValidationError if invalid
```

**Multi-Turn Context Management:**
```python
MAX_RECENT_MESSAGES = 10
MAX_CONTEXT_TOKENS = 8000

async def build_context(conversation_id):
    session = await redis.get(f"session:{conversation_id}")

    # Always included: structured state
    state = {
        "user": session.user_context,
        "current_intent": session.pending_intent,
        "pending_confirmation": session.pending_action,
        "actions_this_session": session.completed_actions,
        "clarifications_asked": session.clarification_history,
    }

    # Recent messages (last 10 or 8K tokens, whichever is less)
    recent = await db.get_recent_messages(conversation_id, limit=MAX_RECENT_MESSAGES)

    # If > 10 messages: use pre-computed summary for older context
    if session.message_count > MAX_RECENT_MESSAGES:
        summary = session.conversation_summary

    return format_context(state, summary, recent)
```

**State Merging (Multi-Turn Intent Update):**
```python
# New entities override old only if user explicitly changed them
session.pending_intent = merge_intent(session.pending_intent, llm_response.parsed_intent)
# "Actually make it 6 instead of 4" → updates count, keeps other fields
if session.message_count > MAX_RECENT_MESSAGES:
    session.conversation_summary = await summarize(conversation_id)
session.last_activity = datetime.utcnow()
await redis.set(f"session:{conversation_id}", session, ttl=1800)
```

### Key Tables
```sql
CREATE TABLE conversations (
    conversation_id UUID PRIMARY KEY,
    user_id VARCHAR(100),
    user_email VARCHAR(255),
    team VARCHAR(100),
    interface VARCHAR(20),   -- cli, slack, web
    status VARCHAR(20) DEFAULT 'active',
    started_at TIMESTAMPTZ,
    last_activity_at TIMESTAMPTZ,
    message_count INT DEFAULT 0,
    session_summary TEXT
);

CREATE TABLE conversation_messages (
    message_id UUID PRIMARY KEY,
    conversation_id UUID REFERENCES conversations,
    role VARCHAR(20),            -- user, copilot
    content TEXT,
    parsed_intent JSONB,
    tool_calls JSONB,            -- [{tool, params, result}, ...]
    created_at TIMESTAMPTZ
);

CREATE TABLE parsed_intents (
    intent_id UUID PRIMARY KEY,
    conversation_id UUID REFERENCES conversations,
    intent_type VARCHAR(50),     -- provision, scale, query, reserve, decommission
    resource_type VARCHAR(100),  -- compute, gpu, storage, network
    instance_type VARCHAR(100),
    count INT,
    region VARCHAR(50),
    duration VARCHAR(50),        -- "14d", "2w", "permanent"
    team VARCHAR(100),
    purpose TEXT,
    service_name VARCHAR(255),
    namespace VARCHAR(100),
    extracted_at TIMESTAMPTZ,
    confidence DECIMAL(3,2)
);

CREATE TABLE action_audit_log (
    action_id UUID PRIMARY KEY,
    conversation_id UUID REFERENCES conversations,
    user_id VARCHAR(100),
    action_type VARCHAR(50),
    target_resource VARCHAR(255),
    parameters JSONB,
    rbac_decision VARCHAR(20),    -- approved, denied
    rbac_reason TEXT,
    policy_decision VARCHAR(20),
    policy_reason TEXT,
    executed BOOLEAN,
    result JSONB,
    created_at TIMESTAMPTZ
);

CREATE TABLE user_context (
    user_id VARCHAR(100) PRIMARY KEY,
    team VARCHAR(100),
    role VARCHAR(100),
    allowed_regions VARCHAR(50)[],
    max_instances INT,
    max_storage_gb INT,
    monthly_budget DECIMAL(10,2),
    timezone VARCHAR(50)
);
```

### NFRs
- 2,000 registered users; 200 DAU (10%); 3–5 requests/user/day = 800 requests/day
- Peak concurrent conversations: 30; conversation context retention: 30 min
- LLM calls/day: 2,400 avg; 4,000 peak (3–5 calls per request)
- Intent parsing accuracy: > 92%; response time (simple query): < 5s; action plan shown: < 10s
- Action execution time: < 2 min for standard ops
- Availability: 99.9%; storage (1 year): 4 GB

---

## intelligent_capacity_planner

### Unique Stack
- Time-series forecasting: Prophet/ARIMA per resource; handles seasonality and changepoints
- Growth model: logistic/Bass curve per customer; models market saturation
- Scenario generator: 4 scenarios (optimistic ×0.85, base ×1.0, pessimistic ×1.25, stress ×1.50)
- Cost optimizer: mixed-integer programming (spot vs RI vs on-demand tradeoffs)
- Procurement lead-time model: P50 and P90 weeks per SKU per vendor
- LLM report generator: translates forecast + recommendations into executive narrative
- ETL pipeline: Airflow/Dagster, hourly pull from Prometheus, CloudWatch, CMDB, finance system, sales/product roadmap

### Key Algorithms / Design Decisions

**Composite Demand Forecasting:**
```python
def forecast(resource_type, region, horizon_weeks):
    # 1. Time-series baseline (Prophet)
    ts_forecast = prophet_model.predict(
        history=365*24,        # 1 year of hourly history
        horizon=horizon_weeks*7*24  # hourly forecasts
    )

    # 2. Growth multiplier from business signals
    signals = get_business_signals(resource_type, region)
    multiplier = 1.0
    for signal in signals:
        impact = (signal.impact_multiplier - 1.0) * signal.confidence
        multiplier += impact

    # 3. 4 scenarios
    base = ts_forecast * multiplier
    return {
        'optimistic': base * 0.85,
        'base':       base * 1.00,
        'pessimistic':base * 1.25,
        'stress':     base * 1.50,
    }
```

**Capacity Gap Calculation:**
```
gap = forecasted_demand - (current_capacity + committed_additions - planned_decommissions)

if gap > 0:
    capacity_shortfall → trigger procurement recommendation
if gap < -0.30 × current_capacity:
    over_provisioned → consider decommissioning
```

**Business Signals Examples:**
```
Customer growth:  impact_multiplier=1.12, confidence=0.95  (signed contract)
Product launch:   impact_multiplier=1.0+(200/current_gpu),  confidence=0.90
Seasonal (BF):    impact_multiplier=3.00, confidence=1.00   (historical)
Contract churn:   impact_multiplier=0.95, confidence=0.40   (pipeline guess)
```

**Mixed-Integer Programming for Cost Optimization:**
```python
# Decision variables
x_purchase[sku][region]  = IntegerVar(lb=0)
x_ri_1yr[sku][region]    = IntegerVar(lb=0)
x_ri_3yr[sku][region]    = IntegerVar(lb=0)
x_on_demand[sku][region][week] = ContinuousVar(lb=0)
x_spot[sku][region][week]      = ContinuousVar(lb=0)

# Objective: minimize total cost over 12 months
minimize(total_cost(all_vars))

# Constraints:
# Supply >= Demand for each resource/region/week
# Total cost <= annual_budget
# Spot <= 30% of total supply (reliability)
# On-prem >= 40% of total supply (compliance)
```

### Key Tables
```sql
CREATE TABLE utilization_history (
    resource_id VARCHAR(255),
    resource_type VARCHAR(50),
    region VARCHAR(50),
    timestamp TIMESTAMPTZ,
    cpu_percent DECIMAL(5,2),
    memory_percent DECIMAL(5,2),
    disk_percent DECIMAL(5,2),
    gpu_percent DECIMAL(5,2),
    network_io_gbps DECIMAL(10,2),
    PRIMARY KEY (resource_id, timestamp)
);

CREATE TABLE procurement_lead_times (
    sku VARCHAR(100) PRIMARY KEY,
    vendor VARCHAR(100),
    p50_weeks INT,
    p90_weeks INT,
    last_updated TIMESTAMPTZ
);

CREATE TABLE demand_forecasts (
    forecast_id UUID PRIMARY KEY,
    resource_type VARCHAR(50),
    region VARCHAR(50),
    week_number INT,
    scenario VARCHAR(20),   -- optimistic, base, pessimistic, stress
    forecasted_demand DECIMAL(15,2),
    confidence_interval JSONB,  -- {p10, p50, p90}
    generated_at TIMESTAMPTZ
);

CREATE TABLE business_signals (
    signal_id UUID PRIMARY KEY,
    signal_type VARCHAR(50),   -- customer_growth, product_launch, seasonal, contract_renewal, hardware_eol
    source VARCHAR(100),
    affected_resources VARCHAR(50)[],
    impact_multiplier DECIMAL(5,2),
    confidence DECIMAL(3,2),
    effective_date DATE,
    created_at TIMESTAMPTZ
);

CREATE TABLE procurement_recommendations (
    recommendation_id UUID PRIMARY KEY,
    sku VARCHAR(100),
    quantity INT,
    region VARCHAR(50),
    scenario VARCHAR(20),
    order_by_date DATE,
    estimated_delivery DATE,
    estimated_cost DECIMAL(15,2),
    urgency VARCHAR(20),   -- immediate, 1week, 2weeks, standard
    reason TEXT,
    created_at TIMESTAMPTZ
);
```

### NFRs
- 60,000 resources (10K servers + 50K cloud instances); 600,000 time-series (10 metrics each)
- 240,000 forecasts per planning cycle (60K × 4 scenarios)
- Forecast accuracy: MAPE < 10% (2-week horizon); MAPE < 25% (12-week horizon)
- Planning cycle: weekly batch + on-demand; recommendation generation: < 5 min full portfolio
- LLM report generation: < 30s; data freshness: < 1 hour
- Storage (total): 125 GB; 50 concurrent dashboard users

---

## llm_assisted_runbook_executor

### Unique Stack
- Runbook store: PostgreSQL + Git; YAML definitions with steps, conditionals, rollback flags, human checkpoints
- pgvector: 3,000 runbooks × 10 chunks avg = 30,000 embeddings; `ivfflat` with lists=50
- A/B testing framework: version_a vs version_b with configurable traffic_split; tracks success_rate per version
- LLM-generated runbook drafts from post-mortems → human review → approved corpus
- Parameter validation against CMDB and Kubernetes API before execution

### Key Algorithms / Design Decisions

**Hybrid Runbook Selection (3 Phases):**
```python
async def select(incident):
    # Phase 1: Exact trigger match (fast, O(1))
    exact = trigger_match(incident)  # matches alert_name in runbook.trigger_patterns
    if len(exact) == 1:
        return exact[0], confidence=0.95

    # Phase 2: Semantic search (broader, top-10)
    query = f"{incident.alert_name} {incident.service} {incident.diagnosis}"
    semantic = await vector_search(query, top_k=10)

    # Phase 3: Merge + LLM re-rank
    candidates = merge_candidates(exact, semantic)[:5]
    reranked = await llm_rerank(incident, candidates)

    best = reranked[0]
    if best.confidence < 0.6:
        return None    # no match → escalate

    return best.runbook, best.confidence
```

**Parameter Extraction + Validation:**
```python
# LLM fills runbook parameter templates from incident context
response = await llm.chat(prompt=f"""
Given incident: {incident.alert_name}, {incident.service}, {incident.diagnosis}
Fill parameters for step: {step.parameters_template}
""")

for param_name, param_value in extracted:
    result = await validate_against_cmdb_or_k8s(param_name, param_value)
    if not result.valid:
        raise ParameterValidationError(f"{param_name}: {result.reason}")
```

**Step Execution with Conditional Branching:**
```python
for step in runbook.steps:
    # Precondition check
    if not await check_preconditions(step.preconditions):
        log_and_escalate("Precondition failed")

    output = await execute_tool(step.action, filled_params)

    # LLM evaluates: did the step succeed?
    llm_eval = await llm.evaluate(expected=step.expected_condition, actual=output)

    if llm_eval.success:
        next_step = step.conditional_next.get(output.status, step.step_number + 1)
    else:
        # Execute rollback steps
        for rb in [s for s in runbook.steps if s.is_rollback]:
            await execute_tool(rb.action, params)

    if step.human_checkpoint:
        await request_approval(step, output)  # pause for Slack/PagerDuty
```

### Key Tables
```sql
CREATE TABLE runbooks (
    runbook_id VARCHAR(50) PRIMARY KEY,   -- e.g., RB-042
    version INT,
    title VARCHAR(255),
    trigger_patterns VARCHAR(255)[],
    tags VARCHAR(100)[],
    yaml_content TEXT,
    status VARCHAR(20),   -- draft, active, deprecated, archived
    embedding vector(1536),
    success_rate DECIMAL(5,2),
    execution_count INT,
    average_duration_min DECIMAL(5,2)
);

CREATE INDEX idx_runbooks_embedding ON runbooks
    USING ivfflat (embedding vector_cosine_ops) WITH (lists=50);

CREATE TABLE runbook_steps (
    step_id VARCHAR(50) PRIMARY KEY,   -- e.g., RB-042-step-3
    runbook_id VARCHAR(50) REFERENCES runbooks,
    step_number INT,
    action VARCHAR(100),
    parameters JSONB,          -- template with ${namespace}, ${pod_name} placeholders
    preconditions JSONB,
    expected_condition TEXT,
    timeout_seconds INT,
    is_rollback BOOLEAN DEFAULT false,
    depends_on INT[],
    conditional_next JSONB,    -- {output_status: next_step_number}
    human_checkpoint BOOLEAN DEFAULT false
);

CREATE TABLE runbook_executions (
    execution_id UUID PRIMARY KEY,
    incident_id UUID,
    runbook_id VARCHAR(50) REFERENCES runbooks,
    version INT,
    status VARCHAR(20),   -- planning, executing, verifying, succeeded, failed, escalated, rolled_back
    started_at TIMESTAMPTZ,
    completed_at TIMESTAMPTZ,
    selected_confidence DECIMAL(3,2),
    parameters_filled JSONB,
    success BOOLEAN,
    rollback_executed BOOLEAN
);

CREATE TABLE step_executions (
    execution_id UUID REFERENCES runbook_executions,
    step_number INT,
    output JSONB,
    llm_interpretation TEXT,
    precondition_result JSONB,
    human_approved_by VARCHAR(100),
    started_at TIMESTAMPTZ,
    completed_at TIMESTAMPTZ,
    duration_ms INT,
    PRIMARY KEY (execution_id, step_number)
);

CREATE TABLE runbook_ab_tests (
    test_id UUID PRIMARY KEY,
    runbook_id VARCHAR(50),
    version_a INT,
    version_b INT,
    traffic_split FLOAT DEFAULT 0.5,
    status VARCHAR(20) DEFAULT 'active',
    results JSONB   -- {version_a: {success_rate, avg_time}, version_b: {...}}
);
```

### NFRs
- 3,000-runbook corpus; runbook coverage: > 70% of incidents have a matching runbook
- 16,000 incidents/day with runbook execution; 8,000 auto-executed (50%), 5,600 human-approved (35%)
- Avg 6 steps/runbook; 81,600 step executions/day; 54,400 LLM calls/day
- Runbook selection accuracy: > 90%; parameter extraction accuracy: > 95%
- Execution start time: < 30s from trigger; step execution reliability: > 99%
- Availability: 99.9%; storage (1 year): 150 GB

---

## predictive_autoscaling_system

### Unique Stack
- Per-service model selection: walk-forward CV on Prophet, ARIMA, LSTM, XGBoost, or inverse-MAPE weighted ensemble
- 14+ features per service: time-based (hour, day_of_week, is_weekend, month) + lag features (5m/15m/30m/1h/1d/7d) + rolling stats (avg_15m, avg_1h, std_1h, trend_1h) + event features (is_event, event_multiplier, recent_deployment)
- Feature store: Redis (hot, 24h) + S3 (cold archive)
- Two-layer scaling: predictive sets `minReplicas` floor; reactive HPA manages above floor
- Asymmetric cooldowns: 3 min scale-up, 10 min scale-down; 20% max decrease per cycle
- Model retraining: weekly scheduled + triggered when rolling 24h MAPE > 25% for 6 hours
- 3-phase model validation: offline CV → 48h shadow → 24h canary at 10% traffic

### Key Algorithms / Design Decisions

**Per-Service Model Selection:**
```python
for model in ['prophet', 'arima', 'lstm', 'xgboost']:
    cv_scores = walk_forward_cv(model, training_data, folds=5)
    results[model] = {'mape': np.mean(cv_scores['mape'])}

best = min(results, key=lambda m: results[m]['mape'])

# If best MAPE > 20%, use ensemble of top-3
if results[best]['mape'] > 0.20:
    top3 = sorted(results, key=lambda m: results[m]['mape'])[:3]
    # Inverse-MAPE weighting
    weights = {m: (1/results[m]['mape']) / sum(1/results[t]['mape'] for t in top3)
               for m in top3}
    return 'ensemble', weights

return best, results[best]
```

**Scaling Decision Logic (5-min cycles):**
```
1. Predict demand at +15 min:
   predicted_demand = model.predict(features) → 850 rps

2. Calculate required replicas:
   required = ceil(predicted_demand / rps_per_pod / target_utilization)
            = ceil(850 / 100 / 0.7) = 13 replicas

3. Safety margin (10%):
   target_min = ceil(13 × 1.10) = 15 replicas

4. Clamp to bounds:
   target_min = max(config.min, min(target_min, config.max))

5. Update HPA minReplicas = 15
   HPA continues to manage actual replicas:
     - If cpu > 70%: scale up beyond 15
     - If cpu < 70%: scale down to 15 (never below floor)

6. Cooldown enforcement:
   Scale-up: no decrease for 3 min after last increase
   Scale-down: max 20% decrease per cycle; wait 10 min between decreases
```

**Feature Vector (computed every 5 min per service):**
```
Time-based: hour_of_day[0-23], day_of_week[0-6], is_weekend, month[1-12]
Lag:        lag_5m, lag_15m, lag_30m, lag_1h, lag_1d, lag_7d
Rolling:    rolling_avg_15m, rolling_avg_1h, rolling_std_1h, trend_1h (linear slope)
Events:     is_event, event_multiplier, recent_deployment (deploy within last 2h)
```

### Key Tables
```sql
CREATE TABLE time_series_metrics (
    service_id VARCHAR(255),
    metric_name VARCHAR(100),   -- cpu_usage, memory_usage, request_rate, error_rate, queue_depth
    timestamp TIMESTAMPTZ,
    value DECIMAL(10,2),
    PRIMARY KEY (service_id, metric_name, timestamp)
);

CREATE TABLE feature_vectors (
    service_id VARCHAR(255),
    timestamp TIMESTAMPTZ,
    hour_of_day INT, day_of_week INT, is_weekend BOOLEAN, month INT,
    lag_5m DECIMAL(10,2), lag_15m DECIMAL(10,2), lag_30m DECIMAL(10,2),
    lag_1h DECIMAL(10,2), lag_1d DECIMAL(10,2), lag_7d DECIMAL(10,2),
    rolling_avg_15m DECIMAL(10,2), rolling_avg_1h DECIMAL(10,2),
    rolling_std_1h DECIMAL(10,2), trend_1h DECIMAL(10,2),
    is_event BOOLEAN, event_multiplier DECIMAL(5,2),
    recent_deployment BOOLEAN,
    PRIMARY KEY (service_id, timestamp)
);

CREATE TABLE model_registry (
    service_id VARCHAR(255) PRIMARY KEY,
    primary_model VARCHAR(50),       -- prophet, arima, lstm, xgboost, ensemble
    alternative_models VARCHAR(50)[],
    model_version INT,
    model_path VARCHAR(255),         -- S3 artifact path
    training_data_size INT,          -- days of history
    last_training_date TIMESTAMPTZ,
    training_mape DECIMAL(5,2),
    validation_mape DECIMAL(5,2),
    production_mape_24h DECIMAL(5,2) -- rolling 24h accuracy in prod
);

CREATE TABLE predictions (
    prediction_id UUID PRIMARY KEY,
    service_id VARCHAR(255),
    prediction_timestamp TIMESTAMPTZ,
    horizon_minutes INT,         -- 15, 30, 60
    predicted_cpu DECIMAL(5,2),
    predicted_memory DECIMAL(5,2),
    predicted_request_rate DECIMAL(10,2),
    predicted_replicas INT,
    safety_margin_pct DECIMAL(5,2),
    target_replicas INT,
    confidence_interval JSONB    -- {p10, p50, p90}
);

CREATE TABLE scaling_actions (
    action_id UUID PRIMARY KEY,
    service_id VARCHAR(255),
    action_type VARCHAR(20),     -- scale_up, scale_down, no_change
    from_replicas INT,
    to_replicas INT,
    reason VARCHAR(50),          -- predicted_demand, hpa_triggered, event_scheduled
    triggered_by VARCHAR(50),    -- predictor, hpa, manual
    executed BOOLEAN,
    result JSONB
);

CREATE TABLE scheduled_events (
    event_id UUID PRIMARY KEY,
    event_name VARCHAR(255),
    affected_services VARCHAR(255)[],
    traffic_multiplier DECIMAL(5,2),
    event_start TIMESTAMPTZ,
    event_end TIMESTAMPTZ
);
```

### NFRs
- 500 services; 5 metrics each = 2,500 time-series; 14.4M data points/day (15s scrape)
- 288 prediction cycles/day (every 5 min); 144,000 predictions/day; 1,500 scaling actions/day
- Forecast MAPE: < 15% (15-min horizon); < 25% (60-min horizon)
- Prediction latency: < 5s per service; cost savings: > 20% vs over-provisioning
- False scale-down rate: < 1%; coverage: > 80% services with valid models
- Retraining trigger: rolling 24h MAPE > 25% for 6 hours
- Model validation: offline CV → 48h shadow → 24h canary (10% traffic) before promotion
- Availability: 99.9%; storage (total): 193 GB
