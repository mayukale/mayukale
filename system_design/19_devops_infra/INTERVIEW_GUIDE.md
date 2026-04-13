# Pattern 19: DevOps & Infra — Interview Study Guide

Reading Pattern 19: DevOps & Infra — 4 problems, 7 shared components

---

## STEP 1 — ORIENTATION

This pattern covers four problems that sit at the intersection of software delivery, runtime configuration, and experimentation infrastructure. They are the systems that make it possible for engineering organizations to ship code safely, control what users see, tune application behavior without redeployment, and measure the impact of changes scientifically.

**The four problems:**
1. **CI/CD Pipeline** — automate the path from a git commit to production
2. **Feature Flag Service** — control feature exposure at runtime without redeployment
3. **A/B Testing Platform** — run statistically valid experiments on user behavior
4. **Config Management Service** — store, version, and hot-reload application configuration

**Why these four cluster together:** They all share the same underlying architecture pattern: a management plane that accepts writes, a Kafka event bus that decouples that write path from downstream consumers, a Redis cache that serves the fast-read path, an SSE delivery mechanism that pushes changes to clients, and an immutable audit log. Once you understand that pattern deeply, you can adapt it to all four problems by changing the domain objects and the specific deep dives.

**What FAANG interviewers are really testing:** These problems let interviewers probe whether you understand the difference between a control plane (low-throughput, humans writing config) and a data plane (extremely high-throughput, SDKs evaluating config). Getting that split wrong — putting evaluation on the network path — is the single most common failure mode in these interviews.

---

## STEP 2 — MENTAL MODEL

**Core idea:** Every system in this pattern follows the same split: a slow, human-driven **management plane** (hundreds of writes per day) and a fast, machine-driven **data plane** (millions to trillions of operations per day). The entire design challenge is making the data plane fast enough that it never calls the management plane on the hot path.

**Real-world analogy:** Think of a TV broadcast system. The station (management plane) prepares and schedules content. The broadcast tower (delivery plane) transmits that content to millions of TVs simultaneously. Your TV does not call the station on every channel change — it receives a cached signal. If the station changes the schedule, it sends a signal update, and every TV updates its guide. The station being momentarily unavailable does not stop your TV from showing what it was already showing.

This is exactly how a feature flag service works. The SDK (your TV) holds a complete local copy of all flag rules. Evaluating a flag (changing the channel) is an in-process operation against that cached copy — sub-millisecond, no network call. When a flag changes, the SSE channel (the broadcast signal) pushes a delta to all connected SDKs. If the flag service goes down, SDKs keep serving from cache, exactly as your TV keeps showing the last channel.

**Why this is hard:**

First, the **scale asymmetry** is enormous. You might have 1,000 flag changes per day but 1 trillion flag evaluations per day. That is a 12-order-of-magnitude difference. Any design that puts the management plane on the evaluation path fails immediately.

Second, **change propagation** is genuinely hard to get right. After a flag changes, every one of 50,000 SDK instances needs to hear about it within 5 seconds. That is 50,000 persistent SSE connections, all of which need to receive the delta before the window closes. Maintaining connection state at that scale, handling reconnects without missing updates, and providing a kill-switch fast lane that bypasses the normal pipeline — these are all non-trivial engineering problems.

Third, **consistency vs. availability** cuts in a non-obvious direction here. You actually want SDKs to continue serving stale-but-functional flag values when the flag service is down (availability), but you want flag *changes* to be strongly consistent so that two SDK instances never serve conflicting rules to the same user. This is a read-your-own-writes problem that most candidates don't explicitly reason about.

Fourth, **audit, compliance, and safety** requirements mean that the systems have operational weight beyond the happy path. Every flag change, every config write, every pipeline approval needs to be immutably logged with before/after state and actor identity. This is not optional window dressing; it is often the first thing a security auditor asks about.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing a single box, ask these:

**Question 1: "Who are the writers and who are the readers, and what are their respective throughput and latency requirements?"**
This is the most important question. Writers (developers, release engineers, PMs) generate hundreds of changes per day. Readers (SDKs, build agents, experiment systems) generate millions to trillions of operations per day. What changes: if the interviewer says latency for reads needs to be sub-millisecond, you are firmly in in-process SDK territory. If they say reads can tolerate 5ms, you have more flexibility.

**Question 2: "Is this a multi-tenant SaaS platform, a single-org internal tool, or a product built for one company's use?"**
Multi-tenant changes everything: per-org data isolation, per-org SSE connection scoping, per-org Kafka partitioning, per-org rate limiting. Single-org tools can skip most of the isolation complexity. Red flag: if the candidate jumps straight into a design without asking this, they will likely produce a single-tenant design and then struggle to retrofit isolation.

**Question 3: "What are the availability and propagation SLAs?"**
For feature flags: is the target 99.99% (requires SDK in-process caching with outage tolerance) or 99.9%? Is the propagation SLA 5 seconds or 1 second? This drives whether you need a kill switch fast lane separate from the normal update pipeline.

**Question 4: "Do clients pull updates on a schedule, or do you need the server to push changes proactively?"**
This drives the choice between polling (simple, works everywhere), SSE (server-push, single-direction, HTTP/1.1 compatible), and WebSocket (full-duplex, heavier). Mobile clients typically can't sustain SSE connections due to battery constraints, so they need a polling fallback. The answer changes the delivery plane design substantially.

**Question 5: "Are there compliance or security constraints around secrets, audit trails, or data residency?"**
This drives whether you need secrets management integration (vault:// references), S3 Object Lock for audit log immutability, data residency per-org, and encryption at rest for sensitive config values.

**What changes based on answers:**
- Multi-tenant → Kafka partitioned by org_id, Redis namespaced by org × env, per-org concurrency limits
- Sub-millisecond evaluation → in-process SDK with full ruleset download; no network call on hot path
- Mobile clients → polling fallback with ETag-based 304 Not Modified instead of SSE
- Secrets in config → vault:// URI resolution, encrypted_value BYTEA column, masking in audit logs
- Compliance → S3 Object Lock, 7-year audit retention, before/after state capture on every write

**Red flags that signal a weak candidate:**
- Designing a remote-call API for flag evaluation without the interviewer asking for it
- Forgetting the audit log entirely
- Using WebSocket when SSE is sufficient (over-engineered for a unidirectional push)
- Not distinguishing between the management API (human-scale writes) and the SDK API (machine-scale reads)

---

### 3b. Functional Requirements

**Core capabilities (must have for a passing design):**
1. Create, update, and delete the domain object (flag, config key, experiment, pipeline)
2. Deliver the current state to all clients within the propagation SLA after any change
3. Clients can evaluate/read the state in-process without a network call (for server-side use cases)
4. Kill switch or emergency override that propagates faster than normal updates
5. Full audit trail: who changed what, when, before/after values
6. Rollback: restore a previous version, either one-click or automatic

**Scope statement (useful to say explicitly in the interview):**
"I'll treat X as in-scope: the management API, the delivery plane, the SDK contract, and the operational plumbing (Kafka, Redis, SSE). I'll treat Y as out-of-scope: the secret vault implementation itself, container registry hosting, statistical analysis engines for unrelated use cases, and full identity management — I'll assume an external IdP provides JWTs."

**Clear requirement statements:**
- "A flag change committed to PostgreSQL must be visible to all connected server-side SDK instances within 5 seconds."
- "A kill switch toggle must propagate within 1 second via a dedicated priority channel."
- "Flag evaluation for server-side SDKs must complete in under 1ms with no network I/O."
- "Every mutation must produce an immutable audit record with actor, timestamp, before-state, and after-state."

---

### 3c. Non-Functional Requirements

**Derive NFRs from the problem, don't just list buzzwords:**

**Availability** derives from the propagation model. If SDKs cache flag rules locally and continue evaluating from cache during outages, the SDK evaluation availability can be 99.999%. The management API is lower — 99.9% is acceptable because humans are not calling it at millions of RPS. State this distinction explicitly.

**Throughput** breaks into two very different numbers: management plane throughput (hundreds to thousands of writes per day — effectively negligible from a systems perspective) and data plane throughput (millions of flag evaluations per second, but served in-process from SDK cache, so not hitting your servers). Only the delivery plane (SSE connections, SDK init downloads) hits your servers, and that is in the tens of thousands of concurrent connections.

**Latency** is layered: < 0.1ms for in-process flag evaluation, < 500ms for SDK initialization (first load of full snapshot), < 5s end-to-end for change propagation.

**Trade-offs to mention unprompted:**
- ✅ In-process evaluation eliminates network latency and decouples availability from the flag service. ❌ It means SDKs run slightly stale rules (up to 5 seconds lag) and require a local memory footprint proportional to the ruleset size (50 MB for large flag sets).
- ✅ SSE gives near-instant push delivery without the overhead of WebSocket. ❌ It requires maintaining 50,000+ persistent HTTP connections, which demands stateful server infrastructure and careful connection draining during deployments.
- ✅ Kafka decouples the management write path from all delivery consumers; a slow audit processor does not block a flag write. ❌ Kafka adds operational complexity (partition management, consumer group lag monitoring) and is another service that can fall behind during bursts.
- ✅ ETag-based polling gives mobile SDKs update delivery without battery-draining persistent connections. ❌ Mobile clients experience up to 30 seconds of staleness (polling interval) instead of near-instant SSE delivery.

---

### 3d. Capacity Estimation

**The formula that applies to all four problems:**

```
Management plane load = (writes per day) / 86,400 seconds
Delivery plane load   = (SDK instances) × (reconnects per day) × (snapshot size)
Data plane load       = (evaluations per day) — served IN-PROCESS, zero server load
SSE connections       = (number of SDK instances) — persistent, stateful
```

**Anchor numbers to memorize:**

For CI/CD:
- 500,000 pipeline triggers/day → ~6 triggers/sec average, ~18 triggers/sec at 3x peak
- 6,000,000 jobs/day → 37,500 concurrent jobs at peak (6M jobs × 3 min average / 1,440 min/day × 3x peak)
- 54 TB/day of build log data (6M jobs × 9 MB/job), compressed to ~4 PB/year

For Feature Flags:
- 10 billion evaluations/day → 115,000/sec average — but **all served in-process**; zero server hits
- 50,000 persistent SSE connections sustained
- 50 MB MessagePack snapshot per org × environment
- ~1,000 flag changes/day across all orgs → ~0.01 writes/sec (management API is negligible)

For A/B Testing:
- 165 million events/day (exposures + conversions) → ~1,900 events/sec average
- Statistical computation triggered every 5 minutes per running experiment
- ClickHouse for raw events; PostgreSQL for pre-computed results served to the dashboard

For Config Management:
- 100,000 application instances × 10 restarts/day = 1M initial config loads/day → ~12 loads/sec average
- 100,000 persistent SSE connections
- Config writes are ~0.01/sec — management API load is negligible

**Architecture implications to state:**

"Because flag evaluations are in-process and never hit the server, I don't need to size the evaluation API for 115,000 RPS. The server only sees the 50,000 SSE connections and the ~12 SDK initializations per second. That dramatically reduces the server footprint and makes the availability argument for in-process evaluation very strong."

"For CI/CD log ingestion, 625 MB/s average (peaking at 1.9 GB/s) means I need object storage (S3) with multi-part upload from agents — I cannot use a relational DB for this volume."

**Time box:** In a 45-minute interview, spend 5-7 minutes on estimation. Get the order of magnitude right, state the critical insight (data plane load is in-process, not server-side), and move on. Do not spend 15 minutes on arithmetic.

---

### 3e. High-Level Design

**The canonical 5-component architecture that applies to all four problems:**

```
[Writers: humans via Management UI/API]
        |
        v
[Management API] — validates, writes to [PostgreSQL (source of truth)]
        |
        v on write: publish change event
[Kafka Event Bus] — partitioned by org_id
        |
   +----+----+
   |         |
   v         v
[Snapshot   [SSE Streaming Server]
 Builder]       |
   |            | push delta to connected SDKs
   v            v
[Redis Cache]  [SDK instances]
 (fast read      (in-process cache
  at SDK init)    + hot reload)
        |
[Polling Endpoint] — ETag-based 304 fallback for mobile
```

**The 4-6 key components and what they do:**

1. **Management API**: The human-facing write path. Validates business rules (schema validation, cycle detection in prerequisite flags, sum-to-100 variant weights). Writes to PostgreSQL transactionally. Publishes a change event to Kafka only after the PostgreSQL commit succeeds — never before, to prevent phantom deliveries.

2. **Kafka Event Bus**: The spine of the delivery plane. Partitioned by org_id so that a burst of changes from one organization does not delay delivery for others. Replication factor of 3 for durability. Enables multiple independent consumers (snapshot builder, SSE server, audit processor, K8s ConfigMap sync operator) to react to the same change without being coupled.

3. **Snapshot Builder + Redis Cache**: Consumes change events from Kafka, rebuilds the full environment snapshot (all flags for one org × environment, serialized as MessagePack), and writes to Redis. SDKs download this snapshot at startup. The ETag (SHA-256 of snapshot content) is stored in Redis alongside the snapshot for conditional GET (304 Not Modified) support.

4. **SSE Streaming Server**: Maintains persistent Server-Sent Events connections with all SDK instances. When a Kafka consumer receives a change event, the SSE server pushes a delta payload (only the changed flags, not the full snapshot) to all connections in the affected org × environment scope. Kill switch updates are routed through a dedicated high-priority Kafka topic and a separate SSE broadcaster to meet the 1-second SLA.

5. **PostgreSQL**: Source of truth for all domain objects. Chosen because the write volume is low (< 1 write/sec), the data model has rich relational structure (flag rules reference segments, experiments reference metrics, jobs reference agents), and the ACID transaction model is needed for version management (every write atomically increments the version number and inserts an audit record).

6. **In-Process SDK**: The most important component for interviews because it is where the key architectural choice lives. The SDK downloads the full ruleset at startup, evaluates entirely in-process (sub-millisecond, no network), maintains an SSE connection in a background thread for updates, and persists the snapshot to local disk so it can start from cache if the flag service is temporarily unavailable.

**Data flow for the most important use case (say this out loud in the interview):**

For Feature Flags: "A developer changes a flag rule in the UI. The Management API validates the rule, writes the new version to PostgreSQL, and publishes a FlagChangedEvent to Kafka. The Config Snapshot Service consumes the event, rebuilds the environment snapshot, and updates Redis. The SSE Server pushes a delta to all 50,000 connected SDK instances. Each SDK receives the delta, acquires a write lock on its in-memory ruleset, applies the update, and releases the lock. Within 5 seconds, all servers serving user traffic are evaluating against the new rule — without any of them restarting."

**Whiteboard order:** Start with PostgreSQL and the Management API (the write path), then draw Kafka, then the SSE server and snapshot builder as Kafka consumers, then the SDK as the terminal receiver. Add Redis between the snapshot builder and SDK init path. Add the polling fallback last. This order tells the story logically and shows you understand the data flow before the components.

---

### 3f. Deep Dive Areas

**Deep dive 1: In-Process SDK Evaluation Engine (Feature Flags and A/B Testing)**

The problem: You need to evaluate a flag for a given user in under 1ms against potentially hundreds of targeting rules with complex conditions (regex, semantic version comparison, set membership). The same user must always get the same variant for percentage-based rollouts.

The solution has three parts:

First, **rule evaluation order**: check if the flag is enabled (global kill switch); then evaluate prerequisite flags recursively (with cycle detection); then evaluate targeting rules in priority order (first match wins); then evaluate percentage rollout rules; finally return the default variant. Rules are pre-sorted by priority at snapshot load time so evaluation is O(n) without any sorting overhead.

Second, **percentage bucketing with MurmurHash3**: `bucket = MurmurHash3(seed + "." + flag_key + "." + user_key) % 10000`. The bucket is in the range [0, 10000). For a 10% rollout, users with bucket < 1000 get the treatment variant. Crucially, the bucket is deterministic — the same user always gets the same bucket for the same flag — so users don't flip between variants as the rollout percentage increases. At 5%, bucket < 500 gets treatment. At 10%, bucket < 1000 gets treatment. Users in 0-499 who were already seeing treatment remain in treatment at 10%. No user who was in treatment falls out.

The seed is included in the hash to ensure that different flags produce statistically independent bucket assignments for the same user. If you used only user_key as the hash input, all 10% rollouts across all flags would assign the same users to treatment, creating correlation.

Third, **atomic ruleset updates**: the SDK holds the ruleset under a read-write lock (RWLock). Evaluations take a read lock (many concurrent evaluations allowed). When an SSE update arrives, the SDK takes a write lock, swaps the ruleset atomically, and releases the lock. In-flight evaluations under the old ruleset complete normally; new evaluations after the lock swap use the new ruleset.

**Trade-offs to mention unprompted:**
- ✅ MurmurHash3 is sub-microsecond, non-cryptographic, and produces uniform distribution. ❌ Not collision-resistant, but we don't need collision resistance — we need uniform distribution. MD5/SHA-256 are ~10x slower with no benefit here.
- ✅ RWLock allows unlimited concurrent reads with only brief write pauses for updates. ❌ In languages without native RWLock (JavaScript single-threaded runtime), a different concurrency model is needed (immutable snapshot swap rather than lock).

---

**Deep dive 2: Job Scheduling and Atomic Dispatch (CI/CD)**

The problem: At peak, 37,500 jobs must be dispatched to the correct agents (matching capability labels like "docker", "macos", "gpu") without any job being dispatched to two agents simultaneously, even if the controller crashes mid-dispatch or agents disappear due to spot instance preemption.

The solution uses a Redis sorted set (ZSET) as the job queue, with a Lua script for atomic dispatch.

Jobs are stored in `ZSET queue:jobs:org:{org_id}` with a priority score of `urgency_level × 1000 - queued_duration_seconds`. The negative time term causes low-priority jobs to age up in priority the longer they wait, preventing starvation.

When an agent polls for work, a Lua script atomically: (1) scans the top 100 jobs by score with ZREVRANGE, (2) checks each job's required labels against the agent's capability labels, (3) removes the matching job from the ZSET with ZREM, and (4) records the assignment in a hash with a visibility deadline. Because Lua scripts are executed atomically on the Redis server, there is no race condition — no two agents can pop the same job.

A reaper process runs every 10 seconds, scanning the assigned jobs hash for entries where `deadline < now`. These represent jobs assigned to agents that have stopped heartbeating. The reaper re-enqueues these jobs (incrementing `retry_count`) and alerts if `retry_count > max_retries`, which indicates a persistently failing job or environment.

**Trade-offs to mention unprompted:**
- ✅ Lua atomicity avoids distributed locks, which add latency and have their own failure modes. ❌ The Lua script scans up to 100 candidates per agent call; if the label space is very sparse (e.g., very few GPU agents), this scan may miss matching jobs. Mitigation: separate queues per capability label combination.
- ✅ The reaper provides durability against agent crashes without requiring agents to ACK explicitly. ❌ If the reaper itself crashes or lags, jobs can remain stuck in the assigned state. Mitigation: the reaper is a simple stateless process with no local state; it can be restarted trivially and will re-process the entire assigned hash.
- ✅ Agents using the pull model (heartbeat + job assignment) are self-describing in their availability; the controller does not need to track which agents are currently idle. ❌ Pull introduces latency proportional to the heartbeat interval. Mitigated with long-polling: agents hold the heartbeat HTTP request open for 60 seconds, and the controller responds as soon as a matching job is available.

---

**Deep dive 3: Statistical Validity in A/B Testing (A/B Platform)**

The problem: If you compute a p-value every 5 minutes and stop the experiment when it crosses 0.05, you inflate the Type I error rate far above 5%. This is the "peeking problem." A naive platform will report a 20% false positive rate instead of the intended 5%. Product managers who peek at experiments and stop them early will launch features that have no real effect.

The solution is **mSPRT (Mixture Sequential Probability Ratio Test)**. Instead of comparing a p-value against a fixed threshold, mSPRT maintains a log-statistic that can be evaluated at any time. The statistic is:

```
log_stat = 0.5 × log(V / (V + τ²)) + (τ² × δ̂²) / (2V × (V + τ²))
```

Where V is the sample variance of the metric difference, τ is a prior standard deviation (hyperparameter), and δ̂ is the estimated treatment effect. The experiment can be stopped when `log_stat > log(1/α)` — for α=0.05, that threshold is approximately 3.0. This is mathematically proven to control the Type I error rate at α regardless of how many times you peek.

A complementary technique is **CUPED (Controlled-experiment Using Pre-Experiment Data)**, which reduces variance (and therefore required sample size) by 20-40%. The idea: subtract a covariate that explains some of the variance in the outcome metric. If you know a user's prior 7-day conversion rate, you can compute `Y_cuped = Y - θ × (X - mean_X)` where X is the pre-experiment covariate and θ = Cov(Y, X) / Var(X). The CUPED-adjusted metric has the same mean as the original but lower variance, meaning you reach statistical significance with fewer users.

**Sample Ratio Mismatch (SRM) detection** is a safety check that should run before any statistical conclusions are trusted. If an experiment is supposed to be 50/50 but the actual split is 52/48, something is wrong with the randomization (SDK bug, bot traffic, partial rollout). A chi-squared test on the observed assignment counts vs. expected counts detects this. Any experiment with SRM should be flagged as potentially invalid before results are shown to stakeholders.

**Trade-offs to mention unprompted:**
- ✅ mSPRT enables valid early stopping, saving days of experiment runtime when an effect is clear. ❌ mSPRT is less powerful than a fixed-horizon t-test at a given sample size — it requires slightly more data to reach the same significance level in exchange for the flexibility to stop early.
- ✅ CUPED is essentially free statistical power — you use data you already have (pre-experiment user history) to reduce variance without collecting more data. ❌ CUPED requires that the covariate is available for all users in the experiment; if pre-experiment data is sparse (new users), the variance reduction benefit diminishes.
- ✅ Bonferroni correction prevents false discoveries when tracking multiple metrics simultaneously. ❌ It is conservative — it increases the required sample size for each individual metric. For experiments tracking 10 metrics, each metric needs to cross α/10 = 0.005 to be declared significant.

---

### 3g. Failure Scenarios

**Failure 1: The flag service goes down entirely during a high-traffic event.**
This is the scenario the in-process SDK was designed for. Server-side SDKs continue evaluating from their in-memory cache indefinitely. New SDK instances starting during the outage fall back to the snapshot persisted to local disk on the previous successful startup. Client-side applications that call the evaluate API see failures; they should be designed to fall back to the default variant with graceful degradation. The management API is unavailable, so no flag changes can be made during the outage — but for a read service, this is acceptable. The key question the interviewer will follow up with: "How do you prevent cache drift if the outage lasts hours?" Answer: SDKs track the age of their cached snapshot and emit a staleness metric; after a configurable threshold (e.g., 30 minutes), they can fall back to all-default behavior if configured to do so for compliance reasons.

**Failure 2: The Kafka cluster loses a partition during CI/CD webhook processing.**
Webhooks are acknowledged to the Git provider with 202 Accepted only after the event is durably written to Kafka. If the Kafka write fails, the webhook gateway returns 5xx, and the Git provider retries the webhook delivery (GitHub retries up to 3 times over 30 minutes). The pipeline controller is idempotent: if it receives a duplicate webhook event, it checks for an existing pipeline run with the same trigger_sha and skips creation if one exists. This deduplication must be implemented in the controller, not assumed from Kafka's exactly-once delivery guarantees.

**Failure 3: A config change is pushed that causes all instances of a service to crash.**
The config SDK persists the last-known-good snapshot to local disk on every successful startup. If a new snapshot causes a crash (e.g., a config value that fails schema validation at runtime due to an edge case the schema didn't catch), the process exits, the supervisor restarts it, and the SDK detects the crash loop. After a configurable number of crash-loop restarts, the SDK falls back to the disk-persisted snapshot rather than fetching the new one from the server. This breaks the crash loop. The SRE can then roll back the config change via the management API (which creates a new version, not a revert), the new snapshot propagates, and instances restart successfully.

**Senior framing for failure scenarios:** "The first question I ask about any failure mode is: does the system degrade gracefully (serving stale data, reduced functionality) or does it fail hard (returning errors to users)? For these DevOps infra systems, graceful degradation almost always means 'serve from cache' for evaluation paths and 'block writes but don't corrupt reads' for management paths. I design the happy path for performance and the failure path for correctness."

---

## STEP 4 — COMMON COMPONENTS

These seven components appear across multiple problems in this pattern. Know each one deeply — why it is used, how it is configured, and what breaks without it.

---

### PostgreSQL as Source of Truth

**Why used:** All four systems need a primary data store with ACID transactions, rich relational querying (for audit trail queries, version history lookups, DAG traversal), and JSONB support for flexible domain-specific schemas (pipeline YAML definitions, flag rules, experiment configuration). The write volume is low (< 1 write/sec for most systems), so PostgreSQL's vertical scaling ceiling is not a concern at the scale described.

**Key configuration:**
- **Immutable audit tables**: `audit_events` tables are protected at the application layer — no UPDATE or DELETE is ever executed against them. At the database layer, a Row Security Policy or a trigger can enforce this. For compliance (SOC2, GDPR), consider S3 Object Lock for archive exports.
- **`SELECT ... FOR UPDATE SKIP LOCKED`**: used by the job scheduler to atomically pop jobs from a queue without double-assignment. This is PostgreSQL's built-in solution for the SKIP-LOCKED pattern in job queue implementations.
- **Optimistic versioning**: every write increments a `version BIGINT` column in the same transaction. Readers check the version before applying updates. This prevents lost updates in concurrent edit scenarios.
- **JSONB columns**: store pipeline definitions, flag rule conditions, Kubernetes manifests, experiment metric configurations. GIN indexes on JSONB enable `@>` containment queries for searching by nested attribute.
- **Shard by org_id** once a single node approaches 2 TB. Citus (PostgreSQL extension) maintains SQL semantics while distributing across nodes.

**What breaks without it:** You lose atomic version management. If you use separate tables for "current value" and "version history" without a transaction, a crash between the two writes creates an inconsistent state where the current value is updated but the history record is missing. You also lose the relational integrity that allows cascade deletes, foreign key constraints, and the audit trail querying that compliance requires.

---

### Kafka as Change Event Bus

**Why used:** Kafka decouples the management write path from all delivery and processing consumers. When a flag changes, the Management API writes to PostgreSQL and publishes to Kafka. Independently, the Snapshot Builder, the SSE Server, the Audit Processor, and the K8s ConfigMap Sync Operator all consume from Kafka. None of them are in the critical path of the write. A slow audit processor does not add latency to the flag change acknowledgment. A consumer crash does not lose events — Kafka retains them until the consumer catches up.

**Key configuration:**
- **Partitioned by org_id**: ensures that all events for a given organization are ordered and consumed by the same partition consumer. This enables per-org sequential processing guarantees.
- **Replication factor 3**: tolerates 2 broker failures without data loss. For a system where flag changes need to reach SDKs, losing a Kafka message silently would mean SDKs never receive the update.
- **Retention**: at minimum 7 days, matching the system's ability to replay from the Kafka log in case a consumer falls behind. For audit purposes, events are also persisted to PostgreSQL (not retained in Kafka beyond 7 days).
- **Separate topic for kill switches**: the `flag-kill-switches` topic is consumed by a dedicated SSE broadcaster that bypasses the normal Config Snapshot Service rebuild pipeline. This is what enables the 1-second kill switch SLA.

**What breaks without it:** Without Kafka, the Management API would need to call the SSE Server, the Snapshot Builder, and the Audit Processor synchronously or via fanout HTTP. This couples their availability — if the SSE Server is restarting, the flag write would fail or block. It also removes the ability to replay events for a new consumer (e.g., when the K8s ConfigMap Sync Operator is deployed for the first time, it can replay all recent config changes from Kafka's log to bring itself up to date).

---

### Redis for Hot-Path Caching and Distributed Operations

**Why used in each system:**
- **CI/CD**: Redis ZSET stores the job priority queue. Lua scripts atomically dequeue jobs without distributed locks. Agents' last-heartbeat timestamps are stored as hash fields, enabling the reaper to detect dead agents without hitting PostgreSQL.
- **Feature Flags**: Redis stores the per-org per-environment MessagePack snapshot (e.g., `snapshot:{org_id}:{env_id}`). SDKs download this at startup instead of querying PostgreSQL directly. The ETag (SHA-256 hash of snapshot content) is stored alongside for conditional GET support.
- **Config Management**: Same snapshot pattern as Feature Flags. Small snapshots (< 10 MB) go to Redis. Large snapshots go to S3 with the S3 key stored in PostgreSQL.
- **A/B Testing**: Active experiment snapshots cached for in-process SDK evaluation. Rate limiters for the event ingest API use Redis atomic INCR.

**Key configuration:** All Redis keys for snapshots include org_id and env_id to ensure namespace isolation. TTLs are not set on critical snapshots — the Snapshot Builder actively overwrites them when a change occurs. Rate limiter keys have TTLs set to the rate window (e.g., 60 seconds for per-minute limits). Use Redis Cluster for horizontal scaling; partition by key hash.

**What breaks without it:** Without Redis, every SDK initialization would query PostgreSQL directly. At 232 concurrent initializations per second (burst), that is hundreds of complex aggregate queries per second hitting the primary PostgreSQL instance — a load it was not designed for. The snapshot pattern in Redis reduces these to simple key lookups that PostgreSQL never sees.

---

### SSE (Server-Sent Events) for Real-Time Change Propagation

**Why used:** SSE is a unidirectional push protocol over HTTP/1.1. The server sends events to the client as they occur; the client does not need to send anything back after the initial HTTP GET. This is exactly the semantics needed for delivering flag changes to SDK instances — the SDK only needs to receive updates, not send them. SSE is simpler to implement and operate than WebSocket (which is full-duplex), and unlike long-polling, SSE provides a persistent connection that eliminates the per-poll reconnect overhead.

**Key configuration:**
- **Per-org per-environment connection scoping**: the SSE server organizes connections in a map keyed by `{org_id}:{env_id}`. When a flag change arrives for org X, environment production, only the connections in that scope receive the push. This is critical — broadcasting all changes to all connections would be a privacy violation in a multi-tenant system.
- **Delta payload, not full snapshot**: the SSE payload contains only the changed flag(s), not the entire 50 MB ruleset. SDKs apply the delta to their in-memory ruleset. If they miss a delta (due to reconnect), they re-fetch the full snapshot from Redis on reconnect.
- **Heartbeat events**: the SSE server sends a heartbeat event (`:keep-alive\n\n`) every 30 seconds. This prevents NAT gateways and load balancers from timing out idle connections.
- **On reconnect**: the SDK sends its current snapshot ETag when re-establishing the SSE connection. If the ETag doesn't match the current snapshot (meaning the SDK missed updates while disconnected), the server sends the full snapshot diff.

**What breaks without it:** Without SSE, the only option is polling. At 50,000 SDK instances polling every 5 seconds, that is 10,000 HTTP requests per second hitting the polling endpoint — all returning 304 Not Modified most of the time. This is wasteful but functional. The kill switch SLA of 1 second cannot be met with a 5-second polling interval; it requires either polling every 1 second (50,000 RPS constant load) or a push mechanism. SSE provides the push without the 50,000 RPS overhead.

---

### ETag-Based Polling Fallback

**Why used:** Mobile SDKs, embedded devices, and legacy applications cannot maintain persistent SSE connections. Battery constraints, intermittent connectivity, and connection pooling limits make persistent connections impractical for these clients. The ETag-based polling fallback provides a bandwidth-efficient alternative: the SDK sends its current snapshot version in an `If-None-Match` header, and the server responds with `304 Not Modified` (no body) if the snapshot has not changed, or `200 OK` with the full new snapshot if it has.

**Key mechanism:** The ETag is the SHA-256 hash of the snapshot content. Changing any flag in the snapshot changes the hash, which changes the ETag. This makes the ETag a reliable change indicator without requiring the server to maintain per-client state about what each client has seen.

**What breaks without it:** Mobile clients would need to choose between full snapshot downloads on every poll (expensive) or maintaining per-client delta tracking on the server (complex, stateful). The ETag provides a stateless way for the server to say "nothing changed since you last asked."

---

### Immutable Append-Only Audit Log

**Why used:** Every system in this pattern can affect what users see (feature flags), how code is built and deployed (CI/CD), what configuration applications run with (config management), and which experiments users participate in (A/B testing). All of these have legal, compliance, and debugging implications. The audit log provides a forensic record of every change: who made it, when, what it was before, and what it became after.

**Key configuration:**
- Application enforces append-only: the ORM/data layer has no UPDATE or DELETE methods for audit tables. The only permitted operation is INSERT.
- `before_state JSONB` and `after_state JSONB`: storing the full domain object state before and after the change makes it possible to reconstruct exactly what a user experienced at any point in time, which is essential for debugging "why did this user get feature X when they shouldn't have?"
- `change_reason TEXT`: optional free-text field for the actor to explain the change. Critical for compliance review ("why was the circuit breaker threshold changed at 2 AM on a Friday?").
- For CI/CD, `actor_type` distinguishes between `USER`, `CI_BOT` (automated pipeline), and `SYSTEM` (auto-rollback triggered by health check failure). This context matters when auditors are reviewing deployment records.
- 7-year retention for compliance (SOC2 Type II, HIPAA). Older records are archived to S3 with Object Lock (WORM compliance).

**What breaks without it:** You lose the ability to answer "what changed and when did this bug start?" in incident investigations. You lose the evidence required for compliance audits. And without before/after state, rollback becomes guesswork rather than a deterministic operation.

---

### MurmurHash3 for Deterministic User Bucketing

**Why used:** Both the Feature Flag Service and the A/B Testing Platform need to assign users to variants deterministically (same user → same variant, every time) without storing the assignment in a database. MurmurHash3 is a non-cryptographic hash function that: (1) produces a uniform distribution across its output range, (2) is computed in under 1 microsecond for typical inputs, (3) is deterministic (given the same input, always produces the same output), and (4) is available in all major programming languages with identical output.

**The formula:**
```
hash_input = seed + "." + flag_key + "." + user_key     (for feature flags)
hash_input = experiment_id + "." + user_id              (for A/B testing)
bucket     = MurmurHash3(hash_input, signed=False) % 10000
threshold  = int(rollout_pct * 100)                     # e.g., 10% → 1000
in_rollout = bucket < threshold
```

Including `flag_key` (or `experiment_id`) in the hash input is critical for cross-experiment independence. Without it, if you have three 10% rollouts running simultaneously, the same 10% of users would always be in all three treatments. With the flag_key as salt, each flag produces an independent bucket assignment for the same user.

The modulo bias — the slight non-uniformity introduced by taking `hash_value % 10000` on a 32-bit hash — is approximately `(2^32 mod 10000) / 2^32 ≈ 0.0001%`, which is negligible for practical rollout percentages.

**What breaks without it:** If you use a cryptographic hash like SHA-256, bucketing takes ~10x longer and still works, but it is overkill for a non-security-sensitive operation. If you use a database lookup for assignment (storing user → variant in a table), you add 1-5ms of latency per evaluation and create a hot-path dependency on the database that can become a bottleneck at millions of evaluations per second.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

This is what makes each problem unique. When an interviewer asks "how is your feature flag design different from your config management design?" — this section is your answer.

---

### CI/CD Pipeline

**What is unique:** CI/CD is the only problem in this pattern where the data plane is not a sub-millisecond in-process read but an active, stateful job execution system. Build agents are ephemeral compute resources that need to be matched to jobs based on capability labels, tracked for health, and reclaimed if they die. The system's hardest problem is not read latency but **job dispatch correctness under failure** — ensuring no job is lost, no job is double-dispatched, and the system recovers automatically from agent preemption.

**Unique components that don't appear elsewhere:**
- **Webhook Gateway with HMAC validation**: the trust boundary between public Git providers and the internal system. Every other system in this pattern has an internal write API; CI/CD has to accept untrusted events from the public internet and validate them before processing.
- **Lua atomic job dequeue**: using a Redis Lua script to atomically pop-and-assign a job prevents double-dispatch without distributed locks. No other problem needs this because flag evaluation and config reads are idempotent reads, not state-mutating dispatches.
- **Canary deployment health-check feedback loop**: the Traffic Manager polls Prometheus/Datadog every 30 seconds during canary steps and automatically rolls back if error rate or p99 latency exceeds thresholds. This closed-loop control system is unique to CI/CD.
- **Deployment strategies**: blue-green (atomic load balancer swap, 2x infra cost for instant rollback), canary (Istio/Nginx Ingress weighted routing, 1.1x-1.5x infra cost, limited blast radius), rolling (sequential pod replacement, 1x cost, slowest rollback). No other problem deals with traffic-weighted deployment.
- **Test parallelization with LPT bin-packing**: uses historical test runtime data from PostgreSQL to distribute test files across agent bins minimizing the total makespan. None of the other three problems have a scheduling optimization component.

**Two-sentence differentiator:** CI/CD is the only problem where the hot path is active job execution across thousands of ephemeral compute nodes, not a passive in-process cache read. The Redis Lua atomic dequeue, spot-instance reaper, canary health-check feedback loop, and HMAC webhook validation are components that exist nowhere else in this pattern.

---

### Feature Flag Service

**What is unique:** Feature Flags is the purest expression of the management plane / data plane split. The evaluation path is entirely in-process (no network, no database), which means the service's most important design decision is about the SDK, not the server. The server's job is just to prepare and deliver the ruleset; the SDK does all the real work.

**Unique components that don't appear elsewhere:**
- **Kill switch as a first-class concept**: `flag_environments.enabled = FALSE` is the highest-priority override in the evaluation engine, checked before any rule evaluation. It propagates via a dedicated high-priority Kafka topic and SSE broadcaster to meet a 1-second SLA. Config management and A/B testing do not have a concept equivalent to a kill switch — there is no "instantly disable all reads for this config namespace."
- **Prerequisite flags**: flag B only evaluates if flag A resolved to a specific variant for this user. This enables dependency-ordered rollouts ("enable advanced checkout only if basic checkout redesign is already on for this user"). Requires DFS cycle detection in the Management API to prevent circular prerequisites.
- **Client-side evaluation endpoint**: `POST /v1/sdk/evaluate` accepts a user context and returns only variant values — not the business rules that produced them. This is a security requirement: you don't want the browser's DevTools to reveal that you're testing a new pricing model on users with `plan == "enterprise" AND country == "US"`. Config management and A/B testing don't have this client-vs-server SDK security distinction.
- **RWLock for concurrent ruleset updates**: the SDK evaluates millions of times per second while periodically receiving atomic ruleset updates. The RWLock pattern (many concurrent reads, brief exclusive write for updates) is specific to this problem's extreme read concurrency combined with periodic writes.

**Two-sentence differentiator:** Feature Flags is unique because its evaluation is entirely in-process — the server's only job is ruleset delivery, and the SDK does all the work — which means the SDK itself is the most interesting engineering component. The kill switch fast lane, prerequisite flag cycle detection, client-side evaluation endpoint (to avoid exposing business rules to browsers), and RWLock-based atomic ruleset swap are features that have no direct equivalent in the other three problems.

---

### A/B Testing Platform

**What is unique:** A/B Testing is the only problem in this pattern where correctness is a statistical property rather than a logical one. It's not enough to build a system that technically tracks who saw what variant — you need to build a system that produces statistically valid conclusions that control false positive rates, even when stakeholders peek at results continuously. The hard problems are all in the computation plane, not the delivery plane.

**Unique components that don't appear elsewhere:**
- **mSPRT sequential testing**: a mathematical framework for valid early stopping that no other system needs. Standard t-tests and z-tests are used for fixed-horizon experiments; mSPRT is used when you want to stop early without inflating the false positive rate.
- **CUPED variance reduction**: using pre-experiment user behavior as a covariate to reduce metric variance by 20-40%, effectively increasing statistical power without collecting more data. This is a pure statistical technique that has no analog in the other three problems.
- **Sample Ratio Mismatch detection**: a chi-squared test that validates whether users were actually assigned in the expected proportions. SRM indicates a bug in the randomization system or data pipeline and should invalidate the experiment's results. No other problem has a "sanity check on the assignment system itself."
- **Layers (mutual exclusion)**: a way to partition the user population so that a user can only be in one of multiple experiments that touch the same product surface. Without layers, two checkout experiments could assign the same user to conflicting variants, confounding both experiments' results.
- **ClickHouse for event storage**: raw exposure and conversion events need columnar OLAP-style aggregation (GROUP BY experiment_id, variant_key across hundreds of millions of rows). PostgreSQL is row-oriented and cannot perform this aggregation in the 5-minute result freshness window. ClickHouse's vectorized execution engine and MergeTree storage do it in seconds.
- **Guardrail metrics**: auto-stopping experiments when safety metrics (error rate, latency) exceed thresholds, regardless of the primary metric's status. Config management and Feature Flags have no concept of an automated safety interlock on the consequence of a change.

**Two-sentence differentiator:** A/B Testing is unique because its primary engineering challenge is statistical correctness rather than system performance — you need mSPRT for valid early stopping, CUPED for variance reduction, SRM detection for assignment validation, and Bonferroni correction for multiple metrics, none of which appear in any other system in this pattern. ClickHouse as the event store (columnar OLAP for massive aggregation), the mutual exclusion Layer system, and the Guardrail Monitor (automated experiment halting on safety metric breach) are also unique to this problem.

---

### Config Management Service

**What is unique:** Config Management is the most "ops-focused" of the four problems. Where Feature Flags is optimized for product team agility (launch features, roll back fast, run experiments), Config Management is optimized for operational reliability (let an SRE change a circuit breaker threshold from 0.5 to 0.3 during an incident without redeploying every service). The unique challenges are around integration with heterogeneous application architectures and the separation between non-secret config and secret config.

**Unique components that don't appear elsewhere:**
- **vault:// URI scheme and Vault Connector**: config values can contain references to secrets vault paths (`vault://secret/payments/db-password`). The SDK resolves these references at startup using an application-scoped vault token, caches the resolved secrets in memory (never persisted to disk or logs), and handles Vault lease renewal. No other problem in this pattern deals with the boundary between non-secret config and secrets management.
- **JSON Schema validation on writes**: every write to a config key is validated against a JSON Schema document attached to the namespace before being committed. This prevents `type: string` being written to a key whose consumers expect `type: integer`. Feature Flags has rule validation (condition operators, variant weights) but not declarative schema validation for arbitrary value types.
- **Sidecar process for legacy apps**: for applications that cannot import the SDK (legacy Java apps reading config from files, C programs, scripts), the sidecar process writes config updates to a mounted volume file, and the application receives an inotify notification. This deployment model has no equivalent in Feature Flags or A/B Testing.
- **Kubernetes ConfigMap Sync Operator**: syncs config service values to Kubernetes ConfigMaps and Secrets on each change event, enabling Kubernetes-native applications to consume config without the SDK. The kubelet then propagates ConfigMap updates to mounted volumes within 1-60 seconds.
- **Rollback as a new version (not a revert)**: rolling back a config key does not overwrite history. It creates a new config_version record pointing to the desired historical value. This means the complete timeline of changes is always preserved, and the rollback itself is auditable (who triggered it, at what time, with what reason).
- **Namespace hierarchy with prefix RBAC**: config keys are organized in hierarchical paths (`payments/gateway/timeout_ms`). Access policies can grant read/write to a prefix (`payments/*`), enabling team-scoped access without granting access to the entire config space. Feature Flags uses flat org-level RBAC; Config Management needs finer-grained namespace-level access control.

**Two-sentence differentiator:** Config Management is unique in its vault:// secret reference integration (the SDK resolves secrets at startup using app-scoped vault tokens, never persisting them in plaintext), its three deployment models for different application architectures (in-process SDK, file-based sidecar, Kubernetes ConfigMap operator), and its immutable version history where rollback creates a new version rather than overwriting history. JSON Schema validation on writes, hierarchical namespace RBAC, and the operational focus (SRE changing circuit breaker values during incidents) make it the most "ops-plumbing" of the four problems and the least similar to a product engineering problem.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Level (expect these in the first 15 minutes)

**Q1: "Why not just evaluate feature flags with a network call to the flag service on every request?"**

At one million requests per second, each flag evaluation adding even a 5ms network call adds 5,000 ms of cumulative per-second latency overhead per application instance. The math doesn't work. More importantly, it creates an availability dependency: if the flag service has a 30-second outage, every service that calls it per-request would also be down for those 30 seconds. By moving evaluation in-process, you keep flag evaluation available even during flag service outages, you eliminate the latency overhead entirely, and you avoid adding a network hop to every user request. The trade-off is that flag changes take a few seconds to propagate rather than being instant, which is almost always acceptable.

**Q2: "What's the difference between a feature flag and a config value?"**

Feature flags have a specific semantics: they control whether a feature is visible to a specific user based on targeting rules, percentage rollouts, and user attributes. They have evaluation logic (first-match-wins rules, percentage bucketing), a concept of variants (true/false, control/treatment), and an SDK that evaluates them client-side. Config values are arbitrary key-value pairs for application configuration — timeout durations, connection pool sizes, circuit breaker thresholds. They don't have evaluation logic; an application reads a config value and gets the same value regardless of who the current user is. The data model, SDK API, and use cases are different enough to warrant separate systems.

**Q3: "How does a canary deployment differ from a blue-green deployment, and when do you use each?"**

**Blue-green**: two identical environments; you deploy to the inactive one, validate it, and atomically swap a load balancer rule to point all traffic to it. Rollback is instant (swap back). Cost is approximately 2x the normal infra cost during the deployment window. Best for: services where even a small blast radius is unacceptable (auth services, payment APIs), or when you need near-instant rollback as the primary safety mechanism.

**Canary**: route a small percentage of traffic (e.g., 5%) to the new version, monitor error rates and latency for a period, then incrementally increase traffic if metrics are healthy. The blast radius is limited to the canary percentage. Rollback is fast (set canary weight to 0). Cost is approximately 1.1-1.5x normal. Best for: high-traffic services where you want to validate with real user traffic before full rollout, especially when synthetic tests in staging are not representative of production traffic patterns.

**Q4: "What is the peeking problem in A/B testing?"**

If you compute a p-value every hour and stop an experiment the moment the p-value drops below 0.05, you have not actually controlled the false positive rate at 5%. The p-value fluctuates due to sampling variance, and by checking it repeatedly, you give yourself multiple chances to observe a false positive. Simulations show that this naive "check and stop" approach inflates the false positive rate to 20% or higher. The correct solution is either to: (a) pre-commit to a fixed experiment duration determined by a power analysis and only check results at the end, or (b) use a sequential testing method like mSPRT that is mathematically designed to control the false positive rate even with continuous monitoring.

**Q5: "Why use Kafka between the Management API and the delivery components? Why not call them directly?"**

Three reasons. First, availability: if the SSE Server is restarting when a flag changes, a direct HTTP call would fail and the change might not be delivered. Kafka's message is durable and the SSE Server will consume it when it comes back up. Second, fanout: there are multiple consumers of the same change event (Snapshot Builder, SSE Server, Audit Processor, K8s ConfigMap Sync Operator). Without Kafka, the Management API would need to call each of them, coupling their availability to the write path. Third, replay: when you add a new consumer (e.g., the K8s Operator being deployed for the first time), it can replay the last 7 days of change events from Kafka's log to catch up, without any special backfill logic.

**Q6: "How do you handle the case where an agent dies while running a job?"**

The agent sends a heartbeat to the Job Scheduler every 10 seconds, updating its status and the job assignment's deadline in Redis. A reaper process runs every 10 seconds, scanning all assigned jobs for entries where `deadline < now`. When an agent stops heartbeating (due to spot instance preemption, OOM kill, or network partition), the reaper detects its jobs have missed the deadline and re-enqueues them (incrementing `retry_count`). The re-enqueued job will be picked up by another healthy agent. If `retry_count` exceeds `max_retries`, the job is marked failed and an alert fires rather than re-enqueueing again, preventing an infinite retry loop for a broken job.

**Q7: "What does the audit log store, and why is that important?"**

Every write to any domain object generates an audit record with: actor ID and actor type (human user, service account, automated pipeline), event type (flag.rule.updated, config.value.changed, pipeline.deployment.approved), entity type and entity ID, timestamp, and crucially, `before_state JSONB` and `after_state JSONB` — the full domain object snapshot before and after the change. This enables: (a) answering "what changed and when did this incident start?" in postmortems, (b) proving to compliance auditors that production config changes required human approval, (c) reconstructing what configuration or flag state any user experienced at any point in time, and (d) providing a rollback target since the before_state contains the exact values you'd want to restore.

---

### Tier 2 — Deep Dive (expect these after you've laid out the HLD)

**Q1: "You said the flag change propagates via SSE within 5 seconds. What if an SDK misses the SSE update because it was reconnecting at exactly that moment?"**

This is a gap in a naive design and a good catch. The solution is: when an SDK reconnects to the SSE endpoint, it sends its current snapshot ETag in the reconnect request. The SSE server compares this ETag to the current ETag stored in Redis for that org × environment. If they differ, the server sends the full current snapshot (not just a delta), ensuring the SDK catches up to the current state regardless of how many updates it missed during the disconnection window. The guarantee is: **at most 5 seconds + reconnect time to receive any change.** There is no scenario where the SDK permanently drifts from the server state, because reconnection always triggers a full sync.

**Q2: "Why MurmurHash3 specifically for bucketing? Why not MD5 or SHA-256?"**

Three reasons. First, speed: MurmurHash3 computes in under 1 microsecond for typical inputs. MD5 takes approximately 100-150ns (similar), but SHA-256 takes around 500ns — five times slower. At 100 million evaluations per second, the difference is measurable. Second, we need **uniform distribution**, not **collision resistance**. MurmurHash3 is specifically designed for uniformity; its output bits are as independent as possible, which minimizes the modulo bias when mapping to a bucket range. Cryptographic hashes like SHA-256 provide collision resistance, which we don't need for bucketing. Third, **availability**: MurmurHash3 implementations across all major languages (Go, Python, Java, JavaScript, C++) are verified to produce identical outputs for the same input, which is critical for ensuring that server-side and client-side SDKs bucket the same user consistently.

**Q3: "How do you prevent a config service outage from taking down all services that depend on it?"**

Three layers of resilience. First, the SDK persists the last successfully loaded snapshot to **local disk** on every successful startup. If the config service is unavailable when an application starts (or restarts), the SDK loads from disk cache instead of making a failed network call. Second, the SDK holds the full config namespace **in memory**; even if the SSE connection drops and the SDK cannot receive updates for hours, the application continues running with the last-known-good config values. Third, the config service's delivery plane (Redis snapshot reads + SSE pushes) is architecturally separated from the management plane (PostgreSQL writes). A failure in the management plane (e.g., PostgreSQL primary failover) does not affect the delivery plane, which continues serving the last-committed snapshot from Redis.

**Q4: "A PM wants to stop an experiment early because the treatment looks great at day 5 of a planned 14-day run. What do you tell them?"**

Stopping early based on a traditional p-value that has crossed 0.05 is statistically invalid — it's the peeking problem. The observed p-value at day 5 may be below 0.05 by random chance; it's effectively a false positive. You have two correct paths. If the platform uses mSPRT (which it should), you can stop early with statistical validity — the mSPRT log-statistic already accounts for the multiple looks and maintains the correct false positive rate. Show the PM the mSPRT statistic rather than the raw p-value. If the platform uses fixed-horizon t-tests, explain that stopping at day 5 with a target of day 14 means the stated α=0.05 is no longer valid; the actual false positive rate could be 3-4x higher. Recommend either continuing to day 14 or migrating to mSPRT for future experiments. Document the decision regardless.

**Q5: "Your CI/CD system handles 37,500 concurrent jobs. How does the job scheduler avoid becoming a bottleneck?"**

Several mechanisms. First, the Redis ZSET job queue distributes the scheduling work — agents pull from the queue themselves using the Lua script, so the scheduler does not need to push to each agent individually. Second, the queue is sharded by `org_id`, so each organization's job queue is independent. Third, the Agent Pool Manager handles auto-scaling decisions (Kubernetes HPA or EC2 Auto Scaling triggered by queue depth metrics), which is a separate concern from job dispatch. Fourth, the Pipeline Controller (which parses YAML and creates job records) is horizontally scalable — multiple instances can be running simultaneously, each consuming from Kafka's webhook-events topic as part of a consumer group. The Kafka partition by org_id ensures that all jobs for a given org are processed by the same controller instance in order, preventing race conditions in pipeline state transitions.

**Q6: "How do you handle database schema migrations in a blue-green deployment without downtime?"**

This is the hardest part of blue-green that candidates routinely skip. The answer is the **expand/contract pattern** (also called parallel-change migration). Phase 1 (Expand): add new columns as nullable. Deploy the new version of the application code that writes to both the old and new columns simultaneously. The old version of the application is still running in the blue environment and only reads/writes the old column — no breaking change yet. Phase 2 (Migrate): run a background migration process that fills the new column from the old column for existing rows. Phase 3 (Contract): once the new column is fully populated and the blue-green swap has been made (new version is active), deploy a third release that reads only from the new column and drops the old column. This requires the schema change to take three separate deployments to complete, which is the price of zero-downtime migrations. Migration linting tools (e.g., `squawk` for PostgreSQL) can statically analyze migration scripts for backward compatibility violations before they reach production.

**Q7: "How do you ensure A/B experiment assignments are consistent across web and mobile for the same logged-in user?"**

Use the user's **authenticated user UUID** as the bucketing key, not a session cookie or device ID. A UUID is stable across sessions, devices, and app reinstalls as long as the user is logged in. The hash `MurmurHash3(experiment_id + "." + user_uuid)` produces the same bucket on web, iOS, and Android, ensuring the same variant. For unauthenticated users, this is not possible — a user switching from anonymous to logged-in mid-experiment may get a different variant after login. The conventional solution is to expose the user to the experiment only after authentication, or to track a persistent anonymous ID in a long-lived cookie that is merged with the user UUID upon login.

---

### Tier 3 — Staff+ Stress Test (these questions require you to reason aloud, not recite answers)

**Q1: "A large customer has 500,000 active feature flags across 10 environments. The 50 MB snapshot you described is now 2.5 GB. How does your design change?"**

Reason aloud: The first thing I'd question is whether a single customer with 500,000 active flags is a reasonable use case or a sign of flag misuse (using flags as a config system, never archiving old flags). I'd add a flag archival policy. But assuming this is legitimate: (1) **Lazy loading by namespace** — instead of downloading all flags at startup, the SDK downloads flags for the namespaces it will actually evaluate (declared in the SDK initialization). This reduces the download to the flags relevant to that service. (2) **Incremental bootstrap** — start evaluating with the most-frequently-evaluated flags first (based on telemetry), loading remaining flags in the background. (3) **Delta-only SSE forever** — the SDK never re-fetches the full snapshot; it downloads the initial snapshot once and applies deltas permanently. If the SSE connection drops, it requests only the deltas it missed (by sending the last snapshot version). This requires the SSE server to maintain a short changelog (last 5 minutes of deltas). (4) **Segment-specific snapshots** — for client-side evaluation, build per-user snapshots server-side and cache them at the CDN edge with a short TTL. The 2.5 GB problem disappears when you only deliver the 50-100 flags relevant to a specific user's context.

**Q2: "An A/B test shows a strong positive result (p=0.001, +20% lift) but your SRM detection found a 5% assignment imbalance. Do you ship the feature?"**

Reason aloud: No, not yet. The SRM (Sample Ratio Mismatch) is a red flag that invalidates the statistical analysis, regardless of how impressive the result looks. The 5% imbalance means the randomization was not working as designed — some users in the treatment group may have been selected non-randomly, which biases every downstream metric. The treatment group may have inadvertently selected for more engaged users, more power users, or users in a specific timezone. The +20% lift could entirely be explained by selection bias rather than a real treatment effect. The correct procedure is: (1) stop the experiment, (2) investigate the SRM root cause (bot traffic? SDK bug? network routing issue for a specific region?), (3) fix the root cause, (4) restart the experiment with a clean user pool. You should not ship a feature based on an experiment with SRM, because you cannot trust the estimated effect size.

**Q3: "The engineering org runs 50 CI/CD pipelines simultaneously on a Friday afternoon release day, and the build queue depth spikes to 50,000 pending jobs. Your build agents are at capacity. What happens, and how does your design handle it?"**

Reason aloud: First, the system should be resilient: the Kafka webhook-events topic buffers incoming pipeline triggers durably, so no triggers are lost even if the pipeline controller is backlogged. The jobs are enqueued in the Redis ZSET but not dispatched until agents are available. The Agent Pool Manager, which monitors queue depth as a metric, should have already triggered auto-scaling 10-15 minutes ago when the queue first started growing. The question is whether auto-scaling is fast enough — spinning up a new EC2 instance or ECS task takes 2-5 minutes. If the spike is sudden (everyone merges their PRs at 5 PM simultaneously), there is an inherent delay. Mitigations: (1) maintain a warm pool of pre-initialized agents so that auto-scaling adds compute in under 30 seconds rather than 2-5 minutes; (2) implement per-org concurrency limits to prevent one large organization from starving all agents; (3) apply priority aging aggressively so that PRs that were queued first get agents first, avoiding situations where a team's job has been waiting for 20 minutes while new jobs from another team get dispatched immediately. The system will eventually drain the queue as agents come online — the SLA question is whether the 30-second p99 job start latency can be maintained or whether it degrades gracefully (it will degrade gracefully — the queue just takes longer to drain).

**Q4: "You are designing the config management system and a principal engineer asks: 'Why not just use etcd or Consul? They were designed for this.'  How do you respond?"**

Reason aloud: Good question — etcd is exactly what Kubernetes uses for its own configuration, and Consul is widely used in service meshes. But for an application-level config management service at the scale described, there are three problems. First, **storage limits**: etcd is designed for < 8 GB of data; our estimates put config version history at ~500 GB across all orgs. etcd is not appropriate for this volume. Second, **query capability**: our audit log queries need `WHERE namespace_path LIKE 'payments/%' AND occurred_at > NOW() - INTERVAL '30 days' AND actor_id = X` — a complex multi-column filter that neither etcd nor Consul supports natively. We'd need a separate query index, which adds operational complexity. Third, **multi-tenancy**: etcd has no per-key RBAC that would give us per-namespace access control for 500 organizations without complex key-prefix schemes and external policy engines. PostgreSQL gives us all of this natively. That said, etcd's watch API (which is similar to our SSE delivery model) is genuinely good and an inspiration for the design. The delivery plane takes inspiration from etcd's watch; the storage layer uses PostgreSQL because etcd's storage engine is not designed for this use case.

**Q5: "How would you add support for Optimistic Locking to the feature flag rule update API, and why might a customer with a large PM team require it?"**

Reason aloud: A customer with 50 PMs all editing feature flags simultaneously could run into a lost update problem: PM A reads a flag's rules (version 41), PM B reads the same flag (version 41), PM A submits a rule update (now version 42), PM B submits a different rule update based on version 41 — their update overwrites PM A's change without PM B knowing PM A had changed anything. This is the classic optimistic locking scenario. The fix: include the current version number in the PUT request body (`"expected_version": 41`). The Management API executes an UPDATE WHERE version = 41; if the row's version is now 42 (because PM A updated it), the UPDATE affects 0 rows, and the API returns a 409 Conflict with a message like "this flag was updated by another user since you last loaded it — please reload and reapply your changes." This requires no distributed locks and works at any scale because it is a single database row condition check.

---

## STEP 7 — MNEMONICS AND OPENING LINES

### Mnemonic 1: PKSEA

Every system in Pattern 19 has the same five-component spine. Remember **PKSEA**:

- **P**ostgreSQL — source of truth, immutable audit log, ACID transactions for version management
- **K**afka — change event bus, decouples management write path from delivery consumers
- **S**napshot + Redis — fast-read cache, ETag-based conditional GET, O(1) SDK init
- **E**SE (SSE) — real-time delta push to connected SDK instances, sub-5-second propagation
- **A**udit Log — append-only, before/after state, actor + timestamp, 7-year retention

Draw these five boxes first. Then differentiate the problem by what sits on the write side (flag management API vs. webhook gateway vs. config editor vs. experiment designer) and what sits on the read side (in-process evaluation engine vs. job scheduler vs. statistical computation engine vs. hot-reload SDK).

### Mnemonic 2: The Two Planes

Before designing anything, orient the interviewer with this phrase: **"This system has two very different planes operating at very different scales."**

Control plane / Management plane: humans write at < 1 write/sec. Correctness matters most. Use PostgreSQL transactions. Audit every write.

Data plane / Evaluation plane: machines read at millions to trillions of operations per day. Latency matters most. Serve from in-process cache. Never call the management plane on the hot path.

Every design decision in this pattern flows from this distinction.

### Opening One-Liners

For Feature Flags: "The key insight is that evaluating a flag and managing a flag are different problems by 12 orders of magnitude — the evaluation plane operates at trillions per day entirely in-process, while the management plane sees hundreds of writes per day. The entire design challenge is keeping these two planes decoupled."

For CI/CD: "The core challenge is not throughput — it's correctness under failure. At 37,500 concurrent jobs, you need atomic job dispatch without double-assignment, automatic recovery from spot instance preemption, and a canary deployment system that can roll back without human intervention if error rates spike."

For Config Management: "The most dangerous thing about config management is that it sits on the startup path of every service. If it's unavailable when a service restarts, the service can't start. The entire design is built around making 'config service unavailable' a recoverable condition via local disk caching and in-memory fallback."

For A/B Testing: "The hard problems here are statistical, not distributed systems problems. The system needs to produce conclusions that control false positive rates even when PMs peek at results continuously — which requires mSPRT sequential testing, not just a t-test. Getting the statistics wrong means shipping features that have no real effect and calling it a win."

---

## STEP 8 — CRITIQUE

### What the source material covers well

The four source files provide exceptionally thorough coverage of:

1. **The in-process evaluation pattern** — the why, how, and trade-offs of serving flag evaluations from in-process SDK cache are explained with working pseudocode and interviewer Q&A. Any candidate who studies this will be able to explain the pattern clearly under pressure.

2. **Specific algorithms with working code** — the Lua atomic dequeue script, the MurmurHash3 bucketing implementation, the mSPRT formula, the CUPED variance reduction formula, and the blue-green swap pseudocode are all present with enough detail to quote specific lines in an interview when needed.

3. **Database choice rationale** — every system explains why PostgreSQL was chosen over etcd, DynamoDB, MongoDB, and Cassandra, which is a common interview follow-up.

4. **Capacity estimates with calculation chains** — anchor numbers are derived from assumptions, not pulled from thin air, making them defensible.

5. **Multi-tenant data isolation** — Kafka partitioning by org_id, Redis namespace isolation by org × env, per-org concurrency limits are consistently applied.

### What is missing, shallow, or could trip you up

**Missing: Network partition and split-brain scenarios for the Redis job queue.** The source material covers the Redis primary failure scenario for the Lua script but does not address what happens if the Redis primary fails after the Lua script commits but before the controller receives the response. The job may be dequeued and lost from the queue (not visible to other agents) but the controller doesn't know it was dispatched. The reaper's visibility deadline mechanism handles this eventually, but there is a window of time where the job is in limbo. Be ready to address this explicitly if asked.

**Missing: SSE horizontal scaling details.** The source material mentions that the SSE Server maintains connections organized by org × environment but doesn't explain how multiple SSE Server instances coordinate. In a horizontally scaled deployment, a flag change consumed by SSE Server instance A needs to be broadcast to connections on instances B, C, and D as well. The standard solution is to have all SSE server instances subscribe to the same Kafka topic and broadcast to their local connections. Each instance broadcasts to its local connections; the full fanout to all 50,000 connections happens through the collective action of all instances. This detail is not in the source material and could come up as a follow-up.

**Missing: Experiment metric carryover and novelty effects.** The A/B testing material is thorough on statistical mechanics but doesn't mention novelty effects (users behave differently when they see something new, which inflates treatment lift in the first few days before reverting to baseline) or carryover effects (users who were in a previous experiment may have modified behavior that persists into the current experiment). These are real concerns in a mature experimentation platform and could come up with a senior interviewer.

**Shallow: Multi-region deployment.** The source material is almost entirely single-region. In a real FAANG system, the flag service and config service need to operate across multiple regions for latency and availability. The key question — do you replicate flag changes to all regions synchronously (strong consistency, higher write latency) or asynchronously (eventual consistency, faster writes, brief cross-region inconsistency) — is not addressed. For feature flags, async replication with the in-process SDK providing a further caching layer is almost always the right answer; for config management where a wrong value can cause service crashes, the trade-off is more nuanced.

**Potential trap: Confusing feature flags with config management.** Interviewers sometimes ask candidates to distinguish these. Be ready to answer precisely: feature flags have evaluation logic (rules, targeting, percentage bucketing, variants) and are used to control feature exposure to specific users. Config values are uniform key-value pairs with no evaluation logic — all instances of a service read the same value. The data models, SDK APIs, and use cases are fundamentally different, even though both use the same PKSEA infrastructure pattern.

### Senior probes to be ready for

1. "How does the SSE server scale to 100,000 connections without a single node running out of file descriptors?" Answer: async I/O (Node.js event loop, Python asyncio, Go goroutines) handles thousands of concurrent connections per process. Multiple SSE server instances behind a load balancer. Each instance manages a subset of connections; all instances consume from the same Kafka topic and broadcast to their local subset. Connection limits: increase the OS-level `ulimit` for file descriptors on each instance.

2. "You use PostgreSQL for everything. At what point do you migrate away, and to what?" Answer: PostgreSQL works comfortably to about 2 TB of data per node with proper indexing. Beyond that, shard by org_id using Citus. The audit log is the highest-growth table; once it exceeds 1 TB, migrate it to TimescaleDB (a PostgreSQL extension for time-series) or archive it to S3 + Athena for compliance queries. Raw A/B testing events were correctly placed in ClickHouse from the start.

3. "A user in a feature flag experiment has their account deleted. What happens?" Answer: the SDK evaluates flags against a user_key (stable UUID). If the user is deleted, their user_key is no longer valid, but any existing evaluation events in the A/B system that reference that user_key remain (needed for statistical validity during the experiment's lifetime). Post-conclusion, GDPR right-to-erasure requires deleting those events. The platform should implement a GDPR erasure endpoint that removes all events associated with a given user_key from ClickHouse and flags any experiment results computed during the user's active period as potentially affected.

4. "What is your strategy for zero-downtime deployment of the CI/CD system itself?" Answer: the CI/CD system is subject to the same constraints as any distributed system. The pipeline controller and job scheduler are stateless (all state in PostgreSQL and Redis) and can be deployed with a rolling update — new instances come up, start consuming from Kafka, and old instances drain. The agent pool manager is also stateless. The SSE/webhook gateway is stateless. The only tricky component is the Redis job queue: during deployment, a brief window exists where both old and new controllers might dispatch the same job. The Lua script's atomic dequeue and the reaper's deadline mechanism handle this correctly — the job is dispatched once, and the reaper recovers any job whose agent disappears mid-transition.

---

*End of INTERVIEW_GUIDE.md — Pattern 19: DevOps & Infra*
