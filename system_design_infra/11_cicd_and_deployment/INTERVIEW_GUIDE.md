Reading Infra Pattern 11: CI/CD & Deployment — 6 problems, 8 shared components

---

# Infra-11: CI/CD & Deployment — Senior/Staff Interview Study Guide

> Self-contained guide covering all 6 problems in this cluster: Artifact Registry, Blue-Green Deployment System, Canary Deployment System, CI/CD Pipeline System, Feature Flag Service, and Rollout Controller. Written for a cloud infrastructure platform engineer role at a FAANG-tier company. Assumes 200+ node Kubernetes clusters, 3,000+ engineers, 1,500+ microservices, 20,000+ bare-metal hosts.

---

## STEP 1 — SCOPE & ORIENTATION

### The 6 Problems in This Cluster

| Problem | One-Line Purpose |
|---------|-----------------|
| **Artifact Registry** | Store, serve, scan, and geo-replicate Docker images, Maven JARs, Python wheels, Helm charts |
| **Blue-Green Deployment** | Zero-downtime atomic traffic switch between two identical environments; instant rollback |
| **Canary Deployment** | Progressive traffic shifting 1%→100% with automated metric analysis and rollback |
| **CI/CD Pipeline** | Git push → build → parallel tests → scan → publish → promote through dev/staging/prod |
| **Feature Flag Service** | Decouple deployment from release; in-process evaluation, real-time updates, kill switch |
| **Rollout Controller** | Wave-based rolling updates across 50,000+ bare-metal hosts with topology-aware batching |

### The 8 Shared Components Across All 6 Problems

Every single one of these systems uses: **MySQL** (authoritative state), **S3** (durable blob storage), **SHA-256 content-addressed artifacts**, **Prometheus** (health gating), **automated rollback on threshold breach**, **immutable audit logs**, **idempotent operations**, and **progressive/wave-based deployment**. Knowing these cold means you cover 70% of interview ground before drawing a single service box.

---

## STEP 2 — MENTAL MODEL

### Core Idea

The entire CI/CD & Deployment domain is fundamentally about **separating concerns**: separating build from deploy, deploy from release, release from rollout, and rollout from validation. Every system in this cluster is a control plane that answers the question: "how do we move a new version of software to production while keeping the existing version serving traffic, measuring whether the new version is healthy, and being able to undo the change in seconds if it is not?"

### Real-World Analogy

Think of launching a commercial aircraft with passengers on board. You cannot take the plane out of service to install an engine upgrade. Instead, you:
1. Build and test the new engine in a hangar (CI pipeline + artifact registry)
2. Install it on a spare identical aircraft and run taxi tests with no passengers (blue-green staging)
3. Fly the spare plane with 1% of passengers on a short route, checking telemetry (canary)
4. Gradually board more passengers wave by wave if every telemetry check passes (rollout controller)
5. Carry a printed checklist of every flight configuration decision so auditors can review it (feature flags + audit logs)
6. Keep the old plane parked and fueled for 30 minutes so you can instantly switch passengers back (rollback)

The artifact registry is the hangar. The CI pipeline is the maintenance crew. The feature flag service is the toggle that lets you say "this engine is installed but not powered yet." Rollback is pulling up to the gate, swapping gates, and loading passengers onto the old plane — no new plane required.

### Why This Domain Is Hard in Interviews

Three reasons candidates fail here:

**First**, the hard problems are invisible. Traffic switching sounds trivial ("just update the load balancer"). The interviewer is probing whether you know about in-flight request draining, database schema compatibility, DNS TTL staleness, and connection pooling behavior during switches — all of which can cause lost data or dropped requests if handled naively.

**Second**, correctness and safety requirements conflict with velocity requirements. Moving fast means fewer gates; safety means more. The interviewer wants to see you reason about *where* to put gates (canary analysis after each weight step, not before) and *what* triggers automatic rollback versus human review.

**Third**, these systems interact with each other in non-obvious ways. A canary deployment is useless if the artifact registry is down and the canary image cannot be pulled. A feature flag kill switch is useless if the SSE gateway is down and the SDK is serving stale flags. You need to reason about failure at the intersection of systems, not just within one system.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these immediately after hearing the problem. They determine which of the 6 systems you are designing and what trade-offs to emphasize.

**1. What is the target environment: Kubernetes, bare-metal, VMs, or hybrid?**

What changes: Kubernetes means you can use Service selector swaps and Argo Rollouts. Bare-metal means you need gRPC agent daemons, a rollout controller with wave planning, and artifact pre-staging. Hybrid is most common in interviews — answer covers both.

**2. What scale? How many services, engineers, deployments per day, and hosts?**

What changes: 50 services and 10 engineers means a simple Jenkins pipeline. 1,500 services, 3,000 engineers, and 15,500 pipeline runs per day means you need Tekton on Kubernetes with distributed build caching, Kafka-decoupled webhook ingestion, and priority-class scheduling.

**3. What is the acceptable blast radius per deployment, and what is the rollback SLA?**

What changes: "Rollback in 5 seconds" drives you toward blue-green (instant selector swap, no redeployment). "Limit exposure to 1% of users" drives you toward canary. "Update 50,000 hosts safely" drives you toward wave-based rollout. Knowing the rollback SLA up front lets you justify infrastructure cost.

**4. Are database schema changes in scope?**

What changes: If yes, you need to explain expand-contract migrations (4-phase: add column → backfill → migrate reads → drop old column). This is one of the most common deep-dive areas for blue-green. If no, you can treat the database as a shared constant and focus on compute traffic switching.

**5. What is the deployment trigger model — GitOps (config repo), push-based (pipeline deploys directly), or hybrid?**

What changes: GitOps (ArgoCD watches a config repo) makes rollback a git revert and provides an auditable single source of truth. Push-based is faster to implement but harder to audit. Hybrid is common: pipeline commits image tags to a config repo; ArgoCD syncs.

**Red flags to watch for during clarification:**
- Interviewer says "100% availability during deployment" — clarify whether this means zero dropped requests (achievable) or zero seconds of reduced capacity (generally not achievable without 2x infrastructure cost).
- Interviewer says "real-time" for a feature flag update — pin them down to a number. "Real-time" means < 5 seconds in practice (SSE propagation), not sub-millisecond.
- Interviewer says "no downtime" for a database migration — this should trigger you to explain expand-contract immediately; dropping and recreating columns is not zero-downtime.

---

### 3b. Functional Requirements

**Core requirements that appear across all 6 problems (always state these):**

- Artifacts are stored by **SHA-256 digest** (content-addressable) so the same content is never stored twice
- Deployments are **idempotent**: re-running them is safe and produces the same result
- Every state change has a **who/what/when audit log** that is immutable
- The system integrates with **Prometheus** to automatically promote or roll back based on error rate and latency thresholds
- Operators can **manually override** any automated decision: pause, resume, rollback, skip a host, adjust batch size

**Problem-specific functional requirements to state clearly:**

For **CI/CD Pipeline**: Git push triggers pipeline within 10 seconds; tests run in parallel shards; artifacts promoted through dev → staging → prod with manual approval gates; secrets never appear in logs.

For **Artifact Registry**: Docker Distribution API v2 compliant; layer deduplication (one SHA-256 blob stored once regardless of how many images reference it); geo-replication to secondary regions within 5 minutes; vulnerability scanning on every push; retention policies expire old versions.

For **Blue-Green Deployment**: Two identical environments exist simultaneously; smoke tests run against the new environment before any traffic; traffic switch is atomic (< 1 second for load-balancer based); rollback requires no redeployment (< 5 seconds).

For **Canary Deployment**: Traffic split progressively (1% → 5% → 25% → 50% → 100%); automated analysis at each step; rollback when canary error rate exceeds stable + tolerance or exceeds absolute 5% threshold; manual approval gates configurable at any step.

For **Feature Flag Service**: In-process SDK evaluation (< 1 ms, no network call); flag changes propagate to all SDK instances within 5 seconds via SSE; emergency kill switch propagates within 2 seconds; flags have lifecycle tracking to prevent accumulation of dead flags ("flag debt").

For **Rollout Controller**: Wave size capped at 5% of fleet; topology-aware batching prevents taking down more than 33% of any AZ in a single wave; two-phase health gate (agent health reports + Prometheus metrics); auto-pause within 30 seconds of error detection.

**Clear scope statement:** "I will design a system that orchestrates [deployment type] for [N] services across [environment type], providing [rollback SLA] rollback, [blast radius] maximum blast radius, and automated promotion/rollback based on Prometheus metrics."

---

### 3c. Non-Functional Requirements

**Derive these, don't just list them.** Explain why each one matters.

**Availability:**
- Artifact Registry: 99.99% — if it is down, no deployments happen anywhere in the organization. Every CI pipeline and every Kubernetes node pull hits it.
- Feature Flag Service: 99.99% — a flag service outage means services either cannot evaluate flags (if SDK requires network) or serve stale flags (if in-process). Kill switch stops working.
- CI/CD Pipeline: 99.95% — a brief outage blocks developer velocity but does not affect production traffic.
- Canary/Blue-Green/Rollout: 99.95% — these are control planes; brief unavailability delays deployments but does not break production.

**Trade-offs to state unprompted:**
- ✅ In-process SDK evaluation (Feature Flags) achieves < 1 ms latency and 10M+ evals/second but means every service instance holds a full copy of all flag rules. A flag change takes up to 5 seconds to propagate because you need SSE push to all 12,000 instances.
- ❌ Server-side evaluation (every evaluation makes a network call) gives you instant consistency but adds 1–5 ms per evaluation and creates a hard dependency — if the flag service is down, every service in the org is broken.
- ✅ 2x infrastructure cost for blue-green during the deploy window buys you instant rollback with no redeployment. The cost is bounded: at $0.048/CPU-hour and 15-minute deploy windows, 200 deployments/day costs roughly $5,760/month for the idle Green capacity.
- ❌ Rolling updates (in-place) cost nothing extra but mean during a rollout, traffic is being served by a mix of old and new versions. If the new version has a bug, you cannot instantly rollback — you need to re-deploy the old version.

**Latency targets to anchor in the interview:**
- Flag evaluation: < 1 ms (in-process, trivially achievable)
- Flag update propagation: < 5 s (SSE push)
- Traffic switch (blue-green): < 1 s (Kubernetes Service selector patch)
- Rollback (blue-green): < 5 s (same selector patch, reverse direction)
- Canary rollback: < 10 s (set weight to 0%, scale canary pods to 0)
- Wave pause (rollout controller): < 30 s from error detection
- Artifact pull (registry): < 5 s for a 200 MB image within region
- Vulnerability scan: < 5 min after push
- Webhook to first pipeline task: < 10 s
- Build queue wait P95: < 30 s

---

### 3d. Capacity Estimation

**The formula to anchor your math:**

Pipeline load: `N engineers × commits/day × pipeline trigger rate = daily pipeline runs`
Example: 3,000 engineers × 4 commits/day × 1.3 (PRs + schedules) ≈ 15,500 runs/day

Peak factor: multiply daily average by 3 for post-standup burst: ~3,900 runs/hour peak

Build pods: 15,500/day ÷ 86,400s × 13 tasks/pipeline × 180s avg task duration × 1.3 headroom ≈ **3,350 concurrent pods**

Storage for artifacts: 9,300 Docker images/day × 150 MB = **1.4 TB/day raw** → ~600 GB/day after layer dedup (60% savings)

Registry bandwidth: 37,000 image pulls/day × 200 MB × 30% cache miss = **2.2 TB/day pull bandwidth**

Feature flag SSE connections: 1,200 services × 10 instances = **12,000 persistent SSE connections**

Rollout blast radius: 500 hosts × 5% per wave = **25 hosts per wave**; 20 waves total for a single service rollout

**Architecture implications from the math:**
- 3,350 concurrent build pods means your Kubernetes cluster needs a separate node pool for CI workers with aggressive autoscaling (0 to 3,500 nodes in minutes during burst).
- 12,000 SSE connections means your flag service SSE gateway needs horizontal scaling backed by Redis Pub/Sub — a single instance cannot maintain 12,000 open connections reliably.
- 1.4 TB/day of Docker pushes means your artifact registry's S3 backend needs a VPC endpoint (free within region) and the cross-region replication budget is ~$660/month.
- 2.2 TB/day of pull bandwidth means CDN or S3 pre-signed URL redirect (clients pull directly from S3, not through your registry API servers) is mandatory.

**Time allocation in the interview:** Spend 3–4 minutes on estimation. Pick the biggest number (build pods or registry bandwidth) and walk through it. Skip the obvious low-traffic paths.

---

### 3e. High-Level Design

**The 4–6 components every CI/CD design needs (whiteboard these in order):**

**1. Event Ingestion Layer** — GitHub webhook gateway that validates HMAC-SHA256 signatures, deduplicates events via Redis `SETNX`, and publishes to Kafka. This protects downstream systems from duplicate triggers and malformed webhooks.

**2. Orchestration Layer** — The controller (Tekton, Argo Workflows, or custom) that reads pipeline-as-code YAML from the repo, builds a DAG of tasks, and schedules pods on Kubernetes. This is the brain of the system.

**3. Execution Layer** — Ephemeral build/test/scan pods that mount build caches from S3, run in isolation, and produce artifacts. Key design: stateless pods, no persistent build agents, no shared filesystem between pods.

**4. Artifact Store** — Content-addressable S3-backed registry (Docker Distribution API v2 for images, standard Maven/PyPI APIs for packages). Key design: blob stored once by SHA-256 digest, regardless of how many images reference it.

**5. Deployment Plane** — ArgoCD (GitOps) for Kubernetes, Rollout Controller for bare-metal. Key design: pipeline commits image digest to a config repo; the deployment plane syncs from config repo to cluster (pull model, not push model).

**6. Observability Gate** — Prometheus with automated promotion/rollback rules. This is the shared health gate used by canary analysis, blue-green post-switch monitoring, and rollout controller wave promotion.

**Data flow to narrate (the "happy path"):**
1. Developer pushes a commit → GitHub fires webhook → Gateway validates HMAC → Redis dedup check → Kafka `pipeline.trigger` topic
2. Pipeline Controller consumes event → clones repo → parses `.tekton/pipeline.yaml` → creates task DAG
3. Build pod: restores Maven cache from S3 (cache key = SHA-256 of pom.xml) → `mvn package` → uploads new cache if main branch
4. Test pods (4 parallel shards): run JUnit, report results as JUnit XML
5. Scan pod: Trivy image scan → SonarQube SAST → block if severity >= HIGH
6. Package pod: `docker buildx build` with BuildKit layer cache → push image to registry as `sha256:abc123`
7. Promotion gate: all tests green + scan clean + (optional) manual approval → tag image as `staging`
8. ArgoCD detects new image tag in Helm config repo → rolls out to staging cluster → smoke tests pass → tag as `prod` → ArgoCD rolls out to prod

**Key decisions to call out explicitly:**
- "I'm using a **pull-based GitOps model** (ArgoCD watches a config repo) rather than the pipeline pushing directly to the cluster. This gives us an auditable record of every deployment, easy rollback via git revert, and separation of build permissions from deploy permissions."
- "I'm using **content-addressable artifacts** (SHA-256 digest) throughout. This means a Docker image built from commit `abc123` is identical whether it was built in CI or locally — the digest is the artifact identity, not a mutable tag like `latest`."
- "I'm using **in-process SDK evaluation** for feature flags rather than a remote evaluation service. This gives < 1 ms evaluation latency and survives flag service outages, but requires SSE push for sub-5-second propagation."

---

### 3f. Deep Dive Areas

**Deep Dive 1: Zero-downtime traffic switching (most commonly probed)**

**The problem:** When you switch traffic from Blue to Green (or promote a canary from 5% to 25%), you have requests in-flight to the old version and connection pools pointing to old pod IPs. A naive "update the config and restart" approach drops those requests.

**The solution for Kubernetes:** Kubernetes Service selector patch. A single API call changes `spec.selector.slot` from `blue` to `green`. The kube-proxy (or Cilium) updates iptables/eBPF rules in milliseconds. New connections go to Green immediately. In-flight requests to Blue continue because the TCP connections are already established — they complete normally. Blue pods are kept running during a drain period (30 seconds by default, configured via `terminationGracePeriodSeconds`). The `preStop` hook adds a brief sleep to allow the load balancer to remove the pod from rotation before the process terminates.

**The solution for bare-metal:** Envoy xDS dynamic route update. The rollout controller sends a gRPC `ClusterLoadAssignment` update to the Envoy control plane. Envoy immediately adjusts backend weights without reloading or dropping existing connections. This achieves < 100 ms switching time with zero dropped requests.

**Trade-offs to mention unprompted:**
- ✅ Kubernetes selector swap: < 1 second, zero dropped requests, native k8s primitive, no extra components
- ❌ DNS-based switching: simple but takes 30 seconds to 5 minutes depending on DNS TTL; clients with stale DNS cache hit the old version after the switch; not appropriate for < 1 second rollback SLA
- ✅ Envoy xDS: sub-100 ms switching, weight-based (enables gradual canary), but requires an Envoy control plane (Istio or custom xDS server)
- ❌ HAProxy config file reload: fast but causes brief connection reset during reload; mitigated by `seamless reload` mode but more complex to operate

**Deep Dive 2: Canary analysis — avoiding false positives and false negatives**

**The problem:** If you use an absolute threshold ("error rate > 1% = fail"), you will get false positives when the stable version itself is temporarily elevated (traffic spike, upstream dependency hiccup). If you set the threshold too high, you will miss real regressions. Both are dangerous.

**The solution:** Baseline comparison with a tolerance band. The analysis query compares canary error rate against stable error rate rather than an absolute number:

```
canary_error_rate <= stable_error_rate + tolerance
```

If stable is at 0.3% and canary is at 0.5%, and tolerance is 0.3%, this passes (0.5% <= 0.3% + 0.3%). An absolute safety net catches severe regressions even if stable is also elevated: if canary error rate > 5%, always fail.

**The statistical significance problem:** At 1% canary with a 10,000 RPS service, only 100 RPS hits the canary. In a 5-minute window, that is 30,000 requests — enough for statistical significance. At lower traffic services (< 100 RPS total), a 1% canary gets < 1 RPS, which produces no meaningful signal. For low-traffic services, skip the 1% step and start at 10% or 25%.

**Trade-offs:**
- ✅ Baseline comparison: accounts for natural traffic variance, fewer false positives
- ❌ Baseline comparison: if stable is already degraded, canary can introduce a regression that looks acceptable relative to stable
- ✅ Absolute safety net: catches severe regressions unconditionally
- ❌ Short analysis windows (< 5 min): insufficient samples for low-traffic services; produces `inconclusive` results that should be treated as pass to avoid premature rollbacks
- ✅ `inconclusive` = pass: prevents premature rollbacks during canary ramp-up when sample size is small
- ❌ `inconclusive` = pass: masks early signals of a bad deployment at low traffic

**Deep Dive 3: Expand-contract database migrations (blue-green specific, most commonly missed)**

**The problem:** Blue and Green environments share the same database. If version N+1 (Green) needs to rename a column from `user_name` to `display_name`, a naive `ALTER TABLE` breaks version N (Blue), which is still reading `user_name`. You cannot switch traffic to Green and immediately drop the old column — you need rollback capability for up to 30 minutes after the switch.

**The 4-phase solution:**

Phase 1 — **Expand**: Deploy Green (v1.2.4) that writes both `user_name` AND `display_name`. Run `ALTER TABLE users ADD COLUMN display_name VARCHAR(255)`. Blue (v1.2.3) reads only `user_name` and is unaffected.

Phase 2 — **Backfill**: Background job sets `display_name = user_name` for all existing rows. Run off-peak, in batches, with throttling.

Phase 3 — **Migrate reads**: Switch traffic to Green (v1.2.4) which reads `display_name` but still writes both columns. Rollback is safe: switching back to Blue still works because `user_name` is still being written.

Phase 4 — **Contract**: After 48+ hours of stable operation with no rollback, deploy v1.2.5 that drops `user_name` with `ALTER TABLE users DROP COLUMN user_name`.

**Trade-offs:**
- ✅ Expand-contract: zero-downtime schema migrations, safe rollback window
- ❌ Expand-contract: requires 3–4 deployment cycles over days or weeks; temporary double-write adds minor overhead; requires discipline from application developers

---

### 3g. Failure Scenarios

**Frame failures as a senior engineer would: what fails, who gets paged, and what the system does automatically before a human intervenes.**

**Scenario 1: Artifact registry is down**

Impact: every CI pipeline fails at the "docker push" and "docker pull" steps. Every Kubernetes node trying to schedule a new pod fails with `ImagePullBackOff`. This is a P0.

Mitigation in the design: pull-through cache serves images that were previously pulled from Docker Hub. Images already pulled to nodes are available from the container runtime cache (no re-pull needed). For registry downtime, the critical path is: does ArgoCD need to pull a new image? If yes, it queues. If the pod was already running, it continues serving.

What the interviewer is probing: did you design for registry HA (99.99% = 52 min/year downtime)? Did you decouple the blob storage (S3, which has its own 99.99% SLA) from the API layer (your registry pods)? Did you design geo-replication so a US-East outage does not break EU-West deployments?

**Scenario 2: Canary analysis detects false positive and rolls back a good deployment**

Impact: teams lose confidence in automated canary. They start bypassing it. Your safety net erodes.

Mitigation: baseline comparison (not absolute thresholds), configurable tolerance, `inconclusive` = pass for low-traffic services. Alert on rollback so humans review the analysis query results. Add a "manual promotion" escape hatch where a human can override the automated decision with a recorded reason.

**Scenario 3: Rollout controller agent loses connection to a host mid-rollout**

Impact: the host's update status is unknown. The health gate blocks waiting for all agents in the wave to report healthy.

Mitigation: timeout per wave (5 minutes). If an agent does not report within the timeout, the host is marked `failed`. The `max_failed_hosts_per_wave` threshold determines whether to auto-pause (default: > 1 failure in a wave triggers pause). The operator can then `skip_host` to exclude the unreachable host from further waves and investigate separately.

**Scenario 4: Feature flag SSE gateway goes down**

Impact: all 12,000 SDK instances stop receiving flag updates. Flags are frozen at their last-known state. Kill switch stops working.

Mitigation: SDKs serve cached values indefinitely. This is the correct behavior for in-process evaluation — never fail open. SDKs implement exponential backoff reconnection. The issue is limited to the inability to change flags during the outage. Design the SSE gateway with multiple replicas behind a load balancer and health checks so a single pod failure triggers automatic replacement within 30 seconds.

**Scenario 5: Blue-green controller crashes in the middle of a traffic switch**

Impact: unclear whether traffic is on Blue or Green. The database might be in an inconsistent state (switch logged as `switching` but not `completed`).

Mitigation: the controller is stateless — all state is in MySQL. On restart, the controller queries for deployments in `switching` status and queries the live load balancer or Kubernetes Service to determine the actual current slot. It reconciles the database to match reality. This is the **reconciliation loop pattern** — desired state is in the database, actual state is in the load balancer, the controller's job is to converge them.

---

## STEP 4 — COMMON COMPONENTS

These 8 components appear across all 6 systems. Know them cold.

---

### Component 1: MySQL as Deployment Metadata Store

**Why it is used:** Every deployment system needs a transactional state machine. When you move a deployment from `deploying` to `testing` to `switching` to `completed`, those transitions must be atomic. MySQL's ACID guarantees prevent two concurrent requests from both believing they are doing the traffic switch. The `UNIQUE KEY uk_service_slot (service_name, slot)` constraint in the blue-green `environments` table prevents two simultaneous Green environments for the same service.

**Key configuration:**
- Status columns are `ENUM` types, not free-form strings. This constrains valid transitions and lets the query planner use the column efficiently.
- `INDEX idx_status (status)` enables queue-polling patterns: "give me all deployments in `switching` state" without a full table scan.
- `updated_at DATETIME ON UPDATE CURRENT_TIMESTAMP` provides automatic timestamp tracking without application code needing to set it.
- Semi-synchronous replication for deployment records (99.999% durability without going to S3 for metadata).

**What breaks without it:** Without transactional metadata, two pipeline runs for the same service could race to claim the Green slot. Both write `status = deploying`. Both run smoke tests. Both attempt to switch traffic. This results in a half-switched deployment where no one knows which version is live.

---

### Component 2: S3 / Content-Addressable Object Storage

**Why it is used:** Artifacts — Docker layers, Maven JARs, build caches, build logs — are immutable once created. The same byte sequence always produces the same SHA-256 digest. S3's flat namespace maps perfectly to content-addressed storage (key = `sha256/<first-2-chars>/<full-digest>`). S3 provides 11-nines durability without any effort from the application layer. Storage is effectively infinite (no pre-provisioning). Cost is $0.023/GB/month, far cheaper than block storage.

**Key configuration:**
- Two-character directory prefix in the S3 key (`sha256/ab/abcdef...`) prevents hot-spot partitioning in the S3 key namespace (all digests otherwise cluster at the beginning of the keyspace).
- VPC endpoint for S3 eliminates inter-region data transfer fees for within-region traffic.
- S3 Intelligent-Tiering or lifecycle policies move old build logs to Glacier after 90 days, cutting storage costs by 80%.
- Pre-signed URLs let clients download blobs directly from S3 (bypassing your registry API pods), which eliminates the registry as a bandwidth bottleneck.

**What breaks without it:** Without content-addressable storage, you cannot deduplicate. 1,500 Java services sharing the same JDK base layer would each store a separate copy of that 200 MB layer. With content-addressing, that layer is stored once regardless of how many images reference it. The savings are enormous: from 540 GB to ~47.8 GB per version across 1,500 services — a 91% reduction.

---

### Component 3: Prometheus as Post-Deploy Health Gate

**Why it is used:** You need an objective, automated signal to decide whether a deployment is safe to proceed or must be rolled back. Prometheus provides this signal through PromQL queries against application metrics (HTTP error rates, latency percentiles, saturation). The same query language works for canary analysis, blue-green post-switch monitoring, and rollout controller wave gates.

**Key configuration:**
- PromQL for error rate: `sum(rate(http_requests_total{service="svc",version="canary",status=~"5.."}[5m])) / sum(rate(http_requests_total{service="svc",version="canary"}[5m]))`
- PromQL for p99 latency: `histogram_quantile(0.99, sum(rate(http_request_duration_seconds_bucket{service="svc",version="canary"}[5m])) by (le))`
- Canary uses **baseline comparison** (canary vs stable + tolerance), not absolute thresholds. This prevents false positives when stable metrics are temporarily elevated.
- Rollout controller uses a **two-phase gate**: all agents in the wave must report healthy (phase 1), AND global Prometheus metrics must be below threshold for 2 consecutive minutes (phase 2).

**What breaks without it:** Without automated metric gating, every deployment decision falls to humans. At 200 deployments/day across 500 services, no team of SREs can review dashboards for every deployment. The canary system degrades to "manual canary" — someone watches a dashboard for 30 minutes and makes a judgment call. This is not scalable and is biased toward promoting bad deployments ("it looks fine, let's go").

---

### Component 4: Automated Rollback on Threshold Breach

**Why it is used:** The critical insight is that the time between "something is wrong" and "a human pages into the incident" is typically 5–15 minutes. Automated rollback can act in < 10 seconds. At 10,000 RPS, 10 seconds of elevated error rates is 100,000 failed requests. At 10 minutes, it is 6 million. The cost of a false positive rollback (revert a good deployment) is minutes of wasted work. The cost of a missed rollback is minutes of user-visible errors.

**Key configuration:**
- Blue-green: after traffic switch, monitor for 5 minutes. If error rate or p99 latency exceeds threshold, trigger `LB_Manager.switch_traffic(to=blue)`. No redeployment — the old Blue environment is still running.
- Canary: error rate > stable + tolerance OR > 5% absolute → set canary weight to 0%, scale canary pods to 0.
- Rollout controller: > 1% global error rate for 2 sustained minutes → pause rollout. Does NOT auto-rollback (you have only partially updated the fleet; a rollback means re-deploying the old version to all already-updated hosts, which itself takes time). Pause and page the operator.

**What breaks without it:** Without automated rollback, a bad deployment that triggers alerts at 3 AM relies on an on-call engineer to wake up, assess the dashboard, decide to rollback, and execute. Mean time to rollback goes from seconds to 15–30 minutes. Every minute costs user trust and revenue.

---

### Component 5: Topology-Aware / Wave-Based Progressive Deployment

**Why it is used:** Deploying to 5% of the fleet first and checking health before proceeding limits the blast radius of a bad deployment. If the new version crashes on startup, only 5% of your capacity is affected instead of 100%. Topology-awareness ensures that 5% is spread across availability zones and racks — you do not accidentally take down 100% of one AZ's capacity while keeping 0% of another.

**Key configuration:**
- Wave size: 5% of fleet per wave (configurable). Absolute maximum (e.g., 50 hosts) prevents a 5% wave from being huge for large fleets.
- AZ constraint: max 33% of any AZ's hosts in a single wave. This ensures the service remains available in all AZs throughout the rollout.
- Rack constraint: max 2 hosts per rack per wave. This prevents a single rack failure from wiping out multiple hosts that were all updated simultaneously.
- Canary wave: wave 0 uses 1–2 pre-selected "canary hosts" with a longer soak time (10 minutes vs 2 minutes for main waves). These are typically the most stable, least-trafficked hosts.

**What breaks without it:** Without topology-awareness, a wave planner might select all 25 hosts from the same availability zone. If a bad deployment causes host instability, you lose an entire AZ. With a redundancy factor of 3, this means 33% of your service capacity is gone instead of the intended 5%.

---

### Component 6: Immutable Audit Log (Who/What/When)

**Why it is used:** Deployments affect production systems. Every state change — flag toggle, traffic switch, rollback, operator override — must be recorded with the initiator, timestamp, and before/after state. This is required for security compliance (SOC 2, ISO 27001), incident post-mortems, and debugging subtle issues caused by a flag that was toggled six hours before the incident.

**Key configuration:**
- Primary audit source: the MySQL state tables themselves. Every row records `triggered_by`, `created_at`, `updated_at`, and status transitions.
- Secondary audit stream: changes published to Kafka, consumed into Elasticsearch for searchable, long-term retention (2 years for pipeline audit, indefinite for flag changes).
- For feature flags: `audit_log` table captures `flag_key`, `old_value`, `new_value`, `changed_by`, `changed_at`. This is critical for incident correlation ("the checkout conversion rate dropped at 14:32 — what flag changed around that time?").
- Operator overrides in the rollout controller: every `pause`, `resume`, `skip_host`, `adjust_wave_size` is recorded in `rollout_overrides` with a required `reason` field.

**What breaks without it:** Without an audit log, a post-mortem question of "who deployed what at 2:47 AM?" becomes unanswerable. A flag that was accidentally set to `false` in production at 3 PM on a Friday cannot be correlated to the 4 PM incident without an audit trail. Compliance auditors have no evidence of change control.

---

### Component 7: Idempotent Operations

**Why it is used:** Networks fail. Kubernetes reconciliation loops re-execute operations. CI pipelines can be retried. If your operations are not idempotent, a retry of a failed operation produces a different result from the first attempt — a duplicate blob upload doubles storage, a duplicate pipeline trigger produces two build runs, a duplicate deployment request creates two competing Green environments.

**Key configuration:**
- Artifact registry: `INSERT INTO blobs ... ON DUPLICATE KEY UPDATE reference_count = reference_count + 1`. The blob is stored once; re-uploading increments a counter.
- CI pipeline webhook: Redis `SETNX event_id TTL=300s` deduplication. If two webhooks arrive for the same commit SHA within 5 minutes, only the first triggers a pipeline.
- Rollout controller: `filterPending(hosts)` skips hosts where `current_version == target_version`. Re-running the same rollout is a no-op for already-updated hosts.
- Feature flags: `UNIQUE KEY uk_flag_key (flag_key)` prevents creating two flags with the same key. Updates use optimistic locking (`version` column, `WHERE version = expected_version`).

**What breaks without it:** A GitHub webhook that fires twice (common during network hiccups) would trigger two parallel CI pipelines for the same commit. Both race to push the same Docker image, populate the same artifact record, and trigger deployments. Idempotency makes the second trigger a no-op.

---

### Component 8: Redis (Dedup, Locking, Pub/Sub, Caching)

**Why it is used:** Redis serves three distinct roles across these systems: distributed locking (only one pipeline runs per commit), event fan-out (flag changes published to all SSE gateway instances), and cache (fast manifest lookups in the artifact registry, hot path read acceleration).

**Key configuration:**
- Webhook dedup: `SETNX pipeline:event:{sha}:{event_id} 1 EX 300`. Returns 0 if already set (duplicate). First write wins.
- Feature flag SSE fan-out: Flag Management Service publishes to Redis channel `flag_updated:{flag_key}`. All SSE Gateway pods subscribe. Each gateway pushes the update to its open SSE connections. This enables horizontal scaling of the SSE gateway without shared in-process state.
- Registry manifest cache: `GET registry:manifest:{repo}:{tag}` → JSON manifest. Tag-to-digest resolution becomes a Redis read instead of a MySQL query. Reduces p99 latency for HEAD requests from 20 ms to < 1 ms.
- Blue-green live slot tracking: `SET service:{name}:live_slot green`. Read by monitoring and routing systems in sub-millisecond latency.

**What breaks without it:** Without Redis Pub/Sub for SSE fan-out, you would need all SSE connections to be on the same server instance or use a message broker like Kafka for each flag change. Without webhook dedup, duplicate pipelines fire and race. Without manifest caching, every Docker pull hits MySQL for tag-to-digest resolution — at 10,000 concurrent pulls, that is 10,000 MySQL reads per second.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Artifact Registry

**What makes this problem unique:** This is the only system in the cluster where the core data model is defined by an external standard (Docker Distribution API v2 / OCI Image Spec). You are not inventing the API — you are implementing a specification. The deep-dive areas are about the internals: content-addressable blobs, layer deduplication, garbage collection (reference counting), and the pull-through cache that proxies Docker Hub and Maven Central to insulate you from upstream outages and rate limits.

**Different decisions from other problems:**
- The blob store must implement SHA-256 content addressing with a two-character directory prefix to avoid S3 hot partitioning.
- Vulnerability scanning is an async side effect of a push (Trivy triggered by webhook), not a blocking gate on the push API itself.
- Geo-replication is for read performance (pull from the nearest region), not for write redundancy. Writes always go to the primary region and replicate asynchronously.
- Retention policy is a garbage collection problem: you cannot delete a blob if any manifest still references it (`reference_count > 0`). The GC must first delete tags, then delete unreferenced manifests, then delete blobs with `reference_count = 0`.

**Two-sentence differentiator:** The artifact registry is a specification-conformant implementation of the Docker Distribution API v2 sitting in front of S3-backed content-addressable blob storage, with deduplication achieved through SHA-256 digest equality rather than application logic. The operationally hard parts are the garbage collector (must not delete blobs that are still referenced by any manifest in any region), the pull-through cache (maintains freshness without overwhelming upstream rate limits), and geo-replication (eventual consistency across regions while guaranteeing that a pull never returns a manifest whose blobs have not yet replicated).

---

### Blue-Green Deployment System

**What makes this problem unique:** Blue-green is the only pattern in this cluster where the traffic switch is fully atomic and binary — 100% of traffic moves from one version to the other in a single operation with no intermediate state. The hard problems are: the database compatibility constraint (both versions must be able to read and write the same schema simultaneously), the infrastructure cost (2x during the deploy window), and the operational question of when to tear down the old Blue environment.

**Different decisions from other problems:**
- Unlike canary, there is no gradual traffic shift — the switch is all-or-nothing. This is a design choice optimized for fast rollback at the cost of blast-radius control.
- The Kubernetes implementation uses a **Service selector patch** (a single API call), not a VirtualService weight update. There is no traffic manager or service mesh required.
- The database constraint drives the **expand-contract migration pattern**, which is unique to blue-green. Canary and rolling updates also share a database but the schema constraint is less severe because both versions run simultaneously only briefly.
- The **bake period** (keeping Blue alive after switching to Green) is a cost/safety trade-off. 30 minutes of warm standby costs roughly $0.48 per deployment but means rollback requires zero redeployment.

**Two-sentence differentiator:** Blue-green deployment's defining characteristic is the instant, binary traffic switch and the equally instant rollback — both achievable because two fully-provisioned environments exist simultaneously, sharing only the database layer. The hardest operational challenge is the database compatibility constraint: any schema change must go through expand-contract (add column, backfill, migrate reads, drop old column across 3–4 separate deployment cycles) because both Blue (old) and Green (new) code must be able to read and write the schema correctly during the switch window.

---

### Canary Deployment System

**What makes this problem unique:** Canary is the only pattern where traffic is split between two versions simultaneously for an extended period (30–60 minutes) with automated metric analysis driving each promotion decision. The analysis engine is the intellectual heart of this system. The Istio VirtualService (or equivalent) is how you actually implement the traffic split in Kubernetes.

**Different decisions from other problems:**
- The analysis engine uses **baseline comparison** (canary vs stable + tolerance), not absolute thresholds, to avoid false positives caused by natural traffic variance.
- **Argo Rollouts** (or Flagger) is the Kubernetes-native implementation — you are not writing a custom controller from scratch; you are configuring a battle-tested open-source controller.
- Statistical significance matters at 1% traffic weight: for services with < 100 RPS total, the 1% canary receives < 1 RPS, producing no meaningful signal in a 5-minute window. Skip the 1% step for low-traffic services or increase the analysis window to 30+ minutes.
- Unlike blue-green, rollback does not require keeping a separate full environment warm — you simply scale the canary pods to 0 and update the VirtualService weight back to 100% for stable.

**Two-sentence differentiator:** Canary deployment is uniquely characterized by its automated analysis engine that compares canary metrics against the stable baseline at each traffic step, using relative comparison (canary <= stable + tolerance) rather than absolute thresholds to avoid false positives during normal traffic variance. The implementation complexity lives in the traffic splitting mechanism (Istio VirtualService or SMI TrafficSplit) and in handling edge cases: low-traffic services where the canary gets insufficient samples, multi-region deployments where each region runs its canary independently, and the interaction between canary traffic weights and connection pool behavior in downstream services.

---

### CI/CD Pipeline System

**What makes this problem unique:** This is the only problem in the cluster that is entirely about developer velocity rather than deployment safety. The hard problems are build throughput at scale (15,500 pipeline runs/day, 3,350 concurrent pods), build cache efficiency (Maven dependency downloads are the long pole in the tent), and secret hygiene (secrets must never appear in build logs). The pipeline is also the only system that interacts with all the other systems — it calls the registry, triggers canary/blue-green deployments, reads feature flag status, and updates the rollout controller.

**Different decisions from other problems:**
- **Build cache** is a first-class concern. Maven cache keyed by `sha256(pom.xml + gradle-wrapper.properties)` and stored in S3. Cache hit ratio of 80%+ cuts Java build times from 3 minutes to 45 seconds.
- **Test sharding** via Tekton task matrix or GitHub Actions matrix: `shard-index: [0,1,2,3]` with 4 parallel pods gives 4x speedup for test execution.
- **Priority classes** (`pipeline-critical` for hotfixes, `pipeline-high` for main branch, `pipeline-normal` for feature branches) prevent hotfix pipelines from waiting behind low-priority feature branch builds.
- **Secret management**: secrets injected as ephemeral environment variables from Vault/AWS Secrets Manager at pod startup; log scrubbing strips secrets from output; secrets are never written to S3 log storage.

**Two-sentence differentiator:** The CI/CD pipeline system is fundamentally a distributed task scheduler — a DAG of ephemeral Kubernetes pods that transforms a Git commit into a vulnerability-scanned, tested Docker image promoted through dev/staging/prod approval gates — and its performance is dominated by build cache hit ratio and parallel test shard efficiency. The critical reliability concern is not the pipeline orchestrator itself (Tekton, Argo Workflows) but the webhook gateway's idempotency (prevent duplicate triggers), the build cache coherency (avoid stale caches causing flaky builds), and the secret hygiene discipline (secrets must be zero-knowledge with respect to log storage).

---

### Feature Flag Service

**What makes this problem unique:** This is the only system in the cluster that is entirely a control plane for application behavior rather than a deployment mechanism. The key architectural insight — **in-process SDK evaluation** — makes the evaluation latency problem trivially solvable (< 1 ms) at the cost of requiring a real-time propagation mechanism (SSE). The tension between "availability during flag service outage" (cached defaults) and "freshness of flag values" (propagation latency) drives most design decisions.

**Different decisions from other problems:**
- **In-process evaluation** means the flag service server handles zero production request traffic — only 12,000 SSE connections and ~50 flag CRUD operations per day. The "scale" problem for this service is SSE connection management, not query throughput.
- **SSE over WebSockets**: SSE is unidirectional (server pushes to client), simpler than WebSockets, HTTP/1.1 compatible, and sufficient for flag updates. WebSockets are overkill for a read-heavy stream where clients only consume updates.
- **murmurHash3 for percentage rollout**: deterministic hash of `(flag_key + ":" + user_id)` maps each user to a stable bucket 0–99. This means user 12345 always sees the feature at 30% rollout and never oscillates between enabled and disabled across requests.
- **Flag lifecycle management** (stale_after date, lifecycle_status ENUM) is a unique operational concern. Without cleanup discipline, flag definitions accumulate until developers are afraid to delete anything. Hundreds of stale flags slow SDK initialization and create cognitive overhead.

**Two-sentence differentiator:** The feature flag service's defining design decision is in-process SDK evaluation, which pushes all rule logic into a client-side `ConcurrentHashMap` updated via SSE, achieving < 1 ms evaluation latency and 10M+ evals/second with zero server-side query load. The system's resilience model is "graceful degradation to last-known state" — when the flag service is unavailable, SDKs continue serving cached values and reconnect automatically, which is safe for feature gating but dangerous for emergency kill switches if the outage is prolonged.

---

### Rollout Controller

**What makes this problem unique:** This is the only system designed for bare-metal and VM fleets where Kubernetes abstractions (Deployments, Services, Pods) do not exist. The controller must maintain a CMDB (fleet inventory), speak gRPC to 20,000+ host agents, implement topology-aware wave planning from scratch, and handle operator overrides gracefully. The wave planner is the most algorithmically interesting component in the entire cluster.

**Different decisions from other problems:**
- **Two-phase health gate**: phase 1 is agent health (all hosts in the wave must report healthy within 5 minutes), phase 2 is Prometheus global metrics (error rate and p99 latency must be below threshold for 2 sustained minutes). Both must pass before the next wave starts. Blue-green uses only phase 2 (Prometheus). Canary uses only phase 2.
- **Auto-pause (not auto-rollback)**: when Prometheus thresholds are breached during a wave, the controller pauses rather than automatically rolling back. This is because a partial rollout rollback (reverting all already-updated hosts) takes 10+ minutes and may not be the right call — the issue might be transient or limited to a single host. A human decides.
- **Canary wave (wave 0)**: 1–2 pre-selected "canary hosts" with a 10-minute soak period before starting the main waves. This provides early signal on the most stable, representative hosts before touching the broader fleet.
- **Artifact pre-staging**: artifacts are distributed to host local caches (via P2P/BitTorrent-like distribution) before the rollout starts. This decouples network bandwidth from the deployment timeline and prevents the rollout from being bandwidth-limited.

**Two-sentence differentiator:** The rollout controller is the only system in the cluster that manages heterogeneous bare-metal infrastructure through a persistent gRPC agent protocol, requiring it to maintain a full fleet inventory (CMDB) and implement topology-aware wave planning that respects AZ and rack distribution constraints to bound blast radius at multiple granularities simultaneously. Its defining operational characteristic is the "pause, don't auto-rollback" policy for Prometheus threshold breaches — unlike canary which can instantly revert to a fully-running stable version, a partial bare-metal rollout requires human judgment to determine whether to continue, pause, or revert the already-updated hosts.

---

## STEP 6 — Q&A BANK

### Tier 1: Surface-Level Questions (2–4 sentence answers)

**Q1: What is the difference between blue-green deployment and canary deployment?**

**Blue-green** switches 100% of traffic from the old version to the new version atomically. There is no period where both versions receive production traffic simultaneously (unless you add an optional pre-switch canary step). The key benefit is instant, zero-redeployment rollback. The key cost is 2x infrastructure during the deploy window.

**Canary** shifts traffic gradually — 1%, 5%, 25%, 50%, 100% — analyzing metrics at each step before proceeding. Both versions serve real production traffic simultaneously for the duration of the rollout (30–60 minutes). This limits blast radius to the canary percentage but means rollback cannot be instant for the 50% step (though it is fast — set weight to 0%, scale pods to 0). The key benefit is safe production validation before full rollout.

---

**Q2: Why do you use a pull-through cache in the artifact registry?**

**Pull-through cache** proxies public registries (Docker Hub, Maven Central, PyPI) and stores fetched artifacts locally. Without it, every build pod that pulls a public base image hammers Docker Hub, which enforces rate limits (100 pulls/6 hours for unauthenticated, 200 for free accounts). At 9,300 Docker builds per day, you would exhaust Docker Hub rate limits within an hour. The pull-through cache also provides resilience — if Docker Hub is down, your builds continue using cached versions.

---

**Q3: What is a feature flag, and why would you use one instead of just deploying a new version?**

A feature flag is a conditional branch in application code that checks a remotely-configurable value before executing a code path: `if (flagService.isEnabled("new_checkout", user)) { ... }`. The key insight is **deployment and release are separate events**: you can deploy code to production with the flag off (dark launch), verify the deployment is healthy, then enable the flag for 1% of users, then ramp to 100% over hours or days. If anything goes wrong, you flip the flag off — no redeployment, no rollback, no incident. Feature flags also enable targeting: "enable for internal employees only" or "enable for users in the US only."

---

**Q4: What does "content-addressable storage" mean for Docker images?**

Every Docker image layer is stored with its SHA-256 digest as the key. If two different images contain the same layer (e.g., the JDK 17 base layer), that layer is stored exactly once in S3. When a client pushes a layer whose digest already exists, the push is a no-op. When a client pulls an image, the manifest lists the digests of all layers, and the client fetches only layers it does not already have locally. For 1,500 Java services sharing the same JDK base layer, this reduces storage from ~540 GB to ~47.8 GB per version — a 91% saving.

---

**Q5: How does a CI/CD pipeline achieve 15,500 runs per day without queue starvation?**

**Priority-class scheduling**: hotfix pipelines get Kubernetes `PriorityClass: pipeline-critical` (priority 1000) which preempts normal-priority build pods. Main branch pipelines get `pipeline-high` (500). Feature branches get `pipeline-normal` (100). When compute is scarce, hotfixes always run immediately by preempting lower-priority builds. The preempted pods are rescheduled when capacity recovers. This ensures critical path (hotfix → production) is always fast even during peak build periods.

---

**Q6: What is the purpose of the rollout controller's "health gate" between waves?**

The health gate is the automated quality check that prevents a bad deployment from propagating to the next wave of hosts. It has two phases: first, all host agents in the completed wave must report `healthy` status via gRPC within a timeout (5 minutes). Second, global Prometheus metrics (error rate, p99 latency) must remain below configured thresholds for 2 sustained minutes. If either phase fails, the rollout auto-pauses. Both conditions must pass before the controller advances to the next wave. This ensures that you catch both local failures (a specific host or rack issue) and global failures (a regression that affects the service-wide error rate).

---

**Q7: Why does the feature flag SDK use SSE rather than WebSockets for receiving flag updates?**

SSE (Server-Sent Events) is unidirectional — the server pushes data to the client, and the client only receives. This matches the flag update use case exactly: the SDK only needs to receive flag changes, never send anything back. SSE is HTTP/1.1 compatible (works through corporate proxies that often block WebSocket upgrade), simpler to implement, and has automatic reconnection built into the browser specification. The only limitation of SSE — that clients cannot send data — is not a limitation here. WebSockets would add bidirectional complexity for zero benefit.

---

### Tier 2: Deep-Dive Questions (trade-offs required)

**Q8: How do you handle database schema changes in a blue-green deployment where both Blue and Green point to the same database?**

The **expand-contract pattern** solves this. The constraint is: at the moment of traffic switch, both Blue (v1.2.3) and Green (v1.2.4) may be reading from and writing to the same schema. Any schema change that breaks old code (dropping a column, renaming a column, changing a column type) will cause errors in the Blue environment during the rollback window.

The solution spans four deployment cycles. First, **expand** the schema: add the new column as nullable without removing the old one; Green (v1.2.4) writes to both columns while Blue only writes to the old column. Second, **backfill** existing rows in a background job. Third, **migrate reads** in Green (v1.2.5): read from the new column, still write to both. Now Blue can roll back safely because the old column still exists and has fresh data. Fourth, after 48+ hours of stable operation with no rollback, **contract**: deploy v1.2.6 that drops the old column.

The trade-off is speed versus safety. This approach requires 3–4 deployment cycles spanning days or weeks for a single column rename. The alternative — doing a big-bang migration with downtime — is faster but requires a maintenance window. For a 99.99% availability target, downtime windows are not acceptable, so expand-contract is the only viable approach.

---

**Q9: Your canary is at 5% traffic with 200 RPS total (10 RPS on canary). How do you make a statistically meaningful promotion decision in 5 minutes?**

At 10 RPS on the canary, you get 3,000 requests in a 5-minute analysis window. At a typical p99 latency around 200 ms, you need roughly 100 requests minimum for a reliable percentile estimate. With 3,000 requests, you have adequate sample size for error rate (even 1 error is 0.03%) and for p99 latency (the 99th percentile of 3,000 samples is the 30th-highest value).

**However**, 10 RPS is not always enough. For services with < 10 total RPS, a 5% canary gets 0.5 RPS — only 150 requests in 5 minutes. Here, you have two options. First, extend the analysis window to 30+ minutes to accumulate more samples before making a promotion decision. Second, skip the 1% and 5% steps and start at 25%, giving you 2.5 RPS on the canary and 750 requests in 5 minutes.

The **`inconclusive` result** (too few samples for a statistically reliable decision) should be treated as "pass" rather than "fail" to avoid premature rollbacks during canary ramp-up. The analysis engine should count consecutive inconclusive results and alert if there have been too many — that signals either very low traffic (extend window) or a dead canary pod (investigate).

---

**Q10: How do you prevent the artifact registry from becoming a single point of failure for all deployments in your organization?**

Multiple layers of resilience. At the **API layer**: the registry runs as multiple Kubernetes pods behind a load balancer with health checks and autoscaling. A single pod failure triggers replacement in < 30 seconds with no request drops (in-flight requests complete via connection draining).

At the **storage layer**: blobs are in S3, which has its own 99.99% availability and 11-nines durability SLA. If your registry API pods are down, existing pods on Kubernetes nodes can still start new containers using layers cached by the container runtime — no registry call needed for a cached layer.

At the **distribution layer**: geo-replication ensures each region has its own copy of all blobs. A US-East registry outage does not affect EU-West deployments. The pull-through cache means even if your registry is partially degraded, base images from Docker Hub are served from local S3.

The remaining risk is a **global metadata outage** (MySQL down). Mitigated by: MySQL semi-synchronous replication with automatic failover, Redis caching of frequent manifest lookups (tag → digest, manifest → layer list), and circuit breakers in the registry API that return cached responses rather than erroring. The circuit breaker approach means a brief MySQL outage degrades to "read-only from cache" rather than "total outage."

---

**Q11: Describe the rollout controller's wave planning algorithm and why topology-awareness matters.**

The wave planner takes the full list of pending hosts (those where `current_version != target_version`) and divides them into ordered batches. Wave 0 is a **canary wave** of 1–2 pre-selected stable hosts with a 10-minute soak period — early signal before touching the broader fleet. Main waves are limited to 5% of the total fleet (configurable), with two hard topology constraints: no more than 33% of any AZ's hosts in a single wave, and no more than 2 hosts from any single rack.

Without topology-awareness, a naive 5% batch might randomly select all 25 hosts from AZ-1 rack-7. If the new version causes those 25 hosts to crash, all of rack-7 is gone. With the rack constraint (max 2 per rack), the 25-host wave is spread across at least 13 different racks. A correlated rack failure (bad power supply, bad network switch) then affects at most 2 hosts in the rollout wave instead of all 25.

The AZ constraint ensures that even if an entire wave fails, at most 33% of any single AZ's capacity is affected. Combined with the 5% total fleet cap, this means a worst-case wave failure affects at most `min(5%, 33% of any one AZ)` — bounded blast radius from two independent angles.

---

**Q12: How does the CI/CD system handle secret management to ensure secrets never appear in build logs?**

**Three layers of protection.** First, secrets are never baked into the pipeline definition YAML. Instead, they are referenced by name (e.g., `$(secret.docker-registry-password)`) and injected at pod startup time by the pipeline controller, which fetches the value from HashiCorp Vault or AWS Secrets Manager using the pod's service account identity. The secret value is set as an environment variable; it never touches disk or the pipeline YAML stored in git.

Second, secret scrubbing is applied to all task output. The pipeline controller knows which environment variable names are secrets and redacts their values from log output before writing logs to S3. The scrubber uses exact-match replacement (`***REDACTED***`), not pattern matching, to catch all occurrences.

Third, build pod service accounts have minimal IAM permissions — they can read secrets scoped to their specific repo and environment, and push to specific registry paths. They cannot read secrets belonging to other repos or access production databases.

The trade-off: the first layer (runtime injection) prevents secrets from leaking in the YAML file (which lives in git). The second layer (log scrubbing) prevents accidental debug output from leaking secrets. The third layer (least-privilege IAM) contains the blast radius if a build pod is compromised. No single layer is sufficient; all three together make secret zero-knowledge achievable in practice.

---

**Q13: How does the feature flag service handle the case where the SSE gateway itself is down and SDK instances cannot receive flag updates?**

The SDK's fallback behavior is to continue evaluating flags using the last-known state from its in-process cache. This is the correct behavior for almost all flags: a stale value is better than an error or a random default. SDK instances implement exponential backoff reconnection (1s, 2s, 4s, 8s, cap at 60s) and will restore real-time updates as soon as the gateway recovers.

The exception is the **emergency kill switch**. If the kill switch flag is `false` (feature disabled) in the SDK cache when the gateway goes down, the kill switch works correctly — the feature remains disabled. If the kill switch is `true` (feature enabled) when the gateway goes down, an operator who needs to disable the feature via kill switch during the outage cannot reach the SDKs.

Mitigation: design the kill switch as a separate code path, not a flag evaluation. The application code checks the flag normally but also has a local file or environment variable override (`KILL_SWITCH_FEATURES=new_checkout,risky_algorithm`) that overrides the SDK evaluation. Operators can set this environment variable (e.g., via a Kubernetes ConfigMap update) even when the flag service is down. This is a defense-in-depth pattern: flags for velocity, environment variables for emergency control.

---

### Tier 3: Staff+ Stress-Test Questions (reason aloud)

**Q14: You deploy a new version via canary to 25% of traffic. The new version introduces a subtle memory leak that only manifests after 6 hours of load. Your 5-minute analysis windows all pass. The canary gets promoted to 100%, and 6 hours later your entire fleet OOMs. How would you redesign the canary system to catch this class of bug?**

This is a **slow regression** that operates on a timescale longer than your analysis window. A few approaches, each with trade-offs.

First, **extend soak times at higher weights**. Instead of 5 minutes at each step, require 30 minutes at 25%, 1 hour at 50%, and 2 hours at 100% before declaring success. This catches slow regressions but extends the full canary cycle to 4–6 hours. For most teams this is too slow; you would need to run canaries overnight.

Second, **add memory and GC metrics to the analysis template**. The 5-minute window passes for error rate and latency, but a memory leak would show as rising `container_memory_working_set_bytes` over time. Alert when the canary's memory growth rate (bytes/minute) exceeds the stable's memory growth rate by more than a tolerance. This requires instrumentation of your memory usage and a trend analysis query rather than a point-in-time query.

Third, **integrate long-running soak tests in staging**. Before any canary reaches production, the artifact runs for 24 hours in a staging environment under synthetic load with memory profiling enabled. This catches slow leaks before they ever hit production. The cost is a 24-hour delay in the promotion pipeline — acceptable for releases, potentially not for hotfixes.

Fourth, **capacity-based signals**: track whether pods are being OOM-killed in the canary subset (`kube_pod_container_status_restarts_total` metric). An OOM kill is a hard signal even if error rates look fine (the process restarts before serving errors).

My recommendation is layers 2 and 3: instrument memory trend in the analysis engine (catches it earlier) and add a 24-hour staging soak for major releases. Layer 1 (extended soak times) is the escape hatch for when you are forced to deploy without a full staging cycle.

---

**Q15: Your organization runs both Kubernetes and a bare-metal fleet of 50,000 hosts. You need to coordinate a deployment where a new gRPC service version must be deployed to all Kubernetes pods AND all bare-metal hosts, with the constraint that the Kubernetes pods must finish their rollout before the bare-metal hosts start. How do you design this orchestration?**

This is a **cross-system coordination** problem — you are linking two different deployment systems (ArgoCD/canary controller for Kubernetes, rollout controller for bare-metal) with a dependency constraint.

The key design principle is **event-driven coordination**: the Kubernetes deployment completion is an event; the bare-metal rollout listens for that event as its trigger. Do not build a monolithic orchestrator that owns both systems.

Concretely: the CI/CD pipeline creates two tasks in a DAG. Task A is the Kubernetes deployment (canary to 100% via Argo Rollouts). Task B is the bare-metal rollout via the rollout controller API. Task B has a dependency on Task A (`runAfter: [kubernetes-canary-complete]`). The pipeline controller waits for Task A to emit a `succeeded` status (which Argo Rollouts sends as a Kubernetes event or a webhook) before scheduling Task B.

The coordination layer is the pipeline itself (Tekton or Argo Workflows). Both deployment systems expose a "wait for completion" API (Argo Rollouts has `GET /apis/argoproj.io/v1alpha1/namespaces/{ns}/rollouts/{name}` with `status.phase`; the rollout controller exposes `GET /api/v1/rollouts/{id}` with a `status` field). The pipeline controller polls or webhook-receives completion before proceeding.

The risk is partial failure: Kubernetes rollout succeeds, bare-metal rollout fails. Now you have a version mismatch between your k8s and bare-metal fleets. The rollout controller must expose a rollback API, and the pipeline must trigger that API on failure. The coordination DAG needs explicit failure-path edges: if Task B fails, trigger rollback of Task A. This is why having both systems emit status to a shared Kafka topic is valuable — the pipeline can correlate events and trigger compensating transactions.

---

**Q16: You are asked to design a feature flag system for a mobile app where 50 million users install the app and you cannot guarantee they will be online to receive SSE updates. How does the architecture change from the server-side SDK design?**

The **client-side (mobile) architecture** is fundamentally different from server-side for three reasons: you cannot maintain persistent connections to 50 million mobile clients, you cannot trust the client with targeting logic (a sufficiently determined user could inspect the flag rules to predict which features are coming), and mobile clients have intermittent connectivity.

The primary change is the **evaluation model**. For mobile, you cannot push all 10,000 flag rules to the client because targeting rules might reveal confidential product roadmap. Instead, the mobile SDK polls the flag server at app startup and at periodic intervals (e.g., every 30 minutes) with the user's identity context. The server evaluates all targeting rules server-side and returns only the resolved values — no rules, just `{"new_checkout": true, "dark_mode": false}`. The client caches these resolved values locally and uses them between polls.

The **update propagation model** shifts from push (SSE) to pull (polling). This increases the propagation latency from < 5 seconds (server-side SSE) to up to 30 minutes (next poll cycle). For emergency kill switches, you implement push notifications (APNs/FCM) that wake the app and trigger an immediate poll. This restores < 5 minute propagation for critical flags without maintaining persistent connections.

The **scale changes dramatically**. 50 million app sessions with 30-minute poll intervals = ~1.7 million poll requests per second. The flag evaluation server is now in the hot path. You need aggressive caching: for each user, cache the resolved flag set for 5 minutes. If the user's targeting attributes have not changed and no flags have been updated, return the cached response. Cache invalidation: when any flag changes, version the flag set (monotonic counter). The client sends its current version; if the server's version is newer, recompute and return. Otherwise, return HTTP 304 Not Modified.

The **security model also changes**: server-side evaluation means you can safely include user PII (email, subscription tier) in targeting rules because those rules never leave the server. The mobile client only sends the attributes you ask for (user ID, country, app version) and gets back resolved boolean values.

---

## STEP 7 — MNEMONICS

### The "BADCFR" Acronym

Remember the 6 problems in this cluster with: **B**lob registry, **A**tomic switch, **D**ifferential canary, **C**ontinuous pipeline, **F**lag service, **R**ollout controller.

Or in plain English, remember this progression: **first you store the artifact (registry), then you build it (pipeline), then you gate it (feature flags), then you ship it three ways — all at once (blue-green), gradually by traffic (canary), or gradually by host (rollout controller).**

### The "SPACIAL" Framework for Any CI/CD Question

When the interviewer gives you a CI/CD problem and you need a moment to organize your answer, run through **SPACIAL**:

- **S**cale — how many services, hosts, engineers, runs/day?
- **P**romotion path — how does an artifact move from dev to prod?
- **A**rtifact — what is being built and how is it identified (SHA-256 digest)?
- **C**ontrol plane — which component orchestrates the deployment (controller)?
- **I**nstrumentation — how do you know the deployment is healthy (Prometheus)?
- **A**utomate rollback — what triggers it and how fast?
- **L**og it — who changed what and when (audit trail)?

### Opening One-Liner

If the interviewer asks "how would you approach a CI/CD system design?", open with:

"The core challenge in CI/CD is separating build from deploy and deploy from release — you want to commit code knowing it will arrive safely in production, meaning the pipeline builds and tests in isolation, the artifact registry stores content-addressed immutable versions, and the deployment system moves traffic progressively while a Prometheus-backed health gate automatically rolls back if error rates or latency spike."

This one sentence covers artifact registry, CI pipeline, progressive deployment, health gating, and automated rollback — the five sub-pillars of the entire domain.

---

## STEP 8 — CRITIQUE

### What Is Well-Covered in These Source Materials

The source files are exceptionally thorough on the **mechanical details** of each system. The Kubernetes implementation specifics (Service selector patch, Argo Rollouts YAML, Tekton task matrix) are production-quality and reflect real-world practice. The SQL schemas are detailed and correct, including the right ENUM types, unique constraints, and indexes for common query patterns. The PromQL queries for canary analysis are exact and usable. The expand-contract database migration is explained with concrete 4-phase steps that candidates can actually recite. The capacity estimates are grounded with real numbers that hold up to scrutiny.

### What Is Missing, Shallow, or Would Be Probed Further

**Multi-region coordination is underspecified.** The artifact registry covers geo-replication (blobs pushed to secondary regions), but the source materials do not address what happens when a deployment runs simultaneously in US-East and EU-West. For canary, each region runs its own canary independently — but who decides if the EU-West canary should roll back if US-East metrics are good and EU-West metrics are bad? Cross-region deployment coordination (phased by region, region-by-region promotion) is a real architectural question that senior interviewers probe.

**The network partition failure mode is missing.** What happens when the rollout controller loses connectivity to a subset of bare-metal hosts? The source says "mark hosts as failed after timeout" but does not address the split-brain scenario: the controller believes a host updated successfully (gRPC acknowledged the command), the host successfully updated, but the health report is lost due to network partition. The host is now `unknown` health status. The wave times out. This creates a situation where the wave "failed" from the controller's perspective, but the hosts are actually healthy. Idempotent re-apply handles this (re-deploying to already-updated hosts is a no-op), but this scenario is not explicitly discussed.

**Secret rotation during deployment is missing.** The CI/CD pipeline covers "inject secrets at pod startup," but what happens when a secret rotates mid-deployment? A canary at 50% traffic has half the pods using the old database password and half using the new one. If the old password is revoked before the canary completes, Blue pods in blue-green fail. This is a real operational gap in the source material.

**The connection pool problem during blue-green switch is not addressed.** Downstream services that call the switching service via gRPC or HTTP/2 may have connection pools to the Blue pods. After the switch, new connections go to Green, but pooled connections still point to Blue pods. If Blue is scaled down quickly, those pooled connections break. The solution (connection draining, pool refresh after switch) is standard knowledge but absent from the source material.

**Performance testing in the pipeline is absent.** The CI/CD pipeline covers unit tests, integration tests, SAST, and container scanning, but not load testing or performance regression testing. A new service version that passes all functional tests but has 3x higher p99 latency will not be caught until it hits the canary analysis in production. Many FAANG-level pipelines include a mandatory performance gate in staging.

### Senior Probes to Prepare For

1. "How do you handle a deployment where the new version requires a breaking change to an API that 30 other internal services depend on?" (Answer: versioned APIs, consumer-driven contract testing, feature flags to gate the breaking change, coordinated deployment with the consumers)

2. "Your canary system auto-rolls back 10% of deployments. How do you determine if that number is too high, too low, or about right?" (Answer: track rollback root causes — are they real regressions or false positives? Compare pre-canary production incident rate to post-canary. Tune thresholds based on false positive/negative history.)

3. "How would you design this system to support compliance requirements where every production deployment must be approved by two senior engineers and the approval must be non-repudiable?" (Answer: approval gates with PKI-signed approvals, audit log entries with cryptographic signatures, automated enforcement that the pipeline only promotes after two approved signatures are recorded in the immutable audit store)

4. "What happens to your CI/CD system when GitHub Enterprise is down?" (Answer: all pipeline triggers stop — you need a manual trigger API in the pipeline controller; cached pipeline definitions let you re-trigger without cloning; operations that do not require new code can proceed; feature flag toggles are completely decoupled from the pipeline)

### Common Traps That Candidates Fall Into

**Trap 1: Conflating mutable tags with content-addressed deployment.** Many candidates say "deploy version 1.2.4" meaning "deploy the Docker image tagged `v1.2.4`." Tags are mutable pointers — `v1.2.4` can be moved to a different digest. The correct answer is always: deploy by **SHA-256 digest** (`sha256:abc123...`). Tags are for human readability; the digest is the identity.

**Trap 2: Proposing DNS-based traffic switching for blue-green.** DNS TTL means switching takes 30 seconds to 5 minutes. This violates the "< 1 second switch, < 5 second rollback" requirement. DNS switching is appropriate for region-level failover, not for service-level blue-green in a Kubernetes cluster.

**Trap 3: Designing feature flags as a configuration management system.** The interviewer wants a feature flag service, not a config store. Feature flags are for gating code paths (boolean gates, user targeting, percentage rollouts) — not for storing database connection strings, timeouts, or batch sizes. The architecture is completely different (in-process SDK vs. config map, SSE push vs. config pull, evaluation context vs. raw values).

**Trap 4: Ignoring the database during blue-green.** Candidates who draw two separate environments without mentioning that they share a database are missing the hardest part of the problem. An interviewer will immediately ask "what about the database?" — you should bring it up yourself and describe expand-contract before being asked.

**Trap 5: Proposing 100% automated rollbacks for the rollout controller.** Unlike blue-green and canary (where rollback is instantaneous and cheap), bare-metal rollout rollback means re-deploying the old version to potentially hundreds of already-updated hosts. This is a 10+ minute operation that consumes rollout controller capacity. Auto-pause and page a human is the correct answer for wave-based bare-metal rollouts. Auto-rollback is correct for blue-green and canary.

---

*End of Infra-11 CI/CD & Deployment Interview Guide. Review Steps 3e (HLD components), 4 (common components), and 5 (differentiators) before your interview. Practice the Tier 3 questions aloud — the reasoning process matters more than the answer.*
