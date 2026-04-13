# Infra-07 Interview Guide: Distributed Systems Primitives

Reading Infra Pattern 7: Distributed Systems Primitives — 7 problems, 6 shared components

---

## STEP 1 — PATTERN OVERVIEW

This pattern covers the seven foundational coordination primitives that every distributed infrastructure platform is built on. They are not application features — they are the infrastructure that other infrastructure depends on. If any of these breaks, the entire platform degrades. That is what makes them interesting to ask about in interviews: the reliability bar is higher than anything built on top of them.

The seven problems are:

1. **Consistent Hashing System** — deterministically map keys to nodes with minimal disruption on topology changes
2. **Distributed Configuration Management** — store, version, and push config/secrets to thousands of services in real time
3. **Distributed Lock Service** — coordinate exclusive access to shared resources with strict correctness guarantees
4. **Distributed Rate Limiter** — enforce per-tenant, per-API request budgets across thousands of service instances
5. **Distributed Transaction Coordinator** — atomically execute multi-service workflows with rollback capability
6. **Leader Election Service** — guarantee exactly one active leader from a pool of candidates at all times
7. **Service Discovery System** — let services find each other's current healthy endpoints in real time

**Why this pattern matters at FAANG/NVIDIA:** Every large-scale distributed system uses several of these simultaneously. Redis Cluster uses consistent hashing. Kubernetes uses leader election and service discovery. Every API gateway uses rate limiting. Every provisioning workflow uses distributed transactions. Being able to design any of these correctly — including articulating the failure modes and trade-offs — is what separates a senior from a staff-level engineer.

---

## STEP 2 — MENTAL MODEL

### The Core Idea

The common thread across all seven problems is this: **you have multiple machines that need to agree on some shared truth, and any one of them can fail at any time.** The techniques are different (hash rings, Raft consensus, token buckets, saga logs), but the underlying challenge is the same — achieving correctness in the presence of failures, network partitions, and race conditions.

### Real-World Analogy

Think of a well-run hospital emergency room. There is a triage coordinator (leader election) who decides which patients see which doctors (consistent hashing for work distribution). The hospital has a master register of all patients currently admitted (service discovery). There are medication dispensing rules that only allow certain quantities per shift (rate limiting). When a surgery involves cardiology, anesthesia, and the OR team, a charge nurse coordinates the sequence and can roll back if the patient's vitals change mid-prep (distributed transaction). The hospital's drug formulary is shared by all departments and must be updated without anyone getting an outdated copy (distributed config management). And only one surgeon can be in a specific OR at once (distributed lock).

Each piece looks simple in isolation. The hard part is that any participant can crash, be unreachable, or be in the middle of an operation when failure hits. The hospital still needs to function.

### Why It Is Hard

Three things make every problem in this pattern genuinely hard, and all of them are related:

**First, failures happen in the middle of operations.** A client holds a lock and then freezes due to a GC pause. A saga step commits to one database but the network drops before the coordinator records the success. A leader renews its lease but is actually in a partition. The system must be correct even when operations are partially completed.

**Second, you cannot trust clocks.** Many naive solutions rely on wall-clock time (Redis Redlock, for example). But clocks drift, NTP corrections are discontinuous, and a process can pause for seconds due to GC or OS scheduling. Any correctness argument that relies on "I know it hasn't been more than X milliseconds" is fragile in production.

**Third, there is a fundamental tension between safety and liveness.** A system that refuses to operate during any uncertainty is safe but useless. A system that always proceeds is available but can corrupt data. Every design in this pattern is a calibrated position on that spectrum, and the interviewer wants to know that you understand where you are and why.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

These are the questions you should ask at the start of any of these problems before drawing a single box.

**Q1: What is the consistency requirement — is correctness critical or is "mostly right" acceptable?**

This is the single most important question. A distributed lock for bare-metal IPMI commands must be linearizable — two conflicting firmware updates could physically destroy a server. A rate limiter for an advertising API can tolerate 1–2% over-admission. The answer drives your entire technology stack choice (Raft consensus vs. Redis, strong consistency vs. AP).

**Q2: What are the scale parameters — how many unique keys, clients, and operations per second?**

The answer determines whether you need sharding (a single Raft cluster maxes out around 50K ops/sec), a sidecar architecture, or a purely local in-memory solution. For rate limiting: 1M checks/sec is fundamentally different from 10K/sec. For service discovery: 100K instances is fundamentally different from 1K.

**Q3: What is the failure mode preference — fail-open or fail-closed?**

If the coordination service itself goes down, should the protected operation proceed or be blocked? For rate limiting, fail-open is almost always right (don't block traffic just because the counter store is unavailable). For distributed locks on destructive operations, fail-closed is right. Get this answer before designing your availability story.

**Q4: Is this single-region or multi-region?**

Multi-region changes almost everything. Raft consensus across regions has 50–100ms round-trip latency on the critical path. Leader election across regions is fundamentally different — you typically use separate election groups per region with regional ownership semantics rather than a global leader.

**Q5: Are the clients homogeneous or are there legacy integrations that need specific interfaces?**

Service discovery that only needs to serve modern gRPC services is simpler than one that must also serve as a DNS server for legacy Java services. Config management that only serves microservices is simpler than one that must integrate with Kubernetes ConfigMaps, Spring Cloud Config, and a custom Python SDK simultaneously.

**What changes based on the answers:**
- Consistency requirement → technology choice (Raft vs. Redis vs. Zookeeper vs. advisory DB lock)
- Scale → single-node vs. sharded vs. tiered (sidecar + centralized) architecture
- Fail mode → whether you need circuit breakers around the coordination service itself
- Multi-region → whether you need federation, regional isolation, or cross-region replication

**Red flags to watch for:**
- Candidate jumps straight to "we'll use Redis" without establishing the consistency requirement
- No mention of what happens when the coordination service itself fails
- Ignoring the clock-skew problem in solutions that rely on TTL timing for correctness
- Treating "eventually consistent" as a hand-wave instead of specifying the concrete convergence bound

---

### 3b. Functional Requirements

**Core requirements (common across all 7 problems):**
- The primary operation must complete with correct semantics (map a key, acquire a lock, elect a leader, etc.)
- The system must be available during partial failures (at least some nodes must be able to serve)
- Clients must be notified of state changes in near-real-time (watch/subscribe API)
- Operations must be idempotent or replayable (so retries do not cause double-effects)

**Scope clarifications by problem:**

For **Consistent Hashing**: the system maps arbitrary string keys to one of N physical nodes. It must support adding and removing nodes with minimal key redistribution (only 1/N of keys affected). It must support weighted nodes (heterogeneous hardware) and replication placement (return N distinct nodes for a key).

For **Distributed Config Management**: the system stores hierarchical key-value config and secrets. Clients must be notified of changes within 100ms. Every change must be versioned and rollback must be available. Secrets must be encrypted at rest with rotation support that requires no service restarts.

For **Distributed Lock Service**: the system grants exactly one exclusive holder at a time per named lock. If the holder crashes, the lock must be released automatically via TTL. Fencing tokens must be issued to allow protected resources to reject stale operations.

For **Distributed Rate Limiter**: the system enforces per-key (tenant + API) request budgets. Multiple algorithms must be supported. The overhead per check must be under 1ms at p50. When the rate limiter itself is unavailable, the system should fail open by default.

For **Distributed Transaction Coordinator**: the system executes multi-step workflows across independent services, each with its own database. If any step fails beyond its retry budget, all previously completed steps must be compensated (rolled back) in reverse order.

For **Leader Election Service**: from N candidates, exactly one holds leadership at any time. If the leader fails, a new one is elected within a bounded time (under 10 seconds). Stale leaders must be fenced. Non-candidates must be able to discover the current leader quickly.

For **Service Discovery System**: services register themselves with health check configuration. Clients can query the catalog by name and receive only healthy endpoints. The system must push updates to watchers within 5 seconds of a health status change.

**Clear scope statement:** You are building the coordination infrastructure layer, not the application logic that uses it. You are not implementing Raft itself, not building the application business logic, and not designing cross-region federation (unless explicitly asked).

---

### 3c. Non-Functional Requirements

**Derive these, do not just list them:**

**Availability — always 99.99%.** These are infrastructure primitives. If a rate limiter is down, every service call in the platform is affected. If service discovery is down, no service can call any other service. The bar is higher than the services that depend on them. Derive this: "If we have 1,000 services, and each depends on this primitive, then 99.9% availability means the primitive causes 8+ hours of downtime per year for every dependent service. That's unacceptable. We need 99.99%."

**Latency — sub-millisecond cached, under 50ms Raft consensus path.** Rate limit checks are on every single API call. Service discovery lookups happen before every inter-service call. These add up. A 10ms p99 latency on a rate limit check on a 100ms API call is a 10% overhead. Derive: "At 1M checks/sec, if each check adds 5ms, we're adding 5M seconds of cumulative latency per second. Must keep it under 1ms on the hot path."

**Consistency — linearizable for coordination, AP for data plane reads.** For anything involving mutual exclusion (locks, leader election), you must be linearizable. For read-heavy operations (service discovery lookups, config reads from cache), eventual consistency with a short convergence window is fine. The key distinction: "Does correctness require that no two clients have different views of this state simultaneously? If yes, linearizable. If a few seconds of staleness is fine, AP."

**Trade-offs to be explicit about:**
- ✅ Raft consensus gives linearizability, durability, automatic leader failover
- ❌ Raft consensus adds ~5–20ms per write operation, has a leader bottleneck, and fails if fewer than (N/2)+1 nodes are healthy
- ✅ Local caching (sidecar, in-memory) gives sub-millisecond latency
- ❌ Local caching means brief windows where different nodes see different states
- ✅ Fail-open on coordination service failure keeps traffic flowing
- ❌ Fail-open during a coordination outage means rate limits or lock guarantees are temporarily violated

---

### 3d. Capacity Estimation

**The formula that applies to most of these problems:**

```
Coordination ops per second = (active clients) × (ops per client per second) × (peak multiplier)
Storage per record × active records = total working set
Keep-alive traffic = (active sessions or leases) / (keep-alive interval in seconds) × (record size)
```

**Anchor numbers for each problem:**

**Consistent Hashing:** 1,000 physical nodes × 150 vnodes = 150,000 ring positions. Each position is 16 bytes (8B hash + 8B node ID). Total ring = 2.4MB — fits in CPU cache. Lookup throughput: binary search on 150K entries = ~17 comparisons = easily 10M lookups/sec on a single CPU. On node add: 150 new positions × 16B = 2.4KB delta gossip per node in the cluster.

**Distributed Config:** 500 services × 100 config keys = 50,000 keys. Read: 100,000/sec (every service call reads config). Write: 100/day. Watch: 10,000 concurrent. Key insight: reads are 100,000× more frequent than writes — cache everything aggressively. Version history at 100 versions/key × 1KB avg = 5GB total.

**Distributed Lock:** 500 services × 20 locks = 10,000 named locks. 50,000 lock ops/sec. Each lock record: ~400B. Total lock state: 4MB — trivially fits in memory. The storage is tiny; the challenge is Raft throughput and session management.

**Distributed Rate Limiter:** 200 APIs × 5,000 tenants = 1,000,000 unique rate-limit keys. 500,000 checks/sec. Redis ops: 3 per check = 1.5M Redis ops/sec. Token bucket state: 24B per key = 24MB for 1M keys. This is why sidecar + batch sync matters: without it you need 1.5M Redis IOPS.

**Distributed Transaction Coordinator:** 1,000 transactions/sec × 4 steps = 4,000 step operations/sec. In-flight at 5s avg duration: 5,000 sagas × 3KB = 15MB working set. Transaction log: 4,000 entries/sec × 512B = 259GB/day at 30-day retention = 7.8TB. This is the largest storage requirement in the group.

**Leader Election:** 500 election groups × 3 candidates = 1,500 leases. Lease renewals: 500/3s = 167/sec. Tiny. Storage: under 1MB. The challenge here is not scale — it is correctness (split-brain prevention).

**Service Discovery:** 20,000 service instances. DNS lookups: 200,000/sec. Health checks: 20,000/sec. The hot path must be in-memory (100MB working set). Health check assignment via consistent hashing: each of 3 nodes checks ~6,700 instances, avoiding duplicate checks.

**Architecture implications from estimation:**
- Rate limiter: sidecar L1 + Redis L2 is mandatory at 1M checks/sec
- Config management: local cache with watch-based invalidation is mandatory at 100K reads/sec
- Service discovery: in-memory catalog with Raft replication only for writes
- Transaction coordinator: MySQL for the transaction log (259GB/day requires durability + queryability)
- Lock service: in-memory FSM with Raft replication; 4MB state is trivially small

**Time to estimate in an interview: 4–6 minutes.** State the formula, plug in the anchor numbers, derive the implication. Do not get lost in precision — the goal is to show that you understand the order-of-magnitude constraints and what they force you to do architecturally.

---

### 3e. High-Level Design

**Components to put on the whiteboard (applicable to most problems in this pattern):**

1. **Client SDK / Sidecar** — the in-process library that clients use; manages sessions, keep-alives, local caches, and reconnection logic
2. **API Layer** — gRPC endpoints (and sometimes DNS/REST) that accept client requests
3. **Core Logic Engine** — the problem-specific logic (lock manager, election engine, rate limit algorithm, saga orchestrator, hash ring)
4. **Consensus Layer (Raft)** — replicates state across 3–5 nodes for durability and linearizability; present in 5 of 7 systems
5. **External Storage** — etcd for coordination state, Redis for counters, MySQL for audit/transaction logs, Kafka for async saga steps
6. **Watch/Notification Engine** — pushes state changes to subscribing clients

**Data flow to describe (use this template):**

"On the write path: client sends operation to the leader via gRPC. Leader proposes to Raft. Majority (2 of 3) replicate and acknowledge. Leader commits, applies to state machine, returns response to client. On the read path: client checks local cache first. On miss, reads from the service (which may read from leader for linearizable, or from follower for stale-OK). On state change: watch engine fans out the event to all current subscribers."

**Key decisions to justify:**
- Why Raft and not Redis? Because Raft gives linearizable writes without clock dependency. Redis Redlock is not safe under GC pauses or clock skew.
- Why 3 nodes and not 5? 3 nodes tolerate 1 failure, which covers single-AZ outages. 5 nodes tolerate 2 failures but double the write latency and cost. For most infrastructure use cases, 3 is the right starting point.
- Why a sidecar for rate limiting? Because 1M checks/sec cannot afford a network round-trip on every single check. The sidecar does local token bucket checks in microseconds and batch-syncs to Redis every 100ms.
- Why separate the client SDK from the server? Because the SDK hides complexity (retry logic, reconnection, keep-alive, local caching) from application engineers. A well-designed SDK means the coordination service is a black box to the application.

**Whiteboard order:**
1. Draw the clients on the left
2. Draw the client SDK / sidecar next to clients
3. Draw the 3-node cluster in the middle (always odd number, label one Leader)
4. Add the external storage (etcd, Redis, MySQL) below the cluster
5. Add Watch/Notification Engine and show the fan-out arrows
6. Now walk the happy-path data flow end to end

---

### 3f. Deep Dive Areas

**Deep Dive 1: Fencing Tokens — the most commonly probed topic**

**The problem:** Client A holds a distributed lock, then pauses for 10 seconds (JVM GC, OS preemption, network stall). During that pause, the lock TTL expires. Client B acquires the lock and starts operating on the shared resource. Client A's GC finishes, it resumes assuming it still holds the lock, and it overwrites Client B's work. The TTL-based expiry protected Client B's acquisition but could not stop Client A from proceeding after waking up.

**The solution:** Every time a lock is granted, issue a **fencing token** — a monotonically increasing integer. Client A gets token 42. After TTL expiry, Client B gets token 43. Client A wakes up and sends its write request to the storage layer with token 42. The storage layer's rule is: **reject any request where the token is less than the last seen token.** Since 42 < 43, Client A's write is rejected. Client B's subsequent writes with token 43 proceed normally.

**The critical point interviewers probe:** This requires the storage layer to participate in the fencing protocol. The lock service alone cannot prevent the damage — it can only issue the tokens. The protected resource (database, file system, API) must enforce the "reject stale token" rule. This is why the fencing token pattern is sometimes called "cooperative fencing" — both sides have to implement it.

**Trade-off to state unprompted:**
- ✅ Fencing tokens make correctness independent of timing assumptions
- ❌ The storage layer must be modified to check tokens — you cannot use an unmodified database
- ✅ Monotonically increasing tokens are trivially cheap (a single counter per lock key in the Raft state machine)
- ❌ If the storage layer is external and unmodifiable (e.g., a third-party API), you need a different approach (idempotent operations, application-level CAS, etc.)

---

**Deep Dive 2: The Saga Pattern and Compensation Failures**

**The problem:** You need to provision a bare-metal server. This involves 4 steps across 4 independent services: reserve in inventory, configure the network VLAN, charge the tenant's billing account, and power on via IPMI. Each service has its own database. If step 3 (billing) fails, you must undo step 2 (VLAN config) and step 1 (inventory reservation). But what if the compensation for step 2 also fails?

**The solution:** The Saga pattern. An orchestrator drives each step via Kafka commands. Each service implements both a forward action and a compensating action. The orchestrator persists every step's status to a transaction log (MySQL) before proceeding. If the forward action fails, the orchestrator drives compensation in reverse order. Compensation must be idempotent — if the compensating action for step 2 fails, the orchestrator retries it (potentially multiple times) because partial compensation is worse than full compensation.

**For the "what if compensation fails" probe:** The standard answer is: compensating transactions must be designed to always succeed eventually. Use idempotent compensation, retry with exponential backoff, and have a dead-letter queue (DLQ) for sagas where compensation has failed the maximum retry count. A human operator handles DLQ entries — this is acceptable because it is rare and the alternative (ignoring the failure) is worse.

**Trade-offs to state unprompted:**
- ✅ Orchestration saga is recoverable — the transaction log lets you resume from the last completed step after a coordinator crash
- ❌ Compensation is not a true rollback — it is a semantic undo, and the world has already seen intermediate states (e.g., the inventory was reserved and then unreserved; a race condition might have caused another reservation in between)
- ✅ Kafka provides durable async messaging, so steps can be retried without coordinator state
- ❌ If a participant never responds (hangs), the saga must have a timeout and treat it as a failure, which means compensation runs even though the step may have partially succeeded on the participant side — this is why idempotency keys on every step are non-negotiable

---

**Deep Dive 3: Distributed Rate Limiting — Sliding Window vs. Fixed Window**

**The problem:** Fixed-window rate limiting (e.g., 1000 requests per minute) has a well-known boundary problem. If a client sends 1000 requests at 12:00:59 and another 1000 at 12:01:01, they have sent 2000 requests in a 2-second window, but each falls within a separate 1-minute window and neither triggers the limit. The effective burst is 2× the configured limit at every window boundary.

**The solution:** Sliding window counter. Instead of a single counter that resets at minute boundaries, maintain two counters: the current window and the previous window. When checking the rate, compute a weighted sum:

```
weighted = current_window_count + (previous_window_count × (1 - elapsed_fraction_of_current_window))
```

If elapsed_fraction is 0.3 (we are 30% through the current window), then previous contributes 70% of its count. This gives a smooth, continuously sliding estimate that eliminates the boundary burst problem. The approximation error is less than 0.003% on average.

**Why not sliding window log?** A sliding window log stores every request timestamp in a sorted set. It is perfectly accurate but uses up to 8KB per active key (1000 entries × 8 bytes). At 1M keys that is 8GB of Redis memory. The sliding window counter uses only 2 integers per key (48 bytes for 1M keys = 48MB). The approximation trade-off is worth it.

**Trade-offs to state unprompted:**
- ✅ Token bucket allows configurable burst (bucket capacity) — good for APIs that are bursty by nature
- ❌ Token bucket does not prevent a client from emptying the bucket instantly, causing a large burst upfront
- ✅ Leaky bucket enforces perfectly smooth output rate — good for protecting downstream systems from load spikes
- ❌ Leaky bucket queues requests, adding latency, and can grow unbounded — needs a max queue depth that causes drops

---

### 3g. Failure Scenarios

These are the failure modes you should proactively raise in an interview. Raising them before the interviewer asks signals senior-level thinking.

**Failure 1: Network partition isolates the leader**

In Raft, if the leader is partitioned from the majority of nodes, it cannot commit new entries (it needs majority acknowledgment). The remaining nodes will elect a new leader after the election timeout. The old leader, if still running, will continue to think it is the leader until its lease expires — this is the "stale leader" window. Any operations the old leader processes during this window are not committed to the cluster's log and will be lost or overridden.

**Senior framing:** "We handle this with two-level protection. First, the Raft leader lease prevents the old leader from committing anything (it does not have quorum). Second, fencing tokens ensure that even if the stale leader issues a command to a protected resource, the resource will reject it because the new leader's higher fencing token has already been seen."

**Failure 2: Cascade failure when the coordination service is slow**

If the rate limiter or lock service has a 500ms p99 latency spike (say, due to a slow GC on the Raft leader), and every API call blocks waiting for a rate limit check, the entire platform's latency degrades by 500ms. This is worse than the coordination service being completely down (where clients would fail open immediately).

**Senior framing:** "The sidecar architecture solves this. The sidecar has a local timeout: if the Redis call for rate limiting takes more than 5ms, it uses the local token bucket estimate and proceeds. The coordination service being slow should not cause cascading timeouts across the entire request path. The Client SDK must have similar timeout-and-fallback logic for lock acquisition and config reads."

**Failure 3: Service discovery self-preservation vs. mass deregistration**

If a network partition between the service discovery cluster and most service instances causes mass TTL expiry, all instances could be marked unhealthy simultaneously. This would cause all DNS responses to return empty, bringing down the entire platform — even though the services themselves are healthy and serving traffic.

**Senior framing:** "This is why Eureka-style self-preservation mode exists: if the renewal rate drops below 85% of expected for 15 consecutive minutes, the service discovery system assumes it is in a partition and stops expiring instances. Stale data is better than empty data. The trade-off is that if instances really are dead, the stale entries will cause connection failures — but clients will detect that via connection errors and retry. The alternative (empty catalog) causes complete platform failure."

**Failure 4: Saga coordinator crash mid-compensation**

The coordinator crashes after starting compensation but before recording all compensation steps as complete. When it restarts, it reads the transaction log and finds a saga in COMPENSATING state. It must determine which steps have already been compensated and which have not, then resume from where it stopped.

**Senior framing:** "This is exactly why we persist every step status to MySQL before dispatching the Kafka command. The sequence is always: (1) write COMPENSATING status to DB, (2) then publish Kafka command. On restart, if we see a step in COMPENSATING state with no corresponding COMPENSATED record, we republish the Kafka command. Since compensation is idempotent (compensating an already-compensated step is a no-op), this is safe. The idempotency key ensures the participant service handles the duplicate gracefully."

---

## STEP 4 — COMMON COMPONENTS

These six components appear across multiple problems. Know them cold.

---

### Component 1: etcd + Raft Consensus

**Why it is used:** etcd provides linearizable key-value storage with a Watch API and lease-based TTL. Five of the seven systems use it. The core reason is that Raft consensus eliminates the class of bugs that come from clock-dependent solutions. A key created with an etcd lease will be atomically deleted when the lease expires — all watchers are notified in a single Raft-committed operation. There is no race between "lease expired" and "key deleted."

**Key configuration:**
- 3-node cluster across 3 availability zones (tolerates 1 AZ failure)
- Lease TTL: 15 seconds (default). Keep-alive interval: TTL/3 = 5 seconds (tolerates one missed heartbeat before expiry)
- Write latency: under 20ms p99 on SSDs (one Raft round-trip = 2 × network RTT + disk fsync)
- Watch API: client subscribes from `last_seen_revision + 1` — guaranteed no missed events even if disconnected

**What the system looks like without it:** Without Raft-backed consensus, you rely on Redis Redlock or similar timing-based approaches. Martin Kleppmann demonstrated in 2016 that Redlock fails under GC pauses, OS scheduling delays, and clock jumps. For infrastructure control-plane operations (exclusive lock for IPMI, single leader for a scheduler), this is unacceptable. You would also lose the atomic "key expires → all watchers notified" guarantee, requiring clients to poll rather than watch.

---

### Component 2: Lease-Based TTL with TTL/3 Keep-Alive

**Why it is used:** Leases solve the "what if the holder dies without releasing?" problem. A client holding a lock, a leader holding its position, or a service instance holding its registration — all must prove they are alive periodically. If they stop, their claim automatically expires. The TTL/3 keep-alive interval tolerates one missed heartbeat before expiry (if you send at 5s intervals with a 15s TTL, you can miss 2 consecutive heartbeats before the TTL fires — this handles transient network blips without unnecessary failover).

**Key configuration:**
- Session/lease TTL: 15 seconds (default in lock service, leader election)
- Keep-alive interval: `TTL / 3` — this is the universal formula; memorize it
- On death: session/lease expiry triggers atomic cleanup of all associated keys (all held locks released, leader key deleted, service deregistered)

**What the system looks like without it:** Without automatic TTL-based expiry, a crashed client holds its lock or leader position forever, requiring manual intervention. The platform has no ability to self-heal from client crashes.

---

### Component 3: Fencing Tokens (Monotonically Increasing, Enforced by Protected Resource)

**Why it is used:** Fencing tokens solve the "paused client" problem that TTL-based expiry cannot. Even after a lock or lease expires, a previously-holding client can wake up and attempt operations. The fencing token (a monotonically increasing integer issued on every grant) allows the protected resource to reject stale operations. Token N is valid only while the current grant has token N. After re-election or re-acquisition, token N+1 is issued, and all future requests with token N are rejected.

**Key configuration:**
- Global monotonic counter per lock key (in distributed lock) or per election group (in leader election)
- Counter is stored in the Raft state machine and CAS-incremented atomically on each grant
- Protected resource stores `last_seen_token` and rejects any request where `token < last_seen_token`
- The token is returned to the client in the acquire/campaign response and must be passed with every protected operation

**What the system looks like without it:** Without fencing tokens, a paused client that resumes after TTL expiry can still corrupt shared state. This has caused real production incidents. The TTL only prevents indefinite blocking; it does not prevent the race condition between "lock expired" and "client resumed."

---

### Component 4: Watch / Subscribe API with Revision-Based Guarantee

**Why it is used:** Push-based notification is far more efficient than polling at scale. If 10,000 services are watching their config, polling would require 10,000 × (poll frequency) requests per second. With watch, state changes generate one event that the server fans out to all subscribers. The revision-based guarantee (client subscribes from `last_seen_revision + 1`) ensures that no event is missed even if the client is disconnected for seconds — etcd caches all revisions.

**Key configuration:**
- gRPC server-streaming or bidirectional for the watch channel
- Client always stores the last seen revision and resumes from `last_seen + 1` on reconnect
- Watch registrations are per-key or per-key-prefix (subscribe to all config keys for a service: `/config/prod/nova-api/`)
- Notification latency target: under 100ms from write commit to client delivery

**What the system looks like without it:** Without a watch API, clients must poll. Polling adds latency (you can only detect changes as fast as you poll), wastes resources, and creates artificial load spikes. For config hot-reload, if services poll every 30 seconds, a critical config change takes up to 30 seconds to propagate — this is too slow for most incident response scenarios.

---

### Component 5: FIFO Wait Queue with Writer-Preference

**Why it is used:** Without an ordered wait queue, lock release triggers a thundering herd: all waiting clients simultaneously try to acquire, and the result is random (first to arrive at the server wins, not first to have asked). FIFO fairness prevents starvation. Writer-preference prevents a stream of read-lock holders from indefinitely blocking a waiting write-lock holder — once an exclusive waiter is at the head of the queue, no new shared waiters are admitted ahead of it.

**Key configuration:**
- Per-lock-key FIFO queue of `WaitEntry` objects (session_id, lock_type, deadline)
- Grant rule: EXCLUSIVE → only if holders is empty AND no exclusive waiters ahead; SHARED → only if no exclusive holder AND no exclusive waiters ahead
- When lock is released: process wait queue — grant all consecutive SHARED waiters until an EXCLUSIVE is found, or grant the leading EXCLUSIVE and stop
- Expired waiters (past their deadline) are purged before processing

**What the system looks like without it:** Without a wait queue, you get thundering herd on lock release (N clients racing → N-1 client round-trips wasted per lock), and you can starve writers if reads are frequent (a continuous stream of read requests always gets priority because there is no queue ordering).

---

### Component 6: Local Cache + Batch Sync to Backend

**Why it is used:** The coordination service cannot be on the synchronous critical path of every single operation if throughput is in the millions per second. The solution is to bring the hot-path computation local (sidecar or in-memory SDK cache) and sync asynchronously to the authoritative backend. This reduces backend load by 10–100× at the cost of brief windows of over-admission or stale data.

**Key configuration (Rate Limiter):**
- Sidecar maintains a local token bucket for each rate-limit key
- Every 100ms (or after N requests), sidecar pushes accumulated count to Redis and pulls back the global count
- If Redis is unavailable, sidecar uses the local count and fails open
- The brief window of over-admission is bounded: up to 100ms of traffic from all instances for a given key may exceed the limit before Redis reconciles

**Key configuration (Config/Service Discovery):**
- Client SDK caches config values in-memory; initial cache populated on startup
- Watch subscription maintains cache freshness — on change event, SDK updates local cache
- TTL-based cache expiry as safety net in case Watch connection is lost

**What the system looks like without it:** At 1M rate-limit checks/sec, without local caching you need 3M Redis operations/sec (3 ops per check). A Redis cluster can handle this but at significant infrastructure cost and with 0.5–2ms per check. The sidecar reduces this to 30K Redis ops/sec (10–100× reduction) and drops the hot-path check to microseconds.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Consistent Hashing System

**Unique things:** The only problem in this pattern that does not use Raft consensus as its coordination mechanism — instead it uses gossip protocol for ring propagation. It is fundamentally a data-structure design problem (sorted array + binary search) rather than a distributed coordination problem. The virtual node count (150 per physical node) is a derived statistic based on the formula `std_dev ≈ 1/sqrt(V×N)`, which yields ~2.6% deviation with 10 nodes and 150 vnodes — the sweet spot before diminishing returns. Zone-aware replication placement is a ring walk that collects distinct physical nodes preferring different zones, which is a different replication strategy from what any other problem in this pattern uses.

**Different decisions:** Uses gossip instead of Raft (ring metadata is read-heavy and can tolerate brief inconsistency during propagation). Uses a sorted array with binary search rather than a hash map (O(log N) lookup vs. O(1) but requires ordered structure for "next node clockwise" semantics). Uses weighted virtual nodes (weight/100 × default_vnodes) for heterogeneous hardware — no other problem in this pattern has a hardware-aware partitioning mechanism.

**Two-sentence differentiator:** Consistent hashing is unique because it is a partitioning algorithm, not a coordination service — the ring itself is the data structure, and the problem is distributing it with minimal disruption. The 150-vnode standard balances load distribution (2.6% std dev) against memory overhead (2.4MB for 1K nodes) and is the answer every senior engineer should know by number.

---

### Distributed Configuration Management

**Unique things:** The only problem that involves two fundamentally different storage backends — etcd for non-secret config (linearizable, watch-capable KV) and HashiCorp Vault for secrets (encrypted at rest with AES-256-GCM, dynamic secret generation, TTL-based lease management). The zero-downtime secret rotation protocol (dual-read window with 300-second grace period) is unique to this problem. Config templating with environment inheritance (`base → env → service overlays`) and schema validation before write commits are features no other problem in this pattern needs.

**Different decisions:** Needs both a consensus store (etcd) and a specialized secret store (Vault) rather than a single backend. The watch mechanism uses `last_seen_revision + 1` to guarantee no missed events, which is more important here than in other watch-using systems because a missed database credential rotation event could break a service indefinitely. The Git backup for config-as-code is unique — no other problem in this pattern writes to a git repository.

**Two-sentence differentiator:** Config management is the only problem that must store both regular config and secrets, requiring etcd for config (watch-friendly, linearizable) and Vault for secrets (encrypted, dynamic, with TTL leases). The secret rotation's 300-second dual-validity window is the critical design detail: old and new credentials are both valid simultaneously so services can rotate without restart.

---

### Distributed Lock Service

**Unique things:** The only problem that fully implements mutual exclusion semantics — specifically the distinction between EXCLUSIVE and SHARED (read/write) lock types and the writer-preference rule that prevents writer starvation. The fencing token is present in leader election too but is more deeply embedded here because lock service has much higher operation rates (50,000 ops/sec vs. 50 elections/day). The choice to use an embedded Raft state machine with bbolt (same as etcd itself) rather than depending on an external etcd is architecturally notable — it keeps the lock service dependency-free.

**Different decisions:** Uses FIFO wait queue per lock key (no other problem in this pattern has this concept). Uses a global monotonic fence counter rather than a per-election counter. Distinguishes between session TTL (life of the connection, 15s default) and lock operations (many per session) — no other problem has this two-level lifecycle.

**Two-sentence differentiator:** The distributed lock service is the only primitive in this pattern that must reason about both mutual exclusion types (shared vs. exclusive) and starvation prevention via FIFO writer-preference queuing. Its correctness argument against Redis Redlock — that any timing-based lock is unsafe under GC pauses — is the most important conceptual point to nail in any lock service interview.

---

### Distributed Rate Limiter

**Unique things:** The only problem that is explicitly an AP system — eventual consistency is acceptable, and fail-open is the default failure mode. It is the only problem with a multi-tier architecture (API gateway → sidecar → centralized Redis), where each tier handles a different granularity and the sidecar absorbs the vast majority of traffic. Four distinct algorithms (token bucket, sliding window counter, sliding window log, leaky bucket) exist and the choice between them is an interview question in itself. The Lua script atomic execution in Redis for the token bucket check-and-decrement is a unique implementation detail.

**Different decisions:** Redis instead of Raft as the primary backend — this is justified because rate limiting can tolerate brief inconsistency (1% over-admission is acceptable; incorrect mutual exclusion is not). Fail-open instead of fail-closed — the opposite of lock service. Per-key Redis sharding via consistent hashing to prevent hot shards on popular tenants.

**Two-sentence differentiator:** Rate limiting is the only primitive in this pattern where eventual consistency is correct by design — brief over-admission is tolerable and far better than blocking all traffic because the coordination service is slow. The sidecar architecture (local token bucket + 100ms batch sync) is what makes 1M checks/sec possible without 1M Redis calls per second.

---

### Distributed Transaction Coordinator

**Unique things:** The only problem that orchestrates multi-step workflows across independent services with independent databases — everything else in this pattern deals with a single operation on a single resource. The saga pattern, compensation logic, and idempotency key design are unique to this problem. The transaction log (MySQL with 7.8TB at 30-day retention) is by far the largest storage requirement in the pattern. Two-phase commit is also present but used rarely (the problem explains why sagas are preferred for long-running workflows).

**Different decisions:** Uses Kafka for async step dispatch rather than direct gRPC calls — this is essential because saga steps can take seconds, and blocking a gRPC call for seconds is wasteful and fragile. Uses MySQL for the transaction log rather than etcd, because 259GB/day of structured data needs ACID transactions and queryability (not just key-value storage). The `idempotency_key = "{saga_id}-{step_index}"` is a dual-purpose key: it both prevents duplicate saga creation and prevents duplicate step execution on Kafka retries.

**Two-sentence differentiator:** The distributed transaction coordinator is the only problem in this pattern that must handle partial failures across multiple independent databases, relying on semantic compensation (not transactional rollback) and a persistent transaction log for recovery. The key insight is that compensation must be idempotent and that the transaction log written before each Kafka dispatch is what enables coordinator crash recovery.

---

### Leader Election Service

**Unique things:** The only problem where the primary constraint is "exactly one, never zero, never two" — the consistency requirement is one-of-N selection rather than mutual exclusion of an arbitrary resource. The voluntary step-down (resign API) is unique to this problem — no other problem needs a graceful handoff mechanism for rolling upgrades. The Observer API (non-candidates discovering the leader for routing purposes) is unique. The CAS fencing token increment at election time (not on every operation like in lock service) creates a different usage pattern.

**Different decisions:** etcd `create_if_not_exists` semantic (atomic key creation with lease attachment) is the core mechanism — in lock service, you use a separate lock state machine; in leader election, the key's existence IS the lock. Watch on the leader key deletion (not watch on a separate lock state) is simpler and more direct. Priority field on candidates (higher priority candidate preferred) is unique to leader election — lock service does not have this concept.

**Two-sentence differentiator:** Leader election is uniquely about one-of-N selection with the constraint that exactly-one must hold at all times, making the etcd create-if-not-exists atomic semantic (not a general lock acquire mechanism) the correct tool. The fencing token CAS increment at election time, combined with the Observer API for non-participants, creates a clean separation between holding leadership and discovering leadership.

---

### Service Discovery System

**Unique things:** The only problem that must support multiple interfaces simultaneously — DNS (for legacy compatibility), gRPC (for rich queries), and REST (for management). The self-preservation mode (stop expiring instances if renewal rate drops below 85% for 15 minutes) is unique to this problem and is the most important operational safeguard. The consistent hash-based health check assignment (each node in the cluster checks a deterministic subset of instances, no coordination needed) is an elegant use of consistent hashing within a non-consistent-hashing problem. Threshold-based health state transitions (3 consecutive failures to go CRITICAL, 2 consecutive passes to return PASSING) prevent flapping.

**Different decisions:** In-memory catalog with Raft replication only for writes — this is different from the lock service's approach of making every operation a Raft proposal, because the read rate (200K DNS/sec, 50K gRPC/sec) would overwhelm Raft if reads went through it. The self-preservation mode is an availability optimization that is not present in any other problem (other problems fail-safe; this one fails-open at the service catalog level). The DNS interface is unique — no other problem in this pattern serves DNS records.

**Two-sentence differentiator:** Service discovery is unique in requiring a multi-protocol interface (DNS + gRPC + REST) to serve both legacy and modern clients, and its self-preservation mode is the key design insight: when the network is partitioned, stale-but-present endpoints are far better than an empty catalog that would take down the entire platform. The consistent hash health check assignment distributes health check load without any coordination overhead — each node independently knows which instances it is responsible for.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2–4 sentences)

**Q1: Why do you use 150 virtual nodes per physical node in consistent hashing?**

**150 vnodes is the industry standard** (Amazon DynamoDB, Apache Cassandra) because it balances load distribution quality against memory overhead. The load balance standard deviation formula is approximately `1/sqrt(V × N)` — with 10 nodes and 150 vnodes, deviation is ~2.6%, which is within the 10% target. Going to 500 vnodes drops it to ~1.5% but triples the ring size and memory usage. Going below 50 vnodes gives deviation above 5%, causing hot nodes that defeat the purpose of sharding.

**Q2: What is the difference between fail-open and fail-closed in a rate limiter, and when would you choose each?**

**Fail-open means allow traffic through when the rate limiter is unavailable; fail-closed means block it.** For most rate-limiting use cases you fail open: the business harm of blocking legitimate traffic is greater than the harm of brief over-admission. The exception is hard resource protection — if the rate limiter guards access to a component that will physically break under overload (like an IPMI controller), you fail closed. The answer to "which mode" is always: "what is the consequence of each failure mode, and which is more acceptable?"

**Q3: What is a saga and how does it differ from a two-phase commit?**

**A saga is a sequence of local transactions, each with a compensating transaction, that achieves eventual consistency across multiple services.** 2PC is synchronous and atomic: all participants lock their resources, prepare, then commit or abort together. Sagas are asynchronous and eventual: steps execute independently, and on failure, compensation runs in reverse. 2PC blocks all participants during the prepare phase (bad for long-running operations, causes resource locking under failure); sagas have no blocking but leave intermediate states visible (other services may see partial progress).

**Q4: Why can't you use Redis SETNX for leader election?**

**Redis SETNX is not safe for leader election because it depends on wall-clock TTL for automatic expiry.** If the leader's process is paused for longer than the TTL (JVM GC, OS scheduling), the TTL fires, another candidate acquires the key, and when the original leader resumes it still thinks it holds the key. Martin Kleppmann's 2016 analysis showed this clearly for Redlock. For safety-critical leader election, you need a system that ties the key's lifetime to a physical network session (etcd lease) and provides linearizable reads.

**Q5: How does a service discover the health of its dependencies in real time?**

**The client SDK subscribes to watch events for each dependency service name using a gRPC streaming watch.** When any instance of a dependency changes health status (or is deregistered), the watch event is delivered within ~5 seconds. The SDK maintains a local copy of the healthy endpoints for each dependency and uses this for load balancing without querying the service discovery server on every call. The cached data is the hot path; the watch mechanism keeps it fresh. DNS-based service discovery (via CoreDNS) is an alternative but is less real-time and does not carry metadata.

---

**Q6: What happens in a consistent hashing ring when a new node joins?**

**Only 1/N of the total keys are affected — those that hash to positions between the new node's virtual node positions and their clockwise predecessors.** For example, if node X joins with 150 virtual nodes, and the total ring has 1,000 nodes × 150 vnodes = 150,000 positions, then 150/150,000 = 0.1% of the ring is "claimed" by X per virtual node. Those key ranges were previously owned by X's clockwise successors and must be migrated. The migration coordinator handles the background data transfer while routing continues uninterrupted by falling back to the old owner on cache miss during migration.

**Q7: What does linearizable mean and why does it matter for distributed locks?**

**Linearizable means every operation appears to execute atomically at some point between its invocation and its response, and the effect is immediately visible to all subsequent operations.** For distributed locks, this means: if client A acquires the lock and client B then tries to acquire it, B is guaranteed to see A's lock — there is no window where both clients believe they hold the lock simultaneously. Without linearizability (say, using eventual consistency), two clients on different replicas could both read "lock is free" and both acquire it. That corrupts shared state and is exactly the class of bug that distributed locks are supposed to prevent.

---

### Tier 2 — Deep Dive Questions (why + trade-offs)

**Q1: Walk me through how etcd leases work and why they are better than Redis key TTLs for distributed coordination.**

**The key difference is atomicity and the Watch API's revision guarantee.** An etcd lease is a first-class object: you create a lease with a TTL, attach multiple keys to it, and when the lease expires, ALL attached keys are atomically deleted in a single Raft-committed operation. A Redis key with TTL expires independently — if you have a lock key and a separate metadata key that should be cleaned up together, Redis can leave them in an inconsistent state. More importantly, etcd's Watch API is revision-based: clients subscribe from `last_seen_revision + 1` and are guaranteed not to miss any event even during a disconnect. Redis keyspace notifications can be dropped under load or during connection failures. The trade-off is that etcd's consensus overhead (5–20ms per write) is higher than Redis's in-memory writes (~0.1ms). For coordination operations where correctness is critical, etcd's guarantees justify the latency cost.

**Q2: A client holds a distributed lock, and there is a network partition between the client and the lock service. Walk me through what happens, step by step, and what the correct outcome should be.**

**Step by step:** The client held lock L with session TTL 15s. It sends keep-alive every 5s. The partition begins. The client continues its work, unaware of the partition. 5 seconds pass — the client's keep-alive fails but it does not immediately know the session is dead (it will retry). 10 seconds pass — a second keep-alive fails. The client's retry logic detects the connectivity loss and starts attempting reconnect. 15 seconds pass — the etcd lease TTL fires. The Raft consensus commits the session expiry. All held locks (including L) are atomically released. A new client can now acquire L and receives fence_token = N+1. The original client eventually reconnects and tries to send its protected operation with fence_token = N. The storage layer rejects it because N < N+1. The original client's work is lost — it must treat this as an error and retry its entire operation from scratch, now re-acquiring the lock. The correct outcome: **the lock service correctly prevented two concurrent holders, and the fencing token correctly prevented stale work from corrupting the new holder's state.** The trade-off is that the original client lost work it thought was committed — this is the fundamental price of safety over liveness.

**Q3: How would you design the rate limiter to work correctly across 100 geographically distributed data centers?**

**You cannot have a single Redis cluster across 100 data centers — the latency would be 50–100ms per check.** The architecture for multi-region rate limiting is: (1) Each region has its own Redis cluster and handles rate limiting locally. (2) Each region is allocated a fraction of the global quota — if the global limit is 10,000 req/min and there are 10 equal regions, each gets 1,000. (3) Periodically (every 10–30 seconds), regions synchronize actual usage with a global aggregator and can "borrow" or "return" quota. This is called the **token bucket with quota redistribution** approach. The trade-off: brief over-admission is possible when one region underestimates and borrows quota that another region has already used. The alternative — synchronous global coordination for every check — is impractical due to latency. For most use cases, 1–2% over-admission across a 30-second window is acceptable. For hard quota enforcement (billing), you must reconcile offline and charge for overages rather than attempting to prevent them synchronously.

**Q4: In the saga pattern, what do you do if a compensating transaction fails?**

**Compensating transactions must be designed to always eventually succeed — this is a design contract, not a wishful assumption.** In practice: (1) Make every compensating transaction idempotent. (2) Retry the compensation with exponential backoff (up to N retries with increasing delays). (3) If all retries fail, write the saga to a dead-letter queue (DLQ). (4) The DLQ triggers an alert and a human operator handles it. The saga enters a COMPENSATION_FAILED terminal state. There is no perfect automated solution when a compensating transaction fails permanently. The business answer is: design compensating transactions so that failure is extremely unlikely (they operate on local databases, not external systems when possible), test them thoroughly, and have a manual remediation runbook for the cases that fall through. The alternative — leaving the saga in a permanently inconsistent state without human intervention — is worse.

**Q5: Describe the leader election failover timeline in detail, and what determines how fast a new leader is elected.**

**The timeline has four phases.** Phase 1: The leader's process crashes or its machine loses power. Elapsed: 0ms. Phase 2: The etcd lease TTL fires — this is the dominant factor. Default is 15 seconds. The old leader's key `/elections/{name}/leader` is atomically deleted. All watchers are notified. Elapsed: TTL (15s). Phase 3: Standby candidates (who were watching the leader key) receive the watch notification and simultaneously attempt to create the key with `create_if_not_exists`. This is a Raft consensus transaction: one candidate wins (the one whose Raft proposal is committed first), the others' proposals fail. Elapsed: 5–20ms for the Raft round-trip. Phase 4: The new leader starts its keep-alive loop and begins operating. The fencing token is CAS-incremented. Elapsed: ~1–5ms. Total failover time: **15–16 seconds for a crashed leader, ~200ms for a voluntary resign (key deleted immediately rather than waiting for TTL).** The key lever is the TTL — shorter TTL means faster failover but higher risk of spurious failovers due to transient network blips. 15 seconds is the standard because it tolerates 3 missed keep-alives (at TTL/3 interval) before expiry.

---

**Q6: Why does the service discovery system use consistent hashing for health check assignment, and what happens when a service discovery node itself fails?**

**Consistent hash assignment ensures every instance is checked by exactly one discovery node without any explicit coordination.** Each discovery node independently computes `ring.get_node(instance_id)` — if the result is `my_node_id`, it performs the health check. No locking, no leader coordination, no deduplication needed. When a discovery node fails, consistent hashing remaps its subset of instances to adjacent nodes on the ring. Each surviving node recalculates its responsibility set and starts checking the newly assigned instances. The convergence time is the TTL on the hash ring membership — typically under 30 seconds. The brief gap means some instances may miss a health check cycle, but with a 3-consecutive-failures threshold before marking CRITICAL, a single missed check does not cause incorrect deregistration.

**Q7: How does the sliding window counter algorithm work and why is it better than a simple fixed window?**

**The sliding window counter uses two counters — the current window's count and the previous window's count — combined with a weighted average based on how far into the current window we are.** The formula: `weighted_count = current_count + (previous_count × (1 - elapsed_fraction_of_current_window))`. If we are 30% into the current window, previous contributes 70% × its count. This creates a continuously sliding estimate that smoothly handles the transition between windows. The fixed window problem: at 12:00:59 and 12:01:01, a client can send 2× the limit because each burst is in a different 1-minute window. The sliding window counter eliminates this — the boundary burst is limited to roughly `(1 + (1 - elapsed_fraction)) × limit`, which converges to the correct limit with less than 0.003% average error. Memory: 2 integers per key (48 bytes per key for 1M keys = 48MB). This is the algorithm recommended for general-purpose production use because it has good accuracy, very low memory, and handles the boundary problem elegantly.

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud)

**Q1: You have a distributed lock service protecting 10,000 unique resources. A deployment pushed a bug that causes every service instance to re-acquire all of its locks simultaneously after a restart. 500 service instances restart simultaneously. Walk me through the thundering herd, what breaks, and how you prevent it.**

Start by sizing the problem: 500 instances × 20 locks each = 10,000 simultaneous acquisition requests, all hitting the Raft leader at once. The leader's Raft log receives 10,000 proposals in a short window. Raft batches proposals into log entries but the leader's disk write and follower replication still have to happen for each commit. At 50,000 ops/sec capacity, 10,000 simultaneous ops takes 200ms — tolerable. But the follow-on problem is the FIFO wait queues: for each of 10,000 named locks, 50 service instances are all queued. Each Raft commit grants one lock, triggers notification to the next waiter, which triggers another Raft proposal. This creates 10,000 × (50-1) = ~500,000 sequential Raft proposals over the next few seconds — a thundering herd converted into a thundering sequential stream.

Mitigations: (1) Add random jitter to the post-restart re-acquisition delay (0–30 seconds uniform). This spreads the 10,000 acquisitions over 30 seconds instead of hitting at once. (2) Rate-limit re-acquisition at the client SDK level: max N lock acquisitions per second per instance. (3) For the Raft leader, implement request batching at the lock manager layer: accumulate 10ms of proposals before writing to Raft log, reducing write amplification. (4) Exponential backoff on failed acquisitions: if the lock service returns OVERLOADED, back off with jitter before retrying. The broader lesson: rolling restarts should be staggered (deploy in waves of 10% of instances), and client SDK lock acquisition should have admission control to prevent self-inflicted thundering herds.

---

**Q2: You are designing the distributed rate limiter for a platform where one tenant (a large enterprise) legitimately sends 80% of all traffic. Your current design uses per-tenant Redis keys. What problems does this create and how do you address them?**

Three distinct problems emerge. First, hot key problem: all rate-limit checks for this tenant hash to the same Redis shard. At 500,000 total checks/sec with 80% for one tenant, that shard handles 400,000 ops/sec — a 50–100× higher load than other shards. Redis can handle ~100,000 ops/sec per CPU core; you are saturating 4 cores on a single shard while others sit idle.

Second, accuracy problem: if you move this tenant to a dedicated Redis shard, the sidecar batch-sync architecture means each of 100 service instances thinks it has a local budget of (limit / 100). But the enterprise tenant's traffic is unevenly distributed — maybe 95% of their calls go through one load-balanced endpoint, so one instance's local bucket is exhausted while 99 others have full budgets. The global limit is being applied globally but the local budget estimation is broken.

Third, SLA complexity: this tenant has negotiated 10× the standard limit. Hard-coding this in a generic policy system is fine, but the hot-key problem means their performance is worse than smaller tenants.

Solutions: (1) For the hot-key problem: hash the enterprise tenant's key with a suffix (e.g., `tenant-enterprise-hash(request_id) % 10`) to spread across 10 shards, then aggregate. The Lua script sums across all shards. (2) For the accuracy problem: the enterprise tenant gets a dedicated rate limit service instance (not just dedicated Redis shards), with sidecar sync tuned to their traffic pattern (sync every 10ms instead of 100ms). (3) For the SLA: treat this as a first-class configuration in the policy engine with a different policy tier. The lesson: rate limiting at scale requires treating outlier tenants as first-class design concerns, not edge cases.

---

**Q3: The service discovery system is used by 20,000 service instances. A bug causes all 20,000 instances to deregister and re-register simultaneously (e.g., a bad config push that triggers a restart loop). Walk through the impact on the platform and how you design for resilience against this.**

Quantifying the impact: 20,000 deregistrations + 20,000 registrations = 40,000 Raft proposals hitting the service discovery cluster. Each registration also triggers the Watch/Notification Engine to fan out to all subscribers watching that service — 5,000 watch subscriptions each receiving up to 20,000 events = 100,000,000 individual event deliveries. The gRPC streaming backpressure would cause watches to fall behind, and the service discovery cluster's CPU would be consumed by Raft log processing and watch fan-out.

Simultaneously: all 20,000 instances are deregistered. All DNS responses return empty. Every inter-service call in the platform fails with "no healthy endpoints." Duration: however long the Raft cluster takes to process 40,000 proposals — at 10,000 ops/sec capacity, that is 4 seconds of unavailability, not counting the health check propagation lag.

Resilience design: (1) **Self-preservation mode** is the primary defense: if renewal rate drops below 85% of expected for 15 minutes, stop expiring instances. But this event is instantaneous (< 1 minute), so self-preservation does not trigger. Need a different mechanism. (2) **Registration rate limiting**: the service discovery cluster should rate-limit registration operations from a single source — max 100 registrations/sec per service. This converts the 4-second impact into a 200-second controlled re-registration (tolerable, since instances are actually running and self-preservation keeps the stale entries alive during this window). (3) **Client-side negative caching**: when clients receive "no healthy endpoints," they should use their last known good endpoints for 30–60 seconds before treating it as a hard failure. (4) **Deployment guard**: a registration event that would deregister more than 10% of a service's instances in under 1 minute should trigger an alert and a human approval step. The broader lesson: the coordination plane must protect itself from being destabilized by the very system it serves.

---

**Q4: Design the idempotency key scheme for a distributed transaction coordinator handling bare-metal provisioning, and explain what goes wrong without it.**

Without idempotency keys: the client sends `POST /provision {server_id: srv-42, tenant: t-7}`. The coordinator starts the saga, sends Kafka command to inventory service (reserve srv-42). The coordinator crashes before recording the Kafka publish. On restart, the coordinator reads the saga from MySQL (status: STARTED, step 1: PENDING). It republishes the Kafka command. The inventory service processes it a second time and attempts to reserve srv-42 — which is already reserved from the first attempt. The inventory service returns an error (duplicate reservation), the saga sees a failure, and triggers compensation that incorrectly unreserves the server. The platform is now in an inconsistent state.

The idempotency key scheme fixes this: Every saga step gets a globally unique key: `{saga_id}-{step_index}`. Before executing any forward action, the participant service checks its local database for an existing record with this idempotency key. If found, it returns the previous result. If not found, it executes the action and writes the result with the idempotency key atomically (in the same local DB transaction). The check + execute + record must be in a single database transaction to prevent a TOCTOU race.

Three properties the scheme must satisfy: (1) **Globally unique per step**: `{saga_id}` ensures uniqueness across different sagas; `{step_index}` ensures uniqueness within a saga. (2) **Stored in the participant's own database** (not the coordinator's): this ensures idempotency is enforced at the execution boundary. (3) **TTL on idempotency records**: after 24 hours, old idempotency records can be purged — a retry after 24 hours should be treated as a new request. Redis with 24-hour TTL works well for fast dedup checks before hitting the MySQL participant database.

The broader principle: at-least-once delivery (which Kafka provides) combined with idempotent consumers is the only correct model for durable message-driven workflows. Exactly-once delivery at the transport layer is a stronger guarantee but not universally available and adds significant complexity.

---

## STEP 7 — MNEMONICS

### Memory Trick 1: "CLRTS-LD" (the 7 primitives)

**C**onsistent Hashing, **L**ock Service, **R**ate Limiter, **T**ransaction Coordinator, **S**ervice Discovery, **L**eader Election, **D**istributed Config

Or arrange them by the question they answer: "Where does this key live? Can I have this resource? How fast can I go? How do I undo this? Who is in charge? Where is this service? What is the current config?" — Hash, Lock, Rate, Transaction, Leader, Discovery, Config.

### Memory Trick 2: "FLAWS" — the five failure patterns that appear everywhere

**F**encing tokens (paused client resumes with stale lock)
**L**ease TTL expiry (holder crashes, resource must auto-release)
**A**P vs CP trade-off (availability vs. correctness on partition)
**W**atch with revision guarantee (no missed events on reconnect)
**S**ession + keep-alive (TTL/3 heartbeat, one missed = tolerated, two = expired)

If you can reason about FLAWS for any problem in this pattern, you can answer the deep-dive questions.

### Opening One-Liner for Any of These Problems

"These are all coordination problems — the hard part is not the happy path, it is keeping the system correct when one node fails in the middle of an operation, and preventing the coordination service itself from becoming a single point of failure for everything that depends on it."

Use this in your first 30 seconds before diving into requirements. It signals that you understand the domain.

---

## STEP 8 — CRITIQUE

### What the Source Material Covers Well

The source material is thorough on the following: the Raft consensus mechanism and why it is chosen over Redis-based alternatives; the fencing token design and the paused-client problem; the virtual node statistics (the `1/sqrt(V×N)` formula and why 150 vnodes is the right answer); the saga orchestration pattern with correct compensation ordering and idempotency key design; the TTL/3 keep-alive rule and its tolerance of missed heartbeats; the sliding window counter algorithm and why it fixes the fixed-window boundary problem; and the self-preservation mode in service discovery.

The API designs (gRPC protobuf, REST endpoints) are complete and realistic. The data models are carefully designed with correct indexing strategies. The capacity estimates use real numbers and derive architectural implications from them.

### What Is Missing, Shallow, or Could Be Improved

**Multi-region operation** is called "out of scope" in several problems but is frequently probed at staff+ level. Knowing the phrase "separate lock clusters per region" is not enough — you should be able to explain regional failover, cross-region config replication lag, and the trade-offs of global vs. regional rate limit enforcement.

**Clock skew** is mentioned in the context of Redis Redlock but not given a thorough treatment. The source material says "NTP < 100ms" but does not discuss how you design a system that is safe even if NTP correction is discontinuous (a common follow-up probe).

**The CAP theorem is referenced implicitly but not named explicitly.** Interviewers at FAANG regularly ask "is this a CP or AP system and why?" for each primitive. The guide should name the CP/AP classification for each problem:
- Lock service: CP (safety over availability)
- Rate limiter: AP (availability over strict accuracy)
- Service discovery: AP for reads (self-preservation), CP for writes (Raft registration)
- Config management: CP for writes, AP for cached reads
- Leader election: CP (split-brain prevention is non-negotiable)
- Consistent hashing: AP (gossip convergence, not Raft)
- Transaction coordinator: AP (saga eventual consistency), CP for 2PC

**Backpressure and flow control** between the Watch/Notification Engine and slow subscribers is not discussed. In a production system, a slow consumer receiving 20,000 watch events must be handled with either a bounded buffer (drop oldest) or a reconnect-from-revision mechanism. The source material implies revision-based reconnect but does not explain what happens to a subscriber that is too slow to consume.

**The Bootstrap Problem** for service discovery is mentioned briefly (you need service discovery to find etcd, but etcd IS service discovery) but not resolved with a concrete answer. The answer is hardcoded seed addresses for the service discovery cluster itself, stored in configuration files that are deployed before the platform starts.

### Senior Probes to Anticipate

These are the follow-up questions that an interviewer uses to stress-test a candidate who has given a good initial answer:

1. "You said you use Raft. How does Raft leader election work under a network partition? Specifically, can you have two Raft leaders?" (Expected answer: No — the old leader loses quorum and cannot commit. The new leader is elected by the majority. Pre-vote and leader leases prevent false positives.)

2. "You use 150 virtual nodes. What if I add 1 very high-capacity node and want it to handle 3× the normal load?" (Expected answer: Set weight=300 on that node. vnodes = 150 × (300/100) = 450 vnodes for that node. It will own 3× the ring positions and receive 3× the keys.)

3. "In your saga, what if step 3 fails after partial side effects? The participant's forward action updated an external API (email notification) that cannot be compensated." (Expected answer: Design steps with external non-compensatable side effects as the last step. Mark them `is_compensatable = false`. Document that if the saga fails after this step, there is no undo — human intervention handles the exception. This is a business design constraint, not a technical one.)

4. "Your rate limiter has a Redis Lua script for atomic token bucket operations. What happens under 50ms Redis latency spikes?" (Expected answer: The sidecar's timeout kicks in; it uses the local token bucket estimate and proceeds. The brief over-admission during the Redis slowdown is acceptable. You should monitor Redis p99 latency and page if it exceeds the sidecar timeout threshold.)

5. "How do you handle a service discovery system that is partitioned from half the cluster? Which half serves requests?" (Expected answer: The majority half (has quorum) serves as the Raft leader and handles registrations. The minority half can still serve reads from its in-memory cache, which is stale. The self-preservation mode prevents the minority half from deregistering instances it cannot reach. Clients should prefer the majority partition for registration but can use either for reads.)

### Traps to Avoid

**Trap 1: Using Redis for anything that requires linearizable guarantees.** Redis is an in-memory database, not a consensus system. SETNX + TTL is two separate operations and is not atomic under failures. Redlock is timing-dependent. If an interviewer pushes back with "but Redis is fast," the counter is: "Fast and correct are different things. We need correctness here, and Raft gives us that at an acceptable latency cost for this use case."

**Trap 2: Forgetting that the coordination service itself can fail.** Every design in this pattern should have an explicit answer for "what happens when the lock service / rate limiter / service discovery cluster goes down?" The answer is not "it won't." The answer is: client SDK has a timeout, falls back to local state, and the system has a defined degraded mode.

**Trap 3: Treating 2PC as the default for distributed transactions.** 2PC blocks all participants during the prepare phase. If any participant is slow or unavailable, the entire transaction blocks indefinitely (the coordinator holds locks on all participants). Sagas have worse consistency properties but are far more operationally tractable for long-running workflows. The question "when would you use 2PC instead of sagas?" has a specific answer: tight coupling, short-duration operations, when intermediate states must not be visible (financial transactions, schema migrations), and when all participants are controlled services that will not be slow.

**Trap 4: Not mentioning the observer API in leader election.** The candidate who designs leader election but only talks about candidates and not about how non-candidate services (load balancers, clients, workers) discover the current leader is missing a significant part of the system. The observer pattern (watch the leader key, cache locally, route to the cached address) is a separate concern from the election mechanism itself.

**Trap 5: Confusing sharding with consistent hashing.** Consistent hashing minimizes data movement on topology change. Range-based sharding (used by HBase, Bigtable) enables range scans but requires explicit rebalancing. Hash-based sharding (simple `hash(key) % N`) is simple but causes K/N key movement on N→N+1 (catastrophic for cache systems). Know which is appropriate for which problem and be able to articulate the trade-offs concisely.

---
