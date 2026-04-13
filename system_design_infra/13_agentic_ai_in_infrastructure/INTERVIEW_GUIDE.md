# Reading Infra Pattern 13: Agentic AI in Infrastructure — 6 problems, 9 shared components

---

## STEP 1 — ORIENTATION

This pattern covers six systems where AI agents (usually LLM-backed) take autonomous or semi-autonomous action on real infrastructure. They are not chatbots that suggest things. They are agents that actually execute kubectl commands, restart pods, order hardware, and scale services. That distinction is the entire reason these systems are hard to design — you must give the AI real power while building enough guardrails that it cannot blow up production.

The six problems are:

1. **Agentic Auto-Remediation** — An agent that autonomously diagnoses and fixes infrastructure failures using a ReAct loop, tool calls (kubectl, SSH, cloud APIs), and an undo stack.
2. **AIOps Platform** — The central nervous system: ingests 2M alerts/day, correlates them into incidents, runs RAG-powered LLM triage, and either auto-remediates or escalates with confidence scores.
3. **Infrastructure Copilot** — A natural language interface across CLI, Slack, and web that translates "provision 4 GPU servers" into validated, RBAC-checked, audited infrastructure operations.
4. **Intelligent Capacity Planner** — A weekly batch system that forecasts resource demand 2–12 weeks ahead, generates procurement recommendations with vendor lead times, and produces LLM-narrated executive reports.
5. **LLM-Assisted Runbook Executor** — Converts static runbook documents into dynamically parameterized executable workflows. The LLM selects the right runbook, fills in values from alert context, executes steps, and handles branching.
6. **Predictive Autoscaling** — Per-service ML models (Prophet, LSTM, XGBoost, ARIMA) that forecast demand 15–60 minutes ahead and proactively set the replica floor before traffic arrives.

The nine shared components that appear across most or all six systems are: the LLM integration layer, the OBSERVE-REASON-ACT-VERIFY loop, human approval gates, PostgreSQL as the OLTP store, Kafka event queues, confidence thresholding and routing, an immutable audit trail, feedback loops, and tool-calling (kubectl/SSH/cloud APIs). Understanding these once and knowing which systems use which variant is how you demonstrate Staff+ fluency.

---

## STEP 2 — MENTAL MODEL

**Core idea:** An agentic AI system in infrastructure is an AI that has been given a set of tool definitions (kubectl get, kubectl scale, cloud APIs, SSH runner) and is allowed to choose and execute those tools in sequence, observing the result of each step before deciding the next. It reasons in language but acts on systems. The architecture must enforce that at every moment before an action fires, an independent safety check has verified the system is in a known-safe state to receive that action.

**The real-world analogy:** Think of a highly experienced on-call SRE sitting down to handle an incident. She reads the alert, looks at the runbook, runs a few kubectl commands to understand the current state, forms a hypothesis, tries a remediation, checks if it worked, and escalates if she is not making progress. An agentic AI system does exactly this — except it does it at machine speed, works 24/7, and can handle ten incidents in parallel. The catch is that a human SRE has judgment built from years of context. The AI gets its judgment from a language model, a retrieval corpus, and a set of hard-coded safety rules. When the SRE is blocked, she escalates. The AI must do the same.

**Why it is genuinely hard to design:**

First, the system must wield dangerous tools — deleting pods, draining nodes, changing cloud autoscaling group targets — with correctness guarantees. If you miss a precondition check, you can cause the outage you were trying to fix.

Second, LLMs are non-deterministic and can hallucinate. An LLM that confidently claims the root cause is a database connection leak while the actual cause is a misconfigured DNS record will take all the wrong actions. You need calibrated confidence that does not rely solely on the model's self-reported certainty.

Third, you need to handle the fact that between the time you check a precondition and the time you act, the system state can change. This is the TOCTOU problem applied to infrastructure operations.

Fourth, feedback loops and learning are hard. If you never improve the system's accuracy based on outcomes, the confidence calibration drifts, the retrieval corpus goes stale, and the ML models miss new traffic patterns. But if you feed back too aggressively, you overfit or accidentally reinforce mistakes.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing anything, ask these four or five questions. Each one changes what you build.

**Question 1: "What is the latency expectation — does this need to act within seconds of an alert, or within minutes?"**
This separates the AIOps/Auto-Remediation cluster (sub-30-second diagnosis, sub-5-minute remediation) from the Capacity Planner (weekly batch). If the answer is "within 60 seconds," you need Kafka ingestion, a streaming correlation engine, and parallel LLM calls. If the answer is "weekly," you can run Airflow batch pipelines and use Snowflake for analytics.

**Question 2: "What is the blast radius tolerance — can the system act automatically on high-confidence diagnoses, or does every action need human approval?"**
This determines whether you need a human approval gateway with Slack/PagerDuty integration, and how you design the confidence routing tiers. Some organizations will not allow any autonomous infrastructure action; others are comfortable with pod restarts but require approval for deployments or scaling.

**Question 3: "What infrastructure tools does the agent need access to — Kubernetes only, or also bare metal, cloud VMs, databases, DNS?"**
The tool executor design changes dramatically. Kubernetes operations via kubectl are relatively well-contained. SSH access to bare-metal hosts requires an allowlist approach and much stricter access controls. Cloud API access (EC2, RDS, Route53) requires IAM policy scoping per action type.

**Question 4: "Do you have an existing runbook corpus, past incident post-mortems, or dependency graph — or are we starting from scratch?"**
This determines whether RAG is feasible from day one. If runbooks exist in Confluence or Markdown, you can build a retrieval corpus. If they do not exist, the system must either operate on general reasoning alone (lower accuracy) or prioritize runbook generation as a first milestone.

**Question 5 (for prediction/autoscaling): "How much history exists per service, and are there known scheduled events like marketing campaigns or batch jobs?"**
Less than two weeks of history forces reactive-only mode. Scheduled events dramatically improve prediction accuracy and are worth explicitly modeling.

**What changes based on answers:** If the org requires human approval for all actions, simplify the confidence routing tiers to just "show plan / escalate" and focus design effort on the approval UX. If no runbook corpus exists, the RAG component is not useful initially — skip it and focus on a structured output schema for the LLM to produce new runbooks from incident post-mortems.

**Red flags that suggest you misunderstood the problem:** If you jump straight to LLM without asking about existing tooling and runbooks, you will design a system with no retrieval grounding, which produces hallucinated diagnoses. If you do not ask about blast radius tolerance, you will get a gentle pushback when you propose auto-execution because the interviewer knows their hypothetical org would never allow it.

---

### 3b. Functional Requirements

**Core requirements that appear in every variant:**

- Accept an incident or request as input (from an alert queue, a user, or a scheduled trigger)
- Reason over available context to determine the best next action
- Execute that action via a defined set of infrastructure tools
- Verify the outcome before declaring success or moving to the next step
- Escalate to a human when confidence is too low, blast radius is too large, or iteration limit is reached
- Log every decision, tool call, and reasoning step immutably

**Scope boundary:** The system you are designing does NOT do initial anomaly detection (another system fires the alerts), does NOT write application code, and does NOT handle infrastructure design (that is a separate architecture review process). It operates on a known incident or request and drives it to resolution or escalation.

**Clear statement for the interview:** "We are building an agent that takes a diagnosed incident as input, uses a ReAct loop to select and execute infrastructure remediation actions, enforces safety before every action, and escalates gracefully when it cannot resolve autonomously. Every action is logged for full audit. The system operates alongside but does not replace human SREs."

---

### 3c. Non-Functional Requirements

**Latency:** For real-time systems (auto-remediation, AIOps, runbook executor), the target is alert to first action in under 60 seconds, end-to-end remediation of common cases in under 5 minutes. For the copilot, intent parsing should produce a response in under 5 seconds. For the capacity planner, the weekly batch can take up to 5 minutes for the full portfolio. The binding constraint is LLM inference latency (2–15 seconds per call), which is why you want minimal, well-structured prompts and tiered model selection.

**Availability:** The AIOps platform needs 99.95% because it is itself critical infrastructure — if the ops platform is down during an outage, you are flying blind. The other systems can operate at 99.9%. Achieving 99.95% means active-active in at least two AZs, health checks on every LLM API dependency, and a fallback path (human-only escalation) when the AI components are degraded.

**Accuracy:** Diagnosis accuracy of greater than 85% for top-3 root causes, remediation success rate of greater than 90% for known patterns, false auto-remediation rate of less than 0.5%. These are not arbitrary numbers; they come from tracking human-labeled outcomes over 90 days and calibrating confidence thresholds accordingly.

**Audit completeness:** 100%, always. Every AI action on production infrastructure must be traceable to a specific incident, reasoning chain, confidence score, and approver (either "auto" or a human username). This is non-negotiable for compliance and post-incident review.

**Cost:** LLM inference at scale is expensive. The AIOps platform targets under $50K/month. This forces tiered model selection (use the cheapest model that meets the accuracy bar for each step), caching frequent patterns, and routing to avoid unnecessary LLM calls.

**Trade-offs to state explicitly:** Higher availability requires more active replicas and a fallback mode, which adds engineering complexity. Lower latency pushes toward smaller, faster LLMs, but smaller models have lower reasoning quality, which increases escalation rate. Stricter safety rules reduce false remediation rate but increase time-to-resolution because more actions require human approval. Every threshold decision — confidence tiers, blast radius limits, iteration caps — is a dial on this trade-off surface.

---

### 3d. Capacity Estimation

**AIOps Platform (the anchor problem):**

Start with the alert volume and work down to the bottleneck.

- 2,000,000 alerts per day from 5,000 services
- 2M / 86,400 seconds = 23 alerts/second average; 10x burst = 230 alerts/second
- After dedup and correlation: 2M alerts reduce to approximately 40,000 incidents per day (one incident clusters about 50 alerts on average)
- Each incident needs 3 LLM calls (context gathering, diagnosis, action routing) = 120,000 LLM calls per day = 1.4 calls/second average, 14 calls/second at peak

**Storage:**
- Alert events for 1 year at 2KB each: 2M × 2KB × 365 = approximately 1.5 TB
- Incident records: 40K × 10KB × 365 = approximately 146 GB
- Runbook and past incident embeddings: vector(1536) × 4 bytes × ~300,000 chunks = approximately 1.8 GB
- Total: approximately 2 TB in year one

**LLM cost anchor:** At $15 per million input tokens and 8KB average prompt (approximately 2,000 tokens), 120,000 calls per day is about 240 million tokens per day = $3,600 per day = $108K per month without optimization. This makes it immediately clear why tiered model selection and diagnosis caching are required to stay under $50K/month.

**Architecture implications from the numbers:**
- 230 alerts/second burst requires Kafka for buffering — no synchronous HTTP ingestion endpoint will hold
- 14 LLM calls/second peak requires either a managed API with burst quota or a self-hosted vLLM cluster sized for this throughput
- 1.5 TB/year of raw alert data requires a storage tier strategy — hot for 30 days in PostgreSQL, cold to S3 thereafter
- 10 concurrent remediation sessions is small enough to run in a single Kubernetes deployment with horizontal pod autoscaling

**Time to estimate this in an interview:** 3–5 minutes. Write four numbers on the whiteboard: alerts/day, incidents/day, LLM calls/day, and storage/year. Derive the bottleneck (LLM) out loud and use it to justify your architecture choices.

---

### 3e. High-Level Design

**The six core components in draw order:**

**1. Event ingestion layer (draw first):** For real-time systems, this is a Kafka cluster with a topic for raw alerts or incidents. Normalizes heterogeneous sources (Prometheus, CloudWatch, Datadog) into a common schema before any processing. For the copilot, this is an API gateway with OAuth2/SSO extracting user identity.

**2. Correlation or session manager (draw second):** For AIOps, this is a Flink streaming job that deduplicates alerts using fingerprints and groups related alerts into incidents using a service dependency graph plus a 5-minute session window. For the copilot, this is a session manager backed by Redis with a 30-minute TTL that maintains conversation state.

**3. AI orchestrator (draw third, biggest box):** The agent loop. OBSERVE collects current system state. REASON calls the LLM with a structured prompt. ACT executes the chosen tool. VERIFY checks the outcome. This is the core differentiator. Inside the orchestrator, always draw three sub-components: a safety gate (precondition checks), a rate limiter (per-service, per-action-type), and a rollback manager (undo stack per session).

**4. Tool executor layer (draw fourth):** The boundary where the agent touches real infrastructure. Separate tool executors for kubectl, SSH (allowlist only), cloud APIs (EC2/ELB/ASG/RDS), Terraform, CMDB. Each executor has its own permission model and risk classification (none / low / medium / high).

**5. Human approval gateway (draw fifth):** Slack and PagerDuty integration. When confidence or risk level requires human review, the system pauses, posts the proposed action plan with blast radius estimates, and waits for a reaction or acknowledgment. Configurable timeout — auto-execute after N minutes for low-risk actions, escalate for high-risk.

**6. Immutable audit log and observability (draw sixth):** PostgreSQL append-only tables capturing every reasoning step, tool call, precondition result, and outcome. Prometheus metrics for accuracy, MTTR trends, confidence distributions. This is what the compliance team and post-incident review process depend on.

**Data flow to narrate:** Alert fires → Kafka → Flink correlation → incident created → AI orchestrator picks up from queue → OBSERVE gathers context (metrics, recent deploys, RAG results) → REASON calls LLM → ACT fires tool after safety gate passes → VERIFY queries metrics → if resolved, write outcome to audit log; if not, loop back to REASON with new observations.

**Key architecture decision to voice aloud:** "I am choosing not to have the LLM directly call infrastructure APIs. Instead, it emits a structured JSON tool call that gets validated by the safety gate and rate limiter before the actual API call fires. This is the key safety guarantee — the LLM is in the reasoning loop but is never in the execution hot path without a deterministic safety check in between."

---

### 3f. Deep Dive Areas

**Deep Dive 1: Safety Gate Design**

The interviewer will ask this because giving an AI agent real tools to run in production is frightening, and the safety gate is what separates a responsible design from a catastrophic one.

The safety gate runs two layers before every action. The first is static rules — fast, O(1) checks that run entirely in memory. Examples: do not allow kubectl operations in the kube-system namespace, do not allow scaling to more than 3x current replicas in one step, do not allow SSH to any host not in the inventory, do not allow any SSH command not in the allowlist. The second is real-time state checks — API calls that verify current infrastructure state. Examples: "before deleting a pod, confirm at least 2 healthy replicas remain" and "before draining a node, confirm remaining nodes have > 20% spare capacity."

The key design decision is fail closed. If the real-time check times out because the Kubernetes API is unreachable, you deny the action. You do not guess. Failing closed means you may delay remediation by a few minutes. Failing open means you may cause an outage. In infrastructure, delayed is recoverable; wrong action is not.

The secondary design decision is TOCTOU mitigation. Between the time you check "2 healthy replicas" and the time you issue the delete command, another pod could have crashed. Three mitigations: (1) Keep the window small — check and execute within the same transaction where possible using Kubernetes resourceVersion for optimistic concurrency. (2) Post-action verification catches cases where state changed between check and action. (3) For critical actions, use Kubernetes admission webhooks that re-validate at the actual API server level.

Trade-offs to name unprompted: Stricter safety rules mean more blocked actions, which means higher escalation rate. You need to track "actions blocked by safety gate" as a metric and review it weekly — if too many legitimate actions are being blocked, your rules are too conservative and humans are doing work the agent should be doing.

**Deep Dive 2: Confidence Scoring and Routing**

The interviewer probes this because every agentic AI system that acts autonomously lives or dies on its confidence calibration. An overconfident AI takes wrong actions. An underconfident AI always escalates and provides no value.

Never use LLM self-reported confidence alone. LLMs are systematically overconfident. Instead, build an ensemble with four components: (1) LLM self-reported confidence, weighted 30%. (2) Retrieval quality — the average cosine similarity of the top-3 RAG results. High similarity means the problem is well-documented and the model has real context. Weight this 30%. (3) Metric clarity — how anomalous are the signals? Greater than 3 sigma anomaly = higher confidence. Weight this 20%. (4) Historical accuracy for this service or pattern — how often has the system been right in the past for this failure type? Weight this 20%.

Then apply Platt scaling calibration trained on 90 days of (predicted_confidence, actual_correct) outcome pairs. This converts the raw weighted average into a calibrated probability.

The routing tiers: confidence >= 0.90 and action risk is low → auto-execute. Confidence >= 0.75 → show plan to human, auto-execute after 5 minutes if no objection. Confidence >= 0.50 → escalate with full diagnosis but no action plan. Below 0.50 → urgent escalation, flag as novel or ambiguous.

Bootstrap problem: In the first 30 days, you have no calibration data. Start conservative — every action requires human approval (shadow mode). After 500 labeled incidents, train the Platt scaling model. Gradually lower thresholds as accuracy is proven.

Trade-offs to name unprompted: When you switch LLM model versions, reset to shadow mode for 48 hours and recalibrate before restoring auto-execution thresholds. Never hot-swap the production LLM without a calibration check.

**Deep Dive 3: RAG Architecture for Incident Retrieval**

This comes up because the LLM's general knowledge does not include your service topology, your runbooks, or your recent incident history. Without RAG, you get generic answers. With bad RAG, you get irrelevant context that wastes tokens and may mislead the model.

Use hybrid retrieval: dense vector search (pgvector with text-embedding-3-large, 1536 dimensions) plus BM25 keyword search (Elasticsearch) in parallel, then merge results using Reciprocal Rank Fusion with k=60. This handles both semantic matches ("out of memory" and "OOM" match even without exact string overlap) and exact term matches (service names, alert names, specific error codes).

Chunking strategy: 500-token chunks with 100-token overlap. Preserve section headings as metadata so the retrieval results show "High Memory Usage > Step 3: Restart Pod" rather than an anonymous text fragment. Run both retrieval paths and fetch top-20 from each, then apply RRF, optionally re-rank with a cross-encoder, and deliver top-5 to 8 chunks to the LLM prompt.

Context window construction is explicit and budgeted: system prompt 500 tokens, incident context 2,000 tokens, current metrics snapshot 1,500 tokens, recent deploys 500 tokens, runbook chunks 3,000 tokens, past incident summaries 2,000 tokens, conversation history 3,000 tokens. Total approximately 13,000 tokens. The discipline here is important — without a budget, prompts bloat, latency rises, and the model's attention dilutes.

Trade-offs: pgvector handles up to about 10 million vectors with sub-200ms latency. Beyond that, you would migrate to a dedicated vector database like Weaviate or Pinecone. Start with pgvector because it keeps embedding search collocated with your relational data, allowing joined queries.

---

### 3g. Failure Scenarios

**Failure 1: LLM hallucinates a confident wrong diagnosis**
Hallucinations often correlate with low retrieval scores — the model is making things up because it found nothing relevant in the corpus. Mitigations: (1) The ensemble confidence down-weights low retrieval scores, so a hallucinated diagnosis with poor RAG support will get a low final confidence and escalate rather than auto-execute. (2) Factual claims can be cross-checked against the CMDB — if the LLM mentions a service that does not exist, that is a detectable hallucination. (3) Post-action verification: if the LLM claimed the problem was a memory leak and the action it took did not change the memory metric, the verification step catches this.

Senior framing: "The safety net for hallucinations is not prompt engineering — it is ensuring no single hallucinated output can trigger action without passing through both the ensemble confidence gate and the safety gate. The model's reasoning is advisory; the safety gate is deterministic."

**Failure 2: Alert storm — 100x normal alert volume during a major outage**
When everything is on fire simultaneously, the system must not itself become a bottleneck. Kafka absorbs the burst. The Flink correlation engine aggressively merges alerts — during a major outage, most alerts trace to one or three root incidents, so 10,000 alerts collapse to a small number of incidents. Apply a circuit breaker: if the LLM inference queue exceeds a threshold (say, 100 pending incidents), bypass AI triage for info and warning severity incidents and process only critical. Pre-warm extra triage workers when alert volume velocity is rising.

Senior framing: "The AIOps platform is itself critical infrastructure. If it goes into cascade failure during the incident it is supposed to help resolve, you have lost your co-pilot when you need it most. The design must degrade gracefully to human-only mode."

**Failure 3: Agent loop stuck — repeated failed actions**
The agent tries the same action twice, fails twice, and then faces the same decision again because the LLM does not have strong enough context about why the previous attempts failed. Mitigations: (1) The full action history including failure outputs is in the LLM context; the prompt explicitly forbids repeating failed actions. (2) Programmatic check: if the proposed action is identical to one that failed in the last three steps, block it and append a note. (3) After two programmatically blocked repeated actions, force escalation. (4) Set a hard iteration cap — 8 iterations for auto-remediation, 5 for AIOps triage — and escalate when reached.

**Failure 4: Partial remediation followed by rollback failure**
The agent successfully completes steps 1 and 2 of a multi-step remediation, step 3 fails, and when you try to roll back steps 1 and 2, the rollback itself fails (for example, the previous state no longer exists). This is an immediate P0 escalation with full context. Keep state snapshots before each action (not just the inverse operation) so a human can manually restore. Log the exact state the system was in before every destructive action, not just the parameterized rollback command.

---

## STEP 4 — COMMON COMPONENTS

These nine components appear across multiple systems. Know them cold. Know which systems use them and why.

---

**Component 1: LLM Integration (all 6 systems)**

All six systems use Claude Sonnet or equivalent via managed API (or self-hosted vLLM) with function/tool calling enabled. The LLM outputs structured JSON for tool selection and parameter filling, not free-form prose. The key configuration decisions are: (1) Context window budget — explicitly allocate token budgets to each section of the prompt; do not let prompts grow unbounded. (2) The system prompt bakes in the agent's role, safety rules, and output format requirements. (3) LLM self-reported confidence is always combined with other signals — never used alone.

Without it: You replace LLM reasoning with static decision trees and keyword matching. This works for the top 50 most common failure patterns but completely fails on novel incidents, which is exactly when you most need help.

---

**Component 2: OBSERVE-REASON-ACT-VERIFY Loop (5 of 6 systems)**

Present in every system except the Intelligent Capacity Planner, which uses a simpler batch pipeline pattern instead. The loop has a fixed structure: OBSERVE gathers current state (metrics, alerts, inventory, conversation history, recent deploys). REASON calls the LLM with this context to select the next action. ACT executes the selected tool after passing through the safety gate and rate limiter. VERIFY checks whether the outcome matched the expected result and updates the history. The loop has a maximum iteration count (5–8 depending on the system) and a wall-clock timeout (10–15 minutes). When either limit is reached, the system escalates.

Key configuration: Max iterations is a critical dial. Too low and you escalate before the agent can resolve genuinely complex multi-step issues. Too high and a stuck agent wastes time and may cause additional harm through repeated failed actions.

Without it: You have a one-shot LLM call that takes an action and then hands off to humans regardless of outcome. This misses the adaptation that makes the agent useful — learning from "that pod restart didn't fix it, so let me look at the logs" is exactly what makes autonomous remediation work.

---

**Component 3: Human Approval Gates (4 of 6 systems)**

Present in auto-remediation, AIOps, copilot, and runbook executor. The gate fires when confidence is medium (between 0.75 and 0.90), when action risk is high, or when blast radius exceeds a threshold. Implementation: post the proposed action plan to Slack (with resource type, estimated impact, blast radius, and a cost estimate if applicable), wait for a reaction emoji or PagerDuty acknowledgment, and either execute or escalate based on response.

Key configuration: Timeout behavior is the most important decision. For low-risk actions, auto-execute after 5 minutes if no response (optimistic gate). For high-risk actions, never auto-execute; escalate if there is no response (pessimistic gate). The system must distinguish these cases automatically based on the action's risk classification.

Without it: Every action either always requires human approval (removes the value of automation) or always executes autonomously (removes the safety guarantee for high-risk actions). The gate enables both efficiency and safety.

---

**Component 4: PostgreSQL as Primary OLTP Store (all 6 systems)**

Every system uses PostgreSQL for authoritative state: incident and session records, audit logs, execution history, model metadata, and runbook definitions. Common schema patterns: UUID primary keys everywhere, JSONB for flexible metadata fields that vary per record, TIMESTAMPTZ for all time fields, DECIMAL(3,2) for confidence scores (range 0.00–1.00), VARCHAR[] for tags and labels.

Key configuration: In two systems (AIOps and runbook executor), PostgreSQL is extended with pgvector for embedding storage — vector(1536) columns with ivfflat indexes and vector_cosine_ops distance function. The ivfflat index is configured with lists=100 for AIOps (larger corpus) and lists=50 for runbook executor (smaller corpus). IVFFlat is appropriate here because these are approximate nearest-neighbor queries that tolerate a few percent miss rate in exchange for sub-200ms latency.

Without it: You could use DynamoDB or Cassandra for the audit trail, but you would lose the ability to do complex analytical queries (MTTR by service, accuracy trends by confidence tier, blast radius reports). PostgreSQL's combination of ACID guarantees, JSONB, and pgvector makes it uniquely well-suited to be the single store here.

---

**Component 5: Kafka Event Queue (3 of 6 systems)**

Present in auto-remediation (remediation queue with priority: critical > warning > info), AIOps (raw-alerts topic ingesting 230 alerts/second peak, then correlated-incidents topic), and runbook executor (execution queue carrying incident context). Key configuration: at-least-once delivery semantics are acceptable here because operations are idempotent — trying to restart a pod that was already restarted is harmless. Schema registry enforces message contracts. Consumer groups allow independent scaling of downstream processors.

Key configuration for AIOps: The raw-alerts topic needs enough partitions to handle the 230 alerts/second peak without consumer lag. A good starting point is 12 partitions for this topic, sized for the Flink consumer group to maintain sub-2-second processing latency.

Without it: Direct synchronous HTTP ingestion from monitoring systems to the AI triage worker. At 230 alerts/second peak during an outage, this creates back-pressure and drops alerts exactly when you need them most.

---

**Component 6: Confidence Thresholding and Routing (4 of 6 systems)**

Present in AIOps, copilot, runbook executor, and predictive autoscaling. The routing decision is always a four-tier switch: >= 0.90 and low risk → auto-execute; >= 0.75 → notify with auto-execute timeout; >= 0.50 → escalate with diagnosis; below 0.50 → urgent escalation. The specific thresholds and timeouts are tunable parameters that you calibrate based on outcome data.

The underlying principle is always the same: confidence alone is not enough to determine routing. You also need action risk classification (which is determined by the action type, not the diagnosis confidence). Confidence 0.95 does not justify automatically draining a node in a cluster that has only three nodes total. Routing must be a function of both confidence AND risk level.

Without it: Binary routing — either always auto-execute or always escalate. Either burns human time on trivial issues or creates high-stakes autonomous actions without appropriate safeguards.

---

**Component 7: Immutable Audit Trail (5 of 6 systems)**

Present in all systems except predictive autoscaling (which has audit logs but they are not the core design focus). Every AI decision, tool call, and LLM reasoning step is written to append-only PostgreSQL tables. No UPDATE or DELETE on audit records — ever. The schema always captures: actor/source, action type, action parameters, result, timestamp, confidence score, and reasoning_context (the chain-of-thought that led to the decision).

Key configuration: Create partial indexes on status for active records and on incident_id for fast lookups. The audit trail is also the primary source of truth for the accuracy and calibration measurements that feed back into the confidence scoring model.

Without it: When an autonomous agent causes an outage (and eventually one will), you have no way to reconstruct what it was thinking, what it saw, and why it chose that action. This is both a compliance failure and an operational intelligence failure.

---

**Component 8: Feedback Loops and Continuous Learning (4 of 6 systems)**

Present in AIOps (human corrections update the retrieval corpus and generate fine-tuning data), capacity planner (actual utilization drives model retraining), runbook executor (execution outcomes update success_rate per runbook and feed A/B tests), and predictive autoscaling (actual vs predicted demand drives rolling MAPE tracking and triggered retraining).

The common implementation pattern: every outcome is compared to the prediction or recommendation. Significant divergence triggers a retraining job (for ML models) or a retrieval corpus update (for RAG). The feedback is never immediate — it goes through a review step to catch cases where the actual outcome was a fluke.

Without it: The system's accuracy slowly degrades as traffic patterns evolve, runbooks go stale, and incident types shift. A system without feedback loops is a system that needs to be completely rebuilt every year.

---

**Component 9: Tool Calling with Typed Tool Definitions (all 6 systems)**

All six systems use LLM function/tool calling where each tool is a defined JSON schema with name, description, parameters, and a risk_level field. The LLM selects a tool and emits a JSON tool call object; the orchestrator then validates the call against the schema, runs the safety gate, and if both pass, invokes the actual executor. The LLM never directly calls infrastructure APIs — it always goes through this typed layer.

Tool risk levels: read-only tools (kubectl get, query_metrics, query_logs, search_runbooks) are risk level "none" and always allowed. Low-risk mutation tools (kubectl_delete_pod with preconditions) require safety gate only. Medium-risk tools (kubectl_scale, cloud autoscaling changes) require safety gate plus blast radius check. High-risk tools (kubectl_drain_node, rollback_deployment, rds_failover) require human approval regardless of confidence.

Without it: You either give the LLM untyped text commands and hope it formats them correctly (prompt injection nightmare), or you hard-code every possible action in the application logic (unmaintainable). The typed tool calling contract is how you get the flexibility of LLM reasoning with the correctness guarantees of typed interfaces.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

For each problem, here is what makes it unique and what decisions you would make differently.

---

**Agentic Auto-Remediation**

Unique elements: The ReAct loop here is the most adversarial — the agent is performing multi-step actions with incomplete information, each step changing the system state and making the next decision harder. The rollback stack is a first-class architectural component: every destructive action records its inverse operation before execution, enabling full session rollback. The blast radius estimator runs before every action: users_affected = (affected_pods / total_pods) × (service_users / total_users) × 100, with escalation if this exceeds 20%. The system receives an already-diagnosed incident from the AIOps platform — it does not need to do root cause analysis, only remediation planning.

Different decisions: The max iteration cap is 8 (highest in the pattern group) because remediation sometimes needs more steps than diagnosis. The SSH runner uses a command allowlist rather than arbitrary execution. The safety gate uses both static rules and real-time API checks in parallel.

Two-sentence differentiator: This is the highest-stakes system in the group — it physically changes production infrastructure and must have an undo stack for every action. The core architectural guarantee is that the LLM is in the reasoning loop but is never in the execution path without a deterministic, real-time safety gate between the tool call and the actual API invocation.

---

**AIOps Platform**

Unique elements: The only system that ingests raw alerts (50,000/minute peak) and must perform its own correlation before the agent loop even starts. The Flink correlation engine is a unique component not found elsewhere in this pattern group: it deduplicates alerts using fingerprints, groups related alerts using a service dependency graph plus a 5-minute session window, and reduces 2 million alerts per day into 40,000 incidents. The RAG corpus is the largest here — 3,000 runbooks plus 15,000 past incident post-mortems — and uses full hybrid retrieval with RRF fusion. The 99.95% availability target is the highest in the group.

Different decisions: Tiered LLM model selection specifically for cost (Haiku or Llama for initial routing, Sonnet/Opus for complex multi-service incidents). Diagnosis caching for frequently recurring identical alert patterns. Context window management includes a token budget table and an explicit overflow strategy (summarize alert bursts exceeding 50 alerts, summarize earlier conversation turns after 5 turns).

Two-sentence differentiator: AIOps is the only system in this group that processes raw unstructured alert noise and must both correlate and diagnose before acting, making it architecturally upstream of all the other systems. It is also the most cost-sensitive because it runs the LLM on every incident, not just the ones that need remediation, so model tiering and diagnosis caching are essential to stay within budget.

---

**Infrastructure Copilot**

Unique elements: The only system in this group that takes natural language input from human users rather than from automated monitoring or batch pipelines. It is fundamentally an interactive system — multi-turn conversations, clarification dialogues, and explicit confirmation before execution. The session manager backed by Redis (30-minute TTL) maintains structured intent state, not just message history: it tracks the current parsed intent, pending confirmation, and actions completed this session. Intent state merging allows "actually make it 6 instead of 4" to update only the count field while preserving other extracted parameters. Three interface adapters (CLI, Slack bot, web chat) all feed the same conversation orchestrator.

Different decisions: RBAC enforcement is a first-class component not present in other systems — every action checks the user's permissions against Okta/Azure AD before execution. Intent validation against known enums (valid regions, valid instance types) runs before tool calls, not just after. The confirmation step is mandatory for all state-changing operations regardless of confidence.

Two-sentence differentiator: Infrastructure Copilot is the only conversational system in this group — it must maintain dialogue state across multiple user turns, ask clarifying questions when requests are ambiguous, and present action plans in human-readable form before execution. Unlike the reactive systems, it has a user identity and RBAC context that gates every action, making the permission model a primary architectural concern rather than an afterthought.

---

**Intelligent Capacity Planner**

Unique elements: The only system here that operates in weekly batch mode rather than real-time or interactive mode. It consumes data from sources other systems do not touch: finance systems (budget commitments, contract pricing), sales and product pipeline data (upcoming customer growth, product launches), and vendor lead time distributions (P50 and P90 delivery weeks per SKU). The four-scenario framework (optimistic at 0.85x, base at 1.0x, pessimistic at 1.25x, stress at 1.50x) is unique. Cost optimization uses mixed-integer programming to find the optimal mix of spot, reserved, and on-demand instances. The LLM's role here is narrative generation for executive reports, not reasoning about actions — it translates a structured forecast and recommendation into English prose.

Different decisions: The AI is not in an action loop; it is a report generator. The "action" is a procurement recommendation that goes to a human with an approval gate before any purchase order is generated. Forecast accuracy is measured in MAPE, not confidence scores.

Two-sentence differentiator: This is the only planning system in the group — it does not react to incidents but proactively forecasts capacity needs weeks ahead using a combination of time-series forecasting, business signal weighting, and mixed-integer programming for cost optimization. The LLM's role is fundamentally different here: it translates structured quantitative output into readable executive narratives rather than reasoning about what action to take.

---

**LLM-Assisted Runbook Executor**

Unique elements: The only system that bridges unstructured knowledge (Markdown runbook documents in Confluence or Git) and structured executable workflows. The runbook YAML schema is a major design artifact — it defines steps, parameters as templates (using ${namespace} placeholders), conditionals, preconditions, rollback steps, and human checkpoint flags per step. A/B testing of runbook versions is a built-in feature: traffic_split controls what fraction of matching incidents use version A vs version B, and success_rate is tracked per version. LLM-generated runbook drafts from incident post-mortems are a first-class feature — the AI writes first drafts that humans review and approve into the corpus.

Different decisions: Runbook selection uses a three-phase funnel: exact trigger pattern match first (fast, highest confidence), then semantic search, then LLM re-ranking. If no runbook matches with confidence >= 0.60, escalate rather than falling back to free-form reasoning. Parameter extraction and validation against CMDB and Kubernetes API before execution eliminates a whole class of runtime failures.

Two-sentence differentiator: Unlike the auto-remediation system which plans freely from scratch, the runbook executor is constrained to a defined corpus — the LLM's job is to select, parameterize, and navigate an existing workflow, not invent one. This makes it more predictable and auditable, but requires investment in building and maintaining the runbook corpus, and it cannot handle incident types that no runbook covers.

---

**Predictive Autoscaling**

Unique elements: The only system that uses classical ML (Prophet, ARIMA, LSTM, XGBoost) as its core reasoning layer instead of an LLM. One model per service per metric, selected through walk-forward cross-validation. The ensemble approach (inverse-MAPE weighted combination of top-3 models) is used only when the best single model has MAPE above 20%. The two-layer scaling architecture (predictive sets minimum replicas floor, reactive HPA handles spikes above that floor) is a unique integration pattern. Asymmetric cooldowns (3 minutes for scale-up, 10 minutes for scale-down) and a 20% maximum decrease per cycle protect against premature scale-down. Model validation follows a three-phase pipeline: offline cross-validation, then 48-hour shadow mode, then 24-hour canary at 10% traffic.

Different decisions: The LLM is only used for dashboard explanations and anomaly summaries (optional), not for the core prediction or scaling decision. Feature engineering is explicit and enumerated: 14+ features including lag values, rolling statistics, calendar features, and event flags. Model retraining is triggered either weekly (scheduled) or immediately when rolling 24-hour MAPE exceeds 25% for 6 hours.

Two-sentence differentiator: This is the only system in the group where the "AI" is classical ML rather than an LLM — the core intelligence is time-series forecasting models that must be trained, validated, versioned, and retrained on a per-service basis. Its unique integration challenge is coordinating with the existing Kubernetes HPA to avoid conflict between proactive prediction-driven scaling and reactive utilization-based scaling, which is solved by having the predictive layer set the minimum replica floor that HPA operates above.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (expect these in the first 20 minutes)

**Q: What is the ReAct pattern and why do you use it here instead of a simple decision tree?**
A: ReAct stands for Reason + Act. Rather than choosing an action and executing it, the agent interleaves a reasoning step (what do I know, what should I try next) with an action step (execute a tool), and uses the result of each action to inform the next reasoning step. This is critical for infrastructure remediation because failures are often novel combinations — a memory leak that only manifests under a specific load pattern, a cascading failure that started in a service three hops away. A **decision tree cannot handle novel combinations** because its branches were pre-built. A ReAct agent can adapt its plan based on what it actually observes. The trade-off is speed: each ReAct iteration costs one LLM call and one tool call, adding latency.

**Q: Why do you never use LLM self-reported confidence alone?**
A: LLMs are systematically overconfident. They will say "I'm 95% sure the root cause is X" even when they are essentially guessing because the retrieval found nothing relevant. The ensemble approach catches this: if the LLM claims high confidence but the RAG retrieval quality is low (meaning the problem is not well-documented in your corpus), the **ensemble confidence will be significantly lower than the self-reported value**, which routes the incident to human review instead of auto-execution. Self-reported confidence alone would cause the system to take wrong autonomous actions on novel incidents — exactly when human judgment matters most.

**Q: What is the blast radius calculation?**
A: Before every destructive action, the system computes users_affected_pct = (affected_pods / total_pods) × (service_users / total_users) × 100, and separately requests_affected_pct = (affected_capacity / total_capacity) × 100. If either exceeds 20%, the system escalates to human rather than executing autonomously. This provides a **quantified measure of potential customer impact** so the routing decision is not just based on action type but on the actual business consequence of that action in the current system state.

**Q: Why do you need Kafka between alert ingestion and the AI triage worker?**
A: Three reasons. First, **burst absorption** — during a major outage, alert volume can spike to 10x normal in seconds. Kafka absorbs this burst and allows the triage workers to process at their natural rate. Second, replay — if there is a bug in the correlation logic, you can replay the raw alert stream from Kafka to reprocess. Third, fan-out — multiple consumers (correlation engine, dashboards, audit log) can all read from the same topic independently. Without Kafka, you are coupling the upstream monitoring systems directly to the AI processing pipeline, which means a slow LLM call backs up into the alerting infrastructure.

**Q: How do you handle a service that has never experienced a particular failure before?**
A: This is where the LLM's general reasoning ability matters most. The RAG will likely retrieve partially relevant runbooks (similar symptoms, different service) and the LLM can reason from first principles using the metrics and logs it observes. The key is to be **explicit about low confidence** in the prompt instructions — the LLM should say "I have not seen this pattern before, confidence 0.35" rather than guessing confidently. Low confidence routes to escalation with a full diagnostic context summary, which is actually valuable to the on-call engineer.

**Q: What happens if the LLM API is unavailable or exceeds latency thresholds?**
A: Circuit breaker on the LLM client. When the API fails or is too slow, the system falls back to heuristic routing (alert severity + historical hit-rate for the affected service) and escalates with all available context. For the AIOps platform specifically, the system cannot silently stop processing — it must degrade gracefully by routing everything to human review rather than silently dropping incidents. The degraded mode is instrumented and alerted on separately.

**Q: Why do you set a maximum iteration count on the agent loop?**
A: Without a hard cap, a stuck agent loop can run indefinitely, burning LLM API costs, holding remediation session resources, and potentially causing escalating harm through repeated failed actions. The **maximum iteration count is a hard safety guarantee** that bounds the worst-case behavior. Eight iterations gives the agent enough steps to handle genuinely complex multi-step remediations (scale, verify, rollback, re-scale, verify) while still ensuring a bounded worst case. When the cap is hit, the system escalates with a full log of everything attempted, which is more useful to the on-call engineer than endless futile retries.

---

### Tier 2 — Deep Dive Questions (expect these if you have done well on Tier 1)

**Q: Walk me through your confidence calibration process. How do you bootstrap it when you have no historical data?**
A: **Bootstrapping requires shadow mode.** For the first 30 days, every action requires human approval regardless of the raw confidence score. This generates a labeled dataset of (predicted_confidence, human_override_rate) pairs — if the system claimed 0.90 confidence but humans rejected 40% of those actions, you know the raw score is overconfident. After 500 labeled incidents, you train a Platt scaling model (logistic regression on the raw scores with actual accuracy as labels) to produce calibrated probabilities. The calibration model needs weekly retraining because the underlying LLM's behavior drifts, incident types shift seasonally, and new services come online. When you change the underlying LLM version, you reset to shadow mode for 48 hours and recalibrate before restoring any auto-execution thresholds. The trade-off here is that the shadow period delays value delivery, but **shipping an uncalibrated agent is how you get a high-profile wrong action in week one**.

**Q: Explain the alert correlation algorithm and its failure modes.**
A: The algorithm uses a service dependency graph (auto-discovered from Istio/Envoy service mesh telemetry, updated nightly) plus time-window grouping. When an alert fires, the system looks up the affected service in the graph and checks if any upstream dependency has an alert within the same 5-minute session window. If yes, the alerts are merged into one incident with the upstream service marked as the probable root. The Flink job applies tumbling 30-second dedup windows (same alert fingerprint within 30 seconds = one alert) and then a 5-minute session gap for grouping. **The primary failure mode is a stale dependency graph** — if a service added a new upstream dependency last week and the graph has not been updated, cascade failures from that dependency will generate separate incidents rather than being correctly correlated. Mitigation: nightly graph refresh, alert if graph age exceeds 24 hours, and preserve a "split incident" API for humans to override incorrect correlations. A secondary failure mode is the "everything is one incident" problem during a global outage — cap incident merging at 20% of all services and create linked incidents rather than one mega-incident.

**Q: How do you prevent the predictive autoscaler from conflicting with the reactive HPA?**
A: The key insight is that they operate at different layers rather than in parallel. **The predictive system sets the minReplicas floor of the HPA configuration** — it says "I predict this service will need at least 12 replicas in 30 minutes" and writes minReplicas=12 into the HPA spec. The HPA then operates above that floor — if actual utilization spikes above what even 12 replicas can handle, HPA adds more. If utilization drops below the floor, HPA cannot scale below 12 because that is the current minimum. The predictive system updates the floor every 5 minutes based on new predictions. The asymmetric cooldowns (3 minutes for scale-up, 10 minutes for scale-down) prevent oscillation — when the predictive system lowers the floor, HPA waits 10 minutes before actually reducing replicas, smoothing out short-lived prediction errors.

**Q: How do you handle prompt injection? An attacker could put instructions in an alert message.**
A: The primary defense is **structural isolation of data from instructions** in the prompt. All user-supplied content (alert messages, log snippets, service names) is placed inside delimited blocks with explicit labeling: the system prompt instructs the LLM to treat content between specific delimiters as data to analyze, not as instructions to follow. The secondary defense is the tool calling contract — the LLM can only emit tool calls that match the defined tool schema; it cannot call arbitrary functions or issue shell commands. The tertiary defense is output validation: tool call parameters are validated against expected patterns (namespaces must be valid K8s namespace names, hostnames must be in the inventory) before execution. A prompt injection that gets through and causes the LLM to emit "call kubectl_delete_pod on kube-system/etcd" will be blocked by the static safety rule that forbids operations in the kube-system namespace. Defense in depth is essential here because no single layer is perfect.

**Q: How do you decide whether to use pgvector vs a dedicated vector database like Pinecone or Weaviate?**
A: The decision is driven by corpus size, query latency requirements, and operational simplicity. **pgvector with an ivfflat index handles up to approximately 10 million vectors with sub-200ms query latency** at AIOps scale (about 3 million vectors total across runbooks and past incidents). Keeping embeddings in PostgreSQL means you can JOIN embedding search results with incident metadata in a single query, avoid operational complexity of a separate database cluster, and handle everything under one backup and monitoring regime. The trigger to migrate to a dedicated vector database is either (1) query latency consistently exceeds 200ms (typically above 10 million vectors), or (2) you need sub-50ms latency for real-time applications, or (3) you need features like multi-tenancy, role-based vector partitioning, or real-time streaming updates to the index that pgvector does not support well.

**Q: How does the runbook executor handle conditional branching and rollbacks differently from the auto-remediation agent?**
A: The runbook executor follows a **pre-defined graph of steps** where conditional branches are explicit in the runbook YAML: each step has a conditional_next map from output status to next step number. The LLM evaluates whether each step's output matches the expected_condition field and picks the branch accordingly. Rollback is also pre-defined — steps marked is_rollback=true are executed in reverse order when the current step fails. This is more predictable and auditable than the auto-remediation agent's free-form planning, but it requires the runbook to have been authored correctly in advance. The auto-remediation agent can handle failure patterns that no runbook covers; the runbook executor cannot. In practice, you want both — run the runbook executor first for incidents that match a known runbook, fall through to the auto-remediation agent for incidents with no runbook match.

**Q: What does the intelligent capacity planner do differently from just looking at current utilization plus 20%?**
A: A naive "current + 20% headroom" approach misses three critical dynamics. First, **growth signals** — a signed contract with a large customer changes the demand trajectory fundamentally. A capacity planner that only looks at historical utilization will not order hardware in time. The business signals layer (customer growth at multiplier 1.12, product launch at specific multiplier, seasonal events like Black Friday at 3x) captures these structural shifts. Second, **vendor lead times** — for GPU hardware, lead time can be 8–16 weeks. You need to forecast far enough ahead to place the order in time, which means forecasting 12 weeks out with explicit lead time models per SKU. Third, the **mixed-integer programming optimization** for cost: at what point does it make financial sense to commit to a 1-year or 3-year reserved instance vs. continuing on-demand? The optimizer solves this across the full portfolio, accounting for utilization probability distributions, not just averages.

---

### Tier 3 — Staff+ Stress Tests (these are the questions that separate principal engineers from seniors)

**Q: If you had to cut 60% of the LLM inference budget without reducing core functionality, how would you do it?**
A: I would attack three levers simultaneously. **First, tiered model selection** — the current design already uses Claude Sonnet by default, but I would route the initial classification step (is this incident similar to one we've seen before?) to Claude Haiku or a fine-tuned Llama 7B. Only the diagnosis step for novel or multi-service incidents needs Sonnet-level reasoning. Based on the distribution, roughly 60–70% of incidents have been seen before, so this alone cuts major inference cost. **Second, aggressive diagnosis caching** — if the same alert fingerprint combination has been seen in the last 24 hours and the previous diagnosis was confirmed correct, return the cached diagnosis and skip the LLM call entirely. This is especially effective during multi-hour incidents where the same alerts repeat. Rough estimate: 30–40% of LLM calls are redundant in this way. **Third, reduce average prompt length** — the current context budget of ~13,000 tokens can be reduced to ~5,000 tokens for high-confidence retrieval hits by truncating the conversation history section and summarizing the runbook chunks more aggressively. These three changes together can cut inference cost by 50–60% without affecting accuracy for the 80% of incidents that are known patterns. The remaining 20% of novel incidents still get the full inference treatment, and that is where the budget should go.

**Q: The agent successfully fixed an incident, but three hours later the same failure recurs. The agent auto-remediates again. This happens six times in one day. How do you detect and handle this?**
A: This is the **symptom-vs-root-cause trap**. The agent is fixing the symptom (OOMKilled pod, so it restarts it) but never addressing the underlying cause (memory leak in version 2.5.1). Three layers of detection and response. Detection: count the number of auto-remediated sessions for the same service and failure pattern within a 24-hour window. After the third recurrence, escalate the next occurrence to a human with a summary of all previous sessions, the actions taken each time, and an explicit note that "this pattern has recurred N times today — likely a deeper root cause." Second layer: the RAG system should already be surfacing this pattern — by the third occurrence, the post-mortem from the first occurrence should be indexed and appearing in retrieval results with "this was previously fixed by X, but recurred." The LLM should be reasoning about why the fix did not hold. Third layer: implement a "pattern recurrence suppressor" that marks a failure pattern as "manually escalated required" after three same-day recurrences and blocks further auto-remediation for that service/pattern combination until a human explicitly clears the flag. The deeper fix requires a feedback loop where recurring remediations get flagged for root cause postmortem generation, and the resulting runbook update includes a "recurrence check" step.

**Q: Your organization is acquiring a company with a completely different monitoring stack, different incident naming conventions, and different runbook formats. How do you absorb them into this platform with minimal disruption?**
A: This is an integration architecture problem with three phases. **Phase 1 — normalization at the edge.** Add an adapter for their monitoring stack to the alert ingestion gateway. The gateway's job is already to normalize heterogeneous sources into a unified schema, so this is a new adapter, not a new architecture. The adapter maps their alert fields to the standard schema (source, severity, service_name, alert_name, labels, fingerprint). This is typically 2–4 weeks of work and can be done independently from anything else. **Phase 2 — runbook corpus migration.** Their runbooks probably cannot be executed as-is; they may reference different tool names or internal hostnames. The strategy is to use the LLM to parse their Markdown/Confluence runbooks and generate draft YAML runbooks in the standard schema, with parameters flagged as requiring validation. Human review is mandatory before any migrated runbook goes into the active corpus. Track coverage separately — "acquired runbooks: 300 total, 150 migrated and validated, 150 pending" — and do not allow auto-execution for incidents matched to pending runbooks. **Phase 3 — dependency graph reconciliation.** Their services need to be added to the service dependency graph for proper alert correlation. Initially, run correlation for their services on time-window only (no dependency graph links) to avoid false correlations. Build the graph incrementally as their service mesh is federated or as manual mappings are provided. The entire migration can take 3–6 months, but you can deliver Phase 1 value (their alerts appear in the unified dashboard and get basic LLM triage) in the first 2 weeks.

**Q: Your on-call SREs complain that the AI always proposes "restart the pod" for memory pressure incidents even when the actual issue is a memory leak in the application code, and they are tired of approving actions they know will not hold. How do you fix this?**
A: This is a **retrieval quality and feedback loop problem**, not a prompt engineering problem. Four root causes to investigate and address. First, check the runbook corpus — if the "High Memory Usage" runbook says "Step 1: restart the pod" and has a high success_rate, the retrieval system is doing its job correctly. The problem is the runbook is incomplete: it should include a "check restart count over last 24 hours — if > 2, escalate for code review rather than restart." Update the runbook corpus with this logic. Second, implement the recurrence detection described above — the system should automatically identify that this service has had the same memory pressure pattern three times this week and change its recommended action from "restart pod" to "escalate for root cause investigation." Third, add a feedback mechanism for SREs to tag diagnoses as "correct diagnosis but wrong action" separately from "wrong diagnosis." This creates a distinct training signal for the action selection component, not just the diagnosis component. Fourth, for the specific case of memory pressure incidents, add a business rule: if kubectl_delete_pod has been executed on this service for the same alert type within the last 24 hours, require human approval even if confidence would otherwise allow auto-execution.

**Q: A zero-day exploit causes a rapid cascade failure across 40% of your services simultaneously. The AIOps platform's correlation engine groups this into 50 separate incidents. The agent loop tries to remediate all 50. The remediations interfere with each other. How do you prevent this from making the situation worse?**
A: This is the most dangerous scenario the system can face, and the design must have multiple layers of protection. **Layer 1: Global blast radius cap.** Before executing any action, check the total percentage of services affected by active auto-remediation sessions. If more than 15% of all services are in active remediation simultaneously, pause new auto-remediations and escalate all pending incidents to human. One human incident commander can then coordinate across the blast. **Layer 2: Cross-session dependency awareness.** The rate limiter must be globally scoped, not just per-service. If service A and service B are both undergoing remediation and A calls B (from the dependency graph), the system should serialize the remediations or at minimum alert the operator about the interaction risk. **Layer 3: Recognize the "simultaneous multi-service incident" pattern.** The correlation engine should have a detector: if more than 10 incidents with no shared dependency chain fire within 60 seconds, emit a "global incident" signal. In global incident mode, all auto-remediations pause and the system generates a single joint escalation summary with all 50 incidents and proposed remediations listed, for a single human incident commander to review. **Layer 4: Read-only mode.** Every system should have a flag that shifts the agent into read-only mode — it continues to gather data, run diagnoses, and provide recommendations, but executes nothing autonomously. In a zero-day scenario, an operator can flip this flag globally and switch to a pure advisory mode while humans take manual control.

---

## STEP 7 — MNEMONICS AND OPENING ONE-LINER

**Mnemonic for the safety gate sequence (every interview answer about safety):**

**SRBRH** — "SRBs Run Hot"

- **S**tatic rules (namespace blocklist, action allowlist, scale bounds)
- **R**eal-time checks (healthy replica count, ongoing deployment flag, cluster capacity)
- **B**last radius calculation (users_affected_pct, requests_affected_pct, 20% threshold)
- **R**ate limiting (per-service, per-action-type, hourly window)
- **H**uman gate (for high-risk actions, regardless of confidence)

Every time you describe how the agent avoids causing harm, walk through these five layers in order. It demonstrates systematic thinking.

**Mnemonic for the confidence ensemble:**

**LRMH** — "LLMs Rarely Make History"

- **L**LM self-reported (weight 0.30)
- **R**etrieval quality — avg cosine similarity top-3 chunks (weight 0.30)
- **M**etric clarity — anomaly score, > 3 sigma = higher confidence (weight 0.20)
- **H**istorical accuracy for this service/pattern (weight 0.20)

Then apply Platt scaling calibration. Whenever someone asks "how do you score confidence," say LRMH and the calibration step.

**Opening one-liner for the interview:**

"Agentic AI in infrastructure is about giving a language model real tools to act on production systems — kubectl, cloud APIs, SSH — and then building enough layered safety guarantees that the inevitable model error cannot cause an outage. The architecture is three things: a ReAct loop that grounds reasoning in real observations, a multi-layer safety gate that blocks unsafe actions deterministically, and a calibrated confidence routing system that knows when to escalate instead of act."

This one-liner establishes that you understand the core tension (power vs safety), the core pattern (ReAct), and the core mechanism (calibrated routing). It signals that you have thought about this from a systems perspective, not just "add some guardrails."

---

## STEP 8 — CRITIQUE

### What the source material covers well

The source files are thorough on the technical implementation depth — the ReAct loop Python code, the confidence ensemble formula with exact weights, the SQL schema with all columns, the blast radius formula, the Flink windowing configuration, the IVFFlat index configuration, the feature engineering table for predictive autoscaling, the MIP formulation for capacity planning cost optimization, and the three-phase model validation pipeline. For anyone who works through all six source documents, there is enough detail to answer nearly any deep-dive question a senior interviewer would ask.

The cross-system design consistency is strong. The shared components (Kafka, pgvector, Redis, confidence tiers, audit trail) are documented consistently, which means you can explain why each technology was chosen once and apply the explanation across all six problems.

### What is missing, shallow, or potentially misleading

**Multi-region and global failure handling** is mentioned briefly (the AIOps platform runs a global correlation layer) but the design of multi-region agent coordination is not fully worked out. If the AIOps platform's global correlation layer is in us-east-1 and that region has an outage, who coordinates incidents? The source files do not give a clear answer. In a real interview, if someone asks "walk me through your multi-region disaster recovery for the AIOps platform itself," you would need to improvise based on general distributed systems knowledge.

**Fine-tuning vs RAG boundary** is addressed in the AIOps document (RAG and fine-tuning serve different purposes) but only briefly. In a Staff+ interview, you might be pushed on "when would you fine-tune the LLM instead of or in addition to RAG?" The answer (fine-tuning for reasoning style and output format, RAG for current specific knowledge) is not fully developed in the source material.

**Quota and back-pressure management for LLM APIs** gets a brief mention (circuit breaker on the client) but no design for graceful back-pressure. In practice, managed LLM APIs have rate limits measured in tokens per minute. At 14 calls per second peak in AIOps, you need either a request queue with rate limiting logic or a self-hosted inference cluster. This design gap would be noticed by a principal engineer who has shipped LLM-at-scale systems.

**Cost attribution and chargeback** is completely absent. For a system that runs LLM inference on behalf of hundreds of teams, who pays for the inference cost when Team A's services generate 30% of all incidents? This is a real operational concern in large organizations, and an interviewer from a finance-conscious company might probe it.

**Testing and simulation environments** are not discussed. How do you test that the agent's safety gate correctly blocks a dangerous action without running it in production? You need a test harness with mock infrastructure APIs that can simulate specific system states. The absence of this from the source material means you should think through your answer independently.

### Senior probes to be ready for

- "What if your Kafka cluster falls over during a major outage?" (you lose alert ingestion; your monitoring is blind; your AIOps platform cannot operate — what is the graceful degradation path?)
- "How do you version control the LLM prompts?" (prompt changes are effectively code changes; they need to be in version control, tested against a regression suite of known incidents, and rolled back if they degrade accuracy)
- "What is your strategy for handling incidents in services that are not yet instrumented?" (the dependency graph has gaps; services that emit no metrics generate no alerts; the AI cannot see what it cannot observe)
- "How do you prove to your security team that the agent cannot access secrets stored in Kubernetes?" (RBAC on the service account; the agent's service account has no rights to Kubernetes secrets, only to pods, deployments, and configmaps; network policy prevents lateral movement)

### Common traps in this pattern

**Trap 1:** Designing the agent as a one-shot system that generates a complete plan upfront and executes it without intermediate observations. This is "plan-then-execute," not ReAct. It is simpler to describe but breaks on novel failures because the initial plan is based on incomplete information. Make sure you articulate "the agent re-reasons after each observation" explicitly.

**Trap 2:** Using LLM self-confidence as the sole routing criterion. If you say "when the LLM is 90% confident, we auto-execute," you will be pushed back on immediately. LLMs overstate confidence. Always say "ensemble confidence combining LLM self-report, retrieval quality, and historical accuracy."

**Trap 3:** Not distinguishing between the auto-remediation system and the runbook executor. They are different: the auto-remediation agent plans freely and adapts in real time; the runbook executor follows a pre-defined workflow. If you describe them the same way, you have missed a key design distinction.

**Trap 4:** Forgetting the audit trail requirement. If you get deep into technical components and never mention that every AI action on production infrastructure must be logged immutably, the interviewer will flag it. It is not a nice-to-have; it is a requirement that influences your storage design.

**Trap 5:** Treating the capacity planner the same as the real-time systems. It is a batch planning system that uses classical ML forecasting and mixed-integer programming, not a real-time ReAct agent. If you try to fit it into the same OBSERVE-REASON-ACT-VERIFY loop framing, you will waste time defending a framing that does not quite fit.

---

*End of INTERVIEW_GUIDE.md — Infra Pattern 13: Agentic AI in Infrastructure*
