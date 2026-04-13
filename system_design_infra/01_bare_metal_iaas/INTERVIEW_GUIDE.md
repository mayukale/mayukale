Reading Infra Pattern 1: Bare Metal & IaaS — 4 problems, 8 shared components

---

# Infra-01: Bare Metal & IaaS — Complete Interview Study Guide

This guide covers four system design problems that all belong to the same physical infrastructure layer: the Bare Metal Reservation Platform, the IaaS Platform Control Plane, the Machine Pool Manager, and the Self-Service Developer Portal. If you are interviewing for a cloud infrastructure platform engineer role at FAANG or a hyperscaler, one or more of these will appear in some form. This guide is fully self-contained. You do not need any other document to use it.

---

## STEP 1 — THE FOUR PROBLEMS AT A GLANCE

Before drilling into frameworks and Q&A, anchor yourself on what each problem is actually asking you to build.

**Bare Metal Reservation Platform** — A calendar and booking system for physical servers, except each "room" costs $200K and a double-booking destroys credibility and money. Engineers specify hardware (GPU type, CPU count, RAM, NVMe), a time window, and a count. The system finds machines, prevents overlaps, triggers provisioning, and manages the lifecycle from `requested` through `active` to `released`. Scale: 50,000 machines, 500 reservation requests per second peak.

**IaaS Platform Control Plane** — The full cloud control plane that abstracts physical hardware into programmable compute (VMs and bare metal), network (SDN, floating IPs, security groups), and storage (Ceph block, object, filesystem) through a unified REST API. OpenStack-compatible. Scale: 100,000 physical servers, 500,000 VMs, 10,000 tenants, 5,000 API calls per second.

**Machine Pool Manager** — The fleet lifecycle layer that nobody thinks about until machines start failing at 3 AM. Groups machines into logical pools, runs continuous health scoring on every machine (0–100 score from IPMI telemetry, GPU sensors, disk SMART, network error counters), auto-ejects degrading machines, manages rolling firmware upgrades across the fleet with blast-radius controls, integrates with vendor RMA portals, and forecasts capacity demand 2–8 weeks ahead. Scale: 100,000 machines, 500 pools.

**Self-Service Developer Portal** — The human-facing surface of all the above. A React web portal plus a Go CLI (`infra-cli`), backed by a Workflow Engine that executes Infrastructure-as-Code templates (Terraform/YAML/HCL), enforces multi-step approval chains (manager approval at $10K, VP approval at $50K), provides real-time cost dashboards with budget enforcement, and sends notifications via Slack, email, and PagerDuty. Scale: 5,000 portal users, 10,000 CLI sessions.

All four problems share the same underlying infrastructure (MySQL, Redis, Kafka, API Gateway) but have meaningfully different core algorithms and data models. Step 5 breaks down exactly what is unique to each one.

---

## STEP 2 — MENTAL MODEL

### The Core Idea

These four problems are all variations on the same theme: **turning raw physical hardware into safely-shareable, observable, programmable resources with strong consistency guarantees where money or correctness is on the line, and eventual consistency everywhere else.**

The word "safely" is doing most of the work. A $200K H100 server cannot be double-booked. A VM cannot be placed on a host that has no capacity. A machine with failing memory cannot silently serve production traffic. A $100K GPU cluster reservation cannot be provisioned without a VP's approval. Every hard design decision in these four systems traces back to: "what happens if we get this wrong, and how bad is it?"

### The Real-World Analogy

Think of a high-end hotel chain. The **Reservation Platform** is the booking system — it holds your room for a specific window and prevents double-booking. The **IaaS Control Plane** is the hotel operations system — it manages all the rooms (VMs), the shared facilities (networks, storage), and coordinates housekeeping, front desk, and maintenance. The **Machine Pool Manager** is the facilities and maintenance department — it tracks the health of every room and piece of equipment, schedules preventive maintenance, handles warranty claims when the HVAC breaks, and forecasts when you will need more rooms. The **Developer Portal** is the guest-facing app and concierge service — the interface where guests (engineers) book, manage, and pay for everything, with approval required for presidential suites.

### Why This Is Hard

Four specific reasons:

**1. Correctness under concurrency at the worst possible moment.** The most expensive machines are the most in demand. Two teams will simultaneously try to reserve the last 8 H100 GPUs at exactly the same time. If your conflict detection has a race condition, you double-book a machine that costs more than most engineers' annual salary. The naive solution (just use a database lock) works but destroys throughput. The correct solution (in-memory pre-filter + pessimistic DB lock as the authoritative gate) requires understanding both the fast path and the correctness path.

**2. Provisioning is a 15-minute multi-step saga touching heterogeneous systems.** IPMI for power control, PXE for OS install, SDN controller for networking — each has different failure semantics, different retry behaviors, and different rollback complexity. If any step fails halfway through, you need to undo exactly what you did, or the machine ends up in a ghost state that no one can use. Designing idempotent, compensatable provisioning steps is genuinely hard.

**3. Fleet health at scale has no finish line.** 100,000 machines, checked every 15 minutes each = 110 health checks per second, continuously, forever. Hardware degrades gradually. A DIMM that is throwing 5 correctable ECC errors per hour today will be throwing 500 next week and then cause data corruption. The system must detect the trend, not just the binary healthy/unhealthy state, and act on it before the machine fails in production. That requires a thoughtful scoring model, not a simple threshold.

**4. The control plane must survive its own failures without affecting the data plane.** Running VMs must keep running even if the IaaS control plane goes down. Scheduling, image distribution, and metering can be unavailable; existing workloads cannot be. This separation of control plane and data plane is easy to say but requires architectural discipline throughout the design.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

These are the questions you should ask in the first 5 minutes of the interview regardless of which of the four problems you are given.

**Question 1: Is this an internal platform for engineers, or a public cloud offering for external customers?**
This changes RBAC complexity, multi-tenancy isolation requirements, SLA enforcement strictness, and billing precision. An internal platform can trust its users somewhat. A public offering cannot.

**Question 2: What is the primary workload — VMs, bare-metal, or both? And are GPUs involved?**
Bare-metal with GPUs is the hardest case: no overcommit, topology-aware placement, expensive hardware, long provisioning times, firmware sensitivity. VMs on general-purpose hosts are simpler. Knowing the workload type anchors every subsequent decision.

**Question 3: What are the scale targets — how many machines, how many tenants, what peak request rate?**
50K vs 100K machines is a different sharding story. 500 requests per second vs 5,000 is a different caching story. Nail these numbers early so your capacity math lands in the right ballpark.

**Question 4: Do we need OpenStack compatibility, or are we building a greenfield API?**
OpenStack compatibility adds a significant facade layer, microversion handling, and Tempest testing overhead. If the interviewer says greenfield, you can simplify your API design considerably. If they say compatibility required, flag that as a major complexity driver.

**Question 5: What is the consistency requirement for resource allocation — can we ever tolerate a brief double-booking in exchange for lower latency, or is it zero tolerance?**
This is almost always zero tolerance for bare metal and quota enforcement. But asking the question signals you understand the trade-off between availability, consistency, and performance.

**What changes based on the answers:**
- Internal platform → lighter RBAC, simpler billing, tighter latency SLAs acceptable
- GPUs present → topology-aware placement, no overcommit, NVLink rack co-location become critical
- OpenStack required → add API facade layer, microversion registry, Tempest suite
- High scale → cell-based architecture, Vitess sharding, per-AZ worker deployment

**Red flags that suggest you are off track:**
- Jumping to "use Cassandra for everything" without addressing the consistency requirement for resource allocation
- Designing a single global database without discussing sharding or cell architecture at the 100K machine scale
- Forgetting the data plane / control plane separation — if your design crashes all running VMs when the scheduler goes down, you have failed
- Not asking about GPU topology (NVLink vs PCIe) for bulk reservations — this is a NVIDIA-specific concern the interviewer will probe

---

### 3b. Functional Requirements

**Core requirements across all four problems:**

1. Resources (machines, VMs, reservations) have explicit state machines with guarded transitions. Every state change is persisted durably and audited.
2. Resource allocation is conflict-free: no two reservations can share the same physical machine at the same time; no two VMs can be placed on a host that lacks capacity for both.
3. Provisioning is a multi-step workflow that completes asynchronously but produces observable status at every step.
4. Multi-tenant isolation: tenants cannot see or affect each other's resources.
5. Self-service with guardrails: engineers can provision without involving an ops team, but expensive or risky actions require approval.

**Scope boundaries to state explicitly:**
- We manage the lifecycle of resources from reservation/creation to release/termination. We do not own physical data center operations (racking, cabling, power) or the BMC firmware itself.
- Billing calculation and invoicing are downstream consumers of our events; we emit metering events but do not compute bills.
- Network fabric design (BGP, EVPN-VXLAN topology) is a precondition, not something we design.

**Clear one-sentence statement of each problem:**

Reservation Platform: "Reserve specific physical machines by hardware spec and time window, guarantee no double-booking, and orchestrate their provisioning."

IaaS Control Plane: "Expose compute, network, and storage as programmable REST APIs over a multi-tenant pool of physical servers, maintaining resource consistency and usage metering."

Machine Pool Manager: "Continuously monitor fleet health, automatically eject degrading machines, manage firmware rollouts with blast-radius controls, and forecast capacity needs."

Developer Portal: "Give engineers a single self-service interface — web and CLI — to provision infrastructure via templates, with approval workflows, cost visibility, and budget enforcement."

---

### 3c. Non-Functional Requirements

**How to derive them in the interview (do not just memorize the numbers):**

Start with the SLA question: "What is the consequence if this API is down for one minute?" For resource allocation, the answer is "engineers cannot provision new resources, but existing ones keep running." That tells you the control plane needs 99.99% (52.6 minutes per year), not 99.999%. The data plane (running VMs) needs 99.999% because downtime there means production outages.

For consistency, ask: "Can we ever show a machine as available that is actually reserved?" The answer is no, because the financial and operational consequence is immediate and direct. That tells you strong consistency for all write paths, with eventual consistency (less than 5 seconds) acceptable for dashboards and status views.

For latency, derive from user behavior: "If an engineer is waiting for a CLI command to confirm a reservation, how long is acceptable?" Under 200ms feels instant. Over 2 seconds feels broken. That gives you p99 < 200ms for the reservation API.

**Key NFR table (anchor numbers for the interview):**

| System | Availability | Write Latency P99 | Provisioning P99 | Scale |
|---|---|---|---|---|
| Reservation Platform | 99.99% | 200ms | 15 min (bare metal) | 50K machines, 500 req/sec |
| IaaS Control Plane | 99.99% control / 99.999% data plane | 500ms | 60s VM, 15 min BM | 100K servers, 500K VMs, 5K req/sec |
| Machine Pool Manager | 99.99% API | 50ms pool query | 15–45 min firmware | 100K machines, 500 pools |
| Developer Portal | 99.9% UI / 99.99% API | 2s CLI, 3s page | N/A (delegates) | 5K users, 10K CLI sessions |

**Key trade-offs to name unprompted:**

✅ Strong consistency for resource allocation prevents double-booking but requires pessimistic locking, which increases write latency under contention. We accept the latency because the correctness risk is a direct financial loss.

❌ Fully synchronous provisioning would give users real-time feedback but is impossible for 15-minute bare-metal workflows. We accept the async model (return a reservation ID immediately, poll or webhook for completion) because the alternative would mean API timeouts.

✅ Cell-based architecture isolates failures to one AZ and lets each cell scale independently, but adds complexity to cross-cell operations like listing all instances for a tenant.

❌ Eventual consistency for dashboards (up to 30 seconds lag) means engineers may see stale data briefly, but it lets us serve dashboard reads from replicas and caches without hitting the primary under load.

---

### 3d. Capacity Estimation

**The right formula to use in the interview:**

For storage: `(records per day) × (bytes per record) × (retention days)` = hot storage needed.
For bandwidth: `(requests per second at peak) × (average payload size)` = throughput.
For DB write load: `(peak write QPS) × (average transaction size)` vs single-node MySQL capacity (~10K simple writes/second on NVMe) to decide whether you need sharding.

**Anchor numbers to remember:**

- Reservation Platform: 50K machines, 10K concurrent active reservations, 600 writes/sec peak (3x average). Storage: 3.6 GB/year for reservations, 110 GB/year for audit logs. Redis availability cache: 10 MB — trivially small, always keep in memory.
- IaaS Control Plane: 100K physical servers, 500K VMs. Metering events: 12M/day at 200 bytes each = 2.4 GB/day, 3.6 TB/year. OS images: 2 GB each, per-AZ cache required for provisioning throughput. Peak API: 15,000 req/sec (3x average of 5,000).
- Machine Pool Manager: 1,100 health data points per second (110 machines/sec × 10 metrics each). Health check records: 9.5M/day × 500 bytes = ~4.3 GB/day, needs monthly partitioning and tiered retention (90 days hot, 1 year cold).
- Developer Portal: Low volume by comparison. 200 page requests/sec, 1,000 template applies/day. The interesting storage is cost aggregates: 50K hourly aggregations/day, partitioned monthly.

**Architecture implications from the numbers:**

- 600 write/sec to MySQL is well within a single primary's capacity. You do not need to shard the Reservation Platform. State this explicitly — it shows you can resist premature complexity.
- 12M metering events/day cannot go directly into MySQL without partitioning. The IaaS metering table is partitioned monthly and eventually archived to object storage.
- 1,100 health data points/second is clearly a time-series problem, not a relational problem. That is why Machine Pool Manager uses Prometheus/VictoriaMetrics for health metrics rather than MySQL.
- At 500K VMs, a global scheduler would be a bottleneck. This is the trigger for the cell-based architecture (~5,000 hosts per cell).

**Time yourself:** Capacity estimation in an interview should take 5–7 minutes. Do not spend 20 minutes on it. Pick 3–4 numbers that matter for your architectural decisions, derive them quickly, and move on.

---

### 3e. High-Level Design

**The 5 components every one of these systems shares (draw these first):**

1. **API Gateway** — TLS termination, JWT validation, rate limiting per tenant, request routing, API versioning. Kong or Envoy. This is your single entry point. Draw it at the top.

2. **Core Service(s)** — Stateless Java/Spring Boot service(s) that contain the business logic. Stateless means all state is in the database, not in the service. Deploy 4–12 replicas behind the gateway.

3. **MySQL Primary with Replicas** — InnoDB, semi-synchronous replication, the authoritative source of truth for all resource state. RPO = 0. Reads go to replicas for dashboards; writes always go to the primary.

4. **Redis Cluster** — The hot-path cache. Sorted sets for availability indexes, hash maps for capacity counters, key-value for session data. Cache-aside pattern with Kafka-driven invalidation.

5. **Kafka** — The event bus for lifecycle events. Topic naming: `<entity>.<action>`. Idempotent producer + transactional consumer for exactly-once delivery. Consumers include notification service, metering, cache invalidation, and downstream systems.

**Whiteboard order:** Draw the API Gateway first, then the core services fanned out below it, then the data stores below them, then the async systems (Kafka consumers, provisioning workers) on the right side. Do not start drawing databases before you have drawn the services — it signals bottom-up thinking.

**Key decisions to announce as you draw:**

- "I am making the core services stateless so I can scale them horizontally without coordination."
- "I am using MySQL rather than Cassandra for resource state because I need ACID transactions for conflict detection — Cassandra's eventual consistency would allow double-booking."
- "I am separating the provisioning workers from the core reservation service because provisioning is long-running, hits BMC networks, and must be co-located per AZ for low latency to IPMI interfaces."
- "Kafka decouples the reservation service from billing, notifications, and cache invalidation. The reservation service only needs to know that the event was durably written; it does not need to wait for every downstream consumer."

**Data flow for "Create a Reservation" (say this aloud as you draw):**
1. POST /api/v1/reservations hits the API Gateway for auth and rate limiting.
2. Reservation Service checks Redis for candidate machines matching the hardware spec.
3. In-memory interval tree does O(log n) conflict pre-check per candidate.
4. MySQL `SELECT FOR UPDATE` acquires pessimistic locks on the winner machines.
5. Re-verify no conflicts under lock (double-check — the interval tree is just a filter).
6. INSERT reservation + UPDATE machine states + INSERT audit log in a single transaction.
7. Publish `reservation.confirmed` to Kafka. Return 201 to client.
8. Provisioning Orchestrator picks up the Kafka event and starts the IPMI → PXE → OS → VLAN → health-check workflow asynchronously.

---

### 3f. Deep Dive Areas

These are the three areas an experienced interviewer will probe. Go deep on these without being asked.

**Deep Dive 1: Conflict Detection — The Double-Booking Race Condition**

The problem: Two reservation requests arrive simultaneously for the last available H100 GPU server. How do you guarantee exactly one succeeds without making every reservation acquire a full table lock?

The two-level solution that interviewers want to hear:

Level 1 — **In-memory interval tree** (the fast filter): Each service replica maintains an augmented BST keyed by machine ID. Each node stores a time interval `[start, end]` and a `maxEnd` augmentation that enables O(log n + k) overlap detection. On each reservation request, query the tree for candidate machines of the requested type. This eliminates 95%+ of obviously-conflicting requests without touching MySQL. The tree is seeded from MySQL on startup and kept in sync via a Kafka consumer on `reservation.confirmed` and `reservation.released` events. Critically, the tree is **only a filter**, not the authority.

Level 2 — **MySQL `SELECT FOR UPDATE`** (the correctness gate): After the interval tree narrows the candidates, acquire pessimistic row locks on those machines. Under the lock, re-query the `reservation_slots` table for actual conflicts. If none found, INSERT the reservation. Commit. The MySQL transaction is the only thing that guarantees no double-booking. The interval tree just makes the common case (obviously available) fast.

Why not optimistic locking for the creation path? Optimistic locking (read version, update with WHERE version=N) works well when contention is low. For popular machine types under burst traffic (5–10 concurrent requests for the same machine type), optimistic retries cascade into a retry storm. Pessimistic locking has more predictable latency at this contention level. We do use optimistic locking for reservation modifications (extend, cancel) where contention is low.

Why not Redis Redlock? Martin Kleppmann's analysis showed Redlock has correctness issues under clock skew and network partitions. More importantly, we already have MySQL for ACID storage of reservation state — using its native row-level locking is simpler, correct, and avoids adding another distributed system dependency. Minimize the number of distributed systems you operate.

✅ **Trade-off to announce unprompted:** The interval tree requires memory proportional to active reservations per machine. At 20 active reservations per machine × 50,000 machines, this is manageable. If reservation granularity were minute-level (thousands of short slots per machine), the tree memory would become a concern and we would switch to a pure DB approach with aggressive indexing.

**Deep Dive 2: Provisioning Orchestrator — The 15-Minute Saga**

The problem: Provisioning a bare-metal machine involves 5+ steps (IPMI power cycle, PXE boot configuration, OS install wait up to 10 minutes, VLAN configuration, health check) across heterogeneous systems. Any step can fail. Some failures are transient (retry helps), some are permanent (BMC unreachable = mark failed, get different machine). The whole thing must be durable across service restarts and observable in the portal.

**The tool that solves this cleanly is Temporal.io.** Temporal provides durable execution (workflow state survives crashes and is replayed on restart), per-activity timeout and retry policies, heartbeat-based detection of stuck activities, a visibility UI for debugging, and saga-style compensation (rollback in reverse order). The alternative of building your own DB-backed state machine with a cron poller requires you to manually implement all of that, which takes months and tends to have correctness bugs in edge cases.

The workflow as a DAG of activities with compensation:
1. Validate machine state (`reserved`)
2. IPMI power cycle ON — compensation: IPMI power OFF
3. Configure PXE boot for the machine's MAC address — compensation: clear PXE config
4. Wait for OS installation (up to 10 minutes, heartbeat every 30 seconds)
5. Configure network VLAN via SDN controller — compensation: remove VLAN config
6. Run health check — if fails: mark machine `failed`, trigger RMA, abort
7. Update machine state to `in_use`, reservation to `active`

Each activity is designed to be **idempotent**: IPMI power-on when already powered on is a BMC no-op. PXE config is keyed by MAC address and is an upsert. SDN API calls are keyed by (machine_id, vlan_id) and are idempotent. State updates use `UPDATE ... WHERE state = 'expected'` to prevent duplicate transitions.

❌ **Failure scenario to discuss:** What if the compensation itself fails? Compensation activities have aggressive retry (5 attempts). If compensation still fails, the workflow transitions to `requires_manual_intervention` (not `failed`), an alert fires, and on-call engineers resolve manually. A background "zombie detector" scans for machines stuck in `provisioning` for more than 30 minutes. This is the correct pragmatic answer — compensation failure is rare, accepting a manual fallback for it is the right trade-off.

**Deep Dive 3: Cell-Based Architecture for the IaaS Scheduler**

The problem: Scheduling 500,000 VM placements across 100,000 physical hosts in a single global scheduler means the scheduler is a single point of failure, and every placement decision requires querying global state. At 1,000 placement decisions/second, this becomes a bottleneck.

The solution is **cell-based architecture** (same pattern as OpenStack Nova Cells v2): partition the fleet into independent cells of ~5,000 hosts each, one cell per AZ. Each cell has its own MySQL instance, its own scheduler, and its own compute agents. A global API and "super-scheduler" route incoming requests to the appropriate cell based on the requested AZ. Cell failure is isolated — if Cell A (us-east-1a) goes down, Cells B and C keep running.

The per-cell scheduler uses a **filter-then-weigh algorithm**:
- Hard filters (eliminate candidates): AZ match, enough CPU/RAM/GPU, host is enabled and up, affinity/anti-affinity rules, no imminent maintenance window
- Soft weighers (score remaining candidates): prefer more free RAM (weight 1.0), prefer more free CPU (weight 0.5), strongly prefer available GPUs (weight 2.0), prefer spreading across racks (weight 0.3), prefer less-loaded hosts (weight 0.8)
- Pick top 3 by score, select randomly among them to prevent scheduling hot-spots

✅ **Overcommit ratios to know cold:** CPU 4:1 for general purpose (most workloads do not sustain 100%), 1:1 for GPU hosts (GPU workloads do). RAM 1.5:1 general, 1:1 GPU. Bare-metal: no overcommit ever.

❌ **Stale capacity problem:** The scheduler reads cached host capacity. Between the scheduler's decision and the hypervisor actually creating the VM, another request may have already placed a VM on that host. Solution: two-phase capacity reservation — the scheduler "claims" resources optimistically (increments `used_vcpus` in MySQL within a transaction), the hypervisor validates and creates the VM, if the hypervisor rejects it the claim is rolled back and the scheduler retries on a different host. This claim-then-verify pattern reduces retry rate to < 0.1% under normal load.

---

### 3g. Failure Scenarios

**How to frame failure scenarios at the senior level:** Do not just say "what if the DB goes down." Go through blast radius, detection mechanism, mitigation, RTO, and RPO. This is what separates a senior response from a junior one.

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---|---|---|---|---|---|
| MySQL primary fails | Writes fail in affected cell | MySQL Orchestrator monitor | Auto-failover to semi-sync replica | 30s | 0 (semi-sync guarantees replica has all committed data) |
| Redis cluster fails | All requests hit MySQL (cache miss), higher latency | Redis health checks | Circuit breaker opens, fall through to MySQL; rebuild cache on recovery | Immediate degraded mode | N/A |
| Kafka broker failure | Event delivery delayed | Kafka broker health | 3-broker cluster, replication factor 3, partition leader re-election | 10s | 0 |
| Temporal worker crashes mid-provisioning | That provisioning workflow is stuck until worker restarts | Missed heartbeat detection | Temporal retries activity on a different worker; activities are idempotent | Seconds | 0 |
| Cell AZ power outage | All instances in that AZ unavailable | External monitoring | Instances cannot be migrated (bare-metal). VMs with HA enabled are rebooted in other AZs if anti-affinity was configured. DNS failover routes new provisioning to other AZs. | Hours for affected AZ | Depends on user's cross-AZ replication config |
| Hypervisor crash | VMs on that host go down | Missed heartbeat (3 missed over 45s) | STONITH (IPMI off) to prevent split-brain. HA instances evacuated to other hosts using same Ceph-backed disk. Tenants notified. | 2–5 minutes | 0 (root disk on Ceph survives) |
| SDN controller failure | Cannot create/modify networks; existing network traffic unaffected | Health check | SDN controller is 3-node HA cluster with leader election | 10s | 0 |

**The data plane / control plane separation point (senior framing):** "My design explicitly separates data plane from control plane. If the control plane — scheduler, compute service, reservation API — goes down completely, all currently running VMs continue running. The hypervisor kernel and the Ceph storage cluster operate independently. Control plane downtime means no new instances can be created or modified, but it does not cause production outages. This is a fundamental design principle: control plane SLA is 99.99%, data plane SLA is 99.999%."

---

## STEP 4 — COMMON COMPONENTS

These 8 components appear in all 4 problems. Know each one well enough to explain why it was chosen, how it is configured, and what breaks without it.

---

### Component 1: State Machine for Resource Lifecycle

**Why used:** Every resource in these systems (machine, reservation, VM instance, workflow, RMA ticket) has a strict lifecycle. An explicit state machine with guarded transitions prevents illegal state changes — you cannot go from `available` directly to `in_use` without going through `reserved` and `provisioning`. Multiple services (reservation service, provisioning orchestrator, health checker, admin portal) can all trigger transitions, creating race conditions that the state machine catches.

**Key config:** Spring Statemachine (Java) for the Reservation and IaaS services. Transitions enforced both in code and via a DB-level check constraint as a safety net against direct DB manipulation. The `version INT UNSIGNED` optimistic lock column on every state-bearing table prevents lost updates when two services read the same state and both try to write.

**What breaks without it:** Without an explicit state machine, you end up with `if state == X and this_other_flag and that_other_field` scattered across the codebase. Someone adds a new code path that skips a required step. A machine ends up in `in_use` without ever being provisioned. These bugs are catastrophic in production and nearly impossible to reproduce in testing.

---

### Component 2: Idempotency Key for Exactly-Once Semantics

**Why used:** Network timeouts are real. A client submits a reservation request, the server commits it and sends the response, but the network drops the response. The client retries. Without idempotency, you create a duplicate reservation for the same window. With an `idempotency_key VARCHAR(128) UNIQUE NOT NULL` on the reservations table, the second request hits the unique constraint, the service detects the duplicate, and returns the original reservation's state. The client gets the same answer whether the request ran once or ten times.

**Key config:** Clients provide the idempotency key in a request header (`Idempotency-Key: <UUID>`). The service stores it with a UNIQUE constraint. On duplicate detection, return HTTP 200 with the original response (not 201). Key scope: per-tenant, not global, to avoid cross-tenant interference. Keys expire after 24 hours (configurable).

**What breaks without it:** Double-booking. Double-provisioning. Two instances created for the same workflow step. These are exactly the class of bugs that are hardest to detect in testing and most damaging in production.

---

### Component 3: MySQL InnoDB as Single Source of Truth (RPO = 0)

**Why used:** Every system in this pattern needs ACID transactions for resource allocation — specifically `SELECT ... FOR UPDATE` for conflict-critical sections and atomic multi-table commits (insert reservation + update machine state + insert audit log in one transaction). Cassandra's eventual consistency would allow double-booking. etcd's 8GB data limit makes it unsuitable for resource state at this scale. PostgreSQL would also work (and has range types that are nice for interval queries), but MySQL InnoDB is the choice here for operational maturity, semi-synchronous replication, and Vitess compatibility for future sharding.

**Key config:** MySQL 8.0 InnoDB. `SERIALIZABLE` isolation for conflict-critical transactions (reservation creation conflict check). `READ COMMITTED` for general reads. Semi-synchronous replication with `AFTER_SYNC` wait point — primary flushes binlog, waits for at least one replica ACK, then commits. This gives RPO = 0: even if the primary crashes after flush and before ACK, the replica has the data. Latency cost: ~1–3ms additional versus async replication. Acceptable because write SLO is 200–500ms P99. Vitess available for sharding by `tenant_id` if write volume exceeds single-primary capacity.

**What breaks without it:** Without ACID transactions, the conflict check and reservation insert become two separate operations with a window for a race condition. Another request could commit between your conflict check and your insert. Result: double-booking, data corruption, financial loss.

---

### Component 4: Redis Cluster as Hot-Path Cache

**Why used:** The most common operation in all four systems is a read: "show me available machines," "show me running instances," "what is the health of machines in pool X?" These reads can be served entirely from Redis without touching MySQL. At the reservation platform's scale (50,000 machines, 10MB availability cache), the entire working set fits in RAM trivially. The cache also holds quota counters (write-through, atomically incremented) to avoid a synchronous DB hit on every create operation.

**Key config:** Cache-aside pattern: reads check Redis first, miss falls through to MySQL, result written back to Redis. Invalidation via Kafka consumer: when `reservation.confirmed` is published to Kafka, a consumer deletes or updates the affected cache keys. TTL as a safety net (30 seconds) ensures stale data cannot persist indefinitely even if a Kafka consumer falls behind. Sorted sets (`ZSET`) for machine availability indexed by type and time window. Hash maps for capacity counters. `LRU` eviction for instance status; `no-eviction` for quota counters (these must never be lost silently).

**What breaks without it:** Every API read hits MySQL directly. Dashboard loads (2,000 reads/sec for the reservation platform) saturate the primary or require a large read replica fleet. The p99 for `GET /machines?available=true` goes from 30ms to 200ms. At peak traffic, the DB becomes the bottleneck and you have a latency problem that compounds into a user experience problem.

---

### Component 5: Kafka Event Bus (Exactly-Once, Topic-per-Entity)

**Why used:** Decouples the core service from every downstream consumer. The reservation service does not need to know that billing, notifications, cache invalidation, and analytics all need to hear about `reservation.confirmed`. It just publishes to Kafka and returns to the client. Consumers can be added, removed, or scaled independently. Exactly-once semantics via idempotent producers (`enable.idempotence=true`) and transactional consumers prevents duplicate billing events.

**Key config:** Topic naming convention: `<entity>.<action>` — e.g., `reservation.confirmed`, `machine.state_changed`, `instance.active`, `machine.ejected`, `workflow.step.execute`. Partitioned by entity type (e.g., machine_type) so that events for the same machine type are processed in order by a single consumer. Replication factor 3, minimum in-sync replicas 2. Consumer groups: each downstream (billing, notifications, cache-invalidation) has its own consumer group with independent offsets. Retention: 7 days raw, then archive to S3 for reprocessing if needed.

The **transactional outbox pattern** for exactly-once publication from MySQL: instead of publishing to Kafka directly in the service, write to an `outbox` table within the same MySQL transaction, then a separate outbox poller publishes to Kafka and deletes the row. This guarantees the event is published if and only if the DB transaction committed.

**What breaks without it:** Tight coupling between the reservation service and every downstream. Adding a new consumer (analytics, audit) requires modifying the reservation service. More critically: if billing consumes directly from the reservation service API instead of Kafka, a billing service restart causes it to miss events, creating billing gaps.

---

### Component 6: API Gateway (Kong/Envoy) with Rate Limiting and JWT Validation

**Why used:** TLS termination, JWT validation, per-tenant rate limiting, API versioning, and request routing are horizontal concerns that belong at the edge, not in every individual service. Centralizing them in the gateway means each service can assume the request is authenticated and within rate limits by the time it arrives.

**Key config:** Rate limiting per tenant (not just per IP) is critical for a multi-tenant platform — one tenant's burst should not impact others. Kong's rate-limiting plugin supports Redis-backed shared counters for accurate distributed rate limiting across gateway replicas. JWT validation with local key caching (validate signature locally, no round-trip to identity service on every request) keeps p99 latency at the gateway under 5ms. API versioning via URL prefix (`/v1/`, `/v2/`) allows backward-compatible rollouts.

**What breaks without it:** Services must implement auth and rate limiting themselves, inconsistently. A misbehaving tenant can saturate the reservation service with requests, causing latency spikes for all other tenants. Without TLS termination at the gateway, you need to manage TLS certificates on every service.

---

### Component 7: Pessimistic Locking (`SELECT ... FOR UPDATE`) for Conflict-Critical Sections

**Why used:** At the exact moment when two concurrent requests race for the last available machine, an in-memory check is not sufficient. The only way to guarantee correctness is to serialize the critical section in the database. `SELECT ... FOR UPDATE` acquires row-level locks in InnoDB, ensuring that only one transaction can read-modify-write a given machine row at a time. The second request's `SELECT FOR UPDATE` blocks until the first transaction commits, then reads the updated state.

**Key config:** Used selectively — only for the reservation creation critical section and quota enforcement. Not for general reads or even for reservation modifications (where optimistic locking suffices). Lock timeout set to 5 seconds; if a lock cannot be acquired within 5 seconds, the transaction fails with a timeout error, and the client retries with different candidates (over-select by 2x to have backups). Lock acquisition order is deterministic (ordered by machine_id) to prevent deadlocks.

**What breaks without it:** The double-booking race condition described in Section 3f. Two concurrent requests both see the machine as available in the in-memory cache, both proceed to MySQL, and without a lock they both commit simultaneously. The unique constraint on `reservation_slots (machine_id, start_time, end_time)` would catch this as a constraint violation, but now you are relying on error handling rather than prevention, and the user experience is a 500 error instead of a clean conflict response.

---

### Component 8: Per-AZ Colocation of Workers

**Why used:** IPMI (Intelligent Platform Management Interface) commands for bare-metal provisioning travel over out-of-band management networks that are physically separate from the data network. These networks are local to the data center. Issuing IPMI commands from a central service across a WAN link introduces latency, unreliability, and network hop count that can cause timeouts in the 2-minute window allowed for power cycle confirmation. Per-AZ workers eliminate all of this.

**Key config:** Provisioning workers (Python, for the Reservation Platform), health data collectors (Python, for the Machine Pool Manager), and image caches (for the IaaS Platform) are all deployed on management nodes within each AZ. They communicate with the central orchestrator via Kafka (the orchestrator publishes job events, workers consume from their AZ-specific partition). Each AZ worker has a connection pool to the local BMC network and SDN controller. Worker capacity is tuned per AZ: max 50 concurrent IPMI operations per AZ to avoid overwhelming the out-of-band network.

**What breaks without it:** IPMI timeouts, particularly during the "wait for OS install" step (up to 10 minutes). Cross-AZ latency also breaks the 15-minute end-to-end provisioning SLA, because network round-trips for each IPMI heartbeat accumulate. Additionally, centralizing workers creates a single point of failure: one worker crash affects provisioning across all AZs.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Bare Metal Reservation Platform

**What makes it unique:**

The defining challenge is **temporal conflict detection at scale with strong consistency guarantees**. Unlike the IaaS scheduler (which places VMs on hosts with available capacity right now), the reservation platform must handle future time windows — machines that are available today but already reserved for next Tuesday. This requires a **time-interval data structure** (the augmented interval tree) rather than a simple capacity counter.

The **interval tree** is the algorithmic heart of this problem. Each node stores `[start, end, maxEnd]` where `maxEnd` is the maximum `end` value in the subtree. An overlap query — "is there any reservation on machine M that overlaps with [queryStart, queryEnd]?" — runs in O(log n + k) time. The pruning optimization: if the left subtree's `maxEnd` is before `queryStart`, no overlap is possible in the entire left subtree (skip it). Without this pruning, the tree degrades to O(n). This is a real algorithm question — be ready to write the overlap check on a whiteboard.

**Topology-aware placement** is unique to this problem: when a user reserves 256 H100 GPUs for a distributed training job, they want all 256 machines on the same rack or within the same network switch domain so that NVLink or InfiniBand interconnects are available. This means the conflict detection must also consider `rack_id` and `gpu_interconnect` type, not just time-window conflicts.

**Preemption logic** is unique to this problem: higher-priority reservations can forcibly evict lower-priority ones with a configurable grace period. The preemption engine identifies overlapping reservations of lower priority, sends grace-period notifications, waits, then forces the state transition. This is structurally similar to a priority queue with eviction — a concept from OS scheduling applied to physical hardware.

**Two-sentence differentiator:** The Bare Metal Reservation Platform is the only one of the four systems that must reason about **future time intervals** rather than current capacity — its interval tree, topology-aware placement, and preemption logic are all driven by the fact that machines must be exclusively reserved for specific future windows, not just allocated from a current pool. When an interviewer asks "how do you prevent double-booking?" on this problem, the complete answer involves both the O(log n) interval tree pre-filter and the MySQL `SELECT FOR UPDATE` correctness gate, and you should explain why both layers are necessary.

---

### IaaS Platform Control Plane

**What makes it unique:**

The scope is dramatically broader — this is the only one of the four problems that must design compute, network (SDN), storage (Ceph), identity, image distribution, metering, and quota management as a unified system. The key design decision is **where to draw the boundaries** between these domains without creating a monolith that fails as a unit.

The **cell-based architecture** is unique to this problem at this scale. At 100,000 hosts and 500,000 VMs, a single global scheduler is a bottleneck and a single point of failure. Partitioning into cells of ~5,000 hosts each, with one cell per AZ, provides both scale (cells can be added as the fleet grows) and blast radius isolation (a cell failure affects one AZ, not the whole platform). The global super-scheduler routes requests to the right cell. Cross-cell list operations scatter-gather from all cells.

The **OpenStack compatibility layer** is unique to this problem. Existing tooling (Terraform OpenStack provider, openstackclient, Heat templates) expects Nova, Neutron, Cinder, Keystone, and Glance API shapes. The design choice — API facade (shim layer) rather than running native OpenStack — gives full control over the implementation while preserving ecosystem compatibility. The facade must handle **API microversioning**: clients send `X-OpenStack-Nova-API-Version: 2.67` and expect version-specific behavior.

The **data plane / control plane separation** is most critical here. Running VMs must survive control plane outages. This requires that the hypervisor agent, storage cluster (Ceph), and network forwarding plane (OVS flow rules) all operate independently of the central compute/network/storage services.

**Two-sentence differentiator:** The IaaS Control Plane is the most architecturally broad of the four problems, requiring you to design five distinct services (compute, network, storage, identity, image) with clean API boundaries and a cell-based partitioning strategy that prevents any single cell failure from cascading across AZs. Its defining unique decision is the **OpenStack-compatible API facade** pattern, which preserves ecosystem tool compatibility without the operational burden of running native OpenStack — a distinction between "expose a compatible API shape" and "deploy OpenStack."

---

### Machine Pool Manager

**What makes it unique:**

This problem is entirely about the **operational lifecycle** of the fleet, not about serving user requests. Its users are operators and on-call engineers, not engineers provisioning new resources. The three unique capabilities that set it apart are: weighted health scoring with per-machine-type configurable thresholds, rolling firmware upgrades with automatic blast-radius controls, and vendor RMA integration.

The **weighted health score** is a seven-component composite formula where weights are configurable per machine type. For GPU servers: GPU health gets 30% weight (GPU is the most valuable component), memory ECC gets 20%, everything else is secondary. For CPU-only servers: memory ECC gets 25%, disk health gets 25%, GPU weight is zero. Crucially, the score drives **graduated responses**, not binary ones: score < 90 = generate alert; score < 60 = add to watch list (check every 5 minutes instead of 15); score < 30 = immediate ejection from available pool + create maintenance task + offer replacement to any active tenant. This graduated approach prevents both premature ejection (wasted capacity) and delayed ejection (data loss or job failures).

The **firmware rolling upgrade planner** with blast-radius controls is unique: batch_size=10 means at most 10 machines are in the upgrade state simultaneously. `max_failure_pct=5.0` means if more than 5% of machines in a plan fail the upgrade, the plan is automatically aborted (not just paused — aborted, with an alert). This prevents a bad firmware image from bricking the entire fleet. The upgrade plan must schedule within maintenance windows and must exclude machines that have active reservations.

The **capacity forecasting engine** (ARIMA, exponential smoothing, Prophet) is the only one of the four problems that does predictive analytics. The output is a 90% confidence interval on demand, expressed as a "gap = predicted_demand − current_supply" per pool per week. When the gap is positive and the procurement lead time for H100 GPUs is 12 weeks, the recommendation is to order now.

**Two-sentence differentiator:** The Machine Pool Manager is the only problem in this set that is entirely about keeping existing hardware healthy rather than serving new resource requests — it is the fleet operations control plane, and its unique contributions are the configurable weighted health scoring model that drives graduated automated responses (watch, warn, eject) and the rolling firmware upgrade planner with automatic abort-on-failure-rate controls. The capacity forecasting component is also distinctive: it is the only one of the four problems that does statistical time-series modeling (ARIMA/Prophet) to produce procurement recommendations, which requires understanding both demand forecasting and hardware procurement lead times.

---

### Self-Service Developer Portal

**What makes it unique:**

This is the only problem where the user experience, not correctness of resource allocation, is the primary design concern. The portal is a **thin orchestration layer** on top of the other three systems — it does not own any resource state, it delegates to the IaaS Platform and Reservation Platform for all resource operations. Its unique value is the **workflow engine with approval chains**, the **GitOps-compatible template engine**, and the **real-time cost visibility with budget enforcement**.

The **workflow engine** is a custom DB-backed state machine (MySQL for durability, 1-second poll loop for execution) rather than Temporal or Airflow. The reasoning: the portal's workflows are typically 3–10 steps with 0–2 approval gates. Custom engine at this scale is simpler than operating Temporal. The approval gate is the defining feature: when a workflow step is of type `APPROVAL`, the engine pauses execution, sends a Slack notification to the required approver (with approve/reject buttons), and waits. On approval, the step transitions to `COMPLETED` and the engine resumes. Approval thresholds: < $10K auto-approved, >= $10K needs manager, >= $50K needs manager + VP.

The **template engine** wrapping Terraform is unique in this set. It parses HCL/YAML infrastructure templates, resolves resource dependencies (create network before reservation that references it), validates against the resource catalog and quotas (dry-run), estimates cost, and then executes `terraform apply` against the IaaS platform's custom Terraform provider. Terraform state files are stored in S3. This is the Infrastructure-as-Code layer on top of the three lower-level platforms.

The **Backend-for-Frontend (BFF) pattern** is used to aggregate data from multiple services into portal-optimized payloads. The portal needs to show, on one page, a workflow's status (from Workflow Engine), the current reservation state (from Reservation Platform), the cost to date (from Cost Service), and any pending approvals (from Approvals table). Without a BFF, the browser would need to make 4 separate API calls and stitch together the data. With a BFF, one call returns a dashboard payload.

**Two-sentence differentiator:** The Self-Service Developer Portal is the only problem in the set that owns no resource state and instead orchestrates across all three other systems — its unique design challenge is the approval-gate workflow engine that must durably pause execution for human decisions, resume on approval, and roll back in reverse order on rejection or failure. The key design decision is **custom DB-backed workflow engine over Temporal** — appropriate for 3–10 step workflows with approval gates, where the simplicity of a MySQL-backed state machine with a 1-second polling loop outweighs the operational overhead of running a Temporal cluster.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (5–7 questions, answer in 2–4 sentences)

**Q: Why use MySQL instead of Cassandra for reservation state?**
**KEY PHRASE: ACID transactions and `SELECT FOR UPDATE`** are non-negotiable for conflict detection. Cassandra's eventual consistency model means two concurrent writes can both succeed without being aware of each other — exactly the double-booking scenario we must prevent. MySQL InnoDB's row-level locking serializes the critical section correctly. Cassandra is appropriate for the audit log and time-series health data where eventual consistency is acceptable.

**Q: How do you prevent a reservation request from timing out during conflict check?**
The in-memory **interval tree** serves as a fast pre-filter that runs in O(log n) without touching MySQL, eliminating the vast majority of candidates in under 1ms. Only viable candidates reach the MySQL `SELECT FOR UPDATE` lock path. Lock timeout is set to 5 seconds; requests over-select candidates by 2x so that if the first batch is lock-contested, we immediately try the next batch. The p99 conflict check target is 20ms.

**Q: What does the provisioning workflow do if a machine fails the health check at the end?**
The health check step validates that the provisioned machine matches expected specs (GPU count, memory, firmware version). If it fails, the workflow runs **saga compensation** in reverse order: remove VLAN config, clear PXE config, power off via IPMI. The machine is marked `failed`, an RMA task is created, and the workflow attempts to substitute from the over-selected candidate pool. If substitution succeeds, the reservation is fulfilled. If not, the reservation returns to the waitlist.

**Q: How does the health scoring engine handle a metric that is temporarily unavailable (IPMI timeout)?**
Missing metrics are treated as **neutral, not critical**: a missing metric contributes the neutral score (100) to the weighted sum, not zero. The system tracks which metrics were unavailable in the `details_json` field and flags them separately. If IPMI is unresponsive across multiple consecutive checks, the `bmc_responsive` component scores 0, which by itself drives the composite score below the watch threshold (60) and triggers increased check frequency. Persistent BMC unavailability eventually drives the score below the ejection threshold (30).

**Q: What happens to queued reservations when a machine is released early (job finishes before the reservation end time)?**
The reservation service publishes a `machine.released` event to Kafka. A **waitlist consumer** picks up the event, queries the waitlist table for pending reservations that match this machine type and whose desired time window overlaps with the newly available window, orders them by priority and creation time, and attempts to confirm the highest-priority match. If a match is found, the reservation is confirmed and the provisioning workflow starts. The waitlisted user is notified via Slack/email within 60 seconds of the machine becoming available.

**Q: How do budget limits work in the Developer Portal — can a team exceed its hard limit?**
Budget enforcement uses **pessimistic locking on the budgets table**: before creating a provisioning workflow, the Cost Service executes `SELECT ... FOR UPDATE` on the relevant budget row, checks `current_spend_usd + estimated_cost_usd <= hard_limit_usd`, and only proceeds if within budget. If the hard limit would be exceeded, the request fails immediately with a clear error message showing the current spend, the limit, and the estimated cost of the requested resources. Soft limits trigger a warning notification but do not block provisioning.

**Q: Why is the data plane 99.999% while the control plane is only 99.99%?**
**KEY PHRASE: Control plane and data plane are independent failure domains.** The hypervisor kernel (KVM/libvirt), network forwarding plane (OVS flow rules), and storage cluster (Ceph) all operate without any communication to the central IaaS services. If the compute service, scheduler, or MySQL go down entirely, existing running VMs continue to run — they only stop receiving API responses for new operations. 99.999% is achievable for the data plane because it has no database dependency in the hot path. 99.99% for the control plane is appropriate because it accepts user requests that require DB writes.

---

### Tier 2 — Deep Dive Questions (5–7 questions, answer with why + trade-offs)

**Q: Why use an in-memory interval tree when `SELECT FOR UPDATE` alone is correct?**
Correctness alone is not the problem — the interval tree is a **performance optimization that also reduces lock contention**. Without the tree, every reservation request would issue `SELECT FOR UPDATE` on candidate machine rows directly. At 600 write requests per second during peak, with multiple requests targeting the same popular machine type (H100s), the lock wait time cascades: request A holds the lock for 50ms (scan + verify + insert), requests B through N are blocked. With the interval tree, 95%+ of candidates are eliminated in memory before any lock is acquired, reducing the lock hold time from ~50ms to ~10ms (just verify + insert under lock). This changes lock contention from a cascading queue to a brief serialization, keeping p99 within the 200ms SLA. The trade-off is memory overhead (manageable at ~20 active reservations per machine) and the need to keep the tree in sync via Kafka, which adds a code complexity path that must be tested carefully.

**Q: Walk through what happens when two requests for the same 8 H100 machines arrive 5ms apart.**
Both requests hit different Reservation Service replicas simultaneously. Both query Redis and find the machines available. Both run their in-memory interval trees and find no conflicts (the tree shows the machines as available — neither request has committed yet). Both proceed to MySQL. MySQL serializes them at the row lock: the first request acquires `SELECT FOR UPDATE` on the 8 machines, the second request blocks at the same lock. The first request re-verifies under lock (still no conflict), inserts the reservation, commits, and releases the locks. The second request's `SELECT FOR UPDATE` unblocks, reads the new state (machines are now reserved), finds a conflict, rolls back, and either returns 409 Conflict or routes to the waitlist. The **idempotency key** ensures the first request's response is stable even if the client retried during the lock wait. This is the correct answer: two-level filtering, MySQL as the authority, idempotency as the safety net.

**Q: How do you handle the "stale host capacity" problem in the IaaS scheduler?**
Between the scheduler reading `used_vcpus=60` from Redis and actually committing the VM placement, another scheduler in the same cell may have already placed a VM on that host. The solution is **two-phase capacity reservation**: (1) The scheduler claims resources optimistically by incrementing `used_vcpus` in MySQL within a transaction (not just reading from cache). (2) The hypervisor agent creates the VM and reports success or failure. If the hypervisor rejects (genuinely out of capacity), the scheduler increments `used_vcpus` back (rolls back the claim) and tries the next candidate. The Redis cache is authoritative for reads but not for writes — writes go through MySQL under the two-phase commit. This reduces the retry rate to under 0.1% under normal load. The trade-off is an extra round-trip to MySQL per placement decision, adding ~5ms latency, which is well within the 500ms API P99 budget.

**Q: Why did you choose a custom workflow engine for the Developer Portal instead of Temporal?**
The decision is based on **matching complexity to the problem size**. Our workflows are 3–10 steps with 0–2 approval gates and 1,000 runs per day. Temporal adds operational overhead: you need a 3-node Temporal cluster, understand its deployment model, manage its database (Cassandra or MySQL), and train your team on its workflow/activity programming model. The custom engine — a MySQL-backed state machine with a 1-second poll loop — handles our complexity correctly with zero additional infrastructure. The specific capability that drives this decision is the **approval gate**: pausing a Temporal workflow for human approval is possible but requires custom activities and signal handling that is not meaningfully simpler than our MySQL approach. If workflow complexity grows (50+ steps, complex branching, long-running signals), Temporal becomes the right answer. We build the simpler thing first and migrate if needed.

**Q: How do you ensure exactly-once billing events — neither missing a charge nor double-charging?**
Two mechanisms working together. First, the **transactional outbox pattern**: instead of publishing to Kafka directly in the compute service (where a crash between "commit DB transaction" and "publish to Kafka" would lose the event), we write an `outbox_events` row within the same MySQL transaction as the instance state change. A separate outbox poller publishes to Kafka and deletes the outbox row. If the poller crashes after publishing but before deleting, it re-publishes on restart, producing a duplicate. Second, the **idempotent Kafka consumer in the metering service** handles this: each metering event has a unique `event_uuid`. The metering service inserts into `metering_events` with `ON DUPLICATE KEY IGNORE` using `event_uuid` as the unique key, so duplicate events are silently dropped. The combination of transactional outbox (prevents loss) and idempotent consumer (prevents double-processing) achieves exactly-once billing semantics end-to-end.

**Q: How does the Machine Pool Manager decide when to start a firmware upgrade and how fast to go?**
The firmware upgrade plan is created explicitly by an operator, not triggered automatically, because firmware upgrades are irreversible-by-default operations with real risk. The plan specifies: which pools are affected, the target firmware release, the batch size (default 10 machines per batch), the maximum failure percentage before auto-abort (default 5%), and the maintenance window (e.g., Tuesday/Wednesday 02:00–06:00 UTC). The **blast-radius controls** are critical: if you upgrade 10 machines and 1 fails (10% failure rate > 5% threshold), the plan auto-aborts immediately, protecting the remaining machines from a potentially bad firmware image. The operator investigates, determines whether the issue is firmware-specific or machine-specific, and either patches the firmware release or resumes with affected machines excluded. The rollback version in the `firmware_releases` table allows the Firmware Manager to flash the previous version if needed. This is a classic **canary deployment** pattern applied to physical hardware firmware.

**Q: What does "warm pool" vs "cold pool" mean, and why does it matter for provisioning latency?**
The Machine Pool Manager maintains three states for machines in a pool. **Active machines** are currently in use (reserved/provisioned). **Warm pool machines** have the OS already installed and network already configured — they are ready to serve a new reservation in under 2 minutes (just needs to be assigned, no PXE boot required). **Cold pool machines** are powered off in standby — they require the full 15-minute provisioning workflow. The warm pool absorbs burst demand without waiting for provisioning. The warm pool target is a configurable percentage of the pool size (e.g., 20 machines out of 500 are always warm). When a warm machine is assigned to a reservation, a cold machine is promoted to warm (powered on, OS installed) to maintain the warm pool level. When demand is low, excess warm machines are demoted to cold to save power. The trade-off is the cost of keeping machines powered on and OS-provisioned versus the user experience benefit of sub-2-minute provisioning.

---

### Tier 3 — Staff+ Stress Tests (3–5 questions, reason aloud through them)

**Q: Your reservation API is at 99.99% availability, but the Redis cache is down. Walk through the degraded-mode behavior and when you would page on-call.**

"Let me reason through this systematically. When Redis goes down, every read that would have hit the availability cache now falls through to MySQL. The reservation service has a circuit breaker on Redis: after 5 consecutive Redis failures, the circuit opens and the service goes into degraded mode — all availability queries hit MySQL directly.

At 2,000 read requests per second normally, the MySQL replica cluster now absorbs that full load. MySQL read replicas can handle this — we size them for peak traffic plus the headroom for cache-miss scenarios. Latency for availability queries degrades from ~10ms (Redis hit) to ~50–100ms (MySQL query with `idx_type_state_az`), but stays within the 100ms p99 SLA.

Write performance is unaffected — writes always go to MySQL primary regardless of Redis.

The cache-aside pattern means that when Redis comes back online, the cache rebuilds organically as reads populate it — no explicit cache warm-up required.

I would not page on-call immediately. Redis failure triggers an automated alert, but since the service is degraded but functional (elevated latency, reads going to MySQL), this is a warning not a critical alert. I would page if: (1) MySQL replicas start saturating (latency > 200ms p99), (2) Redis fails to recover within 15 minutes, or (3) error rate on reservation creation exceeds 1%. The key insight is that degraded mode must be tested in advance so we know it works — a Redis outage in production is not the time to discover that the fallback path has a bug."

**Q: A tenant submits a bulk reservation for 256 H100 machines. Halfway through provisioning (128 machines done, 128 remaining), a networking issue takes down the SDN controller for 20 minutes. What happens?**

"This is a distributed saga failure during execution. Let me trace through it.

The 128 machines that completed provisioning are in `in_use` state and have active VLAN configurations — they are unaffected by the SDN controller going down because the network forwarding plane (OVS flow rules already programmed on the switches) is independent of the SDN controller's availability. Those machines continue to work.

For the remaining 128 machines, the provisioning workflow reaches the `configure_network_vlan` activity and fails with a connection error to the SDN controller. Temporal retries this activity with exponential backoff (10 seconds, 20 seconds, 40 seconds...) for up to the configured retry count. The heartbeat timeout ensures Temporal knows the activity is not hanging indefinitely. Since the SDN controller is HA (3-node cluster), leader re-election takes 10 seconds — most retries will succeed once the new leader is elected.

If the outage lasts the full 20 minutes and exceeds the retry budget for the VLAN activity, those 128 workflows transition to failed. Their compensation activities run: clear PXE config, power off via IPMI. Those 128 machines return to the pool as available.

Now the interesting question: does the reservation as a whole fail, or does it partially succeed?

The reservation platform's SLA for 256-machine bulk reservations is: if fewer than N% of machines fail during provisioning, we offer substitution from the over-selected pool. We over-selected by 20% (so we had 307 candidate machines). The system attempts to substitute the 128 failed machines from the remaining candidates. If enough substitutes are available, the reservation completes with the original 256 count, just with different physical machines for the 128 that failed.

If substitution fails (not enough alternatives), the system reports partial provisioning: 128 of 256 machines are active, 128 provisioning failed. The user is notified and can decide: accept the 128 active machines, cancel the entire reservation, or extend the reservation window to allow re-provisioning after the SDN issue is resolved. The 128 active machines have a grace period before they are released — we do not immediately de-provision them just because the bulk reservation did not fully complete."

**Q: Your MySQL primary for the IaaS control plane in us-east-1 dies. Describe the exact sequence of events over the next 5 minutes, including what users experience and what the automated systems do.**

"Second 0: MySQL primary stops responding. No new writes can commit. In-flight transactions that have not yet committed fail with connection errors.

Seconds 0–5: MySQL Orchestrator (or equivalent HA tool) detects the primary failure via repeated health check failures. It determines which replica is the most up-to-date (has received all semi-synchronized writes — RPO = 0, so the promoted replica has all committed data).

Second 5–30: Orchestrator promotes the best replica to primary. This involves: (1) ensuring the old primary cannot accept writes if it comes back (STONITH via IPMI or network fence), (2) reconfiguring other replicas to follow the new primary, (3) updating the internal DNS record (or VIP) that services use to connect to the primary.

Seconds 5–30 (user experience): All write requests fail with connection errors. The compute service, network service, and reservation service all have connection pools that detect the primary failure within seconds. Circuit breakers on DB connections open. APIs return 503 Service Unavailable for write operations. Read operations continue to succeed against replicas (the replica pool is unaffected).

Second 30: New primary is promoted and DNS updated. Services reconnect (HikariCP connection pools attempt reconnection with exponential backoff).

Seconds 30–60: Writes resume. Users experience approximately 30 seconds of write unavailability. This is within the 99.99% SLA (52 minutes per year).

Running VMs: completely unaffected throughout. The hypervisors never communicated with the MySQL primary during normal operation — only the compute service does.

Metering pipeline: Kafka durably buffers any events published during the 30-second outage. The metering consumer processes them on recovery. No billing data is lost.

Minute 1–5 (post-recovery): The on-call engineer is paged by the MySQL Orchestrator alert. They verify the new primary is healthy, check that the old primary is properly fenced, and decide whether to provision a new replica to restore the replication topology. They check the audit dashboard for any failed writes during the 30-second window and verify that client retry logic handled them correctly (idempotency keys ensure no duplicates)."

**Q: A firmware upgrade plan for H100 GPU firmware is in progress — 200 machines planned, 40 completed successfully. You suddenly see GPU error rates spike on the 40 upgraded machines. What do you do?**

"This is the firmware upgrade blast-radius scenario playing out in production. Let me reason through the immediate actions and the investigation.

Immediate action: Pause the upgrade plan immediately. The Firmware Manager API has a `POST /api/v1/firmware/upgrade-plans/{plan_id}/pause` endpoint. While I could wait for the auto-abort (which triggers at 5% failure rate), the fact that I am seeing GPU error rate spikes on the *upgraded* machines — not the machines I expected to be degraded — tells me something is wrong with the firmware itself, not with individual machines.

At this point, 160 machines are on the old firmware and running fine. 40 machines are on the new firmware and showing GPU errors. I have not lost them yet — GPU ECC errors escalate gradually, so I have time to act.

Investigation: Pull the `firmware_releases` record for this upgrade. Check the `compatible_bios_versions` field — was the BIOS version on the 40 upgraded machines in the compatibility matrix? Check vendor release notes for known issues. Pull health check details for all 40 machines to characterize the error pattern: are all 40 affected, or a subset? Is it one GPU per machine or all GPUs?

Decision tree: If the firmware has a known bug disclosed by the vendor, the correct action is to flash the `rollback_version` (stored in the firmware_releases record) on all 40 affected machines. The Firmware Manager handles this: create a new upgrade plan targeting the 40 machines, with the rollback version as target. Batch size 5 (small, because we want to verify each rollback works before proceeding).

If the GPU error rate is severe enough that machines are approaching the ejection threshold (health score < 30), the Pool Manager will auto-eject them and offer replacement capacity to affected tenants. This is the automated safety net.

Communication: Alert the tenants whose reservations are on the 40 affected machines. Explain the situation honestly: firmware upgrade caused GPU errors, we are rolling back, estimated impact duration.

Post-incident: File a bug with the firmware vendor. Update the `compatible_bios_versions` field in the firmware_releases table to mark this firmware version as incompatible with any BIOS version that was present on the affected machines. Implement a pre-upgrade compatibility check in the Firmware Manager that blocks upgrades if the machine's BIOS version is not in the compatibility matrix."

---

## STEP 7 — MNEMONICS

### The PRICE Framework for Opening Any Infrastructure Interview

When asked "design a bare metal reservation platform" or any variant of these four problems, open with this mental checklist:

**P** — **Protection against double-allocation** (this is always the hardest correctness requirement)
**R** — **Resource lifecycle state machine** (every resource has one; draw it early)
**I** — **Idempotency on all writes** (network retries are real; idempotency keys prevent duplicates)
**C** — **Cache for hot reads, consistent writes** (Redis for reads, MySQL `SELECT FOR UPDATE` for writes)
**E** — **Event bus for loose coupling** (Kafka decouples core service from billing, notifications, cache invalidation)

If you remember PRICE, you will naturally draw the five shared components and avoid the most common mistakes.

### The Two-Level Safety Trick for Conflict Detection

For any concurrency question about resource allocation, the answer structure is always:
"Fast path (in-memory pre-filter, no DB) + Slow path (DB lock as authority)."

The fast path can be wrong (stale cache, interval tree lag) — it is OK because the slow path catches it.
The slow path cannot be wrong — it uses `SELECT FOR UPDATE` and ACID transactions.

Name the fast path, name the slow path, explain why both are necessary. This is the complete answer to "how do you prevent double-booking" for any resource type.

### Opening One-Liner for the Interview

Before drawing anything, say this:

"These systems all share the same fundamental challenge: turning raw physical hardware into safely-shareable, programmable resources where money or correctness is on the line. The design pattern that repeats across all of them is strong consistency for resource allocation (because double-booking a $200K GPU server has direct financial impact) combined with eventual consistency for dashboards and metrics (because being 5 seconds stale on a utilization chart has no consequence). Once you see that pattern, most of the architectural decisions follow from it."

This one statement demonstrates senior-level thinking in the first 30 seconds.

---

## STEP 8 — CRITIQUE

### What Is Well-Covered in the Source Material

The source files are unusually thorough on five areas:

1. **Conflict detection** — The interval tree implementation, the hybrid approach with MySQL pessimistic locking, the exact Java code for the critical section, and the failure mode analysis are all production-quality.

2. **Provisioning orchestration** — The Temporal.io workflow definition with compensation logic, idempotency design for each activity, and the failure mode table are comprehensive.

3. **Data models** — The SQL schemas are detailed and realistic, with proper indexing strategy explained and justified. This is rare in study materials and very useful for the "design the schema" follow-up questions.

4. **Failure scenarios** — The failure tables in the IaaS and reservation platform docs cover blast radius, RTO, and RPO explicitly. This is the senior-level framing most candidates miss.

5. **Capacity math** — The estimation tables are worked out correctly with formula + anchor numbers + architecture implications.

### What Is Missing, Shallow, or Potentially Wrong

**Missing: Cross-region active-active design for global deployments.** The material describes 5 regions but treats them as independent failure domains with no cross-region replication for resource state. An interviewer at a company with truly global infrastructure may probe: "How would you make the reservation API active-active across us-east-1 and eu-west-1?" The correct answer involves the difficulty of distributed transactions across regions and why most production systems accept that resource state is region-local (you provision in one region, not across two simultaneously). This is not covered.

**Missing: Chaos engineering and failure injection.** The material describes failure scenarios but does not describe how you would test that the mitigations work. A Staff+ interviewer will ask: "How do you verify that semi-synchronous replication actually provides RPO=0 before you need it in production?" The answer involves controlled failover drills, chaos engineering (kill the MySQL primary deliberately during business hours in staging), and canary analysis of failover behavior.

**Shallow: Network partition scenarios.** The material mentions Redis Redlock correctness issues briefly but does not deeply explore what happens during a network partition between the API gateway and the MySQL primary, or between the provisioning workers and the Temporal server. These are realistic scenarios in multi-AZ deployments.

**Shallow: Hot-shard / hot-partition problems.** The material assumes uniform distribution of load. In practice, one machine type (H100 GPUs) will be dramatically more popular than others, creating a hot partition on the `machine_type` index in MySQL and a hot key in Redis. The material does not discuss techniques for distributing this load (e.g., sharding the Redis sorted set for gpu_h100_8x across multiple keys with a consistent hash, or using MySQL table partitioning by machine_type).

**Potentially imprecise: Temporal vs. custom workflow engine for provisioning.** The Reservation Platform uses Temporal for provisioning while the Developer Portal uses a custom engine for workflows. The material argues this distinction correctly (Temporal for 15-minute multi-step IPMI sagas, custom engine for 3–10 step approval workflows). However, an interviewer may challenge: "Why not use Temporal for both?" The counter-argument (operational overhead vs. complexity justification) is in the material but could be sharper.

### Senior Probes That Will Catch You Unprepared

- "Your interval tree is per-service-replica and kept in sync via Kafka. What is the theoretical maximum lag between a reservation committing in MySQL and the interval tree update in all replicas? What happens to a request that arrives during this lag window on a specific replica?"

- "You said you over-select candidates by 2x for the pessimistic lock path. What is the maximum number of concurrent reservation requests for the same machine type that this handles before you start seeing lock timeout cascades?"

- "How does the Machine Pool Manager handle a machine that has a health score of 85 (above watch threshold) but whose score has been declining by 2 points per check for the last 10 checks? Does the system catch this trend before the score hits 30?"

- "The OpenStack API facade returns `202 Accepted` for VM creation. The client polls until status = ACTIVE. What is the maximum number of cells that could have a stuck VM (status = BUILDING indefinitely), and how does the system detect and recover from this?"

- "You described the Developer Portal's cost dashboard as having less than 5 seconds lag from metering events. What specifically causes this lag, and what would happen to the lag during a Kafka consumer rebalance?"

### Traps to Avoid

**Trap 1: Reaching for Cassandra because "we have a lot of data."** Every time you are tempted to say Cassandra, ask yourself: "Does this data require a join, a transaction, or a range query for correctness?" Resource state, reservations, and quotas require all three. Only audit logs, metering events, and health time-series are appropriate for wide-column stores.

**Trap 2: Forgetting that bare-metal provisioning is 15 minutes, not 15 seconds.** Many candidates design a synchronous provisioning path and then run into "but what if it takes too long?" too late. Establish upfront that bare-metal provisioning is inherently async, design for async first, and treat the 201 "reservation confirmed" response as a separate event from "machines are ready to use."

**Trap 3: Designing the control plane to be required for data plane operation.** If your VM creation flow requires the scheduler to be running for existing VMs to continue operating, you have coupled the control plane to the data plane. State explicitly early: "The hypervisors run independently of my control plane. My control plane is responsible for directing them, not for keeping them alive."

**Trap 4: Ignoring the financial dimension of hardware failures.** A $200K GPU server going offline is not the same as a $500 commodity server. Design decisions — from health check frequency to warm pool sizing to RMA prioritization — should all be influenced by the unit cost of the hardware. An interviewer from a hardware-intensive company will notice if you treat all hardware as homogeneous.

**Trap 5: Underdesigning the audit trail.** Every system in this pattern requires a comprehensive audit log: who did what, when, what the before/after state was, and why. This is not a nice-to-have — it is required for compliance, debugging production incidents, and disputing erroneous billing. If you omit the audit log, add it explicitly when asked "what did you leave out?"

---

*End of Infra-01 Interview Guide — Bare Metal & IaaS*
