# Common Patterns — Agentic AI in Infrastructure (13_agentic_ai_in_infrastructure)

## Common Components

### LLM Integration (All 6 Systems)
- All 6 systems use Claude Sonnet/Opus via API or self-hosted vLLM for chain-of-thought reasoning and function/tool calling
- LLM outputs structured JSON for tool selection, parameter filling, and confidence scores
- Context window budgets managed explicitly: system prompt, incident/user context, retrieved docs, tool results
- LLM self-reported confidence is never used alone — always combined with other signals (retrieval quality, metric clarity, validation results)

### OBSERVE → REASON → ACT → VERIFY Loop (5 of 6 Systems)
- agentic_auto_remediation, ai_ops_platform, infrastructure_copilot, llm_assisted_runbook_executor, predictive_autoscaling all follow this pattern
- **OBSERVE**: gather current state (metrics, alerts, infrastructure inventory, conversation history)
- **REASON**: LLM generates next action with justification
- **ACT**: execute tool call (kubectl, SSH, cloud API, Terraform) or ask clarification
- **VERIFY**: check whether outcome matches expected; update state
- Loop until resolved, max iterations reached, or escalation triggered
- Max iterations: 5–8 per incident; timeout: 10–15 minutes per session

### Human Approval Gates (4 of 6 Systems)
- agentic_auto_remediation, ai_ops_platform, infrastructure_copilot, llm_assisted_runbook_executor all pause for human approval on high-risk or low-confidence actions
- Approval delivered via Slack reactions or PagerDuty acknowledgment
- Timeout behavior: auto-execute after configurable timeout (if risk is low) OR escalate (if risk is high)
- Show plan before execution: resource type, estimated impact, blast radius, cost estimate

### PostgreSQL as Primary OLTP Store (All 6 Systems)
- All 6 use PostgreSQL for authoritative state: incident/session records, audit logs, execution history, model metadata, runbook definitions
- Common field patterns: `UUID PRIMARY KEY`, `JSONB` for flexible metadata, `TIMESTAMPTZ` for all timestamps, `DECIMAL(3,2)` for confidence scores (0.00–1.00), `VARCHAR[]` for tags/labels
- pgvector extension: 2 of 6 (ai_ops_platform, llm_assisted_runbook_executor) store embeddings directly in PostgreSQL; `vector(1536)` columns with `ivfflat` indexes (`vector_cosine_ops`)

### Kafka Event Queue (3 of 6 Systems)
- agentic_auto_remediation: Kafka remediation queue with priority (critical > warning)
- ai_ops_platform: Kafka topic `raw-alerts` for 50K alerts/min ingestion; Flink/Kafka Streams for correlation
- llm_assisted_runbook_executor: Kafka runbook execution queue; messages include `{incident_id, diagnosis, service, region, severity}`

### Confidence Thresholding and Routing (4 of 6 Systems)
- ai_ops_platform, infrastructure_copilot, llm_assisted_runbook_executor, predictive_autoscaling all route decisions based on confidence tiers
- High confidence + low risk → auto-execute
- Medium confidence → notify human with plan; auto-execute after timeout
- Low confidence → escalate with full context
- Confidence is always an ensemble, never a single LLM self-report

### Immutable Audit Trail (5 of 6 Systems)
- agentic_auto_remediation, ai_ops_platform, infrastructure_copilot, llm_assisted_runbook_executor, predictive_autoscaling all log every decision, tool call, and LLM reasoning step
- Append-only tables; no UPDATE/DELETE on audit records
- 100% audit completeness requirement across all systems
- Fields always captured: actor/source, action, parameters, result, timestamp, confidence, reasoning_context

### Feedback Loops and Continuous Learning (4 of 6 Systems)
- ai_ops_platform: human corrections → retrieval corpus + fine-tuning data
- intelligent_capacity_planner: actual utilization → model retraining pipeline
- llm_assisted_runbook_executor: execution outcomes → success_rate per runbook + A/B test results
- predictive_autoscaling: actual vs predicted → rolling MAPE tracking → triggered retraining

## Common Databases

### PostgreSQL (All 6)
- All 6; primary OLTP; UUID PKs; JSONB for flexible metadata; confidence DECIMAL(3,2); TIMESTAMPTZ for all time fields

### pgvector (2 of 6)
- ai_ops_platform + llm_assisted_runbook_executor; `vector(1536)` with `text-embedding-3-large`; `ivfflat` index; `vector_cosine_ops`; 500-token chunks with 100-token overlap

### Kafka (3 of 6)
- agentic_auto_remediation + ai_ops_platform + llm_assisted_runbook_executor; priority queuing; at-least-once delivery; schema registry for message schemas

### Redis (2 of 6)
- infrastructure_copilot: session state per conversation (30-min TTL); last 10 messages + structured intent state
- predictive_autoscaling: feature store cache; computed features cached per service per time window

### Elasticsearch (2 of 6)
- ai_ops_platform + llm_assisted_runbook_executor: BM25 keyword indexing for hybrid RAG; combined with pgvector semantic search via RRF fusion

### S3 (3 of 6)
- intelligent_capacity_planner: historical forecasts, procurement records (125 GB total)
- llm_assisted_runbook_executor: execution logs archive (150 GB/year)
- predictive_autoscaling: model artifacts, training data archive (193 GB total)

## Common Communication Patterns

### Hybrid RAG (BM25 + Vector Search + RRF Fusion)
- ai_ops_platform + llm_assisted_runbook_executor use hybrid retrieval:
  1. Vector search (pgvector, top-20): semantic similarity via `text-embedding-3-large`
  2. BM25 search (Elasticsearch, top-20): keyword match in parallel
  3. Reciprocal Rank Fusion (RRF): `score(d) = Σ(1 / (k + rank(d)))` where k=60 (standard RRF constant)
  4. Optional cross-encoder re-ranking on top 5–8 results
  5. Top results added to LLM prompt context

### Tool Calling (All 6 Systems)
- All 6 use LLM function/tool calling to invoke infrastructure operations
- Tools: kubectl (get/apply/delete/scale), SSH runner (allowlisted commands, 60s timeout), cloud APIs (EC2/ELB/ASG/RDS), Terraform Cloud, DNS API, CMDB lookup, K8s API
- Tool results returned to LLM for evaluation before next step

### Slack / PagerDuty Integration (3 of 6)
- agentic_auto_remediation + ai_ops_platform + llm_assisted_runbook_executor: write diagnosis + recommended actions to PagerDuty/Slack; receive approvals via reactions/acknowledgments

## Common Scalability Techniques

### Windowed Aggregation (2 of 6)
- ai_ops_platform: Flink/Kafka Streams — tumbling 30s windows for alert dedup; session gap 5 minutes for incident correlation; reduces 2M alerts/day to 40K incidents
- predictive_autoscaling: 5-minute prediction cycles; windowed feature computation (15m, 1h rolling averages)

### Parallel Execution (3 of 6)
- agentic_auto_remediation: concurrent remediation sessions (up to 10 simultaneous)
- llm_assisted_runbook_executor: parallel execution of independent runbook steps
- predictive_autoscaling: all 500 services predicted in parallel per 5-minute cycle

### Lazy / On-Demand Evaluation (2 of 6)
- intelligent_capacity_planner: LLM narrative reports generated on demand (not on every batch cycle); what-if simulations only on request
- infrastructure_copilot: LLM reasoning only invoked when request needs it; CMDB/cost lookups on demand

### Downsampling / Data Compression (2 of 6)
- predictive_autoscaling: raw metrics 15s scrape → 1-min aggregation in storage → hourly for old data; reduces storage 5–10×
- intelligent_capacity_planner: hourly rollup of utilization metrics; store P50/P90/P99 not raw

## Common Deep Dive Questions

### How do you prevent the LLM agent from taking unsafe actions in production?
Answer: Multi-layer safety before every action: (1) Precondition check — verify system is in expected state before acting (e.g., `healthy_replicas >= 2` before killing pods); (2) Blast radius calculation — `users_affected = (affected_pods / total_pods) × (service_users / total_users)`; escalate if > 20% users impacted; (3) Rate limiting — per-service, per-action-type limits prevent cascading failures from repeated actions; (4) Allowlists — SSH runner only executes allowlisted commands; kubectl limited to specific verbs; (5) Rollback stack — every action records its inverse operation; full session rollback possible; (6) Human approval gate — high-risk actions (blast radius > threshold) always pause for human confirmation.
Present in: agentic_auto_remediation, llm_assisted_runbook_executor

### How do you build reliable confidence scoring that doesn't rely solely on LLM self-reporting?
Answer: Ensemble confidence combining independent signals: (1) LLM self-reported confidence (weight 0.30); (2) RAG retrieval quality — avg cosine similarity of top-3 retrieved chunks (weight 0.30); (3) Metric clarity — how anomalous are the signals (> 3σ = higher confidence) (weight 0.20); (4) Historical accuracy for this service/pattern (weight 0.20). Then apply Platt scaling calibration trained on 90 days of `(predicted_confidence, actual_correct)` pairs to convert raw score to calibrated probability. Routing thresholds: ≥ 0.90 + low risk → auto; ≥ 0.75 → notify + 5min auto; ≥ 0.50 → escalate with diagnosis; else → escalate urgent.
Present in: ai_ops_platform, llm_assisted_runbook_executor

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.9% (auto-remediation, copilot, runbook executor, autoscaling); 99.95% (AIOps) |
| **Audit Completeness** | 100% of actions and LLM reasoning steps logged immutably |
| **Latency (action)** | Alert → first action: < 60s; intent parse → plan: < 10s; runbook start: < 30s; prediction: < 5s/service |
| **Accuracy** | Diagnosis > 85% (AIOps); intent parsing > 92% (copilot); runbook selection > 90% (executor); MAPE < 15% (autoscaling) |
| **Success Rates** | Remediation > 90%; false remediation < 0.5%; runbook execution > 99%; false scale-down < 1% |
| **LLM Spend** | < $50K/month (AIOps); controlled via confidence-based routing to avoid unnecessary LLM calls |
| **Throughput** | 50K alerts/min (AIOps); 14.4M data points/day (autoscaling); 40.8K tool calls/day (remediation) |
| **Cost Savings** | > 20% vs over-provisioning (autoscaling); 20–25% MTTR reduction (AIOps) |
