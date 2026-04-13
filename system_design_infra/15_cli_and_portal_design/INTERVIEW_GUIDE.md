# CLI & Portal Design — Complete Interview Guide

> **Scope:** This guide covers all four systems in the `15_cli_and_portal_design` cluster:
> - **CLI Client for Infrastructure Platform** (Go binary, OAuth2 device flow, offline cache)
> - **Developer Self-Service Portal** (Temporal workflows, quota enforcement, approval chains)
> - **Web Portal for IaaS** (React SPA, BFF pattern, WebSocket real-time updates)
> - **Infrastructure as Code Platform** (dependency DAG, state locking, provider plugins)
>
> This document is **completely self-contained**. You do not need to open any other file.

---

## STEP 1 — Source Internalization Summary

All four systems share a unified infrastructure platform for 10,000 engineers managing VMs, bare-metal servers, Kubernetes clusters, and storage. They are distinct surfaces for the same underlying resource model:

| System | Primary User | Core Problem |
|--------|-------------|-------------|
| CLI Client | Developer at terminal / CI pipeline | Fast, scriptable, headless access to platform API |
| Developer Self-Service Portal | Developer requesting resources | Self-service provisioning with approval + quota guards |
| Web Portal for IaaS | All roles, daily operations | Rich UI dashboard, real-time status, cost visibility |
| IaC Platform | Infra engineer / SRE | Declarative, reproducible, auditable infra provisioning |

**Shared foundations across all four:** MySQL 8.0 for transactional state, Redis for sessions and rate limiting, OAuth2/OIDC via Okta, a shared resource state machine (`pending_approval → active → terminated`), per-project quota enforcement, and a 7-year Elasticsearch audit trail.

---

## STEP 2 — Mental Model

### Core Insight

**All four systems are different "handshakes" between a human intent and a physical infrastructure action.** The CLI is the command line handshake. The self-service portal adds the workflow/approval handshake. The web portal adds the visual/real-time handshake. IaC adds the declarative/reproducible handshake. Every hard problem — quota races, provisioning failures, stale state — is fundamentally about keeping those handshakes consistent.

### Real-World Analogy

Think of a hotel chain:
- The **CLI** is a direct phone call to the reservations desk — fast, precise, no waiting room.
- The **Self-Service Portal** is an online booking system — you pick a room (template), it checks availability (quota), sometimes needs manager sign-off (approval workflow), then books it.
- The **Web Portal** is the hotel lobby app — live room status, your booking history, cost breakdown, notifications when your room is ready.
- The **IaC Platform** is a corporate travel management system — you declare "I need 3 rooms in NYC for 5 nights" as a policy; the system compares what's already booked (state file), plans what to add/cancel, and executes atomically.

### Why This Problem Is Hard

Three compounding difficulties:
1. **Concurrency + finite resources:** Multiple teams request GPU servers simultaneously. Without atomic quota reservation, you double-book physical hardware.
2. **Long-running, multi-step workflows:** Bare-metal provisioning takes 15 minutes, crosses multiple services, and can fail at any step. You need durable, resumable execution — not just a REST call.
3. **Developer UX across radically different surfaces:** The same provisioning action must work flawlessly from a CLI in a headless CI container, a browser with real-time status push, and a declarative YAML file applied by a remote engine. Each surface has completely different failure modes and consistency models.

---

## STEP 3 — Interview Framework

### 3a. Clarifying Questions

**Always ask these first. Interviewers score you on what you ask, not just what you answer.**

| Question | Why It Matters | What Changes |
|----------|---------------|-------------|
| "Is this a single-tenant or multi-tenant system? Who are the users?" | Scopes isolation requirements, RBAC complexity, and quota granularity | Multi-tenant (projects/teams) means row-level filtering everywhere |
| "What types of resources are being managed — VMs only, or also bare-metal and K8s?" | Bare-metal has finite, non-fungible inventory; K8s clusters have 10-min creation times | Bare-metal requires a reservation/scheduling layer; K8s needs async polling |
| "Is there a human approval step, or is provisioning fully automated?" | Approval = you need a durable workflow engine (Temporal), not just an API call | Without approval: simpler async queue. With approval: need signal/wait semantics |
| "What are the compliance and audit requirements?" | Drives retention (7 years), immutability, and the choice of Elasticsearch vs MySQL for audit | 7-year retention + full-text search → Elasticsearch ILM, not MySQL partitions |
| "Is the CLI used in CI/CD headless pipelines or only interactive terminals?" | Determines auth model | Headless CI → service account tokens; interactive → OAuth2 device flow |
| "What does 'availability' mean here — portal uptime or provisioning API uptime?" | These have different SLAs (99.95% vs 99.99%) | Provisioning API is on the critical path; portal UI can tolerate short outages |

**What candidates typically skip:**
- Asking about existing infrastructure (OpenStack? ClusterAPI? IPMI?) — this defines which provisioners you integrate with.
- Asking about the cost model — who bears the cost? Chargeback? Showback? This affects how deep the cost tracking goes.
- Asking about the brownfield vs greenfield situation — if thousands of manually-provisioned resources already exist, you need an import mechanism.

---

### 3b. Functional Requirements

**Core features (non-negotiable):**
- Resource CRUD: create, list, show, delete VMs, bare-metal, K8s clusters, storage
- Authentication: OAuth2/OIDC (interactive) + service account tokens (CI/CD)
- Quota enforcement: hard/soft limits per project, per resource type
- Approval workflows: auto-approve below threshold, manager/VP approval above
- Real-time status: user sees provisioning progress without polling
- Audit trail: immutable, searchable, 7-year retention
- Cost estimation before provisioning commits; daily cost snapshots
- Multi-context / multi-environment support (dev/staging/prod)

**Scope decisions to surface explicitly:**
- "I'll scope to VM, bare-metal, and K8s cluster provisioning. Storage and network provisioning are separate systems."
- "I'll cover the portal and CLI surfaces. CI/CD pipeline orchestration and application deployment are out of scope."
- "Cost billing/chargeback is out of scope — we provide estimates and snapshots; finance handles invoicing."

---

### 3c. Non-Functional Requirements

**Critical NFRs and how to derive them:**

| NFR | Target | How to Derive |
|-----|--------|--------------|
| Portal availability | 99.95% | ~4.4 hr/year downtime; internal tool, not customer-facing |
| Provisioning API availability | 99.99% | ~53 min/year; on the critical path for developer productivity |
| API latency P50 / P99 | 50–100 ms / 300–500 ms | Users abandon UIs after 3s; fast enough for interactive use |
| CLI startup | < 100 ms | Engineers run CLI many times per day; slow startup kills UX |
| CLI binary size | < 30 MB | Single binary install; needs to be downloadable in CI |
| Provisioning time | VM < 3 min; bare-metal < 15 min; K8s < 10 min | Benchmark from OpenStack Nova + IPMI PXE boot |
| Concurrent portal users | 1,000 | 10K engineers, 30% DAU, 10% peak concurrent = 300 normal; 3× for events = 1,000 |
| WebSocket connections | 1,000 | Matches peak concurrent users (one socket per active session) |
| Audit retention | 7 years | Compliance requirement (SOX, HIPAA analogs for internal infra) |
| CLI shell completion | < 300 ms | Human perception threshold for "instant" tab completion |

**Trade-offs to mention:**

- **99.99% provisioning API vs 99.95% portal**: ✅ Provisioning API stays up even during portal maintenance | ❌ Requires separate deployment paths and independent circuit breakers.
- **7-year audit in Elasticsearch vs MySQL**: ✅ Full-text search, time-series ILM, horizontal scale | ❌ Extra operational overhead of running an ES cluster.
- **Async provisioning (202 Accepted) vs synchronous**: ✅ No HTTP timeout on 15-min bare-metal provisioning | ❌ Client must poll or use WebSocket for status; more complex UX.

---

### 3d. Capacity Estimation

**The math (portal-focused, works for any system):**

```
10,000 total developers
× 30% DAU                     → 3,000 daily active users
× 10% peak concurrent         → 300 peak concurrent (normal)
× 3× for surge events         → 1,000 max concurrent users

3,000 DAU × 20 page views/day  → 60,000 page views/day
× 5 API calls/page view        → 300,000 API calls/day
÷ 86,400 seconds               → 3.5 RPS average
× 5× peak factor               → 17 RPS peak
```

**CLI-specific:**
```
4,000 CLI users × 30% DAU      → 1,200 daily CLI users
× 15 commands/user             → 18,000 CLI API calls/day
+ 500 CI pipelines × 5 calls   → 2,500 CI API calls/day
Total: ~20,500 / 86,400 × 5    → 1.2 RPS peak
```

**IaC-specific:**
```
500 IaC users (infra/SRE subset)
3,000 workspaces × 50 resources avg → 150,000 managed resources
2,500 plans/day + 1,000 applies/day → 3,500 operations/day
20% in 2-hour burst             → 60 concurrent plans, 25 concurrent applies
```

**What the math tells you:**
- 17 RPS peak is very low — a single Spring Boot pod handles 1,000+ RPS. **The bottleneck is not HTTP throughput; it's quota lock contention and provisioning backend latency.**
- 1,000 WebSocket connections is manageable — a single JVM can handle 10,000+ long-lived WebSocket connections. **The bottleneck is Redis pub/sub fan-out latency, not connection count.**
- 150,000 managed IaC resources across 3,000 workspaces is modest — a state file of 100 KB average fits in S3 cheaply. **The bottleneck is provider API call parallelism during plan, not storage.**

**Storage estimates:**
```
Resources (MySQL):    50,000 active × 2 KB          → ~100 MB
Audit logs (ES):      100,000 events/day × 1 KB × 365 → ~36 GB/year × 7 = ~250 GB
IaC state (S3):       3,000 workspaces × 100 KB × 30 versions → ~9 GB
Portal session (Redis): 1,000 users × 20 KB          → ~20 MB (negligible)
```

---

### 3e. High-Level Design

**Draw in this order — start with the user, work inward:**

**For the Web Portal:**
```
[Browser] → [CDN (CloudFront)] → React SPA (500 KB gzipped)
         → [Load Balancer (HAProxy)] → [BFF Layer (Spring Boot)]
                                       ↓ parallel fan-out
                            [Resource Svc] [Quota Svc] [Template Svc]
                            [Audit Svc]    [Cost Svc]
                                       ↓
                               [MySQL 8.0 Primary + Replicas]
                               [Elasticsearch]    [Redis]
                               [RabbitMQ] → async events

[Real-time]: Resource Svc → RabbitMQ → Redis pub/sub → WebSocket Gateway → Browser
```

**4–6 core components and their roles:**

| Component | Role | Key Decision |
|-----------|------|-------------|
| **BFF (Backend-for-Frontend)** | Portal-specific API aggregation layer; fans out to 4 backend services in parallel via `CompletableFuture`; shapes responses for the frontend | Without BFF: frontend makes 4+ sequential API calls → 2+ sec dashboard load |
| **WebSocket Gateway** | Pushes real-time provisioning status to connected browsers; subscribes to Redis pub/sub channels | `resource:{uid}`, `project:{id}:activity`, `user:notifications` channels; 30s heartbeat |
| **Quota Service** | Enforces hard/soft resource limits per project using `SELECT FOR UPDATE` reservation pattern | Pessimistic locking prevents TOCTOU race; two-phase reserve/commit decouples check from async provisioning |
| **Temporal Workflow Engine** | Durable, resumable orchestration for multi-step provisioning (validate → reserve quota → provision → configure → notify) | Provides built-in retry, timeout, and compensation; survives process restarts mid-workflow |
| **State Backend (IaC)** | S3 for state files + DynamoDB/MySQL for exclusive locks; 30 historical versions per workspace | Conditional write (`attribute_not_exists(lock_id)`) prevents concurrent applies |
| **Redis** | Session store (8h TTL), rate-limit counters (sliding window), quota cache (5-min TTL), WebSocket pub/sub fan-out | Sentinel cluster; falls back to MySQL on Redis failure — no data loss, just slower |

**Key data flows to articulate:**

1. **Provisioning request flow** (10 steps): User submits form → BFF validates JWT → Quota Service reserves → Temporal workflow starts → approval check → on approval: provisioner called → status pushed via WebSocket → quota finalized → notification sent.
2. **Real-time update flow**: Backend completes step → writes MySQL + publishes to RabbitMQ → consumer publishes to Redis pub/sub → WebSocket Gateway receives → pushes to browser < 50 ms P50.
3. **IaC plan/apply flow**: CLI reads YAML → builds dependency DAG → downloads state from S3 → provider plugins `Read` actual state → diff computed → topological walk executes creates/updates/deletes in parallel tiers → state written incrementally to S3 after each success.

---

### 3f. Deep Dive Areas

#### Deep Dive 1: Quota Enforcement — The TOCTOU Race

**Core problem:** Two users in project `ml-team` simultaneously request 10 GPUs each. The project hard limit is 15 GPUs and 8 are in use. Without atomicity, both requests see 7 available, both succeed, and you end up with 28 GPUs "allocated" against a 15-GPU limit — physical hardware that doesn't exist.

**Solution: Two-phase reservation with pessimistic locking.**

```
Phase 1 (synchronous, fast):
  BEGIN TRANSACTION
  SELECT ... FOR UPDATE on quota row (project_id=X, resource_type='gpu')
  IF used + reserved + requested <= hard_limit:
    reserved += requested
    COMMIT  ← lock released
  ELSE:
    ROLLBACK → reject request immediately

Phase 2 (after async provisioning completes):
  Success: reserved -= N; used += N
  Failure: reserved -= N (release)
  
Background reaper (every 5 min):
  Expire reservations stuck > 30 min (process crash guard)
```

**Trade-offs:**

- ✅ Strong consistency — `used + reserved` is always a correct upper bound | ❌ Row-level lock causes serialization; high-concurrency teams see latency spikes
- ✅ MySQL as single source of truth — no Redis/MySQL sync issues | ❌ Quota check is on the hot path; must be < 50 ms
- ✅ Stale reservation reaper handles process crashes | ❌ 30-min TTL means capacity can be temporarily over-reported by up to 1 reservation per resource type

**Deadlock prevention:** Always lock quota rows in canonical alphabetical order by `resource_type` within the same `project_id`. Multi-resource requests (vcpu + memory + gpu) lock all three rows in a single transaction in this canonical order.

---

#### Deep Dive 2: Durable Workflow Orchestration (Temporal)

**Core problem:** Provisioning bare-metal takes 15 minutes and touches 8 distinct steps across 5 services. The JVM process can crash at any step. You need to resume from the last checkpoint, not restart from scratch.

**Why Temporal over a homegrown DB state machine:**

| Approach | What You'd Have to Build | Temporal Gives You |
|----------|-------------------------|-------------------|
| DB state machine + cron | Durable timers, retry policies, signal handling, compensation logic, distributed dispatch | All of these out of the box |
| Spring State Machine | Per-transition handlers, manual timeout tracking, dead-letter queue | Less mature; no built-in workflow versioning |
| Temporal | Just implement Activities (individual steps) | Durability, retries, signals, timers, exactly-once IDs |

**Workflow steps:**
```
ProvisionResourceWorkflow (workflow ID = resource_uid, ensures exactly-once)
  1. ValidateRequest     (5s timeout)
  2. ReserveQuota        (10s, 3 retries)
  3. EvaluateApprovalPolicy  (5s)
  4. WaitForApprovalSignal   (72h timeout → auto-reject on expiry)
  5. Provision           (30min, 3 retries with exponential backoff)
  6. ConfigureResource   (10min: DNS, monitoring agent, SSH keys)
  7. FinalizeQuota       (swap reserved → used)
  8. Notify              (non-critical; retry 3×, log failure if still failing)

Compensation (on failure at any step):
  - Release quota reservation
  - Deprovision partial resources
  - Notify user with failure reason
```

**Trade-offs:**

- ✅ Temporal's `workflow ID = resource_uid` guarantees exactly-once execution | ❌ Operational overhead of running a 3-node Temporal cluster
- ✅ Workflow versioning (`Workflow.getVersion()`) allows safe in-flight deployments | ❌ Engineers must learn Temporal's programming model
- ✅ Test framework allows mocking activities and fast-forwarding timers | ❌ Local development requires a running Temporal dev server

---

#### Deep Dive 3: IaC State Locking and Dependency Execution

**Core problem:** Two engineers run `infra-iac apply` on the same workspace concurrently. Without locking, both modify the state file simultaneously, producing a corrupted merged state where resources appear twice or not at all.

**State locking with atomic conditional write:**
```
DynamoDB: PUT state_lock WHERE attribute_not_exists(lock_id)
MySQL:    INSERT INTO state_locks (workspace_id, lock_id, owner, ...) 
          ON DUPLICATE KEY → fail (workspace_id is PRIMARY KEY)

On conflict: return existing lock owner + operation in error message
On success: proceed with plan/apply
Lock release: DELETE state_lock WHERE workspace_id=X AND version=Y (optimistic)
Stale lock TTL: background reaper deletes locks where expires_at < NOW()
```

**Dependency graph execution:**
```
Resource DAG → Kahn's topological sort → execution tiers

Tier 0 (no dependencies):     execute all in parallel
Tier 1 (depend on Tier 0):    execute all in parallel, wait for Tier 0
...

Example:
  vpc → subnet → security_group → vm → eip
  Tier 0: vpc
  Tier 1: subnet, security_group  (both depend on vpc, run in parallel)
  Tier 2: vm                      (depends on subnet + security_group)
  Tier 3: eip                     (depends on vm)
```

**Trade-offs:**

- ✅ DynamoDB conditional write is a single atomic API call, no deadlock risk | ❌ DynamoDB adds a dependency; MySQL fallback needs `SELECT FOR UPDATE` which has contention
- ✅ Parallel tier execution dramatically reduces plan time for large workspaces | ❌ A failure in one tier stops the subsequent tiers; partial apply state must be handled
- ✅ S3 versioning gives 30 historical state snapshots for rollback | ❌ State rollback reverts the record of resources, not the actual infrastructure — must re-apply after rollback

---

### 3g. Failure Scenarios & Resilience

| Failure | Immediate Impact | Detection | Mitigation | RTO |
|---------|----------------|-----------|------------|-----|
| MySQL primary down | No writes; portal reads from replica | Heartbeat every 5s | MHA/Orchestrator auto-failover; Temporal pauses, resumes on recovery | 30s |
| Elasticsearch down | No audit writes; no search | Health check endpoint | Audit events buffer in RabbitMQ (24h TTL); retry on recovery | 5 min |
| RabbitMQ cluster down | Provisioning workflows stall (events not delivered) | Queue depth monitoring | 3-node mirror cluster; Temporal retains durable state, replays on recovery | 1 min (node), 5 min (cluster) |
| Temporal cluster down | No new workflows start; in-flight workflows pause | Heartbeat monitoring | 3-node cluster; Temporal's own MySQL-backed DB with replication | 2 min |
| Redis down | Cache miss; slightly slower quota checks | Sentinel monitoring | Fall through to MySQL; no data loss; sentinel auto-failover | 10s |
| OpenStack API down | VM provisioning fails | Provider API health checks | Temporal retries with 1/5/15 min backoff; user sees "provisioning delayed" | N/A (OpenStack SLA) |
| Okta (IdP) down | No new logins | Okta status page + synthetic login test | Existing JWTs valid for their TTL (1h); extend JWT TTL via emergency config during outage | N/A (Okta SLA) |
| Quota reservation leak | Capacity appears consumed but no resource exists | Reconciliation job: drift > threshold → alert | Background reaper every 5 min expires reservations > 30 min without status change | 5 min (auto) |
| Token theft (CLI) | Unauthorized API access | Anomalous access patterns (Okta) | 1h access token TTL limits exposure; token blacklist in Redis (populated by Okta webhook) | 1h max exposure |
| IaC state file corruption | Incorrect plan, possible resource duplication | S3 checksum validation on read | Roll back to previous S3 version; re-run plan; 30 historical versions retained | Minutes (manual) |

**Circuit breaker pattern (Resilience4j on all service calls):**
- Failure threshold: 50% failures in 10-call sliding window
- Open state: 30 seconds
- Fallback: return cached data (reads) or enqueue for retry (writes)

---

## STEP 4 — Common Components Breakdown

### MySQL 8.0 — Transactional Source of Truth

**Why used:** ACID transactions are required for quota enforcement (`SELECT FOR UPDATE`), workflow state transitions, and multi-step approval decisions. Strong consistency is non-negotiable here.

**Key config decision:** Enable `innodb_autoinc_lock_mode=2` (interleaved) for high-concurrency inserts. Use `READ COMMITTED` isolation to reduce lock contention on read-heavy replica queries.

**What if you didn't use it:** Using a NoSQL store (DynamoDB, MongoDB) would force you to implement application-level transactions for quota enforcement — complex, error-prone, and slow. Redis-only quota counters are fast but can diverge from MySQL on crash (dual-write problem).

---

### Redis 6.x — Cache, Session, Pub/Sub

**Why used:** Three distinct roles in one cluster: (1) server-side session store (8h TTL, HttpOnly cookie mapping), (2) rate-limit sliding window counters (Lua scripts for atomic increment), (3) WebSocket fan-out pub/sub (O(1) publish to N subscribers).

**Key config decision:** Use Redis Sentinel (3 nodes) not Cluster. The data is small (<1 GB), and Sentinel provides automatic failover without the complexity of hash slot management.

**What if you didn't use it:** Session management moves into MySQL (slow, extra load on the primary). Rate limiting requires DB queries per request. WebSocket fan-out requires polling or point-to-point connections between gateway nodes — both are much worse.

---

### Elasticsearch 8.x — Audit Log Storage

**Why used:** Audit logs require: (1) append-only immutability, (2) time-series range queries (`all events for project X in April`), (3) full-text search across `actor_email`, `resource_uid`, `action`, and (4) 7-year retention via ILM without MySQL table bloat.

**Key config decision:** `3 shards + 1 replica`; time-based index rollover (monthly); ILM policy: hot (SSD, 0–3 months) → warm (HDD, 3–12 months) → cold (S3 snapshot, 1–7 years). Index audit-logs via RabbitMQ (at-least-once) to decouple the hot path.

**What if you didn't use it:** MySQL with partitioning can handle audit at small scale but degrades badly at 36 GB/year × 7 = ~250 GB with complex queries. Full-text search on MySQL is slow and operationally painful.

---

### RabbitMQ — Async Event Bus

**Why used:** Provisioning workflows, status updates, notification dispatch, and audit log ingestion all require async, durable, at-least-once delivery with dead-letter queues. RabbitMQ's routing exchanges and per-queue acknowledgment model fits this perfectly.

**Key config decision:** 3-node mirrored cluster (all queues replicated). Use durable queues (`durable=true`) and persistent messages. Set message TTL on audit queue to 24h as a buffer against Elasticsearch downtime.

**Why RabbitMQ over Kafka:** We need task queues with acknowledgment semantics (one worker processes each message, marks it done). Kafka's log-based model is better for event streaming with multiple independent consumers. Our notifications and provisioning events are task-oriented.

---

### OAuth2 Device Flow (CLI) vs Authorization Code Flow (Portal)

**Why device flow for CLI:** Works in headless environments (SSH sessions, CI containers) where the CLI cannot open a local browser or listen on a localhost port for the authorization callback. GitHub CLI and Azure CLI use the same pattern.

**Key config decision:** Access token TTL = 1 hour (short, limits exposure window). Refresh token TTL = 30 days. Automatic transparent refresh using a `sync.Mutex` to prevent concurrent refresh races across multiple CLI processes.

**Why authorization code for web portal:** The browser is already there. Server-side code exchange (BFF) keeps tokens server-side. Session cookie is `HttpOnly; Secure; SameSite=Strict` — JavaScript cannot access the JWT.

---

### Temporal.io — Durable Workflow Engine

**Why used:** Provisioning workflows span 15 minutes, cross 5 services, and have 8 steps each requiring independent retry policies. Implementing durable timers, retry backoff, signal handling, and compensation logic from scratch against a database state machine is a multi-month engineering project that Temporal solves out of the box.

**Key config decision:** Use `resource_uid` as the Temporal workflow ID. This provides exactly-once execution guarantees (Temporal rejects duplicate workflow IDs). Workers are horizontally scalable; scale by queue depth.

**What if you didn't use it:** A homegrown DB state machine works at small scale but requires you to implement: (1) a cron-based task dispatcher, (2) per-step retry counters, (3) timeout/timer logic, (4) signal reception for approvals, and (5) compensation/rollback tracking. Each of these is a source of bugs in production.

---

### CDN + SPA Architecture (Web Portal)

**Why used:** React SPA (~500 KB gzipped) served from CloudFront edge means returning users get the frontend instantly without hitting the origin. SPA navigation (after initial load) is 200 ms vs 1.5 s for full-page SSR, because only data changes, not the shell.

**Key config decision:** BFF uses `CompletableFuture.supplyAsync` to call 4 backend services (resource, quota, cost, template) simultaneously. Dashboard load time is `max(4 service calls)` ≈ 150 ms, not `sum(4 service calls)` ≈ 600 ms.

**What if you didn't use CDN:** Every page load hits origin with a 500 KB transfer. At 300 peak users, that's 150 MB/burst to a single server — fine, but wasteful. More importantly, users in distant regions see 300–500 ms latency just for the initial HTML.

---

## STEP 5 — Problem-Specific Differentiators

### CLI Client vs Web Portal

**The CLI is optimized for machines, the portal for humans.** The CLI's defining constraint is a < 100 ms startup time achieved through a static Go binary with lazy config initialization and OS keyring token storage — there is zero network activity before a command executes. The portal's defining constraint is real-time feedback; it uses WebSocket + Redis pub/sub to push status updates in < 50 ms P50 without the developer refreshing the page.

**Auth model diverges completely by use case.** The CLI uses OAuth2 device flow for interactive use (works over SSH, no local HTTP server needed) and static service account tokens for CI/CD pipelines. The portal uses authorization code flow with a server-side session in Redis — the browser is always present, and server-side session means the JWT never touches JavaScript.

---

### Developer Self-Service Portal vs Web Portal for IaaS

**Self-service portal is workflow-first; web portal is observability-first.** The self-service portal's core value is the approval + provisioning workflow pipeline, where the hard problems are quota reservation races and durable multi-step orchestration via Temporal. The web portal's core value is real-time visibility — it adds WebSocket status push, BFF parallel aggregation for dashboard performance, customizable widgets, and embedded cost/utilization charts.

**Stack divergence is minimal but meaningful.** Both use Java Spring Boot and share the same MySQL schema. The self-service portal adds Temporal.io and a Notification Service. The web portal adds a Spring WebSocket Gateway and a BFF aggregation layer that the self-service portal doesn't need (it has fewer backend services to fan out to).

---

### Developer Self-Service Portal vs IaC Platform

**Portal is imperative and user-driven; IaC is declarative and state-driven.** In the portal, you click "create VM" and a Temporal workflow orchestrates that specific request. In IaC, you declare the desired state of all resources in YAML and the engine computes and applies the minimum diff. IaC's hard problems are unique: dependency graph resolution (topological sort), provider plugin architecture (gRPC process boundary), and state file consistency (S3 versioning + DynamoDB conditional write locking).

**Target audience is completely different.** The self-service portal is for 10,000 application developers who want to get a VM in 3 minutes without filing a ticket. The IaC platform is for 500 infra engineers and SREs who need reproducible, auditable, version-controlled infrastructure definitions that can be peer-reviewed as code.

---

### IaC Platform vs Everything Else

**IaC is the only system without a human in the critical path.** All other systems involve a human initiating an action in real time. IaC runs in CI/CD pipelines without human interaction, uses service account tokens (not OAuth2), stores state in S3 (not MySQL as primary), and has unique concepts (state drift, plan/apply separation, module registry, workspace isolation) that don't exist in the other three systems.

**State management is what makes IaC fundamentally different.** The state file is IaC's "database" — it records the last-known attributes of every managed resource. Without it, every plan would have to query all provider APIs from scratch (expensive and slow). The state file is also the source of every hard IaC problem: concurrent modification, drift from manual changes, and the complexity of rolling back a state file vs rolling back actual infrastructure.

---

## STEP 6 — Q&A Bank

### Tier 1: Surface Questions (2–4 sentence answers)

**Q: Why Go for the CLI and Java for the portal/IaC backend?**
Go compiles to a **single static binary with < 100 ms startup** — no JVM or interpreter warmup, making it ideal for a tool engineers run dozens of times a day. Java (Spring Boot) is the organizational standard for server-side services, has mature WebSocket support, and the team already has deep expertise in it. The CLI binary is also cross-compiled for Linux, macOS, and Windows (amd64 + arm64) in a single `go build` step, which would be much harder with a JVM-based tool.

**Q: Why MySQL instead of PostgreSQL?**
Both are strong choices; **MySQL is the organizational standard** here, meaning the team has existing operational expertise, runbooks, and monitoring tooling built around it. The features we rely on — `SELECT FOR UPDATE`, JSON columns, `AUTO_INCREMENT`, InnoDB row-level locking, and read replicas — exist in both databases. At our scale (< 20 RPS peak), neither database is a performance bottleneck.

**Q: Why Temporal instead of a simple state machine in MySQL?**
A DB state machine requires you to hand-implement **durable timers, retry policies, signal handling, compensation logic, and distributed task dispatch** — that's months of engineering and an ongoing source of production bugs. Temporal provides all of these as primitives. The operational cost (a 3-node Temporal cluster on K8s) is far less than the engineering cost of building and maintaining a reliable long-running workflow engine from scratch.

**Q: What is the BFF pattern and why does the web portal need it?**
BFF (Backend-for-Frontend) is a **portal-specific API layer** that sits between the browser and backend microservices, aggregating data from multiple services into a single response shaped for the frontend. Without the BFF, the browser would make 4–5 sequential API calls to load a dashboard, adding up to ~600 ms vs the BFF's parallel fan-out approach at ~150 ms. The BFF also handles auth token validation, rate limiting, and error normalization — concerns that belong to the portal, not the individual backend services.

**Q: Why Elasticsearch for audit logs instead of keeping them in MySQL?**
Audit logs require **7-year retention, full-text search across free-form fields, and time-series range queries** — three things MySQL handles poorly at scale. At 36 GB/year, a 7-year audit store is ~250 GB. Elasticsearch's ILM policy automatically transitions indices from hot (SSD) to warm (HDD) to cold (S3 snapshot) tiers as data ages, keeping costs manageable. Full-text search on `actor_email`, `action`, and `resource_uid` is also orders of magnitude faster in Elasticsearch than MySQL's `LIKE` queries.

**Q: How does the CLI work offline?**
The CLI maintains a **file-based cache at `~/.infra-cli/cache/`** with TTL metadata embedded in each cache file. When an API call fails due to network unavailability, the CLI falls back to the most recently cached response and prints a warning showing the cache age. Templates are cached for 1 hour (they change rarely); machine availability is cached for 5 minutes (changes frequently). Write operations (create, delete) always require API connectivity — offline mode is read-only.

**Q: How do you enforce per-project quota when multiple teams share the same resources?**
**Each project has a `quotas` table row per resource type** (vcpu, memory_gb, gpu, vm_count, etc.) with `hard_limit`, `soft_limit`, `used`, and `reserved` columns. When a provisioning request arrives, we acquire a `SELECT FOR UPDATE` lock on the relevant quota rows, check that `used + reserved + requested ≤ hard_limit`, and if so, increment `reserved`. This two-phase approach (reserve on request, finalize on provisioning completion) ensures that concurrent requests from the same project don't race past the hard limit.

---

### Tier 2: Deep Dive Questions (with "why" and trade-offs)

**Q: Walk me through how a quota reservation handles a process crash mid-provisioning.**
The key insight is that **reservations have a TTL enforced by a background reaper**. When a provisioning request is accepted, `reserved` is incremented in MySQL. If the provisioning process crashes before completing (before `reserved → used` or `reserved → released`), the reservation remains. A background job running every 5 minutes scans for reservations where `reserved > 0` and no status change has occurred in the last 30 minutes — these are treated as stale and the `reserved` count is decremented. The 30-minute window is conservative (bare-metal takes at most 15 minutes) but ensures we don't prematurely release a valid in-progress reservation. The trade-off is that capacity can be over-reported by up to one reservation per resource type for up to 30 minutes in the crash case.

**Q: How does the IaC plan engine determine what changed?**
The plan engine performs a **three-way diff** between: (1) desired state parsed from YAML/HCL config files, (2) last-known state from the state file in S3, and (3) actual current state fetched live from provider plugins via gRPC. The comparison is: if desired says resource X should exist with attribute Y, and the state file says X exists with attribute Z, the plan shows "~ update X: Y ≠ Z". If a resource exists in the state file but not in the desired config, the plan shows "- destroy X". The live provider read is important because it detects drift — someone manually changed a resource outside of IaC. The trade-off is that live provider reads slow down plan time; for 500 resources, this is the primary bottleneck (bottleneck is provider API call parallelism, not local computation).

**Q: How does the web portal handle 1,000 concurrent WebSocket connections without overwhelming a single server?**
**WebSocket connections are stateful** — once established, a socket is pinned to a specific server process. The portal uses Redis pub/sub to decouple the publishing side (provisioning backend) from the subscription side (WebSocket gateway). When provisioning completes, the backend publishes an event to a Redis channel (`resource:{uid}`). Every WebSocket gateway pod subscribes to all relevant channels and pushes to its locally connected clients. This means any gateway pod can handle any resource update — you don't need sticky routing for the backend. The 30-second heartbeat (ping/pong) ensures stale connections are cleaned up and Redis subscriptions are garbage-collected. The trade-off is Redis becoming a fan-out bottleneck at very high message rates, but at 60 messages/second across 1,000 connections, this is nowhere near Redis's limits.

**Q: Why use a reservation-based quota model instead of just using Redis atomic counters?**
Redis atomic counters (`INCR`/`DECR`) are fast but **create a consistency gap between Redis and MySQL**. If Redis crashes after a counter increment but before the provisioning record is written to MySQL, you have "used quota" with no corresponding resource record. The reservation pattern using `SELECT FOR UPDATE` on MySQL keeps quota state fully consistent with resource state in a single ACID transaction. The performance cost (row-level lock contention) is acceptable at our scale (< 20 provisioning requests per second peak). If we needed 10,000 provisioning requests per second, Redis counters with periodic MySQL reconciliation would be worth the consistency complexity — but at our scale, correctness beats throughput.

**Q: How does the IaC platform handle a module with 500 resources where the apply fails on resource 347?**
Because the apply engine **writes state incrementally after each successful resource operation**, the state file reflects the 346 resources that were successfully created before the failure. When the user re-runs `apply`, the engine reads this partial state, computes a diff showing 154 resources still to be created, and applies only those. Resources 1–346 are already in the state file as `active` — they won't be re-created (assuming no drift occurred). The trade-off is that this is an "optimistic partial apply" model, not a transactional one — there's no automatic rollback of the 346 already-created resources. For infrastructure, rolling forward is almost always preferable to rolling back, because reverting a half-applied network configuration can be more dangerous than the original failure.

**Q: How do you prevent a rogue developer from exceeding budget by submitting many small requests that individually pass the $100 auto-approve threshold?**
Three mechanisms work in concert. First, **quota hard limits** prevent exceeding a project's total resource allocation regardless of request size — many small requests hit the same quota. Second, **soft limits** trigger warnings (and optionally require manual approval) when cumulative usage exceeds, say, 80% of the hard limit. Third, **daily cost snapshots** feed into a budget tracking metric (`cost.daily_spend > 120% budget → alert`). The Temporal workflow's approval policy evaluates the estimated *total* cost for the requested duration, not just the hourly rate — so a small VM requested for 6 months can still trigger manager approval.

**Q: Walk me through how the CLI handles token refresh without interrupting a long-running command.**
The CLI's `AuthManager` has a **`sync.Mutex` for single-flight refresh**. Before every API call, the auth middleware checks the token's `expires_at`. If the token expires within 30 seconds (buffer for clock skew), it acquires the mutex and attempts refresh. The double-check pattern prevents the thundering herd problem: if two goroutines both detect expiry simultaneously, the second goroutine, after acquiring the mutex, re-reads the stored token — if it was already refreshed by the first goroutine, it uses the new token without making another refresh request. Refresh tokens are stored in the OS keyring (macOS Keychain, Linux Secret Service, Windows Credential Manager) to prevent disk exposure. If the refresh token itself is expired or revoked, the CLI exits with code 3 ("authentication required") and prompts the user to re-login.

---

### Tier 3: Stress Test / Staff+ Questions (reason through them out loud)

**Q: The organization is growing from 10,000 to 100,000 engineers. What breaks first and how do you fix it?**

Think through each layer systematically.

**First bottleneck: MySQL quota table lock contention.** Every provisioning request takes a row-level `SELECT FOR UPDATE` lock. At 10× the current provisioning rate (~700 peak RPS instead of 70), the lock wait queue for popular projects grows. The fix is **sharding the quota table by `project_id % 16`** — most provisioning requests within a project are sequential (same team), so intra-shard contention stays low. You'd deploy 16 MySQL shard pools; the application routes by `project_id`. This is the most impactful change.

**Second bottleneck: Temporal worker throughput.** At 100× provisioning volume, the Temporal task queue depth grows faster than workers drain it. The fix is **horizontal worker scaling** — Temporal workers are stateless, you can add pods. Scale workers by task queue depth metric via HPA.

**Third bottleneck: Elasticsearch write throughput for audit.** At 100× events, you're ingesting ~10 GB/day. Add Elasticsearch data nodes and increase the shard count (from 3 to 9 shards). The RabbitMQ buffer absorbs burst writes; Elasticsearch scales by adding nodes.

**What doesn't break:** The BFF, WebSocket gateway, and Redis are all horizontally scalable stateless services. The CDN is already edge-cached. The IaC platform scales by adding remote execution pods — it's already queue-based.

---

**Q: An Okta outage is happening right now. New logins fail. How do you keep the portal running for current users?**

Reason through the failure modes and mitigations:

**Existing sessions (Redis, 8h TTL):** These work normally. Users who are already logged in continue to work for the remainder of their session lifetime. The portal's BFF validates session cookies against Redis — no Okta dependency for existing sessions.

**CLI users with valid access tokens (1h TTL):** They continue to work. The access token is validated by the API gateway's JWT signature check (it has Okta's public key cached) — no live Okta call needed.

**New logins:** These fail. Users who need to log in for the first time during the outage cannot. Mitigation options: (1) Extend session TTL (Redis) via an emergency config push — existing users get more time. (2) Enable emergency local admin accounts for on-call engineers (stored in MySQL with bcrypt, activated by a feature flag). (3) Show a maintenance banner for new login attempts.

**Automatic token refresh:** The refresh token exchange requires a live call to Okta. Users whose access tokens expire during the outage and need refresh will fail. Mitigation: increase access token TTL temporarily from 1h to 8h via Okta emergency configuration (if Okta's API is partially up).

The key message: **a well-designed portal should remain functional for existing users for at least the session TTL (8h) during an IdP outage.** Only new logins and token refreshes are impacted.

---

**Q: A developer runs `infra-iac apply` and it succeeds, but the actual infrastructure was not created (the provider reported success but never actually created the resource). How do you detect and fix this?**

This is the **state file drift problem** — the state file says the resource exists, the actual infrastructure doesn't.

**Detection — three mechanisms:**
1. **Drift detection runs** (IaC NFR): Once per day, the platform runs `plan` in dry-run mode for every workspace without applying it. If the plan shows resources to create that the state file thinks already exist, that's drift.
2. **Provider health checks**: The provisioner can implement a `Read` operation that queries the resource's actual existence. If the resource `id` in the state file returns 404 from the provider, drift is detected immediately on the next plan.
3. **Monitoring + CMDB**: An independent CMDB (Configuration Management Database) reconciles expected vs actual inventory nightly. Discrepancies trigger alerts.

**Fix options:**
1. **If the resource truly doesn't exist:** Remove it from the state file with `infra-iac state rm <address>` and re-apply. IaC will now try to create it again.
2. **If the resource exists but with a different ID:** Use `infra-iac import <type> <id>` to bring the actual resource under state management with the correct provider ID.
3. **Root cause fix:** The provider bug that reported success without creating the resource needs to be fixed. Add a `Read`-after-`Create` verification step in the provider plugin — after Create returns 200, immediately call Read to confirm the resource actually exists before writing to state.

The deeper lesson: **the state file is a cache of reality, not reality itself.** Every IaC system must have a drift detection layer that periodically validates the state file against the actual infrastructure.

---

**Q: You're asked to add a "cost cap" feature that automatically terminates resources when they exceed a budget. How would you design this?**

**Break it into three sub-problems:**

**1. Real-time cost tracking:** The `daily_cost_snapshots` table records daily spend per project. But for real-time caps, you need running cost totals. Add a `running_cost_cents` field to each `resource` row, updated every hour by a background cost accumulator job that reads `cost_per_hour × hours_running`. This gives you a reasonably accurate (±1 hour) running total without expensive real-time calculations.

**2. Cap evaluation:** Add a `budget_cap_cents` field to the `projects` table. A scheduled job (cron every 30 minutes) compares `SUM(running_cost_cents) WHERE status='active'` per project against `budget_cap_cents`. When the sum exceeds the cap, trigger the termination workflow.

**3. Termination with graceful warning:** Don't terminate immediately. Send a warning notification when running cost reaches 80% of cap ("you will exceed your cap in ~X hours at current burn rate"). At 100% of cap, transition eligible resources (non-critical, non-production) to `expiring` status and start a 24-hour grace period. After grace period, trigger Temporal's deprovision workflow. Critical resources (marked by project admin) are exempt from auto-termination but generate escalating alerts.

**Trade-offs to mention:** Real-time to-the-minute cost accuracy requires streaming compute (Flink/Spark), which is heavy for this use case. Hourly granularity is sufficient for budget enforcement. The 30-minute check cycle means you can overshoot by 30 minutes of burn — for a $100/hr resource, that's $50 of overshoot, which is acceptable.

---

## STEP 7 — Mnemonics & Memory Anchors

### Acronym: **QAWRDS** (for the Self-Service Portal)

When asked "walk me through your design," use **QAWRDS** as your skeleton:

- **Q** — Quota: reserve with `SELECT FOR UPDATE`, two-phase commit
- **A** — Approval: Temporal workflow with human-signal step
- **W** — Workflow: validate → reserve → approve → provision → configure → notify
- **R** — Real-time: WebSocket + Redis pub/sub for status push
- **D** — Data model: MySQL (state) + Elasticsearch (audit) + Redis (session/cache)
- **S** — Scale: BFF parallel fan-out; stateless services; horizontal workers

### Acronym: **SLAP** (for the IaC Platform)

- **S** — State file: S3 + versioning (30 snapshots); the only source of truth
- **L** — Lock: DynamoDB/MySQL conditional write before any apply
- **A** — Analyze (plan): dependency DAG → topological sort → diff desired vs actual
- **P** — Parallelism: independent resource tiers execute in parallel; dependent tiers wait

### One-liner opening sentence (for any of these systems):

> "This is fundamentally a **resource lifecycle management system** — every design decision traces back to: how do we safely translate human intent into physical infrastructure changes across a multi-tenant, quota-constrained, audit-required environment, at the speed developers expect?"

Use this to frame your answer before diving into components.

---

## STEP 8 — Critique

### What the Source Material Covers Well

- **Quota enforcement is exceptionally thorough.** The two-phase reservation pattern, deadlock prevention via canonical lock ordering, the stale reservation reaper, per-user sub-quotas, and quota override mechanisms are all addressed with concrete SQL and Java code.
- **IaC state management details are production-quality.** The state file JSON schema, conditional write locking, topological execution, and the explicit handling of partial apply are all correct and battle-tested patterns (matching Terraform's actual implementation).
- **CLI UX is unusually detailed for a system design resource.** The Go implementation of the auth manager (mutex for concurrent refresh, OS keyring integration), the output formatter, offline cache, and shell completion are real engineering depth that differentiates this material.
- **The NFR table across all four systems** gives concrete, defensible numbers (1.2 RPS CLI, 17 RPS portal, 60 concurrent IaC plans) that you can quote confidently.

### What Is Missing or Shallow

- **Multi-region deployment is underspecified.** The source mentions "shard by region" as a future direction but doesn't detail the data replication strategy (MySQL async vs sync replication, cross-region latency implications for quota enforcement, active-active vs active-passive). For a Staff+ interview, you'd need to go deeper here.
- **Security threat model is light.** Input validation, CSRF, and XSS mitigations are listed but there's no discussion of supply chain security (provider plugin signing), secrets management integration (Vault), or zero-trust network policies between services.
- **IaC rollback semantics are not fully addressed.** The source notes that state rollback ≠ infrastructure rollback but doesn't explain what happens to resources created in a partial apply when you roll back the state file. This is a common interview trap.
- **Cost estimation accuracy is hand-waved.** The source mentions tracking "estimated vs actual" and alerting if error > 10%, but doesn't discuss how pricing data is kept fresh, how reserved-instance pricing vs on-demand pricing is modeled, or how to handle spot/preemptible resources.
- **The portal's mobile/accessibility story is thin.** WCAG 2.1 AA is mentioned as an NFR but not designed for. For a frontend-heavy role, this could matter.

### Real-World Concerns Not in the Source

- **Bare-metal scheduling at scale is harder than described.** The interval-tree overlap check works for hundreds of servers, but rack-affinity bin-packing for thousands of servers requires a proper scheduling algorithm (similar to Kubernetes scheduler's filter + score + bind phases). The preemption logic is also more complex in practice.
- **Temporal operational complexity is underplayed.** Running Temporal in production requires managing its own MySQL/PostgreSQL DB, handling Temporal worker versioning during rolling deploys, and monitoring Temporal-specific metrics (task queue latency, workflow backlogs). Teams often underestimate this.
- **IaC provider plugin security model has a gap.** Running provider plugins as separate gRPC processes is Terraform's approach and it is correct. But the source doesn't discuss plugin signing, sandboxing (seccomp/AppArmor), or what happens when a provider plugin is compromised. For infrastructure-critical systems, this is a real concern.
- **The Elasticsearch ILM cold tier (S3 snapshot) is often misunderstood in interviews.** Cold-tier data is not searchable until it's explicitly restored — a 7-year-old audit query requires a restore-before-search step. This is fine for compliance but not for ad-hoc forensics. Candidates should acknowledge this trade-off.

### Interview Traps to Watch For

- **"Just use Redis for quota"** — sounds appealing (fast, simple) but creates MySQL/Redis consistency splits. Push back: correctness over speed at our scale.
- **"Use Kafka instead of RabbitMQ"** — valid alternative, but requires justifying why you need log-based streaming vs task queues. At 6 RPS provisioning, Kafka's partition model adds complexity with no throughput benefit.
- **"Why not Postgres?"** — acceptable alternative to MySQL; don't get defensive. The real answer is team expertise and organizational standard, not a technical superiority claim.
- **"Can't you just poll for status instead of WebSocket?"** — polling at 1-second intervals from 1,000 clients = 1,000 RPS of status check traffic. WebSocket push eliminates this entirely. Make the math explicit.
- **"Why not just use Terraform instead of building your own IaC platform?"** — Terraform is an excellent answer! The custom IaC platform makes sense only if: (1) you have proprietary infrastructure types with no Terraform providers, (2) you need deeper integration with internal quota/approval workflows, or (3) remote execution with org-specific policy enforcement. Always acknowledge Terraform as the default-correct answer.

---

*This guide was synthesized from: `cli_client_for_infra_platform.md`, `developer_self_service_portal.md`, `web_portal_for_iaas.md`, `infrastructure_as_code_platform.md`, `common_patterns.md`, and `problem_specific.md`. All numbers, schemas, and design decisions are traceable to those source files.*
