# Pattern 13: Rate Limiting — Interview Study Guide

Reading Pattern 13: Rate Limiting — 2 problems, 8 shared components

---

## STEP 1 — PATTERN OVERVIEW

Rate Limiting is one of the most frequently asked system design topics at FAANG and similar companies. It sits at the intersection of distributed systems, algorithm design, and production reliability. Almost every major API platform — Stripe, Twilio, AWS, GitHub, Cloudflare — has had to build or operate a rate limiter at scale.

This pattern covers two distinct but related problems:

**Problem 1: API Rate Limiter**
A distributed, externally-facing system that enforces per-client request limits at the API gateway boundary. Think of it as the bouncer at the door: it checks every incoming request against a configurable rule, increments a shared counter in Redis, and either lets the request through or returns HTTP 429 Too Many Requests. The primary concern here is throughput (8M RPS), accuracy (< 1% over-admission), low latency overhead (< 5 ms p99 per request), and a flexible rule engine (multiple tiers, multiple dimensions, hot-reload without restarts).

**Problem 2: Throttling Service**
An internally-facing system that controls how fast microservices talk to each other. This is not the bouncer at the door — it is the traffic controller inside the building. Its primary concerns are adaptive rate adjustment based on real-time downstream health, priority queuing (P0 payment requests must never be dropped), backpressure propagation to prevent cascading failures, and zero-latency overhead (everything runs in-process with no network hops on the hot path).

**Why both problems appear in the same pattern:** They share the same core algorithms (sliding window counter, token bucket), the same backing infrastructure (Redis, PostgreSQL, Kafka), and the same conceptual goal (prevent overload). But they make opposite architectural choices: the API rate limiter uses a centralized Redis counter store because distributed enforcement is non-negotiable, while the throttling service uses entirely in-process state because latency is non-negotiable. Understanding both problems and why they diverge is what separates a strong senior candidate from everyone else.

**The 8 shared components you will encounter across both problems:**
1. Redis — counter storage (API rate limiter) or threshold caching (throttling)
2. PostgreSQL — durable source of truth for rules/policies
3. Kafka + S3/Athena — async audit logging pipeline
4. In-process LRU/state caching — eliminates network hops on the hot path
5. Sliding Window Counter / Token Bucket algorithms — the core rate limit logic
6. Circuit Breaker — fail-open protection around the backing store
7. Admin API with hot-reload — runtime config changes without restarts
8. Eventual consistency acceptance — < 100 ms propagation window

---

## STEP 2 — MENTAL MODEL

**The core idea:** A rate limiter answers one question per request, in under a few milliseconds, across thousands of nodes simultaneously: "Has this client already used up their allocation in the current time window?" The hard part is not answering the question — it is answering it consistently, atomically, and cheaply when millions of nodes are all incrementing the same counter at the same time.

**The real-world analogy:** Think of a nightclub with a clicker counter at the door. The bouncer has one clicker. Every person who walks in, they click it. When it hits 100, the door closes. Simple. Now imagine the nightclub has 500 doors, all open simultaneously, and you need to enforce the same 100-person total capacity across all doors simultaneously. If every door has its own counter, the club fills to 50,000 people (500 doors × 100 each). If every door shares one counter, they have to shout across the club to each other on every person who enters — too slow. The solution is to have a single electronic counter on a fast shared network (Redis) that each door can query in under 2 milliseconds. That is exactly the architecture.

**The throttling service analogy is different:** Imagine the kitchen in a restaurant. The kitchen can only plate 200 dishes per hour. If every waiter sprints to the kitchen window the moment a table orders, the kitchen gets overwhelmed and starts dropping plates. Instead, you want the waiters to self-regulate — to sense that the kitchen is backed up (latency increasing) and naturally slow their pace. They don't need to talk to a central counter. They just watch the kitchen and adjust. This is adaptive throttling with in-process state and backpressure signals.

**Why this is hard — five reasons:**

1. **Atomicity under concurrency.** Reading a counter value and incrementing it are two separate operations. If two concurrent requests both read "99" and both decide to allow themselves, you get 101 requests past the limit. You need atomic read-check-increment, which is why Lua scripts on Redis exist.

2. **Distributed consistency.** In a fleet of 500 API gateway nodes, each client's requests are distributed randomly across nodes. You cannot count locally — a client could route to all 500 nodes and get 500× their limit. The counter must be shared and consistent enough.

3. **Latency budget.** The rate limiter sits in the critical request path. Every millisecond you add is a millisecond of latency for every API call on your platform, forever. This creates an extreme constraint: the entire rate-limiting decision — rule lookup, counter check, header setting — must complete in under 5 milliseconds p99.

4. **Algorithm trade-offs.** Fixed window counters allow double-bursts at window boundaries. Sliding window logs are perfectly accurate but require O(N) memory (50 KB per client at 1000 req/min). Token buckets allow intentional bursts. You must know which algorithm to recommend and exactly why, with the trade-off table memorized.

5. **Failure modes.** When Redis is down, do you reject all traffic (safe but catastrophic) or allow all traffic (risky but available)? The answer is almost always fail-open, but you need to explain the reasoning, the mitigations, and the monitoring that makes this safe.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these before drawing anything. The answers change the entire architecture.

**Question 1: Is this for external API clients (third-party developers, partners) or for internal service-to-service traffic?**
- External clients → API Rate Limiter: you need distributed counters (Redis), tiered limits (free/pro/enterprise), standard HTTP 429 headers, an admin rule management API
- Internal services → Throttling Service: you need in-process decisions, adaptive rate adjustment, priority queuing, backpressure propagation
- *Red flag:* Candidate jumps to design without asking this. The architectures are fundamentally different.

**Question 2: What are the scale requirements — how many requests per second, how many unique clients?**
- < 10K RPS, < 1M clients → local in-process per-node counters may be viable with sticky routing
- 1M–10M RPS, 10M+ clients → Redis Cluster is mandatory; in-process alone doesn't work
- *What changes:* The number of Redis shards, whether you need pipelining, whether shadow counting is necessary for throughput

**Question 3: What accuracy level is acceptable — is some over-admission tolerable, or is every request counting absolutely required?**
- Exact accuracy → Sliding Window Log (perfect but memory-intensive) or strong consistency (expensive latency)
- < 1% error acceptable → Sliding Window Counter (this is almost always the right answer)
- *What changes:* Algorithm selection, Redis atomicity requirements, local approximation feasibility

**Question 4: What is the fail behavior when the rate-limiting backing store (Redis) is unavailable — fail open (allow all traffic) or fail closed (reject all traffic)?**
- Most platforms: fail open → availability > perfect accuracy; you alert and investigate
- Compliance/security-critical systems (e.g., financial API): fail closed may be required
- *Red flag:* Candidate doesn't ask and assumes one or the other without justifying it

**Question 5: Does the rate limiter need to support multiple dimensions simultaneously — per user ID, per API key, per IP, per endpoint — or just one?**
- Single dimension → simple key scheme
- Multi-dimensional → rule engine with priority ordering, multi-key Redis operations, more complex in-process cache
- *What changes:* Data model complexity, rule matching algorithm, how you handle conflicts (which dimension wins)

**What changes based on answers:**
- Scale changes: Redis shard count, shadow counter strategy, pipelining
- Accuracy changes: algorithm choice
- Failure mode changes: circuit breaker configuration, local fallback depth
- Dimensions change: data model, Lua script key structure, in-process rule lookup

**Red flags that indicate a shallow understanding:**
- Saying "I'll use Redis" without explaining why (not Memcached, not Cassandra, not DynamoDB)
- Designing with only per-IP rate limiting (easily bypassed by any distributed client)
- Not distinguishing between API rate limiting and service throttling
- Not asking about failure mode before the design is complete

---

### 3b. Functional Requirements

**API Rate Limiter — core requirements:**
- Enforce rate limits per configurable dimensions: user ID, API key, IP, endpoint pattern, tenant
- Multiple tiers: free (100 req/min), pro (1,000 req/min), enterprise (10,000 req/min); no redeployment to change limits
- Return standard headers on every response: `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`, `Retry-After`
- Return HTTP 429 with a structured JSON error body when limit is exceeded
- Simultaneous per-second burst limit AND per-minute sustained limit
- Distributed enforcement: a client cannot bypass limits by routing requests to different gateway nodes
- Audit log: every 429 response logged with client identity, rule matched, timestamp

**Throttling Service — core requirements:**
- Control how fast internal services (producers) send work to downstream services (consumers)
- Priority queuing: P0 (critical) through P3 (batch/low), with absolute priority for P0
- Adaptive throttling: automatically adjust throttle thresholds based on downstream health signals (error rates, p99 latency)
- Backpressure propagation: when downstream is saturated, callers reduce their rate before queues fill
- Graceful degradation: return cached/default/partial responses instead of errors when possible
- Circuit breaker coordination across callers in the fleet

**Scope boundary — be explicit in the interview:**
- API Rate Limiter is NOT: DDoS mitigation (L3/L4, handled by CDN/WAF), authentication, request queuing, geographic blocking
- Throttling Service is NOT: API-boundary rate limiting, load balancing, service discovery, Kafka infrastructure management

**Clear one-sentence statement of each:**
- API Rate Limiter: "A distributed gateway middleware that enforces configurable per-client request rate limits across a fleet of API gateway nodes using a shared Redis counter store, returning HTTP 429 with standard headers when clients exceed their allocated rate."
- Throttling Service: "An in-process SDK embedded in each microservice that controls the rate of outbound calls to downstream services using adaptive token buckets, priority queuing, and circuit breakers, coordinated by a central control plane."

---

### 3c. Non-Functional Requirements

Derive these from the functional requirements and scale numbers, then state the trade-offs explicitly.

**Latency overhead (the most critical NFR):**
- **API Rate Limiter:** < 5 ms p99 total overhead (includes Redis round-trip ~1–3 ms + rule lookup < 0.5 ms + header setting < 0.5 ms). This is why Redis is mandatory — Cassandra's 5–15 ms p99 already exceeds the entire budget.
- **Throttling Service:** < 0.5 ms p99 for in-process token bucket check (zero network hops). The control plane path (threshold updates) is async and does not add to request latency.
- ✅ In-process decision = 0 network hops, < 0.1 ms; ❌ External service call = 2–5 ms minimum, unacceptable for 50M RPS fleet

**Availability (fail behavior NFR):**
- Both systems: 99.99% uptime; fail open when backing store unavailable
- **Reasoning:** At 8M RPS, a fail-closed policy during a Redis outage = 8M errors/second = complete platform outage. This is far worse than temporarily over-admitting some traffic.
- ✅ Fail-open = platform stays up, some clients over-admitted, alert fires; ❌ Fail-closed = platform down for the duration of the Redis outage
- Mitigations that make fail-open safe: Redis Cluster auto-failover (MTTR < 30 s), in-process fallback counters (per-node soft limits), immediate alerts on any fail-open event

**Consistency:**
- **Both systems:** Eventual consistency with < 100 ms propagation window is acceptable. Strong consistency would require synchronous cross-node coordination (distributed locks), which violates the latency budget.
- ✅ Eventual consistency = low latency, horizontal scalability, < 1% over-admission at boundaries; ❌ Strong consistency = 5–20 ms per request for distributed lock acquire/release

**Accuracy:**
- < 1% over-admission at burst window boundaries (prefer admitting slightly too many over blocking legitimate traffic)
- Under-admission (blocking legitimate traffic) is treated as a business-critical bug
- The sliding window counter's weighted approximation achieves < 0.003% error in practice (Cloudflare-validated at billions of requests)

**Configuration hot-reload:**
- Rule/policy changes must take effect within 5–30 seconds without a rolling restart
- **API Rate Limiter:** 30-second in-process LRU TTL → changes propagate within one refresh cycle
- **Throttling Service:** Control plane pushes via gRPC stream → < 5 seconds to all instances

**Auditability:**
- Every rejected/throttled request logged with client identity, rule matched, and timestamp
- Kafka decouples this from the hot path — the audit write is non-blocking

---

### 3d. Capacity Estimation

**API Rate Limiter — work through this in the interview:**

Start with the formula: `total_RPS = sum(active_clients_per_tier × avg_req_per_min_per_tier / 60)`

Anchor numbers:
- 10M registered API consumers, 20% daily active = 2M active clients
- Tier split: 80% free, 18% pro, 2% enterprise
- Average utilization: free uses 5% of limit, pro uses 20%, enterprise uses 20%

Working it out:
| Tier | Active Clients | Avg Req/Min | RPS |
|------|---------------|-------------|-----|
| Free (100 req/min limit) | 1.6M | 5 | 133K |
| Pro (1,000 req/min limit) | 360K | 200 | 1.2M |
| Enterprise (10,000 req/min limit) | 40K | 2,000 | 1.33M |
| **Total average** | | | **2.67M RPS** |
| **Peak (3× average)** | | | **8M RPS** |

Rate-limit operations: 2 checks per request (per-minute + per-second burst) → **16M Redis ops/sec at peak**

This single number is the architectural implication anchor: at 16M ops/sec, a single Redis instance maxes out at ~1M simple ops/sec → **Redis Cluster with at least 27 shards is mandatory** (round up to 32 for headroom). CPU-bound, not memory-bound (640 MB total counter data, trivial for Redis).

**Storage:**
- Counter keys: 2M active clients × 2 windows × 160 bytes = ~640 MB (fits in any Redis node)
- Audit log: 8M RPS × 0.1% rejection rate × 200 bytes × 86,400 s = ~138 GB/day raw → ~14 GB/day compressed

**Throttling Service — different scale story:**
- 500 distinct services × 20 instances = 10,000 service instances
- 5,000 RPS per instance → **50M in-fleet RPS total**
- Hot-path throttle checks: 50M/sec, but **all in-process — zero Redis ops on hot path**
- Control plane: only 500 adaptive updates/sec (one per service pair per second) — trivial
- This is why in-process wins: the Redis ops at 50M/sec would require ~85 Redis shards; in-process requires zero

**Time this estimation in the interview: target 4–6 minutes.** Start with users → active fraction → per-user behavior → total ops → storage → architectural implication. One clear conclusion at the end.

---

### 3e. High-Level Design

**API Rate Limiter — 6 components, draw in this order on the whiteboard:**

1. **Client / CDN / WAF** (top of diagram) — L3/L4 flood absorption, TLS termination, out of scope for this design
2. **API Gateway Cluster** (N stateless nodes) — entry point; hosts the rate-limit middleware in-process; stateless so new nodes are immediately functional
3. **Rate Limit Middleware** (inside each gateway node) — extracts client identity → looks up rule in in-process LRU cache → runs Lua script on Redis → sets headers → returns 429 or forwards
4. **Redis Cluster (Counter Store)** — 32 shards, each key `rl:{client_id}:{window_seconds}:{window_bucket}`; hash tags ensure current and previous bucket keys land on same shard; Lua scripts for atomic check-increment
5. **Rules Store (MySQL) + Rules Cache (Redis)** — MySQL is the authoritative rules source; admin API writes there; Redis rules cache is populated every 30 seconds and acts as fan-out to all gateway nodes; in-process LRU on each gateway node refreshes from Redis rules cache
6. **Kafka + S3/Athena Audit Sink** — async 429 event logging, non-blocking to hot path

**Data flow for an allowed request (memorize this sequence):**
1. Client sends request with `Authorization: Bearer <api_key>`
2. Auth middleware validates key, attaches `user_id=u123, tier=pro` to context
3. Rate limit middleware reads tier from context; looks up rule in **in-process cache** (0 network hops): `{limit: 1000, window: 60s}`
4. Constructs Redis key with hash tag: `rl:{u123}:60:28928160` (window bucket = `floor(now/60)`)
5. Runs **atomic Lua script** on the correct Redis shard: reads prev bucket count, reads current count, computes weighted estimate, if under limit → INCR + EXPIRE
6. Redis returns: counter = 142, limit = 1000 → allowed
7. Middleware sets headers: `X-RateLimit-Limit: 1000`, `X-RateLimit-Remaining: 858`, `X-RateLimit-Reset: 1735689660`
8. Request forwarded to upstream; headers appended to response

**Throttling Service — 7 components, draw in this order:**

1. **Service Instance** (each of 10,000 instances) — hosts all in-process throttle state
2. **Priority Classifier** (in-process) — assigns P0–P3 based on endpoint and verified identity
3. **Admission Controller** (in-process) — checks health + queue depth + circuit breaker + quota
4. **Priority Queue** (in-process, MLFQ) — 4-level bounded queue; P0 always first; starvation prevention
5. **Outbound Throttle Client** (in-process per downstream) — token bucket + circuit breaker + client-side throttle
6. **Throttling Control Plane** (separate service) — Adaptive Threshold Engine (AIMD + EWMA), Policy Store (PostgreSQL), Metrics Aggregator (Prometheus/VictoriaMetrics), Push Gateway (gRPC streams to all instances)
7. **Kafka + S3 Audit Sink** — same pattern as API rate limiter

**Key design decisions to call out explicitly in the interview:**

- **Why Redis Cluster and not Cassandra for counters?** Cassandra p99 is 5–15 ms — blows the 5 ms total budget. Redis p99 < 3 ms. Also, Redis Lua scripting provides the atomicity needed for check-and-increment without distributed locks.
- **Why in-process for throttling service and not a sidecar (Envoy ext_authz)?** Sidecar adds 2–5 ms per request (loopback TCP + ext_authz RPC). At 50M RPS, 4 ms × 50M = 200 billion ms/second of added latency. In-process is < 0.1 ms, zero network.
- **Why Lua scripts over Redis MULTI/EXEC?** MULTI/EXEC uses optimistic locking with WATCH — under high contention, watch violations cause retry storms. Lua scripts execute atomically on the server: single network round-trip, no contention, no retries.
- **Why hash tags in Redis keys?** Without them, `rl:{u123}:60:28928160` and `rl:{u123}:60:28928159` (current and previous windows) could land on different shards. A Lua script cannot span shards in Redis Cluster. Hash tags force both keys to the same slot.

---

### 3f. Deep Dive Areas

The interviewer will push on one of these three areas. Go deep proactively.

**Deep Dive 1: Rate Limiting Algorithm Selection**

This is the most commonly probed area. Know all five algorithms cold.

The problem: given a request, determine in < 3 ms whether the client is within their limit, increment their counter atomically, and return accurate headers — without significant over-admission.

Five algorithms and when each applies:

**Fixed Window Counter** — simplest. `count = INCR(key); if count == 1: EXPIRE(key, window)`. O(1) memory (~160 bytes/client). But allows 2× burst at window boundaries: a client can send `limit` requests in the last second of window N and `limit` in the first second of window N+1. ✅ Simplest to implement; ❌ 2× boundary burst; use only for simple internal limits where occasional bursts are acceptable.

**Sliding Window Log** — perfect accuracy. Stores each request as a timestamped entry in a sorted set. Removes expired entries and counts what remains. O(requests_in_window) memory — at 1,000 req/min per client with 50 bytes per entry = 50 KB per client. With 2M active clients = 100 GB. ✅ Perfect accuracy, no boundary burst; ❌ Memory-prohibitive at scale; use only for very low-volume clients (< 100 req/min) where perfect accuracy is a hard business requirement.

**Sliding Window Counter** — the recommended default for high-scale production. Stores two integer counters (current window + previous window). Estimates count as `floor(prev_count × (1 - elapsed_in_window / window_size)) + current_count`. O(1) memory (~320 bytes/client, two keys). < 1% error in practice (Cloudflare measured 0.003% at billions of requests per day). ✅ O(1) memory, < 1% error, adopted by Cloudflare/Stripe/Kong; ❌ Approximation only, not suitable if exact per-window enforcement is a hard requirement.

**Token Bucket** — for burst-tolerant APIs. Tokens refill at constant rate; a request consumes one token; the bucket has a capacity > sustained rate, so bursts are permitted up to the bucket capacity. O(1) memory (~80 bytes/client). ✅ Intentional burst control, very memory-efficient; ❌ Harder to reason about the "window" — clients can drain a full bucket instantly if they've been idle; for the throttling service, the rate is dynamically updated by the control plane (AIMD).

**Leaky Bucket** — for output rate smoothing, not API rate limiting. Requests queue; background drain at constant rate. ✅ Perfectly smooth output, no bursts in processing; ❌ Requires background drain goroutine per client, variable latency, O(queue) memory at worst — completely impractical for an API rate limiter that needs instant accept/reject. Only use for output smoothing (e.g., email delivery, SMS sending).

**What the interviewer is testing:** Can you articulate the memory vs. accuracy trade-off? Do you know why sliding window counter is the industry default? Do you understand when each is appropriate?

**Unprompted trade-off to mention:** "We also layer a per-second token bucket burst check on top of the per-minute sliding window counter. This catches instantaneous floods — a client with a 1,000 req/min limit could send all 1,000 in 1 second without a burst limit. The Lua script checks the burst key first (fast path rejection) then the minute-window counter."

---

**Deep Dive 2: Distributed Counter Consistency and Redis Architecture**

The problem: a single Redis key for a client needs to be incremented atomically by 500 concurrent gateway nodes, with < 3 ms p99 latency, at 16M operations/second.

Five approaches and why Redis Cluster wins:

**Central Redis Cluster** — selected. Network RTT to Redis in same datacenter is < 1 ms typical, < 3 ms p99. Lua atomicity handles concurrency without locks. Hash slots ensure a client key always hits the same shard. ✅ Sub-millisecond in same-AZ, Lua atomicity, consistent hashing; ❌ Network hop required, fail-open on cluster outage.

**Gossip/CRDT counters** — each node maintains a local counter, CRDTs eventually merge. Synchronization lag is seconds, not milliseconds, meaning a client can over-admit by `limit × num_nodes × sync_lag_seconds`. ✅ No network hop; ❌ Over-admission proportional to sync lag and fleet size — at 500 nodes with 2-second sync, a client with limit 100 could admit 100,000 requests. Completely unusable.

**Sticky routing (client → specific node)** — route a client's requests to a single gateway node via consistent hashing. That node maintains a local counter. ✅ Zero network hops, exact counters; ❌ Node failure invalidates client affinity, new requests during failover go to wrong node; cannot horizontally scale gracefully.

**Two-phase distributed lock** — acquire a Redis lock, read-increment-write, release. ✅ Strongly consistent; ❌ Lock acquire + release = 5–20 ms round trips, 4× the budget; under contention, lock queue forms.

**Local shadow counter (optimization, not primary architecture)** — gateway nodes batch Redis updates. Each node tracks a local delta and syncs to Redis every 100 ms. Between syncs, the local counter is used for fast decisions. ✅ Reduces Redis ops by 100×; ❌ Allows up to 100 ms of over-admission per sync interval. Used as a throughput optimization on top of the primary Redis Cluster architecture.

**Redis Cluster sizing (state this explicitly):**
- 16M Lua ops/sec ÷ 600K ops/sec per Redis primary = 27 shards → use 32 (round up for headroom)
- 32 primaries × 3 nodes (1 primary + 2 replicas per shard in different AZs) = 96 Redis nodes total
- Memory: 640 MB total data ÷ 32 shards = 20 MB per shard — trivial, this is CPU-bound

**Hash tag gotcha — call this out proactively:** "Without hash tags, the current window key and previous window key for the same client could land on different shards. Redis Cluster cannot run a Lua script that touches keys on multiple shards. We solve this by using the client ID as the hash tag: `rl:{u123}:60:28928160` — the cluster hashes only the contents of the curly braces, so all keys for `u123` land on the same shard."

**Unprompted trade-off:** "If Redis is down, we fail open. The alternative — fail closed — would mean a Redis outage causes a complete API platform outage. That's a far worse failure mode. We mitigate fail-open risk with: (1) Redis Cluster auto-failover in < 30 seconds, (2) in-process fallback counters as a secondary soft limit, (3) immediate PagerDuty alert on any fail-open event."

---

**Deep Dive 3: Adaptive Throttling with AIMD (Throttling Service)**

The problem: static throttle limits are set at deploy time based on assumptions that become stale. A downstream service degrading from 10K RPS to 3K RPS capacity should automatically cause callers to reduce their send rate — not because someone manually changed a config, but because the system detects the degradation and responds.

**AIMD (Additive Increase, Multiplicative Decrease):**
- Borrowed from TCP congestion control (in use since 1988, proven at internet scale)
- On each healthy tick: `new_rps = min(max_rps, current_rps + max_rps × 0.05)` — increase by 5% of max
- On overload detection: `new_rps = max(min_rps, current_rps × 0.5)` — halve immediately
- Overload condition: smoothed error rate > threshold OR smoothed p99 latency > threshold

**EWMA smoothing prevents oscillation:**
- Each second: `smoothed = 0.3 × observed + 0.7 × smoothed`
- A single error spike has only 30% weight; it takes several consecutive bad ticks to trigger a decrease
- Slow recovery (additive increase) vs. fast decrease (multiplicative) is the asymmetry that prevents sawtooth oscillation
- Cooldown period: minimum 30 seconds between increases after a decrease prevents premature ramp-up

**Other approaches compared:**
- **PID Controller** — more mathematically precise, but requires careful gain tuning (Kp, Ki, Kd) per service type; can oscillate if poorly tuned; AIMD's simplicity is its advantage
- **Google's Client-Side Throttle** — self-contained, zero control plane: `probability = max(0, (requests - K × accepts) / (requests + 1))`; clients pre-reject locally based on recent accept/reject ratio; works as a fallback when the control plane is down; only reacts to server rejections, not latency degradation
- **Gradient descent on latency** — mathematically optimal operating point, but noisy signals cause instability in practice

**Unprompted trade-off:** "The adaptive throttle has an `adaptive_min_rps` floor — it never reduces below 10–20% of max. This ensures the downstream always receives some traffic to process and recover. Without a floor, aggressive AIMD could drive the rate to zero, which starves the downstream of the work it needs to prove it has recovered, creating a deadlock where the system is stuck at minimum rate indefinitely. We also have a 5-minute timeout: if the system has been throttled for 5 minutes without recovery, we reset to 50% of max to break the deadlock."

---

### 3g. Failure Scenarios

Walk through these with the "senior framing": state the scenario, the blast radius, the detection mechanism, and the mitigation.

**For API Rate Limiter:**

**Scenario 1: Single Redis shard fails.**
- Blast radius: ~3% of clients (1/32 shards) get fail-open for 10–30 seconds
- Detection: Redis error rate metric spikes; PagerDuty fires
- Mitigation: Redis Cluster automatic failover promotes a replica within 10 seconds; affected clients get fail-open during the 10-second window; other shards unaffected
- Senior framing: "Because we use Redis Cluster with 32 shards, a single shard failure affects only 1/32 of clients. This is an intentional blast-radius containment design."

**Scenario 2: Full Redis cluster failure.**
- Blast radius: All rate limiting is effectively disabled (fail-open) for the duration
- Detection: Redis connection error counter spikes to 100%; circuit breaker opens immediately
- Mitigation: In-process fallback counter per gateway node activates (per-node soft limit, not fleet-wide distributed); alert fires; SRE investigates; Redis Cluster auto-healing or manual recovery
- Senior framing: "We accept this failure mode intentionally. The alternative — fail closed — would mean our entire API platform is down whenever Redis has a blip. Availability of the API is worth more than perfect rate limiting accuracy for a 30-second window."

**Scenario 3: Kafka audit pipeline failure.**
- Blast radius: 429 events are not logged; audit gap
- Detection: Kafka consumer lag metric; broker health check
- Mitigation: Rate limiting still enforced (Kafka is async and non-blocking); retroactive audit from access logs is possible; events can be replayed once Kafka recovers
- Senior framing: "The audit pipeline is never in the critical path. Decoupling audit from enforcement via Kafka is a deliberate design decision: losing audit data is a compliance concern, but losing enforcement would be a revenue concern."

**For Throttling Service:**

**Scenario 4: Control plane (Adaptive Threshold Engine) goes down.**
- Blast radius: Adaptive thresholds freeze at last-known values; no new policy updates
- Detection: gRPC stream heartbeat from instances reports "push gateway unreachable"
- Mitigation: (1) Service instances continue using last-known thresholds in-process. (2) Google's client-side throttle continues self-calibrating in-process with no control plane input. (3) Circuit breakers remain active in-process. The system operates safely but statically until the control plane recovers.
- Senior framing: "The control plane's only role in steady state is updating thresholds — losing it is a config management issue, not a request-serving issue. This is the key design win of the in-process architecture: the control plane is out of the critical path."

**Scenario 5: Downstream service completely fails.**
- Blast radius: All calls to the downstream fail; circuit breaker opens within 10 seconds
- Detection: Circuit breaker monitors error rate per (caller, callee) pair; opens at 5% error rate in 10-second window
- Mitigation: Once the circuit is open, the throttle client stops sending to the downstream immediately (zero new calls). Graceful degradation returns cached or default responses for eligible request types. P0 requests that cannot be degraded receive 503. Alert fires; SRE investigates.
- Senior framing: "Circuit breakers are the last line of defense in the throttling service. The adaptive throttle should have already started reducing the rate before the circuit opens — the circuit breaker catches cases where degradation is too sudden for the EWMA-smoothed adaptive algorithm to react in time."

---

## STEP 4 — COMMON COMPONENTS

These eight components appear in both problems. Know each one cold.

---

**Component 1: Redis as Counter Store (or Threshold Cache)**

**Why used:** Sub-millisecond latency (< 1 ms GET/SET in-datacenter), atomic Lua scripting for check-and-increment without distributed locks, native TTL for automatic key expiry, consistent hashing via Redis Cluster hash slots.

**Key configuration:**
- In API Rate Limiter: Redis Cluster mode, 32 shards, 1 primary + 2 replicas per shard, hash tags for multi-key Lua atomicity, `maxmemory-policy allkeys-lru` as safety net, AOF persistence optional (counters can be rebuilt if Redis restarts — mild over-admission acceptable)
- In Throttling Service: Redis is NOT on the hot path; used only for threshold distribution from control plane to service instances (auxiliary caching, not per-request access)

**What you lose without it:** Without Redis Cluster, you either have a single Redis instance (caps at ~1M ops/sec, catastrophic SPOF) or you have local in-process counters per gateway node (clients bypass limits by routing to multiple nodes). There is no good alternative at 16M ops/sec that meets the 3 ms p99 budget.

✅ Sub-millisecond, atomic Lua, native TTL, horizontal via hash slots; ❌ Volatile (data lost on failure without AOF), requires cluster management, fail-open on unavailability

---

**Component 2: PostgreSQL for Configuration (Rules / Policies)**

**Why used:** ACID transactions guarantee that a rule update (e.g., raising a client's tier limit) is either fully committed or fully rolled back — no partial updates. Row-level locking allows concurrent admin updates without table-level locks. JSONB (PostgreSQL) handles the complex nested configuration objects in the throttle policy (circuit breaker config, priority shed thresholds, graceful degradation config) with GIN indexing for efficient querying.

**Key configuration:**
- API Rate Limiter uses MySQL (simpler rule schema, no complex JSONB queries needed)
- Throttling Service uses PostgreSQL specifically for JSONB with GIN indexes (enables querying like "find all policies where circuit_breaker error_threshold < 0.05")
- Both: 1 primary + 2 read replicas; small dataset (< 20 MB rules, < 10 MB policies); not on the hot path — only accessed by admin API and rules cache population

**What you lose without it:** Without a durable relational store, rule changes are not transactional. An admin update that changes both `limit_count` and `tier` could partially apply, leaving the system in an inconsistent state. Also, no audit history of who changed what when.

✅ ACID transactions, row-level locking, structured schema, change history; ❌ Not suited for high-throughput reads (use Redis cache for that), requires connection pooling

---

**Component 3: Kafka for Async Audit Logging**

**Why used:** Decouples 429/throttle event logging from the critical request path. The gateway middleware can fire-and-forget to Kafka without waiting for an acknowledgment. Kafka's durable log ensures no events are lost even if the downstream consumer (S3 sink) is temporarily unavailable. High throughput (300 MB/s for throttle events) handled by partitioning.

**Key configuration:**
- API Rate Limiter: `Audit Topic`; 429 events at ~8K events/sec × 500 bytes = ~4 MB/s; retained for 7 days in Kafka, then archived to S3 as Parquet for Athena queries; compressed 10:1 → ~14 GB/day
- Throttling Service: `Throttle Events Topic`; 1M throttled requests/sec × 300 bytes = ~300 MB/s; hot in Elasticsearch for last 10 minutes (~26 GB), cold in S3 for 90 days
- Both: Kafka producers use async sends with `acks=1` (leader acknowledgment only) — acceptable durability trade-off since audit logging is not SLA-critical

**What you lose without it:** Synchronous logging to a database from the hot path would add 5–20 ms of latency to every rejected request. At 8K rejections/second, that's a significant performance impact. Kafka's async model keeps the hot path clean.

✅ Decoupled from hot path, durable, high throughput, partitioned for scale; ❌ Additional infrastructure to operate, eventual delivery (events may be slightly delayed), audit gaps if Kafka is down (mitigated by access logs)

---

**Component 4: In-Process Caching for Configuration**

**Why used:** Rules and policies change rarely (< 1,000 updates/day for a large platform). Fetching them from Redis on every request would add a Redis round-trip just for rule lookup on top of the counter round-trip — doubling Redis load for no benefit. An in-process LRU cache loaded at startup and refreshed every 30 seconds serves rules with < 0.05 ms lookup latency.

**Key configuration:**
- API Rate Limiter: LRU cache per gateway node, 30-second refresh interval from Redis rules cache; entire rules set fits in memory (< 20 MB); hit rate near 100% in steady state
- Throttling Service: in-process token bucket state, circuit breaker state, and quota counters all held in process memory; control plane pushes updates via gRPC streams (no polling); new instance initializes from control plane at startup, conservative 50% of max_rps

**What you lose without it:** Two Redis round-trips per request (one for rules, one for counter) instead of one, effectively doubling Redis throughput requirements and adding 1–3 ms additional overhead per request. At 8M RPS, this would require 64 Redis shards instead of 32.

✅ Zero network overhead for rule lookups, 100% cache hit rate in steady state, eliminates Redis dependency for config; ❌ 30-second staleness window for rule changes, each node independently caches (adds memory pressure at scale)

---

**Component 5: Sliding Window Counter and Token Bucket Algorithms**

**Why used:** These are the two O(1)-memory algorithms that work at scale. Sliding window counter is the default for API rate limiting (< 1% error, no pathological boundary bursts). Token bucket is the default for service-to-service throttling (allows intentional bursts, rate is dynamically updatable by the adaptive algorithm).

**Key configuration:**
- Sliding Window Counter: two Redis keys per client per window size (current bucket + previous bucket); `estimated_count = floor(prev_count × weight) + current_count`; weight = `1 - elapsed_in_window / window_size`; both keys use hash tags to force same Redis shard; wrapped in Lua script for atomicity
- Token Bucket: `tokens = min(BurstSize, tokens + elapsed × Rate)`; consume 1 token per request; `Rate` is dynamically updated by control plane via `UpdateRate()` call; mutex-protected for thread safety; `MinRate` floor prevents AIMD from driving rate to zero

**What you lose without them:** Either you use fixed window counter (allows 2× boundary bursts, gameable) or sliding window log (100 GB RAM for 2M active clients) or leaky bucket (requires background drain process per client, variable response latency). None of these work at production scale.

✅ O(1) memory, proven at scale (Cloudflare, Stripe, Kong), fits in Redis with TTL; ❌ Sliding window counter is an approximation (not suitable for exact per-window billing), token bucket allows bursts (not suitable when strict per-window enforcement is required)

---

**Component 6: Circuit Breaker Pattern**

**Why used:** When Redis (in the API rate limiter case) or a downstream service (in the throttling service case) is degraded — high error rate or high latency — a circuit breaker stops the system from repeatedly hammering a failing dependency. It also implements the fail-open policy cleanly: when the circuit is open, requests skip the Redis call and are allowed through, rather than waiting for Redis timeouts on every request.

**Key configuration:**
- API Rate Limiter: circuit breaker wraps all Redis calls; CLOSED → OPEN at 5% error rate in 10-second window; HALF-OPEN after 30 seconds; 1% probe traffic to test recovery
- Throttling Service: one circuit breaker per (caller, callee) dependency pair; tunable thresholds by service type (payment service: 1% error, 200 ms latency; ML service: 10% error, 2000 ms latency); OPEN stops all new calls to that downstream; HALF-OPEN allows probe requests to test recovery

**What you lose without it:** Without a circuit breaker around Redis calls, a Redis timeout (default 1–5 seconds) on every request during Redis degradation causes all gateway threads to block, eventually exhausting the thread pool and cascading the failure to the entire API platform. The circuit breaker converts a "Redis is slow" problem into a clean "rate limiting temporarily disabled, fail-open" problem.

✅ Converts cascading failures into graceful degradation, fast fail without timeouts, automatic recovery probing; ❌ Requires careful threshold tuning per service type, potential for false positives (treating healthy downstream as overloaded)

---

**Component 7: Admin API with Hot-Reload**

**Why used:** Rate limit rules and throttle policies change frequently in a live platform — tier upgrades, emergency increases for a specific client, adding a new endpoint limit. Requiring a deployment to change a limit is operationally unacceptable. The admin API allows changes at runtime, and the rule propagation mechanism (Redis cache TTL + in-process LRU refresh) ensures changes take effect within 30 seconds.

**Key configuration:**
- API Rate Limiter: REST admin API (GET/POST/PUT/DELETE `/v1/ratelimit/rules`, POST `/v1/ratelimit/overrides`); writes to MySQL, invalidates Redis rules cache; in-process LRU on gateway nodes refreshes within 30 seconds
- Throttling Service: REST admin API with optimistic locking (`version` field on policies prevents concurrent update conflicts); emergency override endpoint (`POST /v1/throttle/override`) propagates via gRPC push within 5 seconds; circuit breaker reset endpoint for on-call SREs
- Both: admin API on internal network only (not exposed to public internet); requires `Authorization: Bearer <admin_token>`

**What you lose without it:** Manual rule changes require a service restart, causing a rolling deployment that takes minutes and disrupts ongoing rate limit state. For emergency situations (e.g., a client attacking the platform at 100× their limit), you need sub-minute response time.

✅ < 30 second rule propagation, no restart needed, enables emergency response; ❌ Propagation window means stale rules for up to 30 seconds during active updates (acceptable for most use cases)

---

**Component 8: Eventual Consistency Acceptance (< 100 ms Propagation Window)**

**Why used:** Strong consistency across all gateway nodes would require synchronous cross-node coordination — either a distributed lock (5–20 ms overhead) or a consensus protocol (Paxos/Raft, 10–50 ms). Both violate the 5 ms latency budget. Eventual consistency with a propagation window of < 100 ms allows each node to operate independently on its local state, with counters converging via Redis asynchronously.

**Key configuration:**
- API Rate Limiter: Redis replication lag < 1 ms within same AZ; counter updates propagate through Redis Cluster asynchronously; worst-case over-admission bounded by `num_nodes × replication_lag × request_rate_per_node`
- Throttling Service: threshold updates propagate from control plane to all instances within 5 seconds via gRPC push (not < 100 ms, but 5 seconds is acceptable for adaptive algorithm changes)
- Both: rule/policy changes propagate within 30 seconds via cache refresh

**What you lose without it:** Strong consistency requires distributed coordination per request. At 16M ops/sec, distributed lock acquire/release = 10–20 ms each, making rate limiting more expensive than the API call itself. The < 1% error budget from the sliding window counter algorithm directly budgets for the small over-admission that eventual consistency allows.

✅ No coordination overhead, horizontal scaling with no inter-node communication, fits within 5 ms latency budget; ❌ Brief window of over-admission possible (bounded and acceptable), rule changes not instantly global

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### API Rate Limiter — What Makes It Unique

The API Rate Limiter is the **external-facing traffic cop**. Its entire architecture is driven by three constraints that do not exist in the throttling service: (1) the limits are contractual (customers pay for tiers and expect them to be enforced fairly), (2) the behavior must be transparent (standard HTTP 429 headers so clients can self-regulate), and (3) the enforcement must be bypass-proof across a distributed fleet.

**Unique components:**
- **Multi-dimensional rule engine with tier-based priority ordering.** Rules are evaluated in priority order (lower number = higher priority); the most-specific applicable rule wins (api_key > user_id > ip > global). This requires an indexed MySQL table (`INDEX idx_priority`), a Redis rules cache keyed by tier and dimension, and an in-process LRU with dimension-based lookups. The throttling service has no equivalent — it just has one policy per (caller, callee) pair.
- **Standard HTTP 429 response headers.** `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`, `Retry-After`. These are a public contract with API consumers. Throttling service responses are internal errors (503/429) without the public rate limit header contract.
- **Client override table.** `client_overrides` allows granting a specific API key higher limits than their tier, exempting an IP range, or setting temporary elevated limits for a contract upgrade. No equivalent in the throttling service.
- **Per-second burst limit layered on per-minute window.** The combined Lua script checks both the burst key (1-second fixed window) and the minute-window sliding counter in a single atomic Redis operation. This prevents instantaneous floods even from clients who are under their per-minute limit.
- **Bypass prevention logic.** Extraction of real client IP from trusted CDN-only `X-Forwarded-For` headers. Rate limiting by user_id (not just api_key) to defeat key rotation. Account farming mitigation via IP-based registration limits.

**Unique decisions:**
- **MySQL for rules store** (not PostgreSQL) — the rule schema is well-structured and does not require JSONB; MySQL's simpler setup and tooling are preferred
- **Two Redis keys per client per window** (not a sorted set) — forces O(1) memory; the sliding window log with sorted sets would be 50 KB per client at limit, totaling 100 GB for 2M active clients
- **In-process LRU for rules** (not Redis lookup per request) — rules change less than once per hour on average; fetching from Redis per request doubles Redis load for no benefit

**Two-sentence differentiator:** The API Rate Limiter is a **distributed, tier-aware external rate enforcement system** where every request hits a shared Redis counter to maintain fairness guarantees across a fleet — the central Redis counter is a feature, not a limitation, because contractual fairness requires all nodes to see the same count. Its unique value is the multi-dimensional rule engine, standard HTTP 429 header contract, and bypass-proof identity resolution, none of which exist in the throttling service.

---

### Throttling Service — What Makes It Unique

The Throttling Service is the **internal service mesh traffic controller**. Its entire architecture is driven by three constraints that do not exist in the API rate limiter: (1) the hot path must have zero network hops (the latency budget of 0.5 ms leaves no room for Redis), (2) not all requests are equal (P0 payment requests must survive even when the system is overloaded), and (3) the limits themselves must adapt in real time to downstream health (static limits become wrong as soon as the downstream's capacity changes).

**Unique components:**
- **In-process AIMD Token Bucket.** The rate (tokens/second) is dynamically updated by the control plane. The token bucket itself is a mutex-protected in-process struct — `TryConsume()` and `UpdateRate()` both execute in < 0.1 ms with no network. No equivalent in the API rate limiter, where the counter is always in Redis.
- **Priority Classifier and Multi-Level Priority Queue (MLFQ).** Assigns P0–P3 based on endpoint, verified caller identity, and request metadata. P0 requests get absolute priority — they are always dequeued first and never shed. P3 (batch jobs) are shed first under overload. Starvation prevention ensures P3 never waits more than 120 seconds. The API rate limiter has no concept of request priority — all requests are treated identically.
- **Adaptive Threshold Engine (AIMD + EWMA).** The control plane's Adaptive Threshold Engine runs every 1 second per service pair, smooths error rate and p99 latency via EWMA (alpha=0.3), and computes a new `current_rps` via AIMD. This is pushed to all instances within 5 seconds via gRPC stream. No equivalent in the API rate limiter — all limits are static and human-configured.
- **Google's Client-Side Throttle as fallback.** Each instance tracks `requests / (requests + rejects)` over a sliding window. When the accept ratio drops, it pre-rejects requests locally before even sending them, reducing load on the downstream. This requires zero control plane coordination — it works even when the control plane is completely down. No equivalent in the API rate limiter.
- **Graceful Degradation.** When a request is shed, instead of returning 503, the handler checks for a cached response (in-process LRU), a default fallback value, or a partial response strategy configured in the policy. This converts shed → degraded response in many cases, making shedding transparent to the caller.
- **gRPC Push Gateway.** Maintains persistent gRPC streams to all 10,000 service instances for sub-5-second threshold propagation. The API rate limiter uses polling (30-second interval from Redis rules cache) — push is necessary for throttling because adaptive thresholds can change every second.

**Unique decisions:**
- **In-process SDK over Envoy sidecar** — sidecar adds 2–5 ms per request via loopback TCP; at 50M RPS, this is architecturally unacceptable; in-process eliminates the network entirely
- **PostgreSQL over MySQL for policy store** — the complex nested JSONB configs (circuit breaker config, priority shed thresholds, graceful degradation config) benefit from PostgreSQL's GIN indexes on JSONB fields; MySQL's JSON support is weaker
- **MLFQ over weighted fair queue (WFQ)** — WFQ would allow P0 to be slowed by P3 load; MLFQ gives P0 absolute priority, which is a hard business requirement (payment operations never shed)

**Two-sentence differentiator:** The Throttling Service is an **adaptive, in-process service mesh throttle** where zero network hops on the hot path enables 50M RPS with < 0.5 ms overhead — the absence of a central counter is a feature, not a limitation, because the control plane's adaptive algorithm makes each instance's locally-cached threshold correct enough. Its unique value is adaptive AIMD rate adjustment, 4-level priority queuing with starvation prevention, and graceful degradation, none of which exist in the API rate limiter.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2–4 sentence answers)

**Q: What is the difference between rate limiting and throttling?**
**KEY PHRASE: rate limiting is at the receiver, throttling is at the sender.** Rate limiting is enforced by the receiver (or a proxy in front of it) — it says "I will reject requests that exceed N/second, regardless of how many you send." Throttling is a signal sent to the sender saying "please slow down" — it relies on the sender to honor the signal voluntarily. In practice, throttling is the preferred mechanism because it works proactively before queues fill up; rate limiting is the fallback when the sender ignores the backpressure signal.

**Q: Why use Redis for rate limiting? Why not a SQL database?**
**KEY PHRASE: Redis Lua script = atomic read-check-increment in one round-trip.** A SQL database like PostgreSQL or MySQL has p99 latency of 5–15 ms for a read-modify-write operation — that alone exceeds the entire 5 ms rate limit overhead budget. Redis operates at < 1 ms p99 in the same datacenter. More importantly, Redis Lua scripts execute atomically on the server: the entire "read prev count, read current count, compute estimate, if under limit then increment" sequence is a single atomic operation with no race conditions and no distributed locks.

**Q: What is the sliding window counter algorithm and why is it better than a fixed window counter?**
**KEY PHRASE: weighted blend of current and previous window eliminates the 2× boundary burst.** A fixed window counter resets cleanly at each window boundary, allowing a client to send `limit` requests in the last second of window N and `limit` more in the first second of window N+1 — a 2× burst. The sliding window counter maintains two integer keys (current and previous window) and computes an estimate using `prev_count × (1 - elapsed_fraction) + current_count`. This weighted blend smooths out boundary effects with only O(1) memory (320 bytes per client), achieving < 1% error in practice. Cloudflare has validated this at billions of requests per day.

**Q: Why do you fail open when Redis is down? Isn't that dangerous?**
**KEY PHRASE: temporary over-admission is far better than total platform outage.** At 8M requests per second, a fail-closed policy during a Redis outage means 8M error responses per second — the entire API platform is down for every user. Fail-open means rate limiting is temporarily disabled for the duration of the outage, potentially admitting more traffic than the configured limits. The risk of fail-open is managed by: Redis Cluster auto-failover (MTTR typically < 30 seconds), in-process fallback counters (per-node soft limits), and immediate PagerDuty alerts. A 30-second window of degraded accuracy is an acceptable trade-off to keep the platform serving traffic.

**Q: What HTTP status code and headers does a rate limited response return?**
**KEY PHRASE: HTTP 429 with X-RateLimit-* headers and Retry-After.** The response code is 429 Too Many Requests. On every request (allowed or rejected), the gateway sets `X-RateLimit-Limit` (the configured limit), `X-RateLimit-Remaining` (how many requests remain in the current window), and `X-RateLimit-Reset` (Unix timestamp when the window resets). On 429 responses only, a `Retry-After` header is added with the number of seconds until the client can retry. The response body is a structured JSON error with the rule name and a documentation URL so developers can understand why they were rate limited.

**Q: How do you ensure rate limits are enforced consistently across 500 API gateway nodes?**
**KEY PHRASE: shared Redis counter is the single source of truth.** Each gateway node is stateless — it does not maintain its own per-client counters. Every rate limit check reads from and writes to the same Redis Cluster, using a client key hash to route to the correct shard. Two concurrent requests from the same client hitting two different gateway nodes will both hit the same Redis shard and execute their Lua scripts sequentially (Redis is single-threaded for command execution). This guarantees that all increments are counted, regardless of which gateway node serves the request.

**Q: What does graceful degradation mean in the context of a throttling service?**
**KEY PHRASE: return a degraded response instead of an error when shedding requests.** When the throttling service sheds a request (drops it due to overload), instead of returning a 503 Service Unavailable, the system can return a cached response from a previous successful request (for read APIs where staleness is acceptable), a hardcoded default value (e.g., `{"fraud_score": 0, "allow": true}` for fraud check when the service is down), or a partial response (first page of search results from cache). This makes shedding transparent to the caller — they get a slightly degraded answer instead of an error, which is often acceptable from a user experience perspective.

---

### Tier 2 — Deep Dive Questions (why + trade-offs)

**Q: Why use Lua scripts for Redis atomicity instead of MULTI/EXEC transactions?**
**KEY PHRASE: Lua executes on the server in one round-trip; MULTI/EXEC causes retry storms under contention.** MULTI/EXEC uses optimistic locking with WATCH — if another client modifies a watched key, the transaction aborts and the caller must retry. Under high contention (many concurrent requests from the same high-volume client hitting the same Redis shard), WATCH violations trigger retry storms that can cascade. Lua scripts execute atomically on the Redis server: all commands in the script run as a single indivisible operation with no opportunity for another client to interleave. There is no retry needed because there is no contention. The entire script — read prev count, read current count, compute estimate, check limit, increment if allowed — is a single network round-trip to Redis.

**Q: How do you prevent a sophisticated client from bypassing rate limits by rotating API keys?**
**KEY PHRASE: rate limit per user_id (not just api_key) to aggregate across all keys for one account.** If you only rate limit by `api_key`, a client with 10 API keys can get 10× their allowed rate. The fix is to enforce rate limits at both the api_key dimension AND the user_id dimension simultaneously. Since multiple API keys can exist for one user account, the user_id limit is the aggregate across all of that user's keys. The Lua script checks two counters: `rl:apikey:{key_hash}:60:{bucket}` and `rl:uid:{user_id}:60:{bucket}`. If either exceeds its limit, the request is rejected. ✅ Eliminates key rotation bypass; ❌ Requires that API keys are always linked to a user_id (unauthenticated callers are rate limited by IP only).

**Q: How does the AIMD adaptive throttle avoid oscillation — the sawtooth pattern where it repeatedly halves and then slowly ramps up?**
**KEY PHRASE: EWMA smoothing makes single-event false positives impossible; asymmetric increase/decrease rates prevent rapid cycling.** Two mechanisms work together. First, EWMA (Exponentially Weighted Moving Average) with alpha=0.3 means a single error spike has only 30% weight in the smoothed signal — multiple consecutive bad ticks are required to trigger a decrease. Second, AIMD's asymmetry (additive increase at +5% of max per healthy second; multiplicative decrease at 50% on overload) means decreases are fast (one tick to halve) but increases are slow (20 ticks to double from 50% to 100%). The system reacts quickly to overload but recovers slowly, giving the downstream time to stabilize before receiving full load again. A 30-second cooldown period after any decrease prevents premature ramp-up.

**Q: How do you handle the thundering herd problem when rate limit windows reset?**
**KEY PHRASE: sliding window counter's weighted blend makes window resets soft, not cliff-edge.** With a fixed window counter, when the window resets at timestamp T, every client who was at their limit suddenly has a count of zero — they all fire simultaneously, causing a burst that the downstream was not designed to handle. The sliding window counter has no cliff-edge reset: as you move through the current window, the previous window's contribution decays smoothly (weight decreases from 1.0 to 0.0 over the window duration). A client who saturated the previous window faces a progressively relaxing limit, not a sudden one. Additionally, the `X-RateLimit-Reset` timestamp in the response headers should be consumed by SDK clients with exponential backoff and jitter, which spreads retries across the reset window rather than synchronizing them.

**Q: Why does the throttling service use in-process state instead of a shared Redis counter like the API rate limiter?**
**KEY PHRASE: 50M in-fleet RPS × 0 network hops = feasible; 50M × 1 Redis round-trip = 85 Redis shards and 150 MB/s baseline Redis traffic.** At 50M in-fleet RPS, if each throttle check required a Redis round-trip, you would need approximately 85 Redis shards (50M / 600K ops/sec per shard). More importantly, even at < 1 ms Redis latency, 50M × 1 ms = 50,000 seconds of cumulative latency added per second across the fleet — that is a crippling overhead. In-process token bucket state has < 0.1 ms decision latency with zero network. The trade-off is that each instance tracks its own state independently — there is no global coordinated view of exactly how much rate has been consumed fleet-wide. The adaptive control plane compensates for this by computing per-service aggregate rates and pushing them to all instances, so all instances converge on the same rate.

**Q: What are the different circuit breaker threshold settings for different service types, and why do they differ?**
**KEY PHRASE: payment services need fast, sensitive trip; ML/inference services need slow, tolerant trip.** Critical payment services are configured with a tight threshold (1% error rate, 200 ms latency threshold, 5-second window, 60-second recovery). Even a small error rate on a payment service represents real money being lost or double-charged — you want to isolate it fast and wait for a full recovery before re-enabling. ML recommendation services, by contrast, have naturally high latency (2,000 ms threshold) and tolerate higher error rates (10%) because stale or slightly inaccurate recommendations are not business-critical. Setting the payment service thresholds on the ML service would cause false positive circuit trips on every slight latency spike, making the service unusable. The per-policy circuit breaker configuration (`circuit_breaker_config JSONB` in PostgreSQL) enables this differentiation.

**Q: How do Redis hash tags work and why are they necessary for the sliding window counter Lua script?**
**KEY PHRASE: without hash tags, current and previous window keys can land on different shards, breaking Lua atomicity.** Redis Cluster uses CRC16 of the full key to determine which hash slot (and therefore which shard) a key belongs to. The key `rl:uid:u123:60:28928160` (current window) and `rl:uid:u123:60:28928159` (previous window) have different CRC16 values — they would likely land on different shards. A Lua script cannot access keys on multiple shards in Redis Cluster; it will fail with a `CROSSSLOT` error. Hash tags instruct Redis to use only the portion of the key inside curly braces for hashing. With `rl:{u123}:60:28928160` and `rl:{u123}:60:28928159`, both keys hash the same `{u123}` portion → same CRC16 → same shard. The Lua script can now atomically read both keys and increment the current one within a single network round-trip.

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud)

**Q: Your system is processing 8M RPS and suddenly sees a 10× traffic spike to 80M RPS due to a viral event. Walk me through exactly what happens and how you prevent a complete outage.**

Reason aloud: "At 80M RPS, Redis operations go to 160M/second. My current 32-shard Redis Cluster maxes out at about 19M ops/sec — we are 8× over capacity. Redis starts dropping connections and returning errors. The circuit breaker on Redis calls opens within 10 seconds (5% error threshold). All gateway nodes switch to fail-open mode. Now all traffic is flowing through with no rate limiting — the actual upstream services may also be overwhelmed.

Three things happen simultaneously: (1) The fail-open circuit breaker + local in-process fallback counters activate, applying a per-node soft rate limit. With 100 gateway nodes each allowing their configured share, this provides a coarse rate limit but not the precise distributed one. (2) The alert fires immediately — the oncall SRE gets paged. (3) Auto-scaling triggers for both gateway nodes (CPU > 70%) and Redis nodes (via CloudWatch/Prometheus alerts).

The SRE's immediate actions: enable the shadow counter mode (gateway nodes batch Redis updates every 100 ms, reducing Redis ops by 100× from 160M to 1.6M/sec — easily handled). This introduces up to 100 ms of over-admission but keeps the system functioning accurately enough. Simultaneously, trigger Redis Cluster expansion via Kubernetes operator or CloudFormation — adding 32 more shards takes 5–10 minutes.

Medium-term: enable adaptive sampling mode (synchronize to Redis on only 10% of requests, multiply delta by 10 on sync). This further reduces Redis pressure at the cost of 10× reduced accuracy. Finally, activate coarser-grained limits (per-hour instead of per-minute) using simpler counters.

The system degrades gracefully through a hierarchy of fallback modes: precise distributed → precise with batching → approximate with sampling → coarse-grained limits → fail-open with per-node soft limits → complete bypass. Each tier has an alert threshold and expected duration."

---

**Q: Design the multi-region deployment of the API Rate Limiter. A client based in Europe who routes some requests to us-east-1 and some to eu-west-1 should be rate limited at their global quota, not 2× their quota. How do you do this without violating the 5 ms latency budget?**

Reason aloud: "The fundamental tension here is: cross-region Redis synchronization has 50–150 ms round-trip latency, which blows the 5 ms budget. Strong global consistency is architecturally incompatible with the latency requirement. So I have to think about this differently.

Option 1: Per-region limits. Set each region's limit to `global_limit / num_regions`. With 2 regions, a 1,000 req/min limit becomes 500 req/min per region. This works for typical clients who are geographically co-located. It fails for sophisticated distributed clients who intentionally route to multiple regions — they get 1,000 req/min per region = 2,000 global. We detect these via the audit log (client_id appearing in both regional 429 streams) and apply manual overrides.

Option 2: Anycast routing with region affinity. Use anycast DNS or GeoDNS to route each client's requests consistently to their nearest region. A European client's requests almost always land in eu-west-1. The global limit is enforced locally in eu-west-1. Only edge cases (mobile clients that travel internationally, or adversarial distributed clients) would split across regions. Combined with per-region limits (Option 1) as a safety net, this is the pragmatic architecture.

Option 3: Asynchronous cross-region counter synchronization (best-effort). Regions sync their per-client counters to each other asynchronously via a Kafka topic. The sync lag is 150–300 ms. This means a global client can momentarily over-admit by `global_rate × 300ms = ~5 requests`. For most use cases, 5 extra requests per window is acceptable over-admission. The advantage: no latency impact on the hot path, and the client eventually gets globally consistent enforcement.

The architecture I recommend: anycast routing (primary) + per-region limits set to global/N (secondary) + async cross-region sync for observability. Document the multi-region over-admission behavior transparently in the API contract."

---

**Q: The throttling service's adaptive algorithm is supposed to reduce load on an overloaded downstream. But what if the overloaded downstream is the single source of truth for an entire product feature — and reducing load means users get errors? How do you make a business decision in code?**

Reason aloud: "This is the classic tension between system stability and business continuity. Pure systems thinking says 'reduce load, protect the downstream.' Business thinking says 'users are getting errors for a revenue-critical feature, reduce load less aggressively.'

The resolution is that the adaptive algorithm must be aware of the business value of different request types, not just the technical health signal. This is exactly why the priority queue exists. A P0 request to the downstream — say, a payment status check — should receive different behavior than a P3 request — a background analytics cache refresh.

Three mechanisms encode the business decision: (1) `adaptive_min_rps` floor — the minimum rate is set higher for P0-sensitive service pairs. For the payment verification service pair, `adaptive_min_rps` might be 2,000 RPS (20% of max) rather than 100 RPS. This ensures payment queries always have meaningful throughput even when the downstream is severely degraded. (2) Priority-aware AIMD — the multiplicative decrease applies to P2/P3 traffic first; P0/P1 traffic rate is reduced only after P2/P3 is fully shed. This is implemented by having separate token buckets per priority class, with the adaptive algorithm applying different decrease factors by priority. (3) Graceful degradation — for endpoints where a cached or default response is acceptable, the policy's `graceful_degradation_config` returns a useful response instead of shedding, decoupling 'user sees an error' from 'downstream is under load.'

The philosophical point: the adaptive algorithm should protect system health while maximizing business value preserved. Configurable `adaptive_min_rps` per service-pair, combined with priority-aware shedding, is how you encode business priority into the rate control algorithm."

---

**Q: How would you extend this rate limiter to support cost-based rate limiting — where requests that consume more compute (like an LLM inference with 10,000 tokens) count for more against the rate limit than a cheap request (a 10-token prompt)?**

Reason aloud: "Standard rate limiting counts requests. Cost-based limiting counts 'work units' or 'credits.' The change touches three layers.

First, the counter: instead of `INCR counter by 1`, the Lua script takes a `cost` parameter and does `INCR counter by cost`. The cost for a request is determined before the rate limit check (either from the request metadata — token count estimate — or from a previous response — actual tokens consumed). The limit is now in cost units per window (e.g., 100,000 tokens/minute), not request count per window. The sliding window counter algorithm still works identically — only the increment amount changes.

Second, the rule schema: `limit_count` changes meaning to 'cost units per window.' The `rate_limit_rules` table needs a `cost_unit` column (e.g., `tokens`, `compute_seconds`) and the admin API documents what one cost unit means. The rule might now say: `limit_count: 100000, window_seconds: 60, algorithm: sliding_window_counter` where the limit is 100,000 tokens per minute.

Third, the header contract: `X-RateLimit-Remaining` now shows remaining cost units, not remaining requests. This is less intuitive for API clients — they need to know the cost of their next request before sending it. Some APIs address this by providing a `X-RateLimit-Estimated-Cost` on every response for the just-completed request.

The hard problem: what if the cost is not known until after the request completes (e.g., LLM streaming responses where you don't know the output token count until generation finishes)? In that case, you charge a pre-estimated cost at request start (based on input tokens + a conservative output estimate), then reconcile the actual cost post-response and issue a credit or debit. This requires a two-phase update: a provisional counter increment at request start, a reconciliation delta at request end. The Lua script handles the provisional increment; a separate post-request callback handles the reconciliation. The counter may temporarily over-count (during generation), which is acceptable — the reconciliation ensures final accuracy."

---

**Q: Your rate limiter is returning incorrect `X-RateLimit-Remaining` headers — clients are reporting they see '500 remaining' but then immediately get a 429. How would you debug this in production?**

Reason aloud: "This is a distributed consistency debugging problem. The symptom — high remaining count but immediate 429 — suggests either a concurrency issue, a stale counter read, or a race condition in the Lua script logic.

First, isolate the scope: is this happening for a specific client, a specific tier, a specific gateway node, or all clients? If one gateway node, it might have a stale in-process rules cache or a misconfigured Redis shard routing. If one client, it might be a counter key collision or a multi-key Lua script issue across shards (hash tag misconfiguration). If all clients, it's likely a system-wide issue (wrong Redis shard for a key cluster, or a clock skew problem).

Second, check for hash tag issues: run `CLUSTER KEYSLOT rl:{u123}:60:28928160` and `CLUSTER KEYSLOT rl:{u123}:60:28928159` on the Redis cluster. If they return different slot numbers, your hash tags are not being applied correctly. All counters for the same client must land on the same shard.

Third, reproduce in debug mode: add a temporary debug log that records `{client_id, prev_key, prev_count, current_key, current_count, weight, estimated_count, limit, decision, remaining}` for every rate limit check. Compare the logged `estimated_count` to what would have been the true count at decision time.

Fourth, check for clock skew: the sliding window counter uses `floor(now / window_seconds)` to compute `window_bucket`. If gateway node clocks are skewed, `now` may be in a different bucket than expected, causing incorrect prev/current key computation. Check NTP sync status on all gateway nodes.

Fifth, check for a multi-window race: if a client is simultaneously hitting both the per-second burst limit and the per-minute window limit, the Lua script returns a 429 with `-1` (burst rejected) even when `X-RateLimit-Remaining` is computed from the minute-window remaining. The per-second remaining is not reflected in the standard headers. This is a known UX gap in the header contract — `X-RateLimit-Remaining` reflects the minute window, but the request was actually blocked by the second window.

The debugging order: check hash tags → check clock skew → reproduce in debug mode → check per-second vs per-minute interaction. Most production occurrences of this bug trace to hash tag misconfiguration or clock skew."

---

## STEP 7 — MNEMONICS

**Mnemonic 1: The 5 Rate Limiting Algorithms — "Fixed Slides Through Leaky Tokens"**
- **F**ixed window counter — F is for Fast and simple, but allows 2× **F**lips at boundaries
- **Sl**iding window **Log** — **Sl**ow memory growth (O(N)), perfect accuracy
- **Sl**iding window **Counter** — **Sl**ow drift (< 1% error), **C**heap memory (O(1)), production default
- **T**oken bucket — **T**olerates bursts by design
- **L**eaky bucket — **L**evel output rate (smoothed, no bursts)

The decision tree: "Do you need exact accuracy for billing?" → Sliding Window Log (if scale allows) or strict Token Bucket. "Do you need to allow intentional bursts?" → Token Bucket. "Is this a high-scale API with millions of clients?" → **Sliding Window Counter** (default). "Is this output rate smoothing (email/SMS sending)?" → Leaky Bucket.

**Mnemonic 2: API Rate Limiter vs Throttling Service — "External Fixed Counter; Internal Adaptive In-Process"**
- **E**xternal = API Rate Limiter (external clients, HTTP 429, public headers)
- **F**ixed limits = API Rate Limiter (human-configured tiers, rule engine)
- **C**entral Redis counter = API Rate Limiter (shared state, distributed enforcement)
- **I**nternal = Throttling Service (microservices talking to each other)
- **A**daptive = Throttling Service (AIMD, changes every second based on health)
- **I**n-process = Throttling Service (zero network hops, token bucket in memory)

**Opening one-liner for the interview:**

"Rate limiting is really two different problems that look similar. The API rate limiter is a distributed external traffic cop — every request touches a shared Redis counter to ensure fairness across a fleet, and we accept a small Redis round-trip cost for guaranteed distributed enforcement. The throttling service is an internal adaptive regulator — all decisions happen in-process with zero network overhead, and a control plane distributes adaptive rate signals so every service instance self-regulates without coordination. The key insight is that the API limiter's central Redis counter is a feature (it enables contractual fairness), while the throttling service's lack of a central counter is a feature (it enables 50M RPS with sub-millisecond overhead)."

---

## STEP 8 — CRITIQUE

### What Is Well-Covered in the Source Material

The source files are genuinely excellent on the following:

- **Algorithm depth.** All five algorithms are covered with code, complexity analysis, and a comparison table. The Lua script for the combined sliding window + burst check is production-grade. The AIMD adaptive algorithm with EWMA smoothing is covered with real pseudocode and worked examples for oscillation prevention.

- **Capacity estimation.** Both problems have thorough, auditable estimates with intermediate steps. The architectural implication of each number (e.g., "16M ops/sec → 32 Redis shards") is explicitly drawn. This is exactly what an interviewer wants to see.

- **Failure mode coverage.** Both problems include detailed failure scenario tables (scenario → blast radius → detection → mitigation). The nuanced positions (fail-open reasoning, circuit breaker tuning by service type) are well-articulated.

- **Code implementation.** The Go and Python pseudocode is concrete enough to be credible without being overwhelming. Hash tag examples, the AIMD tick function, the MultiLevelQueue implementation — all of these signal deep practical experience.

- **Common component differentiation.** The notes explicitly contrast how each component is used differently in the two problems (e.g., Redis is on the hot path for API rate limiter, off-path for throttling service). This is a genuinely useful distinction that most study materials miss.

### What Is Shallow, Missing, or Could Be Wrong

**Gap 1: Multi-tenancy and tenant isolation.** The source mentions `tenant` as one of the dimensions but does not deeply address: how do you prevent a noisy enterprise tenant from consuming disproportionate Redis shard capacity? The answer involves key-level quotas on Redis memory per tenant, per-tenant rate limit on Redis write throughput, and tenant-level circuit breakers. This is likely to be probed at senior levels.

**Gap 2: The exact `Retry-After` header calculation.** The source mentions this header exists but does not specify how to calculate it precisely. For sliding window counter, the reset time is `(window_bucket + 1) × window_seconds` — but if a client is rate limited by the per-second burst key, the retry-after is just 1–2 seconds, not the minute window reset. The header should reflect the sooner of the two reset times.

**Gap 3: Eventual consistency numbers in multi-region.** The source says cross-region Redis synchronization introduces 50–150 ms lag. But it does not quantify the over-admission that results. A candidate should be able to say: "At 1,000 req/min limit and 150 ms sync lag, the maximum over-admission is `1,000/60 × 0.15 = 2.5 requests`. For most use cases, this is negligible."

**Gap 4: Token bucket refill precision.** The source notes that time precision matters in the Lua script for token bucket refill. What it doesn't say: Redis does not have sub-millisecond timestamp precision in Lua (`redis.call('TIME')` returns seconds + microseconds as two integers). If you compute refill from the elapsed time in Lua, you must handle the two-integer TIME response carefully or face floating-point issues at high refill rates.

**Gap 5: The "warm start" problem for new Redis shards during cluster expansion.** When adding new shards, the hash slot migration moves keys from old to new shards. During migration, the source says counter accuracy "degrades slightly for < 1 second." The real failure mode is more nuanced: MIGRATING/IMPORTING slot state causes MOVED redirects, which the client library handles transparently, but there is a brief window where a key's counter reads zero (before migration) on the new shard while the old shard has the correct value. In the worst case, this causes a brief window of allowing requests that should have been rejected.

### Senior Probes the Source Does Not Answer Well

**Probe: "How would you implement a soft limit that warns users when they reach 80% of their rate limit?"**
The source mentions this as a requirement but gives no implementation. The answer: check `estimated_count / limit >= 0.8` in the Lua script and add a `X-RateLimit-Warning: approaching-limit` header (or increment a `ratelimit.soft_limit.total` metric for alerting). No additional Redis operation needed.

**Probe: "How do you rate limit a streaming API (gRPC bidirectional stream, WebSocket, Server-Sent Events) where a single connection can last hours?"**
Not addressed at all. The answer: rate limit on the connection establishment (1 WebSocket connection per second), on the message rate within the connection (token bucket in the protocol handler, not the HTTP middleware), and on total bytes transferred per window (cost-based limiting). The sliding window counter per connection handles message rate; the API key limit handles connection establishment rate.

**Probe: "How would you implement a 'leaky token bucket' — a hybrid of leaky bucket and token bucket?"**
Not addressed but might come up. The answer: a leaky token bucket refills tokens at a constant rate but also has a minimum consumption time (floor of how fast requests can be processed). In practice, this is rarely needed — token bucket handles the real-world use case.

**Probe: "Your Redis Cluster has hot shards — some clients send so many requests that a single client's key consumes 30% of one shard's capacity. How do you fix this?"**
Not addressed. The answer: shard the hot client's key by appending a random suffix (`rl:{u123}:60:28928160:shard_0`, `...:shard_1`, etc.) and distribute across multiple Redis slots. On read, sum all shard counters. This is the "wide key" pattern. The trade-off is that you can no longer use a single Lua script for both reads — you need to fan out to multiple keys and aggregate.

### Common Traps Candidates Fall Into

**Trap 1: Proposing Cassandra for the rate limit counter store.** At first glance, Cassandra's distributed writes look attractive. But its p99 latency is 5–15 ms — the entire rate limit budget. The follow-up question is always "what about p99 latency?" and the candidate cannot recover. Always lead with Redis and its sub-millisecond p99.

**Trap 2: Designing only IP-based rate limiting.** IP-based limiting is a secondary check for unauthenticated traffic. For authenticated clients, IP-based limits are trivially bypassed (distributed client, VPN, NAT). The primary dimension must be user_id or api_key. An interviewer who asks "how do you prevent bypass?" will immediately expose this gap.

**Trap 3: Not distinguishing the two problems.** Candidates who treat "rate limiting" as a single problem and design a Redis counter without discussing adaptive throttling, priority queuing, or in-process architecture will appear shallow. Always open by asking whether this is external-facing (API rate limiter) or internal service-to-service (throttling service).

**Trap 4: Proposing a database (MySQL, Postgres, DynamoDB) for the counter store without acknowledging the latency issue.** Every major cloud database has a minimum p99 of 5–10 ms for write operations. That alone exceeds the budget. If asked "why not DynamoDB?", the answer is always "p99 latency of 5–10 ms minimum; our 5 ms total overhead budget for the entire rate limit check leaves nothing for DynamoDB's latency floor."

**Trap 5: Claiming strong consistency is achievable at scale without the latency cost.** Strong consistency at 16M ops/sec requires synchronous cross-node coordination. At 500 gateway nodes, a round-trip for consensus is 5–20 ms. Candidates who say "use Zookeeper for coordination" or "use etcd for strong consistency" without acknowledging the latency trade-off will be questioned aggressively.

**Trap 6: Missing the hash tag requirement for Redis Cluster Lua scripts.** Drawing a Redis Cluster architecture without mentioning hash tags signals that the candidate has never actually operated Redis Cluster with Lua scripts. This is a concrete implementation detail that separates "I've read about Redis" from "I've operated Redis."

---

*End of Guide — Pattern 13: Rate Limiting*
